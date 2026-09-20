# Distributed offensive-security fabric on Linux — planning notes

**Status: discussed, not started.** No code for this exists yet, on this branch or
anywhere else. This file exists so a future session (or a different person) can pick
the discussion up without re-deriving it. It is a planning record, not a spec that's
been committed to — every open question below is still open.

## How this branch relates to `working`

This branch forked from `working` at the exact commit where Vajra's own
"usable for real work" track was paused (see that branch's `CLAUDE.md`, the
"PAUSED HERE" note). It therefore contains the **entire Vajra source tree**,
unchanged. That's deliberate, not an oversight: Vajra is not being deleted or
abandoned, this branch is just where the *next, unrelated* piece of work starts
from. Two live possibilities for how the projects relate, still undecided:

1. This becomes a genuinely separate project and eventually moves to its own
   repository, with this branch as its origin point.
2. Vajra (or a stripped-down "Range" build of it — see below) becomes one of the
   *targets* this framework is built to attack, in which case keeping both in one
   repo turns out to be the right call after all.

Don't resolve that now — it'll be obvious once the framework exists.

## How we got here

The starting request was to evolve Vajra itself into an offensive-security /
distributed-AI operating system (actors, capabilities, a compute fabric, agents,
an offline LLM, a cyber range — see the full spec pasted into that conversation
for the complete list of 47 sections). Discussion on that surfaced a real design:
split Vajra into a hardened **Core** (holds anything worth protecting) and a
capability-free, disposable **Range** (runs the offensive tooling itself, nothing
persistent, nothing to steal), joined only by the existing device boundary
Vajra's fabric already has.

The user then decided not to build this as Vajra at all: build the same idea —
distributed actors coordinating, attacking authorized systems, backed by an
offline LLM — as a framework on stock Linux instead of a custom kernel. That's
what this branch is for. The Core/Range framing above should carry over even
though the kernel underneath is now gone: nothing about "disposable, capability-
free execution vs. a place that holds findings and decides trust" depends on
having written the kernel yourself.

## The actual proposal, as discussed (not yet decided)

**Don't build a custom Linux distro/kernel/installer.** That's a large amount of
packaging work with no functional payoff over running the framework on any
existing Linux (bare metal, a VM, or a container host). If a bootable branded
appliance is wanted later, that's a packaging step applied *after* the framework
works, not a prerequisite.

**Build the framework**, on top of whatever Linux is already there:

- **Actors**: one process (or container) per role — recon, a specific exploit or
  fuzzer, a protocol probe, an analysis worker. Not one script that does
  everything.
- **Coordination / message bus**: NATS JetStream was the suggested default
  (single binary, both pub/sub and durable work-queue delivery fit "dispatch a
  job, collect results" directly). Redis Streams and ZeroMQ were named as
  alternatives, not committed to.
- **Isolation / blast radius**: since there's no kernel enforcing capabilities
  here, isolation has to come from the platform — one container per actor,
  minimal image, no persistent volume, explicit egress firewall rules standing in
  for "target scope." A compromised or crashed actor is killed and rebuilt from
  its image, the same ephemeral idea as Range.
- **Offline LLM**: a separate node/container running Ollama (simplest to start),
  llama.cpp (most portable, no GPU needed), or vLLM (best throughput with a real
  GPU) — undecided, and doesn't need deciding yet. The LLM only ever sees
  structured tool calls and structured results, never raw execution — same rule
  as in the Vajra discussion, and it doesn't depend on Vajra at all.
- **Findings / knowledge store**: a small structured store (Postgres to start) of
  targets, findings and events, so agents stop rediscovering the same thing. A
  graph layer (e.g. Neo4j) only if path-style queries turn out to be needed.
- **Scope enforcement**: every job carries an explicit, checked list of
  authorized targets. This is a hard requirement, not a nice-to-have — offensive
  tooling only stays legitimate to build if targeting is scoped by construction,
  not by hoping the operator remembers.

## The first slice that was proposed (not started)

1. One coordinator process, a handful of worker containers.
2. One fixed, authorized local target (a deliberately vulnerable lab VM, or a
   future Vajra Range instance).
3. Workers do simple recon against that one target, report findings over the bus.
4. The LLM reads the aggregated findings and produces an advisory summary only —
   no autonomous action yet.
5. A small CLI or dashboard report, so there's something to look at, not just log
   lines.

## Open questions — nothing below is decided

- **Target for the first demo**: the user's own lab VM, or a Vajra Range instance?
- **Language**: Python throughout for the first slice (fastest iteration, huge
  security-tooling ecosystem — scapy, impacket, requests), with a faster language
  for the coordinator only if something is actually shown to be slow?
- **Message bus**: NATS, or a real preference otherwise?
- **LLM runtime**: Ollama to start, or is there GPU hardware that makes vLLM worth
  setting up immediately?
- **Isolation strength**: plain Docker containers per actor, or microVMs
  (Firecracker) from day one?

## Standing rules carried over from Vajra (still apply, OS-agnostic)

- Deterministic tests first; LLM is advisory before it's ever autonomous.
- Every job scoped to authorized targets only, checked, not assumed.
- Findings become regression tests, not one-off logs.
- Every backend feature gets a front end a person can actually look at.
- Verify claims by triggering the failure/success for real, not by review alone.
