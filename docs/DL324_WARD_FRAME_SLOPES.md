# DL-324: Ward frame slopes at rounded poles

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

This removes redundant normalization and trigonometry. Squared slopes stay
nonnegative when `hz` rounds above one. The exact pole has zero slopes and
needs no chosen azimuth. Isotropic evaluation uses `ax=ay`; both models use
the same scalar kernel in `WardSelectionQuadrature.h`. An underflowed
Gaussian returns zero before multiplying or dividing extreme normalization
factors. There is no new epsilon, downstream image repair, energy-policy
change, API change, or object-layout change.

The isotropic SPF previously used `(1-cosH²)/cosH²` in its density, while
its chromatic replay separately clamped that complement. The anisotropic
SPF already used `atan2` for azimuth, but its polar complement could still
be negative. Both now evaluate the polar radius/exponent from tangent
components. Anisotropic chromatic replay recovers the shared radial draw
from that nonnegative exponent; `atan2` remains only for inverse azimuth.
Sampling retains its existing inverse CDF and quadrant convention; stored
isotropic sample densities use `sinTheta/cosTheta` from that draw rather
than subtracting nearly equal squares. Conditional densities, lobe values,
aggregate Pdf and HWSS companion evaluation retain their existing measures.

`WardDensityKrayTest` exercises public `IBSDF` / `ISPF` consumers. Independent
long-double projections evaluate the analytic GMD value, reciprocity, and
explicit hero-density companion weights on axis/non-axis poles, near poles,
ordinary slopes, and near-tangent underflow. Axes include `.01/.37`, equal
`.01`, chromatic axes, and finite `1e-9` density controls that amplify the
old negative exponent. The test asserts that constructed finite normalized
normal/tangent dots actually exceed one. It also exercises sampler zero,
quadrant boundaries, `nextafter(1,0)`, and the excluded upper endpoint one
as a finite pole control, at normal and 89.99-degree incidence.

Two existing contracts matter to the oracle. Nominal RGB black has a small
nonzero lifted spectral reflectance, so full NM value includes that diffuse
term. In RGB, the zero-weight diffuse ray remains an emitted fallback when
the specular sample is rejected. Its aggregate density is nonzero even for
a zero diffuse BRDF. An independent elliptical-Gaussian horizon-tail
integral checks this coefficient. These are accounted for explicitly,
without changing their transport policies.

The historical rare showroom block is not deterministically attributed by
these probes. External evidence records a predeclared six-before/six-after
budget, seeds 8102–8107, 800x600 pixelpel four samples, irradiance cache and
OIDN off, and all cores available (`render_thread_reserve_count 0`). Temporary
per-sample pixelpel and Ward-anisotropic material traps captured no nonfinite
values; all twelve 32-bit EXR captures independently scanned finite. This
bounds the investigation; it does not establish a zero rare-event rate or
identify which lobe caused the historical block. CLI interactive render/quit
normally returns one; validity instead required successful canonical load,
all-pixels completion, fresh 800x600 EXR output and independent finite scan.
An exploratory partial-scene output-configuration error is excluded.

External diagnostic-build measurements retain every replicate. Three
500,000-call public BRDF batches per state measured isotropic `.01` at
18.62/15.25/13.80 ns before versus 22.87/18.99/16.97 ns after at 0/30/80°;
anisotropic `.01/.37` measured 21.68/21.70/21.58 versus
17.74/17.66/17.66 ns. The anisotropic diagnostic finite trap was present
in both builds; these are bounded microbenchmarks, not universal costs.
Sequential blocked showroom batches measured user CPU 42.61±0.30 seconds
before and 41.22±1.36 after (wall 4.172±0.042 versus 4.169±0.246 seconds,
n=6 each). Scheduling and block order confound causal interpretation;
no whole-render speedup is claimed.

Schlick's ruby paths have separate bounded rational distribution/masking
machinery and do not use Ward's tangent-normalization/complement expressions.
Their existing density, masking and BRDF/kray suites remain gate controls.
Integrator RGB/NM/HWSS consumers inherit the fixed material values; no
integrator code changes were needed. Bare-material tests do not expand the
known wrapper/MIS or energy-policy claims in the parent document.
