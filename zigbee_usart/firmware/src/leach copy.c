#include "leach.h"
#include "utils.h"                            // Use your existing utility functions (delayMs, getMsCount, etc.)
#include "click_routines/usb_uart/usb_uart.h" // For usb_uart_USART_Write(), usb_uart_USART_Read(), etc.
#include <stdio.h>
#include <string.h>
#include <stdlib.h> // For atoi, if needed

// Global variables for LEACH module
static char nodeID[32] = {0}; // Unique Node ID (as string)
static LEACH_Role nodeRole = ROLE_IDLE;
static uint32_t roundStartTime = 0; // Timestamp for current round start

// Internal function prototypes
static bool queryDeviceID(void);
static float getRandomFloat(void);
static void broadcastMessage(const char *msg);
static bool checkForMessage(const char *expected, uint32_t timeout_ms, char *buffer, size_t bufferSize);
static void updateLEDIndicator(LEACH_Role role);
static void processRoundStart(void);
static void electRole(void);
static void joinCluster(void);
static void steadyStatePhase(void);

// This function queries the device's unique ID using the "ATI" command.
// It stores the result in the global nodeID variable. If it fails, a fallback value is generated.
static bool queryDeviceID(void)
{
    uint8_t response[100] = {0};
    // Send the ATI command
    if (!sendCommandAndReadResponse((uint8_t *)"ATI\r", "ATI Command", response, sizeof(response)))
    {
        printf("Failed to get device ID via ATI.\r\n");
        return false;
    }

    // Assume the response contains the EUI64 as part of the string.
    // For example, the response might be "Telegesis XYZ R309 0011223344556677 OK"
    // We will search for an 16-character hex substring.
    char *ptr = strstr((char *)response, "00"); // crude way: look for "00" as starting pattern
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

// Returns a random float between 0 and 1 using msCount as a seed.
// (This is a very simple pseudo-random generator for demonstration.)
static float getRandomFloat(void)
{
    // Use the lower bits of msCount as a random seed (not cryptographically secure)
    uint32_t seed = getMsCount();
    // For example, return (seed % 100) / 100.0f
    return ((float)(seed % 100)) / 100.0f;
}

// Broadcast a message over the ZigBee link
static void broadcastMessage(const char *msg)
{
    // For simplicity, assume msg is a null-terminated string
    // usb_uart_USART_Write((uint8_t *)msg, strlen(msg));
    // // Optionally wait until transmission is complete
    // while (usb_uart_USART_WriteIsBusy())
    //     ;
    printf("Broadcasted: %s\r\n", msg);
}

// Check if a message starting with 'expected' is received within timeout_ms.
// If received, the message is stored in buffer.
static bool checkForMessage(const char *expected, uint32_t timeout_ms, char *buffer, size_t bufferSize)
{
    uint32_t startTime = getMsCount();
    memset(buffer, 0, bufferSize);
    // In a real implementation, you would poll your serial or radio buffer.
    // Here, we assume a blocking read with timeout.
    while ((getMsCount() - startTime) < timeout_ms)
    {
        // Try reading available bytes into buffer
        size_t bytesRead = readAllBytesWithTimeout((uint8_t *)buffer, bufferSize);
        if (bytesRead > 0)
        {
            if (strncmp(buffer, expected, strlen(expected)) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

// Update LED pattern based on node role.
// For example:
// - ROLE_CLUSTER_HEAD: LED always ON
// - ROLE_MEMBER: LED blink slowly (500ms on/off)
// - ROLE_JOINING: LED blink fast (200ms on/off)
static void updateLEDIndicator(LEACH_Role role)
{
    // This function should control your LED.
    // Here we use handleLEDBlink() as a placeholder.
    // In a real implementation, you might adjust a timer or set PORT bits accordingly.
    switch (role)
    {
    case ROLE_CLUSTER_HEAD:
        // Set LED ON permanently
        PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14;
        break;
    case ROLE_MEMBER:
        // Blink slowly: every 500ms toggle LED
        if ((getMsCount() % 1000) < 500)
            PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14;
        else
            PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14;
        break;
    case ROLE_JOINING:
        // Blink fast: every 200ms toggle LED
        if ((getMsCount() % 400) < 200)
            PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14;
        else
            PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14;
        break;
    default:
        // Idle state: turn LED off
        PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14;
        break;
    }
}

// Process a ROUND_START message.
// Returns true if a round start message was received.
static void processRoundStart(void)
{
    char msgBuffer[50] = {0};
    // Check if a "ROUND_START" message is received within the backup timeout
    if (checkForMessage("ROUND_START", ROUND_START_TIMEOUT_MS, msgBuffer, sizeof(msgBuffer)))
    {
        printf("Received ROUND_START message: %s\r\n", msgBuffer);
        roundStartTime = getMsCount();
    }
    else
    {
        // If no message, use backup timer: if the last round is over, start new round.
        if ((getMsCount() - roundStartTime) >= ROUND_DURATION_MS)
        {
            printf("Backup timer triggered new round.\r\n");
            roundStartTime = getMsCount();
        }
    }
}

// Elect the node's role for the new round based on probability.
// If random < CH_PROBABILITY, become cluster head; else remain member.
static void electRole(void)
{
    float r = getRandomFloat();
    if (r < CH_PROBABILITY)
    {
        nodeRole = ROLE_CLUSTER_HEAD;
        printf("Node %s elected as Cluster Head.\r\n", nodeID);
        // Broadcast CH advertisement
        char msg[50];
        sprintf(msg, "CH_AD,%s", nodeID);
        broadcastMessage(msg);
    }
    else
    {
        nodeRole = ROLE_JOINING;
        printf("Node %s is a cluster member. Waiting for CH advertisement...\r\n", nodeID);
    }
    updateLEDIndicator(nodeRole);
}

// For a member node, listen for a CH advertisement and send a join request.
static void joinCluster(void)
{
    char msgBuffer[50] = {0};
    // Wait up to a short timeout (e.g., 2 seconds) for a CH advertisement
    if (checkForMessage("CH_AD", 2000, msgBuffer, sizeof(msgBuffer)))
    {
        // Assume message is in the format "CH_AD,<CH_NODE_ID>"
        // Parse the CH node ID (skip "CH_AD,")
        char *chID = strchr(msgBuffer, ',');
        if (chID != NULL)
        {
            chID++; // move past comma
            printf("Received CH advertisement from %s.\r\n", chID);
            // Send join request to CH
            char joinMsg[50];
            sprintf(joinMsg, "JOIN,%s", nodeID);
            broadcastMessage(joinMsg);
            nodeRole = ROLE_MEMBER;
            updateLEDIndicator(nodeRole);
        }
    }
    else
    {
        // No CH advertisement received; remain in joining state.
        printf("No CH advertisement received. Will try again in next round.\r\n");
    }
}

// In steady-state phase, a member sends its dummy sensor reading, and a CH aggregates data.
static void steadyStatePhase(void)
{
    // For demonstration, we use a fixed sensor value, e.g., "25"
    char dataMsg[50];
    if (nodeRole == ROLE_MEMBER)
    {
        // Member node sends its sensor data to the cluster head.
        sprintf(dataMsg, "DATA,%s,25", nodeID);
        broadcastMessage(dataMsg);
        printf("Node %s (member) sent data: 25\r\n", nodeID);
    }
    else if (nodeRole == ROLE_CLUSTER_HEAD)
    {
        // As a simple proof-of-concept, the CH just prints that it would aggregate data.
        printf("Node %s (CH) aggregating data from members...\r\n", nodeID);
        // For simplicity, we assume aggregation and then CH sends aggregated data.
        // In a full implementation, the CH would listen for incoming DATA messages,
        // aggregate them, and then forward a summary to the base station.
        sprintf(dataMsg, "DATA,%s,25", nodeID); // Here, CH sends its own data as aggregated data.
        broadcastMessage(dataMsg);
    }
}

// Main LEACH round procedure.
// 1. Wait for or trigger round start.
// 2. Elect role (CH or joining).
// 3. If joining, listen for CH advertisement and send join request.
// 4. Execute steady-state phase: send sensor data.
static void runRound(void)
{
    processRoundStart();
    electRole();

    if (nodeRole == ROLE_JOINING)
    {
        joinCluster();
    }

    // Wait a bit before entering steady state (simulate setup delay)
    delayMs(1000);

    // Steady-state phase: for the remainder of the round, periodically send sensor data.
    uint32_t steadyStart = getMsCount();
    while ((getMsCount() - steadyStart) < (ROUND_DURATION_MS - 3000))
    {
        steadyStatePhase();
        delayMs(5000); // send data every 5 seconds during steady-state
        updateLEDIndicator(nodeRole);
    }

    // Round complete; reset role for next round.
    nodeRole = ROLE_IDLE;
    updateLEDIndicator(nodeRole);
    printf("Round complete.\r\n");
}

// Public function to initialize the LEACH module.
void leach_init(void)
{
    // Initialize system and LED (assume systemInitialize is defined in utils.c)
    systemInitialize();

    // Query device ID using ATI command.
    if (!queryDeviceID())
    {
        // Fallback: generate a pseudo-unique ID using msCount
        sprintf(nodeID, "RAND_%lu", getMsCount());
        printf("Fallback Node ID: %s\r\n", nodeID);
    }

    // Optionally, join a ZigBee network here using your existing commands.
    // For now, assume the node is part of the network.

    // Reset round start timer
    roundStartTime = getMsCount();
    nodeRole = ROLE_IDLE;
    updateLEDIndicator(nodeRole);

    printf("LEACH module initialized. Node ID: %s\r\n", nodeID);
}

// Public function that should be called repeatedly in main loop.
void leach_main_loop(void)
{
    // For each round, run the LEACH process.
    runRound();
    // After a round, wait a short time before next round begins.
    delayMs(2000);
}