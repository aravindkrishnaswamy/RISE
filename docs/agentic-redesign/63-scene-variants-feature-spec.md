# 63 — Scene Variants (CST-native, named, selectable scene overlays)

Spec for a first-class **named, selectable scene variant** feature — the principled CST-world replacement for
the imperative `> modify` command block (being **deprecated**). Surfaced by the P5 Slice-1 review (doc
[62](62-model-b-p5-save-as-cst-plan.md)) and shaped by the user's decisions (2026-06-28): *name it
`scene_variant`, make it flexible enough to do almost anything, require a name (so it's UI-switchable), store a
selected active variant, single-select; a variant can set the active camera and specify materials.*

Status: **SPEC for ratification** (§9). No code yet.

## 0. Summary

A **`scene_variant`** is a *named overlay* on the base scene: a small, declarative set of overrides applied at
derive time. The base scene (no variant) is the implicit default; named variants (`night`, `macro`, …) are
alternates the author / GUI / agent picks between via a stored `active_scene_variant`. Variants are CST data —
losslessly parsed, CST-editable, CST-saveable — so switching one is a CST edit + re-derive, never an
imperative `>` command. The name is **required** (it is the UI handle).

Motivating case: the `watch_dial` hero's day/night modes, today a *commented-out* `/* > modify … */` block.

## 1. Why `> modify` cannot survive into v7/CST

`> modify` is an **imperative, post-parse mutation** dispatched by `AsciiCommandParser`
([src/Library/Parsers/AsciiCommandParser.cpp](../../src/Library/Parsers/AsciiCommandParser.cpp)). In Model-B
(canonical CST → derived Scene), `DeriveToJob` **silently skips every top-level `>` directive** (v7 has no
command layer), so a `> modify` scene CST-loads *without* its modifications — a silent mis-render. The P5
Slice-1 guard now **refuses** render-affecting `>` directives at load (no silent mis-render), which leaves the
`> modify` use case with no CST-native form. This spec is that form, declaratively.

## 2. The watch night-mode, dissected (the "reasons")

The (commented) night block does three things; investigating each shows all three reduce to **material
overrides + an active-camera choice** — the user's exact proposal — with the new named-camera support
([Job::SetActiveCamera](../../src/Library/Job.h)) and luminaire materials:

| `> modify` op (today)                         | intent                                   | scene_variant form |
|-----------------------------------------------|------------------------------------------|--------------------|
| `rasterizer radiance_scale 0.02`              | darken — the lume is the only night light | **dropped** (a workaround): the night exposure folds into the night luminaire material's `scale`, so no rasterizer override is needed |
| `material soft_top_lum scale 0` (×2)          | turn off the day studio lights            | a **material override**: the `night` variant redefines those luminaire materials to non-emissive |
| `object <o> material lume_glow_night` (×4)     | markers / hands / pin glow blue-green     | a **material override**: the `night` variant redefines those objects' materials to the glow material |

Why the object-rebind becomes a material override: the night objects already carry **dedicated** lume
materials (markers + pin share `lume_white`; both are meant to glow), so redefining the *material* achieves
the same visual as rebinding the *object*, without a separate rebind primitive. The one authoring rule this
implies (§5) is mild and the watch already mostly follows it.

Conclusion: the old object-rebind / material-scale / rasterizer-radiance `>` ops are **not** distinct
primitives v7 needs — they are (material overrides) + (a foldable exposure workaround). The two primitives the
feature actually needs are **material overrides** and **active-camera selection**.

## 3. The feature

### 3.1 Declaring a variant — `scene_variant` chunk

```
scene_variant
{
    name           night          # REQUIRED — the UI handle / switch key
    active_camera   night_hero     # optional: select a named camera (-> Job::SetActiveCamera)
}
```

- A variant is a **named overlay**. The `name` is mandatory; an unnamed `scene_variant` is a parse error.
- `active_camera <name>` (optional) selects which named camera is active when the variant is active — the
  watch has 7 cameras, so day/night/macro variants pick their view. Resolves to the existing
  `Job::SetActiveCamera`; nothing camera-specific beyond selection ("active camera but that's it").

### 3.2 Variant-scoped overrides — the `variant <name>` tag

Any overridable chunk may carry an optional `variant <name>` field, marking it as **belonging to that
variant**: when the variant is active, it **overrides the base chunk of the same name**; when inactive, it has
no effect. This keeps the flat-chunk grammar (no nested chunks) and is **general** — today materials, tomorrow
any overridable type ("flexible enough to do almost anything"):

```
# base (day) luminaire
lambertian_luminaire_material { name soft_top_lum   exitance pnt_soft        scale 1.0 }
# night override of the same material — off
lambertian_luminaire_material { name soft_top_lum   variant night   exitance pnt_soft   scale 0.0 }

# night override of the markers' material — glow
lambertian_luminaire_material { name lume_white      variant night   exitance pnt_lume_glow  scale 0.9 }
```

- A `variant night` chunk's `name` must match a base chunk (override) — a dangling override is a derive
  diagnostic. (Add-new — a `variant` chunk with a brand-new name — is allowed but only takes effect once
  something binds it; the override case is the common one.)
- This is the user's "specify new materials": variant-tagged material chunks redefine base materials for that
  variant. The same tag mechanism extends to other chunk types later without a new concept.

### 3.3 Selecting the active variant

- The base scene (no overrides) is the implicit **default**.
- A stored top-level **`active_scene_variant <name>`** selects one (round-trips through save — a
  panel-editable property that *is* storable). `none` / absent = the base default.
- **Single-select** (one active variant at a time — matches `> modify`; the watch needs only day/night).
- GUI: a **"Variants" accordion** with a selector, mirroring named-animation-paths; CLI/agent: a CST edit to
  `active_scene_variant` → re-derive.

### 3.4 Derive integration (Model-B)

`DeriveToJob` applies the base chunks; then, if `active_scene_variant` names a variant, it (a) applies that
variant's `variant`-tagged chunks as overrides (replace base same-named), and (b) applies the variant's
`active_camera` via `SetActiveCamera`. No `>` layer; CST-loadable / -editable / -saveable end-to-end.
Switching variants = a CST edit to `active_scene_variant` → re-derive (full or incremental per [62](62-model-b-p5-save-as-cst-plan.md) D2; a handful of overrides is sub-millisecond).

## 4. Why an overlay, not N full scenes

A variant is a small **delta** (a few overrides) atop a shared base; an edit to the shared geometry/materials
applies to every variant at once. Day and night share ~95 % of watch_dial.

## 5. Generality + the one authoring rule

- **General by mechanism:** the `variant <name>` tag is not material- or watch-specific — any overridable
  chunk type can carry it, so the feature grows to camera-params, rasterizer settings, object transforms, …
  without a new concept. Materials + active-camera are the P0 set (the two the watch needs).
- **Authoring rule (mild):** because overrides are by material *name*, an object you want a variant to restyle
  should use a material **dedicated** to it (not shared with objects that must stay unchanged). The watch
  already follows this for its lume objects; the only adjustment is giving the hour/minute hands their own
  lume materials. (If a future case needs a *targeted per-object* change with no dedicated material, an
  explicit `object_material` override can be added under the same variant mechanism — but it is **not** a P0
  primitive, per the decision to lead with material specification.)

## 6. Migrator path (P5 Slice 2)

The migrator converts a `> modify` block into a `scene_variant` + variant-tagged override chunks. For
watch_dial, the commented night block becomes a `scene_variant { name night … }` plus `variant night`
material overrides (day lights → off, lume objects → glow, the night exposure folded into the glow `scale`),
with `active_scene_variant` left at the day default — faithfully preserving today's disabled-night state while
making night one selection away. `> modify animation` → the §7 animation-selection field. `> modify` is
removed once the corpus is converted (P5 Slice 6).

## 7. Precedent + shared deprecation (named animations)

Named-animation-paths established the pattern this mirrors: **named chunks, one active, a GUI accordion** (and
today, selection via `> modify animation <name>`). Light/scene variants reuse that shape (a "Variants"
accordion beside "Animation"). Since `> modify animation` is itself a `> modify` form, deprecating `> modify`
also moves **animation selection** to a stored `active_animation <name>` field — tracked here for completeness;
the animation feature owns that field.

## 8. Phasing

- **P0 — scene language + derive.** `scene_variant` chunk (name required) + the `variant <name>` tag on
  material chunks + `active_scene_variant` + `active_camera`. Derive applies the active variant's overrides +
  camera. Tests: name-required parse error, override-by-name applies, dangling-override diagnostic,
  base-default, active-camera selection, save round-trip.
- **P1 — GUI.** A "Variants" accordion + selector (mirror the Animation accordion); selecting re-derives.
- **P2 — migrator.** Convert `> modify` blocks → `scene_variant` + tagged overrides (watch_dial night).
- **P3 — deprecate `> modify`.** Remove the command once the corpus is converted (rides P5 Slice 6).

## 9. Decisions (ratified 2026-06-28) + one remaining detail

- **D-name (O1): `scene_variant`** — general, flexible; name **required** (UI handle). *Ratified.*
- **D-select (O2): stored `active_scene_variant`**, base = default. *Ratified.*
- **D-overrides (O3):** P0 = **`active_camera` selection + material overrides** (via the `variant` tag). The
  old object-rebind / material-scale / rasterizer-radiance ops are subsumed (§2), not P0 primitives. *Ratified.*
- **D-composition (O4): single-select.** *Ratified.*
- **D-default (O5): implicit base-as-default.** *Recommended; not contested.*
- **OPEN-1 — override syntax:** the `variant <name>` tag on a flat top-level chunk (§3.2; recommended —
  fits the descriptor-driven parser, generalizes to any chunk type) vs material chunks nested inside the
  `scene_variant` chunk (more cohesive but needs nested-chunk parsing). Recommend the **tag**.

## 10. Read next

- [62-model-b-p5-save-as-cst-plan.md](62-model-b-p5-save-as-cst-plan.md) — P5 plan; Slice 2 owns the
  `> modify` → `scene_variant` migrator conversion + the `light_rr_threshold` easy-convert.
- [src/Library/Parsers/AsciiCommandParser.cpp](../../src/Library/Parsers/AsciiCommandParser.cpp) — the
  `> modify` dispatch this subsumes; [Job::SetActiveCamera](../../src/Library/Job.h) — the active-camera hook.
- [src/Library/Parsers/README.md](../../src/Library/Parsers/README.md) — how to add the `scene_variant` chunk +
  the `variant` tag (descriptor-driven `IAsciiChunkParser`).
