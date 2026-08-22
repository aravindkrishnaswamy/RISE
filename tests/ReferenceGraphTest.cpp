//////////////////////////////////////////////////////////////////////
//
//  ReferenceGraphTest.cpp - doc-88 Phase 3 S11:
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 6's "S11 -- ReferenceGraph
//    shared type + multi-parent DAG assembler".
//
//  Five parts, tested separately then together (round 1 of an opus
//  review added PARTS 4/5 and the new PART 1/2 cases below -- see each
//  block's own comment for which review item it pins):
//
//    PART 1 -- RISE::SceneReferenceGraph (src/Library/SceneEditor/
//      ReferenceGraph.h; renamed from `ReferenceGraph` in review round
//      1 P3 -- it shadowed `RISE::Cst::ReferenceGraph`'s bare name).
//      A pure function of a Cst::Document, so every case here parses a
//      scene STRING directly (Cst::ParseToCst) and never touches a
//      Job -- no film, no camera, no derive.
//        A. FindReferencesTo: painter -> painter (blend_painter.colora/
//           colorb -> uniformcolor_painter).
//        B. FindReferencesTo: material -> painter (lambertian_material.
//           reflectance).
//        C. The scalar pipe: ggx_material.alphax/alphay -> scalar_painter.
//           Both `alphax` and `alphay` declare referenceCategories =
//           {Painter} (not a distinct "scalar" category -- see
//           ChunkParserRegistry.cpp), and `scalar_painter`'s OWN
//           descriptor is ALSO ChunkCategory::Painter (only the LIVE
//           manager differs, IScalarPainterManager vs IPainterManager --
//           see PainterIntrospection.h's header note).  So the (category,
//           name) resolution Cst::BuildReferenceGraph performs is
//           descriptor-based, not live-manager-based, and already
//           covers this case with NO extension needed -- this test
//           VERIFIES that rather than assuming it (the S11 brief
//           flagged this as a case to check, not to trust).
//        D. Forward query (FindReferencesFrom): a chunk's own out-edges.
//        E. Self-reference: tolerated, not a crash, not silently dropped.
//        F. Dangling reference: DanglingReferences surfaces it; Edges/
//           FindReferencesTo do NOT fabricate an edge for it.
//        G. ResolveChunk: exact category (a Painter-named chunk and a
//           same-named Material-category chunk are NOT the same node).
//        H2. AMBIGUITY OUT-PARAM (review round 1 P1-3): FindReferencesTo/
//           From distinguish "0 referrers" from "ambiguous name, refused"
//           via the new `outOccurrences` parameter.
//        I2. TO/FROM SYMMETRY (review round 1 P2-4): for an unambiguous
//           name, FindReferencesFrom's out-edges and FindReferencesTo's
//           in-edges agree; for an AMBIGUOUS referrer name, the two
//           queries deliberately DISAGREE (FindReferencesTo on the far
//           end still shows the edge; FindReferencesFrom on the
//           ambiguous name refuses) -- pinned as documented, not "fixed".
//
//    PART 2 -- SceneEditController::BuildPainterMaterialGraph, the PURE
//      multi-parent DAG assembler (SceneEditController.h/.cpp).  Driven
//      directly on synthetic GraphNodeSeed/GraphEdgeSeed vectors, the
//      same "hostile input" discipline SceneGraphNodeApiTest's D/E
//      cases use for BuildAuthoredTree. Node identity is now
//      `GraphNodeSeed::id` (review round 1 P1-2), so every seed below
//      carries an explicit `Cst::NodeId`:
//        H. Shared-node fan-out: one painter referenced by two
//           materials is ONE node with TWO inEdges rows.
//        I. A crafted CYCLE (a references b, b references a) does not
//           hang the assembler (it never walks what it builds -- see
//           the header comment on GraphEdgeSeed).
//        J. A dangling edge seed (toId matches no node) becomes a
//           node-less port (otherNode == kInvalidNodeIndex), not a
//           crash and not a dropped edge.
//        K. Self-reference on the pure assembler: one row in both
//           outEdges and inEdges of the SAME node.
//        L. DUPLICATE-ID SEED CONTRACT (review round 1 P1-2): two node
//           seeds sharing one `id` collapse to ONE node (the FIRST in
//           presentation order), not a permanently-unreachable orphan --
//           the exact bug round-1 review caught with a synthetic
//           2-duplicate-seed input producing 3 nodes instead of 1.
//        M. TRANSITIVE FUNCTION CHAIN at the assembler level (review
//           round 1 P2-3): a Painter -> Function -> Function chain wires
//           correctly (two hops, not just one). No CURRENT chunk
//           descriptor produces a genuine Function->Function reference
//           edge (audited: every {Function}-typed Reference param lives
//           on a Painter/Material/Geometry/Medium chunk, never on a
//           Function-category chunk itself), so this is tested at the
//           PURE ASSEMBLER level with synthetic seeds -- exactly the
//           shape BuildPainterMaterialGraphSeedsLocked_'s transitive
//           promotion walk would produce once such a param exists.
//           PART 4b covers the REAL one-hop case
//           (scalar_painter.function1d) end-to-end through a loaded Job.
//
//    PART 3 -- the assembler wired to a REAL loaded scene through
//      SceneEditController::ReadPainterMaterialGraph: expression_painter
//      (2 defs) -> ramp_painter -> two materials (fan-out) + a
//      scalar-pipe bridge (ggx_material.alphax -> scalar_painter),
//      asserting the EXACT node/edge sets ReadPainterMaterialGraph
//      publishes.
//
//    PART 4 -- additional REAL-scene coverage from review round 1
//      (P2-5's listed test gaps), through the same Job-loading harness
//      as PART 3:
//        4a. Same-name colour+scalar PAIR: two chunks of the SAME
//            category sharing a name become TWO distinct nodes, both
//            with a correct (never blank) chunkKeyword, edges attached
//            per Cst::BuildReferenceGraph's documented first-wins.
//        4b. REAL one-hop Function promotion: scalar_painter.function1d
//            -> piecewise_linear_function, reached through a material.
//        4c. Document-wide NO-BLANK-KEYWORD invariant, asserted as a
//            standing structural check (not scene-specific) on every
//            fixture already loaded in this part.
//
//    PART 5 -- enamel_watch.RISEscene, a real 2193-line production scene
//      whose 9 `piecewise_linear_function` chunks are DUAL-REGISTERED
//      (Job::AddPiecewiseLinearFunction also registers a Function1D-
//      SpectralPainter in the COLOUR PAINTER manager) -- the exact shape
//      that produced 9 blank-chunkKeyword phantom Painter nodes under
//      the prior live-manager-union seeding. Asserts the document-wide
//      no-blank-keyword invariant AND that the graph's Painter+Material
//      node COUNT exactly matches an independent AllChunks-based count
//      of named Painter/Material chunks in the same document (every
//      such chunk becomes exactly one node, no fewer, no more, no
//      phantoms).
//
//    PART 6 (PERF) -- sombrero.RISEscene, a 96828-line procedurally-
//      generated scene with 3721 uniformcolor_painter + 3721
//      lambertian_material chunks (7442 Painter/Material nodes) --
//      review round 1 P1-1's regression case: the PRIOR seeding design
//      called SceneReferenceGraph::ResolveChunk (a full O(N log N) document
//      scan) once PER node, measured at 24s under mMutex.  Times a
//      steady-state (nothing changed between calls) ReadPainterMaterialGraph
//      and reports the number the review round asked for.
//
//    PART 7 -- doc-88 S11 review round 2 P1: GraphNode::handle, the
//      generation-tagged replacement for the raw Cst::NodeId this struct
//      used to publish (a NodeId is per-parse and can silently ALIAS a
//      different chunk after a reload -- see GraphNodeHandle's own
//      comment).
//        N. The PURE assembler (BuildPainterMaterialGraph) never stamps a
//           handle -- it has no generation to mint one against -- so every
//           node it produces carries kInvalidGraphNode until a real
//           publish (RefreshPainterMaterialGraphSnapshot_) stamps it.
//        O. CROSS-GENERATION REFUSAL: a handle minted on one controller's
//           published graph does not resolve against a DIFFERENT
//           controller's published graph (the process-global generation
//           counter guarantees the two never share a generation) --
//           ResolveGraphNodeHandle refuses cleanly rather than naming
//           whatever node happens to sit at the same index over there.
//           Mirrors SceneGraphNodeApiTest's case W for TreeNodeHandle.
//
//    PART 8 -- doc-88 S11 review round 2 P2-b: SceneEditController::
//      ExpandFunctionPromotionFrontier, factored out of
//      BuildPainterMaterialGraphSeedsLocked_'s transitive Function-node
//      promotion walk so its visited-set guard (the thing that makes a
//      Function->Function CYCLE terminate) is reachable by a synthetic
//      input -- no current chunk descriptor produces a real one
//      post-derive, so this guard was previously provable only by reading
//      the code, not by a test driving it to termination.
//
//    PART 9 -- doc-88 Phase 3 S14: ReadPainterMaterialGraphLaidOut/
//      WriteGraphLayoutPositions (SceneEditController) + the flat
//      PainterGraph* accessors + the RISE_API_SceneEditController_
//      PainterGraph* C-ABI shims (RISE_API.h/.cpp), through a REAL
//      LoadFixture-loaded scene (so `mJob.GetCstLoadFileIdentity()
//      .filePath` is non-empty and a real sidecar file can be exercised):
//        P. First open (no sidecar file yet): every node's laid-out
//           position matches a directly-computed `GraphLayout::
//           LayoutGraph(graph, {})` oracle -- full auto-layout.
//        Q. A hand-written sidecar entry for ONE node is echoed back
//           EXACTLY (never touched by layout); every other node still
//           matches the `LayoutGraph(graph, saved)` oracle.
//        R. WriteGraphLayoutPositions round-trips a real update through
//           the sidecar file (read back via `GraphLayoutSidecar::
//           ReadSidecar` directly) and MERGES onto what was already
//           saved rather than clobbering it.
//        S. Orphan-prune: a stale sidecar entry for a name outside the
//           CURRENT graph is pruned by the next WriteGraphLayoutPositions
//           call, while every live entry (including ones the call did
//           not itself touch) survives.
//        T. Unsaved-scene write refusal: a controller over a Job that was
//           never CST-loaded (empty scene path) -- WriteGraphLayoutPositions
//           returns true (no-op), no error, no file.
//        U. Handle passthrough: a handle read off ReadPainterMaterialGraph
//           (S11) resolves via ResolveGraphNodeHandle against the SAME
//           generation's laid-out graph, and PainterGraphNodePosition
//           agrees with the bulk read's parallel `positions` entry.
//        V. C-ABI smoke: every RISE_API_SceneEditController_PainterGraph*
//           shim (count/generation/handle-at/name/keyword/category/
//           defCount/position/out-edge/in-edge) is cross-checked against
//           the C++ `ReadPainterMaterialGraphLaidOut` result for the SAME
//           published graph, and RISE_API_SceneEditController_
//           WriteGraphNodeLayoutPosition round-trips through the sidecar
//           the same way case R does for the C++ entry point.
//        W. Save-As sidecar migration (doc-88 S14 review round P2(2)): a
//           REAL `RequestSave` to a path different from the scene's
//           current identity migrates the OLD sidecar's positions to the
//           NEW path verbatim, leaves the OLD sidecar in place untouched,
//           and (W2) never overwrites a sidecar that already exists at
//           the destination path.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/ReferenceGraph.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/GraphLayout.h"          // doc-88 Phase 3 S14: LayoutGraph oracle for PART 9
#include "../src/Library/SceneEditor/GraphLayoutSidecar.h"   // doc-88 Phase 3 S14: direct sidecar read/write for PART 9's cross-checks
#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;

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

// ---------------------------------------------------------------------
// PART 1 oracles / helpers -- SceneReferenceGraph over a bare Cst::Document
// ---------------------------------------------------------------------

//! Find one ReferenceEdge in `edges` whose (paramName, occurrence) matches --
//! a spot check, not a whole-list oracle, since these fixtures are small and
//! the interesting property in each case is "this specific edge exists /
//! resolves correctly", not the whole edge SET's shape (PART 2/3 cover shape).
static const ReferenceEdge* FindEdge( const std::vector<ReferenceEdge>& edges,
                                       const char* paramName, int occurrence )
{
	for( const ReferenceEdge& e : edges )
		if( std::string( e.paramName.c_str() ) == paramName && e.occurrence == occurrence )
			return &e;
	return nullptr;
}

// ---------------------------------------------------------------------
// PART 2 oracles / helpers -- the pure DAG assembler
// ---------------------------------------------------------------------

typedef SceneEditController::GraphNodeSeed NodeSeed;
typedef SceneEditController::GraphEdgeSeed EdgeSeed;
typedef SceneEditController::GraphNode     GNode;
typedef SceneEditController::GraphPort     GPort;
typedef SceneEditController::PainterMaterialGraph Graph;
typedef SceneEditController::PainterMaterialGraphLaidOut LaidOutGraph;
typedef SceneEditController::GraphNodePositionUpdate     PosUpdate;
typedef SceneEditController::SceneGraphModel   ObjGraph;       // PART 14: Object Graph slice -- same concrete type as Graph, distinct spelling for readability
typedef SceneEditController::ObjectGraphLaidOut ObjLaidOutGraph;

static NodeSeed MakeNodeSeed( Cst::NodeId id, const char* name, ChunkCategory cat,
                               unsigned long long order, unsigned long long serial = 0 )
{
	NodeSeed s;
	s.id       = id;
	s.name     = String( name );
	s.category = cat;
	s.order    = order;
	s.serial   = serial;
	return s;
}

static EdgeSeed MakeEdgeSeed( Cst::NodeId fromId, ChunkCategory fromCat, const char* fromName,
                               const char* param, int occ,
                               Cst::NodeId toId, ChunkCategory toCat, const char* toName )
{
	EdgeSeed e;
	e.fromId       = fromId;
	e.fromCategory = fromCat;
	e.fromName     = String( fromName );
	e.paramName    = String( param );
	e.occurrence   = occ;
	e.toId         = toId;
	e.toCategory   = toCat;
	e.toName       = String( toName );
	e.portCategories.push_back( toCat );
	return e;
}

//! Find the node named `name` in category `cat`, or null. When more than
//! one node matches (a legal same-name-same-category pair, PART 4a), finds
//! the FIRST in `g.nodes`' own (presentation) order -- callers that need
//! to distinguish siblings use `FindNodeByKeyword` instead.
static const GNode* FindNode( const Graph& g, ChunkCategory cat, const char* name )
{
	for( const GNode& n : g.nodes )
		if( n.category == cat && std::string( n.name.c_str() ) == name )
			return &n;
	return nullptr;
}

//! Find the node named `name` in category `cat` whose chunkKeyword is
//! EXACTLY `keyword` -- the disambiguator PART 4a needs for a legal
//! same-name-same-category pair (e.g. two Painter nodes both named "P",
//! one `uniformcolor_painter`, one `scalar_painter`).
static const GNode* FindNodeByKeyword( const Graph& g, ChunkCategory cat, const char* name, const char* keyword )
{
	for( const GNode& n : g.nodes )
		if( n.category == cat && std::string( n.name.c_str() ) == name && std::string( n.chunkKeyword.c_str() ) == keyword )
			return &n;
	return nullptr;
}

static const GPort* FindPort( const std::vector<GPort>& ports, const char* paramName, int occ )
{
	for( const GPort& p : ports )
		if( std::string( p.paramName.c_str() ) == paramName && p.occurrence == occ )
			return &p;
	return nullptr;
}

//! Standing invariant (review round 1 P2-5): NO node in a published graph
//! may have a blank chunkKeyword. Under the pre-fix live-manager-union
//! seeding, a dual-registered chunk (e.g. piecewise_linear_function, whose
//! descriptor category is Function but which also enumerates through the
//! colour Painter manager) could seed a Painter-category node whose
//! category-exact ResolveChunk lookup then failed to attribute a keyword,
//! leaving it blank. Document-driven seeding (every node comes straight
//! from a real chunk's own `role`) makes this true BY CONSTRUCTION, so
//! this check is run against every real-scene graph in PARTS 3-6, not
//! just the scenes known to have trip the old bug.
static bool NoNodeHasBlankKeyword( const Graph& g, std::string* outFirstOffender = nullptr )
{
	for( const GNode& n : g.nodes ) {
		if( n.chunkKeyword.size() <= 1 ) {   // String's <=1-is-empty convention
			if( outFirstOffender ) *outFirstOffender = std::string( n.name.c_str() );
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------

static const char* kEdgeFixture =
	"RISE ASCII SCENE 7\n"
	"uniformcolor_painter\n{\nname base\ncolor 0.8 0.2 0.1\n}\n"
	"uniformcolor_painter\n{\nname shine\ncolor 1 1 1\n}\n"
	"blend_painter\n{\nname blended\ncolora base\ncolorb base\nmask base\n}\n"
	"lambertian_material\n{\nname wallA\nreflectance blended\n}\n"
	"scalar_painter\n{\nname rough\nvalue 0.3\n}\n"
	"ggx_material\n{\nname metalA\nrd shine\nalphax rough\nalphay rough\n}\n";

//! Load `text` into a fresh Job via a temp file at `path`, returning the Job
//! (caller owns, must ->release()) or nullptr on load failure (the temp
//! file is always removed either way).
static Job* LoadFixture( const char* path, const std::string& text )
{
	{
		std::ofstream o( path );
		o << text;
	}
	Job* j = new Job();
	const bool ok = j->LoadAsciiSceneViaCst( path );
	std::remove( path );
	if( !ok ) { j->release(); return nullptr; }
	return j;
}

//! PART 12: `mRenderOwnsScene` (the flag `AppearanceClosureForObject`'s
//! `outDegraded` reports on) is set ONLY by `RunPreviewRenderParked` --
//! the "parked render" mechanism (`RenderOwnershipScope`,
//! SceneEditController.cpp) -- NOT by the ordinary interactive `Start()`/
//! `RenderLoop` per-frame loop, which uses a DIFFERENT flag (`mRendering`)
//! entirely (verified against the source: `RenderLoop`'s own
//! `DoOneRenderPass` call site, SceneEditController.cpp, sits under an
//! `ActiveFlipGuard` that flips `mRendering`, never `mRenderOwnsScene`).
//! An earlier draft of this test used a `DoOneRenderPass`-overriding
//! subclass plus `Start()`, mirroring
//! tests/SceneEditorCancelRestartTest.cpp's technique -- that compiled and
//! ran, but never observed `mRenderOwnsScene` true even over a 10s poll,
//! which is what caught this distinction. `RunPreviewRenderParked` is
//! public and runs its callback SYNCHRONOUSLY on the CALLING thread with
//! `mMutex` held for the callback's whole duration (its own header
//! comment) -- so a background `std::thread` calling it with a sleeping
//! lambda gives the main test thread a genuine, observable window to poll
//! `ForTest_RenderOwnsScene()` against, no subclass needed.

int main()
{
	std::cout << "ReferenceGraphTest" << std::endl;

	// Scenes under scenes/ reference media by RISE_MEDIA_PATH-relative
	// path (e.g. enamel_watch's `colors/conductors/Ag.n`) -- default to
	// the repo root (tests run with CWD == repo root, same convention
	// scenes/FeatureBased paths below already assume) without clobbering
	// an operator-supplied override.
#ifdef _WIN32
	_putenv_s( "RISE_MEDIA_PATH_DEFAULT_UNUSED", "" );   // no-op, keeps the #ifdef symmetrical
	if( std::getenv( "RISE_MEDIA_PATH" ) == nullptr ) _putenv_s( "RISE_MEDIA_PATH", "./" );
#else
	setenv( "RISE_MEDIA_PATH", "./", 0 );   // 0: do not overwrite an existing value
#endif

	// =================================================================
	// PART 1 -- RISE::SceneReferenceGraph over a bare Cst::Document
	// =================================================================
	{
		const Cst::Document doc = Cst::ParseToCst( kEdgeFixture );

		// A. painter -> painter: blend_painter.colora / .colorb / .mask
		// all point at `base` (a uniformcolor_painter).
		{
			const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "base" );
			Check( to.size() == 3, "A: three painter-side references resolve to `base` (colora, colorb, mask)" );
			bool sawColora = false, sawColorb = false, sawMask = false;
			for( const ReferenceEdge& e : to ) {
				CheckEq( std::string( e.referrerKeyword.c_str() ), "blend_painter", "A: every referrer is the blend_painter chunk" );
				CheckEq( std::string( e.referrerName.c_str() ),    "blended",       "A: every referrer is named `blended`" );
				Check( e.referrerCategory == ChunkCategory::Painter, "A: referrer category is Painter" );
				Check( e.targetCategory   == ChunkCategory::Painter, "A: target category is Painter" );
				if( std::string( e.paramName.c_str() ) == "colora" ) sawColora = true;
				if( std::string( e.paramName.c_str() ) == "colorb" ) sawColorb = true;
				if( std::string( e.paramName.c_str() ) == "mask" )   sawMask = true;
				Check( !e.portCategories.empty() && e.portCategories[0] == ChunkCategory::Painter,
				       "A: the port's declared referenceCategories is {Painter}" );
			}
			Check( sawColora && sawColorb && sawMask, "A: colora, colorb AND mask all show up as distinct referring params" );
		}

		// B. material -> painter: lambertian_material.reflectance -> blended
		{
			const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "blended" );
			Check( to.size() == 1, "B: exactly one referrer of `blended` (wallA.reflectance)" );
			if( to.size() == 1 ) {
				CheckEq( std::string( to[0].referrerKeyword.c_str() ), "lambertian_material", "B: referrer is lambertian_material" );
				CheckEq( std::string( to[0].referrerName.c_str() ),    "wallA",               "B: referrer is named wallA" );
				CheckEq( std::string( to[0].paramName.c_str() ),       "reflectance",         "B: via `reflectance`" );
				Check( to[0].referrerCategory == ChunkCategory::Material, "B: referrer category is Material" );
			}
		}

		// C. THE SCALAR PIPE: ggx_material.alphax / .alphay -> scalar_painter
		// `rough`.  Verifies (does not assume) that Cst::BuildReferenceGraph's
		// descriptor-based (category,name) resolution already covers a
		// scalar_painter target -- see the file header comment.
		{
			const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "rough" );
			Check( to.size() == 2, "C: alphax AND alphay both resolve to the scalar_painter `rough` -- "
			                       "the scalar pipe IS covered by the descriptor-based (category,name) graph, no extension needed" );
			bool sawAlphax = false, sawAlphay = false;
			for( const ReferenceEdge& e : to ) {
				CheckEq( std::string( e.referrerKeyword.c_str() ), "ggx_material", "C: referrer is ggx_material" );
				if( std::string( e.paramName.c_str() ) == "alphax" ) sawAlphax = true;
				if( std::string( e.paramName.c_str() ) == "alphay" ) sawAlphay = true;
			}
			Check( sawAlphax && sawAlphay, "C: both alphax and alphay are distinct referring params" );
		}

		// D. Forward query: what does `metalA` (the ggx_material) itself reference?
		{
			const std::vector<ReferenceEdge> from = SceneReferenceGraph::FindReferencesFrom( doc, ChunkCategory::Material, "metalA" );
			Check( from.size() == 3, "D: metalA has three outgoing references (rd, alphax, alphay)" );
			const ReferenceEdge* rd = FindEdge( from, "rd", 0 );
			Check( rd != nullptr && std::string( rd->targetName.c_str() ) == "shine", "D: rd -> shine" );
			const ReferenceEdge* ax = FindEdge( from, "alphax", 0 );
			Check( ax != nullptr && std::string( ax->targetName.c_str() ) == "rough", "D: alphax -> rough" );
		}

		// I2a. TO/FROM SYMMETRY for an UNAMBIGUOUS name (review round 1 P2-4):
		// FindReferencesFrom(metalA)'s `alphax` row and FindReferencesTo(rough)'s
		// matching row describe the SAME edge from both ends.
		{
			const std::vector<ReferenceEdge> from = SceneReferenceGraph::FindReferencesFrom( doc, ChunkCategory::Material, "metalA" );
			const std::vector<ReferenceEdge> to   = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "rough" );
			const ReferenceEdge* fromSide = FindEdge( from, "alphax", 0 );
			const ReferenceEdge* toSide   = FindEdge( to,   "alphax", 0 );
			Check( fromSide != nullptr && toSide != nullptr, "I2a: both directions find the alphax edge" );
			if( fromSide && toSide ) {
				Check( fromSide->referrerId == toSide->referrerId, "I2a: SYMMETRY -- same referrerId from both queries" );
				Check( fromSide->targetId   == toSide->targetId,   "I2a: SYMMETRY -- same targetId from both queries" );
				Check( fromSide->occurrence == toSide->occurrence, "I2a: SYMMETRY -- same occurrence from both queries" );
			}
		}

		// E/F/G exercised on tiny dedicated fixtures below (self-reference,
		// dangling, and exact-category resolution deserve isolated scenes
		// rather than being folded into the shared fixture's edge count).
	}

	// E. SELF-REFERENCE: a painter that (nonsensically, but not illegally at
	// the CST layer) names itself as its own mask. Must be tolerated -- a
	// resolvable edge, not a crash, not silently dropped.
	{
		const char* selfRefScene =
			"RISE ASCII SCENE 7\n"
			"blend_painter\n{\nname selfy\ncolora selfy\ncolorb selfy\nmask selfy\n}\n";
		const Cst::Document doc = Cst::ParseToCst( selfRefScene );
		const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "selfy" );
		Check( to.size() == 3, "E: a self-referencing chunk resolves all three of its own params as edges to itself" );
		for( const ReferenceEdge& e : to ) {
			Check( std::string( e.referrerName.c_str() ) == "selfy" && std::string( e.targetName.c_str() ) == "selfy",
			       "E: referrer and target are both `selfy`" );
		}
	}

	// F. DANGLING: a reference to a name nothing declares. DanglingReferences
	// surfaces it; Edges()/FindReferencesTo do NOT fabricate a resolved edge.
	{
		const char* danglingScene =
			"RISE ASCII SCENE 7\n"
			"lambertian_material\n{\nname lonely\nreflectance ghost_painter\n}\n";
		const Cst::Document doc = Cst::ParseToCst( danglingScene );
		const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "ghost_painter" );
		Check( to.empty(), "F: no resolved edge exists for a name nothing declares" );
		const std::vector<ReferenceEdge> from = SceneReferenceGraph::FindReferencesFrom( doc, ChunkCategory::Material, "lonely" );
		Check( from.empty(), "F: the dangling reference does not appear in Edges()/FindReferencesFrom either" );
		const std::vector<Cst::UnresolvedReference> dangling = SceneReferenceGraph::DanglingReferences( doc );
		Check( dangling.size() == 1, "F: DanglingReferences surfaces exactly the one dangling reference" );
		if( dangling.size() == 1 ) {
			CheckEq( dangling[0].chunkKeyword, "lambertian_material", "F: dangling reference attributed to lambertian_material" );
			CheckEq( dangling[0].param,        "reflectance",         "F: ...via `reflectance`" );
			CheckEq( dangling[0].value,        "ghost_painter",       "F: ...naming `ghost_painter`" );
		}
		// F2: EdgesAndDangling (review round 1 P1-1's combined query) agrees
		// with the two separate calls above, from a SINGLE BuildReferenceGraph
		// pass.
		const SceneReferenceGraph::Snapshot snap = SceneReferenceGraph::EdgesAndDangling( doc );
		Check( snap.edges.empty(), "F2: EdgesAndDangling's edges half agrees (still empty)" );
		Check( snap.unresolved.size() == 1, "F2: EdgesAndDangling's unresolved half agrees (still one entry)" );
	}

	// G. EXACT CATEGORY: a Painter-named chunk and a same-named Material-
	// category chunk are TWO DIFFERENT nodes, never merged.
	{
		const char* dualNameScene =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname dupe\ncolor 1 1 1\n}\n"
			"lambertian_material\n{\nname dupe\nreflectance dupe\n}\n";
		const Cst::Document doc = Cst::ParseToCst( dualNameScene );
		int occP = 0, occM = 0;
		const Cst::NodeId painterId  = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Painter,  String( "dupe" ), &occP );
		const Cst::NodeId materialId = SceneReferenceGraph::ResolveChunk( doc, ChunkCategory::Material, String( "dupe" ), &occM );
		Check( painterId != 0 && materialId != 0 && painterId != materialId,
		       "G: the Painter `dupe` and the Material `dupe` resolve to two DIFFERENT chunk ids" );
		Check( occP == 1 && occM == 1, "G: neither resolution is reported ambiguous -- each category has exactly one `dupe`" );
	}

	// H2 / I2b. AMBIGUITY OUT-PARAM + the documented TO/FROM ASYMMETRY for an
	// ambiguous name (review round 1 P1-3 + P2-4). Two Painter-category
	// chunks legally share the name "P" (a blend_painter and a
	// scalar_painter); `defs[(Painter,"P")]` first-wins to whichever is
	// declared FIRST (Cst.cpp's BuildReferenceNamespace) -- here the
	// blend_painter, since it appears first in the fixture text.
	{
		const char* ambiguousScene =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname shine\ncolor 1 1 1\n}\n"
			"blend_painter\n{\nname P\ncolora shine\ncolorb shine\nmask shine\n}\n"
			"scalar_painter\n{\nname P\nvalue 0.5\n}\n";
		const Cst::Document doc = Cst::ParseToCst( ambiguousScene );

		// H2a. A GENUINELY UNREFERENCED, UNAMBIGUOUS name: outOccurrences==1,
		// empty result means "really zero referrers".
		{
			int occ = -1;
			const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "shine", &occ );
			Check( occ == 1, "H2a: `shine` is unambiguous (occurrence count 1)" );
			// `shine` IS referenced (by P's colora/colorb/mask) in this fixture,
			// so assert the COUNT matches occ==1's promise that the query
			// actually ran (not refused) -- the interesting empty-vs-ambiguous
			// distinction is H2b below, on a genuinely unreferenced name.
			Check( to.size() == 3, "H2a: an unambiguous, actually-referenced name returns its real referrers" );
		}
		{
			int occ = -1;
			const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Material, "nobody_references_this", &occ );
			Check( occ == 0, "H2a: a name nothing declares reports occurrence 0" );
			Check( to.empty(), "H2a: ...and an empty result -- '0 referrers' because there IS no such chunk" );
		}

		// H2b / I2b. THE AMBIGUOUS name "P": FindReferencesFrom refuses
		// (empty result, occ==2) -- this MUST NOT be misread as "P has zero
		// out-references" by a future reference-safe-delete consumer
		// (ENTITY_CREATION.md sect. 5): P (the blend_painter that won the
		// first-wins race) genuinely has three resolved out-edges, which
		// FindReferencesTo(Painter,"shine") below independently proves.
		{
			int occ = -1;
			const std::vector<ReferenceEdge> from = SceneReferenceGraph::FindReferencesFrom( doc, ChunkCategory::Painter, "P", &occ );
			Check( occ == 2, "H2b: `P` is reported AMBIGUOUS (occurrence count 2 -- the blend_painter and the scalar_painter)" );
			Check( from.empty(), "H2b: FindReferencesFrom REFUSES on an ambiguous name -- empty, but NOT because P has no out-edges" );
		}
		{
			// I2b: the ASYMMETRY -- querying the FAR END (`shine`, unambiguous)
			// still finds the edges the ambiguous referrer emitted, even though
			// FindReferencesFrom(P) itself refused to answer. This is the
			// documented, NOT a bug: Edges()/FindReferencesTo never resolve BY
			// the referrer's name (only ResolveChunk, which FindReferencesFrom
			// uses as its entry gate, can be ambiguity-refused).
			const std::vector<ReferenceEdge> to = SceneReferenceGraph::FindReferencesTo( doc, ChunkCategory::Painter, "shine" );
			Check( to.size() == 3, "I2b: ASYMMETRY -- FindReferencesTo(shine) sees all 3 edges from the ambiguous referrer P" );
			for( const ReferenceEdge& e : to )
				CheckEq( std::string( e.referrerName.c_str() ), "P", "I2b: ...each attributed to referrer name `P` (the first-wins blend_painter)" );
		}
	}

	// =================================================================
	// PART 2 -- the pure DAG assembler, hostile-input discipline
	// (mirrors SceneGraphNodeApiTest's D/E cases for BuildAuthoredTree)
	// =================================================================

	// H. SHARED-NODE FAN-OUT: one ramp_painter referenced by two materials
	// is ONE node with TWO inEdges rows, never two nodes.
	{
		std::vector<NodeSeed> nodes;
		nodes.push_back( MakeNodeSeed( 1, "ramp",  ChunkCategory::Painter,  0 ) );
		nodes.push_back( MakeNodeSeed( 2, "matA",  ChunkCategory::Material, 1 ) );
		nodes.push_back( MakeNodeSeed( 3, "matB",  ChunkCategory::Material, 2 ) );
		std::vector<EdgeSeed> edges;
		edges.push_back( MakeEdgeSeed( 2, ChunkCategory::Material, "matA", "reflectance", 0, 1, ChunkCategory::Painter, "ramp" ) );
		edges.push_back( MakeEdgeSeed( 3, ChunkCategory::Material, "matB", "reflectance", 0, 1, ChunkCategory::Painter, "ramp" ) );
		const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
		Check( g.nodes.size() == 3, "H: exactly THREE nodes -- the shared painter did not get duplicated" );
		const GNode* ramp = FindNode( g, ChunkCategory::Painter, "ramp" );
		Check( ramp != nullptr, "H: the ramp node exists" );
		if( ramp ) {
			Check( ramp->inEdges.size() == 2, "H: the ramp node has TWO inEdges rows (one per referring material)" );
			Check( ramp->outEdges.empty(), "H: the ramp node itself references nothing" );
			bool fromA = false, fromB = false;
			for( const GPort& p : ramp->inEdges ) {
				if( std::string( p.otherName.c_str() ) == "matA" ) fromA = true;
				if( std::string( p.otherName.c_str() ) == "matB" ) fromB = true;
			}
			Check( fromA && fromB, "H: the two inEdges rows name matA and matB respectively" );
		}
		const GNode* matA = FindNode( g, ChunkCategory::Material, "matA" );
		Check( matA && matA->outEdges.size() == 1 && matA->outEdges[0].otherNode != SceneEditController::kInvalidNodeIndex,
		       "H: matA's own outEdges row resolves (not a node-less port)" );
	}

	// I. A CYCLE among edge seeds terminates without a walk -- the assembler
	// never traverses the structure it produces, so nothing can hang here.
	// (Budgeted with a hard iteration cap purely so a REGRESSION that
	// introduced a walk would fail loudly instead of hanging the suite.)
	{
		std::vector<NodeSeed> nodes;
		nodes.push_back( MakeNodeSeed( 1, "a", ChunkCategory::Painter, 0 ) );
		nodes.push_back( MakeNodeSeed( 2, "b", ChunkCategory::Painter, 1 ) );
		std::vector<EdgeSeed> edges;
		edges.push_back( MakeEdgeSeed( 1, ChunkCategory::Painter, "a", "child", 0, 2, ChunkCategory::Painter, "b" ) );
		edges.push_back( MakeEdgeSeed( 2, ChunkCategory::Painter, "b", "child", 0, 1, ChunkCategory::Painter, "a" ) );
		const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
		Check( g.nodes.size() == 2, "I: both cyclic nodes remain -- a cycle is not dropped" );
		const GNode* a = FindNode( g, ChunkCategory::Painter, "a" );
		const GNode* b = FindNode( g, ChunkCategory::Painter, "b" );
		Check( a && a->outEdges.size() == 1 && a->inEdges.size() == 1, "I: `a` has one out-edge (to b) and one in-edge (from b)" );
		Check( b && b->outEdges.size() == 1 && b->inEdges.size() == 1, "I: `b` has one out-edge (to a) and one in-edge (from a)" );
	}

	// J. A DANGLING edge seed (toId matches no node) becomes a node-less
	// port -- recorded, not dropped, not a crash.
	{
		std::vector<NodeSeed> nodes;
		nodes.push_back( MakeNodeSeed( 1, "onlyOne", ChunkCategory::Painter, 0 ) );
		std::vector<EdgeSeed> edges;
		edges.push_back( MakeEdgeSeed( 1, ChunkCategory::Painter, "onlyOne", "mask", 0, 999, ChunkCategory::Painter, "ghost" ) );
		const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
		Check( g.nodes.size() == 1, "J: only the one seeded node exists -- the dangling target did not spawn a phantom node" );
		const GNode* n = FindNode( g, ChunkCategory::Painter, "onlyOne" );
		Check( n != nullptr, "J: onlyOne exists" );
		if( n ) {
			const GPort* p = FindPort( n->outEdges, "mask", 0 );
			Check( p != nullptr, "J: the dangling port is still RECORDED on outEdges" );
			if( p ) {
				Check( p->otherNode == SceneEditController::kInvalidNodeIndex, "J: otherNode is the invalid-node sentinel" );
				CheckEq( std::string( p->otherName.c_str() ), "ghost", "J: otherName preserves the dangling target's name" );
			}
		}
	}

	// K. SELF-REFERENCE on the pure assembler: one row in outEdges AND one
	// row in inEdges of the SAME node -- not special-cased, not a crash.
	{
		std::vector<NodeSeed> nodes;
		nodes.push_back( MakeNodeSeed( 1, "loopy", ChunkCategory::Painter, 0 ) );
		std::vector<EdgeSeed> edges;
		edges.push_back( MakeEdgeSeed( 1, ChunkCategory::Painter, "loopy", "mask", 0, 1, ChunkCategory::Painter, "loopy" ) );
		const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
		Check( g.nodes.size() == 1, "K: one node" );
		const GNode* n = FindNode( g, ChunkCategory::Painter, "loopy" );
		Check( n && n->outEdges.size() == 1 && n->inEdges.size() == 1, "K: loopy has exactly one out-edge and one in-edge, both to/from itself" );
		if( n && !n->outEdges.empty() ) Check( n->outEdges[0].otherNode == 0, "K: the out-edge resolves to node index 0 (itself, the only node)" );
		if( n && !n->inEdges.empty() )  Check( n->inEdges[0].otherNode  == 0, "K: the in-edge resolves to node index 0 (itself)" );
	}

	// L. DUPLICATE-ID SEED CONTRACT (review round 1 P1-2): two node seeds
	// sharing one `id` collapse to ONE node, not a 3rd orphan node -- the
	// exact regression the round-1 review's synthetic input caught (2
	// duplicate-key seeds + 1 distinct seed producing 3 nodes instead of 2).
	{
		std::vector<NodeSeed> nodes;
		nodes.push_back( MakeNodeSeed( 1, "dup", ChunkCategory::Painter, 0, /*serial*/ 100 ) );   // first occurrence: SURVIVES
		nodes.push_back( MakeNodeSeed( 1, "dup", ChunkCategory::Painter, 1, /*serial*/ 200 ) );   // duplicate id: DROPPED
		nodes.push_back( MakeNodeSeed( 2, "other", ChunkCategory::Painter, 2 ) );
		std::vector<EdgeSeed> edges;
		// An edge sourced from the duplicate id must attach to the SURVIVING
		// (first) node -- proving the dedup is a real merge, not a silent
		// drop of the id from the lookup map alone (the round-1 bug).
		edges.push_back( MakeEdgeSeed( 1, ChunkCategory::Painter, "dup", "mask", 0, 2, ChunkCategory::Painter, "other" ) );
		const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
		Check( g.nodes.size() == 2, "L: exactly TWO nodes -- the duplicate-id seed did not become a 3rd orphan" );
		const GNode* dup = FindNode( g, ChunkCategory::Painter, "dup" );
		Check( dup != nullptr, "L: the surviving `dup` node exists" );
		if( dup ) Check( dup->serial == 100, "L: the SURVIVING node is the FIRST occurrence (serial 100, not 200)" );
		const GNode* other = FindNode( g, ChunkCategory::Painter, "other" );
		Check( other && other->inEdges.size() == 1, "L: `other` sees exactly ONE inEdges row -- the edge from the (deduped) `dup` resolves, not orphaned" );
	}

	// M. TRANSITIVE FUNCTION CHAIN at the assembler level (review round 1
	// P2-3): Painter -> FunctionA -> FunctionB, two hops. Chosen over a
	// one-hop cutoff because the promotion-discovery walk
	// (BuildPainterMaterialGraphSeedsLocked_) is a bounded in-memory BFS
	// over the ALREADY-computed document-wide edge list, not a further
	// document scan -- see that method's own comment. This test pins the
	// ASSEMBLER half (given the seeds a transitive walk would produce, the
	// chain wires correctly); no current chunk descriptor has a genuine
	// Function->Function reference param to exercise the DISCOVERY half
	// end-to-end (audited: every {Function}-typed Reference param lives on
	// a Painter/Material/Geometry/Medium chunk, never on a Function-
	// category chunk itself) -- PART 4b covers the real one-hop discovery
	// case that DOES exist (scalar_painter.function1d).
	{
		std::vector<NodeSeed> nodes;
		nodes.push_back( MakeNodeSeed( 1, "paint", ChunkCategory::Painter,  0 ) );
		nodes.push_back( MakeNodeSeed( 2, "fnA",   ChunkCategory::Function, 1 ) );
		nodes.push_back( MakeNodeSeed( 3, "fnB",   ChunkCategory::Function, 2 ) );
		std::vector<EdgeSeed> edges;
		edges.push_back( MakeEdgeSeed( 1, ChunkCategory::Painter,  "paint", "function1d", 0, 2, ChunkCategory::Function, "fnA" ) );
		edges.push_back( MakeEdgeSeed( 2, ChunkCategory::Function, "fnA",   "compose",    0, 3, ChunkCategory::Function, "fnB" ) );
		const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, edges );
		Check( g.nodes.size() == 3, "M: all three nodes (paint, fnA, fnB) present" );
		const GNode* fnA = FindNode( g, ChunkCategory::Function, "fnA" );
		const GNode* fnB = FindNode( g, ChunkCategory::Function, "fnB" );
		Check( fnA && fnA->inEdges.size() == 1 && fnA->outEdges.size() == 1, "M: fnA has one in-edge (from paint) and one out-edge (to fnB) -- the middle of the chain" );
		Check( fnB && fnB->inEdges.size() == 1 && fnB->outEdges.empty(), "M: fnB has one in-edge (from fnA) and no out-edges -- the end of the chain" );
		if( fnA && !fnA->outEdges.empty() ) CheckEq( std::string( fnA->outEdges[0].otherName.c_str() ), "fnB", "M: fnA's out-edge names fnB" );
	}

	// =================================================================
	// PART 3 -- the assembler wired to a REAL loaded scene, through
	// SceneEditController::ReadPainterMaterialGraph.
	// =================================================================
	{
		const char* path = "test_referencegraph_real.RISEscene";
		Job* j = LoadFixture( path,
			"RISE ASCII SCENE 7\n"
			"film\n{\nwidth 32\nheight 24\n}\n"
			"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"expression_painter\n{\nname pnt_field\ndef n fbm(P*3.0, 3, 0.5, 2.0)\ndef t clamp(0.5+0.5*n, 0, 1)\nexpr mix(vec3(0.1,0.1,0.1), vec3(0.9,0.7,0.4), t)\n}\n"
			"ramp_painter\n{\nname warm_ramp\ninput pnt_field\nchannel R\ninterpolation linear\nstop 0.0 1 0 0\nstop 1.0 0 0 1\n}\n"
			"scalar_painter\n{\nname rough_scalar\nvalue 0.3\n}\n"
			"uniformcolor_painter\n{\nname base_color\ncolor 0.8 0.8 0.8\n}\n"
			"lambertian_material\n{\nname wallA\nreflectance warm_ramp\n}\n"
			"lambertian_material\n{\nname wallB\nreflectance warm_ramp\n}\n"
			"ggx_material\n{\nname metalA\nrd base_color\nalphax rough_scalar\nalphay rough_scalar\n}\n"
			"standard_object\n{\nname obj\ngeometry g\nmaterial wallA\n}\n" );
		Check( j != nullptr, "PART3: fixture scene loads" );
		if( j ) {
			SceneEditController c( *j, 0 );
			SceneEditController::PainterMaterialGraph g;
			c.ReadPainterMaterialGraph( g );

			// Exact node SET: pnt_field, warm_ramp, rough_scalar, base_color
			// (Painter) + wallA, wallB, metalA (Material).  No Function node --
			// nothing here references a Function-category chunk.
			Check( g.nodes.size() == 7, "PART3: exactly 7 nodes (4 painters + 3 materials)" );
			Check( g.generation > 0, "PART3: the published graph carries a real generation (>0)" );
			Check( NoNodeHasBlankKeyword( g ), "PART3: no node has a blank chunkKeyword" );

			const GNode* pntField = FindNode( g, ChunkCategory::Painter, "pnt_field" );
			Check( pntField != nullptr, "PART3: pnt_field node exists" );
			if( pntField ) {
				CheckEq( std::string( pntField->chunkKeyword.c_str() ), "expression_painter", "PART3: pnt_field's chunkKeyword" );
				Check( pntField->defCount == 2, "PART3: pnt_field has 2 def stages (the §1.1 two-level model's hook)" );
				Check( pntField->inEdges.size() == 1, "PART3: pnt_field is referenced once (by warm_ramp.input)" );
				Check( pntField->handle != SceneEditController::kInvalidGraphNode,
				       "PART3: pnt_field carries a real (published, generation-tagged) node handle" );
				// HANDLE ROUND-TRIP (doc-88 S11 review round 2 P1): resolving
				// the handle this SAME published graph just handed back
				// against that SAME graph must name pnt_field's own row --
				// the replacement for the raw-Cst::NodeId identity this test
				// used to assert (a raw id round-trips trivially by
				// definition; a generation-tagged handle actually exercises
				// ResolveGraphNodeHandle's decode path).
				const unsigned int pntFieldIdx = SceneEditController::ResolveGraphNodeHandle( g, pntField->handle );
				Check( pntFieldIdx < g.nodes.size()
				    && std::string( g.nodes[pntFieldIdx].name.c_str() ) == "pnt_field",
				       "PART3: ResolveGraphNodeHandle round-trips pnt_field's own handle back to its own row" );
			}

			const GNode* ramp = FindNode( g, ChunkCategory::Painter, "warm_ramp" );
			Check( ramp != nullptr, "PART3: warm_ramp node exists" );
			if( ramp ) {
				CheckEq( std::string( ramp->chunkKeyword.c_str() ), "ramp_painter", "PART3: warm_ramp's chunkKeyword" );
				Check( ramp->defCount == 0, "PART3: a non-expression painter has defCount 0" );
				const GPort* inputPort = FindPort( ramp->outEdges, "input", 0 );
				Check( inputPort && std::string( inputPort->otherName.c_str() ) == "pnt_field",
				       "PART3: warm_ramp.input -> pnt_field" );
				Check( ramp->inEdges.size() == 2, "PART3: warm_ramp is shared -- TWO inEdges rows (wallA, wallB)" );
				bool fromWallA = false, fromWallB = false;
				for( const GPort& p : ramp->inEdges ) {
					if( std::string( p.otherName.c_str() ) == "wallA" ) fromWallA = true;
					if( std::string( p.otherName.c_str() ) == "wallB" ) fromWallB = true;
				}
				Check( fromWallA && fromWallB, "PART3: warm_ramp's two inEdges name wallA and wallB" );
			}

			const GNode* rough = FindNode( g, ChunkCategory::Painter, "rough_scalar" );
			Check( rough != nullptr, "PART3: rough_scalar node exists" );
			if( rough ) {
				CheckEq( std::string( rough->chunkKeyword.c_str() ), "scalar_painter", "PART3: rough_scalar's chunkKeyword" );
				Check( rough->inEdges.size() == 2, "PART3: rough_scalar is referenced twice (metalA.alphax, metalA.alphay) -- the scalar pipe, live end to end" );
			}

			const GNode* metalA = FindNode( g, ChunkCategory::Material, "metalA" );
			Check( metalA != nullptr, "PART3: metalA node exists" );
			if( metalA ) {
				CheckEq( std::string( metalA->chunkKeyword.c_str() ), "ggx_material", "PART3: metalA's chunkKeyword" );
				Check( metalA->outEdges.size() == 3, "PART3: metalA has three outgoing refs (rd, alphax, alphay)" );
			}

			// STALE-FALLBACK / GENERATION STABILITY: an idle re-read (nothing
			// changed) must NOT bump the generation -- same compare-then-publish
			// discipline RefreshTreeSnapshot_ uses (TreesEquivalent), mirrored
			// here by PainterMaterialGraphsEquivalent.
			SceneEditController::PainterMaterialGraph g2;
			c.ReadPainterMaterialGraph( g2 );
			Check( g2.generation == g.generation, "PART3: an idle re-read does not bump the generation" );
			Check( g2.nodes.size() == g.nodes.size(), "PART3: an idle re-read reports the same node count" );

			j->release();
		}
	}

	// =================================================================
	// PART 4 -- additional REAL-scene coverage, review round 1 P2-5's
	// listed test gaps.
	// =================================================================

	// 4a. SAME-NAME COLOUR+SCALAR PAIR: a uniformcolor_painter and a
	// scalar_painter both named "P" (legal -- doc-88 S11 review round 1
	// P1-2) become TWO distinct Painter-category nodes, both with a
	// correct (never blank) chunkKeyword. Every reference to "P" attaches
	// to whichever chunk Cst::BuildReferenceGraph's own first-wins (the)
	// namespace resolution picked (the colour painter, declared first) --
	// this graph does not, and is not meant to, improve on that
	// resolution (see ReferenceGraph.h's "CATEGORY IS EXACT" note).
	{
		const char* path = "test_referencegraph_dualpainter.RISEscene";
		Job* j = LoadFixture( path,
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname P\ncolor 1 0 0\n}\n"
			"scalar_painter\n{\nname P\nvalue 0.5\n}\n"
			"uniformcolor_painter\n{\nname shine\ncolor 1 1 1\n}\n"
			"lambertian_material\n{\nname m1\nreflectance P\n}\n"
			"ggx_material\n{\nname m2\nrd shine\nalphax P\nalphay P\n}\n" );
		Check( j != nullptr, "PART4a: dual-painter fixture loads" );
		if( j ) {
			SceneEditController c( *j, 0 );
			SceneEditController::PainterMaterialGraph g;
			c.ReadPainterMaterialGraph( g );

			int painterPCount = 0;
			for( const GNode& n : g.nodes )
				if( n.category == ChunkCategory::Painter && std::string( n.name.c_str() ) == "P" ) ++painterPCount;
			Check( painterPCount == 2, "PART4a: TWO distinct Painter-category nodes named P (colour + scalar), not collapsed into one" );

			const GNode* colourP = FindNodeByKeyword( g, ChunkCategory::Painter, "P", "uniformcolor_painter" );
			const GNode* scalarP = FindNodeByKeyword( g, ChunkCategory::Painter, "P", "scalar_painter" );
			Check( colourP != nullptr, "PART4a: the colour node's chunkKeyword is uniformcolor_painter -- never blank" );
			Check( scalarP != nullptr, "PART4a: the scalar node's chunkKeyword is scalar_painter -- never blank" );
			if( colourP && scalarP ) {
				Check( colourP->handle != scalarP->handle, "PART4a: the two nodes carry DIFFERENT handles -- distinct chunks, not aliases of one" );
				// First-wins: uniformcolor_painter is declared FIRST in the
				// fixture, so ALL THREE P-directed references (m1.reflectance,
				// m2.alphax, m2.alphay) resolve to it; the scalar sibling gets none.
				Check( colourP->inEdges.size() == 3, "PART4a: first-wins -- all 3 references to `P` land on the colour node (declared first)" );
				Check( scalarP->inEdges.empty(), "PART4a: ...and NONE land on the scalar node (the documented imprecision, not fixed here)" );
			}
			Check( NoNodeHasBlankKeyword( g ), "PART4a: standing invariant -- no node in this graph has a blank chunkKeyword" );
			j->release();
		}
	}

	// 4b. REAL one-hop Function promotion: scalar_painter.function1d ->
	// piecewise_linear_function, reached through a material (the ONE real
	// {Function}-typed Reference param that lives on a Painter-category
	// chunk today -- see PART 2's test M comment for the descriptor audit).
	{
		const char* path = "test_referencegraph_fnpromote.RISEscene";
		Job* j = LoadFixture( path,
			"RISE ASCII SCENE 7\n"
			"piecewise_linear_function\n{\nname curve\ncp 0.0 0.0\ncp 1.0 1.0\n}\n"
			"scalar_painter\n{\nname roughFn\nfunction1d curve\n}\n"
			"uniformcolor_painter\n{\nname shine\ncolor 1 1 1\n}\n"
			"ggx_material\n{\nname m\nrd shine\nalphax roughFn\nalphay roughFn\n}\n" );
		Check( j != nullptr, "PART4b: function-promotion fixture loads" );
		if( j ) {
			SceneEditController c( *j, 0 );
			SceneEditController::PainterMaterialGraph g;
			c.ReadPainterMaterialGraph( g );

			Check( g.nodes.size() == 4, "PART4b: 4 nodes -- roughFn, shine, m, AND the promoted `curve` Function node" );
			const GNode* curve = FindNode( g, ChunkCategory::Function, "curve" );
			Check( curve != nullptr, "PART4b: the piecewise_linear_function `curve` was PROMOTED into a node" );
			if( curve ) {
				CheckEq( std::string( curve->chunkKeyword.c_str() ), "piecewise_linear_function", "PART4b: curve's chunkKeyword is correct, never blank" );
				Check( curve->defCount == 0, "PART4b: a Function chunk never registers in a painter manager -- defCount 0" );
				Check( curve->inEdges.size() == 1, "PART4b: curve is referenced once (roughFn.function1d)" );
			}
			const GNode* roughFn = FindNode( g, ChunkCategory::Painter, "roughFn" );
			Check( roughFn != nullptr, "PART4b: roughFn node exists" );
			if( roughFn ) {
				const GPort* fn1d = FindPort( roughFn->outEdges, "function1d", 0 );
				Check( fn1d && std::string( fn1d->otherName.c_str() ) == "curve" && fn1d->otherNode != SceneEditController::kInvalidNodeIndex,
				       "PART4b: roughFn.function1d -> curve resolves (not a node-less port)" );
			}
			Check( NoNodeHasBlankKeyword( g ), "PART4b: standing invariant -- no blank chunkKeyword" );
			j->release();
		}
	}

	// =================================================================
	// PART 5 -- enamel_watch.RISEscene: a real 2193-line production scene
	// with 9 dual-registered piecewise_linear_function chunks (each also
	// enumerates through the colour Painter manager via
	// Job::AddPiecewiseLinearFunction) -- the exact shape that produced 9
	// blank-chunkKeyword phantom Painter nodes under the prior live-
	// manager-union seeding (doc-88 S11 review round 1 P1-2).
	// =================================================================
	{
		const char* path = "scenes/FeatureBased/EnamelWatch/enamel_watch.RISEscene";
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path );
		Check( loaded, "PART5: enamel_watch.RISEscene loads" );
		if( loaded ) {
			SceneEditController c( *j, 0 );
			SceneEditController::PainterMaterialGraph g;
			c.ReadPainterMaterialGraph( g );

			std::string offender;
			Check( NoNodeHasBlankKeyword( g, &offender ), ( "PART5: no node has a blank chunkKeyword (first offender if any: " + offender + ")" ).c_str() );

			// None of the 9 dual-registered piecewise_linear_function chunks
			// (their descriptor category is Function, only referenced by
			// out-of-scope homogeneous_medium chunks in this scene) show up as
			// a Painter-category node -- the phantom-node failure mode this
			// scene used to trip.
			Check( FindNode( g, ChunkCategory::Painter, "goldruby_abs" ) == nullptr,
			       "PART5: `goldruby_abs` (a piecewise_linear_function) is NOT a phantom Painter node" );

			// STRUCTURAL COUNT CHECK: every named Painter/Material chunk in the
			// document becomes EXACTLY one node -- no fewer (a dropped/collapsed
			// chunk), no more (a phantom). Computed independently of
			// ReadPainterMaterialGraph's own internals (a second, fresh
			// AllChunks scan of the SAME retained document) so this is a real
			// cross-check, not a tautology.
			const RISE::Cst::Document* doc = j->GetCstDocument();
			Check( doc != nullptr, "PART5: the Job retains a CST document" );
			if( doc ) {
				const std::vector<SceneReferenceGraph::DocumentChunk> chunks = SceneReferenceGraph::AllChunks( *doc );
				std::size_t expectedPainterMaterial = 0;
				for( const SceneReferenceGraph::DocumentChunk& dc : chunks ) {
					if( !dc.hasCategory ) continue;
					if( dc.category != ChunkCategory::Painter && dc.category != ChunkCategory::Material ) continue;
					if( dc.name.size() <= 1 ) continue;   // unnamed: this graph does not seed it either
					++expectedPainterMaterial;
				}
				std::size_t actualPainterMaterial = 0;
				for( const GNode& n : g.nodes )
					if( n.category == ChunkCategory::Painter || n.category == ChunkCategory::Material ) ++actualPainterMaterial;
				Check( actualPainterMaterial == expectedPainterMaterial,
				       "PART5: graph's Painter+Material node count EXACTLY matches an independent document scan -- no phantoms, no drops" );
				std::cout << "  PART5: enamel_watch.RISEscene -- " << expectedPainterMaterial
				          << " Painter/Material chunks, " << g.nodes.size() << " total graph nodes" << std::endl;
			}
			j->release();
		} else {
			j->release();
		}
	}

	// =================================================================
	// PART 6 (PERF) -- sombrero.RISEscene: doc-88 S11 review round 1
	// P1-1's regression case. 3721 uniformcolor_painter + 3721
	// lambertian_material chunks (7442 Painter/Material nodes); the PRIOR
	// seeding design called SceneReferenceGraph::ResolveChunk (a fresh O(N log
	// N) document scan) once PER node, measured at 24s under mMutex.
	// =================================================================
	{
		const char* path = "scenes/FeatureBased/Parser/sombrero.RISEscene";
		Job* j = new Job();
		const bool loaded = j->LoadAsciiSceneViaCst( path );
		Check( loaded, "PART6: sombrero.RISEscene loads" );
		if( loaded ) {
			SceneEditController c( *j, 0 );

			// Cold read: builds the live Painter/Material managers' first
			// snapshot (not the measurement -- discarded).
			SceneEditController::PainterMaterialGraph g1;
			c.ReadPainterMaterialGraph( g1 );
			Check( g1.nodes.size() == 7442, "PART6: sombrero graph has exactly 7442 Painter/Material nodes (3721 + 3721)" );

			// STEADY-STATE read: nothing changed between calls, so this times
			// EXACTLY the cost review round 1 P1-1 flagged --
			// BuildPainterMaterialGraphSeedsLocked_ + BuildPainterMaterialGraph,
			// the same work RefreshPainterMaterialGraphSnapshot_ repeats on
			// every refresh regardless of whether the compare-then-publish step
			// ends up bumping the generation.
			const auto t0 = std::chrono::steady_clock::now();
			SceneEditController::PainterMaterialGraph g2;
			c.ReadPainterMaterialGraph( g2 );
			const auto t1 = std::chrono::steady_clock::now();
			const double ms = std::chrono::duration<double, std::milli>( t1 - t0 ).count();

			std::cout << "  PERF: ReadPainterMaterialGraph steady-state on sombrero.RISEscene ("
			          << g2.nodes.size() << " nodes): " << ms << " ms" << std::endl;

			Check( g2.generation == g1.generation, "PART6: steady-state re-read does not bump the generation (nothing changed)" );
			// Budget: the reviewer measured Edges() alone at 23.8ms and asked
			// for "same order... target < ~100ms" for the FULL steady-state
			// read (seeding + assembly), vs. the pre-fix 24 SECONDS. 2000ms
			// leaves generous headroom for slower/loaded CI machines while
			// still failing hard on any reintroduction of the O(nodes * N log
			// N) pattern (which would push this back into multi-second range).
			Check( ms < 2000.0, "PART6: steady-state ReadPainterMaterialGraph stays under 2000ms (pre-fix baseline: ~24000ms)" );

			j->release();
		} else {
			j->release();
		}
	}

	// =================================================================
	// PART 7 -- doc-88 S11 review round 2 P1: GraphNode::handle discipline,
	// replacing the raw Cst::NodeId this struct used to publish.
	// =================================================================

	// N. The PURE assembler never stamps a handle -- see GraphNode::handle's
	// own header comment for why (no generation exists yet to mint one
	// against; only RefreshPainterMaterialGraphSnapshot_, which has just
	// decided to publish, stamps real handles).
	{
		std::vector<NodeSeed> nodes;
		nodes.push_back( MakeNodeSeed( 1, "onlyOne", ChunkCategory::Painter, 0 ) );
		const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );
		Check( g.nodes.size() == 1, "N: one node" );
		if( !g.nodes.empty() )
			Check( g.nodes[0].handle == SceneEditController::kInvalidGraphNode,
			       "N: the pure assembler never stamps a handle -- kInvalidGraphNode until a real publish" );
	}

	// O. CROSS-GENERATION REFUSAL: a handle minted on controller A's
	// published graph does not resolve against controller B's published
	// graph, even though the two are structurally identical single-node
	// fixtures -- mirrors SceneGraphNodeApiTest's case W for TreeNodeHandle.
	// This is the exact hole a raw Cst::NodeId could not have caught: two
	// independently-derived documents can (and here, deliberately do) mint
	// the SAME NodeId for their one chunk, so an `id`-keyed identity would
	// have aliased silently where the generation-tagged handle refuses.
	{
		const char* sA = "refgraph_xctlA.RISEscene";
		const char* sB = "refgraph_xctlB.RISEscene";
		Job* jA = LoadFixture( sA,
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname AONLY\ncolor 1 0 0\n}\n" );
		Job* jB = LoadFixture( sB,
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname BONLY\ncolor 0 1 0\n}\n" );
		Check( jA != nullptr && jB != nullptr, "O: both cross-controller fixtures load" );
		if( jA && jB ) {
			SceneEditController cA( *jA, 0 );
			SceneEditController cB( *jB, 0 );
			SceneEditController::PainterMaterialGraph gA, gB;
			cA.ReadPainterMaterialGraph( gA );
			cB.ReadPainterMaterialGraph( gB );
			Check( gA.nodes.size() == 1 && gB.nodes.size() == 1, "O: both controllers publish a single-node graph" );
			Check( gA.generation != gB.generation, "O: the two controllers' graphs do not share a generation (process-global counter)" );
			if( gA.nodes.size() == 1 && gB.nodes.size() == 1 ) {
				const SceneEditController::GraphNodeHandle hA = gA.nodes[0].handle;
				const unsigned int idxOnA = SceneEditController::ResolveGraphNodeHandle( gA, hA );
				Check( idxOnA == 0, "O: A's handle resolves cleanly against A's OWN graph (the round-trip half)" );
				// The premise: B has its own single node at the SAME index, so a
				// decoded (rather than refused) cross-controller handle would
				// silently name it.
				CheckEq( std::string( gB.nodes[0].name.c_str() ), "BONLY",
				         "O: the premise -- B's node 0 is a DIFFERENT chunk (BONLY, not AONLY)" );
				const unsigned int idxOnB = SceneEditController::ResolveGraphNodeHandle( gB, hA );
				Check( idxOnB == SceneEditController::kInvalidNodeIndex,
				       "O: A's handle FAILS against B's graph rather than silently naming B's node 0" );
			}
		}
		if( jA ) jA->release();
		if( jB ) jB->release();
	}

	// =================================================================
	// PART 8 -- doc-88 S11 review round 2 P2-b:
	// SceneEditController::ExpandFunctionPromotionFrontier, driven directly
	// with a synthetic Function->Function CYCLE to prove the visited-set
	// guard that makes the transitive promotion BFS terminate is real, not
	// just a comment's say-so (no current chunk descriptor produces a
	// genuine Function->Function reference edge post-derive -- see PART 2's
	// test M comment -- so a real loaded scene can never exercise this).
	// =================================================================
	{
		// A 3-hop cycle: fnA(10) -> fnB(20) -> fnC(30) -> fnA(10).
		ReferenceEdge eAB;
		eAB.referrerId     = 10;
		eAB.referrerCategory = ChunkCategory::Function;
		eAB.targetId       = 20;
		eAB.targetCategory = ChunkCategory::Function;
		eAB.targetName     = String( "fnB" );
		ReferenceEdge eBC;
		eBC.referrerId     = 20;
		eBC.referrerCategory = ChunkCategory::Function;
		eBC.targetId       = 30;
		eBC.targetCategory = ChunkCategory::Function;
		eBC.targetName     = String( "fnC" );
		ReferenceEdge eCA;
		eCA.referrerId     = 30;
		eCA.referrerCategory = ChunkCategory::Function;
		eCA.targetId       = 10;
		eCA.targetCategory = ChunkCategory::Function;
		eCA.targetName     = String( "fnA" );

		// The SAME adjacency shape BuildPainterMaterialGraphSeedsLocked_
		// builds once from its full edge scan -- built here directly from
		// the synthetic edges above, no document, no Job.
		std::multimap<Cst::NodeId, const ReferenceEdge*> edgesByReferrer;
		edgesByReferrer.insert( std::make_pair( eAB.referrerId, &eAB ) );
		edgesByReferrer.insert( std::make_pair( eBC.referrerId, &eBC ) );
		edgesByReferrer.insert( std::make_pair( eCA.referrerId, &eCA ) );

		// fnA(10) is the (synthetic) discovery seed -- as if a Painter/
		// Material referrer's own edge had already promoted it, exactly the
		// state BuildPainterMaterialGraphSeedsLocked_'s seeding loop hands
		// this helper before the frontier walk begins.
		std::set<Cst::NodeId> promotedFunctionIds;
		std::vector<std::pair<Cst::NodeId, String> > functionFrontier;
		promotedFunctionIds.insert( 10 );
		functionFrontier.push_back( std::make_pair( Cst::NodeId( 10 ), String( "fnA" ) ) );

		// TERMINATION: if the visited-set guard (`promotedFunctionIds.insert(
		// ...).second`) were ever dropped, this call would grow
		// `functionFrontier` without bound and hang the suite rather than
		// return -- so reaching the assertions below, on a call directly into
		// the PRODUCTION helper (not a reimplementation of it), is itself
		// the property doc-88 S11 review round 2 P2-b asked to prove.
		SceneEditController::ExpandFunctionPromotionFrontier( edgesByReferrer, promotedFunctionIds, functionFrontier );

		Check( promotedFunctionIds.size() == 3, "PART8: all THREE cycle members visited exactly once (fnA, fnB, fnC)" );
		Check( functionFrontier.size() == 3, "PART8: the frontier grew to exactly 3 entries -- the cycle's re-encounter of fnA did not re-queue it" );
		Check( promotedFunctionIds.count( 10 ) == 1 && promotedFunctionIds.count( 20 ) == 1 && promotedFunctionIds.count( 30 ) == 1,
		       "PART8: the visited set names exactly fnA(10), fnB(20), fnC(30)" );
		if( functionFrontier.size() >= 3 ) {
			CheckEq( std::string( functionFrontier[0].second.c_str() ), "fnA", "PART8: frontier[0] is the seed, fnA" );
			CheckEq( std::string( functionFrontier[1].second.c_str() ), "fnB", "PART8: frontier[1] is fnB, discovered from fnA" );
			CheckEq( std::string( functionFrontier[2].second.c_str() ), "fnC", "PART8: frontier[2] is fnC, discovered from fnB" );
		}
	}

	// =================================================================
	// PART 9 -- doc-88 Phase 3 S14: {nodes, edges, positions} composition,
	// the layout-position write path, the flat accessors, and the C-ABI
	// shims.  See this file's own header comment for the full case list.
	// =================================================================
	{
		const char* path = "test_referencegraph_s14.RISEscene";
		Job* j = LoadFixture( path,
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\nname baseA\ncolor 0.8 0.2 0.1\n}\n"
			"uniformcolor_painter\n{\nname baseB\ncolor 0.1 0.8 0.2\n}\n"
			"lambertian_material\n{\nname wallA\nreflectance baseA\n}\n" );
		Check( j != nullptr, "PART9: S14 fixture scene loads" );
		if( j ) {
			const std::string scenePath( path );
			const std::string sidecarPath = GraphLayoutSidecar::SidecarPathForScene( scenePath );
			std::remove( sidecarPath.c_str() );   // belt-and-braces: no leftover from a prior failed run

			SceneEditController c( *j, 0 );

			// ---- P. First open: no sidecar file yet -- full auto-layout ----
			LaidOutGraph laidOut;
			c.ReadPainterMaterialGraphLaidOut( laidOut );
			Check( laidOut.graph.nodes.size() == 3, "PART9-P: 3 nodes (baseA, baseB, wallA)" );
			Check( laidOut.positions.size() == laidOut.graph.nodes.size(), "PART9-P: positions is parallel to graph.nodes" );
			{
				const GraphLayout::Positions oracle = GraphLayout::LayoutGraph( laidOut.graph, GraphLayout::Positions() );
				bool allMatch = true;
				for( std::size_t i = 0; i < laidOut.graph.nodes.size(); ++i ) {
					const std::string key( laidOut.graph.nodes[i].name.c_str() );
					GraphLayout::Positions::const_iterator it = oracle.find( key );
					if( it == oracle.end() || it->second.x != laidOut.positions[i].x || it->second.y != laidOut.positions[i].y )
						allMatch = false;
				}
				Check( allMatch, "PART9-P: every position matches the direct LayoutGraph({}) oracle (no sidecar yet)" );
			}

			// ---- Q. A hand-written sidecar entry for baseA is echoed verbatim ----
			{
				GraphLayout::Positions manual;
				GraphLayoutPoint pt; pt.x = 111.0; pt.y = 222.0;
				manual[ "baseA" ] = pt;
				std::set<std::string> live;
				for( const GNode& n : laidOut.graph.nodes ) live.insert( std::string( n.name.c_str() ) );
				std::string werr;
				Check( GraphLayoutSidecar::WriteSidecar( scenePath, manual, live, werr ),
				       "PART9-Q: direct WriteSidecar of a hand-authored baseA entry succeeds" );

				LaidOutGraph laidOut2;
				c.ReadPainterMaterialGraphLaidOut( laidOut2 );
				const GNode* baseA = FindNode( laidOut2.graph, ChunkCategory::Painter, "baseA" );
				Check( baseA != nullptr, "PART9-Q: baseA node still present" );
				if( baseA ) {
					const unsigned int idx = SceneEditController::ResolveGraphNodeHandle( laidOut2.graph, baseA->handle );
					Check( idx < laidOut2.positions.size()
					    && laidOut2.positions[idx].x == 111.0 && laidOut2.positions[idx].y == 222.0,
					       "PART9-Q: baseA's position is echoed EXACTLY from the sidecar, untouched by layout" );
				}
				const GraphLayout::Positions oracle2 = GraphLayout::LayoutGraph( laidOut2.graph, manual );
				bool allMatch2 = true;
				for( std::size_t i = 0; i < laidOut2.graph.nodes.size(); ++i ) {
					const std::string key( laidOut2.graph.nodes[i].name.c_str() );
					GraphLayout::Positions::const_iterator it = oracle2.find( key );
					if( it == oracle2.end() || it->second.x != laidOut2.positions[i].x || it->second.y != laidOut2.positions[i].y )
						allMatch2 = false;
				}
				Check( allMatch2, "PART9-Q: every OTHER node still matches LayoutGraph(graph, {baseA}) with baseA pinned" );
			}

			// ---- R. WriteGraphLayoutPositions round-trips + MERGES ----
			{
				std::vector<PosUpdate> updates;
				PosUpdate u; u.name = String( "baseB" ); u.x = 55.0; u.y = 66.0;
				updates.push_back( u );
				std::string werr;
				Check( c.WriteGraphLayoutPositions( updates, werr ), "PART9-R: WriteGraphLayoutPositions succeeds" );
				Check( werr.empty(), "PART9-R: no error message on success" );

				const GraphLayout::Positions onDisk = GraphLayoutSidecar::ReadSidecar( scenePath );
				GraphLayout::Positions::const_iterator itA = onDisk.find( "baseA" );
				GraphLayout::Positions::const_iterator itB = onDisk.find( "baseB" );
				Check( itA != onDisk.end() && itA->second.x == 111.0 && itA->second.y == 222.0,
				       "PART9-R: baseA's earlier position SURVIVED the merge (not clobbered)" );
				Check( itB != onDisk.end() && itB->second.x == 55.0 && itB->second.y == 66.0,
				       "PART9-R: baseB's new position is on disk" );
			}

			// ---- S. Orphan-prune ----
			{
				// Directly seed a "ghost" entry that is NOT among the current
				// graph's node names -- forcing liveNodeNames to include it at
				// THIS write only, bypassing the controller, so it lands on disk
				// once. The very next controller-driven write must prune it,
				// because the controller always supplies the CURRENT graph's own
				// live names (ghost is never among them).
				GraphLayout::Positions withGhost = GraphLayoutSidecar::ReadSidecar( scenePath );
				GraphLayoutPoint ghostPt; ghostPt.x = 9.0; ghostPt.y = 9.0;
				withGhost[ "ghost" ] = ghostPt;
				std::set<std::string> liveWithGhost;
				for( const GNode& n : laidOut.graph.nodes ) liveWithGhost.insert( std::string( n.name.c_str() ) );
				liveWithGhost.insert( "ghost" );
				std::string werr0;
				Check( GraphLayoutSidecar::WriteSidecar( scenePath, withGhost, liveWithGhost, werr0 ),
				       "PART9-S: seeding a ghost entry directly succeeds" );
				Check( GraphLayoutSidecar::ReadSidecar( scenePath ).count( "ghost" ) == 1,
				       "PART9-S: the ghost entry is on disk before the controller write" );

				std::vector<PosUpdate> updates;
				PosUpdate u; u.name = String( "wallA" ); u.x = 77.0; u.y = 88.0;
				updates.push_back( u );
				std::string werr;
				Check( c.WriteGraphLayoutPositions( updates, werr ), "PART9-S: the pruning write succeeds" );

				const GraphLayout::Positions afterPrune = GraphLayoutSidecar::ReadSidecar( scenePath );
				Check( afterPrune.count( "ghost" ) == 0, "PART9-S: the ghost (non-live) entry was pruned" );
				Check( afterPrune.count( "baseA" ) == 1 && afterPrune.count( "baseB" ) == 1,
				       "PART9-S: the untouched live entries (baseA, baseB) survive the prune" );
				GraphLayout::Positions::const_iterator itW = afterPrune.find( "wallA" );
				Check( itW != afterPrune.end() && itW->second.x == 77.0 && itW->second.y == 88.0,
				       "PART9-S: wallA's new position is on disk" );
			}

			// ---- T. Unsaved-scene write refusal ----
			{
				Job* uj = new Job();   // never LoadAsciiSceneViaCst'd -- GetCstLoadFileIdentity().filePath == ""
				SceneEditController uc( *uj, 0 );
				std::vector<PosUpdate> updates;
				PosUpdate u; u.name = String( "whatever" ); u.x = 1.0; u.y = 2.0;
				updates.push_back( u );
				std::string uerr;
				const bool uok = uc.WriteGraphLayoutPositions( updates, uerr );
				Check( uok, "PART9-T: an unsaved scene's write no-ops (returns true, not a refusal)" );
				Check( uerr.empty(), "PART9-T: no error message on the unsaved no-op" );
				uj->release();
			}

			// ---- U. Handle passthrough ----
			{
				Graph g;
				c.ReadPainterMaterialGraph( g );
				const GNode* wallA = FindNode( g, ChunkCategory::Material, "wallA" );
				Check( wallA != nullptr, "PART9-U: wallA node exists via the S11 read" );
				if( wallA ) {
					double px = -1.0, py = -1.0;
					Check( c.PainterGraphNodePosition( wallA->handle, px, py ),
					       "PART9-U: PainterGraphNodePosition resolves a real S11-minted handle" );

					LaidOutGraph laidOut3;
					c.ReadPainterMaterialGraphLaidOut( laidOut3 );
					const unsigned int idx = SceneEditController::ResolveGraphNodeHandle( laidOut3.graph, wallA->handle );
					Check( idx < laidOut3.positions.size()
					    && laidOut3.positions[idx].x == px && laidOut3.positions[idx].y == py,
					       "PART9-U: matches the bulk laid-out read's own parallel positions entry" );
				}
			}

			// ---- V. C-ABI smoke ----
			{
				LaidOutGraph laidOut4;
				c.ReadPainterMaterialGraphLaidOut( laidOut4 );

				const unsigned int cnt = RISE_API_SceneEditController_PainterGraphNodeCount( &c );
				Check( cnt == laidOut4.graph.nodes.size(), "PART9-V: ABI node count matches the C++ read" );
				const unsigned long long gen = RISE_API_SceneEditController_PainterGraphGeneration( &c );
				Check( gen == laidOut4.graph.generation, "PART9-V: ABI generation matches the C++ read" );

				for( unsigned int i = 0; i < cnt; ++i ) {
					unsigned long long h = 0;
					Check( RISE_API_SceneEditController_PainterGraphNodeHandleAt( &c, i, &h ),
					       "PART9-V: ABI PainterGraphNodeHandleAt succeeds for a valid index" );
					const GNode* match = nullptr;
					for( const GNode& n : laidOut4.graph.nodes ) if( n.handle == h ) { match = &n; break; }
					Check( match != nullptr, "PART9-V: the ABI handle names a real C++ node" );
					if( !match ) continue;

					char nameBuf[256] = {0};
					Check( RISE_API_SceneEditController_PainterGraphNodeName( &c, h, nameBuf, sizeof( nameBuf ) ),
					       "PART9-V: ABI node name lookup succeeds" );
					CheckEq( std::string( nameBuf ), std::string( match->name.c_str() ), "PART9-V: ABI node name matches" );

					char kwBuf[256] = {0};
					Check( RISE_API_SceneEditController_PainterGraphNodeKeyword( &c, h, kwBuf, sizeof( kwBuf ) ),
					       "PART9-V: ABI node keyword lookup succeeds" );
					CheckEq( std::string( kwBuf ), std::string( match->chunkKeyword.c_str() ), "PART9-V: ABI node keyword matches" );

					Check( RISE_API_SceneEditController_PainterGraphNodeCategory( &c, h ) == static_cast<int>( match->category ),
					       "PART9-V: ABI node category matches" );
					Check( RISE_API_SceneEditController_PainterGraphNodeDefCount( &c, h ) == match->defCount,
					       "PART9-V: ABI node defCount matches" );

					double px = -1.0, py = -1.0;
					Check( RISE_API_SceneEditController_PainterGraphNodePosition( &c, h, &px, &py ),
					       "PART9-V: ABI node position lookup succeeds" );
					const unsigned int mi = SceneEditController::ResolveGraphNodeHandle( laidOut4.graph, h );
					Check( mi < laidOut4.positions.size() && laidOut4.positions[mi].x == px && laidOut4.positions[mi].y == py,
					       "PART9-V: ABI position matches the C++ bulk laid-out read" );

					const unsigned int outCnt = RISE_API_SceneEditController_PainterGraphNodeOutEdgeCount( &c, h );
					Check( outCnt == match->outEdges.size(), "PART9-V: ABI outEdge count matches" );
					for( unsigned int pi = 0; pi < outCnt; ++pi ) {
						unsigned long long otherH = SceneEditController::kInvalidGraphNode;
						char pnBuf[256] = {0}; int occ = -1; char onBuf[256] = {0};
						Check( RISE_API_SceneEditController_PainterGraphNodeOutEdge(
						           &c, h, pi, &otherH, pnBuf, sizeof( pnBuf ), &occ, onBuf, sizeof( onBuf ) ),
						       "PART9-V: ABI outEdge lookup succeeds" );
						const GPort& gp = match->outEdges[pi];
						CheckEq( std::string( pnBuf ), std::string( gp.paramName.c_str() ), "PART9-V: ABI outEdge paramName matches" );
						Check( occ == gp.occurrence, "PART9-V: ABI outEdge occurrence matches" );
						CheckEq( std::string( onBuf ), std::string( gp.otherName.c_str() ), "PART9-V: ABI outEdge otherName matches" );
						const unsigned long long expectedOther =
							( gp.otherNode == SceneEditController::kInvalidNodeIndex )
								? SceneEditController::kInvalidGraphNode
								: laidOut4.graph.nodes[gp.otherNode].handle;
						Check( otherH == expectedOther, "PART9-V: ABI outEdge otherNode resolves to the SAME handle as the C++ read" );
					}

					const unsigned int inCnt = RISE_API_SceneEditController_PainterGraphNodeInEdgeCount( &c, h );
					Check( inCnt == match->inEdges.size(), "PART9-V: ABI inEdge count matches" );
					for( unsigned int pi = 0; pi < inCnt; ++pi ) {
						unsigned long long otherH = SceneEditController::kInvalidGraphNode;
						char pnBuf[256] = {0}; int occ = -1; char onBuf[256] = {0};
						Check( RISE_API_SceneEditController_PainterGraphNodeInEdge(
						           &c, h, pi, &otherH, pnBuf, sizeof( pnBuf ), &occ, onBuf, sizeof( onBuf ) ),
						       "PART9-V: ABI inEdge lookup succeeds" );
						const GPort& gp = match->inEdges[pi];
						CheckEq( std::string( pnBuf ), std::string( gp.paramName.c_str() ), "PART9-V: ABI inEdge paramName matches" );
						Check( occ == gp.occurrence, "PART9-V: ABI inEdge occurrence matches" );
						CheckEq( std::string( onBuf ), std::string( gp.otherName.c_str() ), "PART9-V: ABI inEdge otherName matches" );
						const unsigned long long expectedOther =
							( gp.otherNode == SceneEditController::kInvalidNodeIndex )
								? SceneEditController::kInvalidGraphNode
								: laidOut4.graph.nodes[gp.otherNode].handle;
						Check( otherH == expectedOther, "PART9-V: ABI inEdge otherNode resolves to the SAME handle as the C++ read" );
					}
				}

				// ABI write round-trip, same shape as PART9-R for the C++ entry point.
				char errBuf[256] = {0};
				const bool wok = RISE_API_SceneEditController_WriteGraphNodeLayoutPosition(
					&c, "baseA", 321.0, 654.0, errBuf, sizeof( errBuf ) );
				Check( wok, "PART9-V: ABI WriteGraphNodeLayoutPosition succeeds" );
				const GraphLayout::Positions onDisk = GraphLayoutSidecar::ReadSidecar( scenePath );
				GraphLayout::Positions::const_iterator itA = onDisk.find( "baseA" );
				Check( itA != onDisk.end() && itA->second.x == 321.0 && itA->second.y == 654.0,
				       "PART9-V: ABI write round-trips baseA's new position through the sidecar" );

				// Null-controller / bad-input refusal, same discrimination style
				// as every other C-ABI shim on this surface (never a crash).
				Check( RISE_API_SceneEditController_PainterGraphNodeCount( nullptr ) == 0, "PART9-V: null controller -> 0 count" );
				Check( !RISE_API_SceneEditController_WriteGraphNodeLayoutPosition( &c, "", 0.0, 0.0, nullptr, 0 ),
				       "PART9-V: empty nodeName is refused" );
			}

			// ---- W. Save-As sidecar migration (doc-88 S14 review round P2(2)) ----
			// A RequestSave to a path DIFFERENT from the scene's current
			// FileIdentity path (a Save-As) must migrate the OLD sidecar's
			// positions to the new path -- see GraphLayoutSidecar.h's
			// "SAVE-AS SIDECAR MIGRATION" note on MigrateSidecarOnSaveAs.
			// Driven through the controller's real RequestSave, same as
			// PainterIntrospectionRoundTripTest's own Save-As coverage,
			// rather than calling MigrateSidecarOnSaveAs directly -- this is
			// the end-to-end wiring check, GraphLayoutSidecarTest already
			// covers the primitive itself in isolation.
			{
				const GraphLayout::Positions before = GraphLayoutSidecar::ReadSidecar( scenePath );
				Check( !before.empty(), "PART9-W: fixture already has a saved sidecar before Save-As" );

				const std::string saveAsPath = "test_referencegraph_s14_saveas.RISEscene";
				const std::string newSidecarPath = GraphLayoutSidecar::SidecarPathForScene( saveAsPath );
				std::remove( saveAsPath.c_str() );
				std::remove( newSidecarPath.c_str() );   // belt-and-braces: no leftover from a prior failed run

				const SaveResult sr = c.RequestSave( saveAsPath );
				Check( Succeeded( sr.status ), "PART9-W: Save-As succeeds" );

				const GraphLayout::Positions after = GraphLayoutSidecar::ReadSidecar( saveAsPath );
				Check( after.size() == before.size(), "PART9-W: the new path's sidecar has the SAME node count as the old one" );
				bool allMigrated = ( after.size() == before.size() );
				for( const std::pair<const std::string, GraphLayoutPoint>& kv : before ) {
					GraphLayout::Positions::const_iterator it = after.find( kv.first );
					if( it == after.end() || it->second.x != kv.second.x || it->second.y != kv.second.y ) allMigrated = false;
				}
				Check( allMigrated, "PART9-W: every position migrated verbatim to the new path's sidecar" );

				const GraphLayout::Positions oldStill = GraphLayoutSidecar::ReadSidecar( scenePath );
				Check( oldStill.size() == before.size(), "PART9-W: the OLD sidecar is left in place, untouched (belongs to the old scene file, which still exists)" );

				// ---- W2. Never overwrites an EXISTING new-path sidecar ----
				{
					const std::string saveAsPath2 = "test_referencegraph_s14_saveas2.RISEscene";
					const std::string newSidecarPath2 = GraphLayoutSidecar::SidecarPathForScene( saveAsPath2 );
					std::remove( saveAsPath2.c_str() );
					std::remove( newSidecarPath2.c_str() );

					GraphLayout::Positions decoy;
					GraphLayoutPoint decoyPt; decoyPt.x = -1.0; decoyPt.y = -1.0;
					decoy[ "decoy" ] = decoyPt;
					std::set<std::string> decoyLive; decoyLive.insert( "decoy" );
					std::string decoyErr;
					Check( GraphLayoutSidecar::WriteSidecar( saveAsPath2, decoy, decoyLive, decoyErr ),
					       "PART9-W2: seeding a decoy sidecar at the future Save-As target succeeds" );

					// c's identity is now saveAsPath (from W above) -- this Save-As
					// migrates FROM saveAsPath, not scenePath; either way the point
					// under test is the destination-side refusal.
					const SaveResult sr2 = c.RequestSave( saveAsPath2 );
					Check( Succeeded( sr2.status ), "PART9-W2: second Save-As succeeds" );

					const GraphLayout::Positions afterDecoy = GraphLayoutSidecar::ReadSidecar( saveAsPath2 );
					Check( afterDecoy.size() == 1 && afterDecoy.count( "decoy" ) == 1
					    && afterDecoy.find( "decoy" )->second.x == -1.0 && afterDecoy.find( "decoy" )->second.y == -1.0,
					       "PART9-W2: an EXISTING new-path sidecar is never overwritten by the migration" );

					std::remove( saveAsPath2.c_str() );
					std::remove( newSidecarPath2.c_str() );
				}

				std::remove( saveAsPath.c_str() );
				std::remove( newSidecarPath.c_str() );
			}

			std::remove( sidecarPath.c_str() );
			j->release();
		}
	}

	// =================================================================
	// PART 10 -- SceneEditController::AppearanceClosureForObject: the
	// node-graph canvas "spotlight" query (viewport/outliner object pick
	// -> chunk (category,name) identities to highlight).  Cases (a)-(f)
	// are the ones named in the feature brief; a composite_material case
	// backs this method's own header-comment claim that Material->Material
	// edges (not just Painter/Function) are included in the closure; case
	// (g) and the standalone ResolveUniqueGraphNodeIndex cases below are
	// review-round additions pinning the (category,name)-qualified result
	// shape and the ambiguous-duplicate refusal.
	// =================================================================
	{
		const char* path = "test_referencegraph_appearance.RISEscene";
		Job* j = LoadFixture( path,
			"RISE ASCII SCENE 7\n"
			"film\n{\nwidth 32\nheight 24\n}\n"
			"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			// matA's chain: matA.reflectance -> ramp1.input -> field1 (a
			// 2-hop transitive closure, PART10 (a)/(c)).
			"expression_painter\n{\nname field1\ndef n fbm(P*2.0, 2, 0.5, 2.0)\nexpr vec3(n,n,n)\n}\n"
			"ramp_painter\n{\nname ramp1\ninput field1\nchannel R\ninterpolation linear\nstop 0.0 1 0 0\nstop 1.0 0 0 1\n}\n"
			"lambertian_material\n{\nname matA\nreflectance ramp1\n}\n"
			// A Painter chunk sharing the EXACT name "matA" -- a DIFFERENT
			// category, unreferenced by anything (PART10g: proves the
			// initial material lookup is category-qualified, not a bare
			// name scan that could latch onto this instead).
			"expression_painter\n{\nname matA\ndef n fbm(P*9.0, 2, 0.5, 2.0)\nexpr vec3(n,n,n)\n}\n"
			// matB's own separate chain, referenced only through the
			// composite material below (PART10 bonus: Material->Material).
			"expression_painter\n{\nname field2\ndef n fbm(P*4.0, 2, 0.5, 2.0)\nexpr vec3(n,n,n)\n}\n"
			"lambertian_material\n{\nname matB\nreflectance field2\n}\n"
			"composite_material\n{\nname compo\ntop matA\nbottom matB\n}\n"
			// obj1/obj2 both bind matA (PART10 (a)/(b)); objNoMat binds
			// nothing (PART10 (e)); objInstance is a `source` instance of
			// obj1 (PART10 (f)); objComposite binds the composite material
			// (PART10 bonus).
			"standard_object\n{\nname obj1\ngeometry g\nmaterial matA\nposition -3 0 0\n}\n"
			"standard_object\n{\nname obj2\ngeometry g\nmaterial matA\nposition -1 0 0\n}\n"
			"standard_object\n{\nname objNoMat\ngeometry g\nposition 1 0 0\n}\n"
			"standard_object\n{\nname objInstance\nsource obj1\nposition 3 0 0\n}\n"
			"standard_object\n{\nname objComposite\ngeometry g\nmaterial compo\nposition 5 0 0\n}\n" );
		Check( j != nullptr, "PART10: fixture scene loads" );
		if( j ) {
			SceneEditController c( *j, 0 );
			typedef SceneEditController::AppearanceClosureEntry ACEntry;

			// ---- (a) object with one material + painter chain: exact
			// closure set + order (material first, then BFS discovery
			// order down the single-branch chain), EACH ENTRY CARRYING THE
			// CORRECT CATEGORY (review-round P1 fix: the result is
			// (category,name) pairs, not bare names) ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "obj1" ) );
				Check( closure.size() == 3, "PART10a: obj1's closure has exactly 3 entries (matA, ramp1, field1)" );
				if( closure.size() == 3 ) {
					CheckEq( std::string( closure[0].name.c_str() ), "matA", "PART10a: index 0 is the bound material" );
					Check( closure[0].category == ChunkCategory::Material, "PART10a: index 0's category is Material" );
					CheckEq( std::string( closure[1].name.c_str() ), "ramp1", "PART10a: index 1 is matA's direct painter reference" );
					Check( closure[1].category == ChunkCategory::Painter, "PART10a: index 1's category is Painter" );
					CheckEq( std::string( closure[2].name.c_str() ), "field1", "PART10a: index 2 is ramp1's own reference (transitive)" );
					Check( closure[2].category == ChunkCategory::Painter, "PART10a: index 2's category is Painter" );
				}
			}

			// ---- (b) two objects sharing a material: each resolves to
			// the SAME closure ----
			{
				const std::vector<ACEntry> closure1 = c.AppearanceClosureForObject( String( "obj1" ) );
				const std::vector<ACEntry> closure2 = c.AppearanceClosureForObject( String( "obj2" ) );
				Check( closure1.size() == closure2.size() && closure1.size() == 3,
				       "PART10b: obj1 and obj2 (sharing matA) resolve to the same-size closure" );
				bool same = closure1.size() == closure2.size();
				for( std::size_t i = 0; same && i < closure1.size(); ++i )
					if( closure1[i].category != closure2[i].category
					 || std::string( closure1[i].name.c_str() ) != std::string( closure2[i].name.c_str() ) ) same = false;
				Check( same, "PART10b: obj1 and obj2's closures are IDENTICAL, not merely same-sized" );
			}

			// ---- (c) material with a deep painter chain: transitive
			// closure complete (field1, two hops from matA, is present) ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "obj1" ) );
				bool hasField1 = false;
				for( const ACEntry& e : closure )
					if( e.category == ChunkCategory::Painter && std::string( e.name.c_str() ) == "field1" ) hasField1 = true;
				Check( hasField1, "PART10c: the 2-hop-away field1 is present -- the closure is transitive, not one-hop" );
			}

			// ---- (d) unknown object name -> empty ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "no_such_object_at_all" ) );
				Check( closure.empty(), "PART10d: an unknown object name resolves to an empty closure" );
			}

			// ---- (e) object with no material bound -> empty.  Precise
			// claim (review-round P2 fix -- the earlier wording overstated
			// what this proves): objNoMat's `standard_object` chunk names
			// no `material` param at all, so its live IObject::GetMaterial()
			// returns null and FindObjectMaterialName reports "" -- this
			// case exercises ONLY that object-level unbound path.  It does
			// NOT exercise (and does not claim to exercise) a material
			// chunk whose OWN painter slot is left at the `none` default;
			// that is a node-less-PORT case inside BuildPainterMaterialGraph
			// itself (dangling/out-of-scope references becoming a port with
			// no node on the other end, never a phantom node), already
			// covered by that assembler's own PART 2 tests -- this method's
			// Step 2 reuses that mechanism verbatim rather than re-deriving
			// it, so it is not re-proven here. ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "objNoMat" ) );
				Check( closure.empty(), "PART10e: an object with no material bound resolves to an empty closure" );
			}

			// ---- (f) instanced-copy object name: resolves to the SAME
			// closure as the source object, because FindObjectMaterialName
			// reads the LIVE post-derive IObject's bound material (copied
			// from the source at Cst::DeriveToJob PASS-2 expansion time),
			// not the instancing chunk's own CST text -- which for
			// objInstance carries only `source obj1`, no `material` line
			// at all ----
			{
				const std::vector<ACEntry> closureSrc = c.AppearanceClosureForObject( String( "obj1" ) );
				const std::vector<ACEntry> closureInst = c.AppearanceClosureForObject( String( "objInstance" ) );
				Check( closureInst.size() == closureSrc.size() && closureInst.size() == 3,
				       "PART10f: the instancing chunk's OWN name resolves to a non-empty closure" );
				bool same = closureInst.size() == closureSrc.size();
				for( std::size_t i = 0; same && i < closureInst.size(); ++i )
					if( closureInst[i].category != closureSrc[i].category
					 || std::string( closureInst[i].name.c_str() ) != std::string( closureSrc[i].name.c_str() ) ) same = false;
				Check( same, "PART10f: an instance's closure is IDENTICAL to its source object's closure" );
			}

			// ---- (g) CROSS-CATEGORY NAME COLLISION (review-round P1 fix):
			// a Painter chunk also named "matA" exists in this fixture
			// (unreferenced), sharing the name with the Material "matA"
			// obj1 is actually bound to.  The closure must still resolve
			// to the MATERIAL "matA" and its real chain -- a name-only
			// lookup (matching ANY category) could latch onto the
			// unrelated Painter "matA" instead, which has no outEdges,
			// and this test would then see a 1-entry closure instead of 3.
			// The Painter "matA" node itself must never appear in the
			// result (it is unreferenced by obj1's own material chain). ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "obj1" ) );
				Check( closure.size() == 3,
				       "PART10g: the cross-category matA collision does not change obj1's closure size (still 3, not 1)" );
				if( !closure.empty() ) {
					Check( closure[0].category == ChunkCategory::Material,
					       "PART10g: index 0 resolves to the MATERIAL matA, not the same-named Painter" );
				}
				// The unreferenced Painter "matA" must be absent (it has no
				// edge from the real matA chain -- confirms it was never
				// mistakenly treated as part of this closure).
				int painterMatACount = 0;
				for( const ACEntry& e : closure )
					if( e.category == ChunkCategory::Painter && std::string( e.name.c_str() ) == "matA" ) ++painterMatACount;
				Check( painterMatACount == 0, "PART10g: the unreferenced same-named Painter matA is NOT in the closure" );
			}

			// ---- bonus: composite_material's Material->Material edges
			// (top/bottom) are traversed, not treated as a closure
			// boundary -- backs this method's own header-comment claim ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "objComposite" ) );
				Check( closure.size() == 6,
				       "PART10-bonus: objComposite's closure has 6 entries (compo, matA, ramp1, field1, matB, field2)" );
				Check( !closure.empty() && std::string( closure[0].name.c_str() ) == "compo"
				    && closure[0].category == ChunkCategory::Material,
				       "PART10-bonus: index 0 is the composite material itself" );
				bool hasMatA = false, hasMatB = false, hasRamp1 = false, hasField1 = false, hasField2 = false;
				for( const ACEntry& e : closure ) {
					const std::string n( e.name.c_str() );
					if( e.category == ChunkCategory::Material && n == "matA" ) hasMatA = true;
					if( e.category == ChunkCategory::Material && n == "matB" ) hasMatB = true;
					if( e.category == ChunkCategory::Painter  && n == "ramp1" ) hasRamp1 = true;
					if( e.category == ChunkCategory::Painter  && n == "field1" ) hasField1 = true;
					if( e.category == ChunkCategory::Painter  && n == "field2" ) hasField2 = true;
				}
				Check( hasMatA && hasMatB, "PART10-bonus: both sub-materials (matA, matB) are in the closure" );
				Check( hasRamp1 && hasField1 && hasField2,
				       "PART10-bonus: both sub-materials' OWN painter chains are in the closure too" );
			}

			j->release();
		}
	}

	// =================================================================
	// PART 11 -- SceneEditController::ResolveUniqueGraphNodeIndex: the
	// PURE "(category,name) -> unique index, or refuse" helper
	// AppearanceClosureForObject uses to resolve the starting material
	// (review-round P1/P2 fix).  Driven against SYNTHETIC graphs built via
	// BuildPainterMaterialGraph (the SAME pure assembler PART 2 uses) --
	// no controller, no Job, no live derive, which matters specifically
	// because a REAL scene load CANNOT construct the duplicate-name case
	// this is meant to pin: a live IMaterialManager::AddItem refuses a
	// second material registered under a name already in use and fails
	// the whole derive (GenericManager::AddItem's "Item of same name...
	// already exists" refusal), so a Job-backed fixture could never reach
	// this state even though the CST DOCUMENT (and therefore this
	// document-seeded graph) tolerates it just fine.
	// =================================================================
	{
		// A. Unique (category,name): resolves, matches == 1.
		{
			std::vector<NodeSeed> nodes;
			nodes.push_back( MakeNodeSeed( 1, "onlyMat", ChunkCategory::Material, 0 ) );
			nodes.push_back( MakeNodeSeed( 2, "onlyMat", ChunkCategory::Painter,  1 ) );   // cross-category same-name sibling -- must NOT count
			const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );
			int matches = -1;
			const int idx = SceneEditController::ResolveUniqueGraphNodeIndex( g, ChunkCategory::Material, String( "onlyMat" ), &matches );
			Check( matches == 1, "PART11A: exactly one Material-category match (the Painter sibling is not counted)" );
			Check( idx >= 0 && static_cast<std::size_t>( idx ) < g.nodes.size()
			    && g.nodes[static_cast<std::size_t>(idx)].category == ChunkCategory::Material
			    && std::string( g.nodes[static_cast<std::size_t>(idx)].name.c_str() ) == "onlyMat",
			       "PART11A: the resolved index names the Material node, not its Painter sibling" );
		}

		// B. Not present at all: matches == 0, index -1.
		{
			std::vector<NodeSeed> nodes;
			nodes.push_back( MakeNodeSeed( 1, "somethingElse", ChunkCategory::Material, 0 ) );
			const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );
			int matches = -1;
			const int idx = SceneEditController::ResolveUniqueGraphNodeIndex( g, ChunkCategory::Material, String( "notThere" ), &matches );
			Check( matches == 0, "PART11B: zero matches for a name not in the graph" );
			Check( idx < 0, "PART11B: refused (index -1), not a guess" );
		}

		// C. SAME-CATEGORY duplicate name: the case a real Job-backed scene
		// cannot construct (see this PART's own header comment) -- two
		// Material-category seeds sharing "dup_mat". Must REFUSE
		// (matches == 2, index -1), never silently pick the first.
		{
			std::vector<NodeSeed> nodes;
			nodes.push_back( MakeNodeSeed( 1, "dup_mat", ChunkCategory::Material, 0 ) );
			nodes.push_back( MakeNodeSeed( 2, "dup_mat", ChunkCategory::Material, 1 ) );
			const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );
			Check( g.nodes.size() == 2, "PART11C: the assembler seeds BOTH same-category same-name chunks as distinct nodes" );
			int matches = -1;
			const int idx = SceneEditController::ResolveUniqueGraphNodeIndex( g, ChunkCategory::Material, String( "dup_mat" ), &matches );
			Check( matches == 2, "PART11C: the ambiguity is reported (matches == 2), not silently collapsed" );
			Check( idx < 0, "PART11C: refused (index -1) rather than guessing which duplicate is the \"real\" one" );
		}

		// D. outMatches is optional -- a null pointer must not crash.
		{
			std::vector<NodeSeed> nodes;
			nodes.push_back( MakeNodeSeed( 1, "dup_mat", ChunkCategory::Material, 0 ) );
			nodes.push_back( MakeNodeSeed( 2, "dup_mat", ChunkCategory::Material, 1 ) );
			const Graph g = SceneEditController::BuildPainterMaterialGraph( nodes, std::vector<EdgeSeed>() );
			const int idx = SceneEditController::ResolveUniqueGraphNodeIndex( g, ChunkCategory::Material, String( "dup_mat" ), nullptr );
			Check( idx < 0, "PART11D: a null outMatches pointer is tolerated (still refuses correctly)" );
		}
	}

	// =================================================================
	// PART 12 -- SceneEditController::AppearanceClosureForObject's
	// `outDegraded` out-param (later external review round P1 fix): an
	// empty result is overloaded ("genuinely nothing to show" vs "could
	// not even attempt a real answer because a render owns the commit
	// lock"), and both platform GUI consumers need to tell the two apart
	// to avoid either an unbounded 0.5s retry-forever loop (Mac) or a
	// full deep resolve on every single preview frame forever (Qt) for a
	// perfectly ordinary material-less object. Pins `outDegraded == false`
	// on (a) a successful closure, (b) a genuinely-empty resolve, (c) an
	// unknown object -- and, since the render-ownership state IS reachable
	// headlessly via the SAME DoOneRenderPass-override technique
	// tests/SceneEditorCancelRestartTest.cpp already established (see
	// DegradeTestController's own comment above), also pins
	// `outDegraded == true` while a render genuinely owns the scene.
	// =================================================================
	{
		const char* path = "test_referencegraph_degraded.RISEscene";
		Job* j = LoadFixture( path,
			"RISE ASCII SCENE 7\n"
			"film\n{\nwidth 32\nheight 24\n}\n"
			"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			"uniformcolor_painter\n{\nname base\ncolor 0.8 0.2 0.1\n}\n"
			"lambertian_material\n{\nname matA\nreflectance base\n}\n"
			"standard_object\n{\nname obj1\ngeometry g\nmaterial matA\nposition 0 0 0\n}\n"
			"standard_object\n{\nname objNoMat\ngeometry g\nposition 2 0 0\n}\n" );
		Check( j != nullptr, "PART12: fixture scene loads" );
		if( j ) {
			SceneEditController c( *j, 0 );
			typedef SceneEditController::AppearanceClosureEntry ACEntry;

			// ---- (a) successful closure -> outDegraded == false ----
			{
				bool degraded = true;   // seeded to the WRONG answer -- the call must actually set it
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "obj1" ), &degraded );
				Check( !closure.empty(), "PART12a: obj1 resolves a non-empty closure (sanity)" );
				Check( !degraded, "PART12a: a successful resolve reports outDegraded == false" );
			}

			// ---- (b) genuinely-empty resolve (material-less object) ->
			// outDegraded == false -- this is THE case the prior fix got
			// wrong (both platforms' consumers previously could not
			// distinguish this from contention) ----
			{
				bool degraded = true;
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "objNoMat" ), &degraded );
				Check( closure.empty(), "PART12b: objNoMat resolves an empty closure (sanity, no material bound)" );
				Check( !degraded, "PART12b: a genuinely-empty resolve reports outDegraded == false, NOT true" );
			}

			// ---- (c) unknown object -> outDegraded == false ----
			{
				bool degraded = true;
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "no_such_object" ), &degraded );
				Check( closure.empty(), "PART12c: an unknown object resolves an empty closure (sanity)" );
				Check( !degraded, "PART12c: an unknown-object refusal reports outDegraded == false, NOT true" );
			}

			// ---- (d) outDegraded == nullptr default -- every pre-existing
			// caller (PART 10's own calls, and both GUI bridges before
			// this round) compiles and behaves unchanged. Not a new
			// behavioral case, just confirming the default-arg contract
			// mentioned in the header doesn't crash with no out-param at
			// all. ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "obj1" ) );
				Check( !closure.empty(), "PART12d: the default nullptr outDegraded is tolerated and the call still resolves" );
			}

			j->release();
		}

		// ---- (e) REAL render contention -> outDegraded == true ----
		{
			// A BARE, never-loaded Job. This part only exercises the
			// `mRenderOwnsScene` gate itself (the FIRST check inside
			// AppearanceClosureForObject, before anything scene-specific
			// is ever touched), so an unloaded Job is adequate -- no scene
			// content is needed to prove the gate.
			Job* rj = new Job();
			SceneEditController dc( *rj, /*interactiveRasterizer*/0 );

			// RunPreviewRenderParked runs its callback SYNCHRONOUSLY, with
			// mMutex held and mRenderOwnsScene set true for the callback's
			// whole duration (its own header comment; see this PART's
			// header comment for why this -- not Start()/RenderLoop -- is
			// the mechanism that actually sets this flag). Drive it from a
			// background thread so the main test thread can poll.
			std::thread renderThread( [&dc]() {
				dc.RunPreviewRenderParked( []() {
					std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
				} );
			} );

			// 300ms is generous relative to the poll granularity (10ms) and
			// the AppearanceClosureForObject call itself (microseconds) --
			// once the poll below observes mRenderOwnsScene flip true, the
			// remaining window comfortably covers the immediately
			// following call.
			bool sawRenderOwn = false;
			for( int i = 0; i < 200; ++i ) {   // up to ~2s
				if( dc.ForTest_RenderOwnsScene() ) { sawRenderOwn = true; break; }
				std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
			}
			Check( sawRenderOwn, "PART12e: the parked render is observed genuinely owning the scene" );

			if( sawRenderOwn ) {
				bool degraded = false;
				const std::vector<SceneEditController::AppearanceClosureEntry> closure =
					dc.AppearanceClosureForObject( String( "obj1" ), &degraded );
				Check( degraded, "PART12e: a call made WHILE a render owns the scene reports outDegraded == true" );
				Check( closure.empty(), "PART12e: a degraded call returns an empty result (never a stale/partial answer)" );
			}

			renderThread.join();
			rj->release();
		}
	}

	// =================================================================
	// PART 13 -- SceneEditController::ReadPainterMaterialGraphLaidOutFocused:
	// the node-graph canvas's "All" vs "Focused" view-scope toggle
	// (user-requested slice on top of the spotlight query). Shares
	// AppearanceClosureForObject's own resolution + BFS (this is what
	// PART13a pins directly: the two must produce IDENTICAL node sets in
	// IDENTICAL order for the SAME object).
	// =================================================================
	{
		const char* path = "test_referencegraph_focused.RISEscene";
		Job* j = LoadFixture( path,
			"RISE ASCII SCENE 7\n"
			"film\n{\nwidth 32\nheight 24\n}\n"
			"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
			"sphere_geometry\n{\nname g\nradius 1\n}\n"
			// ramp1's UPSTREAM chain: ramp1.input -> field1.
			"expression_painter\n{\nname field1\ndef n fbm(P*2.0, 2, 0.5, 2.0)\nexpr vec3(n,n,n)\n}\n"
			"ramp_painter\n{\nname ramp1\ninput field1\nchannel R\ninterpolation linear\nstop 0.0 1 0 0\nstop 1.0 0 0 1\n}\n"
			// ramp1's DOWNSTREAM referrers: matA AND matB both reference it
			// (PART13b's fixture for "downstream referrers excluded" -- a
			// focused read rooted at ramp1 must show neither).
			"lambertian_material\n{\nname matA\nreflectance ramp1\n}\n"
			"lambertian_material\n{\nname matB\nreflectance ramp1\n}\n"
			"standard_object\n{\nname obj1\ngeometry g\nmaterial matA\nposition 0 0 0\n}\n" );
		Check( j != nullptr, "PART13: fixture scene loads" );
		if( j ) {
			SceneEditController c( *j, 0 );
			typedef SceneEditController::AppearanceClosureEntry ACEntry;

			// ---- (a) Object-category focused read matches
			// AppearanceClosureForObject exactly (set + order), and every
			// returned node gets a valid (non-negative, rank-consistent)
			// laid-out position ----
			{
				const std::vector<ACEntry> closure = c.AppearanceClosureForObject( String( "obj1" ) );
				Check( closure.size() == 3, "PART13a: sanity -- obj1's appearance closure has 3 entries (matA, ramp1, field1)" );

				bool degraded = true;   // seeded wrong -- the call must set it
				LaidOutGraph focused;
				c.ReadPainterMaterialGraphLaidOutFocused( ChunkCategory::Object, String( "obj1" ), focused, &degraded );
				Check( !degraded, "PART13a: a non-contended focused read reports outDegraded == false" );
				Check( focused.graph.nodes.size() == closure.size(),
				       "PART13a: the focused subgraph has EXACTLY as many nodes as the appearance closure" );
				Check( focused.positions.size() == focused.graph.nodes.size(),
				       "PART13a: positions is parallel to graph.nodes" );

				bool sameOrder = ( focused.graph.nodes.size() == closure.size() );
				for( std::size_t i = 0; sameOrder && i < closure.size(); ++i ) {
					if( focused.graph.nodes[i].category != closure[i].category
					 || std::string( focused.graph.nodes[i].name.c_str() ) != std::string( closure[i].name.c_str() ) )
						sameOrder = false;
				}
				Check( sameOrder, "PART13a: the focused subgraph's node ORDER matches AppearanceClosureForObject's exactly" );

				// Valid ranks: GraphLayout::ComputeRanks (Kahn's algorithm)
				// finalizes a LEAF node (no out-edges -- field1, which
				// references nothing) at rank 0 FIRST, then propagates
				// rank = 1 + max(dependency rank) toward whatever
				// references it -- so rank (and therefore layout x, "Column
				// x = rank * columnSpacing") strictly INCREASES
				// leaf-to-root: field1.x < ramp1.x < matA.x. All positions
				// non-negative "by construction" (GraphLayout's own
				// invariant).
				double matAx = -1, ramp1x = -1, field1x = -1;
				bool allNonNegative = true;
				for( std::size_t i = 0; i < focused.graph.nodes.size(); ++i ) {
					const std::string n( focused.graph.nodes[i].name.c_str() );
					if( focused.positions[i].x < 0.0 || focused.positions[i].y < 0.0 ) allNonNegative = false;
					if( n == "matA" )   matAx   = focused.positions[i].x;
					if( n == "ramp1" )  ramp1x  = focused.positions[i].x;
					if( n == "field1" ) field1x = focused.positions[i].x;
				}
				Check( allNonNegative, "PART13a: every focused-subgraph position is non-negative" );
				Check( field1x >= 0 && ramp1x > field1x && matAx > ramp1x,
				       "PART13a: layout rank strictly increases field1 -> ramp1 -> matA (leaf-to-root)" );
			}

			// ---- (b) mid-chain Painter-category focused read: node +
			// upstream only. Downstream referrers (matA, matB -- BOTH
			// reference ramp1) are EXCLUDED, even though ramp1 itself is
			// SHARED (2 referrers) ----
			{
				bool degraded = true;
				LaidOutGraph focused;
				c.ReadPainterMaterialGraphLaidOutFocused( ChunkCategory::Painter, String( "ramp1" ), focused, &degraded );
				Check( !degraded, "PART13b: a non-contended focused read reports outDegraded == false" );
				Check( focused.graph.nodes.size() == 2,
				       "PART13b: ramp1's focused subgraph has exactly 2 nodes (ramp1, field1) -- upstream only" );

				bool hasRamp1 = false, hasField1 = false, hasMatA = false, hasMatB = false;
				for( const auto& n : focused.graph.nodes ) {
					const std::string nm( n.name.c_str() );
					if( n.category == ChunkCategory::Painter && nm == "ramp1" )  hasRamp1 = true;
					if( n.category == ChunkCategory::Painter && nm == "field1" ) hasField1 = true;
					if( n.category == ChunkCategory::Material && nm == "matA" )  hasMatA = true;
					if( n.category == ChunkCategory::Material && nm == "matB" )  hasMatB = true;
				}
				Check( hasRamp1 && hasField1, "PART13b: ramp1 and its upstream field1 are both present" );
				Check( !hasMatA && !hasMatB,
				       "PART13b: DOWNSTREAM REFERRERS matA and matB are EXCLUDED from the focused subgraph" );
			}

			// ---- (c) unknown name -> empty ----
			{
				LaidOutGraph focused;
				c.ReadPainterMaterialGraphLaidOutFocused( ChunkCategory::Painter, String( "no_such_painter" ), focused );
				Check( focused.graph.nodes.empty(), "PART13c: an unknown (category,name) resolves an empty focused subgraph" );
			}
			{
				LaidOutGraph focused;
				c.ReadPainterMaterialGraphLaidOutFocused( ChunkCategory::Object, String( "no_such_object" ), focused );
				Check( focused.graph.nodes.empty(), "PART13c: an unknown OBJECT resolves an empty focused subgraph too" );
			}

			j->release();
		}

		// ---- (d) degraded propagation: a focused read for cat==Object
		// made WHILE a render owns the scene reports outDegraded == true
		// and an empty graph -- same RunPreviewRenderParked technique
		// PART12(e) already established (see that PART's own comment for
		// why Start()/DoOneRenderPass is NOT the right mechanism here) ----
		{
			Job* rj = new Job();
			SceneEditController dc( *rj, /*interactiveRasterizer*/0 );
			std::thread renderThread( [&dc]() {
				dc.RunPreviewRenderParked( []() {
					std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
				} );
			} );

			bool sawRenderOwn = false;
			for( int i = 0; i < 200; ++i ) {
				if( dc.ForTest_RenderOwnsScene() ) { sawRenderOwn = true; break; }
				std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
			}
			Check( sawRenderOwn, "PART13d: the parked render is observed genuinely owning the scene" );

			if( sawRenderOwn ) {
				bool degraded = false;
				LaidOutGraph focused;
				dc.ReadPainterMaterialGraphLaidOutFocused( ChunkCategory::Object, String( "obj1" ), focused, &degraded );
				Check( degraded, "PART13d: a focused Object read made WHILE a render owns the scene reports outDegraded == true" );
				Check( focused.graph.nodes.empty(), "PART13d: a degraded focused read returns an empty graph" );
			}

			renderThread.join();
			rj->release();
		}
	}

	// =====================================================================
	// PART 14 -- Object Graph slice (S1 core): SceneEditController::
	// ReadObjectGraph / ReadObjectGraphLaidOut / ReadObjectGraphLaidOutFocused,
	// BuildObjectGraphSeedsLocked_, ObjectGraphFocusedClosure. Reuses the
	// SAME shared model (SceneGraphModel == PainterMaterialGraph) PART 1-13
	// already exercise, so FindNode/FindPort/Graph work unchanged -- only
	// the SEEDING and the FOCUSED closure are new logic to pin here.
	// =====================================================================
	{
		// ---- (a) object_parenting.RISEscene: node set (objects+geometry
		// ONLY, no painters/materials), a container has no geometry edge,
		// rank ordering (parent rank < child rank; geometry rank > every
		// object that consumes it) ----
		{
			const char* path = "scenes/Tests/Geometry/object_parenting.RISEscene";
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path );
			Check( loaded, "PART14a: object_parenting.RISEscene loads" );
			if( loaded ) {
				SceneEditController c( *j, 0 );
				ObjLaidOutGraph lg;
				c.ReadObjectGraphLaidOut( lg );
				const ObjGraph& g = lg.graph;

				std::size_t objCount = 0, geoCount = 0, otherCount = 0;
				for( const GNode& n : g.nodes ) {
					if( n.category == ChunkCategory::Object ) ++objCount;
					else if( n.category == ChunkCategory::Geometry ) ++geoCount;
					else ++otherCount;
				}
				Check( objCount == 14, "PART14a: 14 standard_object chunks become 14 Object nodes" );
				Check( geoCount == 4, "PART14a: 4 geometry chunks become 4 Geometry nodes" );
				Check( otherCount == 0, "PART14a: no Painter/Material nodes leak into the Object Graph" );
				Check( g.nodes.size() == 18, "PART14a: 18 total nodes (14 objects + 4 geometry)" );

				const GNode* pedestalRoot = FindNode( g, ChunkCategory::Object, "pedestal_root" );
				const GNode* pedestal     = FindNode( g, ChunkCategory::Object, "pedestal" );
				const GNode* shoulder     = FindNode( g, ChunkCategory::Object, "shoulder" );
				const GNode* elbow        = FindNode( g, ChunkCategory::Object, "elbow" );
				const GNode* handRoot     = FindNode( g, ChunkCategory::Object, "hand_root" );
				const GNode* palm         = FindNode( g, ChunkCategory::Object, "palm" );
				const GNode* geoBall      = FindNode( g, ChunkCategory::Geometry, "geo_ball" );
				Check( pedestalRoot && pedestal && shoulder && elbow && handRoot && palm && geoBall,
				       "PART14a: sanity -- every referenced node resolves" );

				if( pedestalRoot ) {
					Check( FindPort( pedestalRoot->inEdges, "geometry", 0 ) == nullptr,
					       "PART14a: pedestal_root (a container, no `geometry` line) has NO geometry inEdge" );
					Check( FindPort( pedestalRoot->outEdges, "parent", 0 ) == nullptr,
					       "PART14a: pedestal_root (a root) has no `parent` outEdge" );
				}
				if( pedestal ) {
					const GPort* p = FindPort( pedestal->outEdges, "parent", 0 );
					Check( p && std::string( p->otherName.c_str() ) == "pedestal_root" && p->otherNode != SceneEditController::kInvalidNodeIndex,
					       "PART14a: pedestal.parent -> pedestal_root resolves" );
				}

				// Rank ordering via laid-out x (rank * columnSpacing, strictly
				// increasing root -> leaf -> geometry): pedestal_root(0) <
				// shoulder(1) < elbow(2) < hand_root(3) < palm(4) < geo_ball(5,
				// consumed by palm among others).
				auto xOf = [&]( const char* name, ChunkCategory cat ) -> double {
					for( std::size_t i = 0; i < g.nodes.size(); ++i )
						if( g.nodes[i].category == cat && std::string( g.nodes[i].name.c_str() ) == name )
							return lg.positions[i].x;
					return -1.0;
				};
				const double xRoot  = xOf( "pedestal_root", ChunkCategory::Object );
				const double xShldr = xOf( "shoulder",      ChunkCategory::Object );
				const double xElbow = xOf( "elbow",         ChunkCategory::Object );
				const double xHand  = xOf( "hand_root",     ChunkCategory::Object );
				const double xPalm  = xOf( "palm",          ChunkCategory::Object );
				const double xBall  = xOf( "geo_ball",      ChunkCategory::Geometry );
				Check( xRoot >= 0.0 && xRoot < xShldr && xShldr < xElbow && xElbow < xHand && xHand < xPalm && xPalm < xBall,
				       "PART14a: rank ordering strictly increases pedestal_root -> shoulder -> elbow -> hand_root -> palm -> geo_ball" );
				j->release();
			}
		}

		// ---- (b) object_instancing.RISEscene: DOCUMENT-seeded node count
		// (9 authored object chunks, NOT the 42 live instanced objects),
		// repeatCount on the counted chunk, `source` edges (whole-subtree
		// AND leaf-collapse forms) ----
		{
			const char* path = "scenes/Tests/Geometry/object_instancing.RISEscene";
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path );
			Check( loaded, "PART14b: object_instancing.RISEscene loads" );
			if( loaded ) {
				SceneEditController c( *j, 0 );
				ObjGraph g;
				c.ReadObjectGraph( g );

				std::size_t objCount = 0, geoCount = 0;
				for( const GNode& n : g.nodes ) {
					if( n.category == ChunkCategory::Object ) ++objCount;
					else if( n.category == ChunkCategory::Geometry ) ++geoCount;
				}
				Check( objCount == 9, "PART14b: 9 AUTHORED object chunks (not the 42 live instanced objects) become 9 Object nodes" );
				Check( geoCount == 4, "PART14b: 4 geometry chunks become 4 Geometry nodes" );

				const GNode* row      = FindNode( g, ChunkCategory::Object, "row" );
				const GNode* pairB    = FindNode( g, ChunkCategory::Object, "pair_b" );
				const GNode* globeSolo= FindNode( g, ChunkCategory::Object, "globe_solo" );
				const GNode* lantern  = FindNode( g, ChunkCategory::Object, "lantern" );
				const GNode* lanternGlobe = FindNode( g, ChunkCategory::Object, "lantern_globe" );
				Check( row && pairB && globeSolo && lantern && lanternGlobe, "PART14b: sanity -- every referenced node resolves" );

				if( row )      Check( row->repeatCount == 6, "PART14b: `row` (count_u 3 count_v 2) has repeatCount == 6" );
				if( pairB )    Check( pairB->repeatCount == 0, "PART14b: `pair_b` (no count_u) has repeatCount == 0" );
				if( lantern )  Check( lantern->repeatCount == 0, "PART14b: `lantern` (the authored root) has repeatCount == 0" );

				if( pairB ) {
					const GPort* p = FindPort( pairB->outEdges, "source", 0 );
					Check( p && std::string( p->otherName.c_str() ) == "lantern" && p->otherNode != SceneEditController::kInvalidNodeIndex,
					       "PART14b: pair_b.source -> lantern (whole-subtree instancing) resolves" );
				}
				if( globeSolo ) {
					const GPort* p = FindPort( globeSolo->outEdges, "source", 0 );
					Check( p && std::string( p->otherName.c_str() ) == "lantern_globe" && p->otherNode != SceneEditController::kInvalidNodeIndex,
					       "PART14b: globe_solo.source -> lantern_globe (leaf-collapse instancing) resolves" );
				}
				j->release();
			}
		}

		// ---- (c) synthetic csg_object fixture: obja/objb edges present,
		// rank(composite) > rank(operand) ----
		{
			const char* path = "test_referencegraph_objgraph_csg.RISEscene";
			Job* j = LoadFixture( path,
				"RISE ASCII SCENE 7\n"
				"film\n{\nwidth 32\nheight 24\n}\n"
				"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
				"sphere_geometry\n{\nname gs\nradius 1\n}\n"
				"box_geometry\n{\nname gb\nwidth 1\nheight 1\ndepth 1\n}\n"
				"standard_object\n{\nname opA\ngeometry gs\nposition -1 0 0\n}\n"
				"standard_object\n{\nname opB\ngeometry gb\nposition 1 0 0\n}\n"
				"csg_object\n{\nname combo\noperation union\nobja opA\nobjb opB\n}\n" );
			Check( j != nullptr, "PART14c: synthetic csg fixture loads" );
			if( j ) {
				SceneEditController c( *j, 0 );
				ObjLaidOutGraph lg;
				c.ReadObjectGraphLaidOut( lg );
				const ObjGraph& g = lg.graph;

				const GNode* combo = FindNode( g, ChunkCategory::Object, "combo" );
				const GNode* opA   = FindNode( g, ChunkCategory::Object, "opA" );
				const GNode* opB   = FindNode( g, ChunkCategory::Object, "opB" );
				Check( combo && opA && opB, "PART14c: sanity -- combo/opA/opB all resolve" );
				if( combo ) {
					const GPort* a = FindPort( combo->outEdges, "obja", 0 );
					const GPort* b = FindPort( combo->outEdges, "objb", 0 );
					Check( a && std::string( a->otherName.c_str() ) == "opA" && a->otherNode != SceneEditController::kInvalidNodeIndex,
					       "PART14c: combo.obja -> opA resolves" );
					Check( b && std::string( b->otherName.c_str() ) == "opB" && b->otherNode != SceneEditController::kInvalidNodeIndex,
					       "PART14c: combo.objb -> opB resolves" );
				}
				auto xOf = [&]( const char* name ) -> double {
					for( std::size_t i = 0; i < g.nodes.size(); ++i )
						if( g.nodes[i].category == ChunkCategory::Object && std::string( g.nodes[i].name.c_str() ) == name )
							return lg.positions[i].x;
					return -1.0;
				};
				Check( xOf( "opA" ) >= 0.0 && xOf( "opA" ) < xOf( "combo" ) && xOf( "opB" ) < xOf( "combo" ),
				       "PART14c: rank(combo) > rank(opA) and rank(combo) > rank(opB) -- operands sit LEFT of the composite" );
				j->release();
			}
		}

		// ---- (d) FOCUSED: mid-tree object (`shoulder`) = ancestors
		// (pedestal_root) + descendants (everything under shoulder) + geo
		// (every included object's geometry) -- and EXCLUDES the unrelated
		// sibling subtree (`pedestal`) and the unrelated `floor` root.
		// RED-PROVED below: a combined UP+DOWN worklist (the bug this
		// design deliberately avoids) pulls `pedestal` in too. ----
		{
			const char* path = "scenes/Tests/Geometry/object_parenting.RISEscene";
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path );
			Check( loaded, "PART14d: object_parenting.RISEscene loads" );
			if( loaded ) {
				SceneEditController c( *j, 0 );
				bool degraded = true;
				ObjLaidOutGraph focused;
				c.ReadObjectGraphLaidOutFocused( String( "shoulder" ), focused, &degraded );
				Check( !degraded, "PART14d: a non-contended focused read reports outDegraded == false" );

				auto has = [&]( const char* name ) {
					return FindNode( focused.graph, ChunkCategory::Object, name ) != nullptr
					    || FindNode( focused.graph, ChunkCategory::Geometry, name ) != nullptr;
				};
				Check( has( "shoulder" ), "PART14d: the target itself is present" );
				Check( has( "pedestal_root" ), "PART14d: UP -- the parent is present" );
				Check( has( "upper_arm" ) && has( "shoulder_joint" ) && has( "elbow" )
				    && has( "elbow_joint" ) && has( "fore_arm" ) && has( "hand_root" )
				    && has( "palm" ) && has( "finger_a" ) && has( "finger_b" ) && has( "finger_c" ),
				       "PART14d: DOWN -- every descendant is present" );
				Check( has( "geo_bar" ) && has( "geo_ball" ), "PART14d: GEO -- the descendants' geometry is present" );
				Check( !has( "pedestal" ), "PART14d: EXCLUDES the unrelated SIBLING subtree (pedestal, pedestal_root's OTHER child)" );
				Check( !has( "floor" ) && !has( "geo_floor" ) && !has( "geo_column" ),
				       "PART14d: EXCLUDES the wholly unrelated floor root and its geometry" );
				j->release();
			}
		}

		// ---- (e) FOCUSED: SOURCE is one hop only -- `pair_b` (source
		// lantern) includes `lantern` itself but NOT lantern's own
		// descendants (lantern_post/lantern_head/lantern_globe/lantern_cap) ----
		{
			const char* path = "scenes/Tests/Geometry/object_instancing.RISEscene";
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path );
			Check( loaded, "PART14e: object_instancing.RISEscene loads" );
			if( loaded ) {
				SceneEditController c( *j, 0 );
				ObjLaidOutGraph focused;
				c.ReadObjectGraphLaidOutFocused( String( "pair_b" ), focused, nullptr );
				auto has = [&]( const char* name ) { return FindNode( focused.graph, ChunkCategory::Object, name ) != nullptr; };
				Check( has( "pair_b" ), "PART14e: the target itself is present" );
				Check( has( "lantern" ), "PART14e: SOURCE -- the one-hop source target is present" );
				Check( !has( "lantern_post" ) && !has( "lantern_head" ) && !has( "lantern_globe" ) && !has( "lantern_cap" ),
				       "PART14e: SOURCE does NOT recurse into the source's own subtree (one hop only)" );

				// review-round P3-c: `pair_b`'s source (`lantern`) is a
				// CONTAINER with no geometry of its own, so the "one hop
				// only" claim above is not yet proven against a source that
				// actually HAS geometry to leak. `globe_solo` sources
				// `lantern_globe` directly, and `lantern_globe` carries
				// `geometry geo_globe` -- SOURCE must add `lantern_globe`
				// itself but NOT run GEO on it (GEO only runs over the
				// UP/DOWN/start object set, never over a SOURCE one-hop
				// addition -- see ObjectGraphFocusedClosure's own comment).
				ObjLaidOutGraph focusedLeaf;
				c.ReadObjectGraphLaidOutFocused( String( "globe_solo" ), focusedLeaf, nullptr );
				auto hasLeaf = [&]( const char* name ) {
					return FindNode( focusedLeaf.graph, ChunkCategory::Object, name ) != nullptr
					    || FindNode( focusedLeaf.graph, ChunkCategory::Geometry, name ) != nullptr;
				};
				Check( hasLeaf( "globe_solo" ), "PART14e: (globe_solo) the target itself is present" );
				Check( hasLeaf( "lantern_globe" ), "PART14e: (globe_solo) SOURCE -- the one-hop source target is present" );
				Check( !hasLeaf( "geo_globe" ),
				       "PART14e: (globe_solo) the source target's OWN geometry (geo_globe) does NOT leak in -- SOURCE never runs GEO" );
				j->release();
			}
		}

		// ---- (f) unknown/ambiguous name -> empty, non-degraded ----
		{
			const char* path = "scenes/Tests/Geometry/object_parenting.RISEscene";
			Job* j = new Job();
			const bool loaded = j->LoadAsciiSceneViaCst( path );
			Check( loaded, "PART14f: object_parenting.RISEscene loads" );
			if( loaded ) {
				SceneEditController c( *j, 0 );
				bool degraded = true;
				ObjLaidOutGraph focused;
				c.ReadObjectGraphLaidOutFocused( String( "no_such_object_at_all" ), focused, &degraded );
				Check( !degraded, "PART14f: an unknown name reports outDegraded == false" );
				Check( focused.graph.nodes.empty(), "PART14f: an unknown name resolves to an empty graph" );
				j->release();
			}
		}

		// ---- (g) degraded propagation: a focused read made WHILE a render
		// owns the scene reports outDegraded == true and an empty graph --
		// same RunPreviewRenderParked technique PART12(e)/PART13(d)
		// established. ----
		{
			Job* rj = new Job();
			SceneEditController dc( *rj, /*interactiveRasterizer*/0 );
			std::thread renderThread( [&dc]() {
				dc.RunPreviewRenderParked( []() {
					std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
				} );
			} );

			bool sawRenderOwn = false;
			for( int i = 0; i < 200; ++i ) {
				if( dc.ForTest_RenderOwnsScene() ) { sawRenderOwn = true; break; }
				std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
			}
			Check( sawRenderOwn, "PART14g: the parked render is observed genuinely owning the scene" );

			if( sawRenderOwn ) {
				bool degraded = false;
				ObjLaidOutGraph focused;
				dc.ReadObjectGraphLaidOutFocused( String( "anything" ), focused, &degraded );
				Check( degraded, "PART14g: a focused read made WHILE a render owns the scene reports outDegraded == true" );
				Check( focused.graph.nodes.empty(), "PART14g: a degraded focused read returns an empty graph" );
			}

			renderThread.join();
			rj->release();
		}

		// ---- (h) FOCUSED on the synthetic csg fixture (review-round P2-2):
		// (i) UP's composite-ownership rule -- focusing an OPERAND pulls in
		// the owning composite PLUS the composite's own parent chain, but
		// NOT the composite's OTHER operand (reaching that is a DOWN-only
		// rule -- see ObjectGraphFocusedClosure's own comment for why UP
		// never applies DOWN's rules); (ii) DOWN's own-operand expansion --
		// focusing an ANCESTOR of the composite transitively reaches BOTH
		// operands once the composite itself enters the DOWN worklist, not
		// just the trivial start==composite case. RED-PROVED below
		// (temporarily dropping UP's composite-ownership rule). ----
		{
			const char* path = "test_referencegraph_objgraph_csg_focus.RISEscene";
			Job* j = LoadFixture( path,
				"RISE ASCII SCENE 7\n"
				"film\n{\nwidth 32\nheight 24\n}\n"
				"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
				"sphere_geometry\n{\nname gs\nradius 1\n}\n"
				"box_geometry\n{\nname gb\nwidth 1\nheight 1\ndepth 1\n}\n"
				"standard_object\n{\nname root\nposition 0 0 0\n}\n"
				"standard_object\n{\nname opA\ngeometry gs\nposition -1 0 0\n}\n"
				"standard_object\n{\nname opB\ngeometry gb\nposition 1 0 0\n}\n"
				"csg_object\n{\nname combo\nparent root\noperation union\nobja opA\nobjb opB\n}\n" );
			Check( j != nullptr, "PART14h: synthetic csg-focus fixture loads" );
			if( j ) {
				SceneEditController c( *j, 0 );

				// (i) focus opA (an operand).
				{
					ObjLaidOutGraph focused;
					c.ReadObjectGraphLaidOutFocused( String( "opA" ), focused, nullptr );
					auto has = [&]( const char* name ) {
						return FindNode( focused.graph, ChunkCategory::Object, name ) != nullptr
						    || FindNode( focused.graph, ChunkCategory::Geometry, name ) != nullptr;
					};
					Check( has( "opA" ), "PART14h(i): the target itself is present" );
					Check( has( "combo" ), "PART14h(i): UP -- the owning composite is present" );
					Check( has( "root" ), "PART14h(i): UP -- the composite's own parent is present (transitive climb)" );
					Check( has( "gs" ), "PART14h(i): GEO -- opA's own geometry is present" );
					Check( !has( "opB" ), "PART14h(i): EXCLUDES the composite's OTHER operand (a DOWN-only rule, not reachable climbing UP)" );
					Check( !has( "gb" ), "PART14h(i): EXCLUDES opB's own geometry too" );
				}

				// (ii) focus root (an ancestor of the composite) -- DOWN
				// must transitively reach BOTH operands once `combo` itself
				// enters the DOWN worklist.
				{
					ObjLaidOutGraph focused;
					c.ReadObjectGraphLaidOutFocused( String( "root" ), focused, nullptr );
					auto has = [&]( const char* name ) {
						return FindNode( focused.graph, ChunkCategory::Object, name ) != nullptr
						    || FindNode( focused.graph, ChunkCategory::Geometry, name ) != nullptr;
					};
					Check( has( "root" ), "PART14h(ii): the target itself is present" );
					Check( has( "combo" ), "PART14h(ii): DOWN -- the child composite is present" );
					Check( has( "opA" ) && has( "opB" ),
					       "PART14h(ii): DOWN -- BOTH of the composite's own operands are present (own-operand expansion, transitive through an ancestor focus)" );
					Check( has( "gs" ) && has( "gb" ), "PART14h(ii): GEO -- both operands' geometry is present" );
				}
				j->release();
			}
		}

		// ---- (i) repeatCount edge cases (review-round P2-1/P3-b): a count
		// may be `expr(...)` over a document `let`, and `count_u 1` ALONE
		// (no `count_v`) is a genuine repeat of size 1, not "uncounted". ----
		{
			const char* path = "test_referencegraph_objgraph_repeatcount.RISEscene";
			Job* j = LoadFixture( path,
				"RISE ASCII SCENE 7\n"
				"film\n{\nwidth 32\nheight 24\n}\n"
				"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
				"let\n{\nN 4\n}\n"
				"standard_object\n{\nname base\nposition 0 0 0\n}\n"
				"standard_object\n{\nname exprRow\nsource base\ncount_u expr(N+1)\nposition 0 0 0\n}\n"
				"standard_object\n{\nname oneRow\nsource base\ncount_u 1\nposition 0 0 0\n}\n" );
			Check( j != nullptr, "PART14i: repeatCount edge-case fixture loads" );
			if( j ) {
				SceneEditController c( *j, 0 );
				ObjGraph g;
				c.ReadObjectGraph( g );
				const GNode* exprRow = FindNode( g, ChunkCategory::Object, "exprRow" );
				const GNode* oneRow  = FindNode( g, ChunkCategory::Object, "oneRow" );
				Check( exprRow && oneRow, "PART14i: sanity -- both nodes resolve" );
				if( exprRow )
					Check( exprRow->repeatCount == 5,
					       "PART14i: `count_u expr(N+1)` with let N=4 evaluates to repeatCount 5, NOT 0 (a bare strtoull on the raw text would read 0)" );
				if( oneRow )
					Check( oneRow->repeatCount == 1,
					       "PART14i: `count_u 1` alone is a genuine repeat of size 1 -- PRESENCE selects the repeated form, so this publishes 1, not 0" );
				j->release();
			}
		}

		// ---- (j) doc 89 slice A (loft): a MORPHED sweep_geometry still
		// lands in the object graph as a well-formed Geometry node with the
		// object's `geometry` edge resolving and NO dangling port.  Slice A
		// adds no cross-chunk pointer (profile2 and the morph track are
		// inline numbers, not references), so this is the guard that it
		// stayed that way: the loft node's edge lists must hold EXACTLY the
		// one geometry->consumer edge and nothing else.
		//
		// FIX-ROUND CORRECTION to what this row claims to catch.  The
		// earlier comment said a BARE-STRING pointer (the thing the doc's
		// rule 1 forbids) "would show up as an edge that is MISSING" and
		// leaned on the no-dangling-port assertion for it.  It cannot:
		// SceneReferenceGraph only emits edges for DECLARED references, so an
		// undeclared bare-string pointer produces no edge at all -- nothing
		// dangles, and nothing fails.  What IS mechanically caught here is a
		// declared reference appearing (or the existing one disappearing),
		// which is why the counts below are asserted explicitly instead of
		// only scanning the ports that happen to exist.  Rule 1's bare-string
		// case is caught by the descriptor's own ValueKind, not by this
		// graph. ----
		{
			const char* path = "test_referencegraph_objgraph_loft.RISEscene";
			Job* j = LoadFixture( path,
				"RISE ASCII SCENE 7\n"
				"film\n{\nwidth 32\nheight 24\n}\n"
				"pinhole_camera\n{\nname cam\nlocation 0 0 10\nlookat 0 0 0\n}\n"
				"sweep_geometry\n{\nname loftg\nprofile_circle 0.6 16\nprofile2_rect 0.9 0.5\n"
				"point 0 0 0\npoint 0 1.5 0\npoint 0 3 0\n"
				"point_morph 0\npoint_morph 0.4\npoint_morph 1\n"
				"point_scale 1.0 0.7\npoint_scale 0.9 0.5\npoint_scale 0.7 0.45\n}\n"
				"standard_object\n{\nname loftobj\ngeometry loftg\nposition 0 0 0\n}\n" );
			Check( j != nullptr, "PART14j: morphed-sweep fixture loads" );
			if( j ) {
				SceneEditController c( *j, 0 );
				ObjGraph g;
				c.ReadObjectGraph( g );
				const GNode* geo = FindNode( g, ChunkCategory::Geometry, "loftg" );
				const GNode* obj = FindNode( g, ChunkCategory::Object,   "loftobj" );
				Check( geo != 0, "PART14j: the morphed sweep_geometry becomes a Geometry node" );
				Check( obj != 0, "PART14j: its consumer becomes an Object node" );
				if( obj ) {
					// the object CONSUMES the geometry, so the edge is published
					// on the object's inEdges (PART14a's pedestal_root row pins
					// the same convention from the negative side)
					const GPort* p = FindPort( obj->inEdges, "geometry", 0 );
					Check( p && std::string( p->otherName.c_str() ) == "loftg" &&
					       p->otherNode != SceneEditController::kInvalidNodeIndex,
					       "PART14j: loftobj.geometry -> loftg resolves (no dangling port)" );
				}
				if( geo ) {
					bool anyDangling = false;
					for( const GPort& p : geo->outEdges ) {
						if( p.otherNode == SceneEditController::kInvalidNodeIndex ) anyDangling = true;
					}
					for( const GPort& p : geo->inEdges ) {
						if( p.otherNode == SceneEditController::kInvalidNodeIndex ) anyDangling = true;
					}
					Check( !anyDangling,
					       "PART14j: the loft chunk publishes no dangling port" );
					// The MONEY assertions: EXACT counts.  ReadObjectGraph
					// FLIPS the geometry family at seeding (geometry chunk ->
					// consumer), so the loft geometry owns exactly ONE
					// out-edge -- to loftobj -- and NO in-edge.  A new
					// declared Reference on the loft grammar (a painter slot,
					// a profile source chunk) moves one of these numbers, and
					// so does silently dropping the geometry edge.
					if( geo->outEdges.size() == 1 ) {
						Check( std::string( geo->outEdges[0].otherName.c_str() ) == "loftobj" &&
						       std::string( geo->outEdges[0].paramName.c_str() ) == "geometry",
						       "PART14j: the loft geometry's single out-edge is the flipped `geometry` edge to its consumer" );
					} else {
						Check( false, "PART14j: the loft geometry's single out-edge is the flipped `geometry` edge to its consumer" );
					}
					Check( geo->outEdges.size() == 1,
					       "PART14j: MONEY ASSERTION -- the loft chunk publishes EXACTLY ONE out-edge "
					       "(slice A added no cross-chunk reference; profile2 and point_morph are inline numbers)" );
					Check( geo->inEdges.size() == 0,
					       "PART14j: MONEY ASSERTION -- the loft chunk publishes NO in-edge "
					       "(nothing in the scene refers INTO a geometry chunk in this graph's direction)" );
				}
				j->release();
			}
		}
	}

	std::cout << "Passed: " << passCount << ", Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
