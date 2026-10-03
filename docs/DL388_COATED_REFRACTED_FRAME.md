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
row calibrated to `LookupEssG2`, plus the bin's mass centroid and spread
and a 256-sub-bin histogram with sub-bin centroids.  Per `(alpha, eta,
tau)` the bins are weighted by `Phi`/`Psi` at the centroid (the bins next
to the critical cosine `mu_c` by a uniform window of the bin's own spread,
with the exact weights), and the hemispherical integrals are taken on a
fine sub-grid with the exact weights, the cells next to `mu_c`
sub-sampled (so `G` is the integral the kernel's out-coupling really
performs).  For roughness below 0.25 a **critical patch** carries `g` and
`e` on 17 nodes over `[mu_c, mu_c + 1/16]` (section 8).  Clear coats read
that basis from a second table over (roughness node, 81 eta nodes
**uniform in `mu_c`**), blended bilinearly; absorbing / tinted coats blend
the two roughness-node bases around `alpha`, built on demand into an
8-entry per-thread ring; a 4-entry per-thread memo sits in front.
Colours (`c`, `F0`, `F_ms`) enter linearly per channel.  The substrate is
evaluated through its own `value` at a record whose view is refracted and
whose ambient index is the coat's (`CoatedBRDF::MakeSubstrateRecord`), so a
GGX conductor sees the medium it is buried in.

**Sampler.**  For GGX, half the substrate branch samples the base SPF in
the refracted frame and refracts the draw out (`CoatedSPF::ScatterImpl`);
outside the escape cone the draw is trapped and emits nothing.  0.15 of it
samples a cosine lobe about the macro normal -- the technique for the
recycled term `M`, which the base sampler cannot reach on a smooth lobe
(section 8) -- and the remaining 0.35 the base SPF at the outer
directions.  `PdfImpl` is `(0.35 q_outer + 0.5 q_int(wo') cos_o / (eta^2
mu_o') + 0.15 cos_o / pi)` on the substrate branch.  `kray = value cos /
Pdf` as before.  The selection reuses the branch uniform, so Lambertian /
Oren-Nayar / fabric / weave draw exactly as before.

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
.16, smooth metal), pinned (DL-423; [0.95, 1.20] in round 1, [0.97, 1.08]
+- 5 sem after section 8).

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
probability (0.8195 vs 0.8211 at 30 deg in round 1; section 8 adds the
same gate on the NM row).  `LayeredWhiteFurnaceTest` pins
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

Round-1 numbers (superseded for GGX by the table at the end of section 8).
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
  re-randomisation: smooth metal (F0 .9, alpha .05) +6.5 % at 70 deg (+24 %
  at 85, scratch), glossy alpha .16 +4.5 % at 70 deg, clear coat.  Pinned
  in K4 at [0.97, 1.08] +- 5 sem.  A LOSSLESS substrate is not affected
  (it sums to 1 however the reservoir equilibrates); that is the
  energy-gain part section 8 fixed.
* **DL-417** -- fabric and weave substrates keep the outer-frame model.
* Approximations inside the GGX summary (recycled term only; the single
  bounce is the substrate's own `value`): anisotropic roughness uses
  `sqrt(alphaX alphaY)`; conductor / thin-film Fresnel is projected on the
  Schlick basis through `Directional(1)`; the clear-coat table is blended
  linearly in `mu_c` (nodes 0.0118 apart) and in `sqrt(alpha)`, absorbing
  coats in `sqrt(alpha)`; the critical patch reads the spill histogram at
  an arbitrary view by displacement (exact for a mirror-like lobe).
* A white-metal furnace residual of about +1 % remains near the critical
  angle at a few (alpha, eta, theta) points (worst measured 1.0097 +-
  0.0014, alpha .002, eta 1.058, 84 deg; 1.0058 +- 0.0004, alpha .05, eta
  2.58, 45 deg), inside K5's band.
* A rough coat's transmission is still the macro-surface Fresnel, as before.

## 8. Review round 1 (2026-10-02)

The fresh review found one P1 and two P2s; all fixed in the same slice.

**P1 -- energy > 1 for clear coats with relative index 1-1.06.**  A white
GGX metal (F0 1, alpha .05) under a smooth clear coat read 1.137 at eta
1.005 / 80 deg, 1.160 at eta 1.01 / 80 deg, 1.147 at 1.0125, 1.043 at 1.04,
0.992 at the table node 1.025.  Cause: the clear-coat table blended the
coat's weights linearly between eta nodes 0.025 apart, while the critical
cosine `mu_c = sqrt(1 - 1/eta^2)` has infinite slope at eta = 1 -- one cell
spanned `mu_c` in [0, 0.22], and the blended return `g` overshot the exact
single bounce, breaking "M never returns more than the substrate
reflects".  Reachable: PT reads the outside index from the IOR stack, so a
1.35-1.45 coat underwater has eta 1.02-1.09.  Fix: the 81 eta nodes are
uniform in `mu_c` over [0, sqrt(8/9)] (a cell moves the step by 0.0118).

**P2-2 -- the direct (absorbing-coat) build read 1.011-1.024 at eta
1.06-1.2 / 80 deg.**  Cause: the histogram bin holding `mu_c` was priced at
its mass centroid, and one point cannot price a step that a lobe
straddles.  Fixed with (a) the bins next to `mu_c` priced by a uniform
window of the bin's own spread (a new second moment per bin) with the
exact weights, and (b) the hemispherical integrals sub-sampled (16x) in
the fine cells next to `mu_c`, where `Psi` rises from 0 with a square-root
edge that steepens as eta -> 1 (most of it inside one 1/256 cell at eta
1.06).  K5's band [0.985, 1.012] is kept: post-fix the direct path reads
0.996-1.004 at eta 1.06-1.2 / 70-85 deg.

**The critical patch (found while measuring P2-1).**  Once the sampler
below made smooth-substrate means measurable, a lossless white metal of
alpha <= 0.01 under a 1.5 coat read 1.06-1.09 at 75 deg (1.054 at eta 3 /
79 deg): the return `g(mu)` of a lobe narrower than the view-node spacing
follows `Phi(mu)`'s square-root step at `mu_c`, and linear interpolation
between view nodes 1/32 apart smeared it, so the exact single-bounce
escape plus the interpolated return exceeded the lobe's albedo.  Each
basis (roughness < 0.25) now carries `g` / `e` on 17 nodes over `[mu_c,
mu_c + 1/16]`, clustered quadratically at `mu_c`, built by displacement
interpolation of a 256-sub-bin spill histogram (each sub-bin's mass at its
own centroid, shifted with the view -- a mirror-like lobe moves
one-for-one with its view cosine), joined continuously to the node arrays
at the far end; `LobeDirection` and every hemispherical integral read
through the same lookup.  Point masses, not windows: near normal internal
incidence `mu = cos` compresses a lobe's angular spread by `sin theta`, so
even a 1/256 window overstated a narrow lobe's spread on the edge (eta 3:
1.054 with windows, 0.993 with centroids).  Above roughness 0.25 the patch
is the node interpolation itself.  The eta-3 LOSS the review noted
(-2.1 % to -3.4 % at 60-75 deg, alpha .05) is gone too (0.998-1.003),
from the `mu_c`-uniform nodes' resolution near `mu_c` = 0.94.

**P2-1 -- no technique for the recycled term.**  The recycled `M` is broad;
on a smooth substrate the base sampler draws only the narrow lobe, so `M`'s
weight `value cos / pdf` was unbounded: alpha .002 at 75 deg, max kray
31794, top 0.1 % of draws 28 % of the energy.  A cosine lobe now takes 0.15
of the substrate branch (`CoatedBRDF::RecycledSampleFraction`; 0 for every
model but the GGX lobe reservoir, so nothing else moves).  `Pdf` adds
`0.15 cos_o / pi`, `kray = value cos / Pdf` exactly (the reviewer's
consistency harness: 0 mismatches).  Tail, white metal under a smooth clear
coat, 400k draws:

| alpha | theta | before: max kray / top-0.1 % share | after |
|---|---|---|---|
| 0.002 | 75 | 31794 / 28 % | 6.35 / 0.63 % |
| 0.01 | 75 | (reviewer: extreme) | 8.06 / 0.80 % |
| 0.05 | 75 | -- | 6.65 / 0.67 % |

Rendered (the reviewer's 128x128 room, coated alpha .01 sphere, PT 256 spp,
OIDN off; residual std of luminance against a 5x5 median, per ring):

| ring (radius / width) | master (n=1) | before (n=1) | after (n=5, mean +- sd) |
|---|---|---|---|
| 0.38-0.43 | 0.0646 | 0.1655 | 0.078 +- 0.039 |
| 0.43-0.46 | 0.1382 | 0.1774 | 0.067 +- 0.007 |

**P3s.**  The NM Coated_GGX row of `SPFPdfConsistencyTest` now has the RGB
row's exact mass gate (`int PdfNM` over the sphere vs the measured NM
emission probability, 0.01): 0.8216 vs 0.8234 (30 deg), 0.7972 vs 0.7983
(60 deg).  `LobeReservoirEscape` no longer falls back to `E * 1e6`: it
returns `E / max(1 - Q, E)`, the exact series whenever a round trip
conserves energy and 1 (no amplification) when a diffuse colour authored
above 1 makes `E + Q > 1` -- the role the old `Recycling`'s clamp of R
played (diffuse 1.5: coated 1.338 vs bare 1.352 at 0 deg; diffuse 3:
2.602 vs 2.669).  K4's DL-423 pin is [0.97, 1.08] widened by the row's own
5 sem (measured 1.045 +- 0.004 glossy, 1.065 +- 0.012 smooth metal), was
[0.95, 1.20].  glTF: the reviewer measured `ClearcoatQuad.gltf` move R
+0.3 %, G/B +9 % (a clearcoat over a PBR GGX base).

**K5 now** (8 x 50000 draws; master = `850fd47ff`, the pre-review fix,
rebuilt under the new test: 214 passed, 13 failed; now 227 / 0
coated-only, full suite 397 / 0):

| alpha | eta | theta | before review | now |
|---|---|---|---|---|
| 0.05 | 1.005 | 70 / 80 / 85 | **1.021 / 1.137 / 1.086** | 1.000 / 0.999 / 0.996 |
| 0.05 | 1.01 | 70 / 80 / 85 | **1.025 / 1.162 / 1.138** | 1.000 / 1.000 / 0.995 |
| 0.05 | 1.04 | 70 / 80 / 85 | 1.016 / **1.043 / 1.030** | 1.002 / 1.000 / 1.000 |
| 0.05 | 1.13 | 70 / 80 / 85 | 1.014 / 1.018 / 1.014 | 1.001 / 0.998 / 0.998 |
| 0.05 | 3 | 60 / 75 / 79 / 83 | **0.979 / 0.966** / ... | 0.998 / 1.002 / 1.000 / 1.003 |
| 0.002 | 1.005 / 1.01 | 80 / 85 | **1.98 / 2.35, 2.37 / 2.45** (heavy-tailed) | 1.001 / 1.004, 1.001 / 1.003 |
| 0.002 | 3 | 60 / 75 / 79 / 83 | 1.19 / 1.18 / 1.34 / 1.37 (+- 0.1-0.15) | 1.002 / 1.005 / 0.993 / 0.994 |
| 0.4 | 3 | 60 / 75 / 79 / 83 | 0.984 / 0.986 / 0.989 / 0.990 | 0.999 / 0.999 / 1.000 / 0.999 |

A scan of 40 eta values in [1.0002, 3] x 9 incidences x 7 roughnesses
(150k draws each) reads at most 1.0115 except one alpha .002 / eta 1.058 /
86 deg point (1.0188 at 150k draws; 1.0036 +- 0.0019 at 2M).

**Appearance** re-measured (same protocol as section 5): `lacquer_and_rain`
brass box front +4.01 % (t +156), rim +21.3 %, whole image +0.32 %;
`tidal_stones` unmoved; Lambertian-coat scenes still pixel bit-identical.

**Cost now** (one harness, same machine, master `1691e3f28` -> now, n = 3,
spread < 2 %):

| GGX substrate | value | Pdf | textured roughness, absorbing coat |
|---|---|---|---|
| clear coat, alpha .05 | 201 -> 208 ns | 81 -> 138 ns | -- |
| tinted coat | 244 -> 322 ns | 81 -> 138 ns | 0.21 -> 0.30 us / value |

One-time per process: 33 ms of table builds on the first coated-GGX
evaluation.  An absorbing coat whose roughness leaves its roughness cell
pays a node build (8.6 us, or ~22 us below roughness 0.25 where the patch
is built).

