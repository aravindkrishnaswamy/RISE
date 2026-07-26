# Render-checkpoint grading convergence (2026-07-25)

Backing data for the epoch-13 decision to PIN `samples` (and frame size) on the
`render` checkpoints of the 7 build/observe scenarios. Recorded here because the
measurement justified a GRADING change and would otherwise be unreproducible.

## Method

Each cell's `build_ambiguous_scene.final.RISEscene` (the scene the model left
behind, from `evals/runs/gemini_only_e12` and `evals/runs/ask_user_board_e12`)
was re-rendered through the CLI with only `samples` varied, writing an
UNCOMPRESSED linear Rec.709 EXR (`file_rasterizeroutput { type EXR
exr_compression none color_space Rec709RGB_Linear bpp 32 }`), and mean luma
computed as the unweighted mean of R, G, B over all pixels -- the same quantity
`CheckRenderKind` bands.

CAVEAT, stated plainly: this is an independent re-render through the CLI, not
the harness's in-session grading path, so absolute values are not expected to
match a recorded `digest.json` exactly (and do not). The comparisons below are
all WITHIN this method -- same scene, same path, only `samples` varying -- which
is what the convergence and stability questions actually turn on.

## Mean luma vs sample count, all 20 epoch-12 Gemini cells

Band under test: `meanLumaMin 0.015`, `meanLumaMax 0.35`.

| cell | luma @8spp | verdict | luma @512spp | verdict | drift |
|---|---|---|---|---|---|
| `gemini_only/gemini-3.5-flash__r1` | 0.263 | pass | 0.242 | pass | 8% |
| `gemini_only/gemini-3.5-flash__r2` | 0.005 | FAIL | 0.006 | FAIL | 20% |
| `gemini_only/gemini-3.5-flash__r3` | 0.269 | pass | 0.292 | pass | 9% |
| `gemini_only/gemini-3.5-flash__r4` | 0.751 | FAIL | 0.595 | FAIL | 21% |
| `gemini_only/gemini-3.5-flash__r5` | 0.104 | pass | 0.106 | pass | 2% |
| `gemini_only/gemini-3.6-flash__r1` | 0.431 | FAIL | 0.439 | FAIL | 2% |
| `gemini_only/gemini-3.6-flash__r2` | 0.128 | pass | 0.159 | pass | 23% |
| `gemini_only/gemini-3.6-flash__r3` | 0.162 | pass | 0.180 | pass | 11% |
| `gemini_only/gemini-3.6-flash__r4` | 0.280 | pass | 0.313 | pass | 12% |
| `gemini_only/gemini-3.6-flash__r5` | 0.281 | pass | 0.251 | pass | 11% |
| `ask_user_board/gemini-3.5-flash__r1` | 0.195 | pass | 0.181 | pass | 7% |
| `ask_user_board/gemini-3.5-flash__r2` | 0.244 | pass | 0.210 | pass | 14% |
| `ask_user_board/gemini-3.5-flash__r3` | 0.398 | FAIL | 0.420 | FAIL | 6% |
| `ask_user_board/gemini-3.5-flash__r4` | 1.978 | FAIL | 1.912 | FAIL | 3% |
| `ask_user_board/gemini-3.5-flash__r5` | 0.124 | pass | 0.115 | pass | 7% |
| `ask_user_board/gemini-3.6-flash__r1` | 1.608 | FAIL | 1.276 | FAIL | 21% |
| `ask_user_board/gemini-3.6-flash__r2` | 0.138 | pass | 0.141 | pass | 2% |
| `ask_user_board/gemini-3.6-flash__r3` | 0.311 | pass | 0.320 | pass | 3% |
| `ask_user_board/gemini-3.6-flash__r4` | 0.290 | pass | 0.496 | FAIL | 71% **FLIP** |
| `ask_user_board/gemini-3.6-flash__r5` | 2.403 | FAIL | 2.626 | FAIL | 9% |

**Median drift 9%, max 71%, 1 verdict flip in 20.**

## Repeatability at a fixed sample count

Renders are stochastic run-to-run (no fixed seed). Five repeats of the same
scene at the same settings:

| cell | spp | five repeats | spread | verdicts |
|---|---|---|---|---|
| gemini_only/3.5-flash r1 (well-behaved) | 8 | 0.234, 0.257, 0.247, 0.234, 0.260 | 11% | PPPPP |
| gemini_only/3.5-flash r1 (well-behaved) | 512 | 0.249, 0.243, 0.242, 0.256, 0.247 | 6% | PPPPP |
| gemini_only/3.6-flash r5 (well-behaved) | 8 | 0.253, 0.247, 0.245, 0.263, 0.262 | 8% | PPPPP |
| gemini_only/3.6-flash r5 (well-behaved) | 512 | 0.244, 0.252, 0.251, 0.245, 0.244 | 3% | PPPPP |
| gemini_only/3.5-flash r4 (bright fail) | 8 | 0.699, 0.725, 0.721, 0.739, 0.687 | 8% | FFFFF |
| gemini_only/3.5-flash r4 (bright fail) | 512 | 0.605, 0.593, 0.596, 0.599, 0.600 | 2% | FFFFF |
| ask_user_board/3.6-flash r4 (firefly-heavy) | 8 | 0.361, 0.829, 0.342 | 142% | straddles the 0.35 edge |
| ask_user_board/3.6-flash r4 (firefly-heavy) | 512 | 0.460, 0.463, 0.434 | 7% | FFF |

**Well-behaved scenes hold ~8-11% at 8 spp and never flip; the firefly-heavy
scene spans 142% and straddles the band edge -- i.e. a coin flip.** Pinning at
512 brings every measured cell to 2-7%.

## Convergence: is 512 enough?

| cell | 8 | 256 | 512 | 1024 | 256 vs 1024 | 512 vs 1024 |
|---|---|---|---|---|---|---|
| firefly-heavy | 0.290 | 0.372 | 0.496 | 0.471 | 21% | 5% |
| bright fail | 0.751 | 0.598 | 0.595 | 0.597 | 0% | 0% |
| bright fail | 1.608 | 1.325 | 1.276 | 1.316 | 1% | 3% |

256 spp is NOT enough (21% off on the worst cell); 512 holds the worst cell
within ~5% of its 1024-spp value, at ~1 s per grading render on a 32x24 frame
(vs ~20 ms at 8 spp) -- negligible against a ~200 s mean run.

## What this does NOT fix

Pinning makes the metric REPRODUCIBLE, not ROBUST. Mean luminance over a
576-768 pixel HDR frame is outlier-dominated: failing scenes carry pixels at
100-400x display white, and the firefly-heavy cell is still 21% unstable
between 256 and 1024 spp. A clamped or percentile statistic would be the real
fix; it is deliberately not made here because it would change the metric's
meaning in every scenario.

## Why the pin is applied uniformly, including where convergence cannot matter

`two_tool_observe`'s band is `[0.001, 5.0]` — a 5000x span only a black render
can violate — so sample convergence cannot change its verdict, and 512 spp buys
nothing there on convergence grounds. It is pinned anyway for the OTHER half of
the rationale: an unpinned checkpoint grades at whatever `samples`/`film` the
model left in the scene, and the point of the pin is that the subject does not
choose the instrument. Uniformity also means a future band tightening on any
scenario cannot silently reintroduce the unconverged case.

The genuinely load-bearing case is `lighting_luma_band`, whose band is
`[0.045, 0.13]` — the tightest in the suite, on a 24x24 frame.
