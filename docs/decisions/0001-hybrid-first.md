# ADR 0001: Keep the game stack and isolate the Bionic graphics boundary

Date: 2026-08-18  
Status: accepted for experimentation

## Decision

Keep Steam, Proton, Wine, and FEX in their current glibc/Linux environment.
Build the Android Vulkan-facing component as a native Bionic process, with a
versioned boundary between them.

## Rationale

The current stack launches and benchmarks Tomb Raider 2013, giving us a known
control. Android's Vulkan loader and Adreno HAL use Bionic and Android platform
interfaces. A small explicit boundary lets each side use its native ABI and can
be measured independently.

## Alternatives deferred

- Replacing the entire Linux game stack with Android-native components expands
  scope before the graphics hypothesis is proven.
- Loading the Adreno HAL directly into a glibc process crosses unsupported ABI
  and platform assumptions.
- One synchronous RPC per Vulkan call is unlikely to meet game hot-path latency
  requirements; batching/shared memory will be evaluated after basic gates.

## Consequences

This approach retains Termux and the glibc compatibility layer initially. It
also makes process isolation, handle translation, memory sharing, and
synchronization explicit engineering work rather than hidden assumptions.

