# Vajra C Rewrite — Milestone 15: Addressing, Remote Identity, Reliability

## What this is

Roadmap Phase 12's remaining scope, closed out. Milestone 14 proved an
actor could cross a device boundary at all, deliberately minimal: every
message broadcast to the whole link, no way to tell which remote actor
(or even which remote device) a reply came from, and no delivery
guarantee beyond a single attempt. This milestone closes all three,
without becoming an IP/UDP/TCP stack — see `core/net.c`'s own top
comment for why that's still a deliberate boundary, not an oversight.

## What's included

- **Addressing** — `net_send_message_to()` / `SYS_NET_SEND_TO` unicasts
  a `{type, data}` message to one device's MAC instead of broadcasting.
  `actor_network_peer()` uses it to reply to a HELLO directly, instead
  of re-broadcasting the ACK to the whole link.
- **Remote actor identity** — the wire payload now carries the
  sender's own local actor slot, kernel-stamped from a new
  `actor_current_slot()` (`core/actor.c`) at send time, never
  actor-supplied — the same "sender can't lie about who it is" rule
  `struct message`'s own `sender` field already follows locally.
  Combined with the Ethernet header's own source MAC (always present,
  just not surfaced to callers before now), a received message's
  origin is "actor N on device MAC" — real identity, not just "some
  peer." `struct net_message` (`include/vajra/hal.h`) grew
  `sender_actor` and `sender_mac` fields to carry this back through
  `SYS_NET_RECEIVE`.
- **Reliability** — `net_send_message_reliable_to()` /
  `SYS_NET_SEND_RELIABLE` retransmits the same `{seq, type, data}`
  until a matching ACK comes back from that exact device, or gives up
  after a bounded number of attempts. The receiving side never opts
  in: `net_poll_receive_message()` auto-ACKs every DATA frame it
  successfully decodes, transparently, before ever handing it to the
  caller — the same way a real link layer's ACK isn't an application
  decision. Deliberately not a sliding window, not ordering, not
  multiple messages in flight at once — one outstanding reliable send
  at a time, matching this milestone's own minimal scope.
- **`actor_network_peer()` (`core/main.c`)** — extended to exercise
  reliability for real: once two instances find each other via
  HELLO/HELLO_ACK, the side that heard the ACK sends one PING through
  the reliable path and reports whether it was genuinely acked. The
  other instance's ordinary receive loop ACKs it with no special
  handling at all — proving the transparency claim above, not just
  asserting it.
- **`.github/workflows/network-test.yml`** — a second verify step
  greps both peers' logs for the reliable-delivery confirmation
  string, alongside Milestone 14's existing HELLO/HELLO_ACK check.

## Real bugs found

Three, each caught only once this was actually run across two
genuinely separate QEMU instances on CI — none of them showed up in a
single-instance boot, and none were caught by a clean compile:

1. **A ring-3 page fault reading ordinary `.rodata`.** The first
   version of the MAC-printing helper (`user_write_mac()`, `core/main.c`)
   indexed a `const char *digits = "0123456789ABCDEF"` lookup table.
   That string literal lives in ordinary kernel `.rodata`, which is
   supervisor-only everywhere except `.user_text` itself
   (`hal/x86_64/paging.c`'s own comment: taking such an address is
   fine, *dereferencing* it from ring 3 is not). Compiled clean,
   linked clean, and only genuinely page-faulted (`#PF`,
   present+user-mode read) the first time two real instances actually
   exchanged a HELLO on CI — a single-instance sanity boot never took
   this code path at all, since it never hears from a peer. Fixed by
   doing hex-digit conversion by arithmetic, matching
   `user_write_dec64()`'s own existing pattern, instead of a table
   lookup.
2. **A real race: the replier exits before the pinger ever sends.**
   The side that only ever *replies* to a HELLO (never hears its own
   HELLO_ACK back) ran its full handshake-loop budget and then exited
   immediately. The confirmed side sometimes took nearly that same
   budget just to get its *own* ack before even starting the reliable
   PING — confirmed by real CI logs showing the replier's "no peer
   heard" line and exit already in the log before the pinger's
   "sending one PING" line ever appeared. There is no way to know in
   advance which side will end up needing to stay reachable for the
   other's follow-up reliable send, so both roles now run a shared
   drain phase after their primary phase, discarding whatever arrives
   (only the transparent auto-ACK inside it matters) before finally
   exiting.
3. **`PAYLOAD_LEN` undersized by 8 bytes, truncating `seq`/`is_ack`
   off the wire.** Adding `seq` (8 bytes) and `is_ack` (1 byte) to the
   existing 24-byte payload should have made `PAYLOAD_LEN` 33; it was
   written as 25 — an arithmetic slip, not a typo caught by the
   compiler, since it's just a numeric literal. This undersized
   `frame[FRAME_LEN]`'s declared array size, so the out-of-bounds
   writes past it (still executed, still writing `seq`'s upper bytes
   and `is_ack` — to ordinary stack slack, not a fault) never actually
   made it into what `hal_net_send(frame, sizeof(frame))` transmitted:
   `sizeof(frame)` only covers the declared 39-byte array. The
   receiver's own length check (`n < FRAME_LEN`) used the same wrong
   constant, so a truncated frame still passed it, and `is_ack` was
   reliably decoded as false from bytes the NIC never delivered for
   that frame at all. This masked itself almost completely: the base
   HELLO/HELLO_ACK exchange only ever touches the payload's first 24
   bytes, so it kept passing CI throughout. Only the new reliability
   primitive actually exercised the corrupted tail, and did so
   consistently — real CI logs confirmed the listening side was still
   present and responsive the whole time, ruling out a timing problem
   before this one was found. Fixed by correcting the constant; no
   other change was needed once it was.

## Known follow-ups for the next milestone

- **Still no way to name a specific remote ACTOR**, only a device — a
  reply means "some actor on device Y," not an address the fabric can
  route to directly. This is Phase 13's remote capability delegation
  story, not this milestone's.
- **No authenticated channels** — anything on the same link can claim
  any device's traffic; cryptographic identity between Vajra instances
  remains a deliberately deferred point, not an oversight.
- **One outstanding reliable send at a time, no ordering, no
  multiplexed streams** — see `core/net.c`'s own top comment. Real
  transport semantics beyond single-message ACK+retry are explicitly
  still ahead.
