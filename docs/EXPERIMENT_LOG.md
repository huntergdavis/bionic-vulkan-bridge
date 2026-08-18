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
