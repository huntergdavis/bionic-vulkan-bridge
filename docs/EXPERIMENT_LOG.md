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
