# r231 promotion receipt

Promote `1b42d7de`: exact four-word reuse of temperature-only compensated
divisions in the EOS species loop. Two fresh independent reviewers report
zero P1/P2 in arithmetic/branch/lifetime and test/cost/evidence review.
The latter independently recomputed six process statistics and checked input,
executable and evidence identities. No findings required a code repair.

Measured hot-input outer wall **5.827083 → 5.490994 s/step**, **5.77%**
lower; device **3.939362 → 3.605724 s**. EOS accounts for the saving:
**2.479214 → 2.144314 device s**, with target assembly unchanged near
0.850 s. Three eight-step processes per variant; this is not full-window
performance and does not meet the one-second target.

The native owner gate, eight equal-time persistent payload comparisons
(excluding only producer build identity), native cache regression and clean
make/GUI Deployment/GUI Opto builds pass. The independent direct EOS
evaluator remains unchanged. GUI builds are not executed Opto solver tests.
Exact hashes and scope are in [the r231 entry](FIRE_SMOKE_EOS_BASIS_R231.md).

Preserved measured executable SHA256:
`510fc680514757dd5d8be3f60ef920fe34fb3dd5a681217497fd5731ec1916ea`.
Production with this kernel starts from zero. No migration, tolerance change,
fixed-k shortcut, or full-window equivalence is authorized by these tests.
No production run is active; cost work continues toward the window budget.
