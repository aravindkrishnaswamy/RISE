# FrameStore Redesign — Detailed Design

**Status**: **Phase 1 mostly landed.** L0 (math foundation), L1 (FrameStore + Channel + locks + Render readback + observers), L2 (IFrameEncoder + 7 byte-identical-to-legacy encoders + registry), L3 (FrameSink + FileEncoderObserver + FileRasterizerOutput shim), and L4 platform-agnostic primitives (`ViewportFrameStore` for the GUI viewport facade) are complete and warning-free with full regression coverage. The CLI runs end-to-end through the new pipeline (verified manually with `scenes/Tests/Geometry/shapes.RISEscene`). Platform-specific GUI wiring (macOS SwiftUI / Windows Qt / Android Compose) and L5+ (HDR display, Phase 2 rasterizer rewrite) not yet started. See §11 for per-landing status.
**Author**: design conversation 2026-05-07; updated through L4 on 2026-05-08.
**Scope**: Replace today's "rasterizer-pushes-IRasterImage-into-IRasterizerOutput-sinks-which-each-bake-format-and-tone-mapping" architecture with a canonical-HDR-buffer model that decouples accumulation, notification, and encoding. Targets: keep the CLI byte-identical, give the UI a persistent HDR buffer it can read for live display (LDR or HDR-EDR), live exposure scrubbing, and arbitrary-format Save-As.
**Related**: [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md) §19.8 ("Persistent IRasterImage on every rasterizer") which started this direction; this proposal generalises that beachhead.
**ABI break**: yes, allowed. Phase 2 changes `RISE_API_Create*Rasterizer` signatures and reshapes `IRasterizer` and `IRasterizerOutput`.

## Implementation status (through L4 platform-agnostic primitives)

**Total: 563 test assertions, 0 failures, 0 warnings on `-O3 -flto -ffast-math -Wall -pedantic`.**

| Layer | Status | Test count | Files |
|---|---|---|---|
| L0 — math foundation | ✅ shipped | 303 | `FrameStoreColorSpace.{h,cpp}`, `TargetFormat.{h,cpp}`, `ViewTransform.{h,cpp}`, `tests/FrameStoreColorMathTest.cpp` |
| L1 — FrameStore + Channel + tile lock + Render | ✅ shipped (3 review rounds) | 83 | `FrameStore.{h,cpp}`, `Channel.h`, `IRenderObserver.h`, `tests/FrameStoreTest.cpp` |
| L2 — IFrameEncoder + registry + 7 encoders + byte-identical regression | ✅ shipped | 69 | `IFrameEncoder.h`, `FrameEncoders.{h,cpp}`, `tests/FrameEncoderTest.cpp` |
| L3 — FrameSink + FileEncoderObserver + FileRasterizerOutput shim | ✅ shipped | 60 | `FrameSink.{h,cpp}`, `FileEncoderObserver.{h,cpp}`, rewritten `FileRasterizerOutput.cpp`, `tests/FileRasterizerOutputShimTest.cpp` |
| L4a — Platform-agnostic `ViewportFrameStore` primitives | ✅ shipped (3 review rounds + L4b/c/d rounds 4 & 7 hardening) | 53 | `ViewportFrameStore.{h,cpp}`, `tests/ViewportFrameStoreTest.cpp`. Plus L1 hardening: `BeautyRasterImageView::DumpImage` now acquires all per-tile shared_locks for the duration of the dump (L4 round-1 HIGH-1), making encoder paths (L2 / L3 / L4) safe to call mid-render. Plus L4-round-2 fixes: `OutputIntermediateImage` now copies the affected region tile-by-tile and fires per-tile `OnTileComplete` callbacks (P1-1 — was a no-op, dropping live viewport tile updates); reader paths (`RenderToBuffer` / `SaveAs` / `SaveTo` / `Generation` / `SetCameraExposureCompensationEV`) now snapshot-and-addref the FrameStore under a facade-level `chainMutex_` so a concurrent resolution-change reallocation in the rasterizer thread can't UAF the reader's pointer (P1-2). Plus L4-round-3 fix: `OutputIntermediateImage` was treating the `Rect*` argument as half-open at the IRasterizerOutput boundary even though RISE rasterizers (per `PixelBasedRasterizerHelper::BoundsFromRect`) emit INCLUSIVE bounds; single-pixel regions sitting exactly on a tile boundary (e.g. `Rect(32, 32, 32, 32)`) silently fired zero tile callbacks (P2). Fixed: convert `pRegion->bottom + 1` / `right + 1`, clamped to image size, before the half-open ceiling-division tile-coverage math; tests use the inclusive convention; new boundary regression test for `Rect(32, 32, 32, 32)` → exactly one callback. |
| L4b — macOS SwiftUI viewport wiring | ⏳ next | — | Use `ViewportFrameStore` to replace `BlockRasterizerOutput` in `RISEBridge.mm`; Save-As menu in ContentView.swift; exposure slider |
| L4c — Windows Qt viewport wiring | ⏳ pending | — | Replace `ImageOutputAdapter` + `ViewportPreviewSink` in `RenderEngine.cpp` / `ViewportBridge.cpp` |
| L4d — Android Compose viewport wiring | ⏳ pending | — | Replace `RiseBridge` viewport sink |
| L4 — GUI viewport integration | ⏳ pending | — | — |
| L5a/b/c — Mac EDR + Windows HDR + PQ encoder | ⏳ pending | — | — |
| L6 — Phase 2 rasterizer rewrite (ABI break) | ⏳ pending | — | — |
| L7 — AOV plumbing through rasterizers | ⏳ pending | — | — |
| L8 — Remove `IRasterizerOutput` + `AOVBuffers` | ⏳ pending | — | — |

### L4 platform integration pattern

Each GUI platform (macOS SwiftUI, Windows Qt, Android Compose) follows the same recipe to integrate `ViewportFrameStore`:

```cpp
// Bridge layer (RISEBridge.mm / RenderEngine.cpp / RiseBridge.cpp):

// 1. Construct ONCE, per scene/Job lifetime:
auto* vfs = new ViewportFrameStore();   // Reference: refcount = 1

// 2. Wire callbacks BEFORE attaching to a rasterizer.  These fire
//    from rasterizer worker threads — platform code marshals to UI
//    thread (Qt signals / dispatch_async / Compose LaunchedEffect):
vfs->SetTileCompleteCallback(
    []( const Rect& roi, uint64_t gen ) {
        platformMarshalToUIThread( [=]{ requestRepaint( roi, gen ); } );
    } );
vfs->SetFrameCompleteCallback(
    []( unsigned frame, uint64_t gen ) {
        platformMarshalToUIThread( [=]{ requestRepaint( /*full*/, gen ); } );
    } );

// 3. Attach to the active rasterizer (rasterizer's outputs list addrefs):
vfs->Attach( rasterizer );

// 4. On UI-thread display refresh, render into platform-native pixel buffer:
const ViewTransform xf = viewportIsHDR
    ? ViewTransform::ForHDRDisplay( exposureSliderEV )
    : ViewTransform::ForLDRDisplay( exposureSliderEV, ACES );
const TargetFormat fmt = viewportIsHDR
    ? TargetFormat::RGBA16F_ExtendedLinearSRGB     // L5a/b
    : TargetFormat::RGBA8_sRGB;                    // L4
vfs->RenderToBuffer(
    platformBuffer, platformStride, viewportRect, fmt, xf );
// platform code commits the buffer to its display surface

// 5. On Save-As menu pick:
IFrameEncoder* enc =
    FrameEncoderRegistry::Get().ByExtension( pickedExtension );
EncodeOpts opts;
opts.colorSpace = pickedColorSpace;
opts.viewTransform = ViewTransform::ForLDRDisplay( exposureSliderEV, ACES );
vfs->SaveAs( pickedPath, enc, opts );

// 6. On rasterizer swap (PT → BDPT in UI):
//    Old rasterizer is destroyed (or its outputs freed); the
//    ViewportFrameStore + FrameStore + callbacks ALL persist.
//    Just attach to the new one:
oldRasterizer->FreeRasterizerOutputs();   // see Detach() doc
vfs->Attach( newRasterizer );
//    No re-binding of callbacks, no FrameStore reallocation.

// 7. Teardown (scene unload, app exit):
vfs->release();
```

The platform-specific work is purely the marshaling layer (Qt signals / dispatch_async / Compose remember-state), the exposure slider widget, and the Save-As menu. The pixel pipeline, lifetime semantics, and rasterizer-swap behavior are all encapsulated in `ViewportFrameStore`.

### L0–L2 deviations from the original design

The implementation matches the original design intent in §3–§7, with these minor refinements that emerged during landing:

1. **Reference convention everywhere**: all new refcounted types (`FrameStore`, `BeautyRasterImageView`, encoders) use RISE's intrusive `Reference` base instead of `std::shared_ptr`. Construction is `new T(...)` (refcount starts at 1); release via `safe_release()` or `t->release()`. Matches the rest of the library.
2. **Tile-level `std::shared_mutex` instead of atomic seqlock**: the original design called for an atomic-seqlock with explicit memory fences. L1 round 2 review (P1) flagged this as UB on non-atomic pixel storage per the C++ memory model, regardless of fence correctness. The fix is per-tile `std::shared_mutex` — N readers + 1 writer, C++-standard data-race-free. See §4.
3. **Observer dispatch with in-flight counter + thread-local depth**: the snapshot-then-iterate pattern from §3.6 is augmented with (a) `observerDispatchInFlight_` + `observerDispatchDone_` cv so cross-thread `RemoveObserver` waits for in-flight callbacks before returning (P2), (b) thread-local depth counter so same-thread self-detach skips the wait without deadlock (P2), (c) per-iteration recheck of `observers_` in the dispatch loop so observer A removing-and-destroying observer B in the same snapshot doesn't UAF on B's freed pointer (P2 round 3).
4. **`ChannelTraits<Alpha>::Type = Chel`**, not `float`: the original design said float, but L2 review HIGH-1 found that the double→float→double roundtrip breaks byte-identity for non-1.0 alpha values. Now stored as Chel (double); 4-byte-per-pixel cost is acceptable.
5. **`TransferFunction::Linear` is true identity**: original design had it Sanitise-clamped (NaN/Inf/negatives → 0), which is wrong for HDR archival. L1 round 2 P3 fixed this — Linear now passes through bit-identically; sanitise responsibility shifts to the LDR-fixed quantisation step in `EncodePixel`.
6. **Two new `TargetFormat` values**: `RGBA32F_ROMM_Linear` and `RGB32F_ROMM_Linear` for true bit-identical scene-referred archival in RISE's native ROMM primaries (the existing `*_Linear` variants use sRGB primaries, the industry-default for EXR archival).
7. **`cameraExposureEV` consumed via `FrameStore::Meta()`**: encoders read `store.Meta().cameraExposureEV` and add to `opts.viewTransform.exposureEV` to mirror legacy `FileRasterizerOutput.cpp:231`'s `staticEV + cameraEV` sum. L2 review HIGH-2 fix.

---

## 1. Goals & non-goals

### Goals

1. **Canonical HDR buffer.** The output of any render is a `FrameStore` holding RISEPel-precision linear radiance (and optional AOV channels). Every consumer — viewport display, file save, denoiser input, network mirror — reads from this one buffer.
2. **Polymorphic readback.** A single `FrameStore::Render(target_format, view_transform)` call services LDR-sRGB (PNG, web preview), HDR-EDR (Mac extendedSRGB / Display P3, Windows scRGB), HDR10 PQ (BT.2020 / TVs / future HDR file formats), linear-float (EXR archival), and 16-bit-fixed (legacy GUI) targets.
3. **Live exposure / tone-curve scrubbing.** UI reads from the same HDR buffer on every repaint; changing the view transform does not invalidate the buffer or restart the rasterizer.
4. **Multi-format Save-As decoupled from rasterizer construction.** The user picks a format from a menu after the render is done (or mid-render) and `IFrameEncoder::Encode(*frameStore, ostream, opts)` produces bytes. No re-render, no rasterizer reconstruction.
5. **CLI byte-identical for existing scenes.** `file_rasterizeroutput { … }` chunks keep working. Any existing `.RISEscene` produces bit-identical output bytes.
6. **AOV-capable but not AOV-mandatory.** `FrameStore` can carry albedo / normal / depth / object-id channels; rasterizers that don't produce them allocate nothing.

### Non-goals (for this redesign)

- Mid-render scene mutation. (`InteractiveEditor` handles that separately.)
- Network output (DRISE socket sink keeps working as today, just observes the FrameStore).
- A new file-format encoder beyond what already exists (PNG/EXR/HDR/RGBEA/TIFF/TGA/PPM). Multichannel-EXR-with-AOVs and HDR10 PQ encoding are flagged as follow-ups.
- Removing the existing `IRasterImageWriter` hierarchy. Encoders wrap writers; writers stay.

---

## 2. Architectural overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Phase 2 (target)                              │
│                                                                         │
│  ┌──────────────┐   writes pixels   ┌─────────────────────────────────┐ │
│  │  Rasterizer  │ ─────────────────▶│            FrameStore           │ │
│  │  (PT/BDPT/…) │                   │  ┌──────────────────────────┐   │ │
│  └──────┬───────┘                   │  │ Channel<RISEPel> beauty  │   │ │
│         │                           │  │ Channel<Vector3> normal? │   │ │
│         │ notifies                  │  │ Channel<RISEPel> albedo? │   │ │
│         │  (no data)                │  │ Channel<float>   depth?  │   │ │
│         ▼                           │  └──────────────────────────┘   │ │
│  ┌─────────────────┐                │  + tile-seqlock concurrency     │ │
│  │ IRenderObserver │                │  + Render(fmt, xform) readback  │ │
│  │  list           │                │  + Snapshot<T>(channel)         │ │
│  └─────┬───────────┘                └────────┬───────────┬─────────────┘ │
│        │                                     │           │               │
│   ┌────▼─────────┐  ┌──────────────────┐  reads        reads             │
│   │ Viewport     │  │ FileEncoder      │ ─────┐    ┌────────────┐        │
│   │ Observer     │  │ Observer         │      │    │ UI repaint │        │
│   │ (Qt/SwiftUI/ │  │ (per scene-      │      ▼    │ loop       │        │
│   │  Compose)    │  │  declared        │ ┌──────────────────┐  │        │
│   └──────────────┘  │  output chunk)   │ │ IFrameEncoder    │  │        │
│                     └────────┬─────────┘ │ • PNGEncoder     │  │        │
│                              │           │ • EXREncoder     │  │        │
│                              │           │ • HDREncoder     │  │        │
│                              ▼           │ • TIFFEncoder    │  │        │
│                       writes file        │ • …              │  │        │
│                                          └──────────────────┘  │        │
│                                                                ▼        │
│                                                        Display surface  │
│                                                        (CALayer/QImage) │
└─────────────────────────────────────────────────────────────────────────┘
```

Three independent concerns:

| Concern | Phase 2 owner | Today's owner |
|---|---|---|
| Accumulate samples → HDR buffer | `Rasterizer` writes into `FrameStore` directly | `Rasterizer` keeps internal `IRasterImage`; copies to outputs at completion |
| Notify "tile/frame ready" | `IRenderObserver` event bus | `IRasterizerOutput::OutputIntermediateImage / OutputImage` (data + notification entangled) |
| Convert HDR buffer → bytes (file/screen) | `IFrameEncoder` (file) + `FrameStore::Render` (screen) | `FileRasterizerOutput` baked at construction time |

**Phase 1 (B)** keeps the rasterizer unchanged: the existing `IRasterImage` push path is wrapped in a `FrameSink : IRasterizerOutput` that copies into a FrameStore. There's a redundant HDR copy on every tile, but no rasterizer surgery. Phase 2 (C) eliminates the copy.

---

## 3. Core types

### 3.1 `Channel<T>`

Strongly typed, row-major, contiguous. SoA storage means each channel is independently allocated (no AOV cost when not in use).

```cpp
template <typename T>
class Channel {
    std::unique_ptr<T[]> data_;
    size_t width_, height_, stride_;  // stride in elements, allows future tiled layouts
public:
    Channel(size_t w, size_t h);
    size_t Width() const  { return width_; }
    size_t Height() const { return height_; }

    T*       Row(size_t y);
    const T* Row(size_t y) const;
    T&       At(size_t x, size_t y);
    const T& At(size_t x, size_t y) const;

    // Bulk access for encoders that want to walk linearly.
    T*       Data();
    const T* Data() const;
    size_t   Stride() const { return stride_; }
};
```

### 3.2 `FrameStore`

The canonical artifact.

```cpp
enum class ChannelId : uint32_t {
    Beauty,         // RISEPel, ROMM RGB linear, always present
    Alpha,          // float, [0,1]
    Albedo,         // RISEPel, ROMM RGB linear (denoiser AOV / export)
    Normal,         // Vector3, world space, unit length
    Depth,          // float, camera-space distance
    ObjectId,       // uint32_t
    PrimitiveId,    // uint32_t
    // … extension points …
};

class FrameStore : public RefCounted /* or std::shared_ptr<>-managed */ {
public:
    struct Spec {
        size_t width, height;
        size_t tileEdge = 32;                       // matches existing tile sizes
        std::vector<ChannelId> channels;            // Beauty implicit; rest opt-in
        Metadata meta;                              // scene name, camera id, sample count
    };

    explicit FrameStore(const Spec&);

    // ── geometry ──────────────────────────────────────────────────────
    size_t Width() const;
    size_t Height() const;
    size_t TileEdge() const;
    size_t TileCountX() const;
    size_t TileCountY() const;

    // ── channel presence ──────────────────────────────────────────────
    bool HasChannel(ChannelId) const;
    template <typename T> Channel<T>&       GetChannel(ChannelId);
    template <typename T> const Channel<T>& GetChannel(ChannelId) const;
    // Compile-time channel-to-type binding lives in a traits header.

    // ── write-side API (rasterizer) ───────────────────────────────────
    // Phase 1: not called directly; FrameSink uses bulk CopyTileFrom.
    // Phase 2: rasterizer's per-tile worker calls these.
    void BeginTile(size_t tileX, size_t tileY);
    void EndTile  (size_t tileX, size_t tileY);     // bumps tile seq + global generation

    // Bulk path for Phase 1 FrameSink.
    void CopyTileFromRasterImage(size_t tileX, size_t tileY,
                                 const IRasterImage& src, const Rect& srcRect);

    // Final-frame signal (separate from per-tile so observers know the render is complete).
    void MarkFrameComplete(unsigned frame);

    // ── observer registration ─────────────────────────────────────────
    // Observers attach to the FrameStore, NOT to the rasterizer. The
    // FrameStore is the persistent artifact; the rasterizer is the
    // producer and may be swapped (PT → BDPT in the UI) without
    // detaching observers. See §7.5.
    void AddObserver     (IRenderObserver*);
    void RemoveObserver  (IRenderObserver*);
    void EnumerateObservers(IEnumCallback<IRenderObserver>&) const;

    // ── read-side API (UI, encoders) ──────────────────────────────────
    uint64_t Generation() const;        // monotonically increases on tile/frame events

    // The polymorphic readback. dst is row-major in `fmt`'s layout;
    // dstStride is in bytes per row.
    void Render(void*               dst,
                size_t              dstStride,
                Rect                roi,
                TargetFormat        fmt,
                const ViewTransform& xform) const;

    // Direct access for encoders (no transform).
    template <typename T> ConstChannelView<T> Snapshot(ChannelId) const;

    // Metadata.
    const Metadata& Meta() const;
    Metadata&       MutableMeta();      // for rasterizer to update sample counts etc.

    // ── back-compat bridge ────────────────────────────────────────────
    // Returns the Beauty channel as an IRasterImage view, for code paths that
    // still want IRasterImage (denoiser, legacy writers used by encoders).
    // Phase 1 only — Phase 2 callers move to typed channel APIs.
    IRasterImage& AsBeautyRasterImage();
    const IRasterImage& AsBeautyRasterImage() const;
};
```

**Design decisions baked in:**

- **SoA storage.** Each channel has its own buffer; AOVs allocate iff requested. `Channel<T>` is `T*`-contiguous so encoders that walk a single channel get cache-line-friendly access.
- **Tile-aligned coordinates.** Tile size lives in `Spec`. Rasterizers pick the tile size; FrameStore mirrors it. This lets the seqlock be tile-granular and avoids per-pixel atomics.
- **Beauty channel is mandatory; others optional.** Rasterizer factories declare which channels they populate; FrameStore allocates only those.
- **Metadata is part of the artifact.** Sample count, scene name, camera info, render time — used by EXR multichannel attrs and any future "render history" feature. Read-write so the rasterizer can update sample counts during render.
- **`AsBeautyRasterImage()` is an explicit, scoped back-compat shim.** It lets the existing OIDN denoiser path (which takes `IRasterImage`) and the existing per-format `IRasterImageWriter` instances keep working unchanged in Phase 1. Phase 2 deletes this method.

### 3.3 `ViewTransform`

Stages composed in a fixed pipeline; each stage is optional.

```cpp
enum class ToneCurve : uint32_t {
    None,           // identity
    ACESFilmic,     // Hable / Narkowicz ACES approximation
    Reinhard,
    Hable,          // Uncharted 2 filmic
    Linear,         // simple clamp at 1.0
};

struct ViewTransform {
    // Stage 1 — exposure (always available; 0 EV = identity).
    float exposureEV = 0.0f;

    // Stage 2 — white balance / chromatic adaptation. Identity by default.
    Mat3 whiteBalance = Mat3::Identity();

    // Stage 3 — tone curve. Skipped on linear/HDR targets even if set
    // (TargetFormat governs whether tone mapping applies).
    ToneCurve toneCurve = ToneCurve::None;
    float     toneCurveStrength = 1.0f;

    // Stage 4 — color space matrix is determined by TargetFormat,
    // not stored here.
    // Stage 5 — output transfer (gamma encode) is determined by TargetFormat.

    // ── presets ──
    static ViewTransform Identity();                // EXR-style (no transforms)
    static ViewTransform ForLDRDisplay(float ev = 0.0f, ToneCurve tc = ToneCurve::ACESFilmic);
    static ViewTransform ForHDRDisplay(float ev = 0.0f);   // exposure on, no tone curve
};
```

Pipeline ordering (immutable contract, encoded in `FrameStore::Render`):

```
ROMM-linear pixel  → exposure (multiply by 2^EV)
                   → white balance (3×3 matrix in ROMM)
                   → ROMM → target color space matrix    [from TargetFormat]
                   → tone curve                          [iff TargetFormat is LDR-fixed]
                   → output transfer (sRGB / PQ / Linear) [from TargetFormat]
                   → quantise into TargetFormat pixel layout
```

### 3.4 `TargetFormat` and `ColorSpace`

`TargetFormat` is the *only* parameter the caller specifies on `Render`; it bundles pixel layout + color space + transfer. `ColorSpace` is internal, used by the conversion math.

```cpp
enum class TargetFormat : uint32_t {
    // 8-bit fixed (LDR display, common file formats)
    RGBA8_sRGB,                     // Qt QImage::Format_RGBA8888, PNG/TGA
    RGB8_sRGB,
    BGRA8_sRGB,                     // Win32 DIB native order

    // 16-bit fixed (legacy GUI path; PNG-16; TIFF-16)
    RGBA16_sRGB,                    // current ImageOutputAdapter format

    // 16-bit float (HDR display)
    RGBA16F_ExtendedLinearSRGB,     // Mac CAMetalLayer EDR
    RGBA16F_DisplayP3,              // Mac wide-gamut path
    RGBA16F_BT2020_PQ,              // HDR10 / TVs (PQ transfer applied)
    RGBA16F_Linear,                 // raw linear half-float (debug, intermediate)

    // 32-bit float (archival, debug)
    RGBA32F_Linear,                 // EXR linear
    RGB32F_Linear,                  // .hdr file
};

enum class ColorSpace : uint32_t {
    ROMM_Linear,
    sRGB_Linear, sRGB,
    DisplayP3_Linear, DisplayP3,
    BT2020_Linear, BT2020_PQ, BT2020_HLG,
    BT709_Linear, BT709_Gamma22,
    ACEScg, ACES2065,
};
```

`Render` consults a static table mapping `TargetFormat` → `(ColorSpace, transferFn, pixelLayout, isLDRFixed)`. The `isLDRFixed` flag drives whether tone curve and quantisation apply.

Caller responsibility examples:

| Use case | TargetFormat | ViewTransform |
|---|---|---|
| Standard sRGB viewport | `RGBA8_sRGB` | `ForLDRDisplay(ev=slider, ACESFilmic)` |
| Mac EDR viewport | `RGBA16F_ExtendedLinearSRGB` | `ForHDRDisplay(ev=slider)` (no tone curve) |
| Save as PNG | `RGBA8_sRGB` | `ForLDRDisplay(ev=baked_in_or_zero)` |
| Save as EXR | `RGBA32F_Linear` | `Identity()` |
| Save as TIFF-16 | `RGBA16_sRGB` | `ForLDRDisplay(...)` |
| Save as .hdr (Radiance) | `RGB32F_Linear` | `Identity()` |

### 3.5 `IFrameEncoder` and `FrameEncoderRegistry`

```cpp
struct EncodeOpts {
    ViewTransform viewTransform = ViewTransform::Identity();
    int compressionLevel = -1;                  // format-specific; -1 = encoder default
    std::map<std::string, std::string> attrs;   // metadata (EXR custom attrs, PNG iTXt)
    bool includeAOVs = false;                   // multichannel formats only
    std::vector<ChannelId> aovChannels;
    unsigned frame = 0;                         // animation frame index
};

class IFrameEncoder {
public:
    virtual ~IFrameEncoder() = default;

    virtual std::string FormatName() const = 0;          // "PNG", "EXR", …
    virtual std::vector<std::string> Extensions() const = 0;  // {".png"}
    virtual bool SupportsHDR()       const = 0;
    virtual bool SupportsAOVs()      const = 0;
    virtual TargetFormat NativeTargetFormat() const = 0; // what the encoder reads via Render

    // Single emit. dst is binary-safe (caller handles file vs. memory stream).
    virtual void Encode(const FrameStore& store,
                        std::ostream&     dst,
                        const EncodeOpts& opts) = 0;
};

class FrameEncoderRegistry {
public:
    static FrameEncoderRegistry& Get();              // singleton, populated by RISE_API_Init

    void Register(std::unique_ptr<IFrameEncoder>);
    IFrameEncoder* ByFormatName(std::string_view) const;   // "PNG"
    IFrameEncoder* ByExtension (std::string_view) const;   // ".png"
    std::vector<IFrameEncoder*> All() const;               // for UI menus
};
```

Concrete encoders (one per existing writer): `PNGFrameEncoder`, `EXRFrameEncoder`, `TIFFFrameEncoder`, `HDRFrameEncoder`, `RGBEAFrameEncoder`, `TGAFrameEncoder`, `PPMFrameEncoder`.

**As implemented (L2)**: each encoder constructs the format-specific `IRasterImageWriter` via `RISE_API_Create*Writer`, optionally wraps it in a `DisplayTransformWriter` for LDR formats with non-trivial exposure / tone-curve, then calls `store.AsBeautyRasterImage().DumpImage(effectiveWriter)`.  `BeautyRasterImageView::DumpImage` walks pixels in the same row-major order and the same `RISEColor` value sequence as `RasterImage_Template::DumpImage`, which is why byte-identity to the legacy `FileRasterizerOutput::WriteImageToFile` pipeline holds for free.  (An earlier draft of this doc had each encoder call `store.Render(...)` into a temporary RGBA buffer; that approach would have required reimplementing every per-format byte serialisation.  Driving the existing writer through the IRasterImage shim sidesteps the duplication entirely.)

This is the one cleanly back-compatible step: existing writers don't change at all — they're called from inside the encoders rather than being the output sinks themselves.

### 3.6 `IRenderObserver`

Replaces `IRasterizerOutput` for *notification*. `IRasterizerOutput` survives in Phase 1 as the data path; in Phase 2 it's retired or kept as a deprecated shim.

**Observers attach to the `FrameStore`, NOT to the rasterizer.** The FrameStore is the persistent artifact; the rasterizer is its current producer. The user changes the active rasterizer in the UI (e.g., PT → BDPT, or "Render" with different settings) → a new rasterizer is constructed against the same FrameStore → all attached observers (scene-declared file outputs, UI viewport) survive the swap and observe the new render's output without reattachment. See §7.5.

```cpp
class IRenderObserver {
public:
    virtual ~IRenderObserver() = default;

    // Called from a rasterizer worker thread after a tile is committed
    // to FrameStore. roi is in pixel coordinates.
    virtual void OnTileComplete(const Rect& roi, uint64_t generation) {}

    // Called from the rasterizer thread after the final tile of a frame.
    // Observers expecting "render done" listen here.
    virtual void OnFrameComplete(unsigned frame, uint64_t generation) {}

    // Denoiser hooks — called only when OIDN is enabled.
    virtual void OnPreDenoiseComplete (unsigned frame, uint64_t generation) {}
    virtual void OnDenoiseComplete    (unsigned frame, uint64_t generation) {}

    // Camera-side EV (today's SetCameraExposureCompensationEV).
    // Observers that bake it into ViewTransform listen here.
    virtual void OnCameraExposureCompensation(Scalar ev) {}
};
```

Crucially, observer callbacks pass *no pixel data* — observers consume `FrameStore` directly. This means:

- The render thread never blocks on observer work.
- Observers can be slow (file write, network send) without back-pressuring the render.
- The seqlock model lets observers and the rasterizer run concurrently with no mutex.

---

## 4. Concurrency model — tile seqlock

Each tile has an atomic sequence counter. Writers bump it odd-then-even around their write; readers retry if the counter changed mid-read.

```cpp
// Inside FrameStore:
struct TileMeta {
    std::atomic<uint64_t> seq{0};   // even = stable; odd = being written
    Rect rect;
};
std::vector<TileMeta>     tiles_;
std::atomic<uint64_t>     globalGen_{0};

void BeginTile(size_t tx, size_t ty) {
    auto& t = tiles_[ty * tileCountX_ + tx];
    t.seq.fetch_add(1, std::memory_order_acq_rel);   // → odd
}

void EndTile(size_t tx, size_t ty) {
    auto& t = tiles_[ty * tileCountX_ + tx];
    t.seq.fetch_add(1, std::memory_order_acq_rel);   // → even
    globalGen_.fetch_add(1, std::memory_order_release);
}

// Reader inside Render():
for each tile in roi {
    uint64_t s1, s2;
    do {
        s1 = tile.seq.load(std::memory_order_acquire);
        if (s1 & 1) { std::this_thread::yield(); continue; }
        copy_tile_pixels_through_transform(tile, dst, fmt, xform);
        s2 = tile.seq.load(std::memory_order_acquire);
    } while (s1 != s2);
}
```

Properties:

- **Writer never blocks on readers.** A repaint spinning on a busy tile yields, but the rasterizer never spins waiting for a reader. The render thread's worst case is reading one extra cache line per tile.
- **Reader retries are rare.** A repaint touches each tile ~once; collision probability ≈ (tile-write-time / repaint-period) per tile. For 32×32 tiles taking microseconds and a 16ms repaint loop, retries are sub-1%.
- **No mutex.** Eliminates the rwlock contention that would otherwise grow with channel count.
- **Memory ordering.** `acq_rel` on `EndTile`'s increment establishes happens-before with the reader's acquire load; reader sees all writes to that tile.

For "snapshot the entire frame for an encoder" (file save), use the same per-tile retry loop. If the rasterizer is mid-render, the encoder gets a *consistent-per-tile* snapshot — some tiles converged, some not, none torn. (Saving mid-render is an explicit feature; use case: "save an in-progress preview to share.")

For Phase 1 only, where `FrameSink::OutputIntermediateImage` does the writes from the rasterizer thread, the same Begin/EndTile bracketing protects the bulk-copy from `IRasterImage` into the FrameStore's beauty channel.

---

## 5. Phase 1 — FrameStore ships, rasterizers unchanged

Goal: ship the new model end-to-end without touching any rasterizer factory or `IRasterizer` implementation. This proves out the FrameStore + encoder + observer machinery against the existing CLI.

### Code added

- `src/Library/Rendering/FrameStore.{h,cpp}` — types from §3.1–3.2.
- `src/Library/Rendering/ViewTransform.{h,cpp}` — types from §3.3.
- `src/Library/Rendering/TargetFormat.{h,cpp}` — enum + format-table from §3.4.
- `src/Library/Rendering/ColorSpace.{h,cpp}` — conversion matrices.
- `src/Library/Rendering/IFrameEncoder.h` — interface from §3.5.
- `src/Library/Rendering/Encoders/{PNG,EXR,TIFF,HDR,RGBEA,TGA,PPM}FrameEncoder.{h,cpp}` — wrappers over existing writers.
- `src/Library/Rendering/FrameEncoderRegistry.{h,cpp}`.
- `src/Library/Interfaces/IRenderObserver.h` — interface from §3.6.
- `src/Library/Rendering/FrameSink.{h,cpp}` — `IRasterizerOutput` impl that copies into FrameStore.
- `src/Library/Rendering/FileEncoderObserver.{h,cpp}` — `IRenderObserver` impl that calls an encoder on `OnFrameComplete`.

### Code modified

- `FrameStore` exposes `AddObserver(IRenderObserver*)` / `RemoveObserver(IRenderObserver*)` / `EnumerateObservers(...)`. The rasterizer base (`Rasterizer.h`) is **not** modified to track observers — observers hang off the artifact, not the producer.
- `FrameSink::OutputIntermediateImage` and `FrameSink::OutputImage` (the legacy `IRasterizerOutput` push entry points) copy tile data into the FrameStore's beauty channel, then fire FrameStore observer callbacks. In Phase 2 this responsibility moves to FrameStore's `EndTile` / `MarkFrameComplete` directly. (§19.8's already-centralized fire-points stay where they are; only the consumer changes.)
- `FileRasterizerOutput` is **rewritten as a thin shim** that internally constructs a `(FrameStore, FrameSink, FileEncoderObserver)` trio from its constructor parameters and registers the `FileEncoderObserver` on the FrameStore (not on the rasterizer). The chunk parser for `file_rasterizeroutput` keeps working unchanged. Public constructor signature unchanged.

### What works after Phase 1

- CLI: `.RISEscene` files using `file_rasterizeroutput` produce byte-identical output (the encoders dispatch to the same writers as before; same `DisplayTransformWriter` math under the hood).
- GUI: can attach `FrameSink` + a viewport-side `IRenderObserver` to any rasterizer, get a persistent HDR FrameStore, and call `frameStore->Render(RGBA8_sRGB, ForLDRDisplay(slider))` on every viewport repaint — live exposure scrubbing works without rasterizer involvement.
- Save-As menu: `FrameEncoderRegistry::Get().All()` populates the format dropdown; selection → `IFrameEncoder::Encode(*frameStore, ofstream, opts)`.
- HDR display (Mac EDR): viewport calls `frameStore->Render(RGBA16F_ExtendedLinearSRGB, ForHDRDisplay(slider))`.

### What's still suboptimal

- Two HDR copies exist: rasterizer's internal `IRasterImage` (kept by `PixelBasedRasterizerHelper::GetLastRenderedImage()`) **and** FrameStore's beauty channel. `FrameSink::OutputIntermediateImage` does a full per-tile copy. This is a memory-bandwidth tax on every render.
- Rasterizers can't write AOV channels into FrameStore — they have nowhere to put normals/albedo today (those live in `AOVBuffers` and feed only OIDN).
- The two notification paths (`IRasterizerOutput` and `IRenderObserver`) coexist; new code uses observers, legacy code uses outputs.

---

## 6. Phase 2 — Rasterizer writes directly to FrameStore

Goal: eliminate the redundant copy and let rasterizers populate AOV channels. ABI break.

### Factory signature change

Every `RISE_API_Create*Rasterizer` factory grows a leading `std::shared_ptr<FrameStore>` parameter (or its C-API equivalent). Example:

```cpp
// Before
RISE_API_BOOL RISE_API_CreatePathTracingRasterizer(
    IRasterizer** ppi,
    /* … 30+ params … */);

// After
RISE_API_BOOL RISE_API_CreatePathTracingRasterizer(
    IRasterizer**                ppi,
    std::shared_ptr<FrameStore>  store,
    /* … same 30+ params … */);
```

The `IRasterizer` abstract interface gets:

```cpp
virtual std::shared_ptr<FrameStore> GetFrameStore() const = 0;
```

This is a vtable change — see [docs/skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md). Plan: bump the API version, change all factories in one merge, audit derived classes for name hiding.

### `PixelBasedRasterizerHelper` rewrite

Internally, the helper currently allocates its own `RISERasterImage` and exposes it via `GetLastRenderedImage()`. Phase 2 replaces this with a `FrameStore*` reference:

```cpp
class PixelBasedRasterizerHelper {
    std::shared_ptr<FrameStore> store_;        // injected at construction
    // ↓ delete this, replaced by store_->GetChannel<RISEPel>(Beauty)
    // RISERasterImage internalImage_;
    …

    void OnTileSplat(size_t tx, size_t ty, /* … */) {
        store_->BeginTile(tx, ty);
        // splat directly into store_->GetChannel<RISEPel>(Beauty) …
        store_->EndTile(tx, ty);
        for (auto* obs : observers_) obs->OnTileComplete(tileRect, store_->Generation());
    }
};
```

`GetLastRenderedImage()` is replaced by `GetFrameStore()`. The `AsBeautyRasterImage()` shim on FrameStore lets call sites that aren't yet migrated keep working.

### `FrameSink` and `IRasterizerOutput` retirement

In Phase 2, `IRasterizerOutput` becomes a deprecated interface; `FileRasterizerOutput` is rewritten to use `IRenderObserver` directly (it stops implementing `IRasterizerOutput`). The existing `IRasterizer::AddRasterizerOutput` API stays for one release as a shim that registers a `FrameSink+FileEncoderObserver` pair, then is removed in the release after.

### AOV writes

Rasterizers that produce AOV data (every PT-style rasterizer for the OIDN denoiser pre-pass) now write into FrameStore's optional channels. `AOVBuffers` either becomes a thin view over `FrameStore` AOV channels or is deleted outright — same data, single home.

This unlocks "save albedo / normal / depth as separate files" or "save multichannel EXR" as a follow-up encoder, with no rasterizer changes.

### What works after Phase 2

- One HDR copy (FrameStore is the rasterizer's accumulation target).
- AOVs are first-class artifacts the user can save.
- `IRasterizerOutput` is gone (or trivially shimmed).
- Live scrubbing, multi-format save, and HDR display all continue to work as in Phase 1.

---

## 7. Backwards compatibility

### 7.1 Scene-file `file_rasterizeroutput` chunks

These are descriptor-driven (CLAUDE.md "Chunk parsers are descriptor-driven"). The chunk parser stays exactly as it is; its `Finalize` method changes to construct a `FileRasterizerOutput` shim that wires up FrameStore + FrameSink + FileEncoderObserver internally. From the user's perspective every existing scene file produces the same bytes.

`exposure_compensation`, `display_transform`, `colorspace`, EXR-specific fields all map onto `EncodeOpts.viewTransform` and the encoder selection.

### 7.2 The DRISE socket sink

Today's `drise_client.cpp` stacks a `Win32WindowRasterizerOutput` and a socket output. Both implement `IRasterizerOutput`. In Phase 1, both keep working unchanged (FrameStore is opt-in, alongside the existing output sinks). In Phase 2, the socket sink is rewritten as an `IRenderObserver` that reads tile rects from FrameStore and ships them.

### 7.3 OIDN denoiser

The denoiser today consumes `IRasterImage` (beauty, albedo AOV, normal AOV). In Phase 1, `FrameStore::AsBeautyRasterImage()` provides the bridge; the AOV side keeps using `AOVBuffers` as today. In Phase 2, the denoiser is moved to read directly from FrameStore AOV channels, and `AOVBuffers` is retired.

### 7.4 `PixelBasedRasterizerHelper::GetLastRenderedImage()`

Phase 1: keeps working (the helper still owns its `RISERasterImage`; the FrameStore is in addition). Phase 2: replaced by `GetFrameStore()`. Callers in the InteractiveEditor viewport bridges update accordingly.

### 7.5 Rasterizer-swap and FrameStore-replacement behavior

The observer-on-FrameStore (rather than observer-on-rasterizer) attachment model means observers survive most lifecycle transitions automatically:

| Transition | FrameStore | Observers | Notes |
|---|---|---|---|
| User clicks Render again, same scene + same rasterizer | reused | reused | Trivial; new frame in same buffer. |
| User picks a different rasterizer (PT → BDPT) in UI | reused | reused | New rasterizer constructed against the existing FrameStore. Scene-declared file outputs and the viewport observer stay attached and emit on the new render's frames. **This is the user-visible benefit of attaching to the artifact rather than the producer.** |
| User changes camera resolution | replaced | re-attached by Job | FrameStore is sized at construction; resolution change → allocate new FrameStore. The Job re-runs the scene-side observer attachment for the new store. The UI viewport observer subscribes on a `Job::OnFrameStoreReplaced` signal. The one transition where observer migration is non-automatic. |
| User loads a different scene | replaced | rebuilt from new scene | Wholly new Job state; new scene's `file_rasterizeroutput` chunks build their own observer set. |
| User adds a temporary render-to-file mid-session ("Save As…" with auto-rerender) | reused | observer added then removed | UI attaches a one-shot `FileEncoderObserver` for the next `OnFrameComplete`, then removes it. |

The *only* lifecycle event that requires re-attaching observers is FrameStore replacement, which is also the only event that genuinely invalidates the buffer's contents anyway. Attaching observers at the Job/scene level (rather than to a transient rasterizer) means rasterizer construction stays cheap and stateless from the observer's point of view.

**Implementation note.** `Job` (or whatever owns the current `FrameStore`) provides:

```cpp
class IJob /* …existing methods… */ {
public:
    virtual std::shared_ptr<FrameStore> GetFrameStore() const = 0;
    using FrameStoreReplacedCallback = std::function<void(std::shared_ptr<FrameStore>)>;
    virtual void OnFrameStoreReplaced(FrameStoreReplacedCallback) = 0;
};
```

The UI viewport binds to `OnFrameStoreReplaced`; on fire, it removes its observer from the old store and registers it on the new one. Scene-declared file observers are reattached by `Job` itself when it builds the new FrameStore (the original observer construction lives in the chunk parser's `Finalize`; on resolution change Job re-invokes the equivalent).

---

## 8. CLI flow — before vs. after

Today (`commandconsole.cpp`):

```
LoadAsciiScene(scene)
  → parser builds: scene + rasterizer + N×FileRasterizerOutput sinks
  → parser calls rasterizer.AddRasterizerOutput(eachSink)
REPL: "render"
  → rasterizer.RasterizeScene(scene, …)
  → on each tile: each sink.OutputIntermediateImage(tile)
  → on completion: each sink.OutputImage(image) → writes file
```

After Phase 1:

```
LoadAsciiScene(scene)
  → parser builds: FrameStore + scene + rasterizer + FrameSink
                   + N×FileEncoderObserver
  → parser calls: rasterizer.AddRasterizerOutput(frameSink)         // legacy push (rasterizer-side)
                  frameStore.AddObserver(eachFileEncoderObserver)    // observers live on FrameStore
                  frameStore.AddObserver(eachWindowObserver)         // GUI viewport
REPL: "render"
  → rasterizer.RasterizeScene(scene, …)
  → on each tile: frameSink.OutputIntermediateImage → store.CopyTileFromRasterImage
                  store.EndTile bumps generation
                  store fires OnTileComplete to observers (no data; viewport repaints)
  → on completion: store.MarkFrameComplete(frame)
                  store fires OnFrameComplete to observers
                  FileEncoderObservers call IFrameEncoder::Encode → write file
```

After Phase 2:

```
LoadAsciiScene(scene)
  → parser builds: FrameStore + scene + rasterizer(store) + N×FileEncoderObserver
  → parser calls: frameStore.AddObserver(eachFileEncoderObserver)
                  frameStore.AddObserver(eachWindowObserver)
REPL: "render"
  → rasterizer.RasterizeScene(scene, …)
  → on each tile: rasterizer writes directly into store.Beauty (and AOV channels)
                  store.EndTile bumps generation, fires OnTileComplete to observers
  → on completion: store.MarkFrameComplete(frame), fires OnFrameComplete
                  FileEncoderObservers encode + write
```

User swaps rasterizer in UI (PT → BDPT) — illustrating §7.5:

```
GUI: user picks BDPT
  → job.ConstructRasterizer("bdpt_pel_rasterizer", existingFrameStore)  // same store
  → previousRasterizer destroyed
  → frameStore observer list UNCHANGED (file outputs + viewport still attached)
  → next Render: BDPT writes into same store, same observers fire
```

---

## 9. GUI flow — before vs. after, with HDR display

Today's GUI (per `INTERACTIVE_EDITOR_PLAN.md`):

```
RenderEngine constructs ImageOutputAdapter : IJobRasterizerOutput
  - rasterizer pushes RGBA16 (uint16) frames to adapter
  - adapter converts to RGBA8 std::vector<uint8_t>
  - emits Qt signal → QImage → painted to screen
HDR signal is lost in the RGBA16-uint16 conversion.
Save As: not implemented (would require a second rasterizer pass).
```

After Phase 1:

```
RenderEngine creates: FrameStore (size from camera)
                      Rasterizer + FrameSink (legacy push) + ViewportObserver
ViewportObserver::OnTileComplete(roi, gen):
    queue Qt signal "frame dirty(gen)"

UI thread on display refresh (CADisplayLink / QTimer):
    if storeGen > lastPaintedGen:
        TargetFormat fmt = displayIsEDR ? RGBA16F_ExtendedLinearSRGB : RGBA8_sRGB
        ViewTransform xf = displayIsEDR ? ForHDRDisplay(slider) : ForLDRDisplay(slider, ACES)
        store->Render(layerBuffer, layerStride, viewportRect, fmt, xf)
        lastPaintedGen = storeGen
        layer.commit()

User drags exposure slider:
    just kicks the next display refresh — no rasterizer involvement.

User clicks "Save As PNG…":
    enc = FrameEncoderRegistry::ByExtension(".png")
    EncodeOpts opts{ .viewTransform = ForLDRDisplay(currentSliderEV, ACES) }
    enc->Encode(*store, ofstream(path), opts)

User clicks "Save As EXR…":
    enc = FrameEncoderRegistry::ByExtension(".exr")
    EncodeOpts opts{ .viewTransform = Identity() }   // scene-referred linear
    enc->Encode(*store, ofstream(path), opts)
```

Phase 2 is identical at the GUI level — the GUI was already pulling from FrameStore.

### Mac EDR specifics

- `CAMetalLayer.wantsExtendedDynamicRangeContent = YES`
- `CAMetalLayer.colorspace = kCGColorSpaceExtendedLinearSRGB`
- `CAMetalLayer.pixelFormat = MTLPixelFormatRGBA16Float`
- Render call: `frameStore.Render(metalTextureMappedBuffer, stride, roi, RGBA16F_ExtendedLinearSRGB, ForHDRDisplay(ev))`
- Values in the output buffer can legitimately be > 1.0; OS handles tone-mapping to display dynamic range.
- Headroom detection: query `NSScreen.maximumExtendedDynamicRangeColorComponentValue` on each window-screen-change; if 1.0, drop back to `RGBA8_sRGB` automatically.

### Windows HDR specifics

Windows offers two HDR display paths; both compose with our existing `TargetFormat` enum:

**Path A — scRGB linear (the EDR analogue, recommended default)**

- DXGI swap chain: `DXGI_FORMAT_R16G16B16A16_FLOAT`
- `IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)` — linear scRGB; values in the swap chain are linear, scaled so 1.0 = 80 nits (sRGB white), and can extend past 1.0 (up to ~125 for full HDR10 1000-nit peak).
- Render call: `frameStore.Render(swapChainBackBuffer, stride, roi, RGBA16F_ExtendedLinearSRGB, ForHDRDisplay(ev))` — **same `TargetFormat` and `ViewTransform` as Mac EDR**; the only platform-specific code is the DXGI swap-chain setup.
- Values in `RGBA16F_ExtendedLinearSRGB` are scene-referred in linear sRGB primaries; the OS compositor scales by 80 nits and applies any display-side tone mapping. Identical buffer math to Mac EDR.

**Path B — HDR10 / PQ (for direct-to-display PQ encoding, e.g., HDR10-aware video file targets)**

- DXGI swap chain: `DXGI_FORMAT_R10G10B10A2_UNORM` (10-bit) or `DXGI_FORMAT_R16G16B16A16_FLOAT`
- `IDXGISwapChain3::SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)` — BT.2020 primaries, PQ (SMPTE ST.2084) transfer.
- Render call: `frameStore.Render(swapChainBackBuffer, stride, roi, RGBA16F_BT2020_PQ, ForHDRDisplay(ev))` — values are PQ-encoded; reference white is 10000 nits at 1.0, app is responsible for mapping scene radiance to a target peak (default: 1000 nits).
- Use case: HDR10-aware file export (future AVIF HDR / HEIC HDR encoder), TVs in HDR10 mode, scenarios where the app dictates absolute luminance levels.
- Most desktop usage prefers Path A — let the compositor manage tone mapping rather than baking PQ encoding into the output.

**Headroom detection.** Query `IDXGIOutput6::GetDesc1` for `MaxLuminance`/`MinLuminance` and `ColorSpace`. If the active monitor returns SDR colorspace, fall back to `RGBA8_sRGB`. Re-query on `WM_DISPLAYCHANGE` and on window-monitor changes.

### Linux / Android

- Standard sRGB display: `RGBA8_sRGB` + `ForLDRDisplay(ev, ACESFilmic)`.
- Linux HDR (Wayland color-management protocol, KMS HDR): out of scope for this redesign; revisit when the Wayland protocol stabilises.
- Android: HDR display via `Display.HdrCapabilities` + a `SurfaceView` with `Display.HDR_TYPE_HDR10` works through the same `RGBA16F_BT2020_PQ` path; defer initial wiring until a user asks.

---

## 10. ABI changes summary

| Item | Phase | Change kind | Migration |
|---|---|---|---|
| `FrameStore`, `Channel<T>`, `ViewTransform`, `TargetFormat`, `ColorSpace`, `IFrameEncoder`, `FrameEncoderRegistry`, `IRenderObserver` | 1 | New types, additive | None — opt-in |
| `FrameStore::AddObserver/RemoveObserver/EnumerateObservers` | 1 | New methods on a new type | None (FrameStore is new). Note: observers attach to FrameStore, **not** to `IRasterizer` — see §7.5 |
| `IJob::GetFrameStore() / OnFrameStoreReplaced(callback)` | 1 | New virtuals (vtable grow) | Audit `IJob` derived classes for name collisions per [skills/abi-preserving-api-evolution.md](skills/abi-preserving-api-evolution.md) |
| `FileRasterizerOutput` constructor signature | 1 | Unchanged externally; reimplemented internally | None |
| `RISE_API_Create*Rasterizer` factories | 2 | Add leading `shared_ptr<FrameStore>` param | All callers updated; bump `RISE_API` version |
| `IRasterizer::GetFrameStore()` | 2 | New virtual | Audit derived classes |
| `PixelBasedRasterizerHelper::GetLastRenderedImage()` | 2 | Removed; replaced by `GetFrameStore()` | Update viewport bridges |
| `IRasterizerOutput` | 2 | Deprecated; removed in 2+1 | Migrate sinks to `IRenderObserver` |
| `AOVBuffers` | 2 | Removed (or thin shim over FrameStore) | Denoiser migrates to FrameStore channels |

---

## 11. Recommended landing order

Status legend: ✅ shipped, ⏳ pending.

| Landing | Status | Deliverable | Verification |
|---|---|---|---|
| **L0** | ✅ | `ViewTransform` + `TargetFormat` + `FSColorSpace` + transfer functions + half-float (binary16) encode/decode + unit tests | 303-assertion test: per-target round-trip, NaN/Inf bit preservation, 65k-pattern half sweep, primaries vs ColorUtils parity |
| **L1** | ✅ (3 review rounds) | `FrameStore` + `Channel<T>` + per-tile `std::shared_mutex` + `Render(...)` + `IRenderObserver` + observer-on-FrameStore attachment + IRasterImage shim | 83-assertion test: construction, AOV allocation, observer self-detach safety, observer cross-thread `RemoveObserver` waits for in-flight, observer cascade-removal no-UAF, 2-reader concurrent stress, HDR archival identity (Inf/NaN/negative preservation), Reference lifetime |
| **L2** | ✅ | `IFrameEncoder` + `FrameEncoderRegistry` + 7 concrete encoders (PNG, EXR, TIFF, HDR, RGBEA, TGA, PPM) wrapping existing writers; `BeautyRasterImageView::DumpImage` walks pixels in legacy-matching row-major order | 69-assertion test: byte-identical regression to legacy `FileRasterizerOutput::WriteImageToFile` across all 7 formats with various opts (LDR with/without tone curve, HDR variants, ROMM primaries, edge dimensions 1×1 / 17×16 / 3×64, non-1.0 alpha, cameraExposureEV summing, registry lookup case-insensitive + by-extension) |
| **L3** | ✅ | `FrameSink` + `FileEncoderObserver` + `FileRasterizerOutput` shim that routes to FrameStore + IFrameEncoder under the hood | 60-assertion test: byte-identical to L2 IFrameEncoder for all 7 formats × multiple opt combinations, denoise dual-write (PreDenoise → "<pattern>.png" + Denoise → "<pattern>_denoised.png"), animation `bMultiple=true` frame-numbered output, multi-frame reuse (FrameStore correctly refilled across calls), camera+static EV summing, HDR cameraEV-zeroing.  Manual smoke: `scenes/Tests/Geometry/shapes.RISEscene` runs end-to-end through the new pipeline and produces valid PNG + denoised-PNG output. |
| **L4a** | ✅ | Platform-agnostic `ViewportFrameStore`: facade owning (FrameStore + FrameSink + observer) with Attach/Detach/RenderToBuffer/SaveAs API + tile/frame/preDenoise/denoise callbacks for platforms to wire up.  L1 `BeautyRasterImageView::DumpImage` hardened to acquire all per-tile shared_locks (mid-render Save-As is now data-race-free). | 43-assertion test: lazy chain alloc, callbacks fire on the right events with the right (frame, generation), `RenderToBuffer` reads correctly + respects exposure, `SaveAs` byte-identical to L2 IFrameEncoder direct path, rasterizer-swap simulation, resolution-change reallocation, multi-frame reuse, mid-render concurrent SaveAs (writer + reader threads) produces non-empty files, cameraExposureEV propagates through Meta() and survives reallocation. |
| **L4b/c/d** | ✅ shipped (4 review rounds) | 53 (L4a regression incl. region-bounded RenderToBuffer guard) | GUI viewport wiring per platform — production-render path migrated on all three: macOS SwiftUI `RISEBridge.mm` replaces `BlockRasterizerOutput` with VFS + RGBA16_sRGB read-back into a `ViewportFrameStoreCallbacks` helper that fires the existing `RISEImageOutputBlock`; Windows Qt `RenderEngine.{cpp,h}` replaces `ImageOutputAdapter` with VFS + RGBA8_sRGB direct into `m_pixelBuffer` + `imageUpdated()` Qt signal; Android `RiseBridge.{cpp,h}` + `RiseCallbacks.{cpp,h}` + `rise_jni.cpp` replaces `RasterizerOutputAdapter` with VFS + RGBA8_sRGB direct into `m_framebuffer` + `onRegionInvalidated()` JNI callback.  Each platform adds `setViewExposureEV(ev)` (live exposure scrubbing without re-render) and `saveAs(path, format, ev)` (multi-format Save-As via L2 IFrameEncoder).  Interactive-viewport `ViewportPreviewSink` (macOS / Android) deferred to a follow-up landing — this landing covers the production-render path only.  **Note**: Xcode RISE-GUI target now requires `gnu++17` (`std::shared_mutex` is C++17); was `compiler-default` = C++14.  Project file bumped. |
| **L5a** | ✅ shipped (2 review rounds) | manual EDR-capable Mac smoke | GUI: Mac EDR display path (`RGBA16F_ExtendedLinearSRGB` via CAMetalLayer + MTKView render pipeline).  Bridge: new `RISEHDRImageOutputBlock` typedef (binary16 / extended-linear-sRGB), `-setHDRImageOutputBlock:`, `-setHDREnabled:`, `-displayMaxEDRHeadroom`.  ViewportFrameStoreCallbacks helper now selects `RGBA16F_ExtendedLinearSRGB + ForHDRDisplay(ev)` vs `RGBA16_sRGB + ForLDRDisplay(ev)` per the HDR toggle; both are 8 bpp so the same staging buffer fits both modes.  Swift: new `MetalEDRView` (NSViewRepresentable wrapping MTKView) + durable `MetalEDRRenderer` (owned by RenderViewModel; binds the HDR block exactly once for the model's lifetime to avoid SwiftUI Coordinator-resurrection races).  Render pipeline samples the source MTLTexture with bilinear filtering + aspect-fit letterboxing in a fullscreen-quad fragment shader (no `drawableSize` mutation; preserves Retina sharpness).  EDR availability probe queries `window.screen.maximumExtendedDynamicRangeColorComponentValue` (not `mainScreen` which follows keyboard focus); subscribes to `NSWindow.didChangeScreenNotification` so the toggle dims the moment the user drags the window between EDR-capable and SDR monitors.  ContentView toggle disabled when `!edrAvailable`; interactive editor pointer events suspended while EDR preview is on.  See "Review rounds completed" below for L5a round-1 + round-2 review summaries. |
| **L5b** | ⏳ | GUI: Windows HDR display path (scRGB via DXGI swap chain) | Manual: same scene on HDR10-capable Windows monitor; verify HDR headroom detection on `WM_DISPLAYCHANGE` |
| **L5c** | ⏳ (optional, follow-up) | PQ encoding path for HDR-aware file export | Encoder-level test: round-trip a known HDR pattern through PQ encode/decode |
| **L6** | ⏳ | Phase 2: rasterizer factories take `FrameStore`; `PixelBasedRasterizerHelper` writes directly | Performance test: per-tile copy eliminated; render walltime ≥ pre-redesign |
| **L7** | ⏳ | AOV channels populated by rasterizers; multichannel-EXR encoder | New `.RISEscene` test scenes that emit albedo/normal/depth |
| **L8** | ⏳ | `IRasterizerOutput` removed; `AOVBuffers` removed | Build + tests pass with old paths gone |

L0-L4 are Phase 1 (B); L6+ is Phase 2 (C). L5a / L5b (HDR display on Mac and Windows) are Phase 1 + small platform-specific additions; they can ship in parallel with each other and do not depend on L6. L5c (PQ encoding) is a follow-up encoder, not a display path.

Adversarial review at L3 (correctness gate — CLI byte-identical), L4 (UX gate, rasterizer-swap behavior), L5a/b (HDR-display correctness), L6 (perf + ABI gate).

### Review rounds completed

- **L0 review**: 1 MEDIUM (Display P3 row 2 transcription typo, ~5e-7 off; fixed) + 5 LOWs.
- **L1 review round 1**: 6 HIGHs (lambda return-type inconsistency in half-float encode; cascade ordering; observer mutex deadlock on self-detach; BeautyRasterImageView Reference + unique_ptr lifetime mismatch; presence_ array set on default switch case; reader/writer fence on ARM64) + several MEDs/LOWs. All fixed.
- **L1 review round 2**: 3 P1/P2/P3 (atomic-seqlock UB on non-atomic pixel storage → switched to `std::shared_mutex`; observer dispatch cross-thread UAF → in-flight counter + cv wait; `Linear` transfer not actually identity → fixed). All fixed.
- **L1 review round 3**: 1 P2 (same-thread cascade-removal UAF in observer dispatch → per-iteration recheck against `observers_`). Fixed.
- **L2 review**: 2 HIGHs (alpha precision narrowing float vs Chel → switched to Chel; `cameraExposureEV` not consumed by encoder → encoder pulls from `Meta()` and sums). 7 MEDIUMs (HDR-format warn-on-misuse not mirrored; default colorSpace=sRGB suspicious for HDR formats; test gaps around ROMM, edge dimensions, exposure-only HDR). HIGH-1 + HIGH-2 + test gaps fixed; doc cleanups deferred (low-impact).
- **L3 review**: 1 HIGH (unknown FRO_TYPE silently dropped — legacy fell back to PPM via `default:` switch case; new `FormatNameForType` returned `"PNG"` and registry-miss path silently no-op'd. Fixed: changed default to `"PPM"` to match legacy bit-for-bit), 4 MEDIUMs (M1: Output\*-vs-dtor race assumption now explicitly documented in dtor; M2: pre-denoise dual write inherited from legacy, documented; M3: test gaps for `OutputPreDenoisedImage`/`OutputDenoisedImage` suffix routing, `bMultiple=true` animation, multi-frame reuse — all closed with three new test functions; M4: silent dim-change on resolution swap → now logs warning + reallocates the chain). 7 LOWs all clean or doc-only (camera EV thread contract: documented).
- **L4a review round 1**: 1 HIGH (mid-render `SaveAs` was a documented L1 data race because `BeautyRasterImageView::DumpImage` didn't acquire per-tile locks → Fixed at the L1 layer: DumpImage now acquires every per-tile shared_lock for the duration of the row-major walk; encoders are uniformly mid-render-safe across L2/L3/L4. Byte-identity preserved because the WriteColor sequence is unchanged), 3 MEDIUMs (M1: `Detach()` is structurally a no-op due to IRasterizer's all-or-nothing `FreeRasterizerOutputs` API; documented in header; rasterizer-swap remains safe under the single-active-rasterizer contract; M2: callback std::function reads from rasterizer thread are racy if setters run mid-render — documented as "set at construction"; M3: resolution-change reentrance assumes single rasterizer thread — documented). 6 LOWs (LOW-2 generation-bound tightened; rest clean or doc-only). Plus 3 test gaps closed: mid-render concurrent SaveAs (writer + reader thread loop, 200ms duration). Test gaps left open: real-rasterizer Attach (deferred — would need a stub rasterizer fixture), multi-threaded callback dispatch (covered transitively by mid-render SaveAs).
- **L4a review round 2**: 2 P1s.  **P1-1**: `OutputIntermediateImage` was forwarding to `FrameSink::OutputIntermediateImage` which is a documented no-op (file outputs ignore intermediate updates).  But the rasterizer drives `OutputIntermediateImage` per tile during render (PixelBasedRasterizerHelper.cpp:302 et al.), so a GUI wired to ViewportFrameStore would have seen NO progressive tile updates and NO `OnTileComplete` callbacks until the final `OutputImage`.  Fix: `ViewportFrameStore::OutputIntermediateImage` now bypasses FrameSink and copies the affected region's tiles directly into the FrameStore via per-tile `BeginTile`/`EndTile` (which fires the observer's `OnTileComplete` callbacks).  Two new tests cover full-region + multi-tile-region intermediate flow.  FrameSink keeps its file-output-matching no-op semantics.  **P1-2**: reader paths read `framestore_` as a raw pointer; rasterizer thread reallocates and frees that pointer in `EnsureChain` on resolution change.  A UI repaint or save racing the rasterizer's first frame at a new resolution could dereference freed memory.  Fix: facade-level `std::shared_mutex chainMutex_`; readers (RenderToBuffer / SaveAs / SaveTo / Generation / SetCameraExposureCompensationEV) take a shared_lock briefly, addref the FrameStore into a local snapshot, release the lock, then operate against the snapshot.  EnsureChain's slow-path (realloc) takes unique_lock.  Reader's addref keeps the snapshot alive even if the writer reallocates — the snapshot may be the OLD chain after a swap, but it's never freed memory.  New stress test: writer thread alternates 16x16 and 32x32 OutputImage (forcing repeated reallocation) while reader thread hammers RenderToBuffer + Generation; runs 200ms with hundreds of concurrent reallocs vs reads, no crashes.
- **L4a review round 3**: 1 P2.  `ViewportFrameStore::OutputIntermediateImage` was computing the affected FrameStore-tile range with half-open ceiling division `(pRegion->right + tileEdge - 1) / tileEdge`, but RISE's `Rect` convention at the IRasterizerOutput boundary is INCLUSIVE — `top/left/bottom/right` are pixel indices, the rect covers `[top, bottom] × [left, right]` (closed interval).  Reference: `PixelBasedRasterizerHelper::BoundsFromRect` and the `<= rect.bottom/right` loops in `PixelBasedRasterizerHelper.cpp`.  The mismatch silently dropped single-pixel tile-aligned regions: `Rect(32, 32, 32, 32)` (a single pixel at (32, 32) sitting exactly on a tile boundary in a 64×64 store with `tileEdge=32`) computed `tx0 == tx1 == 1` and fired ZERO tile callbacks.  Fix: convert `pRegion->{bottom, right} + 1u`, clamped to `srcH`/`srcW`, before the half-open ceiling math; nullptr region keeps the "whole image" semantics from `IRasterizerOutput.h:33-36`.  ~10 lines plus a 14-line comment block citing the convention reference.  Test updates: `TestCallbacks` and `TestIntermediateMultiTile` now build inclusive Rects (`Rect(0, 0, kImgH - 1, kImgW - 1)`, `Rect(0, 0, 31, 63)`); new boundary regression test fires `Rect(32, 32, 32, 32)` and asserts exactly one tile callback.  All 49 ViewportFrameStoreTest assertions pass; full 94-test suite green.  Other `Rect` users in the test file (`RenderToBuffer`'s `roi`) remain half-open because `FrameStore::Render` consumes them as half-open internally — the dual convention now lives at a documented boundary (IRasterizerOutput entry → inclusive; FrameStore internal API → half-open) rather than silently mismatching.
- **L4b/c/d review round 4**: 2 P1s + 4 P2s landed; 1 P1 + 4 P3s deferred / documented.
  - **P1-A (Qt thread join)**: `~RenderEngine()` previously called `m_workerThread->quit()/wait()` but `m_workerThread` was declared and never assigned — every render path created a *local* `QThread* thread = QThread::create(...)`.  Dtor `wait()` was a no-op; an engine destroyed mid-render would race the QThread's `Rasterize()` call against `m_job->release()` and the VFS `this`-capture lambdas against engine destruction.  Fix: track render thread in `m_renderThread` (mutex-guarded) at every QThread::create site; new `waitForRenderToFinish()` method joins it.  Dtor + `loadScene` + `clearScene` set `m_cancelFlag = true` then call `waitForRenderToFinish()` before touching the rasterizer or VFS state.  Each render thread also auto-clears `m_renderThread` on `QThread::finished` via a `Qt::DirectConnection` signal so a stale pointer doesn't outlive the thread.
  - **P1-B (Pixel-byte equivalence claim)**: comments in macOS `EmitFullImage_locked` claimed `RGBA16_sRGB` is "byte-equivalent to legacy `Integerize<sRGBPel,unsigned short>(65535)` at EV=0".  False: VFS's `Q16` (FrameStore.cpp:653-658) uses round-to-nearest (`v + 0.5`) while legacy `Integerize` (Color_Template.h:118-122) uses C-cast truncation.  Difference is ±1 LSB on values that fall on a half-step boundary.  The L2 byte-identical regression test passes because it exercises the *file-encoder* path (which still goes through Integerize truncate via `BeautyRasterImageView::DumpImage`), not `RenderToBuffer`.  Fix: corrected the misleading comments on macOS + Qt to accurately describe the ±1-LSB drift and note that exact byte-identity to legacy live preview would require flipping `Q8`/`Q16` to truncation in a follow-up.  Visual impact is imperceptible (sub-quantum on most displays) so deferred as a doc-only correction.
  - **P2-A/B (Defensive callback nulling)**: VFS callback lambdas capture `this` (Qt/Android) or helper+vfs raw pointers (macOS).  The brief argued `m_job->release()` joins workers before VFS release, draining observer dispatch — true in the typical case via the L1 in-flight-counter / cv-wait machinery.  But defense-in-depth costs nothing: each platform's teardown path now nulls all four VFS callbacks (`SetTileCompleteCallback(nullptr)` etc.) before `vfs->release()`, so any future late-fire (e.g. due to a bug) gracefully no-ops instead of dereferencing freed memory.
  - **P2-C (macOS `setImageOutputBlock:` race)**: `_vfsCallbacks` was lazily constructed inside `-ensureVFSAttachedToRasterizer:` on the rasterize-spawning thread.  Concurrent `setImageOutputBlock:` from the UI thread raced the unique_ptr load against the lazy write.  Fix: eagerly construct `_vfsCallbacks` in `-init` (cheap — helper just owns a buffer that's allocated on first emit + an atomic EV).  `setImageOutputBlock:` now unconditionally forwards to `_vfsCallbacks->SetBlock(...)` which locks `bufferMutex_`.  Bonus: `-ensureVFSAttachedToRasterizer:` no longer needs the inner `if (!_vfsCallbacks)` guard — observer slots are bound once at VFS-construction time, before any Attach.
  - **P2-D (Chain-mutex race on dim read)**: Qt's `onVFSTileComplete` and Android's `onVFSTileComplete` read `Width()/Height()` directly off `vfs->GetFrameStore()` — but `GetFrameStore()` is a raw chain-pointer read with no lock.  A concurrent rasterizer-thread `EnsureChain` reallocation can free the pointer between the `GetFrameStore()` read and the `Width()` deref.  This is the exact race that L4 round-2 P1-2's `chainMutex_` snapshot pattern was added to close, but the new bridges bypassed it.  Fix: added `ViewportFrameStore::GetDimensions(unsigned& w, unsigned& h)` that uses the existing `SnapshotFrameStore` helper (chain-mutex shared lock + addref + release pattern) to read dims safely.  All three platforms now call `vfs->GetDimensions(W, H)` instead of `vfs->GetFrameStore()->Width()/Height()`.  New regression test `TestLazyAllocation` checks that `GetDimensions` returns (0,0) before chain alloc and the correct dims after — bumped ViewportFrameStoreTest from 49 to 51 assertions.
  - **Deferred (P1)**: `Rasterizer::FreeRasterizerOutputs()` is not synchronized with the worker thread's iteration of the `outs` list (`PixelBasedRasterizerHelper.cpp:303-330`).  Pre-existing latent issue, not introduced by L4b/c/d, but the new pattern (persistent VFS across renders, `FreeRasterizerOutputs() + Attach()` in every rasterize) makes it slightly more visible.  The library-side fix is to add a mutex around `outs` in `Rasterizer.{h,cpp}`; out-of-scope for L4b/c/d.  Spawned as a follow-up task.
  - **Deferred (P3)**: `EncodeOpts.bpp = 8` unconditionally on all three platforms' `saveAs` (so 16bpc-PNG / 16bpc-TIFF aren't reachable through this surface — only via the legacy `file_rasterizeroutput` chunk in scene files).  Defer to a follow-up that exposes per-format options on the platform `saveAs` API.
  - **Deferred (P3)**: Qt `LogPrinterAdapter` is leaked at engine destruction (engine retains a Reference but never releases it) — pre-existing bug, fix in a separate landing.
  - **Deferred (P3)**: Android viewport-preview sink (still using `>> 8` truncation in legacy `writeDirtyRegion`) and the new VFS-driven production-render sink (using round-to-nearest) produce visually different bytes for the same input.  Out-of-scope for this landing; the viewport sink migration is a follow-up that will unify both paths through VFS.
- **L5a review round 1**: 3 P1s + 4 P2s + 2 P3s identified by the L5a adversarial pass.  P1s + the easy P2s landed; the rest documented as in-place comments or deferred to a follow-up.
  - **P1-1 (Coordinator resurrection)**: SwiftUI may build a NEW MetalEDRView wrapper before the old one's `dismantleNSView` runs, leading to a sequence where old-Coordinator-A binds the HDR block, new-Coordinator-B binds (replacing A), then A's late dismantle clears B's binding — frames go missing until the LDR path resumes.  **Fix**: introduce `MetalEDRRenderer` as the durable, RenderViewModel-owned holder of Metal state + the bridge HDR-block binding.  SwiftUI's MetalEDRView wrapper just borrows the renderer as its Coordinator (`makeCoordinator() returns renderer`).  The renderer binds the HDR block ONCE in its initialiser and unbinds in its `deinit`; SwiftUI rebuilds of the wrapper struct don't touch the binding.  `attach(view:)`/`detach()` only update a weak `view` reference for the `setNeedsDisplay` callback; the bridge binding is invariant across rebuilds.
  - **P1-2 (drawableSize race + Retina blur)**: original code called `view.drawableSize = CGSize(width: srcW, height: srcH)` from `upload()`'s `DispatchQueue.main.async`, then ran an `MTLBlitCommandEncoder.copy` from texture → drawable.  Two real bugs: (a) AppKit's window-resize handler can clobber `drawableSize` between our setter and our `setNeedsDisplay`, leaving the blit copying mismatched dims; (b) on Retina, forcing `drawableSize` to (often-smaller) source dims means the layer up-samples 2-3× via `magnificationFilter`, producing a visibly blurry preview.  **Fix**: replace the blit with a fullscreen-quad render-pipeline pass (vertex stage emits a [-1, 1]² triangle strip + UVs scaled for aspect-fit letterboxing; fragment stage samples the source MTLTexture with linear filtering).  `drawableSize` is no longer mutated — MTKView picks `bounds.size × backingScaleFactor` natively, so Retina sharpness is preserved and the OS-driven resize doesn't fight us.  The shader is built inline via `device.makeLibrary(source:)` so there's no separate `.metal` file to add to the project.
  - **P1-3 (mainScreen vs window.screen)**: `[NSScreen mainScreen]` follows keyboard focus, NOT the window the RISE app is rendering into.  On a multi-display setup with an EDR-capable laptop + an external SDR monitor, the user could drag RISE to the SDR monitor, click another app on the laptop, and have `mainScreen` report EDR-capable headroom while RISE is rendering on the SDR display.  Result: highlights silently SDR-clip.  Also: `NSApplication.didChangeScreenParametersNotification` fires only when the screen *configuration* changes (resolution, attach/detach), not when the window crosses a screen boundary.  **Fix**: RenderViewModel now tracks the host `NSWindow` (set from `RISEApp`'s `.onAppear`) and probes `window.screen.maximumExtendedDynamicRangeColorComponentValue` directly.  Subscribes to `NSWindow.didChangeScreenNotification` (per-window) in addition to the app-level screen-config change.  `bridge.displayMaxEDRHeadroom()` (which still reads `mainScreen`) is kept as a fallback for the pre-window-attach moment at app launch.
  - **P2-2 (CGColorSpace force-unwrap)**: `CGColorSpace(name:)` returns optional and silently nil in unusual configs.  Setting nil on `MTKView.colorspace` falls back to deviceRGB → SDR-clip.  **Fix**: guard the constructor; log a warning and skip the colorspace setter on failure (the layer's `wantsExtendedDynamicRangeContent` stays true so EDR composition still attempts, just in deviceRGB primaries — best-effort fallback).
  - **P2-3 (texture usage flag)**: original `desc.usage = [.shaderRead]` was technically correct for the blit-only path but flagged by Apple's Metal Validation Layer as restrictive.  **Fix**: `desc.usage = [.shaderRead, .renderTarget]` — covers the new fragment-shader sample path and any future render-into-texture use.
  - **P2-5 (redundant layer.pixelFormat)**: original code wrote both `MTKView.colorPixelFormat` AND `layer.pixelFormat`.  MTKView writes through to the layer; the redundant layer-side write can race the first drawable acquire on some macOS versions.  **Fix**: dropped the layer-side write, kept only `MTKView.colorPixelFormat = .rgba16Float`.
  - **Deferred / documented**: P2-1 (lock-across-block deadlock landmine — current Swift block uses `DispatchQueue.main.async`, so no actual deadlock; documented via comment); P2-4 (window-level `NSColorSpace` not configured — relies on layer-only EDR opt-in, which works on macOS 11+ EDR-capable hardware; documented as a known fragility on edge configurations); P3-1 (LDR closure still attached during EDR preview — wasted main-thread work, no correctness impact); P3-2 (`[weak self]` capture invariant — documented in code).
  - **Swift 6 strict concurrency**: surfaced during build verification.  `MetalEDRRenderer` was originally `@MainActor` (the Xcode RISE-GUI module defaults to MainActor isolation), but `upload()` runs on rasterizer worker threads and accesses `texture` / `textureWidth` / `textureHeight` — main-actor-isolated members can't be touched from non-isolated contexts.  **Fix**: dropped `@MainActor` from the class; marked `@unchecked Sendable` since the `textureLock` (NSLock) provides actual cross-thread invariant.  `view` is weak and only touched from main-thread paths (attach / detach / dispatch_async-to-main); `bridge` is read-only after init.  Documented the deliberate concurrency model in the class doc-comment so a maintainer can't accidentally re-add `@MainActor`.
- **L5a review round 2**: 2 P2s — both real correctness races flagged after the round-1 patches landed.
  - **P2-A (HDR mode-snapshot race)**: `EmitRegion_locked` and `EmitFullImage_locked` originally read `hdrEnabled_.load()` TWICE per emit — once inside `SelectTargetFormatAndXform` to choose the TargetFormat for `RenderToBuffer`, then again inside `FireBlock_locked` to choose which Swift block (LDR vs HDR) to invoke.  `setHDREnabled:` mutates `hdrEnabled_` atomically WITHOUT taking `bufferMutex_`, so a UI toggle landing between the encode and the dispatch could deliver bytes encoded in one format through the callback expecting the other — `RGBA16_sRGB` uint16 bytes re-interpreted as half-floats by the Metal path, or vice versa.  **Fix**: snapshot `useHDR` (and `ev`) ONCE at the start of each emit and pass them to BOTH `SelectTargetFormatAndXform(useHDR, ev, ...)` and `FireBlock_locked(useHDR, ...)`.  Eliminates the window without needing `setHDREnabled` to participate in `bufferMutex_`.  No change to `setHDREnabled`'s API.
  - **P2-B (GPU-CPU MTLTexture race)**: `MetalEDRRenderer.upload()` called `texture.replace(region:..)` from rasterizer worker threads on the SAME `.shared`-storage MTLTexture that `draw(in:)` was sampling via the render pipeline.  Apple's contract: for `.shared` storage, CPU writes done after a `cmd.commit()` but before the GPU has finished sampling are racy — the in-flight render pass can see torn pixels (mixed old/new half-float data, especially visible in regions where one tile completed during the GPU sample of a neighbouring tile).  The previous design's `NSLock` only guaranteed Swift-side coherence, not CPU-vs-GPU memory coherence.  **Fix**: rearchitected as **CPU staging buffer + display texture + frame semaphore**.  Worker threads write region updates into a CPU-side `[UInt16]` (`stagingBuffer`) under `stagingLock`.  `draw(in:)` is the SINGLE writer of the GPU's `displayTexture`: under `inflightSem.wait()`, it locks `stagingLock`, full-image-`replace`s the displayTexture from `stagingBuffer`, releases `stagingLock`, encodes the render pass, and adds a `cmd.addCompletedHandler` that signals `inflightSem` after GPU sampling finishes.  The semaphore caps in-flight GPU work at 1, so the CPU-side `replace` of `displayTexture` never overlaps GPU sampling of its previous contents.  Trade-off: per-draw full-image replace (~8MB / 480 MB/s at 1024² @ 60 Hz — trivial on Apple Silicon's ~200 GB/s unified-memory bandwidth) replaces the per-tile region replace; the per-tile cost moves into `stagingBuffer`'s region memcpy, which is the same shape as before.  Incremental rendering preserved: `stagingBuffer` accumulates region updates across multiple worker fires; each `draw(in:)` picks up the latest state.  Build clean; full library tests still pass except a pre-existing `BDPTVertexRIGRebuildTest` failure that landed via the `Uniform Integrator exploration strategy` upstream commit, unrelated to L5a.
- **L4b/c/d review round 5**: 2 P1s (one per platform).
  - **P1-A (Android JNI global ref UAF)**: `RenderViewModel.kt:327` calls `nativeSetCallback(null)` at ViewModel `onCleared` without first joining the `Dispatchers.IO`-launched `nativeRasterize` coroutine — the comment at `RenderViewModel.kt:313` explicitly documents this ("native render is still blocking and winding down").  Meanwhile the new VFS path `RiseBridge::onVFSTileComplete` and the existing `onProgressTick` / `onLogLine` / `ensureFramebuffer` / `writeDirtyRegion` all `JNI->CallVoidMethod` against `m_kotlinCallback` from rasterizer worker threads.  `setCallback(nullptr)` did `DeleteGlobalRef` unsynchronized → race against in-flight `CallVoidMethod` on a stale jobject.  Fix: added `mutable std::mutex m_kotlinCallbackMutex` guarding `m_kotlinCallback`.  All five reader sites (onSceneReady, onRegionInvalidated × 2, onProgress, onLog) plus the `setCallback` writer plus the dtor cleanup take the mutex; readers hold it across the `CallVoidMethod` call so a concurrent `setCallback(null)` can't `DeleteGlobalRef` mid-call.  Holding the lock across CallVoidMethod is safe because the Kotlin callback handlers (Compose state updates) don't re-enter the bridge.  Hottest call site (`onProgressTick` — fires hundreds of times per render) eats the lock cost rather than re-open the UAF window.
  - **P1-B (Qt load thread untracked)**: `loadScene()` started a `QThread::create([this]{ m_job->LoadAsciiScene(...); QMetaObject::invokeMethod(this, ...); })` that the round-4 `m_renderThread` slot never tracked.  `~RenderEngine()` only joined the render slot, so closing the app during scene load races `m_job` access on the load worker against `m_job->release()` in the dtor (and the `QMetaObject::invokeMethod(this,...)` queued lambda against `this` destruction).  Fix: renamed `m_renderThread` → `m_workerThread` and `waitForRenderToFinish` → `waitForWorkerToFinish`.  New `trackWorkerThread(QThread*)` helper centralises the publish-and-track pattern (sets `m_workerThread` under mutex, registers `Qt::DirectConnection` finished-signal that auto-clears, registers `deleteLater`).  All four `QThread::create` sites in the engine (loadScene + startRender + startAnimationRender) now call `trackWorkerThread(thread)` before `thread->start()`.  Load and render don't overlap in practice (UI gates Render on `state==SceneLoaded`) so a single tracking slot is sufficient.  `loadScene` and `clearScene` and the dtor all call `waitForWorkerToFinish()` before touching `m_job` / `m_viewportFrameStore`, so mid-flight worker activity is always drained before scene-state transitions.
- **L4b/c/d review round 7**: 1 P1 (perf regression) + 1 P2 (stale events).
  - **P1 (4× perf regression on macOS)**: VFS `OnTileComplete` callback fired the bridge's `RenderToBuffer(full-image-roi)` on every tile event.  For a 1024×1024 image with 32×32 tiles that's 1024 full-image renders per frame instead of 1024 tile-area renders — exactly the source of the 4× wall-time regression a Mac client run surfaced.  The legacy `Job::CallbackRasterizerOutputDispatch::OutputIntermediateImage` only ran `Integerize<sRGBPel,unsigned short>(65535)` over the dirty rect (not the full image) before firing the user callback with the rect's bounds; my L4b/c/d initial drop ignored the rect and re-rendered the whole frame each fire.  **Fix on all three platforms**: `OnTileComplete` lambdas now receive the half-open `roi` from the FrameStore observer and pass it through to `RenderToBuffer`.  The bridges compute `dst = bufferBase + (y0 * W + x0) * bpp` and pass FULL row stride so the kernel `dst[(y - y0) * dstStride + (x - x0) * bpp]` lands pixels at their actual image-space positions — only the dirty region is touched, the rest of the buffer keeps its prior contents (incremental rendering, exactly like legacy).  The on-screen consumer (Swift `RenderImageBuffer.handleOutput` / Qt QImage / Android onRegionInvalidated) gets the inclusive bounds `(y0, x0, y1-1, x1-1)` so its per-pixel `>> 8` walk stays region-bounded too.  Frame-complete callbacks (which fire once per frame, not per tile, and have no roi) keep the full-image path so post-denoise / post-resolve coherence is preserved.  Per-tile work is now O(tile-area) instead of O(image-area).  **New regression test** in `TestRenderToBuffer`: fills a 16×16 buffer with sentinel `0xAB`, calls `RenderToBuffer` with a 4×4 sub-region at (4,4)→(8,8), then asserts (a) every pixel OUTSIDE the region still equals `0xAB` (the perf-fix invariant — region-bounded must not touch other pixels), and (b) every pixel INSIDE the region has alpha=255 (rendered).  Tests bumped from 51 to 53 assertions.
  - **P2 (Qt stale queued events leak across loadScene / clearScene)**: round-6 added `removePostedEvents(this)` only in `~RenderEngine()`.  Worker threads in `loadScene` / `startRender` / `startAnimationRender` post `Qt::QueuedConnection` completion lambdas to the UI thread; if the user clicks Load (or Clear) while a render is in flight, `m_cancelFlag = true; waitForWorkerToFinish()` joins the worker BUT the queued completion lambda is still in the UI-thread event queue.  The new scene starts loading; meanwhile Qt delivers the stale completion lambda which calls `setState(Completed/Cancelled/Error)` + `m_elapsedTimer->stop()` + emits stale `progressUpdated` / `imageUpdated` / `logMessage` for the previous scene — overwriting the new state.  The QPointer guard from round-6 doesn't help because the engine is still alive (just transitioning scenes, not destroyed).  **Fix**: moved `QCoreApplication::removePostedEvents(this)` from the dtor INTO `waitForWorkerToFinish()`, so every caller (dtor + loadScene + clearScene) drains stale queued events after joining the worker.  RenderEngine only receives events it posts to itself, so removing all events for `this` is acceptable.  Comment in the dtor updated to point at the centralised drain.
- **L4b/c/d review round 6**: 1 P1 — Qt queued-lambda UAF after `waitForWorkerToFinish()`.
  - `thread->wait()` only joins the worker thread function; it does NOT drain `Qt::QueuedConnection` lambdas the worker posted to the receiver's UI-thread event-loop queue via `QMetaObject::invokeMethod(this, ...)`.  After `wait()` returns, the dtor proceeds — but the queued completion lambda is still sitting in `this`'s event-loop queue.  If the UI thread later resumes the event loop (or runs a nested loop) Qt would deliver the queued message against a freed receiver and freed captured pointers (`m_job`, `m_elapsedTimer`, `progressCb`) — UAF on `this->setState(...)`, `this->m_job->SetProgress(nullptr)`, `delete progressCb`.  Affected sites: 7 `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)` calls (loadScene completion, startRender completion, startAnimationRender completion, onProgress, onLogMessage, VFS-tile fan-out in `renderViewportToBufferAndEmit_locked`, plus one repeat).  **Two-layer fix**: (a) every queued lambda now captures a `QPointer<RenderEngine> guard(this)` and early-returns if it cleared (which happens automatically when the QObject is destroyed) — guard checks before any `guard->member` deref, including the `delete progressCb` cleanup path so the heap allocation is reclaimed even on the UAF-avoidance path; (b) `~RenderEngine()` calls `QCoreApplication::removePostedEvents(this)` after `waitForWorkerToFinish()` so Qt's event delivery path drops pending queued messages before the dtor runs to completion.  Either layer alone closes the race; both together is defense-in-depth (the `removePostedEvents` is the primary guarantee, `QPointer` is the safety net for any future code path that adds a queued lambda site without the maintainer remembering the pattern).  All 7 sites converted.  Linux make build + 94/94 library tests still pass; the Qt code only compiles on Windows so end-to-end verification waits on the next Windows-machine smoke test.

---

## 12. Open decisions for review

1. **~~`std::shared_ptr<FrameStore>` vs. RISE's own `Reference`-counted handle?~~**
   **Resolved (2026-05-08):** Use `Reference` (intrusive refcount, the rest-of-library convention).  Construction is by `new FrameStore(...)` with a refcount of 1; ownership transfer is via `AddRef()` / `Release()` rather than `std::shared_ptr` semantics.  All FrameStore APIs that hand out a FrameStore handle return `FrameStore*` (caller responsible for release) per the existing pattern in `IRasterizer`, `IObjectManager`, etc.

2. **~~Channel storage: typed via `ChannelId` enum, or a free-form `std::string` key?~~**
   **Resolved (2026-05-08):** Typed enum (`ChannelId`).  Extending the channel set requires adding to the enum + recompilation, which is acceptable given the small expected set (Beauty + a handful of AOVs).  No string-keyed `Custom` slot; if a third-party use case appears later, the enum can grow.

3. **~~AOVs in Phase 1 or defer to Phase 2?~~**
   **Resolved (2026-05-08):** Structurally support AOVs in Phase 1.  `FrameStore::Spec` accepts an optional list of `ChannelId`s beyond Beauty; FrameStore allocates those channels' storage and exposes typed `GetChannel<T>(ChannelId)` access.  Rasterizers don't populate them yet (that's Phase 2 / L7), but the API surface is in place so encoders and observers can be designed against the final shape.

4. **Should `ViewTransform` be a value type or refcounted/shared?**
   Value type (POD-ish struct, ~40 bytes). Cheap to pass by const ref into `Render`. Recommend value.

5. **`Render` thread-safety — is it required to be callable from multiple threads concurrently?**
   The seqlock makes it *safe*, but is it useful? Use case: viewport repaints + an Auto-Save thread + a network mirror thread, all reading independently. Recommend yes — costs nothing extra.

6. **Tile size: rasterizer-driven or FrameStore-driven?**
   Today the rasterizer picks. If FrameStore stores at a different tile size, the seqlock granularity mismatches the rasterizer's actual write granularity (false sharing). Recommend: FrameStore::Spec accepts a `tileEdge` and the rasterizer factory queries it; or the factory passes its preferred tile size into `Spec`.

7. **`IRenderObserver` callbacks — main-thread dispatch or render-thread dispatch?**
   Render-thread is simpler (today's `IRasterizerOutput` works that way). Each platform's GUI bridge already marshals to the UI thread via Qt signals / `dispatch_async` / Compose `LaunchedEffect`. Recommend: render-thread dispatch, document loud-and-clear.

8. **EXR multichannel layout for AOVs — channel naming convention?**
   Industry options: OpenEXR's `R/G/B + A + N.X/N.Y/N.Z + albedo.R/...` or the Cryptomatte convention. Recommend: follow OpenEXR Technical Introduction §"Storing Multichannel Data" with named layers, and let the encoder accept a `std::map<ChannelId, std::string>` for layer names if the user wants custom naming.

9. **HDR display on Mac: do we need an Info.plist / capability declaration?**
   `wantsExtendedDynamicRangeContent` is the minimum. macOS 10.14+ supports it; some displays return `nil` for `maximumExtendedDynamicRangeColorComponentValue`, in which case we fall back to `RGBA8_sRGB` automatically. UI logic: query the active screen's EDR headroom on each window-move event.

10. **Save-As dialog defaults.** When user picks PNG, default `ViewTransform` is the current viewport's (slider-driven) — or the scene-declared one from `file_rasterizeroutput` (if any) — or zero? Recommend: default to viewport's current; let the dialog show "match scene file" / "linear / no transform" options.

---

## 13. Risks

- **Performance regression in Phase 1 from the redundant copy.** Worst case is ~one frame's worth of memory bandwidth per tile completion. On a 4K beauty-only render (~32MB) at 100ms tiles that's ~320 MB/s — manageable, but measure on Sponza-class scenes before committing. The [performance-work-with-baselines](skills/performance-work-with-baselines.md) skill applies.
- **Tile-seqlock subtleties.** Reader spin-yield can starve under heavy contention; mitigation is the read-side `std::this_thread::yield()` and the fact that writes are short. Adversarial-review the FrameStore concurrency carefully (L1 gate).
- **ABI break in Phase 2 affects all `RISE_API_*` callers.** The DRISE client, all three GUI apps, and any out-of-tree tools all rebuild. Stage carefully; bump API version explicitly. The `abi-preserving-api-evolution` skill is the playbook.
- **Color-space round-trip bugs.** The conversion matrix table is correctness-sensitive. L0 has explicit per-target round-trip tests for a reason — don't skip.
- **`IRasterizerOutput`-using third-party code.** If any out-of-tree integration uses `IRasterizerOutput` directly, Phase 2 breaks them. Phase 1's compatibility shim should be retained for at least one release after Phase 2 lands.

---

## 14. What we're NOT doing (and why)

- **Mid-render scene mutation in this initiative.** The `InteractiveEditor` already handles cancel-restart on edits; FrameStore makes it slightly nicer (edits don't lose the previous frame), but the editor's state machine doesn't change.
- **Multichannel EXR encoder in initial landing.** Schedule for L7 once Phase 2 exposes the AOV channels.
- **Streaming tile-by-tile file write (for crash safety).** Out of scope; would require a "partial encode + finalise" extension to `IFrameEncoder`. Defer until someone asks.
- **Custom user-defined AOVs (string-keyed, beyond the enum).** Defer until use case exists; today's PT/BDPT/VCM produce a fixed AOV set.
- **A `RenderSession` object subsuming Job + Rasterizer + FrameStore + observers.** This was Option D in the high-level design discussion; out of scope unless interactive-edit becomes a near-term product direction.

---

*End of design doc. Ready for review; expect at least one round of revisions before code lands.*
