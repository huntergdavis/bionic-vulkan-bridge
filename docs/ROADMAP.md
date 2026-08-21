# Roadmap

## 0.1 — Native Vulkan proof

- [x] Absolute-path Vulkan loader probe
- [x] Stable JSON schema and exit codes
- [x] Host fake-loader regression test
- [x] Build in Termux on the target tablet
- [x] Enumerate the tablet's physical Vulkan device
- [x] Record loader/HAL identity and probe output

## 0.2 — Cross-libc handshake

- [x] Define a fixed-width, little-endian protocol header
- [x] Build a Bionic service in Termux
- [x] Build a glibc client with the existing native glibc toolchain
- [ ] Validate version negotiation, error handling, and reconnect behavior
- [x] Benchmark warm round-trip and shared-memory notification/replay throughput

## 0.3 — Capability parity

- [x] Query Vulkan capabilities through the bridge
- [x] Compare direct and bridged capability documents field-for-field
- [x] Add Android native-window and surface feasibility probe
- [ ] Evaluate libadrenotools only if the system-loader path needs capabilities
  it cannot provide

## 0.4 — Native execution and surface host

- [x] Create a logical device and command pool through the Android driver
- [x] Submit, wait for, map, and verify a native GPU buffer operation
- [x] Trigger the same operation from glibc and compare deterministic fields
- [x] Inventory Android WSI and AHardwareBuffer/FD interop support
- [x] Create and query an Android Vulkan surface backed by a controlled
  `ANativeWindow`
- [x] Add a dedicated visible Android native-window host without taking over
  Termux:X11's EGL-owned surface
- [x] Create a swapchain, present a deterministic frame, and verify every
  consumer pixel
- [x] Add an explicit lifecycle/status handoff between the visible Activity and
  the Bionic bridge service
- [x] Add immersive-mode navigation-bar control for the eventual game host

## Game-facing milestones

- [x] Trace the Vulkan entry-point set resolved during Wine/DXVK startup
- [x] Generate registry-backed global/instance/device ownership metadata
- [x] Implement typed proxy-handle ownership
- [x] Replay a bounded client-built command batch on the Android driver
- [x] Pass and map a sealed shared-memory command region across libc
- [x] Keep Vulkan objects alive and measure repeated warm batch replay
- [x] Generate executable triangle-subset Vulkan entry-point dispatch
- [x] Pass the generated glibc triangle batch to the reusable Bionic ingress
  receiver
- [x] Transfer a sealed shared region across Android UIDs with a Binder callback
- [x] Relay the Binder-delivered descriptor to glibc/FEX with same-UID
  `SCM_RIGHTS`
- [x] Replay a Binder-brokered shared triangle batch through the visible host
  and compare complete latency with the inline control
- [x] Replace the one-shot shared region with a reusable frame ring and explicit
  producer/consumer synchronization, then measure steady-state replay
- [x] Generate an auditable runtime support policy for every measured DXVK
  startup lookup without advertising unimplemented commands
- [x] Execute the four measured global Vulkan bootstrap calls through Bionic
  and return typed glibc instance proxies
- [x] Add explicit instance destruction and stable, parented physical-device
  proxy enumeration
- [x] Bridge full base properties, queue families, memory topology, and paged
  device-extension enumeration without sharing C structure layout across libc
- [x] Bridge base features and constrained logical-device/queue creation with
  typed device/queue proxies and explicit descendant teardown
- [x] Submit an empty queue operation and execute queue/device idle waits through
  the real glibc-to-Bionic device path
- [x] Add command-pool and command-buffer proxy ownership, recording, and one
  bounded non-empty queue submission
- [x] Add buffer/memory proxy ownership and record one deterministic GPU write
  through the game-facing command buffer
- [x] Bridge bounded host-visible memory map, flush, invalidate, and unmap
  semantics through the game-facing Vulkan device
- [x] Export and import one opaque-FD allocation across two Adreno logical
  devices with deterministic byte parity
- [x] Pass E036 on hardware: export from the visible renderer, relay the FD
  across Binder/SCM_RIGHTS, and import it in the game-facing Bionic device
- [ ] Expand generated dispatch to the measured DXVK startup subset
- [x] Implement GPU external-memory transport across the Android UID boundary
- [x] Implement shared GPU synchronization for externally shared resources
- [x] Share and consume a real GPU image/frame using external memory plus
  `SYNC_FD` ordering, with deterministic GPU readback parity
- [x] Import a producer-fenced external image from one long-lived memory FD
  with no external semaphore or per-frame framework IPC
- [x] Establish one persistent native frame path using one-time Binder handle
  setup plus shared-memory/fence coordination without per-frame Binder/Java
- [x] Load the bridge as a standard ICD through Steam's real glibc Vulkan
  loader and enumerate the Bionic Adreno 730 physical device
- [x] Return real core format and image-format capabilities through the
  glibc-to-Bionic physical-device path
- [x] Create a real logical device and queue through Steam's standard loader,
  then complete queue/device idle and teardown through Bionic
- [x] Preserve bounded device-extension names across glibc-to-Bionic and enable
  an extension advertised by the real Adreno device
- [x] Expose the base Vulkan 1.1 physical-device discovery families and KHR
  aliases through Steam's standard loader
- [x] Advertise and enable one allowlisted instance extension only after its
  complete base command family is available through the ICD
- [x] Reach a rendered test triangle
- [x] Deliver the glibc-generated triangle batch to the visible host
- [x] Drive sustained per-frame shader data through the shared ring and render
  a visibly rotating native-resolution triangle
- [x] Prepare two to four real game-facing exportable swapchain images and a
  fixed-width futex ownership ring
- [x] Relay that setup bundle once into the authenticated Activity and implement
  the lifecycle-gated native import/copy-or-blit/present consumer
- [x] Connect the three-image public virtual swapchain acquire/present path to
  native producer ownership release and publish only after GPU completion
- [x] Prepare the fail-closed E074 versionCode 40 installed-Activity runtime
  gate with APK identity, authentication, E057 import/present markers, and
  bounded cleanup independent of Steam and Termux:X11
- [ ] Deploy the combined E057/E060 launch wiring and visually prove a changing
  game-facing frame in the installed Activity
- [ ] Reach a DXVK sample
- [ ] Run the fixed native-resolution Tomb Raider 2013 benchmark
- [ ] Compare FPS, frame pacing, startup time, peak RSS, and thermals against the
  current glibc/proot control
