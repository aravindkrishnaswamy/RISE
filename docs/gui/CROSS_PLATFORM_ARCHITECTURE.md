# RISE GUI — Cross-Platform Architecture & Code-Sharing Strategy

**Status:** DESIGN. No new code; this is the architectural backbone the other GUI deep-dives defer to on the shared-vs-platform boundary.
**Owner:** Aravind Krishnaswamy
**Scope:** Make `GUI_ROADMAP.md` §10 (and principles §1.2 "maximize shared C++" + §1.3 "Android is not left behind") concrete and authoritative. Defines, with code-confirmed facts: what *must* be shared C++, the small set that is genuinely forced platform-specific, the plan to consolidate the duplicated viewport bridges into one C-ABI core, the secure-credential and GPU-present-surface abstractions, the Android staged-fallback tiering (P3), and where new code should land given the 5-build-project registration cost. The other deep-dives (`MCP_TOOL_SURFACE`, `LLM_AGENT_RUNTIME`, `APPROACHABILITY_FOUNDATION`, `CAMERAS_AND_VIEWS`, `MATERIAL_EDITOR`, `SPECTRAL_DIFFERENTIATORS`, and the foundational [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) / [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) / [ENTITY_CREATION.md](ENTITY_CREATION.md)) each carry an "Android tier + interaction" note that this doc governs.

> **Relationship to the three foundational specs.** This doc owns the *shared-vs-platform boundary* (which code is library vs shell, and the C-ABI seam). It **defers** the layers that ride *inside* shared C++: the document/authority/epoch/transaction model is [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) (the epoch is also the render "generation" token); the single-render-slot arbitration + present/coordination boundary is [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md); add/duplicate/delete + reference-safe deletion + non-camera persistence is [ENTITY_CREATION.md](ENTITY_CREATION.md). All three are shared C++ consumed via the same C-ABI/JNI seam this doc defines, so Android inherits them for free — only their trigger + display are per-platform.

> This doc is the **single source of truth for the boundary**. If another GUI spec disagrees with the shared-vs-platform split here, this doc wins; fix the other spec.

---

## 0. The one-paragraph thesis

RISE is a **CPU spectral path tracer**. That single fact collapses most of the cross-platform problem: there is no per-platform *production-render* code, because there is no GPU path-tracing path to port — pixels are computed in shared C++ and the platform's only job is to *display* them. The desktop GUIs are already ~8.5–9k-LOC apps at near-parity sharing one C++ `SceneEditController` (151 KB `.cpp`, 47 KB `.h`), consumed identically through a flat C-ABI in `RISE_API.h`. Android consumes the *same* controller through the *same* C-ABI over JNI. So the architecture is not "three apps that happen to share a library" — it is **one C++ application core with three thin presentation shells**, and the boundary between them is small, sharp, and enumerable. Everything new — the MCP server, the LLM agent loop and provider adapters, the material-graph model, the named-views model, the spectral-picker math — lands behind that same C-ABI and all three platforms get it for free. The forced-platform set is short: native widgets, the display/EDR present surface, file/credential dialogs, OAuth UI, input-event translation, and **one carve-out** — an *auxiliary* GPU render/compute backend (B5 wireframe panes + D6 scopes), per-API but behind a shared backend-neutral interface so the core stays GPU-API-agnostic (§1.2 #7, §5.4). That carve-out does **not** dent the thesis: the *production path tracer* is still CPU-only and platform-invariant; the GPU touches only auxiliary orientation-wireframe and scope passes.

---

## 1. The rule, stated crisply

> **Everything that is not (a) a native widget, (b) a display/present surface, (c) a file/credential/OAuth dialog, (d) input-event translation, or (e) an auxiliary GPU rendering/compute backend (behind the §5.4 backend-neutral interface) lives in `src/Library/` behind a C-ABI surface. Platform code is a marshaling shell over that ABI — it holds no domain logic.**

The corollary, stated as a test you can apply to any proposed code: **if the logic would give a different *answer* on a different platform, it does not belong in the shell; only logic whose answer is "how this OS draws/inputs/stores, or how this GPU API issues a draw/dispatch" belongs there.** A camera orbit produces the same new camera basis on every platform → shared. "Which NSCursor to show" is a macOS answer → shell. The *wireframe edges of the ortho panes* and the *xy-chromaticity of a vectorscope* are the same on every platform → shared; the *Metal vs D3D vs Vulkan draw call that paints them* is a per-API answer → shell, but only below the §5.4 interface (the shared core never names a GPU API).

### 1.1 What MUST be shared C++ (non-negotiable)

These already are, or must be, in `src/Library/`. None of them may be reimplemented per platform.

| Subsystem | Where it lives / will live | Why it is shared by construction |
|---|---|---|
| Scene model (`Scene`, `IObject`, managers, immutability invariant) | `src/Library/` | The one in-memory truth; mutated only between renders. |
| `SceneEditController` (tool state machine, render thread + cancel-restart, pointer→edit, selection, accordion model, gizmo math) | `src/Library/SceneEditor/SceneEditController.{h,cpp}` | The brain. Already consumed identically by all three platforms. |
| `SceneEditor` / `EditHistory` / `SceneEdit` (mutation + undo/redo + dirtiness) | `src/Library/SceneEditor/` | Invariant chain (`FinalizeTransformations`→`ResetRuntimeData`→`InvalidateSpatialStructure`) is engine-correctness, not UI. |
| Round-trip save (`SaveEngine`, dirty trackers, span indices) | `src/Library/SceneEditor/SaveEngine.cpp` (1,898 lines / ~85 KB) | Byte-exact text rewrite must be identical everywhere. |
| Descriptor reflection / properties model (`CameraIntrospection`, `MaterialIntrospection`, `*Introspection`, `ChunkDescriptor`) | `src/Library/SceneEditor/` + `src/Library/Parsers/` | The properties panel is *generated* from descriptors; per-platform UI only renders rows. |
| Render orchestration (`InteractivePelRasterizer`, `ViewportFrameStore`, `FrameStore`, polling/generation counter) | `src/Library/Rendering/` | CPU pixels; produced once, displayed three ways. |
| Spectral math (JH uplift, `IPainter`/`IScalarPainter`, Sellmeier, blackbody, thin-film) | `src/Library/` | Physics is platform-invariant; spectral pickers are thin UIs over it. |
| **Agent tool surface (MCP protocol) server + tool/resource dispatch** (planned) | new `src/Library/Agent/` (§16: avoid the bare `MCP` dir/type token — `src/DRISE/MCPClientConnection` already means *Master Control Program*) | The single definition of "what an LLM can do to RISE." Written once. |
| **LLM agent loop + provider adapters** (planned) | new `src/Library/Agent/` | Agent state machine, streaming, tool-call translation for Claude/Gemini/OpenAI-compatible. Written once. |
| **Chat transcript model** (planned) | new `src/Library/Agent/` | Turns, tool calls, diffs, approval state — a data model, not a view. |
| Asset-library index (planned), named-views model (planned), material-graph model + serialization (planned) | new `src/Library/` files | All data models with platform-invariant behavior. |

### 1.2 The small set that is forced platform-specific

This list is exhaustive at the architectural level. Anything not here is a smell — re-examine whether it belongs in the library.

1. **Native widgets** — SwiftUI views (macOS), Qt6 widgets (Windows), Jetpack Compose composables (Android). The *layout and styling* of the properties panel, accordion, toolbar, chat bubbles, node canvas.
2. **The display / present surface** — how CPU-produced pixels reach an (HDR-capable) display. See §5. This is *display*, not *render*.
3. **File & directory dialogs** — `NSOpenPanel`/`NSSavePanel`, `QFileDialog`, Android Storage Access Framework (`ACTION_OPEN_DOCUMENT`).
4. **Secure-credential storage** — Keychain / Windows Credential Manager / Android Keystore. See §4.
5. **OAuth / sign-in UI** — the browser-redirect or in-app webview flow for "Sign in with Claude / Gemini." The *token exchange* and refresh logic is shared (it is just HTTPS); only the consent-UI presentation is per-platform.
6. **Input-event translation** — mapping `NSEvent` / `QMouseEvent` / Compose `PointerInputChange` into `(x, y)` in image-pixel space, then calling `OnPointerDown/Move/Up`. The *interpretation* of that delta (orbit vs pan vs translate) is shared in the controller; only the capture + coordinate normalization is per-platform.
7. **Auxiliary GPU rendering / compute backend** — the *per-API GPU programs* behind the **B5 wireframe orientation panes** ([CAMERAS_AND_VIEWS.md](CAMERAS_AND_VIEWS.md) §7: a GL/line rasterizer for the 3 ortho panes — **not** the CPU path tracer) and the **D6 EDR cinematography scopes** ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §13.D6: waveform / vectorscope / false-color that benefit from a GPU compute pass). This is the **one carve-out** to §0's "there is no per-platform *render* code": these are *auxiliary* GPU surfaces (orientation-pane wireframe + scope compute), distinct from the production CPU path tracer, and they ARE per-API (Metal on macOS, D3D/DXGI on Windows, Vulkan/none on Android — GUI_ROADMAP §13a #7: "the cross-platform boundary must add a GPU-render/compute backend for B5/D6"). They are forced platform-specific **only below a shared backend-neutral interface** — see §5.4. The *scene walk, ortho-camera math, wireframe edge extraction, and the scope math (xy chromaticity, IRE, waveform binning)* stay shared C++; only the GPU program that draws lines / runs the compute kernel is per-API.

Everything in this list shares a property: it is an *OS capability surface*, not a decision about RISE's scene or render. **For #7 the litmus is sharper:** the *what to draw / what to compute* (geometry, camera, scope math) is a RISE answer → shared; only *how this GPU API issues the draw/dispatch* is per-platform → shell, and even then behind the §5.4 backend-neutral interface so the shared core never names Metal/D3D/Vulkan.

---

## 2. Current state (code-confirmed, 2026-06)

The audit's central claim is confirmed by direct reading:

- **`RISEViewportBridge` (macOS, Obj-C++) and `ViewportBridge` (Windows, Qt) are structurally identical.** Same method set (`start`/`stop`, `scaleFilmToFit`, `setTool`/`currentTool`, `categoryForTool`, gizmo handle array, `pointerDown/Move/Up`, `cameraSurfaceDimensions`, animation options, scrub triplet, property-scrub bracket, undo/redo, save triplet, panel-mode/header/property snapshot, per-category snapshot+selection, accordion entities, `addCameraFromActive`, `sceneEpoch`). Same enums (`Tool`, `ToolCategory`, `GizmoKind`, `PanelMode`/`Category`) with the **same numeric values**, each documented as "part of the C-API contract." The only differences are the marshaling vocabulary: `NS_ENUM`/`NSArray`/blocks vs `enum class`/`QVector`/signals.
- **Android's `RiseBridge` (JNI) is the same surface again**, prefixed `viewport*`, calling the identical `RISE_API_SceneEditController_*` C-ABI functions. Its method bodies are one-liners that forward to the C-ABI (see `RiseBridge.cpp:1033–1271`). It carries extra plumbing (JNI global-ref mutex, manual RGBA8 framebuffer, `ScopedLocalFrame`) but **zero extra domain logic**.
- **All three bind to the same C-ABI** in `RISE_API.h` (`RISE_API_CreateSceneEditController` + ~70 `RISE_API_SceneEditController_*` entry points, lines ~3332–3720). The enums `SceneEditTool_*` (0–8) and `SceneEditCategory_*` (0–7) are the canonical numeric contract every platform mirrors.
- **There is no MCP / LLM / agent / provider code anywhere yet** (`grep` over `src/Library/` returns nothing). All of §9 in the roadmap is designed-not-built — this doc specifies *where* it lands, not its internals (those are `MCP_TOOL_SURFACE.md` / `LLM_AGENT_RUNTIME.md`).

**Implication.** The three bridges are ~3× duplication of a *marshaling table*, not of logic — the logic is already single-sourced in `SceneEditController`. The consolidation in §3 is therefore low-risk: it removes a hand-maintained duplication of the *binding*, not a fork of behavior.

---

## 3. Bridge consolidation — one C-ABI core, three marshaling shells

### 3.1 The problem being solved
Every time a new controller capability is added, the same method must be hand-written **four times**: once on the C-ABI (`RISE_API.h` + impl), then mirrored in `RISEViewportBridge.mm`, `ViewportBridge.cpp`, and `RiseBridge.cpp`. The three mirrors are mechanical and drift-prone (e.g. an enum value added to `Category` must be re-typed in three `NS_ENUM`/`enum class`/Kotlin copies — the `MEMORY.md` "audit bridge enum-translation getters" note exists precisely because this drift bit before). The C-ABI is *already* the single source of behavior; the bridges are an accident of three languages each needing their own view of it.

### 3.2 The target: the C-ABI **is** the bridge core
There is no need to introduce a *new* shared bridge class — `SceneEditController` + the `RISE_API_SceneEditController_*` C-ABI already are it. The consolidation is to (a) treat that C-ABI as the **frozen, complete** bridge contract, and (b) shrink each platform bridge to a **generated or near-generated marshaling layer** so adding a capability is a *one-place* edit (the C-ABI) plus a regenerated shim.

Two complementary moves:

**(A) A single canonical enum + descriptor header, consumed by all platforms.**
Today `SceneEditTool`/`SceneEditCategory` live in `RISE_API.h` and are *re-declared* in each shell. Instead, make the shells *include-or-generate* from one source:
- C/C++ shells (Windows Qt, Android JNI) `#include` the `RISE_API.h` enums directly and `static_assert` their local `enum class` against them where a typed wrapper is wanted — or drop the local copy entirely and use the C-ABI ints.
- Swift cannot include a C++ header, but *can* import the C enums via the bridging header. The Obj-C `NS_ENUM`s should be `static_assert`-mirrored (an Obj-C++ `.mm` compile-time check: `static_assert((int)RISEViewportToolSelect == SceneEditTool_Select, …)`) so a drift fails the build instead of shipping.
- A small generator (`tools/gen_bridge_enums.py`) reads the canonical enums from `RISE_API.h` and emits the Kotlin `enum class` constants and the Obj-C `static_assert` block, run as a pre-build step. This makes enum drift *structurally impossible*, the same way descriptor-driven parsing made highlighter drift impossible.

**(B) A uniform marshaling pattern, so each method is trivially mechanical.**
The C-ABI already uses only marshaling-friendly types: opaque `SceneEditController*`, scalars, `const char*` in / `char* buf, unsigned bufLen` out, and small fixed out-params. Codify three idioms (already used ad-hoc) so a shim author/generator never invents a new shape:
- **String-out:** `bool f(p, char* buf, unsigned len)` → Swift `String`, Qt `QString`, Kotlin `String` via a fixed-size stack buffer.
- **Indexed-list:** `unsigned countF(p)` + `bool itemF(p, idx, char* buf, len)` → array/`QVector`/`List`.
- **Struct-out:** fill caller-provided fields (gizmo handle's `{kind, axis, x, y, r}`) → one platform value type.

With (A)+(B), a new controller capability is: add the C-ABI function (+ impl in `SceneEditController.cpp`), then add one line per shell following the matching idiom — or regenerate the shims. No behavior is ever written more than once.

### 3.3 What the unified header looks like (illustrative)
The "unified header" is `RISE_API.h`'s SceneEditController block, treated as the contract, plus a generated companion `RISE_API_SceneEdit_Enums.h` (single declaration of the tool/category/gizmo/panel enums) that both the C-ABI and the generator consume. Sketch:

```c
// RISE_API_SceneEdit_Enums.h  (single source; #included by RISE_API.h,
// read by tools/gen_bridge_enums.py to emit Kotlin + Obj-C asserts)
enum SceneEditTool      { SceneEditTool_Select = 0, /* … */ SceneEditTool_RollCamera = 8 };
enum SceneEditCategory  { SceneEditCategory_None = 0, /* … */ SceneEditCategory_Medium = 7 };
enum SceneEditGizmoKind { SceneEditGizmoKind_AxisArrow = 0, /* … */ };
enum SceneEditPanelMode { /* == SceneEditCategory numeric values */ };
```

```c
// RISE_API.h  (frozen marshaling contract — additions go at the END)
bool RISE_API_CreateSceneEditController(IJob*, IRasterizer* preview, SceneEditController** out);
void RISE_API_DestroySceneEditController(SceneEditController*);
bool RISE_API_SceneEditController_Start(SceneEditController*);
/* … pointer events, tools, gizmo, properties, accordion, save … */
// FUTURE (this doc reserves the shape; internals owned by MCP_TOOL_SURFACE / LLM_AGENT_RUNTIME):
//   RISE_API_SceneEditController_GetSceneText(p, char* buf, unsigned len)
//   RISE_API_SceneEditController_ValidateSceneText(p, const char* text, char* diag, unsigned len)
//   RISE_API_SceneEditController_ApplySceneText(p, const char* text)   // wholesale rewrite + reload
```

Each platform binds exactly as today: macOS through the Obj-C++ `.mm` (the only file that may see both Swift-facing Obj-C and C++); Windows through the Qt `ViewportBridge.cpp`; Android through `RiseBridge.cpp` + a `rise_jni.cpp` `JNIEXPORT` per method. The bridges keep their *lifetime/threading* duties (which differ — see §3.4) but lose their hand-mirrored *vocabulary*.

### 3.4 What legitimately stays different across the three bridges
Consolidation does **not** mean byte-identical bridge files. These differences are real and must remain:
- **Threading/lifetime guards.** Android holds a JNI global-ref `std::mutex` because Kotlin may call `setCallback(null)` while worker threads fire callbacks (`RiseBridge.cpp:176–196`). macOS/Windows have no JNI and don't need it.
- **Frame delivery.** macOS/Windows keep a *persistent* `ViewportPreviewSink`; Android *reconstructs* the sink on every start, which is why `suppressFirstFrame` is threaded into `startViewport()` rather than called after (`RiseBridge.cpp:980–1016`). This is an OS-lifecycle difference, not a logic fork.
- **Pixel hand-off.** See §5 — the present surface differs by construction.

These are exactly the four allowed categories from §1.2 (input/threading, present surface). Nothing domain-level differs.

---

## 4. Secure-credential storage abstraction

The LLM integration (roadmap §9) needs to persist API tokens / OAuth refresh tokens. This is the **one genuinely per-platform piece of the LLM stack** — everything else (agent loop, provider adapters, streaming, tool dispatch) is shared C++.

### 4.1 Shared interface
A tiny C++ interface in the library, with the *agent runtime depending only on it*. **Name + home are the GUI_ROADMAP §16 decision:** this doc's `ICredentialStore` is canonical; the LLM-runtime spec's earlier `ISecretStore` name is **unified to `ICredentialStore`** (drop `ISecretStore`), and it lives under `src/Library/Agent/` (the agent subsystem dir), **not** `src/Library/LLM/`. It is reference-counted — `ICredentialStore : public virtual IReference` — matching RISE's `IReference` convention. (Greenfield: `src/Library/Agent/` does not exist in the tree yet — [CURRENT_STATE_AUDIT.md §13](CURRENT_STATE_AUDIT.md) — so this is the decided target shape, not an existing type.)

```cpp
// src/Library/Agent/ICredentialStore.h  (planned; §16-decided home & name)
namespace RISE {
  class ICredentialStore : public virtual IReference {   // reference-counted per §16
  public:
    // service = "anthropic" | "google" | "openai-compatible:<host>" | …
    virtual bool   Store (const char* service, const char* secret) = 0;   // overwrite
    virtual bool   Load  (const char* service, char* out, unsigned len) const = 0; // false if absent
    virtual bool   Erase (const char* service) = 0;
    virtual bool   HasKey(const char* service) const = 0;
  };
}
```

The agent runtime never sees a platform type; it is handed an `ICredentialStore*` at construction (the same dependency-injection shape the controller uses for sinks). A `MemoryCredentialStore` (process-lifetime only, never persisted) is the shared default and the headless/CI/test fallback.

### 4.2 Platform implementations (the only per-platform code)
Each platform provides one concrete implementer, constructed in the shell and passed down through a new C-ABI setter:

| Platform | Backing store | API |
|---|---|---|
| macOS | Keychain Services | `SecItemAdd`/`SecItemCopyMatching`/`SecItemDelete` (generic password items, service = the `service` string). Implementer is Obj-C++ in the bridge layer. |
| Windows | Windows Credential Manager | `CredWriteW`/`CredReadW`/`CredDeleteW` (`CRED_TYPE_GENERIC`). Implementer in the Qt shell. |
| Android | Android Keystore + `EncryptedSharedPreferences` | Jetpack Security `MasterKey` + AES-GCM; the JNI shell hands the decrypted secret across to a C++ `ICredentialStore` impl, or implements `ICredentialStore` natively over an NDK keystore call. |

Wiring: a single C-ABI entry `RISE_API_SetCredentialStore(ICredentialStore*)` (or a setter on the LLM runtime handle). The shell constructs its platform store once at launch and injects it. Because the interface is 4 methods and the secrets are short strings, the marshaling follows the §3.2 string-out idiom.

### 4.3 Non-goals
- No bespoke encryption in the library — defer to the OS keystore; never roll our own.
- Local-endpoint mode (Ollama/LM Studio) typically needs no secret; `HasKey` returns false and the runtime proceeds keyless. Scenes never leave the machine in that mode (the privacy story), so credential storage is a cloud-only concern.

---

## 5. GPU present-surface abstraction

### 5.1 The load-bearing correction: this is a *display* surface, not a render surface
**RISE is a CPU path tracer.** There is no GPU *path-tracing* path to port across platforms — the integrator, FrameStore, OIDN denoise, and tone/EDR math all run in shared C++ and emit pixels in **one canonical format: `RGBA16F_ExtendedLinearSRGB`** (binary16 half-floats, linear sRGB primaries, values may exceed 1.0 for HDR). The platform's only job here is to **blit those bytes onto an (HDR-capable) display surface.** The roadmap's "Metal / DXGI / Vulkan-GLES" should be read as *display/compositing* backends, not render backends. (The **one** place the GPU does real rendering/compute is the *auxiliary* B5-wireframe / D6-scope backend of §5.4 — separate from this present surface and from the production integrator, and likewise behind a shared backend-neutral interface.)

This is confirmed in code — the same `RGBA16F_ExtendedLinearSRGB` `TargetFormat` drives every platform; only the surface that consumes it differs:

| Platform | LDR present | HDR/EDR present (code-confirmed) | Notes |
|---|---|---|---|
| macOS | `NSImage`/`CGImage` blit (`RenderImageView.swift`) | **`CAMetalLayer` at `.rgba16Float`** + `extendedLinear` colorspace (`MetalEDRView.swift`, `MetalEDRRenderer`) | Metal is used only as a textured-quad EDR compositor; the OS tone-maps to the screen's `maximumExtendedDynamicRangeColorComponentValue`. |
| Windows | `QImage`/`QPainter` blit (`ViewportWidget.cpp`) | **Native HWND + DXGI swap chain at `DXGI_FORMAT_R16G16B16A16_FLOAT`**, `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` (scRGB) (`HDRRenderWidget.h/.cpp`, `#if _WIN32`-gated) | "Identical buffer math to the macOS EDR path; the only platform-specific surface is this widget + the swap chain" (its own header comment). |
| Android | CPU blit → `ARGB_8888` `android.graphics.Bitmap` → `asImageBitmap()` → Compose `Image` (`RenderViewModel.kt:521–548`, `ViewportPane.kt`) | **None today** — SDR only. No GLES, no Vulkan, no EGL in the codebase. | See §5.3: this is the open question the roadmap flagged. Today's answer is *neither GLES nor Vulkan*; it's a Compose `Image` over a CPU bitmap. |

### 5.2 The abstraction
Because pixels are already unified, the "present surface abstraction" is deliberately thin — it is the *frame-delivery callback contract*, which already exists:

- The shared `ViewportFrameStore` exposes a generation counter + frame-complete/tile-complete callbacks and a single `RenderToBuffer(dst, stride, rect, TargetFormat, ViewTransform)` that converts the canonical HDR FrameStore into the caller's requested target format (RGBA8 sRGB for LDR, RGBA16F extended-linear for EDR).
- Each platform implements one **present sink**: a callback that receives "region dirty" + calls `RenderToBuffer` into its surface's staging memory, then presents:
  - macOS: `RenderToBuffer(…RGBA16F…)` → Metal texture upload → `CAMetalLayer` present.
  - Windows: `RenderToBuffer(…RGBA16F…)` → D3D11 staging texture → `IDXGISwapChain3::Present`.
  - Android: `RenderToBuffer(…RGBA8…)` → `Bitmap` pixels → Compose recomposition.
- The **poll-vs-push** delivery is already shared: workers bump an atomic generation counter (`FrameStore::EndTile`); the UI thread polls (`pollProductionVFS` / `pollInteractiveVFS`, driven by a display-link / `Choreographer` / Qt timer) and emits only when the counter advanced. This lockless design (introduced "L8 round 9") is the cross-platform frame-pump and must stay in the library.

So the contract every platform binds to is: **(1) receive a dirty-region signal, (2) call the shared `RenderToBuffer` into your surface, (3) present.** No platform contains tone-mapping, EDR-headroom math, or color conversion beyond what the OS compositor does after it gets `RGBA16F_ExtendedLinearSRGB` — those live in `ViewTransform` / `FrameStore` in the library.

### 5.3 Android HDR — the flagged uncertainty, resolved factually
The roadmap asked whether Android's GPU path is GLES or Vulkan. **Code answer: it is neither today — Android presents via a CPU `ARGB_8888` Bitmap into a Compose `Image` (SDR only).** There is no GPU present surface on Android at all; the `>> 8` downconvert in `writeDirtyRegion`/`RenderToBuffer` produces 8-bit sRGB and Compose displays it. Consequences and the forward path:
- **HDR/EDR on Android is a Tier C deferral** (§6) — Compose has no EDR surface equivalent to `CAMetalLayer`/scRGB-DXGI in the versions targeted here. When pursued, the natural fit is a `SurfaceView` backed by Vulkan (Android's first-class modern GPU API; the codebase has no GLES legacy to preserve) presenting the same `RGBA16F` buffer to an HDR `SurfaceControl` — but this is *additive*: it adds one Android present sink, changes **zero** library render code, exactly like the macOS Metal and Windows DXGI sinks were additive. Until then Android stays SDR and loses no other capability.
- Practical guidance for any Android HDR work: implement it as a new present sink behind the existing `RenderToBuffer` contract; do not let HDR concerns leak a single line into `FrameStore`/`ViewTransform`/the integrator.

### 5.4 Auxiliary GPU rendering / compute backend — B5 wireframe + D6 scopes (the §1.2 #7 carve-out)

§5.1–§5.3 cover *display* (blitting CPU pixels to a surface). Two later features need something §5's present surface does **not** provide — an actual GPU **render/compute** pass distinct from the CPU path tracer:

- **B5 wireframe orientation panes** ([CAMERAS_AND_VIEWS.md](CAMERAS_AND_VIEWS.md) §7) — the 3 cheap ortho panes draw scene geometry as **GL/wireframe lines**, a real (if lightweight) GPU rasterization, not a blit of a CPU buffer. CAMERAS_AND_VIEWS §7 flags this as "the single biggest net-new piece in Direction B" precisely because RISE has no GL wireframe rasterizer today.
- **D6 EDR cinematography scopes** ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §13.D6) — waveform / vectorscope / false-color overlays that are naturally a **GPU compute pass** over the framebuffer (per-pixel binning into a histogram/waveform texture).

Both are **TO-BUILD** (no GPU render/compute backend exists in the tree today — RISE is a CPU path tracer, [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §10). The third code-backed review (GUI_ROADMAP §13a #7) requires the boundary to add this category rather than pretend §5's *display* surface covers it.

**The rule that keeps the shared core platform-agnostic: a backend-neutral interface, per-API implementers.** The shared library owns *what to draw / what to compute*; a thin per-API backend owns *how the GPU issues it*. Define one shared C++ interface (sketch; the exact shape is a B5/D6 spike) so the core never names Metal/D3D/Vulkan:

```cpp
// src/Library/Rendering/IAuxGpuBackend.h  (planned; backend-neutral)
// The shared core hands this backend (1) backend-neutral geometry/compute work
// items and (2) a target, and gets back pixels/overlay it can present via the
// existing §5.2 RenderToBuffer/present-sink path. It NEVER exposes a Metal/D3D/
// Vulkan type — the core is compiled against this interface alone.
namespace RISE {
  class IAuxGpuBackend : public virtual IReference {   // ref-counted per RISE convention
  public:
    // B5: rasterize a backend-neutral line/edge list (built by the SHARED scene
    // walk + ortho-camera math) through a given view-projection into a target.
    virtual bool DrawWireframe( const LineSegmentBatch&  edges,      // shared: from scene + ortho camera
                                const Matrix4&            viewProj,  // shared: ProjectWorldToScreen_ basis
                                AuxRenderTarget&          dst ) = 0;
    // D6: run a scope compute pass (waveform/vectorscope/false-color) over the
    // resolved framebuffer; the BINNING/CHROMATICITY MATH is shared, the kernel
    // dispatch is per-API.
    virtual bool RunScopePass( const ScopeRequest&       req,        // shared: which scope + params
                               const FrameBufferView&    src,        // the canonical RGBA16F frame
                               AuxRenderTarget&          dst ) = 0;
    virtual bool IsAvailable() const = 0;   // false ⇒ core falls back (CPU wireframe / SDR scope / "edit on desktop")
  };
}
```

- **Shared C++ (write once):** the scene-geometry walk + ortho-camera math that produce the `LineSegmentBatch` (B5); the scope math — xy chromaticity, IRE legend, waveform binning, false-color thresholds (D6). These give the **same answer** on every platform → library (the §1.2 litmus).
- **Per-API backend (the forced #7 set):** one `IAuxGpuBackend` implementer per platform — Metal (macOS), D3D11/12 (Windows), Vulkan or a CPU fallback (Android). These live in the platform shell / a per-API TU, **selected behind the interface**, so the shared core compiles without a single GPU-API symbol. This mirrors §5.2's present-sink pattern (one sink per platform behind `RenderToBuffer`) — the auxiliary backend is its render/compute-side dual.
- **Fallback is first-class:** `IsAvailable()==false` (e.g. Android, or a headless/CI build) routes B5 to a cheaper CPU wireframe/bbox preview (CAMERAS_AND_VIEWS §7's "interim cheaper option") and D6 to an SDR/"edit on desktop" path (§6.3 Tier C) — never a broken control. The interface makes the absence of a backend a clean degrade, not a build break.

**Why this is the right layer.** It is the same discipline as §5's present surface: keep the platform-invariant decision (geometry, camera, scope math) in the library, push only the OS/GPU-API issuance into the shell — and put a backend-neutral interface between them so "RISE is one C++ core" stays true even with two new GPU surfaces. The production CPU path tracer is **untouched**; B5/D6 are *auxiliary* GPU passes that never enter the integrator path.

---

## 6. Android staged fallback (P3) — same core, different shell, graceful tiering

Android consumes the identical C++ core via JNI (§2), so new *capability* arrives largely for free; what differs is the **mobile-first shell** and a deliberate **feature tiering**. The governing rule for every deep-dive: **never ship a broken or half-working control on Android — either adapt it for touch (Tier B) or present it as "edit on desktop" gracefully (Tier C). Degrading is allowed; breaking is not.**

### 6.1 Tier A — must-have (ships in lock-step with desktop, includes full LLM)
Mobile gets these as first-class, not reduced:
- Scene browse / open (SAF), render view (SDR today — §5.3), Render / Cancel, image Save-As, auto-router resolved-integrator surfacing.
- **Real `.RISEscene` scene-save (being wired).** GUI_ROADMAP §16 commits Android scene-save to **Tier A**: wire `nativeSaveAs` to a real `.RISEscene` round-trip through the shared `SaveEngine`. **Today `nativeSaveAs` saves only the rendered *image*** (TGA/PPM/EXR via the FrameStore encoder), **not the scene** — there is no scene round-trip on Android yet ([CURRENT_STATE_AUDIT.md §5/§13](CURRENT_STATE_AUDIT.md), cross-platform matrix). When it lands it inherits the [TRANSACTION_MODEL.md §9/§11.6](TRANSACTION_MODEL.md) external-file reconcile prompt unchanged (the prompt is a controller decision; the shell just renders the choice), and entity creation/deletion can then persist on Android ([ENTITY_CREATION.md §11](ENTITY_CREATION.md)).
- Basic parameter edit via the descriptor-driven accordion (already shipped in `ViewportPane.kt` — Cameras/Rasterizer/Objects/Lights/Media/Output sections, scrub handles, preset dropdowns).
- Timeline scrub (already shipped, gated on `hasAnimatedObjects`).
- **Full LLM chat + agent operation.** Chat is *more* natural on mobile than desktop — "talk to your renderer from your phone" is a legitimate Android headline. Because the agent tool surface (MCP protocol), agent loop, provider adapters, and transcript model are all shared C++ in `src/Library/Agent/` (§1.1, §16), Android needs only the chat *shell* (a Compose message list + composer) and the per-platform credential store (§4) and OAuth consent UI. **This is the strongest argument for P2:** the marquee feature reaches Android at the cost of a chat-bubble UI, not a reimplementation.

### 6.2 Tier B — touch-adapted (works on mobile with a touch interaction model)
Present, but re-thought for fingers, not deferred:
- **Gizmo transforms** — the controller already computes gizmo handles in image-pixel space and hit-tests them (`viewportGizmoHandleAt`); Compose draws them and routes drags (`GizmoOverlay` in `ViewportPane.kt`). Touch adaptation = larger hit radii, one-finger drag = active handle, two-finger = camera. The *math* is shared; only handle sizing/gesture mapping is Android.
- **Named views + camera list** — thumbnail grid; tap to restore; "promote to scene camera" button.
- **Material *instance* sliders** (Material editor C1) — the flat slider/swatch/preset view maps cleanly to a Compose list; this is Tier B.

### 6.3 Tier C — deferred / desktop-first (degrade gracefully, never break)
Surface as a read-only summary + "Open on desktop to edit," not a broken control:
- **Node-graph material editor** (C2) — a pannable node canvas is poor on a phone; show the resolved instance parameters (Tier B view) and a notice.
- **EDR cinematography scopes** (D6) — waveform/vectorscope/false-color need an EDR display Android lacks today (§5.3); show SDR previews where meaningful, defer the EDR scopes.
- **Split / quad view** (B5) — desktop-first; mobile shows the single progressive viewport.

### 6.4 Mobile-first layout + touch interaction model
- **Layout:** single dominant viewport; panels are bottom-sheets / collapsible accordions (already the `ViewportPane` pattern), not always-on side docks. Properties panel is pinned to the input-hand side in landscape.
- **Touch:** tap = pick (Select tool's fused down+up, `ViewportPane.kt:852–862`); one-finger drag = active tool; pinch = zoom; two-finger drag = pan; long-press on a toolbar slot = sub-tool flyout (Photoshop-style, already implemented). No hover, no right-click, no numpad — any feature depending on those is Tier B/C by definition.
- **Chat-first entry point:** because chat is Tier A and mobile-native, the Android app may surface the LLM composer prominently (e.g. a persistent "Ask" affordance), turning the phone into a remote for a scene.

### 6.5 How "same core, different shell" is enforced
- **No domain logic in Kotlin/JNI.** `RiseBridge.cpp` method bodies are one-line forwards to the C-ABI (verified). A reviewer rejects any Android-only computation that would differ from desktop.
- **Enum parity is generated** (§3.2) so Kotlin constants can't drift from `SceneEditTool_*`/`SceneEditCategory_*`.
- **Every deep-dive spec carries an "Android tier + interaction" note** (roadmap §10.4 mandate) classifying each feature A/B/C and naming the touch adaptation — this doc is what those notes conform to.

---

## 7. Build-system implications — where new code should land

RISE has a hard rule (CLAUDE.md / AGENTS.md "Change Checklist"): **adding or removing any `.cpp`/`.h` under `src/Library/` requires touching all five build projects** —
1. `build/make/rise/Filelist` (Unix/Linux, canonical),
2. `build/cmake/rise-android/rise_sources.cmake` (Android NDK),
3. `build/VS2022/Library/Library.vcxproj` (+ `.filters`),
4. `build/VS2022/Library/Library.vcxproj.filters`,
5. `build/XCode/rise/rise.xcodeproj/project.pbxproj` (four sections × usually two targets).

Platform-UI files, by contrast, live in **one** project each (the SwiftUI files in the Xcode GUI target, Qt files in `RISE-GUI.vcxproj`, Compose/Kotlin in Gradle) and cost nothing in the other four.

This produces a clear economic rule, which *reinforces* P2 rather than fighting it:

> **Per-file, library code is more expensive to *add* (5-project registration) but free to keep in sync (write-once, all platforms consume). Platform-UI code is cheap to add (1 project) but expensive to keep in sync (write N times, drift forever).**

Guidance:
- **Default to the library.** Pay the one-time 5-project registration so the logic is written once and can never drift. This is correct for the MCP server, agent runtime, provider adapters, credential *interface*, material-graph model, named-views model, asset index, spectral-picker math.
- **Amortize the registration cost: prefer fewer, larger files.** `SceneEditController.cpp` is 151 KB precisely because consolidating related logic into one already-registered TU avoids repeated 5-project edits. New shared subsystems should start as a small number of files (e.g. `src/Library/Agent/AgentRuntime.{h,cpp}`, `ProviderAdapters.{h,cpp}`, `ToolServer.{h,cpp}` — all under `src/Library/Agent/` per §16, not a bare `MCP/` dir) rather than a file-per-class sprawl — the registration tax makes fine-grained files genuinely costly here.
- **Add C-ABI entry points to the END of `RISE_API.h`** (ABI rule: never reorder/resize existing exports; see the `abi-preserving-api-evolution` skill). `RISE_API.h` itself is already registered, so new functions cost zero project edits — another reason to push surface through the C-ABI rather than new headers.
- **Platform shells get only marshaling + the §1.2 forced set.** A new SwiftUI/Qt/Compose file is fine when it is *rendering* a shared model; it is a smell when it *computes* one.
- **Watch the enum/bridge drift surfaces** (`MEMORY.md` notes): the generator in §3.2 is the durable fix; until then, `static_assert` enum mirrors on the C/C++ side and grep both bridges when adding a `Category`/`PanelMode` value.

---

## 8. Per-feature shared-vs-platform table (expands roadmap §10.2 across all five Directions)

For every roadmap feature: the **shared component** (library, write-once) and the **platform shell** (marshaling + the forced bits). "Android tier" applies §6.

| Dir | Feature | Shared C++ component (library) | Platform shell (per-OS) | Android tier |
|---|---|---|---|---|
| — | Scene model, edit history, round-trip save | `Scene`, `SceneEditor`, `EditHistory`, `SaveEngine`, dirty trackers | desktop: wired; Android: `nativeSaveAs` → real `.RISEscene` save is the §16 to-do (saves only the image today) | A |
| — | Tool state machine, cancel-restart render loop, pointer→edit | `SceneEditController` | input-event capture + coord normalization to image-pixels | A |
| — | Frame delivery / present | `ViewportFrameStore`, `FrameStore`, `RenderToBuffer`, generation-counter poll | present sink: Metal layer / DXGI swap chain / Compose Bitmap | A (SDR); HDR=C |
| A | A1 sensible-default scene | default-scene assembly in `Job`/controller | — | A |
| A | A2 progressive viewport (pause-on-nav, restart-on-edit) | `InteractivePelRasterizer`, adaptive-scale state machine, idle refine | frame pump (display-link/timer/Choreographer) | A |
| A | A3 drag-to-assign material/HDRI | hit-test → `SceneEdit`; hover-preview render via controller | drag-drop gesture + thumbnail widget | B (touch drag) |
| A | A4 region / ROI render | `RasterizeRegion` + region state in controller | modifier+drag box UI | B |
| A | A5 thumbnail asset library + search | asset index + thumbnail-render-to-buffer in library | thumbnail grid widget, SAF file picks | A (browse) / B (drag) |
| A | A6 progressive disclosure / layout presets | "basic vs advanced" flags on descriptor rows | which rows a panel shows; workspace layout | A |
| B | B1 named views (pose + thumbnail, restore) | named-views model + thumbnail render in library | thumbnail list widget | B |
| B | B2 axis-ball nav gizmo + axis snaps | snap math + gizmo handle layout in controller | axis-ball widget draw + tap routing | B |
| B | B3 "fly then stamp" camera | camera pose capture → `SceneEdit` (promote-to-camera) | "stamp" button | B |
| B | B4 camera list panel | accordion Camera category (shipped) + `addCameraFromActive` | list/thumbnail widget | A (list) / B (thumbs) |
| B | B5 split / quad view | multi-viewport orchestration + **scene-walk + ortho-camera → `LineSegmentBatch`** (shared; §5.4) | pane layout + the per-API `IAuxGpuBackend` wireframe draw (§5.4) — or a shared CPU/bbox fallback where no backend | C |
| C | C1 instance material editor | `MaterialIntrospection` descriptor rows + `SetProperty` (shipped surface) | slider/swatch/preset widgets | B |
| C | C2 node-graph material editor | **material-graph model + serialization** (new), connection-legality from descriptors | node-canvas widget | C |
| C | C3 MaterialX/OpenPBR import | importer + color-vs-scalar routing (`IPainter`/`IScalarPainter`) in library | file picker only | A (runs headless) |
| D | D1 spectral curve / color picker | `spectral_painter`/`PiecewiseLinearScalarPainter`/`blackbody`/Sellmeier math | curve/slider/swatch widget | B |
| D | D2 measured-metal n,k picker | n,k tables + GGX conductor wiring in library | picker widget + (optional) refractiveindex.info fetch | B |
| D | D3 thin-film slider | `fresnel_mode thinfilm` (shipped) + angle-reactive eval | thickness slider + swatch | B |
| D | D4 JH gamut warning | gamut-edge test (`JH_LUT_GAMUT`) in library | warning badge in the picker | B |
| D | D5 "explain the auto-router" heatmap | variance-probe data + per-region rationale (auto-rasterizer) in library | heatmap overlay + tooltip | C (overlay) |
| D | D6 EDR cinematography scopes | waveform/vectorscope/false-color *math* over FrameStore in library (the scope `ScopeRequest`/binning, §5.4) | scope compute via the per-API `IAuxGpuBackend` (§5.4) + scope draw + EDR present surface | C (needs EDR) |
| D | D7 spectral Light Mix | per-light AOV render + re-balance math in library | mix-slider panel | B |
| E | E0 MCP server (read scene/grammar/framebuffer, `validate`, render-control) | **MCP server + tool/resource dispatch** (new, library) | — (loopback transport; optional external clients) | A |
| E | E1 in-app chat + provider adapters (Claude/Gemini/local) | **agent loop + provider adapters + transcript model** (new, library) | chat-bubble UI + **credential store (§4)** + **OAuth UI** | A (chat is mobile-native) |
| E | E2 edit/create tools via controller (L2 operate) | tool dispatch → `SceneEditController` + scoped permissions in library | "show me the code" diff panel | A |
| E | E3 vision feedback loop + external MCPs | framebuffer-as-resource + agent loop in library | image-in-chat rendering | A |

**The pay-off, restated:** every Direction-E row's heavy lifting is library-side and write-once; the *only* per-platform LLM code is the chat bubble UI, the credential store (§4), and the OAuth consent UI. All three platforms — including Android — get full agent control from one C++ implementation.

---

## 9. Non-goals

- **A new shared bridge *class*.** Not needed — `SceneEditController` + the `RISE_API.h` C-ABI already are the bridge core. The work is consolidating the *marshaling*, not introducing another layer.
- **Byte-identical bridge files across platforms.** Threading/lifetime/present differ legitimately (§3.4); forcing them identical would be wrong.
- **Porting the *production path tracer* to the GPU per platform.** RISE is a CPU path tracer; there is no GPU path-tracing path. "Metal/DXGI/Vulkan" are *display* surfaces (§5) — plus, for B5/D6 only, the *auxiliary* GPU render/compute backend (§5.4). The integrator itself is never ported to the GPU; the aux backend draws orientation wireframe / runs scope compute, not light transport.
- **Android HDR/EDR now.** Deferred (Tier C) until a Vulkan/`SurfaceControl` present sink is justified; it will be additive and touch zero library render code (§5.3).
- **Rolling our own credential encryption.** Defer to OS keystores (§4.3).
- **Domain logic in any platform shell.** Kotlin/Swift/Qt code that *computes* a scene/render answer (vs *renders* a shared model) is rejected (§7).
- **A second mutation path for AI edits.** AI routes through `SceneEditController` like every other surface (roadmap principle 6); this doc's C-ABI reservations in §3.3 honor that.
- **Per-platform MCP servers or per-platform provider adapters.** Written once in the library; the only per-platform LLM code is chat UI + credential store + OAuth UI.

---

## 10. Open questions / spikes

1. **Enum/shim generator scope.** Is `tools/gen_bridge_enums.py` (Kotlin emit + Obj-C `static_assert` emit) enough, or do we also auto-generate the indexed-list/string-out shims? Start with enums (highest drift risk), measure.
2. ~~**Where exactly do the MCP/LLM files live?**~~ **DECIDED (GUI_ROADMAP §16):** a combined `src/Library/Agent/` — *not* a bare `src/Library/MCP/` (the `MCP` token already means *Master Control Program* in `src/DRISE/MCPClientConnection`). Presented as "the RISE agent tool surface (MCP protocol)." This doc's tables (§1.1, §7) reflect that. Owned jointly with `MCP_TOOL_SURFACE.md` / `LLM_AGENT_RUNTIME.md`.
3. **Credential-store injection point.** A global `RISE_API_SetCredentialStore` vs a setter on an LLM-runtime handle. Prefer the latter once the runtime handle exists.
4. **Android HDR present.** When/if pursued: Vulkan + `SurfaceControl` HDR vs waiting for a Compose EDR surface. Spike the Vulkan textured-quad-of-`RGBA16F` path against the existing `RenderToBuffer` contract.
5. **Headless/CI parity for the agent.** The shared agent runtime + `MemoryCredentialStore` should be drivable from a test harness with no GUI — confirms the boundary is clean (any GUI dependency in the runtime is a bug).
6. **Provider-adapter interface shape.** Minimal C++ interface fitting Claude tool-use + Gemini function-calling + OpenAI-compatible, incl. streaming + auth. Owned by `LLM_AGENT_RUNTIME.md`; this doc only fixes that it is library-side and credential-store-injected.
7. **Auxiliary GPU backend interface shape (B5/D6).** Confirm the exact `IAuxGpuBackend` surface (§5.4) — the backend-neutral work-item types (`LineSegmentBatch`, `ScopeRequest`, `AuxRenderTarget`), whether B5 wireframe and D6 scopes share one backend or split, and the CPU/SDR fallback contract — against a B5/D6 spike. Co-owned with `CAMERAS_AND_VIEWS.md` (B5) and the D6 owner. Invariant the spike must preserve: the shared core links with zero GPU-API symbols.

---

## 11. Acceptance criteria (GUI_ROADMAP §15 template, filled in)

This is the architectural-backbone doc, so its acceptance covers the *consolidation + abstraction* work it specifies (bridge-enum generator, credential-store abstraction, present-sink contract), all of which must be **behavior-neutral**.

- **Tests.**
  - *Enum-parity (§3.2):* `tools/gen_bridge_enums.py` emits Kotlin constants + Obj-C `static_assert` mirrors of `SceneEditTool_*` / `SceneEditCategory_*` / gizmo / panel enums; a build-time check fails if a `RISE_API.h` enum value drifts from a shell copy (the durable fix for the MEMORY "bridge enum-translation audit" / `case 5:` fall-through trap). Invariant: enum drift is a *compile* failure, not a runtime fall-through to `None`.
  - *Credential store (§4):* a `MemoryCredentialStore` round-trips Store/Load/Erase/HasKey headlessly; each platform implementer (Keychain / Credential Manager / Keystore) passes the same store/load/erase contract test; the agent runtime depends only on `ICredentialStore*` (no platform type leaks — a grep/compile guard).
  - *Present-sink contract (§5):* `RenderToBuffer` produces byte-identical RGBA8 (LDR) and RGBA16F-extended-linear (EDR) output across platforms from the same canonical `FrameStore`; the generation-counter poll emits only when the counter advanced (no redundant present). Invariant: pixels are computed once in shared C++; only the surface differs.
  - *Auxiliary GPU backend boundary (§5.4) — TO-BUILD:* the shared core (B5 scene-walk + ortho-camera → `LineSegmentBatch`; D6 scope math) compiles and links against `IAuxGpuBackend` **with no GPU-API symbol** (a grep/compile guard mirroring the credential-store "no platform type leaks" check); a null/`IsAvailable()==false` backend makes B5 fall back to a CPU wireframe/bbox preview and D6 to SDR/"edit on desktop" rather than failing the build or crashing. Invariant: the shared core never names Metal/D3D/Vulkan; the production integrator is byte-untouched by the presence or absence of an aux backend.
  - *Behavior-neutral bridge refactor:* after shrinking each bridge to a near-generated marshaling layer, the existing macOS/Windows/Android viewport behavior (tool state, picking, gizmo, property edit, save) is unchanged — the existing controller/save tests pass verbatim (the consolidation removes duplicated *binding*, not behavior, §2).
  - *Correctness invariant (engine-touching):* none of this doc's work touches an integrator; any render driven through the consolidated path is byte-identical to today (deferred concretely to [RENDER_COORDINATOR.md §9](RENDER_COORDINATOR.md)).
- **Platform parity.** The entire boundary is shared C++ + a thin marshaling shell per platform. macOS / Windows / Android bind the *same* C-ABI; the only legitimate per-bridge differences are threading/lifetime guards, frame-delivery lifecycle, the present surface (§3.4), and — for B5/D6 — the per-API `IAuxGpuBackend` implementer behind the §5.4 interface. HDR/EDR present is macOS + Windows (Tier A); Android present is **SDR (Tier A), HDR/EDR Tier C** until a Vulkan/`SurfaceControl` sink lands (§5.3) — additive, zero library *production*-render change. The auxiliary GPU backend is likewise additive (it adds an aux render/compute surface, never touches the integrator) and degrades to a CPU/SDR fallback where unavailable (§5.4, §6.3).
- **Performance budget.** Pure boundary/marshaling work: no render-path code, so **no production-render regression** (the L8 ~0.4% bar is untouched). The generated marshaling shims add no per-call cost beyond the existing fixed-buffer copy idioms (§3.2). Present remains a lockless generation-counter poll (§5.2) — no added frame latency.
- **Memory budget.** Negligible. No new resident buffers in the boundary layer; the credential store holds short strings; the present path reuses the existing `RenderToBuffer` staging. New agent/credential subsystems size themselves (owned by their specs).
- **Accessibility.** This doc adds no direct UI surface; it governs *where* logic lives. The per-platform shells it specifies must keep their existing keyboard/focus/no-colour-only behavior; the new per-platform surfaces it sanctions — the OAuth consent UI (§1.2 #5) and the auxiliary GPU backend's host views (B5 wireframe panes / D6 scopes, §1.2 #7 / §5.4) — must be keyboard-reachable and screen-reader-labeled per their owning specs (CAMERAS_AND_VIEWS §15 for B5; GUI_ROADMAP §13.D6 for the scopes).
- **Packaging.** No shipped assets from this doc. Any new shared file (the credential-store impls' shared base, the agent/MCP-protocol subsystem under `src/Library/Agent/` per §16, the **`IAuxGpuBackend` interface + its backend-neutral work-item types** of §5.4, the enum-generator output if checked in) lands under `src/Library/` and so must be registered in all **five** build projects (Filelist, Android cmake, VS2022 `.vcxproj` + `.filters`, Xcode pbxproj) — §7's central economic rule. The per-API `IAuxGpuBackend` *implementers* are platform-shell files (one project each), like the present sinks. Platform-shell files cost one project each.
- **Migration.** No scene-format change. **ABI: additive only** — new C-ABI entry points append to the END of `RISE_API.h`; existing exports are never reordered or resized, per the `abi-preserving-api-evolution` skill. The bridge consolidation is internal (the C-ABI contract is *frozen and complete*, §3.2), so out-of-tree callers are unaffected. No auto-migration tool needed.
- **Rollback.** The enum generator can be disabled (fall back to hand-mirrored enums + `static_assert` guards, §7) without behavior change. The credential store defaults to `MemoryCredentialStore` (process-lifetime, headless/CI fallback, §4.1) if a platform implementer is absent — keyless/local-endpoint mode still works. No saved `.RISEscene` depends on any of this, so any piece can be reverted without breaking a scene.

### Android tier note
The boundary itself is **Tier A by construction**: Android consumes the identical C++ core via JNI (§2, §6), so every shared subsystem this doc places in the library reaches Android for free — only the marshaling shim, the present sink (SDR Tier A / EDR Tier C, §5.3), the Keystore credential implementer (§4.2), and the OAuth consent UI are Android-specific. The §6 tiering (Tier A must-have incl. full LLM and the §16 `.RISEscene` scene-save wiring; Tier B touch-adapted; Tier C desktop-first) is what every other spec's Android note conforms to, with this doc as the governing authority.
