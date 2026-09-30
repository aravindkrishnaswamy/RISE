# DL-324: Ward frame slopes and full Gaussian quotient range

A finite binary64 normalized vector and frame normal can have a dot product
slightly above one. Ward's isotropic and anisotropic BRDFs reconstructed
`tan(acos(n.h))`; the anisotropic BRDF also reconstructed an azimuth with
`acos(u.normalize(h-(n.h)n))`. Rounded poles and tangents can invalidate
either inverse cosine. The exact axis pole is a positive control:
`Vector3Ops::Normalize(0)` returns zero, so its azimuth expression is
`acos(0)`, finite, and the polar tangent is zero.

For frame components `hx=h.u`, `hy=h.v`, `hz=h.n`, the Gaussian exponent is

```
Q = (hx / (hz ax))² + (hy / (hz ay))²
fS / Rs = exp(-Q) / (4 pi ax ay (h.wi)² hz⁴)
pH = exp(-Q) / (pi ax ay hz³)
pS = pH / (4 h.wo)
kray / Rs = 2 cos_o / (cos_i + cos_o)
```

Frame slopes stay nonnegative when `hz` rounds above one. The exact pole
has zero slopes and needs no azimuth. Isotropic evaluation uses `ax=ay`.
Both models share a complete quotient in `WardSelectionQuadrature.h`.
The reflectance or aggregate selection coefficient enters that quotient
before exponentiation: neither `exp(-Q)`, `ax*ay`, nor the unweighted
kernel must be individually representable. The fallback evaluates
`exp(-Q - log(denominator) + log(abs(weight)))` with the weight's sign.
Positive axes are never rejected because their square underflows.

The ordinary fast path requires every denominator factor and absolute
weight in `[2^-64, 2^64]` and `Q<=300`. There are at most eight
denominator factors (plus `4 pi`); their product stays normal. The
combined exponent, weight, and denominator stay above approximately
`2^-1013`, also normal, under any multiplication/division reassociation.
Division rounds the final result directly. These bounds select
an arithmetic path; they are not authored-axis floors, epsilons, energy
clamps, or finite-value suppression. Genuinely nonrepresentable final
values remain nonrepresentable.

Explicit HWSS companions retain their entire spectral quotient too:
`Rs exp(-Q) cos_o / (4 pi ax ay hd² hz⁴ pdfHero)`. The standalone
spectral lobe may round to zero while this quotient is representable;
conversely `Rs cos_o/pdfHero` may overflow before multiplication by the
Gaussian. A separate log helper keeps all terms inside the final
exponent. Default (non-explicit) kray, diffuse semantics and the ordinary
BRDF/density/generation helpers are unchanged. No performance claim is
made for this newly changed HWSS path.

Both samplers now construct Cartesian half-vector slopes and normalize
with scaling and `hypot`, avoiding axis squares, `D=Inf`, and `0*Inf`.
For the anisotropic sampler let `q=floor(4 xi1)`, `v=4 xi1-q`, and
`t=(pi/2)v`. Its existing folded-quarter convention draws
`(ax cos(t), ay sin(t))*sqrt(-log(xi2))` with signs `++`, `-+`, `--`,
`+-` for quarters 0 through 3. Thus quarters 1 and 3 run backwards in
physical azimuth, exactly as the old `pi-phi`/`2pi-phi` construction.
The inverse recovers `t=atan2(abs(hy/ay),abs(hx/ax))` and the signs;
chromatic siblings replay the same recovered pair with their own axes.
At shared quarter endpoints the duplicated axis maps replay identically.
Isotropic azimuth remains uniform `2 pi xi1`. Stored conditional densities
use the sampled `-log(xi2)` directly, while queried densities use the frame
slopes. Radial endpoint zero is the excluded tangent limit. Selection
quadrature/cache, accepted-lobe weighting, random dimensions, diffuse
fallback, and energy policy are unchanged.

`WardDensityKrayTest` exercises public `IBSDF` / `ISPF` consumers. Independent
long-double projections evaluate the analytic GMD value, reciprocity, and
explicit hero-density companion weights on axis/non-axis poles, near poles,
ordinary slopes, and near-tangent underflow. Axes include `.01/.37`, equal
`.01`, chromatic axes, and finite `1e-9` density controls that amplify the
old negative exponent. The test asserts that constructed finite normalized
normal/tangent dots actually exceed one. It also exercises sampler zero,
quadrant boundaries, `nextafter(1,0)`, and the excluded upper endpoint one
as a finite pole control, at normal and 89.99-degree incidence.

Review found two further range defects in the first candidate: finite
off-pole quotients for axes `1e-170` at exponents 700/750 became infinity
or zero, and actual anisotropic RGB/NM sampling at axes `1e-170/.5`
stored NaN despite finite queried density. The final public regressions
also cover `1e-150` controls, full RGB/NM values, HWSS lobe/explicit hero
companions, conditional/aggregate densities, chromatic quarter replay,
and representable reflectance-weighted values above the unweighted
kernel's range. Independent log oracles agree with 80-digit Decimal
anchors; relative checks remain sensitive to tiny nonzero expected values.

With the earlier range/sampler test snapshot (`28b24143` SHA256), clean library builds and exact target
relinks give **17,162 passed / 149 failed** on first-candidate production
`98473a987`, and **15,194 passed / 2,117 failed** on original production
`cc516a0a8`. Axis-pole positive controls remain finite. That earlier repaired
focused run passes **17,315 / 0**. External provenance hashes every
production/header/test state. Prior test counts and the first test
ownership compile error are retained separately, not reused as final proof.

A subsequent actual HWSS hero draw at 650 nm, with axes .0107 there and
.01 at 550 nm, produces a finite stored density while the standalone
550 nm lobe rounds to zero. Its explicit companion is nevertheless
representable (approximately 6.1e-45 in the first control). A second
subnormal-radial draw makes the prematurely formed `Rs/pdfHero` overflow.
Final corrected tests on production `58b7709af` give 17,337 passed /
4 failed, all four explicit-companion checks. The initial second-control
oracle incorrectly assumed sampled `wo=wi`; subnormal random-number
quantization slightly changes the draw. The non-explicit control now
uses actual `2 cos_o/(cos_i+cos_o)`, and those two oracle-only failures
are excluded. Final corrected-test/source hashes and clean library/exact
relink provenance are retained externally. The final corrected focused
GREEN passes **17,341 / 0** after a clean repaired library build and exact
target relink. Earlier RED/GREEN counts
above describe their declared earlier regression snapshots.

On this Apple Silicon target, `long double` has binary64 precision; the
oracle is independent by analytic formulation, not additional precision.

Two existing contracts matter to the oracle. Nominal RGB black has a small
nonzero lifted spectral reflectance, so full NM value includes that diffuse
term. In RGB, the zero-weight diffuse ray remains an emitted fallback when
the specular sample is rejected. Its aggregate density is nonzero even for
a zero diffuse BRDF. An independent elliptical-Gaussian horizon-tail
integral checks this coefficient. These are accounted for explicitly,
without changing their transport policies.

The historical rare showroom block is not deterministically attributed.
The unchanged original six baseline renders are reused with their exact
binary/scene/options/image provenance. Final repaired sampling arithmetic
gets a fresh predeclared six fixed renders, seeds 8102–8107, 800x600
pixelpel four samples, irradiance cache and OIDN off, and all cores
available (`render_thread_reserve_count 0`). Temporary per-sample pixelpel
and Ward-anisotropic material traps record the first nonfinite value.
CLI interactive render/quit normally returns one; validity requires
canonical scene load, all-pixels completion, fresh EXR and independent
finite scan. Original partial-scene error and prior candidate's six fixed
renders remain separate evidence. A finite bounded batch does not prove
a zero rare-event rate or identify the historical lobe.

All six sampler-repair-source EXRs independently scanned finite 800x600
RGBA values and neither trap fired. Prior-candidate twelve-image evidence
and a preliminary repair six-image batch are retained separately; the
latter preceded the final fast-path bound tightening and is excluded
from final-source attribution.

Three retained 500,000-call diagnostic public BRDF replicas/state
(including warm-up) at 0/30/80 degrees measure isotropic `.01`
18.62/15.25/13.80 ns original versus 30.83/24.05/22.93 ns final;
anisotropic `.01/.37` measures 21.68/21.70/21.58 versus
22.91/22.95/23.03 ns. The ordinary kernels are slower; this is the
measured cost of the range checks and complete quotient. The same
anisotropic finite trap was present in both diagnostic builds.
External replicas and standard deviations are retained; these limited
states do not predict universal cost. Original-to-final showroom user
CPU is 42.61±0.30 versus 42.87±0.25 seconds, wall 4.172±0.042 versus
4.201±0.167 seconds, n=6/state. Scheduling, per-thread stream assignment
and block order confound causality; no whole-render speedup is claimed.

The later explicit-HWSS-only repair preserves the measured ordinary
BRDF, Gaussian/density helpers, generation and replay sections byte for
byte; an external section-hash manifest proves this against the measured
source. Those bounded RGB scene/kernel measurements are reused with
that attribution and do not measure the new spectral-companion path.

Schlick's ruby paths have separate bounded rational distribution/masking
machinery. They share tangent normalization but never feed its projection
to `acos`; their inverse polar replay already bounds `hz²` to one. The
shipped ruby has roughness `.05`, isotropy `1`, so its azimuth factor is
identically one even at a rounded pole.
Their existing density, masking and BRDF/kray suites remain gate controls.
Integrator RGB/NM/HWSS consumers inherit the fixed material values; no
integrator code changes were needed. Bare-material tests do not expand the
known wrapper/MIS or energy-policy claims in the parent document.
