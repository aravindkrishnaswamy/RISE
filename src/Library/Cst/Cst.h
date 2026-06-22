//////////////////////////////////////////////////////////////////////
//
//  Cst.h - Concrete Syntax Tree kernel (agentic redesign).
//
//  The in-tree kernel for the document-canonical architecture
//  (docs/agentic-redesign): the scene document is a lossless, immutable CST;
//  text is its serialization and the rendered scene is a separate derivation.
//  This is transfer-gate item 2 (docs/agentic-redesign/IMPLEMENTATION_SLICES.md)
//  -- the foundation validated by the four `tests/Cst*SliceTest` prototypes,
//  now promoted into the real library and gated by the render-equivalence
//  harness (DumpJob(cstJob) == DumpJob(legacyJob)).
//
//  Item-2 scope: bytes <-> CST (lossless, multi-chunk, brace-nested) +
//  derive `sphere_geometry` into a Job through the real apply layer. This kernel
//  is the SEAM that subsequent transfer-gate items extend; it does NOT itself
//  have them: the child container is a std::vector, so the persistent rope and
//  its byte-width/newline aggregates (the O(log N) edit/diff) ARE the item-3
//  work; live descriptor binding is item 5; the traced derivation graph and
//  edit/identity are later. (No O(log N) and no NodeId are claimed here.)
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

		//! Stable lineage identity (D26). 0 == none / invalid.
		typedef long NodeId;

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
			int    count;
		};
		typedef std::shared_ptr<const IdNode> IdSeqRef;

		//! Persistent name-path -> NodeId index (KEY-ordered, weight-balanced):
		//! counted O(log N) name resolution, maintained on parse/rename/insert/
		//! erase. (Kernel keys on "keyword/name", e.g. "sphere_geometry/s";
		//! category name-paths like "geometry/s" are an item-5 descriptor concern.)
		//!
		//! The value is a LIST of NodeIds (document/insertion order), not one id,
		//! so a name-path shared by several chunks (a duplicate-name scene -- valid
		//! to REPRESENT losslessly even though the derive layer rejects it) does not
		//! silently corrupt the index: erasing/renaming one occurrence removes only
		//! that occurrence's id and the survivors stay findable. DocFindByName
		//! returns the FIRST occurrence (the unique id in a well-formed scene).
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
			int     count;
		};
		typedef std::shared_ptr<const IdMapNode> IdMapRef;

		//! Persistent PARAM identity index (KEY-ordered, weight-balanced): a stable
		//! NodeId per parameter OCCURRENCE so widgets / EditIntents / diagnostics /
		//! ReferenceUses can bind to a parameter (D26/D36 -- identity is per-
		//! occurrence, not only per top-level chunk). Keyed by (owning chunk's
		//! NodeId, parameter role), so a value edit -- which keeps the chunk's id and
		//! the parameter's role -- keeps the parameter's id; the param's green node
		//! is repointed in `byId`. (Repeated same-role params are RepeatGroup-era,
		//! out of this gate; value-ATOM sub-identity within a multi-atom value is the
		//! same later work.)
		struct ParamMapNode
		{
			std::shared_ptr<const ParamMapNode> left, right;
			std::string key;    //!< "<chunkId>\x1f<role>"
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
			ParamMapRef paramIds;  //!< (chunkId, param role) -> param NodeId (item 4)
			NodeId      nextId = 1;
		};

		//! bytes -> CST. Lossless: every input byte lands in exactly one leaf,
		//! leaves in document order. Multi-chunk, brace-nested, bounds-safe.
		Document ParseToCst( const std::string& bytes );

		//! CST -> bytes. Byte-identical to the parsed input for an unedited tree
		//! (the INV-4 round-trip invariant).
		std::string SerializeCst( const Document& doc );

		//! Derive the document's `sphere_geometry` chunks into pJob through the
		//! real apply layer (IJob::AddSphereGeometry). Returns the number of
		//! geometries applied.
		//!
		//! SAFE BOUNDARY (item-2 review): every sphere_geometry chunk is VALIDATED
		//! first -- unknown parameter, value-less parameter line, non-finite /
		//! non-numeric radius. If ANY chunk fails, NOTHING is applied (refuse-all)
		//! and the failures are reported in `diagnostics` (when non-null). A
		//! malformed scene is refused, never silently half-derived (no atof()->0
		//! sphere, no ignored unknown parameter). Item-5 generalizes this to the
		//! descriptor registry; here the validation is sphere-specific. Item-2
		//! scope is sphere_geometry only; other chunk types are subsequent items.
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
		Document DocReplaceItem( const Document& doc, int index, NodeRef newItem, int* visits = nullptr );

		//! Insert `newItem` before `index` (clamped to [0,count]) / erase item
		//! `index`. O(log N) on a persistent weight-balanced tree (path-copy +
		//! balance rotations; structural sharing of the untouched subtrees), per
		//! D16's O(log N) insert/remove requirement -- NOT a flatten-and-rebuild.
		//! `*visits` (if non-null) receives the rebuilt-node count (O(log N)).
		//! NON-NULL CONTRACT: a null insert `newItem` is refused (doc unchanged).
		Document DocInsertItem( const Document& doc, int index, NodeRef newItem, int* visits = nullptr );
		Document DocEraseItem ( const Document& doc, int index, int* visits = nullptr );

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

		//! The current top-level document INDEX of a NodeId (-1 if absent), for the
		//! rope splice that applies an edit. This is the one O(N) step (a linear scan
		//! of the id side-map): per D23 it is the disclosed v1 fallback for the
		//! position lookup; the O(log N) reverse RESOLUTION is DocResolveNodeId, and
		//! the O(log N) cursor-edit path is the byte-offset map (item 3). An order-
		//! maintenance index would make splice-by-id O(log N) too (documented
		//! refinement, not in this slice).
		int DocIndexOfNodeId( const Document& doc, NodeId id, NodeRef* outItem );

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

		//! Resolve a parameter by its owning chunk's NodeId + role to the param's
		//! stable NodeId (0 if none) -- name-path-into-chunk addressing
		//! ("materials/gold.reflectance"): resolve the chunk by name, then its param
		//! by role. O(log N).
		NodeId DocParamId( const Document& doc, NodeId chunkId, const std::string& role );

		//! Reparse `newText` and carry NodeIds from `oldDoc` via FOUR hashed passes
		//! (D9/D15/D44: lineage survives rename + reparse on a BEST-EFFORT basis;
		//! genuine ambiguity is invalidated, never position-guessed):
		//!   1. FULL-content, but only for a content group whose multiset is
		//!      UNCHANGED (old count == new count) -- carries unchanged items across
		//!      any REORDER, incl. byte-identical duplicates and trivia. A group
		//!      whose count changed (a PARTIAL edit of duplicates) is NOT consumed
		//!      here -- greedily pairing indistinguishable rows would swap their ids.
		//!   2. (keyword,name) key, unique 1<->1 among the remainder -- a NAMED
		//!      chunk's value edit keeps its id.
		//!   3. keyword, unique 1<->1 among the remainder -- a RENAME of a unique-of-
		//!      type chunk keeps its id (lineage survives rename, D9/D44).
		//! Everything still unmatched: a new item gets a FRESH id; an old id is
		//! INVALIDATED (a genuinely-ambiguous group, or a real delete). Passes 2-3
		//! consider chunks only; trivia/stray carry by content (pass 1) or are
		//! reborn. `*invalidated` (if non-null) receives the old NodeIds with no
		//! re-match. (Structured edits -- DocReplaceItem/Insert/EraseItem -- preserve
		//! identity EXACTLY; reparse is the lossy free-form-text path.)
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
