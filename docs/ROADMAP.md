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
- [ ] Benchmark round-trip and bulk shared-memory throughput

## 0.3 — Capability parity

- [x] Query Vulkan capabilities through the bridge
- [x] Compare direct and bridged capability documents field-for-field
- [ ] Add Android native-window and surface feasibility probe
- [ ] Evaluate libadrenotools only if the system-loader path needs capabilities
  it cannot provide

## Game-facing milestones

- [ ] Trace the minimum Vulkan entry-point set exercised by DXVK startup
- [ ] Implement generated dispatch and handle ownership
- [ ] Implement external/shared memory and synchronization strategy
- [ ] Reach a rendered test triangle
- [ ] Reach a DXVK sample
- [ ] Run the fixed native-resolution Tomb Raider 2013 benchmark
- [ ] Compare FPS, frame pacing, startup time, peak RSS, and thermals against the
  current glibc/proot control
