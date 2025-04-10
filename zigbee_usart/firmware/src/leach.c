#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "definitions.h"
#include "click_routines/usb_uart/usb_uart.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// Configuration Constants
// ---------------------------------------------------------------------------
#define NODE_DELAY_MS 5000UL      // Member data send interval
#define ROUND_DURATION_MS 40000UL // Round duration
#define CH_PROBABILITY 25         // Chance to become CH (0-100)
#define CH_AD_INTERVAL_MS 2000UL  // CH advertisement interval
#define MAX_MEMBERS 10            // Max cluster members
#define MAX_MSG_LEN 50            // Max message length
#define MSG_START '>'             // Message start delimiter
#define MSG_END '<'               // Message end delimiter

// Unique Node ID (change per node)
static const char *NODE_ID = "NODE_C";

// ---------------------------------------------------------------------------
// Data Structures
// ---------------------------------------------------------------------------
typedef struct
{
    char nodeID[10]; // Shortened from 16 to 10
    int sensorTotal;
    uint8_t sensorCount;
} MemberData;

typedef enum
{
    ROLE_IDLE,
    ROLE_CLUSTER_HEAD,
    ROLE_JOINING,
    ROLE_MEMBER
} LEACH_Role;

// ---------------------------------------------------------------------------
// Global Variables (minimized)
// ---------------------------------------------------------------------------
static LEACH_Role nodeRole = ROLE_IDLE;
static uint32_t roundStartTime = 0;
static MemberData memberData[MAX_MEMBERS];
static uint8_t memberCount = 0;
static volatile uint8_t roundEnded = 0;
static uint32_t lastMemberSend = 0;
static uint32_t lastCHAdTime = 0;

// Message buffer (reduced from static to local in functions where possible)
static char msgBuffer[MAX_MSG_LEN];

// ---------------------------------------------------------------------------
// Random Number Utilities (added back)
// ---------------------------------------------------------------------------
static void initRandomSeed(void)
{
    uint32_t seed = getMsCount();
    for (size_t i = 0; i < strlen(NODE_ID); i++)
    {
        seed ^= ((uint32_t)NODE_ID[i]) << (i % 24);
    }
    srand(seed);
    printf("Initialized random seed: %lu\r\n", seed);
}

static float getRandomFloat(void)
{
    return ((float)rand() + 1.0f) / ((float)RAND_MAX + 1.0f);
}

// ---------------------------------------------------------------------------
// Optimized Utility Functions
// ---------------------------------------------------------------------------
static void broadcast(const char *type, const char *content)
{
    char msg[MAX_MSG_LEN];
    int len = snprintf(msg, sizeof(msg), "%c%s,%s%c", MSG_START, type, content, MSG_END);
    if (len <= 0 || len >= MAX_MSG_LEN)
        return;

    char cmd[20];
    snprintf(cmd, sizeof(cmd), "AT+RDATAB:%02X\r", len);

    // Send command
    usb_uart_USART_Write((uint8_t *)cmd, strlen(cmd));
    uint32_t start = getMsCount();
    while (usb_uart_USART_WriteIsBusy() && (getMsCount() - start < 100))
        ;

    // Send message
    usb_uart_USART_Write((uint8_t *)msg, len);
    start = getMsCount();
    while (usb_uart_USART_WriteIsBusy() && (getMsCount() - start < 100))
        ;
    printf("Broadcasted: %s\r\n", msg);
}

static void processInput(void)
{
    uint8_t byte;
    static uint8_t msgIndex = 0;
    static uint8_t inMessage = 0;

    while (!usb_uart_USART_ReadIsBusy() && usb_uart_USART_Read(&byte, 1) == 1)
    {
        if (byte == MSG_START)
        {
            inMessage = 1;
            msgIndex = 0;
            msgBuffer[msgIndex++] = byte;
        }
        else if (inMessage)
        {
            if (msgIndex < MAX_MSG_LEN - 1)
            {
                msgBuffer[msgIndex++] = byte;
            }
            if (byte == MSG_END)
            {
                msgBuffer[msgIndex] = '\0';
                inMessage = 0;
                msgIndex = 0;

                // Parse message
                char type[10], content[20];
                if (sscanf(msgBuffer + 1, "%9[^,],%19[^<]", type, content) == 2)
                {
                    if (strcmp(type, "CH_AD") == 0)
                    {
                        if (nodeRole == ROLE_JOINING)
                        {
                            broadcast("JOIN", NODE_ID);
                            nodeRole = ROLE_MEMBER;
                        }
                        lastCHAdTime = getMsCount();
                    }
                    else if (strcmp(type, "JOIN") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
                    {
                        // First check if this node already exists in our member list
                        int alreadyExists = 0;
                        for (uint8_t i = 0; i < memberCount; i++)
                        {
                            if (strcmp(memberData[i].nodeID, content) == 0)
                            {
                                alreadyExists = 1;
                                break;
                            }
                        }

                        if (!alreadyExists && memberCount < MAX_MEMBERS)
                        {
                            strncpy(memberData[memberCount].nodeID, content, sizeof(memberData[0].nodeID) - 1);
                            memberData[memberCount].nodeID[sizeof(memberData[0].nodeID) - 1] = '\0';
                            memberData[memberCount].sensorTotal = 0;
                            memberData[memberCount].sensorCount = 0;
                            memberCount++;
                            printf("CH recorded JOIN request from %s\r\n", content);
                        }
                        else if (alreadyExists)
                        {
                            printf("CH ignoring duplicate JOIN from %s\r\n", content);
                        }
                        else
                        {
                            printf("CH cannot accept JOIN from %s - member list full\r\n", content);
                        }
                    }
                    else if (strcmp(type, "DATA") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
                    {
                        char sender[10] = {0};
                        int sensorVal;

                        if (sscanf(content, "%9[^,],%d", sender, &sensorVal) == 2)
                        {
                            for (uint8_t i = 0; i < memberCount; i++)
                            {
                                if (strcmp(memberData[i].nodeID, sender) == 0)
                                {
                                    memberData[i].sensorTotal += sensorVal;
                                    memberData[i].sensorCount++;
                                    break;
                                }
                            }
                            printf("CH received DATA from %s: %d\r\n", sender, sensorVal);
                        }
                    }
                    else if (strcmp(type, "ROUND_COMPLETE") == 0 && (nodeRole == ROLE_MEMBER || nodeRole == ROLE_JOINING))
                    {
                        roundEnded = 1;
                        printf("Node %s received ROUND_COMPLETE. Ending round.\r\n", NODE_ID);
                    }
                }
            }
        }
    }
}

static void checkCHTimeout(void)
{
    if (nodeRole == ROLE_MEMBER &&
        (getMsCount() - lastCHAdTime > CH_AD_INTERVAL_MS * 2)) // Allow 2x interval before declaring CH dead
    {
        printf("Node %s: No CH advertisement for %lu ms, assuming CH is down\r\n",
               NODE_ID, getMsCount() - lastCHAdTime);
        roundEnded = 1; // Trigger round end
    }
}

// ---------------------------------------------------------------------------
// LEACH Protocol Functions
// ---------------------------------------------------------------------------
static void electRole(void)
{
    float rnd = getRandomFloat();
    if ((rnd * 100) < CH_PROBABILITY)
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        broadcast("CH_AD", NODE_ID);
        printf("Node %s elected as Cluster Head.\r\n", NODE_ID);
    }
    else
    {
        nodeRole = ROLE_JOINING;
        printf("Node %s will join a cluster.\r\n", NODE_ID);
    }
}

static void joinCluster(void)
{
    uint32_t start = getMsCount();
    while (getMsCount() - start < (CH_AD_INTERVAL_MS * 2) && !roundEnded)
    {
        processInput();
        if (nodeRole == ROLE_MEMBER)
            return;
    }
    printf("Node %s did not receive any CH advertisement this round.\r\n", NODE_ID);
}

static void sendMemberData(void)
{
    if (getMsCount() - lastMemberSend >= NODE_DELAY_MS)
    {
        int sensorValue = (int)(getRandomFloat() * 100);
        char dataContent[20];
        snprintf(dataContent, sizeof(dataContent), "%s,%d", NODE_ID, sensorValue);
        broadcast("DATA", dataContent);
        printf("Member %s sent sensor data: %d\r\n", NODE_ID, sensorValue);

        lastMemberSend = getMsCount();
    }
}

static void aggregateData(void)
{
    while (getMsCount() - roundStartTime < (ROUND_DURATION_MS - 1000UL) && !roundEnded)
    {
        processInput();
    }
    printf("\n--- Aggregated Data from Cluster Members ---\r\n");

    for (uint8_t i = 0; i < memberCount; i++)
    {
        if (memberData[i].sensorCount > 0)
        {
            int avg = memberData[i].sensorTotal / memberData[i].sensorCount;
            printf("%s: Avg Sensor Value = %d (Total = %d, Count = %d)\r\n",
                   memberData[i].nodeID, avg, memberData[i].sensorTotal, memberData[i].sensorCount);
        }
        else
        {
            printf("%s: No data received.\r\n", memberData[i].nodeID);
        }
    }
    printf("----------------------------------------------\r\n");
}

static void runRound(void)
{
    // Initialize round
    roundEnded = 0;
    roundStartTime = getMsCount();
    memberCount = 0;
    lastMemberSend = roundStartTime;
    lastCHAdTime = roundStartTime;

    electRole();

    if (nodeRole == ROLE_JOINING)
    {
        joinCluster();
        if (nodeRole == ROLE_JOINING)
        {
            return;
        }
    }

    // Steady-state phase
    uint32_t lastAdvert = 0;
    while (getMsCount() - roundStartTime < ROUND_DURATION_MS && !roundEnded)
    {
        processInput();

        if (nodeRole == ROLE_MEMBER)
        {
            checkCHTimeout();
            sendMemberData();
        }

        if (nodeRole == ROLE_CLUSTER_HEAD && (getMsCount() - lastAdvert >= CH_AD_INTERVAL_MS))
        {
            broadcast("CH_AD", NODE_ID);
            lastAdvert = getMsCount();
        }
    }

    // Round completion
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        broadcast("ROUND_COMPLETE", NODE_ID);
        printf("CH %s broadcasted ROUND_COMPLETE.\r\n", NODE_ID);
        aggregateData();
    }

    // Clear member data for next round
    memset(memberData, 0, sizeof(memberData));
    memberCount = 0;

    nodeRole = ROLE_IDLE;
    printf("Round complete.\r\n\n");
    delayMs(500);
}

// ---------------------------------------------------------------------------
// Main Function
// ---------------------------------------------------------------------------
int leach_main(void)
{
    systemInitialize();
    initRandomSeed(); // Initialize random number generator

    // Main loop
    while (1)
    {
        runRound();
    }

    return 0;
}