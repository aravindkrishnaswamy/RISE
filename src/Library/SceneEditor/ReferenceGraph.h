//////////////////////////////////////////////////////////////////////
//
//  ReferenceGraph.h - Shared reference-graph query surface (doc-88
//    Phase 3 S11, docs/gui/NODE_GRAPH_CANVAS.md sect. 6).
//
//    Closes the gap ENTITY_CREATION.md:487-529 named "TO-BUILD": a
//    `FindReferencesTo(category, name)` query that answers, for any
//    named chunk, EVERY other chunk that references it -- by param
//    name and occurrence, typed by the port's declared
//    `referenceCategories`.  ONE implementation, TWO consumers: the
//    node-graph canvas (S11-S23) and reference-safe delete
//    (ENTITY_CREATION.md sect. 5).
//
//    WHY THIS WRAPS Cst::BuildReferenceGraph INSTEAD OF RE-WALKING.
//    `Cst::BuildReferenceGraph` already gets RESOLUTION right in every
//    case that is genuinely hard to get right: the painter same-name
//    colour/scalar alias (a `scalar_painter` and a colour painter
//    sharing one name), the dimension-precise Function1D-vs-Function2D
//    sub-namespace, the colour-painter <-> Function2D dual-register,
//    and the runtime-default namespace (`none`, `Default*`).
//    Re-deriving any of that here -- even partially, even "just for
//    the common case" -- would be a second copy of that logic free to
//    drift from the first. So RESOLUTION (which chunk a reference
//    value actually names) is delegated ENTIRELY to
//    `Cst::BuildReferenceGraph`'s `edges` array.
//
//    What `edges` does NOT carry is the presentation detail a canvas
//    or a delete-safety scan needs: WHICH param, WHICH occurrence, and
//    WHAT port type declared the reference (`ReferenceUse` is only
//    `{sourceValueNodeId, targetNodeId}` -- see Cst.h). Recovering
//    that is a purely STRUCTURAL question (which (chunk, role,
//    occurrence) triple does this srcParam NodeId name?), answered by
//    walking each chunk's own Param children once and asking
//    `Cst::DocParamId` whether ITS occurrence's NodeId is a key
//    `BuildReferenceGraph` already resolved -- no re-derivation of
//    WHERE a reference points, only WHICH line wrote it. See Edges()
//    in the .cpp for the exact walk.
//
//    NAMED `SceneReferenceGraph`, NOT `ReferenceGraph`.  The type this
//    file wraps is `RISE::Cst::ReferenceGraph` (Cst.h) -- a different
//    type in a different (nested) namespace, but an identical bare
//    name.  Every call site so far has stayed unambiguous by always
//    qualifying the Cst one as `Cst::ReferenceGraph`, but that is a
//    convention a future careless `using namespace RISE::Cst;` (or a
//    grep that does not notice the namespace) can silently violate --
//    doc-88 S11 review round 1 P3 flagged the shadow as a standing
//    hazard.  Renaming was CHEAP to do here (this type is brand new in
//    this same slice, with exactly two consumers -- SceneEditController
//    and ReferenceGraphTest -- both edited in the same round), so it is
//    done now rather than left for a future rename under a live API.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_REFERENCEGRAPH_
#define RISE_REFERENCEGRAPH_

#include "../Cst/Cst.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Utilities/RString.h"
#include <vector>

namespace RISE
{
	//! One resolved reference edge between two named chunks in a
	//! `Cst::Document`.  Both a "forward" (FindReferencesFrom) and a
	//! "reverse" (FindReferencesTo) query return this same shape --
	//! only which side the caller already knew differs.
	struct ReferenceEdge
	{
		// ---- the REFERRING side: the chunk whose param HOLDS the reference ----
		Cst::NodeId   referrerId       = 0;   //!< the referrer chunk's stable NodeId
		String        referrerKeyword;         //!< e.g. "blend_painter", "ggx_material"
		ChunkCategory referrerCategory = ChunkCategory::Painter;
		String        referrerName;            //!< "" for an unnamed chunk (still a valid referrer)
		String        paramName;               //!< the Reference-kind param's role (e.g. "colora", "alphax")
		int           occurrence      = 0;     //!< 0-based occurrence among same-role siblings on this chunk
		bool          repeatable      = false; //!< the descriptor's own `repeatable` flag for this param
		//! The port's declared TYPE (`ParameterDescriptor::referenceCategories`)
		//! -- what KIND of chunk this slot accepts, e.g. {Painter} for
		//! `alphax`, {Painter,Function} for a domain-warp slot. Empty both
		//! when the referring param has no registered descriptor entry at
		//! all (a structurally-resolved edge whose descriptor lookup
		//! failed -- see Edges()'s header note) AND when the param IS
		//! registered but declares no reference-typed slot at all -- e.g.
		//! `piecewise_linear_function2d`'s `cp` (a literal x/y control-point
		//! pair, not a reference token) can still show up here if a FUTURE
		//! descriptor change mis-declares it as `ValueKind::Reference`
		//! without populating `referenceCategories`; an empty vector is not
		//! by itself proof of "no descriptor", only "no declared target
		//! category" -- check `portCategories.empty()` together with
		//! whether the descriptor lookup is expected to have succeeded.
		std::vector<ChunkCategory> portCategories;

		// ---- the REFERENCED side: the chunk the value NAMES ----
		Cst::NodeId   targetId         = 0;
		ChunkCategory targetCategory   = ChunkCategory::Painter;
		String        targetName;
	};

	//! The shared reference-graph query surface.  Every method is a pure
	//! function of a `Cst::Document` -- no live Job, no manager, no
	//! controller lock -- so it is directly unit-testable against an
	//! in-test parsed scene string, and callable from any consumer that
	//! already holds a Document (the canvas assembler, a future
	//! reference-safe-delete scan, an agent-facing diagnostic).
	class SceneReferenceGraph
	{
	public:
		//! One top-level chunk's structural identity in `doc`, in document
		//! order -- the SAME per-chunk facts `Edges()`'s internal walk
		//! needs, exposed so a caller assembling SEVERAL things from one
		//! document (a node-graph's node seeding, its edges, AND its
		//! dangling ports) can scan the document exactly ONCE and hand the
		//! result to every query below, instead of letting each one
		//! re-scan.  `hasCategory` is false when the chunk's keyword has no
		//! registered descriptor (an unknown/malformed chunk a well-formed
		//! document never contains, but a hostile/synthetic test document
		//! might) -- such a chunk can never be a graph node and is skipped
		//! everywhere a category is needed, never crashed on.
		//!
		//! doc-88 S11 review round 1 P1-1: the PRIOR design left this scan
		//! private and had every per-name lookup (`ResolveChunk`) re-run
		//! it -- fine for a one-off query, but the DAG assembler was
		//! calling `ResolveChunk` once PER painter/material NAME, turning
		//! one O(N log N) document walk into O(nodes * N log N) (measured
		//! 24s under a held mutex on a 7442-chunk scene). Exposing the scan
		//! is what let that caller collapse back down to O(N log N) total.
		struct DocumentChunk
		{
			Cst::NodeId   id = 0;
			Cst::NodeRef  item;
			String        keyword;
			bool          hasCategory = false;
			ChunkCategory category    = ChunkCategory::Painter;
			String        name;   //!< "" (String's <=1-is-empty convention) for an unnamed chunk
		};

		//! Every top-level Chunk item in `doc`, in document order. O(N log N)
		//! (DocNodeIdAt + DocResolveNodeId are each O(log N); DescriptorForKeyword
		//! and ParamValueAsParsed are O(1)/O(chunk kids) respectively). THE
		//! single scan every other method in this class is built from --
		//! callers doing more than one query against the same `doc` should
		//! call this ONCE and pass the result to `Edges`/`EdgesAndDangling`
		//! below via their `prescanned` parameter.
		static std::vector<DocumentChunk> AllChunks( const Cst::Document& doc );

		//! Resolve (category, name) to the unique chunk NodeId that
		//! declares it, or 0 when no such chunk exists OR the name is
		//! AMBIGUOUS within that category (more than one same-category
		//! chunk shares the name) -- refuse rather than guess, the same
		//! posture as `Cst::DocFindByName`. `*outOccurrences` (optional)
		//! reports the raw match count so a caller can distinguish
		//! "no such name" (0) from "duplicate name" (>1).
		//!
		//! CATEGORY IS EXACT, not the UI-union `Cst::DocFindByNameAnyRole`
		//! resolves through ("painter" accepts Painter|Function) -- a
		//! node-graph node's identity is (declared descriptor category,
		//! name), so a Painter-category and a Function-category chunk
		//! sharing one name are two distinct, independently addressable
		//! nodes here, never merged the way the UI-union editor resolver
		//! merges them for interactive addressing.
		//!
		//! NOTE (doc-88 S11 review round 1 P1-2): TWO chunks of the SAME
		//! category CAN legally share a name (a colour painter and a
		//! `scalar_painter` both named "P", both ChunkCategory::Painter) --
		//! this is exactly the AMBIGUOUS case this method refuses rather
		//! than guess at, matching `Cst::BuildReferenceGraph`'s own
		//! documented "conservative same-name ALIAS" resolution for such
		//! pairs. A node-SEEDING caller that wants BOTH same-name chunks as
		//! distinct nodes must NOT go through this method (which can only
		//! ever hand back one, or neither) -- use `AllChunks` directly, one
		//! node per `DocumentChunk`, keyed by `id`, not by (category,name).
		static Cst::NodeId ResolveChunk( const Cst::Document& doc, ChunkCategory category,
		                                  const String& name, int* outOccurrences = nullptr );

		//! Every resolved reference edge anywhere in `doc`, one entry per
		//! (referrer chunk, param role, occurrence) whose value resolves to
		//! another chunk. THE canonical, document-wide edge list -- every
		//! other query below is a filter over this. O(N log N) (one
		//! `Cst::BuildReferenceGraph` pass plus one `Cst::DocParamId` probe
		//! per param occurrence in the document), the same complexity class
		//! `SceneEditController::BuildCategoryTreeLocked_` already pays per
		//! refresh -- cheap enough to call on every canvas snapshot refresh
		//! without a maintained/incremental cache (see the .cpp for why this
		//! module does not hold a `Cst::MaintainedReferenceGraph`).
		//!
		//! A caller that needs edges for SEVERAL chunks (the DAG assembler
		//! seeding every Painter/Material node's ports) MUST call this ONCE
		//! and filter/attribute per node -- calling FindReferencesFrom per
		//! node instead would re-run this O(N log N) walk once per node,
		//! turning an O(N log N) assembly into O(nodes * N log N).
		//!
		//! `prescanned`, when non-null, MUST be `AllChunks(doc)`'s result
		//! for this exact `doc` -- passing it skips the internal re-scan
		//! (see `AllChunks`'s own note). A caller that also needs dangling
		//! references should call `EdgesAndDangling` instead: it computes
		//! both from a SINGLE `Cst::BuildReferenceGraph` pass, where calling
		//! `Edges` then `DanglingReferences` separately would pay that pass
		//! twice.
		static std::vector<ReferenceEdge> Edges( const Cst::Document& doc,
		                                          const std::vector<DocumentChunk>* prescanned = nullptr );

		//! Every reference param anywhere in `doc` whose value does NOT
		//! resolve to any chunk -- a DANGLING port (an authoring typo, or a
		//! reference to a chunk that was since removed). Thin passthrough of
		//! `Cst::BuildReferenceGraph`'s `unresolved` out-param -- the
		//! dangling-port source a caller (the DAG assembler; a future
		//! reference-safe-delete scan) uses to render "a port with no node
		//! on the other end" instead of silently dropping the reference.
		//! See `EdgesAndDangling` for the combined form.
		static std::vector<Cst::UnresolvedReference> DanglingReferences( const Cst::Document& doc );

		//! Both `Edges(doc)` and `DanglingReferences(doc)`, from a SINGLE
		//! `Cst::BuildReferenceGraph` pass (doc-88 S11 review round 1 P1-1:
		//! calling `Edges` then `DanglingReferences` back to back, as the
		//! DAG assembler's seeding step used to, pays that O(N log N) pass
		//! TWICE for no reason -- `Cst::BuildReferenceGraph` already
		//! produces both halves from one walk when given both out-params).
		//! `prescanned`, when non-null, MUST be `AllChunks(doc)`'s result
		//! for this exact `doc` (see `Edges`'s note).
		struct Snapshot
		{
			std::vector<ReferenceEdge>            edges;
			std::vector<Cst::UnresolvedReference>  unresolved;
		};
		static Snapshot EdgesAndDangling( const Cst::Document& doc,
		                                   const std::vector<DocumentChunk>* prescanned = nullptr );

		//! Edges(doc), filtered to those whose TARGET is (category, name)
		//! -- "who references this chunk". Empty when (category, name)
		//! does not resolve (see ResolveChunk).
		//!
		//! `outOccurrences` (optional, doc-88 S11 review round 1 P1-3):
		//! the SAME raw match count `ResolveChunk` reports -- 0 means "no
		//! such chunk", 1 means the query resolved normally (an empty
		//! RETURN then genuinely means "zero referrers"), and >1 means the
		//! name is AMBIGUOUS in this category and the query was REFUSED
		//! (an empty return here means "refused", NOT "zero referrers").
		//! A caller that cannot tell those apart -- e.g. a reference-safe-
		//! delete scan (ENTITY_CREATION.md sect. 5) deciding whether a
		//! chunk is safe to remove -- MUST pass this and check it: reading
		//! an ambiguous refusal as "no referrers, safe to delete" would
		//! delete a chunk something might still be pointing at.
		static std::vector<ReferenceEdge> FindReferencesTo( const Cst::Document& doc,
		                                                     ChunkCategory category, const String& name,
		                                                     int* outOccurrences = nullptr );

		//! Edges(doc), filtered to those whose SOURCE (referrer) is
		//! (category, name) -- "what does this chunk reference", the
		//! canvas's out-edge / forward query. Empty when (category, name)
		//! does not resolve.
		//!
		//! `outOccurrences` -- see `FindReferencesTo`'s note; the same
		//! ambiguous-vs-empty distinction applies here.
		static std::vector<ReferenceEdge> FindReferencesFrom( const Cst::Document& doc,
		                                                       ChunkCategory category, const String& name,
		                                                       int* outOccurrences = nullptr );
	};
}

#endif
