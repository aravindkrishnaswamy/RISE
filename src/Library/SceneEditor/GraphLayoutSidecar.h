//////////////////////////////////////////////////////////////////////
//
//  GraphLayoutSidecar.h - Layout-position persistence for the Painter/
//    Material node graph (doc-88 Phase 3 S13,
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 1.3 + sect. 6 S13 entry).
//
//    Format: `<scenePath>.risegraph.json`, a versioned JSON document,
//    keyed by NODE NAME (see GraphLayout.h's own "NAME-KEYED, NOT
//    HANDLE-KEYED" note for why -- GraphNodeHandle is per-publish-
//    generation and cannot survive a save/reload the way a sidecar
//    entry must):
//
//        { "version": 1,
//          "nodes": { "<name>": { "x": 120.0, "y": 40.0 }, ... } }
//
//    Any OTHER top-level or per-node key is tolerated and ignored on
//    read (forward-compat with a future frames/comments field per
//    NODE_GRAPH_CANVAS.md sect. 1.3's "positions/frames/comments keyed
//    by node name" framing -- only positions are implemented in S13).
//
//    WHY A SIDECAR, NOT A SCENE-FILE CHUNK: design doc sect. 1.3 option
//    B (a `layout{}` pseudo-chunk or comment convention in the
//    `.RISEscene` text itself) was rejected -- it would either become a
//    real chunk kind (the exact "no new node/graph chunk type" non-goal
//    NODE_GRAPH_CANVAS.md sect. 5 states) or a bespoke comment-parsing
//    convention bolted onto the CST's trivia-preserving serializer,
//    polluting every agent's read of the scene text with UI-only bytes.
//    A sidecar is CST-grammar-invisible by construction: the JSON file
//    never enters `Cst::ParseToCst`/`SerializeCst` at all.
//
//    JSON CODEC REUSE: this module uses `RISE::Agent::JsonValue` /
//    `JsonParse` / `JsonSerialize` (src/Library/Agent/Json.h) rather
//    than hand-rolling a second parser. That type lives in the `Agent`
//    module by ORIGIN (it was built for the JSON-RPC transport), but it
//    is a general-purpose, dependency-free RFC-8259 codec with no
//    Agent-specific state or transport coupling in its public surface
//    (see Json.h's own header: "a message codec... sized to the
//    request/response shapes", but the codec ITSELF -- JsonValue/
//    JsonParse/JsonSerialize -- has no RPC-shaped API, just object/
//    array/scalar construction). It is ALREADY reused outside the
//    Agent module today: tests/SourceHygieneTest.cpp (a repo-wide
//    hygiene scanner with no agentic-surface relationship at all)
//    includes it directly. That precedent is what makes reuse here a
//    non-issue rather than a layering violation -- both are one static
//    library (`build/make/rise/Filelist` compiles every SRCLIB* group
//    into the SAME `librise.a`), so there is no build-graph reason two
//    Library subdirectories cannot share a leaf, dependency-free codec.
//    Duplicating a SECOND hand-rolled JSON reader/writer for this one
//    format would be the actual layering smell -- two implementations
//    of RFC-8259 free to drift, the exact risk ReferenceGraph.h's own
//    header warns against for a different pair of consumers.
//
//    ATOMIC WRITE: follows the SAME tmp+rename shape as (does not call
//    -- see the .cpp for why) `SaveEngine.cpp`'s own `AtomicWrite`
//    helper: write to a process-unique `.tmp.<pid>.<counter>` sibling,
//    flush, rename over the target (atomic on both POSIX
//    same-filesystem rename and Windows `MoveFileEx`/`ReplaceFile`
//    semantics). `AtomicWrite` itself is `SaveEngine.cpp`-local
//    (anonymous namespace, not exported via `SaveEngine.h`) --
//    exporting it was judged out of scope for this slice (it would
//    widen SaveEngine's public surface for a single new caller outside
//    its own module), so this file carries its own copy of the tmp+
//    rename pattern rather than either reaching into another
//    translation unit's internal helper or inventing a materially
//    different write strategy. The parity is NOT exact, though, and
//    deliberately so: this sidecar takes two durability relaxations
//    SaveEngine.cpp's `AtomicWrite` does not (see the .cpp's
//    `AtomicWriteSidecar` for where each one lives) -- (1) the tmp-file
//    fsync's return value is ignored (SaveEngine treats fsync failure
//    as fatal to the save), and (2) there is no post-rename directory
//    fsync at all (SaveEngine does one, best-effort). Both are
//    appropriate for THIS file's contents but would not be appropriate
//    for a scene save: a lost or delayed sidecar write in a crash
//    window degrades to "this node re-lays-out on next open" -- the
//    exact "layout is disposable, semantics are not" framing this
//    header opens with -- whereas a lost scene save is data loss.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_GRAPHLAYOUTSIDECAR_
#define RISE_GRAPHLAYOUTSIDECAR_

#include "GraphLayout.h"

#include <set>
#include <string>

namespace RISE
{
	//! Pure/static except for the two filesystem-touching entry points
	//! (ReadSidecar/WriteSidecar); every other method is a function of its
	//! explicit arguments only, directly unit-testable in a temp directory
	//! (no controller, no Job, no live document).
	class GraphLayoutSidecar
	{
	public:
		//! `<scenePath>.risegraph.json` -- the sidecar path for a given
		//! scene file path. Returns "" when `scenePath` is empty (an
		//! unsaved, in-memory-only scene has nowhere for a sidecar to
		//! live; see WriteSidecar's own "never writes when unsaved" note).
		static std::string SidecarPathForScene( const std::string& scenePath );

		//! Parse `text` (the sidecar file's own bytes) into a Positions map.
		//! Returns true on a structurally sound document (top-level object,
		//! `nodes` present and itself an object -- an ABSENT `nodes` key on
		//! an otherwise-valid `{"version":1}` document is NOT malformed,
		//! just empty: a sidecar written before any node had a saved
		//! position). Returns false -- `out` left empty -- when `text`
		//! fails to parse as JSON at all, or parses but is not a top-level
		//! object; the caller (ReadSidecar) is the one that logs the
		//! single warning this case gets, so a direct unit test of THIS
		//! function can assert "empty, no log side effect" cleanly.
		//!
		//! Per-node coordinate sanitization (design brief: "non-finite/
		//! absurd coordinates sanitized (clamp, log)"): a node entry whose
		//! `x`/`y` is non-finite (NaN/Inf -- JSON itself cannot encode
		//! either per RFC 8259, so this only fires against a hand-edited
		//! or corrupted file where the JSON codec's own number parse
		//! produced one some other way) is replaced with 0.0 (a NaN/Inf
		//! has no sign to clamp toward); a FINITE value whose magnitude
		//! exceeds `kAbsurdCoordinateBound` is clamped to
		//! +/-kAbsurdCoordinateBound (sign-preserving). A `x`/`y` that is
		//! PRESENT but not itself a JSON number (e.g. `"x":"garbage"`) is
		//! treated the same way -- replaced with 0.0 and counted -- rather
		//! than silently taking `JsonValue::asNumber`'s default, which
		//! would make a type error indistinguishable from a legitimate
		//! 0.0. Every one of these cases is sanitized, never dropped -- a
		//! sanitized node still gets a position -- and increments
		//! `*outSanitizedCount` (non-null caller opts in to the count;
		//! ReadSidecar logs it when nonzero). A member present in the
		//! JSON but not itself an object, or missing BOTH `x` and `y`, is
		//! skipped for that one entry (not a whole-file failure) -- an
		//! unknown/malformed single node does not cost every other node
		//! its saved position.
		static bool ParsePositions( const std::string& text, GraphLayout::Positions& out,
		                            unsigned int* outSanitizedCount = nullptr );

		//! The inverse of ParsePositions: `{"version":1,"nodes":{...}}`,
		//! compact (no whitespace -- matches JsonSerialize's own contract),
		//! keys in `positions`' own std::map iteration order (lexical by
		//! name) -- so two calls with equal content produce byte-identical
		//! text, which is what lets WriteSidecar's "only when changed"
		//! rule do a plain string compare against the file's current bytes.
		static std::string SerializePositions( const GraphLayout::Positions& positions );

		//! Read the sidecar for `scenePath`. Three outcomes, matching the
		//! design brief exactly:
		//!   - file absent: returns an EMPTY Positions map, no log (this is
		//!     the ordinary "brand-new scene, full auto-layout" case, not a
		//!     failure).
		//!   - file present but malformed (ParsePositions returns false):
		//!     returns an EMPTY Positions map, ONE eLog_Warning line, never
		//!     fatal -- a caller (the canvas bridge) proceeds exactly as if
		//!     no sidecar existed.
		//!   - file present and well-formed: returns the parsed positions;
		//!     if any entry needed coordinate sanitization, ONE
		//!     eLog_Warning line reports the count (still not fatal, and
		//!     still returns the -- now-clamped -- positions, not empty).
		static GraphLayout::Positions ReadSidecar( const std::string& scenePath );

		//! Write the sidecar for `scenePath`, after two transformations of
		//! `positions` the design brief specifies:
		//!   1. ORPHAN-DROP: an entry whose name is not in `liveNodeNames`
		//!      (the CURRENT graph's own node names -- pass
		//!      `PainterMaterialGraph::nodes[i].name` converted the usual
		//!      way) is silently pruned before serializing. Never an
		//!      error, never logged -- a deleted node's old position is
		//!      disposable by design (NODE_GRAPH_CANVAS.md sect. 1.3:
		//!      "layout is disposable, semantics are not").
		//!   2. UNSAVED-SCENE REFUSAL: `scenePath` empty means an
		//!      in-memory-only scene with no sidecar path to write to at
		//!      all (SidecarPathForScene already encodes this -- this
		//!      function just refuses up front rather than attempting a
		//!      write to an empty path). Returns true (not an error: there
		//!      is genuinely nothing to do), no file touched.
		//! Then: if the resulting (pruned) serialized text is BYTE-IDENTICAL
		//! to the file's current on-disk content (or the file does not yet
		//! exist and the pruned map is EMPTY -- writing an empty sidecar
		//! for a graph with no saved positions at all would just create
		//! useless churn), the write is skipped entirely -- "only when
		//! positions actually changed" (design brief). Otherwise writes
		//! atomically: temp file in the same directory, then rename over
		//! the target; the temp file is removed on any failure path (never
		//! left behind) and does not exist under the final path once this
		//! function returns true.
		//! Returns true on success (including a same-content no-op skip or
		//! an unsaved-scene no-op); false with `outError` set on an actual
		//! I/O failure (temp-file open/write, or rename).
		static bool WriteSidecar( const std::string& scenePath, const GraphLayout::Positions& positions,
		                          const std::set<std::string>& liveNodeNames, std::string& outError );

		//! THE SIDECAR-SIDE RENAME PRIMITIVE (design brief: "expose a
		//! MigrateName(old,new) hook the future rename flow can call").
		//! Pure, in-memory, no filesystem I/O -- moves `positions[oldName]`
		//! to `positions[newName]` (erasing the old key) and returns true,
		//! or returns false and leaves `positions` UNCHANGED when either
		//! `oldName` has no entry (nothing to migrate) or `newName` ALREADY
		//! has one (refuses to silently clobber a distinct node's saved
		//! position that happens to already occupy the target name -- an
		//! edge case `Cst::DocRename` itself mostly forecloses for the
		//! CHUNK namespace, since it refuses a same-category name
		//! collision, but the SIDECAR's own map can still hold a stale
		//! entry under `newName` left over from an unrelated, since-
		//! deleted node, which orphan-pruning on the NEXT save would have
		//! cleaned up anyway -- refusing here is the conservative choice
		//! over silently overwriting whichever one loses the race).
		//!
		//! CALLER CONTRACT (this slice provides the primitive; wiring it
		//! into the actual rename flow is later-slice work, per the design
		//! brief): call this ONCE, immediately after a successful
		//! `Cst::DocRename( doc, chunkId, newName )` returns a document
		//! where the rename actually took effect, passing the CHUNK's name
		//! immediately BEFORE that call as `oldName` and `newName` as
		//! the same string passed to `DocRename`. A caller that skips this
		//! call does not corrupt anything -- the renamed node simply reads
		//! as "unpositioned" (auto-laid-out) the next time the sidecar is
		//! read, and the stale `oldName` entry is silently orphan-dropped
		//! on the next `WriteSidecar` call, per that function's own
		//! pruning rule. Losing a manual layout position on an
		//! unmigrated rename is a degraded-but-safe outcome, never a
		//! crash and never a wrong position assigned to the wrong node.
		static bool MigrateName( GraphLayout::Positions& positions,
		                         const std::string& oldName, const std::string& newName );

		//! Coordinate magnitude bound: a saved position beyond this in
		//! either axis is treated as "absurd" and clamped (see
		//! ParsePositions). Generous relative to any layout this codebase
		//! actually produces (LayoutGraph's own defaults are two orders of
		//! magnitude smaller per node-slot) -- this exists to catch a
		//! corrupted/hand-edited file (a stray extra zero, a unit mixup),
		//! not to constrain legitimate manual dragging.
		static constexpr double kAbsurdCoordinateBound = 1.0e7;
	};
}

#endif
