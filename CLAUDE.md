# RISE Claude Companion

This file intentionally stays thin so it does not drift from the shared docs. Start with [README.md](README.md) for the repo map, [AGENTS.md](AGENTS.md) for the shared agent guide, and [docs/README.md](docs/README.md) for deep dives.

## Quickstart

- Linux/macOS main build: `make -C build/make/rise -j8 all`
- Linux/macOS tests: `make -C build/make/rise tests` then `./run_all_tests.sh`
- Windows tests: `.\run_all_tests.ps1`
- Windows tests (Debug): `.\run_all_tests.ps1 -Config Debug`
- Sample render:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
```

### One-time setup: OIDN with GPU support (extlib submodule)

OIDN is shipped as a git submodule at `extlib/oidn/source` so RISE
links against an in-tree install with GPU device backends enabled
(Metal on macOS; CUDA / HIP / SYCL toggles on Windows). System OIDN
installs from Homebrew, vcpkg, etc., are typically built with CPU
device only — RISE's denoise then silently runs on CPU even on a
Metal-capable Mac. Full background in [docs/OIDN.md](docs/OIDN.md)
(OIDN-P0-3 install gotcha).

Run once after cloning, and again whenever the OIDN submodule
commit is bumped:

```sh
# macOS / Linux
git submodule update --init extlib/oidn/source
extlib/oidn/build.sh
```

```powershell
# Windows (PowerShell, in repo root)
git submodule update --init extlib/oidn/source
pwsh -File extlib\oidn\build.ps1     # add -EnableCuda / -EnableHip / -EnableSycl as needed
```

Both scripts populate `extlib/oidn/install/`; RISE's Makefile and
VS2022 project files prefer that path over any system OIDN. Verify
the selected device on your first render — look for:
`OIDN: creating Metal device (one-time per rasterizer)` in the log.

Skipping this step is fine — RISE falls back to the system OIDN
(typically Homebrew's CPU-only build) and denoise still works,
just without GPU acceleration.

## Source-file add/remove — touch ALL five build projects

When you add **or remove** any `.cpp` / `.h` under `src/Library/`, update every one of these; none auto-discovers files:

1. `build/make/rise/Filelist` (Unix/Linux, canonical)
2. `build/cmake/rise-android/rise_sources.cmake` (Android NDK, consumed by `build/cmake/rise-android/CMakeLists.txt` which the Gradle build invokes)
3. `build/VS2022/Library/Library.vcxproj` (Windows, tracked in git) — `<ClCompile>` for `.cpp`, `<ClInclude>` for `.h`.
4. `build/VS2022/Library/Library.vcxproj.filters` — same entries with `<Filter>` tags so the Solution Explorer tree matches the build.
5. `build/XCode/rise/rise.xcodeproj/project.pbxproj` — Xcode references appear in four sections (`PBXBuildFile`, `PBXFileReference`, the containing `PBXGroup`, and per-target `Sources` / `Headers` build phases) and usually in two targets (library + GUI); delete every match when removing a file.

Full rules, exclusions, and related update surfaces (API, parser chunks, tests, scenes) are in [AGENTS.md](AGENTS.md) → "Change Checklist".

## Compiler warnings are bugs

Don't introduce warnings.  Don't leave them.  Don't suppress them with
pragmas or `-Wno-*` flags — fix the root cause.  Both the `make` build
and the Xcode `RISE-GUI` build must come up warning-free on a *clean*
rebuild before you call a task done — incremental builds hide warnings
on files that didn't recompile.  Full rationale, the principled fix per
warning kind, and the verification commands are in [AGENTS.md](AGENTS.md) →
"Compiler Warnings Are Bugs".

## High-Value Facts

- **SMS seeding mode** (`sms_seeding` parameter, [docs/SMS_UNIFORM_SEEDING_PLAN.md](docs/SMS_UNIFORM_SEEDING_PLAN.md), measurements + regime analysis in [docs/SMS_UNIFORM_SEEDING_RESULTS.md](docs/SMS_UNIFORM_SEEDING_RESULTS.md), background in [docs/SMS_FUTURE_DIRECTIONS.md](docs/SMS_FUTURE_DIRECTIONS.md)): two strategies — `"snell"` (default; deterministic Snell-traced seed from shading point toward sampled light, including a multi-level Fresnel-branching variant that splits at sub-critical dielectric vertices and uniform-area-resamples the reflection branch + a pure-mirror caster supplement that uniform-area samples each pure-mirror caster `M = multi_trials` times to recover diacaustic-style chains) and `"uniform"` (Mitsuba-faithful uniform-area sample on cached caustic-caster shapes per `manifold_ss.cpp` / `manifold_ms.cpp`).  **The two are regime-complementary, not one-better:**
  - **Use `"snell"` (default) for displaced/bumpy specular casters and refraction-dominated caustics.**  The deterministic Snell-trace seed is by-construction in the dominant refraction-caustic basin; Newton converges from there even when per-bump basins are too small for uniform-area sampling to reliably hit.  Measured: displaced Veach egg snell ratio 0.37 vs uniform 0.008 — uniform-area collapses on heavy displacement.
  - **Use `"uniform"` for smooth analytic primitives with reflection/glint caustics** (mirror chains, diacaustics, lensed reflections off smooth dielectrics).  Uniform-area covers the full caster surface so reflection-root basins on smooth geometry get hit; Snell-trace toward the light can't seed those chains.
  - On scenes that match neither regime cleanly, both modes give similar ratios within MC noise (smooth Veach 0.93 vs 0.94, k1_botonly tied).  Default `"snell"` is the safer choice when in doubt.

  Photon-aided extension (Weisstein 2024 PMS) layers on top of biased uniform mode and the snell-mode multi-trial loop alike.  Required `seedingMode = Uniform` for principled geometric Bernoulli `1/p` (Zeltner 2020 §4.3) when `sms_biased FALSE`.

- **SMS two-stage solver scope** (`sms_two_stage` parameter, [docs/SMS_TWO_STAGE_SOLVER.md](docs/SMS_TWO_STAGE_SOLVER.md)): the Zeltner 2020 §5 two-stage solver helps on **smooth analytic primitives with normal-perturbing maps** (verified: bumpmap on `ellipsoid_geometry` reaches ΣL_sms/ΣL_supp = 1.15 at gentle bumps).  It **regresses** on **heavily-displaced meshes** (`displaced_geometry` with `disp_scale` > ~10 % of curvature radius): Stage 1 converges to the smooth-surface caustic, whose corresponding mesh position is up to `disp_scale` units away from the bumpy caustic — Stage 2's seed is *worse* than the original Snell-traced on-mesh seed.  This matches Mitsuba's reference scope: their [`Figure_9_Twostage`](https://github.com/tizian/specular-manifold-sampling/tree/master/results/Figure_9_Twostage) scenes use only smooth primitives + `normalmap` BSDFs; their displaced-mesh comparison ([`Figure_16`](https://github.com/tizian/specular-manifold-sampling/tree/master/results/Figure_16_Displacement)) never engages two-stage.  Mitsuba's smoothing is plumbed via `bsdf->lean()` (LEAN moments, Olano-Baker 2010) — a no-op for non-normal-mapped BSDFs.  **Default is OFF; turn ON only for the smooth-primitive + normal-map regime.**  For heavily displaced caustics, prefer VCM.

- **SMS photons help only when seeds miss basins (2026-05, displaced-egg empirical)**: re-ran the v7 displaced-painter sweep with `sms_photon_count 100000` enabled in BOTH snell and uniform modes (commit a3790c4 + 9bd7c5b photon-aware target-bounces filter).  Ran on the 66-cell painter × disp grid with `sms_target_bounces 2`.  Aggregate ratio change vs no-photons baseline:  **snell mean ratio 0.893 → 0.910 (+1.9 %, statistical noise); uniform 0.351 → 0.332 (−5.5 %).**  Per-scene `sms_valid` count is ESSENTIALLY UNCHANGED (snell+t2: 66,402 valid contributions without photons vs 66,356 with — i.e., photons converge to basins that snell-trace already covered, the basin-dedupe drops them, and net unique caustic coverage is the same).  Wall time: ~6× slower with photons.  Take-away: **photons help when the seed-builder MISSES BASINS** (e.g., diacaustic-class caustics where the deterministic Snell-trace can't follow reflection geometry, or scenes with multi-mirror chains).  On scenes where snell-trace already finds the natural caustic basin (the displaced-Veach-egg with interior dielectric shell, k=2 single basin), photons add cost without adding coverage.  Per-scene rule: only enable `sms_photon_count > 0` when there's evidence the snell-trace systematically misses caustic regions.  The default 0 stays the right answer for typical refraction-dominated scenes.  Note: this is an EMPIRICAL claim about photon utility on a single scene class; multi-basin / mirror-chain scenes (diacaustic, sms_slab_close_sms) may show different photon ROI — see `docs/SMS_PHOTON_REGIME.md` for the broader characterisation.

- **SMS `sms_target_bounces` (2026-05, Mitsuba `m_config.bounces` analogue)**: per-rasterizer config wiring in BOTH snell and uniform modes that REQUIRES the seed chain to have exactly K specular vertices.  When `sms_target_bounces > 0`: (i) `SnellContinueChain` breaks at `chain.size() >= K`; (ii) `BuildSeedChainBranching` emits each branch as terminal at K; (iii) `BuildSeedChain` post-checks final length and rejects if `!= K`.  Default 0 = "no target": snell uses the emitter-projection cap (which discovers natural K), uniform uses `maxChainDepth` only.  **Set to the natural caustic K for the scene (typically 2 for a glass shell or interior-light scene).**  v7 sweep result on the 66-cell displaced-painter grid (PT, 4 spp): snell+T2 mean ok rate **94.4 %**, mean ratio ΣLsms/ΣLsup **0.893**; uniform+T2 mean ok **43.4 %**, ratio **0.351**.  Pre-T2 baseline (post seed-overtrace fix only) was ok ~67 %, ratio ~0.85 for snell.  Snell+T2 is +27 pp ok / +0.04 ratio over pre-T2 snell, cleanly beats uniform on heavy-displaced refractive caustics; uniform+T2 is competitive (or wins) on smooth analytical caustics (`sms_k2_glasssphere`: uniform 93 % ok / ratio 5.81 vs snell 86 % ok / ratio 6.60).  Recommended in BOTH snell and uniform mode for any scene whose natural caustic K is known — the `sms_seeding` regime guidance still holds: snell for displaced refractive, uniform for smooth analytical.

- **SMS uniform-mode chain-length cap (2026-05)** ([docs/SMS_SEEDING_OVERTRACE_FIX.md](docs/SMS_SEEDING_OVERTRACE_FIX.md) §"Uniform-mode follow-up"): the seed-overtrace fix below introduced an emitter-projection cap inside `BuildSeedChain` / `BuildSeedChainBranching` / `SnellContinueChain` that's correct in `sms_seeding "snell"` (passes the LIGHT as `end`) but WRONG in `sms_seeding "uniform"` (passes the uniform-area sampled point on the caster as `end`).  Cap fired at `|sp − pos| × 1.05`, clipping the seed chain to a single specular vertex when the natural caustic is k=2.  Combined with RISE not enforcing Mitsuba's `m_config.bounces` target chain length (we have `sms_max_chain_depth` as a *maximum*, default 10), uniform mode produced wildly variable chain lengths (k=1, 2, 3, 4) and a 36–53 % phys-fail rate vs snell's 3–17 %.  **Two-part fix**: (a) added `applyEmitterStop` parameter to the seed builders, default `true` for back-compat, with all six uniform-mode call sites passing `false`; (b) **scene-authoring rule** — for uniform mode, set `sms_max_chain_depth` to the natural caustic K (typically 2 for a glass shell or interior-light scene), not the default 10.  Superseded by the `sms_target_bounces` entry above which is the cleaner config — keep `sms_target_bounces` set rather than hijacking `sms_max_chain_depth`.

- **SMS seed-overtrace fix (2026-05)** ([docs/SMS_SEEDING_OVERTRACE_FIX.md](docs/SMS_SEEDING_OVERTRACE_FIX.md)): `BuildSeedChain` and `BuildSeedChainBranching` were collecting specular hits **past the emitter** when the emitter was inside a closed dielectric shell (e.g. interior light in the Veach-egg cavity).  The natural caustic chain is `k=2` (wall → outer-egg → inner-cavity → light), but the trace continued past the light through inner-front and outer-front, producing a `k=4` chain whose v2/v3 were on the opposite side of the emitter.  Newton converged to algebraically valid roots where wi/wo were both on the same side of the surface — geometric reflection, not refraction — and `ValidateChainPhysics` correctly rejected them.  This was the dominant `physicsFail` source on heavy-displaced scenes and the misdiagnosis behind multiple prior "structural multi-modality" conclusions.  **Fix**: stop adding seed vertices once a candidate hit's projection along the original `(start → end)` direction exceeds `totalDist × 1.05` (the 5% margin handles refractive bending).  ~20 lines, two sites in `ManifoldSolver.cpp`.  **Result on the 66-cell displaced-painter sweep**: Δok% mean +21.6 pp (range +10 to +35), Δphys% mean -17.3 pp (phys-fails now ~0% at low disp, ~3% at disp=40), ratio ΣLsms/ΣLsup mean +0.225.  No regression on the canonical SMS scenes (`sms_k1_refract`, `sms_k2_glassblock`, `sms_k2_glasssphere`, `sms_k1_botonly`).  **What it does NOT change**: residual ~30% Newton-fail rate (still plateau-stalled at ‖C‖ ∈ [0.1, 1) — the documented basin-of-attraction limit of plain Newton).  Rewrites several prior conclusions: the "spurious basin is topologically separated" claim from the F+E+G investigation was wrong (the seed was driving Newton there); photon-seeding's null result (G) was contaminated by the same seed-builder bug and warrants re-test on the fixed baseline.  Diagnostic instrumentation (sign-product, seed-validity, post-Newton ‖C‖, chain-length, cascade-depth, surface-angle, perturbed-restart counters) is gated under `SMS_SOLVE_DIAG` (default 0); flip to 1 for future SMS regressions.

- **SMS Levenberg-Marquardt + scene-authoring rule for displaced caustics** ([docs/SMS_LEVENBERG_MARQUARDT.md](docs/SMS_LEVENBERG_MARQUARDT.md)): NOTE — the "root cause = constraint multi-modality" diagnosis below is **superseded** by the seed-overtrace fix above; the dramatic SMS power drop turned out to be a seeding bug, not a deep solver / constraint-landscape problem.  LM's measured benefit (~5 pp on Newton-fails) is still real but on a much smaller residual gap than reported here.  `ManifoldSolver::NewtonSolve` damps the Jacobian's diagonal by `λ × mean(|J_ii|)` between Newton iterations, with λ shrinking on accepted line-search steps (toward pure Newton) and growing on rejected ones (toward gradient descent).  Runtime toggle: `SMSConfig::useLevenbergMarquardt` / `ManifoldSolverConfig::useLevenbergMarquardt` (bool, **default FALSE — opt-in**); wired through `RISE_API_Create*Rasterizer` factories.  Off by default because LM is ~50-100% slower than pure Newton on heavy-displacement scenes for ~5pp Newton-fail improvement — the cost-vs-gain doesn't justify a global default.  Set true at API construction time for heavy-displaced-mesh scenes that need the extra robustness.  No scene-language parser hook yet.  Variant: damped Newton on the original `J`, not full Marquardt-style normal equations — keeps the existing block-tridiagonal solver path.  **Root cause of the dramatic SMS power drop on heavily-displaced meshes is `ValidateChainPhysics` rejecting algebraically-valid-but-physically-nonsensical converged chains** — not Newton convergence failures (Pearson r = −0.914 between phys-fail rate and PT/VCM energy ratio across a 66-point grid; Newton-fail correlation is only −0.258, barely signal).  Smooth-displaced surfaces create a half-vector constraint landscape with many local minima, some of which correspond to chains where wi/wo are on the wrong sides of the surface for the labeled reflection/refraction topology.  Newton minimises ||C|| blindly and lands on those spurious minima.  **Heaviside displacement (step function, no smooth slopes) preserves energy at ANY amplitude** because its surface is mostly flat with one sharp boundary — only one real local minimum per seed.  Switching to a single-mode low-frequency painter (Gerstner with `num_waves=2 median_wavelength=1.0`) on the same scene at disp_scale=10: ok rate **23 % → 49 %**, energy ratio **0.41 → 0.71**, and the curve becomes invariant to tessellation detail.  Note: a controlled-smoothness experiment (`controlled_smoothness2d_painter`, 6 boundary continuity orders from C⁻¹ Heaviside to C^∞ Gaussian, matched amplitude/radius) refuted the naive "smoother painter = fewer Newton failures" hypothesis — all six modes cluster in 45-53 % ok% with no monotonic relationship to smoothness order.  Heaviside (worst continuity) actually has the HIGHEST ok rate because flat-region Newton converges trivially.  Practical rule: prefer FEWER bumps with longer wavelengths over many small bumps regardless of continuity; finer tessellation does NOT help and often hurts.  Per-Solve λ is local-stack (no global state, no thread races) and starts at 0 each call so the first iter is pure Newton — preserves baseline behaviour on well-conditioned chains.  Measured impact on the displaced Veach egg sweep (snell, M=1, no two-stage, no photons, disp 0..10): Newton-fail rate drops 4–10 pp at every disp value (e.g. disp=0 23.1% → 18.1%, disp=10 72.0% → 67.4%); 15–30k chains rescued per disp.  Smooth-analytic regression (sms_k2_glasssphere) gains +5.8 pp ok rate; plateau-bound scenes (sms_k1_refract, sms_k2_glassblock) unchanged within MC noise.  Cost: ~50-100% slower on heavy-displacement scenes (LM escalations consume the iter budget); negligible on smooth scenes / scenes without SMS.  Two regimes LM **cannot** fix: multi-basin landscapes with no descent connection from the seed (need better seeding — photons in the right basin), and C¹ discontinuities at triangle edges (need smoother surface evaluation).

- **SMS photons + heavy-displacement Newton plateau** ([docs/SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md](docs/SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md)): two correctness fixes plus the documented Newton-plateau limit on displaced caustics.  (1) Photon emit pre-populates the IOR stack via `IORStackSeeding::SeedFromPoint` — without this, photons emitted from a luminaire inside nested dielectrics die on bounce 1 (verified 0% → 81% photon-storage rate on the displaced Veach egg).  (2) Snell-mode SMS no longer gates `QuerySeeds` on `multi_trials > 1`; photon retrieval is independent of `multi_trials`, and `totalTrials` extends to absorb every queried photon — previously a scene that set only `sms_photon_count > 0` paid the photon-pass cost and never used a single seed.  (3) `maxPhotonSeedsPerShadingPoint` (default 16, Fisher-Yates random subsample at all 4 photon-consumption sites) bounds per-pixel Newton-solve cost so the decoupled photon path doesn't pay 100× overhead in dense kd-tree regions.  Fundamental finding: the disp 0 → disp 10 power drop on the Veach egg is **not** an iteration-budget issue (`maxIter=200` is identical to `maxIter=30`) and **not** a seed-quality issue (`seedTooFar=0%` everywhere) — Newton stalls on plateaus where ||C|| ∈ [0.1, 1.0) (74-85% of Newton-fails).  This is the documented basin-of-attraction limit of plain Newton on a displaced normal field.  Doc captures three avenues that would actually move the needle: robust solver (LM / trust-region), photon seeds + robust solver, or specular polynomials all-roots enumeration.  None are in master yet.

- `branching_threshold` (StabilityConfig, default 0.5): normalized throughput gate for subpath splitting at multi-lobe delta vertices (Fresnel dielectric etc.). Parameter on all rasterizer chunks (PT/BDPT/VCM pel & spectral, MLT). `0` = always branch at first encounter, `1` = never branch. Live on RGB + spectral NM for both eye and light subpaths across PT/BDPT/VCM — the photon-store build also branches and renormalizes `mLightSubPathCount` with actual `pathsShot`. MLT forces `1.0` (Markov-chain proposal assumes single-subpath). **Also live on uniform-mode SMS** (`sms_seeding "uniform"`): `BuildSeedChainBranching` reuses the same threshold to split seed chains at sub-critical dielectric vertices, finding Fresnel-reflection caustics that the legacy refraction-only `BuildSeedChain` misses. PT-faithful single-split semantic (matches `PathTracingIntegrator.cpp:1791`'s `!splitFired` guard). See [docs/SMS_UNIFORM_SEEDING_RESULTS.md](docs/SMS_UNIFORM_SEEDING_RESULTS.md). The retired `branch` param on PathTracing/DistributionTracing shader-ops is silently ignored by the parser; photon tracers keep their own `bBranch` (out of scope).
- **Chunk parsers are descriptor-driven** (since 2026-04). Every `IAsciiChunkParser` overrides only `Describe()` and `Finalize(const ParseStateBag&, IJob&)`; the default `ParseChunk` impl validates input lines against `Describe().parameters` and dispatches to `Finalize`. The descriptor IS the accepted-parameter set, so syntax-highlighter / suggestion-engine drift is structurally impossible. Adding a new chunk = new `IAsciiChunkParser` subclass + register in `CreateAllChunkParsers()`. Adding a parameter = one entry in `Describe()` + one `bag.GetX(...)` call in `Finalize`. Full how-to in [src/Library/Parsers/README.md](src/Library/Parsers/README.md); architecture overview in the header of [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp).
- Public construction API: [src/Library/RISE_API.h](src/Library/RISE_API.h)
- High-level construction interface: [src/Library/Interfaces/IJob.h](src/Library/Interfaces/IJob.h)
- Main assembly implementation: [src/Library/Job.cpp](src/Library/Job.cpp)
- Parser source of truth: [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp)
- Render-pass entry point: [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp)
- Scene files expect `RISE ASCII SCENE 5`, and chunk braces must be on their own lines
- `Job::InitializeContainers()` creates the `Scene`, named managers, `"none"` defaults, and default shader ops
- `Job::SetPrimaryAcceleration()` replaces the object manager and discards previously added objects
- Named managers plus explicit reference counting are foundational; inspect [src/Library/Managers/GenericManager.h](src/Library/Managers/GenericManager.h) and [src/Library/Utilities/Reference.h](src/Library/Utilities/Reference.h) before changing lifetimes
- Tests are standalone executables, not a unit-test framework suite
- The logger defaults to `RISELog.txt`, but the main CLI overrides it to `RISE_Log.txt` in the working directory
- **Scene-authoring conventions** ([docs/SCENE_CONVENTIONS.md](docs/SCENE_CONVENTIONS.md)): `directional_light.direction` is the vector FROM the surface TO the light source (a surface is lit when `N · direction > 0`), NOT the shine direction.  Cameras at `+Z` looking at origin need lights with **positive Z** in `direction`.  Importers from foreign formats (glTF, Unity, Unreal) that use the shine-direction convention MUST negate before passing through.  Other recurring traps captured in the same doc: `colorspace` selection, `power` semantics, `standard_object` transform precedence (`matrix > quaternion > orientation`), V-axis flip on glTF textures, alpha-mask only honored under `pixelpel_rasterizer`

## Thread priority — read before touching anything threading-related

- **Production default (topology-aware):** every P-core gets a render worker, every E-core except **one** also gets a worker. The reserved E-core keeps UI / daemons responsive. macOS workers use `QOS_CLASS_USER_INITIATED`; Linux/Windows use CPU-affinity pinning.
- **Benchmarks:** set `render_thread_reserve_count 0` to give every core a worker. `./bench.sh` at repo root does this automatically. Without the override, Apple Silicon's E-core reservation makes the machine appear ~10 % slower than it really is.
- **Do NOT** put all threads at `QOS_CLASS_UTILITY` / `BELOW_NORMAL` / `nice(10)`. That's the old bug — cost 2–4× real throughput on macOS. A legacy `force_all_threads_low_priority` option still exists for users who explicitly want the background-render mode; it's opt-in only.
- Controls live in `src/Library/Utilities/CPUTopology.{h,cpp}`, `src/Library/Utilities/ThreadPool.cpp`, `src/Library/Utilities/Threads/ThreadsPTHREADs.cpp`, `src/Library/Utilities/Threads/ThreadsWin32.cpp`.
- See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) "Thread priority policy" for the full rationale, per-platform APIs, and measurement recipe.

## Engineering Skills — read the matching skill before starting

Process skills distilled from prior RISE sessions.  Auto-invoked via
`.claude/skills/<name>/SKILL.md` shims; full content lives in
[docs/skills/](docs/skills/).

- [adversarial-code-review](docs/skills/adversarial-code-review.md) — validate a non-trivial change with 2–3 orthogonal reviewers in parallel.  Fire when the user asks for adversarial review / multiple reviewers, or after landing any correctness-sensitive change.
- [performance-work-with-baselines](docs/skills/performance-work-with-baselines.md) — structured quantitative perf work: baseline before code, measure with stddev, profile first, one variable at a time, correctness invariant, numeric reporting.
- [abi-preserving-api-evolution](docs/skills/abi-preserving-api-evolution.md) — when touching `RISE_API.h`, any `src/Library/Interfaces/` header, or a derived class that already has an overloaded method name.  Three layers (exported signatures, vtable slots, name hiding) each with their own failure mode.
- [const-correctness-over-escape-hatches](docs/skills/const-correctness-over-escape-hatches.md) — decision tree to run BEFORE reaching for `mutable` / `const_cast` / dropping a `const`.
- [precision-fix-the-formulation](docs/skills/precision-fix-the-formulation.md) — when you're tempted to widen a `< NEARZERO` check, add a fudge factor, or pick a magic ε to silence speckles / fireflies / FP-noise spurious hits.  Find the cancellation; deflate the polynomial / transform all corners / evaluate both algorithm branches instead of papering over with thresholds.
- [sms-firefly-diagnosis](docs/skills/sms-firefly-diagnosis.md) — when you see bright outlier pixels in an SMS render, or the user reports "fireflies" in an SMS scene.  Walks the firefly-triggered trace, decodes `attenChain` fingerprints into specific bugs, and lists audit sites (`ManifoldSolver`, `SMSPhotonMap`, PT emission-suppression state) that historically produce spurious contributions.
- [bdpt-vcm-mis-balance](docs/skills/bdpt-vcm-mis-balance.md) — when a BDPT or VCM render disagrees with PT (mean off, max blown out, splotches that don't fade with sample count), user reports "fireflies even at high spp", or you've just changed MIS code (`MISWeight`, `EvaluateNEE`, `EvaluateMerges`, `Convert*Subpath`, the auto-radius pre-pass).  Covers PT-vs-X test pattern, per-strategy instrumentation, the recurring delta-position-light vs delta-surface-scatter trap, and applying the fix at the right layer (skip rule vs running quantity vs auto-radius gate).
- [variance-measurement](docs/skills/variance-measurement.md) — when you need to PROVE a rendering change reduces variance / RMSE rather than just looks different.  K-trial EXR protocol with `bin/tools/HDRVarianceTest.exe`, master-vs-fix stash-rebuild-pop sequencing, both pure variance and RMSE-vs-reference metrics.  Knows the pitfalls (PNG quantization, HDRReader RLE bug, `LNK1104` SolutionDir trap, K too small).
- [effective-rise-scene-authoring](docs/skills/effective-rise-scene-authoring.md) — when authoring a new `.RISEscene` from scratch, porting from another tool (glTF / Blender / Unity / Unreal lighting setups), or diagnosing a scene that renders unexpectedly dark / wrong-coloured / oriented backwards.  Walks the [SCENE_CONVENTIONS.md](docs/SCENE_CONVENTIONS.md) checklist (directional `direction` is FROM-surface-TO-light, `power` semantics, colour-space defaults, transform precedence), then the Lambertian-control-sphere diagnostic before you touch material code.

## Read Next

- Core library map: [src/Library/README.md](src/Library/README.md)
- Interface contracts: [src/Library/Interfaces/README.md](src/Library/Interfaces/README.md)
- Scene language and parser behavior: [src/Library/Parsers/README.md](src/Library/Parsers/README.md)
- Scene taxonomy: [scenes/README.md](scenes/README.md)
- Curated showcase scenes: [scenes/FeatureBased/README.md](scenes/FeatureBased/README.md)
- Regression scenes: [scenes/Tests/README.md](scenes/Tests/README.md)
- Executable tests: [tests/README.md](tests/README.md)
- Thread-safety and scene immutability: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Planned improvements: [docs/IMPROVEMENTS.md](docs/IMPROVEMENTS.md)
