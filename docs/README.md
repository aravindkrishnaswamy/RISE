# RISE Documentation Guide

Start at [../README.md](../README.md) for the repository map. This directory
contains current technical references, active roadmaps, executed design
records, research notes, and historical snapshots. Those lifecycles are
different: a completed plan can remain useful without being current
architecture.

The latest repository-wide documentation audit is
[DOCUMENTATION_AUDIT.md](DOCUMENTATION_AUDIT.md), and the consolidated
implementation backlog is [ROADMAP.md](ROADMAP.md).

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
- [ALPHA_COVERAGE.md](ALPHA_COVERAGE.md): scalar material intersection coverage
  (`alpha_coverage`, `alpha_mode`, `alpha_cutoff`), CSG inheritance, SSS/SMS
  endpoint rules
- [THIN_FILM_INTERFERENCE.md](THIN_FILM_INTERFERENCE.md): Airy thin-film
  interference (`ggx_material` `fresnel_mode thinfilm` and `dielectric_material`
  `ar_layer`)
- [WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md): `coated_material` wetness
  darkening, `add_wetness` verb, and `sample()` / `sample_scalar()` rules
- [RELIEF_MODIFIER_DESIGN.md](RELIEF_MODIFIER_DESIGN.md): height/bump/normal
  `relief_modifier` and glint modifier contracts
- [BLENDER_MATERIAL_TRANSLATION.md](BLENDER_MATERIAL_TRANSLATION.md): Principled
  BSDF, Hair, Sheen, Coat, SSS, Alpha, and Tangent translation (ABI v17)
- [LIGHTS.md](LIGHTS.md): emitter and light-sampling architecture
- [MIS_HEURISTICS.md](MIS_HEURISTICS.md): PT/BDPT/VCM/MLT MIS choices and
  subsurface-jump partitions
- [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md): $\eta^2$
  basic-radiance scaling across refractive boundaries and graded-index media
- [SUBMERGED_CAMERA_IOR_SEEDING.md](SUBMERGED_CAMERA_IOR_SEEDING.md):
  `IORStackSeeding::SeedFromPoint` contract for cameras, lights, and photons
- [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md): surface-derivative and
  patch Weingarten curvature contract
- [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md),
  [OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md](OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md),
  [CROSS_OBJECT_PROXIMITY_DESIGN.md](CROSS_OBJECT_PROXIMITY_DESIGN.md),
  [PROXIMITY_SHOWCASES.md](PROXIMITY_SHOWCASES.md), and
  [TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md](TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md):
  curvature, convexity/edge, cross-object `proximity(r)` / watertight
  `interior(r)`, and ray-differential `txFootprint` signals
- [SMS.md](SMS.md): current Specular Manifold Sampling overview
- [SMS_EXTENDED_DESIGN.md](SMS_EXTENDED_DESIGN.md),
  [SMS_EXTENDED_PHASE1_VALIDATION.md](SMS_EXTENDED_PHASE1_VALIDATION.md), and
  [SMS_EXTENDED_PHASE2_VALIDATION.md](SMS_EXTENDED_PHASE2_VALIDATION.md):
  shipped `sms_extended` multi-bounce R/T solver across Phases 1–4 (RGB, NM,
  and HWSS per-lane anchors)
- [VCM.md](VCM.md): current Vertex Connection and Merging overview (including
  surface and volumetric merging)
- [OIDN.md](OIDN.md): OIDN integration, GPU backends, Auto quality policy, and
  remaining backlog
- [SPECTRAL_ILLUMINANT_CONVENTION.md](SPECTRAL_ILLUMINANT_CONVENTION.md) and
  [JH_LUT_GAMUT.md](JH_LUT_GAMUT.md): Stage C D65 illuminant-aware Jakob-Hanika
  spectral uplift (`GetColorNM` vs `GetRadianceNM`) and historical Stage A
  gamut analysis
- [DEBT_LEDGER.md](DEBT_LEDGER.md): repository-wide correctness debt ledger
  (357 tracked `DL-*` rows: 339 closed, 18 parked, 0 active open)
- [LEGACY_DEPRECATION_ASSESSMENT.md](LEGACY_DEPRECATION_ASSESSMENT.md):
  canonical frozen/deprecated tier policy for legacy shader-dispatch rasterizers,
  legacy non-PBR materials, and legacy build/plugin surfaces

The scene-load source of truth is outside this directory:
[../src/Library/Cst/Cst.cpp](../src/Library/Cst/Cst.cpp) owns lossless
parse/derive and
[../src/Library/Parsers/ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)
owns registered chunks and descriptors. The retired
`AsciiSceneParser.cpp` streaming front-end no longer exists.

## Editor, GUI, and Agent Surface

- [GUI_ROADMAP.md](GUI_ROADMAP.md): umbrella GUI roadmap (Slices A2/A4, B1–B5,
  C1–C2, D1–D3, Material Editor Slices 1–6 shipped; A1/A3/A5, C3, D4–D7 open)
- [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md): completed viewport
  execution record; its original Phase-6 save design is historical
- [ROUND_TRIP_SAVE_PLAN.md](ROUND_TRIP_SAVE_PLAN.md): historical byte-splice
  plan with a current header describing CST-native save
- [gui/ADR_DOCUMENT_FIRST_EDITOR.md](gui/ADR_DOCUMENT_FIRST_EDITOR.md),
  [gui/CROSS_PLATFORM_ARCHITECTURE.md](gui/CROSS_PLATFORM_ARCHITECTURE.md),
  [gui/DESIGN_BRIEF.md](gui/DESIGN_BRIEF.md), and
  [gui/REDESIGN_IMPLEMENTATION.md](gui/REDESIGN_IMPLEMENTATION.md):
  document-first architecture and shipped macOS/Windows workspace redesign
- [gui/START_SCREEN.md](gui/START_SCREEN.md),
  [gui/APPROACHABILITY_FOUNDATION.md](gui/APPROACHABILITY_FOUNDATION.md),
  [gui/CAMERAS_AND_VIEWS.md](gui/CAMERAS_AND_VIEWS.md),
  [gui/ENTITY_CREATION.md](gui/ENTITY_CREATION.md),
  [gui/PROPERTIES_AND_REFERENCES.md](gui/PROPERTIES_AND_REFERENCES.md), and
  [gui/ENVIRONMENT_SECTION.md](gui/ENVIRONMENT_SECTION.md): start screen,
  presets/templates, cameras/named views, entity creation, inspector widgets,
  and environment/HDRI editor
- [gui/TRANSACTION_MODEL.md](gui/TRANSACTION_MODEL.md),
  [gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md](gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md),
  [gui/VALIDATION_ARCHITECTURE.md](gui/VALIDATION_ARCHITECTURE.md), and
  [gui/OPEN_ITEMS.md](gui/OPEN_ITEMS.md): exact-byte CST undo/redo, live
  validation, and remaining GUI open items
- [gui/NODE_GRAPH_CANVAS.md](gui/NODE_GRAPH_CANVAS.md),
  [gui/MATERIAL_EDITOR.md](gui/MATERIAL_EDITOR.md),
  [gui/SPECTRAL_DIFFERENTIATORS.md](gui/SPECTRAL_DIFFERENTIATORS.md), and
  [gui/RENDER_MODES.md](gui/RENDER_MODES.md): shipped Material Editor Slices
  1–6 (node graph, `ObjectGraphCanvas`, 3D shaderball preview, pipe-coloured
  sockets, Kelvin/Sellmeier/metals/thin-film UI), viewport render modes, and
  Phase-2 scope/AOV backlog
- [gui/LLM_AGENT_RUNTIME.md](gui/LLM_AGENT_RUNTIME.md),
  [gui/MCP_TOOL_SURFACE.md](gui/MCP_TOOL_SURFACE.md), and
  [gui/AI_SECURITY_MODEL.md](gui/AI_SECURITY_MODEL.md): in-app LLM runtime,
  43-tool MCP/RPC surface, and security hardening backlog
- [AGENT_PERCEPTION.md](AGENT_PERCEPTION.md) and
  [PERCEPTION_ATLAS_AB_RESULTS.md](PERCEPTION_ATLAS_AB_RESULTS.md): multi-view
  perception atlas and A/B evaluation record
- [agentic-redesign/00-OVERVIEW.md](agentic-redesign/00-OVERVIEW.md),
  [agentic-redesign/50-agentic-surface.md](agentic-redesign/50-agentic-surface.md),
  and [agentic-redesign/IMPLEMENTATION_SLICES.md](agentic-redesign/IMPLEMENTATION_SLICES.md):
  agentic redesign map (`00–63` CST/derive/UI/variants, `70–72` eval &
  compaction, `73–91` creative richness, staged build, grouping, recursive
  scene graph, shape operators, ratchet, and L1 context lifetimes)
- [agentic-redesign/70-agent-eval-harness.md](agentic-redesign/70-agent-eval-harness.md):
  eval-harness design; operational usage lives in
  [../evals/README.md](../evals/README.md)

Several `docs/gui/` files began as design-only specs and are now partially or
substantially implemented. Their updated headers state what shipped and what
remains. [gui/CURRENT_STATE_AUDIT.md](gui/CURRENT_STATE_AUDIT.md) is a
historical 2026-06-19 snapshot, not present-day ground truth.

## Active or Deferred Roadmaps

- [ROADMAP.md](ROADMAP.md): consolidated, prioritized next-steps implementation
  roadmap across all subsystems
- [IMPROVEMENTS.md](IMPROVEMENTS.md): rendering gap criteria and historical
  closure tracker
- [HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md): Phases 1–4 shipped (Chiang BSDF +
  `hair_geometry` + groom generator + guides + `.hair` import + Blender export +
  Yan 2017 fur medulla, `medulla_ratio` > 0 + Phase 4 stochastic strand
  thinning LOD)
- [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md): Phases 1–3 shipped
  2026-09-03 (`fabric_material` Charlie sheen + Kulla-Conty directional albedo,
  UV-aligned mesh shading tangent, `fabric` presets + `make_fabric`, glTF
  `KHR_materials_sheen`, `weave_material`, and `add_fuzz`)
- [CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md): shipped Phase-1 camera/film
  infrastructure and future panoramic/realistic camera models (Phases 2–7)
- [GLTF_IMPORT.md](GLTF_IMPORT.md): shipped mesh/scene/PBR import (9 KHR
  extensions) and deferred rough-transmission/dispersion/iridescence/animation
- [FRAMESTORE_DESIGN.md](FRAMESTORE_DESIGN.md): substantially implemented
  FrameStore/HDR/AOV pipeline; full primaries-aware AgX and multichannel EXR
  remain open
- [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md): current
  status; read before the older
  [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md)
- [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md): open spectral parity
  tracker
- [VCM_SPECTRAL_PHOTON_STORE_DESIGN.md](VCM_SPECTRAL_PHOTON_STORE_DESIGN.md):
  deferred/reviewed spectral VCM photon-store design
- [VITREOUS_ENAMEL.md](VITREOUS_ENAMEL.md): shipped hero watch/enamel work plus
  remaining rough-dielectric (`G2`) and tinted-coat (`G1`) research
- [gui/RENDER_COORDINATOR.md](gui/RENDER_COORDINATOR.md): deferred process-wide
  multi-document render-arbitration design

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
  [VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md),
  [PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md), and
  [SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md](SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md)
  (closes the BDPT/VCM/MLT geometry-signal neutral-read gap,
  GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 11)
- correctness-debt closures (all 16 landed 2026-09-12, `fc371041..fe826872`;
  each struck its own row in [DEBT_LEDGER.md](DEBT_LEDGER.md)):
  - Translucent material: [DL01_TRANSLUCENT_EXIT_WEIGHT.md](DL01_TRANSLUCENT_EXIT_WEIGHT.md)
    records the NM exit/backscatter lobe double-tau repair; [DL02_TRANSLUCENT_EXIT_DENSITY.md](DL02_TRANSLUCENT_EXIT_DENSITY.md)
    records the diffuse-exit Pdf sign-flip and NM/RGB shape-parity repair;
    [DL03_GUIDED_IOR_CONTINUATION.md](DL03_GUIDED_IOR_CONTINUATION.md) records
    the guided-continuation IOR-stack state repair. Three residuals these
    docs recorded as open were subsequently closed 2026-09-13 (debt-cleanup
    slice `translucent`, struck in place in the same files and in
    [DEBT_LEDGER.md](DEBT_LEDGER.md)): DL-39 (photon-map deposition
    accounted absorbed power as stored power), DL-45 (the diffuse exit
    re-emission ignored the geometric horizon under a tilted shading
    normal), and DL-46 (a camera/light origin already inside a closed
    translucent object was never seeded with its IOR-stack membership).
  - Subsurface scattering: [DL04_SSS_RADIANCE_DECISION.md](DL04_SSS_RADIANCE_DECISION.md)
    records the complete-event eta² convention pin and discriminating
    measurements; [DL48_SSS_NORMALIZATION.md](DL48_SSS_NORMALIZATION.md)
    records the Schlick-transmission `Sw` cosine-hemisphere normalization
    repair; [DL50_RANDOM_WALK_SURVIVAL.md](DL50_RANDOM_WALK_SURVIVAL.md)
    records the random-walk NM spectral free-flight survival double-count
    repair; [DL51_SUBSURFACE_EXIT_IOR.md](DL51_SUBSURFACE_EXIT_IOR.md)
    records the standalone non-absorbing SSS exit destination-IOR repair;
    [DL54_BSSRDF_PROJECTION_NORMAL.md](DL54_BSSRDF_PROJECTION_NORMAL.md)
    records the BSSRDF disk-projection geometric-normal repair;
    [DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md](DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md)
    records the RGB tiny-extinction fallback proposal-density repair;
    [DL57_RANDOM_WALK_DENSITY_CUTOFF.md](DL57_RANDOM_WALK_DENSITY_CUTOFF.md)
    records the finite-positive random-walk collision-density guard repair.
  - GGX / grazing dielectric Fresnel: [DL37_GGX_DIFFUSE_TRANSMISSION.md](DL37_GGX_DIFFUSE_TRANSMISSION.md)
    records the reciprocal interface-attenuated diffuse composition repair;
    [DL56_GRAZING_FRESNEL.md](DL56_GRAZING_FRESNEL.md) records the shared
    grazing dielectric-Fresnel repair and the distinct classification scope
    subsequently closed by DL-58; [DL58_MATCHED_INDEX_GRAZING.md](DL58_MATCHED_INDEX_GRAZING.md)
    records the subsequent matched-index Snell/Fresnel classification repair;
    [DL59_SMS_NORMAL_DERIVATIVE.md](DL59_SMS_NORMAL_DERIVATIVE.md) records
    the separate, test-only unequal-index normal-derivative convention
    correction found during DL-58 review;
    [DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md](DL62_DL64_GGX_SAMPLE_EVAL_MISMATCH.md)
    (2026-09-13) records the GGX glossy-filter roughness parity repair
    (`GGXBRDF::value`/`valueNM` now widen by `ri.glossyFilterWidth` like
    their SPF twins) and the zero-F0 Schlick lobe-selection-weight repair
    (derived from the hemispherical Fresnel average instead of raw F0),
    plus a follow-up DL-63 section (2026-09-14) recording the
    height-correlated-G2 multiscatter-compensation fix and the DL-65
    CookTorrance/Schlick glossy-filter sibling fix (both CLOSED).
  - CSG / signals: [DL34_UNION_INTERIOR_DEPTH.md](DL34_UNION_INTERIOR_DEPTH.md)
    records the published union-overlap interior-depth repair, conservative
    scope, and baseline gate residuals; [DL36_EMITTER_NEIGHBOUR_PIN.md](DL36_EMITTER_NEIGHBOUR_PIN.md)
    records the bounded same-luminary emitter-neighbour consistency pin
    (a regression added for existing, intentionally retained, behavior).
  - Subsequent closures (2026-09-13, debt-sssenv slice, not part of the
    16 above): [DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md](DL52_BSSRDF_PLANAR_PROBE_ORIGIN.md)
    records the disk-projection single-chord probe repair (a normal-axis
    probe no longer skips a coplanar near surface); [DL53_RAYCASTER_ENV_ESCAPE_MIS.md](DL53_RAYCASTER_ENV_ESCAPE_MIS.md)
    records the `RayCaster` explicit-global-map MIS-partner repair (the
    `RayCaster.cpp` analogue of PT_ENV_MIS_DOUBLECOUNT.md's integrator fix);
    [DL71_BSSRDF_PROBE_ENTRY_NORMAL.md](DL71_BSSRDF_PROBE_ENTRY_NORMAL.md)
    records a follow-on defect found gating DL-52's own closure (the
    single-chord probe reported an INVERTED entry normal on near-half
    hits for double-sided/flip-oriented geometry); [DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md](DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md)
    records a follow-on defect found gating DL-53's own closure (BSSRDF
    and internal volume continuations never set `bsdfTimesCos`, so
    `RayCasterEnvEscapeMISWeight`'s optimal-MIS training arm could never
    fire for them — a training-input gap, not a render-time weight bug).
  - Propagation of these 16 models into CLAUDE.md, ARCHITECTURE.md,
    SPECTRAL_PARITY_AUDIT.md and the rest of the canonical docs/skills tree
    is tracked separately from each slice's own DLxx record.

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
