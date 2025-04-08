#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "definitions.h"
#include "click_routines/usb_uart/usb_uart.h"
#include "utils.h"

// Configuration
#define BASE_FLASH_INTERVAL_MS 19000UL // Base flash interval
#define MIN_FLASH_INTERVAL_MS 1000UL   // Minimum allowed interval
#define MAX_FLASH_INTERVAL_MS 10000UL  // Maximum allowed interval
#define SYNC_THRESHOLD_MS 100UL        // Consider synced if within this range
#define SYNC_MESSAGE_RATE_MS 1000UL    // How often to send sync messages
#define LED_FLASH_DURATION_MS 200UL    // How long to keep LED on

// Message format: ">DELAY_MS<"
#define MSG_PREFIX '>'
#define MSG_SUFFIX '<'
#define MAX_MSG_LENGTH 15

// Global state
static uint32_t nextFlashTime = 0;
static uint32_t lastSyncSentTime = 0;
static uint8_t isFlashing = 0;

void sendSyncMessage()
{
    uint32_t currentTime = getMsCount();
    uint32_t delay = (nextFlashTime > currentTime) ? (nextFlashTime - currentTime) : 0;

    char msg[MAX_MSG_LENGTH];
    sprintf(msg, "%c%lu%c", MSG_PREFIX, delay, MSG_SUFFIX);

    // Send via UART
    char cmd[30];
    sprintf(cmd, "AT+RDATAB:%02X\r", (uint8_t)strlen(msg));
    usb_uart_USART_Write((uint8_t *)cmd, strlen(cmd));
    delayMs(20);
    usb_uart_USART_Write((uint8_t *)msg, strlen(msg));

    lastSyncSentTime = currentTime;
    printf("Sent: Flash in %lums\r\n", delay);
}

void processSyncMessage(uint32_t theirDelayMs)
{
    uint32_t currentTime = getMsCount();
    uint32_t theirFlashTime = currentTime + theirDelayMs;

    // Validate the delay is reasonable
    if (theirDelayMs > MAX_FLASH_INTERVAL_MS)
    {
        return;
    }

    // If their flash is sooner than ours, sync to it
    if (theirFlashTime < nextFlashTime)
    {
        uint32_t oldTime = nextFlashTime;
        nextFlashTime = theirFlashTime;

        printf("Adjust: %lu -> %lu (peer in %lums)\r\n",
               oldTime - currentTime, theirDelayMs, theirDelayMs);

        // Flash immediately if very soon
        if (theirDelayMs < 50 && !isFlashing)
        {
            PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // LED ON
            delayMs(LED_FLASH_DURATION_MS);
            PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // LED OFF
            isFlashing = 1;

            // Schedule next flash
            nextFlashTime = currentTime + BASE_FLASH_INTERVAL_MS;
            printf("Early flash! Next in %lums\r\n", BASE_FLASH_INTERVAL_MS);
        }
    }
}

void handleUARTInput(uint8_t ch)
{
    static char msgBuffer[MAX_MSG_LENGTH];
    static uint8_t msgIndex = 0;
    static uint8_t inMessage = 0;

    if (ch == MSG_PREFIX)
    {
        inMessage = 1;
        msgIndex = 0;
        msgBuffer[msgIndex++] = ch;
    }
    else if (inMessage && ch == MSG_SUFFIX)
    {
        msgBuffer[msgIndex++] = ch;
        msgBuffer[msgIndex] = '\0';

        // Parse message
        uint32_t theirDelay;
        if (sscanf(msgBuffer + 1, "%lu", &theirDelay) == 1)
        {
            processSyncMessage(theirDelay);
        }

        inMessage = 0;
    }
    else if (inMessage && msgIndex < MAX_MSG_LENGTH - 1)
    {
        msgBuffer[msgIndex++] = ch;
    }
}

void sync_experiment_run(void)
{
    systemInitialize();

    // Initial setup with random offset
    uint32_t currentTime = getMsCount();
    nextFlashTime = currentTime + BASE_FLASH_INTERVAL_MS + (currentTime % 1000);
    lastSyncSentTime = currentTime;
    isFlashing = 0;

    printf("Firefly Sync Started (Base: %lums)\r\n", BASE_FLASH_INTERVAL_MS);

    while (1)
    {
        currentTime = getMsCount();

        // Handle flashing
        if (currentTime >= nextFlashTime && !isFlashing)
        {
            isFlashing = 1;
            PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // LED ON
            delayMs(LED_FLASH_DURATION_MS);
            PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // LED OFF

            printf("Flashed! Next in %lums\r\n", BASE_FLASH_INTERVAL_MS);

            // Schedule next flash
            nextFlashTime = currentTime + BASE_FLASH_INTERVAL_MS;
            isFlashing = 0;
        }

        // Send periodic sync messages
        if (currentTime - lastSyncSentTime > SYNC_MESSAGE_RATE_MS)
        {
            sendSyncMessage();
        }

        // Handle incoming messages
        if (!usb_uart_USART_ReadIsBusy())
        {
            uint8_t ch;
            if (usb_uart_USART_Read(&ch, 1) == 1)
            {
                handleUARTInput(ch);
            }
        }
    }
}