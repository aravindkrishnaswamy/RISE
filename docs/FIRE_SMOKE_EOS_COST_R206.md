# r206 — EOS cost: reject the ineffective warm start, reuse exact endpoints

Status: exact-commit qualification, the full suite, and measured repeats pass;
fresh review is pending. No focusing verdict, fixed-k choice, window,
new animation, or hour-class projection is claimed.

## Ordered experiment and decision

The owner authorized warm-started certified inversion first, then tabulated
thermo if needed. The prototype starts from the resident transport parent's
temperature. A binary32 heat-capacity estimate proposes Newton hints; only the
existing compensated-energy predicates may move the bracket. The final
adjacent-float midpoint test and ties-to-even rule are unchanged. An ambiguous
hint falls back to the cold inverter. This is not a proof that every possible
starting guess has the same refusal set: the measured prototype is rejected
on cost, not adopted as a universally equivalent solver.

Across the three-step cold prefix, the diagnostic reproduces the actual private
candidate's inversion on every accepted producer call (42 calls, 504,666 cells
each). Of 21,195,972 cell evaluations, 99.9801849% return at Tmin=300 K without
bisection. Exactly 100 cells per call require 24–25 bisections. Cold bisections
total 103,860; including each interior root's final midpoint gives 108,060
energy probes. Warm search plus fallback uses 93,284 interior probes; 3,466 of
4,200 interior roots take the cold fallback. Both paths have zero temperature
bit mismatches against the published field in this diagnostic.

The same executable, with the extra diagnostic dispatch disabled, was compared
with the preserved r205 qualified executable in three interleaved independent
processes per variant, three steps per process:

| EOS inclusive device interval, per call | Mean of process means | Sample standard deviation |
|---|---:|---:|
| r205 baseline | 594.714 ms | 29.378 ms |
| Warm prototype | 596.282 ms | 24.906 ms |

There is no significant improvement. The live r190 owner gate passes, and the
listed cold-prefix physics columns agree exactly, but neither is a reason to
adopt an ineffective search path. The warm implementation is retained only in
the evidence patch; production keeps the original cold root search. A read-only
cold-iteration diagnostic remains available for later front/crossing studies.
The measured population is the first 4.939 ms from zero, **not** a hot-plume
iteration distribution or the complete tier-8 replay.

The warm measurement artifact is
`rendered/fire_production_calibration/r206_eos/warm_measurement.v2.json`, SHA
`27212e469259999198337bb61b24bf8876c5398c24b82e924ddd208ea381e96d`.
It binds the raw logs and publication sidecars. Its source patch is SHA
`181ad8eac85d4a86d1089ea5af107a3b3864cda76c5703a8078eaa4b0f910cee`, based on
signed-off r205 `a660cbc5`. The prototype executable was SHA
`38095fe2a64eaba5e6e671e68cbf6c212f274a09fb4edfcbe3880563d86ef953`;
the preserved baseline executable was SHA
`96fdc911d7d3fd59f4604f320052e3e658199fa7ff45caa605b054d9cfe77f16`
(r205 `qualification.round6.v1.json`, source `904c57b8`). The warm experiment
was a dirty-tree prototype, not an exact-commit production qualification.

## Exact endpoint table, not approximate tabulated inversion

Before introducing interpolation error, the profile exposes case-constant
work: admissibility and inversion repeatedly evaluate the same seven species'
enthalpies at Tmin and Tmax. These 14 values depend only on the sealed thermo
record and EOS metadata, not on the cell or its root-search history.

`build_eos_endpoint_table` now computes those values with the existing
compensated evaluator, on device, in the same command immediately before EOS
evaluation. Each table entry retains all four binary32 words: center, low part,
tail, and enclosure. Each cell performs exactly the original density products,
sum ordering, termwise scale construction, endpoint predicates, interior
bisection, and final rounding. The optimization replaces only repeated
enthalpy evaluations by loads of their identical results. No interpolation,
new epsilon, tolerance change, or subdominance allowance is introduced.

This is a stronger, bit-identity subcase of the authorized table route, not a
claim to have qualified an approximate production thermo model. Such a model,
if later needed, still requires its separate subdominance qualification.

## Authority, arithmetic gates, and working set

- The table is allocated privately inside the authenticated EOS encoder; no
  caller can supply it. Its producer consumes the exact private thermo and EOS
  parameter buffers already bound to the immediate candidate. The qualified
  kernel-set seal binds the new producer and consumer. No CPU substitution,
  host recomputation, interstage upload, or intermediate payload hash is added.
- Qualification compares every endpoint word to direct device evaluation,
  then compares each cell's admitted temperature to direct cold inversion.
  The r190 fp64 owner remains the independent owner reference; the endpoint
  comparator does not replace it. The r199 per-cell T/represented-pressure/
  deviation comparisons and refusal battery remain in force.
- The named `endpoint_enclosure_bit_mutation` RED changes one enclosure word
  on device and refuses with bitmap 0x80 and no candidate/EOS publication.
  The existing unsealed/mismatched parent, CPU substitution, precision-class,
  conservation, and residency REDs remain binding.
- The r199 exhaustive log study still covers 49,512,449 temperature inputs:
  24,756,225 binary32 lattice values plus 24,756,224 midpoints over [300,2300] K.
  This is **not** 49.5 million nine-component mixture states. The original
  compensated log and enthalpy arithmetic are unchanged.
- The new allocation is 224 logical bytes per candidate. Both the fixture
  certificate and live EOS increment add one 16-KiB allocation quantum. The
  complete owner reserves four live sets through its existing four-target
  formula, adding 64 KiB. Actual allocation sizes are still measured separately.
- Kernel-set identity changes regenerate case identity. The next verdict run
  remains from zero; no r78 resume or alteration of the golden checkpoint is
  claimed by these short cost probes.

## Exploratory endpoint result and remaining budget

Three independent prefixes give EOS 64.646 ± 0.101 ms/call (process means),
about 9.2× faster than the interleaved r205 baseline. Whole-owner device p95 is
3,111.655 ms and wall p95 4,587.008 ms across nine cold steps. These preliminary
runs preceded final source cleanup/commit attestation; final qualification and
timings must be recorded separately before adoption. The per-step means still
contain approximately 14 EOS calls, so “tens of ms per call” does **not** mean
tens of ms for the complete owner. The 300–500 ms whole-owner target is unmet.

Target assembly is now the largest measured kernel (~114 ms/call). It is the
next optimization stage after this EOS increment is qualified/reviewed, then
the remaining non-producer work. Host hashing remains deferred as ruled: its
85% sampled CPU share is not an 85% wall-time claim. The from-zero focusing
replay, convergence exporter, k selection, and tier-10 queue remain pending.

## Self-audit / review focus

1. Endpoint values or rounding order changed by storage/reuse: per-word and
   per-cell direct comparison, independent r190 gate, r199 boundary refusals.
2. Table detached from sealed thermo/metadata: only internal private allocation,
   same immediate command and authenticated parent buffers; lineage REDs.
3. Qualification arithmetic accidentally charged to production: comparison is
   qualification-only; separate production-mode counters and seal-equivalence.
4. Omitted allocation/caller: fixture, live-increment, and full-owner formulas
   updated; no new library source files or build registrations.
5. Cost/provenance overclaim: diagnostic timing excluded, three process repeats,
   explicit cold-prefix scope, source/executable/log bindings, no window claim.

## Exact-commit qualification and measurement

Source `20607aea6da099f0083bcdbcbd6f62b3ac5149a2` was forcibly rebuilt with
the r205 clean-input qualifier. Its publication and r190 owner gates pass;
the build is warning-free. Qualification SHA:
`78e4c9f7ee441052d934de30fe6dda058640392da4733565b8db050f88718cb5`.
Executable SHA:
`7bbc7e924b1f35ceff03cc41f610652cb86477133f0bcf5b754c2e36ac5b5eb0`.
The exact executable also passes the full FireSequenceTest suite. The final
iteration probe again compares 21,195,972 cell temperatures with zero bit
mismatches; its extra dispatches are excluded from the timings below.

Three independent, unprobed prefixes (nine accepted cold steps) give:

| Quantity | Result |
|---|---:|
| EOS device ms/call, mean of process means ± sample SD | 64.590 ± 0.082 |
| EOS inclusive device ms/step, mean | 904.253 |
| Whole-owner device p95, ms | 3,243.812 |
| Whole-owner wall p95, ms | 4,736.904 |

Thus EOS is 9.21× faster than the measured r205 baseline per call; the
complete step remains well above 300–500 ms. The diagnostic prefix columns
agree exactly, but that comparison is not substituted for the per-cell owner
gate or claimed as an r78 resume certificate. Timing source/executable and
publication identities are checked by `tools/analyze_fire_eos_warm_start.py`.
Its `measurement.qualified.v2.json` artifact has SHA
`a270c53cb5e8188a7507dfbdb84f0fb9ec4864c5d76e548553aaaa3ac6a336c7`.
The final cold-iteration log SHA is
`04ff728ee827e16a668673a3b1c326277856db3d0e8b1708d7c7508f40b5efd1`.
The golden checkpoint was re-hashed unchanged as
`1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947`.
