# Build flags: make and Xcode, measured (2026-11-27)

Scope: the optimisation and floating-point flags of the two macOS builds (`build/make/rise/Config.OSX` and
`build/XCode/rise/rise.xcodeproj`), the trade between build speed, numerical accuracy and run-time speed, and what
was changed. `docs/SIMULATION_PERFORMANCE.md` §10 raised the question ("a Deployment `rise-sim` may write different
bytes than the make `rise_sim`"); this is the answer. Fire was first left out; §7 covers the fire tier, which was run later.

## 1. Result in five lines

1. **The byte difference does not exist.** The Xcode Deployment and Development `rise-sim` write exactly the same
   frame sets (and the same manifest) as the make `rise_sim` on every scene that bakes, including with fast-math
   removed, with no LTO, with thin LTO, without `-funroll-loops` and with `-mcpu=native` (§3, §4.4). Both builds
   already compile every floating-point-bearing simulation source with `-fno-fast-math -ffp-contract=off`, and the
   two lists of such sources are identical (42 sources; checked mechanically, §6).
2. **Keep `-ffast-math -fno-finite-math-only` in make.** Dropping fast-math buys nothing measurable (run time within
   +-3%, renders statistically indistinguishable) and changes pinned bytes: 6 more fast-tier tests fail (§4.5).
3. **Thin LTO is fast (5 s incremental against 160 s, run time -6% to +3%, §4.2) and is the make default since
   fire solver version 4 (§8).** It first moved two pinned fire digests, so the default stayed full LTO (§7); the
   owner then approved compiling the fire-adjacent sources strict and re-pinning fire once (§8), after which fire's
   bytes are the same under thin, full and no LTO. `make LTO=thin|full|off` and the linker cache are in `Config.OSX`;
   switching modes needs `make clean`.
4. **Xcode: global `-ffast-math` added to Development, Deployment and Default of all three targets** (Opto already
   had it), so the floating-point flags of every non-fire source are the same as make's; every fire source keeps
   exactly the effective flags it had (proved from `clang -###`, §6). Bakes are identical before and after.
5. `-funroll-loops` and `-mcpu=native` have no measurable effect (instructions retired identical to 4 digits).

## 2. Method and caveats

Everything was done in private copies under `/Volumes/SSD/claude-tmp/flags-scratch/` (the real checkout's make
flags were never touched). Copy = `rsync` of the tree at 11:51 (including other agents' uncommitted changes) minus
`bin`, `rendered`, objects; `extlib` and `models` symlinked. Eight make trees A to H, one Xcode tree.

| tree | LTO | fast-math | other |
|---|---|---|---|
| A (current make) | full `-flto` | `-ffast-math -fno-finite-math-only` | `-funroll-loops` |
| B | thin | on | |
| C | none | on | |
| D | full | off (`-fno-finite-math-only` kept) | |
| E | thin | off | |
| F | none | off | |
| G | full | on | no `-funroll-loops` |
| H | full | on | `-mcpu=native` (apple-m4 here) |
| xc | Xcode Deployment, thin | off (before the edit) | no unroll, no OpenVDB define |

* The machine was shared the whole time (load average 8 to 150, mostly 15 to 40; other agents building and running
  tests). **Wall times are therefore weak; the robust figures are user+sys CPU seconds and, for run time,
  instructions retired and cycles (`/usr/bin/time -l`), which do not depend on load.** Cycles carry a few percent of
  noise from SMT and cache sharing; differences under about 3% are not significant. Each run time is the mean of 3
  runs (bakes: 2).
* **Renders are not reproducible run to run** even with one thread and a pinned `srand` (a `DYLD_INSERT_LIBRARIES`
  interposer on `srand` was tried; the integrators draw from scheduling-dependent `ThreadLocalSeed`). A same-seed
  pixel comparison is therefore impossible. Accuracy is measured instead as: relative RMSE between independent
  renders of one build (the noise floor) against relative RMSE between a render of the baseline and a render of
  the variant (3 x 3 pairs); a variant is equivalent when the ratio is about 1; and the ratio of mean image values.
* Render scenes (EXR output, 16-bit float, scratch copies in `scenes/_flags/`): `Tests/Bench/bench_pt` (path
  traced, adaptive, 256x256), `Tests/Spectral/perf_avocado_spectral` at 128 spp (spectral, textured),
  `Tests/Volumes/pt_heterogeneous_sphere` at 16 spp (heterogeneous volume). Each takes 55 to 115 CPU seconds
  (6 to 10 s of wall on a quiet 12+4 core machine). `bench_pt` is adaptive: its work (instructions) depends on the
  variant, so read it as a rough guide only.
* Bakes: `cloth_flag_scene` (91 frames), `liquid_dam_break` (first 12 frames), `rigid_cloth_trampoline` (62 frames,
  cloth + rigid group). Frame files are named by content hash, so equal `*.frame` name sets mean equal bytes.
* Environment for every run: `RISE_SIMULATION_CACHE_DIR` under the scratch folder, `RISE_SIMULATION_TRUSTED_ROOTS=/Volumes/SSD`.

## 3. Task 1: is there a byte difference? No.

Frame-set comparison (number of frames; "same" = identical set of content-hashed frame names; manifest compared
byte for byte on `cloth_flag_scene`: identical).

| scene | frames | make A vs Xcode Deployment | make A vs Xcode Development (-O0) |
|---|---|---|---|
| cloth_flag_scene `flag` | 91 | same | same |
| liquid_dam_break `pool` (12 frames) | 12 | same | same |
| rigid_cloth_trampoline `trampoline` (full / 20 frames) | 62 / 42 | same | same (all 42 frames) |
| cloth_chain_scene_inputs (10 frames) | 10 | same | same |
| liquid_cloth_catch / flap / wave (10 requested) | 6 / 6 / 6 | same | same |
| liquid_honey_water | 10 | same | same |
| liquid_rigid_bodies | 33 | same | same |
| simulated_cloth_drape, simulated_points_spray | 0 (nothing bakes) | n/a | n/a |

Renders (Xcode Deployment before the edit vs make A, 3 runs each): relative RMSE ratio to the noise floor 0.97 to
1.00; image mean ratio 0.9995 to 0.9999: numerically equivalent within sampling noise (§4.3).

Why: the per-file exception in make (`CLOTH_STRICT_OBJS`, `LIQUID_STRICT_OBJS`, `M6_STRICT_OBJS`, fire and OSL
rules) has an exact twin in the Xcode project (`COMPILER_FLAGS = "-fno-fast-math -ffp-contract=off"` on the same
sources in all three targets). The solvers are therefore compiled identically whatever the global flags are, and
the cache code (hashing, CBOR) has no floating-point-order dependence. Residual risk: a header-only helper
instantiated in both a strict and a non-strict source could be merged by the linker to the non-strict copy; no
such case appeared in any of 14 configurations (below), but it is why §6 keeps the lists checkable.

## 4. The matrix

### 4.1 Clean build (`make -j10 rise`, which builds `rise`, `rise_sim`, `simulation_cache_import`)

| config | wall s | user+sys CPU s | notes |
|---|---|---|---|
| A full LTO, fast-math | 187 | 465 | most of it one single-threaded LTO link per binary |
| B thin LTO, fast-math | 61 | 594 | |
| C no LTO, fast-math | 59 | 412 | |
| D full, no fast-math | 231 | 517 | |
| E thin, no fast-math | 73 | 609 | |
| F none, no fast-math | 50 | 413 | |
| G full, no unroll | 226 | 487 | |
| H full, native | 209 | 491 | |

Wall times were taken at load 16 to 78 and are indicative only; CPU time is the comparison. Full LTO costs about
50 CPU seconds of link-time code generation more than no LTO; thin about 180 more (it is parallel, so the wall
time is the lowest of the LTO modes). The patched make (§7) built clean in 59 s wall.

### 4.2 Incremental rebuild after touching one library .cpp (`src/Library/Shaders/PathTracingShaderOp.cpp`; make rebuilds it, the archive and relinks the three binaries)

| config | wall s (3 runs) | CPU s |
|---|---|---|
| A full LTO | 163, 155, 164 | 155 to 162 (one core) |
| D full, no fast-math | 173, 167, 175 | 165 to 170 |
| B thin, no linker cache | 38, 32, 31 | 292 to 299 (everything re-optimised in parallel) |
| E thin, no fast-math | 39, 31, 31 | 298 to 304 |
| **B thin + `-Wl,-cache_path_lto`** | **5.3, 5.4, 5.5** (first run, cold cache: 33) | **6.3** |
| C / F no LTO | 3.3, 3.4 (first run 12.5) | 4 |
| Xcode Deployment (thin, Xcode's own LTO cache), content edit | 14, 15, 21 | 36 |
| Xcode Development (-O0), content edit | 56, 58 | 53 |

Notes: the first run after a clean build also recompiles `Job.cpp` (the build-identity header changes when the
tree becomes dirty); every real edit does the same, adding a few seconds. Xcode skips a pure `touch` (content
signature), make does not. Xcode Development takes about 55 s because of the link of the 85 MB unoptimised binary
(only `Job.cpp` and the edited file compile); that was not investigated further.

### 4.3 Render run time and accuracy (3 runs per cell, ratios to A)

| scene | variant | instructions / A | cycles / A | CPU s / A | relative RMSE to A / noise floor | mean image / A |
|---|---|---|---|---|---|---|
| avocado_spec (spectral, 128 spp) | B thin | 1.000 | 0.978 | 0.966 | 1.004 | 0.9994 |
| | C none | 1.028 | 1.038 | 1.041 | 1.002 | 0.9998 |
| | D no fast-math | 1.025 | 0.988 | 0.980 | 1.002 | 0.9998 |
| | E thin, no fm | 1.025 | 1.028 | 1.027 | 1.003 | 1.0002 |
| | F none, no fm | 1.049 | 1.061 | 1.058 | 1.004 | 0.9995 |
| | G no unroll | 1.000 | 0.981 | 0.980 | | |
| | H native | 0.999 | 0.974 | 0.960 | | |
| bench_pt (path traced, adaptive) | B | 0.968 | 0.948 | 0.936 | 0.969 | 1.0000 |
| | C | 1.014 | 0.961 | 0.962 | 0.992 | 0.9999 |
| | D | 0.996 | 0.976 | 0.969 | 0.979 | 1.0000 |
| | E | 0.971 | 0.924 | 0.927 | 0.945 | 1.0000 |
| | F | 1.008 | 1.031 | 1.028 | 1.006 | 0.9997 |
| | G | 1.000 | 0.998 | 0.997 | | |
| | H | 1.000 | 0.984 | 0.978 | | |
| hetero_sphere (volume, 16 spp) | B | 0.985 | 0.995 | 0.999 | 1.023 | 0.9999 |
| | C | 1.013 | 0.980 | 0.985 | 1.007 | 0.9999 |
| | D | 1.003 | 0.997 | 0.994 | 1.006 | 0.9999 |
| | E | 0.989 | 0.979 | 0.981 | 1.010 | 1.0000 |
| | F | 1.017 | 0.987 | 0.989 | 1.011 | 0.9999 |
| | G | 1.000 | 1.007 | 1.003 | | |
| | H | 1.000 | 0.998 | 0.995 | | |

Reading: nothing differs from A by more than the 3 to 6% noise, except that no-LTO is 4 to 6% slower on the
spectral scene (and neutral on the other two). Fast-math off is not slower (D is -2%, -3%, -1% in CPU). The noise-floor ratio is 0.95 to 1.02 for
every variant, i.e. every variant renders statistically the same image; image means agree to 6e-4 or better.
Xcode Deployment (thin, no fast-math, no unroll) before the edit: CPU 1.018 (avocado), 0.863 (bench_pt, an
adaptive scene whose instruction count is 10% lower and which no make variant reproduced; not explained),
0.980 (hetero_sphere).

### 4.4 Simulation bake run time and bytes (2 runs each, ratios to A; liquid and trampoline are largely single-threaded; instructions of the threaded liquid pool include spin time, so use cycles)

| variant | cloth cycles | liquid cycles | trampoline cycles | frame sets vs A |
|---|---|---|---|---|
| B thin | 0.956 | 1.029 | 0.989 | identical |
| C none | 0.948 | 1.049 | 0.982 | identical |
| D no fast-math | 0.964 | 0.950 | 0.997 | identical |
| E thin, no fm | 0.946 | 1.039 | 0.983 | identical |
| F none, no fm | 0.979 | 1.042 | 1.002 | identical |
| G no unroll | 0.992 | 1.021 | 0.978 | identical |
| H native | 0.954 | 1.022 | 0.975 | identical |
| Xcode Deployment | 0.957 | 1.034 | 0.979 | identical |

All within +-5%; no variant is clearly better. A's absolute figures (loaded machine): cloth 0.85 s wall, liquid 12
frames 4.4 s, trampoline 43 s.

### 4.5 Fast test tier (`./run_all_tests.sh --tier fast --timeout 300 --filter '[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`, 452 tests, run inside the copies; the runner locates its lock in the copy's `bin/` and its logs by the copy's path, so the real checkout was not touched)

Baseline A fails 4 (not caused by flags): `CstDeriveGoldenTest` (red on master already), `SourceHygieneTest` (two
projected-Heun source guards), `SimulationCacheTest` (the scratch folder's ancestor `/Volumes/SSD` is not "private"
to the trust check), `LiquidClothCouplingTest` (physical bands; the copy carries other agents' uncommitted liquid work).

| config | failures | beyond A's 4 |
|---|---|---|
| A | 4 | none |
| B thin LTO | 5 | non-fire tier only (fire tier: §7). `SimulationHostTest` ("publish cost stays flat", a timing check): passes 2 of 2 when rerun alone; a load flake |
| D no fast-math | 10 | **6 on pinned bytes or fixed-seed outcomes**, listed below |

D's six: `ProceduralMeshTest` (pinned digest: got 0x7d1ab32d398c879d, want 0x50894075681f42cc), `TextureExpressionVMTest`
("pre-change golden" fbm and ridged values), `EmitterAverageExitanceTest` (flux ratio 0.9989 against a tight
tolerance), `ObjectMirrorTest` (exact equality of a mirrored hit and its twin), `ReliefModifierTest` (legacy
reference bitwise inert), `VCMVolumeMergeTest` (fixed-seed statistical band, 2 of 2 reruns fail, passes in A).
Not measured: the extended tier and Fire* tests (the latter must not be run here); more pinned values may move.
C, E, F, G, H were not run through the tier (their test library is the same non-LTO build as B's or D's, and
bakes are identical); `SSSNearUnityFresnelTest` (an LTO-codegen-sensitive test that links the LTO library) passed
under B.

## 5. Decision

| question | decision | reason |
|---|---|---|
| fast-math in make | **keep** `-ffast-math -fno-finite-math-only` | Removing it changes pinned bytes (6 fast-tier tests, more elsewhere) and gains nothing measurable: run time within noise, renders and bakes equivalent. Rule: keep pinned bytes unless the alternative is clearly better; it is not |
| fast-math in Xcode | **add it** (same flags as make) | Floating-point flags identical in every build, so output cannot depend on which one built it, including for any future simulation source that lacks the per-file exception. Measured: bakes identical before and after; renders equivalent |
| LTO in make | ~~full stays the default; `LTO=thin` is an opt-in switch~~ (§7). **Superseded by §8: thin is the default** | Thin: 5 s against 160 s incremental, run time -6% to +3%, bakes and the non-fire fast tier identical, **but `FireProductionSolverTest` (r83 table package digest) and `FireSimulationSolverTest` (r80 cycling branch) fail**; no strict or no-LTO compile of the sources involved reproduces the pins (§7), and re-pinning was not allowed at fire version 3. The re-pin was approved later (fire version 4, §8) |
| LTO in Xcode | keep thin (Deployment), none (Development), full (Opto) | already the fast setting; Opto remains the full-LTO shipping option |
| Development carries the same FP flags | **yes, done**: `-ffast-math -fno-finite-math-only` plus the same per-file strict list | Nothing in `-O0` prevents it; bakes of the Development build equal make's (all scenes, §3). So bytes do not depend on the configuration |
| `-funroll-loops`, `-mcpu` | leave | no measurable effect: instruction counts equal to 4 digits, cycles within noise. Native ties the binary to one M generation for nothing |
| strict (no fast-math) everywhere | **not recommended now** | Re-baseline cost above; revisit only if a correctness bug traces to fast-math (renders already agree within noise) |

## 6. Xcode changes applied (`build/XCode/rise/rise.xcodeproj/project.pbxproj`, 39 insertions, 30 deletions)

1. For the 9 target-level configurations (`rise`, `RISE-GUI`, `rise-sim` x Development, Deployment, Default): add
   `"-ffast-math",` before `"-fno-finite-math-only",` in `OTHER_CPLUSPLUSFLAGS`, and set
   `RISE_FP_CONTRACTION_MODE = fast` (it feeds the renderer build identity; make reports `fast` too). The three Opto
   configurations already had both.
2. Per-file flags (all three targets, 7 sources x 3 = 21 entries) so that fire-adjacent sources keep the flags they
   had: `FireOptics.cpp`, `FireParameterSchema.cpp`, `FireProductionTables.cpp`, `FireSequence.cpp`,
   `FireSequenceCache.cpp`, `FireSimulationRecords.cpp` get `-fno-fast-math -ffp-contract=on`;
   `FireProductionComputeMac.mm` becomes `-fobjc-arc -fno-fast-math -ffp-contract=on`. `-ffp-contract=on` is
   clang's default, i.e. exactly the flag they had before; they stay strict-by-default in Xcode.
3. Already present, unchanged: `-fno-fast-math -ffp-contract=off` on every source of make's strict lists
   (7 cloth, 6 rigid/group/coupling, 9 liquid, 6 fire solver, 8 fire production, 6 OSL; 42 sources, 3 target
   entries each, checked by script against `CLOTH_STRICT_OBJS`, `LIQUID_STRICT_OBJS`, `M6_STRICT_OBJS` and the fire
   and OSL rules of `Makefile`); `FireCase.cpp` keeps its own `-O3 -ffast-math ... -fno-lto`.

**Proof that no fire source changed.** `clang -###` on every compile command of the Deployment `rise-sim` build
(476 sources), before and after: the effective cc1 option set is identical for all 50 strict, OSL, fire and
FireCase sources, including `FireProductionComputeMac.mm` and `FireOptics.cpp`; the 426 changed ones are all
non-fire render and library sources (gaining `-ffp-contract=fast`, `-freciprocal-math`, `-funsafe-math-optimizations`,
`-fno-signed-zeros`, `-fapprox-func`, `-mreassociate`, the same set make's `-ffast-math` gives). The only
simulation-named sources among them are `ClothParameterSchema.cpp`, `LiquidParameterSchema.cpp`,
`RigidParameterSchema.cpp` and `ExpressionOslBuiltins.cpp`, which are not strict in make either. No fire tool,
test or GPU run was needed; the `--check` mirrors were not used.

**Verification in the real checkout** (derived data and `DSTROOT` under the scratch folder, per-configuration
`DSTROOT`: a shared one let the Development `rise` overwrite the Deployment one):
`rise`, `rise-sim` and `RISE-GUI`, Development and Deployment: all six `BUILD SUCCEEDED`; warning sets identical to
the unedited project built in the scratch copy (apart from time stamps and the dependency `rise-sim` link
warnings, which appear only when `rise-sim` is not already built). `git status` shows only `project.pbxproj`
changed under `build/`.
Bytes after the edit: the checkout's sources keep moving (other agents), so the byte comparison used a snapshot
of the real checkout with the edited project (`flags-scratch/fin`): make `rise_sim`, Xcode Deployment `rise-sim`
and Xcode Development `rise-sim` on the 9 bakeable scenes of §3: **all frame sets identical** (Development
trampoline: first 20 macro steps = 42 frames). Renders (3 x 3 pairs, make against Xcode Deployment after the edit):
relative RMSE ratio to the noise floor 0.993 (bench_pt), 1.000 (avocado), 1.012 (hetero), mean image ratio within
2e-4; CPU 0.90, 0.99, 1.00 of make's.

Still different between the builds, none of it affecting simulation bytes: LTO mode (thin in Xcode Deployment, none
in Development, full in make and Opto), no `-funroll-loops` in Xcode (no effect), no `-DRISE_ENABLE_OPENVDB` in
Xcode (a feature, not arithmetic), `-DCOLORSPACE_RGB` in Xcode against `-DCOLORS_RGB` in make (both are dead macros:
no source tests them), and **the seven fire-adjacent sources above: fast-math in make, strict in Xcode**. The last
one predates this change; it could make the app's fire tables and records differ from the CLI's. It was not tested
(fire is out of scope here) and not changed (changing it moves Xcode's fire flags).

A mechanical guard worth adding: a check that the per-file strict lists of `Makefile` and `project.pbxproj` stay equal.

## 7. Make: the LTO switch, and why thin LTO is not the default (found 2026-11-27, after the first version of this document)

`Config.OSX` now has `LTO ?= full` (`make LTO=full|thin|off`; thin adds `-Wl,-cache_path_lto,bin/lto-cache`), and
`FireCase.o`'s rule filters `-flto=%` as well as `-flto` (it keeps that object out of LTO; without the fix thin mode
would have compiled it to bitcode). The default is today's build, object for object. When switching modes delete the
library objects (`find src tools -name '*.o' -delete`, plus `tests/SSSNearUnityFresnelTest.o`,
`tests/FireSimulationSolverTest.o`, `tests/FireProductionSolverTest.o`): make does not track the mode, and the three
`TESTS_LINK_LTO` tests link the LTO library. The exact diff is `git diff build/make`.

**Finding.** Under `LTO=thin` (first version of the patch; fire tier run in a copy and in the real checkout) 13 of 16
fire-tier tests passed. The failures:

| test | failure | note |
|---|---|---|
| `FireProductionSolverTest` | r83 table package digest differs from the pinned fixture | thin 0d83f723...; pinned 3e8f4650... |
| `FireSimulationSolverTest` | r80 recorded cycling state: the complementarity discrepancy is pinned to the bit (0.0029506166530252633) | |
| `FireSolverTest` | stop/resume cache seal | the known race with another agent relinking binaries; passes in a quiet run |

These two tests link the LTO library (`TESTS_LINK_LTO`), and their pins were recorded with full LTO and fast-math.

**Bisect (copy `flags-scratch/bis`, tests' objects rebuilt each time; `FireProductionSolverTest` table package id).**

| compile of the library | LTO | id | pin matches |
|---|---|---|---|
| as pinned (fast-math, as in `Config.OSX`) | full | 3e8f4650... | yes (also the r80 value) |
| as pinned | thin | 0d83f723... | no, r80 fails too |
| the six fire-adjacent `.cpp` strict (`-fno-fast-math -ffp-contract=off`), rest as pinned | thin | e7912153... | no, r80 fails |
| everything without `-ffast-math` (contraction at clang's default) | thin and full | e7912153... | no |
| the six `.cpp` fast-math but linked outside LTO (the `FireCase.o` method) | thin and full | 633a3668... | no |

(Sources: `FireOptics`, `FireSimulationRecords`, `FireProductionTables`, `FireSequence`, `FireSequenceCache`,
`FireParameterSchema`, plus `FireProductionComputeMac.mm`.) Reading: the digest is a function of the floating-point
code generation. Strict IEEE (with or without contraction) gives a third value, so **the pinned value is not the strict
IEEE result**; it is the product of fast-math code with full-LTO cross-translation-unit inlining, and thin LTO's
different inlining produces a different value. A strict or no-LTO compile makes the result independent of the LTO mode
(e7912153... and 633a3668... are the same under thin and full) but not equal to the pin, which would need a re-pin;
that is not permitted for fire version 3, so no flags were changed for the fire sources and the default stays full.
Matching the pin under thin LTO would need thin's inlining to equal full's, which no flag guarantees.

**Fire tier in the real checkout under the final configuration (full LTO, clean rebuild of all library objects, the
checkout's fast tier filter `Fire*,SimulationGoldenCacheTest,SimulationSceneTest,SourceHygieneTest`): 17 of 17 pass**,
including `FireSolverTest` (184 s), `FireSequenceTest`, `FireProductionSolverTest`, `FireSimulationSolverTest`,
`SimulationSceneTest`, `SimulationGoldenCacheTest`, `SourceHygieneTest` (so the Xcode project guards pass with the
edited project). The runner's fire-mirror `--check` steps (embedded fire-optics and fire-simulation records, HITEMP
Planck-mean record and tools, quarantined HITEMP record) all print `pass`. No fire source was edited.

**Consequence for Xcode.** Make's fire-adjacent sources (the seven above) keep fast-math in make, and Xcode keeps them
strict (§6). Their digests therefore differ between the two builds: from the bisect, a strict build gives table package
id e7912153... where the CLI gives the pinned 3e8f4650.... Xcode Deployment is thin LTO, which would not reproduce the pin
even with the make flags. So the app's fire table package digest differs from the CLI's; it was not run in the app
(fire GPU solver not used for this). The owner's options: (a) accept, since app and CLI fire are separate bakes;
(b) re-pin fire to strict IEEE (re-baseline fire version 4); (c) build fire in the app with the Opto configuration
(full LTO, fast-math), which is the only Xcode configuration that could reproduce the pin, untested.

### Per-file flags at fire solver version 3 (superseded by §8)

| sources | make (`Config.OSX` + `Makefile`) | Xcode (all three targets, every configuration) |
|---|---|---|
| 42 simulation, fire solver, fire production and OSL sources (lists in §6) | `-fno-fast-math -ffp-contract=off` | same |
| `FireCase.cpp` | `-ffast-math ... -funroll-loops`, no LTO | `-O3 -ffast-math -fno-finite-math-only -funroll-loops -ffp-contract=fast -fno-lto` |
| `FireOptics`, `FireSimulationRecords`, `FireProductionTables`, `FireSequence`, `FireSequenceCache`, `FireParameterSchema`, `FireProductionComputeMac` | global flags: `-ffast-math -fno-finite-math-only`, LTO as selected | `-fno-fast-math -ffp-contract=on` (unchanged effective flags) |
| everything else | global flags (`-O3`, `-ffast-math -fno-finite-math-only`, `-funroll-loops`, LTO as selected) | global flags (`-O3`, `-ffast-math -fno-finite-math-only`, thin LTO in Deployment, none in Development, full in Opto) |

## 8. Fire solver version 4: the fire-adjacent sources strict, thin LTO the default (2026-11-27, owner-approved re-pin)

The owner chose option (b) of §7. Everything was first done in a private copy of `cc071c604`
(`/Volumes/SSD/claude-tmp/strictpin-scratch/RISE`, its own git metadata so the build identity works; deleted afterwards, logs and bakes kept in that folder), then applied to
the real checkout with one `make clean` and a thin rebuild. Physical consequences and the re-pinned values are in
`docs/SIMULATION_M8_RESULTS.md` §15.

**The strict set.** The seven sources of §7 (`FireOptics`, `FireSimulationRecords`, `FireProductionTables`,
`FireSequence`, `FireSequenceCache`, `FireParameterSchema`, `FireProductionComputeMac.mm`) now compile with
`-fno-fast-math -ffp-contract=off`, exactly like the 42 of §6, in make (`FIRE_ADJACENT_STRICT_OBJS` and the
`FireProductionComputeMac.o` rule; their test-library copies join `TEST_SAFE_FP32_OBJS` / `TEST_SAFE_FP32_MM_OBJS`) and
in Xcode (the 21 entries of §6 item 2 change `-ffp-contract=on` to `off`; contraction on was not make's strict mode).
Two more make-only objects: `tools/fire_simulator.o` (the reference simulator; it shares `tools/fire_simulator_core.h`
with the strict CPU backend) and `tests/FireOpticsPresetTest.o` (its mutation fixtures recompute FireOptics' PCHIP
derivative enclosures with a one-ulp margin, so the mirror must use FireOptics' arithmetic; with fast-math in the test
and strict FireOptics, 8 mutation checks hit the enclosure gate first). `FireProductionSolverTest` leaves
`TESTS_LINK_LTO` (its digest no longer depends on LTO).

**Found and left alone (would be invasive; for the owner).**

1. `FireCase.cpp` stays fast-math outside LTO (its own rule, identical in Xcode). It is already LTO-independent.
   Compiled strict it changes the r57 source-pattern digest (`FireCaseTest`, `e399780e...`) and with it every case
   record ID, which committed evidence and docs quote; not done.
2. `FireSimulationSolverTest` (the reference simulator's unit tests, a fast-math test translation unit linked with LTO)
   cannot be made LTO-independent without changing gates. Its own code computes the r80 values, so they move with the
   LTO mode, and some of its roundoff gates hold only for fast-math with LTO inlining:

   | build of the test | r80 discrepancy (selected / other branch) | other failures |
   |---|---|---|
   | fast-math, thin LTO (new default) | 0.0029506166530252841 / 0.038419987516984999 (**pinned now**) | none |
   | fast-math, full LTO | 0.0029506166530252633 / 0.038419987516982668 (the old pin) | r70 pilot ledger (pilot-energy error 1.4e-14) |
   | fast-math, no LTO | 0.0029506166530252494 / 0.038419987516979823 | r70 pilot ledger |
   | strict (`-fno-fast-math -ffp-contract=off`), any LTO | strict value (not pinned) | "finite-increment consumers propagate binary32 precision" (the raw state's volume ratio is 0.99999999999999989, one ulp below 1, so the zero packet's expansion is -1.1e-16, not exactly 0); "V2/V3 projected-Heun executes converged R0, R1, R2" (periodic divergence-target mean -5.55e-14 against its 64-epsilon compatibility tolerance 1.4e-14) |

   So it stays in `TESTS_LINK_LTO`, fast-math, with r80 pinned to the thin value. **It is the one fire test whose
   pins depend on the LTO mode**; under `LTO=full` and `LTO=off` it fails (table). Making it strict means re-deciding
   the two exactness expectations above (and possibly the reference simulator's arithmetic, which the CPU backend
   shares): an owner decision.
3. `LTO=off` cannot link `bin/tests/FireProductionCalibrationOracle` (undefined `...MetalDiagnostic` /
   `...MetalWorkingSetBytes` symbols referenced from `FireSequenceTest.o`; with LTO they are dead-stripped first), so a
   plain `make LTO=off tests` stops there; the runner builds what it needs and the fire tier still runs. Present before
   this change (no source involved was touched); not fixed.

**LTO independence, measured** (copy, a clean build per mode; fire tier = `--filter
'Fire*,SimulationSceneTest,SimulationGoldenCacheTest,SourceHygieneTest,CstDerive*'`, 19 tests; bakes = `rise_sim bake`
of a tier-6 certified Metal window (0.0625–0.13 s, two frames) and a tier-4 CPU window (0–0.0625 s, two frames),
compared by the native `.frame` files and the legacy frames, checkpoint VDBs and frame times; manifests differ only by
producer build identity and wall times):

| build | fire tier | bakes |
|---|---|---|
| thin LTO (default) | 19 of 19 (after the re-pin) | reference |
| full LTO | 18 of 19: only `FireSimulationSolverTest` (item 2) | identical |
| no LTO | 18 of 19: only `FireSimulationSolverTest` (item 2); `FireCPUBackendTest` first failed its CLI render check because `bin/rise` was missing after item 3 stopped the build, and passes once `make LTO=off all` built it | identical |
| thin, and **every other library source strict too** (no fast-math anywhere except `FireCase`) | table package digest, scene keys, `FireSequenceTest`'s pinned digests, agreement, solver and CPU tests unchanged; failures only where test or render code itself is compiled differently (`FireSimulationSolverTest`, the two `FireProductionSolverTest` fixtures whose test-side references mirror fast-math code, `CstDeriveGoldenTest`, and `FireSequenceTest`'s capstone spectral preview, which renders black with an all-strict renderer: renderer behaviour, not investigated) | identical |

The last row is the test for "nothing fire pins comes from fast-math code elsewhere": changing the arithmetic of every
non-fire source (colour, spectral, interpolation and math helpers included) moves no fire byte. It is a stronger version
of the "different optimisation of unrelated code" check, so `-funroll-loops` off was not run separately.

**Real checkout** (`make clean`, then thin `all tests build-tool/rise_sim build-tool/simulation_cache_import`: warning-free):
fire tier 19 of 19 after the strict re-pin, and 19 of 19 again after the chemistry-light change of §15 (which moved no
pinned value).

**Xcode against make** (real checkout; derived data and per-configuration `DSTROOT` under the scratch folder):
all six builds (`rise`, `rise-sim`, `RISE-GUI` × Development, Deployment) `BUILD SUCCEEDED`; apart from the Homebrew
"built for newer macOS" linker notices the only warnings are the project's existing ones (`rise`: unsandboxed script
phase; `RISE-GUI`: AppIntents metadata skipped). The compile lines show `-fno-fast-math -ffp-contract=off` last on the
seven sources. The Xcode **Deployment** `rise-sim` writes exactly the make `rise_sim`'s fire frames (native `.frame` files,
legacy frames, checkpoint VDBs, frame times) and the same job record for the tier-6 Metal bake and the tier-4 CPU bake;
the **Development** (`-O0`) `rise-sim` writes the same CPU-backend frames (1,843 s against 177 s). Before this change the
seven sources were fast-math in make and contraction-on strict in Xcode, so the two builds' table packages differed (§7).
The Development `rise-sim` **cannot run the Metal bake**: it stops at step 0 with "projected-Heun resident owner
certificate failed: working_set_passed=0 actual_bytes=6403604480 certified_bytes=4570578944". A Development `rise-sim`
built from `cc071c604` (before this change) fails identically, so this is pre-existing and unrelated to floating-point
flags (a Debug-build memory difference, not investigated).

### Final per-file flags (fire solver version 4)

| sources | make (`Config.OSX` + `Makefile`) | Xcode (`rise`, `rise-sim`, `RISE-GUI`; every configuration) |
|---|---|---|
| 42 simulation, fire solver, fire production and OSL sources (lists in §6) | `-fno-fast-math -ffp-contract=off` | same |
| `FireOptics`, `FireSimulationRecords`, `FireProductionTables`, `FireSequence`, `FireSequenceCache`, `FireParameterSchema` (.cpp), `FireProductionComputeMac.mm` (+ `-fobjc-arc`) | `-fno-fast-math -ffp-contract=off` | same |
| `tools/fire_simulator.cpp`, `tests/FireOpticsPresetTest.cpp` | `-fno-fast-math -ffp-contract=off` | (not in the project) |
| `FireCase.cpp` | `-ffast-math ... -funroll-loops`, no LTO | `-O3 -ffast-math -fno-finite-math-only -funroll-loops -ffp-contract=fast -fno-lto` |
| everything else | `-O3 -ffast-math -fno-finite-math-only -funroll-loops`, **thin LTO by default** (`LTO=full|off`) | `-O3` (`-O0` in Development) `-ffast-math -fno-finite-math-only`, thin LTO in Deployment, none in Development, full in Opto |

Per-file flags in Xcode do not add `-O3`, so the strict sources compile at `-O0` in Development; strict IEEE code gives
the same bytes at any optimisation level (checked for fire below and for cloth/liquid in §3).

**Switching LTO modes needs `make clean`.** make does not record the mode: objects of two modes would be mixed
(thin bitcode with full bitcode or native objects), which has broken a fire test link before. The renderer build
identity records the mode, so the producer build ID of every cache changes with it (frames do not).

## 9. Not verified

* (§8) The extended tier under full or no LTO; fire under `-funroll-loops` off alone (covered by the all-strict
  variant); the Opto configuration; `RISE-GUI` was only built, not run.
* Fire in the app (§7, Consequence for Xcode): superseded by §8, where the Xcode `rise-sim` bakes were compared.
* The extended tier under any variant, and the fire tier under no-LTO make; `RISE-GUI` was only built, not run; no headless
  GUI probe. (Fire tier under no-LTO make: done in §8.)
* Pixel-exact render equality (impossible: renders are not reproducible); `bench_pt`'s 10% lower instruction count in
  Xcode builds is unexplained.
* Quiet-machine wall times. Clean-build and run-time wall figures carry the load noted in §2.
* C, E, F, G, H through the fast tier (argued from B and D, §4.5).
* `docs/SIMULATION_PERFORMANCE.md` §10 still says the Deployment build may write different bytes; it is superseded
  by this document and was left untouched.
