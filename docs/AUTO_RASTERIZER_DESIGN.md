# Auto-Rasterizer (Integrator Dispatcher) — Design & Plan

**Status:** design — the routing mechanism for the Phase-3 decision (Candidate C,
PT-default auto-routed hybrid). See [UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md)
§4.1; data backing in [UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md).
**Date:** 2026-06-04.
**Goal:** a thin-shell `auto_rasterizer` that picks the right integrator
(PT / BDPT / VCM) per scene, so authors land on the efficient choice without
hand-selecting — *without* adding a new integrator algorithm.

---

## 1. The routing policy (settled — from the matrix)

| transport regime | integrator | why (matrix) |
|---|---|---|
| **default / converged bulk** (diffuse, glossy-metal, mixed, many-light, most env) | **PT** | wins σ²·T on 10/13 converged classes; 3–7× cheaper per sample |
| **strong-indirect / glossy-interreflection** (gi_spheres-class) | **BDPT** | the 3 classes where its connections overcome the 3–7× cost (gi_spheres 56×, alchemists, env_mesh) |
| **caustic / refractive / dispersive** (dielectric caustics, SDS) | **VCM** | the only integrator reaching the transport (PT/BDPT miss 44–78%); accept its bias+cost *here only* |

**Default to PT when uncertain** — never pay BDPT/VCM's penalty unless detection
is confident it pays off. (VCM-as-default and BDPT-as-default are both
contraindicated: §2 of the decision doc.)

---

## 2. Detection — three tiers, cheapest first

1. **Tier 0 — author hints / documented guidance (Phase 0, this commit).** The
   routing rules live in the project docs (RENDERING_INTEGRATORS.md §2 + a
   CLAUDE.md High-Value Fact) so future agents and authors know what to pick, and
   so the static analyzer (Tier 1) has an explicit spec to implement. An author
   can also *pin* an integrator (an explicit override that skips Tiers 1–2).
2. **Tier 1 — static scene-analysis best-guess (cheap, zero render).** Heuristics
   over the assembled scene: dielectric shells + small/point lights → caustic risk
   (VCM); high glossy-bounce material coverage + enclosed geometry → strong-
   indirect (BDPT); else → PT. Fast but approximate (the regime depends on
   *transport*, which static rules only proxy).
3. **Tier 2 — probe render (accurate, gated on cost — see Phase 3 experiment).** A
   cheap low-spp pass on a sparse tile sample that *operationalizes the matrix
   per-scene*: estimate each candidate's σ²·T (and detect caustic energy PT/BDPT
   miss), pick the winner. Backs up / overrides the Tier-1 guess. **Only adopted
   if the cost experiment (Phase 3) shows the probe is a small fraction of the
   render budget AND reliably matches the matrix verdicts.**

The auto-rasterizer runs Tier 0 (pin?) → else Tier 1 (guess) → else Tier 2
(probe, if enabled) and delegates to the chosen concrete rasterizer.

---

## 3. Architecture — a thin-shell `auto_rasterizer`

A new **`auto_rasterizer`** chunk → a thin `IRasterizer` wrapper that, at
construction/first render, runs detection and **delegates to a concrete
PT/BDPT/VCM rasterizer instance**. No integrator algorithm code; it *composes*
the existing ones. Fits RISE's pluggable-rasterizer + descriptor-driven-parser
architecture (a wrapper IRasterizer is idiomatic).

- **Round-trip:** the chunk persists as `auto_rasterizer`; the *runtime choice* is
  logged, not stored (read-only round-trip preference). An author override saves
  as the concrete chunk (or an explicit pin field).
- **Spectral:** an `auto_spectral_rasterizer` sibling delegating to the
  `*_spectral_` variants (same shell, spectral concrete targets).
- **Granularity:** whole-scene selection first (matches the matrix's per-scene
  verdicts). Per-region/per-tile routing (caustic tiles→VCM, rest→PT in one frame)
  is powerful for mixed scenes but needs a unified framebuffer + cross-integrator
  combination — that's a separate research effort (Candidate D bucket), explicitly
  out of scope for v1.

### 3.1 Phase-1 architecture decision (2026-06-04 — implemented)

**Chosen:** `AutoRasterizer` (`src/Library/Rendering/AutoRasterizer.{h,cpp}`)
derives from `Implementation::Rasterizer` (the same base every concrete
PT/BDPT/VCM rasterizer uses) and **resolves its delegate lazily at the first
render-time entry** (`RasterizeScene` / `PredictTimeToRasterizeScene` /
`RasterizeSceneAnimation`), guarded by `std::call_once`. It stores every input
needed to build *any* of the three delegates, runs `SelectIntegrator(scene)`
once, builds exactly one concrete rasterizer via `BuildDelegate`, and forwards
all `IRasterizer` calls to it.

Why this shape (vs. a parse-time switch inside `Job::SetAutoRasterizer`):

- **It hosts the Phase-4 probe by construction.** The probe needs the
  *assembled* scene + a cheap pre-render; that scene first exists at
  `RasterizeScene(scene, …)`. The wrapper defers the choice to exactly that
  point, so Phase 4 is a body change to `SelectIntegrator(scene)` (plus building
  candidate delegates through the existing `BuildDelegate`) — not a
  re-architecture. A parse-time-only dispatch can't see the scene and would have
  to be torn out, which is why it is explicitly insufficient as the foundation.
- **Deriving from `Rasterizer`** means (a) `Job::PushJobFrameStoreToRasterizers`
  (which `dynamic_cast`s registry instances to `Rasterizer*` and calls
  `SetFrameStore`) keeps working unchanged, and (b) the base already buffers the
  output sinks (`outs`), the progress callback, and the FrameStore — precisely
  the state that is attached *before* the delegate exists. At resolution the
  wrapper **replays** those onto the freshly-built delegate.
- **The delegate is built via the same `RISE_API_Create*Rasterizer` factories**
  the concrete chunks call, with the same shared caster / sampler / filter and
  the canonical per-integrator defaults. So `auto_rasterizer integrator pt` is
  *identical construction* to `pathtracing_pel_rasterizer`, not a parallel
  re-implementation — that is what makes the equivalence verification meaningful.
- **`mutable mDelegate` + `std::once_flag`:** `RasterizeScene` is `const` per the
  `IRasterizer` contract, yet it is the genuine render-time hook (`AttachToScene`
  is **not** called by the Job render path). The base already uses `mutable` for
  OIDN state reached from these same const methods, so this matches precedent
  rather than introducing a new pattern.

**Lifecycle wrinkle (handled, not a redesign):** outputs / progress / FrameStore
are added before the delegate exists. Outputs + progress ride the base's existing
buffering and are replayed at resolution; the FrameStore (which can be pushed
late, or change on a viewport resize) is re-synced to the delegate at each
render-time entry via `SyncDelegateFrameStore()`.

**Stop-rule check (design doc §"Stop rule"):** PASSED — no change to the
rasterizer lifecycle was needed. `IRasterizer`'s single-owner / const-render
semantics compose cleanly with a thin buffering wrapper; the only non-obvious
point is the pre-delegate output buffering, which the base already provides.

**Parameter population:** option (i) (§4). The chunk carries the base rasterizer
params + the `integrator` pin; per-integrator specifics (BDPT depths, VCM merge
radius / VC / VM, PT-SMS) come from `BDPTPelDefaults` / `VCMPelDefaults` /
`SMSConfig` at delegate-build time — one source of truth, shared with the
concrete chunk parsers.

**Spectral sibling — ✅ implemented as a DOMAIN FLAG (Phase 1b, 2026-06-05).**
`auto_spectral_rasterizer` reuses the SAME `AutoRasterizer` class via an `mSpectral`
domain flag rather than a parallel class. `BuildDelegate` switches on `mSpectral` to
call the `*_spectral_` factories (`RISE_API_CreatePathTracingSpectralRasterizer`,
`RISE_API_CreateBDPTSpectralRasterizerAdaptive`, `RISE_API_CreateVCMSpectralRasterizer`)
with the spectral-core param bundle (`nmbegin` / `nmend` / `num_wavelengths` /
`spectral_samples` / `hwss`) the wrapper now carries as a `SpectralConfig`. The chunk
exposes *no* path-guiding and *no* optimal-MIS (matching the concrete `*_spectral_`
chunks — the disabled `PathGuidingConfig` the BDPT/VCM-spectral factories still take is
supplied internally). **The entire decision tree — `SelectIntegrator`, `RunProbe`,
`ProbeCandidate`, the static scans, and the median/mean/σ² helpers — is shared
verbatim; the domain flag touches ONLY `BuildDelegate` + the carried `SpectralConfig`.**
This was chosen over a parallel class precisely to keep a single source of truth for
routing: a sibling class would have duplicated ~400 lines of dispatcher/decision code
(the §"Stop rule" anti-pattern), and future tuning would have to be applied twice.
New surfaces: `RISE_API_CreateAutoSpectralRasterizer` + `Job::SetAutoSpectralRasterizer`
+ the descriptor-driven `auto_spectral_rasterizer` parser; **NO new source file**, so
the five build projects are untouched. The spectral routing validation + its one
documented limitation (`spectral_caustic` → PT, not VCM) are in §6.2.2.

---

## 4. OPEN QUESTION — how to populate the auto-rasterizer's parameters

The shell wraps integrators with different parameter sets (PT: max_recursion,
samples; BDPT: max_eye_depth/max_light_depth, samples; VCM: samples, merge radius,
photon count; + common: spectral params, film, output). How does the
`auto_rasterizer` chunk express them? Options:

- **(i) Common params + per-integrator defaults** *(lean for v1)* — the chunk takes
  only universal params (samples, spectral, output); per-integrator-specific params
  use sensible built-in defaults. Simplest; "just works"; loses fine tuning.
- **(ii) Nested per-integrator override blocks** — `auto_rasterizer { samples 64;
  bdpt { max_eye_depth 5 }; vcm { merge_radius ... } }`. Common block + optional
  per-integrator overrides. Clean, explicit, flexible; more parser surface.
- **(iii) Superset flat descriptor** — accept the union of all params, forward the
  relevant subset. Big descriptor; ambiguous which params apply at author time.
- **(iv) Reference named configs** — the chunk names pre-defined PT/BDPT/VCM
  rasterizer chunks; picks among them. Reuses the chunk system; verbose.

**Lean: ship (i) for the MVP** (the dispatcher should need minimal config), **add
(ii) nested overrides** as the power-user path. **This is the design item to settle
before building the shell (Phase 1).**

**✅ Settled + shipped (Phase 1, 2026-06-04): option (i).** The `auto_rasterizer`
chunk carries the base rasterizer params + the `integrator` pin only; per-integrator
specifics are sourced from `BDPTPelDefaults` / `VCMPelDefaults` / `SMSConfig` at
delegate-build time. Putting an unsupported per-integrator knob (e.g. `merge_radius`,
`sms_enabled`) on the chunk is a parse error by design — option (ii) nested blocks
are the documented later add for power users.

---

## 5. Phased plan (build order)

- **Phase 0 — Author hints in project .md** *(this commit)*: routing rules into
  RENDERING_INTEGRATORS.md §2 + a CLAUDE.md High-Value Fact. Tier 0 + the spec for
  Tier 1.
- **Phase 1 — Thin-shell `auto_rasterizer` skeleton + parameter population.**
  ✅ **DONE (2026-06-04).** Built the wrapper IRasterizer (`AutoRasterizer`,
  render-time lazy delegate resolution — see §3.1), the descriptor-driven
  `auto_rasterizer` chunk parser, `RISE_API_CreateAutoRasterizer` + `Job::SetAutoRasterizer`,
  the five build projects, and `tests/AutoRasterizerTest.cpp` (delegation verified
  two ways: `ResolvedIntegrator()` exact-match + radiance ≡ the concrete rasterizer).
  Param population = option (i). Selection is Tier-0 only (pin, else PT) — NO
  detection. Spectral sibling (`auto_spectral_rasterizer`) deferred to **Phase 1b**
  (§3.1). Round-trip persists the pin in the rasterizer snapshot; the UI "Auto"
  selector + introspection is Phase 7.
- **Phase 1b — Spectral sibling `auto_spectral_rasterizer`.** ✅ **DONE (2026-06-05).**
  A domain flag on the existing `AutoRasterizer` (§3.1) delegating to the `*_spectral_`
  rasterizers and **reusing the Pel decision logic verbatim** (single source of truth).
  Adds the `auto_spectral_rasterizer` parser, `RISE_API_CreateAutoSpectralRasterizer`,
  and `Job::SetAutoSpectralRasterizer`; tests in `tests/AutoRasterizerTest.cpp` (pin
  equivalence to the `*_spectral_` rasterizers, spectral static routing incl. the
  reachable VCM route, and the spectral-caustic probe regression). Routing validation
  + the one documented limitation are in **§6.2.2**.
- **Phase 2 — Tier-1 static best-guess.** ✅ **DONE (2026-06-04).** Implemented
  the heuristic in `AutoRasterizer::SelectIntegrator(const IScene*)` and validated
  its picks against the matrix's per-scene verdicts. Full outcome — the
  introspection signals used, the one-paragraph rule, why BDPT is *not* routed
  statically, and the 18-scene hit/miss table — in **§5.1** below.
- **Phase 3 — Probe-cost EXPERIMENT (the GATE).** Before building the probe:
  measure a low-spp sparse-tile probe's cost across a *variety* of scenes
  (diffuse/glossy/caustic/volume/spectral) as a fraction of the full render
  budget, AND its selection accuracy vs the matrix. **Proceed to Phase 4 only if
  the probe is cheap enough (target: a small single-digit % of budget) and
  reliable.** If not, stop at Tier 1 (static) + author pins.
- **Phase 4 — Tier-2 probe-render detection** *(gated on Phase 3)*: add the probe
  to the shell as the backup/override to the static guess.

---

### 5.1 Phase-2 outcome — Tier-1 static best-guess (2026-06-04, implemented)

**Status:** ✅ implemented in `AutoRasterizer::SelectIntegrator(const IScene*)`
([src/Library/Rendering/AutoRasterizer.cpp](../src/Library/Rendering/AutoRasterizer.cpp));
tested in [tests/AutoRasterizerTest.cpp](../tests/AutoRasterizerTest.cpp). Pel path
only (the `auto_spectral_rasterizer` sibling is still Phase-1b).

**Introspection signals — what the assembled `IScene` cheaply exposes.** Selection
runs at the first render-time entry given a `const IScene*`. One early-out object
enumeration plus a light-list walk yield everything the rule reads:

| signal | source | cost |
|---|---|---|
| transmissive / dielectric material present | `IMaterial::CouldLightPassThrough()` over `GetObjects()->EnumerateObjects` (early-out) | O(objects), stops at first hit |
| positional (point/spot/omni) light present | `ILight::IsPositionalLight()` over `GetLights()->getLights()` | O(lights), tiny |
| _(reachable, not yet routed on)_ area/mesh emitter | object `IMaterial::GetEmitter() != null` | O(objects) |
| _(reachable, not yet routed on)_ env-IBL | `IScene::GetGlobalRadianceMap() != null` | O(1) |
| _(reachable, not yet routed on)_ participating media | `GetGlobalMedium()` / per-object `GetInteriorMedium()` | O(1) / O(objects) |

**NOT cheaply reachable** (documented for the Phase-4 probe): glossy / roughness
coverage — there is **no accessor** on `IMaterial` or `IBSDF`, so it would need a
brittle `dynamic_cast` chain over concrete material classes — and a robust
enclosed-vs-open geometry test. Per the §"Stop rule", these are left to the probe.

**The rule (one paragraph).** Tier-0 author pin wins and skips analysis. Otherwise
route **VCM** iff a transmissive/dielectric surface **and** a positional point/spot
light are *both* present — the physical precondition for a concentrated refractive
caustic. This is deliberately coarse (a dielectric scene with no real caustic still
leans VCM here; the Phase-4 probe refines it back off). **Everything else → PT**,
the matrix's converged-bulk winner. The requirement for a *positional* light is what
does the discriminating work: it spares the dielectric-but-area-lit scenes
(`jewel_vault`, `cloister`, `alchemists`, `env_only`, `prism_dispersion` — all carry
glass yet have no point light) from ever paying VCM's cost.

**Why BDPT is not routed statically.** The BDPT-vs-PT boundary is a wall-clock
efficiency (σ²·T) / indirect-transport-ratio question, not a structural one — and
the two decisive matrix scenes prove static analysis cannot see it. `gi_spheres`
(BDPT wins 56×) and `ggx_showcase` (PT wins) are **byte-identical in every
cheaply-reachable signal**: both an enclosed Cornell-style box lit by a single area
emitter, with no point/env/glass/fog. They differ *only* in surface reflectance —
`gi_spheres` is diffuse interreflection, `ggx_showcase` is glossy metal — which
`IScene` exposes no accessor for, and the relationship is **inverted from
intuition** (the diffuse scene wins BDPT; the glossy scene wins PT). Any static
signal that sent `gi_spheres`→BDPT would also send `ggx_showcase`→BDPT, mis-routing
the converged glossy bulk to BDPT's 3–7× per-sample cost — the expensive mistake the
chip names. So BDPT detection is deferred to the Phase-4 σ²·T probe; the static tier
conservatively defaults the regime to PT (which always converges). This is the
§"Stop rule" applied as written, and was confirmed as the chosen policy.

**Matrix accuracy — heuristic pick vs the Phase-1 §5 winner (all 18 scenes).**
Signals read directly from the scene files.

| scene | transmissive? | positional light? | heuristic | matrix winner | result |
|---|---|---|---|---|---|
| `jewel_vault` | yes | no | **PT** | PT | ✅ hit |
| `cloister` | yes | no | **PT** | PT | ✅ hit |
| `ggx_showcase` | no | no | **PT** | PT | ✅ hit |
| `showroom` | no | yes (omni+dir) | **PT** | PT | ✅ hit |
| `corridor_100lights` | no | yes (100 omni) | **PT** | PT | ✅ hit |
| `homogeneous_fog` | no | no | **PT** | PT (σ²·T) | ✅ hit |
| `env_fog` | no | no | **PT** | PT | ✅ hit |
| `env_only` | yes | no | **PT** | PT | ✅ hit — glass, but no point light → correctly *not* VCM |
| `prism_dispersion` | yes | no | **PT** | PT | ✅ hit — glass, but no point light → correctly *not* VCM |
| `pool_caustics` | yes | yes (2 spot) | **VCM** | VCM | ✅ hit |
| `glass_pavilion` | yes | yes (2 omni) | **VCM** | VCM (intent) | ✅ hit |
| `diamond_teapot` | yes | yes (5 spot) | **VCM** | VCM | ✅ hit |
| `torus_chain` | yes | yes (1 spot) | **VCM** | VCM (RMSE) | ✅ hit |
| `gi_spheres` | no | no | **PT** | BDPT | ⚠ miss → probe (held on safe PT) |
| `alchemists` | yes | no | **PT** | BDPT | ⚠ miss → probe (held on safe PT) |
| `env_mesh` | no | no | **PT** | BDPT (σ²·T) / PT (RMSE) | ◑ PT is the RMSE winner & only 4× off — safe env call (§9) |
| `sculptors_studio` | no | yes (omni+spot) | **PT** | VCM (σ²·T) | ◑ PT is the converged reference; the VCM σ²·T "win" is an artifact of a since-fixed PT camera-firefly bug (baselines §7) |
| `spectral_caustic` | yes | no | **PT** | VCM | ⚠ miss → probe + spectral sibling (area-lit dielectric caustic, no point light; also spectral) |

**13/18 exact hits, and 0 false routes to BDPT/VCM.** Every one of the chip's "clear
cases" is correct: diffuse / glossy-metal / many-light → PT (`jewel_vault`,
`cloister`, `ggx_showcase`, `showroom`, `corridor_100lights`) and the dielectric-
caustic trio → VCM (`glass_pavilion`, `pool_caustics`, `diamond_teapot`). All five
non-hits fail in the **safe direction** — a scene that *could* have used BDPT/VCM is
held on PT (which always converges), never the expensive over-route. Two are BDPT
(not statically separable → probe), one is an area-lit/spectral dielectric caustic
(→ probe + spectral sibling), and two (`env_mesh`, `sculptors_studio`) are
σ²·T-marginal where PT is the converged / RMSE-safe choice anyway.

**What the Phase-4 probe must cover (the static tier provably can't):** (1) the
BDPT-vs-PT σ²·T call *within* the converged bulk (the `gi_spheres` / `alchemists` /
`env_mesh` class, indistinguishable from PT-efficient glossy/diffuse scenes
statically); (2) refining "dielectric + point light → VCM" back off VCM when the
dielectric carries no significant caustic energy; (3) the area-lit / spectral
dielectric-caustic case (`spectral_caustic`) that has no positional light to trip
the static VCM signal.

## 6. Probe-cost experiment design (Phase 3 — the gate)

- **Measure:** probe wall-time (low-spp, sparse-tile, candidate integrators) vs
  the scene's full-render wall-time, across the production-weighted corpus (reuse
  the `var_test/` harness + `bin/tools/HDRVarianceTest`).
- **Gate criteria (both must hold):**
  1. **Cost:** probe ≪ full budget (target a small single-digit %; the exact
     threshold is a fork — discuss).
  2. **Accuracy:** the probe's integrator pick matches the matrix's per-scene
     verdict on ≥ most scenes (especially: detects caustic regimes PT/BDPT miss).
- **Knobs to sweep:** probe spp, tile-sample fraction, which integrators to probe
  (static pre-filter can skip the VCM probe when there are no dielectrics).
- **Outcome:** a go/no-go on Tier 2 + the chosen probe parameters.

### 6.1 Phase-3 experiment OUTCOME — **QUALIFIED GO** (2026-06-04)

Ran the experiment over a 10-scene regime spread (both static blind spots
included): 512 probe renders (M=8 trials × {PT,BDPT,VCM} × spp{1,2,4}), all
EXR float32 (FP16-clip-free, cross-checked with an independent Python OpenEXR
reader ≡ HDRVarianceTest to < 3e-5). Full data + tables + repro:
[var_test/PHASE3_PROBE_EXPERIMENT.md](../var_test/PHASE3_PROBE_EXPERIMENT.md);
scripts `var_test/phase3_{probe,analyze,cost}.py`.

**Headline:** a refined probe picks the matrix winner **10/10, incl. 4/4 blind
spots** — but it is "cheap" (single-digit %) only at high production spp, and the
BDPT half splits into a cheap high-stakes case and an un-cheap marginal case. So:
**build the Phase-4 probe, but gate its activation on production spp and adopt the
caustic-first / scope-reduced design below.**

The four gate dimensions:

1. **ACCURACY — PASS (10/10, blind 4/4).** Two refinements were *required* and are
   non-obvious: (a) the caustic signal must be **median** luminance, not mean —
   PT/BDPT fireflies inflate the *mean* on caustic scenes, so mean-lum *misses*
   the area-lit `spectral_caustic` blind spot (0.81×) and *false-positives*
   `env_only` (1.53×); median fixes the miss (1.81×). (b) the caustic probe must
   be **gated on (dielectric ∧ ¬env-IBL)** — `env_only`'s high VCM luminance
   (median 1.67×, right next to spectral_caustic's 1.81× → no threshold separates
   them) is the documented **+63% VCM env-bias**, not caustic energy. The
   env-IBL gate (`GetGlobalRadianceMap()`-cheap) is the only clean discriminator;
   it correctly suppresses VCM on `env_only`/`alchemists` and is exactly the
   chip's "guard against the VCM env/volume bias confound." (A per-pixel
   "VCM-rescues-PT-dark-pixels" signal was tried and *fails* — env-bias lifts
   shadows uniformly.)

2. **RELIABILITY — caustic PASS, BDPT split.** Caustic half is bulletproof
   (median-lum is a stable single-image stat: 100% at n_probe=1, spp≥2). BDPT half
   needs spp≥2 by construction (variance needs ≥2 samples/pixel; spp=1 reads σ²≈0
   and fails). Big-margin BDPT (`gi_spheres`, σ²·T 90–134×) is 100% stable at
   spp2; **marginal BDPT (`alchemists`, ~1.5× win) is not** — at probe spp it
   collapses to 1.12× vs near-tie PT `homogeneous_fog` at 1.08× (no separating
   threshold) and needs spp4 + n_probe4 (≈trial spp) to stabilise. This is the
   chip's stop-rule NO-GO **for the marginal sub-case only**; mis-routing it costs
   ~1.5× (vs the 90× at stake on gi_spheres, which *is* caught cheaply).

3. **COST — conditional on production spp.** Decision tree is gated +
   short-circuited → the probe renders **2 integrators** in every corpus case
   (PT+VCM for caustics, else PT+BDPT). Probe absolute cost is ~constant in spp
   (per-render *setup* dominates), so its *fraction* of budget shrinks with render
   quality: **median 7.3 % at 256 spp (range 2–39 %), ~1–10 % at 1024 spp; under
   5 % only once production spp exceeds ~110–2000 (median ~377).** At
   preview/interactive spp it is 30–1000 %+ overhead. Worst case is the
   **cheap-PT-winner that must probe an expensive candidate to rule it out**
   (`homogeneous_fog`: 2.5 s BDPT-on-a-volume probe to confirm a near-tie PT win →
   39 % even at 256 spp). **Two cost mitigations:** (a) *measured* — probe at **half
   resolution** cuts the dominant BDPT/VCM probe ~3.5–4× (they're pixel-proportional)
   with **no change in pick** (gi_spheres σ²·T 90×→209×, glass median 2.75×→2.40×),
   so the cost table is ~2–4× pessimistic and the single-digit-% crossover drops
   accordingly; (b) *unmeasured* — render the probe *progressively* so the winner's
   probe samples fold into the final image, leaving only the losing candidate as overhead.

4. **SWEEP / threshold.** τ_caustic = **1.30 on median luminance**; τ_bdpt =
   **1.35** (between near-tie PT ≈1.1× and the smallest detectable BDPT win — the
   residual marginal band is 1.1–1.6×). Probe spp = **4** (spp1 structurally too
   low; spp2 fine for caustics + big-margin BDPT; cost ~flat in spp). Probe at
   **half resolution** (the chip's tile knob — measured 3.5–4× cheaper on the
   expensive candidates, decision-preserving).

**Recommended Phase-4 config:** static pre-filter (dielectric? env-IBL?) →
**activation gate: production spp ≥ ~256** (else stay Tier-1 static + pins) →
caustic check first *(if dielectric ∧ ¬env-IBL: render PT+VCM at spp≈4, **half
resolution**; median-lum ratio > 1.30 → VCM, short-circuit)* → else BDPT check
*(render PT+BDPT at spp≈4, half-res, internal per-pixel σ²; σ²·T ratio > 1.35 →
BDPT, else PT)*. Use the renderer's internal per-pixel variance from **one**
render (the experiment's n_probe>1 was an inter-run proxy).
**Residuals left to pins/Tier-1:** marginal BDPT wins (~1.5×, low stakes) and
env-lit real caustics (suppressed by the env gate; VCM carries env-bias there
anyway). Open Q#2 (probe-cost threshold) is answered: **single-digit % ⇔
production spp ≳ 256**, so the probe is a *final-render* tool, not an interactive one.

---

### 6.2 Phase-4 probe — SHIPPED + REAL in-process cost/resolution sweep (2026-06-05)

**Status:** ✅ the Tier-2 probe is implemented in
`AutoRasterizer::SelectIntegrator` → `RunProbe` / `ProbeCandidate`
([src/Library/Rendering/AutoRasterizer.cpp](../src/Library/Rendering/AutoRasterizer.cpp));
the decision tree is the §6.1 design verbatim. It is **pure dispatcher logic** —
integrators + concrete rasterizers are byte-identical to HEAD; the probe only
*composes* them via `BuildDelegate`. Tested in
[tests/AutoRasterizerTest.cpp](../tests/AutoRasterizerTest.cpp) (Phase-4 section);
the sweep harness is `var_test/phase4_sweep.py` (gitignored output
`var_test/phase4_sweep.json`).

#### The machinery (how the in-process probe renders a candidate)

`ProbeCandidate(scene, choice, cfg, needVariance)` renders a candidate integrator
against the **already-assembled** scene and reads its image back, *without* a
re-parse, BVH rebuild, or any touch to the real output:

1. **Resolution** is shrunk via `IScenePriv::ResizeFilm(W/scale, H/scale, AR)`
   (the only resolution knob — the rasterizer reads render dims from
   `IScene::GetFilm()`, not the camera or FrameStore). The original dims are
   restored on every exit path. Safe because the probe runs single-threaded
   inside the `std::call_once` selection, strictly *before* the real render's
   workers spawn — exactly `ResizeFilm`'s concurrency contract.
2. **spp** is set on a `mSamples->Clone()` (never the shared canonical sampler)
   via `SetNumSamples(cfg.spp)`.
3. The candidate is built with `BuildDelegate(choice, probeSampler, /*fs*/null,
   /*denoise*/false, OFF-guiding, OFF-adaptive)` — a **null FrameStore** so the
   render lands in the delegate's own internal image and never disturbs the
   canonical store; **denoise off** because the σ² signal lives in the raw
   per-pixel noise a denoiser would erase; **guiding/adaptive off** so there is no
   training pass and spp is uniform (well-defined cost + variance).
4. A `ProbeCaptureOutput` (an `IRasterizerOutput` that stores per-pixel luminance
   = mean RGB, matching Phase-3's EXR-RGB signal) reads the image back via the
   universal `OutputImage` flush — no EXR round-trip.
5. **median luminance** (caustic signal) is a single-render statistic;
   **mean per-pixel σ²** (BDPT signal) is the cross-render variance over
   `cfg.varianceRenders` renders.

**The cost lever holds — no rebuild across candidates.** `ObjectManager::
PrepareForRendering` is idempotent (`!pBVH` guard), the top-level BVH + per-mesh
BVHs live on the *shared* scene's ObjectManager, and `RayCaster::AttachScene`
early-returns on a re-attach of the same scene (the luminary manager / light
sampler / env sampler are built once, by whichever render hits the scene first).
So every probe render after the first pays only ray-tracing cost (+ VCM's
auto-radius pre-pass, a genuine per-render setup it would pay in production too).
**This is why the real in-process cost is far below the Phase-3 *emulation*'s
fresh-process numbers** — the emulation re-parsed + rebuilt per candidate.

**σ² is the Phase-3 inter-run proxy, in-process.** RISE's QMC stream is seeded
deterministically (`SobolSequence::HashCombine(x,y)`), so the per-pixel variance
across two renders comes *entirely* from multi-threaded run-to-run
non-determinism (the per-worker fallback RNG order + splat FP-accumulation order)
— the same mechanism the Phase-3 cross-process pool measured. The probe therefore
**must render multi-threaded** (a single-threaded probe would read σ²≈0). Reading
the renderer's *internal* Welford variance (ProgressiveFilm) was rejected: it
would require an accessor on `PixelBasedRasterizerHelper`, i.e. touching
rasterizer code, violating the byte-identical constraint.

#### Real cost + resolution sweep (`phase4_sweep.py`)

6 regime-spanning scenes (uniform 256² film, benchmark thread mode, oidn/adaptive
off, K=3 trials at spp 32 + a low-spp point for the `full=a+b·spp` fit so cost% is
extrapolated to 256/1024 without paying a 256-spp render per cell). **Probe
spp 4, variance_renders 2.** Decision + flip-rate are spp-independent (the probe
renders at its own spp), so the same trials give both halves of the gate.

| scene | half | matrix | decision @ 1/2,1/4,1/8 | probe s @ 1/2,1/4,1/8 | cost%@256 @ 1/2,1/4,1/8 | cost%@1024 (1/4) |
|---|---|---|---|---|---|---|
| gi_spheres | bdpt | bdpt | **bdpt bdpt bdpt** ✅ | 1.71 / 1.23 / 1.09 | 2.4 / 1.7 / 1.4 | 0.4 |
| ggx_showcase | bdpt | pt | **pt pt pt** ✅ | 0.66 / 0.16 / 0.05 | 6.2 / 1.3 / 0.4 | 0.3 |
| glass_pavilion | caustic | vcm | **vcm vcm vcm** ✅ | 0.46 / 0.12 / 0.04 | 0.5 / 0.1 / 0.0 | 0.0 |
| env_only | caustic | pt | **pt pt pt** ✅ (env-gate) | 0.11 / 0.03 / 0.01 | 4.5 / 1.1 / 0.3 | 0.3 |
| jewel_vault | caustic | pt | **vcm vcm vcm** ⚠ over-fire → **now pt** (§6.2.1) | 0.70 / 0.18 / 0.06 | 0.6 / 0.2 / 0.0 | 0.0 |
| homogeneous_fog | bdpt | pt | **pt pt pt** ✅ | 8.0 / 2.0 / 0.5 | 31.7 / 7.9 / 1.9 | 0.5 |

(All cells K=3 trials; "decision" is the modal pick — **no cell flipped across
trials at any scale**. Probe seconds = summed candidate-render wall time the probe
logs. cost% = probe-s / extrapolated full-render-s at that spp.)

**Headline:** **5/6 correct as first shipped** (the 6th, the `jewel_vault`
over-fire, is now **fixed** by the transport-reach gate in §6.2.1 → **6/6**),
**zero flips at any scale**, and the real in-process cost is **far below the
emulation**. gi_spheres is **2.4 %@256 half-res vs the emulation's 3 %** (which
already assumed half-res); more decisively, the emulation's *worst case*
(homogeneous_fog **39 %@256**) is here **7.9 %@256 at quarter-res** — the fixed
per-render setup the emulation re-paid every candidate is now paid once on the
shared assembled scene, exactly the predicted win.

**Cost is dominated by the BDPT-on-a-volume probe ruling out a cheap PT winner.**
`homogeneous_fog` is the only scene above single-digit %@256 at half-res (31.7 %)
because a volume makes BDPT ~37× costlier per sample (CLAUDE.md), so the probe
spends most of its time on the candidate it ends up *rejecting*. Quartering the
probe resolution cuts it to 7.9 %, and 1/8 to 1.9 % — and the PT decision never
wavers. Every other scene is ≤ 6.2 %@256 even at half-res.

#### Per-half reliability + the res floor

- **Caustic half (median-lum):** rock-stable — a single-image statistic.
  glass_pavilion (→VCM), env_only (→PT via the env-IBL gate), and even the
  jewel_vault over-fire (→VCM) are **identical at 1/2, 1/4, and 1/8** with zero
  flips. Floor: **1/8 is safe on margin**, **1/4 recommended** for pixel headroom
  on small/thin caustics.
- **BDPT half (σ²·T):** the big-margin win (gi_spheres, σ²·T ~40–480×) and the
  clear PT scenes (ggx_showcase σ²·T 0.2–0.3×; homogeneous_fog) **all hold to 1/8
  with zero flips** — the σ²·T margins are nowhere near τ_bdpt=1.35. Floor: **1/4
  recommended** (the σ² estimator wants more pixels than the median; 4096 px at
  1/4 of 256² — and far more at real ≥1080p finals). As §6.1 warned, the
  **marginal** ~1.5× BDPT win is *not* in any corpus and stays a pin/Tier-1
  residual at every probe resolution.

**Recommended defaults (shipped):** **`auto_probe_scale 4`** (quarter-res — the
measured sweet spot: every decision holds *and* even the worst-case volume probe
is single-digit %@256; half-res leaves homogeneous_fog at ~32 %). `auto_probe_spp
4`, `auto_probe_variance_renders 2`, `τ_caustic 1.30`, `τ_bdpt 1.35`.
**Activation-spp:** the real cost is single-digit-% from ~256 spp at 1/4 (median
1.2 %, worst 7.9 %), so the shipped default `auto_probe_activation_spp 256` is the
measured crossover — *not* baked a priori; below it the dispatcher uses the Tier-1
static guess. All GlobalOptions-overridable (the §6.2 sweep varied them via
per-scale option files). **Stop-rule check: PASSED** — the in-process scratch
render needed only a thin addition (ResizeFilm + a cloned sampler + a capturing
output), no rasterizer-lifecycle surgery, and the candidate renders provably do
**not** rebuild the scene/acceleration structure (idempotent `PrepareForRendering`
+ same-scene `AttachScene` early-return), so the cost dropped as hoped.

#### Two findings the Phase-3 emulation could not surface

1. **`jewel_vault` over-fire — now FIXED (§6.2.1).** The emulation hand-classified
   `jewel_vault` as non-dielectric and never rendered VCM for it. The real probe
   sees it *is* dielectric (glass gems) + area-lit + no env, so the caustic check
   fires and **routes it to VCM (median ~2.3–2.9×)** — but its matrix winner is PT.
   This is a genuine **low-spp artifact of the median-lum signal**: at probe spp the
   gems' refractive energy that PT *would* converge is missing from PT's median, so
   VCM reads brighter — indistinguishable by median alone from a true caustic
   (`jewel_vault` 2.9× actually *exceeds* `spectral_caustic` 1.8×, so no τ separates
   them). It is a **performance** mis-route, not a correctness bug (VCM is unbiased
   here, just slower than PT-optimal). **Closed 2026-06-05 by a second
   transport-reach gate (§6.2.1)** — no author pin needed; the Phase-4 sweep is now
   6/6.
2. **Spectral caustics need the Phase-1b spectral sibling.** `auto_rasterizer` is
   Pel-only; the real probe routes `spectral_caustic` → **PT**, because its
   *dispersive* caustic is spectral-only and its RGB projection carries no strong
   caustic (so PT is in fact correct for the Pel domain). The area-lit-caustic→VCM
   capability is validated on the RGB `glass_pavilion` instead. The
   `auto_spectral_rasterizer` follow-up shipped (§3.1), but **even the spectral probe
   routes `spectral_caustic` → PT** — the RGB-projected mean-reach gate is defeated by
   the VCM-spectral luminance-proxy merge energy loss, a documented out-of-scope gap.
   Full analysis + the kept-identical-gate decision: **§6.2.2**.

---

### 6.2.1 jewel_vault over-fire FIXED — the transport-reach gate (2026-06-05)

**Status:** ✅ the §6.2 over-fire is closed; the Phase-4 sweep is now **6/6**. The
caustic branch gains one cheap second gate. Integrators + concrete rasterizers
remain **byte-identical** to HEAD — this is pure dispatcher-decision logic (only
`src/Library/Rendering/AutoRasterizer.{h,cpp}` + the test changed). Repro harness:
`var_test/jewel_discriminator.py` (gitignored).

**The first-guess discriminator was REFUTED by measurement.** The natural
hypothesis — "a real caustic leaves PT dark-and-*flailing*, so gate the VCM route
on high PT per-pixel variance / σ²·T" — is **INVERTED at the probe config**.
Measured in-process at scale 4 / spp 4 (3 trials each):

| scene | role | medRatio (gate 1) | **meanRatio (gate 2)** | PT σ²·T | PT σ/μ |
|---|---|---|---|---|---|
| `jewel_vault` | PT (over-fire) | 2.5–3.1 ✓fires | **0.96–1.12** | 6–8 | ~330% |
| `crystal_garden` | PT (non-caustic) | 1.5–1.6 ✓fires | **0.67–0.69** | 0.05 | ~155% |
| `cloister` | PT (non-caustic) | ~1.1 (no fire) | 0.88–0.99 | 0.005 | ~88% |
| `diamond_teapot` | VCM (caustic) | 4.6–4.8 ✓fires | **1.88–1.95** | 0.004 | ~84% |
| `glass_pavilion` | VCM (caustic) | 2.0–2.3 ✓fires | **20–32** | 0.0015 | ~96% |
| `pool_caustics` | VCM (caustic) | <1.30 (no fire) | — | — | — |

At probe spp `jewel_vault` is the **NOISIEST** PT in the set (σ²·T 6–8, σ/μ 330%),
not the quietest: its bright, hard 3+-bounce indirect is wildly under-converged at
4 spp, while real-caustic PT renders quietly **dark** (it *misses* the energy
rather than fireflying). The matrix's production-spp intuition (glass PT σ/μ 3675%
vs jewel 31%) does **not** transfer to the probe's low-spp/low-res regime — so PT
variance/σ²·T **cannot** separate the over-fire from a real caustic.

**The discriminator that works: the MEAN-luminance (transport-reach) ratio
`meanVCM / meanPT`.** It realizes the *correct concept* ("a real caustic is energy
PT cannot reach") via the quantity that actually carries the signal at probe
config: a real refractive caustic makes VCM's MEAN luminance (= total energy) far
exceed PT's, while a converging dielectric scene already has PT-mean ≈ VCM-mean.
The over-fire class {jewel 0.96–1.12, crystal 0.67–0.69, cloister 0.88–0.99} and
the real caustics {diamond 1.88–1.95, glass 20–32} separate with a clean
**1.12 | 1.88** gap → **τ_reach = 1.50** (1.35× reject margin on jewel, 1.25×
accept margin on the tightest real caustic, diamond; **zero flips** across 3
trials). `meanLum` is read from the **same single render the median gate already
does**, so the gate is **FREE** — no extra probe render; the caustic-branch cost
is unchanged.

**The fix.** Route VCM iff **both** gates hold: `medRatio > τ_caustic` (gate 1,
the firefly-robust median trigger, unchanged) **AND** `meanRatio > τ_reach`
(gate 2, the new transport-reach test). When the median fires but reach fails (the
`jewel_vault` class — PT reaches the same total energy), fall through to the
general BDPT-vs-PT check, which correctly keeps `jewel_vault` on PT via its σ²·T
win (verified end-to-end: `caustic median 2.95x fired but reach 0.66x <= 1.50 →
fall through → sigma2T 0.21x <= 1.35 → pt`). New knob `auto_probe_tau_reach`
(default 1.50, GlobalOptions-overridable).

**Re-validated routing (probe config, post-fix, zero flips across 3 trials):**

| scene | gate 1 (median) | gate 2 (reach) | route | verdict |
|---|---|---|---|---|
| `jewel_vault` | fires 2.5–3.1× | **fails ≈1.0×** | **PT** | ✅ fixed (was VCM) |
| `glass_pavilion` | fires 2.0–2.3× | passes 20–32× | VCM | ✅ unbroken |
| `diamond_teapot` | fires 4.6–4.8× | passes 1.9× | VCM | ✅ unbroken |
| `crystal_garden` | fires 1.5–1.6× | fails 0.7× | PT | ✅ (bonus: was VCM) |
| `pool_caustics` | no fire <1.30× | — | PT | ⚠ pre-existing residual* |
| `cloister` / `env_only` / `gi_spheres` / `ggx_showcase` / `homogeneous_fog` | unchanged | | PT / PT / BDPT / PT / PT | ✅ all unaffected |

*`pool_caustics` is a **separate, pre-existing** probe limitation, **not**
introduced by this fix: its caustic is localized, so PT's MEDIAN pixel (the
diffusely-lit pool) matches VCM's → the median gate never fires → it routes PT in
**both** the baseline and the fix (byte-identical decision for pool). Catching a
localized caustic whose PT goes *dark* would need an independent energy-reach
trigger (the mean-ratio firing **without** the median gate) — a riskier change
that discards the median gate's firefly-robustness; deferred as out of scope for
the `jewel_vault` fix and documented here.

---

### 6.2.2 Phase-1b spectral sibling — routing validation + the `spectral_caustic` limitation (2026-06-05)

**Status:** ✅ `auto_spectral_rasterizer` ships as a domain flag on `AutoRasterizer`
(§3.1). Integrators + concrete **Pel AND spectral** rasterizers are **byte-identical**
to HEAD — pure dispatcher logic. The Tier-0 pin / Tier-1 static / Tier-2 probe decision
tree is shared verbatim with the Pel path; only `BuildDelegate` (factory switch) and the
carried `SpectralConfig` differ.

**What routes correctly** (measured in-process at the shipped probe config — scale 4,
spp 4, τ_caustic 1.30, τ_reach 1.50, τ_bdpt 1.35; `tests/AutoRasterizerTest.cpp`):

| spectral scene | tier | route | how |
|---|---|---|---|
| pin pt / bdpt / vcm | Tier-0 | PT / BDPT / VCM | delegates to the matching `*_spectral_` rasterizer (radiance ≡ verified) |
| diffuse + point light | Tier-1 static | PT | no dielectric → PT |
| dielectric + point light | Tier-1 static | **VCM** | transmissive ∧ positional → VCM (**proves the spectral VCM delegate is reachable**) |
| dielectric, area-lit only | Tier-1 static | PT | no positional light → PT |
| `spectral_caustic` (dispersive) | Tier-2 probe | **PT** ⚠ | median gate fires, reach gate fails — see below |

**The `spectral_caustic` → PT limitation (the gate is deliberately kept identical to
the Pel path).** Phase-1b's goal was to close `spectral_caustic` → VCM (the Pel auto
routes it PT because its dispersive caustic is spectral-only — §6.2 finding #2). The
spectral probe runs correctly — it renders spectral PT+VCM candidates and evaluates the
same two-gate caustic test — but routes **PT**, measured in-process (zero flips):

- **Gate 1 (median-lum) FIRES:** VCM/PT median ≈ **2.9×** (> τ_caustic 1.30). VCM
  genuinely spreads the dispersive caustic across more pixels than PT, so the caustic
  *is* detected by the firefly-robust median.
- **Gate 2 (mean-reach) FAILS:** VCM/PT mean ≈ **0.7×** (< τ_reach 1.50). The
  transport-reach gate (§6.2.1) encodes "a real caustic is energy PT can't reach, so
  VCM-mean ≫ PT-mean." That is **inverted in the spectral domain** by the documented
  **VCM-spectral luminance-proxy merge** (`RISEPelToNMProxy`, SPECTRAL_PARITY_AUDIT §3):
  the merge loses dispersion energy, so VCM-spectral's RGB-projected MEAN sits *below*
  PT-spectral's (whose sparse caustic-hitting samples become mean-inflating fireflies —
  the same firefly inflation that made mean-lum a broken *primary* caustic signal in
  Phase 3). The reach gate is therefore not discriminating on spectral caustics — it is
  *always-failing*, so it would reject every spectral caustic, real or not.

**Decision (2026-06-05): keep the gate identical; route `spectral_caustic` → PT.** Three
options were weighed: (1) skip gate 2 for spectral (route VCM on the median alone),
(2) keep the gate identical and route PT, (3) make gate 2 firefly-robust for both
domains. **(2) was chosen.** Rationale: the shared two-gate decision stays a *single
source of truth with zero per-domain divergence* (the chip's strongest constraint, and
the whole point of the domain-flag architecture), and PT is a *safe* route — unbiased,
always converges, just noisier than VCM-optimal on this scene per the matrix. Option (1)
would fork the routing policy and rely on the unmeasured assumption that no spectral
analogue of the `jewel_vault` over-fire exists; option (3) changes the validated shared
gate and would require re-validating the full Pel probe corpus, and might still not fire
if VCM's total spectral energy is genuinely below PT's. **Properly** closing
`spectral_caustic` → VCM requires fixing the VCM-spectral merge energy loss
(per-wavelength VCM photons — SPECTRAL_PARITY_AUDIT §3 / BASELINES §"VCM-spectral
dispersion-loss"), a multi-week integrator effort explicitly out of scope for the
dispatcher. The spectral VCM *delegate* is proven reachable (the `vcm` pin + the static
dielectric+point route); only the *probe's automatic selection* of it for a dispersive
caustic is gated by that merge fix. `tests/AutoRasterizerTest.cpp` locks
`spectral_caustic` → PT as a regression guard — it will flip to VCM the moment the
merge energy-loss is closed.

---

## 7. UI integration (to design with the GUI bridges)

- An **"Auto" mode** as the default in the integrator selector; manual override to
  PT/BDPT/VCM/MLT.
- **Surface the choice + reason** ("Auto → VCM: dielectric caustic regime") for
  transparency; let the user pin an override (persists per scene).
- Mac + Windows bridges have switch-on-int enum getters that fall through to None
  on a missing case — **audit both bridges when adding the Auto enum value** (per
  the CLAUDE.md bridge-enum note).

---

## 8. Open questions for discussion

1. **Parameter population (§4)** — confirm (i) common+defaults for v1, with (ii)
   nested overrides later? Or prefer (ii)/(iv) from the start?
2. ~~**Probe-cost gate threshold (§6)** — what fraction of budget is "acceptable"
   for the probe?~~ **ANSWERED (§6.1, Phase-3 experiment):** the probe is
   single-digit % only when production spp ≳ 256 (median 7.3 % @256, ~1–10 %
   @1024); gate probe activation on production spp and keep it a final-render tool.
3. **Granularity** — whole-scene v1 confirmed; per-region deferred to D-research?
4. **Selection caching** — re-run detection every render, or cache the choice
   (and invalidate on scene edit)?
