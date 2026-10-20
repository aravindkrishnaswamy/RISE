# r256 — limiter activity and entrainment diagnostic

2026-10-19. The owner cancels tier 6, defers restart non-identity, and authorizes
a ~4 s statistical continuation from r251 step 9,224 at 15.01872063824 s without
any bit-identity requirement. No production solver source/kernel changes.
The continuation started at 2026-10-20 03:47 UTC (Oct 19 local), with 2,432
additional steps planned from the original history. It finished successfully at
2026-10-20 07:15 UTC (00:15 local), after exactly four simulated seconds and
2,432 accepted steps. See the committed launch manifest and measured data.

The standalone observer calls the unchanged production API and only reads its
accepted publication. It retains the same source, original authored duration,
16 Hz output events, source-step ceiling and eight source workers. The old
history estimates the number of steps for four seconds; no timestep or state
is compared or forced to match it. Actual elapsed simulation time is reported.
The external test harness changes only its diagnostic step-count admission
64→4096, as in r255, not any arithmetic. Existing memory protection remains.

## What the alpha columns mean

Scalar transport publishes one shared primal-face alpha for the conservative
components. Compatible momentum restricts the **accepted mass flux** to its dual
faces; it does not have an independent velocity limiter or a unique alpha
obtained by averaging primal alphas. Thus scalar and momentum-*carrier source*
alpha columns refer to the same measured field at MAC-face locations. They are
labelled as such rather than inventing a second coefficient. Independent
momentum-velocity alpha is undefined; its velocity factor is always centered.

Record x/y/z orientations separately and combined, r<0.8 m, mean adjacent
accepted temperature>301 K. Report fraction alpha<1 exactly and mean alpha,
weighted by accepted dt and face count. Slabs are labelled by cell-center
height; z-normal faces are assigned to the slab immediately above, with the top
face folded into the final slab. Activity is not a kinetic-energy damping rate.

Entrained mass is measured at native heights nearest 1, 2 and 3 m using r253's
R<0.8 m mask, positive-part gas-species density sum, and binary32 centered MAC w.
Every accepted state contributes to trapezoidal temporal integration, starting
with the actual retained initial state. The reducer independently integrates
the time series and compares the same actual window with retained r251 samples,
as well as r251's full-window values and Heskestad. No identity verdict is used.

## Momentum advection comparison

Source: pinned FDS 6.11.1 Technical Reference Guide,
[Momentum chapter, “Simplifications” and “Finite-Difference Approximation”](https://github.com/firemodels/fds/blob/FDS-6.11.1/Manuals/FDS_Technical_Reference_Guide/Momentum_Chapter.tex#L259-L345),
[Mass chapter, “Flux Limiters”](https://github.com/firemodels/fds/blob/FDS-6.11.1/Manuals/FDS_Technical_Reference_Guide/Mass_Chapter.tex#L74-L124),
and [Solution Procedure](https://github.com/firemodels/fds/blob/FDS-6.11.1/Manuals/FDS_Technical_Reference_Guide/Time_Chapter.tex).
The matched FDS output identifies **VLES**; its scalar default is Superbee.
This is distinct from choosing Vreman as its SGS model.

| Aspect | RISE r251 | Matched FDS 6.11.1 |
|---|---|---|
| Momentum scheme | Conservative staggered compatible flux: restricted accepted gas mass flux × centered adjacent velocity | Staggered centered velocity–vorticity form; kinetic head absorbed in pressure, separate baroclinic term |
| Scalar limiting | MC/MUSCL slopes in affine coordinates, then shared FCT alpha for bounds | Superbee in this VLES case, plus boundedness correction; CHARM is the LES/DNS option |
| Momentum limiting | Inherits limited mass carrier; no independent velocity limiter | Vorticity term does not consume scalar-face limiter coefficients |
| Numerical dissipation | Scalar base reconstruction and additional alpha<1 corrections; full update also depends on temporal/projection/boundary/source treatment | Scalar reconstruction/correction; centered momentum has no donor-cell blend; temporal/boundary treatment also matters |
| SGS dissipation (separate from numerical damping) | Vreman c=0.07, Pr/Sc=0.7 | Same selected constants; matching formula does not imply identical gradient/stress stencils |
| Time/source coupling | Projected Heun with frozen finite-step source packet and R0/R1/R2 solves | Explicit RK2 predictor/corrector with thermodynamic-divergence pressure solves |

RISE's relevant implementation is `fct_compatible_stage_rate` and
`fct_mass_slope` in `FireProductionAdvectionMac.mm`; the accepted R1 alpha is
also used in the compatible Heun momentum rates. **Alpha=1 does not mean that
MC slope limiting is inactive**: that limiting already defines the high-order
candidate before FCT acceptance. The diagnostic tests the additional shared
alpha correction, not every source of numerical diffusion.

## Measured result and decision — 2026-10-20

Window: **15.01872063824–19.01872063824 s**, 2,432 accepted steps. The
native cumulative entrainment integral and independent trapezoidal reduction
agree within 2×10⁻¹⁵ kg/s. Original r251 same-window values use interpolated
endpoints and its retained temporal samples, rather than every accepted step.

| Native height (m) | Scalar fraction α<1 | Shared momentum-carrier source fraction α<1 | Mean α, both | Continuation mass (kg/s) | r251 same window (kg/s) | Difference | Heskestad 26.4 kW (kg/s) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0.994 | 0 | 0 | 1 | 0.243158 | 0.242677 | +0.198% | 0.248329 |
| 2.004 | 0 | 0 | 1 | 0.478832 | 0.478767 | +0.0135% | 0.705955 |
| 3.013 | 0 | 0 | 1 | 0.718458 | 0.718566 | −0.0149% | 1.356438 |

At 1.514 and 2.493 m the activity is also zero and mean α is one. Indeed,
all measured plume slabs above 0.199 m have zero additional FCT correction,
for each face orientation. The full 106-height, four-orientation profile is in
[data/alpha_height_profile.csv](data/fire_r256/alpha_height_profile.csv).

Only five near-burner slabs, centered 0.0765–0.1989 m, show any activity.
The largest slab fraction is **0.01489%**, at 0.1683 m, with mean α
**0.99997689**. Over the entire mask, count×time weighted fraction α<1 is
**0.00009193%** and mean α is **0.99999986157**. These are shared primal
carrier-source coefficients, not independent momentum velocity coefficients.

**The upper-plume entrainment deficit is reproduced statistically while the
additional FCT alpha correction is inactive there. No limiter-removal A/B is
run.** Its removal cannot address a correction that is already zero in this
region. This does not exonerate the earlier MC slope reconstruction, which
remains active independently of alpha and was not measured by this observer.
No kinetic-energy damping magnitude is inferred from activity alone.

Next candidate differences, not established causes, are:

1. MC/affine mass-carrier reconstruction at α=1 versus FDS's Superbee scalar
   transport, and its transmission into RISE's compatible momentum flux.
2. Conservative momentum versus FDS's rotational velocity advection, including
   pressure/kinetic-head and baroclinic coupling.
3. SGS gradient/stress discretization. The Vreman constant/formula mismatch is
   ruled out by r254; identical constants do not prove identical discrete stresses.
4. Source/thermodynamic-divergence and pressure coupling.

**Stopped for review.** Production solver unchanged; no A/B, tier 6 or tier 10.
Restart non-identity remains a separate deferred low-priority issue, with no
investigation here. Runtime data are in
`/Volumes/SSD/RISE/fire_production_calibration/r256_limiter_diagnostic/`.
