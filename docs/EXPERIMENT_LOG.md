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
