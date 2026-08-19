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
- [ ] Expand generated dispatch to the measured DXVK startup subset
- [ ] Implement GPU external-memory and synchronization strategy
- [x] Reach a rendered test triangle
- [x] Deliver the glibc-generated triangle batch to the visible host
- [ ] Reach a DXVK sample
- [ ] Run the fixed native-resolution Tomb Raider 2013 benchmark
- [ ] Compare FPS, frame pacing, startup time, peak RSS, and thermals against the
  current glibc/proot control
