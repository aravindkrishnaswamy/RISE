# DL-48: normalize the actual SSS directional transmission law

Base master: `c270836be5bea7433149f31933f697900d9b91c3`.
Formula fix: `9e48b225`; evaluated-record follow-up: `12a7ef3e`.
Final independent review and the full selected gate govern
merge readiness; their receipts are exported with the standalone report.

The five SSS normalization sites divided Schlick transmission by
`(41-20*F0)/42`, which is not its cosine-weighted mean. Consequently the
supposedly normalized directional factor lost energy both when sampled
and when reevaluated for direct lighting or bidirectional connections.

For `mu = cos(theta)`, Schlick gives
`Ft(mu) = (1-F0)*(1-(1-mu)^5)`. The required denominator is
`c = 2*integral_0^1 Ft(mu)*mu dmu = 20*(1-F0)/21`.
Thus `Sw = Ft/(c*pi)` has unit cosine-hemisphere integral. For a
cosine-sampled continuation, `Sw*cosine/pdf = Ft/c`; the spatial weight
must continue to exclude this directional factor. No eta-square factor
or PDF was added.

The exact old normalized integrals are `64/67` at IOR 1.5 and `320/349`
at IOR 2. These are independently recomputed analytic values, not rendered
brightness measurements. The change makes the directional integral one;
it does not claim exact conservation of the complete finite SSS renderer.

## Executed red proof and repair

Test commit `54966b5f` precedes every execution. Root inspection corrected
the remaining copied legacy oracle in `61e7924a`, also before execution.
The library still matched base master. The new regression exited 1:

```text
=== DL-48 normalization failures: 37 ===
```

Those failures comprise five evaluator checks at each of five IORs,
plus diffusion and random-walk RGB/NM sampled-ratio groups at three IORs.
The independent normalized-law controls passed. The real sample groups
all met their positive-activity and finite guards; the error was the
measured full/spatial ratio, not missing sample support. Only the first
ratio mismatch per group is counted to keep the failure log readable.

The strengthened existing suite also failed on the unfixed library:

```text
  eta=1.1: integral=0.974531 expected=1.0 err=2.54692% FAIL
Assertion failed: (ok), function TestSwNormalization, file BSSRDFSamplingTest.cpp, line 384.
```

The process terminated with SIGABRT (Python return code -6). All failing
lines from both runs are included in fix commit `9e48b225` and the report.
The existing million-sample check keeps its sample count; its 0.1% bound
no longer permits the old several-percent normalization loss. The new
independent deterministic quadrature is the sharper regression oracle.

After the correction, freshly linked binaries returned zero:

```text
=== All DL-48 normalization tests passed ===
=== All BSSRDF tests passed ===
```

The new test integrates the actual shared helper, Burley-backed diffusion
adapter, and random-walk adapter in RGB and NM at IOR 1/1.1/1.3/1.5/2.
Its independent midpoint quadrature does not copy the production constant.
For the normalized cosine integrand, the second derivative is bounded by
21, so the 16K-bin midpoint error bound is below 3.3e-9; the assertion
allows 2e-8 including the 32K reference quadrature and roundoff.
Real samples at IOR 1.1/1.3/1.5 separately exercise diffusion and random-walk
full/spatial ratios in RGB and NM. Cancelling spatial throughput isolates
this directional contract from the separately open NM survival defect.

## Review follow-up: spatially varying IOR

The first fresh transport review found one P1 in the same normalization
pattern. The diffusion adapter cached its denominator from the original
hit's IOR while evaluating the numerator at the entry record. A textured
IOR could therefore give different Fresnel laws to the two terms even
after the constant was corrected. This is distinct from DL-22's missing
entry signals: explicitly supplying both records' UVs still reproduces it.

Regression `97fda542` was committed before execution against the first
repair. It uses an actual affine TextureScalarPainter over a two-by-two
in-memory raster, selects IOR 1.5 and 2 at distinct UVs, and checks both
index directions. The same-record controls pass; entry RGB/NM integrals
are `0.9259259256` and `1.08`. Actual diffusion continuation samples also
disagree with the adapter in each direction and wavelength mode. The
process exits one with:

```text
=== DL-48 normalization failures: 8 ===
```

The mathematical mismatches are `25/27` and `27/25`, independently
recomputed from the two Fresnel normal-incidence factors. Fix `12a7ef3e`
gets the denominator from the same profile/record as the numerator in
both value and valueNM. The constructor signature remains source-compatible;
its original-hit IOR argument no longer supplies a cached normalization.
Random-walk adapters already evaluate both terms from one stored index.
BDPT's shared evaluator already reads both terms from the same record.

This follow-up is part of DL-48, not a new residual. The freshly linked
expanded regression returned zero and printed
`=== All DL-48 normalization tests passed ===`. Both index directions and
RGB/NM integrals measured `0.9999999997`; all continuation comparisons
passed. Final gate and fresh review receipts in the report supersede the
first round's readiness status. The constant-only
regression did not exercise different original/entry IORs, explaining why
that first green gate could not expose the mismatch.

## Consumed-field and sibling audit

Pattern: sampled continuation and reevaluated connection weights used a
normalization integral that did not match the evaluated Fresnel law.

| Surface | Status and evidence |
| --- | --- |
| BSSRDFSampling.h, EvaluateSwWithFresnel | FIXED: uses shared inline SchlickTransmissionNormalization. Actual evaluator integral is red-proven. |
| BSSRDFSampling.cpp, SampleEntryPoint | FIXED: full RGB/NM weights use the shared denominator; spatial weights and surface/cosine PDFs are unchanged. Actual sampled ratios are red-proven. |
| RandomWalkSSS.cpp, SampleExit | FIXED: same shared denominator for full RGB/NM weights; spatial survival throughput is unchanged. Actual sampled ratios are red-proven. |
| BSSRDFEntryAdapters.h, both BSDFs | FIXED: random-walk scale uses the shared denominator; diffusion reevaluates it from the same entry record as Fresnel. Both adapters' RGB/NM integrals and diffusion textured-IOR cases are red-proven. |
| Burley and Donner-Jensen profile FresnelTransmission | VERIFIED: both concrete implementations evaluate Schlick transmission. No profile code changed. Burley has the direct adapter fixture; Donner-Jensen's multipole construction is source-audited, not a separate executed fixture. |
| PT RGB/NM | VERIFIED shared consumption: continuation reads full weights; NEE reads spatial weights and evaluates the appropriate adapter. |
| PT HWSS | VERIFIED delegated: initial and midpath SSS fall back to per-wavelength NM integration. No independent normalization site. |
| PathVertexEval RGB/NM and BDPT eye/light | VERIFIED shared reevaluation: entry vertices store spatial throughput, while continuation carries full weights. Directional evaluation calls the shared helper. |
| VCM and MLT RGB/spectral | VERIFIED shared generation/evaluation through BDPT; no additional normalization constant found. |
| Coated/composite/fabric wrappers; SMS modes | No separate modern SSS profile exposure or SSS normalization in these paths was found. No wrapper or SMS change. |

All five occurrences of the old production constant were replaced. One
shared helper lives in an existing header; no library source file was
added or removed, so no build-project file-list change is needed. Nearby
comments now distinguish Sw from its cosine-sampled importance weight.

## Validation scope and remaining limits

Per-class search selects BSSRDFSamplingTest, BSSRDFEntryPointTest,
RandomWalkSSSTest, SSSRadianceScalingTest and SobolDimensionBudgetTest.
The new BSSRDFNormalizationTest and SourceHygieneTest are included, as are
PathValueOpsTest, VCMEyePostPassTest and VCMLightPostPassTest for downstream
entry-vertex bookkeeping. BSSRDFEntryPointTest and RandomWalkSSSTest are
sampler tests, not PT renderer or direct-adapter tests.

The SSS conservative-furnace matrix is also run at IOR 2. Its unchanged
coarse bounds retain the distinct spatial-support, environment-MIS,
relative-index, and spectral-survival limitations found in DL-04. A pass
there is not a new exact-energy or independent-QMC claim. Exact directional
normalization and the coarse complete-render convention are different
assertions. DL-49 through DL-54 remain open. The independent geometric projection
density issue discovered in review is tracked below as DL-54. Final clean-build/gate and review results belong to the
standalone completion report.

Self-audit focused on the missing pi distinction, disagreement between
sampled and reevaluated weights, accidentally scaling spatial throughput,
using different IOR records for numerator and denominator, copying a
wrong constant into the oracle, and claiming NM ratio coverage
as proof of unbiased spectral survival. The independent quadrature, real
sample ratios, unchanged spatial/PDF expressions and explicit scope above
address those risks.

## Independent residual: geometric projection density (DL-54)

The second transport review found a separate spatial-density error in
BSSRDFSampling::SampleEntryPoint. The three disk-to-surface projection
cosines use the post-modifier entryNormal, although entryGeomNormal is
stored alongside it. A shading-normal modifier changes no physical
probe-hit probability, but currently changes the reported surface PDF
and both full and spatial weights. The geometric normal must define this
area Jacobian; the shading normal still defines the angular frame.

This is independent of the Schlick directional normalization and of
DL-52's skipped coplanar support. PT and BDPT RGB/NM share the sampler;
HWSS and VCM/MLT inherit those paths. Current unmodified-sphere fixtures
have matching normals, and a full/spatial ratio cancels this density, so
neither is claimed as coverage. DL-54 has a curved-object, normal-modifier
regression recipe. Evidence is static; no red test or rendered bias
measurement is claimed in DL-48.

The second test review also questioned whether missing entry UV made the
normalization correction unreachable. That finding was withdrawn after
checking the actual default record and sampler: default UV (0,0) selects
the same texel as the forward fixture's (0.1,0.1), so the cached-index
mismatch is reachable without UV propagation. Moreover, matched Schlick
`Ft/c = (21/20)*(1-(1-mu)^5)` is independent of IOR. Repairing its
normalization therefore does not require resolving the separate entry
payload question tracked by DL-22. The reverse fixture remains an
adversarial direct-adapter case; no full PT payload regression is claimed.
