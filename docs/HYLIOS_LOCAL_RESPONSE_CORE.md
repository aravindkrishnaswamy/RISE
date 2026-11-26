# HyLIoS P4 local angular numeric core

This increment supplies a synthetic, immutable projected-cell response and its
nonreciprocal camera transpose. It is **not** a skin asset, a bake, a material,
or a completed P4/P5 milestone. No optical data or oracle histories enter these
tests. No current forward-kernel or measurement-CLI behavior changes. The remaining
bake, asset and renderer acceptance gates are tracked in
[HYLIOS_WORKPLAN.md](HYLIOS_WORKPLAN.md).

`HyLIoSLocalResponse.{h,cpp}` has no renderer dependencies. The representation
tag is `hylios_local_projected_cells_v1`. Independent full incident and outgoing
grids avoid any unproved azimuth-rotation reduction, reciprocity assumption,
or symmetrization. `LocalResponseDefinition` contains caller-supplied **physical
coefficients**, not an MC estimator or a file format. Its incident-cell-constant
interpretation corresponds to projected-angle incident-cell averages when a
future bake actually measures those averages. This code does not establish
that an arbitrary supplied table represents those averages.

Both directions point away from a planar surface. In a right-handed frame,
+Z is the normal and azimuth is `atan2(y,x)`, measured from +X toward +Y.
The forward oracle launch direction is the negative incident direction.
Store `t=mu^2`; projected-angle measure is `dSigma=dt*dphi/2`. A cell's area
is `(t1-t0)*(phi1-phi0)/2`. Physical `F[lambda][i][j]` is constant inside the
two angular cells. Its forward returned energy is `F_ij*A_j`; it is not a
solid-angle density. At a fixed outgoing cell `j`, the continuous camera
mass is `C_j=sum_i F_ij*A_i`. Passive rows do not imply `C_j<=1`.

The only atom is the proved same-exterior planar mirror:
`S(t,phi)=(t,wrap(phi+pi))`. Its scalar coefficient has the same incident-cell
interpretation as F. Camera atom mass is looked up in the **incident** cell
of `S^-1(wo)`, even when the outgoing grid differs. The sampled atom follows
the actual query direction, rather than a grid center. No rough-boundary,
noninvertible, or unknown singular map is accepted implicitly. Normal incidence
has canonical azimuth zero. Angular cells are half-open; the last polar cell
owns `t=1`. Exact grazing queries are excluded (`0<t<=1`).

For canonical binary64 angles, the exact map's scalar representative preserves
the side of **every** authored binary64 incident-cell edge. Upper-half queries
use `phi-pi`, which is exact by Sterbenz. Lower-half queries use the greatest
binary64 value not above the exact dyadic sum `phi+pi`. A materialized
FastTwoSum residual identifies an upward-rounded sum, followed by one downward
`nextafter` only in that case. This also keeps a query just below pi below
two-pi instead of wrapping it to zero. The rule uses no angular tolerance and
does not replace the mirror by a bin-center direction. Scalar representatives
are finite approximations to the underlying planar map and need not form a
bitwise involution when an intermediate angle is unrepresentable.

Negative-angle canonical wrapping uses the same downward representative for
`2*pi+remainder`, so tiny negative angles retain the side below two-pi.
`FromDirection` also retains known quadrant sides if scaling/atan2 rounds a
nonzero component onto the positive-x or negative-x angular axis. An exact
signed-zero component remains on the exact axis. Other Cartesian-to-angle
conversion retains ordinary libm/normalization approximation; no general
exact-arctangent or bitwise direction round-trip guarantee is made.

`EvaluateContinuousNM` returns only F. `CameraPdfNM` returns the complete
mixture's continuous solid-angle sub-density `F*mu_i/(C+D)`, including when
the queried incident direction numerically coincides with the mirror map.
`SampleCameraNM` emits a single continuous or mirror event, carrying weight
`Z=C+D`. Delta-event `pdf` is zero. `eventProbability` is `C/Z` or `D/Z`;
it is descriptive and must not trigger a second compensation in a future SPF.
A zero complete column emits `Event::None`, zero PDF and zero weight.

Spectral nodes must be positive, finite and strictly increasing. Optical coverage is
exactly their closed interval; one node covers only itself. `RequireCoverage`
allows preparation to check a complete requested interval. Coefficients are
interpolated in physical units. The sampler selects a spectral node
using its weighted **column mass**, then samples that node's incoming CDF.
It reports the aggregate interpolated PDF, not the selected-node PDF. Physical
coefficients and masses retain a normalized mantissa and integer binary exponent
through interpolation; equal coefficients return exactly that coefficient.
Both spectral distances are computed independently. The nearer endpoint is
the primary node. One positive binary64 minority coordinate `w` is computed as
the smaller distance divided by the sum of the two computed distances, using
scaled arithmetic. The physical reconstruction is **exactly**
`primary + w*(secondary-primary)`; its complementary weight is the exact real
`1-w`, not an independently rounded second weight. This defines a represented
wavelength coordinate approximating the ideal real wavelength fraction; it
does not claim exact rational division of the original wavelength inputs.
All continuous coefficients, atom coefficients, column masses and node
proposals use this same coordinate choice. Normalized FMA evaluates the convex
form; when common scaling would leave a subnormal result, the fallback retains
the primary complement via `fma(-w,primaryMant,primaryMant)` and the minority
product at their separate scales. Rounded query values are numerical
approximations to this one physical law, not a new set of tabulated coefficients.
Adding
two representations of the same coefficient does not invoke the disjoint-CDF
support-loss check. Adjacent physical columns proved exactly proportional use
one nodal angular law, avoiding an irrelevant tiny latent spectral mixture.
Proportionality is checked by exact binary cross-products (portable two-word
integer products), with no tolerance or overflowing floating cross-products.
Normalized nodal PDFs are never blended using wavelength weights alone.
This NM-only API supplies no HWSS shared-lane proposal: independent-NM fallback
or an independently proved support-preserving all-lane proposal remains required.

All five random inputs are explicit. Event, spectral-node and CDF draws use `[0,1)`;
the two within-cell projected draws use `(0,1)`. This is a numeric-core input
contract, not a claim that RISE's `Get1D` supplies open variates. A future
adapter must specify and test a reproducible open-variate mapping (for example
an integer midpoint construction with a declared finite RNG grid). It must not
silently pass a zero from `[0,1)`. The open polar draw avoids emitting an exact
grazing direction with zero PDF. The azimuth draw uses the same convention for
consistent interior ownership; a zero azimuth endpoint would otherwise be
geometrically valid. Floating-point interpolation can still round an open draw
onto a cell edge. `nextafter` then returns an interior representable point of
that **selected** cell, preserving sample/query cell agreement. Construction
requires representable interiors. These finite-precision endpoint choices do
not claim a continuous RNG or bitwise rotation invariance. Event and node draws
are independent: a rare event's variate is never remapped into the conditional
node selection. Rare-component-first Bernoulli selection avoids computing a
near-one threshold that would round away the rare component.

The caller must explicitly supply `LocalResponseSamplingGrid{precisionBits}`
for its real selector grid `k/2^bits` (1..53 bits); e.g. Sobol32 requires 32,
not an assumed 53. Draw range is validated; their claimed grid/distribution
and independence remain caller obligations. The projected draws separately
need the declared open mapping. `RequireSamplingSupport(wo,nm,grid)` checks all
active event, node and selected-nodal-CDF intervals. Nodal minimum positive CDF
intervals are precomputed, so this check does not scan a column at query time.
Every nontrivial interval must be at least `2*2^-bits + 2^-52`: two RNG lattice
steps plus a `2^-53` normalized binary64 multiplication-error allowance at
each end. Removing those allowances leaves two lattice steps, hence an
interior lattice point survives finite-product rounding. This conservative
support bound does not assert exact continuous probabilities on a finite grid;
ordinary RNG quantization error remains. Unresolvable intervals cause an
explicit `domain_error`; physical F and proposal probabilities are never
floored. A single deterministic branch needs no selector interval.

The column CDF is dimensionless. Construction finds the maximum **cell mass**
`F_ij*A_i` in normalized mantissa/exponent form, then accumulates every cell
mass in units of that power-of-two scale. CDF total and nonzero supported
intervals therefore use normal arithmetic even when all physical coefficients,
all active cell areas, or their products are subnormal. The total's exponent
is retained separately. Projected areas themselves are formed and retained in
the same scaled representation; rounding `dt*dphi/2` to a subnormal scalar is
not allowed to alter a sampling ratio. The finite-grid multiplication-error
bound above applies to these normalized CDF values, not raw subnormal masses.
Event/node probabilities and aggregate `F*mu/Z` use normalized ratios before
physical scale is materialized. This removes percent-level proposal bias from
early subnormal product rounding; it does not ban subnormal response inputs.

Public physical F, row/column masses, atom masses and sample weight still have
the declared `double` type and round once at scalar output. For example, a
uniform `F=16*denorm_min` has exact represented column `16*pi*denorm_min`, while
the returned physical mass/weight rounds to `50*denorm_min`. Its density is
nevertheless `mu/pi`, using the unrounded normalized ratio. At this extreme
scale, forming `F*mu/pdf` using raw subnormal intermediate products is not a
valid precision oracle; normalized units recover `16*pi` before the final
physical-output rounding. That last half-ULP quantization can have noticeable
relative error and limits a consumer of the scalar weight. No exact-unbiased
subnormal scalar-output claim is made. A truly positive physical output below
the scalar range raises `overflow_error` rather than silently returning zero,
even if the corresponding normalized PDF is representable.

`Covers`/`RequireCoverage` describe optical interpolation coverage only. They
do not prove all wavelengths and outgoing directions can be sampled on a given
finite grid. `RequireSamplingSupport` validates **one query**; a later renderer
preparation layer must establish closure over its complete possible query
domain and refuse unsupported domains before shading. In particular, a
nonredundant angular support that appears at a spectral node can become too
small for a finite proposal grid immediately next to that node, while physical
evaluation remains valid there. This core does not solve whole-domain closure
or supply a lower-bounded support-preserving alternative proposal. This
discrete-interval preflight also does not prove scalar output representability
for all eventual query/random-draw combinations; preparation must close both
requirements before using the core in a renderer.

Construction checks extents, full grid coverage, finite/nonnegative values,
positive representable projected areas, and every nodal row's continuous-plus-
mirror mass. The nodal row test forms the **exact dyadic products** of supplied
binary64 F and the retained scaled cell-area mantissas/exponents, adds mirror
mass, and compares the exact sum with one. A portable bounded integer
accumulator retains every product bit; it uses 54 uint64 limbs (432 bytes plus
flag/padding) on the stack. It rejects overfull rows without tolerance, clipping,
or renormalization, even if the excess is a single ultra-tiny positive term or
the rounded scalar total would equal one. A mirror of one plus any positive
continuous contribution is rejected. For example, the usual binary64 `1/pi`
is slightly overfull against the stored binary64 pi measure, although a
rounded multiply returns one; the exact gate refuses it. No coefficient is
silently adjusted to make it pass.

The represented area is the normalized binary64 mantissa/exponent result of
`dt*dphi/2`; its normal mantissa multiplication has ordinary binary64 rounding.
The exact row gate is exact **for those represented areas**, not a proof about
unrounded real grid products. Area representation error still belongs in a
future asset's declared measure/encoding budget. The split-invariance tests use
`dt=1` and phi widths that telescope to the stored pi, where the areas and their
total are exact dyadic scalings and independent rational references apply.

`ForwardRowMassNM` accumulates the two nodal totals exactly and applies the
same exact dyadic-minority/implicit-complement convex blend using bounded
integer arithmetic, then rounds the physical scalar once to nearest/even.
It does not sum independently rounded pointwise query values. Exact passive
nodal rows therefore remain a passive physical spectral law, including
nonproportional angular shapes whose separate scalar query approximations
could sum slightly above one. The diagnostic scans outgoing cells and uses
a few fixed limb buffers; shading evaluation/PDF/sampling do not scan a row.
These exact checks are not a proof that a noisy MC estimate is physically passive.
A future MC bake needs independent uncertainty, unfinished-history accounting,
atom lineage, and encoding-error policies. Very ill-scaled positive normalized
CDF intervals that underflow or disappear in cumulative addition are rejected.
Query interpolation and PDF arithmetic can also throw `overflow_error` for
unrepresentable combinations; preparation must constrain future production
assets accordingly. No silent loss of positive support is intentional.

`EstimateStorage` performs checked integer addition/multiplication and enforces
configurable dimension, coefficient-cardinality and storage limits before the
response makes owned allocations. Its byte count includes `sizeof(object)`;
logical double payloads for all four edge arrays, wavelength nodes, full F,
full wavelength/outgoing/incident-prefix CDF (same cardinality as F), all mirror
coefficients and a minimum-positive CDF probability per wavelength/outgoing
column; `sizeof(Scaled)` for every incident/outgoing area (including struct
padding); one integer exponent per wavelength/outgoing column; and one
proportionality byte per adjacent-wavelength/outgoing column. Defaults cap each grid at
65,536 cells, wavelengths at 4,096, F at 33,554,432 entries, and logical owned
storage at 1 GiB. The count excludes caller-owned inputs, allocator overhead,
the shared-pointer control block and transient stack buffers. There are no file, aggregate-render or
peak-process budget claims in this core. A future loader must account for its
source buffers, metadata, ownership overhead and aggregate residency separately.

Queries allocate no buffers. Evaluation/PDF/sample calls use wavelength and
angular-grid binary searches; sampling additionally uses one nodal column CDF
binary search. `ForwardRowMassNM` is a diagnostic that traverses every outgoing
cell. No throughput or compression claim is made. Published instances are
shared as `shared_ptr<const ...>` and retain private copies, so mutation of
the definition or a future source path cannot mutate an existing instance.

The focused executable gates nonsymmetric unequal-area matrices, projected
transpose power, columns above one, independent solid-angle PDF integration,
per-cell inverse-CDF selection, Lambert/mirror/mixed analytic limits, actual
mirror directions and inverse-map coefficients, wavelength-dependent support,
column-weighted spectral node selection, independent rare-times-rare selector
support, constant/proportional near-endpoint spectral laws and a one-ulp shape
change, explicit finite-grid support failures, tiny positive quantities using
normalized relative assertions, mixed subnormal Cartesian normalization,
subnormal-amplitude and subnormal-area CDF frequencies, independent normalized
PDF integrals, spectral/mirror probability normalization before physical
quantization, exact-rational near-unit passivity fixtures, exact unit rows and
65,536-cell split invariance, mirror-plus-denorm rejection, exact row rounding
across normal/subnormal exponent ranges, convex reconstruction of passive
nonproportional spectral rows, a stratified transpose expectation,
independent dyadic mirror-seam fixtures, below/at/above arbitrary angular
edges with unequal atom coefficients, and Cartesian quadrant-wrap controls,
empty columns, near-grazing Jacobians, half-open edges, finite endpoint draws,
source ownership, invalid values/extents, passivity, storage caps and integer
overflow. It deliberately does not claim forward-oracle statistical validation,
angular refinement convergence, a codec, biological defaults, visible guides,
renderer MIS/light acceptance or release readiness. The numeric core is
registered in the five authoritative Library build projects with ordinary
production floating-point flags; the standalone test is discovered by the
make and Windows CMake test rules. Its two-second pool schedule is an ordering
estimate, not a measured resource profile.

Build and run the repository test:

```sh
make -C build/make/rise build-test/HyLIoSLocalResponseTest
./bin/tests/HyLIoSLocalResponseTest
```

Windows CMake discovers the same standalone test automatically; use the
repository's canonical test configuration and build commands. The implementation
includes `pch.h` first for Visual Studio PCH hygiene. No special per-file
floating-point override is attached to this core.

Before promotion, focused standalone gates passed strict warning-as-error,
ASan/UBSan and production `-O3 -ffast-math -fno-finite-math-only -flto` builds.
Those gates validate this numeric core and its synthetic controls; they do not
establish calibrated assets, a renderer adapter, whole-domain sampling closure,
or cross-platform release readiness. Independent numeric/API reviews covered
the promoted behavior; the integration still requires the repository's build
and warning gates.
