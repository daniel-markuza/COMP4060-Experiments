#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "definitions.h"
#include "click_routines/usb_uart/usb_uart.h"
#include "utils.h"

// Configuration constants
#define FLASH_INTERVAL_MS 17000UL     // Normal time between flashes
#define MIN_FLASH_DELAY_MS 100UL      // Minimum allowed delay (safeguard)
#define LED_ON_DURATION_MS 200UL      // How long to keep LED lit
#define SYNC_MESSAGE_RATE_MS 1000UL   // How often to broadcast sync messages
#define MAX_SYNC_MESSAGES 10          // Max messages to store for averaging
#define NETWORK_DELAY_COMPENSATION 50 // Estimated network delay in ms

// Select sync strategy by uncommenting ONE of these:
// #define SYNC_MODE_AVERAGE    // Average all received delay values
// #define SYNC_MODE_LAST       // Use the last received delay value
#define SYNC_MODE_FIRST // Use the first received delay value

// Message format constants
#define MSG_START_CHAR '>'
#define MSG_END_CHAR '<'
#define MAX_MESSAGE_LEN 15

// Global state variables
static uint32_t nextFlashTime = 0;      // When we'll flash next (absolute time)
static uint32_t lastSyncSendTime = 0;   // When we last sent a sync message
static uint8_t isCurrentlyFlashing = 0; // Flag to prevent re-entrant flashes

// For sync message storage
static uint32_t receivedDelays[MAX_SYNC_MESSAGES];
static uint8_t receivedCount = 0;

/**
 * Safely calculates time until next flash with minimum delay enforcement
 */
uint32_t getSafeMsUntilFlash(uint32_t currentTime)
{
    if (nextFlashTime <= currentTime)
    {
        return MIN_FLASH_DELAY_MS; // Never return 0
    }
    uint32_t delay = nextFlashTime - currentTime;
    return (delay < MIN_FLASH_DELAY_MS) ? MIN_FLASH_DELAY_MS : delay;
}

/**
 * Broadcasts our next flash time to other nodes
 */
void sendSyncMessage()
{
    uint32_t now = getMsCount();
    uint32_t msUntilFlash = getSafeMsUntilFlash(now);

    char message[MAX_MESSAGE_LEN];
    sprintf(message, "%c%lu%c", MSG_START_CHAR, msUntilFlash, MSG_END_CHAR);

    // Format and send the UART command
    char command[30];
    sprintf(command, "AT+RDATAB:%02X\r", (uint8_t)strlen(message));
    usb_uart_USART_Write((uint8_t *)command, strlen(command));
    delayMs(20);
    usb_uart_USART_Write((uint8_t *)message, strlen(message));

    lastSyncSendTime = now;
    printf("Sent: flashing in %lums\r\n", msUntilFlash);
}

/**
 * Processes incoming sync messages based on selected strategy
 */
void processSyncMessage(uint32_t theirMsUntilFlash)
{
    uint32_t now = getMsCount();

    // Apply minimum delay safeguard
    if (theirMsUntilFlash < MIN_FLASH_DELAY_MS)
    {
        theirMsUntilFlash = MIN_FLASH_DELAY_MS;
    }

    // Store the received delay (compensated for network delay)
    uint32_t adjustedDelay = theirMsUntilFlash > NETWORK_DELAY_COMPENSATION ? theirMsUntilFlash - NETWORK_DELAY_COMPENSATION : MIN_FLASH_DELAY_MS;

    if (receivedCount < MAX_SYNC_MESSAGES)
    {
        receivedDelays[receivedCount++] = adjustedDelay;
    }

    // Apply the selected synchronization strategy
    uint32_t strategyDelay;

#ifdef SYNC_MODE_AVERAGE
    // Strategy 1: Average all received delays
    if (receivedCount > 0)
    {
        uint64_t sum = 0;
        for (uint8_t i = 0; i < receivedCount; i++)
        {
            sum += receivedDelays[i];
        }
        strategyDelay = (uint32_t)(sum / receivedCount);
    }
    else
    {
        strategyDelay = adjustedDelay;
    }
#elif defined(SYNC_MODE_LAST)
    // Strategy 2: Use the last received delay
    strategyDelay = adjustedDelay;
#elif defined(SYNC_MODE_FIRST)
    // Strategy 3: Use the first received delay
    strategyDelay = receivedCount > 0 ? receivedDelays[0] : adjustedDelay;
#else
    // Default to last received if no strategy selected
    strategyDelay = adjustedDelay;
#endif

    // Ensure we never schedule a flash in the past
    uint32_t proposedFlashTime = now + strategyDelay;
    if (proposedFlashTime <= now)
    {
        proposedFlashTime = now + MIN_FLASH_DELAY_MS;
    }

    // Only adjust if the new time is earlier than our current schedule
    if (proposedFlashTime < nextFlashTime)
    {
        printf("Adjusting flash from %lu to %lu (delta: %ldms)\r\n",
               nextFlashTime - now, strategyDelay,
               (long)(nextFlashTime - proposedFlashTime));

        nextFlashTime = proposedFlashTime;

        // Immediate flash if very soon (but not too soon)
        if (strategyDelay < (MIN_FLASH_DELAY_MS * 2) && !isCurrentlyFlashing)
        {
            PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // LED ON
            delayMs(LED_ON_DURATION_MS);
            PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // LED OFF
            isCurrentlyFlashing = 1;

            // Schedule next normal flash with safeguards
            nextFlashTime = now + FLASH_INTERVAL_MS;
            receivedCount = 0; // Reset for next round
            printf("Early flash! Next in %lums\r\n", FLASH_INTERVAL_MS);
        }
    }
}

// ... (rest of the code remains the same as previous version) ...

/**
 * Handles incoming UART data byte by byte
 */
void handleIncomingByte(uint8_t incomingByte)
{
    static char messageBuffer[MAX_MESSAGE_LEN];
    static uint8_t bufferIndex = 0;
    static uint8_t isReceivingMessage = 0;

    if (incomingByte == MSG_START_CHAR)
    {
        isReceivingMessage = 1;
        bufferIndex = 0;
        messageBuffer[bufferIndex++] = incomingByte;
    }
    else if (isReceivingMessage && incomingByte == MSG_END_CHAR)
    {
        messageBuffer[bufferIndex++] = incomingByte;
        messageBuffer[bufferIndex] = '\0';

        // Extract the delay value from the message
        uint32_t theirDelay;
        if (sscanf(messageBuffer + 1, "%lu", &theirDelay) == 1)
        {
            processSyncMessage(theirDelay);
        }

        isReceivingMessage = 0;
    }
    else if (isReceivingMessage && bufferIndex < MAX_MESSAGE_LEN - 1)
    {
        messageBuffer[bufferIndex++] = incomingByte;
    }
}

/**
 * Main firefly synchronization routine
 */
void sync_experiment_run(void)
{
    systemInitialize();

    // Initialize with random offset
    uint32_t now = getMsCount();
    nextFlashTime = now + FLASH_INTERVAL_MS + (now % 1000);
    lastSyncSendTime = now;
    isCurrentlyFlashing = 0;
    receivedCount = 0;

    printf("Firefly sync started (Mode: ");
#ifdef SYNC_MODE_AVERAGE
    printf("AVERAGE");
#elif defined(SYNC_MODE_LAST)
    printf("LAST");
#elif defined(SYNC_MODE_FIRST)
    printf("FIRST");
#else
    printf("DEFAULT (LAST)");
#endif
    printf(", Interval: %lums)\r\n", FLASH_INTERVAL_MS);

    while (1)
    {
        now = getMsCount();

        // Handle regular flashing
        if (now >= nextFlashTime && !isCurrentlyFlashing)
        {
            isCurrentlyFlashing = 1;

            // Flash the LED
            PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // LED ON
            delayMs(LED_ON_DURATION_MS);
            PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // LED OFF

            printf("Flashed! Next in %lums\r\n", FLASH_INTERVAL_MS);

            // Schedule next flash and reset message counter
            nextFlashTime = now + FLASH_INTERVAL_MS;
            receivedCount = 0;
            isCurrentlyFlashing = 0;
        }

        // Periodically broadcast our sync message
        if (now - lastSyncSendTime > SYNC_MESSAGE_RATE_MS)
        {
            sendSyncMessage();
        }

        // Process incoming messages
        if (!usb_uart_USART_ReadIsBusy())
        {
            uint8_t incomingByte;
            if (usb_uart_USART_Read(&incomingByte, 1) == 1)
            {
                handleIncomingByte(incomingByte);
            }
        }
    }
}