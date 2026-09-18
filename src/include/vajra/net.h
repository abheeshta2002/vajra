#ifndef VAJRA_NET_H
#define VAJRA_NET_H

#include <stdint.h>

/* ------------------------------------------------------------------
 * Portable core networking: a minimal, from-scratch protocol carried
 * directly in Ethernet frames (a custom EtherType, not ARP/IPv4 --
 * see core/net.c's own comment), used to prove actor-level messages
 * can genuinely cross a device boundary. Deliberately NOT an IP/UDP/TCP
 * stack: this is link-local only (same broadcast domain -- e.g. two
 * QEMU instances joined by `-netdev socket`), with no routing between
 * different networks. That's real, explicitly deferred work -- see
 * docs/ROADMAP.md's Phase 12 entry.
 *
 * Carries remote identity now (device MAC + the sending actor's own
 * local slot, kernel-stamped, never actor-supplied), and a target MAC
 * for a unicast reply -- but still no way to name a specific remote
 * ACTOR, only a device. Cross-device actor addressing is Phase 13's
 * remote capability delegation story, not this one's.
 * ---------------------------------------------------------------- */

/* Initializes the underlying HAL network driver. Returns 0 on success,
 * -1 if no network device is present (an honest, non-fatal outcome --
 * see hal_net_init()'s own comment). */
int net_init(void);

/* Broadcasts one {type, data} message to every device on the local
 * link. Returns 0 on success, -1 if networking isn't online. */
int net_send_message(uint64_t type, uint64_t data);

/* Sends one {type, data} message to exactly one device (its MAC),
 * instead of broadcasting -- the addressing scheme. Returns 0 on
 * success, -1 if networking isn't online. */
int net_send_message_to(const uint8_t dest_mac[6], uint64_t type, uint64_t data);

/* Polls (bounded, like hal_net_poll_receive()) for the next inbound
 * Vajra-protocol message, ignoring any other traffic on the wire
 * (ARP, or anything not carrying this protocol's EtherType). On
 * success (returns 1), fills type/data, *sender_actor (the remote
 * actor's own local slot index -- meaningful only on that remote
 * device, not a fabric-wide id), and sender_mac (that device's own
 * source MAC, straight from the Ethernet header). Returns 0 if
 * nothing arrived within max_spins, -1 if networking isn't online. */
int net_poll_receive_message(uint64_t *type, uint64_t *data, uint64_t *sender_actor,
                              uint8_t sender_mac[6], uint32_t max_spins);

#endif
