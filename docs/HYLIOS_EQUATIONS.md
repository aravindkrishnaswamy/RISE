# HyLIoS P0 executable equations

**Status:** P0 derivation worksheet (`hylios_reference_interp_v1`, 2026-11-25) implemented in `HyLIoSOptics`, `HyLIoSForwardTransport`, `HyLIoSComplexLayout`, and `HyLIoSSurfaceBoundary` (see [HYLIOS_WORKPLAN.md](HYLIOS_WORKPLAN.md) §13 for shipped integration status). Retained as the equation reference separating clean-room interpretations from Chen et al. (2015) source statements; `HYLIOS_RESEARCH_NOTES.md` remains the provenance ledger.

Primary references below mean Chen et al., *Hyperspectral Modeling of Skin Appearance*, ACM TOG 34(3), Article 31 (2015), abbreviated **paper**, and Tenn Francis Chen, *On the Modelling of Hyperspectral Light and Skin Interactions and the Simulation of Skin Appearance Changes Due to Tanning*, PhD thesis (University of Waterloo, 2015), abbreviated **thesis**. Section/equation numbering is included for comparison. Units are SI unless stated; angles are radians in equations and code.

## 1. Bulk attenuation events

For each active homogeneous layer constituent `i`, sample an independent exponential free path

`d_i = -log1p(-U_i) / mu_i(lambda)`, `U_i ~ Uniform[0,1)`, `mu_i >= 0`.

`d_i` is metres when `mu_i` is m^-1. `mu_i=0` means `d_i=+infinity`; `U_i=0` validly gives `d_i=0`, and `U_i=1` is excluded. This stable form matches paper Eq. (1), `-ln(xi)/mu`, using `xi=1-U_i`; it avoids arbitrary RNG endpoint clamping. Advance by `min_i d_i`; the minimum of independent exponentials has rate `sum_i mu_i`. Stop at a layer boundary first and process that boundary before any event beyond it. The model creates attenuation clocks per constituent, not a single summed clock, so event identity is retained.

For ordinary absorbers in layer `j`, the publication uses a volume-weighted sum
`mu_a,j(lambda)=sum_i zeta_i(lambda)*v_i,j` (paper Eq. (2), §4.1); `zeta_i` is a specific absorption
coefficient in m^-1 on the stated constituent basis and `v_i,j` is dimensionless, so the result is
m^-1. Blood terms are nested volume fractions: `sum_k zeta_k*v_k,blood*v_blood,j`. This states the
equation structure only; this worksheet does not alter or certify any spectra, concentration
conversion, or dataset coverage.

This process is Poisson free-flight sampling, not an exact hard-particle packing process. Rejecting
overlaps or layer-crossing particles changes the generated process. Do not apply a generic `1/(1-f)`
correction: no such correction is established by paper Eq. (3).

## 2. Melanosome and complex geometric rate

For a prolate spheroid with semi-minor axis `a>0` and semi-major axis `b>=a`, eccentricity
`e=sqrt(1-a^2/b^2)`. Its surface-to-volume ratio is

`S/V = (3/(2a)) * (a/b + asin(e)/e)`.

The expression is metres^-1. At `e=0`, evaluate by its limit, not `0/0`: `S/V -> 3/a`, the sphere
result. This is paper Eq. (4), thesis Eq. (4.4). Here `a,b` are semi-axes; the appendix dimensions `0.41 x 0.17 um` (light) and `0.69 x 0.28 um` (dark) are treated as full major x minor diameters for this candidate convention, so divide both by two before applying the equation. The paper results prose instead says `0.40 x 0.17 um` for the light specimen. `v1` takes the supplementary appendix table value `0.41` as authoritative and records the discrepancy; major diameter differs by 2.5%, so recompute rates rather than silently substituting. Scaling both axes by `q` scales `S/V` by `1/q`.

For volume fraction `v` in `[0,1]`, use the publication geometric encounter coefficient

`mu_g = (S/V) * v / 4`.

Units are m^-1. For a sphere of radius `r`, this reduces to `mu_g=3v/(4r)`. This is paper Eq. (3),
thesis Eq. (4.3); it is the paper's geometric attenuation approximation, not an independently
validated hard-spheroid collision coefficient. Sample distance with §1. After a candidate encounter,
orient/place the particle and reject if it overlaps its predecessor or crosses its layer boundary,
as described in paper §4.2.1. The papers do not specify a rejection-conditioned replacement-distance
law; `v1` resamples candidate particle geometry/orientation at the same candidate event and records
rejection counts. This is an explicit interpretation.

For a spherical melanosome complex of radius `r_s`, use `S/V=3/r_s` and the same `mu_g=(3/r_s)v/4`;
sphere limit agrees exactly. Paper §4.2.1. A complex has diameter `D_c=2*A_major` for lightly
pigmented presets and `D_c=A_major` for moderately pigmented presets, where `A_major` is the
melanosome full major diameter (paper §4.2.1; thesis §4.2.1). The stated maximum
encapsulated-interaction count is a ratio, not an integer rule. `v1` selects `n_m=max(1,
round_half_up(D_c/d_min))`, with `d_min=2a` the full minor diameter. Let `A_major` be the full
melanosome major diameter from the table; the spheroid semi-axis above is `b=A_major/2`. For
S1-style light dimensions, `D_c=2*A_major`, so `D_c/d_min=2*0.41/0.17=4.824`, hence 5; for moderate
`D_c=A_major`, ratio `0.41/0.17=2.412`, hence 2. Floor would yield 4 and 2; ceil 5 and 3. These
alternatives change the count by up to one internal melanosome per complex. Record the selected rule
in output provenance; this rounding choice is not attributable to the authors.

## 3. Orientation and projected hit point

Paper Eq. (5) gives the orientation weight

`w(alpha) = [chi_1*(1-|cos(alpha)|) + chi_2*|cos(alpha)|] / (chi_1+chi_2)`,

with `chi_1,chi_2` the cross-sectional areas associated with the minor and major axes. `alpha` is
the polar angle between ray direction and spheroid axis; `beta` is azimuth. The printed equation
does not state its measure. Its integral over `cos(alpha) in [-1,1]` is one, while its integral over
`alpha` is generally not one. Therefore **selected measure** is `x=cos(alpha)` with density
`p_x(x)=w(acos(x))` relative to `dx`; independently `beta~Uniform[0,2pi)` from paper Eq. (6).
Equivalently the polar-angle density relative to `dalpha` is `p_alpha(alpha)=w(alpha)*sin(alpha)`.
The `chi` areas are m^2 and cancel; `p_x` is per unit dimensionless `x`, and the equivalent `p_alpha` is per radian. `beta` is per radian. This makes the spherical limit `chi_1=chi_2` isotropic (`p_alpha=sin(alpha)/2`). It also makes clear
that Eq. (5) must not be sampled as a density directly relative to `dalpha`.

This measure is a clean-room resolution of the omitted measure, not proof of the exact author
sampler. `chi` areas must be evaluated consistently in the original parameter convention; the paper
names the associated cross-sections but does not provide a complete construction. `v1` treats
`chi_1` (associated with the minor axis) as the side-on projected ellipse `pi*a*b`, and `chi_2`
(associated with the major axis) as the end-on projected disk `pi*a^2`. In the sphere limit they
coincide. An alternative physically exact collision-conditioning law weights isotropic orientations
by `A_proj(alpha)=pi*a*sqrt(a^2*cos(alpha)^2+b^2*sin(alpha)^2)`; it is not the printed linear blend
and remains a separately named alternative. The printed equation’s measure choice matters: in the
sphere limit, uniform density over alpha gives `P(alpha<30deg)=1/6`, while the isotropic spherical
measure gives `(1-cos(30deg))/2`; v1 uses the latter after applying the printed weight. This
explicit choice is testable and documented, but remains a publication interpretation.

Given the selected orientation, choose a hit point uniformly over the projected spheroid silhouette
facing the incoming ray (paper §4.2.1). “Uniform projected area” means density `1/A_proj` with
respect to projected-area measure, not uniform surface area. The paper does not give a sampler; use
rejection sampling in the projected ellipse or an equivalent area-uniform method. At `a=b`, this is
uniform disk sampling.

## 4. Melanosome internal absorption and scattering

Inside an encountered organelle, use aggregated eumelanin + pheomelanin absorption coefficient
`mu_a,mel(lambda)` in m^-1 and draw `d_a=-ln(U)/mu_a,mel`. If the sampled distance ends before the
next organelle boundary, terminate as absorbed; otherwise reach the boundary and run dielectric
Fresnel reflection/transmission using the melanin and epidermal real indices. A transmitted event
leaves the melanosome; a reflected event remains inside and receives the diffuse perturbation below,
then repeats the absorption test. This is paper §4.2.1. It is an analog survival/absorption test; do
not multiply by another absorption probability after an absorbed path.

For a diffuse interior perturbation, `U~Uniform[0,1)`, `theta=acos(sqrt(1-U))`,
`phi~Uniform[0,2pi)`. This is cosine-weighted hemisphere sampling (`p_omega=cos(theta)/pi` sr^-1);
the polar CDF is `sin^2(theta)`. Reject and resample directions that point out of the interior
according to the inward-normal test in paper §4.2.1. Rejection conditions the direction to remain
inside; it is not a boundary Fresnel event.

### Exponential forward-scatter law (paper Algorithm 1)

The target angular density relative to solid angle is proportional to `exp(-theta/theta0)`, hence
the polar density relative to `dtheta` is

`g(theta) = [exp(-theta/theta0)*sin(theta)] / Z`, `0<=theta<=pi`,

where `Z=integral_0^pi exp(-t/theta0)*sin(t) dt = theta0^2*(1+exp(-pi/theta0))/(1+theta0^2)` and azimuth is
uniform. Units: `theta0` radians; density per radian; `Z` dimensionless. This normalized form
follows from the target named in paper Algorithm 1. The paper's rejection sampler uses a
uniform-`theta` proposal and unnormalized target `h(theta)=exp(-theta/theta0)*sin(theta)/theta0`;
its maximum occurs at `phi=atan(theta0)`. `v1` may implement that exact rejection envelope or an
equivalent sampler; validate the resulting histogram against `g`.

`theta0=5 degrees` below 780 nm. Select the explicit taper `theta0(lambda)=5deg*(1400-lambda)/620`
for `780<=lambda<1400 nm`, and `0` for `lambda>=1400 nm`. At `theta0=0`, return exactly forward
direction (delta at `theta=0`), avoiding division by zero. Keep melanin absorption active when
scattering angle reaches zero. Paper §4.2.1 says the angular contribution diminishes from 780 to
1300 nm and is negligible beyond approximately 1400 nm, but its “linearly reduce to 0 over this
region” does not uniquely fix the endpoint. The selected 1400-nm endpoint is a labeled
interpretation; at 1300 nm it gives `theta0=0.806 degrees`, versus zero for a 1300-nm endpoint. Do
not claim author equivalence for the taper.

## 5. Connective-fiber Rayleigh events

Use the paper's printed Eq. (7) interpretation, including its `eta^2+1` denominator:

`mu_R(lambda) = (128*pi^5*r^6*v_f/(3*lambda^4)) * (3/(4*pi*r^3)) * ((eta^2-1)/(eta^2+1))^2`.

Here `r,lambda` are metres, `v_f` is a dimensionless volume fraction, `eta=n_fiber/n_medium`
dimensionless, and `mu_R` is m^-1. The factors reduce to `(32*pi^4*r^3*v_f/lambda^4)*contrast^2`.
This is paper Eq. (7), thesis Eq. (4.7). The printed denominator differs from the familiar `eta^2+2`
sphere-polarizability form; `v1` deliberately follows the publication, not a silent substitution. At
`eta=1`, rate is zero; at fixed other inputs it scales as `r^3`, `v_f`, and `lambda^-4`. The paper default is `r=100 nm`, `eta=1.5/1.33`, `v_f=0.22` (paper §4.2.2). Thesis §4.2.2 instead prints `eta=1.127`. `v1` uses the paper default; the thesis value is a separately named alternate preset, not a paper default. Preserve this source discrepancy in provenance.

Sample Rayleigh polar angle by rejection from uniform theta in `[0,pi]`, accepting when `U <=
(3*sqrt(6)/8)*(1+cos(theta)^2)*sin(theta)`, `U~Uniform[0,1)`, and uniform azimuth. The coefficient
is the reciprocal maximum of `(1+cos²theta)sin(theta)`; it is `3sqrt(6)/8`, not `sqrt(2)`. The
resulting polar density is `p(theta)=3/8*(1+cos²theta)*sin(theta)` per radian; its integral is one
and its solid-angle density is `3/(16pi)*(1+cos²theta)` sr^-1. This is paper Algorithm 2; thesis
text gives the same formula. At `lambda>0`, `mu_R` is finite; no event rate when `v_f=0` or `eta=1`.

The coefficient represents bulk scattering and is applied only once per papillary-dermis pass (paper
§4.2.2). `v1` defines a pass as a continuous traversal interval from entry into papillary dermis
until exit across either bounding interface. Reset eligibility only after an actual transmitted crossing exits the papillary dermis and a later transmitted crossing re-enters it. A Fresnel reflection that remains in the papillary dermis is not a new entry and does not reset the flag; an ordinary internal fiber-scatter event also does not reset it. The sources do not define re-entry state.

## 6. Layer-size reduction and boundary events

Paper result setup scales melanosome axes in the stratum spinosum to 50% and in the stratum
granulosum to 25% of the Olson et al. base axes; basale retains 100%. Apply each factor to both
axes, preserving aspect ratio, and recompute `S/V` and `mu_g` for that layer. Paper §5.1. Fractions
and dimensions are distinct: changing axes must not silently change the authored volume fraction. At
scale `q`, `S/V` and geometric rate scale by `1/q` at fixed `v`.

For the candidate publication interpretation, use this normalized density for a **micro-normal** polar angle relative to the incident-facing macro normal: `f_s(theta)=s^4*sin(theta)/(s^2*cos^2(theta)+sin^2(theta))^2`, `0<=theta<pi/2`, with uniform azimuth. The printed weight is not normalized for general `s`; define `p_s=f_s/Z_s` relative to `dtheta`, where `Z_s=integral_0^(pi/2)f_s(t)dt`. For `0<s<1`, `Z_s=s^2/2+s^4*atanh(sqrt(1-s^2))/(2*sqrt(1-s^2))`; `Z_1=1`; for `s>1`, replace the atanh term by `atan(sqrt(s^2-1))/(2*sqrt(s^2-1))` while retaining the `s^4` factor. `s` is dimensionless and `p_s` is per radian. At `s->0` the normalized normal distribution tends to a zero-angle delta; `s=1` gives `p_s=sin(theta)`. This normalization is a clean-room correction; the paper does not supply `Z_s`.

Candidate boundary interpretation `hylios2015_macro_fresnel_normal_perturb_v1` first evaluates unpolarized Fresnel reflectance `F=(R_s+R_p)/2` using the planar macro normal and real adjacent refractive indices; use Snell's law for transmission, or set `F=1` under total internal reflection. Draw reflection with probability `F`, transmission with `1-F`. This analog choice has no extra throughput Fresnel weight.

For a selected reflected branch only, orient the macro normal toward the incident side (`dot(-wi,n_i)>0`), sample a micro-normal around `n_i` using the normalized Eq. (8) law above, then reflect the original incident direction `wi` about that micro-normal. Require the micro-normal to face the incident ray (`dot(-wi,n_micro)>0`) and reject/resample if the reflected direction lies in the wrong macro hemisphere. On transmission, ignore the micro-normal and use macro-normal Snell refraction unchanged. This applies the paper's stated normal perturbation after its Fresnel branch test. It is a forward-only publication interpretation: the combination of macro-normal Fresnel branch probability and micro-normal reflected direction is not claimed reciprocal. Renderer connection/NEE work needs a separately derived reciprocal boundary variant; do not reuse this forward rule as a reciprocal BSDF.

The physically distinct alternative `hylios_microfacet_fresnel_v1` samples a perturbed normal first and evaluates Fresnel/Snell on that normal. Do not mix these orderings or claim equivalent results. The four external crossing branches for the first interpretation are:

| Crossing | Branch | `hylios2015_macro_fresnel_normal_perturb_v1` action |
|---|---|---|
| Exterior -> skin (entry) | R | Record outgoing reflection after micro-normal perturbation; terminate history. |
| Exterior -> skin (entry) | T | Refract inward by macro-normal Snell; continue. |
| Skin -> exterior (exit) | R | Reflect inward after micro-normal perturbation; continue walk. |
| Skin -> exterior (exit) | T | Refract outward by macro-normal Snell; record transmission and terminate history. |

Paper §4 introduction says Fresnel tests occur at interface hits; §4.3 gives the outer-surface normal perturbation law for reflected rays. Standard unpolarized Fresnel equations supply the abbreviated test. The branch bookkeeping makes the four R/T cases explicit; these choices require boundary tests before acceptance and do not prove reciprocity.

At internal layer interfaces, use planar normals and Fresnel/Snell with the adjacent layer indices;
transmitted rays enter the adjacent layer. At dermal-hypodermal junction, paper §4.3 sets
reflectance to one and diffusely perturbs the returned direction with the cosine hemisphere law from
§4. No transmission or Fresnel energy split is used there. At an organelle boundary use §4's
separate indices and interior diffuse perturbation only after reflection. Do not confuse these three
boundary classes.

## 7. Limits, outputs, and attribution boundary

Required analytic limits: `a=b` gives `S/V=3/a` and isotropic Eq. (5) orientation; `v=0`, `v_f=0`,
`eta=1`, or `mu=0` removes the corresponding event clock; `theta0=0` is forward delta;
sphere/complex chord and projected-hit samplers reduce to sphere/disk forms; Fresnel `n1=n2` gives
`F=0`; total internal reflection gives `F=1`; each tabulated angular PDF integrates to one under its
stated measure.

The publication leaves Eq. (5)'s `chi` construction, collision-conditioned overlap sampling, and
complex interaction rounding underdetermined; this worksheet also selects a versioned external
boundary interpretation. These are recorded assumptions, not author-equivalent behavior. As drafted,
this remains a P0 worksheet, not an implementation-ready spec: freeze the required composition and
data coverage and pass independent angular/boundary tests first. If exact author parity is required,
the listed underdetermined details need author clarification. Any resulting report must state the
chosen interpretation and sensitivity. This worksheet does not specify spectra or extrapolation.
