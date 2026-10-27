# r261 — retained full-output LES FDS (in progress)

2026-10-27. One owner-authorized repeat, unchanged r258 explicit LES / Vreman
C=0.07 / CHARM / Pr=Sc=0.7 physics, 33 kW methane, 0.30 m burner, same domain and
69×69×106 grid. Eight OpenMP threads, one MPI rank, end time25.0910401272s.
No RISE production change. Raw files remain external and will not be deleted.

## Physical data retained

- 1,398 float32 slices: native mass-flux z faces; every native horizontal layer
  of **physical HRRPUV**, T, u/v/w, density, divergence, pressure, effective and
  molecular viscosity, methane and oxygen fractions; central vertical cuts.
  HRRPUV here is physical slice data, not clipped Smoke3D fire-display bytes.
- 2,160 devices including the existing all-height centerline T and paired
  native w faces, burner-face w, all-height HRR/density and **QZ000..105**
  instantaneous native-layer volume HRR integrals. Divide the layer integral
  bydx for a plane's kW/m; reconstruct 0–0.7 m with a partial top layer from
  the physical volume slices.
- Four radial rays with26native positions each at heights nearest1/2/3m:
  T, physical HRR, density, and lower/upper native w devices. Their arithmetic
  average supplies cell-center w, avoiding the earlier nearest-face ambiguity.
- Device cadence0.03125s, physical slice cadence0.0625s, global HRR ledger and
  normal restart files. The diagnostic mixing-time accumulator is not reset by
  adding an output that would itself modify that accumulator.

The input and output inventory are committed under `docs/data/fire_r261/`.
Runtime: `/Volumes/SSD/RISE/fire_production_calibration/r261_fds_full/`.
Disk space was checked before launch (about1.4TiB free); physical outputs are
expected to occupy several GiB. There is no clipping/rescaling of physical HRR
slices. Smoke3D output remains a separate rendering product.

The run is bounded and first in the serial campaign; no RISE simulation runs
concurrently. The process and advancing simulation time have been checked.
**Completion, retention verification and reduced measurements pending.**
