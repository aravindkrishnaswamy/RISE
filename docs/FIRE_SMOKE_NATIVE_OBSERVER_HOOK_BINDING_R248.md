# r248 native owner hook binding — proposed integration design

This refines the accepted integration contract with inspected native insertion
points. It does not accept an implemented observer, runtime, allocation plan,
full history, state parity or simulation. Synthetic Python token controls are
not substitutes for the actual C++ authorities and Metal command dependencies.
No production code or existing serialized format changes in this increment.

The active class is `ResidentProjectedHeunMetalOwner`. All factories below are
proposed private owner operations. External callers may request an observer
mode and receive a reduced, pending diagnostic result, but may not manufacture
native view leases or attach a guessed target ID. Qualification substitutions
must execute inside a separately selected native qualification path. The
production caller must refuse such qualification selectors as it does today.

## Preconstruction admission and disabled path

The public owner wrapper currently constructs the owner inside
`RunProjectedHeunOwnerInAutoreleasePool`. The constructor deep-copies the frozen
source seal into `sourceSnapshot_` before `Run` applies its owner working-set
certificate and before `Prepare` validates that snapshot. Context construction
can also create pipelines/resources. Therefore a new observer preflight inside
`Prepare` alone is too late to account for all live host/context resources.

Proposed integration must first extend a caller-side, before-construction
admission that inventories the already-live caller source/request, the owner's
source snapshot copy, contexts, owner allocations and incremental observer
resources. It must preserve existing `Run` certificate/device checks and the
caller's host ceiling/reserve. Checked dimensions/byte arithmetic and queried
device bounds precede added field/scratch allocation; context/pipeline creation
has its own prior budget and refusal record. No complete inventory or measured
worst-case capacity is accepted here. Refusal records must include phase,
requested sizes, aliased allocation IDs, live counts, RSS/footprint/peak and actual
device allocations; do not derive reader peaks from source ceilings.

Observer off retains the current constructor, context/pipeline selection,
command encodings, canonical input identity and payloads. The future enabled
path registers a separate observer operation manifest and requests capacity
before constructing its resources. It must not force compilation or creation
of observer pipelines on the off path. Full exact native parity and original
algorithm/golden serialization tests are required after integration.

## Owner-private leases and command capture

Proposed private lease fields bind owner invocation, actual native authority
object and retained capability, selected Stage generation, `id<MTLBuffer>` or
host snapshot reference, allocation identity, offset/length/shape, operation,
producing command and exact writer/read ordering. Pointer addresses are only
live comparison operands, never persistent SHA identities. A copied caller
structure or 39 integers is not issuance. Retaining a capability does not itself
freeze a mutable buffer: the queue dependency and pre-overwrite hook are required.

The existing `Encode` creates a compute encoder on the supplied command, binds
buffers, dispatches and ends encoding. New reductions belong on the same tracked
producer command after the relevant writer and before the next writer. Read-only
inputs must use the native buffer views retained by that Stage. The qualified
integer reducers need native support-address and batched-channel adapters; their
standalone input-upload harnesses cannot be called inside the owner as proof of
resident capture. Intermediate host staging of full grids remains forbidden.
Host reads wait for actual command completion and failure-bit checks. A completed
small output remains private until selected lineage and canonical publication
validate. Command failure, buffer reuse, stage eviction or allocation generation
change invalidates publication rights without changing physical acceptance.

## Concrete insertion points

| Inspected native point | Proposed private binding and capture | Required evidence before implementation acceptance |
|---|---|---|
| `Prepare`, immediately after frozen source admission | Retain a const owner-owned host chemical view referencing `sourceSnapshot_.GasHeatReleaseWPerM3()` and `SootHeatReleaseWPerM3()`. Bind the exact validated source packet, beginning input, case, dt and attempt. | Admission includes `FireProductionFrozenSourcePacketSealMatchesBeginningState`, methane/case checks, represented time and attempt, and exact SourceDelta equality. Preserve all checks; add exact chemical sizes/finite/shape support as required. Hash exactly the host binary64 bytes consumed; any new modern chemical receipt is newly issued diagnostic provenance, not an old producer sidecar. |
| `CorrectTargetAuthority`, immediately following successful `ownerComposeTarget` encoding | Encode bounded pre-finalization reductions of this exact `target.assembled`, its base/monitored/parent operands and correction buffer on the same command, before `finalizeTarget`. | The pending lease binds this actual target object, Stage, command and generation. No target capability/publication ID is invented before successful identification and issuance. Native overwrite and substituted-view controls must fail. |
| Same method, following successful finalize/identify/consumer encodings and capability issuance | Attach the pre-finalization pending result to `target.capability`, `parentTargetCapability`, `correctionIteration` and exact publication/consumer views only for this same target construction. Encode post-finalization reductions before target release. | Actual command completion and failure checks remain mandatory. Issuing a host capability after encoder creation is not proof the GPU succeeded. Bind the produced buffers/tokens and selected lineage after completion; no generic Python immediate-generation assumption can replace the actual new-target-object path. |
| `Project`, after existing bootstrap/derived capability authentication | Bind the exact target consumed by the projection adapter, expected parent capability, stage and correction iteration. | Preserve bootstrap special-case and issued-capability membership plus actual parent-command/publication/consumer checks. Issued-next-target and consumed-target remain separate. |
| Projection adapter `correct_faces` and subsequent periodic seam command | Emit actual executed gradient/branch and endpoint witness reductions before provisional/pressure resources are released; capture endpoint changes before and after seam copy. | Requires reviewed shader/adapter changes, exact compiled source/options/pipelines, branch authenticity and compiler/FMA/native on-off parity. Burner and wall early returns have unavailable gradients; seam copy is separate; no inferred gradient or second EOS impulse. |
| `BuildStage` and terminal Heun average encodings | Capture actual selected R0/R1 instantaneous and averaged physical operands, candidate/source/limiter authorities and materialized Heun rate fields before reuse/release. | Seven-species-before-sign, donor selection/bins, native address/support and endpoint-rate operations remain missing. R2 force fields exist, but do not enter R0/R1 terminal Heun totals. Speculative generations cannot publish merely because their kernels completed. |
| `Run` after `sealedStage` checks, `ownerIssuePublication` and canonical payload digest succeed | Match pending reductions against the actual accepted R0/R1/R2 candidate parents, target capabilities, lineage and input/output roots. Return a pending diagnostic sidecar with exact new runtime/operation manifest identity. | Preserve existing owner gates and no-interstage-transfer refusal. Do not add observer bytes to canonical physical roots merely to make differing payloads appear equal. Owner success does not grant an accepted-step ordinal. |

The actual frozen chemical values are binary64 host vectors, while `frozenSource_`
uploads only binary32 divergence. The accepted Python exact chemical reference
and rounding helpers cannot be installed as a native callback by exporting full
fields or narrowing them. The chosen production route requires a separately
reviewed bounded C++ host integer port (binary64 compound/joint moments and RN-even
finalization), operating directly on the immutable owner snapshot with exact
same-support addressing. That port, its source/runtime identity, and measured
scratch/refusal controls are still unimplemented. SHA streaming scratch and all
canonicalization temporaries belong in the live inventory. CH4/O2 observations
remain a separate resident binary32 state/candidate channel.

## Caller commit and diagnostic failure boundary

The campaign caller is in `tests/FireSequenceTest.cpp`, not a general owner
callback. It calls `AttemptFireProductionProjectedHeunResidentStepMetal`, then
performs remaining physical/token/next-step checks and prepares mandatory
budget evidence. Its mutation sequence is not atomic: states and EOS/temperature
histories are assigned before later pilot gate checks; momentum and pilot
statistics change before ignition and budget-publication refusal points. Only
later does it append accepted dt history and advance clock/count. Earlier mutable
state, statistics or a prospective budget row cannot authorize a diagnostic
accepted ordinal. Preserve this existing ordering and failure semantics; do not
introduce rollback or physics retry as part of observer binding. The exact
clock update is `simulationTimeS += reaction.deltaTimeS`, followed by previous dt
and incremented accepted step count; its binary64 bytes must be copied in that
existing order. Trial dt, authored events and `acceptedSteps+1` from an earlier
attempt are not commit authority.

Proposed caller integration retains a pending diagnostic sidecar through the
same attempt and verifies all bindings before committing a diagnostic accepted
record. It publishes only after all existing acceptance checks and the actual
clock/ordinal update succeed. The caller alone supplies that actual ordinal,
beginning/end clock bits and accepted state root. A failure before that update
may leave earlier mutable fields, but those fields grant no publication rights. Output uses a distinct
modern diagnostic chain with checked pending writes, close/flush/seals and
previous-record linkage. Final success requires the full ordinal chain and
actual authored/observed event links. If diagnostic publication fails after the
physical state commit, stop the campaign before another step and retain that
accepted numerical state with incomplete diagnostic history. Do not retry its
physics, decrement the ordinal, fill the gap with zeros or treat the source run
as physically refused. Crash recovery cannot infer the missing chain from a
legacy checkpoint; no serialization change or automatic resume is sanctioned.

## Incremental native qualification order

1. Review complete before-construction/live host+Metal allocation and capacity
   plan, including context/compiler spill, speculative stages and caller outputs.
   Implement phase/refusal measurement controls before enabling new buffers.
2. Implement owner-private readonly view/capability bindings, first for the
   validated binary64 host source and target pre-finalization command seam.
   Qualify with actual owner/capability substitution, wrong parent/stage, copied
   object descriptors, overwritten buffer, command failure and stage eviction.
   Native controls must execute the actual factory and hooks, not a duplicate model.
3. Port host chemical arithmetic and add the missing resident operations/hooks,
   one family at a time, with independent arithmetic/provenance reviews.
4. Bind selected lineage, canonical owner sidecar and caller accepted-step chain;
   prove source/runtime identity, all event/ordinal coverage and streaming seals.
5. Execute observer off/on exact canonical accepted-state parity under one frozen
   new runtime; review physical interpretation and full CLI before any full run.

All matched tier6 windows, geometry, source/pilot/boundaries, seed1234, physical
gates and golden/legacy serialization remain unchanged. No puffing mechanism,
remedy, canonical taller qualification or soot/tier10 progression is accepted.
