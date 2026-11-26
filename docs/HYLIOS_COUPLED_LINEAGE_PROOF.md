# Coupled HyLIoS angular lineage: certified subsets and remaining limits

Reviewed proof underlying the v2 implementation, developed against v1 lineage
commit `cad47822a` and source HEAD `ceec3e397`, 2026-11-26. Source-linked
implementation gates are described in [angular lineage](HYLIOS_ANGULAR_LINEAGE.md).
This is a mathematical ensemble law conditional on the incident direction, with
fresh model uniforms interpreted as independent uniforms. It is not a distribution
conditional on every realized RNG value. The implementation uses the sufficient
certificates below; independent proof and implementation reviews cover their
stated domains, while the unsupported residual remains explicit.

## Result

There is a rigorous route that closes **all retained individual-particle curved
maps reached after the first accepted reflected-cosine event**. It also closes
arbitrary finite complex child chains, including revisits, whenever a specified
full boundary phase-space density has first been established. This includes a
complex entered with a proved angular-AC direction and a complex whose first
optical child event is reflection followed by accepted cosine. It introduces no additional unknown class for a
zero/one-child complex entered with a classified source. It does **not** yet certify every multiple-child complex
entered with an atom and undergoing curved transmissions before acquiring joint
phase support. Those remaining returns stay unclassified; this proposal does not
make the entire SWIR model bake-complete.

The key invariant is joint density in boundary position (two dimensions) and
travel direction (two dimensions), in a body-relative or fixed-layout frame.
Angular density alone is insufficient. Conditioning on an impact-dependent world
centre would incorrectly remove the position randomness used below.

## 1. Actual kernel facts and scope

- Individual acceptance samples orientation, then two independent projected-area
  variates. The free-flight endpoint is the entry point; the body's world centre
  is that endpoint minus the sampled relative entry point.
- A child entry adds **no Fresnel/refraction**. Its exit uses particle-to-layer-host
  Snell/Fresnel. Reflection is followed by a separately accepted inward cosine
  proposal, at the same boundary point, before the next internal chord.
- Complex layout uses only its dimensions, count, and fresh layout samples. Centre
  proposals are global Cartesian coordinates in a cube followed by a ball test;
  axes use global `z=1-2*u3, phi=2*pi*u4`. They are not rotated by ray direction.
  All overlap/restart tests use layout geometry only. There is no Eq5 child-axis
  resampling. The child count is not an interaction cap.
- The envelope is a geometric boundary, with **no Snell event**. Empty complexes
  have no final angular kick. A nonempty complex applies the final exponential
  once at envelope exit, and that law is exactly identity at >=1400 nm.
- Shapes are finite strictly convex ellipsoids with positive finite semi-axes.
  Complex children do not overlap. Layer coefficients are constant throughout a
  contained body. Inside a body the optical walk has no dependence on its world
  translation or predecessor geometry. PD complex/fibre combinations are refused.

Relevant source seams: `Placement`, `BuildComplex`, `ComplexHost`, `ParticleFlight`,
`Perturb`, `SurfaceExit` in `HyLIoSForwardTransport.cpp`; layout `Propose` in
`HyLIoSComplexLayout.cpp`; projected sampling in `HyLIoSParticleGeometry.cpp`.

## 2. Measure lemmas used, with conditioning kept explicit

### 2.1 Reweighting and mixtures

If a finite measure is absolutely continuous with respect to a reference measure,
restriction to any measurable accepted event and multiplication by any finite
nonnegative integrable weight preserve absolute continuity. This covers containment,
predecessor exclusion, Fresnel choice, Beer survival, roulette, rejection counts,
work/cancel selection and nearest-child topology selection. Countable mixtures
preserve AC. No claim requires a positive lower bound on a density or survival
probability. Histories with zero accepted probability do not generate returned mass.

These facts do not justify conditioning on a null event such as one exact
realized surface point. Such conditioning is used only as a kernel construction
whose resulting conditional densities are then integrated, not as a claim that the
original source remains AC after its complete realization is fixed.

### 2.2 Independent projected impact supplies missing dimensions

Let H be the complete pre-placement history, D its travel direction, and B the
fresh two-dimensional unit-disk projected coordinate. Conditional on H, B has its
stated area density and is independent of H. Thus if the *marginal* law of D at a
named discrete event/layer family is AC, `(D,B)` has a product-area density in
`dOmega_D d^2B`, even if world position, absorption weight or predecessor geometry
is strongly correlated with D. One does **not** condition on H to claim D is AC.
Instead integrate H conditional on D; acceptance events only reweight `(D,B)`.
Finite attempts and finite event-word mixtures retain this conclusion.

For a fixed orientation, `(D,B)` maps smoothly and locally invertibly to
`(relative entry point,D)` (or `(relative exit point,D)`) on the strictly entering
(or exiting) boundary phase space. The inverse is direction plus orthogonal
projection of the surface point onto the plane normal to D. Its surface-to-impact
Jacobian is `abs(n dot D)>0`; the silhouette is null.

Individual orientation does not deterministically reveal D. Write r=a/b>0.
The actual Eq5 sampler has signed axis-cosine density

`f(c|D) = (1-(1-r)*abs(c))/(1+r), -1<c<1`,

and independent uniform azimuth. This follows by inverting the source formula:
`m = [2*y-(1-r)*y^2]/(1+r)` for y=abs(c), m uniform on [0,1]. Consequently its
absolute axis direction has conditional density `f(D dot A)/(2*pi)` with respect
to axis solid angle. If D was AC, the joint `(D,A)` law has a density and, for
axis-area-almost every A, conditional D remains AC. Sphere axes are irrelevant;
conditioning on that auxiliary axis is unnecessary. The retained-individual proof
below needs less: fix the pre-placement D and A first, then use fresh B.

### 2.3 Complex layout does not reveal the incident direction

For fixed material/layer parameters, the complete mathematical layout L is a
function only of a fresh independent layout stream. This stream is independent
of `(H,D,B)`. Therefore the product-density result for `(D,B)` survives disintegration
conditional on L for layout-law-almost every L. This statement remains valid when
`R==b`: the centre may be deterministically zero, but it is still independent of D.

RSA rejection/restart is a countable finite-prefix mixture of functions of that
same independent stream. Selection of completion before a remaining work cap,
including a cap correlated with H, only restricts/reweights the product measure;
it cannot make a positive-probability completed layout reveal an exact incident
D. Conditional distributions are asserted almost everywhere in the layout's own
law, which can have deterministic components. No centre-volume-density assumption
is needed for this independence argument. Future iid draws after a finite
nonanticipating completion/rejection stopping time remain fresh.

## 3. Full boundary phase-space density is preserved

Fix the body orientation or the entire complex layout. On a smooth boundary use
surface position q and a strictly non-grazing direction u. The reference phase
measure may be `dA(q) dOmega(u)` or flux measure

`abs(n(q) dot u) dA(q) dOmega(u)`.

They have the same null sets away from grazing. A finite word of strict geometric
hits, transmission/reflection branches and survival decisions is handled locally.

**Geometric flight.** Between two transverse boundary sections, straight flight
is a smooth locally invertible map `(q,u) -> (q',u)`. Its inverse follows the same
ray backward to the previous section on the chosen word. The implicit-function
derivative exists because both endpoint normal cosines are nonzero. Equivalently,
it preserves geometric flux measure. Selecting the nearest child only restricts
the local domain; it does not collapse any dimension.

**Snell transmission.** At fixed q, in tangent direction coordinates, transmitted
components are `t_T=eta*u_T`. For positive eta and noncritical positive transmitted
normal cosine, this is locally invertible. Its solid-angle Jacobian is

`dOmega_t/dOmega_u = eta^2 * c_u/c_t > 0`.

Position remains q, so the full 4D derivative is block triangular and nonsingular.
This is a statement about null sets, **not** a proposal to add an eta^2 power weight.
The implemented physical-power Fresnel/weight law remains unchanged.

**Specular reflection.** At fixed q the angular map is a smooth involution with
unit angular Jacobian; the full boundary phase map is invertible even when an
angular-only or position-only projected submap has a caustic.

**Accepted cosine.** A phase-AC input has an area-AC position marginal. Conditional
on position and old direction, the new accepted cosine direction has a 2D angular
density on its open allowed hemisphere intersection. Integrating the old direction
gives a new joint `(q,u_new)` density. Rejected finite proposals only reweight the
existing phase law. The same conclusion holds whenever position is known area-AC,
without requiring the old direction to have a density.

A composition of these local 4D maps preserves phase AC. Its angular marginal is
AC. Caustics of a projected 2x2 position or angle Jacobian are allowed and do not
invalidate the 4D theorem. A blanket argument from angular AC alone would fail.

Critical/TIR thresholds and grazing sets are codimension-one null sets under a
phase density. TIR reflection on an open domain is covered by the reflection map.
Zero-length tangent chords are not child entries in the actual kernel. Inter-child
touching points have zero surface area; phase-AC histories hit them with probability
zero. Equal centred full-sphere child/envelope boundaries are different: this is
an exact supported identity of sections, and the zero-host-distance step is the
identity phase map, not a limiting chord or an extra optical interface.

Each returned history has a finite optical word. There are countably many finite
words, including revisits; their mixture remains AC. Infinite unresolved histories
are not silently assigned returned density or an atom.

## 4. Retained individuals: complete certified closure

Condition on pre-placement history, incoming D, and chosen body axis A. Fresh B
has area density. On the far boundary of a convex ellipsoid, its map to the first
exit point q is a surface-area local diffeomorphism away from silhouette. This is
the same projected-area/Gauss-map argument used by v1, but the required conclusion
here is **position-area AC**, not only normal/angular AC.

Fresnel reflection and all finite rejected cosine proposals preserve q's area AC.
The first accepted reflected-cosine supplies a fresh angular density conditional
on q and its old axis. The old axis is integrated, not fixed as an additional
conditioning variable that could itself reveal q. Therefore `(q,u_new)` has full 4D boundary density conditional
on D,A and the earlier nuisance history that does not fix B. Crucially, do not
condition on the body's world centre: it was constructed from B and the entry
endpoint. The subsequent internal optical map depends only on relative q,u and
fixed shape/orientation/index coefficients, not that world centre. World
translation and its correlations can therefore be marginalized before applying
section 3.

Every later internal flight, Snell transmission or reflection preserves phase AC;
any subsequent cosine does also. Thus a retained individual's unmatched exit or
intermediate reflection after this acquisition is angular AC at every supported
positive index ratio, including paraxial focusing ratios and prolate shapes.
No generic analytic-Jacobian argument and no independent position/direction
assumption are used: the joint density was constructed explicitly from B and
the accepted cosine draws.

Before the first reflection/cosine, the existing independent projected-normal
proof handles the initial unmatched transmission/reflection. Exact matched-index
transmission and zero-width final kick preserve the existing class; they do not
turn fresh placement geometry into scattering. Individual retained-chord unknown
returns can consequently be removed under this invariant without a law change.

## 5. Certified complex acquisitions and chains

All statements in this section are conditional on the fixed relative layout L;
its impact-dependent world translation is not conditioned on.

### A. Entry with a proved angular-AC direction

The marginal law D is AC. Fresh envelope impact B adds two independent dimensions.
By sections 2.2–2.3, `(D,B)|L` has a density. The map to envelope-entry boundary
phase variables is locally invertible, so full 4D fixed-layout phase AC is already
established at entry, before any child event. Every later finite child chain,
including arbitrary revisits, is covered by section 3. No reset to unknown is
needed at retained curved maps on these histories.

### B. A first-child reflection followed by accepted cosine

Even when incoming D is an atom or unclassified, fix D and L. The initial projected
beam B has two area dimensions. Before a directional child transform, every child
entry/exit is the same straight parallel ray family. Restricting to a particular
first-hit child and surviving its chord restricts the impact domain. Orthogonal
projection from that child's far surface to the incoming transverse plane has
Jacobian `n dot D>0`; hence the first exit position is surface-area AC conditional
on D,L. A reflected branch followed by an accepted cosine makes that position
and direction jointly 4D AC. The entire later layout walk is certified by section 3.

Finite cosine rejections at that first point retain its area-density proof. The
proof becomes phase-AC only on successful acceptance, not on the reflected branch
choice or the number of consumed variates.

### C. First straight-beam child exit; zero/one children

An initial untouched projected beam also gives the v1 first-normal proof at the
first child exit, conditional on L. Projected density need not be uniform on the
child's own disk; it need only be AC. Unmatched first transmission or reflection
therefore has angular AC. Matched transmission is identity.

For N=1, a transmitted outward ray cannot re-enter that convex sole child while
host direction stays unchanged. There is no other child to bend it. Either it
transmits on the first chord, or a first reflection/cosine acquires phase AC before
its retained chord. Thus N=1 introduces **no new unknown** from a classified input, including R=b
equal centred spheres. A miss or matched-index identity preserves any pre-existing
unknown input rather than repairing it. N=0 likewise remains the exact host-only
identity with no final angular kick.
This does not require independent child centres or a positive centre-support radius.

### D. Later cosine events with weaker support

After any accepted cosine at a fixed point p on a convex child, the new directions
have density conditional on that entire past. The map from inward direction to
its far exit point is a surface-area local diffeomorphism (radial projection,
Jacobian `c/L^2>0`). Therefore that next far **position** is area-AC conditional
on the fixed layout, even if the starting p was not area-AC. A reflection there,
followed by another accepted cosine, acquires full phase AC. A preceding first
cosine does not, by itself, establish joint phase AC at an arbitrary fixed starting
point; position2 and direction2 must both be accounted for.

### Still unproved

For an atom-entered multiple-child complex, several unmatched transmissions may
occur before any phase-density acquisition. Conditional on L, the initial beam
has only two dimensions. Optical maps preserve an immersed ray family but a
projection to output angles can in principle collapse rank. After a first child
transmission, later normals depend on the earlier direction/position and layout.
First-visit/revisit counters alone do not prove the needed Jacobian or joint law.

No proof here establishes full angular rank for every such finite word and every
accepted nondegenerate layout family, nor proves that exactly focusing families
have zero probability under all supported RSA parameters. A generic "analytic,
therefore zeros are null" argument would need a nonzero-Jacobian witness on every
connected accepted parameter family/word (including layout degeneracies); that
work is not supplied. Such paths remain unknown unless they later reach one of
the explicit acquisitions above or a fresh independent angular randomizer.

## 6. Additional restrictive direction-map certificates

These are useful fallback certificates, not substitutes for the joint proof.

For a fixed point p, far exit q, outward normal n, L=|q-p|, u=(q-p)/L, c=u·n>0,
eta>0 and ct=sqrt(1-eta^2*(1-c^2))>0, let S be the positive shape operator of the
strictly convex exit surface and v the tangential components of u. The tangential
derivative of transmitted t with respect to surface tangent coordinates is

`B = (eta/L)*(I-v*v^T) + (ct-eta*c)*S`.

The full angular Jacobian q->t is `det(B)/ct`. This follows by differentiating
`t=eta*u+(ct-eta*c)*n`: the derivative of the scalar normal term has no tangential
component. The radial map q->u has Jacobian c/L^2>0.

- If 0<eta<=1, B is positive definite (`ct-eta*c>=0`, the first term positive
  definite). Therefore **any convex-child point-source angular fan** has AC exit
  direction in that index regime, including a last cosine event at a fixed p.
  The actual organelle 1.7 / host ratio can exceed one, so this does not certify
  every biological input. It also does not automatically compose after a prior
  refraction: the resulting rays need not share a fixed source point.
- For a sphere and any fixed finite point p, every positive eta has angular rank
  two almost everywhere on each strict transmitted branch. Choose the axis along
  p. Rotational symmetry writes the outgoing direction as polar delta(psi) and
  azimuth phi, allowing a sign/phi+pi convention away from zeros. Rank can vanish
  identically on an open meridian interval only if delta is constant there.
  Snell's tangential condition then integrates to
  `eta*sqrt(R^2+|p|^2-2*R*|p|*cos(psi)) = C+R*cos(psi-delta)`.
  Squaring produces a nonzero second harmonic R^2*cos(2*(psi-delta))/2 on the
  right, while the left has only constant/first harmonic terms. This identity is
  impossible. Delta and sin(delta) therefore cannot be identically constant/zero;
  analyticity on each noncritical interval makes their exceptional zeros null.
  A sphere cosine-sourced retained chord can be certified even without position
  area AC. This says nothing blanket about a later multi-sphere chain's non-point
  source ray family.

## 7. Genuine focusing and degeneracies: adversarial obligations

A prolate ellipsoid can exactly collimate a two-dimensional angular fan. Let
semi-axes a=3,b=5, focus f=4, source p=(0,0,f), and eta=b/f=5/4. For q on the lower
half of the surface, |q-p|=b-f*cos(psi). Thus `eta*|q-p|=b^2/f-z`, whose tangent
gradient equals that of constant outgoing direction -Z. The physical transmitted
normal cosine is positive on that lower open patch: its returned angular image is
exactly the atom -Z. Fresnel transmission has positive probability there.

This is a genuine degeneracy of a point-source curved refraction map, not a
counterexample to section 3 (that source has no position density). It is also
**not** a demonstrated positive-probability kernel history: a retained point lies
on its own child's surface, not at its interior focus; a point on another disjoint
child is outside this child. An earlier optical chain could create a virtual
source, but this note neither constructs a reachable exact focusing chain with the
model's common child index nor proves all such families null under RSA. In
particular the simple inverse ellipsoid focusing lens requires eta=f/b<1, while
this collimator requires eta=b/f>1; identical per-layer child ratios do not realize
that simple two-element reciprocal pair. The example falsifies an unrestricted
angular-only rule, not the actual model as a whole.

For a retained sphere at eta=2 and a boundary source, the paraxial direction map
has a zero derivative at the axis but is not an atom. At internal polar alpha,
`delta=2*alpha-asin(2*sin(alpha))=-alpha^3+O(alpha^5)`; its solid-angle Jacobian is
`3*alpha^4+O(alpha^6)`, positive off the isolated axis near zero. Very narrow lobes
and small numeric Jacobians cannot be renamed atoms. Matched eta=1 is an exact
identity law, and the outer same-exterior planar mirror remains the only atom map
certified here. Infinite aspect cylinders, zero axes and eta=0 are unsupported
geometries/indices, not limiting cases silently covered by the theorem.

## 8. Implemented sufficient-support metadata

The `hylios_angular_lineage_v2` keeps the public three classes and adds
read-only sufficient proof support, copied exactly with state and independent of RNG/work:

1. `boundaryPhaseAC` with domain `individual-relative` or `fixed-complex-layout`.
   Set on new projected placement if incoming direction is certified angular AC;
   set after cosine only when current boundary position has an explicit area-AC
   certificate. Preserve through all section-3 maps and rejected proposals.
2. `boundaryPositionAreaAC` at the current optical surface: set at an individual
   first projected exit; set at a complex's untouched straight-beam first exit;
   derive from a phase-AC state; or set at the far exit of a cosine point-source
   chord. Clear rather than extrapolate it across an unproved coupled traversal.
3. `cosinePointSourceChord` on accepted reflected-cosine: if position-area AC was
   available, acquire phase AC immediately. Otherwise the current angular law is
   still AC; at the next far exit derive position-area AC and use only a proved
   fixed-point certificate (sphere or eta<=1) for angular refraction. A further
   reflected/cosine event can then acquire phase AC.
4. `untouchedComplexProjectedBeam` at envelope entry: preserve through geometric
   traversal and exact angular identities only; consume after a curved directional
   child event. It is a law/topology certificate, not a comparison of vectors or
   RNG/counter heuristic. Its first exit has the projected-normal and position
   proofs in section 5. The acquisition is valid conditional on fixed layout.
5. With phase AC, retained curved reflection/transmission preserve public AC with
   a `BoundaryPhaseDensityPreserved` reason. Without it, retain v1 unknown unless
   the explicit first-normal or point-source certificates apply. A nonzero final
   exponential, bottom cosine, accepted Rayleigh or nonzero-fold law still refreshes
   angular AC, without automatically granting arbitrary existing-layout phase AC.
6. Discarding a body clears its private frame-specific proof support; public angular
   AC survives. Starting a fresh projected complex/body can then establish new
   phase support by the product-density argument. Exact matched indices and zero
   kicks preserve existing planar eligibility and do not invent scattering.

This is additional observational proof metadata, with a new lineage-law version,
not a physical transport version change or an inserted jitter law. Positive
remaining unknown mass must still make a production bake refuse the unresolved
measure decomposition.

## 9. Reproducible falsification probes

Run `python3 tools/hylios_lineage_probes.py --output /tmp/hylios-lineage-probes.json`
from the repository root. NumPy is required. The output is diagnostic evidence,
not a production model certificate.
The code mirrors the implemented ellipsoid chord/Snell geometry analytically;
it does not link RISE or claim a renderer test. All thresholds are diagnostic
comparison tolerances and are never proposed production classification rules.

- 6,520 strict noncritical point-source cases across spherical/prolate aspect
  ratios 1,1.5,3,10 and index ratios .5,1,1.01,1.2,1.7,2,3,10 compare the B formula
  with fourth-order finite differences. Largest normalized discrepancy was
  4.03e-8; low-index B eigenvalues stayed positive in all probed cases.
- Decimal arithmetic at 100 digits reproduces the exact ellipsoid-focus collimator
  across four meridian positions, with direction error <=2.5e-99. Rotational
  symmetry supplies the second input angle; the analytic identity is the proof.
- The eta=2 sphere probe at alpha=1e-6 has outgoing transverse component about
  -1e-18 and solid-angle Jacobian about 3e-24, confirming severe near-degeneracy
  without a finite-width-to-atom reclassification.
- At the exact ellipsoid focus, the direction-only 2x2 block has numerical
  determinant near 1e-26 or smaller, while the full4D boundary phase map has
  determinant 1.5625=eta^2 and minimum singular value about .1674 at three step
  sizes. This directly falsifies the assumption that a collapsed angular submap
  implies a collapsed full phase map, or vice versa.

These probes can falsify mistakes in formulas and expose caustics. They do not
prove generic complex-chain rank, RSA almost-sure nonfocusing, or completeness of
an angular bake. The explicit theorem hypotheses above remain the decision rule.
