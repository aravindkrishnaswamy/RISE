# r274: speed pass 1, identical solver output and matching images

2026-11-22. Starting point: a 25 s tier-8 movie took about 36 h to simulate and 10 h to render.

## Result (quiet machine, tier 8, resume of step 13,769, yield 0.3, cap 0.45, deposit mode)

| | Before | After | Gain |
|---|---:|---:|---:|
| Solver, s per step (11 steps) | 7.1 | 3.8 | 1.9x |
| Renderer, s per frame (256x384, 16 samples, frames at 12 s) | 146 | 14.1 | 10x |

Projected 25 s movie: about 16 h of simulation plus under 1 h of rendering, against about 46 h.

## Solver (output bit-identical)

- **EOS bisection table.** `evaluate_resident_eos_candidate` spent about 4 s of GPU time per step on a 24–25 level
  temperature bisection whose first 10 levels came from a table. The table now covers 24 levels
  (`build_eos_deep_prefix_table`), built once per process with the same arithmetic as the inline path. Costs about
  2.2 GB of GPU memory, counted in the owner working set.
- **Source build.** The per-cell inversion/admissibility and expansion-admissibility loops in
  `FireProductionCanonicalSourceAuthority::Build` run on the worker pool; packet workers take small chunks instead
  of fixed slices.
- **Seal hashing.** The packet content hash, recomputed five times per step, is cached and reused only when every
  input byte matches.
- **Check.** 25 steps with a checkpoint every 5: all five checkpoints byte-identical to the old build apart from the
  producer build id and the two checksums covering it. Failure paths were reviewed, not exercised.
- **Not the cause.** GPU command-buffer round trips cost about 0.2 s per step in total; the waits are real GPU work.

## Renderer (`MultichannelHeterogeneousMedium`)

- Per-frame flat padded grids with one 8-corner fetch for all channels (double precision, same operation order as
  `VolumeAccessor_TRI`); wavelength constants cached per thread.
- Deterministic optical depth: skips empty cells, caches cumulative integrals per ray and shares them across
  wavelengths, 2-point Gauss–Legendre where the hot fraction is constant.
- Emission-weight rebuild is parallel (5.4 s to 0.13 s per call).
- **Check.** Three frames at 12 s, old against new: mean display values agree within 0.2%; denoised frames differ by
  3.3/255 RMS, the level of two independent renders. New test `TestFlatGridsMatchAccessorReference`.

## Remaining cost

- Solver, about 3.8 s per step: owner GPU step about 1.5 s (`evaluate_resident_target_terms` 0.55 s, EOS candidate
  0.45 s), source build 0.5 s, the rest host digests and bookkeeping.
- Renderer: optical-depth walk 44%, ratio tracking 30% (one wavelength-independent majorant), chemistry emission 16%.
  `FireSequenceManifest::PreflightAllFrames` costs about 20 s once per movie.
- Owner decisions not taken: more driver workers (8 of 16 cores used), per-step digests deferred to checkpoints,
  per-wavelength majorant (the current one is marked locked).

## r275 (2026-11-22): opt-in fast mode, time step at the flow limit, worker count

All three are opt-ins; with nothing set the output is byte-identical to r274 (13 steps, checkpoint per step).

| Mode (quiet machine, tier 8, resume of step 13,769) | s per step | Wall s per simulated s | 25 s movie |
|---|---:|---:|---:|
| Certified (default) | 3.83 | 2324 | 16 h |
| Certified, `RISE_FIRE_SOURCE_STEP_CAP_S=inf` | 3.97 | 1686 | 12 h |
| `RISE_FIRE_FAST_MODE=1` | 1.85 | 1121 | 7.8 h |
| Fast mode + step cap lifted | 1.93 | 818 | 5.7 h |
| Fast mode + step cap lifted + `RISE_FIRE_WORKERS=14` | 1.73 | 736 | 5.1 h |

Short samples (16–23 steps); the worker-count row is within noise of the one above in certified mode.

- **`RISE_FIRE_FAST_MODE=1`** (`FireFastMode.h`). Same model and discretisation. Plain binary32 EOS inversion and
  divergence-target terms instead of interval-certified extended precision; no per-step seals, content hashes or
  payload digests; pressure solve stops once the unchanged validation band is met (`RISE_FIRE_FAST_FIXED_VCYCLES=1`
  keeps the 17 cycles). Physical refusals, the checkpoint digest and file formats are kept. Safety net: the exact
  budget ledger checked every step, tolerance 1e-5 (`RISE_FIRE_FAST_CONSERVATION_EVERY`).
  - Against certified from the same checkpoint: scalars within 1.7e-5 of field maximum after one step, 3.4e-5 after
    20; velocity 3e-4 (mostly the pressure-solve stop; 6e-6 with fixed cycles). Ledger closure over 304 steps: mass
    5e-10, elements and energy below 2e-9. Means over 0.5 s (heat release, peak and centreline temperature, soot
    flux) agree to four figures.
  - Not guaranteed: correct rounding, error bounds, bit reproducibility, per-step provenance.
- **`RISE_FIRE_SOURCE_STEP_CAP_S=<s|inf>`**. Replaces the 1.646 ms source-step cap; the selector's CFL, buoyancy,
  diffusion and growth limits stay. The cap was the CFL step of one tier-10 state at which r175 tested the source
  arithmetic; the test code is gone, so this is uncertified. 0.5 s at mean dt 2.39 ms: 210 steps against 304, no
  retries, heat release +0.7%, peak temperature −6 K, soot flux about +1%, all inside frame-to-frame spread;
  time error against a half-step reference falls at about second order.
- **`RISE_FIRE_WORKERS=<n>`**. Output identical at 8–16 workers; no measurable gain in certified mode.
- A Picard iteration cap was not added: stages 0 and 1 hold a residual of 0.4–0.6 until the open-boundary cycle is
  detected, so a cap would accept unconverged stages.
- Not verified: runs longer than 0.5 s in either opt-in; fast-mode refusal paths. A full 25 s fast run
  (`tools/fire/r275/run_tier8_fast.sh`) is the check against `r273_poor_burn/tier8_y300_sa`.
- Remaining in a fast step (about 1.9 s): CPU source build 0.75 s (packets, floor irradiance), GPU stage build 0.42 s,
  pressure solve 0.28 s, host bookkeeping about 0.3 s.

## r275 full-window validation (2026-11-22)

`r275_speed/tier8_y300_sa_fast2` (`tools/fire/r275/run_tier8_fast.sh`): the r273 sooty showcase from zero in fast
mode with a 2.5 ms step cap, rendered in follow mode while it ran. Reference: the certified run
`r273_poor_burn/tier8_y300_sa` (same settings, 1.646 ms cap).

| | Certified | Fast, 2.5 ms cap |
|---|---:|---:|
| Wall time | 36.6 h | 5.02 h |
| Accepted steps (rejected) | 15,255 (0) | 10,704 (0) |
| Integrated heat release, J | 384,976 | 385,001 |
| Integrated radiative fraction | 0.3658 | 0.3657 |
| Soot flux at 1.0 / 1.6 / 2.2 m, mg/s (last 10 s) | 133.1 / 134.5 / 135.3 | 133.9 / 134.8 / 134.6 |
| Share of soot above 1300 K at 0.08 m | 0.158 | 0.157 |
| Centreline temperature, layers 0 / 8 / 16 / 32, K | 1182 / 1128 / 880 / 543 | 1180 / 1118 / 862 / 540 |
| Centreline soot volume fraction, layers 0 / 16 / 32, ppm | 1.02 / 0.88 / 0.60 | 1.04 / 0.87 / 0.60 |
| Puffing frequency, Hz | 3.34 | 2.52 |

- 7.3 times faster end to end, with a renderer on 8 threads running beside it the whole time.
- Means agree within about 2% everywhere below 2.8 m; the top two layers differ by up to 5% (outflow boundary).
- **Puffing frequency differs** (2.52 against 3.34 Hz; the capstone's reference is about 2.74 Hz, so fast is 8% low
  and certified 22% high). Which of fast mode and the larger step moves it has not been separated.
- A cap of `inf` fails a cold start (11.6 ms steps, stage validation refuses at step 10); 2.5 ms does not.
- Follow mode: frames rendered at about 24 s each as they appeared; the 235-frame movie was complete 17 s after the
  solver's last frame. Movie: `/Volumes/SSD/RISE/movies/r275_sooty_y300_fast_follow_256x384.mov`.
- Data: `docs/data/fire_r275/tier8_y300_sa_fast2_{budget,centreline}_last10s.txt`.
