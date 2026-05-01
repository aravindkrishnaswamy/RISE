# Cameras — Research & Roadmap

**Status:** Research-only. No code yet. This document is the source of truth
for the camera-side design before any implementation lands. Refine in place.

**Scope:** Survey the production / research state-of-the-art for camera models
beyond the four RISE ships today (pinhole, orthographic, fisheye, thin-lens),
score each against RISE's particular strengths, and lay down a phased roadmap
that respects the project's frozen `ICamera` ABI and descriptor-driven parser.

**Spectral angle:** RISE's spectral-NM rasterizers are the single biggest
asymmetry vs. PBRT-derivative renderers and the foundation for half of the
features below. Wherever a feature is qualitatively *better* in a spectral
pipeline (chromatic aberration, lens flare, sensor SPDs), the doc calls that
out — those are the highest-ROI wins because nobody else does them well.

---

## 1. Status quo — what RISE has today

Source files in [src/Library/Cameras/](../src/Library/Cameras/):

| Camera                | File                                     | Parser keyword          | Notes |
|-----------------------|------------------------------------------|-------------------------|-------|
| Pinhole (FOV)         | [PinholeCamera.h](../src/Library/Cameras/PinholeCamera.h) | `pinhole_camera`        | Plain perspective. |
| Pinhole (ONB)         | (same)                                   | `onb_pinhole_camera`    | Basis-built variant for explicit axes. |
| Orthographic          | [OrthographicCamera.h](../src/Library/Cameras/OrthographicCamera.h) | `orthographic_camera`   | Parallel projection. |
| Fisheye               | [FisheyeCamera.h](../src/Library/Cameras/FisheyeCamera.h) | `fisheye_camera`        | Equidistant projection (180° field). |
| Thin-lens             | [ThinLensCamera.h](../src/Library/Cameras/ThinLensCamera.h) | `thinlens_camera`       | Disk aperture + focus distance. |
| Photographer thin-lens | (same impl)                             | **`realistic_camera`**  | Stub — re-parameterised thin-lens (film size + f-stop), delegates to `SetThinlensCamera`. Used by exactly one test scene (`scenes/Tests/Cameras/realistic.RISEscene`); no production scenes. **Keyword is free to repurpose for Phase 4.** |

Cross-cutting infrastructure already in place:

- **`CameraCommon`** ([CameraCommon.h](../src/Library/Cameras/CameraCommon.h)) factors location/lookat/up, exposure, scanning rate, pixel rate, orientation, ONB/frame, and the editor mutation surface. Every new camera should derive from it.
- **Time-resolved sensor scaffolding.** `exposureTime` / `scanningRate` / `pixelRate` are wired through `ICamera` already — the rolling-shutter and motion-blur weighting plumbing partially exists but isn't a first-class shutter model yet.
- **Lens-sample injection for MLT.** `ThinLensCamera::GenerateRayWithLensSample` takes a primary `Point2` lens sample directly so PSSMLT's small-step mutations on the aperture coordinate stay continuous. The handle is **non-virtual** by design (`MLTRasterizer` `dynamic_cast`s; `ICamera` vtable is frozen — see [ICamera.h](../src/Library/Interfaces/ICamera.h) lines 68–79). Any new lens camera must follow this pattern, not extend the vtable.
- **Descriptor-driven chunk parsing.** Each camera = one `IAsciiChunkParser` subclass with `Describe()` + `Finalize()` ([AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) §"Cameras"). Adding a camera is one new struct + one register call.

What is conspicuously **absent**:

- No multi-element lens system simulation (no glass surfaces, no per-wavelength refraction, no exit-pupil sampling).
- No tilt-shift / Scheimpflug. No anamorphic squeeze. No polygonal aperture.
- No equirectangular / cubemap / dome / ODS output cameras.
- No proper sensor model (spectral SPD, Bayer, read noise, non-rectangular shutter). Only the integration-time scalars are exposed.
- No camera-importance `We()` plumbing distinct from primary-ray generation; BDPT/VCM connections through a thin lens are correct but a multi-element lens needs an explicit importance evaluator.

---

## 2. State-of-the-art landscape

Tiered roughly by distance from where RISE sits today.

### 2.1 Tier 1 — Thin-lens enrichments

Still thin-lens at heart; pick up most of the cinematic effects working DPs care about. Days of work each.

| Feature | What it adds | Notes |
|---|---|---|
| **Tilt-shift / Scheimpflug** | Tilt focal plane and/or shift image plane. Architectural correction (shift), miniature-fake & wedge focus (tilt). | Few-line modification of primary-ray gen. Reference: [Kensler, *Ray Tracing Gems II* ch. 31](https://link.springer.com/chapter/10.1007/978-1-4842-7185-8_31). |
| **Polygonal / textured aperture** | 5/6/7/8-blade hexagonal/heptagonal bokeh, or arbitrary-shape texture-sampled aperture. | Same lens math, different importance sampler over the aperture. |
| **Optical vignetting / cat's-eye bokeh** | Off-axis bokeh truncates because rear lens elements clip the cone. | Falls out of a real lens stack for free; on thin-lens it's a fake (composite of two displaced apertures). |
| **Anamorphic squeeze** | Non-uniform horizontal/vertical scale + oval bokeh. The 2.39:1 cinematic look. | RenderMan and Lentil both expose this. |
| **Focus breathing** | FOV changes with focus distance. | Small but it's what makes virtual cameras look "off" to film people. |

### 2.2 Tier 2 — Realistic multi-element lens (the big jump)

Origin: **[Kolb, Mitchell & Hanrahan, SIGGRAPH '95](https://graphics.stanford.edu/papers/camera/)**. Refined in PBRT v3/v4 as `RealisticCamera`. Today's production baseline.

Camera = a stack of optical surfaces (spherical / aspheric / biconic, each with thickness, curvature radius, aperture radius, glass IOR). Each primary ray is *traced through the lens stack* with refraction at every interface.

You get for free, with no extra code paths:
- All five Seidel monochromatic aberrations (spherical, coma, astigmatism, Petzval field curvature, distortion).
- Real off-axis vignetting (cos⁴ falls out automatically; cat's-eye bokeh too).
- Correct entrance / exit pupil — must be sampled, not just "disk on the front element."
- **Chromatic aberration** if (and only if) you trace per-wavelength with the glass's Sellmeier / Cauchy IOR. **This is RISE's spectral-pipeline payoff.** Other renderers fake CA by jittering the IOR; in a spectral pipeline it's the truth.
- Focus shift across the spectrum, lateral colour, "glow" near focus, etc.

PBRT ships a small lens database (Tessar, Double-Gauss, fisheye Nikkor, Sony 28-mm zoom). The format is plain-text and can be parsed directly.

References: [PBRT v3 — Realistic Cameras chapter](https://www.pbr-book.org/3ed-2018/Camera_Models/Realistic_Cameras), [PBRT v4 — Cameras and Film](https://pbr-book.org/4ed/Cameras_and_Film).

### 2.3 Tier 3 — Wave-optics extensions

Geometric optics breaks down at small apertures, near point lights, and for high-contrast star sources.

- **Aperture diffraction → starburst** patterns around bright lights (the n-pointed star at f/16; n = 2× blade count for even n, equal for odd). FFT-PSF convolution or sampling the Fraunhofer pattern.
- **Lens flare / ghosting** — multi-bounce paths inside the lens stack between element pairs. [Hullin et al. 2011 — *Physically-Based Real-Time Lens Flare*](https://resources.mpi-inf.mpg.de/lensflareRendering/) traces an enumerated set of two-bounce paths. With BDPT/VCM you could in principle MIS the lens stack as part of the path; in practice every production renderer does flare as a separate compositing pass.

In a spectral renderer these get qualitatively better — rainbow ghosts, wavelength-dependent fringe spacing.

### 2.4 Tier 4 — Acceleration of multi-element simulation

Tracing 6–8 refractions per primary ray is expensive, especially with BDPT/VCM where you connect *through* the camera many times.

- **Polynomial Optics** ([Hullin, Hanika, Dachsbacher, Heidrich 2012 (UBC TR)](https://www.cs.ubc.ca/labs/imager/tr/2012/PolynomialOptics/); [Hanika & Dachsbacher 2014](https://jo.dreggn.org/home/2014_polynomial_optics.pdf)). Express the entire lens transfer (in₄ → out₄ in 4D ray space) as a degree-3 or degree-5 polynomial fit, evaluated in tens of nanoseconds. Hanika+Dachsbacher '14 add aperture-importance sampling and **BDPT compatibility** — exactly the use case that matters for RISE.
- **Ray-Transfer Functions** ([Goossens, Wandell et al. 2022](https://stanford.edu/~wandell/data/papers/2022-Goossens-RTF.pdf)). Polynomial / black-box surrogate fitted from a Zemax model; the optical house ships the surrogate without revealing the prescription.
- **Pre-baked PSF / IRF tables** by field position and depth — common for look-development cameras.

Lentil-for-Arnold is the canonical production deployment.

### 2.5 Tier 5 — Neural / data-driven cameras

Treat the lens as a learned function from input ray to output ray (or pixel).

- **NeuroLens** ([Zheng et al. 2017](https://quan-zheng.github.io/publication/NeuroLens-paper.pdf)). Small MLP per lens, trained from ray pairs.
- **Neural Lens Modeling** ([Xian et al. CVPR 2023](https://neural-lens.github.io/)). **Invertible** networks for distortion + vignetting, calibrated from real photographs of a calibration target. Invertibility matters because we need the inverse for importance-sampling the lens given a sensor location.
- **LensNeRF** ([Kim et al. WACV 2024](https://openaccess.thecvf.com/content/WACV2024/papers/Kim_LensNeRF_Rethinking_Volume_Rendering_Based_on_Thin-Lens_Camera_Model_WACV_2024_paper.pdf)). Thin-lens integrated into a NeRF integrator.
- 2024–25 commercial side: Reelmind's "LensDNA" (200+ measured cinema lenses). Mostly post-process today; direction of travel is *measured* lenses rather than *prescribed* ones.

Interesting; not yet first-class production. Big appeal is matching a *specific real lens you can hold* — useful for VFX matchmove integration, less useful for a path tracer's default look.

### 2.6 Tier 6 — Specialty / non-rectilinear cameras

- **Omnidirectional Stereo (ODS)** — the right way to render 360° stereo for VR (YouTube / Cardboard / Quest format). Per ray, offset the eye position around a small interpupillary circle indexed by the longitude of the outgoing direction. Reference: [Google Jump ODS spec](https://developers.google.com/vr/jump/rendering-ods-content.pdf).
- **Equirectangular / cubemap / dome (fulldome planetarium).** Trivial extensions of fisheye, but worth being first-class outputs rather than post-process warps.
- **Light-field / plenoptic camera.** Microlens array in front of the sensor; output is a 4D radiance slab from which you can post-refocus.
- **Pushbroom / linear-sensor / rolling-shutter.** Each scanline has its own time/pose. Critical for synthetic data targeting satellites, drones, or CMOS phones — all rolling-shutter.
- **Catadioptric / mirror-based**, **time-of-flight / event simulators** — niche.

### 2.7 Tier 7 — Sensor / shutter (parallel concern, not a "camera")

A "physical camera" in production usually means lens + sensor + shutter together. Most of this is sensor-side:

- **Spectral sensor response** (camera-specific RGB SPDs, not a colour-matching matrix). Pairs naturally with RISE's spectral nm path.
- **Non-rectangular shutter** (rolling shutter; angled mechanical shutter; weighted entry/exit shapes for cinematic motion blur).
- **Real exposure pipeline** (ISO, dual-gain, read noise, dark current, Bayer + demosaic).
- **Pixel sampling / box-vs-Gaussian sensor footprint.**

The only way to actually *match* a reference photograph is to model these too.

---

## 3. RISE-specific scoring

Three axes:

1. **Visible-quality jump.** How much more cinematic / physically convincing is the output?
2. **Pipeline fit.** Does it leverage RISE's spectral NM / BDPT / VCM, or fight them?
3. **Independence.** Is it standalone, or does it require GPU / neural infrastructure RISE doesn't have?

| Tier / item | Quality jump | Pipeline fit | Independence | Score |
|---|---|---|---|---|
| Real multi-element lens (Tier 2) | Very high | Excellent — spectral CA falls out | Standalone | **1** |
| Thin-lens enrichments (Tier 1) | Medium | Excellent | Standalone | **2** |
| Polynomial-optics acceleration (Tier 4) | None directly — perf only | Excellent for BDPT/VCM | Standalone | **3** (after Tier 2) |
| ODS / equirect / cubemap / dome (Tier 6 a–b) | Medium for VR/dome workflows | Neutral | Standalone | **4** |
| Aperture diffraction & lens flare (Tier 3) | High at certain shots | Improved by spectral | Standalone | **5** |
| Sensor / shutter model (Tier 7) | High for matchmove / synthetic data | Excellent — spectral SPD | Standalone | **6** |
| Neural cameras (Tier 5) | High *if* you have the calibration data | Mismatch — needs differentiable pipeline / GPU | Heavy | **7** (defer) |
| Light-field / plenoptic / pushbroom (Tier 6 c–d) | Niche | Neutral | Standalone | **8** (defer) |

---

## 4. Constraints that bound the design

Read before any phase below. These are RISE-specific landmines.

- **`ICamera` vtable is frozen.** Adding a virtual breaks out-of-tree camera plugins compiled against the old interface. Pattern: any new method (e.g., camera importance `We()`, exit-pupil sampler, lens-sample injector) lives as a non-virtual class method, and call sites use `dynamic_cast<NewCamera*>(cam)` to opt in. See the existing `ThinLensCamera::GenerateRayWithLensSample` precedent. Also consult [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md) before each phase.
- **Descriptor-driven parser.** Adding a camera = new `IAsciiChunkParser` subclass with `Describe()` + `Finalize()`, register in `CreateAllChunkParsers()`. Adding a parameter to an existing camera = one descriptor entry + one `bag.GetX(...)`. The descriptor *is* the accepted parameter set — syntax-highlighter / suggestion-engine drift is structurally impossible.
- **Five build-project files when adding `.cpp` / `.h`.** [CLAUDE.md](../CLAUDE.md) lists them: `build/make/rise/Filelist`, `build/cmake/rise-android/rise_sources.cmake`, `build/VS2022/Library/Library.vcxproj`, `Library.vcxproj.filters`, `build/XCode/rise/rise.xcodeproj/project.pbxproj`. None auto-discovers.
- **MLT requires `GenerateRayWithLensSample`.** Any camera with an aperture must expose a non-virtual lens-sample-taking primary-ray generator so PSSMLT mutations stay continuous on the aperture coordinate. PT/BDPT/VCM/photon paths can use the random-context version.
- **BDPT/VCM connect through the camera.** A multi-element lens needs a real importance-evaluation path: given a world-space point, can the camera see it, with what `We` and what raster coords? Thin-lens currently relies on the analytic disk pdf; multi-element needs an exit-pupil sample or a polynomial-optics inverse.
- **Spectral rasterizer chunks already exist.** Per-wavelength refraction integrates naturally — add a wavelength-aware `GenerateRay` overload (non-virtual; opt-in) so the spectral integrator threads λ into the lens trace.
- **Compiler warnings are bugs.** Clean rebuild on both `make` and the Xcode `RISE-GUI` target before calling a phase done. `-Wno-*` and `#pragma`-suppression are not acceptable; fix the root cause. (CLAUDE.md "Compiler Warnings Are Bugs".)

---

## 5. Roadmap

Phases are ordered by ROI under the constraints above. Each phase is independently shippable: you can stop after any one and still have something useful.

### Phase 0 — Reclaim the `realistic_camera` keyword

**Status:** No code action needed today. The keyword is effectively free.

**Discovery.** [AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) §`RealisticCameraAsciiChunkParser` registers `realistic_camera` and delegates to `SetThinlensCamera`. Only **one** in-tree scene uses it — [scenes/Tests/Cameras/realistic.RISEscene](../scenes/Tests/Cameras/realistic.RISEscene), a demo showing the photographer-friendly parameter form. No production scenes, no user-facing API contract. The keyword is ours to redefine.

**Plan.** Repurpose `realistic_camera` for the **real** multi-element lens camera in Phase 4. Until Phase 4 ships, the existing stub stays as-is (no point thrashing it). When Phase 4 lands:

1. `realistic_camera` becomes the multi-element lens chunk, parameterised by `lens_file` plus residual photographic params that still make physical sense for a real lens (`film_size` → sensor diagonal, `fstop` → working aperture, `focus_distance` → focus, `focal_length` is *no longer* an input — it's a property of the lens prescription and is rejected with a clear error if specified).
2. The lone test scene migrates to `thinlens_camera` (trivial — same parameters, just translated).
3. If we later want to keep the photographer-friendly thin-lens *as its own thing*, add a separate `photographic_camera` chunk. That's optional; today's `thinlens_camera` already covers the use case.

**Acceptance.** No work this phase beyond noting the decision. Track this section in Phase 4's checklist as the migration step that gates `realistic_camera`'s new semantics.

---

### Phase 1.0 — Thin-lens parameter overhaul + polygonal/anamorphic aperture *(LANDED)*

**Goal.** Replace the over-specified, scene-unit `(fov, aperture_size, focal_length, focus_distance)` quartet with the photographic quartet `(sensor_size, focal_length, fstop, focus_distance)` matching every production renderer (Cycles, Arnold, V-Ray, RenderMan, Karma). Same change folds in polygonal aperture + anamorphic squeeze — the only Phase-1.1 enrichments that share the constructor signature, avoiding a second ABI break. Plan: [/Users/aravind/.claude/plans/cozy-snacking-sunrise.md](/Users/aravind/.claude/plans/cozy-snacking-sunrise.md).

**New parameter set** (defaults in parentheses): `sensor_size` (36, full-frame mm-equivalent), `focal_length` (35, "natural"), `fstop` (2.8), `focus_distance` (**required, no default**), `aperture_blades` (0 = disk), `aperture_rotation` (0°), `anamorphic_squeeze` (1.0). FOV is derived; aperture diameter is derived. The `realistic_camera` keyword is now reserved for Phase 4 (multi-element lens) — its photographer-friendly stub semantics moved into the new `thinlens_camera`. Sensor-format presets table is documented in [src/Library/Parsers/README.md](../src/Library/Parsers/README.md).

**Unit story** (corrected, post-review). All three lengths — `sensor_size`, `focal_length`, `focus_distance` — must be in the **same unit as scene geometry**. The FOV formula is unit-free (the sensor/focal ratio is all that matters), but the lens equation `v = f·u/(u−f)` requires unit consistency. The presets are mm-equivalent (matching the photographic convention) — scenes in mm use them directly; scenes in metres scale by 1/1000 (e.g. `focal_length 0.035` for a 35mm lens), etc. The parser enforces `focus_distance > focal_length` (otherwise the math gives a negative film distance) with an error message that calls out the unit-matching trap directly.

**Polygonal aperture sampler** uses inverse-CDF on `sec²(θ_rel)` so the joint Cartesian density is exactly uniform on the regular n-gon (corner-vs-edge density bias would otherwise be `cos²(π/n)` ≈ 25% for 6 blades). The closed-form mapping:

```
uxIn = uv.x · n − floor(uv.x · n)
θ_rel = atan( (2·uxIn − 1) · tan(π/n) )
θ = (k + 0.5) · 2π/n + θ_rel
r = sqrt(uv.y) · halfAperture · cos(π/n) / cos(θ_rel)
```

The mapping is C0 across blade seams (proven algebraically; verified numerically — cross-seam jumps O(ulp), in-blade jumps the same order), so PSSMLT continuity is preserved. `apertureBlades < 3` falls through to `PointOnDisk` → bit-identical to the pre-overhaul disk path.

**Files touched.**
- [src/Library/Cameras/ThinLensCamera.h,cpp](../src/Library/Cameras/) — new constructor, new member fields, polygonal+anamorphic aperture sampler with inverse-CDF radial bias correction, load-bearing setter contract docs (caller must call `RegenerateData()` after a Set*; matches the existing `CameraCommon` pattern), updated keyframe handling.
- [src/Library/RISE_API.h,cpp](../src/Library/) — `RISE_API_CreateThinlensCamera` re-signed.
- [src/Library/Interfaces/IJob.h](../src/Library/Interfaces/IJob.h) + [src/Library/Job.h,cpp](../src/Library/) — `SetThinlensCamera` virtual + impl re-signed.
- [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) — `ThinlensCameraAsciiChunkParser` rewritten; `RealisticCameraAsciiChunkParser` removed; `realistic_camera` no longer registered (descriptor-driven validation auto-rejects retired `fov` / `aperture_size`).
- [src/Library/Parsers/README.md](../src/Library/Parsers/README.md) — sensor-format preset table, chunk-count + AddCameraCommonParams reference updated to "5 cameras".
- [src/Library/SceneEditor/CameraIntrospection.cpp](../src/Library/SceneEditor/CameraIntrospection.cpp) — properties-panel get/set wired to new params (sensor_size, fstop, aperture_blades, aperture_rotation, anamorphic_squeeze; `fov` row dropped for ThinLens since it's derived).
- [build/XCode/rise/RISE-GUI/App/PropertiesPanel.swift](../build/XCode/rise/RISE-GUI/App/PropertiesPanel.swift) — Mac UI: `aperture_rotation` added to the angular-field list (0.5°/px scrub rate). Other new params surface automatically because the panel iterates the descriptor.
- [build/VS2022/RISE-GUI/ViewportProperties.cpp](../build/VS2022/RISE-GUI/ViewportProperties.cpp) — Windows UI: same one-line angular-field-list update.
- 7 scene migrations (6 `thinlens_camera` + 1 `realistic_camera` → `thinlens_camera`).
- [scenes/Tests/Parser/thinlens_retired_keys.RISEscene](../scenes/Tests/Parser/thinlens_retired_keys.RISEscene) — negative regression scene exercising the descriptor-rejection path for the retired keys.

**ABI.** Hard break by design (no backwards-compat constraint). `SetThinlensCamera` signature replaced outright; out-of-tree implementors of `IJob` must update. Single in-tree caller is the parser; no other callers in `src/`, `tests/`, or `bindings/`.

**Acceptance (Phase 1.0) — verified.**
- Visual regression: math equivalence proven analytically (max FOV drift across the 7 migrated scenes 0.089°, halfAperture exact for 5/6 — well below sample noise). End-to-end smoke renders confirm both `realistic.RISEscene` and `thinlens.RISEscene` produce plausible output through the new camera + the side-task rasterizer migration.
- Parser-error path: descriptor-driven validation rejects retired `fov`/`aperture_size` with `Failed to parse parameter name 'fov' (not declared in 'thinlens_camera' descriptor)`. Regression scene at [scenes/Tests/Parser/thinlens_retired_keys.RISEscene](../scenes/Tests/Parser/thinlens_retired_keys.RISEscene).
- Build: `make -C build/make/rise -j8 all` and `xcodebuild -scheme RISE-GUI` clean rebuilds, both warning-free.
- Tests: 72/72 pass after the change.
- MLT continuity: provable (inverse-CDF mapping is C0 by construction; numerically validated cross-seam jumps O(ulp)).
- Polygonal-aperture density: validated numerically — old code under-sampled corner regions by 7.28% (theoretical 25% peak density bias), new code is uniform within MC noise (+0.04% over 2M samples).
- Adversarial review (3 reviewers, parallel, orthogonal concerns): zero CRITICAL findings post-fix; one MAJOR (the n-gon density bias) addressed in-scope; remaining MAJORs are pre-existing (mixed-unit `focal >= focus` check) or expected (ABI break per user mandate).

---

### Phase 1.2 — Unit conventions: scene-level scale + mm-input camera lens *(LANDED)*

**Goal.** Fix the mm-vs-scene-units ambiguity that surfaced during the Phase 1.1 review and demos. Settle the contract: scene-internal default = metres, camera lens specs typed in mm, scene-level `scene_options { scene_unit ... }` factor bridges the two for non-metre scenes. Match Cycles / Arnold / V-Ray practice.

**What changed.**
- New `scene_options` chunk with a `scene_unit` parameter (meters per scene unit; default 1.0). Stored in thread_local parser state; reset per parse via `ChunkParsers::ClearParseState()`. Future physical-quantity work (volumetric atmosphere, sky, sensor noise) consumes the same scale.
- `thinlens_camera` lens parameters (`sensor_size`, `focal_length`, `shift_x`, `shift_y`) are now MM in the scene file and the editor — photographic convention. `focus_distance` stays in scene units (matches geometry coords).
- Camera storage refactor: `ThinLensCamera` keeps the mm values as source-of-truth, plus a `sceneUnitMeters` member, and converts mm → scene-units inside `Recompute()` via `mm_to_scene = 0.001 / sceneUnitMeters`. The lens-equation hot path operates entirely in scene units; the per-ray cost is unchanged from Phase 1.1 (one extra multiply on shift, folded into the cached `shiftX_sceneUnits` / `shiftY_sceneUnits` computed once per Recompute).
- `IJob::SetThinlensCamera` and `RISE_API_CreateThinlensCamera` gain a `sceneUnitMeters` parameter (third-from-the-end). Editor introspection passes mm values through directly — no new conversion code in the editor surface, since storage *is* mm now.
- `focus_distance > focal_length` validation now compares both in scene units (parser converts focal mm → scene units before the check), with a unit-aware error message.

**Files touched.**
- [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp): new `SceneOptionsAsciiChunkParser`, `s_sceneOptions` thread-local state, `thinlens_camera` reads scene_unit and passes through. Descriptor texts updated to say "MILLIMETRES".
- [src/Library/Parsers/ChunkDescriptor.h](../src/Library/Parsers/ChunkDescriptor.h): no schema change (presets already there from 1.1).
- [src/Library/Cameras/ThinLensCamera.h,cpp](../src/Library/Cameras/): `sensorSize`/`focalLength`/`shiftX`/`shiftY` semantics changed from "scene units" to "mm"; new `sceneUnitMeters` member + `GetSceneUnitMeters/SetSceneUnitMeters`; `Recompute` does mm→scene conversion; cached `shiftX_sceneUnits/shiftY_sceneUnits` for the hot path.
- [src/Library/RISE_API.h,cpp](../src/Library/), [src/Library/Interfaces/IJob.h](../src/Library/Interfaces/IJob.h), [src/Library/Job.h,cpp](../src/Library/): added `sceneUnitMeters` parameter to the thinlens entry points.
- [src/Library/Parsers/README.md](../src/Library/Parsers/README.md): replaced the "scene units" unit story with the mm + scene_unit story; documented the conversion table; added a `scene_options` section.
- 8 existing scenes get `scene_options { scene_unit 0.001 }` (mm-scale) at the top to preserve visual output without numeric changes to lens values.
- [scenes/Tests/Cameras/thinlens_tiltshift.RISEscene](../scenes/Tests/Cameras/thinlens_tiltshift.RISEscene) redesigned at metres-scale (camera 4 m back, 30 cm spheres, 35 mm lens, focus 3 m, tilt_y 25°) — addresses the "doesn't render anything" report.
- [tests/CameraUnitConversionTest.cpp](../tests/CameraUnitConversionTest.cpp) new — verifies stored fields stay mm, focus stays scene-units, FOV is unit-invariant, and rays from "metres camera" vs "mm camera with positions ×1000" differ exactly by the scale factor in origin and are identical in direction.
- [tests/SceneEditorSuggestionsTest.cpp](../tests/SceneEditorSuggestionsTest.cpp) chunk-count assertion bumped 130 → 131 (covers `scene_options`).

**ABI.** Additive on `SetThinlensCamera`/`RISE_API_CreateThinlensCamera` (one new param). Camera storage semantics changed: existing scenes that interpreted `focal_length` as "scene units" break unless they declare a matching `scene_options { scene_unit }` block. All in-tree scenes have been migrated; out-of-tree consumers must declare a scene_unit if their geometry isn't metres.

**Phase 1.2.1 follow-up — UI unit labels.** After Phase 1.2 landed, a real-user review surfaced that even though the panel data path is in mm, the field labels don't surface the unit at all — so a user looking at "focal_length 35" can't tell at a glance whether it's 35 mm, 35 cm, or 35 m. Fixed by adding a `unitLabel` field to `ParameterDescriptor` (e.g. "mm", "°", "scene units"), plumbed through `CameraProperty` → `SceneEditController::PropertyUnitLabel` → `RISE_API_SceneEditController_PropertyUnitLabel` → both bridges → both panels render the suffix next to the value field. Camera params now surface labels: lens lengths "mm", angles "°", focus_distance "scene units", dimensionless params (fstop, anamorphic_squeeze) leave the suffix empty. New `TestPanelUnitLabels` regression test asserts every camera parameter exposes its expected unit label.

**Acceptance — verified.**
- 216 / 216 assertions pass in `CameraUnitConversionTest` (10 test groups: stored mm, focus_distance-is-scene-units, FOV unit-invariance, ray scale-invariance, non-zero shift unit-invariance, tilt cache unit-invariance, **panel unit labels**, panel-shows-mm-not-metres across 5 setups, preset-pick roundtrip in mm, focal-length-setter roundtrip in mm, editor SetX is mm-direct).
- All 8 migrated existing scenes parse and render unchanged from Phase 1.1.
- New tilt-shift demo scene renders correctly (50% non-zero coverage; Scheimpflug effect visible — focal plane rotated by tilt_y catches the diagonal sphere row).
- Build: `make -C build/make/rise -j8 all` clean rebuild, zero warnings.
- 71/72 tests pass; the one failure is the pre-existing `GLTFLoaderTest` flake, filed separately.

---

### Phase 1.1 — Tilt-shift + scene-level defaults + UI sensor presets *(LANDED)*

**Goal.** Finish the thin-lens story Phase 1.0 started: add Scheimpflug tilt + architectural shift, surface the photographic sensor-format presets in the editor as a combo box, and let scenes declare reusable camera defaults at the top of the file. Skipped from the original 1.1 list per the recommendation: `optical_vignetting` (becomes redundant once Phase 4 lands real multi-element cat's-eye for free); `aperture_texture` (deferred — niche, rolls into a future enrichment phase if any user asks).

**Tilt-shift.**
- New params on `thinlens_camera`: `tilt_x`, `tilt_y` (degrees, default 0) and `shift_x`, `shift_y` (scene units, default 0). All four default to 0, which gives a plain perpendicular-focus thin-lens — bit-identical output to Phase 1.0 for unchanged scenes.
- Implementation generalises the focus-plane intersection. The focal plane is parameterised as `n · P = kFocus`; for tilt = (0,0) this collapses to `n = (0,0,1)`, `kFocus = focusDistance` — the original perpendicular formula. With non-zero tilt the chief ray from a sensor sample through the lens center intersects a tilted focal plane, giving the Scheimpflug "wedge of focus" used for miniature-fake / selective-focus.
- Shift translates the image-plane sample point. Convention: positive `shift_y` is "lens up" (image content moves down — standard architectural correction); positive `shift_x` is "lens left" (image content moves right). Sign choices documented in the descriptor parameter help-text and parser README.
- Math derivation, equivalence-with-Phase-1.0 proof, and PSSMLT continuity argument all live in [src/Library/Cameras/ThinLensCamera.cpp](../src/Library/Cameras/ThinLensCamera.cpp) `Recompute()` block comment.
- Reference: Kensler, *Tilt-Shift Rendering Using a Thin Lens Model*, Ray Tracing Gems II ch. 31.

**Scene-level `camera_defaults` chunk.**
- New top-level chunk that sets fallback values for `sensor_size`, `focal_length`, and `fstop`. `thinlens_camera` consults these when those params are omitted on the camera itself. Order-dependent: `camera_defaults` must precede the camera chunks that consume it (same convention as `standard_shader`). Cameras with explicit values override.
- Backed by a `thread_local` static `s_cameraDefaults` in the parser's anonymous namespace, reset at the start of each `ParseAndLoadScene` via `ChunkParsers::ClearParseState()` — no global state leaks between scene loads.
- `focus_distance` is intentionally NOT in `camera_defaults` (it's shot-specific). `tilt`/`shift` are also excluded for the same reason.

**UI sensor-format combo box.**
- Extended `ParameterDescriptor` with a `presets: std::vector<ParameterPreset>` field (label/value pairs). Plumbed through CameraIntrospection → SceneEditController → C API → both bridges → both panels.
- Mac: SwiftUI `Menu` with the `list.bullet` SF Symbol next to the line edit; picking a preset writes its value via the same SetProperty path as keyboard editing (so undo/redo works identically).
- Windows: `QToolButton` with the `⋮` glyph and a popup `QMenu`, same dispatch pattern.
- Both panels render the combo box only when `presets` is non-empty — no churn for existing rows. The line edit stays usable for arbitrary custom values.
- Sensor-format preset list (12 formats, full-frame to 8×10) baked into the `sensor_size` descriptor in `thinlens_camera` and `camera_defaults`. Parser README cross-links.

**Files touched.**
- [src/Library/Cameras/ThinLensCamera.h,cpp](../src/Library/Cameras/) — tilt/shift members, focal-plane equation cache (`nFocusX/Y/Z`, `kFocus`), refactored `Recompute()` and `GenerateRay`/`GenerateRayWithLensSample` to use the general formulation.
- [src/Library/RISE_API.h,cpp](../src/Library/), [src/Library/Interfaces/IJob.h](../src/Library/Interfaces/IJob.h), [src/Library/Job.h,cpp](../src/Library/) — extended `RISE_API_CreateThinlensCamera` / `IJob::SetThinlensCamera` with 4 new tilt-shift params.
- [src/Library/Parsers/ChunkDescriptor.h](../src/Library/Parsers/ChunkDescriptor.h) — new `ParameterPreset` struct and `presets` field on `ParameterDescriptor`.
- [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) — `CameraDefaultsAsciiChunkParser` (new), tilt-shift params on `ThinlensCameraAsciiChunkParser`, sensor-size presets baked into both descriptors, parser-state reset.
- [src/Library/Parsers/README.md](../src/Library/Parsers/README.md) — new sections for `camera_defaults` and tilt-shift; preset table cross-links to the descriptor.
- [src/Library/SceneEditor/CameraIntrospection.h,cpp](../src/Library/SceneEditor/) — `CameraProperty.presets` field, GET/SET wiring for tilt/shift in radians/degrees with the standard editor unit conversions.
- [src/Library/SceneEditor/SceneEditController.h,cpp](../src/Library/SceneEditor/) — `PropertyPresetCount/Label/Value` accessors.
- [src/Library/RISE_API.h,cpp](../src/Library/) — `RISE_API_SceneEditController_PropertyPreset*` C accessors.
- [build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h,mm](../build/XCode/rise/RISE-GUI/Bridge/) — `RISEViewportPropertyPreset` Obj-C class + propertySnapshot population.
- [build/XCode/rise/RISE-GUI/App/PropertiesPanel.swift](../build/XCode/rise/RISE-GUI/App/PropertiesPanel.swift) — `PropertyPreset` Swift struct + Menu rendering when presets are non-empty + `tilt_x`/`tilt_y` added to angular-field list.
- [build/VS2022/RISE-GUI/ViewportBridge.h,cpp](../build/VS2022/RISE-GUI/) — `ViewportPropertyPreset` struct + propertySnapshot population.
- [build/VS2022/RISE-GUI/ViewportProperties.cpp](../build/VS2022/RISE-GUI/ViewportProperties.cpp) — QToolButton + QMenu rendering when presets are non-empty + `tilt_x`/`tilt_y` added to angular-field list.
- [scenes/Tests/Cameras/thinlens_tiltshift.RISEscene](../scenes/Tests/Cameras/thinlens_tiltshift.RISEscene) — regression scene with a row of spheres at varying depth and `tilt_y=8` rotating the focal plane.
- [scenes/Tests/Cameras/camera_defaults.RISEscene](../scenes/Tests/Cameras/camera_defaults.RISEscene) — smoke test for the scene-level fallback path.
- [tests/SceneEditorSuggestionsTest.cpp](../tests/SceneEditorSuggestionsTest.cpp) — chunk-count assertion bumped 128 → 130 (covers `camera_defaults` and the previously-untracked `gltfmesh_geometry`).

**ABI.** Additive-only on the descriptor and editor surfaces (new field on `ParameterDescriptor`; new methods on `SceneEditController` and the C API). `SetThinlensCamera` and `RISE_API_CreateThinlensCamera` extended with 4 new params at the end — same break-pattern as Phase 1.0, no out-of-tree callers found.

**Deferred from this phase.**
- `optical_vignetting` — superseded by Phase 4's real multi-element cat's-eye.
- `aperture_texture` — niche; rolls into a future enrichment if requested.

**Acceptance (Phase 1.1) — verified.**
- Tilt-shift fast-path with default zeros is bit-identical to Phase 1.0 (mathematically — the new `focus = oneMinusT · p_sensor` formula reduces exactly to the old `f_over_d_minus_f` precompute). All 7 migrated scenes from Phase 1.0 still parse and render unchanged.
- Tilt-shift regression scene renders successfully (visible spheres in focus along the tilted plane).
- `camera_defaults` test scene renders successfully through the fallback path (omits sensor/focal/fstop on the camera; inherits from defaults).
- Parser-error path still rejects retired `fov`/`aperture_size`; new params are accepted and validated.
- **Tilt-magnitude validation:** `|tilt_x|` and `|tilt_y|` capped at 80° at parse time with a focused error message. Beyond that, the chief-ray formula `kFocus / (n·p_sensor)` would divide by near-zero and produce NaN frames; cap leaves enough headroom for stylized renders (real lenses max around ±15°).
- **Shift sign convention follows Blender Cycles:** positive `shift_x` = lens-right (camera looks right, content moves left); positive `shift_y` = lens-up (camera looks up, content moves down — the standard architectural-correction direction). Both implemented as `x_img = x_pix·sx − shiftX` and `y_img = y_pix·sy − shiftY`. Verified analytically against the camera-frame basis: in RISE the camera-frame "right" maps to world `−U` because `U = up × forward` flips sign; subtracting shift compensates so positive lens-right gives the camera-intuitive direction.
- Both GUI panels surface the new params automatically (descriptor-driven). Sensor-size presets render as a Menu/QToolButton next to the line edit; `tilt_x`/`tilt_y` get the angular scrub-rate. Preset bridge buffers sized to 256 bytes (was 64/128) so future longer labels won't truncate; Mac bridge defensively skips entries whose UTF-8 round-trip yields nil.
- Build: `make -C build/make/rise -j8 all` and `xcodebuild -scheme RISE-GUI` clean rebuilds, zero warnings. Tests: 71/72 pass; the one failure is a pre-existing `GLTFLoaderTest` flake on master (filed separately).
- Adversarial review (3 reviewers, parallel, orthogonal concerns): zero CRITICAL findings; three MAJORs addressed in this round (shift sign convention, tilt validation, preset buffer hardening). Pre-existing items flagged for future work: nested `> load` wipes parser state mid-parse (affects `s_painterColors` too — not Phase-1.1-specific); `mProperties` snapshot vs render-thread race (pre-existing, applies to all properties).

---

### Phase 2 — Output-format cameras (equirect / cubemap / dome / lat-long)

**Goal.** First-class non-rectilinear cameras for HDRI capture, dome installations, and 360° pre-renders. Each is a small standalone class.

**Scope.** Four new chunks:
- `equirectangular_camera` — lat-long projection over the full sphere (or capped by `min_theta` / `max_theta`).
- `cubemap_camera` — six perspective faces dispatched per pixel; output writes to a `+x −x +y −y +z −z` strip or 3:2 cross.
- `dome_camera` — angular fisheye (180° / 220° / configurable) for fulldome planetarium output.
- `lat_long_camera` — equivalent to equirectangular; alias for clarity in HDRI workflows.

**Files touched.**
- `src/Library/Cameras/EquirectangularCamera.{h,cpp}`, `CubemapCamera.{h,cpp}`, `DomeCamera.{h,cpp}` (new).
- `src/Library/Parsers/AsciiSceneParser.cpp` — four new chunk parsers.
- `src/Library/RISE_API.h` + `Job.cpp` — four new `SetXxxCamera` exports.
- All five build-project files.

**ABI.** Additive. New camera classes derive from `CameraCommon`. No vtable change.

**Acceptance.**
- Round-trip test: render a known HDRI environment with `equirectangular_camera`, sample it back as a `radiance_background`, render with `pinhole_camera`. The pinhole render should match a baseline rendered without the round-trip within a small RMSE.
- Dome renders sanity-checked against a regular fisheye for the central 180° patch.

---

### Phase 3 — Omnidirectional Stereo (ODS)

**Goal.** Per-eye 360° stereo for VR delivery. Standard format for YouTube / Quest / Cardboard 360°-stereo content.

**Scope.** A single `ods_camera` chunk that takes `ipd` (interpupillary distance), `eye` (left/right/stacked-pair), and the equirect parameters. Implementation: per outgoing direction, offset the ray origin by `±ipd/2` along the axis perpendicular to (look-direction, up-direction) projected for that longitude. The reference is the Google Jump spec.

**Files touched.** `src/Library/Cameras/ODSCamera.{h,cpp}` + parser + RISE_API + 5 build files. `eye=stacked` writes a doubled-height image with left-on-top / right-on-bottom — the YouTube 360° stereo convention.

**ABI.** Additive.

**Acceptance.** Render a simple scene (sphere of textured cubes around the camera) at small IPD and verify left/right disparity matches geometry expectations — front-of-camera objects should have larger disparity than peripheral, and disparity should fall to zero at the poles.

---

### Phase 4 — Realistic multi-element lens camera (the big one)

**Goal.** Real lens-system simulation per Kolb '95 / PBRT v3 §6.4 — multi-surface refraction, exit-pupil sampling, spectral chromatic aberration. Activates the spectral pipeline's most photogenic capability. Two to three weeks of careful work, gated on Phase 1's clean photographic-param foundation; the doc earns its keep here.

**Headline parameter set.** `lens_file` (required filename, points at a `.dat` lens prescription), `sensor_size` (mm, default 36), `fstop` (default 4.0; clamps the iris but cannot exceed the lens's open aperture — error if asked to), `focus_distance` (scene units, required), `aperture_blades` / `aperture_rotation` / `anamorphic_squeeze` (carried over from `thinlens_camera` for shaped-bokeh control on the iris), `wavelength_resolved` (Bool, default `true`; debug toggle that forces d-line evaluation when `false`). `focal_length` is **rejected with an explicit error** — the lens prescription determines focal length, not the user.

#### Architectural decisions (resolved)

1. **Lens prescription file format — PBRT-strict.** Lens `.dat` files are loaded verbatim in PBRT's `radius / thickness / IOR(d) / semi_aperture` column format. No header magic, no extension. Files written for PBRT v3/v4 work in RISE without modification.
2. **Per-surface dispersion via sidecar `.glass` files.** PBRT files only carry scalar IOR (typically d-line). To get chromatic aberration we need per-surface glass info. Solution: each shipped `.dat` lens has a parallel `.glass` sidecar with the same basename (`dgauss-50mm.dat` ↔ `dgauss-50mm.glass`) listing per-surface glass type. Two row formats supported in the sidecar:
   - `surface <i> glass <NAME>` — resolves through `data/lenses/glass_catalog.txt` to Sellmeier coefficients (preferred for catalogued glass).
   - `surface <i> n_d <Nd> V_d <Vd>` — falls back to 3-term Cauchy synthesized from the (n_d, V_d) pair (used for unidentified PBRT glasses).
   When a `.glass` sidecar is absent, scalar IOR from the `.dat` is used at every wavelength → no CA. Same effective behaviour as PBRT v3/v4. The sidecar is *additive*; it never modifies the lens's geometric prescription.
3. **Cauchy synthesis from (n_d, V_d).** For glass not in the Schott catalog, the sidecar's `n_d` / `V_d` pair drives a 3-term Cauchy `n(λ) = A + B/λ² + C/λ⁴` fit through the (F-line, d-line, C-line) anchors. Approximate but defensible — within ~1e-4 of Sellmeier across the visible band for typical optical glass. PBRT's distribution lenses use enough unidentified glasses that this fallback is load-bearing.
4. **Class split — thin `RealisticCamera` over reusable `LensSystem`.** `LensSystem` is data-only plus stateless `TraceFromSensor` / `TraceFromWorld` methods, reusable by Phases 5 (polynomial fit), 6 (lens-flare ghost paths), 8 (neural lenses). `RealisticCamera` (derives `CameraCommon`) owns a `LensSystem` plus the sensor placement, the exit-pupil cache, and the photographic params.
5. **Aspheric surfaces — defer.** None of the four shipped lenses use them. The `LensSurface` interface is designed so an `EvenAsphereSurface` can subclass in without API changes when a future lens needs them.
6. **Exit-pupil cache key — radial only, with a measurement-driven escalation path.** Same as PBRT v3 §6.4.5: one bounding disc per sensor-radius bin (default 64 bins). λ-keying deferred. Phase 4.2/4.3 instruments rejection rates at λ=400 nm and λ=750 nm; if either exceeds 30% (vs ~5% expected at d-line) we escalate to a `(radius, λ)`-keyed cache.
7. **`GenerateRaySpectral` — non-virtual on `RealisticCamera`, dispatched via `dynamic_cast`.** Same pattern as `ThinLensCamera::GenerateRayWithLensSample`. Adding a virtual to `ICamera` would break new-caller → old-impl on out-of-tree camera implementors (the appended vtable slot doesn't exist in the old vtable). New helper `GenerateCameraRaySpectral(camera, rc, ray, screen, lambda_nm)` lives in `src/Library/Rendering/CameraRayDispatch.h` and downcasts to `RealisticCamera*`; for every other camera it falls through to `camera.GenerateRay(...)` — bit-identical to today.
8. **Wavelength threading — per-sample, not per-path.** The current spectral rasterizers call `camera.GenerateRay()` *outside* the per-λ loop. Phase 4.3 moves it *inside*. For HWSS (4-wavelength hero bundles), Phase 4.3 *measures* the cost of re-tracing the camera ray per λ vs sharing the hero ray; the default ships per-λ but switches to shared-hero if the CA scene RMSE difference is < 0.5%.
9. **Camera importance for BDPT/VCM — backwards-trace with a chief-ray cache.** Roadmap option (a) — geometric backwards-trace from the world point through the lens to the sensor. Phase 5 polynomial fit was originally proposed as the cheap alternative; we make it *optional* by caching the chief-ray cone per pupil-cache bucket so the Newton iteration starts close to the answer (typical convergence 2–3 iterations vs 6–8 cold). Phase 5 escalation triggers if Phase 4.4's BDPT wall-clock exceeds 2.5× thin-lens BDPT on the bench scene.
10. **`wavelength_resolved` debug toggle — shipped.** Setting `wavelength_resolved false` forces all surfaces to use the d-line IOR regardless of λ. Debugging only — A/B comparison to confirm CA is what's driving an observed render artefact. May remove after one release if no users exercise it.
11. **Aperture-stop iris clamp.** The lens's open aperture defines the *minimum* fstop achievable. `fstop` smaller than that → loud parser error. `fstop` larger → clamp the iris by reducing the aperture-stop's effective semi-aperture (the photographer-expected behaviour; matches stopping a real lens down).
12. **Editor `lens_file` reload — apply on blur.** Changing `lens_file` triggers a full exit-pupil cache rebuild (1–2 s). Both the Mac and Windows panels apply text changes to `Filename`-typed parameters on focus blur, never on keystroke. Other parameter types stay on their existing apply policy.

#### Sub-phase ordering (six PR-sized commits)

| # | Scope | Commit-size |
|---|---|---|
| **4.0** | Glass catalog + lens prescription parser + sidecar parser + `data/lenses/*.{dat,glass}` corpus + `tests/GlassDispersionTest.cpp` + `tests/LensPrescriptionParserTest.cpp`. **No camera or geometric tracer yet.** Pure-data PR; gets the corpus into the tree and validates Sellmeier numerics independently of geometry. | small |
| **4.1** | `LensSurface` + `LensSystem` ray-stack tracer + `tests/LensSystemRayTraceTest.cpp` (hand-verified single-surface and 2-surface analytic cases). **Test-only entry point — no parser or `RealisticCamera` yet.** | small |
| **4.2** | `ExitPupilCache` + `RealisticCamera` shell + `RealisticCameraAsciiChunkParser` + `IJob`/`Job`/`RISE_API` exports + `CameraIntrospection` + first end-to-end test scene + `tests/ExitPupilCacheConsistencyTest.cpp`. **All renderers consume `RealisticCamera` via the existing `ICamera::GenerateRay` virtual at d-line λ.** Spectral CA is *not* yet visible. | **largest** |
| **4.3** | `RealisticCamera::GenerateRaySpectral` + `Rendering/CameraRayDispatch.h` + the four spectral rasterizer touchpoints (PT/BDPT/MLT/VCM) + the HWSS hero-ray-vs-per-λ measurement + per-λ pupil-cache rejection-rate instrumentation. Adds the chromatic-aberration regression scene. **The CA payoff lands here.** | medium |
| **4.4** | `RealisticCamera` branches in `BDPTCameraUtilities::Rasterize` / `Importance` / `PdfDirection`. Chief-ray fast-path cache (decision 9). Adds the PT-vs-BDPT-vs-VCM consistency scene per `bdpt-vcm-mis-balance`. **Phase 5 trigger gate measures here.** | medium |
| **4.5** | Showcase scenes under `scenes/FeatureBased/Cameras/`, `data/lenses/README.md`, parser README update, adversarial review pass. | small |

#### Files touched

**New source files** (all in `src/Library/Cameras/` unless noted):
- `Glass.{h,cpp}` — Sellmeier / Cauchy IOR evaluation; static `GlassCatalog::Lookup(name) → GlassCoefficients` with ~20 Schott-cataloged glasses (N-BK7, N-BAK4, F2, SF6, SF11, LAK21, BAF10, etc.) baked in.
- `LensPrescriptionParser.{h,cpp}` — reads `.dat` (PBRT-verbatim) and `.glass` (sidecar) files; resolves glass names against `GlassCatalog`; errors locate line+column. The two parsers are independent: a `.dat`-only load yields scalar-IOR-everywhere behaviour, a `.dat` + sidecar load yields full per-surface dispersion.
- `LensSurface.{h,cpp}` — single spherical surface (radius, thickness, semi-aperture). One extension point for `EvenAsphereSurface` later.
- `LensSystem.{h,cpp}` — surface stack, forward + backward Snell trace, aperture-stop logic. Wavelength-agnostic at this layer — caller supplies IOR.
- `ExitPupilCache.{h,cpp}` — radial pupil-bounding-disc cache; built once per lens at scene-load. Per-λ rejection-rate counters added in Phase 4.3 (instrumentation, not yet a (radius, λ) cache; that's the escalation path).
- `RealisticCamera.{h,cpp}` — derives from `CameraCommon`. Owns `LensSystem` + `ExitPupilCache`. Implements virtual `GenerateRay` (d-line λ) and non-virtual `GenerateRaySpectral` / `GenerateRayWithLensSampleSpectral`.
- `src/Library/Rendering/CameraRayDispatch.h` — header-only static helper `GenerateCameraRaySpectral(camera, rc, ray, screen, lambda_nm)` wrapping the `dynamic_cast<RealisticCamera*>`. Mirrors the `GenerateCameraRayWithLensSample` pattern in [src/Library/Rendering/MLTRasterizer.cpp](../src/Library/Rendering/MLTRasterizer.cpp).

**New tests** (under `tests/`):
- `GlassDispersionTest.cpp` — Sellmeier evaluation against tabulated N-BK7 (n_d=1.51680, n_F=1.52238, n_C=1.51432) within 5e-6; Cauchy synthesis residuals vs Sellmeier within 1e-4.
- `LensPrescriptionParserTest.cpp` — round-trip the four shipped lenses; verify glass-name resolution; verify error messages on malformed files.
- `LensSystemRayTraceTest.cpp` — hand-verified single-planar-surface refraction (Snell), single-spherical paraxial focal length (R/(n−1)), two-surface plano-convex lens-maker's-equation residual, aperture-rejection edge case, V_d-driven dispersion direction-of-effect.
- `ExitPupilCacheConsistencyTest.cpp` — soundness (no false positives), tightness (≥95% of random pupil-disc samples succeed), radial symmetry.

**New scene files:**
- `scenes/Tests/Cameras/realistic.RISEscene` — replaces the existing thin-lens stub. Painted box at 8 m through `dgauss-50mm` at f/2.0; smoke test for the lens trace.
- `scenes/Tests/Cameras/realistic_chromatic_aberration.RISEscene` — high-contrast white-on-black off-axis edge through `wide-22mm`, rendered with `path_tracing_spectral_rasterizer`. Acceptance: lateral-colour offset between R and B channels ≥ 0.5 px; pinhole reference renders the same edge with R/B colocation ≤ 0.05 px.
- `scenes/Tests/Cameras/realistic_vignetting.RISEscene` — uniform white-wall through `dgauss-50mm` at f/2.0. Acceptance: cos⁴(field-angle) plus geometric pupil-clip within 5%.
- `scenes/Tests/Cameras/realistic_bokeh.RISEscene` — bright-point grid against black, focus pulled close. Acceptance: round bokeh at f/8.0, cat's-eye truncation off-axis at f/2.0.
- `scenes/Tests/Cameras/realistic_bdpt_mirror.RISEscene` — Cornell box + small area light + mirror, camera positioned so the mirror reflection of the light is visible *through* the lens. PT vs BDPT vs VCM agreement within 5% mean RGB at the relevant pixel; max-pixel firefly variance within 2× of PT (per [docs/skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)).
- `scenes/Tests/Cameras/realistic_lens_file_invalid.RISEscene` — negative regression; references a non-existent lens file; parser must reject with a clear error.
- `scenes/Tests/Cameras/realistic_focal_length_rejected.RISEscene` — negative regression; specifies `focal_length`; parser rejects with the dedicated error message.
- `scenes/FeatureBased/Cameras/realistic_dgauss50.RISEscene` — showcase scene (lands with Phase 4.5).
- `scenes/Tests/Bench/cornellbox_realistic_dgauss50.RISEscene` — perf bench scene paired with a thin-lens baseline.

**Modified existing files:**
- [src/Library/Parsers/AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) — re-introduce `RealisticCameraAsciiChunkParser` (the keyword is reserved per Phase 0). New `Describe()` per the headline parameter set above. `Finalize()` resolves `lens_file` via `RISE_MEDIA_PATH`, attempts companion `.glass` sidecar load (silent on absence), calls `pJob.SetRealisticCameraFromLensFile(...)`. Reject `focal_length` with the explicit error: *"focal_length is determined by the lens prescription; remove this parameter or use thinlens_camera if you want direct focal-length control."*
- [src/Library/Interfaces/IJob.h](../src/Library/Interfaces/IJob.h) — append pure-virtual `SetRealisticCameraFromLensFile`. Safe vtable extension because `IJob` is the construction interface; out-of-tree subclassers fail at compile time, not at runtime.
- [src/Library/Job.{h,cpp}](../src/Library/) — concrete override calls `RISE_API_CreateRealisticCamera`.
- [src/Library/RISE_API.{h,cpp}](../src/Library/) — new exported `RISE_API_CreateRealisticCamera(ICamera**, lensFilePath, sensorSize, fstop, focusDistance, sceneUnitMeters, ...common-camera-params...)`. Distinct symbol; ABI-additive.
- [src/Library/SceneEditor/CameraIntrospection.cpp](../src/Library/SceneEditor/CameraIntrospection.cpp) — `RealisticCamera*` branch in `Inspect`/`GetPropertyValue`/`SetProperty`/`GetDescriptorKeyword`. `lens_file` surfaces as a `Filename`-typed property with apply-on-blur semantics; `focal_length` does NOT appear in the panel for this camera type.
- [src/Library/Rendering/PathTracingSpectralRasterizer.cpp](../src/Library/Rendering/PathTracingSpectralRasterizer.cpp) — move the `GenerateRay` call **inside** the per-λ loop (both HWSS and non-HWSS branches at line 126). Replace with `GenerateCameraRaySpectral(camera, rc, cameraRay, ptOnScreen, lambda)`. Bit-identical for non-spectral cameras.
- [src/Library/Rendering/BDPTSpectralRasterizer.cpp](../src/Library/Rendering/BDPTSpectralRasterizer.cpp) — same edit at lines 99 (non-HWSS) and 317 (HWSS). HWSS regenerates per λ for spectral consistency (decision 8).
- [src/Library/Rendering/MLTSpectralRasterizer.cpp](../src/Library/Rendering/MLTSpectralRasterizer.cpp) — adds parallel `GenerateCameraRayWithLensSampleSpectral` dispatch helper alongside the existing `GenerateCameraRayWithLensSample`. PSSMLT continuity in λ is preserved (λ is an extra primary-sample dimension).
- [src/Library/Rendering/VCMSpectralRasterizer.cpp](../src/Library/Rendering/VCMSpectralRasterizer.cpp) — line 290.
- [src/Library/Cameras/CameraUtilities.{h,cpp}](../src/Library/Cameras/) — `RealisticCamera` branches in `Rasterize` / `Importance` / `PdfDirection` per Phase 4.4.
- [src/Library/Parsers/README.md](../src/Library/Parsers/README.md) — new `realistic_camera` section documenting the chunk, the lens-file format, the sidecar `.glass` format, and the photographic parameter mapping.
- [src/Library/README.md](../src/Library/README.md) and [scenes/FeatureBased/README.md](../scenes/FeatureBased/README.md) — list the new camera once Phase 4.5 ships.
- [tests/SceneEditorSuggestionsTest.cpp](../tests/SceneEditorSuggestionsTest.cpp) — chunk-count assertion bump (will be at 137 + 1 = 138 when `realistic_camera` re-registers).

**Lens-database files** (new top-level `data/` directory; resolved via `RISE_MEDIA_PATH` exactly like `textures/` and `models/`):
- `data/lenses/dgauss-50mm.dat` + `data/lenses/dgauss-50mm.glass` — PBRT double-Gauss; 6 elements, f/2.0, 50 mm. Default reference lens; well-corrected, baseline rectilinear behaviour.
- `data/lenses/wide-22mm.dat` + `data/lenses/wide-22mm.glass` — PBRT wide-angle; 9 elements, f/4.0, 22 mm. **The CA showcase lens** — short focal + wide field amplifies lateral colour.
- `data/lenses/tele-150mm.dat` + `data/lenses/tele-150mm.glass` — PBRT telephoto. Compressed perspective, narrow DOF demo.
- `data/lenses/fisheye-10mm.dat` + `data/lenses/fisheye-10mm.glass` — PBRT Nikkor 10 mm fisheye; 8 elements (spherical-only despite the focal length).
- `data/lenses/glass_catalog.txt` — Schott Sellmeier coefficients for ~20 common optical glasses, shared across all sidecar files.
- `data/lenses/README.md` — lens-file format spec, sidecar format spec, attribution to PBRT (BSD), citations to Kolb '95 and the Schott catalog.

**Build-project files (per [CLAUDE.md](../CLAUDE.md) "Source-file add/remove"):** all five — `build/make/rise/Filelist`, `build/cmake/rise-android/rise_sources.cmake`, `build/VS2022/Library/Library.vcxproj` and `.filters`, `build/XCode/rise/rise.xcodeproj/project.pbxproj` (both library and GUI targets) — receive entries for every new `.cpp` and `.h` listed above. New tests register in `tests/Makefile`, `run_all_tests.sh`, `run_all_tests.ps1` per the existing test pattern.

#### ABI

Three layers per [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md):

- **Layer 1 (exported function signatures).** `RISE_API_CreateRealisticCamera` is a *new* symbol — purely additive. No existing exports change.
- **Layer 2 (vtable layout).** `ICamera` vtable **unchanged**; `GenerateRaySpectral` / `GenerateRayWithLensSampleSpectral` are non-virtual on `RealisticCamera` and reached via `dynamic_cast`. `IJob` gains an appended pure-virtual `SetRealisticCameraFromLensFile` — safe because `IJob` is the construction interface; out-of-tree subclassers fail loud at compile time.
- **Layer 3 (derived-class name hiding).** New method name; no overload collision; no `using` declarations needed.

Verification (Step 5 of the skill): after Phase 4.2 lands, diff `ICamera.h` against the prior revision (must be byte-identical); run `nm -g` on the library and confirm prior `RISE_API_*` symbols still resolve.

#### Acceptance per sub-phase

**Phase 4.0:**
- `GlassDispersionTest`: N-BK7 Sellmeier residuals vs Schott catalog within 5e-6 at d/F/C lines; Cauchy synthesis from (n_d, V_d) within 1e-4 of Sellmeier at the same wavelengths.
- `LensPrescriptionParserTest`: round-trip the four shipped lens files; sidecar glass-name resolution succeeds for every cataloged glass; cleanly reports an error on a malformed `.dat` line.

**Phase 4.1:**
- `LensSystemRayTraceTest`: single-planar Snell exact within 1e-9; single-spherical paraxial focal length (R/(n−1)) within 1e-4 mm; two-surface plano-convex lens-maker residual within 1e-3 mm; aperture rejection returns `optional::nullopt`; high-V_d crown produces smaller F-vs-C deflection than low-V_d flint at the same geometry.

**Phase 4.2:**
- `ExitPupilCacheConsistencyTest`: zero false positives across 100 000 random sensor-position × pupil-sample combinations; ≥95% tightness on the dgauss-50mm; radial symmetry to floating-point precision.
- `realistic.RISEscene` renders end-to-end and matches a `pinhole_camera` of equivalent FOV in the central image region within RMSE < 0.02 in tonemapped sRGB. Off-axis differs intentionally (vignetting + distortion).
- Build clean across both `make` and Xcode `RISE-GUI`, zero warnings.
- `IJob` ABI verification per the skill.

**Phase 4.3:**
- `realistic_chromatic_aberration.RISEscene`: lateral colour offset between R and B channel centroids at the off-axis edge ≥ 0.5 px; pinhole reference colocates within 0.05 px. Direction-of-effect matches lens theory (blue defocuses more than red on a wide-angle off-axis edge).
- HWSS hero-ray-vs-per-λ measurement: rendered RMSE difference logged; if < 0.5%, switch default to shared-hero (decision 8).
- `wavelength_resolved=false` toggle produces the same render as a pinhole CA scene up to MC noise (debug A/B sanity).
- Per-λ pupil-cache rejection rates at 400 nm and 750 nm logged for the four shipped lenses; if > 30% on any, escalate to (radius, λ)-keyed cache.

**Phase 4.4:**
- `realistic_bdpt_mirror.RISEscene`: PT vs BDPT vs VCM mean RGB at the mirror-reflection-of-light pixel agrees within 5%; max-pixel firefly variance in BDPT/VCM within 2× of PT (the `bdpt-vcm-mis-balance` skill's tolerance).
- Per-strategy splat dump shows the t=1 connection through the lens producing finite, non-zero contribution — no NaN, no infinity.
- Chief-ray cache reduces per-connection iteration count to 2–3 (logged); this is the gate that keeps Phase 5 optional.

**Phase 4.5:**
- Adversarial review pass per [docs/skills/adversarial-code-review.md](skills/adversarial-code-review.md): three reviewers in parallel, orthogonal concerns (numerics / ABI / spectral integration). Zero CRITICAL, MAJORs addressed in-scope.
- Vignetting falloff: cos⁴(θ) plus pupil-clip term within 5% on the white-wall scene.
- Bokeh: round at f/8.0, cat's-eye off-axis at f/2.0.
- All test scenes deterministic across runs (fixed seed).

#### Performance baseline plan

Bench scene: `scenes/Tests/Bench/cornellbox_realistic_dgauss50.RISEscene`. Standard Cornell box, dgauss-50mm at f/2.8, focus on the back wall. Five trials at 64 spp via `bench.sh` (sets `render_thread_reserve_count 0`). Persist baseline at `docs/performance/realistic_camera_baseline.txt` per [docs/skills/performance-work-with-baselines.md](skills/performance-work-with-baselines.md).

Targets — wall-clock vs the matched-fstop matched-focal-length thin-lens baseline:
- Phase 4.2 (geometric trace, no spectral): ≤ 1.5×.
- Phase 4.3 (spectral CA, per-λ trace): ≤ 2.0×. The 4× HWSS lens-trace overhead is partially absorbed by unchanged path-tracing cost.
- Phase 4.4 (BDPT/VCM camera-connection): ≤ 2.5× thin-lens BDPT cost. The chief-ray cache from decision 9 is what gets us under this number.

**Phase 5 polynomial-optics escalation gate:** if Phase 4.4 wall-clock exceeds 2.5× thin-lens BDPT on the bench scene, escalate to Phase 5. Otherwise defer indefinitely — geometric tracing is the simpler ground-truth path.

Profile before optimizing: `xcrun xctrace` (macOS) or `perf record` (Linux) against the Phase 4.2 bench scene; verify the lens trace dominates camera-side cost (~30% of total render time for a Cornell box, dwarfed by BSDF/intersection in the path tracer's own work).

#### Out of scope (and why)

- **Aspheric surfaces beyond simple even-aspheres** — none of the four shipped lenses use them; the `LensSurface` interface accommodates a future `EvenAsphereSurface` subclass cleanly.
- **Gradient-index (GRIN) glass** — not in the shipped lenses; defer until a real consumer surfaces.
- **Real-world IR / UV bands** — RISE's spectral configuration is 380–780 nm; out-of-band physics is outside the project's spectral scope.
- **Complex-valued IOR for metal coatings** — anti-reflective coatings + lens-flare ghost paths need complex Fresnel; that's Phase 6 territory.
- **Mechanical / sensor-microlens vignetting** — geometric pupil-clip + cos⁴ produces the dominant visible vignetting; sensor-side vignetting is Phase 7.
- **`photographic_camera` keyword** — `thinlens_camera` already covers the photographic-friendly param set; revisit only if a user explicitly asks.
- **Zoom lenses** — all four shipped lenses are primes. Zooms re-position internal element groups via a focal-length parameter; the prescription format would need extending. Defer.
- **Lens-flare and aperture diffraction** — Phase 6, separately scoped.
- **Tilt-shift on the realistic camera** — doesn't compose cleanly with the multi-element trace (no axisymmetric prescription for a tilted sensor in PBRT format). Use `thinlens_camera` for tilt-shift.

#### Skills checklist per sub-phase

- **4.0:** [`write-highly-effective-tests`](skills/write-highly-effective-tests.md) (Sellmeier table is foundational; tight unit tests vs tabulated values pay off through every later phase).
- **4.1:** [`precision-fix-the-formulation`](skills/precision-fix-the-formulation.md) (resist the urge to bump aperture-clip thresholds when per-surface residuals accumulate; find the formulation), [`write-highly-effective-tests`](skills/write-highly-effective-tests.md).
- **4.2:** [`abi-preserving-api-evolution`](skills/abi-preserving-api-evolution.md) (the load-bearing skill — `ICamera` vtable preservation, safe `IJob` extension, non-virtual `RealisticCamera` methods), [`const-correctness-over-escape-hatches`](skills/const-correctness-over-escape-hatches.md) (exit-pupil cache is conceptually-const + lazily-built; run the decision tree before reaching for `mutable`).
- **4.3:** [`variance-measurement`](skills/variance-measurement.md) (spectral CA changes the variance profile of every spectral render; confirm RMSE-vs-truth reduces, not just looks different).
- **4.4:** [`bdpt-vcm-mis-balance`](skills/bdpt-vcm-mis-balance.md) (the PT-vs-BDPT-vs-VCM agreement test is the diagnostic for camera-connection correctness), [`performance-work-with-baselines`](skills/performance-work-with-baselines.md) (the 2.5× threshold for Phase 5 escalation).
- **4.5:** [`adversarial-code-review`](skills/adversarial-code-review.md) (Phase 4 is correctness-sensitive; 2–3 reviewers in parallel with orthogonal concerns), [`simplify`](skills/simplify.md) (after the dust settles, scan for reuse — e.g. shared Snell-refraction code with `DielectricSPF`).

#### Open questions deferred to phase boundaries (measurement-driven)

- HWSS shared-hero-ray vs per-λ retrace — decided in Phase 4.3 by measurement on the CA scene.
- Per-(radius, λ) exit-pupil cache — escalation triggered at Phase 4.2/4.3 by per-λ rejection-rate logging.
- Phase 5 polynomial-optics gate — fired at Phase 4.4 by the 2.5× bench measurement.

---

### Phase 5 — Polynomial-optics acceleration (only if needed)

**Goal.** Cut Phase 4's per-ray cost via a polynomial fit of the lens transfer. Land *iff* Phase 4 measurably hurts BDPT/VCM throughput on representative workloads — performance-baseline driven, not speculative.

**Scope.** Implement Hanika & Dachsbacher 2014 form: degree-3 or degree-5 4D Taylor expansion of the (sensor → world) and (world → sensor) ray maps, fit offline once per lens (or computed at scene-load), evaluated in tens of ns per ray. Aperture importance sampling avoids the wasteful exit-pupil rejection of the geometric tracer.

**Where it pays off.** BDPT/VCM camera connections are the heavy user — every light-subpath vertex tries to connect through the camera, so the per-call cost is multiplied by light-subpath length × samples. Hanika '14 is specifically engineered for this — it provides the inverse map needed by `We()` evaluation as a closed-form polynomial, removing the per-call exit-pupil iteration from §4.4.

**Files touched.** `src/Library/Cameras/PolynomialLens.{h,cpp}` (new). `RealisticCamera` gains a `mode` flag to switch between geometric (truth) and polynomial (fast) tracing. The geometric path remains and serves as ground truth for validation.

**ABI.** Additive — `mode` is a constructor arg with a default.

**Acceptance.**
- Polynomial mode matches geometric mode within a target RMSE on a held-out test (rendered scenes, not just ray pairs).
- BDPT/VCM throughput improves by a measurable factor on the camera-heavy bench (typical published gains: 5–20× for the lens trace step, which translates to 1.3–1.8× whole-render).
- Variance does not increase — the [variance-measurement skill](skills/variance-measurement.md) protocol applies.

---

### Phase 6 — Aperture diffraction & lens flare

**Goal.** Starbursts, glare, and lens flare ghosts. Mostly post-process; a non-trivial part touches the lens stack again so it lives here rather than in a "filters" pass.

**Scope.**
- **Aperture diffraction PSF** — for a given aperture shape and wavelength, compute the Fraunhofer pattern (FFT of the aperture). Convolve a brightness-thresholded radiance buffer with a per-wavelength PSF and add to the final image. Spectrally, this gives rainbow-tinted starbursts instead of the white-only ones every other renderer ships.
- **Lens flare ghosts** — Hullin 2011 enumerated two-bounce paths inside the lens stack. A pre-pass tracing a small number of bright sources through the stack with one anti-Snell bounce per element-pair identifies which ghosts are visible; render them as additive overlays.

**Files touched.** `src/Library/Rendering/Postprocess/AperturePSF.{h,cpp}`, `src/Library/Cameras/LensFlare.{h,cpp}`. Hooks into the existing post-process / radiance-output stage.

**ABI.** Additive — new post-process chunk.

**Acceptance.** Visual validation against reference photographs for canonical lenses. Starburst point counts match aperture blade counts.

---

### Phase 7 — Sensor / shutter model (parallel track)

**Goal.** Real exposure pipeline: spectral sensor SPD, Bayer mosaic, read noise, dark current, non-rectangular shutter weighting. Pairs with Phase 4 to *match* a reference photograph rather than just resemble one.

This is its own subsystem and should get its own roadmap doc once Phase 4 is in flight; flagged here so we don't conflate it with lens work. Parts of the scaffolding are already in `CameraCommon` (`exposureTime` / `scanningRate` / `pixelRate`), and the existing `Frame` class is the natural place to grow a sensor model.

**Scope sketch (not committed yet).**
- Camera-RGB SPDs (CIE sensitivity functions or per-camera measured) replace the post-spectral colour-matching matrix.
- Bayer pattern + demosaic option for synthetic-data workflows.
- Rolling shutter pose-per-scanline sampling. (`scanningRate` is the input.)
- Read noise + dark current additive at the sensor stage.
- Shutter-shape weighting (rectangular default; Cosine, exit-only, mechanical-curtain shapes).

**Defer until:** Phase 4 lands. The realistic lens is the dominant visual factor; sensor effects sit on top of it.

---

### Phase 8 — Neural lenses (deferred)

**Goal.** Match a specific real lens-you-can-hold from photographs of a calibration target.

**Defer until:** there is a concrete consumer (e.g., a VFX matchmove pipeline) and we have the differentiable-rendering / tabular-NN inference plumbing it requires. The Xian 2023 invertible-INN form is the right starting point if and when this becomes a real ask. Until then, the geometric / polynomial paths cover the offline-renderer use cases.

---

### Phase 9 — Light-field / pushbroom (deferred, niche)

**Goal.** Light-field capture for post-refocus; pushbroom / rolling-shutter for sat / drone / phone synthetic data.

**Defer until:** synthetic-data work or a research consumer requests them. Both are mostly camera-side scheduling problems (per-pixel time / pose), not lens-design problems — they slot in cleanly *after* Phase 7 lands the time-resolved sensor.

---

## 6. Cross-cutting acceptance

For *every* phase:

- **Skill checklist:** [abi-preserving-api-evolution](skills/abi-preserving-api-evolution.md) before any header change, [performance-work-with-baselines](skills/performance-work-with-baselines.md) for any change with a perf claim, [adversarial-code-review](skills/adversarial-code-review.md) for non-trivial changes (Phase 4 absolutely qualifies), [const-correctness-over-escape-hatches](skills/const-correctness-over-escape-hatches.md) before reaching for `mutable` on the exit-pupil cache (it almost certainly is conceptually-const, lazily-built — same shape as the camera's frame matrix).
- **Build matrix:** clean `make -C build/make/rise -j8 all` and clean Xcode `RISE-GUI` rebuild, both warning-free. Increment build also clean — `find src/Library/Cameras -name '*.o' -delete` then full rebuild — to catch warnings on files that didn't recompile.
- **Tests:** `./run_all_tests.sh` (macOS/Linux) or `.\run_all_tests.ps1` (Windows). New regression scenes go under [scenes/Tests/Cameras/](../scenes/Tests/) following the existing taxonomy in [scenes/README.md](../scenes/README.md).
- **Docs:** parser additions documented in [src/Library/Parsers/README.md](../src/Library/Parsers/README.md). New cameras listed in [src/Library/README.md](../src/Library/README.md) and [scenes/FeatureBased/README.md](../scenes/FeatureBased/README.md) once a showcase scene exists.

---

## 7. Open questions

- **Per-wavelength primary rays — sample-and-hold or per-sample re-trace?** A spectral renderer typically picks one λ per primary sample and uses it through the path. The lens trace happens once per primary sample, so per-λ is "free." But MLT mutations might want to mutate λ separately from `lensSample` — needs to be checked against `MLTRasterizer`'s mutator state.
- **Exit-pupil cache key — image-plane radius or full (x,y)?** PBRT keys on radius (assumes radial symmetry). Anamorphic / squeezed lenses break that assumption. Decide at Phase 4.2.
- **Lens-prescription format compatibility.** PBRT's format is the obvious choice (largest available lens corpus). Zemax `.zmx` is the industrial standard but proprietary. Start PBRT-only; revisit if a user supplies a real Zemax file we want to import.
- **Polynomial-optics fit time — at scene-load or pre-baked?** Hanika '14 fits in seconds; either works. Pre-baked is reproducible; load-time is less infrastructure. Decide at Phase 5.
- **Should we keep a `photographic_camera` chunk?** Once Phase 4 reclaims `realistic_camera` for the multi-element camera, the photographer-friendly thin-lens parameterisation (film_size + fstop + focal_length + focus_distance, no lens prescription) loses its keyword. `thinlens_camera` already covers it via aperture/focal/focus, but those parameters are less DP-friendly. Optional Phase-1 add-on.

---

## 8. Reference reading

Implementation:
- [PBRT v3 — Realistic Cameras](https://www.pbr-book.org/3ed-2018/Camera_Models/Realistic_Cameras) — canonical multi-element camera implementation.
- [PBRT v4 — Cameras and Film](https://pbr-book.org/4ed/Cameras_and_Film) — modernised version with film/sensor model.
- [Kensler — Tilt-Shift Rendering Using a Thin Lens Model, RT Gems II ch. 31](https://link.springer.com/chapter/10.1007/978-1-4842-7185-8_31) — minimal Phase 1 reference.
- [Google Jump — Rendering ODS Content](https://developers.google.com/vr/jump/rendering-ods-content.pdf) — Phase 3 reference.

Acceleration:
- [Hullin et al. — Polynomial Optics (UBC TR 2012)](https://www.cs.ubc.ca/labs/imager/tr/2012/PolynomialOptics/) — original poly-optics framing.
- [Hanika & Dachsbacher 2014](https://jo.dreggn.org/home/2014_polynomial_optics.pdf) — BDPT-compatible polynomial optics with aperture importance sampling. The exact form RISE wants in Phase 5.
- [Goossens et al. 2022 — Ray-Transfer Functions](https://stanford.edu/~wandell/data/papers/2022-Goossens-RTF.pdf) — black-box surrogate variant.

Wave optics & flare:
- [Hullin et al. 2011 — Physically-Based Real-Time Lens Flare](https://resources.mpi-inf.mpg.de/lensflareRendering/) — Phase 6 reference.

Neural:
- [Zheng et al. 2017 — NeuroLens](https://quan-zheng.github.io/publication/NeuroLens-paper.pdf).
- [Xian et al. 2023 — Neural Lens Modeling](https://neural-lens.github.io/) — invertible INN form.
- [Kim et al. WACV 2024 — LensNeRF](https://openaccess.thecvf.com/content/WACV2024/papers/Kim_LensNeRF_Rethinking_Volume_Rendering_Based_on_Thin-Lens_Camera_Model_WACV_2024_paper.pdf).

Production tools (for taste):
- [Lentil — Polynomial-optics camera for Arnold](http://www.lentil.xyz/).
- [zoic — extended Arnold camera shader](https://github.com/zpelgrims/zoic).
- [RenderMan PxrCamera / anamorphic bokeh](https://rmanwiki-26.pixar.com/display/REN/Bokeh).
