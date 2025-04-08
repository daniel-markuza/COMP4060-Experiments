#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "definitions.h"                      // SYS_Initialize() and hardware definitions.
#include "click_routines/usb_uart/usb_uart.h" // UART read/write functions.
#include "utils.h"                            // delayMs(), getMsCount(), etc.

// Select the synchronization strategy by uncommenting one of the following:
// (Default strategy is averaging)
#define SYNC_MODE_AVERAGE // Average all incoming flash times
// #define SYNC_MODE_LAST      // Use the last received value
// #define SYNC_MODE_FIRST     // Use the first received value

#define LED_FLASH_INTERVAL_MS 3000UL // Default flash interval (in ms)
#define MAX_SYNC_MESSAGES 10         // Maximum number of sync messages to store

// Global variables for synchronization state.
static uint32_t syncMessages[MAX_SYNC_MESSAGES];
static uint8_t syncCount = 0;
static volatile uint32_t myNextFlash = 0; // Scheduled time for the next flash (in ms)

static void send_sync_message(const char *msg)
{
    uint8_t msgLen = (uint8_t)strlen(msg);
    char cmdBuffer[30];

    // Format the RDATAB command with the message length.
    sprintf(cmdBuffer, "AT+RDATAB:%u\r", msgLen);
    usb_uart_USART_Write((uint8_t *)cmdBuffer, strlen(cmdBuffer));
    delayMs(50); // Optional delay to allow the module time to process the command.

    // Send the raw synchronization message.
    usb_uart_USART_Write((uint8_t *)msg, msgLen);
}

static void processIncomingMessage(char *msg)
{
    size_t len = strlen(msg);
    if (len < 3)
        return; // Message too short.

    if (msg[0] != '>' || msg[len - 1] != '<')
    {
        // Incorrect format; ignore message.
        return;
    }

    // Extract the numeric portion from between '>' and '<'.
    char numStr[20];
    size_t numLen = len - 2; // Exclude the '>' and '<'
    if (numLen >= sizeof(numStr))
        numLen = sizeof(numStr) - 1;

    strncpy(numStr, msg + 1, numLen);
    numStr[numLen] = '\0';

    // Convert the string to an unsigned integer.
    uint32_t flashTime = (uint32_t)strtoul(numStr, NULL, 10);

    // Save the extracted flash time if there is available space.
    if (syncCount < MAX_SYNC_MESSAGES)
    {
        syncMessages[syncCount++] = flashTime;
        printf("Received sync message: %lu\r\n", flashTime);
    }
}

static uint32_t computeNextFlash(void)
{
    uint32_t current = getMsCount();
    uint32_t nextFlashValue = current + LED_FLASH_INTERVAL_MS; // Fallback default

#ifdef SYNC_MODE_AVERAGE
    if (syncCount > 0)
    {
        uint64_t sum = 0;
        for (uint8_t i = 0; i < syncCount; i++)
            sum += syncMessages[i];
        nextFlashValue = (uint32_t)(sum / syncCount);
    }
#elif defined(SYNC_MODE_LAST)
    if (syncCount > 0)
        nextFlashValue = syncMessages[syncCount - 1];
#elif defined(SYNC_MODE_FIRST)
    if (syncCount > 0)
        nextFlashValue = syncMessages[0];
#endif

    // Ensure the next flash time is in the future.
    if (nextFlashValue <= current)
        nextFlashValue = current + LED_FLASH_INTERVAL_MS;

    return nextFlashValue;
}

void sync_experiment_run(void)
{
    // Initialize system modules (if not already done elsewhere).
    systemInitialize();

    // Set the initial flash time.
    myNextFlash = getMsCount() + LED_FLASH_INTERVAL_MS;
    char syncMessage[20];
    sprintf(syncMessage, ">%08lu<", myNextFlash);

    // Send the initial synchronization message using the RDATAB command.
    send_sync_message(syncMessage);
    printf("Starting synchronization experiment. Initial flash time: %lu\r\n", myNextFlash);

    // Variables for receiving UART data.
    uint8_t inByte;
    char rcvBuffer[30];
    uint8_t rcvIndex = 0;

    // Main loop for the experiment.
    while (1)
    {
        uint32_t current = getMsCount();

        // Check if it's time to flash the LED.
        if (current >= myNextFlash)
        {
            // Blink the LED: turn it on
            PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14;
            delayMs(100); // LED stays on for 100 milliseconds
            // Then turn the LED off
            PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14;

            // Compute the new flash time based on the selected strategy.
            myNextFlash = computeNextFlash();

            // Clear stored synchronization messages.
            syncCount = 0;

            // Prepare and broadcast the new flash time.
            sprintf(syncMessage, ">%08lu<", myNextFlash);
            send_sync_message(syncMessage);
            printf("Flashed at %lu, scheduled next flash at %lu\r\n", current, myNextFlash);
        }

        // Process incoming UART data one byte at a time.
        if (usb_uart_USART_Read(&inByte, 1))
        {
            // Reset buffer if start of message is detected.
            if (inByte == '>')
                rcvIndex = 0;

            // Append the received character to the buffer (if space permits).
            if (rcvIndex < (sizeof(rcvBuffer) - 1))
                rcvBuffer[rcvIndex++] = inByte;

            // When end-of-message marker is detected, process the complete message.
            if (inByte == '<')
            {
                rcvBuffer[rcvIndex] = '\0';
                printf("Received next flash time from peer: %s\r\n", rcvBuffer);
                processIncomingMessage(rcvBuffer);
                rcvIndex = 0;
            }
        }

        delayMs(10); // Short delay to reduce CPU load.
    }
}