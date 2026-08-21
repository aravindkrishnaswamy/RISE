//////////////////////////////////////////////////////////////////////
//
//  GraphCanvasEditSmokeTest.cpp - doc-88 Phase 3 S21:
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 6 S21's "headless probe" --
//    pins the WHOLE Mac canvas edit data path by driving the SAME
//    SceneEditController verbs the RISEViewportBridge Obj-C methods
//    call verbatim (see build/XCode/rise/RISE-GUI/Bridge/
//    RISEViewportBridge.mm: -createChunkNodeWithKeyword:...,
//    -checkConnectionWithTargetCategory:..., -wouldCycleFromCategory:...,
//    -rewireConnectionWithTargetCategory:..., -duplicateGraphNodeWithCategory:...,
//    -deleteGraphNodeWithCategory:..., -writeGraphNodeLayoutPositionWithName:...,
//    -paletteKeywordsForCategory: are every one of them a THIN
//    passthrough to the controller method this file calls directly).
//    A green run here means the Swift canvas's whole edit surface --
//    palette, create, legality pre-check, wire, drag-persist, duplicate,
//    delete -- is wired correctly all the way down to the CST, headless,
//    with no Xcode / AppKit / GUI process required.
//
//  FLOW (one continuous scenario, not independent cases -- this probe
//  is a SMOKE test of the composed path, not a policy exerciser; S17's
//  ConnectionLegality corpus, S18's EntityTemplatesTest, S19's
//  RewireConnectionTest, and S20's DeleteDuplicateGraphNodeTest already
//  own the exhaustive per-verb policy coverage):
//
//    1. PALETTE      -- PaletteKeywords(Painter/Function/Material) is
//                        non-empty and category-pure; contains the
//                        keywords this scenario goes on to use.
//    2. CREATE        -- CreateChunkNode a new Painter with no required
//                        args; SceneEpoch ADVANCES.
//    3. LEGALITY      -- CheckConnection accepts the legal wire this
//                        scenario is about to commit; a deliberately
//                        ILLEGAL one (a Material as a Painter reference)
//                        is refused with a non-empty diagnostic; neither
//                        pure-read call advances SceneEpoch.
//    4. CYCLE         -- WouldCycle is false for the wire about to be
//                        committed and true for a wire that would close
//                        one; SceneEpoch unmoved by either.
//    5. WIRE          -- RewireConnection commits the checked wire as
//                        ONE undoable step; SceneEpoch ADVANCES; the
//                        rewired-away painter is reported nowUnreferenced.
//    6. REPOSITION    -- WriteGraphLayoutPositions (the bridge's
//                        -writeGraphNodeLayoutPosition passthrough)
//                        persists the new painter's canvas position;
//                        ReadPainterMaterialGraphLaidOut echoes it back
//                        exactly, and SceneEpoch is UNMOVED (the sidecar
//                        is UI state, not scene semantics -- sect. 1.3).
//    7. DUPLICATE     -- DuplicateGraphNode forks the new painter;
//                        SceneEpoch ADVANCES; the fork resolves and is
//                        independently wireable.
//    8. DELETE        -- DeleteGraphNode removes the now-orphaned
//                        painter the wire stepped away from;
//                        SceneEpoch ADVANCES.
//    9. FINAL SHAPE   -- ReadPainterMaterialGraphLaidOut's node set
//                        matches exactly what steps 2-8 predict: the
//                        orphan is gone, the new painter and its fork
//                        both present, the material's edge points at
//                        the new painter.
//
//  Self-contained: OIDN off, no render pass ever started, one temp
//  scene file (removed at exit either way).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/ChunkDescriptorRegistry.h"
#include "../src/Library/SceneEditor/ConnectionLegality.h"
#include "../src/Library/SceneEditor/OwnershipClosure.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int g_pass = 0, g_fail = 0;
	void Check( bool c, const std::string& what )
	{
		if( c ) { ++g_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else    { ++g_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	std::string TempPath( const char* name )
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		return dir + name;
	}

	//! Load `text` into a fresh Job through a real on-disk path (so
	//! `WriteGraphLayoutPositions` has a scene path to key its sidecar
	//! by -- step 6 would otherwise hit the documented "unsaved scene"
	//! no-op and prove nothing). Caller owns the returned Job.
	Job* LoadScene( const std::string& text, const std::string& path )
	{
		{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
		Job* pJob = new Job();
		if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) )
		{
			pJob->release();
			std::remove( path.c_str() );
			return nullptr;
		}
		return pJob;
	}

	const SceneEditController::GraphNode* FindNode(
		const SceneEditController::PainterMaterialGraph& g,
		ChunkCategory category, const std::string& name )
	{
		for( const SceneEditController::GraphNode& n : g.nodes ) {
			if( n.category == category && std::string( n.name.c_str() ) == name ) return &n;
		}
		return nullptr;
	}

	const char* const kScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_red\ncolor 1 0 0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_white\ncolor 1 1 1\n}\n\n"
		"blend_painter\n{\nname blend_main\ncolora pnt_red\ncolorb pnt_white\nmask pnt_white\n}\n\n"
		"lambertian_material\n{\nname mat_main\nreflectance blend_main\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_main\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";
}

int main()
{
	std::printf( "GraphCanvasEditSmokeTest\n" );

	const std::string path = TempPath( "test_graph_canvas_edit_smoke.RISEscene" );
	Job* j = LoadScene( kScene, path );
	Check( j != nullptr, "fixture scene loads" );
	if( !j ) { std::printf( "\n%d passed, %d FAILED\n", g_pass, g_fail ); return g_fail ? 1 : 0; }

	SceneEditController c( *j, nullptr );

	// =====================================================================
	// 1. PALETTE -- the "add node" search list the Swift canvas populates
	//    its palette from (RISEViewportBridge -paletteKeywords(forCategory:)).
	// =====================================================================
	{
		const std::vector<String> painters  = c.PaletteKeywords( ChunkCategory::Painter );
		const std::vector<String> materials = c.PaletteKeywords( ChunkCategory::Material );
		Check( !painters.empty(), "PART1: Painter palette is non-empty" );
		Check( !materials.empty(), "PART1: Material palette is non-empty" );

		auto contains = []( const std::vector<String>& v, const char* s ) {
			for( const String& x : v ) if( std::string( x.c_str() ) == s ) return true;
			return false;
		};
		Check( contains( painters, "perlin3d_painter" ), "PART1: Painter palette lists perlin3d_painter" );
		Check( contains( materials, "lambertian_material" ), "PART1: Material palette lists lambertian_material" );
		Check( !contains( painters, "lambertian_material" ), "PART1: Painter palette does NOT leak a Material keyword" );

		// Sorted, matching the descriptor-registry doc comment (a stable
		// order a search palette can filter over without re-sorting).
		bool sorted = true;
		for( std::size_t i = 1; i < painters.size(); ++i ) {
			if( std::strcmp( painters[i - 1].c_str(), painters[i].c_str() ) > 0 ) { sorted = false; break; }
		}
		Check( sorted, "PART1: Painter palette is lexicographically sorted" );

		// Cross-check against ChunkDescriptorRegistry directly -- the
		// controller method must not silently diverge from what it wraps.
		const std::vector<String> direct = AllKeywordsForCategory( ChunkCategory::Painter );
		Check( direct.size() == painters.size(), "PART1: controller passthrough matches the registry directly" );
	}

	unsigned int epoch = c.SceneEpoch();

	// =====================================================================
	// 2. CREATE -- a new Painter node with no required args (the palette's
	//    zero-arg keyword case; the required-arg / file-slot pre-flight is
	//    S18's own EntityTemplatesTest coverage, not re-tested here).
	// =====================================================================
	String newPainterName;
	{
		const SceneEditController::AgentCommitResult r =
			c.CreateChunkNode( String( "perlin3d_painter" ), String( "canvas_noise" ), {}, &newPainterName );
		Check( r.applied, "PART2: CreateChunkNode(perlin3d_painter) applies" );
		Check( std::string( newPainterName.c_str() ) == "canvas_noise",
			"PART2: the new node landed under the requested base name (no collision to dedupe)" );

		const unsigned int e2 = c.SceneEpoch();
		Check( e2 != epoch, "PART2: SceneEpoch ADVANCES on a structural create" );
		epoch = e2;
	}

	// =====================================================================
	// 3. LEGALITY -- CheckConnection pre-validates BEFORE any commit; a
	//    pure read never advances SceneEpoch.
	// =====================================================================
	{
		const ConnectionVerdict legal = c.CheckConnection(
			ChunkCategory::Material, String( "mat_main" ), String( "reflectance" ),
			ChunkCategory::Painter, newPainterName );
		Check( legal.legal, "PART3: wiring the new painter into mat_main.reflectance is LEGAL" );
		Check( legal.diagnostic.empty(), "PART3: a legal verdict carries no diagnostic" );

		// The deliberately illegal candidate: a Material has no Color-pipe
		// registration at all, so binding one into a painter reference slot
		// must be refused with a non-empty, real diagnostic (S17's whole
		// point -- a canvas-rejected drop reads like the parser's own).
		const ConnectionVerdict illegal = c.CheckConnection(
			ChunkCategory::Material, String( "mat_main" ), String( "reflectance" ),
			ChunkCategory::Material, String( "mat_main" ) );
		Check( !illegal.legal, "PART3: binding a Material into a Painter reference slot is ILLEGAL" );
		Check( !illegal.diagnostic.empty(), "PART3: the illegal verdict carries a real diagnostic" );

		Check( c.SceneEpoch() == epoch, "PART3: CheckConnection never advances SceneEpoch" );
	}

	// =====================================================================
	// 4. CYCLE -- WouldCycle, also a pure read.
	// =====================================================================
	{
		Check( !c.WouldCycle( ChunkCategory::Material, String( "mat_main" ),
		                      ChunkCategory::Painter, newPainterName ),
			"PART4: wiring mat_main -> canvas_noise (a fresh leaf) does not cycle" );
		// blend_main currently reaches mat_main? No -- edges run the other
		// way (mat_main -> blend_main). The genuinely cyclic probe: would
		// wiring blend_main to reference mat_main close a loop back through
		// mat_main's own (about-to-be-replaced) edge to blend_main? Today
		// mat_main -> blend_main exists, so blend_main -> mat_main would.
		Check( c.WouldCycle( ChunkCategory::Painter, String( "blend_main" ),
		                     ChunkCategory::Material, String( "mat_main" ) ),
			"PART4: wiring blend_main -> mat_main WOULD cycle (mat_main already reaches blend_main)" );
		Check( c.SceneEpoch() == epoch, "PART4: WouldCycle never advances SceneEpoch" );
	}

	// =====================================================================
	// 5. WIRE -- RewireConnection commits the checked wire.
	// =====================================================================
	{
		const SceneEditController::RewireResult r = c.RewireConnection(
			ChunkCategory::Material, String( "mat_main" ), String( "reflectance" ), 0,
			ChunkCategory::Painter, newPainterName, nullptr );
		Check( r.commit.applied, "PART5: RewireConnection applies" );
		Check( r.closure == ClosureClassification::Clean, "PART5: Clean closure (mat_main solely owns its own slot)" );
		Check( !r.legalityRefused && !r.cycleRefused, "PART5: neither refusal gate fired" );

		bool orphanedBlend = false;
		for( const String& n : r.nowUnreferenced ) if( std::string( n.c_str() ) == "blend_main" ) orphanedBlend = true;
		Check( orphanedBlend, "PART5: blend_main is reported nowUnreferenced (mat_main was its only referrer)" );

		const unsigned int e5 = c.SceneEpoch();
		Check( e5 != epoch, "PART5: SceneEpoch ADVANCES on the wire commit" );
		epoch = e5;
	}

	// =====================================================================
	// 6. REPOSITION -- the drag-to-reposition commit path (bridge's
	//    -writeGraphNodeLayoutPosition, one node per call).
	// =====================================================================
	{
		std::vector<SceneEditController::GraphNodePositionUpdate> updates;
		SceneEditController::GraphNodePositionUpdate u;
		u.name = newPainterName; u.x = 321.0; u.y = 654.0;
		updates.push_back( u );
		std::string werr;
		Check( c.WriteGraphLayoutPositions( updates, werr ), "PART6: WriteGraphLayoutPositions succeeds" );
		Check( werr.empty(), "PART6: no error message on success" );

		SceneEditController::PainterMaterialGraphLaidOut laidOut;
		c.ReadPainterMaterialGraphLaidOut( laidOut );
		const SceneEditController::GraphNode* np = FindNode( laidOut.graph, ChunkCategory::Painter, newPainterName.c_str() );
		Check( np != nullptr, "PART6: the repositioned node is still present" );
		if( np ) {
			const unsigned int idx = SceneEditController::ResolveGraphNodeHandle( laidOut.graph, np->handle );
			Check( idx < laidOut.positions.size()
			    && laidOut.positions[idx].x == 321.0 && laidOut.positions[idx].y == 654.0,
				"PART6: the saved position is echoed back EXACTLY" );
		}

		Check( c.SceneEpoch() == epoch, "PART6: a layout-sidecar write does NOT advance SceneEpoch (UI state, not scene semantics)" );
	}

	// =====================================================================
	// 7. DUPLICATE -- Cmd-D.
	// =====================================================================
	String dupName;
	{
		const SceneEditController::DuplicateResult r =
			c.DuplicateGraphNode( ChunkCategory::Painter, newPainterName, nullptr );
		Check( r.commit.applied, "PART7: DuplicateGraphNode applies" );
		Check( !r.newName.empty() && std::string( r.newName.c_str() ) != std::string( newPainterName.c_str() ),
			"PART7: the fork landed under a DIFFERENT, non-empty name" );
		dupName = r.newName;

		const unsigned int e7 = c.SceneEpoch();
		Check( e7 != epoch, "PART7: SceneEpoch ADVANCES on the duplicate" );
		epoch = e7;

		// The fork is independently addressable/wireable -- proven with a
		// LEGALITY READ, not an actual rewire: CheckConnection is a pure
		// read (PART3 already pins that it never advances SceneEpoch), so
		// this is a cheap "not a dangling copy" check with nothing to
		// revert afterward. (Corrected S21 review round 1 P3 -- an earlier
		// draft of this comment claimed a rewire-then-revert-back, which
		// the code below never did.)
		const ConnectionVerdict v = c.CheckConnection(
			ChunkCategory::Material, String( "mat_main" ), String( "reflectance" ),
			ChunkCategory::Painter, dupName );
		Check( v.legal, "PART7: the freshly duplicated node is itself a legal rewire candidate" );
	}

	// =====================================================================
	// 8. DELETE -- the orphan RewireConnection stepped away from in PART5.
	// =====================================================================
	{
		const SceneEditController::DeleteResult r = c.DeleteGraphNode(
			ChunkCategory::Painter, String( "blend_main" ),
			SceneEditController::GraphDeleteMode::TargetOnly, nullptr );
		Check( r.commit.applied, "PART8: DeleteGraphNode(blend_main) applies (it is the orphan PART5 made)" );
		Check( !r.referenceRefused, "PART8: no reference refusal (it really had zero referrers)" );
		Check( r.removed.size() == 1 && std::string( r.removed[0].c_str() ) == "blend_main",
			"PART8: exactly blend_main was removed" );

		const unsigned int e8 = c.SceneEpoch();
		Check( e8 != epoch, "PART8: SceneEpoch ADVANCES on the delete" );
		epoch = e8;
	}

	// =====================================================================
	// 9. FINAL SHAPE -- the composed data path's end state, read back
	//    exactly like the Swift canvas's -painterMaterialGraph call would.
	// =====================================================================
	{
		SceneEditController::PainterMaterialGraphLaidOut laidOut;
		c.ReadPainterMaterialGraphLaidOut( laidOut );

		Check( FindNode( laidOut.graph, ChunkCategory::Painter, "blend_main" ) == nullptr,
			"PART9: blend_main (deleted) is gone from the graph" );
		// TargetOnly deletes ONLY the named node -- pnt_red/pnt_white
		// (blend_main's own references) are NOT swept, so both survive as
		// now-orphaned leaves. Asserting their SURVIVAL, not their
		// removal, is the honest read of GraphDeleteMode::TargetOnly
		// (a Cascade delete is a different, already-covered verb --
		// DeleteDuplicateGraphNodeTest PART 1c/1d own that policy).
		Check( FindNode( laidOut.graph, ChunkCategory::Painter, "pnt_red" ) != nullptr,
			"PART9: pnt_red survives -- TargetOnly does not cascade below blend_main" );
		const SceneEditController::GraphNode* np  = FindNode( laidOut.graph, ChunkCategory::Painter, newPainterName.c_str() );
		const SceneEditController::GraphNode* dup = FindNode( laidOut.graph, ChunkCategory::Painter, dupName.c_str() );
		const SceneEditController::GraphNode* mat = FindNode( laidOut.graph, ChunkCategory::Material, "mat_main" );
		Check( np != nullptr, "PART9: the created painter is present" );
		Check( dup != nullptr, "PART9: its duplicate is present" );
		Check( mat != nullptr, "PART9: mat_main is present" );
		if( mat && np ) {
			bool pointsAtNew = false;
			for( const SceneEditController::GraphPort& p : mat->outEdges ) {
				if( std::string( p.paramName.c_str() ) == "reflectance"
				 && std::string( p.otherName.c_str() ) == std::string( newPainterName.c_str() ) ) {
					pointsAtNew = true;
				}
			}
			Check( pointsAtNew, "PART9: mat_main.reflectance still points at the new painter (PART7's legality read is a pure check -- it never touched this wire)" );
		}
	}

	// =====================================================================
	// 10. SAVE ROUND-TRIP (S23 regression pass) -- the CstSaveFidelityTest
	//     invariant, but exercised over content that PARTS 2-8 produced
	//     through the canvas verbs themselves (create+rewire+duplicate+
	//     delete), not through a hand-authored fixture.  Two properties:
	//     (a) SerializeCst(retained head) is itself a stable fixpoint
	//     (re-parsing it and re-serializing changes nothing -- the same
	//     "unedited round-trip is byte-exact" invariant CstSaveFidelityTest
	//     case A pins on a static fixture, pinned here on a document whose
	//     every byte was produced by SceneEditController mutations); (b) a
	//     real save-to-disk + fresh-Job-reload (the actual GUI "Save" then
	//     "Open" pair) reproduces the identical bytes -- no verb left the
	//     retained Document in a state that reserializes differently once
	//     it has round-tripped through a file.
	// =====================================================================
	{
		const Cst::Document* pHead = j->GetCstDocument();
		Check( pHead != nullptr, "PART10: the live Job still retains its CST head after the edit sequence" );
		if( pHead )
		{
			const std::string savedText = Cst::SerializeCst( *pHead );
			Check( !savedText.empty(), "PART10: the edited document serializes to non-empty text" );

			// (a) Parse -> Serialize is a fixpoint on canvas-authored content.
			const std::string reparsed = Cst::SerializeCst( Cst::ParseToCst( savedText ) );
			Check( reparsed == savedText,
				"PART10a: re-parsing the canvas-edited save text and re-serializing is byte-identical (no drift)" );

			// (b) A genuine save-to-disk + fresh-Job reload round-trips the same bytes.
			const std::string reloadPath = TempPath( "test_graph_canvas_edit_smoke_reload.RISEscene" );
			Job* pReloaded = LoadScene( savedText, reloadPath );
			Check( pReloaded != nullptr, "PART10b: the saved canvas-edited scene reloads into a fresh Job" );
			if( pReloaded )
			{
				const Cst::Document* pReloadedHead = pReloaded->GetCstDocument();
				Check( pReloadedHead != nullptr, "PART10b: the reloaded Job retains a CST head" );
				if( pReloadedHead )
				{
					const std::string reloadedText = Cst::SerializeCst( *pReloadedHead );
					Check( reloadedText == savedText,
						"PART10b: save -> reload -> re-serialize reproduces the identical bytes" );
				}
				pReloaded->release();
			}
			std::remove( reloadPath.c_str() );
		}
	}

	j->release();
	std::remove( path.c_str() );

	std::printf( "\n%d passed, %d FAILED\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
