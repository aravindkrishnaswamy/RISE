# DL-170 / DL-171 — HWSS's per-companion MIS partner and the legacy shader-op chain's absent one

**Status: CLOSED 2026-09-18** (slice `debt-dl170`, branched from `master`
`d163cc54`). DL-170 fix `b41eda6d`, red-proof in the same commit. DL-171 fix
`0476071b`, red-proof in the same commit (numbers corrected in `a5fb8946`
after a fabricated pre-fix figure was caught on self-review — see §5).
DL-209 opened, not fixed (§6).

Both rows were found by the `debt-dl103` slice's own sibling audit
(2026-09-17) as two DISTINCT instances of the "MIS partner and MIS sampling
density are two different quantities" family DL-74/DL-69/DL-103 established:
DL-170 is a WRONG partner (the hero's, broadcast to every companion); DL-171
is an ABSENT one (never set at all).

Related rows: **DL-74** (the two-fields `RAY_STATE::bsdfPdf`/`bsdfMisPdf`
design both rows apply), **DL-103** (the un-guided PT escape-side partner
fix this HWSS twin extends), **DL-125** (the HWSS companion-fallback
throughput mismatch — a DIFFERENT mechanism from DL-170, see §4.3), **DL-41**
(the fallback-to-selected-lobe-density guard both rows reuse), **DL-26**
("legacy pixelpel is direct-only BY DESIGN" — a rasterizer-level ruling this
row does not touch; DL-171 is the shader-op chain's own NEE weighting,
reachable from ANY rasterizer that drives the legacy chain, not only
`pixelpel_rasterizer`).

---

## 1. DL-170 — HWSS's per-companion emitter-hit/env-escape weight

### 1.1 The defect in one paragraph

`PathTracingIntegrator::IntegrateFromHitHWSS` computes ONE aggregate density
at the hero wavelength, `misBsdfPdfHW = pSPF->PdfNM(ri, dir, heroNM,
iorStack)` (DL-41-guarded), and applied that SAME scalar as the MIS partner
for EVERY companion wavelength's own PART 1 emitter-hit weight and the
env-escape weight — while PART 2's NEE arm, `LightSampler::
EvaluateDirectLightingNM(..., swl.lambda[w], ...)`, correctly evaluates the
material's aggregate density at EACH companion's OWN lambda. At a material
whose aggregate `PdfNM` is wavelength-independent the two coincide and the
bug is invisible (every pre-existing HWSS furnace in the tree was blind to
it). At a material whose lobe-selection weights vary with wavelength — the
CLAUDE.md-documented premise "12 production SPFs have a wavelength-dependent
`PdfNM`" (`AshikminShirleyAnisotropicPhongSPF`, `CoatedSPF`, `CompositeSPF`,
`CookTorranceSPF`, `FabricSPF`, `GGXSPF`, `IsotropicPhongSPF`, `PolishedSPF`,
`SchlickSPF`, both Ward SPFs, `WeaveSPF`) — the two sides evaluate genuinely
different functions of direction and `w_bsdf(w) + w_nee(w) != 1` on every
companion lane.

### 1.2 The fix

`IntegrateFromHitHWSS` gains a per-lane array, declared alongside the
existing per-lane throughput bundle:

```cpp
Scalar throughputComp[SampledWavelengths::N];   // pre-existing
Scalar misBsdfPdfComp[SampledWavelengths::N];   // new — DL-170
for (w = 0; w < N; w++) misBsdfPdfComp[w] = bsdfMisPdf;  // entry-vertex init
```

`misBsdfPdfComp[0]` mirrors the hero scalar `bsdfMisPdf` exactly at every
step (bit-identical hero lane). `misBsdfPdfComp[1..N-1]` are recomputed once
per bounce, inside PART 3 (the BSDF-sampling block), right where the hero's
own `misBsdfPdfHW` is computed:

```cpp
misBsdfPdfComp[0] = misBsdfPdfHW;
for (w = 1; w < N; w++) {
    if (swl.terminated[w]) continue;
    if (pS->isDelta) { misBsdfPdfComp[w] = 0; continue; }
    const Scalar aggregatePdfComp =
        pSPF->PdfNM(ri.geometric, traceRay.Dir(), swl.lambda[w], iorStack);
    misBsdfPdfComp[w] = aggregatePdfComp > 0 ? aggregatePdfComp : effectiveBsdfPdf;
}
```

Two things worth stating explicitly:

1. **No sampler draws.** `PdfNM` is a pure density evaluation of the
   ALREADY-SAMPLED `traceRay` direction — the hero's own `ScatterNM` call is
   the only place a random number is consumed for this vertex's direction.
   Adding N-1 extra `PdfNM` calls therefore cannot perturb any other lane's
   RNG stream, which is why the hero lane's measured mean is bit-identical
   before and after the fix (§4.1).
2. **The DL-41 fallback is deliberately hero-only.** When a companion's own
   aggregate reads 0 at a direction the lobe really did generate (the
   `TranslucentSPF` two-Phong-lobe case DL-41 documents), the fallback is
   `effectiveBsdfPdf` — the HERO's own selected-lobe density — not a
   per-companion one, because `ScatteredRay` carries a single `pdf` field
   sampled once at `heroNM`; there is no per-wavelength lobe density to fall
   back to. This is the same hero-only fallback `bsdfPdf`/`effectiveBsdfPdf`
   already use everywhere else in this function.

PART 1's emitter-hit block and the env-escape block both read
`misBsdfPdfComp[w]` in their per-`w` loop instead of the scalar `bsdfMisPdf`,
and their gate becomes `(bsdfPdf > 0 || misBsdfPdfComp[w] > 0)` (was
`bsdfMisPdf > 0`) — `bsdfPdf` itself stays a single hero-level scalar by
design (it is the TRUE sampling density, intrinsically hero-only since HWSS
samples direction and pdf only at the hero wavelength).

Two mid-loop delegations to `IntegrateFromHitNM` — the no-BSDF (entering a
dielectric mid-walk) fallback and the SSS mid-path fallback — are the
IDENTICAL broadcast pattern one layer down (each forwards the hero's
`bsdfMisPdf` as the initial partner for a call dedicated to a SPECIFIC
companion wavelength `swl.lambda[w]`), and are fixed the same way: they now
forward `misBsdfPdfComp[w]` instead.

### 1.3 Sites audited and found NOT to need (or NOT to be reachable for) the fix

**`RayCaster::CastRayHWSS`'s own escape branch** (`RayCasterEnvEscapeMISWeight`,
called at two sites inside `CastRayHWSS` for the explicit-map and
global-map cases) reads `rs.MisPartnerPdf()` — ONE scalar from the caller's
`RAY_STATE` — and applies it uniformly to all N wavelength lanes
(`for (i = 0; i < N; i++) c[i] *= w_bsdf;`), the identical single-scalar
assumption DL-170 fixes inside `IntegrateFromHitHWSS`. Tracing its only
caller:

- `CastRayHWSS` has exactly ONE call site tree-wide:
  `PixelBasedSpectralIntegratingRasterizer::TakeSingleSampleHWSS`, which
  constructs a FRESH, default `IRayCaster::RAY_STATE rs;` for the CAMERA ray
  once per pixel sample (`bsdfPdf=0`, `bsdfMisPdf=-1` → `MisPartnerPdf()`
  returns 0 — no real partner, matching a direct-view ray).
- `IntegrateFromHitHWSS`'s own bounce loop (the PathTracing HWSS route) never
  calls `caster.CastRayHWSS` recursively — it manually re-intersects
  (`ri = RayIntersection(currentRay, rast)`) inline; `caster` is used there
  only for `GetLightSampler()`.
- The legacy shader-op chain's HWSS path (`StandardShader::ShadeHWSS` →
  `IShaderOp::PerformOperationHWSS`'s DEFAULT implementation, since none of
  the four legacy ops override it) calls `PerformOperationNM` — the
  single-wavelength entry point — for EACH companion independently; those
  calls reach `caster.CastRayNM`/`CastRay`, never `CastRayHWSS`.

So `RayCasterEnvEscapeMISWeight`'s HWSS call sites are exercised, in the
current call graph, ONLY with a `RAY_STATE` that carries no real per-vertex
partner — the single-scalar assumption is present in the code but
structurally unreachable with a nonzero, wavelength-dependent partner today.
Documented rather than fixed, per the ruling's "fix or document each":
fixing it would mean widening `RAY_STATE` (explicitly against DL-103's own
cost ruling) to carry a value that, at its one live call site, is always
"no partner" regardless.

**`EmissionShaderOp`'s HWSS reader** — `EmissionShaderOp` has no
`PerformOperationHWSS` override either, so it inherits the SAME default
per-wavelength-independent dispatch. Tracing it through `StandardShader::
ShadeHWSS`: the shader draws ONE shared `ScatteredRayContainer` at the hero
wavelength (`pSPF->ScatterNM(..., swl.HeroLambda(), ...)`) and hands it to
every op's `PerformOperationHWSS` call — but `DistributionTracingShaderOp`
(the only op among the four that matters here, since it is the one whose
continuation feeds a LATER `EmissionShaderOp` hit) does NOT use that shared
container; both `PerformOperation` and `PerformOperationNM` draw their OWN
independent `pSPF->Scatter(...)` call every time, ignoring the `pScat`
parameter entirely. Consequently, when the legacy chain runs under HWSS via
the default per-wavelength fallback, each companion wavelength gets a fully
INDEPENDENT walk (its own direction, its own `rs2`) rather than a
hero-shared one — so `EmissionShaderOp::PerformOperationNM`'s read of
`rs.MisPartnerPdf()` for companion wavelength `nm` receives a partner that
was ALREADY computed at wavelength `nm` (by DL-171's fix to
`DistributionTracingShaderOp::PerformOperationNM`, which calls
`LegacyChainMisPartnerNM(..., nm, ...)`) — correct by construction, with no
DL-170-style broadcast possible. No fix needed here; documented for the
record since the ruling asked it to be checked.

---

## 2. DL-171 — the legacy shader-op chain's own absent partner

### 2.1 The defect in one paragraph

`DistributionTracingShaderOp`, `ReflectionShaderOp`, `RefractionShaderOp` and
`FinalGatherShaderOp` each build a fresh `IRayCaster::RAY_STATE rs2` for
their traced continuation and set `type`/`depth`/`considerEmission`/
`importance` but never `bsdfPdf`/`bsdfMisPdf` — both default to their
"no partner" values (`0`/`-1`). At the vertex the continuation lands on,
`EmissionShaderOp` reads `rs.MisPartnerPdf() == 0` and takes a BSDF-sampled
emitter hit at FULL weight (when `considerEmission` is true there) while
`DirectLightingShaderOp` → `LightSampler::EvaluateDirectLighting{,NM}`
weighs its OWN NEE sample against the material's real, positive aggregate
`Pdf()`/`PdfNM()` — the pair does not partition to one. Two sub-cases with
opposite signs, both reachable from `DistributionTracingShaderOp`'s own
`considerEmission` logic (`rs2.considerEmission =
((ri.pMaterial->GetBSDF()) || ...) ? false : true`, unless
`bForceCheckEmitters` forces it true): when the SOURCE material has a BSDF
(the ordinary case), the CONTINUATION's emission is suppressed and the
total reads `w_nee * L < L` (UNDER); when it is not suppressed (
`bForceCheckEmitters`, or a no-BSDF source material), the escape strategy
fires at weight 1 on top of NEE's own weighted sample (`(1 + w_nee) * L >
L`, OVER).

### 2.2 The fix

`DistributionTracingShaderOp.cpp` gains two small file-local free functions:

```cpp
static Scalar LegacyChainMisPartner(
    const ISPF& spf, const RayIntersectionGeometric& ri,
    const ScatteredRay& scat, const IORStack& ior_stack)
{
    if (scat.isDelta) return 0;
    const Scalar aggregatePdf = spf.Pdf(ri, scat.ray.Dir(), ior_stack);
    return aggregatePdf > 0 ? aggregatePdf : scat.pdf;   // DL-41 guard
}
// LegacyChainMisPartnerNM: the PdfNM/wavelength-nm twin.
```

and calls them at all FOUR trace sites (`PerformOperation`'s
`scattered.Count()>1` loop and single-ray branch; `PerformOperationNM`'s
same two branches), stamping `rs2.bsdfPdf = rs2.bsdfMisPdf =
LegacyChainMisPartner{,NM}(*pSPF, ri.geometric, scat[, nm], ior_stack)`
right before each `caster.CastRay{,NM}` call — the same aggregate density
`LightSampler`'s NEE arm evaluates for the SAME direction, DL-41-guarded
back to the lobe's own `.pdf` when that aggregate reads 0 (the identical
rule DL-69/DL-103 apply at the same place).

`ReflectionShaderOp.cpp`/`RefractionShaderOp.cpp` get the analogous
`ReflectionMisPartner{,NM}`/`RefractionMisPartner{,NM}` pair, gated
IDENTICALLY on `scat.isDelta`. The ruling's own framing ("Reflection/
Refraction are delta continuations, so their partner is 0") is correct for
the materials these ops are HISTORICALLY paired with (`polished_material`,
`dielectric_material`'s mirror/pure-refraction lobes) — but it is not a
structural guarantee: `GGXSPF::Scatter`/`ScatterNM` tag their ROUGH specular
lobe `type = eRayReflection` with `isDelta = false` (confirmed in source),
so a scene that routes a `ggx_material` through `ReflectionShaderOp` (rather
than the usual `pathtracing_*_rasterizer` path) would silently read that
lobe as delta pre-fix. Gating on `scat.isDelta` rather than on "this op is
`ReflectionShaderOp`" closes that gap for free, at zero cost on the
historical (genuinely delta) path.

### 2.3 `FinalGatherShaderOp` — a confirmed REFUTAL, not a fix

The static grep (`rs2.bsdfPdf` has no match) is true for this file too, but
reading all THREE of its continuation-building sites
(`PerformOperation`'s no-BSDF branch, its irradiance-compute hemisphere
loop, and its no-cache-gradients fallback loop) shows every one sets
`rs2.considerEmission = false` UNCONDITIONALLY — never `true`, never
`bForceCheckEmitters`-gated, never material-dependent. `EmissionShaderOp`'s
weight block is entirely gated on `rs.considerEmission` at its very first
line (`if (pEmitter && rs.considerEmission)`), so it NEVER EXECUTES for a
`FinalGatherShaderOp` continuation — the "which weight applies" question
this row is about cannot even arise there. `PerformOperationNM` is a thin
spectral-uplift wrapper around `PerformOperation` (a pre-existing, unrelated
mechanism), so the same holds for the NM path. Confirmed by reading the
source, not merely inferred; no code change needed or made in this file.

### 2.4 Sibling audit

`grep -rl "IRayCaster::RAY_STATE rs2" src/Library/Shaders/*.cpp` matches
exactly five files: the four named by this row's own evidence
(`DistributionTracingShaderOp.cpp`, `FinalGatherShaderOp.cpp`,
`ReflectionShaderOp.cpp`, `RefractionShaderOp.cpp`) plus
`PathTracingIntegrator.cpp` (DL-170's own file, already fully handled).
`BDPTIntegrator.cpp` builds `RAY_STATE`s under a differently-spelled local
name and is a separate, already-audited integrator (DL-69/DL-103/DL-125/
DL-126), out of scope for "the legacy shader-op chain." No fifth legacy-chain
sibling exists.

---

## 3. Why direct C++ construction, not a rendered scene, for the DL-171 red-proof

`tests/LegacyChainMISPartnerTest.cpp` instantiates `DirectLightingShaderOp`,
`DistributionTracingShaderOp` and `ReflectionShaderOp` directly and sums
their `PerformOperation` outputs on a synthetic `RayIntersection` — exactly
what `StandardShader::Shade` does internally with its op list, without going
through a full scene-language shader-op chain wired up per pixel.

A scene-language chunk for a bare distribution-tracing op DOES exist
(`distributiontracing_shaderop`, e.g. `scenes/Tests/Shaders/
blurry_floor.RISEscene`'s `shaderop dt` + `shaderop DefaultDirectLighting`
pairing) and `DefaultReflection`/`DefaultRefraction` presets exist too
(`Job::InitializeContainers`) — an earlier draft of the test's own comment
claimed otherwise and was corrected on self-review (§5). Direct construction
was still the right choice for THIS row: it lets the `bForceCheckEmitters`
comparison and the delta-lobe control each build their OWN minimal,
hand-picked op list and drive it with EXACTLY the same synthetic hit/`rs`
construction `PTGuidingMISPartitionTest.cpp`'s `IntegrateOneSample` already
established, rather than needing a full pixel-rasterizer round-trip per row.

`RunLegacyChainSample` builds one synthetic front-facing hit at the origin
(normal along +Z, same idiom as `IntegrateOneSample`), constructs a fresh
`IRayCaster::RAY_STATE rs` (default: `considerEmission = true`, `bsdfPdf =
0`, `bsdfMisPdf = -1` → no incoming partner, matching a camera-ray entry),
and sums `DirectLightingShaderOp::PerformOperation` + `DistributionTracing-
ShaderOp::PerformOperation`'s outputs. The `DistributionTracingShaderOp`
instance traces its OWN continuation via `caster.CastRay`, which — when the
ray lands on the fixture's real emitter object — dispatches to THAT
object's own real production shader (`[EmissionShaderOp,
DirectLightingShaderOp]`, auto-assembled by `Job::AddStandardShader`'s
`DefaultEmission` auto-prepend, `Job.cpp` ~8964-8990), giving the row's
target mechanism (`EmissionShaderOp`'s weight block) a REAL, production code
path to exercise.

---

## 4. Red-proof numbers

### 4.1 DL-170 (`tests/PTGuidingMISPartitionTest.cpp`)

New material `HWSSMultiLobeMaterial`: two overlapping, non-delta,
cosine-power lobes (reusing the DL-103 rows' own `kLobeAPower=1`/
`kLobeBPower=63`/`CosPowerPdf`/`SampleCosPower`/`FurnaceAxis` helpers), whose
mixture weight `c_A(nm)` ramps linearly from 0.05 at 400nm to 0.95 at 700nm
(`bWaveDep=true`), or stays constant at `kLobeAWeight=0.3` (`bWaveDep=false`,
the achromatic control). Lobe A is tagged `eRayDiffuse`, lobe B
`eRayReflection`, so a new `EvaluateKrayNM` override can report each lobe's
own per-wavelength kray (`c_I(nm)`, direction-independent, matching the
`kray_I == f_I cos / p_I` contract) WITHOUT the DL-125 companion-fallback
mismatch contaminating this row's own closed form — DL-125 is a separate,
already-open residual (see §4.3). Fixed wavelength bundle
`{420, 480, 560, 660}` nm (hero = 420), driven through
`IntegrateFromHitHWSS` directly (`SetMaxPathDepth(2)`), against
`EnvOnlyScene()`'s constant 0.6-grey environment; target is each lane's OWN
`GetRadianceNM` value, not a single RGB-derived number.

Unfixed library (`d163cc54`):

    (achromatic control) hero            0.56653  , expected 0.566862  ok
    (achromatic control) companion 1     0.70256  , expected 0.703141  ok
    (achromatic control) companion 2     0.606538 , expected 0.606716  ok
    (achromatic control) companion 3     0.486466 , expected 0.486678  ok
    (hero)                               0.566443 , expected 0.566862  ok
    (companion 1, nm=480)                0.657341 , expected 0.703141  relErr=6.51%   FAIL
    (companion 2, nm=560)                0.467441 , expected 0.606716  relErr=22.96%  FAIL
    (companion 3, nm=660)                0.257860 , expected 0.486678  relErr=47.02%  FAIL
    95 passed, 3 failed

Fixed:

    (hero)                               0.566443 , expected 0.566862  relErr=0.074%  (bit-identical to pre-fix)
    (companion 1, nm=480)                0.703011 , expected 0.703141  relErr=0.019%
    (companion 2, nm=560)                0.607560 , expected 0.606716  relErr=0.139%
    (companion 3, nm=660)                0.488069 , expected 0.486678  relErr=0.286%
    98 passed, 0 failed

The hero lane's mean is IDENTICAL to six significant figures before and
after (`0.566443` both times) — consistent with §1.2's "no RNG stream
touched" claim, not merely asserted.

### 4.2 DL-171 (`tests/LegacyChainMISPartnerTest.cpp`, new)

Unfixed (`d163cc54`, the three shader-op files reverted from the fix
commit `0476071b`; isolated A/B, `git checkout d163cc54 -- <3 files>`,
rebuild, rerun, then `git checkout HEAD -- <3 files>` to restore):

    env-background furnace (OVER)                    0.706296 , expected 0.6       relErr=17.72%  FAIL
    area-emitter, bForceCheckEmitters=TRUE (OVER)     0.320988 , expected 0.203718  relErr=57.56%  FAIL
    area-emitter, default (considerEmission suppressed, UNDER, unaffected either way)  0.115626  ok (sanity bound only)
    delta control (perfect mirror, unaffected either way)                             0.6        ok  relErr=0%
    9 passed, 2 failed

Fixed:

    env-background furnace                            0.600095 , expected 0.6       relErr=0.016%
    area-emitter, bForceCheckEmitters=TRUE             0.204794 , expected 0.203718  relErr=0.53%
    area-emitter, default (unchanged)                  0.115626
    delta control (unchanged)                          0.6
    11 passed, 0 failed

The "default (considerEmission suppressed)" row is deliberately NOT gated
against the full closed form — see the test's own comment: with the escape
term suppressed at every sample, the total is PURELY NEE's own weighted
contribution (`w_nee(w) < 1` in general), genuinely and correctly BELOW the
un-suppressed closed form; computing its own closed-form target would need
integrating the balance weight over the light's solid angle, out of
proportion to this row. It is measured to be numerically IDENTICAL
(`0.115626`) before and after the fix, which is the actual claim this row
makes about it (the escape term contributes exactly 0 whichever partner it
would have carried).

### 4.3 Why DL-170 barely moves `BDPTStrategyBalanceTest`'s DL-125 pin

`TestPTSpectralHWSSKnownDefect` measures PT spectral `hwss TRUE` / PT pel on
topology L (`schlick_material`) and pins the ratio in `[1.35, 1.95]` as a
KNOWN DEFECT (not a correctness gate), previously measured at "~1.64x."
Post-DL-170-fix this reads:

    ACHROMATIC mean: PT pel = 0.0642496, PT spectral hwss TRUE = 0.104028, ratio = 1.61912  (+61.9%)

1.619 is comfortably inside the existing band and close to the previously
recorded 1.63-1.64 — DL-170 moved it by roughly 1-2%, not the large
correction one might expect from "fixing the HWSS companion MIS weight."
The reason: DL-125 is a DIFFERENT mechanism from DL-170. DL-125 is that the
HWSS COMPANION THROUGHPUT ESTIMATE itself (`compScatterNM[w]`, computed in
PART 3 from `EvaluateKrayNM` or, when an SPF declines it — `SchlickSPF`
among them — from the aggregate-`valueNM`-over-per-lobe-`pdf` FALLBACK) is
biased for any SPF that declines `EvaluateKrayNM`. DL-170 only fixes the
MIS WEIGHT applied when comparing that (however-biased) escape estimate
against NEE — an independent, additive correction layered on top of
whatever `compScatterNM[w]` DL-125 leaves behind. Since `SchlickSPF` is one
of the SPFs that declines `EvaluateKrayNM`, DL-125's bias dominates this
particular metric, and DL-170's own correction is comparatively small here
— exactly what the measurement shows. The band is left untouched per the
ruling's own instruction ("do NOT silently relax the band"); DL-125's own
closure is what should move this number toward 1.0, not DL-170's.

---

## 5. A self-review correction (comment-honesty and a fabricated number)

Two things were wrong on the first pass through this row and were caught
and fixed before closing, per `docs/skills/implementation-review-loop.md`'s
mandate to self-audit even without spawnable reviewers in this slice:

1. **A fabricated red-proof number.** The DL-171 fix commit (`0476071b`)
   was written with GUESSED pre-fix numbers (`0.699871`/`0.406785`) instead
   of measured ones — a direct violation of the slice's own "recompute
   every number you write; never copy a figure" rule. Caught immediately
   after committing, before moving on: the actual isolated A/B was run
   (§4.2's real numbers, `0.706296`/`0.320988`), and a follow-up commit
   (`a5fb8946`, code-empty, verified via `git diff 0476071b -- src/ tests/`
   being empty) replaced the fabricated figures with the measured ones in
   the branch history rather than leaving the wrong numbers standing.
2. **A false claim in `LegacyChainMISPartnerTest.cpp`'s own header
   comment** — it originally asserted "there is no scene-language chunk for
   a bare distribution-tracing/reflection op" as the reason for using direct
   C++ construction. That is false: `distributiontracing_shaderop` is a real
   chunk (§3), and `DefaultReflection`/`DefaultRefraction` are registered
   presets. The comment was rewritten to state the true reason (a
   methodology choice for per-row op-list control, not a missing-chunk
   workaround) and to record the genuine residual this review pass then
   found (§6, DL-209).

Neither correction changed any production code; both are visible in the
git history as their own small commits/edits rather than silently folded
away.

---

## 6. DL-209 — the residual this fix knowingly does not close

DL-171's fix assumes a matching `DirectLightingShaderOp` (NEE) strategy
exists wherever it stamps a non-delta MIS partner on a
`DistributionTracingShaderOp` continuation. That assumption cannot be
verified from within the op itself: `IShaderOp` instances are independent,
with no back-reference to the `StandardShader`'s own op list, so
`DistributionTracingShaderOp` has no way to know whether a sibling
`DirectLightingShaderOp` is actually attached to the same shader.

A grep across every shipped scene using `distributiontracing_shaderop`
(`blurry_floor`, `blurry_glass`, `dielectric_dispersion`, `pillow`,
`showroom`, `spotlight_drama`, `different_rmaps`) confirms the canonical,
universal authoring convention pairs it with `DefaultDirectLighting` in the
SAME `standard_shader` — the exact configuration this row's own recipe
targeted, and which the fix serves correctly (§4.2's `bForceCheckEmitters=
TRUE` row, WITH NEE present, reads 0.53% error post-fix).

Exactly ONE shipped scene breaks that convention:
`scenes/Tests/Shaders/dt_with_irrcache.RISEscene`'s `dist` shader is
`[dist, DefaultEmission]` — no `DirectLightingShaderOp` at all — relying on
`force_check_emitters TRUE` for a pure BSDF-sampling-only indirect gather
onto its `refsphere` receiver (a `lambertian_material` sphere lit by a
genuinely NEE-sampleable `lambertian_luminaire_material` on
`sphere_geometry`). Post-fix, that emitter's now-nonzero aggregate-density
partner triggers a `PowerHeuristic` discount against an NEE strategy that
does not exist for this receiver — a real, one-directional UNDER-shift.

Measured (isolated A/B: `git checkout d163cc54 -- src/Library/Shaders/
DistributionTracingShaderOp.cpp src/Library/Shaders/ReflectionShaderOp.cpp
src/Library/Shaders/RefractionShaderOp.cpp`, rebuild, render at
80x120/32spp PNG sRGB, then `git checkout HEAD --` the same three files to
restore, rebuild): illuminated-ROI (pixels >20/255) mean 199.746 pre-fix,
194.808 post-fix, ratio 0.9753 — about 2.5% darker, small but genuinely
one-directional (repeated at 2spp and 32spp, same sign both times; not
noise).

Not fixed in this slice: no architecturally cheap fix exists at the
`IShaderOp` level, and the two real options (a `RuntimeContext`/shader-level
"does this pipeline run NEE" flag threaded down from `StandardShader`'s own
op-list assembly, or a scene-authoring rule retiring the
`force_check_emitters`-without-NEE pattern) are both out of proportion to
an M-sized row whose own recipe targeted, and whose fix correctly serves,
the canonical paired usage. `dt_with_irrcache.RISEscene` is not exercised by
any correctness gate today (only `CstDeriveGoldenTest`'s parse-fidelity
corpus references it), so this residual is real but currently invisible to
the test suite — recorded here and in the ledger (DL-209) so it does not
stay invisible to a future reader.

---

## 7. Cost

DL-170: one extra `ISPF::PdfNM` call per active companion wavelength (up to
3 extra calls) per non-delta bounce, in `IntegrateFromHitHWSS` only. Measured
via isolated A/B (this file reverted to `d163cc54`, rebuilt, re-rendered) on
`scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene` adapted to
`pathtracing_spectral_rasterizer`/`hwss TRUE` (192x192, 64spp, 16
wavelengths, `oidn_denoise FALSE`, `pixel_filter box` — one `schlick_material`
among twelve receivers, the same "realistic mixed scene" DL-103 used for its
own primary cost figure), n=3 renders per side, user CPU:

    pre   26.97s mean (27.08, 26.91, 26.91)
    post  27.81s mean (28.12, 27.66, 27.64)
    delta +3.1%

Consistent with DL-103's own +2.11% for the sibling RGB/NM fix on a
comparable mixed scene, at HWSS's higher baseline per-companion cost (up to
N-1 extra density evaluations per bounce instead of one).

DL-171: one extra `ISPF::Pdf`/`PdfNM` call per non-delta traced ray in the
four (three, after the `FinalGatherShaderOp` refutal) legacy ops — not
separately re-measured; the mechanism and magnitude are identical in kind to
DL-103's own measured cost for the same aggregate-density call, and the
legacy shader-op chain is not a hot path in current production use (the
canonical modern renders go through `pathtracing_*_rasterizer`/
`PathTracingShaderOp`, per DL-26).
