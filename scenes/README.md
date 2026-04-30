# Scene Guide

This directory contains the authored scene assets used to exercise RISE. The top-level split is intentional:

- [FeatureBased/README.md](FeatureBased/README.md): curated showcase and torture scenes that are visually coherent and exercise multiple subsystems together
- [Tests/README.md](Tests/README.md): focused regression, baseline, comparison, and validation scenes
- `Internal/`: local or historical internal scenes that are not part of the curated public taxonomy

## Placement Rules

- Put a scene in `FeatureBased/` when its main value is presentation, multi-feature stress, or end-to-end showcase coverage.
- Put a scene in `Tests/` when its main value is verifying one feature, comparing two configurations, or catching regressions.
- If a new rendering feature is both user-facing and regression-sensitive, add one scene to each tree: one focused validation scene under `Tests/` and one stronger showcase scene under `FeatureBased/`.
- Avoid reintroducing generic one-feature folders under `FeatureBased/`. Cameras, painters, pixel filters, and similar isolated checks belong in `Tests/`.

## Authoring Conventions

Before writing a new scene from scratch — and especially before debugging one that renders unexpectedly (too dark, wrong orientation, washed-out colors, etc.) — read [docs/SCENE_CONVENTIONS.md](../docs/SCENE_CONVENTIONS.md) and follow the procedure in [docs/skills/effective-rise-scene-authoring.md](../docs/skills/effective-rise-scene-authoring.md). The conventions doc is the reference; the skill is the diagnostic procedure (Lambertian-control-sphere check, log inspection, chunk bisection).

The most common scene-authoring bug is `directional_light.direction` — RISE uses the FROM-surface-TO-light convention, NOT the shine-direction convention used by some foreign tools. Get this wrong and camera-facing surfaces render unlit.

## Root-Level Utilities

The root of `scenes/` still contains a few utility or historical files used by many scenes:

- `colors.RISEscript`: shared color definitions loader
- `standard_colors.RISEscene`, `povray_colors.RISEscene`: shared color scene fragments
- `iorstack.RISEscene`, `pr.RISEscene`: older standalone utility scenes

These are support assets, not the preferred place for new sample organization.
