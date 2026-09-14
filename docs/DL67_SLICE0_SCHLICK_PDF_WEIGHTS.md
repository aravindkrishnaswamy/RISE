# DL-67 Slice 0 — `SchlickSPF::Pdf`/`PdfNM` lobe weights

Scope: make `SchlickSPF::Pdf`/`PdfNM`'s aggregate-pdf lobe weights
consistent with the weights `PTScatterSelectWeight`/`PTRandomlySelect`
actually use to pick a lobe at runtime. This is the shared prerequisite
identified in the DL-67/DL-69 design note's "Slice 0" — it does **not**
touch DL-69 (BDPT/VCM ordinary throughput) or the rest of DL-67 (PT/BDPT
guided RIS/one-sample candidates), which consume this fix but are
separate slices.

## 0. What `Scatter()` emits

`SchlickSPF::Scatter`/`ScatterNM` (`SchlickSPF.cpp`) draw the diffuse ray
and the specular ray **unconditionally, every call** (subject only to
geometric accept-checks), and push BOTH into the `ScatteredRayContainer`:

- Diffuse: `d.kray = pDiffuse->GetColor(ri)` — a pure function of the
  shading point `ri`. It does **not** vary with the diffuse lobe's own
  sampled direction. `d.pdf = cosTheta/pi` (cosine-weighted hemisphere).
- Specular: `s.kray = rho + (1-rho)*fresnel`, where `fresnel =
  (1-hdotk)^5` and `hdotk = dot(h, wi)`, `h = half-vector(wi, wo_S)` —
  a function of the specular lobe's **own** sampled direction `wo_S`.
  `s.pdf = ComputeSchlickSpecularPdf(ri, wo_S, r, p)` (the Schlick
  half-vector-sampling density).

(The anisotropic/per-channel branch emits up to three per-channel
specular rays instead of one; each uses the identical fresnel formula,
so the analysis below is unaffected — Pdf() already collapses this case
to a single averaged-roughness specular lobe, and this slice does not
change that pre-existing collapse.)

Neither call resamples: both rays are drawn from the SAME `Scatter()`
invocation and both survive into the container (subject to their own
accept-checks) before anything picks between them.

## 1. `q_I` as `PTScatterSelectWeight`/`PTRandomlySelect` compute it

The integrator does the actual lobe selection, later, via
`ScatteredRayContainer::RandomlySelect` (`ScatteredRayContainer.cpp`),
called from `PTRandomlySelect<Tag>` (`PathTracingIntegrator.cpp:1338-
1342`). Both `RandomlySelect`'s internal weight and
`PTScatterSelectWeight<PelTag>`/`<NMTag>` (`:1354-1356`) use
`MaxValue(kray)` (Pel) / `krayNM` (NM) — evaluated on the **realized**
kray of each already-sampled ray. So the selection weight for lobe `I`
at this call is:

```
w_D = MaxValue(rd)                                     -- constant, independent of any draw
w_S(wo_S) = MaxValue(rho + (1-rho)*fresnel(wi, wo_S))   -- a function of the specular lobe's OWN drawn direction
```

and the realized selection probability is `q_D = w_D/(w_D+w_S(wo_S))`,
`q_S = w_S(wo_S)/(w_D+w_S(wo_S))`.

## 2. The true generating density `sum_I q_I p_I(omega)`

Because `w_D` never depends on which direction the diffuse lobe drew,
and `w_S` is a **deterministic** function of the specular lobe's own
drawn direction alone (not of the diffuse draw, and not of any other
randomness), the two lobes' conditional selection probabilities behave
asymmetrically when we ask "what is the density of the FINAL, selected
direction landing at some direction `omega`, for an arbitrary `omega`
(not necessarily anything actually drawn)?":

- **Specular contribution, exact.** If the specular lobe's own draw
  happens to be `omega`, the probability specular wins is
  `q_S(omega) = w_S(omega) / (w_D + w_S(omega))` — a function of
  `omega` ALONE (via the half-vector fresnel term), with **no
  dependence on the diffuse lobe's unobserved draw** (since `w_D` is a
  known constant, not something that needs to be averaged over). Pdf()
  is asked to evaluate at a specific `omega = wo`, so it can compute
  `w_S(wo)` exactly — no resampling needed, no approximation needed.

- **Diffuse contribution, an expectation.** If the diffuse lobe's own
  draw happens to be `omega`, the probability diffuse wins is
  `q_D(omega) = w_D / (w_D + w_S(omega_S))`, where `omega_S` is the
  SPECULAR lobe's own, statistically INDEPENDENT draw for that same
  `Scatter()` call — not `omega`, and not observable from a bare query
  `Pdf(ri, omega)`. This makes the diffuse lobe's true per-draw
  selection probability a genuine expectation over the specular
  sampling distribution:
  `C_D = E_{omega_S ~ p_S}[ w_D / (w_D + w_S(omega_S)) ]` — a CONSTANT
  (it does not depend on `omega` at all, since `w_D` doesn't), but one
  that cannot be evaluated in closed form without integrating over the
  whole specular lobe.

So the true marginal density of SchlickSPF's own two-stage generation
procedure, evaluable at an arbitrary `omega`, is:

```
f_true(omega) = C_D * p_D(omega) + q_S(omega) * p_S(omega)
```

(`p_D`, `p_S` are the diffuse/specular sampling densities.) This
integrates to exactly 1 over the hemisphere provided `C_D` is the exact
expectation above (`∫f_true = C_D + E[q_S(omega_S)] = C_D + (1-C_D) =
1`) — a genuine, well-posed density, not a heuristic.

## 3. What `Pdf()` returned pre-fix

```
dWeight = MaxValue(rd)
sWeight = MaxValue(rs)                    -- RAW painter albedo, no fresnel term at all
return (dWeight*diffusePdf + sWeight*specPdf) / (dWeight+sWeight)
```

This used neither `q_S(omega)` (exact, cheaply available) nor a
principled approximation of `C_D` — `MaxValue(rs)` is not even a
plausible proxy for either quantity, since it omits the fresnel term
entirely (`rs` alone, not `rho+(1-rho)*<something>`).

## 4. The minimal change

`C_D` cannot be computed in closed form (it requires integrating
`w_S(omega_S)` over `p_S`), but the codebase already has a standard,
shipped proxy for "the expected Fresnel-boosted reflectance under an
unspecified angular measure": `SchlickFresnelAvg(F0) = F0 + (1-F0)/21`
(`GGXSPF.cpp:77-82` / `GGXBRDF.cpp:77`, DL-64's hemispherical-average
Schlick reflectance). Reusing it:

```
sWeightAvg   = SchlickFresnelAvg(rs)                              -- proxy for E_{omega_S~p_S}[w_S(omega_S)]
sWeightExact = rs + (1-rs)*fresnelAtWo                            -- EXACT w_S(wo), no averaging
cD = dWeight / (dWeight + sWeightAvg)                             -- constant, approximates C_D
qS = sWeightExact / (dWeight + sWeightExact)                      -- exact q_S(wo)

Pdf(wo) = cD * diffusePdf(wo) + qS * specPdf(wo)
```

**This is NOT** the design note's originally literal phrasing
("`MaxValue(rho+(1-rho)*SchlickFresnelAvg(rs))`" used as a SINGLE
replacement for `sWeight` everywhere). That phrasing double-counts
`rho`: `SchlickFresnelAvg(rho)` **already** equals the hemisphere
average of the full `rho+(1-rho)*fresnel` expression (see the
docstring on `SchlickFresnelAvg` in `GGXBRDF.cpp`: `F_avg = 2∫[F0 +
(1-F0)(1-mu)^5] mu dmu = F0 + (1-F0)/21` — `F0` IS `rho`, already
folded in). Wrapping it in a second `rho+(1-rho)*X` would apply the
Fresnel blend twice. The fix above uses `SchlickFresnelAvg(rs)` directly
as the (already-complete) averaged reflectance, and — the second,
larger correction over the design note's literal text — uses it **only**
for the diffuse coefficient's denominator, not for the specular
coefficient, which gets the CHEAPER, EXACT, per-query-direction value
instead of any averaged approximation.

`fresnelAtWo` is computed by rebuilding the exact half-vector
`h=normalize(wi+wo)` and `hdotk=dot(h,wi)`, mirroring
`ComputeSchlickSpecularPdf`'s own construction. Algebraic proof this
recovers exactly what `GenerateSpecularRay` would have computed had it
happened to draw this `wo`: `GenerateSpecularRay` reflects the incoming
ray `d=ri.ray.Dir()` about its sampled half-vector `h` to get
`wo = d - 2(d.h)h`; substituting `wi=-d` gives
`wi+wo = 2(h.wi)h`, so whenever `hdotk=h.wi>0` (the sampler's own accept
condition), `normalize(wi+wo)=h` — i.e. the query-side reconstruction
recovers the SAME `h`, and therefore the same fresnel, for any `wo` the
sampler could legitimately have produced. No approximation is needed on
the specular side at all.

## 5. Consumer impact

`ISPF::Pdf()`/`PdfNM()` is called from:

- **NEE partner** (`IMaterial::Pdf` → `SchlickSPF::Pdf`, via
  `LightSampler.cpp`'s BSDF-side MIS weight). Unedited this slice; picks
  up the corrected density automatically through the shared interface.
  Its own MIS partition is unaffected in kind (still `w_bsdf+w_nee=1`
  for any valid density), only the NUMBER changes, and only at
  `SchlickSPF` vertices.
- **BDPT `pdfRev`** (`PathValueOps::EvalPdfAtVertex` →
  `PathVertexEval.h` → `pSPF->Pdf(...)`). Read-only this slice (DL-69's
  own `pdfFwd` redefinition is a separate slice); the aggregate density
  it reads is now the corrected one.
- **PT/BDPT guided RIS candidate 1 / guided-accepted branch**
  (`PTEvalPdfAtSurface` → `ISPF::Pdf`). DL-67's own remaining slices
  (candidate 0 / kept-BSDF fix) consume this improved density for free,
  as the design note anticipated — not edited here.
- **VCM** — same `ISPF::Pdf()` interface, same effect as BDPT.
- **`SPFPdfConsistencyTest`** (`tests/SPFPdfConsistencyTest.cpp`) — the
  Schlick row's flags (`skipCrossVal=true, skipChi2=true`) are
  UNCHANGED (see §7 below for why); the row's comment and its cited
  integral numbers were updated to the corrected post-fix values.
- **New `SchlickSPFPdfConsistencyTest`** — added this slice; see §6.

## 6. Red-proof

Two independent checks, both run against master (`32824325`, unfixed
`SchlickSPF.cpp`) for RED and against the fixed tree for GREEN, via
`git diff > patch; git checkout HEAD -- SchlickSPF.cpp; <rebuild+run>;
git apply patch; <rebuild+run>` (never `git stash`, per the slice's
common rules).

### 6.1 Closed-form replica (`tests/SchlickSPFPdfConsistencyTest.cpp` Part 1)

At 5 hand-picked `(theta, rd, rs, roughness, isotropy)` points, 37
query directions each: an INDEPENDENT replica of the derived formula
(`DerivedPdfReplica`, extracting `specPdf` via an `rd=0` twin material —
at `rd=0`, `cD=0` and `qS=1` identically, so `Pdf_at_rd0(wo) ==
specPdf(wo)` exactly, no equation-solving needed) is compared against
the shipped `spf->Pdf()`.

- **Pre-fix (master):** `agreeWithDerived=0/37` at every one of the 5
  points (shipped code uses the old formula, which the closed-form
  replica of the DERIVED formula correctly does not match).
- **Post-fix:** `agreeWithDerived=37/37` at every point (shipped code
  matches the derivation to < 1e-6 relative error).

### 6.2 Statistical, per-lobe, ground-truth lower bound (Part 2/3)

Replicates `PTScatterSelectWeight`/`PTRandomlySelect`'s ACTUAL selection
rule inline (same `MaxValue(kray)` weighting, same
`ScatteredRayContainer` production code path via real `Scatter()`/
`ScatterNM()` calls), and checks, for each non-delta ray in a real
Scatter() call, that `Pdf(ri, wo)` at that ray's own realized direction
lower-bounds that lobe's realized per-call contribution
`weight_j*pdf_j/totalWeight` — split by lobe type (diffuse vs specular),
50000 draws per angle, seeded (`RandomNumberGenerator(424242)` RGB,
`(909090)` NM) for reproducibility.

| | Pre-fix (master) | Post-fix |
|---|---|---|
| RGB specular @ 30deg | 64/37070 fail, maxRel 4.20% | **0/37070 fail** |
| RGB specular @ 60deg | 2857/32008 fail, maxRel 21.43% | **0/32008 fail** |
| RGB diffuse @ 30deg | 9409/50000 fail, maxRel 29.78% | 9709/50000 fail, maxRel 32.28% |
| RGB diffuse @ 60deg | 15354/50000 fail, maxRel 34.20% | 15502/50000 fail, maxRel 36.70% |
| NM specular @ 30deg | 79/36966 fail, maxRel 4.10% | **0/36966 fail** |
| NM specular @ 60deg | 2846/32037 fail, maxRel 21.37% | **0/32037 fail** |

The specular side goes from a real, non-trivial failure rate (up to
8.9% of checks, max relative error 21.4%) to **exactly zero**, matching
§2's proof that the specular coefficient is exactly computable. The
diffuse side is essentially unchanged (a ~0.3-2.5pp shift, consistent
with `SchlickFresnelAvg` being an imperfect but reasonable proxy for the
true `p_S`-weighted expectation `C_D` — see the standalone diagnostic in
§7) — this is the PROVEN-INHERENT residual from §2's derivation, not a
regression: no constant coefficient can satisfy a per-call lower bound
against a randomly-varying per-call denominator. The new test's gate
allows this residual up to 40%/0.50 (its pre-fix magnitude) so a genuine
future regression is still caught, without asserting the mathematically
impossible.

## 7. Why full strict `SPFPdfConsistencyTest` gating stays off for Schlick

A standalone diagnostic (`Monte-Carlo estimate of the TRUE C_D` at
`rd=0.5, rs=0.3`, roughness 0.3, isotropy 0.8, 2,000,000 draws per
angle) shows `SchlickFresnelAvg`-based `cD` tracks the true `C_D`
reasonably but not exactly:

| theta | true C_D (MC) | cD (SchlickFresnelAvg) |
|---|---|---|
| 10 deg | 0.6248 | 0.6 |
| 30 deg | 0.6238 | 0.6 |
| 45 deg | 0.6200 | 0.6 |
| 60 deg | 0.6085 | 0.6 |
| 75 deg | 0.5818 | 0.6 |

This ~2-4% gap between the true (incidence-angle-dependent) expectation
and the flat hemisphere-average proxy is why `SPFPdfConsistencyTest`'s
Schlick row keeps `skipCrossVal=true`/`skipChi2=true` (its "multi-lobe
lower-bound" cross-val and full chi2 both check the DIFFUSE side too,
which is provably not exactly satisfiable — see §2/§6.2). Improving `cD`
past `SchlickFresnelAvg`'s flat proxy (e.g. an incidence-angle-aware
term) is future work, not part of this slice's scope (the task only
asks for consistency with `PTScatterSelectWeight`'s realized weights,
which is fully and provably achieved on the specular side, the only
side where an exact match is even possible).

## 8. Sibling audit — other multi-lobe SPFs

One-sentence bug pattern: *`Pdf()` weights a multi-lobe mixture by the
raw, angle-independent painter albedo(s) instead of the REALIZED
per-draw `MaxValue(kray)` weight `PTScatterSelectWeight`/
`PTRandomlySelect` actually select by.*

| SPF | `Scatter()` kray direction-dependence | `Pdf()` weight | Verdict |
|---|---|---|---|
| `SchlickSPF` | diffuse constant; specular = `rho+(1-rho)*fresnel(wo)` | **fixed this slice**: exact fresnel(wo) for specular, `SchlickFresnelAvg` for diffuse's constant | Fixed |
| `SchlickBRDF` | n/a — no `Pdf()` method exists (BRDF classes are eval-only; importance sampling is the SPF's job) | n/a | Not applicable — nothing to fix |
| `GGXSPF`/`CoatedSPF` | every lobe's `.pdf` field set to the SAME `mixPdf` (the full mixture density) | uses that same `mixPdf`/`ComputeLobeWeights` machinery | Immune (already established, DL-69 ledger; reconfirmed here — `GGXSampleEvaluationConsistencyTest` 48/0) |
| `CookTorranceSPF` | selects a lobe via `ComputeLobeWeights(wdRGB, wsRGB, Ess_i)` computed from `ri` alone (incidence angle `cosWi`, painter albedos) BEFORE sampling — a genuinely fixed, ri-only weight, unlike Schlick's post-hoc realized-kray selection | Pdf() calls the SAME `ComputeLobeWeights` with the SAME inputs | Immune — different (select-first) architecture, not the DL-67 pattern. Confirmed: `CookTorrance*` rows already run with `exactSelectedPdf=true`, 0 cross-val failures. |
| `PolishedSPF` | specular kray = `tau*Rs`, diffuse kray = `Rd*(1-Rs)`, `Rs` = Fresnel reflectance computed from the INCIDENT geometry | `Pdf()`/`PdfNM()` ALREADY compute the same `Rs` the same way and weight by it (`PolishedSPF.cpp` comment: "Weight by MaxValue(kray) to match RandomlySelect") | Already fixed (pre-existing, not this slice) — not in-pattern |
| `WardIsotropicGaussianSPF` / `WardAnisotropicEllipticalGaussianSPF` | BOTH `d.kray` and `s.kray` are pure `GetColor(ri)` — no direction-dependent modulation at all | `MaxValue(rd)` / `MaxValue(rs)` | Immune (coincidentally correct: kray happens to already be direction-independent for both lobes, so raw-albedo weighting IS the exact realized weight) |
| `IsotropicPhongSPF` | diffuse constant; **specular kray = `Rs*(N+2)/(N+1)*cos_o`**, `cos_o=dot(wo,n)` — direction-dependent, exactly analogous to Schlick's fresnel term and just as cheaply computable at a query `wo` | `MaxValue(rd)` / `MaxValue(rs)` — raw albedo, NO `cos_o` term at all | **In-pattern — filed as DL-98 below** |
| `AshikminShirleyAnisotropicPhongSPF` | diffuse and specular kray both direction-dependent (`specFactor=fresnel/max(cos_i,cos_o)`, `diffFactor` similar) | ALREADY partially mitigated: weights are evaluated "at the mirror reflection direction" (`cos_o=cos_i`) rather than raw albedo, with its own comment explaining why — but this is still an approximation, since `Pdf()` receives the actual query `wo` and could evaluate `specFactor`/`diffFactor` exactly there instead of at the mirror proxy | Partially in-pattern (already better than raw-albedo, but not exact) — filed as DL-99 below, lower priority |
| `TranslucentSPF` | disjoint-hemisphere lobes (entry: front+trans; exit: trans+front) | per DL-69's ledger analysis, disjoint support means no lobe overlap to mis-weight | Not in this pattern (per DL-69) |
| `CompositeSPF` | `Pdf()` is a documented hard-coded 50/50 placeholder (`SPFPdfConsistencyTest.cpp`'s own comment) | n/a — not attempting to match `RandomlySelect` at all | Out of scope (pre-existing, documented placeholder, not a DL-67-shaped regression) |
| `FabricSPF`/`WeaveSPF` | mixture Pdf is the REAL, full, priced mixture per `docs/CLOTH_FABRIC_DESIGN.md` §9.2's "sample-then-reprice" recipe (verified: `SPFPdfConsistencyTest.cpp`'s Fabric/Weave rows run with `skipCrossVal=false`, 0 mismatches) | n/a | Not in this pattern |

New debt rows (this slice's assigned id block DL-98/DL-99; not fixed —
scope is Schlick only):

- **DL-98** — `IsotropicPhongSPF::Pdf`/`PdfNM` weight the diffuse/
  specular mixture by raw `MaxValue(rd)`/`MaxValue(rs)`
  (`IsotropicPhongSPF.cpp:293-294`), but `Scatter()`'s specular kray is
  `Rs*(N+2)/(N+1)*cos_o` (`:141-142`) — a function of the query
  direction's own `cos_o=dot(wo,n)`, exactly analogous to
  `SchlickSPF`'s fresnel term and just as cheaply exact at a query
  `wo`. `SPFPdfConsistencyTest.cpp`'s existing `IsotropicPhong` row
  already documents "massive cross-val divergence (38k-45k mismatches
  out of 50k samples)". Same fix shape as this slice's `SchlickSPF` fix
  (exact per-wo weight for the direction-dependent lobe's coefficient;
  a hemisphere/lobe-average constant, if one is needed, for any
  direction-independent lobe's coefficient — here `dWeight=MaxValue(rd)`
  is already exact since diffuse kray is direction-independent, so NO
  averaging approximation is even needed on the diffuse side, unlike
  Schlick). Size S, physics-bias, user-visible (`phong_material`
  guided/BDPT/VCM renders and NEE partner weight). Recipe: mirror
  `tests/SchlickSPFPdfConsistencyTest.cpp`'s Part 2/3 per-lobe
  ground-truth check against `IsotropicPhongSPF`.
- **DL-99** — `AshikminShirleyAnisotropicPhongSPF::Pdf`/`PdfNM`
  evaluate their Fresnel/diffuse-factor lobe weights at the MIRROR
  REFLECTION direction (`AshikminShirleyAnisotropicPhongSPF.cpp:417-
  451`, `cos_i` only) rather than at the actual query `wo` — an
  existing, better-than-raw-albedo mitigation (its own comment: "gives
  constant weights that are much more representative than raw Rs/Rd,
  especially at grazing"), but `Scatter()`'s kray is a function of
  BOTH `cos_i` and `cos_o` (`specFactor = fresnel/max(cos_i,cos_o)`),
  so the mirror-direction proxy (`cos_o=cos_i` there) is still an
  approximation Pdf() need not make, since it already has the real
  query `wo` in hand. `SPFPdfConsistencyTest.cpp`'s own row still
  documents "large mismatch (16k-43k)" despite this existing mitigation.
  Size S-M (both `Pdf`/`PdfNM`, RGB+NM `specFactor`/`diffFactor`
  reconstruction at the query direction rather than the mirror
  direction), physics-bias, user-visible, LOWER PRIORITY than DL-98
  since a deliberate partial mitigation is already in place. Recipe:
  same per-lobe ground-truth harness, applied to
  `AshikminShirleyAnisotropicPhongSPF`.
