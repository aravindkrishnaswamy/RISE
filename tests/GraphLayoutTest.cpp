//////////////////////////////////////////////////////////////////////
//
//  GraphLayoutTest.cpp - doc-88 Phase 3 S12:
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 6's "S12 -- rank-by-dependency
//    layout core" (src/Library/SceneEditor/GraphLayout.{h,cpp}).
//
//  Drives `GraphLayout::ComputeRanks`/`LayoutGraph` directly on
//  synthetic `SceneEditController::PainterMaterialGraph` instances,
//  built the same way `ReferenceGraphTest.cpp` PART 2 builds them: via
//  `SceneEditController::BuildPainterMaterialGraph` over hand-crafted
//  `GraphNodeSeed`/`GraphEdgeSeed` vectors -- the "only layer a test may
//  hand a hostile input to" posture that assembler's own header
//  documents, which `GraphLayout` inherits (see GraphLayout.h's own
//  "HOSTILE-CYCLE TERMINATION" section). No UI, no controller, no Job.
//
//  Parts:
//    PART 1 -- golden rank/layout shapes named in the design brief:
//      linear chain, diamond fan-in (shared node), wide flat rank,
//      multi-root (two independent roots feeding one consumer).
//    PART 2 -- "longest path from any root", not "one more than the
//      first edge seen": a node with both a direct edge to a leaf AND
//      a transitive edge through an intermediate must rank past the
//      LONGER of the two paths.
//    PART 3 -- declaration-order tiebreak pinned: within a rank, row
//      order follows GraphNode::order, not alphabetical name, even
//      when the two disagree.
//    PART 4 -- the four STABILITY CONTRACTS from the design brief,
//      each its own case:
//        4a. a node present in SavedPositions never moves.
//        4b. un-positioned nodes never displace positioned ones
//            (collision policy: next free row slot skips a saved row).
//        4c. re-running layout with identical inputs is byte-deterministic.
//        4d. adding one (appended, i.e. highest-order) node changes only
//            that node's assigned position.
//        4e. the DOCUMENTED non-contract corollary of 4d: inserting a
//            new un-positioned node with an order BETWEEN two existing
//            un-positioned siblings in the same rank can shift the
//            later sibling's row -- GraphLayout.h's own "STABILITY
//            CONTRACT" section documents this as expected, not a bug;
//            pinned here so a future change cannot silently alter it
//            without a reviewer noticing the assertion move.
//    PART 5 -- hostile inputs: a crafted 2-node cycle, a node
//      transitively downstream of a cycle, and a self-reference, none
//      of which may hang ComputeRanks or LayoutGraph (finishing this
//      test file IS the termination proof); dangling ports (toId == 0)
//      never contribute to rank.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/GraphLayout.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}
static void CheckDoubleEq( double got, double want, const char* n )
{
	// Every value compared here is the product of small exact integers
	// (a rank or row slot, both small non-negative ints) times a
	// caller-chosen spacing constant -- plain IEEE double multiplication
	// of exact values, no accumulated error across differing code paths.
	// A strict == is therefore the RIGHT check, not an epsilon-fudge --
	// see docs/skills/precision-fix-the-formulation.md's standing
	// guidance against papering over with a tolerance where the
	// arithmetic is actually exact.
	if( got == want ) { ++passCount; return; }
	++failCount;
	std::cout << "  FAIL: " << n << "\n    got  : " << got << "\n    want : " << want << std::endl;
}

// ---------------------------------------------------------------------
// Helpers -- mirrors ReferenceGraphTest.cpp PART 2's own MakeNodeSeed/
// MakeEdgeSeed (test files in this repo are self-contained; no shared
// test-support header exists to import them from).
// ---------------------------------------------------------------------

typedef SceneEditController::GraphNodeSeed NodeSeed;
typedef SceneEditController::GraphEdgeSeed EdgeSeed;
typedef SceneEditController::PainterMaterialGraph Graph;

static NodeSeed MakeNodeSeed( Cst::NodeId id, const char* name, unsigned long long order,
                               ChunkCategory cat = ChunkCategory::Painter )
{
	NodeSeed s;
	s.id       = id;
	s.name     = String( name );
	s.category = cat;
	s.order    = order;
	return s;
}

//! `toId == 0` produces a DANGLING port (see BuildPainterMaterialGraph's
//! own contract) -- PART 5's dangling-port case passes 0 deliberately.
//! Both category args default to Painter (the common case in this
//! file's cases: Painter -> Painter); the diamond fan-in case passes
//! Material explicitly for `fromCat` since the assembler matches
//! purely by NodeId (`e.toId`), not by category -- see
//! BuildPainterMaterialGraph's own "MIRROR onto the target's inEdges"
//! code, which never reads GraphEdgeSeed::fromCategory/toCategory to
//! decide connectivity -- but passing the correct category keeps this
//! file's synthetic seeds honest about what a real seeding pass would
//! produce.
static EdgeSeed MakeEdgeSeed( Cst::NodeId fromId, const char* fromName, const char* param,
                               Cst::NodeId toId, const char* toName,
                               ChunkCategory fromCat = ChunkCategory::Painter,
                               ChunkCategory toCat = ChunkCategory::Painter )
{
	EdgeSeed e;
	e.fromId       = fromId;
	e.fromCategory = fromCat;
	e.fromName     = String( fromName );
	e.paramName    = String( param );
	e.occurrence   = 0;
	e.toId         = toId;
	e.toCategory   = toCat;
	e.toName       = String( toName );
	e.portCategories.push_back( toCat );
	return e;
}

static const GraphLayoutPoint* FindPos( const GraphLayout::Positions& pos, const std::string& name )
{
	const GraphLayout::Positions::const_iterator it = pos.find( name );
	return it == pos.end() ? nullptr : &it->second;
}

static GraphLayout::Config TestConfig()
{
	GraphLayout::Config cfg;
	cfg.columnSpacing = 100.0;
	cfg.rowSpacing    = 50.0;
	return cfg;
}

// =======================================================================
// PART 1 -- golden shapes
// =======================================================================

static void TestLinearChain()
{
	std::cout << "PART 1a: linear chain (A -> B -> C)" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 3, "C", 0 ) );
	nodes.push_back( MakeNodeSeed( 2, "B", 1 ) );
	nodes.push_back( MakeNodeSeed( 1, "A", 2 ) );
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 2, "B", "ref", 3, "C" ) );   // B -> C
	edges.push_back( MakeEdgeSeed( 1, "A", "ref", 2, "B" ) );   // A -> B

	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
	const std::vector<unsigned int> ranks = GraphLayout::ComputeRanks( g );
	Check( ranks.size() == 3, "1a: one rank entry per node" );

	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* a = FindPos( pos, "A" );
	const GraphLayoutPoint* b = FindPos( pos, "B" );
	const GraphLayoutPoint* c = FindPos( pos, "C" );
	Check( a && b && c, "1a: all three nodes positioned" );
	if( a && b && c ) {
		CheckDoubleEq( c->x, 0.0,   "1a: C (leaf) at column 0" );
		CheckDoubleEq( b->x, 100.0, "1a: B at column 1" );
		CheckDoubleEq( a->x, 200.0, "1a: A at column 2" );
		// each rank here has exactly one node -- row 0 in every column
		CheckDoubleEq( c->y, 0.0, "1a: C row 0" );
		CheckDoubleEq( b->y, 0.0, "1a: B row 0" );
		CheckDoubleEq( a->y, 0.0, "1a: A row 0" );
	}
}

static void TestDiamondFanIn()
{
	std::cout << "PART 1b: diamond fan-in (M1 -> P, M2 -> P, shared node)" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "P",  0, ChunkCategory::Painter ) );
	nodes.push_back( MakeNodeSeed( 2, "M1", 1, ChunkCategory::Material ) );
	nodes.push_back( MakeNodeSeed( 3, "M2", 2, ChunkCategory::Material ) );
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 2, "M1", "reflectance", 1, "P", ChunkCategory::Material, ChunkCategory::Painter ) );
	edges.push_back( MakeEdgeSeed( 3, "M2", "reflectance", 1, "P", ChunkCategory::Material, ChunkCategory::Painter ) );

	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
	// P should carry both fan-out edges as inEdges -- the "one node, two
	// referrers" contract S11 already pins; spot-checked here because
	// LayoutGraph's rank walk depends on it directly.
	for( const SceneEditController::GraphNode& n : g.nodes )
		if( std::string( n.name.c_str() ) == "P" )
			Check( n.inEdges.size() == 2, "1b: P has two inEdges (shared, not duplicated)" );

	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* p  = FindPos( pos, "P" );
	const GraphLayoutPoint* m1 = FindPos( pos, "M1" );
	const GraphLayoutPoint* m2 = FindPos( pos, "M2" );
	Check( p && m1 && m2, "1b: all three positioned" );
	if( p && m1 && m2 ) {
		CheckDoubleEq( p->x, 0.0,   "1b: P at column 0" );
		CheckDoubleEq( m1->x, 100.0, "1b: M1 at column 1" );
		CheckDoubleEq( m2->x, 100.0, "1b: M2 at column 1" );
		// M1 (order 1) before M2 (order 2): distinct rows, M1 first
		Check( m1->y != m2->y, "1b: M1 and M2 occupy distinct rows (no overlap)" );
		CheckDoubleEq( m1->y, 0.0,  "1b: M1 row 0 (lower order)" );
		CheckDoubleEq( m2->y, 50.0, "1b: M2 row 1 (higher order)" );
	}
}

static void TestWideFlatRank()
{
	std::cout << "PART 1c: wide flat rank (three independent leaves)" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "X", 0 ) );
	nodes.push_back( MakeNodeSeed( 2, "Y", 1 ) );
	nodes.push_back( MakeNodeSeed( 3, "Z", 2 ) );
	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );

	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* x = FindPos( pos, "X" );
	const GraphLayoutPoint* y = FindPos( pos, "Y" );
	const GraphLayoutPoint* z = FindPos( pos, "Z" );
	Check( x && y && z, "1c: all three positioned" );
	if( x && y && z ) {
		CheckDoubleEq( x->x, 0.0, "1c: X column 0 (all rank 0)" );
		CheckDoubleEq( y->x, 0.0, "1c: Y column 0 (all rank 0)" );
		CheckDoubleEq( z->x, 0.0, "1c: Z column 0 (all rank 0)" );
		CheckDoubleEq( x->y, 0.0,  "1c: X row 0 (order 0)" );
		CheckDoubleEq( y->y, 50.0, "1c: Y row 1 (order 1)" );
		CheckDoubleEq( z->y, 100.0, "1c: Z row 2 (order 2)" );
	}
}

static void TestMultiRoot()
{
	std::cout << "PART 1d: multi-root (R1, R2 independent roots -> one consumer)" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "R1", 0 ) );
	nodes.push_back( MakeNodeSeed( 2, "R2", 1 ) );
	nodes.push_back( MakeNodeSeed( 3, "Consumer", 2 ) );
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 3, "Consumer", "a", 1, "R1" ) );
	edges.push_back( MakeEdgeSeed( 3, "Consumer", "b", 2, "R2" ) );

	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* r1 = FindPos( pos, "R1" );
	const GraphLayoutPoint* r2 = FindPos( pos, "R2" );
	const GraphLayoutPoint* c  = FindPos( pos, "Consumer" );
	Check( r1 && r2 && c, "1d: all three positioned" );
	if( r1 && r2 && c ) {
		CheckDoubleEq( r1->x, 0.0,   "1d: R1 (root) column 0" );
		CheckDoubleEq( r2->x, 0.0,   "1d: R2 (root) column 0" );
		CheckDoubleEq( c->x,  100.0, "1d: Consumer column 1 (two roots, not two columns)" );
	}
}

// =======================================================================
// PART 2 -- longest path from any root, not "one more than any edge"
// =======================================================================

static void TestLongestPath()
{
	std::cout << "PART 2: Top -> L1 direct AND Top -> Mid -> L1: rank(Top) uses the LONGER path" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "L1",  0 ) );
	nodes.push_back( MakeNodeSeed( 2, "Mid", 1 ) );
	nodes.push_back( MakeNodeSeed( 3, "Top", 2 ) );
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 2, "Mid", "a", 1, "L1" ) );    // Mid -> L1
	edges.push_back( MakeEdgeSeed( 3, "Top", "a", 1, "L1" ) );    // Top -> L1  (direct, distance 1)
	edges.push_back( MakeEdgeSeed( 3, "Top", "b", 2, "Mid" ) );   // Top -> Mid (transitive, distance 2)

	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* l1  = FindPos( pos, "L1" );
	const GraphLayoutPoint* mid = FindPos( pos, "Mid" );
	const GraphLayoutPoint* top = FindPos( pos, "Top" );
	Check( l1 && mid && top, "2: all three positioned" );
	if( l1 && mid && top ) {
		CheckDoubleEq( l1->x,  0.0,   "2: L1 column 0" );
		CheckDoubleEq( mid->x, 100.0, "2: Mid column 1" );
		// If rank(Top) were naively "1 + rank of whichever edge is seen
		// first", the direct L1 edge could win and put Top one column
		// too early (column 1, colliding with Mid). The longest-path
		// rule requires column 2.
		CheckDoubleEq( top->x, 200.0, "2: Top column 2 (via the LONGER Mid path, not the direct L1 edge)" );
	}
}

// =======================================================================
// PART 3 -- declaration-order tiebreak, not alphabetical
// =======================================================================

static void TestDeclarationOrderTiebreak()
{
	std::cout << "PART 3: within a rank, row order follows GraphNode::order, not name" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "Zebra", 0 ) );   // declared FIRST (order 0)
	nodes.push_back( MakeNodeSeed( 2, "Apple", 1 ) );   // declared SECOND (order 1), alphabetically first
	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );

	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* zebra = FindPos( pos, "Zebra" );
	const GraphLayoutPoint* apple = FindPos( pos, "Apple" );
	Check( zebra && apple, "3: both positioned" );
	if( zebra && apple ) {
		CheckDoubleEq( zebra->y, 0.0,  "3: Zebra (order 0) gets row 0 despite losing alphabetically" );
		CheckDoubleEq( apple->y, 50.0, "3: Apple (order 1) gets row 1 despite winning alphabetically" );
	}
}

// =======================================================================
// PART 4 -- the four stability contracts
// =======================================================================

static void TestSavedNeverMovesAndCollisionAvoidance()
{
	std::cout << "PART 4a/4b: saved position never moves; auto nodes skip its row" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "X", 0 ) );
	nodes.push_back( MakeNodeSeed( 2, "Y", 1 ) );
	nodes.push_back( MakeNodeSeed( 3, "Z", 2 ) );
	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );

	// Y is SAVED at row 0 (y=0) -- the exact slot X (order 0, processed
	// first among the un-positioned nodes) would otherwise claim.
	GraphLayout::Positions saved;
	saved["Y"] = GraphLayoutPoint{ 42.0, 0.0 };   // deliberately NOT what auto-layout would ever compute for Y (x=42 is off-grid)

	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, saved, TestConfig() );
	const GraphLayoutPoint* x = FindPos( pos, "X" );
	const GraphLayoutPoint* y = FindPos( pos, "Y" );
	const GraphLayoutPoint* z = FindPos( pos, "Z" );
	Check( x && y && z, "4a: all three positioned" );
	if( y ) {
		CheckDoubleEq( y->x, 42.0, "4a: Y's saved x echoed verbatim" );
		CheckDoubleEq( y->y, 0.0,  "4a: Y's saved y echoed verbatim" );
	}
	if( x && z ) {
		// 4b: row 0 is taken (by Y's saved position) -- X and Z, in
		// order, must be pushed to rows 1 and 2, never landing on 0.
		Check( x->y != 0.0, "4b: X does not land on Y's saved row" );
		Check( z->y != 0.0, "4b: Z does not land on Y's saved row" );
		CheckDoubleEq( x->y, 50.0,  "4b: X takes the next free slot (row 1)" );
		CheckDoubleEq( z->y, 100.0, "4b: Z takes the next free slot after that (row 2)" );
	}
}

static void TestByteDeterministic()
{
	std::cout << "PART 4c: identical inputs -> byte-identical output" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 3, "C", 0 ) );
	nodes.push_back( MakeNodeSeed( 2, "B", 1 ) );
	nodes.push_back( MakeNodeSeed( 1, "A", 2 ) );
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 2, "B", "ref", 3, "C" ) );
	edges.push_back( MakeEdgeSeed( 1, "A", "ref", 2, "B" ) );
	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );

	GraphLayout::Positions saved;
	saved["C"] = GraphLayoutPoint{ 7.0, 3.0 };

	const GraphLayout::Positions pos1 = GraphLayout::LayoutGraph( g, saved, TestConfig() );
	const GraphLayout::Positions pos2 = GraphLayout::LayoutGraph( g, saved, TestConfig() );
	Check( pos1.size() == pos2.size(), "4c: same node count across runs" );
	bool allEqual = pos1.size() == pos2.size();
	for( const std::pair<const std::string, GraphLayoutPoint>& kv : pos1 ) {
		const GraphLayout::Positions::const_iterator it2 = pos2.find( kv.first );
		if( it2 == pos2.end() || it2->second.x != kv.second.x || it2->second.y != kv.second.y ) allEqual = false;
	}
	Check( allEqual, "4c: every node's position bit-identical across two runs" );
}

static void TestAddingOneNodeStability()
{
	std::cout << "PART 4d: appending one node changes only that node's position" << std::endl;
	std::vector<NodeSeed> baselineNodes;
	baselineNodes.push_back( MakeNodeSeed( 1, "X", 0 ) );
	baselineNodes.push_back( MakeNodeSeed( 2, "Y", 1 ) );
	const Graph gBaseline = SceneEditController::BuildPainterMaterialGraph( baselineNodes, std::vector<EdgeSeed>() );
	const GraphLayout::Positions baseline = GraphLayout::LayoutGraph( gBaseline, GraphLayout::Positions(), TestConfig() );

	// Append W with the HIGHEST order -- the real-world shape of a
	// freshly created CST chunk (always appended to the document; see
	// GraphLayout.h's own "STABILITY CONTRACT" section).
	std::vector<NodeSeed> grownNodes = baselineNodes;
	grownNodes.push_back( MakeNodeSeed( 3, "W", 2 ) );
	const Graph gGrown = SceneEditController::BuildPainterMaterialGraph( grownNodes, std::vector<EdgeSeed>() );
	const GraphLayout::Positions grown = GraphLayout::LayoutGraph( gGrown, GraphLayout::Positions(), TestConfig() );

	const GraphLayoutPoint* xBase = FindPos( baseline, "X" );
	const GraphLayoutPoint* yBase = FindPos( baseline, "Y" );
	const GraphLayoutPoint* xGrown = FindPos( grown, "X" );
	const GraphLayoutPoint* yGrown = FindPos( grown, "Y" );
	const GraphLayoutPoint* w = FindPos( grown, "W" );
	Check( xBase && yBase && xGrown && yGrown && w, "4d: all positions found in both runs" );
	if( xBase && xGrown ) {
		CheckDoubleEq( xGrown->x, xBase->x, "4d: X's column unchanged after adding W" );
		CheckDoubleEq( xGrown->y, xBase->y, "4d: X's row unchanged after adding W" );
	}
	if( yBase && yGrown ) {
		CheckDoubleEq( yGrown->x, yBase->x, "4d: Y's column unchanged after adding W" );
		CheckDoubleEq( yGrown->y, yBase->y, "4d: Y's row unchanged after adding W" );
	}
	if( w ) {
		CheckDoubleEq( w->y, 100.0, "4d: W (appended, order 2) lands on the next free row" );
	}
}

static void TestInsertBetweenSiblingsDocumentedShift()
{
	std::cout << "PART 4e: DOCUMENTED non-contract -- inserting BETWEEN existing un-positioned "
	              "siblings (a lower order than a later one) can shift the later one's row" << std::endl;
	// Baseline: X(order 0), Y(order 10) -- a gap deliberately left in the
	// order space, exactly as GraphLayout.h's header describes as the
	// one case the "adding one node" contract does NOT cover.
	std::vector<NodeSeed> baselineNodes;
	baselineNodes.push_back( MakeNodeSeed( 1, "X", 0 ) );
	baselineNodes.push_back( MakeNodeSeed( 2, "Y", 10 ) );
	const Graph gBaseline = SceneEditController::BuildPainterMaterialGraph( baselineNodes, std::vector<EdgeSeed>() );
	const GraphLayout::Positions baseline = GraphLayout::LayoutGraph( gBaseline, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* yBase = FindPos( baseline, "Y" );
	Check( yBase != nullptr, "4e: Y positioned in baseline" );
	if( yBase ) CheckDoubleEq( yBase->y, 50.0, "4e: baseline Y at row 1 (only X ahead of it)" );

	// Insert Z with an order BETWEEN X and Y (5) -- not an append.
	std::vector<NodeSeed> grownNodes = baselineNodes;
	grownNodes.push_back( MakeNodeSeed( 3, "Z", 5 ) );
	const Graph gGrown = SceneEditController::BuildPainterMaterialGraph( grownNodes, std::vector<EdgeSeed>() );
	const GraphLayout::Positions grown = GraphLayout::LayoutGraph( gGrown, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* yGrown = FindPos( grown, "Y" );
	const GraphLayoutPoint* zGrown = FindPos( grown, "Z" );
	Check( yGrown && zGrown, "4e: Y and Z both positioned after the mid-order insert" );
	if( zGrown ) CheckDoubleEq( zGrown->y, 50.0, "4e: Z (order 5) takes row 1, between X and Y" );
	if( yGrown ) CheckDoubleEq( yGrown->y, 100.0, "4e: Y's row SHIFTS to 2 -- documented, not a bug (see GraphLayout.h)" );
}

// =======================================================================
// PART 5 -- hostile inputs
// =======================================================================

static void TestHostileCycleTerminates()
{
	std::cout << "PART 5a: a crafted 2-node cycle does not hang ComputeRanks/LayoutGraph" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "CycA", 0 ) );
	nodes.push_back( MakeNodeSeed( 2, "CycB", 1 ) );
	nodes.push_back( MakeNodeSeed( 3, "Downstream", 2 ) );   // depends on a cycle member, transitively unresolvable too
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 1, "CycA", "ref", 2, "CycB" ) );   // A -> B
	edges.push_back( MakeEdgeSeed( 2, "CycB", "ref", 1, "CycA" ) );   // B -> A
	edges.push_back( MakeEdgeSeed( 3, "Downstream", "ref", 1, "CycA" ) );

	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
	// Reaching this line at all (no hang) IS the primary assertion this
	// test exists to make.
	const std::vector<unsigned int> ranks = GraphLayout::ComputeRanks( g );
	Check( ranks.size() == 3, "5a: ComputeRanks returns one entry per node despite the cycle" );
	for( unsigned int r : ranks )
		Check( r == 0, "5a: every cycle member (and its downstream dependent) falls back to rank 0 -- documented hard fallback" );

	// The full LayoutGraph call must also complete and position every node.
	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	Check( pos.size() == 3, "5a: LayoutGraph positions all three nodes despite the cycle" );
}

static void TestSelfReferenceTerminates()
{
	std::cout << "PART 5b: a self-reference does not hang and gets a position" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "Self", 0 ) );
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 1, "Self", "ref", 1, "Self" ) );   // Self -> Self

	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
	Check( g.nodes.size() == 1 && g.nodes[0].outEdges.size() == 1 && g.nodes[0].inEdges.size() == 1,
	       "5b: self-reference produces one outEdges row AND one inEdges row on the SAME node (S11 contract)" );

	const std::vector<unsigned int> ranks = GraphLayout::ComputeRanks( g );
	Check( ranks.size() == 1 && ranks[0] == 0, "5b: self-reference falls back to rank 0, no hang" );
	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	Check( FindPos( pos, "Self" ) != nullptr, "5b: self-referencing node still gets a position" );
}

static void TestDanglingPortIgnoredForRank()
{
	std::cout << "PART 5c: a dangling port (toId == 0) never contributes to rank" << std::endl;
	std::vector<NodeSeed> nodes;
	nodes.push_back( MakeNodeSeed( 1, "Leaf", 0 ) );
	nodes.push_back( MakeNodeSeed( 2, "Referrer", 1 ) );
	std::vector<EdgeSeed> edges;
	edges.push_back( MakeEdgeSeed( 2, "Referrer", "a", 1, "Leaf" ) );        // real dependency -> rank 1
	edges.push_back( MakeEdgeSeed( 2, "Referrer", "b", 0, "ghost" ) );       // dangling: toId == 0

	const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
	bool foundDangling = false;
	for( const SceneEditController::GraphNode& n : g.nodes )
		if( std::string( n.name.c_str() ) == "Referrer" )
			for( const SceneEditController::GraphPort& port : n.outEdges )
				if( port.otherNode == SceneEditController::kInvalidNodeIndex ) foundDangling = true;
	Check( foundDangling, "5c: the assembler actually produced a node-less port (test setup sanity)" );

	const GraphLayout::Positions pos = GraphLayout::LayoutGraph( g, GraphLayout::Positions(), TestConfig() );
	const GraphLayoutPoint* referrer = FindPos( pos, "Referrer" );
	Check( referrer != nullptr, "5c: Referrer positioned" );
	// Rank should be exactly 1 (from the ONE real dependency) -- if the
	// dangling port were miscounted as a second dependency of unknown
	// rank, this would either crash (out-of-range read) or, if it were
	// silently treated as rank -1/0 contributing +1, still land on 1 by
	// coincidence; the real regression this guards is an
	// out-of-bounds/UB read on `otherNode`, which the kInvalidNodeIndex
	// check in GraphLayout::ComputeRanks exists to prevent.
	if( referrer ) CheckDoubleEq( referrer->x, 100.0, "5c: Referrer at column 1 (dangling port excluded from the deps count)" );
}

int main()
{
	std::cout << "=== GraphLayoutTest ===" << std::endl;

	TestLinearChain();
	TestDiamondFanIn();
	TestWideFlatRank();
	TestMultiRoot();

	TestLongestPath();

	TestDeclarationOrderTiebreak();

	TestSavedNeverMovesAndCollisionAvoidance();
	TestByteDeterministic();
	TestAddingOneNodeStability();
	TestInsertBetweenSiblingsDocumentedShift();

	TestHostileCycleTerminates();
	TestSelfReferenceTerminates();
	TestDanglingPortIgnoredForRank();

	std::cout << "\n=== " << passCount << " passed, " << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
