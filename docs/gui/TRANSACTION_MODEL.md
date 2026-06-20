# RISE Transaction Model — Authoritative State, Published Snapshots, Document Identity, Transactions, Undo Attribution, and Reconciliation

**Status:** DESIGN (no code). The **authoritative** owner of RISE's document / authority / transaction / undo model. Spun off [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §1 / §16 in response to the 2026-06-19 adversarial review, which found that the seven `docs/gui/` specs each *assumed* a state-and-mutation model without any one document owning it — producing two unresolved contradictions (the AI "dispatches to the live controller" **vs** "diffs before mutation"; a "single shared global undo" across in-app and external clients). **Hardened 2026-06-20** after a *second* adversarial review against the four now-CONFIRMED concurrency decisions below; that pass found the v1 concurrency core was not mechanically correct (six mechanical blocking findings + the multi-writer-scope clarification = seven items, §0.1). This revision replaces the lock-free-read / per-move-epoch / revision-only-precondition / serialize-from-introspection mechanisms with: a **single-writer authoritative document**, **RCU-style published immutable snapshots** for all readers, **`(documentUUID, revision)` document identity**, attribution **recorded inside the commit critical section**, an **opaque per-client transaction builder**, and **reload reclassified as a document-level swap** (not an undoable transaction). **The other GUI specs DEFER to this document** for: what counts as authoritative state, what a transaction is, how readers observe a consistent scene, how concurrent clients are reconciled, how AI proposals stage, how undo is attributed, and how external file edits are reconciled.
**Owner:** Aravind Krishnaswamy
**Scope:** The document/authority model for the interactive editor and every client that mutates a loaded scene — in-app GUI (macOS / Windows / Android), the in-process AI agent, and external MCP clients — layered as a *thin extension* of the shipped `SceneEditController` + `SceneEditor` + `EditHistory` + `DirtyTracker` + `SaveEngine`. ABI-additive only: new non-virtual controller methods + C-ABI exports appended to [../../src/Library/RISE_API.h](../../src/Library/RISE_API.h); no edits to existing signatures, no new virtuals on shipped interfaces.
**Honors:** GUI_ROADMAP §1 principles — text is the durable source of truth (#1); maximize shared C++ (#2); Android not left behind (#3); **everything routes through one mutation path** (#6). And §16 decisions (agent subsystem is `src/Library/Agent/`; avoid the bare `MCP` token; `ICredentialStore : IReference`).
**Ground truth:** every code claim is cited `file:line` against master and was re-derived from code per [CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) — plan-doc `Status:` headers were treated as suspect. `.claude/worktrees/` ignored.
**Related (consumers of this spec):** [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md), [LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md), [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md), [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md), [ENTITY_CREATION.md](ENTITY_CREATION.md), [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md), [CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md). Foundation docs: [../INTERACTIVE_EDITOR_PLAN.md](../INTERACTIVE_EDITOR_PLAN.md), [../ROUND_TRIP_SAVE_PLAN.md](../ROUND_TRIP_SAVE_PLAN.md).

---

## 0. Confirmed concurrency decisions (the frame this spec is mechanically correct against)

These were CONFIRMED on 2026-06-20 and are the non-negotiable frame for everything below. The v1 draft of this spec assumed weaker versions of several and was found mechanically incorrect; §0.1 lists the findings each section now closes.

1. **Single-writer authoritative document.** There is exactly one writer of the live `Scene`/`Job` graph at any instant — the `SceneEditController`'s commit path under `mMutex`. v1 is *not* concurrent multi-writer. Every mutation, from every client, is serialized through that one writer (§4, §6).
2. **Drags render in a non-authoritative preview.** A live gizmo / scrub drag mutates **owner-private preview state** that is NEVER published and NEVER snapshotted for other readers. The drag bumps **no revision per pointer-move**; it commits **exactly one transaction** (the existing composite bracket) on pointer-up (§3.3, §4.5).
3. **Render / thumbnail / RMSE / read jobs get an immutable published snapshot.** No background job reads the live graph. Each acquires a ref to an immutable `SceneSnapshot` the writer published; it reads only that snapshot for its whole lifetime (§3, §6.2). This is RCU-style publication, not lock-free reads of live state.
4. **External MCP clients are read + propose-diff only in v1.** An external client may read snapshots and submit a **proposed transaction** (a staged diff); it does NOT mutate the live document directly. The document **owner** (the local GUI user, or an autonomy policy) applies the proposal. Concurrent multi-writer — and the conflict/rebase machinery it needs — is **v2** (§7, §12, §14).
5. **Document identity = `(UUID, revision)`.** A precondition is matched against the *pair*, never the revision alone. A reloaded document gets a **fresh UUID**, so a stale `baseRevision` from a previous load can never match by coincidence (§3.4, §4, §11.4).
6. **One process-wide worker pool.** Render, thumbnail, RMSE, denoise-AOV, and probe jobs draw from a single shared pool, not per-controller threads. Snapshot lifetime (§3.5) is what makes this safe: a pooled worker holds a snapshot ref, so the document it is rendering cannot be freed under it even after the user reloads or closes the document.

### 0.1 Findings this hardening pass closes (second adversarial review, 2026-06-20)

| # | Blocking finding (v1) | Where v1 was wrong | Closed by |
|---|---|---|---|
| **1** | Live-drag epoch (v1 §3.2 E1 / §4.5) implied per-move revision activity and left "two clients, same revision, different state" reachable. | A drag's per-move `Apply`s mutated state the model called authoritative, so a reader at revision N could observe drag-interior state that the committed revision N never contained. | §3.3 (preview state), §4.5 (one commit on pointer-up); E1/E2 rewritten in §3.2. |
| **2** | "Lock-free read of the live scene" (v1 §6.2 M2). | Bumping an atomic epoch does NOT prevent a torn read of non-atomic `Scene` members while the writer is mid-`Apply`. The render thread reads the live graph without holding `mMutex` during a pass (`SceneEditController.cpp:1422-1428` parks it precisely *because* concurrent read is unsafe) — an atomic counter cannot fix that. | §3 (published immutable snapshots, RCU), M2 rewritten in §6.2, §3.6 justification. |
| **3** | Precondition was revision alone (v1 §4 / §11.4). | After reload the epoch lineage restarts; a stale `baseEpoch` could *coincidentally* equal a fresh controller's counter and a stale write would be accepted as in-sync. | §3.4 (`DocumentId = (UUID, revision)`), §4.1/§4.2 (precondition matches the pair), §11.4. |
| **4** | Commit attribution recorded *after* unlocking (v1 §4.2 pseudocode: `record attribution(...)` ran after `unlock`). | Two commits could interleave between unlock and the attribution push, desynchronizing the attribution deque from the edit it describes (and from the revision it claims). | §4.2 (attribution pushed inside the critical section, in lockstep with the revision bump + `Apply`), §8. |
| **5** | Controller-global C-ABI builder `CommitBegin/AppendEdit/CommitEnd` (v1 §12). | The builder kept per-commit state on the controller, so two clients calling `AppendEdit` concurrently would intermix edits into one transaction. | §12 (opaque per-client `TxnBuilder*` handle **or** one-shot whole-payload submit; no controller-global builder state). |
| **6** | External-overwrite fallback = "full re-serialize from introspection"; reload modeled as an undoable `Reload`-origin transaction (v1 §9.2 / §9.3 / §7.5 / §11.4). | Re-serialize from introspection cannot reproduce unsupported chunks, `>` commands, macros, `FOR` loops, or construction-only state the live graph never retained (`Job` keeps byte offsets only — CURRENT_STATE_AUDIT §12) — so it would silently drop them; and reload re-parses into a fresh controller, which has no history slot an "undo" could pop. | §9 (refuse-with-reload-or-Save-As; never silent merge/clobber), §9.3 + §11.4 (reload = document-level swap that destroys controller + history), §13 (true reload-undo needs a real document snapshot, out of v1 scope). |
| **7** | Multi-writer scope was implied "supported" (v1 §6.3 worked an external-client commit as a peer write). | The CONFIRMED frame is single-writer for v1; external clients propose, the owner applies. | §0 #4, §7 (proposed transactions), §14, §13 non-goals. |

---

## 1. Executive summary (read this first)

1. **The authoritative session state is the live in-memory scene** held by `SceneEditController` — specifically the `Job`/`Scene` graph that `SceneEditController` borrows (`SceneEditController.h:233`, ctor takes `IJobPriv&`). It has **exactly one writer** (the commit path under `mMutex`). It is *not* read directly by background jobs — those read **published snapshots** (item 3). The on-disk `.RISEscene` is the **durable checkpoint** (written by `SaveEngine`, `SaveEngine.cpp:863` re-reads it at save; correctness is byte-identity, not a counter). The text editor buffer, AI proposals, drag-in-progress preview state, and sidecars are **non-authoritative** until *committed* through the controller.

2. **`SceneEditor::Apply` is already the only sanctioned mutator** (`SceneEditor.h:4`, "The only sanctioned mutator of a loaded scene"; `SceneEditor.h:91`). This is the existing realization of GUI_ROADMAP principle #6. The transaction layer does **not** add a parallel write path; it *wraps* `Apply`.

3. **Readers observe an immutable snapshot, not the live graph.** The snapshot **mechanism is PROTOTYPED & TESTED** (`feature/gui-snapshot-prototype`; §3.5): `Scene::CreateSnapshot()` returns a ref-counted `SceneSnapshot` that **clones** the document's mutable wrapper state (object transforms + camera pose) and **addref-shares** the immutable leaves, and `tests/SceneSnapshotTest.cpp` proves it is independent of later live mutation (18/18 + negative control). The *publication* model layered on top — a scene **revision** (monotonic, bumped only on a committed transaction) that the single writer uses to atomically *publish* the latest snapshot for readers to acquire (render / thumbnail / RMSE / probe / panel / MCP-resource), RCU-style — is the **target design, NOT yet built** (§3.5.4): the spike has only on-demand `CreateSnapshot()`, no published pointer and no `AcquireSnapshot()`. The clone is what makes the eventual published view safe: it RETRACTS the v1 "lock-free read of the live scene" claim (finding #2) — an atomic epoch bump cannot prevent a torn read of non-atomic `Scene` members mid-`Apply`, but a snapshot a reader holds is never mutated. Today `mSceneEpoch` (`SceneEditController.h:765`) is a *reload sentinel* set once per controller (`SceneEditController.h:391-406`), bumped only on a few structural ops (`SceneEditController.cpp:1594,1633`); this spec adds the per-commit **revision** alongside it (the sentinel stays — item 5 / §3.4), and they are distinct.

4. **Document identity is `(documentUUID, revision)`; a transaction's precondition matches the pair.** A transaction carries a `baseDocument = (UUID, baseRevision)`. The commit is accepted only if `UUID` matches the live document **and** `baseRevision` equals the live revision; otherwise it is **rejected with a conflict** — never a silent clobber. Matching the UUID too (not the revision alone) closes finding #3: after a reload the document has a *fresh UUID*, so a stale precondition from the previous load can never coincidentally match the new revision counter. The transaction *composes the existing `SceneEdit` value-records* (`SceneEdit.h:108`) — one transaction = one or more `SceneEdit`s, exactly the bracket that `CompositeBegin`/`CompositeEnd` (`SceneEdit.h:255-256`) already expresses. A transaction is the existing composite, plus a precondition (the document pair), plus attribution.

5. **A live drag is one transaction, committed on pointer-up, with no per-move revision.** `OnPointerMove` mutates **owner-private preview state** that is never published and never snapshotted; readers cannot observe it (closing finding #1 — "two clients, same revision, different state" is unreachable because drag-interior state is not part of any revision). Only `OnPointerUp` commits the single composite transaction that bumps the revision and publishes one new snapshot (§3.3, §4.5).

6. **AI L1 staging is a *proposed transaction*** — a diff vs a captured `(UUID, baseRevision)` held in a staging area that does **not** touch the live `SceneEditor`. On owner approval it is replayed as a normal transaction with the precondition **re-checked at apply time**. This resolves the review's flagged contradiction (§7). **External MCP clients can ONLY propose in v1** — they never commit directly (§0 #4).

7. **Transactions are the undo unit, and each is attributed** to the client that committed it (in-app / agent / external-MCP-approved). `EditHistory` (`EditHistory.h:26`) already stores `SceneEdit` records on one stack; we add a small per-transaction attribution record alongside it, **pushed inside the same commit critical section** as the revision bump + `Apply` (closing finding #4 — never after unlocking). There is **one** history per controller (one document = one undo timeline), but every entry knows *who* authored it — replacing the review's "single shared global undo with no attribution."

8. **Multi-client reconciliation is single-writer; external clients propose, the owner applies.** v1 is **not** concurrent multi-writer (§0 #4). In-app gestures and approved proposals funnel through the controller's existing `mMutex` + cancel-and-park serialization (`SceneEditController.cpp:1422-1467`). Commits are *totally ordered* by the mutex; *reads* are immutable published snapshots (item 3); a *stale* precondition (whose `(UUID, baseRevision)` lost the race or came from a previous load) is rejected, not merged. Conflict/rebase **across concurrent writers** is v2 (§14).

9. **External-file reconciliation refuses; reload is a document swap, not an undoable transaction.** `SaveEngine` already refuses to save when the on-disk file changed since load (mtime+size guard, `SaveEngine.cpp:827-857`, against `FileIdentity` `SourceSpanIndex.h:133-140`). This spec *promotes* that hard refusal into a **reload-or-Save-As prompt** surfaced before save and on window-focus — never a silent merge or clobber of hand-edits (finding #6). A reload is reclassified as a **document-level swap** that destroys the controller + its history and constructs a new one with a fresh UUID — it is *not* an in-history undoable transaction (re-serialize-from-introspection cannot reproduce unsupported chunks / `>` commands / macros / `FOR` loops / construction-only state; and a re-parse has no history slot to pop). A truly undoable reload needs a real document snapshot and is out of v1 scope (§9.3, §11.4, §13).

10. **The "show me the code" diff derives from a transaction** — the same artifact the L1 review gate shows, computed once in shared C++ against a snapshot (§10).

**One-line ABI posture:** everything below is new *non-virtual* methods on `SceneEditController` / `SceneEditor` and new `RISE_API_*` C exports — additive, out-of-tree-safe per the `abi-preserving-api-evolution` discipline. No shipped signature changes.

---

## 2. Vocabulary (the model in one table)

| Term | Definition | Backed by (code) |
|---|---|---|
| **Authoritative document** | The live in-memory `Scene`/`Job` graph the controller borrows. **One writer** (the commit path); background jobs do NOT read it — they read published snapshots. | `SceneEditController` ctor `IJobPriv&` (`SceneEditController.h:233`) |
| **Durable checkpoint** | The on-disk `.RISEscene`. Updated only by an explicit save; re-read at save time. | `SaveEngine::Save` (`SaveEngine.cpp:863`) |
| **Non-authoritative** | Editor text buffer, AI proposed transaction, import sidecars, **drag-in-progress preview state** — not observed by any reader until *committed*. | new (§3.3, §7); editor buffer is platform UI |
| **Revision** | Monotonic `uint64` on the controller; bumped **once per committed transaction**, under `mMutex`, in lockstep with snapshot publication. NOT bumped per pointer-move. | new `mRevision` (§3.2), distinct from `mSceneEpoch` |
| **Reload sentinel** | The *existing* `mSceneEpoch` — a per-controller-construction unique value used only to detect that the controller was torn down + rebuilt (reload). KEPT SEPARATE from the revision. | `mSceneEpoch` (`SceneEditController.h:765`, `.cpp:102-105,125`) |
| **DocumentId** | `(documentUUID, revision)`. The UUID is minted once per controller construction (fresh on every reload); the revision is the per-commit counter. Preconditions match the **pair**. | new (§3.4) |
| **SceneSnapshot** | A ref-counted, immutable capture of the document's *mutable wrapper state* (object transforms + camera pose) that **clones** that state and **addref-shares** the immutable leaves — provably independent of later live mutation. *Prototyped* (clone + read accessors); the *published-per-revision* layer (acquire/restore) is not yet built. | **implemented** in `Scene.cpp:862` (`SceneSnapshot`) + `Scene.cpp:927` (`CreateSnapshot`) + `Object.cpp:103` (`CloneSnapshot`); §3.5 |
| **Published snapshot** | The single snapshot pointer the writer last installed; readers atomically load-and-ref it. RCU-style: a new publish does not disturb readers already holding an older snapshot. | new (§3, §3.5) |
| **Preview state** | Owner-private, drag-interior scene state mutated by `OnPointerMove`; never published, never snapshotted. Discarded or folded into the one pointer-up commit. | new (§3.3, §4.5) |
| **Transaction** | An ordered group of `SceneEdit`s applied atomically, carrying a `baseDocument = (UUID, baseRevision)` precondition + attribution + label. One transaction = one undo unit. | composes `SceneEdit` (`SceneEdit.h:108`) + composite bracket (`SceneEdit.h:255`) |
| **Commit** | Apply a transaction to the live document under `mMutex`, after the precondition passes; bumps the revision **and** publishes a snapshot **and** records attribution — all in one critical section. | wraps `SceneEditor::Apply` (`SceneEditor.h:91`) |
| **Conflict** | `baseDocument` doesn't match the live `(UUID, revision)` at commit time → reject; state untouched. | new (§4) |
| **Proposed transaction** | A transaction captured against a `baseDocument` but NOT committed (AI L1, **all external-MCP edits in v1**, batch preview). Lives in staging; applied by the *owner* on approval. | new (§7) |
| **Attribution** | Who authored a committed transaction: `InApp`, `Agent`, `ExternalMCP`. (No `Reload` — reload is not a transaction; §9.3.) | new field (§8) |
| **Client** | A mutation *source* bound to the controller: the GUI shell, the in-process agent, or an external MCP connection. In v1 only `InApp` (and an autonomy policy acting for `Agent`) commits; external clients propose. | new `ClientId` (§6.1) |
| **Worker pool** | One process-wide pool serving render / thumbnail / RMSE / probe / denoise-AOV jobs. Each job holds a `SceneSnapshot` ref for its lifetime. | new (§0 #6, §3.5, §6.4) |
| **Reconciliation (file)** | Resolving divergence between the live document and a `.RISEscene` changed on disk under us — **refuse with reload-or-Save-As**, never merge/clobber. | promotes `FileIdentity` guard (`SaveEngine.cpp:827-857`) |
| **Reload (document swap)** | Destroy the controller + its history and construct a new one (fresh UUID) by re-parsing the file. NOT an undoable transaction. | `LoadAsciiScene` re-parse (CURRENT_STATE_AUDIT §3, §12) |

---

## 3. Authoritative document, published immutable snapshots, and document identity

### 3.1 What is authoritative, what is not

```
        DURABLE                AUTHORITATIVE (one writer)        NON-AUTHORITATIVE
   .RISEscene on disk   ──►   live Scene/Job graph         ◄──   editor text buffer
   (SaveEngine ckpt)          (SceneEditController, mMutex)       AI proposed txn
         ▲                          │  │                          drag preview state
         │ save (explicit)          │  │ commit (one writer):     import sidecars
         └──────────────────────────┘  │   Apply → ++revision → publish snapshot
                                        │
                                        ▼  publish (RCU)
                              ┌───────────────────────────┐
                              │  PUBLISHED SceneSnapshot    │  ◄── readers ref this:
                              │  immutable @ revision N     │      render / thumbnail /
                              │  (refs immutable resources) │      RMSE / probe / panel /
                              └───────────────────────────┘      MCP rise://scene/* read
```

- **Authoritative = the borrowed `IJobPriv&`, written by exactly one thread.** All mutation funnels through the commit path under `mMutex`. **No background job reads it.** Renderers (interactive + production), thumbnailers, RMSE jobs, the auto-router probe, every introspection bundle, every panel read, every MCP `rise://scene/*` resource, and the AI's scene context all read a **published snapshot** (§3.3–§3.5), not the live graph. There is exactly one live document per open scene.
- **Durable = disk.** Only `RequestSave` (`SceneEditController.h:483`) advances it. Crucially, the `Job` does **not** retain source text (`CURRENT_STATE_AUDIT.md` §12; `Job.h:116` holds only byte-offset `SourceSpanIndex`) — so "the file" and "the live state" are genuinely two stores that can diverge, which is *why* §9 reconciliation is mandatory and *why* reload cannot be a re-serialize (§9.3).
- **Non-authoritative is invisible to readers.** The text editor buffer is a platform widget; an AI proposed transaction is a value object in `src/Library/Agent/`; drag preview state (§3.3) is owner-private. None is seen by any reader until it becomes a *committed transaction* and the writer publishes the resulting snapshot.

### 3.2 The revision (distinct from the reload sentinel)

The shipped `mSceneEpoch` (`SceneEditController.h:765`, ctor `.cpp:125`) is a **reload sentinel**: "set ONCE … unique per controller … NOT bumped on mid-session structural mutations" (`SceneEditController.h:391-406`). Two call sites bump it on selection/clone/undo (`SceneEditController.cpp:1594,1633`) for list-rebuild signalling. **We do NOT overload it into a revision** — the v1 draft did, and it conflated two roles with different lifetimes (the sentinel must stay *non-comparable across reloads*; the revision must be *strictly monotonic within one document*). Instead we add a separate per-commit revision:

```cpp
// SceneEditController.h — additive, non-virtual
std::atomic<uint64_t>  mRevision;          // new: per-commit, monotonic within this document
uint64_t  CurrentRevision() const;         // the live revision (== published snapshot's revision)
```

> **Invariant E1 — Revision advances iff authoritative state changed, and always with a publish.** Every committed transaction does, *inside one `mMutex` critical section, in this order*: (a) `Apply` all edits; (b) `++mRevision`; (c) publish a fresh immutable snapshot stamped with the new revision; (d) push the attribution record (§8). Steps (b)–(d) are inseparable — there is never a published revision without its snapshot, nor a snapshot without its attribution. No path other than commit mutates `mRevision`. (Reload constructs a fresh controller with `mRevision = 0` and a fresh UUID — §3.4 — so cross-reload revisions are never compared by value; the UUID guards that, §4.)

> **Invariant E2 — Every read carries the snapshot's `DocumentId`.** A reader does not "tag a read with an epoch" after the fact (the v1 mechanism, which had a window between reading state and reading the counter). Instead it acquires a `SceneSnapshot` whose `DocumentId() == (UUID, revision)` is *intrinsic and immutable*. `RefreshProperties`, introspection bundles, and the MCP `rise://scene/*` payloads all read from a held snapshot and report `snapshot->DocumentId()`. A client that read snapshot `(U, N)` and writes with `baseDocument = (U, N)` gets last-writer-protection for free, with no torn-read window.

### 3.3 Drag preview state (owner-private, never published)

A live gizmo drag or timeline scrub needs to *show* intermediate frames without making each pointer-move a revision. The model:

- `OnPointerDown` captures `baseDocument = (UUID, CurrentRevision())` and opens the existing composite bracket (`SceneEditor::BeginComposite`, `SceneEditor.h:103`).
- `OnPointerMove` applies edits to the live graph **for the owner's own interactive preview only**, under the existing per-move cancel-and-park (`SceneEditController.cpp:1309-1323` object-motion path; `:1422-1428` time-scrub path). The interactive rasterizer renders these frames into the **owner's** preview sink — but the writer does **not** bump `mRevision` and does **not** publish a snapshot during the drag. No other reader (thumbnail, RMSE, external MCP, a second panel) ever acquires a snapshot mid-drag, so none can observe drag-interior state. This is what makes "two clients, same revision, different state" **unreachable** (finding #1): drag-interior state belongs to no revision.
- `OnPointerUp` closes the composite and performs the *single* commit (E1): the whole drag becomes one transaction, one revision bump, one published snapshot, one undo entry.

> **Why preview mutation of the live graph is safe.** The drag's per-move `Apply`s run under the SAME cancel-and-park that already serializes the writer against the interactive render thread (the only other consumer during a drag). Background pooled jobs (§3.5) read *snapshots*, which are not republished until pointer-up, so they never see the half-applied drag. The owner's interactive preview is *expected* to see it — that is the point of a live drag. (A future fully-isolated preview that clones into a scratch document is possible but unnecessary for v1's single-writer model and is noted as a v2 refinement in §14.)

### 3.4 Document identity — `(documentUUID, revision)`

> **Production layer — NOT in the spike.** `DocumentId`, the per-commit `mRevision`, and the document UUID are part of the publication layer §3.5.4 still has to build. The prototyped `SceneSnapshot` (§3.5) carries **no** `DocumentId` today. The design below is the target.

```cpp
// SceneTransaction.h (new header, §12) — value type, trivially copyable; NOT YET IMPLEMENTED
struct DocumentId {
  uint64_t  uuidHi;       ///< 128-bit document UUID, minted once per controller construction
  uint64_t  uuidLo;
  uint64_t  revision;     ///< per-commit counter within this document
  bool operator==( const DocumentId& o ) const
  { return uuidHi==o.uuidHi && uuidLo==o.uuidLo && revision==o.revision; }
};
```

- The **UUID** is minted in the `SceneEditController` constructor (next to the existing `mSceneEpoch` seed, `SceneEditController.cpp:125`) and never changes for the life of that controller. A reload destroys the controller and builds a new one → **new UUID** (§11.4). The reload sentinel `mSceneEpoch` continues to exist and serve its list-rebuild role; the UUID is the *durable, collision-free* identity that preconditions match.
- The **revision** is `mRevision` (§3.2).
- **Preconditions match the whole `DocumentId`'s `(uuid, revision)`** (§4). Matching the UUID is what closes finding #3: a `baseRevision` carried over from a previous load can never satisfy a fresh controller's precondition, because the UUID differs even if the revision counters coincide. The UUID also lets a client *detect* a reload (its held `DocumentId.uuid` no longer matches the controller's) and re-bootstrap, rather than silently issuing writes against a different document.

> **Why a UUID rather than reusing the reload sentinel as the high bits?** The sentinel is a 32-bit process-local counter (`SceneEditController.h:765`) that wraps and is reused across the process's lifetime; an external MCP client that reconnects after the host restarted could hold a sentinel value the new process re-issues. A 128-bit UUID is collision-free across processes and restarts, which matters precisely because external clients are out-of-process (§6.1). The sentinel stays for its in-process list-rebuild signalling; the UUID is the cross-process identity.

### 3.5 SceneSnapshot — implemented clone design, resource lifetime, and invalidation

> **Status: PROTOTYPED & TESTED.** A real spike landed on branch `feature/gui-snapshot-prototype`. The snapshot mechanism in this section — clone-the-mutable-wrapper-state, addref-the-immutable-leaves — is implemented and proven independent of later live mutation by [`tests/SceneSnapshotTest.cpp`](../../tests/SceneSnapshotTest.cpp) (**18/18 pass**, including a negative control). The *publication* layer (RCU-style published-pointer, `AcquireSnapshot()`, and the **restore/swap** path) is NOT yet built — see §3.5.4 and the prototype-scope-vs-production-gaps note (§3.7). The v1 paper design (a thin `IReference` handle that *only* addrefs resources, with no clone) is **superseded**: addref preserves lifetime, not state, and the editor mutates the very instances it would have addref'd, so a pure-addref snapshot would observe the mutation. The spike chose the clone instead, and the test proves the choice.
>
> **⚠ 4th code-backed review (2026-06-20) — the spike's leaf handling is itself buggy; do NOT read "PROTOTYPED & TESTED" as "foundation done."** The test exercises **null material/shader bindings**, so it never touched these P1 defects: **(1)** the code addref-shares material/shader/medium as "immutable leaves," but the editor **mutates them in place** (`SetMaterialProperty`, Phase B; shared shader runtime-cache reset on transform commit, `SceneEditor.cpp:765`) — a real snapshot would observe material/shader edits and risks a shared-cache race/UAF vs a concurrent snapshot render. **Material/shader/medium must be CLONED, not addref'd** (the *confirmed* form of the "leaf-immutability audit" flagged in §3.7 — not hypothetical). **(2)** `Object::CloneSnapshot()` is **non-virtual/concrete**, so a `CSGObject` (derives from `Object`) is **sliced** — operands lost, null geometry; needs a **virtual / per-type clone**. **(3)** `SceneSnapshot` is **publicly mutable** (`AddClonedObject`/`SetCameraPose`) and captures **no lights/luminaries/film/env/global-medium/camera-collection**. **(4)** the cost below is a single cold number (~165–174 µs); the warm / per-100-object figures are **estimates, not measured**. **UPDATE — increments A + B since landed & independently verified:** defects (1)–(3) are FIXED (material/medium **cloned** via `SnapshotLeafClone`; `CloneSnapshot` made **virtual** + a `CSGObject` override; snapshot **frozen**; lights/film/environment/active-camera now captured render-faithfully, camera per-type not pose-only), and (4) is now measured (~48 µs warm / 100 objects + lights + film + camera). `tests/SceneSnapshotTest.cpp` = **97/0** (break-it-first material/CSG/light/thin-lens/scene-complete). **Bounded residuals remain:** SSS-shader + object interior-medium deep clone (a real `ResetRuntimeData`-vs-`Shade` UAF), ONB-constructed cameras (refused → clone returns null), heterogeneous global medium (addref'd), out-of-tree types (addref fallback); restore/publish is #2. See [GUI_ROADMAP.md](../GUI_ROADMAP.md) §13a #1.

**The implemented design — clone the mutable wrapper, share the immutable leaves.** RISE's scene resources fall into two classes, and the snapshot treats them differently:

- **Immutable leaves** — geometry, material, modifier, shader, radiance map, interior medium, UV generator. These are already reference-counted (`Reference.h`, `IReference`) and read-only during a render. The snapshot **addref-shares** them (no copy).
- **Mutable wrapper state** — the per-object transform building blocks and finalized matrices, and the active camera pose. The editor mutates *these* in place (a `TranslateObject` + finalize rewrites the object's matrices). The snapshot **deep-copies** them so a later live mutation cannot bleed in.

`Object::CloneSnapshot()` ([`Object.cpp:103-146`](../../src/Library/Objects/Object.cpp); declared `Object.h:95`) is the per-object realization. It is a **concrete method, NOT a new interface virtual** — so the spike adds **no vtable / ABI change** to `IObject`/`IObjectPriv` (see the note at `Object.h:72-95`). It:

```cpp
// Object.cpp:103 — abridged; full source in-tree
Object* Object::CloneSnapshot() const {
  Object* pClone = new Object( pGeometry );          // ctor addrefs the geometry leaf
  // immutable leaves — addref-share via the Assign* setters:
  if( pMaterial )       pClone->AssignMaterial( *pMaterial );      // Object.cpp:112
  if( pModifier )       pClone->AssignModifier( *pModifier );
  if( pShader )         pClone->AssignShader( *pShader );
  if( pRadianceMap )    pClone->AssignRadianceMap( *pRadianceMap );
  if( pInteriorMedium ) pClone->AssignInteriorMedium( *pInteriorMedium );
  if( pUVGenerator )    pClone->SetUVGenerator( *pUVGenerator );
  // cheap value flags (visibility / shadows / eps / tangent sign):  Object.cpp:120-124
  // mutable transform BUILDING BLOCKS — deep copy (this is what makes it independent):
  pClone->m_mxPosition     = m_mxPosition;           // Object.cpp:133
  pClone->m_mxOrientation  = m_mxOrientation;
  pClone->m_mxScale        = m_mxScale;
  pClone->m_mxStretch      = m_mxStretch;
  pClone->m_transformstack = m_transformstack;       // std::deque<Matrix4> value copy (Object.cpp:137)
  // finalized matrices — copy so the clone is render-ready without re-finalize:
  pClone->m_mxFinalTrans    = m_mxFinalTrans;        // Object.cpp:141
  pClone->m_mxInvFinalTrans = m_mxInvFinalTrans;
  pClone->m_mxInvTranspose  = m_mxInvTranspose;
  return pClone;                                      // returned with refcount 1
}
```

The deep-copied members are exactly the `Transformable` protected state (`Transformable.h:32-41`: `m_mxFinalTrans`, `m_mxInvFinalTrans`, the `m_transformstack` deque, and `m_mxPosition/Orientation/Scale/Stretch`). Copying the *finalized* matrices too means the clone reflects the live pose **at snapshot time** and is render-ready without a re-finalize. (`Object::CloneFull()` is unsuitable: it re-runs the `Assign*` setters but copies *none* of the transform state, so a `CloneFull`'d object starts at identity — `Object.h:90-94`.)

**`SceneSnapshot`** ([`Scene.cpp:862-925`](../../src/Library/Scene.cpp); declared `Scene.h:51-75`) is the container. It lives **inside `Scene.cpp` — there is no new source file** (so the spike does not touch the five build projects). It is a `Reference` subclass holding the owned cloned objects, their parallel names, and the camera pose:

```cpp
// Scene.h:51 — the real class (spike scope)
class SceneSnapshot : public virtual Reference {
protected:
  std::vector<Object*>      clonedObjects;   //!< owned (each held with refcount 1)
  std::vector<String>       objectNames;     //!< parallel to clonedObjects
  CameraPoseSnapshot*       cameraPose;      //!< owned; null if no active camera
  String                    activeCameraName;
public:
  size_t   GetObjectCount() const;
  Matrix4  GetObjectFinalTransform( size_t index ) const;   // Scene.cpp:895
  String   GetObjectName( size_t index ) const;
  bool     HasCamera() const;
  Point3   GetCameraPosition() const;        //!< stored (rest) location  (Scene.cpp:911)
  Point3   GetCameraLookAt() const;
};
```

The destructor (`Scene.cpp:868-878`) drops one ref per cloned object (`AddClonedObject` took ownership of the clone's single reference without an extra addref — `Scene.cpp:880-885`) and deletes the camera pose. The read accessors are the surface the decisive test reads directly. **Note this is the *spike* surface** — `GetObjectFinalTransform(index)` / `GetCameraPosition()`, NOT the production `Id()` / `Scene()` / typed-introspection surface the v1 paper design sketched. The production accessors (an immutable `IScene` view + intrinsic `DocumentId`) are §3.5.4 work.

**`Scene::CreateSnapshot()`** ([`Scene.cpp:927-972`](../../src/Library/Scene.cpp); declared `Scene.h:200-204`) is the entry point. It enumerates objects via `IObjectManager::EnumerateItemNames` (`Scene.cpp:959`), `dynamic_cast`s each live `IObjectPriv` to the concrete `Object` (`Scene.cpp:947` — the same downcast pattern `Scene::ResyncCamerasToFilmDims` uses for `CameraCommon`), calls `CloneSnapshot()`, and records a stable name per clone. It then captures the active camera's pose by value via `CameraCommon::CaptureSnapshot()` (`Scene.cpp:967`). The caller owns the returned `SceneSnapshot` and `release()`s it.

**Camera capture is by value (pose only).** `CameraPoseSnapshot` (`CameraCommon.h:34-43`) is a plain value struct — `position` (rest location), `lookAt`, `up`, euler `orientation`, `targetOrientation`, and a `fromONB` honesty flag. `CameraCommon::CaptureSnapshot()` (`CameraCommon.h:171-181`) is a concrete inline method (no ABI change) that copies those fields. This is a deliberate spike shortcut and a **production gap**: pose-by-value loses thin-lens/orthographic projection parameters, and ONB-constructed cameras can't be rebuilt through the non-ONB factory — see §3.7.

**Lifetime / freeing.** A snapshot owns its clones and is freed when the last holder `release()`s it. The cloned *wrapper* objects are destroyed with it; the *shared immutable leaves* (geometry/painters/…) keep their own refcounts and outlive the snapshot exactly as long as anything else references them. This is the property that, once the publication layer exists (§3.5.4), will make one process-wide worker pool safe: a job holding a snapshot ref keeps the wrapper state it reads alive, and the leaves it shares stay alive by their own refcounts — even across ten subsequent commits or a reload. (That cross-job lifetime guarantee is a *consequence* the production layer will rely on; the spike proves only the per-snapshot independence, not the held-across-commits-by-a-pooled-job path.)

**Invalidation = "stale," not "freed" (production semantics, not yet built).** In the production model, publishing a newer snapshot will not free an older one; it only changes which snapshot a new `AcquireSnapshot()` returns, and an in-flight job runs to completion against the snapshot it holds (its result then reconciled against the live revision by the render coordinator — the snapshot's revision is the token it compares; §6.4, [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md)). The spike has no published-pointer and no `AcquireSnapshot()`, so this paragraph describes the *target*, not current code.

#### 3.5.1 Measured cost

The spike instruments `CreateSnapshot()` (`SceneSnapshotTest.cpp:128-133`, `:217-218`):

- **≈ 3.5–4 µs warm** for a 2-object scene (the test's measured `[cost]` line).
- By the design analysis (the cost is dominated by per-object clone work — a handful of `addref`s on the leaves + a `std::deque<Matrix4>` value copy of the transform stack + the matrix copies), a full scene scales at roughly **~2–5 ms per 100 objects**. For a *render-faithful* snapshot the dominant cost is **not** the clone but the **TLAS rebuild** that the **restore** path will incur (re-running `Job::SetPrimaryAcceleration`'s top-level BVH4 build — CLAUDE.md / `ARCHITECTURE.md` "Top-Level Acceleration") plus a `LightSampler` rebuild (§3.7); the spike does neither, which is why it measures only the µs-scale clone.

> **The v1 "`O(handles)` addrefs / sub-µs" cost claim is DELETED.** It described a pure-addref thin handle that was never implemented and could never have been correct (a pure-addref snapshot fails the independence test — that is the whole point of the negative control). The measured numbers above replace it.

#### 3.5.2 The decisive test (and its negative control)

[`tests/SceneSnapshotTest.cpp`](../../tests/SceneSnapshotTest.cpp) builds a 2-object + pinhole-camera scene the same way the `SceneEditor` tests do (`Job` + `AddSphereGeometry` + `AddObject` + `RISE_API_CreatePinholeCamera`), then:

1. Takes a snapshot and reads pre-mutation values out of it (object "alpha" at translation `(1,2,3)`; camera at `(0,0,5)`).
2. **Mutates the live scene** after the snapshot: `liveAlpha->TranslateObject((10,0,0))` + `FinalizeTransformations()` (mirroring `SceneEditor`'s `TranslateObject` + `RunObjectInvariantChain`), and moves the live camera via `SetLocation` + `RegenerateData`.
3. **Asserts the snapshot is unchanged** — alpha still reads `1`, not `11`; camera still `(0,0,5)`, not `(99,..)` — i.e. the snapshot now *differs* from live (`SceneSnapshotTest.cpp:188-207`).
4. **Negative control (the payoff):** a bare `addref`'d handle to the *same* live object (`SceneSnapshotTest.cpp:157-164`, `:209-214`) **does** observe the mutation (reads `11`). This is the mechanical proof that addref-alone is insufficient and the clone is required — exactly the v1 design's flaw, demonstrated.

Result: **18/18 pass**. This validates the snapshot *mechanism* (independence of mutable state from later live mutation). It does **not** exercise restore/publish or render-faithful camera cloning (§3.5.4, §3.7).

#### 3.5.3 Implemented surface (spike) — files touched

| Symbol | File:line | Role |
|---|---|---|
| `Object::CloneSnapshot()` | `Object.cpp:103`, decl `Object.h:95` | per-object clone; concrete, no new virtual |
| `SceneSnapshot` | `Scene.cpp:862-925`, decl `Scene.h:51-75` | container; **in `Scene.cpp`, no new file** |
| `Scene::CreateSnapshot()` | `Scene.cpp:927-972`, decl `Scene.h:200-204` | entry point; enumerate → downcast → clone → capture camera pose |
| `CameraPoseSnapshot` / `CaptureSnapshot()` | `CameraCommon.h:34-43`, `:171-181` | camera pose by value; concrete, no new virtual |
| `SceneSnapshotTest` | `tests/SceneSnapshotTest.cpp` | decisive test + negative control (18/18) |

Because the spike adds **no new source file** (`SceneSnapshot` lives in `Scene.cpp`) and **no new interface virtual**, it required **no edits to the five build projects** and incurs **no ABI cost**. (The production layer in §3.5.4 *will* add `SceneTransaction.h` and may add a published-snapshot header; those follow the CLAUDE.md five-project rule — §12.)

#### 3.5.4 What the production layer still adds (NOT in the spike)

The spike proves independence; turning it into the render-faithful, publishable snapshot the rest of this spec assumes still needs:

- **A published-pointer + `AcquireSnapshot()` seam.** The single writer publishes the latest snapshot at commit; readers atomically load-and-ref it. The spike has only `CreateSnapshot()` (an on-demand clone), no published pointer, no acquire. The intended controller surface (additive, non-virtual):
  ```cpp
  // SceneEditController.h — additive, non-virtual (NOT YET IMPLEMENTED)
  RISE::SceneSnapshot* AcquireSnapshot() const;   ///< addref'd; caller Release()s; the published revision
  DocumentId           CurrentDocumentId() const; ///< (uuid, CurrentRevision())
  ```
- **An intrinsic `DocumentId` on the snapshot** (`Id()`), so a reader's view carries `(UUID, revision)` with no torn-read window (E2). The spike's `SceneSnapshot` has no `DocumentId`.
- **A production read surface** — an immutable `IScene` view (`Scene()`) and typed introspection accessors — rather than the spike's `GetObjectFinalTransform`/`GetCameraPosition` probes.
- **The restore/publish (swap) path** — see §3.7 and working-copy item #2 (§16 of [GUI_ROADMAP](../GUI_ROADMAP.md) / this doc's transaction-atomicity discussion): swapping a snapshot/working-copy back in as the live scene must **rebuild the TLAS and the `LightSampler`** (neither is captured or restored by the spike).

### 3.6 Why clone-the-mutable-state (vs pure-addref, deep-clone-everything, or seqlock)

The single-writer / many-reader shape, plus the fact that the editor mutates per-object *wrapper* state in place, makes "clone the mutable wrapper, share the immutable leaves" the right tool — and the spike's negative control is the empirical proof:

- **vs pure-addref-the-live-instances (the v1 paper design, FALSIFIED).** Addref preserves an object's *lifetime*, not its *state*. The editor mutates the exact `IObjectPriv` the manager hands out (object transforms) and the exact `ICamera` behind `GetCameraMutable()` — the very instances a pure-addref snapshot would have aliased. `SceneSnapshotTest.cpp`'s negative control demonstrates this directly: a bare-addref handle reads `11` after the live mutation, while the cloned snapshot still reads `1`. Cloning the mutable wrapper is the *minimum* that makes a snapshot independent.
- **vs deep-clone-everything (geometry + BVH + painters).** Cloning the immutable leaves too would be `O(scene size)` per snapshot and would dominate thumbnail/RMSE/probe throughput on Sponza-class scenes for no benefit — those leaves are read-only during a render and are already refcounted. The spike copies *only* the small mutable wrapper (transform matrices + the transform-stack deque + the camera pose) and shares everything heavy by refcount. This is why the measured clone is µs-scale (§3.5.1).
- **vs seqlock.** A seqlock lets readers retry on a concurrent write, which works only for a short, bounded, restartable critical section. A render pass is neither short nor restartable — a denoise-AOV pass that ran 200 ms cannot "retry because the writer bumped the sequence." A reader needs a *stable* view for an unbounded duration, which a held cloned snapshot gives and a seqlock cannot.
- **vs a global reader-writer lock.** An `RWLock` held for a whole render pass blocks every commit for the pass duration — interactivity dies. A published snapshot lets the writer commit while long readers run against the snapshot they hold (this is the *production* payoff §3.5.4 enables; the spike alone does not yet publish).

The clone's one cost — duplicating the mutable wrapper state per snapshot — is bounded and small: it is the per-object transform matrices + the `m_transformstack` deque + the camera pose, **not** the scene's geometry/BVH/painters. Peak extra RSS from holding older snapshots is proportional to (concurrently-held snapshots) × (per-object wrapper size × object count), not to scene size × reader count.

### 3.7 Prototype scope vs production gaps (be honest about what the spike does NOT do)

The spike (§3.5) **validates the snapshot mechanism** — that a cloned snapshot is independent of later live mutation. It is *not* a render-faithful, publishable, restorable snapshot. Four explicit gaps remain before the rest of this spec is grounded in code rather than design:

1. **Camera is captured by value (pose only) — a render-faithful snapshot needs a per-camera-type clone.** `Scene::CreateSnapshot` captures `position / lookAt / up / orientation / targetOrientation` via `CameraCommon::CaptureSnapshot()` (`CameraCommon.h:171-181`). RISE cameras are polymorphic (pinhole / **thin-lens** / **orthographic** / ONB). Pose-by-value **loses** thin-lens parameters (aperture, focal distance) and orthographic projection extents — two cameras with identical poses but different lens models would snapshot identically, so a render off the snapshot would be *wrong*. And **ONB-constructed cameras cannot be rebuilt through the non-ONB factory** (`CameraCommon::IsFromONB()`, `CameraCommon.h:128-135`) — the `fromONB` flag is carried in the pose struct (`CameraPoseSnapshot::fromONB`, `CameraCommon.h:41`) precisely as an *honesty marker* that the pose-only path is insufficient for those cameras. Production needs a **per-camera-type clone** (each concrete camera clones its own projection params, or rebuilds via the matching factory honoring `fromONB`), not a single pose struct.
2. **Restore must rebuild the TLAS and the `LightSampler` — the spike builds neither.** The spike's `SceneSnapshot` captures object wrapper state + camera pose and exposes *read* accessors; it has **no** restore/swap-back path. Swapping a snapshot (or a working copy, §16/working-copy item #2) back in as the live scene changes object transforms and bindings, which means the top-level acceleration is stale: restore must re-run `Job::SetPrimaryAcceleration`'s top-level BVH4 build (CLAUDE.md / `ARCHITECTURE.md` "Top-Level Acceleration (TLAS)"). It must **also** rebuild the `LightSampler`, which `RayCaster::AttachScene` constructs and `Prepare()`s from the scene's luminaries (`RayCaster.cpp:160-171`) — a snapshot that changed emissive bindings or light transforms invalidates the alias/BVH light tables. **This TLAS + `LightSampler` rebuild is the dominant cost of restore** (§3.5.1), and none of it exists in the spike.
3. **`CreateSnapshot` must run under cancel-and-park, not concurrently with a render.** The spike calls `CreateSnapshot()` on a quiescent scene in a unit test. In the editor it walks the live object manager and `dynamic_cast`s + clones each live `Object` (`Scene.cpp:939-960`) — reading live wrapper state. That read must be **serialized against the writer / render thread** via the existing cancel-and-park (`SceneEditController.cpp:1422-1428`), exactly as every mutation is. Calling `CreateSnapshot()` concurrently with an in-flight render (or a mid-`Apply` commit) would read torn wrapper state. The spike does not address this; the production seam (§3.5.4) must take the snapshot inside the parked critical section.
4. **The leaf-immutability assumption must be audited.** `CloneSnapshot` addref-shares geometry, painters, materials, etc. (`Object.cpp:111-117`) on the assumption that those leaves are immutable for the snapshot's lifetime. That holds today for static scenes — but a **painter keyframe** (animated material parameter) or any feature that mutates a leaf *in place* would make a shared leaf change under the snapshot, defeating independence for that leaf. Before relying on the snapshot for animated/keyframed content, audit every leaf type for in-place mutation; any mutable leaf must be **cloned too**, not shared. (The transform path is already safe — it is exactly the mutable wrapper the spike clones.)

These four are why the spike is labeled "validates the mechanism," not "implements snapshots." Items 1–2 are the render-faithfulness blockers; items 3–4 are the correctness-under-the-editor blockers.

---

## 4. Transactions

### 4.1 The transaction object

A transaction is the existing composite bracket made first-class, with a precondition and provenance. It is **not** a new mutation mechanism — `Apply` still does the work.

```cpp
// New header: src/Library/SceneEditor/SceneTransaction.h
namespace RISE {

  enum class EditOrigin : int {        // C-ABI surface — append only, don't reorder
    InApp       = 0,   ///< a platform GUI shell (gizmo, panel, menu)
    Agent       = 1,   ///< the in-process AI agent (src/Library/Agent/)
    ExternalMCP = 2    ///< an external MCP client (propose-only in v1; the OWNER commits, §7)
    // NOTE: there is intentionally NO `Reload` origin.  A reload is a
    // document-level swap (§9.3, §11.4), not a committed transaction —
    // it has no SceneEdit list and no history slot.  (Finding #6.)
  };

  enum class CommitStatus : int {      // C-ABI surface
    Committed      = 0,   ///< applied; revision advanced; snapshot published; returned newRevision
    Conflict       = 1,   ///< baseDocument != live (uuid,revision) — rejected, state untouched
    Rejected       = 2,   ///< an inner SceneEdit failed Apply (unknown entity, read-only, composed material)
    NoOp           = 3    ///< empty / all-no-op transaction; revision unchanged, no new snapshot
  };

  struct SceneTransaction {
    DocumentId               baseDocument; ///< precondition: must equal live (uuid, revision) at commit
    EditOrigin               origin;
    uint32_t                 clientId;     ///< which bound client (0 = the local GUI shell)
    std::string              label;        ///< undo-menu label ("Move cube", "AI: warm key light")
    std::vector<SceneEdit>   edits;        ///< the ordered ops (reuses the shipped value-record)
  };

  struct CommitResult {
    CommitStatus  status;
    DocumentId    newDocument;     ///< (uuid, revision) after commit (== baseDocument on Conflict/NoOp)
    DocumentId    currentDocument; ///< on Conflict, the live (uuid, revision) the loser must rebase onto
                                   ///<   (a UUID mismatch here means "the document was reloaded")
    int           failedEditIndex; ///< on Rejected, which edit failed (-1 otherwise)
    std::string   message;         ///< human-readable reason (Conflict / Rejected)
  };
}
```

`SceneEdit` is trivially-copyable by design (`SceneEdit.h:7`, "trivially copyable so it composes into ring buffers and history stacks") so `std::vector<SceneEdit>` is cheap to move across the C-ABI marshaling boundary. `DocumentId` (§3.4) is likewise trivially-copyable.

### 4.2 Commit path — wraps the shipped machinery; publishes + attributes inside the lock

`SceneEditController::Commit(const SceneTransaction&)` is the single new entry point. It reuses the existing cancel-and-park sequence verbatim (the one `SetProperty`/`Undo`/`Redo`/clone already use, `SceneEditController.cpp:1422-1467`). The critical correctness point (finding #4): the revision bump, snapshot publication, **and** attribution push all happen **inside the same `mMutex` critical section** as `Apply` — never after unlocking.

```
Commit(txn):
  lock mMutex
    if mRendering: mCancelProgress.RequestCancel()
    cv.wait until !mRendering              # park the render thread (existing pattern)
    if txn.baseDocument != (mUuid, mRevision):   # ── PRECONDITION: match the PAIR (§3.4)
        # UUID mismatch ⇒ reloaded under the client; revision mismatch ⇒ lost the race.
        unlock
        return { Conflict, newDocument=txn.baseDocument,
                 currentDocument=(mUuid, mRevision), … }
    editor.BeginComposite(txn.label)       # existing SceneEditor.h:103
    for e in txn.edits:
        if !editor.Apply(e):               # existing SceneEditor.h:91 — the ONLY mutator
            editor.EndComposite()          # close the bracket we opened …
            editor.Undo()                  # … then unwind the partial composite atomically
            unlock
            return { Rejected, failedEditIndex=i, currentDocument=(mUuid, mRevision), … }
    editor.EndComposite()
    if composite_was_all_noop:
        unlock; return { NoOp, newDocument=(mUuid, mRevision) }   # no bump, no publish
    # ── E1: the inseparable commit step, all under the lock ──
    ++mRevision                            # (b) advance revision
    publish( MakeSnapshot( mUuid, mRevision ) )   # (c) RCU-publish immutable snapshot (§3.5)
    history.PushTxnMeta( {txn.origin, txn.clientId, mRevision, txn.label} )  # (d) attribution (§8)
    mEditPending = true                    # existing kick (SceneEditController.cpp:1452/2400)
    newDoc = (mUuid, mRevision)            # capture under the lock for the return value
  unlock
  KickRender()                             # wake the interactive render thread (no shared-state write)
  return { Committed, newDocument=newDoc }
```

Notes:
- **Attribution under the lock (finding #4).** In v1 the attribution push ran *after* `unlock`; two commits could interleave between unlock and that push, so the deque could record edits in an order that didn't match the revision sequence (or attribute a revision to the wrong client). Pushing `TxnMeta` inside the same critical section as `++mRevision` makes the revision↔attribution↔history triple atomic. `newDoc` is captured under the lock so the returned `newDocument.revision` cannot be a *later* revision produced by a commit that sneaked in after unlock.
- **Publish under the lock (finding #2).** The snapshot is built and published while the writer still holds `mMutex` and `Apply` has fully completed, so the published view is internally consistent. A reader's `AcquireSnapshot()` either returns the pre-commit snapshot or the post-commit one — never a half-built scene.
- **Partial-failure unwind.** All-or-nothing semantics use the tools that already exist: open a composite, and on any inner `Apply` failure, close it and `Undo()` once — `EditHistory` collapses a composite into a single undo entry (`EditHistory.cpp:133-144` walks back to the matching `CompositeBegin`; `:158-187` trims atomically by composite), so one `Undo()` reverts the whole partial group. No new rollback engine, no revision bump, no publish.
- `BeginComposite`/`EndComposite` are existing no-cost brackets; a 1-edit transaction produces the same history shape it does today.
- `KickRender()` after unlock writes no shared mutable state beyond the atomics it already touches (`SceneEditController.cpp:2400-2430`) and re-acquires `mMutex` briefly itself — it is safe outside the commit critical section and must be, because it can block on the render thread.

### 4.3 Why a `(UUID, revision)` precondition, not a lock-and-hold

The controller is single-writer (the mutex). The danger is **not** two threads writing the same instant — the mutex serializes that. The danger is a **read-modify-write torn across time**: a client acquires snapshot `(U, N)`, a *different* commit advances the live revision to `N+1`, then the first client commits an edit computed from the stale snapshot and silently overwrites the second. The precondition closes exactly that window: the first commit's check (`baseDocument=(U,N) != live=(U,N+1)`) fails, it gets `Conflict { currentDocument: (U, N+1) }`, and it must re-acquire a snapshot and rebase. Matching the **UUID** as well as the revision additionally closes the *reload* window (finding #3): if the document was reloaded between read and commit, `baseDocument.uuid` no longer matches the live UUID, so the stale write is rejected even if the fresh controller's revision counter happens to equal `N`. This is optimistic concurrency control (the CAS / `If-Match` / ETag pattern), chosen over pessimistic read-locks because reads are frequent, snapshot-based, and (for external MCP) cross-process.

### 4.4 Conflict response (the contract every client obeys)

On `Conflict`, the committer receives `currentDocument`. Two sub-cases, distinguished by the UUID:

- **Revision mismatch, same UUID** — the document advanced under the client; re-acquire a snapshot at the new revision and rebase.
- **UUID mismatch** — the document was *reloaded* (or swapped); the client's whole base is gone. It must re-bootstrap from a fresh `AcquireSnapshot()` / `rise://scene/*` read, not "rebase a delta."

Required behavior, by client:

| Client | On `Conflict` |
|---|---|
| In-app GUI | Auto-rebase the *interaction*: a live gizmo drag is already committed as one transaction on pointer-up against the `baseDocument` captured at pointer-down, so an *intervening* foreign commit is rare; if it happens, re-acquire the object transform from a fresh snapshot and recompute the delta (drags are relative + bracketed, so this is seamless). A one-shot panel edit re-reads the field from a fresh snapshot and re-applies if the user's value is still meaningful, else surfaces "the scene changed — re-enter the value." On a UUID mismatch, the panel drops its selection and re-bootstraps. |
| AI agent (L1) | The proposal is *re-diffed* against the new `currentDocument` and re-presented (the diff may have changed). The model is told "scene advanced; here is the new base." (§7.4) |
| External MCP | **Cannot reach a commit `Conflict` in v1** — it never commits directly. Its `propose_*` call is staged against `baseDocument`; the conflict (if any) surfaces when the *owner approves* and the owner's `Commit` re-checks the precondition (§7). The external client is then told via its proposal-status channel to re-read `rise://scene/*` and re-propose. (MCP_TOOL_SURFACE §4 defers its `conflict` contract here.) |

There is **never** a silent merge and **never** a silent clobber — both are explicit non-goals (§13).

### 4.5 Composition with existing edit entry points — one drag, one commit, no per-move revision

The shipped one-shot entry points (`SetProperty`, gizmo `OnPointerUp`, `OnTimeScrub`, `CloneActiveCamera`) are *re-expressed as single-transaction commits* with `origin = InApp`, `clientId = 0`, `baseDocument = CurrentDocumentId()` captured at the start of the gesture. They keep their existing public signatures (ABI), and internally route through `Commit`. This is a refactor-behind-the-facade, not an API break: the macOS/Windows/Android bridges call the same `SetProperty` they call today. The only new public surface is `Commit` + the C-ABI mirror (§12), used by the agent and (via the owner's approval path) external MCP.

A live drag stays **exactly one** transaction, and the revision changes **only** on pointer-up (finding #1):

- `OnPointerDown` captures `baseDocument = (UUID, CurrentRevision())` and opens the composite.
- `OnPointerMove` mutates **owner-private preview state** (§3.3) under the existing per-move park (`SceneEditController.cpp:1309-1323` / `:1422-1428`). It does **not** call `Commit`, does **not** bump `mRevision`, and does **not** publish a snapshot. No background reader can observe these frames.
- `OnPointerUp` closes the composite and is the *single* commit point: one `Commit` → one `++mRevision` → one published snapshot → one undo entry → one attribution record.

(This matches today's "one drag = one undo entry," `SceneEditController.h:275-277`, sharpened to "one drag = one transaction = one revision = one snapshot publish." Because the revision does not move during the drag, the property "two clients at the same revision observe identical state" holds at every revision boundary, which is the only place any reader can land.)

---

## 5. Undo as the transaction stack, attributed

### 5.1 One document, one timeline, attributed entries

`EditHistory` (`EditHistory.h:26`) is a single bounded undo/redo stack of `SceneEdit`s, already grouping composites (`EditHistory.cpp:133-144` walks back to the matching `CompositeBegin` for the label). We keep **one** stack per controller — a single document has a single linear undo timeline, which is the model users expect and the only one that stays coherent when in-app, AI, and external edits interleave. What changes:

> **Each undo unit (composite or solo edit) gains an attribution record** — `(EditOrigin origin, uint32_t clientId, uint64_t revision)` — stored in a parallel `std::deque<TxnMeta>` inside `EditHistory`, pushed/popped in lockstep with composite boundaries, and (per §4.2) pushed **inside the commit critical section** so it can never desync from the revision it describes.

This is additive: `EditHistory::Push` already special-cases composite markers (`EditHistory.cpp:36-49`); we add a `PushTxnMeta` called from `Commit` at the same point it bumps `mRevision`/publishes (§4.2 step (d)), not from `EndComposite` (which has no client/origin context). The existing `LabelForUndo`/`LabelForRedo` (`EditHistory.h:72-73`) gain siblings `OriginForUndo()` / `ClientForUndo()` so the menu can render "Undo AI: warm key light" with an agent glyph.

### 5.2 Why not per-client undo stacks

The review asked us to "replace any single shared global undo across clients." The fix is **attribution, not partitioning**. Per-client stacks are unsound here: edits from different clients are *causally ordered through one authoritative state* (client B's edit may depend on client A's). Undoing A's edit out of order while B's later edit remains would leave the scene in a state neither client ever produced. So: one stack, strict LIFO, but every entry labeled with its author. The UI may *filter the view* ("show only my edits") but undo still pops in true reverse-commit order. This is the same choice collaborative-but-linear editors (e.g. single-document IDE undo with multiple cursors) make.

### 5.3 Undo/redo are themselves revision-advancing, snapshot-publishing transactions

`Undo()`/`Redo()` already mutate authoritative state (`SceneEditor::Undo/Redo`, `SceneEditor.h:96-99`) through `Apply` of inverse ops, under the same cancel-and-park (`SceneEditController.cpp:1553-1597` Undo; `:1600-1637` Redo). Under E1 they **advance the revision and publish a new snapshot** like any commit (a client that holds snapshot `(U, N)` must treat an undo as "the document moved to `(U, N+1)`"). Their `++mRevision` + publish runs inside the same critical section the existing Undo/Redo already hold for the inverse `Apply`. (The existing code also bumps the *reload sentinel* `mSceneEpoch` here at `:1594`/`:1633` for list rebuilds — that bump stays and is orthogonal to the new revision.) Attribution for an undo is the *origin of the undo action* (usually `InApp`), with the reverted transaction's original author preserved in the redo metadata so the menu can still say "Redo AI: …".

### 5.4 Cross-client undo safety

In v1, external MCP clients propose-only and never commit, so they cannot directly `undo` an in-app user's last gizmo move — undo is an owner/in-app action. Even so, the model keeps undo as global LIFO and exposes attribution so an *autonomy policy* acting for the agent (which can commit) is governed by scope: the `AI_SECURITY_MODEL` / scope layer gates whether a given origin's scope includes `undo` (default: the agent gets `edit` but **not** `undo` of *other* clients' transactions; it may only undo its own most-recent transaction). The model exposes the attribution; the scope layer decides who may pop what. See [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md). (Concurrent-writer undo arbitration is a v2 concern, §14.)

---

## 6. Multi-client reconciliation (in-app + external on one controller)

### 6.1 Clients

A *client* is any bound mutation **source**. The local GUI shell is `clientId = 0` (implicit, always present). Each external MCP connection and the in-process agent register and receive a `clientId`:

```cpp
// SceneEditController.h — additive
uint32_t RegisterClient( EditOrigin origin, const std::string& displayName );
void     UnregisterClient( uint32_t clientId );
```

`clientId` is used for attribution, scope, and proposal routing. It does **not** create a second write path. In v1 (§0 #4):

- `clientId = 0` (in-app) commits directly (its gestures are the owner's own actions).
- The in-process **agent** commits via the autonomy policy (L1 = only after the owner approves; L2/L3 within granted scopes) — see §7.
- **External MCP** clients are **read + propose-only**: they `AcquireSnapshot()` / read `rise://scene/*`, and submit *proposed* transactions; they **never** call `Commit`. The owner (or policy) applies the proposal. There is exactly one writer at any instant; `clientId` records *who originated* a committed transaction, not a license to write concurrently.

### 6.2 Ordering & atomicity

> **Invariant M1 — Total order by mutex.** Commits are linearized by `mMutex`. There is no interleaving *within* a transaction: `Commit` holds the lock across the whole `Begin…Apply…End…++revision…publish…PushTxnMeta` block (§4.2). Because v1 is single-writer, "concurrent commits" means at most the owner's commit racing the agent-policy's commit; the loser sees the winner's revision bump and either matches its `baseDocument` (proceeds) or conflicts (rejects). External proposals are not commits and contend for nothing until the owner applies them.

> **Invariant M2 — Reads use published immutable snapshots; they never read the live graph and are never torn.** (This RETRACTS v1's "lock-free read of the live scene"; finding #2.) A reader calls `AcquireSnapshot()` (§3.5) and reads only that snapshot. The snapshot is internally consistent because it was built and published *inside* the commit critical section, after `Apply` completed (§4.2) — a reader's acquire returns either the pre-commit or the post-commit snapshot, never a half-built scene, and the snapshot it holds is immutable thereafter so a *later* commit cannot tear it. Concretely: the interactive render thread renders the owner's drag-preview frames from the live graph under the existing park (that is the *owner's own* preview, §3.3), but **every other** consumer — production render, thumbnailer, RMSE, probe, panel `RefreshProperties`, and each MCP `rise://scene/*` payload — reads a held snapshot. `AcquireSnapshot()` takes only a momentary lock (or an atomic-shared-ptr load) to addref the published pointer; it does not hold `mMutex` for the read and so never blocks a writer.

### 6.3 The interleave, worked (single-writer: external proposes, owner applies)

```
rev 7   GUI user starts dragging cube              base=(U,7), composite open, PREVIEW only
rev 7   Agent AcquireSnapshot()                    holds snapshot (U,7)
rev 7   External MCP AcquireSnapshot()             holds snapshot (U,7); reads rise://scene/objects
rev 7→8 GUI user releases drag → Commit            (Committed, rev 8, publish snapshot (U,8))
        Agent proposes "move cube +x"  base=(U,7)  (STAGED, not committed)
        External MCP proposes "tint key" base=(U,7) (STAGED; external is propose-only)
        Owner approves agent proposal → Commit(base=(U,7))   (Conflict! current=(U,8))
rev 8   Agent re-diffs against (U,8), re-presents; owner re-approves
rev 8→9 Commit(base=(U,8))                          (Committed, rev 9, origin=Agent, publish (U,9))
        Owner approves external proposal → Commit(base=(U,7)) (Conflict! current=(U,9))
rev 9   External proposal re-staged against (U,9); owner re-approves
rev 9→10 Commit(base=(U,9))                         (Committed, rev 10, origin=ExternalMCP, publish (U,10))
```

No clobber: every stale-base commit is rejected, forcing a rebase against the live document. The undo stack reads `[…, drag(InApp,8), move(Agent,9), tint(ExternalMCP,10)]` — all attributed, all single-writer (each `Commit` ran alone under `mMutex`). The two readers that held snapshot `(U,7)` were never disturbed by the owner's drag (preview-only) or by any publish — they each ran against stable `(U,7)` until they chose to re-acquire.

### 6.4 Render coordination is delegated; the snapshot is the generation token; one worker pool

This spec owns *state* concurrency; it does **not** own *render* arbitration. The interactive render thread, cancel-restart, viewport-vs-production exclusivity, and stale-result rejection are owned by [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md). Two seams:

- **Kick.** A successful `Commit` sets `mEditPending` + `KickRender` (existing, `SceneEditController.cpp:1452`/`2400-2430`), which the interactive coordinator consumes.
- **Generation token = the snapshot's revision.** Each background job (render / thumbnail / RMSE / probe) is dispatched **with a `SceneSnapshot` ref** and reads only that snapshot (§3.5). The snapshot's `Id().revision` *is* the coordinator's stale-result token: a result computed against revision `N` while the live revision has moved to `N+k` is stale and may be discarded by the coordinator. One revision counter, two consumers (precondition check + stale-result rejection).
- **One process-wide worker pool (§0 #6).** All these jobs draw from a single shared pool, not per-controller threads. This is safe *because* each job holds a snapshot ref for its lifetime: the document (and the resources the job reads) cannot be freed under a pooled worker even if the user reloads or closes the document mid-render (§3.5 lifetime). The pool does not need to know about documents; it needs only that each task carries its own snapshot ref.

---

## 7. AI L1 staging — the proposed transaction (resolves the review contradiction)

### 7.1 The contradiction, named

[LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md) says, in the same document:
- §7.1 / §10: "**Every** act tool routes through `SceneEditController`" (line 290) — i.e. the agent *dispatches to the live controller*.
- §1 / L1 (line 276): the agent "**Composes a full scene diff; user approves/rejects** … On approve, applies via `SceneEditController`" — i.e. it must *diff before mutating*, which means it must **not** have mutated yet.

Both cannot be literally true at L1: you cannot show a pre-mutation diff if the act tool already mutated the live state. The resolution is a staging layer.

### 7.2 Resolution — propose, then commit (and the same shape for external clients)

> **An L1 act tool does NOT call `SceneEditController::Commit`. It builds a `SceneTransaction` (with `baseDocument = CurrentDocumentId()` captured from the snapshot the agent read) and hands it to the staging area** (`AgentSession`, `src/Library/Agent/`). The live `SceneEditor` is untouched. The diff (§10) is computed from that proposed transaction against the held snapshot. Only on owner **Approve** does the staging area call `SceneEditController::Commit(txn)` — and the precondition `(UUID, baseRevision)` is **re-checked there** (§4.2). If the document advanced (or was reloaded) between proposal and approval, the commit conflicts and the proposal is re-diffed (§4.4 / §7.4).

This makes both statements true under a precise reading: act tools *route through the controller* (the apply path is `Commit`, the one mutation path — no bespoke writer), **and** L1 *diffs before mutating* (the diff is computed from the staged transaction before `Commit` runs). The "dispatch to live controller" is **deferred to approval time**, not skipped.

> **External MCP uses the identical propose path, with NO autonomy escalation.** An external client's `propose_*` tool builds the same `SceneTransaction` against its held snapshot's `baseDocument` and stages it; the *owner* approves and the *owner's* controller commits (§0 #4, §6.1). External clients have no L2/L3 — they cannot self-approve. This is the single-writer guarantee at the protocol boundary.

### 7.3 Autonomy levels mapped onto staging (agent only; external clients are propose-only)

| Level | Staging behavior | Precondition handling |
|---|---|---|
| **L0 Advisor** | No transaction built; chat only. | snapshot acquired; no commit |
| **L1 Propose-and-confirm** (default) | Build proposed transaction → diff → **wait for owner Approve** → `Commit` (precondition re-checked). | `baseDocument` at proposal; re-check at approve |
| **L2 Operate-with-guardrails** | `Commit` immediately within granted scopes (no human gate per edit), but still as an attributed single-writer transaction under `mMutex`. | `baseDocument` = the document at the tool call; conflict → auto re-acquire snapshot + one retry, then surface |
| **L3 Autonomous** | Same as L2, batched; each variation is its own transaction (so each is independently undoable / attributable). | per-transaction base; conflicts abort the batch step, not the batch |

`origin = Agent` for L0–L3; `origin = ExternalMCP` for external proposals. `clientId` is the registered id (§6.1). L2/L3 are agent-only and still single-writer (each `Commit` is serialized by `mMutex`; "operate without a human gate" relaxes the *approval* requirement, not the *one-writer* requirement).

### 7.4 Re-diff on conflict

Because the proposed transaction holds `baseDocument`, an Approve that loses the race returns `Conflict { currentDocument }`. The staging area then: (a) re-acquires a snapshot at `currentDocument`, (b) recomputes the diff, (c) if the diff is *identical* (the intervening change was orthogonal) **and** the UUID is unchanged, silently re-commits with the new base; (d) if it *changed*, re-presents the new diff to the owner; (e) if the **UUID** changed (the document was reloaded), discards the proposal entirely and re-bootstraps — there is no "rebase a delta onto a different document." The model is fed the conflict as a tool result so it can revise. This is the agentic self-correction loop the runtime spec wants, made safe by the precondition.

### 7.5 MVP wholesale-rewrite is a document swap, not a transaction

LLM_AGENT_RUNTIME §9.5 notes the MVP can skip structured edits and *rewrite the whole scene text + reload*. This is **NOT** modeled as a committed transaction (finding #6): re-parsing replacement text constructs a **fresh controller** (new UUID, empty history, `mRevision = 0`) — there is no `SceneEdit` list and no history slot, so it cannot be an undo unit. It is a **document swap** (§9.3, §11.4), governed by the same "unsaved changes will be lost" confirm as any reload. Undo does not cross the swap. The structured-transaction path (this whole spec) is the *only* one that produces undoable, attributed history; "wholesale rewrite" is a coarse re-author that replaces the document, and the agent UI must present it as such (not as a reversible edit). A genuinely-undoable whole-scene rewrite would require a real document snapshot to restore from — out of v1 scope (§13).

---

## 8. Attribution storage

Minimal, additive, lives next to the history:

```cpp
// EditHistory.h — additive
struct TxnMeta {
  EditOrigin   origin;
  uint32_t     clientId;
  uint64_t     revision;     ///< the revision this transaction produced (the published snapshot's)
  std::string  label;        ///< already have a label path; mirrored for fast menu access
};
// parallel deque, one entry per composite/solo undo unit.  Pushed from Commit
// INSIDE the critical section (§4.2 step (d)), in lockstep with ++mRevision +
// publish; popped/moved to the redo side in lockstep with the composite on
// Undo/Redo; trimmed in lockstep with the by-composite TrimToMax (§11.3).
```

Accessors (additive): `OriginForUndo()`, `ClientForUndo()`, `OriginForRedo()`, plus a read-only enumerator `EnumerateHistory(fn)` for an audit/timeline panel (the AI runtime's "show me the code" history and the security model's audit log both read this — one source). No change to the existing `Push`/`PopForUndo`/`PopForRedo` signatures (`EditHistory.h:35-44`); the meta deque is maintained alongside. **The `revision` field never desyncs from its edit** because both are written inside the one commit critical section (finding #4) and trimmed/popped together (the existing by-composite trim, `EditHistory.cpp:158-187`, gains a matching `TxnMeta` pop).

---

## 9. External-file reconciliation

### 9.1 What already exists (do not rebuild)

`SaveEngine` **already detects** external modification: at save it `stat()`s the source and compares `st_size` + `st_mtime`(+nsec) against the `FileIdentity` captured at load (`SaveEngine.cpp:827-861`; `FileIdentity` `SourceSpanIndex.h:133-140`). On mismatch it returns `Status::Refused` with a clear message and **leaves the file untouched** (`SaveEngine.cpp:847-854`). Post-save it refreshes the stored identity (`SaveEngine.cpp:1848-1868`). This is the byte-offset-safety guard, and it is the foundation — the spec turns its *hard refusal* into a *user choice*.

### 9.2 Promote refusal → reconcile prompt

> **Invariant F1 — never silently clobber hand-edits.** Before any save, and on window-focus-gained, the controller compares the live `FileIdentity` to the current on-disk `stat`. On divergence it raises a **reconcile decision** to the platform UI — it does not auto-save and does not auto-reload.

New controller surface (additive):

```cpp
// SceneEditController.h — additive
enum class FileSyncState : int { InSync = 0, DiskChanged = 1, DiskMissing = 2 };
FileSyncState CheckDiskSync() const;     // stat vs FileIdentity; no side effects

// NOTE: there is no "KeepMineOverwrite that silently clobbers" choice (finding #6).
// On a disk-changed file the only safe writes are: (a) Save-As to a DIFFERENT path
// (the on-disk hand-edits are preserved at the original path), or (b) an explicit,
// loud "overwrite — discard the on-disk version" the user typed past a warning.
enum class ReconcileChoice : int { SaveAsNewPath = 0, ReloadFromDisk = 1, OverwriteDiscardingDisk = 2, Cancel = 3 };
SaveResult ResolveAndSave( const std::string& filePath, ReconcileChoice choice );
```

`CheckDiskSync()` is cheap (one `stat`) and is wired to: (a) the focus-gained handler in each shell, (b) the start of `RequestSave`. Behavior:

| Disk state | `RequestSave` behavior | Focus-gained behavior |
|---|---|---|
| `InSync` | save normally (existing path) | nothing |
| `DiskChanged` | **refuse** the in-place save; prompt **Save-As (new path) / Reload / Overwrite-discarding-disk / Cancel** | non-modal banner "File changed on disk — Reload / Save a copy / Keep editing" |
| `DiskMissing` | prompt Save-anyway (recreate at path) / Save-As / Cancel | banner "File removed on disk" |

`ResolveAndSave`:
- `SaveAsNewPath` → the safe default offered first. Writes the live edits to a **different** path; the original (externally-edited) file is untouched. `SaveEngine`'s offsets are validated against the *source* identity for the in-place portions and the engine refuses any chunk it cannot place (existing behavior, §9.4) — the hand-edits at the original path are never at risk.
- `OverwriteDiscardingDisk` → only after the user explicitly confirms "discard the version on disk." Even here we do **not** byte-splice against the moved bytes (the load-time offsets are invalid). Two honest options, both of which the user is told about:
  - *Re-write the file from the live document's own retained source* — but RISE's `Job` **does not retain source text** (`Job.h:116` holds only byte offsets; CURRENT_STATE_AUDIT §12), so there is no in-memory original to re-emit faithfully. A "full re-serialize from introspection" is **rejected as the silent default** because introspection cannot reproduce unsupported chunks, `>` commands, macros, `FOR` loops, or construction-only state (finding #6) — it would silently drop them.
  - *Therefore the only correct `OverwriteDiscardingDisk` is: re-parse the live document's intended text and write that.* In v1, where the live document has no retained text, this collapses to "save the structured edits the engine CAN represent and **Refuse** (loudly) the rest" — i.e. the existing `SaveEngine` refusal surface, shown to the user, not a lossy auto-rewrite. The clean long-term fix (retain source text on `Job`, or carry a true document snapshot) is flagged for ENTITY_CREATION / SaveEngine / ROUND_TRIP_SAVE follow-up and is out of v1 scope (§13).
- `ReloadFromDisk` → discard the in-memory edits *after* an "unsaved changes will be lost" confirm (drive off `HasUnsavedChanges()`, `SceneEditController.h:510`), then perform a **document swap** (§9.3): destroy the controller + history, construct a new one (fresh UUID) by re-parsing the file.
- `Cancel` → no-op; banner persists.

> **Invariant F1 (sharpened) — never silently clobber, never silently merge, never lossily auto-rewrite.** On any disk divergence the controller refuses the in-place save and surfaces a choice. It never auto-saves, never auto-reloads, and never re-serializes-from-introspection behind the user's back. (Finding #6.)

### 9.3 Reload is a document-level swap, NOT an undoable transaction (finding #6)

A `ReloadFromDisk` is **not** a committed transaction and has **no** `EditOrigin` (there is no `Reload` origin — §4.1). It is a **document swap**:

- The current `SceneEditController` (with its `EditHistory`, `mRevision`, UUID, and published snapshot) is **destroyed**. A new controller is constructed by re-parsing the file (`LoadAsciiScene`), with a **fresh UUID** and `mRevision = 0` (§11.4). This matches how reload already works — the GUI tears down + recreates the bridge, which builds a new controller (CURRENT_STATE_AUDIT §3 / `SceneEditController.h:391-396`).
- **Undo does not cross the swap.** The new controller's history is empty; there is no slot for an "undo reload" to pop. Modeling reload as an in-history transaction (the v1 §9.3) was unsound: the controller that would hold the entry is the one being destroyed, and the reverse op would have to reconstruct the *entire prior document* — which re-serialize-from-introspection cannot do (finding #6).
- **In-flight readers are unaffected by the swap.** Background jobs dispatched against the old document hold their own `SceneSnapshot` refs (§3.5), so the old document's resources stay alive until those jobs finish — even though the controller is gone. Their results are stale (old UUID) and the render coordinator discards them (§6.4).

> **A truly undoable reload is possible only with a real document snapshot** (a full, restorable capture of the prior document, not a thin resource-pinning view). That is a larger feature — out of v1 scope (§13). For v1, reload is a deliberate, confirmed, one-way swap; the application *may* keep the prior file path on a recent-documents list, but that is a UI affordance, not an undo.

### 9.4 Save-As and cross-file

`SaveEngine` already distinguishes Save-As (`isSaveAs`, `SaveEngine.cpp:824`) and refuses cross-file managed-block / chunk cases (`CURRENT_STATE_AUDIT.md` §1). Reconciliation respects this: a Save-As to a *new* path skips the external-mod check against the *target* (it's new) but still validates offsets against the *source* identity, and surfaces any `SaveEngine` refusal through the same reconcile UI rather than dropping content. `SaveAsNewPath` (§9.2) is exactly this path, offered as the safe response to a disk-changed file.

---

## 10. "Show me the code" diff derives from a transaction

The diff shown by the L1 review gate (LLM_AGENT_RUNTIME §9 "show me the code") and the post-commit "receipt" are the **same artifact**, computed once in shared C++ from a `SceneTransaction`:

> **`std::string SceneEditController::RenderTransactionDiff(const SceneTransaction&) const`** — produces a unified `.RISEscene` text diff for the transaction. Implementation reuses the `SaveEngine` re-emit machinery in a **dry-run** mode: render the affected chunks *as they are in the base snapshot* vs *as they would be after the transaction's edits*, without writing to disk. (This reuses `SaveEngine`'s per-entity chunk renderers, e.g. `RenderCreatedCameraChunk` `SaveEngine.cpp:544`, in a buffer-only path.)

- **Pre-commit** (L1, staged): the diff is computed from the *proposed* transaction against the snapshot it was staged on (`txn.baseDocument`) — a preview. Nothing has mutated.
- **Post-commit** (receipt / audit): the same renderer runs against the committed transaction's recorded edits + its attribution (§8), so the timeline panel can show "AI changed these lines at revision 9."
- The diff is **shared** (one renderer, all platforms); only the red/green styling is per-platform (CROSS_PLATFORM_ARCHITECTURE §10.2).

This keeps the diff honest by construction: it is generated by the same engine that performs the save, so what the user reviews is what the file will contain — the anti-drift principle (GUI_ROADMAP §1, descriptor-as-truth) applied to diffs.

---

## 11. Edge cases & lifecycle

### 11.1 Empty / no-op transactions
A transaction whose edits all net to no-op (e.g. drag-then-undo within the bracket) returns `NoOp`, does **not** advance the revision, **does not publish a new snapshot**, and pushes nothing to history. This mirrors `SaveEngine`'s NoOp byte-identity result (`SaveEngine.h:48`) and the existing "drag → undo → no-op save" test (`CURRENT_STATE_AUDIT.md` §1). Readers continue to observe the prior published snapshot.

### 11.2 Partial failure mid-transaction
Handled in §4.2: open composite, on inner `Apply` failure close + `Undo` once (atomic-by-composite, `EditHistory.cpp:158-187`), return `Rejected{failedEditIndex}`. State is exactly the pre-transaction state. **No revision bump, no publish, no `TxnMeta` push.**

### 11.3 History trim vs attribution
`EditHistory` is bounded (`EditHistory.h:29`, default 1024) and trims oldest *by whole composite* (`EditHistory.cpp:158-187`). The parallel `TxnMeta` deque trims in the same step so attribution never desynchronizes from the edit it describes. Trimming an old transaction does not affect the revision (revisions are monotonic regardless of history depth) and does not affect any published snapshot (snapshots are independent of history depth).

### 11.4 Reload mints a fresh UUID and resets the revision lineage
Loading a scene constructs a fresh `SceneEditController` (`CURRENT_STATE_AUDIT.md` §3 / `SceneEditController.h:391-396`, "the GUI tears down + recreates the bridge, which builds a new controller"). The new controller mints a **new UUID** (§3.4) and starts at `mRevision = 0`. Revisions are **not** comparable across reloads — but a client never needs to compare them by value, because the **UUID** differs. A `baseDocument` from a previous load has a stale UUID, so it never matches → `Conflict` with a UUID mismatch (§4.4), which the client reads as "reloaded; re-bootstrap." The existing reload sentinel `mSceneEpoch` continues to signal list-rebuilds; the UUID is the precondition-grade identity. (This is the structural guarantee behind finding #3: a fresh controller can have the same *revision counter value* as the old one and a stale precondition still cannot match.)

### 11.5 Concurrency invariant summary
- **E1** revision advances iff state changed — one bump per commit, under `mMutex`, **inseparable from snapshot publication + attribution push** (§4.2).
- **E2** every read carries the held snapshot's intrinsic, immutable `DocumentId = (UUID, revision)` (no post-hoc tagging, no torn-read window) (§3.2).
- **M1** commits totally ordered by `mMutex`; single-writer (§6.2).
- **M2** reads use published immutable snapshots — never the live graph, never torn (publish is inside the commit critical section after `Apply`) (§6.2). *(Replaces the v1 "lock-free read of the live scene"; finding #2.)*
- **D1** (new) document identity is `(UUID, revision)`; preconditions match the pair; reload mints a fresh UUID (§3.4, §11.4). *(Finding #3.)*
- **S1** (new) a snapshot **clones** the document's mutable wrapper state and addref-shares the immutable leaves, so it is independent of later live mutation for its lifetime; freed when the last holder releases (**PROTOTYPED & TESTED**, §3.5). The *published-per-revision + held-across-commits-by-a-pooled-job* form that makes it safe under one shared worker pool is the production target (§3.5.4). *(Finding #2/#6.)*
- **A1** (new) attribution is recorded inside the commit critical section, in lockstep with the revision bump (§4.2, §8). *(Finding #4.)*
- **W1** (new) single-writer in v1: in-app and agent-policy commit; external MCP proposes only (§0 #4, §6.1, §7). *(Finding #7.)*
- **F1** never silently clobber, merge, or lossily auto-rewrite the on-disk file (§9.2). *(Finding #6.)*
- **R1** (new) reload is a document-level swap (destroys controller + history), not an undoable transaction (§9.3). *(Finding #6.)*
- **T1** a transaction is atomic: all edits apply or none do; the revision bumps exactly once (with one publish + one attribution) or not at all (§4.2).

### 11.6 Android
Android consumes the *same* C++ controller via JNI, so it gets transactions, revisions, published snapshots, document identity, attribution, and conflict handling for free. Today Android has **no scene save** at all (`CURRENT_STATE_AUDIT.md` §5/§13 — `nativeSaveAs` saves the *image*); GUI_ROADMAP §16 commits to wiring real `.RISEscene` save as Tier A. When that lands, Android's save path inherits the §9 reconcile prompt unchanged (the refuse-with-reload-or-Save-As decision is a controller decision; the shell just renders the choice). The AI staging (§7) is Tier A on Android (chat is mobile-native) and works identically — external MCP propose-only applies on Android too. See the Android-tier note in §15.

---

## 12. Public surface & ABI (additive only)

Per the `abi-preserving-api-evolution` discipline and GUI_ROADMAP §10.5 constraint: **no existing signature changes, no new virtuals on shipped interfaces.** Everything is new non-virtual methods + appended C exports.

**New C++ (non-virtual) on `SceneEditController`:**
`CurrentRevision()`, `CurrentDocumentId()`, `AcquireSnapshot() const`, `Commit(const SceneTransaction&) -> CommitResult`, `RegisterClient(...)`, `UnregisterClient(...)`, `CheckDiskSync()`, `ResolveAndSave(...)`, `RenderTransactionDiff(...) const`. (The existing `SetProperty`/gizmo/`OnTimeScrub`/`CloneActiveCamera` are re-implemented to route through `Commit` internally — same signatures. `SceneEpoch()` is **kept unchanged** as the reload sentinel — §3.2.)

**New C++ on `EditHistory` / `SceneEditor`:** the `TxnMeta` parallel deque + `Origin*`/`Client*`/`EnumerateHistory` accessors; `SceneEditor::ApplyTransaction` is an internal helper (not public) — `Commit` lives on the controller because it owns the mutex + revision + snapshot publish + render kick.

**Snapshot — already in-tree, NO new header.** The prototyped `SceneSnapshot` (§3.5) lives **inside `Scene.cpp`** (declared in the existing `Scene.h`), and `Object::CloneSnapshot` / `CameraCommon::CaptureSnapshot` are concrete methods on existing classes — so the spike added **no new source file** and touched **none** of the five build projects. If the production layer (§3.5.4) later promotes the publishable snapshot + `DocumentId` into their own header (e.g. for an intrinsic `Id()` value type shared across translation units), *that* header would follow the five-project rule below.

**New header for the transaction layer (requires touching all five build projects):**
- `src/Library/SceneEditor/SceneTransaction.h` — `EditOrigin` / `CommitStatus` / `SceneTransaction` / `CommitResult` (§4.1). (A `DocumentId` value type, §3.4, would live here or in a small companion header; it does **not** exist in the spike.)

Adding a `.h` (and any `.cpp`) requires touching **all five build projects** per CLAUDE.md: `build/make/rise/Filelist`, `build/cmake/rise-android/rise_sources.cmake`, `build/VS2022/Library/Library.vcxproj` + `.vcxproj.filters`, and `build/XCode/rise/rise.xcodeproj/project.pbxproj` (the four pbxproj sections × the two targets). Called out so it is not forgotten when the transaction header lands.

**New C-ABI (append to [../../src/Library/RISE_API.h](../../src/Library/RISE_API.h)), following the existing `RISE_API_SceneEditController_*` pattern** (e.g. the Phase-4b block at the tail of the header). The commit surface is an **opaque per-client transaction-builder handle** — NOT controller-global builder state (finding #5):
```c
uint64_t RISE_API_SceneEditController_CurrentRevision( SceneEditController* p );
// DocumentId is small + trivially-copyable; marshal as out-params.
void     RISE_API_SceneEditController_CurrentDocumentId( SceneEditController* p,
             uint64_t* outUuidHi, uint64_t* outUuidLo, uint64_t* outRevision );

// ---- Opaque per-client transaction builder (finding #5) ----
// A builder belongs to ONE client; two clients each create their own handle,
// so concurrent AppendEdit calls cannot intermix edits into one transaction.
// The controller keeps NO global "current builder" — all in-progress edit
// state lives inside the opaque RISE_TxnBuilder the caller owns.
typedef struct RISE_TxnBuilder RISE_TxnBuilder;
RISE_TxnBuilder* RISE_API_TxnBuilder_Begin( int origin, uint32_t clientId,
             uint64_t baseUuidHi, uint64_t baseUuidLo, uint64_t baseRevision,
             const char* label );
int  RISE_API_TxnBuilder_AppendEdit( RISE_TxnBuilder* b, /* edit-op fields … */ );
// Submit the built transaction atomically to the controller; frees the builder.
int  RISE_API_TxnBuilder_Commit( RISE_TxnBuilder* b, SceneEditController* p,
             uint64_t* outNewUuidHi, uint64_t* outNewUuidLo, uint64_t* outNewRevision,
             uint64_t* outCurUuidHi, uint64_t* outCurUuidLo, uint64_t* outCurRevision,
             int* outFailedIdx, char* outMsg, unsigned int msgLen );  // returns CommitStatus
void RISE_API_TxnBuilder_Abort( RISE_TxnBuilder* b );  // discard without committing

// Alternatively (and the ONLY external-MCP path, propose-only): submit a whole
// edit set in one call.  A flattened payload is built client-side and handed
// over atomically — no per-edit round-trips, no shared builder.
int  RISE_API_SceneEditController_Propose( SceneEditController* p,
             int origin, uint32_t clientId,
             uint64_t baseUuidHi, uint64_t baseUuidLo, uint64_t baseRevision,
             const char* label, const void* editPayload, unsigned int payloadLen,
             /* out: proposal handle for status / approval routing */ );

uint32_t RISE_API_SceneEditController_RegisterClient( SceneEditController* p,
             int origin, const char* displayName );
int      RISE_API_SceneEditController_CheckDiskSync( SceneEditController* p );
// + AcquireSnapshot/Release, ResolveAndSave, RenderTransactionDiff, history-attribution getters …
```
The opaque-handle builder (or the one-shot `Propose`/whole-payload submit) **eliminates the controller-global `CommitBegin/AppendEdit/CommitEnd` race** (finding #5): in v1 the builder kept the in-progress transaction *on the controller*, so two clients interleaving `AppendEdit` would merge their edits. Here each builder is per-client and owned by the caller; the controller only ever sees a *complete* transaction at `Commit`/`Propose` time, and applies it under `mMutex` as one unit. Both shapes keep the C-ABI free of struct-layout coupling (the same marshaling shape the existing `Property*For` getters use). The bridge-enum mirror generator (GUI_ROADMAP §10.5, `tools/gen_bridge_enums.py`) emits `EditOrigin` / `CommitStatus` / `ReconcileChoice` / `FileSyncState` to Kotlin + Obj-C `static_assert`s so the `case N:` fall-through bug stays structurally impossible (per the MEMORY note on bridge enum-translation audits — `ReconcileChoice` now has **four** values, so the generator must cover `case 3:`).

---

## 13. Non-goals

- **Concurrent multi-writer (v1).** v1 is **single-writer** (§0 #4): in-app and the agent-policy commit; external MCP clients **propose only**, applied by the owner. Concurrent multi-writer — and the conflict/rebase machinery *between live writers* it needs — is **v2** (§14). The `(UUID, revision)` precondition already lays the optimistic-concurrency groundwork, but v1 never has two threads racing to commit different documents' worth of state.
- **Operational-transform / CRDT merge.** No automatic three-way merge of concurrent edits. Conflicts are *rejected and rebased*, not merged — the right call for a single-writer desktop tool with optimistic concurrency.
- **A true, restorable document snapshot (and therefore an undoable reload / undoable wholesale-rewrite).** The `SceneSnapshot` (§3.5) is — *as prototyped* — a clone of the document's **mutable wrapper state** (object transforms + camera pose) sharing the immutable leaves by refcount, with **read** accessors only; it is **not** a full restorable capture of the document (no restore/swap-back path, no per-camera-type clone, no TLAS/`LightSampler` restore — §3.7). Reload and MVP wholesale-rewrite are **document swaps** (§9.3, §7.5), not undo units. A genuinely-undoable reload needs a real, *restorable* document snapshot (or retained source text on `Job`, which it does not have — CURRENT_STATE_AUDIT §12); that is out of v1 scope (finding #6).
- **Re-serialize-from-introspection as a save strategy.** Rejected as a silent fallback (§9.2, finding #6): it cannot reproduce unsupported chunks, `>` commands, macros, `FOR` loops, or construction-only state. Overwriting a disk-changed file is handled by refuse-with-Save-As / loud explicit overwrite, never a lossy auto-rewrite.
- **Multi-document / multi-controller sessions.** One controller = one open scene = one document (one UUID) = one revision lineage = one undo timeline. Multiple open documents are independent controllers (out of scope here).
- **Per-client (partitioned) undo stacks.** Rejected in §5.2 — attribution on one stack, not N stacks.
- **Silent clobber, silent merge, silent reload, or lossy auto-rewrite of the on-disk file.** Explicitly forbidden by F1 (§9.2).
- **Real-time collaborative editing across machines.** External MCP clients are local-loopback tools (MCP_TOOL_SURFACE §13 transport), not networked co-authors; v1 supports them as propose-only readers, not as a CRDT collaboration substrate.
- **Lock-free reads of the live scene.** RETRACTED (finding #2). All non-owner reads go through published immutable snapshots (§3, §6.2).
- **Owning render arbitration.** Delegated to [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) (§6.4) — this spec stops at "commit kicks a render via the existing `mEditPending`/`KickRender` seam, and each job carries a snapshot whose revision is the stale-result token."
- **Owning the validation pass.** The side-effect-free `validate` lives in [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md); a transaction may be *validated* before commit but the validator is that doc's concern.

---

## 14. How the other specs consume this (the deference map)

| Spec | What it defers here |
|---|---|
| [LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md) | L1 = proposed transaction (§7); "act tools route through controller" = via `Commit` at owner-approve time; "show me the code" diff (§10); undo attribution for AI edits (§5/§8); MVP wholesale-rewrite = document swap, not undo (§7.5). |
| [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) | external tools are **propose-only** in v1 (§0 #4, §7.2); `propose_*` carries `baseDocument = (UUID, revision)`; the `conflict` contract surfaces at owner-approval (§4.4); resources stamped with `{"uuid","revision"}` (E2). |
| [RENDER_COORDINATOR.md](RENDER_COORDINATOR.md) | jobs are dispatched with a `SceneSnapshot` ref; the snapshot's `revision` is the stale-result "generation" token; commit→`mEditPending`/`KickRender` is the seam; one process-wide worker pool is safe via snapshot refs (§3.5, §6.4). |
| [ENTITY_CREATION.md](ENTITY_CREATION.md) | Add/delete/duplicate are transactions; created-entity persistence and the §9.2 refuse-or-Save-As reconcile (NOT a silent re-serialize) bound what persists. |
| [AI_SECURITY_MODEL.md](AI_SECURITY_MODEL.md) | `clientId`/`EditOrigin` feed scope checks; external = propose-only; who-may-undo-whose-transaction policy (§5.4); audit log reads `EnumerateHistory` (§8). |
| [CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md) | The C-ABI surface (opaque per-client builder / `Propose`) + bridge-enum mirror (§12) is the cross-platform seam; reconcile-prompt UI is the only per-platform piece of §9. |
| [CAMERAS_AND_VIEWS.md](CAMERAS_AND_VIEWS.md) | "Promote named view → scene camera" is an `AddCamera` transaction; the commit's revision bump + publish refreshes camera lists (the existing reload-sentinel bump at `SceneEditController.cpp:1594` also fires for list rebuilds). |

### 14.1 What is explicitly deferred to v2

- **Concurrent multi-writer.** Two live writers (e.g. external MCP committing directly alongside the in-app user). Needs: per-writer commit serialization that is already present (`mMutex`), plus a *rebase* protocol so a losing writer's transaction can be re-expressed against the winner's revision rather than rejected — and a policy for who may overwrite whom. The `(UUID, revision)` precondition + snapshot publication are the v1 substrate this builds on.
- **A true restorable document snapshot.** Enables undoable reload and undoable wholesale-rewrite (§13). Needs either retained source text on `Job` or a deep document capture.
- **Fully-isolated drag preview.** Cloning the drag into a scratch document so even the owner's preview never touches the live graph (§3.3) — the working-copy / staging model (§14.2). The clone capability this needs is now **proven** (§3.5); the restore/publish path it also needs is **not yet built** (§14.2).

### 14.2 Working-copy / transaction-atomicity — rollback BUILT (inverse-edit undo); snapshot/restore EXPERIMENTAL

> **⚠ UPDATE (2026-06-20) — the narrative below is SUPERSEDED.** Restore/publish *was* built (#2a/#2b) but a 5th code-backed review found P1 bugs in it; the editor's transaction **rollback was re-based on inverse-edit undo (NOT snapshot-restore)** (`3b32b7ba`), and `Scene::CreateSnapshot`/`RestoreFromSnapshot` are now **EXPERIMENTAL** (off the rollback path, retained for a future isolated-render use). The clone-only spike narrative here and in §3.5 (incl. "18/18", "no new source file", "pose-only camera") is **A-era and stale** — see [GUI_ROADMAP](../GUI_ROADMAP.md) §13a #2's P1 register for the current verified state (`SceneEditTransactionTest` 85/0 on inverse-edit undo).

This is the "◑ partially grounded" item from the code-backed review ([GUI_ROADMAP](../GUI_ROADMAP.md) §13a item #2). Its status, honestly:

- **Grounded now (the clone capability is proven).** The snapshot mechanism (§3.5, `tests/SceneSnapshotTest.cpp` 18/18 + negative control) is the foundation the **working-copy / staging** model rests on: *drag on a scratch copy, build the transaction off-graph, publish only on success.* Because a cloned snapshot is provably independent of the live graph, a drag (or an AI/batch edit) can mutate a working **copy** without touching the live scene — so there is no double-apply, and nothing is left redoable-after-reject. That architectural claim is now backed by code, not just design.
- **NOT yet built (the publish/restore path).** Making the working copy real requires the **restore/publish (swap) path** — swap the working copy back in as the live scene on success — which **must rebuild the TLAS and the `LightSampler`** (§3.7 item 2: re-run `Job::SetPrimaryAcceleration`'s top-level BVH4 and rebuild the `LightSampler` that `RayCaster::AttachScene` constructs from luminaries, `RayCaster.cpp:160-171`). It also requires **routing the drag through the working copy** instead of the live graph (today §3.3's drag mutates owner-private preview state on the *live* graph under cancel-and-park, not a clone). Neither exists in the spike.

> **Do NOT claim atomic rollback / publish-on-success works yet.** The mechanism that *enables* it (independent clone) is proven; the mechanism that *delivers* it (restore/publish with TLAS + `LightSampler` rebuild, drag-on-working-copy) is the **next implementation step** after the snapshot spike, and is unimplemented. Until restore lands, this spec's "publish a snapshot on commit" / "swap back on rollback" language describes the target architecture (§3.5.4), not current behavior.

---

## 15. Acceptance criteria (GUI_ROADMAP §15 template, filled in)

- **Tests.**
  - `SceneSnapshotTest` (**DONE — PROTOTYPED & PASSING, 18/18**; [`tests/SceneSnapshotTest.cpp`](../../tests/SceneSnapshotTest.cpp), branch `feature/gui-snapshot-prototype`): the decisive mechanism test. Takes a `Scene::CreateSnapshot()`, mutates the live scene (`TranslateObject` + finalize; camera `SetLocation` + `RegenerateData`), and asserts the snapshot is **unchanged** and now differs from live — with a **negative control** proving a bare `addref`'d handle *does* see the mutation (so addref-alone is insufficient and the clone is required; §3.5.2). Also records the measured cost (`[cost]` line, ≈3.5–4 µs warm for 2 objects). This is the implemented proof of snapshot independence; it does **not** exercise publication, restore, or render-faithful camera cloning (those are the production-layer tests below, still pending the §3.5.4 / §3.7 work).
  - `SceneTransactionTest` (new standalone test, per the repo's "tests are standalone executables" convention): commit advances the **revision** by exactly 1 and publishes a new snapshot whose `Id().revision` matches (E1); empty/no-op transaction leaves revision unchanged + **publishes no new snapshot** + history empty (§11.1); stale `baseDocument` → `Conflict` with `currentDocument` and **byte-identical state** before/after (invariant T1 — assert via an introspection snapshot equality, not a screenshot); partial-failure transaction → `Rejected{failedEditIndex}` + pre-transaction state restored + no revision bump (§11.2).
  - `SnapshotPublicationDataRaceTest` (**NEW — required; depends on the not-yet-built publication layer §3.5.4; run under ThreadSanitizer**): one writer thread runs a tight loop of `Commit`s (each a small material/transform edit) while N reader threads spin on `AcquireSnapshot()` → read the snapshot's scene → `Release()`. Assertions: (a) **TSan reports zero data races** on `SceneSnapshot` publication/consumption and on the resources a snapshot pins (this is the mechanical proof for finding #2 / invariant M2/S1); (b) every reader's view is internally consistent (a committed edit is observed either fully or not at all — e.g. a two-field edit never shows one field old + one new); (c) a snapshot held across K subsequent commits still reads its original revision's state (immutability); (d) resources a held snapshot pins are not freed until the last reader releases (instrument the resource dtor / refcount). Build the test target with `-fsanitize=thread` per the repo's standalone-test convention; gate it in the runner like the other engine tests. (This extends the proven `SceneSnapshotTest` to the *published-pointer* path — `AcquireSnapshot()` does not exist in the spike yet.)
  - `PreconditionMismatchAfterReloadTest` (**NEW — required**): acquire `baseDocument = (U0, N)` from controller A; *destroy A and construct a fresh controller B by re-parsing the same file* (the reload/document-swap seam, §11.4) — B has UUID `U1 ≠ U0`; attempt `B.Commit(txn{ baseDocument=(U0,N) })`. Assert it returns `Conflict` with a **UUID mismatch** in `currentDocument` (never `Committed`), **even when B's revision counter happens to equal `N`** (force this by issuing `N` commits on B first, then retry the stale precondition). This is the mechanical proof for finding #3 / invariant D1. Also assert a same-document stale precondition `(U1, N-1)` after a real commit returns a *revision*-mismatch `Conflict`.
  - `TransactionAttributionTest`: a 3-commit sequence (InApp, Agent-approved, ExternalMCP-proposed-then-owner-approved) produces a single LIFO stack whose `Origin*ForUndo` reads back the correct author per pop; the `TxnMeta.revision` for each entry equals the revision that commit produced (proving the in-critical-section push, finding #4 / invariant A1); history trim drops attribution in lockstep (§11.3).
  - `ConflictRebaseTest`: simulate acquire@(U,N), foreign commit N→N+1, then commit with base (U,N) → `Conflict { currentDocument=(U,N+1) }`; re-acquire + re-commit with base (U,N+1) → `Committed`. (Models the §6.3 interleave; single-writer — the "foreign" commit is the owner's prior commit.)
  - `ProposeOnlyExternalClientTest`: an `ExternalMCP`-origin client can `AcquireSnapshot` + `Propose` but has **no** direct `Commit` path that mutates the live document; the staged proposal applies only via the owner-approval `Commit`, which re-checks the precondition (proves finding #7 / invariant W1).
  - `ExternalFileReconcileTest`: extend the existing external-mod guard test (`SaveEngineTest.cpp:766`) — `CheckDiskSync` returns `DiskChanged` after a touch; an in-place `RequestSave` is **refused** (not a silent overwrite); `ResolveAndSave(SaveAsNewPath)` succeeds and **leaves the original disk file byte-identical**; `ResolveAndSave(ReloadFromDisk)` performs the document swap (new UUID, empty history — assert the controller identity changed and `UndoDepth()==0`); `ResolveAndSave(OverwriteDiscardingDisk)` does **not** silently re-serialize-from-introspection (it surfaces the `SaveEngine` refusal for un-representable content); **no path clobbers/merges/auto-rewrites without the explicit choice** (F1, finding #6).
  - **Correctness invariant** (engine-touching discipline): committing then immediately undoing a transaction yields a scene **byte-identical on save** to the pre-transaction save (re-uses `SaveEngine` NoOp byte-identity, `SaveEngineTest.cpp`). No render/RMSE change — the transaction layer is pure orchestration, integrators are byte-identical.
- **Platform parity.** Shared C++: the entire model (revision, published snapshots, document identity, transactions, attribution, conflict, reconcile decision, diff). macOS / Windows: full, incl. the reconcile prompt (refuse → Save-As / Reload / Overwrite / Cancel) + attributed-undo menu. Android (Tier A): transactions/revision/snapshots/identity/attribution via JNI for free; the reconcile prompt and AI staging ship with the Tier-A `.RISEscene` save wiring (§11.6); external-MCP propose-only applies; attributed-undo menu degrades to a simple Undo button (label/glyph optional) — never a broken control.
- **Performance budget.** Commit overhead is one extra `uint64` increment + one snapshot publish + one `TxnMeta` push, all under a lock RISE already takes — **no interactive-frame regression** (the cancel-and-park cost is unchanged; transactions reuse it, and a drag publishes only once on pointer-up — §4.5). The snapshot is a **clone of the mutable wrapper state** (§3.5), not a thin handle: **measured ≈3.5–4 µs warm for a 2-object scene** (`SceneSnapshotTest.cpp`), scaling ~2–5 ms per 100 objects by the design analysis (§3.5.1) — the *publish* itself is cheap, but the **restore** path (not yet built) carries the dominant cost (TLAS + `LightSampler` rebuild, §3.7). The old "`O(handles)` addrefs / sub-microsecond publish" estimate is **retired** (it described an un-implemented pure-addref handle). `CheckDiskSync` is one `stat()` on focus/save, not per-frame. **Zero production-render impact** (cite the L8 ~0.4% bar): integrators are byte-identical; the layer never runs *inside* a render pass — it only takes/hands a snapshot before a pass starts (under cancel-and-park, §3.7 item 3).
- **Memory budget.** Per-transaction: `TxnMeta` (~48 B + label) added to the bounded (1024-entry) history — worst case ~tens of KB, trimmed by-composite with the edits. A snapshot clones **only** the mutable wrapper (per-object transform matrices + the `m_transformstack` deque + the camera pose, §3.5) and shares the heavy immutable leaves (geometry/BVH/painters) by refcount; the extra RSS from holding older snapshots is proportional to (concurrently-held snapshots) × (per-object wrapper size × object count), **not** scene size × reader count — bounded and small per revision. Proposed-transaction staging holds at most a handful of `SceneEdit` vectors in `src/Library/Agent/`; no per-feature cache. Peak RSS delta: negligible.
- **Accessibility.** Attributed-undo menu items are full keyboard-reachable (same path as today's Undo/Redo); author distinction is conveyed by **text label** ("Undo AI: …"), not colour/glyph alone (no colour-only dependence). The reconcile prompt is a standard focus-trapped dialog with keyboard defaults (Save a copy / Reload / Keep editing / Cancel). No numpad dependence.
- **Packaging.** No new shipped assets. **The snapshot spike added NO new source file** — `SceneSnapshot` lives in `Scene.cpp` and `CloneSnapshot` / `CaptureSnapshot` are concrete methods on existing classes, so it touched **none** of the five build projects (and added no `tests/` build-project entry beyond the standalone `SceneSnapshotTest.cpp`). The *production* layer's new `SceneTransaction.h` (§12), and any future published-snapshot header, must each be added to **all five build projects** (CLAUDE.md source-add rule); the bridge-enum mirror is generated, not hand-maintained.
- **Migration.** No scene-format change — `.RISEscene` is untouched (the revision, UUID, and snapshots are in-memory only; the disk format is unchanged). ABI: additive C exports only (incl. the opaque `RISE_TxnBuilder` / `Propose` surface, §12); out-of-tree callers unaffected. No auto-migration tool needed.
- **Rollback.** The transaction layer is default-on but behaviorally invisible when only the in-app user edits (the GUI path is identical to today, just routed through `Commit`, which publishes a snapshot the existing readers happily consume). A build-time/feature flag can bypass `Commit` and call `SetProperty`/`Apply` directly (the pre-transaction path) — but note this *also* disables snapshot publication, so background readers would fall back to the *old* live-graph reads; that flag is therefore only safe in the single-reader interactive-only configuration and is for emergency regression isolation, not a shipping mode. Precondition-conflict rejection can be downgraded to last-writer-wins via a controller flag for debugging, but that is **not** a shipping default (it reintroduces the silent-clobber the spec exists to prevent).

### Android tier note
**Tier A** (must-have, per GUI_ROADMAP §10.4 and §16's Android-scene-save commitment). Rationale: the model is 100% shared C++ consumed via JNI, so Android inherits transactions / revisions / published snapshots / document identity / attribution / conflict with **zero** Android-specific logic; the one process-wide worker pool and snapshot lifetime (§3.5) are platform-agnostic. The two surfaces that need shell work — the reconcile prompt (§9, refuse → Save-As / Reload / Overwrite / Cancel) and the AI staging approve/reject (§7, with external-MCP propose-only) — are exactly the Tier-A AI-chat and Tier-A scene-save features Android is already committed to; both are mobile-natural (a swipe-to-reveal diff sheet; a standard "file changed" dialog). The only graceful degradation is cosmetic: the attributed-undo menu may render as a plain Undo affordance on the mobile layout. Nothing in this model is desktop-gated.
