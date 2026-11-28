# Documentation Audit

This document tracks repository-wide audits of RISE's first-party documentation.
The consolidated implementation backlog lives in [ROADMAP.md](ROADMAP.md).

---

## Audit & Stale-Doc Cleanup — 2026-11-27 (`b6663f1af`)

Between the 2026-11-10 audit (`6946c4c82`) and `b6663f1af` (1,447 commits),
major subsystems landed across **HyLIoS hyperspectral skin** (P0–P8,
`hylios_skin_material` v2, `hyperspectral_radiometric_rasterizer` v2),
**Simulation** (`cloth.shell` m3a.8, `rigid.bodies` m6.2, `liquid.mac` m7m.1,
`fire.methane` v3 CPU/Metal), **Rough Dielectric BSDF**
(`rough_dielectric_material`), **Blender procedural / OSL / MixMaterial**
(C ABI v23, 42 native shader nodes), **unified make/Xcode build flags**
(`24423067f`), and **full Windows VS2022 + 503/503 CMake test suite builds**
(`b6663f1af`).

### 1. Aggressive Pruning & Citation Migration

Pruned **290** completed execution records, per-revision logs, closed debt memos,
and superseded snapshots (preserving git history and all runtime-read data/test
fixtures such as `docs/data/`, `docs/evidence/`, `rendered/fire_production_calibration/`,
`docs/INTERACTIVE_EDITOR_PLAN.md`, `docs/FIRE_SIM_DATASET_MANIFEST.md`,
`docs/FIRE_SMOKE_TRANSPORT_ADOPTION_R212.md`, and
`docs/FIRE_SMOKE_PRODUCER_KERNEL_CAMPAIGN.md`):

- **25** `BATCH7..12_*` closed debt-batch memos → migrated citations to [DEBT_LEDGER.md](DEBT_LEDGER.md)
- **83** closed `DL*` individual debt-closure memos → migrated citations to [DEBT_LEDGER.md](DEBT_LEDGER.md)
- **94** `FIRE_SMOKE_*_R2xx` per-revision experiment logs, commit maps, and superseded fire proposals → migrated citations to [FIRE_SMOKE_STATUS.md](FIRE_SMOKE_STATUS.md)
- **32** `SIMULATION_M0..M8` completed milestone design/results logs → migrated citations to [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) / [SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md) (retaining [SIMULATION_M7_REFERENCES.md](SIMULATION_M7_REFERENCES.md))
- **34** completed landing plans, dated audits, baselines, review-round logs, and retired removal candidates (`CONSOLIDATION_*`, `PRE_PHASE1_*`, `BLENDER_TRANSLATION_DESIGN_REVIEW_ROUND1..3`, `PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1..3`, `UNIFIED_INTEGRATOR_*`, `SMS_UNIFORM_SEEDING_*`, `SMS_EXTENDED_PHASE1..2_VALIDATION`, `GUILLOCHE_*`, `INTEGRATOR_REFACTOR_*`, and completed C++/BVH/displaced-geometry/round-trip-save plans)
- **8** superseded/completed `docs/gui/` snapshots and branch ledgers (current-state audit, design brief, redesign implementation, properties/references, open-items ledger, transaction model, environment section, and start-screen plan; remaining open items migrated to [GUI_ROADMAP.md](GUI_ROADMAP.md))
- **11** completed `docs/agentic-redesign/` cutover plans, closed arc logs, and superseded designs (`61`, `62`, `IMPLEMENTATION_SLICES`, `72–76`, `81`, `86`, `CREATIVITY_JOURNAL`)
- **3** dated scene/eval audit snapshots (`scenes/` 2026-07-24 scene audit, `evals/` render-grading convergence and dated results snapshots)

All **307** incoming citations across `src/`, `tests/`, `tools/`, `scripts/`, and
surviving Markdown files were migrated to canonical surviving docs.

### 2. Active Documentation Updates

- **`CLAUDE.md`**: Rewritten from ~359 KB of superseded chronological entries into a compact current-invariants companion.
- **`README.md` & `AGENTS.md`**: Updated Xcode `Deployment` fast-math parity (`24423067f`), `LTO=full|thin|off`, `hyperspectral_radiometric_rasterizer`, Blender ABI v23, `data/` layout, and simulation doc pointers.
- **Core references (`docs/`)**: Updated `README.md`, `ROADMAP.md`, `PERFORMANCE.md` (`ExpressionMemo::Tables` = 4016 B, removed nonexistent `maxStoredLightVertices`), `CROSS_PLATFORM_CI.md` (`b6663f1af` clean Windows build), `MATERIALS.md` (34 material chunks, BioSpec correction, `hylios_skin_material` v2, `rough_dielectric_material`/`mix_material`/`diffuse_transmission_material`/`null_boundary_material`), `RENDERING_INTEGRATORS.md` (13 rasterizers: 9 active + 4 frozen), `LIGHTS.md` (`rect_light`/`shape_light`), `SCENE_CONVENTIONS.md`, `VCM.md`, `OIDN.md`, `IMPROVEMENTS.md`, `SPECTRAL_PARITY_AUDIT.md`, `RELEASE_NOTES.md`, `BLENDER_MATERIAL_TRANSLATION.md` (ABI v23 & 42 native shader nodes), `BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md`, `MIX_MATERIAL_PATH_STATE_AO_DESIGN.md`, `HYLIOS_WORKPLAN.md`, `RADIOMETRIC_PRIMITIVES.md`, `HYLIOS_LOCAL_RESPONSE_CORE.md`, `HYLIOS_EQUATIONS.md`, `data/hylios/README.md`, `SIMULATION_IN_RISE_PROPOSAL.md`, `SIMULATION_SOLVER_CONTRACT.md`, `SIMULATION_GUI_STATUS.md`, `SIMULATION_PERFORMANCE.md`, and `FIRE_SMOKE_STATUS.md`.
- **Subsystem, GUI, Scene, Test & Eval docs**: Updated `src/Library/README.md` (`Acceleration/`, `Simulation/`, `HyperspectralRadiometricRasterizer`), `src/Library/Parsers/README.md` (193 keywords, removed `branching_threshold`), `src/Blender/README.md` (ABI v23), `android/README.md` & `build/cmake/rise-android/toolchain_notes.md` (484 TUs), `scenes/README.md` (removed nonexistent `Internal/`), `scenes/FeatureBased/README.md` (`HyLIoS/`), `scenes/Tests/README.md` (`Materials/HyLIoS/`, 10 new `Simulation/` scenes), `scenes/Tests/SDF/README.md`, `tests/README.md` (10 dual-mode extended suites, Windows MSVC build status), `evals/README.md` (OpenAI Responses API, `evals/references/`), `docs/GUI_ROADMAP.md`, `docs/gui/*`, `docs/agentic-redesign/00-OVERVIEW.md` / `00-CHARTER.md` / `50-agentic-surface.md` / `60-supersession-and-migration.md`, and `docs/skills/README.md` / `variance-measurement.md`.

### 3. Stale Code Comment Sweep (`src/`, `build/`, `tests/`)

- Updated stale `-ffast-math` / `-ffinite-math-only` comments that still claimed `std::isfinite`/`std::isnan` fold away on macOS.
- Updated stale `AsciiSceneParser` / `ParseAndLoadScene` / `DispatchChunkParameters` comments pointing to the deleted July 2026 streaming parser.
- Updated stale `BSPTreeSAH` comments on `ObjectManager` / `TriangleMeshGeometryIndexed` and `BilinearPatchGeometry` copy-paste comments.
- Updated stale `ROMMRGB` working-space comments to `Rec709RGB_Linear` / `Rec709RGBPel`.
- Removed stale `TODO` / "Phase N pending" / "not yet implemented" comments on already-shipped features (`VCMPelRasterizer`, `MaterialIntrospection`, `BVH.h`, `AccelerationConfig.h`, `PixelBasedRasterizerHelper.cpp`, `RISE_API.h`, `ViewportBridge.h`, `RISEViewportBridge.h`, `SceneEditor.cpp`, `SceneEditController.h`, `AgentSession.h`, `FabricPresets.h`, `HairBSDF.h`, `FrameSink.h`, `ThinFilmBRDFTest.cpp`).
- Fixed stale hardcoded counts (`AgentMcpAdapter.cpp`, `AgentRpc.h`, `SceneTextEdit.cpp`, `SceneEditorSuggestionsTest.cpp`), broken/non-existent doc paths in comments (`CAMERA_CONTROLS_UX.md`, `BVH_CLEANUP_AND_NEXT.md`, `abi-preserving-api-evolution.md`, `docs/skills/procedural-textures.md`, `eval-hosted-provider-keys.md`, `memory/feedback_no_default_params.md`), and pruned-test citations.

---

## Earlier Audits (Summary)

- **2026-11-10 (`6946c4c82`)**: Updated root guides, rendering/material/spectral docs, GUI/agentic headers, and `DEBT_LEDGER.md` counts; fixed all relative Markdown links.
- **2026-07-24 (`1514297d`)**: First repository-wide audit post-CST cutover; replaced `AsciiSceneParser` doc references, introduced lifecycle labels in `docs/README.md`, and established the safe citation-migration cleanup sequence.

## Ongoing Maintenance Rules

- Generate test, chunk, and tool counts from the tree rather than hand-maintaining static counts in prose whenever possible.
- Require every plan document to state its lifecycle status (`proposed`, `active`, `partially implemented`, `shipped`, `superseded`) at the top.
- Cite stable reference docs (`ARCHITECTURE.md`, `MATERIALS.md`, `DEBT_LEDGER.md`, `SIMULATION_SOLVER_CONTRACT.md`, etc.) from source comments instead of transient per-phase or per-revision logs.
- Run a repository-relative Markdown link scan and `SourceHygieneTest` whenever moving, renaming, or deleting documentation files.
