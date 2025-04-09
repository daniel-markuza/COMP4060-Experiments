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
#define NODE_DELAY_MS 3000UL      // How long a member node waits before sending data (in ms)
#define ROUND_DURATION_MS 17000UL // Total duration of one LEACH round (in ms)
#define CH_PROBABILITY 25         // Percent chance (0-100) to become Cluster Head in a round
#define CH_AD_INTERVAL_MS 2000UL  // Interval between CH advertisements (in ms)
#define MAX_JOIN_MESSAGES 10      // Maximum number of join requests (and DATA messages) to store
#define MAX_MSG_LEN 50            // Maximum length for any message

// Adjust the minimum interval between consecutive CH roles so that a node is not prevented from becoming CH too long
#define MIN_CH_INTERVAL_MS 8000UL // Minimum time (in ms) between CH roles for a node

// ---------------------------------------------------------------------------
// Message Delimiters
// ---------------------------------------------------------------------------
#define MSG_START '>'
#define MSG_END '<'

// ---------------------------------------------------------------------------
// Data Structures and Global Variables
// ---------------------------------------------------------------------------
typedef enum
{
    ROLE_IDLE,
    ROLE_CLUSTER_HEAD,
    ROLE_JOINING,
    ROLE_MEMBER
} LEACH_Role;
static LEACH_Role nodeRole = ROLE_IDLE;
static uint32_t roundStartTime = 0; // Absolute time at which the current round starts

// For aggregation: store member sensor data (from DATA messages)
typedef struct
{
    char nodeID[16];
    int sensorValue;
} MemberData;
static MemberData memberData[MAX_JOIN_MESSAGES];
static uint8_t memberCount = 0;

// For simple role election, record last time this node was CH to avoid back-to-back CH roles
static uint32_t lastCHTime = 0;

// For UART message processing:
static char msgBuffer[MAX_MSG_LEN];
static uint8_t msgIndex = 0;
static uint8_t inMessage = 0; // Flag indicating a message is being received

// Global flag for round completion (set when a ROUND_COMPLETE message is received)
static volatile uint8_t roundEnded = 0;

// Hardcoded Node ID (ensure each node has a unique identifier)
static const char *NODE_ID = "NODE_A"; // Change for each node (NODE_A, NODE_B, NODE_C, etc.)

// ---------------------------------------------------------------------------
// Utility Functions
// ---------------------------------------------------------------------------

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
    // Create a message formatted as: ">TYPE,CONTENT<"
    snprintf(msg, sizeof(msg), "%c%s,%s%c", MSG_START, type, content, MSG_END);
    uint8_t len = (uint8_t)strlen(msg);

    char cmd[30];
    sprintf(cmd, "AT+RDATAB:%02X\r", len);
    usb_uart_USART_Write((uint8_t *)cmd, strlen(cmd));
    delayMs(20);
    usb_uart_USART_Write((uint8_t *)msg, len);
    printf("Broadcasted: %s\r\n", msg);
}

/**
 * @brief Processes a complete message (delimited by MSG_START and MSG_END).
 *
 * Expected message format: "TYPE,DATA"
 *
 * The sscanf call below:
 *    if (sscanf(msg, "%15[^,],%31[^<]", type, content) != 2)
 * means: read up to 15 characters until a comma into 'type', and then
 * read up to 31 characters until a '<' into 'content'. If 2 tokens are not
 * successfully extracted (i.e. the format is not exactly “TYPE,DATA”), then
 * the message is rejected.
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
        // Join the cluster by broadcasting JOIN request
        broadcast("JOIN", NODE_ID);
        nodeRole = ROLE_MEMBER;
    }
    else if (strcmp(type, "JOIN") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
    {
        // CH records the joining node's ID for aggregation.
        if (memberCount < MAX_JOIN_MESSAGES)
        {
            strncpy(memberData[memberCount].nodeID, content, sizeof(memberData[memberCount].nodeID) - 1);
            memberData[memberCount].nodeID[sizeof(memberData[memberCount].nodeID) - 1] = '\0';
            memberData[memberCount].sensorValue = 0;
            memberCount++;
            printf("CH recorded join request from %s\r\n", content);
        }
    }
    else if (strcmp(type, "DATA") == 0 && nodeRole == ROLE_CLUSTER_HEAD)
    {
        // Expect data in the format "NODE_ID,sensorValue"
        char sender[16] = {0};
        int sensorVal = 0;
        if (sscanf(content, "%15[^,],%d", sender, &sensorVal) == 2)
        {
            int found = 0;
            for (uint8_t i = 0; i < memberCount; i++)
            {
                if (strcmp(memberData[i].nodeID, sender) == 0)
                {
                    memberData[i].sensorValue = sensorVal;
                    found = 1;
                    break;
                }
            }
            if (!found && memberCount < MAX_JOIN_MESSAGES)
            {
                strncpy(memberData[memberCount].nodeID, sender, sizeof(memberData[memberCount].nodeID) - 1);
                memberData[memberCount].nodeID[sizeof(memberData[memberCount].nodeID) - 1] = '\0';
                memberData[memberCount].sensorValue = sensorVal;
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
                    // Process message without delimiters (pass msgBuffer+1)
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
 * Uses a random number (0.0 to 1.0) to decide whether the node becomes CH,
 * provided it hasn't been CH too recently.
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
    float rnd = getRandomFloat(); // Returns a float between 0 and 1
    if ((rnd * 100) < CH_PROBABILITY)
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        lastCHTime = now;
        // Broadcast CH advertisement
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
 * @brief For joining nodes, wait for a CH advertisement for an entire round.
 * Once a CH advertisement is received, send a JOIN request.
 */
static void joinCluster(void)
{
    uint32_t start = getMsCount();
    // Wait for the entire round duration to hear a CH advertisement
    while (getMsCount() - start < ROUND_DURATION_MS && !roundEnded)
    {
        processInput();
        if (nodeRole == ROLE_MEMBER)
        {
            return; // Already joined
        }
    }
    // If no CH advertisement received this round:
    printf("Node %s did not receive any CH advertisement this round.\r\n", NODE_ID);
}

/**
 * @brief For member nodes: wait for NODE_DELAY_MS then send sensor data to the CH.
 */
static void sendMemberData(void)
{
    uint32_t now = getMsCount();
    // Wait for the configured member delay within the round.
    while (getMsCount() - now < NODE_DELAY_MS)
    {
        processInput();
        if (roundEnded)
        {
            return; // End round early if a ROUND_COMPLETE message is received.
        }
    }
    // Simulate sensor reading (replace with real sensor code if available)
    int sensorValue = (int)(getRandomFloat() * 100);
    char dataContent[32];
    snprintf(dataContent, sizeof(dataContent), "%s,%d", NODE_ID, sensorValue);
    broadcast("DATA", dataContent);
    printf("Member %s sent sensor data: %d\r\n", NODE_ID, sensorValue);
}

/**
 * @brief For CH nodes: aggregate sensor data received near the end of the round.
 */
static void aggregateData(void)
{
    // Wait until near the end of the round or until a ROUND_COMPLETE message is received.
    while (getMsCount() - roundStartTime < (ROUND_DURATION_MS - 1000UL) && !roundEnded)
    {
        processInput();
    }
    // Print aggregated member sensor data.
    printf("\n--- Aggregated Data from Cluster Members ---\r\n");
    for (uint8_t i = 0; i < memberCount; i++)
    {
        printf("Node %s: Sensor Value = %d\r\n", memberData[i].nodeID, memberData[i].sensorValue);
    }
    printf("----------------------------------------------\r\n");
}

/**
 * @brief For CH nodes: broadcast a ROUND_COMPLETE message to end the round.
 */
static void sendRoundComplete(void)
{
    broadcast("ROUND_COMPLETE", NODE_ID);
    roundEnded = 1; // Signal locally that round is complete.
    printf("CH %s broadcasted ROUND_COMPLETE.\r\n", NODE_ID);
}

/**
 * @brief Runs one complete LEACH round.
 *
 * - Elect role (CH or JOINING).
 * - If JOINING, wait for CH advertisement for entire round and then send JOIN request.
 * - Steady-state: CH nodes broadcast periodic CH_AD and aggregate sensor DATA; member nodes wait for their TDMA slot and then send DATA.
 * - At the end of the round, CH nodes broadcast a ROUND_COMPLETE message; members listen for it.
 * - Reset state and wait for the next round.
 */
static void runRound(void)
{
    roundEnded = 0;
    roundStartTime = getMsCount();
    memberCount = 0;

    electRole();

    if (nodeRole == ROLE_JOINING)
    {
        joinCluster();
    }

    uint32_t roundStart = getMsCount();
    // Steady-state phase: run for the duration of the round.
    while (getMsCount() - roundStart < ROUND_DURATION_MS && !roundEnded)
    {
        processInput();
        // For CH nodes, send CH advertisements more frequently.
        if (nodeRole == ROLE_CLUSTER_HEAD && ((getMsCount() - roundStart) % CH_AD_INTERVAL_MS) < 50)
        {
            broadcast("CH_AD", NODE_ID);
        }
        // For member nodes that have already joined, send sensor data after waiting for NODE_DELAY_MS.
        if (nodeRole == ROLE_MEMBER)
        {
            sendMemberData();
        }
    }

    // For CH nodes: at the end of the round, broadcast ROUND_COMPLETE and aggregate any remaining data.
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        aggregateData();
        sendRoundComplete();
    }

    // End of round processing: reset role and state.
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

    roundStartTime = getMsCount();

    // Main LEACH loop: continuously run rounds.
    while (1)
    {
        runRound();
    }
    return 0;
}