# r253 — retained r251 plume mass and heat flux diagnosis

2026-10-19. **Insufficient entrainment is the measured problem.** The lateral
ambient does not build up at the heights where r252's maximum errors occur.
At 3.013 m the plume carries 0.7424 kg/s against 1.3564 kg/s predicted; even summing
all upward mass flux in the entire plane gives only 0.7994 kg/s. The requested
convective cp flux is 26.347 kW against 26.4 kW. The hot, fast centerline is therefore
consistent with too little dilution of essentially the correct convective heat.
This identifies the flow deficiency, not a unique defective solver term.

**No solver changes or simulation runs. No wider-domain run or tier 10.** The
cold-ambient branch is followed and plume widths/eddy viscosity are below.
A small, cooling outlet-zone temperature excess is reported rather than hidden.
Stop for owner review before any numerical/model change or new run.

## Data and averaging

The offline probe reads 214 retained full-state checkpoints: 213 from r251's
`checkpoint_history` and its final checkpoint. Statistics are 10.466040127157–
25.091040127212 s,14.625000000055 s. Linear endpoint interpolation and trapezoidal
integration give 204 window nodes, maximum gap 0.103606 s. Products are evaluated
on each instantaneous state **before** time averaging, preserving resolved
velocity–temperature–density correlations. This is retained-state quadrature,
not the accepted-stage exact integration used by the fuel balance.

`r253_plume_probe.cpp` calls only the checkpoint reader, thermochemical property
getters and existing pure Vreman evaluator. It never calls a stepping method.
`r253_reduce_plume.py` produces the committed profiles and time series. The
large instantaneous table and observer build products stay in
`/Volumes/SSD/RISE/fire_production_calibration/r253_plume_diagnosis/`.

Keeping alternate retained samples changes plume mass integrals by at most
0.60%, whole-plane upward mass by 0.48%, and heat flux by 1.54% over 0.81–3.12 m.
Sampled centerline means differ from the exact accepted-integral r252 profiles
by at most 2.29% in temperature rise and 2.61% in velocity. These checks describe
sampling sensitivity; they are not new acceptance gates or confidence limits.

## 1. Ambient drift

Each side is the area mean in its outermost four cells,0.12237 m wide. Opposite
sides are kept separate; the combined band counts corners only once. The
additional far-field check uses radius r>0.8 m about the burner axis. The domain
is 2.110925 m wide. Heights are native cell centers, not interpolated stations.
Trend is a trapezoid-weighted linear regression on the retained samples.

| z (m) | x-low (K) | x-high (K) | y-low (K) | y-high (K) | Combined trend (K/s) | First / last third (K) |
|---:|---:|---:|---:|---:|---:|---:|
| 0.505 | 300.000000 | 300.000000 | 300.000000 | 300.000000 | +0 | 300.000000 / 300.000000 |
| 0.994 | 300.000000 | 300.000000 | 300.000000 | 300.000000 | +0 | 300.000000 / 300.000000 |
| 1.514 | 300.000000 | 300.000000 | 300.000000 | 300.000000 | +0 | 300.000000 / 300.000000 |
| 2.004 | 300.000000 | 300.000000 | 300.000000 | 300.000000 | +0 | 300.000000 / 300.000000 |
| 2.493 | 300.000000 | 300.000000 | 300.000000 | 300.000000 | +0 | 300.000000 / 300.000000 |
| 3.013 | 300.000000 | 300.000000 | 300.000000 | 300.000000 | +0 | 300.000000 / 300.000000 |
| 3.044 | 300.000000 | 300.000000 | 300.000000 | 300.000000 | +0 | 300.000000 / 300.000000 |
| 3.105 | 300.001303 | 300.000000 | 300.006294 | 300.000000 | -0.000728323 | 300.006049 / 300.000000 |
| 3.197 | 300.036667 | 300.000000 | 300.021792 | 300.000071 | -0.00549596 | 300.046454 / 300.000000 |

All side-band samples through z=3.044 m are exactly 300 K, with zero slope; this
includes the height of r252's worst temperature-rise error. At 3.013 m the wider
r>0.8 m region averages 300.00000973 K (largest sampled spatial mean 300.00036050 K).
At 3.044 m that region averages 300.00026493 K, with a largest sampled mean
300.01283864 K. These small positive tails are real in this extraction, not
rounded away to claim exact 300 K everywhere.

The side bands acquire a nonzero excess at 3.075 m and above: combined mean
300.002016 K at 3.105 m; the largest near-top combined mean is 300.015533 K at 3.197 m.
At 3.197 m the largest instantaneous band mean is 300.181800 K. The trends are
**negative** and the last third returns to 300 K. This is a small early outlet
transient, not a growing hot ambient throughout plume heights. It cannot
explain the roughly 34 K excess centerline rise at 3.044 m. No noticeable bulk
ambient warming is found, so the wider-domain conditional is not triggered.
Cold boundaries alone do not prove that finite-domain flow constraints have
no effect on entrainment; that causal question remains open for review.

## 2. Mass entrainment and 3. convective heat flux

The reference is Heskestad's far-field correlation as printed in
[Jones et al., CFAST Technical Reference Guide, NIST SP1026, April 2009,
printed p 24, Eq3.32](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication1026r2009.pdf):

    m_air =0.071 Qc^(1/3) (z-z 0)^(5/3) [1+0.026 Qc^(2/3)(z-z 0)^(-5/3)]
    z 0 =-1.02 D+0.083 Q_total^(2/5)
    m_plume =m_air+m_fuel

Use kW, m and kg/s: Qc=26.4 kW; total Q=33 kW; D=0.30 m; z 0=0.0301117293 m.
Eq3.33 supplies the small added burner mass,0.000659639 kg/s from 33 kW/LHV.
The expanded additive term is 0.071×0.026×Qc; it is not rounded to 0.0018Qc.
The kW convention is also explicit in [NISTIR6470, pp 3–4](https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir6470.pdf).
The comparison is made above the flame; no lower-flame entrainment fit is claimed.
The CSV flags the original comparison window (0.809908 < z < 3.120498 m)
as `correlation_far_field`; lower and outlet-adjacent rows remain for context.

At each z, integrate the native cell-center gas density and vertical velocity
with area dx². Gas density is the sum of positive-part gas species densities,
matching the thermochemical property convention; soot is excluded and is zero
in r251. Cell-center w is the binary 32 mean of its bounding MAC faces. Net
plume flux uses the fixed r<0.8 m cross-section and includes downward crossings.
This avoids deleting cold entrained air with a temperature threshold.

The output includes radii 0.2,0.4,0.6,0.8 and 1.0 m and the entire plane. At 3.013 m,
net mass is 0.7437/0.7424/0.7253 kg/s at R=0.6/0.8/1.0 m, so truncating the plume
at 0.8 m does not create its deficit. More decisively, **all upward flow anywhere
in the entire plane** is an upper bound on every upward-only plume subset;
that generous bound is still 41.1% below the correlation at 3.013 m. The 0.8 m
cylinder contains 99.999998% of the net sensible-enthalpy flux at that height.

The requested heat quantity uses local species-dependent cp(T):

    F_cp(z)= integral w (T-300K) sum_gas[rho_s cp_s(T)] dA

The independently useful sensible-enthalpy flux is also reported:

    F_h(z)= integral w sum_gas[rho_s (h_s(T)-h_s(300K))] dA

These are signed whole-plane fluxes, including downward transport. Because
cp varies with temperature, cp(T)×ΔT is not exactly h(T)-h(300 K).

| z (m) | Plume net, R=0.8 m (kg/s) | Entire-plane upward upper bound (kg/s) | Correlation total (kg/s) | Net shortfall | cp flux (kW) | cp flux vs 26.4 | Enthalpy flux (kW) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.811 | 0.1965 | 0.2503 | 0.1893 | 3.8% | 27.256 | +3.24% | 26.533 |
| 0.994 | 0.2415 | 0.2979 | 0.2483 | -2.8% | 26.962 | +2.13% | 26.477 |
| 1.514 | 0.3698 | 0.4298 | 0.4577 | -19.2% | 26.640 | +0.91% | 26.434 |
| 2.004 | 0.4862 | 0.5527 | 0.7060 | -31.1% | 26.483 | +0.31% | 26.363 |
| 2.493 | 0.6025 | 0.6725 | 0.9992 | -39.7% | 26.472 | +0.27% | 26.392 |
| 3.013 | 0.7424 | 0.7994 | 1.3564 | -45.3% | 26.347 | -0.20% | 26.290 |
| 3.044 | 0.7503 | 0.8066 | 1.3789 | -45.6% | 26.328 | -0.27% | 26.272 |

Above 1.5 m the requested cp flux is within about 1% of 26.4 kW; the sensible
enthalpy form is closer still. The accepted realized source corresponds to
about 26.306 kW convective power, a little below the nominal 26.4 kW reference.
This is strong evidence against excessive transported heat causing r252's
large upper-centerline error. It is **not** an exact finite-volume energy
balance: cell-center quadrature, transient storage and vertical SGS/molecular
heat transport remain outside this advective diagnostic.

## Plume width and model eddy viscosity (cold-ambient branch)

Define the RMS thermal radius about the burner axis by
`bT²=integral(r² mean(positive ΔT))dA / integral(mean(positive ΔT))dA`.
The reported width is 2bT. For an axisymmetric Gaussian this radius is its
1/e temperature radius; no Gaussian fit is imposed on the data. Heat width
uses the same moment ratio weighted by positive upward sensible-enthalpy
flux. Ratios are formed from time-averaged moments, not averaged radii.

Vreman νt is reconstructed from retained face velocities with the existing
**binary 32 production evaluator**, record Cv, isotropic native filter width
and central gradients. The grid is interior at every tabulated height.
Viscosity averages use r<0.8 m and T>301 K, with area weighting and, separately,
positive upward sensible-enthalpy-flux weighting. The maximum is the largest
sampled cell value in that region. These are evaluations on accepted retained
states, not a claim to recover unsaved intermediate Picard-stage viscosities.

| z (m) | bT RMS radius (m) | 2bT width (m) | Heat-flux RMS radius (m) | Heat-weighted νt (m²/s) | Area-weighted νt (m²/s) | Largest sampled νt (m²/s) |
|---:|---:|---:|---:|---:|---:|---:|
| 0.811 | 0.123 | 0.246 | 0.102 | 0.00124885 | 0.000631504 | 0.00352219 |
| 0.994 | 0.137 | 0.275 | 0.112 | 0.00116927 | 0.000613947 | 0.00325716 |
| 1.514 | 0.168 | 0.335 | 0.137 | 0.000981516 | 0.000579456 | 0.00280206 |
| 2.004 | 0.195 | 0.389 | 0.158 | 0.000872531 | 0.000545511 | 0.00250856 |
| 2.493 | 0.220 | 0.440 | 0.178 | 0.000800784 | 0.000521756 | 0.00228642 |
| 3.013 | 0.248 | 0.495 | 0.202 | 0.000739869 | 0.00049881 | 0.00242739 |
| 3.044 | 0.250 | 0.500 | 0.203 | 0.000733355 | 0.00049385 | 0.00234005 |

The thermal RMS radius grows from 0.137 m at 1 m to 0.248 m at 3 m; the heat-flux
radius grows from 0.112 m to 0.202 m. Meanwhile the heat-weighted νt falls from
1.17×10^-3 to 7.40×10^-4 m²/s. The model evaluates to nonzero viscosity on these retained fields. This does
not recover every unsaved frozen/stage coefficient used during the run.
These facts, together with the mass deficit and heat-flux closure, identify
insufficient dilution/entrainment. They do not distinguish an inadequate
SGS response, under-resolved resolved turbulence, transport formulation or
finite-domain velocity effects. No coefficient change is proposed as proven
by this dataset. **Stop for review.**

## Cleanup and reproducibility

The seven requested r251 scratch folders were deleted: `r251_xcode`,
`r251_picard_balanced`, `r251_picard_diagnostic`, `r251_picard_half`,
`r251_picard_quarter`, `r251_sources`, and `r251_spectrum_reader_check`.
The 32 remaining r251 scratch files (one-off binaries/objects, build scripts,
test and probe logs) were also removed. Their inventory is in
[cleanup.json](data/fire_r253/cleanup.json). Total logical contents removed:
9,363,487,785 bytes (9.363 GB).
The accepted `r251_tier8_full_window` directory, its production log, retained
history/states, committed evidence and r252 movie are preserved. Historical
r251 report paths into the deleted scratch runs are now historical references;
their committed residual histories and review evidence remain available.

[Full height profiles](data/fire_r253/mean_height_profile.csv),
[selected-height time series](data/fire_r253/selected_height_timeseries.csv),
[results](data/fire_r253/results.json) and
[input/source manifest](data/fire_r253/inputs_and_sources.json) accompany this
report. The observer build is warning-free. The solver and renderer binaries
were not rebuilt. No new simulation was executed.

Independent native-flux and scientific/evidence reviews found no P1/P2
issues. Both recomputed key results; count, page-numbering and trend-method
wording were clarified.
