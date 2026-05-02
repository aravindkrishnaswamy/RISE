# Landing 1 Design — HDR primary output + separated display transform

Detailed implementation design for Landing 1 of the
[Physically Based Pipeline Plan](PHYSICALLY_BASED_PIPELINE_PLAN.md).

This doc supersedes the brief Landing 1 section in the parent plan;
that section gets marked DONE with a pointer here when this lands.

## TL;DR

EXR support already exists in the engine but is wired as an
afterthought.  The actual gap is:

1. There's no display-transform stage between the linear film and
   the file writer.  PNG outputs go straight from linear radiance
   to 8/16-bit clamp through the colour-space converter — anything
   above 1.0 saturates to white.
2. There's no exposure parameter.
3. The EXR writer hard-codes RGBA / half-float / PIZ — useful but
   not parameterised.
4. The default scene authoring pattern still produces PNG-only
   output, so renders aren't archivable as ground truth.

Landing 1 fixes all four by adding a `DisplayTransformWriter`
between the source `IRasterImage` and the writer, exposing
`exposure` + `display_transform` parameters on
`file_rasterizeroutput`, parameterising the EXR writer's compression
/ precision / channel count, and migrating Sponza to declare both an
EXR primary and a tone-mapped PNG view.

ABI break: not required.  Constructor-overload pattern keeps the
existing `RISE_API_CreateFileRasterizerOutput` signature working.

## Status quo

### What exists

- `FileRasterizerOutput` with format enum `TGA / PPM / PNG / HDR /
  TIFF / RGBEA / EXR` — see
  [src/Library/Rendering/FileRasterizerOutput.h](../src/Library/Rendering/FileRasterizerOutput.h).
- `EXRWriter` backed by OpenEXR `Imf::RgbaOutputFile`, fixed
  half-float / PIZ_COMPRESSION / RGBA premultiplied alpha — see
  [src/Library/RasterImages/EXRWriter.cpp](../src/Library/RasterImages/EXRWriter.cpp).
- `EXRReader` for round-trip verification.
- Chunk parser entry at
  [AsciiSceneParser.cpp:7058–7072](../src/Library/Parsers/AsciiSceneParser.cpp).
- Build wiring: vcpkg on Windows; Homebrew auto-detect on macOS;
  off on Linux (`-DNO_EXR_SUPPORT`).
- Internal working space is `RISEPel = ROMMRGBPel` (linear ROMM RGB,
  wide gamut), declared in
  [Color.h:45](../src/Library/Utilities/Color/Color.h).

### What's missing

- No exposure parameter anywhere in the output pipeline.
- No tone-mapping / display-transform stage.  Pixels above 1.0
  saturate at the integerization step in `PNGWriter::WriteColor`.
- No way to declare "give me both EXR primary and tone-mapped PNG"
  in a single scene.  Today scenes pick one format and live with it.
- EXR writer's compression / precision / alpha are not surfaced as
  scene parameters.
- EXR header carries no scene metadata (no scene path, no render
  config, no chromaticity tag).  Hard to trace an EXR back to the
  render that produced it.
- `Imf::RgbaOutputFile` only does half-float.  Full-float and
  multi-channel (AOVs) need the lower-level `Imf::OutputFile` API.

### Existing convention as written today

```
file_rasterizeroutput
{
    pattern             rendered/sponza_new
    type                PNG
    bpp                 8
    color_space         sRGB
}
```

## Goals

1. Render to linear-radiance EXR as the **primary** archival output.
   No exposure, no display transform — the EXR is the integrator's
   ground truth.
2. PNG (and other LDR formats) become **derived views**:
   `radiance × 2^EV → display_transform → OETF → 8/16-bit encode`.
3. A single scene can declare both an EXR primary and one-or-more
   PNG views with different exposure / transform combinations.  No
   re-render needed; same `IRasterImage` flows to all outputs.
4. Defaults preserve existing behaviour byte-for-byte.  No scene
   re-renders unless the author opts in.
5. Sponza updates to the new pattern, and the new pattern is
   documented in [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) as
   the recommended setup for any future scene.

## Non-goals (deferred to v1.1 or later)

- **EXR multi-channel AOVs** (albedo, normal, depth, primary
  visibility into one EXR).  Requires switching from
  `Imf::RgbaOutputFile` to `Imf::OutputFile` with a custom channel
  list.  Tracked as a v1.1 follow-up.
- **EXR full-float (32-bit) precision.**  Same blocker as AOVs.
  v1.1.
- **AgX with primaries-aware look transform.**  v1 ships a
  scalar AgX sigmoid; the full AgX with input/output primaries
  matrices is a separate spectral / colour-management exercise that
  belongs after Landing 3.
- **Auto-exposure / histogram-driven tone mapping.**  Out of scope
  for a path tracer; if needed, ship as a separate offline tool.
- **Spectral EXR output** (one channel per wavelength bin).  Belongs
  with Landing 3.
- **Tone-mapping shader-op chain.**  We use the simple
  parameter-on-output model for v1; promote to a shader-op only if
  composition demands it (see Open Questions in the parent plan).

## Design

### High-level data flow

Today:

```
Integrator → Film (FilteredFilm) → Resolve → IRasterImage
   → FileRasterizerOutput::WriteImageToFile
   → IRasterImageWriter (per-format)
   → primaries convert + OETF + clamp inside writer
```

After Landing 1:

```
Integrator → Film → Resolve → IRasterImage (linear radiance)
   ├── FileRasterizerOutput[EXR]
   │     → IRasterImageWriter (EXR)
   │     → linear radiance written verbatim, half-float
   │
   └── FileRasterizerOutput[PNG, exposure=0, display_transform=aces]
         → DisplayTransformWriter wraps inner IRasterImageWriter (PNG)
         │   per-pixel: linear * 2^EV → tone curve → forward to inner
         → IRasterImageWriter (PNG)
         → primaries convert + OETF + clamp (existing behaviour)
```

The key insight: the display transform sits **between** the source
image and the format writer.  The format writer's existing
primaries/OETF/clamp logic is unchanged.  This means PNG, TGA, PPM,
TIFF, HDR, RGBEA all benefit from display transform / exposure
without any per-writer changes.

### Why wrap the writer (not the image)

Threading exposure + transform through every writer means
duplicating the chain in PNG / TGA / PPM / TIFF / HDR / RGBEA /
EXR writers.  That's seven copies of the same logic, each with its
own bug surface.

`IRasterImage::DumpImage(IRasterImageWriter*)` already iterates
pixels and calls `pWriter->WriteColor(c, x, y)` per pixel.
Interposing on `WriteColor` is a single class with three short
methods (`BeginWrite` / `WriteColor` / `EndWrite`, each forwarding
to an inner writer with `WriteColor` applying the per-pixel
transform).  Wrapping the image instead would require
re-implementing `DumpImage` iteration plus stubs for unused
`SetPEL` / `Clear` / `LoadImage` / etc.

### New types

```cpp
// src/Library/Rendering/DisplayTransform.h
namespace RISE
{
    enum DISPLAY_TRANSFORM
    {
        eDisplayTransform_None     = 0,   // identity (no tone-mapping)
        eDisplayTransform_Reinhard = 1,   // x / (1 + x)
        eDisplayTransform_ACES     = 2,   // Narkowicz fit (DEFAULT)
        eDisplayTransform_AgX      = 3,   // Sobotka scalar form
        eDisplayTransform_Hable    = 4,   // Uncharted 2 / John Hable
    };

    namespace DisplayTransforms
    {
        // Pure per-channel functions.  Each takes a linear scalar
        // (one channel of an RISEPel), returns the post-transform
        // linear scalar.  Negative inputs are clamped to 0 before
        // the curve (ACES / Reinhard are undefined for negatives).
        // NaN / Inf inputs collapse to 0 to avoid downstream poison.
        inline Scalar None    ( Scalar x );
        inline Scalar Reinhard( Scalar x );
        inline Scalar ACES    ( Scalar x );
        inline Scalar AgX     ( Scalar x );
        inline Scalar Hable   ( Scalar x );

        // Convenience: dispatch over the enum and apply to all
        // channels of an RISEPel.  Caller scales by exposure first.
        inline RISEPel Apply( DISPLAY_TRANSFORM dt, const RISEPel& linear );
    }
}
```

```cpp
// src/Library/Rendering/DisplayTransformWriter.h
namespace RISE { namespace Implementation {

// Wraps an inner IRasterImageWriter; applies exposure (EV stops)
// then a display transform to every WriteColor before forwarding
// to the inner writer.  BeginWrite / EndWrite are pass-throughs.
//
// Lifetime: holds an addref'd reference to the inner writer; releases
// in the destructor.  Caller still owns the inner writer reference
// they passed in (i.e. inner_writer.addref() count goes up by 1 when
// this wrapper is constructed).
class DisplayTransformWriter :
    public virtual IRasterImageWriter,
    public virtual Reference
{
protected:
    IRasterImageWriter&  inner;
    const Scalar         exposureMul;   // = pow(2, exposureEV)
    const DISPLAY_TRANSFORM  dt;

    virtual ~DisplayTransformWriter();

public:
    DisplayTransformWriter(
        IRasterImageWriter& innerWriter,
        Scalar exposureEV,
        DISPLAY_TRANSFORM dt );

    void BeginWrite( unsigned int width, unsigned int height ) override;
    void WriteColor( const RISEColor& c, unsigned int x, unsigned int y ) override;
    void EndWrite() override;
};

}}
```

### `FileRasterizerOutput` constructor change

```cpp
// Single signature; all callers pass all parameters.  The chunk
// parser supplies sensible defaults from the chunk descriptor when
// the user omits a parameter.
FileRasterizerOutput(
    const char* szPattern_,
    bool bMultiple_,
    FRO_TYPE type_,
    unsigned char bpp_,
    COLOR_SPACE color_space_,
    Scalar exposureEV_,
    DISPLAY_TRANSFORM display_transform_,
    EXR_COMPRESSION exr_compression_,
    EXR_PRECISION exr_precision_,
    bool exr_with_alpha_ );
```

`WriteImageToFile` wraps the format writer in
`DisplayTransformWriter` when `(exposureEV != 0 || dt != None)` and
the format is an LDR type.  HDR types (EXR / HDR / RGBEA) skip the
wrap and pass the source straight through — no display transform
on archival outputs, ever (and a warning fires if the chunk
specified a non-default `exposure` or `display_transform` on an
HDR type).

### `IJob::AddFileRasterizerOutput` signature

Single signature with the same parameters.  No overload pattern —
ABI is not preserved (decision recorded above).  Every caller
passes the full parameter list; chunk-parser defaults supply the
common case.

### Chunk parser surface

```
file_rasterizeroutput
{
    pattern             rendered/sponza      # required
    multiple            FALSE                # existing, default FALSE
    type                EXR                  # existing enum + EXR
    bpp                 8                    # existing; "32" silently
                                             # treated as half until v1.1
    color_space         sRGB                 # existing

    # NEW (LDR formats only — warning if set on HDR types)
    exposure            0.0                  # EV stops; default 0
    display_transform   aces                 # none|reinhard|aces|agx|hable
                                             # default: aces for LDR, none for HDR
                                             # (per-type default avoids spurious
                                             #  HDR warnings on existing scenes)

    # NEW (EXR only — warning if set on non-EXR types)
    exr_compression     piz                  # none|zip|piz|dwaa|dwab
                                             # default: piz
    exr_precision       half                 # half (v1.1: full)
    exr_with_alpha      TRUE                 # default TRUE
}
```

ChunkDescriptor entries get appended in
[AsciiSceneParser.cpp:7066–7068](../src/Library/Parsers/AsciiSceneParser.cpp);
the `Finalize` method reads them via
`bag.GetDouble("exposure", 0.0)` etc.

### EXR writer changes

`EXRWriter` constructor takes the three new parameters
(`compression`, `precision`, `with_alpha`).  For v1, only
`compression` is honoured beyond the existing PIZ default;
`precision = full` and `with_alpha = false` log a warning that v1
ignores them and falls back to half + RGBA.

EXR header gets:
- `software` = `"RISE vMAJ.MIN.PATCH build N"` (already known)
- `comments` = scene file path (passed through from caller)
- `chromaticities` = ROMM RGB primaries (so colour-managed viewers
  see correct hues; viewers that ignore the tag see slightly
  desaturated values, but the data is recoverable)
- `whiteLuminance` = 1.0 (declares pixel value 1.0 == 1 cd/m² for
  HDR display — a starting convention; revisit when Landing 5
  ships physical camera)

The chromaticity tag specifically:
```cpp
header.insert("chromaticities", Imf::ChromaticitiesAttribute(
    Imf::Chromaticities(
        Imath::V2f(0.7347f, 0.2653f),  // ROMM red
        Imath::V2f(0.1596f, 0.8404f),  // ROMM green
        Imath::V2f(0.0366f, 0.0001f),  // ROMM blue
        Imath::V2f(0.3457f, 0.3585f)   // D50 white
    )));
```

### Display transform formulae

All operate per-channel on linear input, return linear output.
Negatives are clamped to 0 before the curve.

**None** — identity.

**Reinhard** — `f(x) = x / (1 + x)`.  Approaches 1 asymptotically;
no whitepoint.  Cheap, well-known, doesn't crush highlights.

**ACES (Narkowicz fit)**:
```
f(x) = saturate( x*(2.51*x + 0.03) / (x*(2.43*x + 0.59) + 0.14) )
```
Per-channel; matches ACES RRT+ODT for sRGB to within ~1% at
ordinary luminances.  Standard "PBR-looking" curve.

**AgX (scalar)**:  Troy Sobotka's sigmoid in log space.
```
f(x) = 0.5 + 0.5 * tanh( a * (log10(max(x, 1e-10)) - b) )
```
with `a = 1.0`, `b = 0.0` for v1.  The full AgX with primaries
look-transform is a separate exercise (deferred — see non-goals).

**Hable (Uncharted 2)**:
```
A=0.15; B=0.50; C=0.10; D=0.20; E=0.02; F=0.30
hable(x) = ((x*(A*x+C*B) + D*E) / (x*(A*x+B) + D*F)) - E/F
f(x) = hable(x * 2.0) / hable(11.2)   // pre-scale + white-point norm
```

### File-system convention

Each `file_rasterizeroutput` writes to `<pattern>.<ext>` (or
`<pattern>NNNN.<ext>` when `multiple = TRUE`).  When two outputs
share `pattern` but differ in `type`, both files coexist:

```
rendered/sponza_new.exr   ← from the EXR output
rendered/sponza_new.png   ← from the PNG output
```

When the source image is denoised, the suffix `_denoised` is
inserted before the extension as today:

```
rendered/sponza_new.exr
rendered/sponza_new_denoised.exr
rendered/sponza_new.png
rendered/sponza_new_denoised.png
```

The display transform applies to **both pre- and post-denoise
PNGs**; OIDN operates on linear radiance and the adapter sits
downstream.  Correct ordering.

### Backwards compatibility

Every existing scene file has a `file_rasterizeroutput` with no
new parameters.  Defaults `exposure = 0` and `display_transform =
none` mean the chain is identity.  PNG bytes are pixel-identical
to today.

The only observable change for existing scenes: a new
`ScanRasterImageForPathologicalPixels` warning may fire for
display-transform-clamped pixels — but it already runs and
produces this warning in the current code, so no behavioural
change.

## Sub-landing phasing

Each sub-landing is a single PR-equivalent commit, individually
shippable + verifiable.

### L1.1 — Display-transform foundation

- Add `src/Library/Rendering/DisplayTransform.h` with the
  enum + curves.
- Add `src/Library/Rendering/DisplayTransformWriter.{h,cpp}`.
- Extend `FileRasterizerOutput` with the new constructor overload;
  `WriteImageToFile` wraps source via adapter when needed.
- Extend `IJob::AddFileRasterizerOutput` overload.
- Extend `RISE_API_CreateFileRasterizerOutput` overload.
- Extend `file_rasterizeroutput` chunk parser with `exposure` +
  `display_transform` parameters.
- Update **all five** build projects per CLAUDE.md (Filelist,
  rise_sources.cmake, Library.vcxproj, Library.vcxproj.filters,
  rise.xcodeproj).
- New tests:
  - `tests/DisplayTransformTest` — sentinel input/output values for
    each curve, verified to float epsilon.
  - `tests/DisplayTransformWriterTest` — wrap a known image,
    verify per-pixel transform applies.
- New regression scene:
  - `scenes/Tests/Output/display_transform_curves.RISEscene` —
    horizontal radiance gradient (0 → 8 in linear), four PNG
    outputs (none / reinhard / aces / hable) with golden
    references checked into `scenes/Tests/Output/golden/`.
- Existing scenes render byte-identical.  Verify with the existing
  `tests/RegressionRender` corpus.

### L1.2 — EXR parameterisation + metadata

- Extend `EXRWriter` constructor with compression / precision /
  with_alpha.  v1 honours compression only; precision != Half and
  with_alpha == false log a warning.
- Add EXR header attributes (software, comments, chromaticities,
  whiteLuminance).
- Extend `RISE_API_CreateEXRWriter` overload.
- Extend `file_rasterizeroutput` chunk parser with EXR parameters.
- New test:
  - `tests/HDRRoundTripTest` — write a synthetic image with
    known float values (gradients, near-zero, large, NaN-clamped)
    to EXR, read back via `EXRReader`, verify within half-float
    epsilon.
- New regression scene:
  - `scenes/Tests/Output/exr_round_trip.RISEscene` — emits an
    EXR; the test loads it and checks pixel values against
    expected analytic radiance.

### L1.3 — Sponza migration + docs

- Update `scenes/FeatureBased/Geometry/sponza_new.RISEscene` to
  declare both an EXR primary and a tone-mapped PNG view.
  Update the inline comments to explain the pattern.
- Add a "HDR output pipeline" section to
  [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) documenting:
  - The recommended EXR-primary + PNG-derived pattern.
  - Display transform tradeoffs (when to pick which curve).
  - Exposure semantics (EV stops, additive in log space).
  - The non-goal of folding tone-mapping into the integrator.
  - Why `display_transform = none` is the default (back-compat).
- Mark Landing 1 DONE in
  [PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md).
- Render Sponza at fixed seed/spp/camera; commit the EXR (or
  external pointer if too large) as the baseline for Landings
  2–14 to compare against.

### L1.4 — (deferred to v1.1)

Tracked here for future:

- AOV channels in EXR (albedo, normal, depth, primary visibility)
  via `Imf::OutputFile` lower-level API.
- EXR full-float precision.
- AgX with primaries-aware input/output transforms (after
  Landing 3 spectral lands).
- Filmic / Blender-style LUT-backed transform (after we decide
  whether we want LUT support at all).

## Verification

### Test scaffolding additions

| Test | Type | What it proves |
|---|---|---|
| `DisplayTransformTest` | unit | each curve matches its formula at sentinel values |
| `DisplayTransformWriterTest` | unit | adapter applies per-pixel correctly |
| `HDRRoundTripTest` | unit | EXR write + read preserves pixel values within half-float epsilon |
| `display_transform_curves.RISEscene` | regression | each curve renders pixel-identical to checked-in golden PNG |
| `exr_round_trip.RISEscene` | regression | EXR output of a known scene round-trips through the EXR reader |
| Existing `RegressionRender` corpus | regression | every existing scene renders byte-identical to today (no display transform default = no-op) |

### Manual validation

After L1.1:
1. Render Sponza with the existing chunk → byte-identical PNG.
2. Add `display_transform aces` → visibly tone-mapped PNG, no
   highlight blow-out.
3. Add `exposure 1.0` → image is 2× brighter linearly before tone
   curve.

After L1.2:
1. Render Sponza with `type EXR`, open in oiiotool / Nuke /
   tev — verify chromaticity tag is read, image is in linear
   space, dynamic range is preserved.
2. Round-trip via `oiiotool --colorconvert ROMM linear-sRGB foo.exr foo_srgb.exr`
   and verify a viewer that respects chromaticities renders
   identically to our PNG output with `display_transform none`
   (i.e., RISE's PNG path and oiiotool's primaries conversion
   produce matching pixels).

After L1.3:
1. Sponza scene declares EXR + PNG; both files appear; PNG looks
   right; EXR opens correctly in external tools.
2. SCENE_CONVENTIONS.md describes the new pattern.

## Migration notes

### Sponza scene update

Before:

```
file_rasterizeroutput
{
    pattern         rendered/sponza_new
    type            PNG
    bpp             8
    color_space     sRGB
}
```

After (proposed):

```
##################################################
# HDR primary output — radiance ground truth.
# No exposure, no display transform.  The EXR is
# the integrator's verbatim output, suitable for
# external colour-managed compositing and as the
# source of truth for variance / RMSE comparison
# across PB pipeline landings.
##################################################
file_rasterizeroutput
{
    pattern             rendered/sponza_new
    type                EXR
    color_space         ROMMRGB_Linear
    exr_compression     piz
}

##################################################
# PNG display preview — derived view of the EXR.
# `exposure 0` = the EXR's nominal radiance values
# are passed through unscaled.  `display_transform
# aces` applies the Narkowicz ACES tone curve so
# highlights roll off rather than clipping.  Tweak
# exposure here for over/under exposure tests
# without re-rendering.
##################################################
file_rasterizeroutput
{
    pattern             rendered/sponza_new
    type                PNG
    bpp                 8
    color_space         sRGB
    exposure            0.0
    display_transform   aces
}
```

### Other scenes in the corpus

The IMPROVEMENTS.md guidance is "scenes shouldn't change unless
they explicitly opt in."  We do NOT bulk-migrate the regression /
test corpus.  Each scene that uses `file_rasterizeroutput` keeps
working byte-identical; future work can opt in scene by scene.

## Risks and open questions

1. **Half-precision rounding under tone curve.**  When PNG goes
   through the linear-radiance display transform then Integerize,
   intermediate values are double-precision.  When EXR captures
   half, dynamic range below ~6e-5 is lost.  This is fine for our
   use case (we're not doing HDR display below that range) but
   document it.
2. **`bpp = 32` enum value.**  Already listed in the chunk
   parser.  v1 silently treats `32` on EXR as "use half" with a
   warning.  Promote to true full-float in v1.1.
3. **Chromaticity tag and external tools.**  Most HDR viewers
   (tev, mrViewer) honour `chromaticities`.  oiiotool needs an
   explicit `--colorconvert`.  Photoshop's EXR support ignores
   them.  Document this in SCENE_CONVENTIONS.md.
4. **Display transform on negative pixels.**  Mitchell / Lanczos
   filters can produce negatives.  Curves that aren't defined for
   negatives (Reinhard saturates negatives to negative
   asymptotically; ACES produces NaN) get a clamp-to-zero gate.
   Document and test.
5. **Existing `OutputIntermediateImage` is no-op.**  An interactive
   editor (eventually) would want intermediate updates to also pass
   through display transform.  Out of scope for Landing 1; flag
   for the editor work in
   [INTERACTIVE_EDITOR_PLAN.md](INTERACTIVE_EDITOR_PLAN.md).
6. **Linux EXR support.**  Disabled by default.  This landing
   relies on EXR; should we either (a) enable it on Linux by
   default and require OpenEXR be installed, or (b) keep it off
   and have Sponza fall back to PNG-only on Linux?  Recommend (a)
   and update the build docs accordingly.

## Cross-references

- Parent plan: [PHYSICALLY_BASED_PIPELINE_PLAN.md](PHYSICALLY_BASED_PIPELINE_PLAN.md)
- Variance measurement protocol that consumes the new EXR baselines:
  [skills/variance-measurement.md](skills/variance-measurement.md)
- Scene conventions doc to be updated:
  [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md)
- Build-system update obligations: see CLAUDE.md "Source-file
  add/remove — touch ALL five build projects"

## Estimated effort

- L1.1: ~1 day (code + tests)
- L1.2: ~0.5 day (mostly EXR header attributes + parameter wiring)
- L1.3: ~0.5 day (scene update + docs + Sponza baseline render)

Total: ~2 engineer-days, assuming OpenEXR build wiring on Linux is
straightforward.

## Decisions made (2026-05-02)

1. **Display transform set**: `none / reinhard / aces / agx /
   hable`.  **Default depends on the output format**:
   - LDR types (PNG / TGA / PPM / TIFF): `aces` (Narkowicz fit) —
     the de-facto PBR tone curve, gives proper highlight rolloff.
     Existing LDR scenes will start producing tone-mapped pixels;
     this is a deliberate quality upgrade and is noted in the
     migration section.
   - HDR types (EXR / HDR / RGBEA): `none` — those formats
     archive linear radiance verbatim, so tone-mapping them would
     corrupt the radiometric ground truth.
   The per-type default keeps existing HDR scenes byte-identical
   (and warning-free) while delivering the better LDR look.  The
   constructor's "display_transform ignored on HDR" warning fires
   only when the user explicitly sets a non-`none` curve on an HDR
   format (genuine misuse).
2. **Linux EXR posture**: enabled by default.  `Config.Linux`
   drops `-DNO_EXR_SUPPORT` and gains the OpenEXR include / lib
   block from `Config.OSX`.  Documented in
   [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) that OpenEXR is now
   a build dependency.
3. **ABI continuity is not preserved**.  The existing
   `FileRasterizerOutput` constructor, `RISE_API_CreateFileRasterizerOutput`,
   `IJob::AddFileRasterizerOutput`, and `EXRWriter` constructor get
   the new parameters appended directly — no overload pattern, no
   compatibility shims.  Cleaner code; user has explicitly
   permitted this.
4. **One landing, no commits until review**.  L1.1 / L1.2 / L1.3
   collapsed into a single uncommitted change that the user
   reviews end-to-end before anything is committed.
5. **AgX**: ship the simple scalar form (sigmoid-in-log) for v1.
   [Landing 3](PHYSICALLY_BASED_PIPELINE_PLAN.md#landing-3--spectral-upsampling--spectral-sun-and-sky)
   gets a follow-up to upgrade to the full AgX with primaries-aware
   input/output transforms — track this as a Landing 3 sub-task,
   not a v1.1 of Landing 1.

### Architecture refinement during finalisation

Wrap the **writer**, not the **image**.  `IRasterImage::DumpImage`
already iterates and calls `pWriter->WriteColor(c, x, y)` per
pixel — interposing on `WriteColor` is one class with three short
methods, vs. wrapping `IRasterImage` which would re-implement
iteration in `DumpImage` plus stub the unused
`SetPEL / Clear / LoadImage` etc.  New class is therefore named
`DisplayTransformWriter`, not `DisplayTransformWriter`.  The
design above is updated; this note records the change.

## Migration impact

Defaulting `display_transform = aces` means **every existing
scene with a PNG (or other LDR) output starts rendering with a
tone curve applied**.  In practice this:

- Looks better for any scene that produces HDR radiance (most
  PB scenes — the curve replaces hard clipping with soft rolloff).
- Looks visibly different from today's render for scenes that
  were authored to have peak radiance ≤ 1.0 (the curve compresses
  contrast slightly even in the linear regime).
- Does not affect any automated regression test in the current
  corpus — a `Grep` for golden-PNG comparison turned up none.

Scenes that want byte-identical-to-today behaviour should opt in
explicitly with `display_transform none`.  Document this in the
SCENE_CONVENTIONS HDR section.
