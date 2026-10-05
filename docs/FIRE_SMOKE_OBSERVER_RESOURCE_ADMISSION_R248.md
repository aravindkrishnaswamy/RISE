# r248 observer resource admission — source-derived implementation contract

This contract specifies the admission and measurement work required before adding
resident observer buffers. It is a design prerequisite. No complete live inventory,
resource bound, runtime implementation, measured peak, observer parity or full-run
qualification is accepted by this document. Existing numerical gates, host policy,
public fixture caps and serialization remain unchanged.

## Existing source evidence

The campaign caller constructs `projectedHeunSource`, builds an ordinary resident
request, then `BuildProductionProjectedHeunOwnerRequest` deep-copies source and
transport fields into `projectedHeunRequest`. The public native wrapper constructs
`ResidentProjectedHeunMetalOwner`; its initializer copies the frozen seal into
`sourceSnapshot_` before acquiring contexts. `Run` performs its device certificate
check later. A gateway solely in `Prepare` cannot cover these earlier allocations.

The frozen seal contains three binary32 vectors (temperature, nine-component
source delta, divergence) and eight binary64 cell vectors. The producer fills
these lengths, including moving canonical temperature into the seal. For an
admitted canonical seal, element payload alone is 108*C bytes. During the native
call the caller seal, request seal and owner snapshot are distinct owned copies:
324*C element bytes across these three seals. At 104*104*160 this is 560701440
bytes. This is neither vector capacity nor allocator footprint nor complete host
storage: string/object overhead, ordinary/request field copies, packets, states,
tangent temporaries and allocation slack are additional. Count actual vector
capacities in the implementation; do not infer capacity from these payload sizes.
Validate source lengths before forecasting its copy. Do not reconstruct, narrow,
mutate or export the binary64 chemical fields to implement admission.

`ResidentTransportContext(productionStageTokens)` initializes an ordinary static
context even when requesting the production static context. Cold production entry
therefore may construct both. FCT context is another static; payload Merkle context
is constructed lazily from `HashPayload`; projection and force adapters have their
own contexts. They persist beyond one owner lifetime. The owner's device baseline
is sampled after its constructor context acquisition, so the reported baseline
subtraction does not include those earlier cold resources. Warm context admission
must count persistent resources once; cold admission must reserve and measure
compilation/library/function/pipeline/queue creation before invoking constructors.
A runtime/device change invalidates a previously measured context qualification.

`Private` and `Upload` increment `actualBytes_` when allocation succeeds, without
release subtraction. `ObserveDeviceAllocation` samples process-device allocation
and stores a maximum relative to its constructor baseline. These provide different
observations, neither an allocation-ID live ledger. `ReadOpenClass` allocates shared
staging directly, and `EncodePayloadMerkle` allocates private tree nodes outside
those owner helpers. Command retention extends resource lifetime beyond a C++
local variable or authority destruction. Pool drainage is a lifetime boundary;
encoder completion alone is not release proof. Preserve existing reported fields
and introduce separate live inventory diagnostics.

The existing owner certificate composes projection, target and nonpressure
certificates plus rounded cell/face, ingress and publication/Merkle reservations.
The existing observer certificate adds a full-grid qualification trace packet;
it is not a certificate for the new reduced observer. The launcher reserve also
includes optional r245 terminal/host publication operands and general scratch.
Those reservations remain mandatory. Do not charge already-live memory again
as a future increment or silently spend a legacy reserve twice.

## Required admission phases

1. **Caller ingress, before request copying.** Use the current host memory sample
   and enumerate already-live caller allocations, capacities and aliases. Forecast
   new request/source copies and bounded admission/refusal scratch using checked
   uint64 arithmetic before allocating. Keep the existing host ceiling and reserve
   rule. Any additional observer reserve must be explicitly fitted under that
   unchanged ceiling; failure refuses observer campaign execution. Observer off
   retains the current path and physical identity.
2. **Native gateway, before owner construction.** Revalidate immutable request
   shape and lengths without source copies. Query actual device limits without
   initializing owner/observer compiler contexts. Record the existing persistent
   context inventory or cold context creation plan. Refuse unknown runtime/device,
   unavailable telemetry, count/byte overflow, invalid source sizes and an
   insufficient host/device allocation reservation before constructing the owner.
   The existing `Run` preflight still executes later.
3. **Context and setup allocation.** Measure cold creation as a separate admitted
   phase, then register persistent resources. Sample host RSS/physical footprint,
   process high-water RSS and absolute device `currentAllocatedSize` before and
   after creation; distinguish sampling maxima from process high-water telemetry.
   Measure compiler transients and disclose unavailable register/spill telemetry.
   A queried threadgroup bound is not a compiler/register memory bound. New
   observer contexts are created only for enabled mode after their admission.
4. **Stage, command and reduction.** Before every allocation family reserve its
   peak simultaneous lifetime, including the old input bank and new output bank.
   Track actual buffers by allocation identity/generation and retained commands,
   views, authorities and capabilities. Release credit only after all retainers
   and completed commands cease to own the allocation. Sample at allocation,
   completion, stage selection/eviction and drained pool boundaries. Preserve
   selected R0/R1 while solving later stages; include prior/current/certified
   candidates, active-set history and cycle branches, terminal reprojection and
   adapters. Iteration count is not a substitute for an actual lifetime maximum.
5. **Publication and caller return.** Include simultaneous canonical terminal
   device payload, Merkle trees, shared staging, decoded result vectors, pending
   r245 evidence, reduced observer records, SHA/IO scratch and binary64 integer
   finalization workspace. Pending diagnostics persist until the actual caller
   acceptance ordinal. Postcommit publication failure stops incomplete history
   while retaining accepted numerical state. Count pending/failed output resources
   until released; never manufacture a successful receipt.

## Complete inventory obligations

Each family below needs actual source allocation sites, request-dependent sizes,
checked arithmetic, maximum permitted live multiplicity, alias IDs, storage kind,
creation/refusal/release phases and queried device constraints. This table is a
coverage requirement, not evidence that an implementation already supplies it.

| Family | Included lifetimes and required measurements |
|---|---|
| Caller host | States, reactions, transport evaluations, beginning/staged/source packets, source seals, ordinary and owner requests, tangent molecular/temperature/conservative/velocity storage, accepted histories and pending frame/budget/checkpoint serialization. Scope exits and capacities must be inspected on each selected production path. |
| Native host | Owner source copy, packed setup tables/parameters/boundaries, copied strings and descriptors, control readback vectors, stage residual/class history, capability registries, canonical output/result copies and failure messages. |
| Persistent contexts | Ordinary and production transport, FCT, projection, force and Merkle contexts; libraries/functions/pipelines/queues, cold compiler transients and retained runtime resources. Warm baselines count each actual allocation once. |
| Owner Metal | Every setup upload/private pair, input digest assembly/tree, root/control/failure buffers, source/transport/physical/EOS/target authorities, all packed stage fields, target/class/lineage seals and adapter buffers. Include direct allocation sites outside Private/Upload. |
| Command/adapters | Projection multigrid/force scratch, internal command retention, active class/history/cycle buffers, target correction temporaries, shared staging and all per-command Merkle levels. Capture concurrent retainers rather than summing completed commands. |
| New observer | Native address/channel adapters, alternate integer banks, parameters/failure words, selected reduced outputs, host binary64 integer accumulators and rounding scratch, source SHA streaming, manifest/capability/view registry and pending ordinal/stream publication. No full-grid interstage host transfer. |
| Runtime overhead | Allocator slack, driver allocations, compiler/register spill and command/report/IO overhead: measurements under the exact source/runtime/device and disclosed unresolved components, not zeros or invented constants. |

Keep requested bytes, rounded design reservation, actual buffer allocatedSize,
absolute process-device allocation, current RSS/footprint, process peak RSS and
observed sample maximum in distinct fields. On unified memory, shared buffers may
appear in both host and device observations: enforce both limits independently;
do not add RSS to currentAllocatedSize and call the sum unique physical memory.
An alias references the same live allocation ID, not another charged copy. Two
separate buffers containing identical bytes are two allocations. No pointer
address becomes persisted authenticity or a guessed resource identity.

## Controls required before runtime acceptance

Execute controls against the real caller gateway/native allocation sites after
implementation; a Python planner or caller-authored allocation manifest cannot
qualify this contract. Run all heavy qualifications sequentially after inspecting
host exclusivity. Controls must freeze exact source/runtime/toolchain/device and
record baseline, reached refusal site, allocation attempts and observed memory.

- Cold versus warm actual production contexts, with observer disabled/enabled;
  prove no observer pipeline or buffer creation on the disabled path.
- Insufficient budget at caller-copy, native-constructor, context/compiler, setup,
  stage, reducer and publication phases. Each refusal precedes the denied family
  allocation; evidence records which earlier admitted allocations remain live.
- Malformed or oversized source vector, arithmetic overflow and unavailable host
  telemetry/device bounds. Refusal uses bounded scratch and avoids source copying.
- Real alias versus distinct equal-byte buffer, retained command after Stage
  eviction, adjacent candidates plus accepted parents, and bank alternation.
- Inject allocation/command/publication failure at actual sites; prove cleanup,
  bounded retained payloads and no sealed receipt on incomplete publication.
- Exact native observer off/on canonical accepted-state and golden serialization
  parity remains a later prerequisite; small allocation controls alone cannot
  qualify full history or physical interpretation.

The next implementation increment is the caller/native gateway and bounded
resource/refusal measurement plumbing, before readonly capability hooks or new
observer buffers. A complete measured qualification must follow under the exact
new runtime. Compiler/spill, caller capacity and all simultaneously retained
families remain unqualified until that evidence exists. No simulation, puffing
remedy, soot progression or tier10 acceptance is authorized by this design.
