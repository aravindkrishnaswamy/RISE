# r248 resident observer integration contract — design only

This supplements the accepted causal design and numeric/native prerequisites.
It accepts no implementation, owner observation, resource qualification, state
parity, new run or causal remedy. All original failures and unavailable claims
remain unchanged. The existing 39 identity slots are an address reservation;
they are not an authority envelope and cannot be reused as proof of parentage.

## Owner anchors and observation transaction

The inspected active owner is `ResidentProjectedHeunMetalOwner` in
`FireProductionAdvectionMac.mm`. `Project` authenticates bootstrap/issued target
capabilities and expected parent capability before the projection adapter call.
`BuildStage` issues transport, endpoint-class, physical, candidate, EOS, source,
target and lineage authorities. R1 candidate physical flux is `averagedPhysical`,
whereas its instantaneous `physical` remains a distinct authority. At terminal
publication, `sealedStage` checks candidate parents, target capabilities and
lineage before `ownerIssuePublication`; the canonical payload is hashed before
accepted budget observation. These gates remain mandatory and unchanged.

Proposed per-attempt observer transaction:

1. Allocate bounded output/scratch only under the reviewed incremental resource
   plan. Mint an observer transaction capability owned by this owner invocation,
   binding immutable input root, case, attempt, source, geometry, dt and new build.
   An attempt identifier is not an accepted ordinal.
2. At each exact producer hook, authenticate the resident buffer view (owner object,
   retained capability, producing command, role, offset, length, stage, generation,
   and producer/consumer authority). Encode reductions after its writer and before
   any overwrite/release. Store only bounded partial/result records, not copies of
   full grids. Wait for command completion before any host result access.
3. Keep speculative records private to the attempt. Picard/active-set/limiter
   attempts may produce different generations; retain only the records associated
   with the actually selected lineage. Evicted generations lose publication rights.
   A pre-finalization record is attached to its later-issued target only if the
   same command/view/generation reaches that target's successful issuance. Never
   identify an early snapshot using a guessed future target ID.
4. After existing terminal owner authentication and canonical payload hash succeed,
   verify observer manifests against those exact selected R0/R1/R2 authorities.
   Seal the diagnostic record with the canonical owner input/output roots. Only
   the simulation caller's accepted-step commit grants the next ordinal. A failed
   owner/step/observer/publication transaction cannot enter accepted aggregates.
5. Observation failure leaves the diagnostic campaign incomplete with preserved
   payload/log/refusal; it does not mark a numerical solver step as a physical
   rejection or retry it with altered dt. The diagnostic driver must stop before
   another step if it cannot publish a complete accepted history. Existing solver
   acceptance and the original source run status are not relabeled.

No full-grid interstage host transfer is permitted. Existing qualification trace
packets transfer full fields and therefore cannot implement this observer. Existing
terminal canonical transfers may remain; this contract adds reduced records only.

## Binding envelope beyond the 39 slots

The new diagnostic namespace must carry versioned, typed fields in addition to
numeric rows: source/runtime/kernel-set/observer-operation manifest SHA256,
canonical case and input root, owner output root, attempt ID, caller commit ID,
accepted ordinal, previous accepted-record SHA256 and record byte SHA256. Each
R0/R1/R2 binding includes current candidate parent, stage lineage, transport,
instantaneous physical and candidate-physical (averaged R1) authorities, endpoint
class producer, EOS/source identities, target-issued and target-consumed identities,
both correction iterations and target parent capability lineage. Bootstrap is
explicit, not an invented zero parent; authority IDs are opaque values whose
meaning is checked by the owner, not by nonzero tests on caller-supplied integers.

Numeric reductions additionally bind role/support/operation version and exact
native view dimensions/offsets/byte counts; boundary parameters, head/classification,
prescribed burner operand and their issued identities accompany projection rows.
Owner issuance is required: copying 39 uint64 values into a host packet cannot
create authority. Current qualified stage tokens/FNV identities are not SHA256
payload seals; do not describe them as cryptographic field authentication.
The new small records get modern payload SHA256 seals and exact source/runtime
provenance, while native authority correctness remains a qualified owner contract.
The implementation must register observer operation pipelines and compiler options
in its own exact manifest and qualify their execution, rather than infer lineage
from a record hash alone. Pointer addresses never become persistent identities.

## Required hook and operation manifest

| Family | Resident producer and capture deadline | Required operation and limit |
|---|---|---|
| Scalar | Immutable Q0/source inputs; selected R1 candidate and R1 averagedPhysical low/delta with selected R1 alpha, after producer completion and before stage release | Q0/R1/frozen density increments; exact low+alpha*anti reference per face before signed split. Record materialized solver operands separately; this reference is not FCT floating-tree replay. Nine fields plus species1..7 total before cell/face sign split. Seven-term compound leaf is **not implemented** by the accepted two-term compound kernel. |
| Spatial reaction/fuel/oxygen | Exact immutable frozen-source host snapshot for chemical binary64 fields; each declared native conservative stage input/candidate for CH4/O2, with separate source and candidate authority binding | Exact binary64 gas+soot before support through a separately scoped authenticated host source channel (new arithmetic/provenance prerequisite); CH4/O2 uses actual native conservative field bits. Native/common/central/perheight P/N/count and joint P/M1/M2 use each declared representation. Same immutable leaf/support for all moments; caller-coordinate standalone controls do not authenticate native addresses. Chemical power, frozen density increments and inventory remain distinct from delivery. |
| Donor/limiter | Actual instantaneous R0/R1 transport parent states and native donor selection operands, instantaneous low/delta and selected limiter lineage before release | Separate R0/R1 donor witnesses with shared accepted alpha where actually consumed. Averaged R1 low/delta is not a single donor state. Bind exact upwind/drift/boundary donor selection from the active kernel; no assumed station or averaged donor. Bin/zero-positive support reductions need their own native kernels and qualification. |
| Heun momentum | Selected stage rates, R1 heunR0AdvectionRate and advectionRate, R0/R1 nonpressure RHS; terminal materialized Heun fields and R1 nextMomentum before publication transfer | Reduce actual f32 stage-sum/signed-half fields for advection/buoyancy/stress/phaseSource. Endpoint/projection/total/closure differences use explicitly versioned arithmetic, distinct from endpoint f32 operations. Whole native/common and unique central MAC supports; counts/means are not volume integrals. R2 also generates resident advection/nonpressure RHS fields; this first manifest intentionally excludes those unconsumed R2 diagnostics from the published Heun contribution. They are available resident fields, not unavailable/zero. Any later R2 diagnostic channel requires its own stage authority/support and must not enter R0/R1 Heun totals. |
| Projection | Inside actual adapter `correct_faces`, then before and after `copy_periodic_seam`, before private pressure/provisional buffers are released | Capture executed branch and materialized gradient variable on gradient branches only; prescribed burner and wall early returns have unavailable gradient. Actual endpoint change and later seam-copy change are distinct. Exact diagnostic dt*gradient can use product reduction; rounded/FMA endpoint discrepancy is retained and never assigned to EOS. Hooking/reduction of gradient/branch still **not implemented**. |
| EOS target | Immediately after ownerComposeTarget and before finalizeTarget overwrites assembled; after finalization/issuance; consumed target at Project entry | Capture pre-finalized composed output, base/tangent/source/absoluteDiagnostic/monitoredAbsolute and parent/consumed/issued targets as available. Iteration0 is base-copy; later EOSDD computation is not independently replayed/certified by integer references. Closed-domain finalization may change all cells; pressure-open skips mean removal. Record target terms/differences in1/s, not a second restoration momentum impulse. |

Every row distinguishes unavailable support/field from a known zero. Native and
common80 z80 support is pressure-open boundary for the wide case and interior for
the tall case, authenticated by case/boundary operands rather than geometry labels.
Exact MAC prefix and central union addressing must be implemented from the accepted
layout; no doubled shared faces or lateral layer80 in common80 support.

Frozen chemical source representation is a separate prerequisite: the source seal
owns GasHeatReleaseWPerM3/SootHeatReleaseWPerM3 as vector<double> in
FireProductionTransport.h. Current frozenSource_ uploads only the divergence
binary32 target, not gas/soot chemical fields. This design chooses an explicitly
scoped host channel consuming the already-owned immutable source snapshot; it
adds no interstage full-grid device-to-host transfer. It must bind the original
binary64 bit patterns, exact modern source seal/PacketIdentity and owner snapshot
lifetime, and declare its CPU producer scope separately from resident fields.
New exact binary64 compound/moment arithmetic, bounded resource/finalization and
provenance controls are required before that channel is accepted. Include values
that differ below binary32 precision, signed zeros/extremes and source substitutions.
Never narrow chemical doubles to binary32, infer them from divergenceTarget, or
present the existing binary32 native compound/spatial witness gates as qualifying
this channel. A host-owned source snapshot is not a native chemical buffer.

Primitive controls now qualify f32 sums, two-factor products and two-term compound
references only. A resident integration still needs native address selection,
seven-term species composition, donor/bin accounting, endpoint/rate/channel
operations, projection branch/gradient emission and EOS before-finalization hooks.
Existing host packet references are not automatically device output codecs. The
spatial witness codec requires four captured fields and cannot validate a reduced
standalone packet without them. A new reduced-record decoder must independently
admit representation/availability/support constraints; do not transfer full fields
just to satisfy that host witness codec.

Each operation manifest states exact input bit formats, leaf composition, sign
split, support, scale, reduction record, rounding/finalization and units. Exact
integer references have no FP reduction tree; actual producer f32/EOSDD operations
retain their original tree and compiler contract. Unit conversion must bind exact
measure bytes and their construction (solver promoted binary32 dx versus authored
binary64 geometry are distinct). The host quotient finalizer admits only its
specified count/dt/one binary64 multiplier modes. Joint centroid/variance and any
multi-factor measure require an explicit separate bounded finalization contract;
logical integer bounds are not measured allocation. No silent per-leaf rounding
or unsupported use of the finalizer is allowed.

## Lifetime and publication ordering

The live Stage owns unique authority objects and mutable assembled buffers. Future
hooks retain exact views and capabilities until queued reductions complete. New
allocation generations distinguish reuse of the same address. Dependencies must
be encoded on the producer command or explicitly ordered on the same tracked queue;
no concurrent unsynchronized observer read. A finalization hook reduces the small
pre-finalization result before the following writer in that command, not afterward
from the overwritten view. Later acceptance selects the speculative result by
stage/attempt/generation and issued target lineage. Release/drain discarded records
in bounded per-attempt pools; no storage proportional to iteration/step count.

Owner and simulation caller are separate commit authorities. The caller maintains
binary64 beginning/end through the actual accepted binary32 dt in its exact existing
update order; the observer copies those bit patterns and independently checks that
update. Do not replace clocks with authored event times. Ordinals start at1 after
from-zero step acceptance, increase by1 with previous-record linkage, and end at the
actual accepted count. Authored235 events are a separately linked manifest; dt and
step count are outcomes and never forced to historical15315.

Stage diagnostic reductions may complete before canonical owner sealing, but no
accepted record becomes externally complete before the caller's accepted step and
all diagnostic bindings/measurements verify. Use exclusively created pending files,
checked writes/flush/close and modern seals; bounded append/chunk output binds every
ordinal with no duplicate/gap/reordering. Final completion binds full from-zero
history, window and authored/actual events, final accepted state and output chain.
A restart without exact diagnostic chain is an incomplete-history run; it cannot
inherit full coverage from a checkpoint alone or mutate legacy checkpoint bytes.
Refused speculative records use a distinct namespace and cannot fill accepted gaps.

## Resource and parity gates before integration acceptance

Before creating any diagnostic field/scratch, inventory the complete simultaneously
live owner and observer resources, including transient projection adapter resources,
selected/discarded stages, capabilities/commands, target hooks, input views, partial
banks, batched channels, output/staging, finite checks, hashing/Merkle/serialization
buffers, queued IO and host finalizer temporaries. Aliases have one allocation ID
with all lifetime references; requested/allocated sizes and compiler private-spill
costs are separate. Enumeration must produce byte formulas and phase live maxima
for104x104x80/160 with overflow refusal before allocation. No current complete
inventory/capacity bound is accepted; standalone256MiB controls prove neither.

Preserve existing owner preflight/host ceiling/next-step reserve and device limits.
Account observer incremental live bytes under a separately reviewed plan, not by
borrowing StageA1GiB/StageB2GiB/public2GiB caps. Query device and pipeline limits,
requested versus actual allocated sizes, current RSS and measured peak at real
phase/refusal points; reject unsupported capacity and retain those operand records.
A measured healthy sample is not worst-case register/spill or allocation proof.
Bounded channel batching is permitted only if all required families/supports are
covered, including every layer and all selected lineage before release. Scratch
reuse requires completion before reuse and no pending IO referencing old bytes.

Default off creates no observer buffers, dispatches, reads, IO or altered solver
inputs. Enabled/disabled controls use the same frozen new runtime/compiler/kernel
manifest; observer mode stays outside canonical physical input/publication identity.
If operation insertion changes canonical arithmetic or roots, parity fails; cannot
mask new identity fields or relabel different roots as equal. Full canonical accepted
payload bytes, stage outcomes/iteration counts, dt/event decisions, physical gates,
transfer counts and original serialization/golden checkpoint must match exactly
under new native controls. Observer commands/resources may differ but are recorded
separately. Actual gradient hook may alter compiler scheduling/FMA/register usage;
standalone arithmetic does not prove this harmless. Disabled-path compatibility
with the original algorithm/golden checkpoint also needs review/verification.

## Incremental qualification and experiment boundaries

Implement reviewed prerequisites in order: operation/channel/view/lifetime manifest;
synthetic authority substitution/stage-parent-consumed-issued/generation/overwrite
controls; complete resource/refusal controls; each missing native arithmetic/channel;
projection/EOS hooks; atomic owner/caller publication and chain; native observer
on/off parity and original golden serialization. Fresh design/implementation,
arithmetic/contract and lifecycle/provenance reviews accept each milestone.
A fresh physical review and frozen full CLI composition precede full simulations.
All full tier6 clocks/windows/geometries/source/pilot/boundaries/seed1234 remain
unchanged; no short control replaces those runs or canonical taller qualification.
Preregister phase estimator/repeated seeds and budgets before robustness runs.
Temporal correlation alone cannot select a solver/boundary remedy. Soot and tier10
remain gated; prior full r246/r247 derivations and native gates are not repeated.
