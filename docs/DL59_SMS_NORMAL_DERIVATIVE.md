# DL-59: SMS unequal-index normal derivative

CLOSED 2026-09-12 — `a0c4a808`.

The test-only SMS normal derivative interpreted eta differently from the
specular direction it differentiates. The direction uses incoming/transmitted
ratio `1/eta` on entry and `eta` on exit, with the normal oriented toward the
incoming endpoint. The derivative used eta on both sides and missed the
exiting normal sign. At normal incidence with eta 1.5, its tangential entry
coefficient was +0.5 instead of -1/3; its exit coefficient was -2.5 instead of
-0.5.

## Repair and scope

Let `s` be the sign of `wi·N`, `c=abs(wi·N)`, and `r` the side-dependent
ratio. The raw direction is `-r*wi + (r*c-cosT)*s*N`. Its derivative with
respect to the original normal is

`J_N = r*(1-r*c/cosT)*N*wi^T + s*(r*c-cosT)*I`.

The repair uses that ratio and sign, with the shared stable transmitted-cosine
helper. The two orientation signs cancel in the outer-product term; one
remains on the identity term. Matched-index zero and reflection are retained,
as is the existing finite critical-angle regularization. The direction
function classifies TIR before the Jacobian caller uses this derivative; no
physical derivative at a non-differentiable TIR boundary is asserted.

`ComputeSpecularDirectionDerivativeWrtWi` already uses the correct ratio and
oriented normal. Its matched-index negative-identity guard remains covered by
DL-58. The only normal-derivative caller is `BuildJacobianAngleDiff`; the
angle-difference family is test-only. Production `Solve` uses the half-vector
Jacobian. The helper comment now identifies its actual test-only caller.
No production rendering improvement or nested-dielectric angle-difference
support is claimed.

## Committed red-proof

`fcbc69da` committed `ManifoldNormalDerivativeTest` before its first execution
against the unfixed library. It reported `Checks: 141 Failures: 12`: eight
entering/exiting normal/oblique tangent-derivative comparisons and four curved
Jacobian entries failed. Matched-index, reflection, physical transmission/TIR,
activity and closed-form direction finite-difference controls passed.

The same test source reports `Checks: 141 Failures: 0` after `a0c4a808`.
It compares the derivative of normalized output, applying
`(I-wo*wo^T)/|rawWo|` to the raw analytical derivative. Finite differences use
tangent perturbations of unit normals, avoiding the raw-versus-normalized
mismatch in the older disabled FD tests. The curved cases are synthetic
non-pole, one-vertex chains with supplied normal derivatives; they call the
actual analytical and numerical angle-difference builders. They are not
rendered or traced curved surfaces.

The final run's maximum vector discrepancy across 14 direct FD
comparisons is `1.0441641706224828e-10`; the maximum entry discrepancy across
8 curved matrix entries is `2.3757129596901905e-10`. These deterministic
numerical checks are not confidence intervals. No test source changed between
the committed red run and this fixed run.

## Validation and status

Compiled source: `a0c4a808b26cf4df2ac5e6fe3f57990a095fd90f`. The branch passed five selected gates across
12 distinct stages: clean Make, Make build, five individual links, and five
serial test executions. A source-hygiene retry was additionally required:
its first scan found a copied skill inside the owned Xcode app bundle. After
the successful Xcode build, that generated directory was removed and the
unchanged hygiene binary passed. The failed scan and retry are both retained.
No source change or test relaxation was used for that retry.

Clean Xcode passed with zero compiler diagnostics. Four environment notices
were retained: three missing local OIDN search paths and one AppIntents
metadata notice. Final selected gate summaries:

- `ManifoldNormalDerivativeTest`: `Checks: 141 Failures: 0`.
- `ManifoldSolverTest`: successful exit; its full output is retained.
- `GrazingSnellFresnelTest`: `Checks: 38  Failures: 0`.
- `GrazingFresnelThroughputTest`: `Checks: 44  Failures: 0`.
- `SourceHygieneTest`: `(scanned 317 test files) 165 passed, 0 failed.`

Documentation-only closure edits within this DL-59 pass retain the compiled
source, executable tests, scenes and build inputs. Independent review and
integration evidence belong in the standalone completion report.

| File | Status |
|---|---|
| `src/Library/Utilities/ManifoldSolver.cpp` | Modified by `a0c4a808`: ratio/sign derivative and caller comment. |
| `tests/ManifoldNormalDerivativeTest.cpp` | Added by `fcbc69da`, committed before execution. |
| `tests/README.md` | Added regression scope and measured counters. |
| `docs/DEBT_LEDGER.md` | Closed DL-59, recipe and counts. |
| `docs/DL58_MATCHED_INDEX_GRAZING.md` | Closed its DL-59 residual; historical DL-58 integration clarified. |
| `docs/DL59_SMS_NORMAL_DERIVATIVE.md` | This closure record. |
| `docs/README.md` | Added closure index link. |

No Library source files were added or removed, so the five explicit build
source lists require no change. No new residual was found. At this closure,
the ledger has 58 main rows: 44 open and 14 closed by cleanup.
