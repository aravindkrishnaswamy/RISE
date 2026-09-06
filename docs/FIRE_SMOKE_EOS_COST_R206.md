# r206 — EOS cost: reject the ineffective warm start, reuse exact endpoints

Status: exact-commit qualification, the full suite, and measured repeats pass.
Review findings and closure are recorded in the adjacent r206 review ledger;
they are separate from the numerical source/executable attestation below.
No focusing verdict, fixed-k choice, window,
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

## First review and evidence-gate repairs

The numerical/lineage reviewer found no P1/P2. The independent evidence
reviewer reproduced both measurement JSONs and found three P2 parser gaps:
absent diagnostic verdict counters defaulted to zero; repeated copies of one
log could be counted as independent processes; and the inherited owner-log
gate checked 41 distinct labels without requiring the actual field names and
extents. The saved evidence itself was complete and distinct.

All three are repaired. Diagnostic schemas now require their counters exactly;
repeats pass the existing distinct-path/distinct-log guard; and the owner gate
requires its canonical field set, cell/face extents derived from the pinned
4-cubed fixture, and residual lengths from the recorded owner iteration counts.
The sibling verdict checks now require exact singleton success records rather
than success-looking substrings. Named REDs delete required counters, copy
process evidence, repeat resolved paths, and rename/drop/shorten/zero every
owner field. `tools/test_fire_r206_eos.py` and all eleven r205 CPU regressions
pass. Reanalysis still produces the identical measurement SHA above. These
repairs touch evidence tooling only; numerical source remains `20607aea`.

Clean Xcode Deployment and Opto builds both pass. The only warnings are the
documented absent `extlib/oidn/install/lib` search path and AppIntents metadata
extraction; no source compiler warning is discounted. Logs are retained in
the r206 evidence directory. Fresh post-fix review remains pending.

## Second evidence review

The fresh reviewer again reproduced both measurements byte-for-byte and found
no P1, but two further P2 false-green paths: a passing owner verdict could
coexist with a failed verdict of the same tag, and required histogram metadata
could carry impossible populations or temperature bounds. Both are repaired:
each verdict tag has exactly one successful record; histogram counters are
bounded by their populations, lower-endpoint counts equal the zero-bisection
bin, and the diagnostic domain is exactly the pinned case's 300..2300 K.
Duplicate schedule counters and unknown sealing records also fail closed.

Named in-memory REDs append contradictory owner/publication verdicts, corrupt
population counts, and replace domain endpoints with nonfinite, reversed, or
wrong-case values. The new tests fail against the previous committed parsers
(`review.round2.redproof.v1.log`) and pass against the repairs. Existing raw
logs and measurement values are unchanged; numerical source is still
`20607aea`. Fresh final review is pending.

## Third evidence review: shared counter parsing

The reviewer found no P1 and one P2: completion records still admitted
duplicate `complete` or `outcome_sha256` tokens via last-value-wins parsing.
The repair covers the family: the EOS analyzer, owner-cost binder, and
historical producer-prefix consumer use the same strict parser for terminal
and producer records. Missing, unknown, malformed, or repeated counters are
refused before normalization. JSON owner-profile records likewise reject
duplicate keys and require the emitted schema. Completion additionally binds
step count and end time to the trajectory and requires an empty error.

The historical parsers accept the contradictory terminal while both repaired
paths refuse it (`review.round3.redproof.v1.log`). The CPU battery mutates every
field in completion, producer-command, kernel, and profile records; all existing
r205 regressions and profile self-tests pass. Reanalysis still gives the same
`a270c53c…` measurement SHA. This is an evidence-only repair, not numerical
code or a changed acceptance tolerance.

## Fourth evidence review: legacy consumers

Two further P2 bypasses, no P1, were found in older consumers: the standalone
cost-prefix CLI checked hash presence without requiring successful completion,
and the instrumentation qualifier allowed conflicting FP64/convergence verdict
records. Completion validation is now centralized in the cost-prefix module
and used by its CLI, the current binder/EOS analyzer, and the historical
producer-prefix consumer. Instrumentation qualification requires singleton
successful verdicts. The shared metadata reader also rejects duplicate keys.

REDs exercise the actual CLI and the qualification helper against their old
versions (`review.round4.redproof.v1.log`). Synthetic parser-only inputs in the
helper test are isolated from production evidence. The saved cost results
remain byte-identical; numerical source and all tolerances are unchanged.

## Fifth review and proof correction

New-agent creation was unavailable. Two existing reviewers who had not reviewed
r206 independently examined its numerical/lineage and evidence/cost axes. The
numerical reviewer found no P1/P2. The evidence reviewer found one P2 in the
remaining artifact/named-RED records: duplicate counters could still pass the
old suffix checks. Both families now require unique counters and exact schemas,
including the unquoted diagnostic error field. Every field is mutated through
the actual qualification helper.

Self-audit also found that the round-4 historical helper RED log failed during
synthetic fixture setup, not on the intended mutant: the old parser required a
space following `passed=1`. The fixture now includes a second field, and the
replacement `review.round5.redproof.v1.log` requires assertion failures with
zero setup errors against both historical helpers. The original log is retained
but is superseded for that RED claim. Numerical gates and measurements were
never dependent on this parser-only fixture; their SHA remains unchanged.

## Sixth review: whitespace consistency

Fresh-agent capacity returned and two new reviewers ran in parallel. Numerical
and lineage review again found no P1/P2. Evidence review found one P2: the
unquoted-error parser recognized literal spaces but could swallow a tab before
a contradictory counter. Counter boundaries now use the same whitespace class
as the token parsers. Space, tab, nonbreaking-space, vertical-tab, form-feed,
and repeated-space mutants exercise duplicate and unknown counters inside
diagnostic prose. The historical parser fails the new assertions with zero
setup errors (`review.round6.redproof.v1.log`). No numerical result, tolerance,
or measured timing changed.

## Seventh review and exact-source EOS closure

Two P2 findings, no P1: whitespace-aware field parsing was still preceded by
raw-prefix dispatch in histogram/producer/profile readers, allowing indented
duplicate records to be ignored; and the standalone EOS refusal/mutation
battery had only a preliminary execution with a different full Metal source
identity. The default suite and owner gate did not replace that EOS fixture.

Known record tags are now recognized consistently before schema checks. REDs
append whitespace-prefixed duplicates through the actual histogram, producer,
and owner-cost consumers; their historical versions fail the assertions with
zero setup errors (`review.round7.redproof.v1.log`).

The standalone EOS battery was executed again, exit zero, using the unchanged
attested executable `7bbc7e92…`. Its final-source log SHA is
`8c4588b5d843deb73240853b41eddac3d4e55a9be1900191b37cd8b1ca62ecf9`.
It reports all 49,512,449 log-domain inputs, worst residual/bound
0.5192248117002557, and the endpoint mutation's exact 0x80 atomic refusal.
Metal library source SHA is
`4a43e205d81b5855b9366353df25b8fa2f511f9d6f31c075c7d77f0fbb283ab1`.
The execution supplement `qualification.eos.v2.json` binds the command,
exit code, unchanged before/after executable hashes, parent qualification,
source commit, and log. Its SHA is
`b627b6292f8626e0ec1ce974f9a6c38fabc5c140f982a6c57a72bf96f82fb798`.

Qualified analysis now **requires** this supplement, including the named
15-RED battery, endpoint mutation's exact refusal/publication counters, and
full domain study. Missing or mismatched execution identity, an omitted/wrong
mutation, a renamed RED, or a shortened domain fails closed. The mandatory
dependency is tested through the complete analyzer as well as its helper.

`measurement.qualified.v3.json` supersedes v2 for complete EOS qualification;
its SHA is `f889b9148415927d9ef7ac1304c819f6d4177d5a0b39da032e9484cd03774569`.
Only the added qualification binding changes the measurement artifact: all
timing and histogram values are unchanged. Historical logs/artifacts remain
unmodified. The 300–500 ms whole-owner target remains unmet.

## Eighth review: execution authority and disabled-observer admission

No P1; two P2 families. The new EOS reader could admit contradictory detailed
counters if a caller rewrote both the log and its declared hash; domain record
and lattice/midpoint split were incomplete checks. The off/on instrumentation
CLI also still searched for an ASCII-space tag while its reader accepted tabs.

The r206 analyzer now pins the **recorded execution supplement's byte SHA** as
an authority root, just as it pins the baseline/warm executable identities.
This is verification of this one executed r206 campaign, not an issuer for
arbitrary future EOS runs. Rewriting a log and recomputing its own declaration
cannot create execution authority. A future execution requires a new recorded
qualification and its own campaign binding, never automatic trust in a
self-declared hash. Bytes are read once before parsing to preserve exact
payload binding. Domain endpoints/scope, lattice/midpoint counts, runtime
consistency, core bit-comparison verdicts, and every named atomic refusal's
bitmap/execution/publication counters are additionally checked explicitly.

Mutants alter every named refusal field, domain coverage, and detailed verdicts
with recomputed declarations, through both helper and complete analysis. The
disabled-observer RED exercises its actual CLI with whitespace-separated
profiles in off-mode. Historical implementations fail those assertions with
zero setup errors (`review.round8.redproof.v1.log`). The actual v3 measurement
and all numerical code remain unchanged.

## Ninth review: independent executions, not different log bytes

Numerical/fidelity review found no P1/P2. Evidence review found one P2:
copies of one run, with cosmetic whitespace/comment changes to their logs,
could count as three executions. Distinct paths and raw SHA inequality alone
were insufficient. The generic repeat guard now requires distinct recorded
CPU/GPU command-clock sequences, ignoring cosmetic log content. The fixed r206
analyzer additionally pins all nine executed timing-log roots; their terminal
hashes bind the associated outcomes and CSV/sidecar records. A rewritten log
does not acquire authority as another measured execution.

The full-analyzer RED copies complete directories, including their valid
sidecars, into an isolated parser-test directory, then changes only trailing
whitespace or comments. Both the repeat guard and analyzer refuse. Historical
versions fail the new assertions with zero setup errors
(`review.round9.redproof.v1.log`). Actual repetitions were already distinct;
the v3 measurement SHA and numerical values are unchanged.
