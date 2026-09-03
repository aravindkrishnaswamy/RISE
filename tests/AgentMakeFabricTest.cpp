//////////////////////////////////////////////////////////////////////
//
//  AgentMakeFabricTest.cpp - docs/CLOTH_FABRIC_DESIGN.md 9.7
//    (2026-09-02): make_fabric, the verb that converts one material
//    into cloth.
//
//  WHAT THIS VERB HAS TO GET RIGHT, and therefore what is measured
//  here.  Its contract is NOT add_wetness's ("wrap what is there, edit
//  nothing"): 9.3's whole argument is that a pure wrap is INSUFFICIENT,
//  because `fabric_material` holds a REFERENCE to a base it can neither
//  retype nor re-parameterise -- so `fabric satin` over a Lambertian is
//  chalk with a faint sheen, and a verb that shipped that would be
//  WORSE than no verb, because it would look like it worked.  So this
//  verb MINTS a substrate, and the assertions below are aimed at the
//  three ways minting can silently fail:
//
//    (a) THE SILENT BLACK SATIN.  `ggx_material.fresnel_mode` defaults
//        to `conductor` and `rs` ("Specular reflectance / F0") is a
//        REQUIRED colour-painter reference resolved BY NAME.  A minted
//        base carrying only rd/alphax/alphay therefore gets `rs`
//        unset -- the built-in `none` painter, which is BLACK -- under
//        conductor Fresnel: no dielectric specular at all, i.e. NO
//        HIGHLIGHT, which is the entire reason GGX was chosen for
//        denim/silk/satin.  Case A asserts all three of the required
//        fields, out of the DOCUMENT text, and that the fourth minted
//        chunk (the 0.04 F0 painter) is what `rs` names.
//    (b) THE DROPPED DYE.  The point of re-homing rather than
//        re-authoring is that the author's colour painter -- a texture,
//        an expression graph, anything -- survives the conversion
//        untouched.  Case A asserts the minted base's colour slot names
//        the ORIGINAL painter, and case D asserts the three-name lookup
//        (`reflectance` -> `base_color` -> `rd`) finds it on each of the
//        three predecessor classes.
//    (c) THE HALF-APPLIED CONVERSION.  Four minted chunks plus N object
//        rebinds must be ONE undo step and ONE head bump, and every
//        refusal must leave the document BYTE-IDENTICAL.  Cases F and G.
//
//  Cases:
//    A  MINT: a Lambertian + `fabric satin` mints FOUR chunks
//       (_fabric_f0, _fabric_base, _fabric_weave, _fabric), binds them
//       to each other correctly, re-homes `reflectance`, rebinds every
//       bound object, leaves the ORIGINAL chunk byte-identical, and the
//       re-derived document validates with no error/warning diagnostic
//       and renders.
//    B  MINT, the two-chunk shape: a Lambertian + `fabric wool` mints
//       exactly the base and the wrapper -- no F0 painter (an
//       Oren-Nayar substrate needs none) and no weave rotation (wool is
//       isotropic).
//    C  PURE WRAP: a ggx base + `fabric silk` mints NO substrate,
//       reports substrateWasReused, and does NOT claim the original is
//       unreferenced -- it IS the substrate now.
//    D  THE THREE-NAME COLOUR LOOKUP, across lambertian / pbr / ggx.
//    E  INFERENCE from the object's own name: `denim_jacket` -> denim;
//       `cushion` -> cotton, and the message SAYS it guessed.
//    F  THE FIVE REFUSALS, each with the document BYTE-IDENTICAL after.
//    G  ONE headVersion bump, ONE undo step through a live controller.
//    H  SELECTION + DETERMINISM: the bare call takes the most-bound
//       material; two runs from the same document are byte-identical.
//    I  WIRE SURFACE: declared in the shared chat-codec tool table, and
//       -- the 1ed4e7c3 two-list-drift lesson -- ADVERTISED and
//       ROUTABLE on MCP; refused under Read, and refused under Propose
//       with the verb's OWN posture-specific message.
//    J  PRESET PARITY: the verb's value list IS FabricPresetTable()'s
//       rows in order, and the chat codec's hand-authored JSON literal
//       carries the same seven in the same order.
//    K  `originalNowUnreferenced` is a TRUTH, not "did we mint?": a
//       `composite_material` reaching the original through `top` or
//       `bottom` still counts, the removal advice is withheld, and the
//       document still derives.
//    L  The message NAMES the slots the mint did not carry across.
//
//  Self-contained: no RISE_MEDIA_PATH, inline native-v7 scenes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Materials/FabricPresets.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentDiagnostic.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/SceneEditor/SceneEditController.h"

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
// A NO-OP-render SceneEditController, so case G can drive the REAL
// Undo() the GUI's Cmd-Z drives.
//----------------------------------------------------------------------
class QuietController : public SceneEditController
{
public:
	explicit QuietController( IJobPriv& job )
	: SceneEditController( job, /*interactiveRasterizer*/0 ) {}
protected:
	void DoOneRenderPass() override {}
};

//! Split one chunk body into its `param value` lines.  Deliberately
//! whitespace-agnostic (the fixtures below author `name o1` with a
//! space; the verb emits `name\t\t\to1` with tabs, and the CST is
//! lossless, so BOTH spellings survive into the same document) -- these
//! assertions are about the BINDING, never about tabs.
static bool LineTokens( const std::string& line, std::string& key, std::string& value )
{
	const std::size_t ks = line.find_first_not_of( "\t " );
	if( ks == std::string::npos ) return false;
	const std::size_t ke = line.find_first_of( "\t ", ks );
	if( ke == std::string::npos ) { key = line.substr( ks ); value.clear(); return true; }
	key = line.substr( ks, ke - ks );
	const std::size_t vs = line.find_first_not_of( "\t ", ke );
	if( vs == std::string::npos ) { value.clear(); return true; }
	std::size_t ve = line.find_last_not_of( "\t \r" );
	value = line.substr( vs, ve + 1 - vs );
	return true;
}

//! Every `param value` pair of the chunk of kind `kind` whose `name`
//! line reads `name`.  Used to prove the ORIGINAL material chunk came
//! through the swap intact and that every minted chunk really binds what
//! the result struct claims.
static std::vector<std::pair<std::string, std::string> > ChunkParams(
	const std::string& doc, const std::string& kind, const std::string& name )
{
	std::vector<std::pair<std::string, std::string> > out;
	std::size_t at = 0;
	for( ;; ) {
		at = doc.find( kind, at );
		if( at == std::string::npos ) return out;
		// The keyword must start a line and be followed by the block.
		const bool lineStart = ( at == 0 || doc[at-1] == '\n' );
		const std::size_t close = doc.find( "\n}", at );
		if( close == std::string::npos ) return out;
		if( lineStart ) {
			std::vector<std::pair<std::string, std::string> > pairs;
			bool named = false;
			const std::string body = doc.substr( at, close + 1 - at );
			std::size_t ls = 0;
			while( ls < body.size() ) {
				const std::size_t le = body.find( '\n', ls );
				const std::string line = body.substr( ls, ( le == std::string::npos ? body.size() : le ) - ls );
				std::string k, v;
				if( LineTokens( line, k, v ) && k != kind && k != "{" && k != "}" && k[0] != '#' ) {
					if( k == "name" && v == name ) named = true;
					pairs.push_back( std::make_pair( k, v ) );
				}
				if( le == std::string::npos ) break;
				ls = le + 1;
			}
			if( named ) return pairs;
		}
		at += kind.size();
	}
}

//! Is `param value` bound on the chunk named `name` of kind `kind`?
static bool ChunkBinds( const std::string& doc, const std::string& kind,
                        const std::string& name, const std::string& param,
                        const std::string& value )
{
	const std::vector<std::pair<std::string, std::string> > ps = ChunkParams( doc, kind, name );
	for( std::size_t i = 0; i < ps.size(); ++i )
		if( ps[i].first == param && ps[i].second == value ) return true;
	return false;
}

//! Does the chunk named `name` of kind `kind` carry `param` at all?
static bool ChunkHasParam( const std::string& doc, const std::string& kind,
                           const std::string& name, const std::string& param )
{
	const std::vector<std::pair<std::string, std::string> > ps = ChunkParams( doc, kind, name );
	for( std::size_t i = 0; i < ps.size(); ++i )
		if( ps[i].first == param ) return true;
	return false;
}

static bool ValidatesClean( const std::string& doc, std::string& firstOffender )
{
	// The severity-aware oracle (commit f2ef553a's rationale):
	// ValidateText legitimately appends Info-severity DESIGN_* advisories,
	// so "clean" means NO diagnostic whose severity is other than Info --
	// not "no diagnostics".
	const std::vector<Agent::AgentDiagnostic> diags = Agent::AgentSession::ValidateText( doc );
	for( const Agent::AgentDiagnostic& d : diags ) {
		if( d.severity == Agent::AgentDiagnostic::Severity::Info ) continue;
		firstOffender = d.code + ": " + d.message;
		return false;
	}
	return true;
}

//----------------------------------------------------------------------
// Fixtures.
//----------------------------------------------------------------------
static std::string Preamble()
{
	return
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n"
		"pixelpel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n}\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname dye\n\tcolor 0.42 0.28 0.14\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname spec\n\tcolor 0.6 0.6 0.6\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.4 0.5 0.9\n}\n\n";
}

//! A curv-bearing geometry, so refusal 3's gate never fires by accident
//! in the positive cases.
static std::string Sphere( const std::string& name )
{
	return "sphere_geometry\n{\n\tname " + name + "\n\tradius 0.7\n}\n\n";
}

//! A curv-BARREN geometry -- CurvBarrenGeometryKind_'s own family.  The
//! normal field is constant across it, so `curv` is 0 everywhere and a
//! grazing-halo preset has no silhouette to appear on.
static std::string Plane( const std::string& name )
{
	return "infiniteplane_geometry\n{\n\tname " + name + "\n\txtile 1.0\n\tytile 1.0\n}\n\n";
}

static std::string Obj( const std::string& name, const std::string& geo,
                        const std::string& mat, double x )
{
	char buf[64];
	std::snprintf( buf, sizeof( buf ), "%g", x );
	return "standard_object\n{\n\tname " + name + "\n\tgeometry " + geo + "\n\tmaterial " + mat +
	       "\n\tposition " + buf + " 0 0\n}\n\n";
}

static const char* const kLambChunk =
	"lambertian_material\n{\n\tname cloth\n\treflectance dye\n}\n\n";

//! ONE Lambertian bound to two spheres -- the canonical mint case.
static std::string SceneLambertian()
{
	std::string s = Preamble();
	s += Sphere( "sph" );
	s += kLambChunk;
	s += Obj( "o1", "sph", "cloth", -1 );
	s += Obj( "o2", "sph", "cloth",  1 );
	return s;
}

//======================================================================
static void TestMintFourChunks()
{
	std::printf( "A: MINT -- a Lambertian + `fabric satin` mints three chunks and wires them\n" );
	const std::string tmp = TempPath( "makefabric_a.RISEscene" );
	Job* pJob = LoadScene( SceneLambertian(), tmp );
	Check( pJob != nullptr, "A: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( std::string(), "satin" );
	Check( r.ok && r.applied, std::string( "A: applied -- " ) + r.message );
	Check( r.status == "applied", "A: status is \"applied\"" );
	Check( r.material == "cloth", "A: it converted `cloth`" );
	Check( r.materialKind == "lambertian_material",
	       "A: materialKind still reports the ORIGINAL kind -- this verb never retypes a chunk" );
	Check( r.fabricPreset == "satin", "A: the explicit `fabric` argument won" );
	Check( r.qualifyingMaterials == 1, "A: one material qualified" );
	Check( r.boundObjects == 2, "A: it reports the blast radius (2 bound objects)" );

	Check( r.fabricMaterial   == "cloth_fabric",       "A: the wrapper is `cloth_fabric`" );
	Check( r.mintedSubstrate  == "cloth_fabric_base",  "A: the substrate is `cloth_fabric_base`" );
	Check( r.mintedSubstrateKind == "weave_material",
	       "A MONEY: satin's recommended substrate is a weave_material since Phase 2, and a "
	       "Lambertian is not one, so one was MINTED -- 9.3's whole point is that the preset "
	       "cannot retype the base it merely references" );
	Check( r.baseMaterial     == "cloth_fabric_base",  "A: the wrapper's `base` names the minted substrate" );
	Check( r.weavePainter     == "cloth_fabric_weave", "A: a weave painter was minted (satin is directional)" );
	Check( r.rotationPainter  == r.weavePainter,
	       "A: rotationPainter reports what `weave_rotation` actually binds, which IS the minted "
	       "painter here" );
	Check( !r.substrateWasReused, "A: substrateWasReused is FALSE on the mint path" );
	Check( r.originalNowUnreferenced,
	       "A MONEY: the original chunk is reported as UNREFERENCED -- a different and more "
	       "surprising outcome than add_wetness's wrap, which leaves the original as the substrate" );
	Check( r.rebindObjectCount == 2, "A: both bound objects moved to the wrapper" );

	const std::string doc = sess->ReadDocument();
	Check( doc != docBefore, "A: the document really changed" );

	// -- The minted chunks, read back out of the DOCUMENT.
	//
	// PHASE 2 CHANGED THE SHAPE HERE FROM FOUR CHUNKS TO THREE, and the
	// chunk that went away is the point.  The Phase-1 mint was an
	// anisotropic `ggx_material` plus a 0.04 dielectric-F0
	// `uniformcolor_painter` its `rs` had to name; 9.9 gate 9b measured
	// that composition and found it reads as BRUSHED METAL (95-99 % of
	// the substrate's anisotropy survives the sheen, and there is still
	// no pattern scale).  A `weave_material` carries its own fibre model
	// and its own Fresnel, so there is no `rs` to name and no F0 painter
	// to mint.
	Check( doc.find( "cloth_fabric_f0" ) == std::string::npos,
	       "A MONEY: NO F0 painter is minted any more -- it existed only to feed a ggx substrate's "
	       "`rs`, which resolves BY NAME and would otherwise bind the built-in BLACK `none` "
	       "painter.  A weave_material has no such slot" );

	Check( ChunkBinds( doc, "weave_material", "cloth_fabric_base", "fabric", "satin" ),
	       "A MONEY: the minted substrate names satin's WEAVE preset, which is what carries the "
	       "5-harness float draft, the 2.5-degree flat warp and the opposite float tilts -- the "
	       "things that are actually satin" );
	Check( ChunkBinds( doc, "weave_material", "cloth_fabric_base", "warp_color", "dye" ),
	       "A MONEY: the ORIGINAL colour painter was RE-HOMED onto the minted substrate's WARP -- "
	       "the author's dye, texture or expression graph survives the conversion untouched.  The "
	       "warp specifically, because satin_5 puts it on top 4/5 of the time, so the author's "
	       "tone stays dominant while the preset's own WEFT colour survives to carry the two-tone" );
	Check( !ChunkHasParam( doc, "weave_material", "cloth_fabric_base", "weft_color" ),
	       "A MONEY: ...and `weft_color` is DELIBERATELY unwritten, so the chunk seeds it from the "
	       "same preset table -- overwriting both families with one painter would flatten exactly "
	       "the two-tone the weave was minted for" );
	Check( !ChunkHasParam( doc, "weave_material", "cloth_fabric_base", "warp_width" ) &&
	       !ChunkHasParam( doc, "weave_material", "cloth_fabric_base", "weave" ),
	       "A: every other slot is left to the chunk's own preset seeding -- writing the numbers "
	       "into the document would freeze them against a later retune" );

	Check( ChunkBinds( doc, "scalar_painter", "cloth_fabric_weave", "value", "0.0" ),
	       "A: the weave painter is a CONSTANT 0 -- a bit-exact no-op rotation; the point is that "
	       "the slot is wired for the author to rebind" );
	Check( ChunkBinds( doc, "weave_material", "cloth_fabric_base", "weave_rotation", "cloth_fabric_weave" ),
	       "A MONEY: the rotation is bound on the WEAVE, not on the fabric wrapper -- the weave is "
	       "the thing with a grain, and writing it in both places would ADD the two rotations and "
	       "turn the yarn twice" );
	Check( !ChunkHasParam( doc, "fabric_material", "cloth_fabric", "weave_rotation" ),
	       "A: ...and the wrapper leaves its own `weave_rotation` unwritten, taking the 0.0 default" );

	Check( ChunkBinds( doc, "fabric_material", "cloth_fabric", "fabric", "satin" ),
	       "A: the wrapper names the preset" );
	Check( ChunkBinds( doc, "fabric_material", "cloth_fabric", "base", "cloth_fabric_base" ),
	       "A: ...binds the minted substrate as `base`" );
	Check( !ChunkHasParam( doc, "fabric_material", "cloth_fabric", "sheen_roughness" ) &&
	       !ChunkHasParam( doc, "fabric_material", "cloth_fabric", "sheen_color" ),
	       "A MONEY: `sheen_color` and `sheen_roughness` are DELIBERATELY unwritten, so the chunk "
	       "seeds them from the same preset table this verb read -- writing them would freeze the "
	       "numbers into the document against a later retune" );

	// -- The original chunk, byte-identical.
	Check( doc.find( "lambertian_material\n{\n\tname cloth\n\treflectance dye\n}" ) != std::string::npos,
	       "A MONEY: the ORIGINAL `cloth` chunk came through the swap BYTE-IDENTICAL -- this verb "
	       "never edits it, on either path" );

	// -- Declare-before-use, which for `base` and `rs` is not a style
	//    choice: both resolve out of already-registered managers.
	{
		const std::size_t weave = doc.find( "name\t\t\tcloth_fabric_weave" );
		const std::size_t base  = doc.find( "name\t\t\tcloth_fabric_base" );
		const std::size_t wrap  = doc.find( "name\t\t\tcloth_fabric\n" );
		Check( weave != std::string::npos && base != std::string::npos && wrap != std::string::npos &&
		       weave < base && base < wrap,
		       "A MONEY: the emitted order is weave painter, then substrate, then wrapper -- a "
		       "forward reference does not merely read badly here, it FAILS TO PARSE, because "
		       "`base` and `weave_rotation` both resolve out of already-registered managers" );
	}

	// -- The rebinds.
	Check( ChunkBinds( doc, "standard_object", "o1", "material", "cloth_fabric" ) &&
	       ChunkBinds( doc, "standard_object", "o2", "material", "cloth_fabric" ),
	       "A MONEY: BOTH bound objects' `material` reference moved to the wrapper, read back out "
	       "of the document -- a mint that left an object on the original would render the "
	       "unconverted material" );

	// -- It really derives, and really renders.
	{
		std::string offender;
		Check( ValidatesClean( doc, offender ),
		       "A MONEY: the re-derived document carries NO error/warning diagnostic -- in "
		       "particular no `fabric_material ... expects a weave_material substrate` mismatch "
		       "warning, which is exactly the diagnostic this verb exists to make unnecessary"
		       + ( offender.empty() ? std::string() : ( " [" + offender + "]" ) ) );
	}
	{
		Agent::AgentRenderParams rp;
		rp.width = 24; rp.height = 24; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "A: the converted scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "A: ...and is non-black" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestMintTwoChunks()
{
	std::printf( "B: MINT, two-chunk shape -- `fabric wool` needs no F0 and no weave\n" );
	std::string scene = Preamble();
	scene += Sphere( "sph" );
	// A LAMBERTIAN predecessor on purpose: wool's recommended substrate is
	// an orennayar_material, so an Oren-Nayar predecessor would take the
	// PURE-WRAP path (case C's shape) and this case would prove nothing
	// about minting.
	scene += "lambertian_material\n{\n\tname felt\n\treflectance dye\n}\n\n";
	scene += Obj( "o1", "sph", "felt", 0 );

	const std::string tmp = TempPath( "makefabric_b.RISEscene" );
	Job* pJob = LoadScene( scene, tmp );
	Check( pJob != nullptr, "B: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "felt", "wool" );
	Check( r.ok && r.applied, std::string( "B: applied -- " ) + r.message );
	Check( r.fabricPreset == "wool", "B: the preset is wool" );
	Check( r.mintedSubstrateKind == "orennayar_material",
	       "B: wool's recommended substrate is an orennayar_material" );
	Check( r.mintedSubstrate == "felt_fabric_base", "B: it minted the base" );
	Check( r.weavePainter.empty() && r.rotationPainter.empty(),
	       "B MONEY: NO weave painter -- wool is isotropic, and a preset that minted a rotation it "
	       "does not use would be authoring a knob with nothing behind it" );

	const std::string doc = sess->ReadDocument();
	Check( doc.find( "felt_fabric_f0" ) == std::string::npos,
	       "B MONEY: NO F0 painter -- an orennayar_material's whole surface is `reflectance` plus "
	       "`roughness`; the F0 chunk exists only to satisfy ggx's by-name `rs`" );
	Check( doc.find( "felt_fabric_weave" ) == std::string::npos, "B: ...and no weave chunk" );
	Check( !ChunkHasParam( doc, "fabric_material", "felt_fabric", "weave_rotation" ),
	       "B: the wrapper leaves `weave_rotation` unwritten, taking the chunk's own 0.0 default" );
	Check( ChunkBinds( doc, "orennayar_material", "felt_fabric_base", "reflectance", "dye" ),
	       "B: the colour painter was re-homed" );
	Check( ChunkBinds( doc, "orennayar_material", "felt_fabric_base", "roughness", "0.6" ),
	       "B MONEY: the minted base carries WOOL's calibrated sigma (0.6) straight off the one preset "
	       "table -- a Lambertian predecessor has no roughness of its own, so this number can only "
	       "have come from the preset" );
	Check( ChunkBinds( doc, "standard_object", "o1", "material", "felt_fabric" ),
	       "B: the bound object moved to the wrapper" );
	{
		std::string offender;
		Check( ValidatesClean( doc, offender ),
		       "B: the re-derived document carries no error/warning diagnostic"
		       + ( offender.empty() ? std::string() : ( " [" + offender + "]" ) ) );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestPureWrap()
{
	std::printf( "C: PURE WRAP -- an orennayar base + `fabric wool` mints no substrate\n" );
	// PHASE 2 MOVED THIS CASE.  It used to be a ggx base under `silk`,
	// which was the pure-wrap path while silk recommended a GGX
	// substrate; silk now recommends a `weave_material`, so that same
	// pair MINTS.  The pure-wrap path itself is unchanged and still
	// needs a positive case, so the fixture moved to a preset whose
	// recommendation Phase 2 did not touch -- and the silk-over-ggx pair
	// is kept below as the MINT case it has become, because a change
	// that quietly stopped minting there would otherwise go unnoticed.
	std::string scene = Preamble();
	scene += Sphere( "sph" );
	scene += "orennayar_material\n{\n\tname shot\n\treflectance dye\n\troughness 0.25\n}\n\n";
	scene += Obj( "o1", "sph", "shot", 0 );

	const std::string tmp = TempPath( "makefabric_c.RISEscene" );
	Job* pJob = LoadScene( scene, tmp );
	Check( pJob != nullptr, "C: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "shot", "wool" );
	Check( r.ok && r.applied, std::string( "C: applied -- " ) + r.message );
	Check( r.substrateWasReused,
	       "C MONEY: the bound base ALREADY matched wool's recommended class, so nothing was minted "
	       "-- 9.7's pure-wrap path, which is add_wetness's whole shape and this verb's exception" );
	Check( r.mintedSubstrate.empty() && r.mintedSubstrateKind.empty(),
	       "C: no substrate is reported, because none was minted" );
	Check( r.baseMaterial == "shot",
	       "C: the wrapper's `base` names the ORIGINAL material, which is now the substrate" );
	Check( !r.originalNowUnreferenced,
	       "C MONEY: the original is NOT reported unreferenced on this path -- it is the substrate, "
	       "and telling the author otherwise would invite them to delete a live chunk" );
	Check( r.weavePainter.empty(),
	       "C: and NO weave painter is minted -- wool is isotropic, so there is no yarn direction "
	       "for the slot to steer" );

	const std::string doc = sess->ReadDocument();
	Check( doc.find( "shot_fabric_base" ) == std::string::npos, "C: no substrate chunk landed" );
	Check( doc.find( "shot_fabric_f0" )   == std::string::npos, "C: and no F0 painter" );
	Check( ChunkBinds( doc, "orennayar_material", "shot", "roughness", "0.25" ),
	       "C MONEY: the reused substrate's OWN roughness was NOT retuned to wool's 0.6 -- this "
	       "verb never edits the original chunk, on either path, and the message says so" );
	Check( ChunkBinds( doc, "standard_object", "o1", "material", "shot_fabric" ),
	       "C: the bound object moved to the wrapper" );
	{
		std::string offender;
		Check( ValidatesClean( doc, offender ),
		       "C: the re-derived document carries no error/warning diagnostic"
		       + ( offender.empty() ? std::string() : ( " [" + offender + "]" ) ) );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestGgxUnderSilkNowMints()
{
	std::printf( "C2: a ggx base + `fabric silk` MINTS a weave, because Phase 2 moved the "
	             "recommendation\n" );
	// The pair this case is built from -- an anisotropic ggx_material
	// under `silk` -- was Phase 1's pure-wrap path, and it is exactly the
	// composition 9.9 gate 9b measured and found wanting: 95 % of the
	// substrate's anisotropy survives the sheen and the frame still reads
	// as brushed metal, because one elliptical lobe has no pattern scale.
	// So the verb must now MINT rather than reuse, and it must do so
	// without touching the original chunk.
	std::string scene = Preamble();
	scene += Sphere( "sph" );
	scene += "ggx_material\n{\n\tname shot\n\trd dye\n\trs spec\n\talphax 0.3\n\talphay 0.1\n"
	         "\tfresnel_mode schlick_f0\n}\n\n";
	scene += Obj( "o1", "sph", "shot", 0 );

	const std::string tmp = TempPath( "makefabric_c2.RISEscene" );
	Job* pJob = LoadScene( scene, tmp );
	Check( pJob != nullptr, "C2: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "shot", "silk" );
	Check( r.ok && r.applied, std::string( "C2: applied -- " ) + r.message );
	Check( !r.substrateWasReused && r.mintedSubstrateKind == "weave_material",
	       "C2 MONEY: a ggx base under `silk` now MINTS a weave_material -- the Phase-1 pure-wrap "
	       "path for this pair is gone, because the composition it produced is the one gate 9b "
	       "measured as brushed metal" );

	const std::string doc = sess->ReadDocument();
	Check( ChunkBinds( doc, "weave_material", "shot_fabric_base", "fabric", "silk" ) &&
	       ChunkBinds( doc, "weave_material", "shot_fabric_base", "warp_color", "dye" ),
	       "C2: the minted weave names silk's preset and re-homes the original's `rd` painter" );
	Check( ChunkBinds( doc, "ggx_material", "shot", "alphax", "0.3" ) &&
	       ChunkBinds( doc, "ggx_material", "shot", "alphay", "0.1" ),
	       "C2 MONEY: the ORIGINAL ggx chunk is untouched -- this verb never edits it, and the "
	       "author still has their old material to fall back on" );
	{
		std::string offender;
		Check( ValidatesClean( doc, offender ),
		       "C2: the re-derived document carries no error/warning diagnostic"
		       + ( offender.empty() ? std::string() : ( " [" + offender + "]" ) ) );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestWeaveBaseReuses()
{
	std::printf( "C3: a `weave_material` base + `fabric satin` PURE-WRAPS -- REVIEW_P2R2.md P2\n" );
	// The predecessor to C2: an author has ALREADY bound a correct
	// `weave_material` (satin's own recommended class since Phase 2) as
	// the base.  `FabricSubstrateClassOfChunkKind_` must map
	// "weave_material" to `eFabricSubstrateWeave` -- without that row the
	// verb cannot tell this base apart from an unrelated one and would
	// MINT A SECOND, REDUNDANT weave_material on top of an already-
	// correct one, exactly the "minting when nothing was needed" defect
	// 9.7's pure-wrap path exists to avoid for lambertian/orennayar/ggx.
	std::string scene = Preamble();
	scene += Sphere( "sph" );
	scene += "weave_material\n{\n\tname wbase\n\tfabric satin\n\twarp_color dye\n}\n\n";
	scene += Obj( "o1", "sph", "wbase", 0 );

	const std::string tmp = TempPath( "makefabric_c3.RISEscene" );
	Job* pJob = LoadScene( scene, tmp );
	Check( pJob != nullptr, "C3: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "wbase", "satin" );
	Check( r.ok && r.applied, std::string( "C3: applied -- " ) + r.message );
	Check( r.substrateWasReused,
	       "C3 MONEY: the bound base is ALREADY a weave_material, satin's own recommended class, "
	       "so nothing is minted -- the pure-wrap path C/C2 established for lambertian/orennayar/"
	       "ggx also covers an already-correct weave base" );
	Check( r.mintedSubstrate.empty() && r.mintedSubstrateKind.empty(),
	       "C3: no substrate is reported minted" );
	Check( r.baseMaterial == "wbase",
	       "C3: the wrapper's `base` names the ORIGINAL weave_material, now the substrate" );
	Check( !r.originalNowUnreferenced,
	       "C3: the original weave_material is NOT reported unreferenced -- it IS the substrate" );

	const std::string doc = sess->ReadDocument();
	Check( doc.find( "wbase_fabric_base" ) == std::string::npos,
	       "C3: no second weave_material chunk landed" );
	Check( ChunkBinds( doc, "weave_material", "wbase", "fabric", "satin" ),
	       "C3: the ORIGINAL weave_material chunk is untouched" );
	{
		std::string offender;
		Check( ValidatesClean( doc, offender ),
		       "C3: the re-derived document carries no error/warning diagnostic"
		       + ( offender.empty() ? std::string() : ( " [" + offender + "]" ) ) );
	}
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestColourLookupOrder()
{
	std::printf( "D: the three-name colour lookup across lambertian / pbr / ggx predecessors\n" );

	struct Case_ { const char* label; const char* chunk; const char* mat; const char* preset;
	               const char* substrateKind; };
	// Each predecessor spells the albedo with a DIFFERENT name, and there
	// is no common accessor -- 9.7 step 0's whole reason for existing.
	const Case_ cases[] = {
		{ "lambertian/reflectance",
		  "lambertian_material\n{\n\tname m\n\treflectance dye\n}\n\n", "m", "wool", "orennayar_material" },
		{ "pbr/base_color",
		  "pbr_metallic_roughness_material\n{\n\tname m\n\tbase_color dye\n\tmetallic 0.0\n"
		  "\troughness 0.5\n}\n\n", "m", "wool", "orennayar_material" },
		{ "ggx/rd",
		  "ggx_material\n{\n\tname m\n\trd dye\n\trs spec\n\talphax 0.2\n\talphay 0.2\n"
		  "\tfresnel_mode schlick_f0\n}\n\n", "m", "wool", "orennayar_material" },
	};

	for( const Case_& c : cases ) {
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += c.chunk;
		scene += Obj( "o1", "sph", c.mat, 0 );
		const std::string tmp = TempPath( "makefabric_d.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, std::string( "D[" ) + c.label + "]: fixture derives" );
		if( !pJob ) continue;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( c.mat, c.preset );
		Check( r.ok && r.applied, std::string( "D[" ) + c.label + "]: applied -- " + r.message );
		Check( r.mintedSubstrateKind == c.substrateKind,
		       std::string( "D[" ) + c.label + "]: it minted the preset's class" );
		const std::string doc = sess->ReadDocument();
		Check( ChunkBinds( doc, c.substrateKind, "m_fabric_base", "reflectance", "dye" ),
		       std::string( "D[" ) + c.label + "] MONEY: the painter was found under THIS class's own "
		       "slot name and re-homed -- the lookup tries reflectance, then base_color, then rd, "
		       "because the four convertible kinds do not agree on a name and there is no accessor "
		       "that asks them all" );
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// A pbr predecessor under a DIRECTIONAL preset always MINTS.  Under
	// Phase 1 the reason was that pbr_metallic_roughness_material
	// resolves to a GGXMaterial at scene-build time but its CHUNK has one
	// isotropic `roughness` and no alphax/alphay to express the ratio
	// with.  Under Phase 2 the reason is simpler and stronger: the
	// recommended class is `weave_material`, which no predecessor is.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += "pbr_metallic_roughness_material\n{\n\tname m\n\tbase_color dye\n\tmetallic 0.0\n"
		         "\troughness 0.5\n}\n\n";
		scene += Obj( "o1", "sph", "m", 0 );
		const std::string tmp = TempPath( "makefabric_d2.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "D[pbr/silk]: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "m", "silk" );
			Check( r.ok && r.applied && !r.substrateWasReused &&
			       r.mintedSubstrateKind == "weave_material",
			       "D MONEY: a pbr predecessor under `silk` MINTS -- Phase 1 minted an anisotropic "
			       "ggx substrate here because the pbr CHUNK has no alphax/alphay to express the "
			       "ratio with; Phase 2 mints a weave_material, which has the whole draft" );
			const std::string doc = sess->ReadDocument();
			Check( ChunkBinds( doc, "weave_material", "m_fabric_base", "warp_color", "dye" ),
			       "D: ...with `base_color`'s painter re-homed onto the minted weave's WARP dye" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

//======================================================================
static void TestInference()
{
	std::printf( "E: preset inference from the object's own name\n" );

	// `denim_jacket` names a fabric unambiguously.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += kLambChunk;
		scene += Obj( "denim_jacket", "sph", "cloth", 0 );
		const std::string tmp = TempPath( "makefabric_e1.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "E1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric();
			Check( r.ok && r.applied, std::string( "E1: applied -- " ) + r.message );
			Check( r.fabricPreset == "denim",
			       "E1 MONEY: `denim_jacket` -> denim, with NO argument at all -- the no-argument call "
			       "is the intended one, and the author's own naming is the cheapest signal there is" );
			Check( r.mintedSubstrateKind == "weave_material",
			       "E1: ...and the inferred preset drives the SUBSTRATE too, not just the sheen -- "
			       "`denim_jacket` mints a weave carrying the 3/1 twill draft that IS the wale" );
			Check( r.message.find( "INFERRED" ) != std::string::npos,
			       "E1: the message says it inferred rather than being told" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// `cushion` names no fabric -- velvet is NOT inferable from it.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += kLambChunk;
		scene += Obj( "cushion", "sph", "cloth", 0 );
		const std::string tmp = TempPath( "makefabric_e2.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "E2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric();
			Check( r.ok && r.applied, std::string( "E2: applied -- " ) + r.message );
			Check( r.fabricPreset == "cotton",
			       "E2 MONEY: `cushion` names no fabric, so this falls back to cotton rather than "
			       "guessing velvet -- 9.7 names this exact pair as the inferable/not-inferable line" );
			Check( r.message.find( "DEFAULTED to `cotton`" ) != std::string::npos,
			       "E2 MONEY: ...and the message SAYS it defaulted, naming the alternatives -- a guess "
			       "the author cannot see is a guess they cannot correct" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// TWO different fabrics named across the bound objects is ambiguity,
	// not a first-wins race.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += kLambChunk;
		scene += Obj( "silk_scarf",  "sph", "cloth", -1 );
		scene += Obj( "wool_throw",  "sph", "cloth",  1 );
		const std::string tmp = TempPath( "makefabric_e3.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "E3: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric();
			Check( r.ok && r.applied && r.fabricPreset == "cotton",
			       "E3 MONEY: two objects naming DIFFERENT fabrics is ambiguous, so the inference "
			       "declines and falls back -- taking the first would silently make one of the two "
			       "names a lie" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}
}

//======================================================================
static void TestRefusals()
{
	std::printf( "F: the five refusals, each leaving the document BYTE-IDENTICAL\n" );

	// -- Refusal 1: nothing qualifies.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += "dielectric_material\n{\n\tname glass\n\tior 1.5\n\ttau 1.0\n\tscattering 1000000\n}\n\n";
		scene += Obj( "o1", "sph", "glass", 0 );
		const std::string tmp = TempPath( "makefabric_f1.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "F1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric();
			Check( !r.ok && !r.applied && r.status.empty(),
			       "F1: refusal 1 -- ok=false with an EMPTY status, the pre-commit refusal shape" );
			Check( r.qualifyingMaterials == 0, "F1: nothing qualified" );
			Check( sess->ReadDocument() == before, "F1 MONEY: the document is BYTE-IDENTICAL" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- Refusal 2: already a fabric_material, in both its forms.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += kLambChunk;
		scene += "fabric_material\n{\n\tname already\n\tfabric cotton\n\tbase cloth\n}\n\n";
		scene += Obj( "o1", "sph", "already", 0 );
		const std::string tmp = TempPath( "makefabric_f2.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "F2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentMakeFabricResult r1 = sess->MakeFabric( "already", "silk" );
			Check( !r1.ok && !r1.applied,
			       "F2a: naming the fabric_material itself refuses" );
			Check( r1.message.find( "ALREADY a `fabric_material`" ) != std::string::npos,
			       "F2a: ...and the message says which rule it tripped" );
			const Agent::AgentSession::AgentMakeFabricResult r2 = sess->MakeFabric( "cloth", "silk" );
			Check( !r2.ok && !r2.applied,
			       "F2b MONEY: naming the SUBSTRATE of an existing fabric_material also refuses -- "
			       "wrapping it again would put a fabric inside a fabric and leave the outer one "
			       "pointing at a material nothing else reaches" );
			Check( r2.message.find( "already the SUBSTRATE" ) != std::string::npos,
			       "F2b: ...and names the wrapper to retune instead" );
			Check( sess->ReadDocument() == before, "F2 MONEY: the document is BYTE-IDENTICAL" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- Refusal 3: planar-only geometry under a grazing-halo preset.
	{
		std::string scene = Preamble();
		scene += Plane( "flat" );
		scene += kLambChunk;
		scene += "standard_object\n{\n\tname o1\n\tgeometry flat\n\tmaterial cloth\n}\n\n";
		const std::string tmp = TempPath( "makefabric_f3.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "F3: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( std::string(), "velvet" );
			Check( !r.ok && !r.applied,
			       "F3 MONEY: velvet on planar-only geometry REFUSES -- the Charlie lobe's mass sits "
			       "at grazing half-vectors, and on a constant normal field there is no silhouette "
			       "for the halo to appear on, so it would render a uniform faint lift over the "
			       "colour it started from" );
			Check( r.geometryUniform, "F3: ...and the result reports WHY (every bound object is planar)" );
			Check( sess->ReadDocument() == before, "F3: the document is BYTE-IDENTICAL" );

			// The SAME geometry under a broad preset is NOT refused: the
			// gate is a claim about the LOBE, not about the geometry.
			const Agent::AgentSession::AgentMakeFabricResult r2 =
				sess->MakeFabric( std::string(), "cotton" );
			Check( r2.ok && r2.applied,
			       "F3 MONEY: the SAME planar scene under `cotton` APPLIES -- refusal 3 is "
			       "preset-dependent, because a matte cloth reads perfectly well on a slab and a "
			       "blanket geometry ban would refuse most of the real uses of this verb" );
			Check( r2.message.find( "uniform lift" ) != std::string::npos,
			       "F3: ...and says the sheen will read uniform there, rather than pretending" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- Refusal 4: an unconvertible kind, and a material with no colour slot.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += "dielectric_material\n{\n\tname glass\n\tior 1.5\n\ttau 1.0\n\tscattering 1000000\n}\n\n";
		scene += "lambertian_material\n{\n\tname blank\n}\n\n";
		scene += kLambChunk;
		scene += Obj( "o1", "sph", "glass", -2 );
		scene += Obj( "o2", "sph", "blank",  0 );
		scene += Obj( "o3", "sph", "cloth",  2 );
		const std::string tmp = TempPath( "makefabric_f4.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "F4: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();

			const Agent::AgentSession::AgentMakeFabricResult r1 = sess->MakeFabric( "glass", "silk" );
			Check( !r1.ok && !r1.applied, "F4a: a dielectric refuses" );
			Check( r1.message.find( "dielectric" ) != std::string::npos &&
			       r1.message.find( "IOR-stack" ) != std::string::npos,
			       "F4a MONEY: the refusal NAMES what could not be carried across -- minting a "
			       "substrate does not make this refusal obsolete, it sharpens it" );
			Check( r1.message.find( "lambertian_material, orennayar_material, ggx_material, "
			                        "pbr_metallic_roughness_material" ) != std::string::npos,
			       "F4a MONEY: ...and NAMES THE ALLOWLIST, read off FabricMaterial's own "
			       "SubstrateAllowlistText so the message cannot drift from what the engine enforces" );

			const Agent::AgentSession::AgentMakeFabricResult r2 = sess->MakeFabric( "blank", "silk" );
			Check( !r2.ok && !r2.applied,
			       "F4b MONEY: a convertible KIND carrying none of reflectance/base_color/rd still "
			       "refuses -- a fabric whose dye was silently dropped is worse than no fabric" );
			Check( r2.message.find( "no colour painter to re-home" ) != std::string::npos,
			       "F4b: ...and says so" );

			Check( sess->ReadDocument() == before, "F4: the document is BYTE-IDENTICAL after both" );

			// An unknown preset spelling is refused rather than silently
			// resolving to `custom` -- the reason the argument is an enum.
			const Agent::AgentSession::AgentMakeFabricResult r3 =
				sess->MakeFabric( "cloth", "crushed burgundy velour" );
			Check( !r3.ok && !r3.applied,
			       "F4c MONEY: an unknown `fabric` spelling REFUSES rather than falling back to "
			       "`custom` -- LookupFabricPreset's fallback is a defence for API callers, and "
			       "letting it answer here would produce a fabric with none of the look that was "
			       "asked for" );
			Check( r3.message.find( "cotton, denim, silk, satin, velvet, wool, linen" ) != std::string::npos,
			       "F4c: ...and lists the values that DO exist, in FabricPresetTable()'s own order" );
			Check( sess->ReadDocument() == before, "F4c: the document is BYTE-IDENTICAL" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- Refusal 5: collision with an add_wetness coat on the same material.
	{
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += kLambChunk;
		scene += "scalar_painter\n{\n\tname cw\n\tvalue 0.6\n}\n\n";
		scene += "coated_material\n{\n\tname cloth_wetcoat\n\tbase cloth\n\tcoat_weight cw\n}\n\n";
		scene += Obj( "o1", "sph", "cloth_wetcoat", 0 );
		const std::string tmp = TempPath( "makefabric_f5.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "F5: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "cloth", "velvet" );
			Check( !r.ok && !r.applied,
			       "F5 MONEY: a material already wrapped by an add_wetness coat REFUSES -- wet fabric "
			       "is a legitimate and commonly wanted composition, but it needs coated_material to "
			       "accept a fabric_material substrate, which is a separate slice" );
			Check( r.message.find( "coated_material" ) != std::string::npos,
			       "F5: ...and names the wrapper that blocked it" );
			const Agent::AgentSession::AgentMakeFabricResult r2 = sess->MakeFabric( "cloth_wetcoat", "velvet" );
			Check( !r2.ok && !r2.applied,
			       "F5b: naming the COAT itself refuses too (an existing layered stack)" );
			Check( sess->ReadDocument() == before, "F5 MONEY: the document is BYTE-IDENTICAL" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}

	// -- An unknown name, and a name that exists but is not a material.
	{
		const std::string tmp = TempPath( "makefabric_f6.RISEscene" );
		Job* pJob = LoadScene( SceneLambertian(), tmp );
		Check( pJob != nullptr, "F6: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentMakeFabricResult r1 = sess->MakeFabric( "nope", "silk" );
			Check( !r1.ok && r1.message.find( "no chunk named" ) != std::string::npos,
			       "F6a: an unknown name says so, distinctly from a rule it tripped" );
			const Agent::AgentSession::AgentMakeFabricResult r2 = sess->MakeFabric( "sph", "silk" );
			Check( !r2.ok && r2.message.find( "exists but is not a material chunk" ) != std::string::npos,
			       "F6b: a name that EXISTS but is a geometry says THAT instead" );
			Check( sess->ReadDocument() == before, "F6: the document is BYTE-IDENTICAL" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}
}

//======================================================================
static void TestUndo()
{
	std::printf( "G: ONE headVersion bump, ONE undo step\n" );
	const std::string tmp = TempPath( "makefabric_g.RISEscene" );
	Job* pJob = LoadScene( SceneLambertian(), tmp );
	Check( pJob != nullptr, "G: fixture derives" );
	if( !pJob ) return;

	{
		QuietController c( *pJob );
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &c );

		const std::string before = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion hvBefore = sess->ReadDocumentSnapshot().headVersion;

		const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( std::string(), "satin" );
		Check( r.ok && r.applied, std::string( "G: applied through the controller -- " ) + r.message );
		const std::string after = sess->ReadDocument();
		Check( after != before, "G: the controller-attached commit really changed the document" );

		const RISE::Cst::CstHeadVersion hvAfter = sess->ReadDocumentSnapshot().headVersion;
		Check( hvAfter.revision == hvBefore.revision + 1,
		       "G MONEY: EXACTLY ONE headVersion bump for four minted chunks and two object rebinds "
		       "-- it is one composite document swap, not six edits" );

		c.Undo();
		Check( sess->ReadDocument() == before,
		       "G MONEY: ONE Undo() restores the pre-verb document BYTE-EXACTLY -- so undo can never "
		       "strand an object pointing at a wrapper that is gone, or a wrapper at a substrate "
		       "that is gone" );
		c.Redo();
		Check( sess->ReadDocument() == after, "G: Redo() reinstalls the post-verb document" );

		sess->AttachController( nullptr );
		sess.reset();
	}
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestSelectionAndDeterminism()
{
	std::printf( "H: the bare call takes the most-bound material; two runs are byte-identical\n" );

	std::string scene = Preamble();
	scene += Sphere( "sph" );
	scene += "lambertian_material\n{\n\tname mat_a\n\treflectance dye\n}\n\n";
	scene += "lambertian_material\n{\n\tname mat_b\n\treflectance dye\n}\n\n";
	scene += Obj( "o1", "sph", "mat_a", -2 );
	scene += Obj( "o2", "sph", "mat_b", -1 );
	scene += Obj( "o3", "sph", "mat_b",  0 );
	scene += Obj( "o4", "sph", "mat_b",  1 );

	std::string first;
	for( int pass = 0; pass < 2; ++pass ) {
		const std::string tmp = TempPath( "makefabric_h.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "H: fixture derives" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric();
		Check( r.ok && r.applied, std::string( "H: applied -- " ) + r.message );
		if( pass == 0 ) {
			Check( r.material == "mat_b",
			       "H MONEY: the bare call took `mat_b` -- bound to THREE objects, not `mat_a`, which "
			       "is FIRST in the document (one object). The rule is the identical "
			       "most-objects-then-lexicographic rule add_wear and add_wetness use" );
			Check( r.qualifyingMaterials == 2, "H: both flat materials qualified" );
			Check( r.rebindObjectCount == 3, "H: all three of its objects moved to the wrapper" );
			first = sess->ReadDocument();
		}
		else {
			Check( sess->ReadDocument() == first,
			       "H MONEY: two runs from the SAME input document produce BYTE-IDENTICAL output -- "
			       "no clock, no PRNG state, nothing to make a re-run of a scene diff noisily" );
		}
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//======================================================================
static void TestWireSurface()
{
	std::printf( "I: wire surface -- chat-codec table, MCP advertised AND routable, Read refuses\n" );

	{
		const std::string defs = Agent::ChatToolDefsFingerprint();
		Check( defs.find( "make_fabric" ) != std::string::npos,
		       "I: the verb is declared in the shared kToolDefs table (so every provider codec "
		       "carries it -- one table, four formatters)" );
	}

	const std::string tmp = TempPath( "makefabric_i.RISEscene" );
	{
		Job* pJob = LoadScene( SceneLambertian(), tmp );
		Check( pJob != nullptr, "I: fixture derives" );
		if( pJob ) {
			// MCP: ADVERTISED and ROUTABLE.  The 1ed4e7c3 lesson is that the
			// adapter keeps two independent lists (BuildToolsList and
			// IsKnownToolName) and a verb can land in one but not the other
			// -- advertised, and answering -32601 to every call.
			std::unique_ptr<Agent::AgentSession> mcpSess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentMcpAdapter mcp( std::move( mcpSess ), Agent::AgentAutonomy::Commit );

			Agent::JsonValue listEnv; std::string lerr;
			Check( Agent::JsonParse( mcp.HandleLine(
			           "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\",\"params\":{}}" ),
			       listEnv, lerr ), "I: tools/list parses" );
			bool advertised = false;
			Agent::JsonValue fabricSchema;
			const Agent::JsonValue& tools = listEnv.get( "result" ).get( "tools" );
			for( std::size_t i = 0; i < tools.size(); ++i )
				if( tools.at( i ).get( "name" ).asString() == "make_fabric" ) {
					advertised = true;
					fabricSchema = tools.at( i ).get( "inputSchema" )
						.get( "properties" ).get( "fabric" ).get( "enum" );
				}
			Check( advertised, "I MONEY: tools/list ADVERTISES make_fabric" );
			Check( fabricSchema.isArray() && fabricSchema.size() == 7,
			       "I MONEY: `fabric` surfaces as a JSON `enum` of exactly the seven selectable "
			       "presets -- a closed list is what constrains a model toward a value that exists, "
			       "where a free string invites a spelling that only earns a refusal" );

			Agent::JsonValue callEnv; std::string cerr;
			Check( Agent::JsonParse( mcp.HandleLine(
			           "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
			           "\"params\":{\"name\":\"make_fabric\",\"arguments\":{}}}" ),
			       callEnv, cerr ), "I: tools/call parses" );
			const bool disowned = callEnv.has( "error" ) &&
			                      callEnv.get( "error" ).get( "code" ).asNumber( 0 ) == -32601.0;
			Check( !disowned,
			       "I MONEY: tools/call ROUTES make_fabric (not -32601) -- the two-list-drift bug "
			       "1ed4e7c3 fixed for build_element/place_element cannot recur for this verb" );
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// JSON-RPC dispatch, on a FRESH job, plus the enum argument travelling
	// over the wire.
	{
		const std::string tmp2 = TempPath( "makefabric_i2.RISEscene" );
		Job* pJob2 = LoadScene( SceneLambertian(), tmp2 );
		Check( pJob2 != nullptr, "I: rpc fixture derives" );
		if( pJob2 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob2 );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Commit );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"make_fabric\","
				"\"params\":{\"fabric\":\"velvet\"}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "I: response parses" );
			Check( !env.has( "error" ), "I: dispatch is not a JSON-RPC error" );
			const Agent::JsonValue& res = env.get( "result" );
			Check( res.get( "applied" ).asBool(), "I: the RPC call applied" );
			Check( res.get( "fabricPreset" ).asString() == "velvet",
			       "I MONEY: the ENUM argument travelled over the wire and selected the preset -- "
			       "the first enum-typed argument on any of these verbs" );
			Check( res.get( "substrateWasReused" ).asBool() &&
			       res.get( "mintedSubstrate" ).asString().empty() &&
			       !res.get( "originalNowUnreferenced" ).asBool(),
			       "I: velvet over a Lambertian is the PURE WRAP -- velvet's recommended substrate IS "
			       "a lambertian_material, so nothing needed minting and the three substrate-decision "
			       "fields say so consistently over the wire" );
			pJob2->release();
			std::remove( tmp2.c_str() );
		}
	}

	// Read autonomy refuses before the verb is ever reached.
	{
		const std::string tmp3 = TempPath( "makefabric_i3.RISEscene" );
		Job* pJob3 = LoadScene( SceneLambertian(), tmp3 );
		Check( pJob3 != nullptr, "I: read-autonomy fixture derives" );
		if( pJob3 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob3 );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Read );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"make_fabric\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.has( "error" ),
			       "I MONEY: make_fabric is REFUSED under Read autonomy -- it is not on the read-safe "
			       "allowlist, and the deny-by-default gate stops it before AgentSession sees it" );
			pJob3->release();
			std::remove( tmp3.c_str() );
		}
	}

	// PROPOSE autonomy: refused too, and with the verb's OWN
	// posture-specific message rather than the generic Read-flavoured
	// fallback.  make_fabric is deliberately excluded from
	// IsProposeSafeVerb for add_wetness's reason -- its commit is one
	// composite whole-document swap, which is no AgentProposalKind an
	// Owner could approve card-by-card -- so a regression that either
	// silently ALLOWED it under Propose or dropped it to the generic
	// message would otherwise pass unnoticed.
	{
		const std::string tmpP = TempPath( "makefabric_i5.RISEscene" );
		Job* pJobP = LoadScene( SceneLambertian(), tmpP );
		Check( pJobP != nullptr, "I: propose-autonomy fixture derives" );
		if( pJobP ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJobP );
			const std::string before = sess->ReadDocument();
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Propose );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"make_fabric\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "I: propose response parses" );
			Check( env.has( "error" ), "I MONEY: make_fabric is REFUSED under Propose autonomy too" );
			const std::string msg = env.get( "error" ).get( "message" ).asString();
			Check( msg.find( "make_fabric" ) != std::string::npos,
			       "I: the Propose refusal NAMES the verb" );
			Check( msg.find( "--agent-autonomy=propose" ) != std::string::npos &&
			       msg.find( "--agent-autonomy=commit" ) != std::string::npos,
			       "I MONEY: it is the verb's OWN Propose message -- truthful about the current posture "
			       "and about commit being the real escape hatch, not the generic Read-flavoured "
			       "fallback" );
			// The dispatcher owns the session, so read the document back
			// through it rather than through the moved-from pointer.
			const std::string after = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"read_document\",\"params\":{}}" );
			Check( after.find( "cloth_fabric" ) == std::string::npos,
			       "I: nothing was minted -- the Propose refusal is a no-op" );
			Check( !before.empty(), "I: the pre-refusal document was captured" );
			pJobP->release();
			std::remove( tmpP.c_str() );
		}
	}

	// External authority has no staged-proposal form.
	{
		const std::string tmp4 = TempPath( "makefabric_i4.RISEscene" );
		Job* pJob4 = LoadScene( SceneLambertian(), tmp4 );
		Check( pJob4 != nullptr, "I: external-authority fixture derives" );
		if( pJob4 ) {
			std::unique_ptr<Agent::AgentSession> sess =
				Agent::AgentSession::WrapJob( pJob4, Agent::AgentAuthority::External );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( std::string(), "satin" );
			Check( !r.ok && !r.applied,
			       "I: an External-authority session refuses -- one composite document swap is no "
			       "AgentProposalKind an Owner could approve card-by-card" );
			Check( r.message.find( "insert_chunk" ) != std::string::npos,
			       "I: ...and the refusal spells out the staged sequence that WOULD work" );
			Check( sess->ReadDocument() == before, "I: the document is BYTE-IDENTICAL" );
			sess.reset();
			pJob4->release();
			std::remove( tmp4.c_str() );
		}
	}
}

//======================================================================
// J: PRESET PARITY.  Three copies of one value list exist by design --
// `FabricPresetTable()` (the numbers), `AgentSession::
// kMakeFabricPresetValues` (what the verb accepts and what
// AgentMcpAdapter reads programmatically), and AgentChatCodecs'
// hand-authored JSON literal ("two texts, one verb").  A comment
// claiming they agree is exactly the thing a reorder of the table
// falsifies silently, so the claim is ASSERTED here instead.
//======================================================================
static void TestPresetParity()
{
	std::printf( "J: preset parity -- the verb's list, the table, and the chat codec's literal\n" );

	unsigned int n = 0;
	const RISE::Implementation::FabricPreset* table = RISE::Implementation::FabricPresetTable( n );
	Check( table != nullptr && n == Agent::AgentSession::kMakeFabricPresetCount + 1,
	       "J: the table has exactly ONE row more than the verb's list -- `custom`, which has no "
	       "recommended substrate to mint and is therefore deliberately not selectable" );
	if( !table || n < 1 ) return;

	Check( std::string( table[n-1].name ) == "custom",
	       "J: `custom` is the table's LAST row, which is also LookupFabricPreset's fallback" );

	bool ordered = true;
	for( std::size_t i = 0; i < Agent::AgentSession::kMakeFabricPresetCount && i + 1 < n; ++i )
		if( std::string( Agent::AgentSession::kMakeFabricPresetValues[i] ) != table[i].name )
			ordered = false;
	Check( ordered,
	       "J MONEY: the verb's value list is FabricPresetTable()'s rows, IN ORDER -- reordering the "
	       "table without reordering the array fails HERE rather than shipping a tool schema whose "
	       "order disagrees with the numbers behind it" );

	// The chat codec's copy is a hand-authored JSON string literal, so it
	// cannot read the array; that is precisely why it needs pinning.
	const std::string defs = Agent::ChatToolDefsFingerprint();
	std::string wanted = "\"enum\":[";
	for( std::size_t i = 0; i < Agent::AgentSession::kMakeFabricPresetCount; ++i ) {
		if( i ) wanted += ",";
		wanted += "\"" + std::string( Agent::AgentSession::kMakeFabricPresetValues[i] ) + "\"";
	}
	wanted += "]";
	Check( defs.find( wanted ) != std::string::npos,
	       "J MONEY: the chat codec's HAND-AUTHORED enum literal carries the same seven values in the "
	       "same order -- \"two texts, one verb\" for the one part of the schema that cannot be read "
	       "off the shared array" );
}

//======================================================================
// K: the `originalNowUnreferenced` TRUTH.  The mint path leaves the
// original chunk in the document with nothing pointing at it -- USUALLY.
// A material can also be reached through a `composite_material`'s
// `top`/`bottom`, which is the legacy sheen-over-base pairing this
// tree's own `sheen_material` descriptor points at, and that reference
// survives the swap untouched.  Claiming otherwise and advising
// `remove_chunk` would leave a dangling reference and stop the document
// deriving for every object that uses the composite.
//======================================================================
static void TestForeignReferences()
{
	std::printf( "K: originalNowUnreferenced is a TRUTH -- composite top/bottom still count\n" );

	const char* const kSlots[] = { "top", "bottom" };
	for( const char* slot : kSlots ) {
		std::string scene = Preamble();
		scene += Sphere( "sph" );
		scene += kLambChunk;
		scene += "lambertian_material\n{\n\tname other\n\treflectance dye\n}\n\n";
		// `cloth` is bound to an object AND composed into a composite --
		// both legal, and only the first is what this verb rewrites.
		scene += std::string( "composite_material\n{\n\tname layered\n\t" ) + slot +
		         " cloth\n\t" + ( std::string( slot ) == "top" ? "bottom" : "top" ) + " other\n}\n\n";
		scene += Obj( "o1", "sph", "cloth",  -1 );
		scene += Obj( "o2", "sph", "layered", 1 );

		const std::string tmp = TempPath( "makefabric_k.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, std::string( "K[" ) + slot + "]: fixture derives" );
		if( !pJob ) continue;

		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "cloth", "satin" );
		Check( r.ok && r.applied,
		       std::string( "K[" ) + slot + "] : the conversion still APPLIES -- composing the original "
		       "into a composite_material is legal, so this is a report, not a refusal -- " + r.message );
		Check( !r.substrateWasReused && r.mintedSubstrate == "cloth_fabric_base",
		       std::string( "K[" ) + slot + "]: it is the MINT path, where the first cut claimed the "
		       "original was unreferenced unconditionally" );
		Check( !r.originalNowUnreferenced,
		       std::string( "K[" ) + slot + "] MONEY: originalNowUnreferenced is FALSE -- "
		       "`composite_material`.`" + slot + "` still names `cloth`, and the reference set is "
		       "derived from the DESCRIPTORS (every Material-piped parameter on every chunk kind), "
		       "not from a hand-kept list of slot names that never had `top`/`bottom` in it" );
		Check( r.message.find( "STILL REFERENCED" ) != std::string::npos &&
		       r.message.find( std::string( "composite_material `layered`." ) + slot ) != std::string::npos,
		       std::string( "K[" ) + slot + "]: the message NAMES the chunk and slot that still points "
		       "at it" );
		Check( r.message.find( "remove_chunk it if you do not want it back" ) == std::string::npos,
		       std::string( "K[" ) + slot + "] MONEY: the removal advice is WITHHELD -- following it "
		       "would leave a dangling `" + slot + "` reference and the document would stop deriving "
		       "for every object bound to the composite" );
		Check( r.message.find( "do NOT remove it" ) != std::string::npos,
		       std::string( "K[" ) + slot + "]: ...and says so explicitly rather than merely staying "
		       "quiet" );

		const std::string doc = sess->ReadDocument();
		Check( ChunkBinds( doc, "composite_material", "layered", slot, "cloth" ),
		       std::string( "K[" ) + slot + "]: the composite still binds the ORIGINAL, untouched -- "
		       "this verb rebinds OBJECT `material` slots only" );
		Check( ChunkBinds( doc, "standard_object", "o1", "material", "cloth_fabric" ),
		       std::string( "K[" ) + slot + "]: ...while the directly-bound object DID move" );
		{
			std::string offender;
			Check( ValidatesClean( doc, offender ),
			       std::string( "K[" ) + slot + "] MONEY: the document still DERIVES -- which is exactly "
			       "what removing the original on the strength of the old message would have broken"
			       + ( offender.empty() ? std::string() : ( " [" + offender + "]" ) ) );
		}
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}

	// The control: the SAME fixture without the composite really does
	// report the original as unreferenced, so the case above is testing
	// the predicate rather than a message that never fires.
	{
		const std::string tmp = TempPath( "makefabric_k2.RISEscene" );
		Job* pJob = LoadScene( SceneLambertian(), tmp );
		Check( pJob != nullptr, "K3: control fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "cloth", "satin" );
			Check( r.ok && r.applied && r.originalNowUnreferenced,
			       "K3: with NOTHING else pointing at it, the same mint DOES report the original "
			       "unreferenced -- the predicate discriminates" );
			Check( r.message.find( "remove_chunk it if you do not want it back" ) != std::string::npos,
			       "K3: ...and the removal advice IS given there" );
			sess.reset(); pJob->release(); std::remove( tmp.c_str() );
		}
	}
}

//======================================================================
// L: what did NOT come across.  Only the colour painter is re-homed; a
// pbr predecessor's `metallic`/`roughness` can be spatially-varying
// MAPS, and losing one is a real texture loss from a verb whose
// headline promise is that the dye survives.
//======================================================================
static void TestUnportedSlotsAreNamed()
{
	std::printf( "L: the message names the slots the mint did NOT carry across\n" );
	std::string scene = Preamble();
	scene += Sphere( "sph" );
	// `pbr_metallic_roughness_material.roughness` is the COLOUR pipe by
	// construction, not by meaning (its descriptor says so: a
	// `scalar_painter` name here is looked up in the wrong manager, missed,
	// and silently synthesized as a ZERO painter).  So the painter-bound
	// roughness this case is about has to be a real Colour-pipe painter.
	scene += "uniformcolor_painter\n{\n\tname rough_map\n\tcolor 0.3 0.3 0.3\n"
	         "\tcolorspace Rec709RGB_Linear\n}\n\n";
	scene += "pbr_metallic_roughness_material\n{\n\tname panel\n\tbase_color dye\n"
	         "\tmetallic 0.0\n\troughness rough_map\n}\n\n";
	scene += Obj( "o1", "sph", "panel", 0 );

	const std::string tmp = TempPath( "makefabric_l.RISEscene" );
	Job* pJob = LoadScene( scene, tmp );
	Check( pJob != nullptr, "L: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentMakeFabricResult r = sess->MakeFabric( "panel", "denim" );
	Check( r.ok && r.applied, std::string( "L: applied -- " ) + r.message );
	Check( r.message.find( "NOTHING ELSE came across" ) != std::string::npos,
	       "L MONEY: the message says only the colour painter was re-homed" );
	Check( r.message.find( "roughness" ) != std::string::npos &&
	       r.message.find( "metallic" ) != std::string::npos,
	       "L MONEY: ...and NAMES the dropped slots -- `panel`'s roughness was bound to a PAINTER, and "
	       "that binding is gone, which is a real texture loss from a verb whose headline promise is "
	       "that the dye survives" );
	Check( r.message.find( "re-authored on `panel_fabric_base`" ) != std::string::npos,
	       "L: ...and says where to put it back" );
	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

int main()
{
	std::printf( "AgentMakeFabricTest -- CLOTH_FABRIC_DESIGN 9.7: make_fabric\n" );
	TestMintFourChunks();
	TestMintTwoChunks();
	TestPureWrap();
	TestGgxUnderSilkNowMints();
	TestWeaveBaseReuses();
	TestColourLookupOrder();
	TestInference();
	TestRefusals();
	TestUndo();
	TestSelectionAndDeterminism();
	TestWireSurface();
	TestPresetParity();
	TestForeignReferences();
	TestUnportedSlotsAreNamed();
	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail ? 1 : 0;
}
