//////////////////////////////////////////////////////////////////////
//
//  Cst.h - Concrete Syntax Tree kernel (agentic redesign).
//
//  The in-tree kernel for the document-canonical architecture
//  (docs/agentic-redesign): the scene document is a lossless, immutable CST;
//  text is its serialization and the rendered scene is a separate derivation.
//  Built across the transfer-gate items (docs/agentic-redesign/
//  IMPLEMENTATION_SLICES.md) -- the foundation validated by the four
//  `tests/Cst*SliceTest` prototypes, promoted into the real library and gated by
//  the render-equivalence harness (DumpJob(cstJob) == DumpJob(legacyJob)).
//
//  Scope landed so far:
//    * item 2 -- bytes <-> CST (lossless, multi-chunk, brace-nested) + derive
//      into a Job through the real apply layer.
//    * item 3 -- the top-level item list is a persistent balanced rope with
//      cached byte-width/newline aggregates (O(log N) COUNTED edit/diff).
//    * item 4 -- NodeId lineage + name-path / reverse / param identity.
//    * item 5 -- the derive binds through the LIVE chunk-parser descriptor
//      registry (CreateAllChunkParsers): EVERY registry chunk type is validated
//      via the same DispatchChunkParameters and applied via the same Finalize as
//      the legacy parser (with the same param-line whitespace normalisation), so
//      the two paths build an identical Job for the canonical scenes the CST is
//      fed. See DeriveToJob for the exact equivalence scope + the two-tier
//      (validation refuse-all / apply abort-on-first) failure boundary.
//
//  Derive domain: the CST is the v7 runtime format -- macro-free and
//  expression-free. v6 `$( )` / DEFINE / FOR (and `>` run/load/set/clearall
//  directives, and non-canonical whitespace/comment forms) are the one-shot
//  v6->v7 MIGRATOR's job (D8), never the CST runtime's; descriptor-driven
//  validation of params (rejecting unknown/ill-typed values, which the legacy
//  parser does) landed in item 5. So the equivalence gate is exact for the
//  canonical scenes the CST is fed -- see DeriveToJob for the precise scope.
//
//////////////////////////////////////////////////////////////////////

#ifndef CST_H
#define CST_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <cstdint>

namespace RISE
{
	class IJob;

	namespace Cst
	{
		struct ReferenceGraph;   // defined below; forward-declared for DeriveToJob's D35 out-param

		//! Green-node kinds. Leaves (Token / Trivia) carry exact bytes; internal
		//! nodes (Document / Chunk / Param) carry ordered children only.
		enum class NodeKind { Document, Chunk, Param, Token, Trivia };

		//! An immutable, structurally-shared green node. Width is RELATIVE
		//! (a leaf's byte count, an internal node's child sum) -- never a stored
		//! absolute offset; positions are computed on demand.
		struct Node
		{
			NodeKind                                    kind;
			std::string                                 text;   //!< leaves only (Token / Trivia)
			std::string                                 role;   //!< Chunk: keyword; Param: param name; Token: kw / lbrace / rbrace / pname / pvalue / tok
			std::vector< std::shared_ptr<const Node> >  kids;   //!< internal nodes only
		};
		typedef std::shared_ptr<const Node> NodeRef;

		//! A persistent, structurally-shared balanced sequence of the document's
		//! TOP-LEVEL items (chunks + inter-chunk trivia) -- the D16 rope. Each
		//! node caches its subtree's aggregates: element count, serialized byte
		//! width, and newline count. Those aggregates are what make locating an
		//! edit target by byte offset (or index) O(log N) -- the find is COUNTED,
		//! never an O(N) side scan (the slice-3 gap the item-2 review flagged).
		//! An edit path-copies only the root->leaf spine (O(log N) new nodes);
		//! everything else is shared by pointer.
		struct SeqNode
		{
			std::shared_ptr<const SeqNode> left, right;
			NodeRef item;          //!< the element (a Chunk or a trivia / stray leaf)
			int     count;         //!< subtree element count
			size_t  bytes;         //!< subtree serialized byte width
			int     newlines;      //!< subtree newline count
			size_t  itemBytes;     //!< THIS item's own byte width (cached once; the
			                       //!<   immutable item's stats never change, so a
			                       //!<   path-copy edit reuses them -> rebuild is O(log N)
			int     itemNewlines;  //!<   regardless of item size, not O(log N * item))
		};
		typedef std::shared_ptr<const SeqNode> SeqRef;

		//! Stable lineage identity (D26). 0 == none / invalid. Explicit 64-bit
		//! (NOT `long` -- that is 32-bit on Windows/LLP64, which would overflow the
		//! id counter and collapse the order-label scheme below).
		typedef std::int64_t NodeId;

		//! The SEPARATE persistent identity side-map (D23/D26) -- NOT a field on
		//! the green/seq node. A NodeId per top-level item, ordered IN LOCKSTEP
		//! with the item sequence (occurrence/position -> NodeId). So identity
		//! survives a value edit (replace keeps the id at that position) and an
		//! insert/erase index shift (the id moves WITH its item; position changes,
		//! identity does not). Persistent balanced (path-copy, O(log N)).
		struct IdNode
		{
			std::shared_ptr<const IdNode> left, right;
			NodeId id;
			std::int64_t label;   //!< order-maintenance key (ascending in document order); enables O(log N) NodeId -> position
			int          count;
		};
		typedef std::shared_ptr<const IdNode> IdSeqRef;

		//! Persistent name-path -> NodeId index (KEY-ordered, weight-balanced):
		//! counted O(log N) name resolution, maintained on parse/rename/insert/
		//! erase. Kernel keys on "keyword/name", e.g. "sphere_geometry/s".
		//! (Category name-paths like "geometry/s" -- one category maps to MANY
		//! keywords -- are DEFERRED: item 5 binds the descriptor registry for
		//! validation + derive, but reference resolution in the derive runs
		//! through the engine's named managers by name, so category addressing is
		//! a CST-navigation nicety, not load-bearing for items 5-8. Adding it is a
		//! second index keyed on the descriptor's ChunkCategory.)
		//!
		//! The value is a LIST of NodeIds (document/insertion order), not one id,
		//! so a name-path shared by several chunks (a duplicate-name scene -- valid
		//! to REPRESENT losslessly even though the derive layer rejects it) does not
		//! silently corrupt the index: erasing/renaming one occurrence removes only
		//! that occurrence's id and the survivors stay findable. DocFindByName
		//! REFUSES an ambiguous name (returns 0 + occurrence count if the list has
		//! != 1 entry); it resolves only the unique id of a well-formed scene.
		struct NameNode
		{
			std::shared_ptr<const NameNode> left, right;
			std::string         name;
			std::vector<NodeId> ids;   //!< all NodeIds with this name-path, in insertion order
			int                 count;
		};
		typedef std::shared_ptr<const NameNode> NameMapRef;

		//! Persistent REVERSE index NodeId -> current green node (KEY-ordered,
		//! weight-balanced): a durable agent/UI ref holds a NodeId and resolves it
		//! to the node it now labels in O(log N) (NOT an O(N) scan of the id
		//! side-map). Maintained on parse/replace/insert/erase/reparse.
		struct IdMapNode
		{
			std::shared_ptr<const IdMapNode> left, right;
			NodeId  key;
			NodeRef val;
			std::int64_t label;   //!< the top-level item's order-maintenance label (0 for a param id, which has no rope position)
			int          count;
		};
		typedef std::shared_ptr<const IdMapNode> IdMapRef;

		//! Persistent PARAM identity index (KEY-ordered, weight-balanced): a stable
		//! NodeId per parameter OCCURRENCE so widgets / EditIntents / diagnostics /
		//! ReferenceUses can bind to a parameter (D26/D36 -- identity is per-
		//! occurrence, not only per top-level chunk). Keyed by (owning chunk's
		//! NodeId, parameter role, OCCURRENCE INDEX among same-role siblings), so
		//! REPEATED same-role params (part / cp / value / time / shaderop) each get a
		//! distinct id; on edit they are matched by CONTENT (not occurrence index),
		//! so a value edit keeps the param's id and a sibling insert/remove does not
		//! shift ids onto unrelated values. The param's green node is repointed in
		//! `byId`. (Only value-ATOM sub-identity WITHIN a multi-atom value -- e.g. a
		//! component of `color 1 0 0` -- remains deferred, RepeatGroup-era.)
		struct ParamMapNode
		{
			std::shared_ptr<const ParamMapNode> left, right;
			std::string key;    //!< "<chunkId>\x1f<role>\x1f<occurrence-index>"
			NodeId      id;
			int         count;
		};
		typedef std::shared_ptr<const ParamMapNode> ParamMapRef;

		//! A parsed document: the green item sequence (item 3) + the identity
		//! side-map and name-path index (item 4), all persistent / structurally
		//! shared, plus a monotonic fresh-id source.
		struct Document
		{
			SeqRef      items;     //!< green top-level item sequence (item 3)
			IdSeqRef    idseq;     //!< position -> NodeId, lockstep with items (item 4)
			NameMapRef  byName;    //!< name-path -> NodeId (item 4)
			IdMapRef    byId;      //!< NodeId -> current green node, O(log N) reverse lookup (item 4)
			ParamMapRef paramIds;  //!< (chunkId, role, occurrence) -> param NodeId (item 4)
			NodeId      nextId = 1;
		};

		//! bytes -> CST. Lossless: every input byte lands in exactly one leaf,
		//! leaves in document order. Multi-chunk, brace-nested, bounds-safe.
		Document ParseToCst( const std::string& bytes );

		//! CST -> bytes. Byte-identical to the parsed input for an unedited tree
		//! (the INV-4 round-trip invariant).
		std::string SerializeCst( const Document& doc );

		//! Derive the document's chunks into pJob through the LIVE chunk-parser
		//! registry (item 5). FIRST resets the chunk parsers' cross-chunk parse
		//! state (ClearChunkParserState, as the legacy ParseAndLoadScene does at
		//! its start) so nothing leaks between successive derives -- the redesign
		//! runs this on every edit. Each chunk is then looked up by keyword in
		//! CreateAllChunkParsers(); each param line is normalised exactly as the
		//! legacy parser normalises it (whitespace runs collapsed to single
		//! spaces, like AsciiCommandParser::TokenizeString + rejoin), validated +
		//! bagged by the SAME DispatchChunkParameters the legacy parser runs, and
		//! applied via the SAME IAsciiChunkParser::Finalize. So ANY registry chunk
		//! type derives, and the CST path and the legacy path build an IDENTICAL
		//! Job for the CANONICAL scenes the CST is actually fed -- the v6->v7
		//! serializer's output:
		//!   * macro-free ($()/DEFINE/FOR),
		//!   * directive-free (no `>` run/load/set/clearall command lines),
		//!   * comments either `#` line-comments or MULTI-line `/* */` blocks
		//!     (`/*` and `*/` on separate lines), each on its own line -- NOT
		//!     mid-line after a value, and NOT a single-line `/* ... */`,
		//!   * single-space-separated values.
		//! These exclusions are all the v6->v7 MIGRATOR's domain (D8), not the CST
		//! runtime: a legacy scene that uses them is migrated to canonical form
		//! before it reaches the CST. On a NON-canonical legacy input the two paths
		//! may diverge, each by its own rules:
		//!   - the CST skips a `>` line as a stray (it does not run directives --
		//!     a `> run`-included reference then goes unresolved and is reported by
		//!     the apply-time boundary below);
		//!   - the CST strips a MID-LINE `#`/`/* */` comment as trivia, whereas the
		//!     legacy tokenizer keeps it as VALUE tokens (it never strips a comment
		//!     after the first token of a line). The legacy outcomes, all DIVERGING
		//!     from the CST's clean strip:
		//!       * `#`/`/* */` after a STRING-valued param -- legacy SILENTLY MIS-
		//!         CAPTURES (the comment bytes become part of the value: `name s # x`
		//!         binds the name `s # x`, not `s`; parse SUCCEEDS, value corrupted),
		//!         where the CST yields `s`;
		//!       * a trailing `#` on a NUMERIC value -- legacy TOLERATES it (the
		//!         numeric validator stops at the `#`: `radius 1 # cm` reads 1), and
		//!         here the CST AGREES (also reads 1), so this case alone does not
		//!         diverge the Job;
		//!       * `/* */` on a NUMERIC value -- legacy REJECTS it (the validator
		//!         sees a non-numeric token and fails the chunk), where the CST reads
		//!         the clean number;
		//!   - a SINGLE-LINE `/* ... */` block comment on its own line also
		//!     diverges: the CST strips it as one trivia run, but the legacy
		//!     top-level comment-block state machine only inspects each line's
		//!     FIRST token, so it enters comment mode on `/*` and never sees the
		//!     same-line `*/` -- swallowing the rest of the file (or, in a chunk
		//!     body, rejecting the chunk). Only a MULTI-line block (`*/` opening a
		//!     later line) round-trips through legacy.
		//! The serializer emits none of these, so they are out of the equivalence
		//! scope. Returns the number of chunks applied.
		//!
		//! TWO-TIER SAFE BOUNDARY:
		//!   * VALIDATION-time (PASS 1, what DispatchChunkParameters detects:
		//!     unknown chunk type / unknown parameter / value-less line / non-
		//!     finite-or-non-numeric numeric value) is REFUSE-ALL -- if any chunk
		//!     fails validation, NOTHING is applied.
		//!   * APPLY-time (PASS 2, a Finalize failure that only surfaces on apply,
		//!     e.g. an unresolved reference) matches the legacy parser's
		//!     ABORT-ON-FIRST-FAILURE: it stops at the first failing chunk, leaving
		//!     chunks BEFORE it applied (as legacy does) -- not silently swallowed,
		//!     not continued past. Full apply-atomicity (rollback) is later work.
		//! Both tiers report the failures in `diagnostics` (when non-null); neither
		//! silently half-derives. (Top-level non-chunk items -- the scene header
		//! strays / trivia -- are skipped; they carry no Job state.)
		//!
		//! D35 record-during-derive (slice 1, §8): when `outRecorded` is non-null, the derive
		//! RESETS it at entry (it reflects THIS derive only -- empty on a refused/failed derive --
		//! so a reused or caller-supplied graph is never mixed with stale state), then RECORDS the
		//! reference graph from the engine's ACTUAL resolution -- it brackets each chunk's Finalize
		//! and captures every entity the chunk PRODUCES (manager AddItem) and RESOLVES (manager
		//! GetItem), then writes `(producer -> consumer)` reverse-adjacency into
		//! `outRecorded->dependents`. This graph cannot drift from the engine (it IS the
		//! engine's production+resolution), unlike the static BuildReferenceGraph heuristic.
		//! It is CHUNK-level (the chokepoint records the resolved entity pointer, not the source
		//! param), so it serves CLOSURE; rename keeps the param-level static path. Opt-in: when
		//! `outRecorded` is null (the default / production path), no recording happens and the
		//! recording hooks stay disabled. Slice 1+2 RECORD + cross-check; closure CONSUMERS still
		//! read BuildReferenceGraph until the consumer-switch lands (gated on live consumers, §8).
		//! Covers `GenericManager`-backed entities AND participating media (slice 2 hooks the
		//! Job's separate `mediaMap` -- `Add*Medium` + `SetObjectInteriorMedium`/`SetGlobalMedium`
		//! -- since media bypass the GenericManager chokepoint).
		int DeriveToJob( const Document& doc, IJob& pJob, std::vector<std::string>* diagnostics = nullptr, ReferenceGraph* outRecorded = nullptr );

		//! Incrementally re-apply ONLY a closure (DocEditClosure) into an
		//! already-derived Job after an edit, instead of a full DeriveToJob: it recreates
		//! the closure's NON-OBJECT entities (drop via IJob's typed removal, then
		//! re-Finalize) but RE-POINTS the closure's standard_objects IN PLACE, so the work
		//! is O(closure . log N), far below the O(N . log N) of a full re-derive.  Pass the
		//! EDITED document + the closure (chunkIds) DocEditClosure returned on it.
		//! STABLE-OBJECT APPLY (slice 3 of 21-stable-apply-and-resolver.md): objects keep
		//! their ADDRESS (which the top-level BVH stores raw) and are re-pointed via
		//! AddObject/AddObjectMatrix's repoint path -- the object's own addref keeps a
		//! dropped entity alive until the re-point swaps it, so there is no UAF (P1.1).
		//! The invariant pass is CLOSURE-GATED: it invalidates the TLAS only when a
		//! re-pointed object's world bbox actually changed (a geometry-extent / transform
		//! edit), so a NON-SPATIAL edit (material/painter value) SKIPS the TLAS rebuild
		//! (P1.2 dissolved); it bumps the light-topology generation only when the emitter
		//! set changed (a re-pointed object's pre- OR post-edit material emits, or a Light
		//! chunk was recreated).
		//! Refuse-all + PER-PARSER reversibility: it validates the WHOLE closure before
		//! touching the Job -- every chunk must be named AND incrementally reversible.
		//! It REFUSES (-> 0, caller full-re-derives; D51 never a silent partial undo):
		//! non-single-manager categories (PAINTER: a scalar_painter sub-type lives in a
		//! separate manager RemovePainter does not clear; Camera/Function/...), COMPOSED
		//! materials (PBR creates helper painters -- IsMaterialComposed), translucent_-
		//! material (ambient thread-local parser state -- P1.3/P1.5), gltf_import (a bulk
		//! importer), an Object chunk that is neither standard_object nor csg_object (both re-point
		//! in place; other object-spawning chunks are unknown), a csg_object operand-REFERENCE change
		//! (obja/objb re-pointed to a DIFFERENT object -- re-binding would un-hide a possibly-shared
		//! dropped operand), any document with an animation/timeline,
		//! and any document with an override_object (its String target reference is
		//! untraceable -- review P1.3).  (An optional-slot removal -- a material/modifier/
		//! shader/radiance_map/interior_medium set to "none" -- is APPLIED by clearing it in
		//! place post-re-point; workstream #3.)
		//! ATOMIC (review #1, Part A): a WHOLE-PLAN PREFLIGHT (review P1.7) validates that
		//! every drop target EXISTS and every NAME reference RESOLVES slot-precisely (incl.
		//! radiance_map colour-only, and a numeric in an OBJECT reference slot is refused since
		//! interior_medium applies in a SEPARATE post-AddObject step -- review #1 2nd pass)
		//! before ANY mutation; AND any re-Finalize failure the preflight cannot foresee (a
		//! numeric in a pure-painter slot; a non-reference value DispatchChunkParameters
		//! accepted) is ROLLED BACK -- the non-object closure entities
		//! are captured (addref'd) before the drop and restored on failure, and a closure
		//! that places an object before a later entity is now APPLIED (the apply sorts entities-first,
		//! so every object is re-pointed only AFTER every entity) -- a failure can only occur
		//! before any object is touched.  On EITHER path the Job is left unmutated, so a
		//! return of 0 always means "nothing changed; fall back to a full derive."
		//! Returns the count applied (== chunkIds.size() on success).  Landed: the shared resolver (slice 1),
		//! atomic rename (slice 2), the stable-object re-point this function performs
		//! (slice 3), the cost re-measurement (slice 4), and the maintained-graph closure
		//! primitive -- DocEditClosure(id, graph), O(closure . log N) over a held graph
		//! (slice 5), which a CALLER uses to find this function's `chunkIds` cheaply.  Still
		//! deferred: routing the derive's OWN resolution through the recorded graph so the
		//! static graph and the apply resolution cannot drift even in principle (the
		//! remaining D35 step -- Part B of review #1/#3, the typed resolver).  Both slice-3
		//! follow-ups -- optional-slot removal AND CSG-operand re-point (op/slot/operand-INTERNAL edits;
		//! an operand-REFERENCE change falls back) -- LANDED in workstream #3.
		int DeriveToJobIncremental( const Document& doc, IJob& pJob, const std::vector<NodeId>& chunkIds, std::vector<std::string>* diagnostics = nullptr );

		//==============================================================
		// Item 3 -- persistent Document: lookups + edit where finding the edit
		// target is COUNTED in the cost (O(log N) via the cached aggregates, not
		// an O(N) scan). These are the load-bearing operations the item-2 review
		// required not be shortcut.
		//==============================================================

		//! Number of top-level items.
		int DocItemCount( const Document& doc );

		//! Total serialized byte width of the document (== SerializeCst(doc).size()),
		//! read from the root aggregate in O(1).
		size_t DocByteWidth( const Document& doc );

		//! Total newline count of the document (read from the root aggregate in
		//! O(1)); == the number of '\n' bytes in SerializeCst(doc).
		int DocNewlineCount( const Document& doc );

		//! Diagnostic for the cost gate: the running count of per-ITEM stat walks
		//! (each call to compute a fresh item's byte/newline stats). A correct
		//! path-copy edit walks exactly ONE item (the new/changed one) regardless
		//! of N; a hidden re-scan of the unchanged spine would bump this. Test-only
		//! instrumentation (single-threaded parse/edit context).
		unsigned long DebugItemStatWalks();

		//! Diagnostic for the reparse cost gate: the running count of old-item
		//! touches during DocReparse matching. The 4-pass hashed matcher touches
		//! each old item O(1) times, so this grows O(M+N) -- a regression to the old
		//! nested-loop matcher would make it O(M*N). Test-only instrumentation.
		unsigned long DebugReparseOldVisits();

		//! Diagnostic for the insert label-reflow cost gate: total order-labels
		//! rewritten by the WINDOWED reflow. A gap-exhausting insert rewrites
		//! O(window) labels; a regression to a global reflow would rewrite N each
		//! time. Test-only instrumentation.
		unsigned long DebugReflowLabelWrites();

		//! Diagnostic for the param-match cost gate: old-param touches in
		//! MatchParamSlots (hashed -> O(P) per chunk edit / reparse; a regression
		//! to the nested-loop matcher would make it O(P^2)). Test-only.
		unsigned long DebugParamMatchVisits();

		//! #4b cost gate: total ComputeChunkRefs evaluations.  A from-scratch BuildReferenceGraph or a
		//! MaintainedReferenceGraph REBUILD does N (one per chunk); an INCREMENTAL reference/cp edit does
		//! exactly 1 -- the committed proof the maintained edit is O(this chunk's refs . log N), not O(N).
		unsigned long DebugChunkRefsComputed();

		//! Locate the top-level item spanning byte `offset` (the byte->node map a
		//! UI/agent uses for "what's at this cursor position"). Returns the item's
		//! index (or -1 if out of range); `outItem`/`outStart` receive the item and
		//! its start offset, and `*visits` (if non-null) receives the number of
		//! sequence nodes descended -- O(log N), using the cached byte-width
		//! aggregates. THE find is counted here, not assumed.
		int DocItemAtByteOffset( const Document& doc, size_t offset,
		                         NodeRef* outItem, size_t* outStart, int* visits );

		//! Replace top-level item `index` with `newItem` (path-copy: O(log N) new
		//! sequence nodes, the rest shared by pointer; aggregates recomputed along
		//! the spine). `*visits` (if non-null) receives the rebuilt-node count.
		//! NON-NULL CONTRACT: a null `newItem` is refused -- the document is
		//! returned unchanged (visits 0). Out-of-range index is also a no-op.
		//! The chunk's NodeId persists; its params are re-matched by content (a
		//! unique-role value edit keeps the param's id; an ambiguous repeated-param
		//! value edit invalidates rather than position-remaps). `*invalidated` (if
		//! non-null) receives any param NodeIds dropped by the edit.
		Document DocReplaceItem( const Document& doc, int index, NodeRef newItem, int* visits = nullptr, std::vector<NodeId>* invalidated = nullptr );

		//! Insert `newItem` before `index` (clamped to [0,count]) / erase item
		//! `index`. The rope/idseq/byId splices are each O(log N) (path-copy +
		//! balance rotations; structural sharing of the untouched subtrees), per
		//! D16 -- NOT a flatten-and-rebuild. `*visits` (if non-null) receives the
		//! rope rebuilt-node count.
		//! COST CAVEAT (item-4 identity layer): assigning the new item's order-label
		//! is O(log N) when a label gap is available (the common case). A
		//! gap-EXHAUSTING insert triggers a WINDOWED reflow (ReflowWindow), which is
		//! tiny in the common/sparse case (measured window 2) but, being fixed-density
		//! rather than level-scaled, can reach Theta(N) under an ADVERSARIAL DENSE
		//! pattern (repeated inserts packing a prefix) -- so such an insert is
		//! Theta(N log N) WORST-CASE. The windowed reflow is therefore a common-case
		//! optimization over a global reflow, NOT an asymptotic one, and is the
		//! disclosed v1 fallback (D23 sanctions an O(N) v1 identity cost). Bender's
		//! level-scaled order-maintenance is the O(log N)-insert refinement, not yet
		//! landed. (The COUNTED lookups -- name / id->node / id->position -- ARE
		//! O(log N); only the insert-side label maintenance carries this caveat.)
		//! NON-NULL CONTRACT: a null insert is refused.
		//!
		//! ROUND-TRIP CONTRACT: the inserted item is spliced VERBATIM. The caller
		//! must supply any needed separation in the item's own bytes -- a Chunk is
		//! self-delimiting (braces), but a bare stray word inserted directly against
		//! another word would serialize to bytes that re-tokenize as ONE word. The
		//! higher edit/apply layer (a later item) enforces well-formed insertion;
		//! the kernel represents faithfully whatever it is given.
		Document DocInsertItem( const Document& doc, int index, NodeRef newItem, int* visits = nullptr );
		//! Erase item `index`. `*invalidated` (if non-null) receives the erased
		//! item's NodeId plus its param NodeIds (their durable bindings just died).
		Document DocEraseItem ( const Document& doc, int index, int* visits = nullptr, std::vector<NodeId>* invalidated = nullptr );

		//==============================================================
		// Item 4 -- NodeId identity + name-path addressing (the name-path half of
		// the counted lookup; the byte-offset half is item 3). Identity lives in a
		// SEPARATE persistent side-map, stable across edits (D26); name resolution
		// is O(log N) and COUNTED; reparse re-matches by content-key (full content,
		// then a unique keyword/name) and INVALIDATES genuinely-ambiguous rows
		// rather than position-remapping them (D9/D15: invalidate-don't-remap).
		//==============================================================

		//! NodeId of the top-level item at `index` (O(log N) via the id side-map;
		//! 0 if out of range). `*visits` (if non-null) receives the descent count.
		NodeId DocNodeIdAt( const Document& doc, int index, int* visits = nullptr );

		//! Resolve a name-path (e.g. "sphere_geometry/s") to its NodeId, REQUIRING a
		//! unique occurrence: returns the id iff exactly one chunk has that name-path,
		//! else 0. `*occurrences` (if non-null) receives the count (0 = absent,
		//! 1 = unique, >1 = AMBIGUOUS) so a caller distinguishes "no such name" from
		//! "ambiguous -- refuse" (a duplicate-name scene the derive layer rejects;
		//! addressing it would be history-dependent, so we refuse rather than guess).
		//! O(log N); `*visits` receives the BST descent count -- COUNTED, not O(N).
		NodeId DocFindByName( const Document& doc, const std::string& namePath, int* visits = nullptr, int* occurrences = nullptr );

		//! Resolve a durable NodeId to the green node it now labels (null if gone),
		//! in O(log N) via the persistent reverse index -- the counted "agent holds
		//! a NodeId, what is it now" path. `*visits` (if non-null) receives the BST
		//! descent count.
		NodeRef DocResolveNodeId( const Document& doc, NodeId id, int* visits = nullptr );

		//! The current top-level document INDEX of a NodeId (-1 if absent or not a
		//! top-level item), and `outItem` its node -- the edit-target position for a
		//! splice that applies an EditIntent by NodeId. O(log N) and COUNTED
		//! (`*visits` receives the descent count): an order-maintenance label per
		//! item lets id -> position be a label lookup (byId) + a rank query (idseq),
		//! both O(log N). So the end-to-end durable NodeId -> edit-target is O(log N)
		//! (this lookup) + DocReplaceItem/EraseItem (O(log N)), not an O(N) scan.
		int DocIndexOfNodeId( const Document& doc, NodeId id, NodeRef* outItem, int* visits = nullptr );

		//! Within-chunk descent (the "edit geometry/s.radius" path): resolve a byte
		//! offset to the innermost Param at that offset and its enclosing chunk.
		//! Returns the Param NodeRef (or null if the offset is not inside a chunk's
		//! Param); `outChunk` (if non-null) receives the enclosing chunk; `*visits`
		//! the rope-descent count (O(log N) to the chunk + O(params) within).
		//! `*outParamId` / `*outChunkId` (if non-null) receive the resolved param's
		//! and chunk's stable NodeIds (0 if none) -- the durable handle a widget /
		//! EditIntent / diagnostic binds to, resolvable later via DocResolveNodeId.
		NodeRef DocParamAtByteOffset( const Document& doc, size_t offset, NodeRef* outChunk, int* visits,
		                              NodeId* outParamId = nullptr, NodeId* outChunkId = nullptr );

		//! Resolve a parameter by its owning chunk's NodeId + role (+ occurrence
		//! index among same-role siblings, default 0) to the param's stable NodeId
		//! (0 if none) -- name-path-into-chunk addressing ("materials/gold.reflectance"):
		//! resolve the chunk by name, then its param by role. O(log N).
		NodeId DocParamId( const Document& doc, NodeId chunkId, const std::string& role, int occ = 0 );

		//! One traced reference edge (D14/D25, the §2.5 reference graph): a
		//! resolved reference FROM a referring param TO the chunk it names.
		//!   * sourceValueNodeId -- the NodeId of the referring param (e.g. an
		//!     object's `material` param). The design (D14) calls this the
		//!     reference's VALUE node; at item-4 identity granularity the finest
		//!     stable handle is the PARAM NodeId (value-ATOM sub-identity is the
		//!     deferred RepeatGroup-era refinement), and for a single-value
		//!     reference param the param IS the value holder, so it is exact.
		//!   * targetNodeId -- the NodeId of the referenced chunk (the chunk in the
		//!     param's reference category whose `name` matches the referring value).
		struct ReferenceUse { NodeId sourceValueNodeId; NodeId targetNodeId; };

		//! The traced reference graph for a document (slice 1 of
		//! docs/agentic-redesign/21-stable-apply-and-resolver.md): the resolved
		//! reference edges + a content STAMP. The stamp is a CONSERVATIVE fingerprint
		//! of the reference-relevant content (each chunk's keyword + name + every
		//! reference-param value) AND each chunk's NodeId. Its load-bearing guarantee is
		//! one-directional: every edit that COULD change the graph (a reference re-point, a
		//! rename of a referenced chunk, a chunk add/remove) changes the stamp -- so
		//! stamp-unchanged ⟹ graph-unchanged.  This is a one-shot CONSISTENCY check
		//! (compare two graphs' stamps in O(1)) -- NOT a cheap reuse oracle: obtaining a
		//! fresh stamp means running BuildReferenceGraph (O(N)), so a stamp-gated holder
		//! rebuilds every edit anyway.  For O(log N)-per-edit reuse, decide from the edit, not
		//! the stamp (see MaintainedReferenceGraph).  (P1.8: the prior TraceReferences was
		//! unstamped, so a stale graph could be silently trusted.)  The NodeId is
		//! folded because the graph's edges + dependents are NodeId-KEYED, so "graph
		//! unchanged" must include identity: erasing a chunk and reinserting a byte-
		//! IDENTICAL one (a NEW NodeId, same content) MUST move the stamp, else a reused
		//! graph would walk a dead NodeId (review P1.5). A value edit preserves NodeIds, so
		//! this stays stable across the non-reference edits the stamp must not move on.
		//! It is NOT a precise graph hash: it is STABLE across edits that cannot touch
		//! the graph (a comment, whitespace, a NON-reference value edit), but it may
		//! ALSO change on some graph-NEUTRAL edits (e.g. renaming a chunk nothing
		//! references re-mixes its name) -- a stamp change therefore means "re-derive
		//! the graph to be safe", which at worst costs a needless re-derive, never a
		//! missed staleness.
		struct ReferenceGraph {
			std::vector<ReferenceUse>             edges;        //!< resolved (sourceParam NodeId -> targetChunk NodeId) edges
			std::map<NodeId, std::set<NodeId> >    dependents;  //!< reverse adjacency: a referenced chunk -> the CHUNKS that reference it. A SET since #4b -- a referrer appears ONCE per target regardless of how many of its params reference it; closure-neutral (the reverse-BFS already dedups via its seen-set) and O(log N) referrer REMOVAL for MaintainedReferenceGraph's incremental update. Computed in the SAME single BuildReferenceGraph pass as `edges`, so a caller holding the graph gets DocEditClosure( changedChunkId, graph ) as a pure O(closure . log N) reverse-BFS (the BFS in isolation) instead of the O(N . log N) full re-trace. The END-TO-END reuse across edits is MaintainedReferenceGraph (it decides reuse from the EDIT in O(log N) -- a NodeId-index chunk lookup + a descriptor scan, NOT O(1) -- whereas a stamp-gated reuse cannot decide at all without an O(N) re-trace, since computing the new stamp is itself an O(N) BuildReferenceGraph).
			unsigned long long                    stamp = 0;    //!< conservative content fingerprint (see above)
		};

		//! Trace the document's reference graph (item 6). A reference resolves to the
		//! chunk of that name in the param's reference CATEGORY, in the descriptor-
		//! derived category namespace (Geometry -> geometry/, Material -> materials/,
		//! ... -- the SAME category-name keying the named managers use, §2.5). It
		//! traces every reference the DESCRIPTOR declares: a `ValueKind::Reference`
		//! param (whole value), OR a Reference element of a TUPLE param (the token at
		//! each `tupleKinds[k] == Reference` -- e.g. advanced_shader's
		//! `shaderop <ref> <min> <max> <op>`, voronoi's `gen <x> <y> <ref>`). So it
		//! AGREES with the engine's resolution for those declared references. Returns
		//! a `ReferenceUse` per resolved EXPLICIT reference (params actually present
		//! with a non-"none" value; descriptor defaults and the explicit-"none" idiom
		//! are not edges). An unresolvable reference is a DANGLING reference --
		//! reported in `diagnostics` (when non-null), never a silent edge. This is
		//! the graph D14 renames rewrite referrers from and D25 incremental
		//! re-derivation walks for the dependency closure.
		//!
		//! SCOPE: this is a DESCRIPTOR-BASED resolver (D14's "descriptor-provided
		//! reference resolver"), run as a separate pass over the CST, with these
		//! honest limits:
		//!   * DYNAMIC references whose target category is chosen at derive time by
		//!     another param (e.g. `timeline.element` keyed by `element_type`, D14)
		//!     are invisible to `referenceCategories` -- not traced here.
		//!   * A reference carried in a param the descriptor declares as neither
		//!     `Reference` NOR a tuple-Reference is a DESCRIPTOR-completeness gap
		//!     (the descriptor is the contract); fix it by declaring the token.
		//!   * The (category,name) namespace is slightly COARSER than the engine's
		//!     per-slot resolution: `scalar_painter` and colour painters share
		//!     ChunkCategory::Painter but live in SEPARATE managers, so a same-name
		//!     scalar+colour pair can mis-resolve (handled by a conservative same-name
		//!     ALIAS -> a superset closure).  (ior/film_ior previously ALSO over-declared
		//!     {Painter,Function}; workstream #2 dropped that phantom Function category --
		//!     they are now {Painter}, matching the engine's scalar-then-colour resolution.)
		//! The production primary path (D35) records `ReferenceUse` FROM the actual
		//! derivation resolver as it runs (no parallel pass -> no drift, dynamic refs
		//! captured); this descriptor-based pass is the transfer-gate demonstration
		//! of the graph + its uses (rename / closure / dangling), with that
		//! derive-time tracing deferred. O(N log N) (a chunk's NodeId is an O(log N)
		//! positional lookup per item; per chunk an O(params x descriptor-params)
		//! scan, both bounded per chunk).
		std::vector<ReferenceUse> TraceReferences( const Document& doc, std::vector<std::string>* diagnostics = nullptr );

		//! Build the document's reference graph (slice 1): the authoritative resolver
		//! TraceReferences/DocEditClosure/DocRename consume. Same edge resolution as
		//! TraceReferences (which is now a thin wrapper returning `.edges`), PLUS:
		//!   * RESOLVES OVER THE COMPLETE NAMESPACE -- the CST chunk definitions AND
		//!     the engine's runtime defaults (the `none` material/painter + the
		//!     `Default*` shader ops the Job pre-registers). So a reference to a
		//!     runtime default RESOLVES (it is not a false "unresolved"/dangling), and
		//!     produces NO edge (the default is not a CST chunk) -- closing the
		//!     coarser-namespace gap (P1.8). CstResolverTest asserts every listed
		//!     default is PRESENT in a freshly-derived Job, so a Job.cpp default
		//!     renamed/removed relative to this list fails the test (a Job-side
		//!     ADDITION, or a drop from the list alone, is not auto-caught -- a
		//!     reference to a then-missing default would instead surface as dangling).
		//!   * A NON-RESOLVING value is a DANGLING reference only if it is a NAME --
		//!     i.e. NOT entirely numeric tokens. A purely-numeric value is a LITERAL (a
		//!     scalar `0.5` or an inline `r g b`), not a dangling reference, so it is
		//!     neither diagnosed nor an edge (closing the P1.8 ref-or-literal gap --
		//!     PBR `roughness 0.5` is not a false dangling). A number in a PURE-
		//!     reference slot (e.g. `reflectance 0.5`) is a TYPE MISMATCH, which the
		//!     full DeriveToJob refuses at apply time; the static pass does not double-
		//!     report it. (This name-vs-number formulation replaced a fragile per-slot
		//!     ref-or-literal flag -- no allowlist to keep complete.)
		//!   * STAMPS the result (see ReferenceGraph) -- a one-shot O(1) CONSISTENCY compare
		//!     of two already-held stamps, NOT a cheap per-edit staleness oracle (obtaining
		//!     a fresh stamp is itself this O(N) BuildReferenceGraph; MaintainedReferenceGraph
		//!     does O(log N)-from-the-edit reuse instead).
		//! It does NOT yet capture DYNAMIC references created at derive time (timeline
		//! String elements, expr): those are the slice-5 derive-time-routing scope --
		//! callers that must be exact about dynamics (incremental apply, rename) refuse
		//! when the Job has any animation (see DeriveToJobIncremental). O(N log N) for typical
		//! documents; the conservative painter same-name ALIAS (PASS A) adds O(K^2) per name for
		//! K same-named MIXED-kind painters -- a degenerate case (real scenes have distinct names).
		ReferenceGraph BuildReferenceGraph( const Document& doc, std::vector<std::string>* diagnostics = nullptr );

		//! Reparse `newText` and carry NodeIds from `oldDoc` via FOUR hashed passes
		//! (D9/D15/D44: lineage survives rename + reparse on a BEST-EFFORT basis;
		//! genuine ambiguity is invalidated, never position-guessed):
		//!   1. FULL-content. A CHUNK group carries only when its multiset is
		//!      UNCHANGED (old count == new count) -- a count-changed group of
		//!      byte-identical chunks is ambiguous, so pairing them would swap ids;
		//!      it is deferred to invalidation. A TRIVIA/STRAY group carries
		//!      GREEDILY in doc order regardless of count (no id-swap hazard: a ref
		//!      can only rebind WITHIN a byte-identical group, so meaning is
		//!      preserved), so a pure append keeps every existing separator id
		//!      instead of spuriously invalidating it.
		//!   2. (keyword,name) key, unique 1<->1 among the remainder -- a NAMED
		//!      chunk's value edit keeps its id.
		//!   3. keyword, unique 1<->1 among the remainder -- a RENAME of a unique-of-
		//!      type chunk keeps its id (lineage survives rename, D9/D44).
		//! Everything still unmatched: a new item gets a FRESH id; an old id is
		//! INVALIDATED (a genuinely-ambiguous chunk group, or a real delete). Passes
		//! 2-3 consider chunks only; trivia/stray are matched in pass 1 ONLY --
		//! greedily within each byte-identical group, regardless of count -- and a
		//! surplus old trivia id with no new partner is invalidated. `*invalidated`
		//! (if non-null) receives the old NodeIds with no re-match. (Structured edits
		//! -- DocReplaceItem/Insert/EraseItem -- preserve identity EXACTLY; reparse is
		//! the lossy free-form-text path.)
		//!
		//! COST: the matching passes are O(M+N) hashed (DebugReparseOldVisits()
		//! counts old-item touches -- linear, the committed anti-quadratic gate), but
		//! the WHOLE call is O(M log M): it rebuilds the sorted name index and the id
		//! reverse index with M persistent-tree insertions (as ParseToCst itself
		//! does). Not O(M+N) -- a sorted/keyed index build carries the log factor.
		Document DocReparse( const Document& oldDoc, const std::string& newText, std::vector<NodeId>* invalidated = nullptr );

		//! STRUCTURED within-chunk value edit (item 7): set the (occ-th) param named
		//! `role` in the chunk identified by `chunkId` to `newValue` (re-tokenised
		//! into pvalue tokens), preserving the pname + its leading trivia and SHARING
		//! every other child by pointer. Applied as a `DocReplaceItem` at the chunk's
		//! position, so the chunk's NodeId is PRESERVED (lineage, D44); the unchanged
		//! params keep their identities, and the EDITED param keeps its id by the
		//! item-4 content match (a unique-role param, or a repeated param whose new
		//! value stays unique among its siblings -- an AMBIGUOUS repeated-param edit
		//! that makes two same-role params byte-identical INVALIDATES rather than
		//! position-remaps the id, per DocReplaceItem). Returns the new Document, or
		//! `doc` unchanged if the chunk or param is absent (no-op). This
		//! is the editor's core "edit geometry/s.radius" operation and the item-8
		//! non-spatial edit. O(log N) (find the chunk + path-copy the spine) + the
		//! chunk's own rebuild. (newValue is re-tokenised whole; rewriting a single
		//! ATOM within a multi-token value is the deferred value-atom refinement.)
		Document DocSetParamValue( const Document& doc, NodeId chunkId, const std::string& role, int occ, const std::string& newValue, int* visits = nullptr );

		//! RENAME a chunk (item 7, the D14 driver): set the chunk's `name` to
		//! `newName` AND rewrite every referrer's value to `newName`, found from the
		//! traced reference graph (TraceReferences -- NOT a re-resolution). The
		//! renamed chunk's NodeId is PRESERVED (lineage survives rename, D9/D44), so
		//! UI/agent bindings keyed on NodeId survive; only the name-path string
		//! changes, and for a non-colliding rename the references re-resolve to the
		//! SAME chunk via the new name.
		//!
		//! ATOMIC, rewrite-all-or-refuse (D14, slice 2). Every referrer is rewritten:
		//! a plain Reference param via its value, AND a TUPLE-reference referrer (the
		//! reference is one token of a multi-token value, e.g. advanced_shader.shaderop
		//! `<ref> <min> <max> <op>`) by substituting just that reference TOKEN, leaving
		//! the other tuple tokens intact -- no partial rename, no dangling referrer.
		//! REFUSED ATOMICALLY (applies NOTHING, returns `doc` unchanged + a diagnostic)
		//! when the rename would be unsafe: (a) `newName` already names another chunk
		//! of the same category (would make the name-path ambiguous + silently
		//! re-target referrers -- D14/§2.5); (b) `newName` is a reserved RUNTIME DEFAULT
		//! of the category (`none` / `Default*`, which the engine pre-registers and the
		//! manager would keep over the rename); (c) `newName` is empty; (d) the document
		//! has any animation/timeline (a `timeline` references its element + owning
		//! animation as ValueKind::String, invisible to the static graph -- a rename
		//! could leave it dangling; lifted in slice 5 when the resolver traces those).
		//! A no-op rename (`newName` == old) returns `doc` unchanged. The same-category
		//! collision check is by ChunkCategory, slightly COARSE (scalar vs colour
		//! painters share `Painter` but live in separate managers -- see
		//! TraceReferences), so a scalar<->colour same-name rename is conservatively
		//! over-refused (the SAFE direction; never under-refuses a real same-manager
		//! collision). Returns the new Document, or `doc` unchanged on any refusal /
		//! non-chunk target.
		Document DocRename( const Document& doc, NodeId chunkId, const std::string& newName, std::vector<std::string>* diagnostics = nullptr );

		//! The re-derive CLOSURE of editing `changedChunkId` (item 8, D25): the
		//! chunk itself + every chunk that TRANSITIVELY references it (its
		//! dependents), walked over the reference graph's reverse adjacency
		//! (ReferenceGraph::dependents -- which carries the plain/tuple edges PLUS the
		//! dimension-precise function, cp, and painter-alias dependents, a SUPERSET of
		//! TraceReferences's forward edges). This is the set DeriveToJobIncremental re-applies when the
		//! chunk changes; its SIZE scales with the DEPENDENTS, not with the document
		//! size. (Whether a re-derive touches the TLAS depends on the APPLY: the slice-3
		//! stable-object DeriveToJobIncremental re-points objects in place and skips the
		//! TLAS rebuild for a NON-SPATIAL edit, paying the engine's O(N log N) BVH rebuild
		//! only for a SPATIAL edit -- a geometry's SHAPE, an object's TRANSFORM, or an
		//! object's GEOMETRY reference.) Returns `changedChunkId` first.
		//! COST: the closure SIZE is O(closure), but COMPUTING it via THIS overload is
		//! O(N log N) -- it re-traces the whole reference graph (BuildReferenceGraph)
		//! each call. Use the graph overload below to amortise that.
		std::vector<NodeId> DocEditClosure( const Document& doc, NodeId changedChunkId );

		//! Closure over a PRE-BUILT reference graph (slice 5): a pure O(closure . log N)
		//! reverse-BFS over `graph.dependents`, with NO re-trace of the document.  This is
		//! the BFS IN ISOLATION -- it does NOT include building or validating the graph.
		//! To actually remove the closure-COMPUTE term end-to-end you must REUSE the graph
		//! across edits WITHOUT rebuilding it; a STAMP-gated holder cannot do that (computing
		//! the new stamp to compare is itself an O(N) BuildReferenceGraph -- you rebuilt to
		//! check).  `MaintainedReferenceGraph` (below) is the construct that achieves it: it
		//! decides reuse from the EDIT in O(log N) (a NodeId-index lookup; not O(1)).  The doc-only overload above is exactly this
		//! BFS preceded by a from-scratch BuildReferenceGraph.  (Does NOT take `doc`: the
		//! BFS needs only the graph's reverse adjacency.)
		std::vector<NodeId> DocEditClosure( NodeId changedChunkId, const ReferenceGraph& graph );

		//! A MAINTAINED reference graph (slice 5 / #4b): holds a Document + its ReferenceGraph and keeps
		//! them in sync INCREMENTALLY, so the END-TO-END per-edit cost -- not just the closure BFS in
		//! isolation -- is sub-O(N) for the common edits.  The edit CLASS is decided from the EDIT ITSELF
		//! in O(log N) (resolve the chunk via the NodeId index + scan its descriptor), NOT by recomputing
		//! the stamp (recomputing the stamp is itself an O(N) BuildReferenceGraph, so a stamp-validated
		//! reuse saves nothing end-to-end).  Three edit classes:
		//!   - a NON-reference value edit cannot change the graph (NodeId-keyed edges/dependents are
		//!     untouched -- a value edit preserves NodeIds) -> REUSED, O(log N), no rebuild;
		//!   - a REFERENCE / cp edit re-resolves only THIS chunk against the held namespace `m_defs`
		//!     (producers are unchanged by a value edit, so `m_defs` is reused): diff this chunk's
		//!     reverse-dependent targets + replace its commutative per-chunk stamp -> O(this chunk's refs . log N),
		//!     NO O(N) re-trace (#4b).  The flat `edges` view is lazily reflattened on Graph();
		//!   - a NAME edit changes the namespace (re-resolves every edge TO this chunk), and a reference
		//!     edit on an ALIAS-involved painter could touch a dependents entry shared with the painter
		//!     same-name alias (indistinguishable in the merged set) -> REBUILD, O(N) (rare; conservative).
		//! (Structural edits -- insert/erase/reparse -- are not handled here; a holder rebuilds on those.)
		//! CstEditCostTest / CstMaintainedGraphTest exercise + measure this end-to-end.
		class MaintainedReferenceGraph
		{
		public:
			explicit MaintainedReferenceGraph( const Document& doc );
			const Document&       Doc() const   { return m_doc; }
			//! The held graph.  Lazily reflattens the `edges` view if an incremental edit dirtied it
			//! (dependents + stamp are always current; only the flat edges view is deferred).
			//! The reflatten is O(N) (one item pass) but fires only on the FIRST Graph() after an
			//! incremental edit; EditClosure (dependents) never triggers it, so per-edit cost stays
			//! sub-O(N) UNLESS a caller polls Graph().edges every edit.
			const ReferenceGraph& Graph() const;
			void SetParamValue( NodeId chunkId, const std::string& paramRole, int occurrence, const std::string& value );
			//! The edit closure over the maintained graph -- O(closure . log N), no re-trace.  Reads the
			//! (always-current) dependents, so it does NOT trigger the lazy edges reflatten.
			std::vector<NodeId> EditClosure( NodeId changedChunkId ) const { return DocEditClosure( changedChunkId, m_graph ); }
			//! Whether the most recent SetParamValue REBUILT the graph (O(N)) vs reused/updated it
			//! incrementally (measurement/diagnostics; CstMaintainedGraphTest asserts on it).
			bool LastEditRebuilt() const { return m_lastRebuilt; }
		private:
			void RebuildAll();   // full O(N) build of m_defs + the per-chunk caches + m_graph (ctor + name/alias edits)
			Document               m_doc;
			mutable ReferenceGraph m_graph;          // mutable: Graph() lazily reflattens m_graph.edges (a derived cache)
			bool                   m_lastRebuilt;
			mutable bool           m_edgesDirty;     // m_graph.edges stale after an incremental edit -> reflatten on Graph()
			// The #4b incremental caches add O(total edges + N) memory (m_chunkEdges duplicates the flat
			// m_graph.edges) -- the price of O(this chunk's refs . log N) reference edits vs an O(N) rebuild.
			std::map<std::pair<int,std::string>, NodeId> m_defs;        // the (category,name) namespace -- REUSED across reference edits
			std::map<NodeId, std::vector<ReferenceUse> > m_chunkEdges;   // per-chunk edges (source of truth; flat m_graph.edges is derived)
			std::map<NodeId, unsigned long long>         m_chunkCs;      // per-chunk commutative stamp -- stamp += new-old on an edit
			std::set<NodeId>                             m_aliasInvolved; // painters with a same-name cross-kind alias twin (conservative rebuild)
		};
	}
}

#endif
