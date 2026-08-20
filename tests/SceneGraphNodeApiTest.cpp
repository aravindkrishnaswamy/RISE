//////////////////////////////////////////////////////////////////////
//
//  SceneGraphNodeApiTest.cpp - docs/agentic-redesign/87-recursive-scene-graph.md
//    section 5 step 4a: the GENERIC NODE-CHILDREN API on
//    SceneEditController, over the AUTHORED graph, plus its C-ABI
//    surface.
//
//  4a ships no UI.  What it ships is the one surface 4b (SwiftUI
//  `OutlineGroup`) and 4c (Qt `QAbstractItemModel`) both consume, so
//  everything asserted here is asserted through that surface and not
//  through the object manager it is built from.
//
//  Cases:
//    A -- a FLAT category still enumerates.  Camera / Material / Painter
//         have no hierarchy, and 4a models them as N roots with no
//         children rather than letting each shell special-case them.  The
//         node set must equal the flat CategoryEntityName list exactly,
//         in the same order, or the two surfaces disagree about what a
//         scene contains.
//    B -- a parent/child chain NESTS, and a three-level chain nests
//         through the middle node (a two-level fixture cannot tell
//         "parent links are followed" from "every non-root is attached to
//         the first root").
//    C -- sibling order is DECLARATION order (87 section 2), not the name
//         order the manager's own map enumeration would hand over.
//    D -- a DANGLING parent is rooted, not lost.
//    E -- a CYCLE terminates and keeps every node visible.
//         D and E are driven through the PURE assembler: neither is
//         reachable through IObjectManager::SetObjectParent (it refuses an
//         unknown parent outright and walks the ancestor chain to refuse a
//         cycle), so a scene-level fixture cannot produce either.
//    F -- the COLLAPSE-case instancing node (87 step 3a, `source <leaf>`)
//         stays VISIBLE as its own node: its entry name IS the chunk name,
//         so it is the authored node, not a copy of one.
//    G -- a SUBTREE instance's members FOLD: `I` is one row, `I.X` is not
//         a row at all.  The source subtree it was copied from is
//         untouched -- `source` copies, it does not hide.
//    H -- a COUNTED instance folds the same way, and the chunk the author
//         wrote gets the row.  This is the case where the fold TARGET has
//         no live entry of its own (`count_u 2` produces `I[0,0]` and
//         `I[1,0]`, and no `I`).
//    I -- a document node parented onto a SYNTHESIZED entry (`Z parent
//         I.B`, which 87 step 3b supports) lands under the instancing
//         chunk rather than becoming a stray root.
//    J -- the snapshot serves STALE rather than racing once the render
//         owns the scene, with a live control proving the assertion
//         discriminates.
//    K -- the C-ABI surface agrees with the C++ one, node for node, and
//         reports every out-of-range address as a failure rather than as
//         a plausible-looking zero.
//    L -- the flat API is UNCHANGED.  4b/4c migrate the shells off it;
//         nothing may regress before then.
//    M -- a COUNTED ARRAY is not swallowed by an unrelated live object
//         that shares the instancing chunk's name.  The fold reads the
//         LIVE manager's keyspace; the derive-time collision guard reads
//         the DOCUMENT's `name`-param keyspace, and a `gltf_import`
//         object puts `<prefix>.obj.n<node>.p<prim>` in the first and
//         nothing in the second.  (The old witness -- an unnamed chunk,
//         which defaults to `noname` -- is now refused at derive time,
//         asserted alongside.)
//    N -- a STALE handle FAILS rather than resolving to a different
//         object, through both the C++ and the C-ABI surface.
//    O -- a read is TRANSACTIONAL (ReadTree copies the whole tree under
//         one lock hold), and an idle refresh neither republishes nor
//         invalidates an outstanding handle.
//    P -- the sibling sort's tie-break-by-name branch.
//    Q -- a RENAME that leaves the tree's SHAPE alone still republishes.
//         Equivalence is defined on observable CONTENT, and a name is
//         content: without the name half of the compare a rename keeps the
//         generation, so every outstanding handle keeps resolving to the OLD
//         name while the flat surface reports the new one.
//    S -- a REPLACEMENT under the same name invalidates handles.  Remove an
//         entity and re-add a DIFFERENT instance under the SAME name and
//         every structural member of its row -- and its name -- compares
//         equal, so only the registration serial can tell the tree changed.
//    R -- there is a WAY BACK from a ReadTree row to the per-node getters.
//         ReadTree is what every multi-node consumer is told to use and it
//         yields RAW indices; without HandleFor a shell that follows the
//         advice has to re-walk (losing the transactional property) or
//         hand-roll the handle layout (losing the encapsulation).  C++-only:
//         the C-ABI surface has no ReadTree to pair it with, because
//         AuthoredTree is a nested C++ type RISE_API.h cannot name without
//         including SceneEditController.h.
//    T -- a PAINTER row's serial belongs to the entity that row DENOTES.
//         Painter is the union of two managers with independent serial
//         counters and no dedup, so one name can address two different
//         entities; deriving the serial by probing the managers in order
//         gave both rows the colour painter's, and a scalar-side
//         replacement then moved nothing the tree could see.
//    U -- a FULL RE-DERIVE invalidates, even though every serial comes back
//         IDENTICAL.  A serial is unique only within one manager instance,
//         and ClearAll + re-derive restarts every counter -- so the whole
//         per-row equivalence is defeated by a rebuild unless the tree
//         records which build it came from.
//    V -- the HANDLE-CHURN ASYMMETRY: an incremental param edit drops and
//         re-adds a Geometry (fresh serial, every handle dies) but
//         re-points an Object in place (serial held, nothing dies).
//         Correct, surprising, and pinned so it cannot change silently.
//    W -- a handle minted on ONE controller does not resolve on another.
//         Generations come from a process-global counter, so no two
//         published trees anywhere share one; while the counter was a
//         per-controller member seeded at 1, A's handle named B's node.
//    X -- SetPrimaryAcceleration is the SECOND site that must bump the
//         container-rebuild count: it replaces the ObjectManager without
//         going through InitializeContainers, and the fresh manager restarts
//         its serial counter -- the precondition the count exists to detect.
//    Y -- the STRUCTURE the outliner shells reshape ReadTree into (step 4b):
//         roots+children is a total, duplicate-free, in-bounds partition that
//         agrees with each row's `parent`, over every shape the assembler can
//         produce including its two hostile inputs.
//   AA -- a SYNTHESIZED entry RESOLVES to the row that represents it.
//         Includes the two contracts nothing reachable through scene text
//         can reach: the ITERATED chain walk with its cycle guard (every
//         expansion records provenance one hop, so the loop never takes a
//         second iteration -- driven here through the public provenance
//         setter), and the NO-REFRESH-WHEN-WARM cost guarantee both shells
//         depend on.
//         Viewport picking selects the LIVE entry a ray hit (`I[1,0]`),
//         which is not a row, so an outliner matching the selection name
//         against row names highlighted NOTHING.  `ResolveTreeRowName`
//         folds the entry onto its instancing chunk for row-highlighting
//         only -- the selection itself, and therefore the panel, the gizmo
//         and the viewport chrome, still name the copy that was clicked.
//    Z -- a row the tree OFFERS must INSPECT (step 4b review).  A counted
//         instancing chunk's node has no live object of its own, and the
//         properties panel's Object arm inspected the live object only -- so
//         the one row whose whole justification is "it has a chunk to edit"
//         published an EMPTY panel.  Now it falls back to the chunk, and the
//         edit route follows.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <cstdio>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectPriv.h"   // Q: addref/release around the rename's drop + re-register
#include "../src/Library/Interfaces/IMaterialManager.h" // S: the flat-category half of the replacement case
#include "../src/Library/Interfaces/IPainterManager.h"       // T: the colour half of the Painter union
#include "../src/Library/Interfaces/IScalarPainterManager.h" // T: the physical-scalar half

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0, failCount = 0;
static void Check( bool c, const char* n )
{
	if( c ) { ++passCount; }
	else    { ++failCount; std::cout << "  FAIL: " << n << std::endl; }
}
static void CheckEq( const std::string& got, const std::string& want, const char* n )
{
	if( got == want ) { ++passCount; return; }
	++failCount;
	std::cout << "  FAIL: " << n << "\n    got  : " << got << "\n    want : " << want << std::endl;
}

typedef SceneEditController::Category Cat;
typedef SceneEditController::TreeNodeSeed Seed;

// ---------------------------------------------------------------------
// Oracles
// ---------------------------------------------------------------------

//! The WHOLE tree as one string: a depth-first pre-order walk from the
//! roots, one node per line, two spaces of indent per level, lines joined
//! by `|`.  A whole-structure oracle rather than a per-name spot check
//! because every failure mode this file guards against is STRUCTURAL --
//! a fold that stops folding adds rows, a parent resolution that stops
//! resolving MOVES a whole branch to the root, a lost sort permutes
//! siblings.  A spot check on one name sees none of those.
//!
//! Walks by handle through the public API only, so it also exercises
//! TreeRootNode / TreeChildNode / TreeNodeNameByHandle / TreeNodeParent -- and
//! asserts, on the way, that every node's recorded parent agrees with the
//! node it was reached FROM.
static std::string TreeDump( const SceneEditController& c, Cat cat, bool* outParentsAgree = 0 )
{
	if( outParentsAgree ) *outParentsAgree = true;
	std::string out;
	// (node, depth, expected parent) -- an explicit stack, so a cycle that
	// somehow survived the assembler shows up as a hang in THIS test rather
	// than as a stack overflow with no line number.  Bounded below.
	std::vector<SceneEditController::TreeNodeHandle> stackNode, stackParent;
	std::vector<unsigned int> stackDepth;
	const unsigned int total = c.TreeNodeCount( cat );
	const unsigned int roots = c.TreeRootCount( cat );
	for( unsigned int r = roots; r > 0; --r ) {
		stackNode.push_back( c.TreeRootNode( cat, r - 1 ) );
		stackDepth.push_back( 0 );
		stackParent.push_back( SceneEditController::kInvalidTreeNode );
	}
	unsigned int emitted = 0;
	while( !stackNode.empty() ) {
		const SceneEditController::TreeNodeHandle n = stackNode.back();  stackNode.pop_back();
		const unsigned int d = stackDepth.back();                        stackDepth.pop_back();
		const SceneEditController::TreeNodeHandle p = stackParent.back(); stackParent.pop_back();
		if( c.TreeNodeParent( cat, n ) != p && outParentsAgree ) *outParentsAgree = false;
		if( !out.empty() ) out += "|";
		for( unsigned int i = 0; i < d; ++i ) out += "  ";
		out += c.TreeNodeNameByHandle( cat, n ).c_str();
		if( ++emitted > total ) { out += "|<OVERRUN>"; break; }
		const unsigned int kids = c.TreeChildCountByHandle( cat, n );
		for( unsigned int k = kids; k > 0; --k ) {
			stackNode.push_back( c.TreeChildNode( cat, n, k - 1 ) );
			stackDepth.push_back( d + 1 );
			stackParent.push_back( n );
		}
	}
	return out;
}

//! The same walk over a raw AuthoredTree, for the pure-assembler cases.
//! `budget` bounds the walk so a cycle the assembler failed to break is
//! reported as `<OVERRUN>` instead of hanging the suite -- a hang in CI
//! reads as infrastructure flake, not as a red test.
static std::string RawDump( const SceneEditController::AuthoredTree& t )
{
	std::string out;
	std::vector<unsigned int> node, depth;
	for( std::size_t r = t.roots.size(); r > 0; --r ) {
		node.push_back( t.roots[r - 1] );
		depth.push_back( 0 );
	}
	std::size_t budget = t.nodes.size() + 1;
	while( !node.empty() ) {
		const unsigned int n = node.back();  node.pop_back();
		const unsigned int d = depth.back(); depth.pop_back();
		if( budget-- == 0 ) { out += "|<OVERRUN>"; break; }
		if( !out.empty() ) out += "|";
		for( unsigned int i = 0; i < d; ++i ) out += "  ";
		out += t.nodes[n].name.c_str();
		const SceneEditController::TreeNodeRow& row = t.nodes[n];
		for( unsigned int k = row.childCount; k > 0; --k ) {
			node.push_back( t.childIndices[ row.firstChild + k - 1 ] );
			depth.push_back( d + 1 );
		}
	}
	return out;
}

//! THE STRUCTURAL CONTRACT A SHELL RESHAPES `ReadTree` UNDER (87 step 4b).
//!
//! Both outliner shells turn one `AuthoredTree` into {name, parent, children}
//! rows plus a roots list and then draw exactly the nodes that walk reaches.
//! Four properties make that drawing faithful, and a shell can check NONE of
//! them for itself -- a node missing from the walk is simply a row that is not
//! there, and a node reached twice is a row drawn twice, neither of which
//! looks like a failure on screen.
//!
//! HONEST SCOPE: this is NOT coverage of `-[RISEViewportBridge categoryTree:]`
//! or of `ViewportBridge::categoryTree`.  Both live in GUI targets that are not
//! linkable from here, so what is pinned is the CONTRACT they are built on, not
//! either one's transcription of it.  Said out loud rather than left to read as
//! shell coverage.
//!
//! Returns "" when every property holds, otherwise the first violation found.
static std::string TreeShapeViolation( const SceneEditController::AuthoredTree& t )
{
	const std::size_t n = t.nodes.size();

	// (1) PARTITION.  Every node is either a root or exactly one node's
	// child, so the two lists together account for the table exactly once.
	// A shell that draws roots-then-descendants relies on this to draw the
	// whole tree without a separate reachability pass.
	std::size_t kids = 0;
	for( std::size_t i = 0; i < n; ++i ) kids += t.nodes[i].childCount;
	if( kids != t.childIndices.size() ) return "childCounts do not sum to childIndices.size()";
	if( t.roots.size() + t.childIndices.size() != n ) return "roots + children != nodes";

	// (2) BOUNDS.  A shell converts these to array indices; an out-of-range
	// one is either a crash or -- as both bridges are written, defensively --
	// a silently dropped row.
	for( std::size_t r = 0; r < t.roots.size(); ++r )
		if( t.roots[r] >= n ) return "root index out of range";
	for( std::size_t c = 0; c < t.childIndices.size(); ++c )
		if( t.childIndices[c] >= n ) return "child index out of range";
	for( std::size_t i = 0; i < n; ++i ) {
		const SceneEditController::TreeNodeRow& row = t.nodes[i];
		if( static_cast<std::size_t>( row.firstChild ) + row.childCount > t.childIndices.size() )
			return "child slice runs off childIndices";
	}

	// (3) THE WALK IS TOTAL AND DUPLICATE-FREE.
	std::vector<unsigned char> seen( n, 0 );
	std::vector<unsigned int>  stack;
	for( std::size_t r = t.roots.size(); r > 0; --r ) stack.push_back( t.roots[r - 1] );
	std::size_t budget = n + 1;
	while( !stack.empty() ) {
		const unsigned int cur = stack.back(); stack.pop_back();
		if( budget-- == 0 ) return "walk overran the node count (a cycle survived assembly)";
		if( seen[cur] ) return "a node is reached twice by the roots+children walk";
		seen[cur] = 1;
		const SceneEditController::TreeNodeRow& row = t.nodes[cur];
		// (4) PARENT AGREEMENT.  `parent` and the child slices are two
		// INDEPENDENT descriptions of one edge set, written by two different
		// passes of the assembler, so a disagreement means one of them is
		// wrong and only comparing them can say so.
		//
		// NOT because the macOS shell reads both -- it does not.  4b's
		// `outlinerFlatten` walks `children` only and accumulates each row's
		// expand-state path on the way down, and `RISESceneTreeNode.parent`
		// has no reader in any Swift file; the bridge merely WRITES it.  It
		// stays pinned because it is published API that a consumer may read
		// (the Qt tree of 87 step 4c carries the same field, and re-deriving
		// roots by scanning for parent == -1 is exactly the shape 4b's commit
		// flagged as tempting), and because a `parent` that disagrees with the
		// slices would make selection-reveal and nesting name different trees.
		for( unsigned int k = row.childCount; k > 0; --k ) {
			const unsigned int child = t.childIndices[ row.firstChild + k - 1 ];
			if( t.nodes[child].parent != cur ) return "a child's `parent` is not the row it hangs under";
			stack.push_back( child );
		}
	}
	for( std::size_t i = 0; i < n; ++i ) if( !seen[i] ) return "a node is unreachable from the roots";
	for( std::size_t r = 0; r < t.roots.size(); ++r )
		if( t.nodes[ t.roots[r] ].parent != SceneEditController::kInvalidNodeIndex )
			return "a ROOT carries a parent index";
	return "";
}

static Seed MakeSeed( const char* name, const char* parent, unsigned long long order )
{
	Seed s;
	s.name   = String( name );
	s.parent = String( parent );
	s.order  = order;
	return s;
}

// ---------------------------------------------------------------------
// Scene fixtures
// ---------------------------------------------------------------------

static void WriteScene( const char* path, const std::string& body )
{
	std::ofstream o( path );
	o << "RISE ASCII SCENE 7\n"
	     "film\n{\nwidth 32\nheight 24\n}\n"
	     "pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
	     "uniformcolor_painter\n{\nname p\ncolor 1 1 1\n}\n"
	     "lambertian_material\n{\nname m\nreflectance p\n}\n"
	     "sphere_geometry\n{\nname g\nradius 1\n}\n"
	  << body;
}

static Job* LoadScene( const char* path, const std::string& body, const char* what )
{
	WriteScene( path, body );
	Job* j = new Job();
	Check( j->LoadAsciiSceneViaCst( path ), what );
	return j;
}

int main()
{
	std::cout << "SceneGraphNodeApiTest" << std::endl;

	// =================================================================
	// A -- a FLAT category still enumerates, as roots with no children,
	//      and node-for-node equals the flat list.
	// =================================================================
	{
		const char* s = "sgnode_flat.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname obj\ngeometry g\nmaterial m\n}\n",
			"A: flat scene loads" );
		{
			SceneEditController c( *j, 0 );
			// Materials: `m` only.  Cameras: `cam` only.  Painters: `p` plus
			// whatever the derive registers -- so compare against the FLAT
			// list rather than against a hard-coded name set, which is the
			// property that actually matters (the two surfaces must not
			// disagree about what the scene contains).
			const Cat flats[3] = { Cat::Camera, Cat::Material, Cat::Painter };
			for( int f = 0; f < 3; ++f ) {
				const Cat cat = flats[f];
				const unsigned int n = c.CategoryEntityCount( cat );
				Check( n > 0, "A: the flat category is non-empty (the fixture is meaningful)" );
				Check( c.TreeNodeCount( cat ) == n,
				       "A: the tree has exactly as many nodes as the flat list has entries" );
				Check( c.TreeRootCount( cat ) == n,
				       "A: every entry of a flat category is a ROOT" );
				bool sameNames = true, anyChildren = false;
				for( unsigned int i = 0; i < n; ++i ) {
					const SceneEditController::TreeNodeHandle node = c.TreeRootNode( cat, i );
					if( std::string( c.TreeNodeNameByHandle( cat, node ).c_str() )
					 != std::string( c.CategoryEntityName( cat, i ).c_str() ) ) sameNames = false;
					if( c.TreeChildCountByHandle( cat, node ) != 0 ) anyChildren = true;
					if( c.TreeNodeParent( cat, node ) != SceneEditController::kInvalidTreeNode ) anyChildren = true;
				}
				Check( sameNames, "A: root i's name IS flat entry i's name, in the SAME order" );
				Check( !anyChildren, "A: a flat category's nodes have no children and no parent" );
			}
			// Category::None and an out-of-range int are answered, not indexed.
			Check( c.TreeNodeCount( Cat::None ) == 0, "A: Category::None has no tree" );
			Check( c.TreeRootNode( Cat::None, 0 ) == SceneEditController::kInvalidTreeNode,
			       "A: Category::None root lookup is the invalid handle" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// B -- a parent/child chain NESTS, at two levels and at three.
	//
	// The three-level case is not decoration: with only `root`/`kid` a
	// build that attached every non-root to the FIRST root would be
	// indistinguishable from one that follows the links.
	// =================================================================
	{
		const char* s = "sgnode_chain.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname root\nposition 1 0 0\n}\n"
			"standard_object\n{\nname mid\nparent root\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname leaf\nparent mid\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname loner\ngeometry g\nmaterial m\n}\n",
			"B: chain scene loads" );
		{
			SceneEditController c( *j, 0 );
			bool parentsAgree = false;
			const std::string dump = TreeDump( c, Cat::Object, &parentsAgree );
			CheckEq( dump, "root|  mid|    leaf|loner",
			         "B: a three-level chain nests through its middle node, and an unparented "
			         "object stays a root" );
			Check( parentsAgree,
			       "B: every node's recorded parent is the node it was reached from" );
			Check( c.TreeNodeCount( Cat::Object ) == 4, "B: four nodes" );
			Check( c.TreeRootCount( Cat::Object ) == 2, "B: two roots" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// C -- sibling order is DECLARATION order, not name order.
	//
	// The manager's item map is keyed by name, so its enumeration hands
	// entries over ALPHABETICALLY.  87 section 2 says child order for
	// display comes from declaration order, which is what the
	// registration serial carries.  Declaring `zeta` before `alpha` makes
	// the two orders disagree, so a build that just kept enumeration
	// order is red here.
	// =================================================================
	{
		const char* s = "sgnode_order.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname hub\n}\n"
			"standard_object\n{\nname zeta\nparent hub\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname alpha\nparent hub\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname mid\nparent hub\ngeometry g\nmaterial m\n}\n",
			"C: sibling-order scene loads" );
		{
			SceneEditController c( *j, 0 );
			CheckEq( TreeDump( c, Cat::Object ), "hub|  zeta|  alpha|  mid",
			         "C: siblings come out in DECLARATION order, not the manager's name order" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// D -- a DANGLING parent is ROOTED, not dropped.
	//
	// Driven through the pure assembler: SetObjectParent refuses a parent
	// that is not a registered object, and RemoveItem re-roots orphans, so
	// no scene can put a dangling link in front of the tree build.  What
	// the guard protects against is a FUTURE path that records a link
	// without going through those, and the cost of getting it wrong is an
	// object that silently disappears from the outliner.
	// =================================================================
	{
		std::vector<Seed> seeds;
		seeds.push_back( MakeSeed( "keep",    "",        1 ) );
		seeds.push_back( MakeSeed( "orphan",  "ghost",   2 ) );   // `ghost` is not a seed
		seeds.push_back( MakeSeed( "child",   "orphan",  3 ) );
		const SceneEditController::AuthoredTree t = SceneEditController::BuildAuthoredTree( seeds );
		CheckEq( RawDump( t ), "keep|orphan|  child",
		         "D: a node whose parent does not exist becomes a ROOT, keeps its own children, "
		         "and is not lost" );
		Check( t.nodes.size() == 3, "D: every seed still produced a node" );
	}

	// =================================================================
	// E -- a CYCLE terminates and keeps every node.
	//
	// Three shapes, because they fail differently: a 2-cycle, a 3-cycle
	// with a branch hanging off it (the branch must survive too), and a
	// SELF-parent.  Without the cycle break none of the cycle's nodes is a
	// root, so none is reachable from the root list -- the whole group
	// vanishes from the outliner while still occupying the node table,
	// which is exactly what the `<OVERRUN>`-free dump below catches.
	// =================================================================
	{
		std::vector<Seed> seeds;
		seeds.push_back( MakeSeed( "a", "b", 1 ) );
		seeds.push_back( MakeSeed( "b", "a", 2 ) );
		const SceneEditController::AuthoredTree t = SceneEditController::BuildAuthoredTree( seeds );
		CheckEq( RawDump( t ), "a|  b",
		         "E1: a 2-cycle is broken at one link -- both nodes remain, one becomes a root" );
	}
	{
		std::vector<Seed> seeds;
		seeds.push_back( MakeSeed( "x",    "z", 1 ) );
		seeds.push_back( MakeSeed( "y",    "x", 2 ) );
		seeds.push_back( MakeSeed( "z",    "y", 3 ) );
		seeds.push_back( MakeSeed( "twig", "y", 4 ) );
		const SceneEditController::AuthoredTree t = SceneEditController::BuildAuthoredTree( seeds );
		CheckEq( RawDump( t ), "x|  y|    z|    twig",
		         "E2: a 3-cycle with a branch is broken once -- all four nodes stay visible" );
	}
	{
		std::vector<Seed> seeds;
		seeds.push_back( MakeSeed( "self", "self", 1 ) );
		seeds.push_back( MakeSeed( "kid",  "self", 2 ) );
		const SceneEditController::AuthoredTree t = SceneEditController::BuildAuthoredTree( seeds );
		CheckEq( RawDump( t ), "self|  kid",
		         "E3: a self-parent is a root, and still parents its own children" );
	}
	{
		// A long chain, iteratively assembled and iteratively walked: the
		// assembler must not recurse, or a deep hierarchy overflows the stack
		// inside a UI poll.  4096 is far past any authored depth and far
		// short of a heap concern.
		std::vector<Seed> seeds;
		char nm[32], pn[32];
		for( int i = 0; i < 4096; ++i ) {
			std::snprintf( nm, sizeof(nm), "n%04d", i );
			std::snprintf( pn, sizeof(pn), "n%04d", i - 1 );
			seeds.push_back( MakeSeed( nm, i == 0 ? "" : pn, (unsigned long long)( i + 1 ) ) );
		}
		const SceneEditController::AuthoredTree t = SceneEditController::BuildAuthoredTree( seeds );
		Check( t.nodes.size() == 4096 && t.roots.size() == 1,
		       "E4: a 4096-deep chain assembles to one root and 4096 nodes without recursing" );
		unsigned int walked = 0, cur = t.roots.empty() ? 0u : t.roots[0];
		while( walked < 8192 ) {
			++walked;
			if( t.nodes[cur].childCount == 0 ) break;
			cur = t.childIndices[ t.nodes[cur].firstChild ];
		}
		Check( walked == 4096, "E4: and the chain really is 4096 links long" );
	}

	// =================================================================
	// F -- the COLLAPSE case stays VISIBLE.
	//
	// `I source S` over a LEAF source produces exactly one entry, named
	// after the instancing chunk (87 step 3a).  Its provenance row exists,
	// so a fold rule that keyed on "has provenance" instead of on "entry
	// name != chunk name" would delete the author's own row.
	// =================================================================
	{
		const char* s = "sgnode_collapse.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\nposition 2 0 0\n}\n"
			"standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n",
			"F: collapse-case scene loads" );
		{
			SceneEditController c( *j, 0 );
			CheckEq( TreeDump( c, Cat::Object ), "S|I",
			         "F: the collapse-case instance is its own node (entry name IS the chunk name), "
			         "and the source it copied is still there" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// G -- a SUBTREE instance's members FOLD.
	//
	// `I source S` where S has a child produces `I` and `I.X`.  `I.X` has
	// no chunk of its own, so it is not a row: one instance, one node.
	// The source subtree keeps both of its own rows.
	// =================================================================
	{
		const char* s = "sgnode_subtree.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname X\nparent S\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n",
			"G: subtree-instance scene loads" );
		{
			SceneEditController c( *j, 0 );
			// The manager really does carry the synthesized entry -- otherwise
			// the fold below would be asserting nothing.
			const IScene* sc = j->GetScene();
			const IObjectManager* om = sc ? sc->GetObjects() : 0;
			Check( om && const_cast<IObjectManager*>( om )->GetItem( "I.X" ) != 0,
			       "G: the manager DOES hold the synthesized entry `I.X` (the fold has something to fold)" );
			CheckEq( TreeDump( c, Cat::Object ), "S|  X|I",
			         "G: the instance is ONE node -- its cloned member `I.X` is folded into it -- "
			         "while the source subtree keeps both rows" );
			// And the flat list still shows the raw render list, unchanged.
			bool flatHasClone = false;
			for( unsigned int i = 0; i < c.CategoryEntityCount( Cat::Object ); ++i )
				if( std::string( c.CategoryEntityName( Cat::Object, i ).c_str() ) == "I.X" ) flatHasClone = true;
			Check( flatHasClone,
			       "G: the FLAT list is untouched -- it still enumerates `I.X` (4a changes no existing surface)" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// H -- a COUNTED instance folds, and the CHUNK gets the row.
	//
	// `count_u 2` produces `I[0,0]` and `I[1,0]` and NO entry named `I`
	// (87 step 3c: PRESENCE of a count selects the repeated naming).  So
	// this is the one shape where the fold TARGET has no live entry, and
	// a build that folded without synthesizing one would make the whole
	// array -- and the chunk the author wrote -- vanish from the tree.
	// =================================================================
	{
		const char* s = "sgnode_counted.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname anchor\n}\n"
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname X\nparent S\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nparent anchor\nsource S\ncount_u 2\nposition expr(i*3) 0 0\n}\n",
			"H: counted-instance scene loads" );
		{
			SceneEditController c( *j, 0 );
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om && om->GetItem( "I[0,0]" ) && om->GetItem( "I[1,0]" ) && om->GetItem( "I[1,0].X" ),
			       "H: the manager holds the four synthesized entries" );
			Check( om && om->GetItem( "I" ) == 0,
			       "H: and NO entry named `I` -- the fold target has no live object (the premise)" );
			CheckEq( TreeDump( c, Cat::Object ), "anchor|  I|S|  X",
			         "H: all four repetitions fold into ONE node named after the chunk, and that node "
			         "inherits the chunk's own `parent`" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// I -- a document node parented onto a SYNTHESIZED entry.
	//
	// 87 step 3b supports `parent I.B`.  `I.B` is folded away, so unless
	// the parent NAME is resolved through the same fold, `Z` loses its
	// parent and becomes a stray root -- structure silently lost, with no
	// diagnostic anywhere.
	// =================================================================
	{
		const char* s = "sgnode_parentsynth.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname B\nparent S\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n"
			"standard_object\n{\nname Z\nparent I.B\ngeometry g\nmaterial m\nposition 0 0 1\n}\n",
			"I: parent-onto-synthesized scene loads" );
		{
			SceneEditController c( *j, 0 );
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om && om->GetItem( "I.B" ) != 0, "I: `I.B` really was synthesized" );
			CheckEq( TreeDump( c, Cat::Object ), "S|  B|I|  Z",
			         "I: a node parented onto a folded entry lands under the INSTANCING CHUNK, "
			         "not at the root" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// J -- STALE beats racing.
	//
	// `RefreshTreeSnapshot_` must bail before touching the live managers
	// once the render owns the scene: a UI-thread walk of the manager on
	// demand inverts the lock order against the render.  Driven through
	// `PrepareForDestruction`, which is the one public entry that raises
	// that flag WITHOUT also holding mMutex -- so the flag, and not the
	// try_lock fallback behind it, is what this case separates.
	//
	// The second scope is the control: a fresh controller on the SAME job
	// reports the detached shape, so the stale answer above is a real
	// difference and not the only answer available.
	// =================================================================
	{
		const char* s = "sgnode_stale.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname hub\n}\n"
			"standard_object\n{\nname arm\nparent hub\ngeometry g\nmaterial m\nposition 0 1 0\n}\n",
			"J: stale-fallback scene loads" );
		{
			SceneEditController c( *j, 0 );
			CheckEq( TreeDump( c, Cat::Object ), "hub|  arm", "J: the tree starts nested" );

			// Live structural mutation, made behind the controller's back.
			Check( j->SetObjectParent( "arm", 0 ), "J: `arm` detached on the live manager" );

			Check( c.PrepareForDestruction(), "J: the controller is prepared for destruction" );
			Check( c.ForTest_RenderOwnsScene(), "J: ... which leaves the render owning the scene" );

			CheckEq( TreeDump( c, Cat::Object ), "hub|  arm",
			         "J: the tree serves the STALE snapshot while the render owns the scene -- it does "
			         "not walk the live manager" );
		}
		{
			SceneEditController c2( *j, 0 );
			CheckEq( TreeDump( c2, Cat::Object ), "hub|arm",
			         "J: control -- a controller that IS allowed to refresh reports the detached shape" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// K -- the C-ABI surface.
	//
	// THE FIXTURE IS THE TEST HERE, but NOT for the reason this comment
	// used to give.  It was written against the pre-handle API, where a
	// node's address was a raw INDEX and the first-registered root's index
	// was 0 -- the same value a wrapper that merely zeroed its out-param
	// produces -- so `*outNode = 0;` in TreeRootNode and `*outParent = 0;`
	// in TreeNodeParent both left the suite green.  Generation tagging
	// retired that: a published tree's generation starts at 1, so a real
	// handle is never 0 and NO fixture can make a zeroed out-param look
	// right.  Both zeroing mutations are caught on the single-root fixture
	// too; that is no longer what this shape buys.
	//
	// What it buys is INDEX DISCRIMINATION.  Mutate TreeRootNode to ignore
	// `rootIdx` and always answer a properly encoded `roots[0]`: a
	// single-root fixture cannot see that AT ALL -- there is only one root,
	// so ignoring the index and honouring it are the same answer, by
	// construction and not by luck.  With `aaa` declared first the node
	// under test is the SECOND root, and that mutation reddens the walk
	// wholesale: measured 30 assertions red across A, B, F, G, H, I, J, K,
	// M, N, Q and S, six of them inside K itself.  The same argument
	// applies to `kid`'s parent, which must not be the first-registered
	// node.  Do not "simplify" the fixture back to one root.
	// =================================================================
	{
		const char* s = "sgnode_cabi.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname aaa\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname root\n}\n"
			"standard_object\n{\nname kid\nparent root\ngeometry g\nmaterial m\n}\n",
			"K: C-ABI scene loads" );
		{
			SceneEditController c( *j, 0 );
			const int OBJ = static_cast<int>( Cat::Object );

			Check( RISE_API_SceneEditController_TreeNodeCount( &c, OBJ ) == 3, "K: C count == 3" );
			Check( RISE_API_SceneEditController_TreeRootCount( &c, OBJ ) == 2, "K: C root count == 2" );
			Check( RISE_API_SceneEditController_TreeGeneration( &c, OBJ ) != 0,
			       "K: a published tree has a non-zero generation" );

			// Root 0 is `aaa`.  Reading it is what makes root 1 below a
			// DISCRIMINATING read rather than one that any constant would pass.
			unsigned long long firstRoot = 0xDEADull;
			Check( RISE_API_SceneEditController_TreeRootNode( &c, OBJ, 0, &firstRoot ),
			       "K: root 0 resolves" );
			char buf[128] = { 0 };
			Check( RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, firstRoot, buf, sizeof(buf) )
			    && std::string( buf ) == "aaa", "K: root 0 is `aaa` -- the first-registered object" );

			unsigned long long rootNode = 0xDEADull;
			Check( RISE_API_SceneEditController_TreeRootNode( &c, OBJ, 1, &rootNode ),
			       "K: root 1 resolves" );
			buf[0] = 0;
			Check( RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, rootNode, buf, sizeof(buf) )
			    && std::string( buf ) == "root", "K: root 1's name comes back as `root`" );
			Check( rootNode != firstRoot,
			       "K: the two roots have DIFFERENT handles -- TreeRootNode is not answering a constant" );

			Check( RISE_API_SceneEditController_TreeChildCountByHandle( &c, OBJ, rootNode ) == 1,
			       "K: `root` has one child" );
			Check( RISE_API_SceneEditController_TreeChildCountByHandle( &c, OBJ, firstRoot ) == 0,
			       "K: `aaa` has none -- the child count is per node, not a constant" );
			unsigned long long kidNode = 0xDEADull;
			Check( RISE_API_SceneEditController_TreeChildNode( &c, OBJ, rootNode, 0, &kidNode ),
			       "K: child 0 resolves" );
			buf[0] = 0;
			Check( RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, kidNode, buf, sizeof(buf) )
			    && std::string( buf ) == "kid", "K: the child's name comes back as `kid`" );

			unsigned long long par = 0xDEADull;
			Check( RISE_API_SceneEditController_TreeNodeParent( &c, OBJ, kidNode, &par )
			    && par == rootNode && par != firstRoot,
			       "K: the child's parent is `root` -- and NOT the first-registered node, so this "
			       "is not a zeroed out-param passing by coincidence" );

			// Every out-of-range address is a FAILURE, and leaves its
			// out-parameter untouched -- a shell that mistook a zeroed
			// out-param for node 0 would render the wrong row.
			unsigned long long sentinel = 0xBEEFull;
			Check( !RISE_API_SceneEditController_TreeRootNode( &c, OBJ, 9, &sentinel )
			    && sentinel == 0xBEEFull, "K: an out-of-range root index fails and writes nothing" );
			Check( !RISE_API_SceneEditController_TreeChildNode( &c, OBJ, rootNode, 9, &sentinel )
			    && sentinel == 0xBEEFull, "K: an out-of-range child index fails and writes nothing" );
			Check( !RISE_API_SceneEditController_TreeChildNode( &c, OBJ, 999, 0, &sentinel )
			    && sentinel == 0xBEEFull, "K: an unknown node handle fails and writes nothing" );
			Check( !RISE_API_SceneEditController_TreeNodeParent( &c, OBJ, rootNode, &sentinel )
			    && sentinel == 0xBEEFull, "K: a ROOT reports NO parent and writes nothing" );
			buf[0] = 'z';
			Check( !RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, 999, buf, sizeof(buf) ),
			       "K: an unknown handle has no name" );

			// Null-controller hardening, the same contract every other
			// RISE_API_SceneEditController_* getter carries.
			Check( RISE_API_SceneEditController_TreeNodeCount( 0, OBJ ) == 0, "K: null controller -> 0 nodes" );
			Check( RISE_API_SceneEditController_TreeRootCount( 0, OBJ ) == 0, "K: null controller -> 0 roots" );
			Check( RISE_API_SceneEditController_TreeGeneration( 0, OBJ ) == 0, "K: null controller -> generation 0" );
			Check( !RISE_API_SceneEditController_TreeRootNode( 0, OBJ, 0, &sentinel ), "K: null controller -> no root" );
			Check( RISE_API_SceneEditController_TreeChildCountByHandle( 0, OBJ, 0 ) == 0, "K: null controller -> 0 children" );
			Check( !RISE_API_SceneEditController_TreeChildNode( 0, OBJ, 0, 0, &sentinel ), "K: null controller -> no child" );
			Check( !RISE_API_SceneEditController_TreeNodeParent( 0, OBJ, 0, &sentinel ), "K: null controller -> no parent" );
			Check( !RISE_API_SceneEditController_TreeNodeNameByHandle( 0, OBJ, 0, buf, sizeof(buf) ), "K: null controller -> no name" );
			Check( !RISE_API_SceneEditController_TreeRootNode( &c, OBJ, 0, 0 ), "K: null out-pointer is refused" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// L -- the FLAT API is unchanged.  4b/4c migrate the shells off it;
	//      until then every existing caller must keep working, including
	//      on a scene that exercises every 4a code path (instancing +
	//      parenting).
	// =================================================================
	{
		const char* s = "sgnode_flatregress.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname X\nparent S\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n",
			"L: flat-regression scene loads" );
		{
			SceneEditController c( *j, 0 );
			std::set<std::string> flat;
			for( unsigned int i = 0; i < c.CategoryEntityCount( Cat::Object ); ++i )
				flat.insert( c.CategoryEntityName( Cat::Object, i ).c_str() );
			Check( flat.size() == 4
			    && flat.count( "S" ) && flat.count( "X" ) && flat.count( "I" ) && flat.count( "I.X" ),
			       "L: the flat object list is still the RENDER list -- all four entries, synthesized "
			       "ones included" );
			Check( std::string( c.CategoryActiveName( Cat::Camera ).c_str() ) == "cam",
			       "L: CategoryActiveName still answers" );
			Check( c.CategoryEntityName( Cat::Object, 999 ).size() <= 1,
			       "L: an out-of-range flat index is still empty" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// M -- A COUNTED ARRAY MUST NOT VANISH INTO AN UNRELATED OBJECT.
	//
	// Two keyspaces that do not agree.  The fold reads the LIVE manager's
	// names.  The derive-time guard that is supposed to make a fold target
	// unambiguous -- ExpandSourceInstance's collision scan -- reads the
	// DOCUMENT's `name` PARAM names.  A live object whose name is spelled by
	// no `name` param ANYWHERE is invisible to that guard, so the document
	// loads with ZERO diagnostics while the two keyspaces disagree about
	// exactly one name.
	//
	// THE WITNESS IS `gltf_import`, and it is the durable one.  It is a
	// FIFTH object-producing route (`Job::ImportGLTFScene` ->
	// `AddObjectMatrix`, GLTFSceneImporter.cpp) that registers one object per
	// mesh primitive as `<name_prefix>.obj.n<node>.p<prim>` -- names built
	// from the glTF file's own hierarchy, which no document text spells and
	// no document-level scan can enumerate.  `Box.glb` has its mesh on glTF
	// node 1, primitive 0, so `name_prefix P` yields exactly `P.obj.n1.p0`.
	// (Verified against the asset, not assumed: a `standard_object` spelling
	// that name fails its own AddItem with "Item of same name already
	// exists", and `P.obj.n0.p0` does not.)
	//
	// These cases used to use a NAMELESS `standard_object`, which defaults
	// its live entry to `noname`.  That divergence has since been closed at
	// its source -- Cst.cpp's `RoleDefaultedEntryName` indexes the defaulted
	// name and the old fixture is now REFUSED at derive time (pinned just
	// below, and in CstSourceInstanceTest).  The gltf route is NOT closable
	// that way and is what keeps PASS 1b load-bearing.
	//
	// Without PASS 1b: `P.obj.n1.p0[0,0]` and `[1,0]` fold into the unrelated
	// imported object `P.obj.n1.p0`; the synth pass then skips the chunk
	// because the target "already had an entry"; so the array AND the chunk
	// the author wrote get NO ROW AT ALL, and `Z parent P.obj.n1.p0[0,0]`
	// resolves onto the stranger.  Silently.
	//
	// With it: the fold is REFUSED (the target is live but is not the
	// collapse case), the repetitions stay visible as their own nodes, and
	// `Z` stays under the repetition it actually names.  A noisy outliner
	// is recoverable; a missing array is not.
	//
	// Both chunk ORDERS, because the fold decision must not depend on
	// which chunk registered first.
	// =================================================================
	static const char* const kGltfBox =
		"gltf_import\n{\nfile scenes/Tests/Geometry/assets/Box.glb\nname_prefix P\n}\n";
	{
		const char* s = "sgnode_namespace.RISEscene";
		Job* j = LoadScene( s,
			std::string( "standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n" )
			+ kGltfBox                                                            // -> live object `P.obj.n1.p0`
			+ "standard_object\n{\nname P.obj.n1.p0\nsource S\ncount_u 2\n}\n"
			  "standard_object\n{\nname Z\ngeometry g\nmaterial m\nparent P.obj.n1.p0[0,0]\n}\n",
			"M: the two-keyspace scene loads with no diagnostic" );
		{
			SceneEditController c( *j, 0 );
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om && om->GetItem( "P.obj.n1.p0" ) && om->GetItem( "P.obj.n1.p0[0,0]" )
			          && om->GetItem( "P.obj.n1.p0[1,0]" ),
			       "M: the premise -- an UNRELATED live object named `P.obj.n1.p0` coexists with a "
			       "counted instancing chunk of the same name" );
			CheckEq( TreeDump( c, Cat::Object ),
			         "S|P.obj.n1.p0|P.obj.n1.p0[0,0]|  Z|P.obj.n1.p0[1,0]",
			         "M: the counted array still has rows, and `Z` stays under the repetition it names "
			         "-- neither is swallowed by the unrelated same-named object" );
		}
		j->release();
		std::remove( s );
	}
	{
		const char* s = "sgnode_namespace2.RISEscene";
		Job* j = LoadScene( s,
			std::string( "standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n" )
			+ "standard_object\n{\nname P.obj.n1.p0\nsource S\ncount_u 2\n}\n"
			+ kGltfBox                                                            // imported AFTER
			+ "standard_object\n{\nname Z\ngeometry g\nmaterial m\nparent P.obj.n1.p0[0,0]\n}\n",
			"M: the reversed-order scene loads with no diagnostic" );
		{
			SceneEditController c( *j, 0 );
			CheckEq( TreeDump( c, Cat::Object ),
			         "S|P.obj.n1.p0[0,0]|  Z|P.obj.n1.p0[1,0]|P.obj.n1.p0",
			         "M: and the same holds with the chunks in the other order -- the fold decision "
			         "does not depend on which registered first" );
		}
		j->release();
		std::remove( s );
	}
	{
		// M's ORIGINAL fixture, now on the other side of the line.  A NAMELESS
		// `standard_object` beside an instancing chunk named `noname` was the
		// two-keyspace witness these cases used to run on; Cst.cpp now indexes
		// the defaulted entry name, so the derive REFUSES it instead of loading
		// it silently.  Asserted HERE, next to the cases it displaced, so the
		// reason they moved to the gltf route is recorded where a reader meets
		// them -- and so re-opening the divergence reddens this suite too, not
		// only CstSourceInstanceTest.
		const char* s = "sgnode_namespace_noname.RISEscene";
		WriteScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\ngeometry g\nmaterial m\n}\n"                    // unnamed -> live object `noname`
			"standard_object\n{\nname noname\nsource S\ncount_u 2\n}\n" );
		Job* j = new Job();
		Check( !j->LoadAsciiSceneViaCst( s ),
		       "M: the ORIGINAL nameless-`standard_object` witness is now REFUSED at derive time -- "
		       "the document-side half of this divergence is closed at its source" );
		j->release();
		std::remove( s );
	}

	// =================================================================
	// N -- A STALE HANDLE FAILS.  It does NOT quietly name another object.
	//
	// Measured on the pre-fix API: handle 1 resolved to `bkid`'s
	// predecessor, an object was removed, a count getter refreshed, and
	// handle 1 then resolved to a DIFFERENT node -- with the name getter
	// returning success and a plausible name.  No caller could tell.
	//
	// The shapes that reach it are ordinary: `for( i = 0; i <
	// c.TreeRootCount( cat ); ++i )` refreshes once per iteration, and a
	// QAbstractItemModel parks handles in QModelIndex::internalId() across
	// event-loop turns.  Node order is (registration serial, name), so a
	// remove shifts every later index down by one.
	//
	// The fixture is built so the stale index lands on a node of a
	// DIFFERENT KIND (a child, not a root) -- otherwise "resolved to the
	// wrong node" and "resolved to the right node" would look alike.
	// =================================================================
	{
		const char* s = "sgnode_stalehandle.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname aaa\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname bbb\n}\n"
			"standard_object\n{\nname bkid\nparent bbb\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname ccc\ngeometry g\nmaterial m\n}\n",
			"N: stale-handle scene loads" );
		{
			SceneEditController c( *j, 0 );
			const int OBJ = static_cast<int>( Cat::Object );
			CheckEq( TreeDump( c, Cat::Object ), "aaa|bbb|  bkid|ccc", "N: the tree starts as declared" );

			const SceneEditController::TreeNodeHandle h = c.TreeRootNode( Cat::Object, 1 );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Object, h ).c_str() ), "bbb",
			         "N: the captured handle names `bbb` before the mutation" );
			const unsigned long long genBefore = c.TreeGeneration( Cat::Object );

			// Remove the FIRST object, which shifts every later node index
			// down by one -- so the captured handle's raw index now addresses
			// `bkid`, a child of a different node.
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om && om->RemoveItem( "aaa" ), "N: `aaa` removed from the live manager" );
			Check( c.TreeNodeCount( Cat::Object ) == 3, "N: a count getter refreshes and sees three nodes" );
			Check( c.TreeGeneration( Cat::Object ) != genBefore,
			       "N: a tree that CHANGED gets a new snapshot generation" );

			Check( c.TreeNodeNameByHandle( Cat::Object, h ).size() <= 1,
			       "N: the STALE handle has NO name -- it does not resolve to whatever node now sits "
			       "at that index" );
			Check( c.TreeNodeParent( Cat::Object, h ) == SceneEditController::kInvalidTreeNode,
			       "N: the stale handle has no parent either" );
			Check( c.TreeChildCountByHandle( Cat::Object, h ) == 0, "N: and no children" );

			char buf[128] = { 0 };
			buf[0] = 'z';
			Check( !RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, h, buf, sizeof(buf) ),
			       "N: the C ABI reports the stale handle as a FAILURE, not as a row" );
			unsigned long long sentinel = 0xBEEFull;
			Check( !RISE_API_SceneEditController_TreeNodeParent( &c, OBJ, h, &sentinel )
			    && sentinel == 0xBEEFull,
			       "N: and the stale parent read fails and writes nothing" );

			// The control: the API is refusing STALENESS, not refusing to work.
			CheckEq( TreeDump( c, Cat::Object ), "bbb|  bkid|ccc",
			         "N: control -- a fresh walk after the mutation is correct and complete" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// O -- A WALK IS TRANSACTIONAL, and an idle refresh costs nothing.
	//
	// Two halves of the same problem.  (1) Even with NO mutation, a
	// multi-call walk takes the snapshot lock once per getter, so another
	// thread's count call can land between two of them; `ReadTree` closes
	// that by copying the whole tree under ONE hold, and the copy is the
	// caller's -- a later republish cannot reach into it.  (2) A refresh
	// that finds NOTHING CHANGED must not republish, or every count call
	// would invalidate every outstanding handle and the handle API would
	// be unusable for the shells it exists for.
	//
	// Driven single-threaded on purpose.  A genuinely concurrent mutator
	// would have to mutate the LIVE manager from a second thread, which no
	// production path does and which the controller's lock does not cover
	// -- that would be testing an unsupported shape, not this one.  The
	// property under test is the one that matters and it is fully
	// determined: the copy is unaffected by a republish that happens after
	// it was taken.
	// =================================================================
	{
		const char* s = "sgnode_transactional.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname hub\n}\n"
			"standard_object\n{\nname arm\nparent hub\ngeometry g\nmaterial m\nposition 0 1 0\n}\n",
			"O: transactional-read scene loads" );
		{
			SceneEditController c( *j, 0 );

			// (2) an idle refresh does not move the generation, so a handle
			//     taken before it still resolves after it.  The walk BEGINS at
			//     a count getter, which is the documented entry point -- the
			//     per-node getters serve the published snapshot and a fresh
			//     controller has not published one yet.
			Check( c.TreeNodeCount( Cat::Object ) == 2, "O: the first count publishes the tree" );
			const SceneEditController::TreeNodeHandle h = c.TreeRootNode( Cat::Object, 0 );
			const unsigned long long gen0 = c.TreeGeneration( Cat::Object );
			for( int i = 0; i < 5; ++i ) { c.TreeNodeCount( Cat::Object ); c.TreeRootCount( Cat::Object ); }
			Check( c.TreeGeneration( Cat::Object ) == gen0,
			       "O: refreshing an UNCHANGED tree does not republish it" );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Object, h ).c_str() ), "hub",
			         "O: so a handle survives the idle refreshes a UI poll makes" );

			// (1) the transactional read agrees with the handle walk ...
			SceneEditController::AuthoredTree t;
			c.ReadTree( Cat::Object, t );
			CheckEq( RawDump( t ), "hub|  arm",
			         "O: ReadTree hands over the WHOLE tree in one pass -- nodes, child index and roots" );
			Check( t.generation == c.TreeGeneration( Cat::Object ) && t.generation != 0,
			       "O: and it carries the generation it was published at" );

			// ... and the copy is immune to a later republish.
			Check( j->SetObjectParent( "arm", 0 ), "O: `arm` detached on the live manager" );
			Check( c.TreeNodeCount( Cat::Object ) == 2, "O: a count getter refreshes" );
			CheckEq( RawDump( t ), "hub|  arm",
			         "O: the copy taken earlier is UNCHANGED by the republish -- a walk over it cannot "
			         "mix two trees" );
			SceneEditController::AuthoredTree t2;
			c.ReadTree( Cat::Object, t2 );
			CheckEq( RawDump( t2 ), "hub|arm", "O: while a fresh read sees the new shape" );
			Check( t2.generation != t.generation, "O: which is a different generation" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// P -- the sibling sort's TIE-BREAK-BY-NAME branch.
	//
	// Every scene fixture gives its objects distinct registration serials,
	// so nothing above reaches the tiebreak; without this the branch is
	// live code with no coverage at all.  Driven through the pure
	// assembler, which is the only place two equal `order` keys can be
	// constructed.
	// =================================================================
	{
		std::vector<Seed> seeds;
		seeds.push_back( MakeSeed( "bee", "", 5 ) );
		seeds.push_back( MakeSeed( "ant", "", 5 ) );   // same order key as `bee`
		seeds.push_back( MakeSeed( "cat", "", 4 ) );
		const SceneEditController::AuthoredTree t = SceneEditController::BuildAuthoredTree( seeds );
		CheckEq( RawDump( t ), "cat|ant|bee",
		         "P: `order` decides first, and equal orders break by NAME -- not by the order the "
		         "seeds happened to arrive in" );
	}

	// =================================================================
	// Q -- A RENAME REPUBLISHES, EVEN THOUGH IT MOVES NOTHING.
	//
	// Equivalence is defined on observable CONTENT (TreesEquivalent), and
	// a NAME is content -- it is the row label and the selection identity.
	// A rename is, at the manager level, a DROP plus a RE-REGISTER under a
	// new name, and a fresh registration serial keeps the entity in the
	// same sort position.  So the rebuilt tree has the same size, the same
	// parents, the same roots and the same child slices as the published
	// one: every STRUCTURAL comparison agrees, and the row differs only in
	// the two CONTENT members.
	//
	// TWO HALVES, because the object half does NOT isolate the name.  A
	// manager-level rename moves the registration SERIAL as well (that is
	// what a drop plus a re-register does), so once the serial joined the
	// equivalence -- case S -- the serial catches the object half on its
	// own.  Measured: with the serial in place, deleting the name compare
	// leaves the object half green.  That is not a reason to drop the name
	// compare; it is a reason to test it where it is the ONLY thing that
	// can differ.
	//
	// It is the only thing that can differ wherever no serial is
	// available: Medium, Rasterizer, Film, Animation and SceneVariant all
	// seed 0, because none is reached through an IManager.  So the second
	// half renames a SCENE VARIANT.  Delete the name compare and that
	// rename republishes nothing -- the generation stands, the handle
	// taken before it keeps resolving, and it answers the OLD variant name
	// while the flat surface reports the new one.
	// =================================================================
	{
		const char* s = "sgnode_rename.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname AAA\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname BBB\ngeometry g\nmaterial m\nposition 2 0 0\n}\n",
			"Q: rename scene loads" );
		{
			SceneEditController c( *j, 0 );
			const int OBJ = static_cast<int>( Cat::Object );
			CheckEq( TreeDump( c, Cat::Object ), "AAA|BBB", "Q: the tree starts as declared" );

			const SceneEditController::TreeNodeHandle h = c.TreeRootNode( Cat::Object, 1 );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Object, h ).c_str() ), "BBB",
			         "Q: the captured handle names `BBB` before the rename" );
			const unsigned long long genBefore = c.TreeGeneration( Cat::Object );

			// THE RENAME.  Drop and re-register the SAME object under a new
			// name -- which is what a rename is here, since the manager keys
			// on the name.  addref across the drop because RemoveItem
			// releases the manager's own reference; AddItem takes a fresh
			// one, so the local ref is handed back immediately after.
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			IObjectPriv* moved = om ? om->GetItem( "BBB" ) : 0;
			Check( moved != 0, "Q: `BBB` is live before the rename" );
			if( moved ) {
				moved->addref();
				Check( om->RemoveItem( "BBB" ), "Q: `BBB` is dropped from the manager" );
				Check( om->AddItem( moved, "CCC" ), "Q: ... and re-registered as `CCC`" );
				moved->release();
			}

			// THE PREMISE: nothing structural moved.  Same node count, and
			// the new name lands in the same sort position (a fresh serial is
			// higher than `AAA`'s, so it stays last) -- so a shape-only
			// comparison would call the two trees equivalent.
			Check( c.TreeNodeCount( Cat::Object ) == 2,
			       "Q: the premise -- the rename left the node COUNT alone (a count getter refreshes)" );
			Check( c.TreeRootCount( Cat::Object ) == 2,
			       "Q: ... and the root count, so nothing structural changed at all" );

			Check( c.TreeGeneration( Cat::Object ) != genBefore,
			       "Q: the rename REPUBLISHES -- the snapshot generation advanced on a change that "
			       "moved no node" );
			Check( c.TreeNodeNameByHandle( Cat::Object, h ).size() <= 1,
			       "Q: so the handle captured before the rename FAILS -- it does not keep naming an "
			       "entity the manager no longer holds" );
			char buf[128] = { 0 };
			buf[0] = 'z';
			Check( !RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, h, buf, sizeof(buf) ),
			       "Q: and the C ABI refuses it too, rather than reporting a row" );

			CheckEq( TreeDump( c, Cat::Object ), "AAA|CCC",
			         "Q: a fresh walk shows the NEW name, in the position the old one held" );

			std::set<std::string> flat, tree;
			for( unsigned int i = 0; i < c.CategoryEntityCount( Cat::Object ); ++i )
				flat.insert( c.CategoryEntityName( Cat::Object, i ).c_str() );
			SceneEditController::AuthoredTree t;
			c.ReadTree( Cat::Object, t );
			for( std::size_t i = 0; i < t.nodes.size(); ++i ) tree.insert( t.nodes[i].name.c_str() );
			Check( flat == tree && flat.size() == 2 && flat.count( "AAA" ) && flat.count( "CCC" ),
			       "Q: the FLAT and TREE surfaces agree on the new name -- neither is still serving "
			       "the old one" );

			// THE SERIAL-LESS HALF.  SceneVariant is an index-addressed list
			// with no IManager behind it, so every row seeds serial 0 and the
			// NAME is the only member of the row that can change.  Rename the
			// one declared variant and the row count, the row order and every
			// index stay put; delete the name compare and nothing
			// republishes.
			Check( j->DeclareSceneVariant( "vA", "cam" ), "Q: a scene variant is declared" );
			CheckEq( TreeDump( c, Cat::SceneVariant ), "(base)|vA",
			         "Q: the SceneVariant tree lists the base entry and the variant" );
			const SceneEditController::TreeNodeHandle hv = c.TreeRootNode( Cat::SceneVariant, 1 );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::SceneVariant, hv ).c_str() ), "vA",
			         "Q: and a handle on the variant row resolves" );
			const unsigned long long varGen = c.TreeGeneration( Cat::SceneVariant );
			j->ClearSceneVariants();
			Check( j->DeclareSceneVariant( "vB", "cam" ), "Q: the variant is RENAMED (cleared and re-declared)" );
			Check( c.TreeNodeCount( Cat::SceneVariant ) == 2,
			       "Q: the premise -- same row count, and this category seeds NO serial, so the name "
			       "is the only member that differs" );
			Check( c.TreeGeneration( Cat::SceneVariant ) != varGen,
			       "Q: the rename republishes on the strength of the NAME comparison alone" );
			Check( c.TreeNodeNameByHandle( Cat::SceneVariant, hv ).size() <= 1,
			       "Q: so the variant handle taken before the rename fails" );
			CheckEq( TreeDump( c, Cat::SceneVariant ), "(base)|vB",
			         "Q: and a fresh walk shows the new variant name" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// R -- THE WAY BACK: a raw ReadTree index -> a per-node handle.
	//
	// ReadTree is documented as the transactional read every multi-node
	// consumer should use, and 4b/4c will drive selection and property
	// edits off tree ROWS.  But ReadTree yields RAW INDICES while the
	// per-node getters take generation-tagged HANDLES whose layout is
	// deliberately private, so without `HandleFor` a shell that follows
	// the advice has exactly two ways out of a model row, and both are
	// bad: re-walk with the per-node getters (surrendering the
	// transactional property it took ReadTree for), or hand-roll
	// `(generation << 32) | index` in shell code (breaking the invariant
	// that two functions know the layout).
	//
	// The VALIDITY claim is asserted, not just documented: a handle minted
	// from a copy resolves for exactly as long as that copy is still the
	// published tree.
	// =================================================================
	{
		const char* s = "sgnode_handlefor.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname hub\n}\n"
			"standard_object\n{\nname arm\nparent hub\ngeometry g\nmaterial m\nposition 0 1 0\n}\n",
			"R: handle-for scene loads" );
		{
			SceneEditController c( *j, 0 );
			SceneEditController::AuthoredTree t;
			c.ReadTree( Cat::Object, t );
			Check( t.nodes.size() == 2 && t.roots.size() == 1 && t.generation != 0,
			       "R: the fixture is one published root with one child" );

			bool allNamed = ( t.nodes.size() > 0 );
			for( std::size_t i = 0; i < t.nodes.size(); ++i ) {
				const SceneEditController::TreeNodeHandle hh =
					SceneEditController::HandleFor( t, static_cast<unsigned int>( i ) );
				if( std::string( c.TreeNodeNameByHandle( Cat::Object, hh ).c_str() )
				 != std::string( t.nodes[i].name.c_str() ) ) allNamed = false;
			}
			Check( allNamed,
			       "R: HandleFor turns EVERY raw ReadTree index into a handle the per-node getters "
			       "resolve to that same row" );
			Check( SceneEditController::HandleFor( t, t.roots[0] ) == c.TreeRootNode( Cat::Object, 0 ),
			       "R: and it is bit-for-bit the handle TreeRootNode mints -- ONE encoding, not a "
			       "second one that happens to agree today" );
			Check( c.TreeChildCountByHandle( Cat::Object, SceneEditController::HandleFor( t, t.roots[0] ) ) == 1,
			       "R: a handle made from the copy drives the OTHER getters too" );

			Check( SceneEditController::HandleFor( t, 999 ) == SceneEditController::kInvalidTreeNode,
			       "R: an out-of-range index mints the INVALID handle, not a plausible-looking one" );
			std::vector<Seed> soloSeeds;
			soloSeeds.push_back( MakeSeed( "solo", "", 1 ) );
			const SceneEditController::AuthoredTree unpub =
				SceneEditController::BuildAuthoredTree( soloSeeds );
			Check( unpub.nodes.size() == 1 && unpub.generation == 0,
			       "R: the premise -- BuildAuthoredTree leaves a tree UNPUBLISHED" );
			Check( SceneEditController::HandleFor( unpub, 0 ) == SceneEditController::kInvalidTreeNode,
			       "R: no handle is minted from a tree that was never published -- one could never "
			       "resolve, so handing one back would be a lie" );

			// VALIDITY -- the copy's handles die exactly when the copy stops
			// being the published tree.
			Check( j->SetObjectParent( "arm", 0 ), "R: `arm` detached on the live manager" );
			Check( c.TreeNodeCount( Cat::Object ) == 2, "R: a count getter refreshes and republishes" );
			Check( c.TreeNodeNameByHandle( Cat::Object, SceneEditController::HandleFor( t, 0 ) ).size() <= 1,
			       "R: a handle minted from the OLD copy no longer resolves -- validity is that "
			       "copy's generation still being the published one" );
			SceneEditController::AuthoredTree t2;
			c.ReadTree( Cat::Object, t2 );
			Check( t2.nodes.size() == 2
			    && std::string( c.TreeNodeNameByHandle( Cat::Object,
			           SceneEditController::HandleFor( t2, 0 ) ).c_str() )
			       == std::string( t2.nodes[0].name.c_str() ),
			       "R: while one minted from a FRESH copy does" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// S -- A REPLACEMENT IS NOT A NO-OP.  Name is not identity; the
	//      registration SERIAL is.
	//
	// Remove `victim` and re-add a DIFFERENT IObjectPriv under the SAME
	// name.  The rebuilt tree has the same node count, the same names, the
	// same parents, the same roots, the same child slices -- and the fresh
	// serial is still the highest, so even the sort position holds.  Every
	// member TreesEquivalent compares EXCEPT the serial says "identical".
	//
	// Without the serial the generation stands and the handle captured
	// before the swap keeps resolving, reporting `victim` -- an instance
	// the manager no longer holds.  That reads as benign because SELECTION
	// is by name, so a shell that only selects through the stale handle
	// still hits the live object; the defect is that handle validity would
	// then rest on a WEAKER relation (name equality) than the one the
	// editor enforces one layer down.  `IManager::GetItemSerial` exists to
	// draw exactly this distinction ("detect a remove + re-add under the
	// SAME name (a DIFFERENT instance)"), and SceneEditor.cpp's
	// capture/apply gate already refuses an op whose target serial moved.
	// Anything keyed on the handle rather than the name -- 4b/4c expand
	// state, a selection cache held across event-loop turns -- would
	// transfer silently onto the replacement.
	// =================================================================
	{
		const char* s = "sgnode_replace.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname AAA\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname victim\ngeometry g\nmaterial m\nposition 2 0 0\n}\n"
			"lambertian_material\n{\nname m2\nreflectance p\n}\n",   // referenced by nothing: safe to swap
			"S: replacement scene loads" );
		{
			SceneEditController c( *j, 0 );
			const int OBJ = static_cast<int>( Cat::Object );
			CheckEq( TreeDump( c, Cat::Object ), "AAA|victim", "S: the tree starts as declared" );

			const SceneEditController::TreeNodeHandle h = c.TreeRootNode( Cat::Object, 1 );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Object, h ).c_str() ), "victim",
			         "S: the captured handle names `victim` before the swap" );
			const unsigned long long genBefore = c.TreeGeneration( Cat::Object );

			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			IObjectPriv* before = om ? om->GetItem( "victim" ) : 0;
			const unsigned long long serialBefore = om ? om->GetItemSerial( "victim" ) : 0;
			Check( before != 0 && serialBefore != 0, "S: `victim` is live, with a registration serial" );
			// HOLD A REFERENCE across the swap.  RemoveItem drops the
			// manager's own ref, the object is destroyed, and the allocator
			// hands the SAME address straight back to the replacement -- so
			// without this the "different instance" premise below compares
			// two equal pointers and fails on a true statement.
			if( before ) before->addref();

			// THE REPLACEMENT.  RemoveItem drops the entry AND its serial;
			// AddObject builds a NEW IObjectPriv and registers it, taking a
			// fresh serial off the manager's counter.
			Check( om && om->RemoveItem( "victim" ), "S: `victim` removed" );
			RadianceMapConfig nilRMap;
			const double pos[3] = { 2, 0, 0 }, orient[3] = { 0, 0, 0 }, one3[3] = { 1, 1, 1 };
			Check( j->AddObject( "victim", "g", "m", 0, 0, nilRMap, pos, orient, one3, true, true ),
			       "S: ... and a DIFFERENT object re-registered under the same name" );
			const IObjectPriv* after = om ? om->GetItem( "victim" ) : 0;
			Check( after != 0 && after != before,
			       "S: the premise -- the live `victim` is a different instance now" );
			Check( om && om->GetItemSerial( "victim" ) != serialBefore,
			       "S: ... which the manager records as a new registration serial" );
			if( before ) before->release();

			// THE PREMISE: everything the tree can see about SHAPE is
			// unchanged, so only the serial can carry the difference.
			Check( c.TreeNodeCount( Cat::Object ) == 2,
			       "S: the swap left the node count alone (a count getter refreshes)" );
			Check( c.TreeRootCount( Cat::Object ) == 2, "S: ... and the root count" );
			CheckEq( TreeDump( c, Cat::Object ), "AAA|victim",
			         "S: ... and the walk is identical, name for name and position for position" );

			Check( c.TreeGeneration( Cat::Object ) != genBefore,
			       "S: the REPLACEMENT republishes -- the generation advanced on a change no "
			       "structural comparison and no name comparison can see" );
			Check( c.TreeNodeNameByHandle( Cat::Object, h ).size() <= 1,
			       "S: so the handle captured before the swap FAILS -- it does not silently transfer "
			       "onto the replacement" );
			Check( c.TreeNodeParent( Cat::Object, h ) == SceneEditController::kInvalidTreeNode,
			       "S: and neither does a parent read through it" );
			char buf[128] = { 0 };
			buf[0] = 'z';
			Check( !RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, h, buf, sizeof(buf) ),
			       "S: the C ABI refuses it too" );

			const SceneEditController::TreeNodeHandle fresh = c.TreeRootNode( Cat::Object, 1 );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Object, fresh ).c_str() ), "victim",
			         "S: control -- a handle taken AFTER the swap resolves, so the API is refusing "
			         "staleness and not refusing to work" );

			// AND THE SAME FOR A FLAT CATEGORY.  Objects get their serial for
			// free (it is already the display-order key); every other
			// manager-backed category needs a separate lookup, and without
			// this the whole of CategoryEntitySerialLocked_ would be
			// unexercised code.  `m2` is referenced by nothing, so removing
			// it fires no deleted-callback into the objects.
			Check( c.TreeNodeCount( Cat::Material ) > 0, "S: the Material tree publishes" );
			const unsigned int mi = c.TreeRootCount( Cat::Material );
			SceneEditController::TreeNodeHandle hm = SceneEditController::kInvalidTreeNode;
			for( unsigned int i = 0; i < mi; ++i ) {
				const SceneEditController::TreeNodeHandle t2h = c.TreeRootNode( Cat::Material, i );
				if( std::string( c.TreeNodeNameByHandle( Cat::Material, t2h ).c_str() ) == "m2" ) hm = t2h;
			}
			Check( hm != SceneEditController::kInvalidTreeNode, "S: `m2` has a Material row" );
			const unsigned long long matGenBefore = c.TreeGeneration( Cat::Material );
			IMaterialManager* mm = j->GetMaterials();
			Check( mm && mm->RemoveItem( "m2" ), "S: `m2` removed from the material manager" );
			Check( j->AddLambertianMaterial( "m2", "p" ),
			       "S: ... and a different material re-registered under the same name" );
			Check( c.TreeNodeCount( Cat::Material ) == mi,
			       "S: the Material tree is the same size and shape as before" );
			Check( c.TreeGeneration( Cat::Material ) != matGenBefore,
			       "S: a FLAT category's replacement republishes too -- the serial is looked up per "
			       "category, not only for Objects" );
			Check( c.TreeNodeNameByHandle( Cat::Material, hm ).size() <= 1,
			       "S: so the Material handle taken before the swap fails as well" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// T -- A PAINTER ROW'S SERIAL BELONGS TO THE ENTITY THAT ROW DENOTES.
	//
	// Category::Painter is the only category whose rows come from TWO
	// stores: the colour-painter manager and the physical-scalar-painter
	// manager (CLAUDE.md's IScalarPainter split -- material slots route by
	// physical meaning, so `uniformcolor_painter P` and `scalar_painter P`
	// are different KINDS of entity, not two spellings of one).
	// `CollectPainterUnionNames` concatenates them WITHOUT dedup, and it is
	// right not to dedup: `Cst::ChunkNamePath` keys the document index on
	// `role + "/" + name`, so both are real, separately-addressable document
	// entities and both must get a row.
	//
	// So a duplicated name produces TWO rows, and which entity a given row
	// denotes is decided by POSITION.  Round 3 derived each row's serial by
	// probing the colour manager and then the scalar one and taking the
	// first hit -- on a comment asserting a name "lives in exactly one of
	// them", which the concatenation it cited disproves.  Both rows then
	// carried the COLOUR painter's serial, so replacing the SCALAR painter
	// moved nothing the tree could see: the generation stood and a handle
	// captured before the replacement kept resolving.
	//
	// The two managers' serial counters run independently, which is what
	// makes the fixture discriminating -- the base scene declares a colour
	// painter before this one, so the colour `DUP` sits at a different
	// serial from the scalar `DUP`.
	// =================================================================
	{
		const char* s = "sgnode_painterunion.RISEscene";
		Job* j = LoadScene( s,
			"uniformcolor_painter\n{\nname DUP\ncolor 1 0 0\n}\n"
			"scalar_painter\n{\nname DUP\nvalue 0.25\n}\n",
			"T: painter-union scene loads" );
		{
			SceneEditController c( *j, 0 );
			IPainterManager*       pm = j->GetPainters();
			IScalarPainterManager* sm = j->GetScalarPainters();
			Check( pm != 0 && sm != 0, "T: both painter managers exist" );

			const unsigned long long colourSerial = pm ? pm->GetItemSerial( "DUP" ) : 0;
			const unsigned long long scalarSerial = sm ? sm->GetItemSerial( "DUP" ) : 0;
			Check( colourSerial != 0 && scalarSerial != 0,
			       "T: the premise -- BOTH managers hold an entity named `DUP`, so the name does not "
			       "identify one" );
			Check( colourSerial != scalarSerial,
			       "T: ... and their independent counters give the two different serials, so the "
			       "assertions below discriminate" );

			// Locate the two `DUP` rows.  Both are roots (Painter is flat) and
			// the union puts colour painters before scalars, so the FIRST is
			// the colour one -- a stable_sort on the name alone preserves that.
			Check( c.TreeNodeCount( Cat::Painter ) > 0, "T: the Painter tree publishes" );
			SceneEditController::AuthoredTree pt;
			c.ReadTree( Cat::Painter, pt );
			std::vector<std::size_t> dupRows;
			for( std::size_t i = 0; i < pt.nodes.size(); ++i )
				if( std::string( pt.nodes[i].name.c_str() ) == "DUP" ) dupRows.push_back( i );
			Check( dupRows.size() == 2,
			       "T: the union does NOT dedup -- both `DUP` entities get their own row" );
			if( dupRows.size() == 2 ) {
				Check( pt.nodes[ dupRows[0] ].serial == colourSerial,
				       "T: the first `DUP` row carries the COLOUR painter's serial" );
				Check( pt.nodes[ dupRows[1] ].serial == scalarSerial,
				       "T: and the second carries the SCALAR painter's -- a row's serial comes from "
				       "the manager the row came from, not from a name probe that races the two" );
			}

			// A handle on the SCALAR row, then replace the scalar entity.
			SceneEditController::TreeNodeHandle hs = SceneEditController::kInvalidTreeNode;
			if( dupRows.size() == 2 )
				hs = SceneEditController::HandleFor( pt, static_cast<unsigned int>( dupRows[1] ) );
			Check( hs != SceneEditController::kInvalidTreeNode, "T: the scalar `DUP` row has a handle" );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Painter, hs ).c_str() ), "DUP",
			         "T: which resolves before the replacement" );
			const unsigned long long genBefore = c.TreeGeneration( Cat::Painter );

			Check( sm && sm->RemoveItem( "DUP" ), "T: the SCALAR `DUP` is removed" );
			IScalarPainter* fresh = 0;
			Check( RISE_API_CreateUniformScalarPainter( &fresh, Scalar( 0.75 ) ) && fresh != 0,
			       "T: ... a different scalar painter is built" );
			if( fresh ) {
				Check( sm->AddItem( fresh, "DUP" ), "T: ... and registered under the same name" );
				fresh->release();
			}
			Check( sm && sm->GetItemSerial( "DUP" ) != scalarSerial,
			       "T: the premise -- the scalar manager records a NEW registration serial" );
			Check( pm && pm->GetItemSerial( "DUP" ) == colourSerial,
			       "T: ... while the COLOUR `DUP` is untouched, so a name probe would see no change "
			       "at all" );

			Check( c.TreeNodeCount( Cat::Painter ) == static_cast<unsigned int>( pt.nodes.size() ),
			       "T: the replacement left the Painter row count alone" );
			Check( c.TreeGeneration( Cat::Painter ) != genBefore,
			       "T: the SCALAR-side replacement REPUBLISHES -- the row that denotes it carries its "
			       "serial, so the change is visible" );
			Check( c.TreeNodeNameByHandle( Cat::Painter, hs ).size() <= 1,
			       "T: so the handle captured before it FAILS" );

			SceneEditController::AuthoredTree pt2;
			c.ReadTree( Cat::Painter, pt2 );
			bool freshResolves = false;
			for( std::size_t i = 0; i < pt2.nodes.size(); ++i ) {
				if( std::string( pt2.nodes[i].name.c_str() ) != "DUP" ) continue;
				if( std::string( c.TreeNodeNameByHandle( Cat::Painter,
					SceneEditController::HandleFor( pt2, static_cast<unsigned int>( i ) ) ).c_str() ) == "DUP" )
					freshResolves = true;
			}
			Check( freshResolves,
			       "T: control -- a handle taken AFTER the replacement resolves, so the API is "
			       "refusing staleness and not refusing to work" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// U -- A FULL RE-DERIVE INVALIDATES, EVEN THOUGH EVERY SERIAL COMES
	//      BACK IDENTICAL.
	//
	// The serial is the identity relation cases Q and S rest on, and it is
	// only an identity WITHIN ONE INSTANCE of the stores.
	// `GenericManager::m_nNextSerial` is a per-manager member starting at 0;
	// `Job::ClearAll` destroys and recreates every manager; a re-derive
	// re-registers the same document in the same order.  So a full re-derive
	// hands genuinely NEW entities the SAME serials the old ones had, every
	// row of the rebuilt tree compares equal to the published one, the
	// generation stands, and a handle minted before the rebuild resolves --
	// onto an entity that did not exist when it was minted.
	//
	// Driven through `SetSelection( SceneVariant, "(base)" )`, which is the
	// controller's own supported route into `Job::RederiveCstWithVariant`
	// (ClearAll + re-derive + rebind).  It is not the only reachable one --
	// `ApplyCstParamEdit` returning 2/3 is the fallback for every category
	// the incremental path refuses, and reopening a document into a reused
	// Job is what both GUIs do -- but it is the one a test can drive without
	// leaving the controller holding pointers into freed managers.
	//
	// The PREMISE half is what makes this a test of the rebuild counter and
	// not of something else: it asserts the serials really do come back
	// identical, so nothing else in TreesEquivalent could have caught it.
	// =================================================================
	{
		const char* s = "sgnode_rederive.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname AAA\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname BBB\ngeometry g\nmaterial m\nposition 2 0 0\n}\n",
			"U: re-derive scene loads" );
		{
			SceneEditController c( *j, 0 );
			const int OBJ = static_cast<int>( Cat::Object );
			CheckEq( TreeDump( c, Cat::Object ), "AAA|BBB", "U: the tree starts as declared" );

			const IScene* sc0 = j->GetScene();
			const IObjectManager* om0 = sc0 ? sc0->GetObjects() : 0;
			const unsigned long long serialA = om0 ? om0->GetItemSerial( "AAA" ) : 0;
			const unsigned long long serialB = om0 ? om0->GetItemSerial( "BBB" ) : 0;
			Check( serialA != 0 && serialB != 0, "U: both objects have registration serials" );

			const SceneEditController::TreeNodeHandle h = c.TreeRootNode( Cat::Object, 1 );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Object, h ).c_str() ), "BBB",
			         "U: the captured handle names `BBB` before the re-derive" );
			const unsigned long long genBefore   = c.TreeGeneration( Cat::Object );
			const unsigned long long rebuild0    = j->GetContainerRebuildCount();
			SceneEditController::AuthoredTree before;
			c.ReadTree( Cat::Object, before );
			Check( before.rebuildCount == rebuild0 && rebuild0 != 0,
			       "U: the published tree records WHICH BUILD of the stores it was read from" );

			// THE RE-DERIVE.  ClearAll + re-derive the retained document.
			Check( c.SetSelection( Cat::SceneVariant, String( "(base)" ) ),
			       "U: a scene-variant activation re-derives the document" );
			Check( j->GetContainerRebuildCount() == rebuild0 + 1,
			       "U: the premise -- the container set was rebuilt exactly once" );

			// THE PREMISE THAT MAKES THIS CASE NECESSARY.
			const IScene* sc1 = j->GetScene();
			const IObjectManager* om1 = sc1 ? sc1->GetObjects() : 0;
			Check( om1 && om1->GetItemSerial( "AAA" ) == serialA
			    && om1->GetItemSerial( "BBB" ) == serialB,
			       "U: the premise -- the rebuilt managers hand the NEW entities the SAME serials, "
			       "because each manager's counter restarted with the manager" );
			Check( c.TreeNodeCount( Cat::Object ) == 2, "U: a count getter refreshes and sees two nodes" );
			CheckEq( TreeDump( c, Cat::Object ), "AAA|BBB",
			         "U: ... and the walk is identical, name for name and position for position -- so "
			         "no row comparison could tell the trees apart" );

			Check( c.TreeGeneration( Cat::Object ) != genBefore,
			       "U: the RE-DERIVE republishes anyway -- the tree records which build of the stores "
			       "it came from, and that is the only member that moved" );
			Check( c.TreeNodeNameByHandle( Cat::Object, h ).size() <= 1,
			       "U: so the handle captured before the re-derive FAILS -- it does not silently "
			       "transfer onto the object the rebuild created" );
			Check( c.TreeNodeParent( Cat::Object, h ) == SceneEditController::kInvalidTreeNode,
			       "U: and neither does a parent read through it" );
			char buf[128] = { 0 };
			buf[0] = 'z';
			Check( !RISE_API_SceneEditController_TreeNodeNameByHandle( &c, OBJ, h, buf, sizeof(buf) ),
			       "U: the C ABI refuses it too" );

			const SceneEditController::TreeNodeHandle fresh = c.TreeRootNode( Cat::Object, 1 );
			CheckEq( std::string( c.TreeNodeNameByHandle( Cat::Object, fresh ).c_str() ), "BBB",
			         "U: control -- a handle taken AFTER the re-derive resolves" );

			// EVERY CATEGORY, not only the ones with serials.  A rebuild
			// replaces all the stores at once, so the Material tree must
			// invalidate on the same event -- this is the half that the
			// serial-less categories (Medium/Rasterizer/Film/Animation/
			// SceneVariant) depend on entirely.
			SceneEditController::AuthoredTree mt;
			c.ReadTree( Cat::Material, mt );
			Check( mt.rebuildCount == rebuild0 + 1,
			       "U: a category read after the rebuild records the NEW build, so its handles are "
			       "keyed to it too" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// V -- THE HANDLE-CHURN ASYMMETRY, pinned so it cannot move silently.
	//
	// An INCREMENTAL CST param edit drops and re-adds the affected
	// Material / Geometry / Light / Modifier entity in the same manager
	// under the same name (Cst.cpp's re-Finalize loop), which mints a fresh
	// serial -- while OBJECTS are re-pointed IN PLACE, deliberately, so the
	// raw addresses the TLAS holds stay valid, and they keep their serial.
	//
	// So a radius drag invalidates every Geometry handle on every tick and
	// the same gesture on an object's transform invalidates nothing.  That
	// is CORRECT under the identity model and harmless under the round-4
	// scope decision (ReadTree is the sanctioned multi-node surface and
	// nothing holds a handle across a turn) -- but it is surprising enough
	// that a 4b/4c author who trips over it should find it asserted rather
	// than rediscover it.
	//
	// The `code == 1` checks are load-bearing PREMISES, not decoration: a
	// code of 2 or 3 would be a full re-derive, which bumps every category
	// by design (case U), and the asymmetry is only claimed for the
	// incremental path.
	// =================================================================
	{
		const char* s = "sgnode_churn.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname obj\ngeometry g\nmaterial m\n}\n",
			"V: churn scene loads" );
		{
			SceneEditController c( *j, 0 );
			Check( c.TreeNodeCount( Cat::Geometry ) > 0, "V: the Geometry tree publishes" );
			Check( c.TreeNodeCount( Cat::Object ) > 0, "V: the Object tree publishes" );
			const unsigned long long geomGen0 = c.TreeGeneration( Cat::Geometry );
			const unsigned long long objGen0  = c.TreeGeneration( Cat::Object );
			const unsigned long long rebuild0 = j->GetContainerRebuildCount();

			// (a) a GEOMETRY param edit -- dropped and re-added, fresh serial.
			Check( j->ApplyCstParamEdit( "g", "geometry", "radius", 0, "2.0" ) == 1,
			       "V: the premise -- a geometry param edit applies INCREMENTALLY (code 1), so no "
			       "re-derive is in play" );
			Check( j->GetContainerRebuildCount() == rebuild0,
			       "V: ... and the container set was NOT rebuilt" );
			Check( c.TreeNodeCount( Cat::Geometry ) > 0, "V: a count getter refreshes the Geometry tree" );
			Check( c.TreeGeneration( Cat::Geometry ) != geomGen0,
			       "V: a Geometry entity is DROPPED AND RE-ADDED by the incremental apply, so its "
			       "serial moves and every Geometry handle dies -- on every tick of a radius drag" );

			// (b) an OBJECT param edit -- re-pointed in place, serial held.
			const IScene* sc = j->GetScene();
			const IObjectManager* om = sc ? sc->GetObjects() : 0;
			const unsigned long long objSerial0 = om ? om->GetItemSerial( "obj" ) : 0;
			Check( objSerial0 != 0, "V: `obj` has a registration serial" );
			Check( c.TreeNodeCount( Cat::Object ) > 0, "V: the Object tree is current before the edit" );
			const unsigned long long objGen1 = c.TreeGeneration( Cat::Object );
			Check( objGen1 == objGen0,
			       "V: ... and the geometry edit did not disturb it either" );
			const unsigned long long rev0 = j->GetCstHeadVersion().revision;
			Check( j->ApplyCstParamEdit( "obj", "object", "position", 0, "1 0 0" ) == 1,
			       "V: the premise -- an object param edit also applies incrementally" );
			Check( j->GetContainerRebuildCount() == rebuild0,
			       "V: ... with no rebuild" );
			Check( j->GetCstHeadVersion().revision != rev0,
			       "V: ... and the edit REALLY LANDED (the head revision moved), so the "
			       "no-republish assertion below is not vacuously true of a rejected edit" );
			Check( c.TreeNodeCount( Cat::Object ) > 0, "V: a count getter refreshes the Object tree" );
			const IObjectManager* om2 = j->GetScene() ? j->GetScene()->GetObjects() : 0;
			Check( om2 && om2->GetItemSerial( "obj" ) == objSerial0,
			       "V: an OBJECT is re-pointed IN PLACE, so its serial is unchanged" );
			Check( c.TreeGeneration( Cat::Object ) == objGen1,
			       "V: ... and the Object tree does NOT republish -- the asymmetry is real, and a "
			       "shell must not assume one category's churn rate is the other's" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// W -- A HANDLE FROM ANOTHER CONTROLLER DOES NOT RESOLVE.
	//
	// The generation tag is compared against the CURRENTLY PUBLISHED
	// generation for the category, and nothing in the comparison names a
	// controller.  While the counter was a per-controller member seeded at
	// 1, controller A's first published tree and controller B's first
	// published tree both carried generation 1, so a handle minted on A
	// decoded cleanly against B and named B's node at the same index --
	// with a valid-looking name coming back.  Round 4 moved the counter to
	// a process-global atomic, so no two published trees anywhere share a
	// generation.
	//
	// Not reachable in the Mac GUI today (the bridge is torn down before
	// its replacement is built, so two controllers are never live at once),
	// but a 4b model that caches handles across a document reload is the
	// realistic shape, and `HandleFor`'s doc used to SELL the aliasing as a
	// feature ("does not care which controller the copy came from").
	// =================================================================
	{
		const char* sA = "sgnode_xctlA.RISEscene";
		const char* sB = "sgnode_xctlB.RISEscene";
		Job* jA = LoadScene( sA,
			"standard_object\n{\nname AONLY\ngeometry g\nmaterial m\n}\n",
			"W: controller-A scene loads" );
		Job* jB = LoadScene( sB,
			"standard_object\n{\nname BONLY\ngeometry g\nmaterial m\n}\n",
			"W: controller-B scene loads" );
		{
			SceneEditController cA( *jA, 0 );
			SceneEditController cB( *jB, 0 );
			// Enter through a count getter on BOTH: only the count getters
			// refresh, and a fresh controller has published nothing yet.
			Check( cA.TreeRootCount( Cat::Object ) == 1 && cB.TreeRootCount( Cat::Object ) == 1,
			       "W: both controllers publish a single-root tree" );
			const SceneEditController::TreeNodeHandle hA = cA.TreeRootNode( Cat::Object, 0 );
			CheckEq( std::string( cA.TreeNodeNameByHandle( Cat::Object, hA ).c_str() ), "AONLY",
			         "W: the handle names A's node on A" );
			CheckEq( std::string( cB.TreeNodeNameByHandle( Cat::Object, cB.TreeRootNode( Cat::Object, 0 ) ).c_str() ),
			         "BONLY",
			         "W: the premise -- B has its own single root at the same INDEX, so a decoded "
			         "cross-controller handle would name it" );
			Check( cA.TreeGeneration( Cat::Object ) != cB.TreeGeneration( Cat::Object ),
			       "W: the two controllers' trees do not share a generation" );
			Check( cB.TreeNodeNameByHandle( Cat::Object, hA ).size() <= 1,
			       "W: so A's handle FAILS on B rather than naming `BONLY`" );
			Check( cB.TreeNodeParent( Cat::Object, hA ) == SceneEditController::kInvalidTreeNode
			    && cB.TreeChildCountByHandle( Cat::Object, hA ) == 0,
			       "W: ... through the other getters too" );
		}
		jA->release();
		jB->release();
		std::remove( sA );
		std::remove( sB );
	}

	// =================================================================
	// X -- SetPrimaryAcceleration REPLACES the ObjectManager without going
	//      through InitializeContainers, so it is the SECOND site that has to
	//      bump the container-rebuild count.  Round 5 found it missing: the
	//      fresh manager restarts its serial counter at 0, so the next objects
	//      registered get serials byte-identical to the ones just destroyed --
	//      the exact precondition the rebuild count exists to detect.
	//
	//      No GUI can reach this today (the callers are the CLI console, the
	//      Blender bridge at job-build time, and 3DSMax, none of which holds a
	//      SceneEditController), which is why it is pinned HERE, at the Job,
	//      rather than through a controller: the guard has to survive whoever
	//      wires a controller up to an accelerator swap later.
	// =================================================================
	{
		const char* s = "sgnode_accel.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname AAA\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname BBB\ngeometry g\nmaterial m\n}\n",
			"X: two-object scene loads" );

		const unsigned long long before = j->GetContainerRebuildCount();

		// The PREMISE, asserted so the case cannot pass for the wrong reason:
		// the live manager really is handing out non-zero serials right now.
		const IObjectManager* om0 = j->GetScene()->GetObjects();
		Check( om0 && om0->GetItemSerial( "BBB" ) != 0,
		       "X: the premise -- BBB has a real, non-zero serial before the swap" );

		j->SetPrimaryAcceleration( true, false, 4, 32 );

		const IObjectManager* om1 = j->GetScene()->GetObjects();
		Check( om1 && om1 != om0,
		       "X: the premise -- SetPrimaryAcceleration really did replace the manager" );
		Check( om1 && om1->GetItemSerial( "BBB" ) == 0,
		       "X: ... and the replacement starts its serials over, so the NEXT "
		       "registrations would reproduce the destroyed ones" );
		Check( j->GetContainerRebuildCount() != before,
		       "X: so the container-rebuild count MUST move -- it is the only thing "
		       "that can tell the tree snapshot the serials were reset" );

		j->release();
		std::remove( s );
	}

	// =================================================================
	// Y -- THE SHAPE THE OUTLINER SHELLS RESHAPE `ReadTree` INTO (step 4b).
	//
	//      4b turns one AuthoredTree into {name, parent, children} rows plus
	//      a roots list and draws exactly the nodes a roots-then-children
	//      walk reaches.  Everything else in this file asserts the tree's
	//      CONTENT against a golden dump; this case asserts its STRUCTURE,
	//      because the two failures a shell cannot see are structural: a node
	//      the walk never reaches is a row that is simply absent, and a node
	//      it reaches twice is a row drawn twice -- on screen both read as a
	//      scene that is different from what the author wrote, not as a bug.
	//
	//      Run over every shape the assembler can produce, INCLUDING its two
	//      hostile inputs: a dangling parent and a cycle are exactly where a
	//      totality claim would break, and neither is reachable through
	//      IObjectManager::SetObjectParent, so the pure seam is the only way
	//      to drive them.  See TreeShapeViolation for what is checked and for
	//      the honest scope (this pins the CONTRACT, not either shell).
	// =================================================================
	{
		const char* s = "sgnode_shape.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname hub\n}\n"
			"standard_object\n{\nname arm\nparent hub\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname hand\nparent arm\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname B\nparent S\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nparent hub\nsource S\ncount_u 2\nposition expr(i*3) 0 0\n}\n",
			"Y: nested + instanced scene loads" );
		{
			SceneEditController c( *j, 0 );
			// A hierarchy, a fold, and a FLAT category -- the flat one matters
			// because 4a models it as N roots with no children precisely so the
			// shells need no per-category branch, which is a claim about THIS
			// partition holding there too.
			const Cat cats[4] = { Cat::Object, Cat::Material, Cat::Painter, Cat::Geometry };
			for( int f = 0; f < 4; ++f ) {
				SceneEditController::AuthoredTree t;
				c.ReadTree( cats[f], t );
				Check( t.nodes.size() > 0, "Y: the fixture category is non-empty (not vacuously shaped)" );
				CheckEq( TreeShapeViolation( t ), "",
				         "Y: a live category's tree is a partition its shell can draw whole" );
			}
			// The premise for the Object row: this really is a MULTI-LEVEL tree
			// with a fold in it, so the walk above had something to get wrong.
			SceneEditController::AuthoredTree t;
			c.ReadTree( Cat::Object, t );
			CheckEq( TreeDump( c, Cat::Object ),
			         "hub|  arm|    hand|  I|S|  B",
			         "Y: the premise -- three levels, a counted instance folded to one node" );
			Check( t.roots.size() == 2 && t.nodes.size() == 6,
			       "Y: ... two roots over six nodes, so roots+children is a real partition" );
		}
		j->release();
		std::remove( s );

		// The two hostile inputs, straight at the pure assembler.
		{
			std::vector<Seed> seeds;
			seeds.push_back( MakeSeed( "kept",    "ghost", 1 ) );   // dangling -> root
			seeds.push_back( MakeSeed( "child",   "kept",  2 ) );
			CheckEq( TreeShapeViolation( SceneEditController::BuildAuthoredTree( seeds ) ), "",
			         "Y: a DANGLING parent still yields a whole, once-each partition" );
		}
		{
			std::vector<Seed> seeds;
			seeds.push_back( MakeSeed( "a", "c", 1 ) );
			seeds.push_back( MakeSeed( "b", "a", 2 ) );
			seeds.push_back( MakeSeed( "c", "b", 3 ) );
			seeds.push_back( MakeSeed( "self", "self", 4 ) );
			const SceneEditController::AuthoredTree cyc =
				SceneEditController::BuildAuthoredTree( seeds );
			Check( cyc.nodes.size() == 4,
			       "Y: the premise -- a 3-cycle and a self-parent both kept every node" );
			CheckEq( TreeShapeViolation( cyc ), "",
			         "Y: ... and a BROKEN cycle is still a partition -- the outliner draws each of "
			         "those nodes exactly once, which is the whole reason the break exists" );
		}
	}

	// =================================================================
	// Z -- A ROW THE TREE OFFERS MUST INSPECT (step 4b review).
	//
	//      Case H established that a COUNTED instancing chunk gets a node
	//      of its own because it is the row that HAS a chunk to edit.  That
	//      node's whole justification is editability, and until this case
	//      nothing checked it: `I count_u 2` puts `I[0,0]` / `I[1,0]` in the
	//      object manager and NO `I`, so the properties panel's Object arm --
	//      which inspects the LIVE object -- found nothing and published an
	//      EMPTY row set.  The shell drew an Object section with zero rows:
	//      a row that selects, highlights, and then inspects to nothing.
	//      Pre-4b the outliner listed `I[0,0]` / `I[1,0]` instead, and those
	//      DID inspect, so the tree traded a working panel for a tidier list.
	//
	//      The honest inspection of a synthesized node is its CHUNK, which is
	//      what the generic descriptor+CST surface reports.  Asserted through
	//      the CONTROLLER (SetSelection -> RefreshProperties -> the panel
	//      accessors), not through CstIntrospection directly, because the
	//      defect was in the arm's routing and a direct call would have
	//      passed all along.  Both shells read these accessors, so this is
	//      the one place the fix could go that covers 4b and 4c at once.
	// =================================================================
	{
		const char* s = "sgnode_synthpanel.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i*3) 0 0\n}\n",
			"Z: counted-instance scene loads" );
		{
			SceneEditController c( *j, 0 );

			// The PREMISE, both halves: the tree offers a row named `I`, and
			// the object manager has never heard of it.  Without the second
			// half the case would not be testing the synthesized path at all.
			SceneEditController::AuthoredTree t;
			c.ReadTree( Cat::Object, t );
			bool haveI = false;
			for( std::size_t i = 0; i < t.nodes.size(); ++i )
				if( std::string( t.nodes[i].name.c_str() ) == "I" ) haveI = true;
			Check( haveI, "Z: the premise -- the authored tree offers a row for the instancing chunk `I`" );
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om != 0 && om->GetItem( "I" ) == 0,
			       "Z: ... and NO live object carries that name, so live introspection has nothing "
			       "to inspect" );
			Check( om != 0 && om->GetItem( "I[0,0]" ) != 0 && om->GetItem( "I[1,0]" ) != 0,
			       "Z: ... while the repetitions the fold hid ARE live" );

			// THE DEFECT: selecting the row and refreshing must produce rows.
			Check( c.SetSelection( Cat::Object, String( "I" ) ), "Z: the synthesized row selects" );
			c.RefreshProperties();
			const unsigned int nRows = c.PropertyCountFor( Cat::Object );
			Check( nRows > 0,
			       "Z: selecting a SYNTHESIZED node fills the Object panel -- an empty row set here is "
			       "the outliner offering a row that inspects to nothing" );
			Check( c.PropertyCount() == nRows,
			       "Z: ... and the PRIMARY snapshot the single-panel shells read carries the same rows" );

			// The rows are the CHUNK's, and they carry its AUTHORED values --
			// a non-empty row set built from some other entity would pass the
			// count check above.  `count_u`/`source` say which chunk, and the
			// values say it is read from the document rather than defaulted.
			std::string countU = "<absent>", source = "<absent>", boundsRow = "<absent>",
			            typeRow = "<absent>";
			for( unsigned int i = 0; i < nRows; ++i ) {
				const std::string rn( c.PropertyNameFor( Cat::Object, i ).c_str() );
				if( rn == "count_u" ) countU = std::string( c.PropertyValueFor( Cat::Object, i ).c_str() );
				if( rn == "source"  ) source  = std::string( c.PropertyValueFor( Cat::Object, i ).c_str() );
				if( rn == "Bounds"  ) boundsRow = "present";
				if( rn == "chunk_type" ) typeRow = std::string( c.PropertyValueFor( Cat::Object, i ).c_str() );
			}
			CheckEq( countU, "2", "Z: the rows are the INSTANCING CHUNK's -- `count_u` reads what the "
			                      "author wrote" );
			CheckEq( source, "S", "Z: ... and so does `source`" );
			CheckEq( typeRow, "standard_object",
			         "Z: ... and the leading row names the chunk keyword, which is how the generic "
			         "CST surface labels what it is showing" );

			// CONTROL: a LIVE object still inspects through the LIVE path.
			// Without this the fix could have replaced live introspection
			// wholesale and every assertion above would still pass.
			//
			// The discriminator is NOT the descriptor params -- ObjectIntrospection
			// ALREADY emits the whole standard_object param set for a live object,
			// which is why the synthesized panel looks familiar rather than alien.
			// It is the two rows only a LIVE object can have: `Name` and `Bounds`
			// are computed from the instantiated object (its world extent), and no
			// chunk text can produce them.
			CheckEq( boundsRow, "<absent>",
			         "Z: the synthesized panel has NO live-only row -- there is no instantiated object "
			         "to measure" );
			Check( c.SetSelection( Cat::Object, String( "S" ) ), "Z: a live object selects" );
			c.RefreshProperties();
			bool liveHasBounds = false, liveHasTypeRow = false;
			const unsigned int nLive = c.PropertyCountFor( Cat::Object );
			for( unsigned int i = 0; i < nLive; ++i ) {
				const std::string rn( c.PropertyNameFor( Cat::Object, i ).c_str() );
				if( rn == "Bounds" ) liveHasBounds = true;
				if( rn == "chunk_type" ) liveHasTypeRow = true;
			}
			Check( nLive > 0, "Z: the control -- a LIVE object still fills the panel" );
			Check( liveHasBounds && !liveHasTypeRow,
			       "Z: ... through the LIVE introspection path, not the chunk one -- it has the "
			       "live-only `Bounds` row and not the CST surface's `chunk_type` row, so the fallback "
			       "really is only a fallback" );

			// AND THE ROWS WORK.  The panel offers them as editable; an edit
			// that silently did nothing would be a worse defect than the empty
			// panel it replaced.  Editing the chunk's `count_u` re-derives the
			// array, which the live manager reports.
			Check( c.SetSelection( Cat::Object, String( "I" ) ), "Z: re-select the synthesized row" );
			Check( c.SetPropertyForCategory( Cat::Object, String( "count_u" ), String( "3" ) ),
			       "Z: an edit to a synthesized node's chunk row APPLIES -- the SceneEdit ops the "
			       "Object arm normally uses all address a live object, and there is none" );
			// The re-derive replaces the manager, so re-read it.
			const IScene* sc2 = j->GetScene();
			IObjectManager* om2 = sc2 ? const_cast<IObjectManager*>( sc2->GetObjects() ) : 0;
			Check( om2 != 0 && om2->GetItem( "I[2,0]" ) != 0,
			       "Z: ... and the array really grew -- a third repetition exists" );
		}
		j->release();
		std::remove( s );
	}

	// =================================================================
	// AA -- A SYNTHESIZED ENTRY RESOLVES TO THE ROW THAT REPRESENTS IT.
	//
	//      Cases G and H established the fold: `I.X` and `I[1,0]` are not
	//      rows, `I` is.  Selection is by NAME, and a VIEWPORT PICK can only
	//      ever name the LIVE entry its ray hit -- so after a pick on any
	//      copy of an instanced array the selection held a name no row
	//      answered to, and both outliners highlighted nothing.  87 step 4b
	//      opened that gap deliberately and left it open.
	//
	//      The fold applied here is PRESENTATIONAL and one-directional:
	//      `ResolveTreeRowName` answers which ROW to highlight, while
	//      `GetSelectionName` keeps naming the clicked copy -- which is what
	//      the property panel inspects, what the gizmo attaches to, and what
	//      the viewport chrome shows.  The last assertion in each block is
	//      what pins that apart: fold the selection itself and the copy is
	//      unreachable, and for a COUNTED chunk (whose row has no live object
	//      at all) the gizmo would have nothing to attach to rather than
	//      merely the wrong thing.
	//
	//      The authority for "what is a row" is TREE MEMBERSHIP, not a
	//      restated fold rule -- so the two shapes where an entry with
	//      provenance is nonetheless its OWN row (the step-3a COLLAPSE case,
	//      and case M's refused fold) must resolve to THEMSELVES, and they
	//      are checked here rather than assumed.
	// =================================================================
	{
		// -- counted array: `I[1,0]` -> `I`, whose row has NO live object.
		const char* s = "sgnode_rowresolve_counted.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname X\nparent S\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i*3) 0 0\n}\n",
			"AA: counted-instance scene loads" );
		{
			SceneEditController c( *j, 0 );
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			// The PREMISE: these are exactly the names a viewport pick can
			// return, and none of them is a row.
			Check( om && om->GetItem( "I[1,0]" ) && om->GetItem( "I[1,0].X" ) && om->GetItem( "I" ) == 0,
			       "AA: the premise -- the repetitions are live, the chunk they fold into is not" );
			CheckEq( TreeDump( c, Cat::Object ), "S|  X|I",
			         "AA: ... and the tree draws the chunk, not the repetitions" );

			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "I[1,0]" ) ).c_str() ), "I",
			         "AA: a picked REPETITION resolves to the instancing chunk's row" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "I[1,0].X" ) ).c_str() ), "I",
			         "AA: ... and so does a picked CLONE INSIDE a repetition" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "S" ) ).c_str() ), "S",
			         "AA: an ordinary object resolves to itself" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "X" ) ).c_str() ), "X",
			         "AA: ... including one that is a source subtree member -- `source` copies, and "
			         "the original is still its own row" );

			// Through the SELECTION, which is the path a viewport pick takes.
			Check( c.SetSelection( Cat::Object, String( "I[1,0]" ) ),
			       "AA: the picked repetition selects" );
			CheckEq( std::string( c.SelectionRowName().c_str() ), "I",
			         "AA: the outliner highlights the chunk's row -- an EMPTY or unfolded answer here "
			         "is the 4b gap: a pick that highlights nothing" );
			CheckEq( std::string( c.GetSelectionName().c_str() ), "I[1,0]",
			         "AA: while the SELECTION still names the clicked copy -- the panel, the gizmo and "
			         "the viewport chrome all read this, and folding it would move an edit onto every "
			         "copy at once" );
			Check( om && om->GetItem( c.GetSelectionName().c_str() ) != 0,
			       "AA: ... and it still names a LIVE object, which is what the gizmo needs: the row "
			       "it highlights has none" );

			// THE C ABI, which is what the two shells actually call -- neither
			// bridge can reach `SelectionRowName()` directly.  Case K's
			// convention for every `RISE_API_SceneEditController_*` wrapper:
			// one real call plus the null-controller contract.  Untested, the
			// wrapper is one copy-paste away from calling `GetSelectionName`
			// (it sits four lines above it in RISE_API.cpp and does almost the
			// same thing), which would silently restore the exact "a pick
			// highlights nothing" defect this whole case exists to prevent --
			// with every C++-side assertion above still green.
			{
				char buf[128] = { 0 };
				Check( RISE_API_SceneEditController_GetSelectionRowName( &c, buf, sizeof(buf) )
				    && std::string( buf ) == "I",
				       "AA: the C ABI reports the FOLDED row name -- this, not the C++ accessor, is "
				       "what both outliners compare against their rows" );
				char raw[128] = { 0 };
				Check( RISE_API_SceneEditController_GetSelectionName( &c, raw, sizeof(raw) )
				    && std::string( raw ) == "I[1,0]",
				       "AA: ... while the C ABI's SELECTION accessor still reports the clicked copy, "
				       "so the two wrappers have not been collapsed into one" );
				Check( !RISE_API_SceneEditController_GetSelectionRowName( 0, buf, sizeof(buf) ),
				       "AA: null controller -> failure, not a name" );
			}
		}
		j->release();
		std::remove( s );
	}
	{
		// -- subtree clone (no count): `I.X` -> `I`, whose row IS live.
		const char* s = "sgnode_rowresolve_subtree.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname X\nparent S\ngeometry g\nmaterial m\nposition 0 1 0\n}\n"
			"standard_object\n{\nname I\nsource S\nposition 5 0 0\n}\n",
			"AA: subtree-instance scene loads" );
		{
			SceneEditController c( *j, 0 );
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om && om->GetItem( "I.X" ) != 0,
			       "AA: the premise -- `I.X` really was synthesized, so the fold has something to fold" );

			// AND THE COLD-START PREMISE, which is why this block deliberately
			// does NOT call TreeDump/ReadTree first.  `ResolveTreeRowName` does
			// not refresh the tree (both shells re-read the selection every
			// frame and gate their tree PULL on the scene epoch, so a refresh
			// here would put an O(n) rebuild back on the per-frame path); its
			// ONE exception is a category nothing has published yet, which
			// publishes once so the accessor is usable on its own.  Nothing
			// else in this file exercises that branch -- without the branch,
			// the assertion below resolves to an unchanged `I.X`, because every
			// membership test would run against a never-published tree.
			//
			// So do not "tidy" a TreeDump call into the top of this block for
			// parity with the others: that silently deletes this coverage.
			Check( c.TreeGeneration( Cat::Object ) == 0,
			       "AA: the premise -- NOTHING has published this category's tree yet, so the next "
			       "call takes the cold-start self-publish branch" );

			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "I.X" ) ).c_str() ), "I",
			         "AA: a cloned subtree member resolves to the instance row -- from a COLD "
			         "controller, i.e. through the self-publish branch" );
			Check( c.TreeGeneration( Cat::Object ) != 0,
			       "AA: ... and that call really did publish the tree it then asked about" );

			// THE COLLAPSE CASE.  It returns through the SAME fast path as an
			// ordinary object -- `I` is already a row, so the membership test
			// answers before provenance is ever consulted -- and that IS the
			// point: "already a row" is the question, and having provenance
			// never enters into it.  (The chain walk's own `next == cur` guard
			// is a SECOND, independent protection for the same shape, reached
			// only when a collapse entry turns up as a fold TARGET, as `I.X`
			// above does.  No single mutation isolates this assertion; it is
			// here to pin the contract, not to discriminate.)
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "I" ) ).c_str() ), "I",
			         "AA: the COLLAPSE-case instance resolves to ITSELF -- it has provenance, and it "
			         "is still the row" );
		}
		j->release();
		std::remove( s );
	}
	{
		// -- case M's REFUSED fold: the repetitions are their own rows, so
		//    they must resolve to themselves and NOT to the unrelated live
		//    object whose name their provenance still records.  Same
		//    two-keyspace witness as M, and see M for why it is the gltf
		//    import rather than a nameless chunk.
		const char* s = "sgnode_rowresolve_unfold.RISEscene";
		Job* j = LoadScene( s,
			std::string( "standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n" )
			+ "gltf_import\n{\nfile scenes/Tests/Geometry/assets/Box.glb\nname_prefix P\n}\n"
			+ "standard_object\n{\nname P.obj.n1.p0\nsource S\ncount_u 2\n}\n",
			"AA: the two-keyspace scene loads" );
		{
			SceneEditController c( *j, 0 );
			CheckEq( TreeDump( c, Cat::Object ),
			         "S|P.obj.n1.p0|P.obj.n1.p0[0,0]|P.obj.n1.p0[1,0]",
			         "AA: the premise -- the fold was REFUSED, so the repetitions are their own rows" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "P.obj.n1.p0[1,0]" ) ).c_str() ),
			         "P.obj.n1.p0[1,0]",
			         "AA: a repetition whose fold was refused resolves to ITSELF -- following its "
			         "provenance would highlight the unrelated same-named object instead" );
		}
		j->release();
		std::remove( s );
	}
	{
		// -- THE ITERATED CHAIN, AND ITS CYCLE GUARD.
		//
		//    `ResolveTreeRowName` walks the fold chain with a VISITED SET
		//    rather than a hop budget, and the header sells that as
		//    "cycle-proof, and a future expansion that folds one synthesized
		//    entry into another still resolves".  NOTHING REACHABLE THROUGH
		//    SCENE TEXT EXERCISES IT: `ExpandSourceInstance` records the
		//    IMMEDIATE instancing chunk for every entry it mints (Cst.cpp's
		//    two `SetObjectProvenance` calls), so even `I[i,j].X` is one hop,
		//    and the loop provably never takes its second iteration.  Left
		//    uncovered, the walk is write-only code: "this loop only ever runs
		//    once, simplify it" is a plausible future edit that would break the
		//    documented contract with the whole suite green (measured -- a
		//    single-lookup rewrite passes 301/301).
		//
		//    So drive the provenance map DIRECTLY.  That is the honest way to
		//    test a contract about inputs the scene language cannot yet
		//    produce: `IObjectManager::SetObjectProvenance` is the same public
		//    setter the derive uses, and re-pointing it is exactly what a
		//    future expansion would do.  Both hops here are REAL live entries,
		//    so nothing about the fixture is fictional except the wiring.
		const char* s = "sgnode_rowresolve_chain.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i*3) 0 0\n}\n",
			"AA: chain-walk scene loads" );
		{
			SceneEditController c( *j, 0 );
			// Publish FIRST: everything below depends on `I` being a row in the
			// published tree, and on later calls being WARM (see the
			// no-refresh assertion at the end).
			CheckEq( TreeDump( c, Cat::Object ), "S|I", "AA: the premise -- `I` is a row, the two "
			                                            "repetitions are not" );
			const unsigned long long gen = c.TreeGeneration( Cat::Object );
			Check( gen != 0, "AA: ... and the tree really is published" );

			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om != 0, "AA: the object manager is reachable" );

			// TWO HOPS: `I[1,0]` -> `I[0,0]` -> `I`.  The middle hop is a live
			// entry that is NOT a row, so a single-hop resolver stops there,
			// fails the membership test, and hands back the unfolded name.
			Check( om && om->SetObjectProvenance( "I[1,0]", "I[0,0]", "S" ),
			       "AA: re-point the first entry's provenance at the second" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "I[1,0]" ) ).c_str() ), "I",
			         "AA: a TWO-HOP fold chain resolves to the row at its end -- a resolver that took "
			         "only one hop would stop on `I[0,0]`, which is not a row, and give up" );

			// A CYCLE terminates and degrades, rather than spinning.  Neither
			// name is a row, so there is no row to find; the guard's job is to
			// make that a RETURN and not a hang.  (A hang shows up as this test
			// never finishing, which is why the assertion is worth having even
			// though its expected value is "unchanged".)
			Check( om && om->SetObjectProvenance( "I[0,0]", "I[1,0]", "S" ),
			       "AA: close the loop -- the two entries now name each other" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "I[0,0]" ) ).c_str() ),
			         "I[0,0]",
			         "AA: a fold CYCLE terminates and hands back the unfolded name -- the visited set "
			         "is what stops the walk instead of spinning on it" );

			// AND THE WARM CALLS DID NOT REFRESH.  Re-pointing provenance
			// changes what BuildObjectTreeSeedsLocked_ would fold, so a
			// resolver that refreshed unconditionally would have republished a
			// DIFFERENT tree by now and moved the generation.  This is the
			// mirror of the cold-start assertion in the subtree block: that one
			// proves the publish happens when nothing is published, this one
			// proves it does NOT happen when something is -- which is the
			// per-frame cost guarantee both shells depend on.
			Check( c.TreeGeneration( Cat::Object ) == gen,
			       "AA: the warm resolves did NOT refresh the tree -- an unconditional refresh here "
			       "would put an O(n) rebuild back on every preview frame in the Qt shell" );
		}
		j->release();
		std::remove( s );
	}
	{
		// -- THE RENDER-OWNS-SCENE GUARD.  Resolution reads the LIVE object
		//    manager for provenance, so it must refuse to touch it once the
		//    render owns the scene -- the same rule case J pins for the tree
		//    snapshot, and the same mechanism: `PrepareForDestruction` is the
		//    one public entry that raises the flag WITHOUT also holding
		//    mMutex, so what this separates is the FLAG, not the try_lock
		//    behind it.
		//
		//    This discriminates precisely because the unguarded code would
		//    still get the RIGHT answer: with no competing lock holder the
		//    chain walk runs uncontended and folds `I[1,0]` to `I` quite
		//    happily.  Deleting the guard therefore does not break the fold --
		//    it breaks the refusal, silently reintroducing a live-manager read
		//    on the one path that must not make one.  Only an assertion that
		//    demands the DEGRADED answer can tell those apart.
		const char* s = "sgnode_rowresolve_renderowns.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname I\nsource S\ncount_u 2\nposition expr(i*3) 0 0\n}\n",
			"AA: render-owns-scene scene loads" );
		{
			SceneEditController c( *j, 0 );
			// Publish the tree and prove the fold WORKS here, so the refusal
			// below is a change of behaviour and not this fixture's baseline.
			CheckEq( TreeDump( c, Cat::Object ), "S|I", "AA: the premise -- the array is folded" );
			Check( c.SetSelection( Cat::Object, String( "I[1,0]" ) ), "AA: the repetition selects" );
			CheckEq( std::string( c.SelectionRowName().c_str() ), "I",
			         "AA: the premise -- while the controller owns the scene, it folds normally" );

			Check( c.PrepareForDestruction(), "AA: the controller is prepared for destruction" );
			Check( c.ForTest_RenderOwnsScene(), "AA: ... which leaves the render owning the scene" );

			CheckEq( std::string( c.SelectionRowName().c_str() ), "I[1,0]",
			         "AA: once the render owns the scene, resolution DEGRADES to the unfolded name "
			         "rather than reading the live manager for provenance -- the caller highlights "
			         "nothing for a frame, which is the safe answer" );
		}
		j->release();
		std::remove( s );
	}
	{
		// -- the degradations.  Every one of these must hand back what it was
		//    given: the caller then highlights nothing, which is the
		//    pre-existing behaviour.  A resolver that guessed here would move
		//    a highlight onto a WRONG row, which is worse than none.
		const char* s = "sgnode_rowresolve_degrade.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname I\nsource S\ncount_u 2\n}\n",
			"AA: degradation scene loads" );
		{
			SceneEditController c( *j, 0 );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String( "ghost" ) ).c_str() ), "ghost",
			         "AA: a name no entity carries comes back unchanged" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Object, String() ).c_str() ), "",
			         "AA: an EMPTY selection stays empty -- `section open, no row picked` must not "
			         "acquire a row" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::None, String( "I[1,0]" ) ).c_str() ), "I[1,0]",
			         "AA: Category::None resolves nothing" );
			CheckEq( std::string( c.ResolveTreeRowName( Cat::Light, String( "I[1,0]" ) ).c_str() ), "I[1,0]",
			         "AA: and a non-Object category resolves nothing -- only Objects are synthesized" );
			// A selection in another category must not be folded through the
			// Object tree either: SelectionRowName carries the CATEGORY.
			Check( c.SetSelection( Cat::Light, String( "I[1,0]" ) ), "AA: a Light selection is taken" );
			CheckEq( std::string( c.SelectionRowName().c_str() ), "I[1,0]",
			         "AA: SelectionRowName resolves within the SELECTED category, not always Object" );
		}
		j->release();
		std::remove( s );
	}

	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
