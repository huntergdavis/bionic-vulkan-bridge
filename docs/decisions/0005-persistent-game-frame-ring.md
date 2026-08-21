# Decision 0005: prepare a persistent game-to-Activity external-image ring

Status: service allocation plus Activity import/copy/present implementation
complete in source and host contracts; tablet runtime proof pending.

## Context

Decision 0004 exposes `VK_KHR_swapchain` as a bridge-owned extension but keeps
`vkCreateSwapchainKHR` at `VK_ERROR_FEATURE_NOT_PRESENT` because no game frame
can reach the installed Activity. E042 already proved a persistent external
image plus shared futex control page, but its Activity-to-Termux direction and
single serialized image are the reverse of game presentation.

## Decision

- Allocate two to four optimal-tiling images on the game-facing native Vulkan
  device with dedicated opaque-FD-exportable memory. These are real registered
  `BVB_OBJECT_IMAGE` handles, not dummy swapchain handles.
- Add transfer-source usage internally so the Activity can copy or blit each
  completed image into its Android swapchain without changing the game's
  requested render usage.
- Export all image-memory FDs plus one 4 KiB control-region FD in one setup
  response. The response is available only after the service has an
  authenticated Activity snapshot with a live window, resumed lifecycle, and
  renderer-ready state at the requested extent.
- Use a fixed-width cross-libc ring with per-slot
  `AVAILABLE -> ACQUIRED -> PRESENTED -> AVAILABLE` ownership. Producer and
  consumer sequences are release/acquire atomics with process-shared futex
  wakeups. No Java, Binder, socket, or FD transfer belongs in the frame loop.
- Require producer-local GPU completion before publishing `PRESENTED`, and
  Activity-local GPU completion after copy/present before releasing
  `AVAILABLE`. Shared CPU atomics never substitute for GPU synchronization.
- Keep `vkCreateSwapchainKHR` unavailable until the installed Activity imports
  every setup FD once and demonstrates a changing game-to-window frame. A
  successful but invisible swapchain remains forbidden.

## Implemented boundary

The service owns the native image allocations, dedicated memory, stable wire
image IDs, ring mapping, generation, descriptor bundle, and deterministic
teardown. The allocation bundle now has a fixed 128-byte setup envelope. A
same-UID native socket gives it once to an `app_process` helper; the existing
authenticated callback returns the image/control FDs through Binder to the
Activity. Native Activity code validates the generation, maps the ring,
recreates and imports all opaque-FD images, intersects each FD's consumer-side
memory-type bits instead of assuming private-Turnip indices match the Android
loader, and owns a dedicated command pool, semaphores, and fence.

The consumer sleeps on the native ring, acquires an Android swapchain image,
copies or blits the presented game image, submits and presents locally, waits
for its local copy fence, and only then releases the game slot. Pause stops new
claims; resume wakes the consumer; window/device teardown fails the ring and
destroys every imported object. Java, Binder, socket calls, and FD transfer
remain setup-only.

## Next gate

Connect the game-facing virtual `vkCreateSwapchainKHR`, acquire, and present
calls to this prepared transport. Producer present must complete its local GPU
work, transition/release the image to `VK_IMAGE_LAYOUT_GENERAL` and
`VK_QUEUE_FAMILY_EXTERNAL`, then publish `PRESENTED`. A tablet run must prove a
deterministic changing frame reaches the Activity before the public swapchain
is allowed to return success.

## Provenance

The required recall query—`E042`—found the earlier four-slot shared-ring work
and its explicit acquire/release ordering. This implementation reuses E035's
opaque-FD allocation/import rules, E038's authenticated Activity broker,
E041's allocation-time producer completion, E042's fixed-width shared futex
control, and decision 0004's exact no-fake-success boundary.
