# ADR 0002: Use a controlled surface host; do not reinterpret the X connection

Date: 2026-08-18  
Status: accepted; controlled, visible, and immersive presentation passed in
E006-E009

## Context

Android's Vulkan loader implements `VK_KHR_surface`,
`VK_KHR_android_surface`, and swapchain interaction with `ANativeWindow`.
`ANativeWindow` is the producer side of an Android image queue and corresponds
to a Java `Surface`; an X11 connection or X window identifier is not an
`ANativeWindow`.

At official Termux:X11 commit
[`139f219`](https://github.com/termux/termux-x11/tree/139f2197e6093d04d5df1400baa998bf1fb07b3c),
`LorieView` owns a `SurfaceView`, passes its Java `Surface` directly to
in-process JNI, and converts it with `ANativeWindow_fromSurface`. Its Binder
interface exports X and log file descriptors, not the display `Surface`.
Termux:X11's renderer also owns an EGL window surface on that native window.

E004 found Android surface/swapchain plus AHardwareBuffer and external-FD
memory/synchronization support on the tablet, but no
`VK_EXT_headless_surface`.

## Decision

E006 creates a controlled non-visible `ANativeWindow` and queries an Android
Vulkan surface without modifying Termux:X11. E007 creates its swapchain,
presents an opaque-magenta frame, and verifies all 4,096 consumer pixels.
E008 packages a standalone `NativeActivity` and visibly presents the same
magenta frame on its dedicated Android window. E009 hides Android's system bars
with immersive-sticky View flags and restores them after focus changes without
altering the renderer. E010 passes explicit lifecycle/status transfer: the
Activity authenticates fixed-width events to an opt-in loopback ingress with a
fresh 256-bit launch capability, and the glibc client queries the derived state
through the existing owner-authenticated Unix socket. It does not reinterpret
an X socket/window or concurrently take over Termux:X11's existing EGL-owned
surface. The next gate is game-facing Vulkan dispatch.

The service remains Bionic and the game-facing client remains glibc. Termux:X11
continues to own Steam/X11 UI until a dedicated game surface is active.

## Consequences

- Surface and swapchain lifecycle are now explicit protocol state.
- A dedicated view can be switched full-screen for a game without discarding
  the working Steam UI or login state.
- Input routing and view switching become separate later gates.
- AHardwareBuffer and external FD support provide credible future sharing
  options, but no zero-copy claim is made until an import/export probe passes.

## Rejected assumptions

- An X11 connection is not an Android native window.
- Headless-surface code is not usable on this driver because the extension is
  absent.
- The existing LorieView surface cannot safely have simultaneous EGL and
  Vulkan producers.
- Capability presence alone does not prove swapchain creation or presentation.

## Sources and provenance

- [AOSP Vulkan WSI](https://source.android.com/docs/core/graphics/implement-vulkan)
- [Android NDK Native Window](https://developer.android.com/ndk/reference/group/a-native-window)
- [Khronos queue rules](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html)
- [Termux:X11 pinned source](https://github.com/termux/termux-x11/tree/139f2197e6093d04d5df1400baa998bf1fb07b3c)

The required `deja` query found no earlier Android-surface bridge
implementation to reuse. This decision uses the cited primary sources and the
measured E004/E005 results.
