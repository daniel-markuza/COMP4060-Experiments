#ifndef LEACH_H
#define LEACH_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Configuration Constants (tweak as needed before compile)
// ---------------------------------------------------------------------------
#define NODE_DELAY_MS 3000UL      // slot length (ms)
#define ROUND_DURATION_MS 17000UL // how long a round lasts (ms)
#define CH_PROBABILITY 25         // base % chance to be CH
#define CH_AD_INTERVAL_MS 2000UL  // cluster‐head advert interval (ms)
#define CH_AD_TIMEOUT_MS 4000UL   // member gives up if no CH_AD (ms)
#define MIN_CH_INTERVAL_MS ROUND_DURATION_MS

#define MAX_MEMBERS 10
#define MAX_MSG_LEN 80

// Power‐mode register (S39) AT‐commands
#define CMD_SLEEP_MODE "ATS39=3\r" // Mode 3: proc & timers off, wake on UART
#define CMD_WAKE_MODE "ATS39=0\r"  // Mode 0: normal (radio & CPU on)

// Message framing
#define MSG_START '>'
#define MSG_END '<'

// ---------------------------------------------------------------------------
// Roles in our simplified LEACH‐style algorithm
// ---------------------------------------------------------------------------
typedef enum
{
    ROLE_IDLE,         // not in a round yet
    ROLE_CLUSTER_HEAD, // elected cluster head
    ROLE_JOINING,      // looking for a CH_AD to join
    ROLE_MEMBER        // joined—will send/forward data
} Role;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief  Entry point for our tiny LEACH‐style protocol.
     *
     * This will initialize hardware, fetch the device EUI64 (NODE_ID),
     * seed the RNG, and then loop forever running rounds of:
     *   1) election
     *   2) join (if not head)
     *   3) TDMA steady‐state (heads advert, members send & sleep)
     *   4) head broadcasts ROUND_COMPLETE
     *
     * @returns  never returns (infinite loop), but signature returns int for
     *          compatibility with typical `main`.
     */
    int leach_main(void);

#ifdef __cplusplus
}
#endif

#endif // LEACH_H