# r217 — a shared reciprocal error, not four physical failures

The r216 continuation accepted step 1165 at 1.7873949808417819 s, then
refused the next R0 producer group with bitmap 33564800. It did not reach
the old 2.131 s crossing or the 3.5 s horizon. Last accepted max velocity
was 9.8398113250732422 m/s; no onset-survival verdict is claimed.

The retained checkpoint SHA is
`7eefb57bfd01f27817d0e245da8293c5ec85c770f0bf157556f20f906865e966`.
The checkpoint, its v2 root sidecar, complete accepted/retry trajectories,
continuation identity and terminal log are archived in
`rendered/fire_production_calibration/r217_clause_diagnostic/failure1165.v1.tar.gz`,
SHA `b3dd5ceb2f864f10e643d28838539b95462ca0cd2e1e4d24183347fd1ec425db`.
The input checkpoint was not modified by any diagnostic replay.

## First writer, rather than bitmap inference

The shared bit 7 has both FCT and EOS writers. Therefore the initial bitmap
alone cannot diagnose inversion. Reporting-only ordered copies of the device
failure word produced the same sequence on repeated step-1165 replays:

| Completed producer phase | Cumulative bitmap |
|---|---:|
| Physical flux | 0 |
| FCT candidate | 0 |
| EOS | 128 |
| Target and its consumer | 10368 |
| Stage seal | 33564800 |

There is one primary failure and three fail-closed consequences:

| Bit | Named clause in this producer group | Certificate / quantity |
|---|---|---|
| 7 | `evaluate_resident_eos_candidate`: `eos_temperature` refuses an unresolved lattice ordering | Per-cell energy separation must exceed its propagated energy enclosure, both J/m³ |
| 11 | `produce_resident_frozen_source` / `evaluate_resident_target_terms`: parent authority unavailable | Conjunction of authenticated parent seals, dimensionless identity words |
| 13 | `issue_resident_projection_consumer` / `consume_resident_target`: target authority unavailable | Immediate authenticated target and parent seals, dimensionless identity words |
| 25 | `owner_issue_stage_seal`: parent lineage cannot seal | All eight immediate authority parents must authenticate; zero means no publication |

The periodic-seam and commuting-identity aliases of bits 11/13 are not the
writers in this failing BuildStage group. FCT is explicitly cleared by the
first-writer measurement, not by assuming that absent bit 6 proves EOS.
Default-zero rejected result fields are not physical measurements.

The complete replay reads the actual terminal identity words: projection
4242672244311018523, transport 7887772084331594268, physical flux
12561591782754025130; candidate, EOS, frozen source, target, both consumers
and stage are all **0**. Thus bits 11/13/25 compare a measured missing
authority (0) with their required authenticated nonzero parent conjunction;
they are provenance refusals, not quantities measured in pascals or J/m³.
The complete raw transcript SHA is
`2a25b791ed5a4a4fff460721a2949018d7a4191ae1e3b4c8dbfd5cbdcaedc0d4`.
The frozen reciprocal-mutant executable SHA is
`2adb8006a924d8060bed456175beb704c3ba57e4aec29a4a5361d5a4f70b4eca`.
It reproduces the old arithmetic only; it has no migration authority.

## In-domain cell and independent reference

Cell 367001, `(39,53,49)`, fails at the lattice temperature
338.39862060546875 K, inside the pinned [300,2300] K range. Its signed
species densities and all nine conservative values, parent state, Q0 and
source increment are retained in the raw replay transcript. The fp64 record
accepts those exact binary32 input values and returns
338.39862061370206 K, whose binary32 projection is the same lattice value.
Its represented pressure ratio at that projected temperature is
0.9999991168940322. This is not an inadmissible physical state.

At that exact lattice temperature:

| Quantity | J/m³ |
|---|---:|
| Candidate sensible energy | 40438.5 |
| Device energy minus candidate energy | -5.321899371296365e-11 |
| Certified device separation lower bound | 5.3218991713510633e-11 |
| Device propagated enclosure | 5.4029482277062613e-11 |
| Canonical fp64 energy minus candidate energy | -8.682269253768027e-6 |
| Isolated rounded-reciprocal bias | +8.682238559391025e-6 |

The predicate overlap is genuine, but its enclosure describes the wrong
expression. Both compensated device enthalpy implementations used
`eos_mul(t5,eos_dd(0.2f))`. Binary32 0.2 is
0.20000000298023223876953125, not 1/5, yet its input bound was zero.
The direct/shared comparator repeated the same error. The unaccounted
fifth-term bias accounts for the discrepancy against the independent record.

Classification: **an EOS kernel/formulation certificate defect on an
in-domain, admissible state**, not a qualification-range extension and not
four independent physical failures. The initial adjacent-rounding-bin idea
was withheld when the independent energy comparison exposed this error.
No search bypass, rounding-bin relaxation or manifold-policy change is made.

## Remedy and sibling audit

Both compensated fifth primitives now use `eos_div(t5,eos_dd(5.0f))`.
Five is exactly represented; the existing residual-based division enclosure
certifies the operation. No tolerance or live bound is widened. Endpoint
tables, direct enthalpy, shared mixture energy and downstream target enthalpy
inherit the same correction. Half and quarter are exact binary fractions;
the cubic primitive already uses certified division by three.

The ordinary binary32 `pf_enthalpy` path is distinct: its r198 qualification
already accounts for packing, logarithm and uncancelled termwise binary32
arithmetic, including rounded reciprocals, and propagates this uncertainty
into fluxes. It is left unchanged. The audit found no other unqualified
nonexact decimal literal entering EOSDD as a zero-bound constant.

## Qualification contract, not a fitted allowance

The new diagnostic exposes each of seven species enthalpies (J/kg) and the
signed-mixture energy (J/m³) separately at 300, the offending lattice value,
1000 and 2300 K. It produces no candidate or lineage authority.

Comparison includes the unchanged device enclosure **and a separately
derived host arithmetic enclosure**. The latter is an independent outward
binary64 interval walker of the canonical expression, including signed
species accumulation. Mathematical logarithm is enclosed independently by
power-of-two normalization and the atanh series through odd degree 49 with
its analytic remainder; ln(2) is enclosed by the same rational series. The
actual host libm value must lie inside that interval. Device residuals never
enter the derivation. This replaces an incomplete exploratory comparator
that incorrectly attributed all host arithmetic error to the device.

GREEN requires the residual upper bound within the total local enclosure.
The reciprocal RED requires a residual **lower** bound exceeding it; a
possible failure or malformed payload cannot count as a demonstrated RED.
The old reciprocal is restored only in a separately compiled diagnostic
library, with both replacements counted and its distinct source SHA logged.

Named REDs cover the fifth-primitive mutant separately for species and
mixture energy; missing/corrupt phase witnesses; missing predicate/input/seal
witnesses; malformed mutant bounds; host projection error; independent log
enclosure; and an upper-only residual crossing that does not prove failure.
Diagnostic completion requires the complete typed witness conjunction.

The r199 EOS surface, exhaustive log-domain study, r190 owner gate, kernel
sweep, warning gates and fresh review receipts are recorded separately.
Exploratory logs are retained as exploratory; they are not final build seals.

## Continuation boundary

This is a numerical formulation change, not reporting-only instrumentation.
No r78 v2 equivalence is inferred from the unchanged rounded temperature of
one cell or from the historical eight-step certificate. The corrected build
must be qualified, sealed and run **from zero**, retaining the original
tier-ten seed, source policy, crossing bundle and 3.5 s-or-first-crossing
horizon. The step-1165 checkpoint and golden `1b944176…4947` remain historical
and untouched. No window or movie claim follows from this diagnosis.

## Exact-build qualification

Source commit `5cba686186ac57a7486184cfd72e4b979bdc102e` is frozen as
`executables/FireSequenceTest_5cba6861` under the r217 evidence directory,
outside the cleanable `bin/tests` directory. Executable SHA is
`91978041bb53124e45324838a082d6fb64bb0f2a6da28c68ddc7e8c186faaefa`;
build identity is
`f2ee8ec4cdf43ed7c527dce95718439888561cb70861f99c22f3f5fd4a311609`.
The tracked executable archive SHA is
`93da10407ba24959797700afd07fb4b247c1b6f7ac3bb0722f090c09d698d47c`.

All four temperatures pass independently for each quantity below. These
ratios compare the residual upper bound with that row's device plus
independently derived host-reference enclosure, in the indicated units.
There is no pooled cross-field acceptance statistic.

| Quantity | Units | Worst local ratio |
|---|---|---:|
| Species 0 enthalpy | J/kg | 0.030713995923697234 |
| Species 1 enthalpy | J/kg | 0.025397768562647461 |
| Species 2 enthalpy | J/kg | 0.029039578335242396 |
| Species 3 enthalpy | J/kg | 0.082319984282222675 |
| Species 4 enthalpy | J/kg | 0.049398906540902232 |
| Species 5 enthalpy | J/kg | 0.051057769075312932 |
| Species 6 enthalpy | J/kg | 0.051260198977755175 |
| Signed-mixture energy | J/m³ | 0.025351062476971446 |

At the offending temperature, corrected signed-mixture energy has a residual
upper bound of 3.637978807091714e-11 J/m³, device enclosure
5.402946492982785e-11 J/m³, and independent host-reference enclosure
4.540197551250459e-9 J/m³. The original live enclosure was not enlarged.
The reciprocal mutant proves separation using residual lower bounds and
fails both the species and mixture checks. Complete per-field rows and all
named REDs are in `evidence.v1/energy_gate.final.log`, SHA
`7de7be3482c887434a17e710dca787cae7880fb8dff2be19b45a6c3173155bec`.

The EOS qualification re-runs 49,512,449 log lattice/midpoint inputs and the
resident refusal surface. Log worst residual/bound remains
0.5192248117002557; per-cell T, represented pressure and deviation
binary32 projections all match. Its log SHA is
`be8ac4c9c8fdea5289b7c260e7ff5d4d443faaa7b593651d36cc7ab9b63ddfe0`.
The independent r190 owner gate and target lineage/atomicity/residency REDs
pass (log SHA
`d36f4fbe01932a878b718d5ef4703b42a48e90ca4e4e49b002cb37b3accf1c5c`).
The kernel sweep passes (SHA
`3ef41f126fb9332500fdbbfac347592ee816adaa298f05280c247c369d2a79ee`).
The full CPU FireSequence suite passes (SHA
`28272ab42999c10a48f26dbb8a1d52d3136c8cce797498d15ac8ba2135d774ee`).

Clean make, Xcode Deployment and shipping Opto builds pass. Make is
warning-free; Xcode has only the guide's documented absent local OIDN
search-path and AppIntents metadata exceptions. The clean target removes
`bin/tests`, including frozen copies; archived r211/r212/r213/r214/r216/r217
workers were restored, and the critical r214/r216/r217 hashes rechecked.
Golden and retained-checkpoint bytes are unchanged. New frozen workers live
outside that clean target, with a tracked archive.

Fresh mathematical and test-integrity reviews have no remaining P1/P2 after
the ledger's corrections and final what's-left round. See
`evidence.v1/review_receipt.v1.md`. The eight-step repaired-state
counterfactual and production launch are separate from these completed
qualification gates.

## Repaired retained-state counterfactual

The pre-registered `repaired_step_protocol.v1` requires exactly accepted
steps 1166–1173, native solver success and the unchanged input checkpoint;
it explicitly prohibits a migration claim. All eight steps passed, ending
at 1.7975185813265853 s with peak velocity 10.153646469116211 m/s. All
rows carry corrected kernel SHA
`e938373ec6cd9e0217c04242ad96cd4d46055236e422bd7ad0ea34e164d0a8b2`
and full input/publication v2 roots. The adapter exits 93 as pre-registered:
the original-refusal assertion no longer reproduces, while the native
solver reports success. This is not a failed production run or an r78
equivalence certificate.

Transcript SHA:
`4bb512c20cc09c3dc53d87d10d4dde4de418854115e107c129ffc66d75e1e84b`.
Eight-row trajectory SHA:
`d4596fa5d957365f577bdd40a593c6beec4598f3a15ec0a83570786e5d64cdea`.
Checkpoint SHA remains `7eefb57b…5e966`. No onset-horizon verdict follows
from these eight steps. The sealed production restart is from zero under
the unchanged r214 observation protocol, not from step 1165.
