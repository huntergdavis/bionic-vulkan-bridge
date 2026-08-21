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

## E008 — dedicated visible Android Vulkan host (2026-08-18)

Status: passed at commit `e1ba5a7d05b4116f104e88b9b035792281225f88`.

Hypothesis: a standalone Android Activity can own a display `ANativeWindow` and
present the deterministic Adreno Vulkan frame visibly without taking ownership
of Termux:X11's EGL surface.

Method: the project added a no-Java `android.app.NativeActivity` APK. Its
Bionic shared library receives the Activity's window callback, creates the
Android Vulkan surface, presentation queue, FIFO swapchain, semaphores, and
command buffer, clears an acquired RGBA8 image to opaque magenta, and presents
it. Renderer objects remain alive until Android destroys the window. The
reproducible Termux script compiles with Clang `-Werror`, packages with `aapt`,
aligns, debug-signs, and verifies the APK. Generated keys and binaries remain
outside Git in `out/`.

Build result: the native library targets Android API 24 and needs only
`libandroid.so`, `liblog.so`, `libvulkan.so`, `libdl.so`, and `libc.so`. Its
exported `ANativeActivity_onCreate` entry point was present. The APK targets API
36, verified with v2/v3 signatures, installed as
`io.github.huntergdavis.bvb.visiblehost`, and exposed the NativeActivity as its
launcher.

Visual result: after installation and explicit launch, the user reported a
solid magenta screen occupying the display. Android navigation icons remained
visible along the bottom. This is a passed visible-presentation gate; hiding
the navigation bar is a later immersive-mode task. The app did not modify or
share Termux:X11's existing surface.

Artifact identities:

- APK SHA-256:
  `f9159df9419418ea6eebd62963e27ac2273f7be3bf4acdbc6fc8a5ff66aeb298`
- native library SHA-256:
  `8f8396492e304fe671b033cd1f3ca9b7ded8f179ed506376dba4bcfbbc7b4b75`
- signer certificate SHA-256:
  `5754d1170b6d007e72afed7de676312b9a1320d39c7b1405be9efa1ab2ef1e06`

The required NativeActivity implementation and crash-specific `deja` queries
returned no indexed prior match. One early `pidof` observation was initially
treated as an exit, but cross-UID process visibility made that inference
invalid; the packaged ELF and entry point were intact, no matching crash was
observed, and the direct display result established that the Activity was
running.

Conclusion: the project now owns a working visible Android presentation target
in addition to the pixel-verified offscreen path. It is still a standalone
test APK: explicit lifecycle/status handoff through the bridge, shared-UID
integration, game-facing dispatch, DXVK, and performance A/B remain unproven.

## E009 — immersive visible Android Vulkan host (2026-08-18)

Status: passed at source commit
`4b2b5a6cbd116cadfbe4595d8c610046decf3291`.

Hypothesis: the E008 host can occupy the entire tablet display, including the
navigation-bar area, while preserving the already-proven Vulkan presentation
path.

Method: the native Activity uses JNI to obtain its Window decor View and apply
Android's legacy immersive-sticky system-UI flags: hide navigation, fullscreen,
stable layout, layout-hide-navigation, layout-fullscreen, and
immersive-sticky. It applies them during `ANativeActivity_onCreate` and again
from `onWindowFocusChanged` whenever focus returns. The Vulkan renderer and its
window lifecycle are unchanged from E008.

Visual result: after updating and launching version `0.1.1` (version code 2),
the opaque-magenta Vulkan frame remained visible and the user reported that it
was "totally full screen no icons." This direct observation on the physical
tablet passes the immersive presentation gate. Android's cross-UID capture and
process visibility restrictions mean no Termux-side screenshot or `pidof`
claim is used as evidence.

Artifact identities:

- APK SHA-256:
  `9da4ba1d459bc6470ed7a9c0729adfb03e77a79accb1739cd5393293aa0ebb57`
- native library SHA-256:
  `b4772396af9bda82a41247ab06b153bb2d37eb5f2ff9447951ff3a418b7f21e5`
- signer certificate SHA-256:
  `5754d1170b6d007e72afed7de676312b9a1320d39c7b1405be9efa1ab2ef1e06`

The required recall query—`Android NativeActivity immersive sticky hide
navigation bar JNI setSystemUiVisibility onWindowFocusChanged`—returned no
indexed prior match. E009 therefore reused E008's proven NativeActivity window
ownership and renderer, adding only the Android View system-UI policy.

Conclusion: the project now has a dedicated, visibly rendered, immersive
Android Vulkan host. The next controlled gate is an explicit lifecycle/status
handoff between this Activity and the existing Bionic bridge service; Steam,
DXVK, game input, and game-facing dispatch remain outside this result.

## E010 — authenticated Activity lifecycle/status handoff (2026-08-18)

Status: passed at commit `293fbee07b6710b45d39b74dde4dd1255452e557`.

Hypothesis: the standalone Android Activity can explicitly publish its window,
renderer, and focus state to the existing Bionic bridge service, and a glibc
client can query that accepted state without weakening the existing
owner-authenticated Unix control socket.

Method: protocol v1 gained fixed-width lifecycle records and ACKs plus a
read-only `ACTIVITY_STATUS` opcode. Because the Activity and Termux service use
different Android UIDs, the service opens an opt-in ephemeral listener bound
only to `127.0.0.1`. The harness generates a fresh 256-bit capability from
`/dev/urandom` and supplies it to both the service and Activity through private
launch arguments. Records carry the capability, monotonically increasing
sequence, event, Activity PID, dimensions, and monotonic timestamp. The token
is not printed, returned by the query, or retained in evidence. The service's
mode-0600, peer-credential-authenticated Unix socket remains unchanged.

Security control: before launching the Activity, the hardware harness sent a
well-formed `CREATED` record with an all-zero token. The service returned an
explicit `-EACCES` ACK and recorded exactly one rejected event. It then accepted
seven events from the real Activity PID 6254 in order: `CREATED`, `STARTED`,
`RESUMED`, `WINDOW_CREATED`, `RENDERER_READY`, `FOCUS_LOST`, and
`FOCUS_GAINED`, with sequences 1 through 7.

Result: the glibc client queried the Bionic service and received service flags
7 (Bionic, Android system Vulkan loader, and Activity ingress), 64-bit pointers,
and 4,096-byte pages. The Activity snapshot reported seven authenticated
events, one rejected event, last sequence 7, last event `FOCUS_GAINED`, state
flags 63, and every active flag true: created, started, resumed, window present,
renderer ready, and focused. The window was exactly 2800x1752, matching the
tablet panel; destroyed was false. The final event's service-receive timestamp
was 914,896 ns after its Activity timestamp. Service stderr was empty.

Binary and artifact identities:

- visible-host APK SHA-256:
  `82cea5f637ac448ca438078ba1d6757953da16c8ffe37978dc422878f0ab0bb5`
- visible-host native library SHA-256:
  `aed79197bfe5c7a118bfc1b2c1b2c8377bf8f95e6db9bbb96be7e6bab4132587`
- Bionic service SHA-256:
  `082dc59b257f27557525cdb612c5c84a95dff21a660a162883989e4b639da594`
- glibc client SHA-256:
  `8cb4eabf2163b668cdf141fb2b0735414d96326246d59198fa38d4c27b30bac5`
- status JSON SHA-256:
  `cd95b40b7cb0a0baebfea34cc4859b942ba05c7cecbf97179e5a41211d5070ad`
- service stdout SHA-256:
  `26a1d8857119437f99fd7b7c0268f17aa234ea82c1175163960fcc6aedce0a85`
- empty service stderr SHA-256:
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- signer certificate SHA-256:
  `5754d1170b6d007e72afed7de676312b9a1320d39c7b1405be9efa1ab2ef1e06`

The service ELF requests `/system/bin/linker64`; the querying client requests
Termux glibc's `ld-linux-aarch64.so.1`. APK version `0.1.2` (code 3) targets API
36, its native library targets API 24, and its only added permission is
`android.permission.INTERNET` for the loopback connection.

The required recall query—`Bionic Vulkan bridge NativeActivity lifecycle status
handoff Unix socket Android Activity service window focus protocol`—returned no
indexed prior implementation. E010 reused the repository's E002-E005
fixed-width protocol and authenticated Unix control plane plus E008-E009's
Activity ownership, callbacks, renderer, and immersive behavior.

Conclusion: visible-host lifecycle is now real, authenticated bridge state that
a glibc process can query. E010 does not carry Vulkan commands or game input and
does not claim an FPS gain. The next gate is to trace the minimum Vulkan entry
points required at DXVK startup and design generated handle/dispatch ownership
without turning each hot Vulkan call into a synchronous socket RPC.

## E011 — process-proven Tomb Raider Vulkan dispatch inventory (2026-08-18)

Status: passed with the manifest generator at commit
`9d9147a959892371bce2c31977519d08e724d460`.

Hypothesis: an opt-in glibc preload can inventory the real Vulkan entry points
resolved during Tomb Raider's Wine/DXVK startup, identify the originating
process, preserve normal resolution behavior, and turn the result into
deterministic dispatch ownership metadata from the pinned Khronos registry.

Method: the tracer wraps `dlsym`, `vkGetInstanceProcAddr`, and
`vkGetDeviceProcAddr`. Trace format v1 writes each lookup in one append-only
record containing format version, PID, TID, process-local sequence, lookup
stage, resolution result, and name. The analyzer rejects malformed records and
joins each name to pinned `vk.xml` metadata to classify global, instance,
device, and Wine-private dispatch.

The first SSH-started attempt was rejected before Steam by the existing
affinity guard because its controller was in `cpuset=/moderate` and
`cpu=/background`; its trace remained empty. The valid attempt reused the
repository's guarded foreground-service technique. A `/system/bin/sleep 20`
probe first proved both `/top-app` controllers. The real controller PID 5373,
dispatcher PID 5390, affinity guard PID 5392, Steam PID 5973, and later game
PID 7063 all independently reported `cpuset=/top-app` and `cpu=/top-app`.

Result: the stable trace contains 3,828 valid v1 records, 742 unique names, and
440 names that resolved at least once. Registry ownership is 4 global, 101
instance, and 635 device commands plus exactly two Wine-private names:
`wine_vkAcquireKeyedMutex` and `wine_vkReleaseKeyedMutex`. No name was unknown
to both the pinned registry and the explicit Wine-private namespace.

PID 7063 was verified through its exact `TombRaider.exe` command line,
`STEAM_COMPAT_APP_ID=203160`, trace-file environment, affinity log, and both
top-app controllers. It contributed 3,074 records, 711 unique names, and 328
resolved names. Transient Wine-side startup PID 7028 contributed 754 records,
675 unique names, and 430 resolved names. Its exact command line was not
retained, so no stronger process identity is claimed. The combined inventory
is required: the transient process contributed 31 display/WSI names not seen
from the game PID.

Evidence identities:

- tablet trace:
  `steam-arm64/logs/tombraider-vulkan-resolve-20260819T014633Z-5373.tsv`,
  187,192 bytes, SHA-256
  `3c9b119672cde7027fa0dcac42a6b7f72d5ecff54bd436647906e10d264cec61`
- generated manifest:
  `docs/evidence/e011-tombraider-vulkan-dispatch-manifest.json`, 251,387
  bytes, SHA-256
  `6a78ddd1f1b34773e4123d3612233fab2270ed0ff418d01204ad41af1cab8afc`
- tablet glibc tracer: SHA-256
  `f05b548e9e7a20da536a082aeb6014e41d822a0d6986c478f5ebdd12daed0683`

Two earlier legacy traces were byte-for-byte identical to each other at
124,085 bytes and SHA-256
`ff23b1ced1c30a90ed067199901fc21abbb010ed9fcab218b47db48afbe9be30`.
They have the same 3,828 records, 742 names, 440 resolved names, and ownership
counts, but cannot separate processes. That repeat validates the logical
inventory while v1 supplies the missing provenance.

The root-window screenshot attempt timed out and left a PNG that the evidence
viewer could not decode, so E011 makes no visual UI claim. After the trace was
stable, only the fully validated game PID received `TERM`; it exited, while
Steam PID 5973 and Termux:X11 PID 27923 remained alive. No performance result
is claimed because tracing was enabled.

The required recall query—`bionic vulkan bridge next gate external memory
swapchain game integration performance`—returned no indexed prior session.
E011 reused the repository's E010 fixed-width ownership model, the earlier
foreground RunCommandService race fix, and the opt-in trace launch added at
steamclienttermux commit `2d1c4ec`.

Conclusion: the startup resolution gate is complete and repeatable. Resolution
is not execution-frequency profiling, so the manifest bounds dispatch coverage
but does not label commands hot. The next gate is generated proxy-handle
ownership plus a shared-memory command batch for the minimal rendered triangle
subset, following [decision 0003](decisions/0003-batched-game-dispatch.md).

## E012 — glibc-built batch replayed by Bionic Vulkan (2026-08-18)

Status: passed on hardware at commit
`1f1229828fb11408dc308ce2b4d3662699513048`.

Hypothesis: a glibc client can construct one bounded, little-endian command
batch with typed wire handles, send it once across the authenticated bridge,
and have the Bionic service resolve those handles and replay the commands on
the real Android Vulkan driver without an RPC for each Vulkan command.

Method: stable 64-bit IDs encode object type in the high byte and a nonzero
serial in the remaining 56 bits. The Bionic side stores real handle bits in an
open-addressed ownership table; pointer values never enter the batch. The
client built a 104-byte batch containing a command-buffer ID, sequence 1, one
`FILL_BUFFER` record, and one `BUFFER_HOST_READ_BARRIER` record. The service
validated the entire batch, resolved its command buffer and buffer, recorded
both operations, submitted once, waited, mapped the 4 KiB allocation, and
checked all 1,024 words against `0xa5c3f00d`.

The hardware harness ran three paths against `/system/lib64/libvulkan.so` and
Adreno 730: the established cross-libc service self-test, the standalone
Bionic control, and the new glibc-built batch. Capability and deterministic
result fields matched across all paths. The batch had zero mismatched words.
Its GPU submit-plus-wait observation was 4,427,084 ns; the standalone control
was 4,888,594 ns and the established service path was 6,469,115 ns. Total
batch client/service elapsed time was 199,013,750 ns and included fresh process,
instance, device, memory, and queue setup.

These are single correctness observations while Steam remained resident, so
they do not establish that batching is faster. They do establish that the
batch mechanism did not add a per-command socket exchange and that its replay
reached the same real-driver result.

All seven tablet CTest cases passed, including the native command-batch
contract and fake-driver integration. The glibc client requests
`/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1`; the Bionic
service requests `/system/bin/linker64`. Both batch client and service stderr
artifacts were empty. Steam PID 5973 and Termux:X11 PID 27923 remained alive;
no game process ran during this test.

Evidence identities:

- result: `docs/evidence/e012-cross-libc-batch.json`, 958 bytes, SHA-256
  `1c823fedbfbfc9a499d289456253d508e022d75554dd3fced7746b49ee910445`
- glibc client: 75,616 bytes, SHA-256
  `7756a8f363b3e1334145a14e0c92e50ef78fdf046ce712b3f6de80765543d2b7`
- Bionic service: 50,088 bytes, SHA-256
  `639532e6c75edc10e58cecdc18a18ad8cd940c9e2e020bd885c5f2c3487b27e7`
- batch service stderr and batch client stderr: empty, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

The required recall query—`Vulkan proxy handles shared memory command batch
protocol triangle bionic glibc`—returned no indexed prior implementation.
E012 reused E004/E005's exact native buffer, memory, submission, and mapped
verification lifecycle plus E002's authenticated, fixed-width cross-libc
transport.

Conclusion: the first real driver command batch now crosses glibc to Bionic
and verifies GPU output. The packet-sized 104-byte proof is not the final data
plane. The next gate is a shared-memory batch region with sequence/ownership
validation and measured warm throughput, followed by generated triangle
dispatch.

## E013 — sealed shared-memory batch replay on Adreno (2026-08-18)

Status: passed on hardware at code commit
`d942b9a62b99232634b37d921012a3ad2d42ae2b`.

Hypothesis: the glibc client can pass a command region to Bionic once, then
request replay using only bounded metadata while the command bytes remain out
of the socket payload. The Bionic service must validate ownership, generation,
bounds, and sequence before replaying the same deterministic GPU operation as
E012.

Method: the client created a 4,096-byte memfd with
`MFD_CLOEXEC | MFD_ALLOW_SEALING`, placed the 104-byte E012 transfer batch at
offset 64, and sealed the file against growth and shrinkage. It sent that file
descriptor once with `SCM_RIGHTS` in a 16-byte setup payload. The Bionic
service verified the regular-file size and page alignment, mapped it read-only,
and associated it with a nonzero random generation for the authenticated
connection. Execution used a 24-byte payload containing only generation,
offset, length, and monotonically increasing sequence; with the normal 24-byte
header, the request was 48 bytes. No command bytes or pointers crossed in the
execute packet.

The service rejected duplicate setup, stale generation, invalid bounds,
non-increasing sequence, and unexpected descriptors. It resolved the typed
command-buffer and buffer IDs to real Bionic-owned Vulkan handles, replayed the
fill and host-read barrier through `/system/lib64/libvulkan.so`, submitted on
Adreno 730, and verified all 1,024 32-bit words in the 4 KiB buffer.

Result: the shared path had zero mismatched words. Its GPU submit-plus-wait
observation was 4,001,042 ns, compared with 3,324,843 ns for the packet batch,
3,647,344 ns for the standalone direct control, and 4,366,980 ns for the
established bridged control in the same harness invocation. Total client/service
elapsed observations were 192,158,906 ns for shared memory, 197,819,947 ns for
the packet batch, and 210,582,448 ns for the established bridge.

Those are single cold correctness observations, not a speed conclusion. Every
path still includes fresh processes plus Vulkan instance, device, queue,
allocation, submission, and teardown. E013 proves the intended data-plane
shape; it does not yet measure steady-state notification or replay overhead.

All eight Android-supported CTest cases passed, including real memfd descriptor
transport and fake-driver bridge integration. All ten host tests also passed.
The glibc client requests
`/data/data/com.termux/files/usr/glibc/lib/ld-linux-aarch64.so.1`; the Bionic
service requests `/system/bin/linker64`. Both shared client and service stderr
artifacts were empty. Termux:X11 PID 27923 remained alive after the test; no
Steam-named process was resident when post-run state was inspected.

Evidence identities:

- result: `docs/evidence/e013-cross-libc-shared-batch.json`, 965 bytes,
  SHA-256
  `659c660297a314e129a0f3dc922b8517a7a7ea072e50aad3719389f7ec86bf2c`
- glibc client: 76,424 bytes, SHA-256
  `330dd56a34308b08a106c2fe84efa282b2c2f0815b517c24d4b4f02eb1245c63`
- Bionic service: 53,664 bytes, SHA-256
  `2235f78ce51ae841c7ae3d861bb7e6737864c86c348e1ab7b9ef3a44e13da320`
- shared client and service stderr: empty, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

The required recall queries—`Termux Android Bionic glibc memfd SCM_RIGHTS
shared memory Vulkan batch` and `Android Bionic memfd_create undeclared
SYS_memfd_create transport test`—returned no indexed implementation. E013
reused E012's exact typed-handle batch and deterministic verification, E002's
authenticated fixed-width transport, and Android/Linux's working
`SYS_memfd_create` plus Unix descriptor passing.

Conclusion: the command data plane no longer copies batch bytes through each
execute request. The next speed gate is to keep the Bionic Vulkan
instance/device/queue and handle table alive, replay multiple sequenced batches
from the same mapping, and measure warm control, validation, replay, RSS, and
thermal behavior. Generated game-facing dispatch and the visible triangle
follow that persistent-context measurement.

## E014 — persistent Vulkan context and warm replay (2026-08-18)

Status: passed on hardware at commit
`64e22748b551a3375b84d398ffb906029e0f504b`.

Hypothesis: most of E013's roughly 192 ms end-to-end observation is cold
process and Vulkan initialization rather than shared-memory transport. Keeping
the Bionic loader, instance, device, queue, allocation, command pool/buffer,
mapped verification memory, function table, and typed-handle table alive should
reduce each later batch to bounded notification, validation, command recording,
GPU work/wait, verification, and response.

Method: the one-shot self-tests were preserved as create/execute/destroy
wrappers around a new opaque persistent context. A shared connection creates
the context on sequence 1, which is excluded as warm-up. Before each later
request, the glibc client rewrites the 104-byte batch in the existing memfd with
a new sequence, publishes it with a release fence, then sends the same 48-byte
header-plus-metadata execute request used in E013. The Bionic service acquires
the shared bytes, rejects non-increasing sequences, resets and rerecords the
existing command pool/buffer, submits, waits, verifies the persistently mapped
4 KiB result, and replies. No Vulkan object or memfd is recreated during the
100 measured executions.

Result: all 100 measured batches had zero mismatched words. Complete control
round-trip, validation, replay/wait, verification, and response latency was
653,854 ns minimum, 853,561 ns mean, and 1,659,010 ns maximum. GPU
submit-plus-wait was 326,510 ns minimum, 503,056 ns mean, and 1,344,271 ns
maximum. The difference between the two means is about 350,505 ns of client,
transport, validation/recording, verification, serialization, and scheduling
overhead per batch.

The same harness invocation's cold one-shot shared path took 200,972,865 ns
end-to-end. Comparing that cold process/lifecycle observation with the warm
control mean gives an approximately 235-fold latency reduction. This is the
intended effect of persistence, not a claim that a game is 235 times faster.
The proof deliberately calls `vkQueueWaitIdle` and validates host-visible
memory every iteration; an actual render path must use asynchronous fences and
multiple in-flight command buffers.

The full tablet harness also preserved direct, bridged, packet-batch, and
one-shot shared parity. All eight Android-supported CTest cases and all ten
normal host tests passed. In an ASan/UBSan host build, every changed path,
including the repeated bridge integration, passed; the unrelated preload
`vulkan-resolve-trace` test failed only in that sanitized build and remains
passing normally, so no sanitizer-wide all-green claim is made.

Evidence identities:

- result: `docs/evidence/e014-warm-shared-batch.json`, 1,280 bytes, SHA-256
  `c4e46b25cd66094058f421418775698e847c62d68206463516421b4e840a314e`
- glibc client: 76,576 bytes, SHA-256
  `1dfbe8503ba12c9e5eff2fafa08b3b7712d46b80b1bbf5041e031b758a461428`
- Bionic service: 55,304 bytes, SHA-256
  `d89f662cbdf2f69dfcd57e901d80dacc2cde93694f4e9f420ac4fc655599ed5c`
- warm client and service stderr: empty, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

The required recall query—`persistent Bionic Vulkan context warm shared memory
batch benchmark repeated execute`—returned no indexed implementation. E014
reused E012's typed handles and deterministic transfer batch, E013's
connection-owned shared region and descriptor transport, and the existing
native Vulkan object's exact capability/memory selection.

Conclusion: cold Vulkan setup is no longer in the batch hot path, and the
remaining measured bridge overhead is sub-millisecond for this small
synchronous proof. The next gate is generated executable client dispatch for
the minimal triangle subset, backed by this persistent service and followed by
asynchronous external-memory/synchronization work for real frames.

## E015 — generated executable glibc triangle dispatch (2026-08-18)

Status: passed on the target glibc ABI at commit
`0188ee9c2bf4f4fb3a1aa4d4696bc7e25de28ccb`.

Hypothesis: the E011 trace and pinned Vulkan registry can generate a reviewable
device-dispatch table whose returned functions accept real Vulkan signatures
and encode the already-tested triangle batch, without hand-maintaining aliases
or exposing internal encoder symbols.

Method: the generator reads pinned `vk.xml` and the exact E011 manifest. It
requires each canonical command to return `void`, dispatch first on
`VkCommandBuffer`, and have resolved at device stage in the Tomb Raider trace.
Observed aliases are discovered from registry identity rather than a parallel
alias list. The generated include contains eight names: six canonical commands
(`vkCmdBeginRendering`, `vkCmdBindPipeline`, `vkCmdSetViewport`,
`vkCmdSetScissor`, `vkCmdDraw`, and `vkCmdEndRendering`) plus the observed KHR
begin/end aliases. Compile-time `_Generic` assertions require every wrapper to
match its generated `PFN_vk*` type exactly.

The glibc library's `vkGetDeviceProcAddr` returns those internal wrappers and
returns null for an unimplemented name. A proxy command buffer owns a bounded
builder and sticky error state. The initial supported dynamic-rendering shape
is deliberately narrow: one color attachment, origin-zero render area, no
depth/stencil, no multiview or resolve, one viewport/scissor, and a graphics
pipeline. Unsupported shapes fail deterministically instead of being silently
misencoded.

Result: the executable ABI test resolved all canonical functions, proved the
KHR aliases return their canonical function pointers, invoked real Vulkan C
signatures, and produced a valid six-record, sequence-11 batch with typed
command-buffer, image-view, and pipeline IDs. Decoding recovered the exact
1,280 × 720 render area, clear color, pipeline, viewport/scissor, and
three-vertex draw. A two-viewport request correctly produced `-ENOTSUP`.

The same test passed under a focused ASan/UBSan host build, the normal host
suite passed all 11 cases, and the Bionic tablet build passed all nine
Android-supported cases. A separate `grun` harness compiled the shared library
with `-O3` on the tablet, linked and ran a glibc test executable, and observed
empty stderr. The library exports only `vkGetDeviceProcAddr` and four
bridge-owned command-buffer helpers; individual wrapper and encoder symbols
remain hidden.

Evidence identities:

- generated table: `docs/evidence/e015-triangle-dispatch.inc`, 1,000 bytes,
  SHA-256
  `c3e4aac0e16abdb218d34adb7cc83c23118b2be0e0047e9e5668d30a16185839`
- tablet glibc library: 73,680 bytes, SHA-256
  `d3c0c5497c57afa0168364dce77851b99108a1987c821c6484fa931bb8b3ae9a`
- tablet glibc test executable: 74,416 bytes, SHA-256
  `ee16a02a54bd71ba240f57448c92152974c0ed856109179803648b0c6a6d3b17`
- test stdout: 45 bytes, SHA-256
  `4b71acdf604cc2cb9cc9a648ee76088a490dff103e25b1b01d0502262890e44c`
- empty test stderr: SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

The test executable requests Termux glibc's
`ld-linux-aarch64.so.1`; the generated library needs only `libc.so.6` and that
loader. The generated table embeds pinned `vk.xml` SHA-256
`80e7394d0e787d6ec78b67aa324add6f96129fdd042ba640cc336a5481a208ee`
and E011 manifest SHA-256
`6a78ddd1f1b34773e4123d3612233fab2270ed9fcab218b47db48af1cab8afc`.

The required recall query—`generated Vulkan client ICD dispatch
vkGetInstanceProcAddr triangle subset shared command batch vk.xml`—returned no
indexed implementation. E015 reused E011's process-proven registry metadata,
E012's typed proxy handles, and the six triangle records already covered by the
command-batch contract.

Conclusion: Vulkan command calls can now enter generated executable dispatch
on the real glibc client ABI and become validated bridge batches. This is not
yet a rendered triangle: the Bionic side still needs real image-view/pipeline
ownership and triangle replay, and the visible host then needs an explicit
external-image/synchronization path.

## E016 — visible triangle with render-pass compatibility lowering (2026-08-18)

Status: passed on hardware. The initial dynamic-rendering implementation was
committed at `8b0b962`; the working compatibility fallback is commit
`d3f5763`, with the hardened installed-version/lifecycle harness at `32869a3`.

Hypothesis: the visible Bionic Activity can own an Android swapchain image,
image view, graphics pipeline, and synchronization objects, then replay the
same six-record triangle batch emitted by E015 and present a real GPU-drawn
frame. The batch must stay independent of native pointer values.

Method: checked-in GLSL vertex and fragment shaders are compiled to Vulkan 1.1
SPIR-V and embedded in the no-Java `NativeActivity` library. The Activity
creates the Android surface and swapchain, inserts the command buffer, image
view, and graphics pipeline into the typed handle table, validates the exact
six-record batch, records it, submits it to Adreno 730, and presents it.

The first hardware launch produced a fullscreen black surface and emitted
authenticated lifecycle event 12 (`renderer failed`). A Bionic self-test
against the same absolute `/system/lib64/libvulkan.so` path showed one Vulkan
1.1.128 Adreno device and 90 device extensions, but no
`VK_KHR_dynamic_rendering`. This rejected the original extension requirement
before shader or draw work. Termux's separate Mesa/Turnip `vulkaninfo` result
was deliberately excluded because it is not the Android loader used by the
Activity.

The working executor preserves the game-facing begin/end-rendering records but
lowers the currently supported one-color, clear/store, single-layer shape to
`vkCmdBeginRenderPass` and `vkCmdEndRenderPass`. Bionic owns the compatible
render pass and framebuffer; viewport, scissor, pipeline bind, and draw remain
direct native commands. Unsupported shapes return `-ENOTSUP` rather than being
silently changed.

Result: APK version 0.1.4 (version code 5) compiled and signature-verified on
the tablet. The authenticated lifecycle gate rejected an invalid 256-bit
token, then reported a created, started, resumed, focused, window-present, and
renderer-ready Activity with state flags 63 at 2800x1752. The user visually
confirmed the triangle on the tablet. Android's privileged screenshot command
is unavailable to the Termux UID, and this Termux:API release has no screenshot
endpoint, so no screenshot artifact is claimed yet.

All 11 normal host tests passed before the hardware build. The hardened
lifecycle harness now rejects a stale installed APK version before launch and
prints authenticated renderer-failure events immediately instead of reporting
only a timeout.

Evidence identities:

- lifecycle status: `docs/evidence/e016-visible-triangle-status.json`, 574
  bytes, SHA-256
  `d7b75bda7abe1fb34c9fda7e4a5d1ed62a264bcce4adad6ec7687f08f499345e`
- Android-loader self-test:
  `docs/evidence/e016-android-vulkan-selftest.json`, 767 bytes, SHA-256
  `fe8605453844277ea95d431913e6289c83a7bdc3623ba46a04c34012cf8f229f`
- APK: 33,136 bytes, SHA-256
  `96d8d75d836bdc51594aa2c8ba3f2a772c3d5c8defb949e6a696d9d313c97d7d`
- Bionic native library: 51,328 bytes, SHA-256
  `32a9ac5d79b01047768a565b6324309e0f51ddfea2f697d73f7d8563120a2cc4`
- vertex SPIR-V: 1,512 bytes, SHA-256
  `d60c5ef67f473fb37afefe47e74430816ed25b0938f85889632f448a3b876cb7`
- fragment SPIR-V: 500 bytes, SHA-256
  `cd85f7f832d23a0700eab03801e7db793137f7a6fbee0c6de3dac1b82569b2e8`

The required recall queries—`bionic vulkan visible host black screen E016
dynamic rendering triangle Android native activity` and `bionic vulkan dynamic
rendering missing classic render pass fallback vkCmdBeginRendering`—returned
no indexed prior implementation. E016 reused E008-E010's visible Activity and
authenticated lifecycle control, E012's typed handles and batch validator, and
E015's exact six-record triangle shape.

Conclusion: the bridge has now translated a newer game-side Vulkan rendering
shape onto the tablet's older Android driver and presented a visible GPU-drawn
triangle. This proves Bionic-side compatibility lowering, not yet the complete
cross-process path: the Activity constructed the batch locally. The next gate
is to transfer the E015 glibc-generated batch into this visible executor while
preserving batching, handle ownership, and explicit synchronization.

## E017 — authenticated cross-libc visible-ingress preflight (2026-08-18)

Status: passed below the Android surface boundary at commit `72890aa`.

Hypothesis: the exact E015 generated glibc dispatch can encode the triangle in
a shared region, pass its sealed memfd to a persistent Bionic receiver through
an abstract Unix socket, authenticate both control messages with a fresh
256-bit capability, and receive completion without per-command socket RPC.

Method: a glibc executable resolves the same six real Vulkan command
signatures from `libvulkan-bvb-glibc.so`, records typed command-buffer,
image-view, and pipeline IDs into a 4 KiB memfd at offset 64, applies grow and
shrink seals, and sends one 72-byte setup packet with `SCM_RIGHTS` followed by
one 80-byte execute packet. The reusable Bionic ingress worker checks the
token in constant time, verifies the regular-file size and seals, maps it
read-only, validates generation, sequence, bounds, batch header, and every
record, then exposes the batch through a renderer-thread claim/complete API.
The client does not receive execute success until the consumer calls complete.

Result: all 14 Linux-host tests passed, including a wrong-token connection
followed by a valid connection and a focused ASan/UBSan client/worker test. All
12 tests available in the Android/Bionic build then passed on the tablet. The
actual ARM64 glibc producer connected to the actual Bionic receiver on the
tablet, transferred and independently decoded a 200-byte six-command batch,
and completed its synchronous execute round trip in 249,636 ns (about 0.250
ms). Both processes reported sequence 1 and empty receiver stderr.

The first preflight attempt also found a packaging defect: `grun -s` consumed
the intended `$ORIGIN` runpath and emitted an empty dynamic tag. The builder
now writes and asserts the explicit Termux glibc output path; `readelf` confirms
that runpath plus dependencies on the generated dispatch, `libc.so.6`, and the
AArch64 glibc loader.

Artifact identities are recorded in
`docs/evidence/e017-cross-libc-visible-ingress.json`. APK v7 (version 0.1.6)
also compiles and signature-verifies with the ingress receiver and ANR-safe
renderer worker, but remains deliberately uninstalled while the user is away.
It is staged as `/sdcard/Download/bvb-visible-host-v7.apk`; the installed APK
is still v5, and the E018 hardware script correctly refuses that stale version
before force-stop or launch.

The required recall queries—`Android cross UID abstract Unix socket SCM_RIGHTS
memfd NativeActivity Termux service Vulkan batch` and `Termux grun FEX glibc
ORIGIN runpath shared library not found`—returned no indexed implementation.
E017 reuses E012-E015's fixed-width batch, typed handles, shared-memory
validation, and generated dispatch rather than introducing a second encoding.

Conclusion: the cross-libc batch boundary is now real and sub-millisecond for
this synchronous triangle proof. It does not yet claim a visible external
frame. E018 is the prepared hardware gate: install v7, launch it with the
socket capability, replay this glibc-owned batch through the already visible
render-pass backend, and retain the authenticated lifecycle and E018 logs as
evidence.

## E018 — Android cross-UID abstract-socket denial (2026-08-19)

Status: failed at the transport boundary on hardware; renderer execution was
not reached.

Hypothesis: APK v7 can reuse E017's abstract Unix socket, `SCM_RIGHTS`, and
sealed memfd path when the Bionic receiver runs inside the visible Android
Activity under its own application UID.

Method: installed signature-verified APK v7, manually force-stopped it to
guarantee a fresh process, launched it with fresh lifecycle and ingress
capabilities, waited for authenticated window creation, then ran the E017
Termux-glibc triangle client with the Activity's native 2800x1752 dimensions.
The client was instrumented to distinguish shared-region, connect, setup, and
execute failures. All 14 host contracts passed before the hardware attempt.

Result: Activity PID 16205 authenticated created, started, resumed,
window-created, and focused events. The glibc client then returned
`stage=connect_abstract: Permission denied (-13)`. No setup packet crossed the
boundary and no renderer work began. This isolates the failure from token
verification, memfd validation, Vulkan replay, and presentation. E017 passed
only because its Bionic receiver was launched from Termux and therefore shared
Termux's Android UID; moving the same receiver into the APK changed the Android
security boundary.

Evidence is recorded in
`docs/evidence/e018-visible-cross-uid-ingress.json`. The local recall queries
`Termux am wrapper unknown force-stop use am start -S`, `Android Termux
NativeActivity abstract Unix socket EACCES cross UID SELinux token mismatch`,
and `Termux grun FEX glibc ORIGIN runpath shared library not found` returned no
matching prior solution.

Conclusion: cross-libc correctness and sub-millisecond same-UID batching remain
valid, but an abstract Unix socket cannot be the direct APK/Termux boundary on
this stock device. The next A/B gate will keep the authenticated batch and
renderer unchanged while copying the small proof batch over loopback TCP. A
subsequent production path must use an Android-supported descriptor broker
(for example Binder/`ParcelFileDescriptor`) to recover shared-memory batching;
the inline TCP gate is diagnostic, not the final high-throughput design.

## E019 — visible glibc-to-Bionic replay across Android UIDs (2026-08-19)

Status: passed on hardware at commit `b50b85c`.

Hypothesis: E018 failed specifically because Android denied its cross-UID Unix
socket, not because the glibc-generated batch, capability authentication,
Bionic executor, or Vulkan presentation path was invalid. Replacing only that
transport with loopback TCP and an inline proof batch should reach the existing
renderer.

Method: protocol opcode 10 carries a 32-byte launch capability followed by the
unchanged self-describing command batch, bounded by the existing 4 KiB packet
limit. The Activity binds only `127.0.0.1` on a fresh port. Its ingress worker
performs constant-time token comparison and the full existing batch validation,
then uses the same wait/claim/complete synchronization consumed by E016-E018.
The glibc client adds a mutually exclusive `--tcp-port` mode; its abstract Unix
and sealed-memfd mode remains covered separately. A wrong-token TCP connection
is rejected before a valid connection in the host contract.

Result: all 14 host tests and all 12 tests available in the tablet's Bionic
build passed. Signed APK v8 (version 0.1.7) launched as PID 19204 and emitted six
authenticated lifecycle events with zero rejections: created, started, resumed,
2800x1752 window-created, focused, and renderer-ready. The real ARM64 glibc
client sent one 256-byte packet containing the exact 200-byte, six-command,
sequence-1 triangle batch. Bionic validated, replayed, submitted, presented,
completed the client response, and reported state flags 63. The synchronous
end-to-end round trip was 14,618,177 ns (about 14.6 ms); client stderr was empty.

Exact state and artifact hashes are recorded in
`docs/evidence/e019-visible-loopback-inline.json`. Android still prevents the
Termux UID from reading this app's logcat, so the independently authenticated
lifecycle completion is retained as the machine-readable pass gate. E019
reuses E017's generated dispatch and batch format plus E016's compatibility
render-pass executor. The required recall searches found no matching prior
cross-UID solution.

Conclusion: this is the first complete external replay: real glibc-generated
Vulkan commands crossed the stock Android application boundary and were
presented by the Bionic/Adreno host at native tablet resolution. Inline TCP is
an intentionally bounded correctness bridge, not the final game transport.
The next performance gate is an Android descriptor broker that hands shared
memory to a Termux-UID Bionic shim, which can then pass that descriptor to the
glibc/FEX side locally with `SCM_RIGHTS`. That restores zero-copy batching while
respecting Android's cross-UID security model.

## E020 — cross-UID Binder shared-region delivery (2026-08-19)

Status: passed on hardware at commit `25bad27` with signed APK v10 (0.1.9).

Hypothesis: Android Binder can transfer a `ParcelFileDescriptor` from the
visible-host APK UID to a Termux-UID helper even though SELinux denied E018's
direct cross-UID Unix socket. Once the descriptor is in a Termux process, the
already-proven E017 same-UID Unix transport can relay it to glibc/FEX.

The first implementation used an exported `ContentProvider.openFile`. It
isolated three independent bootstrap boundaries rather than passing the gate:
raw `app_process` initially lacked a prepared main `Looper`; a synthetic system
context then identified its operation package as `android` instead of
`com.termux`; and the corrected package context was still rejected because its
`IApplicationThread` was not an AMS-registered application process. Android's
privileged external-provider route was also unavailable to an ordinary Termux
UID. These are retained as failed observations rather than treated as a
working transport.

The passing implementation reuses Termux:X11's established callback shape. A
raw Termux `app_process` creates a Binder callback, embeds that Binder in a
package-targeted broadcast, and sends the broadcast through an intent sender
owned by the real `com.termux` package. The registered visible-host receiver
validates the fresh 256-bit lifecycle capability in native code, creates a
sealed 4 KiB memfd, and writes its `ParcelFileDescriptor` into a synchronous
Binder callback transaction. Binder installs the descriptor directly in the
Termux helper; no provider acquisition, privileged permission, filesystem
exchange, or TCP payload copy is involved.

Result: a deliberately wrong capability returned `-EACCES` (`-13`). The valid
request delivered exactly 4,096 bytes and the helper validated
`BVB_E020_SHARED_REGION binder_parcel_fd=PASS`. Both helper stderr artifacts
were empty. The authenticated Activity event history reached a renderer-ready
2,800 x 1,752 window with zero rejected lifecycle events; the final status
snapshot correctly records that the Activity later stopped and lost its
window, so the gate validates event history rather than transient final focus.

Evidence and artifact identities:

- evidence: `docs/evidence/e020-binder-fd-gate.json`, 1,924 bytes, SHA-256
  `ace84aa3445f8f63dad8fbd295262c48e08e71c697f36dd2281c83393f81c5c2`;
- signed v10 APK and staged helper: 49,577 bytes, SHA-256
  `6c24d6a0d4a27062219beea1078501c0d9d2a761c740774268e8900a6c2ce32e`;
- valid and wrong-token stderr: empty, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.

The required recall searches returned no indexed provider or Binder-relay
implementation. E020 reused E016's visible renderer and lifecycle capability,
E017's sealed-region format and same-UID descriptor plan, and the Binder
callback mechanism from pinned Termux:X11 commit `139f219`. The next gate is
E021: detach the Binder-delivered descriptor and relay it once over a
Termux-owned Unix socket with `SCM_RIGHTS` to the real glibc client.

## E021 — Binder-to-glibc same-UID descriptor relay (2026-08-19)

Status: passed on hardware at commit `076d207` using the E021 harness added at
`58c0e34`.

Hypothesis: after E020 installs a memfd in a Termux-UID Java helper, an abstract
Unix socket is valid again because both the helper and the real glibc consumer
have Android UID 10469. Passing that descriptor once with `SCM_RIGHTS` should
preserve a writable shared mapping without copying its 4 KiB contents through
Java or TCP.

Method: the optional relay mode keeps E020's wrong-capability control, Binder
callback, and Activity lifecycle gate. A new Termux-glibc consumer binds an
unpredictable abstract socket, then authenticates the Java helper with
`SO_PEERCRED`. Java attaches the Binder-delivered descriptor to one
`LocalSocket` write containing the existing version-1 hello packet. The glibc
consumer uses `bvb_transport_receive_fd`, validates the fixed-width header,
peer UID, 4,096-byte regular-file size, grow/shrink/seal seals, absence of a
write seal, exact marker, and a read/write shared mapping. It sends the normal
fixed-width response only after all checks pass.

Result: the wrong capability again returned `-EACCES`. The valid helper relayed
the descriptor and received its ACK in 1,602,656 ns, including abstract-socket
connect, packet/FD send, validation, and response. The glibc consumer's
receive-plus-validation interval was 178,750 ns. It observed peer UID 10469,
seals value 7, the exact 4 KiB region, and a writable mapping. Helper, relay,
and wrong-token stderr were all empty. These timings isolate descriptor relay;
they do not include Vulkan execution or presentation and therefore are not an
FPS result or a direct comparison with E019's complete visible-frame latency.

Evidence and artifact identities:

- evidence: `docs/evidence/e021-binder-glibc-relay.json`, 2,174 bytes,
  SHA-256
  `8d46f6384b9fd4f15066a4dd2a9e5bf9e3174559b0fdea272af48fc10b1919d3`;
- helper APK: 49,577 bytes, SHA-256
  `5e20f7f7837e869954f737055007e33ac2e26dc9f32150690fb8377b373b3775`;
- real glibc relay consumer: 74,848 bytes, SHA-256
  `c0eff9f47ce77c1e7d562c8c1254c5cdcdfa64277435a52e503c4408fc30e3b5`,
  requesting Termux glibc's AArch64 loader;
- all three stderr artifacts: empty, SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.

The required recall query returned no indexed E021 implementation. This gate
reused E017's fixed-width `SCM_RIGHTS` transport and peer authentication,
E020's Binder callback broker, and the existing protocol encoder/decoder. The
next gate is E022: place the generated triangle batch in the Binder-delivered
mapping, replay it through the visible Bionic renderer, and compare complete
shared-path latency against E019's inline control.

## E022 — Binder-brokered shared visible replay (2026-08-19)

Status: passed twice consecutively on hardware with signed APK v12 (0.1.11).

Hypothesis: E019's visible loopback path can retain its authenticated control
plane while replacing the 200-byte inline batch with a 56-byte execute record
that points into E020/E021's Binder-brokered shared mapping. This should remove
the per-frame payload copy without changing the generated six-command workload,
Bionic validator, Vulkan replay, or presentation boundary.

Method: the visible host retains a read-only mapping when its Binder receiver
returns the sealed 4 KiB memfd. The real Termux-glibc relay receives the same FD
through same-UID `SCM_RIGHTS`, maps it read/write, and calls the exact builder
shared with E019. It writes the 200-byte batch at offset 64, applies a release
fence, sends authenticated opcode-9 generation/offset/length/sequence metadata
over loopback, and waits for the response. The Activity responds only after
validation, command replay, `vkQueuePresentKHR`, `vkQueueWaitIdle`, and ingress
completion. A wrong 256-bit capability still returns `-EACCES` before any FD is
delivered. The required recall query found no indexed E022 implementation; the
gate reused E019's visible renderer and E021's Binder/SCM_RIGHTS ownership chain.

Result: both v12 runs rendered at the authenticated 2,800 x 1,752 window with a
4,096-byte writable shared mapping, seals value 7, a 200-byte batch, six
commands, and no rejected lifecycle events. Run 1 measured 536,614 ns for FD
receive/validation, 10,224,687 ns for execute-to-present, 12,105,625 ns from FD
receive through present, and 13,171,145 ns for Java's full relay round trip. The
canonical consecutive run measured 673,281 ns, 13,586,771 ns, 16,723,906 ns,
and 17,301,145 ns respectively. Against E019's comparable 14,618,177 ns inline
execute round trip, E022 was 30.05% faster on run 1 and 7.06% faster on run 2.
The full broker interval varied from 9.90% faster to 18.35% slower, identifying
Binder/connect setup as a separate one-shot cost rather than hiding it inside
the graphics measurement.

The first retry exposed `-EALREADY`: Android stopped the Activity but retained
its PID and native globals, so the previous token, listener, and mapping
survived. APK v12 resets and destroys stale ingress at the next Activity
configuration. The immediately consecutive passing run used the same Activity
PID, proving restart repeatability without reinstalling or relying on shell
force-stop permission. The final status snapshot records the later stopped
window; it is not a render failure because the opcode-9 response had already
acknowledged present and queue idle. Android denied readable app logcat and the
Termux UID produced no `screencap` file, so no screenshot is claimed.

Evidence and artifact identities:

- canonical evidence: `docs/evidence/e022-brokered-visible-gate.json`, SHA-256
  `6bc2255a2526de5a20bd731232fec7861f92e9e419b3471c5f95a0b6930adbef`;
- signed v12 APK: 49,577 bytes, SHA-256
  `cb12988a7fa5d56989d25616c4138e6dd292d8c356d2425c51203d89ba56d715`;
- standalone glibc relay: SHA-256
  `d213ce94e932a3121ed61dbc490fc23791841550d8e7a8471c03dd3821e97a33`,
  requesting Termux glibc's AArch64 loader and only `libc.so.6` plus that loader;
- all 15 host protocol, dispatch, ingress, relay, Vulkan, and bridge contracts
  passed before the hardware gate.

Conclusion: the Android-supported, zero-copy command-batch path is now complete
for one visible frame and is at least as fast as the inline control in both
measured execute-to-present runs. E023 should retain the mapping and connection
across a frame ring, add explicit producer/consumer synchronization, and report
a multi-frame latency distribution before expanding dispatch toward DXVK.

## E023 — Persistent visible frame ring (2026-08-19)

Status: passed twice consecutively on hardware with signed APK v13 (0.1.12).

Hypothesis: E022's Binder-brokered mapping and authenticated metadata channel
can stay alive across many visible frames. A small rotating shared-memory ring
with an acknowledgement after presentation should prove safe slot ownership,
remove per-frame Binder and TCP connection setup, and expose a steady-state
latency distribution at the tablet's native resolution.

Method: the sequence-aware triangle builder writes a new 200-byte six-command
batch into one of four 256-byte slots beginning at offset 64 in the existing
sealed 4 KiB memfd. The real glibc relay keeps one loopback connection open for
64 opcode-9 execute records. It does not reuse a slot until Bionic validates,
replays, submits, calls `vkQueuePresentKHR`, waits for the queue, and returns the
matching response. The ingress explicitly marks itself ready for the next frame
before completing the current response, avoiding a scheduler-dependent gap.
APK v13 keeps its Vulkan instance, device, queue, swapchain, render pass,
pipeline, command pool, and semaphores alive; it cycles the acquired image view,
framebuffer, and command buffer per frame. `bvb_visible_frames` is opt-in and
the default remains the proven one-frame path. The canonical harness is
`scripts/test-visible-frame-ring-termux.sh`.

Result: both runs completed all 64 monotonically sequenced frames, wrapped all
four slots repeatedly, rejected the wrong 256-bit capability with `-EACCES`,
and presented through the authenticated 2,800 x 1,752 Android window with zero
rejected lifecycle events. Run 1 measured 1.462 ms minimum, 16.488 ms p50,
18.958 ms p95, 16.069 ms mean, and 27.674 ms maximum execute-to-ack latency;
its 64-frame interval was 1.028 s. The canonical repeat measured 1.671 ms,
16.815 ms, 19.352 ms, 16.329 ms, and 38.773 ms respectively, over 1.045 s.
The combined mean is 16.199 ms, or about 61.7 serialized acknowledgements per
second. The distribution is dominated by the FIFO/vsync presentation boundary,
so it demonstrates sustained native-resolution replay but is not a Tomb Raider
FPS result. E019's one-shot 14.618 ms result is not a throughput control because
a single frame can land at a different point in the display cycle.

This ring currently permits only one in-flight frame: the producer deliberately
waits for the consumer acknowledgement before issuing the next sequence. That
is sufficient to prove ownership and safe wraparound, but it does not yet test
pipelined frames or eliminate the per-frame Android image-view/framebuffer/
command-buffer churn. The final lifecycle snapshot is taken after the test
window has stopped; successful opcode-9 responses had already acknowledged all
64 presents. Android again denied readable app logcat and Termux produced no
screenshot, so no image is claimed.

Evidence and artifact identities:

- canonical repeat evidence:
  `docs/evidence/e023-brokered-visible-gate.json`, 2,773 bytes, SHA-256
  `67a243e5719a8c1d61f7e7777f5eacb4b0a40e4d32ff9c0126750a679d321055`;
- signed v13 APK: 49,577 bytes, SHA-256
  `8f83fd6d7158064e820b2a926affa12ceed5abd63b9e20569e7e0288b81ca0be`;
- standalone glibc relay: 78,344 bytes, SHA-256
  `cab8177389a1db459fa6262b4cfee5a12c8c7b92541cbf60322de515a12abca9`;
- all 15 normal-host contracts and all 13 contracts available under Termux
  ARM64 passed. Termux Python's missing `os.memfd_create` wrapper is covered by
  a test-only libc fallback.

The required recall query found no indexed E023 implementation or Termux
`memfd_create` workaround. This gate reused E022's visible renderer and retained
mapping, E021's Binder/`SCM_RIGHTS` descriptor chain, and their authenticated
metadata protocol. E024 should expand generated dispatch from the six-command
triangle subset toward the measured DXVK startup entry-point set while keeping
this persistent transport as the regression baseline.

## E024 — Generated measured DXVK dispatch policy (2026-08-19)

Status: passed on normal Linux, Termux ARM64, and the real Termux glibc target;
the E023 visible 64-frame hardware regression also passed.

Hypothesis: E011's observed DXVK startup inventory can become an exhaustive,
generated runtime policy before the bridge implements more Vulkan semantics.
This should make every lookup classification inspectable and testable while
preventing the loader from receiving a callable pointer for a command the
bridge cannot execute.

Method: `scripts/generate-dxvk-dispatch-policy.py` joins the pinned E011
742-name manifest with the generated E015 triangle dispatch table. It emits a
C include consumed by `src/dxvk_dispatch_policy.c` and a JSON summary. Every
entry records its Vulkan dispatch scope, whether it resolved during E011, and
one of three support states: `executable`, `required_unimplemented`, or
`probed_null`. The runtime resolver returns a pointer only for executable
entries. The expanded dispatcher test queries all 742 observed names and
asserts both the classification totals and resolver behavior. The canonical
Termux harness builds and runs the shared object and test binary with the real
AArch64 glibc interpreter, records dynamic exports, and emits the evidence JSON.

Result: the policy contains all 742 observed names: 4 global, 101 instance,
635 device, and 2 Wine-private lookups. E011 resolved 440 of them. Eight names
are executable aliases for the existing six-command triangle path; the other
432 resolved names return null because their semantics remain unimplemented.
All 302 names that probed null in E011 also return null. The glibc executable
queried every entry and observed exactly 8 non-null, 432 required-but-null, and
302 probed-null results. Its stderr was empty. This is an honest capability
boundary, not a claim that DXVK can execute through the bridge yet.

All 16 normal-host tests passed. All 14 tests available on Termux ARM64 passed,
and the standalone glibc dispatcher test passed under
`ld-linux-aarch64.so.1`. The policy-aware E023 hardware regression then
presented all 64 native 2,800 x 1,752 frames through four persistent slots. It
measured 1.543 ms minimum, 16.654 ms p50, 19.539 ms p95, 16.239 ms mean, and
29.887 ms maximum execute-to-ack latency over 1.039 s. That matches the earlier
vsync-paced E023 range and detects no transport regression from policy lookup.

Evidence and artifact identities:

- canonical evidence: `docs/evidence/e024-dxvk-dispatch-policy.json`, 3,023
  bytes, SHA-256
  `ff17d8c2991c2de57f74326c9532f62fd2b75a8f90d887db5e6a941d34456045`;
- generated policy include: 117,745 bytes, SHA-256
  `fba18fa35304927d05491fe3bd3210bb53c6b6224509ea20fecc599f7813498a`;
- generated policy summary: 1,018 bytes, SHA-256
  `323624b49a5b34170caea626ba5f27c37eef18ec9ac18e6e7b695ad11b579bad`;
- glibc shared library: 139,936 bytes, SHA-256
  `217c617db2c1ba9a3b72868a941aaeba1f36481ce952af41fa3d8d38e7214aeb`;
- glibc test executable: 74,912 bytes, SHA-256
  `e38783e696559eba994075cc9a9aee71fb826bef87232415b7231699cef19406`.

The required recall query—`DXVK startup measured Vulkan entry points generated
dispatch E011 E015 next subset E024`—found no indexed E024 implementation. This
gate reused E011's measured inventory, E015's executable dispatch, and E023's
persistent visible transport as its regression control. E025 should implement
the four observed global bootstrap calls (`vkCreateInstance`,
`vkEnumerateInstanceExtensionProperties`,
`vkEnumerateInstanceLayerProperties`, and `vkEnumerateInstanceVersion`) against
the Bionic control path, introduce proxy instance ownership, and expose only
extensions the bridge can actually support.

## E025 — Real glibc-to-Bionic Vulkan global bootstrap (2026-08-19)

Status: passed on the Galaxy Tab S8+ with the real Termux AArch64 glibc client,
Android Bionic service, and `/system/lib64/libvulkan.so`.

Hypothesis: the four global calls observed during E011 can execute across the
existing authenticated control boundary without claiming any extension or
layer semantics the bridge does not yet implement. A native instance can remain
owned by the Bionic connection while glibc receives only a typed proxy handle.

Method: protocol-v1 opcodes 11 and 12 add fixed-width global-info and
instance-create records. The Bionic context loads `vkGetInstanceProcAddr` from
the selected loader; executes `vkEnumerateInstanceVersion`,
`vkEnumerateInstanceExtensionProperties`, and
`vkEnumerateInstanceLayerProperties`; and caches both native inventory counts
and an intentionally empty exposed inventory. It sanitizes instance creation,
rejecting extensions, layers, flags, `pNext`, and custom allocation callbacks
that cannot cross the current bridge. Successful native instances enter E012's
typed handle table and are destroyed when the authenticated connection closes.

The glibc shared library now exports `vkGetInstanceProcAddr`, keeps one
same-UID authenticated Unix connection selected by `BVB_BRIDGE_SOCKET`, and
implements the four real Vulkan signatures. Its dispatchable instance proxy has
a stable dispatch anchor, a magic value, and the returned 64-bit typed ID. The
E025 executable-name input advances the generated E024 policy from 8 to 12
working semantic names. The other 428 resolved names and 302 originally-null
names still return null.

Result: the canonical hardware client used the Termux glibc interpreter while
the service used `/system/bin/linker64`. Android's loader reported Vulkan
1.4.0 (`4210688`). Both extension and layer enumeration returned an exposed
count of zero. Two valid `vkCreateInstance` calls returned proxy IDs
`0x0100000000000001` and `0x0100000000000002`: object type 1 (`INSTANCE`) with
serials 1 and 2. Client and service stderr were both empty, and both processes
exited successfully.

The first real-loader attempt failed before enumeration because the Bionic
context asked `vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroyInstance")`.
Android correctly returned null for that instance-scoped command, whereas the
host fake loader had permissively returned it and masked the error. Commit
`4171bc1` retained GIPA and resolves destruction only after a native instance
exists. The unchanged real-loader procedure then passed. This is evidence for
the four global calls and instance ownership; physical-device discovery,
instance-command dispatch, supported extensions, and DXVK execution remain
unimplemented.

Validation and artifact identities:

- all 17 normal-host contracts and all 15 contracts available under Termux
  ARM64 passed after the Android-specific correction; the real glibc/Bionic
  hardware gate passed at source commit `feade5e`;
- canonical evidence:
  `docs/evidence/e025-global-vulkan-bootstrap.json`, 3,496 bytes, SHA-256
  `e27a42248ecc0302a38becf972203e3453314d0729b616d7c962e9aa9ca081d0`;
- generated E025 policy summary: 1,268 bytes, SHA-256
  `adc5d1f5bc4bba61dbb45ff520b3fc1bdcea4ec48290ae50cd08a70805d3799c`;
- glibc `libvulkan-bvb` bridge: 143,080 bytes, SHA-256
  `efbf49ca7decc938d2e29ca618cb73c12b637b75cd027ac55c97a40c0afdd7ca`;
- glibc bootstrap client: 71,408 bytes, SHA-256
  `8b5ba663b0923071e9adbcad51e5f8cc2c31903ff0f4662654ab40f4a723932d`;
- Bionic bridge service: 62,608 bytes, SHA-256
  `27eefa8ab39aa8d7b46f9c3cd775ca947f01fed3255ac5bb0d63be9064dbc448`.

The required recall queries—`Vulkan global bootstrap vkCreateInstance
vkEnumerateInstanceExtensionProperties vkEnumerateInstanceLayerProperties
vkEnumerateInstanceVersion proxy instance Bionic bridge E025` and the broader
`vkCreateInstance proxy handle Vulkan bridge`—found no indexed prior
implementation. E025 reused E003's authenticated control discipline, E012's
typed handle IDs, E015's generated client-dispatch pattern, and E024's measured
truth table. E026 should add explicit `vkDestroyInstance` dispatch plus
`vkEnumeratePhysicalDevices`, returning connection-owned physical-device proxy
IDs without yet advertising unimplemented property or WSI calls.

## E026 — Stable physical-device proxies and explicit teardown (2026-08-19)

Status: passed on normal Linux, Termux ARM64, and the real Galaxy Tab S8+
glibc-to-Bionic Android Vulkan path.

Hypothesis: E025's connection-owned instance can enumerate native physical
devices without leaking Bionic pointers, return stable dispatchable proxies
across Vulkan's count/fill pattern, and explicitly destroy its native instance
and child ownership before connection teardown.

Method: protocol-v1 opcodes 13 and 14 carry a fixed-width instance ID and a
bounded variable-length physical-device response. The Bionic context expands
its E012 handle table to hold both instances and their physical-device children.
It resolves `vkEnumeratePhysicalDevices` from the native instance, limits the
result to eight devices, reuses an existing proxy ID when the same native handle
is observed again, and records the instance as parent. Destruction removes all
child mappings before removing and destroying the native instance.

The glibc library adds `vkEnumeratePhysicalDevices` and `vkDestroyInstance` to
instance-scoped GIPA. It maintains heap-backed type-2 proxies in a per-process
cache so repeated enumeration returns the same `VkPhysicalDevice` pointer for
the same wire ID. Successful destruction invalidates and frees every child
proxy and then the type-1 instance proxy. E026's checked-in executable list
advances the measured policy to 14 names; instance functions resolve only when
GIPA receives a valid instance proxy.

Result: the real Android loader again reported Vulkan 1.4.0. The count call
reported one physical device. The fill call and a repeated fill call returned
the same client proxy pointer and wire ID `0x0200000000000001`: object type 2
(`PHYSICAL_DEVICE`), serial 1, parented to instance
`0x0100000000000001`. The client then explicitly destroyed that instance and a
second instance. Client and service stderr were empty and both exited zero.
This proves identity and lifecycle only; the proxy still exposes no physical-
device properties, queue/memory inventory, extensions, logical device, or WSI.

All 17 normal-host contracts and all 15 contracts available under Termux ARM64
passed. The canonical real-glibc/Bionic gate passed at source commit
`cf1cf74`. The policy classifies 14 of 742 measured names executable, 426
resolved names required-but-unimplemented, and 302 observed null probes.

Evidence and artifact identities:

- canonical evidence:
  `docs/evidence/e026-instance-vulkan-bootstrap.json`, 3,784 bytes, SHA-256
  `25456a8c75b4c6d844dde39a800c69bc5a005819bd8c36c6d71025ec794ce175`;
- generated E026 policy summary: 1,327 bytes, SHA-256
  `762742ce0e60c7058e09ac8c41cda38be44baa3cb1240dd3decf7d3ea247a9f3`;
- glibc `libvulkan-bvb` bridge: 143,488 bytes, SHA-256
  `4452c2572ed483ab97fdc3545e384ec398bb59f76dc8e3827f8a3e5eda980e69`;
- glibc bootstrap client: 71,472 bytes, SHA-256
  `cca5c356adddd06e95dac38fc075b343c0ac2ea1c8d5cdf3318ed7570220c6c8`;
- Bionic bridge service: 65,544 bytes, SHA-256
  `57e7af46d25269201e93ea326a0de678631aba3f5277f6d7046ccfcfd2b40e10`.

The required recall query—`vkDestroyInstance vkEnumeratePhysicalDevices proxy
physical device stable handle Bionic bridge E026`—found no indexed prior
implementation. E026 reused E012's typed and parented handle table, E025's
persistent authenticated client connection, and E025's corrected instance-
scoped GIPA resolution. E027 should add the bounded read-only discovery calls
DXVK needs next: physical-device properties, queue families, memory properties,
and device-extension enumeration, without yet allowing logical-device creation.

## E027 — Full base physical-device discovery (2026-08-19)

Status: passed on real hardware from a Termux ARM64 glibc client through the
Android Bionic service at source commit `02dee34`.

Hypothesis: E026's stable, parented physical-device proxy is sufficient to
resolve and execute the four base discovery calls that DXVK looked up during
E011, provided that their results use a field-defined wire ABI rather than
copying native Vulkan C structures between libc implementations.

Method: E027 adds instance-scoped implementations of
`vkGetPhysicalDeviceProperties`,
`vkGetPhysicalDeviceQueueFamilyProperties`,
`vkGetPhysicalDeviceMemoryProperties`, and
`vkEnumerateDeviceExtensionProperties`. The Bionic side recovers both the
native physical device and its owning native instance from E026's handle table,
then resolves each command with that instance. Queue families are bounded at
64 and device extensions at 1,024. Extension records use 15-entry pages, so
each response stays below the protocol's 4 KiB maximum while the glibc wrapper
preserves Vulkan's ordinary count/list and `VK_INCOMPLETE` behavior.

A generator reads the project's pinned Khronos `vk.xml` and emits explicit
little-endian codecs for every leaf of `VkPhysicalDeviceProperties` (including
all limits and sparse properties), `VkQueueFamilyProperties`,
`VkPhysicalDeviceMemoryProperties`, and `VkExtensionProperties`. Native
structure layout never crosses the process boundary. The resulting property
record is 804 bytes; each queue record is 24 bytes; the full fixed-capacity
memory record is 456 bytes rather than the native 520 bytes because compiler
padding is omitted; and a two-record extension page is 536 bytes.

Result: the real Android loader reported Vulkan 1.4.0 and the physical device
reported Vulkan 1.1.128. The client received `Adreno (TM) 730`, vendor ID
20,803, device ID 117,637,121, two queue families, nine memory types, two
memory heaps, and 90 device extensions. Retrieving all extensions exercised
six 15-record pages. The physical proxy remained
`0x0200000000000001`; both instances were destroyed explicitly; client and
service stderr were empty. The generated E027 policy classifies 18 of the 742
measured names executable, 422 resolved names required-but-unimplemented, and
302 observed-null names unavailable.

All 18 host contracts and all 16 contracts available under Termux ARM64
passed. Canonical evidence is
`docs/evidence/e027-physical-device-discovery.json`, 4,447 bytes, SHA-256
`0f5804f74b3509d331cb85cf47c33c18dfc0b47e286e8b5bc943343ba0c6ceda`.
The canonical artifacts are:

- generated E027 policy: 1,499 bytes, SHA-256
  `b27e59befe72b1db56517aa197186b287bf31c8c8556629b16d27dbc6ca28020`;
- glibc `libvulkan-bvb` bridge: 210,352 bytes, SHA-256
  `5ff994a71f497f726ee2c4089d20962e23c9790a868df8640f21529f89f9a465`;
- glibc discovery client: 71,568 bytes, SHA-256
  `88af3e15e460db0c230dcd0b2b67c6c42b2eaac9e7c03220abba38b8c607949f`;
- Bionic bridge service: 81,024 bytes, SHA-256
  `56d52cc3ff8f8e8e20342bdd08867264650a6cd52e4e348ce951042705c2bf6d`.

The required recall query—`physical device properties Vulkan bridge`—found
the earlier E025/E026 session record. E027 reused E025's persistent
authenticated glibc-to-Bionic connection and E026's stable, parented physical-
device handles. The next bounded gate should add the feature-query surface and
constrained logical-device/queue creation required before game-facing command
submission; WSI remains unavailable until its semantics are bridged.

## E028 — Typed logical device and queue (2026-08-19)

Status: passed on real hardware from a Termux ARM64 glibc client through the
Android Bionic service at source commit `ade25a8`.

Hypothesis: E027's full base discovery and stable type-2 physical proxy can
support a minimal game-facing native device lifecycle without yet exposing
extensions, feature chains, WSI, or resource commands. A one-queue device is a
small enough gate to validate ownership and device-scoped dispatch separately
from command submission.

Method: E028 adds a 220-byte generated fixed-width codec for every member of
`VkPhysicalDeviceFeatures` and makes `vkGetPhysicalDeviceFeatures` executable.
`vkCreateDevice` accepts exactly one queue-create record, validates the family
and count against a fresh native queue inventory, and rejects layers,
extensions, enabled base features, `pNext` chains, allocators, and nonzero
flags. The 32-byte create request transports the physical ID, queue family,
count, and exact IEEE-754 priority bits. The Bionic owner creates a native
device and returns a type-3 proxy; `vkGetDeviceQueue` returns a cached type-4
proxy for the native queue. `vkGetDeviceProcAddr` validates a device proxy
before exposing `vkDestroyDevice`, `vkGetDeviceQueue`, or the already-
executable triangle command subset.

Device metadata records retain the created queue family/count. Explicit device
destruction removes queue children before the device and calls the native
destroy function. Instance teardown also destroys any remaining descendant
devices before removing physical-device children, while context teardown keeps
the same device-before-instance ordering.

Result: the real Adreno base features reported sampler anisotropy support. The
glibc client created logical-device proxy `0x0300000000000001` (type 3,
serial 1) using one queue from the validated family. Two queue lookups returned
the same client pointer and queue proxy `0x0400000000000001` (type 4,
serial 1). The client explicitly destroyed the logical device and both test
instances. Client and service stderr were empty. The E028 policy classifies 23
of 742 measured names executable, 417 resolved names required-but-unimplemented,
and 302 observed-null names unavailable.

All 18 host contracts and all 16 contracts available under Termux ARM64
passed. Canonical evidence is `docs/evidence/e028-logical-device.json`, 5,021
bytes, SHA-256
`2ccf3e0de86afecd206566e3172bfd7e190f6acb7012d641c4d2adcfef93c171`.
The canonical artifacts are:

- generated E028 policy: 1,630 bytes, SHA-256
  `e749c31663d5b51afcab5f6dcadec588329379b6fc0fd4740e712a1bf09fe2b5`;
- glibc `libvulkan-bvb` bridge: 211,544 bytes, SHA-256
  `e2430cfca18eb7d1c61c5ee4cca7ae88254403565f5ed587333404865918c833`;
- glibc logical-device client: 71,720 bytes, SHA-256
  `d96bce6505e180bb2ee772021e57860158e103dad503961ab005362ceea9ecb1`;
- Bionic bridge service: 90,040 bytes, SHA-256
  `9f1a8a36dd09d45ecd80592bf8f931513765db260a35cf336e68cc08ed63db9f`.

The required recall queries found no indexed E028 bridge implementation. The
only `vkCreateDevice` recall was the existing direct-stack control, so E028
reused the repository's proven native self-test sequence together with E025's
persistent connection and E026/E027's parented physical-device ownership. The
next bounded gate should bridge ordinary command-pool/buffer submission and
synchronization through these device/queue proxies; this gate does not claim
DXVK resource creation, WSI, animation, or an FPS change.

## E029 — Empty queue submit and idle synchronization (2026-08-19)

Status: passed on real hardware from a Termux ARM64 glibc client through the
Android Bionic service at source commit `98faeae`.

Hypothesis: E028's typed device and queue proxies can carry ordinary
device-dispatch traffic to the native Adreno driver before the bridge has
command-buffer, fence, or semaphore objects. Vulkan permits a zero-count
`vkQueueSubmit`, making it the smallest honest queue-execution gate; queue and
device idle calls then prove device-scoped synchronous result transport.

Method: protocol opcodes 23–25 transport a queue or device proxy ID and return a
fixed-width signed `VkResult`. The Bionic owner resolves the queue's parent
device, loads `vkQueueSubmit`, `vkQueueWaitIdle`, and `vkDeviceWaitIdle` through
the native device proc-address function, and invokes them on the owned native
handles. The glibc dispatch library exposes those names only for a valid device
proxy. `vkQueueSubmit` accepts only `submitCount == 0`, a null submit array, and
a null fence; it returns `VK_ERROR_FEATURE_NOT_PRESENT` locally for all
non-empty or fenced work. This boundary is deliberate: E029 transports no
pointers and does not claim command-buffer or synchronization-object support.

Result: the real Adreno 730 returned `VK_SUCCESS` for the empty queue submit,
queue idle wait, and device idle wait. A non-empty submission was rejected on
the client side as designed, so it never crossed the protocol. The logical
device and stable queue retained E028's type-3/type-4 IDs, then underwent
explicit teardown. Both client and service stderr were empty. The E029 policy
classifies 26 of 742 measured names executable, 414 resolved names
required-but-unimplemented, and 302 observed-null names unavailable.

All 18 host contracts and all 16 contracts available under Termux ARM64 passed.
Canonical evidence is `docs/evidence/e029-empty-submit.json`, 5,284 bytes,
SHA-256
`a11c8483133cf254e4d5caa386514ed177e4876d5e40107b6da44200d75d2616`.
The canonical artifacts are:

- generated E029 policy: 1,698 bytes, SHA-256
  `39d702bf6caec788ca9f5013e36ab67ce9c5e09cd33948e1ce59279f8a947ecc`;
- glibc `libvulkan-bvb` bridge: 211,864 bytes, SHA-256
  `6081fe2814d9de12ae1790503a7c572787ad12e0e3a2dc4ea9632750a73b4aec`;
- glibc integration client: 71,720 bytes, SHA-256
  `ec2b52f24db09eb94d2d60ba0c41cf53ef86f9fbe0a73212cd2482a20251ec2e`;
- Bionic bridge service: 92,680 bytes, SHA-256
  `48d15e8d95e7a18f78b2989c086940ca129e62fa85da5e0c213f66d96de5ebfc`.

The required `deja "vkQueueSubmit vkQueueWaitIdle device proxy bridge"` recall
query found no earlier bridge implementation. E029 reused E025's persistent
authenticated connection, E026–E028's parented proxy-handle model, and the
repository's older native/fake Vulkan queue-submit and idle implementations.
The next bounded gate is command-pool and command-buffer ownership followed by
one real non-empty submission. E029 does not claim DXVK resource creation, WSI,
game-facing animation, or an FPS change.

## E030 — Typed command buffer and non-empty submit (2026-08-19)

Status: passed on real hardware from a Termux ARM64 glibc client through the
Android Bionic service at source commit `5b33ae6`.

Hypothesis: E029's device and queue proxies can own an ordinary native command
pool and primary command buffer across the libc boundary. One begun-and-ended
buffer with no recorded GPU commands is the smallest valid payload for a
non-empty `vkQueueSubmit`; it separates command-object lifecycle and submit
ownership from resource and command encoding.

Method: protocol opcodes 26–33 use fixed-width, type-checked records for command
pool create/destroy/reset, one-buffer allocate/free, begin/end, and one-buffer
queue submit. Pools are type-10 children of devices; buffers are dispatchable
type-11 children of pools and retain their device ID on the glibc side. Submit
validates that queue and buffer share a device on both sides. The current
contract accepts one primary buffer, no inheritance data, at most the
one-time-submit usage flag, no semaphores, and no fence. Pool/device teardown
invalidates children before their parents.

Result: the real Adreno 730 created command-pool proxy
`0x0a00000000000001` and command-buffer proxy `0x0b00000000000001`.
`vkBeginCommandBuffer`, `vkEndCommandBuffer`, a one-buffer `vkQueueSubmit`,
`vkQueueWaitIdle`, and `vkResetCommandPool` all returned `VK_SUCCESS`. The
client explicitly freed the command buffer and destroyed the pool before
destroying the device and instances. Both stderr streams were empty. The E030
policy classifies 33 of 742 measured names executable, 407 resolved names
required-but-unimplemented, and 302 observed-null names unavailable.

All 18 host contracts and all 16 contracts available under Termux ARM64 passed.
Canonical evidence is `docs/evidence/e030-command-buffer.json`, 6,037 bytes,
SHA-256
`cbefbe5d64797a597a1e77e21cc4ed489ec2c79988571d22938f6cea94bb16a4`.
The canonical artifacts are:

- generated E030 policy: 1,893 bytes, SHA-256
  `91836567a1023c43b596165a29ec32f7344d9f6adc6ee3b9aeff92025801d262`;
- glibc `libvulkan-bvb` bridge: 213,976 bytes, SHA-256
  `5b797501f6281a93154b37f932b5c0f07430ed68fc0684869db35b541906e44b`;
- glibc integration client: 71,840 bytes, SHA-256
  `21d7636120a043676acbffdf84350e22a866d76b7276b2f6a980e7efc1b9aa87`;
- Bionic bridge service: 106,176 bytes, SHA-256
  `dc614f95f297a896898294b97bd4d805f1e5ec10614ec34065373c2e183d892f`.

The required
`deja "Vulkan command pool command buffer proxy vkAllocateCommandBuffers non-empty vkQueueSubmit Bionic bridge"`
query found no earlier cross-libc implementation. E030 reused the repository's
native self-test create/begin/end/submit order, the older native/fake Vulkan
command lifecycle, E025's persistent authenticated connection, and E026–E029's
parented proxy model. One host failure exposed and fixed a handle-table lookup
that omitted its required native-bits output before tablet deployment.

The next bounded gate is buffer/device-memory ownership and one deterministic
recorded GPU write through this command buffer. E030's submit array is genuinely
non-empty, but its command buffer contains no GPU commands; it does not claim
resource creation, rendering, game-facing animation, or an FPS change.

## E031 — Deterministic buffer fill and readback (2026-08-19)

Status: passed on real hardware from a Termux ARM64 glibc client through the
Android Bionic service at source commit `5ac7cde`.

Hypothesis: E030's command-buffer and submission path can own ordinary buffer
and device-memory resources, record one deterministic GPU transfer command,
and preserve the result across the libc boundary for native readback.

Method: protocol opcodes 34–41 add bounded create/destroy, requirements,
allocate/free, bind, command-fill, and verification records. The current
contract deliberately accepts only an exclusive transfer-destination buffer,
no allocation callbacks or `pNext` chains, and allocations up to 16 MiB. The
glibc test creates a 4 KiB buffer, selects compatible host-visible/coherent
memory, binds it, records `vkCmdFillBuffer` with word `0xa5c3f00d` followed by
a transfer-write-to-host-read barrier, submits and waits, then asks the Bionic
service to map and verify all 1,024 words. Buffer and memory IDs remain typed,
parented device children and are explicitly released.

Result: the real Adreno 730 created buffer proxy `0x1300000000000001` and
device-memory proxy `0x0900000000000001`, using memory type 6. Submission,
queue wait, command-pool reset, readback, and explicit teardown all succeeded.
Every one of 1,024 words matched, and both client and service stderr were
empty. The E031 policy classifies 40 of 742 measured names executable, 400
resolved names required-but-unimplemented, and 302 observed-null names
unavailable.

All 18 host contracts and all 16 contracts available under Termux ARM64 passed.
Canonical evidence is `docs/evidence/e031-buffer-fill.json`, 6,746 bytes,
SHA-256
`8ed3e99014865629a8f76875b3e11216ee17853f936bdb674eab27d5de676d51`.
The canonical artifacts are:

- generated E031 policy: 2,068 bytes, SHA-256
  `b1f6d5539e2ba405cf4e62745c3c1b855875ec3fb6fb7475671dd4c8c62c56a5`;
- glibc `libvulkan-bvb` bridge: 215,960 bytes, SHA-256
  `34e945ea2d30d65f6bc12db138a8cbc3db106b75886d0e6bc06688908d4ac675`;
- glibc integration client: 72,000 bytes, SHA-256
  `eb7819381cbbd948c6d68329f4c32aecded5610699c49b9a3098fdfe9b5e3993`;
- Bionic bridge service: 118,056 bytes, SHA-256
  `64b10b202c540b76646ec18df2d9012a263d1fa693eaf21112a6b1372bda8f72`.

The required
`deja "vkCreateBuffer vkAllocateMemory vkBindBufferMemory vkCmdFillBuffer glibc Bionic bridge"`
query found no earlier cross-libc implementation. E031 reused the repository's
proven native fill/barrier/map/readback sequence, E025's persistent
authenticated connection, and E026–E030's parented proxy-handle lifecycle.

The next bounded gate is to expand the generated dispatch from this proven
resource operation toward the measured DXVK startup subset. E031 does not yet
claim image or shader resources, WSI/presentation through this game-facing
path, DXVK startup, animation, an FPS result, or removal of Termux.

## E032 — Fence-backed asynchronous GPU submission (2026-08-19)

Status: passed on real hardware from a Termux ARM64 glibc client through the
Android Bionic service at source commit `1e06ab4`.

Method: fixed-width opcodes 42–47 add typed fence create/destroy, status,
one-fence wait/reset, and a fenced variant of E031's one-command-buffer submit.
The client and service both require the queue, command buffer, and fence to
share one device. The test creates an unsignaled fence, submits the 4 KiB
`vkCmdFillBuffer`, waits on the fence rather than idling the whole queue,
verifies all 1,024 words, resets the fence, and destroys every object.

Result: Adreno 730 reported `VK_NOT_READY` before submit, `VK_SUCCESS` after
submit and wait, and `VK_NOT_READY` after reset. Readback had zero mismatches;
both stderr streams were empty. All 18 host and all 16 Termux ARM64 contracts
passed. The E032 policy classifies 45 of 742 names executable, 395
required-but-unimplemented, and 302 observed-null.

Canonical evidence is `docs/evidence/e032-fence-submit.json`, 7,372 bytes,
SHA-256
`c01dc3b12143db9edcb96965503a128b07e5aa01a195706be96edee53d560119`.
The required recall query found no earlier fence-backed cross-libc bridge;
E032 reused E025's persistent connection and E026–E031's parented handles,
command recording, and deterministic fill/readback.

The next visual gate adds a per-frame rotation push constant to the existing
shared frame ring. E032 does not yet claim mapped-memory sharing, general
semaphore support, DXVK startup, or a game FPS change.

## E033 — Bridged per-frame rotating triangle (2026-08-19)

Status: passed on real Adreno 730 hardware from a Termux ARM64 glibc producer
through the Android Bionic visible host at source commit `90f087d`.

Hypothesis: E023's reusable four-slot shared frame ring can carry changing
game-side data every frame, and the Bionic host can replay it through a real
Vulkan push constant without deriving animation locally.

Method: command-batch opcode 9 adds a fixed 16-byte record containing a typed
pipeline-layout ID, an angle, and the native-window aspect ratio. Generated
glibc `vkCmdPushConstants` dispatch writes a deterministic angle based on the
batch sequence: one revolution every 600 frames. Bionic validates and resolves
the layout ID, then pushes both floats into an aspect-correct vertex shader.
Each frame contains seven commands and occupies 224 bytes of its 256-byte ring
slot. The Activity received 4,096 frames; the initial 10-second helper timeout
failure was reproduced, traced to the outer SCM-rights completion wait, and
fixed with a bounded five-minute helper timeout. Per-frame renderer timeouts
remain 10 seconds.

Result: the user visually confirmed continuous rotation, and the complete
native-resolution 2800x1752 run processed sequences 1 through 4,096 in
68.443 seconds. Mean throughput was 59.8 FPS; per-frame round-trip timing was
1.703 ms minimum, 16.657 ms median, 18.869 ms p95, 16.670 ms mean, and
27.354 ms maximum. Wrong-capability access returned `-EACCES`, all four ring
slots were reused, and relay, service, and valid-helper stderr were empty. All
18 host contracts and all 16 Termux ARM64 contracts passed. The E033 policy
classifies 46 of 742 names executable, 394 resolved names
required-but-unimplemented, and 302 observed-null.

Canonical evidence is `docs/evidence/e033-rotating-triangle.json`, 2,773
bytes, SHA-256
`24e573c19181a4725dfbd67dcfecbe4435ffbacfd4b72d9bc313de2e77093407`.
The canonical artifacts are:

- generated E033 policy: 2,205 bytes, SHA-256
  `b8b975d3331594fb14fd3eedc35b852c2ed0797ee60a54f97c8447c641f858b2`;
- glibc shared-region relay: 219,656 bytes, SHA-256
  `5ae38fc76a65b100c1fbe017c498dbdf3438dbc6148ab1391a3fdd8d7833615f`;
- signed Bionic visible-host APK: 53,673 bytes, SHA-256
  `89ec99246a886c73931e350873cb731e8381891b02009d829b620dc4f478bfd8`.

The required recall query found no earlier rotating-triangle bridge or timeout
fix. E033 reused E023's persistent frame ring, E025's authenticated connection,
and E026–E032's typed handles, generated dispatch, command recording, and
fence-backed resource path.

The next bounded gate is client-visible mapped upload memory, proven by moving
triangle vertex data out of the shader and into a glibc-populated Vulkan vertex
buffer. E033 does not yet claim general mapped-memory coherence, image or
shader creation through the game-facing control path, descriptor sets,
indexed drawing, DXVK startup, or a game FPS change.

## E034 — Game-facing mapped-memory correctness (2026-08-19)

Status: passed on real Adreno 730 hardware from a Termux ARM64 glibc client
through the Android Bionic service at source commit `61c1b98`.

Hypothesis: the game-facing proxy can expose Vulkan's map/flush/invalidate/
unmap lifecycle without sharing process pointers across the glibc/Bionic
boundary, and a byte pattern written by glibc can reach and return from the
real Adreno allocation exactly.

Method: fixed-width protocol opcodes 48 and 49 add bounded memory write and
read operations. `vkMapMemory` allocates a client-side shadow and initializes
it from native memory in chunks of at most 4,072 bytes. Flush copies the
selected shadow range into the Bionic-owned allocation; invalidate copies the
native range back. Bionic transiently maps the complete allocation for each
chunk and performs whole-mapping cache maintenance when the selected memory
type is not host-coherent. Queue submit conservatively uploads every still-
mapped shadow owned by that queue's device before submitting work. This is an
intentional correctness bridge, not the eventual bulk data path.

Result: the Galaxy Tab S8+ selected Adreno memory type 6, mapped 4,096 bytes,
wrote and flushed a deterministic byte pattern, zeroed the local shadow,
invalidated it, and recovered the pattern with zero mismatches. The existing
1,024-word GPU fill also retained zero mismatches, all fence transitions
passed, and client/service stderr were empty. All 18 host contracts passed;
the tablet's generated glibc dispatch contract and live global integration
both passed. The E034 policy classifies 50 of 742 names executable, 390
resolved names required-but-unimplemented, and 302 observed-null.

Canonical evidence is `docs/evidence/e034-mapped-memory.json`, 7,611 bytes,
SHA-256
`e707f57d135850957fd27926a7c055c3200447531582e87751e03cfe7b832eec`.
The canonical artifacts are:

- generated E034 policy: 2,316 bytes, SHA-256
  `4123d62045af497e357bd56f8a1cd1d039616afa486afc2fa9f043561eb4302f`;
- glibc `libvulkan-bvb` bridge: 217,792 bytes, SHA-256
  `d2a4b6c84c99e6efb7b3f7565b6d5f788c05bf71b80abf8e4036f744ad4d1b4b`;
- glibc integration client: 72,048 bytes, SHA-256
  `ec45885430e6eb358b6781ffaef0efb9379ae87f855d067cf944b90fb160b9bc`;
- Bionic bridge service: 129,736 bytes, SHA-256
  `c5356aeba2abb0070c54955055917b4aeaf7ac9f0f78a4bb495c66e8d9ac1a41`.

The required recall query found no earlier E034 mapped-memory implementation.
E034 reused E031's allocation metadata and deterministic readback path plus
E032's device-owned queue submission lifecycle.

The next bounded gate is an external-memory-backed upload allocation that the
visible renderer can consume without thousands of inline RPC chunks. Once that
ownership and synchronization path is proven, the rotating triangle can move
its vertex data from shader constants into a glibc-populated vertex buffer.
E034 does not claim zero-copy sharing, image or shader creation through the
game-facing path, descriptor sets, indexed drawing, DXVK startup, a game FPS
result, or removal of Termux.

## E035 — Cross-device opaque-FD external memory (2026-08-19)

Status: passed on real Adreno 730 hardware at source commit `7e576b9`.

Hypothesis: two distinct logical devices created from the tablet's same Adreno
physical device can reference one Vulkan buffer allocation through a single
opaque POSIX file descriptor, eliminating E034's need to copy bulk data through
4,072-byte control packets.

Method: the existing Bionic selftest bootstrap now enables
`VK_KHR_external_memory` and `VK_KHR_external_memory_fd` only when both were
enumerated, plus the instance capability extension used for the external-
buffer query. It creates matching 4 KiB transfer/vertex buffers on two logical
devices. The source allocation is exportable and dedicated when required; a
deterministic byte pattern is mapped, written, and flushed as needed. The
source returns one opaque FD, the destination imports ownership into a distinct
`VkDeviceMemory`, maps and invalidates it as needed, and compares every byte.
The implementation closes an exported FD only if import did not consume it.

Result: Adreno reported external-memory features `7` (`DEDICATED_ONLY`,
`EXPORTABLE`, and `IMPORTABLE`), opaque-FD compatible and re-exportable handle
masks of `1`, and selected host-visible/coherent memory type 4 with property
flags `7`. The destination recovered all 4,096 bytes with zero mismatches and
stderr was empty. All 19 host contracts passed, including a real memfd-backed
fake-driver export/import regression.

Canonical evidence is `docs/evidence/e035-external-memory.json`, 1,196 bytes,
SHA-256
`fa2c7893a9b6a0539f81d559afc713192fa567164a2d40c73e4d1aba15e5b371`.
The Bionic external-memory selftest is 60,688 bytes with SHA-256
`719fc40f113540c743926977519b679cc63911bb3cee250ce0d95a55d4503b9c`.

The required recall query found no earlier E035 external-memory implementation.
E035 reused E004/E005's loader, device, memory-selection, and cleanup path, and
follows E020/E021's explicit descriptor-ownership discipline. The
[Khronos external-memory-FD reference](https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VK_KHR_external_memory_fd.html)
specifies that export transfers FD ownership to the application and successful
import transfers it to the destination Vulkan implementation.

The next bounded gate is E036: export the allocation from the real visible
Android renderer, deliver its FD across the already-proven Binder callback and
same-UID `SCM_RIGHTS` relay, import it into the game-facing Bionic device, and
add explicit cross-device synchronization. E035 does not yet claim cross-UID
sharing, visible consumption, glibc Vulkan import dispatch, a vertex-buffer
triangle, DXVK startup, a game FPS result, or removal of Termux.

## E036 — Visible-renderer external-memory handoff (2026-08-19/20)

Status: passed on real Adreno 730 hardware with visible-host v24 at source
commit `7388d52`.

The bounded gate creates the exportable allocation on the visible Activity's
actual Adreno `VkDevice`, exports one opaque FD, returns it through the existing
Binder callback, relays it into Termux with same-UID `SCM_RIGHTS`, and imports
it in a separate Android-Bionic Vulkan process. The receiver checks all 4,096
deterministic bytes and rejects a wrong 256-bit capability. The host contract
suite now contains 20 tests and passes 20/20. The APK compiles under NDK r29
with `-O3 -Werror` and has a valid v2/v3 signature.

Hardware attempts through installed v21 reached a live, focused 2800x1752
renderer but failed before Vulkan import. The fixed internal endpoint in v19
was reachable but rejected authorization; v20 and v21's token-derived endpoint
returned `Connection refused`. Keeping the lifecycle service alive through the
request did not change that result. Android's cross-UID process and socket
tables were not visible from Termux, so the failure was not relabeled as a
driver or Vulkan error.

v23 removes Activity-recreation state from the socket address. A custom
`NativeActivity` owns the current capability and the exported receiver compares
it in constant time. The native broker returns to one fixed internal endpoint
and retains same-UID peer authentication. This reuses the reachable endpoint
shape from repository commit `52165b7` while avoiding a stale
`pthread_once`-derived address. Java, Binder, and socket setup execute only when
an allocation is shared; none are present in the frame hot path.

v23 restored an explicit visual liveness signal. v24 removes its artificial
30-FPS sleep and lets FIFO swapchain acquisition/presentation follow the active
display refresh: 60 Hz in the tablet's 60-Hz mode and up to 120 Hz in 120-Hz
mode. A native counter reports achieved FPS every 120 frames. External ingress
disables this local heartbeat, so it cannot consume game-frame budget. This
reuses E033's typed push-constant rotation from commit `3be1388` rather than
adding a Java animation path.

The required recall queries for the broker refusal, lifecycle ordering,
Activity reuse, and Package Manager install path returned no indexed prior
sessions. Unattended installation was attempted without weakening security:
SELinux denied system-server reads from Termux private storage, and streamed
installation was rejected for the Termux caller, so the signed v24 APK used the
normal Android install confirmation.

The hardware gate passed on 2026-08-20. The focused 2800x1752 Activity exported
a 9,396-byte allocation from its own renderer device. The Binder helper
received an opaque FD, relayed it by same-UID `SCM_RIGHTS`, and the separate
Bionic consumer imported it through `/system/lib64/libvulkan.so`. Adreno chose
host-visible/coherent memory type 4 with flags 7. The consumer recovered all
4,096 patterned bytes with zero mismatches; its stderr was empty. End-to-end
receive/import took 155,340,885 ns. The deliberately wrong capability was
rejected with `-EACCES` before descriptor delivery.

Canonical evidence is `docs/evidence/e036-external-memory-broker.json`, 1,797
bytes, SHA-256
`976b6835cbe5233ccba37bbe018d41438cf12770330515842a9eeef58735eb22`.
The signed v24 APK is 61,865 bytes with SHA-256
`f91c6b53add62caefe5c671f77a7506c0fd4fb7a47716ef23a6b833fab58f94d`;
the Bionic receiver is 72,624 bytes with SHA-256
`b607e5051f67e67b33ed0b0bd259f94ec17f5cf472d788bebda9df5f59358e98`.
`renderer_export_log_seen` is false because Termux cannot read the other UID's
logcat buffer; the successful opaque-FD callback and independent import are the
gate's direct evidence.

The next bounded gate is E037: export/import a GPU synchronization primitive
and prove ordered producer-to-consumer access without CPU waits or per-frame
Binder traffic. E036 proves the cross-UID zero-copy transport primitive, not
shared images, DXVK startup, Tomb Raider output, or a game FPS improvement.

## E037 — Cross-process GPU synchronization (2026-08-20)

Status: passed on real Adreno 730 hardware with visible-host v27. The producer
path is source commit `33c9bb3`; the final lifecycle-aware evidence harness is
commit `486c9eb`.

The real device capability probe rejected the original semaphore assumption:
`OPAQUE_FD` reports no external-semaphore features, while `SYNC_FD` reports
features 3 (both exportable and importable). E037 therefore retains E036's
opaque-FD external memory allocation but pairs it with a temporary one-shot
`SYNC_FD` binary-semaphore payload. The visible Activity queues the GPU signal
before exporting the sync FD. The separate Bionic consumer imports it with
`VK_SEMAPHORE_IMPORT_TEMPORARY_BIT`, queues a GPU wait, waits on an evidence
fence, and only then validates the shared allocation.

The hardware gate received both descriptors through Binder and same-UID
`SCM_RIGHTS`, imported them through `/system/lib64/libvulkan.so`, and recovered
all 1,024 expected 32-bit words with zero mismatches. The consumer GPU fence
wait took 54,948 ns; receive/import took 140,961,146 ns. The wrong 256-bit
capability was rejected with `-EACCES`, both native stderr streams were empty,
and the final Activity status correctly records Android tearing down the
window after the earlier renderer-ready event. The evidence harness now tests
that renderer readiness occurred rather than incorrectly requiring it to
remain the final lifecycle state. This reuses E035's external-memory ownership
model and E036's allocation-time capability broker; Binder and Java remain
absent from the measured per-frame path. The required `deja` recall query
returned no indexed prior session.

Canonical evidence is `docs/evidence/e037-external-sync-broker.json`, 2,161
bytes, SHA-256
`478171f4a89ae0efa51f903a117d645f35e0b018d2d479a8ad7cde43d29a307c`.

The next bounded gate is E038: export/import an actual Vulkan image and deliver
per-frame `SYNC_FD` payloads over a persistent native descriptor channel. The
one-time Binder setup may establish that channel, but steady-state frames must
not cross Binder or Java. E037 proves ordering for one shared buffer; it does
not yet prove shared image layout, sampling/presentation, DXVK startup, Tomb
Raider output, or an FPS improvement.

## E038 — Cross-process external Vulkan image (2026-08-20)

Status: passed on real Adreno 730 hardware with visible-host v28. The producer,
consumer, Java/Binder metadata path, protocol, and fake-driver regression are
source commit `15fb544`; the real-hardware harness is commit `0eb926b`.

The visible Activity created a dedicated exportable 64×64
`VK_FORMAT_R8G8B8A8_UNORM` optimal-tiling image with transfer and sampled usage,
transitioned it to `GENERAL`, and cleared it to exact magenta on the GPU. It
queued a temporary binary-semaphore signal, exported the 25,780-byte image
allocation as opaque FD and the pending signal as `SYNC_FD`, and delivered both
through the E036 capability broker. The separate Bionic consumer recreated the
identical image, imported the allocation, temporarily imported the sync FD,
queued a GPU wait and image-to-buffer copy, and compared the readback.

All 4,096 pixels matched `0xffff00ff` with zero errors. Adreno selected image
memory type 0 and host-visible/coherent readback flags 15. The consumer fence
wait took 2,696,041 ns and receive/import/readback took 242,367,604 ns. The
wrong capability was rejected with `-EACCES`, receiver stderr was empty, and
the lifecycle again showed renderer readiness before Android removed the
window. The signed v28 APK is 65,961 bytes with SHA-256
`30219769e4b9247c1538bfa26854895e8b1da65e31a0ad3263d6243da9e36134`.

Canonical evidence is `docs/evidence/e038-external-image-broker.json`, 2,264
bytes, SHA-256
`b4aa96c79d657717ae7272e092fbcc4af730f5a28a81fdbeca24ee85a75f190e`.
This reuses E035's external allocation ownership, E036's cross-UID capability
broker, and E037's Adreno-specific temporary `SYNC_FD` ordering. The required
recall queries returned no indexed prior session.

The next bounded gate is E039: establish the cross-UID channel once and keep a
native socket endpoint for steady-state frame metadata and per-frame
`SYNC_FD` descriptors. E038 proves image compatibility and pixel parity, but
its one-shot validation still invokes Binder/Java for that image. It does not
yet prove a 60/120-FPS native frame stream, visible consumption, DXVK startup,
Tomb Raider output, or an FPS improvement.

## E039 — Direct native cross-UID descriptor channel (2026-08-20)

Status: blocked by Android application-domain policy. Native stream and
datagram variants were implemented and validated on the host before hardware
testing. The stream client failed its cross-UID `connect` with `-EACCES`.
Datagrams crossed the UID boundary in both directions, but Termux `recvmsg`
returned `-EACCES` as soon as the Activity attached Vulkan FDs with
`SCM_RIGHTS`; no response or descriptor reached userspace. This isolates the
policy boundary from authentication, Vulkan, and transport framing.

Canonical evidence is `docs/evidence/e039-native-socket-policy.json`. The
required `deja` recall searches returned no indexed prior implementation.

## E040 — Global native NDK Binder bootstrap (2026-08-20)

Status: blocked by Android service-manager policy. The Activity and Termux
client compiled against stable API-29 `libbinder_ndk`; the service retained its
required strong reference and reported registration through authenticated
lifecycle event 13. `AServiceManager_addService` returned `EX_SECURITY` (`-1`),
encoded as unsigned width `4294967295`, and the native Termux lookup returned
`STATUS_NAME_NOT_FOUND` (`-2`). No Java ran in this experiment and no Binder
transaction or Vulkan import occurred.

Canonical evidence is `docs/evidence/e040-native-binder-policy.json`. Together,
E039 and E040 prove that separate ordinary Android UIDs require a
framework-managed Binder handoff for GPU FDs: raw sockets cannot transfer the
FDs, and an untrusted app cannot globally publish its own native Binder
service. The next gate transfers long-lived memory/control handles once via
the proven Binder callback, exits Java, and tests shared-memory/fence frame
coordination with no per-frame Binder, Java, socket, or FD transfer.

## E041 — Producer-fenced external image without external semaphore (2026-08-20)

Status: passed on real Adreno 730 hardware with visible-host v38 at source
commit `12c22f5`. The signed 74,153-byte APK has SHA-256
`3f6f9b5206e66fa35ee5880967e90db04a0f03d8f2b2dcb3af65e8264e9731a6`.

E037 established that this Adreno supports temporary one-shot `SYNC_FD`
semaphores but not reusable opaque-FD semaphores. E039 then proved Android
blocks native cross-UID Vulkan-FD transfer, so a per-frame `SYNC_FD` channel is
not available to ordinary app UIDs. E041 tests the alternative ownership rule:
the producer completes and externally releases its image writes with
`vkQueueWaitIdle` before a one-time framework Binder handoff. The consumer
imports only the long-lived opaque image-memory FD and uses its own local GPU
fence for the readback submission; no external semaphore is created or
imported.

The first unattended v36/v37 attempts exposed a real Android lifecycle race:
renderer-ready event 11 was followed by window-destroyed event 8 and Activity
stop event 5 before the Termux `app_process` helper completed startup. v38's
explicit E041 mode disables the visible heartbeat and retains the external
Vulkan device/cache across temporary window loss, while real Activity
destruction still schedules full cleanup. That offscreen lifetime is required
for a bridge that must survive Android surface churn; it is not a retry around
`-EAGAIN`.

The hardware gate transferred one 25,780-byte optimal-tiling 64×64 RGBA8 image
allocation. The consumer imported it through `/system/lib64/libvulkan.so`,
copied it on-GPU, and matched all 4,096 `0xffff00ff` pixels with zero errors.
The consumer fence wait was 4,836,562 ns and the complete receive/import path
was 241,366,667 ns. Exactly one descriptor crossed Binder and the same-UID
`SCM_RIGHTS` relay. The wrong capability returned `-EACCES`; receiver stderr
was empty.

Canonical evidence is `docs/evidence/e041-fenced-external-image.json`, 2,329
bytes, SHA-256
`42482a09591b0710901c6bf47c951f19857aab785e649e440193c892305e415d`.
This reuses E035's external allocation ownership, E036's one-time Binder relay,
E038's image metadata/import path, and E023's sequence/ring discipline for the
next gate. The required `deja` searches found the prior gate plan but no prior
E041 implementation or lifecycle diagnosis.

E042 now adds a shared control page beside the long-lived image allocation.
Producer and consumer will use shared atomic sequence/acknowledgement words,
futex wakeups, and local GPU fences. Java, Binder, sockets, and descriptor
transfer must remain absent from every measured frame; the initial serialized
proof can then expand to multiple pipelined slots.

## E042 — Persistent native external-image frame ring (2026-08-20)

Status: passed on real Adreno 730 hardware with visible-host v39. The fixed
cross-libc synchronization ABI is commit `8b7d593`; the persistent producer,
consumer, protocol, and Android relay are commit `434439d`; the repeatable
hardware harness is commit `6369442`.

The framework path transfers exactly two long-lived descriptors during setup:
one opaque image-memory FD and one 4 KiB control memfd. The consumer imports the
64×64 optimal-tiling RGBA8 image once and retains that image, its allocation,
readback allocation, mapping, command pool, and Vulkan device for the complete
run. Each of 120 measured frames then uses only fixed-width shared atomics,
process-shared Linux futex wakeups, a producer-local GPU fence, and a
consumer-local GPU fence. No Java, Binder, socket message, or descriptor
transfer occurs inside the frame loop.

All 120 alternating magenta/green frames completed and all 491,520 aggregate
pixel comparisons matched. The serialized native loop took 233,743,020 ns,
or 1.948 ms per 64×64 validation frame (513.4 handoffs/s). Consumer GPU fence
waits totaled 59,211,559 ns, or 0.493 ms/frame. Complete receive, one-time
import, persistent loop, and teardown took 368,773,176 ns. These figures prove
that allocation/import setup is no longer paid per frame; they are not a
native-resolution game FPS prediction because the validation image is only
64×64 and each frame deliberately performs CPU pixel comparison.

The wrong 256-bit capability was rejected with `-EACCES`, receiver stderr was
empty, and Android surface loss did not destroy the retained external renderer.
Canonical evidence is `docs/evidence/e042-persistent-frame-ring.json`, 2,276
bytes, SHA-256
`b828195c8fd8ca1b8dbe37e1b46d700beb7ec27ce92e4aa0675616e9c18e6744`.

This implementation reuses E035's external allocation ownership, E036's
one-time Binder relay, E038's external-image import, and the E023 ring's
sequence/ownership discipline recalled from prior sessions. The required
`deja` query found no prior E042 implementation or failure diagnosis. The next
gate connects the persistent native consumer to the generated DXVK path and
must show a real DXVK frame before attempting the fixed native-resolution Tomb
Raider 2013 benchmark.

## E043 — Steam glibc Vulkan loader selects the Bionic bridge ICD (2026-08-20)

Status: passed on the Galaxy Tab S8+ and real Adreno 730. Steam's AArch64 glibc
Vulkan loader 1.3.296 loaded the standard `bvb_icd.aarch64.json` manifest,
negotiated the ICD ABI with `libvulkan-bvb-glibc.so`, created an instance across
the authenticated Unix-socket boundary, and enumerated the Bionic driver's
physical device as `Adreno (TM) 730` (`vendorID=20803`,
`deviceID=117637121`, `apiVersion=4206888`). Client and service stderr were
both empty.

The implementation adds the loader negotiation exports, loader-compatible
dispatchable-handle prefix, standard ICD manifest, and a real-loader smoke
client. Internal bridge calls use symbolic binding so glibc loader symbol
interposition cannot recurse back into the ICD. Loader-private instance
`pNext` data is accepted but never forwarded across libc boundaries, and
allocation callbacks remain local. The three mandatory Vulkan 1.0 format
queries initially return conservative unsupported results rather than
inventing device capabilities.

Canonical evidence is
`docs/evidence/e043-steam-glibc-icd-loader.json`, 1,454 bytes, SHA-256
`d0cdd186312e9430baa0003f91242f8784816574a13b7c62963362309f5719da`.
This gate reuses E023's
fixed-width transport discipline and the instance/physical-device ownership
introduced in the generated dispatch work. The required `deja` searches found
the prior architecture and ownership decisions but no prior standard-ICD
implementation. E043 proves loader discovery and hardware enumeration, not
DXVK execution or a game frame. The next gate replaces conservative capability
answers with real Bionic queries and advances the measured DXVK startup path.

## E044 — Real format capabilities cross glibc-to-Bionic (2026-08-20)

Status: passed through Steam's real AArch64 glibc Vulkan loader and the Adreno
730 system driver. New fixed-width requests carry only the physical-device ID
and Vulkan scalar inputs for `vkGetPhysicalDeviceFormatProperties` and
`vkGetPhysicalDeviceImageFormatProperties`; the Bionic service resolves the
owned native handle, calls the system driver, and encodes scalar results back.
No libc-dependent Vulkan structure or pointer crosses the transport boundary.

For `VK_FORMAT_R8G8B8A8_UNORM`, the device returned optimal-tiling feature bits
`1047939`, including sampled-image support. A 2D optimal-tiling sampled-image
query returned `VK_SUCCESS`, maximum extent `16384×16384×1`, and maximum
resource size `562949953421312` bytes. The measured 2800×1752 Tomb Raider target
therefore fits the queried hardware limit. Client and service stderr were
empty, and all 23 host tests passed.

Canonical evidence is `docs/evidence/e044-real-format-capabilities.json`,
1,549 bytes, SHA-256
`8bcb5918b4e546b194bc3d824e66f32f808a74eb64a18aaeedd2dca247740e47`.
Implementation is commit `700f0a0`; the distinct hardware harness artifact
names are commit `3a0dfb4`. The required `deja` search found no prior E044
implementation to reuse. E044 proves the two core Vulkan 1.0 queries, not
extended `pNext` capability chains, DXVK execution, or a game frame. The next
gate observes the real next DXVK/loader failure and implements that measured
capability family.

## E045 — Standard-loader logical device and queue (2026-08-20)

Status: passed through Steam's real AArch64 glibc Vulkan loader and the Bionic
Adreno 730 driver. The loader created the bridge instance and physical-device
proxy, the bridge created a native logical device on graphics queue family 0,
and the client recovered the queue and completed both `vkQueueWaitIdle` and
`vkDeviceWaitIdle`. Teardown returned through the standard loader path. Client
and service stderr were empty, and all 23 host tests passed.

The first hardware attempt returned `VK_ERROR_INITIALIZATION_FAILED` before any
device RPC. Opt-in diagnostics showed the loader prepended private
device-creation metadata even though the application supplied no feature
chain. The fix reuses E043's instance-side rule: loader-private `pNext` data
stays in the glibc ICD and only the fixed-width application fields cross to
Bionic. No loader pointer crosses the socket boundary.

Canonical evidence is `docs/evidence/e045-standard-loader-device.json`,
1,365 bytes, SHA-256
`0450fa2d0be7cf796f320c7fde950ecf8902774177ee0910f90a9179631f7889`.
The hardware harness is commit `f4f41a2`, opt-in diagnosis is `f8e8bf0`, and
the device-side loader metadata fix is `17395b7`. The required `deja` search
found no prior standard-loader logical-device implementation; the fix reuses
the E043 loader-private instance handling recovered in the current experiment
log. E045 does not yet pass application feature/extension chains, execute DXVK,
or render a game frame. The next gate bridges the measured instance/device
extension requirements that Wine/DXVK needs before resource creation.

## E046 — Device extension names reach native Bionic creation (2026-08-20)

Status: passed through Steam's standard AArch64 glibc Vulkan loader and the
real Adreno 730. The client enumerated the physical device's real extension
list, confirmed `VK_KHR_swapchain`, encoded the name in the bounded canonical
wire format, and enabled that same extension in the Bionic driver's native
`VkDeviceCreateInfo`. Logical-device creation, queue family 0 retrieval,
queue/device idle, and teardown all passed; client and service stderr were
empty.

The wire format carries up to 24 extension names in 128-byte zero-padded slots.
It rejects missing terminators, empty names, excess counts, inconsistent
payload lengths, and nonzero bytes after a terminator. Only string values cross
the libc boundary; the service builds its own Bionic pointer array. All 23 host
tests pass, including an end-to-end fake driver that observes the enabled
extension and a codec test that rejects corrupted padding.

Canonical evidence is `docs/evidence/e046-device-extension-create.json`,
1,482 bytes, SHA-256
`6713be83250541f1e93ffb07c233709e92fceda209f9bcb86379ae97a6f5500c`.
Implementation is commit `2df2488`; distinct E046 artifact naming is
`814d346`. The required `deja` search found no prior extension-name transport
implementation. E046 removes the unconditional device-extension rejection but
does not yet support instance extensions, feature `pNext` chains, swapchain
commands, DXVK execution, or a game frame. The next gate covers the measured
instance-extension/WSI boundary and then the feature-chain subset DXVK asks for.

## E047 — Vulkan 1.1 physical-device discovery through the ICD (2026-08-20)

Status: passed through Steam's real AArch64 glibc loader and the Adreno 730.
The ICD now resolves core and KHR aliases for Features2, Properties2, Format2,
ImageFormat2, QueueFamilyProperties2, MemoryProperties2, and
SparseImageFormatProperties2. Their base structures reuse the already-proven
fixed-width Vulkan 1.0 RPCs, preserving the caller's structure headers and
never sending a pointer across the socket.

On hardware, `VkPhysicalDeviceFeatures2` reported sampler anisotropy,
`VkPhysicalDeviceMemoryProperties2` reported 9 memory types and 2 heaps, and
FormatProperties2 matched the RGBA8 optimal-tiling feature bits from E044.
The standard-loader device still enabled `VK_KHR_swapchain` and completed
queue/device idle. Client and service stderr were empty; all 23 host tests
passed across all seven base wrappers and aliases.

Canonical evidence is `docs/evidence/e047-vulkan11-discovery.json`, 1,563
bytes, SHA-256
`132bd04e8fe9fa4720fed397bb401d7c4009b5903457f24e517fdcc4344bf20b`.
Implementation and harness are commit `6970fa3`. The required `deja` search
found no prior Features2/Properties2 bridge implementation. E047 covers base
Vulkan 1.1 structures only; it does not claim extended `pNext` capability
chains, instance WSI, DXVK execution, or a game frame. The next measured gate
serializes the specific extended feature/property structures requested by
Wine/DXVK or reaches the virtual-surface boundary, whichever occurs first.

## E048 — Command-backed instance extension crosses into Bionic (2026-08-20)

Status: passed through Steam's standard AArch64 glibc Vulkan loader and the
real Adreno 730. The Bionic service now filters the native instance-extension
list to a bridge allowlist. The glibc ICD advertises exactly
`VK_KHR_get_physical_device_properties2`, whose complete base command family
was implemented in E047, transports that canonical name, and enables it in the
native `VkInstanceCreateInfo`.

The same bounded 128-byte canonical name slots used for device extensions are
used for instance creation. The service rejects non-allowlisted names and
builds its own Bionic pointer array. Hardware then completed the Features2,
Properties2, Format2, and Memory2 checks, enabled `VK_KHR_swapchain` on the
device, and passed queue/device idle with empty client and service stderr. All
23 host tests passed.

Canonical evidence is `docs/evidence/e048-instance-extension-create.json`,
1,502 bytes, SHA-256
`07e97299a4d98ee46bb48fec0630514140994f10300266b17464790f59f8fd4d`.
Implementation and harness are commit `a0bce7b`. The required `deja` result
only found the current E048 plan and no earlier implementation to reuse. E048
does not advertise surface/WSI extensions whose commands are not yet connected,
and it does not prove extended `pNext` chains, DXVK execution, or a game frame.
The next gate is the virtual surface/WSI contract backed by E042's persistent
Android image path.

## E049 — Real Tomb Raider Wine instance requirements measured (2026-08-20)

Status: passed as a discovery gate, with no rendering claim. The authenticated
ARM64 Steam client launched Tomb Raider 2013 through the existing direct glibc
dispatcher and selected the BVB ICD through Steam's standard Vulkan loader. A
concurrent Bionic service independently completed hello negotiation for both
Wine's Zink client and the real `TombRaider.exe` process.

Diagnostics-only WSI advertisement made Wine's pre-instance decision visible.
Zink requested `VK_KHR_get_physical_device_properties2`, `VK_KHR_surface`,
`VK_KHR_wayland_surface`, and `VK_KHR_xcb_surface`. Tomb Raider's Wine Vulkan
path recognized the properties2, surface, Xlib-surface, and Wayland-surface
names, rejected XCB surface at Wine's own boundary, then required exactly three
host instance extensions: `VK_KHR_external_memory_capabilities`,
`VK_KHR_external_semaphore_capabilities`, and
`VK_KHR_get_physical_device_properties2`.

Only the last extension is command-backed by E048, so Steam's loader correctly
returned `VK_ERROR_EXTENSION_NOT_PRESENT` before calling the ICD's
`vkCreateInstance`. This is now the measured E050 boundary; Steam login, X11,
and the private bridge route remained alive through cleanup. The opt-in Wine
Vulkan channel is excluded from normal and benchmark launches, which retain
`WINEDEBUG=-all`.

Canonical evidence is
`docs/evidence/e049-tombraider-wine-instance-requirements.json`, 3,440 bytes,
SHA-256
`20ec2b6b1618297b2d2ba9d1c80b3be036795b50bc4f5b863051f7c60e1da66f`.
The work reuses E011's Tomb Raider dispatch evidence and E043-E048's standard
loader, proxy, wire, and hardware contracts. Required `deja` searches found no
additional prior implementation. Bridge commits are `837c402`, `f263026`, and
`169449a`; parent diagnostics commits are `048854b` and `0d35605`. All 23 host
contracts and the E048 Adreno hardware gate passed. E050 must connect the two
external-capability query families before surface or swapchain work can be
truthfully exposed.

## E051 — Real Wine reaches authenticated virtual WSI (2026-08-20)

Status: passed as an Activity, WSI-discovery, and instance-creation gate; no
game-frame claim. E050 connected the two external-capability families and
normalized duplicate loader extension names. E051 then advertised four
app-facing virtual WSI extensions while filtering those virtual names out of
the native Bionic `vkCreateInstance`, and answered surface discovery from the
authenticated Activity's measured 2800×1752 dimensions.

The integrated AppID 203160 run started the installed Activity only after the
Steam and direct-dispatch readiness markers agreed. Activity event 11 reported
a live 2800×1752 Vulkan renderer. Wine requested the three command-backed
native instance extensions plus `VK_KHR_surface` and
`VK_KHR_xlib_surface`; the bridge normalized the five application names to
three native names, and Bionic returned `VK_SUCCESS` with an owned instance.
Wine then resolved the Xlib/XCB/Wayland virtual surface and presentation-query
entry points through the ICD.

The child exited with status 3 before calling a virtual surface constructor or
presenting a Tomb Raider frame. Earlier in the same child log, a distinct Zink
client called `vkCreateDevice` with two queue-create records, a non-null
`pNext`, and 33 device extensions. The current E046 wire contract deliberately
accepts one queue record and at most 24 extension names, so that request
returned before device RPC. This is a measured integration limitation, but it
is not yet proven to be the sole cause of Wine's later exit; the next gate must
capture the transition after Wine's WSI function resolution and separately
extend realistic device creation.

Implementation is bridge commit `14593f9`; the parent handoff corrections are
`7fed0b0`, `96c7645`, and `940c9a9`. This gate reuses E042's persistent
Activity renderer, E045's loader-private metadata rule, E046's canonical
extension encoding, and E049's real-game trace route. Required `deja` searches
found no additional prior implementation. A native Android Activity and
successful Wine `vkCreateInstance` are proven; swapchain creation, presentation,
a game frame, and an FPS comparison remain unproven.

## E052 — Private Turnip clears DXVK's native feature ladder (2026-08-20)

Status: passed as a private-driver selection and adapter-feature progression
gate; no adapter-acceptance or rendering claim. Steam's glibc loader selected
the BVB ICD, the authenticated Bionic service loaded a private Mesa 26.2.0
Turnip ICD, and DXVK identified `Turnip Adreno (TM) 730 (26.2.0)` with Vulkan
1.4.354. The private driver completed logical-device/queue idle in the isolated
hardware gate. This direct private-ICD result is authoritative for the current
game path; the earlier E043/E044 system-loader version observations do not
establish the raw system ICD's DXVK API floor.

Seven real Tomb Raider launches moved the rejection boundary from
`shaderDrawParameters` through `bufferDeviceAddress`, `descriptorIndexing`,
the required descriptor-indexing subfeatures, `depthClipEnable`,
`robustBufferAccess2`, and `nullDescriptor`. A single native
`vkGetPhysicalDeviceFeatures2` chain now queries the required Vulkan 1.1, 1.2,
1.3, depth-clip, and robustness structures. The BVB response serializes only
the booleans Turnip actually returns and writes only matching caller
structures. No feature is hardcoded true. The existing generated physical
properties codec was also proven to preserve Turnip's required
`maxPushConstantsSize=256`.

DXVK now reaches `VK_KHR_maintenance5`. Mesa Turnip implements maintenance5
and maintenance6, but the private Android API 34 build's strict generated
allowlist hides them. That build-time filter is the next adapter-selection
gate. `VK_KHR_swapchain` remains separate: raw Android Turnip exposes
`VK_ANDROID_native_buffer`, so desktop swapchain semantics must be virtualized
over the authenticated Activity path rather than forwarded or invented.

Canonical evidence is
`docs/evidence/e052-private-turnip-dxvk-features.json`, 4,478 bytes, SHA-256
`2bee7055b0bca6e7ae8f4d81c90170f23db101d946ea26326417a1031c0fe1af`.
The feature bridge culminates in commits `5780c93`, `3358fff`, and `a38e061`;
all 23 host contracts passed after each set. This work reused E043-E051's
fixed-width loader, ownership, query, and Activity contracts plus the exact
DXVK a6764047 feature order recovered by the feature-map agent. The required
`deja` search found that current investigation and no older implementation.
E052 does not prove maintenance5/6 exposure, final adapter acceptance, the
DXVK feature chain reaching native `vkCreateDevice`, swapchain creation, a
game frame, or FPS.

## E053 — Tomb Raider creates a native logical device (2026-08-20)

Status: passed as a native logical-device and exact crash-boundary gate; no
queue, swapchain, or game-frame claim. In `dxvk-direct-5tdfe6k2`, DXVK selected
private Turnip and reached device creation. The bridge returned Vulkan result
0 and Wine recorded `Created device`. The 479,065,129-byte direct log hashed to
`fbf2370f2893cd565eab3b22f2f876a41d9007cf8d0414fde0c45a5cec5e807f`.

The next observed boundary was three identical `vkGetDeviceQueue` thunks for
queue family 0, queue index 0, followed by access violation `0xc0000005` at
`pc=0`. Initializing loader dispatchable device objects did not move it: the
`dxvk-direct-d87075do` callback retest again created the native device, emitted
three queue thunks, and failed at the same null PC. Its 129,610,144-byte log
hashed to
`5d767bd563dd6b12723b625966cae110c9be951cd4993b69f59119234a0f2834`.
Both oversized direct logs were deleted only after their byte counts, complete
hashes, and compact boundary summaries were retained; Steam and X11 survived
cleanup.

Canonical evidence is
`docs/evidence/e053-tombraider-native-device-boundary.json`, 3,274 bytes,
SHA-256
`d6088dfe823379f0eb31f0c46a97343f52c8cbb239d13f8c008980e1dde6ade2`.
Relevant implementation commits are `937c54f`, `a7ac075`, `2c9fc7e`,
`87f6230`, and `db56f7a`. This gate reuses E052's private-Turnip feature ladder
and E043-E051's loader, wire, ownership, and Activity contracts. Required
`deja` searches returned no indexed match; exact hashes were recovered from
retained local Codex transcript summaries rather than guessed. E054 must prove
the queue handle/dispatch path before swapchain or frame work can advance.

## E054 — DXVK timeline semaphore primitives are native-backed (2026-08-20)

Status: passed as a host integration gate; no tablet deployment, frame, or FPS
claim. Pinned DXVK `a6764047e587178` acquires three queue roles, constructs its
device objects, then creates two timeline semaphores in `dxvk_queue.cpp`.
Source-order analysis proved this is the synchronization gate immediately after
the descriptor/sampler bootstrap rather than the cause of the earlier null-PC
failure.

The bridge now forwards binary and timeline semaphore creation, destruction,
counter queries, waits, and signals to the real Bionic Vulkan device. Core and
KHR spellings resolve to the same implementation. The fixed-width wire carries
no pointers, validates every parent handle, bounds a wait to 16 semaphores, and
rejects unsupported allocators, flags, structures, and cross-device handles.
The native object table was raised from the 64-entry self-test size to 4,096 so
a game does not exhaust it during ordinary resource creation.

All 28 host contracts pass, including initial timeline value 7, signal to 11,
successful wait at 11, timeout at 12, and deterministic destruction. This work
reuses E032's fence ownership/teardown pattern, E034's generated dispatch
policy, and E045's rule that loader-private metadata never crosses the wire.
The required `deja` query found no older implementation. Canonical evidence is
`docs/evidence/e054-dxvk-timeline-semaphores-host.json`. Descriptor/sampler
bootstrap, timeline values in queue submission, Activity-side image import,
the first Tomb Raider frame, and FPS remain separate gates.

## E055 — DXVK Submit2 preserves timeline synchronization (2026-08-20)

Status: passed as a host transport gate; no tablet deployment, frame, or FPS
claim. Pinned DXVK uses `vkQueueSubmit2` in `dxvk_cmdlist.cpp` with bounded
arrays of timeline waits, command buffers, and timeline signals. The bridge now
serializes that exact shape in one request and reconstructs one native
`VkSubmitInfo2` on the Bionic service. Core and KHR entry points share the same
implementation.

The wire bounds each array to 16 records and rejects pointers, unsupported
`pNext` chains, device-group indices, allocators, invalid handle types, and
cross-device ownership. The host contract waited for timeline value 11,
submitted one real command-buffer handle, signaled value 13, and read 13 back
from the native fake driver. All 28 contracts pass. This reuses E030's command
buffer ownership, E032's fence parent checks, E034's generated policy, and
E054's timeline semaphore lifecycle. The required `deja` query found no older
implementation. Canonical evidence is
`docs/evidence/e055-dxvk-submit2-host.json`. Descriptor/sampler construction,
the remaining rendering command surface, Activity consumption, a Tomb Raider
frame, and FPS remain separate gates.
## E056 — DXVK descriptor/sampler device bootstrap contract (2026-08-20)

Status: passed through the cross-process host fake driver; no Android or game
frame claim. Against exact DXVK commit
`a6764047e587178283fcde4073ae6e1410af594f`, the bridge now forwards the
legacy sampler-heap constructor sequence: descriptor-set layout, descriptor
pool, batched set allocation, sampler, and batched sampler descriptor update.
Matching sampler, pool, and layout destroys reach the native driver in DXVK's
order. Every returned handle owns a real native fake-driver object; none is a
dummy.

The wire contains only canonical little-endian scalars and typed object IDs.
The Bionic side reconstructs all Vulkan arrays, structures, and the supported
binding-flags `pNext` locally. Counts are bounded, allocation and updates are
batched, and the client and service fail closed on allocators, immutable
samplers, unknown chains, unsupported descriptor shapes, invalid lineage, and
nonzero padding. This reuses E045's loader-private/local-pointer rule and
E046's fixed-width ownership pattern. The required `deja` search found no
older descriptor-bootstrap implementation.

All 28 current host contracts pass, including canonical codec corruption
checks and a fake native driver that verifies the exact requested flags,
counts, enums, float fields, descriptor contents, and destroy order. Canonical
evidence is `docs/evidence/e056-dxvk-descriptor-bootstrap.json`, 3,659 bytes,
SHA-256
`c09ca06162e1576c035d705d1fd4507fafc8e2a5b0cba43a577486809ccb861b`.
Source tracing shows the first still-unresolved eager DXVK call is
`vkCreatePipelineLayout`, after the empty descriptor-set layout used by the
null fragment pipeline.

## E062 — Private Turnip maintenance5/6 hardware acceptance (2026-08-21)

Status: passed on the real Adreno 730 target; no bridge-frame or FPS claim.
The hash-pinned Mesa 26.2.0 Android API 34 private candidate enumerated 150
device extensions, including `VK_KHR_maintenance5` and
`VK_KHR_maintenance6` at spec version 1. Native feature queries returned both
feature bits as true, and a logical device created with both extension names
and both feature structs enabled. `vkDeviceWaitIdle` and teardown passed.
Steam PID 5973 and Termux:X11 PID 27923 remained alive after the isolated
probe. This validates the selective API-34 allowlist exception described in
`docs/PRIVATE_TURNIP.md`; it does not validate the remaining BVB feature wire,
virtual swapchain, a Tomb Raider frame, or FPS. Canonical evidence is
`docs/evidence/e062-private-turnip-maintenance56-hardware.json`.

## E058 — DXVK eager pipeline-layout boundary (2026-08-21)

Status: passed through the cross-process host fake driver; no Android, game
frame, or FPS claim. Against exact DXVK commit
`a6764047e587178283fcde4073ae6e1410af594f`, the bridge now forwards
`vkCreatePipelineLayout` and `vkDestroyPipelineLayout` through opcodes 72 and
73. The proven eager null-fragment shape has independent-set semantics, the
sampler-heap layout, an empty fragment layout, a null vertex slot, and one
160-byte push-constant range covering the vertex, geometry, and fragment
stages truthfully exposed by the fake Adreno profile.

The canonical variable-length wire carries only little-endian scalars and
typed object IDs. Set-layout and push-range counts are bounded at eight and
four. The Bionic service resolves same-device native layouts, reconstructs all
Vulkan arrays locally, owns the returned native pipeline-layout handle, and
destroys it explicitly or before descriptor dependencies at device teardown.
Both sides fail closed on allocators, unknown chains, unsupported flags or
stages, invalid null slots, wrong lineage, overlapping stages, unaligned or
oversized push ranges, malformed lengths, and nonzero reserved bytes.

All 30 current host contracts pass. The fake native driver verifies the exact
DXVK flags, three-slot native/null topology, stage mask, 160-byte range, real
handle, and destroy order. The immutable E011 manifest already records both
commands at device scope with seven lookups each; policy promotion yields 74
executable, 366 required-unimplemented, and 302 probed-null names. This reuses
E045's loader-private/local-pointer rule and E046/E056's canonical typed-object
ownership pattern. The required `deja` query found no prior pipeline-layout
implementation.

Canonical evidence is
`docs/evidence/e058-dxvk-pipeline-layout-host.json`, 4,309 bytes, SHA-256
`206a00a42afc636e29789995a48d613661837cdd31926ff62a452fb65ce6eb34`.
Source tracing identifies the next exact eager null entry as
`vkCreateGraphicsPipelines`, submitted by
`DxvkShaderPipelineLibrary::compileFragmentShaderPipeline` immediately after
the independent pipeline layout.

## E063 — Native image and image-view resource slice (2026-08-21)

Status: passed through the cross-process host fake driver; no Android, game
frame, or FPS claim. The bridge now forwards `vkCreateImage`,
`vkDestroyImage`, legacy `vkGetImageMemoryRequirements`, `vkBindImageMemory`,
`vkCreateImageView`, and `vkDestroyImageView` through opcodes 90–95. The wire
uses fixed-width little-endian scalars and typed IDs only, bounds queue-family
and view-format arrays at eight and sixteen, and reconstructs only the
supported format-list, stencil-usage, and view-usage `pNext` records on the
Bionic side.

Every returned image and view owns a real native fake-driver handle. Images
are parented to their device, views to their image, and image-memory binding
requires common device ownership. Explicit image destruction refuses a live
view child; device teardown destroys views before images. Both sides fail
closed on allocation callbacks, unsupported or malformed chains, unsupported
image shapes, invalid ranges and enums, cross-device IDs, and noncanonical
wire fields. All 31 host contracts pass, including native create,
16,384-byte requirements, allocation/bind, view creation, explicit destroy,
and an intentional leaked image/view pair that the service must tear down
before the fake driver permits device destruction.

The E063 allowlist is mechanically E058's proven list plus exactly the six
new names. Generated policy counts are 80 executable, 360
required-unimplemented, and 302 probed-null. Pinned DXVK source identifies the
next exact call in this image path: `vkCreateImage` at
`src/dxvk/dxvk_memory.cpp:1111` is immediately followed by
`vkGetImageMemoryRequirements2` at line 1130 with a
`VkMemoryDedicatedRequirements` response chain. The legacy requirements call
implemented here therefore does not yet let DXVK complete image allocation;
the v2 query is the next gate. The ordinary allocation path's 16 MiB ceiling
is another known blocker for a native-resolution RGBA image.

Canonical evidence is
`docs/evidence/e063-native-image-resources-host.json`, 5,118 bytes, SHA-256
`a637cc151e1ad055db4d6066f6a4ac6fd66dee32fe71bf3f3db7c083208d1f98`.
Required `deja` searches returned no indexed prior image-resource
implementation. This gate reuses E034/E058 resolver-policy generation, E046
fixed-width typed ownership, E056 local `pNext` reconstruction, and the
existing buffer/device-memory lifecycle pattern. Real Turnip execution,
requirements2, large allocations, rendering, presentation, a Tomb Raider
frame, and FPS remain unproven.

## E064 — DXVK null-fragment graphics pipeline (2026-08-21)

Status: passed through the cross-process host fake driver; no Android, game
frame, or FPS claim. Against exact DXVK commit
`a6764047e587178283fcde4073ae6e1410af594f`, the bridge now forwards the
null-fragment-library `vkCreateGraphicsPipelines` call and matching
`vkDestroyPipeline` through opcodes 74 and 75. Opcodes 76–79 remain free, and
the E060 producer path remains on opcodes 100–101.

The exact accepted topology is one fragment-library create with the
graphics-library, flags2-library, and zero dynamic-rendering pNext chain. It
preserves maintenance5's embedded `VkShaderModuleCreateInfo`, so there is no
invented `vkCreateShaderModule` prerequisite. The bridge also preserves the
zero depth/stencil block, all null irrelevant fixed-function state, and DXVK's
exact nine-state dynamic list—including its repeated stencil-test-enable
entry. The canonical wire contains only little-endian scalars, typed IDs,
bounded dynamic enums, and at most 256 SPIR-V words. The service reconstructs
all pointers locally, owns the real native pipeline, validates pipeline-layout
lineage, and destroys pipelines before their layouts.

All 31 current host contracts pass after rebasing on E063 commit `909e2cc`.
The fake native driver verifies the full three-node pNext topology,
the 180-byte dummy fragment module, exact dynamic-state order, real native
layout and pipeline handles, and dependency-safe destruction. Dispatch policy
promotion yields 82 executable, 358 required-unimplemented, and 302
probed-null names. This reuses E045's loader-private/local-pointer rule,
E046/E056's canonical typed-object ownership pattern, and E058's layout
lineage validation. The required `deja` query found no indexed implementation.

A fuller pinned-source audit corrects the prior E058 ordering claim:
`DxvkMemoryAllocator::determineBufferUsageFlagsPerMemoryType` directly reaches
the still-unimplemented `vkGetDeviceBufferMemoryRequirements` before the
sampler and pipeline-manager members are constructed. That maintenance4 query
is therefore an earlier missing prerequisite, not a later post-E064 call; the
integrated runtime trace must confirm the real boundary. Canonical evidence is
`docs/evidence/e064-dxvk-null-fragment-pipeline-host.json`, 6,447 bytes,
SHA-256
`f9aade59ae46739ed1ef2cfb3ff7e485d64b0b41e56bf998360674c3d45cd92f`.

## E065 — DXVK image requirements and dedicated allocation (2026-08-21)

Status: passed through the cross-process host fake driver; no Android, game
frame, or FPS claim. Pinned DXVK commit
`a6764047e587178283fcde4073ae6e1410af594f` calls
`vkGetImageMemoryRequirements2` immediately after image creation, consumes a
`VkMemoryDedicatedRequirements` output, may pass the image through
`VkMemoryDedicatedAllocateInfo`, may prepend the device-address-only
`VkMemoryAllocateFlagsInfo`, and finally uses legacy `vkBindImageMemory`.
E065 forwards that exact bounded chain through opcodes 96 and 97. The wire is
pointer-free and fixed-width; the service resolves typed same-device image
ownership and reconstructs all native records locally.

The ordinary device-memory ceiling is now a finite 256 MiB, matching pinned
DXVK's `MaxChunkSize`. This admits a 2800x1752 RGBA image (19,622,400 bytes,
or 19,623,936 bytes at 4096-byte alignment) without changing the 4,096-byte
protocol payload ceiling or the 4,072-byte mapped-memory I/O chunk. Unknown
or duplicate pNext records, dedicated buffers, memory-priority, import/export,
device groups, nonzero device masks, callbacks, invalid ownership, and the
first byte above the cap all fail closed.

All 31 host contracts pass. The fake-native integration returns the
19,623,936-byte requirement, reports
both dedicated booleans true, verifies the exact flags-to-dedicated native
chain and native image handle, allocates real memory, and completes legacy
binding. Only `vkGetImageMemoryRequirements2` is newly promoted, producing 83
executable, 357 required-unimplemented, and 302 probed-null names. The pinned
path does not invoke BindImageMemory2, so E065 does not fabricate or promote
it. The known eager `vkGetDeviceBufferMemoryRequirements` prerequisite remains
a separate gate.

Canonical evidence is
`docs/evidence/e065-dxvk-image-allocation-host.json`, 5,521 bytes, SHA-256
`cba3c3eb7e1fabd46ceedd41832e4f1b02f35ca6687940737690f87e25386b7e`.
The required `deja` query returned no indexed implementation. This gate reuses
E046 typed ownership, E056 bounded local pNext reconstruction, E063 image
lineage and legacy binding, and E034/E064 resolver-policy generation.

## E066 — DXVK device buffer requirements cross the native boundary (2026-08-21)

Status: passed through the cross-process host fake driver; no Android, game
frame, or FPS claim. The corrected E064 source audit identified
`vkGetDeviceBufferMemoryRequirements` as an earlier DXVK device-constructor
prerequisite. E066 now forwards that core entry through opcode 76. Pinned DXVK
`a6764047e587178283fcde4073ae6e1410af594f` declares and calls only the core
spelling, so the KHR alias deliberately remains null.

The 64-byte request carries one typed device ID and the complete bounded core
`VkBufferCreateInfo` shape: size, flags, usage, sharing mode, and at most eight
unique queue-family indices. The 32-byte response carries the native size,
alignment, memory-type mask, and both native dedicated-allocation booleans.
All pointers and Vulkan structures are reconstructed in the Bionic service.
The client accepts only null input chains and either a null output chain or one
terminal `VkMemoryDedicatedRequirements`; rejected void calls leave
deterministic zero output rather than inventing requirements.

The fake native driver observes DXVK's first unconditional allocator query:
65,536 bytes, no flags, exclusive sharing, and uniform-texel plus transfer-src
and transfer-dst usage. It returns size 65,792, alignment 256, memory-type mask
5, and a required but not preferred dedicated allocation. The generated policy
promotes exactly the core name over E065's complete list, yielding 84
executable, 356 required-unimplemented, and 302 probed-null entries.

Canonical evidence is
`docs/evidence/e066-dxvk-device-buffer-requirements-host.json`. The required
`deja` search found no prior implementation; E066 reuses E046's fixed-width
pointer-local RPC discipline and E063's native-device and canonical resource
wire conventions. Because the observed real boundary reached this call, the
sparse-binding-only device-image query was not taken, and every later
source-guaranteed constructor call found in the audit is already bridged. The
next exact unresolved call must therefore come from a new tablet trace after
deployment rather than a source-order guess.

## E068 — DXVK global buffer requirements and device address (2026-08-21)

Status: passed through the cross-process host fake driver; no Android, game
frame, or FPS claim. Pinned DXVK commit
`a6764047e587178283fcde4073ae6e1410af594f` uses the same exact core sequence
for ordinary dedicated and global buffers: create, core
`vkGetBufferMemoryRequirements2`, allocate or reuse memory, legacy bind, then
core `vkGetBufferDeviceAddress` when shader-device-address usage is present.
E068 forwards those two missing queries through opcodes 78 and 79, leaving
reserved opcode 77 and producer opcodes 100–101 untouched.

The fixed-width wire carries only a typed buffer ID, one optional dedicated
requirements bit, native scalar requirements, and the device address. The
client and service both enforce same-device buffer ownership and reconstruct
all Vulkan structures locally. Requirements2 accepts only null input `pNext`
and either null output `pNext` or one terminal
`VkMemoryDedicatedRequirements`; the address info accepts only null `pNext`.
Malformed shapes deterministically return zero output without inventing a
result.

The existing buffer-create ceiling now matches pinned DXVK's bounded 256 MiB
`MaxChunkSize`, and the accepted global-buffer usage mask is restricted to the
source-audited allocator flags with mandatory transfer-src, transfer-dst, and
shader-device-address bits. Address queries additionally require that exact
usage capability and a successful memory bind on both sides of the bridge.

The fake driver verifies size 4,096, alignment 256, memory-type bit 1,
preferred-but-not-required dedicated booleans, and address
`0x123456780000`; it aborts if the address query arrives before legacy memory
binding. Policy promotes only the two core names, yielding 86 executable, 354
required-unimplemented, and 302 probed-null entries. KHR aliases stay null and
the EXT address spelling stays probed-null because pinned DXVK calls core and
this gate has no separate native-alias proof.

Canonical evidence is
`docs/evidence/e068-dxvk-global-buffer-address-host.json`, 5,647 bytes,
SHA-256
`b9c20ae5d0514d251bcacda888627bd3bd5f44fd4489bc0c6912bc1c9742a9bc`.
The required `deja` search found no indexed implementation; this gate reuses
E046 typed ownership, E056/E063 bounded local `pNext` reconstruction, E066
requirements validation, and the existing generated policy mechanism. Real
Turnip execution, feature gating, Tomb Raider progress, dedicated buffer
allocation, and the next runtime boundary remain unproven pending deployment.
## E069 — DXVK D3D9 color-image initialization commands (2026-08-21)

Status: passed through the cross-process host fake driver; no Android, game
frame, or FPS claim. Pinned DXVK
`a6764047e587178283fcde4073ae6e1410af594f` creates Tomb Raider's D3D9
backbuffers in batches of one through four and initializes each ordinary color
image with a zero `vkCmdClearColorImage`. It records the clear first, then
finalizes the accumulated undefined-to-transfer-destination transition through
core `vkCmdPipelineBarrier2`; submission deliberately executes the barrier
command buffer before the clear command buffer.

E069 carries those exact shapes through opcodes 102 and 103. The 48-byte
barrier record contains one typed command-buffer ID, a count, and four bounded
typed image slots. The 16-byte clear record contains only the command-buffer
and image IDs. No pointer or Vulkan aggregate crosses the ABI: the Bionic
service verifies same-device native ownership and reconstructs local
`VkDependencyInfo`, `VkImageMemoryBarrier2`, `VkClearColorValue`, and
`VkImageSubresourceRange` records. Unsupported chains, flags, layouts, access
masks, stages, ranges, duplicate images, and stale or cross-device handles are
rejected without forwarding.

The cross-process fake driver checks every native field and requires the exact
DXVK CPU call order before `vkEndCommandBuffer` can succeed. Generated policy
promotes only core `vkCmdClearColorImage` and core `vkCmdPipelineBarrier2`, for
88 executable, 352 required-unimplemented, and 302 probed-null names. The KHR
barrier alias stays null because the pinned loader has no alias member.
Conditional depth/stencil initialization remains excluded pending a real
Tomb Raider presentation trace, and opcode 104 remains unassigned.

Canonical evidence is
`docs/evidence/e069-dxvk-init-image-commands-host.json`. The required `deja`
query found no indexed implementation. E069 reuses E045/E046 fixed-width
pointer-local transport, E031 command-buffer forwarding, E054 synchronization2
validation, and E063 typed native image ownership. The earlier E067 proposal
was corrected and cancelled: the Adreno 730's native Turnip feature truth makes
DXVK disable sparse image probing; observing the buffer query alone did not
prove that conclusion.

## E070 — honest real-hardware global validation mode (2026-08-21)

Status: both strict-fake and divergent hardware-mode host contracts pass; real
tablet deployment remains pending, with no visibility or FPS claim. The first
synthetic Activity tablet run authenticated all six lifecycle events and then
failed because the global client demanded the fake driver's sampled-image
maximum width of 4096 while Adreno truthfully returned 16384. Auditing the
whole client found additional fake-only constants in memory requirements,
buffer addresses, dedicated-allocation preferences, proxy serials, and the
separate 59-invented-extension transport stress device.

The default remains `strict-fake` and retains every exact fixture assertion.
An explicit `BVB_GLOBAL_DISPATCH_HARDWARE=1` branch instead requires Vulkan
capability bounds and cross-call consistency: at least Vulkan 1.0, DXVK's
256-byte `MaxTotalPushDataSize` acceptance floor, a sampled-image maximum of at
least 2800x1752 for the native-resolution Tab S8+ target, matching legacy and
Properties2 results, power-of-two memory alignment, nonzero in-range memory
type masks, stable repeated queries and buffer addresses, and valid dedicated
allocation booleans. Return codes, unsupported-shape zeroing, typed same-device
ownership, lifecycle authentication, mapping contents, command recording,
fences, and timeline semantics are never weakened. The synthetic scale-device
success path remains strict-fake-only because its `VK_BVB_scale_extension_*`
names are not real driver extensions.

The standalone Activity harness now accepts `--hardware-validation`, injects
the environment variable into the client child only, and records the selected
mode in its result JSON. The Termux gate uses that explicit route. A divergent
fake hardware fixture reports 512 push-constant bytes, a 16384x8192 image
maximum, alignment 4, a different nonzero device address, consistent 16384-byte
image requirements, and false dedicated preferences; passing it proves that
the invariant branch is real rather than a renamed exact fixture. Dispatch
policy remains 88 executable, 352 required-unimplemented, and 302 probed-null.
The complete host suite passes 36/36 through `scripts/check.sh`.

Canonical host evidence is
`docs/evidence/e070-real-hardware-global-validation-host.json`. The required
`deja "BVB global_dispatch real hardware mode fake-specific assertions Termux Activity harness"`
query returned no indexed implementation. E070 reuses the repository's exact
global-dispatch fake contract and authenticated synthetic Activity harness as
the strict baseline. Real Adreno execution, image import/display, and game
performance remain separate gates.

## E071 — private-Turnip hardware route reaches virtual swapchain (2026-08-21)

Status: passed as a truthful driver-selection and capability-route gate, with
an exact swapchain-creation boundary; no imported Activity image, visible game
frame, benchmark, or FPS claim. The first E070 tablet attempt authenticated all
six Activity lifecycle records but used the harness default
`/system/lib64/libvulkan.so`, whose driver returned
`computeFullSubgroups=false`. Mesa 26.2.0 Turnip sets that Vulkan 1.3 feature
true unconditionally, and selecting the installed private driver by its
verified SHA-256 immediately cleared the feature boundary.

The next attempt showed a second route mismatch: Android exports external
memory through `OPAQUE_FD` but external semaphores through `SYNC_FD`. Hardware
mode now queries that measured split; strict-fake mode keeps its original exact
`OPAQUE_FD` semaphore fixture. The Termux gate passes the absolute private ICD
path to the Bionic service, refuses missing or symlinked loaders, and records
the selected artifact in successful evidence. Both corrections preserve all
typed ownership, Vulkan return-code, and DXVK 256-byte push-constant checks.

With private Turnip plus `SYNC_FD`, the real 2800x1752 run cleared feature and
capability discovery, logical-device creation, queue and command operations,
memory/resource checks, and reached `vkCreateSwapchainKHR`. The service then
failed precisely with `device lacks external-image ring entry points`. The
test device enabled only virtual `VK_KHR_swapchain`; the current transport
requires native `vkGetMemoryFdKHR`, whose `VK_KHR_external_memory_fd`
dependency must be enabled internally rather than imposed on the application.
That is E072's exact implementation gate.

Canonical evidence is
`docs/evidence/e071-private-turnip-swapchain-boundary.json`, 3,635 bytes,
SHA-256
`e58120281203246336060c97b6ec14cd5779967ae8be5de1d607836e6c2cf023`.
The required `deja` queries found no indexed implementation. E071 reuses E052's
authoritative private-Turnip route, E035's measured `OPAQUE_FD` memory and
`SYNC_FD` semaphore split, and E070's dual strict/hardware validation design.
Steam PID 5973 and Termux:X11 PID 27923 survived both bounded probes.

## E072 — internal native export dependency for virtual swapchain (2026-08-21)

Status: host contract passed; tablet redeployment and the next real-hardware
boundary remain separate. `vkCreateDevice` now keeps the application-facing
`VK_KHR_swapchain` virtual while enabling native
`VK_KHR_external_memory_fd` exactly once when raw device enumeration supports
it. It preserves first-occurrence application extension order, does not inject
the dependency for non-swapchain devices, and returns
`VK_ERROR_EXTENSION_NOT_PRESENT` before native device creation when the raw
driver hides it. The implementation adds no advertised capability and no wire
opcode.

A strict fake-native matrix proves implicit injection, no duplication when the
application already names the dependency, no injection without swapchain, and
fail-closed behavior when native enumeration hides it. A second contract hides
`vkGetMemoryFdKHR` and verifies that swapchain preparation names that exact
missing function; the diagnostic also lists every other missing ring entry
point rather than collapsing them into one generic message. The complete host
suite passes 38/38. Dispatch policy remains 742 total commands: 88 executable,
352 required-unimplemented, and 302 probed-null.

Canonical host evidence is
`docs/evidence/e072-virtual-swapchain-native-dependency-host.json`. The required
`deja "E071 virtual swapchain external_memory_fd dependency injection Turnip"`
query returned no indexed implementation. E072 reuses E071's measured private
Turnip boundary and the existing cached raw device-extension enumeration;
there is no imported Activity image, visible game frame, benchmark, or FPS
claim.

## E073 — private Turnip virtual WSI and frame export pass on tablet (2026-08-21)

Status: passed through the real private-Turnip virtual-swapchain producer and
one-time frame-FD handoff; Activity image import, changing pixels, a Tomb
Raider frame, benchmark, and FPS remain unproven. The hardware client now
requests Vulkan 1.3, matching pinned DXVK. This corrected the prior mismatch
where the client created a Vulkan 1.1 instance and later called core
`vkGetDeviceBufferMemoryRequirements`, which the private driver correctly did
not expose through that API contract. Strict-fake mode remains Vulkan 1.1, so
the established fixture was not weakened.

The canonical 2800x1752 Tab S8+ run used the explicit private Turnip artifact
`libvulkan_freedreno.so` (18,041,856 bytes, SHA-256
`8ac6ef78c3c92998aa46c59fd0081edcba82756f5bad561d1b24a57684874a45`).
It authenticated lifecycle events 1, 2, 3, 7, 11, and 9; identified Adreno
730 with Vulkan 1.4.354; enumerated 151 device extensions; created the logical
device and queue; and passed the exercised buffer, image, memory, command,
fence, and timeline paths. It then created the virtual surface and swapchain,
returned three 19,910,656-byte swapchain images, acquired and presented one,
and delivered the one-time four-FD bundle (three images plus the shared control
page) to the authenticated synthetic Activity sink. Client and service exited
zero with empty stderr. Steam PID 5973 and Termux:X11 PID 27923 survived.

Generated full evidence is
`out/e071-current-global.json`, 11,833 bytes, SHA-256
`77572d731226da949adad9d5c38f4e4cf2ad6a46a6ed8c4797bae4d9a1216fc8`.
The compact canonical record is
`docs/evidence/e073-private-turnip-virtual-wsi-tablet.json`, 4,433 bytes,
SHA-256
`b8b79fdfb1899a3ebab5edb570c4dfbe983999bfb1d7ebcf71ce6d5a2199ef27`.
The required `deja` queries found no indexed implementation. E073 reuses E035's
measured Android external-handle split, E052's private-Turnip provenance,
E070's honest hardware-validation branch, E071's explicit driver route, and
E072's internal native export dependency. The next gate is installing visible
host v40 and proving that the real Activity imports this FD bundle and presents
changing pixels before attempting a Tomb Raider or performance claim.

## E074 — exact v40 Activity import runtime gate prepared (2026-08-21)

Status: host contracts passed; the script has not installed or launched an
Android Activity, and tablet import/present proof remains pending. The
standalone Termux entry point refuses source, staged, or installed identities
other than versionCode 40, requires the installed base APK to be byte-identical
to the staged signed APK, compares signing-certificate digests, and inspects
the packaged native library for both E057 completion markers. This prevents
the versionCode 39 E042-only Activity from silently accepting a test it cannot
perform.

The runtime sequence reuses E010 wrong-token rejection, launches the Activity
with the previously missing `bvb_retain_external_renderer=1` requirement,
waits for authenticated created/started/resumed/window/renderer-ready events,
starts the same-UID `FrameTransportClient`, then runs the real E073 virtual WSI
smoke producer. The global test client accepts the Activity's measured extent
and a bounded post-present hold of at most 30 seconds so teardown cannot race
the E057 consumer. All children, the exact launched BVB package, and the
private runtime directory have bounded cleanup; Steam and Termux:X11 are never
started or signalled. Per-run logs and evidence use a unique ignored output
directory, and the launch capability is not retained.

Before this gate was prepared, the installed tablet payload was updated and
verified: bridge client SHA-256
`c0b3dbf36f45bad941a8579bf37bcc8d5773ac7b4d3c0e10a601b58fc4aee3eb`,
bridge service SHA-256
`0917ef33209b0ea32a337de48646908057854f829387671d0a832ec707371241`,
and unchanged private Turnip SHA-256
`8ac6ef78c3c92998aa46c59fd0081edcba82756f5bad561d1b24a57684874a45`.
The `install-pre-0c54e92` rollback directory was verified, and the installed
one-shot private-Turnip ICD test passed. These deployment checks precede E074;
they are not evidence that the Android Activity import runtime has passed.

Canonical host evidence is
`docs/evidence/e074-activity-frame-v40-runtime-gate-host.json`. The required
exact `deja` query returned no indexed implementation. E074 reuses E010, E057,
E060, E073, and the current `steamclienttermux`
`start-tombraider-bvb-probe.sh` service/helper ordering. Because E073 presents
an image without a deterministic pattern, a successful runtime pass proves
authenticated import and native present markers only; the Activity may remain
black, and no screenshot, changing game frame, Tomb Raider output, benchmark,
or FPS is claimed.

## E075 — persistent shared-memory command recording (2026-08-21)

Status: host contracts pass; opt-in transport milestone only. E075 removes the
per-call Unix-socket round trip from command recording while preserving the
strict legacy path as the default A/B control. `BVB_COMMAND_STREAM=shared`
creates one 16 MiB sealed-size memfd during connection setup and leases one of
256 fixed 64 KiB slots only from Begin until the first successful Submit2
replay. Allocation alone consumes no slot; Reset, Free, pool teardown, a failed
End, or a successful replay returns it. Canonical local records cover Begin,
FillBuffer, zero ClearColorImage, initialization PipelineBarrier2, and End. The
record framing leaves IDs 10 and 11 free for E076's general Barrier2 and
ClearColor forms; E075 uses IDs 20 through 23 for its lifecycle and current
fixed E069 image shapes.

Dedicated opcode 105 carries a bounded 40-byte `vkQueueSubmit2` command
reference: typed command-buffer ID, shared generation and sequence,
slot-aligned offset and length, device mask, and one shared-stream flag. The
legacy opcode-85 Submit2 record remains byte-for-byte 16 bytes, so strict A/B
and replayed native command buffers keep the established wire. The service
performs an acquire fence and validates the complete Submit2 ownership
graph, every batch header, Begin/End topology, every resource's type and native
device, and per-command-buffer monotonic generation before any native replay.
Validated generations are consumed before replay so a failed or malicious
retry cannot replay a partially touched native command buffer. Unsupported
void-command input poisons the local recording, so End and Submit2 fail
deterministically rather than submitting a partial stream.

The cross-process fake-driver contract observes the exact E069 native command
sequence and successful Submit2 while the client exchange counter proves zero
socket exchanges across Begin/Fill/Clear/Barrier/End. The unchanged strict
contract measures five exchanges across the same valid recording sequence.
Codec tests reject corrupt generation, slot alignment, flags, reserved words,
duplicate image IDs, and stale/replayed sequences. The shared global contract
also rejects unsupported void shapes and a real typed buffer owned by a second
logical device, reuses a released slot, and allocates 257 additional live
command buffers without consuming stream slots. This gate makes no tablet
deployment, visible-frame, Tomb Raider, benchmark, or FPS claim.

The required `deja` query found no indexed persistent game-command
implementation. E075 reuses Decision 0003's bounded batched-dispatch direction,
E014's one-time memfd setup and release/acquire generation discipline, E042's
setup-only descriptor transfer with a socket-free shared data plane, and E060's
stable typed ownership/generation model.

## E075a — immutable replay and transactional generations (2026-08-21)

Status: 42/42 host contracts pass; corrective host-only gate. The E075 audit
confirmed that zero recording RTTs were real but found that the service
validated and replayed directly from a peer-writable mapping, committed
generation entries one command at a time, and retained a sealed slot when
native Submit2 returned a non-success `VkResult`. E075a takes one bounded heap
snapshot of each referenced stream and never validates or replays the shared
mapping itself. It stages every generation update in a private 4096-entry
shadow table and copies that table back only when all streams pass. If the table
is full, one bounded scan discards only entries whose typed command-buffer
handle no longer resolves in the connection's native context, then retries the
transaction once.

The client now distinguishes a clean opcode-105 reply from the Vulkan result
inside that reply. A clean reply proves the native command buffer was replayed,
so the slot is retired even when native Submit2 returns an error. The regression
forces the third fake Submit2 to return `VK_ERROR_OUT_OF_DEVICE_MEMORY`, then
proves the retry uses opcode 85 rather than replaying opcode 105. The reused
command-buffer test now records with flags zero, making its resubmission legal.
Unit contracts mutate the shared source after snapshot, reject a later update
without partially changing the real generation table, and reclaim one exact
dead-handle entry while preserving the live entry. Strict recording remains
five socket exchanges and shared recording remains zero.

The exact E075a `deja` query found no indexed correction. The broader recall
reused E075's deliberately separate opcode-105/shared Begin-Cmd-End design and
the audit's verified strict opcode-85 compatibility. Mapped-memory flush fan-out
and global recording-mutex scalability are recorded as E077 follow-up work.
This gate has no tablet deployment, visible-frame, Tomb Raider, benchmark, or
FPS claim.

## E076 — rich shared records and four-frame producer (2026-08-21)

Status: 43/43 host contracts pass; host-only producer gate. Shared record 10
now carries one to four image barriers with the original stage, access, layout,
queue-family, typed image, and bounded image-range fields. Shared record 11
carries an arbitrary four-word clear color and one to four color ranges. Both
are fixed width, pointer-free, and require zeroed inactive slots. Memory and buffer
barrier arrays remain rejected because this gate does not implement or claim
them. Exact E069 zero-clear and initialization-barrier records remain valid,
and the strict socket path still uses legacy RPC 102/103.

The synthetic virtual-swapchain producer runs four frames so the three-image
ring must be reused. Each frame acquires an image, resets and begins a command
buffer, transitions `UNDEFINED` (or `PRESENT_SRC_KHR` on reuse) to
`TRANSFER_DST_OPTIMAL`, clears red/green/blue/white, transitions to
`PRESENT_SRC_KHR`, ends, submits with acquire/render binary semaphores, and
presents. A concurrent host sink observes and releases slots 0,1,2,0 in ABI
order. The strict fake driver confirms all four native command sequences and
binary semaphore transitions. Recording the commands adds zero socket round
trips.

E076 rebases on E075a: every rich decode consumes the same private immutable
snapshot used for validation, and all stream generations are applied as one
transaction before replay. Unit corruption tests reject nonzero inactive
payload slots and wrong handle types; integration tests poison invalid layouts
and a rich clear against an image owned by another logical device. The prior
exact `deja` query found no indexed first-frame sequence. This gate reuses
E060's typed frame ring, E069's fixed command compatibility, E073's three-image
virtual WSI, E075's zero-RTT stream, and E075a's replay correctness.

The exact next visible boundary is deployment with the v40 Activity importer
and observation of the four changing colors on the tablet. The bounded
`scripts/test-rich-frame-animation-v40-termux.sh` runner needs no APK change:
it reuses the installed byte-identical v40 Activity, requires the exact
deployed E076 service and producer-client SHA-256 values, rejects a producer
without the E076 controls/markers, emits `E076_FRAME_EXPECTED` records, and
correlates all four with one Activity import generation and matching
`E057_FRAME_PRESENTED` sequence/slot markers. It prints a separate visual
confirmation request and leaves screenshot status pending because metadata is
not pixel proof. No Activity import, tablet-visible frame, Tomb Raider first
frame, benchmark, or FPS is claimed by this host gate.

## E077 — persistent upload-memory mirrors (2026-08-21)

Status: 49/49 host contracts pass; host-only upload transport gate. The first
general-mirror draft was rejected because a broad completion
pull could expose writes from unrelated queues and a full mirror upload could
overwrite GPU-produced bytes. The corrected gate classifies each allocation.
Only memory already bound exclusively to GPU-read-only buffers can use shared
transport. Transfer destinations, storage, shader-device-address, transform
feedback, images, and unknown/unbound memory remain strict; unsafe bind after
Map is rejected. General coherent device-to-host visibility is not claimed.

Each eligible Map creates one exact-size memfd, seals its capacity, passes it
once with SCM_RIGHTS, and uses pointer-free metadata opcodes 106--109. The
service retains a private byte baseline and uploads only host-diverged spans
before Submit. Strict map/flush/invalidate/unmap/submit costs 2/2/2/2/3 socket
exchanges; eligible shared costs 1/1/1/1/1 with exact opcodes
106/107/108/109/47. The existing GPU-writable fixture proves hybrid fallback
stays on strict opcodes 49/48. Live guards reject a GPU-writable buffer bind and
an image bind into an already active shared mirror.

Noncoherent Map leaves contents undefined without implicit native Invalidate.
Explicit partial Flush/Invalidate of bytes 257+7 copies exactly those bytes and
expands the native maintenance range only to atom-aligned 256+256. Submit and
Unmap do not flush noncoherent host changes. A lost Unmap acknowledgement
always releases the local mapping, permanently poisons the connection, and a
subsequent Vulkan call performs zero exchanges rather than reconnecting stale
proxy IDs. Live fake contracts cover these paths. FD seal variants, stale and
cross-device generations, duplicate maps, caps, setup-ack loss, and disconnect
cleanup are source-audited rather than fault-injected live in this gate.
An unrelated E038 fake-driver regression found during the full run was fixed by
retaining the established fixed buffer identity outside explicit mirror mode;
the external-memory receiver harness now also treats an early EOF as a bounded
failure instead of spinning forever.

The required `deja "BVB Vulkan mapped memory dirty range shared mirror submit
flush"` query returned no indexed implementation. E077 reuses E034's mapped
memory lifecycle and fake contract, Decision 0003's bounded shared data-plane
direction, E014's sealed-size memfd and release/acquire discipline, E042's
setup-only descriptor transfer, E060's typed ownership, and E075/E075a's
strict/shared fail-closed setup model. No Activity/APK/launcher file changed;
there is no tablet deployment, visible-frame, Tomb Raider, benchmark, or FPS
claim.

### E077 tablet candidate transport A/B

An exact `7dd1ffe3c86bbbd510e76e00f40dd5b0d6e460f8` git archive was
built on the Galaxy Tab S8+ against clean Vulkan-Headers commit
`01393c3df0e5285b54ee6527466513f9e614be94` and the unchanged private
Turnip SHA-256 `8ac6ef78...a45`. The stale Termux evidence parser was first
fixed to understand the E075--E077 telemetry, then tightened so the strict
control keeps its mapping alive through Submit just like shared mode. All
49/49 host contracts passed after both fixes.

The final real-hardware comparison holds source, binaries, Turnip, Adreno 730,
and 2800x1752 three-image frame setup constant. Strict-kept-mapped reports
map/flush/invalidate/unmap/submit round trips `2/2/2/2/3`; E077
shared-upload-only reports `1/1/1/1/1`. That is 11 versus 5 eligible control
round trips, a 54.5% reduction for this bounded sequence. Both modes report
zero mapped-byte and GPU-fill mismatches. Shared mode also proves an ineligible
GPU-writable allocation falls back to strict opcodes 49/48 instead of silently
using the mirror.

The compact evidence is
`docs/evidence/e077-upload-memory-transport-tablet.json`; the retained tablet
evidence file SHA-256 is `6e782f8c88ab4983d9b9aee8df5c0a91134405fb9f298743f1bd1ca2bb44d545`.
The retained tablet
raw evidence SHA-256 values are `664214cc...0c0` (strict) and
`08227b17...eca` (shared). The installed E073 service/client/manifest hashes
remain unchanged, and live Steam PID 5973 plus Termux:X11 PID 27923 survived.
This is a real Turnip cross-libc transport proof only. The synthetic Activity
received the one-time FD bundle but did not import/display it, so no visible
frame, Tomb Raider, benchmark, or FPS improvement is claimed.

The exact tablet-result `deja` query returned no indexed implementation. The
run reused the E034/E014/E042/E060/E075/E075a contracts already cited above.

The next v40 animation gate was then reduced to one fail-closed command. From
the exact E077 tablet candidate, `scripts/test-rich-frame-animation-v40-termux.sh`
selects the candidate service SHA-256 `214e8b11...9a8`, producer-client SHA-256
`50a2589e...024`, glibc ICD SHA-256 `e6479b4a...9d2f`, and staged v40 APK path by default, while still accepting
explicit environment/path overrides that must satisfy those immutable hashes.
It runs the candidate in isolation and therefore does not replace installed
E073. The Activity update remains a user-confirmed Android action, and the
gate continues to stop at correlated RGBW metadata until a human confirms the
changing colors and captures a screenshot.

A read-only adversarial audit caught that service/client hashes alone did not
prove which glibc ICD the dynamically linked producer loaded. The hardened
runner now requires the adjacent ICD's exact hash, checks the producer's one
`NEEDED` entry and RUNPATH, strips inherited `LD_LIBRARY_PATH`, `LD_PRELOAD`,
and `LD_AUDIT`, and records the ICD artifact. It parses exactly one helper JSON
document with positive generation, exactly three images, and zero per-frame
Java/Binder calls; requires one import plus exactly four presents; requires
slots 0,1,2,0; and rejects any `E057_FRAME_CONSUMER_FAIL`. Logcat starts with a
bounded tail to exclude recycled-PID history. The exact integration `deja`
query returned no hit; this hardening reused E057/E060/E074/E076/E077 evidence.

Deployment review caught that the first written evidence had expanded the
abbreviated `7dd1ffe` to a nonexistent full hash. Both strict-kept-mapped and
shared-upload-only hardware runs were repeated from the unchanged exact archive
with Git's authoritative full hash above; this log and the compact evidence now
reference only the repeated raw artifacts. The same review also corrected the
historical baseline-artifact default to the actual installed E073 glibc ICD
path. That baseline identity check is separate from the newly pinned candidate
ICD that the producer really loads.

Before any E077 deployment, an exact no-clobber rollback snapshot was created at
`~/steam-arm64/bvb-backups/e073-and-selectors-pre-e077-20260821T0535PDT`.
It preserves the installed E073 ICD (`c0b3dbf3...ee3eb`), service
(`0917ef33...1241`), manifest (`77f3395e...33df`), and install stamp
(`8ca779ff...cb3`), plus the installed pre-E077 Tomb Raider probe
(`e534c471...74f`), direct wrapper (`63c4df87...137`), and Pressure Vessel
dispatcher (`f5025f54...b4c`). Every source/backup SHA-256 pair matched before
the snapshot was accepted. The operation did not replace installed files or
change the live Steam/X11 processes. This closes the deployment review's E073
rollback gap; it does not authorize deployment before the v40 RGBW visual gate.
The complete source/backup paths, modes, full SHA-256 values, unchanged Turnip
identity, and protected process start ticks are retained in
`docs/evidence/e077-predeployment-rollback-tablet.json`. The
`e077-tablet-evidence-contract` mechanically ties that rollback manifest to the
compact transport evidence, RGBW artifact pins, round-trip arithmetic, and the
compact evidence checksum.
The two authoritative 12,785-byte and 12,797-byte hardware records are archived
byte-for-byte under `docs/evidence/raw/`; their recomputed SHA-256 values are the
strict/shared raw hashes recorded by the compact evidence.
