#include "vajra/hal.h"
#include "vajra/net.h"

/* ------------------------------------------------------------------
 * Roadmap Phase 12, the piece after Milestone 13's raw driver: a
 * minimal, from-scratch wire protocol proving an actor-level message
 * can genuinely cross a device boundary, not just that a NIC can send
 * and receive bytes.
 *
 * Carried directly in Ethernet frames under EtherType 0x88B5 -- one of
 * the two values IEEE reserves for "local experimental" use, exactly
 * this kind of from-scratch protocol, and safely distinguishable from
 * real traffic (ARP is 0x0806, IPv4 is 0x0800) sharing the same wire.
 * Frame shape: the ordinary 14-byte Ethernet header (destination
 * always broadcast -- there's no addressing scheme yet, only "every
 * device on this link"), followed by a 16-byte payload: an 8-byte
 * type, then an 8-byte data word, matching the two fields of core's
 * own struct message that actually carry application meaning
 * (sender is deliberately not included -- see this file's own header
 * comment on why remote sender identity isn't meaningful yet).
 *
 * This file is written to stay portable (only calls hal_net_*
 * functions), even though "Ethernet framing" is itself an x86/NIC-
 * driver detail today: it's also the near-universal shape every real
 * OS's networking stack normalizes other link layers (Wi-Fi, cellular)
 * into at the driver boundary, so treating it as core-level rather
 * than x86-specific is a deliberate bet, not an oversight -- a future
 * hal/aarch64/ Wi-Fi driver would be expected to present frames the
 * same way, translating its own radio protocol into this shape itself,
 * the same way a real Wi-Fi driver does on Linux.
 * ---------------------------------------------------------------- */

#define VAJRA_ETHERTYPE_HI 0x88
#define VAJRA_ETHERTYPE_LO 0xB5
#define ETH_HEADER_LEN     14
#define PAYLOAD_LEN        16
#define FRAME_LEN          (ETH_HEADER_LEN + PAYLOAD_LEN)

static uint8_t my_mac[6];
static int net_online = 0;

int net_init(void) {
    if (hal_net_init() != 0) {
        return -1;
    }
    hal_net_get_mac(my_mac);
    net_online = 1;
    return 0;
}

int net_send_message(uint64_t type, uint64_t data) {
    if (!net_online) {
        return -1;
    }

    uint8_t frame[FRAME_LEN];
    for (int i = 0; i < 6; i++) {
        frame[i] = 0xFF; /* broadcast -- no addressing scheme yet, see top comment */
    }
    for (int i = 0; i < 6; i++) {
        frame[6 + i] = my_mac[i];
    }
    frame[12] = VAJRA_ETHERTYPE_HI;
    frame[13] = VAJRA_ETHERTYPE_LO;

    uint8_t *p = frame + ETH_HEADER_LEN;
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(type >> (8 * i));
    }
    for (int i = 0; i < 8; i++) {
        p[8 + i] = (uint8_t)(data >> (8 * i));
    }

    return hal_net_send(frame, sizeof(frame));
}

int net_poll_receive_message(uint64_t *type, uint64_t *data, uint32_t max_spins) {
    if (!net_online) {
        return -1;
    }

    uint8_t frame[64];
    int n = hal_net_poll_receive(frame, sizeof(frame), max_spins);
    if (n <= 0) {
        return 0; /* timed out, or the arrived frame didn't fit -- either way, no message */
    }
    if (n < FRAME_LEN) {
        return 0; /* too short to be one of ours */
    }
    if (frame[12] != VAJRA_ETHERTYPE_HI || frame[13] != VAJRA_ETHERTYPE_LO) {
        return 0; /* real traffic on the same wire (e.g. ARP) -- not this protocol, ignore it */
    }

    uint64_t t = 0, d = 0;
    const uint8_t *p = frame + ETH_HEADER_LEN;
    for (int i = 0; i < 8; i++) {
        t |= ((uint64_t)p[i]) << (8 * i);
    }
    for (int i = 0; i < 8; i++) {
        d |= ((uint64_t)p[8 + i]) << (8 * i);
    }

    *type = t;
    *data = d;
    return 1;
}
