# RISE GUI — Cameras & Views (Direction B)

**Status:** DESIGN — pending review. No code.
**Owner:** Aravind Krishnaswamy
**Scope:** Design the camera & view *management* layer for the RISE desktop GUIs (macOS + Windows) and an Android tier note. Five deliverables — **B1** Named Views (session/UI state) + promote-to-scene-camera; **B2** axis-ball nav gizmo + axis snaps + Home; **B3** fly-then-stamp; **B4** camera list panel + bind-camera-to-time-range; **B5** right-sized split view (3 GL/wireframe orientation panes + 1 render pane). Built on the shipped multi-camera infrastructure ([../CAMERAS_ROADMAP.md](../CAMERAS_ROADMAP.md) Phase 1.3/1.4). Parent: [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §6, §10.4, §11 (Phase 2), §12, §13.
**Predecessors:** Multi-camera `ICameraManager` (Phase 1.3, shipped), Camera/Film/Output split (Phase 1.4, shipped), Interactive viewport Phases 1–5 ([../INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md)).
**Dependents / soft-blocks:** Round-trip save **is built** — Mode A/B transforms + Phase B property re-emit (camera/light/material/medium) + Phase C created-entity emit, and camera **creation/property** persistence ships and is tested ([../../src/Library/SceneEditor/SaveEngine.cpp](../../src/Library/SceneEditor/SaveEngine.cpp); `tests/SaveEngineTest.cpp:1690`; [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §1). So promote-to-camera (B1), stamp (B3), and the camera-property half of bind-to-time-range (B4) persist **today**. **Two** save soft-blocks genuinely remain (the prior draft listed only the first): **(i)** B4b's `animation`-chunk *extension* (camera cuts) — Phase C is cameras-only (`SaveEngine.cpp:1351`, "V1: only cameras are creatable"), and a non-camera/animation-cut chunk in the managed block is a hard Refused (`SaveEngine.cpp:1373`); and **(ii)** **deleting a *file-authored* camera (B4a)** — Phase C re-emit cannot express *removing* a source-text chunk, and the managed-tombstone mechanism that handles this for other families [ENTITY_CREATION.md](ENTITY_CREATION.md) §5/§7.5 **cannot express a camera tombstone today**: `> remove camera` does not exist (`ParseRemove` covers only `painter|material|geometry|object|light|modifier`, ENTITY_CREATION §5.4). So a file-authored-camera delete **refuses to persist** until a one-line `> remove camera` sub-command is added (wiring the existing `Job::RemoveCamera`); ENTITY_CREATION §5.4 is the authority and correctly says this case must refuse. Camera *creation*, *rename-of-created*, and *property* edits persist now; *delete-of-file-authored* and *cuts* do not yet. The non-camera *creation* + *deletion-tombstone* persistence work is owned by [ENTITY_CREATION.md](ENTITY_CREATION.md); transaction/epoch semantics by [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md). See §3.1, §7.1.

---

## 1. The reframe (load-bearing)

The roadmap's §6 reframe is the spine of this doc: **the high-value feature is camera / named-view MANAGEMENT, not a literal four-up live path-traced quad.** Nobody — not Maya, not Max — renders four final-quality panes; quad view is "3 cheap GL panes + 1 shaded" (§5, citations below). For a *converging spectral path tracer* the cost asymmetry is even starker: a single extra live PT pane competes for the same cores as the one the user is actually looking at. So the design splits cleanly into two cost tiers:

- **Zero-render-cost camera/view features (B1–B4):** named views, axis-ball nav, fly-then-stamp, the camera list. These manipulate camera *math* and *UI state* only — with **one** engine-side render-path addition, B3's render-camera override (§5.5), which only changes *which* camera the existing interactive pass renders through (a transient `ViewportPose`), never adding a pass and never touching `Scene::pActiveCamera` or production render. They are the bulk of the value and ship first.
- **One-expensive-pane split view (B5):** deferred, and explicitly **not** four live renderers — three GL/wireframe orientation panes share the one render pane's framebuffer is *not* attempted; only the perspective pane path-traces.

A second load-bearing principle from the roadmap (project memory: *"prefer read-only over extending the scene language"*): **Named Views are UI/session state, not a new scene chunk.** The single action that touches `.RISEscene` is the explicit **"Promote to scene camera"** — and it reuses the existing camera-construction + round-trip path rather than inventing a `named_view` chunk.

---

## 2. What exists today (grounding)

Read against the actual tree (`src/Library/Cameras/`, `src/Library/SceneEditor/`, `build/XCode/rise/RISE-GUI/`, `build/VS2022/RISE-GUI/`), ignoring `.claude/worktrees/`:

| Capability | Where | Notes |
|---|---|---|
| Many named cameras, one active | `ICameraManager` ([../../src/Library/Interfaces/ICameraManager.h](../../src/Library/Interfaces/ICameraManager.h)) = `IManager<ICamera>`; `IScene::GetActiveCameraName` / `SetActiveCamera` | Phase 1.3. Camera list (B4) is mostly a view over this. |
| Pixel grid is camera-independent | `IFilm` / `Implementation::Film` (Phase 1.4) | Per-pane resolution (B5) and preview-scale already decoupled from camera optics. |
| Camera math: world→screen, ray-through-pixel | `ProjectWorldToScreen_` in [../../src/Library/SceneEditor/SceneEditController.cpp](../../src/Library/SceneEditor/SceneEditController.cpp) (~L280); test hook `ForTest_ProjectWorldToScreen` | **Shared C++.** Axis-ball hit-testing + gizmo math belong here, next to it. |
| Camera mutation surface | `CameraCommon` ([../../src/Library/Cameras/CameraCommon.h](../../src/Library/Cameras/CameraCommon.h)): `SetLocation/SetLookAt/SetUp`, `GetRestLocation`, `SetEulerOrientation`, `SetTargetOrientation`, `SetDimensionsAndPixelAR`, `RegenerateData()` | Setters mutate; caller calls `RegenerateData()` once. This is exactly what a "set as home" / "snap to axis" restore needs. |
| Camera tools (orbit/pan/zoom/roll) | `SceneEditController::Tool` {OrbitCamera=4, PanCamera=5, ZoomCamera=6, RollCamera=8}; `ToolCategory::Camera` | Orbit mutates `target_orientation`; pan translates pos+lookAt; zoom dollies forward. Implies turntable-style orbit today (see §4.3). |
| Gizmo overlay (object transforms) | C++ computes `GizmoHandle[]` (`RefreshGizmoHandles`, `GizmoHandleAt`); platform draws it — Mac [ViewportGizmoOverlay.swift](../../build/XCode/rise/RISE-GUI/App/ViewportGizmoOverlay.swift) (`Canvas`), Windows twin | **This is the exact shared-math / thin-platform-draw pattern B2's axis-ball must follow.** |
| Toolbar: Photoshop category slots | Mac [ViewportToolbar.swift](../../build/XCode/rise/RISE-GUI/App/ViewportToolbar.swift), Windows [ViewportToolbar.cpp](../../build/VS2022/RISE-GUI/ViewportToolbar.cpp) | Select / Camera / ObjectTransform slots with last-used memory + flyout. B2/B3 add affordances, not a new slot model. |
| Clone active camera (in-memory) | `addCameraFromActive(proposedName)` — Mac [RISEViewportBridge.h](../../build/XCode/rise/RISE-GUI/Bridge/RISEViewportBridge.h) L422; Windows [ViewportBridge.cpp](../../build/VS2022/RISE-GUI/ViewportBridge.cpp) L524 → `RISE_API_SceneEditController_AddCameraFromActive` | **Already shared via C-ABI.** Header comment flags: clone lives only in memory; reload drops it (round-trip pending). This is the seed of "promote to scene camera" (B1). |
| Accordion Category model | `RISEViewportCategory` {Camera=1 … Animation=8}; per-category `categoryEntities:` / `setSelection:` | B4's camera list slots into `Category::Camera`; a Named Views list is a *new* UI category, NOT a scene Category (§3.3). |
| Timeline + named animations | `animationTimeStart/End/NumFrames`, `scrubTime*`; `Category::Animation` | B4 "bind camera to time-range" attaches to this. |

### 2.1 Spike resolution — §13 #2 ("axis/split-view ground truth")

The roadmap §13 #2 spike and §2 "parity note" flag a disagreement: a Windows-side audit *guessed* macOS already has axis/split views; a direct read of the macOS code said it does not.

**My read of the tree (to be confirmed by eye before Phase 2 kickoff):** *neither* desktop platform has axis views, a nav gizmo (axis-ball or ViewCube), "set as home," named views, or any split/quad layout today. A find+grep across `build/**` and `src/**` (excluding `worktrees/`) for `namedview|axisball|setAsHome|homeView|quadview|splitview|fourUp|alignCameraToView|lockCameraToView` returned **zero** matches in either platform's sources. The only "gizmo" present is the *object-transform* gizmo (arrows/rings/scale cubes), which is camera-driven but is not a navigation gizmo. **Uncertainty flag:** this is a static-search conclusion, not a runtime inspection; the spike should still confirm interactively. If confirmed, Direction B is greenfield on both platforms — which is the *good* outcome, because it means B1–B5 land symmetrically with no pre-existing divergent implementation to reconcile.

---

## 3. B1 — Named Views

### 3.1 Concept & the cross-tool footgun to avoid

A **Named View** is a saved viewport *pose* + thumbnail you can restore with one click — Rhino's "Named Views" and SketchUp's "Scenes". The critical design decision is borrowed from those two tools, and from their well-documented footgun:

- **Rhino Named Views** save title, projection, camera + target, and lens length; restoring is an *explicit* user action ("Save as" / "Restore"), never silent. Notably Rhino does **not** save display mode in a named view ([docs.mcneel.com/rhino/8 — NamedView](https://docs.mcneel.com/rhino/8/help/en-us/commands/namedview.htm); [View options](https://docs.mcneel.com/rhino/8/help/en-us/options/view.htm)).
- **SketchUp Scenes** save camera location plus optional per-scene toggles (style, shadows, visible tags, section planes) and carry a **thumbnail** ([help.sketchup.com — Creating Scenes](https://help.sketchup.com/en/sketchup/creating-scenes)). **The footgun:** if "Update automatically when changes are made" is left on, navigating/editing silently overwrites the saved scene — SketchUp added scene-update undo/redo in 2026.0 precisely because "making the wrong update to a scene… could require hours of work to get a scene back" ([SketchUp 2026.0 release notes](https://help.sketchup.com/en/release-notes/sketchup-desktop-20260)).

**RISE decision: explicit update only.** A Named View is captured on demand ("Capture View") and re-stamped only via an explicit "Update View" (mirroring Rhino's restore-is-explicit and SketchUp's documented `Update Scene` action). **No auto-update mode.** This sidesteps the entire SketchUp footgun class by construction. Restore is one click; capture/update are separate, deliberate clicks.

**Payload (full optics, but camera-only — never display state):** like Rhino, a RISE Named View stores the camera's framing, not display state. "Camera-only" is the line against the SketchUp footgun (no integrator / tone-curve / visibility — §10 non-goals), **not** an excuse for a thin payload. The review pinned the failure mode of a pose-only payload: a Named View that stores only location/lookAt/up **cannot reproduce framing** — a 35 mm and a 200 mm shot from the same eye point look nothing alike, and a depth-of-field shot restored without its focus/fstop loses the entire look. So the payload carries the **full optics**: projection type, focus distance, f-stop, aperture, and the sensor/focal pair (§3.2). Storing display mode / integrator / tone-curve in a view remains a hard **non-goal** for B1 (§10) — it bloats the payload and re-introduces the "what did this view secretly change?" surprise.

### 3.2 Data shape (shared C++)

A `NamedView` is a value type owned by a new `NamedViewStore` living in the shared library (alongside `SceneEditController`, not in it — see §9). It is **session/UI state**, not part of the Scene graph.

**The payload IS the existing `CameraSnapshot` — do not re-list optics fields by hand.** The third code-backed review (GUI_ROADMAP §13a #7: "camera full-optics via `CameraSnapshot`") found that the earlier ad-hoc field list silently **dropped real camera fields** that the shipped clone path already captures: tilt X/Y, lens shift X/Y, anamorphic squeeze, aperture blades + aperture rotation, exposure / scanning-rate / pixel-rate, ISO, scene-unit-meters, the *two-axis* orthographic `viewportScale[2]` (the hand list collapsed it to a single `orthoExtent`), the fisheye scale, and the diverged `pixelAR`. Re-listing optics by hand is exactly how the payload drifts from the real camera state. The fix: a `NamedView` carries a **tagged per-camera payload — the existing `CameraSnapshot`** (`SceneEdit.h:34`, the trivially-copyable value the `AddCamera` op already uses), whose `type` discriminator (`Pinhole/ThinLens/Fisheye/Orthographic`) selects the live subset. Because capture and stamp both already go through `CameraIntrospection::CaptureCameraSnapshot` (the clone path, [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §2), reusing `CameraSnapshot` makes the Named-View payload **structurally unable to drift** from the camera state — new optics fields land in one struct, consumed by capture, restore, and stamp alike.

```cpp
// src/Library/SceneEditor/NamedViewStore.h  (NEW; shared C++)
//
// The optics are NOT re-declared here — they ARE the existing CameraSnapshot
// (SceneEdit.h:34), the same trivially-copyable value the AddCamera op and the
// CloneActiveCamera path already capture (CameraIntrospection::CaptureCameraSnapshot).
// CameraSnapshot::type (Pinhole/ThinLens/Fisheye/Orthographic) is the tag that
// selects the live optics subset (the unused fields carry zeros, per its header).
// This is the same value shape B3's transient ViewportPose uses (§5.3), so
// capture / stamp / restore all draw from one struct and CANNOT drift from the
// real camera — a new optics field (tilt/shift/anamorphic/aperture blades/exposure/
// scanning/pixel rate/two-axis ortho viewportScale[2]/fisheye scale/pixelAR …)
// added to CameraSnapshot is picked up everywhere with no edit here.
struct NamedView {
    std::string    name;          // user label, unique within the store
    // --- pose + FULL optics, as the tagged per-camera snapshot ---
    CameraSnapshot camera;        // tag = camera.type; carries location/lookat/up/
                                  // target_orientation/orientation AND every optics
                                  // field for the kind (SceneEdit.h:40-98). Restore
                                  // dispatches on camera.type to the matching kind.
    // --- presentation ---
    std::vector<uint8_t> thumbnailPNG;   // small (e.g. 160×90) tonemapped LDR
    uint64_t       createdEpoch;
};
```

Why a snapshot, not a hand list: pose alone fixes *where you stand and where you look*; the snapshot's per-kind optics (`focalLength`/`sensorSize`/`fov`, `fstop`/`focusDistance`/`apertureBlades`/`apertureRotation`, `tiltX/Y`/`shiftX/Y`/`anamorphicSqueeze`, `fisheyeScale`, the two-axis `viewportScale[2]`, `exposure`/`scanningRate`/`pixelRate`/`iso`/`pixelAR`) fix the *angle of view*, the *depth-of-field look*, and the *lens model itself* — and **restore handles each `CameraKind` via the snapshot's `type`-selected variant** (the same per-kind `Add*Camera` factory dispatch the snapshot already drives, `SceneEdit.h:30-36`). Omitting any optics field makes "restore" fail to reproduce what the user saw; carrying the whole snapshot makes that omission impossible — the review's central B1 correction, now drift-proof by construction. (Note `CameraSnapshot` has no `fromONB` flag today; the ONB-basis-mismatch routing in §3.4 keys off `CameraIntrospection`'s clonability check — non-ONB Pinhole/ThinLens/Fisheye/Orthographic, the same gate `CloneActiveCamera` uses, [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §2 — rather than a payload field.)

**Restore = load the payload into the `ViewportPose` — NOT a scene-camera mutation, NOT a transaction.** This is the central correction of the second adversarial review. A Named View is UI/session state (§3.3), so *restoring* one must stay on the same side of the authoritative boundary: restore copies the view's pose + full optics into the owner-private transient `ViewportPose` (the same value shape, §5.3) and re-points the viewport's render path at it (the render-camera override, §5.5). It does **not** call `CameraCommon` setters on any scene camera, does **not** bump the scene revision, and does **not** produce an `EditHistory` entry — it is identical in cost and side-effect profile to flying the free-fly camera to that framing. (Earlier drafts of this doc made restore "a transaction on the active camera"; that contradicted both the "UI/session state" positioning here and the transient `ViewportPose` introduced in the prior pass. It mutated a scene camera on a *bookmark recall* — exactly the SketchUp "what did this view secretly change?" footgun §3.1 sets out to avoid. Corrected: **restore touches only the `ViewportPose`; the single action that mutates a camera is Stamp/Promote, §3.4.**)

Because restore lands the payload in the `ViewportPose` — a value that carries its own `projection`/`CameraKind` (§5.3) — there is **no type-mismatch problem at restore time**: the pose *becomes* a fisheye/ortho/pinhole pose regardless of any scene camera, and the viewport renders through it via the render-camera override (§5.5), which honors the pose's projection directly. A fisheye view restores as a fisheye free-fly view, full stop. The type-mismatch question only arises when the user later **stamps/promotes** that pose onto a *scene camera* of a different kind — and that is resolved in §3.4 / §5.3, where stamp is the camera-mutating transaction. Restore itself is always an exact, non-destructive reproduction of the framing in the transient view.

(The ONB-basis-mismatch detection that *promotion* needs — to route to the build-a-new-camera path rather than degrade an existing camera's basis (§3.4) — keys off the same `CameraIntrospection` clonability gate `CloneActiveCamera` uses (non-ONB only, [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §2), not a payload flag. It is irrelevant to restore, which never touches a scene camera's basis.)

**Thumbnail generation (shared C++, reuses the preview rasterizer):** capture grabs the most recent interactive preview framebuffer (the `IRasterizerOutput` already feeding `setImageBlock:`), tonemaps + downsamples to ~160×90, and stores the PNG bytes. No new render is forced — the preview is already on screen at capture time. This is cheaper than Substance/Blender thumbnail regeneration because RISE already has a live tonemapped frame in hand.

### 3.3 Where it's stored (this is the key call)

| Aspect | Decision | Rationale |
|---|---|---|
| In the scene graph? | **No.** Not a `Scene` member, not a Category in `RISEViewportCategory`. | Keeps the "prefer read-only" instinct; a Named View is a bookmark, not scene data. |
| In the `.RISEscene` file? | **No** (for B1). No `named_view` chunk. | Avoids extending the scene language for transient UI state; avoids round-trip-save coupling for the MVP. |
| Persisted at all? | **Optionally**, as a UI sidecar — `<scene>.RISEscene.views.json` next to the file, or in app prefs keyed by scene path. | Survives app restart without touching the canonical text. Loss of a sidecar never corrupts a scene. |
| Promoted to durable? | **Yes, on explicit "Promote to scene camera"** (§3.4). | The one bridge from view-bookmark → real, diffable, sharable camera. |

The sidecar is **UI-only** and **per-platform-marshaled but shared-format**: the `NamedViewStore` serializes/deserializes the JSON in shared C++; platforms only choose *where* the sidecar lives (Keychain-adjacent prefs vs a dotfile). Uncertainty flag: sidecar-vs-prefs is a platform-storage detail to settle in implementation; it does not affect the shared model.

### 3.4 Promote to scene camera — the one scene-language touch

This is the single B1 action that writes to `.RISEscene`. It is intentionally **not** a new code path — it composes two things that already exist:

1. **In-memory:** call the existing `addCameraFromActive`-style path, but seed the new camera from the *Named View's* pose instead of the live active camera. (Today `addCameraFromActive` clones the active camera; promote needs an "add camera from an explicit pose+optics" sibling — `AddCameraFromPose(name, NamedView)` on the controller, one new C-ABI export, both bridges marshal it. ABI: additive, follows the `abi-preserving-api-evolution` skill.)
2. **Persist:** route through the round-trip save engine ([../ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md)). A promoted camera emits a real `pinhole_camera` / `thinlens_camera` chunk with a `name`. **This persists today:** camera re-emit (Phase B property re-emit) and created-camera emit (Phase C) ship and are tested — `tests/SaveEngineTest.cpp:1690` proves a created camera survives save→reload→resave byte-identically ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §1). There is **no** "won't survive reload" caveat for a promoted camera. (The `SceneEditController.h:640-644` comment still says "round-trip pending" — that is a stale comment that predates Phase C, per the audit; the test is the ground truth.) As a *transaction*, promote is an `AddCamera` commit (the transaction object + commit path are [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4; the `AddCamera` creation op is [ENTITY_CREATION.md](ENTITY_CREATION.md) §4) — undoable and attributed (`origin = InApp`) like any edit. **Promote/Stamp is the ONLY B1 action that mutates a camera** (restore does not — §3.2); it bumps the scene revision by exactly one and publishes one snapshot per [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4.2.

**Scene-language vs UI split for B1:**

| Piece | Layer |
|---|---|
| `NamedView` model, store, serialize, thumbnail capture, restore-into-`ViewportPose` (no scene mutation, §3.2/§5.5) | **Shared C++** |
| `.RISEscene` write on Promote | **Scene language** (existing camera chunk + round-trip re-emit) — no new chunk |
| Named-views sidecar location | **UI-only** (platform prefs/dotfile; shared serialization) |
| The views strip/grid widget + thumbnails + drag-to-restore | **Platform** (SwiftUI list/grid; Qt `QListWidget` icon mode) |

---

## 4. B2 — Axis-ball nav gizmo + axis snaps + Home

### 4.1 Why an axis-ball, not a 26-region ViewCube

Two reference designs:

- **AutoCAD/Fusion ViewCube** — a 3D cube whose 6 faces + 12 edges + 8 corners = **26 clickable regions** snap to standard/iso views ([Autodesk — About the ViewCube](https://help.autodesk.com/view/ACD/2025/ENU/?guid=GUID-E6D3896C-AF39-4F5C-A57C-CACE2A1117F9); the 6+8+12=26 breakdown is stated verbatim at [Autodesk Maya-Basics ViewCube](https://help.autodesk.com/cloudhelp/2023/ENU/Maya-Basics/files/GUID-C1861E55-85FA-47F9-B4D2-71366875E56D.htm)).
- **Blender navigation gizmo** — a compact colored XYZ axis-ball in the viewport corner: **click an axis label to snap the view to it (click again = opposite); drag to orbit** ([docs.blender.org — Navigation Introduction](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/introduction.html)).

**RISE picks the axis-ball.** The UX argument (roadmap §6): fewer, bigger targets beat 26 small ones. Six axis nubs (+X/−X/+Y/−Y/+Z/−Z) are large hit targets at any gizmo size; the cube's 12 edges and 8 corners are sub-targets that are hard to hit on a small overlay and rarely worth a dedicated click (an isometric view is one drag away). This also matches our existing object-gizmo idiom (axis-colored handles: red=X, green=Y, blue=Z — already in `ViewportGizmoOverlay.axisColor`).

### 4.2 Axis snaps + Home (zero render cost)

- **Six axis snaps:** clicking a nub re-poses the **transient `ViewportPose`** (§5.3) to look down that world axis at the current pivot, preserving distance — the same pose math, applied to the owner-private view rather than to a scene camera. A snap is a *navigation* action, so (like free-fly, §5.3) it drives the `ViewportPose` and the viewport renders through it via the render-camera override (§5.5); it does **not** mutate `Scene::pActiveCamera`. Optionally also bind keyboard shortcuts mirroring Blender's Numpad (1=front, 3=right, 7=top, Ctrl+ = opposite, 5=ortho/persp toggle) ([docs.blender.org — Navigation](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/navigation.html); [Projections](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/projections.html)). **Per roadmap §12 + §13, do NOT hard-depend on a numpad** — the nub clicks are the primary affordance; numpad keys are an accelerator, and any keymap should be reachable without a numeric keypad (cf. Blender's own "Emulate Numpad" preference at [Input preferences](https://docs.blender.org/manual/en/latest/editors/preferences/input.html)).
- **Home / "Set as home":** "Home" restores a stored home pose into the `ViewportPose`; "Set as home" captures the current pose as the home slot. This is a degenerate Named View (a single reserved slot) and reuses the §3.2 restore path — i.e. it lands in the `ViewportPose`, not on a scene camera. Zero render cost.

A snap or Home moves the **view** (the `ViewportPose`), not a scene camera, so — exactly like free-fly navigation (§5.3) — it dirties no scene, produces **no** `EditHistory` entry, and bumps no revision. The bridge to a durable camera is still the explicit **stamp** (§5.3); axis snaps and Home are non-destructive view changes.

### 4.3 Turntable vs trackball

Blender exposes both orbit methods: **Turntable** "rotates the view keeping the horizon horizontal" (stable world-up; cannot roll/tumble past the poles — like a record player); **Trackball** "is less restrictive, allowing any orientation" (free tumble + roll). Turntable is Blender's default ([docs.blender.org — Navigation preferences](https://docs.blender.org/manual/en/latest/editors/preferences/navigation.html)).

**RISE today already behaves turntable-ish:** the Orbit tool mutates `target_orientation` (a `Vector2` — two angles around a target), which is structurally a turntable (azimuth/elevation about a pivot, no free roll). Roll is a *separate* tool (`RollCamera`). **Decision: default = turntable** (matches current behavior, matches Blender default, and is the safer "I didn't mean to tilt the horizon" choice for approachability per roadmap principle 4). A trackball mode is a *later* preference, not a B2 requirement; if added, it is shared-C++ orbit math toggled by a UI preference, parallel to the existing target-orientation path.

### 4.4 Hit-testing math — shared, mirroring the object gizmo

The object-transform gizmo is the template: **all hit-testing + screen-layout math lives in C++** (`RefreshGizmoHandles` / `GizmoHandleAt` / `GizmoHandle[]`), and each platform only *draws* the handle array (Mac `Canvas`, Windows `QPainter`). The axis-ball follows this exactly:

- New shared API on the controller, e.g. `RefreshNavGizmo()` → an array of `NavGizmoNub { axis, screenX, screenY, screenRadius, facing }`, plus `NavGizmoNubAt(Point2 px) -> int`. The "facing" bit (is this the +Z or −Z nub, given the current view) is computed in C++ from the camera basis so front/back nubs can be drawn differently (dimmed when pointing away) — same spirit as the gizmo overlay's active-handle styling.
- The nub layout reuses `ProjectWorldToScreen_` so the ball's axis directions track the live camera with no platform math.

**Scene-language vs UI / shared vs platform for B2:**

| Piece | Layer |
|---|---|
| Nub layout, hit-testing, facing computation, axis-snap pose math, Home store | **Shared C++** (next to `ProjectWorldToScreen_` / gizmo math) |
| Axis-ball overlay rendering (6 colored nubs + drag-to-orbit) | **Platform** (SwiftUI `Canvas` / Qt `QPainter`) — extends the existing overlay |
| Numpad/axis keyboard accelerators | **Platform** (key handling), routed to the shared snap math |
| `.RISEscene` | **Nothing.** B2 is camera-only, zero scene-file writes. |

---

## 5. B3 — Fly then stamp

### 5.1 The pattern

Blender's two camera-from-view affordances are the canonical "fly then stamp":

- **Align Active Camera to View** (`View ▸ Align View ▸ Align Active Camera to View`, Ctrl+Alt+Numpad0) — moves/rotates the scene camera to match the current viewport in one shot ([docs.blender.org — Align View](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/align.html)).
- **Lock Camera to View** — a checkbox that makes viewport navigation move the camera *live* while in camera view ([docs.blender.org — Camera View](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/camera_view.html)).

This is a **perfect path-tracer fit** (roadmap §6/B3): cheap navigation is decoupled from expensive rendering. The user flies around with the cheap interactive preview (or, in split view, a GL pane), finds an angle, then *stamps* it onto a real camera — paying the full render cost once, deliberately.

### 5.2 The contradiction this section must resolve

The earlier framing of "fly then stamp" had a hole, surfaced in review: **today, viewport navigation mutates the *active scene camera* directly** — the interactive preview renders through the active camera and the orbit/pan/zoom tools call `CameraCommon` setters on it ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §8; orbit mutates `target_orientation`, §2.3 here). There is **no transient pose** distinct from a scene camera. So "stamp the current view onto a camera" is incoherent as stated: there is nothing transient to promote — the navigation *already* edited a scene camera. You cannot "fly without committing" because flying *is* committing.

The fix is **not** to defer a transient-view mode (the old §10/§11 recommendation) but to **make it the model B3 is built on.**

### 5.3 The transient viewport pose (the model)

> **B3 introduces an explicit `ViewportPose` — a free-fly view that is NOT any scene camera.** It is the same value shape as a Named View's pose+optics (§3.2: location/lookAt/up/targetOrient + `CameraKind`/optics/projection), held as **session/UI state** on the controller, *not* in `ICameraManager` and *not* in the scene. When the user enters free-fly, navigation drives the `ViewportPose`; the interactive preview renders **through the transient pose** instead of through the active scene camera — via the render-camera override specified in §5.5, which does **not** mutate `Scene::pActiveCamera`. No scene camera is touched while flying.

This makes the two affordances precise:

- **Free-fly navigation** mutates only the `ViewportPose` (UI state). Cheap, never dirties the scene, never produces an undo entry. Leaving free-fly (or selecting a scene camera in the camera list) restores rendering through the active scene camera; the transient pose persists as "the last place I flew to" until overwritten.
- **Stamp = promote the transient pose to a scene camera.** "Set Camera to View" applies the `ViewportPose` onto a chosen camera. This is the §3.4 promote path, sourced from the `ViewportPose` instead of a Named View:
  - **Stamp onto the active camera** (overwrite) = a property-edit transaction on the existing camera (the `CameraCommon` setters, as a single committed transaction; see [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4 — `origin = InApp`, one undo unit). Persists today via Phase B camera-property re-emit (§1; `SaveEngineTest.cpp:1418`).
  - **Stamp into a new camera** = an `AddCamera` transaction seeded from the `ViewportPose` — exactly the `AddCameraFromPose(name, pose)` entry point §3.4 already needs (entity creation is owned by [ENTITY_CREATION.md](ENTITY_CREATION.md) §4; epoch/precondition by [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)). Persists today via Phase C created-camera emit (§1).

The payload that the transient pose carries — and therefore what a stamp reproduces — is the **full optics** (projection/focal/sensor/focus/fstop/aperture), not just pose: it is the same `NamedView` payload defined in §3.2. Because the `ViewportPose` is the *same* value shape as a Named View, a stamp and a "capture this as a Named View" (§3) draw from the identical source — one pose concept, two destinations (scene camera vs UI bookmark).

### 5.4 Lock-to-view (live) and the relationship to scene cameras

- **Lock-to-view (live) = a toggle that binds free-fly to a scene camera.** When ON, navigation mutates the bound scene camera *live* (Blender's "Lock Camera to View"), i.e. it abandons the transient pose and edits the camera each move — expressed as one transaction per drag ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4.5, "one drag = one transaction"). When OFF (the default), navigation drives the transient `ViewportPose` and the scene camera is untouched until an explicit stamp. This is the clean separation the old framing lacked: lock-to-view is *opt-in live editing*, free-fly is *non-destructive exploration*, and stamp is the bridge.
- This is strictly more capable than today's "always edits the active camera" behavior, and it subsumes it: today's behavior is "lock-to-view permanently ON." Defaulting lock-to-view **OFF** makes free-fly the safe-by-default mode (roadmap principle 4, approachability — "I didn't mean to move my framed camera").

**Scene-language vs UI for B3:** the transient `ViewportPose` and free-fly navigation are **UI/session state only** — never written to the scene, no chunk. The single scene write is the **stamp**, which is a camera transaction (overwrite = property re-emit; new camera = `AddCamera`/Phase C) — both **persist today** (§1). No new chunk.

### 5.5 Rendering through the `ViewportPose` — the render-camera override (the engine mechanism)

§5.3 and §3.2 both say the viewport "renders **through** the `ViewportPose`," but the renderer today has no such concept — and that gap is the open finding the second adversarial review flagged. This section specifies the mechanism. It is **shared-C++ engine work**, not a platform detail.

**The constraint (grounded in code).** Every pixel rasterizer obtains the camera from the *scene*, not from a parameter: `PixelBasedRasterizerHelper` reads `pScene.GetCamera()` at four sites ([PixelBasedRasterizerHelper.cpp:158, 941, 1449, 1752](../../src/Library/Rendering/PixelBasedRasterizerHelper.cpp)), and `Scene::GetCamera()` returns the single `pActiveCamera` ([Scene.h:101](../../src/Library/Scene.h), [Scene.cpp:34](../../src/Library/Scene.cpp)). So "render through a transient pose" cannot mean "set `pActiveCamera` to the pose" — that would mutate authoritative scene state on every pointer-move (the very thing §5.2 rejects), would be visible to every reader/snapshot ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.1), and would make free-fly indistinguishable from lock-to-view. The override must render through a *different* camera **without** disturbing `Scene::pActiveCamera`.

**The mechanism — a viewport-local render-camera override.** Introduce a transient, viewport-private `ICamera` the interactive render path uses *in place of* the scene's active camera when a `ViewportPose` is active:

- The `ViewportPose` (an owner-private value, §5.3) is **realized into a throwaway `ICamera`** — the same per-kind factory `AddCameraFromPose` (§3.4) uses, but the result is held only by the controller's viewport state, **never** added to `ICameraManager` and **never** assigned to `Scene::pActiveCamera`. It is rebuilt (or its `CameraCommon` setters re-applied + one `RegenerateData()`) as the pose changes during a drag; because it is owner-private and consumed only by the owner's own interactive pass, this is the "drag preview state" class TRANSACTION_MODEL §3.3 already sanctions — no revision bump, no snapshot publish, no other reader can observe it.
- The interactive render pass selects the camera by a **render-camera parameter** rather than reading `pScene.GetCamera()` unconditionally. Two equivalent plumbings, to settle in implementation (uncertainty flag):
  - **(a) A `const ICamera*` override threaded into the interactive rasterize call** — add an optional render-camera argument alongside the existing `const Rect* pRect` on the rasterize entry the interactive path uses (`IRasterizer::RasterizeScene`, [IRasterizer.h:70](../../src/Library/Interfaces/IRasterizer.h)); when non-null the helper uses it, else it falls back to `pScene.GetCamera()`. Additive signature, ABI per the `abi-preserving-api-evolution` skill (a new overload, not a changed one).
  - **(b) A render-time scene *view* that overrides only the camera accessor** — a lightweight wrapper presenting the same `IScene` with `GetCamera()` returning the override, passed to the existing `RasterizeScene`. No rasterizer-signature change; the substitution lives entirely in the controller's interactive path.

  Either way the selection point is the controller's interactive pass (`DoOneRenderPass`, [SceneEditController.cpp](../../src/Library/SceneEditor/SceneEditController.cpp)) — when a `ViewportPose` is active it supplies the override camera; when free-fly is inactive (or lock-to-view is ON, §5.4) it supplies nothing and the pass renders through `Scene::pActiveCamera` exactly as today.
- **Production render is unaffected.** The production path renders the *scene* camera — `prod->RasterizeScene( *scene, /*pRect*/0, /*seq*/0 )` ([SceneEditController.cpp:1692](../../src/Library/SceneEditor/SceneEditController.cpp)) — and never receives an override. A `ViewportPose` is a viewport-navigation convenience; a final render always uses the camera the scene file defines (or the one the user stamped). This is the invariant that keeps free-fly non-destructive: you can fly anywhere and your rendered output still comes from `Scene::pActiveCamera` until you explicitly stamp (§5.3).

**Why this is the right layer.** The override is the rendering-side dual of the editing-side rule: editing routes through one mutation path (`SceneEditor::Apply`), and *reading* a transient view routes through one render-camera selection point, never through `pActiveCamera`. It keeps `Scene::pActiveCamera` as the single authoritative camera (so snapshots, thumbnails, RMSE, and production all agree on "the camera"), while giving the viewport a private lens to look through. Background readers ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §6.2) never see the override because they read published snapshots of the scene, and the override is never in the scene.

**Scope note:** this is **shared-C++ engine work** (a render-camera parameter on the interactive render path + an owner-private realized camera), consumed identically by macOS, Windows, and Android — *not* a per-platform shim. It is the one genuinely new engine-side piece B3 adds beyond the camera-math/UI orchestration of the rest of Direction B; the integrators themselves are byte-untouched (they already take their camera from whatever the helper hands them — only *which* camera changes).

---

## 6. B4 — Camera list panel + bind-camera-to-time-range

### 6.1 Camera list

The infrastructure is shipped: `ICameraManager` holds the named cameras; `categoryEntities:(Camera)` already lists them; `setSelection:(Camera, name)` already activates one (`SetActiveCamera`); `addCameraFromActive` clones. **B4 is mostly presentation:** turn the existing flat Camera accordion section into a proper list with **thumbnails + click-to-activate**.

- **Thumbnails per camera:** reuse the §3.2 thumbnail machinery. A camera thumbnail is rendered once (cheap interactive preview through that camera, tonemapped, cached), regenerated on demand or when the camera is edited. Cache key = **(camera name, `DocumentId`)** — the `(UUID, revision)` document identity from [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.4, **not** the old scene-epoch (GUI_ROADMAP §13a #7: "camera thumbnail key still uses scene-epoch not `(UUID,revision)`"). Keying on the `DocumentId` keeps the rest of the design consistent (every snapshot-derived read is `DocumentId`-stamped, TRANSACTION_MODEL §3.2 invariant E2) and invalidates the thumbnail exactly when a commit advances the revision or a reload mints a fresh UUID.
- **Actions:** activate (exists), clone (exists), rename, delete (`RemoveCamera` exists), "new from current view" (= B3 stamp into a new camera). **Persistence is not uniform across rename and delete** (the prior draft over-claimed "already persists" for both):
  - **Rename** persists for a *created* camera (Phase C re-emits the whole chunk under the new name); a *file-authored* camera rename is **Refused** in V1 (in-memory only) because the name token appears at the declaration and every reference site ([ENTITY_CREATION.md](ENTITY_CREATION.md) §6.3).
  - **Delete** of a *created* camera needs no persistence (it simply drops from the re-emit set). **Delete of a *file-authored* camera** removes a source chunk and **refuses to persist today** — and not merely "until the managed tombstone lands." The tombstone mechanism emits a `> remove {family} <name>` command, but **`> remove camera` does not exist**: `ParseRemove` accepts only `painter | material | geometry | object | light | modifier` ([ENTITY_CREATION.md](ENTITY_CREATION.md) §5.4; `AsciiCommandParser.cpp:534-565`), with **no** camera sub-command (even though `Job::RemoveCamera` exists, `Job.cpp:724`). So a file-authored-camera delete is a **refusal-to-persist** (the in-memory delete stands for the session; the camera returns on reload — surfaced loudly, never silent) until a `> remove camera` sub-command is added — a clean one-line follow-up to `ParseRemove` wiring the existing `Job::RemoveCamera` (ENTITY_CREATION §5.4 lists it among the three missing `> remove {camera,medium,shader}` sub-commands). [ENTITY_CREATION.md](ENTITY_CREATION.md) §5.4 is the authority and correctly says this case **must refuse**; ENTITY_CREATION §5/§7.5 own the general managed-tombstone design, but cameras specifically also need the missing sub-command before the tombstone is even expressible.

### 6.2 Bind camera to time range

This is the one genuinely *new* timeline capability. Today the timeline scrubs scene time and supports named animation paths (`Category::Animation`); cameras and the timeline are independent. B4 adds: **a camera can be bound to a `[t_start, t_end]` range on the existing timeline**, so that scrubbing/rendering through that range uses that camera — i.e. multi-camera *cut editing* (camera A for frames 0–48, camera B for 49–96).

Design questions and the recommended answers:

| Question | Recommendation |
|---|---|
| Is the binding scene data or UI state? | **Scene data** — it changes the rendered output of an animation, so it must be reproducible/diffable (roadmap principle 1). This is the one B4 piece that legitimately *earns* a scene-language touch, per the memory rule's "unless promoted" clause. |
| New chunk or extend existing? | Extend the existing **`animation`** chunk surface (named animation paths already live there — see [../../src/Library/Parsers/](../../src/Library/Parsers/)) with optional camera-cut entries (grammar in §6.2.1), rather than a brand-new top-level chunk. Keeps cuts adjacent to the timeline they index. This is the **one remaining save soft-block** for Direction B (the preamble): camera-cut persistence needs the `animation`-chunk descriptor extension + Phase-C-style re-emit (a non-camera managed-block emit, which `SaveEngine.cpp:1373` refuses today). |
| Runtime model | Shared C++: a `CameraTrack` the animation dispatcher consults per frame to pick the active camera before `RenderFrameOfAnimation`. Falls back to the scene's active camera when no cut covers `t` (§6.2.2). |

#### 6.2.1 Boundary semantics, time units, and overlap validation

Replacing the prior "first-match-wins" hand-wave with a precise, validated model:

- **Time units = the timeline's own units.** A cut binds a camera to a `[t_start, t_end]` expressed in the *same* unit the timeline already uses (`animationTimeStart/End` + `NumFrames`; the scrub surface is `scrubTime*`, §2). Authoring is in **scene time** (the timeline's float seconds-or-ticks), not frame indices — frame index is derived (`frame = round((t − tStart)/(tEnd − tStart) · (NumFrames−1))`) so a cut survives a re-time / change of `NumFrames`. The grammar accepts scene-time floats; a UI may *display* frame numbers.
- **Boundary convention = half-open `[t_start, t_end)`.** A cut covers `t_start ≤ t < t_end`. Half-open is the standard cut convention (it makes back-to-back cuts `[0,48)`, `[48,96)` tile the timeline with **no shared-frame ambiguity** at the seam — frame 48 belongs to the second cut, unambiguously). The final cut's `t_end` is treated as inclusive of the last frame so the timeline's final frame is always covered.
- **Overlap is a validation error, not a silently-resolved race.** Two cuts whose half-open ranges intersect are **rejected at parse/edit time** with a diagnostic naming both cuts and the overlapping interval — there is no "first match wins," because a scene that means two cameras at one instant is an authoring mistake, and resolving it by declaration order makes the rendered output depend on chunk ordering (un-diffable, violates roadmap principle 1). The descriptor-driven parser ([src/Library/Parsers/README.md](../../src/Library/Parsers/README.md)) validates non-overlap in `Finalize`; the timeline UI prevents drawing an overlapping region (drag is clamped to the neighbor's boundary) so the error is structurally hard to author.
- **Gaps are legal and explicit.** An instant `t` covered by **no** cut falls back to the scene's active camera (§6.2.2). A gap is a deliberate "use the default camera here," not an error — only *overlap* is invalid.
- **Ordering for evaluation is by `t_start`,** and because overlap is forbidden, the covering cut for any `t` is **unique** — the dispatcher does a single range lookup, not a first-match scan.

#### 6.2.2 Rename, delete, and interaction with camera animation

- **Rename a bound camera.** A camera referenced by a cut is renamed via the rename op ([ENTITY_CREATION.md](ENTITY_CREATION.md) §6.2). The cut stores the camera **by name** (the `animation` chunk re-emits names), so rename is a **composite transaction** ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4) that rewrites the cut's reference in lockstep with the manager key — never leaving a cut pointing at a dead name. Per ENTITY_CREATION §6.3, renaming a *file-authored* camera that a cut references may be Refused by the save engine in V1 (in-memory only) — surface that, consistent with the rename UI's honesty rule.
- **Delete a bound camera.** Deleting a camera that a cut references is **reference-safe** ([ENTITY_CREATION.md](ENTITY_CREATION.md) §5): the controller's `FindReferencesTo(Camera, name)` must include camera-cut edges, and the delete **blocks with the referrer list** ("camera 'hero' is used by 2 cuts — [Reassign cuts to…] [Cancel]") or, on confirm, **cascade-removes the cuts** in the same composite transaction so undo restores both the camera and its cuts atomically. A cut must never reference a deleted camera. (Today `Scene::RemoveCamera` already auto-promotes the first remaining camera when the *active* one is deleted; cuts are a second referrer class that must be scanned — the manager won't report it, per ENTITY_CREATION §5.1.)
- **Interaction with camera animation (a camera that is itself animated).** Cuts and per-camera animation are **orthogonal and compose**: a cut selects *which* camera is active over `[t_start, t_end)`; that camera's own animated parameters (a moving/animated camera on a named animation path) are then evaluated **at the same scene time `t`**. The dispatcher order per frame is: (1) range-lookup the covering cut → pick the camera (or fall back to active); (2) advance *that* camera's animation to `t`; (3) `RenderFrameOfAnimation`. So a cut to an animated "crane" camera plays the crane move *during its window* and the dispatcher cuts to the next camera at the boundary — a hard cut, not a blend (cross-fades/dolly-blends between cameras are a **non-goal**, §10; a transition is authored as overlapping moves on one camera, not two cameras at one `t`, which the overlap rule forbids anyway).

**Scene-language vs UI for B4:**

| Piece | Layer |
|---|---|
| Camera list contents, activate/clone/rename/delete dispatch, thumbnail render+cache | **Shared C++** (over `ICameraManager` + preview rasterizer) |
| Camera-cut model + dispatcher consultation per animation frame | **Shared C++** |
| Camera-cut **persistence** | **Scene language** — extend the `animation` chunk (one descriptor addition + `Finalize` read), round-trip re-emit |
| The list widget (thumbnails) + timeline cut-region drawing/drag | **Platform** (SwiftUI / Qt) |

---

## 7. B5 — Right-sized split view (3 GL/wireframe panes + 1 render pane)

### 7.1 What it is and is NOT

**IS:** a deterministic single↔quad toggle where the quad layout is **3 cheap orientation panes** (GL/wireframe top/front/side) **+ 1 render pane** (the live progressive path-traced view). **IS NOT:** four live path-traced panes (roadmap §12 non-goal — "cost non-starter").

Reference designs confirm the "cheap panes + one shaded" composition is industry-standard, not a RISE compromise:

- **3ds Max:** default 2×2 of Top/Front/Left/Perspective; **only one viewport is active at a time** ("Only one viewport can be in the active state at a time"); **Alt+W** is the Maximize Viewport Toggle (the deterministic single↔quad switch) ([Autodesk — Configuring Viewports](https://help.autodesk.com/view/3DSMAX/2026/ENU/?guid=GUID-B39C0590-058C-4E59-B03D-AEC52DE830AB); [General Viewport Concepts](https://help.autodesk.com/cloudhelp/2017/ENU/3DSMax/files/GUID-368DCAE3-4118-4539-853F-C25B7D56EF3F.htm)). Crucially, **the orthographic panes default to wireframe** — verbatim: *"Wireframe… is the default setting for non-Perspective viewports"* ([3ds Max — Wireframe Mode](https://help.autodesk.com/cloudhelp/2024/ENU/3DSMax-Reference/files/GUID-E7D5074C-E45F-4CC5-B22F-80F77F63F6E6.htm)). That is the direct citation for "nobody renders four final-quality panes."
- **Maya:** Four View = three orthographic (top/front/side) + one perspective; the deterministic toggle is a **quick tap of the Spacebar** → *"Switch between the current layout and a full screen of the active panel (where the mouse cursor is)"* ([Autodesk — Panels/layouts](https://help.autodesk.com/cloudhelp/2017/ENU/Maya/files/GUID-9794DC0D-3E5B-4170-861B-6AD5C197C388.htm)).

### 7.2 RISE mapping

- **The render pane** is the existing interactive viewport unchanged (active camera, progressive PT, preview-scale, OIDN). Only it consumes the path tracer.
- **The three GL/wireframe panes** are a **new lightweight rasterizer** — not the path tracer. They draw scene geometry as wireframe/flat-shaded GL through three fixed orthographic cameras (top/front/side) at the pivot. RISE already has the camera math (`OrthographicCamera`, `ProjectWorldToScreen_`) and a scene geometry graph; what's new is a cheap GL/line renderer for the panes. Uncertainty flag: RISE has no existing GL wireframe rasterizer — this is the single biggest net-new piece in Direction B, and the reason B5 is **deferred to last** (roadmap §6 "later", §11 Phase 2 tail). An interim cheaper option: render the three ortho panes as very-low-spp / bbox-only previews rather than true GL wireframe, to avoid a new GL pipeline; recommend evaluating that trade in the B5 spike.
- **Deterministic toggle:** a single keystroke/button switches single↔quad, mirroring Alt+W / Spacebar. State is per-window UI state; the layout never changes which camera is *active* for rendering (the render pane stays bound to the active scene camera; the ortho panes are view-only and never become the scene camera unless the user stamps one via B3).

**Scene-language vs UI / shared vs platform for B5:**

| Piece | Layer |
|---|---|
| The three fixed ortho cameras' pose math, scene-geometry walk for wireframe | **Shared C++** |
| The cheap GL/line pane rasterizer | **Shared C++ core + platform GL/present surface** (Metal/DXGI), like the existing present path |
| Quad↔single layout state + toggle | **UI-only** (per-window; SwiftUI split / Qt `QSplitter`) |
| `.RISEscene` | **Nothing.** Layout is never persisted to the scene. |

---

## 8. Phasing

Ordered by value-per-cost; each row is independently shippable. Desktop-shared work lands macOS + Windows together (roadmap principle 2).

| Sub-phase | Deliverable | Net-new shared C++ | Net-new platform | Scene-language touch | Soft-blocks |
|---|---|---|---|---|---|
| **B2** | Axis-ball gizmo + axis snaps + Home | nav-gizmo layout/hit-test/snap math (next to existing gizmo math) | axis-ball overlay draw (extends gizmo overlay) | none | none — ship first (zero render cost, greenfield) |
| **B1a** | Named Views (capture/restore/thumbnail), in-memory + sidecar | `NamedViewStore`, restore-into-`ViewportPose` (§3.2/§5.5; *not* `CameraCommon`), thumbnail capture, JSON serialize | views strip/grid widget | none | restore needs the B3 `ViewportPose` + render-camera override — sequence B3's pose/override foundation alongside or before B1a *restore* (capture/store/thumbnail have no such dep) |
| **B3** | Fly-then-stamp (transient `ViewportPose` + "Set Camera to View") | `ViewportPose` (UI state) + free-fly nav + **render-camera override (§5.5)** + `AddCameraFromPose` + stamp-onto-active (overwrite) | toolbar/menu action + lock-to-view toggle | writes existing camera chunk (stamp only) | **none for stamp persistence** — stamp persists today (Phase B overwrite / Phase C new-camera, [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §1); the render-camera override is additive engine work (§5.5), not a save soft-block |
| **B1b** | Promote to scene camera (durable) | reuse B3 `AddCameraFromPose` | menu action | existing camera chunk re-emit | **none** — created-camera persists today (Phase C, `SaveEngineTest.cpp:1690`) |
| **B4a** | Camera list panel (thumbnails, activate/clone/rename/delete) | thumbnail cache over `ICameraManager`; reference-safe delete ([ENTITY_CREATION.md](ENTITY_CREATION.md) §5) | list widget | rename re-emit (Phase B/C); **delete: needs `> remove camera` sub-command** (ENTITY_CREATION §5.4) | **rename:** none for *created* cameras (Phase C re-emits under the new name); *file-authored* rename is in-memory in V1 (ENTITY_CREATION §6.3). **delete:** *created*-camera delete needs no persistence (just drop it from the re-emit set); **deleting a *file-authored* camera REFUSES to persist** — `> remove camera` does not exist (`ParseRemove` covers only `painter\|material\|geometry\|object\|light\|modifier`, ENTITY_CREATION §5.4), so even with the managed-tombstone block the camera tombstone is not expressible; in-memory only (loud refusal) until the one-line `> remove camera` sub-command is added (ENTITY_CREATION §5.4) |
| **B4b** | Bind camera to time-range (camera cuts) | `CameraTrack` + dispatcher hook + non-overlap validation (§6.2.1) | timeline cut-region UI | **extend `animation` chunk** | **`animation`-chunk re-emit** (one of two remaining save soft-blocks — the other is B4a's file-authored-camera-delete, which needs the missing `> remove camera` sub-command, ENTITY_CREATION §5.4; relax `SaveEngine.cpp:1373` for the cut entry) |
| **B5** | Right-sized split view (3 GL panes + 1 render pane) | fixed ortho cameras + cheap GL/wireframe pane rasterizer | quad layout + GL present per pane | none | **new GL wireframe pipeline** (the big lift) |

---

## 9. Cross-platform architecture (roadmap §10)

Everything that is not a native widget or a GPU present surface lives in `src/Library/` behind the existing C-ABI (`SceneEditController` / `RISE_API_SceneEditController_*` + the two bridges). Direction B adds:

- **Shared C++ (write once):** `NamedViewStore`, the transient **`ViewportPose`** + free-fly navigation, the **render-camera override** (§5.5 — the one engine-side render-path addition: a viewport-private realized `ICamera` + the interactive pass's camera-selection point, leaving `Scene::pActiveCamera` and production render untouched), the nav-gizmo math (layout + hit-test), the axis-snap/Home pose math (targeting the `ViewportPose`), `AddCameraFromPose`, the `CameraTrack` cut model + dispatcher hook, thumbnail capture/cache, and the B5 ortho-camera + wireframe-walk core. All of it consumed identically by macOS, Windows, and Android JNI.
- **Platform (thin shells):** the axis-ball overlay draw, the Named-Views/camera-list widgets + thumbnails, timeline cut-region drawing, the quad layout container + per-pane GL present surface, and keyboard accelerators.
- **C-ABI additions** are all **additive** — new `RISE_API_SceneEditController_*` exports + new bridge methods/structs, mirroring how `addCameraFromActive` / `RISEViewportGizmoHandle` / `RISEViewportPropertyPreset` are already done. Follow the **`abi-preserving-api-evolution`** skill (no virtuals added to frozen interfaces; new functionality is non-virtual controller methods + C exports). Audit the enum-translation getters in **both** bridges if any new `Category`/`PanelMode`-style enum value is introduced for a Named-Views UI section (project memory: the `case N:` fall-through-to-None trap).

This is the §10 payoff in miniature: the camera math, named-views model, and gizmo hit-testing are written once; only the widgets differ.

### 9.1 Android tier note (roadmap §10.4)

| Direction-B feature | Android tier | Interaction adaptation |
|---|---|---|
| **B1 Named Views** | **Tier B** (touch-adapted) | Views as a horizontally-scrolling thumbnail strip; tap to restore, long-press for capture/update/promote menu. Model + thumbnails are shared C++ via JNI → free; only the strip is new. |
| **B2 Axis-ball + snaps + Home** | **Tier B** | Axis-ball as a larger touch overlay (bigger nubs — the "fewer/bigger targets" argument helps touch most of all); drag-to-orbit is a one-finger drag. No numpad dependency (already a constraint). Home as a button. |
| **B3 Fly-then-stamp** | **Tier B** | "Set Camera to View" button after touch-navigating. Natural on mobile. |
| **B4 Camera list** | **Tier B** | Camera list = same thumbnail strip pattern as B1. Camera-cut editing on the timeline is **Tier C-ish** (fiddly on a small timeline) — present cuts read-only on mobile, edit on desktop, never a broken control. |
| **B5 Split view** | **Tier C** (deferred / desktop-first) | Quad view is not attempted on a phone-sized screen; present gracefully as "use desktop for split view." The render pane (single view) is Tier A and unaffected. |

---

## 10. Non-goals (deliberately NOT doing)

- **Four-up live path-traced quad** (roadmap §12). B5 is 3 cheap GL/wireframe panes + 1 render pane; the path tracer drives exactly one pane.
- **A `named_view` scene chunk.** Named Views are UI/session state (sidecar). The only scene-file write is Promote (which emits a *camera*) and the B4b camera-cut extension to the existing `animation` chunk. No new chunk for view bookmarks.
- **Storing display mode / integrator / tone-curve / visibility in a Named View** (the SketchUp "Scene saves everything" surprise). RISE views are **camera-only** (pose + full optics, §3.2), never display state, like Rhino Named Views. Note this is *not* "pose-only" — the payload deliberately carries the full optics (projection/focal/sensor/focus/fstop/aperture) so framing reproduces (§3.1).
- **Auto-update views/scenes.** Explicit capture/update only — sidesteps the SketchUp auto-overwrite footgun by construction.
- **A 26-region ViewCube.** Six big axis nubs; iso views are one drag away.
- **Hard numpad dependency** for axis snaps (roadmap §12). Nub clicks are primary; numpad keys are an optional accelerator.
- **Cross-fades / dolly-blends between cameras at a cut boundary** (B4b). A cut is a hard switch; the overlap rule (§6.2.1) forbids two cameras at one instant. A transition is authored as moves on a *single* animated camera, not as two overlapping cuts.
- **Persisting split-view layout to the scene file.** Layout is per-window UI state.

Note: the transient `ViewportPose` (B3, §5.3) is **no longer a non-goal** — the earlier draft deferred a "separate viewport camera" and met the transient-pose need via Named-View capture. The adversarial review showed that framing was contradictory (navigation already mutates the active scene camera, so there is nothing transient to stamp). The transient `ViewportPose` is now the *foundation* B3 is built on (§5.2–§5.4), rendered through the render-camera override (§5.5), not an optional future mode. Correspondingly, **restoring a Named View is no longer a scene-camera mutation** — it lands in the `ViewportPose` (§3.2); the only camera-mutating B1 action is Stamp/Promote.

---

## 11. Open questions / spikes

1. **§13 #2 confirmation (interactive):** static search says *neither* desktop platform has axis/split/nav-gizmo/named-view code (§2.1). Confirm by eye before kickoff. If confirmed → greenfield, symmetric landing.
2. **B5 GL wireframe pipeline:** does RISE want a true GL/line rasterizer for the ortho panes, or a cheaper bbox/low-spp interim? This is the largest net-new piece and gates B5's cost. (Recommend interim-cheap first.)
3. **B1 sidecar location:** dotfile next to the scene vs app prefs keyed by path. Platform-storage detail; doesn't affect the shared model. Pick per platform conventions.
4. **B3 transient-view mode — RESOLVED (no longer open):** the review settled this. The explicit transient `ViewportPose` ("fly without mutating the scene camera until stamped") is now the model B3 is built on (§5.2–§5.4), the **render-camera override** that actually renders through it is specified (§5.5), and lock-to-view is an opt-in toggle for live camera editing. It is not deferred. Two implementation-level spikes remain (not whether to have the mode): **(a)** the render-camera-override *plumbing* — an additive `const ICamera*` overload on the interactive `RasterizeScene` vs a render-time scene-view wrapper that overrides `GetCamera()` (§5.5 (a)/(b)); pick by which is least invasive to the interactive pass, both are additive/ABI-safe; and **(b)** *interaction tuning* — default-OFF lock-to-view, the free-fly enter/exit affordance.
5. **B4b camera-cut grammar:** confirm the exact extension shape on the `animation` chunk (camera-cut sub-entries vs a sibling `camera_track` block) with the parser owner; keep it descriptor-driven so the highlighter/suggestion engine can't drift. Boundary/overlap/time-unit semantics are now specified (§6.2.1) — the open item is purely the surface syntax, validated for non-overlap in `Finalize`.
6. **Round-trip dependency sequencing — narrowed, but delete and cuts remain:** the camera *creation/property* persistence half is **already built** (§1) — B1b promote, B3 stamp (overwrite *and* new-camera), and B4a **rename of a *created* camera** persist today via Phase B + Phase C ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §1). Two save dependencies are **not** yet satisfied, and the prior draft wrongly folded them into "already persists":
   - **Camera *delete* persistence (B4a)** is **not** "already built," and is blocked on more than the tombstone. Removing a *created* camera needs nothing (drop it from the re-emit set). But **deleting a *file-authored* camera** removes a chunk that lives in the source text, which Phase C re-emit cannot express — and the **managed tombstone** mechanism [ENTITY_CREATION.md](ENTITY_CREATION.md) §5/§7.5 that handles this for other families **cannot express a camera tombstone today**, because `> remove camera` does not exist (`ParseRemove` covers only `painter|material|geometry|object|light|modifier`, ENTITY_CREATION §5.4). So a file-authored-camera delete is a **loud refusal-to-persist** (in-memory only for the session) until a one-line `> remove camera` sub-command is added (wiring the existing `Job::RemoveCamera`, `Job.cpp:724`) — ENTITY_CREATION §5.4 is the authority and correctly requires the refusal. (Created-camera delete and file-authored *rename*-vs-*delete* are different cases — see the §8 B4a row and §6.1.)
   - **B4b camera-cut persistence** still needs the `animation`-chunk extension + a non-camera managed-block re-emit (relaxing `SaveEngine.cpp:1373`; owned by [ENTITY_CREATION.md](ENTITY_CREATION.md) §7). Land B4b's in-memory cut model first; its persistence follows the `animation`-chunk re-emit.

---

## 12. References

**RISE internal:** [../GUI_ROADMAP.md](../GUI_ROADMAP.md) (§6, §10.4, §11, §12, §13) · [../CAMERAS_ROADMAP.md](../CAMERAS_ROADMAP.md) (Phase 1.3/1.4) · [../INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md) · [../ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md) · `abi-preserving-api-evolution` skill (`.claude/skills/`). Key code: `ICameraManager.h`, `CameraCommon.h`, `SceneEditController.{h,cpp}` (`ProjectWorldToScreen_`, gizmo math), `RISEViewportBridge.h` (`addCameraFromActive`, gizmo overlay machinery), `ViewportGizmoOverlay.swift`, `ViewportToolbar.{swift,cpp}`, `ViewportProperties.cpp`.

**Web (named views / saved viewport state):** Rhino Named Views — [NamedView](https://docs.mcneel.com/rhino/8/help/en-us/commands/namedview.htm), [View options](https://docs.mcneel.com/rhino/8/help/en-us/options/view.htm). SketchUp Scenes — [Creating Scenes](https://help.sketchup.com/en/sketchup/creating-scenes), [2026.0 release notes (scene-update undo)](https://help.sketchup.com/en/release-notes/sketchup-desktop-20260).

**Web (nav gizmo / axis snaps / fly-then-stamp):** Blender — [Navigation Introduction (axis-ball)](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/introduction.html), [Navigation (numpad axes)](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/navigation.html), [Projections (Numpad 5)](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/projections.html), [Input prefs (Emulate Numpad)](https://docs.blender.org/manual/en/latest/editors/preferences/input.html), [Navigation prefs (turntable vs trackball)](https://docs.blender.org/manual/en/latest/editors/preferences/navigation.html), [Align View (align camera to view)](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/align.html), [Camera View (lock camera to view)](https://docs.blender.org/manual/en/latest/editors/3dview/navigate/camera_view.html). ViewCube — [Autodesk About the ViewCube](https://help.autodesk.com/view/ACD/2025/ENU/?guid=GUID-E6D3896C-AF39-4F5C-A57C-CACE2A1117F9), [26-region breakdown](https://help.autodesk.com/cloudhelp/2023/ENU/Maya-Basics/files/GUID-C1861E55-85FA-47F9-B4D2-71366875E56D.htm).

**Web (quad view):** Maya — [Panels & layouts / spacebar toggle](https://help.autodesk.com/cloudhelp/2017/ENU/Maya/files/GUID-9794DC0D-3E5B-4170-861B-6AD5C197C388.htm). 3ds Max — [Configuring Viewports / Alt+W](https://help.autodesk.com/view/3DSMAX/2026/ENU/?guid=GUID-B39C0590-058C-4E59-B03D-AEC52DE830AB), [General Viewport Concepts (one active)](https://help.autodesk.com/cloudhelp/2017/ENU/3DSMax/files/GUID-368DCAE3-4118-4539-853F-C25B7D56EF3F.htm), [Wireframe Mode (ortho default = wireframe)](https://help.autodesk.com/cloudhelp/2024/ENU/3DSMax-Reference/files/GUID-E7D5074C-E45F-4CC5-B22F-80F77F63F6E6.htm).

---

## 15. Acceptance criteria ([../GUI_ROADMAP.md](../GUI_ROADMAP.md) §15 template, filled in)

Per-deliverable where it matters; shared where it doesn't. Each sub-phase (§8) is independently shippable and carries the relevant slice.

- **Tests.**
  - *B1 Named View round-trip (model):* a `NamedView` captured then restored reproduces the **`ViewportPose`** exactly — pose **and** full optics (projection/focal/sensor/focus/fstop/aperture, §3.2). Invariant: value-equality of the `ViewportPose` (or the override `ICamera` it realizes, §5.5) before-capture vs after-restore. **Restore touches no scene camera** — assert every scene camera's introspection snapshot is byte-identical across a capture→restore (no `SceneEdit`, no revision bump, no undo entry; restore is not a transaction, §3.2).
  - *B1 restore is projection-faithful with no active-camera coupling (§3.2):* restoring a `Fisheye` view yields a fisheye `ViewportPose` and the viewport renders through it via the render-camera override (§5.5) **regardless of the active scene camera's kind** — there is no restore-time type-mismatch and no partial-apply, because restore never writes a scene camera. Invariant: the override camera realized for the view has the view's `projection`; the active scene camera is unchanged. *(The type-mismatch question moves to **stamp/promote**, below.)*
  - *B1 stamp/promote type-mismatch (§3.4 / §5.3 policy):* stamping a `Fisheye` `ViewportPose` onto a `Pinhole` **scene camera** **never silently partial-applies** — assert either an explicit pose-only-onto-active result (flagged) or a new `AddCameraFromPose` camera of the pose's kind is created (per the chosen branch). Invariant: no stamp path leaves a scene camera with a dropped projection.
  - *B3 transient pose (§5.3 / §5.5):* free-fly navigation (and axis snaps / Home, §4.2) mutates the `ViewportPose` and leaves **every scene camera byte-identical** (no `SceneEdit`, no revision bump, no snapshot publish, no undo entry); the interactive pass renders through the render-camera override (§5.5) while `Scene::pActiveCamera` is untouched; a subsequent stamp produces exactly one transaction. Invariant: scene-camera introspection snapshot unchanged across a fly; the scene **revision** advances by exactly 1 on stamp (per [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §4.2 commit / §11.5 invariant summary).
  - *B3 render-camera override (§5.5):* with a `ViewportPose` active, an interactive render pass renders through the override camera (the rendered framing matches the pose's projection + optics) while `Scene::GetCamera()` / `Scene::pActiveCamera` still returns the unchanged scene camera; a **production** render of the same scene renders through the scene camera, **ignoring** the override. Invariant: `Scene::pActiveCamera` pointer + introspection snapshot identical before/during/after a free-fly; production output unchanged by the presence of a `ViewportPose`.
  - *B1b/B3 promote persistence:* a promoted / stamped-new camera survives **save→reload→resave byte-identically** on the second save — extend the shipped created-camera test (`tests/SaveEngineTest.cpp:1690`). Stamp-onto-active persists via Phase B re-emit (extend the camera-property round-trip test, `SaveEngineTest.cpp:1418`).
  - *B4b camera-cut semantics (§6.2.1):* overlapping cuts are **rejected at parse/edit** with a diagnostic naming both cuts; half-open `[t_start, t_end)` boundary assigns the seam frame to the later cut; a gap falls back to the active camera; the covering cut for any `t` is unique (single range lookup). *Cut + animated camera (§6.2.2):* an animated camera bound to a cut plays its move only within its window and hard-cuts at the boundary.
  - *B4b reference-safe delete (§6.2.2):* deleting a camera referenced by a cut blocks with the referrer list or cascade-removes the cuts in one composite transaction; undo restores camera **and** cuts atomically ([ENTITY_CREATION.md](ENTITY_CREATION.md) §5).
  - *B4a delete persistence (§8 / §6.1):* deleting a *created* camera and saving omits it cleanly (drops from the re-emit set). Deleting a **file-authored** camera then saving returns **Refused** — and stays Refused until a **`> remove camera` sub-command** is added (it does **not** exist today; `ParseRemove` covers only `painter\|material\|geometry\|object\|light\|modifier`, ENTITY_CREATION §5.4). The managed tombstone (ENTITY_CREATION §5/§7.5) is **necessary but not sufficient** for cameras — without the sub-command the camera tombstone is inexpressible. Invariant: no save path silently removes a source-authored camera chunk; the refusal is loud (tells the user to delete the chunk in text) and only flips to persisting after the `> remove camera` follow-up lands.
  - *Correctness invariant (engine discipline):* B1–B4 are camera-math + UI orchestration; integrators are **byte-untouched** — no RMSE/variance change. B5's GL/wireframe panes do not touch the path-traced render pane's output (the render pane is the existing interactive viewport unchanged).
- **Platform parity.** Shared C++: the named-views model, nav-gizmo math, `ViewportPose` + free-fly, `AddCameraFromPose`, the `CameraTrack` cut model + non-overlap validation + dispatcher hook, thumbnail capture, and the B5 ortho-camera/wireframe-walk core (§9). macOS + Windows land each sub-phase together. Android: **B1/B2/B3/B4 list = Tier B** (touch-adapted, §9.1); **camera-cut editing read-only on mobile** (edit on desktop), never a broken control; **B5 split view = Tier C** (desktop-first; the single render pane is Tier A and unaffected).
- **Performance budget.** B1–B4 are **zero render cost** (camera/UI-state math only, §1) — no interactive-frame regression and **zero production-render impact** (cite the L8 ~0.4% bar; integrators byte-identical). Free-fly navigation drives the `ViewportPose` and renders through the existing interactive preview at its existing preview-scale — same per-pass budget as today's active-camera navigation. B5 adds three cheap ortho panes; the budget item is the **new GL/wireframe rasterizer** (the one cost in Direction B), which must not steal cores from the render pane (the path tracer drives exactly one pane — the non-goal below).
- **Memory budget.** Per Named View: the `CameraSnapshot` payload (~a couple hundred bytes, trivially-copyable) + one ~160×90 LDR thumbnail PNG (a few KB). Per-camera list thumbnail: cached, keyed `(camera name, DocumentId)` (the `(UUID, revision)` identity, §6.1), regenerated on edit — cap the thumbnail cache like any swatch cache. The `ViewportPose` is a single struct (one per window). Camera-cut model is O(cuts). Negligible peak-RSS delta.
- **Accessibility.** Axis-snap nubs are clickable affordances with numpad keys as an **optional** accelerator — **no numpad dependency** (§4.2, roadmap §12). Named-views and camera-list rows are full keyboard-reachable; restore/capture/stamp are menu/button actions, not gesture-only. Camera-cut regions on the timeline have a keyboard path for set-start/set-end. No colour-only state (active-camera and cut badges carry icon + text). Free-fly enter/exit and lock-to-view are togglable without a pointer.
- **Packaging.** No new shipped assets. New shared `.h/.cpp` (e.g. `NamedViewStore.{h,cpp}`, nav-gizmo math, `CameraTrack`, the B5 wireframe rasterizer) must each be added to **all five build projects** ([../../CLAUDE.md](../../CLAUDE.md) source-add rule: Filelist, Android cmake, VS2022 `.vcxproj` + `.filters`, Xcode pbxproj). The named-views **sidecar** (`<scene>.RISEscene.views.json` or app prefs) is a per-platform storage location (§3.3) — no installer change.
- **Migration.** **No scene-format change** for B1–B3 (named views are sidecar UI state; promote/stamp emit *existing* camera chunks). The **one** scene-language touch is **B4b's `animation`-chunk extension** for camera cuts (one descriptor addition + `Finalize` read + re-emit, relaxing `SaveEngine.cpp:1373` for the cut entry) — backward-compatible (older scenes parse unchanged; the cut entry is optional). Two **save-engine capability** dependencies (not scene-format changes) are owned by [ENTITY_CREATION.md](ENTITY_CREATION.md): the B4b cut re-emit above, and **B4a file-authored-camera delete**, which needs both the general managed tombstone (§5/§7.5 there) **and** a new **`> remove camera` sub-command** (`ParseRemove` lacks it today — only `painter|material|geometry|object|light|modifier`, ENTITY_CREATION §5.4) before a camera deletion is even expressible — both backward-compatible. ABI: all additive C exports + new bridge methods/structs, per the **`abi-preserving-api-evolution`** skill (`.claude/skills/`) — no virtuals on frozen interfaces, no signature changes (§9). The **render-camera override (§5.5)** is an additive interactive-render parameter (a new overload or a render-time scene view) — no scene-format change, no signature change to existing rasterizer entry points.
- **Rollback.** Each sub-phase is independently feature-flaggable and default-on once its tests pass; with a flag off, the GUI falls back to today's behavior (active-camera navigation, clone-only camera creation) without touching any `.RISEscene`. B4b camera cuts degrade safely: a scene with cuts the build can't yet re-emit stays **Refused** on save (never silently dropped, the existing save-engine philosophy) — disabling cuts cannot corrupt a file. **File-authored-camera delete** degrades the same way: until the `> remove camera` sub-command (and the managed tombstone, §8 B4a / ENTITY_CREATION §5.4) lands, the delete is in-memory only and a save stays **Refused** rather than silently erasing the source chunk — disabling it cannot corrupt a file. Lock-to-view defaults **OFF** (§5.4); turning the whole transient-pose model off reverts B3 to overwrite-active-only (the render-camera override §5.5 is simply not engaged, and the interactive pass reads `Scene::pActiveCamera` as today).

### Android tier note
Per §9.1 and roadmap §10.4: **B1 Named Views, B2 axis-ball, B3 fly-then-stamp, B4 camera list = Tier B** (touch-adapted — thumbnail strips, larger nubs, one-finger orbit, FAB/long-press menus; the model + thumbnails are shared C++ via JNI, so only the widgets are new). **Camera-cut *editing* = Tier C-ish** (present read-only on a small timeline, edit on desktop). **B5 split view = Tier C** (desktop-first; the single render pane is Tier A and unaffected). Nothing here is desktop-gated except the quad layout and cut authoring.

> **Non-goal preserved (load-bearing):** there is **no four-up live path-traced quad** (§10, roadmap §12). B5 is 3 cheap GL/wireframe panes + 1 render pane; the path tracer drives **exactly one** pane.
