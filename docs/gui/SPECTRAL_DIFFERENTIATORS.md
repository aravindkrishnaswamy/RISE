# Direction D — Spectral Differentiators ("don't replicate")

**Status:** DESIGN. No D-feature code. Deep-dive spec spun off from [GUI_ROADMAP.md](../GUI_ROADMAP.md) §8.
Ground truth for what *backing* actually ships is [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) (it supersedes
this header where they disagree). Two backing-state caveats the audit pins: **D5's per-pixel probe buffers are
private** (a net-new additive readback, not a free read — §5.3) and **D3's thin-film backing is on the unpushed
`feature/thin-film-interference` branch, not master** (so "already in the tree" for D3 means that branch — §4.2).
**Owner:** Aravind Krishnaswamy
**Scope:** Design the seven GUI features (D1–D7) that an RGB renderer **physically cannot** do, because they
expose RISE's spectral engine: live spectral colour/curve editing, measured-metal n,k, real thin-film
interference, Jakob–Hanika gamut honesty, "explain the auto-router," EDR cinematography scopes, and spectral
Light Mix. Per roadmap principle 5, these are mostly **thin UIs over painters and engine features RISE already
has** — the differentiation is in surfacing them, not in new physics. Per principle 2, the **math/painter
construction is shared C++** in `src/Library/`; only the widget/overlay is platform-specific.

This spec honours all six roadmap principles (§1) and includes an Android-tier note per §10.4 for every feature.

---

## 0. One-paragraph thesis

Every other "easy" renderer is RGB-internal: KeyShot, Blender, Octane, V-Ray, Substance all carry exactly three
numbers through the whole pipe, so a Kelvin slider is a lookup into a baked RGB ramp, "iridescence" is a
three-point approximation (the [glTF KHR_materials_iridescence][gltf-irid] / Belcour–Barla pre-integration —
itself an *RGB workaround for not having wavelengths*), and a chromaticity scope is the best they can do because
there is no spectrum to inspect. RISE carries 4 hero wavelengths per path through a real spectral integrator
([THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md) §3, "A BSDF sees its wavelength on the spectral
path"), uplifts RGB to spectra via the Jakob–Hanika LUT ([JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md)), and already has
the painters — `spectral_painter`, `blackbody_painter`, `scalar_painter { sellmeier ... }`,
`scalar_painter { file ... }` (measured n,k), `fresnel_mode thinfilm` — to make all of this *physical, not
faked*. The work is a set of widgets that author those painters and a set of scopes that read the EDR
framebuffer. **D1 and D4 are the cheapest and highest-ROI** (pure UI over painters/LUT that already ship), so
they lead the priority order (§9).

---

## 1. Priority ordering (cheapest / highest-ROI first)

| # | Feature | Backing already in tree? | Net-new C++ | ROI rationale |
|---|---|---|---|---|
| **1** | **D4 — JH gamut warning** | LUT + residual fully shipped | a const-query helper + a tiny in-gamut test | Near-zero cost; a *uniquely-spectral honesty* badge no RGB tool can show. Rides on every colour pick (D1, material editor). |
| **2** | **D1 — Spectral picker / curve editor** | all four painters ship | swatch-render helper + curve sampler + named-glass table | Exposes 4 existing painters through one widget; the headline "spectral" UI. Kelvin sub-slider is trivial (`blackbody_painter` exists). |
| **3** | **D2 — Measured-metal n,k picker** | `scalar_painter { file }` → GGX `ior`/`ext` ships; conductor Fresnel ships | a bundled n,k library + a metal dropdown | Pure data + a dropdown; "gold's colour from physics." Reuses D1's swatch renderer. |
| **4** | **D3 — Spectral thin-film slider** | **branch-only** — `fresnel_mode thinfilm` + `film_*` slots + introspection are on the unpushed `feature/thin-film-interference` branch, **not master** (§4.2; [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) punch-list) | reuse D1/D2 swatch at a few angles | Two sliders (thickness nm, view angle) over the branch material. RISE *beats* glTF here (exact vs RGB approx). **Ships only once the branch lands.** |
| **5** | **D5 — "Explain the auto-router" heatmap** | probe (`RunProbe`) + per-pixel σ² ship | expose probe AOVs + a heatmap overlay | No competitor can replicate. Medium lift (plumb probe outputs to the GUI). Pair with RMSE per §5.4. |
| **6** | **D7 — Spectral Light Mix** | partial — NEE picks one light, but emission/env/BDPT-splat/VCM-merge attribution does **not** exist (§7.2a) | **PT** per-light attribution binning (NEE + camera-visible emission + MIS-consistent BSDF-on-emitter) + **Top-K selector** (pilot/scout pass or static-power preselection, §7.3a) + bounded AOV accumulation (Top-K + Environment + other) + re-balance compositor + **single composite-denoise** (§7.2b) | Heaviest engine work (multi-buffer render). **V1 = exact intensity-only re-balance, PT-only** (sound, the V-Ray feature); BDPT/VCM/MLT out of V1 (§7.2a). Top-K chosen up front, frozen for the render (no mid-render swap, §7.3a). Layers stored raw, denoised once on composite (§7.2b). The *spectral* SPD-swap is research-flagged, not v1 (§7.3). |
| **7** | **D6 — EDR cinematography scopes** | EDR display path (`MetalEDRView`) ships | scope compute (CPU/Metal) + overlays | Read-only over the framebuffer; high polish value, but 4 sub-scopes + the EDR-headroom plumbing make it the biggest UI surface. |

The ordering is deliberately **engine-debt-ascending**: D4/D1/D2 ship on painters that already exist on master;
**D3 rides on the thin-film material, which is branch-only** (`feature/thin-film-interference`, not yet merged —
§4.2), so D3 is "wire a widget to an existing painter/material" *once that branch lands*; D5/D7/D6 each need a new
render-output or compute path. Lead with the cheap four (D4/D1/D2/D3) that are "wire a widget to a painter/material
RISE already has," which also seeds the material editor (Direction C) and the approachability story (Direction A).

---

## 2. D1 — Live spectral colour picker / curve editor

### 2.1 What it is

A colour-input control that, unlike every RGB picker, can emit a **real spectrum**. Four input modes, all
writing to a painter chunk the parser already accepts, with a **live spectral swatch** and (for the curve modes)
**the SPD plotted beside the swatch**:

1. **Kelvin slider** (1000–12000 K) → `blackbody_painter`.
2. **Monochromatic slider** (380–780 nm) → a single-spike `spectral_painter`.
3. **Drag-a-CSV measured SPD** (e.g. a real D65/flash/LED measurement) → a multi-sample `spectral_painter`, with
   the loaded curve drawn next to the swatch.
4. **Named-glass dropdown** (BK7 / SF11 / diamond / fused silica …) → `scalar_painter { sellmeier ... }` on a
   dielectric's `ior` slot, with **Abbe number + n_d auto-filled read-only** and a **live prism-dispersion
   preview**.

### 2.2 Backing painters (all already in tree)

| Mode | Painter | Construction site | Parser form |
|---|---|---|---|
| Kelvin | `BlackBodyPainter` ([BlackBodyPainter.h](../../src/Library/Painters/BlackBodyPainter.h)) | `temperature`, `nmbegin/nmend`, `numfreq`, `normalize`, `scale` | `blackbody_painter { temperature 5600 ... }` ([AsciiSceneParser.cpp:2891](../../src/Library/Parsers/AsciiSceneParser.cpp)) |
| Monochromatic + CSV | `SpectralColorPainter` ([SpectralColorPainter.h](../../src/Library/Painters/SpectralColorPainter.h)) — *the one painter that properly implements `GetColorNM`* | `(nm, amplitude)` sample pairs; `cp` repeatable or `file`/`nmfile`/`ampfile` | `spectral_painter { nmbegin .. cp "550 1.0" ... }` ([AsciiSceneParser.cpp:1215](../../src/Library/Parsers/AsciiSceneParser.cpp)) |
| Named glass (IOR) | `SellmeierScalarPainter` ([SellmeierScalarPainter.h](../../src/Library/Painters/SellmeierScalarPainter.h)) — `n²(λ)=1+Σ Bᵢλ²/(λ²−Cᵢ)`, λ in µm, d-line 587.6 nm representative | 6 coefficients `B1 B2 B3 C1 C2 C3` | `scalar_painter { name bk7 sellmeier "1.039 0.231 1.010 0.006 0.020 103.5" }` ([AsciiSceneParser.cpp:1345](../../src/Library/Parsers/AsciiSceneParser.cpp)) |

### 2.3 The math

**Kelvin → spectrum.** `BlackBodyPainter` already evaluates Planck's law per wavelength
(`IntensityForWavelength`, [BlackBodyPainter.cpp:26](../../src/Library/Painters/BlackBodyPainter.cpp)) and
exposes the inverse helpers the UI needs for free: `TemperatureFromPeakNM` / `PeakNMFromTemperature` (Wien's
law) for a "peak wavelength" read-out beside the Kelvin slider. The slider value is the chunk's `temperature`
verbatim — no UI-side baked ramp.

**Monochromatic → spectrum.** One `cp "<nm> 1.0"` sample (or a narrow triangular bump across ±Δnm so the 4-hero
sampler reliably catches it — a near-delta spike can fall between hero wavelengths; the UI should author a small
FWHM, e.g. 5 nm, not a true Dirac). `SpectralColorPainter::GetColorNM` returns `spectrum.ValueAtNM(nm)` directly,
so the rendered hero samples are the authored curve with no JH detour.

**CSV → spectrum.** The dragged file is `(nm, amplitude)` whitespace pairs — *exactly* the `spectral_painter`
`file` form (`fscanf("%lf %lf", ...)`, [AsciiSceneParser.cpp:1158](../../src/Library/Parsers/AsciiSceneParser.cpp)).
The UI validates two numeric columns, ascending nm, ≥ a few samples, then writes either an inlined `cp` list (for
small curves, keeps the scene self-contained / diffable per principle 1) or copies the file into the scene's
asset dir and references it (for dense measurements).

**Named glass → Abbe + prism preview.** Given the 6 Sellmeier coefficients, the UI computes the standard
optical-glass read-outs itself (the same formula `SellmeierScalarPainter::EvalAtNM` uses):
`n_d = n(587.56 nm)`, and the **Abbe number** `V_d = (n_d − 1) / (n_F − n_C)` with the Fraunhofer F-line
486.13 nm and C-line 656.27 nm. Shipped table (Schott catalog, [Schott TIE-29][schott-tie29]):

| Glass | B1, B2, B3 | C1, C2, C3 (µm²) | n_d | V_d |
|---|---|---|---|---|
| **N-BK7** | 1.03961212, 0.231792344, 1.01046945 | 0.00600069867, 0.0200179144, 103.560653 | 1.5168 | 64.17 |
| **SF11** | 1.73848403, 0.311168974, 1.17490871 | 0.0136068604, 0.0615960463, 121.922711 | 1.7847 | 25.76 |
| **Diamond** | (single-term Sellmeier, [refractiveindex.info][rii-diamond]) | — | 2.417 | 55.3 |
| **Fused silica** | 0.6961663, 0.4079426, 0.8974794 | 0.0046791, 0.0135121, 97.934 | 1.4585 | 67.8 |

The **prism preview** is a small built-in render: a triangular `dielectric_material` prism with the chosen
`ior` painter under a thin white beam, rendered with `pathtracing_spectral_rasterizer` so the dispersion fan is
*real transport*, not a fake gradient. The spread directly visualizes V_d (lower Abbe = wider fan: SF11 splays
noticeably more than BK7). Cost-managed as a tiny cached thumbnail (§2.5).

### 2.4 The widget

- **Mode tabs** (Kelvin / λ / CSV / Glass) with a shared right-hand **swatch + curve panel**: the rendered
  swatch on top, the SPD plot beneath (x = 380–780 nm, y = amplitude or n(λ)), spectral-locus tick marks. The
  curve-beside-swatch layout is the signature spectral affordance — RGB pickers have nothing to plot.
- A **JH-gamut badge** (D4) lives in the corner of the swatch and lights when the resolved colour lands in the
  failing blue corner.
- **Round-trip:** the widget reads/writes the painter chunk through `SceneEditController` (principle 6); on
  reopen, it re-parses the chunk back into the right mode (a `blackbody_painter` reopens on the Kelvin tab, a
  6-coeff `scalar_painter` on the Glass tab, etc.).

### 2.5 Feasibility — the live spectral swatch cost (cross-ref §13 #1)

This is the open spike the roadmap flags for A3/D3 (§13 #1: "is a spectral hover-preview cheap enough, or do we
fall back to commit-on-release?"). Concrete guidance for the swatch:

- A flat colour swatch needs **no path tracer** — for the Kelvin/mono/CSV modes the swatch colour is just
  `SpectralPacket::GetXYZ()` → `XYZtoRec709RGB` (the exact path `SpectralColorPainter` and `BlackBodyPainter`
  already run in their constructors — [SpectralColorPainter.cpp:22](../../src/Library/Painters/SpectralColorPainter.cpp),
  [BlackBodyPainter.cpp:160](../../src/Library/Painters/BlackBodyPainter.cpp)). This is **microseconds**, fully
  live on slider drag, no render. *Decision: the swatch is computed, not rendered, for D1/D2.*
  - **⚠ Evaluate the swatch by deterministic dense-wavelength integration, NOT 4 stochastic hero samples.**
    The renderer carries 4 hero λ per *path* and Monte-Carlo-integrates over many paths; a UI swatch has no path
    population to average, so reusing the 4-hero sampler for a single colour chip would alias badly — most
    catastrophically the D1 **monochromatic mode**, whose ~5 nm peak would fall between hero wavelengths and read
    as black (or flicker as the stratified offset moves), and any narrow-band measured SPD. The correct evaluator
    is a *fixed* dense sampling: step λ across **380–780 nm at a fixed Δλ** (e.g. 1–5 nm), accumulate the SPD ×
    CMF products → XYZ, then `XYZtoRec709RGB`. This is exactly what `SpectralPacket::GetXYZ()` already does
    internally (a deterministic CMF integral over the packet, not a hero-sample draw) — so the swatch is *that
    one call*, not a mini path trace. Still microseconds, still live on drag, and stable for a monochromatic
    spike because the fixed grid always lands a sample in-band. The same deterministic integral feeds the SPD
    plot (§2.4) so the curve and the swatch are consistent.
- Only the **prism preview** (D1 glass mode) and the **angle-reactive thin-film swatch** (D3) need actual
  spectral transport. Those follow the A3 rule: a tiny fixed-size (e.g. 96², low-spp, denoise-on) cached
  thumbnail re-rendered on *commit* (slider release), with the live drag updating only the cheap flat swatch.
  **Route that thumbnail render through the `RenderCoordinator` as a `NodePreview`/`Thumbnail`-class isolated job**
  ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §3.1, §5.2) — a *snapshot scene + private `IFilm`*, queued and
  preempted by interactive edits. **Do NOT copy the auto-router probe's live-film `ResizeFilm` round-trip for
  this**: that pattern is safe *only* inside the probe's pre-fan-out `call_once` window and tears the live film
  for any other caller ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §1.3, §5.1, §5.5). The coordinator's
  isolated-job model is the correct general mechanism the roadmap's A3 spike resolves to.
- **Shared vs platform:** the swatch/curve/prism *computation* (spectrum → XYZ → RGB, Sellmeier eval, Abbe,
  thumbnail render request) is shared C++ behind the bridge; the slider/tab/plot widget is platform UI.

### 2.6 Android tier

**Tier B** (§10.4 — "material *instance* sliders," touch-adapted): the Kelvin and λ sliders and the glass
dropdown are touch-native; the swatch is the cheap computed one. The CSV-drag mode degrades to a file-picker
import. The prism preview is a cached thumbnail (Tier B), not interactive.

---

## 3. D2 — Measured-metal n,k picker

### 3.1 What it is

A dropdown of real metals (gold, silver, copper, aluminium, chromium, titanium, …) whose **colour comes from
measured physics** — selecting "gold" loads its spectral complex index of refraction (n + ik) from measured
data into a GGX conductor material. Gold looks gold because its n,k *make* it gold at 4 hero wavelengths, not
because someone typed `(1.0, 0.78, 0.34)`.

### 3.2 Backing feature (all already in tree)

The pipe is fully built and proven by the thin-film work:

- **Data → painter:** `scalar_painter { file <path.n> }` and `{ file <path.k> }` load 2-column `(nm, value)`
  files into a `PiecewiseLinearScalarPainter` ([PiecewiseLinearScalarPainter.h](../../src/Library/Painters/PiecewiseLinearScalarPainter.h)),
  which interpolates per hero wavelength and clamps to endpoints outside the sampled band.
- **Painter → material:** GGX's `ior` and `extinction` slots are `IScalarPainter` (n,k are physical scalars, **not**
  colours — they must *not* go through JH uplift), with `fresnel_mode conductor` ([GGXMaterial.h:49–50](../../src/Library/Materials/GGXMaterial.h),
  [AsciiSceneParser.cpp:3775](../../src/Library/Parsers/AsciiSceneParser.cpp)). The conductor Fresnel is the
  single templated chokepoint `Optics::CalculateConductorReflectance<T>` ([THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md) §3).
- **Precedent:** `colors/thinfilm/substrates/{Ti,Steel,Ta,Nb}.{n,k}` already ship in exactly this format with a
  full provenance README — the metal n,k library is a *superset of files already in the repo*.

### 3.3 The math + data sourcing

No new math — the dropdown writes two `scalar_painter { file }` chunks plus a `ggx_material { fresnel_mode
conductor ior <name>_n extinction <name>_k }`. The **swatch is computed** from the conductor Fresnel at normal
incidence integrated against the CMFs (reuse D1's compute-not-render swatch path).

**Data sourcing — refractiveindex.info, CC0.** Bundle a curated metal set under `colors/metals/<Metal>.{n,k}`,
generated from [refractiveindex.info][rii] the same way `colors/thinfilm/` was. Licensing is settled and
favourable (this is a **load-bearing fact, not a guess**): the refractiveindex.info database is released by its
maintainer (M. N. Polyanskiy) under **CC0 1.0 Universal (public-domain dedication)** — "copy, modify and
distribute, even for commercial purposes, all without asking permission" ([refractiveindex.info-database
README][rii-license]; [Polyanskiy 2024, *Sci. Data* **11**, 94][rii-paper]). The `colors/thinfilm/README.md`
already records this verdict: *"No license restriction applies to redistributing these tabulated constants
alongside RISE."* The **underlying journal papers** are the scientific sources and must be cited (the README
pattern: dataset author/year + publication + RII page per file). Canonical datasets to bundle:

| Metal | Dataset (RII) | Reference |
|---|---|---|
| Gold (Au) | Johnson–Christy 1972 | *Phys. Rev. B* **6**, 4370 |
| Silver (Ag) | Johnson–Christy 1972 | same |
| Copper (Cu) | Johnson–Christy 1972 | same |
| Aluminium (Al) | Rakić 1995 / Cheng 2016 | *Appl. Opt.* 34, 4755 |
| Chromium (Cr) | Johnson–Christy 1974 | *Phys. Rev. B* **9**, 5056 |
| Titanium (Ti) | Rakić 1998 *(already in tree)* | *Appl. Opt.* **37**, 5271 |

**Format discipline** (from the thinfilm README, repeated here because the loader is a bare `fscanf`): two
whitespace numeric columns `nm value`, **nm ascending**, **no headers/comments/commas/units**, n and k in
separate files, ≥ 380–780 nm coverage (RII publishes µm → ×1000). A non-numeric token silently truncates the
load. The import tool (or bundling script) must enforce this.

### 3.4 The widget

Dropdown of bundled metals + a computed colour chip per entry + "n(λ)/k(λ)" curve view (the same SPD plot as
D1). An **"import measured n,k…"** affordance accepts a refractiveindex.info CSV/YAML export and runs it through
the format-discipline validator. Selecting an entry sets the material's `ior`/`ext` slots via
`SceneEditController` and forces `fresnel_mode conductor`.

### 3.5 Feasibility / uncertainties

- **Swatch:** computed (cheap), as D1 §2.5.
- **⚠ Licensing nuance (flag, low-risk):** the *tabulated constants* are CC0 and safe to bundle; the **citation
  obligation** to the underlying papers is real and must travel with the data (mirror the thinfilm README's
  per-file provenance table). This is the only D2 uncertainty and it is procedural, not blocking.
- **Anisotropic / layered metals** (e.g. brushed gold) compose with the existing GGX `alphax`/`alphay` +
  `tangent_rotation` — out of scope for the picker, but the material editor (Direction C) can layer them.

### 3.6 Android tier

**Tier B**: dropdown + computed chip are touch-native; "import n,k" degrades to a file-picker. Curve view is a
static plot.

---

## 4. D3 — Spectral thin-film slider

### 4.1 What it is

Two sliders — **film thickness (nm)** and **view angle** — over a live **angle-reactive swatch** that shows real
soap-bubble / anodized-titanium iridescence shifting hue with both thickness and incidence. This is the feature
where **RISE visibly beats the RGB tools**: [glTF KHR_materials_iridescence][gltf-irid] is an *RGB
approximation* (Belcour–Barla analytical spectral pre-integration — explicitly "an approximation of the original
model" that exists *because* real-time RGB renderers can't carry wavelengths), whereas RISE evaluates the Airy
reflectance **exactly per hero wavelength** on the spectral path ([THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md)
§7, "Spectral path — primary, exact").

### 4.2 Backing feature (Phases 1–3 complete on a branch — not yet master)

The thin-film material is **on the `feature/thin-film-interference` branch, not yet merged to master**
([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) punch-list) — so "already in the tree" below means *that branch*,
and D3 ships only once it lands:

- GGX `fresnel_mode thinfilm` + the `film_ior` / `film_extinction` / `film_thickness` slots (all `IScalarPainter`),
  with exact per-hero-λ evaluation in `ScatterNM`/`valueNM` ([GGXMaterial.h:53–55, 146–161](../../src/Library/Materials/GGXMaterial.h);
  [THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md) Phase-2 outcome).
- The Airy evaluator `ThinFilm::ReflectanceConductor` (`src/Library/Utilities/ThinFilm.h`), N-layer-capable,
  cross-checked against a TMM reference to ~1e-15.
- **Material introspection already surfaces the film slots** — `MaterialIntrospection` exposes `film_thickness`
  as live-editable plus a read-only `fresnel_mode` row ([THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md)
  adversarial-review outcome, commit `48d99f04`). So the property panel can *already* edit thickness; D3 is the
  purpose-built widget on top.
- glTF parameter mapping is ~1:1 (`iridescenceIor`→`film_ior`, `iridescenceThickness`→`film_thickness`); the
  glTF defaults give the UI sensible ranges: **IOR default 1.3**, **thickness 100–400 nm** mapping texture
  0.0→1.0 ([glTF spec][gltf-irid]).

### 4.3 The math

- **Thickness slider (nm):** writes a uniform `film_thickness` scalar (a `scalar_painter { value <nm> }` or the
  introspection setter). For a spatially-varying dial the painter is an image/function2D, but the *slider*
  authors a constant. Sensible range 0–800 nm (covers first + second interference order; glTF's 100–400 is the
  common band).
- **Angle-reactive swatch:** the swatch is rendered at the chosen incidence by evaluating
  `ThinFilm::ReflectanceConductor(cosθ, nm)` at each hero λ for `cosθ = cos(view angle)`, then → XYZ → RGB. The
  hue shift toward grazing is the *physical* `δ = 2π n d cosθ / λ` phase term, identical to what the renderer
  computes — the swatch is a faithful 1-pixel preview of the BSDF, not a gradient hack. Because it's a single
  Fresnel evaluation per hero λ (not a path trace), it is **cheap enough to be live on drag** (unlike the prism
  preview) — the angle slider can update the swatch in real time.
- **Film IOR / extinction** default to the shipped MgF₂-ish transparent film; a "film material" sub-picker can
  reuse D2's n,k library for the oxide (e.g. TiO₂ already ships).

### 4.4 The widget

- Two sliders (thickness nm, view angle 0–89°) + the live angle-reactive swatch + a small "first/second order"
  hue-ladder strip (thickness sweep at fixed angle) so the author sees the whole anodize sequence at once.
- Optional **base-metal** dropdown (reuse D2) and **film-oxide** dropdown so the swatch matches the substrate
  (the thinfilm showcase already proves Ti/Steel/Ta/Nb generality).
- Round-trips to the `ggx_material` chunk's `fresnel_mode thinfilm` + `film_*` lines via `SceneEditController`.

### 4.5 Feasibility (cross-ref §13 #1)

- **Angle-reactive swatch is the cheap case** — a per-hero-λ Fresnel eval, not transport → **live on drag**.
  This is a *better* answer to the §13 #1 spike than the general scene preview: thin-film's "preview" is a
  closed-form BSDF sample, so it sidesteps the hover-preview cost question entirely for the swatch. Only a
  *full-object* preview (curved surface, grazing rim) needs a thumbnail render on commit (§2.5).
- **Shared vs platform:** `ThinFilm::ReflectanceConductor` is shared C++ and already exists; the swatch driver
  (loop hero λ, → RGB) is a shared helper; the two sliders + strip are platform UI.

### 4.6 Android tier

**Tier B**: two sliders + the cheap closed-form swatch are ideal for touch (the swatch is the most
"interactive-feeling" spectral control and costs almost nothing). Full-object preview is a commit-time thumbnail.

---

## 4A. D4 — Jakob–Hanika gamut-honesty warning

(Numbered `4A` so this body section can be inserted without renumbering D5–D7 / §8–§11 below; D4 is **#1 in the
priority order** per §1, ahead of D1 — it is the cheapest, riding on every colour pick.)

### 4A.1 What it is

A small honesty **badge** that lights when a colour the author picks lands in a region the Jakob–Hanika
RGB→spectral uplift **cannot faithfully represent** — i.e. the deep-blue gamut corner where the JH sigmoid model
has limited expressive power ([JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md)). No RGB renderer can show this warning: it
exists *because* RISE uplifts RGB to a real spectrum and can therefore tell the author "the spectrum I will
actually render is not the RGB you typed here." It is a *uniquely-spectral* affordance, which is exactly why it
leads the priority table (§1).

### 4A.2 The critical scoping rule — uplift-only

**The warning applies to RGB→spectral *uplift* paths ONLY. Native spectral inputs need no warning and must not
show one.** The JH LUT is consulted *only* when an RGB colour must be turned into a spectrum
([JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md), `RGBUnboundedSpectrum::FromRGB` / `RGBIlluminantSpectrum::FromRGB`). The
D1 spectral inputs — `spectral_painter` (monochromatic / CSV / measured SPD), `blackbody_painter`,
`scalar_painter { sellmeier }`, measured n,k via `scalar_painter { file }` — **never go through JH uplift** (this
is the whole point of the `IScalarPainter` pipe and of `SpectralColorPainter`'s direct `GetColorNM`, CLAUDE.md
`IScalarPainter` fact). So a colour authored *as a spectrum* is already physical and the badge stays dark; the
badge is meaningful only on a slot fed an RGB triple (an `uniformcolor_painter` reflectance/emission, an
RGB-encoded image texel resolved through `RGBUnboundedSpectrum`). Surfacing a gamut warning on a native-spectral
input would be a false positive and would muddy the very distinction D1/D2/D3 exist to teach.

### 4A.3 The trigger criterion (already-shipped data)

The LUT is `extlib/jakob-hanika-luts/rec709.coeff` and ships at **~3.9 % gamut-corner cell failures** post
Stage-A migration (mean residual 1.2 × 10⁻³, max 9.5 × 10⁻², all in the deep-blue corner — not a colour-space
bug but the sigmoid model's expressive limit) ([JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md) "TL;DR (Stage A update)";
CLAUDE.md JH-LUT fact). The badge does **not** re-run the solver per pick; it consults the *baked* LUT's
per-cell residual for the cell the picked RGB maps to and lights when that residual exceeds the LUT's acceptance
tolerance (the documented `1 × 10⁻⁴` convergence threshold — [JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md) "What NOT to
do"). Concretely this is **a const query against the shipped LUT** plus a tiny in-gamut test (§1 net-new C++
column: "a const-query helper + a tiny in-gamut test"). Implementation note: the LUT data is baked in
`src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp` and read through `RGBToSpectrumTable::Get()`; the
helper exposes that residual read-back without otherwise touching the uplift path. (If the baked LUT does not
retain a per-cell residual, the cheapest faithful proxy is to uplift→re-integrate→ΔE against the requested RGB at
pick time — still microseconds for one colour, and bounded to the uplift path so it inherits the uplift-only
scope of §4A.2.)

### 4A.4 Behaviour + where it surfaces

- **Colour picker (D1):** a corner badge on the live swatch (§2.4 already reserves "a JH-gamut badge (D4) lives
  in the corner of the swatch and lights when the resolved colour lands in the failing blue corner"). On hover it
  reads the one-line honesty string ("This blue is outside what the spectral uplift can match — the rendered
  spectrum will be the nearest in-gamut blue"), with the residual magnitude for the curious.
- **Material swatch (property panel / material editor):** the *same* corner badge on any material slot's RGB
  colour chip whose value is an uplift-fed `IPainter` reflectance/emission — so the warning rides on the material
  editor (Direction C), not just the standalone picker. Scalar/`IScalarPainter` slots (IOR, roughness, n,k,
  thin-film) never show it (§4A.2).
- It is **advisory, not blocking** — the author may keep the out-of-gamut colour; the badge only makes the
  uplift's limitation legible (mirrors [JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md)'s "ship it as-is, the residual is
  the model's limit" stance). No auto-clamp, no value rewrite.

### 4A.5 Shared vs platform

The residual-query helper + the in-gamut predicate are **shared C++** (one const accessor over the baked LUT); the
badge glyph + hover string are platform UI. Because it is a pure read of already-shipped LUT data, D4 adds
essentially no engine work — the reason §1 ranks it #1.

### 4A.6 Android tier

**Tier B** (rides on the D1/material-instance sliders, which are Tier B): the badge is a tiny indicator on the
touch colour chip and the cheap computed swatch, so it carries over for free wherever an RGB colour is editable
on device. No separate Android work beyond drawing the indicator.

---

## 5. D5 — "Explain the auto-router" per-region variance heatmap

### 5.1 What it is

When the integrator is **Auto** (the shipped `auto_rasterizer` / `auto_spectral_rasterizer`,
[AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md)), surface *why* it chose PT/BDPT/VCM: a **per-region
heatmap** of the probe's variance/energy signals plus a one-line rationale ("Auto → BDPT: glossy/indirect σ²·T
high" / "Auto → VCM: dielectric caustic — PT can't reach this energy"). **No competitor can replicate this** —
it requires both a spectral integrator *and* an auto-router, and it turns the router from a black box into a
teaching tool.

### 5.2 Backing feature (shipped — the probe is real and in-process)

The Phase-4 probe is **already implemented** and produces exactly the signals the heatmap needs, per-render,
in-process, with no re-parse/rebuild:

- `AutoRasterizer::RunProbe` / `ProbeCandidate` render PT + (BDPT|VCM) candidates on the assembled scene at
  reduced resolution (`auto_probe_scale`, default ¼) and low spp (`auto_probe_spp`, default 4), reading images
  back through a `ProbeCaptureOutput` ([AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md) §6.2).
- The decision tree's quantities are directly mappable to heat: **median-luminance ratio** (caustic gate 1,
  τ=1.30), **winsorized mean-luminance / transport-reach ratio** (gate 2, τ_reach=1.50), and **per-pixel σ²·T**
  (BDPT gate, τ_bdpt=1.35). The router already logs the resolved choice and the gate values that fired.
- The GUIs already **surface the resolved integrator** ("Auto → PT/BDPT/VCM") after a render
  (CLAUDE.md High-Value Fact); D5 is the *explanatory layer* under that label.

### 5.3 The math + data sourcing

The heatmap is the probe's **already-computed per-pixel candidate images**, not a new render:

- **σ²·T layer (BDPT rationale):** per-pixel variance from the probe's `varianceRenders` (default 2) candidate
  renders, scaled by per-candidate wall-time T. Hot = BDPT-favouring (connections beat PT's per-sample cost
  there). This is the σ²·T quantity the whole integrator matrix is decided on.
- **Caustic-energy layer (VCM rationale):** per-pixel `VCM_luminance − PT_luminance` (positive where VCM reaches
  energy PT misses). Hot = VCM-favouring.
- **Decision overlay:** the scalar gate values (medRatio, robust reach, σ²·T ratio) shown as a small "why"
  card with the firing thresholds, so the rationale string is *backed by the numbers the router used*.

**Data sourcing:** add read-back accessors so `RunProbe` can optionally retain the per-pixel candidate luminance
buffers (today they're consumed to scalars and discarded). This is a **pure-additive** change to `AutoRasterizer`
(no integrator code touched — the byte-identical constraint the dispatcher maintains stays intact); the buffers
already exist transiently inside `ProbeCandidate`. **Confirmed net-new (ground truth, [CURRENT_STATE_AUDIT.md
§11](CURRENT_STATE_AUDIT.md)):** the per-candidate `ProbeResult` struct and the `ProbeFrame`'s per-pixel `lum`
vector are **`private`** ([AutoRasterizer.h:192](../../src/Library/Rendering/AutoRasterizer.h) — the struct
holds only `medianLum / meanLum / robustMeanLum / meanVar / rasSeconds`, all consumed to scalars inside
`RunProbe`/`ProbeCandidate` and discarded). Only the *scalar* surface is public today
(`ResolvedIntegratorName()` / `ResolveReason()` / `LastProbeSeconds()` / `LastProbeRenders()`,
[AutoRasterizer.h:151-168](../../src/Library/Rendering/AutoRasterizer.h)). So D5's heatmap **requires a net-new
additive readback** (retain + expose the per-pixel buffers); it cannot read anything that exists today. (This is
why the audit singles D5 out as stating the surface correctly, vs `MCP_TOOL_SURFACE`'s `rise://render/variance`
which overstates it.)

### 5.4 Pair with RMSE (the σ²·T-rewards-dark-images trap)

**Load-bearing caveat (from memory + [AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md)):** σ²·T
*rewards dark images* — a near-black region has tiny variance and will read "cold/converged" even if it's wrong.
So the heatmap must **not** present σ²·T alone as "quality." Pair it with an **RMSE-vs-reference view** when a
reference (a higher-spp render of the same frame, or the converged final) is available: show σ²·T as the
*router's decision signal* and RMSE as the *actual error*, side by side. Where they disagree (low σ²·T but high
RMSE in a dark region) is exactly the regime the probe's median-not-mean and reach-gate refinements exist to
handle — and showing that disagreement is itself the "explain the router" payoff. Without a reference, label the
σ²·T view honestly as "noise estimate, not error."

**Use a fixed normalization for both layers.** Because σ²·T rewards dark images, the heatmap's colour mapping
must not be per-frame auto-normalized (auto-scaling re-stretches the legend every frame so a converged dark
region and a noisy bright region can read the same hue). Pin the σ²·T and RMSE colour scales to a **fixed,
documented range** (e.g. a shared absolute σ²·T ceiling and a fixed RMSE ceiling), so the heat is comparable
across frames and across the σ²·T-vs-RMSE pair. This is the visualization-side complement to pairing with RMSE:
RMSE is the layer that *corrects* the dark-image bias, fixed normalization is what keeps the *picture* of it
honest.

### 5.5 The widget

- A toggle over the viewport: **Off / Decision heatmap / σ²·T / Caustic-energy / RMSE** (RMSE greyed until a
  reference exists).
- A **"Why this integrator?" card**: the resolved choice + the gate values that fired + the matrix rationale
  sentence (sourced from the routing-policy table, [AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md) §1).
- Honors the existing "Auto → X" label and the pin-override affordance (§7 of the auto-rasterizer doc).

### 5.6 Feasibility / uncertainties

- The probe only runs at **production spp ≳ 256** (`auto_probe_activation_spp`); below that the router uses the
  static Tier-1 guess and there is **no probe heatmap to show**. The card should then explain the *static* rule
  ("dielectric + point light → VCM," else PT) instead — still informative, just not a heatmap. Flag clearly in
  the UI which tier decided.
- **⚠ Reference for RMSE:** "is there a converged reference?" is a UX question — simplest is "render a reference"
  button (a high-spp background render) feeding the RMSE layer; otherwise σ²·T-only with the honest label. **That
  reference render is a long, high-spp, denoise-off full-scene render and MUST route through the
  `RenderCoordinator` as a `RmseReference`-class isolated job** ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md)
  §3.1, §5) — it renders a snapshot scene into a *private* `IFilm`, never the live film, and is preempted by
  interactive edits; it must not be a direct `RasterizeScene` call that double-books the global worker pool
  ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §1.1). The probe itself stays *inside* its owning render
  (`ProbeInternal`, not a separate slot — [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §3.4), so the
  per-pixel-buffer retention this section adds reads buffers that already exist during that render; only the
  *RMSE reference* is a new coordinator job.
- **Shared vs platform:** the probe + per-pixel buffer retention + RMSE compute are shared C++; the overlay
  renderer and the card are platform UI. (Reuse the EDR present path's texture upload for the heatmap layer.)

### 5.7 Android tier

**Tier C** (deferred / desktop-first, like scopes): the heatmap is a diagnostic overlay best on a large display;
Android shows the resolved "Auto → X" label + the static/probe rationale **text card** (Tier A-friendly), but
not the per-pixel heatmap.

---

## 6. D6 — EDR cinematography overlays (scopes)

### 6.1 What it is

Broadcast/cinematography scopes computed over the **true HDR/EDR framebuffer**: **false color** (ARRI/SmallHD
IRE legend), **zebra**, **waveform**, and a **spectral vectorscope** plotting *true CIE xy chromaticity* (not the
fake Cb/Cr circle a video vectorscope draws — RISE has the actual spectrum/XYZ, so it can plot where each pixel
sits on the real chromaticity diagram against the gamut triangle). These make RISE legible to colorists/DPs and
lean directly on the shipped EDR display path.

### 6.2 Backing feature (EDR path shipped)

- The Mac EDR present path is real: `MetalEDRRenderer` drives a `CAMetalLayer` with `pixelFormat =
  .rgba16Float`, `colorspace = extendedLinearSRGB`, `wantsExtendedDynamicRangeContent = true`, fed an `[UInt16]`
  (binary16) staging buffer of the framebuffer ([MetalEDRView.swift](../../build/XCode/rise/RISE-GUI/App/MetalEDRView.swift)).
  The scopes read that same staging buffer — the data is already in hand, per-frame, on the GPU-upload path.
- **EDR headroom is already probed** the right way: `maximumPotentialExtendedDynamicRangeColorComponentValue`
  (capability, not current) via `RenderViewModel.refreshEDRAvailability()` and `RISEBridge.displayMaxEDRHeadroom`
  ([RenderViewModel.swift:382](../../build/XCode/rise/RISE-GUI/App/RenderViewModel.swift),
  [RISEBridge.mm:777](../../build/XCode/rise/RISE-GUI/Bridge/RISEBridge.mm)).

### 6.3 The math + the four scopes

| Scope | Reads | Computes | Note |
|---|---|---|---|
| **False color** | per-pixel luminance | map luminance → IRE band → overlay colour + on-screen IRE legend | Use the **ARRI scale**: Purple 0–2.5, Blue 2.5–4, Green 38–42 (mid-grey ~18%), Pink 52–56 (skin), Yellow 97–99, Red 99–100 IRE ([SmallHD ARRI false color][smallhd-arri]). HDR-aware: above-100-IRE (EDR) bins need their own bands since EDR exceeds SDR white. |
| **Zebra** | per-pixel luminance | diagonal stripes where luminance > threshold | Highlight-clip guide; user-set threshold (commonly ~95–100 IRE for clipping). Cheap, per-pixel. |
| **Waveform** | per-column luminance (or RGB parade) | x = image column, y = luminance distribution | Luminance waveform; optional RGB-parade (three side-by-side) for white-balance ([Videomaker][videomaker-scopes]). |
| **Spectral vectorscope** | per-pixel **XYZ tapped at the film resolve** | scatter pixels on the **CIE 1931 xy diagram** with the spectral-locus horseshoe + Rec.709/P3 gamut triangles overlaid | This is the *spectral* differentiator: a video vectorscope plots encoded Cb/Cr; RISE plots **real chromaticity** because it has XYZ. Saturated/monochromatic pixels land on the locus boundary; whites cluster at D65 ([CIE xy diagram][cie-xy]). Ties directly to D4's gamut story. |

**Tap XYZ at the film resolve, NOT reconstructed from display RGB.** The vectorscope's chromaticity scatter is
*only* a true spectral xy diagram if it reads the integrator's XYZ **before** the resolve converts it to display
primaries. The film resolves XYZ → RISEPel (= Rec.709) exactly once per pixel via `XYZtoRec709RGB`
([FilteredFilm.cpp:88-116](../../src/Library/Rendering/FilteredFilm.cpp) `Resolve`; the `ProgressiveFilm` resolve
mirrors it). Reconstructing XYZ from the already-resolved Rec.709 framebuffer (`Rec709toXYZ`, the matrix inverse,
[Color.cpp:159](../../src/Library/Utilities/Color/Color.cpp)) is **lossy and non-spectral**: the resolve has
already projected the full spectral chromaticity onto the Rec.709 triangle, so any pixel whose true chromaticity
lay *outside* Rec.709 (a saturated monochromatic spike, a dispersive caustic, the deep-blue corner D4 warns
about) gets clamped/projected to the gamut edge before the inverse can run — round-tripping it back can never
recover the out-of-gamut point, and the scatter would collapse onto the Rec.709 triangle, defeating the entire
"plot real chromaticity against the gamut" differentiator. **Requirement:** add an **XYZ-retention** path — the
resolve must be able to emit (or retain) the per-pixel XYZ triple it computes at
[FilteredFilm.cpp:111](../../src/Library/Rendering/FilteredFilm.cpp) (`resolvedXYZ`) as a parallel buffer/AOV the
vectorscope reads, *in addition to* the Rec.709 display buffer. This is a net-new additive readback on the film
resolve (analogous in spirit to D5's probe-buffer retention), not a free read of the present surface. The
waveform / false-color / zebra need only **luminance**, which is recoverable from the display buffer, so they do
**not** require the XYZ retention — only the vectorscope does.

**Colour/exposure domain — define it explicitly per scope.** Scopes are meaningless without a stated reference
domain; specify three:
- **Scene-linear** — the framebuffer is scene-referred linear radiance (the integrator's output, EDR-extended:
  values exceed 1.0). The **vectorscope** operates here (chromaticity is exposure-invariant: xy normalizes out
  magnitude, so it reads scene-linear XYZ directly with no view transform).
- **IRE / waveform / false-color** — these are *exposure* tools and must map a chosen **encoding** to the
  0–100-IRE legend (and the >100-IRE EDR bands, §6.3 false-color note). State whether the IRE axis is over the
  *scene-linear* values or over a *display/PQ-encoded* signal; the ARRI/SmallHD legend (§6.3) is calibrated to a
  specific encoding, so the false-color mapping must declare which (and recompute on EDR-headroom change, §6.4).
- **View transform** — false-color/zebra/waveform should optionally read **through the active view transform / tone
  map** (what the user actually sees) as well as raw scene-linear (what the renderer produced), and the panel must
  label which is active. The vectorscope is *not* view-transformed (a tone map changes luminance, not the
  underlying chromaticity we want to inspect).

### 6.4 Dynamic EDR headroom (the re-query requirement)

**Load-bearing:** EDR headroom is **dynamic** — it changes when the window moves between displays, when
System Settings toggles HDR, and after sleep/wake. The IRE/false-color mapping and the "above-white" bands
**must be recomputed on headroom change**, not cached at launch. macOS delivers
`NSApplicationDidChangeScreenParametersNotification` for exactly this ([Apple AppKit][apple-edr]); the code
already has `refreshEDRAvailability()` — the scopes must subscribe to the same notification and re-derive their
scales. (Known OS wrinkle: on recent macOS, `maximumExtendedDynamicRangeColorComponentValue` can wrongly read
1.0 after sleep/wake on third-party HDR displays — prefer the *Potential* property and re-query, matching the
existing code's Round-3 fix.)

### 6.5 The widget

A scopes panel (dockable / overlay) with the four scopes individually toggleable, the false-color IRE legend
inline, a zebra-threshold slider, and a waveform/parade switch. The vectorscope shows selectable gamut
overlays (Rec.709 / P3 / Rec.2020).

### 6.6 Feasibility / uncertainties

- **Compute location:** false-color/zebra are per-pixel and can run in the **Metal fragment shader** that
  already blits the framebuffer (cheap, live). Waveform + vectorscope are reductions/scatters — do them as a
  **Metal compute pass** over the staging texture (or a downsampled CPU pass at interactive rates). Either way
  the input is the existing `[UInt16]` buffer; no new render.
- **Windows parity:** the Mac path is `CAMetalLayer`; Windows is DXGI/Qt. The **scope math is shared C++**
  (luminance bins, xy scatter, gamut triangles); only the present/compute surface differs (Metal vs D3D/compute).
  Design the scope compute as a platform-agnostic buffer-in / overlay-geometry-out helper.
- **⚠ EDR-headroom OS quirks** (sleep/wake 1.0 bug) — mitigated by the Potential-property + re-query pattern
  already in the codebase; call it out so the scope code doesn't regress it.

### 6.7 Android tier

**Tier C** (deferred / desktop-first, §10.4): scopes are a large-display colorist tool. Android can show a
simple histogram (Tier B-ish) but the full waveform/vectorscope/false-color suite is "edit on desktop,"
presented gracefully (not a broken control).

---

## 7. D7 — Spectral Light Mix

### 7.1 What it is

Render the scene **once** with each tracked light's contribution captured to its own buffer (per-light AOVs —
**bounded Top-K + an Environment row + an "other" overflow bucket**, §7.3 V1, not literally one buffer per scene
light), then let the user **re-balance live after the render** without re-rendering, recompositing from the stored
**raw** per-light layers and **denoising the composite once** (§7.2b). This is V-Ray's Light Mix. Two contracts
make it sound rather than aspirational, both pinned before the tiers: **(a) what each integrator can honestly
attribute per-light** (§7.2a — V1 is **PT-only**, covering NEE + camera-visible emission + the MIS-consistent
BSDF-on-emitter term + a single Environment row; BDPT splats / VCM merges are *out of V1*), and **(b) the denoise
contract** (§7.2b — OIDN is nonlinear, so store raw layers and denoise the composite, never per-layer). **The
re-balance capability then splits into three sharply different tiers (§7.3), and only the first is a sound ship-now
feature:** (V1) exact **intensity-only** per-light re-balance; (optional) **approximate RGB recolor** for
colour-temperature nudges, *explicitly labeled an approximation*; and (research) true **wavelength-resolved SPD
replacement** — swapping a 3200 K tungsten for a measured sodium-vapour SPD and having the bounce light recolour
*physically*. The third is the spectral headline, but it is **not recoverable from aggregated per-light AOVs** the
way intensity is (§7.3 explains why) — do not promise it as a post-process re-balance in v1.

### 7.2 Backing feature

The contribution-per-light concept partly exists in the integrator's light sampling (NEE picks a light and
accumulates its contribution); D7 needs that accumulation **split per light into separate framebuffers**. This
is the heaviest engine lift of the seven (a new multi-buffer render-output mode). **But NEE is not the only way
radiance from a light reaches a pixel — so "per-light attribution" is not free, and what V1 can honestly
attribute is integrator-specific and bounded.** §7.2a pins the V1 attribution contract before §7.3 spends it.

**Retraction (important — the earlier draft overclaimed):** it is **NOT** true that "only contribution binning
changes; the integrator math is untouched." That holds *only* for the intensity-only tier (V1). The moment you
want to re-tint a light by colour temperature or swap its SPD (Tiers 2/3), per-light *RGB* accumulation is
insufficient and the attribution must change: which light a path is even attributed to is decided by
light-selection PDFs *during* transport, the emitter SPD is folded into the throughput *along the path*, and the
information needed to undo-and-replace it is destroyed by the per-pixel RGB sum. So D7's higher tiers require
real per-integrator attribution plumbing, not a post-process re-bin of an unchanged integrator. §7.3 lays out
exactly what each tier costs.

### 7.2a The V1 attribution contract (what each integrator can attribute per-light — and what V1 scopes OUT)

"Attribute this pixel's radiance to the light that produced it" is only well-defined for transport where a single
emitter is identifiable at accumulation time. **NEE is the clean case** (the estimator already picked one light);
several other transport paths are *not*, and they differ per integrator. V1 must state honestly which transport it
bins per-light and which it lumps or drops — otherwise `Σ_i L_i` silently disagrees with the beauty pass and the
re-balance is wrong even at `w_i = 1`.

| Transport / path | Attributable to one light at accumulation? | V1 treatment |
|---|---|---|
| **NEE (direct light sampling)** — PT, and the s≥1·t≥1 explicit connections in BDPT/VCM | **Yes** — the estimator selected light *i*; bin `L_i`. | **Attributed per-light** (the V1 core). |
| **BSDF-sampled ray that randomly hits an emissive surface** (the s=0 / "implicit" emission strategy) | **Partially** — the hit *surface* maps to an emitter, but its weight is MIS-combined with the NEE estimate for the *same* emitter; double-attribution is the trap. | **Attributed to that emitter** *only when* the hit resolves to a known light-bound object **and** the MIS partner is binned to the same light (so NEE + BSDF-on-emitter sum into one row, never two). Otherwise → **"other" bucket** (§7.5). |
| **Environment / IBL radiance** (a BSDF ray escaping to the env map, or env-NEE) | **As one pseudo-light** — the environment is a single emitter, not per-direction. | **One dedicated "Environment" row** (a reserved AOV), not split by direction. Env-NEE and env-escape both fold into it. |
| **Emissive surfaces seen directly by the camera** (primary ray hits a luminaire) | Yes — the camera ray's first hit is that emitter. | **Attributed to that emitter's row** (so turning a visible area light to `w=0` dims its own glow too — matches V-Ray). |
| **BDPT light-subpath connection strategies (t=1 light-image splats, interior connections)** | **Costly** — the light vertex's originating emitter is known, but the splat lands at an *arbitrary* pixel and the per-strategy MIS bookkeeping differs from PT. | **V1 SCOPES THIS OUT for the bidirectional integrators**: D7 V1 is specified for **PT (`pathtracing_*`) only**; under BDPT/VCM the per-light split is **not offered in V1** (the splat/merge attribution is the §7.3-research-class plumbing). The mixer is disabled (with a reason) when the active integrator is BDPT/VCM/MLT. |
| **VCM photon merges** (the merge estimator) | **No** — a merge combines many photons whose originating lights are lost in the map. | **V1 SCOPES THIS OUT** (same as above — VCM not a V1 integrator for Light Mix). |
| **Pel vs NM vs HWSS spectral paths** | The *binning* is representation-specific (RGB sum vs hero-λ bundle). | **V1 attributes in the render's native representation and stores the per-light layer as RGB-resolved** (the V1 re-balance is intensity-only, so RGB layers suffice — §7.3 V1). HWSS bundles resolve to the same RGB layer; **no per-wavelength per-light storage in V1** (that is the §7.3 research tier). |

**The honest V1 scope, stated once:** D7 V1 ships for **PT (Pel and spectral) only**, attributing **NEE + camera-visible emission + the MIS-consistent BSDF-on-emitter term** per light, with **one reserved "Environment" row** and **one "other/unattributed" bucket** that catches everything not cleanly assignable (so `Σ_i L_i + L_env + L_other ≡ beauty` holds by construction). **BDPT / VCM / MLT do not get a V1 per-light split** (their light-subpath/merge attribution is research-class, §7.3). This is a real limitation — surface it in the UI ("Light Mix is available for the Path Tracer; the bidirectional integrators capture light contributions differently") rather than letting the mixer silently mis-attribute.

### 7.2b The denoise contract (OIDN is nonlinear — composite *then* denoise)

**The naive pipeline "denoise each per-light layer, then sum" is wrong**, because OIDN is a **nonlinear** operator:
`denoise(Σ_i w_i·L_i) ≠ Σ_i denoise(w_i·L_i)`. Per-layer denoise would (a) break the `Σ layers == beauty`
invariant the whole feature rests on, (b) denoise each layer at a far worse SNR than the beauty (each light carries
a fraction of the samples), and (c) re-run on every slider drag. **V1 policy:**

- **Store RAW (noisy, un-denoised) per-light layers.** The K per-light AOVs (+ Environment + other, §7.3 V1) are
  the linear, separable radiance estimates — denoise touches none of them.
- **Re-balance composites the raw layers** (`Σ_i w_i·L_i + w_env·L_env + L_other`) — exact and linear, live on
  the slider (GPU), no denoise in the loop.
- **Denoise is applied once, to the COMPOSITE**, at commit (slider release) or for the final/presentation frame —
  `denoise(composite)`, the single nonlinear step, consistent with the [denoise-always-on-for-finals
  policy](../../CLAUDE.md). Optionally cache albedo/normal AOVs once (they're light-independent) so the composite
  denoise reuses them.

This is the "store raw per-light layers, composite, then denoise the composite" contract. It also means the
mixer's live preview is the *raw composite* (slightly noisy) and the committed/final frame is the denoised
composite — set that expectation in the UI.

### 7.3 The three tiers (sharply different cost and soundness)

The three re-balance operations are **not** points on one continuum where "more storage = more fidelity"; they
sit on three different sides of what aggregated per-light AOVs can and cannot represent. Label them as such in
the UI so the approximation is never mistaken for the real thing.

#### V1 — exact intensity-only re-balance (sound; ship this)

- **Scope:** **PT only**, attributing the transport §7.2a lists (NEE + camera-visible emission + the
  MIS-consistent BSDF-on-emitter term), with a reserved **Environment** row and an **other/unattributed** bucket.
  BDPT/VCM/MLT are out of V1 (§7.2a).
- **Render:** accumulate `L_i(pixel)` per attributed light *i* into per-light framebuffers, **bounded by a hard
  cap K** (§7.5): the **Top-K brightest contributors get their own row; everything beyond K folds into the single
  "other" bucket** alongside the Environment row. So the storage is **K per-light layers + Environment + other**,
  *not* literally one framebuffer per light in the scene — this resolves the "one row per light vs Top-K" tension:
  the UI shows one row per *tracked* light (the Top-K) plus an aggregated **"Other lights"** row (read-only, not
  re-balanceable) and an **Environment** row. The invariant `Σ_{i≤K} L_i + L_env + L_other ≡ beauty` holds by
  construction. **Which K lights are "tracked" is decided *before* the binning render by the selection mechanism
  of §7.3a — never re-decided mid-render.**
- **Layers are stored RAW (un-denoised)**; the composite is denoised once (§7.2b).
- **Re-balance:** sliders drive the scalar `w_i` for the K tracked lights (+ Environment); the composite
  `Σ_{i≤K} w_i · L_i + w_env · L_env + L_other` recomposites the raw buffers per frame (cheap, GPU), then the
  committed/final frame denoises the composite (§7.2b). Scaling a tracked light's *intensity* is exact because
  intensity is linear and separable — `w_i · L_i` *is* the contribution at the new intensity, with no
  approximation. This is the V-Ray Light Mix feature and it is genuinely post-process: per-light **storage +
  attribution binning** is the new engine cost, but the PT integrator's radiance *math* is untouched (the one
  place the retracted claim in §7.2 actually holds). **This is the v1 — sound and useful, with the honest scope of
  §7.2a (PT-only, Top-K + Environment + other) and the denoise contract of §7.2b; not "no caveats."**

##### 7.3a How the Top-K is selected (the chicken-and-egg the V1 design must close)

The §7.3 V1 allocates K per-light layers to the **"brightest contributors,"** but **brightness is only known *after*
accumulation** — and the K layers must be chosen *before* the binning render allocates them. Leaving this implicit is
the gap [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #7 flags ("Light-Mix Top-K needs a pilot pass"). V1 closes it with
**one of two up-front mechanisms**; both decide the tracked set once, before the full binning render:

1. **Pilot / scout pass (recommended).** Run a **cheap low-spp, low-resolution probe render first** (reuse the
   `auto_probe_scale` / low-spp machinery the auto-router already uses — [AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md)
   §6.2) that accumulates *only a scalar* integrated-luminance estimate per light (no per-pixel per-light buffers — the
   pilot is cheap precisely because it bins to scalars, not framebuffers). **Rank lights by that estimate, freeze the
   Top-K, then run the full binning render** allocating exactly K per-light layers + Environment + other. The pilot is a
   second render pass, so like any production-class render it routes through the `RenderCoordinator`
   ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §3.1), not a direct `RasterizeScene`.
2. **Stable preselection metric (cheaper, no pilot).** Rank lights up front by a **scene-static proxy** computed before
   any render — e.g. **emitter radiant power** (`power` × emitter area, already available from the light chunks) optionally
   weighted by **proximity / solid angle to the camera** — freeze the Top-K from that, and run a single binning render. No
   pilot pass, at the cost of occasionally tracking a powerful-but-occluded light (which simply contributes little and can
   be dropped to the "other" bucket on a re-run). Use this when the pilot's extra pass isn't worth it.

A third shape — **track *all* lights with scalar statistics during one pass, then collapse to Top-K** — is acceptable
only if "all lights" means *scalar* per-light accumulators (cheap), **not** all-light *framebuffers* (that defeats the K
cap and the §7.5 memory budget). It is effectively the pilot pass folded into the main render's scalar bookkeeping; if
used, the per-pixel per-light *layers* are still allocated only for the Top-K after the scalar ranking settles.

**Forbidden: dynamic mid-render replacement of a tracked light.** The tracked Top-K is **frozen for the whole binning
render**. RISE must **never** swap one light out of a per-light layer for another partway through accumulation — doing so
would **mix two different lights' radiance into one buffer**, destroying per-light attribution and silently breaking the
`Σ_{i≤K} L_i + L_env + L_other ≡ beauty` invariant (the buffer would no longer correspond to *any* single light). If the
pilot's ranking proves wrong (an important light landed in "other"), the fix is to **re-run the binning render with the
corrected Top-K**, not to hot-swap a layer. The "other" bucket and the Environment row are the only non-tracked sinks,
and both are read-only aggregates by construction (§7.3 V1).

**Cost/effort impact on V1 (reflected in §7.5 and §8.1).** The V1 engine work is therefore **the per-light accumulator
(Top-K + Environment + other) + the PT attribution binning (§7.2a) + the GPU recompositor with a single composite-denoise
(§7.2b) + the Top-K selector** — a scalar-ranking **pilot pass** (mechanism 1) *or* a static-power preselection
(mechanism 2). The pilot adds one cheap probe-class render before the binning render (a few percent of the binning render's
cost at low spp/res, coordinator-scheduled); the static metric adds essentially nothing. Either way the selector is **a
bounded, up-front step**, not per-frame and not mid-render — it does not change the post-render re-balance being pure,
linear GPU compositing.

#### Optional — approximate RGB recolor (label it an approximation)

- Store `L_i` as RGB; a colour-temperature change multiplies `L_i` by the ratio of the new vs old light's RGB (a
  chromatic-adaptation-style per-channel scale). Cheap; an RGB tool could *almost* do this too.
- **It is an approximation, and must be labeled one in the UI.** A single per-light RGB scale cannot reproduce the
  true result of re-emitting at a different SPD, because the surfaces the light hit have wavelength-dependent
  reflectance — recolouring by a flat RGB ratio ignores how the *new* SPD interacts with each surface's
  spectrum (a warm SPD on a surface that reflects strongly only in a narrow band recolours differently than a
  flat RGB tint predicts). Useful for a quick warm/cool nudge; never sold as physically correct. Optional, gated
  behind an explicit "approximate" affordance.

#### Research — wavelength-resolved SPD replacement (NOT recoverable from aggregated hero-λ AOVs)

The headline "swap a tungsten for a measured sodium-vapour SPD and the bounce light recolours physically" is a
genuine spectral differentiator — and it is **not** achievable by storing a richer per-light AOV and re-weighting
it after the fact. The reasons compound:

1. **Hero wavelengths vary per path.** RISE carries 4 *stochastic* hero λ per path
   ([THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md) §3), drawn differently for each path and resolved to
   XYZ as a Monte-Carlo estimate. A per-light AOV is a *sum over many paths each carrying different wavelengths* —
   there is no fixed per-pixel spectrum to re-weight; the spectral axis was already integrated away stochastically
   at accumulation time. You cannot factor a single emitter SPD back out of a hero-λ-randomized sum.
2. **Emission *and* light-selection PDFs are entangled in the estimate.** Which light a path is attributed to is
   chosen by light-selection PDFs *during* transport, and the emitter SPD enters the path throughput folded
   together with those sampling weights. Replacing the SPD changes both the radiance *and* the optimal sampling —
   a faithful result needs the contribution re-estimated under the new emitter, not the old estimate rescaled.
3. **Zero-support spectra can't be reweighted.** If the original emitter SPD had ~zero power at a wavelength where
   the new SPD has energy (e.g. tungsten → a near-monochromatic sodium line), the stored contribution carries no
   samples there to scale up — the information was never gathered. A reweight of an empty bin stays empty;
   recovering it requires actually tracing transport at the new wavelengths.
4. **It needs per-integrator *and* per-domain attribution plumbing.** Doing this correctly means storing per-light
   *separable per-wavelength throughput* (the path throughput *without* the emitter SPD folded in) and
   re-integrating it against the new SPD → CMF → display. That attribution has to be threaded through **each
   integrator** (PT NEE, BDPT connection strategies, VCM merges — the s,t-strategy bookkeeping differs per
   integrator) **and** through the **Pel / NM / HWSS** paths (the spectral-bundle representations differ; cf. the
   HWSS env-IBL bias arc in CLAUDE.md). This is a substantial cross-integrator engine workstream, not an AOV add.

  **Verdict:** Tier-V1 ships. The "approximate RGB recolor" is an *optional, explicitly-approximate* extra. The
  wavelength-resolved SPD swap is **research-flagged** (§9), not a v1 post-process re-balance — promising it as
  "re-balance after the render" would be the same kind of overclaim §7.2 retracts.
- **Data sourcing:** when the SPD-swap research tier is eventually built, the new emitter SPD comes straight from
  D1's picker (Kelvin slider / CSV / spectral_painter) — D7 and D1 share the colour-input widget. The
  *intensity* V1 needs no D1 dependency.

### 7.4 The widget

A "Light Mix" panel: **one row per *tracked* light** — the **Top-K brightest contributors** (§7.3 V1 / §7.5),
each with name, on/off, **intensity slider** (the V1 control) — plus a reserved **Environment** row (its own
slider) and a read-only **"Other lights"** row (the aggregated overflow beyond K; not re-balanceable, shown so the
user sees what the cap excluded). The live composite in the viewport is the **raw composite** (slightly noisy);
the committed/final frame is the **denoised composite** (§7.2b). A "**bake to scene**" button writes the chosen
weights back into the light chunks via `SceneEditController` (principle 6 — the re-balance becomes durable scene
edits, not a hidden compositor state). The panel is **disabled with a reason when the active integrator is
BDPT/VCM/MLT** ("Light Mix is available for the Path Tracer", §7.2a). The **color-temp slider** appears only behind
the *optional* approximate-RGB-recolor affordance and is clearly badged "approximate"; an **SPD-swap button**
(reusing D1) appears **only if/when the research tier is built** — it is not a v1 row. Until then, a true SPD
change is a scene edit + re-render, not a Light-Mix re-balance.

### 7.5 Feasibility / uncertainties

- **Memory:** the V1 cost is **K per-light RGB layers + 1 Environment layer + 1 "other" overflow layer** — a hard
  cap, not one buffer per scene light (§7.2a, §7.3 V1). Pick K (e.g. 8–16); lights beyond K fold into the read-only
  "other" bucket, preserving `Σ layers ≡ beauty`. The research SPD-swap tier would additionally need
  per-wavelength-separable storage (× hero-λ count and the un-folded-SPD throughput), a far larger budget — another
  reason it is research, not v1.
- **Top-K selection (§7.3a) — settled, not hand-waved.** Which K lights get a layer is decided **up front**, before
  the binning render, by either a **scalar-luminance pilot/scout pass** (recommended; cheap low-spp/low-res probe that
  ranks lights, then freeze) or a **scene-static preselection metric** (emitter power × area, optionally proximity to
  camera — no pilot). The set is **frozen for the whole binning render**; **dynamic mid-render replacement is
  forbidden** (it would mix two lights into one buffer and break `Σ layers ≡ beauty`). A wrong pilot ranking is fixed by
  **re-running** the binning render with the corrected Top-K, never by hot-swapping a layer. *(Open UX detail: the exact
  K and how often a scene's important lights exceed it — §9 #4 — is tuning, not a soundness question now that the
  selector is specified.)*
- **Attribution coverage is integrator-specific (§7.2a):** V1 is **PT-only**. The clean NEE bin is augmented with
  camera-visible emission + the MIS-consistent BSDF-on-emitter term so `Σ layers` matches the beauty; BDPT
  splat/interior-connection and VCM merge attribution are **out of V1** (research-class). The mixer is disabled
  with a reason under BDPT/VCM/MLT rather than mis-attributing.
- **Denoise (§7.2b):** layers are stored **raw**; the composite is denoised **once** (`denoise(Σ w_i·L_i)`),
  because OIDN is nonlinear (`denoise(Σ) ≠ Σ denoise`). Per-layer denoise is never done — it would break the
  composite invariant and denoise each layer at a fraction of the beauty's SNR. Live preview = raw composite;
  committed/final = denoised composite.
- **⚠ The SPD-swap (research) tier needs cross-integrator per-wavelength attribution plumbing** that does not
  exist (§7.3) — flag as the gating engine work; it is *not* "spectral AOV plumbing" you can bolt on, it is
  per-integrator + per-Pel/NM/HWSS attribution. **The intensity V1 ships with only the Top-K selector (§7.3a:
  scalar-luminance pilot pass or static-power preselection) + the bounded per-light accumulator (Top-K + Environment +
  other, §7.3 V1) + the PT attribution binning of §7.2a + a GPU recompositor with a single composite-denoise (§7.2b) —
  no spectral plumbing at all.**
- **Shared vs platform:** the per-light accumulation and the recompositor are shared C++; the mixer panel is
  platform UI. (The spectral re-integration belongs only to the unbuilt research tier.)
- **Render path:** the per-light multi-buffer pass is a full-scene production-class render; like any production
  render it routes through the `RenderCoordinator` ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §3.1) rather
  than a direct `RasterizeScene`. The *re-balance* is pure compositing and touches no render slot.

### 7.6 Android tier

**Tier B** for the *consumer* side (intensity sliders + temp on a pre-rendered light-mix set is a great
"talk-to-your-renderer-from-your-phone" demo); the *producing* render (the multi-buffer pass) is desktop-driven.

---

## 8. Cross-cutting: shared C++ vs platform, and the swatch-cost spike

### 8.1 Shared vs platform (principle 2)

| Concern | Shared C++ (`src/Library/`) | Platform (thin shell) |
|---|---|---|
| Spectrum → XYZ → RGB swatch, **deterministic dense-λ integration** (D1/D2/D3) | `SpectralPacket::GetXYZ` (fixed-grid CMF integral) + `XYZtoRec709RGB` (exist) | the swatch view |
| JH gamut-residual query + in-gamut test, **uplift-only** (D4) | const accessor over the baked `rec709.coeff` LUT (a tiny helper) | corner badge + hover string |
| Sellmeier eval, Abbe, named-glass table (D1) | `SellmeierScalarPainter` (exists) + a small helper | dropdown + curve plot |
| n,k library + format validator (D2) | bundled `colors/metals/` + a loader-discipline check | dropdown + import dialog |
| Airy reflectance at angle (D3) | `ThinFilm::ReflectanceConductor` (exists) | two sliders + ladder strip |
| Probe signals + per-pixel buffer retention (net-new, `ProbeResult` is private) + RMSE (D5) | `AutoRasterizer::RunProbe` (exists) + additive read-back; RMSE reference is a `RenderCoordinator` job | heatmap overlay + "why" card |
| Scope math: IRE bins, xy scatter, gamut triangles + **XYZ-retention at film resolve** (D6 vectorscope) | new platform-agnostic buffer-in/overlay-out helper + additive XYZ readback on `FilteredFilm::Resolve` | Metal/D3D compute + overlay |
| Per-light **RGB** AOV accumulate (PT attribution binning §7.2a; Top-K + Environment + other §7.3) + **Top-K selector** (scalar pilot pass or static-power preselection, §7.3a) + recompositor + single composite-denoise §7.2b (D7 **V1**) | new render-output mode + bounded accumulator + up-front Top-K selector + recompositor | mixer panel (PT-only; disabled under BDPT/VCM/MLT) |
| Per-wavelength-separable per-light throughput + per-integrator re-integration (D7 **research** only) | cross-integrator + per-Pel/NM/HWSS attribution — **not built** | — |
| Painter chunk read/write for all of the above | `SceneEditController` (one mutation path, principle 6) | — |

The pattern holds throughout: **the physics/painter construction is shared and mostly already written; the
widget is the only per-platform part.** D5/D6/D7-V1 each add one shared engine helper (probe read-back + an RMSE
reference job / scope compute + XYZ-retention / per-light RGB AOV); D1/D2/D3/D4 add essentially none. Only D7's
*research* SPD-swap tier is a large cross-integrator engine workstream (§7.3).

### 8.2 The live-spectral-swatch spike, resolved (§13 #1)

The roadmap's open spike — is a live spectral preview affordable on a spectral PT? — splits cleanly:

- **Flat colour swatch (D1 Kelvin/λ/CSV, D2 metal):** *not a render* — a spectrum→XYZ→RGB compute, microseconds,
  **live on drag**. No path tracer involved.
- **Closed-form BSDF swatch (D3 thin-film at angle):** a per-hero-λ Fresnel eval, **live on drag**.
- **Actual transport preview (D1 prism, full-object thin-film rim):** a small cached thumbnail re-rendered on
  *commit* (slider release), as a **`RenderCoordinator` isolated `Thumbnail`/`NodePreview` job** (snapshot scene
  + private film, [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §5.2) — *not* live during drag, and *not* a
  borrow of the live film. (The auto-router's live-film `ResizeFilm` fast path is **not** the model to copy here;
  see [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §5.1/§5.5 and §2.5 above.)

So the answer for these features is **"live where it's a compute, commit-time isolated-job thumbnail where it's
transport,"** which is a stronger position than the general A3 hover-preview question (most spectral controls here
never need a path trace at all), and it composes with the single-render-slot invariant the coordinator owns.

---

## 9. Open questions / uncertainties (flagged, not resolved)

1. **D2 citation obligation** — CC0 lets us bundle the constants freely (settled), but the underlying-paper
   citations must travel with the data (mirror `colors/thinfilm/README.md`'s per-file provenance). Procedural,
   low-risk.
2. **D5 RMSE reference** — needs a "render reference" affordance or honest σ²-only labeling; pin down the UX. The
   reference render itself is a `RmseReference`-class isolated job on the `RenderCoordinator`
   ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §3.1) — that part is settled, the open piece is only the UX.
3. **D5/D7 only at high spp / multi-buffer** — D5's probe is a ≳256-spp tool; D7's per-light pass is a distinct
   render mode. Both are final-render features, not interactive — set expectations.
4. **D7 V1 scope is bounded and PT-only; SPD-swap is research, not a v1 AOV add.** The V1 attribution contract
   (§7.2a) ships for **PT only** — NEE + camera-visible emission + the MIS-consistent BSDF-on-emitter term + one
   Environment row + an "other" overflow bucket, with a **Top-K** cap on per-light rows (§7.3 V1) — and **BDPT /
   VCM / MLT are out of V1** (splat/merge attribution is research-class). Layers are stored **raw** and the
   composite is denoised **once** (§7.2b — OIDN is nonlinear). The wavelength-resolved re-tint (the real
   differentiator) is **not recoverable from aggregated per-light AOVs** (§7.3: hero-λ vary per path,
   emission/selection PDFs are entangled, zero-support spectra can't be reweighted) and needs cross-integrator +
   per-Pel/NM/HWSS attribution plumbing that does not exist. **V1 ships exact intensity-only re-balance; the
   approximate RGB recolor is an optional, explicitly-labeled extra.** Do not promise SPD swap as a post-process
   re-balance. **The Top-K *selection mechanism* is now specified (§7.3a)** — chosen up front by a scalar-luminance
   **pilot/scout pass** (recommended) or a **static-power preselection metric**, frozen for the whole binning render,
   with **dynamic mid-render replacement forbidden** (it would mix two lights into one buffer and break
   `Σ layers ≡ beauty`); a wrong ranking is fixed by re-running, not hot-swapping. *(Open detail reduced to: the exact
   K, and how often a scene's important lights exceed it and need the "other" bucket — a UX-tuning question, not a
   soundness one.)*
4a. **D6 vectorscope XYZ-retention** — a true spectral xy scatter requires tapping the integrator's XYZ at the
   film resolve (before `XYZtoRec709RGB`, [FilteredFilm.cpp:111](../../src/Library/Rendering/FilteredFilm.cpp)),
   a net-new additive readback; reconstructing from display RGB collapses out-of-Rec.709 chromaticities and is
   non-spectral (§6.3). The waveform/false-color/zebra do not need it.
5. **Fluorescence / reradiation — FUTURE, not v1.** A true fluorescent/reradiating material (absorb at λ₁, emit
   at λ₂ — a Donaldson-matrix / bispectral model) is a *different* spectral capability and **likely does not
   exist in the engine today** (the painters here are all single-λ-in/single-λ-out; `GetColorNM` returns the
   value *at* nm, no cross-wavelength transfer). A "fluorescent picker" would be a compelling future D-series
   addition but must **not** be promised in v1 — flag it as a research item, not a widget. (Cf. the
   fluorescent-material literature surfaced during research, [arXiv:2505.19672][fluor].)
6. **Windows scope parity** — the scope *math* is shared, but the compute surface (Metal vs D3D) needs a
   platform-agnostic helper interface designed up front (§6.6).

---

## 10. Non-goals

- **Re-implementing the painters.** D1–D2 are UIs over `spectral_painter` / `blackbody_painter` /
  `scalar_painter` (sellmeier + file) — all shipped on master. D3 is a UI over `fresnel_mode thinfilm`, which is
  **branch-only** (`feature/thin-film-interference`, not yet merged — §4.2). No new painter type either way.
- **A new Fresnel/material model for D3.** The thin-film material exists **on the `feature/thin-film-interference`
  branch (not master — §4.2)**; D3 is a slider over it, not new optics, and ships only once that branch lands. Do
  not add a `GetColorNM` to `IScalarPainter` (the whole point of that interface is the *absence* of JH uplift —
  CLAUDE.md `IScalarPainter` fact).
- **Touching integrator code for D5.** The auto-router maintains a byte-identical-to-HEAD integrator constraint;
  D5 is read-back of probe signals only, never an integrator change.
- **Re-rendering for D7 V1 re-balance.** The V1 intensity re-balance is composite-from-stored-AOVs; if a change
  needs a re-render (new geometry, new light) that's outside Light Mix.
- **Promising a post-process SPD swap in D7 v1** (§7.3 research tier) — wavelength-resolved emitter replacement is
  not recoverable from aggregated per-light AOVs; only exact *intensity* re-balance (and an explicitly-approximate
  RGB recolor) ship without a re-render. Do not market the SPD swap as a Light-Mix slider.
- **Per-layer denoise in D7 (§7.2b).** OIDN is nonlinear (`denoise(Σ) ≠ Σ denoise`); never denoise the individual
  per-light layers. Store them raw and denoise the **composite** once (live preview = raw composite, committed/final
  = denoised composite). Per-layer denoise would break `Σ layers ≡ beauty` and run at a fraction of the beauty's SNR.
- **A D7 V1 per-light split under BDPT / VCM / MLT (§7.2a).** V1 is **PT-only**; the bidirectional integrators'
  light-subpath splats and photon merges don't carry clean per-light attribution. Disable the mixer with a reason
  under those integrators rather than producing a silently mis-attributed split.
- **One per-light framebuffer per scene light in D7 V1.** Storage is **bounded**: Top-K contributors + one
  Environment row + one "other" overflow bucket (§7.3 V1 / §7.5), not an unbounded buffer-per-light.
- **Dynamic mid-render replacement of a tracked light in D7 V1 (§7.3a).** The Top-K is chosen **up front** (scalar
  pilot pass or static-power preselection) and **frozen for the whole binning render**. Never swap one light out of a
  per-light layer for another partway through accumulation — it mixes two lights' radiance into one buffer and breaks
  `Σ layers ≡ beauty`. A mis-ranked pilot is corrected by **re-running** the binning render, not by hot-swapping.
- **A video-style (Cb/Cr) vectorscope.** RISE has real XYZ — the vectorscope plots *true CIE xy chromaticity*
  against the spectral locus, not an encoded chroma circle, and it reads XYZ tapped **at the film resolve**, not
  reconstructed from display RGB (§6.3). (Doing either fake would forfeit the differentiator.)
- **A D4 gamut warning on native-spectral inputs.** D4 is **uplift-only** (§4A.2); a spectral/scalar painter slot
  never shows it. No false positives on physically-authored spectra.
- **Borrowing the live film for a swatch/thumbnail/preview render.** The auto-router probe's live-film
  `ResizeFilm` round-trip is *not* a reusable thumbnail architecture; isolated previews use a snapshot scene +
  private `IFilm` via the `RenderCoordinator` ([RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) §5.1).
- **Promising fluorescence in v1** (§9 #5) — future research, not a shipped widget.
- **Caching EDR headroom at launch** — it's dynamic; re-query on display change (§6.4).

---

## 10A. Acceptance criteria (GUI_ROADMAP §15)

(Numbered `10A` to slot before References without renumbering §11.) The §15 template, filled in for the D-series.
Most of D1–D4 is "wire a widget to a shipped painter/LUT," so their bars are light; D5/D6/D7 each carry the
net-new engine read-back this spec identified.

- **Tests** —
  - *Swatch evaluator (D1/D2/D3):* a unit test that a monochromatic ~5 nm `spectral_painter` swatch resolves to a
    **stable** non-black RGB via the deterministic dense-λ `GetXYZ` integral (§2.5) — and that re-running it is
    bit-stable (no stochastic hero-sample aliasing). Guards "swatch is deterministic, not a 4-hero draw."
  - *D4 uplift-only scope:* assert the gamut badge fires for a deep-blue **RGB** uplift cell (residual > the LUT
    tolerance, [JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md)) and **never** fires for a native `spectral_painter` /
    `blackbody_painter` / `scalar_painter` slot (§4A.2). Guards "no false positive on spectral input."
  - *D5 probe read-back (net-new):* a test that the additive per-pixel buffer accessor returns the same scalars
    `RunProbe` already computes (`medianLum`/`robustMeanLum`/`meanVar`), and that enabling it leaves the resolved
    integrator + the production render **byte-identical** (the [AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md)
    byte-identical-integrator invariant). Engine-touching ⇒ byte-identity is the correctness invariant.
  - *D6 XYZ-retention (net-new):* a test that the retained per-pixel XYZ at the resolve
    ([FilteredFilm.cpp:111](../../src/Library/Rendering/FilteredFilm.cpp)) round-trips a known monochromatic pixel
    to a chromaticity **on the spectral locus** (not clamped onto the Rec.709 triangle as a display-RGB
    reconstruction would, §6.3). Guards "vectorscope is truly spectral."
  - *D7 V1 exactness + attribution closure:* a **PT** test that the **raw** composite
    `Σ_{i≤K} w_i·L_i + w_env·L_env + L_other` at all `w=1` reproduces the single-shot **pre-denoise** beauty to MC
    noise (the §7.2a `Σ layers ≡ beauty` invariant — exercises NEE + camera-visible emission + the MIS-consistent
    BSDF-on-emitter term + Environment + the overflow bucket, *not* NEE alone), and that scaling one `w_i` equals a
    re-render at that light's new intensity (intensity is linear/separable, §7.3). Beyond-K lights must land in the
    "other" bucket, not vanish. No test asserts SPD-swap recovery — it is explicitly out of v1.
  - *D7 V1 denoise contract (§7.2b):* a test that `denoise(composite)` is applied **once to the summed composite**,
    and that per-layer denoise is **not** in the pipeline — assert `denoise(Σ layers) ≠ Σ denoise(layer)` on a
    noisy fixture (OIDN nonlinearity) so a regression that denoises layers individually is caught.
  - *D7 V1 integrator gating:* a test that the per-light split is offered for `pathtracing_*` and **refused /
    disabled** for `bdpt_*` / `vcm_*` / `mlt_*` (§7.2a) rather than silently producing a mis-attributed split.
  - *D7 V1 Top-K selection (§7.3a):* a test that the tracked Top-K is chosen **before** the binning render (by the
    scalar pilot pass or the static-power metric) and that on a scene with > K lights the **brightest K** get their own
    layer while the rest land in "other"; assert the tracked set is **frozen for the render** (no API path swaps a
    light into an existing per-light layer mid-accumulation) — a regression that hot-swaps a layer must break the
    `Σ layers ≡ beauty` check. Guards "Top-K is up-front and stable, never dynamically replaced."
- **Platform parity** — All shared physics/queries are C++ ⇒ macOS / Windows identical by construction; the
  widgets land **desktop-first**. Android: D1/D2/D3 sliders + D4 badge + D7-V1 intensity sliders are **Tier B**
  (touch-native, computed swatch); D5 heatmap and D6 scopes are **Tier C** (desktop-first; Android shows the
  "Auto → X" text card / a simple histogram, presented gracefully, §5.7/§6.7). Degradations are all "edit on
  desktop," never a broken control.
- **Performance budget** — D1/D2/D3 swatches are **microsecond computes, live on slider drag** (§8.2); the D1
  prism / D3 full-object previews are commit-time isolated-job thumbnails (no interactive-frame cost). D4 is a
  const LUT read per pick (negligible). D5/D6 overlays are read-only over existing buffers (Metal fragment/compute
  or a downsampled CPU pass, §6.6). **No production-render regression** — the only engine additions (D5 probe
  buffer retention, D6 XYZ retention) are additive readbacks gated off by default; the L8 ~0.4% production bar is
  the ceiling and these paths are off the hot render loop. D7 V1's per-light accumulation is the one measurable
  render-time cost (writes to **K per-light + Environment + other** layers) — bound by capping **K** (§7.5); the
  **Top-K selector** (§7.3a) adds either a cheap scalar-luminance **pilot pass** (one low-spp/low-res probe-class
  render before the binning render, coordinator-scheduled) or **zero** extra render if the static-power preselection is
  used — both are bounded, up-front, and not per-frame. The re-balance itself is GPU compositing of raw layers with a
  single composite-denoise (§7.2b), off the render loop.
- **Memory budget** — D1–D4: negligible (swatch buffers + the already-baked LUT). D5: per-pixel candidate buffers
  retained only while the heatmap is shown (bounded by probe resolution `auto_probe_scale`, default ¼). D6: one
  extra full-res XYZ buffer (3× fp per pixel) only while the vectorscope is on. D7 V1: **K per-light RGB layers +
  1 Environment + 1 "other" overflow layer** — a hard cap on K (top-K brightest contributors, rest lumped into
  "other", §7.2a/§7.5), *not* one framebuffer per scene light; layers stored raw, one composite-denoise (§7.2b).
  The D7 research tier's per-wavelength storage is explicitly *not* in the v1 budget.
- **Accessibility** — sliders keyboard-reachable with numeric entry (no drag-only); the D4 badge and any scope
  state must not be **colour-only** (pair the badge with an icon/label, the false-color legend with text IRE
  values); curve/SPD plots need a text/numeric read-out alternative. No numpad dependence.
- **Packaging** — D2 bundles `colors/metals/*.{n,k}` (CC0 constants; **per-file provenance README** mirroring
  `colors/thinfilm/README.md` — §3.5, §9 #1). No other seed assets. Any new shared `.cpp/.h` (D5 read-back
  helper, D6 scope-math helper, D7 accumulator) must be registered in **all five build projects** per CLAUDE.md
  (Filelist, Android cmake, VS2022 `.vcxproj` + `.filters`, Xcode pbxproj). New C-ABI entry points go to the END
  of `RISE_API.h` (ABI rule; the [abi-preserving-api-evolution](../../.claude/skills/abi-preserving-api-evolution/SKILL.md)
  skill).
- **Migration** — **No scene-format change** (D1–D3 write painter chunks the parser already accepts; D4–D6 are
  read-only over render outputs; D7 V1 "bake to scene" writes existing light-chunk fields). **No public-ABI
  break** if the D5/D6/D7 read-backs are introduced as additive accessors/overloads rather than signature changes
  (follow the [abi-preserving-api-evolution](../../.claude/skills/abi-preserving-api-evolution/SKILL.md)
  discipline). Caveat: D3 thin-film facts are on the unpushed `feature/thin-film-interference` branch, not master
  ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) punch-list) — D3 ships only once that branch lands.
- **Rollback** — Each widget is independently toggleable and additive; removing a D-feature widget leaves the
  shipped painters/LUT/probe untouched. The engine read-backs (D5 probe buffers, D6 XYZ retention, D7
  accumulation) are **off by default** and gated, so disabling them restores the exact current render path and
  keeps every existing test green. No saved `.RISEscene` depends on any D-feature.

---

## 11. References

### RISE internal
- [GUI_ROADMAP.md](../GUI_ROADMAP.md) §8 (this spec's parent), §10.4 (Android tiering), §13 (spikes), §15
  (acceptance template — filled in at §10A), §16 (confirmed `pipe`-field decision, relevant to D4's IPainter-vs-
  IScalarPainter scoping).
- [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) — code-verified ground truth (supersedes any `Status:` header
  here); §11 confirms the `ProbeResult` per-pixel buffers are **private** (D5 net-new readback) and the punch-list
  notes D3 thin-film is on the unpushed `feature/thin-film-interference` branch.
- [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) — the isolated preview/thumbnail job model + single render slot;
  D1 prism / D3 full-object previews, D5 RMSE reference, and D7's multi-buffer pass all route through it (§2.5,
  §5.6, §7.5, §8.2).
- [JH_LUT_GAMUT.md](../JH_LUT_GAMUT.md) — the blue-corner uplift residual (~3.9 % gamut-corner cells, `1 × 10⁻⁴`
  acceptance tolerance) and the uplift-only nature of JH (D4 basis).
- [THIN_FILM_INTERFERENCE.md](../THIN_FILM_INTERFERENCE.md) — `fresnel_mode thinfilm`, `film_*` slots, exact
  spectral path, introspection, the refractiveindex.info bundling precedent (D2/D3).
- [AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md) — the probe + per-pixel signals + σ²·T-rewards-dark
  caveat (D5); [AutoRasterizer.h:151-200](../../src/Library/Rendering/AutoRasterizer.h) (public scalar surface
  vs the private `ProbeResult` at `:192`).
- Painters: [SpectralColorPainter.h](../../src/Library/Painters/SpectralColorPainter.h)
  ([.cpp:22](../../src/Library/Painters/SpectralColorPainter.cpp) `GetXYZ` swatch path,
  [.cpp:43](../../src/Library/Painters/SpectralColorPainter.cpp) `ValueAtNM`),
  [BlackBodyPainter.h](../../src/Library/Painters/BlackBodyPainter.h)
  ([.cpp:160](../../src/Library/Painters/BlackBodyPainter.cpp) `GetXYZ`),
  [SellmeierScalarPainter.h](../../src/Library/Painters/SellmeierScalarPainter.h),
  [PiecewiseLinearScalarPainter.h](../../src/Library/Painters/PiecewiseLinearScalarPainter.h).
- Material: [GGXMaterial.h](../../src/Library/Materials/GGXMaterial.h) (conductor + thin-film slots).
- Film resolve (D6 XYZ-retention point): [FilteredFilm.cpp:88-116](../../src/Library/Rendering/FilteredFilm.cpp)
  (`Resolve`, `XYZtoRec709RGB` at `:94`, `resolvedXYZ` at `:111`); inverse
  [Color.cpp:159](../../src/Library/Utilities/Color/Color.cpp) (`Rec709toXYZ`, the lossy reconstruction to avoid).
- JH LUT data + accessor (D4): `extlib/jakob-hanika-luts/rec709.coeff`, baked in
  `src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp`, read via `RGBToSpectrumTable::Get()`.
- EDR display: [MetalEDRView.swift](../../build/XCode/rise/RISE-GUI/App/MetalEDRView.swift),
  [RenderViewModel.swift](../../build/XCode/rise/RISE-GUI/App/RenderViewModel.swift) (headroom probe).
- Parser surface: [AsciiSceneParser.cpp](../../src/Library/Parsers/AsciiSceneParser.cpp)
  (`spectral_painter` ~1215, `scalar_painter` ~1233, `blackbody_painter` ~2891, `fresnel_mode` ~3775;
  `auto_probe_activation_spp` ~8346).

### External (web-researched)
- glTF iridescence (the RGB approximation RISE beats): [KHR_materials_iridescence README][gltf-irid].
- Belcour & Barla 2017, analytical spectral pre-integration: [belcour.github.io][belcour].
- refractiveindex.info — measured n,k + **CC0 license**: [database README][rii-license],
  [Polyanskiy 2024, *Sci. Data* 11, 94][rii-paper], [site][rii].
- Named optical glass / Sellmeier + Abbe: [Schott TIE-29 Refractive Index & Dispersion][schott-tie29],
  [refractiveindex.info BK7][rii-bk7].
- ARRI / SmallHD false-color IRE legend + zebra: [SmallHD Exposure Assist][smallhd-arri].
- Waveform / vectorscope / parade: [Videomaker scopes explained][videomaker-scopes].
- CIE 1931 xy chromaticity diagram: [overview][cie-xy].
- macOS EDR headroom (dynamic; re-query): [Apple `maximumPotentialExtendedDynamicRange…`][apple-edr],
  [AppKit screen-parameters notification][appkit-edr].
- Fluorescence (future, not v1): [A Fluorescent Material Model][fluor].

[gltf-irid]: https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_iridescence/README.md
[belcour]: https://belcour.github.io/blog/research/publication/2017/05/01/brdf-thin-film.html
[rii]: https://refractiveindex.info/
[rii-license]: https://github.com/polyanskiy/refractiveindex.info-database/blob/master/README.md
[rii-paper]: https://www.nature.com/articles/s41597-023-02898-2
[rii-bk7]: https://refractiveindex.info/?shelf=3d&book=glass&page=BK7
[rii-diamond]: https://refractiveindex.info/?shelf=3d&book=crystals&page=diamond
[schott-tie29]: https://www.schott.com/shop/medias/tie-29-refractive-index-and-dispersion-eng.pdf
[smallhd-arri]: https://guide.smallhd.com/a/808517
[videomaker-scopes]: https://www.videomaker.com/how-to/editing/color-correction/waveforms-and-vectorscopes-explained/
[cie-xy]: https://www.av8n.com/imaging/cie-xy-xyy.xhtml
[apple-edr]: https://developer.apple.com/documentation/appkit/nsscreen/maximumpotentialextendeddynamicrangecolorcomponentvalue
[appkit-edr]: https://mackuba.eu/notes/wwdc19/whats-new-in-appkit/
[fluor]: https://arxiv.org/pdf/2505.19672
