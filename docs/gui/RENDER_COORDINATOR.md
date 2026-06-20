# RISE GUI — Render Coordinator

**Status:** DESIGN. No code. This is the deep-dive spec for the one missing
piece an adversarial review of the seven `docs/gui/` specs surfaced: **there is
no single arbiter for the many render consumers, which all compete for the same
process-global worker pool and the same mutable scene.** **Hardened 2026-06-20**
against a *second* adversarial review held against the now-CONFIRMED concurrency
frame in [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §0 (single-writer; **one
process-wide worker pool**; readers get an immutable published `SceneSnapshot`).
That pass found three mechanical defects (§0.1): the coordinator was described as
*factored from each controller*, so two open documents would each own a
coordinator and render concurrently — violating the one-render-takes-all-cores
rule the doc exists to enforce; the anti-starvation claim ("low-priority work runs
between passes") had no policy implementing it; and the isolated-job model still
read as if this doc *designed* the snapshot rather than *consuming*
TRANSACTION_MODEL's.
**Owner:** Aravind Krishnaswamy
**Scope:** Design a single process-wide shared-C++ **`RenderCoordinator`** (a
singleton lease on the one global CPU worker pool) with **per-document clients**,
that arbitrates *every* full-scene and isolated render across *all open
documents*: interactive viewport refinement, production render, asset/material
thumbnails, node-graph previews, the auto-router probe, RMSE references, and
agent (AI) renders. It unifies the per-spec render policies that today
contradict each other (thumbnails "queue", node previews "budget", MCP
"reject-if-busy", agent "stop/restart the viewport"), defines priority +
exclusivity + a concrete **anti-starvation policy**, cancellation +
stale-revision rejection, a *safe* isolated preview/thumbnail job model that
consumes TRANSACTION_MODEL's immutable `SceneSnapshot`, viewport suspend/resume,
and the API the GUI / MCP / agent call. Excludes scene-state authority,
document identity, and the snapshot primitive itself (that is
[TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3) and side-effect-free
parse/validate (that is [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md)).

This doc sits under [../GUI_ROADMAP.md](../GUI_ROADMAP.md) §9–§11 and honors its
§1 principles — in particular **#2 "maximize shared C++"** (the coordinator is
library code; nothing platform-specific) and **#6 "everything routes through one
mutation path"** (extended here to "everything routes through one *render*
path"). Status headers in the other specs are treated as suspect; all
load-bearing claims cite `file:line` against the
[CURRENT_STATE_AUDIT.md](CURRENT_STATE_AUDIT.md) ground truth.

---

## 0. One-paragraph thesis

RISE already enforces the hard project invariant — **a render takes all cores,
so there is never more than one full-scene render at a time** — but only by
accident of having exactly one consumer: `SceneEditController` owns a single
render thread ([../../src/Library/SceneEditor/SceneEditController.cpp:567](../../src/Library/SceneEditor/SceneEditController.cpp)),
a single-in-flight `mRendering` atomic
([SceneEditController.h:822](../../src/Library/SceneEditor/SceneEditController.h)),
and a "cancel-and-park" handshake every mutation runs before touching the scene
([SceneEditController.cpp:1425-1428](../../src/Library/SceneEditor/SceneEditController.cpp)).
Every pixel rasterizer then fans that one render out across the **process-global**
worker pool, `GlobalThreadPool()`
([../../src/Library/Rendering/PixelBasedRasterizerHelper.cpp:868](../../src/Library/Rendering/PixelBasedRasterizerHelper.cpp)),
which is what makes a render saturate the machine. The roadmap adds six *more*
render consumers (thumbnails, node previews, the probe, RMSE refs, agent renders,
external MCP renders), each spec'd with its own ad-hoc "what do I do when
something else is rendering" policy — and each able to call `RasterizeScene`
directly. Two such consumers running at once would each spawn `numWorkers` tasks
into the *same* pool and the machine becomes unusable (the documented "never kick
off two renders concurrently" rule). And because the worker pool is **one
process-wide singleton** ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §0 #6) but
a session may have **several open documents**, the danger is not just N consumers
*within* one document — it is N consumers *across* documents. The fix is not new
render code; it is to **promote the implicit single-render invariant
`SceneEditController` enforces into one explicit, process-wide
`RenderCoordinator`** that owns the worker pool, holds the one render slot, ranks
all consumers (from every document) by priority under a single anti-starvation
policy, and routes isolated work (thumbnails / previews / probe) onto
**snapshot-isolated jobs** (against TRANSACTION_MODEL's immutable `SceneSnapshot`,
§5) instead of the live film. There is exactly **one** coordinator for the
process; each `SceneEditController` registers as a **client** of it (§2.1). The
coordinator *is* `SceneEditController`'s former render brain, factored out,
made a singleton, and generalized so every document, plus the GUI / MCP / agent,
share the one pool fairly.

### 0.1 Findings this hardening pass closes (second adversarial review, 2026-06-20)

Held against the CONFIRMED frame in [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md)
§0 (single-writer; one process-wide worker pool; readers get an immutable
published `SceneSnapshot`). The v1 draft of this doc predated that frame and was
mechanically wrong in three places.

| # | Blocking finding (v1) | Where v1 was wrong | Closed by |
|---|---|---|---|
| **1** | Coordinator *factored from each controller* (v1 §2.2 "the coordinator *is* `SceneEditController`'s render brain, factored out"; §1.1 #1 said the pool is process-global but the slot owner was per-controller). | With one coordinator **per document** but **one** process-wide pool, two open documents each hold a render slot and each fan out `numWorkers` tasks into the *same* pool — exactly the oversubscription the doc exists to forbid. The single-render invariant is per-document, not process-wide. | §2.1 (one process-wide `RenderCoordinator` singleton; per-document **clients**; the coordinator, not the controller, owns the pool), §2.2 (controller as client), §3.6 (cross-document fair scheduling). |
| **2** | Anti-starvation was asserted, never specified (v1 §3.1 "never starves behind a wall of swatch renders"; §5.4 / §7 "yielding the slot to any Interactive/Production edit that arrives between them"; the worked example claimed thumbnails "run between passes"). | Interactive owns priority 100 *continuously*; under the strict priority queue (v1 §3.2) a priority-40 job is admitted only when nothing ≥100 is runnable, which an always-refining viewport never guarantees. No yield point, no credit/burst scheme, no bound — low-priority work could starve indefinitely, and the worked example described behavior no rule produced. | §3.6 (concrete yield points + credit/burst scheme + starvation bound), §3.2 (admission consults the credit scheme), §7 (worked example rewritten to match). |
| **3** | Isolated jobs read as if this doc *designed* the snapshot (v1 §5.2 "a scene snapshot … owned by TRANSACTION_MODEL" was correct, but §3.5 `RenderRequest` carried a bare `SceneEpoch`, §4.3 invented its own epoch axis, and there was no job lifecycle binding a *held* snapshot ref to a job). | TRANSACTION_MODEL §3.5 now defines the immutable ref-counted `SceneSnapshot` + `AcquireSnapshot()`, and §6.4 names the snapshot's `Id().revision` as *the* stale-result token the coordinator compares. v1 duplicated that with a separate "epoch" and never said the coordinator *acquires/holds/releases* a snapshot ref per isolated job — the very thing that makes one process-wide pool safe (a pooled worker pinning the document it renders). | §3.5 (`RenderRequest` carries a held `SceneSnapshot` ref + `DocumentId`), §4.3 (revision = the snapshot's `Id().revision`, drop-stale at dequeue/completion), §5.2 (acquire-render-release job lifecycle), §5.6 (snapshot lifetime under a job). |

---

## 1. The problem, concretely

### 1.1 One pool, one scene, N would-be consumers

RISE is a CPU path tracer; "the GPU" is a present surface, not a render path
([CURRENT_STATE_AUDIT.md §10](CURRENT_STATE_AUDIT.md)). The render resources that
are genuinely scarce and genuinely shared are:

1. **The global worker pool.** `GlobalThreadPool()` is a process singleton; a
   render calls `pool.ParallelFor( numWorkers, … )` and occupies every core for
   the duration ([PixelBasedRasterizerHelper.cpp:868-878](../../src/Library/Rendering/PixelBasedRasterizerHelper.cpp),
   and the animation path at `:1358-1363`). Thread policy (P-cores + E-cores
   minus one) is owned by `CPUTopology` / `ThreadPool` per CLAUDE.md; the *count*
   is topology-aware but the *pool* is one — **process-wide, not per-document**
   ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §0 #6 confirms render / thumbnail
   / RMSE / probe all draw from this one pool). A session with two open documents
   therefore shares this single pool between them; an arbiter that lived inside
   each document could not see the other document's in-flight render and would
   double-book the pool (finding #1).
2. **The mutable scene + its live film.** The scene is shared, mutable state;
   `IScenePriv::ResizeFilm` writes `width/height/pixelAR` as **non-atomic
   stores** and its doc comment states the contract bluntly: it "must NOT run
   concurrently with rendering … a concurrent reader on another thread would see
   a torn (width-new, height-old) triple"
   ([../../src/Library/Interfaces/IScenePriv.h:219-244](../../src/Library/Interfaces/IScenePriv.h)).
3. **The canonical `FrameStore`.** One generation-counted framebuffer the UIs
   poll (`FrameStore::Generation()`,
   [../../src/Library/Rendering/FrameStore.h:206-209](../../src/Library/Rendering/FrameStore.h)).
4. **The single geometry-freeze window.** `RenderParallelScope`
   ([PixelBasedRasterizerHelper.cpp:877, 1362](../../src/Library/Rendering/PixelBasedRasterizerHelper.cpp))
   asserts geometry is realized single-threaded before the parallel fan-out and
   frozen during it — a second concurrent render would violate that guard.

Every one of these is a process-global or scene-global singleton. There is no
per-consumer pool, no per-consumer film, and **no per-consumer scene** today.

### 1.2 The conflicting per-spec policies (what this doc unifies)

The review found each consumer invented its own arbitration rule, and they do not
compose:

| Consumer | Spec | Today's ad-hoc policy | Problem |
|---|---|---|---|
| Interactive viewport | [APPROACHABILITY_FOUNDATION.md](APPROACHABILITY_FOUNDATION.md) A2 | self-cancels on edit; drops to ½/¼-res via `ResizeFilm`; refines on idle | already correct *for the single consumer*; assumes it owns the pool |
| Production render | GUI_ROADMAP §11 | `Stop()` the viewport (join thread), render synchronously, `Start()` | correct, but the stop/restart logic is hand-rolled per call site |
| Asset/material thumbnail | [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) C2, [APPROACHABILITY_FOUNDATION.md](APPROACHABILITY_FOUNDATION.md) A5 | "render thumbnails" / "queue them" | no queue exists; would call `RasterizeScene` on the live film/pool |
| Node-graph preview | [MATERIAL_EDITOR.md](MATERIAL_EDITOR.md) C2 | per-node live thumbnails on a "budget" | no budget mechanism; same pool collision |
| Auto-router probe | [AUTO_RASTERIZER_DESIGN.md](../AUTO_RASTERIZER_DESIGN.md), [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D5 | `ResizeFilm`-down → probe → `ResizeFilm`-up **on the live film** | safe ONLY because it runs inside the render's own `call_once`, pre-fan-out ([AutoRasterizer.cpp:486-490, 538-540](../../src/Library/Rendering/AutoRasterizer.cpp)); unsafe as a generic pattern |
| RMSE reference | [SPECTRAL_DIFFERENTIATORS.md](SPECTRAL_DIFFERENTIATORS.md) D5, variance-measurement skill | offline, high-spp, denoise-off | a *long* full-scene render; must not collide with the viewport |
| Agent (AI) render | [LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md), [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) | render-control tools "stop and restart the viewport" | re-implements the production stop/restart; can be invoked from a non-UI thread |
| External MCP render | [MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) | "reject if busy" | a flat reject is wrong for a thumbnail but right for a second production render — the policy depends on *type*, not a global busy bit |

These are five different answers ("queue", "budget", "reject-if-busy",
"stop/restart", "runs inline") to one question. The coordinator replaces all of
them with a single priority + admission policy.

### 1.3 Why "just add a second render path" is wrong

`AutoRasterizer`'s live-film `ResizeFilm` round-trip is *correct in its current
home* and the review flagged it precisely so it is **not** copied as a generic
thumbnail/preview architecture. It is safe only because it runs single-threaded
inside the dispatcher's `std::call_once`, **strictly before** the real render's
worker threads spawn, and restores the dims before returning
([AutoRasterizer.cpp:486-490](../../src/Library/Rendering/AutoRasterizer.cpp)).
A thumbnail or node preview that did the same thing while the viewport thread was
live would (a) tear the film mid-read, (b) double-book the global pool, and (c)
require stopping the render thread first per the `ResizeFilm` contract. Isolated
work needs an *isolated private film* and a *held immutable
[`SceneSnapshot`](TRANSACTION_MODEL.md)* (TRANSACTION_MODEL §3.5), not a borrow of
the live film or a read of the live graph (see §5).

---

## 2. Design overview

### 2.1 One process-wide coordinator, per-document clients, one slot

`RenderCoordinator` (new, shared C++; proposed home
`src/Library/Rendering/RenderCoordinator.{h,cpp}`) is a **process-wide
singleton** — there is exactly one for the whole application, obtained via
`RenderCoordinator::Instance()`, with the same lifetime as `GlobalThreadPool()`
([PixelBasedRasterizerHelper.cpp:868](../../src/Library/Rendering/PixelBasedRasterizerHelper.cpp)).
It is the **sole owner of the render slot** and the **sole submitter to the
global worker pool for full-scene-class work**, *across every open document*. It
does not contain any integrator; it composes the existing rasterizers exactly as
`AutoRasterizer` composes PT/BDPT/VCM
([AutoRasterizer.h:7-8](../../src/Library/Rendering/AutoRasterizer.h)).

**Per-document clients.** Each open document — i.e. each `SceneEditController`
(one controller = one document = one `(UUID, revision)` lineage,
[TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §13 non-goals "multi-document") —
**registers as a client** of the one coordinator and gets a `RenderClientId`. A
client is a *submission source bound to a document*, not an owner of the pool:

```cpp
// RenderCoordinator (shared C++) — registration surface
RenderClientId RegisterClient( const std::string& documentLabel );  // one per controller
void           UnregisterClient( RenderClientId );                  // on document close
```

`SceneEditController` calls `RegisterClient` in its constructor and
`UnregisterClient` in its destructor. Every `RenderRequest` carries its
`RenderClientId` (§3.5) so the coordinator can (a) attribute the job to a
document, (b) schedule *fairly across documents* (§3.6), and (c) drop every job
belonging to a client when that client unregisters (document close / reload's
document-swap, [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §9.3). The coordinator
owns the slot, the queue, and the pool; the controller owns only its document and
its submissions. This is the crux of finding #1: arbitration cannot live *inside*
a document, because the scarce resource (the pool) is shared *between* documents.

Core invariants the coordinator guarantees by construction:

- **I1 — At most one full-scene render runs at a time, process-wide.** This is the
  hard project rule (CLAUDE.md / MEMORY "never kick off two renders
  concurrently"). It is enforced *here*, once, for the whole process — **not
  per-document** — instead of being re-derived by each consumer or each open
  window. (Isolated thumbnail/preview jobs are also full-pool renders and obey the
  same single-slot rule — see §5.4 for why "tiny" does not mean "free".)
- **I2 — A render takes all cores.** The coordinator hands the active job the
  full `GlobalThreadPool()`. There is no core-splitting / co-scheduling; the
  project rule is exclusivity, not fair-share of cores. (Fairness is over *time*
  — whose job runs next — never over *cores* within a job; §3.6.)
- **I3 — Every job carries the `DocumentId` it was admitted against.** A job whose
  document revision is superseded by completion (the scene changed under it) has
  its result dropped, not displayed (§4.3). The revision is the snapshot's
  `Id().revision` from [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5 / §6.4 —
  the coordinator *consumes* that token, it does not mint its own.
- **I4 — Isolated jobs never touch the live film or live scene.** They render
  against a *held* immutable `SceneSnapshot` (acquired from
  [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5) + a private `IFilm` (§5).
- **I5 — All admission, preemption, completion, and cross-document fairness
  decisions are made on the coordinator's own thread under one lock.** No
  consumer — and no document — pokes the pool, the live film, or another job
  directly.
- **I6 — No client may starve.** Every registered client makes guaranteed
  forward progress under the credit/burst scheme, with a bounded worst-case wait
  (§3.6). This holds both across priority classes (a wall of thumbnails behind a
  busy viewport) and across documents (two windows competing for the one pool).

### 2.2 Relationship to `SceneEditController` (controller becomes a client)

The render *mechanism* the controller hand-rolls today moves **behind the shared
singleton**, and the controller is re-cast as one of its clients. With a single
open document this is a pure factor-out; the design only diverges from "factored
out" when a *second* document exists, where the shared singleton — not a
per-document brain — is what keeps the pool single-booked (finding #1).

- The controller's single `mRenderThread` / `RenderLoop`
  ([SceneEditController.cpp:567, 2515](../../src/Library/SceneEditor/SceneEditController.cpp)),
  its `mRendering` single-in-flight atomic
  ([SceneEditController.h:822](../../src/Library/SceneEditor/SceneEditController.h)),
  its `mCancelProgress` (`CancellableProgressCallback`,
  [SceneEditController.cpp:549, 584](../../src/Library/SceneEditor/SceneEditController.cpp)),
  and its preview-scale pump
  ([SceneEditController.cpp:3754-3756, 3830-3869](../../src/Library/SceneEditor/SceneEditController.cpp))
  all **move into / behind** the singleton `RenderCoordinator`. The per-document
  `mRendering` atomic is *replaced* by the coordinator's one process-wide slot:
  "is *this document* rendering" becomes "does the active slot belong to this
  client." The render thread is no longer per-controller; the **one** pool serves
  every client (finding #1, [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §0 #6).
- `SceneEditController` becomes a *registered client* (§2.1): on construction it
  `RegisterClient`s; it submits a long-lived `Interactive` job for its viewport
  and feeds edits to the coordinator; on destruction it `UnregisterClient`s and
  the coordinator drops that client's queued + active jobs. Its cancel-and-park
  mutation handshake (§1.1) is preserved but expressed as
  `coordinator.SuspendForEdit(clientId)` / lease-drop (§6).
- Production render's hand-rolled `Stop()`→synchronous-`RasterizeScene`→`Start()`
  ([SceneEditController.cpp:1646-1698](../../src/Library/SceneEditor/SceneEditController.cpp))
  becomes "submit a `Production` job; the coordinator suspends *this client's*
  viewport for its duration" (§6) — same observable behavior, one implementation,
  and now correct when another document is also live (its viewport keeps its place
  in the fair schedule rather than being stomped).

This keeps roadmap principle #6 intact: GUI, AI, and hand-edits still converge on
`SceneEditController` for *mutation*; now they also converge on the **one**
`RenderCoordinator` for *rendering*. Mutation authority stays per-document (each
controller's `mMutex`); render arbitration becomes process-global (the one
coordinator). The two are deliberately different scopes —
[TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) owns the former, this doc the latter.

### 2.3 Shared-C++ vs platform-specific

| Component | Shared C++ (`src/Library/`) | Platform-specific (thin shell) |
|---|---|---|
| `RenderCoordinator` **singleton** (slot, queue, priority, preemption, revision check) | all of it | — |
| Client registry (`RegisterClient` / `UnregisterClient`, per-document) | all of it | — |
| Cross-document fair scheduling + anti-starvation (credits/bursts, yield points) | all of it (§3.6) | — |
| Worker-pool ownership / fan-out | all of it (already process-wide `GlobalThreadPool`) | — |
| Job model (`RenderRequest`, `JobHandle`, `RenderClientId`, priorities) | all of it | — |
| Snapshot-isolated thumbnail/preview job (private `IFilm` + held `SceneSnapshot`) | all of it; the `SceneSnapshot` itself is owned by [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5 | — |
| Cancellation / stale-revision plumbing | all of it (`CancellableProgressCallback`, `FrameStore::Generation`, snapshot `Id().revision`) | — |
| Viewport suspend/resume around production & agent renders | all of it | — |
| Result delivery | coordinator → `IRasterizerOutput` / `FrameStore` (shared) | the present surface that *displays* the framebuffer (Metal / DXGI / Android bitmap) — display only, unchanged |
| Submitting renders | `SceneEditController` (per document), MCP dispatch, agent loop all call the shared API | the input event that *triggers* a submit (button, gesture, chat send) |

The pay-off mirrors [CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md):
the coordinator is written once and macOS, Windows, and Android (Tier A render
view) get correct arbitration for free.

---

## 3. Job model

### 3.1 Consumer classes → priority

Eight render consumers, collapsed onto a small priority lattice. Higher number =
higher priority. Priority drives *admission* and *preemption*, not core count
(I2).

| Priority | Class | Consumer(s) | Render kind | Default policy when slot busy |
|---|---|---|---|---|
| 100 | `Interactive` | Viewport refinement | live film, scaled | **owns the slot by default**; yields to 200+; never queues (latest edit wins) |
| 200 | `Production` | "Render" button, `render_*` MCP/agent tool, animation frame | live film, full-res | **preempts** Interactive (suspend); queues behind another Production/RMSE |
| 200 | `Agent` | Agent autonomous render (L2/L3) | live film, full-res | identical to Production (it *is* a production render initiated by the agent) |
| 180 | `RmseReference` | RMSE / variance reference render | isolated film, high-spp, denoise-off | preempts Interactive; queues behind Production; long-lived |
| 60 | `ProbeInternal` | Auto-router Tier-2 probe | runs *inside* the owning render (not a separate slot) | N/A — see §3.4 |
| 40 | `Thumbnail` | Asset/material library swatches | isolated film, tiny, denoise-off | **queues**; coalesces duplicates; preempted by 100+ — but earns guaranteed slices via the anti-starvation credit scheme (§3.6) |
| 40 | `NodePreview` | Node-graph per-node previews | isolated film, tiny | **queues** with a debounce + cap; preempted by 100+; same credit scheme (§3.6) |
| 40 | `IsolatedAdhoc` | MCP "render this snippet" / vision-loop preview | isolated film | queues; preempted by 100+; same credit scheme (§3.6) |

`Interactive` deliberately sits *below* `Production`/`Agent` so a render
"preempts" the viewport (suspends it), matching the existing
`Stop()`→render→`Start()` behavior. It sits *above* thumbnails/previews so
typing in the viewport feels instant.

> **Priority alone does NOT prevent starvation — and is not relied on to.** A
> priority-40 job admitted "only when nothing ≥100 is runnable" would never run
> against a viewport that refines continuously. The priority lattice decides
> *preemption* and *who-runs-when-the-slot-frees*; the **guarantee** that
> low-priority and other-document work makes forward progress comes from the
> anti-starvation policy in **§3.6** (yield points + a credit/burst scheme + a
> bounded worst-case wait), which §3.2 admission consults. Read §3.2 and §3.6
> together: §3.2 is the fast-path decision, §3.6 is the fairness override that
> keeps it from starving anyone.

### 3.2 Preempt vs queue — the decision table

When a job is submitted and the slot is occupied by `active`:

```
submit(new) with slot held by active:
  if new.priority > active.priority:
      if active.preemptible:   suspend(active) → run(new) → on completion resume(active)   // Interactive ← Production
      else:                    enqueue(new) at front                                         // wait out a non-preemptible run
  elif new.priority == active.priority:
      enqueue(new) FIFO (fairness within a class)                                            // second Production waits
  else: // new.priority < active.priority
      enqueue(new) by (priority desc, client-fair-order, submit-time asc)                    // thumbnails wait for the viewport (§3.6 client fairness)
```

And the dual decision — **what the coordinator picks next when the slot frees**
(`PickNext`) — is *not* "highest priority wins unconditionally"; it is gated by
the §3.6 anti-starvation policy:

```
PickNext() when slot frees (runs on the coordinator thread, under the lock):
  if a starvation-credit job is DUE (§3.6):     return that job        // fairness override
  else:                                          return head of (priority desc, client-fair-order, seq asc)
```

The credit override is what makes the priority-40 `Thumbnail`/`NodePreview` and
the other-document jobs progress even while a priority-100 viewport keeps
re-submitting (§3.6). Without it, the strict-priority `PickNext` above would
starve them — the v1 defect (finding #2).

- **Interactive is preemptible** (it is cheap to suspend/resume; it already
  self-cancels per pass). Production/Agent/RMSE/Thumbnail/NodePreview are **not
  preemptible mid-run** — they run to completion or are *cancelled* (dropped),
  never paused-and-resumed, because their partial state isn't a useful resumable
  artifact the way the viewport's persistent buffer is
  ([PixelBasedRasterizerHelper.h:147-164](../../src/Library/Rendering/PixelBasedRasterizerHelper.h)).
- **"Suspend" the Interactive job** = cancel its current pass via the existing
  flag (§4.1) and *not* re-admit it until the higher job completes; its
  persistent framebuffer is retained so it resumes without a black flash
  ([InteractivePelRasterizer](../../src/Library/Rendering/InteractivePelRasterizer.cpp)
  `PrepareImageForNewRender` skips the clear).

### 3.3 Fairness & coalescing

- **Within a class, FIFO by submit time** (the queue is a stable priority queue
  keyed `(priority desc, seq asc)`).
- **Latest-wins for Interactive, per client.** Only one Interactive job exists
  *per client* (one per open document); a new edit cancels that client's in-flight
  pass and supersedes it (today's `KickRender`,
  [SceneEditController.cpp:2400-2427](../../src/Library/SceneEditor/SceneEditController.cpp)).
  Two documents each have their own Interactive job; they alternate on the slot via
  client fairness (§3.6.4), they do not supersede each other.
- **Coalesce identical isolated jobs.** Thumbnail/NodePreview requests carry a
  *content key* (entity id + slot-values hash + `DocumentId` revision + target px).
  A submit whose key matches a queued-or-running job attaches to it instead of
  enqueuing a duplicate (the node-graph "budget" the spec wanted, expressed as
  dedup + a bounded per-client queue depth, e.g. cap 64, drop-oldest).
- **Debounce previews.** NodePreview submits are debounced (~80 ms) at the
  coordinator so a slider drag does not enqueue 200 previews.

### 3.4 The probe is not a slot — it is part of a render

The auto-router Tier-2 probe is explicitly **not** an independently scheduled
consumer. It runs inside the owning render's `EnsureResolved` /
`std::call_once` window, single-threaded, before that render's worker fan-out
([AutoRasterizer.h:18-26](../../src/Library/Rendering/AutoRasterizer.h),
[AutoRasterizer.cpp:486-540](../../src/Library/Rendering/AutoRasterizer.cpp)).
Because the coordinator already guarantees that render holds the slot exclusively
(I1) and is pre-fan-out at that moment, the probe's existing live-film
`ResizeFilm` round-trip remains valid **unchanged** — it executes while the
coordinator's slot is held and no workers are live. The coordinator's only
obligation is to never admit anything else during that window, which I1 already
gives. This is the one place the live-film `ResizeFilm` fast path stays (§5.5).

### 3.5 API surface (the one entry point GUI / MCP / agent call)

```cpp
namespace RISE {

enum class RenderClass {            // §3.1
    Interactive, Production, Agent, RmseReference,
    Thumbnail, NodePreview, IsolatedAdhoc
};

struct RenderRequest {
    RenderClass     cls;
    RenderClientId  client;         // §2.1 — which document submitted (fairness + drop-on-close)
    // Document identity + the held snapshot the job reads.  BOTH come from
    // TRANSACTION_MODEL §3.4/§3.5 — this doc consumes them, does not define them.
    DocumentId      baseDocument;   // (UUID, revision) admitted against; stale-drop key (§4.3)
                                    //   NOTE: DocumentId is production-layer, NOT in the spike (§5 status note).
    SceneSnapshot*  snapshot;       // isolated jobs: an addref'd, held SceneSnapshot.
                                    //   The TYPE is real (Scene.cpp:862, clones mutable wrapper +
                                    //   addref-shares leaves; produced today by Scene::CreateSnapshot,
                                    //   Scene.cpp:927).  The published-pointer acquire
                                    //   SceneEditController::AcquireSnapshot() and the intrinsic Id()
                                    //   are the not-yet-built production seam (TRANSACTION_MODEL §3.5.4),
                                    //   such that Id().revision == baseDocument.revision.  null for
                                    //   live-film jobs, which read the canonical scene under the slot (§5.5).
    // Target: exactly one of —
    //   live    : render the canonical scene into the canonical FrameStore (Interactive/Production/Agent)
    //   isolated: render `snapshot` into a private IFilm (Thumbnail/NodePreview/RMSE/Adhoc) (§5)
    RenderTarget    target;
    IntegratorChoice integrator;    // pt|bdpt|vcm|auto (auto → AutoRasterizer)
    RenderParams    params;         // spp / region / resolution-scale / denoise / spectral core
    ContentKey      coalesceKey;    // optional; enables §3.3 dedup for isolated jobs
    IRasterizerOutput* sink;        // where results go (FrameStore for live; capturing sink for isolated)
    IRenderObserver*   observer;    // progress / completion / dropped-stale callbacks (coordinator thread)
};

// ONE instance for the whole process (a singleton lease on GlobalThreadPool, §2.1).
// Obtained via RenderCoordinator::Instance(); not constructed per document.
class IRenderCoordinator : public IReference {
public:
    // Register / unregister a document as a client (§2.1).  Each SceneEditController
    // registers in its ctor and unregisters in its dtor; unregister drops every
    // queued + active job for that client (close / reload document-swap).
    virtual RenderClientId RegisterClient( const std::string& documentLabel ) = 0;
    virtual void           UnregisterClient( RenderClientId ) = 0;

    // Submit a render. Returns immediately with a handle; the coordinator
    // schedules per §3.2/§3.6. For Interactive, supersedes any in-flight
    // Interactive *for the same client*.  req.client identifies the document;
    // req.snapshot (isolated jobs) is the held SceneSnapshot the job reads (§5.2).
    virtual JobHandle Submit( const RenderRequest& req ) = 0;

    // Cancel a specific job (cooperative; §4). No-op if already done/dropped.
    virtual void      Cancel( JobHandle h ) = 0;

    // Editing handshake for a client's live scene (§6): suspend the active render
    // so the caller can mutate THAT document safely, then resume. Used by each
    // SceneEditController's cancel-and-park sites and by Production/Agent admission.
    virtual EditLease SuspendForEdit( RenderClientId ) = 0;  // RAII; blocks until the slot is parked

    // Introspection for UIs ("Rendering production…", queue depth, "Auto → VCM").
    // Per-client when given a client id; process-wide aggregate otherwise.
    virtual RenderStatus Status( RenderClientId = {} ) const = 0;
};

}
```

`JobHandle` is a lightweight token; `EditLease` is RAII (its destructor calls
`ResumeAfterEdit`). `RenderRequest` carries the submitting `RenderClientId`
(§2.1) and a held `SceneSnapshot` ref for isolated jobs (§3.5 / §5.2).
`IRenderObserver` callbacks fire on the coordinator thread with one of
`{progress, completed, cancelled, dropped_stale}`; the platform marshals to its
UI thread exactly as the bridges already marshal `FrameStore` generation polls
today
([RISEBridge.mm:500](../../build/XCode/rise/RISE-GUI/Bridge/RISEBridge.mm)).

### 3.6 Anti-starvation policy (yield points, credits, and the bound)

Finding #2: priority alone starves low-priority and other-document work. This
section is the concrete policy that the §3.2 `PickNext` consults. It has three
parts — **yield points** (when the high-priority occupant gives the slot up),
a **credit/burst scheme** (so the freed slot deterministically goes to
otherwise-starved work), and a **starvation bound** (the worst-case wait every
job is guaranteed).

#### 3.6.1 Yield points — when the slot becomes available to lower work

The Interactive viewport is the only *continuously re-submitting*, *preemptible*
occupant, so it is the only one that must actively yield. Two yield points,
both already latent in the existing render loop:

1. **Between interactive refinement passes.** The interactive rasterizer renders
   in passes at increasing resolution (the preview-scale pump,
   [SceneEditController.cpp:3754-3756, 3830-3869](../../src/Library/SceneEditor/SceneEditController.cpp),
   `mPreviewScale` [SceneEditController.h:867-870](../../src/Library/SceneEditor/SceneEditController.h)).
   The coordinator inserts a **scheduling checkpoint at each pass boundary**: when
   an Interactive pass completes, control returns to the coordinator thread, which
   runs `PickNext` (§3.2) *before* re-admitting the next Interactive pass. If a
   credit is due (§3.6.2), the lower-priority/other-document job runs in that gap;
   the Interactive job re-enqueues at priority 100 and resumes after it (its
   buffer is retained, §4.2 — no flash). This is the mechanism the v1 worked
   example *described* ("run between passes") but never specified.
2. **When the viewport is idle / converged.** The interactive refinement
   terminates when it reaches full-res and no edit is pending (the polish pass
   completes and the loop parks). At that point the Interactive job is **not
   runnable** — it has nothing to refine — so `PickNext` naturally drains the
   low-priority queue at full rate with no credit needed. An incoming edit makes
   Interactive runnable again and it preempts on the next checkpoint. "Idle" is
   precisely "Interactive has no pending pass," which the existing
   `mEditPending`/polish-state machine already tracks
   ([SceneEditController.cpp:1367-1371](../../src/Library/SceneEditor/SceneEditController.cpp)).

Production/Agent/RMSE are non-preemptible (§3.2) and run to completion, so they
have no mid-run yield point; fairness *among* them and *against* a waiting
viewport is handled at their completion boundary by the same `PickNext`. (A
single Production render is bounded by its own spp; it is not an infinite
re-submitter, so it cannot starve anything indefinitely on its own.)

#### 3.6.2 Credit / burst scheme — guaranteeing forward progress

Pure "yield if something is waiting" is not enough: a viewport that re-submits a
new pass every 16 ms could let each lower job run one tile and then reclaim the
slot forever, so a thumbnail never *finishes*. The fix is an **aging-credit**
scheme with **bursts**, evaluated under the coordinator lock:

- **Aging.** Every *runnable* job that is **not** the highest-priority class
  accrues a credit each time it is **passed over** at a `PickNext` checkpoint
  (i.e. a higher-priority job was chosen instead). Credit accrues per job and,
  separately, per **client** (so fairness holds both across classes *and* across
  documents — §3.6.4).
- **Due.** A job becomes **due** when its accrued credit crosses a threshold
  `kStarveCredits` (a count of consecutive pass-overs, default tuned so the bound
  in §3.6.3 holds — see below). A due job is selected by `PickNext` ahead of
  higher-priority work *for one burst*.
- **Burst.** When a due job is selected, it runs a **bounded burst** — not "until
  done," which could stall the viewport for a long isolated render, and not "one
  tile," which never finishes a thumbnail. The burst is **one whole isolated job**
  for tiny jobs (`Thumbnail`/`NodePreview`/`IsolatedAdhoc` are 128²/256² and
  complete in well under the interactive pass budget, §5.4) — i.e. a due
  thumbnail runs to completion in its turn, then the viewport reclaims the slot.
  For a *long* low-priority job (a `RmseReference` that somehow sits below a
  viewport — it does not by default, §3.1, but the rule must be total) the burst
  is bounded by a **wall-clock quantum** `kBurstQuantumMs` (the job is
  cooperatively cancelled at the next tile boundary per §4.1 if it overruns, and
  re-enqueued with its credit reset; it resumes its turn next cycle). After a
  burst, the served job's credit resets and the viewport resumes.
- **Reset.** Completing a burst resets that job's (and contributes to its
  client's) credit. The highest-priority class never accrues credit (it does not
  need protection); it is the thing other jobs are protected *from*.

Concretely, a library refresh that submits 30 thumbnails behind a continuously
dragging viewport drains at a steady rate: every `kStarveCredits` viewport passes,
one whole thumbnail completes in the inter-pass gap, until the grid fills — while
each viewport pass still lands within its latency target (§9 performance budget,
≤33 ms/pass; a 128² thumbnail burst fits the gap).

#### 3.6.3 The starvation bound

> **Bound B1 — every runnable job runs within a bounded number of
> higher-priority admissions.** A job at priority *p* is passed over at most
> `kStarveCredits` times before it becomes due and is served for one burst.
> Therefore its worst-case wait is `kStarveCredits` higher-priority jobs (or
> bursts) ahead of it, **not unbounded**. With the default `kStarveCredits = K`
> and a per-pass viewport cost ≤33 ms, a thumbnail behind a busy viewport starts
> within ≈ `K × 33 ms` and, being one-burst-to-completion, finishes one burst
> later — a small constant, independent of how long the user keeps dragging.

> **Bound B2 — no client is starved by another client.** Because credit accrues
> per client as well as per job (§3.6.4), a second open document's viewport (or
> its thumbnails) becomes due against the first document's viewport within the
> same `kStarveCredits` window. Two windows dragging at once therefore *alternate*
> at the credit cadence rather than one freezing the other — the multi-document
> guarantee (I6).

`kStarveCredits` and `kBurstQuantumMs` are the two tunables; both have defaults
and both are surfaced for the §9 acceptance test (the starvation-bound test
asserts B1 with a fixed `kStarveCredits`). They trade promptness of low-priority
work against viewport smoothness; the defaults favor viewport smoothness (the
project's interactivity priority) while still guaranteeing the bound.

#### 3.6.4 Two-level fairness: class, then client

`PickNext`'s ordering key is `(due-credit first, then priority desc, then
client-fair-order, then seq asc)`. The **client-fair-order** term is
round-robin-with-credit across `RenderClientId`s at the same priority, so:

- Within one document, classes are ranked by priority but no class starves
  (§3.6.2).
- Across documents, same-priority work alternates fairly (B2) — e.g. two windows
  each with a viewport share the slot in turn; two windows each refreshing a
  thumbnail grid interleave their thumbnails rather than one document draining
  entirely first.

This two-level scheme is the direct answer to finding #1's corollary: once the
coordinator is process-wide with per-document clients, *fairness across
documents* becomes a first-class requirement, not an afterthought. A
single-document session behaves exactly as before (one client → the client term
is a no-op).

---

## 4. Cancellation + stale-revision rejection

Reuse the existing two mechanisms verbatim — do not invent new ones.

### 4.1 Cooperative cancel (the `cancelled` flag)

The block dispatcher already polls a `std::atomic<bool> cancelled`
([../../src/Library/Rendering/RasterizeDispatchers.h:37, 63-65, 99-103](../../src/Library/Rendering/RasterizeDispatchers.h)),
fed by `IProgressCallback::Progress()` returning false / `IsCancelled()` going
true
([../../src/Library/Interfaces/IProgressCallback.h:25-27, 30-51](../../src/Library/Interfaces/IProgressCallback.h)),
and `CancellableProgressCallback` is the concrete carrier
([../../src/Library/SceneEditor/CancellableProgressCallback.cpp:32-40](../../src/Library/SceneEditor/CancellableProgressCallback.cpp)).
The coordinator owns **one `CancellableProgressCallback` per live render slot**
plus one per isolated job, and wires it as the job's progress callback
(`SetProgressCallback`, exactly as
[SceneEditController.cpp:3655](../../src/Library/SceneEditor/SceneEditController.cpp)
does today). `Cancel(handle)` and preemption both call `RequestCancel()` on the
target job's callback; the dispatcher aborts at the next tile boundary and
returns. This is the *only* cancellation primitive; the coordinator does not
kill threads.

### 4.2 Suspend = cancel-the-pass, retain-the-buffer

Suspending the Interactive job is `RequestCancel()` on its callback + *not
re-admitting it* until the preemptor finishes. Its persistent buffer survives
([PixelBasedRasterizerHelper.h:147-164](../../src/Library/Rendering/PixelBasedRasterizerHelper.h)),
so resume is a fresh pass on top of existing pixels (no flash). Resume re-arms
its callback (`Reset()`,
[CancellableProgressCallback.cpp:37-40](../../src/Library/SceneEditor/CancellableProgressCallback.cpp))
and re-enqueues at priority 100.

### 4.3 Stale-revision rejection (drop, don't display)

> **Production-layer (depends on the revision/`DocumentId` work not in the spike).**
> The stale-drop logic below keys on `DocumentId` / `snapshot->Id().revision` /
> `CurrentDocumentId()`, all of which are the publication layer
> [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5.4 still has to build (the
> prototyped `SceneSnapshot` carries no `DocumentId` yet). The rule is sound and
> is what the coordinator *will* enforce; it is not exercisable until that seam
> lands.

Every `RenderRequest` is admitted against a `DocumentId = (UUID, revision)`
([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.4). For isolated jobs that
revision is **intrinsic** to the held `SceneSnapshot` —
`req.snapshot->Id().revision == req.baseDocument.revision` — which is exactly the
stale-result token TRANSACTION_MODEL §3.5 / §6.4 hands the coordinator ("the
snapshot's `Id().revision` *is* the coordinator's stale-result token"). The
coordinator does **not** mint its own counter; it compares the snapshot's
revision against the live document's. This is the coordinator's
**stale-generation rule**. Two checkpoints, both keyed on the *pair*:

1. **At dequeue (pre-run):** read the submitting client's live `DocumentId` via
   `controller.CurrentDocumentId()` (TRANSACTION_MODEL §3.5). If
   `job.baseDocument != live` — either the **revision** moved (the scene was
   edited while the job waited) or the **UUID** moved (the document was reloaded /
   swapped, [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §9.3) — **discard the
   job** before it ever runs. Observer gets `dropped_stale`. This is what makes a
   queue of thumbnails self-clean when the user reloads or edits the scene. (A
   UUID mismatch is the reload case; that client is also unregistering, so its
   jobs are dropped on that path too — §2.1. The dequeue check is the belt for an
   in-place edit that bumps only the revision.)
2. **At completion (post-run):** if the revision advanced *during* the run (an
   edit committed via the controller while the job ran), the produced pixels
   describe a superseded scene. For **Interactive** this is benign (the next pass
   supersedes anyway). For **isolated** jobs, the result is delivered only if
   `job.baseDocument == controller.CurrentDocumentId()` at completion; otherwise
   dropped. Note the isolated job *itself* never saw the new revision — it read
   its **held immutable snapshot** for its whole life (TRANSACTION_MODEL §3.5
   "an in-flight job on snapshot N runs to completion against stable N; its result
   is then reconciled against the live revision by the render coordinator"); the
   completion check only decides whether the *finished* result is still worth
   showing. The canonical `FrameStore::Generation()`
   ([FrameStore.h:206-209](../../src/Library/Rendering/FrameStore.h)) remains the
   *display* freshness signal; the **document revision** is the *correctness*
   signal. They are different axes — generation says "the framebuffer changed,"
   revision says "the world the framebuffer describes changed."

Because the coordinator is the single mutation-aware render gate, the
suspend-for-edit handshake (§6) means live renders normally *cannot* be running
across a revision bump in the first place; checkpoint (2) is the belt-and-braces
for edits that land via a non-coordinator path and is the same class of guard as
SaveEngine's external-mod span check
([CURRENT_STATE_AUDIT.md §1](CURRENT_STATE_AUDIT.md)).

> **Terminology — "revision," not "epoch."** This doc tracks the per-commit
> **revision** (TRANSACTION_MODEL §3.2's `mRevision`, bumped once per committed
> transaction with a snapshot publish), **not** the reload sentinel `mSceneEpoch`
> ([SceneEditController.h:765](../../src/Library/SceneEditor/SceneEditController.h)),
> which TRANSACTION_MODEL §3.2 keeps strictly separate as a list-rebuild signal.
> The full identity the coordinator matches is the `(UUID, revision)` pair, so a
> reloaded document (fresh UUID) can never coincidentally match a stale job's base
> ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.4, finding #3 there).

---

## 5. Isolated preview / thumbnail jobs (the safe path)

The review's central correctness ask: **do not** reuse `AutoRasterizer`'s
live-film `ResizeFilm` round-trip as the thumbnail/preview architecture. Here is
the safe alternative.

> **Implementation status (what this section consumes is partly built).** The
> snapshot primitive this section relies on is **PROTOTYPED & TESTED** on branch
> `feature/gui-snapshot-prototype`
> ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5): `Scene::CreateSnapshot()`
> ([`Scene.cpp:927`](../../src/Library/Scene.cpp)) returns a ref-counted
> `SceneSnapshot` ([`Scene.cpp:862`](../../src/Library/Scene.cpp)) that **clones**
> the document's mutable wrapper state (object transforms via
> `Object::CloneSnapshot`, [`Object.cpp:103`](../../src/Library/Objects/Object.cpp);
> camera pose by value) and **addref-shares** the immutable leaves. It is proven
> independent of later live mutation by `tests/SceneSnapshotTest.cpp` (18/18 +
> negative control), and **measured ≈3.5–4 µs warm for a 2-object scene** (~2–5 ms
> per 100 objects by the design analysis). **What is NOT yet built** and this
> section therefore describes as the *target*: (a) the published-pointer
> `SceneEditController::AcquireSnapshot()` seam — the spike has only the on-demand
> `Scene::CreateSnapshot()`, no published snapshot a reader atomically refs; (b)
> the snapshot's intrinsic `DocumentId`/`Id()` (so "the snapshot's `Id().revision`"
> below is the target accessor, not current code); (c) a **render-faithful** view —
> the spike captures camera pose by value only (loses thin-lens/ortho params; ONB
> cameras need a factory rebuild — TRANSACTION_MODEL §3.7), so a render *off* a
> spike snapshot is not yet projection-faithful; and (d) any **restore/swap-back**
> path. Isolated rendering off a snapshot additionally needs that snapshot's TLAS +
> `LightSampler` to be valid for the view it renders — the spike captures neither
> (TRANSACTION_MODEL §3.7 item 2; the dominant cost when restore lands). Where this
> section says `AcquireSnapshot()` / `Id().revision`, read it as the production seam
> TRANSACTION_MODEL §3.5.4 will add; the *clone mechanism* underneath is real.

### 5.1 Why the live-film fast path is unsafe for thumbnails

`ResizeFilm` mutates the shared film with non-atomic stores and requires the
render thread stopped ([IScenePriv.h:219-244](../../src/Library/Interfaces/IScenePriv.h)).
A thumbnail or node preview is, by definition, something the user wants
*alongside* a live viewport — exactly when the render thread is **not** stopped.
Borrowing the live film would tear it, and submitting the preview's tiles into
the global pool while the viewport's tiles are also in it violates I1/I2. The
probe gets away with it only because of its unique pre-fan-out `call_once`
position (§3.4), which a thumbnail can never occupy.

### 5.2 Isolated job = held `SceneSnapshot` + private film (acquire → render → release)

The isolated job's scene view is **not designed here** — it is a held reference to
[TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5's ref-counted `SceneSnapshot`
(prototyped: a clone of the document's mutable wrapper state that addref-shares the
immutable leaves). This doc *consumes* that primitive; TRANSACTION_MODEL owns its
definition, publication, resource lifetime, and the production gaps (§3.7). An
isolated job carries:

- **A held `SceneSnapshot` ref.** In the *implemented* spike the snapshot is
  produced on demand by `Scene::CreateSnapshot()`
  ([`Scene.cpp:927`](../../src/Library/Scene.cpp)); in the *production* model it is
  acquired from the published pointer via the not-yet-built
  `SceneEditController::AcquireSnapshot()` seam
  ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5.4: "addref'd; caller
  `Release()`s … reflects the currently-published revision"). Either way the
  snapshot is a read-only capture of the document as of one revision: it clones the
  mutable wrapper (object transforms + camera pose) and addref-shares the immutable
  leaves (meshes, painters, radiance maps — TRANSACTION_MODEL §3.5). The isolated
  job **never** mutates the canonical scene and **never** reads the live graph; it
  reads only its held snapshot for its whole life. For a material/asset swatch it
  composes the snapshot's scene with a *substitute* material/HDRI applied to a
  sphere/preview rig, not the live objects. Any geometry realization the preview rig
  needs is single-threaded (honors `RenderParallelScope`, §1.1 #4) before the job's
  own fan-out. **Caveat (TRANSACTION_MODEL §3.7):** rendering off the snapshot
  requires a valid **TLAS** and **`LightSampler`** for the view — neither is
  captured by the spike — and a **render-faithful camera** (the spike's pose-by-value
  loses thin-lens/ortho params); both are production prerequisites before isolated
  *rendering* (as opposed to the proven *state capture*) is correct.
- **A private `IFilm`** sized to the thumbnail/preview target (e.g. 128² or
  256²), allocated per job, never the canonical one. (`IFilm` is the existing film
  abstraction `ResizeFilm`/`SetFilm` already operate on,
  [IScenePriv.h](../../src/Library/Interfaces/IScenePriv.h).)
- **A capturing `IRasterizerOutput` sink** that reads the finished pixels back
  into the caller's buffer and discards the film — the same pattern the probe
  uses for candidate renders ("a null FrameStore … land in the delegate's own
  internal image, read back via a capturing output, and never disturbs the
  canonical store",
  [AutoRasterizer.h:247-261](../../src/Library/Rendering/AutoRasterizer.h)).

#### 5.2.1 Job lifecycle — acquire at admission, render against it, release on completion

The held snapshot ref is what makes one process-wide pool safe (§5.6); its
lifecycle is explicit. The *ref-counting* underneath is real (the prototyped
`SceneSnapshot` is a `Reference` subclass that owns its clones and frees them on
last release — [`Scene.cpp:868`](../../src/Library/Scene.cpp)); the
`AcquireSnapshot()` / `snapshot->Id()` / `snapshot->Scene()` accessors named below
are the **production seam TRANSACTION_MODEL §3.5.4 still has to add** (the spike's
analogue is the on-demand `Scene::CreateSnapshot()` with the §3.5 read accessors,
and no intrinsic `DocumentId`). The lifecycle shape is what matters and it is
sound regardless of which acquire primitive backs step 1:

1. **Acquire at submission/admission.** The submitter (or the coordinator at
   `Submit`) calls `controller.AcquireSnapshot()` and stamps the job's
   `baseDocument = snapshot->Id()` (TRANSACTION_MODEL §3.5). The ref is held by the
   `RenderRequest`/job from this moment. Acquiring at submission (not at run)
   means the job pins a *consistent* revision even while it waits in the queue.
2. **Stale-drop at dequeue.** When the slot frees and `PickNext` selects the job,
   the coordinator applies the §4.3 stale-generation rule: if
   `job.baseDocument != controller.CurrentDocumentId()` (the live revision moved,
   or the UUID changed = reload), the job is **dropped at dequeue** — its snapshot
   ref is released, the observer gets `dropped_stale`, and it never runs. This is
   the coordinator's stale-generation rule applied *before* spending pool time on
   a job the world has outrun.
3. **Render against the held snapshot.** The surviving job renders into its
   private film, reading **only** `job.snapshot->Scene()` (TRANSACTION_MODEL §3.5)
   — never the live graph. Because the snapshot is immutable for its lifetime, a
   *concurrent* commit on the document (another client, the user) cannot tear the
   job's view; the job simply keeps reading its frozen revision (TRANSACTION_MODEL
   §3.5 "a snapshot held across K subsequent commits still reads its original
   revision's state").
4. **Release on completion (or completion-time stale-drop).** On finish, the
   coordinator applies the §4.3 completion checkpoint: deliver the captured pixels
   iff `job.baseDocument == controller.CurrentDocumentId()`; otherwise emit
   `dropped_stale`. Either way the job **releases its `SceneSnapshot` ref** — and
   its private film — exactly once, on the coordinator thread. When the last
   reader releases, TRANSACTION_MODEL §3.5 frees the snapshot (and any resources
   only it pinned).

So an isolated job is a *self-contained little render* of a *held, immutable
snapshot* into a *private* film, with **zero references to the canonical film or
canonical `FrameStore`** and **zero reads of the live graph**. Tearing is
impossible because the snapshot it reads is immutable and nothing else can see
its film.

### 5.3 It still goes through the coordinator (still one slot)

Isolated jobs are full-pool renders — a 256² thumbnail still calls
`ParallelFor(numWorkers, …)`. So they obey I1: the coordinator runs them in the
single slot, queued at priority 40, preempted by Interactive/Production. They do
**not** run "in the background alongside" a viewport render; they run when the
slot is free, when nothing higher wants it, **or when the anti-starvation credit
scheme makes one due** at an interactive pass boundary (§3.6) so they progress
even under a continuously-refining viewport. This is the unification of the
spec'd "queue" (thumbnails) and "budget" (node previews): both become *low-
priority isolated jobs in the one queue*, with dedup + debounce + a depth cap
(§3.3) providing the "budget" and §3.6 guaranteeing they are not starved.

### 5.4 "Tiny" is not "free"

A 128² thumbnail is fast but still saturates all cores for its (short) duration
via the global pool. Running several "in parallel" would re-create the exact
oversubscription the project rule forbids. Therefore even thumbnails serialize
through the slot. The throughput knob is **batching**: a library refresh submits
N thumbnail requests; the coordinator runs them back-to-back in priority order,
**interleaved with the viewport per the §3.6 credit cadence** — a higher-priority
Interactive/Production edit preempts at the next checkpoint, and a passed-over
thumbnail still comes due within `kStarveCredits` viewport passes (B1). Because a
tiny isolated job's burst is "run one whole job" (§3.6.2), each due thumbnail
*completes* in its turn rather than being perpetually nibbled. This gives
responsive typing *and* a steadily-filling thumbnail grid without ever
double-booking the pool — and the same cadence interleaves two documents' grids
fairly (B2).

### 5.5 When the live-film fast path IS acceptable (exactly the two documented single-actor cases)

The live-film `ResizeFilm` round-trip is acceptable in **exactly two** cases, and
no others — both are single-actor (the resizer *is* the only renderer) and
single-slot-exclusive, so neither needs an isolated snapshot:

1. **The auto-router probe** (§3.4) — runs pre-fan-out inside its owning render's
   `std::call_once`, single-threaded, strictly before that render's worker fan-out
   ([AutoRasterizer.cpp:486-540](../../src/Library/Rendering/AutoRasterizer.cpp));
   keep as-is.
2. **The interactive viewport's own scale-down** — `SceneEditController`'s
   preview-scale `ResizeFilm` pump
   ([SceneEditController.cpp:3754-3756](../../src/Library/SceneEditor/SceneEditController.cpp))
   is the *same* render resizing *its own* live film between its own passes,
   serialized by the render thread being the sole driver (the contract
   [IScenePriv.h:229-244](../../src/Library/Interfaces/IScenePriv.h) calls out by
   name). Under the coordinator this stays the Interactive job's internal
   behavior; the coordinator simply guarantees no other job is admitted while the
   Interactive job holds the slot and resizes.

In both, the resizer and the (only) renderer are the same logical actor holding
the slot, so there is no second reader to tear and no need for a published
snapshot. **Every other** render that wants a scene view — thumbnails, node
previews, RMSE, ad-hoc MCP previews, and any future consumer — is a *different*
actor from the live renderer and **must** use an isolated job against a held
`SceneSnapshot` (§5.2), never the live film. This is the line finding #3 draws:
the two single-actor live-film cases stay; everything else consumes
TRANSACTION_MODEL §3.5's snapshot.

### 5.6 Snapshot lifetime under a job — why one process-wide pool is safe

The held-snapshot ref is precisely the mechanism that lets the **one
process-wide worker pool** ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §0 #6)
serve isolated jobs from multiple documents safely. TRANSACTION_MODEL §3.5 owns
the lifetime guarantee; this doc relies on it.

> **What the spike already gives this.** The prototyped `SceneSnapshot` is a
> `Reference` subclass that **owns its cloned wrapper objects outright** and
> **addref-holds the immutable leaves** ([`Scene.cpp:862`,
> `:868`](../../src/Library/Scene.cpp)) — so a held snapshot keeps *its own copy*
> of the transform state alive (stronger than a pure-addref handle, which would
> alias the live, mutating objects). The lifetime shape below is therefore already
> backed by the spike's ownership model. **What is still production-target:** the
> snapshot being held by a *pooled job across subsequent commits* (the spike's
> test holds it on one thread and never publishes), and the "old document's
> resources survive a reload because the controller's swap doesn't free them"
> path — which needs the publication + document-swap layer (TRANSACTION_MODEL
> §3.5.4 / §9.3), not yet built.

- **A job pins its document's resources for its whole life.** While an isolated
  job holds its `SceneSnapshot` ref, the immutable resources that snapshot
  observed cannot be freed — even if, meanwhile, the user commits ten more
  transactions, reloads, or **closes the document** (TRANSACTION_MODEL §3.5
  lifetime; §9.3 "in-flight readers are unaffected by the swap … the old
  document's resources stay alive until those jobs finish, even though the
  controller is gone"). The pooled worker therefore never reads freed memory.
- **The pool does not need to know about documents.** It needs only that each
  task carries its own snapshot ref (TRANSACTION_MODEL §6.4: "the pool does not
  need to know about documents; it needs only that each task carries its own
  snapshot ref"). The coordinator enforces exactly that: every isolated
  `RenderRequest` carries its `snapshot` (§3.5), held across the job lifecycle
  (§5.2.1).
- **Close / reload interaction.** When a client unregisters (document close, or
  reload's document-swap, §2.1), the coordinator drops that client's *queued*
  jobs and `RequestCancel()`s its *active* job (§4.1). A cancelled active isolated
  job still finishes its current tile, then releases its snapshot ref on the
  coordinator thread — at which point, if it was the last reader, the old
  document's resources are freed. There is no window where a pooled worker
  touches a freed scene: the snapshot ref outlives the controller by construction.
- **Stale results after close.** A job whose document was closed/reloaded has a
  `baseDocument` whose UUID no longer matches any live controller; its result is
  dropped at the completion checkpoint (§4.3) — it is computed but never
  displayed, exactly as TRANSACTION_MODEL §9.3 / §6.4 prescribe.

This is the §0 #6 ↔ §3.5 contract from TRANSACTION_MODEL, restated from the
coordinator's side: *because* every job holds a snapshot ref for its lifetime,
one shared pool across all documents is safe; the coordinator's job is to acquire
that ref at admission and release it at completion (§5.2.1), nothing more.

---

## 6. Viewport suspend/resume around production & agent renders

Today production render hand-rolls the sequence: `Stop()` (request-cancel + join
the render thread), run `RasterizeScene` synchronously, `Start()` (respawn)
([SceneEditController.cpp:1646-1698, 570-592](../../src/Library/SceneEditor/SceneEditController.cpp)),
and the agent runtime is spec'd to "stop and restart the viewport" separately
([LLM_AGENT_RUNTIME.md](LLM_AGENT_RUNTIME.md)). Fold both into one coordinator
operation:

```
RenderCoordinator::RunHigherPriorityLive(job):           // Production or Agent, for client C
    lease = SuspendForEdit(job.client)  // cancel client C's Interactive pass, park the slot (§4.2)
    runToCompletion(job)                // full-res, all cores, on the slot
    // lease dtor → ResumeAfterEdit(): re-admit client C's Interactive at priority 100,
    //                                 buffer retained → no black flash
```

- **One implementation** for "stop the viewport, do a big render, bring the
  viewport back," whether triggered by the Render button, an MCP `render_*` tool,
  or an autonomous agent task. The agent no longer needs its own stop/restart
  code — it submits a `RenderClass::Agent` request and the coordinator does the
  rest (eliminating the divergent "agent stops/restarts viewport separately"
  path the review flagged).
- **Per-client suspend.** `SuspendForEdit(clientId)` parks **the one process-wide
  slot** but is scoped to *which document's* viewport it cancels and later
  resumes. A Production render for document A suspends A's viewport; document B's
  viewport, if it held the slot, simply keeps its place in the fair schedule
  (§3.6) — it is not silently stomped, and it resumes per the credit cadence once
  A's render completes. (With a single open document this is identical to today's
  behavior.)
- **Thread-origin agnostic.** MCP/agent requests can arrive on a non-UI thread;
  `Submit()` is thread-safe and the actual run happens on the coordinator's
  thread, so an agent render is admitted under the same lock and lease as a button
  render. (This is why the suspend/resume must live in shared C++, not in a
  platform bridge.)
- **Mutation cancel-and-park is the same primitive.** The controller's existing
  pre-mutation handshake (cancel + `mCV.wait(!mRendering)` at SetSelection /
  OnTimeScrub / CloneActiveCamera,
  [SceneEditController.cpp:1425-1428, 2110-2113, 3125-3128](../../src/Library/SceneEditor/SceneEditController.cpp))
  becomes `SuspendForEdit(clientId)` → mutate → lease drop. One RAII lease covers
  "edit the scene safely" and "run a higher-priority render safely" because both
  need the same thing: the slot parked and the scene quiescent. This is the seam
  TRANSACTION_MODEL §6.4 names ("a successful `Commit` sets `mEditPending` +
  `KickRender`, which the interactive coordinator consumes") — mutation publishes
  a new snapshot (TRANSACTION_MODEL §3.5) and kicks; the coordinator re-admits the
  viewport against the new revision.

---

## 7. Worked sequences

Each step below is the policy of §3.2 + §3.6 in action — the example *matches*
the rules, it does not assert behavior no rule produces (finding #2).

**Viewport drag (continuous), a thumbnail grid loads, then user clicks Render:**

1. Drag → Interactive job runs at ¼-res, self-cancels/refines per edit (priority
   100, owns the one slot, client = document A). Each completed pass returns
   control to the coordinator's `PickNext` checkpoint (§3.6.1 yield point #1).
2. Material library opens → 30 `Thumbnail` requests submitted (priority 40,
   isolated, each with a held `SceneSnapshot` and `baseDocument`, §5.2). They
   queue behind the Interactive job; each is stale-checked at dequeue (§4.3).
3. **User keeps dragging continuously** (the case v1 hand-waved): each edit
   supersedes the Interactive pass (latest-wins). At every pass boundary the
   coordinator runs `PickNext`; the queued thumbnails accrue a credit each time
   they are passed over for the priority-100 viewport (§3.6.2 aging). After
   `kStarveCredits` passes, the head thumbnail is **due** and `PickNext` selects
   it ahead of the viewport for **one burst** = one whole 128² thumbnail (fits the
   inter-pass gap, §3.6.2 / §5.4). The thumbnail completes, the viewport reclaims
   the slot on the next checkpoint with its buffer retained (no flash, §4.2). The
   grid thus fills at a steady cadence *even though the user never pauses* — each
   thumbnail starts within ≈ `kStarveCredits × 33 ms` (bound B1) and finishes one
   burst later. (If the user *does* pause, Interactive becomes non-runnable,
   §3.6.1 yield point #2, and the queue drains at full rate with no credits
   needed.)
4. User clicks **Render** → `Production` (200) submitted for document A → preempts
   Interactive (`SuspendForEdit(A)`, buffer retained) and, being non-preemptible,
   runs to completion ahead of the queued thumbnails (priority 200 > 40; the
   credit override only promotes a job *over higher priority within the starvation
   bound* — a single bounded Production render cannot itself starve them, §3.6.1).
   On completion the lease drops, Interactive resumes, and thumbnails continue
   draining on the same credit cadence.

**Two open documents, each with a live viewport (the multi-document case):**

1. Document A and document B each register as a client (§2.1) and submit an
   `Interactive` job (both priority 100). There is **one** slot and **one** pool
   (§2.1) — they cannot both render at once (I1).
2. `PickNext` alternates A and B at the **client-fair-order** cadence (§3.6.4):
   A's pass runs, returns at its checkpoint, B is now credit-due against A and
   runs its pass, then A again. Neither window freezes the other (bound B2 / I6).
3. A's user clicks **Render** → `Production` for A preempts **A's** viewport
   (`SuspendForEdit(A)`). There is one slot, and A's Production (200) outranks B's
   Interactive (100), so B's viewport waits too — but it is credit-protected (B
   accrues credits while passed over) and resumes the instant A's *bounded*
   Production completes (B1). B is *delayed*, never *starved* — and a single
   Production render, being finite, cannot delay B beyond its own runtime.
4. Closing document B mid-render → B `UnregisterClient`s; the coordinator drops
   B's queued jobs and cancels B's active job (§4.1); B's in-flight isolated jobs
   (if any) finish their tile and release their snapshot refs, freeing B's
   resources only then (§5.6). A is unaffected.

**Agent: "make 5 lighting variations and render thumbnails" (L3):**

1. Agent loop calls `SuspendForEdit(agentClientId)` (or mutates via
   `SceneEditController`, which itself suspends), applies variation 1, drops the
   lease. Each mutation publishes a new snapshot + bumps the revision
   (TRANSACTION_MODEL §3.5 / §4.2).
2. Agent submits a `RenderClass::Agent` full render *or* a batch of `Thumbnail`
   isolated renders (its choice). Each carries a held `SceneSnapshot` +
   `baseDocument` (§5.2); if the user edits the scene mid-task, the agent's stale
   in-flight previews are `dropped_stale` at the completion checkpoint (§4.3, the
   snapshot's `Id().revision` no longer matches the live revision) rather than
   shown.
3. Vision feedback reads the captured isolated buffers
   ([MCP_TOOL_SURFACE.md](MCP_TOOL_SURFACE.md) framebuffer resource); no canonical
   film was disturbed — each preview read its own held snapshot, never the live
   graph (§5.2).

**External MCP client requests a render while the viewport is live:**

- A `render_*` tool maps to `Production`/`Agent` (preempts that client's viewport)
  or, if the tool asked for a preview/snippet, to `IsolatedAdhoc` (queues, held
  snapshot). The MCP layer no longer needs a bespoke "reject if busy" rule — it
  submits and the coordinator's priority + anti-starvation policy decides. ("Reject
  if busy" survives only as an *optional* per-tool fast-fail for a *second*
  `Production` when the caller doesn't want to wait — surfaced via `Status()`.)

---

## 8. Non-goals

- **Co-scheduling two renders across split core sets.** The project rule is
  exclusivity (a render takes all cores). The coordinator serializes; it does not
  partition the pool. (If a future "render on E-cores while UI runs on P-cores"
  mode is ever wanted, it is a separate proposal and must not weaken I1.)
- **A second mutation path.** Mutation still goes through `SceneEditController`
  (roadmap #6). The coordinator gates *rendering*, and offers the
  `SuspendForEdit` lease so mutation and rendering interlock; it does not itself
  edit the scene.
- **Owning scene state / revisions / document identity / the snapshot primitive.**
  That is [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3. The coordinator
  *consumes* the `(UUID, revision)` `DocumentId` (admission tag + drop-stale, §4.3)
  and *holds* an immutable `SceneSnapshot` per isolated job (§5.2); it does not
  define, publish, or pin them. It never reads the live scene graph for an
  isolated job — only the held snapshot (§5.2).
- **Per-document coordinators / per-controller render threads.** There is exactly
  **one** `RenderCoordinator` for the process (§2.1); documents are *clients*, not
  owners. The old per-controller `mRenderThread`/`mRendering` model is retired
  (§2.2) precisely because the pool is one and the documents are many.
- **Concurrent multi-document rendering.** Two open documents do **not** render at
  once (I1); they share the one slot under the fair schedule (§3.6). Fairness is
  over *time*, not *cores* (I2).
- **Side-effect-free parse/validate.** That is
  [VALIDATION_ARCHITECTURE.md](VALIDATION_ARCHITECTURE.md). A `validate` tool does
  not render and does not touch the coordinator.
- **GPU/real-time scheduling.** RISE is a CPU path tracer; there is no DXR/30fps
  path to schedule ([CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md)).
- **Resumable production renders.** Only the Interactive job is suspend/resume;
  production-class jobs are run-to-completion or cancelled-and-dropped (§3.2).
- **MLT region render.** Region (`pRect`) is honored by the pixel integrators but
  ignored by MLT ([CURRENT_STATE_AUDIT.md §5](CURRENT_STATE_AUDIT.md)); the
  coordinator forwards `pRect` and inherits that limitation — a region request
  under MLT silently renders full-frame. Documented, not fixed here.

---

## 9. Acceptance criteria (GUI_ROADMAP §15)

- **Tests** —
  - *Single-slot invariant, process-wide (I1):* a unit test submits two
    `Production` jobs and asserts the second does not enter `ParallelFor` until the
    first completes (instrument via a test `IRenderObserver` + a counting fake
    pool). Guards "never two renders concurrently." **Repeat with the two jobs
    submitted by two *different* registered clients** (two `SceneEditController`s /
    documents) and assert the same single-slot serialization — the mechanical
    proof for finding #1 (the singleton, not a per-document brain, enforces I1).
  - *Multi-document fairness (B2 / I6, §3.6.4) — NEW, required:* register two
    clients A and B; submit a long-lived `Interactive` job from each; drive a tight
    loop of completed passes. Assert (a) **neither client is starved** — both A's
    and B's passes run within `kStarveCredits` of each other (instrument the
    `PickNext` selection sequence via the test observer), and (b) submitting a
    `Production` for A delays B's viewport but B resumes within one
    bounded-Production runtime, never `dropped_stale` for waiting. Also assert two
    clients each submitting a thumbnail grid **interleave** rather than one
    draining fully first.
  - *Starvation bound (B1, §3.6.3) — NEW, required:* with a fixed
    `kStarveCredits = K`, submit a continuously-re-submitting `Interactive` job and
    one `Thumbnail`; assert the thumbnail is passed over **at most K times** before
    it is selected for a burst, runs to completion, and reports `completed` — i.e.
    a bounded wait, never indefinite. Vary K and assert the observed pass-over
    count tracks K (proves the credit scheme is what bounds the wait, not luck).
  - *Preemption (§3.2):* submit Interactive, then Production; assert Interactive's
    `cancelled` flag is set, Production runs, Interactive resumes — and the
    Interactive framebuffer is byte-identical before/after suspend (retained
    buffer, no clear).
  - *Stale-revision drop (§4.3):* enqueue a Thumbnail with `baseDocument = (U, N)`
    (a held snapshot at revision N), commit a transaction so the live revision
    becomes `N+1`, assert the job is `dropped_stale` at dequeue and never rendered.
    Add the **reload variant**: after acquiring the job's snapshot, swap the
    document (fresh UUID `U'`); assert the job is `dropped_stale` on the UUID
    mismatch (consumes TRANSACTION_MODEL §3.4 identity, never a revision-only
    match).
  - *Isolation + snapshot lifetime (§5.2 / §5.6) — strengthened, run under
    ThreadSanitizer:* run an isolated thumbnail job concurrently with a live
    viewport in a stress loop while a writer commits transactions. Assert (a) the
    canonical film's `Generation()` advances only from the viewport job and the
    thumbnail's private film is never the canonical pointer (no tearing; TSan
    clean); (b) the thumbnail reads **only** its held `SceneSnapshot` — instrument
    that it never dereferences the live graph; (c) **close/reload the document
    while the isolated job is mid-render** and assert the job's snapshot ref keeps
    the document's resources alive until the job finishes and releases (instrument
    the resource refcount/dtor), proving the one-process-wide-pool safety of §5.6.
    (This pairs with TRANSACTION_MODEL §15's `SnapshotPublicationDataRaceTest`,
    which owns the publication-side TSan proof; this test owns the
    consumption-side, render-coordinator half.)
  - *Behavior-neutral refactor:* the existing `SceneEditControllerSaveTest` /
    interactive render tests pass unchanged after the render loop moves behind the
    coordinator (the controller's observable render behavior is preserved),
    **including with a single open document** (one client → the client-fairness
    term is a no-op, behavior identical to today).
  - *Correctness invariant (engine-touching):* a coordinator-driven production
    render of a canonical scene is **byte-identical** to today's direct
    `RasterizeScene` (the coordinator adds scheduling, not integrator changes —
    same discipline as `AutoRasterizer` "byte-identical integrators").
- **Platform parity** — Coordinator (singleton) + client registry + job model +
  isolation + anti-starvation are shared C++: **macOS, Windows, Android (Tier A
  render view) identical by construction.** Only the *trigger* (button/gesture/chat)
  and *display* of results are per-platform and already exist. Thumbnail/node-preview
  *UIs* land on desktop first; Android Tier A still gets correct arbitration for its
  viewport + production + agent renders (the chip set it already has). Node-graph
  previews are Android **Tier C**
  ([CROSS_PLATFORM_ARCHITECTURE.md](CROSS_PLATFORM_ARCHITECTURE.md) §10.4) — the
  job class exists but no mobile UI invokes it.
  > **Android note — the single-pool constraint matters *more* on mobile.** The
  > one-process-wide-pool / single-render-takes-all-cores rule
  > ([TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §0 #6, I1/I2) is most load-bearing
  > on phones and tablets: far fewer cores, a hard thermal ceiling, and an OS that
  > kills a backgrounded app that pegs the CPU. A second concurrent render (or an
  > ungated thumbnail batch) that the desktop merely *feels* would on Android thermal-
  > throttle, drain battery, or get the process reaped. Because arbitration is the
  > **one** shared coordinator with the anti-starvation policy (§3.6), Android gets
  > exactly this protection for free — no Android-specific scheduler. Android is
  > single-document today (one `SceneEditController` via JNI), so it registers one
  > client and the cross-document fairness term is a no-op; the value Android draws
  > now is the *single-slot + anti-starvation* guarantee (the viewport never starves
  > behind agent thumbnails, and the pool is never double-booked). The multi-document
  > machinery is dormant-correct on Android, ready if a future tablet layout opens
  > two documents.
- **Performance budget** — No production-render regression beyond the L8 ~0.4%
  bar: the coordinator's overhead on the production path is one lock acquire +
  one `PickNext`/credit check + the per-client suspend/resume of the viewport
  (which already happens via `Stop()`/`Start()`), i.e. effectively unchanged. The
  `PickNext` credit evaluation is O(queue depth) under a lock the coordinator
  already holds, run only at slot-free / pass-boundary checkpoints (not per tile),
  so it is off the hot path. Interactive latency target preserved (≤33 ms/pass at
  adaptive scale; the preview-scale pump is unmodified,
  [SceneEditController.h:868-876](../../src/Library/SceneEditor/SceneEditController.h));
  the anti-starvation burst is *one whole tiny job* per `kStarveCredits` passes,
  sized (§5.4) to fit the inter-pass gap so a due thumbnail never blows the pass
  budget. Thumbnail/preview jobs must never delay an Interactive pass by more than
  one such burst (and Interactive preempts at the next checkpoint regardless,
  cancellation granularity = tile boundary, §4.1).
- **Memory budget** — Per-isolated-job: one private `IFilm` at target px (128²/256²
  ≈ ≤1 MB fp + AOVs) + the held `SceneSnapshot`'s incremental cost — which is a
  **clone of the mutable wrapper state only** (per-object transform matrices + the
  transform-stack deque + the camera pose), addref-sharing the heavy immutable
  leaves (geometry/BVH/painters), **not** a deep scene copy. Owned/bounded by
  [TRANSACTION_MODEL.md](TRANSACTION_MODEL.md) §3.5/§3.6 (extra RSS ∝ per-object
  wrapper size × object count, not scene size); measured clone time ≈3.5–4 µs warm
  for 2 objects (~2–5 ms per 100 objects). Cap: **thumbnail queue depth ≤ 64 per client** with drop-oldest;
  **node-preview debounce ~80 ms**; a single shared scratch-film pool may be reused
  across sequential isolated jobs to bound peak RSS (only one isolated job runs at
  a time, §5.3, so one scratch film suffices even across documents — the one slot
  means one live isolated render process-wide). Note: a *queued* isolated job holds
  its snapshot ref while it waits (§5.2.1), so the queue-depth cap also bounds the
  number of concurrently-pinned snapshots; with drop-oldest + stale-drop (§4.3),
  stale snapshots are released promptly rather than accumulating.
- **Accessibility** — No new direct UI surface; the only user-visible additions
  are status text ("Rendering production…", queue depth, "Auto → VCM: <reason>")
  and a Cancel affordance, both of which must be keyboard-reachable and
  screen-reader-labeled, with no colour-only state (a spinner/percent, not just a
  hue). No numpad dependence.
- **Packaging** — None. Pure shared-library addition (`RenderCoordinator.{h,cpp}`
  + the isolated-job class). Must be registered in all **five** build projects per
  CLAUDE.md (Filelist, Android cmake, VS2022 `.vcxproj` + `.filters`, Xcode
  pbxproj) since new `.cpp`/`.h` land under `src/Library/`.
- **Migration** — No scene-format change. **No public-ABI break** if the
  coordinator is introduced behind a new `IRenderCoordinator` and
  `SceneEditController` is refactored internally (the controller's existing
  `RISE_API` surface is unchanged): the singleton is reached via
  `RenderCoordinator::Instance()`, and `RegisterClient`/`UnregisterClient` are
  called from the controller's ctor/dtor — both internal, no exported-signature
  change. The controller's `AcquireSnapshot()` / `CurrentDocumentId()` are the
  additive non-virtual methods TRANSACTION_MODEL §12 already specifies; this doc
  *calls* them, it does not add to the controller's ABI. If the controller's
  constructor were to take an `IRenderCoordinator*`, follow the
  [abi-preserving-api-evolution](../skills/) discipline (additive overload, not a
  signature change).
- **Rollback** — Feature-flagged: a `coordinator_enabled` build/runtime gate that,
  when off, leaves `SceneEditController` driving its own render thread exactly as
  today (the coordinator code is dormant). Default-on once the byte-identity +
  TSan + parity tests pass; default-off path must keep all existing tests green so
  a regression can be bisected by flipping one flag. No saved scene depends on the
  coordinator, so disabling it never breaks a `.RISEscene`. **Caveat: the "off"
  path is only safe single-document.** With the coordinator disabled, each
  controller drives its own render thread, which reintroduces the exact
  multi-document pool double-booking the singleton exists to prevent (finding #1)
  — and the off path also forgoes the isolated-job snapshot isolation, so it is an
  emergency single-document regression-isolation mode, not a shipping
  multi-document configuration. (This mirrors TRANSACTION_MODEL §15's rollback
  caveat that disabling `Commit` also disables snapshot publication.)

---

## 10. Open questions

- **Production-job queue policy when a second Production is submitted mid-render.**
  Default here is FIFO-queue (§3.2); the alternative is reject-newer (fast-fail)
  for the GUI button while keeping queue for MCP/agent. Leaning FIFO for the agent
  ("render these 5") and an *optional* per-tool reject for interactive buttons via
  `Status()`. Needs a UX call.
- **Should `RmseReference` be allowed to run while a viewport is idle but present,
  or always fully exclusive?** It is long and denoise-off; treating it as
  Production-class (fully preempts, no viewport during) is simplest and matches the
  variance-measurement discipline (no interference). Confirm.
- **Snapshot cost for large scenes** (Sponza-class, 155 mesh objects) —
  **MOSTLY RESOLVED by the prototyped TRANSACTION_MODEL §3.5 (measured, not
  estimated), with one open dependency.** The snapshot is a **clone of the mutable
  wrapper state** (object transforms + camera pose) that addref-shares the immutable
  leaves — **not** a deep scene copy. Measured **≈3.5–4 µs warm for 2 objects**,
  scaling **~2–5 ms per 100 objects** by the design analysis (so a Sponza-class
  snapshot is single-digit ms, not a stall). The old "thin handle / `O(handles)` /
  sub-µs" estimate is retired. **The remaining open cost** is the **restore/swap**
  path (not yet built, TRANSACTION_MODEL §3.7): swapping a snapshot back in as the
  live scene must rebuild the **TLAS** and the **`LightSampler`**, which is the
  dominant cost and is unmeasured. For *isolated reads* (thumbnails) there is no
  restore, so the clone cost above is the whole story; for any future *working-copy
  publish/restore* it is not. Coordinator consumes the snapshot (§5.2); the
  O(1)-per-read requirement this doc flagged is met for reads.
- **`kStarveCredits` / `kBurstQuantumMs` defaults** (§3.6) — the two anti-starvation
  tunables trade low-priority promptness against viewport smoothness. Defaults favor
  smoothness; the exact values want a UX pass on real thumbnail-grid + drag
  workloads (and a confirm that "one whole tiny job per burst" stays within the
  pass budget on the slowest target — Android especially). Needs measurement, not a
  design decision.
- **Per-class core caps as a future relaxation of I2** — explicitly out of scope
  now (Non-goals), but if "thumbnails on N-1 cores while the viewport keeps one"
  is ever wanted, it would require the project's exclusivity rule to be revisited
  first. Flagged, not designed.
