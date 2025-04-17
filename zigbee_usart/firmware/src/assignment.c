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
#define NODE_DELAY_MS 3000UL      // base slot length
#define ROUND_DURATION_MS 60000UL // steady‐state window
#define CH_PROBABILITY 25         // % chance to be CH
#define CH_AD_INTERVAL_MS 2000UL  // CH_AD broadcast interval
#define JOIN_WINDOW_MS 8000UL     // time to collect JOINs
#define CH_AD_TIMEOUT_MS (ROUND_DURATION_MS / 2)

#define MAX_MEMBERS 10
#define MAX_MSG_LEN 80
#define MAX_SENDS_PER_ROUND 2

// *** Set your static slot here (1-based) ***
#define STATIC_SLOT 2

static const char CMD_SLEEP_MODE[] = "ATS39=3\r";
static const char CMD_WAKE_MODE[] = "ATS39=0\r";

#define MSG_START '>'
#define MSG_END '<'

typedef enum
{
    ROLE_IDLE,
    ROLE_CLUSTER_HEAD,
    ROLE_JOINING,
    ROLE_MEMBER
} Role;

//------------------------------------------------------------------------------
// Global state
//------------------------------------------------------------------------------
static Role nodeRole = ROLE_IDLE;
static char NODE_ID[17] = {0};
static char headID[17] = {0};
static uint8_t memberCount = 0;
static char memberList[MAX_MEMBERS][17];
static uint32_t lastCHAD = 0;    // last CH_AD seen
static uint32_t steadyStart = 0; // start of steady‐state
static uint8_t sendsThisRound = 0;

// UART framing
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
// Fetch NODE_ID via “ATI”
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
                    NODE_ID[16] = '\0';
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
// Broadcast a framed packet
//------------------------------------------------------------------------------
static void broadcast(const char *type, const char *dst, const char *data)
{
    char pkt[MAX_MSG_LEN];
    int n = snprintf(pkt, sizeof(pkt),
                     "%c%s,SRC=%s,DST=%s,DATA=%s%c",
                     MSG_START, type, NODE_ID, dst, data, MSG_END);
    printf("[%s] TX %-8s → %s : %s\r\n", NODE_ID, type, dst, data);

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
// Handle one complete received packet in msgBuf
//------------------------------------------------------------------------------
static void handleMessage(void)
{
    char *p = msgBuf + 1;
    char *end = strchr(p, MSG_END);
    if (end)
        *end = '\0';

    char *type = strtok(p, ","), *src = NULL, *dst = NULL, *data = NULL;
    if (!type)
        return;
    for (char *tok = strtok(NULL, ","), *next; tok; tok = next)
    {
        next = strtok(NULL, ",");
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

    printf("[%s] RX %-8s ← %s : %s\r\n", NODE_ID, type, src, data);
    exitSleep();

    // any CH_AD refresh
    if (!strcmp(type, "CH_AD"))
    {
        lastCHAD = getMsCount();
    }

    // MEMBER: on first CH_AD, join
    if (!strcmp(type, "CH_AD") && nodeRole == ROLE_JOINING)
    {
        strncpy(headID, src, 16);
        headID[16] = '\0';
        printf("[%s] Found CH %s → sending JOIN\r\n", NODE_ID, headID);
        broadcast("JOIN", headID, NODE_ID);
        nodeRole = ROLE_MEMBER;
        steadyStart = getMsCount();
        printf("[%s] Using slot=%u\r\n", NODE_ID, STATIC_SLOT);
    }
    // CH: record JOINers (no slot assignment)
    else if (!strcmp(type, "JOIN") && nodeRole == ROLE_CLUSTER_HEAD)
    {
        if (memberCount < MAX_MEMBERS)
        {
            strncpy(memberList[memberCount], src, 16);
            memberList[memberCount][16] = '\0';
            memberCount++;
            printf("[%s] CH added member %s (#%u)\r\n",
                   NODE_ID, src, memberCount);
        }
    }
    // CH: forward DATA
    else if (!strcmp(type, "DATA") && nodeRole == ROLE_CLUSTER_HEAD)
    {
        // build a “except” fwd message:
        char fwdData[MAX_MSG_LEN];
        // prefix: EXCEPT=<srcEUI64>, then the original payload
        snprintf(fwdData, sizeof(fwdData),
                 "EXCEPT=%s,%s",
                 src, data);

        // log what we’re doing, including who we’re excluding
        printf("[%s] FWD DATA → ALL except %s : %s\r\n",
               NODE_ID, src, data);

        broadcast("FWD", "ALL", fwdData);
    }
    else if (!strcmp(type, "FWD") && nodeRole == ROLE_MEMBER)
    {
        // data looks like "EXCEPT=<origID>,<payload…>"
        if (strncmp(data, "EXCEPT=", 7) == 0)
        {
            char *comma = strchr(data, ',');
            if (!comma)
                return;
            *comma = '\0';
            char *exceptID = data + 7; // the EUI64 to skip
            char *payload = comma + 1; // actual forwarded data

            // only print/process if it's *not* me
            if (strcmp(exceptID, NODE_ID) != 0)
            {
                printf("[%s] RX %-8s ← %s : %s\r\n",
                       NODE_ID, "FWD", exceptID, payload);
            }
            // else: it was me, so silently drop
        }
    }
    // any: end‐of‐round
    else if (!strcmp(type, "ROUND_COMPLETE"))
    {
        nodeRole = ROLE_IDLE;
    }
}

//------------------------------------------------------------------------------
// Non‑blocking UART poll
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
                msgBuf[msgIdx] = 0;
                handleMessage();
                inMessage = 0;
            }
        }
    }
}

//------------------------------------------------------------------------------
// Randomized CH election
//------------------------------------------------------------------------------
static void electRole(void)
{
    // if (lastCH && now - lastCH < ROUND_DURATION_MS)
    // {
    //     nodeRole = ROLE_JOINING;
    //     return;
    // }

    if (1 < (CH_PROBABILITY / 100.0f))
    // if (getRandomFloat() < (CH_PROBABILITY / 100.0f))
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        // lastCH = now;
        lastCHAD = getMsCount();
        memberCount = 0;
        printf("[%s] Elected CLUSTER_HEAD\r\n", NODE_ID);
    }
    else
    {
        nodeRole = ROLE_JOINING;
        printf("[%s] Will JOIN cluster\r\n", NODE_ID);
    }
}

//------------------------------------------------------------------------------
// JOIN phase
//------------------------------------------------------------------------------
static void joinCluster(void)
{
    uint32_t start = getMsCount(), deadline = start + JOIN_WINDOW_MS;
    printf("[%s] JOIN phase: up to %lums\r\n",
           NODE_ID, (unsigned long)JOIN_WINDOW_MS);
    while (getMsCount() < deadline && nodeRole == ROLE_JOINING)
    {
        processInput();
        // give up if no CH_AD
        if (getMsCount() - lastCHAD > JOIN_WINDOW_MS)
        {
            printf("[%s] No CH_AD → idle\r\n", NODE_ID);
            nodeRole = ROLE_IDLE;
            return;
        }
    }
    if (nodeRole == ROLE_JOINING)
    {
        printf("[%s] JOIN timeout → idle\r\n", NODE_ID);
        nodeRole = ROLE_IDLE;
    }
}

//------------------------------------------------------------------------------
// Member steady‑state
//------------------------------------------------------------------------------
static void runMember(void)
{
    uint32_t roundEnd = steadyStart + ROUND_DURATION_MS;
    sendsThisRound = 0;

    while (nodeRole == ROLE_MEMBER && getMsCount() < roundEnd)
    {
        processInput();
        // lost CH?
        if (getMsCount() - lastCHAD > CH_AD_TIMEOUT_MS)
        {
            printf("[%s] Lost CH_AD → idle\r\n", NODE_ID);
            nodeRole = ROLE_IDLE;
            return;
        }
        // on our static slot
        if (STATIC_SLOT > 0 && sendsThisRound < MAX_SENDS_PER_ROUND)
        {
            // use the actual member count, not MAX_MEMBERS
            uint32_t slotIndex = STATIC_SLOT + sendsThisRound * memberCount;
            uint32_t target = steadyStart + slotIndex * NODE_DELAY_MS;

            if (getMsCount() >= target)
            {
                exitSleep();
                if (nodeRole != ROLE_MEMBER)
                    return;
                int v = (int)(getRandomFloat() * 100);
                char pl[24];
                snprintf(pl, sizeof(pl), "%s,%d", NODE_ID, v);
                printf("[%s] DATA slot%u → %d\r\n", NODE_ID, STATIC_SLOT, v);
                broadcast("DATA", headID, pl);
                sendsThisRound++;
                enterSleep();
            }
        }
        delayMs(10);
    }
}

//------------------------------------------------------------------------------
// Cluster‐Head round
//------------------------------------------------------------------------------
static void runClusterHead(void)
{
    uint32_t joinEnd = getMsCount() + JOIN_WINDOW_MS;

    // election / JOIN collection
    while (getMsCount() < joinEnd && nodeRole == ROLE_CLUSTER_HEAD)
    {
        processInput();
        if (getMsCount() - lastCHAD >= CH_AD_INTERVAL_MS)
        {
            broadcast("CH_AD", "ALL", "election");
            lastCHAD = getMsCount();
        }
    }

    if (memberCount == 0)
    {
        printf("[%s] No JOINs → end round\r\n", NODE_ID);
        broadcast("ROUND_COMPLETE", "ALL", "no_members");
        nodeRole = ROLE_IDLE;
        return;
    }

    // steady‐state
    steadyStart = getMsCount();
    printf("[%s] Steady‐state start (%u members)\r\n",
           NODE_ID, memberCount);

    uint32_t nextAd = steadyStart;
    uint32_t roundEnd = steadyStart + ROUND_DURATION_MS;

    while (nodeRole == ROLE_CLUSTER_HEAD && getMsCount() < roundEnd)
    {
        processInput();
        if (getMsCount() >= nextAd)
        {
            broadcast("CH_AD", "ALL", "steady");
            nextAd += CH_AD_INTERVAL_MS;
        }
    }

    // end‐round
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        broadcast("ROUND_COMPLETE", "ALL", "end");
        nodeRole = ROLE_IDLE;
    }
}

//------------------------------------------------------------------------------
// Main LEACH loop
//------------------------------------------------------------------------------
static void runRound(void)
{
    printf("\r\n[%s] ===== NEW ROUND =====\r\n", NODE_ID);
    memberCount = 0;
    sendsThisRound = 0;

    electRole();

    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        runClusterHead();
    }
    else if (nodeRole == ROLE_JOINING)
    {
        joinCluster();
        if (nodeRole == ROLE_MEMBER)
        {
            runMember();
        }
    }

    nodeRole = ROLE_IDLE;
    delayMs(50);
}

int leach_main(void)
{
    systemInitialize();
    delayMs(50);

    fetchNodeID();
    // seed rand
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