# r254 — resolved turbulence and momentum-damping audit of r251

2026-10-19. **Resolved relative fluctuation intensities are deficient, especially in the
upper plume. The Vreman constant and formula match FDS. A limiter-caused damping
rate cannot be recovered from the retained data.** No solver change, solver
advance, replay, or new simulation was made. Tier 10 remains unlaunched.

## Resolved fluctuations

All 235 retained VDB frames cover 10.466040127212–25.091040127212 s at
0.0625 s spacing. Endpoints receive half weight. The observer reads the native
scalar `(x,y,z)` and exported velocity `(x+1,y+1,z+1)` coordinates, following
`WriteFrame` in `FireSequenceTest.cpp`; it does not sample the shifted
velocity grid at scalar indices. Native dimensions are 69×69×106, dx =
0.030593116067411014 m. No rendered movie pixels are used.

The composition/viscosity calculation additionally uses r253’s 214 checkpoint
paths (204 have nonzero endpoint-interpolated weights), with its same
10.466040127157–25.091040127212 s quadrature. The source of each measurement
is distinguished in the CSV names. Comparing checkpoint and frame estimates
at the six tabulated heights changes centerline w RMS ratios by up to 11.2%
and temperature RMS ratios by up to 1.9%; these are sampling sensitivities,
not confidence bounds. The 235-frame result is the primary RMS measurement.

At each fixed cell compute the temporal mean and variance, then average those
**pointwise variances** over radial annuli. Averaging instantaneous annuli
before computing variance would erase non-axisymmetric turbulence. Centerline
is the single central cell. Each field's half-width is the first radius where
its annular time mean falls to half its centerline mean, interpolated between
annuli of width dx. The half-width RMS is the square root of the interpolated
annular variance, divided by that field's local mean. Velocity and temperature
half-widths are distinct locations.

Temperature comparisons use θ = T−300 K and θ_rms/mean(θ), matching the
experimental convention. The CSV also supplies T_rms/mean(T), dimensional RMS,
mean temperature rise and mean velocity. θ and T have identical fluctuations.

| z (m) | Center w RMS/mean | At w half-width | Center θ RMS/mean | At θ half-width | w half-width (m) | θ half-width (m) |
|---:|---:|---:|---:|---:|---:|---:|
| 0.505 | 0.239 | 0.443 | 0.251 | 0.487 | 0.0832 | 0.0671 |
| 0.994 | 0.254 | 0.446 | 0.250 | 0.456 | 0.1226 | 0.1018 |
| 1.514 | 0.238 | 0.431 | 0.236 | 0.437 | 0.1528 | 0.1302 |
| 2.004 | 0.221 | 0.424 | 0.242 | 0.442 | 0.1819 | 0.1516 |
| 2.493 | 0.228 | 0.421 | 0.246 | 0.456 | 0.2046 | 0.1737 |
| 3.013 | 0.200 | 0.424 | 0.266 | 0.491 | 0.2252 | 0.1997 |

**Primary comparison:** Shabbir & George, *Experiments on a round turbulent
buoyant plume*, JFM 275 (1994), 1–32,
[DOI 10.1017/S0022112094002260](https://doi.org/10.1017/S0022112094002260),
[author-hosted original PDF](https://turbulence-online.com/Publications/Papers/SG94.pdf).
Printed p 14, Table 2 gives centerline ratios **w RMS/Wc = 0.33**, **temperature
RMS/ΔTc = 0.40**, and half-width growth ratios 0.107 and 0.100 respectively.
Figure 7(a,d), printed p 17, shows substantially larger ratios off axis:
approximately **0.5–0.65 for w** and **0.6–0.8 for temperature** at their mean
half-widths. These broad visual ranges use the plotted second moments, Table
2's half-widths, and division by the local half-centerline mean; they are not
published tabulated RMS ratios or precision digitizations. The centerline
numbers, which determine the comparison, are directly tabulated.

At 3.013 m, r251's centerline velocity ratio is **39.4% below** 0.33 and its
temperature ratio **33.5% below** 0.40. Half-width fluctuations are also low.
The upper plume has resolved motion, with smaller normalized intensities than
this experimental plume. At 3.013 m its dimensional RMS values are 0.637 m/s
and 18.954 K. Because the mean velocity and temperature rise are already
excessive, low normalized intensities alone do not establish a deficit of
absolute temperature variance or identify numerical damping as its cause.
Using r253's virtual origin 0.03011 m, the upper w/θ half-width growth ratios
are 0.0755/0.0670, about 29%/33% below Table 2. This supports the measured
entrainment deficit and narrow plume; it does not identify its numerical cause.

This is a comparison with a fully developed heated-air turbulent plume, not a
matched methane-fire experiment. Lower flame rows are context, not a universal
self-similarity test. LES cell filtering, a finite 14.625 s record and temporal
sampling omit some experimental variance: 16 Hz samples have an 8 Hz Nyquist
frequency and no saved higher-frequency anti-alias guarantee. Treat these as
resolved retained-sample RMS values, not total turbulence or a formal
confidence interval. r251's accepted puffing also varies between subwindows.

## Resolved turbulent mass flux

A fixed R=0.8 m cylinder is used, matching r253. The checkpoint observer
separately evaluates the exact Reynolds decomposition of sampled axial flux:

    integral mean(rho*w) dA
      = integral mean(rho)*mean(w) dA + integral mean(rho'*w') dA.

The height profile reports all three terms and the covariance fraction.

| z (m) | Total axial mass (kg/s) | Mean-product part (kg/s) | Covariance (kg/s) | Covariance / total |
|---:|---:|---:|---:|---:|
| 0.505 | 0.121822 | 0.127841 | -0.006019 | -4.941% |
| 0.994 | 0.241484 | 0.248463 | -0.006979 | -2.890% |
| 1.514 | 0.369818 | 0.376986 | -0.007168 | -1.938% |
| 2.004 | 0.486183 | 0.493732 | -0.007549 | -1.553% |
| 2.493 | 0.602480 | 0.610795 | -0.008315 | -1.380% |
| 3.013 | 0.742357 | 0.752020 | -0.009663 | -1.302% |

Hot, rising fluid tends to be less dense, so the covariance is negative.
At 3.013 m it is −0.009663 kg/s, −1.30% of total axial mass flux.

This fraction is **not the turbulent share of entrainment**. In a Boussinesq
plume rho' approaches zero while turbulent entrainment remains finite.
Entrainment is lateral incorporation across a plume boundary; its decomposition
would require a specified moving boundary or lateral covariance/control-surface
analysis. No causal percentage of total entrainment is inferred from axial
rho' w'. The retained axial covariance is reported as a measurable transport
indicator only.

## Momentum limiter activity and damping

The active kernel is `fct_compatible_stage_rate` in
`FireProductionAdvectionMac.mm`; its CPU counterpart is
`EvaluateFireProductionCompatibleFCTMomentumCPUImpl` in
`FireProductionTransport.cpp`. The live resident owner dispatches it for
stage and final Heun rates. It computes

    Fg = Fg_low + alpha * delta_Fg + Jg
    momentum flux = I_i(Fg) * (u_i,L + u_i,R)/2.

There is **no separate momentum-velocity reconstruction limiter**. Its direct
blend to donor-cell velocity is structurally zero on all interior faces.
The scalar alpha affects the mass carrier inherited by momentum; it is not a
coefficient blending the centered velocity factor toward upwind velocity.

| Height range | Independent velocity donor blend: fraction / mean | Inherited scalar alpha<1 fraction | Mean inherited low-order blend 1−alpha |
|---|---:|---:|---:|
| Every interior plume height | 0 / 0 (operator definition) | Not retained | Not retained |

The checkpoint structure saves accepted states, MAC momentum/velocity and
integrated statistics, **not** R0/R1/R2 flux pairs, face alphas, stage source
packets or intermediate Picard states. VDB exports and budget ledgers do not
supply them either. The log's limiter-*class discrepancy* concerns nonlinear
convergence, not the spatial fraction of limited faces. Rebuilding a limiter
on an accepted snapshot using a selected timestep/source would evaluate a
new hypothetical stage and would not measure the limiter that produced r251.
No such replay is presented as historical activity.

For the interior semidiscrete compatible advection term, a face carrying mass
F between two dual cells contributes opposite momentum fluxes F(uL+uR)/2.
With the same mass continuity, the pair's kinetic-energy change is

    (uR−uL)*F*(uL+uR)/2 − (uR²−uL²)*F/2 = 0.

Thus the explicit **velocity-jump diffusion coefficient of this face flux is
zero**, including when its mass flux was limited. This is an algebraic fact,
not a measured zero for the full solver's numerical damping. Time integration,
projection, boundary/source terms and scalar mixing can change resolved kinetic
energy and turbulence. Their accumulated dissipation cannot be reconstructed
from these sparse accepted snapshots. Neither a damping power nor an
artificial viscosity for the complete r251 momentum update is identifiable
here. The design's phrase “limiter blending adds ... dissipation” must not be
read as evidence of a separate upwind velocity term in this implementation.

## Vreman against FDS

Use the version-pinned [FDS 6.11.1 Technical Reference Guide, Momentum Transport
and Pressure, “Vreman's Model”](https://github.com/firemodels/fds/blob/FDS-6.11.1/Manuals/FDS_Technical_Reference_Guide/Momentum_Chapter.tex#L155-L170).
Its formula is

    alpha_ij = d u_j / d x_i
    beta_ij = sum_m Delta_m² alpha_mi alpha_mj
    B_beta = beta11 beta22−beta12² + beta11 beta33−beta13²
             + beta22 beta33−beta23²
    mu_t = rho * c * sqrt(B_beta / sum_ij alpha_ij²),  c = 0.07.

The general LES filter width is the cube root of cell volume; this Vreman
subsection permits directional widths. r251's cubic grid makes both
conventions dx = 0.0305931161 m. The record's Cv is 0.07, **not** a Smagorinsky
Cs that should be squared again. The implementation's kinematic νt is the
formula above divided by rho; its dynamic stress uses rho*(νmolecular+νt).
Vreman needs the full velocity-gradient tensor, not only the symmetric strain.
The CPU evaluator and Metal force kernel implement the same contractions,
clamp roundoff-negative B_beta to zero and return zero for a zero gradient.
No constant, tensor-transpose, filter-width or density-factor discrepancy is
found. FDS offers other SGS models; matching its Vreman option does not mean
r251 uses every default choice of FDS.

An independent binary64 formula is evaluated on gradients reconstructed from
retained MAC velocities, alongside the existing binary32 production Vreman
evaluator. Both use the same cell-centered velocity and central-derivative
stencil; binary64 differences, including dx rounding, remain in the comparison.
Interior plume mask: R<0.8 m and instantaneous T>301 K. Profile weights are
area and retained time. This checks the expected viscosity on accepted fields;
it does not recover the unsaved intermediate-stage viscosity actually consumed.

| z (m) | Production νt (m²/s) | Independent FDS νt (m²/s) | Relative L1 difference | RMS full-gradient norm (s⁻¹) |
|---:|---:|---:|---:|---:|
| 0.505 | 0.000650921275 | 0.00065092128 | 6.66e-08 | 32.697 |
| 0.994 | 0.000613947361 | 0.000613947365 | 6.6e-08 | 29.176 |
| 1.514 | 0.000579456194 | 0.000579456199 | 6.56e-08 | 26.553 |
| 2.004 | 0.000545511398 | 0.000545511402 | 6.52e-08 | 24.612 |
| 2.493 | 0.000521755977 | 0.000521755981 | 6.58e-08 | 23.322 |
| 3.013 | 0.000498810423 | 0.000498810426 | 6.58e-08 | 22.163 |

The mean viscosities agree to rounding; the sum of absolute cellwise
differences divided by the sum of expected viscosities is about 6.6×10⁻⁸
at these heights. This is an independent arithmetic check of the formula,
not evidence that SGS viscosity is dynamically sufficient or excessive.


## Decision and deliverables

**Keep the solver unchanged for review.** The relative-intensity deficit is measured,
but the second condition—substantial inherited limiter activity in the plume—is
unmeasured, and the assumed independent upwind momentum blend is absent.
Removing alpha from the momentum carrier would break the accepted mass/momentum
compatibility that addressed tier-10 focusing. Increasing/decreasing Cv to fit
entrainment is unsupported: the coefficient and formula already match FDS.
No smallest damping fix can be selected honestly from this evidence alone.

If subsequent evidence attributes suppression to scalar over-limiting, the
smallest admissible candidate would modify the **scalar flux acceptance** while
keeping accepted-mass restriction, centered velocity factor, free-stream
preservation and the projected-Heun schedule. It must preserve the existing
admissibility bounds and pass the earlier focusing case; bypassing mass-flux
compatibility or globally lowering SGS viscosity is not justified. This is a
conditional design constraint for review, not a selected algorithm or an
implemented fix. No new instrumentation or certification machinery is added.

Observer/reducer: `tools/fire/r254_turbulence_probe.cpp` and
`tools/fire/r254_reduce_turbulence.py`. Small results and input/source hashes:
`docs/data/fire_r254/`. Full cell moments, source PDFs, build products and logs:
`/Volumes/SSD/RISE/fire_production_calibration/r254_turbulence_diagnosis/`.
