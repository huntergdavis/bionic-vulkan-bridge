# Decision 0004: expose swapchain as an Activity-backed virtual extension

Status: first control-plane slice implemented; frame transport pending.

## Context

The private Turnip ICD exposes `VK_ANDROID_native_buffer`, not desktop
`VK_KHR_swapchain`. The visible-host Activity already owns a real Android
`ANativeWindow`, swapchain, and full-screen presentation loop. Passing an Xlib,
XCB, or Wayland pointer to the raw Android driver would therefore be invalid,
and a second producer cannot take ownership of the Activity window.

ADR 0002 established that desktop surfaces are virtual handles backed by the
authenticated Activity. E042 established a persistent external-image and
shared-atomic frame-ring transport, but in the producer/consumer direction
used by that experiment rather than the game-to-Activity presentation
direction required here.

## Decision

- Expose `VK_KHR_swapchain` to the glibc application only when the bridge
  service has authenticated Activity ingress.
- Treat that name as a bridge-owned virtual device extension. Remove it from
  the native extension list passed to raw Turnip during `vkCreateDevice`.
- Remember whether each device enabled virtual swapchain and resolve the core
  swapchain/device-group entry points only for such a device.
- Validate `vkCreateSwapchainKHR` against the virtual surface's parent instance,
  the live Activity renderer, and the Activity's current extent.
- Return `VK_ERROR_FEATURE_NOT_PRESENT` until the game-to-Activity external
  image transport is installed. Do not allocate dummy images or return a
  successful swapchain whose pixels cannot reach the display.

This slice deliberately advances Wine/DXVK through extension discovery,
native device creation, and WSI function resolution while preserving an exact
failure boundary at the missing frame transport.

## Next gate

Reverse E042's persistent image-ring direction: create a bounded set of
exportable game render images in the Bionic service, import them once into the
Activity renderer, and keep descriptor/Binder transfer outside the frame
loop. `vkAcquireNextImageKHR` will claim a shared slot and `vkQueuePresentKHR`
will publish it with shared atomics/futex plus local GPU synchronization. Only
after that hardware path presents a deterministic changing frame may
`vkCreateSwapchainKHR` return success.

## Provenance

The required `deja` query—`bionic-vulkan-bridge Android WSI
VK_ANDROID_native_buffer VK_KHR_swapchain surface presentation`—returned no
indexed implementation to reuse. This decision reuses ADR 0002's controlled
surface ownership, ADR 0003's virtual WSI rule, E023's persistent slot
discipline, and E042's external-image frame-ring evidence.
