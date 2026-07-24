# Documentation Audit — 2026-07-24

This report records a repository-wide audit of RISE's first-party
documentation at commit `1514297d`. It is both a snapshot of the work
performed and a removal/consolidation backlog for documentation that no longer
belongs in the active reference set.

No documentation was deleted during this audit. Design records in this
repository are frequently cited from source comments and tests, so deletion is
only safe after those citations and any still-current invariants have been
migrated.

## Scope and method

The pre-audit inventory contained 142 tracked first-party documentation files:

- 4 root documents
- 114 files under `docs/`
- 24 subsystem, test, scene, tool, and asset README files

The count excludes vendored documentation under `extlib/`, third-party license
files, generated logs and render output, and ignored local experiment notes.

The audit:

1. inventoried every tracked first-party Markdown/text document;
2. compared commands, paths, architecture claims, feature status, tests, and
   scene indexes against the current tree and implementation;
3. checked repository-relative Markdown links;
4. added lifecycle labels to plans whose implementation status had changed;
5. separated active references from historical records and removal candidates.

## Corrections made

### Canonical architecture and workflow

- Replaced stale references to the removed `AsciiSceneParser.cpp` with the
  current CST loader and `ChunkParserRegistry.cpp` entry points.
- Documented the v7/CST scene-language cutover and identified legacy
  macro/loop syntax as migration history rather than current syntax.
- Corrected the parser-extension instructions in the agent and parser guides.
- Marked `README.txt` as the intentionally frozen May 2006 user manual.
- Rebuilt `docs/README.md` as a lifecycle-oriented index instead of presenting
  every plan as equally current.

### Tests and sample scenes

- Replaced the manually maintained 77-test inventory with the current 219
  standalone test executables and source-of-truth discovery commands.
- Corrected scene-directory indexes, adding current `Camera`, `Lighting`, and
  `Lights` coverage and removing nonexistent directories.
- Added missing feature-scene entries for EnamelWatch, GuillocheWatch,
  Materials, and SDF.
- Corrected the current BSSRDF output color-space description.

### Feature-status drift

Lifecycle/status headers were corrected for the following major documents:

- auto rasterizer: shipped;
- FrameStore/HDR/AOV redesign: substantially implemented rather than
  Phase-1-only;
- glTF importer and physically based pipeline: broad core shipped with
  explicit deferred extensions;
- color-space migration stages A and B: complete;
- scene variants: shipped;
- agent chat compaction: shipped;
- CST/save cutover execution plans: completed; broader immutable-scene design:
  partial;
- dynamic UI and agentic product facets: core surfaces shipped, advanced
  snapshot/orchestration/security work partial;
- interactive editor plan: completed/superseded by the CST implementation;
- GUI roadmap and approachability work: partially implemented;
- desktop redesign brief, environment editor, and editor-state hardening:
  executed;
- cameras/views and render-mode milestones: mostly or fully implemented;
- entity creation, material editing, AI security, and transaction work:
  partially implemented;
- LLM runtime, MCP tool surface, and validation core: shipped with remaining
  hardening work;
- guilloche and unified-integrator studies: completed historical studies;
- SMS uniform-seeding/two-stage plans: completed execution records;
- vitreous-enamel plan: hero implementation shipped, broader roadmap partial.

### Link hygiene

The initial relative-link scan found 278 invalid targets. Most were mechanically
stale `file.cpp:line` link targets or paths moved during the parser cutover.
Those links were normalized or retargeted.

The residual targets referred to deleted experiment scripts, reverted parser
prototypes, removed tests, or pre-consolidation implementation files. Those
were converted from dead links to plain historical path names. The final
repository-relative link scan reports no missing first-party targets.

## Current documentation classification

### Active reference material

Keep current and treat as authoritative:

- `README.md`, `AGENTS.md`, `CLAUDE.md`, and `docs/README.md`;
- architecture and workflow references such as `ARCHITECTURE.md`,
  `SCENE_CONVENTIONS.md`, `PERFORMANCE.md`, `MATERIALS.md`,
  `RENDERING_INTEGRATORS.md`, and `SPECTRAL_RENDERING.md`;
- current subsystem READMEs under `src/`, `tests/`, `scenes/`, and `tools/`;
- process skills under `docs/skills/`;
- GUI and agentic specifications that still contain unimplemented milestones.

### Intentionally historical

Retain, but do not present as current implementation guidance:

- `README.txt`, the frozen 2006 manual;
- completed design and retrospective documents whose decisions are still
  cited by code or tests;
- dated audits and investigation logs that explain a shipped architecture or
  a rejected approach.

Historical documents should carry an explicit status/date banner and point to
the current reference when one exists.

## Removal candidates

### High-confidence candidates

These files appear to have completed their one-time purpose and have no known
live source/test citations. Review once for unique operational knowledge, then
delete or move their useful residue into a retrospective:

| Candidate | Why it is no longer an active reference | Gate before removal |
|---|---|---|
| `docs/CONSOLIDATION_MANUAL_GUI_CHECKLIST.md` | One-time consolidation verification checklist | Confirm no outstanding unchecked item still maps to a release requirement |
| `docs/CONSOLIDATION_WINDOWS_PASS.md` | Dated Windows consolidation pass record | Preserve any still-relevant platform command in the current build docs |
| `docs/_audit_memory.md` | Seed state for a weekly audit workflow that has not been maintained since May 2026 | Confirm no external automation consumes the file; this report supersedes it |
| `docs/PRE_PHASE1_OPTION_C_DESIGN.md` | Records an abandoned/reverted PRE experiment and is not cited by current source/tests | Extract any unique stop condition into the current PRE/VCM retrospective and migrate the historical link in `CLAUDE.md` |

### Consolidate first

These clusters are stale or overly fragmented, but deletion now would destroy
useful rationale or break live citations:

| Cluster | Recommended destination | Why deletion must wait |
|---|---|---|
| `PRE_PHASE1_STATUS.md` and related PRE/VCM experiment notes | A compact PRE/VCM retrospective plus current architecture notes | The status log is very large, contains unique experiment evidence, and is cited from source |
| Guilloche plan/results documents | Feature-scene README or one completed-study retrospective | Results are useful, but the plan/result split no longer helps day-to-day navigation |
| Unified-integrator analysis, baselines, decision, and auto-rasterizer design | Keep the current auto-rasterizer reference and one compressed decision record | Some decision documents are cited by implementation comments |
| Physical-light-transport landing plans 1–3 | One shipped-architecture retrospective | Several source comments still cite individual phases |
| SMS uniform/seeding/two-stage solver fragments | `SMS.md` plus one dated solver retrospective | Multiple implementation comments cite the individual plans |
| Agentic execution plans 61 and 62 | Current runtime/tool-surface docs plus a short rollout record | A few live implementation citations remain |
| `docs/gui/CURRENT_STATE_AUDIT.md` | A generated/current GUI capability map | It is already historical, but other GUI plans still use it as their baseline |

### Do not delete yet

- `docs/ROUND_TRIP_SAVE_PLAN.md`: many live source citations target its
  invariants and section anchors.
- BVH and displaced-subdivision plans: they explain shipped architecture and
  remain cited by implementation code.
- `README.txt`: intentionally retained project history, now clearly labelled.
- GUI/agentic plans with remaining unshipped milestones: their status labels
  now distinguish implemented work from backlog.

## Safe cleanup sequence

For each removal or consolidation:

1. identify every source, test, and documentation citation;
2. extract still-current invariants into an active architecture/reference
   document;
3. migrate citations to that stable destination;
4. preserve material experiment results in one retrospective when they affect
   future engineering choices;
5. rerun the relative-link audit;
6. only then delete the superseded files.

This order prevents documentation cleanup from erasing the reasoning behind
load-bearing renderer and scene-language decisions.

## Ongoing maintenance recommendations

- Generate test counts and inventories from the tree; do not hand-maintain a
  numbered executable list.
- Require every execution plan to declare one of: proposed, active, partially
  implemented, shipped, superseded, or historical.
- When code cites a design document, cite a stable architecture section rather
  than a temporary phase plan whenever possible.
- Run a repository-relative link check when files are moved or removed.
- Update `docs/README.md` when a document changes lifecycle, not merely when a
  new document is added.
