# r269 — the missing ambient inflow comes from a warm floor in FDS

2026-10-29. Read-only analysis by Claude of r264 (RISE, centered momentum) against r263 (FDS no-slip). Existing data only; no run, no code change.

**Sources.**
- RISE: time means of the full 3D temperature and velocity fields from r264's 235 retained frames (`mean3d.cpp`).
- FDS: time means of U, V, W, temperature, density, pressure and ρu, ρv, ρw from the horizontal slices at all 106 heights (`fds_mean3d.py`).
- The window is 10.466–25.091 s.
- RISE density is taken as 352.6/T. That is exact for air and adequate outside the flame, which is where this analysis looks.

**Files.** Scripts are in [`tools/fire/r269/`](../tools/fire/r269/). They `exec` `meander.py` from `tools/fire/r266/`, so run them from a directory containing both. Outputs are in [`data/fire_r269/`](data/fire_r269/).

## 1. Where the extra FDS inflow enters

Side-boundary mass inflow, cumulative from the floor (kg/s, FDS / RISE):

| Up to z (m) | FDS | RISE | Deficit |
|---:|---:|---:|---:|
| 0.29 | 0.107 | 0.071 | 0.036 |
| 0.99 | 0.313 | 0.238 | 0.075 |
| 2.00 | 0.687 | 0.515 | 0.172 |
| 3.23 (all sides) | 1.164 | 0.846 | 0.318 |

The side totals match each code's top outflow, and RISE's matches its accepted-stage budget. The deficit has two parts:
- **A near-floor part, below about 0.3 m.** FDS's inflow there is 1.5–2.5× RISE's. Through the r = 0.5 m cylinder, below 0.12 m: 0.052 against 0.023 kg/s.
- **A far-plume part, above 1.5 m.** This is the turbulence deficit from r266/r267.

Between 0.5 and 1 m the radial inflow velocities agree within 10%.

## 2. In FDS the near-floor air does not feed the flame; it rises around it

- **Air reaching the flame is close.** Inflow through r = 0.2 m below 0.31 m is 0.066 kg/s (FDS) against 0.057 (RISE), only 14% apart. Below 1 m it is 0.176 against 0.161.
- **FDS's extra air turns upward in an annulus at r = 0.2–0.5 m:**

| z (m) | Annulus updraft, r 0.2–0.5 (kg/s), FDS / RISE | Mean-flow sensible heat carried up (kW), FDS / RISE |
|---:|---:|---:|
| 0.11 | 0.020 / −0.003 | 0.105 / 0.000 |
| 0.29 | 0.045 / 0.002 | 0.081 / 0.001 |
| 0.50 | 0.057 / 0.011 | 0.079 / 0.024 |

This annulus is the "ambient updraft outside the core" that r266 found missing. It is not potential flow induced from above. It is warm air rising off the floor.

## 3. The FDS floor is heated; the RISE floor is not

Mean temperature rise in the lowest cell, z = 0.015 m (K, FDS / RISE):

| r (m) | 0.15–0.2 | 0.2–0.3 | 0.3–0.4 | 0.5–0.7 | 0.7–0.9 | 0.9–1.04 |
|---|---:|---:|---:|---:|---:|---:|
| ΔT | 30 / 10 | 15 / 0.0 | 9.8 / 0.0 | 4.6 / 0.0 | 3.0 / 0.0 | 1.9 / 0.0 |

- **RISE's floor air outside the burner is exactly ambient.** FDS's is warm out to the domain edge, and the warming falls off with distance the way absorbed flame radiation would.
- **Why:** the r255/r263 FDS deck sets the floor to `ADIABATIC=T`. In FDS an adiabatic surface has zero *net* heat flux. It absorbs the flame's radiation, heats until it re-emits and convects that energy back, and so warms the gas next to it. FDS's own budget (`r263_fds_hrr.csv`, statistics window) shows the floor convecting **Q_COND = +0.51 kW** into the gas. Q_RADI is −6.86 kW and the radiative fraction is 0.20.
- **RISE does not do this.** It treats radiation as an optically thin loss, and its "adiabatic" bed is adiabatic only to the gas: zero conductive and diffusive flux (`FIRE_SMOKE_DESIGN.md` §boundary conditions). The radiated 6.6 kW never heats the floor.
- **So the two runs do not share a boundary condition, although both are labelled adiabatic.**

## 4. Consequences for the r264 verdict

- **The FDS reference contains floor-reradiation physics that RISE does not model.** About 0.5 kW, 1.5% of HRR, returns at the floor and drives an annular updraft of 0.02–0.06 kg/s below 0.5 m.
- **This plausibly explains the near-field differences:**
  - all of the r266 "outside-core" deficit below 1 m;
  - part of r264's longer, hotter flame, since FDS's co-flowing warm annulus changes how the flame base mixes.
  - It does not obviously explain the far-plume entrainment deficit above 1.5 m.
- **The McCaffrey comparison is unaffected,** but which FDS features count as the "right" answer is now in question.

## Next: needs owner approval, since it is a new run

**Proposed:** an FDS variant of r263 identical except that the floor does not absorb radiation, giving `&SURF ID='FLOOR'` `EMISSIVITY=0`. It stays adiabatic and no-slip, so it matches RISE's floor.
- Cost: about 1.5 h on 8 threads (r263 took 5,377 s).
- It touches no RISE code.

Reading:
- **If the warm floor layer, annular updraft and near-floor inflow disappear, and FDS's flame lengthens toward RISE,** the near-field gap is a reference-setup mismatch rather than a RISE error. Only the far-plume turbulence deficit would remain to explain.
- **If not,** look at RISE's near-floor treatment next: the no-slip bed and its momentum flux.

The alternative, adding floor radiation absorption to RISE, is a modelling change for the owner to decide on.
