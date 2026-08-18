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
//         the DOCUMENT's `name`-param keyspace, and an unnamed chunk puts
//         `noname` in the first and nothing in the second.
//    N -- a STALE handle FAILS rather than resolving to a different
//         object, through both the C++ and the C-ABI surface.
//    O -- a read is TRANSACTIONAL (ReadTree copies the whole tree under
//         one lock hold), and an idle refresh neither republishes nor
//         invalidates an outstanding handle.
//    P -- the sibling sort's tie-break-by-name branch.
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
//! TreeRootNode / TreeChildNode / TreeNodeName / TreeNodeParent -- and
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
		out += c.TreeNodeName( cat, n ).c_str();
		if( ++emitted > total ) { out += "|<OVERRUN>"; break; }
		const unsigned int kids = c.TreeChildCount( cat, n );
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
					if( std::string( c.TreeNodeName( cat, node ).c_str() )
					 != std::string( c.CategoryEntityName( cat, i ).c_str() ) ) sameNames = false;
					if( c.TreeChildCount( cat, node ) != 0 ) anyChildren = true;
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
	// THE FIXTURE IS THE TEST HERE.  An earlier version used just
	// `root` + `kid`, where `root` is the only root AND the
	// first-registered object, so its node index is 0 -- and 0 is also
	// what a wrapper that simply zeroed its out-parameter would produce.
	// Two numbering schemes coincided, and `*outNode = 0;` in
	// TreeRootNode, and `*outParent = 0;` in TreeNodeParent, both left the
	// suite fully green.  (Verified; the same mutation on TreeChildNode
	// WAS caught, because `kid`'s child handle is 1.)  So the fixture now
	// declares `aaa` FIRST: the root under test is the SECOND root and the
	// second-registered node, `kid`'s parent handle is not 0, and neither
	// coincides with a zeroed out-param.
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
			Check( RISE_API_SceneEditController_TreeNodeName( &c, OBJ, firstRoot, buf, sizeof(buf) )
			    && std::string( buf ) == "aaa", "K: root 0 is `aaa` -- the first-registered object" );

			unsigned long long rootNode = 0xDEADull;
			Check( RISE_API_SceneEditController_TreeRootNode( &c, OBJ, 1, &rootNode ),
			       "K: root 1 resolves" );
			buf[0] = 0;
			Check( RISE_API_SceneEditController_TreeNodeName( &c, OBJ, rootNode, buf, sizeof(buf) )
			    && std::string( buf ) == "root", "K: root 1's name comes back as `root`" );
			Check( rootNode != firstRoot,
			       "K: the two roots have DIFFERENT handles -- TreeRootNode is not answering a constant" );

			Check( RISE_API_SceneEditController_TreeChildCount( &c, OBJ, rootNode ) == 1,
			       "K: `root` has one child" );
			Check( RISE_API_SceneEditController_TreeChildCount( &c, OBJ, firstRoot ) == 0,
			       "K: `aaa` has none -- the child count is per node, not a constant" );
			unsigned long long kidNode = 0xDEADull;
			Check( RISE_API_SceneEditController_TreeChildNode( &c, OBJ, rootNode, 0, &kidNode ),
			       "K: child 0 resolves" );
			buf[0] = 0;
			Check( RISE_API_SceneEditController_TreeNodeName( &c, OBJ, kidNode, buf, sizeof(buf) )
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
			Check( !RISE_API_SceneEditController_TreeNodeName( &c, OBJ, 999, buf, sizeof(buf) ),
			       "K: an unknown handle has no name" );

			// Null-controller hardening, the same contract every other
			// RISE_API_SceneEditController_* getter carries.
			Check( RISE_API_SceneEditController_TreeNodeCount( 0, OBJ ) == 0, "K: null controller -> 0 nodes" );
			Check( RISE_API_SceneEditController_TreeRootCount( 0, OBJ ) == 0, "K: null controller -> 0 roots" );
			Check( RISE_API_SceneEditController_TreeGeneration( 0, OBJ ) == 0, "K: null controller -> generation 0" );
			Check( !RISE_API_SceneEditController_TreeRootNode( 0, OBJ, 0, &sentinel ), "K: null controller -> no root" );
			Check( RISE_API_SceneEditController_TreeChildCount( 0, OBJ, 0 ) == 0, "K: null controller -> 0 children" );
			Check( !RISE_API_SceneEditController_TreeChildNode( 0, OBJ, 0, 0, &sentinel ), "K: null controller -> no child" );
			Check( !RISE_API_SceneEditController_TreeNodeParent( 0, OBJ, 0, &sentinel ), "K: null controller -> no parent" );
			Check( !RISE_API_SceneEditController_TreeNodeName( 0, OBJ, 0, buf, sizeof(buf) ), "K: null controller -> no name" );
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
	// DOCUMENT's `name` PARAM names, and BuildObjectChunkIndex only indexes
	// a chunk that CARRIES one.  A `standard_object` with no `name` line
	// still registers a live object, because Finalize defaults the name to
	// `noname`.  So this document loads with ZERO diagnostics and the two
	// keyspaces disagree about exactly one name.
	//
	// Before the fix: `noname[0,0]` and `noname[1,0]` folded into the
	// unrelated plain object `noname`; the synth pass then skipped the
	// chunk because the target "already had an entry"; so the array AND
	// the chunk the author wrote got NO ROW AT ALL, and `Z parent
	// noname[0,0]` resolved onto the stranger.  Silently.
	//
	// After: the fold is REFUSED (the target is live but is not the
	// collapse case), the repetitions stay visible as their own nodes, and
	// `Z` stays under the repetition it actually names.  A noisy outliner
	// is recoverable; a missing array is not.
	//
	// Both chunk ORDERS, because the fold decision must not depend on
	// which chunk registered first.
	// =================================================================
	{
		const char* s = "sgnode_namespace.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\ngeometry g\nmaterial m\n}\n"                    // unnamed -> live object `noname`
			"standard_object\n{\nname noname\nsource S\ncount_u 2\n}\n"
			"standard_object\n{\nname Z\ngeometry g\nmaterial m\nparent noname[0,0]\n}\n",
			"M: the two-keyspace scene loads with no diagnostic" );
		{
			SceneEditController c( *j, 0 );
			const IScene* sc = j->GetScene();
			IObjectManager* om = sc ? const_cast<IObjectManager*>( sc->GetObjects() ) : 0;
			Check( om && om->GetItem( "noname" ) && om->GetItem( "noname[0,0]" ) && om->GetItem( "noname[1,0]" ),
			       "M: the premise -- an UNRELATED live object named `noname` coexists with a counted "
			       "instancing chunk of the same name" );
			CheckEq( TreeDump( c, Cat::Object ), "S|noname|noname[0,0]|  Z|noname[1,0]",
			         "M: the counted array still has rows, and `Z` stays under the repetition it names "
			         "-- neither is swallowed by the unrelated same-named object" );
		}
		j->release();
		std::remove( s );
	}
	{
		const char* s = "sgnode_namespace2.RISEscene";
		Job* j = LoadScene( s,
			"standard_object\n{\nname S\ngeometry g\nmaterial m\n}\n"
			"standard_object\n{\nname noname\nsource S\ncount_u 2\n}\n"
			"standard_object\n{\ngeometry g\nmaterial m\n}\n"                    // unnamed, declared AFTER
			"standard_object\n{\nname Z\ngeometry g\nmaterial m\nparent noname[0,0]\n}\n",
			"M: the reversed-order scene loads with no diagnostic" );
		{
			SceneEditController c( *j, 0 );
			CheckEq( TreeDump( c, Cat::Object ), "S|noname[0,0]|  Z|noname[1,0]|noname",
			         "M: and the same holds with the chunks in the other order -- the fold decision "
			         "does not depend on which registered first" );
		}
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
			CheckEq( std::string( c.TreeNodeName( Cat::Object, h ).c_str() ), "bbb",
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

			Check( c.TreeNodeName( Cat::Object, h ).size() <= 1,
			       "N: the STALE handle has NO name -- it does not resolve to whatever node now sits "
			       "at that index" );
			Check( c.TreeNodeParent( Cat::Object, h ) == SceneEditController::kInvalidTreeNode,
			       "N: the stale handle has no parent either" );
			Check( c.TreeChildCount( Cat::Object, h ) == 0, "N: and no children" );

			char buf[128] = { 0 };
			buf[0] = 'z';
			Check( !RISE_API_SceneEditController_TreeNodeName( &c, OBJ, h, buf, sizeof(buf) ),
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
			CheckEq( std::string( c.TreeNodeName( Cat::Object, h ).c_str() ), "hub",
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

	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
