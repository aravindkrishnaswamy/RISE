# HYLIOS incident directions and joint exit measurements

Measurement schema `hylios-forward-measure-v5` adds a fixed incident direction and
weighted joint exit-cell masses. These are forward diagnostic measurements over
all incident histories. They are not a local bake, finite PDF, BSDF, microscopic
adjoint or classification of angular atoms.

## Directions and launch convention

`--incident-polar-deg` defaults to zero and accepts finite theta in [0,90).
`--incident-azimuth-deg` defaults to zero and accepts any finite value, wrapped
modulo 360 into [0,360). The angles describe the **outward source direction** wi,
not the ray's incoming travel direction. For outward macro normal n=(0,0,-1):

- wi=(sin(theta)cos(phi), sin(theta)sin(phi), -cos(theta));
- incoming travel is -wi, launched at local origin;
- both resolved unit vectors and the nominal source angles are recorded.

Azimuth is measured in the fixed global x/y plane, from +x toward +y.
Consequently +x cross +y is +z, the inward normal; this is not an outward-normal
right-handed spherical frame. Consumers must honor the recorded convention.
A smooth same-exterior mirror maps source azimuth phi to outgoing phi+180 degrees.
The polar angle of wi about n equals the launch polar angle about inward +z.

At theta=0, azimuth is undefined and is recorded canonically as zero; the exact
old launch (0,0,1) is retained without trigonometric reconstruction. Explicit
zero polar angle and any finite azimuth preserve default physical histories and
RNG consumption. Every history in one measurement uses the same fixed source
direction: there is no incident-cell jitter or cell averaging.

## Fixed grid and complete radial support

`exit_distribution.version` is `hylios_exit_radius_azimuth_cosine_cells_v1`.
The bounded grid has 8 radial, 8 outgoing-azimuth and 8 outgoing-cosine bins:
512 joint cells. Array order is radius, azimuth, cosine, with cosine varying
fastest. The exact binary64 edges used for classification are emitted in JSON.

| Coordinate | Edges and convention |
|---|---|
| Lateral radius r in metres | [0,.0001,.0005,.001,.002,.005,.01,.02], plus overflow [.02,+infinity) |
| Outgoing azimuth phi in radians | 0 through 2pi in eight equal nominal intervals; from atan2(outgoing.y,outgoing.x) |
| Outgoing cosine mu | 0 through 1 in eight intervals of .125; outward normal is -z |

Intervals are lower-inclusive and upper-exclusive, except cosine one belongs to
the final cosine bin. Outgoing cosine is -direction.z/hypot(direction), removing
only stored unit-length rounding. Exact poles with x=y=0 use canonical azimuth
zero; near-pole directions receive no angular tolerance. A negative azimuth
approaching the periodic seam remains in the final bin even if adding 2pi rounds
to the upper endpoint.

Radius is hypot(exit.x,exit.y) relative to the launch point. There is no detector
crop or renormalization to the finite radial window. Overflow has no finite
radial width or annular area. Finite coordinates whose norm exceeds binary64's
representable radius also belong to overflow; their weight is retained and
`unrepresentable_radius_history_count` records this case. Nonfinite coordinates
or a non-exterior direction are explicit diagnostics, not cropped observations.

Each finite joint cell records its solid-angle measure
Delta(mu)*Delta(phi), projected-angle measure
.5*(mu1-mu0)*(mu1+mu0)*Delta(phi), radial width, annular area
pi*(r1-r0)*(r1+r0), and annular-area times solid-angle measure. Exit-position
azimuth has already been integrated out: the joint experiment observes radial
annulus and outgoing angle. These measures describe cells only. Dividing a cell
mass by any of them does not establish continuity or recover an atom's PDF.

## Weighted cell masses and all-history uncertainty

There is one observation per terminal escape. `all_returns`,
`external_reflection` and `subsurface_return` are separate channels. External and
subsurface name transport outcomes; they are not atom/continuous labels. A
positive-weight escape updates its combined cell and exactly one outcome cell.
No internal interface crossing contributes an additional escape.

For bin b and incident history h, X_h,b is escaped terminal weight if that history
belongs to b, and zero otherwise. Every cell reports:

- `sum_weight`: sum of X_h,b;
- `mean`: sum/N, returned incident power in that cell;
- `standard_error`: sqrt(M2/[N*(N-1)]), or null for N=1;
- `history_count`: the original incident N;
- `nonzero_history_count`: the number of positive bin contributions.

Absorbed, roulette-killed, unresolved and unstarted-cancelled histories contribute
zeros to every escaped-power bin. No cell or marginal is conditioned on escape,
completed histories or the finite radial window. Weighted/RR observations can
exceed one and are neither clamped nor renormalized.

The implementation keeps nonzero Welford moments only. If their count, mean and
centered second-moment sum are k, m and M2nz, missing zeros are merged at publish:

`mean_all=m*k/N`

`M2_all=M2nz+m*m*k*(N-k)/N`.

Means/sums use a running maximum-weight scale. Nonzero variance has a separate
first-observation origin and centered mean offset, normalized by observed range.
Positive mean, centered mean offset and normalized M2 retain compensated hi/lo
residuals, so many small updates do not stall below their high part’s ulp. The
rounded absolute mean never feeds variance. In exact arithmetic, a
nonconstant observation set contains both range endpoints, so its normalized
nonzero M2 is at least 1/2; a large common weight offset cannot bury that spread. Parallel merges express both
groups relative to one origin/range rather than subtracting rounded absolute
means. Positive means use each group’s own nonnegative count fraction rather than
a subtraction-form blend that loses a rare large contribution. Volatile binary64 materialization protects those centered operations from
fast-math reassociation without suppressing warnings. Squared physical weights
need not fit binary64, and tiny squared weights need not underflow before forming
a representable SE. Parallel Welford merges combine disjoint joint
observations for angular and radial marginals; their means and SE describe the
same returned population, rather than a sum of independent-bin SEs. The scalar
power/layer summaries use the same scaled accumulator, removing the old sibling
that could silently report zero uncertainty for tiny positive contributions.

`nonzero_cell_updates` is a cost counter: two joint-cell updates per positive
terminal escape, independent of bins and zero histories. Publication visits the
fixed grid to emit cells and merge marginals. It never iterates over every bin for
every incident history. True unrepresentable published sums, means or errors
produce an explicit range diagnostic before atomic output replacement; they are
not converted to zero or absorption. Compensated accumulation is not an exact-real
certificate for arbitrary-length histories. Empirical SE describes the realised escaped
contribution. It does not resolve future escaped power hidden in unresolved
histories, whose live-weight accounting remains separate.

## Analytic controls and direct arithmetic gates

`HyLIoSMeasurementStatisticsTest` includes the real CLI translation unit with its
entry point renamed. It exercises private production accumulators without a
public test hook or a copied implementation. An independent dense Welford loop
consumes 20,000 varied observations, including interleaved zeros. Separate exact
identities cover N=10^18, all-zero populations, singleton null SE, disjoint-stream
merges, 1e-300/1e150 scale changes, representable mean/SE despite an unrepresentable
unscaled variance, explicit output-range failure, count-overflow preservation,
adjacent-value pair/triple/quartet exact rational variance at powers from -900
through 900, cross-group centered merges, rare-high/tiny-positive groups through
N=10^18, 100000 sequential low-positive appends versus batch and exact two-level
mean/SE identities, recorded-edge ties, the periodic seam,
radius representation overflow and complete
weighted joint/marginal power. The scalar tiny-SE regression was observed RED
before sharing the scaled accumulator and GREEN afterward. The adjacent-value
variance regression was separately observed RED before the origin/range-centered
variance state, then GREEN without loosening its relative tolerance.

CLI tests replay deterministic one-history measurements and independently build
dense all-history bin moments, including weighted/RR observations above one.
They also check exact default-normal compatibility, source/travel vector signs,
oblique mirror-cell mapping, source-angle atomic validation, canceled zeros,
correct measures and uncropped grazing overflow.

The equal-index, transparent slab with cosine diffuse bottom has an independent
joint oracle at normal incidence. For total slab depth H,

`p(mu)=2*mu`, `r=H*sqrt(1-mu*mu)/mu`, uniform phi.

A radial interval [r0,r1) restricts cosine to
(H/hypot(H,r1),H/hypot(H,r0)], with the lower cosine zero for radial overflow.
Intersect that interval with the recorded cosine cell; if the resulting bounds
are a<b, its joint mass is (b-a)*(b+a)*Delta(phi)/(2pi). The overflow mass is
H²/(H²+Rmax²). The CLI tests this full joint identity, not only its angular
marginal. Under oblique launch the same baseline's bottom angular law is unchanged;
a separate independent-seed quarter-turn test checks joint rotation covariance.
These limit controls do not establish rotation reduction for every selected
particle/complex/fiber/boundary ensemble.

## Combined histograms and separate returned-law summary

The response can contain angular atoms or other singular components. Random
geometry alone does not prove continuity; matched-index transmission and exact
zero-width kicks can preserve deterministic directions. Terminal outcome names,
rounded direction equality and proposal counts are not a classification.

`exit_distribution.angular_measure_classification` describes the combined
per-cell mixture, and the histogram retains `is_density=false`,
`continuous_pdf_available=false`, and `production_bake_available=false`. Version 6
adds a separate whole-return `returned_angular_law` summary, with mirror, proved
continuous and unclassified mass estimates. See
[the lineage contract](HYLIOS_ANGULAR_LINEAGE.md) and
[measurement version 6](HYLIOS_MEASUREMENT.md#version-6-returned-angular-laws).
The per-cell histogram does not split these laws and cannot be divided by its
cell measure to create a finite BSDF. No response interpolation or bake file is
implemented. A future local response still requires explicit incoming/outgoing
projected measures, a production key, passivity checks and refusal of positive
unclassified returned mass.
