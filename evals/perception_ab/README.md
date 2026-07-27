# Perception-atlas paired A/B

This eval asks whether one local vision model answers controlled scene questions
more accurately when its single observation is the RISE perception atlas rather
than beauty alone.

The comparison is paired and budget-matched:

- RISE renders each case once with perception capture enabled.
- The beauty arm receives `read_image {representation:"beauty", maxEdge:384}`.
- The atlas arm receives `read_image {representation:"perception", maxEdge:384}`.
- Both inputs are therefore one 384x384 PNG, use the same prompt, model,
  temperature, output cap, and number of calls. The atlas pays for its extra
  channels by reducing each panel to 192x192.
- First-arm order is block-randomized: it is balanced within every task family,
  and every case sees both orders across its odd number of repeats.
- Questions have two exact labels. Cases mirror label, side, and sign so a fixed
  response policy scores 50% rather than masquerading as cue use.

The twelve cases target three claims that beauty does not determine reliably
from one view: front-surface depth order, the sign of a world-space surface
normal, and intrinsic albedo under compensating illumination. Beauty can retain
weak perspective or silhouette cues, so this is a controlled diagnostic rather
than an information-free baseline or a natural-task benchmark. A positive
result demonstrates that the selected model can use the atlas as a whole; it
does not identify a causal panel or establish the same gain for arbitrary models
or prompts.

Run from the repository root after building `bin/rise` and starting Ollama:

```sh
python3 tools/perception_ab_eval.py
```

The tool selects the smallest installed model advertising Ollama's `vision`
capability (override with `--model`), disables hidden reasoning for speed, and
writes auditable PNGs, raw responses, a JSON summary, and a Markdown report
under the ignored `evals/runs/perception_ab_*` directory. Generated scene text,
exact prompts, snapshots and hashes of the harness and manifest, and the RISE
binary hash pin the run provenance. The tool captures these inputs before
inference, revalidates the Ollama tag/digest before every call, and aborts if the
model, hashes, or Git state change during the run. An explicit in-repository
`--output-dir` must be covered by Git ignore rules so output cannot alter that
state; external directories are also allowed. Use `--self-test` for the
dependency-free scoring/parser checks and `--help` for overrides.

Treat each unique authored case as the primary unit. The exact paired sign test
therefore compares case-majority outcomes. Repeat-pooled accuracy and
`atlas_only_wins` versus `beauty_only_wins` remain useful stability diagnostics,
but byte-identical repeated inputs are correlated and receive no inferential
p-value. Prompt-token parity is true only when usage is present and matched for
every complete pair. Repeats must be odd so every arm has an unambiguous case
majority.
