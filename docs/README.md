# RISE Documentation Guide

Start at [../README.md](../README.md) for the repository map. This directory
contains current technical references, active roadmaps, executed design
records, research notes, and historical snapshots. Those lifecycles are
different: a completed plan can remain useful without being current
architecture.

The latest repository-wide documentation audit is
[DOCUMENTATION_AUDIT.md](DOCUMENTATION_AUDIT.md).

## Lifecycle Labels

- **Current reference:** describes the code contributors should follow today.
- **Active roadmap:** contains work that has not all shipped; trust its status
  header and verify implementation claims in source/tests.
- **Executed design record:** retained to explain why shipped code has its
  shape. Read its status header before treating body checklists as pending.
- **Historical snapshot:** accurate only at its stated date.
- **Removal candidate:** useful content should be consolidated or code
  citations migrated before deletion. The audit tracks these; this index does
  not silently remove history.

## Current Technical References

- [ARCHITECTURE.md](ARCHITECTURE.md): scene/runtime ownership, render phases,
  snapshots, TLAS, OIDN/film behavior, and thread-safety invariants
- [PERFORMANCE.md](PERFORMANCE.md): topology-aware worker policy, benchmarking,
  and the load-bearing thread-priority rules
- [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md): canonical scene-authoring
  semantics and common portability traps
- [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md): rasterizer and
  integrator selection guide
- [MATERIALS.md](MATERIALS.md): material, BSDF/SPF, BSSRDF, and phase-function
  taxonomy
- [LIGHTS.md](LIGHTS.md): emitter and light-sampling architecture
- [MIS_HEURISTICS.md](MIS_HEURISTICS.md): PT/BDPT/VCM/MLT MIS choices
- [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md): surface-derivative
  contract
- [SMS.md](SMS.md): current Specular Manifold Sampling overview
- [VCM.md](VCM.md): current Vertex Connection and Merging overview
- [OIDN.md](OIDN.md): OIDN integration, backlog, and decisions
- [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md): current Rec.709 Jakob-Hanika LUT limits

The scene-load source of truth is outside this directory:
[../src/Library/Cst/Cst.cpp](../src/Library/Cst/Cst.cpp) owns lossless
parse/derive and
[../src/Library/Parsers/ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)
owns registered chunks and descriptors. The retired
`AsciiSceneParser.cpp` streaming front-end no longer exists.

## Editor, GUI, and Agent Surface

- [GUI_ROADMAP.md](GUI_ROADMAP.md): partially executed umbrella roadmap
- [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md): completed viewport
  execution record; its original Phase-6 save design is historical
- [ROUND_TRIP_SAVE_PLAN.md](ROUND_TRIP_SAVE_PLAN.md): historical byte-splice
  plan with a current header describing CST-native save
- [gui/REDESIGN_IMPLEMENTATION.md](gui/REDESIGN_IMPLEMENTATION.md): shipped
  macOS/Windows workspace redesign
- [gui/START_SCREEN.md](gui/START_SCREEN.md): shipped three-path start screen
- [gui/ENVIRONMENT_SECTION.md](gui/ENVIRONMENT_SECTION.md): shipped
  environment/HDRI editor
- [gui/RENDER_MODES.md](gui/RENDER_MODES.md): shipped render modes and N-up
  viewports, plus remaining refinements
- [agentic-redesign/00-OVERVIEW.md](agentic-redesign/00-OVERVIEW.md): agentic
  redesign map
- [agentic-redesign/50-agentic-surface.md](agentic-redesign/50-agentic-surface.md):
  current agent product/surface design and landing record
- [agentic-redesign/70-agent-eval-harness.md](agentic-redesign/70-agent-eval-harness.md):
  eval-harness design; operational usage lives in
  [../evals/README.md](../evals/README.md)

Several `docs/gui/` files began as design-only specs and are now partially or
substantially implemented. Their updated headers state what shipped and what
remains. [gui/CURRENT_STATE_AUDIT.md](gui/CURRENT_STATE_AUDIT.md) is a
historical 2026-06-19 snapshot, not present-day ground truth.

## Active or Deferred Roadmaps

- [IMPROVEMENTS.md](IMPROVEMENTS.md): rendering backlog and acceptance criteria
- [CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md): shipped Phase-1 camera
  infrastructure and future camera models
- [GLTF_IMPORT.md](GLTF_IMPORT.md): shipped mesh/scene/PBR import and deferred
  animation/extensions
- [FRAMESTORE_DESIGN.md](FRAMESTORE_DESIGN.md): substantially implemented
  FrameStore/HDR/AOV redesign and remaining compatibility/multichannel work
- [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md): current
  status; read before the older
  [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md)
- [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md): open spectral parity
  tracker
- [VCM_SPECTRAL_PHOTON_STORE_DESIGN.md](VCM_SPECTRAL_PHOTON_STORE_DESIGN.md):
  deferred/reviewed spectral VCM design
- [VITREOUS_ENAMEL.md](VITREOUS_ENAMEL.md): shipped hero/material work plus
  broader deferred enamel research
- [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md) and
  [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md):
  partially implemented material editing and future advanced/spectral UX
- [gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md): unimplemented
  process-wide render-arbitration design

## Executed Plans and Retrospectives

These are intentionally retained as design history. Read their status headers;
do not execute old phase lists blindly.

- BVH: [BVH_RETROSPECTIVE.md](BVH_RETROSPECTIVE.md) and
  [BVH_ACCELERATION_PLAN.md](BVH_ACCELERATION_PLAN.md)
- physically based pipeline:
  [PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md) and
  the three `PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_*.md` records
- auto-routing: [UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md),
  [UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md),
  [UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md), and
  [AUTO_RASTERIZER_DESIGN.md](AUTO_RASTERIZER_DESIGN.md)
- geometry/material changes:
  [DISPLACED_GEOMETRY_PLAN.md](DISPLACED_GEOMETRY_PLAN.md),
  [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md),
  [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md), and
  [ENAMEL_SPARKLE_BRDF.md](ENAMEL_SPARKLE_BRDF.md)
- investigations and postmortems:
  [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md),
  [NORMAL_USAGE_AUDIT.md](NORMAL_USAGE_AUDIT.md),
  [INTEGRATOR_BUGFIX_FINDINGS.md](INTEGRATOR_BUGFIX_FINDINGS.md),
  [PT_PEL_NM_ASYMMETRY_AUDIT.md](PT_PEL_NM_ASYMMETRY_AUDIT.md),
  [CAUSTIC_PHOTONMAP_NORMALIZATION.md](CAUSTIC_PHOTONMAP_NORMALIZATION.md),
  and [VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md)

The SMS, guilloché, physical-pipeline, pre-Phase-1, GUI, and agentic-redesign
clusters contain additional detailed execution records. The audit identifies
which clusters are good consolidation/removal candidates and which still have
live source-code citations.

## Engineering Skills

- [skills/](skills/): process skills for implementation review, adversarial
  review, bug-pattern audits, performance baselines, const correctness,
  numerical precision, SMS/MIS diagnosis, variance measurement, testing, scene
  authoring, and HDR animation output
- [../skills/agent/](../skills/agent/): runtime scene-building and observation
  skills consumed by the RISE agent

Read the matching skill before beginning work of that shape.

## Related Documentation Outside `docs/`

- [../AGENTS.md](../AGENTS.md): contributor invariants and change checklist
- [../CLAUDE.md](../CLAUDE.md): thin compatibility companion
- [../src/Library/README.md](../src/Library/README.md): core-library map
- [../src/Library/Interfaces/README.md](../src/Library/Interfaces/README.md):
  interface taxonomy
- [../src/Library/Parsers/README.md](../src/Library/Parsers/README.md): current
  CST/chunk language and legacy migration
- [../scenes/README.md](../scenes/README.md): scene taxonomy
- [../tests/README.md](../tests/README.md): executable-test workflow and
  representative test families
- [../evals/README.md](../evals/README.md): agent eval workflow
- [../README.txt](../README.txt): frozen 2006 manual

Vendored dependency docs under `extlib/`, font licenses, generated logs, build
outputs, and ignored experiment notes are not maintained as RISE documentation.
