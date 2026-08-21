# Architecture

## Goal

Use Android's native Vulkan driver path from a Linux game stack while keeping
every boundary measurable, replaceable, and independently testable.

## Constraints

The Samsung Galaxy Tab S8+ runs Android on AArch64. Its Vulkan loader and Adreno
HAL are Bionic/Android libraries. Steam, Proton, Wine, and the current FEX path
expect glibc/Linux behavior. Loading the Android HAL directly into a glibc
process is therefore not a supported ABI boundary.

Android's documented path is:

```text
Android application
  -> /system/lib64/libvulkan.so
  -> /vendor/lib64/hw/vulkan.<ro.hardware.vulkan>.so
  -> GPU
```

On the test tablet, `ro.hardware.vulkan=adreno`, so the HAL is
`/vendor/lib64/hw/vulkan.adreno.so`.

## Hybrid-first architecture

```text
Windows game
  -> DXVK / vkd3d-proton
  -> Linux Vulkan ABI in the glibc process
  -> thin client library (future)
  -> versioned transport and shared-memory command/data regions
  -> Bionic bridge service
  -> Android Vulkan loader
  -> Adreno Vulkan HAL
```

Termux remains useful as the Android/Bionic control plane: builds, launches,
logs, CPU policy, audio, and Termux:X11 integration all live there today. The
glibc layer remains because the commercial game stack depends on it. The bridge
is intended to remove graphics translation overhead and incompatibility, not to
replace unrelated working components.

## Visible host control boundary

The visible Android Activity and Termux service have different Android UIDs, so
the Activity cannot use the service's mode-0600 filesystem socket. E010 keeps
that owner-authenticated Unix socket unchanged and adds a separate, opt-in
listener bound only to `127.0.0.1`. A fresh 256-bit capability is passed to the
Activity in its launch Intent; every fixed-width lifecycle record carries that
capability and receives an explicit ACK. The capability is never returned by
the status query or written to the evidence artifacts.

The Bionic service derives Activity state only from authenticated, ordered
events. A glibc client can query the snapshot through the existing Unix
protocol. This is a low-rate control plane for creation, focus, window, and
renderer readiness—not the future Vulkan hot path. Per-call Vulkan RPC remains
out of scope; game command/data transfer must still be batched or shared-memory
based.

## Development gates

1. **Native loader proof (0.1):** a Bionic process opens the absolute Android
   loader, creates an instance, enumerates the real physical device, and emits
   machine-readable evidence.
2. **ABI handshake (0.2):** a glibc client and Bionic service negotiate a
   version and exchange deterministic messages without Vulkan calls.
3. **Vulkan capability path (0.3):** move enumeration through the boundary and
   verify results against the direct probe.
4. **Command path:** introduce handle mapping, memory ownership, synchronization,
   and batched operations, measuring latency at every stage.
5. **Game integration:** expose enough Vulkan ABI for DXVK, then A/B test a fixed
   Tomb Raider 2013 benchmark against the current path.

An RPC per Vulkan call is not the target architecture. Hot-path work must be
batched or shared-memory based; otherwise dispatch overhead can erase the native
driver benefit. The earlier gates exist to provide evidence before locking in
that larger design.

## Invariants

- Never rely on Vulkan loader search order in a hardware experiment.
- Every cross-libc message begins with a versioned, fixed-width header.
- Pointer values never cross the process boundary.
- Cross-UID Activity events require a fresh launch capability and a
  loopback-only listener; they never weaken the owner-authenticated Unix
  control socket.
- Each optimization has an A/B control and records thermals, resolution, and
  process topology.
- A failed gate is recorded; it is not rewritten as success.

The project now has a dedicated, visibly rendered, immersive Android Vulkan
host plus an authenticated lifecycle/status handoff. E011 also provides a
registry-backed inventory of the entry points resolved by the real Tomb Raider
Wine/DXVK startup path. E012 proves typed proxy ownership and one client-built
command batch replayed by the real Android driver. E013 moves that same batch
into a sealed shared-memory region passed once to Bionic, leaving only bounded
generation/offset/length/sequence metadata on the execute control path. E014
keeps the loader, instance, device, queue, allocation, mapping, command pool,
command buffer, and typed-handle table alive. On Adreno 730, 100 executions
after one excluded warm-up averaged 0.854 ms for the complete control exchange,
validation, GPU replay/wait, verification, and response; GPU submit/wait alone
averaged 0.503 ms. This synchronous transfer proof is not an FPS prediction.
E015 adds a generated glibc client dispatch for the six-command dynamic-
rendering triangle subset plus the two observed KHR aliases. Calls resolved
through `vkGetDeviceProcAddr` now encode the existing typed batch on the real
target glibc ABI. E016 adds Bionic-side swapchain image-view and pipeline
ownership and replays the same six records into the visible host. The Adreno
Android driver is Vulkan 1.1.128 and does not expose
`VK_KHR_dynamic_rendering`, so the Bionic executor validates the narrow dynamic
rendering shape and lowers begin/end rendering to a classic render pass and
framebuffer. The client batch format does not change. This compatibility
lowering produced a visually confirmed, GPU-drawn triangle at 2800x1752.

The E016 Activity currently constructs the validated batch locally. The next
controlled gate is to deliver the E015 glibc-generated batch to that visible
executor, followed by external-image synchronization as described in
[decision 0003](decisions/0003-batched-game-dispatch.md). A bridged game frame
and game input remain outside the completed result.

The current game-facing WSI direction is recorded in
[decision 0005](decisions/0005-persistent-game-frame-ring.md). The service now
prepares a bounded ring of real exportable images plus one shared futex control
page after authenticated Activity readiness. The service can relay that bundle
once to a same-UID helper, whose authenticated Binder reply installs it in the
Activity. The native lifecycle-gated consumer imports every image, copies or
blits presented slots into the Android swapchain, and releases a slot only after
its local GPU fence. E060 adds the matching game-facing three-image swapchain,
service-side acquire signaling, and synchronous producer ownership release
before ring publication. Public create still fails closed without the Activity
setup socket and exact live extent. Host contracts pass; tablet-visible changing
pixels remain unproven.

E075 adds the first opt-in game-command hot path. With
`BVB_COMMAND_STREAM=shared`, the glibc client passes one sealed-size 16 MiB
memfd during connection setup. Slots are leased only while a command buffer is
being recorded or awaits its first replay; live command-buffer proxies do not
consume shared space. `vkBeginCommandBuffer`, the currently executable
`vkCmdFillBuffer`, `vkCmdClearColorImage`, and `vkCmdPipelineBarrier2` shapes,
and `vkEndCommandBuffer` write canonical pointer-free records locally with no
socket exchange. `vkQueueSubmit2` remains the control boundary: dedicated
opcode 105 carries the sealed generation, sequence, offset, and length, while
the legacy opcode-85 wire remains unchanged. The Bionic service
prevalidates the whole submit, command topology, and typed same-device
ownership before replaying into native command buffers. The default path still
uses the strict per-call socket implementation for A/B comparison. Corrupt,
stale, unsupported, or cross-device streams fail closed before native replay.

E075a closes the shared-stream correctness gaps found in the first audit.
The service snapshots each referenced slot into private immutable memory before
validation, stages every monotonic-generation change in a shadow table, and
commits the table only after the complete submit validates. A full generation
table reclaims entries only for command buffers that the typed native context
proves are no longer live. Once opcode 105 has been accepted and replayed, the
client retires the slot even when native `vkQueueSubmit2` returns a Vulkan
error; a legal retry therefore uses the ordinary opcode-85 native handle rather
than resending an already-consumed stream. The 5-to-0 recording exchange A/B
result is unchanged. Per-submit mapped-memory flush fan-out and the global
recording mutex remain later performance/scalability work (E077); E075a makes
no FPS or visible-frame claim.
