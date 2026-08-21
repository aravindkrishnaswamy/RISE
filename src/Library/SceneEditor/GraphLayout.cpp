//////////////////////////////////////////////////////////////////////
//
//  GraphLayout.cpp - see GraphLayout.h for the full algorithm writeup.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GraphLayout.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace RISE
{

std::vector<unsigned int> GraphLayout::ComputeRanks(
	const SceneEditController::PainterMaterialGraph& graph )
{
	const std::size_t n = graph.nodes.size();
	std::vector<unsigned int> ranks( n, 0 );
	if( n == 0 ) return ranks;

	// remaining[i] -- how many of node i's OWN out-edges still point at an
	// in-graph node (otherNode != kInvalidNodeIndex) whose rank is not yet
	// finalized. A dangling/out-of-scope port (otherNode ==
	// kInvalidNodeIndex) never contributes -- see GraphPort::otherNode's
	// own comment in SceneEditController.h; it simply never counts here.
	std::vector<unsigned int> remaining( n, 0 );
	for( std::size_t i = 0; i < n; ++i ) {
		unsigned int deps = 0;
		for( const SceneEditController::GraphPort& port : graph.nodes[i].outEdges )
			if( port.otherNode != SceneEditController::kInvalidNodeIndex ) ++deps;
		remaining[i] = deps;
	}

	std::vector<bool> finalized( n, false );
	std::vector<unsigned int> ready;
	ready.reserve( n );
	for( std::size_t i = 0; i < n; ++i )
		if( remaining[i] == 0 ) ready.push_back( static_cast<unsigned int>( i ) );

	// Kahn's main loop. `ready.size()` is RE-READ every iteration (the same
	// growing-worklist idiom ExpandFunctionPromotionFrontier uses), so a
	// node THIS pass finalizes and enqueues gets walked in the same call.
	// Each node index can be pushed onto `ready` at most once (the
	// `remaining[j] == 0` push only fires the instant remaining[j]
	// transitions from 1 to 0 -- see below), so this loop is bounded by
	// `n` iterations total no matter how the edges are wired. THAT is the
	// structural "cap" GraphLayout.h's header documents: not an arbitrary
	// constant, but a property of the algorithm itself.
	for( std::size_t qi = 0; qi < ready.size(); ++qi ) {
		const unsigned int i = ready[qi];
		finalized[i] = true;   // ranks[i] is already final: 0 (no deps) or the running max below

		for( const SceneEditController::GraphPort& port : graph.nodes[i].inEdges ) {
			const unsigned int j = port.otherNode;
			// otherNode is validated against THIS SAME graph's node count by
			// construction (BuildPainterMaterialGraph only ever mirrors an
			// outEdges row it just resolved against `byId`, see its own
			// "MIRROR onto the target's inEdges" comment) -- the >= n guard
			// is defensive against a hand-built synthetic GraphNode a test
			// assembles directly (bypassing that assembler) with an
			// out-of-range inEdges row, never expected on a real graph.
			if( j == SceneEditController::kInvalidNodeIndex || j >= n ) continue;
			if( finalized[j] ) continue;   // e.g. a self-reference already finalized in this same pass

			const unsigned int candidate = ranks[i] + 1;
			if( candidate > ranks[j] ) ranks[j] = candidate;

			if( remaining[j] > 0 ) {
				--remaining[j];
				if( remaining[j] == 0 ) ready.push_back( j );
			}
		}
	}

	// HARD FALLBACK: a node the main loop never reached -- a member of a
	// hostile synthetic cycle (its `remaining` count can never reach 0,
	// since every member is mutually waiting on another member of the same
	// cycle), or a node transitively downstream of one. Grouped at rank 0
	// rather than left at whatever partial max it accumulated before its
	// own dependents stalled -- deterministic, and never exercised on the
	// guaranteed-acyclic path a real Document produces (see GraphLayout.h's
	// "HOSTILE-CYCLE TERMINATION" section).
	for( std::size_t i = 0; i < n; ++i )
		if( !finalized[i] ) ranks[i] = 0;

	return ranks;
}

namespace {

//! The integer row-slot a saved y-coordinate falls into, for seeding a
//! rank's "used" set so an auto-assigned sibling does not visually land
//! on top of it. `rowSpacing <= 0` is a degenerate Config a caller should
//! not pass (see Config's own doc), but this is cheap to guard: falls
//! back to "no correlation with saved rows" rather than a divide-by-zero.
long long SavedRowSlot( double y, double rowSpacing )
{
	if( rowSpacing <= 0.0 ) return 0;
	return static_cast<long long>( std::llround( y / rowSpacing ) );
}

} // anonymous namespace

GraphLayout::Positions GraphLayout::LayoutGraph(
	const SceneEditController::PainterMaterialGraph& graph,
	const Positions& savedPositions,
	const Config& config )
{
	Positions out;
	const std::size_t n = graph.nodes.size();
	if( n == 0 ) return out;

	const std::vector<unsigned int> ranks = ComputeRanks( graph );

	// Pass 1: echo every SAVED node's position back VERBATIM (the "never
	// moves" half of the stability contract), and seed each rank's
	// used-row set from the saved nodes that land in it -- so pass 2's
	// "next free row slot" search skips rows a saved node already
	// occupies. A node whose name collides with another node's name (the
	// documented same-category-same-name ambiguity, see GraphLayout.h's
	// header) is treated as saved by this same lookup either way -- this
	// is what guarantees an auto-assigned node can never OVERWRITE a
	// saved entry in `out`, even under that ambiguity: any node whose name
	// resolves in `savedPositions` is excluded from pass 2's candidate
	// list below, full stop.
	std::map<unsigned int, std::set<long long>> usedRowsByRank;
	std::vector<std::size_t> toAssign;
	toAssign.reserve( n );
	for( std::size_t i = 0; i < n; ++i ) {
		const std::string name( graph.nodes[i].name.c_str() );
		if( name.empty() ) continue;   // unaddressable node: see LayoutGraph's own header note

		const Positions::const_iterator saved = savedPositions.find( name );
		if( saved != savedPositions.end() ) {
			out[name] = saved->second;
			usedRowsByRank[ ranks[i] ].insert( SavedRowSlot( saved->second.y, config.rowSpacing ) );
			continue;
		}
		toAssign.push_back( i );
	}

	// Pass 2: un-positioned nodes, visited in (rank, order, name) order --
	// see GraphLayout.h's "ROW (within a rank)" + "STABILITY CONTRACT"
	// sections for why this ordering is what makes an APPENDED new node
	// (the only real-world insertion shape: a fresh CST chunk always lands
	// at the end of the document, hence the highest `order`) never disturb
	// an existing sibling's row.
	std::stable_sort( toAssign.begin(), toAssign.end(),
		[&graph, &ranks]( std::size_t a, std::size_t b ) {
			if( ranks[a] != ranks[b] ) return ranks[a] < ranks[b];
			if( graph.nodes[a].order != graph.nodes[b].order ) return graph.nodes[a].order < graph.nodes[b].order;
			return std::string( graph.nodes[a].name.c_str() ) < std::string( graph.nodes[b].name.c_str() );
		} );

	for( std::size_t i : toAssign ) {
		const unsigned int rank = ranks[i];
		std::set<long long>& used = usedRowsByRank[rank];
		long long row = 0;
		while( used.find( row ) != used.end() ) ++row;
		used.insert( row );

		GraphLayoutPoint p;
		p.x = static_cast<double>( rank ) * config.columnSpacing;
		p.y = static_cast<double>( row )  * config.rowSpacing;
		out[ std::string( graph.nodes[i].name.c_str() ) ] = p;
	}

	return out;
}

} // namespace RISE
