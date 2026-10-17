# r247 — original-region reaction and transport localization

Status: retained diagnostic design accepted at5f88dc2b2; synthetic source-moment arithmetic accepted after fresh round2 zeroP1/P2 reviews. Full selected Stage A frozen-source derivation now accepted; Stage B, new simulations and solver/boundary remedies remain pending.

The owner’s campaign authority and milestones remain in
[r244](FIRE_SMOKE_PUFFING_CAMPAIGN_R244.md),
[r245](FIRE_SMOKE_BUDGET_LOCALIZATION_R245.md) and
[r246](FIRE_SMOKE_TOP_LOCATION_SENSITIVITY_R246.md). The accepted top-only
sensitivity changes retained original-region observations, but does not establish
expected puffing dominance or a boundary defect. Soot and tier10 remain gated.
Original r246 exit98, separate recovered-data acceptance, unavailable observations
and no canonical taller qualification remain explicit.

## 1. Questions and retained evidence

Distinguish variation of a fixed central2×2 reaction probe from variation of the
spatial reaction source. Then measure the available original80-layer transport
and momentum response. These are localization observations, not causal proof.

`docs/data/fire_r245/r247_retained_operand_coverage.json` pins the accepted wide
and taller audit receipts and records a metadata-only inventory. Both cases have
15315 accepted scalar ledgers, but spatial capsules exist at only235 selected
steps6400..15315. Their complete original observation windows are retained.
Baseline central-column histories remain useful context; baseline spatial
beginning/native/flux capsules and signed side budgets are unavailable.

Each wide capsule contains540438368 bytes across beginning/native/flux packets;
each taller capsule contains1079308128 bytes. This inventory is not another
payload seal/root/arithmetic qualification. Consumers must bind each actual
input to its already accepted receipt and mandatory modern seal while deriving
the new quantities. They must not repeat the completed whole-campaign gates.

The producer’s wire layout is explicit in `PrepareR245Budget` in
`tests/FireSequenceTest.cpp` and native publication in
`src/Library/Utilities/FireProductionAdvectionMac.mm`:

- Beginning packet: binary32 Q0[9 cells], source increments[9 cells], M0[all
  faces]; five binary64 arrays (gas heat release, soot heat release, radiative
  cooling, pilot energy increment, reacted fuel); burner mask.
- Native packet: R1 accepted conservative candidate; R2 projected momentum,
  velocity and face density; R1 provisional momentum; actual Heun advection,
  buoyancy, stress and phase-source rates; scalar limiter alpha; R1 EOS
  temperature/pressure diagnostics; eddy viscosity and retained target fields.
- Flux packet: nine low-order and nine antidiffusive scalar flux arrays plus
  the recorded stage momentum rates, with exact input/native/candidate binding.

These are distinct owned stages. Frozen-source values are not terminal VDB
reaction fields; R1 temperature is not silently paired as a simultaneous
terminal field with R2 momentum. Q0/R1 O2 and CH4 are available local states,
but the complete scalar donor/upwind history is not retained. There is no
independent pressure-versus-EOS-restoration split.

## 2. Stage A — spatial frozen-source localization

Consume only the beginning packet and its binding/selected ledger for all235
wide and taller events. This first stage does not need native or flux payloads.
Retain accepted-step index, beginning time, binary32 dt, accepted end/event,
input/native/flux roots and source/case/producer identities on every row.

For gas-plus-soot frozen chemical source, export signed volume-integrated power
and separate positive and negative parts for native and original layers0..79.
Pilot/radiative/source-increment quantities retain their own units and names.
Use positive chemical-source weights for centroid, lateral spread and vertical
spread, explicitly distinguishing those weights from signed net chemical power.
Zero positive weight yields unavailable centroid/spread, not a zero coordinate.
Report integer cell-index moments and separately convert them with the recorded
binary32 solver dx. Do not substitute the authored binary64 scene/station dx.

Export the fraction of positive chemical source within the four central columns
and within the original80 layers, plus per-height source sums. Compare fixed
central columns with spatial totals and centroid movement across the complete
235-event window. Any sample averages, interpolation or lag summaries remain
descriptive; they do not constitute cumulative source energy through all15315
steps or a stationarity/uncertainty certificate. The accepted final global
source statistics remain the only full native cumulative reference here.

Initial local O2/CH4 summaries, if implemented, must name Q0 support and units;
mass fractions require the retained species-density denominator. Invalid or
zero denominator is refused or explicitly unavailable, never replaced with a
default oxygen/fuel value. No claim about all flux donors follows from Q0.

Stage A implementation gates must include hand-computable translated-source
centroids, exact-zero source, signed negative contribution, layer80 exclusion,
central-column fraction, species denominator and byte/identity/lifecycle REDs.
Shift exact constant operands before spectral detrending; never add epsilon to
create or hide a mode. New full-window derivation requires fresh independent
arithmetic/provenance reviews before physical interpretation.

## 3. Stage B — sampled original80-layer transport

After Stage A, use accepted native/flux/beginning operands for the same235
selected intervals to reduce Q0/Q1 inventories, frozen source increment and
limited scalar flux through all six surfaces of layers0..79. The limited flux
is the recorded low-order flux plus recorded alpha times antidiffusive flux.
Use recorded binary32 dx and dt for volume/area/step increments. Preserve signed
incoming/outgoing and source/closure residuals separately; do not widen any
roundoff or physical bounds to force closure.

For wide, z-face80 is the pressure-open native top boundary. For taller, the
same z-face80 is an interior transfer plane. Both have outward +z orientation
for the original region, but are different boundary classifications. Lateral
planes include only cells/layers0..79; whole-native taller lateral-area totals
are not substituted. Reproduce wide whole-native selected-step reductions as
a consumer arithmetic control using accepted ledger operands, not a rerun of
the full audit. Boundary/interior labels and bottom burner/nonburner masks must
survive output publication.

Momentum outputs retain actual stress, buoyancy, advection, phase source and
combined projection, with beginning/provisional/R2 terminal face momentum.
Use the already accepted central-column exports for the first common-height
comparison; a later all-face reduction must distinguish face averages from
cell-volume or boundary integrals and avoid counting shared faces twice.
Do not rename combined projection as pressure or restoration independently.

The selected intervals cover only235 of15315 accepted steps. Step increments
may be summed as **selected-step sums**, with their total covered dt, but no
interpolation/extrapolation may promote them to cumulative original-region
statistics. Sampling can miss dynamics between events; a new full instrumented
run is considered only if the available observations leave a precise missing
mechanism discriminant. Its design requires independent review first.

## 4. Admission, memory and publication

All work stays in the authorized worktree; logs, payloads and runtimes remain
external. Original producer files, exit98 and frozen identities are immutable.
The new reader/issuer and its exact executed implementation hashes stay distinct
from the original producer and prior snapshot/recovery issuers. Capture exact
reviewed audit/frame/ledger/payload/seal bytes, enforce regular nonaliased modern
seals and pending-publication refusal before/after capture, and report the same
consumed hashes. Bind loaded code/defaults/constants to captured source and
execute the captured strict modern Merkle verifier.

Before field allocation, admit exact dimensions, layouts, event identity and
sizes, then enumerate the actual reader buffers, derived arrays, verifier scratch
and result storage. Stage A reads one beginning packet at a time, releases its
views before the next case/frame, and does not copy all235 packets. A proposed
1GiB reader peak requires a complete reviewed allocation estimate and actual
RSS observation; it is not yet a measured limit. Stage B needs its own reviewed
peak estimate for all three packets and conversions. The original source host
ceiling34359738368/reserve23930269536 remains source provenance, distinct from
these reader working sets and from synthetic fixture reserves. Do not repurpose
the source reserve as a fabricated reader measurement. Retain existing native
owner/preflight gates and public2GiB fixture limits; no GPU owner is introduced
by this read-only diagnostic.

Inspect hostwide exclusivity before any full derivation. Only one heavy job may
run; full jobs are sequential. Controls are synthetic and explicitly nonempirical.
Outputs require a fresh owner-only root, captured-buffer publication, new modern
seals/receipt and full completion record; interrupted/failed results remain
failed. Independent interpretation reviews retain fixed-camera optical confounds,
central-probe support, nonlinear single-trajectory limits and no canonical taller
qualification. Only a localized mechanism and a discriminating supported design
can authorize a solver/boundary remedy under the existing campaign milestones.

## 5. Accepted source-moment arithmetic prerequisite

Candidate7e6878e7f passes fresh independent round2 arithmetic and contract/test
integrity reviews, recorded in `r247_source_moment_gate.json` and
`r247_source_moment_review.json`. The pure NumPy helper reads caller-owned
synthetic arrays only; it performs no native source admission or field reads.
The preserved v1 controls missed subnormal coordinate-product underflow and a
common fraction coupled to rounded physical power. Exact power-of-two weight
rescaling repairs moments without epsilon; common support uses dimensionless
weights, while rounded binary64 positive/negative power has explicit underflow
flags. Translated smallest-positive sources and volume-underflow controls now
discriminate both failures.

Next implement strict retained-source input admission and the Stage A reader.
The old wide audit pins native/flux roots and column hashes, but does not itself
list beginning-packet/frame-record hashes. New captured modern frame/ledger/seal
bytes must bind those consumed beginning packets, with this distinction explicit
in provenance. The accepted taller capsule receipt supplies exact beginning and
frame-record pins. The reader must account for captured packet, moment arrays,
Merkle verifier nodes and publication storage before fields, then observe actual
RSS. No completed audit or capsule gate is rerun.

## 6. Accepted selected-event metadata admission

Candidate6bcb3b170 passes fresh round2 contract/test-integrity and lifecycle/
provenance reviews. `r247_source_metadata_gate.json` and
`r247_source_metadata_review.json` bind the exact external v3 report, issuer
implementation and modern seal. Both235-event inventories bind captured
modern frame/selected-ledger bytes, exact binary32 dt/event addition, domain,
producer and accepted audit roots. All470 beginning-seal declarations are
captured; beginning payload bytes have not been read or verified by this stage.

Wide beginning/frame hashes are newly captured original modern declarations,
distinct from the old audit’s enumerated native/flux roots and columns. Taller
uses the accepted capsule receipt’s exact beginning/frame pins. Its original
frame CSV lacks an original sidecar; the accepted snapshot-reader modern seal
protects the consumed snapshot CSV, whose bytes exactly match the pinned
original CSV. This does not invent original producer authentication.

A malformed matching beginning digest was caught in round1 and repaired through
strict lowercase64hex validation of every frame SHA field. The v3 gate has
23 synthetic lifecycle/declaration REDs; previous metadata artifacts remain.
No native field packet, full audit, render, simulation or physical gate was
performed. Next implement the Stage A reader with exact v3 receipt/seal pins,
same-buffer modern beginning-payload verification and selected-ledger source
cross-checks. Enumerate packet, moment arrays, finite-check scratch, Merkle node
list and bounded output storage before allocation, and measure actual process
RSS. A reader result must preserve snapshot-stage and sampled-window limits.

## 7. Accepted Stage A reader composition

Candidatee8fe90721 passes fresh round2 independent arithmetic/test-integrity and
lifecycle/provenance reviews. `r247_source_reader_gate.json` and
`r247_source_reader_review.json` bind exact synthetic validation/runtime bytes.
Round1 caught a consumed certificate pin gap and an inaccurate finite-check
allocation inventory. Actual metadata/frame/ledger pair reopens now require the
consumed seal digest to equal the accepted pin; a real initialA/valid resealedB
callback distinguishes old unbound acceptance from repaired refusal. The
inventory includes the actual five-source boolean finite-check buffer.

The reader captures one beginning packet, validates its exact SHA and modern
Merkle seal, and derives source moments from those same bytes. Native selected
source sums are cross-checked against captured ledger operands using the
existing32-cell-count binary64 reduction enclosure. Original80-layer source
observations retain frozen stage and selected-event support. Direct buffer
estimates precede field reads; actual process peak RSS is separately checked
against1GiB. The original producer host reserve remains provenance only.
The new generated row-stream digest binds its captured publication bytes.

Composition acceptance authorizes one sequential full235-wide/235-taller source
derivation after hostwide exclusivity inspection. It does not accept empirical
results, physical inference, a full cumulative original-region budget or a new
simulation. Original exit98 and prior preparedfalse receipts stay unchanged.

## 8. Preserved first Stage A publication failure and reviewed repair

The original Stage A reader completed470 candidate rows but exited1 at
2026-10-04T17:54:33UTC before output sealing. Its source output, log, completion
and pending marker remain unchanged in external v1 directories;
`r247_source_spatial_failure.json` pins them. Row peak RSS was457048064bytes,
below1GiB. No failed result is accepted or relabelled successful.

Full `os.stat_result` equality incorrectly treats access time as mutation. An
independent same-SSD unchanged-byte read changed only atime and reproduced
this guaranteed failure. The actual before-stat/generated digest were not
persisted, so this identifies a sufficient mechanism rather than proving the
sole defect. Candidate2b90b99eb binds stable device/inode/mode/link count/size/
mtime/ctime plus the generated digest, excluding access time. Fresh round3
contract/test-integrity and lifecycle/provenance reviewers return zeroP1/P2.
The18-RED gate includes an actual forced-old-atime oldRED/newGREEN fixture,
content mutation and alias refusal. One separate repaired full derivation is
authorized after exclusivity inspection; empirical acceptance remains pending.

## 9. Accepted full Stage A retained spatial observations

The separate repaired v2 reader completed exit0 at2026-10-04T18:00:45UTC,
with235 wide and235 taller rows. Two new modern output pairs and all four
implementation identities pass fresh arithmetic/provenance reviews, recorded
in `r247_source_spatial_completion.json` and `r247_source_spatial_review.json`.
All470 exact event/source/producer/beginning links and1880 native selected-step
source reductions are independently checked. Actual peak RSS456998912bytes
remains below1GiB. The failed first v1 result and original taller exit98 stay
failed; the v2 candidate receipt’s false acceptance field is unchanged. This
separate review attests selected frozen-source data/arithmetic only.

An independent physical interpretation review records descriptive source
localization in `r247_source_spatial_physical_review.json`. All sampled chemical
source lies in original80 layers, concentrated near the burner. Sample mean
whole-source power rises from18101.493010W wide to18394.103334W taller, while
centralfour-column total falls from5336.132276W to5238.528824W. The central
fractions average0.301991883 and0.291875631. This identifies source outside the
fixed probe; it does not prove the cause of puffing or a defective boundary.

Centralfour **total** must be divided by4 for a column average. Historical
station data use gas-only column averages at8916 samples; Stage A uses
positive gas-plus-soot source at235 selected events, with a distinct wide
producer run. Their proximity is descriptive, not exact equivalence. Separate
mean fraction × mean whole power cannot replace the mean central power.
Zero sampled upper-layer chemical source does not exclude unsampled or
nonchemical upper-region dynamics. Fixed-camera optical confounds and single
nonlinear trajectory limits remain.

Next implement Stage B selected original80-layer signed scalar surface
transfers and actual momentum terms. Its larger native/flux/beginning working
set needs a separate reviewed allocation estimate and actual observation;
Stage A’s1GiB cap is not silently widened. Keep wide z80 boundary versus taller
z80 interior, selected-step sums versus unavailable complete15315-step
cumulative common-region budgets, and combined projection versus unavailable
pressure/restoration split explicit. No new simulation or remedy follows from
this Stage A acceptance.

## 10. Accepted Stage B scalar arithmetic prerequisite

`tools/fire/r247_region_transfers.py` reduces caller-owned binary32 Q0, R1
candidate, frozen source increments, low/antidiffusive flux and limiter arrays
for one selected interval. It forms total species from fields1..7 per cell
and per face before splitting incoming/outgoing, preserving counterflow.
Six outward orientations, original-layer lateral truncation, bottom masks,
exact promoted binary32 step measures and kg/J units stay explicit. The closure residual
is measured, never forced to zero. Native-top versus interior-plane labels
assume the campaign geometry contract; this pure helper authenticates no
producer boundary metadata.

Synthetic hand controls cover signed surfaces, nonzero closure, species
counterflow, limiter endpoints, layer80 exclusion, minimum-positive binary32 geometry and17 rejection cases.
The external frozen v2 evidence and code hashes are pinned in
`r247_region_transfer_math_gate.json`. Fresh round2 arithmetic/contract and test-integrity/provenance reviewers
return zeroP1/P2 for candidate1bc8c6760, recorded separately in
`r247_region_transfer_math_review.json`. This accepts synthetic scalar arithmetic
only: momentum arithmetic, exact source admission, separate Stage B allocation
inventory/RSS and the full new selected derivation remain pending. No producer
payload or accepted prior full gate is reread by these synthetic controls.

The initial scalar v1 candidate accepted arbitrary binary64 dx/dt. A review
found silent inventory underflow with dx1e-95 and uncaught geometry overflow
with dx1e308. Exact finite positive binary32 promotion now precedes measures,
matching retained solver operands without epsilon or widened bounds. Both
failures and nonrepresentable authored decimal dx/dt are discriminating REDs.
Original v1 evidence remains preserved.

## 11. Accepted central-face momentum arithmetic prerequisite

`tools/fire/r247_central_momentum.py` uses caller-owned binary32 M0, R1
provisional and projected R2 momentum plus four recorded actual Heun rate
arrays and their paired stage operands. Selected central-face rates must match
the producer binary32 averaging operation tree bit for bit, including signed
zero. Overflowing binary32 stage sums raise an explicit refusal from the actual
rounded operation. The helper
preserves advection, buoyancy, stress, phase source and combined projection;
its measured closure follows the producer named-term order without epsilon.

Each axis collects unique faces bounding the fixed central2x2 columns. For
common80, x/y each have480 faces and z has324, totaling1284 unique faces
versus1604 column associations. Shared faces contribute once. Vertical
profiles include zface80, while common lateral support excludes layer80.
Outputs are signed and absolute face means with kg/m2/s or kg/m2/s2 units,
never cell-volume or boundary integrals. Native versus interior upper-plane
classification assumes the campaign domain and remains unauthenticated here.

Six hand controls and17 operand/stage/support REDs are frozen and pinned in
`r247_central_momentum_math_gate.json`. Fresh independent round2 arithmetic/contract and test-integrity/provenance
reviews return zeroP1/P2 for candidate de57ff923, recorded in
`r247_central_momentum_math_review.json`. This accepts synthetic arithmetic only;
strict source/packet admission must authenticate the same consumed operands
before any empirical derivation. Separate Stage B allocation/RSS admission,
full transport/momentum derivation and physical interpretation remain pending.

The first momentum candidate falsely refused a real sum above binary32 max
that still rounds to finite binary32 max. The repaired helper classifies the
actual producer operation, converting overflow exceptions to explicit refusal.
A near-maximum finite GREEN and genuine-overflow RED discriminate this without
epsilon. Frozen v1 evidence remains preserved; v2 is the reviewed candidate.

## 12. Accepted Stage B reader composition and proposed memory envelope

`tools/fire/r247_transport_reader.py` admits only the exact accepted v3 metadata
pair and full235-wide/235-taller source domains through its empirical command.
Each consumed frame and selected ledger retains exact accepted byte/seal pins.
All three packet sizes and captured modern declarations precede any field read;
body verification and consumer derivation use the same captured bytes. Native
and flux Merkle roots, input/candidate/owner headers and central-column source
operands/clocks/eddy/closure are explicitly cross-checked. Column certificate
bytes are captured and pinned before the accepted modern pair loader runs.

The captured scalar and momentum helpers derive common80 signed transfers and
unique central-face summaries. Wide whole-native selected-step scalar reduction
uses200 comparisons against the same retained step ledger, preserving the
existing32C /32C+32F /64C+64F reduction enclosures. This is a consumer control,
not a repeat full audit. The full command has no fixture mode. Synthetic
2x2x80 wire and lifecycle controls are separate and explicitly nonempirical.

Stage B has its own2GiB reader peak ceiling; Stage A's1GiB ceiling is unchanged.
Before each frame, actual current process RSS from macOS ps is added to a
separate allocation inventory. Measured process peak RSS uses native resource
units and is checked after packet consumption and final output capture. The
inventory includes all three captured packets, one largest-packet resize
reserve, two inventory conversions, per-plane scratch, sequential finite-check
booleans, Merkle nodes/chunks, bounded metadata/columns/Python objects, central
rows and final publication storage. With a synthetic64MiB baseline the taller
estimate is1887791976bytes; this is not an actual native-reader measurement.
The source34GiB ceiling and23.9GiB reserve remain provenance only, and the
public2GiB per-packet ceiling remains.

The reader captures and executes exact pinned helper/verifier source, compares
its loaded code/defaults/constants with its own captured source, and reports
all six implementation identities. Stable generated-stream capture excludes
atime while binding storage identity and generated bytes. Exclusive outputs
carry a distinct new transport issuer; original producer files/exit98 and prior
false receipts are immutable. The31-RED gate includes later-packet size refusal
before any beginning read, actual wire header/column/ledger corruption, source
certificate pin changes, loaded-constant mutation and output aliases.

`r247_transport_reader_gate.json` pins frozen v1 controls and archive copies.
Fresh independent round1 arithmetic, lifecycle/provenance and memory reviews
return zeroP1/P2 for candidate9e5974a06; `r247_transport_reader_review.json`
accepts composition and authorizes one sequential full new470-row derivation
after hostwide exclusivity inspection. The memory estimate is a reviewed
prospective envelope, not empirical allocation acceptance. Empirical peak RSS, complete outputs
and physical interpretation remain unaccepted; no new simulation or remedy
follows from implementation review.

## 13. Preserved first Stage B refusal and accepted local-index repair

The first full reader exited1 at2026-10-04T19:28:35UTC after one wide row,
refusing frame1 column observation index. External `r247_transport.v1` and
`r247_transport_launch.v1` remain failed, unsealed and pending. Their exact
bytes are pinned in `r247_transport_failure.json`; no result is promoted.

`PrepareR245Budget` passes literal0u to `FireProductionStaggeredColumn::Write`
at every frame. That exported observation index is local to the one retained
observation, while the global ordinal/event is already bound by the exact
frame record, selected ledger and payload roots. The new reader incorrectly
required this local index to equal the global frame ordinal. Actual frame1
columns contain0, confirming this sufficient refusal mechanism; this does not
prove absence of other defects.

The repair requires the producer's exact local0 index and retains every
global frame/event/clock/root pin and actual operand check. A second-event
synthetic fixture keeps local0: the preserved failed reader refuses, while
the repaired consumer passes. Local-index1 remains RED. There is no source
edit, retiming, tolerance widening or relabelling. Fresh round2 arithmetic/contract and lifecycle/provenance/memory reviewers
return zeroP1/P2 for candidate f0d20c88e. The separate
`r247_transport_reader_repair_review.json` authorizes one repaired full
derivation after exclusivity inspection; empirical results remain unaccepted.

## 14. Stage B v2 memory refusal and private captured mappings

The separate v2 run failed before its first taller packet after all 235 wide
rows. Its unsealed pending output and exit1 are preserved; the memory failure
receipt pins the seven original artifacts. Last-wide current RSS was
614105088 bytes and peak RSS616398848; the exact failure-site RSS was not
persisted. The unchanged2GiB admission refused current RSS plus the taller
increment. This does not identify live references versus allocator retention.

The candidate reader streams each packet into an unnamed exclusive private
file with 65536-byte scratch, retains an ACCESS_READ mapping, and closes the
writable file descriptor before SHA/Merkle verification and all consumers.
It never maps mutable source files. All three source declarations precede
capture, and exact same-buffer seals and source lifecycle checks remain.
Mappings close after consumers; exception traceback frames clear before closing
NumPy-backed mappings. The inventory still counts all three resident payloads,
all existing conversion/finite/Merkle/output costs and private mapping overhead.
It replaces full-packet heap resize reserve with bounded copy scratch, without
changing StageB2GiB, StageA1GiB or public per-packet2GiB caps.

Synthetic controls prove heap/mapped exact derived arithmetic and Merkle parity,
read-only mutation refusal, live-view close refusal, explicit owner release,
source/hash/seal mutations and preflight-before-any-capture discrimination.
No actual mapped native derivation or measured peak is accepted by these controls.
Fresh independent implementation/provenance review is required before a separate
full run; the previous failed outputs remain untouched.

## 15. Complete selected Stage B data and remaining causal gap

The separately reviewed mapped v3 run completed exit0 at
2026-10-04T19:58:10.746523UTC, all235wide+235taller events. Fresh independent
arithmetic/contract and lifecycle/provenance reviews found zeroP1/P2; an
independent physical review accepts descriptive sensitivity only. All470event
links,940frame/ledger modernpairs,1880payload receipt links,6runtime identities,
2output modernpairs and47000wide arithmetic checks were verified. Measured
peak1163493376 bytes is below the unchanged2147483648-byte StageB ceiling.
Original v1/v2 failed outputs remain preserved. The reader receipt's false
acceptance fields remain unchanged; the separate review accepts selected data
and arithmetic only, with original taller exit98 and no canonical qualification.

Both variants cover0.3695923899067566 seconds of selected accepted intervals,
not the full15315-step history. Selected common80 upper-plane net species
transfers are0.20717184520068943/0.31089023967599183 kg; sensible-enthalpy
transfers5441.413043089011/5507.7404453436175 J (wide/taller). Dividing selected
sums by covered dt yields upper mass rates0.560541426/0.841170566 kg/s and
lateral net outward rates−0.560120822/−0.840406196 kg/s. Increased throughflow
is largely balanced by lateral entrainment; this does not diagnose a defect.
The widez80 plane is a pressure-open boundary and tallerz80 is interior.

Central common-height vertical-face means retain separate equal-event and
dt-weighted labels in the physical review. Dt-weighted signed projection
means0.003003/0.004285 kg/m²/s² coexist with absolute means3.738237/3.763980;
equal-event signed means0.24753/0.23316 use a different sampling measure.
Signed cancellation does not make local projection negligible. These means
are neither boundary integrals nor whole-region momentum budgets.

No supported puffing mechanism or solver/boundary remedy follows. A next
reviewed diagnostic design must resolve full accepted-step original80 scalar
balances, spatial reaction/oxygen/fuel delivery, and whole-region plus central
momentum histories, including independently measured pressure and EOS-restoration
corrections and relevant limiter donor states. Phase-resolved matched-case and
repeated-seed controls are needed before attributing reaction oscillations to
entrainment/backflow or projection. No new simulation or remedy is authorized
by this interpretation alone; the existing campaign requires fresh design,
implementation, arithmetic/provenance and physical review gates first.
