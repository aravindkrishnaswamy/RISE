# DL-126 — BDPT/VCM/MLT terminated at any null-BSDF material

Status: **CLOSED 2026-09-18** (debt-dl126 slice, branch `debt-dl126`).
Red-proofs `tests/BDPTStrategyBalanceTest.cpp` topology N and
`tests/VCMStrategyBalanceTest.cpp` topology J (both new), plus a direct
`mlt_rasterizer` bootstrap sanity render.

Related: DL-69 (whose sibling audit opened this row; its own doc's
account of *which* gate killed the walk was itself wrong -- corrected
below and in `docs/DL69_BDPT_LOBE_THROUGHPUT.md`), DL-125 (the HWSS
companion fallback pattern this fix's HWSS branch deliberately mirrors),
DL-131 (a distinct, unrelated `GenericHumanTissueSPF` in-medium bug,
explicitly NOT touched here).

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
