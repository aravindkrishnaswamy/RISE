# Engineering Skills

Process skills distilled from hard-won RISE development sessions. Each
file captures a discipline that we repeatedly wanted to follow after
the fact but forgot to apply up front. The intent is for any
LLM-powered agent or human contributor working on the repo to read the
relevant skill BEFORE starting a task of the matching kind, not to
re-derive the lessons from first principles every time.

## How To Use

- Read the skill whose trigger matches the task you are about to do.
- Follow the procedure; prefer it over ad-hoc judgment on the covered
  points.  The lessons are here precisely because ad-hoc judgment
  reliably misses them.
- If a new lesson is hard-earned during a session, add a new skill
  here rather than burying it in commit messages.

## Format

Each skill is a plain Markdown file with YAML frontmatter (`name`,
`description`).  The frontmatter is consumable by tooling that wants
auto-discovery (e.g. Claude Code loads `name`/`description` into the
session's available-skills list); the body is ordinary prose readable
by any model or human.

Standard body layout:

- **When to use / When NOT to use** — triggering conditions.
- **Procedure** — numbered steps.
- **Anti-patterns** — shapes of wrong that look right.
- **Concrete example** — a real case from this repo.
- **Stop rule** — how to know when the skill's work is done.

## Claude Code Integration

Each skill has a companion shim at
`.claude/skills/<name>/SKILL.md` whose only job is to register the
trigger with the Claude Code harness and point at the full content
here.  The source of truth is this directory; the shim stays thin.

Other LLM tools (Cursor, Aider, Codex CLI, etc.) should be configured
to read `docs/skills/` at session start via their own equivalent
mechanism, or follow references from [AGENTS.md](../../AGENTS.md).

## Available Skills

| Skill | Trigger |
|---|---|
| [adversarial-code-review](adversarial-code-review.md) | Validate a non-trivial change; user asks for multiple / adversarial reviewers. |
| [performance-work-with-baselines](performance-work-with-baselines.md) | Optimize runtime or memory; any change framed as "make X faster." |
| [abi-preserving-api-evolution](abi-preserving-api-evolution.md) | Change a public API — exported function, virtual interface, or abstract base class. |
| [const-correctness-over-escape-hatches](const-correctness-over-escape-hatches.md) | Tempted to add `mutable` / `const_cast` / drop a `const` — stop and apply this decision tree first. |
| [sms-firefly-diagnosis](sms-firefly-diagnosis.md) | Bright outlier pixels in an SMS render; "fireflies" reported; a new SMS scene differs from its VCM reference by bright speckles. |
| [write-highly-effective-tests](write-highly-effective-tests.md) | Add or strengthen tests; convert smoke tests into real regression guards; decide whether coverage belongs in `tests/`, `scenes/Tests`, or `tools/`. |

## Authoring New Skills

A new skill is worth writing when:

1. You caught yourself repeating a discipline across two or more
   sessions.
2. The discipline has clear trigger conditions and a concrete
   procedure — not just "be careful."
3. You can cite a real example where skipping the discipline caused
   rework.

Copy the structure of an existing skill, keep the body under ~300
lines, and add the trigger row to the table above.  Add the
companion `.claude/skills/<name>/SKILL.md` shim so Claude Code can
auto-discover the new skill.
