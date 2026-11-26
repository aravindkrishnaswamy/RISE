# HyLIoS forward measurement and diagnostic variants

`tools/hylios_measure.cpp` measures the current forward-only individual-spheroid
and explicit-complex kernel with explicitly selected post-scattering and outer-boundary semantics. Diagnostic or budget-limited histories retain unresolved power in the result. It does not validate the publication's physiological reflectance.

## Build and run

From the repository root on macOS/Linux:

```sh
make -C build/make/rise build-tool/hylios_measure
./bin/tools/hylios_measure \
  --specimen resolved-s4.json \
  --wavelength-nm 550 --placement null --post-scatter discard --boundary smooth \
  --histories 1000 --max-transitions 10000 --seed 55000001 \
  --output measurement.json
python3 -m unittest discover -s tools -p test_hylios_measure.py -v
```

The existing generic tool rule discovers this source; it is not added to `all`.
Fresh checkouts need the platform setup described in [AGENTS.md](../AGENTS.md).
The tool links the shared forward transport, slab/optics, particle geometry and
certified overlap and strict complex-layout implementations; it does not run the renderer.

The input is the actual JSON emitted by `tools/hylios_compose_specimen.py`:

```sh
python3 tools/hylios_compose_specimen.py \
  --approximation-bundle /path/to/explicit-approximation-bundle.json \
  --preset S4 --wavelength-nm 550 --wavelength-nm 1300 \
  --output resolved-s4.json
```

Use `--strict-curves-dir` instead of an approximation bundle when independently
provisioned raw coverage supports every active input. The CLI never fetches,
extrapolates or substitutes spectra. It preserves the resolved artifact's
`approximate`, coverage profile, bundle identity, composition revision, preset,
interpretation, provenance and claimed specimen digest labels. These are input
claims, not a new certification. It hashes the **exact opened file bytes** using
`RISECBOR64::SHA256Hex` and records `input_bytes_sha256`. It explicitly reports that
it does not verify the Python producer's canonical semantic JSON hash. Formatting
changes legitimately change byte identity while preserving the claimed hash.

Required options: `--specimen FILE`, `--wavelength-nm N`, `--placement fixed|null`,
`--post-scatter diagnostic|discard`, `--boundary smooth|hylios2015`, `--histories N`, `--max-transitions N`, `--seed U64`,
`--output FILE`. Every option occurs at most once; unknown options fail.
The wavelength must exactly equal an explicitly resolved grid value; no spectral
interpolation occurs. Six layers must use SC/SG/SS/SB/PD/RD order and SI unit tags.
Top-level and nested bundle approximation/profile/identity metadata must agree;
the known strict and approximation profiles cannot contradict their flags.
All coefficient/index arrays must have the complete grid shape, even at
unselected wavelengths. Duplicate JSON keys and nonfinite values are rejected.

Input JSON numbers use binary64 semantics, with supported source magnitude at most
`9007199254740992` (`2^53`). The CLI compares each original decimal numeric token
against this bound **before** parsing it, including decimal/exponent forms, so an
integer such as `9007199254740993` cannot silently become its rounded neighbor.
Nonzero tokens that underflow to zero are also rejected. Larger sequence IDs and
other exact large quantities must be encoded as JSON strings. Fractional numbers
retain ordinary binary64 rounding; the report does not claim to preserve arbitrary
precision decimal values or numeric token spelling. These checks apply to the
entire resolved artifact, including copied provenance and unselected fields.

Metadata strings and object keys must contain Unicode scalar values encoded as
valid UTF-8, or equivalent JSON escapes with correctly paired UTF-16 surrogates.
Unpaired surrogates, overlong encodings and malformed UTF-8 are rejected before
publication. Valid non-ASCII text and escaped supplementary characters round-trip.
The complete serialized report is validated again before atomic replacement; any
unsupported metadata leaves existing output intact. The original source-byte hash
continues to identify the exact accepted input bytes.
Active organelles need evaluated internal absorption. Individual organization uses
spheroids; active `complexes` additionally require the explicit
`--complex-layout explicit-pack-v1` choice. No complex interpretation is selected
by default. Each complex requires `complex_diameter_m` and an integer
`maximum_internal_count` from 0 to 64. The diameter is divided by two to obtain R;
that R, child axes and count must satisfy the layout constructor's finite-range,
bounding-sphere containment and necessary volume checks. N=0 is an empty-envelope
analytic control. Under this clean-room convention the resolved integer counts
**actual spatial children**, not a maximum number of optical interactions.
Revisits can exceed N without absorption or termination at a count cap. Disabled particles are skipped,
including their unused geometry/absorption fields, but cannot claim positive
fraction. When a producer supplies a geometric rate it must match the rate
recomputed from its axes and fraction to the relative bound
`64*epsilon*max(declared,computed)`. For individual particles the rate uses the
spheroid S/V; complex envelopes use `3*f/(4*R)`, independently of child count or
child axes. The nominal outer fraction is not a measured packed child fraction.

Optional inputs have named, reported values rather than hidden kernel defaults:

| Option | Reported default / convention |
|---|---|
| `--complex-layout explicit-pack-v1` | no default; required for active complexes |
| `--estimator analog\|weighted` | analog |
| `--roulette-target N` | 0, disabled; nonzero requires weighted |
| `--exterior-ior N` | 1, measurement exterior convention |
| `--fiber-radius-m N` | 1e-7, paper Eq7 parameter |
| `--fiber-volume-fraction N` | .22, paper Eq7 parameter |
| `--fiber-relative-ior N` | 1.5/1.33, paper Eq7 parameter |

The rate is computed from those reported fiber inputs at the selected wavelength.
These are the publication's separately named fiber parameter convention, not
covert physiological inputs selected by the kernel. Every explicit CLI option is
also retained verbatim. Fiber fraction 0 disables that clock. No incident-angle or
spatial-light source is implemented: all histories launch at local origin with
normal inward direction (0,0,1), recorded in the result.

For an explicitly resolved complex specimen, select its construction:

```sh
./bin/tools/hylios_measure --specimen resolved-s3.json --wavelength-nm 550 \
  --placement null --post-scatter discard --complex-layout explicit-pack-v1 \
  --boundary smooth --histories 100 --max-transitions 100000 --seed 55000001 \
  --output measurement-s3.json
```

The history/transition limits are caller work budgets. A 256-proposal child restart
is construction work, not absorption; packing is not guaranteed for arbitrary
inputs. Inspect unresolved power and layout counters before interpreting a run.

## Scientific scope and unresolved histories

This measures the selected forward individual-spheroid and explicit spatial
complex models. It does not establish S1–S3 author-model or physiological
reproduction, reverse camera sampling, a marginal PDF, adjoint transport or
reciprocity. The complex law is detailed in
[HYLIOS_COMPLEX_LAYOUT.md](HYLIOS_COMPLEX_LAYOUT.md).
`maximum_internal_count` as actual children is a recorded clean-room choice.
Layout construction is independent of the ray, uses five draws per proposal, and
preserves its full proposal/restart/pair-check counters. Host gaps and true child
chords own separate absorption; there is no envelope Fresnel or per-child final
exponential event. The final angular event occurs once at nonempty envelope exit;
empty complexes introduce no angular kick even at visible wavelengths. PD
complexes with a nonzero fiber rate are explicitly refused; current composed
S1/S2/S3 complexes occupy SG/SS/SB.
Boundary selection is mandatory. `--boundary smooth` is an explicit **override** of authored fold metadata.
The original fold aspect ratio is retained and
`authored_surface_folds_simulated=false` is explicit for smooth. Choosing smooth
never evaluates that field. `--boundary hylios2015` instead evaluates authored
`surface_fold_aspect_ratio` with the selected
`hylios2015_macro_fresnel_normal_perturb_v1` law: macro Fresnel first, a normalized
Eq. (8) normal perturbation only for reflection, and macro Snell transmission.
Normalization and strict incident-side rejection are explicit clean-room choices.
This forward boundary is documented in
[HYLIOS_SURFACE_BOUNDARY.md](HYLIOS_SURFACE_BOUNDARY.md); it is not an adjoint or
reciprocity claim.

Placement is mandatory: `fixed` selects `FixedEventDiagnosticV1`, with a frozen
entry event and resampled geometry; `null` selects `NullCollisionPlacementV1`,
where a definitely invalid single geometry consumes a null event. Both follow the
versioned kernel conventions. Their accepted process is different, and nominal
volume fraction is not asserted equal to realized accepted fraction. Counts retain
candidates, attempts, accepted particles, nulls and rejection causes.

Post-scattering policy is mandatory. `diagnostic` selects
`hylios_diagnostic_retain_particle_v1`: an inward final direction produces
`post_exit_reentry` with live unresolved power. `discard` selects
`hylios_discard_after_scatter_v1`: the full angular draw is retained and host
travel resumes, even through the discarded particle's former volume. The old
shape is retained only for accepted-predecessor exclusion. This explicit virtual
particle interpretation follows the publication's discard-after-scattering
wording; it does not claim a persistent packed medium or author-code equivalence.
Both variants count inward draws in `inwardPostExitEvents`. Data approximation
labels are separate from the recorded transport interpretation. At the exact
zero-angle endpoint, both policies produce identical physical histories and RNG
consumption. The report schema is `hylios-forward-measure-v4`.
For HyLIoS boundary selection, zero fold is the exact smooth delta. Positive
source folds must lie in the explicit binary64 working interval
[sqrt(epsilon),1/sqrt(epsilon)]; unsupported values fail before histories.
The smooth override retains the legacy nonnegative-source validation and does
not silently apply the new positive-fold working interval.

Fixed placement can also
remain impossible; a finite caller transition budget becomes `budget_exceeded`,
not absorption. Invalid samples and numerical failures remain unresolved too.
Detailed overlap failure categories are retained. The CLI always reports the
original number of incident histories. It never divides by completed histories.

After input resolution the CLI flushes `hylios_measure: signal handlers ready` to stderr;
this is the readiness handshake used by the cancellation test, with no timing sleep.
SIGINT/SIGTERM is checked between at most 64 transport transitions and between
every fold-boundary transition. A once-per-run
`hylios_measure: reflected surface branch pending` readiness line marks the first
pending reflected-normal sampler; it is diagnostic progress, not scientific identity. A started history
retains its already tallied absorption and current live weight. All unstarted
histories are recorded as cancelled with initial live weight 1. The complete JSON
is atomically published, then cancellation returns exit130. Other invalid input
or publication failure returns exit1 without replacing existing output. A normal
measurement, including diagnostic unresolved outcomes, returns exit0; users must
inspect `complete_physical_estimate` and `unresolved_history_count`.

Input is limited to 64 MiB, the wavelength grid to 10,000 entries, histories and each
transition cap to 10,000,000, and their product to 10^10. These are explicit tooling
budgets, not physical model cutoffs. In HyLIoS boundary mode, the cap counts kernel
transport work plus every boundary branch/proposal attempt. A rejected normal
costs one unit and keeps the Fresnel branch pinned. Smooth retains its exact
legacy cap ordering, counting transport transitions and excluding its boundary
draws. `boundary_work_convention` records this difference. Positive bounds are validated before running.

## Output and statistics

The result records versions, model/placement/boundary/data conventions, exact-byte
identity and original provenance, selected resolved layer coefficients, fiber
inputs/rate, estimator/RR settings, RNG seed/algorithm and random draw count.
History IDs are zero based; SplitMix64 starts with state
`(seed+history_id) mod 2^64` and maps its top 53 bits to [0,1). Histories use separate
streams, so scheduling/batching cannot change their initial random state.

`caller_work_units` records the complete caller work total.
`aggregate_counters.workUnits` remains transport-only; the separate
`surface_boundary_counters` records branch/proposal attempts, rejected normals,
exterior/interior R/T branches, ready decisions, pending-state stops and failures.
Budget/cancel stops preserve live kernel weight. In HyLIoS boundary mode, invalid-variate and
numerical exceptions cancel pending transport for cleanup and publish their own
unresolved diagnostic classification; they do not abort a partially completed
measurement or add absorption. Boundary numerical failures are identified as
`surface_boundary` in `numerical_failure_details`. The cancellation counter counts
pending boundary-state cleanup, including diagnostics; terminal outcome counts
remain the physical accounting authority. A selected reflected branch still
awaiting a normal is unresolved power, not escaped power.

Outcomes have separate counts, terminal live weights and unresolved flags.
External reflection and subsurface return are distinct. Absorption is accumulated
separately for each layer's host and particle path. Layer host/particle/total path
lengths and all aggregate event counters are included. Selected layers distinguish
`active_individual_spheroids` from `active_complex_pack`; the latter records R,
actual child count, the count interpretation, child axes/coefficient/index and
nominal envelope rate. `complex_layout_policy` and `complex_layout_convention`
record the chosen construction, isotropic axis prior, conservative centre support,
64-child resource limit and 256-proposal restart threshold. An interaction cap is
explicitly absent. `acceptedComplexes` counts accepted outer envelopes, while
`complexChildEntries`/`Exits`/`Reflections` count actual child events.
`complexLayoutProposals`, `complexLayoutContainmentRejections`,
`complexLayoutOverlapRejections`, `complexLayoutRestarts` and
`complexLayoutPairChecks` expose construction work. `complexHostSegments` and
`complexExits` describe internal host travel and successful envelope exits. Zero-weight diagnostics are
conservatively incomplete even when their surviving weight is zero. Roulette
kills are a statistical estimator outcome, excluded from unresolved power and
physical absorption.

Welford means and sample-standard-error estimates use **all incident histories**
for escaped contribution, external reflection, subsurface return, physical
absorption, unresolved live weight, accounted weight, and each layer's host and
particle absorption. A single history has `standard_error=null`. With unresolved
work, escaped contribution is not a complete physical reflectance estimate. Its
remaining conditional expected escaped power lies between zero and unresolved
live weight; future roulette realizations and finite-MC uncertainty are statistical.
Weighted/RR accounted power is tested statistically, never asserted equal to one
on individual paths. Histories killed by compensated roulette can correctly have
sample-accounted power different from one.

The full report is serialized and checked for finite emitted numerical values
before any output mutation. POSIX uses a unique sibling `mkstemp`, checked writes,
`fsync`, close and rename. Windows has an explicit guarded implementation using
`CreateFileW(CREATE_NEW)` with bounded unique candidate names, checked `WriteFile`,
`FlushFileBuffers` and `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`. Failure removes
only the newly created temporary. No fixed `.tmp` race or external subprocess
hashing is used. The POSIX branch was exercised on macOS. Linux and Windows code have not
been compiled or executed in this environment.

## Contract tests and numerical build modes

```sh
python3 tools/test_hylios_measure.py -v
python3 -m unittest discover -s tools -p test_hylios_measure.py -v
RISE_HYLIOS_MEASURE=/absolute/path/to/hylios_measure \
  python3 -m unittest discover -s tools -p test_hylios_measure.py -v
```

Tests default to repository `bin/tools/hylios_measure` (`hylios_measure.exe` on
Windows). `RISE_HYLIOS_MEASURE` selects another binary without consuming unittest
arguments. A missing binary explicitly skips the twenty-three contracts with the build
command. Standard unittest discovery, test selection and flags remain available.

A strict standalone POSIX build reads sources directly and avoids shared-archive
relink races:

```sh
hylios_check_dir="$(mktemp -d)"
inc=src/Library/Materials/HyLIoS
clang++ -std=c++17 -O2 -Wall -Werror -pedantic \
  -fno-fast-math -ffp-contract=off -I src/Library \
  tools/hylios_measure.cpp "$inc/HyLIoSForwardTransport.cpp" \
  "$inc/HyLIoSOptics.cpp" "$inc/HyLIoSParticleGeometry.cpp" \
  "$inc/HyLIoSParticleOverlap.cpp" "$inc/HyLIoSComplexLayout.cpp" \
  "$inc/HyLIoSSurfaceBoundary.cpp" \
  "$inc/HyLIoSSlabTransport.cpp" src/Library/Agent/Json.cpp \
  src/Library/Utilities/RISECBOR64.cpp -o "$hylios_check_dir/hylios_measure"
RISE_HYLIOS_MEASURE="$hylios_check_dir/hylios_measure" \
  python3 tools/test_hylios_measure.py -v
```

The canonical build uses its platform optimization flags. Particle geometry and
certified overlap, complex layout and surface boundary retain strict floating-point compilation.
For isolated checks, compile the source list in the Windows example below with
C++17 and the repository `src/Library` include directory. GCC/Clang strict mode uses
`-fno-fast-math -ffp-contract=off`; ASan/UBSan adds
`-O1 -g -fsanitize=address,undefined` to compilation and linking. Full-LTO checks use
`-O3 -flto=full`, compiling the driver/forward/optics/slab with
`-ffast-math -fno-finite-math-only` and particle geometry, overlap, complex layout, surface boundary, JSON and SHA
with `-fno-fast-math -ffp-contract=off`. Compile each TU separately before linking
so its floating-point contract survives LTO. The isolated modes were exercised on
macOS; Windows/Linux have not been executed in this environment.

Tests use temporary synthetic resolved specimens. Strong oracles include exact
vacuum power, independent Beer+cosine-bottom quadrature, compensated weighted/RR
mean agreement, all-history hard-budget accounting with partial absorption,
cancellation with unstarted initial power, active-particle resolution, inactive
skip, missing coverage and missing explicit complex-policy refusal,
complex count/radius/rate validation with atomic output preservation,
one-child full-envelope exact event counts, successful five-child physical paths,
construction-only budget accounting, visible empty-complex Beer/bottom quadrature,
macro Fresnel mass under rejected normal proposals, exact zero-fold smooth parity,
pinned reflected-proposal budgets, authored-fold validation and in-sampler cancellation,
schema/unit/shape/nonfinite rejection,
source labels and byte-hash preservation, deterministic seeded replay, atomic
failure preservation and concurrent unique publication. No render is invoked.

A standalone Windows build can compile the same implementation with MSVC from
a Developer PowerShell. This command is documented for review and has **not**
been compiled or executed here; no tool target was added to the test-only Windows
CMake project:

```powershell
$repo = (Get-Location).Path
$inc = "$repo\src\Library\Materials\HyLIoS"
New-Item -ItemType Directory -Force "$repo\bin\tools" | Out-Null
Push-Location "$repo\bin\tools"
cl /nologo /std:c++17 /EHsc /W4 /WX /fp:strict /I"$repo\src\Library" `
  "$repo\tools\hylios_measure.cpp" "$inc\HyLIoSForwardTransport.cpp" `
  "$inc\HyLIoSOptics.cpp" "$inc\HyLIoSParticleGeometry.cpp" `
  "$inc\HyLIoSParticleOverlap.cpp" "$inc\HyLIoSComplexLayout.cpp" `
  "$inc\HyLIoSSurfaceBoundary.cpp" `
  "$inc\HyLIoSSlabTransport.cpp" `
  "$repo\src\Library\Agent\Json.cpp" "$repo\src\Library\Utilities\RISECBOR64.cpp" `
  /Fe:"$repo\bin\tools\hylios_measure.exe"
Pop-Location
.\bin\tools\hylios_measure.exe --specimen resolved-s4.json --wavelength-nm 550 `
  --placement null --post-scatter discard --boundary smooth --histories 1000 --max-transitions 10000 `
  --seed 55000001 --output measurement.json
python tools/test_hylios_measure.py -v
```

The two POSIX SIGINT contracts are skipped on Windows; the other twenty-one CLI contracts
have no POSIX-specific invocation requirement. Their Windows implementation
remains unverified here.

## Actual S4 exploratory replay

```sh
python3 tools/hylios_measure_explore.py \
  --specimen resolved-s4.json --output-dir rendered/hylios-s4-measurement
```

This runs sequentially and writes four complete JSON reports plus
`s4-exploration-summary.json` into the chosen directory. `--binary` or
`RISE_HYLIOS_MEASURE` can select an isolated binary. Input wavelengths are fixed
at 550 and 1300 nm and must already be resolved in the specimen. Initial
histories: 1000, normal incidence, smooth boundary override, diagnostic post-scattering policy, analog, cap 10,000
transitions; seed=`wavelength*100000+1`.
The prior explicit approximation specimen produced the exploratory counts below.
They are historical observations for that exact labeled input, rather than
regression expectations for every S4 artifact. The replay used coverage profile
`hylios_cleanroom_approximation_v1`, exact input-byte SHA-256
`adbcae00ae33b95259916c1d35b3d430c144b40e3df8368a69fe97764b84af55`,
and claimed resolved-specimen digest
`9a1ea034eb717f9d1f059687fc4e7f7c55f38319dd1fac6f7bbd9ff8a9267ba9`:

|nm|placement|absorbed|escaped|post-exit reentry unresolved|budget unresolved|
|---|---|---:|---:|---:|---:|
|550|fixed|381|364|220|35|
|550|null|384|408|208|0|
|1300|fixed|254|646|9|91|
|1300|null|283|709|8|0|

Every row retains 1000 incident histories and accounts for all physical absorption,
escaped and unresolved live weight. No numerical failures occurred in this small
exploration. The substantial visible reentry count and fixed-placement budgets
are limitations of the diagnostic policy, not validation of full S4 reflectance. The discard variant continues those inward histories under its separately recorded transport rule. The
underlying approximation data are not author-equivalent or physiologically
validated.

## Full-suite pinned-fixture runtime

The measurement CLI's twenty-three contracts and the HyLIoS binaries can be tested
directly with the commands above. The repository-wide `run_all_tests.sh` also
checks unrelated committed fire records before applying its test filter.

The fire-simulation and production Planck-mean generators explicitly preserve
the published sequential binary64 reduction order and reproduce their committed
bytes under Python 3.9.6 and 3.14.4. The separate quarantined synthetic Voigt/LBL
fixture still requires **Python 3.9** for exact byte parity: newer Python float
`sum()` changes its numerical reductions. That fixture pins its generator and
library source hashes, so changing its accumulation order would also require a
deliberate fixture provenance update. No such record or hash update is part of
this measurement work.

For the full runner on this macOS machine, `/usr/bin/python3` is the verified
3.9.6 interpreter. Prefer it for that invocation only:

```sh
/usr/bin/python3 --version
PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH" ./run_all_tests.sh --filter 'HyLIoS*'
```

On another machine, select a Python 3.9 environment before invoking the full
runner. A newer default interpreter's synthetic-LBL "stale" error is a pinned
fixture runtime limitation, rather than a HyLIoS test failure; it must not be
resolved by silently regenerating scientific records with different bytes.
## First completed discard-policy S4 controls

Using the same input-byte digest, 1000 histories, seeds, smooth boundary and
transition cap as the historical table above, select `--placement null
--post-scatter discard`. The first clean-room runs produced:

| Wavelength (nm) | Absorbed | Escaped | Inward angular events continued | Unresolved histories | Escaped mean ± standard error |
|---|---:|---:|---:|---:|---:|
| 550 | 527 | 473 | 282 | 0 | 0.473 ± 0.01580 |
| 1300 | 288 | 712 | 8 | 0 | 0.712 ± 0.01433 |

Inward angular events can occur more than once in a history. These runs account
for all incident power and have no numerical failures. They demonstrate completion
under the selected versioned interpretation, not agreement with measured skin or
unavailable author code. They still override authored surface folds and use the
explicit approximation data profile. `complete_physical_estimate` describes
completion of this declared forward model, not physiological validation.



## Version 3 exploratory S1–S4 controls (2026-11-26)

Eight sequential runs used the final ownership fix and the standalone mixed-full-LTO
binary, without linking a shared library archive. Every run used 100 normal-incidence
histories, analog absorption, roulette disabled, `--placement null --post-scatter
discard --complex-layout explicit-pack-v1 --boundary smooth`, transition cap 100000,
and the named Eq7 fiber defaults (r=100nm, fraction .22, relative IOR 1.5/1.33).
Seeds were 55001 at 550nm and 130001 at 1300nm. Authored surface folds remained
an explicit smooth-boundary override.

The report version is `hylios-forward-measure-v3`; the kernel version is
`hylios-forward-explicit-complex-draft-v2`. Apple clang 21.0.0
(clang-2100.4.1.1), arm64, compiled geometry, overlap, layout, JSON and SHA strictly;
driver, transport, optics and slab used `-O3 -flto=full -ffast-math
-fno-finite-math-only`. Binary SHA-256:

`ffcee8c844504d25db6e306509f2aa8e7aa89917e1202a4046b96eeb7c27c418`

All inputs carry `approximate=true`, profile
`hylios_cleanroom_approximation_v1`, and spectral bundle SHA-256
`39068ada1e82a488f48781fd50842c1067ade52a44496bdba689d699e60d9d64`.
The first digest below identifies exact opened bytes. The second remains the
producer's unverified canonical resolved-specimen claim, as recorded by the CLI.

| Preset | Exact input-byte SHA-256 | Claimed resolved-specimen SHA-256 |
|---|---|---|
| S1 | `99b796267b69ff37536a8e48d098ef61df2acea827713f62f058a4706e95c2a2` | `6a461fd6ac5ed7592e1dff6e197f585d3a35e07d41994ec675fa661b62f3b5c6` |
| S2 | `f393b06719ab2beb5058c51406303d9dd31119a15910077fc0f8a0b72deb5af2` | `24db13e739012867b2ea9d91b3de41d2976c7c46be8a1a343a34227e9b6ed6e4` |
| S3 | `0f611dd6236e3d4ecfed002657d6cc5b241192a617fc9309b27977a902be1bc8` | `3b51e51263657c8bc7b9071d7ab9aca556b87a53c9a396e0c2f79f4824ad0283` |
| S4 | `2a64624521d0fa790558e173e0309b2eb6503efd1929312d4747f64a536d2417` | `98a4b7eb83cd43f8ad625cc4039332807434c5304176be5acdc228412b7f5bfa` |

R is escaped incident power; A is physical absorption. The uncertainty shown is
the sample standard error over all 100 incident histories. All eight reports
have zero unresolved histories, zero unresolved live weight and zero numerical
failures; incident R+A accounts for one within binary64 summation error. Inward
angular events were continued under discard and are not unresolved outcomes.

| Preset | nm | R ± standard error | A | Inward events continued |
|---|---:|---:|---:|---:|
| S1 | 550 | 0.41 ± 0.04943 | 0.59 | 9 |
| S1 | 1300 | 0.41 ± 0.04943 | 0.59 | 0 |
| S2 | 550 | 0.25 ± 0.04352 | 0.75 | 9 |
| S2 | 1300 | 0.43 ± 0.04976 | 0.57 | 0 |
| S3 | 550 | 0.35 ± 0.04794 | 0.65 | 9 |
| S3 | 1300 | 0.36 ± 0.04824 | 0.64 | 1 |
| S4 | 550 | 0.53 ± 0.05016 | 0.47 | 30 |
| S4 | 1300 | 0.63 ± 0.04852 | 0.37 | 2 |

The work table retains actual event counts. Outer accepts include individual
particles and complex envelopes; the complex subset is shown after `/`. S4 has
individual particles and no complex construction despite the explicitly selected
available policy. Layout proposals and pair checks are different units of work.
Wall times are sequential elapsed process times on this workspace, with other
workspace activity possible; they are not isolated throughput guarantees.

| Preset | nm | Outer accepts / complexes | Child entries / reflections | Layout proposals | Restarts | Pair checks | Work units | Wall seconds |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| S1 | 550 | 971 / 971 | 411 / 323 | 856279 | 2003 | 680925 | 863744 | 7.341 |
| S1 | 1300 | 968 / 968 | 395 / 322 | 791633 | 1839 | 628291 | 798799 | 6.287 |
| S2 | 550 | 651 / 651 | 282 / 202 | 552802 | 1297 | 440986 | 557787 | 4.266 |
| S2 | 1300 | 833 / 833 | 358 / 297 | 686854 | 1596 | 545275 | 693264 | 5.714 |
| S3 | 550 | 818 / 818 | 330 / 237 | 658590 | 1538 | 524123 | 664936 | 5.387 |
| S3 | 1300 | 754 / 754 | 319 / 275 | 641666 | 1502 | 509225 | 647814 | 3.471 |
| S4 | 550 | 3111 / 0 | 0 / 0 | 0 | 0 | 0 | 19406 | 0.026 |
| S4 | 1300 | 8641 / 0 | 0 / 0 | 0 | 0 | 0 | 54693 | 0.053 |

Reports are named `rendered/hylios-complex/S{1,2,3,4}-{550,1300}-measure-v3.json`;
`rendered/hylios-complex/exploration-v3-summary.json` retains report hashes,
identities, unrounded means and work/timing values. These generated files are
local evidence, not committed fixtures. One replay command, after rebuilding the
declared binary mode, is:

```sh
./bin/tools/hylios_measure --specimen rendered/hylios-complex/S3-resolved.json \
  --wavelength-nm 550 --placement null --post-scatter discard \
  --complex-layout explicit-pack-v1 --boundary smooth --histories 100 \
  --max-transitions 100000 --seed 55001 --estimator analog --roulette-target 0 \
  --output rendered/hylios-complex/S3-550-measure-v3.json
```

These are small exploratory samples of the selected clean-room forward model
and explicitly approximate data. They demonstrate completed accounting for these
inputs, not convergence, author-code equivalence, agreement with measured skin,
or a validated ordering of presets. More histories and angle studies remain
separate work; none of these runs implements authored surface folds.

The eighteen CLI contracts at that milestone passed source-linked strict, ASan/UBSan and mixed full-LTO
builds. They included the exact full-envelope one-child limit that was observed
RED with 267 numerical failures among 500 histories before owned-boundary reuse,
and GREEN with complete R=1 afterward. The same test checks exact child/envelope
counts and identical physical histories with or without a consistent declared
encounter rate. Source, kernel and measurement mechanics tests remain separate
from the exploratory scientific interpretation above.


## Version 4 authored-fold controls (2026-11-26)

Eight runs selected `--boundary hylios2015` and the authored fold aspects
S1=.1, S2=.25, S3=.1, S4=.45. All other experimental inputs match the v3 smooth
controls above: 100 normal-incidence histories per row, analog, no roulette,
explicit null/discard/explicit-pack policies, named Eq7 fiber defaults,
transition cap 100000, seeds 55001 and 130001. Every exact input-byte digest and
claimed/source bundle identity matches the corresponding v3 row. The new cap
counts transport plus boundary work; no row exhausted it. Existing v3 reports
were preserved.

The source-linked mixed-full-LTO binary includes strict geometry, overlap,
complex layout, surface boundary, JSON and SHA TUs; caller/forward/optics/slab use
fast math with finite-math-only disabled. Compiler and platform match the v3
record. Measurement version is `hylios-forward-measure-v4`, kernel version remains
`hylios-forward-explicit-complex-draft-v2`, and boundary law is
`hylios2015_macro_fresnel_normal_perturb_v1`. Binary SHA-256:

`b17724743a593fa878d9baf1da8cf6089bf32d1e37768e0f586949815521b90d`

All 800 incident histories complete with zero unresolved live weight, zero
budget/cancel stops, zero numerical failures and zero invalid boundary samples.
R+A accounts for one per row within binary64 summation error. External and
subsurface returns below are counts out of 100; their sum gives escaped R.
The uncertainty is the sample standard error, not an author-reference error.

| Preset | nm | Fold s | Escaped R ± standard error | External / subsurface returns | A | Historical smooth R |
|---|---:|---:|---:|---:|---:|---:|
| S1 | 550 | 0.10 | 0.40 ± 0.04924 | 4 / 36 | 0.60 | 0.41 |
| S1 | 1300 | 0.10 | 0.36 ± 0.04824 | 2 / 34 | 0.64 | 0.41 |
| S2 | 550 | 0.25 | 0.21 ± 0.04094 | 4 / 17 | 0.79 | 0.25 |
| S2 | 1300 | 0.25 | 0.45 ± 0.05000 | 2 / 43 | 0.55 | 0.43 |
| S3 | 550 | 0.10 | 0.31 ± 0.04648 | 4 / 27 | 0.69 | 0.35 |
| S3 | 1300 | 0.10 | 0.41 ± 0.04943 | 2 / 39 | 0.59 | 0.36 |
| S4 | 550 | 0.45 | 0.49 ± 0.05024 | 4 / 45 | 0.51 | 0.53 |
| S4 | 1300 | 0.45 | 0.72 ± 0.04513 | 2 / 70 | 0.28 | 0.63 |

Every selected reflected branch completes exactly once in these rows:
normal proposals minus rejected normals equals exterior plus interior reflection
branches. Boundary work equals branch attempts plus normal proposal attempts;
caller work equals transport work plus boundary work. The table reports these
raw counts and sequential elapsed process times, with no isolated-throughput
claim. R/T branch counts are exterior R/T then interior R/T.

| Preset | nm | Transport work | Exterior R/T; interior R/T | Normal proposals / rejections | Boundary work | Caller work | Wall seconds |
|---|---:|---:|---:|---:|---:|---:|---:|
| S1 | 550 | 736203 | 4/96; 101/36 | 439 / 334 | 676 | 736879 | 2.277 |
| S1 | 1300 | 736667 | 2/98; 70/34 | 260 / 188 | 464 | 737131 | 2.299 |
| S2 | 550 | 489905 | 4/96; 50/17 | 303 / 249 | 470 | 490375 | 1.513 |
| S2 | 1300 | 692913 | 2/98; 66/43 | 298 / 230 | 507 | 693420 | 2.164 |
| S3 | 550 | 646455 | 4/96; 107/27 | 518 / 407 | 752 | 647207 | 1.976 |
| S3 | 1300 | 782380 | 2/98; 107/39 | 494 / 385 | 740 | 783120 | 2.418 |
| S4 | 550 | 19297 | 4/96; 85/45 | 396 / 307 | 626 | 19923 | 0.022 |
| S4 | 1300 | 50788 | 2/98; 168/70 | 932 / 762 | 1270 | 52058 | 0.045 |

Distinct reports are named
`rendered/hylios-complex/S{1,2,3,4}-{550,1300}-measure-v4-hylios2015.json`.
`rendered/hylios-complex/exploration-v4-hylios2015-summary.json` retains exact
input/report/binary hashes, unrounded means, all boundary counters and timing.
A replay command after rebuilding the declared mode is:

```sh
./bin/tools/hylios_measure --specimen rendered/hylios-complex/S3-resolved.json \
  --wavelength-nm 550 --placement null --post-scatter discard \
  --complex-layout explicit-pack-v1 --boundary hylios2015 --histories 100 \
  --max-transitions 100000 --seed 55001 --estimator analog --roulette-target 0 \
  --output rendered/hylios-complex/S3-550-measure-v4-hylios2015.json
```

These are accounting and implementation sanity controls using the same explicitly
approximate spectra/composition and selected clean-room transport rules. At only
100 histories, smooth/fold differences and preset ordering do not establish
statistical significance or convergence; changed random consumption also means
identical seeds are not an independently validated paired estimator. No
comparison to author code or measured skin is inferred. Incidence studies,
outgoing histograms and any adjoint/renderer boundary remain separate increments.

The complete 23-contract CLI suite passes strict, ASan/UBSan and mixed full LTO.
A temporary mutation omitting boundary work from the cap was observed RED in the
reflected-proposal budget contract (five proposals occurred with cap1), while the
implemented guard is GREEN. An additional weighted cap2 control preserves
exp(-.2) live power plus 1-exp(-.2) prior host absorption across the new caller
work accounting. Invalid used variate and numerical-boundary paths are defensive
adapter diagnostics; the production SplitMix64 source emits only valid uniforms,
while boundary-unit tests directly exercise invalid variates.
