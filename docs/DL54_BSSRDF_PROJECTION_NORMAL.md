# ~~DL-54: BSSRDF geometric projection density~~

CLOSED 2026-09-12 — `ab85092d`, BSSRDFProjectionNormalTest:
`All DL-54 projection-normal tests passed` (unfixed: 960 failures).

## Mechanism and repair

The shared disk-projection sampler casts rays against physical geometry,
then converts disk-area density to surface-area density. It previously
used the modified entry shading normal in all three projection cosines.
A normal map could therefore alter the PDF and inverse-PDF weights even
though the probe-hit distribution had not changed.

All three projection factors now use entryGeomNormal. The exit proposal
axes, projected radii, axis/channel probabilities and hit selection remain
the sampled proposal. The entry shading normal and ONB still control angular
continuation and Sw. No angular normalization, eta convention, ray offset,
probe support or sampling threshold changes are included.

## Red-proof and independent checks

Terra committed the new regression as `bef908da`, followed by `7507325a`
for fixture ownership, finite checks, ordered Get2D draws, axis activity
and precise scope wording. Before the first execution, root committed
`5fc79ad9` to qualify the shared epsilon and guard draw indexing. The
library remained unchanged from base
`8157f2fc6d57702592459b6341f7c36b3e9df6b1` during the red run.

The fixture uses a fixed synthetic exit record, real SphereGeometry entry
intersections, a real NormalMap modifier and a Burley-backed material.
Paired local RNG streams use seeds 5400 through 5527. RGB and NM 550 each
have 48 valid paired samples out of 128 attempts. Valid samples consume
seven matching draws; rejected pairs must also consume matching sequences.
The three selected-axis activity counts are 33, 10 and 5 in each case.

The test removes the shading-normal epsilon offset from returned positions
before comparing physical points. It retraces the selected probe chain and
computes a geometric disk-to-area PDF without reading the reported PDF.
This oracle shares the production geometry and radial-profile APIs; it is
not an independent ray-intersection implementation. The separate invariance
check compares bare and modified geometry under identical sampling.

Before repair there were 960 failures: 96 each for the modified PDF versus
the geometric oracle and PDF invariance, 288 each for RGB spatial and full
weights, and 96 each for NM spatial and full weights. All activity, geometric
point/normal, finite and angular-frame controls passed. The complete red
output is retained in the fix commit body and external evidence.

No test code changed in source repair `ab85092d`. After repair:

```text
Test RGB: curved-sphere projection PDF ignores normal-map tilt
  active=48/128 matched=48 tilted-normals=48 rotated-directions=48 axes=33/10/5
Test NM 550nm: curved-sphere projection PDF ignores normal-map tilt
  active=48/128 matched=48 tilted-normals=48 rotated-directions=48 axes=33/10/5
=== All DL-54 projection-normal tests passed ===
```

Full-weight equality is specific to paired identical local cosine samples.
It does not assert that Sw is invariant for a fixed world-space direction
when the shading frame rotates. The fixture establishes this geometric
projection contract, not a rendered-image bias magnitude or confidence interval.

## Sibling and consumer audit

The single helper computes the shared PDF and both RGB/NM full/spatial
weights. PT's Pel/NM loop consumes those fields directly and carries both
normals into entry records. HWSS delegates SSS to NM paths. BDPT eye/light
RGB/NM generators consume the weights and forward area PDF; VCM and MLT
reuse those generators. No consumer duplicates the projection Jacobian.
These integrator statements are source-audit scope, not separate integration
renders of the new normal-map fixture.

BSSRDFEntryBSDF and PathVertexEval evaluate angular Sw and retain the shading
frame. RandomWalkSSS shares SampleResult and normalization helpers but has
volume free-flight sampling and pdfSurface=0, with no disk projection.
Coated/Fabric supported substrate lists exclude SSS; CompositeMaterial does
not forward a diffusion profile. No sibling projection site remains.

DL-52's coplanar probe omission and DL-49/DL-55/DL-56 remain distinct open
work. This slice found no new residual. The ledger has 55 main rows: 46 open
and 9 closed by this cleanup.

## File status and validation scope

| File | Status |
|---|---|
| `src/Library/Utilities/BSSRDFSampling.cpp` | Modified: geometric normals for all three area Jacobians (`ab85092d`). |
| `src/Library/Utilities/BSSRDFSampling.h` | Modified: clarify geometric versus shading normal roles and current PT consumer. |
| `tests/BSSRDFProjectionNormalTest.cpp` | Added: committed-before-execution geometry/modifier regression. |
| `tests/README.md` | Modified: test scope and observed red/fixed results. |
| `docs/DEBT_LEDGER.md` | Modified: closure and counts. |
| `docs/DL48_SSS_NORMALIZATION.md` | Modified: close the discovery entry and update follow-up status. |
| `docs/DL54_BSSRDF_PROJECTION_NORMAL.md` | Added: mechanism, evidence and coverage limits. |

No library source file was added or removed, so build-project source lists
need no changes. Selected gates are BSSRDFProjectionNormalTest,
BSSRDFSamplingTest, BSSRDFEntryPointTest, BSSRDFNormalizationTest,
RandomWalkSurvivalTest, RandomWalkSSSTest, SobolDimensionBudgetTest,
SSSRadianceScalingTest and SourceHygieneTest. The external completion report
records their exact counters, clean make/Xcode results, independent review
rounds and merged-master validation.
