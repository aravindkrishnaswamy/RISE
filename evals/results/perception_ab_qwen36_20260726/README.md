# Curated perception-atlas A/B evidence

This tracked snapshot preserves the clean 384x384 `qwen3.6:27b` run summarized
in [docs/PERCEPTION_ATLAS_AB_RESULTS.md](../../../docs/PERCEPTION_ATLAS_AB_RESULTS.md).
It contains all 72 raw response rows, 24 input PNGs, 12 generated RISE scenes,
and the machine-generated summary.

The experiment ran at Git revision `ef02d4b7d4e219ca0bb31717f94702a07d8e2f26`.
That revision reconstructs the exact harness named by `harnessSha256` in
`summary.json`; the manifest and RISE binary are also hash-pinned there. The
source live-run files had these hashes before curation:

- `summary.json`: `02b1433f788eb798f18fde21e0a9d2302149cbbd739c4a42b35a2a5351fb4092`
- `results.jsonl`: `18eaa7fe012a77a8fe203eaccf1aec008cbdedd2493d8d2341c290a0b162e807`

Curation changed only absolute local path strings to repository-relative paths.
Image bytes, scene text, raw model answers, scores, usage, timing, response IDs,
and all content hashes are unchanged. Recompute the committed summary with the
current harness's `summarize` function or independently from `results.jsonl`.
