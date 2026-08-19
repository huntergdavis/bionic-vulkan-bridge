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
The native execution gate now also passes: both the direct Bionic probe and a
glibc-triggered service request submitted and verified an Adreno command buffer
with zero data mismatches. See
[E004/E005](docs/EXPERIMENT_LOG.md#e004--native-vulkan-command-submission-2026-08-18).
E006 then created a separately owned `AImageReader`/`ANativeWindow`, created an
Android Vulkan surface, and queried real Adreno presentation capabilities four
times with clean teardown. This is WSI proof, not yet a swapchain or frame.
E007 completes the controlled presentation loop: it creates a six-image
swapchain, clears an acquired image to magenta, presents it, then acquires the
consumer image through Media NDK and verifies all 4,096 RGBA pixels.
E008 packages the same presentation architecture in a standalone Android
`NativeActivity`. The installed app visibly displayed the opaque-magenta Vulkan
frame fullscreen on the tablet; Android's bottom navigation icons remained
visible. This proves a dedicated visible host, not game integration.
E009 adds immersive-sticky system-UI control without changing the renderer.
The magenta frame remained visible while the tablet's navigation controls were
fully hidden, and immersive mode is restored whenever the Activity regains
focus.
E010 adds an explicit Activity-to-service lifecycle handoff. A fresh 256-bit
launch capability authenticates events over an opt-in loopback listener; the
existing owner-authenticated Unix control socket then exposes the accepted
state to glibc. The hardware gate reported an active, focused, renderer-ready
2800x1752 window and rejected a deliberately invalid token.
E011-E015 trace the real DXVK startup dispatch, add typed handles and
shared-memory batching, keep the Bionic Vulkan context warm, and generate an
executable glibc dispatch for the six-command triangle subset. E016 replays
that same batch shape into the visible Android host. Because the tablet's
Android Vulkan 1.1 driver does not expose `VK_KHR_dynamic_rendering`, Bionic
translates the encoded begin/end-rendering commands to a classic render pass.
The authenticated hardware gate reported a focused, renderer-ready 2800x1752
window, and the triangle was visually confirmed on the tablet. The batch is
still constructed inside the Activity. E017 passes that exact generated batch
from a glibc process to a same-UID Bionic ingress receiver in about 0.250 ms on
the tablet. E018 then isolated the real Android boundary: APK v7 reached a
focused native-resolution window, but stock Android denied Termux's cross-UID
abstract Unix socket connection before setup (`EACCES`). E019 changes only that
boundary: APK v8 accepts an authenticated inline batch over loopback TCP. The
real glibc client delivered its 200-byte six-command stream, and Bionic
validated, replayed, submitted, and presented it through Adreno at 2800x1752 in
a 14.6 ms synchronous round trip. This proves visible external replay across
Android UIDs. E020 then adds the Android-supported descriptor broker: APK v10
returns a sealed 4 KiB memfd through a capability-authenticated Binder callback
to a Termux-UID helper. A wrong capability returns `-EACCES`; the valid region
and exact marker validate with no TCP payload copy. E021 relays that descriptor
once from the Termux helper to a real glibc process with same-UID `SCM_RIGHTS`.
The complete connect/send/validate/ACK took 1.603 ms, while the glibc
receive-plus-validation portion took 0.179 ms. These are transport-only
measurements; E022 will replay the shared batch through the visible renderer
for a fair comparison with E019's complete inline frame.

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

Run only the native command-submission gate with:

```sh
./build/bvb-vulkan-selftest | python -m json.tool
```

This creates a Vulkan device and command pool, fills a 4 KiB host-visible
buffer on the GPU, synchronizes it for host access, and verifies all 1,024
words. It also inventories the WSI and external-memory extensions needed by the
next surface phase.

Query an independently owned Android surface with:

```sh
./build/bvb-vulkan-surface-probe | python -m json.tool
```

The probe dynamically opens the Android Vulkan and Media NDK libraries, creates
a 64x64 RGBA8888 `AImageReader` window, enables the Android surface extensions,
and reports presentation queues, capabilities, formats, and modes. It does not
create a swapchain, present pixels, or touch Termux:X11's existing surface.

Run the complete non-visible presentation/readback gate with:

```sh
./build/bvb-vulkan-present-selftest | python -m json.tool
```

This uses a CPU-readable `AImageReader`, FIFO swapchain, transfer clear, and
Media NDK plane readback. Success requires every 64x64 pixel to equal opaque
magenta after presentation. Termux:X11's surface is not used.

Build the dedicated visible Android host from Termux with:

```sh
./scripts/build-visible-host-termux.sh
```

The script compiles a no-Java `NativeActivity` library, packages it with
`aapt`, aligns and debug-signs the APK, verifies the signature, and prints the
artifact path and ELF dependencies. Generated APKs and the debug key stay in
ignored `out/`. The E008/E009 acceptance test is a visibly magenta, immersive
Android Activity; it does not use or replace Termux:X11.

After installing the generated APK, run the complete E010 lifecycle gate with:

```sh
./scripts/test-visible-host-lifecycle-termux.sh
```

The harness creates a fresh launch token, starts an ephemeral loopback ingress,
proves invalid-token rejection, launches the Activity with private Intent
extras, queries the resulting state through a glibc client, and writes evidence
under ignored `out/`. The token is not printed or retained in those artifacts.

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
