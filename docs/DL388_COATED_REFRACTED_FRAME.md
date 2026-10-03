# DL-388 -- `coated_material`: the substrate in the coat's refracted frame

Slice `debt-dl388`, 2026-10-02, branched from `master` `1691e3f28`.

## 1. The defect

`coated_material` evaluated the substrate BRDF at the OUTER (unrefracted)
directions and recycled the trapped light with the Lambertian factor
`1 / (1 - E_ret R)`:

    f = c f_coat + ( c T(c_i) T(c_o) a_i a_o rec / eta^2 + 1 - c ) f_b(wi, wo)

That is exact for a Lambertian and only for a Lambertian
([DL342_COATED_ABSORBING_COAT.md](DL342_COATED_ABSORBING_COAT.md) section
4).  Two separate errors hide in it for any other substrate:

* **The frame.**  A glossy lobe evaluated at the outer directions keeps its
  outer width, so the `1/eta^2` that is a pure solid-angle compression for a
  Lambertian removes `1 - 1/eta^2` of the lobe's energy: a glossy GGX
  (diffuse 0, F0 .5, alpha .16) under a clear coat read **0.727** of the
  composite at normal incidence.  A GGX diffuse lobe's `(1 - A(mu))` was
  read at the outer grazing angle where the light really leaves the
  substrate near the critical angle (~42 deg inside at eta 1.5).
* **The recycling shape.**  Amplifying `f_b` itself by `1/(1 - q)` recycles
  the part of the substrate's reflection that escapes on the FIRST bounce.
  Harmless for a Lambertian (whose return is the same from every
  direction), wrong for a lobe: a mirror-like lobe at normal incidence
  sends almost nothing into the trapped field.

The DL-24 composite of the same physical layers is the reference.

## 2. Derivation

**Single bounce.**  Radiance `L` from outer direction `wi` (cosine `c_i`)
enters the smooth coat with transmittance `T(c_i)`.  Inside, the `n^2` law
gives radiance `eta^2 T L` in the solid angle `dw' = c_i dw / (eta^2 mu_i)`
about the refracted `wi'` (internal cosine `mu_i`), so the substrate's
irradiance is `T(c_i) L c_i dw` -- flux is conserved.  The substrate
returns `f_b(wi', wo')` of it toward `wo'`, one traversal each way
attenuates by `a(mu) = exp(-tau/mu)`, and leaving divides radiance by
`eta^2`:

    f_1(wi, wo) = T(c_i) T(c_o) a(mu_i) a(mu_o) f_b(wi', wo') / eta^2

So the `1/eta^2` stays exactly where it was -- it IS the solid-angle
compression -- and only the substrate's arguments move.  Reciprocal
because `T`, `a` and `f_b` are.  A perfect mirror check:
`INT f_1 c_o dw_o = T(c_i) T(c_o)` with `c_o = c_i` (the outer mirror), as it
must be.

**Recycling.**  What the substrate sends up from `wi'` either escapes
(weight `Psi(mu) = (1 - F_in(mu)) a(mu)` per direction; `F_in = 1` below the
critical cosine) or is returned to it by the coat's underside
(`Phi(mu) = F_in(mu) a(mu)^2`).  Define

    g(mu)   = INT f_b(w, u) mu_u Phi(mu_u) du        first-bounce return
    e_1(mu) = INT f_b(w, u) mu_u Psi(mu_u) du        first-bounce escape
    rho(mu) = INT f_b(w, u) mu_u du                  directional albedo

and treat the returned field as a reservoir that re-enters the substrate
cosine-weighted: its loop gain is `Q = 2 INT rho mu Phi`, its escape per pass
`E = 2 INT rho mu Psi`.  The reciprocal kernel that in-couples AND
out-couples through the substrate's own `g` is

    M(wi', wo') = g(mu_i) g(mu_o) E / ( pi G (1 - Q) ),   G = 2 INT g mu Psi

and the full substrate term is `T T a a [ f_b(wi', wo') + M ] / eta^2`.

* Energy M carries out of `wi'`: `g(mu_i) E / (1 - Q)`.  Since
  `E + Q <= 2 INT rho mu <= 1`, that is at most `g(mu_i)`: the layer never
  returns more than the substrate reflects, for ANY substrate, given `g`.
* A lossless albedo-1 substrate of ANY lobe shape sums to exactly 1
  (`E = 1 - Q`, `g = 1 - e_1`).
* For a single separable lobe `f_b = s(w) s(u)` (a Lambertian) `M` is
  exactly the rest of the interreflection series, `f_b q / (1 - q)`; for a
  Lambertian it is the pre-DL-388 `1/(1 - E_ret R)`.

**Two alternatives ruled out on measurement** (scratch harness, coated /
composite): the old shape `f_b(wi', wo') / (1 - q)` with any constant `q`
reads 1.2-2.0x on a glossy metal (it recycles the first-bounce escape); a
separable proxy built from the directional albedo alone,
`rho(i) rho(o) q / (pi R (1 - q))`, returns up to 60 % of a mirror-like lobe
that in fact escapes -- **1.4x** on a smooth metal at normal incidence,
and a white glossy metal's furnace reads 1.5.  The in-coupling must be the
substrate's own `g`.

## 3. Implementation

Per substrate class (CoatedMaterial's allowlist is closed):

| substrate | model | why |
|---|---|---|
| Lambertian | pre-DL-388 expression verbatim | it IS the formula (exact, bit-identical) |
| Oren-Nayar | refracted frame, old `1/(1 - E_ret R)` on the refracted `f_b` | diffuse enough: within 1 % of the composite |
| GGX | refracted frame, lobe reservoir | the case the row is about |
| fabric, weave | pre-DL-388 outer frame | out of scope (DL-417) |

**GGX summary** (`CoatedBRDF.cpp`, anonymous namespace).  GGX is
`f_ss + f_ms + f_d`; `f_ms` and `f_d` are separable, so their `g`, `e_1`,
`rho` are 1-D integrals of `(1 - Ess(u))` and `1 - (1-u)^5` against `Phi`,
`Psi`.  The glossy `f_ss` uses a once-per-process table: for 24 roughness
nodes (uniform in `sqrt(alpha)`) and 32 view-cosine nodes ending at 1, a
32-bin histogram over the outgoing cosine of `INT f_ss mu du`, by
deterministic VNDF quadrature (`E[F G2/G1]`, 32x32 strata) for the two
Schlick basis functions `1` and `(1 - w.m)^5` (Schlick's F0 is exact;
conductor / thin-film are projected on it through `Directional(1)`), each
row calibrated to `LookupEssG2`, plus the bin's mass centroid.  Per
`(alpha, eta, tau)` the bins are weighted by `Phi`/`Psi` interpolated at
the centroid and the hemispherical integrals are taken on a fine sub-grid
with the exact weights (so `G` is the integral the kernel's out-coupling
really performs).  Clear coats read that basis from a second table over
(roughness node, 81 eta nodes), blended bilinearly; absorbing / tinted
coats build it directly; both sit behind a 4-entry per-thread memo.
Colours (`c`, `F0`, `F_ms`) enter linearly per channel.  The substrate is
evaluated through its own `value` at a record whose view is refracted and
whose ambient index is the coat's (`CoatedBRDF::MakeSubstrateRecord`), so a
GGX conductor sees the medium it is buried in.

**Sampler.**  For GGX, half the substrate branch samples the base SPF in
the refracted frame and refracts the draw out (`CoatedSPF::ScatterImpl`);
outside the escape cone the draw is trapped and emits nothing.  `PdfImpl`
adds `omega q_int(wo') cos_o / (eta^2 mu_o')`.  `kray = value cos / Pdf` as
before.  The selection reuses the branch uniform, so Lambertian / Oren-Nayar
/ fabric / weave draw exactly as before.

`albedo` (the OIDN AOV) is the reservoir's own directional albedo
`F + T a (e_1 + g E / (1 - Q))`; `hemisphericalAlbedo` is
`r_e + eta^2 (2 INT mu Psi e_1 + G E / (1 - Q))` (both reduce to the old
closed forms for a Lambertian).  No HWSS override: `CoatedSPF` still
stamps the aggregate hero density and `valueNM` is the aggregate, so the
ISPF fallback stays exact (`HWSSCompanionKrayTest` 193/0).

## 4. Red-proof: `CompositeEnergyConservationTest` K3-K7

`--coated-only` (~15 s).  Smooth 1.5 coat (alpha 0.001), thickness 1,
sigma_t = coat optical depth; 8 x 20000 position-jittered draws per arm.
Master = the base `Coated*` sources rebuilt under the new test: **120
passed, 50 failed**; DL-388: **172 / 0** (full suite 342 / 0).

Coated / composite, directional albedo:

| substrate | sigma_t | theta | master | DL-388 (+- sem) |
|---|---|---|---|---|
| GGX diffuse .8, F0 .04, alpha .16 | 0 | 0 / 60 | 0.995 / 1.002 | 1.005 / 1.010 |
| | 0.2 | 0 / 60 | **0.946** / 0.970 | 0.996 / 1.002 |
| | 0.5 | 0 / 60 | **0.944** / 0.974 | 0.996 / 1.001 |
| GGX glossy diffuse 0, F0 .5, alpha .16 | 0 | 0 / 45 / 70 | **0.727** / 0.807 / 0.968 | 1.013 / 1.025 / 1.041 |
| | 0.2 | 0 / 45 / 70 | **0.637** / 0.723 / 0.930 | 1.004 / 1.003 / 1.012 |
| | 0.5 | 0 / 45 / 70 | 0.645 / 0.735 / 0.949 | 1.005 / 0.993 / 1.005 |
| GGX rough glossy F0 .5, alpha .5 | 0 / 0.2 / 0.5 | 0 | 0.943 / 0.921 / 0.933 | 1.008 / 1.007 / 1.013 |
| GGX mixed diffuse .5, F0 .5, alpha .3 | 0 / 0.2 / 0.5 | 0 | 0.891 / 0.804 / 0.799 | 1.024 / 1.013 / 1.009 |
| GGX smooth metal F0 .9, alpha .05 | 0 | 0 / 45 / 70 | 0.941 / 0.947 / 0.956 | 1.005 / 1.016 / **1.072** |
| | 0.2 | 0 / 45 / 70 | **0.606** / 0.627 / 0.802 | 0.996 / 0.994 / 1.021 |
| | 0.5 | 0 / 45 / 70 | 0.550 / 0.584 / 0.834 | 0.998 / 0.993 / 1.006 |
| Oren-Nayar rho .8, sigma .8 | 0 / 0.2 / 0.5 | 70 | 1.038 / 1.035 / 1.024 | 0.997 / 1.005 / 1.007 |

(sem 0.002-0.016; every row of the full 57-row table is in the test log.)
Gates: K3 (diffuse-dominant GGX) `max(1 %, 5 sem)`, as the Lambertian rows;
K4 theta 0 / 45 `max(3 %, 5 sem)`, theta 70 `max(3 + 6 sigma_t %, 5 sem)`
except the two lobes that trap light near the critical angle (glossy alpha
.16, smooth metal), pinned [0.95, 1.20] (DL-423).

**K5 furnace**, clear coat over white GGX metals (F0 1), 8 x 50000:

| alpha | 0 deg | 45 | 70 | 85 |
|---|---|---|---|---|
| 0.05, master | **1.053** | **1.038** | 0.921 | 0.857 |
| 0.05, DL-388 | 1.0039 | 1.0055 | 1.0005 | 1.0003 |
| 0.4, master | **1.016** | 1.000 | 0.986 | 0.987 |
| 0.4, DL-388 | 0.9936 | 0.9973 | 0.9997 | 0.9997 |

Gate [0.985, 1.012]; the residual is the substrate's own GGX directional
albedo (1.003 at normal incidence for F0 1, alpha .05) and the tables'
escape/return split.

**K6** the AOV against the furnace it summarises (GGX rows, theta 0/45,
`max(2 %, 5 sem)`; master 0.947-0.972).  **K7** a double-sided
`indexedmesh_geometry` quad, coated GGX, back-wound vs front-wound, under a
directional light (NEE reads `value`): PT and BDPT ratio 1.0000 (a
consistency pin -- the record is built from the ray-facing frame).

Also: `SPFBSDFConsistencyTest`'s MC furnace divided by the attempts that
EMITTED rather than all attempts, over-reading any sub-density sampler by
`1/P(emit)` (the new refracted branch: 18-20 %); it now divides by every
attempt (Fabric_GGXaniso moved 2.24 % -> 0.10 % as a side effect).
`SPFPdfConsistencyTest` Coated_GGX: Part 2's hemisphere band is a sanity
bound (0.25); Part 2b gates the mass against the measured emission
probability (0.8195 vs 0.8211 at 30 deg).  `LayeredWhiteFurnaceTest` pins
14 / 15 re-measured (80 deg 0.568 -> 0.688 white, 0.455 -> 0.528 red).

## 5. Appearance

Shipped scenes binding `coated_material` over a non-Lambertian base: two.
PT pel at each scene's own film and spp, n = 3 per build, Sobol' salted per
replicate, OIDN off, linear float capture, Rec.709 luminance:

| scene / region | master | DL-388 | delta | t |
|---|---|---|---|---|
| `lacquer_and_rain_still_life` brass box (`mat_worn_lacquer_brass`, GGX alpha .24, oil tint) front face | 0.12694 +- 0.00004 | 0.13206 +- 0.00004 | **+4.03 %** | +156 |
| same, box top rim (grazing) | 0.08164 +- 0.00009 | 0.09882 +- 0.00019 | **+21.1 %** | +138 |
| same, candlestick (Lambertian control) | 0.02536 | 0.02537 | +0.03 % | 0.0 |
| same, wet stones (Lambertian control) | 0.20168 | 0.20168 | 0.00 % | 0.1 |
| same, whole image | 0.06822 | 0.06845 | +0.33 % | +40 |
| `tidal_stones` stones (GGX, coat only where `interior()` > 0, i.e. submerged) | 0.18634 / 0.14275 / 0.16193 | 0.18636 / 0.14274 / 0.16192 | <= 0.01 % | <= 0.8 |
| same, whole image | 0.08916 | 0.08916 | 0.00 % | 0.2 |

The brightening is the expected sign: the outer frame read the GGX diffuse
lobe's `(1 - A)` at the outer grazing angle.  Every Lambertian-based coat
(`coated_material.RISEscene`, `rainwet_courtyard_night`) is pixel
bit-identical (single-thread pinned hash, `WeaveGapShadowTransmittanceTest`
scenehash mode).  **`add_wetness` output does not move**: the verb wraps
only a LAMBERTIAN base in `coated_material` (a GGX base is modulated in
place, an Oren-Nayar one darkened only -- `AgentChatCodecs.cpp`'s verb
description), so its coats take the verbatim path; the DL-388 ledger row's
"incl. `add_wetness` over GGX" was wrong.  Who does move: a glTF
`KHR_materials_clearcoat` over a PBR base and the Blender bridge's Coat
(both a coat over a GGX base), and any hand-authored coat over GGX /
Oren-Nayar.

## 6. Cost

`CoatedBRDF::value` / `CoatedSPF::Pdf` / `Scatter` micro-benchmark (best of
5 x 2e5, this machine, master -> DL-388):

| | value | valueNM | Pdf | Scatter |
|---|---|---|---|---|
| Lambertian, clear | 93 -> 94 ns | 90 -> 89 | 19 -> 19 | 213 -> 219 |
| Oren-Nayar, clear | 128 -> 164 | 119 -> 155 | 19 -> 19 | 275 -> 327 |
| GGX, clear | 202 -> 217 | 188 -> 206 | 40 -> 100 | 400 -> 430 |
| GGX, grey absorbing | 213 -> 224 | 192 -> 209 | 41 -> 101 | 410 -> 436 |
| GGX, tinted | 235 -> 292 | 200 -> 216 | 40 -> 101 | 429 -> 488 |

The `Pdf` rise is the second (refracted-frame) base `Pdf` plus a
1120-byte record copy.  A roughness that changes every evaluation (a
textured GGX roughness) misses the memo: 255 ns per clear-coat `value`
(the table blend), but ~3.5 us under an absorbing / tinted coat (a direct
basis build).  One-time: the two tables build in 16 ms on the first coated-GGX evaluation of a process.

## 7. Residuals

* **DL-423** -- light a SMOOTH glossy lobe returns near the critical
  angle stays near it (the mirror keeps its polar angle), bouncing between
  total internal reflection and the substrate and losing `1 - rho` and
  `a^2` each round trip, while the reservoir lets it escape after one
  re-randomisation: smooth metal (alpha .05) +7 % at 70 deg (+24 % at 85,
  scratch), glossy alpha .16 +4 % at 70 deg, clear coat.  Pinned in K4.
* **DL-417** -- fabric and weave substrates keep the outer-frame model.
* Approximations inside the GGX summary (recycled term only; the single
  bounce is the substrate's own `value`): anisotropic roughness uses
  `sqrt(alphaX alphaY)`; conductor / thin-film Fresnel is projected on the
  Schlick basis through `Directional(1)`; the eta axis of the clear-coat
  table is blended linearly between nodes 0.025 apart.
* A rough coat's transmission is still the macro-surface Fresnel, as before.
