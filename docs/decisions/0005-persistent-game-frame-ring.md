# Decision 0005: prepare a persistent game-to-Activity external-image ring

Status: service-side allocation and transport contract implemented; Activity
import and copy/present consumer pending.

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

The service now owns the native image allocations, dedicated memory, stable
wire image IDs, ring mapping, generation, descriptor bundle, and deterministic
teardown. Host contracts exercise a 4,096-frame three-slot ring, native Vulkan
export calls through the pinned test loader, SCM_RIGHTS descriptor delivery,
authenticated Activity gating, and response decoding.

## Next gate

Extend the existing allocation-time Binder callback so the Activity imports
the image FDs and control FD once, then run a native Activity consumer thread:
wait for the next presented sequence, acquire an Android swapchain image,
copy/blit the imported image, submit/present with local Vulkan synchronization,
and release the slot. Only that visually verified path may make the public
swapchain calls return success.

## Provenance

The required recall query—`BVB E042 persistent external image ring virtual
swapchain VK_KHR_swapchain acquire present Activity ingress futex`—returned no
indexed prior session. This implementation reuses the repository's E035
opaque-FD allocation/import rules, E038 authenticated Activity image broker,
E041 allocation-time producer completion, E042 fixed-width shared futex
control, and decision 0004's exact no-fake-success boundary.
