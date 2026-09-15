# r238 — accepted fuel budget from the existing observer

Decision: use the already implemented owner iteration observer, not a new
resident authority, kernel, checkpoint format or migration mechanism.
The r237 retained-event donor estimate cannot close the fuel budget because
it uses frozen input velocity rather than the accepted R1 scalar operator.

## Measured object and implementation route

`FireProductionProjectedHeunIterationTrace` already captures the exact
`scalarLowFlux`, `scalarFluxDelta`, shared face alpha, transport state and
accepted conservative state. `CaptureIterationTrace` obtains its scalar
pair from `averagedPhysical` where present, so R1 carries the Heun pair
actually consumed by the scalar candidate. Owner publication takes scalars
from R1 and momentum/velocity from R2. Substituting R2 velocity into a
donor formula would therefore measure a different operator.

The next increment is a host-only reporting reduction of the existing
observer output. Select the terminal accepted R1 trace only after matching
its candidate identity and conservative bytes to owner publication. For
CH4, report each boundary's signed low-order and limited-antidiffusive
contributions separately, using the captured shared alpha and represented
geometry/time. Report net transport over that observed step and compare
with summed scalar change minus the exact frozen source increment.
Separate original-double source-staging volume from binary32 owner volume;
never hide their conversion in a purported exact ledger comparison.

The observer's existing replay must retain its accepted-output bit-match;
reporting neither feeds the owner nor publishes a continuation. Use a
retained-state isolated diagnostic, labelled as such. It is not a resumed
empirical run, a kernel-transition certificate, or reconstruction of the
missing full-window boundary integral. Preserve all original checkpoints.
Do not launch another full window solely to collect this diagnostic.

## Checks before claiming numbers

- Asymmetric-grid fixture with isolated positive and negative flow on each
  side, so axis swaps, inward/outward signs and face strides cannot hide.
- Distinct low/antidiffusive contributions and alpha 0/1/intermediate,
  preventing a donor-only or high-only calculation from passing.
- Discrete boundary sum versus the full-grid divergence sum on exact
  dyadic fixtures; units are kg/s and kg per observed step respectively.
- Reject a nonterminal, wrong-stage or mismatched accepted-state trace.
- Finite-precision closure residual is reported, not assumed zero and not
  excused by widening an existing bound. No new physical tolerance.

The full-window 4.106-g residual remains unaccounted until measured. A
single-step budget can localize the mechanism but cannot claim its integral
over the original statistics window. This is the smallest next diagnostic
because all required resident payloads already exist in the observer;
production hot-path sealing and arithmetic remain untouched.

## Implementation and current gate status

`63649b4e` adds the host reducer and asymmetric dyadic fixtures;
`bee45936` connects it to `WriteProductionCrossingConvergence`, after the
existing accepted-owner bit-match. No resident code or kernel changed.
The reducer's success means a diagnostic was calculated, not that its
closure residual passed a conservation tolerance. The caller supplies the
original live request's Q0, frozen source, represented dx and dt.

Standalone clang++ C++17 with `-Wall -Wextra -Werror` and ASan/UBSan passes.
Focused FireSequenceTest build has no warnings; the native existing owner
convergence fixture exits 0. Its **synthetic** accepted-step budget reports
boundary and divergence transport both -8.2037236603571814e-11 kg, and
closure residual -1.3743236450965095e-13 kg. This is fixture evidence only,
not a measurement of the r237 window or its retained physical state.
Initial reducer math and safety reviews found no P1/P2. Test-coverage nits
were strengthened; two fresh reviews of integrated `bee45936` found no
P1/P2. One independently repeated the sanitizer test. Remaining coverage
opportunities (combined source/transport fixture and poisoning every output
member) are disclosed, not claims of exhaustive testing.

Raw artifacts outside git in `rendered/fire_production_calibration/r238_accepted_fuel_budget/`:
build log SHA256 `7ed10c80b28ca104479cb9f2c6cd6b5f4c07a852d2dcfcee4dd7cd241ee29f21`;
native fixture log `4e18c1c7356a1df618aa2e44207f88d26f6cab8dc3b3fd77ece0355a3a9829c2`;
synthetic convergence/budget artifact
`7e7728cf541093de1a222c026d76c7db1638d2be41543fd35e8d22feb7f62d06`.
Next: use an isolated retained-state owner
observation. Do not use the historical compatible-momentum replay wrapper:
it selects the old diagnostic configuration rather than the ported owner.

## Retained-state physical observation

Launcher `ba8f8173` admits only r237 step 6489, its SHA/build/case, into
an isolated eight-step diagnostic. It selects the ported owner explicitly,
not the historical compatible-momentum replay. There is no normal checkpoint
or empirical continuation output and no migration certificate. The existing
RSS emergency path may write a diagnostic-only stop checkpoint in the new
output directory and refuse; the original checkpoint remains untouched.
Both fresh launcher reviewers found no confirmed P1/P2 for the pinned input.
They noted a general inherited-success-flag caveat on other checkpoints;
future launcher tightening should also require empty error, exact step count
and the bound budget artifact, rather than trust a boolean alone.

The diagnostic exited 0. Independent output inspection confirms exactly
steps 6490–6497, from 10.465922561998013 to 10.479092689754907 s, with each
recorded time equal to the running sum of accepted timesteps. No checkpoint
output exists. Input checkpoint and sidecar hashes are unchanged. The first
step's observational owner reproduces accepted fields and stage identities
bit-for-bit before reduction; observer wall time is 11.0534 s, separate
from ordinary step timings. End-of-step RSS samples are 1.35–1.81 GB; these
are not a continuous observer-peak RSS measurement.

First observed step dt=0.0016462659696117043 s, **accepted R1** CH4 budget:

| Quantity | Measured |
|---|---:|
| Top outward flux | 2.209966489560536e-4 kg/s |
| Bottom outward flux | 8.684407688336857e-5 kg/s |
| Sum of all six outward fluxes | 3.079539934234137e-4 kg/s |
| Inward-signed boundary transport | -5.0697417957899245e-7 kg |
| Full-grid divergence transport | -5.0697417957899394e-7 kg |
| Frozen reaction source increment | -5.3821442482340222e-7 kg |
| Scalar-state change after staging | -1.0451883392156661e-6 kg |
| State minus source minus transport residual | 2.6518672863335422e-13 kg |

All six limited-antidiffusive boundary contributions are zero for this
observed step. The low-order pair already contains its nonadvective term;
do not label it purely advection. This is direct evidence of methane leaving
the accepted scalar domain, mainly at the top but materially at the bottom,
not merely a donor estimate. It localizes one step; it does **not** reconstruct
the historical 4.106-g full-window residual or prove a boundary-condition
defect. The residual is reported, not granted a new conservation tolerance.

Output `/Volumes/SSD/RISE/fire_production_calibration/r238_retained_fuel_budget.v1`:
receipt SHA256 `19f1aa610d79d86c77d0752d3fbaee0b6aad5649b240ae2d37c382676cdafad9`;
accepted convergence/budget artifact
`6c7a9f726fe1eccb829ee33cd221d0294b5e6513cf01db5997384c5170dca3a5`.
Executable SHA256 `d5c230f603fd672584cb8a6acf039a94c9ed3751b8f75665b59d3a7ca5e9f91c`;
preserved physical log SHA256
`79e702092adc91ddd8e7a99b03039423f94a93b9243e7ec8b664ab4a4b269e58`.

Operational correction: an initial missing-input test started before its
link session was confirmed complete and ran the old executable's broad
suite. It was stopped, with no remaining child process; it supplies no gate
evidence. After link completion, the isolated missing-input retry exited 91
without creating output. No build overlapped the physical diagnostic.

## Bottom-source footprint localization

Observer-only increment `cd7385ff` partitions bottom faces by the actual
canonical source pattern (`pattern > 0`), not by an inferred inlet class.
The isolated v2 diagnostic completed all eight steps, with empty error and
all completion checks true. All eight exact step times, qualified kernel-set
identities, input payload roots and publication payload roots match v1.
The original checkpoint SHA remains
`68421f97b1035a6788b134c5c1e1ce65fddc9330aeb20ed929298c27c77345ae`.

For the first accepted step only:

| Bottom region | Outward low-pair CH4 flux (kg/s) | Limited correction (kg/s) |
|---|---:|---:|
| 120 positive-source footprint faces | 8.2734249337153135e-5 | 0 |
| Remaining bottom faces | 4.1098275462152664e-6 | 0 |

Thus 95.2676% of this step's bottom methane export crosses the source
footprint. The binary mask SHA is
`c02730f284cb8a3e38d7a462f0823c2c314f93819796657f75c489879b3bb63b`.
This is not a full-window export integral and does not alone prove the
cause of the empirical deficit. Observational terminal bit-identity passes;
observer overhead is 9138.550541 ms. End-of-step RSS samples span
2.0690–2.1635 GB (not continuous peak measurements).

Two independent reviews of this increment found no P1/P2 findings on
indexing/signs, physical interpretation, no-feedback behavior and completion
semantics. The standalone sanitizer test and focused native build pass.
Optional coverage backlog: nonzero correction partition with unequal alpha
and an asymmetric interior mask index. No full clean/Opto gate is claimed
for this host-only diagnostic increment.

Output `/Volumes/SSD/RISE/fire_production_calibration/r238_retained_fuel_budget.v2`:
receipt SHA `6e09b9f1f9aa59d3bdd81297c773a789b3940fb6afd440bc6db4d15845e6961a`;
budget artifact SHA
`d9014ea355e3c17037a5c2448479957530e81a75a481d97471c082c1cdb69ed1`;
executable SHA `33802e209af5c4738e7cc7e7ad6934b231b2b6f663053295fc0db81196f48b2b`.
Physical log preserved beside the earlier r238 logs, SHA
`995cae2f6ecdd647bdc62b49c479d40916806447e8f7478aa7a8a050de4de7b1`.

### Boundary audit opened by the measurement

The canonical oracle config installs `bottomFuelMask` and a prescribed
per-face methane flux from the source pattern. Its `FuelInletBoundary3D`
scalar flux is prescribed inward, not an ordinary pressure-open donor flux
(`tools/fire_simulator_core.h`, `BuildOpenBoundaryFlux3D` implementation).
The live production launcher instead stages fuel into bottom cells, sets
their initial bottom-face momentum/velocity, and constructs the ported owner
with every fuel-inlet classification byte zero. These are different boundary
constructions. The resident transport admission currently admits fuel masks
only on side 4 with a wall-class side, so simply copying the mask would not
be a complete boundary fix. Nor may prescribed inlet flux be added while
retaining the same volumetric fuel staging: that risks counting injection
twice. Next work is conformance of the source/boundary split, including its
projection constraint, against the existing oracle boundary contract. No
solver change or new empirical run is claimed by this diagnostic.
