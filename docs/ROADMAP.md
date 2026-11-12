# RISE Implementation Roadmap

**Last Verified Against Repository:** `HEAD` (`2026-11-12`, post-Tier-0 completion)
**Lifecycle:** Active roadmap — authoritative consolidated backlog across core rendering, materials, spectral/color pipeline, GUI/editor, platform bridges, and agentic authoring.

See [README.md](README.md) for the documentation catalog, [DEBT_LEDGER.md](DEBT_LEDGER.md) for the granular correctness-debt tracker, [IMPROVEMENTS.md](IMPROVEMENTS.md) for historical rendering gap criteria, and [GUI_ROADMAP.md](GUI_ROADMAP.md) for the GUI milestone breakdown.

---

## Executive Snapshot: What Has Already Shipped

Before prioritizing new work, note what is **complete in production** as of November 2026 so contributors do not re-plan shipped systems:

- **Scene Language & CST Core:** Lossless CST parse/derive/save (`RISE ASCII SCENE 7`, 176 descriptor-driven chunk keywords in [../src/Library/Parsers/ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)), `let` bindings, `expr(...)` evaluator with `sample()` / `sample_scalar()`, incremental editor derive (`DeriveToJobIncremental`), and multi-variant scenes (`scene_variant` / `active_scene_variant`).
- **Integrators & Transport:** Unidirectional PT, BDPT, VCM (including SPPM auto-radius clipped to pixel footprint, surface + volumetric merging, and continuous-PMF env+light sampling), PSSMLT (frozen legacy tier), Specular Manifold Sampling (`SMS` + `sms_extended` Phases 1–4 across RGB/NM/HWSS), OpenPGL path guiding (one-sample + RIS with exact aggregate MIS partitioning), and the `auto_rasterizer` / `auto_spectral_rasterizer` two-tier router.
- **Materials, Curves & Signals:** GGX + height-correlated Smith $G_2$ directional/anisotropic energy-compensation LUTs, `pbr_metallic_roughness_material`, `coated_material` (refracted-frame substrate + trapped diffuse recycling + `coat_normal`), `ggx_material` (`fresnel_mode thinfilm`) + `dielectric_material` (`ar_layer`) Airy thin-film interference, `fabric_material` (Charlie sheen) + `weave_material` + `add_fuzz`, `hair_geometry` + Chiang `HairBSDF` + Yan 2017 fur medulla (`medulla_ratio`) + `.hair` loader + Hair Phase 4 LOD, Donner-Jensen diffusion + `randomwalk_sss_material` (spectral + relative exterior IOR), scalar material `alpha_coverage`, `relief_modifier` / `glint_modifier` / `add_wetness` / `add_wear`, and cross-object `proximity(r)` / watertight `interior(r)` shading signals.
- **Spectral & Color Pipeline:** `RISEPel = Rec709RGBPel` (Linear Rec.709 / D65 working space), Stage C illuminant-aware Jakob-Hanika LUT (`100 %` cell convergence; `GetColorNM` vs `GetRadianceNM`), physical-scalar `IScalarPainter` separation, GPU-backed OIDN denoising with albedo/normal AOVs (`OIDN-P1-1` inline delta-walk first-diffuse AOV accumulation and `oidn_prefilter`), and `FrameStore` HDR display pipeline.
- **Multi-Platform GUI, Importers & Agent Surface:** Native macOS SwiftUI + Metal and Windows Qt6 + D3D11/OpenGL editors (with Material Editor Slices 1–6: node graph, `ObjectGraphCanvas`, 3D `MaterialPreview` shaderball, Kelvin/Sellmeier/preset-metal/thin-film spectral controls, `NamedViewsPanel`, and Keychain / Windows Credential Manager API-key persistence), Android Compose + NDK viewer/editor, glTF 2.0 importer (9 KHR material/light/transform extensions), Blender 4.x add-on (C ABI v17), and the 43-tool MCP / HTTP / in-app LLM agent with 9 runtime authoring skills (`skills/agent/`), 12 contributor workflow skills (`docs/skills/`), and 525 standalone C++ test suites.

---

## Tier 0 — Immediate Bug Fixes, Active Correctness Debts & Parity Quick Wins (**COMPLETED 2026-11-12**)

All 14 Tier 0 items are **complete, red-proven, and verified green** on `master` (leaving **0 active unparked** items in [DEBT_LEDGER.md](DEBT_LEDGER.md)):

### 0.1 Code & Scene Hygiene Bugs (**COMPLETED**)

1. ~~**Blender Bridge Normal-Map Color-Space Bug:**~~ **SHIPPED.** Switched Blender normal-map and non-color scalar PNG textures from `COLOR_SPACE_ROMM_LINEAR = 2` to `COLOR_SPACE_LINEAR = 0` (`Rec709RGB_Linear`) in `src/Blender/addons/rise_renderer/{exporter.py,material_bake.py}` and `src/Blender/native/rise_blender_bridge.{h,cpp}`, clamped legacy `color_space == 2` normal-map callers to `0`, and gated in `tests/BlenderBridgeCoatNormalTest.cpp` + `test_hair_export.py`.
2. ~~**Unblock `add_wear` on Analytic Patch Geometries:**~~ **SHIPPED.** Removed `bezierpatch_geometry` and `bilinearpatch_geometry` from `CurvBarrenGeometryKind_` in [../src/Library/Agent/AgentSession.cpp](../src/Library/Agent/AgentSession.cpp), unblocking `add_wear` on analytic patches (`tests/AgentAddWearTest.cpp`).
3. ~~**Relocate Stray Frozen-Material Test Scene & Stale RPC Comment:**~~ **SHIPPED.** Removed duplicate `scenes/Tests/Materials/composite_material.RISEscene` (canonical copy kept at `scenes/Tests/Legacy/composite_material.RISEscene`) and aligned `populate_scene` wire-contract comments in [../src/Library/Agent/AgentRpc.h](../src/Library/Agent/AgentRpc.h) / `AgentRpc.cpp`.

### 0.2 Active Un-Parked Correctness Debts ([DEBT_LEDGER.md](DEBT_LEDGER.md) `DL-626..DL-632` — **ALL 7 CLOSED**)

1. ~~**`DL-632` (Windows Test Portability):**~~ **CLOSED.** Guarded POSIX-dependent test suites and fixtures for `_WIN32`, migrated all 143 scratch-writing test files in `tests/` to `RISE::TestSupport::ScratchBaseDir()` / `ScratchPath()` in `tests/TestTempDir.h` (`TMPDIR` -> `TEMP` -> `TMP` -> `temp_directory_path(ec)` -> `"."`, zero hardcoded `_WIN32`-active `"/tmp"` paths), quoted `_WIN32` `_popen` commands in `LuminarySamplerAreaTest.cpp` and space-containing interactive `load "..."` paths in `CLIExitStatusTest.cpp` / `AsciiCommandParser.cpp`, cleared `RISE_MEDIA_PATH` in `run_all_tests.{sh,ps1}` (with non-fatal `\tmp` fallback in `run_all_tests.ps1`), and added a fail-closed `Win32PpVal` `_WIN32` preprocessor evaluator + unit self-tests scanning all 546 files in `tests/` (`tests/SourceHygieneTest.cpp` **192/0**; native MSVC test execution remains gated on a Windows CI/host).
2. ~~**`DL-628` (Deterministic Heterogeneous Medium RNG):**~~ **CLOSED.** Added `ScopedMediumTileSeed` + `ActiveMediumRNG{,_NM}()` across all tile/row/light/MLT dispatchers so multi-threaded Pel, NM, and HWSS `HeterogeneousMedium` transmittance evaluations reproduce bit-identically under a fixed `std::srand` (`tests/LuminarySamplerAreaTest.cpp` mode `F`).
3. ~~**`DL-627` (CSG Subtraction Multi-Lobe Edge Cases):**~~ **CLOSED.** Fixed outside-both covered-lobe advance, multi-`B`-entry interval clamping, degenerate zero-thickness `B` pinch skipping (`SkipDegenerateMultiLobeB`, `SkipDegenerateClampSummaryB`), shared nested-CSG advance budgeting (`ScopedCsgQueryBudget` across `WalkTrimmedSheet` and solid loops), un-truncated continuation probing (`RISE_INFINITY`), unbacked `CoincidenceTol` comparisons behind covering `B` lobes, and shrank per-frame `NextLobeSummary` storage from ~1 KB to ~176 bytes in `src/Library/Objects/CSGObject.cpp` (`tests/CsgMeshOperandTest.cpp` Section `L`, **316/0**).
4. ~~**`DL-626` (Open-Sheet Seeding & Walk Edge Cases):**~~ **CLOSED.** Added `ShapeRevision()` tracking for AABB-preserving shape edits, canonical declaration- and TLAS-order-invariant geometric ordering (`SheetGeomKeyLess` with transform-matrix tie-breaking, `Snapshot::columnDist`, exact `aloneDist`/`activeDists`/`sheetColDists` ray-hit distances, `ProbeCoplanarSheetBackAt` + `ProbeCoplanarBlockingOpenSheetAt` + `RecordCoplanarOrderedExitsAt` + `OpenSheetCatalog::closedTrackable` coplanar handling across normal/detour/`TallyProbe` walks, `ConfirmPoseScope` + `OpenSheetNormalGroup::PlaneBand` binary search, and confirmation-hit tie-breaking in `ConfirmOpenSheetsAt`), shared `columnWalkUsed` + `GroupDetourRayBudget()` enforcement across `sheet.normal` + `auxNormals`, `FindRetainedSelf` material-identity guard, multi-fold `CrossingMedia` enclosure in `ResolveOpenSheetCrossing`, `DetourOriginForSeedExit` past `pSeedObject`, and 64-sample / 32-normal `DescribeOpenSheet` facet coverage ([DL622_OPEN_SHEET_SEEDING_AND_WALK_MEMBERSHIP.md](DL622_OPEN_SHEET_SEEDING_AND_WALK_MEMBERSHIP.md) §5.8; `tests/OpenSheetHalfSpaceSeedingTest.cpp` **239/0**).
5. ~~**`DL-629` (Non-Rectangular `ClippedPlaneGeometry` Luminary Residual):**~~ **CLOSED.** Verified zero luminary sampler bias via high-power $96 \times 96 \times 1024\,\text{spp} \times 64$ quadrature and fixed progressive Pel multi-pass `XYZPel` round-trip compounding (`ProgressivePixel::LoadPelPassInit` / `StorePelPassDelta` across `PixelBasedPelRasterizer`, `PathTracingPelRasterizer`, `BDPTPelRasterizer`, `VCMPelRasterizer`), progressive `pSplatFilm` (`t == 1`) `BidirectionalRasterizerBase::AddAdaptiveSamples` (`mSplatTotalSamples`) normalization across Pel/NM/HWSS `BDPT*` & `VCM*`, single- and multi-threaded (`RasterizeBlockDispatcher`) 1-tile progressive progress weighting and cancellation, orthographic `BDPTIntegrator` `eyeVerts[0].position`, rotated `OrthographicCamera` basis projection in `RasterizeOrthographic`, and odd-dimension `OrthographicCamera` centering (`tests/LuminarySamplerAreaTest.cpp` modes `P`/`Q` **90/0**, `tests/CameraImportanceTest.cpp` **1000/0**).
6. ~~**`DL-630` (Certified Watertight `.bezier` / `.bilinear` Glass Models):**~~ **CLOSED.** Shipped `models/bezier/teapot_watertight.bezier` (632 bicubic patches), `models/raw/vessel_watertight.bilinear` (552 bilinear patches), and `scenes/Tests/Geometry/teapot_watertight_glass.RISEscene`, certified across analytic patch refraction, `DisplacedGeometry`, `SeedFromPoint`, and CSG while preserving historical defective-model census pins (`tests/PatchSetCertificationTest.cpp` **253/0**).
7. ~~**`DL-631` (`+2 %` Render CPU Drift Bisect):**~~ **CLOSED.** Replaced `MyIORStack`'s heap `std::vector<IORDATA>` base with inline 4-entry `m_inline[4]` SBO (`-65 %` `malloc`/`free` samples; copy-on-write `MyIORStack::ContextList` stays out-of-line only for open-sheet pushes), added 8-entry inline SBO in `LaneSeed` (`SeedLanesFromPoint`), added `!cat.hasTrackableMedia` 0-ray opaque-scene skip + `!cat.hasSheets` 32-entry stack fast path in `SeedFromPointImpl`, restored lazy `ior_stack` bounce copy and lazy `#ifdef RISE_ENABLE_OPENPGL` `guidedIorStack` construction in PT, and eliminated duplicate `HWSSLaneSeeds` origin probe walks in `PathTracingIntegrator::IntegrateRayHWSS` (`hwss_prism_dispersion_pt` `-3.23 %` wall time across $n = 6$ `ABBA` pairs; `cornellbox_bdpt_materials_pt` `-0.24 %` user CPU).

### 0.3 Desktop Parity & CLI Quick Wins (**COMPLETED**)

1. ~~**Windows Qt6 `NamedViewsPanel` Widget (`GUI_ROADMAP` B2 Parity):**~~ **SHIPPED.** Added `build/VS2022/RISE-GUI/NamedViewsPanel.{h,cpp}` wired into `MainWindow.{h,cpp}` and `RISE-GUI.vcxproj`.
2. ~~**Windows Credential Manager for LLM API Keys:**~~ **SHIPPED.** Added `build/VS2022/RISE-GUI/CredentialStore.{h,cpp}` (`CredWriteW` / `CredReadW` / `CredDeleteW`) with write-only `ChatPanel` key entry matching macOS `AgentChatKeychain` in `ChatViewModel.swift`.
3. ~~**CLI Overrides (`OIDN-P2-1` & Camera Selection):**~~ **SHIPPED.** Added `--denoise` / `--no-denoise` (`IRasterizer::SetDenoisingOverride`, including `AutoRasterizer` for both `auto_rasterizer` and `auto_spectral_rasterizer`), `--camera <name>` / `--camera=<name>`, and `=`-joined `--width=` / `--height=` / `--pixel-ar=` in `src/RISE/CliOptions.h` + `src/RISE/commandconsole.cpp` (`tests/CliOverridesTest.cpp` **77/0**).
4. ~~**Standalone `MaterialPreviewTest`:**~~ **SHIPPED.** Added `tests/MaterialPreviewTest.cpp` gating headless 3D `MaterialPreview` shaderballs, D1–D3 spectral helpers, and `CreateInteractiveMaterialPreviewPipeline` in `fast` CI (**63/0**).

---

## Tier 1 — Core Rendering, Materials, Spectral & Output Pipeline

High-impact rendering capabilities that close the remaining physical-transport and output gaps:

### 1.1 Rough Dielectric BSDF (`BTDF`, Walter et al. 2007) + Remaining glTF Extensions

- **Why:** Currently `dielectric_material` uses a delta Snell refraction with optional Phong/HG directional perturbation (`scattering`), while `ggx_material` is opaque-only. True microfacet rough transmission (VNDF half-vector sampling for both reflection and refraction with $G_2$ height-correlated masking, $1/\eta^2$ radiance scaling, and multiscatter energy compensation — [VITREOUS_ENAMEL.md](VITREOUS_ENAMEL.md) Gap G2) is the single biggest material-model gap in RISE.
- **Unlocks:**
  - Physically accurate frosted/etched glass, ground crystal, ice, and translucent opal enamel without heuristic Phong exponents.
  - Full `KHR_materials_transmission` roughness parity in [GLTF_IMPORT.md](GLTF_IMPORT.md) (currently mapped to `dielectric_material` scattering approximations) and Blender Principled BSDF `Transmission` + `Roughness`.
  - Remaining glTF 2.0 material extensions: `KHR_materials_dispersion` (Abbe-number spectral Cauchy/Sellmeier mapping), `KHR_materials_iridescence` (direct mapping onto RISE's existing `ggx_material` `fresnel_mode thinfilm` / `dielectric_material` `ar_layer` Airy layer), and `KHR_materials_clearcoat` rough/normal texture polish.

### 1.2 Full Primaries-Aware AgX Display Transform & FrameStore Multichannel Output

- **Full AgX Tonemapper:** Replace the scalar log-sigmoid placeholder in `src/Library/Rendering/DisplayTransform.h` (`DisplayTransform::AgX` — currently documented as *not* full primaries-aware AgX) with Troy Sobotka's full inset-matrix → 1D log2 Lut/curve → outset-matrix transform so saturated HDR highlights desaturate cleanly without hue skew across CLI, macOS Metal, Windows D3D11/GL, and Android viewports ([FRAMESTORE_DESIGN.md](FRAMESTORE_DESIGN.md)).
- **Multichannel AOV EXR Output (`FrameStore` L7):** Upgrade `EXRFrameEncoder` (`SupportsAOVs() == true`) to write multi-layer OpenEXR files (`Beauty`, `Albedo`, `Normal`, `Depth`, `Variance`), expose an `aov` selection parameter on `file_rasterizeroutput`, and enable 16-bpc PNG/TIFF + HDR EXR in GUI `saveAs`.

### 1.3 VCM, Spectral Media & Coat Transport Completions

- **VCM Completions ([VCM.md](VCM.md), [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)):**
  - Add `directional_light` NEE and light-subpath emission support in `VCMIntegrator` (currently PT/BDPT support directional lights while VCM drops them).
  - Add a configurable memory cap (`max_stored_light_vertices`) on surface `LightVertexStore` matching `LightMediumVertexStore` (`10M` cap) to bound RAM at ultra-high resolutions.
  - Close VCM volumetric single-scatter environment-IBL MIS weighting (`EnvLightBalanceTest` strict residual notes).
  - Evaluate hero-wavelength `LightVertexNM` spectral photon storage ([VCM_SPECTRAL_PHOTON_STORE_DESIGN.md](VCM_SPECTRAL_PHOTON_STORE_DESIGN.md)) if narrowband/dispersive VCM caustics become a priority.
- **Spectral Participating Media & Equi-Angular BDPT/VCM:**
  - Extend `IPhaseFunction` with `EvaluateNM` / wavelength-dependent Mie & Rayleigh phase functions and per-wavelength extinction/albedo in `HeterogeneousMedium` ([IMPROVEMENTS.md](IMPROVEMENTS.md) §17).
  - Bring equi-angular distance sampling (currently in PT `MediumTransport`) to BDPT and VCM medium walks for point/spot lights embedded in fog.
- **Absorbing / Tinted Clearcoat Presets & Importer Surfacing:**
  - `coated_material` already supports `coat_thickness`, `coat_absorption`, and `coat_tint` (`DL-342`); surface measured spectral extinction curves ($\sigma_a(\lambda)$) and absorbing-coat presets across glTF/Blender importers, GUI material presets, and vitreous enamel authoring ([VITREOUS_ENAMEL.md](VITREOUS_ENAMEL.md) Gap G1).

### 1.4 Extended SMS Performance (`DL-500`) & Coverage Extensions

- **Optimize `sms_extended` Area-Emitter Cost (`DL-500`):** Extended SMS (`sms_extended TRUE`) solves multi-bounce R/T caustics across RGB/NM/HWSS cleanly, but area-emitter estimator B + `ClassifyExtendedChain` over 6 canonical seeds currently runs ~`9–17×` slower than `sms_extended FALSE`. Profile and prune redundant canonical-seed Newton walks (early-exit bounding / shared Jacobian factorization).
- **SMS Participating Media & Alpha Coverage in Extended Mode:** Lift the current `sms_extended` restrictions on participating media and non-opaque `alpha_coverage` casters ([SMS_EXTENDED_DESIGN.md](SMS_EXTENDED_DESIGN.md)), and evaluate `k=1` Specular Polynomials exact root enumeration (Zhdan et al. 2025, [SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md)) for single-bounce glints/caustics.

---

## Tier 2 — GUI, Spectral Differentiators, Blender & Platform Parity

Features that turn RISE's spectral and physical differentiators into frictionless interactive workflows:

### 2.1 Approachability & Scene Editing Completion ([GUI_ROADMAP.md](GUI_ROADMAP.md), [gui/OPEN_ITEMS.md](gui/OPEN_ITEMS.md))

1. **Zero-Config Studio Startup (`A1`):** Ship a curated `default_studio.RISEscene` (shaderball + 3-point lighting + neutral cyclorama + camera presets) reachable in one click from `StartScreenView` / `StartScreenWidget`.
2. **Viewport Drag-and-Drop (`A3`) & Bundled Asset Browser (`A5`):** Drag materials from the palette/graph onto viewport objects (raycast hit → `setObjectParameter("material", ...)`), drag `.hdr`/`.exr` files onto the sky to wire IBL, and add a searchable bundled CC0 material/HDRI/model shelf.
3. **Entity Creation & Inspector Gaps ([gui/OPEN_ITEMS.md](gui/OPEN_ITEMS.md) §§2, 4):**
   - Support non-16:9 aspect presets and inserting a `film` chunk when absent (`applyFilmPreset`).
   - Add editable Hosek–Wilkie sky parameters (`turbidity`, `ground_albedo`, `sun_direction`) to `EnvironmentPanel` alongside HDRI mode.
   - Add `Category::Geometry` creation templates (sphere, box, torus, cylinder, clipped plane, SDF, displaced), surface `Shader` and `Modifier` categories in the Outliner, and wire atomic glTF/OBJ mesh import into an open `.RISEscene` ([gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md) §11).

### 2.2 MaterialX Import (`C3`) & Remaining Spectral Differentiators (`D1`, `D4–D7`)

Slices 1–6 of the Material Editor and Spectral Differentiators (`D1` Kelvin/Sellmeier, `D2` measured metals, `D3` thin-film interference strip) have already shipped ([gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md), [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md)). Remaining high-leverage slices:

1. **`D1` Remainder — CSV SPD Import & Monochromatic Wavelength Slider:** Drag-import `.csv` / `.spd` spectral curves onto painter/function nodes with an inline 380–780 nm SPD plot, plus a single-wavelength laser/monochromatic source helper.
2. **`D4` & `D6` — HDR Scopes, Gamut Clipping & Pixel Loupe:** Live HDR waveform, RGB parade, Rec.709/P3 gamut-warning overlay, exposure false-color EV mode, and spectral/linear-RGB pixel inspector loupe (`gui/RENDER_MODES.md` Phase 2).
3. **`D5` — Pre-Render Auto-Router Diagnostic Overlay:** Visualize `auto_rasterizer`'s Tier-2 probe metrics (caustic merge share, indirect ratio, resolved PT/BDPT/VCM choice) directly in the viewport HUD.
4. **`D7` — Interactive Post-Render Light Mix:** Per-light-group FrameStore AOV buffers allowing real-time intensity/tint re-balancing after a single path-traced render without re-tracing.
5. **`C3` — MaterialX (`.mtlx`) Import:** Translate `standard_surface` and `open_pbr_surface` graphs into native RISE `ggx_material` / `coated_material` / `fabric_material` / painter chains.

### 2.3 Android Tier-A Completion, Linux TLS & Blender Viewport Bridge

1. **Android App Completion ([../android/README.md](../android/README.md)):**
   - Wire CST `.RISEscene` round-trip save through `RiseBridge.cpp` + Android SAF document picker.
   - Expose remaining Outliner/Inspector categories (`Material`, `Painter`, `Geometry`, `Options`) on tablet layouts.
   - Add an OkHttp/NDK `ITlsTransport` + Android Keystore bridge so the in-app LLM Agent tab can run on Android.
2. **Linux Headless TLS Transport:** Implement a `libcurl` or OpenSSL `ITlsTransport` backend (`TlsTransportCurl.cpp`) to replace `TlsTransportUnsupported.cpp` on Linux so `--agent-eval` live LLM runs and headless HTTP/agent workflows work natively on Linux CI/workstations.
3. **Blender 4.x Interactive Viewport & Area Lights ([../src/Blender/README.md](../src/Blender/README.md)):**
   - Implement `view_update` / `view_draw` interactive viewport rendering in the Blender add-on via `InteractivePelRasterizer` / progressive PT.
   - Export Blender `AREA` lights directly to RISE's native `rect_light` / disk luminaire instead of mesh-quad workarounds, and bridge Blender 4.2+ Principled Thin-Film (`thin_film_thickness` / `thin_film_ior`) onto `ggx_material`'s thin-film parameters.

---

## Tier 3 — Agentic Hardening, Expressive Authoring, Cameras & Architecture v2

Longer-horizon strategic tracks:

### 3.1 Agent Security, Schema Tooling & Editor Diagnostics

- **Security Boundary Hardening ([gui/AI_SECURITY_MODEL.md](gui/AI_SECURITY_MODEL.md)):** Land `ProjectRootJail` (`openat` verified-fd path confinement for scene/asset writes), `IOutboundUrlPolicy` (SSRF private-subnet blocklist for custom LLM endpoints), `SecretRedactor` over logs/transcripts, and first-egress vision/cloud consent dialogs on macOS and Windows.
- **Inline Validation Squiggles & Enum Codegen ([gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md), [gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md)):** Wire Tier-1 parse/reference/arity diagnostics into inline squiggle gutters in the raw `.RISEscene` text tab, add `SchemaConformanceTest`, and add `tools/gen_bridge_enums.py` so C++, Swift, Kotlin, and Qt bridge enums cannot drift.

### 3.2 Agent Context Lifetimes, Procedural Geometry & Eval Refresh

- **Context Compaction Extensions ([agentic-redesign/91-context-lifetimes-and-visible-detail.md](agentic-redesign/91-context-lifetimes-and-visible-detail.md), [agentic-redesign/71-context-compaction-design.md](agentic-redesign/71-context-compaction-design.md)):** Extend L1 superseded-observation compaction to `remove_chunks`, `finish_element`, and `place_element`, and add `S3` head-anchored edit-ledger summaries for 100+ turn scene-construction sessions.
- **Geometry Expressiveness & Hierarchy Speed ([agentic-redesign/85-geometry-expressiveness-candidates.md](agentic-redesign/85-geometry-expressiveness-candidates.md), [agentic-redesign/87-recursive-scene-graph.md](agentic-redesign/87-recursive-scene-graph.md)):**
  - Add `branch_geometry` / botanical L-system / space-colonization foliage (`C5`), architectural macro-builders (`C4`), and `extruded_poly_geometry`.
  - Memoize `ObjectManager::RebakeHierarchy` pointer plans so deep `group_object` animations/turntables avoid $O(N)$ rebuilds per frame.
- **Agent Eval Harness Refresh ([../evals/README.md](../evals/README.md)):** Add robust HDR percentile grading (`p95`/`p99` firefly-resistant luminance bands) and refresh `evals/RESULTS.md` across current frontier models (`gemini-4-argon`, `claude-sonnet-5`, `gpt-5.6-terra`) including the two `DL-60` replay scenarios (`altar_stress`, `rainwet_closeup`).

### 3.3 Specialized Cameras & Multi-Document Architecture

- **Cameras Roadmap Phases 2–7 ([CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md)):** Implement `equirectangular_camera` (360° lat-long IBL baking/VR), `cubemap_camera`, `dome_camera` (fulldome), `ods_camera` (omnidirectional stereo), optical vignette / polygonal aperture blades / Scheimpflug tilt-shift (`thinlens_camera` extensions), and multi-element `realistic_camera` ray tracing through tabular lens prescriptions.
- **Immutable Scene Snapshot v2 & Multi-Document `RenderCoordinator` ([agentic-redesign/20-derivation-engine.md](agentic-redesign/20-derivation-engine.md), [gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md)):** Only if multi-tab simultaneous scene editing is needed: decouple live viewport reads from `Job` mutation locks via immutable `DerivedScene` snapshots and process-wide `RenderCoordinator` thread-budget arbitration.
- **Legacy Isolation Prep ([LEGACY_DEPRECATION_ASSESSMENT.md](LEGACY_DEPRECATION_ASSESSMENT.md) `D13`/`D14`):** Decouple `PathTracingIntegrator` SSS exit continuations from `defaultshader` (`D13`) and migrate the `RAW1` mesh loader onto `TriangleMeshGeometryIndexed` (`D14`) so the frozen shader-op and non-indexed mesh surfaces have zero active-pipeline dependencies.
