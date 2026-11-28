//////////////////////////////////////////////////////////////////////
//
//  Cst.h - Concrete Syntax Tree kernel (document-canonical v7 format).
//
//  Lossless, immutable CST (docs/agentic-redesign/50-agentic-surface.md,
//  docs/agentic-redesign/21-stable-apply-and-resolver.md):
//    * Persistent balanced top-level rope with cached byte/newline
//      aggregates for O(log N) counted lookup and path-copy edits.
//    * Separate persistent NodeId lineage side-map, name-path index,
//      reverse NodeId -> Node map, and per-occurrence param identity.
//    * Descriptor-registry-driven derivation (CreateAllChunkParsers ->
//      DispatchChunkParameters -> Finalize). Macro-free and directive-
//      free runtime domain (v6 constructs belong to the migrator).
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
		struct ReferenceGraph;

		//! Green-node kinds. Leaves (Token/Trivia) carry exact bytes; internal nodes carry ordered children only.
		enum class NodeKind { Document, Chunk, Param, Token, Trivia };

		//! Immutable, structurally-shared green node. Width is relative (never absolute offset).
		struct Node
		{
			NodeKind                                    kind;
			std::string                                 text;   //!< leaves only (Token / Trivia)
			std::string                                 role;   //!< Chunk: keyword; Param: param name; Token: kw / lbrace / rbrace / pname / pvalue / tok
			std::vector< std::shared_ptr<const Node> >  kids;   //!< internal nodes only
		};
		typedef std::shared_ptr<const Node> NodeRef;

		//! Persistent balanced rope of top-level items (chunks + inter-chunk trivia).
		//! Caches subtree count/bytes/newlines for O(log N) counted offset/index lookup and path-copy edits.
		struct SeqNode
		{
			std::shared_ptr<const SeqNode> left, right;
			NodeRef item;          //!< Chunk or trivia/stray leaf
			int     count;         //!< subtree element count
			size_t  bytes;         //!< subtree serialized byte width
			int     newlines;      //!< subtree newline count
			size_t  itemBytes;     //!< cached byte width of `item` (O(log N) rebuilds independent of item size)
			int     itemNewlines;  //!< cached newline count of `item`
		};
		typedef std::shared_ptr<const SeqNode> SeqRef;

		//! Stable 64-bit lineage identity (0 = invalid). Explicit int64_t for Windows/LLP64 safety.
		typedef std::int64_t NodeId;

		//! Separate persistent identity side-map in lockstep with SeqNode (survives value edits and index shifts).
		struct IdNode
		{
			std::shared_ptr<const IdNode> left, right;
			NodeId id;
			std::int64_t label;   //!< ascending order-maintenance key for O(log N) NodeId -> position
			int          count;
		};
		typedef std::shared_ptr<const IdNode> IdSeqRef;

		//! Persistent "keyword/name" -> NodeId list index (O(log N)). Multi-id list preserves
		//! duplicate-name scenes losslessly; DocFindByName refuses ambiguous (>1) entries.
		struct NameNode
		{
			std::shared_ptr<const NameNode> left, right;
			std::string         name;
			std::vector<NodeId> ids;   //!< all NodeIds with this name-path, in insertion order
			int                 count;
		};
		typedef std::shared_ptr<const NameNode> NameMapRef;

		//! Persistent reverse index NodeId -> current green node (O(log N)).
		struct IdMapNode
		{
			std::shared_ptr<const IdMapNode> left, right;
			NodeId  key;
			NodeRef val;
			std::int64_t label;   //!< top-level order-maintenance label (0 for param ids)
			int          count;
		};
		typedef std::shared_ptr<const IdMapNode> IdMapRef;

		//! Persistent per-occurrence param identity index keyed by (chunkId, role, occurrence).
		//! Content-matched across chunk edits so sibling insert/remove does not remap unrelated ids.
		struct ParamMapNode
		{
			std::shared_ptr<const ParamMapNode> left, right;
			std::string key;    //!< "<chunkId>\x1f<role>\x1f<occurrence-index>"
			NodeId      id;
			int         count;
		};
		typedef std::shared_ptr<const ParamMapNode> ParamMapRef;

		//! Persistent parsed document: item rope, identity/name/reverse/param indices, and monotonic id source.
		struct Document
		{
			SeqRef      items;     //!< top-level item rope
			IdSeqRef    idseq;     //!< position -> NodeId, lockstep with items
			NameMapRef  byName;    //!< "keyword/name" -> NodeId list
			IdMapRef    byId;      //!< NodeId -> current green node
			ParamMapRef paramIds;  //!< (chunkId, role, occurrence) -> param NodeId
			NodeId      nextId = 1;
			//! Count of top-level `standard_object` chunks carrying a live `source` instancing param.
			//! Maintained in O(1) across parse/replace/insert/erase so DeriveToJobIncremental can refuse in O(1).
			int         sourceInstanceCount = 0;
		};

		//! Optimistic-concurrency identity of a Job's retained CST head
		//! (docs/gui/EDITOR_STATE_AND_TRANSACTION_HARDENING.md §3.4,
		//! docs/agentic-redesign/50-agentic-surface.md §2.2.2/§2.4).
		//! Both fields must match for a base precondition to succeed; stays under 2^53 for exact JSON numbers.
		struct CstHeadVersion
		{
			std::uint64_t uuid     = 0;   //!< retained head identity; 0 = none; minted fresh per load
			std::uint64_t revision = 0;   //!< bumps iff retained Document content changed
		};
		inline bool operator==( const CstHeadVersion& a, const CstHeadVersion& b )
		{
			return a.uuid == b.uuid && a.revision == b.revision;
		}
		inline bool operator!=( const CstHeadVersion& a, const CstHeadVersion& b )
		{
			return !( a == b );
		}

		//! Lossless bytes -> CST parse (every byte lands in exactly one leaf in document order).
		Document ParseToCst( const std::string& bytes );

		//! CST -> bytes (byte-identical to parsed input for an unedited tree).
		std::string SerializeCst( const Document& doc );

		//! Serialize a single top-level node to its verbatim bytes (empty for null).
		std::string SerializeNode( const NodeRef& n );

		//! True iff `doc` is native v7 form: `RISE ASCII SCENE <n>` header + chunks + render-neutral `>`
		//! directives (`> echo`, `> set accelerator`) only. Rejects `FOR`/`ENDFOR`, `> run`/`> load`,
		//! render-affecting `>` directives, and missing/malformed headers. Version-number agnostic.
		bool IsNativeV7Document( const Document& doc );

		//! Derive all chunks into `pJob` through the live descriptor registry (resets parser state first).
		//! Returns the number of chunks applied.
		//!
		//! Two-tier boundary (both tiers report in `diagnostics` when non-null):
		//!   * Pass 1 (validation via DispatchChunkParameters): REFUSE-ALL; nothing is applied on any error.
		//!   * Pass 2 (apply via Finalize): continues past failing chunks so all independent errors surface;
		//!     earlier/later valid chunks stay applied (callers either discard a throwaway Job, surface a
		//!     partial-scene warning, or rebind on committing re-derive).
		//!
		//! When `outRecorded` is non-null, resets it at entry and records actual engine productions/resolutions
		//! (GenericManager + participating media) into `outRecorded->dependents` (chunk-level).
		//! When `warnDeprecated` is true (initial scene load only), logs one warning per deprecated chunk type.
		int DeriveToJob( const Document& doc, IJob& pJob, std::vector<std::string>* diagnostics = nullptr, ReferenceGraph* outRecorded = nullptr, const char* activeVariantOverride = nullptr, bool warnDeprecated = false );

		//! Incrementally re-apply `chunkIds` (from `DocEditClosure`) into a live Job in O(closure * log N).
		//! Drops and re-Finalizes non-object closure entities (entities-first) and re-points `standard_object` /
		//! `csg_object` in place so raw TLAS addresses stay valid; invalidates TLAS only when a world bbox
		//! changes and bumps light-topology generation only when the emitter set or a Light changes.
		//!
		//! Atomic refuse-all: runs a whole-plan preflight before mutation and rolls back addref'd non-object
		//! entities if re-Finalize fails. Returns `chunkIds.size()` on success, or 0 (Job untouched; caller
		//! must full re-derive) on refusal: unnamed chunks, non-single-manager categories (Painter/Camera/Function),
		//! composed materials, `translucent_material`, `gltf_import`, other object-spawning chunks, CSG operand
		//! re-binding, documents with animations/timelines, `override_object`, or `sourceInstanceCount > 0`.
		int DeriveToJobIncremental( const Document& doc, IJob& pJob, const std::vector<NodeId>& chunkIds, std::vector<std::string>* diagnostics = nullptr );

		//==============================================================
		// Persistent Document queries and O(log N) rope edits
		//==============================================================

		int DocItemCount( const Document& doc );
		//! Total serialized byte width (O(1) root aggregate).
		size_t DocByteWidth( const Document& doc );
		//! Total '\n' count (O(1) root aggregate).
		int DocNewlineCount( const Document& doc );

		//! Test-only relaxed-atomic instrumentation counters for O(log N)/O(N) cost gates.
		//! Safe under concurrent parses (per-parse parser state and derive sinks are thread_local).
		unsigned long DebugItemStatWalks();
		unsigned long DebugReparseOldVisits();
		unsigned long DebugReflowLabelWrites();
		unsigned long DebugParamMatchVisits();
		unsigned long DebugChunkRefsComputed();

		//! Locate top-level item spanning byte `offset` in O(log N) (-1 if out of range).
		int DocItemAtByteOffset( const Document& doc, size_t offset,
		                         NodeRef* outItem, size_t* outStart, int* visits );

		//! Start byte offset of top-level item `index` in O(log N) (`(size_t)-1` if out of range).
		size_t DocByteOffsetOfItem( const Document& doc, int index );

		//! Tight byte range (`role value...`, excluding leading indentation trivia) of the `occ`-th
		//! `role` param in `chunkId`. Returns false if chunk or occurrence is absent. O(chunk kids).
		bool DocByteRangeOfParam( const Document& doc, NodeId chunkId, const std::string& role,
		                          int occ, size_t* outOffset, size_t* outLength );

		//! Parser-semantics value of `role` on `chunk` (LAST occurrence wins, multi-token values joined
		//! with internal trivia). Do NOT use for occurrence-addressed undo capture (use ParamValueAtOccurrence).
		std::string ParamValueAsParsed( const NodeRef& chunk, const std::string& role,
		                                bool* outPresent = nullptr );

		//! Canonical value join for an already-selected Param node (empty for null/non-Param).
		std::string ParamValueOfNode( const NodeRef& param, size_t* childVisits = nullptr );

		//! Evaluate `standard_object` `count_u`/`count_v` using the derive-time expander against `doc`'s
		//! `let` bindings (`count_v` defaults to 1). Returns false (outputs set to 1) if `count_u` is absent
		//! or evaluation fails.
		bool EvaluateObjectRepeatCounts( const Document& doc, const NodeRef& chunk,
		                                  int& outCountU, int& outCountV );

		//! Value of the `occ`-th `role` param counted from the front (0 = first), matching write addressing.
		//! Returns "" (`*outPresent = false`) if null, negative `occ`, or out of range. O(chunk kids).
		std::string ParamValueAtOccurrence( const NodeRef& chunk, const std::string& role,
		                                    int occ, bool* outPresent = nullptr );

		//! Number of `role` params on `chunk` (0 if absent/null).
		int ParamOccurrenceCount( const NodeRef& chunk, const std::string& role );

		//! Path-copy replace of top-level item `index` (O(log N)). Preserves chunk NodeId and content-matches
		//! param NodeIds (invalidating ambiguous repeated params into `*invalidated`). No-op if null or out of range.
		Document DocReplaceItem( const Document& doc, int index, NodeRef newItem, int* visits = nullptr, std::vector<NodeId>* invalidated = nullptr );

		//! Insert `newItem` verbatim before `index` (clamped to [0,count]); no-op on null.
		//! Rope/index splices are O(log N); gap-exhausting order-label reflow is windowed (O(N log N) worst-case).
		//! Caller supplies any token-separating trivia needed for round-trip well-formedness.
		Document DocInsertItem( const Document& doc, int index, NodeRef newItem, int* visits = nullptr );
		//! Erase item `index` (no-op if out of range). `*invalidated` receives erased chunk + param NodeIds.
		Document DocEraseItem ( const Document& doc, int index, int* visits = nullptr, std::vector<NodeId>* invalidated = nullptr );

		//! Exact inverse of DocInsertItem (thin wrapper over DocEraseItem; preserves sibling NodeIds).
		Document DocRemoveItem( const Document& doc, int index, int* visits = nullptr );

		//! Trivia-preserving chunk erase safe for any file-authored or inserted chunk: removes `index`
		//! and collapses its trailing whitespace Trivia only when provably glue-safe (preceding bytes end
		//! in `\n`, or `index == 0`). Preserves final trailing newline; no-op if out of range.
		Document DocEraseChunkTidy( const Document& doc, int index, int* visits = nullptr );

		//==============================================================
		// NodeId identity, name-path addressing, and reference graph
		//==============================================================

		//! NodeId of top-level item `index` in O(log N) (0 if out of range).
		NodeId DocNodeIdAt( const Document& doc, int index, int* visits = nullptr );

		//! Canonical `"keyword/name"` key for a named chunk item (`""` for unnamed or non-chunk).
		std::string ChunkNamePath( const NodeRef& item );

		//! Directed migration advice for a removed historical chunk keyword, or null if unknown.
		const char* RetiredChunkAdvice( const std::string& kw );

		//! Resolve `"keyword/name"` in O(log N), requiring uniqueness (returns 0 if absent or >1 matches;
		//! `*occurrences` distinguishes 0 vs >1).
		NodeId DocFindByName( const Document& doc, const std::string& namePath, int* visits = nullptr, int* occurrences = nullptr );

		//! O(N) bare-name lookup, unique-or-refuse (0 if absent or ambiguous). Non-empty `roleKindSuffix`
		//! is a hard kind filter via `RoleMatchesKindConstraint`; `*occurrences` reports post-narrowing count.
		//! Opt-in `uniqueFallback` resolves a sole unnamed suffix-matching chunk when no named match exists.
		NodeId DocFindByNameAnyRole( const Document& doc, const std::string& bareName, int* occurrences = nullptr, const std::string& roleKindSuffix = std::string(), bool uniqueFallback = false );

		//! Returns the chunk deciding transform param `role` for `chunkId`: the last same-named
		//! `override_object` if one exists, else `chunkId`. Unchanged for non-transform roles,
		//! non-Object chunks, unnamed chunks, or `override_object` targets. O(N) on transform roles.
		NodeId DocTransformOwnerId( const Document& doc, NodeId chunkId, const std::string& role, bool* outIsObjectTransform = nullptr );

		//! Shared kind-constraint predicate (exact match, `_<suffix>`, or DescriptorRegistry category union;
		//! empty suffix matches all).
		bool RoleMatchesKindConstraint( const std::string& role, const std::string& roleKindSuffix );

		//! True iff unnamed positional camera fallback is permitted for `bareName`: empty name,
		//! equals `activeCameraName`, or document contains exactly one suffix-matching camera chunk.
		bool DocCameraUniqueFallbackPermitted( const Document& doc, const std::string& bareName, const std::string& activeCameraName );

		//! Resolve durable NodeId -> current green node in O(log N) (null if gone).
		NodeRef DocResolveNodeId( const Document& doc, NodeId id, int* visits = nullptr );

		//! Current top-level index of `id` in O(log N) (-1 if absent or not top-level).
		int DocIndexOfNodeId( const Document& doc, NodeId id, NodeRef* outItem, int* visits = nullptr );

		//! Resolve byte `offset` to innermost Param + enclosing chunk + optional NodeIds (O(log N) + O(params)).
		NodeRef DocParamAtByteOffset( const Document& doc, size_t offset, NodeRef* outChunk, int* visits,
		                              NodeId* outParamId = nullptr, NodeId* outChunkId = nullptr );

		//! Resolve (chunkId, role, occ) -> stable param NodeId in O(log N) (0 if absent).
		NodeId DocParamId( const Document& doc, NodeId chunkId, const std::string& role, int occ = 0 );

		//! Point-in-time snapshot of all occurrences of repeatable param `role` in `chunkId` (document order).
		struct RepeatGroupView {
			NodeId               chunkId;
			std::string          role;
			std::vector<NodeRef> occurrences;     //!< Param green nodes in document order
			std::vector<NodeId>  occurrenceIds;   //!< parallel stable per-occurrence NodeIds
		};

		RepeatGroupView DocRepeatGroup( const Document& doc, NodeId chunkId, const std::string& role );
		int DocRepeatCount( const Document& doc, NodeId chunkId, const std::string& role );
		NodeId DocRepeatElementId( const Document& doc, NodeId chunkId, const std::string& role, int index, NodeRef* outParam = nullptr );

		//! Resolved reference edge from referring param NodeId to target chunk NodeId.
		struct ReferenceUse { NodeId sourceValueNodeId; NodeId targetNodeId; };

		//! Structured dangling-reference record emitted alongside string diagnostics in BuildReferenceGraph.
		struct UnresolvedReference
		{
			NodeId       sourceChunkId;   //!< referring chunk NodeId
			std::string  chunkKeyword;    //!< e.g. "lambertian_material"
			std::string  param;           //!< e.g. "reflectance"
			std::string  value;           //!< unresolved token
		};

		//! Static reference graph + conservative fingerprint `stamp` (folds chunk keyword, name, ref params,
		//! and NodeIds: stamp-unchanged implies graph-unchanged; use MaintainedReferenceGraph for O(log N) reuse).
		struct ReferenceGraph {
			std::vector<ReferenceUse>             edges;        //!< resolved (sourceParam -> targetChunk) edges
			std::map<NodeId, std::set<NodeId> >    dependents;  //!< reverse adjacency: targetChunk -> referring chunks
			unsigned long long                    stamp = 0;    //!< conservative content+identity fingerprint
		};

		//! Thin wrapper returning `BuildReferenceGraph(doc, diagnostics).edges`.
		std::vector<ReferenceUse> TraceReferences( const Document& doc, std::vector<std::string>* diagnostics = nullptr );

		//! Authoritative descriptor-based static reference graph (O(N log N)). Resolves explicit Reference
		//! and tuple-Reference params over CST chunks + runtime defaults (`none`, `Default*`), treating pure
		//! numeric literals as inline values rather than dangling refs. Dynamic derive-time refs (timelines/expr)
		//! are not traced; same-name colour/scalar painters use a conservative superset alias.
		ReferenceGraph BuildReferenceGraph( const Document& doc, std::vector<std::string>* diagnostics = nullptr,
			std::vector<UnresolvedReference>* unresolved = nullptr );

		//! Reparse `newText` and preserve NodeIds best-effort (1: full-content multiset-exact for chunks /
		//! greedy for trivia; 2: unique `(keyword,name)`; 3: unique `keyword`), invalidating ambiguous or
		//! deleted ids into `*invalidated`. O(M+N) matching, O(M log M) index rebuild.
		Document DocReparse( const Document& oldDoc, const std::string& newText, std::vector<NodeId>* invalidated = nullptr );

		//! Set `occ`-th `role` param on `chunkId` to `newValue` (preserving chunk NodeId and unaffected trivia).
		//! No-op if chunk or param is absent. O(log N) + chunk rebuild.
		Document DocSetParamValue( const Document& doc, NodeId chunkId, const std::string& role, int occ, const std::string& newValue, int* visits = nullptr );

		//! Like DocSetParamValue, inserting a new `role value` line before `}` if absent (`*inserted` set).
		Document DocSetOrAddParamValue( const Document& doc, NodeId chunkId, const std::string& role, int occ, const std::string& newValue, bool* inserted = nullptr, int* visits = nullptr );

		//! Remove all occurrences of `role` from `chunkId` (brace-safe; idempotent).
		Document DocRemoveParam( const Document& doc, NodeId chunkId, const std::string& role, int* visits = nullptr );

		//! Remove only the `occ`-th (0-based) occurrence of `role`, preserving sibling occurrences byte-exact.
		Document DocRemoveParamOcc( const Document& doc, NodeId chunkId, const std::string& role, int occ, int* visits = nullptr );

		//! Replace `chunkId` verbatim with single-chunk `newText` iff it currently serializes to `expectText`.
		//! Preserves chunk NodeId; returns `doc` unchanged (`*replaced = false`) on mismatch or invalid `newText`.
		Document DocReplaceChunkIfText( const Document& doc, NodeId chunkId, const std::string& expectText, const std::string& newText, bool* replaced = nullptr );

		//! Atomic chunk rename: updates `name` and rewrites all plain and tuple referrers (preserving NodeId).
		//! Refuses atomically (returns `doc` unchanged + diagnostic) if `newName` is empty, collides in
		//! ChunkCategory, matches a reserved runtime default, or if the document has animations/timelines.
		Document DocRename( const Document& doc, NodeId chunkId, const std::string& newName, std::vector<std::string>* diagnostics = nullptr );

		//! Transitive re-derive closure (`changedChunkId` first) via `ReferenceGraph::dependents`.
		//! Doc-only overload pays O(N log N) BuildReferenceGraph; graph overload is O(closure * log N).
		std::vector<NodeId> DocEditClosure( const Document& doc, NodeId changedChunkId );
		std::vector<NodeId> DocEditClosure( NodeId changedChunkId, const ReferenceGraph& graph );

		//! Incrementally maintained Document + ReferenceGraph: non-ref edits reuse in O(log N),
		//! ref/cp edits re-resolve only the edited chunk in O(refs * log N), and name / painter-alias
		//! edits rebuild in O(N). Structural edits require a fresh instance.
		class MaintainedReferenceGraph
		{
		public:
			explicit MaintainedReferenceGraph( const Document& doc );
			const Document&       Doc() const   { return m_doc; }
			//! Returns held graph, lazily reflattening `edges` (O(N)) if dirtied by an incremental edit.
			const ReferenceGraph& Graph() const;
			void SetParamValue( NodeId chunkId, const std::string& paramRole, int occurrence, const std::string& value );
			//! O(closure * log N) closure over always-current `dependents` (does not trigger lazy edges reflatten).
			std::vector<NodeId> EditClosure( NodeId changedChunkId ) const { return DocEditClosure( changedChunkId, m_graph ); }
			//! True if the last SetParamValue performed an O(N) rebuild rather than incremental reuse.
			bool LastEditRebuilt() const { return m_lastRebuilt; }
		private:
			void RebuildAll();
			Document               m_doc;
			mutable ReferenceGraph m_graph;
			bool                   m_lastRebuilt;
			mutable bool           m_edgesDirty;
			std::map<std::pair<int,std::string>, NodeId> m_defs;          // (category, name) namespace reused across ref edits
			std::map<NodeId, std::vector<ReferenceUse> > m_chunkEdges;    // per-chunk source of truth
			std::map<NodeId, unsigned long long>         m_chunkCs;       // per-chunk commutative stamp
			std::set<NodeId>                             m_aliasInvolved; // same-name cross-kind painters (conservative rebuild)
		};
	}
}

#endif
