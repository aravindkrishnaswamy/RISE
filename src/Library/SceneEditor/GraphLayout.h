//////////////////////////////////////////////////////////////////////
//
//  GraphLayout.h - Headless rank-by-dependency auto-layout for the
//  Painter/Material node graph (docs/gui/NODE_GRAPH_CANVAS.md §4).
//
//  Algorithm & invariants:
//  - Rank: 0 for leaves (no valid in-scope outEdges with
//    otherNode != kInvalidNodeIndex), else 1 + max(rank(dep)). Computed
//    iteratively via Kahn's algorithm in O(V+E) with no recursion; any
//    hostile cycle member not dequeued falls back to rank 0.
//    Column x = rank * columnSpacing.
//  - Row: unpositioned nodes are visited in (rank, GraphNode::order, name)
//    order and assigned the lowest free integer row slot in their rank
//    column (skipping saved and previously placed nodes); y = row * rowSpacing.
//  - Stability: savedPositions (keyed by GraphNode::name, matching
//    GraphLayoutSidecar.h; inherits same-category duplicate-name coarseness)
//    are echoed verbatim. Appending a new node with higher `order` leaves
//    existing positions unchanged.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_GRAPHLAYOUT_
#define RISE_GRAPHLAYOUT_

#include "SceneEditController.h"

#include <map>
#include <string>
#include <vector>

namespace RISE
{
	//! One node's 2D position in the canvas' own coordinate space.
	struct GraphLayoutPoint
	{
		double x = 0.0;
		double y = 0.0;
	};

	//! Pure, static, headless -- see the file header for the full algorithm.
	class GraphLayout
	{
	public:
		//! Grid spacing. Uses a user-provided default ctor (not NSDMIs) so
		//! Clang accepts `const Config& config = Config()` inside the enclosing
		//! class definition.
		struct Config
		{
			double columnSpacing;   //!< x-distance between adjacent ranks
			double rowSpacing;      //!< y-distance between adjacent row slots
			Config() : columnSpacing( 220.0 ), rowSpacing( 140.0 ) {}
		};

		//! Keyed by `GraphNode::name` (converted with `std::string(name.c_str())`,
		//! the codebase's standing RISE::String -> std::string convention --
		//! see SceneEditController.cpp's own `byName` maps for precedent).
		//! Same type serves as both the caller's "what's already known" input
		//! and this function's "everything, known or computed" output.
		typedef std::map<std::string, GraphLayoutPoint> Positions;

		//! Parallel-indexed ranks for `graph.nodes`; exposed for direct
		//! cycle-termination unit testing.
		static std::vector<unsigned int> ComputeRanks(
			const SceneEditController::PainterMaterialGraph& graph );

		//! Computes full layout for `graph.nodes`. Entries present in
		//! `savedPositions` are echoed unchanged; unpositioned nodes get
		//! rank/row positions, and empty-named nodes are skipped.
		static Positions LayoutGraph(
			const SceneEditController::PainterMaterialGraph& graph,
			const Positions& savedPositions,
			const Config& config = Config() );
	};
}

#endif
