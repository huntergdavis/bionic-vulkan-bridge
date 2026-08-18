# ADR 0002: Use a controlled surface host; do not reinterpret the X connection

Date: 2026-08-18  
Status: accepted; first controlled-surface gate passed in E006

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
Vulkan surface without modifying Termux:X11. Visible output will next use a
dedicated Android `SurfaceView` host in the shared-UID app stack. It must have
explicit ownership and lifecycle transfer to the Bionic Vulkan service; it
will not reinterpret an X socket/window or concurrently take over
Termux:X11's existing EGL-owned surface.

The service remains Bionic and the game-facing client remains glibc. Termux:X11
continues to own Steam/X11 UI until a dedicated game surface is active.

## Consequences

- Surface and swapchain lifecycle become explicit protocol state.
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
