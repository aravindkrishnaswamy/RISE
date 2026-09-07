# r214 shared-owner qualification input prerequisite

This is a qualification-input adapter, not a shared-input flow verdict. It
does not change production arithmetic, publish a checkpoint, authorize a
migration, or provide a CPU fallback to the resident owner. No tier-8 formal
contract row is claimed by this increment.

## Why the checkpoint alone is insufficient

The live `RunMethaneFrameProbe` path first stages inlet fuel into
`packetBeginning`, re-represents its EOS energy, and stages inlet momentum and
velocity. The canonical source producer consumes that staged state, while its
eligibility input is the accepted **pre-injection** conservative state. It also
consumes source evaluation temperature, mixing time, pilot/contact controls,
case and exact time. A raw checkpoint-to-owner conversion would omit this
distinction. Historical `source_observation_inputs.v1` retains both Q states
and the source bytes but not all of the canonical control inputs/seal metadata;
it cannot mint the missing authority.

`FireProductionSharedOwnerInput.h` instead has a one-shot
`CanonicalSourceCapture`: the existing canonical construction runs once and
its exact inputs and opaque output seal are retained. `OwnerCapture` consumes
the resulting **actual owner request**, checks the immediately linked source
and candidate parents, and copies it into the reviewed generated r190 fp64
owner's request. It preserves the existing `CalibrationImport` boundary for
the already-authenticated source; it never accepts a caller-authored raw dose
as a source seal. All nested qualification mutations refuse.

The payload has a versioned binary schema, fixed little-endian words and
length-delimited fields. It includes staged source Q, pre-injection eligibility
Q, every source control, source packet fields and seal identities, owner Q/M,
boundary/class inputs, nullspace coefficients, physical constants, tolerances
and record-derived mirror contract terms. Its bytes are ready for SHA/v2
publication by the eventual qualification endpoint. There is deliberately no
historical-checkpoint claim inside that payload; that endpoint must bind the
actual checkpoint SHA, executable/build hashes and exact accepted schedule.

The initial test uses a real canonical 4-cubed source-active fixture and the
reviewed fp64 owner's `Begin` preflight. It does **not** claim R0/R1/R2 numerical
comparison or Metal execution. Named REDs retain the same packet object while
changing eligibility, source contact or mixing inputs; these refuse before
admitting the mirror. Stale parent, negative-zero dose mutation, nested
CPU-candidate mutation, unsealed capture and second construction/publication
also refuse. Distinct momentum bytes change the capture payload and the exact
promoted fp64 input. Mutation tests remove individual control serialization
terms and corrupt gravity mapping to prove those assertions fail.

## Separate fp64 qualification resource certificate

The next real tier-8 mirror exposed an inherited byte-cap error before any
numerical solve:

| Quantity | Value |
|---|---:|
| Tier-8 cells, 69 × 69 × 106 | 504,666 |
| Total staggered faces | 1,533,387 |
| r190 binary32 certificate, `(274 C + 97 F) sizeof(float)` | 1,148,068,092 bytes |
| Promoted mirror certificate, `(274 C + 97 F) sizeof(double)` | 2,296,136,184 bytes |
| Inherited raw byte cap | 2,147,483,648 bytes |
| Over inherited raw byte cap | 148,652,536 bytes |

The charter's section 2 explicitly assigns 2 GiB to tier-10 **fp32 production
resident storage**. The r190 entry and its evidence pin the float-word
certificate, including callback snapshots. The generator mechanically replaces
`float` with `double`, so the mirror correctly doubles the scalar footprint
but formerly retained the literal byte limit in `Begin`. No separate fp64
qualification resource derivation justified that inherited raw byte limit.

The correction preserves the original number of scalar slots:

`fp64 qualification capacity = (2 GiB / sizeof(float)) × sizeof(double) = 4 GiB`.

The generator now emits an explicitly named qualification-only capacity and
admission predicate. Integral slot-count and overflow assertions guard the
derivation. The footprint formula is unchanged, and non-scalar metadata is
not made smaller by this capacity conversion. The original production
`FireProductionForce.{h,cpp}`, its 2 GiB admission, and every numerical
expression remain unchanged. This is an execution-resource derivation
correction, not a numerical tolerance change and not a size chosen to fit the
tier-8 datum. The same-slot, exact-capacity, cap-plus-one, maximum-integer and
1024-cubed overcapacity tests retain fail-closed admission. The generator and
generated source manifest give this qualification executable a new identity.

## Qualification and remaining scope

Run `python3 tools/test_fire_r214_shared_owner_input.py` for a clean CPU-only
ASan/UBSan build of the adapter and its real implementation dependencies, with
the repository's `-Wall -pedantic` warning gate and `-Werror`. Run the mirror
generator with `--check` to verify generated identity. Metal is not invoked.

The retained CPU gate `r214_shared_owner_input/cpu_asan_ubsan.v1.log` has SHA256
`a6419ffc9cb1b7f09be88315908758438e74a03c2a90346e64ad9a8f0f62f711`.
It records 64 source-active cells, 64 nonzero source entries, a 19,894-byte
capture, a passing native fp64 `Begin`, and four compiled mutants failing with
the required exit status. Every compiled fixture executable and the adapter,
fixture, driver and generated source manifest are SHA-bound in that log.

Self-audit and fresh-review axes: omitted source-control identity despite
unchanged packet bytes; stale/forged immediate parent; incorrect promotion or
lost per-field operand; accidental production budget/arithmetical change;
false-green capacity/RED tests or claims extending beyond the exercised input
preflight. The native real-checkpoint endpoint is not included in this gate.

The native real-checkpoint capture endpoint, its checkpoint/build/schedule SHA
publication, the r190 fp64 transport-provider integration and complete
R0/R1/R2 run remain to be wired and qualified. A captured common owner request
is itself only a prerequisite to oracle shared-input composition. Production
and oracle own temporal refinements, port-specific spatial calibration, B32
subdominance and the filtered per-quantity contract table remain separate
required measurements. No existing r213 separate-trajectory diagnostic is
relabelled as a shared-input contract result.
