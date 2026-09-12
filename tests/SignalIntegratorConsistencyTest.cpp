//////////////////////////////////////////////////////////////////////
//
//  SignalIntegratorConsistencyTest.cpp -- docs/SIGNALS_UNDER_BIDIRECTIONAL_
//  TRANSPORT.md S2 (section 6): the money test that PT, BDPT, VCM (and,
//  as a smoke check, MLT) price a geometry-derived shading signal
//  (`curv`, `occlusion(r)`, `convexity(r)`, `thickness(r)`, `proximity(r)`,
//  `interior(r)`) THE SAME WAY.
//
//  THE BUG THIS PINS.  Before S1 (docs/SIGNALS_UNDER_BIDIRECTIONAL_
//  TRANSPORT.md section 3), `PathVertexEval::PopulateRIGFromVertex` -- the
//  helper every BDPT/VCM/MLT BSDF/pdf evaluation at a REBUILT vertex funnels
//  through -- did not copy `ri.geometric.derivatives` or `.signals` onto the
//  rebuilt record, so every painter evaluated downstream of a stored vertex
//  read the documented NEUTRAL value for all six signals, no matter what the
//  live geometry actually carried.  PT never rebuilds a vertex -- every
//  painter it evaluates sees the object manager's own stamped hit -- so it
//  is the reference-free-of-the-bug integrator this file is written against.
//  Because BDPT/VCM/MLT eye rays carry no ray differentials at all
//  (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md section 2, "BDPT/VCM/MLT
//  eye rays carry no ray differentials"), `fw`/`fwo` read 0 there both
//  before and after S1 -- a PT-vs-bidirectional difference that has nothing
//  to do with signals, so every scene below is built so NO expression reads
//  `fw`/`fwo` (no `fbm`/noise call anywhere): the six signal builtins and
//  plain arithmetic only.
//
//  STATUS (S2 follow-up, run in worktree signals-bidir-test against the
//  S1-landed tree, HEAD 5a15586a rebased onto S1's 8687bfb1): S1's widening
//  of `PopulateRIGFromVertex` (section 3) IS present in this tree, so every
//  Layer-1 row now PASSES -- the GREEN half of the red-proof.  The RED half
//  (this same file, run against a worktree BEFORE S1 landed) is recorded
//  verbatim in "OBSERVED PRE-FIX NUMBERS" below; both directions of the
//  red-proof mutation (drop `signals`, drop `derivatives`) were re-run IN
//  THIS worktree with the fix in place and are recorded in "RED-PROOF
//  PROTOCOL -- OBSERVED OUTCOMES".  Do NOT loosen any band to make a
//  BDPT/VCM row pass -- the bands are derived from PT's own run-to-run
//  noise (section "BAND DERIVATION"), not tuned to hide a regression.
//
//  A later fix round (this commit, worktree signals-bidir) addressed two
//  reviewers' findings on this file: F1 (positive rasterizer-keyword +
//  param verification), F2 (asymmetric-blowup fail-loud path), F3
//  (convexity finer-grid cross-check, which raised the control's own grid
//  from 9x9 to 21x21), F4 (n=5 masked-band re-derivation, 3-decimal
//  coverage printing), F5 (one shared `kSdfSamplingDetail` constant), G1
//  (six unit scenes, not four), G2 (corrected convexity band-derivation
//  history), G3 (pre-fix numbers re-cited verbatim from their log), G6/G7
//  (honest post-fix/masked-example ranges re-cited from two fresh runs),
//  and G9 (the design doc's actual 25% Layer-1 sensitivity band, not 3%).
//  See each finding's own comment at its fix site, and the sections below
//  they update.
//
//  A SECOND fix round (this commit, worktree signals-bidir) addressed four
//  more findings: K-P1 (the plank BDPT masked ratio's flake risk -- see
//  "BAND DERIVATION -- MASKED LAYER" for the full n=9-then-n=6 derivation;
//  the fix is averaging the masked ratio over `kLayer2MaskedSubRenders`=6
//  INDEPENDENT full sub-renders for BDPT/VCM, not a higher single-render
//  spp, which was tried first and measured to make the spread WORSE), K-P2a
//  (`FindChunkByRoleCounted` + a hard `Check(count == 1)` on both
//  `*_rasterizer` lookups, guarding against Job::RegisterAndActivateRasterizer's
//  last-parsed-chunk-wins activation semantics -- see that function's own
//  comment for the Job.cpp line citations), K-P2b (`ClassifyBlowup`'s
//  `agree` now computes the LITERAL `|ratioE/ratioB - 1|` the comment always
//  claimed, not `|meanE/meanB - 1|` -- the two only coincided because every
//  showcase measured is whole-image insensitive), and L-P2 (the convexity
//  N=9/15/27/41 sweep points, previously cited from an unsaved log, were
//  re-run with a temporary env-gated block and saved to
//  .../scratchpad/s2_convexity_sweep.txt -- one number, the 9x9-vs-15x15
//  disagreement, was a transcription error (0.366997) and is corrected to
//  the log's actual value (0.368865); every other cited value reproduced
//  bit-for-bit).  See each finding's own comment at its fix site.
//
//  A THIRD fix round (this commit, worktree signals-bidir) addressed a P1:
//  this file's seeding comments (this one included, in earlier drafts) at
//  three sites claimed every render was "wall-clock seeded" -- FALSE.
//  `RandomNumberGenerator( unsigned int seed = rand() )`
//  (src/Library/Utilities/RandomNumbers.h ~:32) and per-thread
//  construction in src/Library/Rendering/RasterizeDispatchers.h (~:147,
//  ~:201) read libc `rand()` from several worker threads with no
//  `srand()` anywhere in this binary's call path -- the run-to-run spread
//  this file relies on for independent sub-renders is an unsynchronized
//  DATA RACE, not a seeding policy.  tests/FabricRenderTest.cpp and
//  tests/PrimitiveSelfHitTest.cpp document exactly this fact and fix it
//  the same way, adopted here: an optional seed base (`argv[1]`, default
//  1000) and `std::srand( g_seedBase + g_renderIndex++ )` immediately
//  before every `Job::Rasterize()` call this file issues (Layer 1's
//  `RenderSceneText`, Layer 2's `RenderShowcaseVariant` -- once per
//  variant, so E and B each get their own seed).  The three false
//  "wall-clock" claims are corrected at their sites (see g_seedBase's own
//  declaration for the full seeding rationale).  A hard
//  `Check( ratioCount == kLayer2MaskedSubRenders )` was also added: the
//  masked sub-render loop previously `continue`d silently past a failed
//  or degenerate sub-render, which could average over fewer renders than
//  the band below was actually measured against without failing loudly.
//
//  RE-DERIVING K UNDER EXPLICIT SEEDING.  The SECOND fix round's K=6
//  choice (below) was measured against the wall-clock-race spread, which
//  turned out not to be a controlled independent sample -- moving to
//  explicit, reproducible seed bases surfaced a WIDER true spread than
//  the K=6-era numbers showed.  Six independent seed bases (1000, 2000,
//  ..., 6000; one `SIGNAL_CONSISTENCY_FILTER=plank ./bin/tests/
//  SignalIntegratorConsistencyTest <base>` run each, full logs
//  .../scratchpad/s2_plank_seeded_b1000.txt through b6000.txt) were run
//  at each of three K values in turn:
//    K=6  (unchanged from the second fix round): BDPT masked ratio mean
//         -0.0723, sample sd 0.0283, worst -0.1197.  4sigma bar: |mean|
//         + 4*sd = 0.1854 <= band (0.20) -- PASSES (margin 1.08x; every
//         margin below is band / (|mean| + 4*sd) or band / (2*|worst|)).
//         2x-worst bar: band (0.20) < 2*|worst| = 0.2394 --
//         FAILS (margin 0.84x).  VCM: mean -0.0548, sd 0.0132, worst
//         -0.0695 -- both bars pass comfortably (margins 1.86x / 1.44x).
//         BDPT-only problem, matching the K-P1 finding's own observation
//         that this metric is BDPT-only -- REJECTED (K=6 no longer
//         clears both bars once genuinely independent samples are used).
//    K=9  (.../scratchpad/s2_plank_k9_b1000.txt through b6000.txt): BDPT
//         mean -0.0676, sd 0.0183, worst -0.1024.  4sigma bar: 0.1406 <=
//         0.20 -- PASSES (margin 1.42x).  2x-worst bar: 0.2049 > 0.20 --
//         STILL FAILS, though closer (margin 0.98x).  VCM: mean -0.0548,
//         sd 0.0119, worst -0.0712 -- both bars pass (1.95x / 1.40x) --
//         REJECTED (BDPT 2x-worst bar not yet cleared).
//    K=12 (.../scratchpad/s2_plank_k12_b1000.txt through b6000.txt): BDPT
//         mean -0.0695, sd 0.0121, worst -0.0885.  4sigma bar: 0.1180 <=
//         0.20 -- PASSES (margin 1.70x).  2x-worst bar: 0.1770 <= 0.20 --
//         PASSES (margin 1.13x).  VCM: mean -0.0579, sd 0.0143, worst
//         -0.0773.  4sigma bar: 0.1149 <= 0.20 -- PASSES (margin 1.74x).
//         2x-worst bar: 0.1545 <= 0.20 -- PASSES (margin 1.29x) -- CHOSEN.
//         `kLayer2MaskedSubRenders` raised 6 -> 12.  Cost: two fresh
//         default whole-suite runs measured 104.47s and 108.69s wall (read
//         off the `time` wrapper around each run -- the cited logs are the
//         suites' stdout and carry no clock line)
//         (.../scratchpad/s2_fixround3_run1.txt, run2.txt) -- up from
//         ~70s at K=6, still under the ~2 minute budget but with less
//         headroom than before (see the KNOBS section below).
//
//  TWO LAYERS (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md section 6):
//
//  LAYER 1 -- CONSTANT-SIGNAL UNIT SCENES (section 6.1, "the sharp gate").
//  Six small inline scenes, each built so ONE signal is a KNOWN CONSTANT
//  over the entire framed region -- known either by an exact closed form
//  (curv on a sphere: `2*sqrt(3)`, independent of radius, from
//  SurfaceCurvatureTest; proximity/interior on box-family receivers: the
//  design doc's closed forms `1-h/r` and `depth/r`) or by a DIRECT,
//  non-rendering evaluation of the SAME production code the painter calls
//  (`ExpressionScalarPainter::GetValuesAt` against a hand-built
//  `SDFGeometry` hit, matching the render geometry exactly field-for-field
//  -- see "REFERENCE HARNESS" below) for occlusion/convexity/thickness,
//  which have no simple closed form on a general SDF.  The CONTROL scene
//  is the SAME expression body with the signal call replaced by that known
//  constant (not by its neutral).  For each scene and integrator I in
//  {PT, BDPT, VCM} (plus one MLT smoke row on the curv scene):
//
//      | mean(I, expr) / mean(I, control) - 1 | < band
//
//  and, on the PT row only, a SENSITIVITY check that the same expression
//  moved the mean by a wide margin between the signal's LIVE value and its
//  documented NEUTRAL, using a DELIBERATELY WIDER band than the
//  consistency check above -- design doc section 6.1's own number is 25%,
//  not the 3% consistency band (G9: an earlier draft of this file used 3%
//  here too, which is far too tight a bar for "moved by a wide margin" and
//  would have let a barely-sensitive expression pass):
//
//      | mean(PT, expr) / mean(PT, neutral-baked) - 1 | > 0.25
//
//  so the consistency check can never pass by insensitivity (a broken
//  program that always reads 0 would trivially match a control of 0).
//  Observed margins on every scene (0.54-3.92, see OBSERVED POST-FIX
//  NUMBERS below) clear 25% by a wide margin.
//
//  LAYER 2 -- SHOWCASE RATIO-OF-RATIOS (section 6.2, "the money numbers").
//  For each of plank_closeup, tidal_stones, shelf_bunny (Textures/) and
//  pavilion_colonnade (Combined/) -- loaded through the canonical CST path
//  at reduced resolution, oidn_denoise FALSE, pixel_filter box, no adaptive
//  sampling -- two text variants: E (the file as shipped, signals live) and
//  B (every signal call rewritten to its NEUTRAL constant by whole-word /
//  balanced-paren text substitution: `occlusion(...)`->`1`, `thickness(...)`
//  ->`1`, `convexity(...)`->`0`, `proximity(...)`->`0`, `interior(...)`->`0`,
//  whole-word `curv`->`0`, whole-word `curvR`->`0`).  For I in {BDPT, VCM}:
//
//      R_E = mean(I,E) / mean(PT,E);  R_B = mean(I,B) / mean(PT,B)
//      | R_E / R_B - 1 | < band
//
//  with a same-shaped sensitivity check that `mean(PT,E)/mean(PT,B) - 1` is
//  itself outside the band (a showcase that fails this is not a witness for
//  the gap and is DROPPED from the WHOLE-IMAGE assertion, with a printed
//  note, never silently passed).  Every showcase measured so far is
//  insensitive at the WHOLE-IMAGE level (see "LAYER 2 FINDING" below), so a
//  second, MASKED form of the same ratio-of-ratios is ALSO computed
//  (S2 follow-up addendum -- not in the original design doc text, but the
//  same section 6.2 spirit: "if a showcase is insensitive... "): build a
//  per-pixel mask from the two PT renders, `|PT(E)-PT(B)|/max(PT(B),eps) >
//  20%` -- the pixels the signal actually moves -- require the mask to
//  cover >= 1% of pixels (else print a note and skip, same as the
//  whole-image sensitivity drop), then compute masked means for every
//  (I, variant) and assert the SAME ratio-of-ratios restricted to the
//  masked pixels.  PT only BUILDS the mask (a selector); the invariant
//  under test still compares each integrator's OWN E vs B means, never
//  PT's, so this is not circular.  Both forms are printed; only the masked
//  form is asserted where whole-image is insensitive (which is everywhere
//  observed so far).
//
//  A per-integrator BLOW-UP GATE (section 5 of the S2 follow-up brief) runs
//  before either ratio form: if `mean(I,E)/mean(PT,E)` for I in {BDPT,VCM}
//  falls outside [0.5, 2.0], that is a KNOWN, PRE-EXISTING, NON-SIGNAL
//  integrator disagreement (VCM auto-radius instability at low spp on
//  these reduced-resolution showcases -- E and B agree with each other,
//  only the OTHER integrator disagrees with PT) -- both ratio forms are
//  SKIPPED for that (showcase, integrator), counted in `g_blowupSkipCount`,
//  and printed with an explicit "INTEGRATOR DISAGREEMENT (not
//  signal-attributable...)" line.  See "LAYER 2 FINDING" for the observed
//  skip list.
//
//  BAND DERIVATION.  Both layers assert on RENDERED MEANS, which carry
//  Monte Carlo noise even under PT.  The bands below were chosen by
//  rendering each layer-1 PT row 3 times at its final (scene, spp) and
//  reading the run-to-run spread of `mean(PT,expr)/mean(PT,control)`
//  (design doc target: <=1% noise on the mean at the chosen spp, 3% band).
//  Layer 2's WHOLE-IMAGE band (5%) is the design doc's own number, derived
//  the same way but at the coarser (160x120, low-spp) showcase resolution,
//  where Monte Carlo noise on a full-scene mean is larger.  See "BAND
//  DERIVATION -- MEASURED SPREAD" for the actual observed numbers from
//  this run, and "BAND DERIVATION -- MASKED LAYER" for the masked band.
//
//  REFERENCE HARNESS.  Occlusion/convexity/thickness have no closed form on
//  a general SDF, so their "known constant" is obtained by intersecting a
//  hand-built `SDFGeometry` -- constructed from the IDENTICAL `part` lines
//  the scene text below authors, so there is no risk of the two geometries
//  drifting apart -- with the exact ray the render camera's chief ray casts
//  at the framed station, and evaluating `occlusion(r)` / `convexity(r)` /
//  `thickness(r)` through `ExpressionScalarPainter::GetValuesAt` (the SAME
//  call a `scalar_painter { expression ... }` chunk makes at render time,
//  just invoked directly instead of through a full path trace).  This is
//  the same methodology `tests/SurfaceSignalsTest.cpp`'s `EvalAtHit` uses
//  and the same one the tidal_stones / plank_closeup showcase tests use to
//  validate a rendered ratio against a "predicted" number
//  (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md section 8's showcase notes).  A
//  strong oracle: it is not a theoretical approximation, it is the actual
//  estimator the render will call, evaluated once outside the Monte Carlo
//  loop.
//
//  RED-PROOF PROTOCOL -- OBSERVED OUTCOMES (S2 follow-up, exercised in
//  THIS worktree -- signals-bidir-test is the worker's own isolated tree,
//  never the shared checkout; each mutation was built, run with
//  `SIGNAL_CONSISTENCY_FILTER=unit`, then reverted with `git checkout --`
//  and the library rebuilt back to the clean state before continuing).
//
//  (a) Dropped `ri.signals = vertex.signals;` from `PopulateRIGFromVertex`
//      (src/Library/Utilities/PathVertexEval.h): 10 FAILs, exactly the
//      predicted set -- convexity/occlusion/thickness/proximity/interior
//      BDPT+VCM (5 scenes x 2 integrators), curv and every PT row stayed
//      GREEN.  Observed ratios (`mean(expr)/mean(control)-1`):
//      convexity BDPT -0.550, VCM -0.550; occlusion BDPT -0.822, VCM
//      -0.824; thickness BDPT +1.183, VCM +1.183; proximity BDPT -0.694,
//      VCM -0.690; interior BDPT -0.756, VCM -0.758 -- 43 passed / 10
//      failed overall, 0 unexpected reds.  Full log:
//      /private/tmp/claude-501/-Users-aravind-Working-GitHub-RISE/
//      f398a734-fdcc-484a-9edd-d32d2ce33bf5/scratchpad/
//      s2_redproof_drop_signals.txt.
//  (b) Dropped `ri.derivatives = vertex.derivatives;` (same function,
//      `signals` copy left in place): 3 FAILs, exactly the predicted set --
//      curv BDPT/VCM/MLT only, every other scene (convexity through
//      interior) and every PT row stayed GREEN.  Observed ratios: curv
//      BDPT -0.755, VCM -0.755, MLT -0.755 -- 50 passed / 3 failed
//      overall, 0 unexpected reds.  Full log: .../scratchpad/
//      s2_redproof_drop_derivatives.txt (same directory as (a)).
//
//  Both mutations were reverted (`git checkout --
//  src/Library/Utilities/PathVertexEval.h`, confirmed by an empty
//  `git status --short` and a `git diff --stat` with no output) and the
//  library rebuilt clean before any further work.
//
//  OBSERVED POST-FIX NUMBERS (this fix round -- reviewer findings F1-F5,
//  G1-G3, G6/G7, G9 below -- worktree signals-bidir, two independent
//  default runs after all of them landed; full logs .../scratchpad/
//  s2_fixround_run1.txt and run2.txt).  Layer 1: every row (PT, BDPT, VCM,
//  and the MLT smoke row), all six scenes, both runs --
//  `|mean(I,expr)/mean(I,control)-1|` stays under 2e-3 (worst observed:
//  convexity VCM -0.00189 in run1; every other scene/integrator row is
//  several times tighter, most an order of magnitude or more) --
//  comfortably inside the 3% band, the
//  GREEN half of the red-proof, exactly as section 3 predicts post-S1.
//  (G6/G7: an earlier draft of this table cited exact tiny per-scene
//  values, e.g. "thickness VCM -1.4e-6", that did not match the log they
//  claimed to cite and were Monte-Carlo noise at this sample count anyway
//  -- replaced with the honest range above, re-derived from the two runs
//  actually cited.)  PT sensitivity margins (unchanged from pre-fix within
//  noise, as expected -- PT never rebuilds a vertex, so S1 cannot move its
//  numbers): curv ~3.09, convexity ~1.25, occlusion ~3.93, thickness
//  -0.542, proximity ~2.13, interior ~3.16 -- all comfortably outside the
//  25% sensitivity band (G9 below; NOT the 3% consistency band -- the two
//  are different numbers for different checks).
//
//  Layer 2 (see "LAYER 2 FINDING" below for the full per-showcase
//  breakdown): every showcase renders at its correctly resized dimensions
//  (the film-chunk fix), every integrator marker check AND every
//  keyword/parameter read-back (F1, this fix round) passes, and the
//  masked ratio-of-ratios passes on every (showcase, integrator) row not
//  caught by the blow-up gate.  First fix round, kLayer2MaskedSubRenders=1:
//  332 passed / 0 failed / 4 blow-up skips (tidal BDPT, tidal VCM, bunny
//  VCM, pavilion VCM -- all pre-existing non-signal integrator
//  disagreements, not a signals regression; F2 below explains why none of
//  the four qualify as an ASYMMETRIC blow-up instead).  SECOND fix round,
//  kLayer2MaskedSubRenders=6 (K-P1 -- each of the 6 extra sub-renders per
//  BDPT/VCM row adds its own set of derive/render/marker/dimension
//  Checks): whole-suite total grows to 900 passed / 0 failed / 4 blow-up
//  skips on both s2_fixround2_run1.txt and run2.txt (same four rows), 0
//  regressions from the K=6 sub-render change.
//
//  OBSERVED PRE-FIX NUMBERS (historical record from the S2 test's original
//  author, worktree signals-bidir-test at ed3e6063 -- BEFORE S1 landed,
//  kept here unedited as the RED half of the red-proof this file's
//  Layer-1 assertions are written against; PT's own numbers are IDENTICAL
//  pre- and post-fix within run-to-run noise, since PT never rebuilds a
//  vertex and is therefore untouched by S1 -- compare the PT column above
//  to the PT column below).  Every number below is taken verbatim
//  (`grep ratio-1=`) from the cited log -- (G3, this fix round) an earlier
//  draft of this table had transcribed the wrong number for convexity
//  BDPT (-0.541, which is actually thickness's OWN PT sensitivity ratio
//  from the same log, not convexity's BDPT consistency ratio; the log's
//  real convexity BDPT value is -0.549953).  Full log:
//  /private/tmp/claude-501/-Users-aravind-Working-GitHub-RISE/
//  f398a734-fdcc-484a-9edd-d32d2ce33bf5/scratchpad/s2_prefix_run.txt.
//  Layer 1, `mean(I,expr)/mean(I,control) - 1` per scene/integrator:
//
//      scene       PT              BDPT         VCM          MLT (curv only)
//      curv        +0.000152       -0.755303    -0.755323    -0.755286
//      convexity   +0.014855       -0.549953    -0.550399    --
//      occlusion   -0.00000299     -0.821765    -0.823770    --
//      thickness   -0.00000086     +1.182770    +1.182760    --
//      proximity   -0.0000242      -0.692705    -0.690414    --
//      interior    -0.0000588      -0.755329    -0.757079    --
//
//  Every PT row passes the 3% band; every BDPT/VCM row (and the MLT row)
//  fails it by 55-118 percentage points -- the RED half of the red-proof,
//  exactly as section 3 predicts pre-S1.  Sensitivity margins
//  (`mean(PT,expr)/mean(PT,neutral)-1`, all comfortably outside the 25%
//  sensitivity band -- G9 below): curv 3.08708, convexity 1.25517,
//  occlusion 3.92066, thickness -0.541867, proximity 2.12706, interior
//  3.15597.  13/13 expected red rows fired, 0 unexpected reds, 117 passed
//  / 13 failed overall.
//
//  BAND DERIVATION -- MEASURED SPREAD.  Two independent PT runs at the
//  final (scene, 48 spp) settings gave `mean(PT,expr)/mean(PT,control)-1`
//  agreeing to within 1e-4 on five of six scenes (i.e. <0.05% Monte Carlo
//  noise).  Convexity is the exception, and its history is a
//  SUPERSESSION, not two formulations that were "both well inside 3%"
//  (G2, this fix round: an earlier draft of this paragraph said exactly
//  that, citing "9.3e-3 vs 1.4e-2" -- which understated the real number by
//  an order of magnitude and hid that the first formulation FAILED the
//  band).  What actually happened: convexity(r)'s estimator is a
//  32-direction discrete sample of the query ball, "spun about the normal
//  per hit so the answer is an expectation rather than a multiple of
//  1/12" (the expression_painter builtin doc's own language) -- a
//  DETERMINISTIC per-hit result (reproduced bit-for-bit across repeat
//  program runs; this is discretization bias, not wall-clock noise) that
//  is nonetheless measurably POSITION-dependent on the sphere.  The
//  file's ORIGINAL control value used a single hand-picked off-axis
//  station's ComputeConvexity answer (0.316); against that reference PT's
//  own layer-1 ratio measured 9.3% -- OUTSIDE the eventual 3% band and
//  the reason this formulation was SUPERSEDED -- because a 9-point harness
//  quadrature spanning the camera's frustum found convexity(0.3) actually
//  ranging 0.304-0.440 (a 13.6-percentage-point spread) with a 9x9 average
//  of 0.369, 9.3% away from that single station.  The quadrature-average
//  harness that superseded it (`RunConvexityQuadrature`/
//  `BuildConvexityScene`) brought PT's ratio down to ~1.4% at 9x9 --
//  inside 3%, but (F3, this fix round) that 9x9 grid itself disagreed
//  with a 15x15 re-run of the SAME quadrature by 2.06% (0.368865 vs
//  0.374733) -- real, deterministic discretization bias, not noise.  (L-P2,
//  this fix round: re-ran with a temporary env-gated sweep -- since removed
//  -- and saved the output verbatim to .../scratchpad/s2_convexity_sweep.txt;
//  N=9 avg is 0.368865, not the earlier draft's rounded "0.369", which is
//  why the 9x9-vs-15x15 disagreement above now reads 0.368865 vs 0.374733
//  instead of the previous "0.366997 vs 0.374733" -- 0.366997 does not
//  appear in the corroborating log at all and was a transcription error.)
//  A sweep at N=9,15,21,27,33,41 found the average stabilizing from N=21
//  onward (21: 0.379127, 27: 0.377271, 33: 0.378536, 41: 0.378159 -- all
//  mutually within ~0.5%, and every one of these six values, N=9 and
//  N=15 included, is reproduced bit-for-bit in s2_convexity_sweep.txt),
//  so the grid this file actually uses was raised to 21x21 and is
//  cross-checked against 33x33 inside `BuildConvexityScene`
//  itself (asserted <1% agreement; observed 0.156%).  Against the new
//  21x21 control, PT's layer-1 convexity ratio is now ~0.0001-0.001 (see
//  OBSERVED POST-FIX NUMBERS above) -- indistinguishable from the other
//  five scenes' <0.05% noise floor.  3% comfortably covers that noise
//  floor with wide margin, while remaining a fraction of every observed
//  BDPT/VCM bias (55-118 points).  Layer 2's 5% band is the design doc's
//  own number, at the coarser (160x120, 16 spp) showcase resolution.
//
//  LAYER 2 HARNESS FIXES (S2 follow-up).  The original S2 author's
//  `ResizeFilmChunk`/`SwapRasterizer` were raw-text regex/brace-counting
//  over the WHOLE scene file.  Two defects surfaced when this session
//  instrumented the captured image's actual dimensions (a temporary
//  `capE->width`x`capE->height` print, since removed in favour of the
//  permanent hard `Check` now in `RunLayer2Showcase`):
//    - shelf_bunny.RISEscene rendered at its NATIVE 800x600, never
//      resized.  Root cause: its own "DEVIATIONS FROM THE SPEC" header
//      comment spells the real film chunk's values out in prose --
//      `` `film { width 800 height...` `` split across two `#` lines --
//      and a bare `film\s*{` regex matches THAT decoy (it appears earlier
//      in the file) before ever reaching the real chunk.  The decoy's own
//      "600 }" sits behind a `#` on the next line, so only `width` inside
//      the comment got mangled; `height` in the comment and BOTH params on
//      the real chunk were untouched.
//    - tidal_stones.RISEscene was SUSPECTED of the same failure (no
//      "bound to canonical FrameStore 160x120" log line in the original
//      pre-fix run), but the width/height diagnostic proved this was a
//      FALSE ALARM: tidal_stones has only one `film` occurrence in the
//      whole file and resized correctly to 160x120 every time; the
//      missing log line is unrelated to resize correctness (its own
//      `file_rasterizeroutput` chunk, still present untouched alongside
//      the swapped-in rasterizer, apparently doesn't hit the exact
//      `FileRasterizerOutput::OnRasterizerFrameStoreChanged` code path
//      that prints that line for this scene -- cosmetic, not a
//      correctness gap, and not investigated further since the hard
//      dimension Check is what actually matters).
//  FIX: `ResizeFilmChunkCst` and `SwapRasterizerCst` now edit the parsed
//  Cst::Document STRUCTURALLY -- `FindChunkByRole` enumerates real Chunk
//  nodes only (comment trivia is never a Chunk node, so the decoy above
//  cannot be matched), `ParamValueAsParsed`/`DocSetOrAddParamValue` read
//  and write width/height, and the rasterizer swap uses `DocReplaceItem`
//  at the chunk's own resolved index instead of a keyword-then-brace-count
//  text search.  Every Layer-2 render now asserts (hard `Check`, not a
//  printed note) that the captured image is EXACTLY the requested
//  `targetW`x`targetH` -- this cannot silently regress again.  A second
//  fix verifies the SWAP itself: `CaptureStdoutDuring` redirects
//  `std::cout` around each `Rasterize()` call (GlobalLog's StreamPrinter
//  writes through `std::cout`, Log.cpp:179) and `CheckIntegratorMarker`
//  greps the captured log for an integrator-specific, unconditional
//  marker -- `"BDPT Progressive::"` for BDPT, `"VCMRasterizerBase::
//  PreRenderSetup::"` for VCM (that function is called at a fixed pipeline
//  hook on every VCM render and prints on every branch), and for PT the
//  ABSENCE of both (PT's own "Progressive::" line is shared with VCM's
//  progressive pass, so it isn't distinctive on its own) -- so a swap that
//  silently fell back to a different integrator would fail loudly via the
//  log once the render actually ran, instead of producing an
//  unremarkable-looking mean.  (F1, this fix round) `CheckIntegratorMarker`
//  alone only fires AFTER a render completes and only distinguishes
//  "BDPT/VCM specifically ran" from "neither did" -- it does not prove the
//  DOCUMENT about to be rendered carried the intended keyword before that
//  render started.  `VerifySwappedRasterizerChunk` closes that gap: it
//  resolves the swapped document's own `*_rasterizer` chunk by role and
//  hard-`Check`s that its keyword (`Chunk::role`) equals the intended
//  integrator's (`pathtracing_pel_rasterizer` / `bdpt_pel_rasterizer` /
//  `vcm_pel_rasterizer`) for EVERY integrator, including PT, plus a
//  read-back that the chunk still carries `oidn_denoise FALSE` and
//  `pixel_filter box`.  Between the two checks, a swap that silently fell
//  back to a different integrator's chunk now fails loudly at the
//  document level (before any render runs) as well as the log level
//  (after one does).
//
//  LAYER 2 FINDING (SECOND fix round, worktree signals-bidir,
//  kLayer2Samples=32, kLayer2MaskedSubRenders=6, two independent default
//  runs -- full logs .../scratchpad/s2_fixround2_run1.txt and run2.txt;
//  supersedes the first fix round's numbers below, which predate both
//  K-P1's sub-render averaging and K-P2b's literal-ratio `agree` fix).  At
//  160x120, all FOUR showcases' WHOLE-IMAGE `mean(PT,E)` vs `mean(PT,B)`
//  still differ by well under the 5% sensitivity band across both runs
//  (plank 0.23%-0.29%, tidal_stones 0.027%-0.049%, shelf_bunny
//  1.838%-1.842%, pavilion_colonnade 0.851%-0.858%) -- each signal-driven
//  region (a nail's contact seam, a buried stone's waterline, dust under a
//  bunny's foot, a column's crevice wear) is too small a fraction of the
//  WHOLE-IMAGE mean to move it past noise at this resolution -- so the
//  WHOLE-IMAGE ratio-of-ratios is dropped on all four, as designed.  The
//  MASKED form DOES witness the gap: mask coverage ranged 2.146%-2.203%
//  (plank) to 12.740%-13.057% (tidal) of pixels across both runs (bunny
//  6.828%-6.911%, pavilion 6.745%-6.849%), and on every (showcase,
//  integrator) pair NOT caught by the blow-up gate the K=6-sub-render-
//  averaged masked ratio-of-ratios PASSED comfortably inside the 20%
//  band.  Observed across both runs: plank BDPT R_E/R_B-1 (K=6 average)
//  in {-0.0793, -0.0472}, plank VCM in {-0.0688, -0.0430}, bunny BDPT in
//  {+0.0089, +0.0109}, pavilion BDPT in {-0.0132, -0.0188} -- every one of
//  the eight rows now sits well inside the 20% band with 2.5x-22x
//  headroom (see "BAND DERIVATION -- MASKED LAYER" below for the fuller
//  n=6-run plank derivation that motivated K=6).  BLOW-UP SKIPS (task
//  item 5; the `agree` figures below are the K-P2b LITERAL
//  |ratioE/ratioB - 1|, not the pre-K-P2b |meanE/meanB - 1| -- both
//  formulas classify the same four rows as `BlowupClass::Blowup`, since
//  every showcase here is whole-image insensitive, but the literal
//  numbers differ slightly from the first fix round's citation below):
//  tidal BDPT (mean(BDPT)/mean(PT,E) ~1099.5-1099.8x, mean(BDPT)/mean(PT,B)
//  ~1112.9-1113.0x, E/B agree within 1.18%-1.20%), tidal VCM
//  (~3466.0-3466.8x / ~3466.9-3467.0x, agree within 0.0046%-0.026%), bunny
//  VCM (~4449.9-4450.0x / ~4445.1-4445.3x, agree within 0.105%-0.109%),
//  pavilion VCM (~749.5-749.8x / ~746.0-746.3x, agree within 0.465%-0.474%)
//  -- all four comfortably inside the 10% E/B-agreement band
//  (`kLayer2BlowupAgreeBand`), so all four classify as `BlowupClass::Blowup`
//  (the known, pre-existing, non-signal case) rather than
//  `BlowupClass::Asymmetric`; neither run flagged any row as a possible
//  signal-attributable regression instead.  These figures match the
//  supervisor's independently-reproduced CLI figures (tidal BDPT
//  157-338x, tidal VCM 433-1656x, bunny VCM ~4558x across 160x120/
//  400x300/800x600) and VCM's known auto-radius instability at low spp
//  (flagged in the pre-fix run for whoever next touches VCM's auto-radius
//  pre-pass; still open).  `g_blowupSkipCount` == 4 on both runs.
//
//  FIRST fix round's numbers (kLayer2MaskedSubRenders=1 -- i.e. what the
//  file measured before K-P1 -- and the pre-K-P2b `agree` formula), kept
//  for the historical record: full logs .../scratchpad/s2_fixround_run1.txt
//  and run2.txt.  Mask coverage ranged 2.12%-2.20% (plank) to 13.1%-13.2%
//  (tidal) of pixels (bunny 6.60%-6.72%, pavilion 6.69%-6.82%); observed
//  single-sub-render masked ratios: plank BDPT R_E/R_B-1 in {-0.0385,
//  -0.1062}, plank VCM in {-0.0521, -0.0582}, bunny BDPT in {+0.0248,
//  +0.0263}, pavilion BDPT in {+0.0074, +0.0102} -- every one of the eight
//  rows inside the 20% band, though plank BDPT's -0.1062 left under 2x
//  headroom, which is exactly the flake risk K-P1 closes.  Blow-up
//  agreement (pre-K-P2b |meanE/meanB-1| formula): tidal BDPT ~1.23%, tidal
//  VCM ~0.05%, bunny VCM ~1.74%, pavilion VCM ~0.39%.
//
//  BAND DERIVATION -- MASKED LAYER.  The masked ratio-of-ratios averages
//  over a MUCH smaller pixel set (roughly 400-2600 of 19200 pixels
//  depending on showcase) than the whole-image form, so it carries more
//  Monte Carlo noise per sample.
//
//  (K-P1, this fix round) The single-sub-render metric (kLayer2MaskedSub-
//  Renders=1, i.e. what the file measured before this round) was flake
//  risk, not just noisy: across 9 independent runs at the shipped
//  (scene, 32 spp) settings -- 5 from .../scratchpad/s2_plank_n5_run1.txt
//  through run5.txt, 2 from s2_fixround_run1.txt/run2.txt, 2 from
//  reviewer2_run1.txt/run2.txt -- plank's masked BDPT ratio was
//  {+0.0012, -0.0171, -0.0346, -0.1282, -0.0911, -0.0385, -0.1062,
//  -0.0470, -0.0369}: mean -0.0554, sample sd 0.0432, worst -0.1282 --
//  only 1.66sigma from the 20% band edge at the worst observation, and
//  the "band >= 4sigma from the mean" check FAILS (0.145 available vs
//  0.173 needed) as does "band >= 2x worst" (0.20 available vs 0.256
//  needed).  VCM's own 9-run stats (mean -0.0590, sd 0.0096, worst
//  -0.0741) already clear both bars comfortably -- this was a BDPT-only
//  problem, though the fix below is applied to both "for symmetry" per
//  the finding that reopened this.
//
//  Two mitigations were tried, empirically, in this order:
//    1. Raise kLayer2Samples for BDPT/VCM only (96 spp, 3x).  n=6 plank
//       runs gave BDPT mean -0.0799, sd 0.0549, worst -0.1327 -- WORSE
//       on every statistic than the 32-spp n=9 baseline above, matching
//       this file's own earlier note (previous fix round) that 64 spp
//       "did not tighten the ratio spread" for the same masked estimator.
//       A single render's spp does not behave like naive 1/sqrt(N) noise
//       reduction for this metric -- REJECTED.
//    2. Average the FINAL masked ratio over `kLayer2MaskedSubRenders`
//       INDEPENDENT full renders (fresh Job/Rasterizer per sub-render, so
//       each gets its own explicit `srand( g_seedBase + g_renderIndex++ )`
//       call immediately before its `Rasterize()` -- see
//       RenderShowcaseVariant/RunLayer2Showcase's masked-ratio loop, and
//       the seeding comment at g_seedBase's declaration for why this is
//       NOT "wall-clock seeded": independence across sub-renders comes
//       from each one's distinct explicit seed, not the clock).
//       n=6 plank runs at kLayer2MaskedSubRenders=4 gave BDPT mean
//       -0.0623, sd 0.0332, worst -0.0957 -- both bars now pass, but only
//       just (margin 1.04x on the 4sigma bar).  Raising to 6 sub-renders
//       (n=6 plank runs, full logs .../scratchpad/s2_plank_bdpt_n1.txt
//       through n6.txt) gave BDPT mean -0.0550, sd 0.0185, worst -0.0700
//       (margin 1.96x on the 4sigma bar, 1.43x on the 2x-worst bar) and
//       VCM mean -0.0486, sd 0.0066, worst -0.0605 (margin 5.72x / 1.65x)
//       -- CHOSEN.  kLayer2MaskedSubRenders=6 adds ~35s to the default
//       whole-suite runtime (~35s at K=1 measured earlier this round ->
//       ~70s at K=6, both measured on this machine) -- comfortably under
//       the ~2 minute budget.
//
//  Mask coverage is unaffected by K (it is built once from the single PT
//  render, independent of BDPT/VCM sub-render count): 2.089%-2.276% across
//  the historical n=5 run, printed to 3 decimal places so a reader sees
//  the ~2x headroom over the 1% floor (`kLayer2MaskMinCoverage`) directly
//  instead of it rounding away at "2%".  The 20% band (kLayer2MaskedBand)
//  is kept as-is -- widening it was never necessary once the ESTIMATOR
//  (K=6 sub-render average) was fixed rather than the band; every K=6
//  observation above (BDPT worst -0.0700, -0.1600 as a single-sub-render
//  outlier that the K=6 average absorbs -- see run 1 of
//  s2_plank_bdpt_n1.txt) sits comfortably inside it.  This band is
//  intentionally looser than Layer 1's 3% -- it is a production-scene
//  diagnostic, not the sharp gate; Layer 1 is the file's primary
//  red/green witness (per its own header framing above), and the masked
//  Layer-2 numbers are corroborating evidence on real showcase content.
//
//  ADAPTIVE K, AND THE COMMON-MODE PT DENOMINATOR (debt 28 follow-up,
//  2026-09-11).  Everything above this paragraph derived K against the
//  PLANK showcase.  Debt 28's aperture fix brought tidal's whole-image
//  BDPT/PT inside the blow-up gate (it had been 338x), so tidal's masked
//  BDPT row began running for the first time -- and failed about four
//  runs in six at K=12.  Two changes, in this order:
//
//    1. The stopping rule is now a PRECISION target, not a count.  The
//       loop draws at least kLayer2MaskedSubRenders sub-renders, then
//       keeps going while the standard error of the mean ratio exceeds
//       kLayer2MaskedSEFraction of the band (0.25 * 0.20 = 0.05), to a
//       hard cap of kLayer2MaskedSubRendersCap = 48.  A row that hits
//       the cap with SE still above target is reported as INSUFFICIENT
//       PRECISION and counted in `g_maskedPrecisionSkipCount` -- NOT a
//       pass and NOT a failure, the same honesty the blow-up gate
//       practises.  Fixed K was the wrong shape of knob for an
//       estimator whose per-showcase variance differs this much: tidal's
//       masked pixels are ~30x darker than its frame average (masked PT
//       mean 0.0017 against a whole-image 0.056, over 13.5% of pixels).
//
//    2. Each sub-render now draws its OWN PT denominator pair.  The
//       estimator is (maskedE/maskedB) * (maskedPT_B/maskedPT_E), so the
//       PT pair is a MULTIPLICATIVE factor common to every sub-render:
//       re-rendering only BDPT/VCM averaged away the integrator-side
//       noise and left the PT-side noise entirely intact.  That is
//       measurable -- tidal's within-run SE at K=12 was 0.017 against a
//       run-to-run sd of ~0.06 over the six runs tabulated in
//       docs/RENDERING_INTEGRATORS.md, 3.5x larger -- so an SE built on
//       BDPT draws alone is not the SE of the asserted quantity, and a
//       stopping rule using it stops early and confidently.  The PT
//       draws come from a per-showcase pool shared by the BDPT and VCM
//       loops (cost: max(K) PT pairs per showcase, not their sum).  The
//       MASK is deliberately NOT redrawn -- it is a selector, and
//       redefining which pixels are measured per sub-render would
//       average over a different quantity each time.
//
//  What that bought, measured over two full runs at the default seed
//  base -- run 1 with the shared PT denominator (one draw, reused by
//  every sub-render), run 2 with the fix (an independent PT pair per
//  sub-render).  At THIS seed base every row's single realization moved
//  TOWARD zero:
//
//    row              shared-PT (run 1)   independent-PT (run 2)
//    plank BDPT         -0.0737             +0.0001
//    plank VCM          -0.0515             +0.0082
//    bunny BDPT         +0.0042             +0.0032
//    bunny VCM          +0.0063             +0.0031
//    pavilion BDPT      -0.0391             -0.0113
//    pavilion VCM       -0.0361             -0.0086
//    tidal BDPT         -0.2432             -0.2443
//
//  One draw per row at one seed base is a REALIZATION, not a measured
//  bias -- calling this "a systematic negative bias from every row"
//  would overclaim what two single-sample points can show.  What IS a
//  measured quantity is the within-run SE the shared-PT estimator
//  carries (0.017 on tidal, see below) against the run-to-run spread it
//  produces (~0.06) -- that comparison is what justifies drawing an
//  independent PT pair per sub-render, independent of which direction
//  any one row's point estimate happened to move.
//
//  K stayed at the 12 minimum on every row (SE 0.0006-0.0179, all under
//  the 0.05 target).  Cost: a fresh PT pair per sub-render multiplies
//  PT renders by K instead of sharing one draw across all K, which on
//  this machine is +36s (2:11 with the adaptive rule alone -> 2:47 with
//  independent PT denominators too) for the whole suite.
//
//  AND THE TIDAL ROW IS NOT A FLAKE, but its earlier framing overclaimed
//  the statistic.  At -0.2443 with SE 0.0179 (and -0.2432 / 0.0173 on
//  the previous run), the earlier draft of this comment said that sits
//  "13.6 standard errors outside the 0.20 band" -- that number is
//  |mean| / SE (i.e. standard errors from ZERO), not standard errors
//  from the band edge.  The correct distance from the band is
//  (0.2443 - 0.20) / 0.0179 ~= 2.5 SE (B2 P1-1, debt 28 review round 2)
//  -- a real miss, but a much smaller one than "13.6" suggested.  The
//  argument for "not a flake" was never that one number anyway: it is
//  CROSS-RUN reproducibility.  A second reviewer (B2) saw the row read
//  -0.252 through -0.273 over five independent fresh runs -- NONE of
//  them passing -- which is the actual evidence, not any single run's
//  SE.  (The point estimates above, and elsewhere in this file, read
//  like exact repeatable numbers; they are not bit-reproducible run to
//  run -- see the pre-existing worker-side `rand()` race this file's
//  own `g_seedBase` comment documents -- so treat any single decimal
//  value here as one draw from that spread, not a constant.)  The
//  six-run spread that made an EARLIER version of this estimator look
//  like a flake was the common-mode PT denominator; with that removed
//  the estimate tightened, and what it settled on is that tidal's
//  masked BDPT-vs-PT transport really does differ by a large,
//  reproducible amount -- not that the measurement itself is unreliable.
//
//  WHAT CHANGED THIS ROUND (supervisor ruling, debt 28 round 2): the
//  PT-referenced ratio-of-ratios assumes PT's own bias is
//  material-independent between the E and B variants at the masked
//  pixels.  On tidal it is not -- the masked set is exactly the
//  caustic-lit, near-black region reached by a delta light refracted
//  through the scene's dielectric water, a transport class PT's NEE
//  cannot sample at all, so PT's E-vs-B response there is
//  direct-light-only while BDPT's and VCM's are the full response.
//  Measured: PT vs BDPT's own neutral-variant (B) masked mean disagree
//  by ~2.8-3.0x, and PT vs VCM's by ~57.5-58x (VCM's merges recover far
//  more of that transport than BDPT's connections do) -- both comfortably
//  past the reference-completeness gate below, so PT is excluded as a
//  reference for tidal's masked row entirely, on BOTH integrators.  The
//  PT-independent BDPT<->VCM masked invariant is asserted instead (see
//  RunLayer2Showcase's "CROSS-INTEGRATOR MASKED INVARIANT" block) --
//  and it is ITSELF right at the edge of the same 20% band: -0.1996
//  (SE 0.0199) on one run, -0.2354 (SE 0.0129) on another, i.e. BDPT and
//  VCM disagree with each other by an amount that straddles the band
//  rather than sitting cleanly inside or outside it.  Per the ruling,
//  that is reported honestly rather than papered over: the band was NOT
//  loosened, and a run that fails the cross-check is a real
//  signals-vs-integrator finding, not a bug in the test.  Root-causing
//  the underlying BDPT-vs-VCM gap on tidal's masked pixels (and the
//  wildly different VCM-vs-BDPT reference-incompleteness magnitudes,
//  57x vs 2.8x) is open work, tracked as its own item -- debt 30 -- in
//  docs/RENDERING_INTEGRATORS.md (round 3 below; debt 27 keeps only the
//  PT-strategy-gap data).
//
//  WHAT CHANGED THIS ROUND (supervisor ruling, debt 28 round 3): the
//  PT-independent BDPT<->VCM cross-check above is only meaningful if
//  BDPT and VCM actually agree with each other on the NEUTRAL (B)
//  variant's masked pixels -- if they don't, the cross-check is asking
//  two witnesses who disagree by 20x to referee a 20% question, which
//  is not a referee at all.  Two full runs at the previous round's
//  fixed reference-completeness gate confirmed exactly that: tidal's
//  neutral-variant masked ratios are BDPT/PT ~2.8-3.0x and VCM/PT
//  ~57.5-58x, i.e. BDPT and VCM disagree with EACH OTHER by ~20x on
//  the very pixels the cross-check is about to compare them on -- and
//  the cross-check itself came back a coin flip, -0.1996 (SE 0.0199,
//  pass) on one run and -0.2354 (SE 0.0129, fail) on another.  A test
//  that is red one run in two because two non-agreeing integrators are
//  being asked to referee each other is not testing signals; it is
//  reporting an open integrator disagreement as if it were a coverage
//  gap.  The fix makes the reference-completeness rule SYMMETRIC:
//  before asserting the BDPT<->VCM masked invariant, the test now
//  computes `| mean_mask(BDPT,B) / mean_mask(VCM,B) - 1 |` (the same
//  cheap sub-render #1 the PT-referenced gate already uses -- no extra
//  render) and, if it exceeds `kReferenceIncompleteThreshold` (0.5, the
//  same coarse gate reused rather than a second magic number), prints a
//  labelled `NO COMPLETE REFERENCE ON MASK` line naming all three
//  ratios (BDPT/VCM neutral-variant, PT/BDPT, PT/VCM), skips the
//  assertion entirely, and counts it in a NEW summary counter,
//  `g_noCompleteReferenceSkipCount` -- distinct from both the blow-up
//  skip and the PT-reference-incomplete skip, because this is a THIRD
//  kind of "no usable referee exists for this row," not either of the
//  other two.  When BDPT and VCM DO agree within 50% on the neutral
//  variant, the cross-check runs and is asserted exactly as before --
//  this round changes nothing about that path.  On tidal, applying the
//  gate: BDPT/VCM neutral-variant masked ratio is ~0.05 (1/20 -- BDPT's
//  masked B mean is ~20x VCM's), which fails the 50% agreement bar, so
//  the cross-check is skipped and reported rather than asserted and
//  flaked on.  This is NOT a workaround to make the suite green: BDPT
//  and VCM disagreeing by ~20x on tidal's caustic-lit pixels is a real,
//  open integrator disagreement, now recorded as its own item -- debt
//  30 in docs/RENDERING_INTEGRATORS.md -- rather than being laundered
//  through a cross-check that could never have meant anything on this
//  showcase to begin with.
//
//  KNOBS.  `SIGNAL_CONSISTENCY_FILTER` (env, substring match against `unit`,
//  `showcase`, and the four showcase names `plank`, `tidal`, `bunny`,
//  `pavilion`) restricts which layer/scene runs, like FabricRenderTest's
//  own filter.  Unset (the default, and the CI invocation) runs BOTH
//  layers, all six unit scenes and all four showcases.  `argv[1]`, if
//  given, is an alternate seed base (default 1000; see "A THIRD fix
//  round" above and g_seedBase's own declaration) -- pass a different one
//  for a genuinely independent sample, the same convention
//  tests/FabricRenderTest.cpp and tests/PrimitiveSelfHitTest.cpp use.
//  Default runtime on this machine: 2 minutes 47 seconds wall (measured,
//  2026-09-11, adaptive K with independent PT denominators; the
//  fixed-K=12 shared-PT predecessor was 2:11 and the K=6 era ~70s).
//  Layer 1 is ~20s of that.  K settles at its 12 minimum on every row at
//  the default seed base, so the adaptive rule costs nothing there and
//  buys precision only where a showcase needs it, to the 48 cap.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>
#include <regex>
#include <filesystem>
#include <functional>
#ifdef _WIN32
	#include <process.h>		// _getpid()
	#define getpid _getpid
#else
	#include <unistd.h>			// getpid()
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Geometry/SDFGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/ExpressionEval.h"
#include "../src/Library/Painters/ExpressionPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;
namespace fs = std::filesystem;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

//! (round-3 fix) SEEDING.  This binary does not go through
//! src/RISE/commandconsole.cpp's `main()` -- the only place in the tree
//! that calls `srand( GetMilliseconds() )` -- so libc's `rand()` starts
//! from its default state here.  What actually varies run to run is that
//! every render worker constructs `RandomNumberGenerator random;`
//! (default argument `seed = rand()`, src/Library/Utilities/
//! RandomNumbers.h ~:32) concurrently from `ThreadPool::ParallelFor`
//! (src/Library/Rendering/RasterizeDispatchers.h ~:147, ~:201) -- i.e.
//! libc `rand()`'s hidden global state is read and mutated from several
//! threads with NO synchronisation.  That is an unsynchronized DATA RACE,
//! not a "wall-clock seeded" policy (this file's own comments used to
//! claim the latter at three sites -- all three were wrong and are fixed
//! at their sites).  tests/FabricRenderTest.cpp and
//! tests/PrimitiveSelfHitTest.cpp hit the exact same fact and adopt the
//! same fix: an OPTIONAL SEED BASE (argv[1], default `kDefaultSeedBase`)
//! with `std::srand( g_seedBase + g_renderIndex++ )` called immediately
//! before every render this file issues (Layer 1's `RenderSceneText`,
//! Layer 2's `RenderShowcaseVariant` -- once per Job::Rasterize() call,
//! so variant E and variant B each get their own seed).  Two
//! consequences: the default invocation is REPRODUCIBLE in intent (same
//! renders start from the same `rand()` state every time), and
//! `./SignalIntegratorConsistencyTest 2000`, `3000`, ... are independent
//! BY CONSTRUCTION -- different, documented, far-apart seed bases --
//! rather than by hoping the worker-side race decorrelates them.  NOT
//! claimed: bit-exact reproducibility of a single run -- the worker-side
//! race still perturbs which worker draws which seed, so two runs at the
//! same base agree closely but not exactly; independence across
//! sub-renders comes from the distinct `srand` value each one starts
//! from, not from the clock.
static const unsigned int kDefaultSeedBase = 1000u;
static unsigned int g_seedBase = kDefaultSeedBase;
static unsigned int g_renderIndex = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//======================================================================
// Env filter (FabricRenderTest / PrimitiveSelfHitTest pattern)
//======================================================================

static std::string g_filter;

static bool FilterAllows( const char* keyword )
{
	if( g_filter.empty() ) return true;
	return g_filter.find( keyword ) != std::string::npos;
}

//! Layer 1 (the six constant-signal unit scenes) runs when the filter is
//! unset or names "unit".
static bool WantsLayer1()
{
	return g_filter.empty() || FilterAllows( "unit" );
}

//! One showcase runs when the filter is unset, names "showcase" (all
//! four), or names that specific showcase's own keyword.
static bool WantsShowcase( const char* keyword )
{
	return g_filter.empty() || FilterAllows( "showcase" ) || FilterAllows( keyword );
}

//////////////////////////////////////////////////////////////////////
// CapturingRasterizerOutput -- same pattern as BDPTStrategyBalanceTest /
// VCMStrategyBalanceTest / ShelfBunnyShowcaseTest: capture the final
// image into memory, no file I/O.
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

//////////////////////////////////////////////////////////////////////
// Image mean -- composited-over-black (base*alpha), the convention
// BDPTStrategyBalanceTest's ComputeStats uses (see
// docs/INTEGRATOR_BUGFIX_FINDINGS.md Bug 2: PT and BDPT disagree on the
// unpremultiplied-vs-premultiplied alpha convention at partial-coverage
// silhouette pixels; base*alpha is convention-independent).  None of this
// file's scenes have partial-coverage edges in their framed region (every
// station is a full-coverage interior hit), so this mostly just protects
// against a stray background pixel creeping into the frame.
//////////////////////////////////////////////////////////////////////
struct ImageStats
{
	double meanRGB[3];
	double mean;		// average of the three channels
	bool   valid;
};

static ImageStats ComputeStats( const CapturingRasterizerOutput& cap )
{
	ImageStats s{};
	if( cap.pixels.empty() ) return s;

	double sum[3] = { 0, 0, 0 };
	for( const RISEColor& c : cap.pixels ) {
		const double cov = c.a;
		sum[0] += c.base.r * cov;
		sum[1] += c.base.g * cov;
		sum[2] += c.base.b * cov;
	}
	for( int c = 0; c < 3; c++ ) s.meanRGB[c] = sum[c] / double(cap.pixels.size());
	s.mean = ( s.meanRGB[0] + s.meanRGB[1] + s.meanRGB[2] ) / 3.0;
	s.valid = true;
	return s;
}

//////////////////////////////////////////////////////////////////////
// Scene-text rendering: write to a unique temp file, load via the
// canonical LoadAsciiSceneViaCst path, capture in memory, rasterize.
//////////////////////////////////////////////////////////////////////
static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag, int counter )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/signal_consistency_%s_%d_%d.RISEscene",
		tag, static_cast<int>(::getpid()), counter );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

static ImageStats RenderSceneText( const std::string& sceneText, const char* tag )
{
	static int counter = 0;
	ImageStats result{};

	const std::string path = WriteSceneToTempFile( sceneText, tag, counter++ );
	if( path.empty() ) return result;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		std::remove( path.c_str() );
		return result;
	}

	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		safe_release( pJob );
		std::remove( path.c_str() );
		return result;
	}

	pJob->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
	GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "signal consistency capture output" );
	pJob->GetRasterizer()->AddRasterizerOutput( pCap );

	// See the seeding comment at g_seedBase's declaration: this binary is
	// not wall-clock seeded, so the starting `rand()` state for this
	// render's worker threads is set explicitly here rather than left to
	// the unsynchronized libc race.
	std::srand( g_seedBase + g_renderIndex++ );
	const bool bRendered = pJob->Rasterize();
	if( bRendered ) {
		result = ComputeStats( *pCap );
	}

	safe_release( pCap );
	safe_release( pJob );
	std::remove( path.c_str() );
	return result;
}

//////////////////////////////////////////////////////////////////////
// Rasterizer chunk builders.  Every one forces `oidn_denoise FALSE` +
// `pixel_filter box` + no adaptive sampling (adaptive_max_samples
// defaults to 0 = disabled, so simply never setting it), per
// docs/skills/bdpt-vcm-mis-balance.md Step 0's non-MIS-cause checklist.
//////////////////////////////////////////////////////////////////////
static std::string RasterizerChunkPT( unsigned int samples )
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "pathtracing_pel_rasterizer\n{\n\tsamples " << samples
	   << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_pt_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

static std::string RasterizerChunkBDPT( unsigned int samples )
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples " << samples
	   << "\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_bdpt_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

static std::string RasterizerChunkVCM( unsigned int samples )
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "vcm_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples " << samples
	   << "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_vcm_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

static std::string RasterizerChunkMLT()
{
	std::ostringstream ss;
	ss << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	ss << "mlt_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tbootstrap_samples 4096\n"
	      "\tchains 64\n\tmutations_per_pixel 16\n\tlarge_step_prob 0.3\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
	ss << "file_rasterizeroutput\n{\n\tpattern /tmp/signal_consistency_mlt_unused\n\ttype PNG\n\tbpp 8\n\tcolor_space sRGB\n}\n\n";
	return ss.str();
}

enum class Integrator { PT, BDPT, VCM, MLT };

static const char* IntegratorName( Integrator i )
{
	switch( i ) {
		case Integrator::PT:   return "PT";
		case Integrator::BDPT: return "BDPT";
		case Integrator::VCM:  return "VCM";
		case Integrator::MLT:  return "MLT";
	}
	return "?";
}

static std::string RasterizerChunk( Integrator i, unsigned int samples )
{
	switch( i ) {
		case Integrator::PT:   return RasterizerChunkPT( samples );
		case Integrator::BDPT: return RasterizerChunkBDPT( samples );
		case Integrator::VCM:  return RasterizerChunkVCM( samples );
		case Integrator::MLT:  return RasterizerChunkMLT();
	}
	return std::string();
}

//======================================================================
// LAYER 1 -- REFERENCE HARNESS: evaluate a signal expression directly
// against a hand-built SDFGeometry hit, matching the render scene's own
// `part` lines field-for-field.  Adapted from tests/SurfaceSignalsTest.cpp's
// EvalAtHit / BuildSdf* helpers.
//======================================================================

static RayIntersection MkRI( const Point3& origin, const Vector3& dir )
{
	return RayIntersection( Ray( origin, dir ), nullRasterizerState );
}

static bool HitObject( const Object* obj, RayIntersection& ri )
{
	obj->IntersectRay( ri, RISE_INFINITY, true, true, false );
	return ri.geometric.bHit;
}

static bool CompileWithContext( const std::string& body, ExpressionProgram& out )
{
	ExpressionProgram::Builder b;
	b.EnableContextVars( true );
	return b.Finalize( body, out );
}

//! (F5) The ONE sampling-detail value shared by every scene-text `part`
//! chunk this file authors AND every hand-built SDFGeometry in
//! EvalAtSdfHit below, so the two constructions can never silently drift
//! apart -- before this constant existed, EvalAtSdfHit relied on
//! SDFGeometry's own default argument (also 64) matching the scene text's
//! literal `sampling_detail 64` by coincidence; a future change to either
//! default would break that coincidence without anyone noticing.
static const int kSdfSamplingDetail = 64;

//! Evaluate `body` (an expression referencing occlusion/convexity/thickness)
//! at the first hit of (origin,dir) against an SDFGeometry built from
//! `parts`.  Returns false on miss or compile failure.
static bool EvalAtSdfHit(
	const std::vector<SDFGeometry::Part>& parts,
	const Point3& origin, const Vector3& dir,
	const std::string& body, Scalar& outValue )
{
	SDFGeometry* g = new SDFGeometry( parts, 512, Scalar( 1e-5 ), kSdfSamplingDetail );
	Object* obj = new Object( g );
	safe_release( g );
	obj->FinalizeTransformations();

	ExpressionProgram prog = ExpressionProgram::Invalid();
	bool ok = false;
	if( CompileWithContext( body, prog ) ) {
		RayIntersection ri = MkRI( origin, dir );
		if( HitObject( obj, ri ) ) {
			std::vector<ParamSpec> specs;
			ExpressionScalarPainter* painter = new ExpressionScalarPainter( prog, specs );
			outValue = painter->GetValuesAt( ri.geometric ).v[0];
			painter->release();
			ok = true;
		}
	}
	obj->release();
	return ok;
}

//======================================================================
// LAYER 1 -- the shared material/lighting recipe.  ONE material reads
// the signal-driven scalar `t` in BOTH its colour (expression_painter,
// a ramp from a dark/rough "unworn" look to a light/smooth "worn" look)
// AND its GGX roughness (scalar_painter on alphax/alphay) -- exactly the
// "forward-live, priced-neutral" pattern the design doc's section 1
// describes: one material, one vertex, sampled and priced through the
// SAME `t`.  `tBody` is substituted three ways per scene: the real
// signal call (EXPR), a literal matching the harness/closed-form value
// (CONTROL), and the signal's documented neutral (NEUTRAL, PT-only,
// for the sensitivity check).
//======================================================================

static std::string BuildSignalScene(
	Integrator integrator, unsigned int samples,
	const std::string& geometryCameraLight,
	const std::string& tBody )
{
	std::ostringstream ss;
	ss << "RISE ASCII SCENE 7\n";
	ss << RasterizerChunk( integrator, samples );

	ss << "uniformcolor_painter\n{\n\tname sig_f0\n\tcolor 0.04 0.04 0.04\n}\n\n";
	ss << "uniformcolor_painter\n{\n\tname sig_gray\n\tcolor 0.5 0.5 0.5\n}\n\n";
	ss << "lambertian_material\n{\n\tname sig_neutral_mat\n\treflectance sig_gray\n}\n\n";

	ss << "scalar_painter\n{\n\tname sig_rough\n\tdef t " << tBody << "\n\texpression t\n}\n\n";
	ss << "expression_painter\n{\n\tname sig_col\n\tdef t " << tBody
	   << "\n\texpr mix(vec3(0.2,0.2,0.2), vec3(0.8,0.8,0.8), t)\n}\n\n";
	ss << "ggx_material\n{\n\tname sig_mat\n\trd sig_col\n\trs sig_f0\n\talphax sig_rough\n\talphay sig_rough\n\tfresnel_mode schlick_f0\n}\n\n";

	ss << geometryCameraLight;
	return ss.str();
}

//! The six layer-1 unit-scene descriptors.
struct SignalUnitScene
{
	const char* name;
	std::string geometryCameraLight;
	std::string exprT;		// the live builtin call
	std::string controlT;	// literal matching the harness/closed-form value
	std::string neutralT;	// literal matching the documented neutral
	double controlValue;	// the numeric value behind controlT (for printing)
};

//======================================================================
// LAYER 1 -- scene geometry.  Film is 32x32 for all six (BDPTStrategy-
// BalanceTest's own precedent: sub-percent MC noise on a smooth scene's
// mean at moderate spp over ~1000 pixels).
//======================================================================

static const char* kFilm32 = "film\n{\n\twidth 32\n\theight 32\n}\n\n";

// --- (1) curv: one analytic sphere_geometry, radius-independent
// closed form curv = 2*sqrt(3) (SurfaceCurvatureTest section (h)), which
// SATURATES clamp(curv,0,1) to EXACTLY 1.0 on ANY sphere (curv = 3.4641
// > 1).  Neutral curv = 0 => clamp(0,0,1) = 0.0.  So control = "1.0" is
// not a measurement, it is exact arithmetic.
static SignalUnitScene BuildCurvScene()
{
	SignalUnitScene s;
	s.name = "curv";
	std::ostringstream g;
	g << "sphere_geometry\n{\n\tname curv_sphere\n\tradius 2.0\n}\n\n";
	g << "standard_object\n{\n\tname obj_curv\n\tgeometry curv_sphere\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 7\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 36.0\n}\n\n";
	g << "omni_light\n{\n\tname l_curv\n\tpower 8.0\n\tcolor 1 1 1\n\tposition 3 3 7\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();
	s.exprT = "clamp(curv,0,1)";
	s.controlT = "1.0";
	s.controlValue = 1.0;
	s.neutralT = "0.0";
	return s;
}

// --- (2) convexity: one SDF sphere (rho = 1.5), fraction r = 0.3.
//
// convexity(r)'s estimator is a 32-DIRECTION DISCRETE sample of the query
// ball, "spun about the normal per hit so the answer is an expectation
// rather than a multiple of 1/12" (the expression_painter builtin doc).
// That per-hit spin is measured (below) to depend on the hit's LOCATION
// on the sphere, not just its rotational-symmetry class: a single
// hand-picked station's ComputeConvexity answer is NOT representative of
// the image-wide mean the render actually integrates (measured spread
// ~9% between a single off-axis station and the true camera-weighted
// average -- see docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md S2's "BAND
// DERIVATION -- MEASURED SPREAD").  The reference used as CONTROL is
// therefore the AVERAGE of the SAME direct evaluation over a grid of
// rays matching the render camera's own pinhole projection (fov, aspect,
// image plane) -- i.e. a coarse quadrature of the exact quantity PT's
// Monte Carlo mean estimates, using the SAME production ComputeConvexity
// call, just off the Monte Carlo path.  This is still independent of the
// render (no pixel from the actual render feeds back into it) and still
// a strong oracle (direct evaluation, not a theoretical approximation).
//! (F3) An NxN pinhole quadrature of `exprBody` over normalized device
//! coordinates [-0.9,0.9]^2 (staying off the silhouette) against a
//! square-aspect pinhole camera with vertical fov `fovDeg` -- factored out
//! of BuildConvexityScene so the SAME quadrature can be run at two grid
//! sizes and cross-checked against each other for grid-size convergence
//! (a coarse quadrature converging to a strictly-better one is separate
//! evidence from PT's own Monte-Carlo run-to-run spread; both bound the
//! same "is the control number trustworthy?" question from different
//! sides).
static double RunConvexityQuadrature(
	const std::vector<SDFGeometry::Part>& parts, const Point3& camLoc, double fovDeg,
	const std::string& exprBody, int N, double* outMin, double* outMax, int* outCount )
{
	const double halfFov = fovDeg * 0.5 * (M_PI / 180.0);
	const double tanHalf = std::tan( halfFov );
	double sum = 0.0;
	int count = 0;
	double minV = 1e30, maxV = -1e30;
	for( int iy = 0; iy < N; ++iy ) {
		for( int ix = 0; ix < N; ++ix ) {
			const double ndcX = -0.9 + 1.8 * ( double(ix) / double(N-1) );
			const double ndcY = -0.9 + 1.8 * ( double(iy) / double(N-1) );
			Vector3 dir = Vector3Ops::Normalize( Vector3(
				ndcX * tanHalf, ndcY * tanHalf, -1.0 ) );
			Scalar v = 0;
			if( EvalAtSdfHit( parts, camLoc, dir, exprBody, v ) ) {
				sum += double(v);
				count++;
				minV = std::min( minV, double(v) );
				maxV = std::max( maxV, double(v) );
			}
		}
	}
	if( outMin ) *outMin = minV;
	if( outMax ) *outMax = maxV;
	if( outCount ) *outCount = count;
	return count > 0 ? sum / double(count) : 0.0;
}

static SignalUnitScene BuildConvexityScene( double* outHarnessValue )
{
	SignalUnitScene s;
	s.name = "convexity";
	const double rho = 1.5;
	const double rFrac = 0.3;
	const Point3 camLoc( 0, 0, 6 );
	const double fovDeg = 32.0;
	std::ostringstream g;
	g << "sdf_geometry\n{\n\tname convex_sphere\n\tpart sphere union 0  0 0 0  0 0 0  1 1 1  " << rho << " 0 0  0\n"
	  << "\tmaxsteps 512\n\tepsilon 0.00001\n\tsampling_detail " << kSdfSamplingDetail << "\n}\n\n";
	g << "standard_object\n{\n\tname obj_convex\n\tgeometry convex_sphere\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 6\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov " << fovDeg << "\n}\n\n";
	g << "omni_light\n{\n\tname l_convex\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 3 3 6\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();

	std::ostringstream tb;
	tb << "convexity(" << rFrac << ")";
	s.exprT = tb.str();

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), Scalar(rho), 0, 0, 0 ) );

	// Pinhole quadrature: an NxN grid over normalized device coordinates
	// [-0.9,0.9]^2 (staying off the silhouette, where a grazing ray can
	// miss the sphere entirely), matching a square-aspect pinhole camera
	// with vertical fov = fovDeg.
	//
	// (F3) Grid size is 21x21, not the original 9x9: this quadrature's
	// per-hit result is DETERMINISTIC (a fixed-seed "spin", not wall-clock
	// noise -- reproduced bit-for-bit across repeat runs), so a 9x9-vs-15x15
	// cross-check measures true discretization bias, and 9x9 vs 15x15
	// disagreed by 2.06% (avg 0.368865 vs 0.374733) -- outside the 1% bound
	// this cross-check enforces.  (L-P2, this fix round) A sweep at
	// N=9,15,21,27,33,41 -- re-run with a temporary env-gated block and
	// saved verbatim to .../scratchpad/s2_convexity_sweep.txt, since this
	// file itself carries only the production N=21/N=33 pair -- found the
	// average stabilizing from N=21 onward (21: 0.379127, 27: 0.377271,
	// 33: 0.378536, 41: 0.378159 -- all mutually within ~0.5%), so the
	// primary grid was raised to 21x21 and is cross-checked against
	// 33x33 below.
	const int N = 21;
	double minV = 1e30, maxV = -1e30;
	int count = 0;
	const double avg = RunConvexityQuadrature( parts, camLoc, fovDeg, tb.str(), N, &minV, &maxV, &count );
	Check( count > ( N * N ) / 2, "(harness) convexity quadrature hits the sphere over most of the grid" );
	std::cout << "  (harness) convexity(" << rFrac << ") quadrature: N=" << count
	          << " avg=" << avg << " min=" << minV << " max=" << maxV
	          << " spread=" << ( maxV - minV ) << std::endl;

	// (F3) Grid-size cross-check: re-run the SAME quadrature at 33x33 and
	// assert the two grid sizes agree within 1% -- bounding the quadrature
	// bias itself (as distinct from PT's own Monte-Carlo run-to-run spread,
	// which BAND DERIVATION in the file header measures separately).  If
	// this ever fails again, the fix is to raise the grid this function
	// actually uses for `avg` further (currently 21x21) and re-state the
	// observed convergence in the file header -- not to loosen the 1% check.
	const int Nfine = 33;
	double minFine = 1e30, maxFine = -1e30;
	int countFine = 0;
	const double avgFine = RunConvexityQuadrature( parts, camLoc, fovDeg, tb.str(), Nfine, &minFine, &maxFine, &countFine );
	Check( countFine > ( Nfine * Nfine ) / 2, "(harness) convexity 33x33 cross-check quadrature hits the sphere over most of the grid" );
	const double gridConvergence = avg != 0.0 ? std::fabs( avg / avgFine - 1.0 ) : std::fabs( avgFine );
	std::cout << "  (harness) convexity(" << rFrac << ") grid-size cross-check: 21x21 avg=" << avg
	          << " vs 33x33 avg=" << avgFine << " |21x21/33x33-1|=" << gridConvergence << std::endl;
	Check( gridConvergence < 0.01,
		"(harness) convexity quadrature: 21x21 and 33x33 grids agree within 1% (quadrature-bias bound)" );

	*outHarnessValue = avg;
	std::ostringstream cv;
	cv.precision( 10 );
	cv << avg;
	s.controlT = cv.str();
	s.controlValue = avg;
	s.neutralT = "0.0";
	return s;
}

// --- (3) occlusion: a spherical pocket carved from a big SDF sphere by
// subtraction (tests/SurfaceSignalsTest.cpp section (k)'s "spherical
// pocket" construction, folded into ONE SDFGeometry's own multi-part
// field instead of a CSGObject of two -- SDFGeometry's own boolean
// subtract composes a single continuous signed field, so no CSG-level
// normal/field-sense re-pairing is needed for THIS geometry family).  The
// camera sits INSIDE the carved-out cavity (an established RISE pattern:
// vcm_sdf_luminaire_jellyfish's submerged camera), looking at the pocket
// floor -- the only way to frame a genuinely occluded station without an
// optical trick, since a solid opaque neighbour and "visible from
// outside" are mutually exclusive for a fully enclosed cavity.
static SignalUnitScene BuildOcclusionScene( double* outHarnessValue )
{
	SignalUnitScene s;
	s.name = "occlusion";
	const double bigR = 4.0;
	const double rho = 1.5;
	const double rFrac = 0.15;
	std::ostringstream g;
	g << "sdf_geometry\n{\n\tname pocket_sdf\n"
	  << "\tpart sphere union 0     0 0 0    0 0 0   1 1 1   " << bigR << " 0 0   0\n"
	  << "\tpart sphere subtract 0  0 0 2.0  0 0 0   1 1 1   " << rho << " 0 0   0\n"
	  << "\tmaxsteps 512\n\tepsilon 0.00001\n\tsampling_detail " << kSdfSamplingDetail << "\n}\n\n";
	g << "standard_object\n{\n\tname obj_pocket\n\tgeometry pocket_sdf\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 2.9\n\tlookat 0 0 0.5\n\tup 0 1 0\n\tfov 8.0\n}\n\n";
	g << "omni_light\n{\n\tname l_pocket\n\tpower 3.0\n\tcolor 1 1 1\n\tposition 0.3 0.3 2.7\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();

	std::ostringstream tb;
	tb << "clamp(1-occlusion(" << rFrac << "),0,1)";
	s.exprT = tb.str();

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), Scalar(bigR), 0, 0, 0 ) );
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimSphere, SDFGeometry::eOpSubtract, 0,
		Point3( 0, 0, 2.0 ), 0, 0, 0, Vector3( 1, 1, 1 ), Scalar(rho), 0, 0, 0 ) );
	Scalar measured = 0;
	Check( EvalAtSdfHit( parts, Point3( 0, 0, 2.9 ), Vector3( 0, 0, -1 ), tb.str(), measured ),
		"(harness) occlusion reference evaluates on the pocket floor" );

	*outHarnessValue = double( measured );
	std::ostringstream cv;
	cv.precision( 10 );
	cv << double( measured );
	s.controlT = cv.str();
	s.controlValue = double( measured );
	s.neutralT = "0.0";		// occlusion neutral 1 => clamp(1-1,0,1) = 0
	return s;
}

// --- (4) thickness: a wide, thin SDF box slab (tests/SurfaceSignalsTest
// .cpp's BuildSdfSlab pattern) viewed on its broad face, framed to a
// small central patch far from every edge -- the slab's own field is
// EXACTLY translation-invariant there (a flat box interior has no
// curvature), so the constant is exact within the estimator's own
// numerical precision, not merely "close to within 1%".
static SignalUnitScene BuildThicknessScene( double* outHarnessValue )
{
	SignalUnitScene s;
	s.name = "thickness";
	const double halfW = 0.15;	// slab half-thickness (full width 0.3)
	const double rFrac = 0.1;
	std::ostringstream g;
	g << "sdf_geometry\n{\n\tname slab_sdf\n\tpart box union 0  0 0 0  0 0 0  1 1 1  5.0 5.0 " << halfW << "  0\n"
	  << "\tmaxsteps 512\n\tepsilon 0.00001\n\tsampling_detail " << kSdfSamplingDetail << "\n}\n\n";
	g << "standard_object\n{\n\tname obj_slab\n\tgeometry slab_sdf\n\tmaterial sig_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 4.0\n}\n\n";
	g << "omni_light\n{\n\tname l_slab\n\tpower 6.0\n\tcolor 1 1 1\n\tposition 2 2 5\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();

	std::ostringstream tb;
	tb << "thickness(" << rFrac << ")";
	s.exprT = tb.str();

	std::vector<SDFGeometry::Part> parts;
	parts.push_back( SDFGeometry::MakePart(
		SDFGeometry::ePrimBox, SDFGeometry::eOpUnion, 0,
		Point3( 0, 0, 0 ), 0, 0, 0, Vector3( 1, 1, 1 ), 5.0, 5.0, Scalar(halfW), 0 ) );
	Scalar measured = 0;
	Check( EvalAtSdfHit( parts, Point3( 0, 0, 5.0 ), Vector3( 0, 0, -1 ), tb.str(), measured ),
		"(harness) thickness reference evaluates on the slab's broad face" );

	*outHarnessValue = double( measured );
	std::ostringstream cv;
	cv.precision( 10 );
	cv << double( measured );
	s.controlT = cv.str();
	s.controlValue = double( measured );
	s.neutralT = "1.0";		// thickness neutral is 1 (thick), not 0
	return s;
}

// --- (5) proximity: a receiver clipped-plane under a wide flat box, gap
// h = 0.5, query radius r = 1.0 => proximity = clamp(1-h/r,0,1) = 0.5
// EXACTLY (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md's closed form; box is
// one of the EXACT-family receivers/neighbours).  Camera sits IN the gap,
// between the two, looking straight down -- the box is directly behind
// the camera's view direction and never enters frame.
static SignalUnitScene BuildProximityScene()
{
	SignalUnitScene s;
	s.name = "proximity";
	std::ostringstream g;
	g << "clippedplane_geometry\n{\n\tname recv_plane\n\tpta -2 0 -2\n\tptb 2 0 -2\n\tptc 2 0 2\n\tptd -2 0 2\n}\n\n";
	g << "box_geometry\n{\n\tname proxbox_geom\n\twidth 6\n\theight 0.6\n\tdepth 6\n}\n\n";
	g << "standard_object\n{\n\tname obj_recv\n\tgeometry recv_plane\n\tmaterial sig_mat\n}\n\n";
	g << "standard_object\n{\n\tname obj_proxbox\n\tgeometry proxbox_geom\n\tmaterial sig_neutral_mat\n\tposition 0 0.8 0\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 0.2 0\n\tlookat 0 0 0\n\tup 0 0 1\n\tfov 20.0\n}\n\n";
	g << "omni_light\n{\n\tname l_prox\n\tpower 4.0\n\tcolor 1 1 1\n\tposition 1.0 0.3 0\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();
	s.exprT = "proximity(1.0)";
	s.controlT = "0.5";
	s.controlValue = 0.5;
	s.neutralT = "0.0";
	return s;
}

// --- (6) interior: a receiver clipped-plane embedded 5 world units deep
// inside a 10x10x10 box (half-extent 5 in every axis), query radius
// r = 0.5 => interior = clamp(depth/r,0,1) = clamp(5/0.5,0,1) = 1.0
// EXACTLY (saturates, so the exact depth doesn't even need to be 5 to
// 12 significant figures -- any depth >= r gives the same 1.0).  Camera
// sits INSIDE the box too (between the box's own far wall and the
// receiver, so the receiver -- much closer -- is hit first and the box's
// own surface is never reached; the same "camera and target share a
// solid's interior" pattern the occlusion scene uses, but here the box
// truly has open space inside it for ray-intersection purposes, since
// RISE's box_geometry is a boundary representation with no participating
// volume).
static SignalUnitScene BuildInteriorScene()
{
	SignalUnitScene s;
	s.name = "interior";
	std::ostringstream g;
	g << "clippedplane_geometry\n{\n\tname recv_plane2\n\tpta -2 0 -2\n\tptb 2 0 -2\n\tptc 2 0 2\n\tptd -2 0 2\n}\n\n";
	g << "box_geometry\n{\n\tname intbox_geom\n\twidth 10\n\theight 10\n\tdepth 10\n}\n\n";
	g << "standard_object\n{\n\tname obj_recv2\n\tgeometry recv_plane2\n\tmaterial sig_mat\n}\n\n";
	g << "standard_object\n{\n\tname obj_intbox\n\tgeometry intbox_geom\n\tmaterial sig_neutral_mat\n}\n\n";
	g << "pinhole_camera\n{\n\tlocation 0 2 0\n\tlookat 0 0 0\n\tup 0 0 1\n\tfov 20.0\n}\n\n";
	g << "omni_light\n{\n\tname l_int\n\tpower 4.0\n\tcolor 1 1 1\n\tposition 1.0 3 0\n}\n\n";
	g << kFilm32;
	s.geometryCameraLight = g.str();
	s.exprT = "interior(0.5)";
	s.controlT = "1.0";
	s.controlValue = 1.0;
	s.neutralT = "0.0";
	return s;
}

//======================================================================
// LAYER 1 driver
//======================================================================

static const unsigned int kLayer1Samples = 48;

struct Layer1Row
{
	std::string integrator;
	double meanExpr, meanControl, ratio;
	bool pass;
};

static std::vector<Layer1Row> g_layer1Rows;

static void RunLayer1Scene( const SignalUnitScene& scene, bool includeMlt )
{
	std::cout << "\n=== LAYER 1: " << scene.name << " (control = " << scene.controlT << ") ===" << std::endl;

	std::vector<Integrator> integrators = { Integrator::PT, Integrator::BDPT, Integrator::VCM };
	if( includeMlt ) integrators.push_back( Integrator::MLT );

	const double band = 0.03;			// 3% -- see header BAND DERIVATION
	const double sensitivityBand = 0.25;	// (G9) design doc section 6.1: must be OUTSIDE 25%, not 3% -- every observed margin (0.54-3.92) clears this comfortably

	double ptExprMean = 0.0, ptNeutralMean = 0.0;
	bool ptOk = false;

	for( Integrator integ : integrators ) {
		const unsigned int samples = ( integ == Integrator::MLT ) ? 0 : kLayer1Samples;
		const std::string exprScene    = BuildSignalScene( integ, samples, scene.geometryCameraLight, scene.exprT );
		const std::string controlScene = BuildSignalScene( integ, samples, scene.geometryCameraLight, scene.controlT );

		const ImageStats exprStats    = RenderSceneText( exprScene, ( scene.name + std::string("_expr") ).c_str() );
		const ImageStats controlStats = RenderSceneText( controlScene, ( scene.name + std::string("_ctrl") ).c_str() );

		const bool rendersOk = exprStats.valid && controlStats.valid;
		Check( rendersOk, std::string( scene.name ) + " " + IntegratorName(integ) + ": expr and control both render" );

		Layer1Row row;
		row.integrator = IntegratorName( integ );
		row.meanExpr = exprStats.mean;
		row.meanControl = controlStats.mean;
		row.ratio = rendersOk && controlStats.mean != 0.0 ? ( exprStats.mean / controlStats.mean - 1.0 ) : 1e9;
		row.pass = rendersOk && std::fabs( row.ratio ) < band;
		g_layer1Rows.push_back( row );

		std::cout << "  " << IntegratorName( integ )
		          << ": mean(expr)=" << exprStats.mean
		          << " mean(control)=" << controlStats.mean
		          << " ratio-1=" << row.ratio
		          << ( row.pass ? "  [pass]" : "  [FAIL]" ) << std::endl;

		Check( row.pass, std::string( scene.name ) + " " + IntegratorName(integ)
			+ ": | mean(expr)/mean(control) - 1 | < " + std::to_string(band) );

		if( integ == Integrator::PT ) {
			ptExprMean = exprStats.mean;
			ptOk = rendersOk;
		}
	}

	// Sensitivity: PT expr vs PT neutral-baked must be OUTSIDE the band.
	if( ptOk ) {
		const std::string neutralScene = BuildSignalScene( Integrator::PT, kLayer1Samples, scene.geometryCameraLight, scene.neutralT );
		const ImageStats neutralStats = RenderSceneText( neutralScene, ( scene.name + std::string("_neutral") ).c_str() );
		Check( neutralStats.valid, std::string( scene.name ) + " PT: neutral-baked render succeeds" );
		if( neutralStats.valid ) {
			ptNeutralMean = neutralStats.mean;
			const double sensRatio = ptNeutralMean != 0.0
				? ( ptExprMean / ptNeutralMean - 1.0 )
				: ( ptExprMean != 0.0 ? 1e9 : 0.0 );
			std::cout << "  SENSITIVITY: mean(PT,expr)=" << ptExprMean
			          << " mean(PT,neutral=" << scene.neutralT << ")=" << ptNeutralMean
			          << " ratio-1=" << sensRatio << std::endl;
			Check( std::fabs( sensRatio ) > sensitivityBand,
				std::string( scene.name ) + " PT: expr vs neutral-baked is OUTSIDE the band (real sensitivity)" );
		}
	}
}

//======================================================================
// LAYER 2 -- showcase ratio-of-ratios
//======================================================================

static fs::path FindRepoRoot()
{
	const char* candidates[] = { ".", "..", "../..", "../../.." };
	for( const char* c : candidates ) {
		const fs::path p( c );
		if( fs::exists( p / "scenes" / "FeatureBased" / "Textures" / "plank_closeup.RISEscene" ) ) {
			return p;
		}
	}
	return fs::path();
}

static std::string ReadFile( const fs::path& p )
{
	std::ifstream in( p );
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

//! Replace every whole-word call `fnName(...)` (balanced parens, so a
//! nested call inside the argument list is handled correctly) with
//! `constant`.  Whole-word: the character before `fnName` (if any) must
//! not be alnum/`_`, so this never fires inside a longer identifier.
static std::string ReplaceCallsWithConstant( const std::string& text, const std::string& fnName, const std::string& constant )
{
	std::string out;
	out.reserve( text.size() );
	std::size_t i = 0;
	while( i < text.size() ) {
		std::size_t pos = text.find( fnName + "(", i );
		if( pos == std::string::npos ) { out += text.substr( i ); break; }
		const bool boundaryOk = ( pos == 0 ) ||
			!( std::isalnum( static_cast<unsigned char>(text[pos-1]) ) || text[pos-1] == '_' );
		if( !boundaryOk ) {
			out += text.substr( i, pos - i + fnName.size() );
			i = pos + fnName.size();
			continue;
		}
		out += text.substr( i, pos - i );
		std::size_t parenStart = pos + fnName.size();
		int depth = 0;
		std::size_t j = parenStart;
		for( ; j < text.size(); ++j ) {
			if( text[j] == '(' ) depth++;
			else if( text[j] == ')' ) { depth--; if( depth == 0 ) { j++; break; } }
		}
		out += constant;
		i = j;
	}
	return out;
}

static std::string ReplaceWholeWord( const std::string& text, const std::string& word, const std::string& constant )
{
	std::regex re( "\\b" + word + "\\b" );
	return std::regex_replace( text, re, constant );
}

//! Build the neutral (variant B) text: every geometry signal call
//! rewritten to its documented neutral, per design doc section 6.2.
//! Order matters only for curv/curvR (curvR first is harmless either way
//! since \b already excludes it from the `curv` pattern, but doing it
//! first keeps the intent obvious to a reader).
static std::string BuildNeutralVariant( const std::string& original )
{
	std::string t = original;
	t = ReplaceCallsWithConstant( t, "occlusion", "1" );
	t = ReplaceCallsWithConstant( t, "thickness", "1" );
	t = ReplaceCallsWithConstant( t, "convexity", "0" );
	t = ReplaceCallsWithConstant( t, "proximity", "0" );
	t = ReplaceCallsWithConstant( t, "interior", "0" );
	t = ReplaceWholeWord( t, "curvR", "0" );
	t = ReplaceWholeWord( t, "curv", "0" );
	return t;
}

//! Parse `chunkText` (one or more top-level items, same idiom as a
//! hand-authored chunk string) and return its FIRST Chunk-kind item --
//! tests/ShelfBunnyShowcaseTest.cpp's `FirstChunkItem` helper, generalised
//! to take text instead of a pre-parsed Document, so callers can build a
//! brand-new chunk from a string and splice it into an existing Document
//! via DocReplaceItem/DocInsertItem.
static Cst::NodeRef FirstChunkItemFromText( const std::string& chunkText )
{
	Cst::Document d = Cst::ParseToCst( chunkText );
	const int n = Cst::DocItemCount( d );
	for( int i = 0; i < n; ++i ) {
		const Cst::NodeRef it = Cst::DocResolveNodeId( d, Cst::DocNodeIdAt( d, i ) );
		if( it && it->kind == Cst::NodeKind::Chunk ) return it;
	}
	return Cst::NodeRef();
}

//! Find the top-level chunk whose role EQUALS `roleExact` (when non-empty)
//! or ENDS WITH `roleSuffix` (when non-empty) -- the unnamed-chunk-by-KIND
//! lookup tests/ShelfBunnyShowcaseTest.cpp's BuildProbeDocument step 4 uses
//! ("Rasterizer chunk is UNNAMED -- find it by role").  This is a
//! STRUCTURAL lookup over real Chunk nodes, not a text search, which
//! matters here: shelf_bunny.RISEscene's own header prose (its DEVIATIONS
//! FROM THE SPEC section) spells `film { width 800 height 600 }` verbatim
//! inside a `#` comment, documenting the real chunk's values.  A raw-text
//! regex for `film\s*{` matches THAT decoy first (it appears earlier in
//! the file than the real chunk) and never reaches the real one -- this
//! was S2's original `ResizeFilmChunk` bug, caught by the new hard
//! dimension Check in RunLayer2Showcase below (see "OBSERVED LAYER 2
//! HARNESS FIXES" in the file header).  Comment trivia is never a Chunk
//! node, so this lookup cannot be fooled by it.
static Cst::NodeId FindChunkByRole( const Cst::Document& doc, const std::string& roleExact, const std::string& roleSuffix = std::string() )
{
	const int n = Cst::DocItemCount( doc );
	for( int i = 0; i < n; ++i ) {
		const Cst::NodeId nid = Cst::DocNodeIdAt( doc, i );
		const Cst::NodeRef it = Cst::DocResolveNodeId( doc, nid );
		if( !it || it->kind != Cst::NodeKind::Chunk ) continue;
		const std::string& role = it->role;
		if( !roleExact.empty() && role == roleExact ) return nid;
		if( !roleSuffix.empty() && role.size() > roleSuffix.size() &&
			role.compare( role.size() - roleSuffix.size(), roleSuffix.size(), roleSuffix ) == 0 ) {
			return nid;
		}
	}
	return 0;
}

//! (K-P2a) Counted sibling of FindChunkByRole: returns the FIRST matching
//! chunk (same semantics as above) but also reports how many top-level
//! chunks matched via `*outCount`.  A "_rasterizer" lookup that silently
//! returns the first of TWO matches would be dangerously misleading here:
//! Job::RegisterAndActivateRasterizer (src/Library/Job.cpp:14841, the
//! unconditional `pRasterizer = pRaster; activeRasterizerName = name;`
//! at Job.cpp:14878-14879, run again by every Add*Rasterizer call with no
//! guard) makes the LAST-PARSED rasterizer chunk the one that actually
//! renders -- so if a showcase scene ever grew a second `*_rasterizer`
//! chunk (e.g. a leftover from a hand-edit), FindChunkByRole's "first
//! match" would resolve, verify, and structurally replace the WRONG one
//! while the derived job quietly rendered the other, untouched chunk.
//! The callers below hard-`Check(count == 1)` so that scenario fails
//! loudly instead of silently mis-swapping.
static Cst::NodeId FindChunkByRoleCounted( const Cst::Document& doc, const std::string& roleExact, const std::string& roleSuffix, int* outCount )
{
	const int n = Cst::DocItemCount( doc );
	Cst::NodeId first = 0;
	int count = 0;
	for( int i = 0; i < n; ++i ) {
		const Cst::NodeId nid = Cst::DocNodeIdAt( doc, i );
		const Cst::NodeRef it = Cst::DocResolveNodeId( doc, nid );
		if( !it || it->kind != Cst::NodeKind::Chunk ) continue;
		const std::string& role = it->role;
		bool matches = false;
		if( !roleExact.empty() && role == roleExact ) matches = true;
		if( !roleSuffix.empty() && role.size() > roleSuffix.size() &&
			role.compare( role.size() - roleSuffix.size(), roleSuffix.size(), roleSuffix ) == 0 ) {
			matches = true;
		}
		if( matches ) {
			if( count == 0 ) first = nid;
			++count;
		}
	}
	if( outCount ) *outCount = count;
	return first;
}

//! Resize the scene's `film` chunk to an aspect-preserving ~`targetWidth`-
//! wide target by STRUCTURAL node edit (DocSetOrAddParamValue on the
//! film chunk found via FindChunkByRole), never by text regex -- see that
//! function's header comment for the decoy-comment failure mode this
//! replaces.  `*outNewW`/`*outNewH` report the size actually written, so
//! the caller can assert the captured render came out exactly that size.
static Cst::Document ResizeFilmChunkCst( const Cst::Document& inDoc, unsigned int targetWidth, unsigned int* outNewW, unsigned int* outNewH )
{
	*outNewW = 0;
	*outNewH = 0;
	Cst::Document doc = inDoc;
	const Cst::NodeId filmId = FindChunkByRole( doc, "film" );
	Check( filmId > 0, "Layer 2: the showcase's film chunk is found by role" );
	if( filmId == 0 ) return doc;

	const Cst::NodeRef filmChunk = Cst::DocResolveNodeId( doc, filmId );
	bool presentW = false, presentH = false;
	const std::string wStr = Cst::ParamValueAsParsed( filmChunk, "width", &presentW );
	const std::string hStr = Cst::ParamValueAsParsed( filmChunk, "height", &presentH );
	unsigned int origW = 800, origH = 600;
	if( presentW ) { try { origW = static_cast<unsigned int>( std::stoul( wStr ) ); } catch( ... ) {} }
	if( presentH ) { try { origH = static_cast<unsigned int>( std::stoul( hStr ) ); } catch( ... ) {} }

	const unsigned int newW = targetWidth;
	const unsigned int newH = std::max<unsigned int>( 1u,
		static_cast<unsigned int>( std::lround( double(targetWidth) * double(origH) / double(origW) ) ) );

	doc = Cst::DocSetOrAddParamValue( doc, filmId, "width", 0, std::to_string( newW ) );
	doc = Cst::DocSetOrAddParamValue( doc, filmId, "height", 0, std::to_string( newH ) );

	*outNewW = newW;
	*outNewH = newH;
	return doc;
}

//! Build the replacement `..._rasterizer { ... }` chunk TEXT for
//! `integrator`, carrying over `radiance_map`/`radiance_background` from
//! the original chunk when present (pavilion_colonnade's own env map).
//! No standard_shader chunk is emitted here: every one of the four
//! showcases already declares `standard_shader { name global ... }`
//! (plank_closeup / tidal_stones / shelf_bunny with `shaderop
//! DefaultPathTracing`, pavilion_colonnade with `shaderop
//! DefaultDirectLighting`) -- the modern PT/BDPT/VCM rasterizers below
//! don't consult the shader chain's op at all, they only need
//! `defaultshader` to resolve to a real shader chunk (BDPTStrategyBalance-
//! Test's own header explains why the legacy pixelpel_rasterizer, unlike
//! these, DOES execute the chain literally) -- so pavilion_colonnade's
//! DefaultDirectLighting "global" is a perfectly valid reference for these
//! three swapped-in chunks.  No `file_rasterizeroutput` chunk is emitted
//! either: the caller captures pixels via its own CapturingRasterizerOutput
//! and calls `RemoveRasterizerOutputs()` before adding it, so any output
//! chunk the ORIGINAL scene declared (left untouched by the structural
//! chunk-index replace below) is harmless -- it derives, gets removed, and
//! is never read.
static std::string BuildSwapRasterizerText( Integrator integrator, unsigned int samples, bool hasRadianceMap, const std::string& radianceMap, bool hasRadianceBackground, const std::string& radianceBackground )
{
	std::ostringstream ss;
	switch( integrator ) {
		case Integrator::PT:
			ss << "pathtracing_pel_rasterizer\n{\n\tdefaultshader global\n\tsamples " << samples << "\n\trr_min_depth 8\n";
			break;
		case Integrator::BDPT:
			ss << "bdpt_pel_rasterizer\n{\n\tdefaultshader global\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " << samples << "\n";
			break;
		case Integrator::VCM:
			ss << "vcm_pel_rasterizer\n{\n\tdefaultshader global\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " << samples
			   << "\n\tmerge_radius 0.0\n\tvc_enabled true\n\tvm_enabled true\n";
			break;
		case Integrator::MLT:
			ss << "mlt_rasterizer\n{\n\tdefaultshader global\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tbootstrap_samples 4096\n\tchains 64\n\tmutations_per_pixel 16\n\tlarge_step_prob 0.3\n";
			break;
	}
	ss << "\tpixel_filter box\n\toidn_denoise FALSE\n";
	if( hasRadianceMap )        ss << "\tradiance_map " << radianceMap << "\n";
	if( hasRadianceBackground ) ss << "\tradiance_background " << radianceBackground << "\n";
	ss << "}\n";
	return ss.str();
}

//! Structurally REPLACE the scene's rasterizer chunk (found by role suffix
//! "_rasterizer" -- it is unnamed, same idiom as the film chunk above) with
//! a fresh `integrator` chunk via DocReplaceItem, addressed by the chunk's
//! own top-level index resolved through DocIndexOfNodeId.  Unlike the old
//! text-splice `SwapRasterizer` (brace-counted from a keyword string
//! search) this cannot mismatch chunk boundaries or be fooled by a keyword
//! occurring inside a comment or string.
static Cst::Document SwapRasterizerCst( const Cst::Document& inDoc, Integrator integrator, unsigned int samples )
{
	Cst::Document doc = inDoc;
	int rastCount = 0;
	const Cst::NodeId rastId = FindChunkByRoleCounted( doc, std::string(), "_rasterizer", &rastCount );
	Check( rastId > 0, "Layer 2: the showcase's rasterizer chunk is found by role" );
	// (K-P2a) Job::RegisterAndActivateRasterizer activates whichever
	// rasterizer chunk was parsed LAST (Job.cpp:14841, unconditional
	// pRasterizer/activeRasterizerName assignment at 14878-14879) -- a
	// scene with two `*_rasterizer` chunks would silently derive/render
	// the SECOND one while this function structurally replaces the FIRST
	// (FindChunkByRole's documented "first match" semantics).  Fail loudly
	// instead of swapping the wrong chunk.
	Check( rastCount == 1, "Layer 2: the showcase has EXACTLY ONE rasterizer chunk (found " + std::to_string( rastCount ) + ")" );
	if( rastId == 0 || rastCount != 1 ) return doc;

	const Cst::NodeRef rastChunk = Cst::DocResolveNodeId( doc, rastId );
	Cst::NodeRef rastItem;
	const int idx = Cst::DocIndexOfNodeId( doc, rastId, &rastItem );
	Check( idx >= 0, "Layer 2: the rasterizer chunk's top-level index resolves" );
	if( idx < 0 ) return doc;

	bool hasMap = false, hasBg = false;
	const std::string mapVal = Cst::ParamValueAsParsed( rastChunk, "radiance_map", &hasMap );
	const std::string bgVal  = Cst::ParamValueAsParsed( rastChunk, "radiance_background", &hasBg );

	const std::string newChunkText = BuildSwapRasterizerText( integrator, samples, hasMap, mapVal, hasBg, bgVal );
	const Cst::NodeRef newChunkItem = FirstChunkItemFromText( newChunkText );
	Check( (bool)newChunkItem, "Layer 2: the replacement rasterizer chunk text parses" );
	if( !newChunkItem ) return doc;

	return Cst::DocReplaceItem( doc, idx, newChunkItem );
}

struct ShowcaseSpec
{
	const char* keyword;
	const char* relPath;
};

static const ShowcaseSpec kShowcases[] = {
	{ "plank",    "scenes/FeatureBased/Textures/plank_closeup.RISEscene" },
	{ "tidal",    "scenes/FeatureBased/Textures/tidal_stones.RISEscene" },
	{ "bunny",    "scenes/FeatureBased/Textures/shelf_bunny.RISEscene" },
	{ "pavilion", "scenes/FeatureBased/Combined/pavilion_colonnade.RISEscene" },
};

static const unsigned int kLayer2TargetWidth = 160;
static const unsigned int kLayer2Samples = 32;
//! (K-P1, this fix round) The plank BDPT masked ratio-of-ratios sat only
//! 1.66sigma from the 20% band edge over 9 independent runs at 32 spp --
//! see "BAND DERIVATION -- MASKED LAYER" for the measurement.  Raising
//! kLayer2Samples to 96 for BDPT/VCM (tried first, empirically) did NOT
//! shrink the spread -- it made it slightly WORSE (n=6 sd 0.0549 vs the
//! 32-spp n=9 sd 0.0432, worst -0.1327 vs -0.1282) -- matching the file's
//! own earlier note that 64 spp "did not tighten the ratio spread".  The
//! fix that DOES work empirically is averaging the FINAL masked ratio
//! over kLayer2MaskedSubRenders independent full renders (fresh Job/
//! Rasterizer per sub-render, so each gets its own explicit
//! `srand( g_seedBase + g_renderIndex++ )` seed immediately before its
//! `Rasterize()` call -- NOT the clock, see the seeding comment at
//! g_seedBase's declaration -- see RunLayer2Showcase's masked-ratio loop)
//! for BDPT/VCM only; PT is untouched (K=1, its own noise is already
//! <0.05% per OBSERVED POST-FIX NUMBERS).
//!
//! (debt 28 follow-up, 2026-09-11) K is now ADAPTIVE rather than a
//! fixed 12.  The tidal showcase's masked BDPT row -- which only began
//! running at all once debt 28's aperture fix brought tidal's
//! whole-image BDPT/PT inside the blow-up gate -- failed roughly 4 runs
//! in 6 at K=12, and the reason is visible in the numbers rather than
//! in the integrators: tidal's masked pixels are ~30x darker than its
//! frame average (masked PT mean 0.0017 against a whole-image 0.056,
//! over 13.5% of pixels), so its per-sub-render masked ratio has a much
//! wider spread than plank's, and a K tuned on plank is simply not
//! enough samples there.  Fixed K is the wrong shape of knob for an
//! estimator whose per-showcase variance differs by that much.
//!
//! So the loop keeps drawing sub-renders until the STANDARD ERROR of
//! the mean ratio is <= kLayer2MaskedSEFraction of the band, or the cap
//! is reached.  A row that reaches the cap with the standard error
//! still above that target is NOT quietly passed or failed -- it is
//! reported as INSUFFICIENT PRECISION and counted as a skip, the same
//! honesty the blow-up gate already practises.  kLayer2MaskedSubRenders
//! is the MINIMUM (the sample size the SE estimate itself needs to mean
//! anything, and the number the band was derived at).
static const unsigned int kLayer2MaskedSubRenders = 12;
static const unsigned int kLayer2MaskedSubRendersCap = 48;
//! Target standard error of the mean masked ratio, as a fraction of
//! kLayer2MaskedBand.  1/4 of the band means a row that passes is ~4
//! standard errors inside it -- i.e. the pass is about the estimator's
//! CENTRE, not about where one draw happened to land.
static const double kLayer2MaskedSEFraction = 0.25;
static const double kLayer2Band = 0.05;			// design doc section 6.2's own number (whole-image)
static const double kLayer2SensitivityBand = 0.05;	// whole-image
static const double kLayer2MaskThreshold = 0.20;	// |PT(E)-PT(B)|/PT(B) > this -> "the signal moved this pixel"
static const double kLayer2MaskMinCoverage = 0.01;	// mask must cover >= 1% of pixels to be trusted
static const double kLayer2MaskedBand = 0.20;		// see "BAND DERIVATION -- MASKED LAYER" in the file header
static const double kLayer2BlowupLow = 0.5, kLayer2BlowupHigh = 2.0;	// non-signal integrator-disagreement gate
static const double kLayer2BlowupAgreeBand = 0.10;	// (F2) E and B must also agree with EACH OTHER within this to call a blow-up "not signal-attributable"

//! Total blow-up skips across every showcase/integrator (task item 5):
//! counted here, printed in main()'s summary, never silently absorbed.
static int g_blowupSkipCount = 0;

//! Masked rows that hit kLayer2MaskedSubRendersCap with the standard
//! error still above target.  Neither a pass nor a failure: the run did
//! not buy enough precision to say.  Printed in main()'s summary.
static int g_maskedPrecisionSkipCount = 0;

//! (supervisor ruling, debt 28 round 2) The masked ratio-of-ratios
//! R_E/R_B assumes PT's own bias is material-independent across the E
//! and B variants at the masked pixels.  On a masked set that is itself
//! reached only by a transport class PT's NEE cannot sample (e.g. a
//! delta light refracted through a dielectric), PT's E-vs-B response is
//! direct-light-only while integrator I's is the full response, so
//! R_E != R_B for a reason that has nothing to do with the signal under
//! test.  `kReferenceIncompleteThreshold` gates that: if integrator I's
//! masked NEUTRAL-variant ratio to PT, R_B,mask = mean_mask(I,B) /
//! mean_mask(PT,B), misses 1 by more than this fraction, PT is not a
//! usable reference for that row's masked comparison at all -- assert
//! the PT-independent BDPT<->VCM cross-invariant instead (see
//! RunLayer2Showcase).  Threshold is deliberately coarse (a gate, not
//! the asserted quantity) and evaluated from the row's cheap sub-render
//! #1 rather than an averaged value.
static const double kReferenceIncompleteThreshold = 0.5;

//! Rows where the masked PT reference was ruled incomplete (see
//! `kReferenceIncompleteThreshold`) and excluded from the PT-referenced
//! masked-band assertion.  Printed in main()'s summary alongside the
//! other skip counters -- this one is not a precision gap, it is a
//! documented reference-completeness gap (docs/RENDERING_INTEGRATORS.md
//! debt 27).
static int g_referenceIncompleteCount = 0;

//! (supervisor ruling, debt 28 round 3) SYMMETRIC reference-completeness
//! for the PT-INDEPENDENT BDPT<->VCM cross-check itself.  The
//! cross-check (see RunLayer2Showcase's "CROSS-INTEGRATOR MASKED
//! INVARIANT" block) is only a meaningful referee between BDPT and VCM
//! if the two of them agree with each other on the NEUTRAL (B) variant's
//! masked pixels to begin with -- two integrators that already disagree
//! by ~20x on the neutral variant (tidal_stones: BDPT/PT masked ~2.8-3.0x,
//! VCM/PT masked ~57.5-58x) cannot referee a 20% signal invariant between
//! themselves.  Reusing `kReferenceIncompleteThreshold` (0.5) rather than
//! inventing a second magic number: if
//! `| mean_mask(BDPT,B) / mean_mask(VCM,B) - 1 |` exceeds it, the
//! cross-check is skipped (not asserted, not silently dropped) and a
//! labelled `NO COMPLETE REFERENCE ON MASK` line is printed with all
//! three ratios (BDPT/VCM neutral-variant, PT/BDPT, PT/VCM).  This is a
//! THIRD skip kind, distinct from `g_blowupSkipCount` (integrator
//! disagreement on the whole image) and `g_referenceIncompleteCount`
//! (PT unusable as a reference for one integrator's masked row): here PT
//! is not even in the asserted quantity, but the two referees disagree
//! with each other too much to referee.  Tracked as a real, open
//! integrator disagreement under docs/RENDERING_INTEGRATORS.md debt 30 --
//! not a test-harness gap.
static const double kNoCompleteReferenceThreshold = kReferenceIncompleteThreshold;

//! Cross-checks skipped by `kNoCompleteReferenceThreshold` above.
//! Printed in main()'s summary alongside the other skip counters.
static int g_noCompleteReferenceSkipCount = 0;

//! (debt 28 round 4, item 4) Showcases dropped from the WHOLE-IMAGE
//! R_E/R_B assertion because PT itself is not sensitive to its own
//! signal calls at this resolution/spp (|ratio-1| <= kLayer2SensitivityBand).
//! Not a failure -- there is nothing to witness either way at this
//! sample count -- but it must be COUNTED rather than silently NOTE'd,
//! so a run that drops every showcase this way is still visible in the
//! summary line.
static int g_wholeImageInsensitiveCount = 0;

//! (debt 28 round 4, item 4) Masked layers dropped because the mask
//! covers < kLayer2MaskMinCoverage of the frame -- too few pixels to
//! trust a masked ratio-of-ratios.  Not a failure, but counted for the
//! same reason as `g_wholeImageInsensitiveCount` above: a NOTE alone
//! can vanish into scrollback and make a whole showcase's masked
//! coverage silently absent from the result.
static int g_maskedLayerDroppedCoverageCount = 0;

//! Redirect std::cout into a private buffer for the duration of `fn`,
//! returning what was written, and echo it back to the REAL stdout
//! afterward so nothing a human is watching is lost.  GlobalLog's
//! StreamPrinter writes through `std::cout` (Log.cpp:179-180), so this
//! captures every render-time log line -- including the integrator-
//! specific markers CheckIntegratorMarker below greps for -- without
//! needing a second logging channel.
static std::string CaptureStdoutDuring( const std::function<void()>& fn )
{
	std::ostringstream capture;
	std::streambuf* old = std::cout.rdbuf( capture.rdbuf() );
	fn();
	std::cout.rdbuf( old );
	std::cout << capture.str();
	return capture.str();
}

//! Per-render log markers that are unconditional (fire on every render of
//! that integrator, regardless of scene content) -- verified by reading
//! the emitting call sites: `BDPTRasterizerBase.cpp:1050` prints "BDPT
//! Progressive:: All pixels complete..." unconditionally at the end of
//! every BDPT progressive pass; `VCMRasterizerBase.cpp`'s `PreRenderSetup`
//! (called at a fixed pipeline hook every VCM render, per its own header
//! comment) prints "VCMRasterizerBase::PreRenderSetup::" on EVERY branch
//! (VM disabled, no specular found, auto-radius computed, auto-radius
//! failed).  PT's own `PixelBasedRasterizerHelper.cpp:1282` print
//! ("Progressive:: All pixels complete...", no prefix) is NOT distinctive
//! on its own -- VCM's progressive pass reuses the same shared helper and
//! prints the identical line -- so PT is verified by the ABSENCE of the
//! other two markers instead.
static const char* kMarkerBDPT = "BDPT Progressive::";
static const char* kMarkerVCM  = "VCMRasterizerBase::PreRenderSetup::";

static void CheckIntegratorMarker( Integrator integ, const std::string& log, const std::string& keyword, const char* variantTag )
{
	const bool hasBDPT = log.find( kMarkerBDPT ) != std::string::npos;
	const bool hasVCM  = log.find( kMarkerVCM ) != std::string::npos;
	const std::string tag = std::string( keyword ) + " " + variantTag + " " + IntegratorName( integ );
	switch( integ ) {
		case Integrator::PT:
			Check( !hasBDPT && !hasVCM, tag + ": no BDPT/VCM marker in the render log (did not silently fall back through/to a different integrator)" );
			break;
		case Integrator::BDPT:
			Check( hasBDPT, tag + ": \"BDPT Progressive::\" marker present (BDPT actually ran)" );
			break;
		case Integrator::VCM:
			Check( hasVCM, tag + ": \"VCMRasterizerBase::PreRenderSetup::\" marker present (VCM actually ran)" );
			break;
		default: break;
	}
}

//! (F1) The keyword SwapRasterizerCst is supposed to have written for each
//! integrator -- the POSITIVE half of the swap verification.
//! CheckIntegratorMarker above is a log-side witness (present markers
//! prove BDPT/VCM specifically ran); this is a document-side witness
//! (the chunk that will actually be derived really is that keyword) so a
//! swap that silently produced/left a `pathtracing_pel_rasterizer` chunk
//! for a BDPT or VCM row fails loudly here even before any render happens.
static const char* ExpectedRasterizerKeyword( Integrator integ )
{
	switch( integ ) {
		case Integrator::PT:   return "pathtracing_pel_rasterizer";
		case Integrator::BDPT: return "bdpt_pel_rasterizer";
		case Integrator::VCM:  return "vcm_pel_rasterizer";
		case Integrator::MLT:  return "mlt_rasterizer";
	}
	return "";
}

//! (F1) Positive verification of a post-SwapRasterizerCst document: the
//! unique `*_rasterizer` chunk resolves, its keyword (Chunk::role) is
//! EXACTLY the intended integrator's, and it still carries
//! `oidn_denoise FALSE` + `pixel_filter box` (read back from the chunk's
//! own parameters, not assumed from the text this file generated) -- the
//! same non-MIS-cause checklist every rasterizer builder in this file is
//! written to satisfy (docs/skills/bdpt-vcm-mis-balance.md Step 0).
static void VerifySwappedRasterizerChunk( const Cst::Document& doc, Integrator integ, const std::string& keyword, const char* variantTag )
{
	const std::string tag = keyword + " " + variantTag + " " + IntegratorName( integ );
	int rastCount = 0;
	const Cst::NodeId rastId = FindChunkByRoleCounted( doc, std::string(), "_rasterizer", &rastCount );
	Check( rastId > 0, tag + ": swapped rasterizer chunk resolves by role" );
	// (K-P2a) Same one-and-only-one guard as SwapRasterizerCst -- see that
	// function's comment for why "first match" alone is not enough
	// (Job::RegisterAndActivateRasterizer activates the LAST-parsed
	// rasterizer chunk, Job.cpp:14841/14878-14879).
	Check( rastCount == 1, tag + ": exactly one rasterizer chunk resolves by role (found " + std::to_string( rastCount ) + ")" );
	if( rastId == 0 || rastCount != 1 ) return;

	const Cst::NodeRef rastChunk = Cst::DocResolveNodeId( doc, rastId );
	const std::string expected = ExpectedRasterizerKeyword( integ );
	Check( (bool)rastChunk && rastChunk->role == expected,
		tag + ": swapped rasterizer chunk keyword is \"" + expected + "\" (got \""
		+ ( rastChunk ? rastChunk->role : std::string("<null>") ) + "\") -- a swap that silently fell back to a "
		"different integrator's chunk now fails loudly here, not just in a flat-looking mean" );
	if( !rastChunk ) return;

	bool hasOidn = false, hasFilter = false;
	const std::string oidnVal   = Cst::ParamValueAsParsed( rastChunk, "oidn_denoise", &hasOidn );
	const std::string filterVal = Cst::ParamValueAsParsed( rastChunk, "pixel_filter", &hasFilter );
	Check( hasOidn && oidnVal == "FALSE",
		tag + ": swapped chunk carries oidn_denoise FALSE (got \"" + ( hasOidn ? oidnVal : std::string("<absent>") ) + "\")" );
	Check( hasFilter && filterVal == "box",
		tag + ": swapped chunk carries pixel_filter box (got \"" + ( hasFilter ? filterVal : std::string("<absent>") ) + "\")" );
}

//! Per-pixel composited-over-black scalar (base*coverage averaged across
//! channels) -- the SAME quantity ComputeStats averages over the whole
//! image, kept per-pixel here so the masked ratio-of-ratios (design doc
//! S2 addendum, "LAYER 2 -- the mask that can actually witness the gap"
//! below) can restrict its mean to a pixel subset instead of the whole
//! frame.
static void ComputePerPixelValues( const CapturingRasterizerOutput& cap, std::vector<double>& out )
{
	out.resize( cap.pixels.size() );
	for( std::size_t i = 0; i < cap.pixels.size(); ++i ) {
		const RISEColor& c = cap.pixels[i];
		const double cov = c.a;
		out[i] = ( ( c.base.r + c.base.g + c.base.b ) / 3.0 ) * cov;
	}
}

static double VectorMean( const std::vector<double>& v )
{
	if( v.empty() ) return 0.0;
	double sum = 0.0;
	for( double x : v ) sum += x;
	return sum / double( v.size() );
}

struct Layer2Row
{
	Integrator integ;
	std::vector<double> valsE, valsB;
	double meanE = 0.0, meanB = 0.0;
};

//! (F2) A whole-image blow-up gate that keys ONLY on `mean(I,E)/mean(PT,E)`
//! cannot tell a genuine "E and B agree with each other, only the OTHER
//! integrator disagrees with PT" non-signal blow-up (the documented VCM
//! auto-radius case) from an ASYMMETRIC one where E and B disagree WITH
//! EACH OTHER under the blown-up integrator -- which would itself be
//! evidence of a signal-attributable regression hiding behind the skip.
//! Blowup:     both ratioE and ratioB fall outside [0.5x,2x] of PT AND
//!             ratioE/ratioB agrees with 1 within kLayer2BlowupAgreeBand --
//!             the known, previously-recorded non-signal case.
//! Asymmetric: only one of {ratioE,ratioB} is outside the band, OR both
//!             are outside but E/B disagree by more than the agree band --
//!             NOT skipped; the caller must fail loudly on this.
//!
//! (K-P2b, this fix round) `agree` is the LITERAL ratio-of-ratios
//! agreement |ratioE/ratioB - 1|, not meanE/meanB.  An earlier draft
//! computed |meanE/meanB - 1| here while this comment (and the printed
//! "E/B agree within" lines) claimed ratioE/ratioB -- the two coincide
//! only when meanPT_E ~= meanPT_B, since
//! ratioE/ratioB = (meanE/meanPT_E)/(meanB/meanPT_B)
//!              = (meanE/meanB) * (meanPT_B/meanPT_E).
//! Every showcase this file has run against happens to be whole-image
//! INSENSITIVE (mean(PT,E) ~= mean(PT,B), see "LAYER 2 FINDING" in the
//! file header), which is exactly the condition under which the two
//! formulas agree -- so the old code's classifications were correct by
//! coincidence, not by construction.  Compute the quantity the comment
//! (and the skip-report text below) actually claims.
enum class BlowupClass { None, Blowup, Asymmetric };

static BlowupClass ClassifyBlowup(
	const Layer2Row& r, double meanPT_E, double meanPT_B,
	double* outRatioE, double* outRatioB, double* outAgree )
{
	const double ratioE = meanPT_E != 0.0 ? r.meanE / meanPT_E : 0.0;
	const double ratioB = meanPT_B != 0.0 ? r.meanB / meanPT_B : 0.0;
	*outRatioE = ratioE;
	*outRatioB = ratioB;
	const bool eOut = ( meanPT_E == 0.0 ) || ratioE < kLayer2BlowupLow || ratioE > kLayer2BlowupHigh;
	const bool bOut = ( meanPT_B == 0.0 ) || ratioB < kLayer2BlowupLow || ratioB > kLayer2BlowupHigh;
	const double agree = ( ratioB != 0.0 ) ? std::fabs( ratioE / ratioB - 1.0 ) : 1e9;
	*outAgree = agree;
	if( eOut && bOut && agree < kLayer2BlowupAgreeBand ) return BlowupClass::Blowup;
	if( eOut || bOut ) return BlowupClass::Asymmetric;
	return BlowupClass::None;
}

//! (K-P1, this fix round) The single-render body the per-integrator loop
//! in RunLayer2Showcase used to inline, factored out so it can ALSO be
//! called for the extra independent sub-renders the masked ratio-of-
//! ratios averages over (kLayer2MaskedSubRenders) for BDPT/VCM.  Each
//! call builds fresh Cst::Documents, a fresh Job/Rasterizer pair and a
//! fresh render -- exactly what the ORIGINAL per-integrator loop already
//! did once per (showcase, integrator); calling it again gets an
//! independently-seeded render (this function calls
//! `srand( g_seedBase + g_renderIndex++ )` immediately before each of its
//! two `Rasterize()` calls -- see the seeding comment at g_seedBase's
//! declaration for why that is NOT "RISE renders seed from wall clock",
//! which is what this comment used to claim), which is what actually
//! reduces the masked ratio's run-to-run spread -- see
//! kLayer2MaskedSubRenders' own comment for why raising spp in a SINGLE
//! render does not.
static bool RenderShowcaseVariant(
	const std::string& variantEText, const std::string& variantBText,
	Integrator integ, unsigned int samples, const char* keyword,
	unsigned int targetW, unsigned int targetH, Layer2Row* outRow, bool* outDerived = nullptr )
{
	if( outDerived ) *outDerived = false;
	outRow->integ = integ;
	Cst::Document docE = SwapRasterizerCst( Cst::ParseToCst( variantEText ), integ, samples );
	Cst::Document docB = SwapRasterizerCst( Cst::ParseToCst( variantBText ), integ, samples );
	VerifySwappedRasterizerChunk( docE, integ, keyword, "E" );
	VerifySwappedRasterizerChunk( docB, integ, keyword, "B" );

	Job* jobE = new Job();
	Job* jobB = new Job();
	std::vector<std::string> diagsE, diagsB;
	Cst::DeriveToJob( docE, *jobE, &diagsE );
	Cst::DeriveToJob( docB, *jobB, &diagsB );
	for( const auto& d : diagsE ) std::cout << "  " << keyword << " " << IntegratorName(integ) << " E diagnostic: " << d << std::endl;
	for( const auto& d : diagsB ) std::cout << "  " << keyword << " " << IntegratorName(integ) << " B diagnostic: " << d << std::endl;
	Check( diagsE.empty(), std::string( keyword ) + " " + IntegratorName(integ) + " variant E derives with no diagnostics" );
	Check( diagsB.empty(), std::string( keyword ) + " " + IntegratorName(integ) + " variant B derives with no diagnostics" );
	if( !diagsE.empty() || !diagsB.empty() ) { safe_release( jobE ); safe_release( jobB ); return false; }
	if( outDerived ) *outDerived = true;

	jobE->RemoveRasterizerOutputs();
	jobB->RemoveRasterizerOutputs();

	CapturingRasterizerOutput* capE = new CapturingRasterizerOutput(); capE->addref();
	CapturingRasterizerOutput* capB = new CapturingRasterizerOutput(); capB->addref();
	jobE->GetRasterizer()->AddRasterizerOutput( capE );
	jobB->GetRasterizer()->AddRasterizerOutput( capB );

	// See the seeding comment at g_seedBase's declaration: this binary is
	// not wall-clock seeded.  E and B each get their own explicit `srand`
	// call immediately before their own `Rasterize()`, so a sub-render
	// call (see kLayer2MaskedSubRenders below) is independent of the
	// sub-render before it by construction, not by hoping the
	// unsynchronized worker-side `rand()` race decorrelates them.
	bool renderedE = false, renderedB = false;
	std::srand( g_seedBase + g_renderIndex++ );
	const std::string logE = CaptureStdoutDuring( [&]() { renderedE = jobE->Rasterize(); } );
	std::srand( g_seedBase + g_renderIndex++ );
	const std::string logB = CaptureStdoutDuring( [&]() { renderedB = jobB->Rasterize(); } );
	Check( renderedE, std::string( keyword ) + " " + IntegratorName(integ) + " variant E renders" );
	Check( renderedB, std::string( keyword ) + " " + IntegratorName(integ) + " variant B renders" );
	CheckIntegratorMarker( integ, logE, keyword, "E" );
	CheckIntegratorMarker( integ, logB, keyword, "B" );

	if( renderedE ) {
		Check( capE->width == targetW && capE->height == targetH,
			std::string( keyword ) + " " + IntegratorName(integ) + " E: captured image is the requested "
			+ std::to_string(targetW) + "x" + std::to_string(targetH) + " (got "
			+ std::to_string(capE->width) + "x" + std::to_string(capE->height) + ")" );
		ComputePerPixelValues( *capE, outRow->valsE );
		outRow->meanE = VectorMean( outRow->valsE );
	}
	if( renderedB ) {
		Check( capB->width == targetW && capB->height == targetH,
			std::string( keyword ) + " " + IntegratorName(integ) + " B: captured image is the requested "
			+ std::to_string(targetW) + "x" + std::to_string(targetH) + " (got "
			+ std::to_string(capB->width) + "x" + std::to_string(capB->height) + ")" );
		ComputePerPixelValues( *capB, outRow->valsB );
		outRow->meanB = VectorMean( outRow->valsB );
	}

	std::cout << "  " << IntegratorName(integ) << ": mean(E)=" << outRow->meanE << " mean(B)=" << outRow->meanB << std::endl;

	capE->release();
	capB->release();
	safe_release( jobE );
	safe_release( jobB );

	return renderedE && renderedB;
}

static void RunLayer2Showcase( const fs::path& root, const ShowcaseSpec& spec )
{
	const fs::path scenePath = root / spec.relPath;
	const std::string original = ReadFile( scenePath );
	Check( !original.empty(), std::string( "Layer 2: " ) + spec.keyword + " scene file reads" );
	if( original.empty() ) return;

	unsigned int targetW = 0, targetH = 0;
	const Cst::Document resizedDoc = ResizeFilmChunkCst( Cst::ParseToCst( original ), kLayer2TargetWidth, &targetW, &targetH );
	if( targetW == 0 ) return;		// film chunk not found; already Checked false above

	const std::string resizedText = Cst::SerializeCst( resizedDoc );
	const std::string variantEText = resizedText;
	const std::string variantBText = BuildNeutralVariant( resizedText );

	std::cout << "\n=== LAYER 2: " << spec.keyword << " (target " << targetW << "x" << targetH << ") ===" << std::endl;

	double meanPT_E = 0, meanPT_B = 0;
	bool ptOk = false;
	std::vector<Layer2Row> rows;

	for( Integrator integ : { Integrator::PT, Integrator::BDPT, Integrator::VCM } ) {
		Layer2Row row;
		bool derived = false;
		const bool renderedBoth = RenderShowcaseVariant( variantEText, variantBText, integ, kLayer2Samples, spec.keyword, targetW, targetH, &row, &derived );
		if( !derived ) continue;		// matches the original loop's "diagnostics non-empty -> skip this row entirely"

		if( integ == Integrator::PT ) {
			meanPT_E = row.meanE;
			meanPT_B = row.meanB;
			ptOk = renderedBoth;
		}
		rows.push_back( std::move( row ) );
	}

	if( !ptOk || meanPT_B == 0.0 ) {
		std::cout << "  NOTE: " << spec.keyword << " PT reference unavailable -- skipping ratio assertions for this showcase." << std::endl;
		return;
	}

	std::cout << "  RAW MEANS (E variant): ";
	for( const Layer2Row& r : rows ) std::cout << IntegratorName(r.integ) << "=" << r.meanE << "  ";
	std::cout << std::endl;

	const double sensitivity = meanPT_E / meanPT_B - 1.0;
	std::cout << "  SENSITIVITY: mean(PT,E)=" << meanPT_E << " mean(PT,B)=" << meanPT_B
	          << " ratio-1=" << sensitivity << std::endl;

	const bool sensitive = std::fabs( sensitivity ) > kLayer2SensitivityBand;
	if( !sensitive ) {
		std::cout << "  NOTE: " << spec.keyword << " is NOT sensitive to its own signal calls at this resolution/spp "
		          << "(|ratio-1| = " << std::fabs(sensitivity) << " <= " << kLayer2SensitivityBand
		          << ") -- dropped from the WHOLE-IMAGE R_E/R_B assertion (cannot witness the gap either way)." << std::endl;
		g_wholeImageInsensitiveCount++;
	}

	// (F2) Per-integrator blow-up gate (task item 5): a >2x or <0.5x
	// whole-image disagreement with PT is a KNOWN non-signal integrator
	// issue (VCM auto-radius instability at low spp; see
	// docs/RENDERING_INTEGRATORS.md) ONLY when E and B agree with EACH
	// OTHER under the blown-up integrator -- both the whole-image and the
	// masked ratio-of-ratios are meaningless there and are SKIPPED, not
	// asserted on.  An ASYMMETRIC blow-up (only one of E/B outside the
	// band, or both outside but disagreeing with each other by more than
	// 10%) is NOT skipped -- it is exactly the shape a signal-attributable
	// regression would take (E and B priced differently by the SAME
	// integrator), so it fails loudly here instead.
	for( const Layer2Row& r : rows ) {
		if( r.integ == Integrator::PT ) continue;
		double ratioE = 0.0, ratioB = 0.0, agree = 0.0;
		const BlowupClass cls = ClassifyBlowup( r, meanPT_E, meanPT_B, &ratioE, &ratioB, &agree );
		if( cls == BlowupClass::Blowup ) {
			std::cout << "  " << IntegratorName(r.integ) << ": mean(" << IntegratorName(r.integ) << ")/mean(PT,E) = " << ratioE
			          << "  mean(" << IntegratorName(r.integ) << ")/mean(PT,B) = " << ratioB
			          << "  (E/B agree within " << agree << ")"
			          << " -- INTEGRATOR DISAGREEMENT (not signal-attributable; recorded in docs/RENDERING_INTEGRATORS.md)"
			          << " -- SKIPPING ratio-of-ratios for this showcase." << std::endl;
			g_blowupSkipCount++;
			continue;
		}
		if( cls == BlowupClass::Asymmetric ) {
			std::cout << "  " << IntegratorName(r.integ) << ": mean(" << IntegratorName(r.integ) << ")/mean(PT,E) = " << ratioE
			          << "  mean(" << IntegratorName(r.integ) << ")/mean(PT,B) = " << ratioB
			          << "  E/B disagreement = " << agree
			          << "  -- ASYMMETRIC BLOW-UP (possible signal-attributable regression, NOT skipped)" << std::endl;
			Check( false, std::string( spec.keyword ) + " " + IntegratorName(r.integ)
				+ ": asymmetric blow-up (ratioE=" + std::to_string(ratioE) + " ratioB=" + std::to_string(ratioB)
				+ " E/B disagreement=" + std::to_string(agree) + ") is NOT a known non-signal integrator disagreement" );
		}
		if( sensitive ) {
			if( r.meanE == 0.0 || meanPT_E == 0.0 || r.meanB == 0.0 ) {
				Check( false, std::string( spec.keyword ) + " " + IntegratorName(r.integ) + ": non-zero whole-image means for R_E/R_B" );
			} else {
				const double R_E = r.meanE / meanPT_E;
				const double R_B = r.meanB / meanPT_B;
				const double ratio = R_E / R_B - 1.0;
				std::cout << "  WHOLE-IMAGE " << IntegratorName(r.integ) << ": R_E=" << R_E << " R_B=" << R_B
				          << " R_E/R_B-1=" << ratio
				          << ( std::fabs(ratio) < kLayer2Band ? "  [pass]" : "  [FAIL]" ) << std::endl;
				Check( std::fabs( ratio ) < kLayer2Band,
					std::string( spec.keyword ) + " " + IntegratorName(r.integ) + " (whole-image): | R_E/R_B - 1 | < " + std::to_string(kLayer2Band) );
			}
		}
	}

	// LAYER 2 -- the mask that can actually witness the gap (design doc S2
	// addendum).  Whole-image means average over every pixel in the frame,
	// most of which the signal never touches; the mask restricts the mean
	// to exactly the pixels where PT(E) and PT(B) -- the one integrator
	// known to price the signal correctly -- disagree by more than 20%.
	// PT is used to BUILD the mask (never to price it: the ratio-of-ratios
	// below still compares each integrator's OWN E/B means, never PT's),
	// so the mask is an independent selector, not a second copy of the
	// invariant under test.
	const Layer2Row* ptRow = nullptr;
	for( const Layer2Row& r : rows ) if( r.integ == Integrator::PT ) { ptRow = &r; break; }

	if( !ptRow || ptRow->valsE.empty() || ptRow->valsE.size() != ptRow->valsB.size() ) {
		// Unlike the sensitivity and coverage drops below, this is not a
		// legitimate "nothing to witness" case -- PT was rendered (ptOk
		// was already asserted true above) and its per-pixel arrays
		// SHOULD exist; their absence means the row failed to capture
		// per-pixel data, silently dropping the whole masked layer for
		// this showcase.  That is a harness failure, not a NOTE.
		Check( false, std::string( spec.keyword ) + ": PT per-pixel arrays unavailable for the masked ratio-of-ratios" );
		return;
	}

	const std::size_t N = ptRow->valsE.size();
	std::vector<bool> mask( N, false );
	std::size_t maskCount = 0;
	static const double kMaskEps = 1e-6;
	for( std::size_t i = 0; i < N; ++i ) {
		const double e = ptRow->valsE[i];
		const double b = ptRow->valsB[i];
		if( std::fabs( e - b ) / std::max( b, kMaskEps ) > kLayer2MaskThreshold ) { mask[i] = true; maskCount++; }
	}
	const double maskFrac = double( maskCount ) / double( N );
	// (F4) 3 decimal places on the printed coverage percentage: plank's
	// coverage (~2.1%, see BAND DERIVATION -- MASKED LAYER in the file
	// header) sits at roughly 2x the 1% floor, and that headroom is only
	// visible to a reader at this precision -- "2%" alone looks alarmingly
	// close to the floor.
	std::cout << "  MASK: " << maskCount << "/" << N << " pixels ("
	          << std::fixed << std::setprecision(3) << ( maskFrac * 100.0 ) << std::defaultfloat << std::setprecision(6)
	          << "%) with |PT(E)-PT(B)|/PT(B) > " << ( kLayer2MaskThreshold * 100.0 ) << "%" << std::endl;

	if( maskFrac < kLayer2MaskMinCoverage ) {
		std::cout << "  NOTE: " << spec.keyword << " masked coverage "
		          << std::fixed << std::setprecision(3) << ( maskFrac * 100.0 ) << std::defaultfloat << std::setprecision(6)
		          << "% < " << ( kLayer2MaskMinCoverage * 100.0 ) << "% -- skipping the masked ratio-of-ratios (too few pixels to witness the gap)." << std::endl;
		g_maskedLayerDroppedCoverageCount++;
		return;
	}

	auto maskedMean = [&]( const std::vector<double>& v ) -> double {
		if( v.size() != N ) return 0.0;
		double sum = 0.0; std::size_t n = 0;
		for( std::size_t i = 0; i < N; ++i ) if( mask[i] ) { sum += v[i]; n++; }
		return n > 0 ? sum / double(n) : 0.0;
	};

	const double maskedPT_E = maskedMean( ptRow->valsE );
	const double maskedPT_B = maskedMean( ptRow->valsB );
	std::cout << "  MASKED PT: mean(E)=" << maskedPT_E << " mean(B)=" << maskedPT_B << std::endl;

	// (debt 28 follow-up) INDEPENDENT PT DENOMINATORS.
	//
	// The estimator is
	//     R_E/R_B = (maskedE / maskedB) * (maskedPT_B / maskedPT_E),
	// so the PT pair enters as a single MULTIPLICATIVE factor common to
	// every sub-render.  Re-rendering only BDPT/VCM therefore averages
	// away the integrator-side noise and leaves the PT-side noise
	// untouched -- which is why the old fixed-K estimator's within-run
	// spread looked small while its across-run spread was ~3.5x larger
	// (measured on tidal: within-run SE 0.017 at K=12 against a
	// run-to-run sd of ~0.06 over the six runs tabulated in
	// docs/RENDERING_INTEGRATORS.md).  An SE computed from BDPT draws
	// alone is not the SE of the quantity being asserted, and a
	// stopping rule built on it would stop early and confidently.
	//
	// So each sub-render index gets its OWN PT pair, drawn from this
	// lazily-grown pool.  Index 0 is the primary PT row already
	// rendered above.  The pool is shared between the BDPT and VCM
	// loops of the same showcase: sub-render i of both uses PT draw i,
	// which costs max(K_bdpt, K_vcm) PT pairs per showcase instead of
	// their sum, at the price of a little correlation BETWEEN the two
	// rows -- they are separate assertions, so that is harmless.
	//
	// The MASK is NOT redrawn.  It is a selector, not part of the
	// estimator (see the header's note that PT only BUILDS the mask),
	// and redefining which pixels are measured per sub-render would
	// average over a different quantity each time.
	std::vector< std::pair<double,double> > ptDenomPool;
	ptDenomPool.push_back( std::make_pair( maskedPT_E, maskedPT_B ) );

	//! Returns the i-th independent PT denominator pair, rendering it
	//! if the pool has not reached that far.  Returns (0,0) if the
	//! render or its pixel arrays come back unusable, which the caller
	//! counts as a sub-render failure exactly like an integrator-side one.
	auto ptDenominators = [&]( std::size_t i ) -> std::pair<double,double> {
		while( ptDenomPool.size() <= i ) {
			Layer2Row ptSub;
			bool ptDerived = false;
			RenderShowcaseVariant( variantEText, variantBText, Integrator::PT, kLayer2Samples, spec.keyword, targetW, targetH, &ptSub, &ptDerived );
			if( !ptDerived || ptSub.valsE.size() != N || ptSub.valsB.size() != N ) {
				return std::make_pair( 0.0, 0.0 );
			}
			const double e = maskedMean( ptSub.valsE );
			const double b = maskedMean( ptSub.valsB );
			if( e == 0.0 || b == 0.0 ) return std::make_pair( 0.0, 0.0 );
			ptDenomPool.push_back( std::make_pair( e, b ) );
		}
		return ptDenomPool[i];
	};

	// (supervisor ruling, debt 28 round 2) REFERENCE-COMPLETENESS GATE.
	// The masked ratio-of-ratios R_E/R_B assumes PT's own bias is
	// material-independent between the E and B variants at the masked
	// pixels.  That fails when the masked set is reached only by a
	// transport class PT's NEE cannot sample (a delta light refracted
	// through a dielectric, on tidal): PT's E-vs-B response there is
	// direct-light-only while integrator I's is the full response, so
	// R_E != R_B for a reason unrelated to the signal under test.  Check
	// that on each row's cheap sub-render #1 (no extra render): if
	// integrator I's masked NEUTRAL-variant ratio to PT strays more than
	// `kReferenceIncompleteThreshold` from 1, PT is not a usable
	// reference for this row's masked comparison -- exclude it from the
	// PT-referenced assertion below and, instead of silently dropping
	// it, assert the PT-independent BDPT<->VCM masked invariant (see the
	// "if( crossCheckNeeded )" block after this loop).
	struct MaskedRowInfo
	{
		Integrator integ = Integrator::PT;
		bool valid = false;
		bool wholeImageBlowup = false;
		bool referenceIncomplete = false;
		double maskedE = 0.0, maskedB = 0.0;
		double R_E = 0.0, R_B = 0.0;
	};
	std::vector<MaskedRowInfo> maskedInfos;
	for( const Layer2Row& r : rows ) {
		if( r.integ == Integrator::PT ) continue;
		MaskedRowInfo info;
		info.integ = r.integ;
		double ratioE = 0.0, ratioB = 0.0, agree = 0.0;
		info.wholeImageBlowup = ( ClassifyBlowup( r, meanPT_E, meanPT_B, &ratioE, &ratioB, &agree ) == BlowupClass::Blowup );
		if( r.valsE.size() != N || r.valsB.size() != N || maskedPT_E == 0.0 || maskedPT_B == 0.0 ) {
			Check( false, std::string( spec.keyword ) + " " + IntegratorName(r.integ) + ": masked means available for R_E/R_B" );
			maskedInfos.push_back( info );
			continue;
		}
		info.maskedE = maskedMean( r.valsE );
		info.maskedB = maskedMean( r.valsB );
		if( info.maskedE == 0.0 || info.maskedB == 0.0 ) {
			Check( false, std::string( spec.keyword ) + " " + IntegratorName(r.integ) + ": non-zero masked means for R_E/R_B" );
			maskedInfos.push_back( info );
			continue;
		}
		info.R_E = info.maskedE / maskedPT_E;
		info.R_B = info.maskedB / maskedPT_B;
		info.referenceIncomplete = std::fabs( info.R_B - 1.0 ) > kReferenceIncompleteThreshold;
		info.valid = true;
		maskedInfos.push_back( info );
	}

	bool crossCheckNeeded = false;
	for( const MaskedRowInfo& info : maskedInfos ) {
		if( info.valid && info.referenceIncomplete ) crossCheckNeeded = true;
	}

	for( const MaskedRowInfo& info : maskedInfos ) {
		if( !info.valid ) continue;

		if( info.referenceIncomplete ) {
			std::cout << "  REFERENCE INCOMPLETE ON MASK (PT vs " << IntegratorName(info.integ)
			          << " neutral-variant masked ratio " << info.R_B << ")" << std::endl;
			g_referenceIncompleteCount++;
			continue;		// no PT-referenced assertion for this row -- see the cross-check block below
		}
		if( info.wholeImageBlowup ) continue;		// already reported + counted above; masked ratio meaningless here (unchanged pre-existing behavior)

		const Integrator integ = info.integ;
		const double R_E = info.R_E;
		const double R_B = info.R_B;

		// Per-sub-render values of the estimator, kept individually (not
		// just summed) so the standard error below is computable.
		std::vector<double> ratios;
		ratios.push_back( R_E / R_B - 1.0 );
		int subRenderFailures = 0;

		// Standard error of the mean of `ratios`.  Returns -1 until
		// there are at least two samples.
		auto standardError = [&]() -> double {
			const std::size_t n = ratios.size();
			if( n < 2 ) return -1.0;
			double mean = 0.0;
			for( double v : ratios ) mean += v;
			mean /= double( n );
			double ss = 0.0;
			for( double v : ratios ) ss += ( v - mean ) * ( v - mean );
			const double sd = std::sqrt( ss / double( n - 1 ) );	// sample sd
			return sd / std::sqrt( double( n ) );
		};

		const double seTarget = kLayer2MaskedBand * kLayer2MaskedSEFraction;

		// (K-P1, then debt 28 follow-up) BDPT/VCM average the masked
		// ratio over INDEPENDENT full renders -- see
		// kLayer2MaskedSubRenders' comment for the measurement showing
		// that this, not a higher single-render spp, is what shrinks the
		// run-to-run spread.  This row's own render above is
		// sub-render #1.  The loop now runs to a PRECISION target rather
		// than a fixed count: at least kLayer2MaskedSubRenders (the
		// sample size the band was derived at, and the minimum for the
		// SE estimate to mean anything), then onward while the standard
		// error of the mean is above `seTarget`, to a hard cap.  PT is
		// not looped (K=1): its own noise is already <0.05% (OBSERVED
		// POST-FIX NUMBERS).
		if( integ == Integrator::BDPT || integ == Integrator::VCM ) {
			while( ratios.size() + (std::size_t)subRenderFailures < (std::size_t)kLayer2MaskedSubRendersCap )
			{
				if( ratios.size() >= (std::size_t)kLayer2MaskedSubRenders ) {
					const double se = standardError();
					if( se >= 0.0 && se <= seTarget ) break;
				}
				Layer2Row subRow;
				bool subDerived = false;
				RenderShowcaseVariant( variantEText, variantBText, integ, kLayer2Samples, spec.keyword, targetW, targetH, &subRow, &subDerived );
				if( !subDerived || subRow.valsE.size() != N || subRow.valsB.size() != N ) { subRenderFailures++; continue; }
				const double subMaskedE = maskedMean( subRow.valsE );
				const double subMaskedB = maskedMean( subRow.valsB );
				if( subMaskedE == 0.0 || subMaskedB == 0.0 ) { subRenderFailures++; continue; }
				const std::pair<double,double> den = ptDenominators( ratios.size() );
				if( den.first == 0.0 || den.second == 0.0 ) { subRenderFailures++; continue; }
				ratios.push_back( ( subMaskedE / den.first ) / ( subMaskedB / den.second ) - 1.0 );
			}
		}

		// (round-3 fix, task item 4; kept under adaptive K) A sub-render
		// that fails to derive, or comes back with a mismatched or
		// degenerate pixel array, used to be `continue`d past silently,
		// so a bad run could average over fewer sub-renders than the
		// estimator was measured at.  Under adaptive K the count is no
		// longer fixed, so what is asserted is the thing that was
		// actually wrong: NO sub-render failed, and the minimum sample
		// size was reached.
		if( integ == Integrator::BDPT || integ == Integrator::VCM ) {
			Check( subRenderFailures == 0,
				std::string( spec.keyword ) + " " + IntegratorName(integ)
				+ " (masked): every sub-render succeeded (failures: "
				+ std::to_string(subRenderFailures) + ")" );
			Check( ratios.size() >= (std::size_t)kLayer2MaskedSubRenders,
				std::string( spec.keyword ) + " " + IntegratorName(integ)
				+ " (masked): at least " + std::to_string(kLayer2MaskedSubRenders)
				+ " sub-renders (got " + std::to_string(ratios.size()) + ")" );
		}

		double ratio = 0.0;
		for( double v : ratios ) ratio += v;
		ratio /= double( ratios.size() );
		const double se = standardError();

		const bool loopedRow = ( integ == Integrator::BDPT || integ == Integrator::VCM );
		const bool insufficient = loopedRow && se > seTarget;

		std::cout << "  MASKED " << IntegratorName(integ) << ": R_E=" << R_E << " R_B=" << R_B
		          << " R_E/R_B-1(sub-render 1)=" << ( R_E / R_B - 1.0 )
		          << "  avg over " << ratios.size() << " sub-render(s)=" << ratio;
		if( loopedRow ) {
			std::cout << "  SE=" << se << " (target <= " << seTarget << ")";
		}
		if( insufficient ) {
			std::cout << "  [INSUFFICIENT PRECISION]" << std::endl;
			std::cout << "  INSUFFICIENT PRECISION: " << spec.keyword << " "
			          << IntegratorName(integ)
			          << " (masked) reached the " << kLayer2MaskedSubRendersCap
			          << "-sub-render cap with SE=" << se << " > " << seTarget
			          << ".  The estimate (" << ratio << ") is NOT asserted either way "
			          << "-- counted as a skip, not a pass and not a failure." << std::endl;
			g_maskedPrecisionSkipCount++;
			continue;
		}
		std::cout << ( std::fabs(ratio) < kLayer2MaskedBand ? "  [pass]" : "  [FAIL]" ) << std::endl;
		Check( std::fabs( ratio ) < kLayer2MaskedBand,
			std::string( spec.keyword ) + " " + IntegratorName(integ) + " (masked): | R_E/R_B - 1 | < " + std::to_string(kLayer2MaskedBand) );
	}

	// (supervisor ruling, debt 28 round 2) CROSS-INTEGRATOR MASKED
	// INVARIANT.  Triggered whenever at least one of {BDPT,VCM}'s masked
	// PT reference was ruled incomplete above.  The PT-referenced
	// ratio-of-ratios is unusable there, but a PT-INDEPENDENT invariant
	// still holds if BDPT and VCM see the same masked-pixel transport:
	//     | (mean_mask(BDPT,E)/mean_mask(VCM,E))
	//       / (mean_mask(BDPT,B)/mean_mask(VCM,B)) - 1 | < kLayer2MaskedBand
	// Algebraically this is (mean_mask(BDPT,E)/mean_mask(BDPT,B))
	// / (mean_mask(VCM,E)/mean_mask(VCM,B)) - 1 -- each integrator's OWN
	// E/B self-ratio, compared to the other's -- so PT enters this
	// computation only through the (fixed, PT-derived) mask's pixel
	// selection, never through its own masked means.  Both integrators'
	// self-ratios are measured with the SAME adaptive-K machinery as the
	// PT-referenced loop above (independent sub-renders, minimum
	// kLayer2MaskedSubRenders, SE target kLayer2MaskedBand *
	// kLayer2MaskedSEFraction, capped at kLayer2MaskedSubRendersCap), run
	// UNCONDITIONALLY for both BDPT and VCM -- including a row whose
	// whole-image ratio was itself a blow-up skip (tidal's VCM row: the
	// blow-up gate above only ever gated the PT-REFERENCED assertions;
	// it must not also suppress the masked means this cross-check needs).
	//
	// (supervisor ruling, debt 28 round 3) SYMMETRIC gate on this
	// cross-check.  Before running the expensive adaptive sub-render
	// loop below, the code checks whether BDPT and VCM even agree with
	// each other on the NEUTRAL (B) variant's masked pixels (cheap
	// sub-render #1, no extra render) -- if they don't, within the same
	// 50% band `kReferenceIncompleteThreshold` already uses, PT-freedom
	// does not make this cross-check meaningful: two referees who
	// disagree ~20x with each other cannot referee a 20% invariant
	// between themselves.  That row prints `NO COMPLETE REFERENCE ON
	// MASK`, is counted in `g_noCompleteReferenceSkipCount`, and is NOT
	// asserted.  See `kNoCompleteReferenceThreshold`'s own comment and
	// docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md section 6.2.
	if( crossCheckNeeded ) {
		std::cout << "  CROSS-CHECK TRIGGERED: at least one masked row's PT reference is incomplete -- "
		          << "asserting the PT-independent BDPT<->VCM masked invariant instead (see "
		          << "docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md section 6.2)." << std::endl;

		const MaskedRowInfo* bdptInfo = nullptr;
		const MaskedRowInfo* vcmInfo = nullptr;
		for( const MaskedRowInfo& info : maskedInfos ) {
			if( info.integ == Integrator::BDPT ) bdptInfo = &info;
			if( info.integ == Integrator::VCM  ) vcmInfo  = &info;
		}

		if( !bdptInfo || !vcmInfo || !bdptInfo->valid || !vcmInfo->valid ) {
			Check( false, std::string( spec.keyword ) + " BDPT<->VCM (masked cross-check): both rows' masked means are available" );
		} else {
			// (supervisor ruling, debt 28 round 3) SYMMETRIC
			// reference-completeness gate on the cross-check ITSELF.  The
			// PT-independent BDPT<->VCM invariant below is only a
			// meaningful referee if BDPT and VCM agree with each other on
			// the NEUTRAL (B) variant's masked pixels to begin with -- see
			// `kNoCompleteReferenceThreshold`'s own comment.  Computed from
			// the cheap sub-render #1 already sitting in `maskedInfos`
			// (`info.maskedB`), no extra render, symmetric with how
			// `kReferenceIncompleteThreshold` is evaluated above.  Every
			// masked mean and ratio is printed regardless of which branch
			// this takes.
			const double bdptVcmNeutralRatio = ( vcmInfo->maskedB != 0.0 ) ? bdptInfo->maskedB / vcmInfo->maskedB : 0.0;
			const double bdptVcmNeutralAgree = std::fabs( bdptVcmNeutralRatio - 1.0 );
			const double ptOverBdptNeutral = ( bdptInfo->maskedB != 0.0 ) ? maskedPT_B / bdptInfo->maskedB : 0.0;
			const double ptOverVcmNeutral  = ( vcmInfo->maskedB  != 0.0 ) ? maskedPT_B / vcmInfo->maskedB  : 0.0;
			std::cout << "  NEUTRAL-VARIANT MASKED MEANS: PT=" << maskedPT_B
			          << " BDPT=" << bdptInfo->maskedB << " VCM=" << vcmInfo->maskedB
			          << "  BDPT/VCM=" << bdptVcmNeutralRatio
			          << " PT/BDPT=" << ptOverBdptNeutral << " PT/VCM=" << ptOverVcmNeutral << std::endl;

			if( bdptVcmNeutralAgree > kNoCompleteReferenceThreshold ) {
				std::cout << "  NO COMPLETE REFERENCE ON MASK (BDPT/VCM neutral-variant masked ratio "
				          << bdptVcmNeutralRatio << "; PT/BDPT " << ptOverBdptNeutral
				          << "; PT/VCM " << ptOverVcmNeutral << ")" << std::endl;
				g_noCompleteReferenceSkipCount++;
				return;
			}

			auto standardErrorOf = []( const std::vector<double>& v ) -> double {
				const std::size_t n = v.size();
				if( n < 2 ) return -1.0;
				double mean = 0.0;
				for( double x : v ) mean += x;
				mean /= double( n );
				double ss = 0.0;
				for( double x : v ) ss += ( x - mean ) * ( x - mean );
				const double sd = std::sqrt( ss / double( n - 1 ) );
				return sd / std::sqrt( double( n ) );
			};
			const double seTarget = kLayer2MaskedBand * kLayer2MaskedSEFraction;

			//! selfContrast_i = mean_mask(I,E)_i / mean_mask(I,B)_i - 1
			//! over independent sub-renders of integrator `info.integ`.
			//! No PT denominator anywhere -- that is the entire point of
			//! this invariant.
			auto adaptiveSelfContrast = [&]( const MaskedRowInfo& info, std::vector<double>* outContrasts, int* outFailures ) {
				outContrasts->push_back( info.maskedE / info.maskedB - 1.0 );
				*outFailures = 0;
				while( outContrasts->size() + (std::size_t)*outFailures < (std::size_t)kLayer2MaskedSubRendersCap ) {
					if( outContrasts->size() >= (std::size_t)kLayer2MaskedSubRenders ) {
						const double se = standardErrorOf( *outContrasts );
						if( se >= 0.0 && se <= seTarget ) break;
					}
					Layer2Row subRow;
					bool subDerived = false;
					RenderShowcaseVariant( variantEText, variantBText, info.integ, kLayer2Samples, spec.keyword, targetW, targetH, &subRow, &subDerived );
					if( !subDerived || subRow.valsE.size() != N || subRow.valsB.size() != N ) { (*outFailures)++; continue; }
					const double subE = maskedMean( subRow.valsE );
					const double subB = maskedMean( subRow.valsB );
					if( subE == 0.0 || subB == 0.0 ) { (*outFailures)++; continue; }
					outContrasts->push_back( subE / subB - 1.0 );
				}
			};

			std::vector<double> bdptContrasts, vcmContrasts;
			int bdptFailures = 0, vcmFailures = 0;
			adaptiveSelfContrast( *bdptInfo, &bdptContrasts, &bdptFailures );
			adaptiveSelfContrast( *vcmInfo,  &vcmContrasts,  &vcmFailures );

			Check( bdptFailures == 0, std::string( spec.keyword ) + " BDPT (masked cross-check): every sub-render succeeded (failures: " + std::to_string(bdptFailures) + ")" );
			Check( vcmFailures == 0,  std::string( spec.keyword ) + " VCM (masked cross-check): every sub-render succeeded (failures: "  + std::to_string(vcmFailures)  + ")" );
			Check( bdptContrasts.size() >= (std::size_t)kLayer2MaskedSubRenders, std::string( spec.keyword ) + " BDPT (masked cross-check): at least " + std::to_string(kLayer2MaskedSubRenders) + " sub-renders (got " + std::to_string(bdptContrasts.size()) + ")" );
			Check( vcmContrasts.size()  >= (std::size_t)kLayer2MaskedSubRenders, std::string( spec.keyword ) + " VCM (masked cross-check): at least "  + std::to_string(kLayer2MaskedSubRenders) + " sub-renders (got " + std::to_string(vcmContrasts.size())  + ")" );

			const double meanBdpt = VectorMean( bdptContrasts );
			const double meanVcm  = VectorMean( vcmContrasts );
			const double seBdpt = standardErrorOf( bdptContrasts );
			const double seVcm  = standardErrorOf( vcmContrasts );

			const double bdptSelfRatio = 1.0 + meanBdpt;	// mean_mask(BDPT,E)/mean_mask(BDPT,B)
			const double vcmSelfRatio  = 1.0 + meanVcm;	// mean_mask(VCM,E)/mean_mask(VCM,B)
			const double crossRatio = ( vcmSelfRatio != 0.0 ) ? bdptSelfRatio / vcmSelfRatio : 0.0;
			const double crossValue = crossRatio - 1.0;

			// Delta-method propagation: the relative SE of a ratio of two
			// INDEPENDENT means is the root-sum-square of their own
			// relative SEs (first order).  BDPT's and VCM's sub-renders
			// are drawn independently of each other (and of PT), so this
			// is the right combination rule.
			const double relSeBdpt = ( bdptSelfRatio != 0.0 && seBdpt >= 0.0 ) ? std::fabs( seBdpt / bdptSelfRatio ) : 0.0;
			const double relSeVcm  = ( vcmSelfRatio  != 0.0 && seVcm  >= 0.0 ) ? std::fabs( seVcm  / vcmSelfRatio  ) : 0.0;
			const double crossSE = std::fabs( crossRatio ) * std::sqrt( relSeBdpt * relSeBdpt + relSeVcm * relSeVcm );

			std::cout << "  CROSS-CHECK masked self-ratio: BDPT mean_mask(E)/mean_mask(B)=" << bdptSelfRatio
			          << " (K=" << bdptContrasts.size() << " SE=" << seBdpt << ")"
			          << "  VCM mean_mask(E)/mean_mask(B)=" << vcmSelfRatio
			          << " (K=" << vcmContrasts.size() << " SE=" << seVcm << ")" << std::endl;
			std::cout << "  CROSS-CHECK (BDPT/VCM masked self-ratio) - 1 = " << crossValue
			          << "  SE~=" << crossSE
			          << ( std::fabs(crossValue) < kLayer2MaskedBand ? "  [pass]" : "  [FAIL]" ) << std::endl;
			Check( std::fabs( crossValue ) < kLayer2MaskedBand,
				std::string( spec.keyword ) + " BDPT<->VCM (masked cross-check): | (mean_mask(BDPT,E)/mean_mask(VCM,E)) / (mean_mask(BDPT,B)/mean_mask(VCM,B)) - 1 | < " + std::to_string(kLayer2MaskedBand) );
		}
	}
}

//======================================================================
// main
//======================================================================

int main( int argc, char** argv )
{
	// See the seeding comment at g_seedBase's declaration.  argv[1], if
	// given, is an alternate seed base -- the FabricRenderTest /
	// PrimitiveSelfHitTest convention for taking a genuinely independent
	// sample on demand (e.g. `./SignalIntegratorConsistencyTest 2000`).
	if( argc > 1 ) {
		const long v = std::strtol( argv[1], nullptr, 10 );
		if( v > 0 ) g_seedBase = (unsigned int)v;
	}

	if( const char* env = std::getenv( "SIGNAL_CONSISTENCY_FILTER" ) ) {
		g_filter = env;
	}

	std::cout << "SignalIntegratorConsistencyTest -- docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md S2" << std::endl;
	std::cout << "seed base = " << g_seedBase
		<< "  (pass a different one as argv[1] for an independent sample)" << std::endl;
	if( !g_filter.empty() ) std::cout << "Filter: " << g_filter << std::endl;

	if( WantsLayer1() ) {
		double convexityHarness = 0, occlusionHarness = 0, thicknessHarness = 0;

		SignalUnitScene curvScene = BuildCurvScene();
		SignalUnitScene convexityScene = BuildConvexityScene( &convexityHarness );
		SignalUnitScene occlusionScene = BuildOcclusionScene( &occlusionHarness );
		SignalUnitScene thicknessScene = BuildThicknessScene( &thicknessHarness );
		SignalUnitScene proximityScene = BuildProximityScene();
		SignalUnitScene interiorScene = BuildInteriorScene();

		std::cout << "\nReference harness values: convexity(0.3)=" << convexityHarness
		          << "  occlusion-derived t=" << occlusionHarness
		          << "  thickness(0.1)=" << thicknessHarness << std::endl;

		RunLayer1Scene( curvScene, /*includeMlt=*/true );
		RunLayer1Scene( convexityScene, false );
		RunLayer1Scene( occlusionScene, false );
		RunLayer1Scene( thicknessScene, false );
		RunLayer1Scene( proximityScene, false );
		RunLayer1Scene( interiorScene, false );
	}

	bool anyShowcase = false;
	for( const ShowcaseSpec& spec : kShowcases ) if( WantsShowcase( spec.keyword ) ) anyShowcase = true;
	if( anyShowcase ) {
		const fs::path root = FindRepoRoot();
		Check( !root.empty(), "Layer 2: repo root found" );
		if( !root.empty() ) {
			for( const ShowcaseSpec& spec : kShowcases ) {
				if( !WantsShowcase( spec.keyword ) ) continue;
				RunLayer2Showcase( root, spec );
			}
		}
	}

	std::cout << "\n========================================" << std::endl;
	std::cout << "Passed: " << passCount << "  Failed: " << failCount
	          << "  Blow-up skips (integrator disagreement, not signal-attributable): " << g_blowupSkipCount
	          << "  Masked precision skips (cap reached, SE still above target): " << g_maskedPrecisionSkipCount
	          << "  Reference-incomplete masked rows (PT unusable as reference, BDPT<->VCM cross-check asserted instead): " << g_referenceIncompleteCount
	          << "  No-complete-reference skips (BDPT and VCM disagree on the neutral variant too, masked cross-check skipped): " << g_noCompleteReferenceSkipCount
	          << "  Whole-image insensitive showcases (PT not sensitive to its own signal at this res/spp): " << g_wholeImageInsensitiveCount
	          << "  Masked layer dropped: coverage (< " << ( kLayer2MaskMinCoverage * 100.0 ) << "% of pixels): " << g_maskedLayerDroppedCoverageCount << std::endl;
	std::cout << "========================================" << std::endl;

	return failCount > 0 ? 1 : 0;
}
