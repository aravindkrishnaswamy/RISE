# Test Guide

The tests in this repository are standalone executables built from `tests/*.cpp`. They are not managed by a unit test framework.

Scene-based validation taxonomy now lives in [../scenes/README.md](../scenes/README.md) and [../scenes/Tests/README.md](../scenes/Tests/README.md).

## Build And Run

### Linux / macOS

```sh
make -C build/make/rise tests
./run_all_tests.sh
```

Build behavior comes from [../build/make/rise/Makefile](../build/make/rise/Makefile). The makefile glob picks up every `tests/*.cpp` file automatically and links it against the core library.

### Windows

```powershell
# One-time configure:
cmake -S build/cmake/rise-tests -B build/cmake/rise-tests/_out -A x64

# Prereq: build the Library + CLI from build/VS2022/RISE.sln in both Debug
# and Release once so bin/RISE.lib and dbin/RISE.lib exist.
.\run_all_tests.ps1                              # Release
.\run_all_tests.ps1 -Config Debug                # Debug
.\run_all_tests.ps1 -Filter Math3DTest,*Noise3D* # Subset by wildcard
.\run_all_tests.ps1 -TimeoutSeconds 60           # Kill any test exceeding 60s
```

Build behavior comes from [../build/cmake/rise-tests/CMakeLists.txt](../build/cmake/rise-tests/CMakeLists.txt). CMake globs every `tests/*.cpp` into its own per-test executable, links against the existing `RISE.lib` produced by the VS2022 Library project, and stages the OpenEXR + OIDN runtime DLLs alongside each test exe.

Built binaries land in `bin/tests/` (Release) or `dbin/tests/` (Debug).

#### Windows debt

Known `_WIN32` branches in `tests/` that have **not** been compiled by an
MSVC build yet, so the first Windows run of each should expect to fix them:

* `ExpressionMemoTest.cpp` — check (j)'s temp-directory and child-process
  plumbing (`_putenv_s`, `GetTempPathA` / `GetTempFileNameA` /
  `CreateDirectoryA` / `RemoveDirectoryA`, and the `set "X=Y" && ...`
  command string handed to `std::system`), added 2026-09-07. The POSIX
  branch is exercised on every macOS/Linux run.

## Test Map

There are 219 standalone `tests/*.cpp` executables as of 2026-07-24. Do not
maintain a hand-counted filename inventory here: the CST, editor, GUI,
FrameStore, agent, and eval work adds tests often enough that such lists become
wrong within days. The filesystem and build globs are authoritative:

```sh
rg --files tests -g '*Test.cpp' | sort
find tests -maxdepth 1 -type f -name '*.cpp' | wc -l
```

Useful filename families:

- `Agent*Test.cpp`: agent session, chat loop, MCP/stdio/HTTP transports,
  autonomy, proposals, renders, viewport reads, trajectories, and evals
- `Cst*Test.cpp`: lossless parsing, derive contracts, minimal-diff edits,
  identity, reference graphs, scene variants, incremental apply, and cost gates
- `SceneEditor*Test.cpp`, `Viewport*Test.cpp`, `SourceTraceTest.cpp`, and
  `EntityTemplatesTest.cpp`: editing, persistence, source traceability,
  render modes, N-up panes, and entity creation
- `FrameStore*Test.cpp`, `FrameEncoderTest.cpp`, and
  `ViewportFrameStoreTest.cpp`: framebuffer storage, color math, encoders, and
  GUI delivery
- `AutoRasterizerTest.cpp`, `BDPT*Test.cpp`, `VCM*Test.cpp`,
  `ManifoldSolverTest.cpp`, and the `*Spectral*Test.cpp` family: transport,
  MIS, SMS, and spectral behavior
- geometry, materials, painters, samplers, volumes, importers, color, and
  utility code use descriptive subsystem prefixes

CLI diagnostics and data-processing programs that require file arguments live
under `tools/`; they are not assertion-based `run_all_tests` executables.

## Style Of Test Used Here

- Each file is an executable with its own `main`.
- Assertions are usually plain `assert(...)`.
- Helpful progress text is printed with `std::cout`.
- The best targets are deterministic helpers, math utilities, cache logic, and other focused behavior that does not require comparing full rendered images.
- For procedural / noise tests, separate **exact contract checks** from **sampled-difference heuristics**. Put exact identities first in `main()` and label the weaker sampled-difference checks clearly so future readers do not mistake them for strong oracles.
- Prefer exact identities such as periodicity, parameter collapse (`blend=0`, `warpAmplitude=0`, `persistence=0`), symmetry, sign behavior, and simple analytic reference points before adding "different settings produce different outputs" checks.
- Ignored `*.o` files or `* 2.o` files under `tests/` are local build artifacts, not source-of-truth tests.

## Per-Test Knobs

A few render-comparison tests take arguments or read an environment
variable.  These exist so a tolerance can be *derived* (re-run the same
case at several independent seed bases) without paying for the whole
file each time.  They never change what a case asserts.

| Test | Knob | Meaning |
|---|---|---|
| `FabricRenderTest` | `argv[1]` — seed base (default 1000) | `std::srand(seedBase + n)` before render *n*; different bases are independent runs **by construction**, which is how the file's tolerance derivations were measured. |
| `FabricRenderTest` | `FABRIC_TEST_FILTER` (env) | Runs only the cases whose keyword is a substring of the value.  Keywords: `hwss`, `parity`, `curtain`, `arealit`, `touching`, `wrapped`, `gaparea`, `closedbox`, `mediumvertex`.  Unset (the CI invocation) runs everything.  A filtered run's `Passed:`/`Failed:` counts are over the selected subset only, so it is a measurement aid, **not** a substitute for the full-suite gate. |
| `PrimitiveSelfHitTest` | `argv[1]` — seed base (default 1000) | Same `std::srand(seedBase + n)` convention as `FabricRenderTest`; its bands were derived over bases 1000..5000. |
| `PrimitiveSelfHitTest` | `PRIM_TEST_FILTER` (env) | Runs only the cases whose keyword is a substring of the value.  Keywords: `crossing` (the transmissive light-behind rows), `lambertian` (the radius-1000 sphere and its unit twin).  Unset runs everything; a filtered run's counts are over the subset only.  Note the Lambertian rows' derivation was taken filtered (its four renders — PT and BDPT for the radius-1000 row and for the unit twin — at render indices 0-3), so an unfiltered run, after the crossing test's 18 renders, seeds them at indices 18-21 — a different seed sequence, well inside the band's headroom. |
| `BDPTStrategyBalanceTest` | — | No knobs.  Its PT reference is `pathtracing_pel_rasterizer` (since 2026-09-05; it was the legacy `pixelpel_rasterizer`, which executes the scene's `standard_shader` chain literally and so is only as complete as the ops that chain lists — see the history block near the end of that file and docs/CLOTH_FABRIC_DESIGN.md §15 debt 26).  Both rasterizer strings set `oidn_denoise FALSE`: the in-test capture output receives post-denoise pixels through the default `OutputDenoisedImage`, and OIDN's timing-based `auto` quality made a topology's PT mean bimodal between identical runs.  Topology J (debt 30) is the submerged-floor scene shared with `VCMStrategyBalanceTest` topology H, and is green BEFORE and AFTER the η² basic-radiance fix by construction: an eye path that enters the water and exits it toward the emitter multiplies 1/n² by n², so it pins the cancellation that hid the missing factor.  Topology K (debt 30 review round 2) is the AIR-ceiling / submerged-floor-and-emitter scene shared with `VCMStrategyBalanceTest` topology I: unlike J's isolated in-and-out cancellation, here the eye-side s=0/s=1 strategies (crossing INTO the water, carrying the η² factor) and the t==1 light-tracing splat (crossing OUT of the water while building the light subpath, carrying none) are two MIS strategies for the SAME path, combined at one shared vertex — a consistency pin on the cross-strategy combination itself, green before and after this round's TranslucentSPF fix by construction (the scene never touches that material).  66 checks, ~36 s (measured this round). |
| `VCMStrategyBalanceTest` | — | VCM/PT strategy-balance gate with nine render topologies: omni, mesh emitter, mixed lights, focused/defocused thin lens, six-bladed aperture, orthographic camera, submerged floor, and air ceiling with submerged floor/emitter. Direct-only topologies use the legacy direct-lighting chain; the two submerged topologies use modern PT. Both captures are composed over black before statistics, with an exact coverage-alpha oracle. Explicit box filtering, denoising off, and a fresh libc seed per render; scheduling still prevents bit reproducibility. Default mean/p99/max bands are 8%/25%/100%, orthographic mean is 4%, submerged-floor bands are 8%/60%/4x, and ceiling bands are 8%/30%/100%. The ceiling topology is a consistency pin; the alpha oracle is red-proven in the DL-01 cleanup. |
| `RefractiveRadianceScalingTest` | `argv[1]` — seed base (default 1000) | docs/REFRACTIVE_RADIANCE_SCALING.md: the η² basic-radiance factor a RADIANCE-mode walk must apply whenever a scattered ray's medium changes (`(η_before/η_after)²`; importance-mode walks get none).  Two CLOSED-FORM rows and two agreement rows: (A) a Lambertian luminaire submerged in a water box seen from air at near-normal incidence must read `T·L/n²` at ior 1.33 AND 1.5, on PT, BDPT, VCM and the legacy `pixelpel_rasterizer` (four rasterizers because `DefaultRefraction`'s kray consumer is a different code path from the integrators'; two iors so a wrong-direction or hard-coded factor cannot pass both) — review round 3 added a fifth variant of row A at ior 1.33 with `mat_water` swapped from `dielectric_material` to `perfectrefractor_material` (a second producer through the same consumer sites, green without any code change); (B) a camera INSIDE a lossless ior-1.5 box under a uniform env of radiance 1 must read exactly `n²` = 2.25 — the equilibrium statement, independent of the Fresnel curve, and the EXIT direction row A cannot reach; (C) VCM within 8% of PT on a submerged floor lit by a sphere emitter in air (pre-fix 1.149×); (D) transparent-shadow PT within 15% of VCM on the same slab with a delta omni, where plain PT is structurally 0.  38 checks, ~27 s.  Red-proof (12 failures) recorded in the header. |
| `CameraImportanceTest` | — | No knobs, no renders, no RNG dependence — every oracle is a closed form and every sample point is a fixed stratified grid, so it is bit-reproducible.  docs/RENDERING_INTEGRATORS.md debt 28: the camera side of the t==1 light-tracing strategy.  Pins that `ThinLensCamera::GetApertureWorldArea()` really is 1/(the density `SampleLensPoint` draws at) for disk / polygonal / anamorphic apertures, that `RasterFromLensPoint` round-trips `GenerateRayWithLensSample` to 6e-14 px (with lens shift on), that the circle of confusion is exactly zero on the plane of focus and exactly the analytic radius off it, that `Importance` / `PdfDirection` equal the matched pinhole's closed form at EVERY f-stop (the aperture area cancels — that IS the fix), and that a uniformly radiating plane filling the frustum integrates to a film response of exactly 1 for pinhole and thin lens alike.  Test 4 asserts the circle of confusion's SIGNED per-axis scale against `kRasterSign{X,Y} * H*(1/S1 - 1/S2)/(2 tan)`, not just its magnitude, so a mirrored or never-reversing blur fails.  960 checks, under a second. |
| `SignalIntegratorConsistencyTest` | `SIGNAL_CONSISTENCY_FILTER` (env) | docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md S2: PT/BDPT/VCM (plus one MLT smoke row) must price `curv`/`occlusion(r)`/`convexity(r)`/`thickness(r)`/`proximity(r)`/`interior(r)` the same way.  Layer 1 is six constant-signal unit scenes (a control expression with the signal replaced by its known constant — closed form for curv/proximity/interior, a direct non-rendering evaluation of the production estimator for occlusion/convexity/thickness); Layer 2 is a whole-image AND a masked ratio-of-ratios (the mask is the pixels where the two PT renders, signal-live vs signal-neutral, actually disagree — whole-image is insensitive on every showcase measured so far) over the four Textures/Combined showcases at reduced resolution, with a per-integrator blow-up gate that skips (counts, never asserts on) a >2x/<0.5x integrator-vs-PT disagreement ONLY when E and B also agree with each other within 10% — an asymmetric blow-up (only one of E/B outside the band, or both outside but disagreeing) fails loudly instead, as a possible signal-attributable regression — as a recorded non-signal issue otherwise (docs/RENDERING_INTEGRATORS.md).  Filter substrings: `unit` (layer 1 only), `showcase` (all four layer-2 showcases), or a showcase's own keyword (`plank`, `tidal`, `bunny`, `pavilion`).  Unset (the CI invocation) runs both layers, **2 min 47 s** wall as of 2026-09-11 (from ~104-109s; the masked sub-render count is now ADAPTIVE — at least 12, then onward while the standard error of the mean masked ratio exceeds a quarter of the 20% band, capped at 48, and a row that reaches the cap without the precision prints `INSUFFICIENT PRECISION` and counts as a skip rather than a pass or a fail — and each sub-render draws its OWN PT denominator pair, since the PT pair is a multiplicative factor common to every sub-render and re-rendering only BDPT/VCM left its noise entirely intact).  Also takes an optional seed base as `argv[1]` (default 1000, the tests/FabricRenderTest.cpp / tests/PrimitiveSelfHitTest.cpp convention): this binary is NOT wall-clock seeded (no `srand( GetMilliseconds() )` call is on its path), so `std::srand( seedBase + renderIndex )` runs immediately before every render it issues, making repeat default runs reproducible in intent and `./SignalIntegratorConsistencyTest 2000`, `3000`, ... genuinely independent samples by construction rather than by hoping the unsynchronized worker-side `rand()` race decorrelates them.  As of 2026-09-11 (debt 28 review round 2 ruling), the tidal masked BDPT row's earlier framing overclaimed its statistic: "13.6 standard errors outside the 0.20 band" was `|mean|/SE` (standard errors from ZERO), not from the band edge — the correct distance is `(0.2443-0.20)/0.0179 ~= 2.5 SE`.  The real "not a flake" argument is CROSS-RUN reproducibility (a reviewer saw -0.252..-0.273 over five fresh runs, none passing), not one run's SE — and point estimates like "-0.2443" are one draw from a spread, not bit-reproducible run to run (pre-existing worker-side `rand()` race).  Deeper problem found on top of that: the masked ratio-of-ratios assumes PT's own bias is material-independent between the E and B variants at the masked pixels, which fails when the masked set is reached only by a transport class PT's NEE cannot sample (tidal's delta light refracted through dielectric water) — PT is then an INCOMPLETE reference, not merely a noisy one.  A new reference-completeness gate checks each masked (showcase, integrator) row's neutral-variant ratio to PT; a row that misses by more than 50% prints `REFERENCE INCOMPLETE ON MASK`, is excluded from the PT-referenced assertion, and is counted in a NEW skip kind, `g_referenceIncompleteCount` (distinct from the blow-up skip and the precision skip) — printed in the summary line.  Whenever that gate trips, the suite instead asserts a PT-INDEPENDENT cross-check between BDPT and VCM's own masked E/B self-ratios (same adaptive-K machinery, same PT-derived mask, no PT in the asserted quantity).  On tidal this triggers for BOTH integrators (BDPT's masked neutral ratio to PT ~2.8-3.0x, VCM's ~57.5-58x) and the BDPT<->VCM cross-check itself sits at the edge of its own 20% band: -0.1996 (SE 0.0199, PASS) on one run, -0.2354 (SE 0.0129, FAIL) on another — a real, open BDPT-vs-VCM disagreement, reported honestly rather than loosened, tracked as its own item, debt 30, in docs/RENDERING_INTEGRATORS.md (not debt 28, the camera-aperture arc; debt 27 keeps only the PT-strategy-gap data).  Typical summary line (round 4, the current shape): `Passed: 2968  Failed: 0  Blow-up skips: 1  Masked precision skips: 0  Reference-incomplete masked rows: 2  No-complete-reference skips: 1  Whole-image insensitive showcases: 4  Masked layer dropped: coverage: 0` — green, with every drop counted; before round 3's symmetric rule the tidal cross-check flapped between pass and `Failed: 1` on the same ~20% BDPT/VCM disagreement.  (It was GREEN at 1528 passed / 0 failed / 4 blow-up skips when S1 landed) — the file's own header carries both the pre-S1 RED numbers and the post-S1 GREEN numbers, the red-proof mutation outcomes (dropping `signals` vs `derivatives` from `PathVertexEval::PopulateRIGFromVertex`), and the masked-band derivation.  As of 2026-09-11 (debt 28 round 3 ruling), the reference-completeness rule above was found to have a gap: the PT-INDEPENDENT BDPT<->VCM cross-check it falls back to is only a meaningful referee if BDPT and VCM agree with each other on the NEUTRAL variant's masked pixels to begin with — on tidal they don't (BDPT/PT masked ~2.8-3.0x, VCM/PT masked ~57.5-58x, i.e. BDPT and VCM disagree with EACH OTHER by ~20x there), which is exactly why the cross-check itself was landing a coin flip, -0.1996 (pass) on one run and -0.2354 (fail) on another, against its own 20% band.  The rule is now SYMMETRIC: before asserting the cross-check, the test computes `| mean_mask(BDPT,B) / mean_mask(VCM,B) - 1 |` (reusing the existing 50% `kReferenceIncompleteThreshold`, no new magic number) and, if it exceeds that band, prints a labelled `NO COMPLETE REFERENCE ON MASK` line naming the BDPT/VCM neutral-variant ratio and both PT ratios, skips the assertion, and counts it in a FOURTH skip kind, `g_noCompleteReferenceSkipCount` (distinct from the blow-up skip, the masked-precision skip, and the reference-incomplete skip).  On tidal the BDPT/VCM neutral-variant masked ratio is ~0.05 (BDPT's masked B mean is ~20x VCM's), which trips the new gate, so the cross-check is now skipped and reported rather than asserted and flaked on.  Two full runs at this round (seed bases 1000 and 2000) both landed the identical summary: `Passed: 2968  Failed: 0  Blow-up skips: 1  Masked precision skips: 0  Reference-incomplete masked rows: 2  No-complete-reference skips: 1`.  This is not a loosened test — BDPT and VCM disagreeing ~20x on tidal's caustic-lit pixels is a real, open integrator disagreement, now tracked as its own item, debt 30, in docs/RENDERING_INTEGRATORS.md, distinct from debt 27's PT-strategy-gap framing. |
| `TranslucentSpectralParityTest` | — | DL-01: deterministic exit/backscatter lobe-weight checks against Beer extinction, RGB/NM parity at 450/550/650 nm, and chained NM entry/exit throughput with tau paid once. Covers zero/unit tau, scattering endpoints, zero/nonzero extinction and distance, uniform and per-channel Phong N. DL-02 adds analytic exit-density support, cosine inverse-CDF and hemisphere-normalization checks at N=1/8 and per-channel N=5/10/15, scattering endpoints, tilted normals, and entry-horizon controls. |
| `MISWeightsTest` | — | Exact MIS-weight identities plus DL-03 shared guided-continuation stack boundary checks: same/opposite sides, geometric-normal orientation, sampling-frame fallback, tangent directions, exact retained candidate and absent transition. No rendering. |
| `TranslucentIORStackTest` | — | No knobs or rendering. Original SPF stack oracles are exact; DL-03 adds seeded sampled-coverage counters around exact downstream stack checks. Regression guard for debt 30's P2-1 fix: `TranslucentSPF::Scatter`/`ScatterNM` fabricated a medium change by pushing the literal `1.0` onto the IOR stack when a ray entered a `translucent_material` object, instead of the enclosing medium's own IOR — `translucent_material` has no `ior` parameter, so a translucent object nested in water (or any non-air medium) got mispriced by `RISE::RadianceEtaScale` as a 1.33 → 1.0 transition (a 1.7689x, i.e. n², throughput inflation) instead of no transition at all.  Three sub-tests cover all three entry push sites (single-ray RGB, the per-RGB-channel loop for an anisotropic Phong N, and the NM twin), each chaining the entry call's own output stack into a second Scatter call to also exercise the matching exit pop — pre-fix, BOTH directions failed (entry at n² = 1.7689, the chained exit at 1/n² = 0.565323); post-fix both read exactly 1.  The anisotropic sub-test also red-proofs an incidental sibling bug found in the same loop: `TranslucentSPF.cpp`'s entry-side per-channel loop (~line 154) wrote `trans.kray[0] = p[0]` on every iteration instead of `trans.kray[i] = p[i]`, zeroing channels 1 and 2 of that lobe — it checks the summed `kray` across the three emitted rays reconstructs the untouched color exactly, one channel per ray.  The same sub-test also red-proofs that loop's exit-side sibling (`TranslucentSPF.cpp` ~line 230-231): `front.kray = 0; front.kray[i] = f[i]*(1-scat[i]);` was reset to 0 on every iteration, but `front` (the exit diffuse ray) is added to `scattered` only ONCE after the loop, so only the last-written channel (B) survived on the ray that actually leaves the object — checked by asserting all three channels of the exit ray's `kray` are non-zero and equal `f*(1-scat)` componentwise.  Review round 3 added two more: the anisotropic sub-test also asserts `delete_stack == true` on every entry-loop ray that carries a non-null `ior_stack` (C1 -- the loop reused one local across three iterations and a fresh `IORStack` allocation per iteration without re-arming `delete_stack`, leaking two stacks per call on the success path), and a new sub-test 4 asserts that a (0, 0.5, 0.5) reflectance/transmittance painter still emits both the front and translucent entry lobes (C2 -- both lobes were gated on channel 0 alone, so a painter with zero red silently emitted neither).  Original four sub-tests retain their checks. With OpenPGL, DL-03 also exercises production PT RGB/NM from an explicitly seeded diffuse arrival, and BDPT eye/light RGB/NM from real translucent entry/exit scatters. A real trained guiding field supplies directions; only the later intersections are controlled. PT both modes, BDPT one-sample eye/light, and BDPT light RIS require actual outward substitutions, then check the carried stack and the next same-object Scatter classification; one-sample inward directions must retain the pre-exit stack. RIS retained-SPF candidates and unguided paths are separate positive controls. BDPT eye RIS currently retains every SPF candidate because DL-43 rejects guide candidates; its exit PDF-query count must exceed the unguided baseline and its retained-candidate count must be positive, without claiming actual guide replacement. A second PT fixture tilts the shading normal 60 degrees from the geometric normal and requires both inward and outward guide replacements of a geometrically inward SPF exit; actual replacements are classified by incoming direction and geometric normal. Unchanged SPF candidates retain their legacy behavior and are not claimed geometrically correct. No claim of image-energy parity or dedicated PT HWSS guiding coverage. |
| `SignalEmitterRecordTest` | `argv[1]` — seed base (default 1000), or `--louvres-only` | docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md S3: an EMISSIVE material keyed on a geometry signal (`curv`, `proximity`) or on `Po` must read the same value when its light is reached by NEE / the light-subpath root as when it is hit directly — six families (SDF sphere, analytic sphere, proximity-keyed quad, `Po`-keyed emitter with a gate-invariance block, high-epsilon SDF floor, two-lobe non-convex emitter with `casts_shadows FALSE`), PT/BDPT/VCM plus spectral and HWSS rows; 93 checks including the DL-36 pin; `std::srand( seedBase + n )` before every render, the PrimitiveSelfHitTest convention. Eleven red-proofs recorded in the header. DL-36 adds an exact two-blade luminary consistency pin for the accepted neighbour payload and geometry-preserving replay (`--louvres-only`). |

Example — re-deriving the backlit-curtain band:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
for b in 1000 2000 3000 4000 5000; do
  FABRIC_TEST_FILTER=curtain ./bin/tests/FabricRenderTest $b
done
```

## Adding A New Test

1. Add a new `tests/<Name>.cpp` file.
2. Include the minimal headers you need from `src/Library`.
3. Keep the test deterministic and fast.
4. Use `assert` for pass/fail checks.
5. Build with `make -C build/make/rise tests` on Linux/macOS, or `cmake --build build/cmake/rise-tests/_out --config Release --target rise_all_tests --parallel` on Windows.
6. Run with `./run_all_tests.sh` on Linux/macOS, or `.\run_all_tests.ps1` on Windows.

No makefile edit is needed for a new `tests/*.cpp` file because the existing wildcard-based rule discovers it automatically. The Windows CMake test project also auto-discovers new `tests/*.cpp` files via `file(GLOB ... CONFIGURE_DEPENDS ...)`.

## Transport Correctness Scenes (Roadmap Step 2)

These scenes validate spectral and SMS correctness. They require visual or
statistical comparison rather than deterministic assertions.

Assume `RISE_MEDIA_PATH` is set to the repo root before running any of the scene-based checks below:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
```

### Spectral Non-Mesh Lights (2B)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/Spectral/cornellbox_pointlight_spectral.RISEscene
```

**Expected**: The scene is illuminated (not black). Before the fix, spectral rendering with point lights produced a completely black image because `EvaluateDirectLightingNM` skipped non-mesh lights.

### SMS Visibility (2D)

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_visibility_unoccluded.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_visibility_occluded.RISEscene
```

**Expected**: The unoccluded scene shows a caustic beneath the glass sphere. The occluded scene blocks the caustic with an opaque wall. Note: inter-specular visibility (occluders between glass vertices) is not checked; see `ManifoldSolver::CheckChainVisibility` documentation.

### SMS Spectral Regression

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/Spectral/spectral_dispersive_caustic_pt_sms.RISEscene
```

**Expected**: Dispersive glass caustic with per-wavelength evaluation. The sphere should show a slight chromatic tint from dispersion via the spectral PT + SMS path through `ManifoldSolver::EvaluateAtShadingPointNM` (G(x,v_1) · |det(δv_1/δy)| geometry).

### PT + SMS Caustic Regression

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_slab_close_vcm.RISEscene             # reference
printf "render\nquit\n" | ./bin/rise scenes/Tests/SMS/sms_slab_close_pt_sms_hispp.RISEscene   # unit under test
```

**Expected**: The PT+SMS render's caustic mean luminance should match the VCM reference within ~5%.

## BSSRDF Furnace Tests (Energy Conservation)

These scenes validate that the BSSRDF subsurface scattering implementation conserves energy. They use a large sphere (R=10, ~40x mean free path) in a uniform emissive box so the geometry approaches the flat-slab limit where analytical predictions are available.

```sh
printf "render\nquit\n" | ./bin/rise scenes/Tests/BSSRDFFurnace/furnace_sss_absorption.RISEscene
printf "render\nquit\n" | ./bin/rise scenes/Tests/BSSRDFFurnace/furnace_sss_zero_absorption.RISEscene
```

**Output format**: HDR explicitly converted to `ROMMRGB_Linear`. RISE's working
space is Rec.709 Linear, so this is no longer a verbatim-store path; the ratios
below are intentionally measured after the same output conversion in both
renders.

**Verification procedure**:
1. Render both scenes
2. Measure sphere center vs background corner pixel values in each HDR image
3. Compute `ratio_abs = sphere/bg` for the absorption scene and `ratio_zero = sphere/bg` for the zero-absorption scene
4. The corrected ratio `ratio_abs / ratio_zero` should match the flat-slab prediction within 1%:
   - Red: 0.995, Green: 0.872, Blue: 0.672
5. The zero-absorption scene should have equal ratios across all channels (~0.96, deficit from probe failures/recursion limits)

**What this catches**: Any regression in BSSRDF weight computation, Fresnel handling, profile evaluation, or importance sampling PDF that would break energy conservation.

**Companion unit test**: `tests/BSSRDFSamplingTest.cpp` tests the same properties deterministically without rendering (profile normalization, sampling consistency, Fresnel conservation, Sw normalization, weight formula correctness, flat-slab energy balance).

## Path Guiding RIS Regression (Roadmap Stage 8)

The script `tests/test_ris_regression.sh` is an automated regression test for the RIS path guiding implementation.  It renders a Cornell box at 128×128 / 64 SPP with both RIS and one-sample MIS guiding, then compares mean luminance, firefly counts, and floor variance.

```sh
bash tests/test_ris_regression.sh
```

**Thresholds:**
- Luminance difference < 5% (energy conservation).
- RIS fireflies < 3× MIS fireflies + 10 (no firefly regression).
- RIS floor variance ratio < 2.0 (no variance explosion).

**Exit code:** 0 on pass, 1 on failure.

**Requirements:** RISE binary (`bin/rise`), Python 3 with Pillow and numpy.  Optional: scipy for neighbor-aware firefly detection.

The script generates a minimal Cornell box scene on the fly, so it does not depend on any checked-in scene files.  Temporary files are cleaned up on exit.

### Related Scenes

- `scenes/Tests/PathTracing/pt_guiding_stress_ris.RISEscene` — small-opening stress test with RIS guiding.
- `scenes/Tests/PathTracing/pt_indirect_test_ris.RISEscene` — indirect-only Cornell box with RIS guiding.
- `scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene` — PT jewel vault with RIS guiding.
- `scenes/Tests/BDPT/bdpt_jewel_vault.RISEscene` — unguided BDPT comparison for the same composition.

### Adaptive Alpha

The guiding alpha is adaptively scaled using a variance-aware approach inspired by Rath et al. 2020.  The coefficient of variation (CoV) of indirect sample energy determines how much the guiding distribution helps.  An alternative Cycles-style approach (using `sqrt(indirectFraction)`) was also tested — see the inline comments in `src/Library/Rendering/PixelBasedPelRasterizer.cpp` and `BDPTRasterizerBase.cpp` for how to switch between approaches.

## Relationship To Sample Scenes

- Use `tests/` for deterministic logic and small subsystem checks.
- Use `scenes/FeatureBased/` for curated showcase and torture scenes.
- Use `scenes/Tests/` for isolated regression, comparison, and image-validation scenes.
- If a feature is user-visible and deterministically testable, it usually deserves both.
