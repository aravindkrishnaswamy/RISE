# DL-342 -- `coated_material` under an absorbing coat

Slice `debt-dl342`, 2026-10-02, branched from `master` `69ed3b54b`.

## 1. The defect

`coated_material`'s layered response is the Saunderson / Weidlich-Wilkie
form ([CoatedLayer.h](../src/Library/Materials/CoatedLayer.h)):

    f(wi, wo) = T(ci) T(co) a(mu_i) a(mu_o) f_base(wi, wo)
                -----------------------------------------
                       eta^2 ( 1 - E_ret R )

with `a(mu) = exp(-tau / mu)` one traversal of the coat at the INTERNAL
cosine `mu`, `tau = coat_absorption * coat_thickness - ln(coat_tint)` the
normal-incidence optical depth, `R` the substrate's hemispherical albedo
and `E_ret` the fraction of the substrate's diffuse up-welling field one
round trip (up the film, Fresnel / total internal reflection at the
coat's underside, down the film) returns to the substrate.

The entry and exit factors `a(mu_i) a(mu_o)` were already exact.  `E_ret`
was not: the code used `r_i * a(mu_bar)^2`, the clear-coat internal
diffuse reflectance times ONE Beer factor at `mu_bar` = the refraction of
an outer cosine 0.5.  The exact quantity is

    E_ret = INT_0^1 2 mu a(mu)^2 F_in(mu) dmu

and the weighting by `F_in` is what moves it: `F_in = 1` for every
`mu` below the critical cosine `sqrt(1 - 1/eta^2)` (0.745 at eta 1.5),
i.e. the reflected part of the trapped field is DOMINATED by total
internal reflection at the LONGEST paths, which absorb most.  No single
mean cosine can fix that.

| eta | tau | old `r_i a(mu_bar)^2` | exact `E_ret` |
|---|---|---|---|
| 1.5 | 0 | 0.5963 | 0.5963 |
| 1.5 | 0.2 | **0.3654** | **0.2583** |
| 1.5 | 0.5 | 0.1752 | 0.0895 |
| 1.5 | 1 | 0.0515 | 0.0181 |
| 1.5 | 2 | 0.0044 | 0.0010 |
| 1.33 | 0.2 | 0.2786 | 0.1866 |
| 1.33 | 0.5 | 0.1264 | 0.0579 |

(4e5-point midpoint quadrature.)  `E_ret` too large makes the recycling
denominator too small, so the coat read BRIGHT, by up to 15 % in
directional albedo (table in section 4).

`hemisphericalAlbedo{,NM}` carried the same stand-in for its ENTRY
(one traversal at outer cosine 0.5), and `albedo()` (the OIDN AOV)
charged its exit a second traversal at the VIEW's angle, which the
trapped diffuse field does not take.

## 2. The exact series

For a smooth interface over a Lambertian substrate every bounce
re-randomises the field to a cosine distribution, so the interreflection
series is geometric and three hemispherical numbers close it:

    E_ret  = INT_0^1 2 mu a(mu)^2 F_in(mu) dmu          returned per round trip
    escape = INT_0^1 2 mu a(mu) (1 - F_in(mu)) dmu      leaves per round trip
    entry  = INT_0^1 2 c (1 - F(c)) a(mu(c)) dc         white-sky entry

with `escape == entry / eta^2` by reciprocity.  With them:

* `value` (directional, both pipes, both reflection and the DL-23
  transmission branch): unchanged structure, `E_ret` replaces
  `r_i a(mu_bar)^2` in the recycling factor.
* `hemisphericalAlbedo` = `r_e + R entry escape / (1 - E_ret R)`.
* `albedo` (AOV) = `F(co) + T(co) a(mu_o) R escape / (1 - E_ret R)`.

At `tau = 0` the three are `r_i`, `1 - r_i`, `1 - r_e` and every changed
expression reduces to the old one -- BIT FOR BIT in `value`/`valueNM`; `albedo` (the
OIDN guide AOV) and `hemisphericalAlbedoNM` move by 1 ulp because fast-math
reorders the new product (review, 2026-10-02), so rendered pixels are unchanged
but denoised clear-coat output is not guaranteed bit-identical (`OpticalDepth` is exactly
0 for an untinted, non-absorbing coat and `InteriorDiffuseTransport`
returns those values untouched).  `add_wetness` emits exactly such a coat
(`coat_thickness`/`coat_absorption`/`coat_tint` left at their defaults),
as does the glTF clearcoat importer, so neither moves.

**Quadrature** (`CoatedLayer::InteriorDiffuseTransport`).  Split at the
critical cosine.  Below it the integrand is `mu exp(-2 tau/mu)`; above it
the substitution `mu = mu(c)` (`mu dmu = c dc / eta^2`, `c` the outer
cosine Snell pairs with `mu`) turns `F_in(mu)` into the external `F(c)`
and removes the square-root kink at the critical angle.  Both pieces are
smooth; the 21-point Gauss-Legendre set `CoatedLayer` already carries is
accurate to ~1e-6 for eta in [1, 3] and tau in [0, 10] (12 nodes would
give 4e-6; measured against a 4e5-point midpoint rule).  Each piece is
evaluated as a DEFICIT from its `tau = 0` value, so the result is
continuous into `tau = 0` rather than jumping by the quadrature error.

**Cost.**  The quadrature is 42 `expm1` + 21 Fresnel evaluations, ~170 ns.
A per-thread 4-entry memo keyed on `(eta, tau)` answers the repeat
queries a coat with uniform painters makes (one `tau` grey, three tinted).
`CoatedBRDF::value` micro-benchmark (this machine, best of 5 x 2e5):
clear coat 76-94 ns (run-to-run spread under load), grey absorbing 113 ns,
tinted 138 ns; uncached the latter two read 242 and 564 ns.  A textured
`coat_thickness` / `coat_absorption` / `coat_tint` misses the memo and pays
the quadrature per evaluation per distinct channel.

`CoatedSPF` needed no change: its `kray` is `value * cos / Pdf` and its
`Pdf` does not involve the recycling term, so `kray * Pdf == value * cos`
holds by construction (`SPFBSDFConsistencyTest`'s Coated rows, green).
`CoatedSPF` has no `EvaluateKrayNM`/`EvaluateLobeFNM` override (see the
note at the end of `CoatedSPF.cpp`); the default fallback re-evaluates
`valueNM`, which carries the fix.

## 3. Red-proof: `CompositeEnergyConservationTest` Section K

`--coated-only` runs it alone (~7 s).  All committed; pre-fix numbers are
the base sources (`69ed3b54b`) rebuilt under the new test.

* **K1** -- `value` / `valueNM` at four off-specular pairs against
  Section G's independent `ClosedFormLayered`, Lambertian (0.8, 0.2, 0.2)
  substrate, sigma_t in {0, 0.2, 0.5, 1, 2}, plus a tinted
  (0.9, 0.6, 0.3), non-absorbing coat.  Gate 0.1 %.

  | sigma_t | pre ch0 (rho .8) | pre ch1 (rho .2) | pre NM | post worst |
  |---|---|---|---|---|
  | 0 | +0.0002 % | +0.001 % | +0.001 % | +0.001 % |
  | 0.2 | **+12.1 %** | +2.31 % | +2.62 % | +0.002 % |
  | 0.5 | +7.98 % | +1.78 % | +2.01 % | +0.003 % |
  | 1 | +2.79 % | +0.68 % | +0.77 % | +0.010 % |
  | 2 | +0.30 % | +0.15 % | +0.15 % | +0.080 % |
  | tint (.9,.6,.3) | +16.1 % / +10.2 % / +2.2 % (ch0/1/2) | | | +0.0005 % |

  The residual post-fix figure at sigma_t 2 is the coat GGX lobe at
  alpha 0.001 off-specular and the closed form's own midpoint error.
* **K2** -- `hemisphericalAlbedo` against the white-sky closed form
  `r_e + R entry^2 / (eta^2 (1 - R E_ret))`; pre-fix +9.9 % at sigma_t 0.2
  (white), post-fix |error| <= 4e-7.
* **K3** -- directional albedo, coated vs the composite of the same
  physical layers (8 x 20000 position-jittered draws each).

Totals: **32 passed, 61 failed** on the base sources; **93 / 0** on the
fix.  Full suite 263 / 0.

## 4. Coated vs exact vs composite (directional albedo, eta 1.5, white substrate)

Lambertian white substrate (the coated model is exact here; "exact" is
the closed form `F(ci) + T(ci) a(mu_i) escape / (1 - E_ret)`):

| sigma_t | theta | coated pre | coated post | exact | composite (+- sem) | post/composite |
|---|---|---|---|---|---|---|
| 0 | 0 | 1.0004 | 1.0004 | 1 | 0.9991 +- 0.0019 | 1.0013 |
| 0 | 60 | 0.9997 | 0.9997 | 1 | 1.0011 +- 0.0021 | 0.9986 |
| 0.2 | 0 | **0.4388** | 0.3813 | 0.3808 | 0.3809 +- 0.0009 | 1.0010 |
| 0.2 | 60 | 0.4501 | 0.3980 | 0.3983 | 0.4003 +- 0.0006 | 0.9944 |
| 0.5 | 0 | 0.2021 | 0.1869 | 0.1863 | 0.1868 +- 0.0005 | 1.0005 |
| 0.5 | 60 | 0.2259 | 0.2130 | 0.2132 | 0.2147 +- 0.0004 | 0.9917 |
| 1 | 0 | 0.0890 | 0.0874 | 0.0867 | 0.0873 +- 0.0004 | 1.0008 |
| 1 | 60 | 0.1256 | 0.1243 | 0.1246 | 0.1257 +- 0.0007 | 0.9893 |
| 2 | 0 | 0.0461 | 0.0461 | 0.0455 | 0.0463 +- 0.0004 | 0.9965 |
| 2 | 60 | 0.0923 | 0.0923 | 0.0925 | 0.0944 +- 0.0008 | 0.9772 |

Pre-fix coated/composite read **1.152** (sigma_t 0.2, theta 0), 1.125,
1.082, 1.052.  Post-fix coated sits on the closed form (within 0.3 %
everywhere); the composite's theta-60 rows read slightly above both by
1-2 sem, its own Monte-Carlo walk.

GGX substrate (diffuse 0.8, Schlick F0 0.04, alpha 0.16) -- no closed
form; the composite is the reference:

| sigma_t | theta | coated/composite pre | post |
|---|---|---|---|
| 0 | 0 | 0.995 | 0.995 |
| 0 | 60 | 1.002 | 1.002 |
| 0.2 | 0 | 1.033 | **0.946** |
| 0.2 | 60 | 1.043 | 0.970 |
| 0.5 | 0 | 0.994 | **0.944** |
| 0.5 | 60 | 1.009 | 0.974 |
| 1 | 0 | 0.974 | 0.963 |
| 1 | 60 | 0.997 | 0.991 |
| 2 | 0 | 0.982 | 0.982 |
| 2 | 60 | 1.003 | 1.003 |

**The GGX rows get WORSE at sigma_t 0.5, and that is a second, separate
error the first one was hiding -- opened as DL-388, pinned in K3 at
[0.92, 1.02].**  `coated_material` evaluates the substrate BRDF at the
UNREFRACTED (outer) directions.  For a Lambertian that is exact (the
`1/eta^2` compression accounts for it); for a GGX substrate the diffuse
lobe's `(1 - A(o))` factor is read at the OUTER grazing angle, where the
light actually leaves the substrate at the critical angle (~42 degrees
inside), so the first-pass escape is undercounted.  A separable-lobe
analysis (`(c/pi) g(mu_i) g(mu_o)`, `g = 1 - (1-mu)^5`, F0 0) reproduces
the composite to 0.2 % and the coated model to 0.6 %, and isolates it:
escape 0.3107 (outer frame) vs 0.3215 (internal frame) at sigma_t 0.2.
At sigma_t 0 the recycling term over-reads by about the same amount
(`E_ret R` 0.4327 vs the true `q` 0.4131) and the two cancel (0.999);
any absorption suppresses recycling, the cancellation fails, and the
residual appears.  Pre-fix the over-bright `E_ret` happened to cancel it
again.  A glossy-dominant substrate (GGX diffuse 0, F0 0.5) reads 0.72
of the composite even at sigma_t 0 -- the same frame approximation,
independent of absorption.  Fixing it is a model change (refracted-frame
substrate evaluation, with its own Jacobian and sampler consequences),
not part of this row.

## 5. Shipped scenes

Every shipped scene binding `coated_material` with a non-zero optical
depth (grep of `scenes/` for `coat_absorption` / `coat_tint` /
`coat_thickness`): two scenes, three materials.  PT pel at each scene's
own film and spp, n = 3 per build, Sobol' value-salted per replicate,
linear float capture (no OIDN), Rec.709 luminance region means:

| scene / object | pre | post | delta | t |
|---|---|---|---|---|
| `Tests/Materials/coated_material` -- `mat_lacquer` sphere (amber tint, absorption 1.8, thickness 0.05) | 0.09480 +- 0.00024 | 0.09125 +- 0.00064 | **-3.74 %** | -9.0 |
| same, clear `mat_wet` sphere (control) | 0.25243 +- 0.00013 | 0.25237 +- 0.00007 | -0.02 % | -0.7 |
| same, whole image | 0.35586 | 0.35578 | -0.02 % | |
| `FeatureBased/Materials/lacquer_and_rain_still_life` -- hardware box (`mat_worn_lacquer_brass`, oil tint) | 0.17994 +- 0.00009 | 0.17935 +- 0.00009 | -0.33 % | -8.0 |
| same, candlestick (`mat_lacquer_wood`, curvature-driven thickness, absorption 1.8, amber) | 0.02691 +- 0.00026 | 0.02679 +- 0.00024 | -0.45 % | -0.6 |
| same, wet stones (clear coat, control) | 0.15978 +- 0.00001 | 0.15980 +- 0.00003 | +0.01 % | +1.0 |
| same, whole image | 0.06821 | 0.06818 | -0.05 % | |

Small, because the shipped absorbing coats sit over dark (wood, brass)
substrates where recycling carries little; the sign is the expected one
everywhere.  `add_wetness` output and every clear coat are bit-identical
by construction (section 2).

## 6. Residuals

* **DL-388** (opened) -- substrate evaluated in the outer frame; section 4.
  FIXED 2026-10-02 (`debt-dl388`): Oren-Nayar and GGX substrates are now
  evaluated in the coat's refracted frame with a first-bounce-return
  recycling term -- [DL388_COATED_REFRACTED_FRAME.md](DL388_COATED_REFRACTED_FRAME.md).
* The `1/eta^2`-exact claim is for a SMOOTH coat over a Lambertian; a
  rough coat's transmission is still the macro-surface Fresnel, as before.
