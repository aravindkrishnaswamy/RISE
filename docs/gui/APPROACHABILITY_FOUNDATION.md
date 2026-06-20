# RISE GUI — Approachability Foundation (Direction A)

**Status:** DESIGN. No code. Deep-dive spec spun off from [GUI_ROADMAP.md](../GUI_ROADMAP.md) §5 (Direction A), §11 (Phase 1), and §13 spikes #1–#2.
**Owner:** Aravind Krishnaswamy
**Scope:** Make a freshly-opened RISE produce a good image with zero clicks, make the viewport behave like the renderer (progressive, pause-on-navigation, restart-on-edit), and let users *assign* materials/HDRIs by dragging thumbnails — all built so macOS, Windows, and (staged) Android stay in lock-step by construction. This is the interaction-and-approachability layer the roadmap calls the highest-leverage gap. It leans on engine pieces RISE already owns: the `auto_rasterizer` integrator dispatcher, always-on OIDN, the shipped `SceneEditController` cancel-restart render loop, the multi-level adaptive preview scaler, `IJobPriv::ScaleFilmToFit` / `IScenePriv::ResizeFilm`, and the `IRasterizer::RasterizeScene(scene, const Rect* pRect, …)` region parameter that is already plumbed but always passed `nullptr` today.

This doc honors the six [GUI_ROADMAP.md](../GUI_ROADMAP.md) §1 principles, especially **Principle 2 (maximize shared C++)** and **Principle 4 (approachable by default, ≤2 disclosure levels)**. Every Direction-A item below states explicitly what is **shared C++ library** vs **platform-specific shell**, and carries an **Android tier note** (§10.4 of the roadmap).

---

## 0. One-paragraph thesis

RISE already converges on a believable image without the user picking an integrator (`auto_rasterizer`) or fighting noise (always-on OIDN). What is missing is the *first thirty seconds*: an empty app that needs a hand-authored `.RISEscene` before it shows anything; a viewport that only previews the scene the file declared rather than inviting exploration; and no way to try a material or a lighting environment without text-editing a chunk. Direction A closes that gap with six features that are mostly thin UI over machinery that already exists. The competitive north star is KeyShot — *"the Real-Time View **is** the renderer; drag a material onto the part with a hover preview and it commits on release; drag an HDRI into the view to relight; the node graph is opt-in"* ([KeyShot: Assigning Materials](https://manual.keyshot.com/manual/materials/assigning-materials/)) — adapted to a converging spectral path tracer instead of a near-real-time GPU previewer.

---

## 1. The six items at a glance

| # | Item | New machinery? | Hardest sub-problem | Phase |
|---|---|---|---|---|
| A1 | Sensible default scene | No (composes existing chunks) | Where the seed scene + assets live; license of shipped HDRI | 1 |
| A2 | Viewport *is* the renderer | No (re-uses cancel-restart + adaptive scaler) | Explicit Start/Pause/Restart/Stop state machine + convergence/stop-rule | 1 |
| A3 | Drag-to-assign material / HDRI | Library: edit + reload orchestration **+ the drag hit-test (`PickAt`)**. Platform: raw pointer capture + thumbnail/highlight widgets | **Spectral hover-preview cost** (§13 #1) — likely commit-on-release | 1–2 |
| A4 | Region / ROI render | No (the `pRect` param already exists) | Auto-clear so a region never leaks into a final | 1 |
| A5 | Thumbnail asset library + seed set | Library: index + scan + thumbnail render. Platform: grid widget | Thumbnail generation cost & cache invalidation | 1 |
| A6 | Progressive disclosure + Simple/Full presets | Library: per-row Basic/Advanced tag in the descriptor | Tagging every descriptor row without a second UI code path | 1 |

A5 must ship at least a minimal seed set **in the same phase as A3** — "an empty library defeats drag-to-assign" ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §5).

---

## 2. Architectural ground truth (what we build on)

Confirmed by reading the tree (ignoring `.claude/worktrees/`):

- **`SceneEditController`** ([src/Library/SceneEditor/SceneEditController.cpp](../../src/Library/SceneEditor/SceneEditController.cpp)) is the single shared brain. Both desktop bridges — `RISEViewportBridge` (Obj-C++, [build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h](../../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h)) and `ViewportBridge` (Qt, [build/VS2022/RISE-GUI/ViewportBridge.cpp](../../build/VS2022/RISE-GUI/ViewportBridge.cpp)) — are thin marshaling layers over the same C-ABI. The Swift `ViewportView` / Qt `ViewportWidget` are "purely a thin sink + event router" (their own words).
- **Cancel-restart loop + multi-level adaptive scaler already shipped** ([INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md) §19.7): six preview-scale levels {1,2,4,8,16,32}, during-motion adaptation, resume-after-pause snap on `OnPointerMove`, idle refinement walking toward scale=1 over ~500 ms, plus a 4-SPP post-release polish pass with elevated max-recursion. A2 *formalizes the controls around this*; it does not re-implement it.
- **`IScenePriv::ResizeFilm(w,h,pixelAR)`** ([src/Library/Interfaces/IScenePriv.h:240](../../src/Library/Interfaces/IScenePriv.h)) mutates Film dims in place and re-syncs every camera's `Frame`. Its **concurrency contract is explicit**: it writes non-atomic stores, so it must NOT run concurrently with rendering — callers `Stop()` the editor render thread first. `IJobPriv::ScaleFilmToFit` ([src/Library/Interfaces/IJob.h:3033](../../src/Library/Interfaces/IJob.h)) wraps it for the "fit preview to screen" case the bridges already call at scene-load.
- **The `pRect` region parameter is already in the rasterizer interface AND already has a library entry point.** `IRasterizer::RasterizeScene(scene, const Rect* pRect, seq)` ([IRasterizer.h:72](../../src/Library/Interfaces/IRasterizer.h)) — *"Region in the scene to rasterize; if NULL, rasterizes the entire scene"* — and `Job::RasterizeRegion` ([Job.cpp:8806](../../src/Library/Job.cpp)) already builds a `Rect` and forwards it. The **pixel** integrators honor it (via `PixelBasedRasterizerHelper::BoundsFromRect`); the gap is purely **GUI wiring** — every controller `RasterizeScene` call passes `nullptr` today ([CURRENT_STATE_AUDIT.md §5](CURRENT_STATE_AUDIT.md)). A4 is "stop passing nullptr," not new engine code. (Region caveat under MLT: §8.)
- **`AutoRasterizer` already does a film-resize round-trip** ([src/Library/Rendering/AutoRasterizer.cpp:489-540](../../src/Library/Rendering/AutoRasterizer.cpp)): it `ResizeFilm`s down to a probe resolution, renders, and `ResizeFilm`s back to the original. It is a working *sizing* template for A2's preview-resolution control. **It is NOT the architecture to copy for A3/A5 thumbnails:** that live-film round-trip is safe only because it runs single-threaded inside a single render's `call_once`, pre-fan-out — a thumbnail that did it alongside a live viewport would tear the film and double-book the pool. Thumbnails/previews are isolated coordinator jobs (snapshot scene + private film), per [RENDER_COORDINATOR.md §5](RENDER_COORDINATOR.md).
- **No drag-drop, asset-library, or thumbnail code exists yet** in either GUI — A3/A5 are greenfield (the only grep hit, `FilmIntrospection.cpp`, is unrelated).
- **`PixelBasedRasterizerHelper::GetLastRenderedImage()`** persists the last framebuffer ([INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md) §19.8) — reusable for "save the swatch I just rendered" in A5.

The pay-off of this audit: **A2 and A4 add almost no engine code; A1/A5/A6 are data + orchestration; A3 is the only item with a genuine open feasibility question.** That question is the spectral hover-preview (§13 #1), addressed head-on in §5.

---

## 3. Competitive findings (cited)

| Source | Finding we adopt |
|---|---|
| [KeyShot: Assigning Materials](https://manual.keyshot.com/manual/materials/assigning-materials/) | *"Before you drop the material onto the part, you will see a preview of the material on the part beneath the mouse cursor. The material will not be assigned until you let go of the left mouse button."* Works in the **Real-Time View**. Modifiers: **Shift** = multi-material, **Alt** = retain textures, **Ctrl/Cmd** = retain labels. → A3 commit-on-release + modifier semantics. |
| [KeyShot: Material Tab](https://manual.keyshot.com/manual/user-interface/project-window/material-tab/) / [Environment Settings](https://manual.keyshot.com/manual/environments/environment-settings/) | Drag an HDRI from the Environment list "into the real-time view… the lighting updates" immediately. Thumbnail libraries are the primary navigation. → A3 drag-HDRI-to-relight, A5 library-first browsing. |
| [Blender Cycles X](https://code.blender.org/2021/04/cycles-x/) | Progressive viewport that refines over time; the modern bar is a continuously-improving preview, not a one-shot. → A2 progressive refinement framing (already true in RISE; we surface it). |
| [Arnold RenderView (ARV)](https://help.autodesk.com/cloudhelp/ENU/AR-Maya/files/am-Arnold_for_Maya_User_Guide/rendering/arnold_for_maya_rendering_am_Arnold_RenderView_Window_html) | IPR that *"progressively renders… increasing AA samples in steps"*, **each scene change triggers a render update**, plus **region render** and **isolate-selected**. → A2 restart-on-edit + convergence indicator; A4 region render. |
| [Octane forum: inactive viewport updating](https://render.otoy.com/forum/viewtopic.php?f=28&t=75296) | Users actively want to *pause* a progressive viewport during navigation / when it is not focused, to reclaim cycles. → A2 explicit pause + pause-on-navigation. |
| [Blender Manual: View Regions](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/regions.html) | *"If you define a render region while in Camera View, it will apply not just to the viewport, but also to the final render."* This is the **region-leaks-into-final footgun**. → A4 auto-clear region before any production render + a visible badge. |
| [NN/g: Progressive Disclosure](https://www.nngroup.com/articles/progressive-disclosure/) | **Two disclosure levels maximum** — *"Designs that go beyond 2 disclosure levels typically have low usability."* Initial display = frequently-needed core; secondary = rarely-used/advanced. Improves learnability, efficiency, error rate. → A6 hard cap at two levels (Basic / Advanced). |
| [NN/g: Onboarding — skip it when possible](https://www.nngroup.com/videos/onboarding-skip-it-when-possible/) + [useronboard.com: Sensible Defaults](https://www.useronboard.com/onboarding-ux-patterns/sensible-defaults/) | Get the user to the "win" state with minimal effort; sensible defaults *"eliminate steps on the path to value."* → A1 zero-click good image; A6 default to the Simple layout. |

---

## 4. The shared-vs-platform contract (Principle 2, made concrete for Direction A)

Everything that is not a native widget, a GPU present surface, a file/credential dialog, or input-event translation lives in `src/Library/`. For Direction A specifically:

| Sub-capability | Shared C++ (library) | Platform-specific (thin shell) |
|---|---|---|
| Default-scene resolution + load (A1) | seed-scene discovery, media-path wiring, "is this a blank session?" check | "New from default" menu item |
| Render-state machine: Start/Pause/Restart/Stop + convergence + stop-rule (A2) | **all of it** — extend `SceneEditController` | toolbar buttons + a convergence readout label |
| Preview-resolution / quality control (A2) | **all of it** (already in the adaptive scaler) | a quality slider that calls one setter |
| Drag-to-assign: edit + reload + render orchestration (A3) | **all of it** — a new `ApplyAssetToTarget` op on the controller routed through `SceneEditor`; **plus the drag-over hit-test itself** — `SceneEditController::PickAt` ([SceneEditController.cpp:2322](../../src/Library/SceneEditor/SceneEditController.cpp)) is shared C++ and already wired on desktop, so "which object is under the drag" is resolved in the library | drag *source* (thumbnail); **raw pointer-event capture** (the OS drag-position events) marshaled to image-pixel space; modifier-key read |
| Hover-preview decision + commit-on-release (A3) | the preview/commit policy + the cheap-preview render path (if feasible); the object-under-pointer resolution (`PickAt`, shared) | the highlight-feedback *draw* once the shared pick result says which object is hit |
| Region/ROI: film-loop restriction + auto-clear (A4) | **all of it** — a `SetRenderRegion(Rect*)` + auto-clear-on-production on the controller | modifier+drag to draw the box; the box overlay |
| Asset-library: index format, scan, search, thumbnail render (A5) | **all of it** — `AssetLibrary` model + `ThumbnailRenderer` | the thumbnail grid widget + drag source |
| Progressive disclosure metadata (A6) | per-parameter `Basic`/`Advanced` tag in the existing `ChunkDescriptor`; layout-preset model | show/hide rows; the Simple/Full layout toggle |

The descriptor-driven properties panel is the lever for A6: a row already auto-appears from `IAsciiChunkParser::Describe()` metadata, so adding a `disclosure` field to the descriptor entry means **zero new per-parameter UI code** — both platforms read the same flag.

---

## 5. A1 — Sensible default scene

### Interaction
On launch with no file, the app is not blank: a **"New from Default"** action (and the empty-state placeholder's primary button) loads a shipped studio scene that renders a good image immediately — neutral dielectric/again-metal control object on a ground plane, a studio HDRI environment, `auto_rasterizer` (→ resolves to PT for this diffuse-ish scene), always-on OIDN, and auto-exposure. Zero further clicks to a believable frame. This is the [NN/g "skip onboarding / sensible defaults"](https://www.nngroup.com/videos/onboarding-skip-it-when-possible/) posture: the first win costs no decisions.

### What ships where
- **Shared C++:** a `DefaultScene` resolver that finds the seed `.RISEscene` under the media path, wires the project root (so `FileRasterizerOutput` and the HDRI resolve), and exposes "load default" through the same `IJob` load path a normal file uses. No special-case render path — the default scene is just a scene.
- **Platform:** one menu item + the empty-state button. The Mac empty state today literally renders the text *"Render to see the scene"* ([ViewportView.swift](../../build/XCode/rise/RISE-GUI/App/ViewportView.swift) `draw(_:)`); that becomes *"Open a scene — or start from the default studio"* with a button.

### Where the seed scene + assets live
- A new `scenes/Default/studio.RISEscene` plus a `scenes/Default/assets/` holding the studio HDRI and any referenced textures. This sits beside the existing `scenes/FeatureBased/` taxonomy ([scenes/README.md](../../scenes/README.md)) and is covered by the existing `RISE_MEDIA_PATH` walk-up that the bridges already perform on load ([RenderViewModel.swift](../../build/XCode/rise/RISE-GUI/App/RenderViewModel.swift) `loadScene` walks up to `global.options`).
- The shipped HDRI **must be a permissively-licensed studio HDRI** (e.g. CC0 from Poly Haven). **Flag (open):** license + repo-size budget for a multi-MB HDRI — resolve before shipping (a downscaled 2k studio HDRI keeps the repo lean; the full-res one can be an optional download). This is the same asset pool A5 seeds from (§9), so A1 and A5 share `scenes/Default/assets/`.

### Android tier
**Tier A.** Mobile opens to the same default scene; the "win in zero clicks" story is *more* compelling on a phone where typing a scene is painful.

### Non-goals
No template *gallery* / new-scene wizard (that is creative-power territory, roadmap §12 "node editor on the default path" caution applies — keep the first run a single sensible default, not a chooser).

---

## 6. A2 — The viewport *is* the renderer

### Interaction
The viewport is already always-on and progressive ([INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md) §5, §19.7). A2 makes it feel like a renderer, not a preview toy, by adding the four explicit controls every IPR has ([Arnold ARV](https://help.autodesk.com/cloudhelp/ENU/AR-Maya/files/am-Arnold_for_Maya_User_Guide/rendering/arnold_for_maya_rendering_am_Arnold_RenderView_Window_html)) and a convergence readout:

1. **Pause / Resume** — stop refining and hold the current frame, reclaiming cores (the [Octane "let me pause inactive viewports"](https://render.otoy.com/forum/viewtopic.php?f=28&t=75296) request). RISE's loop already idles on a condvar between passes; pause is "do not wake for idle refinement."
2. **Pause-on-navigation, refine-on-idle** — *already implemented* as the adaptive scaler dropping to coarse scale during motion and walking back to scale=1 on idle. A2 surfaces this as the documented behavior and optionally lets the user pin "stay coarse while orbiting."
3. **Restart-on-edit** — *already implemented* as `KickRender` tripping the cancel flag on every `Apply`. A2 makes the trigger set explicit (transform, property, material/HDRI assign from A3, region change from A4 all funnel through it).
4. **Stop** — abandon refinement entirely (distinct from Pause: Stop resets to scale-coarse on next interaction).

Plus a **convergence indicator + stop condition**: show current pass / sample count and let the user set "refine to N samples then stop" so an idle viewport does not burn cores forever. This reuses the idle-progressive pass counter the rasterizer already reports as `currentPass / idleMaxPasses`.

### Mapping onto existing machinery (no new render algorithm)
| A2 control | Existing primitive |
|---|---|
| Preview resolution while moving | `mPreviewScale` ladder + `IScenePriv::ResizeFilm` round-trip (already wired, [SceneEditController.cpp:3743-3767](../../src/Library/SceneEditor/SceneEditController.cpp)) |
| Restart-on-edit | `CancellableProgressCallback::RequestCancel()` via `KickRender` |
| Pause / Stop | gate the render-loop condvar predicate (`mIdle` / a new `mPaused` atomic) |
| Convergence count / stop-rule | the idle-pass counter; add a `mIdleStopSamples` cap |
| Quality slider | one setter mapping a slider to `liveSamplesPerPass` + the idle cap |

Coordinator note: the viewport is the `Interactive` job of the shared [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md), which owns the single render slot. The preview-scale `ResizeFilm` round-trip above stays valid because it is the *same* render resizing *its own* live film between its own passes ([RENDER_COORDINATOR.md §5.5](RENDER_COORDINATOR.md)); A2's Start/Pause/Restart/Stop map onto the coordinator's admission + suspend/resume rather than re-implementing a second render loop.

### What ships where
- **Shared C++:** the Start/Pause/Restart/Stop state machine, the stop-rule cap, and a `ConvergenceState` query — all in `SceneEditController` (the coordinator's render-slot owner). New C-ABI: `RISE_API_SceneEditController_{Pause,Resume,SetIdleStopSamples,GetConvergence}` appended to the end of [RISE_API.h](../../src/Library/RISE_API.h) (ABI-additive, per the `abi-preserving-api-evolution` skill).
- **Platform:** four toolbar buttons + a "Pass 12 / 64" readout. Both bridges already expose `start`/`stop`; add `pause`/`resume`/convergence accessors mirroring the existing pattern (e.g. the Mac bridge's `isRunning`, `lastSceneTime`).

### Non-goals
No marketing as "real-time"; not chasing D5/Lumion 30 fps DXR ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §12). One progressive viewport, not a fan-out of IPR/Vision/Vantage modes (the V-Ray confusion §12 warns against). Keep full OIDN for finals (the "denoise always on for final" rule) — the fast interactive denoise is never presented as final.

### Android tier
**Tier A** for the progressive view + Pause/Stop; **Tier B** for the convergence-tuning UI (touch-adapted, fewer knobs). Pause matters *more* on mobile (battery/thermal).

---

## 7. A3 — Drag-to-assign material / HDRI (the feasibility-risk item)

### Interaction (target)
KeyShot-faithful ([KeyShot: Assigning Materials](https://manual.keyshot.com/manual/materials/assigning-materials/)):
- Drag a **material** thumbnail from the A5 library onto an object in the viewport. While dragging over an object, that object highlights; **on release**, the material is assigned. Optional hover-preview shows the material on the hovered object *before* release.
- Drag an **HDRI** thumbnail onto the viewport **background** (not an object) → relight: swap the environment map.
- **Modifiers** (mirror KeyShot): **Shift** = add as a multi-material / additional slot rather than replace; **Alt** = retain existing textures; **Ctrl/Cmd** = retain existing labels/overrides. (RISE's exact retain semantics map to keeping bound painter sub-chunks — see the IPainter/IScalarPainter routing in [ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md); a material swap must preserve scalar-slot bindings, not re-route them through JH uplift.)

### The feasibility question (roadmap §13 #1) — addressed head-on
**Is a live spectral hover-preview affordable, or do we fall back to commit-on-release?**

The cost reality: RISE is a *converging spectral path tracer*, not a rasterizer. A hover-preview means, on every pointer-move over a candidate object, (a) apply the material to the in-memory scene, (b) rebuild whatever the material change invalidated, (c) kick a render, (d) cancel it the instant the pointer moves to the next object. The adaptive scaler makes a coarse first frame cheap, but a *material* change is not free the way a camera orbit is:

- A camera move invalidates nothing structural; a material swap can invalidate BSDF/SPF state and (for emissive swaps) light sampling. For spectral rasterizers the new material's colors go through the Jakob–Hanika uplift at bind time.
- Pointer-move-rate hover (tens of Hz) would thrash apply+cancel far harder than the orbit case the scaler was tuned for.

**Decision: ship commit-on-release as the baseline (matches KeyShot's own commit-on-release contract), and treat hover-preview as an opt-in enhancement gated on a measured spike.** Rationale:

1. KeyShot's *assignment* is itself commit-on-release; the hover-*preview* is the only part in question. Commit-on-release alone already delivers the headline interaction.
2. The existing 4-SPP post-release polish path is exactly the "show a decent frame right after the commit" mechanism — so commit-on-release reuses shipped code with zero new render plumbing.
3. Hover-preview, if pursued, should reuse the **coarse single-object** idea: render the hovered object at the coarsest preview scale with a hard per-hover time budget (e.g. one ≤scale-8 pass), debounced so only a hover that *dwells* >~150 ms triggers a preview (not every transit). This bounds cost to roughly one coarse pass per dwell, comparable to a single orbit step. **Such a hover preview is a coordinator job, not a live-film resize:** it must be submitted as a low-priority isolated preview through the [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) (`NodePreview`/`IsolatedAdhoc` class — a snapshot scene + private film), so it never tears the live film or double-books the worker pool while the viewport is refining. Do **not** borrow the auto-router's live-film `ResizeFilm` round-trip for it (that fast path is safe only pre-fan-out inside a single render's `call_once`, [RENDER_COORDINATOR.md §3.4/§5.5](RENDER_COORDINATOR.md)).

**Spike to run before committing to hover-preview (§13 #1):** on the A1 default scene plus one heavy scene (Sponza-class), measure wall-clock for: material apply + invalidate + one scale-8 pass + cancel, for both an RGB and a spectral rasterizer. Acceptance bar: a dwell-preview frame within ~100–150 ms on the default scene. If spectral blows the budget (likely, given JH uplift + BSDF rebuild), ship hover-preview for the **RGB/pixel preview rasterizer only** and use commit-on-release for spectral — the preview rasterizer is already a non-spectral fast path, so a hover preview there is honest about being a preview. **This is the single largest uncertainty in Direction A; do not design downstream features assuming live spectral hover works.**

### What ships where
- **Shared C++:** a new `SceneEditController::ApplyAssetToTarget(assetRef, targetKind, targetName, modifiers)` that routes through `SceneEditor::Apply` (Principle 6 — one mutation path). It resolves an asset-library entry (§9) to the scene-language mutation: material → rebind the object's material chunk; HDRI → **swap** the environment/global radiance map. **⚠ "Round-trippable" is overstated — the assignment is NOT yet round-trippable as a structured edit** ([CURRENT_STATE_AUDIT.md §1, §7](CURRENT_STATE_AUDIT.md); [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7). Two reasons, both **partly TO-BUILD**:
  - **A library material is a *fragment* that usually has to be *created*, not just rebound.** Applying it generally means **creating** the material chunk (and its painter chunks) the object then binds to — and **entity creation persistence is camera-only today** (Phase C; the only creation op is `CloneActiveCamera`, audit §2/§3). Material/painter creation is the **ENTITY_CREATION** work ([ENTITY_CREATION.md](ENTITY_CREATION.md)), and painter-property round-trip in particular is TO-BUILD (Phase B does **not** cover `Painter` — [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) §2.1). So a freshly-assigned library material persists only once material/painter **creation** (and its save re-emit) lands; until then it is a live-viewport edit that does not survive save/reload.
  - **HDRI swap is wholesale-only and not wired anywhere.** The swap entry point `SetGlobalRadianceMap` ([IScenePriv.h:131](../../src/Library/Interfaces/IScenePriv.h)) is **wholesale** (replace the whole map, no structured per-field edit) and is **L-but-not-G** — wired into no GUI on any platform (audit §7; [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md)'s `load_hdri` row marks the env-map swap "today (wholesale) … not yet a structured-save case," `SetGlobalRadianceMap`, L-but-not-G). A3's HDRI path is therefore **net-new GUI wiring over a wholesale library swap**, not a near-wired structured round-trip. **Use the right accessor for a swap:** the swap entry point is `IScenePriv::SetGlobalRadianceMap` ([IScenePriv.h:131](../../src/Library/Interfaces/IScenePriv.h)); `IScenePriv::GetGlobalRadianceMapMutable()` ([IScenePriv.h:259](../../src/Library/Interfaces/IScenePriv.h)) only gives mutable access to **scale** the existing map (it backs `radiance_scale`), not to replace it — and **neither is wired into any GUI today** (audit §7). The hover-preview policy + (optional) cheap-preview render path also live here.
- **Shared C++ (the hit-test too):** the drag-over **hit-test** — which viewport object is under the cursor — is **not** per-platform. `SceneEditController::PickAt` ([SceneEditController.cpp:2322](../../src/Library/SceneEditor/SceneEditController.cpp)) casts the ray, intersects via `IObjectManager::IntersectRay`, resolves the object name, and is already wired on desktop ([CURRENT_STATE_AUDIT.md §6](CURRENT_STATE_AUDIT.md)). A3's drag reuses that shared pick; the controller answers "what is under this pixel."
- **Platform:** the drag *source* (a thumbnail in the A5 grid); **raw pointer-event capture** — the OS drag-position events, normalized to image-pixel space and handed to the shared pick (NSItemProvider/`dropDestination` on macOS; `QDragEnterEvent`/`dropEvent` on Qt); the highlight-feedback *draw* once the shared pick reports the hit object; and reading the modifier-key state. Only raw pointer capture + the thumbnail/highlight widgets are per-platform — the hit-test decision itself is shared.

### Android tier
**Tier B (touch-adapted).** No hover state on touch, so the interaction becomes **tap-to-select-target then tap-thumbnail-to-apply** (or long-press-drag). Commit-on-release maps cleanly; hover-preview is desktop-only. Document gracefully, never a broken control (§10.4).

### Non-goals
No in-viewport material *editing* (that is Direction C / the material editor). A3 is *assignment of an existing library asset*, not authoring.

---

## 8. A4 — Region / ROI render

### Interaction
Modifier+drag a rectangle in the viewport to restrict rendering to that box (the highest sample-efficiency-per-effort IPR feature, [Arnold ARV](https://help.autodesk.com/cloudhelp/ENU/AR-Maya/files/am-Arnold_for_Maya_User_Guide/rendering/arnold_for_maya_rendering_am_Arnold_RenderView_Window_html)). The region shows as an overlay box with a small "REGION" badge. Outside the box keeps its last pixels; inside refines.

### The footgun and its fix
The [Blender region-leaks-into-final behavior](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/regions.html) — *"a render region… will apply not just to the viewport, but also to the final render"* — is the documented #1 footgun. **RISE's rule: a region is a viewport/interactive concept only; any production render (`RequestProductionRender` / Render Animation) auto-clears the region first**, and the UI shows a persistent badge while a region is active so it is never invisible. An explicit "promote region to final" is a *separate deliberate action*, never the default.

### Mapping onto existing machinery (almost free)
`IRasterizer::RasterizeScene` already takes `const Rect* pRect` ([IRasterizer.h:72](../../src/Library/Interfaces/IRasterizer.h)), and `Job::RasterizeRegion` ([Job.cpp:8806](../../src/Library/Job.cpp)) already wraps it (builds the `Rect`, forwards it); today the interactive loop passes `nullptr` ([SceneEditController.cpp:3793](../../src/Library/SceneEditor/SceneEditController.cpp) `RasterizeScene(*scene, /*pRect*/0, …)`). So the region machinery **exists in the library** and every gap is GUI wiring ([CURRENT_STATE_AUDIT.md §5](CURRENT_STATE_AUDIT.md)). A4 is: store an optional region rect on the controller, pass it into the interactive `RasterizeScene`, and **null it before any production dispatch**. The persistent framebuffer ([INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md) §19.8) means pixels outside the region survive between passes — exactly the "refine the box, leave the rest" behavior.

### Integrator caveat — region silently no-ops under MLT
Region render is honored by **all pixel integrators** (PT / BDPT / VCM, pel + spectral) via `PixelBasedRasterizerHelper::BoundsFromRect`, but **MLT ignores `pRect`** — `MLTRasterizer::RasterizeScene` takes the parameter commented out ([MLTRasterizer.cpp:1226](../../src/Library/Rendering/MLTRasterizer.cpp); also `:1340`) because Metropolis-Hastings mutations explore the whole image plane and clipping to a rect would break the chain's ergodicity. A region request under MLT **silently renders the full frame** ([CURRENT_STATE_AUDIT.md §5](CURRENT_STATE_AUDIT.md); [RENDER_COORDINATOR.md §8](RENDER_COORDINATOR.md) "MLT region render"). The A4 UI must therefore either disable the region affordance or show a "region ignored under MLT" note when the active integrator (or `auto_rasterizer`'s resolved pick) is MLT — never let the user believe a box is active when it isn't.

### What ships where
- **Shared C++:** `SceneEditController::SetRenderRegion(const Rect*)` / `ClearRenderRegion()`, the auto-clear-on-production guarantee, and threading the rect into the interactive `RasterizeScene` call (the `Interactive` job the [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) submits — the coordinator forwards `pRect` and inherits the MLT limitation above, [RENDER_COORDINATOR.md §8](RENDER_COORDINATOR.md)). (Coordinate convention note: like picking, the rect is in image-pixel space, and the region must be **rescaled when the preview-scale ladder steps** — divide by the active `mPreviewScale` — so the same screen box maps correctly at every scale level. This is the exact coord-space trap §19.16/§19.19 of the editor plan already fought for pointer events.) New C-ABI appended to [RISE_API.h](../../src/Library/RISE_API.h).
- **Platform:** the modifier+drag gesture to draw the box, the overlay rectangle, the badge.

### Android tier
**Tier B.** Two-finger or long-press-drag to draw the region; same auto-clear guarantee. Lower priority than A1–A3 on mobile.

### Non-goals
No multiple simultaneous regions; no per-region integrator override (that is the auto-router-heatmap territory of Direction D). Region never silently leaks into a final (§12 spirit).

---

## 9. A5 — Thumbnail asset library + search + shipped seed set

### Interaction
A browsable, searchable grid of **material** and **HDRI** thumbnails. Click filters/searches by name/tag; drag a thumbnail into the viewport to trigger A3. Ships with a non-empty seed set so A3 is usable out of the box.

### Index format & where the seed set lives
- **Seed set location:** `scenes/Default/assets/` (shared with A1) for the shipped HDRI(s), and `assets/materials/` for a small curated set of material snippets. Each material is a self-contained `.RISEscene` *fragment* (the chunk text that defines one material) so "apply" is a text-level operation consistent with Principle 1 (text is canonical) and the AI spine (§9.5 of the roadmap — the same fragments an LLM could emit).
- **Index format:** a JSON manifest `assets/library.json` enumerating entries: `{ id, kind: "material"|"hdri", displayName, tags[], sourcePath, thumbnailPath, license }`. JSON because it is diffable, hand-editable, and trivially consumed on all three platforms. The shared `AssetLibrary` model loads/merges manifests from (a) the shipped `assets/`, (b) a user library dir, and (c) the current scene's own directory.
- **Search/filter logic** lives in the shared `AssetLibrary` (name + tag substring match), so all platforms get identical results — no per-platform filtering drift.

### How thumbnails are generated
- A shared **`ThumbnailRenderer`** renders each material onto a standard preview shape (sphere on a neutral ground under a small fixed studio light) and each HDRI onto a reflective/diffuse probe, at a fixed small size (e.g. 128–256 px). It does **not** borrow the live film: each thumbnail is an **isolated render job** submitted to the [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) at the `Thumbnail` priority — a snapshot scene composed onto the preview rig + a private `IFilm` + a capturing sink ([RENDER_COORDINATOR.md §5.2](RENDER_COORDINATOR.md)). This is **not** the `AutoRasterizer` live-film `ResizeFilm` round-trip ([AutoRasterizer.cpp:489-540](../../src/Library/Rendering/AutoRasterizer.cpp)); that fast path is safe only pre-fan-out inside a single render's `call_once` and would tear the live film if reused for a thumbnail while the viewport is live ([RENDER_COORDINATOR.md §5.1](RENDER_COORDINATOR.md)). Thumbnails are cached to `thumbnailPath` and regenerated only when the source fragment's mtime/hash changes (cache invalidation keyed in the manifest).
- Generation honors the "render sequentially, never parallel" rule from the project memory **structurally**: the coordinator holds a single render slot, so thumbnails serialize through it — they queue at priority `Thumbnail` behind any interactive/production render and are preempted by a viewport edit, with duplicate requests coalesced by content key and a bounded queue ([RENDER_COORDINATOR.md §3.3, §5.3-§5.4](RENDER_COORDINATOR.md)). "Tiny is not free": a 128² thumbnail still saturates all cores for its short duration, which is exactly why it must go through the one slot rather than run alongside the viewport. Shipped thumbnails are pre-baked into the repo so first-run shows a populated grid with zero render cost.

### Seed-asset packaging & Save-As behavior
A1's default scene and A5's seed library are **shipped assets**, so they must reach the user through each platform's packaging channel and survive being edited from a read-only install location. This is its own design surface, not an afterthought:

- **How seed assets are bundled per platform.** macOS: inside the `.app` bundle (`Contents/Resources/scenes/Default/…`), reached by the existing `RISE_MEDIA_PATH` walk-up the bridges already perform at load (§5). Windows: under the installer's program-data / resources dir on the same media-path search. Android: as **Gradle/AAPT asset-bundle entries** copied out of the read-only APK to app-private storage on first run (the device cannot render a thumbnail in place and the APK assets are not a writable filesystem — the seed library is pre-baked and copied, never regenerated on-device, matching the Android tier below). The shipped HDRI's size/license is the open §12 flag and directly drives the bundle budget.
- **"Untitled copy" Save-As (never overwrite the seed in place).** Opening a shipped default scene from a read-only install must not let a save write back into the bundle. On first edit-then-save, the default scene saves as an **untitled working copy** in the user's documents area (a Save-As to a writable path), exactly as the seed scene is "just a scene" (§5) but its *source location is read-only*. The save engine's existing Save-As-to-new-path branch handles the mechanics ([CURRENT_STATE_AUDIT.md §1](CURRENT_STATE_AUDIT.md)); the new requirement is the GUI defaulting the target out of the bundle. This is also why the default scene's `FileRasterizerOutput` path must resolve relative to the working copy, not the bundle.
- **Dependency-closure on Save-As / copy-out.** A `.RISEscene` references its HDRI(s) and textures **by path**, not by embedding (§5, §9 manifest `sourcePath`). Copying a scene out of the bundle (or sharing it) must therefore copy its **asset dependency closure** alongside, or rewrite the references to the canonical install location — otherwise the working copy renders with a missing environment. The closure is the set the manifest already enumerates per entry; A5's resolver computes it. (This mirrors the import-sidecar caveat in [ENTITY_CREATION.md §10.2](ENTITY_CREATION.md) — a scene is portable only with its assets.)
- **Namespacing, collision, and manifest precedence.** The `AssetLibrary` merges three manifest sources — shipped `assets/`, a user library dir, and the current scene's directory (§"Index format"). Define the precedence so a user override of a shipped id wins predictably: **scene-local > user > shipped**, keyed on the manifest `id`. Ids should be **namespaced** (e.g. `rise:studio_neutral` for shipped vs an un-prefixed or `user:` id) so a user asset never silently shadows a shipped one by accidental name reuse, and a collision on `id` is resolved by precedence (with a one-line diagnostic surfaced, never a silent pick) rather than load order. The merge + precedence logic is shared C++ in `AssetLibrary` so all three platforms resolve a collision identically.

### What ships where
- **Shared C++:** `AssetLibrary` (manifest load/merge, search), `ThumbnailRenderer` (render + cache + invalidation), and the asset → scene-mutation resolution used by A3. All of it.
- **Platform:** the thumbnail **grid widget** (SwiftUI `LazyVGrid` / Qt `QListView` icon mode), the search box, and the drag source. Widget only.

### Android tier
**Tier A/B.** Browsing + apply is Tier A; live on-device thumbnail *generation* is Tier C (expensive on mobile) — Android ships **pre-baked thumbnails only** and does not regenerate locally. Document as "thumbnails managed on desktop."

### Non-goals
No cloud asset marketplace, no in-app downloads (curated external MCPs cover that later, roadmap §9.3). No baking RISE-specific assets that encode scene knowledge — assets are general (a material is a material), per the "features must be general" project rule.

---

## 10. A6 — Progressive disclosure + Simple/Full layout presets

### Interaction
Per [NN/g](https://www.nngroup.com/articles/progressive-disclosure/), **exactly two levels, never more**:
- Every properties panel shows **Basic** rows by default with a single **"Advanced"** disclosure toggle revealing the rest. Basic = the handful of knobs a typical user touches (e.g. for a material: base color, roughness, metalness; for the rasterizer: samples, the Auto/integrator pick); Advanced = spectral curves, BDPT/VCM depths, SMS knobs, etc.
- Two **workspace layout presets**: **Simple** (viewport + a slim inspector + the render controls; editor and log hidden) and **Full** (today's everything — text editor, log, all accordion sections). **Default to Simple.**

### What ships where (the descriptor lever)
- **Shared C++:** add a `disclosure: Basic|Advanced` field to each parameter entry in `IAsciiChunkParser::Describe()` (the same descriptor that already drives the auto-generated property rows — [chunk parsers are descriptor-driven](../../src/Library/Parsers/README.md)). Because the panel is generated from this metadata, tagging a row is a **one-line descriptor change with no new UI code path** — and the same tags will feed the MCP grammar view (roadmap §2 anti-drift). The layout-preset *model* (which panes are visible in Simple vs Full) is a shared enum + default.
- **Platform:** the "Advanced" expander control and the Simple/Full toggle (a segmented control). Both read the shared flags; neither hard-codes which rows are Basic.

### Constraint
Never gate a *basic result* behind Advanced (the Houdini node-graph trap, §12). The default Simple layout + Basic rows + A1 default scene together mean a new user reaches a good image and the three knobs that matter without ever opening Advanced.

### Android tier
**Tier A.** Mobile is inherently Simple-first; the Advanced expander maps to a "More" disclosure. Full layout (text editor, scopes) is Tier C — "edit on desktop."

### Non-goals
No third level of disclosure; no Maya-style mode-swapped menus or dual property panels for one object — one canonical inspector (§12).

---

## 11. Phase breakdown

Desktop-shared work lands on macOS + Windows simultaneously (Principle 2); Android follows per its tiering. This maps under [GUI_ROADMAP.md](../GUI_ROADMAP.md) §11 Phase 1.

| Sub-phase | Deliverable | Depends on | Risk |
|---|---|---|---|
| **1a** | A1 default scene + A6 Simple/Full presets + Basic/Advanced descriptor tags | descriptor infra (shipped); a shipped HDRI (license TBD) | low |
| **1b** | A2 explicit Start/Pause/Restart/Stop + convergence/stop-rule | cancel-restart + adaptive scaler (shipped) | low (mostly surfacing) |
| **1c** | A4 region/ROI render + auto-clear | `pRect` param (exists); region-scale-vs-preview-ladder coord care | low–medium (coord trap) |
| **1d** | A5 asset library (model + scan + search) + pre-baked seed thumbnails | `ThumbnailRenderer` reuses `AutoRasterizer` resize pattern | medium (thumbnail cache) |
| **1e** | A3 drag-to-assign **commit-on-release** (material + HDRI) | A5 (needs a library to drag from); one-mutation-path via `SceneEditor` | medium |
| **1f (spike-gated)** | A3 hover-preview — **only if §13 #1 spike passes**; RGB-only fallback otherwise | A3 commit-on-release; measured cost | **high — the one real feasibility risk** |

Sequencing note: **A5 before A3** (empty library defeats drag). **A1 shares assets with A5**, so stand up `scenes/Default/assets/` in 1a and populate the manifest in 1d.

---

## 12. Open questions / spikes (consolidated)

1. **(§13 #1) Spectral hover-preview cost — the headline risk.** Measure material-apply + invalidate + one coarse pass + cancel on the default scene and a Sponza-class scene, RGB and spectral. If spectral misses ~100–150 ms, ship hover-preview RGB-preview-only and commit-on-release for spectral. *Do not assume live spectral hover works.* (§7)
2. **Shipped-HDRI license + repo size.** Which CC0 studio HDRI, at what resolution, checked into the repo vs optional download. Blocks A1/A5 shipping. (§5, §9)
3. **Thumbnail cache invalidation key.** mtime vs content-hash of the source fragment; where the user-library cache dir lives per platform. (§9)
4. **Region coord-space under the preview-scale ladder.** Confirm the rect rescales correctly across all six scale levels (the same trap §19.16/§19.19 fought for pointer deltas). (§8)
5. **Material-swap retain semantics.** Exactly which bound sub-chunks Alt/Ctrl preserve, and that a swap never re-routes a scalar slot through JH uplift ([ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md)). (§7)
6. **Convergence stop-rule default.** Should an idle viewport stop at N samples by default (battery/thermal, esp. mobile) or refine indefinitely until paused. (§6)

---

## 13. Non-goals (Direction A, aligned with [GUI_ROADMAP.md](../GUI_ROADMAP.md) §12)

- Marketing the viewport as "real-time" / chasing D5/Lumion 30 fps DXR — RISE is a converging product, one progressive viewport.
- Showing the fast interactive denoise as if it were a final ("denoise always on for finals").
- A node/material editor on the default path (Direction C, gated "Advanced").
- More than two disclosure levels; mode-swapped menus or dual inspectors for one object.
- Region render silently leaking into a final — auto-clear is mandatory.
- A second mutation path for drag-assign edits — everything routes through `SceneEditor` + round-trip + undo/redo (Principle 6).
- Scene-knowledge-encoding "assets" — library entries stay general (a material is a material).
- Cloud asset marketplace / in-app downloads in this phase.

---

## 14. How this honors the roadmap principles

| Principle | How Direction A honors it |
|---|---|
| 1. Text is the source of truth | Material library entries are scene-language fragments; drag-assign writes the same text a hand-editor or the AI would; region/convergence are session state, not scene state (read-only preference). |
| 2. Maximize shared C++ | State machines, asset model, thumbnail renderer, region logic, disclosure tags, **and the drag hit-test (`PickAt`)** all in `src/Library/`; platforms own only widgets + raw pointer capture + present surface (§4 table). |
| 3. Android not left behind | Every item has a tier note; A1/A2/A5/A6 are Tier A/B; A3 touch-adapts; thumbnail *generation* and Full layout degrade gracefully to Tier C. |
| 4. Approachable by default, ≤2 levels | A1 zero-click image; A6 hard two-level cap defaulting to Simple; never gate a basic result behind Advanced. |
| 5. Differentiate on spectral | A3 confronts the spectral-preview cost honestly rather than pretending RISE is an RGB previewer; the seed material set can showcase spectral materials (measured metal, thin-film) that an RGB tool cannot. |
| 6. One mutation path | `ApplyAssetToTarget` and region/convergence all route through `SceneEditController` → `SceneEditor` → undo; no parallel writes. (Durable **round-trip** of an A3 assignment is gated, not shipped — material/painter creation + wholesale HDRI swap are partly TO-BUILD, §7; the *path* is one mutation path today, *persistence* lands with creation.) |

---

## 15. Acceptance criteria (GUI_ROADMAP §15 template, filled in)

Direction-A-wide; each sub-phase (§11) inherits the relevant rows.

- **Tests.**
  - *A1 default scene:* a headless test loads `scenes/Default/studio.RISEscene` through the normal `IJob` load path and asserts it parses, resolves its HDRI under `RISE_MEDIA_PATH`, and renders a non-black frame with `auto_rasterizer` (no special-case render path — invariant: the default scene is "just a scene").
  - *A2 state machine:* unit-test Start/Pause/Resume/Restart/Stop transitions and the idle-stop-at-N-samples cap on `SceneEditController`; assert Pause holds the framebuffer (no further passes) and Restart-on-edit trips the cancel flag. (The render-loop arbitration these map onto is covered by [RENDER_COORDINATOR.md §9](RENDER_COORDINATOR.md) tests.)
  - *A3 assignment + hit-test:* assert `ApplyAssetToTarget` routes through `SceneEditor::Apply` (undoable + dirty-tracked); a material swap preserves scalar-slot bindings and **never** re-routes a scalar slot through JH uplift ([ISCALARPAINTER_REFACTOR.md](../ISCALARPAINTER_REFACTOR.md)); the drag hit-test resolves the object via the shared `PickAt` ([SceneEditController.cpp:2322](../../src/Library/SceneEditor/SceneEditController.cpp)); HDRI assign calls `SetGlobalRadianceMap` (wholesale swap) not the scale accessor. **⚠ Round-trip persistence is NOT yet asserted — it is gated, not shipped** (§7): a library material that requires material/painter **creation** persists only once non-camera creation + painter save re-emit land ([ENTITY_CREATION.md](ENTITY_CREATION.md); painter persistence TO-BUILD, [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) §2.1), and HDRI swap persists only via the **wholesale** path ([CURRENT_STATE_AUDIT.md §7](CURRENT_STATE_AUDIT.md)). Until then the A3 test asserts the *live in-memory* edit (render restarts, undo works), with a round-trip test added when those land. **Spike gate (§12 #1):** the hover-preview frame-time measurement is the go/no-go for the 1f sub-phase — a real number, not a guess.
  - *A4 region:* the rect clips a pixel-integrator render (interior pixels match a full render within MC noise; edge pixels differ) and is **auto-cleared before any production dispatch**; an explicit test asserts a region request under MLT renders **full-frame** (the documented no-op, [MLTRasterizer.cpp:1226](../../src/Library/Rendering/MLTRasterizer.cpp)).
  - *A5 library:* `AssetLibrary` manifest load/merge with the **scene-local > user > shipped** precedence on a colliding `id` (deterministic winner + diagnostic, not load-order); thumbnail cache invalidates on source mtime/hash change; isolated thumbnail jobs never touch the canonical film ([RENDER_COORDINATOR.md §5](RENDER_COORDINATOR.md) isolation test).
  - *A6 disclosure:* a descriptor row tagged `Advanced` is hidden in the Basic view and shown under the single Advanced expander; assert no second per-parameter UI code path (both platforms read the one `disclosure` flag).
  - *Correctness invariant (engine-touching):* A2/A4 add scheduling/region plumbing, not integrator changes — a coordinator-driven full render is byte-identical to a direct `RasterizeScene` of the same scene ([RENDER_COORDINATOR.md §9](RENDER_COORDINATOR.md)); A3 material-swap correctness is the JH-uplift invariant above.
- **Platform parity.** Shared C++: all state machines, asset model, thumbnail renderer, region logic, `PickAt`, disclosure tags. macOS + Windows ship A1–A6 in lock-step (desktop-first for thumbnail/grid widgets). Android: A1 **Tier A**, A2 progressive view + Pause/Stop **Tier A** (convergence-tuning UI Tier B), A3 **Tier B** (tap-to-target then tap-thumbnail; hover-preview desktop-only), A4 **Tier B**, A5 browse/apply **Tier A** but on-device thumbnail *generation* **Tier C** (pre-baked only), A6 **Tier A** (Full layout Tier C). Never a broken control — degrade, don't break (§10.4).
- **Performance budget.** Interactive latency target unchanged (≤33 ms/pass at adaptive scale; A2 does not modify the preview-scale pump). Thumbnail/preview isolated jobs must never delay an Interactive pass by more than one in-flight tile-batch (coordinator cancellation granularity, [RENDER_COORDINATOR.md §9](RENDER_COORDINATOR.md)). **No production-render regression** beyond the L8 ~0.4% bar (integrators byte-untouched). A3 hover-preview, if shipped, holds a hard ≤~150 ms per-dwell budget (§7); over budget → RGB-preview-only or commit-on-release.
- **Memory budget.** A5 thumbnail cache is on-disk (pre-baked + invalidated by hash), not resident; per isolated job a private `IFilm` at 128²/256² (≈ ≤1 MB fp + AOVs) bounded by the coordinator's single-slot scratch-film reuse ([RENDER_COORDINATOR.md §9](RENDER_COORDINATOR.md)). Seed HDRI resolution capped to the §12 repo-size decision (downscaled 2k studio HDRI; full-res optional download). No per-frame allocation added.
- **Accessibility.** A2 controls (Start/Pause/Restart/Stop) + the convergence readout are keyboard-reachable and screen-reader-labeled, with state shown as text/percent, not colour alone. A4's REGION badge carries a text label, not a colour-only cue. A6 Advanced expander and Simple/Full toggle are keyboard-operable; focus order viewport → inspector. No numpad-only path. Drag-to-assign (A3) has a keyboard/menu equivalent ("Assign selected material to selected object") so it is not pointer-only.
- **Packaging.** Seed scene + asset library ship per platform (app-bundle / installer / Android asset-bundle copy-out) per §9 "Seed-asset packaging"; "untitled copy" Save-As keeps a read-only-install default scene from being overwritten in place; asset dependency-closure travels with a copied/shared scene; `id` namespacing + **scene-local > user > shipped** manifest precedence resolves collisions deterministically. **Open (§12 #2):** the shipped HDRI's licence + resolution/repo-size budget blocks A1/A5 shipping.
- **Migration.** No scene-format change (region/convergence are session state, not scene state; the default scene uses existing chunks). All new C-ABI (A2 Pause/Resume/convergence, A3 `ApplyAssetToTarget`, A4 `SetRenderRegion`) is appended to the end of `RISE_API.h` — additive, out-of-tree-safe per the `abi-preserving-api-evolution` skill. Adding the new shared files (`AssetLibrary`, `ThumbnailRenderer`) touches all **five** build projects (CLAUDE.md source-add rule).
- **Rollback.** Each item is independently gateable: A1 behind a "start from default" affordance (off → today's empty state), A2 behind the explicit-controls surfacing (off → today's always-on progressive viewport), A3/A4/A5 behind their feature flags (off → no drag-assign / no region / no library), A6 behind the Simple/Full toggle defaulting to Full when disabled. No saved `.RISEscene` depends on any Direction-A feature, so disabling any of them never breaks a scene.

### Android tier note
Direction A is **majority Tier A/B**: A1 (Tier A), A2 (Tier A core, Tier B tuning), A5 browse/apply (Tier A), A6 (Tier A) are first-class on mobile — "win in zero clicks" and "talk/tap to assign" are *more* compelling on a phone. A3 touch-adapts to tap-to-target (Tier B; hover-preview desktop-only), A4 is Tier B (two-finger/long-press region), and the two genuinely desktop-class pieces — on-device thumbnail *generation* and the Full editor layout — degrade gracefully to Tier C ("managed/edit on desktop"), never a broken control. Because the entire Direction-A core is shared C++ consumed via JNI, Android inherits the behavior for free; only the Compose widgets + touch gestures are Android-specific.
