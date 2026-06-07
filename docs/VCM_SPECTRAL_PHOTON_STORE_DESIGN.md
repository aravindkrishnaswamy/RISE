# VCM-Spectral Per-Wavelength Photon Store — Design Proposal

**Status:** **v2 REJECTED by round-1 adversarial review (3-of-3).**  v3
sketched as deferred multi-week scope; no code landed; audit §3 stays
OPEN.  See §13 for the round-1 verdict.
**Author:** Claude (Anthropic), session 2026-06-07.
**Closes:** [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §3 (the single remaining open item).
**Hard rule:** changes land in the working tree only; the user reviews and commits.
**Reading order if you're picking this up cold:** §1 (problem), §13 (what
happened to v2), §14 (the v3 architecture sketch), then §2-§12 as
historical context for the rejected v2 proposal.

---

## 1. Problem (one paragraph)

`VCMIntegrator::EvaluateMergesNM` reads photon throughput out of the
shared light-vertex store via
`LightVertexThroughput<NMTag>(lv) = RISEPelToNMProxy(lv.throughput)` —
a Rec.709 luminance projection of the photon's RGB-rendered throughput.
The store was populated by a Pel `GenerateLightSubpath` pass.  Symptoms:
on dispersive caustics, dye glass, Beer-Lambert filters, and HWSS
merges the proxy collapses per-wavelength transport to one scalar and
loses wavelength separation entirely.  See audit §3.2 for the failure-
mode catalogue and §3.3 for the intended `LightVertexNM` structure
(declared at [VCMLightVertex.h:110-118](../src/Library/Shaders/VCMLightVertex.h#L110) but unused).

## 2. Scope choice — per-photon-hero (v2), NOT full-chain

Audit §7-Q1 surfaces two options:

| Option | Memory | Companion-wavelength accuracy | Effort |
|---|---|---|---|
| **Per-photon-hero (v2)** | +16 B per photon (throughputNM + nm) | Hero-match merges exact; mismatches read the photon-hero throughput (stratified-hero approximation) | ~1 week |
| Full chain (v3) | +80 B per photon-bounce | All wavelengths exact via `RecomputeSubpathThroughputNM` | 3-4 weeks; ~10× memory tax on long subpaths |

**This proposal implements v2 only**, matching the §7-Q1 recommendation.
Rationale:

1. **Strictly better than today** at the merge site.  The proxy
   collapses the photon to a luminance-weighted scalar that mixes
   chromaticity; v2 carries the actual wavelength-accurate scalar
   transport from the photon's light pass.  Every dispersive scene
   the audit lists (triplecaustic, pool dispersion, dye-glass, prism)
   gets the wavelength its photon was traced at — not a Rec.709
   luminance of an RGB rendering.
2. **HWSS amplification recovered for hero-matching bundles.**  Each
   HWSS bundle's hero merges exactly; companions see photons whose
   stored hero is uniformly stratified across `[λ_begin, λ_end]` (we
   pick the per-photon hero via Sobol stratification), so the
   companion-on-photon-hero average converges to a band-averaged
   throughput — a quantitative improvement over the proxy's
   wavelength-blind luminance projection.
3. **Memory-affordable.**  +16 B per photon vs the ~150 B/photon
   `LightVertex` baseline is ~10%.  Full chain storage is up to 10×
   on a 10-bounce caustic photon; not justified until v2 measurements
   show residual error.
4. **No new failure modes.**  The MIS quantities (`dVCM/dVC/dVM`) are
   wavelength-independent ([VCMSpectralRecurrenceTest](../tests/VCMSpectralRecurrenceTest.cpp))
   so the recurrence is reused verbatim; only the throughput field
   the merge integrator reads changes.
5. **Reversible — v3 is a strict extension.**  Adding a chain pointer
   alongside `(throughputNM, nm)` is additive: v2 stays correct on
   non-dispersive scenes and the full chain only kicks in on the
   subset of scenes where v2's stratified-hero approximation shows
   measurable residual error.

**If round-1 review surfaces a regime that v2 demonstrably can't
handle**, the recommendation flips to v3.  The most likely failure-
mode candidate is heavy spectral absorption (dye glass) where the
photon-hero throughput at λ_photon is orders of magnitude off the
eye-wavelength throughput at λ_eye and the stratified average is
no longer a fair estimator.  Round-1 reviewer (b) is asked to refute
this explicitly.

## 3. Data structure changes

### 3.1 LightVertexNM stays as scaffolded

[VCMLightVertex.h:110-118](../src/Library/Shaders/VCMLightVertex.h#L110):

```cpp
struct LightVertexNM : public LightVertex {
    Scalar throughputNM;
    Scalar nm;
    LightVertexNM() : LightVertex(), throughputNM(0), nm(0) {}
};
```

No layout change; the first two fields (`ptPosition`, `plane`) stay
inherited from `LightVertex` so the KD-tree algorithms operate on
this type unchanged.

### 3.2 Templatize `LightVertexStore` on the vertex type

[VCMLightVertexStore.{h,cpp}](../src/Library/Shaders/VCMLightVertexStore.h) becomes templated:

```cpp
template<class VertexT>
class LightVertexStoreT { /* current body, vertex-type-generic */ };

using LightVertexStore   = LightVertexStoreT<LightVertex>;    // Pel callers unchanged
using LightVertexStoreNM = LightVertexStoreT<LightVertexNM>;  // new
```

Strategy: keep all method bodies in the `.cpp` via explicit-template
instantiation for `<LightVertex>` and `<LightVertexNM>`.  Avoids a
header-explosion that would bloat compile times across the project.

The free helpers `LessThanX/Y/Z`, `BalanceSegment`, `LocateAllInRadiusSq`,
`ComputeMedian` template on the vertex type — they only read
`v.ptPosition[axis]` and `v.plane`, both inherited unchanged in NM.
`ClampOutlierThroughputs` already only reads `v.throughput` (RISEPel)
which exists on both types; the percentile clamp is preserved
verbatim for the Pel store; for the NM store, we ALSO apply a
parallel clamp on `throughputNM` (separate percentile-of-scalar
rank).  Without an NM-side clamp, a single firefly photon at a heavy
absorption peak would survive into the merge.

### 3.3 Add `ConvertLightSubpathNM` companion

[VCMIntegrator.h](../src/Library/Shaders/VCMIntegrator.h) gains:

```cpp
static void ConvertLightSubpathNM(
    const std::vector<BDPTVertex>& verts,
    const VCMNormalization&        norm,
    std::vector<LightVertexNM>&    out,
    std::vector<VCMMisQuantities>* outMis,
    Scalar                         heroNM
    );
```

Implementation:  copy of the existing `ConvertLightSubpath` body,
allocating `LightVertexNM` instead of `LightVertex` and assigning
`lv.throughputNM = v.throughputNM; lv.nm = heroNM`.  Everything else
is line-for-line identical (the MIS recurrence is wavelength-
independent — assertion in [VCMSpectralRecurrenceTest](../tests/VCMSpectralRecurrenceTest.cpp)).
**No algorithmic change** — just a different output collection
element type.  Could alternatively be written as a single templated
body shared with `ConvertLightSubpath`; either is acceptable.  I will
template it to avoid drift between the two converters.

## 4. Light-pass changes (spectral rasterizer only)

### 4.1 Hero wavelength per photon

The spectral rasterizer's light-pass (currently shared with Pel via
`VCMRasterizerBase::PreRenderSetup` → `LightPassDispatcher`) picks a
hero wavelength per photon via the same Sobol stream as the eye-side
HWSS:

```cpp
const Scalar u  = sampler.Get1D( /* new dim */ );
const Scalar nm = bUseHWSS
    ? SampledWavelengths::SampleEquidistant(u, lambda_begin, lambda_end).HeroLambda()
    : ( lambda_begin + u * (lambda_end - lambda_begin) );
```

The `LightPassDispatcher` is templated on a `LightPassMode` so:
- `Pel` mode calls `pGen->GenerateLightSubpath(...)` and pushes
  `LightVertex` records (unchanged Pel path).
- `NM` mode samples a per-photon `heroNM`, calls
  `pGen->GenerateLightSubpathNM(..., heroNM, &swl)`, then walks the
  output through `ConvertLightSubpathNM(..., heroNM)` and pushes
  `LightVertexNM` records.

The `VCMRasterizerBase` base class declares both `pLightVertexStore`
(Pel) and `pLightVertexStoreNM` (NM) pointers and gives the
subclasses a virtual `DoLightPass()` that selects the right one.
**The Pel subclass instantiates ONLY the Pel store; the spectral
subclass instantiates ONLY the NM store** — no double-pay.

### 4.2 Domain split

| Rasterizer | Store | Light pass | Photon throughput field |
|---|---|---|---|
| `VCMPelRasterizer` | `LightVertexStore`   | `GenerateLightSubpath`   | `RISEPel throughput` |
| `VCMSpectralRasterizer` | `LightVertexStoreNM` | `GenerateLightSubpathNM` | `Scalar throughputNM` + `Scalar nm` |

The base class API surface (the `VCMRasterizerBase::pLightVertexStore`
field) keeps its existing type; we ADD a parallel NM pointer.  This
preserves all in-tree callers (the Pel rasterizer's read path is
byte-for-byte unchanged).

## 5. Merge-path changes (the actual bug fix)

[VCMIntegrator.cpp:268-278](../src/Library/Shaders/VCMIntegrator.cpp#L268)
is the bug site.  The fix replaces the NM-tag `LightVertexThroughput`
specialization with a direct field read.  But because the integrator's
`EvaluateMergesNM` takes `const LightVertexStore& store`, we must
either (a) extend the API to take an NM store, or (b) make
`EvaluateMergesNM` templated.

I choose (a) — a new explicit NM-store overload — because:

- The Pel-store overload becomes a "we have only Pel throughputs" path
  that we want to delete entirely (it's the bug).  Keeping both
  overloads invites a future caller to accidentally re-use the
  proxy.  After the fix, the only caller of any Pel-store-via-NM-tag
  merge is gone.
- API churn is minimal: one new `EvaluateMergesNM(const LightVertexStoreNM&, …, nm)`
  signature, one call site in `VCMSpectralRasterizer::IntegratePixel`
  updated to pass `pLightVertexStoreNM`.

The integrator body (`EvaluateMergesImpl<NMTag>`) is retained as a
template, generalized over `StoreT`:

```cpp
template<class Tag, class StoreT>
typename SpectralValueTraits<Tag>::value_type EvaluateMergesImpl(
    const std::vector<BDPTVertex>& eyeVerts,
    const std::vector<VCMMisQuantities>& eyeMis,
    const StoreT& store,
    const VCMNormalization& norm,
    const Tag& tag);
```

The NM specialization of `LightVertexThroughput` is updated:

```cpp
// NEW: reads from LightVertexNM directly.
template<>
inline Scalar LightVertexThroughput<NMTag>(const LightVertexNM& lv, const NMTag&)
{
    return lv.throughputNM;
}
```

The Pel specialization stays:

```cpp
template<>
inline RISEPel LightVertexThroughput<PelTag>(const LightVertex& lv, const PelTag&)
{
    return lv.throughput;
}
```

There is no more `LightVertexThroughput<NMTag>(const LightVertex&, …)`
overload — the proxy path is **deleted** (not preserved as a
fallback).  Compile-time enforcement that no caller resurrects it.

`RISEPelToNMProxy` itself remains: it is still used by
`EvalLightRadiance<NMTag>` for ILight (point/spot/directional) sources
that lack a spectral API.  That use is the audit §3.1 sibling — see §8.

### 5.1 HWSS bundle handling

The HWSS eye-side loop in
[VCMSpectralRasterizer::IntegratePixel](../src/Library/Rendering/VCMSpectralRasterizer.cpp)
already re-evaluates each companion wavelength by re-running
`EvaluateMergesNM(..., companionNM)` on the same store.  Under the
new design:

- Hero merge at λ_h: reads each candidate photon's `throughputNM` at
  the photon's stored λ_p.  When the photon population is stratified
  on λ_p, the expectation of the average over a query ≈ band-average
  throughput.  For non-dispersive scenes (the bulk), the band-average
  ≡ the true throughput at every λ — i.e. **bit-exact merge** modulo
  Monte Carlo noise.  For dispersive scenes, this is a known
  approximation, strictly better than the Rec.709 luminance proxy.
- Companion merge at λ_c: same read — the photon-hero throughput is
  reused.  **Companions and hero share the same per-photon
  throughputs**, which is the source of the stratified-hero
  approximation.  When λ_p ≈ λ_c (a stratified-uniform population
  guarantees this for some photons in any non-trivial query) the
  contribution is locally exact; over the population the bias
  averages out.

**Key invariant preserved:** the `VCMMisQuantities` stored at every
photon are wavelength-independent.  The store's `lv.mis` is identical
to what a Pel pass would produce on the same geometric path.  So
**no MIS-weight change between Pel and NM** beyond the recurrence
itself, which was already wavelength-independent before this fix.

## 6. MIS invariance argument

The Georgiev 2012 balance-heuristic recurrence (per
[VCMRecurrence.h](../src/Library/Shaders/VCMRecurrence.h) +
CLAUDE.md "MIS heuristic per integrator") reads only:

| Field | Wavelength-dep? |
|---|---|
| `v.pdfFwd` (area-measure forward sampling pdf) | NO — RISE BSDFs sample lobes by integrated luminance, not per-wavelength density |
| `v.pdfRev` (reverse pdf, same) | NO |
| `v.cosAtGen` (geometry) | NO |
| `v.position` / `v.normal` (geometry) | NO |
| `v.emissionPdfW`, `v.pdfSelect` | NO (emission *pdfs*; intensity is separate from sampling density) |
| `v.isDelta`, `v.isConnectible` | NO (structural) |

Therefore `ConvertLightSubpath` and `ConvertLightSubpathNM` produce
**byte-identical** `VCMMisQuantities` for the same `BDPTVertex` array
across wavelength choices — already asserted by
[VCMSpectralRecurrenceTest](../tests/VCMSpectralRecurrenceTest.cpp).
We extend that test to additionally verify that a `LightVertexNM`
record's `mis` field is bit-identical to the `LightVertex` produced
on the same input — closes the gap that the existing test only
covered `ConvertLightSubpath`'s Pel output type.

VCM's `VCMMis(x) = x` (balance) stays untouched.  Power-2 / optimal
MIS are **explicitly out of scope** per CLAUDE.md "MIS heuristic per
integrator" — the Georgiev recurrence is β=1-mandatory.

## 7. Threading argument (KD-tree determinism)

- The KD-tree balance and parallel-build code (`BalanceSegment`,
  `BalanceSegmentParallel`) reads only `v.ptPosition[axis]` and writes
  `v.plane` — both fields inherited verbatim from `LightVertex` into
  `LightVertexNM`.  Templating the helpers on the vertex type leaves
  the per-element layout invariant (same offset for `ptPosition` and
  `plane` in both Pel and NM), so the parallel build's already-
  documented "queries identical, partition of non-medians not byte-
  identical" property holds with no change.
- The per-photon hero sampling pulls one extra `Get1D()` dimension
  from the SAME Sobol sampler that already drives the BDPT light pass.
  No new shared state; deterministic per-pixel-seed × Sobol-index
  pair.  Two runs with the same scene + same `samples` produce the
  same per-photon hero, the same NM-store balance, the same merge
  results.
- The `Concat` / move-buffer path is reused as-is; the source-vector
  template element type just changes.

## 8. Sibling-site decision — `EvalLightRadiance<NMTag>` (audit §3.1)

The proxy is invoked at TWO sites today
([VCMIntegrator.cpp:212, :277](../src/Library/Shaders/VCMIntegrator.cpp#L212)):

1. **`LightVertexThroughput<NMTag>`** (line 277) — the merge bug.  In scope; this proposal fixes it.
2. **`EvalLightRadiance<NMTag>`** (line 212) — point/spot/directional light radiance for the NM path, because `ILight` lacks a spectral API.

Audit §3.1: site (2) is "a separate, smaller issue (point-light-
through-glass scenes only)".

**Decision: site (2) is OUT OF SCOPE for this change.**

Reasons:
- Site (2) requires extending the `ILight` virtual interface
  (`emittedRadianceNM(dir, nm)`) and migrating every ILight
  subclass — a much wider refactor that touches material-side code.
- This proposal is contained to the VCM photon-store layer; it
  doesn't need ILight changes.
- The audit explicitly marks the two sites as separable; tackling
  both in one change muddies the review.

**Mitigation:** I will (a) mark the call site with a `TODO(audit-§3.1)`
referencing the open-follow-up status, and (b) add a one-line note to
SPECTRAL_PARITY_AUDIT.md §3.1 stating "deferred — see
VCM_SPECTRAL_PHOTON_STORE_DESIGN.md §8" so the audit doc stays the
single source of truth for what's done vs not.

The audit-by-bug-pattern sweep (post-implementation) will also re-
examine whether any OTHER call sites use the proxy and missed the
audit's enumeration.

## 9. Test plan

### 9.1 Existing tests this change touches

| Test | What it asserts | Expected behaviour |
|---|---|---|
| [VCMRecurrenceTest](../tests/VCMRecurrenceTest.cpp) | Pel-side recurrence math | Untouched; PASS |
| [VCMSpectralRecurrenceTest](../tests/VCMSpectralRecurrenceTest.cpp) | NM recurrence == Pel recurrence | Extend to assert `ConvertLightSubpathNM` produces identical `lv.mis` to `ConvertLightSubpath` |
| [VCMLightVertexStoreTest](../tests/VCMLightVertexStoreTest.cpp) | KD-tree build + query correctness on `LightVertex` | Untouched; PASS.  Add a sibling test instantiating `LightVertexStoreNM` against the same grid + brute-force oracle. |
| [VCMRecurrenceTest](../tests/VCMRecurrenceTest.cpp), [VCMEyePostPassTest](../tests/VCMEyePostPassTest.cpp), [VCMLightPostPassTest](../tests/VCMLightPostPassTest.cpp), [VCMStrategyBalanceTest](../tests/VCMStrategyBalanceTest.cpp) | Various VCM correctness assertions | All Pel-side, all untouched, must continue to PASS |

### 9.2 New regression scenes

Per audit §3.5:

- **`scenes/Tests/VCM/triplecaustic_vcm_spectral.RISEscene`** (NEW) — spectral counterpart to `triplecaustic_vcm`.  Three RGB glass spheres → three dielectric caustics; canonical wavelength-separation test.  Render with `vcm_spectral_rasterizer` + `hwss true`.

Existing scenes used for regression:
- `pool_caustics_vcm.RISEscene` (switch to `vcm_spectral_rasterizer` for measurement, do not commit the switch)
- `cornellbox_vcm_spectral.RISEscene` (non-regression check)

### 9.3 Pel byte-identity gate

`scenes/Tests/VCM/triplecaustic_vcm.RISEscene` is the canonical Pel
VCM scene this fix MUST NOT regress.  Render before and after, EXR
diff with pyOpenEXR (per CLAUDE.md memory entry: `EXRReader` historically
had FP16 issues — verify with a non-RISE reader).  Tolerance: bit-
identical.  If not bit-identical, the Pel store / Pel light-pass /
Pel merge read path has drifted and the fix is rolled back.

### 9.4 Variance reduction gate

Per audit §3.2 "HWSS amplification": the fix MUST demonstrate
**measurable variance reduction on HWSS-mode caustic renders** vs
pre-fix.  Protocol per `variance-measurement` skill:

- K=4 EXR trials per (mode, fix) configuration.
- Metrics: σ² per pixel; mean σ²·T; RMSE vs PT-spectral ground truth
  at a higher sample budget.
- Scenes: `triplecaustic_vcm_spectral` (new), `pool_caustics_vcm_spectral`
  (local diff), `cornellbox_vcm_spectral` (non-regression).
- Configs: HWSS=true and HWSS=false for each.

Numbers reported in the final user report.  If the change does not
measurably reduce variance on HWSS dispersive scenes, the fix is
**incomplete** and the design returns to round-1 review.

### 9.5 Memory cost gate

Quantitatively report photon-count × bytes/photon, before vs after.
Pel mode: 0 bytes change (Pel store unchanged).  NM mode: +16 bytes
per photon.  Confirm against a representative scene's actual store
size; flag if disagreement > 5%.

## 10. Out of scope for this change

Explicitly deferred (will be called out in the user report):

- `EvalLightRadiance<NMTag>` (audit §3.1).  TODO in code + audit note.  See §8.
- Full-chain v3 storage.  Recommended only if v2 regression numbers
  show unacceptable residual error on dispersive scenes.
- VCM optimal-MIS / path-guiding extensions.  Architecturally
  blocked per CLAUDE.md "MIS heuristic per integrator" and audit
  §2.13, §2.15.
- `pixelintegratingspectral_rasterizer` photon-store work (no VCM
  variant exists; soft-deprecated chunk).

## 11. Files I expect to touch

- [src/Library/Shaders/VCMLightVertex.h](../src/Library/Shaders/VCMLightVertex.h) — `LightVertexNM` stays; verify layout.
- [src/Library/Shaders/VCMLightVertexStore.{h,cpp}](../src/Library/Shaders/VCMLightVertexStore.h) — templatize, add NM alias + explicit instantiation.
- [src/Library/Shaders/VCMIntegrator.{h,cpp}](../src/Library/Shaders/VCMIntegrator.h) — add `ConvertLightSubpathNM`, new `EvaluateMergesNM(LightVertexStoreNM&, …)` overload (or templated body), rewrite `LightVertexThroughput<NMTag>` to read `lv.throughputNM`.
- [src/Library/Rendering/VCMRasterizerBase.{h,cpp}](../src/Library/Rendering/VCMRasterizerBase.h) — add `pLightVertexStoreNM`, factor light-pass into a per-mode helper, leave Pel mode byte-identical.
- [src/Library/Rendering/VCMSpectralRasterizer.{h,cpp}](../src/Library/Rendering/VCMSpectralRasterizer.h) — own `PreRenderSetup`/`OnProgressivePassBegin` that build the NM store; `IntegratePixel` passes `pLightVertexStoreNM` to `EvaluateMergesNM`.
- [tests/VCMSpectralRecurrenceTest.cpp](../tests/VCMSpectralRecurrenceTest.cpp) — extend to cover `ConvertLightSubpathNM` mis invariance.
- [tests/VCMLightVertexStoreTest.cpp](../tests/VCMLightVertexStoreTest.cpp) — extend (or add sibling) for `LightVertexStoreNM`.
- [scenes/Tests/VCM/triplecaustic_vcm_spectral.RISEscene](../scenes/Tests/VCM/) — NEW.
- [docs/SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) — §3 marked DONE with outcome block.
- [docs/VCM.md](VCM.md) — "spectral merges use luminance proxy" v1 limitation entry updated to point at this design's outcome.

Build-project sync per CLAUDE.md "Source-file add/remove": no new
.cpp/.h files expected (everything is edits in place).  If round-1
review surfaces a reason to add a file (e.g. NM-store split into its
own .cpp), all five build projects get updated.

## 12. Validation gate before declaring done

All of the following must be true:

- [ ] `make -C build/make/rise -j8 all` warning-free on clean rebuild
- [ ] `make tests && ./run_all_tests.sh` — all PASS, including extended
  `VCMSpectralRecurrenceTest` and `VCMLightVertexStoreTest`
- [ ] Xcode `RISE-GUI` clean rebuild warning-free (per CLAUDE.md
  compiler-warnings-are-bugs)
- [ ] Pel byte-identity on `triplecaustic_vcm.RISEscene` (EXR diff = 0)
- [ ] Measurable variance reduction (σ²·T or RMSE-vs-reference) on at
  least one of the regression scenes; HWSS=true and HWSS=false both
  examined
- [ ] Memory cost: +16 B/photon NM, 0 B Pel, both within 5% of predicted
- [ ] Round-2 adversarial review (3-of-3 reviewers agree fix is sound)
- [ ] `audit-by-bug-pattern` sibling sweep complete with documented
  decision on `EvalLightRadiance<NMTag>` and any other sites it surfaces
- [ ] `git status --short` shows ONLY my files modified (no auto-commit)
- [ ] Audit doc §3 updated to DONE with outcome block

---

## 13. Round-1 adversarial review verdict — **v2 REJECTED**

Three independent reviewers were asked to refute the v2 design with
distinct orthogonal focuses: (R1a) numerical correctness vs PBRT-v4
and SmallVCM; (R1b) threading and KD-tree determinism; (R1c) HWSS
bundle interaction.  Each was given the design doc, the audit §3,
the relevant source files, and the CLAUDE.md MIS-heuristic guidance.

**Verdict: 3-of-3 refuted v2.**  None could be patched without
dropping the load-bearing claims of v2 itself.  Catalog:

| # | Source | Finding | Status |
|---|---|---|---|
| 13.1 | R1a-A,B | Stratified-hero `E[product] = product[truth]` is mathematically false on dispersive/absorbing scenes.  Concrete: dye glass `T(λ)` with `α(450)=8, α(700)=0.5, d=1` — band-average over-counts 450 nm by ~900× and under-counts 700 nm by ~2×.  v2 is **worse** than the proxy on this scene class. | Confirmed |
| 13.2 | R1a-C,D | `lv.mis` byte-identity to the Pel store is **false** in RISE.  `BDPTIntegrator.cpp:1322-1339, 2045-2065, 5267-5343` show the NM path selects scatter lobes by `krayNM` (hero λ) while Pel uses `MaxValue(kray)`.  Multi-lobe SPFs (coated glass, Fresnel-blended dielectrics — the typical caustic generators) get different `selectProb` → different `pdfFwd` → different `lv.mis`.  The proposal's §6 invariance table is wrong on a row it asserted as proven. | Confirmed |
| 13.3 | R1a-F | Sodium-vapor lamp through dye glass: spectral emitter spikes at 589.0/589.6 nm; with uniformly stratified photon-hero across 380-780 nm, ~99% of photons sample wavelengths the emitter doesn't emit; v2 reads `lv.throughputNM` (zero Le at those λ_p) and multiplies by nonzero `cameraBsdf(589)`.  Result: caustic disappears or fireflies.  Proxy at least preserved magnitude. | Confirmed |
| 13.4 | R1a-F (audit §3.1 sibling) | v2 inherits the `EvalLightRadiance<NMTag>` luminance proxy at the photon SOURCE for ILight emitters (point / spot / directional).  The proposed §8 "deferral" of audit §3.1 means v2 stores a proxy-corrupted Le into `throughputNM` at v[0] — so v2 ships **broken at the source** on every delta-position-emitter scene. | Confirmed — audit §3.1 must be IN scope |
| 13.5 | R1b-A | Sobol stream collision.  `BDPTIntegrator.cpp:4843` shows `GenerateLightSubpath` calls `sampler.StartStream(0)` immediately, which resets `dimension = 0`.  A pre-call `sampler.Get1D()` for the photon hero is silently discarded; the photon hero ends up sharing the SAME Sobol coordinate as light-position-u.  Structural colour-vs-spatial-position bias on dispersive scenes (purple top, red bottom).  Concrete fix would require a reserved stream phase ID outside the existing SMS / NEE phase range. | Confirmed |
| 13.6 | R1b-G + R1c-C | **Dispersive-photon contamination.**  v2 stores photons whose light-side path geometry was traced at η(λ_p).  At merge, the eye reads `lv.throughputNM` for a contribution attributed to λ_eye ≠ λ_p.  But the photon's positions/normals/refraction angles WERE FIXED BY λ_p — they are geometrically wrong for λ_eye.  On the audit's own §3.5 motivating scenes (triplecaustic, pool dispersion, prism), v2 is a **correctness regression vs the proxy**.  The proxy was wrong but at least its error was magnitude-O(1); v2's error is geometrically wrong-path. | Confirmed — fatal |
| 13.7 | R1c-A | The audit's "HWSS speedup degrades to ≈0" framing is a **bias** description mislabelled as variance.  Inside one HWSS bundle of 4 wavelengths, both the proxy AND v2 produce the same scalar `lv.throughput*` (luminance vs hero) — constant across the bundle.  v2 changes the photon-population expectation but does **not recover bundle-internal companion variance** on the merge path.  v2 is therefore a **bias fix only**.  HWSS variance recovery requires per-companion re-evaluation, which is v3. | Confirmed — re-frames the entire v2 value proposition |
| 13.8 | R1c-G | Heavy Beer-Lambert volumes: eye-side connection transmittance at λ_eye × light-side photon throughput at λ_p multiplies wavelength-mismatched transmittances.  Error grows with optical depth.  v2 is worse than the proxy on this regime. | Confirmed |

**Reviewer consensus statement (R1c, paraphrased):** "v2 is a bias fix
that strictly improves the proxy on non-dispersive scenes and is a
stratified-hero approximation elsewhere; v3 remains the principled
HWSS-correct path."

**Implication for §2 and §7-Q1.**  The audit's §7-Q1 recommendation
("per-photon-hero as v2 default; v3 only if regression scenes show
residual error") rests on the load-bearing assumption that v2 is at
worst neutral on dispersive scenes.  Findings 13.1, 13.6, 13.8
demolish that — v2 is **strictly worse than the proxy** on the
audit's own motivating scenes.  Therefore:

- **Do not ship v2 as a bias-fix-only patch.**  The scenes it
  IMPROVES (uniform-IOR wavelength-dependent absorption with no
  eye-side volume) are not the scenes that motivate the §3 audit.
- **The audit's §3 recommendation needs to be re-written** to
  reflect that v2 was investigated and rejected; v3 is the path.
- **The "stratified-hero" terminology should be retired** in favour
  of "per-photon hero with population averaging", because the latter
  honestly conveys that it is a population-asymptotic bias fix,
  not a per-bundle MC noise reducer.

## 14. v3 architecture sketch — chain-storing per-photon design

A v3 design that addresses every confirmed problem in §13.  This is
**a sketch for cost estimation**, not a final design — v3 must go
through its own round-1 adversarial review before any code is
written.

### 14.1 Storage

Each NM-store photon carries:

```cpp
struct LightVertexNM : public LightVertex {
    Scalar               throughputNM;    // at photon hero (the merge endpoint)
    Scalar               nm;              // hero used during photon trace
    bool                 isDispersive;    // any upstream specular vertex was wavelength-dep
    PhotonChainPrefix*   pChain;          // see §14.2
};

struct PhotonChainPrefix {
    // Lightweight projection of the BDPTVertex prefix needed to drive
    // RecomputeSubpathThroughputNM at a companion wavelength.
    // Excludes BDPT-only fields (path-guiding metadata, throughput
    // RGB, MIS bookkeeping) that the recompute doesn't read.
    std::vector<ChainVertex> verts;  // ~80-100 B/vertex × bounces
};

struct ChainVertex {
    Point3        position;
    Vector3       normal;
    Vector3       geomNormal;
    Point2        ptCoord;          // for IOR/painter wavelength lookup
    Point2        ptCoord1;
    bool          bHasTexCoord1;
    Point3        ptObjIntersec;
    RISEPel       vColor;
    bool          bHasVertexColor;
    BDPTVertex::Type type;
    bool          isDelta;
    Scalar        mediumIOR;
    bool          insideObject;
    const IMaterial* pMaterial;
    const IObject*   pObject;
    const IMedium*   pMediumVol;
    const IPhaseFunction* pPhaseFunc;
    const IObject*   pMediumObject;
    Scalar        sigma_t_scalar;
    const ILight*    pLight;
    const IObject*   pLuminary;
    const IRadianceMap* pEnvLight;
    Scalar        throughputNM;     // at hero
};
```

Estimated `sizeof(ChainVertex) ≈ 200 B`.  Typical caustic photon depth
2-4 bounces → chain cost 400-800 B per photon, on top of the
`LightVertexNM` base (~170 B).  **Total ~570-970 B/photon** vs the
current `LightVertex` ~150 B — **a 4-7× store memory expansion in
spectral mode**, matching the audit's 10× upper bound for long
subpaths.

### 14.2 Light pass

In `VCMRasterizerBase::PreRenderSetup` / `OnProgressivePassBegin`,
the spectral subclass:

1. Reserves a **dedicated Sobol stream phase** for photon-hero
   sampling (e.g. phase 64 — outside the existing SMS/NEE/path-
   guiding ranges; needs verification against the full stream-table
   audit per finding 13.5).
2. For each photon: `sampler.StartStream(<heroPhase>); heroU = sampler.Get1D();`
   THEN `sampler.StartStream(0);` (restores the dimension reset that
   `GenerateLightSubpath` expects) THEN
   `pGen->GenerateLightSubpathNM(..., heroNM, &swl)`.
3. While building each photon's chain, mirror the `BDPTVertex` array
   into the `ChainVertex` array (drop the throughput-RGB, path-
   guiding, MIS-bookkeeping fields).
4. Set `isDispersive` true if `HasDispersiveDeltaVertex(verts, heroNM,
   heroNM + ε) || HasDispersiveDeltaVertex(verts, heroNM, heroNM − ε)`
   — i.e. the photon traversed a wavelength-dependent IOR at all.
5. Address finding 13.4 by either (a) extending the `ILight`
   interface to add `emittedRadianceNM` virtuals and migrating every
   ILight implementation, or (b) re-tracing the light endpoint's
   wavelength-dependent `Le` from the stored chain at merge time
   (more storage but less interface churn).  Option (a) is the
   structural fix per the audit; option (b) is a tactical patch.

### 14.3 Merge

`EvaluateMergesNM(LightVertexStoreNM&, …, eyeNM)`:

```
for each candidate photon p in the radius query:
    if (p.isDispersive && |p.nm - eyeNM| > δ):
        recompute_path = COPY(p.pChain)
        if HasDispersiveDeltaVertex(recompute_path, p.nm, eyeNM):
            continue            // can't share geometry; skip
        RecomputeSubpathThroughputNM(recompute_path, isLightPath=true, p.nm, eyeNM, scene, caster)
        photon_throughput = recompute_path.back().throughputNM
    elif p.isDispersive:        // hero-matches: exact path
        photon_throughput = p.throughputNM
    else:                       // non-dispersive: throughput is wavelength-independent
        photon_throughput = p.throughputNM
    accumulate(cameraBsdf(eyeNM) * photon_throughput * weight)
```

This handles **every** finding:
- 13.1, 13.3, 13.8: companion mismatches re-evaluate the actual path,
  so throughput is correctly attributed to λ_eye.
- 13.2: still needs the lobe-selection wavelength-dependence fix
  (separate sub-task — make NM lobe selection match Pel's
  `MaxValue(kray)`, OR migrate Pel to NM's per-wavelength selection
  with appropriate MIS reweighting).  This is independent of v3 and
  may need its own design + review round.
- 13.4: option (a) ILight spectral migration fixes the source;
  option (b) chain replay reaches back to v[0] to re-evaluate Le.
- 13.5: dedicated Sobol stream phase.
- 13.6: dispersive photons either re-evaluate (correct) or skip
  (admissible but increases variance — measure both options).
- 13.7: v3 DOES recover bundle-internal HWSS companion variance
  because each companion's merge against a dispersive photon
  re-evaluates the throughput at the companion's wavelength.

### 14.4 Cost estimate

Honestly accounting for everything:

| Sub-task | Estimate | Notes |
|---|---|---|
| Templatize `LightVertexStore` | 2 days | Already specced in v2; reusable |
| `LightVertexNM` + `ChainVertex` + chain capture | 1 week | New struct, validation against `BDPTVertex` field-by-field |
| Spectral light pass + Sobol stream | 3 days | Stream-phase audit + new dispatcher mode |
| `EvaluateMergesNM` with dispersive replay | 1 week | Per-photon `RecomputeSubpathThroughputNM` invocation, performance tuning |
| `ILight::emittedRadianceNM` migration (option a) | 1-2 weeks | Cross-cuts all ILight subclasses; ABI-preserving API evolution per the skill |
| OR chain-replay source Le (option b) | 3 days | Cheaper but stores per-photon Le-state |
| Lobe-selection wavelength-dependence (13.2) | **separate design + review round** | This is a deep correctness change to BDPT/PT NM; may not be v3-coupled |
| Regression scenes + measurement | 1 week | Per audit §3.5 + finding 13.8 added scenes |
| Two adversarial review rounds (design + implementation) | 1 week | Per the original prompt's discipline |
| Total | **4-6 weeks** | Audit's 3-4 week estimate was for v3 alone; the lobe-selection sibling pushes higher |

### 14.5 Out-of-scope-for-v3 audit items rediscovered

The reviewers surfaced TWO items the original audit treated as resolved or unrelated, that are NOT:

- **§3.1 ILight Le proxy**: cannot be deferred — v2 and v3 both inherit corrupted emission at the photon source.  Must be either fixed or admitted as a v3 limitation.
- **NM lobe-selection probability divergence from Pel** (finding 13.2): this is a Pel-vs-NM correctness gap in BDPT, PT, and VCM, not specific to merging.  Needs its own design discussion and may be the highest-impact correctness issue in the spectral path overall — coloured glass scenes ALREADY render at slightly-wrong intensity in BDPT-spectral, just for a reason the parity audit didn't catch.

## 15. Recommendation to the user

Three options:

**(A) Stop here.**  v2 was investigated and rejected; v3 is multi-week
scope.  Mark audit §3 as "in design; v2 refuted; v3 architecture
sketched; no implementation yet."  The VCM-spectral proxy stays.
**Pro:** no risk, no code, the user decides when to invest the
multi-week budget.  **Con:** the §3 correctness gap stays open.

**(B) Commit to v3.**  Implement the §14 design over 4-6 weeks of
focused work; budget the second adversarial review on design and on
implementation; deliver per audit §3.5 + finding 13.8 scenes.
**Pro:** the only path that closes §3 correctly.  **Con:** large
investment; touches BDPT/PT/VCM NM paths via the lobe-selection
sibling.

**(C) Ship a documented luminance-projection that is "honestly wrong but
predictable".**  Keep the current proxy, add explicit per-photon nm
storage so MIS-debug tools can flag wavelength mismatches, log
warnings on dispersive scenes that the merge proxy is approximate.
Don't claim to fix §3 — just make the failure mode visible.
**Pro:** small, low-risk, makes existing code's known-wrong behaviour
explicit.  **Con:** does not close §3; could be perceived as scope
inflation.

My recommendation: **(A) — stop here, push the multi-week v3 to the
next focused session.**  Reasons:
1. The audit's recommendation was based on a flawed v2 framing; the
   honest re-framing is that §3 is multi-week scope, not 2-week.
2. The lobe-selection wavelength-dependence (13.2) might also affect
   BDPT-spectral and PT-spectral correctness for coloured-glass
   scenes, regardless of VCM merging.  Surfacing this finding now
   matters more than rushing v3 in one session.
3. The "be slow and right rather than fast and probably-wrong"
   instruction in the prompt explicitly authorizes this stance.
