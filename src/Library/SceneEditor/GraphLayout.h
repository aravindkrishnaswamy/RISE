//////////////////////////////////////////////////////////////////////
//
//  GraphLayout.h - Rank-by-dependency auto-layout for the Painter/
//    Material node graph (doc-88 Phase 3 S12,
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 4 + sect. 6 S12 entry).
//
//    Pure headless C++, zero UI deps: a function of a
//    `SceneEditController::PainterMaterialGraph` snapshot (S11) plus a
//    caller-supplied set of already-known ("saved") positions, producing
//    a full position for every node.  No controller, no lock, no Job --
//    directly unit-testable against a synthetic graph, same posture as
//    `SceneEditController::BuildPainterMaterialGraph` itself.
//
//    ALGORITHM (design doc sect. 4 option B, "simple rank-by-dependency
//    columns" -- chosen over a full Sugiyama crossing-minimizer because
//    RISE's painter graphs are shallow by construction, G6's "few,
//    powerful chunks" discipline; and over force-directed because that
//    option is explicitly unstable across edits, which is disqualifying
//    for a sidecar-persisted, user-tweakable layout):
//
//      RANK.  rank(node) = 0 when node has no valid out-edge (a "leaf"
//      dependency -- e.g. a base `uniformcolor_painter` with nothing to
//      reference); otherwise rank(node) = 1 + max(rank(dep)) over every
//      node it references (`GraphNode::outEdges` entries with
//      `otherNode != kInvalidNodeIndex` -- a dangling or out-of-scope
//      port never contributes to rank, see `GraphPort::otherNode`'s own
//      comment). Column x = rank * columnSpacing.
//
//      Computed by a Kahn's-algorithm walk over `outEdges`/`inEdges`
//      (ComputeRanks, below) -- NOT recursive DFS.  This is a
//      deliberate choice, not just a style preference: a plain
//      memoized-DFS rank walk recurses to a depth bounded only by the
//      graph's longest dependency chain, which is fine for RISE's
//      typical shallow chains but is an unbounded stack-depth hazard
//      for a HOSTILE synthetic input (a test is explicitly allowed to
//      hand this function a long chain or a cycle -- see
//      `BuildPainterMaterialGraph`'s own "only layer a test may hand a
//      hostile input to" framing, which this function inherits).
//      Kahn's algorithm processes each node's OUT-degree-in-scope
//      countdown exactly once and only ever dequeues a node once its
//      full dependency set already has a finalized rank -- so the total
//      work is O(nodes + edges) with NO recursion and no call-stack
//      growth at all, regardless of chain depth.
//
//      HOSTILE-CYCLE TERMINATION.  A real Document can never contain a
//      chunk-level reference cycle post-derive (the parser resolves a
//      reference to an EXISTING named chunk; nothing lets a chunk name
//      itself into existence before it is declared), but S11's own
//      assembler is documented to tolerate a crafted synthetic cycle
//      rather than assume the input is always well-formed, and this
//      layer inherits that obligation. Kahn's algorithm is naturally
//      cycle-safe: a node whose dependency set includes a cycle member
//      can never have its out-degree countdown reach zero (each member
//      of the cycle is mutually waiting on another member of the same
//      cycle), so it is simply never dequeued by the main loop -- no
//      infinite loop, no unbounded recursion, just a node the loop
//      never reaches. `ComputeRanks` sweeps every node NOT reached by
//      the main loop in one final bounded pass and assigns it rank 0 --
//      documented in `ComputeRanks`'s own comment as the "hard
//      fallback", never exercised on the guaranteed-acyclic path a real
//      Document produces. This is the "cap" the design brief asks for:
//      the cap is structural (Kahn's algorithm dequeues each node at
//      most once, so the main loop is bounded by `nodes.size()`
//      iterations by construction), not an arbitrary iteration-count
//      constant -- see docs/skills/precision-fix-the-formulation.md's
//      standing guidance against magic thresholds where the algorithm
//      itself can be made to terminate.
//
//      ROW (within a rank).  A node ABSENT from `savedPositions` gets
//      the next free integer row slot in its rank's column, searched
//      upward from 0 and skipping any row already taken -- by a SAVED
//      node whose own rank happens to land in this column, or by an
//      earlier-processed un-positioned sibling in the SAME rank.
//      Un-positioned nodes are visited in (rank, GraphNode::order, name)
//      order -- `order` is the node's own CST declaration-order key,
//      already carried on `GraphNode` by S11 (`GraphNode::order`, "same
//      role as TreeNodeSeed::order"); no new ordering field was needed.
//      Row y = rowSlot * rowSpacing.
//
//      STABILITY CONTRACT for "adding one node changes only that node's
//      assigned position" (design brief sect. 6 S12): this holds
//      UNCONDITIONALLY for a node whose `order` is higher than every
//      other un-positioned node already occupying its rank -- the
//      normal, in fact ONLY, real-world case, since `order` is a
//      document-wide monotonic counter assigned by walking the CST in
//      document order (`BuildPainterMaterialGraphSeedsLocked_`) and a
//      freshly created chunk is always APPENDED to the document, never
//      inserted before an existing one. A synthetic test that inserts a
//      new seed with a LOWER `order` than existing un-positioned
//      siblings in the same rank can observe those siblings' row shift
//      (their "next free slot" search now finds the new node's row
//      taken ahead of them) -- this is documented behavior, not a bug,
//      and mirrors the same "least surprising degradation under a
//      contract violation" posture `BuildPainterMaterialGraph`'s own
//      duplicate-id-seed handling documents.
//
//      A node's SAVED position (present in `savedPositions`, keyed by
//      `GraphNode::name`) is echoed back VERBATIM and never touched by
//      rank/row computation at all -- the "never moves" half of the
//      stability contract.
//
//    NAME-KEYED, NOT HANDLE-KEYED (design doc sect. 1.3): `GraphNode::
//    handle` is generation-tagged and stamped only at PUBLISH time
//    (S11), so it cannot survive a save/reload round-trip the way a
//    sidecar-persisted position must. `GraphNode::name` is the
//    identity the S13 sidecar keys on too -- see GraphLayoutSidecar.h.
//    This inherits the SAME documented coarseness `SceneReferenceGraph::
//    ResolveChunk` already lives with: two chunks of the SAME category
//    can legally share a name (a colour painter and a `scalar_painter`
//    both named "P"), and a name->position map cannot distinguish them
//    -- one entry, whichever was written last, silently stands in for
//    both. Not fixed here; the same "document, don't silently
//    second-guess" posture ReferenceGraph.h's own header comment takes
//    for the identical ambiguity.
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
		//! Grid spacing. Defaulted to values a canvas widget can use as-is;
		//! a caller with its own visual scale can override either.
		//!
		//! The default constructor is USER-PROVIDED (an initializer list,
		//! not `= 220.0`-style default member initializers) deliberately:
		//! `LayoutGraph`'s own default argument below (`const Config&
		//! config = Config()`) default-constructs THIS nested type from a
		//! member-function declaration of the ENCLOSING class
		//! `GraphLayout` -- with plain NSDMI members, Clang rejects that
		//! ("default member initializer for 'columnSpacing' needed within
		//! definition of enclosing class 'GraphLayout' outside of member
		//! functions"), because evaluating the implicit default
		//! constructor's exception specification for a default ARGUMENT is
		//! not treated as "inside a member function" the way a member
		//! function BODY is. A user-provided constructor sidesteps the
		//! quirk entirely -- no implicit special member function, no NSDMI
		//! evaluation-context question.
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

		//! One rank per `graph.nodes[i]`, parallel-indexed (ranks[i] is
		//! nodes[i]'s rank). Exposed directly (not just as a LayoutGraph
		//! implementation detail) so a test can drive the hostile-cycle
		//! termination case without also exercising row/column assignment --
		//! see the file header's "HOSTILE-CYCLE TERMINATION" section.
		static std::vector<unsigned int> ComputeRanks(
			const SceneEditController::PainterMaterialGraph& graph );

		//! The full layout: every node in `graph.nodes` gets an entry in the
		//! returned Positions map. A node named in `savedPositions` is
		//! echoed back UNCHANGED (see the file header's stability contract);
		//! every other node's position is computed from its rank (column)
		//! and its "next free row slot" (row) -- see the file header.
		//!
		//! `graph.nodes` entries with an empty `name` (should not occur --
		//! `BuildPainterMaterialGraphSeedsLocked_` never seeds an unnamed
		//! chunk as a Painter/Material node, see its own `c.name.size() <= 1`
		//! guard -- but this function makes no assumption a hostile
		//! synthetic input could violate) are simply skipped: an empty name
		//! cannot be a Positions map key any more than it can be a
		//! `savedPositions` lookup key.
		static Positions LayoutGraph(
			const SceneEditController::PainterMaterialGraph& graph,
			const Positions& savedPositions,
			const Config& config = Config() );
	};
}

#endif
