# DL-178 / DL-212 — geometric correction for Schlick and bounded Ward

2026-09-19. The shipped appearance changes in both models. This document
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
can delegate to its base BRDF. Accuracy and cost must be judged on those
consumers. Ward retains a conservative saturated `Rd+Rs` auxiliary estimate.
