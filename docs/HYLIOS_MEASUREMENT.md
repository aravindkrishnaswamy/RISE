# HyLIoS forward diagnostic measurement

`tools/hylios_measure.cpp` measures the current forward-only individual-spheroid
kernel. It is an explicit scientific diagnostic, with unresolved histories retained
in the result. It does not validate the publication's physiological reflectance.

## Build and run

From the repository root on macOS/Linux:

```sh
make -C build/make/rise build-tool/hylios_measure
./bin/tools/hylios_measure \
  --specimen resolved-s4.json \
  --wavelength-nm 550 --placement null --boundary smooth \
  --histories 1000 --max-transitions 10000 --seed 55000001 \
  --output measurement.json
python3 -m unittest discover -s tools -p test_hylios_measure.py -v
```

The existing generic tool rule discovers this source; it is not added to `all`.
Fresh checkouts need the platform setup described in [AGENTS.md](../AGENTS.md).
The tool links the shared forward transport, slab/optics, particle geometry and
certified overlap implementations; it does not run the renderer.

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
`--boundary smooth`, `--histories N`, `--max-transitions N`, `--seed U64`,
`--output FILE`. Every option occurs at most once; unknown options fail.
The wavelength must exactly equal an explicitly resolved grid value; no spectral
interpolation occurs. Six layers must use SC/SG/SS/SB/PD/RD order and SI unit tags.
Top-level and nested bundle approximation/profile/identity metadata must agree;
the known strict and approximation profiles cannot contradict their flags.
All coefficient/index arrays must have the complete grid shape, even at
unselected wavelengths. Duplicate JSON keys and nonfinite values are rejected.

JSON numbers use binary64 semantics, with supported magnitude at most
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
Active organelles need evaluated internal absorption and individually dispersed
organization; active complexes are refused. Disabled particles are skipped,
including their unused geometry/absorption fields, but cannot claim positive
fraction. When a producer supplies a geometric rate it must match the rate
recomputed from its axes and fraction to the documented FP rounding bound.

Optional inputs have named, reported values rather than hidden kernel defaults:

| Option | Reported default / convention |
|---|---|
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

## Scientific scope and unresolved histories

This is a forward measurement diagnostic of the current individually dispersed
spheroid milestone. It does not support complexes, S1–S3 reproduction, reverse
camera sampling, a marginal PDF, adjoint transport or a reciprocity claim.
`--boundary smooth` is a mandatory explicit **override** of authored fold metadata.
The original fold aspect ratio is retained and
`authored_surface_folds_simulated=false` is explicit. Surface folds are not
implemented. Choosing smooth must never be interpreted as evaluating that field.

Placement is mandatory: `fixed` selects `FixedEventDiagnosticV1`, with a frozen
entry event and resampled geometry; `null` selects `NullCollisionPlacementV1`,
where a definitely invalid single geometry consumes a null event. Both follow the
versioned kernel conventions. Their accepted process is different, and nominal
volume fraction is not asserted equal to realized accepted fraction. Counts retain
candidates, attempts, accepted particles, nulls and rejection causes.

The publication does not specify continuation when its post-exit forward angular
law points into the just-left particle. That outcome remains explicit
`post_exit_reentry` unresolved work with its live weight. Fixed placement can also
remain impossible; a finite caller transition budget becomes `budget_exceeded`,
not absorption. Invalid samples and numerical failures remain unresolved too.
Detailed overlap failure categories are retained. The CLI always reports the
original number of incident histories. It never divides by completed histories.

After preflight the CLI flushes `hylios_measure: signal handlers ready` to stderr;
this is the readiness handshake used by the cancellation test, with no timing sleep.
SIGINT/SIGTERM is checked between at most 64-transition batches. A started history
retains its already tallied absorption and current live weight. All unstarted
histories are recorded as cancelled with initial live weight 1. The complete JSON
is atomically published, then cancellation returns exit130. Other invalid input
or publication failure returns exit1 without replacing existing output. A normal
measurement, including diagnostic unresolved outcomes, returns exit0; users must
inspect `complete_physical_estimate` and `unresolved_history_count`.

Input is limited to 64 MiB, the wavelength grid to 10,000 entries, histories and each
transition cap to 10,000,000, and their product to 10^10. These are explicit tooling
budgets, not physical model cutoffs. Positive bounds are validated before running.

## Output and statistics

The result records versions, model/placement/boundary/data conventions, exact-byte
identity and original provenance, selected resolved layer coefficients, fiber
inputs/rate, estimator/RR settings, RNG seed/algorithm and random draw count.
History IDs are zero based; SplitMix64 starts with state
`(seed+history_id) mod 2^64` and maps its top 53 bits to [0,1). Histories use separate
streams, so scheduling/batching cannot change their initial random state.

Outcomes have separate counts, terminal live weights and unresolved flags.
External reflection and subsurface return are distinct. Absorption is accumulated
separately for each layer's host and particle path. Layer host/particle/total path
lengths and all aggregate event counters are included. Zero-weight diagnostics are
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
arguments. A missing binary explicitly skips the twelve contracts with the build
command. Standard unittest discovery, test selection and flags remain available.

The canonical build uses its platform optimization flags. Particle geometry and
certified overlap retain their existing strict floating-point build registrations.
For isolated checks, compile the source list in the Windows example below with
C++17 and the repository `src/Library` include directory. GCC/Clang strict mode uses
`-fno-fast-math -ffp-contract=off`; ASan/UBSan adds
`-O1 -g -fsanitize=address,undefined` to compilation and linking. Full-LTO checks use
`-O3 -flto=full`, compiling the driver/forward/optics/slab with
`-ffast-math -fno-finite-math-only` and particle geometry, overlap, JSON and SHA
with `-fno-fast-math -ffp-contract=off`. Compile each TU separately before linking
so its floating-point contract survives LTO. The isolated modes were exercised on
macOS; Windows/Linux have not been executed in this environment.

Tests use temporary synthetic resolved specimens. Strong oracles include exact
vacuum power, independent Beer+cosine-bottom quadrature, compensated weighted/RR
mean agreement, all-history hard-budget accounting with partial absorption,
cancellation with unstarted initial power, active-particle resolution, inactive
skip, missing coverage/complex refusal, schema/unit/shape/nonfinite rejection,
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
  "$inc\HyLIoSParticleOverlap.cpp" "$inc\HyLIoSSlabTransport.cpp" `
  "$repo\src\Library\Agent\Json.cpp" "$repo\src\Library\Utilities\RISECBOR64.cpp" `
  /Fe:"$repo\bin\tools\hylios_measure.exe"
Pop-Location
.\bin\tools\hylios_measure.exe --specimen resolved-s4.json --wavelength-nm 550 `
  --placement null --boundary smooth --histories 1000 --max-transitions 10000 `
  --seed 55000001 --output measurement.json
python tools/test_hylios_measure.py -v
```

The POSIX SIGINT contract is skipped on Windows; the other eleven CLI contracts
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
histories: 1000, normal incidence, smooth boundary override, analog, cap 10,000
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
are model limitations to resolve, not validation of full S4 reflectance. The
underlying approximation data are not author-equivalent or physiologically
validated.

## Full-suite pinned-fixture runtime

The measurement CLI's twelve contracts and the HyLIoS binaries can be tested
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
