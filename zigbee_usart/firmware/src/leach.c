#include "leach.h"
#include "utils.h"                            // Provides delayMs(), getMsCount(), sendCommandAndReadResponse(), readAllBytesWithTimeout(), etc.
#include "click_routines/usb_uart/usb_uart.h" // Provides usb_uart_USART_Write(), usb_uart_USART_Read(), etc.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global variables for LEACH operation
static char nodeID[32] = {0}; // Unique Node ID (queried via ATI, fallback if necessary)
static LEACH_Role nodeRole = ROLE_IDLE;
static uint32_t roundStartTime = 0; // Timestamp marking the start of the current round

// Timing intervals (in ms)
#define MEMBER_DATA_INTERVAL 5000UL   // Members send sensor data every 5000ms
#define CH_AD_INTERVAL 3000UL         // CH advertises its status every 3000ms
#define ROUND_MARGIN 2000UL           // End round 2000ms before total round duration
#define ROUND_START_TIMEOUT_MS 5000UL // Timeout for waiting for round start message

// Forward declarations
static bool queryDeviceID(void);
static uint32_t computeSeed(void);
static void initRandom(void);
static float getRandomFloat(void);
static void rdatabBroadcastMessage(const char *msg);
static bool waitForMessage(const char *expected, uint32_t timeout_ms, char *buffer, size_t bufferSize);
static bool parseRawMessageForKeyword(const char *rawMsg, const char *keyword, char *chID, size_t chIDSize);
static void updateLEDIndicator(LEACH_Role role);
static void blinkMember(void);
static void processRoundStart(void);
static void electRole(void);
static void joinCluster(void);
static void runSteadyState(void);
static void chListenAndAcknowledge(void);
static void roundComplete(void);

//----------------------------------------------------------
// Query the device's unique ID using the ATI command.
static bool queryDeviceID(void)
{
    uint8_t response[100] = {0};
    if (!sendCommandAndReadResponse((uint8_t *)AT_GET_ID, "ATI Command", response, sizeof(response)))
    {
        printf("Failed to get device ID via ATI.\r\n");
        return false;
    }
    // Look for a 16-character hex string (simple heuristic).
    char *ptr = strstr((char *)response, "00");
    if (ptr && strlen(ptr) >= 16)
    {
        strncpy(nodeID, ptr, 16);
        nodeID[16] = '\0';
        printf("Device ID: %s\r\n", nodeID);
        return true;
    }
    else
    {
        printf("Device ID not found in ATI response.\r\n");
        return false;
    }
}

//----------------------------------------------------------
// Compute a seed using getMsCount() mixed with the nodeID to improve uniqueness.
static uint32_t computeSeed(void)
{
    uint32_t seed = getMsCount(); // Time-based component
    for (size_t i = 0; i < strlen(nodeID); i++)
    {
        seed ^= ((uint32_t)nodeID[i]) << (i % 32);
    }
    return seed;
}

//----------------------------------------------------------
// Initialize the random number generator.
static void initRandom(void)
{
    srand(computeSeed());
}

//----------------------------------------------------------
// Improved random function using the standard library.
// Returns a float between 0 and 1.
static float getRandomFloat(void)
{
    return ((float)rand()) / ((float)RAND_MAX);
}

//----------------------------------------------------------
// Broadcast a text message using the AT+RDATAB command.
// First, send "AT+RDATAB:<num_bytes>\r" (with num_bytes in hex), then the actual message.
static void rdatabBroadcastMessage(const char *msg)
{
    char response[100];
    char cmdBuffer[32];
    size_t msgLen = strlen(msg);
    sprintf(cmdBuffer, AT_RDATAB_PREFIX "%02X\r", (unsigned int)msgLen);
    sendCommandAndReadResponse((uint8_t *)cmdBuffer, "RDATAB command", (uint8_t *)response, sizeof(response));
    delayMs(50); // Allow module to switch to data mode.
    sendCommandAndReadResponse((uint8_t *)msg, "sent message", (uint8_t *)response, sizeof(response));
    // Uncomment the next line for debugging:
    // printf("Broadcasted: %s\r\n", msg);
}

//----------------------------------------------------------
// Wait for a message starting with 'expected' string.
static bool waitForMessage(const char *expected, uint32_t timeout_ms, char *buffer, size_t bufferSize)
{
    uint32_t startTime = getMsCount();
    memset(buffer, 0, bufferSize);
    while ((getMsCount() - startTime) < timeout_ms)
    {
        size_t bytesRead = readAllBytesWithTimeout((uint8_t *)buffer, bufferSize - 1);
        if (bytesRead > 0)
        {
            buffer[bytesRead] = '\0';
            if (strstr(buffer, expected))
            {
                return true;
            }
        }
        delayMs(10); // Yield briefly.
    }
    return false;
}

//----------------------------------------------------------
// Parse a RAW message (e.g., "\nRAW:-35,CH_AD,000D6F0018CEE118\r\n").
// If the second token equals 'keyword', copy the third token into chID.
static bool parseRawMessageForKeyword(const char *rawMsg, const char *keyword, char *chID, size_t chIDSize)
{
    char temp[128];
    strncpy(temp, rawMsg, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    char *start = temp;
    while (*start == '\n' || *start == '\r' || *start == ' ')
        start++;
    size_t len = strlen(start);
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' '))
    {
        start[len - 1] = '\0';
        len--;
    }
    if (strncmp(start, "RAW:", 4) != 0)
        return false;
    char *data = start + 4;
    char *token;
    int tokenIndex = 0;
    token = strtok(data, ",");
    while (token != NULL)
    {
        if (tokenIndex == 1)
        {
            if (strcmp(token, keyword) == 0)
            {
                token = strtok(NULL, ",");
                if (token != NULL)
                {
                    strncpy(chID, token, chIDSize - 1);
                    chID[chIDSize - 1] = '\0';
                    return true;
                }
            }
        }
        token = strtok(NULL, ",");
        tokenIndex++;
    }
    return false;
}

//----------------------------------------------------------
// Blink the LED for a member node after sending data.
// Turns LED on (OUTCLR) for 200ms then off (OUTSET).
static void blinkMember(void)
{
    PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // LED ON
    delayMs(200);
    PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // LED OFF
}

//----------------------------------------------------------
// Update LED indicator based on current node role.
static void updateLEDIndicator(LEACH_Role role)
{
    switch (role)
    {
    case ROLE_CLUSTER_HEAD:
        PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // LED ON permanently
        break;
    case ROLE_JOINING:
        PORT_REGS->GROUP[0].PORT_OUTTGL = PORT_PA14; // Toggle LED
        break;
    case ROLE_MEMBER:
        PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // temporarily turn off to start the blinking
        break;
    default:
        PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14;
        break;
    }
}

//----------------------------------------------------------
// Process the round start: wait for a "ROUND_START" message.
// The cluster head is expected to initiate the round, so members wait.
static void processRoundStart(void)
{
    // If we were CH last round, broadcast new round start
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        rdatabBroadcastMessage("ROUND_START");
        roundStartTime = getMsCount();
        printf("CH %s initiated new round.\r\n", nodeID);
    }
    else // Members wait for round start message
    {
        char msgBuffer[50] = {0};
        if (waitForMessage("ROUND_START", ROUND_START_TIMEOUT_MS, msgBuffer, sizeof(msgBuffer)))
        {
            printf("Received ROUND_START message: %s\r\n", msgBuffer);
            roundStartTime = getMsCount();
        }
        else
        {
            printf("Round start timeout. Starting anyway.\r\n");
            roundStartTime = getMsCount();
        }
    }
}

//----------------------------------------------------------
// Elect the node's role for the current round.
static void electRole(void)
{
    float r = getRandomFloat();
    // if (r < CH_PROBABILITY)
    if (r < 0)
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        printf("Node %s elected as Cluster Head.\r\n", nodeID);
        char msg[50];
        sprintf(msg, "CH_AD,%s", nodeID);
        rdatabBroadcastMessage(msg);
        // Play a tune on a newly elected CH.
        uint8_t identResp[50];
        char identCmd[64];
        sprintf(identCmd, "AT+IDENT:%s\r", nodeID);
        sendCommandAndReadResponse((uint8_t *)identCmd, "AT+IDENT", identResp, sizeof(identResp));
    }
    else
    {
        nodeRole = ROLE_JOINING;
        printf("Node %s is joining as a member. Waiting for CH advertisement...\r\n", nodeID);
    }
    updateLEDIndicator(nodeRole);
}

//----------------------------------------------------------
// For a joining node, continuously listen for a RAW message containing "CH_AD"
static void joinCluster(void)
{
    char rawBuffer[128] = {0};
    char chID[32] = {0};
    uint32_t joinStart = getMsCount();
    while (getMsCount() - joinStart < ROUND_DURATION_MS) // Timeout after one round.
    {
        if (waitForMessage("RAW:", 1000, rawBuffer, sizeof(rawBuffer)))
        {
            if (parseRawMessageForKeyword(rawBuffer, "CH_AD", chID, sizeof(chID)))
            {
                printf("Received advertisement from CH %s.\r\n", chID);
                char joinMsg[50];
                sprintf(joinMsg, "JOIN,%s", nodeID);
                rdatabBroadcastMessage(joinMsg);
                nodeRole = ROLE_MEMBER;
                updateLEDIndicator(nodeRole);
                return;
            }
        }
        updateLEDIndicator(nodeRole);
    }
    printf("Join timeout: No CH advertisement received this round. Will retry in next round.\r\n");
}

//----------------------------------------------------------
// For a cluster head, continuously listen for incoming RAW messages
static void chListenAndAcknowledge(void)
{
    char rawBuffer[128] = {0};
    char senderID[32] = {0};

    if (waitForMessage("RAW:", 1000, rawBuffer, sizeof(rawBuffer)))
    {
        if (parseRawMessageForKeyword(rawBuffer, "JOIN", senderID, sizeof(senderID)))
        {
            printf("CH %s received JOIN from %s.\r\n", nodeID, senderID);
            char ackMsg[50];
            sprintf(ackMsg, "ACK_JOIN,%s", senderID);
            rdatabBroadcastMessage(ackMsg);
        }
        else if (parseRawMessageForKeyword(rawBuffer, "DATA", senderID, sizeof(senderID)))
        {
            // Tokenize to extract sender ID and sensor value.
            char temp[128];
            strncpy(temp, rawBuffer, sizeof(temp) - 1);
            temp[sizeof(temp) - 1] = '\0';
            char *token;
            int tokenIndex = 0;
            char localSender[32] = {0};
            char localData[32] = {0};
            token = strtok(temp, ",");
            while (token != NULL)
            {
                if (tokenIndex == 1)
                {
                    if (strcmp(token, "DATA") != 0)
                        break;
                }
                else if (tokenIndex == 2)
                {
                    strncpy(localSender, token, sizeof(localSender) - 1);
                }
                else if (tokenIndex == 3)
                {
                    strncpy(localData, token, sizeof(localData) - 1);
                    break;
                }
                token = strtok(NULL, ",");
                tokenIndex++;
            }
            if (strlen(localSender) > 0 && strlen(localData) > 0)
            {
                printf("CH %s received DATA from %s: %s\r\n", nodeID, localSender, localData);
            }
        }
    }
}

//----------------------------------------------------------
// Run the steady-state phase using tracker variables (non-blocking).
static void runSteadyState(void)
{
    uint32_t lastDataSend = getMsCount();
    uint32_t lastCHAd = getMsCount();
    uint32_t steadyStart = getMsCount();
    char syncBuffer[128] = {0}; // Increased buffer size

    while (true)
    {
        uint32_t now = getMsCount();
        uint32_t elapsed = now - steadyStart;

        if (nodeRole == ROLE_MEMBER)
        {
            // Send sensor data with safety margin
            if ((now - lastDataSend >= MEMBER_DATA_INTERVAL) &&
                (elapsed <= (ROUND_DURATION_MS - (2 * MEMBER_DATA_INTERVAL))))
            {
                int reading = 20 * (getRandomFloat() + 1);
                char dataMsg[50];
                sprintf(dataMsg, "DATA,%s,%d", nodeID, reading);
                rdatabBroadcastMessage(dataMsg);
                printf("Node %s (member) sent sensor data: %d\r\n", nodeID, reading);
                blinkMember();
                lastDataSend = now;
            }

            // Check for ROUND_COMPLETE with minimal delay
            size_t bytes = readAllBytesWithTimeout((uint8_t *)syncBuffer, sizeof(syncBuffer) - 1);
            if (bytes > 0)
            {
                syncBuffer[bytes] = '\0';
                if (strstr(syncBuffer, "ROUND_COMPLETE"))
                {
                    printf("Member %s received ROUND_COMPLETE. Ending steady state.\r\n", nodeID);
                    break;
                }
            }
        }
        else if (nodeRole == ROLE_CLUSTER_HEAD)
        {
            // Send CH advertisement every CH_AD_INTERVAL
            if (now - lastCHAd >= CH_AD_INTERVAL)
            {
                char adMsg[50];
                sprintf(adMsg, "CH_AD,%s", nodeID);
                rdatabBroadcastMessage(adMsg);
                lastCHAd = now;
            }

            // Listen for JOIN/DATA messages
            chListenAndAcknowledge();

            // End steady state when the CH's round duration expires
            if (elapsed >= (ROUND_DURATION_MS - ROUND_MARGIN))
            {
                // Send final CH advertisement
                char adMsg[50];
                sprintf(adMsg, "CH_AD,%s", nodeID);
                rdatabBroadcastMessage(adMsg);
                break;
            }
        }

        delayMs(10); // Yield briefly
    }
}

//----------------------------------------------------------
// Only the cluster head controls round completion.
static void roundComplete(void)
{
    rdatabBroadcastMessage("ROUND_COMPLETE");
    printf("CH %s broadcasted ROUND_COMPLETE command.\r\n", nodeID);
    delayMs(500); // Allow time for members to receive.
}

//----------------------------------------------------------
// Run one complete LEACH round.
static void runRound(void)
{
    processRoundStart();
    electRole();

    if (nodeRole == ROLE_JOINING)
    {
        joinCluster();
    }

    delayMs(1000); // Allow time for cluster formation.

    if (nodeRole == ROLE_JOINING)
    {
        return;
    }

    // Run the steady-state phase (non-blocking).
    runSteadyState();

    // Only the CH broadcasts ROUND_COMPLETE.
    if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        roundComplete();
    }

    nodeRole = ROLE_IDLE;
    updateLEDIndicator(nodeRole);
    printf("Round complete.\r\n\r\n");
}

//----------------------------------------------------------
// Public initialization function for LEACH.
void leach_init(void)
{
    systemInitialize();
    uint8_t buffer[100];
    sendCommandAndReadResponse((uint8_t *)AT_RESET, "Reset", buffer, sizeof(buffer));
    delayMs(1000);

    sendCommandAndReadResponse((uint8_t *)AT_JOIN_NETWORK, "Join Network", buffer, sizeof(buffer));
    delayMs(500);

    sendCommandAndReadResponse((uint8_t *)AT_SET_CHANNEL, "Set Channel", buffer, sizeof(buffer));
    delayMs(500);

    if (!queryDeviceID())
    {
        sprintf(nodeID, "RAND_%lu", getMsCount());
        printf("Fallback Node ID: %s\r\n", nodeID);
    }

    initRandom();

    roundStartTime = getMsCount();
    nodeRole = ROLE_IDLE;
    updateLEDIndicator(nodeRole);

    printf("LEACH module initialized. Node ID: %s\r\n\r\n", nodeID);
}

//----------------------------------------------------------
// Main loop for LEACH operation.
void leach_main_loop(void)
{
    runRound();
    delayMs(1000); // Short pause between rounds.
}