// #include <stdio.h>
// #include <stdint.h>
// #include <string.h>
// #include <stdlib.h>
// #include "definitions.h"                      // SYS_Initialize() and hardware definitions.
// #include "click_routines/usb_uart/usb_uart.h" // UART read/write functions.
// #include "utils.h"                            // delayMs(), getMsCount(), etc.

// // Select the synchronization strategy by uncommenting one of the following:
// // (Default strategy is averaging)
// // #define SYNC_MODE_AVERAGE // Average all incoming delay values
// #define SYNC_MODE_LAST // Use the last received value
// // #define SYNC_MODE_FIRST     // Use the first received value

// #define LED_FLASH_INTERVAL_MS 5000UL // Default flash interval (in ms)
// #define MAX_SYNC_MESSAGES 10         // Maximum number of sync messages to store

// #define DELAY_COMPENSATION_MS 10UL // Estimated delay to subtract from received value (in ms)
// #define MIN_DELAY_MS 0UL           // Minimum allowed delay to avoid too rapid flashing

// // Global variables for synchronization state.
// // The syncMessages array stores received relative delays (in ms).
// static uint32_t syncMessages[MAX_SYNC_MESSAGES];
// static uint8_t syncCount = 0;
// // myNextFlash represents the absolute time (in ms) when the next flash should occur.
// static volatile uint32_t myNextFlash = 0;

// /**
//  * @brief Sends a synchronization message using the AT+RDATAB command.
//  *
//  * The message contains the relative delay (in ms) until the next flash.
//  * The size (in bytes) is formatted as a two-digit hexadecimal value.
//  *
//  * @param msg A null-terminated string representing the sync message.
//  */
// static void send_sync_message(const char *msg)
// {
//     uint8_t msgLen = (uint8_t)strlen(msg);
//     char cmdBuffer[30];

//     // Format the RDATAB command with the message length in two-digit hexadecimal.
//     sprintf(cmdBuffer, "AT+RDATAB:%02X\r", msgLen);
//     usb_uart_USART_Write((uint8_t *)cmdBuffer, strlen(cmdBuffer));
//     delayMs(50); // Allow time for the module to process the command.

//     // Send the raw synchronization message.
//     usb_uart_USART_Write((uint8_t *)msg, msgLen);
// }

// /**
//  * @brief Processes a received synchronization message.
//  *
//  * The expected message format is ">xxxx<", where xxxx is a 32-bit unsigned integer
//  * (in decimal) representing the relative delay (in ms) until the sender's next flash.
//  * A fixed compensation (DELAY_COMPENSATION_MS) is subtracted from the received delay.
//  *
//  * @param msg A null-terminated string containing the complete message.
//  */
// static void processIncomingMessage(char *msg)
// {
//     size_t len = strlen(msg);
//     if (len < 3)
//         return; // Message too short.

//     if (msg[0] != '>' || msg[len - 1] != '<')
//     {
//         // Incorrect format; ignore message.
//         return;
//     }

//     // Extract the numeric portion between '>' and '<'.
//     char numStr[20];
//     size_t numLen = len - 2; // Exclude the '>' and '<'.
//     if (numLen >= sizeof(numStr))
//         numLen = sizeof(numStr) - 1;

//     strncpy(numStr, msg + 1, numLen);
//     numStr[numLen] = '\0';

//     // Convert the string to an unsigned integer representing the relative delay.
//     uint32_t receivedDelay = (uint32_t)strtoul(numStr, NULL, 10);

//     // Adjust the received delay by subtracting the compensation constant.
//     uint32_t adjustedDelay = (receivedDelay > DELAY_COMPENSATION_MS) ? (receivedDelay - DELAY_COMPENSATION_MS) : 0;

//     // Save the adjusted delay if there is available space.
//     if (syncCount < MAX_SYNC_MESSAGES)
//     {
//         syncMessages[syncCount++] = adjustedDelay;
//         printf("Received sync delay: %lu ms\r\n", adjustedDelay);
//     }
// }

// /**
//  * @brief Computes the new relative delay (in ms) until the next flash.
//  *
//  * This function uses the selected synchronization strategy (by default, averaging)
//  * by combining this device's own remaining delay with the adjusted delays received
//  * from neighbors.
//  *
//  * @return uint32_t The computed relative delay until the next flash in ms.
//  */
// static uint32_t computeNextFlash(void)
// {
//     uint32_t current = getMsCount();
//     // Calculate our own remaining delay. Normally, myNextFlash > current,
//     // so selfDelay is the time remaining until our next flash.
//     uint32_t selfDelay = (myNextFlash > current) ? (myNextFlash - current) : LED_FLASH_INTERVAL_MS;

// #ifdef SYNC_MODE_AVERAGE
//     if (syncCount > 0)
//     {
//         uint64_t sum = 0;
//         for (uint8_t i = 0; i < syncCount; i++)
//             sum += syncMessages[i];
//         uint32_t avgDelay = (uint32_t)(sum / syncCount);
//         if (avgDelay < MIN_DELAY_MS)
//             avgDelay = MIN_DELAY_MS;
//         // Choose the minimum between our self delay and the average peer delay.
//         return (selfDelay < avgDelay) ? selfDelay : avgDelay;
//     }
//     else
//     {
//         return selfDelay;
//     }
// #elif defined(SYNC_MODE_LAST)
//     if (syncCount > 0)
//     {
//         uint32_t peerDelay = syncMessages[syncCount - 1];
//         if (peerDelay < MIN_DELAY_MS)
//             peerDelay = MIN_DELAY_MS;
//         // Choose the minimum between our self delay and the last received delay.
//         return (selfDelay < peerDelay) ? selfDelay : peerDelay;
//     }
//     else
//     {
//         return selfDelay;
//     }
// #elif defined(SYNC_MODE_FIRST)
//     if (syncCount > 0)
//     {
//         uint32_t peerDelay = syncMessages[0];
//         if (peerDelay < MIN_DELAY_MS)
//             peerDelay = MIN_DELAY_MS;
//         // Choose the minimum between our self delay and the first received delay.
//         return (selfDelay < peerDelay) ? selfDelay : peerDelay;
//     }
//     else
//     {
//         return selfDelay;
//     }
// #endif

//     return LED_FLASH_INTERVAL_MS;
// }

// /**
//  * @brief Runs the synchronization experiment.
//  *
//  * The device sends a relative delay value (time remaining until its next flash)
//  * via the AT+RDATAB command. It then listens for similar messages from its neighbors,
//  * adjusts for transmission delays, computes a new relative delay based on the selected
//  * strategy, and schedules its next flash accordingly.
//  *
//  * This function is intended to be called from an external main() function.
//  */
// void sync_experiment_run(void)
// {
//     // Initialize system modules (if not already done elsewhere).
//     systemInitialize();

//     // Set the initial flash by adding the default delay to the current time.
//     myNextFlash = getMsCount() + LED_FLASH_INTERVAL_MS;
//     char syncMessage[20];
//     // Send the initial relative delay.
//     sprintf(syncMessage, ">%08lu<", LED_FLASH_INTERVAL_MS);
//     send_sync_message(syncMessage);
//     printf("Starting synchronization experiment. Initial delay: %lu ms\r\n", LED_FLASH_INTERVAL_MS);

//     // Variables for receiving UART data.
//     uint8_t read_chars[100];
//     char out_string[200];
//     char rcvBuffer[30];
//     uint8_t rcvIndex = 0;

//     // Main loop for the experiment.
//     while (1)
//     {
//         uint32_t current = getMsCount();

//         // Check if it's time to flash the LED.
//         if (current >= myNextFlash)
//         {
//             // Blink the LED: turn it on, wait 100 ms, then turn it off.
//             PORT_REGS->GROUP[0].PORT_OUTCLR = PORT_PA14; // Turn LED ON.
//             delayMs(100);
//             PORT_REGS->GROUP[0].PORT_OUTSET = PORT_PA14; // Turn LED OFF.

//             // Compute the new relative delay based on the selected strategy.
//             uint32_t newDelay = computeNextFlash();
//             // Reset synchronization messages for the next round.
//             syncCount = 0;
//             // Set the new next flash time.
//             myNextFlash = current + newDelay;
//             // Prepare and broadcast the new relative delay.
//             sprintf(syncMessage, ">%08lu<", newDelay);
//             send_sync_message(syncMessage);
//             printf("Flashed at %lu, new relative delay set to %lu ms, next flash at %lu\r\n",
//                    current, newDelay, myNextFlash);
//         }

//         // Process incoming UART data one byte at a time.
//         if (!usb_uart_USART_ReadIsBusy())
//         {
//             sprintf(out_string, "%c", read_chars[0]);
//             // Read the next byte.
//             usb_uart_USART_Read((uint8_t *)&read_chars, 1);
//             // If start-of-message marker is detected, reset the receive buffer.
//             if (read_chars[0] == '>')
//                 rcvIndex = 0;
//             // Append the received character to the buffer.
//             if (rcvIndex < (sizeof(rcvBuffer) - 1))
//                 rcvBuffer[rcvIndex++] = read_chars[0];
//             // If end-of-message marker is detected, process the complete message.
//             if (read_chars[0] == '<')
//             {
//                 rcvBuffer[rcvIndex] = '\0';
//                 processIncomingMessage(rcvBuffer);
//                 rcvIndex = 0;
//             }
//         }
//     }
// }