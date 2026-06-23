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
#include <memory>
#include <cstdint>

namespace RISE
{
	class IJob;

	namespace Cst
	{
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
		int DeriveToJob( const Document& doc, IJob& pJob, std::vector<std::string>* diagnostics = nullptr );

		//! Incrementally re-apply ONLY a closure (DocEditClosure) into an
		//! already-derived Job after an edit, instead of a full DeriveToJob: it
		//! drops each closure chunk via IJob's typed removal then re-Finalizes it,
		//! so the work is O(closure), NOT the O(N) of a full re-derive. Pass the
		//! EDITED document + the closure (chunkIds) DocEditClosure returned on it.
		//! Same refuse-all contract as DeriveToJob: it validates the WHOLE closure
		//! -- every chunk must be named AND of a category whose typed removal is a
		//! COMPLETE undo of its Finalize (Material / Geometry / Object / Light /
		//! Modifier, all verified single-manager) -- before touching the Job; on ANY
		//! validation failure nothing is dropped or applied and it returns 0, so the
		//! caller falls back to a full DeriveToJob. PAINTER closures are refused on
		//! purpose: a painter Finalize dual-registers in the painter AND function-2D
		//! managers but RemovePainter clears only the painter manager, so an
		//! incremental drop+re-add would leave a stale func2d entry (D51: never a
		//! silent corruption) -- editing a painter VALUE thus falls back to a full
		//! re-derive. Returns the count applied (== chunkIds.size() on success).
		//! This is the incremental-apply primitive behind the redesign's
		//! edit->preview path; node-granular memoization, multi-manager rollback
		//! (so painters/cameras can re-derive incrementally too), and full
		//! post-Finalize rollback are deferred (Facet-2), as DeriveToJob defers them.
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
		//!     scalar+colour pair can mis-resolve; and a param whose
		//!     `referenceCategories` over-declares (e.g. `ior` lists {Painter,
		//!     Function} but the engine resolves it only against scalar painters)
		//!     can yield a spurious edge if a name exists only in the wrong category.
		//! The production primary path (D35) records `ReferenceUse` FROM the actual
		//! derivation resolver as it runs (no parallel pass -> no drift, dynamic refs
		//! captured); this descriptor-based pass is the transfer-gate demonstration
		//! of the graph + its uses (rename / closure / dangling), with that
		//! derive-time tracing deferred. O(N log N) (a chunk's NodeId is an O(log N)
		//! positional lookup per item; per chunk an O(params x descriptor-params)
		//! scan, both bounded per chunk).
		std::vector<ReferenceUse> TraceReferences( const Document& doc, std::vector<std::string>* diagnostics = nullptr );

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
		//! COLLISION is REFUSED ATOMICALLY: if `newName` already names another chunk
		//! of the same category, the rename applies NOTHING and reports it in
		//! `diagnostics` -- renaming into an existing name would make the name-path
		//! ambiguous and silently re-target the referrers to the other chunk, which
		//! D14/§2.5 forbid ("an unresolvable referrer is flagged, never silently
		//! renamed"). The check is by ChunkCategory, which is slightly COARSE
		//! (scalar vs colour painters share `Painter` but live in separate managers
		//! -- see TraceReferences), so a scalar<->colour same-name rename is
		//! conservatively over-refused; over-refusal is the SAFE direction (refuse a
		//! legal rename rather than risk a silent mis-target), and it never
		//! under-refuses a real same-manager collision. Referrers carried in a TUPLE param (the reference is one token
		//! of a multi-token value, e.g. advanced_shader.shaderop) are NOT rewritten
		//! -- that needs value-atom granularity (deferred, item-4 scope) -- and are
		//! reported in `diagnostics`. Returns the new Document (or `doc` unchanged on
		//! a refused collision / non-chunk target).
		Document DocRename( const Document& doc, NodeId chunkId, const std::string& newName, std::vector<std::string>* diagnostics = nullptr );

		//! The re-derive CLOSURE of editing `changedChunkId` (item 8, D25): the
		//! chunk itself + every chunk that TRANSITIVELY references it (its
		//! dependents), walked over the traced reference graph (TraceReferences
		//! reverse edges). This is the set DeriveToJobIncremental re-applies when the
		//! chunk changes; its SIZE scales with the DEPENDENTS,
		//! not with the document size -- the cost model's O(closure), the gate's
		//! non-spatial-edit claim. (A non-spatial edit -- a material/painter value
		//! -- changes no object's world bounding box, so it leaves the top-level
		//! acceleration structure / TLAS clean; a SPATIAL edit -- a geometry's
		//! SHAPE, an object's TRANSFORM, or an object's GEOMETRY reference -- changes
		//! a world bbox, dirtying it and adding the engine's
		//! O(N log N) BVH rebuild as a SEPARATE cost.) Returns `changedChunkId`
		//! first.
		//! COST: the closure SIZE is O(closure), but COMPUTING it here is O(N log N)
		//! -- it re-traces the whole reference graph (TraceReferences) + rebuilds the
		//! param->chunk map (DocParamId per param) each call; a production system
		//! maintaining the graph incrementally would find the closure in O(closure).
		//! (CstEditCostTest wall-clocks this term.)
		std::vector<NodeId> DocEditClosure( const Document& doc, NodeId changedChunkId );
	}
}

#endif
