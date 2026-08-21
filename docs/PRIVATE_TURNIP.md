# Private Turnip build

BVB uses a private Mesa Turnip ICD so the Bionic service can reach the Adreno
KGSL driver without replacing Android's system Vulkan HAL. The artifact is an
Android API 34 AArch64 shared library; it is not a glibc library and must not be
registered as a system HAL.

## Why maintenance5/6 need a private exception

Mesa 26.2 builds Android drivers with `ANDROID_STRICT`. Its generated table
filters KHR extensions according to the Android release where CTS first allowed
drivers to publish them. Both `VK_KHR_maintenance5` and
`VK_KHR_maintenance6` therefore default to API 35.

That publication policy is separate from Turnip's implementation. On Turnip,
both extensions are advertised for Vulkan 1.1+ devices and both feature bits
are implemented as true in `src/freedreno/vulkan/tu_device.cc`. The BVB private
artifact runs on a Vulkan 1.4 Turnip Adreno 730 and is loaded directly rather
than published to Android applications through the HAL.

The patch keeps `ANDROID_STRICT` enabled and changes only the two generated
conditions from API 35 to API 34. It deliberately leaves maintenance7 at API
36 and leaves every other Android condition unchanged. This is safer than
disabling strict mode, which would expose every driver-supported KHR extension
regardless of Android's allowlist.

## Reproducible build

The build is pinned to Mesa 26.2.0 archive SHA-256
`efd4bb08cdb7c365a812cd4e6c9202ab55b2f22cdcd13c7d6c4f9647b799a4ef`
and Android NDK r29 (`29.0.14206865`) archive SHA-1
`87e2bb7e9be5d6a1c6cdf5ec40dd4e0c6d07c30b`. The script applies the private
resolver export and maintenance5/6 patches before configuring an API 34,
release-mode, freedreno/KGSL-only build:

```sh
./scripts/build-private-turnip-maintenance56.sh
```

Set `BVB_TURNIP_CACHE_ROOT` to a directory containing the pinned Mesa archive,
unpacked NDK, and Python virtualenv. Set `BVB_TURNIP_BUILD_ROOT` to relocate the
ignored build tree. The script is incremental and refuses a source tree that
is neither pristine nor exactly patched.

The ignored `artifacts/` directory contains:

- `libvulkan_freedreno.so`, the Bionic private ICD;
- `bvb-private-turnip-maintenance56-probe`, an API 34 AArch64 native probe;
- `build-manifest.json`, including input and output hashes.

The host validator confirms the generated API thresholds, Turnip source
support, AArch64 ELF type, Bionic dependency shape, private resolver export,
and extension strings. It cannot prove GPU execution on an x86_64 build host.

## Native acceptance gate

Run the generated probe in an Android/Termux process that can access KGSL,
passing an absolute path to the candidate library:

```sh
./bvb-private-turnip-maintenance56-probe \
  /absolute/path/libvulkan_freedreno.so
```

A passing result proves all of the following against the candidate artifact:

1. the raw private ICD creates a Vulkan 1.3 instance and selects Turnip;
2. both extensions enumerate with their spec versions;
3. both maintenance feature queries return `VK_TRUE`;
4. `vkCreateDevice` accepts both extension names and enabled feature structs;
5. the device waits idle and tears down cleanly.

Do not promote the artifact based only on host checks. The native probe must
pass on the target device first.

## Risks and limits

- This intentionally bypasses an Android CTS publication-level restriction for
  a private process-local ICD. It is not an upstream Android allowlist change.
- Turnip source support plus one create-device probe is narrower than the full
  Vulkan conformance suite; later use of maintenance commands still needs game
  and bridge-path coverage.
- The exception is justified for the measured Vulkan 1.4 Adreno 730 target. A
  different GPU or older Turnip-capable device must pass the same native gate.
- This gate exposes two native capabilities. It does not by itself implement
  desktop swapchain translation or make Tomb Raider render through BVB.
