# r214 cost candidate — target temperature basis reuse

Status: isolated candidate preparation, **not accepted production code**. No
Metal qualification, whole-owner comparison, migration, or new performance
claim is issued by this increment. The sealed onset worker is unaffected.

**Historical status above is superseded by [r215](FIRE_SMOKE_TARGET_COST_R215.md):**
the bit-identical change passed its numerical, cost and review gates. The
measurements and deliberately limited authority are recorded there.

## Single-variable arithmetic change

`evaluate_resident_target_terms` now forms one per-cell compensated temperature
basis: `1/T`, `(1/T)²`, `log(T)`, and `T²` through `T⁵`. Seven species reuse
those identical inputs in their enthalpy and heat-capacity evaluations. Each
helper retains its own original segment decision; each polynomial and the
species accumulation retain their original operation order. There is no
interpolation, changed tolerance, changed policy, or arithmetic reassociation.

The prior direct enthalpy/cp helpers remain unchanged. Qualification-mode
evaluation independently compares each species' enthalpy and cp in all four
EOSDD words (`hi`, `lo`, `tail`, `bound`) plus validity. The existing function
constant removes those direct comparisons from production pipelines. The basis
is thread-local, with no new resident authority or interstage transfer. Actual
Metal register pressure, working set and performance remain to be measured.

## CPU-only evidence and its limits

Run `python3 tools/test_fire_r214_target_basis.py`. It extracts the actual
candidate Metal arithmetic and compares against immutable commit
`6f444137733ac01c3236d83a665aeef961550fe6`. The CPU adapter strips Metal address
spaces and dispatch attributes, and removes the intentionally unread absolute-
deviation parameter from both target signatures. It does not reimplement the
target's numerical body. Atomic obligation ORs execute serially in this CPU
adapter; this is not evidence about Metal scheduling or GPU compiler lowering.

For each of production/diagnostic mode, 1,792 synthetic species/temperature
pairs compare separate enthalpy and cp four-word results and class/refusal
bits. 512 synthetic single-cell target cases compare every materialized field
separately: tangent, frozen source, absolute diagnostic, monitored tail, base
target, assembled target, tangent radius and assembled radius. Results:
492 accepted, 20 refused; zero bit/class mismatches. Accepted coverage includes
123 exact-zero divergences, 369 nonzero divergences, 246 positive-deviation
tails and 123 negative-deviation tails. These are synthetic inputs, not r199's
exhaustive domain and never production outputs.

Named REDs require rejection for a wrong shared fifth power, wrong inverse
square (cp separately), changed segment obligation, changed interval-selection
conjunction, and corrupted source publication even with the assembled sum
unchanged. Midpoint probes and adjacent binary32 segment endpoints are included.
No acceptance claim pools fields or compares residuals in different units.

### r215 continuation: diagnostic-witness RED

Independent review identified that the original synthetic tests could not
detect bypassing the new qualification-only word comparator. A kernel-local
one-ulp `cp.bound` corruption now runs after standalone helper checks and must
set failure bit 32768. Weakening the comparator to `hi` alone must lose that
refusal, while a separate production-mode `cp.hi` corruption must still fail
the ordinary per-field comparison. All seven CPU tests pass. This closes a
test-integrity gap, not a numerical production defect; production arithmetic
has not changed from the candidate above.

The updated candidate builds warning-free and its live resident target/owner
gate has executed successfully while the sealed tier-10 onset was running.
Those fixture timings are contended and are excluded from performance claims.
The eight hot-step persistent-byte comparison, uncontended performance runs,
and final promotion reviews remain pending. In particular, this is still not
a promoted cost improvement.

## Pending promotion gates

1. Fresh independent code/test review, clean Metal compilation, separate-term
   resident target qualification and the reviewed r190 owner gate.
2. Eight matched hot accepted steps: bit-identical canonical persistent payloads
   against the pre-change executable, plus unchanged refusal/lineage/residency
   REDs. No existing checkpoint migration certificate is implicitly extended.
3. Three uncontended eight-step hot measurements with exact executable/build
   identities and per-kernel profiling. Report per-process variation, device
   and wall mean/p95, and whole-owner cost; do not add overlapping inclusive
   kernel intervals. Retain r213's mean target cost 2.208697 s/step as the
   baseline observation, not a promised savings.

The mean wall-minus-device residual of 1.897683 s is still unattributed; it is
not a measured host-hashing duration. Fixed-k selection and qualification are
separate semantic work. This candidate neither selects k nor changes owner
termination, active classes, r170 policy, or publication authority.
