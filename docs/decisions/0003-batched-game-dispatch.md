# Decision 0003: generated proxy dispatch with shared command batches

Status: accepted for the first game-facing implementation.

## Context

E011 observed 742 distinct Vulkan or Wine-private names during repeatable
Tomb Raider startup. Of the registry commands, 635 are device scoped and 101
are instance scoped. Hand-writing that boundary is neither repeatable nor
reviewable, while one synchronous socket exchange per Vulkan call would put
IPC in the render hot path.

Khronos documents that applications obtain device-specific entry points from
`vkGetDeviceProcAddr` for the shortest dispatch chain. Vulkan dispatchable
handles carry the dispatch-table identity used for that routing. Khronos also
defines `VK_ANDROID_external_memory_android_hardware_buffer` for importing and
exporting Android hardware buffers as Vulkan memory. Those contracts match the
two halves of this bridge: generated local dispatch on the glibc side and
shared GPU-visible storage owned by the Bionic side.

## Decision

- Generate command scope, signatures, aliases, and handle relationships from
  the pinned Khronos `vk.xml`; never maintain a parallel handwritten registry.
- Give the glibc client stable proxy handles. The Bionic service exclusively
  owns real Android Vulkan handles, and wire IDs are fixed-width integers—not
  pointers copied across the process boundary.
- Keep global/instance queries and resource lifecycle operations on a
  versioned control path. Cache returned capabilities and generated function
  pointers.
- Record command-buffer operations into bounded shared-memory batches and
  replay them in the Bionic process. A queue submission may synchronize a
  batch; individual draw, bind, barrier, and state commands must not perform a
  synchronous socket round trip.
- Prefer Android hardware buffers and external synchronization primitives for
  mapped or GPU-visible resources. Pass operating-system handles on the
  authenticated Unix transport; do not copy frame-sized payloads through the
  control packet format.
- Keep Android WSI ownership in the visible host. A Linux/X11 surface request
  is translated into the authenticated host surface rather than forwarding an
  X11 pointer to Bionic.
- Generate only the first triangle subset as executable dispatch initially,
  but reject an unimplemented resolved command deterministically. Expand from
  measured DXVK requirements, not from guesses.

## Performance gates

The bridge is not accepted on correctness alone. Each stage records warm
control latency, shared-memory throughput, batch size, replay time, and peak
RSS. The triangle and DXVK gates additionally record frame pacing. A design
that requires synchronous IPC for each device command is rejected even if it
renders correctly.

## References

- [Khronos Vulkan Loader application interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderApplicationInterface.md)
- [Khronos Vulkan Loader architecture](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md)
- [VK_ANDROID_external_memory_android_hardware_buffer](https://docs.vulkan.org/refpages/latest/refpages/source/VK_ANDROID_external_memory_android_hardware_buffer.html)
