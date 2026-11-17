# DL-57 random-walk density cutoff

Status: **CLOSED 2026-09-12** — source repair `c187f2cc`.

`RandomWalkSSS::SampleExit` used an absolute `pdfMixture < 1e-20` rejection.
That threshold has inverse-distance units, so it could discard a valid
collision whose normalized conditional weight is order one. The repair adds
the existing `FiniteMath` include and rejects only non-finite or non-positive
mixture densities. It changes no denominator, weight, proposal-rate,
boundary-survival, or throughput-pruning formula.

The committed-before-execution regression (`b86925e5`, `a6b6622f`,
`0226242d`) traces real closed-sphere intersections from a synthetic
south-pole entry. Every oracle uses the actual sampled distance after
inverse-CDF quantization. The unfixed run had one failure: the
large RGB collision followed by an exit was missing; the scaled RGB, NM and
pure-absorption controls passed. After `c187f2cc`, fixed output was:
`All DL-57 density cutoff tests passed`.

The large sphere (radius `1e8`, rate `1e-20`) measured real initial and
post-collision continuation ranges `199999999.99999899` and
`149995554.97086945`; its collision density was
`9.9999999999949996e-21`. RGB spatial weights were `1` in every channel and
full weights `1.0171875000000001`. The scale-paired sphere (radius `1`, rate
`1e-12`) measured ranges `1.9999989999989998` and `1.4999545497077045`, with
RGB spatial `1.0000000000000002` and full `1.0171875000000004` in every
channel. The NM control was spatial `1`, full `1.0171875000000001`.

The sampler-contract calculation is static bound evidence, not a renderer
measurement. With legal `xi` in `[0,1)`, the maximum optical distance is
`36.7368005696771`, the approximate selected-survival lower bound is
`1.1102230246251573e-16`, and the approximate three-way mixture survival lower bound is
`3.700743415417191e-17`; at effective rate `1e-20`, the corresponding
collision-density bound is `3.7007434154171905e-37`. This supports removing
the dimensional cutoff. The separate `pdfExit < 1e-20` guard remains because
an exit distance cannot exceed the sampled distance. Throughput pruning is a
different post-normalization policy and remains unchanged.

## File status

| File | Status |
|---|---|
| `src/Library/Utilities/RandomWalkSSS.cpp` | Modified by `c187f2cc`; no production `src/Library` files were added or removed. |
| `tests/RandomWalkDensityCutoffTest.cpp` | Added before first execution; real RGB/NM regression and controls. |
| `tests/README.md` | Updated test coverage and evidence. |
| `docs/DEBT_LEDGER.md` | Updated DL-57 closure and counts. |
| `docs/DL50_RANDOM_WALK_SURVIVAL.md` | Updates the later DL-57 closure cross-reference. |
| `docs/DL55_RANDOM_WALK_FALLBACK_PROPOSALS.md` | Closed the residual heading and linked evidence while preserving DL-55 historical status. |
| `docs/DL57_RANDOM_WALK_DENSITY_CUTOFF.md` | This closure record. |
| `docs/README.md` | Added index link. |

Validation selects ten individual gates (22 stages). The standalone
completion report will record the final branch and isolated merged-master
results. No new render measurement was made. Clean Xcode
source audit at `c187f2cc` had zero compiler warnings; its four notices were
three missing local OIDN search-path notices and one AppIntents metadata
notice.

This slice opens no new residual. Invalid coefficient inputs and
overflow from summing enormous rates are not asserted solved; the sampler
support bounds assume valid positive finite effective proposal rates.
