# Roadmap

## 0.1 — Native Vulkan proof

- [x] Absolute-path Vulkan loader probe
- [x] Stable JSON schema and exit codes
- [x] Host fake-loader regression test
- [x] Build in Termux on the target tablet
- [x] Enumerate the tablet's physical Vulkan device
- [x] Record loader/HAL identity and probe output

## 0.2 — Cross-libc handshake

- [ ] Define a fixed-width, little-endian protocol header
- [ ] Build a Bionic service in Termux
- [ ] Build a glibc client in the existing native glibc rootfs
- [ ] Validate version negotiation, error handling, and reconnect behavior
- [ ] Benchmark round-trip and bulk shared-memory throughput

## 0.3 — Capability parity

- [ ] Query Vulkan capabilities through the bridge
- [ ] Compare direct and bridged capability documents byte-for-byte where valid
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
