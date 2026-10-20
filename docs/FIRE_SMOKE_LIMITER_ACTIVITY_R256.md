# r256 — limiter activity and entrainment diagnostic (in progress)

2026-10-19. The owner cancels tier 6, defers restart non-identity, and authorizes
a ~4 s statistical continuation from r251 step 9,224 at 15.01872063824 s without
any bit-identity requirement. No production solver source/kernel changes.
Measured four-second results and the A/B decision are **pending**.

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

## Decision to apply after the measurements

If the measured correction is active in the deficient upper plume, formulate
the smallest experimental change that removes the identified momentum damping
without breaking mass/momentum compatibility or the tier-10 focusing protection,
and test a short A/B from this same checkpoint. The owner's explicit short-A/B
instruction permits a diagnostic checkpoint comparison, not production adoption
or an accepted from-zero result. No change is selected merely from alpha<1:
correction magnitude/its momentum effect must support attributing damping.

If alpha is mostly inactive where entrainment is deficient, do not run a
limiter-removal A/B. Next candidates, not established causes, are: MC/affine
mass-carrier reconstruction even at alpha=1 versus FDS's Superbee; conservative
momentum versus rotational velocity advection and pressure/baroclinic coupling;
SGS gradient/stress discretization; and source/divergence coupling. The Vreman
constant/formula discrepancy remains ruled out by r254.

No production change, tier 6, or tier 10 follows automatically. The observer and
reducer stop after this bounded window. Runtime data are in
`/Volumes/SSD/RISE/fire_production_calibration/r256_limiter_diagnostic/`.
