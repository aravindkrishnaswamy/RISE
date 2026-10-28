# r262 — combined transport, near-flame comparison and onset safety

Owner accepted r260 and authorized one combined four-second continuation,
read-only comparison with the retained r261 FDS outputs, and from-zero onset
checks of the better centered/combined candidate. Production solver unchanged.

## Execution status

Combined CHARM plus centered momentum is running externally from r251's
step9224 checkpoint, t=15.018720638239756s, to at least19.018720638239756s.
The shared r260 observer compares all accepted Q/T/M/U fields against the
same MC baseline in the first two steps. Both steps differ: first-step maximum
vertical-face velocity difference0.222298m/s; second-step0.406099m/s. The
four-second window proceeds only after that check.

External directory: `/Volumes/SSD/RISE/fire_production_calibration/r262_combined`.
`campaign_status.json` records the bounded serial controller's current stage.
After successful reduction it selects the larger3m mass flux of centered
and combined, then runs that candidate from zero at tier8 through3.5s and
at tier10 through3s, sequentially. Existing stops are60m/s and15m/s,
respectively. These are diagnostic onset checks, not full-window acceptance.
No production adoption follows automatically. An execution failure stops the queue.

Pending: combined entrainment, candidate selection, and both onset maxima.
Measured results will be in `docs/data/fire_r262/results.json` and
`component_comparison.csv`; complete onset trajectories are retained there.

## Near-flame result — existing r251 against full-output LES FDS r261

Statistics window10.4660401272–25.0910401272s. Actual common native cell
heights are0.291/0.505/0.994m. Upward flux integrates positive vertical mass
flux inside radius0.8m; it differs from the net entrainment used in the A/B table.

| Height (m) | RISE upward (kg/s) | FDS upward (kg/s) | RISE deficit | Temperature half-width RISE/FDS (m) | Velocity half-width RISE/FDS (m) |
|---:|---:|---:|---:|---:|---:|
|0.291|0.09301|0.13997|33.6%|0.0621 /0.0634|0.0746 /0.0777|
|0.505|0.14370|0.18472|22.2%|0.0671 /0.0753|0.0832 /0.0933|
|0.994|0.26331|0.31063|15.2%|0.1017 /0.1270|0.1226 /0.1397|

Centerline temperature rise RISE/FDS is1236.7/1051.9K,838.2/633.7K,
and317.0/202.8K. Centerline vertical velocity is4.123/4.025m/s,
4.391/4.057m/s, and3.675/3.318m/s. At0.29m the widths and center velocity
are close; by0.5m RISE is narrower in both fields and hotter, with further
separation by1m. The mass-flow difference already exists inside the flame.
This comparison locates the difference; it does not establish its cause.

The vertical mean heat distribution is very similar: below1m it integrates
to32.835kW in RISE and32.957kW in FDS. Of that heat,84.88/84.27% lies
below0.3m,96.20/96.10% below0.5m, and99.15/99.24% below0.7m.
Mean heat-release height below1m is0.1385/0.1409m. The entrainment and
width differences coexist with closely matched mean heat placement.

![Radial profiles](data/fire_r262/radial_profiles.png)
![Heat height profile](data/fire_r262/heat_height.png)

## Measurement conventions and limits

Radial means use all cells in dx-wide annuli; half-width is the first outward
crossing of half the centerline value, linearly interpolated. RISE radial
profiles/HRR use235 retained frames with trapezoidal weights. Its mass flux
uses the existing retained-checkpoint quadrature. FDS uses physical float32
slices over the same time window. W is centered from adjacent native staggered
faces; scalar temperatures are cell-centered, converted from Celsius.

FDS native mass-flux faces are centered before positive clipping. RISE uses
cell density times centered W. To quantify this convention difference, FDS
was also reduced using cell density times centered W: upward flux becomes
0.139969/0.184704/0.310659kg/s, less than0.01% different. It does not explain
the near-flame deficit. Full-plane upward flux and net radius0.8m values
are also in `near_flame_mass.csv`.

Physical FDS QZ layer HRR volume integrals are kW; division by dx gives kW/m.
RISE slab powers are W and are converted identically. The final layer is
fractionally weighted at1m. Source code, build hashes, small data and figures
are retained in the repository; full fields, checkpoints, binaries and logs
remain external. No new physical simulation was used for this comparison.
