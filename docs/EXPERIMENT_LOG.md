# Experiment ledger

Detailed game runs remain in the
[`steamclienttermux`](https://github.com/huntergdavis/steamclienttermux)
technical log. This ledger records only reusable bridge milestones.

## E000 — Target discovery (2026-08-18)

Status: complete.

Target: Samsung Galaxy Tab S8+ (`SM-X808U`), AArch64, Android kernel
`5.10.236`.

Observed on the target:

- Android loader: `/system/lib64/libvulkan.so`, 236,304 bytes,
  SHA-256 `ca031dfc7e10449b207b5fd54d44bfe37404d4a5ae3556a1eeebc3b35cd3d304`.
- Adreno HAL: `/vendor/lib64/hw/vulkan.adreno.so`, 4,044,904 bytes.
- Android properties: `ro.hardware.vulkan=adreno` and
  `ro.hardware.egl=adreno`.
- Termux separately provides `libvulkan.so.1.4.354`, SHA-256
  `c81e31f778c2d727a934893f3cb5f5692305e109ac470380ab2c4545f6a434f0`.
- Termux Clang 21.1.8 targets `aarch64-unknown-linux-android24`.

Conclusion: the native compiler and Android system loader are available, but
loader search order is an experimental confound. E001 must use the absolute
system-loader path.

Prior-session recall query:
`deja "Bionic Vulkan bridge glibc Android system driver GameNative AdrenoTools Termux X11"`
returned no indexed match. The design therefore reuses measured evidence from
the companion repository rather than an unverified recollection.

## E001 — Direct Bionic Vulkan enumeration (2026-08-18)

Status: passed at commit `561e50e22fe1ce828e0225c3ed52a1cb79b79421`.

Hypothesis: the Termux-built Bionic probe can open Android's system Vulkan
loader, create a Vulkan 1.0 instance, and enumerate at least one physical GPU.

Control: the same probe must pass against the deterministic fake loader on both
the development host and tablet.

Success criteria:

- probe exits `0`;
- JSON names the absolute Android system loader;
- at least one physical device is present;
- output records Vulkan API, vendor/device IDs, queue families, and memory heaps.

No game-performance claim follows from this experiment; it only proves the
native half of the proposed bridge.

Result:

- exit code `0`, empty standard error, elapsed time `345,112,708 ns`;
- loader `/system/lib64/libvulkan.so`, Vulkan loader API 1.4.0, 14 instance
  extensions;
- one `Adreno (TM) 730`, Vulkan API 1.1.128, Qualcomm vendor ID `0x5143`;
- two queue families, two memory heaps, 7,914,782,720 device-local bytes;
- loader SHA-256
  `ca031dfc7e10449b207b5fd54d44bfe37404d4a5ae3556a1eeebc3b35cd3d304`;
- HAL SHA-256
  `12781cd1ef0b4bc5fceacd83722d39594005b5e259d44e976b98e41324b55051`.

Conclusion: a Termux-built Bionic executable can access the documented Android
Vulkan loader/HAL path and enumerate the physical GPU directly. Gate 0.2 may
now test a small cross-libc process boundary without speculating about whether
the native half works.

## E002 — glibc client to Bionic service handshake (2026-08-18)

Status: passed at commit `8ec07eef26bad65a3d3307a92b4c23753e0ae72f`.

Hypothesis: two native AArch64 processes using different C libraries can
authenticate over an owner-only Unix socket, exchange the fixed-width protocol,
negotiate version 1, and report Bionic-side Vulkan visibility.

Binary identities:

- service interpreter `/system/bin/linker64`, with Android `libc.so`;
- client interpreter
  `/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1`, with
  `libc.so.6`;
- client compiler `aarch64-linux-gnu` GCC 14.2.1 from `gcc-glibc`;
- client SHA-256
  `f0f8e9449c8885b7f7e97235cc217bad502dddda1e47bf83322c79ecd8ec8fbd`.

Result: both processes exited `0` with empty standard error. The glibc client
negotiated protocol version 1 and received service flags `3`: the server was
compiled for Bionic and could access `/system/lib64/libvulkan.so`. Both sides
reported a 64-bit process and the service reported a 4,096-byte page size. The
observed 66,330,052 ns includes `grun` client process startup and is not yet a
steady-state transport benchmark.

Conclusion: the proposed libc boundary works on the target without PRoot. The
next gate should move the already-proven Vulkan capability query across this
boundary, then compare its document against E001.

## E003 — bridged Vulkan capability parity (2026-08-18)

Status: passed at commit `7a59622fefab86f7a1991925e9771954e3ec9e8f`.

Hypothesis: the Bionic service can run the same Vulkan collector as E001,
encode its result over protocol v1, and produce an equivalent document after a
glibc client decodes it.

Method: `scripts/test-cross-libc-termux.sh` compiled the client with
`gcc-glibc`, verified its glibc interpreter, started the Bionic service, and
requested `VULKAN_CAPS`. It then ran a fresh direct Bionic probe and compared
loader API, extension/device counts, device identity, Vulkan/driver versions,
queue and heap counts, and device-local bytes.

Result: `capability_parity=PASS`. The bridged document reported the Adreno 730,
loader API 1.4.0 (`4210688`), device API 1.1.128 (`4198528`), 14 instance
extensions, two queue families, two memory heaps, and 7,914,782,720
device-local bytes—identical to the direct control. Both service and client
exited `0` with no reported error. The measured 311,055,990 ns includes glibc
client startup, two protocol exchanges, and the Vulkan query; it is not a
steady-state per-call benchmark.

Conclusion: Vulkan data now crosses the real glibc/Bionic boundary without
PRoot and without changing the observed capabilities. This proves control-plane
feasibility. It does not yet implement Vulkan object creation, command
submission, Android surfaces, or a game-facing loader.

## E004 — native Vulkan command submission (2026-08-18)

Status: passed at commit `0ad41f81f96a828cb73b387af025427a7c138a76`.

Hypothesis: the Bionic process can move beyond discovery and execute a real
command buffer through Android's Adreno driver, then verify the result from
host-visible memory.

Method: the self-test opened the absolute Android loader, inventoried known
instance/device extensions, created a logical device, selected a compatible
queue and host-visible memory type, recorded `vkCmdFillBuffer` plus a
transfer-to-host barrier, submitted and waited, mapped 4,096 bytes, and checked
all 1,024 32-bit words against `0xa5c3f00d`.

The first hardware attempt at commit `819371c` failed cleanly with `no
transfer-capable queue family`. The Adreno queue flags were later observed as
`27`: graphics, compute, sparse-binding, and protected, but no explicit
transfer bit. The Vulkan specification makes the transfer bit optional when a
queue reports graphics or compute. Commit `0ad41f8` corrected only that
selection rule to accept graphics, compute, or explicit-transfer queues.

Result: exit `0`, empty standard error, and zero mismatched words. The selected
queue was family 0 with flags `27`; memory type 6 had flags `15`
(device-local, host-visible, coherent, cached). Submit plus queue-idle wait took
3,298,542 ns and whole-process time was 206,800,729 ns.

The loader exposed 14 instance and 90 device extensions. Known relevant
extensions included Android surface/swapchain, external memory and semaphore
FDs, Android hardware-buffer memory, timeline semaphore, and external fence
FD. `VK_EXT_headless_surface` was absent.

## E005 — glibc-triggered native command execution (2026-08-18)

Status: passed at commit `46a079a7e95c42de086e25cd4fe4dd55aa85ebd3`.

Protocol opcode 3 carries a fixed 64-byte little-endian self-test result. The
glibc client negotiated protocol v1, requested capabilities, then caused the
Bionic service to execute the same native GPU fill. The published script ran a
second direct Bionic control and required parity for every deterministic field.

Result: `capability_parity=PASS` and `command_selftest_parity=PASS`. The
service-side GPU submit/wait took 3,440,573 ns; the immediate direct control
took 2,371,511 ns. Both verified 1,024 words with zero mismatches. The combined
glibc startup, handshake, capability query, and bridged command test took
209,695,208 ns. These single timings establish operation and rough scale, not a
steady-state throughput claim.

Conclusion: the project now proves discovery, device/object creation, command
recording, submission, synchronization, host-visible memory, and cross-libc
control against the real Android driver. The next unproven boundary is WSI: an
owned `ANativeWindow`, Vulkan surface, swapchain, and visible presentation.

## E006 — controlled ANativeWindow Vulkan surface (2026-08-18)

Status: passed at commit `b47d9b56c1aecf4dad579d86c248a6e8c2730fd9`.

Hypothesis: a Bionic process can create an independently owned Android native
window and use it for Vulkan WSI without reinterpreting or taking ownership of
Termux:X11's existing X/EGL surface.

Method: the probe dynamically opened `/system/lib64/libmediandk.so`, created a
64x64 RGBA8888 `AImageReader` with GPU-sampled-image consumer usage, obtained
its `ANativeWindow`, then opened `/system/lib64/libvulkan.so`. It enabled
`VK_KHR_surface` and `VK_KHR_android_surface`, created the Android surface, and
queried queue support, capabilities, formats, and present modes. The reader
owns the native window and deletes it after the Vulkan surface and instance.
No Steam, Termux:X11, KDE, or login-state process was touched.

Result: the first hardware run exited zero in 669,521,667 ns. Queue family 0
supported presentation with three queues. The 64x64 surface reported minimum
and maximum image counts of 6 and 64, extents from 1x1 through 4096x4096, all
nine transform bits, inherited composite alpha, and usage flags `159`.

Five formats were available: `VK_FORMAT_R8G8B8A8_UNORM`,
`VK_FORMAT_R8G8B8A8_SRGB`, `VK_FORMAT_R5G6B5_UNORM_PACK16`,
`VK_FORMAT_R16G16B16A16_SFLOAT`, and
`VK_FORMAT_A2B10G10R10_UNORM_PACK32`, all with nonlinear sRGB color space.
Present modes were MAILBOX, FIFO, SHARED_DEMAND_REFRESH, and
SHARED_CONTINUOUS_REFRESH; IMMEDIATE was absent.

Three immediate lifecycle repeats returned identical capabilities and took
290,248,333, 288,357,448, and 282,208,125 ns. A first `pgrep -f` cleanup check
incorrectly matched its own wrapper command; an anchored executable pattern
then confirmed no lingering probe process. The probe ELF uses Android
`/system/bin/linker64`, needs only `libdl.so` and `libc.so`, and has SHA-256
`dfcb55c01d86974dd06a7dc14419de605047854d0b008bc270dc95907e453a63`.

Conclusion: an independently controlled Android native window and Vulkan
surface now work on the Adreno 730. This does not yet prove swapchain creation,
image acquisition, command rendering, queue presentation, or visible output.
Those are the next gate; Termux:X11's EGL-owned surface remains untouched.

## E007 — swapchain present and consumer readback (2026-08-18)

Status: passed at commit `e4af028553450bcfdf7d8efc903cd410ca8ef83a`.

Hypothesis: the controlled `AImageReader` surface can complete the entire
Vulkan producer and Android BufferQueue consumer loop, with pixel verification,
without using Termux:X11's display surface.

Method: a Bionic executable created a CPU-readable 64x64 RGBA8888
`AImageReader`, Android Vulkan surface, logical device, presentation queue, and
FIFO swapchain. It acquired image 0 with a semaphore, transitioned the image
from undefined to transfer-destination layout, cleared it to opaque magenta,
transitioned it to presentation layout, submitted with acquire/render
semaphores, and called `vkQueuePresentKHR`. It then synchronously acquired the
consumer `AImage`, honored its plane, row, and pixel strides, and checked every
pixel for RGBA bytes `ff 00 ff ff`.

Result: the first run created the driver's requested six-image swapchain and
passed with 0 of 4,096 pixels mismatched. The consumer image had one plane,
4-byte pixel stride, 256-byte row stride, and 16,384 accessible bytes.
Submit plus presentation took 3,988,438 ns and total cold process time was
471,708,072 ns. The image was available on the first Media NDK acquisition
attempt.

Three immediate full lifecycle repeats returned the same swapchain/image and
zero-mismatch result. Their submit/present times were 3,343,698, 3,955,313,
and 3,721,719 ns; total times were 310,404,166, 301,790,677, and 303,929,270
ns. An anchored process check found no lingering executable. The Bionic ELF
uses `/system/bin/linker64`, needs only `libdl.so` and `libc.so`, and has
SHA-256
`1a0eed2d72b053cbb913d2aa1c0473caaf1f8127bcb3df47b05d72da6722d35c`.

The required recall query—`Android Vulkan AImageReader ANativeWindow swapchain
acquire present clear image Adreno Bionic glibc Termux X11`—returned no indexed
prior implementation, so E007 reused this repository's E006 ownership model
and the pinned Vulkan/Media NDK contracts.

Conclusion: device creation, swapchain creation, acquisition, command
execution, synchronization, queue presentation, BufferQueue delivery, CPU
consumer acquisition, and deterministic pixel verification now pass on the
real Adreno driver. This is genuine rendered/presented offscreen output, but it
is not yet visible on the tablet display and is not yet reachable through the
glibc bridge or DXVK. The next gate is a dedicated visible `SurfaceView` host.
