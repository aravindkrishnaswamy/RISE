# RISE Documentation Guide

This directory holds focused design notes and planning docs. Start at [../README.md](../README.md) for the repo map, then use this file to decide which deeper document you actually need.

## Stable Reference Docs

- [ARCHITECTURE.md](ARCHITECTURE.md): focused deep dive on scene immutability, thread safety, render phases, and known exceptions. Read this when touching rasterizers, animation-time mutation, caches, or shared renderer state.
- [PERFORMANCE.md](PERFORMANCE.md): parallel-efficiency architecture and the load-bearing thread-priority policy. Read this before changing anything threading-related — the "good citizen" UTILITY-class default cost ~75% of throughput on Apple Silicon last time.
- [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md): canonical scene-authoring conventions (directional-light direction semantics, `power` units, transform precedence, V-axis flip, etc.). The reference; the matching procedural skill is [skills/effective-rise-scene-authoring.md](skills/effective-rise-scene-authoring.md).
- [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md): contract for `IGeometry::ComputeSurfaceDerivatives` and how SMS / future derivative-aware code must interpret its output.

## Subsystem Docs

- [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md): selection guide for the ten rasterizer chunks. Decision tree (PT / BDPT / VCM / MLT / shader-dispatch / spectral variants), the shader-dispatch vs. pure-integrator pipeline split, and the optional-feature support matrix (path guiding, adaptive sampling, SMS, optimal MIS, OIDN).
- [MATERIALS.md](MATERIALS.md): taxonomy of RISE materials (BRDFs, SPFs, BSSRDFs, phase functions, luminaires), sampling protocol (delta vs. continuum, area-measure PDFs), composition rules, and the checklist for adding a new BSDF.
- [LIGHTS.md](LIGHTS.md): emitter taxonomy (`ILight` vs. luminaire materials), the unified `LightSampler` pipeline (alias table, RIS, light BVH, environment importance sampling), MIS subtleties per selection mode, and the checklist for adding a new emitter.
- [SMS.md](SMS.md): Specular Manifold Sampling solver design, constraint formulations, MIS analysis (SMS vs BDPT path-space disjointness), and tuning guidance.
- [VCM.md](VCM.md): Vertex Connection and Merging integrator (BDPT + photon mapping under one MIS umbrella), recurrence design, post-pass conversion, and Veach-transparency handling for specular chains.
- [OIDN.md](OIDN.md): Intel Open Image Denoise integration — audit of current usage vs. upstream feature set, ranked improvement backlog with stable IDs, and decision log. Update entries in place as work lands.

## Active / Recent Plans

- [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md): current state of the PT/BDPT/VCM integrator deduplication effort. Phases 0/1/2a shipped; later phases deferred. **Read this before [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md)** — the plan is the design record; the status file is what's true today.
- [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md): original phased design for collapsing Pel/NM/HWSS duplication across the three integrators. Historical context for the in-flight refactor.
- [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md): six-phase plan for the always-on 3D viewport across macOS, Windows, and Android. Phases 1–5 shipped (Library + all three platforms + production-render integration); Phase 6 (round-trip save) not started.
- [GLTF_IMPORT.md](GLTF_IMPORT.md): glTF 2.0 import. Phase 1 (mesh) and Phase 2 (scene + PBR + tangent normal mapping) shipped; Phase 3+ (animation, KHR extensions) deferred. Implementation-status table at the top is the canonical view.

## Completed Plans / Retrospectives

These describe shipped work. Retained as historical design records; the body should not be read as a to-do list.

- [BVH_RETROSPECTIVE.md](BVH_RETROSPECTIVE.md): persistent record of the 2026-04-26→27 BVH replacement. SAH BVH2 + BVH4 SIMD collapse + float Möller-Trumbore is the production configuration. Supersedes the in-flight phase reports.
- [BVH_ACCELERATION_PLAN.md](BVH_ACCELERATION_PLAN.md): original architectural plan for the BVH replacement. Read [BVH_RETROSPECTIVE.md](BVH_RETROSPECTIVE.md) first; this is the design record that produced it.
- [DISPLACED_GEOMETRY_PLAN.md](DISPLACED_GEOMETRY_PLAN.md): plan for the `DisplacedGeometry` wrapper + `IGeometry::TessellateToMesh` contract. All 12 phases shipped 2026-04-18; one post-completion amendment (BezierPatch went analytical).
- [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md): postmortem on two variant MLT rasterizers (MMLT, PathMLT) that shipped briefly and were retired. Explains what was built, why it was motivated, the empirical data that killed them, and what workload would actually justify reviving them.

## Forward-Looking Plans

- [IMPROVEMENTS.md](IMPROVEMENTS.md): ranked backlog of rendering improvements (transport, materials, samplers, volumes), with acceptance criteria and starting files per item. Items 1–9 marked DONE; items 10–11 pending.
- [CAMERAS_ROADMAP.md](CAMERAS_ROADMAP.md): survey of production / research camera models beyond pinhole / ortho / fisheye / thin-lens, scored against RISE's spectral pipeline, plus a phased implementation roadmap (thin-lens enrichments → output-format cameras → ODS → realistic multi-element lens → polynomial-optics acceleration → diffraction & flare → sensor/shutter). Named-camera scaffolding has shipped; tier feature work has not started.

## Engineering Skills

- [skills/](skills/): process skills distilled from prior RISE sessions — multi-reviewer code review, perf work with baselines, ABI-preserving API evolution, const-correctness decision tree, precision-fix-the-formulation, SMS firefly diagnosis, BDPT/VCM MIS balance, variance measurement, effective scene authoring, write-highly-effective-tests. Read the matching skill BEFORE starting a task of that shape. Model-agnostic markdown; Claude Code auto-invokes via `.claude/skills/` shims.

## Related Docs Outside `docs/`

- [../README.md](../README.md): top-level repo map and canonical command quick reference
- [../AGENTS.md](../AGENTS.md): concise working guide for LLM contributors
- [../CLAUDE.md](../CLAUDE.md): thin Claude-compatible shim that points back to the shared docs
- [../scenes/README.md](../scenes/README.md): overall scene taxonomy and placement rules
- [../src/Library/README.md](../src/Library/README.md): core library structure and extension checklist
- [../src/Library/Parsers/README.md](../src/Library/Parsers/README.md): scene language and parser rules
- [../scenes/FeatureBased/README.md](../scenes/FeatureBased/README.md): curated showcase and torture scenes
- [../scenes/Tests/README.md](../scenes/Tests/README.md): focused regression and comparison scenes
- [../tests/README.md](../tests/README.md): executable tests and validation scenes
- [../README.txt](../README.txt): historical manual and user-facing background

## Organization Rule Of Thumb

- Put repo entry-point guidance in `README.md`.
- Put tool-specific shims in `AGENTS.md` or `CLAUDE.md`, but keep them thin and link back to shared source docs instead of duplicating long explanations.
- Put subsystem navigation close to the code in subtree `README.md` files.
- Put focused design notes and roadmaps in `docs/`.
- When a plan ships, leave the plan in place but mark it as completed in the body and move its index entry to "Completed Plans / Retrospectives" above. Do not rewrite history.
