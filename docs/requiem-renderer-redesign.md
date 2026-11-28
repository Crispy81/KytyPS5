# Requiem renderer redesign

## Capture baseline

The screenshot and `_kyty.txt` identify Resident Evil Requiem PPSA31246, version 01.310.000, on a Ryzen 7 7700X and Radeon RX 9070 XT. The capture matches fork commit `475956fc` on the Requiem shader-precompile branch. This patch targets the open checkout based on `main` at `7e904d4b`. Both revisions contain the exact-range HTile fix. The capture branch also contains separate BC alias, MSAA, and shader-cache changes. Use same-branch before/after binaries when isolating this patch's effect.

| Signal | Screenshot | What to investigate |
| --- | --- | --- |
| Frame time | 90.3 ms, 11.1 FPS | The 60 FPS budget is 16.67 ms. This scene needs roughly a 5.4× reduction in average frame time. |
| Synchronous buffer readbacks | 1,323/s, 428 ms/s | Roughly 119 readbacks and 39 ms of blocking per frame at the observed rate. This time overlaps other host timings. |
| Queue submits and texture uploads | 1,520 submits/s, 2,368 MB/s uploaded | Identify copies and submissions that precede a real consumer and memory ranges uploaded despite unchanged contents. |
| Present and acquire | 0.6 ms present, 0.1 ms acquire | Leave presentation pacing alone while investigating the larger costs. |
| Graphics | Green and cyan speckles on the wet pavement and near silhouettes | Track which pass first creates the affected pixels. The screenshot alone cannot identify the resource, format, or pass. |

The log contains 27 readback profile records. The median record covers 1,497 reads, 551 writes, and about 264 MiB of transfers. Two recurring owners account for 76% of the recorded bytes. One 24 MiB owner is write-fault dominated and a separate 32 MiB owner is read-fault dominated. The capture already widens a 512 KiB window to 2 MiB when it detects nearby accesses. Measure both owners separately when evaluating further widening. A smaller window can reduce bytes while increasing blocking waits, and larger windows can copy dirty ranges the guest never reads.

The log starts with an empty shader cache and records 88,661 direct-map diagnostic records and 115,281 split-job descriptor records. Capture both cold and warm runs and use the same logging settings in every comparison. DCC/CMASK readback time was zero in the screenshot. The log reports BC5 and BC6H storage fallback, which warrants checking format handling if the affected pass actually uses those resources.

## This change

Release builds skip routine direct-map and split-decode logging while retaining failure diagnostics. The overlay counts synchronous downloads and times the wait and publication phases. Capture build `475956fc` started the equivalent timer before staging allocation, so compare updated binaries with identical instrumentation. The scheduler avoids timeline queries for an unsubmitted tick and checks the cached completed tick before polling. Dirty range subtraction retains existing map nodes when trimming a range.

The readback predictor can expand to 8 MiB for repeated faults across nearby regions of a large buffer. It falls back toward smaller windows when repeated faults hit one region, preserves the full requested range, and copies only GPU-dirty bytes. The transfer path merges adjacent copies, limits barriers to the copied source span, and reuses synchronous copy records. Indirect compute argument reads use the existing precise guest-memory reader. Reads that require an image-to-buffer synchronization stay on the image-aware path when a registered texture overlaps.

The HTile path checks a live owner and exact range under one lock, rejects dispatches with other visible writes, and keeps rejected metadata dispatches out of the generic buffer-fill shortcut. The existing clear substitution still infers coverage from the descriptor. Proving uniform coverage and the correct HTile value requires inspecting the guest shader and depth pass.

## Renderer design

1. **Resolve ownership for the consumed bytes.** Route small CPU consumers through the guest-memory reader. For GPU-authored data, use the last writer's synchronization and publish dirty bytes before the CPU can observe them. Preserve dirty bytes across partial CPU writes and through virtual aliases. Keep GPU-only indirect parameters on the GPU where the pipeline can use an indirect command.
2. **Batch transfers at the consumer boundary.** Collect dirty ranges that must become CPU-visible together, pack aligned regions into staging memory, and use one submission and timeline wait for the batch. Split at actual guest synchronization or ownership boundaries. Make read-side prefetch decisions separately from write-fault preservation. Keep pending staging allocations alive until GPU completion and CPU publication.
3. **Keep texture transfers tied to content changes.** Measure upload bytes by resource and subresource. Reuse a host image only while its backing generation, format, tiling and view interpretation still match. Recheck alias invalidation and metadata transitions when GPU writes become visible through a different view.
4. **Make clear substitution prove equivalence.** Require an exact live HTile owner and range, a proven clear pattern, and evidence that the dispatch has no other visible writes. Update metadata state atomically with validation. Otherwise execute the guest shader. Trace depth and color passes to isolate the first visible difference before changing decompression or sampling.

## Windows validation

Use a Windows GitHub Actions artifact from the candidate commit, then replay the same 1920×1080 save and camera route on the RX 9070 XT. Record cold and warm runs. Compare unique guest frames, missed 60 Hz presentation slots, frame-time percentiles, synchronous readback count and time, transferred bytes and submissions. Constant 60 FPS requires each measured warm-run frame to meet its 16.67 ms deadline with no repeated-frame substitution.

With the overlay hidden, compare the first affected render pass and final output at matched camera positions against a known-good reference. Check moving geometry, wet pavement, headlights and shadows for speckles, stale depth or temporal shimmer. Include overlapping guest mappings, partially overwritten buffers, mismatched HTile ranges and shaders with an additional output in the regression set.
