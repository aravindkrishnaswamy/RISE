# r268 — where a production step's time goes

2026-10-29. Profiling by Claude, at the owner's request. No code change.

**Binary.** `bin/tests/FireSequenceTest`, SHA-256 `2ca55c10…`, the same executable as r264.

**Method.** A fresh tier-8 temporal capstone run was started from zero and warmed up for 30 steps. Instruments then attached for 12 s:
- once with Metal System Trace;
- once with Time Profiler, as a separate run.

Each 12 s window covers about four steps. Both runs were then stopped. Parsers are in [`tools/fire/r268/`](../tools/fire/r268/) and outputs in [`data/fire_r268/`](data/fire_r268/). The traces stay outside git, in `/Volumes/SSD/RISE/fire_production_calibration/r268_step_profile/` (184 MB and 9 MB).

**Caveat.** These are early steps (t ≈ 0.05 s), which take 2.9 s each. The r264 steady state was 5.45 s/step:
- 3.7 s inside the step (`monitored_diagnostics.csv` `wall_ms`, of which 3.2 s is `device_ms`);
- 1.76 s between steps.

The proportions below describe the step's structure. The absolute numbers grow as the flame develops.

## Breakdown of one step (about 2.9 s)

| Part | Time per step | Share |
|---|---:|---:|
| GPU compute | about 1.1 s | 38% |
| GPU fully idle while the main thread works (one block per step) | about 1.4 s | 48% |
| Short GPU idle gaps between about 120 command buffers (submit/wait/readback) | about 0.45 s | 15% |

- **The GPU is busy only 37% of the time.** Across the 12 s window it ran compute for 4.56 s of 12.24 s, in 1,410 encoders and 478 command buffers.
- **The idle block repeats every 2.9 s.** It lasts 1.39–1.42 s each time.

### Main thread (Time Profiler, 6.5 s of CPU in 12 s)

| Inclusive time | Function | What it is |
|---:|---|---|
| 2.84 s | `FireProductionCanonicalSourceAuthority::Build` | Rebuilds and identifies the source packets on the CPU every step |
| 1.60 s | `SourcePacketContentIdentity` | Content hashing of the source packets |
| 1.56 s | `FireProductionFrozenSourcePacketSeal::Matches` | Re-checks the seal against the beginning state |
| 1.09 s | `HashFluxValues` | Hashing |
| 0.98 s | `HashSourceDoubleValues` | Hashing |
| 0.59 + 0.27 s | `AcceptedManifoldPayloadDigest`, `AcceptedStatePayloadDigestFast` | Digests of the accepted state |
| 0.82 / 0.67 / 0.41 s | `SensibleEnthalpiesBySpeciesOrder`, `InvertAcceptedConservativeState…`, `AcceptedMethaneCellStateAdmissible` | Per-cell EOS inversion and admissibility, in double precision on the CPU |
| 1.32 s | `ResidentProjectedHeunMetalOwner::Run` | The actual GPU step: encoding, submission and readback |

The rows are inclusive and overlap.

- **Hashing, sealing and identity work** together cost about 3.3 s per 12 s, or about 0.8 s/step.
- **CPU-side EOS and admissibility re-checks** cost about 0.4 s/step.
- **This explains the single-threaded behaviour.** These checks run serially on the main thread while the GPU waits. The other threads total under 0.5 s each.

### GPU side

- **Encoder durations:**
  - 305 encoders of 5–20 ms make up 3.0 s of the 4.56 s;
  - 332 encoders of 1–5 ms make up 0.9 s.
- **For scale,** a stencil kernel over 505k cells should take well under 1 ms on the M4 Max.
- **The command buffers carry no labels,** so encoders cannot yet be attributed to kernels.
- **The likely shape:** a repeating group of about 8 command buffers, about 15 per step, each with a 60–75 ms buffer. That fits one buffer per Picard round or multigrid cycle, with host waits between them.

## Between steps (production only)

In r264, 27,000 s of the 83,748 s wall time fell outside the per-step `wall_ms`, which is 1.76 s/step. Candidates:
- per-step budget CSVs (30,770 files) with payload-v2 manifests;
- 550 checkpoint-history snapshots (22 GB);
- frame and checkpoint digests.

This was not traced. The 12 s windows did not include a checkpoint or frame write.

## Opportunities

None of these has been implemented. Each is a production-harness change that needs owner review.

| # | Change | Expected effect |
|---:|---|---|
| 1 | Run per-step identity hashing and seal re-checks only at checkpoints and frames, or as a sampled audit, instead of every step | About 0.8 s/step on the host |
| 2 | Move canonical-source and EOS admissibility checks to the GPU, or drop them where the GPU result is already certified | About 0.4–0.6 s/step on the host |
| 3 | Write budget CSVs and checkpoint history at a lower cadence | Up to 1.76 s/step |
| 4 | Label command buffers and encoders, then re-trace to attribute GPU time per kernel; batch the per-cycle waits into fewer command buffers | Attribution first; then the 1.1 s of GPU and 0.45 s of stalls |

Items 1–3 are host overhead. Together they plausibly cut r264's 5.45 s/step to about 2.5 s. The rest is GPU work (item 4), which is still about 5× the design target of 0.2 s/step.
