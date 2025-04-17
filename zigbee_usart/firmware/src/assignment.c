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
#define ROUND_DURATION_MS 40000UL
#define CH_PROBABILITY 25          // % chance to be CH
#define CH_AD_INTERVAL_MS (3000UL) // head advert interval
#define CH_AD_TIMEOUT_MS (4 * CH_AD_INTERVAL_MS)
#define JOIN_WINDOW_MS 10000UL
#define MAX_MEMBERS 10
#define MAX_MSG_LEN 80
#define MAX_SENDS_PER_ROUND 10

// <<< Set this per‐node to simulate different send intervals >>>
static const uint32_t SEND_INTERVAL_MS = 3000UL; // e.g. Node A:3000, Node B:5000

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
static uint32_t lastCHAD = 0;    // last time we saw CH_AD
static uint32_t steadyStart = 0; // start of steady‐state window
static uint8_t sendsThisRound = 0;

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

static void broadcast(const char *type,
                      const char *dst,
                      const char *data)
{
    char pkt[MAX_MSG_LEN];
    int n = snprintf(pkt, sizeof(pkt),
                     "%c%s,SRC=%s,DST=%s,DATA=%s%c",
                     MSG_START, type, NODE_ID, dst, data, MSG_END);
    printf("[%s] TX %-8s → %s : %s\r\n",
           NODE_ID, type, dst, data);

    char at[16];
    int m = snprintf(at, sizeof(at), "AT+RDATAB:%02X\r", (uint8_t)n);
    usb_uart_USART_Write((uint8_t *)at, m);
    while (usb_uart_USART_WriteIsBusy())
        ;
    usb_uart_USART_Write((uint8_t *)pkt, n);
    while (usb_uart_USART_WriteIsBusy())
        ;
}

static void handleMessage(void)
{
    char *p = msgBuf + 1;
    char *end = strchr(p, MSG_END);
    if (end)
        *end = 0;

    char *type = strtok(p, ","), *src = 0, *dst = 0, *data = 0;
    if (!type)
        return;

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

    if (strcmp(type, "FWD"))
    {
        printf("[%s] RX %-8s ← %s : %s\r\n",
               NODE_ID, type, src, data);
    }

    exitSleep();

    // any CH_AD extends timeout
    if (!strcmp(type, "CH_AD"))
    {
        lastCHAD = getMsCount();
    }

    if (!strcmp(type, "CH_AD") && nodeRole == ROLE_JOINING)
    {
        // we heard CH, join it
        memcpy(headID, src, 17);
        broadcast("JOIN", headID, NODE_ID);
        nodeRole = ROLE_MEMBER;
        steadyStart = getMsCount();
        sendsThisRound = 0;
    }
    else if (!strcmp(type, "JOIN") &&
             nodeRole == ROLE_CLUSTER_HEAD)
    {
        if (memberCount < MAX_MEMBERS)
            memcpy(memberList[memberCount++], src, 17);
    }
    else if (!strcmp(type, "DATA") &&
             nodeRole == ROLE_CLUSTER_HEAD)
    {
        // build a “except” fwd message:
        char fwdData[MAX_MSG_LEN];
        // prefix: EXCEPT=<srcEUI64>, then the original payload
        snprintf(fwdData, sizeof(fwdData),
                 "EXCEPT=%s:%s",
                 src, data);

        broadcast("FWD", "ALL", fwdData);
    }
    else if (!strcmp(type, "FWD") && nodeRole == ROLE_MEMBER)
    {
        // data looks like "EXCEPT=<origID>:<payload…>"
        if (strncmp(data, "EXCEPT=", 7) == 0)
        {
            char *colon = strchr(data, ':');
            if (!colon)
                return;
            *colon = '\0';
            char *exceptID = data + 7; // the EUI64 to skip
            char *payload = colon + 1; // actual forwarded data

            // only print/process if it's *not* me
            if (strcmp(exceptID, NODE_ID) != 0)
            {
                printf("[%s] RX %-8s ← %s : %s\r\n",
                       NODE_ID, "FWD", exceptID, payload);
            }
            // else: it was me, so silently drop
        }
    }
    else if (!strcmp(type, "ROUND_COMPLETE"))
    {
        nodeRole = ROLE_IDLE;
    }
}

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
    }
}

static void runClusterHead(void)
{
    printf("[%s] I am CH\r\n", NODE_ID);
    memberCount = 0;
    lastCHAD = getMsCount();

    uint32_t phaseEnd = getMsCount() + JOIN_WINDOW_MS;
    while (getMsCount() < phaseEnd && nodeRole == ROLE_CLUSTER_HEAD)
    {
        processInput();
        if (getMsCount() - lastCHAD >= CH_AD_INTERVAL_MS)
        {
            broadcast("CH_AD", "ALL", "HI!!!!");
            lastCHAD = getMsCount();
        }
    }
    if (memberCount == 0)
    {
        broadcast("ROUND_COMPLETE", "ALL", "no_members");
        nodeRole = ROLE_IDLE;
        return;
    }

    // start steady-state
    steadyStart = getMsCount();

    uint32_t nextAd = steadyStart;
    while (nodeRole == ROLE_CLUSTER_HEAD &&
           getMsCount() - steadyStart < ROUND_DURATION_MS)
    {
        processInput();
        if (getMsCount() >= nextAd)
        {
            broadcast("CH_AD", "ALL", "HI!!!!");
            nextAd += CH_AD_INTERVAL_MS;
        }
    }
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        broadcast("ROUND_COMPLETE", "ALL", "end");
        nodeRole = ROLE_IDLE;
    }
}

static void runMember(void)
{
    uint32_t roundEnd = steadyStart + ROUND_DURATION_MS;
    sendsThisRound = 0;
    uint32_t nextSend = steadyStart + SEND_INTERVAL_MS;

    while (nodeRole == ROLE_MEMBER &&
           getMsCount() < roundEnd)
    {
        processInput();

        // if we lose CH, bail out
        if (getMsCount() - lastCHAD > CH_AD_TIMEOUT_MS)
        {
            printf("[%s] Lost CH_AD → idle\r\n", NODE_ID);
            nodeRole = ROLE_IDLE;
            return;
        }

        if (sendsThisRound < MAX_SENDS_PER_ROUND &&
            getMsCount() >= nextSend)
        {
            exitSleep();
            if (nodeRole != ROLE_MEMBER)
                return;

            int v = (int)(getRandomFloat() * 100);
            char pl[24];
            snprintf(pl, sizeof(pl), "%d", v);
            printf("[%s] DATA #%u → %d\r\n",
                   NODE_ID, sendsThisRound + 1, v);
            broadcast("DATA", headID, pl);

            sendsThisRound++;
            nextSend += SEND_INTERVAL_MS;
            enterSleep();
        }
    }

    // round ended without explicit COMPLETE
    nodeRole = ROLE_IDLE;
}

static void runRound(void)
{
    printf("\r\n[%s] === NEW ROUND ===\r\n", NODE_ID);
    nodeRole = ROLE_IDLE;
    sendsThisRound = 0;

    electRole();
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        runClusterHead();
    }
    else if (nodeRole == ROLE_JOINING)
    {
        // wait for CH_AD + JOIN
        uint32_t start = getMsCount();
        lastCHAD = start;
        while (nodeRole == ROLE_JOINING &&
               getMsCount() - start < JOIN_WINDOW_MS)
        {
            processInput();
            if (getMsCount() - lastCHAD > CH_AD_TIMEOUT_MS)
            {
                printf("[%s] JOIN timeout → idle\r\n", NODE_ID);
                nodeRole = ROLE_IDLE;
            }
        }
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

    // seed RNG
    uint32_t seed = getMsCount();
    for (int i = 0; i < 16; i++)
        seed ^= ((uint32_t)NODE_ID[i]) << (i % 24);
    srand(seed);

    while (1)
        runRound();
    return 0;
}