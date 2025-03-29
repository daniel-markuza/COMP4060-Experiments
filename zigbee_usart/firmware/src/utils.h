#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Constants
#define MS_TICKS 48000UL
#define LED_FLASH_MS 1000UL
#define SLEEP_INTERVAL_MS 60000
#define COMMAND_TIMEOUT_MS 5000
#define FIXED_MESSAGE_LENGTH 19
#define BYTE_TIMEOUT_MS 50    // Timeout for each byte read
#define TOTAL_TIMEOUT_MS 1000 // Timeout for the entire read operation

// Common Commands
//  Declare but do not define global variables
extern uint8_t get_id[];
extern uint8_t restart[];
extern uint8_t join_own_network[];
extern uint8_t join_existing_network[];
extern uint8_t disassociate[];
extern uint8_t change_channel[];
extern uint8_t network_info[];

extern uint8_t set_power_mode_3[];     // Set power mode to 3 (sleep)
extern uint8_t enter_command_mode[];   // Command to wake up
extern uint8_t check_voltage[];        // Check battery voltage
extern uint8_t valid_command_rdatab[]; // 11 hex -> 19 decimal bytes: 10 for timestamp, 4 for voltage, 3 for formatting

// Function declarations
void delayMs(uint32_t milliseconds);
size_t readAllBytesWithTimeout(uint8_t *buffer, size_t maxBufferSize);
bool sendCommandAndReadResponse(uint8_t *command, const char *description, uint8_t *readBuffer, size_t bufferSize);
void handleLEDBlink();
void systemInitialize();
void SysTick_Handler(); // Declare SysTick handler
void doInputOutput();
uint32_t getMsCount();

#endif // UTILS_H