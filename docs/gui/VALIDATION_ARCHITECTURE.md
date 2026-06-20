# Scene Validation Architecture — Safe `validate(scene_text)` for the Agent Surface

**Status:** DESIGN. No code. Deep-dive spec spun off from [GUI_ROADMAP.md](../GUI_ROADMAP.md) §9 / §15 / §16, owning the corrected design of the keystone `validate` tool. It supersedes the `validate(scene_text)` sketch in [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §6 ("a capturing `ValidationJob` that is side-effect-free AND parser-identical"), which an adversarial review found is **not achievable as specified**, and the [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §5.3 "cannot drift" / "schema = exact parser" claim, which is **too strong**.
**Owner:** Aravind Krishnaswamy
**Scope:** A safe, side-effect-free scene-validation path for the agent/MCP tool surface and the GUI editor's inline diagnostics — what "validation" can honestly mean given the current parser, the command policy that keeps validation from terminating the process or kicking a render, a **two-tier** v1 (a parse-only **IR** path that backs the fast editor gutter, and a deeper isolated-and-**sandboxed** throwaway `Job` that backs the `validate` tool's construction-level checks), a principled end-state (`parse → IR → validate IR → apply IR`), the descriptor-vs-real-schema gap (and the conformance test that keeps a generated schema honest), and the diagnostic format that powers both the MCP `validate` tool and editor squiggles. **Out of scope:** the rest of the tool/resource catalog, transports, provider adapters, the agent loop (those are [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) / [LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md)).

> **Second adversarial review — confirmed resolutions (2026-06-20).** A second pass found three load-bearing gaps; all three are now resolved per a confirmed decision, and the prose below is hardened accordingly. **(1) Validity had two incompatible definitions** (the severity policy "`error` = the parser rejects" collided with the conformance test asserting the schema's *constraint* set matches the parser). **Resolution — legacy-tolerant:** `error` is emitted **only** where the *real, tolerant* parser actually rejects; every richer constraint the parser does **not** enforce (vector cardinality, non-integer-`UInt`, closed-enum membership) is a **`warning`**, never an `error`. Existing scenes keep parsing. The conformance test (§5.4) is rewritten to assert only the schema's **error** set neither over- nor under-accepts vs the parser; richer-constraint **warnings** are explicitly *allowed* to be stricter than the parser and are **not** conformance failures. **(2) v1 fast-mode was undefined** (the gutter needs construction-free validation but the only concrete v1 was the full-construction throwaway `Job`). **Resolution — promote the `parse → IR` seam into phase 1:** the gutter runs IR-level syntax/schema validation (no `Job`, no file I/O — inherently safe and fast); the deep `validate` tool runs the isolated throwaway `Job` for construction-level checks (reference resolution via `Job::ResolveOrDiagnoseScalar`, file resolution, the §1.4 pipe check). Both are v1 (§3), with the IR path as the gutter's backing. **(3) "Isolated" ≠ "sandboxed"** (a throwaway `Job` still resolves attacker-supplied filenames, decodes large assets, probes file existence, and runs process-global code). **Resolution — sandbox the throwaway `Job`** (§3.5): project-root confinement via the **shared `ProjectRootJail`** ([AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4.1), size/time/memory caps, cancellation, and risky construction (asset decode) ideally **out-of-process**. The IR parse-only path needs none of this — it performs no construction.

> **Third (code-backed) review — taxonomy + sandbox honesty (2026-06-20; GUI_ROADMAP §13a #5).** Two refinements on top of the second review. **(A) The single `error` class above is too coarse and mislabels barrier commands.** Second-review point (1) parked `BARRIER_COMMAND` under "`error` = the parser rejects," but the *real parser accepts and executes* a barrier command (`> quit` runs `exit(1)`, §1.2) — it does not reject it. So the blocking outcome is now **two distinct classes**: **`parser_error`** (the parser rejects — the class the §5.4 conformance test pins to the parser) and **`validation_policy_error`** (the parser accepts+runs it but policy refuses it — barrier commands, plus the §3.5 sandbox refusals `PATH_OUTSIDE_PROJECT_ROOT`/`VALIDATION_LIMIT_EXCEEDED`), with `warning`/`info` advisory below them. The full definition is §6.5; the codes are reclassified in §6.2; §5.4 excludes `validation_policy_error` from the parser-equivalence oracle. **(B) The §3.5 "no crash/OOM" guarantee requires *actual* out-of-process construction for the untrusted/agent path** — second-review point (3) said out-of-process asset decode is "ideally" used; for the MCP `validate` tool fed arbitrary text it is **required**, because an in-process codec crash/OOM takes down the host (§3.5 #4, hardened below).

This doc honors the [GUI_ROADMAP.md](../GUI_ROADMAP.md) §1 principles — **#1 text is the source of truth**, **#2 maximize shared C++** (the validator is library code), **#6 everything routes through one mutation path** (validation never becomes a second write path) — and the §16 confirmed decisions: the adopted **`ParameterSemantics` structure on `ParameterDescriptor`** (separate fields, not one overloaded enum), the **`src/Library/Agent/`** subsystem name, and "**avoid the bare `MCP` token**" in type/dir names.

> **Ground-truth basis.** Every code claim cites `file:line` against the tree at the time of writing and was re-derived from source per [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) (which is the authority where any plan-doc `Status:` header disagrees). `.claude/worktrees/` was ignored.

---

## 0. One-paragraph thesis

The two adversarial findings are real and they reshape the design. First: the parser **builds as it parses** — `IAsciiChunkParser::ParseChunk` runs the generic dispatcher then calls `Finalize(bag, pJob)`, and every `Finalize` is hardwired to call `pJob.Add*/Set*` ([AsciiSceneParser.cpp:9862-9869](../../src/Library/Parsers/AsciiSceneParser.cpp)); the shared command parser's `> quit` calls `exit(1)` ([AsciiCommandParser.cpp:182](../../src/Library/Parsers/AsciiCommandParser.cpp)); and a faithful capturing `IJob` mock would have to implement the **217 pure-virtual** methods of `IJob` (249 `virtual` total). So "side-effect-free **and** parser-identical via a `ValidationJob` mock" is not buildable as a small thing. Second: the descriptor is **not** an exact schema — generic dispatch only checks that `Double`/`UInt`/`Vec`/`Mat`-kind values are *finite numeric tokens* ([AsciiSceneParser.cpp:725-742](../../src/Library/Parsers/AsciiSceneParser.cpp)); it does **not** check vector cardinality, integer-ness, enum membership, or reference existence, and some declared enum values are explicitly *suggestions*. A JSON Schema mechanically generated from the descriptor can therefore be **stricter** than the real parser, so "cannot drift" is false — it can over-reject. The fix has three parts. (1) Split v1 into two tiers: a **parse-only IR** path (the `parse → IR` seam from the end-state, *promoted into phase 1*) that the editor gutter uses for construction-free syntax/schema validation, and an isolated **and sandboxed** throwaway real `Job` (construction, but neutralized + jailed + capped) that the deep `validate` tool uses for the genuinely runtime-dependent checks — making validation side-effect-free *by isolating the sink, not by mocking it*, with a clean `parse → IR → validate IR → apply IR` separation as the end-state. (2) Adopt a **legacy-tolerant definition of validity** with a clean split between the two kinds of blocking outcome (§6.5): a **`parser_error`** means *the real, tolerant parser would reject the scene* — this is the class the conformance test pins to the parser; a **`validation_policy_error`** means *the parser would accept and run it but validation/agent-safety policy refuses it* (the `>` barrier commands — the parser **executes** them, §1.2, so they are a policy refusal, **not** a parser-reject — and the §3.5 sandbox limits); and the richer descriptor-encoded constraints the parser does not enforce (cardinality, integer-ness, closed-enum membership) are **`warning`**, never a blocking error, so existing scenes keep parsing. (3) Treat the generated schema as a *first-pass filter* held honest by a **conformance test** that asserts its **`parser_error`** verdict neither over- nor under-accepts versus the real parser on a corpus (richer-constraint warnings are allowed to be stricter and are not failures; `validation_policy_error` codes are outside the parser-equivalence oracle), rather than as ground truth by fiat.

---

## 1. Why "side-effect-free AND parser-identical" is impossible as first specified

[MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §6.1 proposes running the *real* descriptor dispatch against a **`ValidationJob`** — an `IJob` whose `Add*`/`Set*` record "well-formed" and return success without building anything — and claims this is both side-effect-free and "cannot diverge from what `ParseAndLoadScene` accepts." Three concrete facts in the tree make that combination infeasible as a small, faithful component:

### 1.1 The parser builds as it parses (no parse-only seam)

`ISceneParser::ParseAndLoadScene(IJob&)` ([ISceneParser.h:61](../../src/Library/Interfaces/ISceneParser.h)) either succeeds while **mutating** the `IJob`, or returns `false` with errors sent to `GlobalLog()` — there is no structured report and no dry-run mode. One level down, the descriptor architecture is explicitly *build-on-finalize*:

```cpp
// AsciiSceneParser.cpp:9862
bool IAsciiChunkParser::ParseChunk( const ParamsList& in, IJob& pJob ) const
{
    ParseStateBag bag( &Describe() );
    if( !Implementation::ChunkParsers::DispatchChunkParameters( Describe(), bag, in ) )
        return false;
    return Finalize( bag, pJob );   // ← every Finalize calls pJob.AddX(...) / pJob.SetX(...)
}
```

Validity is determined in two places that cannot be cleanly separated today: the **syntactic / descriptor** pass (`DispatchChunkParameters` — name-known + numeric-finite) and the **semantic / engine** pass (`Finalize` → `pJob.Add*`, which is where a material's painter-pipe, a reference target's existence, a value's physical admissibility, etc. are actually enforced — see §1.4). "Parser-identical" therefore requires running `Finalize`, and running `Finalize` *is* the side effect.

### 1.2 `> quit` terminates the process (and `render`/`load`/`run` have hard side effects)

The shared `AsciiCommandParser` table maps `quit → ParseQuit` ([AsciiCommandParser.cpp:48](../../src/Library/Parsers/AsciiCommandParser.cpp)), and:

```cpp
// AsciiCommandParser.cpp:179
bool AsciiCommandParser::ParseQuit( ... ) {
    GlobalLog()->Print( eLog_Warning, "Bye!" );
    exit(1);            // ← terminates the whole process
    return true;        // ← dead code; never reached
}
```

A validator that fed scene/script text through the command parser unmodified would **kill the host application** the instant the text (or an attacker-supplied snippet) contains a `quit` line — the `return true` after `exit(1)` is unreachable. The other barrier commands are equally unsuitable for a "validate" path: `render → ParseRasterize` kicks a full render that consumes all cores ([AsciiCommandParser.cpp:632](../../src/Library/Parsers/AsciiCommandParser.cpp)); `load → ParseLoad → pJob.LoadAsciiScene` reads an arbitrary file from disk ([:157](../../src/Library/Parsers/AsciiCommandParser.cpp)); `run → ParseRun → pJob.RunAsciiScript` executes an arbitrary script file ([:168](../../src/Library/Parsers/AsciiCommandParser.cpp)); `clearall`/`remove` destroy live scene state ([:152](../../src/Library/Parsers/AsciiCommandParser.cpp), [:524](../../src/Library/Parsers/AsciiCommandParser.cpp)). None of these may execute during validation.

### 1.3 A faithful `IJob` mock is not small

`IJob` ([IJob.h](../../src/Library/Interfaces/IJob.h)) declares **217 pure-virtual** methods (249 `virtual` total). A `ValidationJob` that is *parser-identical* must implement every one with enough fidelity that the `Finalize` methods take the same accept/reject branches the real `Job` would — including the branches that depend on **runtime manager state** (does a referenced painter exist? is it the right pipe? §1.4). That is not a thin "record and return true" stub; reproducing those branches faithfully is reimplementing a large fraction of `Job`'s validation logic in a parallel class — a second source of truth that *will* drift, defeating the stated goal. (`IJobPriv` adds 24 more virtuals on top.)

### 1.4 The decisive validation lives in `Job`, not in the descriptor

The clearest demonstration that "descriptor dispatch == parser acceptance" is false: scalar-painter routing. A material's `roughness`/`ior`/`tau` is declared in the descriptor as an ordinary `Reference`-kind parameter (the descriptor has **no `pipe` field today** — `ValueKind` is `Bool/UInt/Double/DoubleVec3/DoubleVec4/DoubleMat4/String/Filename/Enum/Reference`, [ChunkDescriptor.h:110](../../src/Library/Parsers/ChunkDescriptor.h); `ParameterDescriptor` fields end at `unitLabel`/`apply`, [ChunkDescriptor.h:296-310](../../src/Library/Parsers/ChunkDescriptor.h)). The accept/reject decision — is this name bound to an `IScalarPainter` (OK), to a per-channel scalar painter in a single-scalar slot (reject), to a legacy `IPainter` chunk (reject with the IScalarPainter-refactor diagnostic), or unknown (reject)? — is made entirely in `Job::ResolveOrDiagnoseScalar` ([Job.cpp:2718-2777](../../src/Library/Job.cpp)) by consulting the live `IScalarPainterManager` / `IPainterManager`. The descriptor dispatcher never sees any of this. So a validator that only re-runs `DispatchChunkParameters` validates *less* than the parser; one that re-runs `Finalize` against a faithful mock validates exactly as much only by reimplementing `Job`. This is the crux that the v1 **Tier-2** design (§3.1) resolves by using a *real* (isolated + sandboxed) `Job` as the sink — and the reason this particular check is delegated to Tier 2 rather than the construction-free Tier-1 IR path (§3.0), which by design cannot consult live manager state.

**Conclusion.** Keep the *goal* (a side-effect-free validity check whose verdict matches the real parser) but reject the *mechanism* (a capturing mock that is simultaneously faithful and cheap). The achievable decomposition is two-tier even in v1: **construction-free syntax/schema validation via the parse-only IR path** (the editor gutter's backing — §3.0/§6.4, promoted from the end-state) and, for the genuinely runtime-dependent checks, **side-effect-free by isolating + neutralizing + sandboxing the sink** (the deep `validate` tool — §3.1/§3.5). It becomes **provably parser-faithful by construction** for syntax/schema once that IR seam is the *only* construction-free path and `apply IR` is the lone build stage (`parse → IR → validate IR → apply IR`, the end-state §4).

---

## 2. The command policy (shared with the destructive-command policy)

Validation must classify every `>`-introduced command and decide, per command, whether it is **inert** (safe to ignore during validation), **barrier** (must be rejected/neutralized — never executed), or **applicable** (a legitimate in-scene mutation that the *applied* path may run but the *validate* path records as a no-op intent). This classification is the same allow/deny taxonomy the agent's destructive-command guard needs.

| Command | Handler | Class for validation | Validation behavior |
|---|---|---|---|
| `quit` | `ParseQuit` → `exit(1)` ([:179](../../src/Library/Parsers/AsciiCommandParser.cpp)) | **barrier (process-fatal)** | Never dispatch. Recognize the keyword, emit a diagnostic ("`quit` is not permitted in validated input"), continue. |
| `render`, `renderanimation` | `ParseRasterize(Animation)` ([:632](../../src/Library/Parsers/AsciiCommandParser.cpp), [:713](../../src/Library/Parsers/AsciiCommandParser.cpp)) | **barrier (compute/side-effect)** | Never dispatch (would seize all cores). Recognize + diagnose; rendering is an explicit, separately-scoped tool. |
| `load` | `ParseLoad` → `LoadAsciiScene` ([:157](../../src/Library/Parsers/AsciiCommandParser.cpp)) | **barrier (arbitrary FS read + recursive parse)** | Never dispatch. Recognize + diagnose; cross-file inclusion is out of scope for a single-text validation. |
| `run` | `ParseRun` → `RunAsciiScript` ([:168](../../src/Library/Parsers/AsciiCommandParser.cpp)) | **barrier (arbitrary script execution)** | Never dispatch. Recognize + diagnose. |
| `clearall`, `remove …` | `ParseClearAll`/`ParseRemove` ([:152](../../src/Library/Parsers/AsciiCommandParser.cpp), [:524](../../src/Library/Parsers/AsciiCommandParser.cpp)) | **barrier (destructive)** | Never dispatch against shared state. (Against the v1 *isolated* `Job` they are harmless, but validation still diagnoses them as out-of-scope rather than silently honoring them.) |
| `photonmap … save/load` | `ParsePhotonMap` ([:892](../../src/Library/Parsers/AsciiCommandParser.cpp)) | **barrier (FS I/O)** | Never dispatch; recognize + diagnose. |
| `mediapath …` | `ParseMediaPath` ([:1006](../../src/Library/Parsers/AsciiCommandParser.cpp)) | **barrier (mutates global locator)** | Never dispatch; it mutates process-global `GlobalMediaPathLocator()`. Recognize + diagnose. |
| `set`, `modify`, `predict`, `echo` | `ParseSet`/`ParseModify`/… | **applicable / inert** | `predict`/`echo` are inert (no scene mutation worth blocking). `set`/`modify` are scene mutations the *applied* path may run; the *validate* path checks their argument shape and records intent without executing. |

**Policy realization.** Validation does **not** route through `AsciiCommandParser` unmodified. Either (a) the validator drives only the chunk path and treats any `>`-line as data to classify against this table, or (b) a thin policy shim wraps command dispatch and refuses every **barrier** command before the underlying `Parse*` runs (so `exit(1)` is structurally unreachable). The shim is the single enforcement point; the table is its allow/deny list.

**Shared with the agent security model.** This barrier-command taxonomy is the same allow/deny list the agent surface needs for its destructive-command guard. [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) is the canonical home of that policy and **imports this table** as the authority for "which `>` commands are barriers"; scope tiers and transport security live in [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §7 (`read`/`edit`/`render-and-spend`; no shell, no arbitrary FS; loopback-only) and [LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md) §7.4. This §2 table stays the validation-time source of truth — keep AI_SECURITY_MODEL in sync with it. The mapping is exact: every **barrier** row here requires the `render-and-spend` scope or is outright denied (FS/compute side effects), and **none** of them is reachable from the `read` scope that `validate` itself runs under (`validate` is `readOnly:true`, MCP §4.6).

---

## 3. v1 — pragmatic: a parse-only IR path *and* an isolated, sandboxed throwaway `Job`

The shippable first version is **two tiers**, because the two consumers have different needs and different safety envelopes:

- **Tier 1 — parse-only IR (§3.0): the editor gutter's backing.** Syntax + descriptor-schema validation with **no construction** — no `Job`, no managers, no file I/O. Inherently side-effect-free *by type* and fast enough for a keystroke debounce. This is the `parse → IR` seam from the end-state (§4), **promoted into phase 1** so the gutter has a real v1 backing instead of waiting on the full refactor. It cannot perform the runtime-dependent checks (reference existence, file resolution, the §1.4 pipe check) — those have no static answer.
- **Tier 2 — isolated + sandboxed throwaway `Job` (§3.1, §3.5): the deep `validate` tool's backing.** The only way to be **parser-faithful for construction-level checks** today is to run the real parser against a real `Job`. v1 achieves **side-effect-free** by *isolating and neutralizing* that `Job` rather than mocking it, and (per the second review) **bounds the residual attack surface by sandboxing it** — a throwaway `Job` still resolves attacker-supplied filenames, decodes assets, probes file existence, and runs process-global code, so "isolated" is necessary but not sufficient.

The `validate` tool runs Tier 2 (and includes Tier-1 results); the gutter's `syntax` mode runs Tier 1 only. Both share one `Diagnostic` shape (§6) so the gutter and the agent never disagree about what is valid.

### 3.0 Tier 1 — parse-only IR validation (gutter backing, construction-free)

`SceneValidator::ValidateSyntax(text) → ValidationReport` runs **only** the `parse → IR` + `validate IR` stages of the end-state pipeline (§4.1):

1. **parse** — text → `SceneIR` (§4.2). Lex + structure only: chunks, parameters, source spans. No `IJob`, no managers, no file reads. Fails only on lexical/structural errors (unterminated chunk, brace not on its own line, bad scene version).
2. **validate IR** — `SceneIR` + the grammar descriptor + the richer constraints (§5) → diagnostics. Side-effect-free **by type** (there is no `IJob` in scope to mutate). This is where unknown-chunk / unknown-parameter / type / cardinality / non-integer / closed-enum / required / barrier-command checks live. Per the legacy-tolerant severity policy (§6.5), the only `error`s this tier can raise are the ones the *real* parser also rejects (unknown chunk, unknown parameter, non-finite numeric token, structural/version errors, a barrier command); cardinality / non-integer-`UInt` / closed-enum-membership findings are **`warning`s**.

**Safety.** Tier 1 needs **none** of the §3.5 sandbox machinery: it constructs nothing, opens no file, resolves no asset, and touches no process-global state, so there is nothing for an attacker-supplied filename or oversized asset to act on. It is the path that can safely run on every keystroke and on fully untrusted text with zero capability checks. Its *limit* is that it cannot answer the runtime-dependent questions (does a referenced painter exist and in the right pipe? does a `Filename` resolve under `RISE_MEDIA_PATH`?) — those are delegated to Tier 2 / `full` mode (§5.1, §5.3).

### 3.1 Tier 2 — mechanism (isolated throwaway `Job`)

`SceneValidator::Validate(text, mode=full) → ValidationReport` (the deep tier; `mode=syntax` short-circuits to the Tier-1 IR path of §3.0 and never reaches the steps below):

1. **String input.** Add a string-input entry to `AsciiSceneParser` (a `ParseString`/string constructor) so validation needs no temp file. Independently useful (tests, the Tier-1 IR build, the editor gutter, §6).
2. **Sandbox established** (§3.5). Before any construction runs, the call enters a **sandbox**: filename resolution and asset I/O are confined to the session's project root via the shared `ProjectRootJail` ([AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4.1); size/time/memory caps and a cancellation token are armed. This bounds what attacker-supplied filenames, oversized assets, and runaway construction can do — *before* step 3 lets `Finalize` touch any of them.
3. **Isolated `Job`.** Construct a **fresh, private `Job`** (the same concrete type the real load uses) that shares **no** state with the live document's controller/`Job`/`Scene`/managers. Run the normal descriptor dispatch + `Finalize` against *this* `Job`. Because `Finalize` calls `pJob.Add*` on the **throwaway**, the full semantic pass (`ResolveOrDiagnoseScalar` and friends, §1.4) runs against an isolated manager set — so the verdict matches the real parser, and the only mutations are to an object that is immediately discarded.
4. **Command policy applied** (§2). The barrier commands are refused before they can run — no `exit`, no render, no disk `load`/`run`, no global-locator mutation. The isolated `Job` never reaches a render or a disk write because no code path calls `Rasterize()`/`Save*()` on it.
5. **Capture diagnostics.** Redirect this parse's diagnostics into a per-call structured sink instead of `GlobalLog()` (the parser currently logs to the global log; validation needs an instance-scoped collector — see §6). Each captured message becomes a `Diagnostic` (§5).
6. **Discard.** Destroy the isolated `Job` and its managers; release the sandbox (jail, caps, cancellation). Nothing escapes; the live document is untouched.

### 3.2 Isolation contract (what "isolated" must guarantee)

- **No shared mutable singletons reachable from `Finalize`.** The isolated `Job` owns its own `Scene`, named managers, and `"none"` defaults (`Job::InitializeContainers()`), distinct from the live document's. The one process-global the parser can touch — `GlobalMediaPathLocator()` — is *not* mutated because `> mediapath` is a barrier (§2) and filename **resolution** is read-only against it (acceptable: it only affects whether a `FILE_NOT_FOUND` is reported, which is the desired check in `full` mode).
- **No threads, no render, no present surface.** The isolated `Job` is never started, never rasterized. Validation is synchronous and CPU-cheap (parse only).
- **No `GlobalLog()` cross-talk.** Diagnostics go to the per-call sink, not the shared `RISE_Log.txt`, so a validate call from one MCP session can't pollute another session's log or the user's render log.

### 3.3 Lifetime

The isolated `Job` lives **only** for the duration of one `Validate` call: constructed at entry, destroyed before return (RAII — a local `unique_ptr<Job>`). No caching of the validation `Job` across calls (a stale validation `Job` would diverge from the descriptor after a hot-reload and reintroduce a drift surface). `validate` is `idempotent:true, readOnly:true` (MCP §4.6) precisely because each call is a fresh isolated parse with zero retained state.

### 3.4 Honest limits of v1

- **Tier-2 is not a *proof* of parser-identity** — it is parser-identity *in practice* because it runs the same code, but the syntactic and semantic passes are still entangled *inside Tier 2* (the throwaway `Job` runs engine construction even for inputs whose only defects are syntactic). The end-state (§4) makes the separation structural; the **Tier-1 IR path (§3.0) already delivers construction-free syntax/schema validation in v1** — it is the gutter's answer to "validate syntax without building anything," and Tier 2 is reserved for the runtime-dependent checks that genuinely need construction.
- **`mediapath`/cross-file** semantics are deliberately not modeled (barriers); a scene that depends on a prior `> load` or `> mediapath add` validates against the default locator only. Document this in the tool description so the model doesn't over-trust a green result for include-dependent scenes.
- **Cost.** Tier 1 is one parse (no construction) — interactive on a keystroke debounce. Tier 2 is one parse + one throwaway scene-graph construction; for pathological inputs (e.g. a huge `sdf_geometry` or a tessellating geometry) construction is non-trivial, which is *why* Tier 2 is sandboxed with size/time/memory caps (§3.5) and is reserved for explicit `full`-mode / pre-commit checks rather than the keystroke path. The gutter defaults to `syntax` (Tier 1); `full` (Tier 2) is opt-in. Geometry that *refuses to tessellate at parse time* (deferred-realization `CanTessellate` parse-refusal) is itself a validation signal, not a crash.

### 3.5 Sandbox (Tier 2 only — "isolated" is necessary but not sufficient)

Isolation (§3.2) guarantees the throwaway `Job` shares no state with the live document. It does **not** make a `Job` constructed from *attacker-supplied text* safe: `Finalize` still resolves filenames the text names, decodes whatever assets those filenames point at (HDRIs, meshes, textures), probes the filesystem for existence, and runs process-global construction code. A `validate(scene_text)` of hostile input is therefore an untrusted-input-driven construction and must be **sandboxed**, not merely isolated. (The Tier-1 IR path of §3.0 needs **none** of this — it constructs nothing, so there is no filename to resolve, no asset to decode, and no global code to run.)

The sandbox layers four controls onto the Tier-2 call, all of them *shared with the agent security model* so there is one enforcement surface, not a validation-private fork:

1. **Project-root confinement (reuse `ProjectRootJail`).** Every filename the validated text names — directly (`png_painter { file … }`, mesh `file`, `> load`/`> mediapath` targets) or transitively — is resolved-and-confined through the **shared `ProjectRootJail`** helper in `src/Library/Agent/` ([AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4.1): absolute paths and `..`-escapes are rejected, and containment is enforced by the jail's **verified-handle / no-follow `openat` design ([AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4.1.2), NOT canonicalize-then-open** — a canonical-path check alone is a TOCTOU race (a symlink that passes the check can be swapped to point outside the root before the `open`), so the jail resolves component-by-component from a held root-dir handle with `openat(…, O_NOFOLLOW)` and verifies the final descriptor (`fstat` `st_dev`/`st_ino` ancestry; on Windows `FILE_FLAG_OPEN_REPARSE_POINT` + `GetFinalPathNameByHandle`), then does all I/O on that verified handle. A target outside the root yields the structured `PATH_OUTSIDE_PROJECT_ROOT` code (surfaced as a `FILE_NOT_FOUND`-class diagnostic, never a silent read of `/etc`). The validator must **not** re-implement a parallel canonicalizer (§9 non-goal) — it consumes the jail's verified-open path verbatim. Reads may resolve through `RISE_MEDIA_PATH` / asset-library roots exactly as the jail's read-allowlist permits; *writes never happen* in validation (the barrier-command policy §2 already blocks `save`/`photonmap save`, and no validate path calls `Save*()`). This is the same jail the §2 barrier table and `load_scene` policy lean on — validation is its strictest consumer (read-only, no Save-As dialog escape hatch).
2. **Resource caps (size / time / memory).** Construction is bounded so a hostile or merely huge input cannot DoS the host: a **wall-clock time cap** (cancellation fires past it — see #3), a **peak-memory cap** on the throwaway `Job`'s allocations, and **input/asset size caps** (max scene-text length; max decoded-asset bytes, enforced *before* an image/mesh is fully decoded so a "decompression-bomb" asset is refused early, not after it has inflated). Hitting any cap ends the call with a diagnostic (`VALIDATION_LIMIT_EXCEEDED`), not a crash or an OOM-kill of the whole process. These mirror the per-call budget the §8 acceptance block bounds (one throwaway `Job` + one buffer + one IR).
3. **Cancellation.** The whole Tier-2 construction is cooperatively cancellable — the time cap, an explicit client cancel (an MCP request cancellation), or session teardown aborts the parse/construct in flight and tears down the throwaway `Job`. Because the isolated `Job` is never started/rasterized (§3.2), cancellation only has to unwind a synchronous construction, not stop a running render.
4. **Risky construction out-of-process — REQUIRED for the untrusted/agent path, not merely "ideal" (corrects the §3.5 sandbox-honesty overstatement; GUI_ROADMAP §13a #5). 🔨 TO-BUILD — this child process does not exist yet.** The §8 "no crash / no OOM / no hang" guarantee is only *honest* for untrusted input if the highest-risk step — decoding attacker-supplied image/mesh assets through third-party codecs — runs in a **separate, locked-down child process** (its own `ProjectRootJail`, its own caps, no credentials, no network) that returns only a structured accept/reject + diagnostics over a pipe. **In-process caps cannot deliver that guarantee against hostile input:** a peak-memory cap is checked between allocations, but a codec that segfaults, corrupts the heap, or allocates one giant buffer past the cap in a single call takes down the *whole host* before any cap fires — exactly the crash/OOM the guarantee promises to prevent. Only a child process (which the OS can hard-kill on crash or RLIMIT) actually bounds those. Therefore:
   - **The MCP `validate` tool path (the untrusted/agent path) MUST construct out-of-process** to claim "no crash/OOM." Until that child is built (it is net-new — the same RISE binary re-invoked in a locked-down mode, §7 / §8 Packaging), the *honest* statement is that in-process Tier-2 on untrusted text bounds **soft** limits (the cap-checked time/memory budget, the jail) but **cannot** guarantee against a hard codec crash/OOM — so an MCP `validate` of arbitrary text either runs out-of-process or is documented as best-effort-not-guaranteed on the crash/OOM axis, never advertised as fully crash-safe in-process.
   - **The editor's own *trusted* buffer** (text from the user's keystrokes) may run Tier-2 in-process with caps + jail only — the threat model there is a user typing into their own editor, not an adversary, so the hard-crash guarantee is not load-bearing.
   - Android's in-process Tier-2 (§8 Platform parity) carries the same honesty caveat: it applies jail + caps + cancellation, and the out-of-process hardening is a desktop-first build; on Android the untrusted-text crash/OOM guarantee is best-effort until/unless an equivalent isolated path exists.

**Net:** Tier 1 is safe by construction (nothing to attack); Tier 2 is made safe by jail + caps + cancellation, reusing the agent security model's `ProjectRootJail` rather than inventing a second confinement — **and, for the untrusted MCP `validate` path, by *required* out-of-process asset decode (#4, 🔨 TO-BUILD): in-process caps bound soft budgets but cannot guarantee against a hard codec crash/OOM, so the "no crash/OOM" guarantee holds out-of-process or is stated as best-effort until that child ships.**

---

## 4. End-state — principled: `parse → IR → validate IR → apply IR`

The clean architecture the v1 path is a stepping-stone toward. It removes the entanglement that forces v1 to construct a real `Job` just to check syntax.

### 4.1 The seam

Split parsing into three stages with an explicit intermediate representation between them:

```
scene text ──▶ [ parse ] ──▶ Scene IR ──▶ [ validate IR ] ──▶ ValidationReport
                                  │
                                  └────────▶ [ apply IR ] ──▶ Job / Scene   (the build-on-finalize step, unchanged)
```

- **parse** — pure text → IR. Lex + structure only: chunks, their parameters, and **source spans**. No `IJob`, no managers, no construction. Cannot fail on semantics — only on lexical/structural errors (unterminated chunk, brace not on its own line, bad scene version).
- **validate IR** — IR + grammar (descriptor) + the richer constraints (§5) → diagnostics. Side-effect-free *by type*: it has no `IJob` to mutate. This is where the construction-free checks live: unknown-chunk / unknown-parameter / non-finite-numeric / required (the parser-rejected set → `error`, §6.5) **plus** cardinality / non-integer-`UInt` / closed-enum-membership (the richer, parser-tolerated set → `warning`, §6.5). Reference **existence** and file **resolution** are *not* here — they need runtime manager / locator state and are deferred to `apply IR` / Tier-2 `full` mode (§1.4, §3.0).
- **apply IR** — IR → `Job` via the existing `Finalize` logic (the only stage that builds anything; the current `pJob.Add*` path, essentially unchanged). The semantic checks that *must* see runtime manager state (the `ResolveOrDiagnoseScalar` pipe check, §1.4) run here and surface the **same `Diagnostic` shape** so an apply-time failure is reported identically to a validate-time one.

### 4.2 The IR (sketch)

A small, allocation-light tree mirroring the chunk grammar, each node carrying its byte span (so diagnostics get line/column for free):

```cpp
namespace RISE {                       // src/Library/Agent/SceneIR.h (no bare "MCP" token, per §16)
  struct ParamNode {
    std::string name;                  // "roughness"
    std::string rawValue;              // "0.1 0.4 0.9"  (verbatim, pre-coercion)
    RawTokenSpan keywordSpan;          // byte range of the name token   (reuses SourceSpanIndex.h:51)
    RawTokenSpan valueSpan;            // byte range of the value tokens
    // resolved lazily by validate-IR: kind from descriptor, token count, etc.
  };
  struct ChunkNode {
    std::string keyword;               // "ggx_material"
    ChunkCategory category;            // from the matched ChunkDescriptor (or "unknown")
    std::string name;                  // the chunk's own name, if it has one
    std::vector<ParamNode> params;
    std::size_t chunkBeginOffset, chunkEndOffset;
  };
  struct SceneIR {
    int version = 6;                   // "RISE ASCII SCENE 6"
    std::vector<ChunkNode> chunks;
    std::vector<CommandNode> commands; // classified per §2; barriers flagged here
  };
}
```

The IR reuses the byte-range types **already designed for round-trip save** — `RawTokenSpan` / `ParameterSpan` ([SourceSpanIndex.h:51-73](../../src/Library/SceneEditor/SourceSpanIndex.h)) — so validation and save share one span vocabulary. Note `Job` itself retains **only** byte offsets, not text (`SourceSpanIndex` = offsets only, [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §12; SaveEngine re-reads the file from disk at save time, `SaveEngine.cpp:869`). The IR is the missing in-memory text-backed structure; building it also satisfies the separate "in-memory scene-text accessor" need ([MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §3.1).

### 4.3 Why this cleanly separates concerns

- **Syntax/schema validation needs no engine.** `validate IR` has no `IJob` in scope, so it is side-effect-free **by construction**, not by isolation discipline — the v1 "throwaway `Job`" disappears for everything except the genuinely runtime-dependent checks.
- **`validate` and `apply` share one rule set.** Both read the same descriptors and the same IR; the only checks reserved for `apply` are the ones that *require* live manager state (§1.4), and those reuse the `Diagnostic` shape, so there is no second diagnostic vocabulary.
- **It is the substrate `add_entity` / structured save / the node graph already want.** A validated IR node is exactly what `add_entity` should append, what round-trip save should diff against, and what the material node-graph editor should serialize ([GUI_ROADMAP.md](../GUI_ROADMAP.md) §7 C2).

### 4.4 Migration

`apply IR` is a refactor of the existing `Finalize`-calls-`pJob.Add*` flow, not a rewrite — `DispatchChunkParameters` already produces a per-chunk `ParseStateBag`, which is one short step from a `ChunkNode`. This is sequenced after v1 ships and is gated behind the same byte-identity / corpus tests v1 introduces, so the integrator/parser behavior stays observably unchanged.

---

## 5. The descriptor-is-not-an-exact-schema problem

[MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §5.3 claims a schema generated from `SceneGrammar::Describe()` "cannot drift" from the parser and that "a value that passes `validate` is exactly a value the real parser accepts — by construction." **Retract both.** A generated schema can be *stricter* than the parser, because the descriptor encodes more intent than the generic dispatcher enforces, and some descriptor metadata is advisory.

### 5.1 What the generic dispatcher actually checks (and doesn't)

The only value validation the descriptor dispatcher performs is in the `switch` at [AsciiSceneParser.cpp:725-742](../../src/Library/Parsers/AsciiSceneParser.cpp): for `Double`/`DoubleVec3`/`DoubleVec4`/`DoubleMat4`/`UInt` it runs `AllTokensAreFiniteNumbers` ([:658](../../src/Library/Parsers/AsciiSceneParser.cpp)) — "every whitespace-separated token is a finite number, no `nan`/`inf`." Everything else falls to `default: break` (no check). Consequences:

The "drift direction" below describes what a **naive** schema (one that emitted every descriptor intent as a hard `error`) *would* do — which is exactly the over-rejection the **legacy-tolerant** policy (§6.5) forecloses: each "schema stricter" row is resolved by emitting that constraint as a **`warning`**, not an `error`, because the tolerant parser accepts the value. The schema's *error* set therefore stays equal to the parser's reject set (§5.4); the richer checks survive only as helpful warnings.

| Descriptor intent | Parser actually enforces? | A naive generated schema would enforce | Drift direction (naive) → legacy-tolerant resolution |
|---|---|---|---|
| `DoubleVec3` = exactly 3 numbers | **No** — `AllTokensAreFiniteNumbers` counts tokens but does not require 3; `ParseStateBag::GetVec3` `sscanf`s whatever is there ([ChunkDescriptor.h:221](../../src/Library/Parsers/ChunkDescriptor.h)), zero-filling missing, ignoring extra | `minItems:3, maxItems:3` | naive: **schema stricter** (rejects a 2-/4-token value the parser silently accepts) → **emit as `warning`** (`VECTOR_CARDINALITY`), never `error` |
| `UInt` = a non-negative *integer* | **No** — `strtod` accepts `3.5`/`1e2`; `GetUInt` → `String::toUInt` coerces later ([ChunkDescriptor.h:197](../../src/Library/Parsers/ChunkDescriptor.h)) | `type:integer, minimum:0` | naive: **schema stricter** (rejects `3.5` the parser truncates) → **emit as `warning`** (`NON_INTEGER_UINT`), never `error` |
| `Enum` = one of `enumValues` (closed) | **No** — `Enum` hits `default: break`; the value passes dispatch and any membership decision is made (or not) later in `Finalize` | `enum:[...]` | naive: **schema stricter** *(and worse for open enums — §5.2)* → **emit as `warning`** (`ENUM_VALUE_INVALID`, closed enums only), never `error`; open/suggestion enums emit `examples`, no finding |
| `Reference` names an existing chunk | **No** at dispatch — existence is a `Finalize`/`Job` concern (§1.4) | `type:string` (no existence check) | schema looser (acceptable; existence is a Tier-2 `full`-mode check, whitelisted in §5.4) |
| `Filename` resolves under `RISE_MEDIA_PATH` | **No** at dispatch | `type:string` | schema looser (acceptable; `FILE_NOT_FOUND` is a Tier-2 `full`-mode check, whitelisted in §5.4) |

### 5.2 Some declared enum values are intentionally suggestions

The strongest counterexample to "enum is a constraint": `pathguiding_sampling_type` declares `enumValues = {"ris","RIS","OneSampleMIS"}` with the description *"any string other than ris/RIS selects OneSampleMIS"* ([AsciiSceneParser.cpp:828](../../src/Library/Parsers/AsciiSceneParser.cpp)). The parser accepts **any** string here; the enum list is a *quick-pick hint for the editor combo box*, not a closed set. This matches the documented `ParameterPreset` semantics — "the user can still type a custom value — the preset list is a convenience, not a constraint" ([ChunkDescriptor.h:287-294](../../src/Library/Parsers/ChunkDescriptor.h)). A schema that emits `"enum":["ris","RIS","OneSampleMIS"]` here is **wrong** — it would reject a value the parser honors. So the generator must distinguish *closed* enums (reject off-list) from *open/suggestion* enums (treat the list as `examples`, not `enum`). That distinction is **not currently encoded** in `ParameterDescriptor` and is one of the "richer constraints" §16 puts on the new field.

### 5.3 Replacement claim (what is actually true)

> The schema is generated from the descriptor **plus the adopted `ParameterSemantics` structure** (separate fields: pipe `color`/`scalar`/`either`, cardinality, integer-ness, closed-vs-open enum, units, colour space, spatial-vs-spectral, `requireSingle`), and carries a **two-level severity**: a constraint maps to a schema **error** *only* when the real, tolerant parser also rejects it; every richer constraint the parser tolerates is a schema **warning** (§6.5). It is held honest by a **conformance test** (§5.4) that asserts the schema's **error** verdict neither over- nor under-accepts versus the real parser on a corpus — the *warnings* are deliberately permitted to be stricter than the parser and are **not** conformance failures. The schema is a fast **first-pass filter** for the LLM and the editor; the **`validate` tool (§3/§4) remains the authority**, because only it runs the parser's full accept/reject (including the runtime-manager checks of §1.4 that no static schema can express).

The richer constraints live in the **`ParameterSemantics` structure on `ParameterDescriptor`** adopted in [GUI_ROADMAP.md](../GUI_ROADMAP.md) §16 — **separate fields** (pipe `color`/`scalar`/`either`, cardinality, units, colour space, spatial-vs-spectral, `requireSingle`), **not** one overloaded enum — owned by [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md). This doc consumes that structure; it does not own its shape. With it, the schema can finally type `ior`/`film_ior` (scalar pipe) differently from a true color slot — the very distinction the descriptor lacks today ([CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) §4, §7).

### 5.4 The conformance test (definition)

A standalone test (`tests/SchemaConformanceTest.cpp`, the repo's executable-test convention — not a framework suite) that, for a **corpus** of parameter values per chunk, asserts the generated schema's **`parser_error`** verdict matches the real parser's accept/reject verdict — catching both over- and under-acceptance *of the parser-error set*. The legacy-tolerant severity policy (§6.5) is baked into the test: the unit of comparison is "would the schema raise a **`parser_error`** (an `errorClass:"parser"` finding)?" versus "would the parser **reject**?", and two categories are **explicitly outside the equivalence**: (a) **richer-constraint `warning`s** — allowed to be stricter than the parser, never a failure; and (b) **`validation_policy_error` codes** (`BARRIER_COMMAND`, `PATH_OUTSIDE_PROJECT_ROOT`, `VALIDATION_LIMIT_EXCEEDED`) — these are *policy/sandbox* verdicts the parser does **not** make (the parser *accepts and runs* a barrier command, §1.2), so they are not schema-emitted and are never measured against the parser-reject oracle (governed instead by the §2 command policy + §3.5 sandbox).

1. **Corpus.** For each `ChunkDescriptor` from `SceneGrammar::Instance()` and each `ParameterDescriptor`, generate value cases: (a) the `defaultValueHint`; (b) each `preset`/`enumValue`; (c) **boundary/negative cases by kind** — for `DoubleVec3`: 2-, 3-, 4-token strings (cardinality), a non-finite token (`nan`/`inf`); for `UInt`: `3`, `3.5`, `-1`, `1e2`; for `Enum`: an on-list token, an off-list token (and, for an *open*/suggestion enum, an arbitrary string); for `Reference`: a name that exists in the corpus scene and one that doesn't; for a `Filename`: a resolvable and an unresolvable path. Each case is tagged with the **parser's expected disposition** (accept / reject) so the test knows which equivalence to assert.
2. **Oracle = the real parser.** Run each case through the **v1 isolated-`Job` validator** (§3.1, Tier 2) — the ground-truth accept/reject (it runs the actual, tolerant parser). "Reject" = the parser refused to load (a `parser_error`-class outcome); "accept" = the parser loaded it (possibly with the parser's own non-fatal diagnostic). Note a barrier command is an *accept* by this oracle (the parser would execute it), which is exactly why it is classified `validation_policy_error`, not `parser_error`.
3. **Candidate = the generated schema.** Run each case through the schema validator (`Mcp`-free name per §16, e.g. `SchemaGen` + a JSON-Schema check), recording the **class** of any finding (schema `parser_error` vs `warning`), not just accept/reject. (The schema never emits `validation_policy_error` codes — those are validate-time policy/sandbox verdicts, §6.5.)
4. **Assertions (`parser_error`-set equivalence — the one consistent definition of validity).** For every case:
   - **`schema.raisesParserError(case) == parser.rejects(case)`.** The schema's *`parser_error`* set must neither over- nor under-accept versus the parser. (Only `errorClass:"parser"` findings count; `validation_policy_error` codes are not schema-emitted and are not in this comparison at all.)
   - A **schema-error-stricter** mismatch (schema raises a `parser_error`, parser accepts) **fails the test** — this is the §5.1/§5.2 over-rejection bug. The fix is to **demote that constraint to a `warning`** in the generator (the legacy-tolerant default: cardinality, non-integer-`UInt`, closed-enum membership all become warnings, because the tolerant parser accepts them — §6.5) and/or emit `examples` not `enum` for open enums. *Tightening the parser* to reject the value is also permissible **only** when the descriptor intent is the genuinely desired contract **and** the value is one the real parser already rejects today — never a back-door for promoting a tolerated value to a hard error (that would break existing scenes; recorded explicitly in the test if ever done).
   - A **schema-warning that the parser accepts is NOT a mismatch** — it is the intended, legacy-tolerant behavior. The test asserts these emit `warning` (never a blocking error) and otherwise ignores them; a warning being "stricter than the parser" is by design and is **not** a conformance failure.
   - A **schema-error-looser** mismatch (schema raises no `parser_error`, parser rejects) is allowed **only** for the documented runtime-only checks (reference existence, file resolution, the §1.4 pipe check) which are explicitly delegated to `validate`'s `full` mode (Tier 2) and to `apply IR` — the test whitelists exactly those codes (`UNRESOLVED_REFERENCE`, `FILE_NOT_FOUND`, `WRONG_PIPE`) and fails on any other looseness.
   - **`validation_policy_error` cases are asserted separately, not against the parser oracle.** A barrier-command input asserts a `BARRIER_COMMAND` (`errorClass:"policy"`) is raised **and** that the parser-reject oracle reports *accept* (the parser would run it) — proving the code is correctly classified as policy, not parser. This case **fails** only if a barrier command is mislabeled `errorClass:"parser"` or measured as a parser-reject mismatch.
5. **Coverage guard.** Assert one schema entry per `SceneGrammar::AllChunkKeywords()` (the existing anti-fall-out check, [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §5.3) so a new chunk can't silently drop out of the AI's view.

This test is what *earns* the "no drift" property the original doc merely asserted, under one consistent definition: the schema's **`parser_error`** verdict tracks the parser's reject verdict exactly (drift becomes a **red test**), while richer-constraint **warnings** are free to be more helpful than the parser without ever being a conformance failure, and **`validation_policy_error`** codes (barrier commands, sandbox limits) are governed by their own policy/sandbox specs rather than the parser oracle.

---

## 6. Diagnostics format (powers both the MCP `validate` tool and editor squiggles)

One diagnostic shape serves both consumers. It is sourced from the **raw-token byte ranges** so it carries precise locations.

### 6.1 Shape

```jsonc
{
  "ok": false,
  "diagnostics": [
    {
      "severity": "error",                 // error | warning | info (gating: error blocks apply)
      "errorClass": "parser",              // on severity:"error" only — "parser" | "policy" (§6.5);
                                           //   parser = the tolerant parser rejects (parser_error);
                                           //   policy = parser accepts+runs it but policy refuses
                                           //   (validation_policy_error: BARRIER_COMMAND, sandbox limits)
      "code": "UNKNOWN_PARAMETER",         // stable, machine-matchable (table below)
      "chunk": "ggx_material",             // chunk keyword for context
      "parameter": "roughnes",             // offending parameter, when applicable
      "line": 42, "column": 5,             // 1-based, derived from byteBegin (§6.3)
      "endLine": 42, "endColumn": 13,      // span end, for squiggle extent
      "byteBegin": 1180, "byteEnd": 1188,  // raw offsets (editor/AOV-agnostic)
      "message": "Parameter 'roughnes' is not declared on chunk 'ggx_material'.",
      "suggestion": "roughness",           // single best fix (SuggestionEngine)
      "candidates": ["roughness"]          // ranked alternatives, when ambiguous
    }
  ]
}
```

### 6.2 Codes

Stable set (extends the MCP §6.2 list with the schema-gap cases this doc surfaces). The **default class** of each code is fixed by the legacy-tolerant policy (§6.5) — there are **three blocking-or-advisory classes**: `parser_error` (the tolerant parser *rejects*), `validation_policy_error` (the parser *accepts and runs* it but policy refuses it — **not** a parser-reject), and `warning`/`info` (richer constraints the parser *accepts*). On the wire all of `parser_error`/`validation_policy_error` carry `severity:"error"` with an `errorClass` discriminator (`parser`/`policy`); see §6.5:

- **`parser_error` (`errorClass:"parser"` — the parser rejects):** `SYNTAX_ERROR`, `SCENE_VERSION`, `UNKNOWN_CHUNK`, `UNKNOWN_PARAMETER`, `TYPE_MISMATCH` *(a non-finite numeric token in a `Double`/`UInt`/`Vec`/`Mat` slot — the one value check the dispatcher enforces, [AsciiSceneParser.cpp:725-742](../../src/Library/Parsers/AsciiSceneParser.cpp))*, `MISSING_REQUIRED`, `DUPLICATE_NAME`. The runtime-only Tier-2/`full`-mode codes `UNRESOLVED_REFERENCE`, `WRONG_PIPE` *(new — the §1.4 `ResolveOrDiagnoseScalar` color-vs-scalar failure)*, and `FILE_NOT_FOUND` are also `parser_error`s **when reached** (the parser rejects these too, just later, in `Finalize`/`Job`) — they are the §5.4-whitelisted checks a static schema can't express. This is the bucket the conformance test (§5.4) pins to the parser's reject verdict.
- **`validation_policy_error` (`errorClass:"policy"` — the parser ACCEPTS and runs it, but policy refuses; NOT a parser-reject, excluded from §5.4 equivalence):** `BARRIER_COMMAND` *(new — a `>` barrier command per §2; the parser would **execute** it, e.g. `> quit` → `exit(1)` — §1.2 — so this is a policy refusal, **not** "the parser rejects")*, `PATH_OUTSIDE_PROJECT_ROOT` *(new — a filename escapes the `ProjectRootJail`, §3.5)*, `VALIDATION_LIMIT_EXCEEDED` *(new — a size/time/memory cap was hit, §3.5; the verdict is incomplete-by-policy, not a parse failure)*. All three block a `validate`-gated apply (so the model/editor never treats a barrier/escaped/capped input as green) but are governed by the §2 command policy + §3.5 sandbox, **not** by the parser-equivalence oracle.
- **`warning` (parser tolerates — richer constraint only, NEVER a blocking error):** `VECTOR_CARDINALITY` *(new — §5.1, e.g. `DoubleVec3` got 2 or 4 tokens; parser zero-fills/ignores)*, `NON_INTEGER_UINT` *(new — §5.1; parser truncates)*, `ENUM_VALUE_INVALID` *(new — §5.2, **closed** enums only; open/suggestion enums emit no finding)*. These are precisely the constraints the conformance test (§5.4) forbids from ever being emitted as a blocking error.

`suggestion`/`candidates` reuse the existing `SuggestionEngine` ranking (nearest known parameter; nearest existing reference of the right category) so the model gets the *fix*, not just the complaint.

### 6.3 Sourcing locations from byte ranges

The IR's `RawTokenSpan` (§4.2) / round-trip save's `ParameterSpan` ([SourceSpanIndex.h:62-73](../../src/Library/SceneEditor/SourceSpanIndex.h)) give `byteBegin`/`byteEnd` per token. Line/column are computed once per validate call from a newline index over the input text (the validator owns the text — that's the point of the string-input entry, §3.1 / §4.2). Carrying both raw offsets *and* line/column lets the editor map to its own buffer while the MCP client can render "line N, column M" without re-deriving. Where the v1 path can't yet attach a span (a diagnostic raised deep in `Finalize`/`Job`), fall back to chunk-keyword + parameter-name context (the dispatcher always knows both) and a chunk-level line — never a bare message.

### 6.4 Two consumers, one source

- **MCP `validate` tool** returns `ValidationReport` as `structuredContent` (MCP §6.2/§8). On a mutation tool that touches scene text, the **same** report is returned with `isError:true` *instead of* mutating, so the model self-corrects from line/parameter/suggestion feedback and the scene is never left half-edited (MCP §8). `validate` is `readOnly:true, idempotent:true` and runs under the `read` scope (§2).
- **Inline editor squiggles.** The editor's "problems" gutter calls the **same** `SceneValidator` in `syntax` mode (the **Tier-1 parse-only IR path**, §3.0) on a debounce — fast because it constructs nothing and touches no file — maps `byteBegin/byteEnd` to squiggle ranges, and shows `message` + `suggestion` in the hover/quick-fix. Because both consumers call one library function over one rule set, the gutter and the agent can never disagree about what's valid (principle #2 — shared C++).

### 6.5 Severity policy (legacy-tolerant — the one consistent definition of validity)

This is the **single, authoritative** severity definition; §5.3, §5.4, and §6.2 all derive from it. The contract is **legacy-tolerant** so existing scenes keep parsing. **It has THREE blocking outcomes plus two advisory ones — the third blocking outcome (`validation_policy_error`) is the correction (GUI_ROADMAP §13a #5): the earlier model had a single `error` class and parked the barrier commands under "parser rejects," which is factually wrong — the *real parser accepts and executes* a barrier command (`> quit` runs `exit(1)`, §1.2; that is the danger, not a rejection). A barrier command is refused by *policy*, not by the parser, so it cannot share the `error`-≡-parser-rejects bucket.**

- **`parser_error` = the *real, tolerant* parser would reject (the scene won't load).** This is the bucket the conformance test (§5.4) pins to the parser's reject verdict. Concretely: structural/lexical/version failures (`SYNTAX_ERROR`, `SCENE_VERSION`), an unknown chunk (`UNKNOWN_CHUNK`), an unknown parameter (`UNKNOWN_PARAMETER`), a non-finite numeric token in a numeric slot (`TYPE_MISMATCH` — the lone dispatcher value check, [AsciiSceneParser.cpp:725-742](../../src/Library/Parsers/AsciiSceneParser.cpp)), a missing required parameter (`MISSING_REQUIRED`), a duplicate name (`DUPLICATE_NAME`), and — when Tier-2/`full` mode reaches them — the runtime-only rejections the parser makes later in `Finalize`/`Job` (`UNRESOLVED_REFERENCE`, `FILE_NOT_FOUND`, the §1.4 pipe mismatch `WRONG_PIPE`).
- **`validation_policy_error` = the parser would *accept and run* it, but validation/agent-safety policy refuses it (NOT a parser-reject).** The parser does not reject these — RISE's tolerant parser *executes* them — so they are **not** measured against the parser's accept/reject verdict and are **excluded from the §5.4 conformance equivalence**. They block a `validate`-gated apply because the *policy* forbids them, not because the scene "won't load": a `>` barrier command (`BARRIER_COMMAND`, §2 — `quit`/`render`/`load`/`run`/`mediapath`/`clearall`/`remove`, the same barrier table AI_SECURITY_MODEL §5 imports), and the Tier-2 sandbox refusals `PATH_OUTSIDE_PROJECT_ROOT` (a filename escapes the `ProjectRootJail`, §3.5) and `VALIDATION_LIMIT_EXCEEDED` (a size/time/memory cap was hit, §3.5 — the verdict is *incomplete by policy*, not a parse failure). These are reported as a blocking policy refusal so the model/editor never treats a barrier or a capped/escaped input as a green result.
- **`warning` = the parser *accepts*, but a richer, descriptor-encodable constraint the tolerant parser does not enforce is violated.** Because the parser does **not** enforce these, they MUST be `warning`, never `parser_error`/`validation_policy_error` (emitting them as a blocking error would reject scenes the engine loads fine — the §5.1/§5.2 over-rejection bug, forbidden by the conformance test §5.4): vector cardinality (`VECTOR_CARDINALITY`, `DoubleVec3` with ≠3 tokens), non-integer `UInt` (`NON_INTEGER_UINT`), off-list **closed**-enum membership (`ENUM_VALUE_INVALID`, closed enums only), an out-of-pipe `Reference` the runtime will coerce, an off-list **open**-enum token (no finding for open/suggestion enums).
- **`info` = advisory** (a deprecated-but-tolerated idiom).

**Gating.** Both blocking classes — `parser_error` **and** `validation_policy_error` — block a `validate`-gated apply; `warning`/`info` surface but don't gate (exactly matching the parser's own accept-with-diagnostic behavior). The distinction between the two blocking classes is *why* they block: `parser_error` because the engine won't load it, `validation_policy_error` because policy refuses it even though the engine would. This is why the richer checks can be strictly more helpful than the parser without ever breaking a legacy scene: they ride as non-gating warnings, and the conformance test (§5.4) holds the *`parser_error`* line precisely at "the parser rejects it" — while the policy refusals (barrier commands, sandbox limits) are governed by the §2 command policy and §3.5 sandbox, **not** by the parser-equivalence oracle.

> **Wire compatibility note.** The diagnostic `severity` field on the wire (§6.1) keeps its existing three values `error | warning | info` for back-compat; the `parser_error` vs `validation_policy_error` split is carried by an additional **`errorClass`** discriminator (`parser` | `policy`) on `severity:"error"` diagnostics (and is implied by the stable `code` — e.g. `BARRIER_COMMAND`/`PATH_OUTSIDE_PROJECT_ROOT`/`VALIDATION_LIMIT_EXCEEDED` are always `policy`; everything else in the blocking set is `parser`). A consumer that only reads `severity` still gates correctly (both classes are `error`); a consumer that wants to explain *why* reads `errorClass`/`code`.

---

## 7. Shared-C++ vs platform split

| Concern | Layer | Where |
|---|---|---|
| `SceneValidator` (Tier-1 IR path + Tier-2 isolated/sandboxed `Job`), IR, schema generator, conformance test, diagnostic types | **Shared C++** | `src/Library/Agent/` (per §16 naming; no bare `MCP` token) |
| String-input parse entry, IR build, `apply IR` refactor | **Shared C++** | `src/Library/Parsers/` |
| Tier-2 sandbox: `ProjectRootJail` (reused), size/time/memory caps, cancellation | **Shared C++** | `src/Library/Agent/` — **`ProjectRootJail` is shared with [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4.1, not forked** |
| Out-of-process asset-decode child (§3.5 #4), if/when built | **Shared C++ + platform-thin** | shared child entry point in `src/Library/Agent/`; process spawn is platform-thin |
| `validate` tool registration, scope enforcement, JSON-RPC marshaling | **Shared C++** | `src/Library/Agent/` |
| Editor gutter rendering (squiggle draw, hover/quick-fix UI) | **Platform** | macOS SwiftUI editor / Windows Qt editor; Android Compose |
| Debounce + buffer→byte-offset mapping plumbing | **Platform-thin** | calls the one shared `SceneValidator` (Tier-1) |

The entire validation *brain* is shared C++; platforms own only how a diagnostic is drawn. Per [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) cross-platform matrix, this is the same thin-shell pattern the property panel and picking already follow.

**Source-file checklist reminder.** Any new `.cpp`/`.h` under `src/Library/` here (e.g. `SceneValidator.{h,cpp}`, `SceneIR.h`) must be added to **all five** build projects per [CLAUDE.md](../../CLAUDE.md) "Source-file add/remove" — none auto-discovers files.

---

## 8. Acceptance criteria (per [GUI_ROADMAP.md](../GUI_ROADMAP.md) §15)

- **Tests.**
  - `tests/SchemaConformanceTest.cpp` — schema-vs-parser **`parser_error`-set** equivalence (§5.4): `schema.raisesParserError(case) == parser.rejects(case)` over the corpus; a schema-error-stricter mismatch (schema raises a `parser_error`, parser accepts) **fails**; richer-constraint **warnings** are explicitly allowed to be stricter than the parser and are **not** failures (the test asserts cardinality / non-integer-`UInt` / closed-enum cases emit `warning`, never a blocking error); only the whitelisted runtime-only codes (`UNRESOLVED_REFERENCE`, `FILE_NOT_FOUND`, `WRONG_PIPE`) may be schema-looser; **`validation_policy_error` codes (`BARRIER_COMMAND`, sandbox limits) are not schema-emitted and are outside this oracle (§5.4)**; one schema entry per `AllChunkKeywords()`.
  - `tests/SceneValidatorTest.cpp` — (a) **Tier-2 isolation invariant:** validating a scene leaves the live document byte-identical and mutates **no** shared manager/global state (assert via a before/after snapshot of an independently-loaded reference `Job`); barrier-command inputs (`quit`/`render`/`load`/`run`/`mediapath`) **never** execute (a `quit` in the input does **not** terminate the test process — the regression guard for [AsciiCommandParser.cpp:182](../../src/Library/Parsers/AsciiCommandParser.cpp)) **and are reported as `BARRIER_COMMAND` with `errorClass:"policy"` while the parser-reject oracle reports *accept* — proving the §6.5 policy-vs-parser classification**; each diagnostic `code` is reproduced by a crafted input; locations match expected line/column. (b) **Tier-1 IR path:** `ValidateSyntax` of the same inputs raises the `parser_error`s and the richer `warning`s **without** constructing a `Job`, opening a file, or resolving an asset (assert no file-open / no manager-construction side effect occurred); a runtime-only defect (dangling reference, missing file) is **absent** from the Tier-1 report and **present** in the Tier-2 report — proving the §3.0/§5.3 delegation.
  - `tests/ValidationSandboxTest.cpp` *(new — the §3.5 sandbox-limits test)* — a validated scene that names a path outside the project root (absolute, `..`-escape) is rejected with `PATH_OUTSIDE_PROJECT_ROOT` and **no read of the out-of-root file occurs**; **the symlink-swap TOCTOU is covered by the verified-handle path, not canonicalize-then-open** — a path whose component is swapped to a symlink-outside-root *between* the first-pass check and the open is refused at the no-follow `openat`/verified-fd step ([AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4.1.2), reusing the `ProjectRootJail` swap-race assertions of [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §12 so the jail is exercised on the validation path, not re-tested in a fork; an input that exceeds the scene-text-size / time / memory cap ends with `VALIDATION_LIMIT_EXCEEDED` (a *cooperative* cap hit — see the fuzz test for the hard crash/OOM bound) and the cancellation token aborts an in-flight construct.
  - `tests/ValidationFuzzTest.cpp` *(new — the fuzz / oversized-asset test)* — a fuzz corpus of malformed/hostile scene text drives both tiers and asserts the **invariant: no input escapes the jail, hangs past the time cap, or executes a barrier command, and only a structured `ValidationReport` (possibly all-error / `VALIDATION_LIMIT_EXCEEDED`) is ever returned**. The **stronger "no crash / no OOM of the host" leg of the invariant is asserted against the out-of-process decode path** (§3.5 #4, 🔨 TO-BUILD): a codec-crash / single-oversized-allocation case takes down only the disposable child, leaving the host validator responsive; **in-process Tier-2 cannot make that guarantee for hostile input** (the test asserts the cooperative caps for the in-process path and the hard crash/OOM containment only for the out-of-process path, so the suite states the honest boundary rather than claiming in-process crash-safety). Includes an **oversized / decompression-bomb asset** case (an image/mesh `file` whose decoded size exceeds the cap) refused **before** full decode (§3.5 #2).
  - **Correctness invariant:** validating then loading the *same* text yields the *same* accept/reject as a direct `ParseAndLoadScene` (parser-faithfulness, the §1 goal); the integrator/parser remains byte-identical for non-validation paths (the `apply IR` refactor is behavior-neutral).
- **Platform parity.** Shared `SceneValidator` (both tiers) ships on **macOS / Windows / Android** identically (it's library code), as does the shared `ProjectRootJail` and the caps/cancellation. The MCP `validate` tool and the editor gutter are wired on **macOS + Windows** first; **Android** gets the library + the chat-surface `validate` per the §16 **Android Tier A** posture ("talk to your renderer from your phone"), with the inline editor gutter a later mobile-UI tier. The validator never degrades — only its UI surfacing differs. (The out-of-process asset-decode child of §3.5 #4 is the **required** untrusted-path hardening and a **desktop-first** build; until it ships, the MCP `validate` crash/OOM guarantee on untrusted text is best-effort, and Android's in-process Tier-2 — jail + caps + cancellation — carries the same honesty caveat.)
- **Performance budget.** **Tier-1** `syntax`-mode validation (the gutter path) is parse-only — no `Job`, no file I/O — and must stay interactive on a debounce (target sub-frame for typical scenes). **Tier-2** `full` mode (pre-commit) constructs the sandboxed throwaway scene and is not on the keystroke path; its worst case is bounded by the size/time caps (§3.5) rather than left unbounded. **No production-render regression** — validation touches no integrator code (cite the L8 ~0.4% bar); the sandbox checks are per-call/per-path, not per-sample.
- **Memory budget.** Peak RSS delta bounded by one throwaway `Job` + one text buffer + one IR per in-flight Tier-2 `Validate` call. The §3.5 memory cap is **cap-checked between allocations**, so it bounds *cooperative* growth (most inputs hit `VALIDATION_LIMIT_EXCEEDED` first) — but it is **not** a hard ceiling against a single oversized allocation or a crashing codec **in-process** (§3.5 #4). The hard ceiling for hostile input therefore comes from the **out-of-process decode child** (🔨 TO-BUILD), which the OS bounds via `RLIMIT`/hard-kill; in-process Tier-2 on untrusted text bounds soft growth only and is documented as best-effort on the OOM axis until that child ships. Tier-1 holds only one text buffer + one IR. v1 holds no cross-call cache; the IR is freed with the call. No per-feature persistent cache.
- **Accessibility.** Diagnostics carry text `message` + `code` + location (no colour-only signal); the gutter exposes a keyboard path to next/previous problem and to apply a `suggestion`.
- **Packaging.** No new runtime assets; no third-party JSON-Schema dependency beyond the JSON-RPC codec chosen for the agent surface (deferred to [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §11.1). The Tier-2 sandbox reuses the agent subsystem's `ProjectRootJail` (no new dependency). The out-of-process asset-decode child (§3.5 #4) — **required** for the untrusted MCP `validate` crash/OOM guarantee, 🔨 TO-BUILD — is the same RISE binary re-invoked in a locked-down mode (no extra shipped artifact, just a new launch mode + IPC glue). Any new `.cpp`/`.h` (`SceneValidator.{h,cpp}`, `SceneIR.h`, the sandbox glue, and the out-of-process child entry point) goes in **all five** build projects per [CLAUDE.md](../../CLAUDE.md).
- **Migration.** No scene-format change. The string-input parse entry and `apply IR` refactor are **ABI-additive** (new entry points; the existing `ParseAndLoadScene(IJob&)` signature is unchanged). The `ParameterSemantics` structure on `ParameterDescriptor` is owned by [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md)/§16 — this doc consumes it. **No legacy-scene break:** the legacy-tolerant severity policy (§6.5) guarantees no scene that loads today acquires a new `parser_error` (or `validation_policy_error` — barrier commands in a *scene file* are a separate concern from the agent path; a user hand-running a scene with a `> quit` is unaffected, the policy applies to the agent/validate surface); richer checks land as non-gating warnings only.
- **Rollback.** `validate` is read-only and additive; disabling the tool (scope off) removes it with no scene-file impact. The editor gutter (Tier-1) is a default-on toggle. The sandbox is on whenever Tier-2 runs (it cannot be disabled independently of `validate` — a `validate` without the jail/caps would reintroduce the §3.5 attack surface). The `apply IR` refactor lands behind the byte-identity corpus tests so it can be reverted to the direct `Finalize`-calls-`Add*` path without touching scenes.

---

## 9. Non-goals / deliberately NOT doing

- **No capturing `IJob` mock as the validation mechanism.** Rejected in §1 — it is neither cheap nor faithful. The Tier-1 IR path (gutter) and the isolated+sandboxed *real* `Job` (Tier-2 `validate`) replace it, with the full IR split as the end-state.
- **No richer constraint emitted as `error`.** Per the legacy-tolerant policy (§6.5), cardinality / non-integer-`UInt` / closed-enum violations are **warnings**, never `error`s, because the tolerant parser accepts them; promoting any of them to `error` would break existing scenes and is forbidden by the conformance test (§5.4). `error` ≡ "the real parser rejects it," full stop — one consistent definition of validity.
- **No unsandboxed Tier-2 `validate`.** The throwaway `Job` is never run without the §3.5 jail + caps + cancellation; "isolated" alone is insufficient against attacker-supplied filenames/assets. (Tier-1 needs no sandbox — it constructs nothing.)
- **No second confinement for the validator.** The Tier-2 sandbox **reuses** the agent security model's `ProjectRootJail` ([AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) §4.1); validation does not invent a parallel root-jail / path-canonicalizer.
- **No routing validation through `AsciiCommandParser` unmodified.** The barrier-command shim (§2) is mandatory; `exit(1)` must be structurally unreachable from a validate call.
- **No second source of truth for grammar.** The schema and `validate` both derive from `SceneGrammar::Describe()` + the `pipe`/richer-constraint fields; the conformance test (§5.4) keeps them honest rather than a hand-maintained schema.
- **No claim that the generated schema is the parser.** It is a first-pass filter; `validate` is the authority (§5.3).
- **No cross-file / `> load` / `> mediapath` resolution in validation.** Single-text validation only; include-dependent scenes are documented as out-of-scope for a green verdict (§3.4).
- **No new parser dialect.** Validation reuses the existing descriptor dispatch and (end-state) refactors `Finalize`, never forks a parallel parser.
- **No `AI_SECURITY_MODEL.md` content duplicated here.** This doc owns the *validation*-time command policy table (§2) and **consumes** the security model's `ProjectRootJail` (§3.5); the security model ([AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md)) owns the jail's design and points back at §2 for the barrier-command classification. Scope tiers and transport security stay in [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) §7 / [LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md) §7.4.

---

## 10. File-link index

- Umbrella: [GUI_ROADMAP.md](../GUI_ROADMAP.md) (§1 principles, §9 LLM, §15 acceptance, §16 confirmed decisions incl. `pipe` field + `src/Library/Agent/` + Android Tier A)
- Ground truth: [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) (§4 pipe-not-in-descriptor, §12 no side-effect-free parse / `Job` retains no text)
- The `validate` consumer (being corrected separately; this doc supersedes its §6 mechanism + §5.3 "no drift" claim): [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md)
- Shared `ProjectRootJail` + command policy reused by the Tier-2 sandbox (§3.5): [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) (§4.1 root-jail, §5 destructive-command policy that imports this doc's §2 barrier table, §12 jail tests)
- Agent loop / scopes / diff-gate: [LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md) (§7 guardrails)
- Material-editor owner of the `pipe`/richer-constraint field: [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md)
- Parser & descriptor architecture: [../../src/Library/Parsers/README.md](../../src/Library/Parsers/README.md), [../../src/Library/Parsers/ChunkDescriptor.h](../../src/Library/Parsers/ChunkDescriptor.h), [../../src/Library/Parsers/AsciiSceneParser.cpp](../../src/Library/Parsers/AsciiSceneParser.cpp) (dispatch `:697-751`, generic value check `:725`, build-on-finalize `:9862`)
- Command parser (barrier commands): [../../src/Library/Parsers/AsciiCommandParser.cpp](../../src/Library/Parsers/AsciiCommandParser.cpp) (table `:36-51`; `exit(1)` `:182`)
- Scene-parser interface (no parse-only seam): [../../src/Library/Interfaces/ISceneParser.h](../../src/Library/Interfaces/ISceneParser.h) (`:61`)
- The decisive runtime-state check: [../../src/Library/Job.cpp](../../src/Library/Job.cpp) (`ResolveOrDiagnoseScalar` `:2718-2777`)
- Span types reused by the IR: [../../src/Library/SceneEditor/SourceSpanIndex.h](../../src/Library/SceneEditor/SourceSpanIndex.h) (`RawTokenSpan` `:51`, `ParameterSpan` `:62`)
- Source-file build-project checklist: [../../CLAUDE.md](../../CLAUDE.md)
