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

- **Date (UTC):** 2026-05-01
- **Commit at audit time:** (none — initial seed before first agent run)

## Accumulated lessons (most recent first)

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

- The `INTEGRATOR_REFACTOR_PLAN` Phase 2b/2c/3/4 — check whether any
  shipped during the week.
- The `CAMERAS_ROADMAP` Phase 2+ — output-format cameras, ODS,
  realistic-lens. Check if any landed; the named-camera infra may
  unblock further work.
- Glance at the OIDN doc — heavy backlog there with stable IDs;
  worth checking if any IDs got marked done in the doc but the
  code didn't follow (or vice versa).
- New `docs/_audit_memory.md` itself — should it be in
  `docs/README.md`'s index, or stay as an internal artifact? Either
  is defensible; pick a position and stop drifting.
