# Standalone global-dispatch Activity harness

`scripts/run-global-dispatch-activity-harness.py` lets the Termux glibc global
gate exercise the current virtual surface and swapchain path without launching
the Android Activity. It starts the real Bionic bridge service with a unique
filesystem control socket, random 256-bit lifecycle token, dynamic loopback
Activity port, and unique abstract frame socket. It then sends the authenticated
`CREATED`, `STARTED`, `RESUMED`, `WINDOW_CREATED`, `RENDERER_READY`, and
`FOCUS_GAINED` records used by the real Activity.

The harness reuses the E010 lifecycle record/ack protocol and the E057/E060
one-time Activity frame setup envelope. A same-UID native listener validates the
128-byte setup record and its exact `image_count + 1` FD bundle, holds all FDs
until both owned processes exit, and then closes them. The game-facing glibc
client remains the existing global-dispatch contract, including its
surface, swapchain, get-images, acquire, and present assertions. Per-frame
Binder or Java is not introduced.

For a real Vulkan driver, pass `--hardware-validation`. The harness sets
`BVB_GLOBAL_DISPATCH_HARDWARE=1` only in the client child and records
`client_validation_mode: hardware` in its JSON. This preserves exact fake
fixtures by default while making the tablet client accept native sizes,
alignments, dedicated-allocation preferences, and proxy serials only when they
satisfy the corresponding Vulkan, typed-ownership, repeatability, and target
2800x1752 capability invariants. DXVK's 256-byte push-constant adapter floor
remains mandatory.

This is a transport gate, not a visibility gate. The synthetic sink does not
import an external image into an Activity Vulkan device, consume the frame-ring
futex protocol, blit, or present to Android. Its JSON evidence therefore always
sets `visible_frame_claim` and `fps_claim` to false. Only an actual Activity
consumer run can cross that remaining boundary.

The process owner uses bounded readiness/client/service/FD waits. On success or
failure it terminates only children it created, closes every received FD, and
removes only its unique control socket and empty runtime directory. The random
authentication token is never written to its result JSON.
