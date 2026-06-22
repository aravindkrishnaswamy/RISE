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
//  expression-free. v6 `$( )` / DEFINE / FOR are the one-shot v6->v7 MIGRATOR's
//  job (D8), never the CST runtime's; descriptor-driven validation of params
//  (rejecting unknown/ill-typed values, which the legacy parser does) is item 5.
//  So the equivalence gate is exact for macro-free, descriptor-valid scenes.
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
		//! registry (item 5). Each chunk is looked up by keyword in
		//! CreateAllChunkParsers(); each param line is normalised exactly as the
		//! legacy parser normalises it (whitespace runs collapsed to single
		//! spaces, like AsciiCommandParser::TokenizeString + rejoin), validated +
		//! bagged by the SAME DispatchChunkParameters the legacy parser runs, and
		//! applied via the SAME IAsciiChunkParser::Finalize. So ANY registry chunk
		//! type derives, and the CST path and the legacy path build an IDENTICAL
		//! Job for the scenes the CST is actually fed -- the v6->v7 serializer's
		//! canonical output: macro-free (D8: $()/DEFINE/FOR are the migrator's
		//! domain, not the CST runtime) with comments on their own lines (a
		//! mid-line `#`/`/* */` after a value is CST-stripped trivia but a legacy
		//! token, so it is excluded by construction, as the legacy parser also
		//! rejects it for numeric params). Returns the number of chunks applied.
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
	}
}

#endif
