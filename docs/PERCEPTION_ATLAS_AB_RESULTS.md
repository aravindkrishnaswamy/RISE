# Perception atlas: first local A/B result

**Run date:** 2026-07-26 (America/Los_Angeles)  
**Model:** Ollama `qwen3.6:27b`, digest `a50eda8ed977…`  
**RISE base revision:** `be336e23`  
**Harness:** [tools/perception_ab_eval.py](../tools/perception_ab_eval.py) with
[evals/perception_ab/cases.json](../evals/perception_ab/cases.json)

## Result

Yes: this local model successfully used part of the atlas to improve exact
answers in the controlled suite. Across 12 mirrored cases and three repeats:

| Observation | Correct | Accuracy | Wilson 95% interval |
|---|---:|---:|---:|
| Beauty only | 15 / 36 | 41.7% | 27.1–57.8% |
| Perception atlas | 25 / 36 | 69.4% | 53.1–82.0% |
| **Difference** | **+10** | **+27.8 points** | — |

Paired outcomes were 17 atlas-only wins versus 7 beauty-only wins, with 8 pairs
both correct and 4 both wrong. The exact two-sided McNemar/sign-test p-value is
0.0639: suggestive overall evidence, just outside a conventional 0.05 threshold.

The lift was not uniform:

| Task family | Beauty | Atlas | Delta | Atlas-only / beauty-only | Exact p |
|---|---:|---:|---:|---:|---:|
| Depth order | 5/12 (41.7%) | 7/12 (58.3%) | +16.7 points | 4 / 2 | 0.6875 |
| Material vs. lighting | 5/12 (41.7%) | 12/12 (100%) | +58.3 points | 7 / 0 | 0.0156 |
| World-normal sign | 5/12 (41.7%) | 6/12 (50.0%) | +8.3 points | 6 / 5 | 1.0 |

The clean result is material-versus-lighting diagnosis: `qwen3.6:27b` used the
top-right albedo panel consistently. It did not reliably decode log depth or
negative-X normal colors despite the prompt explaining both encodings.

## Controls

- Every call used the same model, prompt, temperature (`0.1`), hidden-reasoning
  setting (`none`), and 32-token output ceiling.
- Each arm received exactly one 384x384 PNG. Beauty used the full resolution;
  the atlas paid for four cues with 192x192 panels.
- Ollama reported exactly equal prompt tokens within all 36 pairs (0% maximum
  relative difference; 299, 303, or 309 tokens depending on question family).
- RISE rendered each case once; both observations came from that same cached
  frame. Arm order was shuffled deterministically within each pair.
- Cases mirror correct label, left/right placement, and normal sign. A constant
  label or side policy scores 50%.
- Ground truth comes from the authored scene parameters, not a visual judge.
  Generated atlases were visually checked after calibration, including the
  initially troublesome right-side depth and negative-X normal cases.
- Median local response latency was 1.09 seconds; all 72 calls took 70.6 seconds
  of measured inference latency. RISE rendered each 384x384 case in under 100 ms.

## What this does and does not establish

The result closes the narrowest evidence gap: at least one real local vision
model can extract useful conditional information from the shipped atlas, at an
equal reported image-token budget. It does **not** yet establish a general
agent-task improvement. Three repeats of one parameterized scene topology are
correlated; using per-case majority, beauty solved 6/12 unique cases and atlas
8/12 (5 atlas-only versus 3 beauty-only, exact p=0.7266). This is why the result
is described as positive preliminary signal rather than a rollout-wide win.

The next useful evaluation is a larger set of unique, natural tool-using tasks:
object placement, occlusion diagnosis, edit localization, and relighting. The
weak depth/normal result also motivates testing panel labels, per-channel image
returns, or a model-facing legend before spending on a broader model matrix.

Reproduce with:

```sh
python3 tools/perception_ab_eval.py --model qwen3.6:27b --repeats 3
```

Raw responses and generated PNGs are written beneath gitignored
`evals/runs/perception_ab_<timestamp>/`; the committed tool records image hashes,
dimensions, metadata, token usage, latency, exact answers, and paired statistics.
