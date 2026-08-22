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

E076 adds two bounded pointer-free shared records without changing the legacy
RPC path: record 10 carries up to four general image `Barrier2` entries and
record 11 carries four raw clear-color words plus up to four color ranges.
The client accepts only typed same-device images (including virtual-swapchain
images), poisons invalid void-command shapes, and retains RPC 102/103 for the
strict A/B path. The service decodes only E075a's private snapshots,
prevalidates the complete batch, then reconstructs native Vulkan structures.
The host proof produces red, green, blue, and white through four acquire,
record, binary-synchronized Submit2, and present cycles; the concurrent sink
releases slots in the observed order 0,1,2,0, proving three-image reuse and
zero command-recording RTT. This proves native producer replay, not Activity
import, tablet-visible pixels, Tomb Raider's first frame, or FPS.
The bounded `test-rich-frame-animation-v40-termux.sh` handoff reuses the
installed byte-identical v40 Activity without an APK change. Its default paths
and immutable hashes select the isolated E077 candidate service and producer
client plus the adjacent glibc ICD, so the gate does not replace the installed
bridge. The producer's NEEDED/RUNPATH is checked against that exact ICD and
inherited loader overrides are removed. The helper must report a positive
generation, an exact three-image ring, and zero per-frame Java/Binder calls.
One import and exactly four presents must match RGBW slots 0,1,2,0; any native
consumer-failure marker rejects the proof. The gate correlates each producer
`E076_FRAME_EXPECTED` RGBW sequence/slot with the Activity's
`E057_FRAME_PRESENTED` generation/sequence/slot, and still leaves visual
confirmation and screenshot status explicitly pending.

E077 adds an independent, opt-in upload-memory data plane. With
`BVB_MAPPED_MEMORY=shared`, only memory already bound exclusively to buffers
whose usage proves GPU-read-only is eligible. Transfer-source, uniform,
index, vertex, and indirect buffers qualify; unbound memory, every image,
transfer destinations, storage, shader-device-address, and transform-feedback
buffers keep the strict path. Bind-after-map checks prevent later widening an
eligible mirror to GPU-writable use. This is deliberately not general coherent
`vkMapMemory` support: device-to-host completion tracking remains deferred.

An eligible Map passes one sealed-size memfd to the service. Fixed pointer-free
opcodes 106--109 carry setup, Flush, Invalidate, and Unmap metadata. A private
baseline makes Submit copy only host-diverged spans, so unchanged native bytes
are not overwritten and no opcode-48 bulk writes occur. Noncoherent Map does
not read or invalidate native memory; explicit Flush/Invalidate copy the exact
caller span while native maintenance expands only to the required atom
boundaries, and Unmap never flushes. Uncertain setup or Unmap acknowledgement
permanently poisons the connection, releases local Unmap state, and forbids
reconnecting stale proxies. The strict default measures 2/2/2/2/3
map/flush/invalidate/unmap/submit exchanges; an eligible upload measures
1/1/1/1/1 with opcodes 106/107/108/109/47, while an ineligible mapping still
uses strict opcodes 49/48. This is a host upload-transport contract, not a
tablet deployment, visible frame, Tomb Raider run, benchmark, or FPS result.

E078 removes the socket/control mutex from the opt-in shared command-recording
path without changing the strict default or any wire record. Each command
buffer owns its recording mutex, so different command buffers can build their
disjoint shared slots concurrently. A dedicated bounded allocator protects
only slot leases and sequence assignment. Typed buffer/image ownership checks
use a read-mostly resource/swapchain registry; create/destroy remains
control-serialized and takes the matching write lock. `vkQueueSubmit2` retains
the control mutex for the socket exchange, then locks the bounded set of
submitted command buffers before reading or retiring their stream state. Lock
order is control, command buffer, then slot allocator; ownership reads are
released before recording locks.

This does not relax Vulkan external synchronization: the application must
still serialize access to each command buffer and its command pool, while
distinct command buffers may record on different threads. The focused host
contract starts two threads together, records 256 fills into each of two
command buffers with zero recording exchanges, and submits both immutable
streams for whole-batch validation and native replay. Existing poison,
cross-device, stale/corrupt, and non-success acknowledgement contracts remain
in force. This is host synchronization/replay evidence only; it has not been
deployed and carries no Tomb Raider or FPS claim.

E079 adds a default-off, bounded first-real-rejection diagnostic for the global
game ICD. `BVB_FIRST_REJECTION_DIAGNOSTIC=1` changes only dispatch pointers
returned to that process. Registry-generated exact-signature proxies count
actual executable calls and report the first negative `VkResult`. Names that
the E011 trace resolved but the active policy still classifies as required and
unimplemented receive exact-signature diagnostic stubs; a stub reports only
when the game actually invokes it, never when DXVK merely resolves it. Generated
platform-protected names are skipped, so the six existing Xlib/Xcb/Wayland WSI
entry points keep their real executable pointers; private and probed-null names
remain null.

Void command failures retain their first canonical entry, bounded shape, and
reason on the command-buffer proxy. The diagnostic emits that record only when
End observes the poisoned batch, including the typed command-buffer ID and
shared-stream generation. One process emits at most one
`BVB_FIRST_REJECTION` line; E079a freezes the winner snapshot and later events
take the atomic emitted fast path. With the selector absent or not exactly
`1`, resolvers return the original raw pointer or null and command recording
keeps the established E075/E077 behavior. This is a diagnosis mechanism, not a
new supported Vulkan gate: the next entry is determined only by a bounded real
Tomb Raider invocation, not by resolver order.

E079a makes that diagnostic fail closed and race-safe. A required `vkCmd*` void
stub does not emit at the call site: it poisons the real command-buffer proxy,
and `vkEndCommandBuffer` owns the process's sole record with the typed command
ID, shared-stream sequence, and `end_poison=1`. A required non-command void stub
cannot imply success by returning; after winning and emitting its record, the
opt-in diagnostic process exits immediately with status 86. This exit applies
only to exact-selector diagnostic runs.
E079a therefore supersedes E079 for any deployment: the E079 candidate must not
be deployed because a non-command void stub could return and a generated
command stub could emit before its real command-buffer poison reached End.

The first winner is selected and copied under one state mutex, then the mutex is
released before one bounded `write(2)`. The record is capped below `PIPE_BUF`,
is never retried, and all later calls take an atomic already-emitted fast path
instead of the state mutex. Negative results from the three executable
platform-surface create functions now enter the same diagnostic; their
presentation-support `VkBool32` queries retain their real boolean meaning and
are not treated as errors. Generated pointer erasure is guarded by an exact
PFN-size static assertion. These changes add diagnosis only, not Vulkan support
or a visible-frame claim.

E080 removes repeated client ownership-registry reads from the opt-in shared
recording path. Each command buffer has a 16-entry direct-mapped positive cache
of exact typed buffer/image IDs. A hit remains under that command buffer's
stream mutex. A miss takes the established registry read lock and performs the
same typed same-device lookup; only a positive result is cached. An exact-key
mismatch is a collision, so it revalidates and replaces instead of aliasing.
Every Begin/rerecord and successful command-pool reset clears the cache and its
observation counter.

On a miss, the client snapshots the recording sequence, releases the command
buffer mutex, takes and releases the registry read lock, then reacquires the
command buffer and verifies that sequence before inserting. Registry and
command-buffer locks therefore remain non-overlapping as in E078. Registry
writers remain control then registry write. Submit remains control then command
buffer, and slot allocation remains command buffer then slot allocator. Positive
entries rely only on Vulkan's existing object-lifetime and external-sync rules
for one recording; the application may not concurrently destroy an object it
is recording. Rerecord clears any prior positive before a later reference.
If an invalid application destroys a positively cached resource after End but
before Submit2, the client may still transmit the sealed stream. The Bionic
service remains authoritative: it snapshots and prevalidates the entire stream
against live typed same-device native objects before any replay. The adversarial
host contract destroys such a buffer and verifies both rejection and an
unchanged native-memory sentinel. Strict mode and all opcodes/wire records are
unchanged. This is a bounded client lookup reduction, not native validation,
tablet, Tomb Raider, or FPS evidence.

E092 replaces cross-driver DMA-BUF frame images with Android Hardware Buffers.
Private Turnip imports each buffer into the game-facing virtual-swapchain
image; Samsung Vulkan imports the same allocation into the fullscreen Activity.
Three native handles plus one futex control region cross setup exactly once.
Acquire/present ownership and sequence changes stay in native Vulkan plus the
shared ring: there is no per-frame Java, Binder, socket, or FD transport. The
Activity copies the imported producer image into its Android swapchain, so the
remaining steady-state performance cost is one consumer-side GPU copy/blit and
the current synchronization boundary, not an additional Linux graphics stack.

E093 retains E056's pointer-free descriptor wire and typed native ownership,
but removes its sampler-only layout/pool bootstrap restriction. Layout and pool
creation now accept Vulkan's eleven core descriptor types; immutable samplers,
extension-only descriptor types, unsupported flags/pNext shapes, and malformed
counts remain fail-closed. Descriptor writes are deliberately unchanged, so
this gate does not imply that arbitrary game descriptor updates are executable.

E094 keeps the same AHardwareBuffer production path on Android and extends the
cross-process fake-driver contracts to execute that platform branch. Android
test sinks receive and retain the native handles with
`AHardwareBuffer_recvHandleFromUnixSocket`, then release them after the service
and client finish. Python timeout sockets are nonblocking at the descriptor
level, so the adapter treats `EAGAIN` as a bounded readiness/retry condition;
other native-handle errors still fail immediately. Non-Android hosts retain the
original DMA-BUF/FD contracts; no production transport selector or runtime
fallback is added.

E100 carries descriptor template updates as canonical typed values, never as
the application's pointer-shaped template blob. Template creation retains the
bounded entry layout independently on the glibc client and Bionic service. The
client translates each 24-byte DXVK legacy descriptor into typed bridge IDs;
the service verifies its own metadata and reconstructs native Vulkan records in
Bionic-owned storage. Descriptor-set binding is a strict RPC or local shared
command record, preserving zero recording socket round trips in shared mode
and authoritative whole-stream validation at Submit2.

E101 routes the first complete DXVK swapchain-blit render bundle through the
same global command-buffer model. Strict mode sends one canonical record with
generic opcode 114; shared mode appends records locally and retains Submit2 as
the first socket boundary. Dynamic-rendering attachments, graphics pipelines,
pipeline layouts, image views, and descriptor sets are resolved to Bionic-owned
native handles only after typed same-device ancestry validation. General push
constants are capped at 256 bytes. This deliberately covers a coherent draw
bundle rather than one resolver name, while preserving fail-closed shapes and
the no-application-pointer ABI.

E102 makes Vulkan mappings representable by 32-bit Windows applications even
though the client ICD is a 64-bit AArch64 library. Both strict anonymous
shadows and eligible shared memfd mirrors try a bounded address below 4 GiB.
Each candidate uses `MAP_FIXED_NOREPLACE`, so the bridge can never overwrite a
Wine or application mapping. If no safe low hole exists, the original normal
mapping is retained for a possible 64-bit caller; Wine's 32-bit thunk can then
fail truthfully. No pointer crosses the glibc/Bionic wire, and the native
Bionic mapping remains unconstrained.

E103 carries the two exact families exposed after E102 without widening the
per-call socket hot path. `VkFormatProperties3` uses one 24-byte response with
the native driver's 64-bit format feature masks. General dynamic-rendering
graphics pipelines use the existing FD-bearing pipeline opcode with schema 2:
a sealed, at-most-256-KiB memfd contains a versioned ABI-size header and a
pointer-free graph of bounded relative offsets. The Bionic service privately
maps the blob, validates every offset/count/sType/pNext and pipeline-layout
lineage, patches only service-local pointers, and invokes Turnip once. No
glibc pointer, per-frame Binder call, or unbounded pipeline array crosses the
bridge.

E104 advances that graph to schema 3 for the exact rasterization extension
state used by pinned DXVK. A terminal
`VkPipelineRasterizationDepthClipStateCreateInfoEXT` is copied into the same
sealed blob, covered by an ABI-size header field, validated on the private
service snapshot, and reconstructed as a Bionic-local pNext chain. Other
rasterization extension state remains fail-closed, and the pipeline still
costs one setup transaction rather than one transaction per nested struct.

E106 generalizes dynamic rendering without changing the hot-path transport.
E107 similarly keeps record ID 10 while widening synchronization2 to bounded
memory, buffer, and image arrays. Strict clients send one pointer-free record;
shared clients append it locally, while the Bionic service remains the only
side that reconstructs native barriers and authoritatively resolves buffer and
image ownership.

E108 treats vertex/index binding and indirect drawing as one hot-path family.
Nine canonical record IDs cover thirteen core/KHR/EXT public names without
adding per-command sockets in shared mode. Fixed arrays contain typed buffer
IDs and scalar offsets/sizes/strides only; the Bionic service authoritatively
checks same-device ancestry and reconstructs native handles and arrays. Null
vertex/index slots remain representable for the enabled maintenance/null-
descriptor feature set, while malformed or cross-device streams poison the
whole command buffer before native replay.

E109 similarly groups the scalar graphics dynamic-state surface rather than
advancing one resolver name at a time. Thirty-one core/EXT public names map to
nineteen canonical kinds in fixed command record 33. The record contains only
an enum, a bounded value count, and raw scalar/IEEE-754 words; NaN, infinity,
invalid enums, invalid booleans, and noncanonical inactive slots fail closed.
Shared recording appends locally with no socket exchange, while the strict
path sends one immediate record and the Bionic service reconstructs each
native command call after resolving the owning command buffer.

E110 groups the adjacent clear family: `vkCmdClearDepthStencilImage` and
`vkCmdClearAttachments`. Fixed records 34 and 35 bound image subresource
ranges, clear attachments, and clear rectangles; no glibc pointer crosses the
wire. Shared clients append locally with zero recording RPC, while strict
clients send one immediate record. The Bionic service rechecks typed image
ancestry and reconstructs every native range, attachment, rectangle, and
clear value before calling Turnip.
One fixed 600-byte, pointer-free record carries up to eight color attachments,
optional depth and stencil attachments, resolve image views, clear-value bits,
render flags/area/multiview, and terminal attachment-feedback-loop metadata.
The glibc client caches only typed ownership; the Bionic service revalidates
every image-view/device relationship and reconstructs all native arrays and
pNext nodes locally. Strict mode retains one immediate-record RPC, while shared
mode appends the whole rendering command locally with zero recording RPC.
