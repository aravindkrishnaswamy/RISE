# r208 — one canonical source publication, exact persistence carry

Status: source repair implemented and build/publication/r190 owner qualified.
Executed evidence lives in `rendered/fire_production_calibration/r208_single_source/`;
review and replay launch are recorded separately. No repaired focusing verdict is claimed.

## Finding and boundary

r207 refused before the resident owner at accepted step 390, time
0.6231521824374795 s. Source index 4039499 is sensible energy (component 8)
at zero-based cell (32,31,0) on the 69×69×106 tier-eight grid. Canonical value
was -0.048315051943063736; persistence had -0.048315037041902542, four binary32
ULPs apart. Both producers were CPU calls to the methane source machinery,
not a device-versus-host FMA comparison.

The harness independently called `BuildFrozenMethaneSourcePackets`, then
`FireProductionCanonicalSourceAuthority::Build` called it again. In addition
to separate state/control preparation, the harness integrated radiation using
binary64 grid width cubed; the authority uses its binary32 width widened to
double and cubed. This is a concrete input discrepancy, not yet a measured
attribution of all four ULPs. The diagnostic separately compares the original
legacy packet and a legacy rerun changing only volume, against canonical.
It reports remaining eligibility-temperature differences and does not label a
step-388/last-dt probe as the missing step-390 candidate.

## Single source

The projected-Heun route constructs the canonical seal once, before tangent
preparation. Persistence receives an exact copy/widening of that seal's source
bytes and all eight diagnostic fields, plus its radiation factor. The resident
request copies `SourceDelta()` directly. It never computes source physics from
the persistence packets. No alternative raw-dose or reconstruction callback is
an input to the carry API. The independent legacy/oracle operators retain their
own source path, in the mutually exclusive non-projected-owner branch.

The canonical producer and r190/Metal numerical owner are unchanged. The input
packing moved earlier, with the same binary32 Q, momentum, velocity, geometry,
dt and attempt-identity construction. The identity gate remains; its comparison
is now explicitly bytewise and size-safe, including signed-zero rejection.
This is not admission of a four-ULP tolerance. Canonical source authority remains
the one the old owner already consumed; failed r207 has no accepted next step
whose arithmetic could be retrospectively relabelled equal.

Device kernels, private allocations and owner working-set certificate are
unchanged. Source identity packing is completed and released before request
layout; the previously concurrent independent packet build is removed. No
host-memory or speedup measurement is claimed from this ordering alone. Build
identity changes; source/kernel numerical semantics and case physics do not.
Existing historical source ledgers are never rewritten with canonical values.

## REDs and gates

- Separate per-cell source bits and diagnostic bits must carry unchanged.
- A separate authenticated active-pilot fixture requires nonzero, distinct
  pilot energy and expansion fields. Omission of either field and swapping the
  fields must fail the byte comparator. Review caught that the original
  no-pilot owner fixture alone could not detect those mutations.
- The sibling soot-coverage audit adds qualification-only carbon along the
  pinned reverse-oxidation vector, preserving the affine species relation, and
  recomputes sensible energy at the original temperature. Its canonical packet
  must cover nonzero oxidation and soot heat, as well as the other live ledger
  fields. All eight fields reject nonzero injection and all 28 pairwise swaps;
  omissions reject for each nonzero field. Gross formation is the one structural
  zero: PhysicalV1 has exactly zero soot yield, which the fixture explicitly
  checks. Omitting that zero is an actual no-op, not a fabricated nonzero case.
- The soot fixture checks all seven species source bytes and energy as well as
  diagnostics. Nonzero carbon consumption is a precondition, and omitting that
  source component is RED; every source component also rejects injected bytes.
- One shared source comparator checks persisted double bytes against the exact
  widening of each canonical float in all three fixtures. One-double-ULP changes
  must refuse even when their binary32 round trip is unchanged; narrowing before
  comparison is insufficient for the persistence contract.
- An alternate reconstruction callable is not invocable through the carry API;
  it cannot reach the identity gate.
- Unsealed input refuses before modifying persisted packets.
- Defense in depth refuses changed dose, short/long payloads and negative zero.
- Structural routing mutants insert a second constructor or replace exact
  copy with reconstruction; they must fail. These are source-route checks, not
  a replacement for the numerical owner oracle.
- Independent r190 owner gate, full suite, crossing export and a sealed fresh
  executable are required before the repaired verdict run is claimed launched.

## Restart rule

r78 needs at least eight rejection-free old/new continuation steps with exact
time/temperature/EOS/frame identities from the same checkpoint. Retained r207
step 388 has only two subsequent accepted steps before refusal; no migration
certificate exists. The generic legacy migration fixture does not select this
resident owner. The r194 protocol additionally forbids resume. Therefore the
repaired verdict restarts from zero in a new directory under the unchanged r194
protocol SHA `5578fa19dcd500ff68743f7f1cfeff21d22da43fef48a3ada42df042badf3e3d`.
Neither the preserved r207 checkpoint nor the golden checkpoint is changed.

The executed build-and-owner receipt `qualification.v2.json` binds source commit
`2790a8ddc7bd03c6b967cceb839a871c7fa8c246`, executable
`57d87bbc4dcfbcc4f48fc96401950365513ec9f50052e9bc409f25c685d55edd`,
and separate build, publication and independent-r190 gate logs. Receipt SHA:
`e0905bed362d23db54f3542402c1e7cef3bac601a9b325140df4059adffc4112`.
No production kernel or fp64 mirror was changed. The later pilot repair affects
qualification inputs only, not the source carry used by the retained diagnostic.

The isolated old/repaired eight-step diagnostic from step 388 confirms the
repair clears the original refusal: the old executable accepts two further
steps then reproduces the same source mismatch; the repaired executable reports
`solver_accepted=1 checkpoint_unchanged=1`. The diagnostic's historical exit 93
on successful continuation means its expected EOS-refusal fixture did not fire;
it is not a repaired solver refusal. The old diagnostic exits 94 because it
cannot publish its EOS-specific envelope for a source-identity refusal. Its raw
log is evidence, not a complete publication certificate. Neither diagnostic
grants migration authority.

The retained-state localization probe finds zero differing components at step
388 using that checkpoint's last dt, including the volume-only variant. This
does not isolate the four-ULP failure's arithmetic cause at step 390. The fix
removes independent production reconstruction rather than claiming an unmeasured
FMA or geometry diagnosis.

## Parallel cost track

Target-assembly preparation lives in the separate `r207-target-assembly`
worktree. It is not merged into this repair or its replay executable. Compilation
alone is not device qualification or performance evidence. No fixed-k decision
or focusing claim is available from the refused r207 prefix.
