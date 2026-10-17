# r239 — restore the prescribed burner boundary, once

Status: conformance implementation in progress. No corrected-run result
or empirical pass is claimed. The amendment/source audit preceded the code.

## Raw-resident projection operand increment

The raw resident projection seam now borrows an immutable Private buffer
containing the float mask followed by prescribed mass flux. It neither
reconstructs nor uploads that operand between stages. Extent, storage mode,
device, presence and alias restrictions are checked before use. Caller-side
qualification owns payload validity, as with the existing raw resident
density/momentum inputs; this is not an authenticated authority surface.
Authenticated burner owner calls still refuse until the existing case-input
root binds the operands and the flux/source integration is complete. The
earlier blanket resident refusal described below is narrowed only this far.

Actual allocation accounting includes the borrowed operand exactly once;
the existing optional certificate remains conservative because the resident
path does not allocate the comparator's upload/packed host copy. No cap,
enclosure, arithmetic kernel or no-burner schedule changes in this increment.

`tools/qualify_fire_resident_burner_projection.mm` qualifies a 7×5×4 analytic
zero-pressure input with four asymmetric inlet patches, heterogeneous density,
and opposed provisional momentum. All published fields equal the CPU result
with zero pre/post residual. Changing host flux placeholders has no effect;
removing the device inlet payload breaks that solution (RED), demonstrating
that the private operand, not host reconstruction, feeds the solve. Missing,
Shared, short, oversized and unexpected operands refuse; unwired owner-token
mode refuses. Reported interstage reads are zero and actual allocation is
below the unchanged optional certificate. Other-device execution and general
restoration-oracle coverage remain pending. The later review-fix increment
below adds the specific nonzero-target placeholder-independence regression.

Focused projection suite and standalone raw-resident qualification both exit
zero. Incremental rebuilt files have no warnings; full clean/Opto and live
owner qualification remain pending. Reproduce the native harness after the
normal focused projection build with:

```sh
clang++ -std=c++17 -O2 -fno-fast-math -ffp-contract=off -fobjc-arc \
  tools/qualify_fire_resident_burner_projection.mm \
  src/Library/Utilities/FireProductionProjection.o \
  src/Library/Utilities/FireProductionProjectionMac.o \
  src/Library/Utilities/RISECBOR64.o \
  -framework Metal -framework Foundation -o /tmp/r239-resident-projection-check
/tmp/r239-resident-projection-check
```

Logs retained outside git in the r239 calibration directory:

| Artifact | SHA256 |
|---|---|
| resident-projection-build.log | aae1d3806c0540c2109fa64e279710fcbdb1ac85bb30f6671f840f2b0dffe77a |
| resident-projection-suite.log | 42d0de013e06ca95eb2d0e1f5ad492d1a0d4fca0a19d3866463ec65241e3ed6f |
| resident-projection-test.log | 8e0f63c63426f16ae28c1e601297559defec46343ef18462491f182f703de1a0 |

Review of `f8c9c77a`: arithmetic/test integrity found no P1/P2. The safety
review found one adjacent P2: density, target and separate momentum buffers
could belong to another device even though the burner/first momentum pair
was checked. Admission now checks every borrowed projection operand against
the context device, including no-burner calls. Conditional REDs independently
replace each operand on multi-device hosts; this M4 exposes one device, so
those REDs explicitly report **not exercised**, not pass. The focused suite
and raw-resident harness pass after the fix, with no build warnings.

| Post-review artifact | SHA256 |
|---|---|
| resident-projection-reviewfix-build.log | c56738c1b8320773a6e151380e196bb569069e43fa813a8464a0d394dcfee79e |
| resident-projection-reviewfix-suite.log | 64dc115718cb5f498477069f24619df0159d022ecf2773a88a37d2b86adb9ffe |
| resident-projection-reviewfix-test.log | 8f180bc586aa8b246eae4e9581a1afb032c5cf4e5893eefd64ee6f243900fb1d |

Fresh post-fix review of `df4f8e45` closed device membership and found one
further adjacent P2: restoration solved against its resident target but sized
its validation band from host target placeholders. A new regression fails on
that commit when only the host target is zeroed. This could both spuriously
refuse and improperly admit a result, depending on the host placeholder.

The fix computes `max(abs(device_target))` using the existing device reduction
and scratch storage, returning one scalar in the existing diagnostic buffer.
The derived band remains exactly `0.005 * maximumTarget`, in s⁻¹. It is the
measurement source that changes, not the bound. Terminal and state-only
restoration now have identical bands/verdicts with unchanged resident operands
and changed host placeholders; terminal fields also agree. Interstage reads
remain zero. Physical projection does not dispatch the additional reduction;
its schedule is unchanged. The reduction copy kernel gains an absolute-value
mode, so the executable's kernel identity changes and future runs are from
zero, not migrated. No corrected window has been launched.

The native harness and focused projection suite both exit zero after completed
warning-free incremental builds. Nonzero-target restoration coverage now
includes the host-placeholder independence check above; it is not a general
restoration oracle qualification. Full clean/Opto and authenticated owner
integration remain pending.

| Restoration validation artifact | SHA256 |
|---|---|
| restoration-target-red.log | 5b4810d943e707ca2edf516a7bebf35707ebf8eb4312e71c67a0da77c25a0de5 |
| restoration-target-build.log | c56738c1b8320773a6e151380e196bb569069e43fa813a8464a0d394dcfee79e |
| restoration-target-suite.log | 62fc87ee4df4e2f69ceae6dd31ee090224635d7d93aafd342bd4229ebd7a8d99 |
| restoration-target-test.log | e13a052e681152e8244319b2a994845b239e87d6304c331c996cd88aacd0262e |

Fresh review of `7b93784e` found no P1/P2 in the corrected reduction,
diagnostic lifetime, shared restoration variants, or regression integrity.
Across the three rounds, two adjacent P2 findings were fixed (device
membership and restoration measurement source); no P1 was reported.
Sibling audit: both Metal restoration entry points use the repaired shared
implementation. CPU, generated fp64 and generated trace restoration derive
the scale from the same request target their solve consumes, so that
host/device-placeholder mismatch does not apply there. Publication retains
the solve's diagnostics rather than rebuilding the scale. This closes the
raw adapter increment only, not the pending authenticated owner/flux/source
integration, complete force suite, or clean/Opto qualification.

## CPU projection increment (reviewed, not the complete port)

The CPU projection now accepts explicit bottom mask/flux and injected gas
density, with a wall-class bed and an open outlet required. It installs the
prescribed velocity before constructing the RHS, preserves it at correction,
uses the arithmetic injected/interior face density, and retains the existing
bed coefficients at all levels. The restoration path preserves the same fixed
boundary while validating its correction divergence. Added request payload
bytes enter the CPU admission's existing working-set limit.

At the CPU increment, Metal rejected nonempty burner requests explicitly.
The qualification-only device increment below supersedes that refusal for
host-input comparisons; resident owner requests remain refused. The live
owner is not wired to these operands yet. This is not a production fix or a
performance claim.

Focused `FireProductionProjectionTest` passes after a completed warning-free
incremental build. New tests include opposed provisional momentum, fixed
zero flux, invalid mask/unmasked flux, exact empty-mask collapse, and the
live fp64 oracle's burner-face density/velocity rounded to binary32. The old
pressure-open mutant fails the prescribed momentum identity. These are not
a full filtered-field owner contract or an all-face oracle comparison.

Two stale source checks were repaired by named scope: the sealed class/head
metadata upload adds exactly one command and two private copies to the old
census; Xcode's existing Metal file flags additionally include `-fobjc-arc`.
Neither change relaxes an arithmetic tolerance or a residency requirement.
Full clean/Opto and the complete burner implementation remain pending.

First independent review: no P1, one confirmed P2. A restoration request
with sealed nonzero physical boundary heads omitted them from its correction
RHS but reused them in final velocity correction. CPU and Metal now both use
homogeneous correction heads at that final consumer. A zero-target masked
burner test with nonzero physical heads has exactly zero correction residual
and preserves prescribed momentum. Focused tests pass again; two fresh
reviewers of `be07fdd8` found no P1/P2. This kernel edit also prohibits resuming historical
production runs under the changed executable.

The reviewer also noted a pre-existing representational difference: unmasked
wall faces store interior density in production versus an ambient/interior
average in the live oracle. Their velocity is fixed zero; this increment
does not claim all-face stored-density equivalence. The later compatible
momentum/owner gate must resolve any downstream consumption before claiming
full boundary conformance. No oracle comparison is silently broadened.

Scope remaining for the Metal/owner increment: executable nonzero-head Metal
restoration regression, nonzero-target heterogeneous burner restoration,
malformed-operand wrapper refusal tests, and neighboring-field oracle checks.
Raw logs are outside git under
`rendered/fire_production_calibration/r239_burner_projection/`:
focused-test SHA256
`4dcba653056e48ef125a868602f84af0f7c05338c6c91a0f30d53135f75f0881`;
build SHA256
`4ef9c53355538adfe770a29c8357b3f3dbb1c473330c0e598bc9f6bd1aad23bd`.

## Evidence and scope

### Analytic burner RHS regression

The focused suite now includes a 7×5×4 heterogeneous-density field with
four masked bottom faces (including corners and a zero-flux patch). The
authored target is exactly the divergence of the prescribed velocities:
`S[cell] = -(j/rho_face)/dx` on those bottom cells and zero elsewhere.
All other provisional face momenta are zero; masked provisional momenta
are deliberately reversed. Thus pressure and every non-burner velocity
must be exactly zero, not merely close to a second implementation.
CPU and native Metal satisfy that solution with zero pre/post residual.
This tests physical projection, not heterogeneous restoration.

A temporary Metal mutant removed only the prescribed-face branch in
`setup_velocity`, leaving final correction's fixed-momentum overwrite
intact. The analytic test refused with pre/post residual 0.625 s⁻¹;
restoring the branch made the suite pass. This distinguishes a correct
RHS from an endpoint-only clamp. Additional native tests cover masked
zero flux and all-zero-mask/absent-mask equality of published fields.
Those equality checks compare floating values, not signed-zero bit patterns.

No production kernel or owner semantics changed in this test increment.
Resident burner operands, their owner input-digest binding, and the
scalar/source split are still pending. No new run has been launched.
Focused rebuilt files were warning-free; full clean/Opto remains pending.
Logs are retained outside git in the r239 calibration directory:

| Artifact | SHA256 |
|---|---|
| analytic-final-test.log | 14c1dc943b1bfc858febec942d96650af7ab8210f37148d07ae5f114abd432b9 |
| analytic-red-test.log | cd3177efa6468cfeb70449c3c4aa106dcf297fa0d9e8914d7a9e6e14db919a9e |
| analytic-final-build.log | c56738c1b8320773a6e151380e196bb569069e43fa813a8464a0d394dcfee79e |

Two fresh read-only reviews of `9d25672b` (oracle/test integrity and
numerical derivation/scope honesty) found no P1/P2. Neither review expands
the qualification beyond this physical-projection regression increment.

### Device arithmetic increment (reviewed, resident integration pending)

The nonresident Metal comparison path now uploads one packed mask/flux
operand buffer into device-private storage before the solve. The fine face
density, provisional velocity and final correction kernels use the prescribed
bed; the restoration residual's beginning velocity uses that same boundary.
The existing all-level wall coefficients and open-head exclusion are unchanged.
No-mask requests allocate no burner buffer and branch away from its placeholder
binding. Level metadata grows from 88 to 96 bytes, within the same allocation
quantum. A conditional working-set overload counts both rounded buffers,
packed host data and original mask/flux arrays; the original no-inlet
certificate and its exact cap-boundary tests are unchanged.

The native focused suite passes with the opposed-momentum device burner test
and existing residual gate; the optional-byte delta is tested exactly. This
does not qualify arbitrary resident caller buffers: nonempty burner requests
with resident input are refused until the owner's immutable operands and
publication identity are wired. No per-step authority or hashing mechanism
has been introduced. Remaining work includes resident identity/operand reuse,
full CPU/device comparisons and the source/flux/target integration.

Two fresh reviews of `9d3db44d` found no P1/P2 in kernel mathematics/bindings
or allocation/transfer safety. They explicitly leave broader field comparison,
device zero/empty-mask cases and executable resident-refusal coverage pending;
the result is not a complete owner qualification. Preserved focused native
test log SHA256
`b6ef7ead5e2f287640133b1e3b45c93b4f872891bf0f7470d15a78ae195c8f6b`;
build log SHA256
`4ef9c53355538adfe770a29c8357b3f3dbb1c473330c0e598bc9f6bd1aad23bd`,
under the same external r239 log directory. No full-window run is active.

r238's accepted R1 budget at retained tier-10 step 6489 localizes
8.2734249337153135e-5 kg/s of outward methane transport to the 120 canonical
source-footprint faces: 95.2676% of bottom export at that observed step.
Budget SHA256:
`d9014ea355e3c17037a5c2448479957530e81a75a481d97471c082c1cdb69ed1`.
Eight diagnostic accepted-state payload roots match the prior observation;
this is not an empirical continuation or a full-window flux integral.

The implementation audit identifies a boundary-construction mismatch, not
an EOS tolerance defect. `FireSequenceTest.cpp` installs the oracle's
`bottomFuelMask` and prescribed flux from the canonical source pattern.
Its production branch instead adds fuel to bottom-cell Q before constructing
the reaction packet, sets initial bottom-face momentum/velocity, then passes
an all-zero fuel mask and all-pressure-open sides to the resident owner.
Those starting face values are not a constraint on later projections.

The gap extends beyond the launcher: the resident physical-flux request has
no injected tuple or per-face prescribed mass-flux operand, and its kernels
implement wall/open/seam scalar fluxes. The resident projection request has
side-level boundary kinds but no per-face prescribed burner flux. Therefore
enabling the existing transport mask alone cannot implement the oracle
boundary. The transport mask currently accepts nonzero entries only on a
wall-class bottom side; changing that admission alone is also insufficient.

## Chosen formulation

Conform to the existing live oracle's prescribed inlet; do not tune heat
release, chemistry, domain dimensions, velocities, or empirical thresholds.
The bottom is the oracle's adiabatic bed, with prescribed inlet exceptions
on the masked faces; lateral/top faces remain pressure-open. Keep the same
source pattern and nominal fuel rate.

**Audit correction before promotion:** the first r239 draft incorrectly
said the oracle's unmasked bottom was open. `OpenBoundaryConfig3D` first
fills all sides open, then sets `kind[4]=AdiabaticWallBoundary3D`.
`ValidateOpenBoundaryConfig3D` explicitly refuses a fuel mask on a non-wall
bottom. Reading only the fill/default missed that override. The port must
conform the entire bed, not invent a mixed pressure-open/burner bottom.
The initial uncommitted CPU prototype was corrected accordingly; no run or
production promotion used that prototype. This also removes the need for a
new mixed-mask multigrid coarsening scheme: the bed's pressure coefficient
is zero on every level, while the fine-grid imposed velocity enters the RHS.

Let j be the nonnegative prescribed inward mass flux in kg/(m² s), Q_in
the injected conservative tuple, rho_in its constituent total density,
and s the outward-normal sign (-1 on the bottom). The prescribed scalar
outward tuple is F_out = -j Q_in/rho_in. Both low and high candidates carry
this same tuple; their antidiffusive difference is zero at that face. The
limiter must not attenuate the imposed source or replace it with ambient.

For the divergence-target split, use the oracle's existing identity:
f_N,out = F_out - u_out Q_cell. The interior carrier already participates in
the tangent projection. Using the full imposed tuple as f_N would count that
carrier twice. Validate imposed tuple, carrier, and difference separately,
with local termwise arithmetic bounds rather than relative cancellation
tests; their sum alone is not the test.

Projection uses the live `ProjectPressureOpenMACVelocity3D` and
`ProjectPressureOpenMACVelocity3DFinal` reference, not the historical
`ReferenceProjectPressureOpenMACVelocity3DEliminated` helper. The live paths
use rho_face = (rho_cell + rho_in)/2, hold M_normal = -s j, and hence
u_normal = -s j/rho_face at the burner. The historical eliminated helper
instead divides by rho_in; importing it would silently change the reference.
Prescribed faces have no pressure correction coefficient and no open-head
unknown, but their fixed velocity contributes to the divergence RHS and
post-projection validation. Masked faces must also be excluded from open
class/head updates and preserved through R2 and compatible momentum sweeps.
Lateral/top open faces retain the current active-set and head treatment;
unmasked bottom faces have zero normal velocity and scalar flux.

Fuel enters exactly once: remove volumetric fuel staging for the conformed
ported path when the imposed boundary is active. Build the frozen chemistry
packet from that step's actual Q0, following the oracle schedule, not from a
second fuel-enriched copy. Retain reaction/pilot/radiative source terms.
Accumulate accepted imposed injection in the source ledger only on acceptance;
a rejected trial contributes nothing. Keep the ordinary legacy path isolated
if still needed to reproduce historical evidence. Case-record semantics and
binary hashes must identify the changed production source/boundary schedule.

## Implementation boundaries and gates

### Prescribed scalar-flux comparator increment

The four physical-flux producer/composition/validation kernels now implement
the prescribed inlet split in the qualification comparator. Its existing
Private ambient operand contains `[ambient9, maskN, jN, injected9]` when the
optional extent is present; the diagnostic payload identity includes all
words. The injection division and carrier subtraction are device operations,
not CPU-produced intermediate fluxes. A retained carrier product prevents
FMA contraction from giving the physical term a different rounding path from
the separately published donor. The appended input reuses the existing
upload/private pair; the optional working-set overload counts its expanded
rounded allocations. No-inlet arithmetic is unchanged; the review correction
below repairs both inlet and no-inlet allocation inventories.

The comparator checks four asymmetric patches on a 4×5 bottom against the live fp64
`BuildOpenBoundaryFlux3D` reference, with the outward sign converted to the
positive-z storage convention. One patch has zero prescribed flux; zero and
negative carrier velocities exercise the split independently of the live
projection's imposed inward velocity. Nonzero interior carbon distinguishes
the six-gas sum from the seven-constituent density. The
injected density is exactly 1/2, so its sum has no rounding error in this
fixture. For each component and face, the donor enclosure is
`gamma(1)*abs(u*Q)` and the physical enclosure is
`gamma(4)*(abs(imposed)+abs(u*Q))`, covering the multiply/divide, carrier
multiply and subtraction separately. Here gamma uses binary32 epsilon as
the existing r198 comparator does. Low/high imposed values must equal the
dyadic reference exactly; the advective delta must be zero. This derivation
qualifies these matched inputs, not all possible injected compositions.

The injected sensible energy is a tiny dyadic value against the hot-cell
carrier. The test verifies that recomposing `donor + f_N` loses bits, while
the directly published imposed tuple remains exact. The high-candidate
mutation is refused on an inlet as well as in the historical fixture.
Unmasked flux, short extent, zero constituent density and non-wall bed
refuse; independently changing the injected tuple, flux or mask changes the
publication identity. Gas flux is compared separately with the oracle's
components 1–6, with local bound `sum(component_bounds) +
gamma(5)*(sum(abs(reference_components)) + sum(component_bounds))`.
EOS/owner admission for prescribed-flux requests still explicitly refuses:
this increment cannot accidentally enable half-integrated live physics.

Per-field worst residual/local-bound ratios for the four inlet samples:

| Field | Donor ratio | Physical f_N ratio |
|---|---:|---:|
| rhoZ | 0 | 0.106322 |
| CH4 | 0.316253 | 0.054173 |
| O2 | 0 | 0 |
| N2 | 0.188761 | 0.047190 |
| CO2 | 0.326584 | 0.081646 |
| H2O | 0.492836 | 0.123209 |
| CO | 0 | 0 |
| carbon | 0 | 0 |
| sensible energy | 0.277154 | 0.069276 |

The separate gas physical-flux ratio is 0.050956 (kg m⁻² s⁻¹ on both sides).

Mass rows use kg m⁻² s⁻¹; energy uses W m⁻². Raw output records each field's
maximum residual and its corresponding local bound in the same units, plus
the independently accumulated worst local ratio. No across-field maximum
is used to admit a field. Imposed-tuple residuals are zero for every field.
The existing diagnostic label order was corrected to the actual species
order (N2, CO2, H2O); arithmetic and historical evidence are not rewritten.

Live owner ingress, projection/force mask connection, accepted source ledger,
general injected-mixture qualification and clean/Opto
remain pending. No corrected production run has been launched.

The final completed build is warning-free incrementally; the existing
`--fire-production-metal-fp64-kernel-sweep` exits zero, including the new
inlet comparisons and historical owner/target/compatible-momentum gates.
Generated fp64/trace mirror checks pass. An earlier invocation started before
its build-exit confirmation and executed the older binary; that output is
excluded from qualification. The recorded sweep below was launched only
after confirmed build exit zero. Logs remain outside git in the r239
calibration directory:

| Artifact | SHA256 |
|---|---|
| scalar-flux-build.log | 7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21 |
| scalar-flux-kernel-sweep.log | c1ecc6fbd5a300ff43fca0eace177cc74c89d4bedebd10e66ad8c522d4ac8e0e |

### Scalar-flux review correction

The first independent math review found no P1/P2; the allocation review
found a P2: the comparator's actual-byte sum omitted the separately allocated
endpoint-class identity, and the rounded certificate grouped three private
control words into one allocation. Confirmed and repaired in the physical,
EOS and target comparator family. The owner measures the device allocation
peak independently and does not share the subtotal omission. The common
certificate now counts one shared control upload, three separate private
controls and the endpoint identity: exactly three additional 16-KiB quanta
per retained set, or `4 * 3 * 16384` bytes in the owner's four-set derivation.
The 2-GiB comparator cap and device working-set admission limit are unchanged.
The owner census test explicitly derives this increment; its first run
correctly failed with the obsolete census rather than hiding the change.

A separate physical-comparator inventory sums the actual retained buffer
handles independently of authority subtotals and demands exact equality.
Removing the endpoint term reproduces `allocation inventory disagrees` and
sweep exit 203; restoring it gives full native sweep exit zero. This is
qualification-only checking, not extra production instrumentation. The
certificate test also crosses the ambient-pair allocation boundary on a
64×33 bottom and requires exactly two additional 16-KiB quanta. Inlet
under-budget requests refuse before publication.

Coverage now uses off-diagonal patches on a non-square 4×5 bottom, zero and
reversed carriers, carbon-bearing interior input, the independently bounded
gas sum, and separate mask/flux/tuple identity mutations. The current ratios
in the table above come from this expanded fixture, not the earlier 4×4
log. No numerical enclosure changed. Both mechanical mirror checks pass;
the rebuilt files have no compiler warnings. Full clean/Opto remains pending.
Two fresh read-only post-fix reviews of `89f14563` found no P1/P2:
allocation ownership/sibling completeness and numerical fixture/claim integrity.
They independently inspected the implementation and derivations; they did not
rerun the logged native executions. This closes the comparator increment's
review, not live-owner integration or full solver qualification.

| Artifact | SHA256 |
|---|---|
| scalar-flux-review-build.log | 7c6c7cd2b28439e733e2f16b7b5a32ec6aa3fbc364d2d03cf87d2507fd3075d9 |
| scalar-flux-review-sweep.log | 505e9db07d4c5ff340132a7c7da4ccb40ef6ed2bebec16cc5b295da8d3713dc4 |
| scalar-flux-accounting-red.log | a44700acf69d95b8c1ea92252661c6a603e86a59f720c1ef1764e4123e681412 |

### Remaining physical-flux integration map

For the physical-flux qualification increment, extend the existing immutable
ambient operand allocation with the prescribed bottom mask, per-face j, and
injected conservative tuple. This keeps the issuer's existing buffer/parent
structure; metadata declares the optional extent, and the full diagnostic
identity hashes all appended words. The comparator qualifies this first;
EOS/owner entry remains refused for the new operands until the existing
owner ingress root and projection/force consumers are connected together.
No separate authority protocol or live-path intermediate seal is added.

The comparator increment now covers all four device consumers in
`FireProductionAdvectionMac.mm`, not just the ordinary wall early-return:

- `evaluate_resident_physical_flux`: at an inlet, publish the imposed tuple
  minus the interior carrier as the nonadvective mass/energy term.
- `evaluate_resident_advective_pair`: retain that same interior carrier and
  zero advective high-minus-low increment at the prescribed face.
- `compose_resident_flux_pair`: publish the imposed tuple directly in both
  candidates. Subtracting the carrier and then adding it back is not the
  canonical rounding path for a prescribed boundary flux.
- `validate_resident_physical_flux`: the inlet comparison must use the exact
  imposed tuple for low/high, while independently checking the split terms;
  the current universal `low == donor + physical` bit test is not the inlet
  contract. Interior/open/seam validation remains unchanged.

The historical r198 fixtures in `FireSequenceTest.cpp` retain the ordinary
reconstruction check; the new inlet fixtures independently compare the
prescribed tuple and split terms alongside the device validator. Changing
only one would create a false refusal or a false reference. The gas-flux
reduction over components 1…6 is
intentional: component 7 is condensed C(gr), not a seventh gaseous species.
Do not extend that sum while adding inlet composition.

The missing immutable inputs belong in the existing physical-flux request,
`PrepareResidentPhysicalFluxRequest`, comparator ingress, and owner `copyIn`
ingress. Owner input-payload hashing already includes every `copyIn` segment;
use that path for the prescribed tuple/flux and projection controls. The
current `fuel_` buffer is a six-side uchar classification, not mass flux;
it cannot be substituted for the projection's packed float operand or the
force adapter's exact-length bottom mask. Count actual separate allocations
where those adapters require different layouts. This is an implementation
map, not a qualified input surface or a completed production change.

### Owner connection audit before enabling the inlet

At `b59d35f7`, `evaluate_resident_target_terms` already takes the six oriented
physical-flux face values directly, including boundary values; it does not
zero wall faces again. Its tangent evaluator therefore needs the newly
qualified inlet f_N input, not a second inlet source or a new target term.
The existing EOS/owner guard remains closed until the following are connected:

- `Prepare` packs the extended ambient operand and projection/force layouts
  through `copyIn`, including injected gas density among the hashed adapter
  controls; the existing input-root/bootstrap carries the lineage.
- `Project` supplies the private mask/j operand and matching immutable
  controls. `PrepareProducer` packs the projection's actual face density and
  momentum for the force adapter, so it must not recompute inlet density.
- The launcher removes volumetric staging before canonical source creation;
  chemistry sees actual Q0. `externalFuelMassKG` is currently assigned to
  accepted accounting only on acceptance (then accumulated later); preserve
  that ordering while replacing its source with the accepted imposed flux.

One additional **unmeasured gate interaction** must be tested before launch.
`fct_compatible_stage_rate` matches the live oracle's
`OpenCompatibleMomentumFluxDivergence3D`: wall and inlet ghosts carry zero
tangential momentum, so a transverse boundary product is zero even when
prescribed gas mass enters there. The current `fct_commuting_identity` uses
unit-velocity momentum transport as a proxy for restricted scalar divergence.
That proxy was exercised with zero wall mass flux. For a bottom-cell pair
sharing a tangential face and receiving uniform inlet j, with no other gas
flux, scalar restriction increases by `dt*j/dx`, while the prescribed
zero-tangential boundary product contributes zero to that unit-velocity
momentum rate. Thus a bare comparison is predicted to include a boundary
source term, not merely roundoff. This is a source-level derivation, **not a
measured failure or permission to weaken the gate**. Exercise the actual
CPU/Metal operators on that fixture before changing it; distinguish the
algebraic `D_i I_i` identity from the physical prescribed-velocity boundary
product. Keep the oracle's zero tangential inlet momentum, and account for
the exact boundary contribution if the prediction is confirmed. No envelope
or tolerance change is admissible, and no production verdict is inferred.

### Inlet commuting-proxy diagnostic (pre-correction measurement)

`tools/diagnose_fire_inlet_commuting.mm` extracts the unchanged production
`SingleStageFCTSource` literal, compiles it with the production safe-math
mode, and executes `fct_compatible_stage_rate` and `fct_commuting_identity`.
It compares all 240 momentum-rate faces with the live fp64
`OpenCompatibleMomentumFluxDivergence3D` at identical dyadic inputs. This is
a standalone raw-kernel diagnostic, not an authenticated owner qualification.
The scalar accepted state is analytically constructed from the prescribed
flux, not computed by the scalar producer in this diagnostic.

The 4³ grid has dx=0.125 m, dt=0.03125 s, pressure-open sides except the
prescribed bottom, gas density 1 kg/m³, unit test velocity, and zero other
fluxes. On the bottom-adjacent interior tangential face:

| Input j (kg m⁻² s⁻¹) | Metal physical tangential rate (kg m⁻² s⁻²) | Scalar increment (kg m⁻³) | Bare identity residual (kg m⁻³) | Oracle face mismatches |
|---:|---:|---:|---:|---:|
| 0.0625 | 0 | 0.015625 | 0.015625 | 0 / 240 |
| 0 | 0 | 0 | 0 | 0 / 240 |

Both executions have zero kernel failure bits. The nonzero residual is the
exact boundary contribution `dt*j/dx`, not a Metal/oracle momentum mismatch.
The unit-velocity proxy has applied the physical zero-tangential boundary
condition, while the scalar restriction includes imposed mass. It is not
the homogeneous commuting identity that the existing fixture exercised.

Implementation ruling, before code: retain the physical momentum operator
unchanged. In the identity check, include the exactly derived prescribed
boundary contribution in the restricted scalar balance (or evaluate the
unclosed algebraic restriction separately, whichever is smaller). For a
tangential component, restrict the incoming lower minus outgoing upper
prescribed mass flux across its two neighboring primal cells, multiply by
dt/dx, and use the same half-weight at a pressure-open component endpoint
as the existing restriction. Prescribed normal component faces retain their
existing fixed-rate check. The CPU sibling and Metal diagnostic must agree;
ordinary zero-wall-flux results must remain unchanged. The current uncorrected
proxy is the RED. Do not change a residual envelope or skip inlet-adjacent
faces. This corrects the measured object, not the physical boundary condition.
The implementation follows below; live inlet-owner qualification remains pending.

The historical measurement used the tool and kernel source at `8ce745f0`.
These are its serialized build/run commands; the current tool instead tests
the corrected zero-residual balance described below:

```sh
clang++ -std=c++17 -O2 -fno-fast-math -ffp-contract=off -fobjc-arc \
  tools/diagnose_fire_inlet_commuting.mm -framework Metal -framework Foundation \
  -o /tmp/r239-inlet-commuting
/tmp/r239-inlet-commuting src/Library/Utilities/FireProductionAdvectionMac.mm
/tmp/r239-inlet-commuting src/Library/Utilities/FireProductionAdvectionMac.mm --zero
```

The final build log is empty (no warnings). Payload logs stay outside git.
Two fresh read-only reviews of `8ce745f0` (boundary mathematics and diagnostic
integrity/claims) found no P1/P2 in the instrument or proposed derivation.
They did not rerun the diagnostic. The correction must additionally cover
unequal adjacent fluxes, upper prescribed-boundary signs and pressure-open
tangential endpoints; the present uniform-bottom diagnostic does not qualify
those cases. This review applied to the diagnostic before the correction below.

| Artifact/source | SHA256 |
|---|---|
| tools/diagnose_fire_inlet_commuting.mm | b5738b84e37620325699586c727d0060cac87cad7c7f1239bbaaa0d42bb79cf2 |
| src/Library/Utilities/FireProductionAdvectionMac.mm | b29167c6d1abb8952b70ef8faba6b35ea662c770fd20863c41b686b81270d772 |
| tools/fire_simulator_3d_advance.h | eb8bb8fee31c8b7de7450c17a2f4c562f85275c406f86b4627c107c5011740b8 |
| inlet-commuting.log | 586a5387b844dc017a4ca2d647837b9ee51bae43cafd2f3f9ec08b179fc8866d |
| inlet-commuting-zero.log | 01d7d11d55761f5f0eeabde56a687522200be785eb83c59ab49582c879411467 |
| inlet-commuting-build.log | e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 |

### Corrected prescribed-boundary commuting balance

The CPU scalar-FCT identity and Metal `fct_commuting_identity` now add the
derived prescribed transverse boundary mass contribution to the advanced
scalar restriction. This changes neither physical momentum transport nor
the scalar update. Metal borrows the accepted low/delta/shared-alpha buffers
already owned by R1; no new allocation, upload, authority or publication
surface is introduced. Normal prescribed faces retain their exact positive-
zero rate check. Ordinary zero-wall-flux cases skip the added arithmetic.
The existing gamma128 residual bound and its scale are unchanged.

CPU tests now execute the scalar producer with supplied prescribed face
fluxes, independently checking the full accepted state byte-for-byte against
the dyadic scalar balance and requiring zero identity residual. Four cases
pass: zero inlet, uniform inlet, unequal adjacent inlet fluxes, and unequal
upper prescribed flux (outflow sign). The raw Metal harness tests the same
boundary balances, including pressure-open tangential endpoints, and still
compares every physical momentum rate with the live fp64 oracle. All four
have zero maximum identity residual and zero oracle face mismatches. The
CPU test and raw Metal harness are separate tests, not an end-to-end native
scalar-producer/owner inlet qualification.

Two native mutation REDs are reproduced: removing the boundary correction
leaves 0.015625 kg/m³ residual; omitting the endpoint half-weight leaves
0.0078125 kg/m³ residual. Both exit 10. The corrected cases exit zero. The
standalone harness now expects zero residual; its prior diagnostic behavior
and source SHA remain in the historical measurement above.

The full `--fire-production-metal-fp64-kernel-sweep` exits zero, including
the added CPU fixtures and existing owner, target and compatible-momentum
gates. Generated fp64 and arithmetic-trace sources are mechanically updated
and their checks pass. Rebuilt files and the standalone tool compile without
warnings. Clean/Opto and live inlet-owner integration remain pending; no
corrected production run is launched and no kernel migration is permitted.
Two fresh read-only reviews of `5130f7e9` found no P1/P2: boundary mathematics
and binding/test/sibling integrity. They did not rerun the recorded tests.
The qualified fixtures do not cover nondyadic cancellation-heavy transport,
nonzero source/correction flux, or multiple prescribed planes meeting at a
corner; do not extrapolate this increment into a general inlet-owner claim.

| Artifact | SHA256 |
|---|---|
| commuting-fix-build.log | cea45b3ad9e74fa7af8a84054664fd3875f94b7db846ec8823a3a146a9562276 |
| commuting-fix-sweep.log | a56f6ae8445835eb738b98b65c7ff5800633dc78070f184639874ceacff8d83c |
| commuting-harness-build.log | e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 |
| commuting-corrected-base.log | 66968d480966713468ca0fdbd9329294d43d80e2386605c4ce97421b844b999c |
| commuting-corrected-zero.log | 0a8e0bd1e4a145b12c5948df0ff0614f752a39ee9256b460449e4e6f7e2b7c73 |
| commuting-corrected-unequal.log | 2478527753b183a1c168fa8107be23a1ba4c72cb3710ac9895ccc858f2b9a311 |
| commuting-corrected-upper.log | 504ab354a568a9545781fe880f011dea3b4414d6fdaebcc712543d03be6f15c1 |
| commuting-corrected-omission-red.log | bc087e96936b054e42ddda962820c19d736033ef20d71fcda3399cac56f4dfc7 |
| commuting-corrected-endpoint-red.log | 972b9ada2f24ce7220e9a53c203e6821f89ba5fabaa9109548ccc752455c8054 |

### Downstream force consumer audit (integration prerequisite)

The prescribed projection cannot be enabled alone. In
`FireProductionAdvectionMac.mm`, `ResidentProjectedHeunMetalOwner` packs
the projected momentum and density into the nonpressure RHS. Its current
`FireProductionForceMac.mm::setup_face_velocity` treats every wall-class
face as zero, including a future masked burner; `neighbor_velocity` then
uses the homogeneous wall ghost `-u_cell`. The live oracle's
`tools/fire_simulator_3d_advance.h::BuildOpenNonpressureMomentumRHS3D`
instead consumes the projected inlet velocity and uses
`2*u_boundary-u_cell` for the normal ghost component. Thus wiring only
projection and scalar flux would still silently lose the inlet from
near-bed stress/velocity-gradient inputs. This is a source-audit finding,
not a newly measured production trajectory.

Carry the same immutable inlet mask through the existing nonpressure RHS
request and resident inputs. Preserve prescribed velocity in its face-to-cell
gather and use the actual projected normal boundary velocity in the ghost;
tangential boundary velocity remains zero. Keep the ordinary wall/no-inlet
path unchanged. Compare cell velocity, gradients/stress, and phase-source
restriction separately against the live oracle before enabling the owner.
The phase-source kernel also currently suppresses all wall faces, whereas
the oracle restricts the adjacent-cell source with a half weight and
multiplies by projected velocity; masked nonzero-velocity faces need that
same treatment. This is existing-operator conformance, not a new force or
an adjustable dissipation coefficient. Include CPU siblings in the change.

The stored-density audit is narrower: ordinary unmasked wall density is
not used for force velocity or buoyancy because those paths explicitly
zero the wall. Compatible advection consumes scalar flux and projected
velocity, not this packed density. That observation does not resolve
burner handling or certify every other public force API; no all-face
stored-density equivalence is claimed.

### CPU force prerequisite (not live owner qualification)

The CPU nonpressure RHS now accepts the optional bottom classification,
validates its extent, binary values and wall bed, and accounts its bytes
against the unchanged limit. The internal force stencil preserves projected
inlet velocity and the inhomogeneous normal ghost; phase-source restriction
uses the adjacent half-cell. Public frozen-force advance behavior is unchanged.
At this CPU increment the Metal nonpressure comparison entry refused nonempty
masks explicitly. The native increment below supersedes that refusal. This
is intentionally not a live-owner completion claim.

An analytic uniform-throughflow test has zero stress/eddy viscosity and
exact phase rates 0.5 kg m⁻² s⁻² at bottom/top versus 1 in the interior.
The homogeneous-wall mutant produces nonzero stress and loses the bottom
phase rate. All-zero masks preserve the old field values; malformed masks
refuse atomically. These tests pass in the full executable. Generated fp64
and trace mirrors were refreshed mechanically (including the preceding
r239 projection changes); both generator `--check` invocations pass.

The complete `FireProductionSolverTest` executable is **not green**: both
unchanged HEAD `7d4e4246` and the force patch report the identical eight
failures. The unchanged baseline was rebuilt/run with the four-file patch
temporarily stashed, then the patch was restored and rebuilt/run. No
failure is attributed to the patch merely by assumption. The failures are
the legacy full-step cap fixture (both old shapes now exceed 2 GiB), three
resulting timing-trial admission refusals, its 200 ms aggregate, and three
source/build/census assertions. The actual cap stays unchanged. The old
performance gate is not promoted into a production timing claim. These
legacy gate repairs remain backlog under the tangible-output sprint;
this entry claims only the named passing inlet tests, not suite sign-off.
Focused rebuilt files have no compiler warnings. Native inlet-force,
matched-oracle force fields, full owner, clean/Opto and review remain pending.

Logs retained outside git under the r239 calibration directory:

| Artifact | SHA256 |
|---|---|
| force-baseline-test.log | 2a3e8d2f84a1b637254658ed67e721f8e06ea12bfd4d439e30ec521e862746ec |
| force-cpu-final-test.log | 42e995753d2f138b0fa72d13a28837feaa07e0853a29e60d95da90e689fcb77a |
| force-cpu-final-build.log | d7a97d16f58508dcc53fce49c06eb2f5d5beb8ba76413fe7a32d7626c36ee81d |

Independent review of `87f3a101` found one P1 and one P2. The P1 was an
unhandled fourth force consumer: masked-face buoyancy still took the ordinary
wall zero. The live oracle's `BuildRelativeBuoyancyMomentumRate3D` uses the
injected/interior face density there. The correction now applies the same
`(rho_face-rho_ambient)*g` formula on masked faces while leaving ordinary
wall behavior unchanged. A new regression with rho_face=0.75 kg m⁻³,
ambient=1 kg m⁻³ and g_z=−10 m s⁻² requires +2.5 kg m⁻² s⁻² in both inlet
buoyancy and combined RHS (zero phase source, boundary stress zero).
It failed before the fix and passes after it. No tolerance was added.

The P2 was a short-mask test contaminated by the preceding nonbinary value.
It now resets that value before shortening; oversized and non-wall-bed
cases independently refuse too. Mechanical mirrors were regenerated and
checked. The executable again has exactly the eight baseline failures and
no r239 failure, not an overall suite pass. Two fresh post-fix reviews of
`70375ac6` (math/indexing and safety/test integrity) found no P1/P2 in this
CPU prerequisite. They do not qualify the pending native/live-owner path.
A sparse asymmetric mask with varying density/velocity is additional
coverage to carry into that native comparison.

| Artifact | SHA256 |
|---|---|
| force-buoyancy-red-test.log | 08024203c9c3af9f5fb3ec8032a080c314e7e8fe5aba756301afba3d52d10f57 |
| force-review-fix-test.log | 036819cb9058a3b3570da1f0062f527166973e83ddecc253505d1163f608f64a |
| force-review-fix-build.log | 4748dea78429f7b6d0b46e8135e9bab8f56eaff21afe9ffdbf285a5d2ef484c6 |

### Native force prerequisite (owner binding pending)

The resident nonpressure RHS can now borrow a private immutable bottom mask.
It validates exact extent, storage mode, device and wall bed before encoding;
the caller's existing case lineage owns mask validity and lifetime, as for
the other resident input buffers. The comparison wrapper validates binary
values and uploads once before its first kernel. No CPU force evaluation or
interstage full-grid staging was added. The live owner does not yet supply
this operand and cannot launch the conformed burner run.

The setup, stress, buoyancy and phase-source kernels now use the same inlet
exceptions as the reviewed CPU path. The existing 88-byte parameter record's
unused word carries the mask count. Ordinary frozen-force consumers pass a
placeholder binding with zero count and never read it; they allocate nothing
new. The standalone optional certificate adds `nx*ny` caller bytes and two
separately rounded mask buffers. The resident evaluator borrows the mask;
the future owner must include its actual ingress allocation in its peak.

Native uniform-flow, buoyancy, zero/absent-mask and sparse asymmetric 7×5×4
comparisons pass for every output field value, without an added tolerance.
The sparse case has varying dyadic face density and velocity and disables
SGS to isolate boundary/stress arithmetic; it is not an arbitrary-input
Vreman qualification. Invalid nonbinary, short, oversized and non-wall
requests refuse before a command or partial output. Existing interstage
transfer checks and the exact optional allocation delta pass.

A temporary native mutant set the mask count to zero despite supplying the
private mask. Uniform-flow, buoyancy and sparse comparisons all failed;
restoring the count cleared all three. The full executable still fails seven
legacy checks, not a suite pass. The prior force-build source assertion was
repaired to require the existing `-fobjc-arc` alongside strict FP flags;
the exact copy-site census is 17 prior sites plus one named inlet upload.
No tolerance, resource ceiling or performance gate was widened. Rebuilt files
are warning-free; mechanical mirror checks pass. Two fresh read-only reviews
of `0894698d` (native math/binding siblings and storage/accounting/test
integrity) found no P1/P2. Full owner binding, clean/Opto, broader oracle
comparisons, direct resident bad-buffer REDs and a nonuniform nonzero-SGS
burner comparison remain pending; these wrapper fixtures do not claim them.

| Artifact | SHA256 |
|---|---|
| force-metal-restored-test.log | d6f5475b7b48db48a45975dd0749af9ce367645b091b296db43de2e3ab6f1296 |
| force-metal-restored-build.log | 78d94121d55c85d2c80fe1142abe55805fc1862d13e919c7c9d94861c6dd4b9f |
| force-metal-red-test.log | 5aeff2d6958352c710dca8424dbde7a696aa9e9aea34ee193a0feec4c6482c97 |

Use immutable case operands through existing requests and private resident
buffers; no new authority class or certificate protocol. Extend existing
identity inputs and working-set accounting for the actual added operands.
An empty/no-inlet request must preserve the current arithmetic and old tests.

1. Bed/inlet projection: a masked face surrounded by adiabatic bed faces;
   opposed provisional momentum and strong pressure forcing must not reverse
   its imposed velocity. Compare the live fp64 oracle per face, including
   arithmetic face density, fixed momentum, neighboring head and residual.
   Check bed coefficients on every level and all post-residual consumers, not just
   the final velocity overwrite.
2. Scalar/target split: arbitrary interior mixtures, zero and nonzero j,
   inward and outward provisional velocities. Imposed low/high tuples are
   identical, correction zero, f_N matches the oracle independently. The
   old open-donor mutant must permit outward fuel in the reversing fixture.
3. End-to-end source ledger: a small reaction-disabled bed/inlet case
   proves injected mass enters exactly once; the staged-plus-inlet mutant
   must be detected. Reaction-enabled matched inputs then check the changed
   source schedule. Existing owner gates and residency checks remain intact.
4. Run a short from-zero production prefix before another full window.
   Report accepted imposed injection and actual boundary flux separately;
   retain top export as a real measured loss, not something this fix erases
   by assumption. Reuse r238's accepted-flux observer at selected states.

This is a semantic boundary conformance change, not a bit-identical cost
promotion. Historical checkpoints remain untouched; no kernel migration.
Only a from-zero full window can revise the empirical rows or deficit claim.
Fresh implementation review and existing qualification gates precede launch.

### Shared operand packing before owner connection

The physical comparator, EOS comparator, target comparator, and owner now
use one `PackResidentPhysicalFluxOperands` routine. The validated layout
remains `[ambient9, maskN, jN, injected9]` for prescribed flux and exactly
`ambient9` otherwise. This removes four independently maintained ingress
packings; it does not change a kernel, a numerical operation, a seal layout,
or any admitted input. The owner still hashes the actual private copy.

At `9ad7a299`, the EOS/owner inlet refusal and authenticated projection inlet refusal stay
in place. Their removal still requires the coherent projection/force wiring,
optional-allocation accounting, and launcher removal of volumetric staging.
In particular, the EOS and target comparator working-set functions currently
cover only the admitted ordinary layout; they must account for the larger
operand pair before accepting prescribed inputs. No live inlet run or
inlet EOS qualification is claimed by this refactor.

The focused native test rebuilt without warnings and the complete
`--fire-production-metal-fp64-kernel-sweep` exited 0, including the existing
inlet physical-flux, owner, lineage, and refusal fixtures. Clean/Opto remains
pending; this is the focused gate, not a full-project qualification.
External logs under `rendered/fire_production_calibration/r239_burner_projection/`:

| Log | SHA256 |
|---|---|
| `operand-packing-build.log` | `7c304e110f0b5be9758d73cd726696cfa61a609d43a5c8b7eb977a8c16fa3f51` |
| `operand-packing-sweep.log` | `e4be1fc500f0f5fdbeaf68741201074d74477a0947c55f5d18a60f08772015bd` |

Two fresh read-only reviewers examined `9ad7a299`: packing correctness /
lifetime / provenance, and sibling coverage / accounting / evidence fidelity.
Both returned no P1/P2 findings in one round. They did not independently run
the suite. Additional direct byte-layout and target/owner inlet-refusal tests
were identified as optional coverage; the existing comparator mutation tests
and shared EOS refusal cover this admitted-path refactor. This review does
not qualify the still-disabled live inlet path.

### Prescribed flux through scalar candidate and EOS qualification

Before opening the live owner, the existing EOS comparator will admit the
already-qualified prescribed physical-flux operand. Its scalar producer
must consume the imposed tuple once, followed by the unchanged EOS kernel.
The internal preparation guard stays closed by default for the target and
owner; only the qualification comparator opts into the inlet input. Its
working-set calculation inherits the physical comparator's optional enlarged
operand pair, without changing the cap or any arithmetic bound.

Use a uniform thermochemical state, zero velocity/source, and asymmetric
prescribed bottom fluxes: the accepted scalar field has an independently
constructed `Q + (dt/dx)*imposed` answer at bottom cells and exact unchanged
values elsewhere. Check every component and cell, then compare each accepted
temperature/represented pressure with the fp64 functional's binary32
projection. Nonempty zero-flux is the no-op control; missing flux, altered
mask and underbudget cases must not silently publish. This qualifies only
the candidate/EOS consumer chain, not the still-disconnected live owner.

At `07ea6a80`, the EOS qualification comparator opts in explicitly; target and owner
preparation retain their default refusal. The optional EOS working-set
argument delegates to the already qualified physical-flux accounting for
the enlarged upload/private pair. The unsupported-platform sibling and both
mechanically generated mirrors carry the same declaration. No MSL kernel,
EOS enclosure, authority issuer, or production admission changes.

The new `--fire-production-burner-eos-metal` fixture failed with exit 207
before the opt-in (the old prescribed-input refusal). Afterward both controls
pass: nonempty all-zero flux and four asymmetric nonzero patches at cells
0/5/11/15 on a 4-cubed domain, with 425 K uniform composition. All 576 scalar
words per case match the independent integrated tuple byte-for-byte; all
64 temperatures and represented-pressure ratios per case match the fp64
functional's binary32 projection bit-for-bit. Missing flux with a nonzero
injected tuple, flux outside
the mask, and an understated allocation budget refuse without publication.
The 64-by-33 bottom extent calculation grows exactly two allocation quanta,
one each for upload/private operands. Zero interstage transfers and actual
allocation within the certificate are checked. The prior physical fixture's
explicit EOS refusal is replaced by an explicit target refusal, since only
the qualification EOS consumer has been opened.

These fixtures do not exhaust injected compositions or replace the r199 EOS
domain qualification. Their injected mixture matches the uniform interior;
mixed-front live-owner qualification and actual case fuel remain pending.

The focused build was warning-free; the analytic EOS fixture and complete
native Metal/fp64 sweep exited 0. Both generated mirrors pass `--check`.
Clean/Opto remains pending. Logs are external in the same r239 directory:

| Log | SHA256 |
|---|---|
| `burner-eos-before.log` (exit 207) | `fc9f3233225e412fadc2a5048bd8b3c1fc49e8e132f5bab60b0bb2599b5810e7` |
| `burner-eos-build.log` | `1b8c6580b651e247afb02aae0877268b37c842f14d650743f631d634199a41bf` |
| `burner-eos-after.log` | `355fdb6505f55521249cafd3e62e176a180ad3e60497fadc20353554dc367b7f` |
| `burner-eos-sweep.log` | `dbd081b82cb1a74640c2d7f8ad2725e2960250a338926af2fb5112c54740f4f7` |

Two fresh read-only reviewers examined `07ea6a80` (math/input qualification
and isolation/accounting/evidence integrity), both with no P1/P2 findings.
They did not independently execute the tests. The unsupported implementation
also passes `clang++ -std=c++17 -fsyntax-only -Wall -Wextra` without diagnostics.
Direct fully-valid owner refusal and different injected/interior mixtures
remain coverage opportunities before live inlet integration. The legacy
mask-only classification request with empty prescribed flux and zero injected
tuple remains intentionally unchanged; the new missing-flux RED does not
claim to reject that historical path.

### Target consumer inlet qualification

The next existing comparator to open is the target consumer, not the owner.
Its opt-in follows the EOS qualification path and its allocation calculation
inherits the enlarged physical operand pair. The target kernel already
differences boundary f_N, so no new target term or kernel is called for.
Qualify its tangent, frozen source, recorded absolute deviation, tail drain,
and assembled target separately against the existing fp64 functionals and
local enclosures. Use the mixed-temperature, mixed-composition target fixture
with unequal prescribed patches and a distinct injected composition; preserve
an open side so closed-domain mean removal does not hide a boundary term.
Keep owner admission refused until projection and force inputs are root-bound.

The target comparator now opts in; the owner remains default-refused and the
physical-flux fixture checks that refusal explicitly. The target working-set
option delegates to the EOS/physical calculation, preserving the existing cap.
No target kernel, functional, bound, or source construction changed.

The focused fixture failed with exit 220 on the old prescribed-input guard,
then passed with the opt-in. It uses the existing mixed-state fixture,
300 K pure injected fuel (different from the interior mixture), four unequal
bottom patches, and pressure-open x sides. The fp64 target functionals consume
the separately qualified physical-flux and EOS outputs at matched inputs;
this is a consumer qualification, not a complete independent fp64 owner run.
All five terms are checked independently at all 64 cells in s^-1:

| Field | Worst-ratio cell | Residual (s^-1) | Local bound (s^-1) | Worst residual/bound |
|---|---:|---:|---:|---:|
| tangent | 38 | 1.4785057400601431e-8 | 1.4901162970204496e-8 | 0.99220828804870986 |
| frozen source | 0 | 0 | 0 (exact copy) | exact |
| recorded absolute diagnostic | 21 | 2.9802311729554276e-8 | 2.9802322387695312e-8 | 0.99999964237213135 |
| outlier tail | 63 | 2.7008344338774748e-8 | 2.9802322387695312e-8 | 0.90624965354800224 |
| assembled target | 4 | 2.8910026816930667e-8 | 2.9802325940408991e-8 | 0.97005941330678302 |

The bounds are the existing authenticated tangent/assembled enclosures,
zero for the source copy, and half-lattice spacing for represented diagnostic
and tail outputs. No pooled acceptance ratio is used. Prescribed-input
underbudget refuses before device work; an unsealed physical parent yields
bits 11/13 with zero target/consumer publications. The 64-by-33 allocation
test adds exactly two 16-KiB quanta. No interstage transfers are observed.

The focused build has no warnings; the prescribed-target fixture and complete
native Metal/fp64 sweep exit 0. Generated mirrors pass `--check`; the unsupported
implementation passes the standalone warning-enabled syntax check. Clean/Opto
and the live-owner inlet remain pending. External r239 logs:

| Log | SHA256 |
|---|---|
| `burner-target-before.log` (exit 220) | `8ae8ade2a8ffb7b47441f1eb35a198f0d60b11b16e2845d0c1cd065041edf986` |
| `burner-target-build-v2.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `burner-target-after.log` | `5c00baa3c7e82aff600d742a770a2e70ec1f6479435226998b2684c3d2f5554c` |
| `burner-target-sweep.log` | `64cefe4099a0279b8437d506ef900146765932cea2aa432760d93d4178cb90e1` |

First fresh review of `4bed0bce`: mathematical/observable review found no
P1/P2. Integrity review found one P2 in the new unsealed-parent test: the
generic helper required zero consumer identity but only printed target
identity. The observed target was correctly zero; regression protection was
incomplete. The burner-parent check now explicitly requires both identities
zero and rejects a synthetic leaked-target result in the refusal predicate.
This is a test-predicate RED, not a new kernel mutation or authority surface.
Other consumer-refusal tests intentionally may retain a valid target (for
example a mismatched projection topology), so the stronger target-zero
requirement is explicit rather than indiscriminately applied to them.

The strengthened fixture passes in the complete native sweep (exit 0),
including `synthetic_target_publication=1 rejected=1`; rebuild diagnostics
remain empty. The review-fix logs in the same external directory are
`burner-target-review-build.log` SHA256
`7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21`
and `burner-target-review-sweep.log` SHA256
`b51a46e0df59f187ecad29eafce5b74b80af5df5a115bcbe89e7d9c94998140c`.

The fresh final round reviewed `3242a310` on correctness/test integrity and
isolation/accounting/evidence fidelity. Both reviewers returned no P1/P2.
Two rounds total: one P2 assertion gap found and fixed; no production defect
or widened bound. Reviews were read-only, without independent test execution.
The live-owner inlet connection remains unqualified.

### Reference-owner input audit for the remaining live connection

The CPU owner's `FireProductionScalarPhysicalFluxPrerequisiteRequest` has no
prescribed inlet tuple/mass-flux fields at this point, and its two projection
request builders do not populate burner controls. The existing raw projection,
force, physical-flux, EOS and target qualifications therefore must not be
reported as a whole-owner inlet comparison. Complete the reference-owner
input plumbing (or compare with the preserved live oracle's actual burner
owner at matched inputs) alongside the resident wiring; keep the existing
cold-flow owner gates unchanged. This is the remaining input-domain gap,
not a reason to relax the owner comparison or to launch a half-connected run.

The resident connection will be exercised through the existing owner
qualification entry first. Move the temporary inlet refusal to the production
step entry while wiring the same owner class, rather than adding a new
qualification protocol. Its private projection mask/flux and force mask enter
the existing input digest along with injected gas density; adapters borrow
those exact buffers. Extend the input-payload budget by the actual extra
operand bytes, as well as the allocation certificate. Production admission
stays closed until reference comparison and launcher integration are complete.

### Resident owner connection: bounded qualification increment

The existing owner qualification entry now consumes the prescribed inlet
through the same resident owner class: physical/EOS/target packing, the private
projection mask/mass-flux operand, and the force mask. These operands and the
injected gas-density control are bound by the existing ingress payload root.
The capstone inlet is gas-only; a prescribed solid component is refused rather
than silently treating total injected mass as gas momentum. No MSL arithmetic
or numerical bound changes in this increment.

For N bottom faces the ingress allowance increases by 17N + 40 bytes:
physical packing adds 8N + 36, projection adds 8N, force adds N, and the
separately bound density adds 4. The allocation certificate additionally counts
the separately rounded upload/private pairs and the existing subordinate inlet
certificates. It remains conservative, not a measured allocation estimate.

The mixed-state owner fixture exercises four cases: base patches, a changed
patch mass flux, a changed injected density, and an added zero-flux mask bit.
Each has bit-exact prescribed endpoint momentum, velocity and masked density;
each mutation changes the ingress root. All four report zero interstage grid
transfers, 1,261,568 actual bytes against 13,762,560 certified bytes. This proves
operand binding and endpoint behavior, **not** full inlet-owner fp64 trajectory
conformance. The production entry still refuses the inlet until the reference
input domain and launcher are integrated; its refusal is tested explicitly.

The focused fixture, complete native Metal/fp64 sweep and raw resident
projection harness pass. Mirror generators pass `--check`, the unsupported
force implementation passes warning-enabled syntax checking, and focused build
logs contain no warnings. Clean/Opto and whole-owner inlet reference comparison
remain pending. Self-audit priorities are root coverage of every consumed
operand, adapter authority admission, independently rounded allocation sizes,
endpoint-test scope, and preservation of the production refusal.

External logs in the r239 calibration directory (SHA256):

| Log | SHA256 |
|---|---|
| `burner-owner-build.log` | `c80d7f1474b1a1299b9523b6ca951d65a762ad73fb48955ebb86f1b077c26c69` |
| `burner-owner-binding.log` | `c3cd50d83dae728622d4f2560fd61997eb21da2e8a6c34e60d8611c4e96209a5` |
| `burner-owner-sweep.log` | `203cd2c4cd41c87126efb17ce6c9ba314aad417c0bdc6f04b33efb4e5bea8d8a` |
| `burner-owner-raw-build.log` | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `burner-owner-raw.log` | `33f34ade0a81e429df6aec1949b1d52fd9786fa590fcb5945354d45b5f0ba182` |

Fresh review of `1379a7c8` returned no P1/P2 across two independent axes:
provenance/allocation and numerical wiring/test integrity. Both reviews were
read-only, without independent execution. An initially suspected raw-harness
false green was rejected: the refusal helper restores the valid original input
after every case. No code changed after the reviewed commit.

Coverage still required before production admission: the inlet-owner matched
reference must detect omitted force-mask wiring in stage forces/interior state
and omitted imposed scalar flux in species/enthalpy (endpoint tests alone
cannot). Inlet-budget rounding-edge cases remain separate coverage work.
These are remaining qualification work, not evidence of a completed live inlet
or of a production-window result.

The focused owner test now also rejects a nonzero injected solid component,
an all-zero injected tuple, and a working-set limit one byte below the
inlet-aware certificate. Each must report its named clause with no conservative
state or publication root; the zero-density input correctly fails in physical
input preparation before owner execution. These are malformed-input REDs, not
kernel-omission mutants. The focused native test exits 0 and rebuild diagnostics
are empty; production sources and kernels are unchanged by these additions.
External log `burner-owner-admission.log` SHA256:
`c215236768be67ec837a65e249b45e030041b716c6af75ce3fb46296e6a344ba`;
`burner-owner-admission-build.log` SHA256:
`7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21`.

The complete native sweep also exits 0 after these test additions:
`burner-owner-admission-sweep.log` SHA256
`933c828660c93ab58e5692134b08c2341a67a830281986e520895589fa925117`.
Two fresh read-only reviewers of `71222555` found no P1/P2 on test integrity
and reference-plan fidelity. No production source or kernel changed.

### Reference input-domain completion: implementation decision

Extend the existing CPU owner and its mechanically generated fp64 instantiation,
not a new comparison owner. Keep the empty-inlet path's arithmetic and identity
bytes unchanged. The extension must cover the complete chain in one admission:

1. Carry the authored mask, mass flux and injected tuple into the physical and
   scalar contracts; include their exact bytes in the existing stage/contract
   identities only when present. Reject disagreement between their copies.
2. At masked bottom faces the donor carrier is `u * Q_interior`; the centered
   physical term is `imposed - carrier`. Publish composite low/high directly as
   the imposed tuple, with zero delta, never by subtracting then re-adding the
   carrier. The ordinary diffusive nullspace certificate applies to ordinary
   faces, not to this externally prescribed flux; verify the prescribed branch
   against the original `BuildOpenBoundaryFlux3D` functional separately.
3. Carry boundary supply into the absent-species reconstruction and limiter
   logic. A species absent in the current cells but supplied by the burner is
   not globally absent. The oracle's `BuildOpenFluxPair3D` already makes this
   distinction. Merely overwriting the final low flux would miss this consumer.
4. Populate inlet controls in both CPU-owner projection builders (coupled R0/R1
   and terminal R2), and pass the same mask into the nonpressure force request.
   Use the projection's actual face density/momentum, not a second restriction.
5. Preserve the existing prescribed transverse commuting term through scalar
   acceptance and the owner's independent witness. Keep the same bound.
6. Exercise matched-input stages and whole-owner outputs, with an omitted-force
   mask mutant and an omitted-imposed-flux mutant. Check interior forces and
   species/enthalpy, in addition to boundary values. The original oracle
   boundary functions remain the independent boundary reference; the generated
   fp64 owner provides the already-reviewed monitored schedule, not the
   globally restoring oracle's different time-step policy.

This audit identifies why adding only fields to the projection request is not
sufficient. It adds no authority surface or migration protocol. Production
admission and the launcher remain closed until this coherent comparison passes.

First implementation diagnostic: both extended CPU precisions reach the existing
`scalar Heun physical affine certificate differs` refusal; Metal accepts the
same inlet fixture. The stage validator currently applies the homogeneous
diffusive-nullspace check to every boundary face. The prescribed branch has an
external affine flux, not a homogeneous diffusive flux. Carry its authored
mask/tuple/flux in the existing stage payload and composition hash, require its
direct imposed low and zero delta, and retain the homogeneous check unchanged
on ordinary faces. Averaging requires identical authored inlet inputs. This is
classification of a different boundary functional, not a wider envelope or
suppression of an ordinary-face constraint. The reference candidate is not yet
qualified and must not enable the production inlet.

### Existing CPU/fp64 owner: input-domain implementation, gate still pending

The existing CPU contracts now carry optional inlet operands through scalar
donor construction, direct imposed composite flux, retained physical flux,
both projection builders, force mask and the independent commuting witness.
Their bytes enter existing identities conditionally; empty-inlet identities
retain their original preimages. The Heun stage retains the authored operands
and validates prescribed low/zero-delta separately from ordinary diffusive
constraints. Averaging refuses differing authored operands. The generated fp64
and roundoff-trace instantiations are regenerated from the same source; tuple
identity uses the established scalar bit-comparison helpers (never raw bytes
of the trace wrapper).

The physical and stage payload queries include the optional operand footprint;
the owner adds a conservative 32 simultaneous operand-copy allowance across
retained requests, stage builders and projection scratch. This is reference
host accounting, not a new Metal allocation or performance claim. Numerical
bounds and device kernel arithmetic are unchanged.

The original-oracle physical-flux fixture additionally requires exact CPU/Metal
prescribed-branch bytes at its four asymmetric patches, using the same existing
per-field original-oracle enclosures. The full native regression sweep passes.
The existing owner fixture's new diagnostic mode runs both CPU precisions and
Metal to acceptance on the inlet input; mismatched parent flux and forged stage
mask are refused. This is execution of the reference domain, **not yet the
r201 per-cell/per-field owner contract gate**. That gate, force/scalar omission
mutants, clean/Opto and launcher integration remain outstanding.

Observed final scalar differences from this diagnostic, each over 64 cells:

| Component | Units | Metal/CPU32 unequal words | Max absolute Metal/fp64 distance |
|---|---|---:|---:|
| 0 | kg/m³ | 0 | 4.3441756858442204e-9 |
| 1 | kg/m³ | 0 | 3.6363114463977553e-9 |
| 2 | kg/m³ | 0 | 2.178345823167227e-8 |
| 3 | kg/m³ | 0 | 7.2049167743060138e-8 |
| 4 | kg/m³ | 0 | 5.0178224075825284e-10 |
| 5 | kg/m³ | 0 | 5.9959747496923788e-10 |
| 6 | kg/m³ | 64 | 3.4158566046432948e-14 |
| 7 | kg/m³ | 64 | 1.8958953763594531e-14 |
| 8 | J/m³ | 0 | 0.051595005381386727 |

No pass/fail is inferred from these maxima: the local enclosures and class
sequences still decide the owner gate. Production admission remains refused.
The authored reconstruction projector is carried unchanged; the boundary-supply
census at its launcher construction is still part of the pending coherent gate,
not established by this mixed-composition fixture.

External logs (SHA256), all in the r239 calibration directory:

| Log | SHA256 |
|---|---|
| `burner-reference-diagnostic.log` (initial affine refusal, exit 221) | `cec365cea5736ee7e09f09038d639ada7bca41019e66281462eb8b9e252a33de` |
| `burner-reference-build-v6.log` (no warning/error diagnostics) | `1b8c6580b651e247afb02aae0877268b37c842f14d650743f631d634199a41bf` |
| `burner-reference-sweep.log` (exit 0) | `48aa1618f87d8bd7ccc772da46293ec86974eddff5bef90575d19acf58436c03` |
| `burner-reference-diagnostic-v3.log` (exit 0, not a fidelity verdict) | `46db15d947dfa162cd515d375f9fcde06b5d3698942b85fb47b803669c15c4a7` |

The first fresh review of `5888f8ee` returned no P1/P2 on numerical/sibling
correctness and identity/accounting/test integrity. Review identified a coverage
limitation: changing only one parent mask tests cross-parent mismatch before the
retained-content hash. A strengthened RED changes the same zero-flux mask bit
in both parents, preserves their stale identities, and requires the exact
`scalar Heun flux-stage content identity differs` refusal with no publication.
It passes in the native reference diagnostic. Header descriptions now explicitly
include optional inlet payloads and direct prescribed composite assignment.
External review-build log SHA256:
`1b8c6580b651e247afb02aae0877268b37c842f14d650743f631d634199a41bf`;
review-diagnostic log SHA256:
`453adbde7af8612719fc85dda450130dc156da637137e22ec0cd84938f81822f`.

### Reference validation allocation review

Fresh review of `9b00ffaf` found an uncharged temporary inlet copy in stage
validation. Standalone averaging retains three stages; at shape 4×555×826 its
three-stage payload is 2,147,474,448 bytes, below the unchanged 2-GiB cap, but
the additional 11,100-byte mask/flux copy exceeded that cap. Validation also
introduced a throwing allocation outside some callers' allocation handlers.
The repair removes the owning temporary: the same predicate reads the stage's
operands by const reference, with its existing shape and boundary. No scratch
payload remains to charge, no allocation-failure mechanism is added, and no
arithmetic, identity preimage, or numerical bound changes. Both generated
precision variants receive the repair. Other predicate callers already pass
their owning request by reference and introduce no analogous copy.

The independent numerical/sibling review of `9b00ffaf` found no P1/P2; the
allocation finding is repaired. The rebuild has no warning/error diagnostics;
the native inlet diagnostic and full Metal/fp64 kernel sweep both exit 0.
The inlet diagnostic log is byte-identical to the pre-repair diagnostic.
Both generated-source checks pass. Whole-owner inlet conformance remains pending.

| External log | SHA256 |
|---|---|
| `burner-reference-no-copy-build.log` | `3717f883d790aa642633e92fd69161274f4c75becc559fc95ea3dcb085b6276c` |
| `burner-reference-no-copy-diagnostic.log` | `453adbde7af8612719fc85dda450130dc156da637137e22ec0cd84938f81822f` |
| `burner-reference-no-copy-sweep.log` | `93fe6fb7a4ecd7bdbcc18ad87a6cf02d7575df2a8bfcd64930e616ce76a954a1` |

Two fresh reviewers of `0fba9694` confirmed the no-copy repair and unchanged
predicate semantics across both generated variants. The allocation review
also found an older adjacent exception escape: the standalone compatible-Heun
momentum adapter copied its five request arrays before entering the downstream
allocation handler. Its own `bad_alloc` handler now returns false with an empty
result and attempts to report the named allocation refusal without allowing
diagnostic allocation failure to escape; in that case error text can remain
empty or stale. This changes only allocation-failure handling, not successful arithmetic.
A dedicated persistent-allocation-injection fixture is not claimed here; no
general allocation-instrumentation framework is added to this sprint.

The exception repair rebuild is warning/error-free; native reference diagnostic
and full kernel sweep exit 0; generated mirrors pass `--check`. External logs:

| External log | SHA256 |
|---|---|
| `burner-reference-allocation-build.log` | `3717f883d790aa642633e92fd69161274f4c75becc559fc95ea3dcb085b6276c` |
| `burner-reference-allocation-diagnostic.log` | `453adbde7af8612719fc85dda450130dc156da637137e22ec0cd84938f81822f` |
| `burner-reference-allocation-sweep.log` | `64bfb017cbbb046aba80e47fe2616164e6e3686b856f0c05ed52aa66ee50e19b` |

Final fresh review of `10bc2bd6` found no P1 code findings. Its P2 documentation
finding is corrected above: the error string is best-effort under persistent
allocation failure, while false/empty-result refusal is preserved. No new
numerical acceptance claim or production admission follows from these repairs.

### Inlet owner gate: existing r201 trace extension (in progress)

The dedicated `--fire-production-burner-owner-gate` route reuses the existing
r201 per-iteration and per-field gate with the actual CPU32/generated-fp64
owners. It does not grant production admission. The earlier diagnostic-only
route remains separate. The inlet route must carry the same mask, prescribed
flux, and injected tuple through all reconstructed trace requests.

The first trace attempt exposed an omitted boundary case in the test adapter:
its terminal streaming calculation assumed every wall face had zero velocity,
whereas the inlet face is fixed at j/rho_face. This yielded a false streaming
refusal at R0 iteration 0 despite a matched residual; continuing with unavailable
terminal certificates later crashed the fixture. The adapter is being extended
to the prescribed face calculation (unchanged local rounding operations), and
the fixture now refuses immediately when projection or terminal-parent evidence
is unavailable. No solver arithmetic or bound is altered.

The physical-flux trace separately encloses `(j Q_in/rho_in) - u Q_cell` on
prescribed faces, while composite scalar low flux is assigned directly from
`j Q_in/rho_in`, not reconstructed by cancellation. Force-term traces distinguish
ordinary wall zero force from inlet buoyancy and half-cell source restriction.
All these branches must pass the existing local gate before any conformance
claim; omission mutants, cold-start supply census, and launcher remain pending.

The initial mixed-composition 4³ fixture passed the per-iteration and terminal-field
conjunction against the existing fp64 owner. **Review subsequently rejected its
inlet streaming-bound derivation; this table is diagnostic history, not current
acceptance evidence** (repair below). This is **one fixture**, not full
inlet qualification or a window result. The streaming RED doubles a prescribed
flux while holding the projected output fixed and is refused by the velocity
identity. Production inlet refusal is checked explicitly; it is not mislabeled
as a qualification-control refusal. Historical r136 streaming arithmetic is
untouched (its input domain has no prescribed inlet).

The table pairs the residual and its local bound at the **same index**, selected
by each field's worst residual/bound ratio; units apply to both columns. Every
entry was gated locally. Values here are rounded for readability; the SHA-bound
log retains full precision and per-iteration quantities. A zero/zero entry is
exact equality, recorded with ratio zero, not division by zero.

| Field | Units | Worst-index residual | Same-index bound | Worst ratio |
|---|---|---:|---:|---:|
| R1 provisional momentum | kg m⁻² s⁻¹ | 6.41346e-15 | 4.56261e-14 | 0.140566 |
| R1 Heun advection | kg m⁻² s⁻² | 5.52740e-11 | 1.39494e-10 | 0.396248 |
| R1 Heun buoyancy | kg m⁻² s⁻² | 0 | 0 | 0 |
| R1 Heun stress | kg m⁻² s⁻² | 7.10909e-11 | 5.57597e-9 | 0.0127495 |
| R1 Heun phase source | kg m⁻² s⁻² | 3.69287e-14 | 1.17768e-12 | 0.0313571 |
| R1 alpha | 1 | 0 | 1.22078e-4 | 0 |
| R1 eddy viscosity | m² s⁻¹ | 1.08741e-12 | 5.05225e-10 | 0.00215233 |
| R1 pressure ratio | 1 | 1.23941e-7 | 1.22078e-4 | 0.00101526 |
| R1 EOS deviation | 1 | 1.23941e-7 | 1.22078e-4 | 0.00101526 |
| R0 projection target | s⁻¹ | 8.99453e-8 | 0.448069 | 2.00740e-7 |
| R0 accepted target | s⁻¹ | 8.99453e-8 | 0.671148 | 1.34017e-7 |
| R1 projection target | s⁻¹ | 1.75990e-7 | 0.424627 | 4.14457e-7 |
| R1 accepted target | s⁻¹ | 1.75990e-7 | 0.606937 | 2.89964e-7 |
| R2 projection target | s⁻¹ | 3.41048e-7 | 0.225134 | 1.51487e-6 |
| R2 accepted target | s⁻¹ | 3.23367e-7 | 0.225108 | 1.43649e-6 |
| rhoZ | kg m⁻³ | 2.53096e-9 | 3.32159e-5 | 7.61972e-5 |
| CH4 | kg m⁻³ | 2.48339e-9 | 3.30707e-5 | 7.50933e-5 |
| O2 | kg m⁻³ | 9.97967e-9 | 3.26207e-5 | 0.000305931 |
| CO2 | kg m⁻³ | 3.52316e-8 | 5.33293e-5 | 0.000660643 |
| H2O | kg m⁻³ | 2.49149e-10 | 2.18983e-5 | 1.13775e-5 |
| N2 | kg m⁻³ | 5.99597e-10 | 6.70040e-5 | 8.94868e-6 |
| CO | kg m⁻³ | 3.41586e-14 | 1.44060e-5 | 2.37114e-9 |
| carbon | kg m⁻³ | 1.80725e-14 | 1.44062e-5 | 1.25450e-9 |
| sensible energy | J m⁻³ | 0.0515950 | 49.6716 | 0.00103872 |
| momentum x | kg m⁻² s⁻¹ | 2.63527e-10 | 7.42026e-9 | 0.0355145 |
| velocity x | m s⁻¹ | 4.07160e-10 | 1.14518e-8 | 0.0355542 |
| momentum y | kg m⁻² s⁻¹ | 2.86415e-10 | 1.97046e-8 | 0.0145355 |
| velocity y | m s⁻¹ | 9.83593e-10 | 6.69282e-8 | 0.0146962 |
| momentum z | kg m⁻² s⁻¹ | 3.55536e-10 | 1.22083e-8 | 0.0291225 |
| velocity z | m s⁻¹ | 1.03229e-9 | 3.54760e-8 | 0.0290984 |
| temperature | K | 1.35080e-4 | 0.173964 | 0.000776484 |

External gate log `burner-owner-gate-v3.log` SHA256
`8362ce8fa013df2da06e10d52d60555270f2845bfcce7c26de173c2c0984c9b6`;
warning/error-free build log `burner-owner-gate-build-v6.log` SHA256
`7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21`.
The first adapter-domain failure/crash is retained in `burner-owner-gate-first.log`,
SHA256 `c760ea122dadd97a4cf103ecd8b952d875896ded8db7198e17bec39559c1ff1e`.
The unchanged cold-flow full kernel sweep exits 0 (`burner-owner-gate-cold-sweep.log`,
SHA256 `43cab375c23ee286ef5ebdd703d0dcb38f6324b7e0811660b79192389bf20134`);
both generated mirror checks pass. Clean/Opto qualification is still pending.

#### Review repair: prescribed-boundary parent lift

Review of `a50486a0` found that reconstructing fixed inlet velocity only from
rounded density discarded its cross-precision density-parent displacement.
Pressure residual alone cannot control a changed prescribed normal velocity:
two constant throughflows can both have zero divergence residual. The repair
uses the existing trace quotient `j / rho_face` with the density's full center
and radius at the prescribed face. Its radius enters the existing streaming
norm, and the same quotient enters the existing divergence residual trace.
In error-decomposition terms, the fixed-boundary lift `b` contributes both
`||b||` and `||D b||`; the unchanged pressure inverse then bounds the homogeneous
correction. No solver tolerance, EOS envelope, or measured residual is used to
choose an allowance. An uncertainty-inflation RED preserves the rounded density
while enlarging its admissible parent interval and requires both lift terms to
survive. The earlier flux-doubling RED tests rounded identity, not this property.

The same review found missing RED aggregation at the inlet route's early return
and malformed trace-output indexing. The route now conjuncts the computed
applicable REDs (including the earlier cold-cycle controls, without claiming
inlet-specific cycle coverage). Streaming evidence checks shape-required array
extents before indexing; empty/truncated face arrays, pressure, target, and
boundary-class outputs are refused. The terminal-field helper also refuses
empty/empty evidence. Native requalification passes both the inlet conjunction
and cold full kernel sweep, with no build warnings/errors. The parent-lift RED
requires 3.9424855652421726e-6 m/s (L2 per cell) and exports
4.4353148063273082e-6 m/s; changed-flux and four malformed-output REDs pass.

Current same-index target rows supersede the six historical target rows above;
all other terminal-field report lines are byte-identical to that earlier table's
full-precision source. The current per-iteration, per-field, and RED conjunctions
are each 1; production admission remains 0. Fresh review of this repair is pending.

| Field (s⁻¹) | Worst-index residual | Same-index bound | Worst ratio |
|---|---:|---:|---:|
| R0 projection target | 8.994530531492018e-8 | 0.4480693157584155 | 2.007397118070361e-7 |
| R0 accepted target | 8.994530531492018e-8 | 0.6711483863183928 | 1.340170179180765e-7 |
| R1 projection target | 1.759896522957050e-7 | 0.4246280413825427 | 4.144560300885965e-7 |
| R1 accepted target | 1.759896522957050e-7 | 0.6069383432921384 | 2.899629826336341e-7 |
| R2 projection target | 3.410482176668950e-7 | 0.2251337879223904 | 1.514869095457424e-6 |
| R2 accepted target | 3.233668022240011e-7 | 0.2251083859774340 | 1.436493806394299e-6 |

| External log | SHA256 |
|---|---|
| `burner-owner-gate-review-build.log` | `f3327e77311468163faf52f2efb4f872b255489dfc266806af62c2e7e6583ef7` |
| `burner-owner-gate-review.log` | `5cdd4220e3aca8a5a676ffa63d8afea07bdec92767fd51dd2a7d3ebe4d6c76cf` |
| `burner-owner-gate-review-cold.log` | `21546efafe33a37f5c4f9f4eeb570e8f5586b9cf8ba257ca9a223b433f8b7341` |

Final review receipt for `d850ad2d`: a reviewer independent of the inlet-gate
increment returned no P1/P2, and the numerical reviewer performed an explicitly
identified correction audit and returned no new P1/P2. A new reviewer spawn
hit the task-thread limit, so this is not described as two fresh reviews.
Both confirmed the parent-lift repair and disclosed scope. The independent
reviewer verified the three log hashes above; neither reran builds.
Independent omission of each lift contribution is a useful additional RED;
the current density-inflation RED checks their exported lower bounds together.
That coverage improvement does not replace the still-required device force-mask
and imposed-scalar-flux omission mutants, cold-start supply census, or launcher
qualification. No production inlet run is authorized by this fixture receipt.

### Actual device omission experiments after the inlet gate

Two temporary source mutants were built separately against `343ddb1c` and run
with `--fire-production-burner-owner-gate`. Neither mutant is retained in the
source or admitted for production. Both exited 222, while the preceding normal
gate exited 0. These are actual device wiring/arithmetic mutations, not changed
host expectations.

- Force-mask omission: replace the resident owner's
  `rhs.bottomFuelMask=inletForceMask_` with `nil`. At R1 Heun stress index 176,
  residual 7.4212084192682625e-6 kg m^-2 s^-2 exceeds the **same-index** enclosure
  1.013566875895516e-8 kg m^-2 s^-2 (ratio 732.18734705703639). Phase-source
  index 165 has residual 9.3832004594068209e-9 against 5.7274006344423008e-13
  in the same units. The terminal per-field conjunction refuses; the broader
  per-iteration bounds alone did not catch this mutant.
- Imposed-scalar omission: multiply the numerator of device `pf_imposed` by
  zero, consistently affecting centered physical flux, composite low/high
  flux, and their on-device identity check; keep projection's imposed mass
  flux intact. The independent R0 iteration-0 CH4 physical-flux check refuses
  (worst local residual/bound ratio 981.50485192291694, kg m^-2 s^-1 on both
  sides). Its printed maxima are not a same-index pair and are not presented
  as one. The FCT input enclosure refuses before terminal transport authority
  is available. Later zero-bound transport lines are invalid-parent fallout,
  **not** a qualified interior species/enthalpy comparison. Thus this experiment
  proves actual device omission detection, but does not yet complete the
  separately required interior species/enthalpy omission coverage.

The cold-start census audit located the original boundary-aware absence test
in `tools/fire_simulator_3d_advance.h`: a species is absent only if absent from
all cells and from every available ambient/fuel supplier. Its subsequent
normal projection and exact absent-row slope zeroing are also consumers.
The production launcher currently supplies an identity coordinate projector;
cold-start integration must address the complete reconstruction path, not just
the boundary flux overwrite. This is pending, not a passed census.

External files remain under the existing r239 calibration directory:

| Artifact | SHA256 |
|---|---|
| Force mutant source | `a0d37c1ddec83172aa4b8400d92c0a809800af690ee45eafa8da7e8abd3db64b` |
| Force mutant executable | `a6123acd33dbce47fc9e94c898267ce1e1106838825a845f6c207fc8abab2a26` |
| `owner-force-omission-build.log` | `7c304e110f0b5be9758d73cd726696cfa61a609d43a5c8b7eb977a8c16fa3f51` |
| `owner-force-omission.log` | `6698fcf2c3d99f2b83244f7bf5c1b2606d1f2ade5fdcab88f8c9fad8a4d0454b` |
| Scalar mutant source | `4a5325fe00735989db4aa2d202fd43b32c49f8df668c13d896cda994e73f8370` |
| Scalar mutant executable | `3b2470c4a572ad17ce47d655c429ab60c54ba66834ce2eeba875ac4e07ea4e36` |
| `owner-scalar-omission-build.log` | `7c304e110f0b5be9758d73cd726696cfa61a609d43a5c8b7eb977a8c16fa3f51` |
| `owner-scalar-omission.log` | `58b14069239c7e9917d69871de309fd6187804d7f722346505e0c0135cae9885` |

Restored source SHA256 is
`92bd6161687bdbd1843ecc00d91f2db81658d61b12f3a6c219629a4d99ff3f1a`,
identical to the pre-mutation file. No production admission is inferred from
these narrow experiments. Clean/Opto and coherent launcher qualification remain
pending.

The restored incremental build has no warnings/errors. The restored executable
SHA256 is `e5f70fce696504ffba31497ef50156ce2dcfff4d1229dc6819e326c4c42e9567`.
Its inlet owner gate exits 0 with all three conjunctions equal to 1 and production
admission still 0. `owner-omission-restored.log` SHA256 is
`5cdd4220e3aca8a5a676ffa63d8afea07bdec92767fd51dd2a7d3ebe4d6c76cf`,
byte-identical to the preceding reviewed green log. The restored build log
SHA256 is `7c304e110f0b5be9758d73cd726696cfa61a609d43a5c8b7eb977a8c16fa3f51`.
The restored full cold kernel sweep also exits 0;
`owner-omission-restored-cold.log` SHA256 is
`f4db84e524b82ba2c6dfebac5c611a2a4fc62832df2c6e1fed265e79e8759c63`.

### Cold-start input probe: not yet qualified

A temporary diagnostic changed only the existing inlet owner-gate input, after
its earlier mixed-state controls: every cell and the ambient tuple were built
with `buildState(300.0, 0.0, 1.0)`, momentum remained zero, and the existing three
prescribed fuel faces and flux were retained. The canonical source was rebuilt
from these exact conservative bytes; its temperature/source arrays were carried,
and ambient density was summed from the six gas constituents. Both owners used
this same input. No solver or kernel arithmetic was changed.

Metal and fp64 both accepted this one step. The **comparison gate exited 222**:

| Field | Index | Residual | Local enclosure | Units |
|---|---:|---:|---:|---|
| R0 projection target | 0 | 1.2358403710110988e-10 | 0 (invalid derivation return) | s^-1 |
| R1 Heun stress | 68 | 4.5554029445375891e-14 | 1.3257209335075865e-14 | kg m^-2 s^-2 |
| sensible energy | 5 | 7.5105652959114224e-14 | 5.8145163065646271e-15 | J m^-3 |

The R0 target is 0.0055272472091019154 on device versus
0.0055272470855178783 in fp64; its binary32 projection is bit-identical. Zero
here is not a certified zero-error tolerance: `targetTermwiseEnclosure` uses
zero as its invalid-derivation return, and this path requires clause-level
diagnosis before any bound is claimed. No enclosure has been changed. The
Heun FCT rounded trace also differs at index 1 (6.67148628e-14 versus
6.67148425e-14). Subsequent audit below establishes that this is descriptive,
not a failing numerical clause: different streaming groupings are already
covered by the producer's local enclosure.
Several mixed-state mutation controls lose sensitivity on this uniform input
(for example, recomputing alpha gives the same alpha). They must remain tested
on the mixed fixture, not be represented as solver defects or waived controls.

This probe does not establish that missing absent-species reconstruction caused
any of these failures. The original oracle's `boundaryCanSupply` member is
populated and combined at Heun stages but is not read by `ApplyOpenSharedFCT3D`;
the actual boundary-aware absence decision lives in `BuildOpenFluxPair3D` and
controls slope reconstruction. The correction must follow that live consumer,
not add a ceremonial limiter flag. Production inlet admission stays closed.

| Probe artifact | SHA256 |
|---|---|
| Temporary test source | `4c963e049460aaf061bfce57362c954c6a19b5b7a3d4c4ee43fb521e86cb427a` |
| Probe executable | `6b4d24775c7c452bb26aaf5a5af8d59227da86ee9d3aae34f0e1d602ebe66446` |
| `owner-cold-start-probe-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `owner-cold-start-probe.log` | `dbfd252eca9e76d3adf20741b60934de0db257ce42d74a61404945e2d9930e3d` |

The temporary test edit was removed. Restored test-source SHA256 is
`7271052705b7915e98c08bbf86c9d46ea0830656cc1a6a1dff0f21dfda57ab17`,
identical to the committed file. This is a diagnostic fixture result, not a
window-run result, and supplies no empirical-row claim.
The restored build is warning-free and the normal inlet owner gate again exits
0. `owner-cold-start-restored.log` SHA256 is
`5cdd4220e3aca8a5a676ffa63d8afea07bdec92767fd51dd2a7d3ebe4d6c76cf`,
identical to the reviewed mixed-state green result. No temporary probe remains
in the executable or source.

#### First cold target refusal localized

Clause-only instrumentation on the same cold input identifies the first invalid
return, not an EOS refusal. At R0 bootstrap, cell 0, component 3 (N2), the
reference coefficient is 0.87876549629915557 m^3/kg while the interval's upper
endpoint is 0.87876549629915546 m^3/kg: **one binary64 ULP** apart. Its lower
endpoint is 0.8787654440195265 m^3/kg. Temperature interval is
[300, 300.00001788139451] K; molar-density endpoints both equal
0.040621988691680225 kmol/m^3. The guard at
`targetTermwiseEnclosure`'s `mirrorCoefficient` range check returns invalid
before the subsequent arithmetic enclosure is assembled.

The proof and reference use different arithmetic for the same mathematical
coefficient: the proof evaluates `1/(molarDensity*W) - h/(C_T*T)` whereas
`DivergenceFromDiscreteRateByComponentOrder` evaluates
`(rho/molarDensity)/(rho*W) - h/(C_T*T)`. The former endpoints are calculated
with ordinary nearest-rounded arithmetic, not outward-rounded interval
operations. An algebraically derived coefficient range is being used as if it already
enclosed the evaluated binary64 reference. This is a validation-formulation
defect; the measurement does not authorize an EOS tolerance change or a
solver arithmetic change. The needed correction is operation-wise outward
enclosure of the reference expression (including its actual division/product
order and upstream density, molar-density, heat-capacity and property
evaluations), not a residual-sized epsilon or suppression of the failed clause.
The later stress/energy checks remain unresolved; fixing this first clause
alone will not qualify cold start. The rounded FCT trace is diagnostic only,
as clarified by the subsequent parent-proof audit below.

Evidence: temporary diagnostic source SHA256
`276953427dbe044b2a329f405d93c6530b2ae35c5d2181265bad99d279ce5489`,
executable `d20e3b5eeafdb3d874b5db76643496f95f3c7bda501e9d3bef0fc7890c78a507`,
`owner-cold-target-clause.log`
`5c71d2f897d3b0aef8634c83ecba0a42d054cafdb6515a5decc80ab1bd620167`,
build log `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21`.
This diagnostic again exits 222. Temporary source instrumentation and cold
input substitution were removed; no gate or arithmetic change is committed
with this finding.
Read-only numerical diagnosis audit verified the evidence SHA, units, species
index, exact one-ULP separation, and invalid-return control flow. The two wording
corrections above avoid claiming either exact-real interval certification or
causal independence of the later failures. This is not implementation sign-off.
The restored warning-free build's normal inlet gate exits 0;
`owner-cold-target-restored.log` has the unchanged green SHA256
`5cdd4220e3aca8a5a676ffa63d8afea07bdec92767fd51dd2a7d3ebe4d6c76cf`.

#### Target proof correction (qualification-only; review pending)

The existing `Binary64Interval` walker now follows the reference's evaluated
coefficient expression, without algebraic cancellation of rho. It accumulates
gas density, molar density and volumetric heat capacity in the reference's
order, then evaluates every basis-rate term. The thermochemistry API's returned
binary64 cp and h values are exact leaves for this **evaluated-reference** DAG;
the existing temperature/property sensitivity range remains a separate input
uncertainty calculation. This does not claim a new proof of libm or the
thermochemistry record, and it changes neither of them.

The finite outward interval must contain the existing reference functional's
returned coefficient. Center bit equality is not assumed: the record library
permits contraction/reassociation while the calibration translation unit is
strict. A measured cold point has center 0.87876549629915535 versus library
return 0.87876549629915557, both inside the fixed input-derived interval
[0.87876549629915213, 0.8787654962991589] m^3/kg. Membership is explicitly
checked at every value and fails closed; no theorem of coverage for arbitrary
compiler transformations is claimed. A read-only build-contract audit confirmed
that an initial center-equality guard imposed an unauthorized extra invariant.
That candidate was rejected (exit 222), not presented as a solver defect. The
coefficient range is the hull of this independently input-derived arithmetic
interval and the existing physical-sensitivity range. No measured target
residual is an operand of the derivation; no empirical epsilon, reserve
constant, source kernel, EOS gate or production value changes. This repairs
the mismatch between the proof's object and the evaluated reference, rather
than admitting the target by its observed discrepancy.

A cold 300 K point regression is in the normal gate, distinct from the mixed
coupled fixture. It requires the old algebraic range to refuse and the traced
reference range to validate. Its verdict is conjoined with the iteration gate.
Passing that subcomponent does not remove the other cold coupled failures or
authorize inlet production. Full cold-start qualification and independent
review of this correction remain pending.

Native qualification of this correction: the inlet fixture and full cold
kernel sweep both exit 0; the rebuild has no warnings/errors. The new cold
point reports `algebraic_range_refuses=1 outward_reference_valid=1 passed=1`.
All terminal per-field report lines are byte-identical to
`owner-omission-restored.log`, so the preceding same-index table still applies
to this mixed fixture. No coupled cold-input acceptance is claimed.

| Artifact | SHA256 |
|---|---|
| `target-reference-membership-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `target-reference-membership.log` | `4646709bc98f5aecb685615dd0b816329ab0f404595319210516f2fcf66ad1a4` |
| `target-reference-membership-cold.log` | `c81db8c3cde908466b8951dae0ee9060b9c9543a992e87f26ba9528179b58d08` |
| Rejected center-equality diagnostic `target-reference-center.log` | `82fede0c32201346d9634e0e608a5d46a0b304d1656033ed1e41df7796dd13c8` |

Two fresh independent reviewers of `43017a03` returned no P1/P2 on
mathematical scope and test/evidence integrity. Neither built or edited.
The mathematical reviewer confirmed the hull construction and distinguished
current-value membership from a universal fast-math theorem. The integrity
reviewer verified the documented log hashes, unchanged terminal field lines,
and sticky regression failure through the final gate. The cold witness is a
regression of this native build's old refusal; compiler changes can remove
that original discrepancy. The zero-flux point tests coefficient admission,
not downstream propagation; nonzero-flux hull-mutation coverage remains a
possible strengthening, not a claim made by this test. Full coupled cold
qualification, its other measured failures, and production admission remain
pending. This closes only the target-reference proof correction.

## Reproducible cold probe and terminal-parent diagnosis

`--fire-production-burner-cold-start-probe` now reproduces the uniform 300 K
air input without temporary source edits. It runs the same owner/reference
comparison and retains every mutation obligation. It is diagnostic only:
exit 222 is still a refusal, never production admission. The mixed-input gate
remains the fixture for mutations which become insensitive on uniform data.
In particular, `cpu_surrogate_rounded_match` is only printed; it is not the
Heun acceptance predicate. The cold producer certificates for R0 and R1 both
pass. The cold Heun control fails because the CPU-trajectory substitution
mutant is also enclosed, not because the rounded diagnostic differs.

All cold target trajectories now pass (four iterations per stage). The two
remaining terminal numerical refusals discard existing parent uncertainty:

| Quantity/location | Units | Measured residual | Old terminal bound | Existing parent propagation (diagnostic, not substituted) |
|---|---|---:|---:|---:|
| R1 Heun stress, axis 0 face 68 | kg m^-2 s^-2 | 4.5554029445375891e-14 | 1.3257209335075865e-14 | 2.454378102575011e-6 |
| Sensible energy, cell 5 | J m^-3 | 7.5105652959114224e-14 | 5.8145163065646271e-15 | 6.1197836536151409e-7 |

The stress diagnostic applies the existing operation walker to
`0.5*(R0+R1)` with each stage's already-derived stress enclosure. The rounded
result bit-matches the published Metal average. The scalar diagnostic finds
that the published device values match the certified Heun producer bytes and
the fp64 endpoint equals its Heun accepted state. These parent bounds are
much looser than the observed errors; this is disclosed, not a new tightness
claim. No acceptance expression or numerical bound is changed by this probe.

The next correction is proof composition, not a tolerance adjustment: a copy
inherits its actual producer proof; an average propagates both actual parent
proofs and its arithmetic. It must fail closed on missing proofs, incorrect
candidate identity, mismatched endpoint bytes or stage correspondence. A
read-only independent audit confirmed the data flow: Metal blits the R1
candidate unchanged, and the fp64 owner checks its committed endpoint against
the Heun accepted state. The same discarded-parent pattern occurs in the
other Heun rate averages and provisional momentum; those consumers must be
audited together. Eddy viscosity needs its own transport proof, not a momentum
rate bound. Alpha and represented pressure are different producers.

Three report label lists were also corrected to the authoritative component
order `rhoZ, CH4, O2, N2, CO2, H2O, CO, carbon, sensible_energy` (transport,
physical mass flux, terminal scalars). Historical reports are not rewritten:
their N2/CO2/H2O labels at components 3/4/5 were permuted, while their numeric
vectors, units and acceptance conjunctions were unchanged. A sibling search
in tests/tools found no other copy of this ordering error; the generator's
alphabetical transport-property table is a separate species-name lookup.

External evidence in `rendered/fire_production_calibration/r239_burner_projection/`:

| Artifact | SHA256 |
|---|---|
| `cold-parent-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `cold-parent-probe.log` (exit 222) | `9d97b52db0403a038877b4bfb6b1be3c7ea9f06996d588b4bbf9b76d4004d62b` |
| `cold-parent-mixed-regression.log` (exit 0) | `c278096d9f18f6a07c503d8185bf9140cdecd46b149b646133e2752341fb9601` |
| Diagnostic executable | `41ff92e6ab7f53f42667369205a67d320f8c6af6f4e34a6564e66b7028351113` |
| Diagnostic test source | `2a8764c8c7e5594009e6692a0aebdfc743d75454472bc25e7b8e79e229063777` |

Only the test translation unit and generated Job translation unit rebuilt;
the build log contains no warnings. No clean-build/Opto qualification, restored
window, cold-start acceptance or launcher integration is claimed here.

Two fresh independent reviewers of `f962fcc8` returned no P1/P2: diagnostic
integrity/evidence and mathematical scope. They verified that no acceptance
predicate changed, the fixed indices fit the fixture, the labels follow the
species enum, and the parent-propagation numbers match their stated scope.
The integrity reviewer independently checked all five artifact hashes. Neither
built or edited. This closes the diagnostic increment only; future acceptance
changes still require the documented endpoint/stage binding and mutation REDs.

## Scalar publication proof composition

The terminal scalar comparison now consumes the existing R1 producer proof,
instead of rebuilding an output-relative heuristic from the R1 Euler fluxes.
The published operation is a copy, so it introduces no additional arithmetic
error. Admission of that proof requires its availability, nonzero matching
R1 candidate identity, exact device endpoint bytes, exact fp64 endpoint bytes,
the full `9*cells` extent, and finite nonnegative per-value bounds. The fp64
endpoint bytes are saved when the existing producer proof succeeds, not
reconstructed from the final output. Missing proof never defaults to success
even when the measured residual is zero.

The binding REDs remove availability, forge the candidate identity, change a
device value by one ULP, change a reference value by one ULP, and remove the
device payload. Each must refuse, and the RED conjunction contributes to the
existing owner gate. The cold probe additionally requires the retired
parent-dropping heuristic to reproduce its refusal. That old computation is
retained only as a diagnostic RED, not as an alternative acceptance bound.

This reuses the previously qualified producer enclosures unchanged, including
their disclosed looseness; it does not derive bounds from the final measured
residual. All nine conservative components use the same authenticated copy
rule, in their respective mass-density or energy-density units. No production
kernel, authority surface, EOS threshold or solver arithmetic is changed.
The stress/rate-average siblings remain pending and cold-start production is
still unqualified; passing this scalar comparison alone cannot admit it.

Native cold comparison, at each field's worst residual/bound cell (all mass
rows in kg m^-3; energy in J m^-3). Full precision is retained in the log.

| Field | Cell | Residual | Inherited local bound | Ratio | Scalar verdict |
|---|---:|---:|---:|---:|---|
| rhoZ | 0 | 4.404547116e-14 | 5.641577287e-13 | 0.0780729731 | pass |
| CH4 | 11 | 4.223073619e-14 | 6.428820519e-13 | 0.0656897110 | pass |
| O2 | 0 | 8.026674103e-9 | 1.187726401e-7 | 0.0675801607 | pass |
| N2 | 21 | 7.677803093e-8 | 3.943760248e-7 | 0.1946822984 | pass |
| CO2 | 5 | 7.028170067e-15 | 2.494360272e-13 | 0.0281762428 | pass |
| H2O | 11 | 1.867754015e-14 | 2.450602838e-13 | 0.0762161043 | pass |
| CO | 5 | 6.149474792e-15 | 1.734804517e-13 | 0.0354476526 | pass |
| carbon | 11 | 2.914551582e-15 | 8.594347018e-14 | 0.0339124261 | pass |
| sensible energy | 11 | 5.753637363e-14 | 4.094041984e-7 | 1.405368432e-7 | pass |

The cold probe still exits 222: stress and insensitive mixed-fixture REDs
remain unresolved. The mixed inlet gate exits 0, including the publication
binding RED. Its sensible-energy worst cell 11 has residual
0.031289969338104129 J m^-3 against 3.1006727809320922 J m^-3, ratio
0.01009134841010152. No production output changed.

| Artifact (same external directory) | SHA256 |
|---|---|
| `scalar-publication-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `scalar-publication-cold.log` | `5faee262c00c45ce2c2bc857f0f482f8e396b3eb6f7d57a26c11b7435b4ea379` |
| `scalar-publication-mixed.log` | `0a578a9f6f7603fd46e92d23adf21e4a6cf21f5193b504473c5a0c91a7e88b4e` |
| `scalar-publication-sweep.log` (native kernel sweep, exit 0) | `5376d5fff0820f155d0f8630855ea3dc7fa8b7d26f1aedf3fc3f3fbe2ce930c3` |
| Test source | `a26797215dc15d1626dcba1b635e57cdca773319ffde251c8e04de4ef55b9731` |
| Executable | `bbc5e014282f0ee367938b8e057f6ceb43649207ef27759f094d91cf56e3b051` |

The focused rebuild has no warnings; this is not a clean/Opto qualification.

Fresh mathematical and gate-integrity reviews of `3999455a` concluded no
P1/P2. The integrity reviewer initially reported a missing non-burner final
conjunction, then retracted it after tracing the unconditional terminal-stage
loop: `ownerIterationTraceBounded` feeds `ownerMirrorBounded`, which feeds
both return modes. The proposed false-green is therefore unreachable; no
redundant mechanism was added. All six recorded hashes were independently
verified. The mathematical reviewer confirmed the actual Metal copy and fp64
endpoint checks. This review closes scalar publication only, not cold-start
qualification or the pending stress/rate siblings.

## Nonadvective average composition: scope before implementation

The next repair covers the three published nonadvective averages: buoyancy,
stress and phase-source momentum rate. Each is exactly `0.5f*(R0+R1)` on
device. Reuse each terminal rate's existing enclosure only with its stage,
candidate identity, terminal iteration, actual device/reference bytes and
shape bound to the producer which earned it. Propagate through the existing
add/multiply walker; require finite bounds and the local parent residuals.
Missing or swapped parent and changed-input mutations must refuse.

The earlier suggestion to apply a negative half to the existing R0 advection
bound needs correction: published Heun advection recomputes R0 under **R1's
shared alpha**, while `terminalRateBounds[0][3]` covers the original R0 alpha.
Those are not interchangeable parents. Advection and the provisional-momentum
proof therefore need an explicit shared-alpha audit, not mechanical reuse of
the original R0 bound. Eddy viscosity separately needs its transport proof.
These are qualification-consumer issues; no operator change or additional
runtime authority is authorized by this note.

The three nonadvective consumers now compose their existing local parent
bounds. Qualification retains the terminal snapshots when the rate-parent
proof succeeds; reuse checks both stage identities, terminal iteration tags,
all face extents, exact device/reference rate bytes, and reference endpoint
correspondence. The copied bounds must remain finite and nonnegative and
enclose the local parents. Missing, swapped and changed device-parent
mutations refuse. The existing arithmetic walker then propagates the
addition and multiplication by one half. No residual is used to size it.

In addition, publication must bit-match `0.5f*(R0+R1)` evaluated on the exact
device parent bytes, separately from the fp64 comparison. A one-ULP output
mutation must fail this streaming check. Thus a loose inherited enclosure
cannot hide an incorrect averaging operation. All checks are conjoined with
the owner gate. The old stress magnitude-only bound survives only as the
cold diagnostic omission RED. Production code and every parent enclosure
derivation remain unchanged.

Native measurements below use each field's worst residual/bound face. Every
row has units kg m^-2 s^-2; no maxima from different faces are paired.

| Fixture / field | Packed face | Residual | Composed local bound | Ratio |
|---|---:|---:|---:|---:|
| Cold / buoyancy | 0 | 0 | 3.5032463696218031e-45 | 0 |
| Cold / stress | 197 | 8.7166565384790484e-13 | 2.149416453481636e-6 | 4.0553595485694557e-7 |
| Cold / phase source | 0 | 0 | 4.7115615027996174e-44 | 0 |
| Mixed / buoyancy | 0 | 0 | 3.5032463696218031e-45 | 0 |
| Mixed / stress | 7 | 3.8837343107125943e-10 | 0.0014120675480955322 | 2.7503884753605592e-7 |
| Mixed / phase source | 5 | 2.1683935082432399e-14 | 7.3336656239056055e-9 | 2.9567662604835857e-6 |

All six field checks and their exact-device-parent streaming checks pass.
The inherited stress bound is deliberately reported at its actual loose
scale; passing it is not a tight-error estimate. The missing/swapped/changed
parent REDs and changed-output REDs pass. The cold omission RED reproduces
the old stress refusal. The mixed owner gate exits 0; cold still exits 222,
with mixed-fixture mutation controls insensitive on the uniform input and
their downstream conjunctions still refusing. No production admission is
inferred. Focused build warning-free; clean/Opto remains unclaimed.

| Artifact (same external directory) | SHA256 |
|---|---|
| `nonadvective-streaming-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `nonadvective-streaming-cold.log` | `0f7bbd678398281d748ffbbaed1d6b7775101906b02f255d6d3c881573b85735` |
| `nonadvective-streaming-mixed.log` | `25ee2d8015c20ae6d2fc01106683d259a594f0a72e1e9aee0506afbd79fa2443` |
| `nonadvective-streaming-sweep.log` (native kernel sweep, exit 0) | `0c187bbe242e7eaaa2a526c7ca1322ed2d7cf3b519b1b7e0ff5260af9c88c3bd` |
| Test source | `0a8a80ba05c0ec50d130cff439dc4230628ab1b18f8a602a9e9827f6d78aefd4` |
| Executable | `bccbe89d31eb68d1dcb2a11b1f272227a1b4bba31a49e51bd6e029839a9e06da` |

Two fresh independent reviewers of `52f00f03` reported no P1/P2 on
mathematical/reference scope and gate/evidence integrity. They confirmed the
actual half-sum grouping, authenticated parent reuse, sticky refusals in both
owner modes, and the independent exact streaming check. The integrity review
verified all six artifact hashes and same-face table values. Neither built
or edited. This closes the three nonadvective publication consumers only;
the advection/provisional shared-alpha audit, eddy consumer, cold fixture
qualification and production launcher remain pending.

## Heun-alpha parent and provisional grouping audit

A read-only independent source audit confirmed that the existing R2
`composeProvisional` qualification consumer has two mismatches with the
resident DAG: original R0 alpha instead of R1's selected Heun alpha, and
`M0+(dt/2)*rate0+(dt/2)*rate1` instead of the Metal grouping
`M0+dt*(0.5*(rate0+rate1))`. This is a qualification-consumer defect, not a
production scheduling change. Metal explicitly recomputes the R0 rate using
R1's candidate alpha; the fp64 owner does likewise. The fp64 owner's actual
grouping is `M0+(0.5*dt)*(N0+N1-A0-A1)`. Its actual R2 provisional endpoint
must remain the reference, rather than a fresh regrouping of averaged fields.

Before changing that gate, the existing TraceFloat compatible request is
retained at the R0 terminal and re-evaluated with only its alpha replaced by
the R1 terminal alpha. The probe compares against the actual device and fp64
R1 `heunR0AdvectionRateKGPerM2S2` traces. It prints each face's residual,
existing trace radius, reference-center difference, old-alpha rate gap, and
rounded-byte comparison. It changes no acceptance predicate and adds no
runtime authority or production computation.

Both fixtures emitted all 240 face rows. The following are the worst
residual/radius rows; all quantities except the ratio are kg m^-2 s^-2.

| Fixture | Axis/face | Device–fp64 residual | Trace radius | Reference-center gap |
|---|---|---:|---:|---:|
| Mixed | 0/45 | 2.5880249093952756e-9 | 0.0024717632913109065 | 1.1050839063275752e-13 |
| Cold | 0/14 | 3.7837118709054722e-13 | 2.3581276784452986e-7 | 0 |

Every observed residual fits the diagnostic radius; the mixed trace has 83
rounded-byte differences and cold has zero. Each reports 240 branch
obligations; this probe does not newly discharge them or promote a proof.
Crucially, **both fixtures have zero old-alpha rate gap**. They therefore do
not witness the wrong-parent defect: the source audit names it, but these
inputs cannot validate its correction. The existing forced-limiter
discontinuity fixture must supply a nonzero-gap witness before that parent
repair is claimed. The mixed owner still exits 0; cold still exits 222.
No gate, bound, solver or kernel was altered by this instrument.

| Artifact (same external directory) | SHA256 |
|---|---|
| `heun-alpha-parent-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `heun-alpha-parent-mixed.log` | `9609b8e8ee08255d8e85130d15c08b46cc86507e008a260f908f684de64f6d9e` |
| `heun-alpha-parent-cold.log` | `cae16474cf732d0dd28228a7721919c6a2e7b3401f039350be6fb788d2005cbd` |
| Test source | `e956696677ad5b378083b00728947f56305758124dfbcf5c9433e2a8910a43f4` |
| Executable | `2ba254ed0ced9de82162bd3f1c306df3a1ae7e20dbd34863054b67de68b5b5fb` |

Focused rebuild warning-free; no clean/Opto or new production qualification.

Fresh mathematical/scheduling and diagnostic-integrity reviews of `90d45576`
returned no P1/P2. The reviewers confirmed R0 operand retention with R1 alpha,
safe trace extents/lifetimes, counter-scope restoration, unchanged acceptance,
the five artifact hashes and the zero-gap limitation. Neither built or edited.
This closes the instrument only; the nonzero-gap witness and parent/grouping
repair remain open.

### Forced-limiter witness for the Heun R0 parent

The existing `qualificationForceLimiterDiscontinuity` fixture now prints
the original R0 advection and the R0 advection recomputed with R1's selected
alpha, separately from the correct/mutant provisional momentum. This is
qualification-only instrumentation; no solver operation, acceptance predicate,
or enclosure changed. Array extents are checked before each per-face report.

The mixed fixture supplies the previously missing nonzero witness. Its largest
old-parent rate gap occurs at axis 1, face 15:

| Quantity | Original R0 alpha / mutant | Heun alpha / correct |
|---|---:|---:|
| Alpha at this face (dimensionless) | 1 | 0.5 |
| R0 advection (kg m^-2 s^-2) | 0.0033004153519868851 | 0.0030853559728711843 |
| Published provisional momentum (kg m^-2 s^-1) | -4.7552155990615574e-8 | -4.5911381363339387e-8 |

The rate gap is `2.1505937911570072e-4 kg m^-2 s^-2`; the momentum gap
on the same face is `1.6407746272761869e-9 kg m^-2 s^-1`. The latter is
the two full fixture executions' output difference, not a claim that it
isolates only this face's rate change. Their accepted alpha arrays are
bit-identical. Both fixtures emit 240 face rows with valid shapes. The cold
fixture still has zero old-parent rate gap and an insensitive mutation
control; it is not a witness for this defect.

Focused build: no warnings. Mixed owner gate exits 0; cold probe exits 222
with its existing refusal controls intact. No clean/Opto qualification,
new production run, or completed Heun-parent proof is claimed. Next repair
must retain the original R0 proof for R1, separately qualify R0 under R1's
Heun alpha for R2, and compose provisional momentum in the actual device
operation order against the reviewed owner's endpoint.

| Artifact (same external directory) | SHA256 |
|---|---|
| `forced-alpha-witness-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `forced-alpha-witness-mixed.log` | `f5dafbcb175c28e916bf128a8f108012316e91c69de00eecbe846dd7052f7265` |
| `forced-alpha-witness-cold.log` | `575fbe7b885429a0b29f665a013ea8fba1432529257e21ad57d8c4430a7c18dd` |
| Test source | `f1aa723bb2d3d24702468e8878654d6e9b9f643ee34c267946bae0fa66aa01ed` |
| Executable | `730a03f70794a45e9fd2def4493741b7d1f2200651ab8a3435d8ccb1f31e115c` |

Two fresh read-only reviews of `b3ed292e` returned no P1/P2: one checked
mathematical operand/consumer fidelity, the other checked indexing safety,
unchanged gates, all five hashes, finite rows, and the recorded outcomes.
Both explicitly leave the wrong-parent regression gate and repaired
operation-order proof open; this receipt closes only the diagnostic witness.

### Reviewed provisional endpoint, not a reconstructed reference

The final `R1_provisional_momentum` comparison now reads the reviewed fp64
owner's actual terminal R2 `provisionalMomentumKGPerM2S` trace. The owner
records its `heunMomentum_` directly there (the same array its projection
consumes). The test's separate assembly from averaged component rates was
removed: its different summation order was not the reference endpoint.
The consumer checks presence, terminal marker, topology-sized axes and
finite values; a missing-reference mutation refuses. This changes only
the qualification reference, not production arithmetic or an enclosure.

| Fixture | Worst packed face | Residual (kg m^-2 s^-1) | Existing local streaming bound (same units) | Ratio |
|---|---:|---:|---:|---:|
| Mixed | 28 | 6.4134565100706998e-15 | 4.5626054446101013e-14 | 0.14056566117605121 |
| Cold | 147 | 1.5430778486499547e-19 | 3.9096959193041634e-19 | 0.39467976039542924 |

These are the existing output-relative streaming bounds, **not a new
termwise Heun proof**. The correct R0-under-Heun-alpha parent proof, actual
device grouping, and replacement of that streaming heuristic remain open.
Both endpoint-presence/absence checks pass. Mixed gate exits 0; cold probe
still exits 222. Focused rebuild has no warnings; no full/Opto qualification
or production admission is claimed.

| Artifact (same external directory) | SHA256 |
|---|---|
| `reviewed-provisional-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `reviewed-provisional-mixed.log` | `f31656c5db462d752cf2c229b2aff9750af626588ccab66c659aad6ed89298ef` |
| `reviewed-provisional-cold.log` | `b7661a44bfd5eaae64b9a80baff823d0ce77a54f170fa74998f3b61732a28a0e` |
| Test source | `c3bdbdd42b600ada1199d4c26d0e8e9fee4f50a921d6449cb22c2d4ebb3cc1e4` |
| Executable | `6a91a61c253644b11fe17667270066333ff1e2f4a6bd2f94fabe67664c17101c` |

Fresh mathematical/consumer and safety/evidence reviews of `4a19b4d3`
returned no P1/P2. They confirmed the actual CPU/Metal projection-input
pairing, fail-closed reference extraction, sticky gate refusal, all five
hashes and both per-face measurements. Only the empty-trace path is directly
mutation-tested here; terminal-bit, extent and nonfinite guards were checked
by inspection, not independently RED-proven. Full parent/grouping repair
and production qualification remain pending.

### Compatible-rate alpha admission obligations

The original terminal-rate walker recorded 240 unresolved comparisons in
each stage but did not make their discharge a prerequisite of the rate
parent proof. These comparisons are the compatible momentum routine's
`alpha < 0` / `alpha > 1` admission guards. The trace generator now names
exactly that source seam with the existing `AdmissibilityGuard` class;
its source-occurrence check fails if the seam changes.

Derivation: both the fp64 center and the binary32 operand are admitted
finite alphas in `[0,1]`. This interval is convex. The rate arithmetic is
polynomial in alpha and has no alpha-dependent arithmetic branch. A
symmetric uncertainty interval extending beyond `[0,1]` therefore does
not imply two different arithmetic paths at these actual admitted
endpoints. The existing equivalence certificate has zero branch
divergence; no arithmetic radius is changed. The consumer checks every
alpha endpoint's domain and every obligation's named site and false
rejection result before discharge. Unknown obligations and invalid
arithmetic domains refuse; this is not a blanket branch waiver.

The terminal-rate parent proof now requires that discharge. Mutations
setting an alpha to 2 or injecting an unknown obligation both refuse.
Mixed and cold fixtures each discharge all 240 obligations at R0 and R1;
both new REDs pass. Mixed owner gate exits 0; cold remains 222 under its
unchanged insensitive controls. The Heun-R0 diagnostic is not yet a
qualified parent and remains unpromoted; its separate proof and actual
provisional arithmetic grouping are still pending.

Generator regeneration and `--check` pass. Focused build has no warnings;
no production arithmetic, kernel, tolerance or arithmetic enclosure changed.
The existing Metal/fp64 kernel sweep exits 0.

| Artifact (same external directory) | SHA256 |
|---|---|
| `compatible-admission-build.log` | `f3327e77311468163faf52f2efb4f872b255489dfc266806af62c2e7e6583ef7` |
| `compatible-admission-mixed.log` | `630ac2328f931ab4c4324223bd9a19828b4fe1a0fd514b2b62e33b4fee963fcb` |
| `compatible-admission-cold.log` | `05acc436ab3ae2e9a4b07c779e86e40d36626b478951c6e3e040fa04c434a9d4` |
| `compatible-admission-sweep.log` | `e074bd5ad763e8e383b63e0ea5d50ed9f93de7042fb091f6942a14eaf1768f3c` |
| Test source | `52193200881cae75da45eda52738324a641dfb484fa9d598ee7a357ed58ebe11` |
| Executable | `c9784654d47d50c598dc11756e836a40d32559b7f635d63df0b75818dbbbcd60` |
| Trace generator | `4c80a1fc33862f68b907b44105f49bac88d801c161c7e40603243b1ea44d9460` |
| Generated transport source | `b1960cf12dd58500666481442a5771ec58a08f68c3b555488a53bc1536669dd7` |
| Generated source manifest | `177d26e39ac8390dbfef5171b05156ba4307a70c75ca0095faf00c72a5713742` |

Two fresh reviews of `61ea63b1` (mathematical domain equivalence and
test/generator/evidence integrity) found no P1/P2. Both confirmed the
limited admission scope and unchanged arithmetic enclosures. Additional
one-endpoint-only domain, true-guard, invalid-domain and idempotence
mutations were noted as coverage refinements, not demonstrated defects.
The shared legacy non-inlet owner path also exits 0, with 240/240
discharged at both stages. Its external `compatible-admission-legacy-owner.log`
SHA256 is `7564a85a6c56f2067887f88176ef025c29268fcb3a4c7eb802ed4533632e1817`.
Numeric exit codes here are the launcher's observed process results;
the logs' hashes bind their contents, not an embedded exit-status record.

### R2 consumes the R0-under-Heun-alpha parent

The per-iteration provisional-momentum proof now retains two distinct R0
advection parents. R1's predictor uses the original R0 alpha. R2 uses
R0's saved qualified flux/velocity request with only alpha replaced by
the qualified terminal R1 alpha. The existing admission helper discharges
that invocation's domain obligations before its rate can be consumed.
Each local rate bound is the outward-rounded sum of the existing walker
radius and its center's difference from the reviewed fp64 rate; the
device residual is checked against this bound, never used to derive it.

The R2 binding checks terminal candidate IDs against their published
stage IDs, iteration markers, topology extents, exact R0 low/delta/physical
mass-flux and velocity bytes, and R1 alpha/recomputed-rate bytes. The
eight mass fields remain distinct from the nine conservative fields.
Missing proof, swapped parents, changed alpha, changed low flux and
changed recomputed-rate output all refuse. The same selector used by the
consumer is tested on the existing forced-limiter fixture: it retains
the R0 parent for R1 and rejects the old R0 parent for R2. This is the
nonzero witness recorded above, not a new synthetic tolerance.

The per-iteration arithmetic follows the device's grouping:
`M0 + dt * (0.5 * ((N0-A0) + (N1-A1)))`, rather than separately adding
two half-step impulses. Existing certified arithmetic carries parent
uncertainty. The reviewed fp64 endpoint retains its own grouping; the
reference-center difference is included separately, with no change to
either owner's arithmetic.

Mixed owner gate exits 0. All R2 provisional per-face comparisons pass;
the worst local residual/bound ratio is `9.1278406581425518e-7` (packed
face 226). Cold's corresponding ratio is `2.6178369180798494e-6` (face 0).
These bounds are loose, parent-propagated bounds, not a step-cost or
precision-improvement claim. Both fixtures pass the parent-binding REDs;
cold's forced-limiter selection remains insensitive, so its overall
exit remains 222. No cold-fixture control is waived.

This repairs the **per-iteration** consumer. The final publication's
output-relative provisional/advection bounds have not yet been replaced
with these parent proofs; eddy averaging and coherent launcher admission
also remain pending. No production run or solver/kernel change is claimed.
Focused build has no warnings. The first development run refused because
the new shape check incorrectly expected nine physical mass fields; it
was corrected to the existing eight-field schema, not relaxed.
The kernel sweep and legacy non-inlet owner both exit 0.

| Artifact (same external directory) | SHA256 |
|---|---|
| `heun-parent-repair-v2-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `heun-parent-repair-v2-mixed.log` | `15771516697abf3dfd4074f26e5250dc9e7424a5f3d0a32c503e91d2c91291a7` |
| `heun-parent-repair-v2-cold.log` | `531136e460406bbad1856287281c2725f7fecae44b03aee111e8dd62e2aac7eb` |
| `heun-parent-repair-v2-sweep.log` | `b2fc3a98dead966b57c425bd4e99222fcea96dd215a8eb56f9b519fe07b7b15d` |
| `heun-parent-repair-v2-legacy-owner.log` | `6361f1348845814e5dafeceb436b95760cf03120d9fbcb283a419b1b54a1bd15` |
| Test source | `cb78693e9667491d513a1b6decf9600dac1e91a2cf67b01bb2428c060a97f40d` |
| Executable | `bd83c26ce446615c7e6c7a91505ea097358dfe7368d018a77fd8ac9c120ab96c` |

Fresh math and safety/lineage/evidence reviews of `3473951a` returned
no P1/P2. They confirmed the two distinct parents, actual device grouping,
triangle-inequality reference-center enclosure, sticky binding refusal,
and all seven artifact hashes. The forced-alpha selection is RED-tested;
a separate cancellation-heavy grouping witness and independent mutations
of every identity/extent/operand guard remain coverage refinements. This
review does not close final-publication or launcher qualification.

### Final provisional and advective publication consume parent proofs

The two remaining momentum-publication heuristics are removed. Published
provisional momentum must be an exact copy of the qualified terminal R2
projection input, with its candidate identity, iteration and reference
bytes bound to the recorded endpoint. Its bound is the already-qualified
per-iteration bound; publication performs no arithmetic. Bootstrap traces
cannot supply this terminal proof.

Published advection uses `-0.5 * (A0_Heun + A1)`. Existing certified add
and multiply propagate the distinct qualified Heun-R0 and R1 parent
bounds, preserving their sign and grouping. The device output must
bit-match this two-operation stream. Both fields additionally require
the actual reviewed fp64 reference bytes to match the qualified parent
reference. Missing proof, changed output and changed reference mutations
refuse through the same publication predicate. These byte checks are
against each precision's own parents, not a claim that Metal equals the
binary32 projection of the fp64 result.

| Fixture / field | Worst face | Units | Local residual | Local bound | Residual / bound |
|---|---:|---|---:|---:|---:|
| Mixed provisional | 226 | kg m^-2 s^-1 | 3.630672312217763e-14 | 3.9775807315161636e-8 | 9.1278406581425518e-7 |
| Mixed advection | 226 | kg m^-2 s^-2 | 2.4520344348716305e-9 | 0.0026066870431178521 | 9.4067081867210187e-7 |
| Cold provisional | 0 | kg m^-2 s^-1 | 8.3278108606546226e-18 | 3.1811801579920293e-12 | 2.6178369180798494e-6 |
| Cold advection | 0 | kg m^-2 s^-2 | 5.4577141256386135e-13 | 2.0848149307396494e-7 | 2.6178410587755762e-6 |

These are local parent-derived bounds, intentionally disclosed as loose;
they are not output-scaled tuning constants. Every face passes and both
fixtures pass the publication REDs. Mixed owner gate exits 0. Cold remains
222 under its existing insensitive mixed-fixture controls. A first draft
incorrectly made provisional-proof availability depend on the unrelated
R2 scalar-front gate. Provisional momentum is fixed by R1 before that
projection; availability now follows its own parent/field/iteration
proof. The scalar-front and all other owner gates remain sticky and are
not waived. No solver, kernel, or upstream enclosure changed.

Focused build has no warnings. Eddy-average proof, cold/control separation,
and coherent launcher admission remain pending; no new production run or
complete cold qualification is claimed.
The kernel sweep and legacy non-inlet owner both exit 0.

| Artifact (same external directory) | SHA256 |
|---|---|
| `heun-publication-v2-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `heun-publication-v2-mixed.log` | `a5b43f22704eb723e538d0132862aec46659b22ffeb121347614e1836eb52043` |
| `heun-publication-v2-cold.log` | `ea0e865e34710e9f62bf4e38d1a4577b18c9a79780b65debab7ee55796c75739` |
| `heun-publication-v2-sweep.log` | `06766284389f74baf87d511fedd51a3160b82a278c12c55e32ef0147714c95a0` |
| `heun-publication-v2-legacy-owner.log` | `1b30d5b35918750643e90b2c51751913d35d08e29dea062fe136d37ffc932b48` |
| Test source | `f7c1c1f649b877b818ca4e897f18278d8eb714adaaa933b95dc4e2b2e517f06b` |
| Executable | `115aabda4e70cc5ea2f6c84f95156169a964d1a76040043bf9907615e923a992` |

Two fresh reviews of `ba2bd095` (math/function scope and safety/evidence)
returned no P1/P2. They confirmed both publication expressions, endpoint
binding, bootstrap exclusion, mutation refusals, unchanged sticky owner
gates, all seven SHA values and all four local table rows. Independent
candidate/iteration and truncated-axis mutations remain coverage refinements.
As above, numeric exits are observed launcher results, not records embedded
in the hashed logs. Eddy and complete cold-owner qualification remain open.

### Force-eddy producer audit (qualification only)

The final eddy average is produced by `FireProductionForceMac.mm`'s
`setup_face_velocity -> build_cell_velocity -> build_stress`, not by the
transport coefficient kernel. The former reconstructs velocity from the
projected momentum and density (`M/rho`); the latter consumes projected
velocity directly. Consequently, transporting the existing gradient/eddy
enclosure to the force publication without tracing that division would
skip an arithmetic operation. The stress-enclosure consumer shares this
audit obligation. No enclosure is widened.

The existing matched-input Metal force comparator now replays each actual
terminal R0/R1 trace, with its cell density, molecular viscosity, face
density, momentum and burner mask. It checks byte-identical stress rates
against that trace, then checks the published eddy average against
`0.5f * (replayed_R0 + replayed_R1)`. Missing replay or a one-ULP changed
publication must fail the same predicate. Terminal candidate identities
and non-bootstrap iteration tags are checked. This replay/streaming check
alone is **not** an independent fp64 proof. Phase source is zero only in this
eddy/stress-only replay: the audited kernel dependency graph shows it
enters the later phase-source kernel, not velocity, eddy or stress.

The diagnostic also counts and sizes the represented `M/rho` versus
projected-velocity differences in m/s. This locates the required input
arithmetic for the proof; it does not introduce any acceptance tolerance,
live-path transfer or new production authority surface.

Direct reuse of the existing per-cell Vreman walk was tried and rejected:
although the represented velocity quotient agrees exactly on these
fixtures, the walk's rounded eddy values do not always match the force
kernel. First mixed R0 difference, cell 0, in m^2/s: actual
`7.7721451816614717e-6`, walk `7.7721433626720682e-6`, fp64 force
`7.7721454152439444e-6`. The diagnostic walk bound is
`2.2877336387522846e-5`; its size is not permission to skip arithmetic-path
validation. The proposed exact-reuse gate refused (exit 222). It is not
promoted. The final code keeps the production-kernel replay/streaming
check and reports the walk differences diagnostically, with the original
fp64 eddy heuristic unchanged. It does not claim to have completed the
eddy enclosure or cold qualification. Next: localize the force/walk
arithmetic difference before replacing that heuristic. No solver source,
kernel, tolerance, or upstream bound changed.

Final mixed fixture reports its owner gate passing; cold still exits 222
under its previously recorded controls. Both pass the new missing/changed
publication RED and exact replay checks (R0/R1 stress and eddy average),
and both show zero changed quotient faces. The focused build is warning
free. No new window, full clean/Opto build, or production admission is
claimed.

| Artifact (same external directory) | SHA256 |
|---|---|
| `force-eddy-final-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `force-eddy-final-mixed.log` | `b4786e363c04428aac4fae181b2c505330edbfb64974c1925a1b6833536164a5` |
| `force-eddy-final-cold.log` | `ec121e609ff8260fc763d9a8d05fc14795b8e43b821d63cdf58e20fbc5e0f89b` |
| Legacy owner, observed exit 0: `force-eddy-final-legacy.log` | `753b80350038464ba5e2d4e7fd03c5368b8f2d7e659e18a4cd3b478137638b05` |
| Rejected candidate `force-eddy-trace-mixed.log` | `c8fdc81805dbbc2f29e1d4ce4a1e62d73927d7d8d6fd00e4745236ebc0bc4daf` |
| Final test source | `79aa40e4cd277abe15abb475443d64bd3d96071a4d3b2521a98be61f89d8ce39` |
| Final executable | `406cf9d3c481e54f32d86c2a0ed813d09473098b09cc211c82b2d762c3bf986a` |

Two fresh reviews of `bb7898aa` (math/dataflow and safety/evidence) report
no P1/P2 in this scoped audit. They confirm actual replay inputs, phase-source
independence, sticky gates, byte comparisons and all seven artifact hashes.
Parent identity/tag, truncated-parent, nonzero-source and changed-quotient
fixtures remain coverage refinements for the unfinished force proof.
Process exits are observed launcher results, not embedded in the log bytes.

### Three-path localization: rounded Vreman coefficient

The matched-input replay now also calls the existing strict-binary32 CPU
force implementation. This is diagnostic triangulation, not a replacement
for the reviewed fp64 owner. Before the repair, CPU-versus-walk mismatch
counts were R0/R1 = 1/1 (mixed), 2/2 (cold); CPU-versus-Metal counts were
24/25 (mixed), 28/33 (cold), each over 64 cells. These are two separate
differences. Mixed R0 cell 0 agrees between CPU and walk, so its Metal
discrepancy is not explained by the walker repair below.

The CPU/walk differences localize to the uncertainty fallback where
`alphaSquared.exact <= alphaSquared.error`. For example, mixed R0 cell 27
has `alphaSquared.exact=0.057394278220940016` and
`error=0.060708573684221341`. The fallback's represented expression
multiplied the **double** record coefficient by a float square root and
rounded the product afterwards. Both actual force implementations multiply
the already-represented **float** coefficient. The walker now does the
latter, matching its separated-denominator sibling's `CertifiedRecord`
behavior. Its reference value and analytical error formulas are unchanged.
The double precision reference expression intentionally remains double.

The new RED evaluates the old double-coefficient expression on recorded
terminal fallback operands. It must differ somewhere, while the repaired
walk must byte-match the existing CPU32 force implementation at all cells.
The diagnostic explicitly reports whether the comparisons are available;
empty/malformed inputs cannot be presented as zero observed differences.
The Metal-versus-CPU discrepancy remains unresolved; no force enclosure,
production admission, window or spectral result is claimed here.

After the walker repair, CPU/walk differences are **0/0** for both mixed
and cold; the old-expression mutation differs and the new RED passes at
both stages in both fixtures. Mixed exits 0; cold still exits 222 under
its existing controls. CPU/Metal differences remain exactly 24/25 and
28/33 respectively. The result separates the repaired host-walker
expression from the still-unlocalized backend arithmetic difference.
Build is focused and warning-free, not a full clean/Opto qualification.
The existing kernel sweep exits 0.

| Artifact (same external directory) | SHA256 |
|---|---|
| Pre-repair `force-eddy-three-path-v2-mixed.log` | `7e53a0cf9a5a0d93822c58826fbbc6add437b970d99d262a3c6e71eb8611b720` |
| Pre-repair `force-eddy-three-path-v2-cold.log` | `60b6fbcc64b2194cfa445a04d3d8bf09ec487bf760610fcdaf575e6efe37b55d` |
| `force-eddy-rounded-cv-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `force-eddy-rounded-cv-mixed.log` | `2c70d9fe9ac57ddc6cdce89304213b9d01abc1e6c65a0e71a999cefe6280fa02` |
| `force-eddy-rounded-cv-cold.log` | `eb9ba6dea06227b8b98d92ea7372246f580b67addcbeb125c91bb4230b0b14a8` |
| `force-eddy-rounded-cv-sweep.log` | `d1c3d965c672b58d33f3d2a44d1ec8c41dfe63ad924fa9fab46dc18dd34d04b8` |
| Test source | `92e8291aae7fb378cb44acee8c70247d1bb74678444538eaac1148b0cef10b2f` |
| Executable | `fa3cf0ccf08c117cee904ca1e088b81f97952e700fa9193088501852ec90a295` |

Two fresh reviews of `82db0630` (math/dataflow and safety/test/evidence)
return no P1/P2. They verify the coefficient representation, unchanged
reference/error formulas, all eight hashes, comparison availability,
mutation sensitivity and sticky refusal. Isolated zero/separated/fallback
Vreman fixtures and explicit unavailable-CPU-result coverage remain
nonblocking refinements. The Metal arithmetic discrepancy remains open.

### Contraction hypotheses: insufficient to identify the Metal path

Using the terminal walk's represented gradients, four diagnostic CPU
evaluations test split arithmetic (mode 0), fused norm/beta accumulation
(mode 1), sequential fused minor accumulation (mode 2), and both (mode 3).
The split path exactly reproduces the existing CPU32 force at every cell
of both fixtures. The other paths use explicit float `std::fma`; they are
specific hypotheses, not an enumeration of all legal compiler choices.

| Fixture / stage | Mode 0 Metal mismatches | Mode 1 | Mode 2 | Mode 3 |
|---|---:|---:|---:|---:|
| Mixed R0 | 24 | 21 | 24 | 19 |
| Mixed R1 | 25 | 18 | 25 | 21 |
| Cold R0 | 28 | 19 | 24 | 11 |
| Cold R1 | 33 | 27 | 31 | 19 |

Each count is over the 64 eddy values in that stage. All comparisons are
available and finite. Cold R0 cell 0 matches the fully fused candidate,
but 11 other cells do not; matching that one cell is not a diagnosis.
No tested hypothesis reproduces the whole Metal field. These results
therefore do **not** establish contraction as the cause, nor rule it out.
Next work must inspect actual device intermediates (gradient, norm, beta,
minor sum, quotient/root), rather than select a convenient final-value
match or enlarge an enclosure. No hypothesis supplies a bound or changes
an acceptance predicate. The final eddy proof remains unfinished.

Focused build is warning-free, mixed gate exits 0, cold remains 222. No
production source/kernel, reference owner, tolerance or authority changed.

| Artifact (same external directory) | SHA256 |
|---|---|
| `force-eddy-contraction-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `force-eddy-contraction-mixed.log` | `85b83eb63523d4a1f918ff7c9caf1b5b3f74ba723d7e6d84569a9b9c4d61eb0b` |
| `force-eddy-contraction-cold.log` | `51515956be8b09d5dd70e615b60b23c6c507cef5f61d8228782b5785df34b4c1` |
| Test source | `edbc523e8e53f6d9e10ff75937cbc2ef0db585a8ccfc55072d251b93906eac7d` |
| Executable | `9427c4c56eb2aa8400477f48601567153ee2348dd18b74cb6b5262e12916c2a1` |

Two fresh reviews of `586609eb` (math/scope and safety/evidence) report
no P1/P2. They verify the float hypotheses, diagnostic-only dataflow,
five artifact hashes and all sixteen Metal mismatch counts. Explicit
unavailable/nonfinite-candidate fixtures remain nonblocking coverage;
actual device-intermediate capture remains the next required measurement.

### Actual force-device velocity trace

The qualification comparator now optionally copies the existing packed
face-velocity and component-major cell-velocity buffers at terminal staging.
The resident owner and Metal kernel source are unchanged. The extra allocation
is reported separately (1,728 device bytes in each fixture); the ordinary
comparator certificate, extra device staging and host trace copies together
must remain below the existing 2-GiB cap.
This is diagnostic output, not a new acceptance authority or producer bound.

At both R0 and R1 in both mixed and cold fixtures, every device face velocity
bit-matches CPU `M/rho` with the same wall/inlet treatment. Every device cell
velocity bit-matches CPU averaging of the captured device face bytes. All six
force outputs are byte-identical with and without capture. A one-ULP changed
eddy output and a truncated face trace are rejected by the comparison/shape
predicates used to admit the trace.

This rules out velocity setup and gather as the first differing operation at
these matched inputs; it does not establish equivalence for other inputs.
The remaining difference is downstream in gradient/Vreman arithmetic. Actual
device gradient/norm/beta/minor/root values are the next measurement; the
final eddy enclosure remains unfinished, and no bound has been widened.
Mixed gate exits 0; cold remains 222 with its pre-existing insensitive
controls unwaived. The focused build emits no warnings. No production run
or cold-start admission is claimed.
The Metal/fp64 kernel sweep and legacy non-inlet owner both exit 0.

Artifacts below are in this worktree's untracked
`rendered/fire_production_calibration/r239_burner_projection/` directory.

| Artifact | SHA256 |
|---|---|
| `force-velocity-trace-build.log` | `53a76b7539239bfa3c6aad0ce4c81bc4c20ebefdf94e3bd5512dd684f8c1d8d1` |
| `force-velocity-trace-mixed.log` | `e6f2ac948a5a414117877efc03fc51850ae72d8fd0349fb283bc678e11f8ef72` |
| `force-velocity-trace-cold.log` | `f80c4c9c389ed5daa71feb946d440bf37b44feb42ba4b1fb2e31527f076f30ff` |
| `force-velocity-trace-sweep.log` | `0d058890d40fb5b6ef105cad6f838b5d7935efd8e28f9c7fd8cfd295e83841d9` |
| `force-velocity-trace-legacy.log` | `cde53b47f83b7b94c7f08215ff76a8ad70d521e34ba3f07f1ff6c429b9feef2e` |
| Test source | `1cb876b1f135b00cf3122c7aac1e9a67dca994fa128aafb2694360490ab39014` |
| Executable | `0e22542fff49bb147d58f01b1b142c62f2e3e905180654dd578b03e3a349e798` |

Initial review of `dbccb47f` found one P2: the trace cap used only actual
Metal allocation, omitting the ordinary host budget and new host copies.
The repair preflights the ordinary certificate plus trace host bytes and
16-KiB-rounded trace staging before packing/allocation; actual allocated
staging is also checked before commit. At 180 cubed, the ordinary certificate
is 2,107,913,984 bytes, host trace is 140,356,800 bytes and rounded staging
is 140,361,728 bytes: 2,388,632,512 exceeds 2,147,483,648 and is refused.
These are formula-derived allocation counts, not a large-grid timing run.
The independent indexing/evidence reviewer found no P1/P2.

After the cap repair, the warning-free focused build and both trace fixtures
were rerun: mixed passes and cold retains exit 222. The two complete fixture
logs are byte-identical to the first run, including all four zero-difference
velocity traces. Test source is unchanged. Repair artifacts:

| Artifact | SHA256 |
|---|---|
| `force-velocity-trace-v2-build.log` | `f0b09dfe3a8b62c50ddbe9fdfbe3bcbb449797751e6a923103ebbafe013e5c2c` |
| `force-velocity-trace-v2-mixed.log` | `e6f2ac948a5a414117877efc03fc51850ae72d8fd0349fb283bc678e11f8ef72` |
| `force-velocity-trace-v2-cold.log` | `f80c4c9c389ed5daa71feb946d440bf37b44feb42ba4b1fb2e31527f076f30ff` |
| Repaired executable | `6e62ffb865e4062913f68be1b4a33d049f61173efb8dbd67c9534ee44a2ec474` |

Fresh safety/production-isolation and evidence/test-integrity reviews of
`2b376cc3` both report no P1/P2. Two rounds total: the first found and the
second closed the host-copy accounting P2. Reviewers verified offsets,
publication failure handling, unchanged resident execution and artifact
hashes. Boundary-size execution and populated-trace failure/reset tests
remain coverage gaps, not claims of completed qualification. The next
required measurement remains actual gradient/Vreman device intermediates.

### Stress arithmetic localization (qualification only)

The same qualification comparator now constructs a diagnostic clone directly
from `ForceSource()`: rename `build_stress`, add an output argument and append
stores. The production source string, pipeline and resident owner are not
modified. The clone reads the original device velocity buffers and writes
separate private outputs; terminal readback records 23 component-major
planes. Its eddy plane must byte-match the original, uninstrumented kernel
before any trace is returned. Original force outputs remain independently
compared with the ordinary comparator. This is a matched-input diagnostic,
not proof of identical intermediate compiler choices at arbitrary inputs.

| Planes | Quantity | Units |
|---|---|---|
| 0–8 | gradient, derivative-major | s⁻¹ |
| 9 | squared gradient norm | s⁻² |
| 10–18 | beta matrix, row-major | m² s⁻² |
| 19 | raw minor sum | m⁴ s⁻⁴ |
| 20 | nonnegative minor sum / norm | m⁴ s⁻² |
| 21 | square root | m² s⁻¹ |
| 22 | eddy viscosity | m² s⁻¹ |

Each CPU diagnostic operation consumes the captured device parents, not a
propagated CPU trajectory. Thus differences in norm/beta use the exact
device gradients, the minor sum uses device beta, quotient uses device
norm/raw, root uses device quotient, and the final multiply uses device
root. A separate explicit-FMA norm evaluation tests the first divergent
sum using those same gradients. No diagnostic value supplies an acceptance
enclosure; the final eddy proof and cold qualification remain unfinished.

Trace preflight accounts for the ordinary certificate plus host copies of
`F + 26C` floats, rounded Shared staging of that size, and rounded Private
storage of `34C` floats (11C disposable stress outputs and 23C trace).
Actual additional Metal allocation is checked before commit and reported
as `extra_device_bytes`, now including Private storage, not merely staging.

All four terminal captures pass original-eddy byte identity; the six ordinary
force result families also remain byte-identical. Additional device storage
is 16,320 bytes per fixture. All nine gradient planes match CPU arithmetic
on the same captured velocity operands. The first differing aggregate is
the squared norm: explicit sequential float FMA reproduces the captured
norm in all 64 cells at both stages of both fixtures, whereas split
multiply/add does not. This localizes a contraction difference in the norm;
it does not claim that one FMA rewrite reproduces the whole Vreman path.

| Fixture / stage | Split norm mismatches | FMA norm mismatches | Raw minor mismatches | Quotient mismatches | Root mismatches | Final multiply mismatches |
|---|---:|---:|---:|---:|---:|---:|
| Mixed R0 | 20 | 0 | 25 | 0 | 17 | 0 |
| Mixed R1 | 15 | 0 | 19 | 0 | 22 | 0 |
| Cold R0 | 19 | 0 | 24 | 0 | 11 | 0 |
| Cold R1 | 21 | 0 | 21 | 0 | 18 | 0 |

These are diagnostic bit-difference counts out of 64 cells, each operation
using device parents; they are not pooled residual/enclosure acceptance.
Beta-plane individual counts are retained in the logs. At mixed R0 cell 0,
norm is `0.13367892801761627` on device versus split CPU
`0.13367894291877747` s⁻². Cold R0 cell 0 is
`7.8465724072884768e-5` versus `7.8465716796927154e-5` s⁻².
The norm FMA result matches device in both cases. Device root discrepancies
remain independently visible even when fed the device quotient. No
root-accuracy bound or full producer enclosure is inferred from these counts.
Next: account for the measured operation graph and device elementary-math
accuracy in the termwise producer proof; do not recenter on measured output
or enlarge an existing bound.

The focused build is warning-free. Mixed owner gate passes; cold retains
exit 222 with its controls unwaived. No production admission is enabled.
The full Metal/fp64 kernel sweep and legacy non-inlet owner both exit 0.
Artifacts are in the same untracked r239 directory:

| Artifact | SHA256 |
|---|---|
| `force-stress-trace-final2-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `force-stress-trace-final-mixed.log` | `0efb3bfcce1a2d88a0d344794c2b8cddd812d294fd6b1ba4a436b05344adcfd0` |
| `force-stress-trace-final-cold.log` | `3557ca79c3e8af875fe3d2d6d2c5626717f6b3f2d622658c13cb9a695a30af3f` |
| `force-stress-trace-sweep.log` | `0ad5dbd591b0d7b59ae5ee3ffdf8cc1e591fcb97d12d1defca6d60fa5151c652` |
| `force-stress-trace-legacy.log` | `6aa8fe36210ec9499ee792d262a5535ff57d0c970da91c30c063f92544b1083d` |
| Test source | `56784993e44cbb63d26da1593c90dd06cfdb3a845ed410ad7a3908e89b37beb0` |
| Metal wrapper source | `f821991eaf0e1d201ec744b0a867a208876af9d18c26557ac73c442557bbd15d` |
| Executable | `c2f0ec9a0d336ebb66c3f2d23dcbeb5c9e75b910fe69c68a68193dee21afc618` |

Initial reviews of `bfaced24`: math/evidence found no P1/P2; safety found
one P2 in the new clone pipeline's lifetime. The comparator submits an
unretained command buffer, but the block-local pipeline reference ended
before submission. The repair holds both the diagnostic pipeline and its
Private trace buffer with `objc_precise_lifetime` through completion;
ordinary resident execution is unchanged. This addresses ARC's permitted
early release rather than relying on driver caching or autorelease timing.

An attempted `MTL_DEBUG_LAYER=1` owner qualification aborted before reaching
the trace: Metal API validation rejects the pre-existing absent EOS cache
buffer at index 13 (`evaluate_resident_eos_candidate`). The ordinary kernel
explicitly handles null cache inputs, but validation does not admit that
binding. This optional validation-mode compatibility issue goes on the
sprint infrastructure backlog; it is not a successful trace validation
run, and no solver/cache semantics are changed to accommodate this probe.

The ordinary mixed and cold qualifications were rerun after the lifetime
repair: mixed passes, cold remains 222. Both complete logs byte-match the
pre-repair logs above. Focused build has no warnings. The preceding source
and executable hashes describe `bfaced24`; repaired artifacts are:

| Artifact | SHA256 |
|---|---|
| `force-stress-trace-lifetime-build.log` | `928f5fc69424bcb91f1a72116bd3225c364d1153903b3f4d2850680058ed140a` |
| `force-stress-trace-lifetime-v2-mixed.log` | `0efb3bfcce1a2d88a0d344794c2b8cddd812d294fd6b1ba4a436b05344adcfd0` |
| `force-stress-trace-lifetime-v2-cold.log` | `3557ca79c3e8af875fe3d2d6d2c5626717f6b3f2d622658c13cb9a695a30af3f` |
| API-validation abort, mixed: `force-stress-trace-lifetime-mixed.log` | `4539ffe0b15894a84f6ebd2cb819eda5bdbc3fb8b46dd1f04b3758007f1f6d0d` |
| API-validation abort, cold: `force-stress-trace-lifetime-cold.log` | `574364d705d1fdb5fff270bd2a0173afb0976b55e8962baeb9e89e9b2a0c1d80` |
| Repaired Metal wrapper source | `0a302314a67b3eee4cbca3e10dfcbd6b2aac687138d9c90c072be47eb61ca0ae` |
| Repaired executable | `fe8348111ee412eb3ed2712b2d02c0bd9c8aa2b121a986d7d1798b5174f816c5` |

Fresh safety/isolation and math/evidence reviews of `25a39846` both return
no P1/P2. Two review rounds: the first identified the lifetime P2, the
second confirmed its closure. Successful API-validation execution through
the trace and a complete producer enclosure remain unclaimed.

A subsequent read-only `MTLCompileOptions` probe establishes a separate
configuration fact: default `(mathMode, mathFloatingPointFunctions)` is
`(2,0)` on this host; setting only `.safe` produces `(0,0)`, while explicitly
selecting `.precise` functions produces `(0,1)`. The probe prints the enum
values too: safe=0, fast functions=0, precise functions=1. This agrees with
the installed SDK's `MTLLibrary.h` declaration that the function mode
defaults to Fast. [Apple documents the two separate option properties](https://developer.apple.com/documentation/metal/mtlcompileoptions/mathmode).
The production force context currently sets only math mode. This is a
measured configuration mismatch with an assumption of precise elementary
functions, **not yet proof** that it explains every root/eddy difference.
Next measure the precise-function alternative at the same inputs, then
qualify the appropriate operation graph; no production option has changed.
Probe log `force-math-options-probe.log` SHA256:
`847fe9b8b2f60fcd36518553de5c44f4f960d0705052818d58f2da5ffa986e95`.

### Qualification alternatives: precise functions and contraction off

The diagnostic comparator evaluates three isolated stress clones at the
same resident face/cell velocities and force inputs. Mode 0 retains the
production compile options and must still reproduce original eddy bytes.
Mode 1 explicitly selects precise elementary functions, leaving contraction
enabled. Mode 2 additionally prepends `#pragma clang fp contract(off)` to
the cloned source. Neither alternative changes `ForceSource()`, its source
identity, production pipelines, resident output or any acceptance bound.
Both are diagnostic alternatives, not published force fields.

The payload cap now includes `102C` Private floats (three disjoint 34C
groups), `F+72C` Shared floats and the same host trace size, with device
allocation rounded in preflight and actual sizes checked before commit.
All three pipeline references and the Private output allocation retain
precise ARC lifetimes through completion. The returned arrays preserve the
23-plane layout above. Logs distinguish `arithmetic_mode=0/1/2`; the earlier
two-mode probe logs used `precise_functions=0/1`.

The initial precise-only experiment removed every observed root mismatch
against CPU sqrt at the captured device quotient (17/22/11/18 become zero
for mixed R0/R1 and cold R0/R1). It did not remove contraction differences:
the fully fused CPU hypothesis still differs from its precise-device eddy
field in 6/6/3/2 cells. Thus selecting precise functions alone is not an
exact reproduction of the CPU arithmetic graph. These are measured fixture
results, not an accuracy theorem or a new tolerance.

| Initial precise-only artifact | SHA256 |
|---|---|
| `force-precise-functions-final-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `force-precise-functions-mixed.log` | `37620ddb5d78f9a49905bb6112aeb00a40b3db48b4a2eb1cf22cfb26b752c7ba` |
| `force-precise-functions-cold.log` | `f1bdbf8c18d2ab19a12b4351edf339d3da1651bc959a917d62ffb62fafd86bc1` |

Mode 2 closes this measured arithmetic discrepancy: every one of its 23
planes bit-matches the same-parent split CPU operation in all 64 cells,
at R0/R1 in both fixtures. Its complete eddy field also bit-matches the
existing split CPU hypothesis (mode 0), not just independently reset
subexpressions. The original producer remains different; no production
kernel has been changed by this experiment.

| Fixture / stage | Original eddy vs split CPU mismatches | Precise-only vs split CPU | Precise + contraction-off vs split CPU |
|---|---:|---:|---:|
| Mixed R0 | 24 | 20 | 0 |
| Mixed R1 | 25 | 18 | 0 |
| Cold R0 | 28 | 22 | 0 |
| Cold R1 | 33 | 26 | 0 |

Each count is per eddy field over 64 cells. Original six-output byte
identity and trace mutation/shape controls still pass. Actual additional
device allocation is 65,536 bytes per fixture. Mixed gate passes; cold
remains 222 (unqualified controls), with admission disabled in both.
Focused build is warning-free. No enclosure is widened or promoted here.
The Metal/fp64 kernel sweep and legacy non-inlet owner both exit 0.

**Next implementation decision, pre-registered:** pin the force producer
to the precise, uncontracted arithmetic graph already used by its strict
CPU mirror and certified walker, rather than fit an enclosure to opaque
compiler contractions. Re-run the actual owner and its per-field gates;
only then bind the final eddy average to the existing termwise parents.
This is an arithmetic-semantics change, not a class-A scheduling promotion:
kernel/source identity and case metadata must reflect it, and subsequent
production runs start from zero. Precise-functions-only was rejected as
insufficient by the nonzero middle column above. A hand-selected FMA
reconstruction was rejected by its remaining 6/6/3/2 mismatches. No physical
gate or tolerance is changed by the decision.

| Final three-mode artifact | SHA256 |
|---|---|
| `force-arithmetic-modes-build.log` | `f0b09dfe3a8b62c50ddbe9fdfbe3bcbb449797751e6a923103ebbafe013e5c2c` |
| `force-arithmetic-modes-mixed.log` | `b7c780abec209c6a8a114dda42cb0bce86bf6fd636da92c8d5980762cf32e656` |
| `force-arithmetic-modes-cold.log` | `6c6f0ed78402cf8f7f3b4cdb01826aecdb6a635edd663092ea797bed20c64557` |
| `force-arithmetic-modes-sweep.log` | `62061c7b55ba65a5d2e4e8befd58b99fce91f4ebfaad80ec6e0e86e0d5162651` |
| `force-arithmetic-modes-legacy.log` | `1979bdd8290b956ac827e6d848f0f380f0ef2ca41c73c73adbf6d0de2ee41a20` |
| Test source | `c51402f0c0c254d1cd988358fd1c48be2deceda70570e4b7ad7cdbe8bff7425a` |
| Metal wrapper source | `61d64849c37a747b544a0ef8c850309e917564eced2d534eb28aefca91bbf0bc` |
| Executable | `51b1a54f8cd16810ab8d44f968cc0adaddae39be4aec04c66232d60a152c2471` |

Fresh math/evidence and buffer/lifetime/isolation reviews of `76445244`
both return no P1/P2. Reviewers verified all 92 mode-2 plane records, the
independent full-field comparisons, allocation partitions, production
isolation, generated mirrors and documented hashes. Alternative equality
is presently diagnostic, not a failing assertion: its production promotion
must add the explicit gate and old-configuration mutation control. Failure
injection and boundary-sized cap tests remain nonblocking coverage gaps.

### Force producer arithmetic pin (implementation)

The force library now explicitly selects precise elementary functions and
disables contraction in its source. Unsupported compiler-option APIs refuse
initialization rather than silently changing arithmetic. The source SHA
therefore changes, and the existing qualified kernel-set identity consumes
that SHA. This is the pre-registered semantic change, not a bit-identical
promotion of the old producer. No checkpoint transition is authorized.
The corrected inlet launcher/case metadata remains pending; no new case or
window has been launched with this increment.

Qualification mode 0 now traces actual production arithmetic; mode 1 restores
contraction with precise functions; mode 2 restores the historical fast-function,
contracted configuration. The old mode-2 evidence above remains historical;
new logs explicitly refer to `legacy_bit_differences`. Production's 23
same-parent planes must each bit-match CPU, and the full eddy field must
bit-match both the existing CPU force and its certified walker. The historical
configuration must differ on the fixture, making removal of the pin RED.

Mixed and cold R0/R1 each have 23 zero-difference planes (92 total). Full-field
production/CPU/walker differences are zero. Legacy differences are respectively
24/25/28/33 cells out of 64. Mixed owner exits 0. Cold still exits 222 on its
previously disclosed qualification controls: this is not a cold-admission
claim. The final eddy-average enclosure is not promoted in this increment.

Focused build: zero warnings; generated fp64 and trace mirrors check clean.
Metal/fp64 kernel sweep and legacy non-inlet resident owner both exit 0.
This focused gate does not claim a clean whole-repository or Opto build.
Other Metal option sites were enumerated: they are separate arithmetic graphs,
not consumers of this force walker. Their options are not changed speculatively.
Both direct and resident force paths share this single ForceContext/source;
diagnostic mutants never replace its pipeline or write its output buffers.

| Pin artifact | SHA256 |
|---|---|
| `force-pin-build.log` | `f0b09dfe3a8b62c50ddbe9fdfbe3bcbb449797751e6a923103ebbafe013e5c2c` |
| `force-pin-mixed.log` | `51db56e9a41f0f37acab63e6a5d711cd81483e6b5b22a6448c0e003368f7fb12` |
| `force-pin-cold.log` | `f67a7d30508ef671a82dac1dbbb44667aad845dea75bbe279de8e000957eaa9c` |
| `force-pin-sweep.log` | `293512701efb30c1af1123a4f7a29be9458974281850d89efe7af50dd8ec6fdc` |
| `force-pin-legacy.log` | `26bd309afd6de7c96cfd22bae8a7b103bc895143b5ed3a6cdde08812b4467131` |
| Executable | `227bbc9ce17f92e941059589738fbdd6728d5a67d5c640d45e12d96ec4d56df1` |

Fresh math/test-integrity and runtime/provenance reviews of `fc534115`
both report no P1/P2. Each independently verified the artifact hashes and
checked the new failing assertions. Coverage is fixture-level, not an
exhaustive force-domain proof. Unsupported-option failure injection,
near-cap/reset tests, and independent quotient/gather trace assertions remain
coverage gaps; neither those nor Opto are claimed by this increment.

### Final eddy half-sum consumes the producer parents

Following the arithmetic pin, the final eddy field no longer uses the
output-relative `gamma512` heuristic. Its qualification bound is the existing
certified R0/R1 Vreman parent bounds propagated through binary32 addition and
multiplication by exactly one half. Both parents must match the actual device
producer byte-for-byte and carry the immediate candidate identity; publication
must match the resulting half-sum. The fp64 half-sum center must equal the
reviewed owner's endpoint average, not a fitted output residual. Existing
missing/changed-publication REDs and historical-arithmetic REDs remain binding.

The parents already include the triangle-inequality distance from their
independent arithmetic center to the fp64 force endpoint. This increment
does not alter those parent bounds or add any measured device/output residual
to them. It replaces an unjustified relative-output estimate with inherited
termwise bounds, which are conservative and substantially larger; the small
ratios below are not fidelity, subdominance, or tightness claims.

| Fixture, worst-ratio cell | Residual (m²/s) | Local bound (m²/s) | Residual / bound |
|---|---:|---:|---:|
| Mixed, 11 | 2.1472680286003772e-12 | 1.2102111769808829e-5 | 1.774292015677109e-7 |
| Cold, 5 | 2.0482895435133903e-14 | 1.0070593938735711e-7 | 2.0339312219062007e-7 |

Each row reports the same cell and field on both sides. All cells pass this
field, both endpoint proofs match, and both half-sums match. Mixed owner exits
0; cold remains 222 on its other controls, with production inlet admission
still disabled. This is qualification-only code; force kernel bytes remain
those of `fc534115`. Focused rebuild has no warnings.

| Half-sum artifact | SHA256 |
|---|---|
| `eddy-average-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `eddy-average-mixed.log` | `81236d767d2deca1bf3771cd2041eb052f62a456f82967d8c85e8a419661fadc` |
| `eddy-average-cold.log` | `3e84444f1317d3a364568d192500a1d8c306d32e3a3651c97a4383d14f58d4b4` |
| `eddy-average-legacy.log` | `836687921bf74819491407a5ec278f900a3b319766789c1706d93bcde38eda84` |
| Executable | `ffcc27537058ee2c44fa8ea763fdab6cf54c9fe488360108e2980e4e8c9f5774` |

Fresh math/principled-bound and test-integrity/runtime reviews of `cfa29df1`
both report no P1/P2. They checked reference-center triangle propagation,
sticky refusal flags, immediate parent binding, shapes, publication mutations,
and the recorded same-cell evidence. Legacy non-inlet owner exits 0. Focused
mutations of each endpoint proof's identity/validity/extent remain a
nonblocking coverage opportunity; no broader execution claim is made.

### Cold-start numerical gate and activated mutation controls

Pre-registered implementation: the cold-start probe will first execute the
complete mixed inlet owner gate in the same process. Any mixed failure returns
immediately; there is no caller-supplied bypass. The cold fixture then keeps
every baseline numerical/lineage check, but activation-dependent mutation
witnesses are supplied by that completed mixed gate. Specifically, the CPU
flux-surrogate rejection, nonzero outflow-head mutation, old-alpha selection,
and wrong averaged-parent flux witness are not required to be effective twice
when the cold state makes their operands equal. Their raw cold observations
remain printed, not relabeled as successful mutations.

This separates fixture coverage from numerical validity: neither a cold
numerical failure nor a failed mixed RED can pass the combined command.
The mixed conjunction must explicitly include Heun weighting and shared-alpha
controls as well as its existing REDs. No production kernel, envelope, physical
gate, or admission flag changes. Cold remains non-admitting until the paired
gate passes and the remaining inlet integration is qualified.

Implemented paired command: mixed reports per-iteration/per-field/REDs all
passing, then cold reports all three passing; overall exit 0. Raw non-activated
cold mutation observations remain visible as `passed=0`, but their witness
authority comes exclusively from the completed mixed gate. Production admission
remains 0 for both. The existing projection `command` failure injection makes
the mixed prerequisite exit 220 and the paired command immediately exits 220,
without entering the cold fixture. This proves a failed prerequisite cannot
be treated as a cold exception. No solver source or kernel changed.

Focused rebuild: no warnings. Evidence under the existing r239 artifact root:

| Paired-gate artifact | SHA256 |
|---|---|
| `cold-paired-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `cold-paired.log` | `bf44e291d97188fad91226eeef193cefa691421444664d72e9dd102b5a25cd59` |
| `cold-paired-prerequisite-red.log` | `4d0fe80e4a4c763ce164085279ec88e4c2d07c20f49fa3d084738e60a3dacb6f` |
| Executable | `a6e7d9c8a583aa5c01f197bb2f5568aea8afa28951e8f71548c8ddb7a1b42c7e` |

First review of `7f7260a5`: gate-flow reviewer found no P1/P2; contract/test
reviewer found P2 in the newly mandatory Heun weighting witness. Comparing
mutant Metal bytes only to the fp64 binary32 projection could mistake normal
precision differences for an effective mutation. Fixed by additionally
requiring changed momentum versus the correct Metal owner on identical inputs,
with shape checks and a no-op control using the already executed unmutated
owner. The fp64 comparison remains required. Both mixed and cold now report
`mutant_changes_metal=1 no_op_refused=1`, and the paired gate exits 0.

| Review-fix artifact | SHA256 |
|---|---|
| `cold-paired-review-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `cold-paired-reviewed.log` | `2009b5cb1910ecaf4428df90e284cef413957bef50a8d2bde86c390676d5e1bd` |
| `cold-paired-reviewed-prerequisite-red.log` | `4d0fe80e4a4c763ce164085279ec88e4c2d07c20f49fa3d084738e60a3dacb6f` |
| Executable | `e90f65abbf2e5ed0446c44ffaecf972d758b6424ac679e99574f7d16e6215509` |

Fresh test-integrity and runtime/contract reviews of `e52d42e0` and the
combined paired-gate change both report no P1/P2. The prerequisite RED was
re-run on that executable and again exited 220 before cold. The original P2
is closed. Dedicated late-mixed and cold-only fault injection, and malformed
mutant-extent tests, remain nonblocking coverage opportunities, not new sprint
infrastructure. Next is the coherent production launcher, not further force
arithmetic instrumentation. No corrected window has started.

### Launcher integration plan

The ported capstone will pass its existing canonical oracle boundary data
directly into the owner-request builder. The builder represents the prescribed
flux and injected tuple once in binary32, sets the bed to wall, and binds the
mask, flux, and injected state into the already qualified input lineage.
Non-ported historical calibration requests retain their previous defaults.
Chemistry begins at the actual accepted Q0; ported volumetric fuel staging is
removed rather than balanced by a subtraction. The external-fuel ledger uses
the represented inlet flux, represented dt, and represented cell area, and is
published only after acceptance. Refused attempts cannot charge that ledger.

The numerical inlet policy's explicit version preimage will be included in
the existing case referenced-content hashes, changing case identity without
changing physical inputs or introducing another certificate schema. Sequence
publication also requires the preimage: its loader intentionally requires
every case reference to be embedded. The coupled publication test exposed
this additional consumer. Add one optional numerical-policy bytes/ID pair
to the sequence payload, validate the pair's SHA, and include it in the same
exact referenced-record comparison. Old payloads remain valid; missing,
misbound, or unreferenced policy bytes refuse. This is provenance plumbing
required to publish the corrected run, not an additional correctness gate
or permission to change physical inputs. The live
inlet refusal remains until request construction and source/ledger integration
are qualified together. Corrected production runs start from zero.

First implementation increment: `BuildProductionProjectedHeunOwnerRequest`
can now consume an optional canonical `OpenBoundaryConfig3D`. Its adapter
transcribes the mask, represented flux, gas-only injected tuple, and boundary
kinds only after validating all supplied entries. It leaves the source and
conservative state untouched. Existing callers without the optional argument
are unchanged; the capstone call is deliberately not switched until its
volumetric staging and ledger are changed together. This is request assembly,
not a new seal or a claim that an arbitrary tuple has passed EOS qualification.

CPU fixture `--fire-production-burner-launcher-inputs` exits 0: exact mapping
on a non-square bottom and source/state preservation; unmasked flux, truncated
mask, open bed, carbon-bearing inlet, and negative flux all refuse without
changing previously bound inputs. Focused build has no warnings.

| Request-binding artifact | SHA256 |
|---|---|
| `launcher-input-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `launcher-input.log` | `b7141ac9e469133ee8f72303e45726337d381dde0efebe017b5558b2ba73e7bb` |
| `launcher-input-paired.log` | `2009b5cb1910ecaf4428df90e284cef413957bef50a8d2bde86c390676d5e1bd` |
| Executable | `b5bb6abfd2546af09c7a7f647176b1818f63b7f9e812f97ab0d3c60b6849e2db` |

Fresh mapping/provenance and test-integrity reviews of `5a162432` report no
P1/P2. The paired mixed/cold owner gate remains green, with production inlet
admission still disabled. Broader malformed-input coverage is backlog, not a
claim of this adapter fixture. The focused build/test receipt does not claim
a full clean or Opto qualification.

Sibling audit: the fp64 `BuildOpenFluxPair3D` already includes ambient and
injected supply when determining absent species. `ApplyOpenSharedFCT3D` does
not zero absent rows; it consumes the low/high fluxes. The currently unused
`boundaryCanSupply` metadata is therefore not itself a missing numerical
operation, and no new census machinery is added for it.

### Coupled launcher wiring (live admission still closed)

The ported capstone now supplies the canonical boundary to the owner builder
and skips the legacy volumetric fuel staging, including its unprojected
bottom-momentum overwrite. Chemistry consequently sees the actual accepted
Q0. After an accepted owner result, the inlet ledger sums
`double(j_fp32) * double(dt_fp32) * double(dx_fp32)^2` over bottom faces,
in kg; refused attempts do not publish this mass. The historical replay and
checkpoint-physics diagnostic keep their historical staging, not the new
production policy. No fuel multiplier or physical threshold changed.

The launch case probe and sequence writer both bind the same numerical-policy
preimage. A synthetic publication test confirms changed case identity with
unchanged grid/flow-through time and exact agreement between launcher and
sequence identities. Six independently re-hashed manifest mutations refuse:
changed policy bytes, missing ID, missing bytes, missing pair, a valid but
unreferenced replacement pair, and an unknown field. Historical publication
fixtures still pass. The inlet dose fixture checks represented-flux mass and
half-step scaling exactly. These are wiring/publication tests, not a completed
live capstone or an empirical claim. The resident-entry inlet stop remains;
live admission and its end-to-end source/accepted-ledger test are next.

Self-audit concentrated on duplicate fuel, rejected-step charging, historical
replay semantics, and probe/run/manifest identity drift. The manifest test
initially failed because the sequence reader requires exact equality between
case references and embedded records. Embedding the preimage fixes that
producer/consumer inconsistency without relaxing the equality gate.

| Coupled-wiring artifact | SHA256 |
|---|---|
| `launcher-coupled-build.log` | `9780681cf0ea212c345d7fea472a62370790c4d3177bc7742ac182dbd4ea890e` |
| `launcher-coupled-input.log` | `b7141ac9e469133ee8f72303e45726337d381dde0efebe017b5558b2ba73e7bb` |
| `launcher-coupled-publication.log` | `4fb802d2b755041667f857a0639a462f199bfbff90697f2caf5f4b52eabad4e7` |
| Executable | `d7c813bcccb5c0ba2e66a7158e473eb8715fb318c8d62e21b10180660866d903` |

Focused rebuilt translation units are warning-free. No full clean/Opto build
or corrected production window is claimed by this receipt.

First review of `3cd957ac`: provenance/test-integrity review found no P1/P2.
The numerical/sibling review found an existing retry-state defect: host
temperature rejection occurred after overwriting `advanced.velocityMPerS`,
while retry scalars and momentum still came from accepted Q0. The fix checks
the production candidate's temperatures before replacing accepted velocity,
boundary classes, the retained observation or histories. The later common temperature check stays
as defense in depth. The strict physical ceiling is unchanged. A focused RED
checks the immediate predecessor of 2300 K passes and exactly 2300 K refuses;
the live launcher rejection/retry test remains part of the pending admission
gate, not a claim made by this predicate fixture.

| Review-fix artifact | SHA256 |
|---|---|
| `launcher-review-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `launcher-review-input.log` | `3a9dc882e11f61b7350491f86b40bcbc89d633b6fe5dabcb6385aba3f1e98b47` |
| `launcher-review-publication.log` | `4fb802d2b755041667f857a0639a462f199bfbff90697f2caf5f4b52eabad4e7` |
| `launcher-review-paired.log` | `2009b5cb1910ecaf4428df90e284cef413957bef50a8d2bde86c390676d5e1bd` |
| Executable | `a65a6bbd01002f9bececb11672e6c6eee2456229b8a49a9023add0ba43f29f74` |

Fresh numerical/accepted-state and contract/provenance reviews of `020bcfcf`
report no P1/P2. Both confirmed the early check precedes retained state and
history replacement. The paired mixed/cold Metal gate again exits 0. Two
review rounds: one retry-state finding fixed, final round clean. Next gate:
live resident inlet admission, an end-to-end unstaged-Q0/source and inlet mass
balance check, and rejected-attempt/accepted-only ledger coverage before the
from-zero corrected tier-8 window. No solver run is active at this receipt.

### Live entry admission plan

Remove the temporary blanket inlet stop, not a numerical gate. The resident
entry will use its existing qualified owner and ordinary acceptance checks
for the inlet inputs already supported by that owner. The mixed/cold fixture
must compare the live result with the reviewed raw owner on identical inputs,
including the accepted payload and state identity; malformed/unsealed inputs
and qualification-only controls still refuse without an accepted token.
Before the window, exercise the real launcher from zero and its accepted-only
fuel accounting. No source multiplier, physical gate, tolerance, or historical
checkpoint is changed. Any corrected window starts from zero.

### Live admission and launcher gate (2026-09-17)

The blanket inlet stop is removed. On both mixed and cold fixtures the real
resident entry is byte-identical to the reviewed raw owner, including accepted
state, stage identities, iteration counts and publication roots. Qualification
controls still refuse at the live entry; malformed/unsealed inlet requests
cannot acquire an accepted token. The per-cell/per-field arithmetic gates and
all physical limits remain unchanged.

Self-audit focused on three escapes: comparing only a precursor fixture rather
than the final cold owner; allowing diagnostic controls at the live entry;
and charging or retaining rejected candidate state. The final mixed/cold
comparison and qualification RED cover the first two. The actual launcher
fixture covers the third: three from-zero one-step diagnostic runs (normal,
forced physical-temperature rejection followed by retry, fresh half-step).
Every attempt's source-construction input hashes to its immutable Q0. The
rejected velocity differs from Q0 (a positive witness for the old retry bug),
yet the accepted retry is byte-identical to the fresh half-step. Accepted
histories contain one entry each; attempts are 1/2/1. These are diagnostic
fixtures, not window evidence.

Full-step dt is 0.0016462659696117043 s and fuel charge is
1.0859411917929656e-6 kg. Retry and control dt are
0.00082313298480585217 s; both charge 5.4297059589648282e-7 kg exactly.
The existing independent small-grid dose fixture also passes. Peak sampled
RSS across these three runs is 1,942,847,488 bytes, not a long-run memory claim.

Accepted-R1 fuel-budget observations (kg): cold boundary/divergence influx
6.9849191647070395e-12, state increase 6.9849194820424948e-12, source zero,
closure residual 3.1733545529941351e-19. Mixed boundary transport is
-7.6939066665995589e-11, state change -1.6833656288697828e-10, frozen source
-9.1327824055902113e-11, residual -6.9672165080574584e-14. These reductions
are diagnostics, not a newly invented aggregate tolerance; the existing local
owner enclosures remain the arithmetic acceptance criterion.

Focused build and the paired owner, launcher, input and publication gates all
exit 0; no compiler warning appears. This is not a fresh whole-repo clean or
Opto qualification claim. Fresh independent review is next; the window has
not started at this receipt. Logs stay outside git in
`rendered/fire_production_calibration/r239_burner_projection/`.

| Artifact | SHA256 |
|---|---|
| Executable | `16c014cb614325d08c9916d39e6b400e0e19942a99119184ac35f3517913791a` |
| `live-admission-build.log` | `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21` |
| `live-admission-paired.log` | `80ca46c2620ea80d2be07db4b2d91a9c2535e51fec8190096f7d5210fc5d20f8` |
| `live-admission-launcher.log` | `5a6b435da7b4f772786d3fa1f0e92313a0a726e17a7b4af9c2b1bb8ccfcb9f9b` |
| `live-admission-inputs.log` | `3a9dc882e11f61b7350491f86b40bcbc89d633b6fe5dabcb6385aba3f1e98b47` |
| `live-admission-publication.log` | `4fb802d2b755041667f857a0639a462f199bfbff90697f2caf5f4b52eabad4e7` |

## Rejected shortcuts

- Turning the entire bottom into an impermeable wall without prescribed
  inlet exceptions removes the source. Leaving the surrounding bed open
  instead fails the oracle's explicit bottom boundary contract.
- Clamping an outward velocity after projection violates the solved
  divergence unless the operator/RHS already included the fixed face.
- Imposed scalar injection plus existing volumetric staging double-counts
  fuel; zero-flux sealing plus staged injection is a different split whose
  momentum/volume consistency has not been established.
- Raising nominal fuel to recover 33 kW hides the transport loss and changes
  the capstone input. Normalizing the McCaffrey comparison to measured HRR
  would hide a failed empirical row.

## Audited source identities at cc989d82

| Source | SHA256 |
|---|---|
| tools/fire_simulator_core.h | 21a5cf2c6177cc535d9ff8ced6835a7b0b09b2fe19dfed887c76d7e89c61eae1 |
| tests/FireSequenceTest.cpp | b223222ea17019a82f658942668f9c30d46d510f397ca9948d5d1c50b18cba26 |
| tests/FireProductionDyadicCalibrationFixture.h | ceecf4e7021779837796b6a54b6a2f1227edd08021579b68127fae2cf0ee7bff |
| src/Library/Utilities/FireProductionProjection.h | 263b98cb94622fea89229712cfb061280cebae9bc0a6543adfd729734c290e20 |
| src/Library/Utilities/FireProductionProjectionMac.mm | 741a680bbcc35037a86b1f566f023ba4fd75e6b7da7078c3fc8e97387ffbf158 |
| src/Library/Utilities/FireProductionAdvectionMac.mm | 2fee776af9ce97152fd7524c0609afe89ef89c4da17315e898f17addf1252f67 |
| src/Library/Utilities/FireProductionTransport.h | a8e41ff1a5145634943a3e8c6b485525cdecb15c2a8b24456bcc661889b00016 |
