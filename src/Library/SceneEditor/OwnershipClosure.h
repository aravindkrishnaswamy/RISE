//////////////////////////////////////////////////////////////////////
//
//  OwnershipClosure.h - Pre-mutation ownership-closure rule & refusal
//    policy (docs/gui/NODE_GRAPH_CANVAS.md §6, docs/gui/MATERIAL_EDITOR.md
//    §3.7a, docs/gui/ENTITY_CREATION.md §5).
//
//    Rule:
//      1. Compute the solely-owned closure reachable from the edited chunk.
//      2. Rewrite only the closure (every outside byte stays identical).
//      3. Refuse if shared (>= 2 root owners), unaddressable, or
//         expression-driven, naming shared chunk(s), out-of-closure
//         referrers, and the Duplicate-node escape hatch.
//
//    Ownership model: non-(Painter|Function) chunks are ROOTS that own
//    themselves; interior nodes inherit the union of their referrers'
//    owners (a referrer-less interior node owns itself). `Owners(c).size() >= 2`
//    means shared across independent graphs; `== 1` means solely owned.
//
//    Pure edit-policy layer over `SceneReferenceGraph` (caller must run
//    `ConnectionLegality` first; performs no mutations or orphan deletion).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_OWNERSHIPCLOSURE_
#define RISE_OWNERSHIPCLOSURE_

#include "ReferenceGraph.h"
#include "../Cst/Cst.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace RISE
{
	// Authoritative printf-style refusal format strings (shared with tests).

	//! Shared-target refusal: %s = shared chunk, %s = owners, %s = out-of-closure referrers.
	inline constexpr const char* const kClosureSharedFmt =
		"this topology edit would rewrite `%s`, which is shared between %s -- referenced from %s.  "
		"Rewriting it here would silently change the other graph(s) too.";

	//! Escape-hatch suffix appended to refusals that support node duplication.
	inline constexpr const char* const kClosureDuplicateHatch =
		"  Duplicate the node to fork an owned copy, then repeat the edit.";

	//! Expression-driven refusal: %s = chunk name, %s = keyword.
	inline constexpr const char* const kClosureExpressionFmt =
		"this topology edit would rewrite `%s`, a `%s` chunk whose value is driven by an authored "
		"expression body (`expr` / `def`) the editor cannot rewrite as a unit -- edit the source instead.";

	//! Ambiguous-name refusal: %s = name, %d = match count.
	inline constexpr const char* const kClosureAmbiguousFmt =
		"this topology edit names `%s`, which %d chunks in this category share -- the editor refuses "
		"to guess which one it addresses rather than rewrite the wrong chunk.";

	//! Unresolvable-target refusal: %s = name.
	inline constexpr const char* const kClosureUnresolvedFmt =
		"this topology edit names `%s`, which resolves to no chunk in this document.";

	// Reference-safe delete refusal formats (docs/gui/ENTITY_CREATION.md §5.2).

	//! Still-referenced refusal: %s = target, %s = `referrer`.`param` list, %s = target.
	inline constexpr const char* const kDeleteReferencedFmt =
		"`%s` cannot be deleted: it is still referenced from %s.  Rewire those references away first, or "
		"delete the referring chunk(s) too -- removing it now would leave a dangling reference the scene "
		"could not reload.  Cascade mode does not help here: it sweeps only the chunks BELOW `%s` that "
		"nothing else uses, never the chunks that reference it.";

	//! Shared-cascade refusal (load-bearing cross-check via `OwnershipClosure::OwnersOf`):
	//! %s = target, %s = shared cascade member, %s = owners, %s = target.
	inline constexpr const char* const kDeleteCascadeSharedFmt =
		"cascade-deleting `%s` would also remove `%s`, which is owned by %s -- a shared chunk NEVER "
		"cascades.  Delete `%s` on its own (cascade off), or rewire the other owner(s) off the shared "
		"chunk first.";

	//! Mode-neutral unaddressable-member refusal (TargetOnly and Cascade): %s = display name, %s = reason.
	inline constexpr const char* const kDeleteUnaddressableFmt =
		"delete refused: `%s` cannot be addressed for removal (%s) -- refusing rather than "
		"erasing a chunk the checks never inspected.";

	//! Topology edit kind. `Detach` is pre-flight computable here; committing a
	//! detach requires a NodeId-free legality check for the runtime `none` sentinel.
	enum class TopologyEditKind
	{
		Rewire = 0,
		Detach = 1
	};

	//! NodeId-addressed topology edit intent.
	struct TopologyEditIntent
	{
		TopologyEditKind kind        = TopologyEditKind::Rewire;
		Cst::NodeId      targetChunk = 0;   //!< chunk whose reference param would be rewritten
		std::string      paramName;          //!< reference param role on targetChunk
		int              occurrence  = 0;    //!< 0-based occurrence among same-role siblings
		Cst::NodeId      newRefChunk = 0;    //!< Rewire only; ignored (expected 0) for Detach
	};

	//! Machine-readable closure verdict.
	enum class ClosureClassification
	{
		Clean = 0,              //!< Solely owned; edit rewrites `closure` only
		UnresolvedTarget,       //!< `targetChunk` does not resolve to a registered chunk
		AmbiguousTargetName,    //!< Reserved for name-addressed callers (e.g. RewireConnection)
		ExpressionDrivenTarget, //!< Target is expression-bodied (`expr`/`def`)
		SharedTarget            //!< Interior node owned by >= 2 roots
	};

	//! One chunk in the computed closure.
	struct ClosureChunk
	{
		Cst::NodeId   id       = 0;
		String        name;                              //!< "" for an unnamed chunk
		String        keyword;
		ChunkCategory category = ChunkCategory::Painter;
	};

	//! Closure verdict, solely-owned chunk set, and refusal diagnostics.
	struct OwnershipClosureResult
	{
		ClosureClassification classification = ClosureClassification::UnresolvedTarget;

		//! True iff the edit may proceed. On refusal the caller MUST leave the document byte-identical.
		bool Clean() const { return classification == ClosureClassification::Clean; }

		//! Solely-owned target + transitively reachable chunks sharing the exact same owner set (document order). Empty on refusal.
		std::vector<ClosureChunk> closure;

		//! Shared chunk(s) blocking the edit (1 entry for `SharedTarget`; empty otherwise).
		std::vector<String> sharedChunks;

		//! Referrers outside the closure that would be silently affected (`SharedTarget`).
		std::vector<String> outOfClosureReferrers;

		//! Owning root chunks (size 1 on `Clean`, >= 2 on `SharedTarget`; empty on
		//! `ExpressionDrivenTarget` / unresolved). Unnamed roots format as `<keyword>`.
		std::vector<String> owners;

		//! Chunks whose referrer count would drop to 0 if this edit commits (not cascaded;
		//! at most 1 entry for Rewire). Caller MUST clear/ignore if the commit does not apply.
		std::vector<String> nowUnreferenced;

		//! Formatted refusal diagnostic (from `kClosure*Fmt`). Empty iff `Clean()`.
		std::string diagnostic;
	};

	class OwnershipClosure
	{
	public:
		//! Prebuilt chunk index + reverse/forward adjacency for batch `OwnersOf`/`Compute`
		//! passes (one O(N log N + E) build instead of per-call rebuilds). Construct via `BuildAdjacencyFor`.
		struct Adjacency
		{
			std::map<Cst::NodeId, const SceneReferenceGraph::DocumentChunk*> byId;
			std::map<Cst::NodeId, std::set<Cst::NodeId> > rev;   //!< target -> referrers
			std::map<Cst::NodeId, std::set<Cst::NodeId> > fwd;   //!< referrer -> targets (`Compute` only)
		};

		//! Build `Adjacency` once for `doc`. Optional `prescanned`/`snapshot`, when non-null,
		//! MUST come from `SceneReferenceGraph::AllChunks(doc)` / `EdgesAndDangling(doc, prescanned)`.
		static Adjacency BuildAdjacencyFor(
			const Cst::Document& doc,
			const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned = nullptr,
			const SceneReferenceGraph::Snapshot* snapshot = nullptr );

		//! Pure ownership-closure computation (no Job/manager/lock). Optional `prescanned`,
		//! `snapshot`, and `adjacency` must match `doc`; when `adjacency` is non-null,
		//! `snapshot` is unused, while `prescanned` is still used for document-order listing.
		static OwnershipClosureResult Compute(
			const Cst::Document& doc,
			const TopologyEditIntent& intent,
			const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned = nullptr,
			const SceneReferenceGraph::Snapshot* snapshot = nullptr,
			const Adjacency* adjacency = nullptr );

		//! Sorted, duplicate-free owning root NodeIds for `chunk` (empty if unresolved).
		//! Iterative and cycle-safe (cycle members report themselves as owners).
		//! When `adjacency` is non-null, `prescanned`/`snapshot` are ignored.
		static std::vector<Cst::NodeId> OwnersOf(
			const Cst::Document& doc,
			Cst::NodeId chunk,
			const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned = nullptr,
			const SceneReferenceGraph::Snapshot* snapshot = nullptr,
			const Adjacency* adjacency = nullptr );

		//! True iff `category` is an interior graph node (Painter or Function) rather than a root.
		static bool IsInteriorCategory( ChunkCategory category );
	};
}

#endif
