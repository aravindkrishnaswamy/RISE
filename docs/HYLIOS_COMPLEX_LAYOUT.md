# HYLIOS explicit complex layout

This document specifies the selected `hylios_complex_explicit_pack_v1` construction. The helper and forward integration are implemented; independent review gates determine readiness. A 64-child resource limit bounds allocation and per-proposal pair checks; larger requests fail before allocation, without truncating a physical history.

## Selected construction contract

This construction implements the selected explicit spatial alternative, not the original
HyLIoS authors' undisclosed internal placement code. It deliberately changes the
paper/thesis “at most n_m interactions” interpretation: the resolved composition
integer is N actual children; geometric revisits have no count cap. The inspected
paper §4.2.1 and thesis printed p.28 do not specify the layout/occupancy law.

## Layout law: `hylios_complex_explicit_pack_v1`

Each accepted outer sphere receives a fresh layout, generated without consulting
its incoming ray. Its R is the resolved complex radius, and each child's a,b are
the resolved layer-scaled spheroid semi-axes. The compound object has no extra
optical interface at its envelope. No unspecified filling fraction is inferred.

Construction is ordered sequential adsorption with whole-layout restart. A single
proposal consumes exactly five independent U[0,1) values: three Cartesian centre
coordinates in the cube [-d,d]^3, then isotropic-axis cosine and azimuth. Here d is
the greatest binary64 <= exact R-b. The exact certified sphere control retains
only centres |c|<=d. Thus its continuous idealization is uniform-ball centres,
with isotropic child-axis prior. This conservative bounding-sphere containment is
stricter than actual spheroid-in-sphere containment, and is part of the rule.
It is unrelated to the incoming ray and has no additional Eq.(5) orientation draw
at a hit. Actual geometric interception induces the encountered orientation law.

Every existing accepted child is checked with the certified ellipsoid predicate.
Strict overlap rejects; Disjoint/Touching accept. Indeterminate, InvalidInput and
UnsupportedRange are numerical failures preserving live transport power, never
rejection or retry. A child gets 256 proposals, counting containment and overlap
rejects. Acceptance resets that child's proposal count. The 256th failed proposal
clears all previously accepted children and increments whole-layout restarts.
Every Propose call is resumable visible work; neither rejection nor restart is
absorption or physical travel. There is no hidden retry or overall construction
cap. Caller budget exhaustion leaves unresolved power. No guarantee of completion
exists for arbitrary inputs; a necessary exact N*a²*b<=R³ check is not sufficient
for packing, especially in this conservative centre support. Degenerate d=0
supports N<=1 only. N=0 is retained as the empty-envelope analytic control.

The successful complete-layout distribution is this ordered/restart law. It is
not asserted uniform over all hard-spheroid configurations or an equilibrium
hard-particle ensemble. Individual accepted axes can correlate through exclusion.
Store the version, centre restriction, 256 threshold, RNG mapping, actual count,
R,a,b and counters in provenance. A later broad-support layout is a new revision.

## Forward transport integration

Keep an envelope-local point, full accepted child array, active child index or
host gap, and existing child-reflection perturbation state. At each host gap,
compare true nearest entering child intersection with sphere exit. Scan every
child (O(N) tests per step); there is no spatial acceleration claim. Skip a
child's zero exit root after an outward transmission, using side/normal state,
not a ray epsilon. Tangency owns zero path length and does not enter a child.
Tie handling must preserve child chords at boundary contact: finish an owned
child exit before a coincident envelope exit; in host, process only strictly
entering child boundaries before the outward envelope boundary. The initial sampled envelope chord remains owned across construction yields;
it supplies the first host segment, including N=0. Exact centred sphere identity
(R==a==b, centre zero) gives a zero-distance child entry and a zero-distance
envelope exit after child transmission. No tolerance, surface resampling or
origin displacement is used. Other robustly unresolved geometric signs/roots are
numerical failure, not a miss.

Host gaps own only host Beer absorption. Child interior owns only melanin Beer
absorption. Never apply host absorption again over a child chord or apply the
whole envelope chord as melanin. Preserve the current selected single-spheroid
interface rule: entry introduces no extra Fresnel; absorption precedes exit
Fresnel, reflected rays undergo the existing conditional inward cosine law and
trace another real child chord. After transmission, retain all children and
resume host-gap tracing; revisits of any child remain allowed. The final
exponential angular law occurs exactly once for a nonempty complex, at successful envelope exit, around
the current transmitted/host direction. No per-child final exponential kicks.
An empty complex is host-only with no final angular kick at any wavelength.
Envelope has no Fresnel. Host gap clocks can be independently sampled per segment
because exponential absorption is memoryless; weighted paths tally exp(-mu L).
Bulk outer-complex placement clocks are suspended inside the complex.

Keep the envelope, not the last child, as the outer predecessor exclusion shape.
After the once-only final kick, the discard-after-scatter rule removes the whole
virtual layout even if the direction points into its former envelope. Subsequent
travel is host; only the predecessor copy remains for exclusion of later outer
particles. Numerical/caller-budget failure retains live weight and distinguishes
host versus child absorption already tallied. Construction counters may advance
without travel; path counters cannot. Accepted-complex count increments once,
child entries/exits/reflections and internal gap work need separate counters.

## Independent gates

Helper test passes exact empty/single-sphere chords; two known separated spheres
have one unit total child chord and 1.5 units host gap in a radius-1.25 envelope,
so matched-index survival must be exp(-1.5*mu_host-1*mu_child). The helper test computes independent chord/Beer controls.
`tests/HyLIoSComplexForwardTransportTest.cpp` additionally runs empty/one/two-child
controls through the real kernel, checks analog/weighted/RR power agreement,
compares nearest boundaries against an independent sphere quadratic, forces >N
entries through a child revisit, handles child/envelope and child/child contact,
and verifies host absorption after geometry discard. The CLI tests cover explicit
model selection, atomic malformed-input refusal, construction budgets, physical
child/counter publication and the visible empty-complex host limit.

Packing tests certify every pair, check conservative containment independently,
and test both rotational covariance and the separate one-child uniform-ball
moment E[x²]=d²/5 (rotation tests alone would fail to distinguish a cube/shell).
Numerical-failure tests ensure unsupported axis components remain sticky and do
not turn into a different accepted layout. Restart fixture gives 256 identical
overlap rejections after an accepted child and checks all counters/cleared state.

Measured 3000 complete N=5 layouts per actual S1/S2/S3 layer scale with seed
0x48794c496f53, a=q*.17um/2, b=q*.41um/2, R=2b:

| q | mean proposals | max observed | total restarts | mean pair checks |
|---|---:|---:|---:|---:|
| .25 | 854.570667 | 7149 | 6005 | 679.245667 |
| .50 | 870.083000 | 6405 | 6167 | 690.671000 |
| 1.0 | 836.870333 | 6312 | 5835 | 665.315000 |

Every trial completed below the explicit test-only 100000-proposal budget. These
are sample costs, not a bound or performance promise. The common dimensionless
child volume/envelope volume is 5*(.17/.41)^2/8 = about .10745. Despite that low
overall occupancy, conservative centre support causes substantial restarts.

## Adjoint implications

Stationary geometry within one encounter makes path segments definite, but does
not establish reciprocity of the whole process. The no-entry/exit-only Fresnel,
conditional reflection kick, final angular law, temporary-geometry deletion,
predecessor memory and outer placement rule still define a forward-only kernel.
Ordered layout/restart proposal measures and all variable RNG/rejection branches
must be included in any augmented-state reversal. Do not expose a camera PT/BSDF
sample/PDF or reuse the forward kernel as its adjoint. The eventual gate needs
paired source/detector measurements on the same layout/ensemble, index/area/solid
angle factors, and either a proved adjoint or explicit forward-only use.

## Standalone validation record (2026-11-26)

Strict warning gate: Apple clang 21.0.0, `-std=c++17 -O2 -Wall -Wextra
-Werror -fno-fast-math -ffp-contract=off`, arm64 Apple M1 Max. Test build
links the actual ParticleOverlap and ParticleGeometry TUs. Completed 7,867,186
assertions. `/usr/bin/time -p` reports 26.23s real, 25.89s user, .05s system for
9000 N5 complete layouts, 20000 one-child uniform-ball controls, pair
recertification and all assertions. Dividing 9000 by total wall gives 343.12 N5
layouts/second INCLUDING verification overhead, not transport histories/second.
The test is single-threaded; a separate sanitizer process and other workspace
work ran concurrently, so this is a load-dependent baseline, not an isolated
machine throughput benchmark. No renderer was launched. The library implementation uses the Windows PCH convention and requires strict-FP source settings in all build projects.

The same complete test passes ASan+UBSan at `-O1 -g` with 7,867,186 assertions
and no sanitizer diagnostic. These initial standalone results precede the integrated transport gate and independent review; they do not establish full-model correctness.


## Measurement adapter and standalone checks

Active resolved `complexes` require `--complex-layout explicit-pack-v1` in
`hylios_measure`. `complex_diameter_m/2` is the explicit envelope radius;
`maximum_internal_count` is reinterpreted as actual children under the separately
reported `resolved_maximum_internal_count_as_actual_children_v1` convention.
A declared `encounter_rate_m_inv` must match the outer spherical `3*f/(4*R)` rate.
This input convention is independent of the child axes and does not infer a
packing fraction. PD complexes with nonzero fiber rate are currently refused.

Standalone forward and CLI builds must link `HyLIoSComplexLayout.cpp` alongside
geometry, overlap, optics and slab sources. In mixed full LTO, compile layout,
geometry and overlap separately with `-fno-fast-math -ffp-contract=off`; the driver
and transport can use `-ffast-math -fno-finite-math-only`. Exact source commands
and CLI controls are in [HYLIOS_FORWARD_TRANSPORT.md](HYLIOS_FORWARD_TRANSPORT.md)
and [HYLIOS_MEASUREMENT.md](HYLIOS_MEASUREMENT.md). These checks do not rely on a
shared archive being rebuilt or relinked concurrently.
