//////////////////////////////////////////////////////////////////////
//
//  AgentAddWetnessTest.cpp - docs/WETNESS_COAT_DESIGN.md Phase 1 + Phase
//    2 item 8 (sec 5 Track 1, sec 6, sec 13, 2026-08-31): add_wetness,
//    the VERB half of design-note condition P.
//
//  WHAT THIS VERB HAS TO GET RIGHT, and therefore what is measured here,
//  mirrors AgentAddWearTest.cpp's own discipline:
//    (a) the FOUR branches (Lambertian -> `coated_material` WRAP, item
//        8: the base chunk is left untouched and every bound object's
//        `material` reference moves to a new wrapper chunk; GGX/PBR
//        in-place roughness modulation, UNCHANGED by item 8; Oren-Nayar
//        damp-only, UNCHANGED by item 8; a NAMED metallic material's
//        coat-only) each write exactly the chunks sec 6.2/13 item 8
//        specify and nothing else;
//    (b) the wet <= damp invariant holds BOTH textually (the shared
//        param/def prelude is byte-identical across every consumer
//        chunk a single call writes -- sec 6.1's mechanism) AND
//        numerically (a sweep of every emitted param at its declared
//        min/max keeps 0 <= wet <= damp <= 1, evaluated through the
//        REAL expression VM, not recomputed by hand in the test);
//    (c) EVERY generated chunk round-trips through RISE::Cst::ParseToCst
//        -- the mid-flight correction this file exists to guard: the
//        design doc's own sec 6.3/6.5 prelude listing wraps the final
//        `expr`/`expression` value across two physical lines for
//        readability, and that two-line form does NOT parse (the chunk
//        parser is line-based per parameter) -- so every value this
//        verb emits must be ONE physical line, and this file proves it
//        by deriving the rewritten document, not merely string-matching
//        it;
//    (d) every refusal clause leaves the document BYTE-IDENTICAL,
//        including both directions of the add_wear collision (which
//        item 8 had to re-derive: a coat-WRAPPED base no longer trips
//        either verb's own-params scan, so both scans now also check
//        `namesAlreadyCoated`, "is this material named as `base` by a
//        `coated_material` chunk");
//    (e) collision-safe name minting, and the eight-surface wire
//        checks (MCP advertised + routable, RPC dispatch, chat-codec
//        table).
//
//  Cases:
//    A  LAMBERTIAN -> `coated_material` WRAP, bare call (item 8): the
//       sole qualifying material's OWN chunk is left BYTE-IDENTICAL;
//       coat_weight/coat_roughness minted and bound on a NEW
//       `coated_material` chunk naming it as `base`; every bound object
//       rebound to the wrapper; NO reflectance/darkening painter (the
//       layered transport performs it); prelude byte-identical across
//       both minted chunks, parses+derives+renders non-black with zero
//       error diagnostics.
//    A2 THE wet <= damp INVARIANT, numerically, swept over every
//       emitted param at its declared min/max.
//    A3 THE SIGNAL DRIVES IT: a pooled (occlusion-favourable, high
//       curvature-favourable via ridge=0) synthetic hit reads WETTER
//       than a ridge (convex, ridge-favourable) hit, read straight off
//       the minted `coat_weight` painter.
//    B  GGX in-place, named (UNCHANGED by item 8): reflectance -> `rd`;
//       alphax/alphay each get their OWN roughness field (never one
//       shared chunk), banded around the authored constant.
//    C  PBR-MR in-place, named (UNCHANGED by item 8): reflectance ->
//       `base_color`; roughness field is an `expression_painter` (the
//       colour-pipe trap), not a `scalar_painter`.
//    D  OREN-NAYAR, named (UNCHANGED by item 8, evaluated and declined
//       for this slice -- see the design doc's item 8 note): reflectance
//       only -- no coat, no roughness touch at all (damp-only, sec 6.2).
//    E  METALLIC, named: coat/gloss only -- no reflectance touch, `rd`
//       stays bound to the ORIGINAL painter.
//    F  REFUSALS, each with the document BYTE-IDENTICAL afterwards: no
//       non-metallic candidate (bare call on an all-metallic document);
//       unknown chunk name; a named chunk of the wrong kind; a textured
//       albedo (clause 2); already wet (re-running on the same
//       material, on BOTH the coat-wrap and the in-place shapes); the
//       add_wear collision, BOTH directions (including the coat-wrap
//       shape, which neither verb's own-params scan alone can see).
//    G  NAME-COLLISION MINTING: a pre-existing `<material>_wetcoat`
//       chunk forces the mint to bump to `<material>_wetcoat2`, not a
//       refusal.
//    H  WIRE SURFACE: declared in the shared chat-codec table,
//       advertised AND routable on MCP, dispatches through
//       AgentRpcDispatcher, and a refusal is a SUCCESSFUL response.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentDiagnostic.h"
#include "../src/Library/Agent/Json.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

static std::string TempPath( const char* name )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
	return dir + name;
}

static Job* LoadScene( const std::string& text, const std::string& path )
{
	{ std::ofstream o( path.c_str(), std::ios::binary ); o << text; }
	Job* pJob = new Job();
	if( !pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
		pJob->release();
		std::remove( path.c_str() );
		return nullptr;
	}
	return pJob;
}

//----------------------------------------------------------------------
// A synthetic hit, ProbeHit's own idiom from AgentAddWearTest.cpp: only
// mean curvature varies between probes, so any difference in output is
// attributable to `curv` alone.  Wetness ALSO reads `occlusion()`, which
// on a synthetic (scene-less) hit reads its NEUTRAL fallback (1.0,
// unoccluded) regardless of geometry -- fine for the invariant sweep
// below, which pins the EXPRESSION MATH at a fixed occlusion reading,
// not the occlusion signal itself (that is GEOMETRY_SHADING_SIGNALS_
// DESIGN.md's own coverage).
//----------------------------------------------------------------------
static RayIntersectionGeometric ProbeHit( double meanCurvature )
{
	RayIntersectionGeometric ri( Ray(), nullRasterizerState );
	ri.bHit = true;
	ri.ptCoord = Point2( 0.31, 0.62 );
	ri.ptIntersection = Point3( 0.4, 0.2, 0.7 );
	ri.ptObjIntersec  = Point3( 0.4, 0.2, 0.7 );
	ri.vNormal = Vector3( 0, 1, 0 );
	ri.onb = OrthonormalBasis3D( Vector3( 1, 0, 0 ), Vector3( 0, 0, 1 ), Vector3( 0, 1, 0 ) );
	ri.derivatives.scaleHint      = 1.0;
	ri.derivatives.curvature      = meanCurvature;
	ri.derivatives.curvatureValid = true;
	return ri;
}

static double LumaOf( const RISEPel& p )
{
	return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

//! The value on the FIRST `param <which> <value> ...` line at or after
//! chunk `chunkName`'s `name` line -- AgentAddWearTest.cpp's own helper.
static double ParamValueInChunk( const std::string& doc, const std::string& chunkName,
                                 const std::string& which, bool& ok )
{
	ok = false;
	const std::size_t namePos = doc.find( chunkName );
	if( namePos == std::string::npos ) return 0.0;
	std::size_t pos = namePos;
	while( true ) {
		pos = doc.find( "param", pos );
		if( pos == std::string::npos ) return 0.0;
		const std::size_t eol = doc.find( '\n', pos );
		const std::string line = doc.substr( pos, eol - pos );
		const std::size_t w = line.find( which );
		if( w != std::string::npos ) {
			const std::size_t vs = line.find_first_not_of( " \t", w + which.size() );
			if( vs != std::string::npos ) {
				ok = true;
				return std::strtod( line.c_str() + vs, nullptr );
			}
		}
		pos = ( eol == std::string::npos ) ? doc.size() : eol;
		if( pos >= doc.size() ) return 0.0;
	}
}

//! The prelude substring emitted for `chunkName` -- from the `dryness`
//! param line through the `wet` def line inclusive.  Byte-identical
//! across every consumer chunk a single add_wetness call writes is
//! sec 6.1's own mechanism for the wet<=damp invariant; this is the
//! textual half of case A2/G's check.
static std::string ExtractPrelude( const std::string& doc, const std::string& chunkName )
{
	const std::string nameMarker = "name\t\t\t" + chunkName + "\n";
	const std::size_t namePos = doc.find( nameMarker );
	if( namePos == std::string::npos ) return std::string();
	const std::size_t start = doc.find( "param\t\t\tdryness", namePos );
	if( start == std::string::npos ) return std::string();
	static const char* const kEndMarker = "def\t\t\t\twet clamp(damp * film_amount, 0, damp)\n";
	const std::size_t endMarkerPos = doc.find( kEndMarker, start );
	if( endMarkerPos == std::string::npos ) return std::string();
	const std::size_t end = endMarkerPos + std::strlen( kEndMarker );
	return doc.substr( start, end - start );
}

//! The full `{ ... }` body text of the FIRST chunk named `chunkName`
//! (our own emitted chunks never contain a literal brace, so the next
//! "\n}\n" after the name line is always the close).
static std::string ExtractChunkFullText( const std::string& doc, const std::string& chunkName )
{
	const std::string nameMarker = "name\t\t\t" + chunkName + "\n";
	std::size_t namePos = doc.find( nameMarker );
	if( namePos == std::string::npos ) return std::string();
	// Walk back to the start of this chunk (the keyword line before the
	// nearest preceding "\n{\n" -- i.e. the last blank-or-keyword line).
	std::size_t braceOpen = doc.rfind( "\n{\n", namePos );
	if( braceOpen == std::string::npos ) return std::string();
	std::size_t lineStart = doc.rfind( '\n', ( braceOpen == 0 ) ? 0 : braceOpen - 1 );
	lineStart = ( lineStart == std::string::npos ) ? 0 : lineStart + 1;
	const std::size_t close = doc.find( "\n}\n", namePos );
	if( close == std::string::npos ) return std::string();
	return doc.substr( lineStart, close + 3 - lineStart );
}

//----------------------------------------------------------------------
// Fixtures.
//----------------------------------------------------------------------
static std::string Preamble()
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_stone\n\tcolor 0.42 0.4 0.37\n}\n\n"
		"uniformcolor_painter\n{\n\tname pnt_spec\n\tcolor 0.6 0.6 0.6\n}\n\n"
		"expression_painter\n{\n\tname pnt_textured\n\texpr vec3(u, v, 0.5)\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.4 0.5 0.9\n}\n\n";
}

static std::string SphereGeo( const std::string& name )
{
	return "sphere_geometry\n{\n\tname " + name + "\n\tradius 1.0\n}\n\n";
}

static std::string Obj( const std::string& name, const std::string& geo,
                        const std::string& mat, double x )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", x );
	return "standard_object\n{\n\tname " + name + "\n\tgeometry " + geo + "\n\tmaterial " + mat +
	       "\n\tposition " + buf + " 0 0\n}\n\n";
}

static std::string Lambertian( const std::string& name, const std::string& refl )
{
	return "lambertian_material\n{\n\tname " + name + "\n\treflectance " + refl + "\n}\n\n";
}

static std::string OrenNayar( const std::string& name, const std::string& refl, double roughness )
{
	char buf[32]; std::snprintf( buf, sizeof( buf ), "%g", roughness );
	return "orennayar_material\n{\n\tname " + name + "\n\treflectance " + refl + "\n\troughness " + buf + "\n}\n\n";
}

static std::string GgxNonMetal( const std::string& name, const std::string& rd, double alpha )
{
	char buf[32]; std::snprintf( buf, sizeof( buf ), "%g", alpha );
	return "ggx_material\n{\n\tname " + name + "\n\trd " + rd + "\n\trs pnt_spec\n"
	       "\talphax " + buf + "\n\talphay " + buf + "\n\tior 1.5\n\textinction 0.0\n"
	       "\tfresnel_mode schlick_f0\n}\n\n";
}

static std::string GgxMetal( const std::string& name, const std::string& rd, double alpha )
{
	char buf[32]; std::snprintf( buf, sizeof( buf ), "%g", alpha );
	// fresnel_mode UNSPELLED -- the descriptor default is "conductor", so
	// this is metallic per WetnessMaterialIsMetallic_ without needing to
	// say so explicitly (proving the DEFAULT is read correctly, not just
	// an explicit "fresnel_mode conductor" line).
	return "ggx_material\n{\n\tname " + name + "\n\trd " + rd + "\n\trs pnt_spec\n"
	       "\talphax " + buf + "\n\talphay " + buf + "\n\tior 2.45\n\textinction 3.45\n}\n\n";
}

static std::string PbrNonMetal( const std::string& name, const std::string& baseColor, double roughness )
{
	char buf[32]; std::snprintf( buf, sizeof( buf ), "%g", roughness );
	return "pbr_metallic_roughness_material\n{\n\tname " + name + "\n\tbase_color " + baseColor +
	       "\n\tmetallic 0.0\n\troughness " + buf + "\n}\n\n";
}

//----------------------------------------------------------------------

static void TestLambertianRewrite()
{
	std::printf( "A: Lambertian -> coated_material WRAP, bare call (item 8)\n" );
	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += Lambertian( "mat_lam", "pnt_stone" );
	body += Obj( "o1", "s", "mat_lam", 0 );
	const std::string tmp = TempPath( "addwet_a.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "A: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness();
	Check( r.ok && r.applied, std::string( "A: the no-argument call APPLIED -- " ) + r.message );
	Check( r.status == "applied", "A: status is \"applied\"" );
	Check( r.material == "mat_lam", "A: it took the sole qualifying material" );
	Check( r.materialKind == "lambertian_material",
	       "A MONEY: it STILL reports lambertian_material -- item 8's wrap never changes the base's own kind" );
	Check( r.wrappedInCoat, "A MONEY: wrappedInCoat is true" );
	Check( !r.coatedMaterial.empty(), "A: the minted coated_material chunk is named" );
	Check( !r.coatWeightPainter.empty() && !r.coatRoughnessPainter.empty(),
	       "A: both coat field chunks are named" );
	Check( r.rebindObjectCount == 1, "A MONEY: exactly the one bound object was rebound" );
	Check( r.reflectanceSlot.empty() && r.reflectancePainter.empty(),
	       "A MONEY: NO reflectance/darkening painter on this branch -- coated_material's own layered "
	       "transport performs the darkening (sec 7.1), so a second painter here would double-count it" );
	Check( r.scatteringSlots.empty() && r.scatteringPainters.empty(),
	       "A: the GGX/PBR in-place branch's scatteringSlots/Painters stay EMPTY here -- this branch's "
	       "gloss half is coatRoughnessPainter instead" );
	Check( std::fabs( r.baseR - 0.42 ) < 1e-9 && std::fabs( r.baseG - 0.4 ) < 1e-9 &&
	       std::fabs( r.baseB - 0.37 ) < 1e-9,
	       "A2: baseR/G/B still report the authored pnt_stone colour (read, even though never rewritten)" );
	// ---- P2: the success message states the +Y-up axis assumption AND
	// interpolates the bound-object count into the PROSE (not just JSON).
	Check( r.message.find( "+Y" ) != std::string::npos && r.message.find( "vec3(0,1,0)" ) != std::string::npos,
	       "A MONEY: the success message states the gravity gate assumes +Y up (dot(N, vec3(0,1,0))) -- "
	       "a Z-up scene needs that axis hand-edited, and the message says so rather than leaving it a "
	       "silent surprise" );
	Check( r.message.find( std::to_string( r.boundObjects ) ) != std::string::npos,
	       "A MONEY: the bound-object count is interpolated into the PROSE message, not just the JSON "
	       "`objects` field -- sec 6.9 item 14's blast-radius figure" );
	Check( r.message.find( "double-count" ) != std::string::npos,
	       "A MONEY: the success message itself explains why there is no separate darkening painter, not "
	       "just the empty struct field" );

	const std::string docAfter = sess->ReadDocument();
	Check( docBefore != docAfter, "A: the document really changed" );
	Check( docAfter.find( "coated_material" ) != std::string::npos,
	       "A MONEY: a `coated_material` chunk was minted" );
	Check( docAfter.find( "lambertian_material" ) != std::string::npos,
	       "A MONEY: the ORIGINAL `lambertian_material` keyword is STILL PRESENT -- item 8's wrap, unlike "
	       "Phase 1's rewrite, never removes the base chunk" );
	Check( docAfter.find( "name mat_lam\n\treflectance pnt_stone\n" ) != std::string::npos,
	       "A MONEY: the base chunk's own body -- name AND reflectance -- is BYTE-IDENTICAL to what was "
	       "authored: `mat_lam` is still `reflectance pnt_stone`, untouched" );
	Check( docAfter.find( "base\t\t\tmat_lam" ) != std::string::npos,
	       "A MONEY: the coated_material's `base` names the untouched original" );
	Check( docAfter.find( "coat_weight\t\t" + r.coatWeightPainter ) != std::string::npos,
	       "A: coat_weight names the minted scalar_painter" );
	Check( docAfter.find( "coat_roughness\t\t" + r.coatRoughnessPainter ) != std::string::npos,
	       "A: coat_roughness names the minted scalar_painter" );
	Check( docAfter.find( "coat_ior" ) == std::string::npos,
	       "A MONEY: NO `coat_ior` line -- the descriptor's own default (1.33, sec 7.2's water value) is "
	       "left to speak for itself rather than spelling out a literal that matches it anyway" );
	Check( docAfter.find( "material " + r.coatedMaterial ) != std::string::npos,
	       "A MONEY: the bound object's `material` reference now names the coated wrapper, not `mat_lam`" );

	// ---- Parse round-trip -- the mid-flight correction this file exists
	// to guard: a two-line-wrapped `expr`/`expression` value does NOT
	// parse, so every value this verb emits must be ONE physical line.
	{
		const RISE::Cst::Document rt = RISE::Cst::ParseToCst( docAfter );
		Check( RISE::Cst::DocItemCount( rt ) > 0,
		       "A MONEY: the emitted document ROUND-TRIPS through RISE::Cst::ParseToCst -- catches a "
		       "line-wrapped expr/expression/def value the way a plain string-match on the source text "
		       "cannot" );
	}
	Check( pJob->GetScene() != nullptr, "A: the rewritten document still derives" );
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( docAfter );
		bool anyError = false;
		for( const Agent::AgentDiagnostic& d : diags )
			if( d.severity == Agent::AgentDiagnostic::Severity::Error ) anyError = true;
		Check( !anyError,
		       "A MONEY: the rewritten document validates with ZERO error diagnostics -- the generated "
		       "expression bodies really compile in the VM, not merely parse as text" );
	}
	{
		Agent::AgentRenderParams rp;
		rp.width = 32; rp.height = 32; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "A: the rewritten scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "A: ...and is non-black" );
	}

	// ---- Textual half of the wet<=damp invariant: byte-identical prelude.
	{
		const std::string preludeWeight = ExtractPrelude( docAfter, r.coatWeightPainter );
		const std::string preludeRough  = ExtractPrelude( docAfter, r.coatRoughnessPainter );
		Check( !preludeWeight.empty() && !preludeRough.empty(),
		       "A: both preludes were extracted" );
		Check( preludeWeight == preludeRough,
		       "A MONEY: the param/def prelude is BYTE-IDENTICAL across coat_weight/coat_roughness -- "
		       "sec 6.1's mechanism for the wet<=damp invariant (painters cannot reference one another's "
		       "def names, so it must be duplicated verbatim rather than shared)" );
	}
	Check( docAfter.find( "occlusion(0.08)" ) != std::string::npos,
	       "A MONEY: the occlusion radius is a NUMERIC LITERAL, add_wear's own reason (the DynR twin "
	       "reads the neutral fallback on an indexed mesh)" );
	Check( docAfter.find( "min " ) != std::string::npos && docAfter.find( "max " ) != std::string::npos &&
	       docAfter.find( "step " ) != std::string::npos && docAfter.find( "label " ) != std::string::npos,
	       "A MONEY: every emitted `param` carries min/max/step/label metadata" );
	Check( docAfter.find( "\tseed" ) != std::string::npos, "A: ...and a `seed` line" );
	Check( docAfter.find( "sqrt(2.0/(film_gloss_lo+2.0))" ) != std::string::npos,
	       "A MONEY: coat_roughness is the Phong-exponent band RE-EXPRESSED as a GGX alpha via "
	       "alpha=sqrt(2/(n+2)) (sec 6.3) -- not a raw Phong exponent copied into a slot that expects an "
	       "alpha" );

	// ---- A2: THE wet<=damp INVARIANT, numerically, over every emitted
	// param's declared min/max.  A "damp probe" chunk -- byte-identical to
	// the real coat_weight chunk except its final line reads `damp` instead
	// of `wet` -- lets the test read damp straight out of the SAME
	// expression text add_wetness actually emitted, rather than
	// recomputing it by hand.
	{
		std::string dampProbeText = ExtractChunkFullText( docAfter, r.coatWeightPainter );
		Check( !dampProbeText.empty(), "A2: the coat_weight chunk's full text was extracted" );
		const std::string dampProbeName = r.coatWeightPainter + "_dampprobe";
		{
			const std::size_t namePos = dampProbeText.find( "name\t\t\t" + r.coatWeightPainter + "\n" );
			Check( namePos != std::string::npos, "A2: the damp-probe's name line was found for renaming" );
			if( namePos != std::string::npos )
				dampProbeText.replace( namePos, std::string( "name\t\t\t" + r.coatWeightPainter ).size(),
				                       "name\t\t\t" + dampProbeName );
		}
		{
			const std::size_t exprPos = dampProbeText.rfind( "expression\t\twet\n" );
			Check( exprPos != std::string::npos, "A2: the damp-probe's `expression wet` line was found" );
			if( exprPos != std::string::npos )
				dampProbeText.replace( exprPos, std::string( "expression\t\twet" ).size(), "expression\t\tdamp" );
		}

		static const char* const kParams[] = {
			"dryness", "pool_gain", "ridge_shed", "base_wetness", "gravity_bias",
			"film_amount", "breakup_amp", "breakup_scale"
		};
		static const double kMins[] = { 0, 0, 0, 0, 0, 0, 0, 0.1 };
		static const double kMaxs[] = { 1, 4, 6, 1, 1, 1, 1, 40 };

		int sweepsRun = 0, sweepsOk = 0;
		for( int p = 0; p < 8; ++p ) {
			for( int bound = 0; bound < 2; ++bound ) {
				const double v = ( bound == 0 ) ? kMins[p] : kMaxs[p];
				RISE::Cst::Document d = RISE::Cst::ParseToCst( docAfter + "\n" + dampProbeText );
				char valBuf[32]; std::snprintf( valBuf, sizeof( valBuf ), "%g", v );
				const RISE::Cst::NodeId weightId = RISE::Cst::DocFindByNameAnyRole( d, r.coatWeightPainter );
				const RISE::Cst::NodeId dampId   = RISE::Cst::DocFindByNameAnyRole( d, dampProbeName );
				if( !weightId || !dampId ) continue;
				d = RISE::Cst::DocSetParamValue( d, weightId, kParams[p], 0, valBuf );
				d = RISE::Cst::DocSetParamValue( d, dampId,   kParams[p], 0, valBuf );
				const std::string variantText = RISE::Cst::SerializeCst( d );
				const std::string tmpv = TempPath( ( std::string( "addwet_a2_" ) +
					std::to_string( p ) + "_" + std::to_string( bound ) + ".RISEscene" ).c_str() );
				Job* pv = LoadScene( variantText, tmpv );
				if( !pv ) continue;
				IScalarPainterManager* sm = pv->GetScalarPainters();
				IScalarPainter* weightField = sm ? sm->GetItem( r.coatWeightPainter.c_str() ) : nullptr;
				IScalarPainter* dampField   = sm ? sm->GetItem( dampProbeName.c_str() ) : nullptr;
				if( weightField && dampField ) {
					for( double curv : { -1.5, 0.0, 1.5 } ) {
						++sweepsRun;
						const double wet  = weightField->GetValuesAt( ProbeHit( curv ) ).v[0];
						const double damp = dampField->GetValuesAt( ProbeHit( curv ) ).v[0];
						const bool ok = wet >= -1e-9 && wet <= damp + 1e-9 && damp <= 1.0 + 1e-9 && damp >= -1e-9;
						if( ok ) ++sweepsOk;
						else std::printf( "    A2 sweep fail: param=%s bound=%d curv=%g wet=%.6f damp=%.6f\n",
						                  kParams[p], bound, curv, wet, damp );
					}
				}
				pv->release();
				std::remove( tmpv.c_str() );
			}
		}
		Check( sweepsRun >= 8 * 2 * 3 - 6, "A2: the sweep actually ran across (nearly) every param x bound x curvature" );
		Check( sweepsOk == sweepsRun,
		       "A2 MONEY: 0 <= wet <= damp <= 1 holds for EVERY emitted param at its declared min/max, "
		       "evaluated through the real expression VM -- pins the `film_amount` bound and the outer "
		       "clamp exactly as sec 6.3 specifies" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestSignalDrivesIt()
{
	std::printf( "A3: the signal drives it -- ridge vs. flat reads drier\n" );
	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += Lambertian( "mat_lam", "pnt_stone" );
	body += Obj( "o1", "s", "mat_lam", 0 );
	const std::string tmp = TempPath( "addwet_a3.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "A3: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness();
	Check( r.ok && r.applied, "A3: applied" );
	if( r.applied ) {
		IScalarPainterManager* sm = pJob->GetScalarPainters();
		IScalarPainter* weightField = sm ? sm->GetItem( r.coatWeightPainter.c_str() ) : nullptr;
		Check( weightField != nullptr, "A3: the minted coat_weight painter resolved in the live manager" );
		if( weightField ) {
			const double ridge = weightField->GetValuesAt( ProbeHit(  2.5 ) ).v[0];   // convex ridge: sheds water
			const double flat  = weightField->GetValuesAt( ProbeHit(  0.0 ) ).v[0];
			std::printf( "    A3: coat_weight (wet)  ridge %.4f  flat %.4f\n", ridge, flat );
			Check( ridge < flat - 1e-6,
			       "A3 MONEY: a CONVEX (ridge) hit reads DRIER than a flat one at the same world point -- "
			       "the `ridge` term genuinely subtracts from `damp_raw`, which is what \"convex ridges "
			       "shed water\" (sec 6.3) means in the emitted expression, not merely in prose" );
		}
		// ---- Item 8's OWN claim, checked live rather than merely by absent
		// field: the base material's colour painter is genuinely UNTOUCHED --
		// it still reads the flat authored constant at BOTH probes, since
		// nothing on this branch rebinds it any more (darkening is now the
		// coated_material transport's job, sec 7.1).
		IPainterManager* pm = pJob->GetPainters();
		IPainter* baseField = pm ? pm->GetItem( "pnt_stone" ) : nullptr;
		Check( baseField != nullptr, "A3: the ORIGINAL base painter is still live and resolvable by its own name" );
		if( baseField ) {
			const double ridgeLuma = LumaOf( baseField->GetColor( ProbeHit(  2.5 ) ) );
			const double flatLuma  = LumaOf( baseField->GetColor( ProbeHit(  0.0 ) ) );
			Check( std::fabs( ridgeLuma - flatLuma ) < 1e-9,
			       "A3 MONEY: the base painter reads IDENTICALLY at ridge and flat -- it is a plain "
			       "uniformcolor_painter nothing rebinds any more, confirming there is no separate "
			       "darkening pass on this branch" );
		}
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestGgxInPlace()
{
	std::printf( "B: GGX in-place, named -- alphax/alphay each get their OWN field\n" );
	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += GgxNonMetal( "mat_ggx", "pnt_stone", 0.2 );
	body += Obj( "o1", "s", "mat_ggx", 0 );
	const std::string tmp = TempPath( "addwet_b.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "B: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_ggx" );
	Check( r.ok && r.applied, std::string( "B: applied -- " ) + r.message );
	Check( !r.wrappedInCoat, "B: NOT wrapped in a coated_material (in-place branch, unchanged by item 8)" );
	Check( r.coatedMaterial.empty() && r.coatWeightPainter.empty() && r.coatRoughnessPainter.empty(),
	       "B MONEY: NO coat fields at all -- GGX has no separate coat lobe in Phase 1" );
	Check( r.reflectanceSlot == "rd", "B: reflectance rebound `rd`, the albedo slot" );
	Check( r.scatteringSlots.size() == 2, "B: both alphax and alphay were rebound" );
	Check( r.scatteringPainters.size() == 2, "B MONEY: TWO roughness painters were minted, one per slot" );
	if( r.scatteringPainters.size() == 2 )
		Check( r.scatteringPainters[0] != r.scatteringPainters[1],
		       "B MONEY: the two roughness painters are DIFFERENT chunks -- sharing one would flatten an "
		       "authored alphax!=alphay anisotropy even at dryness=1 (fully dry)" );

	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "ggx_material" ) != std::string::npos, "B: still a ggx_material" );
	Check( docAfter.find( "rd " + r.reflectancePainter ) != std::string::npos,
	       "B: `rd` names the minted reflectance chunk" );
	for( std::size_t i = 0; i < r.scatteringPainters.size(); ++i ) {
		Check( docAfter.find( r.scatteringSlots[i] + " " + r.scatteringPainters[i] ) != std::string::npos,
		       "B: " + r.scatteringSlots[i] + " names its minted chunk" );
		bool okBase = false;
		const double base = ParamValueInChunk( docAfter, r.scatteringPainters[i], "rough_base", okBase );
		Check( okBase && std::fabs( base - 0.2 ) < 1e-9,
		       "B MONEY: rough_base is EXACTLY the authored 0.2 -- the dry state is banded around what "
		       "was already there" );
	}
	{
		const RISE::Cst::Document rt = RISE::Cst::ParseToCst( docAfter );
		Check( RISE::Cst::DocItemCount( rt ) > 0, "B: the emitted document round-trips through ParseToCst" );
	}
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( docAfter );
		bool anyError = false;
		for( const Agent::AgentDiagnostic& d : diags ) if( d.severity == Agent::AgentDiagnostic::Severity::Error ) anyError = true;
		Check( !anyError, "B: the rewritten document validates with zero error diagnostics" );
	}
	// ---- P2: extend the textual wet<=damp invariant check to the GGX
	// branch's THREE emitted chunks (reflectance + two roughness fields).
	if( r.scatteringPainters.size() == 2 ) {
		const std::string preludeRefl  = ExtractPrelude( docAfter, r.reflectancePainter );
		const std::string preludeRough0 = ExtractPrelude( docAfter, r.scatteringPainters[0] );
		const std::string preludeRough1 = ExtractPrelude( docAfter, r.scatteringPainters[1] );
		Check( !preludeRefl.empty() && !preludeRough0.empty() && !preludeRough1.empty(),
		       "B: all three GGX-branch preludes were extracted" );
		Check( preludeRefl == preludeRough0 && preludeRough0 == preludeRough1,
		       "B MONEY: the param/def prelude is BYTE-IDENTICAL across reflectance and BOTH roughness "
		       "fields on the GGX in-place branch -- the same sec 6.1 mechanism the Lambertian branch "
		       "uses, extended to the three-chunk shape this branch actually emits" );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestPbrInPlace()
{
	std::printf( "C: PBR-MR in-place, named -- roughness field is an expression_painter\n" );
	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += PbrNonMetal( "mat_pbr", "pnt_stone", 0.4 );
	body += Obj( "o1", "s", "mat_pbr", 0 );
	const std::string tmp = TempPath( "addwet_c.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "C: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_pbr" );
	Check( r.ok && r.applied, std::string( "C: applied -- " ) + r.message );
	Check( r.reflectanceSlot == "base_color", "C: reflectance rebound `base_color`" );
	Check( r.scatteringSlots.size() == 1 && r.scatteringSlots[0] == "roughness",
	       "C: exactly one roughness slot, `roughness`" );
	const std::string docAfter = sess->ReadDocument();
	Check( r.scatteringPainters.size() == 1, "C: one roughness painter minted" );
	if( r.scatteringPainters.size() == 1 ) {
		const std::string chunkText = ExtractChunkFullText( docAfter, r.scatteringPainters[0] );
		Check( chunkText.find( "expression_painter" ) == 0,
		       "C MONEY: the roughness field is an `expression_painter`, NOT a `scalar_painter` -- "
		       "PBR-MR's roughness slot is Color-pipe by construction and a scalar_painter there "
		       "silently synthesizes a ZERO painter rather than binding (ChunkParserRegistry.cpp's own "
		       "documented trap)" );
		Check( chunkText.find( "expr\t\t\tmix(rough_base, rough_wet, wet)" ) != std::string::npos,
		       "C: ...and it uses `expr`, the colour-pipe form, not `expression`" );
	}
	{
		const RISE::Cst::Document rt = RISE::Cst::ParseToCst( docAfter );
		Check( RISE::Cst::DocItemCount( rt ) > 0, "C: the emitted document round-trips through ParseToCst" );
	}
	// ---- P2: extend the textual wet<=damp invariant check to the PBR-MR
	// branch's TWO emitted chunks (reflectance + one roughness field).
	if( r.scatteringPainters.size() == 1 ) {
		const std::string preludeRefl  = ExtractPrelude( docAfter, r.reflectancePainter );
		const std::string preludeRough = ExtractPrelude( docAfter, r.scatteringPainters[0] );
		Check( !preludeRefl.empty() && !preludeRough.empty(),
		       "C: both PBR-branch preludes were extracted" );
		Check( preludeRefl == preludeRough,
		       "C MONEY: the param/def prelude is BYTE-IDENTICAL across reflectance and the roughness "
		       "field on the PBR-MR in-place branch" );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestOrenNayarDampOnly()
{
	std::printf( "D: Oren-Nayar, named -- damp only, no coat/roughness touch\n" );
	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += OrenNayar( "mat_orn", "pnt_stone", 0.3 );
	body += Obj( "o1", "s", "mat_orn", 0 );
	const std::string tmp = TempPath( "addwet_d.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "D: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_orn" );
	Check( r.ok && r.applied, std::string( "D: applied -- " ) + r.message );
	Check( r.isOrenNayar, "D: isOrenNayar is true" );
	Check( !r.reflectancePainter.empty(), "D: reflectance WAS rebound" );
	Check( !r.wrappedInCoat && r.coatedMaterial.empty() && r.coatWeightPainter.empty() &&
	       r.coatRoughnessPainter.empty() && r.scatteringPainters.empty(),
	       "D MONEY: NO coat wrap, NO scattering/roughness field -- Oren-Nayar's substrate lobe is strictly "
	       "Lambertian and there is no coat concept for it (sec 6.2; item 8 evaluated and declined the "
	       "re-target for this branch on scope, see the design doc's item 8 note)" );
	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "roughness 0.3" ) != std::string::npos ||
	       docAfter.find( "roughness\t\t\t0.3" ) != std::string::npos ||
	       docAfter.find( "roughness\t0.3" ) != std::string::npos,
	       "D MONEY: the material's OWN `roughness` (its retroreflective sigma) is UNTOUCHED at 0.3" );
	Check( docBefore != docAfter, "D: the document still changed (the reflectance rebind)" );
	// ---- P1-D: round-trip the emission, the same block cases A/B/C use --
	// the Oren-Nayar branch had NO such check before this review round.
	{
		const RISE::Cst::Document rt = RISE::Cst::ParseToCst( docAfter );
		Check( RISE::Cst::DocItemCount( rt ) > 0, "D: the emitted document round-trips through ParseToCst" );
	}
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( docAfter );
		bool anyError = false;
		for( const Agent::AgentDiagnostic& d : diags ) if( d.severity == Agent::AgentDiagnostic::Severity::Error ) anyError = true;
		Check( !anyError, "D: the rewritten document validates with zero error diagnostics" );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestMetallicCoatOnly()
{
	std::printf( "E: metallic, named -- coat/gloss only, no darkening\n" );
	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += GgxMetal( "mat_metal", "pnt_stone", 0.1 );
	body += Obj( "o1", "s", "mat_metal", 0 );
	const std::string tmp = TempPath( "addwet_e.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "E: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_metal" );
	Check( r.ok && r.applied, std::string( "E: applied -- " ) + r.message );
	Check( r.isMetallic, "E MONEY: isMetallic is true (fresnel_mode defaulted to conductor)" );
	Check( r.reflectancePainter.empty() && r.reflectanceSlot.empty(),
	       "E MONEY: NO reflectance touch -- a metal has no subsurface to darken (sec 2.1)" );
	Check( r.scatteringPainters.size() == 2, "E: both alphax/alphay roughness fields were minted" );
	Check( r.message.find( "no darkening" ) != std::string::npos ||
	       r.message.find( "no subsurface to darken" ) != std::string::npos,
	       "E MONEY: the success message itself EXPLAINS the skip-darkening rule, not just the struct "
	       "fields -- a caller reading only the prose still learns why `rd` is untouched" );
	const std::string docAfter = sess->ReadDocument();
	Check( docAfter.find( "rd pnt_stone" ) != std::string::npos,
	       "E MONEY: `rd` is STILL bound to the ORIGINAL `pnt_stone` painter -- darkening was skipped "
	       "entirely, not applied-and-then-undone" );
	// ---- P1-D: round-trip the emission -- the metallic branch had NO such
	// check before this review round.
	{
		const RISE::Cst::Document rt = RISE::Cst::ParseToCst( docAfter );
		Check( RISE::Cst::DocItemCount( rt ) > 0, "E: the emitted document round-trips through ParseToCst" );
	}
	{
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( docAfter );
		bool anyError = false;
		for( const Agent::AgentDiagnostic& d : diags ) if( d.severity == Agent::AgentDiagnostic::Severity::Error ) anyError = true;
		Check( !anyError, "E: the rewritten document validates with zero error diagnostics" );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//----------------------------------------------------------------------
// F: refusals.
//----------------------------------------------------------------------
static void TestRefusals()
{
	std::printf( "F: refusals -- document byte-identical afterwards\n" );

	// F1: bare call, ONLY a metallic candidate exists.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += GgxMetal( "mat_metal", "pnt_stone", 0.1 );
		body += Obj( "o1", "s", "mat_metal", 0 );
		const std::string tmp = TempPath( "addwet_f1.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness();
			Check( !r.applied, "F1 MONEY: the bare call REFUSES when every candidate is metallic" );
			Check( r.status.empty(), "F1: a pre-commit refusal carries an EMPTY status" );
			Check( sess->ReadDocument() == before, "F1: document byte-identical" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F2: unknown chunk name.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_lam", "pnt_stone" );
		body += Obj( "o1", "s", "mat_lam", 0 );
		const std::string tmp = TempPath( "addwet_f2.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "no_such_chunk" );
			Check( !r.applied, "F2: refused" );
			Check( r.message.find( "no chunk named" ) != std::string::npos, "F2: message says so" );
			Check( sess->ReadDocument() == before, "F2: document byte-identical" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F3: a named chunk that exists but is the wrong KIND.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_lam", "pnt_stone" );
		body += Obj( "o1", "s", "mat_lam", 0 );
		const std::string tmp = TempPath( "addwet_f3.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F3: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "pnt_stone" );
			Check( !r.applied, "F3: refused -- `pnt_stone` is a painter, not a rewritable material kind" );
			Check( r.message.find( "not a kind this verb rewrites" ) != std::string::npos,
			       "F3: message names the rule" );
			Check( sess->ReadDocument() == before, "F3: document byte-identical" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F4: textured albedo -- clause 2.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_tex", "pnt_textured" );
		body += Obj( "o1", "s", "mat_tex", 0 );
		const std::string tmp = TempPath( "addwet_f4.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F4: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_tex" );
			Check( !r.applied,
			       "F4 MONEY: a textured albedo REFUSES -- Phase 1 cannot darken a textured substrate "
			       "(the expression VM cannot sample another painter), so this declines rather than "
			       "deliver a coat with no darkening" );
			Check( sess->ReadDocument() == before, "F4: document byte-identical" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F5b: already wet, GGX in-place (kind stays put -- clause 4's own-params
	// test fires, unchanged by item 8).
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += GgxNonMetal( "mat_ggx", "pnt_stone", 0.2 );
		body += Obj( "o1", "s", "mat_ggx", 0 );
		const std::string tmp = TempPath( "addwet_f5b.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F5b: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWetnessResult first = sess->AddWetness( "mat_ggx" );
			Check( first.applied, "F5b: the first call applied" );
			const std::string afterFirst = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult second = sess->AddWetness( "mat_ggx" );
			Check( !second.applied,
			       "F5b MONEY: a SECOND add_wetness call on the same (still ggx_material) target REFUSES "
			       "-- already wet" );
			Check( second.message.find( "already been made wet" ) != std::string::npos,
			       "F5b: message says so" );
			Check( sess->ReadDocument() == afterFirst, "F5b: document byte-identical to after the FIRST call" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F5c (item 8, 2026-08-31): already wet, Lambertian coat-WRAP -- unlike
	// Phase 1's `polished_material` rewrite (which changed the chunk's own
	// KEYWORD, so a second call failed on F3's "not a kind this verb
	// rewrites" before clause 4 was ever reached), item 8's wrap leaves
	// `mat_lam` a `lambertian_material` with its colour slot untouched, so
	// a second call on the SAME NAME must be caught by the NEW
	// `namesAlreadyCoated` half of clause 4 -- "is this material named as
	// `base` by an existing `coated_material` chunk" -- since the own-
	// params scan alone would see nothing unusual.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_lam", "pnt_stone" );
		body += Obj( "o1", "s", "mat_lam", 0 );
		const std::string tmp = TempPath( "addwet_f5c.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F5c: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWetnessResult first = sess->AddWetness( "mat_lam" );
			Check( first.applied && first.wrappedInCoat, "F5c: the first call applied and wrapped" );
			const std::string afterFirst = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult second = sess->AddWetness( "mat_lam" );
			Check( !second.applied,
			       "F5c MONEY: a SECOND add_wetness call on the SAME (still lambertian_material, still "
			       "reading `pnt_stone`) target REFUSES -- already coated, caught via `namesAlreadyCoated` "
			       "rather than via a changed kind" );
			Check( second.message.find( "already" ) != std::string::npos,
			       "F5c: message says so" );
			Check( sess->ReadDocument() == afterFirst, "F5c: document byte-identical to after the FIRST call" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F6: add_wear applied FIRST -> add_wetness refuses, naming add_wear.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += GgxNonMetal( "mat_ggx", "pnt_stone", 0.2 );
		body += Obj( "o1", "s", "mat_ggx", 0 );
		const std::string tmp = TempPath( "addwet_f6.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F6: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWearResult wearResult = sess->AddWear( "mat_ggx" );
			Check( wearResult.applied, "F6: add_wear applied first" );
			const std::string afterWear = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult wetResult = sess->AddWetness( "mat_ggx" );
			Check( !wetResult.applied,
			       "F6 MONEY: add_wetness REFUSES a material `add_wear` already rewrote (clause 2: its "
			       "colour slot no longer binds a plain uniformcolor_painter)" );
			Check( wetResult.message.find( "add_wear" ) != std::string::npos,
			       "F6 MONEY: the refusal message NAMES `add_wear` -- sec 6.4's \"make it loud\" rule" );
			Check( sess->ReadDocument() == afterWear, "F6: document byte-identical to after add_wear" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F7: add_wetness applied FIRST -> add_wear refuses, naming add_wetness
	// (the RECIPROCAL direction -- the fix landed in add_wear itself).
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += GgxNonMetal( "mat_ggx", "pnt_stone", 0.2 );
		body += Obj( "o1", "s", "mat_ggx", 0 );
		const std::string tmp = TempPath( "addwet_f7.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F7: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWetnessResult wetResult = sess->AddWetness( "mat_ggx" );
			Check( wetResult.applied, "F7: add_wetness applied first" );
			const std::string afterWet = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWearResult wearResult = sess->AddWear( "mat_ggx" );
			Check( !wearResult.applied,
			       "F7 MONEY: add_wear REFUSES a material `add_wetness` already rewrote (its colour slot "
			       "reads the wetness prelude's geometry signals)" );
			Check( wearResult.message.find( "add_wetness" ) != std::string::npos,
			       "F7 MONEY: the refusal message NAMES `add_wetness` -- the RECIPROCAL half of sec 6.4's "
			       "collision, fixed in add_wear itself" );
			Check( sess->ReadDocument() == afterWet, "F7: document byte-identical to after add_wetness" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F7b (item 8, 2026-08-31): the SAME reciprocal direction as F7, but on
	// the coat-WRAP shape specifically -- `add_wetness` applied first on a
	// LAMBERTIAN base leaves its colour slot an untouched plain
	// `uniformcolor_painter` (unlike F7's GGX case, whose colour slot DOES
	// get rebound to an expression), so `add_wear`'s own-params scan alone
	// would see nothing unusual.  Proves the `namesAlreadyCoated` half of
	// `add_wear`'s clause (d) actually catches it.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_lam", "pnt_stone" );
		body += Obj( "o1", "s", "mat_lam", 0 );
		const std::string tmp = TempPath( "addwet_f7b.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "F7b: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWetnessResult wetResult = sess->AddWetness( "mat_lam" );
			Check( wetResult.applied && wetResult.wrappedInCoat, "F7b: add_wetness applied first (coat wrap)" );
			const std::string afterWet = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWearResult wearResult = sess->AddWear( "mat_lam" );
			Check( !wearResult.applied,
			       "F7b MONEY: add_wear REFUSES a material `add_wetness` already COAT-WRAPPED, even though "
			       "the material's OWN colour slot is still a plain, untouched uniformcolor_painter -- "
			       "caught via `namesAlreadyCoated` (\"is this material named as `base` by a "
			       "`coated_material` chunk\"), not via the own-params expression-body scan" );
			Check( wearResult.message.find( "coated_material" ) != std::string::npos ||
			       wearResult.message.find( "add_wetness" ) != std::string::npos,
			       "F7b MONEY: the refusal message names the collision (either the coated_material wrapper "
			       "or add_wetness itself)" );
			Check( sess->ReadDocument() == afterWet, "F7b: document byte-identical to after add_wetness" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- P1-E: clause-2 unreadable-base sub-cases -- blackbody, spectral,
	// and a non-default `colorspace` uniformcolor_painter.  All three must
	// take the SAME "cannot read... as a plain Rec.709-linear triple"
	// decline path, distinct from "no colour slot at all".
	{
		static const char* const kNames[] = { "mat_bb", "mat_spec", "mat_cs" };
		for( int i = 0; i < 3; ++i ) {
			std::string body = Preamble();
			body += SphereGeo( "s" );
			if( i == 0 ) {
				body += "blackbody_painter\n{\n\tname pnt_bb\n\ttemperature 3000\n}\n\n";
				body += Lambertian( kNames[i], "pnt_bb" );
			}
			else if( i == 1 ) {
				body += "spectral_painter\n{\n\tname pnt_spec2\n\tcp 550,1.0\n}\n\n";
				body += Lambertian( kNames[i], "pnt_spec2" );
			}
			else {
				body += "uniformcolor_painter\n{\n\tname pnt_romm\n\tcolor 0.5 0.5 0.5\n\tcolorspace ROMMRGB_Linear\n}\n\n";
				body += Lambertian( kNames[i], "pnt_romm" );
			}
			body += Obj( "o1", "s", kNames[i], 0 );
			const std::string tmp = TempPath( ( std::string( "addwet_f8_" ) + std::to_string( i ) + ".RISEscene" ).c_str() );
			Job* pJob = LoadScene( body, tmp );
			Check( pJob != nullptr, std::string( "F8." ) + std::to_string( i ) + ": fixture derives" );
			if( pJob ) {
				std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
				const std::string before = sess->ReadDocument();
				const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( kNames[i] );
				Check( !r.applied,
				       std::string( "F8." ) + std::to_string( i ) + " MONEY: an unreadable base (blackbody / "
				       "spectral / non-default colorspace) REFUSES" );
				Check( r.message.find( "cannot read" ) != std::string::npos &&
				       r.message.find( "there is no base to darken from" ) != std::string::npos,
				       std::string( "F8." ) + std::to_string( i ) + ": the unreadable-base decline text fires "
				       "(not the generic \"no colour slot at all\" one)" );
				Check( sess->ReadDocument() == before,
				       std::string( "F8." ) + std::to_string( i ) + ": document byte-identical" );
				sess.reset(); pJob->release();
			}
			std::remove( tmp.c_str() );
		}
	}

	// ---- D1: an UNREADABLE `metallic` on pbr_metallic_roughness_material
	// (a textured/procedural mask) REFUSES the WHOLE material -- "cannot
	// tell whether darkening applies" -- rather than proceeding as if it
	// were not metallic.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += "pbr_metallic_roughness_material\n{\n\tname mat_pbrunread\n\tbase_color pnt_stone\n"
			"\tmetallic pnt_textured\n\troughness 0.4\n}\n\n";
		body += Obj( "o1", "s", "mat_pbrunread", 0 );
		const std::string tmp = TempPath( "addwet_d1.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "D1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_pbrunread" );
			Check( !r.applied,
			       "D1 MONEY: an unreadable (textured/procedural) `metallic` on pbr REFUSES the whole "
			       "call -- proceeding as if not metallic would risk darkening an actual metal" );
			Check( r.message.find( "cannot tell whether darkening applies" ) != std::string::npos,
			       "D1: the decline message states the rule, not a generic unreadable-base one" );
			Check( sess->ReadDocument() == before, "D1: document byte-identical" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- D2: ggx alphax/alphay disagree in writability (one constant, one
	// spatially varying) -> the pair is SKIPPED ENTIRELY rather than
	// half-modulated; coat/darkening (the reflectance half) still applies.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += "scalar_painter\n{\n\tname rough_var\n\texpression u\n}\n\n";
		body += "ggx_material\n{\n\tname mat_ggxmix\n\trd pnt_stone\n\trs pnt_spec\n"
			"\talphax 0.15\n\talphay rough_var\n\tior 1.5\n\textinction 0.0\n"
			"\tfresnel_mode schlick_f0\n}\n\n";
		body += Obj( "o1", "s", "mat_ggxmix", 0 );
		const std::string tmp = TempPath( "addwet_d2.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "D2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_ggxmix" );
			Check( r.ok && r.applied, std::string( "D2: applied (coat/darkening still apply) -- " ) + r.message );
			Check( r.scatteringSlots.empty() && r.scatteringPainters.empty(),
			       "D2 MONEY: the microsurface pair was SKIPPED ENTIRELY -- never half-modulate an "
			       "anisotropy the author did not write" );
			Check( !r.reflectancePainter.empty(),
			       "D2: the reflectance/darkening half STILL applies -- D2 only withholds the roughness "
			       "pair, not the whole call" );
			Check( r.message.find( "alphax/alphay disagree" ) != std::string::npos,
			       "D2 MONEY: the success message explains WHY no roughness field was minted" );
			const std::string docAfter = sess->ReadDocument();
			Check( docAfter.find( "alphax 0.15" ) != std::string::npos,
			       "D2: alphax is UNTOUCHED at its original literal 0.15" );
			Check( docAfter.find( "alphay rough_var" ) != std::string::npos,
			       "D2: alphay is UNTOUCHED, still bound to `rough_var`" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// ---- no-bound-object refusal: a qualifying material with ZERO
	// standard_object binding it.
	{
		std::string body = Preamble();
		body += Lambertian( "mat_orphan", "pnt_stone" );
		const std::string tmp = TempPath( "addwet_noobj.RISEscene" );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "no-obj: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_orphan" );
			Check( !r.applied, "no-obj MONEY: a material with no bound object REFUSES" );
			Check( r.message.find( "standard_object" ) != std::string::npos,
			       "no-obj: the decline message names the missing binding" );
			Check( sess->ReadDocument() == before, "no-obj: document byte-identical" );
			sess.reset(); pJob->release();
		}
		std::remove( tmp.c_str() );
	}
}

static void TestNameCollisionMinting()
{
	std::printf( "G: name-collision minting -- bumps to a free suffix, not a refusal\n" );
	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += Lambertian( "mat_lam", "pnt_stone" );
	body += Obj( "o1", "s", "mat_lam", 0 );
	// Pre-occupy the FIRST name add_wetness's Lambertian branch would mint
	// (item 8: coat_weight is minted before coat_roughness/coated_material).
	body += "uniformcolor_painter\n{\n\tname mat_lam_wetcoatweight\n\tcolor 0 0 0\n}\n\n";
	const std::string tmp = TempPath( "addwet_g.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "G: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_lam" );
	Check( r.ok && r.applied, std::string( "G: applied despite the collision -- " ) + r.message );
	Check( r.coatWeightPainter == "mat_lam_wetcoatweight2",
	       "G MONEY: the mint BUMPED to `mat_lam_wetcoatweight2` rather than colliding with the "
	       "pre-existing `mat_lam_wetcoatweight` painter" );
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! P1 regression (2026-09-01 review round): `DocFindByNameAnyRole` with an
//! EMPTY `roleKindSuffix` counts a bare-name match across EVERY chunk kind,
//! not just objects.  Naming an object the SAME as its geometry is a
//! routine idiom (34 scenes in this repo alone, e.g.
//! scenes/Tests/Cameras/film_chunk.RISEscene) -- before the fix, the
//! rebind loop's `DocFindByNameAnyRole( work, objName, &occ )` (no suffix)
//! saw TWO bare-name matches for that name (the geometry AND the object)
//! and hard-refused with `occ != 1`, even though the OBJECT itself
//! resolves uniquely once kind-narrowed.  The fix passes `"object"` as
//! the suffix, which matches only `standard_object`/`csg_object`/
//! `override_object` (every keyword ending `_object`) -- exactly
//! `boundObjectNames`'s own population, so it is an EXACT narrowing, not
//! a heuristic one.
static void TestObjectGeometryNameCollision()
{
	std::printf( "J: object name == geometry name -- P1 regression, the routine-idiom collision\n" );
	std::string body = Preamble();
	// Geometry and object share the bare name "dup" -- the exact shape
	// that hard-refused before the "object" roleKindSuffix fix.
	body += SphereGeo( "dup" );
	body += Lambertian( "mat_lam", "pnt_stone" );
	body += Obj( "dup", "dup", "mat_lam", 0 );
	const std::string tmp = TempPath( "addwet_j.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "J: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness();
	Check( r.ok && r.applied,
	       std::string( "J MONEY: add_wetness SUCCEEDS despite the object/geometry name collision -- " ) +
	       r.message );
	Check( r.wrappedInCoat && r.rebindObjectCount == 1,
	       "J MONEY: the coat-wrap fired and the ONE bound object (name-colliding with its own "
	       "geometry) was rebound" );
	if( r.ok && r.applied ) {
		const std::string docAfter = sess->ReadDocument();
		// The OBJECT's `material` param moved; the GEOMETRY chunk of the
		// same bare name is untouched (it never had a `material` param to
		// move in the first place, so this just confirms the right chunk
		// was the one addressed).
		Check( docAfter.find( "material " + r.coatedMaterial ) != std::string::npos,
		       "J MONEY: the rebind actually landed -- `material` now names the coated wrapper" );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//! P2-1 regression (2026-09-01 review round): a `csg_object` ALSO carries
//! a `material` OVERRIDE param (ChunkParserRegistry.cpp), which the scan
//! used to bucket only for `standard_object` -- a csg override stayed
//! bound to the dry base after a coat-wrap rebind, while the success
//! message claimed every reference moved, and a re-run would then be
//! wrongly blocked by `namesAlreadyCoated` with a stale csg reference
//! left behind.  Fixed via `csgMaterialObjectNames`, a separate bucket
//! consumed only by add_wetness's own rebind (see its own doc for why it
//! is not merged into `materialObjectNames`).
static void TestCsgObjectMaterialRebind()
{
	std::printf( "K: csg_object material override is rebound too -- P2-1 regression\n" );
	std::string body = Preamble();
	body += SphereGeo( "s1" );
	body += SphereGeo( "s2" );
	body += Lambertian( "mat_lam", "pnt_stone" );
	body += "uniformcolor_painter\n{\n\tname\tpnt_other\n\tcolor\t0.2 0.2 0.2\n}\n\n";
	body += "lambertian_material\n{\n\tname\tmat_other\n\treflectance\tpnt_other\n}\n\n";
	body += Obj( "o1", "s1", "mat_lam", 0 );
	// o2 deliberately does NOT bind mat_lam -- only o1 (standard_object)
	// and obj_csg (csg_object override, below) do, so rebindObjectCount
	// == 2 pins the csg contribution specifically rather than being
	// muddied by a third unrelated standard_object binding.
	body += Obj( "o2", "s2", "mat_other", 2 );
	// A csg_object combining the o1/o2 OPERAND OBJECTS (obja/objb are
	// Object references, not geometry -- ChunkParserRegistry.cpp's own
	// descriptor), its OWN `material` override naming `mat_lam` -- the
	// binding shape this test exists to catch.
	// `name\t\t\t` (three tabs) matches ExtractChunkFullText's own name-line
	// marker below -- the helper Test A/G already use for chunks add_wetness
	// mints, reused here for this hand-authored fixture too.
	body += "csg_object\n{\n\tname\t\t\tobj_csg\n\tobja\to1\n\tobjb\to2\n\toperation\tunion\n"
	        "\tmaterial\tmat_lam\n\tposition\t4 0 0\n}\n\n";
	const std::string tmp = TempPath( "addwet_k.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "K: fixture derives" );
	if( !pJob ) return;
	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_lam" );
	Check( r.ok && r.applied, std::string( "K: applied -- " ) + r.message );
	Check( r.wrappedInCoat, "K: the coat-wrap fired" );
	Check( r.rebindObjectCount == 2,
	       "K MONEY: BOTH bound references were rebound -- the standard_object (`o1`) AND the "
	       "csg_object's own `material` override (`obj_csg`), not just the former" );
	if( r.ok && r.applied ) {
		const std::string docAfter = sess->ReadDocument();
		// The csg_object's own chunk body must now carry the coated
		// wrapper's name on its `material` line, not the dry `mat_lam` it
		// started with -- extracted via the SAME full-chunk-text helper
		// Test A uses, so this is a real per-chunk check, not a
		// document-wide substring guess that could match `o1`'s line.
		const std::string csgText = ExtractChunkFullText( docAfter, "obj_csg" );
		Check( !csgText.empty(), "K: the csg_object chunk's full text was extracted" );
		Check( csgText.find( "material\t" + r.coatedMaterial + "\n" ) != std::string::npos,
		       "K MONEY: the csg_object's `material` line now names the coated wrapper `" +
		       r.coatedMaterial + "` -- extracted chunk text: " + csgText );
		Check( csgText.find( "material\tmat_lam\n" ) == std::string::npos,
		       "K: ...and no longer names the dry `mat_lam`" );
	}
	// A second call must now be refused -- `namesAlreadyCoated` sees
	// `mat_lam` as `base` on the wrapper, so re-running does not leave
	// the csg reference in limbo (the bug this test guards against would
	// otherwise have let a second call slip through and double-wrap).
	const Agent::AgentSession::AgentAddWetnessResult second = sess->AddWetness( "mat_lam" );
	Check( !second.applied, "K: a second call on the same (now-coated) material refuses" );
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

static void TestWireSurface()
{
	std::printf( "H: wire surface -- RPC dispatch, chat-codec table, MCP advertised AND routable\n" );

	{
		const std::string defs = Agent::ChatToolDefsFingerprint();
		Check( defs.find( "add_wetness" ) != std::string::npos,
		       "H: the verb is declared in the shared kToolDefs table" );
	}

	std::string body = Preamble();
	body += SphereGeo( "s" );
	body += Lambertian( "mat_lam", "pnt_stone" );
	body += Obj( "o1", "s", "mat_lam", 0 );
	const std::string tmp = TempPath( "addwet_h.RISEscene" );
	Job* pJob = LoadScene( body, tmp );
	Check( pJob != nullptr, "H: fixture derives" );
	if( !pJob ) return;

	{
		std::unique_ptr<Agent::AgentSession> mcpSess = Agent::AgentSession::WrapJob( pJob );
		Agent::AgentMcpAdapter mcp( std::move( mcpSess ), Agent::AgentAutonomy::Commit );

		Agent::JsonValue listEnv; std::string lerr;
		Check( Agent::JsonParse( mcp.HandleLine(
		           "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\",\"params\":{}}" ),
		       listEnv, lerr ), "H: tools/list parses" );
		bool advertised = false;
		const Agent::JsonValue& tools = listEnv.get( "result" ).get( "tools" );
		for( std::size_t i = 0; i < tools.size(); ++i )
			if( tools.at( i ).get( "name" ).asString() == "add_wetness" ) advertised = true;
		Check( advertised, "H MONEY: tools/list ADVERTISES add_wetness" );

		Agent::JsonValue callEnv; std::string cerr;
		Check( Agent::JsonParse( mcp.HandleLine(
		           "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
		           "\"params\":{\"name\":\"add_wetness\",\"arguments\":{}}}" ),
		       callEnv, cerr ), "H: tools/call parses" );
		const bool disowned = callEnv.has( "error" ) &&
		                      callEnv.get( "error" ).get( "code" ).asNumber( 0 ) == -32601.0;
		Check( !disowned, "H MONEY: tools/call ROUTES add_wetness (not -32601)" );
	}

	pJob->release();
	std::remove( tmp.c_str() );

	{
		const std::string tmp2 = TempPath( "addwet_h2.RISEscene" );
		Job* pJob2 = LoadScene( body, tmp2 );
		Check( pJob2 != nullptr, "H: rpc fixture derives" );
		if( pJob2 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob2 );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"add_wetness\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "H: the response parses" );
			const Agent::JsonValue& result = env.get( "result" );
			Check( result.get( "applied" ).asBool(), "H: the RPC form applied the rewrite" );
			Check( result.get( "material" ).asString() == "mat_lam", "H: ...and echoes the material" );
			Check( result.get( "wrappedInCoat" ).asBool(), "H: ...and wrappedInCoat" );
			Check( result.get( "coatedMaterial" ).asString().size() > 0, "H: ...and the coated_material it minted" );
			Check( result.get( "coatWeightPainter" ).asString().size() > 0, "H: ...and the coat_weight painter it minted" );
			pJob2->release();
			std::remove( tmp2.c_str() );
		}
	}

	// A refusal is a SUCCESSFUL JSON-RPC response, ok=false, never an error envelope.
	{
		std::string body3 = Preamble();
		body3 += SphereGeo( "s" );
		body3 += GgxMetal( "mat_metal", "pnt_stone", 0.1 );
		body3 += Obj( "o1", "s", "mat_metal", 0 );
		const std::string tmp3 = TempPath( "addwet_h3.RISEscene" );
		Job* pJob3 = LoadScene( body3, tmp3 );
		Check( pJob3 != nullptr, "H-refuse: fixture derives" );
		if( pJob3 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob3 );
			Agent::AgentRpcDispatcher disp( std::move( sess ) );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"add_wetness\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "H-refuse: response parses" );
			Check( !env.has( "error" ),
			       "H-refuse MONEY: a pre-commit refusal is a SUCCESSFUL response, not a JSON-RPC error" );
			Check( env.get( "result" ).get( "applied" ).asBool() == false, "H-refuse: applied is false" );
			pJob3->release();
			std::remove( tmp3.c_str() );
		}
	}
}

//----------------------------------------------------------------------
// P1-C: design-note condition P (DESIGN_DRY_RAIN_SCENE) -- mirrors
// AgentAddWearTest.cpp's condition-L pattern (TestNote), which had ZERO
// coverage for condition P before this review round.
//----------------------------------------------------------------------
static void TestDesignNote()
{
	std::printf( "I: the design note -- condition P (dry rain scene) fires exactly on the predicate\n" );

	auto hasCode = []( const std::vector<Agent::AgentDiagnostic>& d, const char* code )
		-> const Agent::AgentDiagnostic* {
		for( const Agent::AgentDiagnostic& e : d ) if( e.code == code ) return &e;
		return nullptr;
	};
	const char* const kCode = "DESIGN_DRY_RAIN_SCENE";

	// (1) Rain-language COMMENT TRIVIA + a qualifying dry material -> FIRES,
	// names the verb and the material, byte-identical across carriers.
	// (A rain WORD glued into an identifier by underscores, e.g.
	// `rainy_plaza`, deliberately does NOT count -- see WetnessBodyReads-
	// PreludeDefs_'s whole-word discipline; a real standalone word needs a
	// `#`-comment or an unglued chunk `name`.)
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_lam", "pnt_stone" );
		body += Obj( "o1", "s", "mat_lam", 0 );
		body += "\n# The rain finally stopped, but the plaza is still soaked.\n";
		const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( body );
		const Agent::AgentDiagnostic* d = hasCode( diags, kCode );
		Check( d != nullptr, "I1 MONEY: rain-language comment trivia + a dry qualifying material FIRES" );
		if( d ) {
			Check( d->severity == Agent::AgentDiagnostic::Severity::Info,
			       "I1: it is an Info-severity ADVISORY" );
			Check( d->message.find( "add_wetness" ) != std::string::npos,
			       "I1 MONEY: the clause NAMES THE VERB" );
			Check( d->message.find( "NO ARGUMENTS" ) != std::string::npos,
			       "I1: ...and states the zero-argument form" );
			Check( d->message.find( "mat_lam" ) != std::string::npos,
			       "I1 MONEY: it names the SAME material a bare call would take -- one shared predicate" );
			Check( d->message.find( "REFUSES" ) != std::string::npos,
			       "I1: ...and is honest about the refusal shape" );
			Check( d->message.find( "ignore" ) != std::string::npos,
			       "I1: ...and carries its own anti-churn escape" );

			const std::string note = Agent::AgentSession::ComputeDesignNote( body );
			Check( note.find( d->message ) != std::string::npos,
			       "I1 MONEY: the diagnostic message appears BYTE-IDENTICALLY inside the render-result "
			       "note -- one shared formatter, two carriers" );
		}
	}

	// (2) SILENT without rain language -- the SAME material/geometry, no
	// rain word anywhere.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_lam", "pnt_stone" );
		body += Obj( "o1", "s", "mat_lam", 0 );
		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "I2 MONEY: no rain vocabulary anywhere -- SILENT, even with a qualifying material" );
	}

	// (3) Rain language present, but NO wettable material kind at all
	// (`dielectric_material` is not in add_wetness's four-kind vocabulary,
	// so `wetCandidateCount` is 0 regardless of the rain language).
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += "dielectric_material\n{\n\tname mat_glass\n\ttau 0.9\n\tior 1.5\n}\n\n";
		body += Obj( "o1", "s", "mat_glass", 0 );
		body += "\n# Set during a rainstorm in the alley out back.\n";
		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "I3 MONEY: rain language present but NOTHING in the document qualifies (no lambertian/"
		       "orennayar/ggx/pbr material at all) -- SILENT, the gate is on candidate count, not merely "
		       "on the vocabulary" );
	}

	// (3b) Rain language + ONLY metallic candidates -- SILENT.  Regression
	// for the missing !addWetName guard (2026-08-31 review round): metallic
	// materials COUNT as wet candidates (the clause teaches the explicit-
	// naming rule for them) but the SELECTOR skips them, so without the
	// guard the note fired with an EMPTY material name, advertising a bare
	// call that then refuses.  Every plain conductor-default ggx_material
	// classifies metallic, making this the common case, not a corner.
	{
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += GgxMetal( "mat_chrome", "pnt_stone", 0.15 );
		body += Obj( "o1", "s", "mat_chrome", 0 );
		body += "\n# Rain hammers the chrome awning all night.\n";
		Check( hasCode( Agent::AgentSession::ValidateText( body ), kCode ) == nullptr,
		       "I3b MONEY: rain language + only-metallic candidates -- SILENT; the note never "
		       "advertises a bare call the selector cannot satisfy (the !addWetName guard is "
		       "load-bearing here, unlike condition L's belt-and-braces twin)" );
	}

	// (4) SELF-DISARM: after add_wetness runs the sole candidate, the note
	// stops firing (wetCandidateCount drops to 0, below the gate).
	{
		const std::string tmp = TempPath( "addwet_i4.RISEscene" );
		std::string body = Preamble();
		body += SphereGeo( "s" );
		body += Lambertian( "mat_lam", "pnt_stone" );
		body += Obj( "o1", "s", "mat_lam", 0 );
		body += "\n# The rain finally stopped, but the plaza is still soaked.\n";
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "I4: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Check( hasCode( Agent::AgentSession::ValidateText( sess->ReadDocument() ), kCode ) != nullptr,
			       "I4: the note fires before the call" );
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness();
			Check( r.ok && r.applied, "I4: the verb applied" );
			Check( hasCode( Agent::AgentSession::ValidateText( sess->ReadDocument() ), kCode ) == nullptr,
			       "I4 MONEY: the note STOPS firing on the document the verb just produced -- the wet "
			       "material leaves the candidate set (clause 4: it now reads the wetness prelude), "
			       "which drops the count below the gate" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// (5) P2 SELF-CERTIFICATION GUARD: a wetted document (carrying `def
	// wet`/`def damp` lines full of rain vocabulary) with NO OTHER rain
	// language must NOT fire condition P on a SEPARATE, still-dry,
	// qualifying material -- the vocabulary scan must exclude
	// param/def/expr/expression lines, or a document would certify its
	// OWN evidence forever after a single add_wetness call.
	{
		const std::string tmp = TempPath( "addwet_i5.RISEscene" );
		std::string body = Preamble();
		body += SphereGeo( "s1" );
		body += SphereGeo( "s2" );
		body += Lambertian( "mat_a", "pnt_stone" );
		body += Lambertian( "mat_b", "pnt_stone" );
		body += Obj( "o1", "s1", "mat_a", -2 );
		body += Obj( "o2", "s2", "mat_b", 2 );
		Job* pJob = LoadScene( body, tmp );
		Check( pJob != nullptr, "I5: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Check( hasCode( Agent::AgentSession::ValidateText( sess->ReadDocument() ), kCode ) == nullptr,
			       "I5: no rain language at all yet -- silent before the call" );
			const Agent::AgentSession::AgentAddWetnessResult r = sess->AddWetness( "mat_a" );
			Check( r.ok && r.applied, "I5: mat_a was made wet (now carries `def wet`/`def damp` text)" );
			const std::vector<Agent::AgentDiagnostic> diags =
				Agent::AgentSession::ValidateText( sess->ReadDocument() );
			Check( hasCode( diags, kCode ) == nullptr,
			       "I5 MONEY: mat_b is STILL a dry qualifying candidate (wetCandidateCount==1, >= gate), "
			       "but condition P does NOT fire -- the only 'wet'/'damp'/'dryness' text in the document "
			       "lives inside excluded param/def/expr/expression lines, never in a chunk name or "
			       "comment, so the document does not certify its own evidence" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}
}

int main()
{
	std::printf( "=== AgentAddWetnessTest ===\n" );
	TestLambertianRewrite();
	TestSignalDrivesIt();
	TestGgxInPlace();
	TestPbrInPlace();
	TestOrenNayarDampOnly();
	TestMetallicCoatOnly();
	TestRefusals();
	TestNameCollisionMinting();
	TestObjectGeometryNameCollision();
	TestCsgObjectMaterialRebind();
	TestWireSurface();
	TestDesignNote();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
