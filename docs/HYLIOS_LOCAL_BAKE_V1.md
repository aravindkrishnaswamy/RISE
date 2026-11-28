# Bounded local bake increment (compact identity v2)

This is an offline local angular adapter, not a renderer material or biological
validation. `hylios_measure bake-local` invokes the shared strict resolved-specimen
parser and the measurement-v6 history driver. Ordinary measurement invocation is
unchanged. No renderer, live archive, global build, or render is needed by these
private gates.

## Supported closure

Every configured wavelength is checked before any random history. Individual
particles and explicit complexes with zero or one child use the proved
`hylios_angular_lineage_v2` laws. Nonempty complexes with more than one child are
accepted only below 1400nm. At/above 1400nm the final angular scale is exactly zero;
those configurations are refused regardless of the observed return histogram.

For the positive-scale extension the static argument is a transition proof,
not an inference from zero observed unknown mass:

1. Candidate placement accepts an envelope contained inside one layer; rejected
   candidates return to bulk without applying a child direction map. Layout and
   child/host phases cannot issue an exterior boundary encounter or return.
2. While an envelope is active, child reflection leads to reflected perturbation
   and another child chord. Child transmission leads back to complex host travel.
   Either can have unknown intermediate angular lineage; neither returns outside.
3. The only successful body exit is the no-child-selected end block of
   `ComplexHost`. For every nonempty child layout it clears body ownership and
   enters `ForwardPerturbation`, including paths which happened to miss children.
4. Below 1400nm, that phase cannot call `FinishForwardScatter` through its
   zero-scale shortcut. Rejected proposals stay in the same phase. Invalid samples,
   geometry errors, cancellation, absorption, and exhausted work terminate without
   a positive published return. An accepted exponential-angle draw applies the
   independent positive-width law, stamps AC, then calls `FinishForwardScatter`.
5. Diagnostic inward reentry is unresolved and refuses the bake. Successful
   diagnostic outward completion and discard both enter bulk after the accepted
   kick. Any later nonempty body again passes through the same dominator. Empty
   complexes apply the identity map and preserve prior classification. The proved
   planar, bottom, fiber, and individual/N1 laws complete the return-path closure.

The AC certificate concerns the ideal ensemble of the named angular laws. Every
IEEE/RNG output is discrete. The separately versioned transport-v3 correction
retains the sampled sine instead of reconstructing it from a cosine rounded to
one; it does not promise a generic Cartesian vector changes under every sub-ULP
rotation. Finite RNG and angular-grid approximations need refinement evidence.

Any actual unknown positive return still refuses publication. So does every
unresolved/numeric/cancelled/budget history. The full-model `hylios_measure` route
remains available for unsupported local configurations. No reciprocity,
azimuth-reduction, exit conditioning, or double Fresnel coat is introduced.

## Grids, frame, and history identity

Incident and outgoing grids are independent full t=mu^2, phi rectangles. Each
incident history consumes two independent top32 midpoint draws from the indexed
SplitMix64 top53 stream before launch, giving a named projected-cell quadrature.
Every cell's representable endpoint support is checked before generation. The
frame is right-handed (kernel +X,-Y,-Z): local outward wi launches along
(-wi.x,+wi.y,+wi.z); escaped kernel d becomes (d.x,-d.y,-d.z). Atomic planar mirror
returns are classified by proved lineage before averaging or binning. Absorption
and every return are counted with all N histories in the denominator.

For L wavelengths, I incident cells, R replicates, N cell histories and G normal
histories, block=I*R*N+R*G. Angular ID=l*block+i*R*N+r*N+h;
normal ID=l*block+I*R*N+r*G+h. Checked uint64 arithmetic proves the contiguous
index domain [0,L*block) before generation. Seed+ID modulo 2^64 is injective over
that domain, including UINT64_MAX seeds. Chunk size and traversal order cannot
change IDs. Normal wi=(0,0,1) uses the distinct final index range; the last angular
cell never substitutes for it. Every fold branch/proposal uses the shared work
counter and the per-history cap includes bulk plus surface attempts.

Default library grids have 2x2 cells, CLI grids 2x4, with 64 histories/replicate
at the library level and two independent replicates. CLI history count stays
explicit. Hard preparation bounds include a separately budgeted 64MiB exact source artifact,
4096 bytes per canonical semantic record, 64MiB full report,
checked worst-case total histories/work, and caller response/asset/resident caps.
There is no fixed eight-node cap: the actual canonical wavelength-list extent,
full compact record sizes, response limits, and total metadata admission bound
diagnose unsupported grids before RNG. Public S1/S4 approximation-profile inputs
prepare at 8/16/32/64 exact nodes over 380–780nm without increasing codec limits.
At 64 nodes their model/data/specimen records are 1445/2218/2618 bytes; all nine
records total 7281 bytes and the conservative full-metadata admission bound is
20253 bytes. Full raw provenance remains in the exact ~1.02MiB source artifact.
The peak admission bound accounts for three response storage objects, 64 report-byte
units for each worst-case report byte, and 256 source-byte units. Exactly one
resolver result is alive at a time, so full source trees are not retained per node.
It is a conservative logical payload/allocation bound, not a process RSS claim.

## Finite passivity and numerical error

Only analog absorption with roulette disabled is supported. Every returned
history must have weight exactly one. Integer counts and all replicate-combined
denominators are <=2^53, so conversion to binary64 is exact. For count c and
all-history denominator D, q=nextafter(round_nearest(c/D),0) is <=c/D (zero is
handled separately). Let A be the core's retained represented projected area,
and a its public binary64 area. For a nonzero normal area,
Aplus=nextafter(a,+infinity) is >=A. The coefficient is
f=nextafter(round_nearest(q/Aplus),0). Hence exactly f*A<=q<=c/D.
The mirror coefficient uses the same downward count ratio, so summing the exact
represented products plus mirror mass is <=sum(return counts)/D<=1. There is no
row renormalization or physical clamp. Subnormal projected areas and unrepresentable
positive coefficients are refused. The core revalidates the source binary64
rows before the codec's independently versioned downward float32 encoding.

For normal positive binary64 intermediates, two quotient round-downs plus the
area upper bound lose less than 8*epsilon times the continuous row mass; the
mirror ratio loses less than 2*epsilon absolutely. The report declares the
conservative universal 10*epsilon row-construction bound for each nonzero row.
This bound is distinct from the codec's float32 coefficient/row error, Monte
Carlo error, and angular/spectral approximation error. Source construction is
gated with unequal-area near-unit rows and denominators through 2^53. No numerical
bound is presented as a biological or angular-grid error certificate.

## Identity, statistics, and publication

Semantic records contain every actual numeric input consumed by the forward
kernel. Packed arrays use exact IEEE binary64 little-endian bytes, with the
versioned `f64le_columns_constant_scalars_v2` layout. A series is either a float64
constant or an N*8 byte array; each six-layer dictionary stores unique series
once and six integer indices (null means no active particle). Column equality
uses exact packed bits, including signed zero. Host absorption and exact wavelength
nodes live in `data`; geometry and layer-IOR columns in `specimen`; particle
absorption columns and fiber parameters/rates in `model`. Geometry field names
appear once, with thickness/organization/fraction/axes/particle IOR/envelope/count
values stored once rather than repeated at every wavelength. Exterior IOR is an
actual constant referencing the stored exact node list. Model, placement,
postscatter, boundary, lineage, scale and approximation versions remain explicit.

Full constituent records and upstream provenance are preserved in an immutable
content-addressed exact-source sidecar. `specimen` binds its independently computed
SHA256 and exact byte extent. The production statistics and full report repeat
that binding, and pair loading recomputes the actual source digest. Independently
prepared semantic keys reconstruct the compact numeric records from the supplied
source bytes. They do not require the author's grid, seed, history count, or work
limits. Publication and loading independently re-resolve the verified source
snapshot and compare all nine complete records and coverage, including numerical
values, geometry, selected model options and versions. No producer hash substitutes
for this rederivation. The source is reparsed independently for every node at
bake, publication and loading. Ordered duplicate-key validation adds comparison
work dependent on key count and length; only one resolver tree is retained at a
time. A producer's bundle claim remains explicitly uncertified; source
hashes identify the exact input and do not certify laboratory/source truth.

The production identity includes actual grids/nodes through the codec, exact seed,
replicate/history/work limits, indexed RNG/jitter/estimator versions, compiler
identity, and SHA256 independently computed from the caller-selected artifact path.
The library caller identifies the relevant build; the CLI resolves its own
executable. This records bytes read from the selected path and attests neither
execution of that artifact nor actual Monte Carlo execution. The build record has
exactly `adapter_version`, `compiler`, `selected_artifact_sha256` and
`adapter_strict_fp`: required matching adapter version, NFC compiler text of 1..256 bytes excluding ASCII control characters
(U+0000..U+001F and U+007F) and strings containing only ASCII spaces, lowercase 64-hex SHA256, and boolean true.
Publication and loading both enforce this schema. Compiler identity uses guarded
Clang/MSVC/GNU/unknown branches, without assuming `__VERSION__` exists. Compatibility gates compile the complete baker TU with that macro
undefined, then exercise the exact extracted MSVC full, MSVC short and unknown
branches on the host after standard headers. These are syntax/branch controls;
a native Windows toolchain and runtime are unavailable. These are
structural provenance checks, not proof that the selected artifact ran. A caller cannot
replace those values with producer hashes through `PrepareLocalBakeSemantic`.

The canonical CBOR full report contains every wavelength/angular/normal row,
per-replicate continuous-bin/mirror/physical-absorption counts, actual bulk and
surface work, per-bin/row Bernoulli history SE and independent-replicate SE, Wilson
95% diagnostics (nonzero upper limits for unseen bins), and numerical error
bounds. The compact asset statistics retain actual totals, maximum uncertainties,
construction error, report format/extent and exact report hash. SEs and Wilson
intervals are diagnostic, not simultaneous coverage guarantees; zero observed bins
do not prove zero support. Angular/spectral refinement is explicitly uncertified.

Bundle validation parses closed row, count, statistic and work schemas, not
opaque row placeholders. It checks complete wavelength/cell/normal ordering,
replicate/bin dimensions, per-replicate all-history count conservation, exact
derived bin/returned statistics, per-replicate work caps and aggregate summaries.
Every continuous, mirror and normal-guide value is reconstructed from counts
using the versioned downward binary64 estimator. Publication compares the raw
values; loading compares the codec's shared exact downward float32 conversion
and recomputed encoding-error bounds. A genuine unit-return report cannot
certify a zero response. These checks establish internal consistency only; they
do not attest actual Monte Carlo execution. Only one bounded row's count/stat
arrays are retained while parsing, with explicit resident scratch admission. The
scratch bound explicitly includes each nested vector header, all six outer
vector objects, four replicate work/count columns, statistics, and bounded
production-record decoder overhead. The 1024-replicate boundary is tested.

Statistics verification requires strict IEEE binary64 round-to-nearest operations,
no contraction, and the same specified operation order. Runtime rounding changes
are refused before preparation. Current strict and full-LTO tests share the
current compiler/platform; cross-compiler/libm last-bit equivalence is not
certified. A differing recomputation is refused rather than silently tolerated.
This limitation concerns report verification; it is not execution attestation.

Publication verifies the exact source bytes and the report source/seed/history
bindings, writes/verifies immutable content-addressed source and report files,
then atomically publishes the asset. A failed replacement retains the previous
asset's dependencies. Late refusal can leave unreferenced immutable sidecars; it
cannot publish a new complete asset. `LoadLocalBake` validates all three files.
All specimen, source, report and build-artifact inputs use the same descriptor
primitive as the codec: nonblocking open on POSIX, fstat of that exact descriptor,
regular-file refusal, extent budget before allocation, exact reads and trailing
checks. FIFO path races cannot block at open, and snapshots retain the opened
regular object through rename/replacement. Loader source/report snapshots are
hashed and inspected from the same owned bytes. The full source is independently
resolved again; a valid source digest cannot override mismatched numeric semantics.
Source reconstruction reserves its 256*source-byte bound. Report snapshots plus
one-row count/stat scratch are admitted against the remaining resident budget.

The caller-selected build artifact is also hashed through a bounded stream (256MiB cap).
The baker pathname entry points validate scalar UTF8, reject embedded NUL, and bound
the complete sidecar pathname to 32768 bytes (81 bytes reserved for the longest
content-addressed suffix). Valid UTF8 source/report paths have an end-to-end test.
The codec's advertised wire/resident admission is checked before any history,
using the same dimensions and maximum metadata envelope as Save.


## Gates and remaining work

Focused tests cover unit diffuse, pure synthetic mirror, black absorber, mixed
surface/absorption guide differences, exact normal indices, N0/N1, positive-kick
N>1 endpoint closure in both policies, nonsymmetric frame bridge, all-history
passivity, work/cancel refusal, exact U64 seed wrap, scheduling reproducibility,
semantic mismatch, report and asset corruption, and failed atomic replacement.
Standalone strict, ASan/UBSan and full-LTO/fast-math caller gates use the production
`COLORS_RGB` and `MERSENNE53` defines;
only the new baker, certified geometry/overlap/layout, and surface-boundary TUs
use strict FP in the production LTO gate. Core, asset, resolver, driver, JSON,
CBOR and Disk I/O dependencies use the main project's default fast-math flags
with finite-math disabled. The build record's `adapter_strict_fp` describes the
baker TU rather than the complete artifact. CLI controls include
real signal cancellation after handler readiness and unchanged default dispatch.
RED mutants separately exercise frame, static refusal, division passivity and
three transport radius families; endpoint-gap reassociation has its own LTO RED.

The one-wavelength, four-history S1/S4 publish/reload checks are explicitly smoke
controls with no accuracy claim. Full 32-node synthetic bakes, exact dense column
retention through 64 nodes, actual capacity refusal, source/report hash mutations
and changed-provenance/selected-coefficient key mismatches are focused regressions.

The production renderer/material, source-data certification, calibrated grid and
spectral convergence thresholds, held-out scientific experiments, spatial model,
adjoint transport, D65 guide projection and renderer compatibility remain outside
this bounded increment. The artifact is a declared local approximation; modest
synthetic tests do not establish skin accuracy.

Before any generic metadata decode, the adapter uses the codec's record scan
without generic Value construction
(the successful scan allocates nothing; rejection diagnostics may allocate bounded
exception text). Its profile is 4096 bytes, a nonempty map, depth eight and 8192
tokens. The adapter admits the
aggregate encoded-record decoder bound, owned scalar buffers and fixed control
scratch against the resident budget. Canonical encoding and NFC validation occur
after this admission. The compiler-text schema deliberately accepts other Unicode
characters, including U+0085, U+2028 and U+200B; it makes no Unicode printability
claim. These structural checks do not attest execution.
