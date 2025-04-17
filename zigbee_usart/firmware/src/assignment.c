#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "definitions.h"                      // systemInitialize()
#include "click_routines/usb_uart/usb_uart.h" // usb_uart_USART_Read/Write()
#include "utils.h"                            // delayMs(), getMsCount()

// ---------------------------------------------------------------------------
// Configurations
// ---------------------------------------------------------------------------
#define NODE_DELAY_MS 3000UL
#define ROUND_DURATION_MS 20000UL // head’s steady‐state window
#define CH_PROBABILITY 25
#define CH_AD_INTERVAL_MS 2000UL
#define CH_AD_TIMEOUT_MS (2 * CH_AD_INTERVAL_MS)
#define JOIN_WINDOW_MS 10000UL
#define STEADY_START_DELAY_MS 2000UL

#define MAX_MEMBERS 10
#define MAX_MSG_LEN 80
#define MAX_SENDS_PER_ROUND 2

static const char CMD_SLEEP_MODE[] = "ATS39=3\r";
static const char CMD_WAKE_MODE[] = "ATS39=0\r";

#define MSG_START '>'
#define MSG_END '<'

// Node states
typedef enum
{
    ROLE_IDLE,
    ROLE_CLUSTER_HEAD,
    ROLE_JOINING,
    ROLE_WAITING_STEADY_START,
    ROLE_MEMBER
} Role;

//------------------------------------------------------------------------------
// Global state
//------------------------------------------------------------------------------
static Role nodeRole = ROLE_IDLE;
static char NODE_ID[17] = {0};
static char headID[17] = {0};
static uint8_t mySlot = 0;
static uint8_t memberCount = 0;
static char memberList[MAX_MEMBERS][17];
static uint32_t lastCH = 0;
static uint32_t lastCHAD = 0;
static uint32_t steadyStart = 0;
static uint8_t sendsThisRound = 0;
static uint8_t steadyStateStarted = 0;
static uint8_t nextSlot = 1; // for dynamic assignment

// UART RX buffer
static char msgBuf[MAX_MSG_LEN];
static uint8_t msgIdx = 0;
static uint8_t inMessage = 0;

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------
static float getRandomFloat(void)
{
    return ((float)rand() + 1.0f) / ((float)RAND_MAX + 1.0f);
}

static int isHexString(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

//------------------------------------------------------------------------------
// Fetch our EUI64 by sending "ATI" over UART & parsing the 16‐hex reply
//------------------------------------------------------------------------------
static void fetchNodeID(void)
{
    usb_uart_USART_Write((uint8_t *)"ATI\r", 4);
    char line[16];
    int idx = 0;
    while (1)
    {
        uint8_t b;
        if (usb_uart_USART_Read(&b, 1) == 1)
        {
            if (b == '\r' || b == '\n')
            {
                if (idx == 16 && isHexString(line, 16))
                {
                    memcpy(NODE_ID, line, 16);
                    NODE_ID[16] = 0;
                    printf("[%s] NODE_ID fetched\r\n", NODE_ID);
                    return;
                }
                idx = 0;
            }
            else if (idx < 16)
            {
                line[idx++] = b;
            }
        }
    }
}

//------------------------------------------------------------------------------
// Sleep / wake commands
//------------------------------------------------------------------------------
static void enterSleep(void)
{
    usb_uart_USART_Write((uint8_t *)CMD_SLEEP_MODE, strlen(CMD_SLEEP_MODE));
    while (usb_uart_USART_WriteIsBusy())
        ;
}
static void exitSleep(void)
{
    usb_uart_USART_Write((uint8_t *)CMD_WAKE_MODE, strlen(CMD_WAKE_MODE));
    while (usb_uart_USART_WriteIsBusy())
        ;
}

//------------------------------------------------------------------------------
// Broadcast (or unicast) a framed "type,SRC=...,DST=...,DATA=..." packet
//------------------------------------------------------------------------------
static void broadcast(const char *type, const char *dst, const char *data)
{
    char pkt[MAX_MSG_LEN];
    int n = snprintf(pkt, sizeof(pkt),
                     "%c%s,SRC=%s,DST=%s,DATA=%s%c",
                     MSG_START, type, NODE_ID, dst, data, MSG_END);
    // show what we're sending
    printf("[%s] TX %-8s → %s : %s\r\n",
           NODE_ID, type, dst, data);

    // AT+RDATAB:<len>
    char at[16];
    int m = snprintf(at, sizeof(at), "AT+RDATAB:%02X\r", (uint8_t)n);
    usb_uart_USART_Write((uint8_t *)at, m);
    while (usb_uart_USART_WriteIsBusy())
        ;
    usb_uart_USART_Write((uint8_t *)pkt, n);
    while (usb_uart_USART_WriteIsBusy())
        ;
}

//------------------------------------------------------------------------------
// Handle one complete incoming packet in msgBuf[0..msgIdx-1]
//------------------------------------------------------------------------------
static void handleMessage(void)
{
     printf("in handleMsg: %s\r\n", msgBuf+1);
    char *p = msgBuf + 1;
    char *end = strchr(p, MSG_END);
    if (end)
        *end = 0;

    char *type = strtok(p, ",");
    if (!type)
        return;

    char *src = 0, *dst = 0, *data = 0;
    for (char *tok = strtok(NULL, ","); tok; tok = strtok(NULL, ","))
    {
        if (!strncmp(tok, "SRC=", 4))
            src = tok + 4;
        else if (!strncmp(tok, "DST=", 4))
            dst = tok + 4;
        else if (!strncmp(tok, "DATA=", 5))
        {
            data = tok + 5;
            break;
        }
    }
    if (!src || !dst || !data)
        return;
    if (strcmp(dst, NODE_ID) && strcmp(dst, "ALL"))
        return;

    // show what we got
    printf("[%s] RX %-8s ← %s : %s\r\n",
           NODE_ID, type, src, data);
    exitSleep();

    // any CH_AD resets our CH‐timeout
    if (!strcmp(type, "CH_AD"))
    {
        lastCHAD = getMsCount();
    }

    // JOIN handling
    if (!strcmp(type, "JOIN") && nodeRole == ROLE_CLUSTER_HEAD)
    {
        if (memberCount < MAX_MEMBERS)
        {
            // record member
            memcpy(memberList[memberCount], src, 17);
            // assign a slot dynamically
            char buf[4];
            int slot = nextSlot++;
            snprintf(buf, sizeof(buf), "%d", slot);
            printf("[%s] ASSIGN slot %d to %s\r\n", NODE_ID, slot, src);
            broadcast("ASSIGN", src, buf);
            memberCount++;
        }
    }
    // ASSIGN handling (member)
    else if (!strcmp(type, "ASSIGN") &&
             (nodeRole == ROLE_WAITING_STEADY_START || nodeRole == ROLE_MEMBER))
    {
        mySlot = atoi(data);
        printf("[%s] Got ASSIGN → mySlot=%u\r\n", NODE_ID, mySlot);
    }
    // CH_AD during JOIN
    else if (!strcmp(type, "CH_AD") && nodeRole == ROLE_JOINING)
    {
        memcpy(headID, src, 17);
        printf("[%s] Found head %s → sending JOIN\r\n", NODE_ID, headID);
        broadcast("JOIN", headID, NODE_ID);
        nodeRole = ROLE_WAITING_STEADY_START;
    }
    // STEADY_START with schedule
    else if (!strcmp(type, "STEADY_START") && nodeRole == ROLE_WAITING_STEADY_START)
    {
        nodeRole = ROLE_MEMBER;
        steadyStateStarted = 1;
        steadyStart = getMsCount();
        // we already got our ASSIGN earlier, so nothing else here
        printf("[%s] Steady‐state BEGIN\r\n", NODE_ID);
    }
    // DATA for CH to forward
    else if (!strcmp(type, "DATA") && nodeRole == ROLE_CLUSTER_HEAD)
    {
        for (int i = 0; i < memberCount; i++)
        {
            if (strcmp(memberList[i], src))
            {
                printf("[%s] FWD DATA → %s\r\n",
                       NODE_ID, memberList[i]);
                broadcast("FWD", memberList[i], data);
            }
        }
    }
    // ROUND_COMPLETE ends everything
    else if (!strcmp(type, "ROUND_COMPLETE"))
    {
        printf("[%s] ROUND_COMPLETE → IDLE\r\n", NODE_ID);
        nodeRole = ROLE_IDLE;
        steadyStateStarted = 0;
    }
}

//------------------------------------------------------------------------------
// Poll UART non‑blocking
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
                printf("msg: %s\r\n", msgBuf+1);
                handleMessage();
                inMessage = 0;
            }
        }
    }
}

//------------------------------------------------------------------------------
// Election: decide CH vs JOIN
//------------------------------------------------------------------------------
static void electRole(void)
{
    uint32_t now = getMsCount();
    if (lastCH && now - lastCH < ROUND_DURATION_MS)
    {
        nodeRole = ROLE_JOINING;
        return;
    }
    // if (getRandomFloat() < (CH_PROBABILITY / 100.0f))
    if (0 < (CH_PROBABILITY / 100.0f))
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        lastCH = now;
        printf("[%s] Elected CLUSTER_HEAD\r\n", NODE_ID);
    }
    else
    {
        nodeRole = ROLE_JOINING;
        printf("[%s] Will JOIN cluster\r\n", NODE_ID);
    }
}

//------------------------------------------------------------------------------
// Join‐phase: listen for CH_AD then send JOIN
//------------------------------------------------------------------------------
static void joinCluster(void)
{
    uint32_t start = getMsCount();
    lastCHAD = start;
    printf("[%s] JOIN phase: waiting up to %lums\r\n",
           NODE_ID, JOIN_WINDOW_MS);
    while (getMsCount() - start < JOIN_WINDOW_MS &&
           nodeRole == ROLE_JOINING)
    {
        processInput();
        // if CH disappears, abort
        if (getMsCount() - lastCHAD > CH_AD_TIMEOUT_MS)
        {
            printf("[%s] CH lost → IDLE\r\n", NODE_ID);
            nodeRole = ROLE_IDLE;
            return;
        }
    }
    if (nodeRole == ROLE_JOINING)
    {
        printf("[%s] JOIN timeout → IDLE\r\n", NODE_ID);
        nodeRole = ROLE_IDLE;
    }
}

//------------------------------------------------------------------------------
// Member: send DATA up to MAX_SENDS
//------------------------------------------------------------------------------
static void sendMemberData(void)
{
    if (!steadyStateStarted || !mySlot || sendsThisRound >= MAX_SENDS_PER_ROUND)
        return;

    // compute our interleaved send time
    uint32_t slotIndex = mySlot + sendsThisRound * memberCount;
    uint32_t target = steadyStart + slotIndex * NODE_DELAY_MS;

    // spin until it's our turn
    while (nodeRole == ROLE_MEMBER)
    {
        processInput();

        // CH must remain alive
        if (getMsCount() - lastCHAD > CH_AD_TIMEOUT_MS)
        {
            printf("[%s] CH timeout → IDLE\r\n", NODE_ID);
            nodeRole = ROLE_IDLE;
            return;
        }

        if (getMsCount() >= target)
        {
            exitSleep();
            if (nodeRole != ROLE_MEMBER)
                return;
            int v = (int)(getRandomFloat() * 100);
            char pl[24];
            snprintf(pl, sizeof(pl), "%s,%d", NODE_ID, v);
            printf("[%s] DATA slot%u → %d\r\n",
                   NODE_ID, mySlot, v);
            broadcast("DATA", headID, pl);
            sendsThisRound++;
            enterSleep();
            return;
        }
        delayMs(10);
    }
}

//------------------------------------------------------------------------------
// Cluster‐Head main
//------------------------------------------------------------------------------
static void runClusterHead(void)
{
    printf("[%s] Starting CH round, collecting JOINs\r\n", NODE_ID);

    // reset for this round
    memberCount = 0;
    nextSlot = 1;
    sendsThisRound = 0;
    steadyStateStarted = 0;

    // Election/Join window
    uint32_t joinEnd = getMsCount() + JOIN_WINDOW_MS;
    lastCHAD = getMsCount();
    while (getMsCount() < joinEnd && nodeRole == ROLE_CLUSTER_HEAD)
    {
        processInput();
        if (getMsCount() - lastCHAD >= CH_AD_INTERVAL_MS)
        {
            broadcast("CH_AD", "ALL", "election");
            lastCHAD = getMsCount();
        }
    }

    if (nodeRole != ROLE_CLUSTER_HEAD)
        return;

    if (memberCount == 0)
    {
        printf("[%s] No members → ROUND_COMPLETE\r\n", NODE_ID);
        broadcast("ROUND_COMPLETE", "ALL", "none");
        nodeRole = ROLE_IDLE;
        return;
    }

    // Build schedule string
    char sched[MAX_MSG_LEN] = "";
    int pos = 0;
    for (int i = 0; i < memberCount; i++)
    {
        pos += snprintf(sched + pos, sizeof(sched) - pos,
                        "%s:%d%s",
                        memberList[i], i + 1,
                        (i + 1 < memberCount) ? "," : "");
    }

    // Wait a bit, then launch steady‐state
    delayMs(STEADY_START_DELAY_MS);
    steadyStart = getMsCount();
    steadyStateStarted = 1;

    printf("[%s] Broadcast STEADY_START: %s\r\n", NODE_ID, sched);
    broadcast("STEADY_START", "ALL", sched);

    // Steady‐state window
    uint32_t steadyEnd = steadyStart + ROUND_DURATION_MS;
    uint32_t nextAd = steadyStart;
    while (nodeRole == ROLE_CLUSTER_HEAD && getMsCount() < steadyEnd)
    {
        processInput();
        if (getMsCount() >= nextAd)
        {
            broadcast("CH_AD", "ALL", "steady");
            nextAd += CH_AD_INTERVAL_MS;
        }
    }

    // End‐of‐round
    printf("[%s] Steady‐state over → ROUND_COMPLETE\r\n", NODE_ID);
    broadcast("ROUND_COMPLETE", "ALL", "end");
    nodeRole = ROLE_IDLE;
}

//------------------------------------------------------------------------------
// Member main
//------------------------------------------------------------------------------
static void runMember(void)
{
    while (nodeRole == ROLE_WAITING_STEADY_START ||
           nodeRole == ROLE_MEMBER)
    {
        processInput();
        if (nodeRole == ROLE_MEMBER)
            sendMemberData();
    }
}

//------------------------------------------------------------------------------
// Full LEACH round
//------------------------------------------------------------------------------
static void runRound(void)
{
    printf("\r\n[%s] ===== NEW ROUND =====\r\n", NODE_ID);
    electRole();

    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        runClusterHead();
    }
    else if (nodeRole == ROLE_JOINING)
    {
        joinCluster();
        if (nodeRole == ROLE_WAITING_STEADY_START ||
            nodeRole == ROLE_MEMBER)
        {
            runMember();
        }
    }

    nodeRole = ROLE_IDLE;
    delayMs(50);
}

//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
int leach_main(void)
{
    systemInitialize();
    delayMs(50);
    fetchNodeID();

    // seed RNG
    uint32_t seed = getMsCount();
    for (int i = 0; i < 16; i++)
        seed ^= ((uint32_t)NODE_ID[i]) << (i % 24);
    srand(seed);

    while (1)
    {
        runRound();
    }
    return 0;
}