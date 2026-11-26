# r249 — one-cell flame-edge eligibility

| Variant | Mean eligible CH4 (g) | Mean below 1 m (g) |
|---|---:|---:|
| (b) | 0.077114689 | 0.077081349 |
| (c) | 0.577397442 | 0.236711377 |
| (e) | 0.136753633 | 0.136630745 |

**Adopt (e).** It retains 57.72% of (c)'s eligible methane below 1 m,
which is a majority; 99.91% of its own eligible mass is below 1 m. It retains
23.68% of (c)'s total mass, so this is not a claim to recover most of (c)'s
unrestricted plume eligibility. Values are time-weighted over the same 156
r241 retained states and 14.547731429222 s interval as the original comparison.

| Height (m) | (b), g | (c), g | (e), g |
|---|---:|---:|---:|
| 0–0.250 | 0.058556836 | 0.122511019 | 0.102473745 |
| 0.25–0.500 | 0.012295946 | 0.038425505 | 0.021058598 |
| 0.5–1.000 | 0.006228567 | 0.075774853 | 0.013098403 |
| 1–2.000 | 0.000033340 | 0.143904349 | 0.000122888 |
| 2–3.243 | 0.000000000 | 0.196781717 | 0.000000000 |

The warm map is immutable: a cold cell must share a face with an original
T > 600 K parcel-passing vertex. A waived cold cell cannot qualify its own
cold neighbor. The augmented graph still starts only from the existing warm
pilot/autoignition seeds. The existing floor-holder exception is preserved.

This removes the long cold methane-tail bridges responsible for (c) ≈ (d).
It does not establish the dynamic consumption or heat-height result. The
[new revision](FIRE_SMOKE_EXTINCTION_PARCEL_R249.md) requires a from-zero
33 kW tier-8 run and all three acceptance measurements before tier 10.

[Mean height profile](data/fire_r249/edge_height_profile.csv),
[per-state masses](data/fire_r249/edge_mass_samples.csv), and
[method, decision and SHA manifest](data/fire_r249/edge_comparison.json).
The (b)/(c) masses were reproduced against the prior report, and per-layer
sums reproduce the total mass for each of the 156 states. The offline
probe was built before the production gate changed; its frozen binary and
full per-state profiles remain in the existing r249 output directory.

The previous eligibility deliverable was committed by explicit pathspec as
`a2ce95979` before this follow-up.
