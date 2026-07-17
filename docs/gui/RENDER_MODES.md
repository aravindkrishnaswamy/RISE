# Render Modes, N-up Viewports, and Agent Visualization Skills

Status: **DESIGN RATIFIED 2026-07-16 — P1 in progress**

Owner surface: both GUIs (Mac + Windows), core library (`src/Library/Rendering`,
`src/Library/SceneEditor`, `src/Library/Agent`), agent skills (`skills/agent/`).

## 1. Vision

Users and agents can rapidly switch the viewport between *kinds* of renderings —
each answering a different question about the scene — and eventually compose an
N-up layout of several renderings from several vantage points simultaneously.
The agent gets the same vocabulary through its `render` tool plus skills that
teach *which mode answers which question*, so it can visualize scenes to model
and mutate them more effectively.

**Organizing principle: every mode answers a question.** The taxonomy below is
grouped by question, and the agent skill is literally a decision table from
question → mode.

## 2. Ratified decisions (2026-07-16)

1. **View-transforms are NOT render modes.** Exposure zebra / luminance false
   color re-transform the existing framebuffer without re-rendering; they live
   in the display cluster (with EV / tone curve), not the mode switcher.
2. **Fixed built-in mode set only.** No user-editable mode presets in v1;
   user-tunable mode settings are a possible future extension.
3. **N-up layouts are a fixed set** (1 / 2-horizontal / 1+2 / 2×2), max 4 panes.
4. **The agent sees everything** — all modes including diagnostics, same names
   as the UI. One namespace, one registry, no curated subset.
5. **Wireframe ships in P1 as a first-hit edge shader** (barycentric distance to
   the nearest edge on the primary hit). Ray-traced first-hit wireframe gets
   hidden-line removal for free. A "proper" wireframe-over-beauty composite is
   a possible P4 refinement, not a P1 requirement.

## 3. Mode taxonomy

Grouped by the question each mode answers. Tier legend:
**T0** exists today · **T1** small new first-hit shader (ephemeral-pipeline
sibling) · **T2** beauty-variant (real integrator + config overrides) ·
**T3** real transport work.

### Structure — "where is everything, exactly?"

| Mode | Tier | Notes |
|---|---|---|
| `objectmap` | T0 | Flat per-object identity segmentation (`ObjectIdShader`), exactness invariant, legend + `query_object_at`. Already an agent mode. |
| `normals` | T1 | World-space shading normal → RGB false color (`0.5·(N+1)`). Also the honest wiring for the declared-but-unfilled `FrameStore` Normal channel. |
| `depth` | T1 | Primary-hit distance, normalized to the frame's [near, far] percentile window, grayscale (near = bright). |
| `facets` | T1 | Headlamp-shaded **geometric** normal (no smoothing, no bump) — reveals tessellation and shading-normal divergence. |
| `wireframe` | T1 | First-hit edge shader: barycentric distance to nearest triangle edge under a screen-space width heuristic; non-edge pixels get a dim facet shade so form reads. Analytic (non-mesh) primitives render facet-shade only (documented honestly — no synthetic tessellation lines). |
| UV checker | T3 | Needs UV plumbing at the hit record. Deferred (P4 candidate). |

### Lighting — "is the light doing what I think?"

| Mode | Tier | Notes |
|---|---|---|
| `clay` | T0 | The existing studio material preview (`InteractiveMaterialPreviewShader`: clay + AO + headlamp). Already the GUI draft preview and the agent `quality:"draft"`. |
| `clay_lights` | T2 | Clay albedo but the scene's REAL lights (the classic "is lighting right independent of materials" check). |
| `direct` | T2 | Direct lighting only (`directlighting_shaderop` exists as the substrate). |
| `indirect` | T2 | Beauty minus direct. |
| light solo | T2 | Render with exactly one light enabled. Needs a light-selector arg; P2 design detail. |

### Transport — "what are reflections/refractions doing?"

| Mode | Tier | Notes |
|---|---|---|
| `deep_reflect` | T2 | The headline ask: low resolution (¼ default) + deep bounce depth + more spp/pixel, so reflections/refractions resolve. Needs the per-render config-override mechanism (§6). |
| specular-only | T3 | Follow only specular lobes (LPE-lite path filter). P4. |
| caustics-only | T3 | VCM merge-only contribution. P4. |

### Diagnostics — "where is the effort going?"

| Mode | Tier | Notes |
|---|---|---|
| sample heatmap | T0 | `show_adaptive_map` exists (adaptive-sampling density). Registry-listed once adaptive sampling is active in the pipeline that rendered. |
| variance heatmap | T2 | The auto-rasterizer probe already computes σ²; wiring it to false color is P2/P4. |

### Explicitly NOT render modes (display cluster instead — decision 1)

Exposure zebra, luminance false color, denoise on/off compare. These are
framebuffer transforms; they compose WITH any render mode.

## 4. Core abstraction: the mode registry

One registry, one namespace, shared by the GUI dropdown and the agent tool —
the skill vocabulary and the UI vocabulary can never drift.

```
ViewportRenderMode (enum class, core library)
    Beauty, Clay, ObjectMap, Normals, Depth, Facets, Wireframe   // P1
    ClayLights, Direct, Indirect, DeepReflect, ...               // P2+

ViewportRenderModeInfo { mode, name ("normals"), title ("Normals"),
                         question ("Which way do surfaces face?"),
                         pipelineKind (BeautyConfig | ShaderPipeline | BeautyVariant),
                         wantsDenoise (false for all data modes) }
```

- Registry lives beside the interactive pipeline factories
  (`src/Library/Rendering/InteractivePelRasterizer.{h,cpp}` family).
- `name` strings are the wire format everywhere: C-ABI, agent `render{mode:}`,
  GUI persistence. Enum values are never serialized.
- **Parity is tested**: a unit test asserts the agent tool's accepted mode-name
  set == the registry's, so adding a mode without agent parity fails CI.

### Naming note (implementation finding)

Today's interactive viewport pipeline IS the studio material preview
(`CreateInteractiveMaterialPreviewPipeline` — clay + AO + studio lights over
real material albedo). The registry therefore names the viewport's default
mode **`preview`** ("Shaded Preview"), not `beauty`: an interactive true-beauty
mode doesn't exist until the P2 beauty-variant work. The agent `render` tool's
`beauty` keeps its existing meaning (production integrator or draft preview
depending on `quality`) — the two surfaces share every other mode name.

### Pipeline kinds

- **BeautyConfig** (`beauty`): today's interactive preview path, untouched.
- **ShaderPipeline**: an ephemeral first-hit shader on an
  `InteractiveMaterialPreviewRayCaster`, exactly the existing clay/objectmap
  pattern. All P1 modes are this kind.
- **BeautyVariant** (P2): the real integrator with per-render config overrides
  (resolution divisor, bounce depth, spp, light filter). Requires §6.

### Denoise / display policy

Data modes (`objectmap`, `normals`, `depth`, `facets`, `wireframe`) are never
OIDN-denoised (denoising a normal map is meaningless) — `wantsDenoise=false`
routes the controller's polish/idle denoise selection to Off while such a mode
is active. Display tone curve still applies; P1 shaders emit linear values
chosen for legibility under the default transform (only `objectmap` needs and
implements byte exactness).

## 5. Viewport mode switching (P1 mechanism)

The shader rides on the ray caster, and `InteractivePelRasterizer` already has
caster-swap machinery (the polish-caster mechanism). Mode switch = **caster
swap**, coordinated by `SceneEditController`:

```
SceneEditController::SetViewportRenderMode(ViewportRenderMode m)
  - no-op if m == current
  - park the interactive render (existing CancelAndParkRender_ machinery)
  - build the mode's caster via CreateInteractiveViewModeCaster(m) (factory
    beside the existing pipeline factories); Beauty/Clay restores the
    original preview + polish casters the platform installed
  - swap the preview caster on mInteractiveImpl; disable polish caster and
    denoise for data modes
  - restart the interactive loop → normal cancel-restart preview repaints
GetViewportRenderMode() for UI state.
```

- The production "Render" path is completely unaffected — modes are a property
  of the interactive viewport only. Production renders stay beauty.
- Object picking, gizmos, region-of-interest, camera nav all keep working in
  every mode (they don't depend on shading).
- C-ABI: `RISE_API_SceneEditController_{Set,Get}ViewportRenderMode(name)` +
  registry enumeration via `RISE_API_GetViewportRenderModeCount()` /
  `RISE_API_GetViewportRenderModeInfo(index, …)` (names + titles + questions,
  for UI and agent listing).
- GUI: a compact mode dropdown in the viewport chrome (Mac: viewport toolbar;
  Windows: TopBar combobox). Persisted per-session only; every scene opens in
  `preview` (a scene silently opening in depth mode would read as a broken
  render).

## 6. Per-render config overrides (P2 prerequisite)

Beauty-variant modes need "the real integrator, but with these deltas" without
rebuilding the scene: a `ViewportConfigOverride { scaleDivisor, maxBounces,
samplesPerPixel, lightFilter }` applied at RasterizeScene time — the config
sibling of the existing `SetViewportCameraOverride` (which pays the same way at
the camera-read site, NOT via wrapper objects — see the Tier-2 pivot lesson).
Design detail deferred to P2.

## 7. N-up multi-viewport (P3)

Pane = `{ mode, vantage, size }`; vantage = scene camera | free-fly pose |
**named view** (Tier 2 named views are the vantage store). One pane is
**primary**: it owns editing, picking, gizmos; others are viewers.

- Fixed layouts: `1`, `2` (side-by-side), `1+2`, `2×2` (decision 3; max 4).
- Rendering: **one interactive render thread**, round-robin across panes,
  coarse-to-fine; ShaderPipeline panes are ~1 spp first-hit (nearly free), so
  budget contention is only among beauty/beauty-variant panes. An edit
  invalidates all panes; the primary refines first.
- Plumbing primitives already exist: refcounted `ViewportFrameStore` per sink,
  `SetViewportCameraOverride` per render, named-view poses (Tier 2 B1).
- Production renders remain single-view.

## 8. Agent surface + skills

- `render{mode:}` widens from `beauty|objectmap` to the full registry
  (decision 4). Data modes ignore `quality`/`samples` exactly as `objectmap`
  does today (honestly noted in the result message).
- **Deferred (P2, not yet implemented):** `render` gaining an optional
  `view:"<named view>"` so the agent composes its own "N-up" as sequential
  renders from saved vantages (no layout tool needed). Deliberately cut from
  the P1 agent-surface wave to keep its scope to mode widening.
- Mode list + questions discoverable via the render tool's schema description
  (generated from the registry — single source of truth).
- `skills/agent/observe-modes.md` extends with the question → mode decision
  table and recipes:
  - placement check → `objectmap` top view → `query_object_at`
  - lighting check → `clay` (later `clay_lights`) before touching materials
  - tessellation/smoothing artifact → `facets` / `wireframe`
  - reflection content → `deep_reflect` at ¼ res (P2)
  - scale/occlusion sanity → `depth`

## 9. Phasing

- **P1 (now)**: registry + T1 shaders (`normals`, `depth`, `facets`,
  `wireframe`) + controller caster-swap + C-ABI + both bridges + viewport
  dropdown + agent `render{mode}` widening + skill update + parity/shader
  tests.
- **P2**: config-override mechanism (§6) + the agent `view:` arg (§8), then
  `deep_reflect`, `direct`,
  `indirect`, `clay_lights`, light solo.
- **P3**: N-up (layouts, per-pane mode+vantage, round-robin scheduler,
  primary-pane semantics).
- **P4**: specular-only, caustics-only, variance heatmap, UV checker,
  wireframe-over-beauty composite.

## 10. Known limitations / honest notes

- Wireframe on analytic primitives (sphere/box/SDF…) shows facet shading with
  no edges — there is no tessellation to draw. Documented in the mode's
  question string and the skill.
- `show_adaptive_map` remains a separate diagnostic toggle until adaptive
  sampling participates in the interactive pipeline.
- Windows GUI changes are review-verified only until the next Windows compile
  (standing caveat).
