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
- **D-syntax (OPEN-1): RESOLVED (2026-06-28) — the `variant <name>` tag** on flat top-level chunks (§3.2);
  it fits the descriptor-driven parser with no nested-chunk machinery and generalizes to any chunk type. The
  nested-material-chunk alternative is a nice future affordance but adds real parser complexity for marginal
  value today; explored + documented in §10 so a future add has an easy starting point.

## 10. Explored + deferred: material chunks nested in the `scene_variant`

We considered expressing a variant's overrides as material chunks **nested inside** the `scene_variant` chunk:

```
scene_variant
{
    name           night
    active_camera   night_hero
    lambertian_luminaire_material { name soft_top_lum   exitance pnt_soft       scale 0.0 }
    lambertian_luminaire_material { name lume_white      exitance pnt_lume_glow  scale 0.9 }
}
```

This is more **cohesive** — a variant's entire delta reads in one place, with no `variant`-tagged chunks
scattered across the file. We **deferred** it (decision 2026-06-28): it requires nested-chunk parsing the
descriptor-driven grammar does not have (chunks are flat — params, not sub-chunks), which is real machinery
for marginal value at P0, since the flat `variant <name>` tag (§3.2) expresses the same semantics.

**If added later, the easy starting point:** the P0 `variant`-tag path already builds the variant-override
store + the active-variant application (§3.4) — nesting is just *sugar* over it. Extend the `scene_variant`
chunk parser to recognize a nested-chunk region and route each inner chunk through its normal `Finalize` with
the enclosing variant supplied as an implicit `variant` tag. No new derive/apply semantics — only a parser
front-end. A future implementer works off §3.2/§3.4, not from scratch.

## 11. Read next

- [62-model-b-p5-save-as-cst-plan.md](62-model-b-p5-save-as-cst-plan.md) — P5 plan; Slice 2 owns the
  `> modify` → `scene_variant` migrator conversion + the `light_rr_threshold` easy-convert.
- [src/Library/Parsers/AsciiCommandParser.cpp](../../src/Library/Parsers/AsciiCommandParser.cpp) — the
  `> modify` dispatch this subsumes; [Job::SetActiveCamera](../../src/Library/Job.h) — the active-camera hook.
- [src/Library/Parsers/README.md](../../src/Library/Parsers/README.md) — how to add the `scene_variant` chunk +
  the `variant` tag (descriptor-driven `IAsciiChunkParser`).

## 12. P0 implementation build sheet (turnkey — resolved against the code 2026-06-28)

Every integration point below is verified against the tree; the build is mechanical execution + the review loop.

**Precedents to copy:** the named-animation feature (`animation` chunk + `DeclareAnimation(...active)` + `> modify animation` selection) and the `static`-cross-chunk parse-state reset (`ClearParseState` / `ClearChunkParserState`, called by BOTH load paths) — the seam that lets one apply-pass serve both.

**(a) `IJob` ABI** — add 4 NON-pure virtuals (default `{return false;}` / `{}`, like `SetActiveAnimation` at `IJob.h:2797`) after `GetActiveAnimationName` (`IJob.h:2823`): `DeclareSceneVariant(name, active_camera)`, `SetActiveSceneVariant(name)`, `SetPendingMaterialVariant(variant)`, `ApplyActiveSceneVariant()`. (Non-pure = no out-of-tree implementer breaks.)

**(b) `Job` store** (after `composedMaterialNames`, `Job.h:281`): `struct SceneVariantData { String activeCamera; std::map<String,IMaterial*> materialOverrides; };` + `std::map<String,SceneVariantData> sceneVariants;` + `String activeSceneVariant;` + `String pendingMaterialVariant;`. Decls (after `Job.h:2840`): the 4 IJob overrides (NO `override` keyword — Job convention) + `RegisterMaterialOrVariant(IMaterial*,name)` + `ClearSceneVariants()`. Defs in `Job.cpp` near `Job::DeclareAnimation` (`9633`).

**(c) Material routing** — the 26 material `RegisterOrDiag(pMatManager, …, name, "material")` calls (24 `pMaterial` + 2 `pLumMaterial` at `Job.cpp:4215,4246` — the luminaires the watch night-mode needs) become `RegisterMaterialOrVariant(pMaterial|pLumMaterial, name)`. `RegisterMaterialOrVariant`: if `pendingMaterialVariant` non-empty → addref into `sceneVariants[pending].materialOverrides[name]` (last-write-wins releases prior); else `RegisterOrDiag(pMatManager,…)`. Behavior-neutral until the parser sets `pendingMaterialVariant`.

**(d) Apply (`ApplyActiveSceneVariant`, the crux)** — objects bind materials by POINTER (realize bakes geometry only; `RayCaster.cpp:159`), so the apply re-points objects, it does NOT just swap the manager entry. Build `remap: const IMaterial*(base ptr from pMatManager->GetItem(name)) -> IMaterial*(override)`; a base miss = a dangling-override diagnostic. Re-point: `IObjectManager : IManager<IObjectPriv>` so enumerate object NAMES (IManager name-enum) → `pObjectManager->GetItem(name)` (NON-const → can call `AssignMaterial`, avoiding a const_cast on the `const`-ref `EnumerateObjects(IEnumCallback<IObjectPriv>)` callback; confirm SceneEditor.cpp:195's pattern at build). For each object whose `GetMaterial()` is a remap key → `AssignMaterial(*override)`; track `anyEmitter` (base or override `GetEmitter()`); if any → `BumpSceneLightGen(pScene)` (regenerates the light list — handles the luminaire dual-role, mirroring `Job::SetObjectMaterial`). Then `pScene->SetActiveCamera(activeCamera)` if set. Empty `activeSceneVariant` → no-op (base default); unknown active variant → warn + base.

**(e) Chunks** (`AsciiSceneParser.cpp`, mirror `AnimationAsciiChunkParser:9666`): `SceneVariantAsciiChunkParser` (Finalize: name REQUIRED else false → `DeclareSceneVariant`; `active_camera` ValueKind::Reference{Camera}) + `ActiveSceneVariantAsciiChunkParser` (Finalize → `SetActiveSceneVariant`). Register after `9909`. Add `static AddVariantTagParam(cd)` (an optional `variant` String param) and CALL it in each of the 26 material `Describe()` IIFEs (descriptor = accepted-set invariant; without it the parser rejects `variant`).

**(f) Pending-variant routing (central, 2 sites)** — around the single `Finalize` call in the default `ParseChunk` (`AsciiSceneParser.cpp:9929`) AND the CST PASS-2 loop (`Cst.cpp:~1441`): if the chunk's category is Material, `pJob.SetPendingMaterialVariant(bag.GetString("variant",""))` before, and `SetPendingMaterialVariant("")` UNCONDITIONALLY after (no leak — the `s_painterColors` reset discipline).

**(g) Apply hook (both paths reach it)** — `pJob.ApplyActiveSceneVariant()` at: legacy end-of-load top-level block (`AsciiSceneParser.cpp:~11116`, beside the snapshot population, `depthGuard.isTopLevel` so a `> load` child doesn't fire early) AND CST `DeriveToJob` end (`Cst.cpp:~1468`, guarded on `diags.empty()`). Apply at derive-end (not realize) so DumpJob + the editor see it pre-render.

**(h) Reset** — `ClearSceneVariants()` (release stored override refs) at both reset sites: legacy `ParseAndLoadScene` metadata clear (`~10569`) + `Cst.cpp` `ClearChunkParserState` (`~1380`) — re-derive must not accumulate stale overrides.

**(i) CST** — the chunk + `variant` param auto-flow through `DeriveToJob` (descriptor-driven). `IsNativeV7Document` accepts them (they're `NodeKind::Chunk`, not `>`/FOR — verify). `DeriveToJobIncremental` must REFUSE (fall back to full derive) when the edited chunk is `scene_variant`/`active_scene_variant`/a `variant`-tagged material (same-name-as-base collision) — mirror the existing refusals (`Cst.cpp:~1622`).

**(j) Build projects** — P0 folds into existing files (no new `.cpp`/`.h`) → NO build-project edits. Tests auto-discover (`run_all_tests.sh` globs `tests/*.cpp`).

**(k) Tests** (`tests/SceneVariantTest.cpp` + `scenes/Tests/SceneVariants/*.RISEscene`, mirror `CstRenderEquivalence.h`'s DumpJob which surfaces luminaire exitance + active_camera): name-required parse error; override-by-name applies (luminaire `scale 0` → object's material exitance ≈ 0) via BOTH legacy + CST; dangling-override diagnostic; base-as-default (no override when inactive); active-camera selection; save round-trip (`SerializeCst`); **CST == legacy DumpJob** with the variant active (the headline gate).

**Risks:** const-callback mutation (use name-enum+GetItem, (d)); luminaire light-gen (BumpSceneLightGen, (d)); incremental-derive collision (refuse, (i)); `pendingMaterialVariant` leak (unconditional clear, (f)); load-once / nested `> load` (top-level-only apply, (g)).
