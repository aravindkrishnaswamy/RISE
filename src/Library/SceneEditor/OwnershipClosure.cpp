//////////////////////////////////////////////////////////////////////
//
//  OwnershipClosure.cpp - See OwnershipClosure.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OwnershipClosure.h"
#include "ChunkDescriptorRegistry.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace RISE
{
	namespace
	{
		typedef SceneReferenceGraph::DocumentChunk DocumentChunk;

		//! String's own emptiness convention (it always carries its NUL, so
		//! an empty String has size() <= 1) -- the same test ReferenceGraph.cpp
		//! uses; Cst.h's String has no empty().
		bool StringEmpty( const String& s ) { return s.size() <= 1; }

		//! How a chunk is NAMED in a diagnostic.  A named chunk contributes
		//! its name; an UNNAMED one (a `film`, a rasterizer, an unnamed
		//! object) contributes `<keyword>` so a refusal never degrades into
		//! "shared between  and " -- an unnamed root is a perfectly ordinary
		//! owner (an unnamed `standard_object` binding a material is the
		//! commonest one in the corpus), not an error case.
		std::string DisplayName( const DocumentChunk& c )
		{
			if( !StringEmpty( c.name ) ) return std::string( c.name.c_str() );
			return "<" + std::string( c.keyword.c_str() ) + ">";
		}

		std::string JoinNames( const std::vector<String>& names )
		{
			std::string out;
			for( size_t i = 0; i < names.size(); ++i ) {
				if( i ) out += ( i + 1 == names.size() ) ? " and " : ", ";
				out += std::string( names[i].c_str() );
			}
			return out;
		}

		//! An EXPRESSION-BODIED chunk, in the only sense v7 still has one.
		//! MATERIAL_EDITOR.md sect. 3.7a's original hazard -- a `FOR`/macro-
		//! generated region with no single editable source line -- is
		//! obsolete post-CST-cutover (Cst.h: "the v7 runtime format --
		//! macro-free and expression-free"; a legacy v6 scene fails to load
		//! with a migration message rather than reaching this code).  What
		//! sect. 3.7a names as the RESIDUAL concern is "a chunk whose identity
		//! depends on a `let`/expression the editor can't rewrite as a
		//! unit", and in v7 that is exactly the texture-expression VM
		//! family: a chunk carrying an `expr` body plus repeatable `def`
		//! let-bindings (expression_painter / expression3d_painter /
		//! expression_function2d, and any future chunk built the same way).
		//! Detected STRUCTURALLY off the descriptor -- an `expr` parameter
		//! together with a repeatable `def` -- rather than by keyword
		//! allowlist, so a future expression-bodied chunk is covered the day
		//! it is registered instead of the day someone remembers to add it
		//! here.
		bool IsExpressionBodied( const ChunkDescriptor& d )
		{
			bool hasExpr = false, hasDefs = false;
			for( size_t i = 0; i < d.parameters.size(); ++i ) {
				const ParameterDescriptor& p = d.parameters[i];
				if( p.name == "expr" ) hasExpr = true;
				if( p.name == "def" && p.repeatable ) hasDefs = true;
			}
			return hasExpr && hasDefs;
		}

		//! Index `chunks` by NodeId once, so every lookup below is O(log N)
		//! instead of a linear re-scan.
		typedef std::map<Cst::NodeId, const DocumentChunk*> ChunkIndex;

		ChunkIndex IndexById( const std::vector<DocumentChunk>& chunks )
		{
			ChunkIndex idx;
			for( size_t i = 0; i < chunks.size(); ++i ) idx[ chunks[i].id ] = &chunks[i];
			return idx;
		}

		//! targetId -> the referring chunk ids (the REVERSE adjacency the
		//! owner walk needs).  A set per target, so a chunk that references
		//! the same target twice (blend.colora and blend.colorb both naming
		//! `P`) counts ONCE as a referrer -- see OwnershipClosure.h's
		//! "OVER-strict" note on why edge counts are the wrong unit.
		//!
		//! Renamed from the pre-P2-2 local `Adjacency` typedef to avoid
		//! colliding with the now-public `OwnershipClosure::Adjacency`
		//! bundle (S19 review round 1 P2-2) -- this is just the map TYPE
		//! that bundle's `rev`/`fwd` fields use.
		typedef std::map<Cst::NodeId, std::set<Cst::NodeId> > AdjMap;

		AdjMap BuildReverse( const std::vector<ReferenceEdge>& edges )
		{
			AdjMap rev;
			for( size_t i = 0; i < edges.size(); ++i )
				rev[ edges[i].targetId ].insert( edges[i].referrerId );
			return rev;
		}

		AdjMap BuildForward( const std::vector<ReferenceEdge>& edges )
		{
			AdjMap fwd;
			for( size_t i = 0; i < edges.size(); ++i )
				fwd[ edges[i].referrerId ].insert( edges[i].targetId );
			return fwd;
		}

		//! The owner walk, shared by `OwnersOf` and `Compute` (which needs
		//! it for the target AND for every candidate closure member, so it
		//! must not re-derive the adjacency each time).
		//!
		//! Iterative worklist + visited set: no recursion (a deep painter
		//! chain must not blow the stack) and cycle-safe (a hostile document
		//! whose graph contains a cycle terminates, reporting the cycle's
		//! members as their own owners rather than hanging -- the same
		//! hostile-input discipline S11's assembler documents).
		std::vector<Cst::NodeId> OwnersWalk( Cst::NodeId start, const ChunkIndex& byId, const AdjMap& rev )
		{
			std::vector<Cst::NodeId> owners;
			const ChunkIndex::const_iterator s = byId.find( start );
			if( s == byId.end() || !s->second->hasCategory ) return owners;

			std::set<Cst::NodeId> seen, out;
			std::vector<Cst::NodeId> work;
			work.push_back( start );
			seen.insert( start );

			while( !work.empty() ) {
				const Cst::NodeId cur = work.back();
				work.pop_back();
				const ChunkIndex::const_iterator it = byId.find( cur );
				// A referrer whose own chunk is missing from the scan (an
				// unknown/malformed keyword with no registered descriptor) is
				// still a real dependant, so it counts as a root rather than
				// being dropped -- dropping it would under-count owners and
				// let a shared edit through.
				if( it == byId.end() || !it->second->hasCategory
				 || !OwnershipClosure::IsInteriorCategory( it->second->category ) )
				{
					out.insert( cur );
					continue;
				}
				const AdjMap::const_iterator r = rev.find( cur );
				if( r == rev.end() || r->second.empty() ) {
					// An interior node nothing references owns ITSELF: an
					// orphan painter the user is mid-way through wiring up is
					// solely owned, not un-owned (un-owned would read as
					// "0 owners", which `Compute` would have to treat as
					// unresolvable).
					out.insert( cur );
					continue;
				}
				for( std::set<Cst::NodeId>::const_iterator p = r->second.begin(); p != r->second.end(); ++p ) {
					if( seen.insert( *p ).second ) work.push_back( *p );
				}
			}

			// A PURE CYCLE reaches no root at all: every referrer of every
			// member is another member, all of which the visited set stops
			// on, so the loop drains with nothing accumulated.  Zero owners
			// is not an answer any caller can use (`Compute` would have to
			// read it as "unresolvable", which it is not -- the chunk very
			// much exists), so report the cycle's own members as their own
			// owners: the header's documented hostile-input contract.  The
			// parser cannot produce such a document post-derive; a test or a
			// hand-edited file can (Cst::ParseToCst is pure syntax), and
			// PART 5d of RewireConnectionTest hands us exactly one.
			if( out.empty() ) out = seen;

			owners.assign( out.begin(), out.end() );   // std::set iterates sorted -- the documented NodeId order
			return owners;
		}
	}

	bool OwnershipClosure::IsInteriorCategory( ChunkCategory category )
	{
		return category == ChunkCategory::Painter || category == ChunkCategory::Function;
	}

	OwnershipClosure::Adjacency OwnershipClosure::BuildAdjacencyFor(
		const Cst::Document& doc,
		const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned,
		const SceneReferenceGraph::Snapshot* snapshot )
	{
		std::vector<DocumentChunk> localChunks;
		if( !prescanned ) { localChunks = SceneReferenceGraph::AllChunks( doc ); prescanned = &localChunks; }
		SceneReferenceGraph::Snapshot localSnap;
		if( !snapshot ) { localSnap = SceneReferenceGraph::EdgesAndDangling( doc, prescanned ); snapshot = &localSnap; }

		Adjacency adj;
		adj.byId = IndexById( *prescanned );
		adj.rev  = BuildReverse( snapshot->edges );
		adj.fwd  = BuildForward( snapshot->edges );
		return adj;
	}

	std::vector<Cst::NodeId> OwnershipClosure::OwnersOf(
		const Cst::Document& doc, Cst::NodeId chunk,
		const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned,
		const SceneReferenceGraph::Snapshot* snapshot,
		const Adjacency* adjacency )
	{
		// S19 review round 1 P2-2: a caller batching `OwnersOf` per node over
		// the SAME document (a canvas badge pass) passes a prebuilt
		// `adjacency` and skips this call's own index/reverse build entirely
		// -- `prescanned`/`snapshot` go unused, exactly as the header
		// documents.
		if( adjacency ) return OwnersWalk( chunk, adjacency->byId, adjacency->rev );

		std::vector<DocumentChunk> localChunks;
		if( !prescanned ) { localChunks = SceneReferenceGraph::AllChunks( doc ); prescanned = &localChunks; }
		SceneReferenceGraph::Snapshot localSnap;
		if( !snapshot ) { localSnap = SceneReferenceGraph::EdgesAndDangling( doc, prescanned ); snapshot = &localSnap; }

		const ChunkIndex byId = IndexById( *prescanned );
		const AdjMap     rev  = BuildReverse( snapshot->edges );
		return OwnersWalk( chunk, byId, rev );
	}

	OwnershipClosureResult OwnershipClosure::Compute(
		const Cst::Document& doc,
		const TopologyEditIntent& intent,
		const std::vector<SceneReferenceGraph::DocumentChunk>* prescanned,
		const SceneReferenceGraph::Snapshot* snapshot,
		const Adjacency* adjacency )
	{
		OwnershipClosureResult res;

		// `prescanned` is NOT subsumed by `adjacency` even when the latter is
		// given -- the closure listing below needs the DOCUMENT-ORDER vector,
		// which `Adjacency`'s NodeId-keyed maps do not preserve (see the
		// header's note on this parameter).
		std::vector<DocumentChunk> localChunks;
		if( !prescanned ) { localChunks = SceneReferenceGraph::AllChunks( doc ); prescanned = &localChunks; }

		// `snapshot` is ALSO not subsumed by `adjacency`: the nowUnreferenced
		// report further down needs the flat EDGES LIST (a linear scan, not
		// an indexed lookup), which `Adjacency`'s maps do not carry -- built
		// unconditionally here so a caller that passes `adjacency` alone
		// still gets a correct report, at the cost of one `EdgesAndDangling`
		// pass `Adjacency` does not save it from.  A batch caller wanting
		// THAT cost gone too should pass its own `snapshot` alongside
		// `adjacency` (both cheap pointers to what it already built once).
		SceneReferenceGraph::Snapshot localSnap;
		if( !snapshot ) { localSnap = SceneReferenceGraph::EdgesAndDangling( doc, prescanned ); snapshot = &localSnap; }

		// byId/rev/fwd: take the prebuilt bundle when given (P2-2's batch
		// path), else build locally exactly as before.
		Adjacency localAdj;
		if( !adjacency ) {
			localAdj.byId = IndexById( *prescanned );
			localAdj.rev  = BuildReverse( snapshot->edges );
			localAdj.fwd  = BuildForward( snapshot->edges );
			adjacency = &localAdj;
		}
		const ChunkIndex& byId = adjacency->byId;
		const AdjMap&      rev = adjacency->rev;
		const AdjMap&      fwd = adjacency->fwd;

		char buf[1024];

		// ---- resolve the target ------------------------------------------
		const ChunkIndex::const_iterator t = byId.find( intent.targetChunk );
		if( t == byId.end() || !t->second->hasCategory ) {
			res.classification = ClosureClassification::UnresolvedTarget;
			std::snprintf( buf, sizeof( buf ), kClosureUnresolvedFmt, "(unresolved node id)" );
			res.diagnostic = buf;
			return res;
		}
		const DocumentChunk& target = *t->second;

		// ---- sect. 3.7a step 3, first arm: expression-driven --------------
		//
		// Checked BEFORE ownership because it is a property of the chunk
		// itself ("can this chunk be rewritten as a unit AT ALL?"), which
		// logically precedes "and does this edit own it?".  Note the caller
		// contract in OwnershipClosure.h: `SceneEditController::
		// RewireConnection` runs ConnectionLegality FIRST, so a rewire aimed
		// at an expression chunk's non-reference param (`expr` itself) is
		// refused as an illegal connection before ever reaching here; this
		// arm is what would catch a future expression-bodied chunk that DOES
		// declare a reference slot.
		{
			const ChunkDescriptor* d = DescriptorForKeyword( target.keyword );
			if( d && IsExpressionBodied( *d ) ) {
				res.classification = ClosureClassification::ExpressionDrivenTarget;
				std::snprintf( buf, sizeof( buf ), kClosureExpressionFmt,
					DisplayName( target ).c_str(), target.keyword.c_str() );
				res.diagnostic = std::string( buf ) + kClosureDuplicateHatch;
				return res;
			}
		}

		// ---- sect. 3.7a step 1: the ownership closure ---------------------
		const std::vector<Cst::NodeId> owners = OwnersWalk( intent.targetChunk, byId, rev );
		for( size_t i = 0; i < owners.size(); ++i ) {
			const ChunkIndex::const_iterator o = byId.find( owners[i] );
			res.owners.push_back( String( ( o != byId.end() ? DisplayName( *o->second )
			                                                : std::string( "<unknown chunk>" ) ).c_str() ) );
		}

		// ---- sect. 3.7a step 3, second arm: shared ------------------------
		if( owners.size() >= 2 ) {
			res.classification = ClosureClassification::SharedTarget;
			res.sharedChunks.push_back( String( DisplayName( target ).c_str() ) );
			// (b) the OUT-OF-CLOSURE referrers: every chunk that references
			// the target.  With >= 2 owners the target has no single owning
			// closure, so by construction EVERY referrer is out of it --
			// listing them all is the honest answer, and it is what a user
			// needs to see to pick which graph to fork.
			const AdjMap::const_iterator r = rev.find( intent.targetChunk );
			if( r != rev.end() ) {
				for( std::set<Cst::NodeId>::const_iterator p = r->second.begin(); p != r->second.end(); ++p ) {
					const ChunkIndex::const_iterator c = byId.find( *p );
					res.outOfClosureReferrers.push_back(
						String( ( c != byId.end() ? DisplayName( *c->second )
						                          : std::string( "<unknown chunk>" ) ).c_str() ) );
				}
			}
			std::snprintf( buf, sizeof( buf ), kClosureSharedFmt,
				DisplayName( target ).c_str(),
				JoinNames( res.owners ).c_str(),
				JoinNames( res.outOfClosureReferrers ).c_str() );
			res.diagnostic = std::string( buf ) + kClosureDuplicateHatch;
			return res;
		}

		// owners.size() == 1 here: OwnersWalk only ever returns 0 entries for
		// an unresolved/description-less chunk, which the guard above already
		// rejected.  Defensive, not expected.
		if( owners.empty() ) {
			res.classification = ClosureClassification::UnresolvedTarget;
			std::snprintf( buf, sizeof( buf ), kClosureUnresolvedFmt, DisplayName( target ).c_str() );
			res.diagnostic = buf;
			return res;
		}

		// ---- sect. 3.7a step 2: what the edit MAY rewrite ------------------
		//
		// The closure = the target plus every chunk transitively reachable
		// FROM it whose owner set is identical to the target's (i.e. nothing
		// outside this graph reaches it).  A reachable chunk with a DIFFERENT
		// or LARGER owner set is a shared/linked node: it is excluded, and
		// the walk does not descend through it (anything only reachable via a
		// shared node is not solely owned by this graph either).
		{
			const Cst::NodeId ownerId = owners[0];
			std::set<Cst::NodeId> inClosure, seen;
			std::vector<Cst::NodeId> work;
			work.push_back( intent.targetChunk );
			seen.insert( intent.targetChunk );
			inClosure.insert( intent.targetChunk );

			while( !work.empty() ) {
				const Cst::NodeId cur = work.back();
				work.pop_back();
				const AdjMap::const_iterator f = fwd.find( cur );
				if( f == fwd.end() ) continue;
				for( std::set<Cst::NodeId>::const_iterator k = f->second.begin(); k != f->second.end(); ++k ) {
					if( !seen.insert( *k ).second ) continue;   // also the cycle guard
					const std::vector<Cst::NodeId> kidOwners = OwnersWalk( *k, byId, rev );
					if( kidOwners.size() != 1 || kidOwners[0] != ownerId ) continue;   // shared: excluded, not descended
					inClosure.insert( *k );
					work.push_back( *k );
				}
			}

			// Document order, which `*prescanned` already is.
			for( size_t i = 0; i < prescanned->size(); ++i ) {
				const DocumentChunk& c = (*prescanned)[i];
				if( inClosure.find( c.id ) == inClosure.end() ) continue;
				ClosureChunk cc;
				cc.id       = c.id;
				cc.name     = c.name;
				cc.keyword  = c.keyword;
				cc.category = c.category;
				res.closure.push_back( cc );
			}
		}

		// ---- the nowUnreferenced report (for the canvas badge) ------------
		//
		// A rewire/detach removes EXACTLY ONE reference edge: the one this
		// (chunk, param, occurrence) currently writes.  So at most one chunk
		// can drop to zero referrers -- the old referent, and only when this
		// edited slot was its last one.  Re-pointing the slot at the SAME
		// chunk it already names removes nothing.
		{
			Cst::NodeId oldRef = 0;
			for( size_t i = 0; i < snapshot->edges.size(); ++i ) {
				const ReferenceEdge& e = snapshot->edges[i];
				if( e.referrerId == intent.targetChunk
				 && e.paramName.size() > 1
				 && std::string( e.paramName.c_str() ) == intent.paramName
				 && e.occurrence == intent.occurrence )
				{ oldRef = e.targetId; break; }
			}
			const bool stillBound = ( intent.kind == TopologyEditKind::Rewire && oldRef != 0
			                          && oldRef == intent.newRefChunk );
			if( oldRef != 0 && !stillBound ) {
				// Referrer CHUNKS, not edges -- but count EDGES here, because
				// what disappears is one edge: a referrer that names `oldRef`
				// from two of its own slots still names it after this edit.
				int remainingEdges = 0;
				for( size_t i = 0; i < snapshot->edges.size(); ++i ) {
					const ReferenceEdge& e = snapshot->edges[i];
					if( e.targetId != oldRef ) continue;
					const bool isTheEditedSlot =
						( e.referrerId == intent.targetChunk
						  && e.paramName.size() > 1
						  && std::string( e.paramName.c_str() ) == intent.paramName
						  && e.occurrence == intent.occurrence );
					if( !isTheEditedSlot ) ++remainingEdges;
				}
				if( remainingEdges == 0 ) {
					const ChunkIndex::const_iterator o = byId.find( oldRef );
					if( o != byId.end() )
						res.nowUnreferenced.push_back( String( DisplayName( *o->second ).c_str() ) );
				}
			}
		}

		res.classification = ClosureClassification::Clean;
		return res;
	}
}
