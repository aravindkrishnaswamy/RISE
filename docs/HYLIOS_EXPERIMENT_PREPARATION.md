# Shared HyLIoS experiment preparation

`ResolveExperiment(sourceBytes, config)` owns and validates an exact resolved
specimen before selecting one authored wavelength node. Its immutable value
exposes the original parsed provenance, the actual six resolved layers, selected
numeric records, source-byte SHA256, fold aspect and requested wavelength. It
adds no spectral interpolation or biological defaults. Claimed upstream hashes
remain producer provenance; they are not independently certified by this parser.

The 64 MiB source bound, numeric-token magnitude/underflow rules, UTF-8 scalar
validation, duplicate-key checks, six-layer ordering, spectrum shapes and
approximation/profile consistency are retained from measurement v6. Shared
`ValidateForwardLayers` checks cumulative representable layer planes, coefficients,
geometric rates and positive-measure particle/envelope containment. The forward
kernel uses that same no-RNG validator. Invalid input or unsupported numerical
ranges can throw `std::invalid_argument` or runtime-derived exceptions; reusable
callers must handle the `std::exception` family and reject the experiment.

`DriveForwardExperimentHistory` uses a caller-owned kernel, state, sampler and
surface counters. It validates state ownership and the sampler's variable-length
capability before either boundary path, including zero-work and terminal calls.
No scene cache or hidden TLS is introduced. Cancellation/progress callbacks run
synchronously on the calling thread; cancellation is latched for that invocation.
Every attempted fold branch or normal proposal, including rejection and failure,
counts as one surface work unit. Smooth boundaries retain measurement v6's
uncharged boundary dispatch and original bulk-cap ordering. This distinction is
part of the declared driver convention, not an interchangeable work estimate.

Fold boundary failures cancel transport and return an explicit diagnostic.
Smooth boundary failures and callback exceptions propagate; they may leave a
pending, nonterminal state. A caller must abort/reject that experiment after an
exception, rather than treating it as absorption or assuming a diagnostic return.
The CLI catches errors at its publication boundary. Histograms, moment statistics,
signal handling, progress throttling and publication remain in the CLI.

`ForwardExperimentRandom` preserves the SplitMix64/top-53-bit generator and draw
counter. Seed plus history index uses uint64 modular arithmetic. A producer must
check its indexing cardinalities before launching histories. This convention
provides reproducibility; it does not claim mathematically independent finite
PRNG streams or certify a production bake by itself.

## JSON locale and lifetime

The shared JSON codec parses numbers with an immutable private C locale and
serializes nonintegral numbers with the classic C++ locale and 17 significant
digits. It never changes process or thread locale. One numeric locale object and
its implementation handle intentionally survive until process exit, so late
`atexit` and global-destructor callers cannot encounter a destroyed singleton.
There is no per-token locale allocation, hidden TLS, or production test counter.

Serialization now constructs a classic-locale stream for each nonintegral number.
A local review microbenchmark of one million such values took 0.529 seconds,
versus 0.424 seconds for the former C-locale formatting, with identical output
bytes. This roughly 25% formatting cost is not an end-to-end renderer or agent
throughput measurement. Integer formatting retains its existing path.

## Verification and scope

Focused repository tests are `HyLIoSResolvedExperimentTest`, `AgentJsonLocaleTest`
and `AgentJsonLifetimeTest`. Private strict, ASan/UBSan and mixed full-LTO gates
passed all three, including actual late callers, two installed comma-decimal
locales, custom C++ locale facets and a four-thread locale check. Five independent
mutations fail the intended controls: wrong selected node, missing fold cancellation
count, successful no-op publication, ambient numeric locale and static teardown.

The refactored measurement CLI passed 29 contracts in each compiler mode. Forty-two
seeded comparisons matched complete JSON bytes and stderr against the original
measurement implementation, with fresh output required for each run. Android NDK
27 arm64/API29 compilation passed; Windows runtime execution remains unverified.
These checks establish the shared preparation/driver behavior, not biological
validation, completed response baking, or renderer calibration. All four new
Library files are registered in the five authoritative build projects.
