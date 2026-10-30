# r267 — r264 full window: centered momentum helps, but only partly

2026-10-29. Read-only analysis by Claude of r264, the full tier-8 run from zero with centered momentum (commit `a0e0055ba`). No solver change.

**Run.**
- Completed 15,380 steps to 25.091 s with exit 0.
- No retries of either kind (hard-bound or physical-projection).
- Wall time was 83,748 s (23.3 h), or 5.45 s/step.

**Analysis inputs.**
- Automated reduction outputs: [`data/fire_r264/`](data/fire_r264/).
- Toolkit outputs: [`data/fire_r267/`](data/fire_r267/), produced with [`tools/fire/r266/`](../tools/fire/r266/) `full.py`, `blocks.py`, `meander.py` and `spec.py`.
- RISE values come from the 235 retained frames.
- FDS values come from the no-slip r263 slices.
- The window is 10.466–25.091 s on the shared 69×69×106 grid.

## Correction to r266 §3

r266 said centered momentum "restores most" of the far-plume turbulence. **The full window does not confirm that.**
- The A/B figures (2 m: 85 K, b/z 0.101; 3 m: 43 K) came from 48 frames.
- Within r264, consecutive 3 s blocks vary by about ±20% in θ_c and b/z. At 3 m, θ_c runs 48–70 K from block to block (`blocks_r264.txt`).
- So the A/B window was a favourable sample, not a converged statistic.

## Far plume, full window

| z (m) | Code | w_c | w_rms/w_c | θ_c (K) | θ_rms/θ_c | b_T/z | Core flux, r<2b_FDS (kg/s) |
|---:|---|---:|---:|---:|---:|---:|---:|
| 0.99 | FDS | 3.82 | 0.348 | 249 | 0.460 | 0.109 | 0.192 |
| 0.99 | r251 | 3.67 | 0.254 | 317 | 0.249 | 0.097 | 0.182 |
| 0.99 | r264 | 3.99 | 0.293 | 356 | 0.444 | 0.087 | 0.169 |
| 2.00 | FDS | 2.72 | 0.314 | 73 | 0.412 | 0.106 | 0.590 |
| 2.00 | r251 | 3.27 | 0.221 | 124 | 0.243 | 0.075 | 0.468 |
| 2.00 | r264 | 3.03 | 0.260 | 108 | 0.358 | 0.085 | 0.482 |
| 3.01 | FDS | 2.36 | 0.327 | 40 | 0.416 | 0.100 | 1.101 |
| 3.01 | r251 | 3.19 | 0.200 | 71 | 0.266 | 0.066 | 0.747 |
| 3.01 | r264 | 2.77 | 0.240 | 60 | 0.353 | 0.074 | 0.846 |

For comparison, Shabbir & George (1994) give w_rms/w_c = 0.33, θ_rms/θ_c = 0.40 and b/z = 0.10. McCaffrey gives about 74 K / 2.8 m/s at 2 m and about 38 K / 2.45 m/s at 3 m.

What improved:
- Temperature fluctuation is now near the published level, at 0.35–0.44 against 0.25.
- Meander rose from about 0.17–0.25 to about 0.24–0.32 of FDS's half-width, against FDS's 0.32–0.38 (`meander_r264.txt`).

What did not:
- Velocity fluctuation is still 25% low.
- Spreading is 0.074–0.087 against 0.10.
- At 3 m, core mass flux is 23% low and θ_c is 50% high.

## The near field got worse

| Quantity | FDS | r251 | r264 |
|---|---:|---:|---:|
| θ_c at 0.50 m (K) | 801 | 837 | 983 |
| θ_c at 0.99 m (K) | 249 | 317 | 356 |
| w_c at 0.50 m (m/s) | 4.62 | 4.39 | 5.08 |
| Flame height, 99% of input HRR (m) | 0.81 (realized) | 0.78 | 0.92 |
| Centreline puffing peak (Hz) | 2.74 (expected) | 4.50 | 3.62 |

Heskestad gives 0.65 m for the flame height.

- **The flame is longer and its core hotter and faster.** Core mass flux below 1 m is slightly lower than r251's, so less air reaches the flame.
- **Combustion closure is not the issue:**
  - 99.55% of the fuel was burned;
  - top methane export is 0.50% of supply;
  - the balance residual is 1e-8 of supply;
  - mean HRR is 32.85 kW;
  - the radiant fraction is 0.199.
- **Peak velocity** was 14.4 m/s, at step 746 (1.16 s, during startup).

## Acceptance against the owner's 15% McCaffrey condition

From the automated reduction, which uses 76 selected stations between 0.81 and 3.12 m:

| Check | Result | Verdict |
|---|---|---|
| Worst temperature error | +56% at 2.95 m (61.7 K vs 39.5 K) | **Fails** |
| Worst velocity error | +13.6% at 0.81 m | Passes |
| Disc mass flux r<0.8 m vs FDS (1, 2, 3 m) | −22%, −23%, −27% | Fails, though r266 says this metric should be retired |

**r264 does not meet the McCaffrey condition, so tier 10 stays blocked.**

## What this says about the cause

- **Centered momentum removed one damping source.** Temperature fluctuation and meander rose.
- **It did not fix entrainment.** RISE's plume still entrains about 20–25% less ambient air than FDS at every height, and the low-frequency w power at 0.5–1 m is still at 0.56–0.83 of FDS's.
- **The near-field result points at the ambient side.** RISE has almost none of FDS's induced ambient updraft. Outside r > 2b at 0.3–1 m it carries 0.023–0.059 kg/s against FDS's 0.077–0.112, and r264 has less of it than r251. A stronger, less-mixed jet then gives a longer, hotter flame.
- **Candidates for the next diagnosis:**
  - the open-boundary pressure condition on the sides, which sets how freely ambient air is drawn in;
  - remaining dissipation in the scalar (MC + FCT) transport of density, which sets baroclinic and buoyant forcing at the plume edge;
  - the SGS model, which matches FDS's constants but may differ in operation.

## Next

1. **Profile one production step** (owner request; step cost is 5.45 s). Faster runs make the diagnosis above affordable.
2. **Diagnose the missing induced ambient flow** from r264 and FDS slices:
   - radial velocity at the side boundaries;
   - pressure at the side boundaries;
   - side-boundary inflow versus height.
   This is read-only.
3. **No tier-10 run.**
