# Durable Document Snapshots in the Trajectory

> **Status:** SHIPPED 2026-08-13. Closes a product gap in the GUI agent trajectory
> (see [70-agent-eval-harness.md](70-agent-eval-harness.md) for the trajectory schema
> this extends): a scene built entirely inside a live RISE-GUI agent session — never
> Save As'd — was unrecoverable if the app quit or crashed. The trajectory recorded
> every tool call and its JSON-RPC result, but never the document TEXT itself.

## 1. The gap

The trajectory (`ChatTrajectory.h`/`.cpp`) is an append-only JSONL log of a chat
session: `session`, `user`, `llm`, `tool`, `history_edit`, `summary` records. A `tool`
record's `head_version_after` proves the document CHANGED on every mutating call, but
never carries the document's own bytes. Reconstructing a scene from the trajectory
alone meant replaying every `propose_patch`/`insert_chunk` call against an empty
document from session start — fragile, and impossible once a `.RISEscene` file was
never saved in the first place. A user who built a whole scene through the agent,
then quit the app without Save As, lost the work outright.

## 2. The fix: `document_snapshot` records

A new `run_type` (`ChatTrajectory.h`'s `TrajectoryDocumentSnapshotRecord`):

```
reason                  "head_bump" | "session_end"
head_version_uuid       uint64 (RISE::Cst::CstHeadVersion::uuid)
head_version_revision   uint64 (RISE::Cst::CstHeadVersion::revision)
document_text           the FULL serialized CST document (RISE::Cst::SerializeCst)
document_bytes          document_text.size(), for cheap scanning without a full parse
```

Both `uuid` and `revision` are stored — unlike `TrajectoryToolRecord::headVersionAfter`
(revision-only, an existing field left unchanged), a snapshot must correlate to a
SPECIFIC retained head: a scene reload mints a fresh `uuid`, so a bare revision number
is ambiguous across loads. `head_version_uuid`/`head_version_revision` are encoded as
JSON numbers via the same double-cast convention every other head-version field in this
file uses (`AgentRpc.cpp`'s `HeadVersionJson`) — a monotonic counter starting at 1 stays
well under 2^53, so the round-trip is lossless.

## 3. Write policy

`AgentChatLoop::SetDocumentSnapshotProvider` installs a
`std::function<bool(std::string& outText, uint64_t& outUuid, uint64_t& outRevision)>`
that returns false when there is no document to save. Plain `uint64_t` out-params, not
`RISE::Cst::CstHeadVersion`, so `AgentChatLoop.h` needs no `Cst.h` dependency. With no
provider set, every hook below is a no-op — headless/eval paths (the CLI agent, the eval
harness, every `AgentSession` unit test) are byte-identical to pre-feature behavior.

**Per-advance (`AddToolResult`).** After the existing `headVersionAfter` parse and
`EmitTool` call, if the tool result's reported `(uuid, revision)` differs from the
last snapshot WRITTEN this trace, the provider is called and a `"head_bump"` snapshot
is emitted — updating the last-written bookkeeping. The first advancing call of a
trace always snapshots (there is no prior write to compare against or decimate by).
A reported `uuid == 0` (the documented "no retained head" sentinel, `Cst.h`) skips
the hook entirely — `read_document` on a headless/no-CST session reports `{0, 0}`
and there is no document to save.

**Decimation past 1MB.** Once a WRITTEN snapshot's `document_bytes` reaches
`kDocumentSnapshotDecimationBytes` (1 MB), later advances are decimated: only every
`kDocumentSnapshotDecimationStride`-th (10th) advance since the last write actually
snapshots. A multi-megabyte scene under active editing should not pay a full
`SerializeCst` + JSONL-line write on every single tool call. The advance counter
resets only on a SUCCESSFUL write (`WriteDocumentSnapshotRecord`), so a provider
that answers "no document" at the stride boundary does not silently consume the
stride — the next advancing call retries immediately.

**Close (`CloseTrajectorySession`).** Before the terminal `summary` record, for ANY
close status — including `"app_quit"` — one final snapshot is attempted, UNLESS the
head has not moved since the last snapshot already written this trace (dedupe by
`(uuid, revision)`). The per-advance policy usually makes this redundant; the dedupe
just avoids writing an identical copy on every ordinary close. `Reset()`,
`SetProvider()`, and `FinishTrajectory()` all funnel through `CloseTrajectorySession`,
so this fires uniformly regardless of why the session ended.

The write-policy bookkeeping (last-snapshotted `(uuid, revision)`, `document_bytes`,
the advance counter) resets whenever the trace rolls to a new one and whenever the
trajectory sink itself is replaced or detached — a fresh trace has nothing to compare
against or decimate by.

## 4. Durability rationale

`MakeTrajectoryFileSink` (`ChatTrajectory.cpp`) flushes the file handle after EVERY
line, not just at session close. A `document_snapshot` line lands on disk the instant
`Emit` returns — a crash or force-quit a microsecond later loses nothing already
written. This is the same guarantee every other trajectory record already relies on;
`document_snapshot` inherits it for free by going through the same `Emit` path
(`ChatTrajectoryRecorder::EmitDocumentSnapshot`), including the unconditional
redaction pass (`RedactTrajectoryLine`).

## 5. Size math

Worst case: a 400-edit session on a scene whose serialized document is ~50 KB,
under the 1 MB decimation threshold. Every advancing edit snapshots: 400 ×
~50 KB ≈ 20 MB added to that session's trajectory file — comparable to or smaller
than the `llm` records' raw response bodies over the same session. Once a document
crosses 1 MB, decimation caps the added volume to roughly `(edits / 10) ×
document_bytes`, so a 5 MB scene under the same 400-edit session adds
~40 × 5 MB = 200 MB, not 2 GB. `PruneTrajectoryDir` (kept at ~50 newest files /
~200 MB total, called before each new session file is created) bounds the
directory regardless.

## 6. Recovery procedure

1. Find the newest `.jsonl` file under the trajectory directory (macOS:
   `~/Library/Application Support/RISE/trajectories/gui/`, or `$RISE_TRAJECTORY_DIR`
   if set) for the session in question.
2. Scan for `"run_type":"document_snapshot"` lines; take the LAST one in the file
   (newest `dotted_order`, hence most recent).
3. Extract `document_text` and write it verbatim to a `.RISEscene` file.
4. Load it normally (`RISE ASCII SCENE 7` header included, since `document_text` is
   `RISE::Cst::SerializeCst`'s output — the same bytes a real Save writes).

**Redaction caveat.** The trajectory's unconditional secret-redaction pass
(`RedactTrajectoryLine`) runs over every line, `document_snapshot` included. A scene
string that coincidentally matches a secret pattern (`sk-` + 8+ token characters, a
Bearer-token shape, etc.) is stored as `[REDACTED]`. This is a deliberate tradeoff —
the trajectory's secrets-never-leak guarantee (red-proven in `AgentTrajectoryTest`
E6) outranks byte-exactness. When recovering, grep the extracted text for
`[REDACTED]` and hand-repair the (rare) mangled identifier before loading.

No tooling currently automates this end to end; it is a manual `jq`/text-editor
recovery, same posture as reading any other trajectory line by hand today.

## 7. Explicit decision: NOT wire-level

Chunk texts are **not** added to tool-call JSON-RPC results. `read_document` and every
mutating verb's result already carry the information the MODEL needs (byte offsets,
`headVersion`, diagnostics) — appending the FULL document to every wire response would
bloat LLM context on every turn until elided, for a benefit (crash recovery) the model
itself has no use for. Snapshots capture the same underlying data OUT OF BAND, in the
trajectory only, at a cadence (per-advance, decimated, close-time) tuned for recovery
rather than every single RPC round-trip.

## 8. Threading

`AddToolResult` and `CloseTrajectorySession` both call the provider on the AGENT RPC
THREAD — the same thread every tool dispatch and read verb runs on, matching
`AgentSession::ReadDocumentSnapshot`'s documented discipline. The intended provider
implementation is a thin wrapper around that method (controller-mediated when a
controller is attached, so a close-time call CAN BLOCK while a render owns the scene —
accepted, not a bug: a close-time snapshot is worth a brief stall). `Reset()`/
`SetProvider()` can reach `CloseTrajectorySession` from the main thread while a render
holds the scene; the same blocking applies there.

## 9. Where it's wired

- Core: `ChatTrajectory.h`/`.cpp` (schema + `EmitDocumentSnapshot`),
  `AgentChatLoop.h`/`.cpp` (`SetDocumentSnapshotProvider`, `MaybeEmitHeadBumpSnapshot`,
  `WriteDocumentSnapshotRecord`, the `CloseTrajectorySession`/`AddToolResult` hooks).
- macOS: `RISEViewportBridge.h`/`.mm` (`agentReadDocumentSnapshotTextWithUuid:revision:`,
  `agentHeadVersionRevision`), `RISEAgentChatBridge.h`/`.mm`
  (`RISEAgentChatDocumentSnapshot`, `setDocumentSnapshotProvider:`), `ChatViewModel.swift`
  (installs the provider in `sceneOpened`; `appWillTerminate()` finishes the trajectory
  with status `app_quit`). The `NSApplication.willTerminateNotification` observer lives
  in `RenderViewModel` — it requests cancellation of any in-flight render FIRST, then
  calls `chat.appWillTerminate()`, mirroring Windows' `~MainWindow` ordering
  (`cancelAndJoinInFlightWork()` before `finishTrajectoryOnQuit()`) so the close-time
  blocking document read waits only for cooperative-cancel latency, not a full render.
- Windows/Qt: `ViewportBridge.h`/`.cpp` (`agentReadDocumentSnapshot`,
  `agentHeadVersionRevision`), `ChatPanel.h`/`.cpp` (installs the provider once in the
  constructor; `finishTrajectoryOnQuit()`), `MainWindow.cpp` (calls it in the destructor
  before `teardownViewport()`).

## 10. Test coverage

`tests/AgentTrajectoryTest.cpp` §E9: schema round-trip, per-advance policy (one
snapshot per advancing headVersion, none for a non-advancing one), decimation past 1MB
(only every 10th advance), close-time dedupe (both directions — skipped when the head
is unchanged, fires with the CURRENT head when it moved), and the no-provider no-op.
An informational (non-gating) timing print measures the emit path — provider call +
build + serialize + redact + sink write — on a ~29 KB synthetic document.
