//////////////////////////////////////////////////////////////////////
//
//  RewireConnectionTest.cpp - doc-88 Phase 3 S19:
//    docs/gui/NODE_GRAPH_CANVAS.md sect. 6's "S19 -- Ownership-closure
//    rewrite + REFUSE path", which implements
//    docs/gui/MATERIAL_EDITOR.md sect. 3.7a.
//
//  WHY ITS OWN FILE rather than more of EntityTemplatesTest.cpp (the
//  home of S18's create-node sweep).  Two reasons, both about failure
//  attribution: (1) S19 is a different subsystem -- ownership policy
//  and refusal, not entity creation -- and every case here needs a
//  PURPOSE-BUILT fixture (a solely-owned graph beside an unrelated
//  neighbour; a painter shared by two materials; a legal same-name
//  colour/scalar pair; a chain that can be made cyclic) that the
//  entity-template suite's minimal base scene deliberately does not
//  have; (2) nearly every assertion here is a BYTE-IDENTITY assertion
//  over the whole serialized document, a discipline no other case in
//  that file uses.  Folding the two together would roughly double
//  EntityTemplatesTest and make "which subsystem broke?" a reading
//  exercise.
//
//  PARTS
//
//    PART 1 -- MATERIAL_EDITOR.md:367's THREE acceptance criteria,
//      verbatim, as literal tests:
//        1a. A topology edit whose closure is SOLELY OWNED rewrites
//            only that closure and leaves every other byte -- a shared
//            painter, a neighbouring unrelated chunk -- BYTE-IDENTICAL.
//        1b. A topology edit whose closure is AMBIGUOUS (a painter with
//            >= 2 OWNERS) returns a refusal diagnostic and does NOT
//            modify the document.  The diagnostic names (a) the shared
//            chunk, (b) the out-of-closure referrers, and (c) the
//            Duplicate-node escape hatch -- asserted by FORMATTING THE
//            SHARED kClosure* SYMBOLS, never a copied literal (the S17
//            lesson: two hand-kept "verbatim" copies had already
//            drifted, and the test only proved self-agreement).
//        1c. The Duplicate-node escape hatch FORKS and UNBLOCKS: once
//            the second owner is re-pointed at its own copy, the
//            previously-REFUSED edit succeeds unchanged.  Proven with
//            RewireConnection alone, so the claim under test is that the
//            refusal is OWNERSHIP-DRIVEN, not a blanket ban.
//        1c'. (S20, rewriting S19's standing gap marker as S19 said to)
//            the hatch END TO END through the shipped verbs: refusal ->
//            DuplicateGraphNode -> rewire the requesting consumer at the
//            copy -> APPLIED, all four states byte-verified, including
//            that the copy lands IMMEDIATELY AFTER the original.
//        1c''. The OUTLINER fork (DuplicateEntity) is unchanged by S20
//            and still appends -- pinned separately so the two verbs
//            cannot be conflated.
//        1d. Wiring a painter into an UNSPELLED reference param (the
//            property panel's "every material slot, including defaulted
//            ones" case) at occurrence 0 INSERTS the param line rather
//            than being refused as out-of-range; undo removes exactly
//            the inserted line, byte-identical.
//
//    PART 2 -- the other REFUSE paths, each also byte-identity-pinned:
//        2a. LEGALITY PARITY (S17): a colour painter dropped into a
//            Scalar-pipe slot is refused with the REAL PARSER's own
//            diagnostic -- asserted EQUAL to what
//            ConnectionLegality::CheckConnection returns for the same
//            inputs, so canvas and hand-edit cannot diverge.
//        2b. CYCLE: a wire whose commit would close a reference cycle is
//            refused -- and, because it is refused by the CYCLE gate and
//            not the closure gate, it also pins the documented gate
//            ORDER (legality/cycle FIRST, ownership second).
//        2c. AMBIGUOUS NAME (the S11 same-name colour/scalar pair), on
//            BOTH the target side and the candidate side.
//        2d. The tuple-embedded reference (voronoi_painter's `gen <x>
//            <y> <painter>`) is refused rather than silently mangled.
//        2e. An out-of-range / non-repeatable occurrence is refused with
//            the count, not silently no-op'd.
//
//    PART 3 -- the nowUnreferenced report (the canvas orphan badge):
//        3a. Rewiring away a chunk's SOLE reference reports it.
//        3b. Rewiring away ONE OF TWO references does NOT report it.
//        3c. It is a REPORT, not a delete: the orphan is still in the
//            document afterwards (S20 owns the delete policy).
//        3d. UNDO/REDO with a non-empty nowUnreferenced: the redone
//            Document actually leaves the predicted orphan orphaned, not
//            just byte-identical in the abstract.
//
//    PART 4 -- commit discipline inherited from ApplyAgentParamEditInner_,
//      asserted here because a composer that forgot to route through it
//      would silently lose each one:
//        4a. UNDO/REDO byte-identity (undo restores the ORIGINAL bytes;
//            redo restores the post-rewire bytes).
//        4b. MID-TRANSACTION refusal (retriable, byte-identical).
//        4c. SCENE-EPOCH bump (the S15 canvas-refresh lesson).
//        4d. HEAD-VERSION conflict (a stale baseVersion refuses without
//            mutating).
//
//    PART 5 -- OwnershipClosure as a PURE function of a Cst::Document
//      (no Job, no controller): the ownership rule itself.
//        5a. OwnersOf: solely-owned / shared / two-slots-one-owner /
//            orphan / root.
//        5b. Detach intent: classified and reported without any commit
//            path existing for it.
//        5c. ExpressionDrivenTarget on an expression-bodied chunk.
//        5d. CYCLE SAFETY on a hostile (cyclic) document -- the only
//            layer a test can hand one to, since ParseToCst is pure
//            syntax and never derives.
//        5f. NON-MATERIAL ROOT: a rasterizer's radiance_map owns a
//            painter exactly like a material does -- SharedTarget
//            generalizes past two-materials, and covers the unnamed-root
//            `<keyword>` DisplayName fallback as an actual reported owner.
//
//    PART 6 -- the OCCURRENCE-ADDRESSING scoping claim, pinned as a
//      registry sweep rather than left as prose: NO chunk kind
//      addressable through this verb declares a repeatable pure-
//      `ValueKind::Reference` parameter today.  Fails loudly the day one
//      appears, which is the day occurrence-addressed rewire becomes
//      reachable and needs a real end-to-end case.
//
//    PART 7 -- PERF PIN (S19 review round 1 P2-2): `OwnershipClosure::
//      Adjacency` / `BuildAdjacencyFor` exist so a caller batching
//      `OwnersOf` once PER NODE over a document builds the chunk index /
//      reverse-edge map ONCE, not once per node.  On a synthetic
//      ~1508-chunk fixture (no checked-in probe scene at exactly that
//      scale -- built in-test, same spirit as ReferenceGraphTest's
//      sombrero.RISEscene perf gate), an owners-for-every-node pass using
//      the prebuilt `Adjacency` must stay the SAME ORDER as ONE adjacency
//      build, asserted as a loose ratio budget rather than a tight
//      wall-clock number (the S11 perf-gate precedent).
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes, OIDN
//  off, no render pass is ever started.
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
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Parsers/ChunkParserRegistry.h"
#include "../src/Library/Parsers/IAsciiChunkParser.h"
#include "../src/Library/SceneEditor/ConnectionLegality.h"
#include "../src/Library/SceneEditor/OwnershipClosure.h"
#include "../src/Library/SceneEditor/ReferenceGraph.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

using namespace RISE;
using namespace RISE::Implementation;

// =====================================================================
// ORDINAL-PINNING static_asserts (S19 review round 1 P2-3).
//
// RISE_API.h's `RISE_API_SceneEditController_RewireConnection` sends
// `RISE::ClosureClassification` across the ABI as a plain `int`, documented
// as "0 Clean / 1 UnresolvedTarget / 2 AmbiguousTargetName /
// 3 ExpressionDrivenTarget / 4 SharedTarget".  BOTH platform bridges
// re-declare a real mirror enum matching those five ordinals verbatim --
// `RewireClosure` (build/VS2022/RISE-GUI/ViewportBridge.h) and
// `RISERewireClosure` (build/XCode/rise/RISE-GUI/Bridge/
// RISEViewportBridge.h) -- an earlier RISE_API.h comment claimed neither
// bridge re-declares it ("no mirror to drift"), which was simply wrong: both
// do, and a reordered/renumbered `ClosureClassification` would silently
// desync three hand-kept copies with no compiler error anywhere.  These
// static_asserts are the SINGLE place that would catch it, at COMPILE time,
// against the ACTUAL enum (not a copy of a copy).  If one of these ever
// fails, `RISE_API.h`'s doc comment AND both bridge mirror enums need the
// same renumbering.
static_assert( static_cast<int>( ClosureClassification::Clean ) == 0,
	"ABI ordinal pin: ClosureClassification::Clean must stay 0 -- RISE_API.h's documented ABI int, "
	"and both bridge mirrors (RewireClosure::Clean / RISERewireClosureClean), depend on this exact value" );
static_assert( static_cast<int>( ClosureClassification::UnresolvedTarget ) == 1,
	"ABI ordinal pin: ClosureClassification::UnresolvedTarget must stay 1 -- see Clean's static_assert above" );
static_assert( static_cast<int>( ClosureClassification::AmbiguousTargetName ) == 2,
	"ABI ordinal pin: ClosureClassification::AmbiguousTargetName must stay 2 -- see Clean's static_assert above" );
static_assert( static_cast<int>( ClosureClassification::ExpressionDrivenTarget ) == 3,
	"ABI ordinal pin: ClosureClassification::ExpressionDrivenTarget must stay 3 -- see Clean's static_assert above" );
static_assert( static_cast<int>( ClosureClassification::SharedTarget ) == 4,
	"ABI ordinal pin: ClosureClassification::SharedTarget must stay 4 -- see Clean's static_assert above" );

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

	//! The whole retained document, byte-exact.  THE instrument every
	//! refusal case in this file uses: "the document is byte-identical"
	//! is asserted by comparing this before and after, not by spot-
	//! checking the chunk we happened to think about.
	std::string DocText( const Job& j )
	{
		const RISE::Cst::Document* d = j.GetCstDocument();
		return d ? RISE::Cst::SerializeCst( *d ) : std::string();
	}

	bool Contains( const std::string& hay, const std::string& needle )
	{
		return hay.find( needle ) != std::string::npos;
	}

	//! The scene every PART 1-4 case runs against.  Laid out so the
	//! sect. 3.7a distinctions are all present at once:
	//!
	//!   SOLELY OWNED   mat_one -> blend_owned -> {pnt_red, pnt_green}
	//!                  (blend_owned has exactly ONE owner, mat_one)
	//!   SHARED         mat_two -> blend_shared <- mat_three
	//!                  (blend_shared has TWO owners; the sect. 3.7a
	//!                   REFUSE case, and the Duplicate-node hatch's
	//!                   subject)
	//!   TWO SLOTS,     mat_four's ggx alphax/alphay BOTH name
	//!   ONE OWNER      scal_rough -- two referrer EDGES, ONE owner, so
	//!                  it must NOT read as shared (the "OVER-strict"
	//!                  trap OwnershipClosure.h names).  scal_smooth is a
	//!                  second, otherwise-unused scalar painter PART 3b'
	//!                  rewires alphay TO, so the nowUnreferenced
	//!                  remainingEdges loop actually runs (rewiring a
	//!                  slot back onto the SAME chunk it already names
	//!                  short-circuits on `stillBound` before that loop).
	//!   SOLE REF       pnt_red is referenced only from blend_owned
	//!                  (PART 3a's orphan case)
	//!   TWO REFS       pnt_blue is referenced from blend_shared.colorb
	//!                  AND mat_five.reflectance (PART 3b)
	//!   NEIGHBOUR      lgt_witness / geo_witness are unrelated chunks
	//!                  whose bytes must never move (the "leaves every
	//!                  other byte byte-identical" half of :367)
	const char* const kGraphScene =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n"
		"film\n{\nwidth 16\nheight 16\n}\n\n"
		"pinhole_camera\n{\nlocation 0 0 3.5\nlookat 0 0 0\nup 0 1 0\nfov 40.0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_red\ncolor 1 0 0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_green\ncolor 0 1 0\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_blue\ncolor 0 0 1\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_white\ncolor 1 1 1\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_mask_a\ncolor 0.5 0.5 0.5\n}\n\n"
		"uniformcolor_painter\n{\nname pnt_mask_b\ncolor 0.5 0.5 0.5\n}\n\n"
		"scalar_painter\n{\nname scal_rough\nvalue 0.3\n}\n\n"
		"scalar_painter\n{\nname scal_smooth\nvalue 0.05\n}\n\n"
		"blend_painter\n{\nname blend_owned\ncolora pnt_red\ncolorb pnt_green\nmask pnt_mask_a\n}\n\n"
		"blend_painter\n{\nname blend_shared\ncolora pnt_green\ncolorb pnt_blue\nmask pnt_mask_b\n}\n\n"
		"lambertian_material\n{\nname mat_one\nreflectance blend_owned\n}\n\n"
		"lambertian_material\n{\nname mat_two\nreflectance blend_shared\n}\n\n"
		"lambertian_material\n{\nname mat_three\nreflectance blend_shared\n}\n\n"
		"ggx_material\n{\nname mat_four\nrd pnt_white\nalphax scal_rough\nalphay scal_rough\n}\n\n"
		"lambertian_material\n{\nname mat_five\nreflectance pnt_blue\n}\n\n"
		"sphere_geometry\n{\nname geo_witness\nradius 0.8\n}\n\n"
		"standard_object\n{\nname obj_one\ngeometry geo_witness\nmaterial mat_one\n}\n\n"
		"omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}\n";

	typedef SceneEditController::RewireResult Rewire;

	//! The (a)/(b)/(c) shared-refusal text, built from the SAME
	//! `inline constexpr` symbols OwnershipClosure.cpp formats from --
	//! so this test cannot pass by agreeing with a stale copy.
	std::string ExpectedSharedDiagnostic( const std::string& sharedName,
	                                       const std::string& owners,
	                                       const std::string& referrers )
	{
		char buf[1024];
		std::snprintf( buf, sizeof( buf ), kClosureSharedFmt,
			sharedName.c_str(), owners.c_str(), referrers.c_str() );
		return std::string( buf ) + kClosureDuplicateHatch;
	}
}

// =====================================================================
// PART 1 -- MATERIAL_EDITOR.md:367's three acceptance criteria
// =====================================================================

static void Part1_AcceptanceCriteria()
{
	std::printf( "\nPART 1 -- MATERIAL_EDITOR.md:367 acceptance criteria\n" );

	// ---- 1a. SOLELY OWNED closure rewrites cleanly ------------------
	{
		const std::string path = TempPath( "test_rewire_p1a.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "1a: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			// blend_owned is reachable only from mat_one -> ONE owner.
			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );

			Check( r.commit.applied, "1a: a solely-owned rewire APPLIES" );
			Check( r.closure == ClosureClassification::Clean, "1a: classification is Clean" );
			Check( r.owners.size() == 1, "1a: exactly ONE owner" );
			if( r.owners.size() == 1 )
				Check( std::string( r.owners[0].c_str() ) == "mat_one",
					"1a: the owner is mat_one (the material whose graph this is)" );

			const std::string after = DocText( *j );

			// THE :367 assertion.  Not "the chunk we edited looks right"
			// but "the ONLY byte that moved is the one token": rebuild
			// `before` with that single substitution and demand equality
			// with `after`.  A stray re-emit anywhere -- the shared
			// painter, the witness light, whitespace, the trailing
			// newline -- fails here.
			std::string expected = before;
			const std::string fromLine = "name blend_owned\ncolora pnt_red\n";
			const std::string toLine   = "name blend_owned\ncolora pnt_white\n";
			const size_t at = expected.find( fromLine );
			Check( at != std::string::npos, "1a: the pre-edit blend_owned body is where the test expects it" );
			if( at != std::string::npos ) expected.replace( at, fromLine.size(), toLine );
			Check( after == expected,
				"1a: EVERY other byte is identical -- only the one reference token changed" );

			// ...and the explicit "the shared painter / the unrelated
			// neighbour did not move" spot checks :367 words out loud.
			Check( Contains( after, "name blend_shared\ncolora pnt_green\ncolorb pnt_blue\nmask pnt_mask_b\n" ),
				"1a: the SHARED painter's bytes are untouched" );
			Check( Contains( after, "omni_light\n{\nname lgt_witness\npower 3.0\ncolor 1 1 1\nposition 0 3 0\n}" ),
				"1a: a neighbouring unrelated chunk's bytes are untouched" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1b. AMBIGUOUS (shared) closure REFUSES, modifies nothing ---
	{
		const std::string path = TempPath( "test_rewire_p1b.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "1b: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			// blend_shared is reachable from mat_two AND mat_three.
			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_shared" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );

			Check( !r.commit.applied, "1b: a shared-closure rewire is REFUSED" );
			Check( std::string( r.commit.status.c_str() ) == "rejected", "1b: status is `rejected`" );
			Check( r.closure == ClosureClassification::SharedTarget, "1b: classification is SharedTarget" );
			Check( !r.commit.retriable, "1b: the refusal is PERMANENT (retrying verbatim can never work)" );

			// THE byte-identity half of the criterion.
			Check( DocText( *j ) == before, "1b: the document is BYTE-IDENTICAL after the refusal" );

			// (a) the shared chunk.
			Check( r.sharedChunks.size() == 1
			    && std::string( r.sharedChunks[0].c_str() ) == "blend_shared",
				"1b: (a) the diagnostic names the SHARED chunk" );
			// (b) the out-of-closure referrers.
			Check( r.outOfClosureReferrers.size() == 2, "1b: (b) BOTH out-of-closure referrers are named" );
			// the owning roots.
			Check( r.owners.size() == 2, "1b: two owning roots" );

			// (c) the Duplicate-node escape hatch, plus (a)+(b) in the
			// prose -- asserted against the SHARED format symbols.
			std::string owners, referrers;
			for( size_t i = 0; i < r.owners.size(); ++i ) {
				if( i ) owners += ( i + 1 == r.owners.size() ) ? " and " : ", ";
				owners += r.owners[i].c_str();
			}
			for( size_t i = 0; i < r.outOfClosureReferrers.size(); ++i ) {
				if( i ) referrers += ( i + 1 == r.outOfClosureReferrers.size() ) ? " and " : ", ";
				referrers += r.outOfClosureReferrers[i].c_str();
			}
			const std::string expectedDiag =
				ExpectedSharedDiagnostic( "blend_shared", owners, referrers );
			Check( Contains( std::string( r.commit.message.c_str() ), expectedDiag ),
				"1b: the refusal message IS the shared-format diagnostic, (a)+(b)+(c) included" );
			Check( Contains( std::string( r.commit.message.c_str() ), kClosureDuplicateHatch ),
				"1b: (c) the message names the Duplicate-node escape hatch" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1c. Duplicate-node FORKS and UNBLOCKS ----------------------
	//
	// S20 ships the one-click Duplicate NODE action.  What it must
	// DELIVER is exactly this: fork the shared painter into an owned
	// copy, re-point the second owner at the copy, and the
	// previously-refused edit then succeeds unchanged.  Proven here with
	// shipped machinery only -- `DuplicateEntity` (which already forks a
	// painter chunk, deduped name and all) plus `RewireConnection` -- so
	// S20 inherits a passing test rather than a promise, and a regression
	// that made the refusal UNCONDITIONAL rather than ownership-driven
	// fails HERE even before S20 exists.
	//
	// The load-bearing claim is that the refusal is OWNERSHIP-DRIVEN, not
	// a blanket ban: drop the second owner's dependency on the shared
	// node and the identical edit lands.  That is asserted first, with
	// nothing but RewireConnection.
	//
	// THE FINDING THIS CASE SURFACED, AND ITS RESOLUTION (S20):
	// `DuplicateEntity` -- the shipped OUTLINER fork -- places its copy by
	// `Job::ApplyCstInsertChunk`'s TIER heuristic, which in a scene whose
	// first chunk is a `standard_shader` (tier 1) targets index 0, ahead of
	// the painters the copy itself references; the positioned dry-run then
	// fails and the insert FALLS BACK to append-at-end.  An EARLIER-declared
	// material could not be re-pointed at that copy -- the full-derivability
	// gate correctly refused (ENTITY_CREATION.md sect. 7.3's declaration-order
	// rule) -- so the shipped fork could not, by itself, complete sect. 3.7a's
	// escape hatch.  S19 pinned that as a standing marker for S20.
	//
	// S20 ships `SceneEditController::DuplicateGraphNode`, which places the
	// copy IMMEDIATELY AFTER the original, and case 1c'' below is that marker
	// REWRITTEN -- as S19 said it should be the day the gap closed -- into the
	// END-TO-END hatch: shared-refusal -> Duplicate -> rewire the requesting
	// consumer at the copy -> APPLIED, with the old append-position behaviour
	// still pinned on `DuplicateEntity` itself so the two verbs cannot be
	// silently conflated.
	{
		const std::string path = TempPath( "test_rewire_p1c.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "1c: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );

			// Baseline: refused while the painter has two owners.
			const Rewire pre = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_shared" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( !pre.commit.applied && pre.closure == ClosureClassification::SharedTarget,
				"1c: baseline -- the edit is refused while the painter is shared" );

			// UNBLOCK: the second owner stops depending on the shared node
			// (which is exactly what forking accomplishes; see the finding
			// below for why the shipped fork cannot supply the copy yet).
			const Rewire rp = c.RewireConnection(
				ChunkCategory::Material, String( "mat_three" ), String( "reflectance" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( rp.commit.applied, "1c: re-pointing the second owner elsewhere applies (it is a root)" );

			// The SAME edit, verbatim, now succeeds.
			const Rewire post = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_shared" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( post.commit.applied,
				"1c: once the second owner is gone, the previously-REFUSED edit APPLIES unchanged" );
			Check( post.closure == ClosureClassification::Clean, "1c: ...and its closure is now Clean" );
			if( post.owners.size() == 1 )
				Check( std::string( post.owners[0].c_str() ) == "mat_two",
					"1c: ...with mat_two as the sole remaining owner" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1c'. THE ESCAPE HATCH, END TO END (S20) --------------------
	//
	// sect. 3.7a's acceptance criterion in full, through the SHIPPED verbs and
	// nothing else: the shared-closure rewire is REFUSED -> Duplicate the
	// shared node -> re-point the REQUESTING consumer at the copy -> the
	// previously-refused edit APPLIES.  All four states byte-verified.
	//
	// This case is the S19 standing marker, rewritten.  It also keeps the
	// marker's own subject alive: `DuplicateEntity` (the OUTLINER's fork)
	// still appends, and still cannot complete the hatch -- asserted below
	// against a SECOND fixture, so "the canvas verb positions correctly" and
	// "the outliner verb still does not" can never be conflated into one
	// passing assertion.
	{
		const std::string path = TempPath( "test_rewire_p1cprime.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "1c': graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );

			// STATE 1 -- the refusal.  mat_three wants blend_shared changed,
			// but mat_two owns it too.
			const std::string s0 = DocText( *j );
			const Rewire pre = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_shared" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( !pre.commit.applied && pre.closure == ClosureClassification::SharedTarget,
				"1c': STATE 1 -- the shared-closure rewire is REFUSED" );
			Check( DocText( *j ) == s0, "1c': STATE 1 -- and the document is BYTE-IDENTICAL" );
			Check( Contains( std::string( pre.commit.message.c_str() ), kClosureDuplicateHatch ),
				"1c': STATE 1 -- the refusal names the Duplicate-node hatch this case now walks" );

			// STATE 2 -- Duplicate.  The copy must land IMMEDIATELY AFTER the
			// original, which is the whole S20 fix.
			const SceneEditController::DuplicateResult dup =
				c.DuplicateGraphNode( ChunkCategory::Painter, String( "blend_shared" ), nullptr );
			Check( dup.commit.applied, "1c': STATE 2 -- DuplicateGraphNode forks the shared painter" );
			Check( dup.newName.size() > 1 && std::string( dup.newName.c_str() ) != "blend_shared",
				"1c': STATE 2 -- under a genuinely new, deduped name" );
			const std::string s1 = DocText( *j );
			{
				// POSITION, asserted on the BYTES: the copy's `name` line must
				// appear after the original's and before the first consumer's.
				const std::string origLine = "name blend_shared\n";
				const std::string copyLine = "name " + std::string( dup.newName.c_str() ) + "\n";
				const size_t origAt = s1.find( origLine );
				const size_t copyAt = s1.find( copyLine );
				const size_t consumerAt = s1.find( "name mat_two\n" );
				Check( origAt != std::string::npos && copyAt != std::string::npos
				    && consumerAt != std::string::npos && origAt < copyAt && copyAt < consumerAt,
					"1c': STATE 2 -- the copy sits AFTER the original and BEFORE its consumers "
					"(the S19 handoff gap, closed)" );
				// SHALLOW: the copy shares the original's referents verbatim.
				Check( Contains( s1, "name " + std::string( dup.newName.c_str() )
				                    + "\ncolora pnt_green\ncolorb pnt_blue\nmask pnt_mask_b\n" ),
					"1c': STATE 2 -- the fork is SHALLOW: it shares every chunk the original referenced" );
				// And the ORIGINAL's own bytes did not move.
				Check( Contains( s1, "name blend_shared\ncolora pnt_green\ncolorb pnt_blue\nmask pnt_mask_b\n" ),
					"1c': STATE 2 -- the original's bytes are untouched" );
			}
			{
				const RISE::Cst::NodeId forkId = SceneReferenceGraph::ResolveChunk(
					*j->GetCstDocument(), ChunkCategory::Painter, dup.newName );
				Check( forkId != 0, "1c': STATE 2 -- the fork NAME resolves to a real Painter chunk" );
			}

			// STATE 3 -- re-point the REQUESTING consumer at the copy.  This is
			// the exact edit the S19 marker pinned as IMPOSSIBLE with the
			// appended fork; it must now APPLY.
			const Rewire rp = c.RewireConnection(
				ChunkCategory::Material, String( "mat_three" ), String( "reflectance" ), 0,
				ChunkCategory::Painter, dup.newName, nullptr );
			Check( rp.commit.applied,
				"1c': STATE 3 -- the requesting consumer re-points at the positioned copy "
				"(the declaration-order gate no longer refuses)" );
			Check( rp.closure == ClosureClassification::Clean, "1c': STATE 3 -- ...cleanly" );
			const std::string s2 = DocText( *j );
			Check( Contains( s2, "name mat_three\nreflectance " + std::string( dup.newName.c_str() ) + "\n" ),
				"1c': STATE 3 -- and the consumer's bytes now name the copy" );

			// STATE 4 -- the originally-REFUSED edit, verbatim, now APPLIES.
			const Rewire post = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_shared" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( post.commit.applied,
				"1c': STATE 4 -- the previously-REFUSED edit APPLIES unchanged (sect. 3.7a's hatch, closed)" );
			Check( post.closure == ClosureClassification::Clean, "1c': STATE 4 -- its closure is now Clean" );
			if( post.owners.size() == 1 )
				Check( std::string( post.owners[0].c_str() ) == "mat_two",
					"1c': STATE 4 -- with mat_two as the sole remaining owner" );
			Check( Contains( DocText( *j ), "name blend_shared\ncolora pnt_white\n" ),
				"1c': STATE 4 -- and the edit landed on the ORIGINAL, not the copy" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1c''. THE OUTLINER FORK STILL APPENDS (the S19 marker's subject) ----
	//
	// `DuplicateEntity` is deliberately UNCHANGED by S20 (it is the outliner's
	// duplicate for EVERY category, UI-Category-addressed, with a different
	// blast radius -- see DeleteGraphNode's placement note).  Its copy still
	// lands by the tier heuristic's append-at-end fallback in this fixture, so
	// an earlier-declared referrer still cannot point at it.  Pinned so a
	// future reader cannot mistake 1c' above for "duplicate was fixed
	// everywhere", and so a change to EITHER verb's positioning is visible.
	{
		const std::string path = TempPath( "test_rewire_p1cprime2.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "1c'': graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );

			String forkName;
			const SceneEditController::AgentCommitResult cr =
				c.DuplicateEntity( SceneEditController::Category::Painter,
				                   String( "blend_shared" ), &forkName );
			Check( cr.applied, "1c'': DuplicateEntity forks the shared painter under a deduped name" );

			const Rewire rp = c.RewireConnection(
				ChunkCategory::Material, String( "mat_three" ), String( "reflectance" ), 0,
				ChunkCategory::Painter, forkName, nullptr );
			Check( !rp.commit.applied,
				"1c'': the OUTLINER fork is still APPENDED, so an earlier-declared referrer cannot point "
				"at it -- use DuplicateGraphNode (case 1c') for the canvas hatch" );
			Check( rp.closure == ClosureClassification::Clean,
				"1c'': ...and the refusal is NOT an ownership one -- the closure was clean; it is the "
				"derive-order gate inside the commit" );
			Check( rp.commit.rawCode == 0,
				"1c'': ...specifically rawCode 0 (ApplyCstParamEditChecked's 'would not derive' code)" );
			Check( Contains( std::string( rp.commit.message.c_str() ), "would not derive" )
			    && std::string( rp.commit.status.c_str() ) == "rejected",
				"1c'': ...and the message is the ACTUAL declaration-order derive-refusal text "
				"(ApplyAgentParamEditInner_'s code-0 branch)" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 1d. UNSPELLED PARAM (occurrence 0) INSERTS the line -- P1 ------
	//
	// S19 review round 1 P1: `occurrence >= count` used to refuse
	// occurrence 0 on a reference param the chunk's TEXT does not spell
	// (count == 0) -- but `Cst::DocSetOrAddParamValue`'s INSERT arm exists
	// precisely for this case (the property panel surfaces every
	// DEFAULTED material slot, and editing one must take effect + persist
	// by inserting a new `role value` line).  The fix narrows the refusal
	// to `occurrence > 0 && occurrence >= count`, so occurrence 0 on an
	// unspelled slot now reaches the insert path instead of being refused
	// as "0 occurrence(s), so index 0 is out of range".
	{
		// 1d-i. ggx_material.emissive (unspelled on mat_four).
		const std::string path = TempPath( "test_rewire_p1d_i.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "1d-i: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			Check( !Contains( before, "emissive" ), "1d-i: mat_four does not spell `emissive` to start with" );

			const Rewire r = c.RewireConnection(
				ChunkCategory::Material, String( "mat_four" ), String( "emissive" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( r.commit.applied,
				"1d-i: wiring a painter into an UNSPELLED reference param at occurrence 0 APPLIES "
				"(not refused as out-of-range on a chunk that spells the param zero times)" );

			const std::string after = DocText( *j );
			Check( Contains( after, "emissive pnt_white" ),
				"1d-i: ...and the param line is genuinely INSERTED, not silently dropped" );
			Check( after != before, "1d-i: ...the document really changed" );

			c.Undo();
			Check( DocText( *j ) == before,
				"1d-i: UNDO restores the document BYTE-IDENTICAL -- the U1 absent-param inverse (a "
				"REMOVAL) correctly reverses a line a REWIRE inserted, not just one an ordinary "
				"value edit replaced" );

			j->release();
			std::remove( path.c_str() );
		}
	}
	{
		// 1d-ii. standard_object.radiance_map (unspelled on obj_one) -- a
		// DIFFERENT category + param than 1d-i's ggx_material.emissive, so
		// this is not just the same code path with different names -- it
		// pins the fix across a genuinely different chunk shape.
		//
		// (scalar_painter.function2d, tried first, turned out to be the
		// WRONG example: scalar_painter enforces "exactly one form" at
		// DERIVE time -- scal_rough already spells `value`, so adding
		// `function2d` fails with "multiple forms specified (mutually
		// exclusive)", a real, unrelated descriptor constraint, not a P1
		// regression.  Every valid scalar_painter always has exactly one
		// form spelled from the start, so there is no fixture where
		// `function2d` is both unspelled AND safe to insert -- that
		// param is structurally unreachable for this test, not a gap in
		// the fix.  `standard_object.radiance_map` is a genuinely
		// ADDITIVE optional slot with no such constraint.)
		const std::string path = TempPath( "test_rewire_p1d_ii.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "1d-ii: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );
			Check( !Contains( before, "radiance_map" ),
				"1d-ii: obj_one does not spell `radiance_map` to start with" );

			const Rewire r = c.RewireConnection(
				ChunkCategory::Object, String( "obj_one" ), String( "radiance_map" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( r.commit.applied,
				"1d-ii: same fix, a DIFFERENT category + param (Object.radiance_map rather than "
				"Material.emissive)" );

			const std::string after = DocText( *j );
			Check( Contains( after, "radiance_map pnt_white" ), "1d-ii: ...param line inserted" );

			c.Undo();
			Check( DocText( *j ) == before, "1d-ii: UNDO restores the document BYTE-IDENTICAL here too" );

			j->release();
			std::remove( path.c_str() );
		}
	}
}

// =====================================================================
// PART 2 -- the other REFUSE paths
// =====================================================================

static void Part2_OtherRefusals()
{
	std::printf( "\nPART 2 -- legality / cycle / ambiguity / tuple / occurrence refusals\n" );

	// ---- 2a. LEGALITY PARITY with the real parser -------------------
	{
		const std::string path = TempPath( "test_rewire_p2a.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "2a: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			// ggx_material.alphax is the SCALAR pipe; pnt_white is a
			// colour painter.  The parser's own three-way
			// ResolveOrDiagnoseScalar diagnostic is what must come back.
			const Rewire r = c.RewireConnection(
				ChunkCategory::Material, String( "mat_four" ), String( "alphax" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );

			Check( !r.commit.applied, "2a: a colour painter into a Scalar-pipe slot is REFUSED" );
			Check( r.legalityRefused, "2a: ...flagged as a LEGALITY refusal, not a closure one" );
			Check( r.closure == ClosureClassification::Clean,
				"2a: ...and the closure step was never reached (gate order: legality FIRST)" );
			Check( DocText( *j ) == before, "2a: the document is BYTE-IDENTICAL after the refusal" );

			// PARITY, by construction: the message is what
			// ConnectionLegality itself returns for the same inputs.
			const ConnectionVerdict v = c.CheckConnection(
				ChunkCategory::Material, String( "mat_four" ), String( "alphax" ),
				ChunkCategory::Painter, String( "pnt_white" ) );
			Check( !v.legal, "2a: the S17 validator agrees the drop is illegal" );
			Check( Contains( std::string( r.commit.message.c_str() ), v.diagnostic ),
				"2a: the rewire refusal carries the S17/parser diagnostic VERBATIM" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2b. CYCLE ---------------------------------------------------
	//
	// mat_one -> blend_owned -> pnt_red.  Rewiring pnt_red's slot is not
	// possible (a uniformcolor_painter has no reference param), so build
	// the cycle the other way: rewire blend_owned.colora at a painter
	// that already reaches blend_owned.  `blend_outer` does.
	{
		std::string text( kGraphScene );
		const std::string anchor = "lambertian_material\n{\nname mat_one\nreflectance blend_owned\n}\n\n";
		const size_t at = text.find( anchor );
		Check( at != std::string::npos, "2b: fixture anchor found" );
		if( at != std::string::npos ) {
			text.insert( at,
				"blend_painter\n{\nname blend_outer\ncolora blend_owned\ncolorb pnt_white\nmask pnt_mask_a\n}\n\n"
				"lambertian_material\n{\nname mat_outer\nreflectance blend_outer\n}\n\n" );
		}
		const std::string path = TempPath( "test_rewire_p2b.RISEscene" );
		Job* j = LoadScene( text, path );
		Check( j != nullptr, "2b: cycle fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "blend_outer" ), nullptr );

			Check( !r.commit.applied, "2b: a wire that would close a cycle is REFUSED" );
			Check( r.cycleRefused, "2b: ...flagged as a CYCLE refusal" );
			Check( !r.legalityRefused, "2b: ...not a legality one (the binding itself is type-legal)" );
			Check( DocText( *j ) == before, "2b: the document is BYTE-IDENTICAL after the refusal" );
			// Gate order: blend_owned's closure is CLEAN (one owner,
			// mat_one) -- so the only thing that could have refused this
			// is the cycle check running BEFORE ownership.
			Check( r.closure == ClosureClassification::Clean,
				"2b: gate ORDER pinned -- the closure was clean; the cycle gate refused first" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2c. AMBIGUOUS NAME (the S11 same-name colour/scalar pair) ---
	{
		std::string text( kGraphScene );
		// A legal same-name-same-category pair: a colour painter and a
		// scalar_painter both named `dupe`.  The scalar one carries a
		// pure-Reference param (`base`), so it can be a rewire TARGET.
		const std::string anchor = "blend_painter\n{\nname blend_owned\n";
		const size_t at = text.find( anchor );
		Check( at != std::string::npos, "2c: fixture anchor found" );
		if( at != std::string::npos ) {
			text.insert( at,
				"uniformcolor_painter\n{\nname dupe\ncolor 0.2 0.2 0.2\n}\n\n"
				"scalar_painter\n{\nname dupe\nbase scal_rough\nscale 1.0\n}\n\n" );
		}
		const std::string path = TempPath( "test_rewire_p2c.RISEscene" );
		Job* j = LoadScene( text, path );
		Check( j != nullptr, "2c: same-name-pair fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );

			// Sanity: the pair really IS ambiguous at the S11 layer.
			int occs = 0;
			SceneReferenceGraph::ResolveChunk( *j->GetCstDocument(), ChunkCategory::Painter,
			                                   String( "dupe" ), &occs );
			Check( occs == 2, "2c: `dupe` genuinely resolves to TWO Painter-category chunks" );

			const std::string before = DocText( *j );

			// TARGET side.
			const Rewire rt = c.RewireConnection(
				ChunkCategory::Painter, String( "dupe" ), String( "base" ), 0,
				ChunkCategory::Painter, String( "scal_rough" ), nullptr );
			Check( !rt.commit.applied, "2c: an AMBIGUOUS target name is REFUSED, never guessed" );
			Check( rt.closure == ClosureClassification::AmbiguousTargetName,
				"2c: ...classified AmbiguousTargetName" );
			char expect[512];
			std::snprintf( expect, sizeof( expect ), kClosureAmbiguousFmt, "dupe", 2 );
			Check( Contains( std::string( rt.commit.message.c_str() ), expect ),
				"2c: ...with the shared ambiguity diagnostic naming the match count" );
			Check( DocText( *j ) == before, "2c: target-side refusal leaves the document BYTE-IDENTICAL" );

			// CANDIDATE side.
			const Rewire rc = c.RewireConnection(
				ChunkCategory::Material, String( "mat_one" ), String( "reflectance" ), 0,
				ChunkCategory::Painter, String( "dupe" ), nullptr );
			Check( !rc.commit.applied, "2c: an AMBIGUOUS candidate name is REFUSED too" );
			Check( Contains( std::string( rc.commit.message.c_str() ), expect ),
				"2c: ...with the same shared ambiguity diagnostic" );
			Check( rc.closure == ClosureClassification::AmbiguousTargetName,
				"2c: P3 -- the CANDIDATE-side refusal also reports a non-Clean closure (it used to leave "
				"the struct's default `Clean` on this path, a refused commit misreporting Clean)" );
			Check( DocText( *j ) == before, "2c: candidate-side refusal leaves the document BYTE-IDENTICAL" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2d. TUPLE-EMBEDDED reference -------------------------------
	//
	// voronoi_painter's `gen <x> <y> <painter>` is repeatable AND carries
	// a Reference token -- so ConnectionLegality accepts a candidate for
	// it (binding one IS legal), but rewriting it would mean re-emitting
	// the whole line, coordinates included.  Refused, not mangled.
	{
		std::string text( kGraphScene );
		const std::string anchor = "lambertian_material\n{\nname mat_one\n";
		const size_t at = text.find( anchor );
		if( at != std::string::npos ) {
			text.insert( at,
				"voronoi2d_painter\n{\nname vor\ngen 0.25 0.25 pnt_red\ngen 0.75 0.75 pnt_green\n"
				"border pnt_blue\n}\n\n"
				"lambertian_material\n{\nname mat_vor\nreflectance vor\n}\n\n" );
		}
		const std::string path = TempPath( "test_rewire_p2d.RISEscene" );
		Job* j = LoadScene( text, path );
		Check( j != nullptr, "2d: voronoi fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "vor" ), String( "gen" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( !r.commit.applied, "2d: a tuple-embedded reference param is REFUSED for rewire" );
			Check( Contains( std::string( r.commit.message.c_str() ), "multi-token value" ),
				"2d: ...with a message that explains why (the whole line would have to be re-emitted)" );
			Check( DocText( *j ) == before, "2d: the document is BYTE-IDENTICAL -- the tuple was not mangled" );

			// The chunk's SINGLE-token reference param on the same chunk
			// (`border`) rewires normally -- so the refusal is about the
			// tuple, not about voronoi_painter.
			const Rewire ok = c.RewireConnection(
				ChunkCategory::Painter, String( "vor" ), String( "border" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( ok.commit.applied, "2d: the same chunk's SINGLE-token reference param rewires fine" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 2e. OCCURRENCE out of range --------------------------------
	{
		const std::string path = TempPath( "test_rewire_p2e.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "2e: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 1,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( !r.commit.applied, "2e: an out-of-range occurrence is REFUSED, not silently no-op'd" );
			Check( Contains( std::string( r.commit.message.c_str() ), "occurrence(s)" ),
				"2e: ...with the actual occurrence count in the message" );
			Check( DocText( *j ) == before, "2e: the document is BYTE-IDENTICAL after the refusal" );

			const Rewire neg = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), -1,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( !neg.commit.applied, "2e: a NEGATIVE occurrence is refused too" );
			Check( DocText( *j ) == before, "2e: ...also byte-identical" );

			j->release();
			std::remove( path.c_str() );
		}
	}
}

// =====================================================================
// PART 3 -- the nowUnreferenced report
// =====================================================================

static void Part3_NowUnreferenced()
{
	std::printf( "\nPART 3 -- the nowUnreferenced orphan report\n" );

	// ---- 3a. SOLE reference -> reported ------------------------------
	{
		const std::string path = TempPath( "test_rewire_p3a.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "3a: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			// pnt_red is referenced ONLY from blend_owned.colora.
			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( r.commit.applied, "3a: the rewire applies" );
			Check( r.nowUnreferenced.size() == 1, "3a: rewiring away the SOLE reference reports one orphan" );
			if( r.nowUnreferenced.size() == 1 )
				Check( std::string( r.nowUnreferenced[0].c_str() ) == "pnt_red",
					"3a: ...and it names the old referent" );

			// ---- 3c. It is a REPORT, not a delete -------------------
			Check( Contains( DocText( *j ), "name pnt_red" ),
				"3c: the orphaned chunk is STILL in the document (S20 owns the delete policy)" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 3b. ONE OF TWO references -> NOT reported -------------------
	{
		const std::string path = TempPath( "test_rewire_p3b.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "3b: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			// pnt_blue is referenced from blend_shared.colorb AND
			// mat_five.reflectance.  Rewiring mat_five away leaves one.
			const Rewire r = c.RewireConnection(
				ChunkCategory::Material, String( "mat_five" ), String( "reflectance" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( r.commit.applied, "3b: the rewire applies" );
			Check( r.nowUnreferenced.empty(),
				"3b: rewiring away ONE OF TWO references reports NO orphan" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 3b'. TWO SLOTS, ONE OWNER is not "shared" -------------------
	//
	// The "OVER-strict" trap OwnershipClosure.h names: mat_four binds
	// scal_rough from BOTH alphax and alphay.  Two referrer EDGES, one
	// owner.  A referrer-COUNT rule would refuse; the ownership rule must
	// not -- and rewiring one of the two must not report an orphan.
	//
	// P2-5 (S19 review round 1): rewires alphay to a DIFFERENT scalar
	// painter (scal_smooth), not back onto scal_rough.  Re-pointing a slot
	// at the SAME chunk it already names hits `stillBound` in the
	// nowUnreferenced computation and returns before the remainingEdges
	// loop ever runs -- exercising that loop is the whole point of this
	// case (scal_rough must still show ONE remaining edge, from alphax).
	{
		const std::string path = TempPath( "test_rewire_p3c.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "3b': graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const Rewire r = c.RewireConnection(
				ChunkCategory::Material, String( "mat_four" ), String( "alphay" ), 0,
				ChunkCategory::Painter, String( "scal_smooth" ), nullptr );
			Check( r.commit.applied,
				"3b': a two-edges-one-owner target is NOT treated as shared" );
			Check( r.nowUnreferenced.empty(),
				"3b': ...and rewiring one of the two edges away leaves scal_rough with ONE remaining "
				"edge (alphax), so the remainingEdges loop -- actually exercised here, not "
				"short-circuited by stillBound -- correctly reports no orphan" );
			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 3d. UNDO / REDO with a NON-EMPTY nowUnreferenced ---------------
	//
	// P3 (S19 review round 1): `nowUnreferenced` is a PREDICTION made
	// before the mutating commit (see OwnershipClosure.h's field doc) --
	// PART 4a already pins that undo/redo round-trips the Document bytes
	// exactly, but nothing yet checks that a REDO of a commit whose
	// `nowUnreferenced` was non-empty actually leaves the predicted orphan
	// orphaned in the real, restored Document -- i.e. that the prediction
	// and the redone reality agree, not just that undo/redo is byte-exact
	// in the abstract.
	{
		const std::string path = TempPath( "test_rewire_p3d.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "3d: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			// Same edit as 3a: pnt_red's SOLE reference is rewired away.
			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( r.commit.applied, "3d: the rewire applies" );
			Check( r.nowUnreferenced.size() == 1
			    && std::string( r.nowUnreferenced[0].c_str() ) == "pnt_red",
				"3d: ...predicting pnt_red as newly orphaned (the 3a case, restated as this test's setup)" );

			c.Undo();
			Check( !SceneReferenceGraph::FindReferencesTo(
				*j->GetCstDocument(), ChunkCategory::Painter, String( "pnt_red" ) ).empty(),
				"3d: after UNDO, pnt_red is REFERENCED again (blend_owned.colora points back at it)" );

			c.Redo();
			Check( SceneReferenceGraph::FindReferencesTo(
				*j->GetCstDocument(), ChunkCategory::Painter, String( "pnt_red" ) ).empty(),
				"3d: after REDO, pnt_red is ACTUALLY unreferenced again -- the redone Document agrees "
				"with what `nowUnreferenced` predicted BEFORE the undo/redo round-trip, not just a "
				"byte-identical restoration of the post-rewire text" );

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
	std::printf( "\nPART 4 -- undo/redo, mid-transaction, epoch, conflict\n" );

	// ---- 4a. UNDO / REDO byte-identity -------------------------------
	{
		const std::string path = TempPath( "test_rewire_p4a.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "4a: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( r.commit.applied, "4a: the rewire applies" );
			const std::string afterEdit = DocText( *j );
			Check( afterEdit != before, "4a: ...and it really changed the document" );

			c.Undo();
			Check( DocText( *j ) == before, "4a: UNDO restores the ORIGINAL bytes exactly" );

			c.Redo();
			Check( DocText( *j ) == afterEdit, "4a: REDO restores the post-rewire bytes exactly" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 4b. MID-TRANSACTION refusal --------------------------------
	{
		const std::string path = TempPath( "test_rewire_p4b.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "4b: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const std::string before = DocText( *j );

			const bool opened = c.BeginTransaction();
			Check( opened, "4b: an editor transaction opens" );
			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( !r.commit.applied, "4b: a rewire mid-transaction is REFUSED" );
			Check( r.commit.retriable,
				"4b: ...and marked RETRIABLE (the gesture will end; the same commit can succeed later)" );
			Check( DocText( *j ) == before, "4b: the document is BYTE-IDENTICAL after the refusal" );
			c.EndTransaction();

			// ...and it really does succeed once the gesture completes.
			const Rewire again = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( again.commit.applied, "4b: the identical commit APPLIES once the transaction closes" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 4c. SCENE-EPOCH bump (the S15 canvas-refresh lesson) --------
	{
		const std::string path = TempPath( "test_rewire_p4c.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "4c: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			const unsigned int epochBefore = c.SceneEpoch();
			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( r.commit.applied, "4c: the rewire applies" );
			Check( c.SceneEpoch() != epochBefore,
				"4c: the scene epoch ADVANCES -- an epoch-gated canvas re-enumerates the rewired graph" );

			// A REFUSAL must NOT bump it (nothing changed to re-read).
			const unsigned int epochAfter = c.SceneEpoch();
			const Rewire refused = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_shared" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( !refused.commit.applied, "4c: the shared rewire is refused" );
			Check( c.SceneEpoch() == epochAfter, "4c: ...and a refusal does NOT bump the epoch" );

			j->release();
			std::remove( path.c_str() );
		}
	}

	// ---- 4d. HEAD-VERSION conflict ----------------------------------
	{
		const std::string path = TempPath( "test_rewire_p4d.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "4d: graph fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );

			// Snapshot the head, then move it with an unrelated commit --
			// the snapshot is now STALE.
			const RISE::Cst::CstHeadVersion stale = j->GetCstHeadVersion();
			const Rewire mover = c.RewireConnection(
				ChunkCategory::Material, String( "mat_five" ), String( "reflectance" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), nullptr );
			Check( mover.commit.applied, "4d: an unrelated commit moves the head" );

			const std::string before = DocText( *j );
			const Rewire r = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), &stale );
			Check( !r.commit.applied, "4d: a rewire on a STALE baseVersion does not apply" );
			Check( r.commit.conflict && std::string( r.commit.status.c_str() ) == "conflict",
				"4d: ...it reports a CONFLICT, not a plain rejection" );
			Check( DocText( *j ) == before, "4d: the document is BYTE-IDENTICAL -- a stale patch never clobbers" );
			// P2-1 (S19 review round 1): this rewire is the 3a scenario
			// (blend_owned.colora is pnt_red's SOLE reference) -- the
			// closure step ran to completion BEFORE the conflict gate (it
			// is Clean, computed a real `nowUnreferenced = [pnt_red]`), and
			// only the mutating commit itself refused.  A refusal must
			// never report an orphan from an edit that never happened.
			Check( r.nowUnreferenced.empty(),
				"4d: the STALE-BASEVERSION conflict reports an EMPTY nowUnreferenced -- even though the "
				"closure step computed a non-empty one before the conflict gate refused the commit" );

			// Re-read the head and retry: it lands.
			const RISE::Cst::CstHeadVersion fresh = j->GetCstHeadVersion();
			const Rewire retry = c.RewireConnection(
				ChunkCategory::Painter, String( "blend_owned" ), String( "colora" ), 0,
				ChunkCategory::Painter, String( "pnt_white" ), &fresh );
			Check( retry.commit.applied, "4d: re-reading the head and retrying APPLIES" );

			j->release();
			std::remove( path.c_str() );
		}
	}
}

// =====================================================================
// PART 5 -- OwnershipClosure as a PURE function of a Cst::Document
// =====================================================================

namespace
{
	//! (category, name) -> NodeId, for the pure-document cases.
	Cst::NodeId Id( const Cst::Document& d, ChunkCategory cat, const char* name )
	{
		return SceneReferenceGraph::ResolveChunk( d, cat, String( name ) );
	}
}

static void Part5_PureClosure()
{
	std::printf( "\nPART 5 -- OwnershipClosure as a pure function of a Cst::Document\n" );

	// ---- 5a. OwnersOf --------------------------------------------------
	{
		const Cst::Document d = Cst::ParseToCst( std::string( kGraphScene ) );

		const Cst::NodeId owned  = Id( d, ChunkCategory::Painter,  "blend_owned" );
		const Cst::NodeId shared = Id( d, ChunkCategory::Painter,  "blend_shared" );
		const Cst::NodeId rough  = Id( d, ChunkCategory::Painter,  "scal_rough" );
		const Cst::NodeId white  = Id( d, ChunkCategory::Painter,  "pnt_white" );
		const Cst::NodeId matOne = Id( d, ChunkCategory::Material, "mat_one" );
		Check( owned && shared && rough && white && matOne, "5a: every fixture chunk resolves" );

		Check( OwnershipClosure::OwnersOf( d, owned ).size() == 1,
			"5a: a solely-owned painter has ONE owner" );
		Check( OwnershipClosure::OwnersOf( d, shared ).size() == 2,
			"5a: a painter reachable from two materials has TWO owners" );
		Check( OwnershipClosure::OwnersOf( d, rough ).size() == 1,
			"5a: TWO SLOTS ON ONE MATERIAL is still ONE owner (edge count is not the unit)" );
		Check( OwnershipClosure::OwnersOf( d, white ).size() == 1,
			"5a: a painter referenced only from a single material's graph has ONE owner" );

		// A ROOT owns itself, however many consumers it has -- otherwise
		// the ordinary "one material, several objects" link semantics
		// would read as shared and refuse every edit.
		const std::vector<Cst::NodeId> mo = OwnershipClosure::OwnersOf( d, matOne );
		Check( mo.size() == 1 && mo[0] == matOne, "5a: a ROOT (material) owns itself" );

		Check( OwnershipClosure::IsInteriorCategory( ChunkCategory::Painter ),
			"5a: Painter is an interior graph node kind" );
		Check( OwnershipClosure::IsInteriorCategory( ChunkCategory::Function ),
			"5a: Function is an interior graph node kind" );
		Check( !OwnershipClosure::IsInteriorCategory( ChunkCategory::Material ),
			"5a: Material is a ROOT kind" );

		// An ORPHAN interior node (nothing references it) owns itself,
		// so it is solely owned and editable rather than "0 owners".
		{
			std::string t( kGraphScene );
			t += "\nblend_painter\n{\nname blend_orphan\ncolora pnt_red\ncolorb pnt_green\nmask pnt_white\n}\n";
			const Cst::Document od = Cst::ParseToCst( t );
			const Cst::NodeId orph = Id( od, ChunkCategory::Painter, "blend_orphan" );
			Check( orph != 0, "5a: the orphan chunk resolves" );
			Check( OwnershipClosure::OwnersOf( od, orph ).size() == 1,
				"5a: an ORPHAN interior node owns itself (solely owned, not un-owned)" );
		}

		// ---- the closure SET itself --------------------------------
		{
			TopologyEditIntent it;
			it.targetChunk = owned;
			it.paramName   = "colora";
			it.newRefChunk = white;
			const OwnershipClosureResult r = OwnershipClosure::Compute( d, it );
			Check( r.Clean(), "5a: a solely-owned target computes a Clean closure" );
			// blend_owned + pnt_red + pnt_green: all reachable from it and
			// all owned by mat_one alone.  pnt_green is ALSO named by
			// blend_shared, whose owners are {mat_two, mat_three} -- so
			// pnt_green has THREE owners and must be EXCLUDED.
			bool sawOwned = false, sawRed = false, sawGreen = false;
			for( size_t i = 0; i < r.closure.size(); ++i ) {
				const std::string n( r.closure[i].name.c_str() );
				if( n == "blend_owned" ) sawOwned = true;
				if( n == "pnt_red" )     sawRed   = true;
				if( n == "pnt_green" )   sawGreen = true;
			}
			Check( sawOwned, "5a: the closure contains the target itself" );
			Check( sawRed,   "5a: ...and its solely-owned child" );
			Check( !sawGreen,
				"5a: ...but NOT a child that another graph also reaches (sect. 3.7a's `unshared` clause)" );
		}
	}

	// ---- 5b. DETACH intent ------------------------------------------
	{
		const Cst::Document d = Cst::ParseToCst( std::string( kGraphScene ) );
		TopologyEditIntent it;
		it.kind        = TopologyEditKind::Detach;
		it.targetChunk = Id( d, ChunkCategory::Painter, "blend_owned" );
		it.paramName   = "colora";
		const OwnershipClosureResult r = OwnershipClosure::Compute( d, it );
		Check( r.Clean(), "5b: a Detach intent on a solely-owned target is Clean" );
		Check( r.nowUnreferenced.size() == 1
		    && std::string( r.nowUnreferenced[0].c_str() ) == "pnt_red",
			"5b: ...and reports what detaching would orphan (pre-flight, with no commit path)" );

		// The SHARED target refuses a Detach for the same reason it
		// refuses a Rewire.
		TopologyEditIntent st;
		st.kind        = TopologyEditKind::Detach;
		st.targetChunk = Id( d, ChunkCategory::Painter, "blend_shared" );
		st.paramName   = "colora";
		const OwnershipClosureResult sr = OwnershipClosure::Compute( d, st );
		Check( sr.classification == ClosureClassification::SharedTarget,
			"5b: a Detach on a SHARED target refuses identically to a Rewire" );
	}

	// ---- 5c. EXPRESSION-DRIVEN target -------------------------------
	//
	// sect. 3.7a's residual `let`/expression case.  Detected structurally
	// (an `expr` body plus repeatable `def` let-bindings), so a future
	// expression-bodied chunk is covered the day it is registered.
	{
		std::string t( kGraphScene );
		t += "\nexpression_painter\n{\nname expr_field\ndef a u*2.0\nexpr vec3(a, a, a)\n}\n";
		const Cst::Document d = Cst::ParseToCst( t );
		TopologyEditIntent it;
		it.targetChunk = Id( d, ChunkCategory::Painter, "expr_field" );
		it.paramName   = "expr";
		Check( it.targetChunk != 0, "5c: the expression chunk resolves" );
		const OwnershipClosureResult r = OwnershipClosure::Compute( d, it );
		Check( r.classification == ClosureClassification::ExpressionDrivenTarget,
			"5c: an expression-bodied chunk is REFUSED as un-rewritable-as-a-unit" );
		Check( Contains( r.diagnostic, "expression_painter" ),
			"5c: ...with a diagnostic naming the chunk kind" );
		Check( Contains( r.diagnostic, kClosureDuplicateHatch ),
			"5c: ...and the escape hatch" );
		Check( r.owners.empty(),
			"5c: P3 -- `owners` stays EMPTY on ExpressionDrivenTarget even though the target itself "
			"resolved fine (the owner walk never runs -- refused before it) -- pins the corrected "
			"OwnershipClosureResult::owners doc comment against an earlier 'always populated' claim" );

		// A NON-expression painter in the same document is unaffected --
		// the detector keys on the descriptor's shape, not on the
		// document containing an expression chunk somewhere.
		TopologyEditIntent ok;
		ok.targetChunk = Id( d, ChunkCategory::Painter, "blend_owned" );
		ok.paramName   = "colora";
		Check( OwnershipClosure::Compute( d, ok ).Clean(),
			"5c: an ordinary painter in the same document is still Clean" );
	}

	// ---- 5d. CYCLE SAFETY on a hostile document ---------------------
	//
	// ParseToCst is PURE SYNTAX -- it never derives -- so a test can hand
	// this layer a cyclic reference graph that the real parser could
	// never produce.  This is the only layer that can be handed one; the
	// walk must terminate, not hang.
	{
		const std::string cyc =
			"RISE ASCII SCENE 7\n"
			"blend_painter\n{\nname cyc_a\ncolora cyc_b\ncolorb cyc_b\n}\n\n"
			"blend_painter\n{\nname cyc_b\ncolora cyc_a\ncolorb cyc_a\n}\n";
		const Cst::Document d = Cst::ParseToCst( cyc );
		const Cst::NodeId a = Id( d, ChunkCategory::Painter, "cyc_a" );
		Check( a != 0, "5d: the cyclic fixture parses (syntax only -- it never derives)" );
		const std::vector<Cst::NodeId> owners = OwnershipClosure::OwnersOf( d, a );
		Check( !owners.empty(), "5d: OwnersOf TERMINATES on a cycle and reports the members, not a hang" );
		Check( owners.size() == 2,
			"5d: ...specifically BOTH cycle members (cyc_a, cyc_b) -- the pure-cycle fallback (the "
			"header's hostile-input contract) reports the cycle's own members as their own owners" );

		TopologyEditIntent it;
		it.targetChunk = a;
		it.paramName   = "colora";
		const OwnershipClosureResult r = OwnershipClosure::Compute( d, it );
		// P3 (S19 review round 1): pin the SPECIFIC classification, not a
		// disjunction over two possible answers.  With owners == {cyc_a,
		// cyc_b}, `Compute`'s `owners.size() >= 2` test is unavoidably TRUE
		// here -- SharedTarget is the only classification a pure cycle
		// (with no external root) can ever reach, so asserting Clean was
		// ever also acceptable was hiding that this case never actually
		// exercises Clean at all.
		Check( r.classification == ClosureClassification::SharedTarget,
			"5d: Compute terminates on a cycle -- and since a pure cycle's OwnersWalk fallback always "
			"reports >= 2 owners (the cycle's own members), the verdict is SPECIFICALLY SharedTarget, "
			"never Clean, for this fixture" );
	}

	// ---- 5e. An UNRESOLVABLE target ---------------------------------
	{
		const Cst::Document d = Cst::ParseToCst( std::string( kGraphScene ) );
		TopologyEditIntent it;
		it.targetChunk = 0;
		it.paramName   = "colora";
		const OwnershipClosureResult r = OwnershipClosure::Compute( d, it );
		Check( r.classification == ClosureClassification::UnresolvedTarget,
			"5e: a NodeId that resolves to nothing is refused, never crashed on" );
		Check( r.closure.empty(), "5e: ...with an empty closure" );
	}

	// ---- 5f. NON-MATERIAL ROOT: a rasterizer's radiance_map is a root too --
	//
	// S19 review round 1 P2-7: every SharedTarget case so far in this file
	// (1b, 2c's candidate side, 5a) shares a painter between two MATERIALS.
	// sect. 3.7a's rule is "shared between two independent ROOTS" -- any
	// non-Painter/Function category, per `IsInteriorCategory` -- not
	// specifically "two materials".  Wire the SAME painter into a
	// material's `reflectance` AND the rasterizer's `radiance_map` (an
	// environment-map painter is exactly as real a root-level consumer as
	// a material) and confirm (a) a Rasterizer-category chunk counts as a
	// root exactly like Material does, so the painter ends up with TWO
	// owners and refuses; and (b) the UNNAMED rasterizer in kGraphScene
	// (it carries no `name` param) renders through the `<keyword>`
	// fallback `DisplayName` uses for every unnamed root -- this is the
	// first case in the file that exercises that fallback as an ACTUAL
	// reported owner, not just a "these bytes didn't move" spot check.
	{
		std::string t( kGraphScene );
		const std::string anchor =
			"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n}\n\n";
		const size_t at = t.find( anchor );
		Check( at != std::string::npos, "5f: fixture anchor found" );
		if( at != std::string::npos ) {
			t.replace( at, anchor.size(),
				"pathtracing_pel_rasterizer\n{\nsamples 4\npixel_filter box\noidn_denoise false\n"
				"radiance_map blend_owned\n}\n\n" );
		}
		const Cst::Document d = Cst::ParseToCst( t );
		const Cst::NodeId owned = Id( d, ChunkCategory::Painter, "blend_owned" );
		Check( owned != 0, "5f: blend_owned resolves" );

		const std::vector<Cst::NodeId> owners = OwnershipClosure::OwnersOf( d, owned );
		Check( owners.size() == 2,
			"5f: a painter bound into BOTH a material's slot and the rasterizer's radiance_map has "
			"TWO owners -- the rasterizer is a root, not an interior node" );

		TopologyEditIntent it;
		it.targetChunk = owned;
		it.paramName   = "colora";
		it.newRefChunk = Id( d, ChunkCategory::Painter, "pnt_white" );
		const OwnershipClosureResult r = OwnershipClosure::Compute( d, it );
		Check( r.classification == ClosureClassification::SharedTarget,
			"5f: ...so an edit targeting one of its own slots is refused as SharedTarget, exactly as "
			"the two-materials case is -- the rule generalizes past Material roots" );
		Check( r.owners.size() == 2, "5f: ...with both owning roots reported" );
		bool sawMatOne = false, sawRasterizer = false;
		for( size_t i = 0; i < r.owners.size(); ++i ) {
			const std::string n( r.owners[i].c_str() );
			if( n == "mat_one" ) sawMatOne = true;
			if( n == "<pathtracing_pel_rasterizer>" ) sawRasterizer = true;
		}
		Check( sawMatOne, "5f: ...the MATERIAL owner is named normally" );
		Check( sawRasterizer,
			"5f: ...and the UNNAMED rasterizer owner renders via the `<keyword>` fallback DisplayName "
			"uses for every unnamed root, not a blank name that would corrupt the diagnostic prose" );
	}
}

// =====================================================================
// PART 6 -- the occurrence-addressing scoping claim, pinned
// =====================================================================

static void Part6_OccurrenceScopingSweep()
{
	std::printf( "\nPART 6 -- registry sweep: repeatable pure-Reference params\n" );

	// RewireConnection's header CLAIMS that no chunk kind addressable
	// through it declares a repeatable pure-`ValueKind::Reference`
	// parameter today -- which is why occurrence-addressed rewire has no
	// reachable end-to-end case.  A claim in prose rots; this sweep
	// FAILS the day it stops being true, which is exactly the day
	// occurrence addressing becomes reachable and needs a real test.
	//
	// The one repeatable pure-Reference param that DOES exist,
	// `standard_shader.shaderop`, lives on a Shader-category chunk the
	// controller has no (category, name) addressing scheme for
	// (RoleKindSuffixForCategory rejects it), so it is not reachable
	// here.  It is listed by name below so the sweep documents WHICH
	// exception it is tolerating rather than tolerating any.
	const std::vector<ChunkParserEntry> entries = CreateAllChunkParsers();

	int addressableRepeatableRefs = 0;
	int knownUnaddressable = 0;
	for( const ChunkParserEntry& e : entries ) {
		if( !e.parser ) continue;
		const ChunkDescriptor& d = e.parser->Describe();
		if( d.keyword.empty() || d.keyword != e.keyword ) continue;   // skip legacy aliases
		for( size_t p = 0; p < d.parameters.size(); ++p ) {
			const ParameterDescriptor& pd = d.parameters[p];
			if( !pd.repeatable || pd.kind != ValueKind::Reference ) continue;
			// Reachable through RewireConnection iff its category has a
			// UI name-addressing scheme.  Mirrors
			// UiCategoryForChunkCategory's accepted set.
			const bool addressable =
				   d.category == ChunkCategory::Painter
				|| d.category == ChunkCategory::Function
				|| d.category == ChunkCategory::Material
				|| d.category == ChunkCategory::Object
				|| d.category == ChunkCategory::Light
				|| d.category == ChunkCategory::Medium
				|| d.category == ChunkCategory::Geometry
				|| d.category == ChunkCategory::Camera;
			if( addressable ) {
				++addressableRepeatableRefs;
				std::printf( "        addressable repeatable Reference param: %s.%s\n",
					d.keyword.c_str(), pd.name.c_str() );
			} else {
				++knownUnaddressable;
			}
		}
	}

	Check( addressableRepeatableRefs == 0,
		"6: NO addressable chunk kind declares a repeatable pure-Reference param -- "
		"occurrence-addressed rewire has no reachable case, as RewireConnection's header states "
		"(if this fails, that scoping note is now WRONG and needs an end-to-end occurrence test)" );
	Check( knownUnaddressable >= 1,
		"6: ...and the one that exists at all (standard_shader.shaderop) is on an unaddressable "
		"category, so the claim is a real narrowing, not a vacuous one" );

	// ---- mirror assertion (P3, S19 review round 1) ---------------------
	//
	// The `addressable` bool above is a HAND COPY of two real functions'
	// combined behavior: `UiCategoryForChunkCategory` (ChunkCategory -> UI
	// Category) and `RoleKindSuffixForCategory` (UI Category -> does it
	// have a chunk-name addressing scheme).  Both live in an anonymous
	// namespace in SceneEditController.cpp (internal linkage) -- not
	// reachable from this TU, and giving them external linkage / a header
	// declaration is a bigger change than this fix round's budget, so this
	// stays a hand copy DELIBERATELY rather than by oversight.  To bound
	// the drift risk anyway: `SceneEditController::DuplicateEntity` is a
	// PUBLIC entry point that calls `RoleKindSuffixForCategory` itself and
	// refuses up front with the EXACT message "category has no chunk-name
	// addressing scheme to duplicate from" when it rejects -- so probing
	// THAT message for a real Job is a live check against the actual
	// private table, not a second hand copy of it.  (`Function` is not
	// separately probed -- `UiCategoryForChunkCategory` maps it to the
	// SAME `Category::Painter` the Painter probe already covers; `Shader`
	// is not probed at all -- `SceneEditController::Category` has no
	// Shader value, matching the prose above.)
	{
		const std::string path = TempPath( "test_rewire_p6_mirror.RISEscene" );
		Job* j = LoadScene( kGraphScene, path );
		Check( j != nullptr, "6: mirror-probe fixture loads" );
		if( j ) {
			SceneEditController c( *j, nullptr );
			auto probe = [&]( SceneEditController::Category cat, const std::string& label ) {
				String outName;
				const SceneEditController::AgentCommitResult r = c.DuplicateEntity(
					cat, String( "definitely_does_not_exist_xyz123" ), &outName );
				Check( !Contains( std::string( r.message.c_str() ),
					"category has no chunk-name addressing scheme to duplicate from" ),
					"6: mirror -- " + label + " really IS addressable per the live "
					"RoleKindSuffixForCategory behavior, not just the hand-copied list" );
			};
			probe( SceneEditController::Category::Painter,  "Painter (also stands in for Function)" );
			probe( SceneEditController::Category::Material, "Material" );
			probe( SceneEditController::Category::Object,   "Object" );
			probe( SceneEditController::Category::Light,    "Light" );
			probe( SceneEditController::Category::Medium,   "Medium" );
			probe( SceneEditController::Category::Geometry, "Geometry" );
			probe( SceneEditController::Category::Camera,   "Camera" );
			j->release();
			std::remove( path.c_str() );
		}
	}
}

// =====================================================================
// PART 7 -- PERF PIN (S19 review round 1 P2-2): owners-for-every-node
//   batch stays the same order as ONE adjacency build.
// =====================================================================

static void Part7_OwnersOfBatchPerf()
{
	std::printf( "\nPART 7 -- perf pin: OwnersOf batch vs ONE Adjacency build\n" );

	// Synthetic ~1508-chunk fixture: no checked-in probe scene sits at
	// exactly that scale (ReferenceGraphTest's own perf-gate fixture,
	// sombrero.RISEscene, is 7442 chunks and lives under scenes/, which
	// this self-contained-scene-only test deliberately never reads), so
	// this generates one -- 754 uniformcolor_painter + 754 lambertian_
	// material chunks, each material binding its OWN distinct painter
	// (graph SHAPE does not matter for this pin; chunk COUNT does, since
	// that is what the index/reverse-edge build below scales with).
	const int N = 754;
	std::string t = "RISE ASCII SCENE 7\n";
	for( int i = 0; i < N; ++i ) {
		char buf[128];
		std::snprintf( buf, sizeof( buf ), "uniformcolor_painter\n{\nname perf_p%d\ncolor 0.5 0.5 0.5\n}\n\n", i );
		t += buf;
	}
	for( int i = 0; i < N; ++i ) {
		char buf[160];
		std::snprintf( buf, sizeof( buf ),
			"lambertian_material\n{\nname perf_m%d\nreflectance perf_p%d\n}\n\n", i, i );
		t += buf;
	}
	const Cst::Document d = Cst::ParseToCst( t );
	const std::vector<SceneReferenceGraph::DocumentChunk> prescanned = SceneReferenceGraph::AllChunks( d );
	Check( prescanned.size() == static_cast<size_t>( 2 * N ),
		"7: the synthetic fixture really has 1508 chunks (754 painters + 754 materials)" );

	// ONE adjacency build.
	const auto adjT0 = std::chrono::steady_clock::now();
	const OwnershipClosure::Adjacency adj = OwnershipClosure::BuildAdjacencyFor( d );
	const auto adjT1 = std::chrono::steady_clock::now();
	const double buildMs = std::chrono::duration<double, std::milli>( adjT1 - adjT0 ).count();

	// The BATCH: OwnersOf for EVERY node, reusing the SAME prebuilt
	// Adjacency -- the P2-2 call pattern a canvas badge-every-node refresh
	// uses.  Before P2-2, the only way to do this was N independent calls
	// each rebuilding its own index/reverse map, i.e. N adjacency builds
	// hiding inside what reads like a cheap per-node query.
	const auto batchT0 = std::chrono::steady_clock::now();
	for( size_t i = 0; i < prescanned.size(); ++i )
		OwnershipClosure::OwnersOf( d, prescanned[i].id, nullptr, nullptr, &adj );
	const auto batchT1 = std::chrono::steady_clock::now();
	const double batchMs = std::chrono::duration<double, std::milli>( batchT1 - batchT0 ).count();

	std::printf( "  PERF: 1 Adjacency build = %.3f ms; %d-node OwnersOf batch over it = %.3f ms\n",
		buildMs, static_cast<int>( prescanned.size() ), batchMs );

	// Loose ratio, the S11 perf-gate precedent (ReferenceGraphTest PART 6):
	// not a tight wall-clock number, but a budget that fails hard on the
	// REGRESSION this pin exists to catch -- OwnersOf silently going back
	// to rebuilding its own index per call, which would make the batch
	// scale as N * buildMs instead of buildMs + O(N) cheap walks.  20x
	// leaves generous headroom for a slow/loaded CI machine; a floor of
	// 50ms keeps the budget meaningful when `buildMs` itself rounds to
	// ~0 on a fast machine.
	const double budgetMs = ( buildMs * 20.0 > 50.0 ) ? buildMs * 20.0 : 50.0;
	Check( batchMs < budgetMs,
		"7: the owners-for-every-node BATCH is the same order as ONE adjacency build, not N of them" );
}

int main()
{
	std::printf( "RewireConnectionTest -- doc-88 Phase 3 S19 (ownership closure + REFUSE)\n" );

	Part1_AcceptanceCriteria();
	Part2_OtherRefusals();
	Part3_NowUnreferenced();
	Part4_CommitDiscipline();
	Part5_PureClosure();
	Part6_OccurrenceScopingSweep();
	Part7_OwnersOfBatchPerf();

	std::printf( "\n==================================================\n" );
	std::printf( "RewireConnectionTest: %d passed, %d failed\n", g_pass, g_fail );
	std::printf( "==================================================\n" );
	return g_fail == 0 ? 0 : 1;
}
