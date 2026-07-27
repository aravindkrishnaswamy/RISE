# Perception atlas: first local A/B result

**Run date:** 2026-07-26 (America/Los_Angeles)

**Model:** Ollama `qwen3.6:27b`, digest `a50eda8ed977…`

**RISE and harness revision:** `ef02d4b7`

**Harness:** [tools/perception_ab_eval.py](../tools/perception_ab_eval.py) with
[evals/perception_ab/cases.json](../evals/perception_ab/cases.json)

**Tracked evidence:**
[evals/results/perception_ab_qwen36_20260726](../evals/results/perception_ab_qwen36_20260726/README.md)

## Result

This local model produced a positive preliminary atlas signal. The primary
unique-case view uses the majority of three calls per arm: beauty solved 5/12
cases and the atlas solved 7/12. Paired case majorities were 4 atlas-only versus
2 beauty-only wins (exact two-sided p=0.6875), so this small run is directional,
not statistically decisive.

For stability, the correlated repeat-pooled counts were:

| Observation | Correct | Descriptive accuracy |
|---|---:|---:|
| Beauty only | 17 / 36 | 47.2% |
| Perception atlas | 23 / 36 | 63.9% |
| **Difference** | **+6** | **+16.7 points** |

Paired repeat outcomes were 13 atlas-only wins versus 7 beauty-only wins, with
10 pairs both correct and 6 both wrong. Because repeats reuse byte-identical
images and prompts, these 36 pairs are correlated and receive no inferential
p-value.

The effect was not uniform:

| Task family | Repeat-pooled beauty | Repeat-pooled atlas | Case-majority beauty | Case-majority atlas |
|---|---:|---:|---:|---:|
| Depth order | 6/12 (50.0%) | 7/12 (58.3%) | 2/4 | 2/4 |
| Material vs. lighting | 6/12 (50.0%) | 12/12 (100%) | 2/4 | 4/4 |
| World-normal sign | 5/12 (41.7%) | 4/12 (33.3%) | 1/4 | 1/4 |

The replicated material-versus-lighting pattern is consistent with using the
top-right albedo panel, but this full-atlas A/B does not identify a causal
quadrant; normal and geometry cues also change. The model did not reliably
decode log depth or world-normal colors despite the prompt explaining both
encodings.

## Controls and provenance

- Every call used the same model, prompt, temperature (`0.1`), hidden-reasoning
  setting (`none`), and 32-token output ceiling.
- Each arm received exactly one 384x384 PNG. Beauty used the full resolution;
  the atlas paid for four cues with 192x192 panels.
- Ollama reported exactly equal prompt tokens within all 36 pairs (0% maximum
  relative difference; 299, 303, or 309 tokens depending on question family).
- RISE rendered each case once; both observations came from that same cached
  frame. First-arm order was exactly balanced at 18 beauty and 18 atlas pairs,
  then shuffled deterministically.
- Cases mirror correct label, left/right placement, and normal sign. The harness
  derives and validates each answer from scene parameters rather than trusting a
  free-standing manifest label.
- Every render and image response passed representation, availability, panel,
  source-size, output-size, PNG structure, and CRC checks.
- The run used a clean `ef02d4b7` worktree. It records the harness SHA-256
  `d6df7687640b…`, manifest SHA-256 `8981d29a052d…`, RISE binary SHA-256
  `c1c66d1cff5d…`, Ollama `0.32.3`, generated scene text, image hashes, and raw
  responses.
- Median local response latency was 1.04 seconds; all 72 calls took 69.3 seconds
  of measured inference latency. RISE rendered each 384x384 case in 66–84 ms.

## What this does and does not establish

The result gives repeatable directional evidence that one real local vision
model can extract useful conditional information from the shipped atlas at an
equal reported image-token budget. It does **not** establish a statistically
decisive lift, a general agent-task improvement, or a causal atlas panel. The 12
cases share parameterized scene topologies, which further limits generalization.

The next useful evaluation is a larger set of unique, natural tool-using tasks:
object placement, occlusion diagnosis, edit localization, and relighting. The
weak depth/normal result also motivates testing panel labels, per-channel image
returns, and panel ablations before spending on a broader model matrix.

Reproduce with:

```sh
python3 tools/perception_ab_eval.py --model qwen3.6:27b --repeats 3
```

Live raw responses, generated scenes, and PNGs are written beneath gitignored
`evals/runs/perception_ab_<timestamp>/`. The evidence for this result is also
preserved in the tracked snapshot linked above, including the hashes, RPC
metadata, token usage, latency, exact answers, and paired statistics needed to
audit it from a fresh checkout.
