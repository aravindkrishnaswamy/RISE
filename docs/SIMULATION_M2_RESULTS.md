# M2 results (draft): the fire behind the solver contract

2026-11-23. Design: [SIMULATION_M2_DESIGN.md](SIMULATION_M2_DESIGN.md). This note covers track C (the fire
adapter); tracks A and B landed in d517a8755. Draft: the verification section is filled from the runs below.

## Baking a fire with rise-sim

The production methane pool fire is registered as `fire.methane` in the shipped `rise-sim`
(`bin/tools/rise_sim`, built by `make all`; `tools/rise_sim.cpp` calls `RegisterFireSolver`).

```sh
# parameters.json (every key optional; the defaults are today's capstone launch)
{"tier": 6, "frame_origin_s": 0.0625, "end_time_s": 0.13}

rise_sim bake fire.methane parameters.json /path/to/cache   # exit 0 sealed, 3 stopped, 1 failed
rise_sim status /path/to/cache                               # state, simulated time, frames, detail
rise_sim resume /path/to/cache                               # continues a stopped or killed bake
```

- Stop: SIGINT/SIGTERM or `--stop-after-frames N`. A signal takes effect at the next accepted step: the run
  parks there, the worker writes `rise-sim.checkpoint`, and `resume` continues the same window exactly as if it
  had not been interrupted. Periodic checkpoints (`--checkpoint-seconds S`, default 300) are taken at the first
  published frame after the cadence elapsed, so a killed bake resumes from its last such frame.
- The cache directory must not have group- or world-writable ancestors (the cache writer refuses them); use a
  private directory, not the SSD mount root.
- Output, in the cache directory:
  - the native cache (`manifest.cbor`, `*.frame`): output `volume`, codec `grid.v1`, channels `carbon` (g/m3),
    `temperature` (K), `reaction` (W/m3), `velocity` (m/s, vec3), `chem_CH`/`chem_C2`/`chem_CO2` (W/m3), one
    frame per 0.0625 s from `frame_origin_s`, fidelity `preview`;
  - `legacy/`: the capstone run directory exactly as `FireSequenceTest --fire-production-temporal-capstone`
    writes it (`checkpoints/` and `frames/` VDB frames, the sealed `sequence_manifest.rise-fire.cbor`,
    `frame_times.csv`, `budgets/`, `monitored_diagnostics.csv`, `production_capstone_summary.txt`,
    `window_launch.v1`, payload-v2 seals), plus `production_final.checkpoint` also for a bounded window.
- Rendering: `fire_medium { sequence_manifest /path/to/cache/manifest.cbor ... }` plays the native cache
  directly (also while it is still baking); `legacy/sequence_manifest.rise-fire.cbor` keeps working as before.

## Parameters

Canonical CBOR map (the worker also accepts a JSON object). Defaults equal today's defaults. Option values are
the exact text the library parses from the legacy variable; a JSON number becomes its shortest round-trip
decimal text, `true` becomes `1`. `""` / `false` / `0` mean "unset" (the library default).

| Parameter | Default | Legacy environment variable | Notes |
|---|---|---|---|
| `tier` | 8 | | 6, 8 or 10 (the capstone tiers) |
| `frame_cadence_s` | 0.0625 | | only 0.0625 |
| `frame_origin_s` | statistics start (5 flow-through times) | `RISE_R273_FRAME_ORIGIN_S` (A/B tools only) | frame clock origin |
| `end_time_s` | window target (statistics start + 40 puffing periods) | `RISE_R271_WINDOW_S` (A/B tools only) | absolute end; shorter = bounded window |
| `pool_diameter_m` | 0.30 | | only 0.30 |
| `heat_release_rate_kw` | 33 | | only 33 |
| `mode` | `certified` | `RISE_FIRE_FAST_MODE` | `fast` = r275 uncertified fast mode |
| `fast_fixed_vcycles` | false | `RISE_FIRE_FAST_FIXED_VCYCLES` | fast mode only |
| `source_step_cap_s` | unset (r175 cap) | `RISE_FIRE_SOURCE_STEP_CAP_S` | > 0, `inf` allowed |
| `floor_emissivity` | unset | `RISE_FIRE_FLOOR_EMISSIVITY` | (0, 1] |
| `inert_plume` | false | `RISE_FIRE_INERT_PLUME` | |
| `inert_seed` | unset | `RISE_FIRE_INERT_SEED` | (0, 1) |
| `advective_cfl` | `0.5` | `RISE_FIRE_ADVECTIVE_CFL` | (0, 1] |
| `fds_mixing_time` | false | `RISE_FIRE_FDS_MIXING_TIME` | |
| `methane_soot_yield` | unset (fuel record) | `RISE_FIRE_METHANE_SOOT_YIELD` | [0, 0.5] |
| `soot_radiative_fraction_cap` | unset | `RISE_FIRE_SOOT_RADIATIVE_FRACTION_CAP` | (0, 1] |
| `soot_sheet_oxidation` | false | `RISE_FIRE_SOOT_SHEET_OXIDATION` | |
| `soot_self_absorption` | 0 | `RISE_FIRE_SOOT_SELF_ABSORPTION` | 0, 1, 2 |
| `scalar_vreman_cv` | unset (transport record) | `RISE_FIRE_SCALAR_VREMAN_CV` | >= 0 |
| `fct_alpha` | unset | `RISE_FIRE_FCT_ALPHA_ONE` | `0` or `1` |
| `scalar_slope` | unset (MC) | `RISE_FIRE_SCALAR_SLOPE` | `central`, `superbee` |
| `wall_slope` | unset | `RISE_FIRE_WALL_SLOPE` | `linear` |
| `limited_momentum_carrier` | false | `RISE_FIRE_LIMITED_MOMENTUM_CARRIER` | |
| `compact_viscous_stencil` | false | `RISE_FIRE_COMPACT_VISCOUS_STENCIL` | |
| `vreman_zone` | unset | `RISE_FIRE_VREMAN_ZONE` | `below|above:<k>:<s>` or `ramp:<k0>:<k1>:<s>`; needs the compact stencil |
| `near_wall_wale` | false | `RISE_FIRE_NEAR_WALL_WALE` | needs the compact stencil |
| `hyper_vreman` | unset | `RISE_FIRE_HYPER_VREMAN` | (0, 1]; needs the compact stencil |

Every one of these enters the cache key. `RISE_FIRE_WORKERS` does not (the state is independent of the worker
count); it is still read from the environment and recorded in the checkpoint's worker history. The legacy
variables are read once, when the solver is created, as parameter defaults; explicit parameters win, and
`rise-sim` prints a note for every value that came from the environment.

## How it works

- **Advance and rendezvous.** `FireSolver` runs the capstone's driver (`RunFireMethaneDriver` with the
  capstone's launch, `FireRun::PlanProductionTemporalLaunch`) on its own thread with a 256 MiB stack. The driver
  calls a new per-step rendezvous (`FireRunPersistenceOptions::acceptedStepRendezvous`) after each accepted step,
  after the step's frame; the solver parks the driver there at every published frame, converts the frame and
  returns from `Advance`; the next `Advance` releases it. `RequestStop` parks it at the next accepted step that is
  not the last. After the last frame the driver runs to the end of the window and the solver finishes the legacy
  directory with the capstone's own finish (`FireRun::FinishProductionTemporalRun`).
- **Native frames.** Each frame is the just-written legacy VDB converted by the legacy importer's code path
  (`FireSequenceManifest::LoadVDBFileGrid`, the body of `ReadVDBFrame`), then `EncodeGrid`: the payload is
  byte-identical to what `ImportFireSequenceCache` produces from the sealed legacy sequence. The receipt names the
  legacy frame and its SHA-256. Encoding from the in-memory fields was not used: the VDB writer's tree layout
  decides which voxels are stored, and reproducing it exactly outside the writer would be a second implementation.
- **fire_medium.** The cache's physical record is the capstone's sequence envelope for this case and frame clock
  without the frame list (`FireSequenceManifest::LoadFrameTemplate`); `FireSequenceManifest::AttachCache`
  recognises a `fire.methane` cache, checks its description against that record and takes each frame's
  whole-file digest from its receipt. Such a cache is `preview_only` (no original sealed manifest). Its time map
  origin is the nominal frame origin; the legacy manifest records the actual first frame time.
- **Checkpoint bytes.** `Checkpoint` serializes the parked driver's accepted state through
  `SerializeMethaneRunCheckpoint`, the in-memory twin of `SaveMethaneRunCheckpoint` (the same bytes as the file;
  `CheckpointWriter` gained an in-memory file mode), and appends an adapter trailer: configuration digest, next
  frame, time, accepted steps, and the run values the fire checkpoint format does not carry (monitored manifold
  histories, device/wall histories, retry counts, external fuel mass, projection tolerances). The fire checkpoint
  format is unchanged.
- **General resume.** New in the driver, behind `FireRunPersistenceOptions::generalResume` (used only by the
  adapter; refused together with every probe, diagnostic and study option): the checkpoint is loaded through the
  existing validated loader (same build and case required), the frame clock continues as `origin + index *
  cadence` (the loop's own formula), the run-lineage fields stay as in an uninterrupted run (`resumedFrom*`
  unchanged, the worker count appended only when it changes), the restored run values are reapplied, and the
  onset/host-memory logs are appended instead of restarted (onset thresholds whose evidence exists stay
  captured). The existing resume and isolated-probe paths and all their refusals are untouched. Other neutral
  options: `windowEndTimeS` (absolute window end) and `finalCheckpointAtWindowEnd`.
- **Worker (generic).** `SolverDescription::sideOutput` names a solver-owned directory beside the cache (here
  `legacy`), emptied when a bake starts a new epoch and kept on resume, and passed to
  `ISimulationSolver::SetSideOutputDirectory`; `SolverDescription::notes` are printed; the status record carries the
  solver's progress detail and is refreshed during a long `Advance`; the checkpoint header is inserted in front of
  the solver state in place, and read back by erasing it in place.

## Transitional: environment application

The fire library still reads its options from the process environment, mostly once per process into static
caches. Until those reads become parameters below the adapter, `Configure` writes the effective options into the
environment (`setenv`/`unsetenv` of the 21 variables and `RISE_FIRE_FAST_MODE`), once per process and before the
first fire library call, and `Start`/`Restore` refuse if any of them changed since. A second, different fire
configuration in the same process is refused, as is a second live fire solver.

getenv sites that remain (all untouched in this track):

- result-affecting, now driven by the parameters above: `FireRunDriver.cpp` (`RISE_FIRE_SOURCE_STEP_CAP_S`),
  `FireRunFrames.cpp` (`RISE_FIRE_FLOOR_EMISSIVITY`), `FireCase.cpp` (`INERT_PLUME`, `INERT_SEED`,
  `ADVECTIVE_CFL`), `FireFastMode.h` (`FAST_MODE`, `FAST_FIXED_VCYCLES`), `FireProductionSourceKernel.h`
  (`FDS_MIXING_TIME`, `METHANE_SOOT_YIELD`, `SOOT_RADIATIVE_FRACTION_CAP`, `SOOT_SHEET_OXIDATION`,
  `SOOT_SELF_ABSORPTION`), `FireProductionAdvectionMac.mm` (`SCALAR_VREMAN_CV`, `FCT_ALPHA_ONE`, `SCALAR_SLOPE`,
  `WALL_SLOPE`, `LIMITED_MOMENTUM_CARRIER`), `FireProductionForceMac.mm` (`VREMAN_ZONE`,
  `COMPACT_VISCOUS_STENCIL`, `NEAR_WALL_WALE`, `HYPER_VREMAN`);
- not result-affecting, still environment only: `RISE_FIRE_WORKERS`, `RISE_FIRE_PARTIAL_MANIFEST`,
  `RISE_FIRE_CAPSTONE_OUTPUT`, `RISE_FIRE_FAST_CONSERVATION_EVERY`/`_LOG_EVERY` (fast-mode checks),
  `RISE_FIRE_PICARD_TRACE`, `RISE_FIRE_NONCANONICAL_DETAIL`, `RISE_R271_PROJECTION_TRACE`,
  `RISE_FIRE_OWNER_PROFILE`, `RISE_FIRE_PRODUCER_KERNEL_PROFILE`, `RISE_FIRE_EOS_ITERATION_PROFILE`;
- diagnostics, probes and test REDs that the production launch refuses (`ProductionTemporalEnvironmentAccepted`):
  `RISE_FIRE_TEMPORAL_RESUME`, `RISE_FIRE_TEMPORAL_TARGET_S`, `RISE_FIRE_REFUSAL_CLAUSES`, `RISE_FIRE_PROFILE`,
  `RISE_FIRE_MOMENTUM_AUDIT_PATH`, `RISE_FIRE_*_IDENTITY_RED`, `RISE_FIRE_PRODUCTION_RESTORATION_*`,
  `RISE_FIRE_RESTORATION_PLATEAU_PROBE`, `RISE_FIRE_PROJECTION_TEST_FAILURE`, `RISE_FIRE_FORCE_TEST_FAILURE`,
  `RISE_FIRE_PRODUCTION_STEP_FAILURE`, `RISE_FIRE_TIMESTEP_VELOCITY_*`, `RISE_FIRE_MANIFOLD_*`,
  `RISE_FIRE_ADVECTIVE_ANOMALY_*`, `RISE_FIRE_GOLDEN_LONG_SHADOW`, `RISE_FIRE_HOST_RESIDUAL_PROBE`,
  `RISE_FIRE_PHYSICAL_PROJECTION_VALIDATION_PROBE`, `RISE_FIRE_ONSET_*`, `RISE_FIRE_EOS_REFUSAL_INPUTS`,
  `RISE_FIRE_ORACLE_MOMENTUM_AUDIT_PATH` (oracle composition only);
- renderer side: `RISE_FIRE_SEQUENCE_PREFLIGHT` (`Job.cpp`), `RISE_SIMULATION_CACHE_DIR` (`FireSequenceCache.cpp`).

## Limits

- One fire solver per process, one fire configuration per process (see above).
- Only the production capstone's case: pool 0.30 m, 33 kW, tiers 6/8/10, 0.0625 s frames; no r244/r245/r246
  domain studies through the solver.
- No wall-clock checkpoints inside the driver: the legacy directory has no `production_run.checkpoint` or
  `checkpoint_history/` (the worker's `rise-sim.checkpoint` replaces them).
- A bounded window whose end lies less than one accepted step before a frame time can publish one frame past the
  declared count; the bake then fails with a message saying so.
- Fast mode is declared nondeterministic: on resume after a kill the worker keeps committed native frames, while
  the legacy frames after the restored checkpoint are written again.
- A kill after the legacy directory was sealed but before the cache was sealed cannot be resumed (rebake with
  `--force`).
- The in-process host gives no side-output directory, so the fire solver runs only through `rise-sim` for now.
- `Capabilities::fidelityModes` lists `certified`/`fast` as requested for track C, while its header comment and the
  reference solvers use it for the labels a solver can earn (`preview`/`predictive`); to be settled with the
  contract review in M3.

## Verification (2026-11-23, M1 Max, Metal)

`tests/FireSolverTest` (fast tier 172 s, exclusive; `--extended` / `RISE_TEST_EXTENDED=1` adds about 160 s):

| Check | Result |
|---|---|
| Every result-affecting option changes the cache key; `RISE_FIRE_WORKERS=3` does not; a legacy variable becomes the effective parameter (and is reported); explicit parameters override it; a JSON number maps to the same text; malformed values, unknown keys and missing prerequisites are refused; a changed environment at Start is refused; a second live solver is refused | pass (24 configurations, 12 s) |
| `rise-sim bake` of tier 6, origin 0.0625 s, end 0.13 s (80 steps, 2 frames): sealed `preview` cache; each native frame byte-identical to `ImportFireSequenceCache` of the sealed legacy sequence; fire_medium's reader plays the cache with every voxel bit-equal to the legacy frame and the same whole-file digests; `SerializeMethaneRunCheckpoint` equals the checkpoint file | pass |
| `--stop-after-frames 1`, then SIGINT 15 accepted steps into the resume (step 53, between frames), then resume: sealed native cache, legacy frames/times/manifest and final fire checkpoint byte-identical to the uninterrupted bake (the checkpoint even raw-equal) | pass |
| Extended: legacy frames, frame times and sealed manifest byte-identical to `RunProductionTemporalCapstone` for the same window | pass |
| Extended: fast mode bakes, stops and resumes; in this run the resumed fast cache was byte-identical to the uninterrupted fast bake (one run; fast mode stays declared nondeterministic) | pass |

Also observed on the same window (manual run): the legacy summary, monitored diagnostics, onset trajectory and
launch receipt of the twice-interrupted bake match the capstone's apart from wall-time columns, so statistics
accumulate exactly through a resume. Wall time: rise-sim bake 77.2 s against the capstone's 76.3-76.5 s on the
same window (about 1 % for the rendezvous and the frame conversion).

Default path unchanged: the r273 A/B resume (tier 8, 25 steps, snapshots every 5) is byte-identical to
`speed_v4.snap` after normalizing the producer id and its checksums, and the from-zero tier-6 capstone (track B's
check) is identical to `m2b_zero_head`, before and after this track.
