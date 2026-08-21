//////////////////////////////////////////////////////////////////////
//
//  DeleteDuplicateGraphNodeTest.cpp - doc-88 Phase 3 S20:
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 6's "S20 -- Cycle/orphan/
//    copy-vs-link enforcement", which implements
//    docs/gui/ENTITY_CREATION.md sect. 5's reference-safe delete policy and
//    ships docs/gui/MATERIAL_EDITOR.md sect. 3.7a's Duplicate-node escape
//    hatch (which S19 could only NAME in a diagnostic).
//
//  WHY ITS OWN FILE rather than more of RewireConnectionTest.cpp (S19's
//  home).  Same failure-attribution argument that file makes for itself:
//  these are DIFFERENT verbs with DIFFERENT policies (block-or-cascade
//  deletion; positioned forking) needing DIFFERENT purpose-built fixtures
//  (a solely-owned CHAIN to sweep; a shared leaf that must survive a
//  sweep; a chunk with no `name` param; a genuinely CYCLIC scene file).
//  The ONE case that genuinely belongs to S19's file -- the 1c' escape
//  hatch, which is that file's own standing marker rewritten end to end
//  now that S20 closed the gap -- stayed there, where the marker was.
//
//  PARTS
//
//    PART 1 -- REFERENCE-SAFE DELETE (ENTITY_CREATION.md sect. 5).
//        1a. ZERO referrers -> deletes cleanly, and ONLY the target's
//            bytes go: the whole rest of the document is byte-identical,
//            rebuilt-with-the-chunk-excised and compared.
//        1b. REFERRERS EXIST -> REFUSED in BOTH modes, byte-identical,
//            naming EVERY referrer as `chunk`.`param` plus the escapes --
//            asserted by FORMATTING the shared kDeleteReferencedFmt
//            symbol, never a copied literal (the S17 drift lesson).
//        1c. CASCADE sweeps the target AND its solely-owned closure, in
//            DOCUMENT ORDER, as ONE undoable composite; a chunk another
//            graph still uses is NOT swept and survives.
//        1d. CASCADE's shared-guard: the positive half (a shared leaf
//            survives a sweep that reaches it) is asserted here; the
//            guard's REFUSAL arm is unreachable by construction from any
//            fixture -- see the case's own comment -- and is RED-PROVED
//            instead.
//        1e. AMBIGUOUS name (the S11 same-name colour/scalar pair) ->
//            REFUSED, never deleted on a guess; UNRESOLVED -> refused.
//        1f. A chunk kind the editor cannot name-address is refused
//            rather than erased.
//        1g. The sweep-MEMBER addressability arm (as opposed to 1f's
//            target-addressability arm) IS reachable from a real fixture --
//            a same-name-same-kind collision the GRAPH resolves first-wins
//            but the EDITOR'S OWN addressing refuses as ambiguous -- so it
//            is pinned directly here rather than red-proved.
//
//    PART 2 -- THE POSITIONED DUPLICATE.
//        2a. THE POSITION INVARIANT: the copy's bytes sit strictly
//            between the original's and the first consumer's.  RED-PROVED.
//        2b. Name collision: duplicating twice yields two distinct names,
//            both resolvable, neither colliding with an unrelated chunk
//            that already holds the obvious suffix.
//        2c. UNDO/REDO byte-identity across the positioned splice.
//        2d. A non-graph-node kind (a light) is refused with its keyword
//            named -- this verb forks graph nodes only.
//        2e. SHALLOW: the copy's reference values are the original's,
//            character for character.
//        2f. LAST-item splice: duplicating the file's FINAL chunk, which
//            has NO trailing newline (the "sepC arm"), incl. undo/redo
//            byte-exactness.
//        2g. CROSS-CATEGORY name squat: the dedup pick retries past a
//            same-name chunk in an UNRELATED category instead of wedging
//            on the first doc-wide collision.
//
//    PART 3 -- CYCLES: THE DERIVE-LAYER FINDING, PINNED.
//      S20's brief asks whether a cycle-creating edit hangs the derive,
//      is refused by it, or is silently accepted -- because the answer
//      determines whether the commit-layer check is defense in depth or
//      the cover for a latent engine bug.  It is REFUSED, cleanly, at
//      every gate.  Each of the following is that answer pinned, so the
//      day one of them stops being true is the day a test fails.
//        3a. A CYCLIC scene FILE does not load at all -- DeriveToJob
//            binds in declaration order, so the cycle's forward reference
//            simply does not resolve.  No hang, no half-bound graph.
//        3b. `propose_patch` closing a cycle is REFUSED by the agent
//            full-derivability gate, document byte-identical.
//        3c. A SELF-reference is refused the same way.
//        3d. The GUI property-panel path refuses a forward reference too
//            (WouldPersistDanglingReference_'s declaration-order clause) --
//            the one reference-retargeting path that reaches the UNCHECKED
//            Job::ApplyCstParamEdit, pinned so it stays covered.
//        3e. At the CANVAS layer the same edit is refused EARLIER and
//            better, by ConnectionLegality::WouldCycle -- with a
//            cycle-specific diagnostic instead of an opaque "would not
//            derive".  That is the value of the commit-layer check.
//        3f. And on a FRESHLY DUPLICATED node: wiring the copy into its
//            own upstream is cycle-refused, so S20's new verb cannot be
//            used to smuggle one in.
//
//    PART 4 -- COMMIT DISCIPLINE inherited from the remove / document-swap
//      verbs, asserted here because a composer that forgot to route
//      through them would silently lose each one:
//        4a. UNDO/REDO byte-identity for a plain delete, for a CASCADE
//            (one Cmd-Z restores every swept chunk), and for a duplicate.
//        4b. SCENE-EPOCH bumps on every mutating outcome.
//        4c. MID-TRANSACTION refusal (retriable, byte-identical) for both
//            verbs.
//        4d. HEAD-VERSION conflict refuses without mutating.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes, OIDN off,
//  no render pass is ever started.
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
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/SceneEditor/ConnectionLegality.h"
#include "../src/Library/SceneEditor/OwnershipClosure.h"
#include "../src/Library/SceneEditor/ReferenceGraph.h"
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

	//! The whole retained document, byte-exact -- THE instrument every
	//! byte-identity assertion in this file uses.
	std::string DocText( const Job& j )
	{
		const RISE::Cst::Document* d = j.GetCstDocument();
		return d ? RISE::Cst::SerializeCst( *d ) : std::string();
	}

	bool Contains( const std::string& hay, const std::string& needle )
	{
		return hay.find( needle ) != std::string::npos;
	}

	//! Byte offset of a chunk's `name <n>` line, or npos.  The position
	//! assertions compare THESE, so "after the original" is a statement
	//! about the document's actual bytes, not about an index the verb
	//! reported about itself.
	size_t NameLineAt( const std::string& doc, const char* n )
	{
		return doc.find( std::string( "name " ) + n + "\n" );
	}

	//! THE fixture.  Laid out so every S20 distinction is present at once:
	//!
	//!   ORPHAN          pnt_orphan is referenced by nothing (PART 1a's
	//!                   clean-delete subject).
	//!   SOLELY-OWNED    mat_chain -> blend_chain -> {pnt_leaf_a, pnt_leaf_b}
	//!   CHAIN           and blend_chain -> blend_inner -> pnt_leaf_a.  Once
	//!                   mat_chain lets go, cascade-deleting blend_chain must
	//!                   sweep blend_chain + blend_inner + pnt_leaf_a +
	//!                   pnt_leaf_b, and NOTHING else -- a multi-level sweep,
	//!                   not a one-hop one, and one whose middle node
	//!                   (pnt_leaf_a) has TWO referrers that are BOTH inside
	//!                   the sweep (the case a single-pass worklist gets
	//!                   wrong depending on edge order).
	//!   SHARED LEAF     pnt_shared is referenced from blend_chain.mask AND
	//!                   from mat_other.reflectance -- it must SURVIVE the
	//!                   sweep above (PART 1c/1d).
	//!   REFERENCED      blend_chain has a referrer (mat_chain) until it is
	//!                   rewired away -- PART 1b's refusal subject, with TWO
	//!                   referrers once mat_second is added.
	//!   SAME-NAME PAIR  a colour painter and a `scalar_painter` both named
	//!                   `dupe` -- the S11 ambiguity PART 1e refuses on.
	//!   NEIGHBOUR       lgt_witness / geo_witness are unrelated chunks whose
	//!                   bytes must never move.
	const char* const kScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_white\ncolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_orphan\ncolor 0.2 0.2 0.2\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_leaf_a\ncolor 1 0 0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_leaf_b\ncolor 0 1 0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_shared\ncolor 0.5 0.5 0.5\n}\n\n"
		"uniformcolor_painter\n{\nname dupe\ncolor 0 0 1\n}\n\n"
		"scalar_painter\n{\nname dupe\nvalue 0.25\n}\n\n"
		"blend_painter\n{\nname blend_inner\ncolora pnt_leaf_a\ncolorb pnt_leaf_b\nmask pnt_white\n}\n\n"
		"blend_painter\n{\nname blend_chain\ncolora pnt_leaf_a\ncolorb blend_inner\nmask pnt_shared\n}\n\n"
		"lambertian_material\n{\nname mat_chain\nreflectance blend_chain\n}\n\n"
		"lambertian_material\n{\nname mat_second\nreflectance blend_chain\n}\n\n"
		"lambertian_material\n{\nname mat_other\nreflectance pnt_shared\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_chain\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	//! A scene whose reference graph is CYCLIC as authored: A names B and B
	//! names A.  PART 3a hands this to the real loader.
	const char* const kCyclicScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_white\ncolor 1 1 1\n}\n\n"
		"blend_painter\n{\nname cyc_a\ncolora cyc_b\ncolorb pnt_white\nmask pnt_white\n}\n\n"
		"blend_painter\n{\nname cyc_b\ncolora cyc_a\ncolorb pnt_white\nmask pnt_white\n}\n\n"
		"lambertian_material\n{\nname mat_cyc\nreflectance cyc_a\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_cyc\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	//! The ACYCLIC twin of the above: cyc_a is declared FIRST and references
	//! only leaves; cyc_b references cyc_a.  Re-pointing cyc_a at cyc_b would
	//! CLOSE the cycle -- the edit PART 3b/3e attempt through two different
	//! surfaces.
	const char* const kChainScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_white\ncolor 1 1 1\n}\n\n"
		"blend_painter\n{\nname cyc_a\ncolora pnt_white\ncolorb pnt_white\nmask pnt_white\n}\n\n"
		"blend_painter\n{\nname cyc_b\ncolora cyc_a\ncolorb pnt_white\nmask pnt_white\n}\n\n"
		"lambertian_material\n{\nname mat_cyc\nreflectance cyc_b\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_cyc\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	//! A MATERIAL declared BEFORE a painter.  Re-pointing that material at
	//! the later painter is a FORWARD reference -- the shape PART 3d drives
	//! through the GUI property-panel path, which is the only
	//! reference-retargeting caller of the UNCHECKED Job::ApplyCstParamEdit.
	const char* const kMatBeforePainterScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_early\ncolor 1 0 0\n}\n\n"
		"lambertian_material\n{\nname mat_early\nreflectance pnt_early\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_late\ncolor 0 0 1\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_early\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	//! A scene whose LAST top-level item is a graph-node Painter with NO
	//! trailing newline -- `DuplicateGraphNode`'s "sepC arm"
	//! (SceneEditController.cpp ~16315-16320: "If the original was the
	//! document's LAST item ... give the file its trailing newline back").
	//! `pnt_last` is deliberately an ORPHAN (no referrer) so the fixture
	//! carries no forward reference for the loader to refuse (PART 3's
	//! declaration-order rule) -- the only property this fixture needs to
	//! exercise is "the original resolves, and it is the document's final
	//! byte-for-byte item" (S20 review round 1 P2-5a).
	const char* const kLastNoNewlineScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_early\ncolor 1 0 0\n}\n\n"
		"lambertian_material\n{\nname mat_witness\nreflectance pnt_early\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_witness\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_last\ncolor 0.4 0.4 0.4\n}";   // NO trailing \n -- the last document byte is `}`

	//! A scene that reaches the CASCADE sweep's ADDRESSABILITY check on a
	//! MEMBER other than the target -- believed unreachable by the
	//! reviewer's inductive argument (every sweep member is reached via a
	//! real NAME-bound reference edge, so it must resolve by name ...), but
	//! that argument conflates two DIFFERENT name-resolution rules (S20
	//! review round 1 P2-5b):
	//!
	//!   * `Cst::BuildReferenceGraph` (the GRAPH `blend_amb.colorb` binds
	//!     through) resolves a same-category same-name collision FIRST-WINS
	//!     -- `defs`, keyed by (category, name), keeps only the first
	//!     `dupe` it sees, so `colorb dupe` deterministically binds the
	//!     first-declared `uniformcolor_painter` and the scene loads fine.
	//!   * `Cst::DocFindByNameAnyRole` (the EDITOR's own addressing, which
	//!     the addressability loop re-verifies against) treats ANY same-
	//!     name-same-kind collision as ambiguous and refuses to resolve it
	//!     at all -- it does not know or care which one the graph bound.
	//!
	//! So a sweep can legitimately reach a member that the GRAPH addresses
	//! fine but the EDITOR refuses to touch: `dupe` here is swept as
	//! `blend_amb`'s solely-owned child (once `mat_amb` is rewired off
	//! `blend_amb`), and a second same-name `scalar_painter` elsewhere in
	//! the document -- never referenced by anything, existing purely to
	//! make the NAME ambiguous -- is what trips `kDeleteUnaddressableFmt`
	//! for that member.  `pnt_neutral` is the rewire's destination, kept
	//! deliberately outside `blend_amb`'s subgraph so it is never swept.
	const char* const kAmbiguousSweepMemberScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_neutral\ncolor 0.9 0.9 0.9\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_leaf\ncolor 1 0 0\n}\n\n"
		"uniformcolor_painter\n{\nname dupe\ncolor 0 0 1\n}\n\n"
		"scalar_painter\n{\nname dupe\nvalue 0.5\n}\n\n"
		"blend_painter\n{\nname blend_amb\ncolora pnt_leaf\ncolorb dupe\nmask pnt_leaf\n}\n\n"
		"lambertian_material\n{\nname mat_amb\nreflectance blend_amb\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_amb\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	//! `DuplicateGraphNode`'s name pick reproduced against a CROSS-CATEGORY
	//! name squat (S20 review round 1 P2-3).  `squat_target` is a Painter;
	//! duplicating it wants the base name itself (taken -- it IS the
	//! original), then its first numeric suffix `squat_target_2` -- which a
	//! `sphere_geometry` chunk, a completely different category, already
	//! holds.  `UniqueEntityName`'s old in-category-only pick could not see
	//! that collision (it only enumerates the live PAINTER manager), so it
	//! handed back `squat_target_2` believing it free; the doc-wide check
	//! then caught the squat but had no retry left, and refused outright.
	//! The fix folds both checks into one predicate with a real retry loop,
	//! so this now dedups PAST the squatted `_2` to `squat_target_3`.
	const char* const kCrossCategorySquatScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname squat_target\ncolor 1 0 0\n}\n\n"
		"sphere_geometry\n{\nname squat_target_2\nradius 0.5\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"lambertian_material\n{\nname mat_witness\nreflectance squat_target\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_witness\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	typedef SceneEditController::DeleteResult    Del;
	typedef SceneEditController::DuplicateResult Dup;
	typedef SceneEditController::GraphDeleteMode Mode;

	//! The still-referenced refusal text, built from the SAME
	//! `inline constexpr` symbol the .cpp formats from -- so this test
	//! cannot pass by agreeing with a stale copy of the wording.
	std::string ExpectedReferencedDiagnostic( const std::string& target,
	                                           const std::vector<String>& referrers )
	{
		std::string joined;
		for( std::size_t i = 0; i < referrers.size(); ++i ) {
			if( i ) joined += ( i + 1 == referrers.size() ) ? " and " : ", ";
			joined += referrers[i].c_str();
		}
		char buf[1536];
		std::snprintf( buf, sizeof( buf ), kDeleteReferencedFmt,
			target.c_str(), joined.c_str(), target.c_str() );
		return buf;
	}

	//! Rewire mat_chain / mat_second off blend_chain so the chain becomes
	//! unreferenced -- the setup every cascade case shares.  Uses only the
	//! shipped S19 verb, so a cascade test can never pass because its own
	//! setup mutated the document some private way.
	bool ReleaseTheChain( SceneEditController& c )
	{
		const SceneEditController::RewireResult a = c.RewireConnection(
			ChunkCategory::Material, String( "mat_chain" ), String( "reflectance" ), 0,
			ChunkCategory::Painter, String( "pnt_white" ), nullptr );
		const SceneEditController::RewireResult b = c.RewireConnection(
			ChunkCategory::Material, String( "mat_second" ), String( "reflectance" ), 0,
			ChunkCategory::Painter, String( "pnt_white" ), nullptr );
		return a.commit.applied && b.commit.applied;
	}
}

// =====================================================================
// PART 1 -- reference-safe delete
// =====================================================================

static void Part1_ReferenceSafeDelete()
{
	std::printf( "\nPART 1 -- reference-safe delete (ENTITY_CREATION.md sect. 5)\n" );

	// ---- 1a. ZERO referrers -> clean delete --------------------------
	{
		const std::string path = TempPath( "test_s20_p1a.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "1a: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			Check( SceneReferenceGraph::FindReferencesTo(
				*j->GetCstDocument(), ChunkCategory::Painter, String( "pnt_orphan" ) ).empty(),
				"1a: pnt_orphan really has zero referrers (the premise, asserted not assumed)" );

			const Del r = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "pnt_orphan" ), Mode::TargetOnly, nullptr );

			Check( r.commit.applied, "1a: an unreferenced node DELETES cleanly" );
			Check( r.closure == ClosureClassification::Clean, "1a: classification is Clean" );
			Check( !r.referenceRefused && !r.cascadeRefused, "1a: neither policy refusal fired" );
			Check( r.referrers.empty(), "1a: no referrers reported" );
			Check( r.removed.size() == 1
			    && std::string( r.removed[0].c_str() ) == "pnt_orphan",
				"1a: exactly ONE chunk reported removed, and it is the target" );

			const std::string after = DocText( *j );
			Check( !Contains( after, "name pnt_orphan\n" ), "1a: the chunk is gone from the document" );
			// Only the target's bytes moved: rebuild `before` with the chunk
			// excised and demand equality.  A stray re-emit anywhere -- a
			// neighbour's trivia, the witness light, the trailing newline --
			// fails here.
			{
				std::string expected = before;
				const std::string block =
					"uniformcolor_painter\n{\nname pnt_orphan\ncolor 0.2 0.2 0.2\n}\n\n";
				const size_t at = expected.find( block );
				Check( at != std::string::npos, "1a: the pre-delete pnt_orphan block is where the test expects it" );
				if( at != std::string::npos ) expected.erase( at, block.size() );
				Check( after == expected,
					"1a: EVERY other byte is identical -- only the target's block left" );
			}

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1b. REFERRERS EXIST -> REFUSE, naming every one -------------
	{
		const std::string path = TempPath( "test_s20_p1b.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "1b: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			for( int pass = 0; pass < 2; ++pass ) {
				const Mode mode = pass ? Mode::Cascade : Mode::TargetOnly;
				const char* label = pass ? "Cascade" : "TargetOnly";
				const Del r = c.DeleteGraphNode(
					ChunkCategory::Painter, String( "blend_chain" ), mode, nullptr );

				Check( !r.commit.applied,
					std::string( "1b (" ) + label + "): a referenced node is REFUSED" );
				Check( std::string( r.commit.status.c_str() ) == "rejected",
					std::string( "1b (" ) + label + "): status is `rejected`" );
				Check( !r.commit.retriable,
					std::string( "1b (" ) + label + "): the refusal is PERMANENT" );
				Check( r.referenceRefused,
					std::string( "1b (" ) + label + "): `referenceRefused` is the machine-readable reason" );
				Check( !r.cascadeRefused,
					std::string( "1b (" ) + label + "): and it is NOT reported as a cascade refusal" );
				Check( r.removed.empty(),
					std::string( "1b (" ) + label + "): nothing is reported removed" );
				Check( DocText( *j ) == before,
					std::string( "1b (" ) + label + "): the document is BYTE-IDENTICAL after the refusal" );

				// EVERY referrer, as `chunk`.`param`.
				Check( r.referrers.size() == 2,
					std::string( "1b (" ) + label + "): BOTH referrers are named" );
				bool sawChain = false, sawSecond = false;
				for( std::size_t i = 0; i < r.referrers.size(); ++i ) {
					const std::string s( r.referrers[i].c_str() );
					if( s == "mat_chain.reflectance" )  sawChain  = true;
					if( s == "mat_second.reflectance" ) sawSecond = true;
				}
				Check( sawChain && sawSecond,
					std::string( "1b (" ) + label + "): each names its chunk AND the param that binds it" );

				// The wording, from the SHARED format symbol.
				const std::string expected = ExpectedReferencedDiagnostic( "blend_chain", r.referrers );
				Check( Contains( std::string( r.commit.message.c_str() ), expected ),
					std::string( "1b (" ) + label + "): the message IS the shared-format diagnostic, "
					"escapes included" );
			}

			// Cascade is named in the message as explicitly NOT an escape --
			// because it is not one, and pointing a user at a mode that
			// refuses identically would be worse than saying nothing.
			const Del r = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), Mode::TargetOnly, nullptr );
			Check( Contains( std::string( r.commit.message.c_str() ), "Cascade mode does not help here" ),
				"1b: the message is explicit that cascade is NOT one of the escapes" );
			Check( Contains( std::string( r.commit.message.c_str() ), "Rewire those references away first" ),
				"1b: ...and names the escape that IS one" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1c. CASCADE sweeps the solely-owned closure -----------------
	{
		const std::string path = TempPath( "test_s20_p1c.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "1c: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			Check( ReleaseTheChain( c ), "1c: setup -- both materials let go of blend_chain" );
			const std::string before = DocText( *j );

			const Del r = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), Mode::Cascade, nullptr );

			Check( r.commit.applied, "1c: the cascade APPLIES" );
			Check( !r.cascadeRefused && !r.referenceRefused, "1c: neither policy refusal fired" );

			// THE SWEEP, exactly: the target, the interior node below it, and
			// both leaves only that subgraph used.  pnt_shared (mat_other also
			// binds it) and pnt_white (mat_chain/mat_second now bind it) must
			// NOT be in the set.
			std::vector<std::string> got;
			for( std::size_t i = 0; i < r.removed.size(); ++i ) got.push_back( r.removed[i].c_str() );
			Check( got.size() == 4, "1c: exactly FOUR chunks swept" );
			bool a = false, b = false, inner = false, chain = false, badShared = false, badWhite = false;
			for( std::size_t i = 0; i < got.size(); ++i ) {
				if( got[i] == "pnt_leaf_a" )  a = true;
				if( got[i] == "pnt_leaf_b" )  b = true;
				if( got[i] == "blend_inner" ) inner = true;
				if( got[i] == "blend_chain" ) chain = true;
				if( got[i] == "pnt_shared" )  badShared = true;
				if( got[i] == "pnt_white" )   badWhite = true;
			}
			Check( a && b && inner && chain,
				"1c: the sweep is the WHOLE solely-owned closure, not just the one hop below the target "
				"(pnt_leaf_a has two referrers, both inside the sweep -- the case a single-pass "
				"worklist gets wrong depending on edge order)" );
			Check( !badShared && !badWhite,
				"1c: and it stops at the chunks other graphs still use" );

			// DOCUMENT ORDER is part of the contract.
			const std::string after = DocText( *j );
			{
				bool ordered = true;
				size_t prev = 0;
				for( std::size_t i = 0; i < got.size(); ++i ) {
					const size_t at = NameLineAt( before, got[i].c_str() );
					if( at == std::string::npos || ( i && at < prev ) ) ordered = false;
					prev = at;
				}
				Check( ordered, "1c: `removed` is in DOCUMENT ORDER, as its contract says" );
			}

			// Every swept chunk is gone; every survivor is still there,
			// byte-identical in its own block.
			Check( !Contains( after, "name blend_chain\n" ) && !Contains( after, "name blend_inner\n" )
			    && !Contains( after, "name pnt_leaf_a\n" ) && !Contains( after, "name pnt_leaf_b\n" ),
				"1c: every swept chunk is gone from the document" );
			Check( Contains( after, "uniformcolor_painter\n{\nname pnt_shared\ncolor 0.5 0.5 0.5\n}" ),
				"1c: the SHARED leaf survived, bytes untouched" );
			Check( Contains( after, "lambertian_material\n{\nname mat_other\nreflectance pnt_shared\n}" ),
				"1c: ...and so did the other graph that binds it" );
			Check( Contains( after, "omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}" ),
				"1c: an unrelated neighbour's bytes never moved" );

			// ONE undoable composite -- the headline property.
			c.Undo();
			Check( DocText( *j ) == before,
				"1c: ONE Undo restores EVERY swept chunk, byte-identically (the composite is one step)" );
			c.Redo();
			Check( DocText( *j ) == after, "1c: ...and one Redo re-applies the whole sweep byte-identically" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1d. THE SHARED-CASCADE GUARD --------------------------------
	//
	// The guard exists so a shared chunk can NEVER be swept as collateral.
	// Its refusal arm is UNREACHABLE from any fixture, and that is a
	// property worth stating rather than hiding: the sweep admits a chunk
	// only when EVERY referrer of it is already in the sweep, and every
	// sweep member is (inductively) owned solely by the target -- so a
	// correctly-built sweep can never contain a shared chunk for the guard
	// to catch.  The guard is therefore a CROSS-CHECK of the sweep by an
	// independent function (OwnershipClosure::OwnersOf), not a case a
	// scene can drive into.  What a test CAN assert is the positive half,
	// below; the refusal arm is RED-PROVED by breaking the sweep's
	// all-referrers-in-sweep condition and watching the guard fire.
	{
		const std::string path = TempPath( "test_s20_p1d.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "1d: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			Check( ReleaseTheChain( c ), "1d: setup -- both materials let go of blend_chain" );

			// pnt_shared is REACHED by the sweep (blend_chain.mask names it)
			// and is owned by TWO roots -- assert BOTH halves of the premise
			// through the shared ownership rule itself, so this case cannot
			// silently degrade into "a chunk the sweep never even reached".
			{
				const RISE::Cst::Document* d = j->GetCstDocument();
				const RISE::Cst::NodeId sharedId =
					SceneReferenceGraph::ResolveChunk( *d, ChunkCategory::Painter, String( "pnt_shared" ) );
				Check( sharedId != 0, "1d: pnt_shared resolves" );
				const std::vector<ReferenceEdge> refs = SceneReferenceGraph::FindReferencesTo(
					*d, ChunkCategory::Painter, String( "pnt_shared" ) );
				Check( refs.size() == 2,
					"1d: pnt_shared IS reached from inside the doomed subgraph AND from outside it" );
				const std::vector<RISE::Cst::NodeId> owners = OwnershipClosure::OwnersOf( *d, sharedId );
				Check( owners.size() == 2,
					"1d: ...and the shared ownership rule agrees it has TWO owning roots" );
			}

			const Del r = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), Mode::Cascade, nullptr );
			Check( r.commit.applied, "1d: the cascade applies (the shared leaf does not block it)" );
			bool swept = false;
			for( std::size_t i = 0; i < r.removed.size(); ++i )
				if( std::string( r.removed[i].c_str() ) == "pnt_shared" ) swept = true;
			Check( !swept, "1d: the SHARED chunk is not swept -- it is simply left alone" );
			Check( Contains( DocText( *j ), "name pnt_shared\n" ),
				"1d: ...and it is still in the document afterwards" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1e. AMBIGUOUS / UNRESOLVED names refuse ---------------------
	{
		const std::string path = TempPath( "test_s20_p1e.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "1e: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			// `dupe` is BOTH a colour painter and a scalar_painter -- both
			// ChunkCategory::Painter.  Deleting on a guess would erase an
			// arbitrary one of them.
			const Del amb = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "dupe" ), Mode::TargetOnly, nullptr );
			Check( !amb.commit.applied, "1e: an AMBIGUOUS name is REFUSED, never deleted on a guess" );
			Check( amb.closure == ClosureClassification::AmbiguousTargetName,
				"1e: ...classified AmbiguousTargetName" );
			Check( DocText( *j ) == before, "1e: ...document byte-identical" );
			{
				char buf[512];
				std::snprintf( buf, sizeof( buf ), kClosureAmbiguousFmt, "dupe", 2 );
				Check( Contains( std::string( amb.commit.message.c_str() ), buf ),
					"1e: ...with the SHARED ambiguity diagnostic, formatted from its own symbol" );
			}

			const Dup ambDup = c.DuplicateGraphNode( ChunkCategory::Painter, String( "dupe" ), nullptr );
			Check( !ambDup.commit.applied && ambDup.closure == ClosureClassification::AmbiguousTargetName,
				"1e: DuplicateGraphNode refuses the same ambiguity (it would fork an arbitrary one)" );
			Check( DocText( *j ) == before, "1e: ...document byte-identical" );

			const Del miss = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "no_such_painter" ), Mode::Cascade, nullptr );
			Check( !miss.commit.applied && miss.closure == ClosureClassification::UnresolvedTarget,
				"1e: an UNRESOLVED name is refused as UnresolvedTarget" );
			Check( DocText( *j ) == before, "1e: ...document byte-identical" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1f. a kind with no name-addressed edit path ------------------
	{
		const std::string path = TempPath( "test_s20_p1f.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "1f: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			// A Shader-category chunk: the graph can name it, the editor has
			// no (category, name) removal addressing for it.  Refuse rather
			// than erase something the checks could not verify.
			const Del r = c.DeleteGraphNode(
				ChunkCategory::Shader, String( "global" ), Mode::TargetOnly, nullptr );
			Check( !r.commit.applied, "1f: a chunk the editor cannot name-address is REFUSED" );
			Check( DocText( *j ) == before, "1f: ...document byte-identical" );
			// `global` resolves fine (ChunkCategory::Shader is a real graph
			// category; SceneReferenceGraph::ResolveChunk finds it and it has
			// no referrers), so this refuses at the ADDRESSABILITY step, not
			// resolution: ChunkCategory::Shader has no entry in
			// UiCategoryForChunkCategory (falls through to Category::None),
			// so `kDeleteUnaddressableFmt` fires -- the single arm the earlier
			// disjunctive assertion here left unpinned (S20 review round 1
			// P2-1).  Per DeleteResult::closure's own contract, an
			// addressability refusal is NOT a closure verdict, so `closure`
			// stays the default `Clean` here (it does not mean "clean delete";
			// read `commit.applied` for that, exactly as the header instructs).
			Check( Contains( std::string( r.commit.message.c_str() ), "cannot be addressed for removal" ),
				"1f: ...with the UNADDRESSABLE diagnostic naming the reason, not a silent skip" );
			Check( r.closure == ClosureClassification::Clean,
				"1f: ...and closure stays Clean (unaddressable is not a closure verdict -- read commit.applied)" );
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1g. the sweep-MEMBER addressability arm -- REACHABLE, not
	// red-proved.  See kAmbiguousSweepMemberScene's own comment: the graph
	// binds `blend_amb.colorb` to the first-declared `dupe` (first-wins
	// same-name resolution), but the EDITOR's own by-name addressing
	// (`DocFindByNameAnyRole`) refuses ANY same-name-same-kind collision,
	// so the cascade sweep's cross-check hits `kDeleteUnaddressableFmt` on
	// `dupe` -- a MEMBER of the sweep, not the delete target itself
	// (S20 review round 1 P2-5b).
	{
		const std::string path = TempPath( "test_s20_p1g.RISEscene" );
		Job* j = LoadScene( kAmbiguousSweepMemberScene, path );
		Check( j != nullptr, "1g: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			// Let go of blend_amb so the cascade target has zero referrers.
			const SceneEditController::RewireResult rw = c.RewireConnection(
				ChunkCategory::Material, String( "mat_amb" ), String( "reflectance" ), 0,
				ChunkCategory::Painter, String( "pnt_neutral" ), nullptr );
			Check( rw.commit.applied, "1g: setup -- mat_amb lets go of blend_amb" );

			// Premise check, through the shared ownership rule itself: `dupe`
			// really is reached by the sweep (blend_amb.colorb names it) and
			// really is solely owned by blend_amb -- so the cascade's OwnersOf
			// cross-check admits it into the sweep, and the ONLY reason it can
			// still refuse is the addressability re-check, not ownership.
			{
				const RISE::Cst::Document* d = j->GetCstDocument();
				const RISE::Cst::NodeId blendId =
					SceneReferenceGraph::ResolveChunk( *d, ChunkCategory::Painter, String( "blend_amb" ) );
				Check( blendId != 0, "1g: blend_amb resolves (unambiguous -- only ITS name collides for `dupe`)" );
				const std::vector<ReferenceEdge> fromBlend =
					SceneReferenceGraph::FindReferencesFrom( *d, ChunkCategory::Painter, String( "blend_amb" ) );
				bool namesDupe = false;
				for( std::size_t i = 0; i < fromBlend.size(); ++i )
					if( std::string( fromBlend[i].targetName.c_str() ) == "dupe" ) namesDupe = true;
				Check( namesDupe, "1g: blend_amb.colorb really does bind (by NodeId) to a chunk named dupe" );
			}

			const Del r = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "blend_amb" ), Mode::Cascade, nullptr );
			Check( !r.commit.applied,
				"1g: the cascade is REFUSED -- the sweep reached an ambiguously-named member" );
			Check( DocText( *j ) != before,
				"1g: ...document is NOT the original bytes (the setup rewire above committed) but the "
				"cascade attempt itself mutated nothing further" );
			const std::string afterSetup = DocText( *j );
			Check( Contains( std::string( r.commit.message.c_str() ), "cannot be addressed for removal" )
			    && Contains( std::string( r.commit.message.c_str() ), "dupe" ),
				"1g: ...naming `dupe` -- the SWEEP MEMBER, not `blend_amb` the requested target" );
			Check( r.cascadeRefused == false,
				"1g: ...classified as an UNADDRESSABLE refusal, not a shared-cascade refusal -- "
				"they are different guards (kDeleteUnaddressableFmt vs kDeleteCascadeSharedFmt)" );

			// Re-running the identical cascade request is a no-op on the
			// document: the refusal is PERMANENT (retriable=false), same
			// contract as every other policy refusal in this file.
			const Del r2 = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "blend_amb" ), Mode::Cascade, nullptr );
			Check( !r2.commit.applied && !r2.commit.retriable,
				"1g: ...and the refusal is PERMANENT, not retriable" );
			Check( DocText( *j ) == afterSetup, "1g: ...document still byte-identical to after setup" );

			j->release();
			std::remove( path.c_str() );
		}
	}
}

// =====================================================================
// PART 2 -- the positioned duplicate
// =====================================================================

static void Part2_PositionedDuplicate()
{
	std::printf( "\nPART 2 -- the positioned Duplicate-node fork\n" );

	// ---- 2a. THE POSITION INVARIANT ----------------------------------
	{
		const std::string path = TempPath( "test_s20_p2a.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "2a: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			const size_t origBefore = NameLineAt( before, "blend_chain" );

			const Dup r = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), nullptr );
			Check( r.commit.applied, "2a: the fork applies" );
			Check( r.closure == ClosureClassification::Clean, "2a: classification is Clean" );
			Check( r.newName.size() > 1, "2a: a name is reported back" );
			Check( r.originalIndex >= 0, "2a: the original's document index is reported" );

			const std::string after = DocText( *j );
			const size_t origAt = NameLineAt( after, "blend_chain" );
			const size_t copyAt = NameLineAt( after, r.newName.c_str() );
			const size_t consumerAt = NameLineAt( after, "mat_chain" );

			Check( origAt != std::string::npos && copyAt != std::string::npos
			    && consumerAt != std::string::npos,
				"2a: original, copy and consumer are all present" );
			Check( origAt < copyAt,
				"2a: THE INVARIANT -- the copy is AFTER the original in declaration order" );
			Check( copyAt < consumerAt,
				"2a: ...and BEFORE the original's consumers, so every one of them can legally "
				"reference it (which is the point: an appended copy cannot be referenced at all)" );

			// IMMEDIATELY after: nothing but separator bytes between the
			// original's closing brace and the copy's keyword.  This is the
			// stronger half of the invariant, and the half a dependency-order
			// heuristic would NOT satisfy.
			{
				const size_t origClose = after.find( "\n}\n", origAt );
				Check( origClose != std::string::npos, "2a: the original's block closes" );
				if( origClose != std::string::npos ) {
					const std::string between = after.substr(
						origClose + 3, ( copyAt > origClose + 3 ) ? ( copyAt - origClose - 3 ) : 0 );
					// Between them: the copy's own `blend_painter\n{\n` header
					// and separator whitespace, nothing else -- no other chunk.
					Check( !Contains( between, "}" ),
						"2a: IMMEDIATELY after -- no other chunk sits between the original and its copy" );
				}
			}

			// The original itself did not move relative to the head of the
			// document: everything before it is byte-identical.
			Check( origBefore != std::string::npos && origAt == origBefore,
				"2a: the original's own byte offset is unchanged -- the splice is purely additive, "
				"after it" );
			Check( before.substr( 0, origBefore ) == after.substr( 0, origAt ),
				"2a: ...and every byte ahead of the original is identical" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2b. name collision safety ------------------------------------
	{
		const std::string path = TempPath( "test_s20_p2b.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "2b: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const Dup a = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), nullptr );
			const Dup b = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), nullptr );
			Check( a.commit.applied && b.commit.applied, "2b: duplicating twice both apply" );
			Check( std::string( a.newName.c_str() ) != std::string( b.newName.c_str() ),
				"2b: the two copies get DISTINCT names" );
			Check( std::string( a.newName.c_str() ) != "blend_chain"
			    && std::string( b.newName.c_str() ) != "blend_chain",
				"2b: neither reuses the original's name" );
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( SceneReferenceGraph::ResolveChunk( *d, ChunkCategory::Painter, a.newName ) != 0
			    && SceneReferenceGraph::ResolveChunk( *d, ChunkCategory::Painter, b.newName ) != 0,
				"2b: BOTH names resolve to real, unambiguous Painter chunks (a collision would have "
				"made ResolveChunk refuse as ambiguous)" );
			// And the second copy is positioned relative to the ORIGINAL it was
			// asked for, not relative to the first copy.
			const std::string after = DocText( *j );
			Check( NameLineAt( after, "blend_chain" ) < NameLineAt( after, b.newName.c_str() ),
				"2b: the second copy is also placed after the original" );
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2c. UNDO / REDO byte-identity --------------------------------
	{
		const std::string path = TempPath( "test_s20_p2c.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "2c: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			const Dup r = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), nullptr );
			Check( r.commit.applied, "2c: the fork applies" );
			const std::string afterEdit = DocText( *j );
			Check( afterEdit != before, "2c: ...and really changed the document" );

			c.Undo();
			Check( DocText( *j ) == before,
				"2c: UNDO restores the ORIGINAL bytes exactly -- including the separator trivia the "
				"splice introduced (the reason this verb records a document PAIR and not an insert)" );
			c.Redo();
			Check( DocText( *j ) == afterEdit,
				"2c: REDO restores the post-fork bytes exactly -- at the SAME position, which a redo "
				"through the tier-positioned insert path could not guarantee" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2d. non-graph-node kinds are refused -------------------------
	{
		const std::string path = TempPath( "test_s20_p2d.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "2d: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			const Dup r = c.DuplicateGraphNode(
				ChunkCategory::Light, String( "lgt_witness" ), nullptr );
			Check( !r.commit.applied, "2d: a LIGHT is refused -- this verb forks graph nodes" );
			Check( DocText( *j ) == before, "2d: ...document byte-identical" );
			Check( Contains( std::string( r.commit.message.c_str() ), "omni_light" )
			    && Contains( std::string( r.commit.message.c_str() ), "GRAPH NODES" )
			    && Contains( std::string( r.commit.message.c_str() ), "outliner's Duplicate" ),
				"2d: ...naming the actual keyword and pointing at the outliner's Duplicate instead" );
			Check( r.newName.size() <= 1 && r.originalIndex == -1,
				"2d: ...and reports no name / index, since nothing landed" );
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2e. SHALLOW: the copy shares, it does not fork the subgraph ----
	{
		const std::string path = TempPath( "test_s20_p2e.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "2e: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::size_t chunksBefore =
				SceneReferenceGraph::AllChunks( *j->GetCstDocument() ).size();
			const Dup r = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), nullptr );
			Check( r.commit.applied, "2e: the fork applies" );

			const RISE::Cst::Document* d = j->GetCstDocument();
			const std::vector<ReferenceEdge> from =
				SceneReferenceGraph::FindReferencesFrom( *d, ChunkCategory::Painter, r.newName );
			Check( from.size() == 3, "2e: the copy has the original's three out-edges" );
			bool leafA = false, inner = false, shared = false;
			for( std::size_t i = 0; i < from.size(); ++i ) {
				const std::string t( from[i].targetName.c_str() );
				if( t == "pnt_leaf_a" )  leafA  = true;
				if( t == "blend_inner" ) inner  = true;
				if( t == "pnt_shared" )  shared = true;
			}
			Check( leafA && inner && shared,
				"2e: ...pointing at the SAME chunks -- the fork unshares exactly ONE level" );
			Check( SceneReferenceGraph::AllChunks( *d ).size() == chunksBefore + 1,
				"2e: exactly ONE top-level chunk was added -- a DEEP copy would have forked the whole "
				"reachable subgraph (blend_inner + three painters) instead" );

			// And the sharing is real both ways: the original's referents now
			// have one MORE referrer each, not a private clone.
			Check( SceneReferenceGraph::FindReferencesTo(
				*d, ChunkCategory::Painter, String( "blend_inner" ) ).size() == 2,
				"2e: the shared child now has TWO referrers (original and copy), not two children" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2f. LAST-item splice: duplicating the file's FINAL chunk, which
	// has NO trailing newline (the "sepC arm" -- SceneEditController.cpp's
	// `DuplicateGraphNode`, "If the original was the document's LAST item
	// ... give the file its trailing newline back").  A probe on this tree
	// showed the splice already lands correctly; this pins it, including
	// undo/redo byte-exactness, so the arm has real coverage rather than
	// none (S20 review round 1 P2-5a).
	{
		const std::string path = TempPath( "test_s20_p2f.RISEscene" );
		const std::string fixtureText = kLastNoNewlineScene;
		Check( !fixtureText.empty() && fixtureText.back() != '\n',
			"2f: fixture sanity -- the authored scene text itself has no trailing newline" );
		Job* j = LoadScene( fixtureText, path );
		Check( j != nullptr, "2f: fixture loads (no trailing newline is legal RISE ASCII)" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			Check( !before.empty() && before.back() != '\n',
				"2f: ...and the RETAINED document preserves that -- pnt_last really is the "
				"document's final byte-for-byte item, not re-terminated on load" );

			const Dup r = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "pnt_last" ), nullptr );
			Check( r.commit.applied, "2f: the fork of the LAST, newline-less chunk applies" );
			const std::string afterEdit = DocText( *j );
			Check( afterEdit != before, "2f: ...and really changed the document" );
			Check( !afterEdit.empty() && afterEdit.back() == '\n',
				"2f: ...the sepC arm gives the file its trailing newline back now that the "
				"copy, not the original, is the document's last item" );
			Check( Contains( afterEdit, "name pnt_last\n" ),
				"2f: ...the original survives, now followed by a real separator (no `}pnt_last` glue)" );
			Check( Contains( afterEdit, "name " + std::string( r.newName.c_str() ) + "\n" ),
				"2f: ...and the deduped copy is present under its landed name" );
			Check( RISE::Cst::SerializeCst( RISE::Cst::ParseToCst( afterEdit ) ) == afterEdit,
				"2f: ...the result round-trips (well-formed CST, not just well-formed-looking text)" );

			c.Undo();
			Check( DocText( *j ) == before,
				"2f: UNDO restores the ORIGINAL bytes exactly -- including the ABSENT trailing newline" );
			c.Redo();
			Check( DocText( *j ) == afterEdit,
				"2f: REDO restores the post-fork bytes exactly, trailing newline included" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2g. CROSS-CATEGORY name squat: the dedup retries past it -----
	// See kCrossCategorySquatScene's own comment for the full mechanics
	// (S20 review round 1 P2-3).  `squat_target_2` is a `sphere_geometry`,
	// not a Painter -- the OLD in-category-only pick could not see it, so
	// the fix under test is specifically that the retry loop tries the
	// NEXT suffix rather than refusing on the first doc-wide collision.
	{
		const std::string path = TempPath( "test_s20_p2g.RISEscene" );
		Job* j = LoadScene( kCrossCategorySquatScene, path );
		Check( j != nullptr, "2g: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );

			// Premise check: `squat_target_2` really does exist, and it
			// really is a DIFFERENT category (Geometry) than the Painter
			// being duplicated -- so this is not just "1e's ambiguous name"
			// in disguise.
			{
				const RISE::Cst::Document* d = j->GetCstDocument();
				const RISE::Cst::NodeId squatId = SceneReferenceGraph::ResolveChunk(
					*d, ChunkCategory::Geometry, String( "squat_target_2" ) );
				Check( squatId != 0,
					"2g: squat_target_2 (a sphere_geometry) resolves -- the cross-category squat is real" );
			}

			const Dup r = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "squat_target" ), nullptr );
			Check( r.commit.applied,
				"2g: the duplicate APPLIES -- it does not wedge on the cross-category squat" );
			Check( std::string( r.newName.c_str() ) == "squat_target_3",
				"2g: ...and dedups PAST the squatted `_2` straight to `_3`, the next name that is "
				"actually free in BOTH the in-category AND doc-wide sense" );
			Check( Contains( DocText( *j ), "name squat_target_3\n" ),
				"2g: ...the landed name is really in the document" );
			// The squat itself is untouched -- this verb works AROUND a
			// collision, it does not resolve one by clobbering anything.
			Check( Contains( DocText( *j ), "sphere_geometry\n{\nname squat_target_2\n" ),
				"2g: ...and the squatting sphere_geometry survives byte-exact" );

			j->release();
			std::remove( path.c_str() );
		}
	}
}

// =====================================================================
// PART 3 -- cycles: the derive-layer finding, pinned
// =====================================================================

static void Part3_Cycles()
{
	std::printf( "\nPART 3 -- cycle enforcement, and where it actually lives\n" );

	// ---- 3a. a CYCLIC scene FILE does not load ------------------------
	{
		const std::string path = TempPath( "test_s20_p3a.RISEscene" );
		Job* j = LoadScene( kCyclicScene, path );
		Check( j == nullptr,
			"3a: THE DERIVE-LAYER ANSWER -- a scene whose reference graph is CYCLIC does not load. "
			"DeriveToJob binds in DECLARATION ORDER, so the cycle's forward reference does not "
			"resolve and the derive REFUSES; it does not hang, and it does not silently accept a "
			"half-bound painter graph.  Every commit-layer cycle check is therefore DEFENSE IN "
			"DEPTH over this, not the cover for a hole." );
		std::remove( path.c_str() );
	}

	// ---- 3b. propose_patch closing a cycle is refused -----------------
	{
		const std::string path = TempPath( "test_s20_p3b.RISEscene" );
		Job* j = LoadScene( kChainScene, path );
		Check( j != nullptr, "3b: acyclic fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			// cyc_b already names cyc_a; naming cyc_b from cyc_a closes the loop.
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "cyc_a" ), String( "painter" ), String( "colora" ), String( "cyc_b" ), nullptr );
			Check( !r.applied,
				"3b: the AGENT surface (propose_patch) cannot close a cycle -- "
				"ApplyCstParamEditChecked's full-derivability gate refuses it" );
			Check( r.rawCode == 0 && std::string( r.status.c_str() ) == "rejected",
				"3b: ...as a plain code-0 rejection" );
			Check( DocText( *j ) == before, "3b: ...leaving the document BYTE-IDENTICAL" );
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 3c. a SELF-reference is refused too --------------------------
	{
		const std::string path = TempPath( "test_s20_p3c.RISEscene" );
		Job* j = LoadScene( kChainScene, path );
		Check( j != nullptr, "3c: acyclic fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			const SceneEditController::AgentCommitResult r = c.ApplyAgentParamEdit(
				String( "cyc_a" ), String( "painter" ), String( "colora" ), String( "cyc_a" ), nullptr );
			Check( !r.applied && DocText( *j ) == before,
				"3c: a chunk cannot name ITSELF either -- its own name is not registered until it "
				"finalizes, so the same declaration-order rule refuses it, byte-identically" );
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 3d. the GUI property-panel path refuses forward refs too ------
	//
	// The GUI material-slot REBIND arm (SetPropertyInner_'s Category::Material
	// case) is the ONE reference-RETARGETING caller that reaches the UNCHECKED
	// Job::ApplyCstParamEdit -- whose own comment says its callers "do not
	// RETARGET references".  It is nonetheless safe, because
	// WouldPersistDanglingReference_ runs FIRST and its target must be
	// "declared BEFORE the referencing chunk" (its own comment, verbatim --
	// SceneEditController.cpp ~11859).  Pinned here so that gate cannot be
	// removed as redundant.
	//
	// WHAT THIS CASE ACTUALLY CHECKS (corrected -- S20 review round 1 P3: an
	// earlier draft of this comment read as if the REFUSAL REASON were
	// verified here too).  `SetPropertyForCategory` returns a bare `bool` --
	// the rejection diagnostic is a `GlobalLog()->PrintEx` warning
	// (SetPropertyInner_'s "names a runtime-only entity with no CST chunk"
	// line), not a message this call surfaces, so this case can assert only
	// the OUTCOME (refused, document byte-identical), not the wording.  The
	// paired `okBack` assertion right below is what actually PINS
	// `WouldPersistDanglingReference_` as the mechanism rather than some
	// unrelated blanket refusal: an edit that is identical in every way
	// except direction (backward instead of forward) succeeds, which a
	// broader "Material rebinds always refuse" or "this panel is disabled"
	// explanation could not produce.
	{
		const std::string path = TempPath( "test_s20_p3d.RISEscene" );
		Job* j = LoadScene( kMatBeforePainterScene, path );
		Check( j != nullptr, "3d: material-before-painter fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			c.SetSelection( SceneEditController::Category::Material, String( "mat_early" ) );
			const bool ok = c.SetPropertyForCategory(
				SceneEditController::Category::Material,
				String( "reflectance" ), String( "pnt_late" ) );
			Check( !ok,
				"3d: the GUI panel cannot bind a material to a LATER-declared painter either" );
			Check( DocText( *j ) == before, "3d: ...document byte-identical" );

			// The same edit BACKWARD is legitimate and must still work, so this
			// case pins a REFUSAL, not a paralysed panel.
			c.SetSelection( SceneEditController::Category::Material, String( "mat_early" ) );
			const bool okBack = c.SetPropertyForCategory(
				SceneEditController::Category::Material,
				String( "reflectance" ), String( "pnt_early" ) );
			Check( okBack, "3d: ...while an EARLIER-declared painter still binds fine" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 3e. the canvas layer refuses EARLIER, and says why -----------
	{
		const std::string path = TempPath( "test_s20_p3e.RISEscene" );
		Job* j = LoadScene( kChainScene, path );
		Check( j != nullptr, "3e: acyclic fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			const SceneEditController::RewireResult r = c.RewireConnection(
				ChunkCategory::Painter, String( "cyc_a" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "cyc_b" ), nullptr );
			Check( !r.commit.applied, "3e: the CANVAS refuses the identical edit" );
			Check( r.cycleRefused,
				"3e: ...at the CYCLE gate (ConnectionLegality::WouldCycle), not at the derive" );
			Check( Contains( std::string( r.commit.message.c_str() ), "reference cycle" ),
				"3e: ...with a cycle-SPECIFIC diagnostic, which is exactly the value the "
				"commit-layer check adds over the derive's opaque `would not derive`" );
			Check( DocText( *j ) == before, "3e: ...document byte-identical" );

			// The derive is still the backstop underneath: a cycle-shaped edit
			// the canvas gate somehow let through would still be refused there.
			// (3b already proves that arm on the same fixture.)
			const RISE::Cst::Document* d = j->GetCstDocument();
			Check( ConnectionLegality::WouldCycle(
				*d,
				SceneReferenceGraph::ResolveChunk( *d, ChunkCategory::Painter, String( "cyc_a" ) ),
				SceneReferenceGraph::ResolveChunk( *d, ChunkCategory::Painter, String( "cyc_b" ) ) ),
				"3e: WouldCycle itself agrees, as a pure function of the document" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 3f. a freshly DUPLICATED node cannot smuggle one in ----------
	{
		const std::string path = TempPath( "test_s20_p3f.RISEscene" );
		Job* j = LoadScene( kChainScene, path );
		Check( j != nullptr, "3f: acyclic fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const Dup dup = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "cyc_b" ), nullptr );
			Check( dup.commit.applied, "3f: cyc_b forks" );
			const std::string afterFork = DocText( *j );

			// The copy names cyc_a.  Wiring cyc_a at the copy would close a
			// cycle through the brand-new node -- the S20 verb must not be a
			// back door around the S17 gate.
			const SceneEditController::RewireResult r = c.RewireConnection(
				ChunkCategory::Painter, String( "cyc_a" ), String( "colora" ), 0,
				ChunkCategory::Painter, dup.newName, nullptr );
			Check( !r.commit.applied && r.cycleRefused,
				"3f: wiring the ORIGINAL into its own fresh COPY is cycle-refused -- duplicating a "
				"node does not create a path around the cycle gate" );
			Check( DocText( *j ) == afterFork, "3f: ...document byte-identical" );

			j->release();
			std::remove( path.c_str() );
		}
	}
}

// =====================================================================
// PART 4 -- inherited commit discipline
// =====================================================================

static void Part4_CommitDiscipline()
{
	std::printf( "\nPART 4 -- undo/redo, epoch, mid-transaction, conflict\n" );

	// ---- 4a. UNDO/REDO byte-identity for a PLAIN delete ---------------
	{
		const std::string path = TempPath( "test_s20_p4a.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "4a: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			const Del r = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "pnt_orphan" ), Mode::TargetOnly, nullptr );
			Check( r.commit.applied, "4a: the delete applies" );
			const std::string afterEdit = DocText( *j );
			c.Undo();
			Check( DocText( *j ) == before, "4a: UNDO restores the ORIGINAL bytes exactly" );
			c.Redo();
			Check( DocText( *j ) == afterEdit, "4a: REDO restores the post-delete bytes exactly" );
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 4b. SCENE-EPOCH bumps ----------------------------------------
	//
	// The S15 lesson: the canvas re-pulls its snapshot on the epoch, so a
	// mutation that does not bump it is invisible to the very surface these
	// verbs exist for.
	{
		const std::string path = TempPath( "test_s20_p4b.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "4b: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );

			const unsigned int e0 = c.SceneEpoch();
			const Dup dup = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), nullptr );
			Check( dup.commit.applied, "4b: the fork applies" );
			Check( c.SceneEpoch() > e0, "4b: a DUPLICATE bumps the scene epoch" );

			const unsigned int e1 = c.SceneEpoch();
			const Del del = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "pnt_orphan" ), Mode::TargetOnly, nullptr );
			Check( del.commit.applied, "4b: the delete applies" );
			Check( c.SceneEpoch() > e1, "4b: a DELETE bumps it too" );

			const unsigned int e2 = c.SceneEpoch();
			Check( ReleaseTheChain( c ), "4b: setup for the cascade" );
			const unsigned int e3 = c.SceneEpoch();
			const Del cas = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), Mode::Cascade, nullptr );
			Check( cas.commit.applied, "4b: the cascade applies" );
			Check( c.SceneEpoch() > e3 && e3 > e2, "4b: and so does a CASCADE" );

			// A REFUSAL must not bump it -- nothing changed, so nothing should
			// force every canvas in the app to re-pull.
			const unsigned int e4 = c.SceneEpoch();
			const Del ref = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "no_such_painter" ), Mode::TargetOnly, nullptr );
			Check( !ref.commit.applied && c.SceneEpoch() == e4,
				"4b: a REFUSAL does NOT bump the epoch (nothing changed to re-pull)" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 4c. MID-TRANSACTION refusal ----------------------------------
	{
		const std::string path = TempPath( "test_s20_p4c.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "4c: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			Check( c.BeginTransaction(), "4c: a transaction opens" );

			const Del del = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "pnt_orphan" ), Mode::TargetOnly, nullptr );
			Check( !del.commit.applied, "4c: a DELETE is refused mid-transaction" );
			Check( del.commit.retriable, "4c: ...and the refusal is RETRIABLE (the txn will close)" );

			const Dup dup = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), nullptr );
			Check( !dup.commit.applied, "4c: a DUPLICATE is refused mid-transaction" );
			Check( dup.commit.retriable, "4c: ...and the refusal is RETRIABLE" );

			Check( DocText( *j ) == before, "4c: the document is BYTE-IDENTICAL after both refusals" );
			c.RollbackTransaction();
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 4d. HEAD-VERSION conflict -------------------------------------
	{
		const std::string path = TempPath( "test_s20_p4d.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "4d: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			RISE::Cst::CstHeadVersion stale = j->GetCstHeadVersion();
			stale.revision += 99;
			const std::string before = DocText( *j );

			const Del del = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "pnt_orphan" ), Mode::TargetOnly, &stale );
			Check( !del.commit.applied && del.commit.conflict,
				"4d: a stale baseVersion makes the DELETE a conflict" );
			Check( DocText( *j ) == before, "4d: ...non-mutating" );

			const Dup dup = c.DuplicateGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), &stale );
			Check( !dup.commit.applied && dup.commit.conflict,
				"4d: ...and the DUPLICATE too" );
			Check( DocText( *j ) == before, "4d: ...also non-mutating" );
			Check( dup.newName.size() <= 1, "4d: ...reporting no landed name" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 4e. the CASCADE's one-step undo, on a fresh fixture -----------
	//
	// PART 1c already asserts this; repeated here against the epoch/history
	// surface so a regression that made the batch push N records (and thus
	// need N Cmd-Zs) fails in the discipline section too, where a reader
	// looks for it.
	{
		const std::string path = TempPath( "test_s20_p4e.RISEscene" );
		Job* j = LoadScene( kScene, path );
		Check( j != nullptr, "4e: fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			Check( ReleaseTheChain( c ), "4e: setup" );
			const std::string before = DocText( *j );
			const Del r = c.DeleteGraphNode(
				ChunkCategory::Painter, String( "blend_chain" ), Mode::Cascade, nullptr );
			Check( r.commit.applied && r.removed.size() == 4, "4e: the four-chunk cascade applies" );
			c.Undo();   // ONE step -- the assertion below is what proves it was enough
			Check( DocText( *j ) == before,
				"4e: ...and that ONE step restored all four chunks -- the composite is one history "
				"record, not four" );
			j->release();
			std::remove( path.c_str() );
		}
	}
}

int main()
{
	std::printf( "DeleteDuplicateGraphNodeTest -- doc-88 Phase 3 S20\n" );
	std::printf( "  (reference-safe delete, positioned duplicate, cycle enforcement)\n" );

	Part1_ReferenceSafeDelete();
	Part2_PositionedDuplicate();
	Part3_Cycles();
	Part4_CommitDiscipline();

	std::printf( "\nDeleteDuplicateGraphNodeTest: %d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
