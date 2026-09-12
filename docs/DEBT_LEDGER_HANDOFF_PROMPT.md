# RISE correctness-debt cleanup — supervisor prompt

Copy everything below the line into a fresh agent session (any harness, any capable model) started
in the RISE checkout. It is self-contained: the work list lives in the repo.

---

You are the supervising agent for a correctness-debt cleanup in the RISE renderer
(`/Users/aravind/Working/GitHub/RISE`, branch `master`). The work list is
`docs/DEBT_LEDGER.md`: 36 verified-open debts, rows `DL-01` … `DL-37` (`DL-35` was reclassified,
so that id is deliberately absent). Each row has a one-line claim, file:symbol evidence, size
(S/M/L), class, visibility, and a "Verification recipes" entry that says exactly how to know it is
fixed. Work them **one at a time**, in the table's order (physics-bias / energy-loss → precision →
API/bridge gap → coverage/test gap → perf; S before M before L), and stop after each one with a
report. Do not start the next row until the current one is on `master`.

## Read first (in this order, before touching anything)

1. `CLAUDE.md` and `AGENTS.md` (repo rules; the "Change Checklist" for source-file adds/removes
   across all five build projects; "Compiler Warnings Are Bugs").
2. `docs/skills/implementation-review-loop.md` — the definition of done: no change is done until a
   fresh round of independent reviewers returns zero P1.
3. `docs/skills/audit-by-bug-pattern.md`, `docs/skills/write-highly-effective-tests.md`,
   `docs/skills/variance-measurement.md`, `docs/skills/bdpt-vcm-mis-balance.md` (its step 0 lists
   nine non-MIS causes of "integrators disagree", including "PT may be the broken reference").
4. `docs/DEBT_LEDGER.md` in full; then, for the row you are about to take, its source-document
   section (the "source doc:item" column) and its verification recipe.

## Per-row procedure

1. **Branch in an isolated worktree** off the current `master` HEAD (record the hash):
   `git worktree add .claude/worktrees/<DL-xx> -b debt-<DL-xx> master`, then
   `ln -sf Config.OSX .claude/worktrees/<DL-xx>/build/make/rise/Config.specific`,
   `mkdir -p .claude/worktrees/<DL-xx>/bin/tests .claude/worktrees/<DL-xx>/bin/tools`, and build with
   `make -C build/make/rise -j8 all` (zero warnings required; `make all` does NOT relink tests).
   All work happens inside the worktree with relative paths. Never edit, build, `reset --hard`,
   `checkout --`, or stash in the shared checkout; never use bare `git stash` anywhere (the stash
   stack is shared); never push.
2. **Re-verify the row before fixing.** Open the cited symbol and confirm the defect is still
   present on this HEAD. If it is not, strike the row with evidence and move on — do not fix a
   non-bug.
3. **Red-prove first.** Write the test the recipe names (extend the named file or create the named
   new one; tests auto-discover via the Makefile wildcard, so only `tests/README.md` needs a row).
   Commit the test, build it (`make -C build/make/rise build-test/<Name>`), run it
   (`./bin/tests/<Name>`) against the UNFIXED library, and paste the failing lines into the fix
   commit message. A test that is green before the fix is not a red-proof: say so and find one that
   is, or label the row honestly as a consistency pin.
4. **Fix at the right layer**, then run the audit-by-bug-pattern sweep: state the bug pattern in one
   sentence, enumerate siblings (RGB/NM twins, Pel/spectral/HWSS rasterizers, PT/BDPT/VCM/MLT walks,
   snell/uniform SMS modes, coated/composite/fabric wrappers), confirm or refute each, and fix every
   sibling in the same slice. Grep for the CONSUMED field, not the producer call: the debt-30 brief
   missed a consumer because it grepped for `.Scatter(` instead of `kray`. Sweep one field further
   than the symptom (debt 31's leak hid behind a correctly fixed `kray` loop in `delete_stack`).
5. **Gate by touched class**, never the full suite: grep `tests/` for every class you changed and
   run each of those suites individually, plus the recipe's test. Long suites
   (`SignalIntegratorConsistencyTest`, `EnvLightBalanceTest`) run under `nohup` into a log and are
   polled. Never run `make tests` or `run_all_tests.sh`. Paste every counter line verbatim into the
   report.
6. **Measurement hygiene** for any render-based number: `oidn_denoise FALSE` and `pixel_filter box`
   on every rasterizer string; EXR output with `color_space Rec709RGB_Linear`; output patterns
   relative to `RISE_MEDIA_PATH` (`export RISE_MEDIA_PATH="$PWD/"`); renders seed from
   unsynchronised libc `rand()`, so repeats are not bit-reproducible — tests that average repeats
   call `std::srand(seedBase + renderIndex)` per render. Recompute every number you write into a
   doc or comment from your own run; never copy a reviewer's or a brief's figure.
7. **Review to zero P1.** Spawn 2–4 orthogonal read-only reviewers in parallel on the worktree:
   the strongest model available for the transport/MIS reviewer whenever the row touches an
   integrator, BSDF, pdf or IOR stack; a tests/red-proof-honesty reviewer; a documentation-truth
   reviewer (every number, count, path, symbol, hash and line reference must be true of the tree).
   Fix every P1 (and cheap P2s), commit, then spawn FRESH reviewers on the new state. Repeat until a
   round on the current state returns no P1. Reviewers are read-only; fix workers never review
   their own work. Expect doc-truth rounds to outnumber code rounds; that is normal here.
8. **Close the ledger row** in the same branch: strike the `DL-xx` row in `docs/DEBT_LEDGER.md` and
   the heading in its source document (`~~…~~` + "CLOSED <date> — <evidence: test name and counter,
   commit hash>"), update `tests/README.md`, update the ledger's Counts section, and add any new
   residual you discovered as a new row (with evidence) rather than leaving it in a commit message.
9. **Merge**: `git merge --no-ff debt-<DL-xx>` into `master`, rebuild `master`, rerun the gate
   suites on `master`, then `git worktree remove` (keep the branch). Never push.
10. **Report** (stand-alone, for a reader who did not watch): the row, root cause in two sentences,
    per-file status table with commit hashes, red-proof output, gate counters verbatim, review
    rounds and what each found, residuals opened, and anything in the ledger row you found to be
    wrong, with evidence. Then stop and wait for the go-ahead on the next row.

## Scope rules

- **L-sized rows (`DL-05`, `DL-06`, `DL-07`, `DL-08`, `DL-24`) need a short design note before
  code** (`docs/<TOPIC>.md`: mechanism, options, chosen fix, red-proof, gate) and an explicit
  go-ahead. `DL-06` has four recorded failed attempts (the env-IBL entry in `CLAUDE.md` and
  `docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md`); do not re-attempt the surgical two-function
  SA-MIS edit.
- `DL-04` (subsurface η² direction) is a physics decision: settle it with the ledger's
  two-material observable rendered under a submerged camera and an air camera before changing any
  convention.
- Rows in the ledger's "Not-a-debt" section are out of scope; rows the source doc marks
  "observed-need gated" stay closed until the need is observed.
- Do not widen a row into neighbouring refactors. A new defect found mid-row is fixed in the same
  slice only when it is the same bug pattern; otherwise it becomes a new ledger row.
- Commit trailers name the model that authored the commit
  (`Co-Authored-By: <model name> <noreply@anthropic.com>`); if you delegate to cheaper worker
  models, the trailer names the worker's tier, not yours.

## Hard constraints (repeat these in every worker brief you write)

- Worktree isolation with relative paths; confirm `git rev-parse --show-toplevel` before any build
  or edit; the supervisor verifies the shared checkout is untouched (`git status`, HEAD) after every
  worker returns.
- Never `make tests`; per-test `build-test/<Name>`; never the full suite unless the user asks.
- Zero compiler warnings; no pragmas; no `-Wno-*`.
- Every red-proof runs against the unfixed library in the worker's own worktree.
- Numbers in docs are recomputed, not copied; doc lists state per-file STATUS, not quotes of other
  files.
- Never push. Never `reset --hard` or `checkout --` a dirty file. Never bare `git stash`.
