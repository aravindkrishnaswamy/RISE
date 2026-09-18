# DL-126 — BDPT/VCM/MLT terminated at any null-BSDF material

Status: **CLOSED 2026-09-18** (debt-dl126 slice, branch `debt-dl126`).
Red-proofs `tests/BDPTStrategyBalanceTest.cpp` topology N and
`tests/VCMStrategyBalanceTest.cpp` topology J (both new), plus a direct
`mlt_rasterizer` bootstrap sanity render.

Related: DL-69 (whose sibling audit opened this row; its own doc's
account of *which* gate killed the walk was itself wrong -- corrected
below and in `docs/DL69_BDPT_LOBE_THROUGHPUT.md`), DL-125 (the HWSS
companion fallback pattern this fix's HWSS branch deliberately mirrors).
**Correction (review round 4, P2-2): an earlier revision of this line
called DL-131 "a distinct, unrelated `GenericHumanTissueSPF` in-medium
bug, explicitly NOT touched here" -- that was wrong.  DL-131 (opened by
the concurrent `debt-pushgates` slice) and DL-184 (SS7.4 below) describe
the IDENTICAL defect in the IDENTICAL function; DL-184's own fix
(`b4172515`) closes both.  DL-184's ledger row is now struck as a
"DUPLICATE of DL-131 -- id retired"; DL-131 is the authoritative id
going forward.  See docs/DEBT_LEDGER.md's DL-131/DL-184 rows.**

---

## 1. The bug

`BioSpecSkinMaterial::GetBSDF()` and `GenericHumanTissueMaterial::
GetBSDF()` both `return 0` -- no aggregate BSDF exists for these two
materials at all; their `IScatter` model (a full Krishnaswamy-Baranoski
layered Monte Carlo simulation, or a simplified translucent-membrane
model) reports its result directly as a `ScatteredRay::kray` weight
and never separately exposes an evaluable `f(wi,wo)` or a tracked
`.pdf` (both SPFs' `Scatter`/`ScatterNM` leave `ScatteredRay::pdf` at
its struct default, 0 -- `ISPF::Pdf`/`PdfNM` are also left at the
base-class default, 0).

`BDPTIntegrator.cpp`'s shared eye/light subpath generator
(`GenerateEyeSubpathImpl`/`GenerateLightSubpathImpl`, templated over
RGB/NM, and reused unmodified by VCM and MLT) has always required a
usable aggregate density to CONTINUE a non-delta subpath.  Two
independent gates each unconditionally kill it at such a vertex:

```cpp
Vector3 scatDir = pScat->ray.Dir();
Scalar effectivePdf = pScat->pdf;
...
if( effectivePdf <= 0 ) {
    break;                          // <-- (1) fires FIRST: pScat->pdf == 0
}
...
} else {                            // non-delta branch
    V f = ...EvalBSDFAtVertex...;   // == 0, GetBSDF() is null
    if( PositiveMagnitude<Tag>( f ) <= 0 ) {
        break;                      // <-- (2) would ALSO fire, never reached
    }
```

**Gate (1) is the one that actually fires** -- `effectivePdf` is
`pScat->pdf`, which these two SPFs never set, so the ordinary
(un-guided) case reads it as exactly 0 and `break`s before the
delta/non-delta branch is even entered.  Gate (2), the vestigial
`PositiveMagnitude(f) <= 0` check DL-69's sibling audit and its own doc
blamed, is a REAL defect too (the aggregate BSDF genuinely is 0 for
these materials, since `GetBSDF()` is null) but never gets the chance
to run.  **DL-69's own doc had this backwards** -- see its "CORRECTION"
addendum, added alongside this fix.

Every subpath that scatters off one of these materials therefore
terminates AT that vertex, in every tag (RGB/NM/HWSS) and in VCM and
MLT (both share this generator).  PT is entirely unaffected: its
ordinary continuation is `PTScatterKray<Tag>(*pS) * (1/selectProb)`
(`PathTracingIntegrator.cpp`) -- it never reads `.pdf` to decide
whether to continue, and its own "Specular surfaces (no BSDF -- use
SPF)" branch is gated on `!pBRDF`, the exact condition these materials
trigger.

The same null `GetBSDF()` also makes `vertices.back().isConnectible ==
false` (`BDPTIntegrator.cpp` sets this from `ri.pMaterial->GetBSDF() !=
0`), so these vertices already correctly support NO connection
strategy -- the loss this row fixes is of the BSDF-SAMPLED
CONTINUATION only, never of a connection (there was none to lose).

---

## 2. Red-proof

### 2.1 Render-level (BDPT)

`tests/BDPTStrategyBalanceTest.cpp` topology N: a `biospec_skin_material`
quad (every parameter at its documented default) lit by the same mesh
area emitter as topology B, 32x32, PT (`pathtracing_pel_rasterizer`) vs
BDPT (`bdpt_pel_rasterizer`), both 32 spp, `oidn_denoise FALSE`,
`pixel_filter box`.

| build | PT mean | BDPT mean | ratio |
|---|---|---|---|
| pre-fix (`3299cd78`, master HEAD) | 0.0615871 | **0.0000000** | 0.0% |
| post-fix | 0.0615871 | 0.0602271 | 97.8% |

Pre-fix BDPT reads EXACTLY zero on all three channels -- not "far
below PT", but a subpath of length 1 with no reachable second vertex at
all, matching the "both gates unconditionally break" analysis above.
Post-fix the two agree well inside the suite's 8%/25%/100% (mean/p99/max)
band.

### 2.2 Render-level (VCM)

`tests/VCMStrategyBalanceTest.cpp` topology J: identical scene.  This
file's default PT reference is the LEGACY `pixelpel_rasterizer` +
`DefaultDirectLighting`, which never calls `ISPF::Scatter` at all (see
`BDPTStrategyBalanceTest.cpp`'s own comment on why it stopped using
that reference) and would read black for BOTH pre- and post-fix VCM --
masking the defect.  This topology therefore pairs a modern-PT
reference (`pathtracing_pel_rasterizer`) with the existing
`kRasterizerVCM` (already `DefaultPathTracing`-driven).

| build | PT mean | VCM mean | ratio |
|---|---|---|---|
| pre-fix (`3299cd78`) | 0.06081 | **0.0000000** | 0.0% |
| BDPTIntegrator.cpp fix only (VCMIntegrator.cpp still pre-fix) | 0.06081 | 0.000992 | 1.6% |
| both fixes | 0.06081 | 0.0598393 | 98.4% |

The middle row is the discovery that this row's own fix, applied ONLY
to `BDPTIntegrator.cpp`, is not sufficient for VCM -- see §4.

### 2.3 MLT sanity

A direct `mlt_rasterizer` render of the same scene (32x32, bootstrap
20000, 16 chains, 64 mutations/pixel):

* pre-fix: `MLTRasterizer:: Bootstrap found zero luminance -- scene
  produces no visible light!` (a hard bootstrap failure, not merely a
  dim render).
* post-fix: `MLTRasterizer:: Bootstrap complete. Mean luminance =
  0.067800` -- consistent with PT's 0.0608 mean (MLT's bootstrap
  luminance is a scalar importance function over the whole image, not
  directly comparable pixel-for-pixel, but the order of magnitude and
  nonzero-ness are what this sanity check verifies).  MLT shares
  `BDPTIntegrator` directly (`MLTRasterizer.cpp` constructs one), so it
  inherits both fixes with no code of its own.

### 2.4 Subpath-length characterization

A dedicated low-level harness invoking `BDPTIntegrator::GenerateEyeSubpath`
directly (bypassing the full render path) was considered but not built:
constructing the minimal `IScene`/`IRayCaster`/`RuntimeContext`/`ISampler`
fixture it needs duplicates most of the render machinery the render-level
tests in §2.1/§2.2 already exercise, for a marginal gain over what those
already show unambiguously.  The render-level result IS the subpath-length
signal in this case: BDPT/VCM reading EXACTLY zero pre-fix is only possible
if every subpath through the null-BSDF vertex terminates there (length 1,
zero contribution, zero further connections) -- any longer subpath, even
one that failed to connect anywhere, would still show up as SOME nonzero
image energy via its own s=0 (BSDF-hits-emitter) strategy if it ever
reached the light, which it provably cannot without continuing past this
vertex. The exact-zero result is therefore already the deterministic,
reproducible evidence the recipe asked for.

---

## 3. The fix

### 3.1 What a null-BSDF vertex can legally do (derivation)

* **Continue: YES.**  The SPF's `kray` is the material's own
  already-integrated, unbiased Monte Carlo transport weight for the
  lobe it drew (RISE's SPF/`kray` contract since `PTScatterKray`
  shipped) -- there is no requirement that `kray` factor as `f*cos/p`
  for some separately-evaluable `f` and `p`; that factoring simply does
  not exist for these two materials.  The correct throughput update is
  therefore exactly the DELTA branch's own formula:
  `KrayValue<Tag>(*pScat) * (bssrdfReflectCompensation / selectProb)` --
  no aggregate BSDF, no pdf division.
* **Connection endpoint: NO** -- unaffected by this fix, already
  correct.  `isConnectible = (GetBSDF() != 0)` is already false, and
  every connection-evaluation site in `BDPTIntegrator.cpp` either gates
  on `isConnectible` directly or self-gates via
  `PositiveMagnitude(EvalBSDFAtVertex(...)) <= 0` (which is 0 for a
  null aggregate BSDF regardless).  `VCMIntegrator.cpp`'s
  `EvaluateMerges` also already gates on `isConnectible` before
  querying the photon-vertex store.  Confirmed by direct source read;
  no code change needed for any of these.
* **MIS at that vertex:** treat it EXACTLY like a delta vertex.  See §3.2.
* **Emit/splat (s=0/t=1 through it, i.e. the walk reaching an emitter
  AFTER passing through this vertex):** fine, and is in fact the ONLY
  way energy reaches the film in the red-proof scene (no NEE is
  possible at this vertex either way).  No special-casing needed --
  the emitter-hit vertex two steps later is an ordinary SURFACE vertex
  like any other.

### 3.2 MIS ruling: is `isConnectible`'s existing "treat it like a
delta vertex" convention sound, and does the code already do it?

**No** -- `BDPTIntegrator::MISWeight`'s light-side and eye-side ratio
walks skip a vertex from the denominator sum ONLY on `vi.isDelta`, not
on `!vi.isConnectible`:

```cpp
if( vi.isDelta ) { continue; }               // (light-side walk)
...
if( vj.isDelta ) { continue; }               // (eye-side walk)
```

For a null-BSDF, NON-delta scatter (`pScat->isDelta == false` for both
affected SPFs -- their re-emission is not a Dirac lobe), `vi.isDelta`
is false, so the vertex is NOT skipped, even though it is exactly as
unreconstructable by a rival strategy as a delta vertex is (per §3.1's
"connection endpoint: NO" -- `isConnectible`'s own contract comment
already states the underlying principle: "a null-BSDF vertex evaluates
every connection to 0 ... so marking it connectible would reserve MIS
mass for zero-yield strategies").  Left as `isDelta`-only, this
DEFLATES the weight of every real strategy that generates a path
through such a vertex: the phantom "connect here" alternative gets
added to `sumWeights` with `ri` UNCHANGED by remap0 (both `pdfFwd` and
`pdfRev` end up 0 at this vertex under DL-126's fix, per the delta-
transparency convention, so remap0 maps BOTH to 1 and the ratio
contributes as if it were an ordinary, fully competitive strategy --
not the zero-density non-existent one it actually is).

**Fix**: both walks (and their "neighbour must also be non-delta"
checks) now skip on `vi.isDelta || !vi.isConnectible` (light side) /
`vj.isDelta || !vj.isConnectible` (eye side), preserving the existing
NEE-at-a-delta-light exception on the light side unchanged.  This
mirrors the EXISTING convention immediately above `MISWeight`'s own
walks, which already clears `isDelta` on a connection endpoint "if
`isConnectible`" -- i.e. the codebase already treats "not connectible"
as a stronger, delta-like condition at that one site; `MISWeight`'s
denominator walk had simply not been brought into line with it.

Sound?  Yes: MIS unbiasedness requires only that the weights assigned
to the techniques that CAN generate a given path sum to 1; a technique
that provably cannot generate it (density exactly, structurally zero --
not merely small) must be excluded from that sum, not included with a
placeholder ratio.  A null-BSDF vertex's "connect here" alternative is
exactly such a technique.

### 3.3 VCM's OWN recurrence needed the identical fix, found by testing

VCM shares `BDPTIntegrator`'s subpath generator but computes its own
Georgiev dVCM/dVC/dVM running quantities in a SEPARATE post-pass
(`VCMIntegrator.cpp`'s `ConvertLightSubpath`/`ConvertEyeSubpath`),
reading back `pdfFwd`/`isDelta` from the same `BDPTVertex` array.  Both
functions' "prepare the recurrence for the NEXT vertex" step branches
on `v.isDelta` alone:

```cpp
if( v.isDelta ) {
    mis = ApplyBsdfSamplingUpdate( mis, cosThetaOut, 0, 0, /*specular*/true, norm );
    continue;
}
// non-specular branch: requires next.pdfFwd, which is 0 here (the
// delta-transparency marker DL-126's fix leaves on the FOLLOWING
// vertex) -- so this silently `continue`s with NO update at all,
// leaving `mis` frozen at its state from BEFORE this vertex instead
// of propagating `cosThetaOut` through it.
```

This is the SAME bug pattern one layer up: a null-BSDF vertex is not
delta, so it takes the "non-specular" branch, whose density-based
update is unavailable (dead-ends on `next.pdfFwd <= 0`) and silently
no-ops instead of falling back to the delta-style propagation.  The
consequence measured directly (§2.2's middle row): with ONLY
`BDPTIntegrator.cpp` fixed, VCM's biospec-skin topology read **1.6%**
of PT's mean -- continuation was enabled, but VCM's own S0
("eye-subpath-hits-emitter") MIS weight formula, which consumes the
`dVCM`/`dVC` running quantities, was starved of the propagation through
the skin vertex and came out catastrophically under-weighted.

Fixed identically to §3.2's reasoning: both `ConvertLightSubpath` and
`ConvertEyeSubpath` now branch on `v.isDelta || !v.isConnectible`,
routing a null-BSDF vertex through the SAME "opaque, no finite SA
density, but still propagate `cosThetaOut`" specular-style update a
genuine delta lobe already gets.  Post-fix, VCM reads 98.4% of PT
(§2.2's last row).

**Scope note.**  This slice's brief named `BDPTIntegrator.cpp` /
`PathVertexEval` / materials as the intended touch set, to avoid
conflicting with concurrent slices working in
`PathTracingIntegrator.cpp` / `LightSampler.cpp`.  `VCMIntegrator.cpp`
was not named either way.  This fix was made anyway, and is called out
explicitly here, because: (a) it is the exact same bug pattern as
§3.2, caught by the audit-by-bug-pattern pass this row's own recipe
required; (b) without it, this row's own stated success criterion
("VCM/PT ratio -> 1 ± noise") is unreachable; (c) it does not touch
either of the two files named as concurrently owned.

### 3.4 HWSS companions

The new `nullBSDFContinuation` branch in both generators mirrors the
DELTA branch's existing HWSS treatment exactly: a hero-only
`krayNM`-based scale applied to every live companion wavelength.
Neither `BioSpecSkinSPF` nor `GenericHumanTissueSPF` overrides
`ISPF::EvaluateKrayNM` (confirmed by grep), so this is the only
information available regardless -- consistent with, and not a new
instance of, DL-125's tracked pattern (that row already covers "HWSS
companion fallback wherever `EvaluateKrayNM` declines").

---

## 4. Sibling audit (docs/skills/audit-by-bug-pattern.md)

**Bug pattern in one sentence:** a material with no evaluable aggregate
BSDF (`GetBSDF() == 0`) but a non-delta SPF sample was treated as a
hard path-termination condition instead of a "cannot connect, but can
continue and must be MIS-transparent" one.

| candidate | GetBSDF() | SPF non-delta lobes? | verdict |
|---|---|---|---|
| `BioSpecSkinMaterial` | null | yes (`BioSpecSkinSPF::Scatter`, `isDelta=false`) | **AFFECTED**, fixed |
| `GenericHumanTissueMaterial` | null | yes (`GenericHumanTissueSPF::Scatter`, `isDelta=false`) | **AFFECTED**, fixed |
| `DielectricMaterial` | null | `DielectricSPF::GenerateScatteredRay` sets `isDelta = true` UNCONDITIONALLY on both lobes, even under the `scattering`/HG diffuse-transmission warp | **IMMUNE** -- always takes the pre-existing, already-correct delta branch |
| `PerfectReflectorMaterial`, `PerfectRefractorMaterial` | null | pure delta by construction | **IMMUNE** |
| `NullMaterial` (`Material.h` base default) | null | `GetSPF()` ALSO returns null -- no SPF to scatter with at all | **IMMUNE**, trivially (moot: nothing can call `Scatter`) |
| every other material (`GetBSDF()` non-null: GGX, CookTorrance, Schlick, Ward x2, Composite, Coated, Polished, Translucent, Fabric, Weave, Hair, Lambertian, OrenNayar, Sheen, IsotropicPhong, AshikminShirley, DataDriven, SubSurfaceScattering, RandomWalkSSS, DonnerJensenSkinBSSRDF, the two Luminaire wrappers) | non-null | n/a | **NOT IN SCOPE** -- `isConnectible` is true, the ordinary DL-69 throughput path already applies |

Consumers of `isDelta` as an implicit connectibility proxy, audited
across `BDPTIntegrator.cpp` and `VCMIntegrator.cpp`:

* `BDPTIntegrator::MISWeight`'s two ratio walks -- **FIXED**, §3.2.
* `VCMIntegrator.cpp`'s `ConvertLightSubpath`/`ConvertEyeSubpath` --
  **FIXED**, §3.3.
* `VCMIntegrator::EvaluateMerges` -- audited, ALREADY gates on
  `!v.isConnectible` before querying the store (`VCMIntegrator.cpp`
  line ~2301); no change needed.
* The medium-vertex `connectible` derivation in
  `GenerateEyeSubpathImpl`/`GenerateLightSubpathImpl` (a MEDIUM vertex
  enclosed by a specular boundary) -- audited, ALREADY keys off
  `prev.isConnectible`, not `prev.isDelta` (fixed for debt 23,
  `docs/CLOTH_FABRIC_DESIGN.md` §15); a null-BSDF boundary crossing
  correctly makes the medium non-connectible too, no change needed.
* The HWSS hero/companion ratio helper (`BDPTIntegrator.cpp`, the
  per-vertex loop computing `cumulativeRatio` for dispersive delta
  chains) -- audited, ALREADY gates its BSDF-ratio branch on
  `v.pMaterial->GetBSDF()` in addition to `!v.isDelta`; a null-BSDF
  vertex already falls through to the documented "Delta, BSSRDF,
  medium, endpoints: scatter ratio = 1.0" default.  No change needed.
* `BDPTIntegrator::HasDispersiveDeltaVertex` -- only inspects
  `v.isDelta` vertices (`continue` on `!v.isDelta`); a null-BSDF,
  non-delta vertex is correctly excluded from dispersion analysis
  (it is not a Snell/Fresnel vertex).  No change needed.

No further sibling sites found.

---

## 5. Gate

Clean rebuild (`make -C build/make/rise -j8 all`), zero warnings, on
both `BDPTIntegrator.cpp` and `VCMIntegrator.cpp`.

| suite | result |
|---|---|
| `BDPTStrategyBalanceTest` | 99 passed / 0 failed (was 97/2 pre-fix, new topology N red -> green) |
| `VCMStrategyBalanceTest` | 74 passed / 0 failed (new topology J red -> green) |
| `MISWeightsTest` | 59/0 |
| `BDPTVertexRIGRebuildTest` | 68/0 |
| `BDPTPhantomStrategyWeightTest` | all passed (BDPT ratio 1.0098, VCM ratio 1.0121 on its control scene) |
| `ConnectionLegalityTest` | 316 passed / 0 failed |
| `VCMEyePostPassTest` | 31/0 |
| `VCMLightPostPassTest` | 34/0 |
| `PSSMLTStreamAliasingTest` | all passed |
| `SSSRadianceScalingTest` | 574017 guards passed / 0 failed (unchanged from the documented baseline -- this fix does not touch SSS materials) |
| `CstDeriveGoldenTest` | 452 MATCH / 0 DRIFT |
| `SourceHygieneTest` | 165/0 |

MLT sanity: direct `mlt_rasterizer` bootstrap on the red-proof scene
goes from "Bootstrap found zero luminance" (pre-fix) to a normal
bootstrap (mean luminance 0.0678, post-fix), with no code change in
`MLTRasterizer.cpp` (it shares `BDPTIntegrator` directly).

---

## 6. Files changed

* `src/Library/Shaders/BDPTIntegrator.cpp` -- `nullBSDFContinuation`
  branch in both `GenerateEyeSubpathImpl`/`GenerateLightSubpathImpl`
  (RGB and NM), both eye and light generators; `MISWeight`'s two ratio
  walks now skip on `isDelta || !isConnectible`.
* `src/Library/Shaders/VCMIntegrator.cpp` -- `ConvertLightSubpath`/
  `ConvertEyeSubpath`'s outgoing BSDF-sampling-update branch selection
  now also checks `!v.isConnectible` (§3.3).
* `tests/BDPTStrategyBalanceTest.cpp` -- new topology N.
* `tests/VCMStrategyBalanceTest.cpp` -- new topology J.
* `docs/DL69_BDPT_LOBE_THROUGHPUT.md` -- corrected the stale "which
  gate kills the walk" claim; closed the DL-126 table row.
* `docs/DEBT_LEDGER.md` -- DL-126 row closed.
* `CLAUDE.md` -- new High-Value Facts entry.
* `tests/README.md` -- new rows for the two changed test files (already
  listed; row text updated to mention the new topologies).

---

## 7. Review round 2 (2026-09-18) -- one real P1, four corrections

An independent (Opus) review confirmed the MIS ruling in §3.2 and the
VCM-recurrence fix in §3.3 (re-derived the (s,t) enumeration
independently; convergence checked at 32/256/16384 spp for BDPT and up
to 4096 for VCM), found no regression on any other topology, and
reproduced this doc's own red-proof numbers including the 1.6%
intermediate in §2.2. It also found one real defect in round 1's own
fix and four smaller issues.

### 7.1 P1 (real defect): HWSS companion wavelengths were grey-ified

Round 1's `nullBSDFContinuation` branch, in both eye/light generators,
scaled every live HWSS companion wavelength by the hero's own
`krayNM` -- copying the DELTA branch's convention without re-deriving
whether it applies. **It does not.** A delta lobe (a mirror) reflects
every wavelength identically, so broadcasting one scalar to the whole
bundle is exact; `biospec_skin_material`'s colour is precisely its
per-wavelength absorb/survive Monte Carlo draw being
wavelength-DEPENDENT, so broadcasting the hero's own realized outcome
made every companion inherit the hero's draw instead of its own --
grey, and badly over-bright in the blue companions this material's
red-dominant absorption spectrum should have mostly killed. Measured
(review, 2026-09-18, 32x32/2048spp converged): hwss=TRUE BDPT
`(0.0607,0.0632,0.0661)` / VCM `(0.0611,0.0637,0.0667)` against PT
`(0.0849,0.0407,0.0175)` -- achromatic ratio 1.33x, blue channel 3.8x
PT.

**The fix was in the wrong place.** `hwssBetaNM` (round 1's edit site)
is read only for Russian-Roulette max-throughput and OpenPGL training
-- never for the rendered image. The actual per-companion-wavelength
image contribution for BDPT/VCM/MLT spectral HWSS is computed by
`BDPTIntegrator::RecomputeSubpathThroughputNM`, called once per
companion wavelength by each of `BDPTSpectralRasterizer.cpp` /
`VCMSpectralRasterizer.cpp` / `MLTSpectralRasterizer.cpp` on a COPY of
the hero-wavelength subpath (`RecomputeSubpathThroughputNM` walks the
stored vertices and multiplies a running `cumulativeRatio` by each
vertex's companion/hero BSDF ratio). Its own Phase-3 scatter-ratio
computation had the SAME bug pattern this whole row is about: it
required `v.pMaterial && v.pMaterial->GetBSDF()` before computing a
ratio, and fell through to "ratio = 1.0" (the file's own documented
default for "Delta, BSSRDF, medium, endpoints") for a null-BSDF
vertex -- silently reusing the "a delta lobe needs no ratio" convention
for a material where that premise is false. First fix attempt (zero
the companion's contribution outright, no termination bookkeeping)
correctly fixed the B/R shape (0.219 vs a 0.242 reference) but
UNDER-corrected the achromatic mean to 25% of hero-only, because a
zeroed-but-not-terminated companion still counts toward
`totalActive`/the sample-count denominator -- the identical dilution
bug the file's existing `swl.terminated[]` mechanism exists to prevent
for dispersive delta vertices (its own comment: "Counting a terminated
companion as a zero-contribution sample divided the bundle mean by N
instead of the surviving count"). Fixed properly with a new static
helper, `BDPTIntegrator::HasNullBSDFContinuationVertex`, checked
alongside the existing `HasDispersiveDeltaVertex` scan in all three
rasterizers so a null-BSDF vertex calls `swl.TerminateSecondary()` up
front (one call, not per-companion, since the condition is
wavelength-INDEPENDENT unlike dispersion) -- excluding those
companions from the denominator exactly like a dispersion-terminated
one.

Post-fix on topology N (32x32, 1024spp hero-only / 256spp hwss=TRUE):
one run measured achromatic ratio 1.00415 (+0.4%), B/R ratio 0.225 vs
a 0.234 reference (was 1.34x / 1.09). Red-proof and fix are `tests/
BDPTStrategyBalanceTest.cpp`'s `TestNullBSDFHWSSCompanionLadder` plus
commits `d91ce076` (test) / `f7944713` (fix).

**Correction (review round 4, P3): quote the SPREAD, not that single
run.** This scene's own run-to-run variance (no fixed seed across
threads; `BlockRasterizeSequence` shuffles from `std::random_device`)
is large enough that the one number above understates it. Three fresh
repeated runs at the current HEAD (post every fix in this document,
including P1-1's unrelated MLT change, which this scene's BDPT-only
path never touches) read:

| run | achromatic ratio | achromatic %  | B/R ref  | B/R hwss=TRUE | B/R relative |
|-----|-------------------|---------------|----------|---------------|--------------|
| 1   | 1.04754           | +4.75%        | 0.246075 | 0.214039      | -13.0%       |
| 2   | 1.00867           | +0.87%        | 0.232389 | 0.206951      | -11.0%       |
| 3   | 1.06087           | +6.09%        | 0.235839 | 0.214039      | -9.2%        |

All three are well inside the 10%/35% bands the test gates on, and all
three are two orders of magnitude below the pre-fix +34%/grey-ified
defect -- but none of the three is the earlier +0.4% figure, and the
B/R relative deviation is consistently in the 9-13% range, not the
single run's -3.8%. Do not quote a single cherry-picked run for this
topology's HWSS row (the same rule this document's SS7.5 already
states for the non-HWSS VCM/PT ratio on the same scene).

### 7.2 P2-1 (documented, not filed): a medium-vertex `isConnectible`
asymmetry between light- and eye-rooted derivations

`BDPTIntegrator.cpp`'s medium-vertex `connectible` derivation (the
"medium enclosed by a specular boundary" logic, ~line 1835) inherits
connectibility from the IMMEDIATELY PRECEDING vertex on THAT subpath:
`connectible = false` only if `prev.type` is `SURFACE`/`MEDIUM` AND
`!prev.isConnectible`. A `LIGHT`-type predecessor (the light subpath's
own root) never satisfies that type check, so a light emitting DIRECTLY
INTO an enclosed medium leaves the medium's first vertex
`isConnectible = true` by the default -- even though it sits inside the
same enclosure a specular-boundary-crossing EYE walk reaching a
similar-looking medium point would correctly mark `false`.

**On reflection, this is very likely NOT a partition defect, and the
reason clarifies what `isConnectible` actually certifies.** A given
rendered PATH has one fixed vertex sequence; `isConnectible` at a
vertex position is evaluated once, from that ONE walk's own history,
and is consulted only by strategies re-splitting THAT SAME sequence at
a different (s,t) -- never compared across two independently-generated
paths that happen to visit similar-looking 3D coordinates by
coincidence. The light-rooted case above is correct for what it
describes: a light emitting directly into a medium with NO intervening
specular surface between the emission point and that first scatter
really can be connected to (no boundary blocks a straight line back to
the light). The eye-rooted case is a genuinely different situation
(the eye ray had to cross a real specular boundary to arrive at ITS
first medium vertex), correctly marked non-connectible. Both
derivations are locally correct for the walk that produced them; there
is no shared vertex position whose two conflicting classifications
would ever need to be reconciled inside one `MISWeight` call.

Not filed as a new row: this reasoning, plus the reviewer's own
preliminary check (`RefractiveRadianceScalingTest` 41/0,
`VolumeAbsorptionAttenuationTest` 89/0, both re-confirmed unchanged
after every fix in this document), together argue against a real
defect. A dedicated isolated render (a light literally inside a
specular-enclosed medium, BDPT vs PT) was NOT built in this round --
constructing one that isolates this mechanism from confounds (a real
dielectric boundary's own Fresnel behaviour; a `perfectrefractor` at
`ior=1.0` degenerately makes a straight-line connection THROUGH the
boundary exact, which could mask rather than expose the effect) is
nontrivial, and is left as a residual for whoever next touches this
derivation, rather than shipping a rushed and possibly misleading
measurement.

### 7.3 P2-2: stale comment corrected

`BDPTIntegrator.cpp`'s "WHICH MATERIALS REACH THAT FALLBACK" comment
(the `misFwdPdf <= NEARZERO` block near the `pdfFwdPrev` derivation)
claimed BioSpecSkin/GenericHumanTissue "never get here" because the
`PositiveMagnitude(f) <= 0` gate breaks first -- wrong even before
DL-126 closed (§1 already established the ACTUAL prior gate was
`effectivePdf <= 0`), and doubly stale now that the
`nullBSDFContinuation` branch routes those materials around the whole
block. Corrected in place; `CLAUDE.md`'s DL-69 bullet carried the
identical stale claim and was corrected too.

### 7.4 P2-3 -- DL-184: `GenericHumanTissueSPF`'s dead interior scatter
lobe (CLOSED, same round)

A distinct, unrelated defect found while building this row's own
red-proof scene in round 1: `GenericHumanTissueSPF::Scatter`/
`ScatterNM`'s interior branch sampled a scattering direction inside
`if( x < (pa + ps) )` and then unconditionally overwrote it with
`trans.ray.SetDir(ri.ray.Dir())` on the very next, un-braced statement
-- every interior interaction was straight-through regardless of the
scattering roll, in both RGB and NM. Fixed by moving that statement
into the matching `else`. Red-proof:
`tests/GenericHumanTissueInteriorScatterTest.cpp` (new) -- direction-
changed=0 on all four (RGB/NM x diffuse/HG) configurations pre-fix
despite thousands of non-absorbed, scattering-branch trials post-fix
direction-changed == non-absorbed on all four. Commits `50cc5d3f`
(test) / `b4172515` (fix). Distinct from DL-183 (this material's
scattered-ray origin bug, still open, not touched).

**Review round 4 (P2-2) correction and closure.** Two things were
wrong in this section as originally written.

1. This section's own opening line called DL-184 "a distinct,
   unrelated defect" -- unrelated to DL-126/DL-183, yes, but it is
   NOT unrelated to DL-131: DL-131 (opened earlier by the concurrent
   `debt-pushgates` slice) describes the IDENTICAL defect in the
   IDENTICAL function. Round 1 and round 2 of this slice simply
   never cross-referenced the ledger's other open rows before
   filing DL-184 as new. `b4172515` (already cited above) closes
   both; DL-184's own ledger row is now struck as "DUPLICATE of
   DL-131 -- id retired" and DL-131 carries the authoritative
   status. See docs/DEBT_LEDGER.md's DL-131/DL-184 rows and this
   doc's own header correction above.

2. The incidental `ScatterNM` interior-HG-branch finding below was
   left unfiled in round 2 pending a decision between filing it as
   DL-202 or fixing it directly. It turned out to be a genuine
   one-line fix (the OUTSIDE branch two lines below already shows
   the correct call), so it was fixed directly rather than filed:
   `ScatterNM`'s interior HG branch read the phase-asymmetry
   parameter via `pG->GetValuesAt(ri).v[0]` (the RGB accessor)
   instead of `GetValueAtNM(ri,nm)` -- the in-medium HG lobe's `g`
   was not actually wavelength-resolved in the NM pipe. Fixed in
   the same commit as this round's other P2-2 items;
   `GenericHumanTissueInteriorScatterTest` (8/0) re-run clean
   post-fix (its own checks don't discriminate this specific
   accessor choice, since they only assert direction-changed vs.
   non-absorbed counts, not the sampled angle's wavelength
   dependence -- no new automated check was added for this
   specific one-line accessor swap given its narrow,
   spectral-accuracy-only scope).

### 7.5 P3s

* **VCM/PT re-quote at final HEAD** (`f7944713`): renders are not
  bit-reproducible run to run (no `srand`, `BlockRasterizeSequence`
  shuffles from `std::random_device`) and this topology's 32 spp /
  no-fixed-seed setup carries real spread. Seven repeated runs read
  VCM/PT = 0.9648, 0.9805, 0.9840, 0.9999, 1.0032, 1.0072, 1.0074 --
  MEAN 0.9924 (99.2% of PT), well inside the suite's 8% band on every
  individual run. Do not quote a single cherry-picked run (this doc's
  earlier §2.2 table and its 98.4%/1.6% figures are a two-point
  before/after CHARACTERIZATION from one A/B pair each, not a
  converged estimate, and are left as originally measured).
* **Gate table counts**: re-measured against the current HEAD (below,
  §8) rather than the round-1 numbers quoted in §5 (which predate the
  P1 fix and the merge with master's concurrent slices) --
  `ConnectionLegalityTest` 319/0 (not 316), `SourceHygieneTest` 167/0
  (not 165); both deltas are new unrelated test files the master merge
  brought in, not anything this row changed.
* **`MISWeight`'s DL-126 comment rewritten**: it claimed the skipped
  vertex's own `pdfFwd`/`pdfRev` are "often exactly 1" via remap0.
  Wrong: Veach's delta-transparency convention zeroes the density of
  the vertex AFTER the null-BSDF one (`pdfFwdPrev = scatterPdf = 0`,
  set at the null-BSDF vertex's own scatter -- see §3.1's derivation),
  not this vertex's OWN `pdfFwd`/`pdfRev`, which come from the
  neighbouring (ordinary, non-null) vertices' own materials and are
  GENERICALLY NONZERO. The skip is correct regardless of what those
  two numbers evaluate to -- the strategy is zero-yield by the
  material, not by an arithmetic coincidence in the ratio -- and the
  comment now says so.
* **Topology N's p99 metric**: at the file's shared 32 spp, this
  scene's only variance source is a single binary absorb/re-emit roll,
  so its 99th percentile over a 32x32 image is one of a handful of
  discrete outcomes and the 25% p99 band was a coin flip by
  construction. Moved topology N's main (RGB) PT-vs-BDPT test onto a
  dedicated 256-spp rasterizer pair; PT and BDPT's p99 now read
  bit-identical. The pre-fix red-proof's own max metric was never
  actually red (0 vs 0.596831 is a relative difference of exactly
  1.0, at the suite's 100% `maxTol` boundary, not beyond it) --
  corrected here since §2.1's own before/after table could be misread
  as claiming all three metrics failed; only mean and p99 did.
* **MLT sanity re-labelled**: §2.3's "Bootstrap complete. Mean
  luminance = 0.0678" is a bootstrap IMPORTANCE-FUNCTION statistic
  (PSSMLT's own internal normalisation constant), not a per-pixel mean
  comparable to PT's -- it demonstrates nonzero energy reaches the
  bootstrap (contrasted with the pre-fix hard failure), not agreement
  with PT's converged 0.0608 (or, at higher sample counts, PT's own
  reported converged value which differs from this bootstrap number by
  design). Treat §2.3 as a sanity check that MLT can render this
  material at all post-fix, not a quantitative cross-integrator
  comparison.

### 7.6 Round-2 gate

Clean rebuild, zero warnings. `BDPTStrategyBalanceTest` 104/0 (was
99/0 before the P1 test/fix pair added 5 more checks),
`VCMStrategyBalanceTest` 74/0, `MISWeightsTest` 59/0,
`BDPTPhantomStrategyWeightTest` all passed (BDPT ratio 1.0100, VCM
1.0100-1.0125 across runs), `VCMEyePostPassTest` 31/0,
`VCMLightPostPassTest` 34/0, `RefractiveRadianceScalingTest` 41/0,
`VolumeAbsorptionAttenuationTest` 89/0, `PSSMLTStreamAliasingTest` all
passed, `SourceHygieneTest` 167/0, `ConnectionLegalityTest` 319/0,
`CstDeriveGoldenTest` 452 MATCH/0 DRIFT, `BDPTVertexRIGRebuildTest`
68/0, `SSSRadianceScalingTest` 574017/0,
`GenericHumanTissueInteriorScatterTest` (new, DL-184) 8/0.
