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
| `depth` | T1 | Primary-hit distance, AUTO-WINDOWED per pass to the frame's VISIBLE min/max hit-distance range (grayscale, near = bright); falls back to the scene bounding-box diagonal on a degenerate/empty scene. SELF-CALIBRATES WITHIN A SINGLE RENDER CALL (2026-07-17 fix). See "Depth auto-windowing" below (2026-07-17 fix — the original fixed scene-diagonal normalization flattened staged scenes into a near-blank image). |
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

### Depth auto-windowing (2026-07-17 fix)

`depth` originally normalized brightness by the scene's world bounding-box
diagonal — fine for a single object filling the frame, but on a staged scene
(subject ~50 units, camera ~95, background studio panels at ~400) the whole
subject landed in a ~1% brightness band and read as a flat, near-blank image.
Replaced with per-pass visible-range calibration: `DepthViewShader` accumulates
the min/max hit distance actually seen during the CURRENT pass (atomic
CAS-loop min/max, seeded with finite sentinels — never infinity, per the
repo's `-ffast-math` rule), then `InteractiveViewModeRayCaster::AttachScene`
(which already runs single-threaded at the top of every `RasterizeScene` pass,
before the parallel block workers dispatch) snapshots that range into the
ACTIVE window for the pass about to run, and resets the accumulators. A
degenerate previous pass (no samples, or a near-zero range) leaves whatever
window was already active — the mode falls back to the fixed scene-diagonal
formula only on the very first pass or when nothing meaningful was visible.
Brightness therefore reflects the VISIBLE depth range, not a fixed world-scale
band: the previous pass calibrates the next, and the interactive cancel-restart
loop converges this within one repaint (mild recalibration while navigating
is intended — exactly like camera auto-exposure).

**Self-calibration within a single call (2026-07-17 fix).** The above was
originally accurate only for the INTERACTIVE multi-frame loop — a single
SETTLED `RasterizeScene` call (a GUI mode switch, or any one-shot render like
the agent `render{mode:"depth"}` path) got exactly one pass, which shipped
the flat scene-diagonal fallback because no *previous* pass had ever
populated the window (the reported "depth renders nothing" bug).
`InteractivePelRasterizer::RasterizeScene` now overrides the base: it runs
the base pass, then — if the active caster is a Depth view-mode caster whose
window is still pending (recorded samples this pass, but not yet snapshotted
into a valid window — `InteractiveViewModeRayCaster::DepthWindowPending` /
`DepthViewShader::WindowPending`) and the pass was not cancelled — runs the
base pass ONCE more. The second pass's `AttachScene` call snapshots the
first pass's accumulators into the window, so the SAME `RasterizeScene` call
returns a windowed image. Guarded structurally to at most one extra pass per
call (the re-run calls the base class method directly, not virtually, so
there is no re-entrancy into the override). The agent path no longer needs
its own warm-up pass — a single `RasterizeScene` call already self-corrects.

### X-ray axis

An orthogonal **boolean axis**, not a fifth mode — it composes with all four
data modes (`normals` / `depth` / `facets` / `wireframe`) AND applies to
`preview` (the studio material-preview pipeline) too. When on, the resolved
primary hit is walked THROUGH transmissive (glass-like) surfaces to the
first OPAQUE hit: a straight-line continuation of the original ray
direction, deliberately with NO refraction bending (an x-ray, not an optics
simulation — it answers "what's under/inside this transmissive geometry", not
"what would actually be seen looking through it"). Bounded to 16 skips so a
stack of nested transmissive shells can't loop forever; a miss partway
through the chain keeps the LAST transmissive hit rather than reporting a
miss (shading something honest beats a black hole). `depth` under x-ray uses
the TOTAL distance from the original ray's origin to the resolved hit point,
not the resolved hit's own (last-segment) range and not a sum of per-segment
ranges — otherwise a skip chain would under-report depth.

**Caster-layer implementation (2026-07-17 refactor).** Resolution used to
live in the four view-mode SHADERS (`InteractivePelRasterizer.cpp`,
anonymous-namespace `ResolveXrayHit`, called per-shader with an `xray`
constructor flag) — which meant `preview` could never see through glass, and
every new shader had to remember to wire it in. It now lives in the CASTER
layer instead, so every shader benefits automatically:

- Core: `RayCaster::ResolveXrayView_(RayIntersection&)` (private,
  `RayCaster.{h,cpp}`) — same 16-skip loop, called from `CastRay` / `CastRayNM`
  (and, for pre-shade-structure completeness, `CastRayHWSS`) immediately
  after the intersection + emitter-visibility check, BEFORE medium transport
  / the modifier site / shading. When at least one skip happens, it restores
  the ORIGINAL primary ray onto `ri.geometric.ray` and recomputes
  `ri.geometric.range` as the TOTAL distance from the original ray's origin
  — so `depth` (and any other range-reading consumer) is correct with ZERO
  x-ray-specific shader knowledge. Deliberately does not apply
  `ri.pModifier` itself — the caller's EXISTING modifier site runs
  immediately after, covering whatever `ri` ends up being.
- Toggle: `RayCaster::{Set,Get}XrayViewResolve(bool)` — protected member
  `bXrayViewResolve`, default `false`; production casters never set it (cost
  when off is one bool test).
- Factories: `CreateInteractiveViewModeCaster` / `CreateInteractiveViewModePipeline`
  keep their trailing `bool xray = false` parameter, but now implement it by
  calling `SetXrayViewResolve` on the built caster instead of threading a
  constructor flag through each shader.
- Rasterizer: `InteractivePelRasterizer::{Set,Get}XrayView(bool)` stamps the
  flag onto every `RayCaster` it can currently reach (active caster, polish
  caster, and both saved-caster slots, via `dynamic_cast<RayCaster*>`,
  null-tolerant) — including the studio preview caster, which is how
  `preview` gets x-ray for free. Every caster-swap site
  (`SetViewModeCaster` install/switch/restore, `SetPolishRayCaster`,
  `SetSampleCount`'s polish swap) re-stamps the current flag onto whatever
  caster becomes active, so a caster installed after `SetXrayView` was
  called still reflects it.
- Controller: `SceneEditController::{Set,Get}ViewportXray(bool)` — same
  lock/park discipline as `{Set,Get}ViewportRenderMode`, but NO caster
  rebuild — it just calls `InteractivePelRasterizer::SetXrayView`, which
  applies uniformly across every mode INCLUDING `preview`. **Default ON**
  (2026-07-17 user decision): `true` at construction and after every
  `RebindEditorToJob` reset (scene load/reload/variant switch), which also
  stamps the interactive rasterizer directly so it starts see-through before
  any render happens.
- C-ABI: `RISE_API_SceneEditController_{Set,Get}ViewportXray` (signature
  unchanged).
- Agent: `render` gains an optional boolean `xray` param (`AgentRenderParams::
  xray`, **default TRUE**) — meaningful ONLY under a view-mode `mode`
  (silently ignored, honestly noted in the result message, under
  `beauty`/`objectmap` — same precedent as `quality`/`samples` under those
  targets); pass `xray:false` to inspect the transmissive surface itself.
  Toggle lives in the viewport chrome next to the mode dropdown on both
  GUIs, defaulting to ON.

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
