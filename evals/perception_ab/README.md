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
- Arm order is deterministically shuffled within every case/repeat pair.
- Questions have two exact labels. Cases mirror label, side, and sign so a fixed
  response policy scores 50% rather than masquerading as cue use.

The twelve cases isolate three claims that beauty cannot determine reliably from
one view: front-surface depth order, the sign of a world-space surface normal,
and intrinsic albedo under compensating illumination. This is a deliberately
diagnostic suite, not a natural-task benchmark. A positive result demonstrates
that the selected model can decode and use the atlas; it does not establish the
same gain for arbitrary models or prompts.

Run from the repository root after building `bin/rise` and starting Ollama:

```sh
python3 tools/perception_ab_eval.py
```

The tool selects the smallest installed model advertising Ollama's `vision`
capability (override with `--model`), disables hidden reasoning for speed, and
writes auditable PNGs, raw responses, a JSON summary, and a Markdown report
under the ignored `evals/runs/perception_ab_*` directory. Use `--self-test` for
the dependency-free scoring/parser checks and `--help` for overrides.

Interpret the paired output, not only pooled accuracy. `atlas_only_wins` versus
`beauty_only_wins` is the relevant contrast; the report includes the exact
two-sided McNemar/sign-test p-value and prompt-token parity. Repeats expose local
sampling variability but are not independent scene evidence, so the report also
shows case-majority accuracy.
