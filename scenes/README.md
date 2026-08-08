# Scene Guide

This directory contains the authored scene assets used to exercise RISE. The top-level split is intentional:

- [FeatureBased/README.md](FeatureBased/README.md): curated showcase and torture scenes that are visually coherent and exercise multiple subsystems together
- [Tests/README.md](Tests/README.md): focused regression, baseline, comparison, and validation scenes
- `Benchmarks/`: frontier-model qualitative benchmark scenes for agent scene-building comparisons; each carries a provenance header (verbatim prompt, authoring model, date) and is held as the bar a state-of-the-art model reaches on that prompt
- `Internal/`: local or historical internal scenes that are not part of the curated public taxonomy

The completed public-corpus deduplication and placement record is
[SCENE_AUDIT_2026-07-24.md](SCENE_AUDIT_2026-07-24.md).

## Placement Rules

- Put a scene in `FeatureBased/` when its main value is presentation, multi-feature stress, or end-to-end showcase coverage.
- Put a scene in `Tests/` when its main value is verifying one feature, comparing two configurations, or catching regressions.
- If a new rendering feature is both user-facing and regression-sensitive, add one scene to each tree: one focused validation scene under `Tests/` and one stronger showcase scene under `FeatureBased/`.
- Avoid reintroducing generic one-feature folders under `FeatureBased/`. Cameras, painters, pixel filters, and similar isolated checks belong in `Tests/`.

## Authoring Conventions

Before writing a new scene from scratch — and especially before debugging one that renders unexpectedly (too dark, wrong orientation, washed-out colors, etc.) — read [docs/SCENE_CONVENTIONS.md](../docs/SCENE_CONVENTIONS.md) and follow the procedure in [docs/skills/effective-rise-scene-authoring.md](../docs/skills/effective-rise-scene-authoring.md). The conventions doc is the reference; the skill is the diagnostic procedure (Lambertian-control-sphere check, log inspection, chunk bisection).

The most common scene-authoring bug is `directional_light.direction` — RISE uses the FROM-surface-TO-light convention, NOT the shine-direction convention used by some foreign tools. Get this wrong and camera-facing surfaces render unlit.

## Root-Level Runtime Scene

`pr.RISEscene` remains at the root because the CLI loads it when launched
without an explicit scene path. It is a runtime default, not a taxonomy
example. The old include-era color fragments were removed after the native-v7
cutover; public scenes contain the required painter chunks inline.
