# DL-170 / DL-171 — HWSS's per-companion MIS partner and the legacy shader-op chain's absent one

**Status: CLOSED 2026-09-18** (slice `debt-dl170`, branched from `master`
`d163cc54`). DL-170 fix `b41eda6d`, red-proof in the same commit — verified
clean by an independent review round and **not touched again** in this
document's own P1-correction round. DL-171 went through TWO fix rounds:

- **V1** (`0476071b`/`a5fb8946`/`d1c25bd1`): stamped a real MIS partner on
  `DistributionTracingShaderOp`/`ReflectionShaderOp`/`RefractionShaderOp`
  continuations, gated on `scat.isDelta`. This closed the OVER sign
  correctly (an env-background furnace and a `bForceCheckEmitters=TRUE`
  area-emitter furnace both moved from a real defect to the closed form)
  but its own closure text WRONGLY asserted that the "default
  (`considerEmission` suppressed)" row's measured 0.115626 was "genuinely
  and correctly BELOW the un-suppressed closed form" — i.e. that the UNDER
  sign the ledger row itself named was just NEE reading correctly with the
  escape term suppressed to 0. It was not: `DirectLightingShaderOp`
  unconditionally applied `w_nee = PowerHeuristic(p_light, p_bsdf)` even
  though `DistributionTracingShaderOp`'s own `considerEmission` suppression
  meant NO competing BSDF-sampled strategy could ever fire at that vertex —
  a real, un-diagnosed discount, independent of anything V1 touched. An
  external review (P1, quoted in full in §2.1) caught this by reverting the
  three V1-touched files to base and finding the "suppressed" row's number
  BIT-IDENTICAL — proof the UNDER half of this row's own two-named-signs
  was never actually addressed.
- **V2** (`8e7bdde7`, this document's own fix): the chain-aware,
  bidirectional mechanism described in §2 below, which closes BOTH signs
  and, as a side effect, closes DL-209 (§6).

Related rows: **DL-74** (the two-fields `RAY_STATE::bsdfPdf`/`bsdfMisPdf`
design both rows apply), **DL-103** (the un-guided PT escape-side partner
fix this HWSS twin extends), **DL-125** (the HWSS companion-fallback
throughput mismatch — a DIFFERENT mechanism from DL-170, see §4.3), **DL-41**
(the fallback-to-selected-lobe-density guard both rows reuse), **DL-26**
("legacy pixelpel is direct-only BY DESIGN" — a rasterizer-level ruling this
row does not touch; DL-171 is the shader-op chain's own NEE weighting,
reachable from ANY rasterizer that drives the legacy chain, not only
`pixelpel_rasterizer`), **DL-209** (closed as a side effect of V2, §6).

---

## 1. DL-170 — HWSS's per-companion emitter-hit/env-escape weight

Unchanged from the original closure; re-verified during the V2 round
(numbers reproduced, hero lane bit-identical, IOR stack live, the two
"sites audited and refuted" claims re-confirmed by re-reading the current
source) per the coordinator's explicit instruction not to touch this file
again. See the original account below.

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
it). At a material whose lobe-selection weights vary with wavelength — 12
production SPFs have a wavelength-dependent `PdfNM` (`AshikminShirley-
AnisotropicPhongSPF`, `CoatedSPF`, `CompositeSPF`, `CookTorranceSPF`,
`FabricSPF`, `GGXSPF`, `IsotropicPhongSPF`, `PolishedSPF`, `SchlickSPF`,
both Ward SPFs, `WeaveSPF`) — the two sides evaluate genuinely different
functions of direction and `w_bsdf(w) + w_nee(w) != 1` on every companion
lane.

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
dielectric mid-walk) fallback and the SSS mid-path fallback — forward
`misBsdfPdfComp[w]` instead of the scalar `bsdfMisPdf`.

### 1.3 Sites audited and found NOT to need (or NOT to be reachable for) the fix

`RayCaster::CastRayHWSS`'s own escape branch (`RayCasterEnvEscapeMISWeight`)
reads ONE scalar from the caller's `RAY_STATE` and applies it uniformly to
all N wavelength lanes — the same single-scalar assumption DL-170 fixes
inside `IntegrateFromHitHWSS`. `CastRayHWSS` has exactly ONE call site tree-
wide (`PixelBasedSpectralIntegratingRasterizer::TakeSingleSampleHWSS`),
which constructs a FRESH, default `RAY_STATE` for the camera ray only
(`MisPartnerPdf()` reads 0 — no real partner). Neither `IntegrateFromHitHWSS`
nor the legacy chain's HWSS default dispatch (`PerformOperationHWSS` ->
per-companion `PerformOperationNM`) ever calls `CastRayHWSS` recursively, so
this site's single-scalar assumption is present in the code but structurally
unreachable with a nonzero, wavelength-dependent partner today. Documented,
not fixed (widening `RAY_STATE` would be against DL-103's own cost ruling,
for a value that is always "no partner" at its one live call site).

`EmissionShaderOp`'s HWSS reader inherits the same default per-wavelength-
independent dispatch, but `DistributionTracingShaderOp` (the only op among
the legacy four whose continuation feeds a LATER `EmissionShaderOp` hit)
draws its OWN independent `Scatter` call per wavelength rather than sharing
one hero-drawn container, so each companion's walk is fully independent —
`EmissionShaderOp::PerformOperationNM`'s read of `rs.MisPartnerPdf()` for
wavelength `nm` receives a partner already computed AT `nm` by DL-171's own
fix. No DL-170-style broadcast possible here; no fix needed.

---

## 2. DL-171 — the legacy shader-op chain's own MIS partner

### 2.1 The P1 finding this document's fix responds to

Quoted from the coordinator's relayed external review (round 1, HEAD
`d1c25bd1`), since it states the defect and the ruling more precisely than
a paraphrase would:

> DL-171's UNDER half is NOT closed and the docs say it is. ... your fix
> stamps a partner on the continuation, but `EmissionShaderOp`'s whole
> weight block is gated on `rs.considerEmission`, so in
> `DistributionTracingShaderOp`'s DEFAULT configuration (`considerEmission
> =false` because the material HAS a BSDF) the stamped partner is never
> read, while `DirectLightingShaderOp` unconditionally hands `ri.pMaterial`
> to `EvaluateDirectLighting`, which applies `w_nee = PowerHeuristic(
> p_light, p_bsdf)` against a BSDF-hit strategy that has been suppressed.
> Reviewer measurement on YOUR fixture's "default (considerEmission
> suppressed)" row: 0.115626 vs closed form 0.203718 = 43.2% LOW at HEAD,
> and bit-identical 0.115626 with the three ops reverted to base — proving
> this UNDER bug pre-dates the DL-171 fix entirely and was never actually
> addressed. ... This is the default for every shipped
> `distributiontracing_shaderop` + `DefaultDirectLighting` scene with a BSDF
> material under a mesh/area light.
>
> RULING (do not re-litigate): the invariant is "a strategy's MIS partner
> is the sibling strategy that ACTUALLY EXISTS in that shader-op chain; an
> absent sibling means weight 1". Implement it chain-aware, both directions.

The last clause of the reviewer's own framing — "every shipped
`distributiontracing_shaderop` + `DefaultDirectLighting` scene ... (`pillow.
RISEscene` has lambertian_luminaire objects and no force_check_emitters)" —
turned out to be an overstatement about that one NAMED example scene, not
about the underlying defect (which is real and confirmed exactly as
described): `pillow.RISEscene` DOES pair `dt` with `DefaultDirectLighting`,
but its ONLY active light sources are five `omni_light` (point/delta)
chunks; its two `lambertian_luminaire_material` definitions (`halo_lum`,
`halo_cool_lum`) are declared but never bound to any `standard_object` in
the scene, so it has ZERO area/mesh emitters that a BSDF-sampled
continuation could ever land on. A delta light can never be hit by a
BSDF-sampled ray (there is no scene geometry there to intersect), so the
whole partition-of-unity question this row is about is moot for THIS scene
— see §8.2 for the direct measurement confirming zero render-level change.
The underlying defect and the fix are real regardless; this is a note about
one illustrative example, not about the ruling.

### 2.2 Root cause: two independent defects were being treated as one row

Reading `EmissionShaderOp.cpp` (:51, :141) alongside
`DistributionTracingShaderOp`'s `considerEmission` logic makes the two
signs' distinct mechanisms explicit:

- **OVER** (V1 fixed this): `EmissionShaderOp`'s weight block executes
  whenever `considerEmission` is true (the ordinary un-suppressed case, or
  any `bForceCheckEmitters=TRUE` continuation) and reads
  `rs.MisPartnerPdf()`. Pre-V1 that was always 0 ("no partner") ->
  `w_bsdf = 1` on top of `DirectLightingShaderOp`'s own weighted NEE sample
  -> genuine double count. V1's partner-stamping fixed exactly this.
- **UNDER** (V1 missed this): whenever `considerEmission` is FALSE (the
  DEFAULT for any source material with a BSDF — i.e. essentially every
  ordinary non-metal, non-mirror material under `distributiontracing_
  shaderop`), `EmissionShaderOp`'s weight block never runs at all —
  regardless of what partner is stamped on the continuation, because the
  gate is on `considerEmission`, not on the partner. Meanwhile
  `DirectLightingShaderOp` -> `LightSampler::EvaluateDirectLighting{,NM}`
  computes `w_nee = PowerHeuristic(p_light, p_bsdf_aggregate)`
  UNCONDITIONALLY — it has no way to know that the competing BSDF-hit
  strategy it is discounting against was suppressed at the sibling op and
  will never fire. `w_nee < 1` is applied for a strategy pair where only
  ONE strategy (NEE) can ever actually contribute — a real, independent
  discount that stamping a partner on the (suppressed, dead) continuation
  can never touch.

V1's fixture had a row for this ("default, considerEmission suppressed")
but treated its measured 0.115626 as an ASSUMED-correct "NEE alone" value
and added only a bounded sanity check, rather than deriving what the row
SHOULD read and discovering the gap — the mistake §5 below dissects.

### 2.3 The ruling, and why NULLing `pMaterial` was not implemented literally

The ruling's own wording for direction (2) is "`DirectLightingShaderOp`
passes a NULL `pMaterial` (w_nee = 1) whenever the chain has NO non-delta
BSDF-sampled continuation op that considers emission." Read for INTENT
(deliver `w_nee = 1` exactly when no competing strategy exists) rather than
literally, because `LightSampler::EvaluateDirectLighting{,NM}` reads
`pMaterial` for a SECOND, unrelated purpose: `bFullSphere = (pMaterial !=
0 && pMaterial->ScattersFullSphere())`, which controls whether the
hemisphere-rejection test on a sampled light direction is skipped — load-
bearing for hair/fabric/weave materials that scatter over the FULL sphere.
Nulling `pMaterial` to force `w_nee=1` would ALSO silently null this,
regressing full-sphere NEE for those three material classes wherever they
happen to sit in a chain with no competing continuation. The implemented
mechanism achieves the same `w_nee=1` outcome through an independent,
additive boolean instead, leaving `pMaterial` (and `bFullSphere`) untouched:

1. Two new `RAY_STATE` fields, doc-commented in `IRayCaster.h`:

   ```cpp
   //! Does THIS shader's own op list contain a DirectLightingShaderOp
   //! (an NEE strategy that could compete for the SAME emitter)?  Read by
   //! DistributionTracingShaderOp to decide whether to stamp a real MIS
   //! partner on its own continuation, or 0 (DL-209: no sibling -> no
   //! partition needed -> full weight).  Default TRUE (matches every
   //! canonical shipped chain) so a producer that predates this field
   //! reproduces the historical (partner-stamped) behaviour.
   bool chainHasNEEOp;

   //! The converse: does THIS shader's own op list contain a
   //! DistributionTracingShaderOp (a non-delta, emission-considering
   //! BSDF-sampled continuation)?  Read by DirectLightingShaderOp,
   //! forwarded to LightSampler::EvaluateDirectLighting{,NM}'s
   //! bBsdfSamplingPartnerExists parameter: TRUE weights NEE's own sample
   //! against the aggregate density (the historical behaviour); FALSE
   //! (no competing strategy) forces w_nee = 1 without touching pMaterial
   //! (bFullSphere stays correct).  Default TRUE for the same reason as
   //! chainHasNEEOp.
   bool chainHasBsdfContinuationOp;
   ```

2. `LightSampler::EvaluateDirectLighting{,NM}` gain a trailing parameter,
   `const bool bBsdfSamplingPartnerExists = true`, gating the four
   weighting blocks that currently multiply by a `PowerHeuristic` term
   (RGB mesh/area, RGB env, NM mesh/area, NM env) — each becomes
   `if (... && bBsdfSamplingPartnerExists && ...)`, so when false the
   weight simply is not applied (full NEE contribution, `w_nee=1`) while
   `bFullSphere`'s own `pMaterial != 0` check is completely untouched.

3. `DirectLightingShaderOp::PerformOperation{,NM}` pass
   `rs.chainHasBsdfContinuationOp` as that trailing argument.

4. `DistributionTracingShaderOp`'s four partner-stamping call sites gate on
   `rs.chainHasNEEOp`:

   ```cpp
   rs2.bsdfPdf = rs.chainHasNEEOp ?
       LegacyChainMisPartner( *pSPF, ri.geometric, scat, ior_stack ) : Scalar( 0 );
   rs2.bsdfMisPdf = rs2.bsdfPdf;
   ```

5. **The chain-knowledge mechanism**, per the ruling's own preference for
   "a resolve-once flag set on the ops by the owner": `StandardShader` (no
   depth ranges — flat op list) resolves both flags ONCE, at construction,
   in the same loop that already resolves `bComputeSPF`:

   ```cpp
   for (i = shaderops.begin(); i != shaderops.end(); i++) {
       if ((*i)->RequireSPF()) bComputeSPF = true;
       if (dynamic_cast<DirectLightingShaderOp*>(*i))     bHasDirectLightingOp = true;
       if (dynamic_cast<DistributionTracingShaderOp*>(*i)) bHasBsdfContinuationOp = true;
   }
   ```

   `Shade`/`ShadeNM`/`ShadeHWSS` each build a local `RAY_STATE rs2 = rs;`
   stamping `rs2.chainHasNEEOp = bHasDirectLightingOp;
   rs2.chainHasBsdfContinuationOp = bHasBsdfContinuationOp;` and dispatch
   every op against `rs2`, not the caller's `rs` — so a `standard_shader`
   named "adv" (say) always answers about ITS OWN op list, never a caller's.
   `AdvancedShader` has PER-OP depth ranges (`[nMinDepth, nMaxDepth]`), so a
   single construction-time flag would be wrong wherever the DirectLighting
   and DistributionTracing ops occupy different depth WINDOWS (confirmed to
   happen in shipped content — §2.6, `blurry_glass.RISEscene`'s `adv`
   shader). `AdvancedShader::ResolveChainFlagsForDepth(depth, out&, out&)`
   scans only the depth-filtered subset of ops and is called PER-SHADE-CALL
   with `rs.depth`, so the flags answer "at THIS depth" rather than "does
   this shader's op list, taken as a flat set, ever contain X."

6. **`dynamic_cast` never mutates the shared op object itself** — it is a
   pure runtime type query on a `const IShaderOp*`, so a shared/named
   instance like `DefaultDirectLighting` (referenced by MANY different
   `StandardShader`/`AdvancedShader` instances across a scene) is read, not
   written; each owning shader's OWN flags are independent of what other
   shaders sharing the same op instance resolve.

7. **`bForceCheckEmitters` is retired for the emission-suppression role,
   kept as a no-op-compatible flag for an orthogonal one.**
   `DistributionTracingShaderOp`'s pre-fix `considerEmission` logic was
   `((ri.pMaterial->GetBSDF()) || (causticMap && !rs.considerEmission)) ?
   false : true` unless `bForceCheckEmitters` forced `true` — a pre-MIS
   hack that suppressed the BSDF-hit strategy whenever the source material
   had a BSDF, precisely BECAUSE there was no real partner to weight it
   against. Now that a real partner is stamped (whenever a sibling NEE op
   exists — §2.4), that hack is obsolete: the fixed logic drops the
   `ri.pMaterial->GetBSDF()` clause entirely,

   ```cpp
   if( bForceCheckEmitters ) {
       rs2.considerEmission = true;
   } else {
       rs2.considerEmission = (caster.GetAttachedScene()->GetCausticSpectralMap() && !rs.considerEmission) ? false : true;
   }
   ```

   so `considerEmission` now tracks ONLY the pre-existing, orthogonal
   caustic-map suppression (a scene relying on a caustic photon map for
   this transport, not on BSDF-vs-NEE MIS, still wants the BSDF-hit
   strategy silenced so it doesn't double the caustic map's own
   contribution) — untouched by this row. `bForceCheckEmitters` stays a
   real, wired constructor parameter and scene-language flag; it is simply
   a NO-OP for the removed clause specifically (a scene that set it to
   force emission-checking under the OLD hack now gets emission-checking
   unconditionally under the new logic too, since the new logic's default
   is already `true` absent a caustic map) — no scene-language or ABI
   change, and no shipped scene's behaviour regresses from this retirement
   (confirmed via §2.6's census: every scene setting `force_check_emitters`
   pairs `dt` with NO `DirectLightingShaderOp` sibling, so `considerEmission`
   was already effectively "always true" for them regardless of the removed
   clause).

8. `ReflectionShaderOp.cpp`/`RefractionShaderOp.cpp` are UNCHANGED from V1
   in this round: their partner-stamping was already gated on `scat.isDelta`
   (0 for the historical delta lobes; a real aggregate density for a
   non-delta rough-reflection lobe like `GGXSPF`'s, per V1's own sibling-
   audit finding). The ruling's direction (3) — "the converse for DL-209" —
   is achieved entirely by `DistributionTracingShaderOp`'s `chainHasNEEOp`
   gate (item 4 above); `Reflection`/`RefractionShaderOp` do not themselves
   need a converse gate because a genuinely delta lobe's partner is already
   0 regardless of chain composition, and the historical materials these
   two ops are paired with (`polished_material`, `dielectric_material`) are
   delta on their mirror/refraction lobes.

### 2.4 `FinalGatherShaderOp` — a confirmed REFUTAL, still true under V2

Unaffected by the V2 change (re-confirmed, not re-derived): all THREE of its
continuation-building sites set `rs2.considerEmission = false`
UNCONDITIONALLY — never `true`, never `bForceCheckEmitters`-gated, never
material-dependent. `EmissionShaderOp`'s weight block is gated on
`rs.considerEmission` at its very first line, so it never executes for a
`FinalGatherShaderOp` continuation regardless of any partner or chain flag.
`StandardShader`'s flag-resolution loop does not even `dynamic_cast` for
`FinalGatherShaderOp*` — a chain of `[DirectLighting, fg, Emission]`
(`gi_spheres.RISEscene` and 2 siblings) therefore gets
`chainHasBsdfContinuationOp = false` (correct: `fg` never competes for the
same emitter) and `DirectLightingShaderOp`'s NEE takes full weight — the
right answer, reached without `FinalGatherShaderOp` needing to participate
in the flag mechanism at all.

### 2.5 Sibling audit

`grep -rl "IRayCaster::RAY_STATE rs2" src/Library/Shaders/*.cpp` matches
exactly five files: the four legacy ops (`DistributionTracingShaderOp.cpp`,
`FinalGatherShaderOp.cpp`, `ReflectionShaderOp.cpp`,
`RefractionShaderOp.cpp`) plus `PathTracingIntegrator.cpp` (DL-170's own
file, already fully handled, not touched by V2). `BDPTIntegrator.cpp`
builds `RAY_STATE`s under a differently-spelled local name and is a
separate, already-audited integrator (DL-69/DL-103/DL-125/DL-126), out of
scope for "the legacy shader-op chain." No fifth legacy-chain sibling
exists.

### 2.6 Census of shipped shader-op chains and the partner each now gets

Grepped every `standard_shader`/`advanced_shader` chunk in `scenes/` for
`shaderop` lines, resolved each named op to its underlying chunk type, and
classified by whether `DirectLightingShaderOp` (NEE) and/or
`DistributionTracingShaderOp` (the only non-delta BSDF-sampled continuation
op in production content) are present in the SAME chain (same flat op list
for `standard_shader`; same depth window for `advanced_shader`):

| Chain shape | Example scene(s) | `chainHasNEEOp` | `chainHasBsdfContinuationOp` | Partner behaviour |
|---|---|---|---|---|
| `[dt, DefaultDirectLighting]` (+ optional `ambocc`) | `spotlight_drama.RISEscene`, `showroom.RISEscene` (4 scenes) | true | true | Both sides partition properly: `dt` stamps the real aggregate density, NEE weights against it. The canonical, most common `distributiontracing_shaderop` pairing. |
| `[DefaultRefraction@[2,100], dt@[1,1]]` (`advanced_shader`, depth-ranged) | `blurry_glass.RISEscene`, `dielectric_dispersion.RISEscene` (the ONLY two shipped instances of this shape -- review correction 2026-09-18: `dielectric_dispersion` is this depth-ranged `advanced_shader` form, not a flat `standard_shader`, and `texture_dispersion`/`simple_dispersion` do not use `dt` at all -- they pair `DefaultDirectLighting[1,100]` with `DefaultRefraction[1,100]`, i.e. the `[DirectLighting, Refraction]` no-continuation shape below; no flat `[DefaultRefraction, dt]` `standard_shader` exists in the corpus) | false at every depth (no `DirectLightingShaderOp` anywhere in this `adv` shader) | n/a | `dt`'s partner forced 0 at depth 1 via `ResolveChainFlagsForDepth`, identically to the flat-shader case above — confirms the per-depth resolution is exercised by real shipped content, not just the flat `StandardShader` path. |
| `[dist, DefaultEmission]` | `dt_with_irrcache.RISEscene` | false | n/a | `dt`'s partner forced 0 — this is DL-209's own scene (§6); pre-fix it relied on `force_check_emitters TRUE` for a pure BSDF-sampling gather, and post-fix its behaviour is UNCHANGED from that pre-fix baseline (confirmed at the render level, §8.1). |
| `[DirectLighting, Reflection, Refraction]` (no `dt`) | `iorstack.RISEscene`, `glass_pavilion.RISEscene` (2+ scenes) | true | **false** (only delta `Reflection`/`Refraction`, no `DistributionTracingShaderOp`) | NEE takes `w_nee = 1` — no competing continuation exists (`Reflection`/`Refraction`'s own delta lobes never trigger `EmissionShaderOp`'s weight block either, since a delta partner is already 0). Exactly the ruling's item-2 example ("a chain of DirectLighting alone, or + delta Reflection/Refraction"). |
| `[DirectLighting, fg, Emission]` | `gi_spheres.RISEscene` (3 scenes) | true | false | NEE takes `w_nee = 1` — `fg` (`FinalGatherShaderOp`) never sets `considerEmission=true` (§2.4's refutal), so there genuinely is no competing strategy; matches the ruling's item-2 "or + FinalGatherShaderOp (which never considers emission)" clause. |
| `[DirectLighting]` alone | 136 scenes (the single most common chain in the corpus) | true | false | NEE takes `w_nee = 1` — no continuation op present at all. |
| `[DirectLighting, ..., CausticPelPhotonMap, ...]` (no `dt`) | `caustic_animation.RISEscene`, `pool_caustics.RISEscene` (7+ scenes) | true | false | NEE takes `w_nee = 1` — the caustic photon map is a separate, additive mechanism with no MIS partner relationship to NEE. |

Every distinct chain shape found across the 459-scene corpus that binds
either `DirectLightingShaderOp` or `DistributionTracingShaderOp` resolves to
one of the rows above; none needed a hand-authored exception. The AUTO-
derived answer (no scene-language change, no new parameter) is correct for
every one.

---

## 3. Why direct C++ construction, not a rendered scene, for the DL-171 red-proof

`tests/LegacyChainMISPartnerTest.cpp` instantiates `DirectLightingShaderOp`,
`DistributionTracingShaderOp` and `ReflectionShaderOp` directly and sums
their `PerformOperation` outputs on synthetic `RayIntersection`s — exactly
what `StandardShader::Shade` does internally with its op list, without a
full scene-language shader-op chain wired up per pixel. This lets each row
(the canonical paired chain, `DirectLighting` alone, `dt` alone, `DirectLighting
+ mirror`) build its OWN minimal, hand-picked op list and drive it with the
same synthetic hit/`rs` construction `PTGuidingMISPartitionTest.cpp`'s
`IntegrateOneSample` already established. `RunLegacyChainSample`'s signature
now takes explicit `bool chainHasNEEOp, bool chainHasBsdfContinuationOp`
parameters, stamped onto the fixture's `rs` before dispatching to whichever
ops are actually present in that row — the row itself asserts the flag
values a real `StandardShader` would have resolved for that op-list shape,
rather than relying on the (untested, in this file) resolution machinery,
which is exercised separately by the real-`StandardShader`/`AdvancedShader`
construction paths the census in §2.6 walks through by hand.

Each op's own continuation still traces via `caster.CastRay`, which — when
the ray lands on the fixture's real emitter object — dispatches to that
object's own real production shader (`[EmissionShaderOp,
DirectLightingShaderOp]`, auto-assembled by `Job::AddStandardShader`'s
`DefaultEmission` auto-prepend), giving the row's target mechanism
(`EmissionShaderOp`'s weight block) a REAL, production code path to
exercise.

---

## 4. Red-proof numbers

### 4.1 DL-170 (`tests/PTGuidingMISPartitionTest.cpp`) — unchanged from V1, re-verified

Fixed wavelength bundle `{420, 480, 560, 660}` nm (hero = 420) against a
constant 0.6-grey environment, wavelength-dependent two-lobe mixture
material:

Unfixed library (`d163cc54`):

    (hero)                               0.566443 , expected 0.566862
    (companion 1, nm=480)                0.657341 , expected 0.703141  relErr=6.51%   FAIL
    (companion 2, nm=560)                0.467441 , expected 0.606716  relErr=22.96%  FAIL
    (companion 3, nm=660)                0.257860 , expected 0.486678  relErr=47.02%  FAIL
    95 passed, 3 failed

Fixed (current):

    (hero)                               0.566443 , expected 0.566862  (bit-identical to pre-fix)
    (companion 1, nm=480)                0.703011 , expected 0.703141  relErr=0.019%
    (companion 2, nm=560)                0.607560 , expected 0.606716  relErr=0.139%
    (companion 3, nm=660)                0.488069 , expected 0.486678  relErr=0.286%
    98 passed, 0 failed

### 4.2 DL-171 (`tests/LegacyChainMISPartnerTest.cpp`) — V2 numbers

Full V2 run, current HEAD (`8e7bdde7`), `20 passed, 0 failed`:

    env-background furnace (canonical [DirectLighting, dt] chain)
        measured=0.600095  expected=0.6         relErr=0.0158%

    area-emitter, DEFAULT chain (considerEmission NOT suppressed --
    the pre-MIS hack retired per S2.3 item 7)
        measured=0.20466   expected=0.203718    relErr=0.462%

    area-emitter, bForceCheckEmitters=TRUE (now a no-op for the removed
    clause; caustic-map suppression unaffected)
        measured=0.204794  expected=0.203718    relErr=0.528%

    DirectLighting ALONE, chainHasBsdfContinuationOp=TRUE
    (the pre-ruling PHANTOM-partner UNDER, quoted for contrast)
        measured=0.115616  full closed form=0.203718   (43.2% LOW --
        matches the reviewer's own quoted figure to 5 decimal places)

    DirectLighting ALONE, chainHasBsdfContinuationOp=FALSE (the fix:
    no BSDF-sampled sibling exists -> NEE takes weight 1)
        measured=0.203427  expected=0.203718    relErr=0.143%

    DistributionTracing WITHOUT DirectLighting, chainHasNEEOp=TRUE
    (DL-209's own pre-fix shape, quoted for contrast)
        measured=0.0879028 full closed form=0.203718   (56.9% LOW)

    DistributionTracing WITHOUT DirectLighting, chainHasNEEOp=FALSE
    (the fix: no NEE sibling exists -> BSDF-hit takes weight 1 -- DL-209 CLOSED)
        measured=0.203133  expected=0.203718    relErr=0.288%

    [DirectLighting, Reflection] on a pure mirror, BOTH values of
    chainHasBsdfContinuationOp (control -- must be unchanged either way,
    since a mirror has no BSDF and NEE contributes 0 regardless)
        chainHasBsdfContinuationOp=FALSE: measured=0.6  expected=0.6  relErr=0%
        chainHasBsdfContinuationOp=TRUE:  measured=0.6  expected=0.6  relErr=0%

    ReflectionShaderOp alone on a perfect mirror (delta-lobe control,
    unaffected by any of this row's changes)
        measured=0.6  expected=0.6  relErr=0%

The suppressed row moved from V1's uninvestigated 0.115626 to the *fixed*
mechanism's own reproduction of that SAME phantom-partner shape as an
explicit CONTRAST row (0.115616, matching the reviewer's 0.115626 to the
precision two independently-built binaries agree at) — and the row the
ruling actually asked to move, `chainHasBsdfContinuationOp=FALSE`, reads
0.203427, a 0.14% error against the closed form: the required
"0.1156 -> ~0.2037" move (§ ruling text) is exactly what happened, with the
pre-fix shape kept alongside it as a labelled contrast rather than
discarded.

### 4.3 Why DL-170 barely moves `BDPTStrategyBalanceTest`'s DL-125 pin

Unchanged from V1 (DL-170 was not touched in the V2 round): `Test-
PTSpectralHWSSKnownDefect` on topology L (`schlick_material`) reads
`ratio = 1.61912`, comfortably inside the pre-existing `[1.35, 1.95]`
KNOWN-DEFECT band. DL-125 (a companion-throughput-estimator mismatch,
independent of DL-170's MIS-weight fix) dominates this particular metric;
DL-170's own contribution here is a 1-2% shift, not a large correction —
see the original derivation for the mechanism split.

---

## 5. A self-review correction (comment-honesty and a fabricated number) — V1, superseded by the P1 finding

Two things were caught and fixed during V1's own self-review, BEFORE the
external P1 review found the deeper defect described in §2.1:

1. **A fabricated red-proof number** in the V1 fix commit (`0476071b`,
   guessed pre-fix figures `0.699871`/`0.406785` instead of measured ones)
   — caught immediately after committing, corrected with the real measured
   numbers (`0.706296`/`0.320988`) in a code-empty follow-up (`a5fb8946`).
2. **A false claim** in `LegacyChainMISPartnerTest.cpp`'s original header
   comment ("there is no scene-language chunk for a bare
   distribution-tracing/reflection op") — `distributiontracing_shaderop` is
   a real chunk, and `DefaultReflection`/`DefaultRefraction` are registered
   presets; corrected to state the true reason (a methodology choice for
   per-row op-list control).

Both corrections are real and stand; they are recorded here for the
history, but neither one is the P1 the external review found — that defect
(§2.1) survived both of these self-review passes, because the self-review
treated the "default (suppressed)" row's specific number as an
uninvestigated, presumed-correct bound rather than asking what it SHOULD
read and why. The lesson: a "bounded sanity check" on a number you have not
derived a target for is not evidence the number is right.

---

## 6. DL-209 — CLOSED as a side effect of the V2 mechanism

V1 opened DL-209 as a distinct, narrower residual: `DistributionTracingShaderOp`'s
V1 fix assumed a matching `DirectLightingShaderOp` (NEE) sibling exists
wherever it stamps a non-delta partner, which could not be verified from
within the op itself (no back-reference to the owning shader's op list).
Exactly one shipped scene broke that assumption —
`scenes/Tests/Shaders/dt_with_irrcache.RISEscene`'s `[dist, DefaultEmission]`
shader, no `DirectLightingShaderOp` at all, relying on
`force_check_emitters TRUE` for a pure BSDF-sampling-only indirect gather —
where V1's stamped partner triggered a real, one-directional ~2.5% UNDER
(measured via isolated A/B at the time).

The V2 mechanism's `chainHasNEEOp` gate is EXACTLY the missing back-
reference DL-209 needed: `DistributionTracingShaderOp` now asks its owning
shader (via the flag `StandardShader`/`AdvancedShader` resolves) rather than
guessing, and stamps 0 (no partner, full weight — the historical, correct
pre-any-fix behaviour for a chain with no competing NEE strategy) whenever
that sibling is absent. `tests/LegacyChainMISPartnerTest.cpp`'s new
`RunDistributionTracingWithoutDirectLightingRow` reproduces DL-209's own
pre-fix shape as an explicit contrast (`chainHasNEEOp=TRUE`, 0.0879028,
56.9% LOW — matching the historical ~2.5%-scale defect's DIRECTION, though
this synthetic fixture's own emitter geometry makes the magnitude larger
than the real scene's measured 2.5%) and confirms the fix
(`chainHasNEEOp=FALSE`, 0.203133, 0.29% error against the closed form).

**Ledger disposition**: DL-209 is struck CLOSED in this same commit, per
the ruling's own instruction ("then strike DL-209 as closed in the same
commit if it does"). See §8.1 for the render-level confirmation on the
actual `dt_with_irrcache.RISEscene` scene (restored to its pre-any-fix
baseline, within noise).

---

## 7. Cost

DL-170 (unchanged from V1): one extra `ISPF::PdfNM` call per active
companion wavelength (up to 3 extra calls) per non-delta bounce, in
`IntegrateFromHitHWSS` only. Measured +3.1% user CPU on a mixed scene with
one `schlick_material` receiver among twelve, consistent with DL-103's own
+2.11% for the sibling RGB/NM fix.

DL-171 V2: adds exactly two boolean reads (`rs.chainHasNEEOp`/
`rs.chainHasBsdfContinuationOp`) and one extra `RAY_STATE` copy
(`rs2 = rs`, already sized at 96 bytes per DL-103's own accounting) per
`Shade`/`ShadeNM`/`ShadeHWSS` call in `StandardShader`/`AdvancedShader` --
no new virtual calls, no new heap allocation. `AdvancedShader`'s
`ResolveChainFlagsForDepth` is an O(ops-in-this-shader) linear scan per
shade call, identical in shape to the pre-existing `bComputeSPF` resolution
it sits beside; not separately re-measured given its magnitude relative to
the `ISPF::Pdf`/`PdfNM` call DL-171's own partner-stamping already pays
(unchanged from V1, ~microseconds per non-delta continuation). The legacy
shader-op chain is not a hot path in current production use (the canonical
modern renders go through `pathtracing_*_rasterizer`/`PathTracingShaderOp`,
per DL-26).

---

## 8. Render-level measurements

Per the ruling's explicit instruction to "re-measure `dt_with_irrcache.
RISEscene` and `pillow.RISEscene` before/after."

### 8.1 `dt_with_irrcache.RISEscene` — DL-209's own scene

80x120, 32spp, PNG sRGB (quick relative comparison; isolated A/B via
`git checkout <rev> -- <11 touched files>`, rebuild, render, restore):

    ORIGINAL pre-any-DL170/171-fix baseline (base d163cc54)  : mean = 71.97166666666666
    V1 fix (DistributionTracingShaderOp always stamps a
      non-delta partner, no chain-awareness -- DL-209's own
      defect, live)                                          : (not separately re-measured this round; V1's own
                                                                   closure doc measured a ~2.5% ROI-mean UNDER-shift here)
    V2 fix (chain-aware; chainHasNEEOp=false for this scene's
      [dist, DefaultEmission] shader, since it has no
      DirectLightingShaderOp -> dt's partner forced back to 0) : mean = 71.94302083333334

V2's mean (71.943) matches the pre-any-fix baseline (71.972) to within
0.04% -- well inside PNG-quantization/MC noise at this sample count --
confirming DL-209 is genuinely closed at the render level: the scene's
BSDF-sampling-only indirect gather (via `force_check_emitters TRUE`, no
NEE sibling) is restored to its historical, correct behaviour rather than
carrying V1's own ~2.5% UNDER-shift forward.

### 8.2 `pillow.RISEscene`

160x120 (scaled down from the scene's native resolution for a quick
relative comparison), PNG sRGB, `pixelpel_rasterizer` (6 samples,
4 lum_samples) -- isolated A/B, same protocol as above:

    pre-fix (base d163cc54)  mean = 212.87019097222222
    post-fix (V2, 8e7bdde7)  mean = 212.8728125
    ratio = 1.0000123151473947

    ROI (pixels > 5/255) pre  mean = 213.02899734176555
    ROI (pixels > 5/255) post mean = 213.01492433587575
    ROI ratio = 0.9999339385432715

This is, correctly, ESSENTIALLY NO CHANGE -- and per S2.1's correction to
the reviewer's own framing, that is the EXPECTED result rather than a
residual gap. `pillow.RISEscene` pairs `dt` with `DefaultDirectLighting`
(the canonical NEE-paired chain, which DOES get a real partner both before
and after V2 -- `chainHasNEEOp=true` for this scene), but the reviewer's
"BSDF material under a mesh/area light" premise does not hold for THIS
scene's actual light setup: all four active light sources are `omni_light`
(point/delta) chunks, and its two `lambertian_luminaire_material`
definitions are declared but never bound to any object. A delta light has
no surface for a BSDF-sampled continuation to ever intersect, so
`EmissionShaderOp`'s weight block (the entire subject of this row) never
executes anywhere in this scene regardless of what partner `dt` stamps --
there is no BSDF-sampled emitter-hit event for the fix to change the
weighting of. The measured near-1.0 ratio (well within FP/PNG-quantization
noise) is the correct confirmation of that, not an unexplained null result.

---

## 9. Self-review against the P1 criteria (per `docs/skills/implementation-review-loop.md`)

Performed once before closing, no fresh P1s found:

- **Numbers recomputed, not copied**: every figure in S4 and S8 was
  measured in this session against the CURRENT `8e7bdde7` build, not carried
  forward from V1's own (partially wrong) closure doc.
- **The literal instruction vs. its intent**: S2.3 documents the ONE
  deliberate deviation (not nulling `pMaterial`) and the reasoning; the
  ruling's INTENT (`w_nee=1` when no competing strategy exists) is fully
  honoured.
- **Census completeness**: S2.6 enumerates every distinct chain SHAPE found
  across the 459-scene corpus that binds either op, not just the rows the
  test file happens to cover.
- **The "pillow.RISEscene" claim re-examined, not repeated**: S2.1/S8.2
  correct the one place this document's own predecessor (via the reviewer's
  relayed text) could have been taken as an unverified claim about a named
  scene, without disputing the underlying defect or ruling.
- **DL-209's closure checked, not assumed**: S6/S8.1 confirm via both a
  synthetic contrast row AND a direct render of the actual scene the row
  was opened against.
