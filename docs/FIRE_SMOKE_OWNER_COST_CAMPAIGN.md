# r202 — measure the resident owner's cost before choosing k

This entry implements the owner's parallel cost ruling following acceptance of
r201m. The full-Picard owner at `b2ce94e43070396eb89c9249a0c7efc7b43c8e2e`
remains the numerical reference. Its sealed tier-8 replay was left unchanged.
On inspection it had exited with status 93 after 44 accepted steps, at
`0.072435702662914991 s`. R0 device validation refused the next attempt:
`bitmap=33565184 eos_failure_cell=179246 eos_failure_term_bitmap=3`.

The primary EOS witness denotes inability to certify the unique binary32
represented-pressure rounding. The deviation diagnostic inherits that result.
The witness alone establishes neither a physical-bound violation nor which
arithmetic interval caused the rounding proof to fail. Cell 179246 maps to
`(53,44,37)` in the 69×69×106 grid. The accepted prefix's largest velocity was
`0.1975611448287964 m/s`. No 15/30/60 m/s crossing was reached, so neither a
focusing verdict nor crossing-based selection of k exists. The refusal is an
EOS certification finding; it is not evidence that compatible momentum fails
to cure focusing. No state from this failed attempt was published.

The immutable accepted trajectory, rejected-attempt record, from-zero identity,
and final retained checkpoint are copied into
`rendered/fire_production_calibration/r202_owner_cost/`. The original replay
directory and executable are untouched. The checkpoint is retained as a
diagnostic input, not permission to resume the sealed leg.

## Measured cost and its scope

All numbers below are milliseconds. P95 is the nearest-rank observation
`sorted[ceil(0.95*N)-1]`, as in the owner test. The 44 observations are accepted
steps in an early, cold prefix of one run, not independent steady-state trials.

| Scope | Samples | Device p95 | Wall p95 |
|---|---:|---:|---:|
| r201m small qualification fixture | 5 | 922.611917 | 950.393375 |
| Sealed tier-8 accepted prefix | 44 | 86643.987124 | 88175.631708 |

The replay CSV's `wall_ms` brackets the resident attempt and its request
construction. Outer source production, checkpoint serialization, and statistics
are outside that bracket. It is not whole-simulation wall time. On average:

| Measured contribution | Mean ms |
|---|---:|
| Resident projection GPU intervals | 2512.143832 |
| Other GPU intervals | 74318.700829 |
| Wall minus summed GPU intervals | 1375.503884 |
| Total device | 76830.844662 |
| Total resident-attempt wall | 78206.348545 |

Other GPU intervals include transport, physical flux, FCT, EOS, target assembly,
payload authentication, momentum, and terminal publication. They are not a
measurement of hashing alone. The wall-minus-device term includes allocations,
encoding, staging, host checks, and scheduling gaps; it is not a direct CPU
profiler measurement. The rejected causal-seal experiment is diagnostic history
only and supplies no admissible optimization result.

| Accepted R0/R1/R2 counts | Steps | Projections | Commits | Mean device / wall ms |
|---|---:|---:|---:|---:|
| 2/2/2 | 1 | 12 | 68 | 69400.477 / 70683.196 |
| 3/3/3 | 8 | 15 | 86 | 86626.384 / 88151.246 |
| 2/2/3 | 35 | 13 | 74 | 74804.161 / 76148.176 |

These groups contain different states; their differences are not a controlled
per-iteration speedup. There are no accepted iterations 4–9 in this prefix.
Reducing an assumed nine iterations to three therefore cannot account for its
cost. Even the measured projection component alone exceeds 300 ms per step;
the target needs evidence about per-iteration implementation cost as well as k.
No tier-10 production-speed projection is claimed from these tier-8 data.

`tools/analyze_fire_owner_cost.py` reproduces the complete per-field timing
summary and count distribution. It rejects missing owner identities, invalid
timings, nonconsecutive steps, stale endpoints, unreconciled timing partitions,
and missing iteration counts. Its report explicitly withholds a window verdict
and a choice of k. The analysis concerns timing only and changes no solver gate.

### Matched three-step stage profile

The separate `--fire-production-owner-cost-prefix` diagnostic completed three
steps from zero using the same three-second authored case and source schedule.
Its initial-state SHA, case identity, all three owner identities, and every
non-timing CSV column match the sealed replay's first three rows exactly.
`tools/check_fire_owner_cost_prefix.py` checks this equality before exposing a
cost comparison. This is a diagnostic prefix, not a shortened focusing verdict.

For its second step (R0/R1/R2 counts 3/3/3), the measured device stage totals are
26788.497 / 32401.374 / 26711.371 ms. The stage scopes include bootstrap and
terminal verification. The disjoint phase contributions include:

| Phase, step 2 | Calls | Exclusive wall ms | Exclusive device ms |
|---|---:|---:|---:|
| Prepare | 1 | 501.262 | 413.220 |
| Producer-stage group | 15 | 84309.684 | 83083.529 |
| Projection adapter | 15 | 2859.954 | 2814.672 |
| Boundary class reads | 24 | 4.214 | 0.058 |
| Combined residual reductions | 12 | 30.190 | 1.869 |
| Alpha residual reductions | 2 | 5.673 | 0.169 |
| Terminal publication | 1 | 12.913 | 4.204 |

The producer-stage group's wall-minus-device difference is 1226.155 ms across
15 calls. Its total device time is 83083.529 ms. This directly separates the
dominant group from projection and control work, but still does not distinguish
its individual kernels or divide host time into allocations versus encoding.
The profiler emits a tree: sum exclusive contributions, or compare inclusive
stage totals, but never add parents to children. Observer output overhead is
recorded separately. Raw iteration tags distinguish bootstrap, ordinary Picard,
and terminal verification. All three timing trees reconcile under only the
rounding bound of the printed timing values.

### Momentum diagnostic scope

`--fire-production-owner-convergence-fixture` exports the already-qualified
Metal and reviewed fp64 owner operands to text and CSV. The column includes all
adjacent horizontal faces and vertical faces. It records signed values and
successive-phase changes for provisional momentum, projected momentum, and
projected velocity, plus shared alpha. Provisional momentum is fixed within a
stage by the owner tableau; its zero delta must not be interpreted as candidate
momentum convergence. The produced `nextMomentum` field is not captured by the
existing trace and is explicitly unavailable in this exporter.

On the small qualified fixture, R0 and R1's sampled Metal projected velocity
changes are exactly zero at iteration 2. R2's successive maximum column velocity
changes at iterations 2, 3, and 4 are approximately 1.82505e-3, 3.92105e-5, and
6.75209e-7 m/s. These are changes, not certified truncation errors. The exporter
does not provide an enclosure for convergence and does not select k.

Six REDs refuse a stale trace, out-of-order iteration, truncated face shape,
swapped stage, production-labelled diagnostic, or mismatched owner before
publication. Profiling off/on preserves the exported operands and identities
byte-for-byte. The original per-cell/per-field fp64 owner gate runs in both
fixture executions. The sealed replay CLI rejects the profiling environment
before creating output. An invalid profile value or mutation environment is
also refused by the separate cost-prefix CLI.

The retained-checkpoint request harness is still missing. It must factor the
existing canonical attempt-preparation path (boundary-source staging, source
eligibility, pilot/contact masks, mixing times, source packet, represented dt,
then owner request). Reconstructing only a request from raw checkpoint scalars
would omit source lineage. The fixture exporter and three-step cost probe do
not satisfy the crossing-state measurement required to choose k.

## Convergence measurement and production variant pre-registration

The following is a design with unresolved measured parameters, not an enabled
production variant. The solver-spec prohibition on fixed iteration counts is
oracle-scope under this owner ruling. It continues to bind the full-Picard
reference. A production fixed-k owner may be qualified against that reference.

1. At each available retained crossing state, construct one authenticated
   matched-input diagnostic request, pin its checkpoint/source/time identities,
   and record each actually executed R0/R1/R2 iteration. Preserve bootstrap,
   Picard, and terminal re-projection as separate phases. For every sampled
   column face record provisional momentum in kg/(m² s), projected velocity in
   m/s, their changes from the preceding phase, and the relevant class sequence.
   Iterations 1–9 are requested observations, not an instruction to manufacture
   iterations after the reference has converged. A missing iteration stays
   absent. The existing full-field qualification trace is diagnostic-only;
   enabling its transfers in the production replay would invalidate residency.
2. Select the smallest k supported by the measured momentum/velocity changes
   and the existing per-cell, per-field contract at all matched states. A
   column trace nominates k; it does not prove whole-field readmission. Bring
   alpha and active-set predicates under the r201 obligation discipline before
   any class-envelope acceptance. Class disagreement with an interval excluding
   zero is a defect. No ambiguity or truncation is hidden by a pooled ratio.
3. The candidate schedule retains FCT transport, one shared alpha, compatible
   momentum flux, force-inclusive provisional momentum, exact Heun weights,
   authenticated parent lineage, and terminal projection against the current
   target. Use the selected fixed k per stage. Record remaining tangent,
   momentum, transport, and class residuals. Monitored manifold handling and
   r170 tail protection remain shared with the reference. Physical projection,
   conservation, admissibility, and unambiguous boundary feasibility stay
   fail-closed. Stopping Picard early does not waive these gates.
4. Bind k, stopping-policy version, monitored policy, source semantics, and the
   diagnostic schema into the owner/case identity. Record residuals per accepted
   step and bind the artifact SHA. Private payloads still require authenticated
   contents and immediate parents; the rejected causal-only seal cannot return.
5. Compare the candidate and full-Picard owner at matched inputs: local
   enclosures when classes agree, discharged two-path envelopes when certified
   predicates are ambiguous. A fixed-k truncation error outside the existing
   contract is a failed candidate, not grounds to widen a roundoff enclosure.
   Verify resume equivalence for scheduling-only changes and atomic refusal for
   incomplete or unsealed stages. Then measure device/wall p95 at tier 10 against
   the 300 ms target; include all resident-step work and disclose outer work.
6. Only after this gate run the variant's independent tier-8 from-zero leg.
   Compare it with the full-Picard focusing verdict. A cure under full Picard
   must survive the variant. A physical failure under full Picard makes the
   speed variant moot. The present pre-onset certification refusal leaves both
   outcomes unknown and keeps tier-10 windows blocked.

The remaining missing evidence is the full-Picard onset/crossing trajectory,
matched-state convergence envelopes, and a tier-10 cost measurement. k remains
unselected. The owner's expectation of convergence by iteration 2–3 is a
testable hypothesis, not a selected production constant.

## Evidence identities

The measured producer-stage table above is the exact `edb4afb6` build; its
artifacts remain immutable under `exact_edb4afb6/`. A fresh review of `919a604a`
found no P1 and four distinct P2 diagnostic-hardening gaps: snapshot destinations
missing from the cost-scope guard; unassociated timing trees; off/on artifacts
compared without their run seals; and unchecked timing/class records. These
were repaired in `827608d5` and the following checker hardening. Named REDs
refuse both snapshot destinations, reordered/duplicated/counter-mismatched
profiles, stale-equal artifacts, absent fp64 qualification, duplicate RED names,
nonfinite timings, and invalid stage/iteration tags. Outcome digests bind the
profile log to the source/case protocol, and root timing plus counters and stage
schedules associate each timing tree with its trajectory row. These repairs
change neither the resident owner's kernels nor its numerical stopping rule.

`exact_827608d5/` records the repaired-build off/on qualification and repeated
three-step cost diagnostic. `instrumentation_live_binding.v3` binds current
sources, the measurement binary, and both generations of evidence without
rewriting v1/v2. The second fresh review found no P1 and one further P2: the
observer checker needed the complete fixture's terminal qualification verdict,
not only its exported-operand markers. Truncated-log and failed-terminal-verdict
REDs now refuse; all four exact-generation fixture logs pass this stronger check.
Both three-step probes are diagnostics, not an onset verdict or
independent steady-state performance trials. The checkpoint-source request
harness and candidate-momentum convergence certificate remain unimplemented.

- Accepted trajectory SHA-256:
  `0f86f47d9b0b9426c89e81fdaf97c5872ab07f51e729ee37e1f899aca496fa5e`.
- Rejected-attempt record SHA-256:
  `bf22133e7c6f0c312e47b487f5217141e9722d1ff48a41d161c77f12232fe811`.
- From-zero identity SHA-256:
  `1db3489484decbf9518b1c5d84fe1fcaa1c0e5e9f72f5c38dfc889d5a15061f0`.
- Final retained checkpoint SHA-256:
  `4a5d1c2f5bac9b23bf149f5e700668314e52658c83e22f15eef597294ca8300a`.
- Derived cost report SHA-256:
  `ea6ad86a08bbcabf7e318758e0c877ddb5b7548cc95a8372756868ef3bb08195`.
- Replay protocol SHA-256:
  `5578fa19dcd500ff68743f7f1cfeff21d22da43fef48a3ada42df042badf3e3d`.
- Original replay executable SHA-256:
  `b797371976d62f14b854b87a83c047700ad1a115caef00c74203a327e0edc323`.

The small-fixture timing comes from the existing r201m owner log at
`r201_projected_heun_metal_owner/repaired_exact_2189762b/owner_gate.log`, SHA-256
`90c82ad6df513f123494896a0ef510611170487cea00582839054f1b1e801727`.
