# Creative-Richness Arc — Session Log (2026-07-29 → 2026-07-30)

> **Status:** arc CLOSED by its own pre-committed stop rule. 11 commits on
> `master` (`10b71058`..`c940f4bb`), gate green throughout (`make all` zero
> warnings, `./run_all_tests.sh` 225/225, `xcodebuild RISE-GUI` clean).
> Design + phase outcomes live in
> [73-creative-richness-design.md](73-creative-richness-design.md) (§6–§10
> are the per-phase measurement records); this log is the process journal —
> what was measured, what was got wrong, and what it teaches — in the
> tradition of [72-agent-turn-efficiency-and-quality-log.md](72-agent-turn-efficiency-and-quality-log.md).
>
> **One-sentence verdict:** on bare build prompts, example-anchoring lifted
> painter richness (floor 1→3) and no form of advice — note or diagnostic,
> render-carried or validate-carried, once or thirty times — ever moved
> mechanism adoption (scalar pipe 0/24 lifetime): models act on facts that
> BLOCK; they habituate to advice.

---

## 1. How this started

The 72-log ended on a live design question: a heavily-structured prompt
naming material intents produced six procedural painter families; the bare
prompt real users write ("build me a courtyard") produced flat uniformcolor.
The user asked for a settled design before any code: more creative by
default, or elicit intent, or both — and, non-negotiably, a way to MEASURE
whether any change held, because every guidance change in the prior arc had
been validated by a single manual run.

The answer designed and executed: creative-by-default; asking reserved for
material ambiguity (the already-shipped `ask_user` description carries that
line); the stated plan is the elicitation; and a measurement-first ladder —
P0 instrument, P1 example-anchoring, P2 observed-state advisory — with the
eval deciding each rung. Mid-P0 the user widened scope from materials to
general creativity including geometry, and later narrowed the instrument to
gemini-only (cost), then added a local-thinking-model probe (evidence
quality). Both interventions materially improved the arc.

## 2. The ladder, with measured verdicts

Instrument: `bare_prompt_build_courtyard` ("Build me a courtyard." against
the empty starter, 11 checkpoints) + `build_ambiguous_scene` as the
ask-side counterweight in every batch; gemini-3.5-flash throughout, plus
qwen3.6:27b where thinking evidence was wanted; N=3 per phase, serialized.

| phase | treatment | painter kinds (floor/typ.) | scalar pipe | advanced geo | verdict |
|---|---|---|---|---|---|
| baseline (3 passes, 9 runs) | none | 1 / 2 | 0/9 | 2/9 | premise PARTIALLY STALE — richness already median-2 |
| P1 `526f6f99` | skill snippets re-anchored | 2 / ~2.7 | 0/3 | 0/3 | examples move copying; mechanisms unmoved |
| P2 `9b3f19a6` | DESIGN NOTE on render+validate | 3 / 3 | 0/3 | natural | fired, seen, ignored |
| qwen probe | same, thinking recorded | — | 0/3 | — | dismissed IN WRITING |
| P2.b `af764f67` | Info diagnostics in validate | 2 / 2 | 0/3 | 0/3 | gemini: exposed+ignored; qwen: mostly unexposed |

Ask-side guard: 2/3–3/3 asked in every phase — the epoch-11 form never
moved. The arc's central tension (richer defaults vs unwanted questions)
closed with zero unwanted questions introduced.

**The two written confessions** (qwen3.6 `message.reasoning`, recorded in
trajectory llm records — the probe's entire value):

> r1: "The full candidate validated clean - no errors. The 'note' about
> scalar_painter is just a design suggestion, not an error."

> r3: "…no errors or warnings beyond the **expected** design note about
> scalar textures (which is a feature suggestion, not an issue)."

"Expected", after its 16th firing, is habituation named by the subject.
r1 saw the note 30 times. Repetition converts advice into wallpaper.

## 3. What shipped (all zero-P1 reviewed, all measured)

- **P0** `10b71058` — four `document` checker ops over the existing
  `Cst::BuildReferenceGraph` (`distinct_chunk_kinds`,
  `any_param_references_kind`, `no_orphan_chunks`,
  `objects_reaching_kinds`), `metricValue`+`metricLabel` per-label
  aggregation in `eval_report.py`, the scenario + 30-line fixture +
  runconfig. Then `77261d11` (gemini-only), `1594fba0` (baseline anchor +
  two calibration fixes the baseline forced).
- **P1** `526f6f99` — seven distinct painter families across the four
  previously-uniformcolor-only skills, two scalar-pipe worked examples,
  C1's one sentence; every changed snippet validated by
  parse+derive+render against `bin/rise` before review sign-off.
- **P2** `9b3f19a6` — the design-note advisory: shared condition scan in
  `doRenderWork`'s tail under the render's own park (a first draft
  self-deadlocked; a second could describe a never-rendered document —
  both designs deleted for the tail relocation), `note` field on beauty
  renders + validate, transport-verified to reach the model on headless,
  Mac GUI async fold, and MCP.
- **P2.b** `af764f67` — the same two facts as Info-severity validate
  diagnostics (`DESIGN_SCALAR_PIPE_UNUSED`, `DESIGN_NO_ADVANCED_GEOMETRY`),
  "clean" redefined as no-errors/warnings in both consumers (load-bearing:
  several committed furnished-scene fixtures trip the scalar advisory).
- **Passing fix** `73e71f26` — `iridescent_painter` per-param descriptions
  contradicted the chunk description; code says colora=grazing,
  colorb=normal; both param descriptions now carry the selecting condition.
- Arc records: `c2e7d622`, `391e8746`, `93bcbbd6`, `c940f4bb` (doc §6–§10).

## 4. Mistakes made, and what they teach

### 4.1 Worktree isolation does not mean what my briefs said it means

Two distinct failure modes, three incidents:

- **`isolation:"worktree"` cuts from HEAD and does NOT carry uncommitted
  changes.** Round-1 reviewers received briefs claiming their worktree
  contained the work under review; all three found clean trees and
  (correctly) refused to fabricate a review. The fix became the arc's
  standard protocol: bundle `git diff` output + a tar of untracked files
  into the scratchpad; the reviewer's brief opens with a provisioning
  block (`checkout --detach <sha>; git apply; tar -xf`) and a
  toplevel-verification command.
- **A resumed worktree agent's cwd silently falls back to the shared
  checkout** when its worktree was auto-cleaned between turns. One
  reviewer detached the main checkout's HEAD (recovered, no content
  change); another ran `tar -xf` INTO the main checkout — harmless only
  because the bundle mirrored files already present, and it would have
  clobbered a post-bundle edit if the timing had differed by one step.
  Rule adopted (and saved to agent memory): a resumed worktree agent runs
  in an UNTRUSTED cwd; briefs must demand re-verification per Bash call;
  the supervisor re-verifies the main tree (`git status` + HEAD + recent
  edits intact) after ANY worktree agent that touched git.

### 4.2 The yield-on-background-build failure mode survived explicit prose

Four occurrences across three different workers, each of whose briefs said
"foreground builds only — never background and yield" (and later versions
named the failure mode explicitly). The prose did not prevent it once.
The working mitigation is structural-by-supervision: the supervisor gates
every build personally and answers each yield with a resume message that
demands foreground completion and an end-on-report. This is the arc's own
thesis applied to its own tooling — instructions ≈ 50%, even for agents
briefed about that exact number.

### 4.3 The supervisor's draft text is not exempt from truth review

Three truth defects in advisory text across the arc, and the worst one was
mine: my P2 fix brief supplied replacement wording naming three materials
and the parameter names `alphax/alphay` — false for two of the three
(`pbr_metallic_roughness_material` exposes `roughness`; cooktorrance's slot
differs; worse, both are baked `ValueKind::Double`, so the scalar pipe
cannot vary them AT ALL — only ggx/ward's slots are Reference-kind). A
round-3 fresh reviewer caught it. Earlier, round 2 caught "all N materials
use constant roughness" as false on roughness-less materials. Lesson: every
sentence a model will read gets the same adversarial truth pass as code,
INCLUDING sentences the supervisor wrote into a fix brief; fresh-reviewer
rounds after fixes are what caught the supervisor-introduced defect.

> **⚠ CORRECTION (2026-07-31, arc-75 pre-design forensics):** the round-3
> reviewer's "fix" was itself wrong, and nobody truth-reviewed the
> reviewer. `pbr_metallic_roughness_material.roughness`/`.metallic` and
> `cooktorrance_material.facets` are **Reference-kind painter slots**
> (ChunkParserRegistry.cpp:3958-3959, cooktorrance facets descriptor) and
> have been since the glTF Phase 2/3 work (`64ca16bc`, 2026-04-30) — the
> parser accepts a painter binding on all of them; the only Double-kind
> roughness params in the registry are the three SSS skin materials. The
> "only ggx/ward" claim was false when written. See §5's corrected
> bindability bullet for the true map and the consequence.

### 4.4 The design's own premise was stale before it was measured

The design opened from the 72-log's "bare prompts produce flat uniformcolor
surfaces." The first baseline showed painter diversity was ALREADY median-2
— the prior arc's `procedural-textures` skill was working, and A1's
approved form (count uniformcolor bindings) targeted a solved problem. The
baseline re-targeted P1 (scalar + advanced geometry, not generic painter
conversion) and forced the §7 amendment of P2's content. Measure before
designing behaviour changes; a premise older than the last shipped change
is a hypothesis, not a fact.

### 4.5 The first baseline's first job was, again, calibrating the harness

Three measurement artifacts in one arc, all found by cross-checking
surprising failures against trajectories before believing them: the
`meanLumaMax 0.35` ceiling (indoor calibration; courtyards are outdoor and
legitimately render 0.67–0.82), the `standard_object max:40` ceiling (it
failed the single richest run of all — 50 objects, 7 painter kinds), and
the near-miss of reading budget exhaustion as flat-materials signal (a
round-1 reviewer caught the 4–5× under-budgeting against the furnished
build family before it polluted anything).

### 4.6 A causal story I wanted was refuted by its own clause

P2's r3 had the note fire, then an insert, and the run's only advanced
geometry — I drafted the hopeful reading. The census clause's own firing
record refuted it: condition B never fired in that run (the sdf predated
the note; the insert was benches). The advisory got zero credit it hadn't
earned. Check WHICH clause fired before crediting a mechanism; the note's
structure made that check possible in one grep.

### 4.7 Exposure before verdict

P2.b's qwen runs looked like a second clean negative — until the exposure
check showed r1/r2 NEVER called validate: the diagnostics carrier reduced
reach for a render-spamming agent (10–57 renders vs 0–2 validates per
run). The mechanism verdict rests on gemini (full exposure, zero action);
the qwen runs contribute a different lesson: **carrier choice must follow
the agent's measured tool habits, not our notion of the right moment.**

### 4.8 What the review tiering bought, concretely

- The Opus lock-discipline reviewer repro'd the implementer's deadlock
  narrative from the outside (`sample` on the hung PID), found the
  post-park consistency window the implementer's fix had left, and
  prescribed the strictly-better design (compute in `doRenderWork`'s tail)
  that deleted the branchy discipline entirely.
- A Sonnet reviewer's independent gaming fixtures found the decoy hole
  (all painter diversity on a hidden 0.001-unit box passed every
  checkpoint) that became `objects_reaching_kinds` — which then doubled as
  the advisory's scan.
- A Sonnet reviewer's execution-validation of P1 (extract every changed
  snippet, parse+derive+render each against `bin/rise`) is the right bar
  for skill content: models copy snippets verbatim; a snippet that doesn't
  render is worse than the flat colour it replaced.
- Mutation probes found the one guard that guarded nothing
  (distinctness-vs-count: mutating `distinctKinds.size()` to
  `matches.size()` passed 1481/1481) and proved every other one.

## 5. Domain facts worth not re-deriving

- **The mechanism law (the arc's product):** models act on facts that
  BLOCK (parse errors, derive diagnostics, an empty prerequisite); they
  habituate to advice regardless of framing (note vs Info diagnostic),
  placement (render vs validate), or repetition (30 exposures in one run).
  Ladder of levers, all now measured on this surface: prose ≈ 50% •
  examples move copying (painter floor 1→3) • advice ≈ 0 • structure
  (schemas, gates, blocking facts) moves behavior. The next lever class
  for mechanism adoption is schema-structural — a required parameter, a
  material template with a scalar slot pre-bound — i.e. a product
  authoring-surface change needing its own design arc.
- **Scalar-pipe bindability — CORRECTED 2026-07-31 (the bullet previously
  here was FALSE; see the §4.3 correction note):** all mainstream
  microsurface slots are painter-bindable, and have been since 2026-04-30
  (`64ca16bc`): `pbr_metallic_roughness_material.roughness`/`.metallic`
  ("painter reference or scalar string", auto-promoted; composes GGX α
  internally), `ggx_material`/`ward_anisotropic_material` `alphax`/`alphay`,
  and `cooktorrance_material.facets` are ALL `ValueKind::Reference`. Only
  the three SSS skin materials bake roughness as `Double`. Consequence for
  the arc's verdict: the 0/24 behavioral result stands (arc-75 forensics
  confirmed every roughness/metallic binding in all 24 runs is a numeric
  constant — 0/24 on the CONSTRUCT too), but the P1 worked examples were
  aimed at the highest-friction path (switch material family to ggx + learn
  scalar_painter + function2d) while the lowest-friction path (bind a
  painter to the pbr `roughness` slot models already write `0.5` into every
  run) was never exampled and never measured — the eval's
  `any_param_references_kind: scalar_painter` checkpoint cannot see it.
  The shipped advisory text (AgentSession.cpp `ComputeDesignNoteFromDoc_`)
  steers to ggx/ward only, on the strength of the false claim.
- **`expression_function2d` is the ONE Painter-category chunk without the
  `_painter` suffix** (36 registered painters) — why the checker ops use a
  registry-resolved `category` filter, not a suffix.
- **Closures handed to `RunPreviewRenderParked` / `SubmitAgentRender*`
  execute holding `SceneEditController::mMutex` (non-recursive).**
  Re-entering the controller (e.g. `ReadDocumentSnapshot`) from inside one
  self-deadlocks. The safe pattern for whole-document reads on the render
  path: compute inside `doRenderWork`'s tail on the live document — under
  the park in every controller branch, writer-free when headless.
- **`SetSkillIndex` is called only by the two GUI drivers.** Headless eval
  runs have no skills section in the system prompt; models pull the index
  with a bare `read_skill{}` (+1 turn) and then read ~5 named skills —
  verified in every baseline trajectory. Deliberately NOT fixed mid-arc
  (instrument stability); now safe to add for parity.
- **`AgentDiagnostic::Severity::Warning` is dead** — nothing constructs
  it; the eval checker's clean-ignores-Info branch has no Warning fixture
  (probe-proved inert). Add a fixture when a real Warning first ships.
- **Eval "clean" now means no error/warning-severity diagnostics** in both
  consumers (`CheckDiagnosticsKind`, `ToolOutcomeLine` rule 8); several
  committed furnished-scene fixtures genuinely trip
  `DESIGN_SCALAR_PIPE_UNUSED` and depend on this.
- **`BuildReferenceGraph` semantics** (for future checker/advisory work):
  `dependents` keyed by referenced chunk's NodeId → referrer NodeIds;
  first-wins name shadowing; self-references excluded; unresolved
  references produce no edge; scalar/colour same-name aliasing folds
  conservative extra edges in.
- **`iridescent_painter`: colora = grazing, colorb = normal** —
  `a*(1-x)+b*x` on `x=|dot(view,normal)|+bias`. Descriptor and per-param
  text now agree (`73e71f26`).
- **qwen3.6 thinking lands in trajectory llm records** at
  `choices[0].message.reasoning` via Ollama's OpenAI-compat endpoint — a
  cheap, decisive instrument: one local probe turned "the model ignored
  it" (inference) into "the model wrote down why" (evidence).
- **Carrier reach by model habit (measured):** qwen renders 10–57× and
  validates 0–2× per build; gemini renders 3–5× and validates 1–2×. A
  validate-only carrier is near-invisible to a render-spammer.

## 6. Working practices that earned their keep

- **Patch+tar bundle protocol** for reviewing uncommitted work in
  worktrees (provisioning block at the top of every reviewer brief;
  re-verify toplevel on every Bash call).
- **The supervisor gates every build**; every worker yield is answered
  with a foreground-finish resume that demands end-on-report.
- **One behavioural variable per eval run; one runDir per phase**
  (`bare_prompt_baseline` → `_p1` → `_p2` → `_p2_qwen` → `_p2b` →
  `_p2b_qwen`); the counterweight scenario rides in every batch; archive,
  never delete, superseded rows.
- **Pre-committed decision rules** ("if P2.b fails, STOP") written into
  the doc before the run — made stopping a lookup, not a debate.
- **Fresh reviewers every round; the round after a fix is not optional** —
  it caught the supervisor-authored P1 (§4.3).
- **Trajectory forensics before belief**: which clause fired, what
  exposure occurred, what the reasoning said — three verdicts in this arc
  flipped or sharpened at that step (§4.5–4.7).
- **Skill snippets are validated by execution**, not inspection (§4.8).

## 7. Open items

1. **Headless `SetSkillIndex` parity** — now safe to implement (arc
   closed, instrument no longer needs stability); saves one discovery
   turn per headless run and matches the GUI product surface.
2. **`Severity::Warning` fixture** for `CheckDiagnosticsKind`'s clean
   branch, whenever a Warning-severity diagnostic first exists.
3. **Second bare-prompt subject** (e.g. "build me a cozy study") when
   repeats go 3→5 — the anti-overfit check on the P1 skill edits.
4. **Disclosed anti-gaming residuals**: multi-decoy scenes and geometry
   decoys pass the CST-side checkpoints (scenario `//` comments carry the
   caveats); combinator painters (`blend`/`channel`) can nominally count
   as distinct kinds. The render-side closer (A2 collapsed-painter
   detector) remains designed-but-unbuilt, and less motivated now.
5. **Schema-structural lever class** for mechanism adoption (required
   params, pre-bound templates) — a new arc if ever wanted; the eval
   instrument and decision-rule discipline are ready for it.
6. **Render-cadence gate** (72-log item 1) — still unlanded; weakened as
   a theory by P2's turns-remaining evidence, but untested as a lever.
7. **Qt/Windows half still never compiled here** — it now also carries
   the note field, Info-diagnostics handling, and `ToolOutcomeLine`
   changes textually.
8. **Advisory plumbing stays in tree deliberately** — truthful, tested,
   zero-cost; re-run the same runconfigs when the model roster changes
   and the 0/24 question re-answers itself.
