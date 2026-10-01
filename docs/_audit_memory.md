# Documentation-Audit Memory

Auto-maintained by the weekly documentation drift audit
(see GitHub issues labelled `documentation`).

The audit agent reads this file at the start of each run and appends
to it at the end. **Treat the latest entry's lessons as additions to
the audit prompt** — the agent should incorporate them when scoping
the next run.

Manual edits are allowed: prune obsolete lessons, fix wrong claims,
reword for clarity. Just don't delete the **Last audit** line below
or the agent will lose its anchor.

---

## Last audit

- **Date (UTC):** 2026-09-28
- **Commit at audit time:** `75f78ba512aab16a218eb81caee123d9c9779ac1`
- **Canonical report:** GitHub issue #28 (https://github.com/aravindkrishnaswamy/RISE/issues/28)

## Accumulated lessons (most recent first)

### 2026-09-28 — index gaps from DL-batch + gui/agentic-redesign cluster growth

- **Lessons for future audits:**
  - The anchor commit in the previous memory entry (`1514297d`) was not a valid object — `git cat-file -t` is the right first check; if invalid, fall back to the full repo state and note it in the report.
  - **Three new canonical docs were orphaned from the README index after the Sep-12 DL batch:** `REFRACTIVE_RADIANCE_SCALING.md` (DL-30), `SUBMERGED_CAMERA_IOR_SEEDING.md`, and `SPECTRAL_ILLUMINANT_CONVENTION.md`. Any time CLAUDE.md High-Value Facts gains a new `docs/` citation, cross-check that docs/README.md also indexes it — CLAUDE.md is updated more diligently than the README.
  - **Subdir growth in `docs/gui/` and `docs/agentic-redesign/` is now the dominant index-completeness gap.** The README enumerates gui/ files individually (so new ones look like omissions), while SMS/pre-phase-1 use a cluster sentence (so growth there is intentional). Run `ls docs/gui/*.md | wc -l` and compare to the README's gui/ reference count each week.
  - **The INTEGRATOR_REFACTOR_STATUS "Branch state: untracked" is a known stale line** — flagged this run; if still present next run, skip and note as already-filed.
  - The "all 16 DL closures" claim in docs/README.md was accurate for the 16-doc batch but misses DL-30 (η²) which also landed 2026-09-12. The corrected count is 17 total DL records; docs/README.md:145 needs a "+1" update.
- **Findings count:** 5 in Group A, 1 in B, 1 in C, 1 in D.
- **Issue link:** https://github.com/aravindkrishnaswamy/RISE/issues/28

### 2026-07-24 — repository-wide audit

- **Treat lifecycle as first-class metadata.** Proposed, partially
  implemented, shipped, superseded, and historical plans must not be mixed in
  an undifferentiated index.
- **The CST cutover is a high-drift surface.** Parser paths, extension
  instructions, and descriptions of macros and loops must be checked against
  the current loader and registry.
- **Generate test inventories.** Count `tests/*.cpp` from the tree instead of
  maintaining a numbered catalog.
- **Separate active and historical link debt.** Broken links in active
  references should be fixed immediately. Deleted prototypes referenced by
  historical experiment logs are a consolidation signal.

### 2026-05-01 — initial seed (manual)

These are the lessons from the human-driven first-pass audit that
established the pattern. Treat them as the baseline scope.

- **The four-group framing works.** Group A (mechanical drift) is
  always the bulk; Group B (stale plan headers) is rare but
  high-impact when found; Group C (doc gaps) needs a high bar — only
  flag when a contributor would genuinely miss something; Group D
  (removals) should default to "leave but mark completed" rather
  than delete.
- **High-drift surfaces:** `src/Library/Parsers/README.md` chunk
  family table, `tests/README.md` test inventory, and `scenes/Tests/README.md`
  subdirectory list all auto-drift as features land. Always recount
  these against the live source.
- **Plan-doc headers go stale silently.** Any doc whose top says
  `Status: Draft` or `Status: Research-only` while the body has
  `*(LANDED)*` markers below is a red flag. Check the status line
  against the latest section markers.
- **`AGENTS.md` and `CLAUDE.md` are usually accurate** — they're
  load-bearing for agents and tend to be kept current. Spot-check a
  few claims (file paths, defaults, API names) but don't budget
  much time here.
- **Resist documenting trivial subsystems.** `src/Library/Modifiers/`,
  `src/Library/Animation/`, `src/Library/Functions/` are small enough
  that the code is the doc. Only recommend a new doc if there's a
  load-bearing invariant the code can't communicate alone.
- **Cite `file:line` for every drift item.** Saying "the chunk count
  is wrong" without a line reference makes the report impossible to
  act on quickly.
- **Cross-check forward-references in newly-written docs.** The
  initial pass added a forward-reference to `MATERIALS.md` from
  `docs/README.md` before `MATERIALS.md` existed; that's fine when
  done in the same change but worth double-checking each run.

### Watchlist for next audit

- **`docs/README.md` index gaps (already filed in #28):** REFRACTIVE_RADIANCE_SCALING.md, SUBMERGED_CAMERA_IOR_SEEDING.md, SPECTRAL_ILLUMINANT_CONVENTION.md, ~14 new docs/gui/ files, ~33 new docs/agentic-redesign/ files. Check whether these have been added before re-filing.
- **`docs/README.md:145` DL count:** says "all 16" but DL-30 makes 17. Check if corrected.
- **`docs/INTEGRATOR_REFACTOR_STATUS.md:6` "untracked (uncommitted)" line** — still stale; check if updated.
- The `CAMERAS_ROADMAP` Phase 2+ — output-format cameras, ODS, realistic-lens. Check if any landed.
- Glance at the OIDN doc — heavy backlog there with stable IDs; worth checking if any IDs got marked done in the doc but the code didn't follow (or vice versa).
- `docs/gui/` is now 23 files with only 9 indexed — track whether the README expands its coverage or consolidates to a cluster sentence.
