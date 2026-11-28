# Hybrid guest memory ownership (experimental)

Baseline: 144cf090. Keep the existing renderer branch unchanged.

## Goals
Reduce synchronous readbacks, queue submits, page protection churn and duplicate backing. Do not weaken guest synchronization or substitute stale data. 30 FPS is a benchmark goal, not a guarantee.

## Memory classes
- Imported host backing: only if extension, buffer usage, host pointer compatibility, allocation lifetime and mapping support are verified.
- Persistent mapped Vulkan buffer: explicit copies to guest backing unless it is truly aliased. Prefer host-cached for CPU reads; benchmark GPU access bandwidth.
- Device-local buffer/image: GPU-intensive workloads; retain existing transfers as fallback.

## Ownership model
Use physical backing identity + offset + mapping generation rather than a virtual address alone. Each interval records latest writer, last GPU read/write completion ticks, dirty byte/page ranges and image representation versions. CPU accesses must wait on conflicting GPU writes (or reads before a CPU write). Imported host memory requires explicit Vulkan visibility dependencies and cache maintenance as appropriate. Never unmap an allocation while any recorded command can reference it.

## Incremental rollout
1. Capability reporting (this commit). No guest buffers imported.
2. Verify aligned pointer import, memory type compatibility, BDA, read/write visibility and fallback using a synthetic allocation. Do not assume driver support from extension presence.
3. Add a bounded mapped-buffer path for high readback-frequency owners; keep existing tracker and exact writeback.
4. Track physical aliases and generations, with tests for remap, eviction, overlapping descriptors, CPU/GPU races and texture aliasing.
5. Introduce more precise write tracking and asynchronous publication at verified guest synchronization boundaries. Keep synchronous fallback for immediate dependencies.
6. Measure same-scene frame times, readback counts/bytes, queue submits, RAM/VRAM and visual correctness. Compare against unchanged baseline.

Do not promote arbitrary guest memory to imported storage until backing lifetime and Windows mapping constraints are established. Vulkan device memory mapping is not a substitute for importing an existing guest address. Do not hardcode Requiem guest virtual addresses or disable effects to reach a performance target.
