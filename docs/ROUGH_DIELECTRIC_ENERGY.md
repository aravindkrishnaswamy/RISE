# Rough dielectric energy compensation

`rough_dielectric_material` adds a reciprocal two-sided Kulla–Conty completion
to the existing height-correlated anisotropic GGX single-scattering kernel.
This approximates multiple scattering with smooth compensation lobes; it is
not an exact microsurface random walk. The raw `MicrofacetUtils::RoughDielectric`
kernel remains single scattering for reference and reuse.

The construction follows the glass reciprocity derivation in
[Kulla and Conty 2017, slides 29–32](https://fpsunflower.github.io/ckulla/data/s2017_pbs_imageworks_slides_v2.pdf).
Unlike a directional brightness gain, it preserves the adjoint relation needed
by BDPT/VCM. [Turquin 2019, §§3.2–4](https://blog.selfshadow.com/publications/turquin/ms_comp_final.pdf)
discusses this distinction and the approximation's diffuse-looking tail shape.

## Two-sided closure

Let `i` be the incident medium and `j` the transmitted medium, with
`eta = n_j/n_i`. All energies below use **importance** transport, before the
renderer applies its radiance eta factor. Define

```
M_i(w) = 1 - integral_sphere f_ss,i(w,v) |v.z| dv
d_i    = integral_hemisphere M_i(v) |v.z| dv / pi
```

The bake includes both reflection and transmission, Fresnel, anisotropy,
height-correlated G2/G1 and null micronormal events. It stores both interface
directions. The diffuse Fresnel transmission satisfies `T_i = eta^2 T_j`;
we integrate the low-to-high value and derive its reverse from that identity.
The jointly solved compensation split is

```
qT_i = T_i d_j / (d_i + d_j)
qR_i = 1 - qT_i
f_ms,R = M_i(wi) qR_i M_i(wo) / (pi d_i)
f_ms,T = M_i(wi) qT_i M_j(wo) / (pi d_j)
```

Zero-mean lobes carry zero probability. The two lobes integrate to `M_i(wi)`.
Their products are symmetric under reversal; for transmission, the remaining
coefficient ratio is `eta^2`. Radiance evaluation divides the transmission term
by `eta` twice. SPF weights exclude this factor, as required by `ISPF`;
integrators continue to apply `RadianceEtaScale` once. Pel, NM and HWSS use
the same scalar closure, with their respective scalar-painter evaluations.

## Table and sampling

`tools/GenerateRoughDielectricEnergy.cpp` deterministically integrates the live
VNDF using 2,048 Hammersley samples per direction, analytically summing both
Fresnel choices. The axes are maximum width, width ratio, bounded index
contrast, azimuth, and incident cosine. Axis exchange and quadrant symmetry
reduce storage without isotropizing anisotropic inputs. Logarithmic nodes
resolve the near-index/grazing transition; below contrast 0.001, interpolation
also uses log contrast. The generator retains the index contrast in Snell's
correction instead of subtracting two rounded, nearly equal cosines. Fresnel
also retains `Nt^2-Ni^2 = +/-4c` in its critical-angle discriminant, so the
smallest nonzero contrast cannot become an index-matched interface.

Directional deficits are quantized to 16 bits. Stored hemispherical means
exactly integrate the quantized, piecewise-bilinear angular profiles; they are
interpolated with the same parameter weights. Compile-time shape checks catch
a stale bake after grid changes. Regenerate with:

```sh
c++ -std=c++17 -O3 -Isrc/Library tools/GenerateRoughDielectricEnergy.cpp -o /tmp/rise-rd-bake
/tmp/rise-rd-bake 2048 > src/Library/Utilities/RoughDielectricEnergyData.h
```

The sampler mixes the original VNDF with the compensation profile using
`p_ms = M_i/(1+M_i)`. This retains single-scatter support even when a tabulated
deficit reaches one. The tail samples the piecewise-linear azimuth marginal
and conditional cosine profile using bounded 52-step CDF inversion. It does
not rejection-loop on a potentially tiny deficit. All rays report the same
aggregate mixture density and total BSDF throughput used by evaluation.
Single-scatter value/density mantissas and binary exponents are retained through
the mixture so overflowing or underflowing scalar PDFs do not create inf/inf
throughput, and radiance scaling is applied before final conversion.
Original VNDF nulls remain null; the added lobe restores their missing energy
in expectation. The geometric-horizon gate still rejects directions crossing
the wrong geometric side; that additional shading-normal clipping loss is not
compensated. Exact equal-index and smooth delta limits bypass the LUT.

## Accuracy and validation

Energy restoration is numerical, not exact: the LUT approximates the raw
kernel's directional albedo. `RoughDielectricEnergyTest` compares independent
single-scatter trials plus compensation against a white furnace over widths,
anisotropies, view angles and both interface directions. It separately checks
near-matched/grazing and high-contrast cases, adjoint reciprocity, axis exchange,
inverse-CDF endpoints, and sampled mass/energy against spherical quadrature.
The regular calibration matrix uses a 3.5% bound; the difficult boundary matrix
uses 5%. These finite test matrices are not a uniform error guarantee for every
positive width, index and direction. Raw-kernel extreme-range tests remain
separate. Exact table-model reciprocity and sample/evaluation agreement are
tested with tighter numerical tolerances.

`RoughDielectricMaterialTest` verifies the closure's API/parser/editor bindings,
nested-media crossings, aggregate PDFs, transport modes and spectral replay.
The existing conservative hero-only HWSS fallback, including nested composites,
remains in force. Importer mappings remain separate Tier 1.1 work.
