#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "definitions.h"                      // Provides systemInitialize(), etc.
#include "click_routines/usb_uart/usb_uart.h" // Provides usb_uart_USART_Read()/Write(), etc.
#include "utils.h"                            // Provides delayMs(), getMsCount(), getRandomFloat(), etc.

// ---------------------------------------------------------------------------
// Configuration Constants (modify these per node if required)
// ---------------------------------------------------------------------------
#define NODE_DELAY_MS 3000UL      // How long a member waits between sending data (ms)
#define ROUND_DURATION_MS 20000UL // Total duration of one LEACH round (ms)
#define CH_PROBABILITY 25         // Percent chance (0-100) to become Cluster Head in a round
#define CH_AD_INTERVAL_MS 2000UL  // Interval between CH advertisements (ms)
#define MAX_JOIN_MESSAGES 10      // Maximum number of join requests (and DATA messages) to store
#define MAX_MSG_LEN 50            // Maximum length for any message
#define MIN_CH_INTERVAL_MS 8000UL // Minimum time (ms) between consecutive CH roles for a node

// ---------------------------------------------------------------------------
// Message Delimiters
// ---------------------------------------------------------------------------
#define MSG_START '>'
#define MSG_END '<'

// ---------------------------------------------------------------------------
// Data Structures and Global Variables
// ---------------------------------------------------------------------------

// Updated MemberData structure: now accumulates sensor data from a member.
typedef struct
{
    char nodeID[16];
    int sensorTotal;     // Sum of all sensor readings received
    uint8_t sensorCount; // Number of DATA messages received
} MemberData;
static MemberData memberData[MAX_JOIN_MESSAGES];
static uint8_t memberCount = 0;

typedef enum
{
    ROLE_IDLE,
    ROLE_CLUSTER_HEAD,
    ROLE_JOINING,
    ROLE_MEMBER
} LEACH_Role;
static LEACH_Role nodeRole = ROLE_IDLE;
static uint32_t roundStartTime = 0; // Absolute time when the current round starts

// For role election, record last time this node was CH.
static uint32_t lastCHTime = 0;

// UART message processing.
static char msgBuffer[MAX_MSG_LEN];
static uint8_t msgIndex = 0;
static uint8_t inMessage = 0; // Flag indicating that a message is being received.

// Global flag for round completion (set by receiving a ROUND_COMPLETE message).
static volatile uint8_t roundEnded = 0;

// Global variable to track the last time a member node sent a DATA message.
static uint32_t lastMemberSend = 0;

// Hardcoded Node ID (ensure each node has a unique identifier).
static const char *NODE_ID = "NODE_B"; // Change for each node (NODE_A, NODE_B, NODE_C, etc.)

// ---------------------------------------------------------------------------
// Utility Functions
// ---------------------------------------------------------------------------

static void initRandomSeed(void)
{
    uint32_t seed = getMsCount(); // Get current uptime in ms (relatively unique between boots)
    for (size_t i = 0; i < strlen(NODE_ID); i++)
    {
        seed ^= ((uint32_t)NODE_ID[i]) << (i % 24); // Mix in NODE_ID characters
    }
    srand(seed);
    printf("Initialized random seed: %lu\r\n", seed);
}

static float getRandomFloat(void)
{
    return ((float)rand()) / ((float)RAND_MAX);
}

/**
 * @brief Sends a message using the AT+RDATAB command.
 *
 * The message is encapsulated in the format: ">TYPE,CONTENT<"
 * and the command sends the length (in two-digit hex) of the message.
 */
static void broadcast(const char *type, const char *content)
{
    char msg[MAX_MSG_LEN];
    snprintf(msg, sizeof(msg), "%c%s,%s%c", MSG_START, type, content, MSG_END);
    uint8_t len = (uint8_t)strlen(msg);

    char cmd[30];
    sprintf(cmd, "AT+RDATAB:%02X\r", len);
    usb_uart_USART_Write((uint8_t *)cmd, strlen(cmd));
    while (usb_uart_USART_WriteIsBusy())
    {
    }
    usb_uart_USART_Write((uint8_t *)msg, len);
    while (usb_uart_USART_WriteIsBusy())
    {
    }
    printf("Broadcasted: %s\r\n", msg);
}

/**
 * @brief Processes a complete message (delimited by MSG_START and MSG_END).
 *
 * Expected message format: "TYPE,DATA"
 *
 * The sscanf call:
 *    if (sscanf(msg, "%15[^,],%31[^<]", type, content) != 2)
 * reads up to 15 characters into 'type' until a comma is encountered and then up to 31 characters into 'content' until a '<' character.
 * If both tokens are not extracted, the message is ignored.
 */
static void handleMessage(const char *msg)
{
    char type[16] = {0};
    char content[32] = {0};
    if (sscanf(msg, "%15[^,],%31[^<]", type, content) != 2)
        return;

    if (strcmp(type, "CH_AD") == 0 && nodeRole == ROLE_JOINING)
    {
        printf("Received CH advertisement from %s\r\n", content);
        // Immediately join by broadcasting a JOIN request.
        broadcast("JOIN", NODE_ID);
        nodeRole = ROLE_MEMBER;
    }
    else if (strcmp(type, "JOIN") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
    {
        // For each JOIN request, record the joining node's ID for later aggregation.
        if (memberCount < MAX_JOIN_MESSAGES)
        {
            strncpy(memberData[memberCount].nodeID, content, sizeof(memberData[memberCount].nodeID) - 1);
            memberData[memberCount].nodeID[sizeof(memberData[memberCount].nodeID) - 1] = '\0';
            // Initialize aggregation values.
            memberData[memberCount].sensorTotal = 0;
            memberData[memberCount].sensorCount = 0;
            memberCount++;
            printf("CH recorded JOIN request from %s\r\n", content);
        }
    }
    else if (strcmp(type, "DATA") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
    {
        // DATA message expected format: "NODE_ID,sensorValue"
        char sender[16] = {0};
        int sensorVal = 0;
        if (sscanf(content, "%15[^,],%d", sender, &sensorVal) == 2)
        {
            int found = 0;
            for (uint8_t i = 0; i < memberCount; i++)
            {
                if (strcmp(memberData[i].nodeID, sender) == 0)
                {
                    // Accumulate the sensor value and increment count for averaging later.
                    memberData[i].sensorTotal += sensorVal;
                    memberData[i].sensorCount++;
                    found = 1;
                    break;
                }
            }
            if (!found && memberCount < MAX_JOIN_MESSAGES)
            {
                strncpy(memberData[memberCount].nodeID, sender, sizeof(memberData[memberCount].nodeID) - 1);
                memberData[memberCount].nodeID[sizeof(memberData[memberCount].nodeID) - 1] = '\0';
                memberData[memberCount].sensorTotal = sensorVal;
                memberData[memberCount].sensorCount = 1;
                memberCount++;
            }
            printf("CH received DATA from %s: %d\r\n", sender, sensorVal);
        }
    }
    else if (strcmp(type, "ROUND_COMPLETE") == 0)
    {
        // When a member receives ROUND_COMPLETE, mark the round as ended.
        if (nodeRole == ROLE_MEMBER || nodeRole == ROLE_JOINING)
        {
            roundEnded = 1;
            printf("Node %s received ROUND_COMPLETE. Ending round.\r\n", NODE_ID);
        }
    }
}

/**
 * @brief Processes incoming UART data one byte at a time in a non-blocking manner.
 */
static void processInput(void)
{
    uint8_t byte;
    if (!usb_uart_USART_ReadIsBusy())
    {
        if (usb_uart_USART_Read(&byte, 1) == 1)
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
                    msgBuffer[msgIndex++] = byte;
                if (byte == MSG_END)
                {
                    msgBuffer[msgIndex] = '\0';
                    // Process message without the start and end delimiters.
                    handleMessage(msgBuffer + 1);
                    inMessage = 0;
                    msgIndex = 0;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// LEACH Protocol Functions
// ---------------------------------------------------------------------------
/**
 * @brief Elects the node's role for the current round.
 *
 * The node becomes a Cluster Head (CH) with probability CH_PROBABILITY (in percent)
 * if it hasn't been CH too recently; otherwise, it becomes a joining node.
 */
static void electRole(void)
{
    uint32_t now = getMsCount();
    // Enforce a minimum interval between CH roles.
    if (now - lastCHTime < MIN_CH_INTERVAL_MS)
    {
        nodeRole = ROLE_JOINING;
        return;
    }
    // float rnd = getRandomFloat();
    float rnd = 0.1;
    rnd = 0.8;
    if ((rnd * 100) < CH_PROBABILITY)
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        lastCHTime = now;
        broadcast("CH_AD", NODE_ID);
        printf("Node %s elected as Cluster Head.\r\n", NODE_ID);
    }
    else
    {
        nodeRole = ROLE_JOINING;
        printf("Node %s will join a cluster.\r\n", NODE_ID);
    }
}

/**
 * @brief For joining nodes: wait for a CH advertisement for the entire round and then send a JOIN request.
 */
static void joinCluster(void)
{
    uint32_t start = getMsCount();
    while (getMsCount() - start < ROUND_DURATION_MS && !roundEnded)
    {
        processInput();
        if (nodeRole == ROLE_MEMBER) // Already joined
            return;
    }
    printf("Node %s did not receive any CH advertisement this round.\r\n", NODE_ID);
}

/**
 * @brief For member nodes: repeatedly send sensor data during the round.
 *
 * Members check if NODE_DELAY_MS has elapsed since their last DATA transmission and, if so, send a new DATA message.
 */
static void sendMemberData(void)
{
    uint32_t now = getMsCount();
    // Check if it's time to send another DATA message.
    if (now - lastMemberSend >= NODE_DELAY_MS)
    {
        int sensorValue = (int)(getRandomFloat() * 100); // Simulate sensor reading
        char dataContent[32];
        snprintf(dataContent, sizeof(dataContent), "%s,%d", NODE_ID, sensorValue);
        broadcast("DATA", dataContent);
        printf("Member %s sent sensor data: %d\r\n", NODE_ID, sensorValue);
        lastMemberSend = now;
    }
}

/**
 * @brief For CH nodes: aggregate sensor data from members.
 *
 * This function waits until near the end of the round (or until a ROUND_COMPLETE message is received)
 * then prints out the average sensor value for each member (computed as sensorTotal / sensorCount).
 */
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
            printf("Node %s: Avg Sensor Value = %d (Total = %d, Count = %d)\r\n",
                   memberData[i].nodeID, avg, memberData[i].sensorTotal, memberData[i].sensorCount);
        }
        else
        {
            printf("Node %s: No data received.\r\n", memberData[i].nodeID);
        }
    }
    printf("----------------------------------------------\r\n");
}

/**
 * @brief For CH nodes: broadcast a ROUND_COMPLETE message to signal round end.
 */
static void sendRoundComplete(void)
{
    broadcast("ROUND_COMPLETE", NODE_ID);
    roundEnded = 1;
    printf("CH %s broadcasted ROUND_COMPLETE.\r\n", NODE_ID);
}

/**
 * @brief Runs one complete LEACH round.
 *
 * - Elect role (CH or JOINING).
 * - If JOINING, wait for a CH advertisement for the entire round and then send a JOIN request.
 * - Steady-state phase:
 *      - CH nodes broadcast periodic CH_AD messages and accept multiple DATA messages from members.
 *      - Member nodes send DATA repeatedly whenever NODE_DELAY_MS elapses.
 * - At the end of the round, CH nodes broadcast a ROUND_COMPLETE message which causes members to finish early.
 * - Reset state for the next round.
 */
static void runRound(void)
{
    roundEnded = 0;
    roundStartTime = getMsCount();
    memberCount = 0;
    lastMemberSend = roundStartTime;

    electRole();

    if (nodeRole == ROLE_JOINING)
    {
        joinCluster();
        // if (nodeRole == ROLE_JOINING)
        // {
        //     return;
        // }
    }

    uint32_t roundStart = getMsCount();
    // Steady-state phase: run for the entire round duration (or until a ROUND_COMPLETE message is received).
    while (getMsCount() - roundStart < ROUND_DURATION_MS && !roundEnded)
    {
        processInput();
        // For CH nodes: periodically send CH advertisements.
        if (nodeRole == ROLE_CLUSTER_HEAD && ((getMsCount() - roundStart) % CH_AD_INTERVAL_MS) < 50)
        {
            broadcast("CH_AD", NODE_ID);
        }
        // For member nodes: repeatedly send DATA messages.
        if (nodeRole == ROLE_MEMBER)
        {
            sendMemberData();
        }
    }

    // For CH nodes: at the end of the round, broadcast a ROUND_COMPLETE message and aggregate any remaining data.
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        sendRoundComplete();
        aggregateData();
    }

    // End-of-round processing: reset state.
    nodeRole = ROLE_IDLE;
    roundEnded = 0;
    printf("Round complete.\r\n\n");
    delayMs(1000); // Short pause before next round.
}

/**
 * @brief Main loop for the LEACH protocol.
 */
int leach_main(void)
{
    systemInitialize();
    initRandomSeed(); // Initialize RNG seed

    roundStartTime = getMsCount();

    // Main LEACH loop: continuously run rounds.
    while (1)
    {
        runRound();
    }
    return 0;
}