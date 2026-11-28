# RISE Documentation Guide

Start at [../README.md](../README.md) for the repository map. This directory
contains current technical references, active roadmaps, architectural design
records, and contributor workflow skills.

The latest repository-wide documentation audit is
[DOCUMENTATION_AUDIT.md](DOCUMENTATION_AUDIT.md), and the consolidated
implementation backlog is [ROADMAP.md](ROADMAP.md).

## Lifecycle Labels

- **Current reference:** describes the code contributors should follow today.
- **Active roadmap:** contains work that has not all shipped; trust its status
  header and verify implementation claims in source/tests.
- **Shipped design record:** retained to explain why shipped code has its shape.
  Read its status header before treating body checklists as pending.

## Current Technical References

### Core Architecture, Build & Conventions

- [RELEASE_NOTES.md](RELEASE_NOTES.md): user-visible changes and asset regeneration requirements
- [ARCHITECTURE.md](ARCHITECTURE.md): scene/runtime ownership, render phases, snapshots, TLAS, OIDN/film behavior, and thread-safety invariants
- [PERFORMANCE.md](PERFORMANCE.md): topology-aware worker policy, benchmarking, and load-bearing thread-priority rules
- [BUILD_FLAGS.md](BUILD_FLAGS.md): measured `make` and Xcode fast-math / strict-FP32 / LTO flag parity (`LTO=full|thin|off`)
- [CROSS_PLATFORM_CI.md](CROSS_PLATFORM_CI.md): Windows (MSVC) and Android (NDK arm64) compile/build status and workflow
- [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md): canonical scene-authoring semantics and common portability traps
- [BEAUTY_ADAPTIVE_MOMENTS.md](BEAUTY_ADAPTIVE_MOMENTS.md): compensated beauty luminance moments, adaptive convergence, and producer parity
- [DEBT_LEDGER.md](DEBT_LEDGER.md): repository-wide correctness debt ledger (361 `DL-*` rows: 339 closed, 3 fixed pending review, 1 active open `DL-634`, 18 parked; plus 23 `DL-R*` and 7 `DR-*`)
- [LEGACY_DEPRECATION_ASSESSMENT.md](LEGACY_DEPRECATION_ASSESSMENT.md): canonical frozen/deprecated tier policy for legacy shader-dispatch rasterizers, legacy non-PBR materials, and frozen legacy surfaces

### Rendering, Integrators, Lights & Spectral Transport

- [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md): rasterizer and integrator selection guide (9 active + 4 frozen rasterizer chunks)
- [AUTO_RASTERIZER_DESIGN.md](AUTO_RASTERIZER_DESIGN.md): shipped two-tier `auto_rasterizer` / `auto_spectral_rasterizer` router
- [LIGHTS.md](LIGHTS.md) and [ENVIRONMENT_SAMPLING.md](ENVIRONMENT_SAMPLING.md): emitter, `rect_light` / `shape_light`, light-sampling, and environment proposal contracts
- [PHYSICAL_LIGHT_SPECTRA.md](PHYSICAL_LIGHT_SPECTRA.md): physical spectra for delta lights
- [MIS_HEURISTICS.md](MIS_HEURISTICS.md): PT/BDPT/VCM/MLT MIS choices and subsurface-jump partitions
- [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md): $\eta^2$ basic-radiance scaling across refractive boundaries and graded-index media
- [SUBMERGED_CAMERA_IOR_SEEDING.md](SUBMERGED_CAMERA_IOR_SEEDING.md): `IORStackSeeding::SeedFromPoint` contract for cameras, lights, and photons
- [SMS.md](SMS.md) and [SMS_EXTENDED_DESIGN.md](SMS_EXTENDED_DESIGN.md): Specular Manifold Sampling overview and shipped `sms_extended` multi-bounce R/T solver across RGB/NM/HWSS (see also [SMS_TWO_STAGE_SOLVER.md](SMS_TWO_STAGE_SOLVER.md), [SMS_LEVENBERG_MARQUARDT.md](SMS_LEVENBERG_MARQUARDT.md), [SMS_PHOTON_REGIME.md](SMS_PHOTON_REGIME.md), [SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md](SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md), [SMS_ENERGY_LOSS_INVESTIGATION.md](SMS_ENERGY_LOSS_INVESTIGATION.md), [SMS_WRONG_TOPOLOGY_LITERATURE.md](SMS_WRONG_TOPOLOGY_LITERATURE.md), and [SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md))
- [VCM.md](VCM.md): Vertex Connection and Merging overview (surface + volumetric merging); see also [VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md) and [VCM_SPECTRAL_PHOTON_STORE_DESIGN.md](VCM_SPECTRAL_PHOTON_STORE_DESIGN.md)
- [OIDN.md](OIDN.md): OIDN integration, GPU backends, Auto quality policy, and remaining backlog
- [SPECTRAL_ILLUMINANT_CONVENTION.md](SPECTRAL_ILLUMINANT_CONVENTION.md), [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md), [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md), [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md), and [SPECTRAL_SOURCE_PROPOSALS.md](SPECTRAL_SOURCE_PROPOSALS.md): Linear Rec.709 / D65 working space, Stage C D65 illuminant-aware Jakob–Hanika LUT, `IScalarPainter` separation, and spectral source proposals
- [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md): open spectral parity tracker
- [RADIOMETRIC_PRIMITIVES.md](RADIOMETRIC_PRIMITIVES.md): fixed-wavelength scalar accumulation, checkpoints, and raw EXR primitives (wired into `hyperspectral_radiometric_rasterizer`)

### Materials, Geometry & Shading Signals

- [MATERIALS.md](MATERIALS.md): material, BSDF/SPF, BSSRDF, and phase-function taxonomy (34 material chunks)
- [ROUGH_DIELECTRIC_ENERGY.md](ROUGH_DIELECTRIC_ENERGY.md): reciprocal two-sided `rough_dielectric_material` energy compensation, bake, sampling, and validation
- [ALPHA_COVERAGE.md](ALPHA_COVERAGE.md): scalar material intersection coverage (`alpha_coverage`, `alpha_mode`, `alpha_cutoff`), CSG inheritance, and SSS/SMS endpoint rules
- [THIN_FILM_INTERFERENCE.md](THIN_FILM_INTERFERENCE.md): Airy thin-film interference (`ggx_material` `fresnel_mode thinfilm` and `dielectric_material` `ar_layer`)
- [WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md): `coated_material` wetness darkening, `add_wetness` verb, and `sample()` / `sample_scalar()` rules
- [RELIEF_MODIFIER_DESIGN.md](RELIEF_MODIFIER_DESIGN.md) and [ENAMEL_SPARKLE_BRDF.md](ENAMEL_SPARKLE_BRDF.md): `relief_modifier` and `glint_modifier` contracts
- [HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md): shipped Chiang BSDF, `hair_geometry`, groom generator, `.hair` import, Yan 2017 fur medulla, and Phase 4 stochastic LOD
- [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md): shipped `fabric_material`, `weave_material`, UV-aligned shading tangent, `make_fabric`, and `add_fuzz`
- [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md), [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md), [OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md](OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md), [CROSS_OBJECT_PROXIMITY_DESIGN.md](CROSS_OBJECT_PROXIMITY_DESIGN.md), [PROXIMITY_SHOWCASES.md](PROXIMITY_SHOWCASES.md), [TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md](TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md), and [SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md](SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md): surface derivatives, curvature, convexity/edge, cross-object `proximity(r)` / `interior(r)`, ray-differential `txFootprint`, and bidirectional neutral-read rules
- [BLENDER_MATERIAL_TRANSLATION.md](BLENDER_MATERIAL_TRANSLATION.md), [BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md](BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md), and [MIX_MATERIAL_PATH_STATE_AO_DESIGN.md](MIX_MATERIAL_PATH_STATE_AO_DESIGN.md): Blender 4.x–5.3 Principled/procedural/material translation (C ABI v23, 42 native shader nodes), `mix_material`, `diffuse_transmission_material`, `blended_modifier`, `osl_ao`, and path-dependence containment

### HyLIoS Hyperspectral Skin

- [HYLIOS_WORKPLAN.md](HYLIOS_WORKPLAN.md): shipped HyLIoS design, implementation summary (§13: P0–P8, `hylios_skin_material` v2 with `wavelength_nm` subset loading & `boundary_roughness_map`, `hyperspectral_radiometric_rasterizer` v2, nonlocal spatial transport), and remaining open items
- [HYLIOS_EQUATIONS.md](HYLIOS_EQUATIONS.md), [HYLIOS_COMPOSITION.md](HYLIOS_COMPOSITION.md), [HYLIOS_COMPLEX_LAYOUT.md](HYLIOS_COMPLEX_LAYOUT.md), [HYLIOS_OVERLAP.md](HYLIOS_OVERLAP.md), [HYLIOS_SURFACE_BOUNDARY.md](HYLIOS_SURFACE_BOUNDARY.md), [HYLIOS_FORWARD_TRANSPORT.md](HYLIOS_FORWARD_TRANSPORT.md), and [HYLIOS_TRANSPORT_DECISIONS.md](HYLIOS_TRANSPORT_DECISIONS.md): physical transport kernel, optical composition, explicit complexes, and surface fold boundary
- [HYLIOS_LOCAL_RESPONSE_CORE.md](HYLIOS_LOCAL_RESPONSE_CORE.md), [HYLIOS_LOCAL_ASSET_V1.md](HYLIOS_LOCAL_ASSET_V1.md), [HYLIOS_LOCAL_BAKE_V1.md](HYLIOS_LOCAL_BAKE_V1.md), [HYLIOS_EXPERIMENT_PREPARATION.md](HYLIOS_EXPERIMENT_PREPARATION.md), [HYLIOS_MEASUREMENT.md](HYLIOS_MEASUREMENT.md), [HYLIOS_MEASUREMENT_DISTRIBUTIONS.md](HYLIOS_MEASUREMENT_DISTRIBUTIONS.md), [HYLIOS_ANGULAR_LINEAGE.md](HYLIOS_ANGULAR_LINEAGE.md), [HYLIOS_COUPLED_LINEAGE_PROOF.md](HYLIOS_COUPLED_LINEAGE_PROOF.md), [HYLIOS_COUPLED_LINEAGE_TRANSITIONS.md](HYLIOS_COUPLED_LINEAGE_TRANSITIONS.md), and [HYLIOS_RESEARCH_NOTES.md](HYLIOS_RESEARCH_NOTES.md): local response baking/assets, measurement CLI, angular lineage proofs, and research evidence

### Simulation & Fire/Smoke

- [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md): frozen v1 solver contract covering `fire.methane` (v3), `cloth.shell` (m3a.8), `rigid.bodies` (m6.2), and `liquid.mac` (m7m.1)
- [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md): simulation architecture, scene chunks, and execution status (§5)
- [SIMULATION_GUI_STATUS.md](SIMULATION_GUI_STATUS.md): macOS/Windows/agent simulation integration status
- [SIMULATION_PERFORMANCE.md](SIMULATION_PERFORMANCE.md): simulation performance measurements and tuning notes
- [SIMULATION_FIELD_FORMAT.md](SIMULATION_FIELD_FORMAT.md), [SIMULATION_GRID_FORMAT.md](SIMULATION_GRID_FORMAT.md), [SIMULATION_MESH_FORMAT.md](SIMULATION_MESH_FORMAT.md), [SIMULATION_POINTS_FORMAT.md](SIMULATION_POINTS_FORMAT.md), [SIMULATION_TRANSFORM_FORMAT.md](SIMULATION_TRANSFORM_FORMAT.md), and [SIMULATION_M7_REFERENCES.md](SIMULATION_M7_REFERENCES.md): native cache payload formats and liquid physics references
- [FIRE_SMOKE_STATUS.md](FIRE_SMOKE_STATUS.md): current fire/smoke state, open problems, and operating rules (start here for fire work); see also [FIRE_SMOKE_DESIGN.md](FIRE_SMOKE_DESIGN.md), [FIRE_SMOKE_PRODUCTION_SOLVER.md](FIRE_SMOKE_PRODUCTION_SOLVER.md), [FIRE_SMOKE_SOLVER_SPEC.md](FIRE_SMOKE_SOLVER_SPEC.md), [FIRE_GAS_OPACITY_DATASET.md](FIRE_GAS_OPACITY_DATASET.md), [FIRE_CHEM_RECORDS_V1.md](FIRE_CHEM_RECORDS_V1.md), [FIRE_OPTICS_PRESET_V1.md](FIRE_OPTICS_PRESET_V1.md), [FIRE_OUTPUT_PROVENANCE_PIN_V1.md](FIRE_OUTPUT_PROVENANCE_PIN_V1.md), [FIRE_DATASET_PULL_MANIFEST.md](FIRE_DATASET_PULL_MANIFEST.md), [FIRE_SIM_DATASET_MANIFEST.md](FIRE_SIM_DATASET_MANIFEST.md), and [RENDER_PREPARATION_LIFECYCLE.md](RENDER_PREPARATION_LIFECYCLE.md)

The scene-load source of truth lives outside this directory:
[../src/Library/Cst/Cst.cpp](../src/Library/Cst/Cst.cpp) owns lossless
parse/derive and
[../src/Library/Parsers/ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)
owns registered chunks and descriptors (`ChunkDescriptor.h`, 193 keywords).

## Editor, GUI, and Agent Surface

- [GUI_ROADMAP.md](GUI_ROADMAP.md): umbrella GUI roadmap (desktop workspace, Material Editor Slices 1–6, `ObjectGraphCanvas`, 5-provider agent stack, macOS `SimulationsPanel` shipped; A1/A3/A5, C3, D4–D7, Windows `SimulationsPanel`, and Android parity gaps open)
- [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md): viewport execution record (pinned by `SourceHygieneTest`)
- [gui/ADR_DOCUMENT_FIRST_EDITOR.md](gui/ADR_DOCUMENT_FIRST_EDITOR.md), [gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md), and [gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md](gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md): document-first CST authority, shared-vs-platform boundaries, and transaction hardening
- [gui/APPROACHABILITY_FOUNDATION.md](gui/APPROACHABILITY_FOUNDATION.md), [gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md), [gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md), and [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md): presets/templates, cameras/named views, outliner CRUD, and two-tier validation
- [gui/NODE_GRAPH_CANVAS.md](gui/NODE_GRAPH_CANVAS.md), [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md), [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md), [gui/RENDER_MODES.md](gui/RENDER_MODES.md), and [gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md): Material Editor, `ObjectGraphCanvas`, spectral widgets, viewport render modes, and render coordination
- [gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md), [gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md), [gui/AI_SECURITY_MODEL.md](gui/AI_SECURITY_MODEL.md), and [AGENT_PERCEPTION.md](AGENT_PERCEPTION.md): in-app LLM runtime, 54-tool MCP / 49-tool chat surface, perception AOVs, and security hardening backlog
- [agentic-redesign/00-OVERVIEW.md](agentic-redesign/00-OVERVIEW.md), [agentic-redesign/00-CHARTER.md](agentic-redesign/00-CHARTER.md), [agentic-redesign/01-DECISIONS.md](agentic-redesign/01-DECISIONS.md), [agentic-redesign/10-scene-language-and-cst.md](agentic-redesign/10-scene-language-and-cst.md), [agentic-redesign/20-derivation-engine.md](agentic-redesign/20-derivation-engine.md), [agentic-redesign/21-stable-apply-and-resolver.md](agentic-redesign/21-stable-apply-and-resolver.md), [agentic-redesign/30-edit-model-and-history.md](agentic-redesign/30-edit-model-and-history.md), [agentic-redesign/40-dynamic-ui.md](agentic-redesign/40-dynamic-ui.md), [agentic-redesign/50-agentic-surface.md](agentic-redesign/50-agentic-surface.md), [agentic-redesign/60-supersession-and-migration.md](agentic-redesign/60-supersession-and-migration.md), [agentic-redesign/63-scene-variants-feature-spec.md](agentic-redesign/63-scene-variants-feature-spec.md), [agentic-redesign/70-agent-eval-harness.md](agentic-redesign/70-agent-eval-harness.md), [agentic-redesign/71-context-compaction-design.md](agentic-redesign/71-context-compaction-design.md), [agentic-redesign/83-staged-construction-plan.md](agentic-redesign/83-staged-construction-plan.md), [agentic-redesign/87-recursive-scene-graph.md](agentic-redesign/87-recursive-scene-graph.md), [agentic-redesign/88-procedural-texture-expressiveness-candidates.md](agentic-redesign/88-procedural-texture-expressiveness-candidates.md), and [agentic-redesign/91-context-lifetimes-and-visible-detail.md](agentic-redesign/91-context-lifetimes-and-visible-detail.md): CST/derivation/agentic architecture specs

## Active or Deferred Roadmaps & Retrospectives

- [ROADMAP.md](ROADMAP.md): consolidated, prioritized next-steps implementation roadmap across all subsystems
- [IMPROVEMENTS.md](IMPROVEMENTS.md): rendering gap criteria and closure tracker
- [CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md): shipped Phase-1 camera/film infrastructure and future panoramic/realistic camera models (Phases 2–7)
- [GLTF_IMPORT.md](GLTF_IMPORT.md): shipped mesh/scene/PBR import (9 KHR extensions) and deferred rough-transmission/dispersion/iridescence/animation
- [FRAMESTORE_DESIGN.md](FRAMESTORE_DESIGN.md): substantially implemented FrameStore/HDR/AOV pipeline; full primaries-aware AgX and multichannel EXR remain open
- [VITREOUS_ENAMEL.md](VITREOUS_ENAMEL.md): shipped hero watch/enamel work plus remaining rough-dielectric importer wiring (`G2`) and tinted-coat presets (`G1`)
- [PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md): physically based rendering pipeline plan and shipped landing summary
- [BVH_RETROSPECTIVE.md](BVH_RETROSPECTIVE.md), [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md), [INTEGRATOR_BUGFIX_FINDINGS.md](INTEGRATOR_BUGFIX_FINDINGS.md), and [PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md): retained architectural retrospectives and postmortems

## Engineering Skills

- [skills/](skills/): process skills for implementation review, adversarial review, bug-pattern audits, performance baselines, const correctness, numerical precision, SMS/MIS diagnosis, variance measurement, testing, scene authoring, and HDR animation output
- [../skills/agent/](../skills/agent/): runtime scene-building and observation skills consumed by the RISE agent

## Related Documentation Outside `docs/`

- [../AGENTS.md](../AGENTS.md): contributor invariants and change checklist
- [../CLAUDE.md](../CLAUDE.md): concise current-invariants companion
- [../src/Library/README.md](../src/Library/README.md): core-library map
- [../src/Library/Interfaces/README.md](../src/Library/Interfaces/README.md): interface taxonomy
- [../src/Library/Parsers/README.md](../src/Library/Parsers/README.md): current CST/chunk language and legacy migration
- [../src/Blender/README.md](../src/Blender/README.md): Blender 4.x–5.3 add-on and C ABI v23 bridge
- [../android/README.md](../android/README.md): Android viewport/editor app
- [../scenes/README.md](../scenes/README.md): scene taxonomy
- [../tests/README.md](../tests/README.md): executable-test workflow and representative test families
- [../evals/README.md](../evals/README.md): agent eval workflow
- [../README.txt](../README.txt): frozen 2006 manual

Vendored dependency docs under `extlib/`, font licenses, generated logs, build outputs, and runtime-pinned fire calibration artifacts are not maintained as active RISE prose documentation.
