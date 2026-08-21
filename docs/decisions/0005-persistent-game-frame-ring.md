# Decision 0005: prepare a persistent game-to-Activity external-image ring

Status: service allocation, Activity import/copy/present, and game-facing
producer implementation complete in source and host contracts; tablet runtime
proof pending.

## Context

Decision 0004 exposed `VK_KHR_swapchain` as a bridge-owned extension but kept
`vkCreateSwapchainKHR` unavailable until a game frame had an honest path to the
installed Activity. E042 proved the persistent external-image/futex primitive;
E057 implemented the Activity-side import/copy/present consumer. E060 closes
the host-side producer path without claiming tablet-visible output.

## Decision

- Allocate real optimal-tiling images on the game-facing native Vulkan device
  with dedicated opaque-FD-exportable memory. They have stable registered
  `BVB_OBJECT_IMAGE` IDs; no dummy swapchain or image handles are allowed.
- Add transfer-source usage internally so the Activity can copy or blit each
  completed image without changing the game's requested render usage.
- Export all image-memory FDs plus one 4 KiB control-region FD in one setup
  response. The response is available only with an authenticated Activity
  snapshot containing a live, resumed, renderer-ready window at the exact
  requested extent.
- Use a fixed-width cross-libc ring with per-slot
  `AVAILABLE -> ACQUIRED -> PRESENTED -> AVAILABLE` ownership and process-shared
  futex wakeups. No Java, Binder, socket, or FD transfer belongs in the frame
  loop.
- Require producer-local GPU completion before publishing `PRESENTED`, and
  Activity-local GPU completion after copy/present before releasing
  `AVAILABLE`. Shared CPU atomics never substitute for GPU synchronization.
- Refuse public swapchain creation unless the service was launched with the
  authenticated Activity frame socket. Treat host success as transport
  evidence only; do not claim visibility until a changing game frame is
  observed on the tablet.

## Implemented boundary

The service owns the native allocations, dedicated memory, stable wire IDs,
ring mapping, generation, descriptor bundle, and deterministic teardown. A
same-UID native socket gives the fixed 128-byte setup envelope and descriptors
once to an `app_process` helper; the authenticated callback returns them
through Binder to the Activity. Native Activity code validates the generation,
imports every opaque-FD image, intersects consumer-side memory-type bits, and
owns dedicated local copy/present synchronization.

The E057 consumer sleeps on the ring, acquires an Android swapchain image,
copies or blits the presented game image, submits and presents locally, waits
for its local fence, and only then releases the game slot. Lifecycle teardown
fails the ring and destroys every imported object. Java, Binder, socket calls,
and FD transfer remain setup-only.

E060 gives the game a real three-image virtual swapchain. Create returns the
service's stable image IDs and maps the shared control page; get-images uses
normal Vulkan enumeration semantics. Acquire uses opcode 100 to claim a slot
and submits the external-to-game ownership reacquire before signaling the
game's binary semaphore or fence. Present uses opcode 101, waits the game's
binary semaphores, submits the `PRESENT_SRC_KHR -> GENERAL` and
game-queue-to-`VK_QUEUE_FAMILY_EXTERNAL` release barrier, waits a private
producer fence, and only then publishes `PRESENTED`. Any uncertain GPU state
fails the ring.

Supported shapes deliberately remain narrow: three backing images, RGBA8 or
BGRA8 UNORM, one layer, exclusive sharing, opaque composite alpha, identity
transform, FIFO present, and the exact Activity extent. Other shapes, timeline
acquire/present semaphores, multiple swapchains per present, and foreign queues
fail closed.

## Launch wiring requirement

Start the installed Activity and wait for resumed/window/renderer-ready state.
Start `FrameTransportClient TOKEN RESULT_JSON SETUP_SOCKET` before the bridge
service, then pass the identical abstract socket name to the service as
`--activity-frame-socket SETUP_SOCKET`. The game-facing process must use that
service's filesystem socket through `BVB_BRIDGE_SOCKET`. The helper, Binder
callback, sockets, and FD transfer are setup-only; no Java/Binder call is
allowed in acquire or present.

## Next gate

Build and deploy the combined E057/E060 path with that launch wiring. A tablet
run must prove an animated game-facing producer reaches the Activity and
capture import/present logs plus visible evidence. Until then, the host
contract must not be described as visible Tomb Raider output.

## Provenance

The required exact E060 recall query found no indexed implementation. A
targeted recall recovered E035 opaque-FD allocation/import, E038 authenticated
Activity ingress, E041 producer-local GPU completion, E042 fixed-width shared
futex ownership, and E057 Activity-native copy/present/release. E060 reuses
those constraints and adds no fake image or swapchain handles.
