# Vajra Architecture

## Vision

Vajra is an operating system built around isolated computational
actors communicating through controlled message passing.

## High-Level Architecture

Applications
    ↓
Actor Runtime / Fabric
    ↓
Vajra Microkernel
    ↓
Hardware

## Core Concepts

### Actors

Applications and system components execute as isolated actors.

### Messages

Actors communicate through messages rather than unrestricted
shared memory.

### Capabilities

Access to resources is granted through explicit capabilities
validated by the kernel.

### Ghost Actors

Temporary computational actors may be created for narrowly
scoped work and destroyed or recycled after completion.

### Adaptive Scheduling

The scheduler will eventually use workload telemetry to
optimize placement and execution of work.

### Distributed Computation

Actors may eventually execute across multiple CPUs, GPUs,
NPUs, or machines while maintaining capability restrictions.

## Initial Architecture

Target:
    x86-64

Boot:
    BIOS/UEFI-compatible boot path

Kernel:
    Minimal experimental microkernel

Development:
    Windows 11 + QEMU

## Current Stage

M1 — Project Foundation