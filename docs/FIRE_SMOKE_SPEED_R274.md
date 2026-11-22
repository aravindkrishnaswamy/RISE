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
