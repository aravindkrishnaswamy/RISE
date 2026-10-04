# r247 — original-region reaction and transport localization

Status: design candidate; no new simulation or solver/boundary remedy.

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
