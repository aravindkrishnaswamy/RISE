# r248 native preconstruction resource probe — incremental implementation

The fixed-size `CaptureFireProductionPreconstructionResources` API reads an actual
owned frozen-source seal by const reference. It reports eleven vector sizes and
capacities, checked element/capacity byte sums, and actual host/device telemetry.
It creates no source copy, owner, field buffer or owner compiler context. Metal
device discovery can create driver resources and a diagnostic string; this is
not a claim of allocation-free execution or a measured complete scratch bound.
Host samples follow device discovery/pool drainage. Values are sequential samples,
not an atomic simultaneously-live allocation inventory.

`sourceLayoutKnown` requires the sealed flag, positive checked geometry/cell width,
uint32 cell-count representation, exact source vector lengths and nonoverflowing
byte totals. It does not validate source packet authenticity, case identity,
chemical values or allocation ownership beyond the actual supplied object.
Capacity bytes describe existing vector element storage; they exclude strings,
object/allocator overhead and all other resources. They do not predict allocation
capacity of a future copy. Invalid layout publishes unknown byte totals with its
availability flag, not an admitted zero-byte source. Telemetry has separate host,
peak and device availability flags; an observed zero device allocation can be
valid while queried device limits must be positive.

The real `AttemptFireProductionProjectedHeunMetalOwner` entry has a qualification
selector that captures this observation and always refuses before construction.
An optional source **element** limit demonstrates refusal of even a copy's payload
lower bound. A larger limit does not admit construction. A limit without the
probe also refuses. Production `AttemptFireProductionProjectedHeunResidentStepMetal`
rejects both selectors before invoking capture. Ordinary/default-off execution
uses the existing owner path; full exact state/golden parity remains unqualified.
No canonical serialization or kernel arithmetic is changed. The new diagnostic
struct is outside existing canonical publication payloads.

This implements a native preconstructor observation/refusal seam. The const API
can be used before caller request copying, but the campaign caller is not yet
wired to it and no complete caller or native allocation admission exists. It does
not implement new resident observer buffers, readonly authority hooks, source
receipt authentication, compiler/spill inventory, live allocation IDs/release
credit, full ordinal history or full-run capacity qualification.

The focused `tools/fire/r248_preconstruction_probe.cpp` uses the real public source
factory for cold ambient4x4x4,6x8x5 and8x8x8 fixtures. These are source/storage
controls, not simulations or substitute campaign tiers. It checks reported actual
sizes/capacities, real independent seal copies, unchanged canonical source seals,
valid telemetry, probe/one-byte/production/selector refusal and empty-source
unavailability. Every owner probe result must have no accepted publication,
conservative payload or command commit. The actual constructor seam is inspected
independently as well; zero result counters alone are not execution proof.

The standalone fixture retains the existing separate256MiB synthetic peak check,
not the public2GiB fixture/source ceiling/StageA/StageB policies. Observed harness
RSS/device data cannot establish any production owner peak. Initial and subsequent
build/test logs remain external; source/runtime identity is the new dirty build,
not the original failed source producer. Sanitizer compilation found an existing
unused error parameter in the internal EOS table pack helper. That dead parameter
and all its callers were removed; numerical table packing and kernel source were
unchanged. No old numerical gates are rerun or relabeled by this increment.

NEXT install bounded actual caller ingress measurement/refusal before request
copies, then complete native/context/stage/command/adapter/publication resource
inventory and real controls before new observer buffers. Keep resource refusal
separate from numerical acceptance and preserve all existing host/device gates.
The accepted full beforeconstruction resource design remains the mandatory scope;
this probe does not discharge its complete measured admission requirement.

R1 found a diagnostic allocation failure outside the owner recovery boundary. The repaired qualification branches execute inside the existing autorelease/recovery boundary. Before any allocating reset, reused results lose accepted authority and publication identity/root. Recovery guards both result reset and diagnostic assignment; the production qualification refusal likewise guards these allocations. Four additional controls inject real `operator new` failures at reset and diagnostic publication in both public entries. This is a refusal-path control, not a production memory-pressure qualification. Original v3 evidence and R1 findings remain separate.
