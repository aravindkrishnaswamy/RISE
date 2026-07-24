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
| `clay_lights` | T2 | **SHIPPED P2b (2026-07-18)** as a BeautyVariant pipeline: full transport, every surface's BRDF/SPF substituted for a shared neutral mid-grey (0.5 albedo) Lambertian pair (`PathTracingIntegrator::SetClayOverride`), real lights/GI untouched — half-res, 12 spp, 12 bounces, OIDN on. See §6. |
| `direct` | T2 | **SHIPPED P2a (2026-07-18)** as a BeautyVariant pipeline: direct lighting only (`maxBounces=1`, wired via `PathTracingIntegrator::SetMaxPathDepth` so the PT main loop genuinely stops after the camera-hit vertex — no `directlighting_shaderop` substrate needed), half-res, 8 spp, OIDN on. See §6. |
| `indirect` | T2 | **SHIPPED P2b (2026-07-18)** as a BeautyVariant pipeline: beauty minus the direct (emission + NEE) contribution at the camera-visible vertex only, all indirect bounces intact (`PathTracingIntegrator::SetIndirectOnly` — a genuine in-loop suppression, not a two-render subtraction) — half-res, 12 spp, 16 bounces, OIDN on. See §6. |
| light solo | T2 | **SHIPPED P2b (2026-07-18)** as a composable AXIS, not a mode: `render{light:"<name>"}` combines with any BeautyVariant mode or a plain production render. Resolves the name against the caster's own light manager, then object manager (mesh luminaries), via `RayCaster::SetSoloLightByName`; solo state lives on `LightSampler` (`SoloKind::{None,Light,Luminary,Environment}`) and is honoured by NEE, by the BSDF-sampled emission MIS partner (`CachedPdfSelectLuminary` returns 1.0 for the target, 0 otherwise), and by every environment-radiance site. Unresolvable names fail loudly with the available-name list. **PATH TRACING ONLY** — BDPT/VCM/MLT/`auto_*` are refused rather than silently ignoring it (see §10). Unbiasedness is certified by a partition test: `solo(A) + solo(B) == all`. |

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
(P2a) plus `indirect` and `clay_lights` (P2b) are the four shipped rows,
all built by the SAME `CreateBeautyVariantPipeline` factory:

| Field | `deep_reflect` | `direct` | `indirect` | `clay_lights` |
|---|---|---|---|---|
| `variantScaleDivisor` | 4 (quarter-res) | 2 (half-res) | 2 (half-res) | 2 (half-res) |
| `variantMaxBounces` | 24 | 1 | 16 | 12 |
| `variantSamplesPerPass` | 16 | 8 | 12 | 12 |
| `wantsDenoise` | true | true | true | true |
| `variantIndirectOnly` | false | false | **true** | false |
| `variantClayOverride` | false | false | false | **true** |

`ViewportRenderModeInfo` carries the three P2a `variant*` fields (0/0/0 for
every non-variant mode) plus the two P2b booleans
`variantIndirectOnly`/`variantClayOverride` (false for every row except
`indirect`/`clay_lights` respectively); `IsBeautyVariantMode(mode)` (keyed off
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
configs, no Z-Sobol.  **P2a review fix (2026-07-18)**: the caster's `maxR`
(reused as `variantMaxBounces` for convenience) is only a harmless
SSS/BSSRDF recursion cap (`RayCaster::CastRay`'s `nMaxRecursions`) — it never
bounded the PT main loop's bounce depth, which is iterative (not recursive)
and was hardcoded to 128 regardless.  The factory now calls the new
`PathTracingPelRasterizer::SetMaxPathDepth(variantMaxBounces)` right after
construction, which forwards to `PathTracingIntegrator::SetMaxPathDepth` —
the loop's REAL cap (see that method's doc for the exact depth-accounting:
`n==1` for `direct` means the camera-hit vertex's emission + NEE only, no
continuation bounce ever traced).

**`pDefaultShader` is load-bearing, not dead code (review-p2b P1-b, 2026-07-
XX)**: the PT main transport loop never calls through it — per-object shading
there is driven entirely by each `IObject`'s own material — but
`RayCaster::CastRay`/`CastRayNM` ARE called recursively by the BSSRDF disk-
projection and random-walk SSS continuation sub-paths
(`PathTracingIntegrator.cpp`), and those resolve shading via
`RayCaster::SelectShader`, which falls back to the caster's default shader
for any hit object with no explicit per-object `IShader` (the common case —
`AddObject`'s `shader` argument is optional and most scenes only set a
per-object `IMaterial`).  A null/black default here silently zeroes ALL such
continuation energy.  When the caller passes null, the factory now builds a
REAL owned default (`BeautyVariantDefaultShader` — a `StandardShader` over a
`PathTracingShaderOp`, i.e. a generic `standard_shader { shaderop
DefaultPathTracing }` chain), not a black placeholder.  **Review-p3 P2-c**:
both current callers (`SceneEditController::SetViewportRenderMode`,
`AgentSession`'s `doBeautyVariantRenderWork`) go further and recover the
*production* rasterizer's own resolved default-shader NAME via
`job.GetRasterizerParameter(job.GetActiveRasterizerName(), "shader")` — the
exact name every `Set*Rasterizer` call stamped into its registry snapshot at
construction time, not a hardcoded `"global"` guess — and pass
`job.GetShaders()->GetItem(thatName)` as `pDefaultShader`, so a scene's own
authored default shader is used whenever one can be resolved.  **Residual
limitation**: the variant pipeline still falls back to the generic
`DefaultPathTracing` chain above when no rasterizer is active yet or the
resolved name isn't registered, so a scene relying on that fallback whose
`global` shader is a CUSTOM shaderop chain can diverge from a CLI render on
SSS/BSSRDF continuations in that (rare) case — there is no sound way to
force-resolve a name that doesn't exist.

**P2b addition (2026-07-18)**: right after the `SetMaxPathDepth` call, the
factory also stamps `PathTracingPelRasterizer::SetIndirectOnly(info->
variantIndirectOnly)` and `SetClayOverride(info->variantClayOverride)` —
both forwarders onto the same `PathTracingIntegrator`, both false for
`deep_reflect`/`direct` (so those two rows stay byte-identical) and each
true for exactly one of the two new rows:

- **`SetIndirectOnly`** (the `indirect` row): gates the NEE direct-lighting
  contribution, the emission contribution, AND the environment/radiance-map
  contribution a camera ray that misses the scene entirely picks up.  NEE and
  the environment/radiance-map miss are gated at the true camera-visible
  vertex only (a loop-local `depth == 0`, not `depth == startDepth`: the HWSS
  loop's mid-path delegation to the per-wavelength NM path passes its OWN
  current `depth` as the delegated call's `startDepth`, so a `depth ==
  startDepth` check would spuriously fire again at that delegated call's
  first iteration even for a genuine indirect bounce — `depth == 0` has no
  such ambiguity, since only a true top-level camera-ray entry point ever
  passes `startDepth == 0`).  **Review-p3 P2-a fix**: the emission gate is
  `depth <= 1`, not `depth == 0` — the direct term at the camera-visible
  vertex is `NEE@depth0` **plus** `emission@depth1-from-that-vertex` (the
  BSDF/phase-sampled ray fired FROM that vertex that happens to land directly
  on an emitter, i.e. the MIS *partner* of the depth-0 NEE, arriving one loop
  iteration later); gating emission at `depth == 0` only suppressed half the
  direct term and left `indirect` rendering `direct × w_bsdf` on any light
  where the BSDF-sampling strategy has non-negligible pdf toward it (a small/
  distant lamp hid this since its `w_bsdf ≈ 0`; a large area light or bright
  env leaked a large, measurable fraction).  `depth <= 1` has the same no-
  ambiguity property as `depth == 0` (every mid-path re-entry — the HWSS
  delegation, the BSSRDF continuation's recursive caster call, and the medium
  in-scattering continuation's `startDepth = 1` delegate — passes the CURRENT
  nonzero depth onward, so the loop-local `depth` can never return to 0 or 1
  partway through a path that has already gone deeper).  The continuation ray
  is still sampled and traced at every vertex — only the direct contribution
  at/just past the primary hit is zeroed, so genuinely indirect energy
  (`depth >= 2` emission, `depth >= 1` NEE) is untouched.  SMS caustic
  contributions are deliberately NOT gated (a caustic reaching a vertex
  through a specular chain is genuinely multi-bounce transport, not the
  open-air NEE connection).
- **`SetClayOverride`** (the `clay_lights` row): substitutes a single
  shared, stateless, mid-grey (0.5 albedo) `LambertianBRDF`/`LambertianSPF`
  pair (wrapping one `UniformColorPainter`, all three built once in the
  `PathTracingIntegrator` constructor and released in its destructor) for
  EVERY surface's real BRDF/SPF, at both the BRDF acquisition and both SPF
  acquisition sites (the specular/no-BRDF branch and the PART 3
  continuation) in the templated Pel/NM loop, plus their HWSS twins.
  Because the substitution makes every hit report a non-null BRDF, the
  "no-BSDF" branch (previously reached by delta-only materials like
  mirrors/glass) is never taken under `clay_lights` either — every surface,
  including originally-specular ones, scatters as clay. The emitter
  accessor is never substituted, so real lights and real emissive
  geometry keep emitting exactly as authored, and the clay BRDF/SPF
  receive real NEE/BSDF-sampled illumination from the scene's actual
  lights and actual multi-bounce GI.

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

**Status: DESIGN (2026-07-20).**  Grounded in a code audit of the shipped
single-viewport architecture; every load-bearing claim cites file:line.  The
five ratified decisions (§1) still bind — in particular decision 3 (fixed
layout set, max 4 panes).

### 7.0 Thesis and scope

One window renders up to four simultaneous views of the same scene — different
render modes, different vantages — with **exactly one interactive render
thread** multiplexing them.  The pane set is a HUMAN ergonomic for the GUI
viewport; the agent already composes its own N-up as sequential
`render{mode:, view:}` calls (§8) and does not need the layout machinery.

Out of scope: Android (small screens; desktop-first), adjustable splitters
(decision 3: fixed layouts), per-pane display transforms (decision 1: display
cluster is not a render mode), production renders (single-view, unchanged),
and any second render thread.

### 7.1 What exists (audit summary) — the singleton row

Everything viewport lives one-per-`SceneEditController` (one controller per
window/bridge; the class is NOT a process singleton —
[SceneEditController.h:57](../../src/Library/SceneEditor/SceneEditController.h)):

| Concern | Member(s) | N-up disposition |
|---|---|---|
| Render thread + loop | `mRenderThread` :2802, `RenderLoop()` :2395, `DoOneRenderPass()` :2357 (virtual, test-overridable) | STAYS SINGULAR — becomes the pane scheduler |
| Interactive rasterizer | `mInteractiveRasterizer` :2611 | stays singular; casters swap per quantum |
| BeautyVariant rasterizer | `mVariantRasterizer` :2626 (+ scale pin `mPreviewScalePinned` :3197) | per-pane (only for panes in a variant mode) |
| Frame buffer | `mInteractiveFrameStore` :2777 (+ `EnsureInteractiveFrameStore_` :2583) | **per-pane FrameStore** |
| Camera override | `mViewportPose`/`mViewportPoseActive`/`mViewportOverrideCamera` :3298-3300, read at the single camera-read site (SceneEditController.cpp:1354) | per-pane |
| Render mode | `mViewportRenderMode` :2631 (+ caster via `InstallViewModeCaster_` :2608) | per-pane |
| X-ray flag | `mViewportXray` :2639 | stays GLOBAL in v1 (one toolbar toggle; casters are per-pane so per-pane x-ray is a cheap later polish) |
| Refinement ladder | `mPreviewScale` :3187 (1..32 divisor), `mPolishState` :3258, `mFullResW/H` :3184 | per-pane |
| Preview sink | `mPreviewSink` :2750, C-ABI `_SetPreviewSink` :3502 | per-pane sink |
| Park discipline | `mMutex` / `CancelAndParkRender_` / `mRenderOwnsScene` :3081 | UNCHANGED — global, one thread |

Existing affordances that make this tractable: `ViewportFrameStore` is
refcounted and explicitly multi-instance
([ViewportFrameStore.h:89](../../src/Library/Rendering/ViewportFrameStore.h));
`FrameStore` is multi-reader/one-writer (per-tile shared_mutex);
`CameraSnapshot` + `RealizeStandaloneCamera` (SceneEditController.cpp:141)
produce a standalone `ICamera` with zero scene mutation — N vantages are a
natural extension; and the agent render path already proves an alternate
vantage renders through the same controller without disturbing the
interactive loop.

### 7.2 Pane model

Four **always-present pane slots** on the controller; the layout selects the
visible subset.  Hidden panes keep their configuration (toggling `2×2 → 1 →
2×2` must not lose setups) and never render.

```
struct ViewportPane {
    // --- configuration (UI-mutated under mMutex + park, like every
    //     viewport setter today) ---
    mode          : registry wire name (default "preview")
    vantageKind   : SceneCamera | FreeFly | NamedView
    pose          : CameraSnapshot        // FreeFly, or snapshot fallback
    namedViewRef  : string                // NamedView: re-resolved via
                                          // FindNamedViewPose each pass;
                                          // falls back to `pose` snapshot
                                          // if the view was deleted
    // --- render plumbing (render-thread-owned; swapped only while parked) ---
    overrideCamera   : ICamera*      // null => scene active camera
    frameStore       : FrameStore*   // per-pane canonical HDR buffer
    viewModeCaster   : per-pane caster instance (depth auto-window state
                       lives here and must survive other panes' quanta)
    variantRasterizer: IRasterizer*  // only when mode is a BeautyVariant
    // --- refinement state (render-thread-owned) ---
    previewScale, polishState, dirty, generation
}
```

Layouts: `Single` (pane 0), `TwoH` (0|1), `OnePlusTwo` (0 big + 1,2 stacked),
`Quad` (0-3).  Pane rects are fixed fractions computed GUI-side; the C++ core
only needs per-pane pixel dims (`SetPaneSurfaceDims`).  **Primary** must be
visible; when a layout shrink hides the primary, primary falls back to pane 0.
The core initializes every pane to `{mode: preview, vantage: SceneCamera}`.
On a pane's first successful reveal, both shells apply the session-local
applied-once spread: pane 1 = `indirect`, pane 2 = `facets`, pane 3 =
`direct`.  `OnePlusTwo` therefore reveals the first two roles and `Quad`
adds the third.  A pane with persisted non-Preview state is already
user-owned, and after the first successful preset application every explicit
choice—including an explicit return to `preview`—survives layout toggles.

Vantage semantics: a `SceneCamera` pane tracks the live active camera (edits
to it re-render the pane).  Tumbling in a SECONDARY pane converts that pane
to `FreeFly` seeded from what it was showing — the per-pane generalization
of the existing enter-free-fly rule, and like it, never mutates the scene
camera.  **Pane 0 retains the classic direct-camera-edit navigation** (orbit
/ pan / zoom / roll mutate the scene camera through the edit/undo system,
exactly as the single viewport does today) — pane 0 IS the legacy-alias
editing surface, and silently converting its navigation to free-fly would
break the established orbit-edits-camera workflow both GUIs are built on.
(Implementation-time refinement, 2026-07-21: the original text said "ANY
pane"; amended when the pointer-routing implementation surfaced the
back-compat conflict.)  A `NamedView` pane re-resolves by name so updating
the view updates the pane.

### 7.3 Scheduler (the render loop, generalized)

Still ONE thread, one in-flight `RasterizeScene` at a time; the park/cancel
discipline is untouched (parking cancels + drains the CURRENT quantum,
whichever pane it belongs to).

**Quantum** = one `RasterizeScene` call for one pane at that pane's current
refinement state: install the pane's caster (or route to its variant
rasterizer), stamp its override camera, point `EnsureInteractiveFrameStore_`'s
`activeRast` wiring at its FrameStore, run, publish to its sink.

**Context-switch model (implementation refinement, 2026-07-20).**  The
existing render loop is ~400 lines of accumulated race fixes (six documented
fix-rounds) whose reasoning is stated over the SINGULAR state members
(`mPreviewScale`, `mPolishState`, `mInRefinementPass`, `mVariantRasterizer`,
...).  Those members are NOT generalized in place.  They remain the loop's
**working registers** for the current quantum; each pane's state is
SAVED/RESTORED around quanta by the scheduler, under the same mint-lock
`mMutex` hold where the loop already does its in-lock re-checks — the same
"mutate only while parked" discipline every viewport setter uses.  The loop
body (and every fix-round comment's reasoning) survives verbatim.  The
UI-thread writers stay coherent because of gesture exclusivity: gesture
handlers only ever adapt the pane being interacted with, which is BY RULE the
currently-scheduled pane, so writes to the registers always target the pane
that owns them.  One new invariant: register swaps happen only while parked,
under `mMutex`.

**Rotation policy** (idle refinement):

1. Rotation set = visible panes with `dirty || previewScale > 1 || polish
   pending`.  Panes that finish (variant budget spent; data mode polished at
   scale 1) leave the set; empty set ⇒ thread sleeps on `mCV` as today.
2. Order within a round: **primary first**, then secondaries by index.  Equal
   shares (one quantum each per round) — primary-weighted shares are a
   revisit-if-needed, not v1.
3. Each pane walks its OWN resolution ladder (per-pane `previewScale`),
   reusing the existing idle-refinement constants; per-pane polish (denoise
   keyed on the registry's `wantsDenoise`, exactly the §6 rule) runs when a
   pane reaches scale 1 idle — primary polishes first by rule 2.

**Gesture exclusivity**: during a pointer gesture in pane *i* (tumble, drag),
the scheduler renders pane *i* EXCLUSIVELY with the existing during-motion
scale adaptation (kTargetMs=33); other panes freeze at their last frame.  This
is the generalization of today's behavior (the single pane IS the gestured
pane) and keeps interaction latency identical to single-viewport.  A completed
gesture quantum does **not** self-arm from those intentionally-frozen panes'
dirty bits: each in-gesture edit supplies its own kick, and gesture-end supplies
the kick that resumes ordinary rotation.  This distinction is load-bearing for
BeautyVariant panes, whose fixed divisor/sample budget would otherwise turn one
gizmo edit into an infinite stream of expensive pinned-pane passes (T0 fix,
2026-07-24).

**Invalidation matrix**:

| Event | Effect |
|---|---|
| Scene edit (any mutation) | all visible panes dirty, scales reset coarse; primary renders first |
| Pane-local: mode / vantage / named-view update | that pane dirty only |
| Layout switch | newly-visible panes dirty; hidden panes dropped from rotation |
| Pane resize (layout change / window resize) | affected panes dirty (store realloc via the existing same-dim short-circuit) |
| Production/agent render starts | `mRenderOwnsScene` no-ops pane setters; loop yields (existing single-slot coordination); on completion ALL panes dirty |

### 7.4 C-ABI (additive; existing calls = pane 0)

New `RISE_API_SceneEditController_*` entries, all taking a pane index; every
EXISTING viewport call keeps its signature and aliases pane 0, so current
platform code keeps working unmodified:

```
_SetViewportLayout(layout) / _GetViewportLayout()
_SetPrimaryPane(pane) / _GetPrimaryPane()
_SetPaneRenderMode(pane, name) / _GetPaneRenderMode(pane)
_SetPaneVantageSceneCamera(pane)
_SetPaneVantageNamedView(pane, name)
_PaneEnterFreeFly(pane) / _PaneExitFreeFly(pane)   // per-pane free-fly twins
_SetPaneSurfaceDims(pane, w, h)
_SetPaneSink(pane, IRasterizerOutput*)             // per-pane push sink
_GetPaneRefinementStatus(pane, ...)                // per-pane phase+scale
_OnPanePointerDown/Move/Up(pane, ...)              // input carries pane id
```

Fail-closed like every existing setter: unknown pane / hidden pane / render
owns scene ⇒ false, nothing mutated.

### 7.5 GUI shells

Both platforms, same shape (parity convention): a layout picker (4 fixed
icons) in the viewport toolbar; per-pane chrome = mode dropdown (the existing
registry-driven control, §4) + vantage menu (Scene camera | named views list |
Free-fly) + a primary marker.  The platform builds one
`ViewportFrameStore`/sink per pane (Mac `ViewportPreviewSink`
[RISEViewportBridge.mm:171] and the Qt `imageUpdated` path generalize by
instantiation, not redesign).  Input: the shell hit-tests pane rects and
forwards events with the pane index.  Gizmo/nav overlays
(`ViewportGizmoOverlay.swift`, Qt `QPainter` overlays) draw on the PRIMARY
pane only.

### 7.6 Composition with RenderCoordinator

[RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) (DESIGN, no code) arbitrates
render consumers process-wide.  The pane set is **one** consumer: one client,
one lease, internal pane multiplexing.  Suspend/resume applies to the whole
pane set.  Nothing here depends on the coordinator landing, and nothing
contradicts it.

### 7.7 Phasing + tests

- **P3a (core)**: pane slots + scheduler generalization + per-pane
  stores/casters/overrides + C-ABI.  Tests exploit `DoOneRenderPass` being
  virtual: a recording override asserts the (pane, scale) sequence — priority
  order, gesture exclusivity, the invalidation matrix, rotation exit — with
  no real rendering.  Per-pane isolation test: depth auto-window state in
  pane A unmoved by pane B's quanta (the P1 process-unique spatial
  generations make this checkable).
- **P3b (shells)**: Mac + Windows together, per parity convention.
- **P3c (polish)**: persistence, agent introspection, per-pane x-ray —
  scope per the open decisions below.

### 7.8 Ratified decisions (2026-07-20)

1. **Primary-switch gesture: click promotes.**  A non-navigation click in any
   pane makes it primary (DCC-standard); navigation drags never steal
   primary.
2. **Persistence: session-only in v1.**  The pane set resets on relaunch and
   joins the named-view sidecar mechanism when that lands — no second
   persistence path invented for it.
3. **Agent surface: introspection only.**  Agents can query layout +
   per-pane `{mode, vantage, primary}` to reason about what the user is
   looking at; they never control the pane set (their own multi-view is
   sequential `render{mode:, view:}`, §8).

### 7.9 User-review remediation (2026-07-21)

An external review of the shipped P3 flagged 6 P1 + 7 P2 correctness issues.
All are fixed; this subsection is the corrected record (it **supersedes** the
earlier "camera-override read stays a pane-0 alias" / "gizmo scoped to pane 0"
statements in §7.1's audit table and §7.5).

- **P1#1 — per-pane sink isolation.**  `FreeRasterizerOutputs()` now runs
  unconditionally before a pass, and the legacy preview-sink fallback is
  restricted to pane 0.  A secondary pane without its own sink no longer
  publishes into pane 0 (or the previously-attached pane).
- **P1#2 — pick/gizmo use the pane's camera.**  A new
  `EffectiveViewportCamera_(scene)` (the override register when free-fly is
  active, else the scene camera) replaces the raw `scene->GetCamera()` at
  `PickAt` + the four gizmo-projection sites.  Correct for every pane during a
  gesture (a gesture makes that pane current).  *(Round-2 concurrency fix: the
  non-gesture readers `RefreshGizmoHandles`/`RefreshNavGizmo` take `mMutex`
  (+`mRenderOwnsScene` guard) so their override-register read can't race the
  render thread's `SwitchToPaneLocked_`; the gesture-pinned readers stay
  lock-free — the render can't rotate while pinned.  See the
  `EffectiveViewportCamera_` invariant comment.)*
- **P1#3 — `read_viewport` atomicity.**  The frame's source pane is captured
  **inside** the parked copy (`CopyInteractiveFrame`'s new `outSourcePane`,
  read under `mInteractiveFrameStoreMutex`) and the RPC overrides
  `panes.sourcePane` with it — image + `sourcePane` now describe the same frame.
- **P1#4 (Windows) — scale-invariant pointer mapping.**  `paneSurfacePoint`
  maps through the stable authored film dims (`cameraSurfaceDimensions()`), not
  the pane's last-delivered image size, so an adaptive preview-scale change
  mid-drag no longer jumps the mapped coordinate.
- **P1#5 (macOS) — non-blocking refinement poll.**  `GetPaneRefinementStatus`
  answers a coarse "Rendering" without `mMutex` while a render owns the scene,
  so the 0.5 s status poll can't wedge the UI (including Cancel).
- **P1#6 (macOS) — nav overlay targets the primary pane.**  The free-fly / nav
  funnel (`SetViewportPose`, `ExitFreeFly`, `IsFreeFlyActive`,
  `GetViewportPose`, `EnterFreeFlyFromActiveCamera`) takes an explicit target
  pane: the default `kViewportNavPrimary` sentinel resolves (under the lock) to
  `mPrimaryPane` for the nav overlay, while the **pane-0 alias forwarders**
  (`Set/GetPaneVantage*(0)`, `PaneEnter/ExitFreeFly(0)`) pass an EXPLICIT `0` so
  they keep operating on pane 0 regardless of which pane is primary.  In single
  view `mPrimaryPane==0` so the default path is byte-identical.  A secondary
  pane's free-fly is persisted into its `mPaneConfigs` vantage; `StampView`
  reads the primary pane's pose.  Scheduler test **(k)** mutation-verifies the
  alias isn't corrupted by a secondary primary.  *(Round-2 fix of a regression
  the first cut introduced — repointing the funnel at `mPrimaryPane` without
  the explicit-0 split had made the pane-0 chrome forwarders hit the primary.)*
- **P2#1 — layout shrink relocates a hidden active pane.**  `SetViewportLayout`
  cancels/parks an in-flight pass on a now-hidden pane, drops a gesture pinned
  to it, and switches the scheduler to the visible primary.  Scheduler test
  scenario (j).
- **P2#2 — preset applied once per pane.**  Both shells track which panes the
  preset touched instead of inferring "untouched" from a pane still reading
  `preview`, so an EXPLICIT Preview choice survives layout changes.
- **T1 — first-reveal layout spread (2026-07-24).**  The existing applied-once
  shell preset now assigns pane 1 = `indirect`, pane 2 = `facets`, and pane 3
  = `direct`.  `OnePlusTwo` reveals the first two roles; `Quad` adds the third.
  Mac and Windows use the same wire names and retain P2#2's success-only
  bookkeeping, so a refused setter retries and an explicit user choice is
  never clobbered by a later layout toggle.
- **P2#3 — primary-pane gizmo on both shells.**  Windows now paints the object
  gizmo on whichever pane is primary (was pane-0-only), matching macOS.  Both
  are exact during an object-transform gesture; the static idle-projection
  through a sibling pane's camera between gestures is a documented bound (a
  race-free primary-pane camera snapshot — avoiding the sibling-slot UAF the
  render thread's `release()` would create — is the follow-up).
- **P2#4 (macOS) — Inspector/Outliner refresh on a pane pick** via the new
  `onSelectionMayHaveChanged` callback (drives the shared `propertyRefresh`).
- **P2#5 — region tool gated to Single.**  N-up panes have no region-drag
  gesture, so the chip is disabled (with an explanatory tooltip) outside the
  single viewport rather than stranding a "· armed" state.
- **P2#6 (macOS) — stale-bridge identity guard.**  The pane-image callbacks
  drop any frame whose owning bridge is no longer `viewportBridge`, so a frame
  already dispatched by a shut-down bridge can't overwrite a freshly-loaded
  scene.
- **P2#7 (Windows) — DPI cross-display refresh.**  MainWindow forwards
  `ScreenChangeInternal` to `ViewportWidget::refreshForDpiChange()`, which
  re-pushes the pane device-pixel dims (a fixed-size window firing no
  resizeEvent otherwise left them stale for the new screen).
- **T0 — gesture-pinned rotation no longer self-arms forever (2026-07-24).**
  A scene edit during a gesture dirties every visible pane, while gesture
  exclusivity deliberately freezes the siblings.  The completion path used to
  observe those dirty siblings, set `mPanePassPending`, and immediately render
  the current pane again because `PickNextVisiblePaneLocked_` correctly honored
  the gesture pin.  The current pane's dirty bit was already clear, the siblings
  stayed dirty, and the cycle repeated until pointer-up.  Under `indirect` this
  was a continuous fixed 12-SPP/OIDN render and appeared to hang the Mac GUI.
  End-of-quantum rotation arming is now suppressed while a pointer/property
  gesture is active; per-edit kicks still repaint the gestured pane.  Pointer-up
  atomically closes the pin, installs the correct pane's final/polish state, and
  publishes the all-pane wake; `EndPropertyScrub` supplies the equivalent kick.
  The lost-property-End watchdog and `StopInteractive` lifecycle boundary now
  perform the same recovery—even when a gesture begins while refinement is
  already paused—so dropped platform callbacks cannot strand panes or block
  coordinated renders.  Repeated `StopInteractive` remains nonblocking while a
  coordinated render owns the scene, and a plain no-gesture Pause preserves an
  owed refinement/polish state.  Gesture exclusivity also covers the live
  pane-register set: pane-0 mode/pose/ExitFreeFly setters refuse a context
  switch during a secondary-pane gesture instead of redirecting its next camera
  edit.  Layout shrink treats
  `mGesturePane` as pointer-only state instead of cancelling an unrelated
  property scrub from a stale pane number, and a concurrent property scrub
  keeps its motion-quality divisor when the hidden pointer pane is finalized.
  Scheduler scenarios (c2–c5/j2b) execute a real interactive pipeline and cover
  object-gizmo, property-watchdog, lifecycle/pause, setter exclusion, and shrink
  paths.  Removing the pin guard is RED (12 focused failures and tens of
  thousands of redundant low-resolution passes); restoring it is GREEN.

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
  - lighting check → `clay` before touching materials, `clay_lights` (SHIPPED
    P2b, ½ res, 12 spp, 12 bounces) once real lights/GI need to be in the
    picture too
  - tessellation/smoothing artifact → `facets` / `wireframe`
  - reflection/refraction content → `deep_reflect` (SHIPPED P2a, ¼ res, 16 spp,
    24 bounces)
  - lighting-only check → `direct` (SHIPPED P2a, ½ res, 8 spp, direct only)
  - where does bounced light land? → `indirect` (SHIPPED P2b, ½ res, 12 spp,
    16 bounces, beauty minus the direct/emission contribution)
  - check the lighting rig without material distraction → `clay_lights`
    (SHIPPED P2b — see above)
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
- **P2b (COMPLETE 2026-07-18: `indirect` + `clay_lights` + light solo)**:
  `indirect` and `clay_lights` are BOTH new
  `PathTracingIntegrator` config flags (`SetIndirectOnly`/`SetClayOverride`,
  the same shape as P2a's `SetMaxPathDepth`) rather than the
  two-renders-and-diff / re-wired-materials sketches this entry used to
  carry — see §6's "Factory" subsection for the exact gating/substitution
  sites. Light solo did indeed need the genuinely new integrator-level knob
  this entry predicted (which light(s) NEE and BSDF-sampled emission are
  allowed to see) rather than an extension of either flag — it landed as
  solo state on `LightSampler` plus matching suppression in
  `PathTracingIntegrator`'s emission and environment sites, exposed as the
  composable `render{light:}` axis rather than as a mode. Its correctness
  property is the partition identity `solo(A) + solo(B) == all`, which two
  fixtures guard: one for energy conservation, one (large close emitters)
  where the BSDF-hit strategy is load-bearing so that an NEE/MIS pdf
  disagreement is actually visible.
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
