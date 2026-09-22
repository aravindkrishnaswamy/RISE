# DL-178 / DL-212 — geometric correction for Schlick and bounded Ward

Derivations and initial experiments: 2026-09-19. Candidate validation: 2026-09-20.
Cost and isolated proposal experiments continued on 2026-09-21. Bounded Schlick
geometric attenuation (DL-178) and Geisler-Moroder & Dür bounded Ward transport
(DL-212) are formally accepted with exact domain-split quadrature and a 4-entry
thread-local evaluation cache. Finite grazing residual for Schlick is tracked
under open DL-225. Appearance changes are documented below for both models. This document
separates a specular model's energy bound, the sum of independently authored
`Rd` and `Rs`, and the bounded auxiliary albedo AOV. These are different
contracts: an AOV saturation does not repair transport energy.

## Published definitions and independent derivation

Schlick's [1994 paper and author copy](https://dept-info.labri.fr/~schlick/DOC/eur2.html)
define Eq.31 as `G(c)=c/[r+(1-r)c]`, using the paper's roughness `r` directly.
RISE retains its existing directional distribution `Z=r/[1-(1-r)t²]²`,
azimuth factor `A=sqrt(p/[p²+(1-p²)w²])`, and Fresnel
`S=rho+(1-rho)(1-h.v)^5`. Here `t=n.h`, `h=normalize(l+v)`,
`nv=n.v`, `nl=n.l`. Multiplication by `G(nv)G(nl)` cancels the former
`nl nv` denominator analytically:

```
f_S = S Z A / [4 pi (r+(1-r)nv)(r+(1-r)nl)]
p_S = t Z p_phi / [2(h.v)]
kray_S = S A (h.v) nl /
         [2 pi t p_phi (r+(1-r)nv)(r+(1-r)nl)]
```

`Z` cancels in the weight; **roughness does not disappear** because `G`
contains `r`. Pel, NM, per-channel, companion `EvaluateKrayNM`, and the
aggregate Pdf's realized selection weights all carry this same factor.
Eq.30's additional re-emission term is not introduced: this change implements
the explicitly prescribed Eq.31 product on RISE's single-scattering model.

Both Ward materials adopt Geisler-Moroder and Dür,
[*A New Ward BRDF Model with Bounded Albedo* (2010)](https://doi.org/10.1111/j.1467-8659.2010.01735.x).
The [authors' presentation](https://www.radiance-online.org/community/workshops/2010-freiburg/PDF/geisler-moroder_duer_thetmeyer_RW2010.pdf)
shows the BRDF, unnormalised-half-vector rewrite, and sampling weight
(slides 12–13, 21–22). Set `ax=ay` for isotropy:

```
E = exp(-[(h.u/ax)^2+(h.v_tangent/ay)^2]/t²)
f_S = Rs E / [4 pi ax ay (h.v)^2 t^4]
p_S = E / [4 pi ax ay t^3 (h.v)]
kray_S = Rs nl / [(h.v)t] = Rs 2 nl/(nv+nl)
```

The new Ward weight is strictly less than `2 Rs` on accepted directions;
its roughness cancels exactly. The Gaussian sampler and stored density stay
unchanged. An independent test reference uses `H=l+v` and
`f_S=Rs E |H|²/[pi ax ay (H.n)^4]`, avoiding the production normalization.
The diffuse lobe remains additive `Rd/pi`; over-authored `Rd+Rs` can still
exceed one even though the Ward specular family is bounded by `Rs`.

## Schlick residual: DL-225 remains open

Eq.31 removes the missing-G divergence, but does **not** make this complete
Schlick specular family energy-conserving. Formula-only half-vector
quadrature at `rho=.9,r=.1,p=1,theta=89.9°` converges as follows:

| Radial × azimuth nodes | Specular directional reflectance |
|---|---:|
| 64 × 128 | 1.2455530234 |
| 128 × 256 | 1.2448134128 |
| 256 × 512 | 1.2446129561 |
| 512 × 1024 | 1.2445614434 |
| 1024 × 2048 | 1.2445492385 |

Independent outgoing-angle quadrature of the live corrected BRDF gives
1.244529893. This is a finite model residual, not sampler/BRDF disagreement.
For isotropy, `Z` is a GGX distribution with `alpha²=r`; at grazing, exact
Smith `G1` scales as `2 nv/sqrt(r)` whereas Eq.31 scales as `nv/r`. The
latter is too large when `r<.25`. Replacing just that isotropic factor does
not establish a bound for RISE's anisotropic `A` family. DL-225 therefore
requires a reciprocal normalization/masking treatment for both families,
with weights and aggregate Pdf updated together. No arbitrary transport
clamp or unreviewed lookup table is part of this slice. The residual witness
is printed as open, not mislabeled as a passing energy bound.

The original DL-178 family is corrected:

| r | Angle | Before Q | With Eq.31 Q |
|---:|---:|---:|---:|
| .1 | 0 | .818189401 | .771496478 |
| .1 | 30 | .823127970 | .752213958 |
| .1 | 60 | .874427388 | .700605683 |
| .1 | 80 | 1.314463504 | .761652130 |
| .5 | 0 | .600015575 | .405628244 |
| .5 | 30 | .642314289 | .399614428 |
| .5 | 60 | .892408981 | .392111758 |
| .5 | 80 | 2.078542030 | .403208766 |
| .8 | 0 | .500016140 | .278303630 |
| .8 | 30 | .561369883 | .277693191 |
| .8 | 60 | .900005384 | .277182101 |
| .8 | 80 | 2.418726332 | .278490429 |

These are formula-only outgoing-angle 800×1600 integrations at `rho=.9,p=1`.

## Auxiliary albedo and consumers

Schlick's auxiliary albedo integrates the corrected directional reflectance,
then saturates to `[0,1]` as required by `IBSDF::albedo`. Sampling `p_h=tZ/pi`
analytically cancels the sharp distribution peak. The substitution `xi=u²`
with Jacobian `2u` removes its grazing endpoint singularity; a deterministic
16×32 rule integrates two Fresnel moments, reusing matching roughness/isotropy
between RGB channels. The actual geometric-horizon gate is included; diffuse
reflectance uses the exact clipped-cosine fraction `(1+n.g)/2`.

An independent 300-state sweep over `r=.02/.1/.3/.5/.8`,
`p=.1/.3/.7/1`, incidence `0/30/60/80/89°`, and azimuth `0/45/90°`
compares against 512×1024 nodes. Maximum raw error is .0187550
(`r=.02,p=.1,89°,azimuth0`, raw estimate1.7384127/reference1.7571677).
Maximum **bounded-AOV** error is .0162536
(`r=.1,p=.1,80°,azimuth0`, .6628490/.6791027). A chromatic test additionally
uses `rho=(.2,.9,.5)`, `r=(.02,.1,.8)`, `p=(.3,.1,1)` and a separate
half-angle solid-angle reference with the reflection Jacobian.

This is not an every-bounce transport operation, but it is also not necessarily
once per final pixel. PT/BDPT capture albedo for requested fast/accurate AOV
samples, `AOVBuffers` can supersample a separate guide pass, the interactive
material preview evaluates it per visible hit, and Fabric's auxiliary albedo
can delegate to its base BRDF (the scene parser currently excludes these
Schlick/Ward models from Fabric's allowed base-material set). Accuracy and cost must be judged on those
consumers. Ward retains a conservative saturated `Rd+Rs` auxiliary estimate.

## Aggregate Pdf: integrate the selection domain

Changing `kray` changes `RandomlySelect`'s realized weights. The aggregate
remains `C_D p_D + sum q_j p_j`, with the same exact query-direction
specular coefficients and clipped-cosine diffuse acceptance. An initial
32×32 sampler replay was insufficient: an independently converged state
(`Rd=.4,Rs=.5,alpha=.1`, view60° in the shading frame, view azimuth0,
geometric tilt30°) has `C_D=.720097153229`, whereas replay32 returns
`.713337220462`. Its diffuse mass discrepancy alone is
`.006759933*(1+cos30°)/2=.0063066`. The public regression now isolates that
coefficient through a direction where the specular Gaussian is below
`1e-100`; the old approximation fails its `1e-5` accuracy requirement.

`WardSelectionQuadrature.h` instead transforms the shared sampler draw to
an elliptical slope and a Gaussian radial coordinate:

```
x_j = alphaX_j cos(psi), y_j = alphaY_j sin(psi)
h_j = (s x_j, s y_j, 1) / sqrt(1+s²(x_j²+y_j²))
p(s) ds = 2s exp(-s²) ds
```

This is a measure-preserving reordering of Ward's quadrant-folded azimuth,
shared by all lanes, so their random-number correlation is preserved. At a
fixed `psi`, write `a²=x²+y²`, `sv=x vx+y vy`, `sg=x gx+y gy`, and `vg=v.g`.
The shading and geometric acceptance boundaries are quadratics in `s`:

```
cos_o numerator = nv (1-a²s²) + 2 sv s
geom_o numerator = 2(nv+sv s)(ng+sg s) - vg(1+a²s²)
```

The union of every lane's positive roots splits the radial integral into
regions with constant acceptance sets. Rejected regions integrate
analytically. Accepted regions use eight-point Gauss–Legendre quadrature;
additional fixed radial cuts at 2 and 4.5 resolve the Gaussian. A further
quadratic split where `cos_o=4 nv` resolves the grazing transition in
`2 cos_o/(nv+cos_o)` without altering that weight. The omitted
tail has total probability `exp(-4.5²)<1.61e-9`; including it with coefficient
one bounds that tail error by the same amount. A scaled, cancellation-safe
quadratic solver handles linear and repeated-root cases.

Angular cuts include each lane's geometric discriminant zeros, the two
shading/geometric horizon intersections, and view-aligned quadrants. A
`sin²` change of variable regularizes interval endpoints; each interval uses
32 Gauss–Legendre nodes. This has fixed work, no recursive convergence loop
and no fallback to the biased midpoint grid. At most 35 angular cuts and
24 radial cuts give a conservative cap of 200,192 numeric radial nodes;
all-rejected pieces cost no numeric nodes. The final 472-state suite combines the original 112-state roughness,
anisotropy, chromatic, incidence and tilt grid with 360 extreme states
through 89.99° and axes .01 through 1. It observes at most 29,016 numeric
nodes and maximum absolute error `6.9047e-6` (axes 1/.01, view 89.9°,
azimuth 0°, no tilt). The worst reference converges from `.643360592079`
at 128 angular nodes to `.643360591407` at both 256 and 512, with 32 radial
nodes. The earlier 24×8 rule missed the preserved `1e-5` target on some
untilted grazing states; the final 32×8 rule and radial transition split
resolve them. The cold
coefficient accuracy target is `1e-5`, substantially below the sampling
error in the public mass, TVD, and estimator tests. The aggregate mass error
from this coefficient is at most its absolute error, because `int p_D<=1`.

The coefficient depends on the incoming view and material state, not the
outgoing query direction. PT uses it for NEE/guiding and continuation; BDPT
also queries forward/reverse and connection endpoints. Four thread-local
memo entries reuse identical values. The complete key is the view and
geometric normal in the sampling frame, diffuse selection weight, lane
count, and all live lane axes and weights. It contains no object, material,
or intersection pointers; an edit or different wavelength changes its
numeric key. Pure cold evaluation remains available to tests. Storage is
616 bytes per thread and disappears with the thread. The cache follows
[const-correctness guidance](skills/const-correctness-over-escape-hatches.md):
it changes evaluation cost only, with no shared material mutation.

A quiet standalone benchmark of the preliminary 24-angular-node prototype
(before the additional grazing transition split) retained all three alternating-order
replicates, including first-replicate warmup variation:

| Algorithm | ns/call mean ± sample SD (n=3) |
|---|---:|
| Midpoint16 | 1027.9 ± 579.2 |
| Midpoint32 | 3286.8 ± 1088.5 |
| Boundary method, cold | 21563.9 ± 1027.3 |
| Boundary method, changing 112-state sequence | 19601.4 ± 168.9 |
| Boundary method, identical-key warm | 3.187 ± .012 |

Cold state means ranged 7993–134018 ns. The changing-state sequence contains
some identical keys (normal-view azimuth variants); it is not advertised as
100% cache misses. These are algorithm-selection microbenchmarks, not whole
renderer overhead. Cold/changing-hit production Pdf and scene timings must
remain visible alongside cache-hit results.

## Production cost (quiet, 2026-09-20)

Three independent process runs per binary, alternating base/fixed order,
retain every measurement including the first run. Each row below gives
mean ± sample SD in ns/call. The incoming angle is measured from the normal;
view azimuth is fixed with tangent components in the ratio .8/.6. Uniform
material states use `Rd=(.4,.1,.2)`, `Rs=(.2,.5,.8)`; Schlick uses
`r=.3,p=.4`, Ward isotropic `.3`, anisotropic `.3/.12`, and chromatic
axes `(.1,.3,.6)/(.3,.12,.6)`. The query direction is `normalize(.2,.3,1)`.

Warm Pdf repeats one hit 100,000 times. Changing-view Pdf cycles 97 distinct
views (angle increments of .001°) for 5,000 calls, exceeding the four-entry
cache; every timed query misses. Both binaries include the same ray update
and loop overhead, and each row has 100 untimed setup calls. These are
production material entry points linked to separate source states, not
the preliminary integration-only benchmark above.

| Model | View | Base Pdf | Fixed warm Pdf | Base changing Pdf | Fixed changing Pdf |
|---|---:|---:|---:|---:|---:|
| Schlick | 30° | 790.6 ± 27.8 | 864.1 ± 24.2 | 780.4 ± 8.1 | 849.8 ± 8.5 |
| Schlick | 80° | 734.5 ± 3.1 | 793.8 ± 2.2 | 733.4 ± 8.2 | 793.1 ± 11.1 |
| Schlick | 89° | 672.4 ± 2.1 | 732.0 ± 2.6 | 672.3 ± 3.1 | 730.5 ± 1.7 |
| WardIso | 30° | 652.3 ± 3.7 | 30.3 ± 0.1 | 653.2 ± 7.8 | 26128.0 ± 205.5 |
| WardIso | 80° | 560.4 ± 1.0 | 31.5 ± 0.2 | 563.9 ± 6.3 | 25874.5 ± 80.8 |
| WardIso | 89° | 523.5 ± 4.5 | 32.5 ± 0.2 | 528.7 ± 6.7 | 26777.2 ± 27.8 |
| WardAniso | 30° | 1124.1 ± 0.5 | 60.1 ± 0.9 | 1131.5 ± 22.3 | 22728.6 ± 137.0 |
| WardAniso | 80° | 1127.3 ± 2.2 | 54.4 ± 0.1 | 1082.3 ± 12.4 | 24986.6 ± 60.5 |
| WardAniso | 89° | 1060.9 ± 2.9 | 56.3 ± 0.4 | 1054.0 ± 17.8 | 26157.8 ± 180.1 |
| WardChromatic | 30° | 3234.9 ± 2.8 | 205.8 ± 1.0 | 3237.1 ± 11.5 | 101362.8 ± 430.9 |
| WardChromatic | 80° | 3069.6 ± 46.3 | 216.5 ± 0.9 | 3038.7 ± 51.9 | 123365.5 ± 1227.8 |
| WardChromatic | 89° | 2878.8 ± 7.6 | 213.6 ± 2.5 | 2873.9 ± 12.9 | 128643.7 ± 330.6 |

The Ward changing-hit cost is material: 20–51× the historical midpoint
approximation. Warm-cache speedups cannot stand in for this cost. Complete
render measurements below therefore include isotropic, anisotropic and
chromatic Ward workloads. BRDF `value()` costs 21–45 ns in these states,
with fixed/base ratios 1.000–1.028 (one million calls per row). Ward's
auxiliary albedo remains 5–6 ns. Schlick's albedo rises from 4.7–5.6 ns
to 645–672 ns (10,000 calls per row); its final per-angle values are
653.7 ± 89.6, 672.1 ± 71.1, and 645.0 ± 73.7 ns at 30°, 80°, and 89°.
This auxiliary cost is measured through the actual preview pipeline below.

## Ward energy and tails

The following live `value()` quadratures use the independent half-vector
solid-angle reference (400×800), `Rd=0`, `Rs=.5`; the anisotropic rows
use fixed `ay=.12`. Both stored-density identities remain exact. The earlier
outgoing-angle reference undersampled the narrow grazing peak, which is why
the replacement integrates half-vector measure instead.

| Model | ax | View | Before Q | After Q |
|---|---:|---:|---:|---:|
| iso | 0.10 | 0.0° | 0.485590 | 0.495064 |
| iso | 0.10 | 60.0° | 0.244526 | 0.487043 |
| iso | 0.10 | 80.0° | 0.091798 | 0.433772 |
| iso | 0.10 | 89.9° | 0.135082 | 0.482773 |
| iso | 0.30 | 0.0° | 0.396954 | 0.455008 |
| iso | 0.30 | 60.0° | 0.211168 | 0.409305 |
| iso | 0.30 | 80.0° | 0.122546 | 0.390239 |
| iso | 0.30 | 89.9° | 0.518433 | 0.491952 |
| iso | 0.60 | 0.0° | 0.247390 | 0.331194 |
| iso | 0.60 | 60.0° | 0.158644 | 0.333649 |
| iso | 0.60 | 80.0° | 0.131546 | 0.379488 |
| iso | 0.60 | 89.9° | 0.787672 | 0.494664 |
| aniso | 0.10 | 0.0° | 0.482540 | 0.493954 |
| aniso | 0.10 | 60.0° | 0.242989 | 0.485915 |
| aniso | 0.10 | 80.0° | 0.091230 | 0.432771 |
| aniso | 0.10 | 89.9° | 0.134381 | 0.482708 |
| aniso | 0.30 | 0.0° | 0.436039 | 0.473918 |
| aniso | 0.30 | 60.0° | 0.231630 | 0.425877 |
| aniso | 0.30 | 80.0° | 0.133107 | 0.399443 |
| aniso | 0.30 | 89.9° | 0.558155 | 0.492387 |
| aniso | 0.60 | 0.0° | 0.344232 | 0.409449 |
| aniso | 0.60 | 60.0° | 0.213134 | 0.381942 |
| aniso | 0.60 | 80.0° | 0.170162 | 0.404863 |
| aniso | 0.60 | 89.9° | 0.995792 | 0.495514 |

The same fixed-seed sampling experiment (200,000 draws per row) at 89.9° gives:

| Model | Before mean / p99.9 / max / max:mean | After mean / p99.9 / max / max:mean |
|---|---|---|
| Ward iso .3 | .5187 / 3.6183 / 3.6865 / 7.1071 | .4935 / .9981 / .9982 / 2.0226 |
| Ward aniso .3/.12 | .5569 / 3.6529 / 3.6866 / 6.6195 | .4919 / .9981 / .9983 / 2.0294 |
| GGX conductor .3 control | .2848 / .4510 / .4522 / 1.5875 | .2848 / .4510 / .4522 / 1.5875 |
| Schlick .3, p1 | 84.8723 / 2395.0084 / 26522.4006 / 312.4977 | .3646 / 6.1102 / 9.7696 / 26.7946 |

These are per-draw sum-of-lobe weights, not image quantiles. DL-177's
tail question is resolved by changing Ward's published normalization: its
accepted per-lobe weight is analytically bounded by `2 Rs`. The Schlick
change strongly reduces this tail without asserting a global conservation
result; DL-225 remains open.

## BDPT controls and measurement dispatch

At 32×32, 256 spp, three independent base/fixed process pairs alternate
execution order. The tabulated scalar is the average of RGB image means,
with sample SD across the three renders. All twelve L/M checks pass in
every process; the original two-percent L parity threshold is unchanged.

| Topology / integrator | Base mean ± sample SD | Fixed mean ± sample SD | Change |
|---|---:|---:|---:|
| L / PT | 0.06427794 ± 0.00000589 | 0.05825891 ± 0.00000374 | -9.364% |
| L / BDPT | 0.06426296 ± 0.00000528 | 0.05822671 ± 0.00000467 | -9.393% |
| M / PT | 0.04712938 ± 0.00000585 | 0.04713510 ± 0.00000724 | +0.012% |
| M / BDPT | 0.04720443 ± 0.00000406 | 0.04720782 ± 0.00000156 | +0.007% |

`--materials-only` calls the unchanged `TestSchlickMultiLobe()` and
`TestGGXLambertianControl()` functions and returns their accumulated status.
Its only other source change adds the option to usage text. The default
full suite, `--spectral-only 1|2`, and `--spectral-aggregate-unit` dispatches
remain intact. The final full candidate run passed 170/0.

## Rejected external fixed-selection alternative (2026-09-21)

An ABI-isolated experiment preserved Ward's emitted directions, conditional
stored densities, physical tags and `kray=f_I*cos/p_I`, but gave the external
selector direction-independent painter weights. Its public density used an
analytic inner Gaussian CDF and bounded outer quadrature. The production
source was not replaced by this experiment. The default selector's 74,016-case
digest stayed identical, 72 emission traces stayed byte-identical, and live
Ward density (404/0), HWSS (172/0), and physical-model checks (917/0) passed.

The quiet whole-render comparison retained all 66 runs: three replicates for
each scalar/chromatic PT/BDPT comparison and the added tilted-chromatic
fixtures. Relative to the original baseline, experimental frame times were
2.044–4.048× for scalar Ward, 7.674–11.900× for chromatic Ward, and
12.544–18.868× for tilted chromatic Ward. This alternative was also rejected
for cost. The isolated preselection experiment remains a faster alternative,
but deliberately changes the stored-density/null-event contract and requires
a coherent DL-67 guiding and response-provenance prerequisite; it is not
a shipping replacement by itself.

A separate bounded profile counted every cache event and sampled per-thread
CPU time on eight fixtures, once each. BDPT cache hit rates were only
4.64–5.19%. Coefficient evaluation accounted for approximately 98.94% of
chromatic Ward Pdf CPU and 99.31% in the tilted chromatic case. Traversal was
a small fraction. Four renderer threads participated; accumulated CPU
estimates are distinct from elapsed frame time. One profile run cannot
isolate causal instrumentation overhead from the previous three timing runs.

Composite's existing DL-24 density-shape defect remains explicit. Bare Ward
proofs do not establish universal wrapper/MIS/guiding compatibility, even
when an aggregate density integrates to approximately one. The candidate
Schlick correction also retains the separately documented finite DL-225
energy residual; no global energy-conservation claim is made.
