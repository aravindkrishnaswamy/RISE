# 63 — Light Configurations (CST-native, selectable scene lighting variants)

Spec for a first-class **named, selectable lighting configuration** feature — the principled CST-world
replacement for the imperative `> modify` command block (being **deprecated**). Surfaced by the P5 Slice-1
review (doc [62](62-model-b-p5-save-as-cst-plan.md) §"Slice 2 — render-affecting `>` directives") and
ratified-in-principle by the user (2026-06-28): *"`> modify` should be deprecated, but [the watch hero's]
'light configurations' … can we spec that out as a feature and build it properly in our CST world?"*

Status: **SPEC for ratification** (O1–O5 in §9). No code yet.

## 0. Summary

A **configuration** is a *named set of declarative property overrides* applied atop the base scene at derive
time. The base scene (no overrides) is the implicit default; named configurations (`night`, …) are alternates
the author/GUI/agent can pick between. Configurations are CST data — losslessly parsed, CST-editable, and
CST-saveable — so selecting one is a CST edit + re-derive, never an imperative post-parse command.

Motivating case: the `watch_dial` hero's day/night modes, today a *commented-out* `/* > modify … */` block.

## 1. Motivation — why `> modify` cannot survive into v7/CST

`> modify` is an **imperative, post-parse mutation** dispatched by `AsciiCommandParser`
([src/Library/Parsers/AsciiCommandParser.cpp](../../src/Library/Parsers/AsciiCommandParser.cpp)):
`SetObjectMaterial`, `SetMaterialEmissionScale`, a rasterizer `radiance_scale` override. In the Model-B/CST
model (canonical CST → derived Scene), `DeriveToJob` **silently skips every top-level `>` directive** (v7 has
no command layer). So a `> modify` scene CST-loads *without* its modifications — a silent mis-render. That is
exactly the P5 Slice-1 `IsNativeV7Document` finding: render-affecting `>` directives are now **refused** at
load (no silent mis-render), which makes the `> modify` use case un-loadable until it has a CST-native form.

This spec is that form.

## 2. The `> modify` surface this must subsume

| `> modify` form                                  | engine op                  | what it overrides                 |
|--------------------------------------------------|----------------------------|-----------------------------------|
| `> modify object <obj> material <mat>`           | `SetObjectMaterial`        | rebind an object's material        |
| `> modify material <mat> scale <v>`              | `SetMaterialEmissionScale` | rescale a luminaire's emission     |
| `> modify rasterizer radiance_scale <v>`         | rasterizer override        | scale the active rasterizer output |
| `> modify animation <name>`                      | select active animation    | (animation selection — see §7)     |

watch_dial's (commented) night block uses the first three: swap marker/hand objects to a `lume_glow_night`
material, zero the two daytime soft luminaires (`scale 0`), and scale the rasterizer radiance to `0.02`.

## 3. The feature

### 3.1 A `configuration` chunk (CST data)

```
configuration
{
    name                       night
    object_material            markerlume  lume_glow_night
    object_material            sdfhour     lume_glow_night
    object_material            sdfminute   lume_glow_night
    object_material            pin         lume_glow_night
    material_scale             soft_top_lum  0
    material_scale             soft_bot_lum  0
    rasterizer_radiance_scale  0.02
}
```

- A **named** set of overrides. Multiple `configuration` chunks per scene.
- Override line types (the §2 surface; the grammar is extensible): `object_material <obj> <mat>`,
  `material_scale <mat> <value>`, `rasterizer_radiance_scale <value>`.
- Every referenced name (`markerlume`, `lume_glow_night`, …) must resolve in the base scene — validated at
  derive (a dangling ref is a derive diagnostic, like any other ref).

### 3.2 Selecting the active configuration

- The **base scene** (no overrides) is the implicit **default** configuration.
- A stored, scene-level selection: a top-level `active_configuration <name>` line (round-trips through
  save — a panel-editable property that *is* storable, per the "prefer storable over read-only" rule).
- GUI: a **"Configurations" accordion** with a selector, mirroring the named-animation-paths accordion;
  CLI/agent: select by a CST edit to `active_configuration` (= `DocSetParamValue`) → re-derive.

### 3.3 Derive integration (Model-B)

`DeriveToJob` applies the base chunks, then — if an `active_configuration` is set — the **active
configuration's overrides**, using the same engine ops `> modify` used (`SetObjectMaterial`, …) but driven by
**data, not a command**. Switching configurations is a CST edit to `active_configuration` → re-derive (full or
incremental per [62](62-model-b-p5-save-as-cst-plan.md) D2; a handful of overrides is sub-millisecond). No `>`
layer is involved, so the scene stays CST-loadable / -editable / -saveable end-to-end.

## 4. Why an override-set, not N full scenes

A configuration is a small **delta** (3–7 overrides) atop a shared base, not a duplicated scene: an edit to
the shared geometry/materials applies to every configuration at once. Day and night share ~95 % of watch_dial;
duplicating the scene would fork that 95 %.

## 5. Generality (features must be general)

Named by **mechanism** (a configuration / override-set), not by scene. Not watch-specific and not strictly
lighting: a `configuration` overrides any of the supported property types; lighting (object-material swap +
emission scale + radiance) is merely the motivating set. New override kinds (camera, a material scalar, …)
extend the same chunk grammar without a new concept. "Light configuration" is the user-facing framing of the
common case; the construct is a general configuration.

## 6. Migrator path (P5 Slice 2)

The migrator converts a scene's `> modify` block to a `configuration` chunk (+ `active_configuration`). For
watch_dial, the **commented-out** `/* > modify … */` night block becomes a `configuration { name night … }`
that is *defined but not active* (`active_configuration` left at the day default), faithfully preserving
today's disabled-night-mode state while making night a one-selection-away CST scene. `> modify animation` →
the §7 animation-selection field. After the corpus convert + this feature land, `> modify` is removed (P5
Slice 6 / the v6 delete).

## 7. Relationship to named animations (shared pattern + shared deprecation)

Named-animation-paths already established the exact pattern this mirrors: **named chunks, one active, selected
via a GUI accordion** (and today via `> modify animation <name>`). Two consequences:

1. Light configurations should reuse that GUI/selection shape (a "Configurations" accordion beside
   "Animation").
2. `> modify animation` is itself a `> modify` form, so deprecating `> modify` also moves **animation
   selection** to a stored `active_animation <name>` field + the edit model. Tracked here for `> modify`
   deprecation completeness; the animation feature owns the field.

## 8. Phasing

- **P0 — scene language + derive.** `configuration` chunk + `active_configuration` (descriptor-driven parser
  per [Parsers/README](../../src/Library/Parsers/README.md); derive applies the active overrides). The three
  §2 override types. Tests: parse round-trip, derive-applies-overrides, dangling-ref diagnostic, base-default.
- **P1 — GUI.** A "Configurations" accordion + selector (mirror the Animation accordion); selecting re-derives.
- **P2 — migrator.** Convert `> modify` blocks → `configuration` + `active_configuration` (watch_dial night).
- **P3 — deprecate `> modify`.** Remove the command once the corpus is converted (rides P5 Slice 6).

## 9. Open questions (for ratification)

- **O1 — name.** `configuration` (general; recommended) vs `light_configuration` (scoped) vs
  `scene_variant` / `preset`. Recommend `configuration` — the construct is general; "light" is just the
  common case.
- **O2 — selection storage.** A stored top-level `active_configuration <name>` (recommended; round-trips,
  GUI/CLI override at render) vs a render-time-only selection (not saved).
- **O3 — P0 override set.** Just the §2 three (recommended; the `> modify` surface) vs adding camera /
  material-scalar overrides now.
- **O4 — composition.** Single-select (recommended; matches `> modify`; watch needs only day/night) vs
  stackable/inheriting configs (night + a separate macro).
- **O5 — default.** Implicit base-as-default (recommended) vs an explicit `configuration { name day }`.

## 10. Read next

- [62-model-b-p5-save-as-cst-plan.md](62-model-b-p5-save-as-cst-plan.md) — the P5 plan; Slice 2 owns the
  `> modify` → `configuration` migrator conversion + the `light_rr_threshold` easy-convert.
- [src/Library/Parsers/AsciiCommandParser.cpp](../../src/Library/Parsers/AsciiCommandParser.cpp) — the
  `> modify` / `> set` dispatch this subsumes.
- [src/Library/Parsers/README.md](../../src/Library/Parsers/README.md) — how to add the `configuration` chunk
  (descriptor-driven `IAsciiChunkParser`).
