# r218 — literal-exactness census and preserved run lineage

Status: r217-lineage audit and clean qualification complete; the additional
closed-domain count correction is not promoted production code. A separate
main/cost-lineage fifth-primitive sibling must be repaired before integration.
The sealed r217
tier-ten onset continues unchanged to 3.5 s or its first 15 m/s crossing.
No prefix is a survival verdict. The baseline crossed 15 m/s at 2.131 s and
60 m/s at 2.138 s; the final report must include the accepted bracket around
2.131 s, peak/final velocity and any crossing bundle.

## Exactness pattern and census

The audited pattern is a rounded literal or conversion entering a zero-radius
constructor while the derivation assumes a different exact rational/integer.
This is not a search for all decimal spellings: authoritative represented
inputs and explicitly enclosed constants have different contracts.

| Surface | Disposition |
|---|---|
| Compensated EOS direct/shared fifth primitives | r217 repairs both rounded `0.2f` operands with certified division by exact 5. Third primitives already divide by exact 3; halves/quarters are exact. |
| EOS logarithm | Odd series denominators and exponent are exact small integers. The ln(2) triple has an explicit 2^-54 mathematical remainder, independently checked by the rational-series oracle. |
| Thermo tables and affine data | Values originate as authoritative binary64 records, then split into binary32 limbs with tail-spacing bounds. Their ideal decimal spellings are not assumed exact. |
| Lattice midpoints and policy constants | Adjacent-float midpoints use exact halves. 0.125, 0.0625, 1.0625 and 0.9375 are dyadic. |
| Target cp, enthalpy, tangent and assembly | Consume bounded thermo coefficients and represented input/source words. The shared corrected enthalpy helper covers downstream targets. |
| Closed-domain target normalization | **Additional defect:** `float(p.cells)` can round an integer count; both the mean and inherited-radius division used that rounded denominator. |
| Raw-float physical-flux thermo | Rounded 1/3, 0.2, ln(2) and gas constant belong to the r198 packing/uncancelled arithmetic-enclosure contract, not a zero-bound EOSDD operand. No change warranted by this pattern. |
| Host interval walkers | Exact integer divisions use outward operations. Canonical binary64 constants denote represented record values. `CertifiedRecord(2.0/3.0)` includes conversion error. |
| CPU owner and generated mirrors | Explicit /3 and /5 retained; generated declarations and source manifests checked, not hand-maintained alternate formulas. |
| Payload digest v2 | Chunk size 4096, fan-in 16, offsets, quotient/remainder and serialization are integer operations. No floating reciprocal/count enters the root. |

This census describes the r217 run/candidate lineage, not the separate r215
optimized target-basis implementation on main. The sweep covers callers and
one downstream hop: EOS endpoint tables,
direct/shared mixture evaluation, inversion, target cp/tangent/assembly,
closed-domain normalization and digest-root serialization.

## Count finding: two distinct local obligations

For a constant-one target and `n = 257^3 = 16,974,593`, binary32 conversion
produces 16,974,592. The nominal mean acquires a 5.8911578e-8 bias before
subtraction. This conversion error is absent from the old denominator's
enclosure. Request shape checks permit counts above 2^24; the conversion
cannot be justified by a general exact-count precondition.

The radius obligation has its own exact witness. With `n = 16,777,219` and
radius sum 1, the old upward-rounded quotient is
5.960463411724959e-8, below exact `1/n` (approximately
5.9604634117251494e-8). The decisive test uses the binary32 significand and
exponent and an integer product, not a floating-point comparison tolerance.
This proves a scalar upper-endpoint defect; later outward additions may
mask it, so this alone is not evidence of a final publication escape.

The correction constructs every uint32 as two exact 16-bit limbs and applies
`two_sum`; UINT32_MAX becomes `{2^32,-1}` with zero input radius. Mean and
radius division consume this same exact count. The old outward operation is
retained when the count is exactly representable, enabling four-word parity
checks. No empirical allowance, new tolerance or enlarged envelope is used.

The active tier-ten run has 976,272 cells and pressure-open boundaries; its
count is exact and its target path returns before closed-domain normalization.
The candidate therefore does not require interrupting that sealed run.

Pre-registered scalar qualification covers exact count reconstruction,
independent mean and inherited-radius/assembly enclosures, signed and zero
numerators, zero/nonzero radii, and exact-count four-word parity. Separate
rounded-mean and rounded-radius mutants must fail. Zero count and unknown
mutation modes refuse before dispatch. No grid-sized fixture or candidate
publication authority is fabricated by this scalar test.

Candidate scalar execution passed: 14 counts times six signed/radius
scenarios, under the production shader and two separate mutants. At the mean
witness the mutant residual is enclosed between 5.891157796311097e-8 and
5.8911577963110997e-8, while its claimed mean bound is only
3.0678566410963912e-25 (all dimensionless). The corrected mean residual is
zero. At the radius witness, the corrected scalar upper endpoint is
5.9604644775390625e-8 cell^-1; the isolated old-radius mutant gives
5.9604634117249589e-8 cell^-1 and fails the exact reciprocal obligation.
Its final assembled enclosure happens to remain valid after subsequent
outward addition, matching the distinction above.

The candidate's local source patch SHA is
`9c3c3fa2da6efc707bf579df4fcbf1c010a4dc7e649a6f94a1f072d993aff023`.
Its scalar transcript SHA is
`c813ded3f30e1834bfd3d4bf46a1ea01c3740e9c6dc181832a6e748360dc5d35`.
The scoped build passes without warnings. The full CPU FireSequence suite
and r190-reference resident owner/target gate also pass, with transcript
SHAs `dd2c219427bef26edae5f9062d977d0b563a6a3c87e8885abdaf654ebaedeedd`
and `be70704385d33d6d322fcf2d2770f0359872e16fbf6112a726f49ce58e17eb41`.
Two fresh independent reviews (numeric derivation and test/provenance)
report zero P1/P2 on the current patch. Subsequent isolated clean qualification
also passes: make all and FireSequenceTest build without warnings; Deployment
and shipping Opto clean builds succeed with only the documented missing local
OIDN path and AppIntents metadata notices. The CPU suite, 252-row Metal count
gate and r190-reference resident owner/target gate pass. The fresh checkout's
first CPU run failed because the r136 historical r112 tier-six binary input
was intentionally absent from git; copying its preserved SHA-pinned bytes
into ignored rendered output restores the exact fixture. No expectation or
tolerance changed. That failure transcript remains recorded, as does an
earlier launch attempted before linking had produced the executable.

All seven candidate source files are byte-identical to the previously reviewed
candidate. A fresh additional code/test review reports zero P1/P2. These
candidate receipts do not identify a new accepted main production build.
The exact tested source is committed as `938ef6d77604` on the new
`codex/r218-literal-qualification` branch, which passes the pre-push guard;
main/cost integration qualification remains.
See `docs/evidence/fire/r218_clean_qualification.v1.json` for per-file and
transcript SHA-256 identities. No performance claim is made under concurrent
build/onset load.

### Main/cost integration finding

The separate main r215 optimization has an additional fifth primitive in
`target_enthalpy_basis_dd` (line 3195 at `784cb922`), using
`eos_mul(basis.t5,eos_dd(0.2f))`. This is the same zero-radius rounded-1/5
defect, not the raw-float physical-flux packing case. It is live in
`evaluate_resident_target_terms`; the loaded thermo-coefficient enclosure
does not cover this separate literal's representation error. Its optional
direct-helper comparator cannot prove correctness while both helpers share
the error. Independent read-only review confirms the call path.

Before main/cost promotion, replace it with certified
`eos_div(basis.t5,eos_dd(5.0f))` alongside the direct/shared r217 repairs;
derive its mutation RED and requalify optimized/direct bit parity and local
enclosures. The isolated r217 candidate lacks this optimized helper, so its
green checks are not evidence for that cost path. No main numerical source
or active-run file has been changed by this qualification.

## CPU formal-contract status, not a substituted verdict

All 19 formal quantities remain unqualified. The required independent terms
are `Eh,P + E_dt,P + Eh,O + E_dt,O + B32`, on the r112 exact-integral cubic
B-spline observables at width 0.04894898570785762 m. Historical old-remap
terms cannot qualify the adopted compatible owner.

The shared native oracle has /8, /16 and /32 outputs at an exact common end
time. Its energy filtered differences are 0.0019403897741448897 and
0.0010119867395659468 J/m³; velocity RMS differences are
1.0416656655103438e-5 and 5.6465786132446363e-6 m/s. These are oracle-only
dyadic diagnostics, not production–oracle distances or additive tolerances.
Filtered trace channels 6 and 7 do not contract; their existing narrow
historical floor certificate cannot simply be reused for these new pairs.

The frozen-child fp64 owner diagnostic still refuses R1 cell 44748: a
fuel-wide [300,5000] K re-inversion selects 300 K while the case-certified
[300,2300] K candidate supplies 300.70410484148613 K. Threading the existing
authenticated case/EOS authority into the target producer is the next CPU
repair; overriding T or widening its equality check is not. Even after that,
strict-fp64 same-scheme source issuance and independent spatial/temporal/B32
qualification remain separate prerequisites.

The full oracle-only difference tables are local at
`worktrees/r215-native-composition/rendered/fire_production_calibration/r215_shared_native/`:
`oracle8_vs16.v4.csv`, SHA
`683729228b786c2ddacf8efd50d803a7cfb9d5ccd002fe572ac9e57d3d7ff148`, and
`oracle16_vs32.v4.csv`, SHA
`e6ae28574feb47f0d8ceacbfe160c4037de69dc06df4ecba757572fde2c5c344`.
The retained R1 authority diagnostic SHA is
`eec4b5270cdfc203cbf7c80314e918346f865b50668219934350e1b257aeff05`.
The -184 versus -23.40 face-rate remains a concentration diagnostic outside
the filtered contract's pointwise scope (r112/r172).

## Existing promoted cost, kept separate from the run

Post-strip main already carries r215 target-basis reuse in `051a53cbcf96`.
Its matched tier-eight hot mean device/wall cost is
5.041421881 / 6.954142799 s; p95 is 5.456842495 / 7.485916334 s. This is
the existing promotion, not a newly measured r218 improvement. The remaining
1.912721 s mean wall-minus-device difference is unattributed, not a measured
hashing cost. The cost receipt SHA is
`b4920eebe7319db38ec32746e092d430319a4f8af36ddabdb4cbafcc9334ad92`.
Neither the overnight nor design-budget target is met; fixed-k remains
unqualified. The frozen onset executable has not been replaced with this
cost branch or the new count candidate.

## Evidence and branch boundary

The owner's 2026-09-08 evidence policy is binding: commit source, tests,
documentation, small summaries and SHA-256 records. Keep payloads,
checkpoints, executables, archives (including split archives), large logs
and frames local under ignored `rendered/`. Rebuild from source commit,
flags and input SHAs; do not describe binary extraction as replayability.
Historical payload seals remain unchanged.

Eleven fully merged branch refs had already been removed after proving
ancestry in the existing pre-strip backup. After explicit owner confirmation,
their recorded tips were recovered into named
`refs/backup/<full-branch-name>-pre-strip` refs. The same named backups were
created for eleven inactive live branches before rewriting any of them.
All local files remain in place; backup refs retain the old objects.

The eleven inactive branches are now re-anchored at their mapped post-strip
forks. All 42 suffix commits preserve their exact old trees minus forbidden
artifact paths only, including unchanged modes and object IDs for every
retained leaf. Each resulting branch passes the pre-push size guard; no push
was performed. The nine participating worktree indexes changed only by
forbidden-path removal. Main's Xcode edit and the dirty target-assembly
source patch retain their pre-operation SHA-256 identities.

The owner explicitly excludes `codex/r217-clause-diagnostic` and
`worktrees/r203-producers` until the onset completes and its evidence is
sealed. That branch's tip, index and frozen executable remain unchanged;
it was excluded even from ref-locking verification transactions. Rebase
it last, with its own prior named backup. A mapped-fork rebase is deliberately
not a merge of current main's cost changes into the sealed run lineage.

The independent cleanup review closed three findings: active-ref locking,
uncertain/interrupted ref-transaction recovery, and index-directory durability
before terminal journal publication. Four isolated synthetic cases pass:
success, interruption after the child commits refs, interruption after the
first index installation, and an index durability failure. Failure cases
restore original refs and index bytes; no payload, staged/unstaged source,
or untracked file changes. Fresh final review reports zero P1/P2.

The branch/commit map and verification receipt are in
`docs/evidence/fire/r218_inactive_reanchor.v2.json`; recovered deleted tips
are in `docs/evidence/fire/r218_branch_backups.v1.json`. Full scripts and
recovery journals stay local with their SHA-256 identities in the receipt.
Literal-candidate clean-build/Opto qualification now passes in the separate
checkout, but the main/cost integration finding above remains open. This
metadata cleanup is not a numerical promotion or an onset verdict.
