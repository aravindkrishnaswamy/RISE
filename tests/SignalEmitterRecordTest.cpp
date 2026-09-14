//////////////////////////////////////////////////////////////////////
//
//  SignalEmitterRecordTest.cpp - Does an EMISSIVE material keyed on a
//    geometry shading signal read that signal LIVE when its light is
//    reached by NEE or as a light-subpath root?
//
//    Slice S3 of docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5.
//
//  THE PROPERTY UNDER TEST
//
//    A light sample is a SAMPLED POINT, not a hit:
//    `IObject::UniformRandomPoint` returns a position, a normal and a
//    UV and nothing else, so every emitter record built from one --
//    LightSampler's two NEE records, `SampleLight`'s emission record,
//    BDPT's NM hero + HWSS companion rebuilds, VCM's light-vertex NEE
//    record, and the `type == LIGHT` subpath root -- used to carry
//    DEFAULT `derivatives` and `signals`.  An emissive material whose
//    exitance keys on `curv` or `proximity(r)` therefore read the LIVE
//    value when a camera ray hit the emitter and the documented
//    NEUTRAL value when NEE reached the same point on the same surface
//    in the same frame.  That is a within-scene inconsistency and it
//    reaches PLAIN PT, not just the bidirectional families.
//
//    Slice S3 recovers the payload from a REAL intersection
//    (`LightSampler::ProbeEmitterSurface`).  This suite is the
//    end-to-end witness.
//
//  THE MEASUREMENT, and why it is reference-free
//
//    Every row renders the SAME scene twice and compares the two means
//    to each other -- never to a hand-computed radiance, and never
//    across integrators:
//
//      EXPR    : exitance = 0.2 + 0.8 * <signal>
//      CONTROL : exitance = 0.2 + 0.8 * <the signal's KNOWN CONSTANT>
//
//      assert  | mean(I, EXPR) / mean(I, CONTROL) - 1 | < band
//
//    The two documents differ in ONE token, so geometry, sampling,
//    filtering, MIS and the integrator's own bias all cancel exactly.
//    PT has been the broken reference twice this year (debts 25/26 and
//    the env double-count), which is why no row compares one integrator
//    against another.
//
//  WHY A CONSTANT SIGNAL MAKES THE CONTROL EXACT
//
//    `curv` is `H * scaleHint`, and on ANY sphere H = 1/r while
//    scaleHint is the bbox diagonal 2r*sqrt(3) -- so curv = 2*sqrt(3)
//    ~ 3.464 at every point, independent of the radius
//    (SurfaceCurvatureTest pins exactly this).  `clamp(curv,0,1)`
//    therefore SATURATES to exactly 1 everywhere on the emitter, and
//    the control is the literal `1.0`.  Saturation is what makes the
//    row robust: an SDF sphere's field-derived H only has to land
//    anywhere above 1/(2*sqrt(3)) ~ 0.289 of the true value for the
//    clamp to give the same answer, so the row tests the RECORD, not
//    the curvature estimator's last digit.
//
//    `proximity(r)` is `clamp(1 - d/r, 0, 1)` with `r` a world length.
//    Row C parks a 2x2x2 box so that its near face is a uniform
//    h = 0.2 in front of the whole emitter quad (the box is wider than
//    the quad in both lateral directions, so every quad point's nearest
//    box point is straight ahead) -- the closed form from
//    docs/CROSS_OBJECT_PROXIMITY_DESIGN.md.  With r = 1.0 the signal is
//    exactly 0.8 over the entire emitter, and the control is `0.8`.
//    This row also proves the CROSS-OBJECT triple (`pScene` / `pSelf` /
//    `ptWorld`) is stamped on the probed record: without it
//    `proximity()` returns its neutral 0 and the row goes red.
//
//    `Po` (row D) is the object-space hit point, and on a sphere of
//    radius 0.5 about its own object origin `|Po|^2 = 0.25` EXACTLY at
//    every surface point, so `clamp(8*|Po|^2, 0, 1)` saturates to 1
//    with 2x headroom.  The control is again `1.0`.
//
//  THE SIX FAMILIES, and what each one is the only witness for
//
//    A  SDF sphere, `curv`.  The broadest row set: PT / BDPT / VCM in
//       RGB, all three again SPECTRAL, and BDPT-spectral once more with
//       HWSS on.  The spectral rows are the ONLY coverage of three of
//       the seven converted sites -- `EvaluateDirectLightingNM`, the NM
//       hero `Le` rebuild in `GenerateLightSubpathImpl`, and the HWSS
//       companion-wavelength rebuild beside it -- since nothing in an
//       RGB rasterizer reaches any of them.
//    B  analytic `sphere_geometry`, `curv`.  A completely different
//       curvature path (the Weingarten map via dndu/dndv, not an SDF
//       field Hessian) has to arrive at the record too.
//    C  flat quad + box neighbour, `proximity(1.0)`.  The cross-object
//       triple.  Run under BDPT and VCM as well as PT because the
//       triple reaches them by a different ROUTE: PT probes at the NEE
//       site and applies the payload there, while BDPT and VCM get it
//       off `LightSample::surface`, probed once in `SampleLight`.
//       (Both routes call the same `ProbeEmitterSurface` since the
//       round-2 review; before it they called different functions, and
//       red-proof (2) is what pins the `SampleLight` one.)
//    D  SDF sphere, `Po`, with NO SIGNAL PAINTER ANYWHERE IN THE SCENE.
//       `Po` is not signal state, so it must not ride the probe's
//       process-wide gate; this family renders with that gate CLOSED
//       and still has to pass, and its extra gate-invariance block
//       renders the same emitter with an unrelated `curv` painter added
//       to the RECEIVER (which opens the gate without changing a pixel
//       of the receiver) and requires the emitter's mean not to move.
//       That block now ASSERTS the gate is shut before each of its two
//       closed renders, so a demand leaked from an earlier family
//       cannot turn an open-versus-closed comparison into an
//       open-versus-open one and make it pass vacuously.
//    E  SDF sphere at `epsilon 0.002`, `curv`.  The luminary's own
//       `SelfHitRootFloor` is then four times the flat standoff the
//       normal-aligned probe used to use, so the probe was marched past
//       the face it was aimed at and refused.  Since the probe was
//       unified this row goes red under ALL THREE integrators when the
//       standoff is flattened -- it used to leave PT green, which was
//       the disagreement itself.
//    F  two-lobe SDF, `casts_shadows FALSE`, `curv`.  THE UNIFIED-PROBE
//       WITNESS: with the NEE sites firing along `vToLight` instead of
//       along the sampled normal, PT reads 23 % off its baked control
//       while BDPT and VCM read 0.0007 % and 0.007 % off theirs -- one
//       scene, one frame, PT and the bidirectional families disagreeing
//       about the same surface.  See the emitter's own comment for the
//       geometry and for why the review's grazing-incidence model is
//       NOT what makes this row work.
//
//  SENSITIVITY -- the check cannot pass by insensitivity
//
//    Each family also renders a NEUTRAL document, the same expression
//    with the signal replaced by the value a neutral read would
//    produce (0 for `curv`, for `proximity` and for `Po`).  The suite
//    asserts that `mean(PT, EXPR) / mean(PT, NEUTRAL) - 1` is FAR
//    outside the band.  By construction the neutral emitter is 0.2/1.0
//    (curv, Po) or 0.2/0.84 (proximity) as bright, so EXPR/NEUTRAL is
//    ~5x / ~4.2x -- MEASURED at 399.5-400.3 % (curv and Po families)
//    and 316-326 % (proximity), i.e. 63-80x the band.  A regression to
//    neutral cannot hide inside a few percent of noise.
//
//  THE CAMERA SEES ONLY THE RECEIVER
//
//    The emitter sits at (0, 1.8, 1.2); the camera at (0, 0, 3.5) with
//    a 30-degree fov sees +-0.94 of the receiver plane at z = 0.  The
//    emitter is 38 degrees off axis -- well outside the frustum -- so
//    no camera ray hits it and its radiance reaches the film ONLY
//    through NEE (PT, BDPT, VCM), a light-subpath root (BDPT, VCM), or
//    a BSDF-sampled continuation off the receiver.  That last route
//    already used a live record before S3, which only makes the test
//    CONSERVATIVE: it dilutes the signal a correct probe must restore.
//
//  BAND: 5 % relative on the per-channel mean.
//
//    Derived from the observed run-to-run spread at 40x40.  RISE renders
//    are NOT wall-clock seeded (an earlier draft of this comment claimed
//    they were, the same mistake tests/SignalIntegratorConsistencyTest.cpp
//    made and corrected): every render worker's `RandomNumberGenerator`
//    draws its default `seed = rand()` from libc's global state, read and
//    mutated unsynchronized across `ThreadPool::ParallelFor` threads, and
//    BDPT/VCM splat accumulation is thread-order dependent on top of that
//    -- so the row-to-row independence below comes from the distinct
//    `std::srand( g_seedBase + g_renderIndex++ )` this file now issues
//    before every `Rasterize()` call (see g_seedBase's declaration), not
//    from the clock.  FIVE FRESH RUNS at seed bases 1000, 2000, 3000,
//    4000, 5000 (`./SignalEmitterRecordTest <base>`; logs saved as
//    s3_final_seed<base>.txt), worst |EXPR/CONTROL - 1| per row, in
//    percent:
//
//      A / PT                  0.049  0.077  0.040  0.021  0.053
//      A / BDPT                0.107  0.358  0.456  0.112  0.142
//      A / VCM                 0.156  0.053  0.005  0.100  0.201
//      A / PT-spectral         0.719  0.372  0.823  0.342  0.615
//      A / BDPT-spectral       0.639  0.194  0.962  0.375  0.100
//      A / VCM-spectral        0.773  0.359  0.876  0.820  0.579
//      A / BDPT-spec + HWSS    0.361  0.184  0.217  0.349  0.371
//      B / PT                  0.028  0.039  0.016  0.019  0.022
//      C / PT                  0.037  0.036  0.338  0.328  0.268
//      C / BDPT                0.005  0.012  0.002  0.031  0.015
//      C / VCM                 0.014  0.043  0.008  0.017  0.064
//      D / PT                  0.036  0.007  0.067  0.011  0.006
//      D / BDPT                0.046  0.131  0.007  0.031  0.469
//      D / VCM                 0.012  0.032  0.114  0.331  0.176
//      D gate-inv CONTROL      0.106  0.059  0.054  0.041  0.018
//      D gate-inv EXPR         0.018  0.027  0.036  0.067  0.017
//      E / PT                  0.088  0.146  0.022  0.051  0.010
//      E / BDPT                0.067  0.453  0.237  0.204  0.196
//      E / VCM                 0.210  0.373  0.005  0.364  0.107
//      F / PT                  0.005  0.036  0.115  0.096  0.078
//      F / BDPT                0.048  0.031  0.066  0.084  0.026
//      F / VCM                 0.000  0.052  0.098  0.028  0.041
//
//    THE LARGEST OF THESE FIVE RUNS IS 0.96 % (A / BDPT-spectral, seed
//    base 3000).  That is a five-run MAXIMUM, not a bound on the row --
//    the round-2 review's correction (H2 P2-9) still applies: an earlier
//    header called 0.61 % "worst observed" and reviewers then saw 0.74 %
//    and 1.10 % on the same spectral rows, and this re-measurement (now
//    genuinely independent, not a wall-clock hope) puts the observed
//    ceiling at 0.96 %, still not a promise for every future run.  The
//    margin to the 5 % band from THIS measurement is 5.2x (5 / 0.96);
//    earlier, non-frozen-seed rounds observed C/PT as high as 1.53 %,
//    A/BDPT-spectral+HWSS 1.27 % and E/BDPT 0.65 % over just two ad hoc
//    runs, which is why the band is set from TAIL BEHAVIOUR, not from
//    any one run's maximum: the non-HWSS spectral rows draw one wavelength per pixel
//    sample, so their per-channel mean is chromatically noisy and can
//    excur past 1 % on an unlucky draw, and 5 % keeps roughly 4-5x
//    headroom above every ceiling observed across this file's history
//    while still sitting 60x+ below the smallest sensitivity swing
//    (~320 %).  A row that regressed to a neutral read moves 8 %
//    (C / VCM, red-proof 9) to 75 % (most others), so the band separates
//    the two populations by more than an order of magnitude at either
//    end.  It is NOT a promise that a given run lands under 0.96 %, only
//    that the tail is nowhere near the failure population.
//
//    The SENSITIVITY rows measured 399.6-400.5 % (curv and Po families),
//    319.5-322.2 % (proximity) and 399.7-400.3 % (family F) across the
//    same five seeded runs.
//
//    SAMPLE COUNTS ARE NOT UNIFORM, and both departures from the base
//    48 are deliberate and were measured, not guessed:
//
//      * ROW C RUNS AT 384.  It is the dimmest family by an order of
//        magnitude (mean ~0.048 against ~0.39), so its relative MC
//        noise is the largest in the suite.  Nine runs at the original
//        48 samples spread to 2.89 %, and five runs at 192 still
//        reached 1.67 %; 384 brought the worst of five to 0.85 %, and
//        the five seeded runs tabled above to 0.34 %.
//      * THE NON-HWSS SPECTRAL ROWS RUN AT 2048.  `spectral_samples 1`
//        draws ONE wavelength per pixel sample out of 380-720 nm, so
//        the per-channel mean carries a CHROMATIC error the RGB rows do
//        not have: 4.0 % (BDPT-spectral) and 5.2 % (VCM-spectral) at 48
//        samples, against a 5 % band -- i.e. noise indistinguishable
//        from a failure.  384 brought it to 2.3 %, 1024 to 2.2 % worst
//        of nine, 2048 to 0.61 %.  The HWSS row needs none of that
//        (0.40 % at 48): a hero wavelength with seven companions
//        averages the bundle WITHIN each sample, which is exactly the
//        variance at issue.  It is given 96 for headroom at negligible
//        cost.
//
//    Whole-suite runtime at these counts is ~57 s.
//
//  RED-PROOF, performed in this slice's own isolated worktree (never
//  the shared checkout).  Each mutation was reverted with
//  `git checkout -- <file>` and `git status --short` checked clean
//  before the next.  Percentages are |EXPR/CONTROL - 1| on the row
//  named; every row not named stayed inside the band.
//
//    THE CHOKE POINT IS `EmitterProbeWanted()`.  It is now a public
//    static on `LightSampler` (the two NEE sites ask it to keep a
//    416-byte payload out of their per-sample loop), and forcing it to
//    return false restores the pre-S3 fallback at ALL SEVEN sites at
//    once -- there is only ONE probe entry point behind it since the
//    round-2 transport review.
//
//    (1) `EmitterProbeWanted()` -> false.  20 FAILs / 57 passes.  Every
//        MONEY row outside family D goes red: A/PT 75.32, A/BDPT 74.88,
//        A/VCM 68.72, A/PT-spectral 75.54, A/BDPT-spectral 75.04,
//        A/VCM-spectral 67.29, A/BDPT-spec+HWSS 74.44, B/PT 75.34,
//        C/PT 43.84, C/BDPT 75.69, C/VCM 68.48, E/PT 75.29,
//        E/BDPT 74.99, E/VCM 68.90, F/PT 67.82, F/BDPT 68.09,
//        F/VCM 65.16.  The A, B and E SENSITIVITY rows also go red, at
//        23.12 / 23.46 / 23.47 -- correctly, because with the probe off
//        EXPR has collapsed most of the way onto NEUTRAL; the ~23 %
//        that survives is the BSDF-sampled continuation that hits the
//        emitter through a REAL record and was never neutral.  C's and
//        F's sensitivity rows stay green (135.4 / 60.8), being further
//        from their neutrals to begin with.  That PT goes red at all is
//        the point: this slice is not a bidirectional-only fix.
//        FAMILY D IS UNTOUCHED (0.021 / 0.008 / 0.038, gate-invariance
//        0.003 / 0.028), which is the whole reason it exists: `Po` is
//        deliberately outside this gate.
//
//    (2) Pass 0 instead of `scene.GetObjects()` at the `SampleLight`
//        probe site, so the cross-object triple is never stamped on the
//        payload BDPT and VCM read while the NEE sites keep their own.
//        2 FAILs: C/BDPT 75.70, C/VCM 68.50.  C/PT stays green at
//        0.017.  (Measured before the probe unification and NOT re-run
//        after it: the site, its argument and the rows that can see it
//        are all unchanged, and family F is keyed on `curv`, which this
//        mutation does not touch.)
//
//    (3) Delete the SelfHitRootFloor-derived term, i.e. restore the
//        flat `kEmitterProbeStandoffFraction * diag` standoff.  4 FAILs:
//        E/PT 75.27, E/BDPT 75.13, E/VCM 68.98, and E's sensitivity row
//        at 23.66.
//
//        THIS RESULT CHANGED WITH THE UNIFICATION, and the change is
//        the point.  Before it the same mutation left E/PT GREEN at
//        0.064 while only BDPT and VCM went red -- PT reached this
//        emitter through a probe with no standoff at all, so the
//        standoff bug could not touch it.  All three now share one
//        probe and fail together, which is what "the three integrators
//        refuse identically" means operationally.
//        (With the intermediate `1.01 * floor` standoff, before the
//        cushion was added, the BDPT and VCM rows sat at 37.4 / 35.1:
//        the probe cleared the floor only on roughly half the samples,
//        the ones whose Newton projection happened to land OUTSIDE the
//        true surface -- the half that landed INSIDE lowered the
//        clearance below the floor and refused instead.  THAT 37.4 / 35.1
//        CUSHION-0 COLLAPSE is what sizes
//        `kEmitterProbeStandoffCushionFraction` -- 0.001 * diag is what
//        closes it, not any post-hoc percentage -- and the choice is only
//        CONFIRMED, not derived, by family E's five-seeded-run worst of
//        0.45 % (E / BDPT, seed base 2000; see the table above), well
//        inside the 5 % band.
//
//    (4) Skip `ApplyEmitterSurface` at the NM NEE site
//        (`LightSampler::EvaluateDirectLightingNM`).  1 FAIL:
//        A/PT-spectral 75.63.  Nothing else moves -- this site is
//        reached by the spectral PT rasterizer and by nothing else in
//        the suite, which is precisely why the spectral rows were
//        added.  (Measured before the unification and not re-run: the
//        site still exists, still applies the payload, and family F has
//        no spectral row.)
//
//    (5) Skip `ApplyEmitterSurface` at BDPT's NM hero `Le` rebuild.
//        WEAK, and reported as measured rather than as a red-proof:
//        the only row that moves at all is A/VCM-spectral, at 5.70 /
//        3.41 / 4.50 over three runs -- i.e. straddling the 5 % band.
//        A/BDPT-spectral moves 0.10 / 0.82 / 0.66, inside its own noise.
//
//    (6) Skip `ApplyEmitterSurface` at BDPT's HWSS companion rebuild.
//        NO ROW MOVES (the HWSS row reads 0.299, its usual noise).
//
//    WHY (5) AND (6) ARE WEAK, and what that means.  Proofs (8) and (9)
//    below show where BDPT and VCM actually price this emitter: BDPT
//    through the `type == LIGHT` ROOT VERTEX (`LuminaryRadiance` /
//    `PopulateRIGFromVertex`), VCM through its own light-vertex NEE
//    record.  The hero `LeNM` and its HWSS companions set the light
//    SUBPATH's throughput instead, which only reaches the film through
//    the s>=1 connection and t=1 splat strategies -- and on a scene
//    this simple (pinhole camera, one diffuse receiver, an area light
//    every eye vertex can see) MIS weights those down to a few percent.
//    Making them dominant needs a light-tracing- or caustic-dominated
//    scene, not a knob on this one.  The two sites ARE converted and
//    their code path is exercised by every spectral row; what this
//    suite does not have is a row that would go red if they regressed.
//    Recorded here rather than papered over.
//
//    (7) Make `LightSampler::EmitterObjectPoint` return its `fallback`
//        when the gate is closed -- i.e. put `Po` back on the GATED
//        payload, the state this slice's first draft shipped.  5 FAILs:
//        D/PT 75.32, D/BDPT 74.85, D/VCM 68.73, D's sensitivity row,
//        and -- the reproduction of the review's own finding -- the
//        GATE-INVARIANCE EXPR row at 304.81, i.e. the gate-OPEN render
//        is 4.05x the gate-CLOSED one from adding a `curv` painter to
//        an unrelated object.  The gate-invariance CONTROL row stays
//        green at 0.073, which is what makes the 304.81 attributable to
//        `Po` alone rather than to the receiver's painter.  (Measured
//        before the unification and not re-run: family D is the only
//        family keyed on `Po`, and neither it nor the gate-invariance
//        block changed.)
//
//    THE SITE-TO-ROW MAP, established by isolating each remaining site.
//    Every one of the seven converted sites has a row that sees it,
//    except the two named in (5) and (6):
//
//    (8) BDPT's `type == LIGHT` root vertex (`if( ls.surface.valid )`
//        forced false).  6 FAILs: A/BDPT 74.81, A/BDPT-spectral 75.03,
//        A/BDPT-spec+HWSS 74.56, C/BDPT 7.99, E/BDPT 75.01,
//        F/BDPT 68.07.  Every VCM and PT row stays green.
//
//    (9) VCM's light-vertex NEE record (`ApplyEmitterSurface` skipped).
//        5 FAILs: A/VCM 56.84, A/VCM-spectral 64.55, C/VCM 8.64,
//        E/VCM 57.21, F/VCM 59.21.  Every BDPT row stays green.
//
//   (10) The RGB NEE site (`ApplyEmitterSurface` skipped in
//        `LightSampler::EvaluateDirectLighting`).  8 FAILs: A/PT 75.31,
//        B/PT 75.32, C/PT 44.44, E/PT 75.30, F/PT 67.77 and the A, B
//        and E PT sensitivity rows (23.46 / 23.43 / 23.59).  Every
//        BDPT, VCM and spectral row stays green.
//
//   (11) THE UNIFICATION ITSELF: restore the pre-round-2
//        along-`vToLight` NEE probe -- same acceptance tolerance, same
//        cross-object stamp, same gate, only fired from
//        `ri.ptIntersection` toward `ptOnLum` with no travel limit
//        instead of from `ptOnLum + standoff*n` back along `-n`.
//        1 FAIL, and it is family F alone: F/PT 23.24, with
//        F/BDPT 0.00065 and F/VCM 0.0069.  Every other row in the suite
//        stays inside the band, which is what makes the failure
//        attributable to the probe's DIRECTION rather than to anything
//        else about family F.
//
//    WHAT (11) DOES NOT SHOW, stated because the review predicted it
//    would.  H2 P1-1 reasoned that the along-ray probe refuses at
//    GRAZING incidence, the `delta / cos(theta)` term exceeding the 1 %
//    acceptance tolerance below cos(theta) ~ 0.2 on family E's
//    `epsilon 0.002`.  That does not reproduce: family E is green
//    there, and an exploratory sweep of the same sphere to
//    `epsilon 0.05` -- an SDF hit band FIVE TIMES the acceptance
//    tolerance at normal incidence -- left every row green as well.
//    `SDFGeometry`'s sampled point and its marched hit evidently agree
//    far more tightly than that arithmetic assumes.  The asymmetry the
//    review found is real; the mechanism family F uses to expose it is
//    the wrong-surface window, not incidence.
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <limits>
#include <string>
#include <algorithm>
#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Interfaces/SurfaceSignalProximity.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
// For family D's gate-invariance block: the two process-wide counters
// `LightSampler::EmitterProbeWanted()` reads.  The block asserts they are
// at zero before each gate-CLOSED render, so a leaked registration cannot
// make the comparison vacuous.
#include "../src/Library/Utilities/SurfaceCurvature.h"
#include "../src/Library/Interfaces/ISurfaceSignalProvider.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

//! SEEDING (P2-6 of the S3 round-3 comment review).  This binary does not
//! go through src/RISE/commandconsole.cpp's `main()` -- the only place in
//! the tree that calls `srand( GetMilliseconds() )` -- so RISE renders
//! here are NOT wall-clock seeded; what actually varies run to run is
//! every render worker's `RandomNumberGenerator random;` default argument
//! `seed = rand()` racing unsynchronized across `ThreadPool::ParallelFor`
//! threads.  tests/SignalIntegratorConsistencyTest.cpp,
//! tests/FabricRenderTest.cpp and tests/PrimitiveSelfHitTest.cpp hit the
//! same fact and adopt the same fix, used here too: an optional seed base
//! (`argv[1]`, default `kDefaultSeedBase`) with
//! `std::srand( g_seedBase + g_renderIndex++ )` immediately before every
//! `Rasterize()` call, so the default invocation is reproducible in
//! intent and `./SignalEmitterRecordTest 2000` is an independent sample
//! by construction rather than by hoping the worker-side race decorrelates
//! it.
static const unsigned int kDefaultSeedBase = 1000u;
static unsigned int g_seedBase = kDefaultSeedBase;
static unsigned int g_renderIndex = 0;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- the in-process capture the balance
// suites use.  Stores the LINEAR radiance buffer, so nothing is
// tone-mapped or quantised before the means are taken.
//
// `oidn_denoise FALSE` on every rasterizer string below is not
// optional: the default `OutputDenoisedImage` forwards POST-denoise
// pixels to `OutputImage`, so with the denoiser on this suite would be
// measuring OIDN, not the integrator (the trap swept across seven
// suites in 2026-08).
//////////////////////////////////////////////////////////////////////
class CapturingRasterizerOutput
	: public virtual IRasterizerOutput
	, public virtual Reference
{
public:
	std::vector<RISEColor> pixels;
	unsigned int width;
	unsigned int height;

	CapturingRasterizerOutput() : width(0), height(0) {}

protected:
	virtual ~CapturingRasterizerOutput() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

	virtual void OutputImage(
		const IRasterImage& pImage,
		const Rect*,
		const unsigned int ) override
	{
		width = pImage.GetWidth();
		height = pImage.GetHeight();
		pixels.resize( width * height );
		for( unsigned int y = 0; y < height; y++ ) {
			for( unsigned int x = 0; x < width; x++ ) {
				pixels[y * width + x] = pImage.GetPEL( x, y );
			}
		}
	}
};

struct ImageStats
{
	double mean[3];
	bool   valid;
};

//! Composited-over-black radiance (base * coverage-alpha) -- the
//! convention-independent quantity, because PT and BDPT resolve
//! partial-coverage pixels with different alpha conventions.  Every
//! pixel here is full-coverage receiver, so this is a no-op; it is kept
//! so the statistic means the same thing as in the balance suites.
static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) {
		return s;
	}
	double sum[3] = { 0, 0, 0 };
	// DL-40: a nonfinite (NaN/Inf) captured component is a broken render,
	// not a statistic -- reject the whole capture (return invalid) before
	// it poisons the mean.  See the matching fix/comment in
	// BDPTStrategyBalanceTest.cpp's ComputeStats (same sibling pattern).
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		const double r = c.base.r * cov, g = c.base.g * cov, b = c.base.b * cov;
		if( !std::isfinite( r ) || !std::isfinite( g ) || !std::isfinite( b ) ) {
			return ImageStats{};   // valid stays false
		}
		sum[0] += r;
		sum[1] += g;
		sum[2] += b;
	}
	const double n = double( cap.pixels.size() );
	for( int c = 0; c < 3; c++ ) {
		s.mean[c] = sum[c] / n;
	}
	s.valid = true;
	return s;
}

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/signal_emitter_record_%s_%d.RISEscene",
		tag, static_cast<int>(::getpid()) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderAndComputeStats( const char* scenePath )
{
	ImageStats result{};

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( scenePath ) ) {
		safe_release( pJob );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	// See the seeding comment at g_seedBase's declaration: this binary is
	// not wall-clock seeded, so the starting `rand()` state for this
	// render's worker threads is set explicitly here rather than left to
	// the unsynchronized libc race.
	std::srand( g_seedBase + g_renderIndex++ );
	const bool bRendered = pJob->Rasterize();
	if( !bRendered ) {
		safe_release( pCap );
		safe_release( pJob );
		return result;
	}

	result = ComputeStats( *pCap );

	safe_release( pCap );
	safe_release( pJob );
	return result;
}

//////////////////////////////////////////////////////////////////////
// SCENE ASSEMBLY
//
// Every document is built from the same four pieces, so two documents
// in one row differ in EXACTLY the exitance expression token.
//////////////////////////////////////////////////////////////////////

//! Receiver + camera.  The receiver spans [-4,4]^2 so the 30-degree
//! camera (which sees +-0.94 at z = 0) is filled edge to edge by it --
//! every pixel in the mean is receiver, none is background.
static const char* kCommonHead =
	"RISE ASCII SCENE 7\n"
	"film\n"
	"{\n"
	"\twidth 40\n"
	"\theight 40\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"uniformcolor_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\tcolor 0.6 0.6 0.6\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname g_receiver\n"
	"\tpta -4 -4 0\n"
	"\tptb 4 -4 0\n"
	"\tptc 4 4 0\n"
	"\tptd -4 4 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_receiver\n"
	"\tgeometry g_receiver\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n";

//! THE SAME HEAD with the receiver's albedo supplied by a `curv`-keyed
//! `expression_painter` instead of a `uniformcolor_painter`.
//!
//! It exists to OPEN THE PROBE GATE without changing a single pixel of
//! the receiver.  `curv` is exactly 0 on a planar primitive (the
//! descriptor says so, and `clippedplane_geometry` reports no curvature
//! at all), so `0.6 * (1 - clamp(curv,0,1))` is 0.6 everywhere -- the
//! same constant `pnt_albedo` carries -- while the compiled program's
//! `curv` reference bumps `SurfaceCurvatureDemand` and therefore flips
//! `EmitterProbeWanted()` from false to true process-wide.
//!
//! The expression is written as a MULTIPLY BY A CLAMP rather than
//! `0.6 + 0.0*curv` deliberately: a constant-folding pass may delete a
//! term multiplied by a literal zero, and with it the demand this head
//! exists to register.
static const char* kCommonHeadSignalReceiver =
	"RISE ASCII SCENE 7\n"
	"film\n"
	"{\n"
	"\twidth 40\n"
	"\theight 40\n"
	"}\n"
	"\n"
	"pinhole_camera\n"
	"{\n"
	"\tlocation 0 0 3.5\n"
	"\tlookat 0 0 0\n"
	"\tup 0 1 0\n"
	"\tfov 30.0\n"
	"}\n"
	"\n"
	"expression_painter\n"
	"{\n"
	"\tname pnt_albedo\n"
	"\texpr vec3( 0.6*(1.0-clamp(curv,0,1)), 0.6*(1.0-clamp(curv,0,1)), 0.6*(1.0-clamp(curv,0,1)) )\n"
	"}\n"
	"\n"
	"lambertian_material\n"
	"{\n"
	"\tname mat_diffuse\n"
	"\treflectance pnt_albedo\n"
	"}\n"
	"\n"
	"clippedplane_geometry\n"
	"{\n"
	"\tname g_receiver\n"
	"\tpta -4 -4 0\n"
	"\tptb 4 -4 0\n"
	"\tptc 4 4 0\n"
	"\tptd -4 4 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_receiver\n"
	"\tgeometry g_receiver\n"
	"\tmaterial mat_diffuse\n"
	"}\n"
	"\n";

//! The emitter material.  `scale` is chosen per family so the receiver
//! mean lands around 0.3-0.6 in linear radiance -- bright enough that
//! the mean is dominated by signal rather than by the black tail, dim
//! enough that nothing clips.
static std::string MakeEmitterMaterial( const char* exitanceExpr, double scale )
{
	char buf[1024];
	std::snprintf( buf, sizeof(buf),
		"expression_painter\n"
		"{\n"
		"\tname p_exitance\n"
		"\texpr vec3( %s, %s, %s )\n"
		"}\n"
		"\n"
		"lambertian_luminaire_material\n"
		"{\n"
		"\tname mat_lum\n"
		"\texitance p_exitance\n"
		"\tscale %g\n"
		"\tmaterial none\n"
		"}\n"
		"\n",
		exitanceExpr, exitanceExpr, exitanceExpr, scale );
	return std::string( buf );
}

//! FAMILY A: the emitter is an SDF sphere (`sdf_geometry`, one sphere
//! part).  The SDF family answers `curv` from its own field, and is the
//! family docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5 names.
static const char* kEmitterSdfSphere =
	"sdf_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tpart sphere union 0  0 0 0  0 0 0  1 1 1  0.5 0 0  0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"}\n"
	"\n";

//! FAMILY B: the same row on an ANALYTIC primitive, so the probe is
//! shown to work for a non-SDF luminary -- `sphere_geometry` reports
//! `curv` through the Weingarten map (dndu/dndv), a completely
//! different code path from the SDF field's Hessian, and the two must
//! both arrive at the record.
static const char* kEmitterAnalyticSphere =
	"sphere_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tradius 0.5\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"}\n"
	"\n";

//! FAMILY C: a flat emitter quad with a wide box parked a uniform
//! h = 0.2 in FRONT of it (further from the receiver, so it never
//! occludes the light path; it is identical between the two documents
//! in any case and cancels).
//!
//! The box spans x in [-1,1], y in [0.8,2.8], z in [1.4,3.4]; the
//! emitter quad is x in [-0.4,0.4], y in [1.4,2.2], z = 1.2 -- inside
//! the box's lateral extent everywhere, so the nearest box point from
//! ANY point of the quad is straight along +z at exactly 0.2.
//! `proximity(1.0)` is therefore exactly 1 - 0.2/1.0 = 0.8 over the
//! whole emitter.
//!
//! The receiver is the only other candidate and sits 1.2 away -- beyond
//! the 1.0 radius -- so it cannot perturb the closed form.
static const char* kEmitterQuadWithNeighbour =
	"clippedplane_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tpta -0.4 -0.4 0\n"
	"\tptb 0.4 -0.4 0\n"
	"\tptc 0.4 0.4 0\n"
	"\tptd -0.4 0.4 0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"}\n"
	"\n"
	"box_geometry\n"
	"{\n"
	"\tname g_neighbour\n"
	"\twidth 2.0\n"
	"\theight 2.0\n"
	"\tdepth 2.0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_neighbour\n"
	"\tgeometry g_neighbour\n"
	"\tmaterial mat_diffuse\n"
	"\tposition 0 1.8 2.4\n"
	"}\n"
	"\n";

//! FAMILY D: the SAME SDF sphere as family A, but the exitance keys on
//! `Po` -- the object-space hit point -- rather than on a signal.
//!
//! WHY THIS ROW EXISTS (transport review of slice S3, P1-1).  `Po` rode
//! on the probe's GATED payload in this slice's first draft, which made
//! an emissive material keyed on it render 2.94x differently depending
//! on whether any signal painter anywhere in the process was alive.  It
//! is now filled by `LightSampler::EmitterObjectPoint` -- ungated and
//! ray-free -- at every emitter-record site, which ALSO closes a
//! PRE-EXISTING inconsistency older than this slice: a camera ray that
//! HIT the emitter read a live `Po` while every NEE record built from
//! the same sampled point read `(0,0,0)`.
//!
//! CLOSED FORM.  Every point of a sphere of radius 0.5 centred on its
//! object origin satisfies `|Po|^2 = 0.25` EXACTLY, so
//! `clamp(8*|Po|^2, 0, 1)` saturates to exactly 1 -- the 8 (rather than
//! 4) gives 2x headroom so the row tests the RECORD, not the last digit
//! of the SDF's Newton projection.  Control `1.0`; neutral `0.0`, since
//! `Po = (0,0,0)` is what a hand-built record used to carry.
//!
//! NO SIGNAL PAINTER APPEARS IN THIS SCENE, which is the point: the
//! probe gate is CLOSED for every row of this family, and the row still
//! has to pass.
static const char* kEmitterSdfSpherePo = kEmitterSdfSphere;

//! FAMILY E: the SAME unit-scaled SDF sphere as family A, at a SURFACE
//! EPSILON of 0.002 of the diagonal instead of the 5e-5 default.
//!
//! WHY THIS ROW EXISTS (transport review of slice S3, P1-3).  The
//! normal-aligned probe -- the one the BDPT light-subpath root and VCM's
//! light vertex go through -- stands off the surface before firing back
//! at it, and this slice's first draft used a flat `0.001 * diagonal` on
//! the claim that it sat "~9 orders above every geometry's
//! SelfHitRootFloor".  `SelfHitRootFloor` is VIRTUAL with eleven in-tree
//! overrides and that claim held only for the INTERFACE DEFAULT.
//! `SDFGeometry`'s is `min( 2*m_eps/shrink/cosI, 0.5*diagonal )` with
//! `m_eps = max( diagonal*m_epsFrac, 1e-6 )`, so at `epsilon 0.002` it
//! is `2 * 0.002 = 0.004` of the diagonal -- FOUR TIMES the old flat
//! standoff.  Below its own floor the SDF treats the probe origin as
//! spawned ON the surface and marches it forward in `4*m_eps` steps
//! until it clears the band, i.e. straight past the face it was aimed
//! at: the probe misses and REFUSES.
//!
//! THE RESULTING DISAGREEMENT IS THE ONE THIS SLICE EXISTS TO REMOVE.
//! The NEE probe stands off not at all, so PT keeps reading the
//! emitter's `curv` LIVE while BDPT and VCM read it NEUTRAL -- exactly
//! what the narrowed containment warning now claims no longer happens.
//! The row therefore runs under all three, and the header's red-proof
//! records what restoring the flat standoff does to each.
//!
//! WHY EPSILON AND NOT A NON-UNIFORMLY SCALED PART.  A part authored
//! `scale 0.1 1 1` raises the same floor by the same mechanism (the
//! field's Lipschitz `shrink` divides it), and was tried first -- but it
//! also squashes the sphere into a 10:1 oblate spheroid, and `curv` is
//! then NOT the sphere's radius-independent `2*sqrt(3)`.  The measured
//! `clamp(curv,0,1)` did not saturate there (PT read 27.5 % off its
//! baked-1.0 control, with the probe working), so the family loses its
//! exact closed-form control and the row would be testing the curvature
//! estimator rather than the record.  Epsilon moves the floor without
//! moving the shape.
//!
//! 0.002 rather than something larger is bounded from ABOVE by the
//! probe's acceptance tolerance: the march stops at `|Map| <= m_eps`, so
//! the hit can sit `m_eps / shrink = 0.002 * diagonal` short of the true
//! surface, against an acceptance window of `0.01 * diagonal`.  Five
//! times the error, four times the old standoff -- the widest separation
//! the two constraints leave.
static const char* kEmitterSdfCoarseEps =
	"sdf_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tepsilon 0.002\n"
	"\tpart sphere union 0  0 0 0  0 0 0  1 1 1  0.5 0 0  0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"}\n"
	"\n";

//! FAMILY F: a TWO-LOBE (non-convex) SDF emitter with
//! `casts_shadows FALSE`, keyed on `curv`.  THE WITNESS FOR THE
//! UNIFIED PROBE (round-2 transport review of slice S3, H2 P1-1).
//!
//! WHY THIS ROW EXISTS.  Until that review there were TWO emitter
//! probes with DIFFERENT refusal predicates: the two NEE sites fired
//! along `vToLight`, from the shading point toward the sampled point,
//! with no travel limit; `SampleLight` -- and therefore the BDPT
//! light-subpath root, its two NM twins and VCM's light vertex --
//! stood off along the sampled normal and fired back.  An emitter
//! that refuses one and not the other renders with PT reading its
//! signals LIVE and BDPT / VCM reading them NEUTRAL on the same
//! surface in the same frame, or the reverse.  This family makes that
//! happen and then requires it not to.
//!
//! THE MECHANISM IS THE WRONG-SURFACE WINDOW, not grazing incidence.
//! The review predicted the along-`vToLight` probe would refuse at
//! grazing (`delta / cos(theta)` exceeding the acceptance tolerance),
//! and that prediction does NOT reproduce: family E is green at
//! `epsilon 0.002`, and an exploratory sweep of the same sphere to
//! `epsilon 0.05` -- an SDF hit band five times the acceptance
//! tolerance at NORMAL incidence -- left every row green as well, so
//! the sampled point and the marched hit agree far more tightly than
//! that arithmetic assumes.  What the along-ray probe really fails is
//! aiming down the LINE OF SIGHT: on a non-convex luminary the
//! closest hit along that line can be a different part of the same
//! surface, whose curvature / occlusion / proximity are a different
//! material state, and the probe then correctly refuses -- leaving PT
//! alone reading neutral.  The normal-aligned probe is never aimed
//! down that line and cannot be intercepted except within its own
//! standoff band.
//!
//! THE GEOMETRY, and why each number is what it is.  One
//! `sdf_geometry` with two disjoint sphere parts on the line from the
//! receiver to the emitter: a NEAR lobe of radius 0.5 at the family
//! A/B/D/E emitter position (0, 1.8, 1.2), 2.163 from the receiver's
//! centre, and a FAR lobe of radius 0.64 at 3.6 along the same ray
//! (local offset (0, 1.196, 0.797)).  The gap between them is 0.30,
//! nine times the acceptance tolerance (1 % of the 3.294 bbox
//! diagonal), so a probe that lands on the wrong lobe is refused, not
//! quietly accepted.
//!
//! THE NEAR LOBE HIDES THE FAR ONE: angular radius asin(0.5/2.163) =
//! 13.4 degrees against asin(0.64/3.6) = 10.2, so every
//! line-of-sight probe aimed at a far-lobe sample from near the
//! receiver's centre is intercepted by the near lobe.  The far lobe
//! still carries about a third of the energy -- area goes as r^2 and
//! irradiance as 1/d^2, so its weight is (0.64/3.6)^2 against the
//! near lobe's (0.5/2.163)^2, a ratio of 0.59 -- which is what turns
//! a refusal into a measurable mean shift.  It cannot be made to
//! carry MORE than half: the same r/d that makes the near lobe
//! occlude makes it brighter.
//!
//! `casts_shadows FALSE` IS LOAD-BEARING, and it is the documented
//! hazard, not a contrivance: `ProbeEmitterSurface`'s own comment
//! used to note that the NEE shadow test narrowed the wrong-surface
//! window "only when the luminary casts shadows".  With shadows on,
//! a far-lobe sample is occluded by the near lobe, discarded, and
//! contributes to neither document -- so the hazard the along-ray
//! probe carried is exactly the one a no-shadow luminary exposes.
//!
//! CLOSED-FORM CONTROL, same as families A / B / E.  `curv` is
//! `H * scaleHint`; on a sphere of radius r, H = 1/r, and scaleHint
//! is the WHOLE geometry's bbox diagonal 3.294 -- so the near lobe
//! reads 6.59 and the far lobe 5.15, and `clamp(curv,0,1)` saturates
//! to exactly 1 on both with five times the headroom the clamp needs.
//! The control is the literal `1.0`; the neutral is `0.0`.
//!
//! BOTH LOBES STAY OUT OF FRAME.  The camera is at (0,0,3.5) with a
//! 30-degree fov; the near lobe subtends 9.9 degrees about an axis 38
//! degrees off the view direction and the far lobe 11 degrees about
//! one 63 degrees off, so neither reaches the 15-degree half-angle.
//! No camera ray hits the emitter, exactly as in every other family.
static const char* kEmitterSdfTwoLobe =
	"sdf_geometry\n"
	"{\n"
	"\tname g_lum\n"
	"\tpart sphere union 0  0 0 0  0 0 0  1 1 1  0.5 0 0  0\n"
	"\tpart sphere union 0  0 1.196 0.797  0 0 0  1 1 1  0.64 0 0  0\n"
	"}\n"
	"\n"
	"standard_object\n"
	"{\n"
	"\tname obj_lum\n"
	"\tgeometry g_lum\n"
	"\tmaterial mat_lum\n"
	"\tposition 0 1.8 1.2\n"
	"\tcasts_shadows FALSE\n"
	"}\n"
	"\n";

//! THE INTEGRATOR CHUNKS, built rather than hard-coded, because three
//! things vary across the rows: the integrator, the sample count (row C
//! is the dimmest family and needs more; see the band derivation in the
//! header) and RGB-versus-spectral.
//!
//! Everything else is held fixed -- the same `standard_shader`, the same
//! `pixel_filter box`, the same `oidn_denoise FALSE` -- so a row's EXPR
//! and CONTROL can never differ by a rasterizer setting.  (No row
//! compares one integrator to another; see the header.)
//!
//! THE SPECTRAL KINDS EXIST FOR THE NM SITES.  `EvaluateDirectLightingNM`,
//! `GenerateLightSubpathImpl`'s NM hero `Le` rebuild and its HWSS
//! companion-wavelength rebuild are three of the seven emitter-record
//! sites this slice converts, and NOTHING in an RGB rasterizer reaches
//! any of them.  The chunk parameters are copied from
//! tests/EnvLightBalanceTest.cpp's spectral rows (380-720 nm, 8
//! wavelengths, 1 spectral sample) minus its env-only `radiance_map`.
//! `hwss true` additionally turns on the companion-wavelength bundle, the
//! only route to the third of those sites.
enum RastKind
{
	eRK_PT = 0,
	eRK_BDPT,
	eRK_VCM,
	eRK_PT_SPECTRAL,
	eRK_BDPT_SPECTRAL,
	eRK_VCM_SPECTRAL
};

static const char* RastName( RastKind k, bool hwss )
{
	switch( k ) {
	case eRK_PT:            return "PT";
	case eRK_BDPT:          return "BDPT";
	case eRK_VCM:           return "VCM";
	case eRK_PT_SPECTRAL:   return hwss ? "PT-spectral/HWSS"   : "PT-spectral";
	case eRK_BDPT_SPECTRAL: return hwss ? "BDPT-spectral/HWSS" : "BDPT-spectral";
	case eRK_VCM_SPECTRAL:  return hwss ? "VCM-spectral/HWSS"  : "VCM-spectral";
	}
	return "?";
}

static std::string MakeRasterizer( RastKind k, int samples, bool hwss )
{
	static const char* kShader =
		"standard_shader\n"
		"{\n"
		"\tname global\n"
		"\tshaderop DefaultPathTracing\n"
		"}\n"
		"\n";
	static const char* kOutput =
		"\n"
		"file_rasterizeroutput\n"
		"{\n"
		"\tpattern rendered/signal_emitter_record_unused\n"
		"\ttype EXR\n"
		"\tbpp 32\n"
		"\tcolor_space Rec709RGB_Linear\n"
		"}\n";

	const bool bSpectral =
		( k == eRK_PT_SPECTRAL || k == eRK_BDPT_SPECTRAL || k == eRK_VCM_SPECTRAL );
	const bool bBDPT = ( k == eRK_BDPT || k == eRK_BDPT_SPECTRAL );
	const bool bVCM  = ( k == eRK_VCM  || k == eRK_VCM_SPECTRAL  );

	const char* keyword =
		( k == eRK_PT )            ? "pathtracing_pel_rasterizer" :
		( k == eRK_BDPT )          ? "bdpt_pel_rasterizer" :
		( k == eRK_VCM )           ? "vcm_pel_rasterizer" :
		( k == eRK_PT_SPECTRAL )   ? "pathtracing_spectral_rasterizer" :
		( k == eRK_BDPT_SPECTRAL ) ? "bdpt_spectral_rasterizer" :
		                             "vcm_spectral_rasterizer";

	std::string s( kShader );
	s += keyword;
	s += "\n{\n";

	char buf[256];
	std::snprintf( buf, sizeof(buf), "\tsamples %d\n", samples );
	s += buf;

	if( bBDPT || bVCM ) {
		s += "\tmax_eye_depth 3\n\tmax_light_depth 3\n";
	}
	if( k == eRK_PT ) {
		s += "\trr_min_depth 8\n";
	}
	if( k == eRK_PT_SPECTRAL ) {
		s += "\tmax_diffuse_bounce 3\n";
	}
	if( bVCM ) {
		s += "\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n";
	}
	if( bSpectral ) {
		s += "\tnmbegin 380\n"
		     "\tnmend 720\n"
		     "\tnum_wavelengths 8\n"
		     "\tspectral_samples 1\n";
		s += hwss ? "\thwss true\n" : "\thwss false\n";
	}
	s += "\tpixel_filter box\n\toidn_denoise FALSE\n}\n";
	s += kOutput;
	return s;
}

static std::string MakeScene(
	const char* head,
	const char* emitterChunks,
	const char* exitanceExpr,
	double scale,
	const std::string& rasterizerChunk )
{
	std::string s( head );
	s += MakeEmitterMaterial( exitanceExpr, scale );
	s += emitterChunks;
	s += rasterizerChunk;
	return s;
}

static ImageStats Render(
	const char* head,
	const char* emitterChunks,
	const char* exitanceExpr,
	double scale,
	const std::string& rasterizerChunk,
	const char* tag )
{
	const std::string text = MakeScene( head, emitterChunks, exitanceExpr, scale, rasterizerChunk );
	const std::string path = WriteSceneToTempFile( text, tag );
	if( path.empty() ) {
		return ImageStats{};
	}
	const ImageStats s = RenderAndComputeStats( path.c_str() );
	std::remove( path.c_str() );
	return s;
}

//! Worst per-channel |a/b - 1|, with an absolute floor so a black
//! render cannot divide by zero (a black render fails the `valid` /
//! brightness checks separately).
//!
//! DL-40: a nonfinite operand on EITHER side must make the WHOLE
//! comparison read as maximally disagreeing (+Inf), not merely be
//! ignored.  std::fmax(a,b) is defined to return the OTHER operand when
//! one argument is NaN -- so accumulating "worst" via fmax across
//! channels let a NaN channel's diff vanish entirely if any other
//! channel had a larger finite value, or read as 0 (a perfect match!)
//! if every channel was NaN, since `worst` starts at 0 and
//! fmax(0,NaN)==0.  Every caller compares this return value against a
//! band with `<`/`>`, and HUGE_VAL correctly fails a `< kBand`
//! agreement check on the money test while remaining moot for a
//! `> kMinSensitivity` check reached only through an already-valid
//! (finite) ImageStats -- see ComputeStats above, which now rejects a
//! nonfinite capture before an ImageStats carrying one ever reaches
//! WorstRelDiff in production.
static double WorstRelDiff( const ImageStats& a, const ImageStats& b )
{
	for( int c = 0; c < 3; c++ ) {
		if( !std::isfinite( a.mean[c] ) || !std::isfinite( b.mean[c] ) ) return HUGE_VAL;
	}
	double worst = 0;
	for( int c = 0; c < 3; c++ ) {
		const double denom = std::fmax( std::fabs( b.mean[c] ), 1e-6 );
		worst = std::fmax( worst, std::fabs( a.mean[c] - b.mean[c] ) / denom );
	}
	return worst;
}

static void PrintMeans( const char* label, const ImageStats& s )
{
	if( !s.valid ) {
		std::cout << "    " << label << ": INVALID (render failed)" << std::endl;
		return;
	}
	std::cout << "    " << label << ": mean=("
	          << s.mean[0] << ", " << s.mean[1] << ", " << s.mean[2] << ")" << std::endl;
}

//! THE BAND.  See the file header for the derivation.
static const double kBand = 0.05;

//! The sensitivity rows must sit at least this far outside the band, so
//! "the signal is live" cannot be confused with "the signal does not
//! matter here".  The constructed swing is 76-80 %; requiring 5x the
//! band (25 %) leaves an enormous margin and is the figure design §6.1
//! names.
static const double kMinSensitivity = 0.25;

//////////////////////////////////////////////////////////////////////
// One family = one emitter + one signal + its known constant.
//////////////////////////////////////////////////////////////////////
struct RowSpec
{
	RastKind	kind;
	bool		hwss;
	int			samples;	//!< 0 = use the family's own count
};

struct Family
{
	const char*		name;
	const char*		head;			//!< receiver + camera; kCommonHead, or the curv-keyed variant
	const char*		emitterChunks;
	const char*		exprSignal;		//!< the live expression
	const char*		exprControl;	//!< the same, signal replaced by its KNOWN CONSTANT
	const char*		exprNeutral;	//!< the same, signal replaced by its NEUTRAL value
	double			scale;
	int				samples;
	const RowSpec*	rows;
	int				nRows;
};

static void RunFamily( const Family& f )
{
	std::cout << "  [" << f.name << "]" << std::endl;

	for( int i = 0; i < f.nRows; i++ ) {
		const int spp = f.rows[i].samples > 0 ? f.rows[i].samples : f.samples;
		const std::string rast = MakeRasterizer( f.rows[i].kind, spp, f.rows[i].hwss );
		const ImageStats expr = Render(
			f.head, f.emitterChunks, f.exprSignal, f.scale, rast, "expr" );
		const ImageStats ctrl = Render(
			f.head, f.emitterChunks, f.exprControl, f.scale, rast, "ctrl" );

		std::string lbl = std::string( f.name ) + " / " + RastName( f.rows[i].kind, f.rows[i].hwss );

		Check( expr.valid && ctrl.valid, ( lbl + ": both renders produced an image" ).c_str() );
		if( !expr.valid || !ctrl.valid ) {
			continue;
		}
		PrintMeans( ( lbl + " EXPR   " ).c_str(), expr );
		PrintMeans( ( lbl + " CONTROL" ).c_str(), ctrl );

		// A dark render would make the ratio meaningless -- and would
		// itself be the bug (an emitter reaching nothing).
		Check( ctrl.mean[0] > 0.02,
		       ( lbl + ": the control render is actually lit (mean > 0.02)" ).c_str() );

		const double d = WorstRelDiff( expr, ctrl );
		std::cout << "      worst |EXPR/CONTROL - 1| = " << ( d * 100.0 ) << " %" << std::endl;
		Check( d < kBand,
		       ( lbl + ": MONEY -- the signal-keyed emitter matches its baked-constant "
		               "twin, i.e. the emitter record read the signal LIVE" ).c_str() );
	}

	// SENSITIVITY, on the PT row: the same expression with the signal
	// replaced by what a NEUTRAL read would give must move the mean far
	// outside the band.  Without this the whole family could pass by
	// being insensitive to the signal in the first place.
	{
		const std::string rast = MakeRasterizer( eRK_PT, f.samples, false );
		const ImageStats expr = Render(
			f.head, f.emitterChunks, f.exprSignal, f.scale, rast, "expr" );
		const ImageStats neut = Render(
			f.head, f.emitterChunks, f.exprNeutral, f.scale, rast, "neut" );
		std::string lbl = std::string( f.name ) + " / PT sensitivity";
		Check( expr.valid && neut.valid, ( lbl + ": both renders produced an image" ).c_str() );
		if( expr.valid && neut.valid ) {
			PrintMeans( ( lbl + " NEUTRAL" ).c_str(), neut );
			const double d = WorstRelDiff( expr, neut );
			std::cout << "      |EXPR/NEUTRAL - 1| = " << ( d * 100.0 ) << " %" << std::endl;
			Check( d > kMinSensitivity,
			       ( lbl + ": the signal moves the mean far outside the band, so a "
			               "neutral read cannot hide inside it" ).c_str() );
		}
	}
}

//////////////////////////////////////////////////////////////////////
// DL-40 red-proof: nonfinite candidate statistics must be REJECTED, not
// silently accepted (ComputeStats) or silently discarded by fmax
// (WorstRelDiff).  Sibling of BDPTStrategyBalanceTest.cpp's identically
// named test -- explicit malformed fixtures, not a live render.
//////////////////////////////////////////////////////////////////////
static void TestNonfiniteCandidateRejected()
{
	std::cout << std::endl << "-- DL-40: nonfinite candidate statistics are rejected --" << std::endl;

	// (1) ComputeStats.
	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 2; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.5, 0.5, 0.5 ), 1.0 ) );
		cap->pixels.push_back( RISEColor( RISEPel( std::nan(""), 0.2, 0.2 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( !s.valid, "DL-40: ComputeStats rejects a capture with a NaN pixel component (valid==false)" );
		cap->release();
	}
	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 1; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.3, std::numeric_limits<double>::infinity(), 0.3 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( !s.valid, "DL-40: ComputeStats rejects a capture with an Inf pixel component (valid==false)" );
		cap->release();
	}
	{
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		cap->width = 1; cap->height = 1;
		cap->pixels.push_back( RISEColor( RISEPel( 0.4, 0.5, 0.6 ), 1.0 ) );
		const ImageStats s = ComputeStats( *cap );
		Check( s.valid, "DL-40: ComputeStats control -- an all-finite capture stays valid" );
		cap->release();
	}

	// (2) WorstRelDiff: a NaN mean on EITHER side must read as a huge
	// (band-failing) diff -- the specific defect named by the ledger
	// row is std::fmax silently discarding the NaN and returning a
	// small (or zero) "worst" value instead.
	{
		ImageStats a{}; a.valid = true; a.mean[0] = 0.5; a.mean[1] = 0.5; a.mean[2] = 0.5;
		ImageStats b{}; b.valid = true; b.mean[0] = std::nan(""); b.mean[1] = 0.5; b.mean[2] = 0.5;
		const double d = WorstRelDiff( a, b );
		Check( d > kBand, "DL-40: WorstRelDiff reads a NaN candidate mean[0] as a huge (band-failing) diff, not 0" );
		Check( std::isinf( d ), "DL-40: WorstRelDiff returns +Inf (not a discarded/zeroed diff) for a NaN operand" );
	}
	{
		// The pathological case the row's own evidence calls out: EVERY
		// channel nonfinite.  worst starts at 0, and fmax(0,NaN)==0 on
		// every channel, so the unguarded implementation returned
		// exactly 0.0 -- a "perfect match" for a totally broken render.
		ImageStats a{}; a.valid = true; a.mean[0] = 0.5; a.mean[1] = 0.5; a.mean[2] = 0.5;
		ImageStats b{}; b.valid = true;
		b.mean[0] = std::nan(""); b.mean[1] = std::nan(""); b.mean[2] = std::nan("");
		const double d = WorstRelDiff( a, b );
		Check( d > kBand, "DL-40: WorstRelDiff with EVERY channel NaN still reads as a huge diff, not the un-guarded 0.0" );
	}
	{
		ImageStats a{}; a.valid = true; a.mean[0] = 0.5; a.mean[1] = 0.5; a.mean[2] = 0.5;
		ImageStats b{}; b.valid = true; b.mean[0] = 0.5; b.mean[1] = 0.5; b.mean[2] = 0.5;
		const double d = WorstRelDiff( a, b );
		Check( d < kBand, "DL-40: WorstRelDiff control -- identical finite means still agree" );
	}
}

//////////////////////////////////////////////////////////////////////
// FAMILY D's EXTRA CHECK -- `Po` must NOT depend on the probe gate.
//
// `Po` is not signal state (`expression_painter` exposes it,
// `voronoi3d_painter` and `mapping_painter` key on it), and the probe's
// gate is PROCESS-WIDE, so an emissive material keyed on `Po` must
// render the same whether or not some unrelated painter elsewhere keeps
// a signal demand alive.  This renders the SAME emitter twice, changing
// only the RECEIVER's albedo painter between a plain
// `uniformcolor_painter` (no demand: the gate is CLOSED) and a
// `curv`-keyed `expression_painter` that evaluates to the SAME 0.6 on a
// planar receiver (a demand: the gate is OPEN).
//
// Two checks, in this order, because the second is only attributable
// once the first holds: the CONTROL pair proves the two receiver
// painters really are albedo-identical, and only then does the EXPR pair
// isolate `Po`.
//////////////////////////////////////////////////////////////////////
static void RunGateInvariance( const Family& f )
{
	const std::string rast = MakeRasterizer( eRK_PT, f.samples, false );

	// THE GATE MUST ACTUALLY BE CLOSED for the two "CLOSED" renders, or
	// this whole block is vacuous: a signal-reading painter leaked from
	// ANY earlier family in this process (the counters are process-wide
	// and only a painter's destructor decrements them) would open the
	// gate for all four renders, the OPEN/CLOSED comparison would be
	// open-versus-open, and it would pass no matter what `Po` did.
	// Asserted immediately before each closed render rather than once at
	// the top, because a leak could appear between them (round-2
	// transport review, H2 P2-10).
	Check( !( SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any() ),
	       "D gate-invariance: the probe gate is CLOSED before the gate-CLOSED CONTROL "
	       "render (no signal demand leaked from an earlier family)" );
	const ImageStats ctrlClosed = Render(
		kCommonHead, f.emitterChunks, f.exprControl, f.scale, rast, "gcc" );
	const ImageStats ctrlOpen = Render(
		kCommonHeadSignalReceiver, f.emitterChunks, f.exprControl, f.scale, rast, "gco" );
	Check( !( SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any() ),
	       "D gate-invariance: the probe gate is CLOSED before the gate-CLOSED EXPR "
	       "render (the gate-OPEN render above released its painter)" );
	const ImageStats exprClosed = Render(
		kCommonHead, f.emitterChunks, f.exprSignal, f.scale, rast, "gec" );
	const ImageStats exprOpen = Render(
		kCommonHeadSignalReceiver, f.emitterChunks, f.exprSignal, f.scale, rast, "geo" );

	std::cout << "  [D gate-invariance: does an UNRELATED curv painter on the RECEIVER "
	          << "move the Po-keyed emitter?]" << std::endl;
	Check( ctrlClosed.valid && ctrlOpen.valid && exprClosed.valid && exprOpen.valid,
	       "D gate-invariance: all four renders produced an image" );
	if( !ctrlClosed.valid || !ctrlOpen.valid || !exprClosed.valid || !exprOpen.valid ) {
		return;
	}
	PrintMeans( "D CONTROL / gate CLOSED", ctrlClosed );
	PrintMeans( "D CONTROL / gate OPEN  ", ctrlOpen );
	PrintMeans( "D EXPR    / gate CLOSED", exprClosed );
	PrintMeans( "D EXPR    / gate OPEN  ", exprOpen );

	const double dCtrl = WorstRelDiff( ctrlOpen, ctrlClosed );
	std::cout << "      CONTROL: |OPEN/CLOSED - 1| = " << ( dCtrl * 100.0 ) << " %" << std::endl;
	Check( dCtrl < kBand,
	       "D gate-invariance: the curv-keyed receiver painter is albedo-identical to the "
	       "plain one (so the EXPR comparison below is attributable to Po alone)" );

	const double dExpr = WorstRelDiff( exprOpen, exprClosed );
	std::cout << "      EXPR   : |OPEN/CLOSED - 1| = " << ( dExpr * 100.0 ) << " %" << std::endl;
	Check( dExpr < kBand,
	       "D gate-invariance: MONEY -- adding an unrelated curv painter somewhere else in "
	       "the scene does NOT change the Po-keyed emitter's mean" );
}

//////////////////////////////////////////////////////////////////////
// THE ROW SETS.
//
// Family A carries the SPECTRAL rows because it is the only family
// whose emitter answers a signal through every geometry path AND whose
// control is exact: the NM NEE site
// (`LightSampler::EvaluateDirectLightingNM`), BDPT's NM hero `Le`
// rebuild and its HWSS companion-wavelength rebuild are three of the
// seven emitter-record sites and NOTHING in an RGB rasterizer reaches
// any of them.
//////////////////////////////////////////////////////////////////////
static const RowSpec kRowsRGB3[3] = {
	{ eRK_PT, false, 0 }, { eRK_BDPT, false, 0 }, { eRK_VCM, false, 0 }
};
static const RowSpec kRowsPTOnly[1] = { { eRK_PT, false, 0 } };

//! THE NON-HWSS SPECTRAL ROWS CARRY THEIR OWN SAMPLE COUNT, and it is
//! forty-odd times the RGB rows'.  Not because the emitter record is
//! any noisier there, but because `spectral_samples 1` draws ONE
//! wavelength per pixel sample out of the 380-720 nm band: the
//! per-channel mean then carries a CHROMATIC MC error the RGB rows do
//! not have, measured at 4.0 % (BDPT-spectral) and 5.2 %
//! (VCM-spectral) at 48 samples against a 5 % band -- noise, not a
//! signal read, but enough to make the row meaningless either way.
//! See the file header's band derivation for the 384 / 1024 / 2048
//! ladder.  The HWSS row needs none of it (0.40 % at 48): a hero
//! wavelength with seven companions averages the bundle within each
//! sample, which is exactly the chromatic variance at issue.  It is
//! given 96 anyway, for headroom at no meaningful cost.
static const RowSpec kRowsFull[7] = {
	{ eRK_PT, false, 0 }, { eRK_BDPT, false, 0 }, { eRK_VCM, false, 0 },
	{ eRK_PT_SPECTRAL, false, 2048 },
	{ eRK_BDPT_SPECTRAL, false, 2048 },
	{ eRK_VCM_SPECTRAL, false, 2048 },
	// HWSS on the BDPT-spectral row: the companion-wavelength `rigW`
	// rebuild in `GenerateLightSubpathImpl` lives there and nowhere else.
	{ eRK_BDPT_SPECTRAL, true, 96 }
};


// DL-36 is a consistency pin of the documented bounded-neighbour read,
// not a physics fix. The real PLY scene has two blades in ONE object.
// Probing the lower blade from +normal intercepts its upper neighbour.
// Use live proximity to distinguish that record from both the sampled
// point and a neutral fallback, then check exactly what replay preserves.
static void RunBoundedNeighbourRead()
{
    std::cout << "DL-36: single-luminary two-blade probe consistency" << std::endl;
    IJobPriv* job = nullptr;
    const bool created = RISE_CreateJobPriv(&job) && job;
    Check(created, "DL-36 job created");
    if (!created) return;
    const bool loaded = job->LoadAsciiSceneViaCst(
        "scenes/Tests/Signals/emitter_louvres.RISEscene");
    Check(loaded, "DL-36 real louvred scene loaded");
    if (!loaded) { safe_release(job); return; }
    const IObject* lum = job->GetObjects()->GetItem("obj_louvres");
    Check(lum != nullptr, "DL-36 both blades have one luminary identity");
    Check(LightSampler::EmitterProbeWanted(), "DL-36 live proximity opens probe gate");
    if (!lum) { safe_release(job); return; }
    const Point3 sample(0.25, -0.25, 0);
    EmitterSurfacePayload payload;
    const bool accepted = LightSampler::ProbeEmitterSurface(
        lum, job->GetObjects(), sample, Vector3(0, 0, 1), payload);
    Check(accepted && payload.valid, "DL-36 within-band neighbour is accepted");
    if (accepted && payload.valid) {
        Check(payload.channel.pSelf == lum && payload.channel.pScene == job->GetObjects(),
              "DL-36 accepted neighbour retains luminary and scene identity");
        const Scalar z = payload.channel.ptWorld.z;
        Check(std::isfinite(z) && std::fabs(z - 0.001) < 1e-8,
              "DL-36 probe reads upper blade rather than sampled lower blade");
        const BoundingBox bb = lum->getBoundingBox();
        const Scalar diag = Vector3Ops::Magnitude(Vector3Ops::mkVector3(bb.ll, bb.ur));
        Check(z > 0 && z < 0.001 * diag && z < 0.01 * diag,
              "DL-36 actual interception lies inside standoff and acceptance bounds");
        const Scalar live = payload.channel.Proximity(0.001);
        Check(std::isfinite(live) && std::fabs(live - 0.5) < 1e-6,
              "DL-36 neighbour carries live half-strength proximity");
        Scalar sampledDistance = 0;
        Check(!job->GetObjects()->NearestOtherSurface(sample, lum, 0.001, sampledDistance),
              "DL-36 sampled lower blade has zero proximity within radius");
        std::cout << "  sample z=0  accepted z=" << z
                  << "  accepted proximity=" << live << "  expected=0.5" << std::endl;
        RayIntersectionGeometric record(Ray(Point3(0, 0, 1), Vector3(0, 0, -1)),
                                         nullRasterizerState);
        record.ptIntersection = sample;
        record.ptObjIntersec = sample;
        record.vNormal = record.vGeomNormal = Vector3(0, 0, 1);
        record.onb.CreateFromW(record.vNormal);
        record.ptCoord = Point2(0.2, 0.7);
        LightSampler::ApplyEmitterSurface(record, payload);
        Check(std::fabs(record.signals.Proximity(0.001) - 0.5) < 1e-6,
              "DL-36 replay forwards accepted live channel");
        Check(record.ptIntersection.z == 0 && record.ptObjIntersec.z == 0 &&
              record.vNormal.z == 1 && record.vGeomNormal.z == 1 &&
              record.onb.w().z == 1 && record.ptCoord.x == 0.2 && record.ptCoord.y == 0.7,
              "DL-36 replay preserves sampled geometry and UV");
    }
    EmitterSurfacePayload upper;
    const bool upperAccepted = LightSampler::ProbeEmitterSurface(
        lum, job->GetObjects(), Point3(0.25, -0.25, 0.001), Vector3(0, 0, 1), upper);
    Check(upperAccepted && upper.valid && std::fabs(upper.channel.ptWorld.z - 0.001) < 1e-8,
          "DL-36 unobstructed upper-blade sample reads itself");
    EmitterSurfacePayload lowerBack;
    const bool lowerAccepted = LightSampler::ProbeEmitterSurface(
        lum, job->GetObjects(), sample, Vector3(0, 0, -1), lowerBack);
    Check(lowerAccepted && lowerBack.valid && std::fabs(lowerBack.channel.ptWorld.z) < 1e-8,
          "DL-36 lower-blade reverse normal avoids the neighbour");
    EmitterSurfacePayload refused;
    refused.channel.ptWorld = Point3(3, 4, 5);
    Check(!LightSampler::ProbeEmitterSurface(lum, job->GetObjects(), sample,
              Vector3(0, 0, 0), refused) && !refused.valid && refused.channel.ptWorld.z == 5,
          "DL-36 unusable normal refuses without changing payload");
    safe_release(job);
    Check(!LightSampler::EmitterProbeWanted(), "DL-36 scene releases its signal demand");
}

int main( int argc, char** argv )
{
	// See the seeding comment at g_seedBase's declaration.  argv[1], if
	// given, is an alternate seed base -- the SignalIntegratorConsistencyTest
	// / FabricRenderTest / PrimitiveSelfHitTest convention for taking a
	// genuinely independent sample on demand (e.g.
	// `./SignalEmitterRecordTest 2000`).
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	std::cout << "SignalEmitterRecordTest -- emissive materials keyed on a geometry signal,"
	          << std::endl
	          << "reached ONLY through NEE / the light-subpath root"
	          << std::endl
	          << "(docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5, slice S3)"
	          << std::endl
	          << "seed base = " << g_seedBase
	          << "  (pass a different one as argv[1] for an independent sample)"
	          << std::endl << std::endl;

	// curv on a sphere = 2*sqrt(3) ~ 3.464, radius-independent, so
	// clamp(curv,0,1) is exactly 1 and the control is the literal 1.0.
	static const Family kSdfCurv = {
		"A: SDF sphere emitter, exitance keyed on curv",
		kCommonHead,
		kEmitterSdfSphere,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsFull, 7
	};

	static const Family kAnalyticCurv = {
		"B: analytic sphere_geometry emitter, exitance keyed on curv",
		kCommonHead,
		kEmitterAnalyticSphere,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsPTOnly, 1
	};

	// proximity(1.0) = 1 - 0.2/1.0 = 0.8 exactly, over the whole emitter
	// quad -- see kEmitterQuadWithNeighbour for the geometry that makes
	// the closed form uniform.
	//
	// 384 SAMPLES, eight times every other family's, for two reasons.
	// (1) It is by far the dimmest row (mean ~0.047 against ~0.39), so
	// its relative MC noise is the largest -- nine runs of the original
	// 48-sample row spread to 2.89 %, against a 5 % band; see the file
	// header's band derivation for the 48 / 192 / 384 ladder.  (2) It
	// runs BDPT and VCM as well as PT, and those two are the only rows
	// in the suite that reach the cross-object triple through
	// `LightSample::surface` -- probed once in `SampleLight` -- rather
	// than through the NEE sites' own probe call.  Both call the same
	// `ProbeEmitterSurface` since the round-2 transport review; what
	// differs is WHERE the payload is stored and which red-proof pins
	// it, (2) for the `SampleLight` route and (10) for the NEE one.
	static const Family kProximity = {
		"C: quad emitter + box neighbour at h=0.2, exitance keyed on proximity(1.0)",
		kCommonHead,
		kEmitterQuadWithNeighbour,
		"0.2 + 0.8*proximity(1.0)",
		"0.2 + 0.8*0.8",
		"0.2 + 0.8*0.0",
		60.0,
		384,
		kRowsRGB3, 3
	};

	// |Po|^2 = 0.25 exactly on a radius-0.5 sphere about its object
	// origin, so clamp(8*|Po|^2,0,1) saturates to exactly 1.  NO SIGNAL
	// PAINTER IS IN THIS SCENE: the probe gate is closed for every row
	// here, and `Po` must land anyway.
	static const Family kObjectPoint = {
		"D: SDF sphere emitter, exitance keyed on Po (NO signal painter in the scene)",
		kCommonHead,
		kEmitterSdfSpherePo,
		"0.2 + 0.8*clamp(8*(Po.x*Po.x + Po.y*Po.y + Po.z*Po.z),0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsRGB3, 3
	};

	// The self-hit-floor row: the same SDF sphere as family A at
	// `epsilon 0.002`, whose SelfHitRootFloor is 4e-3 of the diagonal --
	// four times the flat standoff this slice's first draft used.
	static const Family kShrunkSdfCurv = {
		"E: SDF sphere at epsilon 0.002 (self-hit floor 4x the old standoff), keyed on curv",
		kCommonHead,
		kEmitterSdfCoarseEps,
		"0.2 + 0.8*clamp(curv,0,1)",
		"0.2 + 0.8*1.0",
		"0.2 + 0.8*0.0",
		60.0,
		48,
		kRowsRGB3, 3
	};

	// The unified-probe row.  At HEAD~ -- with the NEE sites still
	// firing along `vToLight` -- PT read 23.2 % off its baked control
	// while BDPT and VCM stayed inside the band (red-proof (11) in the
	// file header reproduces this exact scenario by mutation on the
	// current tree and is the authoritative figure: F/BDPT 0.00065 %,
	// F/VCM 0.0069 % -- this comment used to carry a stale, differently
	// derived 0.085 % / 0.084 % pair for the same claim; keep the
	// header's number).  See the emitter's own comment for why, and the
	// red-proof list in the file header for the mutation that
	// reproduces it.
	static const Family kTwoLobe = {
		"F: two-lobe SDF emitter, casts_shadows FALSE, keyed on curv",
		kCommonHead, kEmitterSdfTwoLobe,
		"0.2 + 0.8*clamp(curv,0,1)", "0.2 + 0.8*1.0", "0.2 + 0.8*0.0",
		20.0, 48, kRowsRGB3, 3
	};
	RunBoundedNeighbourRead();
	if (argc > 1 && std::string(argv[1]) == "--louvres-only") {
		std::cout << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
		return failCount == 0 ? 0 : 1;
	}
	RunFamily( kSdfCurv );
	RunFamily( kAnalyticCurv );
	RunFamily( kProximity );
	RunFamily( kObjectPoint );
	RunGateInvariance( kObjectPoint );
	RunFamily( kShrunkSdfCurv );
	RunFamily( kTwoLobe );
	TestNonfiniteCandidateRejected();

	std::cout << std::endl
	          << "Passed: " << passCount << "  Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
