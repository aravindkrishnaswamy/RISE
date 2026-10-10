# DL-546: repeatable routing policy test

The production Tier-2 metric remains cross-render variance times wall time.
The original test's exact PT expectation was not a stable routing margin:
three completed master runs returned 155/2, 153/4, and 153/4 (passed/failed).
Their rounded sigma2T ratios were GGX 1.4/1.6/1.7 (mean 1.567, sample SD
0.153) and corridor 1.1/1.4/2.3 (mean 1.600, SD 0.624), versus tau 1.35.
These are unsalted scheduling diagnostics, not independent energy samples
or a performance comparison; the logs round BDPT ratios to one decimal.

`ForTest_UseFixedProbeWork` explicitly opts an instance into a single-thread,
owned-Mersenne-backend probe. Two candidate frames use distinct ValueSalts,
reset GlobalRNG and C rand(), and use equal pixel-sample work costs. Thus the
common work cancels and the decision compares variances. It preserves real
wall-time cost instrumentation and restores ValueSalt/GlobalRNG; it does not
restore the C rand() stream. The hook is never enabled by production callers.
It refuses a multithread/non-Mersenne configuration rather than claiming it
is repeatable. No production threshold, scene, estimator or sampler formula
changed. RGB and spectral probes share this body; HWSS is delegated by the
existing spectral probe configuration.

This is a policy test, not a production speed winner prediction. Under its
fixed-work metric GGX takes BDPT (ratio 1.756, margin +0.406) and corridor PT
(0.120, margin -1.230). The GGX assertion is deliberately updated accordingly,
not forced to PT. The transport-reach invariants remain unchanged.

Gates, actual return codes checked:

- Initial fixed-work binary: 20 consecutive FULL runs, 157/0 each, rc0.
- Final binary, with preserved elapsed-time diagnostics and explicit C seeding:
  full 157/0, rc0; 20 consecutive `--dl546-only` runs, 8/0 each, rc0.
  The printed routing ratios are identical across those 20 replays (SD 0).
  Those are n=20 repetitions of the same deterministic input, not n=20
  independent Monte Carlo estimates; each probe uses n=2 distinct salts.
- RasterizerDefaultsConsistencyTest 165/0, FileRasterizerOutputShimTest 67/0,
  SourceHygieneTest 172/0, rc0.
- Named make targets returned zero; compiler warning/error count zero.

Self-review checked disabled-hook production behavior, RNG lifetime/restore,
film restoration, original wall-time counter semantics, spectral sharing and
route/diagnostic consistency. The remaining production time-based routing is
intentional. P2: cross-machine numeric differences or changed scene assets can
still change a deterministic variance winner; this closes scheduling flakiness,
not a permanent semantic golden image. No machine speed claim is made.
