# bionic-vulkan-bridge

Experimental infrastructure for giving a glibc Linux game stack controlled
access to Android's native Bionic/Vulkan graphics path.

The project starts with an intentionally narrow question: can a native Android
process load `/system/lib64/libvulkan.so` and enumerate the tablet's real Vulkan
device reproducibly? Only after that gate passes will the project introduce a
versioned glibc-to-Bionic boundary.

## Why this exists

Steam, Proton, Wine, and FEX expect a glibc/Linux userspace. Android GPU drivers
are Bionic libraries exposed through Android's Vulkan loader and vendor HAL.
Replacing the whole working Linux stack would discard useful compatibility
work. The initial architecture therefore keeps the game stack and moves only
the graphics-driver boundary into a small Bionic component.

```text
Steam / Proton / Wine / FEX (glibc)
                |
       versioned bridge ABI
                |
 Bionic bridge + Android Vulkan loader
                |
        Adreno Vulkan HAL
```

This is not yet a game-ready bridge. Version 0.1.0 is the native Vulkan probe
and the test framework needed to distinguish real progress from assumptions.
The first hardware gate passed on the Galaxy Tab S8+: the probe directly loaded
Android's system Vulkan loader and enumerated its Adreno 730 in 345 ms. See
[E001](docs/EXPERIMENT_LOG.md#e001--direct-bionic-vulkan-enumeration-2026-08-18).
The next gate also passed: an AArch64 glibc client negotiated protocol v1 with
a separate Bionic service that could see Android's Vulkan loader. See
[E002](docs/EXPERIMENT_LOG.md#e002--glibc-client-to-bionic-service-handshake-2026-08-18).
Gate 0.3 then moved Vulkan enumeration through that boundary: every bridged
Adreno capability field matched a fresh direct-probe control. See
[E003](docs/EXPERIMENT_LOG.md#e003--bridged-vulkan-capability-parity-2026-08-18).

## Build and test on a normal Linux host

Requirements: CMake 3.20+, a C17 compiler, Git, and Python 3. Vulkan headers are
fetched at the exact revision recorded in `CMakeLists.txt`; no host Vulkan
runtime is needed.

```sh
./scripts/check.sh
```

The test builds a fake Vulkan loader, runs the real probe against it, and
validates the emitted JSON and failure behavior.

## Run the hardware probe in Termux

```sh
./scripts/build-termux.sh
./build/bvb-vulkan-probe > probe.json
python -m json.tool probe.json
```

The default loader path is explicitly `/system/lib64/libvulkan.so`. This is
important: an unqualified `libvulkan.so` can resolve to Termux's own loader and
invalidate the experiment. A different absolute path can be supplied with
`--loader`.

Exit codes are stable: `0` success, `2` usage, `3` dynamic-loader failure,
`4` Vulkan failure or no physical device, and `5` allocation failure.

With Termux's `glibc`, `glibc-runner`, and `gcc-glibc` packages installed, run
the cross-libc handshake gate with:

```sh
./scripts/test-cross-libc-termux.sh
```

The script verifies that the client names the Termux glibc ELF interpreter,
requests Vulkan capabilities from the independently Bionic-linked service, and
compares every result field with a fresh direct Bionic probe.

## Project boundaries

- This repository owns reusable bridge source, protocol definitions, probes,
  tests, architectural decisions, and release-level results.
- [`steamclienttermux`](https://github.com/huntergdavis/steamclienttermux)
  remains the deployment project and the detailed game/benchmark experiment
  chronicle.
- [`termux-glibc-compat`](https://github.com/huntergdavis/termux-glibc-compat)
  remains the reusable fast glibc launcher layer.

See [Architecture](docs/ARCHITECTURE.md), [Roadmap](docs/ROADMAP.md), and the
[experiment ledger](docs/EXPERIMENT_LOG.md).

## Upstream references

- [Android NDK stable APIs: Vulkan](https://developer.android.com/ndk/guides/stable_apis)
- [AOSP: Implement Vulkan](https://source.android.com/docs/core/graphics/implement-vulkan)
- [Khronos Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
- [libadrenotools](https://github.com/bylaws/libadrenotools)

## License

[MIT](LICENSE)
