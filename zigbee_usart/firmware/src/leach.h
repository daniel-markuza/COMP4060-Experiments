#ifndef LEACH_H
#define LEACH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    ROLE_IDLE,
    ROLE_JOINING,
    ROLE_MEMBER,
    ROLE_CLUSTER_HEAD
} LEACH_Role;

// Configuration parameters
#define ROUND_DURATION_MS 30000UL     // Each round lasts 30 seconds
#define ROUND_START_TIMEOUT_MS 5000UL // Backup: 5 seconds waiting for "ROUND_START"
#define CH_PROBABILITY 0.3f           // 30% chance to become a cluster head

// AT Command strings for network configuration
#define AT_RESET "ATZ\r"
#define AT_GET_ID "ATI\r"
#define AT_SET_CHANNEL "AT+CCHANGE:1A\r" // Example: channel 26 (1A hex)
#define AT_JOIN_NETWORK "AT+JN\r"        // Join PAN command
#define AT_IDENT "AT+IDENT\r"

// AT+RDATAB prefix ? used for sending a broadcast message using binary data mode.
#define AT_RDATAB_PREFIX "AT+RDATAB:"

// Public functions
void leach_init(void);
void leach_main_loop(void);

#endif /* LEACH_H */