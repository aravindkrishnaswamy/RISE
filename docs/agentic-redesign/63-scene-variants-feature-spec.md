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

- A `variant night` chunk's `name` must match a base chunk: a **dangling override** (no base material of that
  name — typically a typo of the base name) is **REFUSED** (refuse-all — the whole derive applies nothing) with a derive diagnostic, so it cannot silently
  register a phantom material while the intended base stays unchanged (the exact silent mis-render §1 exists to
  prevent). Add-new (a `variant` chunk introducing a brand-new name) is **out of P0 scope** — a variant has no
  way to bind a new material to an object yet (variant-tagged *objects* are a later extension, §5), so a
  brand-new variant material could only register unbound; refusing it catches the far-likelier typo.
- A material name has at most ONE active definition: two `variant night` chunks of the same `name` (an **ambiguous
  override**) — or two untagged bases of the same `name` (which AddItem would otherwise reject only mid-apply, with
  a less clear diagnostic) — are **REFUSED** in the pre-scan. (The override is applied at the base's slot, so the
  base must both exist — the dangling guard above — and be unique.)
- This is the user's "specify new materials": variant-tagged material chunks redefine base materials for that
  variant. The same tag mechanism extends to other chunk types later without a new concept.

### 3.3 Selecting the active variant

- The base scene (no overrides) is the implicit **default**.
- A stored top-level **`active_scene_variant <name>`** selects one (round-trips through save — a
  panel-editable property that *is* storable). `none` / absent = the base default; a name matching neither a declared `scene_variant` nor any `variant`-tagged chunk is REFUSED (a selector typo — symmetric to a dangling override).
- **Single-select** (one active variant at a time — matches `> modify`; the watch needs only day/night).
- GUI (BUILT): a **"Variants" accordion** with a selector, mirroring named-animation-paths — picking a row calls
  `SceneEditController::SetSelection(Category::SceneVariant, name)` → `Job::RederiveCstWithVariant` (ClearAll +
  re-derive the retained CST Document with that variant forced active) + a scene-epoch bump.  Requires the scene to
  have been CST-loaded (`RISE_LOAD_VIA_CST`, pending the Slice-5 default); the section lists NO entries (no pickable
  rows) when no CST Document is retained or the scene declares no variants — gated in `CategoryEntityCount` on
  `HasRetainedCstDocument()`, so a legacy-loaded scene can't offer a variant pick that would silently no-op.
  CLI/agent: a CST edit to `active_scene_variant` → re-derive.

### 3.4 Derive integration (Model-B)

`DeriveToJob` **bakes** the active variant: it registers, per material *name*, the ACTIVE definition (the
variant's override if `active_scene_variant` overrides that name, else the base) so objects bind to the active
material BY NAME at derive time — there is NO post-derive re-pointing of a built scene. (Objects bind materials
by pointer — the correct render-time form, realize bakes geometry only — and re-pointing a built graph is the
fragile mutation pattern Model-B avoids: change the source + re-derive, don't mutate.) It also applies the
active variant's `active_camera` via `SetActiveCamera`. No `>` layer; CST-loadable / -editable / -saveable
end-to-end. Switching variants = a CST edit to `active_scene_variant` → re-derive (full per [62](62-model-b-p5-save-as-cst-plan.md) D2; sub-millisecond). CST-native: the whole-document derive sees the active variant before
objects resolve; the legacy streaming reader (slated for deletion) parses the chunks and renders the base.

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
- **P1 — GUI (DONE, commits 782fa451 + fc3ea0c3).** A "Variants" accordion + selector (mirrors the Animation
  accordion) on Mac (+ Windows parity); picking a row re-derives via `Job::RederiveCstWithVariant`.  The switch
  needs a CST-loaded scene (`RISE_LOAD_VIA_CST`); the core is `DeriveToJob`'s `activeVariantOverride` (slice 3a).
- **P2 — migrator.** watch_dial's night `> modify` block is converted (commit 8cd2b48a); the general corpus-wide
  `> modify` → `scene_variant` migrator is P5 Slice 2.
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


## 12. P0 implementation build sheet (design B: BAKE-AT-DERIVE — ratified 2026-06-28)

Ratified pivot from re-point to bake-at-derive (the pointer-binding-fragility discussion, §3.4): the variant is
baked DURING the CST derive so objects bind to the active material by name from the start. This DROPS the
trickiest parts of the re-point design — no object iteration, no `const`-callback, no light-gen bump, no
override store, no per-material Job rewire. "Derive the right scene," not "mutate a built one."

**(a) Chunks** (`AsciiSceneParser.cpp`, mirror `AnimationAsciiChunkParser:9666`): `scene_variant { name <REQUIRED>
active_camera <opt, ValueKind::Reference{Camera}> }` → Finalize `DeclareSceneVariant(name, camera)` (empty name ⇒
return false). `active_scene_variant { name <opt> }` → Finalize `SetActiveSceneVariant(name)` (""/"none"/absent ⇒
base default). Register after `:9909`.

**(b) `variant` descriptor tag** — a shared `AddVariantTagParam(cd)` (optional `variant` String) called in each of
the 25 material `Describe()` IIFEs (so `variant` parses; descriptor=accepted-set invariant holds). The material
`Finalize` IGNORES `variant` — it is a marker the derive reads. NO Job-side material rewire / pending-variant /
override store.

**(c) IJob/Job records** (for GUI + save; the active-variant bake is whole-document but RE-RUNNABLE on demand from
the GUI via `RederiveCstWithVariant`) — 9 non-pure IJob virtuals: the declare/query/clear set near `IJob.h:2844`
(`DeclareSceneVariant`, `SetActiveSceneVariant`, `GetActiveSceneVariant`, `HasSceneVariants` [the
incremental-refuse signal], `ClearSceneVariants`), the GUI variant-list pair `GetSceneVariantCount` /
`GetSceneVariantName`, and — beside `LoadAsciiSceneViaCst` — `RederiveCstWithVariant` (the GUI switch: ClearAll +
re-derive the retained Document with a forced variant) + `HasRetainedCstDocument` (gates the GUI accordion on a
retained Document). `Job` (store near `:281`, decls near `:2840`, defs near `:9633`, NO `override`
keyword): `std::map<String,String> sceneVariantCameras` + `String activeSceneVariant`; methods record.
`ClearSceneVariants` is wired at the per-derive resets: `Job::InitializeContainers` (beside `m_objectOverrideCount
= 0` — the creation + legacy reset) AND `DeriveToJob`'s start (CST re-derive; `ClearChunkParserState` clears
PARSER state, not Job state).

**(d) CST bake (the crux — `Cst.cpp DeriveToJob`)** — between PASS-1 and PASS-2, PRE-SCAN the chunks →
`activeName` (the `active_scene_variant` chunk's `name`), `activeCamera` (that variant's `scene_variant` chunk's
`active_camera`), `activeOverride` (material chunks whose `variant` == activeName, mapped `name` → pending index;
a second override of one name ⇒ ambiguous ⇒ diagnostic). PASS-2 loop: for an untagged base whose `name` is in
`activeOverride`, `Finalize` the ACTIVE OVERRIDE'S pending **at the base's slot** (not the base) — so the active
material registers under its `name` exactly where the base would have, and objects (later, definitions-before-use)
bind that name → the active material **regardless of where the override chunk sits in the file** (it may follow the
objects, as the watch night block does). SKIP a material chunk's `Finalize` iff it carries any `variant` tag (the
active one was already applied at its base's slot; inactive ones are dropped); `Finalize` everything else. After the
loop (guard `diags.empty()`): `pJob.SetActiveCamera(activeCamera)` if set. No re-point, no light-gen bump (lights
build from correctly-bound objects). **Authoring constraint: only the override's PAINTERS need precede the base
(standard painters-before-materials); the override material chunk itself is position-free.**

**(e) Legacy path** (`AsciiSceneParser ParseChunk`, 1 central site) — a streaming reader can't pre-scan and
scene_variant is CST-native, so legacy renders the BASE: SKIP a material chunk's `Finalize` iff it carries a
`variant` tag (`variant none` is the no-variant sentinel — an ordinary base, not skipped; the lookup is gated
behind the material category so it never trips the descriptor diagnostic on non-material chunks). The
`scene_variant`/`active_scene_variant` chunks Finalize normally (record only). No material/camera bake in legacy.

**Render entry point.** Because legacy renders the BASE, rendering a scene's *active* variant requires the CST
load path — which, as of Slice 5, is the **DEFAULT** (the former `RISE_LOAD_VIA_CST=1` opt-in env var has been
REMOVED). Loading a scene normally now goes through `Job::LoadAsciiSceneViaCst` (an `IJob` virtual), which loads via
the CST and bakes the active variant; no silent fallback (a non-native-v7 / derive-error scene fails visibly). To
render the watch night hero, just uncomment `watch_dial.RISEscene`'s `active_scene_variant { name night }` and load
the scene as usual — the active variant resolves automatically via the retained CST Document.

**(f) CST incremental-derive** — `DeriveToJobIncremental` REFUSES (full re-derive) for ANY scene that has
variants, via the O(1) `pJob.HasSceneVariants()` engine signal (the bake is a whole-document decision, so a
per-edited-chunk refusal isn't needed — refusing the whole variant scene is strictly safe and O(1)).
`IsNativeV7Document` accepts `scene_variant` / `active_scene_variant` / `variant`-tagged materials (all `NodeKind::Chunk`).
Note: the *recorded* dependency graph (the D35 record-during-derive capture in `DeriveToJob`, which sees the
redirect) is authoritative for variant scenes and records the override→consumer edges correctly. The *static*
`BuildReferenceGraph` is variant-blind (it records both base→consumer and override-painter edges), but it is never
consulted for a variant scene — incremental edits fall back to full re-derive via `HasSceneVariants()` above — so
the divergence is inert today; a future static-path consumer of a variant-active scene would need variant handling.

**(g) Build projects** — folds into existing files (no new `.cpp`/`.h`) → no build-project edits. Tests
auto-discover (`run_all_tests.sh` globs `tests/*.cpp`).

**(h) Tests** (`tests/SceneVariantTest.cpp` — synthetic inline scene strings, no external scene files — mirror `CstRenderEquivalence.h`'s
DumpJob — surfaces luminaire exitance + active_camera): via CST `DeriveToJob` — name-required parse error; night
active ⇒ the soft-luminaire object's material exitance ≈ 0 AND the marker object's material glows AND
`GetActiveCameraName` == the night camera; base-default (no active ⇒ base values); a `variant` overriding a
non-existent base ⇒ diagnostic. Legacy ⇒ base (variant materials skipped). CST==legacy DumpJob for the BASE case;
for the active case assert the CST bake (legacy = base, documented). Risks: pre-scan reads chunk params before
Finalize (resolve against DeriveToJob's PASS-1 bags); legacy central-skip correctness; incremental refusal.
