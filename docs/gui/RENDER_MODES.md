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
| `clay_lights` | T2 | Clay albedo but the scene's REAL lights (the classic "is lighting right independent of materials" check). **P2b, not yet shipped** — needs a clay-albedo-substituting integrator variant, not just a config delta on the real materials (see §9). |
| `direct` | T2 | **SHIPPED P2a (2026-07-18)** as a BeautyVariant pipeline: direct lighting only (`maxBounces=1`, wired via `PathTracingIntegrator::SetMaxPathDepth` so the PT main loop genuinely stops after the camera-hit vertex — no `directlighting_shaderop` substrate needed), half-res, 8 spp, OIDN on. See §6. |
| `indirect` | T2 | Beauty minus direct. **P2b, not yet shipped** — needs a real subtraction (two renders + a diff) or a dedicated indirect-only integrator mode, neither of which the BeautyVariant config-delta mechanism supports on its own (see §9). |
| light solo | T2 | Render with exactly one light enabled. Needs a light-selector arg; P2b design detail. |

### Transport — "what are reflections/refractions doing?"

| Mode | Tier | Notes |
|---|---|---|
| `deep_reflect` | T2 | **SHIPPED P2a (2026-07-18)** as a BeautyVariant pipeline: quarter-res, 16 spp, 24 bounces, OIDN on. See §6. |
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
ACTIVE window for the pass about to run, and resets the accumulators. The
window is a THREE-state machine (`WindowKind::{Unarmed, Ranged, Flat}`): a
degenerate span (a flat wall filling the frame) arms a first-class FLAT
window that shades a constant mid-gray — it is a settled state, not a
refusal — while `Unarmed` (no samples ever) falls back to the fixed
scene-diagonal formula. All four transitions (ranged↔ranged, ranged↔flat)
self-correct via the staleness re-run below.
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
window is STALE (`InteractiveViewModeRayCaster::DepthWindowStale` /
`DepthViewShader::WindowStale`: never armed, or this pass's accumulated
range deviates from the active window beyond tolerance — endpoints past 15%
of the span, span changed >30%, or a ranged↔flat transition) and the pass
was not cancelled — runs the base pass ONCE more. The second pass's `AttachScene` call snapshots the
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
- **P2a review fix (2026-07-18)**: while the ACTIVE mode is a §6
  BeautyVariant row (`deep_reflect`/`direct`), the x-ray toggle is DISABLED
  on both GUIs — those modes drive `mVariantRasterizer`, a wholly separate
  ephemeral PT pipeline that never reads the x-ray flag/caster
  `InteractivePelRasterizer::SetXrayView` stamps, so toggling it while a
  variant mode is active would silently no-op. New registry-level C-ABI
  `RISE_API_GetViewportRenderModeIsVariant(index, bool* out)` (additive
  sibling of `RISE_API_GetViewportRenderModeWantsDenoise`, wraps
  `IsBeautyVariantMode`) is what both GUIs key the disable off of — Mac's
  `viewportRenderModeChip` Toggle gets `.disabled(activeMode?.isVariant ==
  true)`, Windows' `TopBar::refreshRenderModeCombo` reads the active combo
  item's stashed `isVariant` flag (`Qt::UserRole + 1`). This does NOT change
  `{Set,Get}ViewportXray`'s own behaviour or the stored flag — only the
  UI-facing enablement — so returning to a non-variant mode picks the flag
  right back up.

## 6. BeautyVariant pipelines (P2a — SHIPPED 2026-07-18)

The design that shipped is NOT the config-override sketch this section used to
carry (a `ViewportConfigOverride` struct applied at RasterizeScene time, the
"config sibling of `SetViewportCameraOverride`"). That sketch assumed the
*existing* interactive/production rasterizer could be re-parameterized
per-render; in practice a BeautyVariant mode needs a materially different
rasterizer (different max path depth, different spp, different denoise
policy) built against the SAME scene — cheaper and more honest as a wholly
separate, ephemeral pipeline than as a config knob threaded through the
existing one.

**BeautyVariant is a pipeline kind**, alongside `BeautyConfig` and
`ShaderPipeline` (§4): an ephemeral, controller/agent-**owned** production-class
PT pipeline with per-mode config deltas — a **separate `IRasterizer`** the
render loop *drives* while the mode is active, not a caster swap on the
preview rasterizer, and not a wrapper around it. `deep_reflect` and `direct`
are the two shipped rows:

| Field | `deep_reflect` | `direct` |
|---|---|---|
| `variantScaleDivisor` | 4 (quarter-res) | 2 (half-res) |
| `variantMaxBounces` | 24 | 1 |
| `variantSamplesPerPass` | 16 | 8 |
| `wantsDenoise` | true | true |

`ViewportRenderModeInfo` carries these three `variant*` fields (0/0/0 for every
non-variant mode); `IsBeautyVariantMode(mode)` (keyed off
`variantSamplesPerPass > 0`) is the single source of truth for "is this mode a
BeautyVariant row" everywhere the registry is consulted (controller, agent
schemas, tests) — no hardcoded mode-name lists.

### Factory: `CreateBeautyVariantPipeline`

`InteractivePelRasterizer.{h,cpp}` — mirrors the **minimal production-real**
path `Job::SetPathTracingPelRasterizer` takes: a plain `RayCaster`
(`seeRadianceMap=true`, `showLuminaires=true` — **not** the
`InteractiveMaterialPreviewRayCaster` subclass, so shading resolves through
each object's own real `IMaterial` exactly like a production render) +
`RISE_API_CreatePathTracingPelRasterizer` with a multijittered sampler at
`variantSamplesPerPass`, a box reconstruction filter, SMS off, OIDN on
(quality `Auto`), default (disabled) path guiding/adaptive sampling/stability
configs, no Z-Sobol. The caster's `pDefaultShader` constructor argument is a
throwaway placeholder — it's dead code on the PT transport path
(`RayCaster::SelectShader` is never called by production path tracing;
per-object shading is driven entirely by each `IObject`'s own material), so
the factory needs no Job/shader-manager access.  **P2a review fix
(2026-07-18)**: the caster's `maxR` (reused as `variantMaxBounces` for
convenience) is only a harmless SSS/BSSRDF recursion cap
(`RayCaster::CastRay`'s `nMaxRecursions`) — it never bounded the PT main
loop's bounce depth, which is iterative (not recursive) and was hardcoded to
128 regardless.  The factory now calls the new
`PathTracingPelRasterizer::SetMaxPathDepth(variantMaxBounces)` right after
construction, which forwards to `PathTracingIntegrator::SetMaxPathDepth` —
the loop's REAL cap (see that method's doc for the exact depth-accounting:
`n==1` for `direct` means the camera-hit vertex's emission + NEE only, no
continuation bounce ever traced).

### Controller: `mVariantRasterizer` + the render-loop dispatch

`SceneEditController` owns `IRasterizer* mVariantRasterizer` (refcounted;
released in the destructor and by `RebindEditorToJob`'s every-scene-load
reset). `SetViewportRenderMode`, under the SAME cancel-and-park discipline as
the P1 caster-swap path: on a BeautyVariant target, builds the new pipeline
FIRST (fail-closed if the factory refuses), then releases any prior variant
pipeline and installs the new one; on any OTHER target, releases the variant
pipeline (if one was active) and falls back to the P1 caster-swap logic
unchanged. `mInteractiveImpl`'s own caster is deliberately left **untouched**
while a variant mode is active — it still holds whatever Preview/data-mode
caster was installed before the switch, so leaving the variant mode later
finds it exactly as it was.

`DoOneRenderPass`'s rasterize site reads
`IRasterizer* activeRast = mVariantRasterizer ? mVariantRasterizer : mInteractiveRasterizer;`
and drives `activeRast` everywhere the pass previously hardcoded
`mInteractiveRasterizer`: progress-callback + preview-sink attachment (every
pass re-attaches its sink, so the variant rasterizer needs the same
per-pass attachment the preview one gets), `EnsureInteractiveFrameStore_`
(now takes `activeRast` as an explicit parameter instead of always targeting
`mInteractiveRasterizer`), the `SetViewportCameraOverride` free-fly wiring
(`PixelBasedRasterizerHelper` downcast), and the terminal `RasterizeScene`
call. The pre-pass config block in `RenderLoop` that calls
`mInteractiveImpl->SetPreviewDenoiseMode`/`SetSampleCount` is **skipped
entirely** while a variant mode is active — those are InteractivePelRasterizer-
specific knobs for a rasterizer this pass isn't driving; a variant pass is a
plain full pass at its fixed config, and the polish/denoise state machine
(`mPolishState`) is left running as a no-op against `mVariantRasterizer` so
leaving the mode later finds it consistent.

### Resolution-divisor pin

A BeautyVariant mode's fixed resolution divisor is pinned into `mPreviewScale`
itself (the SAME atomic the P1 adaptive preview-scale ladder already uses) so
no second "current resolution" concept is needed. `mPreviewScalePinned` (bool)
gates every site that would otherwise mutate `mPreviewScale` away from the
pinned value: the idle-refinement walk-down, the during-motion adaptation
ladder, and every gesture-driven reset (`OnPointerDown/Move/Up`,
`OnTimeScrubBegin/End`, `Begin/EndPropertyScrub`) — each now routes through
`SetPreviewScaleIfUnpinned_` (or an inline pin check) instead of a bare
`mPreviewScale.store(...)`. `SetViewportRenderMode` sets the pin (and stores
the mode's divisor) on entry to a variant mode, and on exit clears BOTH the
pin AND `mPreviewScale` itself (restored to `kPreviewScaleMin`) — **P2 review
fix (2026-07-18)**: the original exit branch cleared only
`mPreviewScalePinned`, leaving the pinned divisor (e.g. quarter-res for
`deep_reflect`) sitting in `mPreviewScale`, so the viewport stayed at that
stale resolution after leaving the mode until an unrelated pointer gesture
happened to overwrite it. `RebindEditorToJob` also clears both on scene
reload (gated on a variant having actually been active, so a reload that was
never in a variant mode doesn't disturb whatever adaptive scale a live
gesture already set), matching the "release the variant pipeline on scene
reload" reset.

### Agent surface

`AgentRenderParams::renderTarget == ViewMode` covers BOTH the P1 ShaderPipeline
data modes and the P2a BeautyVariant modes — `AgentSession::RenderCore_`
branches internally (`IsBeautyVariantMode(viewModeInfo->mode)`) between
`doViewModeRenderWork` (P1, unchanged) and the new `doBeautyVariantRenderWork`
(a structural sibling: fresh ephemeral pipeline, the agent's own sink, shared
film/camera-override plumbing, controller-owned cancel wiring — but a REAL
shaded+denoised render, so it applies the same beauty display transform the
production/draft paths use, and its film-dims override divides the EFFECTIVE
requested dims — the caller's `width`/`height` override if given, else the
scene's current authored dims — by `variantScaleDivisor`, always, not only
when an explicit override was requested). `quality`/`samples`/`xray` are all
honestly ignored under a BeautyVariant mode (noted in the result message);
`effectiveSamples` reports the mode's REAL fixed spp (16 or 8), not the
ShaderPipeline exactness invariant's `1`. `AgentRpc.cpp`'s `mode` parser and
`AgentMcpAdapter.cpp`'s generated schema description both widen their
registry filter from `casterFactory` alone to `casterFactory ||
IsBeautyVariantMode(mode)`; `AgentChatCodecs.cpp`'s hand-synced JSON-Schema
literal (documented KEEP IN SYNC BY HAND — it's a raw string constant, not
generated) was updated by hand to match.

### `render{view:}` (P2a — the P1-deferred agent arg, now shipped)

Optional string param on `render`, valid with EVERY mode (composes with
Beauty/ObjectMap/any ShaderPipeline data mode/either BeautyVariant mode).
Resolves to the SAME ephemeral `camera` override fields the existing
`camera:{location,lookat,up,fov}` param uses — `AgentSession::RenderCore_`
builds a local `effectiveCamera` (a copy of `params.camera`, overwritten
wholesale by the resolved named-view pose when `view` is non-empty) and feeds
it into the SAME `applyCameraOverride` machinery every render target already
shares, rather than adding a parallel override mechanism.

Resolution order: (1) a live `SceneEditController`'s in-memory named-view
store (`SceneEditController::FindNamedViewPose`, Tier 2 B1's `mNamedViews`);
(2) headless (`AgentSession::WrapJob`, no controller attached), a scene
**camera** of that exact name (`ICameraManager::GetItem` +
`CameraIntrospection::CaptureCameraSnapshot`) — the honest fallback, since a
headless session has no named-view bookmark store at all. An unresolved name
FAILS the render (`res.ok=false`) with the available-name list (named views
UNION scene camera names) in `res.message`, rather than silently falling
through to the active camera.

**Thread-safety note**: `SceneEditController::mNamedViews` was originally
documented "UI-thread-only, never read by the render thread" (no lock). A
`render{view:}` call can run on `SubmitAgentRenderAsync`'s dedicated
agent-render worker thread — the FIRST reader of `mNamedViews` reachable from
off the UI thread. `mNamedViewsMutex` (new) now guards every touch of the
vector (Capture/Count/Name/Restore/Update/Delete/Promote/Find), closing that
hazard rather than leaving it as an accepted gap.

**Units trap (caught by the E2E test, not by inspection)**: `CameraSnapshot::fov`
is stored in RADIANS (`CameraIntrospection::CaptureCameraSnapshot` /
`PinholeCamera::GetFovStored`'s convention), but `CameraIntrospection::
SetProperty("fov", ...)` — and `AgentCameraOverride::fov`'s documented contract
— both take DEGREES, matching the scene-file `fov` param. The first
implementation fed the raw radian value straight into the override string,
which parses as a technically-valid but absurdly narrow FOV (a fraction of a
degree) — both renders came back essentially black, and only the two-camera
`render{view:}` end-to-end test's pixel-difference assertion caught it (the
individual `ok`/`cameraOverridden` flags all read TRUE even with the bug, since
the override mechanically "succeeded"). Fixed by multiplying by `180/π` in
`AgentSession.cpp`'s `CameraSnapshotToOverride`, mirroring
`CameraIntrospection::GetPropertyValue`'s own `RAD_TO_DEG` conversion for the
SAME field. Lesson: an `ok:true` / `cameraOverridden:true` pair proves the
override APPLIED, not that it applied a SENSIBLE value — a units mismatch
still needs a "does the image actually look different" check to catch.

### DENOISED label honesty (both GUIs)

The Mac `RefinementStatusFormatter` / Windows `TopBar::ComputeRefinementStatus`
"Polishing" label used to gate the "DENOISED — NOT FINAL" honesty tag on
`viewportRenderMode == "preview"` — correct for P1 (only `preview` ever
denoised) but wrong now that BeautyVariant modes genuinely denoise too (they'd
show "Polishing" with NO honesty label, which under-claims). Both formatters
now key the gate on the registry's `wantsDenoise` flag instead: `RISE_API_
GetViewportRenderModeWantsDenoise(index, bool*)` (additive C-ABI, sibling of
`RISE_API_GetViewportRenderModeInfo` — that function's signature is
unchanged for ABI stability) is read once per mode alongside the existing
name/title/question fields and cached on each GUI's own `ViewportRenderModeInfo`
mirror struct (Mac: a new `wantsDenoise: Bool` field on the Swift struct;
Windows: same on the C++ struct). The formatter functions themselves take a
`wantsDenoise: Bool` / `bool wantsDenoise` parameter instead of a mode-name
string comparison.

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

- `render{mode:}` widens from `beauty|objectmap` to the full agent-visible
  registry (decision 4): the P1 `casterFactory` data modes UNION the P2a
  BeautyVariant modes (`IsBeautyVariantMode`). Data modes ignore
  `quality`/`samples` exactly as `objectmap` does today (honestly noted in
  the result message); BeautyVariant modes ALSO ignore `quality`/`samples`
  (their config is fixed by the registry) plus `xray` (meaningless for a real
  transport render), all honestly noted — see §6's "Agent surface" for the
  exact dispatch and honesty-note shape.
- **SHIPPED P2a (2026-07-18)**: `render` gaining an optional
  `view:"<named view or scene camera name>"` so the agent composes its own
  "N-up" as sequential renders from saved vantages (no layout tool needed).
  See §6's `render{view:}` subsection for the resolution order and the
  named-view thread-safety fix it required.
- Mode list + questions discoverable via the render tool's schema description
  (generated from the registry — single source of truth) — widened the same
  way (`casterFactory || IsBeautyVariantMode`).
- `skills/agent/observe-modes.md` extends with the question → mode decision
  table and recipes:
  - placement check → `objectmap` top view → `query_object_at`
  - lighting check → `clay` (later `clay_lights`) before touching materials
  - tessellation/smoothing artifact → `facets` / `wireframe`
  - reflection/refraction content → `deep_reflect` (SHIPPED P2a, ¼ res, 16 spp,
    24 bounces)
  - lighting-only check → `direct` (SHIPPED P2a, ½ res, 8 spp, direct only)
  - scale/occlusion sanity → `depth`
  - several saved angles of the same question → any mode + `view:"<name>"`
    (SHIPPED P2a)

## 9. Phasing

- **P1 (shipped)**: registry + T1 shaders (`normals`, `depth`, `facets`,
  `wireframe`) + controller caster-swap + C-ABI + both bridges + viewport
  dropdown + agent `render{mode}` widening + skill update + parity/shader
  tests.
- **P2a (shipped 2026-07-18)**: the BeautyVariant pipeline kind (§6) +
  `deep_reflect` + `direct` + the agent `view:` arg (§8) + the
  `wantsDenoise`-keyed DENOISED label fix (§6) — see §6 for the full shipped
  design (supersedes the config-override sketch this section used to carry).
- **P2b (remaining)**: `indirect` (needs a real subtraction or a dedicated
  indirect-only integrator mode — the BeautyVariant config-delta mechanism
  alone doesn't cover it), `clay_lights` (needs a clay-albedo-substituting
  integrator variant, not just a config delta on the real materials), light
  solo (needs a light-selector arg). All three need integrator-level work
  beyond what the P2a config-delta mechanism provides.
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
- **`deep_reflect`/`direct` are seconds-scale, not milliseconds-scale, unlike
  every other agent-visible mode.** They're real production-class path traces
  (real materials/lights, real OIDN denoise) at a fixed reduced resolution —
  cheap relative to a full-res production render, but not free the way
  `objectmap`/`normals`/`depth`/`facets`/`wireframe` are. `skills/agent/
  observe-modes.md` calls this out explicitly so an agent doesn't default to
  `deep_reflect` for a question a free diagnostic mode already answers.
- While a BeautyVariant mode is active in the interactive VIEWPORT, the
  preview-scale ladder is PINNED to the mode's fixed divisor — it never
  refines toward full resolution the way `preview`/data modes do on pointer-up
  or idle. This is intentional (a variant render at full interactive
  resolution would defeat its own cost budget), not a bug in the adaptive
  ladder.
