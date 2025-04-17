// leach.c
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "definitions.h"                      // systemInitialize()
#include "click_routines/usb_uart/usb_uart.h" // usb_uart_USART_Read/Write()
#include "utils.h"                            // delayMs(), getMsCount(), etc.

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define NODE_DELAY_MS 3000UL      // TDMA slot length
#define ROUND_DURATION_MS 20000UL // steady‑state window
#define CH_PROBABILITY 25         // % chance to be CH
#define CH_AD_INTERVAL_MS 3000UL  // head advert interval in ms
#define CH_AD_TIMEOUT_MS 4000UL   // give up if no CH_AD
#define MIN_CH_INTERVAL_MS (ROUND_DURATION_MS)
#define JOIN_WINDOW_MS 3000UL // head’s “join” window

#define MAX_MEMBERS 10
#define MAX_MSG_LEN 80

// S39 AT commands for power‐down/up
#define CMD_SLEEP_MODE "ATS39=3\r"
#define CMD_WAKE_MODE "ATS39=0\r"

// framing chars
#define MSG_START '>'
#define MSG_END '<'

// node roles
typedef enum
{
    ROLE_IDLE,
    ROLE_CLUSTER_HEAD,
    ROLE_JOINING,
    ROLE_MEMBER
} Role;

//------------------------------------------------------------------------------
// State & globals
//------------------------------------------------------------------------------
static Role nodeRole = ROLE_IDLE;
static char NODE_ID[17] = {0}; // 16‐hex EUI64 + NUL
static char headID[17] = {0};
static uint8_t mySlot = 0;
static uint8_t memberCount = 0;
static char memberList[MAX_MEMBERS][17];
static uint32_t lastCH = 0;      // last time *we* were CH
static uint32_t lastCHAD = 0;    // last time *member* saw CH_AD
static uint32_t steadyStart = 0; // start of steady‐state
static uint32_t nextAdTime = 0;  // next CH_AD due

// RX parsing
static char msgBuf[MAX_MSG_LEN];
static uint8_t msgIdx = 0;
static uint8_t inMessage = 0;

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------
static float getRandomFloat(void)
{
    // Returns (0,1]
    return ((float)rand() + 1.0f) / ((float)RAND_MAX + 1.0f);
}

static int isHexString(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static void fetchNodeID(void)
{
    printf("Sending ATI to fetch EUI64...\r\n");
    usb_uart_USART_Write((uint8_t *)"ATI\r", 4);
    char line[32];
    int idx = 0;
    while (1)
    {
        uint8_t b;
        if (usb_uart_USART_Read(&b, 1) == 1)
        {
            if (b == '\r' || b == '\n')
            {
                if (idx > 0)
                {
                    line[idx] = '\0';
                    if (idx == 16 && isHexString(line, 16))
                    {
                        strncpy(NODE_ID, line, sizeof(NODE_ID) - 1);
                        printf("Fetched NODE_ID = %s\r\n", NODE_ID);
                        return;
                    }
                    idx = 0;
                }
            }
            else if (idx < (int)sizeof(line) - 1)
            {
                line[idx++] = b;
            }
        }
    }
}

static void initRandomSeed(void)
{
    uint32_t seed = getMsCount();
    for (size_t i = 0; i < strlen(NODE_ID); i++)
    {
        seed ^= ((uint32_t)NODE_ID[i]) << (i % 24);
    }
    srand(seed);
    printf("[%s] initRandomSeed(): seed=%lu\r\n", NODE_ID, seed);
}

// power‐down until UART activity
static void enterSleep(void)
{
    printf("[%s] is sleeping\r\n", NODE_ID);
    usb_uart_USART_Write((uint8_t *)CMD_SLEEP_MODE, strlen(CMD_SLEEP_MODE));
    while (usb_uart_USART_WriteIsBusy())
        ;
}
static void exitSleep(void)
{
    printf("[%s] is waking up\r\n", NODE_ID);
    usb_uart_USART_Write((uint8_t *)CMD_WAKE_MODE, strlen(CMD_WAKE_MODE));
    while (usb_uart_USART_WriteIsBusy())
        ;
}

//------------------------------------------------------------------------------
// Packet send
//------------------------------------------------------------------------------
static void broadcast(const char *type, const char *dst, const char *data)
{
    char pkt[MAX_MSG_LEN];
    int n = snprintf(pkt, sizeof(pkt),
                     "%c%s,SRC=%s,DST=%s,DATA=%s%c",
                     MSG_START, type, NODE_ID, dst, data, MSG_END);
    printf("[%s] TX %s -> %s : %s\r\n", NODE_ID, type, dst, data);
    char atcmd[20];
    int m = snprintf(atcmd, sizeof(atcmd), "AT+RDATAB:%02X\r", (uint8_t)n);
    usb_uart_USART_Write((uint8_t *)atcmd, m);
    while (usb_uart_USART_WriteIsBusy())
        ;
    usb_uart_USART_Write((uint8_t *)pkt, n);
    while (usb_uart_USART_WriteIsBusy())
        ;
}

//------------------------------------------------------------------------------
// Incoming packet handler
//------------------------------------------------------------------------------
static void handleMessage(const char *p)
{
    // 1) Mutable copy of the packet
    char buf[MAX_MSG_LEN];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // 2) First token is TYPE
    char *token = strtok(buf, ",");
    if (!token)
        return;
    char type[16];
    strncpy(type, token, sizeof(type) - 1);
    type[15] = '\0';

    // 3) Walk the rest of the tokens
    char *src = NULL, *dst = NULL, *data = NULL;
    while ((token = strtok(NULL, ",")) != NULL)
    {
        if (strncmp(token, "SRC=", 4) == 0)
        {
            src = token + 4;
        }
        else if (strncmp(token, "DST=", 4) == 0)
        {
            dst = token + 4;
        }
        else if (strncmp(token, "DATA=", 5) == 0)
        {
            // Everything after "DATA=" is your payload (even if it has commas)
            data = token + 5;

            // strip trailing '<' if it’s there
            size_t dlen = strlen(data);
            if (dlen > 0 && data[dlen - 1] == '<')
            {
                data[dlen - 1] = '\0';
            }
            break;
        }
    }

    // 4) Must have all three fields
    if (!src || !dst || !data)
        return;

    // 5) Copy into fixed‑size arrays
    char src_buf[17], dst_buf[17], data_buf[32];
    strncpy(src_buf, src, sizeof(src_buf) - 1);
    src_buf[16] = '\0';
    strncpy(dst_buf, dst, sizeof(dst_buf) - 1);
    dst_buf[16] = '\0';
    strncpy(data_buf, data, sizeof(data_buf) - 1);
    data_buf[31] = '\0';

    if (strcmp(dst, NODE_ID) != 0 && strcmp(dst, "ALL") != 0)
        return;

    printf("[%s] RX %s <- %s : %s\r\n",
           NODE_ID, type, src, data);
    exitSleep();

    if (strcmp(type, "CH_AD") == 0)
    {
        lastCHAD = getMsCount();
        if (nodeRole == ROLE_JOINING)
        {
            strcpy(headID, src);
            printf("[%s] Joining CH %s\r\n", NODE_ID, headID);
            broadcast("JOIN", headID, NODE_ID);
            nodeRole = ROLE_MEMBER;
        }
    }
    else if (strcmp(type, "JOIN") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
    {
        if (memberCount < MAX_MEMBERS)
        {
            strncpy(memberList[memberCount], src, 16);
            memberList[memberCount][16] = '\0';
            memberCount++;
            printf("[%s] CH recorded JOIN from %s (#%u)\r\n",
                   NODE_ID, src, memberCount);
        }
    }
    else if (strcmp(type, "SCHED") == 0 && nodeRole == ROLE_MEMBER)
    {
        printf("[%s] Got SCHED: %s\r\n", NODE_ID, data);
        char *q = data;
        while (q)
        {
            char id[17] = {0};
            int slot = 0;
            if (sscanf(q, "%16[^:]:%d", id, &slot) == 2 && strcmp(id, NODE_ID) == 0)
            {
                mySlot = slot;
            }
            q = strchr(q, ',');
            if (q)
                q++;
        }
        printf("[%s] mySlot=%u\r\n", NODE_ID, mySlot);
    }
    else if (strcmp(type, "DATA") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
    {
        printf("[%s] CH received DATA from %s -> %s\r\n",
               NODE_ID, src, data);
        for (int i = 0; i < memberCount; i++)
        {
            if (strcmp(memberList[i], src) != 0)
                broadcast("FWD", memberList[i], data);
        }
    }
    else if (strcmp(type, "FWD") == 0 && nodeRole == ROLE_MEMBER)
    {
        printf("[%s] Member got FWD: %s\r\n", NODE_ID, data);
    }
    else if (strcmp(type, "ROUND_COMPLETE") == 0)
    {
        printf("[%s] Received ROUND_COMPLETE\r\n", NODE_ID);
        nodeRole = ROLE_IDLE;
    }
}

//------------------------------------------------------------------------------
// UART poll (non‑blocking)
//------------------------------------------------------------------------------
static void processInput(void)
{
    uint8_t b;
    if (!usb_uart_USART_ReadIsBusy() &&
        usb_uart_USART_Read(&b, 1) == 1)
    {
        if (b == MSG_START)
        {
            inMessage = 1;
            msgIdx = 0;
            msgBuf[msgIdx++] = b;
        }
        else if (inMessage)
        {
            if (msgIdx < MAX_MSG_LEN - 1)
                msgBuf[msgIdx++] = b;
            if (b == MSG_END)
            {
                msgBuf[msgIdx] = '\0';
                handleMessage(msgBuf + 1);
                inMessage = 0;
            }
        }
    }
}

//------------------------------------------------------------------------------
// LEACH election
//------------------------------------------------------------------------------
static void electRole(void)
{
    uint32_t now = getMsCount();

    if (lastCH && (now - lastCH) < MIN_CH_INTERVAL_MS)
    {
        printf("[%s] Cooldown, will JOIN this round\r\n", NODE_ID);
        nodeRole = ROLE_JOINING;
        return;
    }

    float threshold = CH_PROBABILITY / 100.0f;
    // float rnd = getRandomFloat();
    float rnd = 1;
    printf("[%s] electing Role rnd=%.3f thresh=%.3f\r\n",
           NODE_ID, rnd, threshold);

    if (rnd < threshold)
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        lastCH = now;
        printf("[%s] Elected CLUSTER_HEAD\r\n", NODE_ID);
        broadcast("CH_AD", "ALL", "round");
    }
    else
    {
        nodeRole = ROLE_JOINING;
        printf("[%s] Will JOIN a cluster\r\n", NODE_ID);
    }
}

//------------------------------------------------------------------------------
// JOIN phase for members
//------------------------------------------------------------------------------
static void joinCluster(void)
{
    uint32_t start = lastCHAD = getMsCount();
    printf("[%s] joining Cluster: waiting up to %lums for CH_AD\r\n",
           NODE_ID, ROUND_DURATION_MS);

    while ((getMsCount() - start) < ROUND_DURATION_MS && nodeRole == ROLE_JOINING)
    {
        processInput();
    }
    if (nodeRole == ROLE_JOINING)
    {
        printf("[%s] joinCluster(): timed out → back to IDLE\r\n",
               NODE_ID);
        nodeRole = ROLE_IDLE;
    }
}

//------------------------------------------------------------------------------
// MEMBER sends in its slot, then sleeps
//------------------------------------------------------------------------------
static void sendMemberData(void)
{
    if (!mySlot)
        return;

    uint32_t target = steadyStart + mySlot * NODE_DELAY_MS;
    printf("[%s] waiting for slot %u @ +%lums\r\n",
           NODE_ID, mySlot, mySlot * NODE_DELAY_MS);

    while (getMsCount() < target && nodeRole == ROLE_MEMBER)
    {
        processInput();
        if ((getMsCount() - lastCHAD) > CH_AD_TIMEOUT_MS)
        {
            printf("[%s] Lost CH_AD → abort round\r\n", NODE_ID);
            nodeRole = ROLE_IDLE;
            return;
        }
    }

    // exitSleep();
    if (nodeRole != ROLE_MEMBER)
        return;

    int v;
    v = (int)(getRandomFloat() * 100);
    char pl[32];
    snprintf(pl, sizeof(pl), "%s,%d", NODE_ID, v);
    printf("[%s] Sending DATA slot#%u: %d\r\n", NODE_ID, mySlot, v);
    broadcast("DATA", headID, pl);
}

//------------------------------------------------------------------------------
// run one LEACH round
//------------------------------------------------------------------------------
static void runRound(void)
{
    printf("\r\n[%s] ======== NEW ROUND ========\r\n", NODE_ID);
    memberCount = mySlot = 0;
    headID[0] = '\0';

    // 1) elect or join
    electRole();

    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        printf("[%s] CH window: %lums to collect JOINs\r\n",
               NODE_ID, JOIN_WINDOW_MS);

        uint32_t jw = getMsCount();
        while ((getMsCount() - jw) < JOIN_WINDOW_MS)
        {
            processInput();
            // re‑advertise in that window
            uint32_t dt = (getMsCount() - jw) % CH_AD_INTERVAL_MS;
            if (dt < 20)
            {
                printf("[%s] (join window) CH_AD→\r\n", NODE_ID);
                broadcast("CH_AD", "ALL", "round");
            }
            delayMs(10);
        }
        printf("[%s] Join closed, %u members\r\n",
               NODE_ID, memberCount);

        // schedule
        char sched[MAX_MSG_LEN] = "";
        for (int i = 0; i < memberCount; i++)
        {
            char piece[20];
            snprintf(piece, sizeof(piece), "%s:%d",
                     memberList[i], i + 1);
            if (i)
                strcat(sched, ",");
            strcat(sched, piece);
        }
        printf("[%s] Broadcasting SCHED: %s\r\n", NODE_ID, sched);
        broadcast("SCHED", "ALL", sched);
    }
    else if (nodeRole == ROLE_JOINING)
    {
        printf("[%s] ROLE_JOINING—waiting for CH_AD…\r\n", NODE_ID);
        joinCluster();
        if (nodeRole == ROLE_MEMBER)
            printf("[%s] Joined CH %s\r\n", NODE_ID, headID);
    }

    // 2) steady‐state window
    steadyStart = getMsCount();
    nextAdTime = steadyStart;
    printf("[%s] Entering steady‐state for %lums\r\n",
           NODE_ID, ROUND_DURATION_MS);

    while ((getMsCount() - steadyStart) < ROUND_DURATION_MS && nodeRole != ROLE_IDLE)
    {
        processInput();

        if (nodeRole == ROLE_CLUSTER_HEAD)
        {
            if (getMsCount() >= nextAdTime)
            {
                printf("[%s] periodic CH_AD→\r\n", NODE_ID);
                broadcast("CH_AD", "ALL", "round");
                nextAdTime += CH_AD_INTERVAL_MS;
            }
        }
        else if (nodeRole == ROLE_MEMBER)
        {
            sendMemberData();
            // enterSleep();
        }
    }

    // 3) end‐of‐round
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        printf("[%s] ROUND_COMPLETE→ broadcast\r\n", NODE_ID);
        broadcast("ROUND_COMPLETE", "ALL", "end");
    }

    nodeRole = ROLE_IDLE;
    printf("[%s] Round done, back to IDLE\r\n", NODE_ID);
    delayMs(1000);
}

//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
int leach_main(void)
{
    systemInitialize();
    delayMs(1000);

    fetchNodeID();
    initRandomSeed();

    while (1)
    {
        runRound();
    }
    return 0;
}