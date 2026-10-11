# DL-475 design: support-safe automatic routing

Written before implementation, against master da81a949e.

The pinned physical path is C-S-M-S-L (or additional delta S vertices),
where L is a delta-position light and M is its only connectible scatter.
With conventional BDPT s/t counting the light/camera endpoints, its five
vertices admit splits (s,t)=(0,5),(1,4),(2,3),(3,2),(4,1).
The first cannot hit a geometric point emitter; every other connection ends
at a delta surface. Extending the eye prefix C-S-M with ordinary s=1 NEE
would connect M directly to L, skipping the intervening S, and visibility
rejects it. A generalized NEE strategy would have to insert that S and
sample/price its delta constraints; it is not a missing weight on an
existing MISWeight strategy. The symmetric light-prefix camera connection
has the same obstruction. M-M is a different, two-scatter physical path.
Batch9's max_volume_bounce=1 pin and PT/VCM measurements witness this gap.

Decision: use the user-authorized routing alternative. Before Tier2 probes,
automatic selection chooses VCM when a world-visible transmissive object
has an explicit interior medium and the scene has a positional delta light.
This is a conservative superset, not a topology/IOR or scattering census:
absorbing-only media and mixed boundary materials can over-route. VCM can
merge M endpoints and sample this class; PT can reach the index-matched
fixture only with transparent shadows, which auto does not expose here.
A once-per-resolution warning names DL-475 and explains the support reason.
The shared selector covers RGB and spectral auto dispatch. Explicit author
pins remain honored, and manual BDPT retains the strict failing energy pin.
No BDPT/MIS/contact implementation is changed. Nested medium bindings not
exposed by the root object's GetInteriorMedium remain a residual census
limitation; there is no all-scenes completeness claim.

Gates planned: a salted auto probe-enabled fog-box regression, checking VCM
resolution, skipped probes, documented reason and energy against independent
PT and VCM; negative/pinned controls; BDPTStrategyBalanceTest,
VolumeEnvFurnaceTest, MediumInsideOutsideInvariantTest and AutoRasterizerTest.
The strict manual-BDPT test stays red. No timing claim.

## Measured routing slice

`AutoMediumDeltaRoutingTest` uses the production selector with probe enabled,
activation threshold 1 and a deliberately loose caustic threshold, one thread,
reserve0, max_volume_bounce1, eight ValueSalts 47500..47507. Its master route
was PT and black (the auto path does not expose the reference's transparent
shadows); this also demonstrates why a cost/variance probe is not support proof.

| n8 quantity | master mean / sample SD | corrected mean / sample SD |
|---|---:|---:|
| auto | 0 / 0 | .0630409049004 / .00178505825706 |
| independent PT | .0635735855148 / .0000714297210758 | .0635672013146 / .0000594373375733 |
| independent VCM | .0628018580356 / .00291853803577 | .0624254283076 / .00300953889581 |

Red25/34 rc1 becomes green59/0 rc0, including VCM resolution, zero probes,
reason, positive energy and paired three-SE agreement against both references.
Explicit BDPT pins remain honored and the strict manual energy pin remains
rc1. An unbound-medium control keeps its existing Tier1 route. The shared
production selector covers spectral routing too; the new energy witness is RGB.
No timing claim or new BDPT transport/MIS strategy is made.

Self-review: the predicate reads capabilities and bound media, never a dummy
shading record; stack callbacks retain no scene pointers. Routing happens only
after explicit-author-pin handling. The deliberate superset can over-route
mixed/absorption-only boundaries; nested medium bindings outside the census
and the manual BDPT/two-scatter residuals remain open. No new library files,
owned allocation, stack convention or contact change.

Named gate results (actual rc0, no compiler warnings): AutoMediumDeltaRouting59/0,
BDPTStrategyBalance370/0, VolumeEnvFurnace32/0, MediumInsideOutsideInvariant52/0,
AutoRasterizer157/0. The manual BDPT strict pin is the intentional rc1 residual.
Master987b43824 was integrated before the pre-merge rebuild of all six named
tests; that rebuild returned0. The routing row is rerun before local merge.

Fresh independent correctness and evidence reviews of5f2aa481f found zero P1s. P2 coverage remaining: add a guard-specific spectral-routing assertion and direct hidden/opaque/no-positional-light controls. The shared selector and existing spectral delegation gates cover the code path, but the new red/green render witness is RGB only. These are disclosed coverage gaps, not claims of a completed transport strategy.

Integration update: masterdfb777341 was merged after the Phase2 scene/tool/docs migration and supervisor DL571 filing. No Library C++ or related test implementation changed. Both ledger recount notes are retained; recomputed338 rows:24 open,314 closed. The six named targets are rebuilt and the routing witness rerun before local integration; no future rerun success is asserted.
