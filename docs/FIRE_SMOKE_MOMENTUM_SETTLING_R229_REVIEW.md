# r229 review ledger

First round on `af0f787b`: numerical/residency reviewer no P1/P2;
test reviewer one P2 (consecutive terminal records could manufacture a zero
delta). Fixed in `f39560e6` with a direct pre-binding rejection test, a
guard-removal mutant, and a positive failed-terminal continuation fixture.

Final round on `f39560e6`: a new numerical/serialization reviewer and an
independent evidence reviewer who had not previously reviewed r229 both
returned no P1/P2. The evidence reviewer reproduced the hot deltas, SHA
records and eight CPU tests. The review service refused a further new agent
at its thread limit; the second reviewer was the prior r228 cost reviewer,
reviewing r229 for the first time, not rechecking their own earlier verdict.

All findings are closed. One disclosed coverage opportunity remains: the
synthetic failed-terminal positive fixture verifies ordering but does not
vary the appended dynamics, so it is not an independent numeric-delta test
of that exact transition. Existing changing-value tests cover ordinary
Picard deltas. No new-field all-domain envelope or fixed-k qualification is
claimed. Full clean platform builds remain separate outstanding checks.

The actual hot measurement rules out claiming the hoped-for k=2–3 from
this state. It supports investigation of EOS cost, not a fabricated fixed-k
speedup. An existing EOS iteration histogram on this same isolated hot
input is the next measurement; its diagnostic overhead must be excluded
from production cost comparisons.
