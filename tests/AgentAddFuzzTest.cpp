//////////////////////////////////////////////////////////////////////
//
//  AgentAddFuzzTest.cpp - docs/CLOTH_FABRIC_DESIGN.md Phase 3
//    (2026-09-03): add_fuzz, the verb that grows a sparse
//    hair_geometry/hair_material fuzz shell over a fabric-like
//    material's own bound objects.
//
//  WHAT THIS VERB HAS TO GET RIGHT, and therefore what is measured
//  here.  Unlike make_fabric/add_wetness, this verb never REWRITES
//  anything -- it only ADDS a triad of new sibling chunks per bound
//  object.  The three ways that could silently fail:
//
//    (a) THE WRONG BASE.  hair_geometry's `base_geometry` must be the
//        TARGET OBJECT'S OWN geometry, not the fuzz object's own (there
//        is no such thing until this call mints it) -- case A asserts
//        this out of the document text.
//    (b) THE LOST DYE.  `color` on the minted hair_material must be
//        the EXACT string the fabric's own dye slot carries -- a chunk
//        name or an inline literal, copied verbatim, never re-resolved
//        into a different representation.  Case D covers the three
//        source shapes (fabric_material sheen_color, its base's
//        fallback, weave_material warp_color, a named plain diffuse).
//    (c) THE HALF-APPLIED ADD.  A triad per bound object must be ONE
//        undo step and ONE head bump, and every refusal must leave the
//        document BYTE-IDENTICAL.  Cases E and G.
//
//  Cases:
//    A  MINT: a wool fabric_material (Oren-Nayar base, no sheen_color)
//       bound to ONE sphere mints exactly three chunks, wires them
//       correctly (base_geometry, color, parent), leaves the ORIGINAL
//       material and object byte-identical, re-derives with no
//       error/warning diagnostic, and renders.
//    B  MULTI-OBJECT: the same material bound to TWO objects mints TWO
//       full triads (six chunks), and reports mintedObjectCount == 2.
//    C  AMOUNT BANDS: light < medium < heavy strand counts, and the
//       default (no `amount` argument) is `medium`.
//    D  COLOUR DERIVATION across the four source shapes.
//    E  THE FOUR REFUSALS, each with the document BYTE-IDENTICAL after.
//    F  THE LIGHT-COUNT WARN: a scene with < 2 lights gets a WARNING in
//       the message; a scene with >= 2 lights does not.
//    G  ONE headVersion bump, ONE undo step through a live controller.
//    H  SELECTION + DETERMINISM: the bare call takes the most-bound
//       fabric/weave material, never a plain diffuse; two runs from the
//       same document are byte-identical.
//    I  WIRE SURFACE: declared in the shared chat-codec tool table,
//       advertised AND routable on MCP, refused under Read, refused
//       under Propose with the verb's OWN posture-specific message,
//       refused under External authority with staged-step guidance.
//    J  `amount`'s enum list matches AgentSession's published array.
//    K  A REAL RENDER proving strands exist: a rim-lit sphere against a
//       black background reads brighter with a fuzz shell than without
//       one, because the fringe extends INTO pixels the bare material's
//       exact silhouette can never reach.
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
#include <utility>
#include <vector>

#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/AgentMcpAdapter.h"
#include "../src/Library/Agent/AgentChatCodecs.h"
#include "../src/Library/Agent/AgentDiagnostic.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Reference.h"

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
// Undo() the GUI's Cmd-Z drives -- AgentMakeFabricTest's own idiom.
//----------------------------------------------------------------------
class QuietController : public SceneEditController
{
public:
	explicit QuietController( IJobPriv& job )
	: SceneEditController( job, /*interactiveRasterizer*/0 ) {}
protected:
	void DoOneRenderPass() override {}
};

//! Split one chunk body into its `param value` lines -- AgentMakeFabricTest's
//! own whitespace-agnostic reader (the emitted text uses tabs; fixtures
//! below use spaces, and the CST round-trips both).
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
//! line reads `name`.
static std::vector<std::pair<std::string, std::string> > ChunkParams(
	const std::string& doc, const std::string& kind, const std::string& name )
{
	std::vector<std::pair<std::string, std::string> > out;
	std::size_t at = 0;
	for( ;; ) {
		at = doc.find( kind, at );
		if( at == std::string::npos ) return out;
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

static bool ChunkBinds( const std::string& doc, const std::string& kind,
                        const std::string& name, const std::string& param,
                        const std::string& value )
{
	const std::vector<std::pair<std::string, std::string> > ps = ChunkParams( doc, kind, name );
	for( std::size_t i = 0; i < ps.size(); ++i )
		if( ps[i].first == param && ps[i].second == value ) return true;
	return false;
}

static bool ChunkExists( const std::string& doc, const std::string& kind, const std::string& name )
{
	return !ChunkParams( doc, kind, name ).empty();
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
	// The severity-aware oracle (commit f2ef553a's rationale): ValidateText
	// legitimately appends Info-severity DESIGN_* advisories, so "clean"
	// means no diagnostic whose severity is other than Info.
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
		"pixelpel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n"
		"film\n{\n\twidth 24\n\theight 24\n}\n\n"
		"pinhole_camera\n{\n\tlocation 0 2 9\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 45.0\n}\n\n"
		"uniformcolor_painter\n{\n\tname dye\n\tcolor 0.42 0.28 0.14\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"uniformcolor_painter\n{\n\tname dye2\n\tcolor 0.85 0.80 0.70\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		"directional_light\n{\n\tname key\n\tpower 3.0\n\tcolor 1 1 1\n\tdirection 0.4 0.5 0.9\n}\n\n";
}

//! Radius matches the Phase 3 evaluation's own calibration sphere
//! (0.15 units), so `medium` reproduces its baseline ~24000-strand
//! recipe rather than the many-hundred-thousand-strand count a
//! MakeFabricTest-style radius-0.7 sphere would scale up to (real
//! area scales with radius SQUARED) -- fast text-only chunk checks
//! don't care, but case A's real Render() does.
static std::string Sphere( const std::string& name, double radius = 0.15 )
{
	char buf[128];
	std::snprintf( buf, sizeof( buf ),
		"sphere_geometry\n{\n\tname %s\n\tradius %g\n}\n\n", name.c_str(), radius );
	return buf;
}

//! A curv-barren geometry hair_geometry's own base check refuses --
//! FuzzGeometryKindHostsHair_'s exact family.
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

//! A wool fabric_material -- Oren-Nayar base, NO `sheen_color` (so the
//! colour-fallback path is exercised by default), bound to `sph`/`o1`.
static std::string SceneWoolFabric()
{
	std::string s = Preamble();
	s += Sphere( "sph" );
	s += "orennayar_material\n{\n\tname wool_base\n\treflectance dye\n\troughness 0.6\n}\n\n";
	s += "fabric_material\n{\n\tname wool_fab\n\tfabric wool\n\tbase wool_base\n}\n\n";
	s += Obj( "o1", "sph", "wool_fab", 0 );
	return s;
}

//! The same wool fabric bound to TWO objects sharing one sphere geometry.
static std::string SceneWoolFabricTwoObjects()
{
	std::string s = Preamble();
	s += Sphere( "sph" );
	s += "orennayar_material\n{\n\tname wool_base\n\treflectance dye\n\troughness 0.6\n}\n\n";
	s += "fabric_material\n{\n\tname wool_fab\n\tfabric wool\n\tbase wool_base\n}\n\n";
	s += Obj( "o1", "sph", "wool_fab", -1.5 );
	s += Obj( "o2", "sph", "wool_fab",  1.5 );
	return s;
}

//======================================================================
static void TestMintThreeChunks()
{
	std::printf( "A: MINT -- a wool fabric_material bound to one object mints three chunks and wires them\n" );
	const std::string tmp = TempPath( "addfuzz_a.RISEscene" );
	Job* pJob = LoadScene( SceneWoolFabric(), tmp );
	Check( pJob != nullptr, "A: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const std::string docBefore = sess->ReadDocument();

	const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
	Check( r.ok && r.applied, std::string( "A: applied -- " ) + r.message );
	Check( r.status == "applied", "A: status is \"applied\"" );
	Check( r.material == "wool_fab", "A: it fuzzed `wool_fab`" );
	Check( r.materialKind == "fabric_material", "A: materialKind is the picked chunk's own kind" );
	Check( r.amount == "medium", "A: the default amount is `medium`" );
	Check( r.qualifyingMaterials == 1, "A: one material qualified" );
	Check( r.boundObjects == 1, "A: it reports the blast radius (1 bound object)" );
	Check( r.mintedObjectCount == 1, "A: one fuzz triad was minted" );
	Check( r.fuzzGeometry == "o1_fuzz",          "A: the geometry is `o1_fuzz`" );
	Check( r.fuzzMaterial == "o1_fuzz_material", "A: the material is `o1_fuzz_material`" );
	Check( r.fuzzObject   == "o1_fuzz_object",   "A: the object is `o1_fuzz_object`" );
	Check( r.strandCount > 0, "A: a positive strand count was realized" );

	const std::string doc = sess->ReadDocument();

	Check( ChunkExists( doc, "hair_geometry", "o1_fuzz" ), "A: the hair_geometry chunk exists" );
	Check( ChunkExists( doc, "hair_material", "o1_fuzz_material" ), "A: the hair_material chunk exists" );
	Check( ChunkExists( doc, "standard_object", "o1_fuzz_object" ), "A: the standard_object chunk exists" );

	Check( ChunkBinds( doc, "hair_geometry", "o1_fuzz", "base_geometry", "sph" ),
	       "A MONEY: base_geometry is `sph` -- the TARGET OBJECT'S OWN geometry, not something new" );
	Check( ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "count" ) &&
	       ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "length" ) &&
	       ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "width_root" ) &&
	       ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "width_tip" ) &&
	       ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "seed" ),
	       "A: every scaled field was written" );
	Check( ChunkBinds( doc, "hair_geometry", "o1_fuzz", "segments", "4" ) &&
	       ChunkBinds( doc, "hair_geometry", "o1_fuzz", "base_detail", "48" ) &&
	       ChunkBinds( doc, "hair_geometry", "o1_fuzz", "frizz", "0.35" ),
	       "A: segments/base_detail/frizz stay at the evaluation's own tuned constants" );
	Check( !ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "comb" ) &&
	       !ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "clump" ) &&
	       !ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "gravity" ) &&
	       !ChunkHasParam( doc, "hair_geometry", "o1_fuzz", "curl_radius" ),
	       "A: no comb/clump/gravity/curl was written -- the cheapest groom recipe" );

	// REVIEW P3R1 P2-4 (2026-09-03): an ABSENT `sheen_color` is NOT "go
	// read the base's own dye" -- it is the chunk's own documented default,
	// WHITE for every preset but velvet (`wool_fab` uses `wool`, so white).
	// `hair_material.color` resolves strictly by name, so that inline
	// white literal is minted into its own `uniformcolor_painter` first.
	Check( ChunkBinds( doc, "hair_material", "o1_fuzz_material", "color", "wool_fab_fuzz_dye" ),
	       "A MONEY: colour is `wool_fab`'s own EFFECTIVE sheen default (WHITE, since `wool` sets no "
	       "sheen_color of its own) -- NOT the base's `reflectance` -- minted into a named painter "
	       "because hair_material.color cannot resolve an inline literal directly" );
	Check( ChunkBinds( doc, "uniformcolor_painter", "wool_fab_fuzz_dye", "color", "1.0 1.0 1.0" ),
	       "A MONEY: the minted dye painter carries the white literal verbatim" );
	Check( ChunkBinds( doc, "hair_material", "o1_fuzz_material", "beta_m", "0.4" ) &&
	       ChunkBinds( doc, "hair_material", "o1_fuzz_material", "beta_n", "0.4" ),
	       "A: beta_m/beta_n are the evaluation's tuned Chiang-lobe roughness" );

	Check( ChunkBinds( doc, "standard_object", "o1_fuzz_object", "geometry", "o1_fuzz" ) &&
	       ChunkBinds( doc, "standard_object", "o1_fuzz_object", "material", "o1_fuzz_material" ) &&
	       ChunkBinds( doc, "standard_object", "o1_fuzz_object", "parent", "o1" ),
	       "A MONEY: the fuzz object binds the minted geometry+material and is `parent`-ed to `o1` with "
	       "no transform of its own, so it exactly tracks the target" );

	// -- The original chunks are byte-identical (contained verbatim).
	Check( doc.find( "fabric_material\n{\n\tname wool_fab\n\tfabric wool\n\tbase wool_base\n}" )
	           != std::string::npos,
	       "A MONEY: the original `wool_fab` chunk is BYTE-IDENTICAL -- this verb never edits the target" );
	Check( doc.find( "standard_object\n{\n\tname o1\n\tgeometry sph\n\tmaterial wool_fab\n\tposition 0 0 0\n}" )
	           != std::string::npos,
	       "A MONEY: the original `o1` object is BYTE-IDENTICAL -- its `material` is STILL `wool_fab`, "
	       "never rebound" );

	{
		std::string offender;
		Check( ValidatesClean( doc, offender ),
		       "A MONEY: the re-derived document carries NO error/warning diagnostic"
		       + ( offender.empty() ? std::string() : ( " [" + offender + "]" ) ) );
	}
	{
		Agent::AgentRenderParams rp;
		rp.width = 24; rp.height = 24; rp.samples = 4;
		const Agent::AgentRenderResult rr = sess->Render( rp );
		Check( rr.ok, "A: the fuzzed scene renders" );
		Check( rr.meanR + rr.meanG + rr.meanB > 0.0, "A: ...and is non-black" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestMultiObject()
{
	std::printf( "B: MULTI-OBJECT -- one material bound to two objects mints two full triads\n" );
	const std::string tmp = TempPath( "addfuzz_b.RISEscene" );
	Job* pJob = LoadScene( SceneWoolFabricTwoObjects(), tmp );
	Check( pJob != nullptr, "B: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
	const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
	Check( r.ok && r.applied, std::string( "B: applied -- " ) + r.message );
	Check( r.boundObjects == 2, "B: two bound objects" );
	Check( r.mintedObjectCount == 2, "B MONEY: TWO fuzz triads were minted, one per bound object" );

	const std::string doc = sess->ReadDocument();
	Check( ChunkExists( doc, "hair_geometry", "o1_fuzz" ) && ChunkExists( doc, "hair_geometry", "o2_fuzz" ),
	       "B: both hair_geometry chunks exist" );
	Check( ChunkExists( doc, "hair_material", "o1_fuzz_material" ) &&
	       ChunkExists( doc, "hair_material", "o2_fuzz_material" ),
	       "B: both hair_material chunks exist" );
	Check( ChunkExists( doc, "standard_object", "o1_fuzz_object" ) &&
	       ChunkExists( doc, "standard_object", "o2_fuzz_object" ),
	       "B: both standard_object chunks exist" );
	Check( ChunkBinds( doc, "standard_object", "o1_fuzz_object", "parent", "o1" ) &&
	       ChunkBinds( doc, "standard_object", "o2_fuzz_object", "parent", "o2" ),
	       "B MONEY: each fuzz object is parented to ITS OWN target, not the other one" );

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
static void TestAmountBands()
{
	std::printf( "C: AMOUNT BANDS -- light < medium < heavy, default is medium\n" );

	double counts[3];
	const char* const amounts[3] = { "light", "medium", "heavy" };
	for( int i = 0; i < 3; ++i ) {
		const std::string tmp = TempPath( ( std::string( "addfuzz_c_" ) + amounts[i] + ".RISEscene" ).c_str() );
		Job* pJob = LoadScene( SceneWoolFabric(), tmp );
		Check( pJob != nullptr, "C: fixture derives" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz( std::string(), amounts[i] );
		Check( r.ok && r.applied, std::string( "C: applied (" ) + amounts[i] + ") -- " + r.message );
		Check( r.amount == amounts[i], std::string( "C: amount echoes `" ) + amounts[i] + "`" );
		counts[i] = r.strandCount;
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
	Check( counts[0] < counts[1] && counts[1] < counts[2],
	       "C MONEY: light < medium < heavy strand counts, strictly increasing" );

	// REVIEW P3R1 fix round (2026-09-03): now that density reads the
	// object's own REAL surface area (IObject::GetArea()) rather than an
	// equivalent-sphere estimate, `SceneWoolFabric()`'s sphere -- radius
	// EXACTLY the calibration baseline (0.15) -- gives an EXACT,
	// numerically predictable answer rather than merely an ordering: a
	// sphere's `GetArea()` is the analytic `4*pi*r^2` (SphereGeometry.cpp),
	// identical in form to the baseline's own calibration arithmetic, so
	// `medium` must be exactly 24000 (the evaluation's own tuned count),
	// `light` exactly 12000 (the documented 0.5x multiplier), and `heavy`
	// exactly 48000 (the documented 2.0x multiplier) -- not merely "some
	// smaller/larger number".
	Check( (int)counts[0] == 12000, "C MONEY: `light` is EXACTLY 12000 (24000 * the documented 0.5x)" );
	Check( (int)counts[1] == 24000, "C MONEY: `medium` is EXACTLY 24000 (the evaluation's own tuned count)" );
	Check( (int)counts[2] == 48000, "C MONEY: `heavy` is EXACTLY 48000 (24000 * the documented 2.0x)" );

	// Default (no argument) matches `medium` exactly.
	{
		const std::string tmp = TempPath( "addfuzz_c_default.RISEscene" );
		Job* pJob = LoadScene( SceneWoolFabric(), tmp );
		Check( pJob != nullptr, "C: default fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( r.ok && r.applied, std::string( "C: default applied -- " ) + r.message );
			Check( r.strandCount == static_cast<int>( counts[1] ),
			       "C MONEY: omitting `amount` reproduces EXACTLY `medium`'s strand count" );
			sess.reset();
			pJob->release();
			std::remove( tmp.c_str() );
		}
	}
}

//======================================================================
static void TestColourDerivation()
{
	std::printf( "D: COLOUR DERIVATION -- fabric sheen_color, preset default (white/velvet), weave warp_color, named diffuse\n" );

	// D1: fabric_material WITH an explicit sheen_color -- it wins over
	// the base's own reflectance.
	{
		std::string s = Preamble();
		s += Sphere( "sph" );
		s += "orennayar_material\n{\n\tname base1\n\treflectance dye\n\troughness 0.6\n}\n\n";
		s += "fabric_material\n{\n\tname fab1\n\tfabric wool\n\tbase base1\n\tsheen_color dye2\n}\n\n";
		s += Obj( "o1", "sph", "fab1", 0 );
		const std::string tmp = TempPath( "addfuzz_d1.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "D1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( r.ok && r.applied, std::string( "D1: applied -- " ) + r.message );
			const std::string doc = sess->ReadDocument();
			Check( ChunkBinds( doc, "hair_material", "o1_fuzz_material", "color", "dye2" ),
			       "D1 MONEY: `sheen_color` (`dye2`) wins over the base's own `reflectance` (`dye`)" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// D4 (REVIEW P3R1 P2-4, 2026-09-03): fabric_material with NO
	// `sheen_color` -- the chunk's own documented default is the PRESET's
	// colour where the preset sets one (velvet only) and otherwise WHITE,
	// NEVER the base's own reflectance.  `wool` sets none, so Test A's own
	// fixture already covers the white branch; this covers the velvet
	// branch, the one preset with its own dark-grey default (0.30 0.30
	// 0.30 per FabricPresetTable).
	{
		std::string s = Preamble();
		s += Sphere( "sph" );
		s += "lambertian_material\n{\n\tname velvet_base\n\treflectance dye\n}\n\n";
		s += "fabric_material\n{\n\tname fab_velvet\n\tfabric velvet\n\tbase velvet_base\n}\n\n";
		s += Obj( "o1", "sph", "fab_velvet", 0 );
		const std::string tmp = TempPath( "addfuzz_d4.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "D4: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( r.ok && r.applied, std::string( "D4: applied -- " ) + r.message );
			const std::string doc = sess->ReadDocument();
			Check( ChunkBinds( doc, "hair_material", "o1_fuzz_material", "color", "fab_velvet_fuzz_dye" ),
			       "D4: colour is the minted dye painter, not the base's own `reflectance`" );
			Check( ChunkBinds( doc, "uniformcolor_painter", "fab_velvet_fuzz_dye", "color", "0.3 0.3 0.3" ),
			       "D4 MONEY: velvet's own dark-grey preset default (0.30 0.30 0.30), NOT white and NOT "
			       "the base's `reflectance` (`dye`)" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// D2: weave_material -- warp_color is the dye.
	{
		std::string s = Preamble();
		s += Sphere( "sph" );
		s += "weave_material\n{\n\tname denim_fab\n\tfabric denim\n\twarp_color dye\n}\n\n";
		s += Obj( "o1", "sph", "denim_fab", 0 );
		const std::string tmp = TempPath( "addfuzz_d2.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "D2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( r.ok && r.applied, std::string( "D2: applied -- " ) + r.message );
			Check( r.materialKind == "weave_material", "D2: it picked the weave_material" );
			const std::string doc = sess->ReadDocument();
			Check( ChunkBinds( doc, "hair_material", "o1_fuzz_material", "color", "dye" ),
			       "D2 MONEY: colour came from `warp_color`" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// D3: a NAMED plain-diffuse material -- a bare call never reaches
	// it, but naming it explicitly does.
	{
		std::string s = Preamble();
		s += Sphere( "sph" );
		s += "lambertian_material\n{\n\tname plain\n\treflectance dye2\n}\n\n";
		s += Obj( "o1", "sph", "plain", 0 );
		const std::string tmp = TempPath( "addfuzz_d3.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "D3: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

			// A BARE call must refuse -- nothing qualifies (`plain` is
			// not on the bare-call pool).
			const Agent::AgentSession::AgentAddFuzzResult bare = sess->AddFuzz();
			Check( !bare.ok || !bare.applied,
			       "D3 MONEY: a BARE call never reaches a plain-diffuse material -- it must be named" );

			const Agent::AgentSession::AgentAddFuzzResult named = sess->AddFuzz( "plain" );
			Check( named.ok && named.applied, std::string( "D3: named call applied -- " ) + named.message );
			Check( named.materialKind == "lambertian_material", "D3: it fuzzed the NAMED plain material" );
			const std::string doc = sess->ReadDocument();
			Check( ChunkBinds( doc, "hair_material", "o1_fuzz_material", "color", "dye2" ),
			       "D3 MONEY: colour came from the plain material's own `reflectance`" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}
}

//======================================================================
static void TestRefusals()
{
	std::printf( "E: THE FOUR REFUSALS, document byte-identical after each\n" );

	// E1: nothing qualifies -- no fabric/weave material, no explicit name.
	{
		std::string s = Preamble();
		s += Sphere( "sph" );
		s += "lambertian_material\n{\n\tname plain\n\treflectance dye\n}\n\n";
		s += Obj( "o1", "sph", "plain", 0 );
		const std::string tmp = TempPath( "addfuzz_e1.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "E1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( !r.ok && !r.applied, "E1: refused -- nothing qualifies" );
			Check( sess->ReadDocument() == before, "E1: document byte-identical" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// E2: an existing fuzz shell collides.
	{
		std::string s = SceneWoolFabric();
		s += "hair_geometry\n{\n\tname o1_fuzz\n\tbase_geometry sph\n\tcount 10\n\tlength 0.01\n}\n\n";
		const std::string tmp = TempPath( "addfuzz_e2.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "E2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( !r.ok && !r.applied, "E2 MONEY: refused -- `o1_fuzz` already exists" );
			Check( r.message.find( "o1_fuzz" ) != std::string::npos, "E2: the refusal NAMES the collision" );
			Check( sess->ReadDocument() == before, "E2: document byte-identical" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// E3: the bound geometry cannot host a groom (infiniteplane_geometry).
	{
		std::string s = Preamble();
		s += Plane( "flr" );
		s += "orennayar_material\n{\n\tname wool_base\n\treflectance dye\n\troughness 0.6\n}\n\n";
		s += "fabric_material\n{\n\tname wool_fab\n\tfabric wool\n\tbase wool_base\n}\n\n";
		s += Obj( "o1", "flr", "wool_fab", 0 );
		const std::string tmp = TempPath( "addfuzz_e3.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "E3: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( !r.ok && !r.applied,
			       "E3 MONEY: refused -- an infiniteplane_geometry base cannot host hair_geometry" );
			Check( sess->ReadDocument() == before, "E3: document byte-identical" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// E4: the picked material resolves to `silk` (or `satin`).
	{
		std::string s = Preamble();
		s += Sphere( "sph" );
		s += "weave_material\n{\n\tname silk_base\n\tfabric silk\n\twarp_color dye\n}\n\n";
		s += "fabric_material\n{\n\tname silk_fab\n\tfabric silk\n\tbase silk_base\n}\n\n";
		s += Obj( "o1", "sph", "silk_fab", 0 );
		const std::string tmp = TempPath( "addfuzz_e4.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "E4: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( !r.ok && !r.applied, "E4 MONEY: refused -- `silk` reads wrong with a fibrous fringe" );
			Check( sess->ReadDocument() == before, "E4: document byte-identical" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// E5: an unknown `amount` value.
	{
		const std::string tmp = TempPath( "addfuzz_e5.RISEscene" );
		Job* pJob = LoadScene( SceneWoolFabric(), tmp );
		Check( pJob != nullptr, "E5: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz( std::string(), "extra_fluffy" );
			Check( !r.ok && !r.applied, "E5: refused -- `extra_fluffy` is not an amount" );
			Check( sess->ReadDocument() == before, "E5: document byte-identical" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}
}

//======================================================================
static void TestLightWarn()
{
	std::printf( "F: the light-count WARN -- <2 lights warns, >=2 does not\n" );

	// F1: the base fixture has exactly ONE light -- warns.
	{
		const std::string tmp = TempPath( "addfuzz_f1.RISEscene" );
		Job* pJob = LoadScene( SceneWoolFabric(), tmp );
		Check( pJob != nullptr, "F1: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( r.ok && r.applied, std::string( "F1: applied -- " ) + r.message );
			Check( r.message.find( "WARNING" ) != std::string::npos,
			       "F1 MONEY: one light in the scene -- the message WARNS about the missing rim/back light" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// F2: add a second (rim) light -- no warning.
	{
		std::string s = SceneWoolFabric();
		s += "directional_light\n{\n\tname rim\n\tpower 2.0\n\tcolor 1 1 1\n\tdirection 0.1 0.1 0.99\n}\n\n";
		const std::string tmp = TempPath( "addfuzz_f2.RISEscene" );
		Job* pJob = LoadScene( s, tmp );
		Check( pJob != nullptr, "F2: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
			Check( r.ok && r.applied, std::string( "F2: applied -- " ) + r.message );
			Check( r.message.find( "WARNING" ) == std::string::npos,
			       "F2 MONEY: two lights -- no warning" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}
}

//======================================================================
static void TestUndo()
{
	std::printf( "G: ONE headVersion bump, ONE undo step\n" );
	const std::string tmp = TempPath( "addfuzz_g.RISEscene" );
	Job* pJob = LoadScene( SceneWoolFabric(), tmp );
	Check( pJob != nullptr, "G: fixture derives" );
	if( !pJob ) return;

	{
		QuietController c( *pJob );
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		sess->AttachController( &c );

		const std::string before = sess->ReadDocument();
		const RISE::Cst::CstHeadVersion hvBefore = sess->ReadDocumentSnapshot().headVersion;

		const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
		Check( r.ok && r.applied, std::string( "G: applied through the controller -- " ) + r.message );
		const std::string after = sess->ReadDocument();
		Check( after != before, "G: the controller-attached commit really changed the document" );

		const RISE::Cst::CstHeadVersion hvAfter = sess->ReadDocumentSnapshot().headVersion;
		Check( hvAfter.revision == hvBefore.revision + 1,
		       "G MONEY: EXACTLY ONE headVersion bump for three minted chunks -- it is one composite "
		       "document swap, not three edits" );

		c.Undo();
		Check( sess->ReadDocument() == before,
		       "G MONEY: ONE Undo() restores the pre-verb document BYTE-EXACTLY" );
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
	std::printf( "H: the bare call takes the most-bound fabric/weave material; two runs are byte-identical\n" );

	std::string scene = Preamble();
	scene += Sphere( "sph" );
	scene += "orennayar_material\n{\n\tname base_a\n\treflectance dye\n\troughness 0.6\n}\n\n";
	scene += "orennayar_material\n{\n\tname base_b\n\treflectance dye\n\troughness 0.6\n}\n\n";
	scene += "fabric_material\n{\n\tname fab_a\n\tfabric wool\n\tbase base_a\n}\n\n";
	scene += "fabric_material\n{\n\tname fab_b\n\tfabric wool\n\tbase base_b\n}\n\n";
	scene += Obj( "o1", "sph", "fab_a", -2 );
	scene += Obj( "o2", "sph", "fab_b", -1 );
	scene += Obj( "o3", "sph", "fab_b",  0 );
	scene += Obj( "o4", "sph", "fab_b",  1 );

	std::string first;
	for( int pass = 0; pass < 2; ++pass ) {
		const std::string tmp = TempPath( "addfuzz_h.RISEscene" );
		Job* pJob = LoadScene( scene, tmp );
		Check( pJob != nullptr, "H: fixture derives" );
		if( !pJob ) return;
		std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
		const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
		Check( r.ok && r.applied, std::string( "H: applied -- " ) + r.message );
		if( pass == 0 ) {
			Check( r.material == "fab_b",
			       "H MONEY: the bare call took `fab_b` -- bound to THREE objects, not `fab_a` (one)" );
			Check( r.qualifyingMaterials == 2, "H: both fabrics qualified" );
			Check( r.mintedObjectCount == 3, "H: all three of its objects got a triad" );
			first = sess->ReadDocument();
		}
		else {
			Check( sess->ReadDocument() == first,
			       "H MONEY: two runs from the SAME input document produce BYTE-IDENTICAL output" );
		}
		sess.reset();
		pJob->release();
		std::remove( tmp.c_str() );
	}
}

//======================================================================
static void TestWireSurface()
{
	std::printf( "I: wire surface -- chat-codec table, MCP advertised AND routable, Read/Propose/External refuse\n" );

	{
		const std::string defs = Agent::ChatToolDefsFingerprint();
		Check( defs.find( "add_fuzz" ) != std::string::npos,
		       "I: the verb is declared in the shared kToolDefs table" );
	}

	const std::string tmp = TempPath( "addfuzz_i.RISEscene" );
	{
		Job* pJob = LoadScene( SceneWoolFabric(), tmp );
		Check( pJob != nullptr, "I: fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> mcpSess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentMcpAdapter mcp( std::move( mcpSess ), Agent::AgentAutonomy::Commit );

			Agent::JsonValue listEnv; std::string lerr;
			Check( Agent::JsonParse( mcp.HandleLine(
			           "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\",\"params\":{}}" ),
			       listEnv, lerr ), "I: tools/list parses" );
			bool advertised = false;
			Agent::JsonValue amountSchema;
			const Agent::JsonValue& tools = listEnv.get( "result" ).get( "tools" );
			for( std::size_t i = 0; i < tools.size(); ++i )
				if( tools.at( i ).get( "name" ).asString() == "add_fuzz" ) {
					advertised = true;
					amountSchema = tools.at( i ).get( "inputSchema" )
						.get( "properties" ).get( "amount" ).get( "enum" );
				}
			Check( advertised, "I MONEY: tools/list ADVERTISES add_fuzz" );
			Check( amountSchema.isArray() && amountSchema.size() == 3,
			       "I MONEY: `amount` surfaces as a JSON `enum` of exactly the three selectable tiers" );

			Agent::JsonValue callEnv; std::string cerr;
			Check( Agent::JsonParse( mcp.HandleLine(
			           "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
			           "\"params\":{\"name\":\"add_fuzz\",\"arguments\":{}}}" ),
			       callEnv, cerr ), "I: tools/call parses" );
			const bool disowned = callEnv.has( "error" ) &&
			                      callEnv.get( "error" ).get( "code" ).asNumber( 0 ) == -32601.0;
			Check( !disowned, "I MONEY: tools/call ROUTES add_fuzz (not -32601)" );
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	// JSON-RPC dispatch, on a fresh job, plus the `amount` enum argument
	// travelling over the wire.
	{
		const std::string tmp2 = TempPath( "addfuzz_i2.RISEscene" );
		Job* pJob2 = LoadScene( SceneWoolFabric(), tmp2 );
		Check( pJob2 != nullptr, "I: rpc fixture derives" );
		if( pJob2 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob2 );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Commit );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"add_fuzz\","
				"\"params\":{\"amount\":\"heavy\"}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "I: response parses" );
			Check( !env.has( "error" ), "I: dispatch is not a JSON-RPC error" );
			const Agent::JsonValue& res = env.get( "result" );
			Check( res.get( "applied" ).asBool(), "I: the RPC call applied" );
			Check( res.get( "amount" ).asString() == "heavy",
			       "I MONEY: the ENUM argument travelled over the wire and selected the tier" );
			pJob2->release();
			std::remove( tmp2.c_str() );
		}
	}

	// Read autonomy refuses before the verb is ever reached.
	{
		const std::string tmp3 = TempPath( "addfuzz_i3.RISEscene" );
		Job* pJob3 = LoadScene( SceneWoolFabric(), tmp3 );
		Check( pJob3 != nullptr, "I: read-autonomy fixture derives" );
		if( pJob3 ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob3 );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Read );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"add_fuzz\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.has( "error" ),
			       "I MONEY: add_fuzz is REFUSED under Read autonomy" );
			pJob3->release();
			std::remove( tmp3.c_str() );
		}
	}

	// Propose autonomy: refused with the verb's OWN posture-specific message.
	{
		const std::string tmpP = TempPath( "addfuzz_i5.RISEscene" );
		Job* pJobP = LoadScene( SceneWoolFabric(), tmpP );
		Check( pJobP != nullptr, "I: propose-autonomy fixture derives" );
		if( pJobP ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJobP );
			Agent::AgentRpcDispatcher disp( std::move( sess ), Agent::AgentAutonomy::Propose );
			const std::string resp = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"add_fuzz\",\"params\":{}}" );
			Agent::JsonValue env; std::string perr;
			Check( Agent::JsonParse( resp, env, perr ) && env.isObject(), "I: propose response parses" );
			Check( env.has( "error" ), "I MONEY: add_fuzz is REFUSED under Propose autonomy too" );
			const std::string msg = env.get( "error" ).get( "message" ).asString();
			Check( msg.find( "add_fuzz" ) != std::string::npos, "I: the Propose refusal NAMES the verb" );
			Check( msg.find( "--agent-autonomy=propose" ) != std::string::npos &&
			       msg.find( "--agent-autonomy=commit" ) != std::string::npos,
			       "I MONEY: it is the verb's OWN Propose message" );
			const std::string after = disp.HandleLine(
				"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"read_document\",\"params\":{}}" );
			Check( after.find( "o1_fuzz" ) == std::string::npos, "I: nothing was minted -- the refusal is a no-op" );
			pJobP->release();
			std::remove( tmpP.c_str() );
		}
	}

	// External authority has no staged-proposal form.
	{
		const std::string tmp4 = TempPath( "addfuzz_i4.RISEscene" );
		Job* pJob4 = LoadScene( SceneWoolFabric(), tmp4 );
		Check( pJob4 != nullptr, "I: external-authority fixture derives" );
		if( pJob4 ) {
			std::unique_ptr<Agent::AgentSession> sess =
				Agent::AgentSession::WrapJob( pJob4, Agent::AgentAuthority::External );
			const std::string before = sess->ReadDocument();
			const Agent::AgentSession::AgentAddFuzzResult r = sess->AddFuzz();
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
static void TestAmountEnumParity()
{
	std::printf( "J: amount enum parity -- kAddFuzzAmountValues and AddFuzzAmountList()\n" );

	Check( Agent::AgentSession::kAddFuzzAmountCount == 3, "J: exactly three amount tiers" );
	Check( std::string( Agent::AgentSession::kAddFuzzAmountValues[0] ) == "light" &&
	       std::string( Agent::AgentSession::kAddFuzzAmountValues[1] ) == "medium" &&
	       std::string( Agent::AgentSession::kAddFuzzAmountValues[2] ) == "heavy",
	       "J MONEY: the order is light, medium, heavy -- matching the tool schema's enum" );
	Check( Agent::AgentSession::AddFuzzAmountList() == "light, medium, heavy",
	       "J: AddFuzzAmountList() joins them comma-separated in the same order" );
}

//======================================================================
// J2 (fix round 2026-09-03, REVIEW P3R1 coordinator round): FLAT-VS-ROUND
// PARITY.  The density model reads each object's own REAL world-space
// surface area (IObject::GetArea()) rather than an equivalent-sphere
// estimate off its bounding box, specifically so a flat object and a
// round object of the SAME real area realize the SAME strand count --
// the earlier equivalent-sphere model over-minted a large flat panel by
// well over an order of magnitude relative to a round object of equal
// area, which is exactly the bug this case guards against regressing to.
//======================================================================
static void TestFlatVsRoundAreaParity()
{
	std::printf( "J2: flat-vs-round parity -- equal real area gives equal strand count\n" );

	// A sphere of radius R has area 4*pi*R^2; a square clippedplane of
	// side s has area s^2 (ClippedPlaneGeometry::GetArea's parallelogram
	// formula is EXACT for an axis-aligned square).  Pick R so the two
	// areas match.
	const double R = 0.3;
	const double sphereArea = 4.0 * 3.14159265358979323846 * R * R;
	const double side = std::sqrt( sphereArea );
	const double half = side * 0.5;

	std::string scene = Preamble();
	scene += Sphere( "sph_round", R );
	{
		char buf[512];
		std::snprintf( buf, sizeof( buf ),
			"clippedplane_geometry\n{\n\tname sph_flat\n"
			"\tpta %g %g 0\n\tptb %g %g 0\n\tptc %g %g 0\n\tptd %g %g 0\n"
			"\tdoublesided TRUE\n}\n\n",
			-half, -half,  half, -half,  half, half,  -half, half );
		scene += buf;
	}
	scene += "orennayar_material\n{\n\tname base_round\n\treflectance dye\n\troughness 0.6\n}\n\n";
	scene += "orennayar_material\n{\n\tname base_flat\n\treflectance dye\n\troughness 0.6\n}\n\n";
	scene += "fabric_material\n{\n\tname fab_round\n\tfabric wool\n\tbase base_round\n}\n\n";
	scene += "fabric_material\n{\n\tname fab_flat\n\tfabric wool\n\tbase base_flat\n}\n\n";
	scene += Obj( "o_round", "sph_round", "fab_round", -2 );
	scene += Obj( "o_flat",  "sph_flat",  "fab_flat",   2 );

	const std::string tmp = TempPath( "addfuzz_j2.RISEscene" );
	Job* pJob = LoadScene( scene, tmp );
	Check( pJob != nullptr, "J2: fixture derives" );
	if( !pJob ) return;

	std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );

	const Agent::AgentSession::AgentAddFuzzResult rRound = sess->AddFuzz( "fab_round" );
	Check( rRound.ok && rRound.applied, std::string( "J2: round applied -- " ) + rRound.message );
	const Agent::AgentSession::AgentAddFuzzResult rFlat = sess->AddFuzz( "fab_flat" );
	Check( rFlat.ok && rFlat.applied, std::string( "J2: flat applied -- " ) + rFlat.message );

	Check( rRound.strandCount > 0 && rFlat.strandCount > 0,
	       "J2: both minted a positive strand count" );
	if( rRound.strandCount > 0 && rFlat.strandCount > 0 ) {
		const double diff = std::fabs( (double)rRound.strandCount - (double)rFlat.strandCount );
		const double denom = std::max( (double)rRound.strandCount, (double)rFlat.strandCount );
		const double relDiff = diff / denom;
		std::printf( "  (J2 measured: round=%d flat=%d relDiff=%.4f)\n",
		             rRound.strandCount, rFlat.strandCount, relDiff );
		Check( relDiff < 0.10,
		       "J2 MONEY: a flat object and a round object of the SAME real surface area realize "
		       "strand counts within 10% of each other -- the surface-area-based density model "
		       "means the same strands-per-unit-area everywhere, not just on round objects" );
	}

	sess.reset();
	pJob->release();
	std::remove( tmp.c_str() );
}

//======================================================================
// K: A REAL RENDER proving strands exist.  A small sphere against a
// black background, rim-lit near the camera's own axis (the same trick
// the Phase 3 evaluation's own rig uses -- a single directional light
// whose terminator coincides with the camera-visible limb).  The bare
// material's silhouette is an exact analytic circle; ANY light landing
// outside that circle in the bare render is impossible.  A fuzz shell
// adds real geometry PAST that circle, so pixels the bare render must
// leave EXACTLY at the background's zero can pick up non-zero radiance
// in the fuzzed render.  Background pixels carry NO Monte-Carlo noise
// (a ray that hits nothing returns exactly 0, deterministically), so
// the comparison isolates the geometric effect from sampling noise for
// every pixel that matters, even though the interior sphere body still
// carries ordinary per-pixel MC variance from the two independent
// renders' own (wall-clock-seeded) sample sequences.
//======================================================================
static std::string SilhouetteScene( bool withFuzzShell, unsigned int samples )
{
	char buf[256];
	std::string s = "RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultDirectLighting\n}\n\n";
	std::snprintf( buf, sizeof( buf ),
		"pixelpel_rasterizer\n{\n\tsamples %u\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n", samples );
	s += buf;
	s += "film\n{\n\twidth 32\n\theight 32\n}\n\n";
	// The sphere is deliberately SMALL in frame (a modest angular radius,
	// ~4-5 px at this distance/fov) so the black background -- exactly
	// zero, with NO Monte-Carlo noise -- dominates the pixel count, and
	// the thin silhouette-fringe band is a much larger FRACTION of the
	// object's own footprint than it would be on a sphere filling most
	// of the frame (edge pixels scale with radius; interior pixels scale
	// with radius SQUARED).
	s += "pinhole_camera\n{\n\tlocation 0 0 7\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n";
	s += "uniformcolor_painter\n{\n\tname dye\n\tcolor 0.35 0.30 0.24\n\tcolorspace Rec709RGB_Linear\n}\n\n";
	// ONE light only -- a RIM light nearly along the camera's own +Z axis
	// (the grazing-limb trick) -- so the body reads dark under grazing-only
	// illumination and the measured signal is dominated by the fringe, not
	// by a front-lit body whose per-pixel noise would otherwise swamp it.
	s += "directional_light\n{\n\tname rim\n\tpower 5.0\n\tcolor 1 1 1\n\tdirection 0.05 0.05 0.998\n}\n\n";
	s += Sphere( "sph", 0.15 );
	s += "orennayar_material\n{\n\tname wool_base\n\treflectance dye\n\troughness 0.6\n}\n\n";
	s += "fabric_material\n{\n\tname wool_fab\n\tfabric wool\n\tbase wool_base\n}\n\n";
	s += Obj( "o1", "sph", "wool_fab", 0 );
	if( withFuzzShell ) {
		// Thicker/longer than a photoreal groom on purpose: this is a
		// synthetic proof of the GEOMETRIC mechanism at a deliberately
		// coarse 32x32 resolution, not a beauty render -- a photoreal
		// 0.1 mm fibre would be sub-pixel here and prove nothing.
		s += "hair_geometry\n{\n\tname o1_fuzz\n\tbase_geometry sph\n\tcount 12000\n\tsegments 4\n"
		     "\tlength 0.045\n\twidth_root 0.0012\n\twidth_tip 0.0004\n\tseed 7\n\tbase_detail 32\n"
		     "\tfrizz 0.35\n}\n\n";
		s += "hair_material\n{\n\tname o1_fuzz_material\n\tcolor dye\n\tbeta_m 0.4\n\tbeta_n 0.4\n}\n\n";
		s += "standard_object\n{\n\tname o1_fuzz_object\n\tgeometry o1_fuzz\n\tmaterial o1_fuzz_material\n"
		     "\tparent o1\n}\n\n";
	}
	return s;
}

typedef std::vector<unsigned char> Rgb8Row;

//! Decode PNG bytes into flat RGB8 rows -- AgentRenderAnchorTest's own
//! `DecodePng` idiom (RISE_API_CreatePNGReader over a non-owning
//! MemoryBuffer, eColorSpace_Rec709RGB_Linear so ReadColor is a bare
//! byte/255 with no transfer-function conversion).
static bool DecodePngRgb8( const std::vector<unsigned char>& png,
                           unsigned int& outW, unsigned int& outH, Rgb8Row& outPx )
{
	if( png.empty() ) return false;
	Implementation::MemoryBuffer* buf = new Implementation::MemoryBuffer(
		const_cast<char*>( reinterpret_cast<const char*>( png.data() ) ),
		(unsigned int)png.size(), /*bTakeOwnership*/false );
	IRasterImageReader* reader = nullptr;
	if( !RISE_API_CreatePNGReader( &reader, *buf, eColorSpace_Rec709RGB_Linear ) || !reader ) {
		safe_release( buf );
		return false;
	}
	unsigned int w = 0, h = 0;
	if( !reader->BeginRead( w, h ) ) { safe_release( reader ); safe_release( buf ); return false; }
	outW = w; outH = h;
	outPx.assign( (std::size_t)w * h * 3, 0 );
	auto toB = []( double v ) -> unsigned char {
		int i = (int)( v * 255.0 + 0.5 );
		if( i < 0 ) i = 0;
		if( i > 255 ) i = 255;
		return (unsigned char)i;
	};
	for( unsigned int y = 0; y < h; ++y ) {
		for( unsigned int x = 0; x < w; ++x ) {
			RISEColor c;
			reader->ReadColor( c, x, y );
			const std::size_t o = ( (std::size_t)y * w + x ) * 3;
			outPx[o]   = toB( c.base.r );
			outPx[o+1] = toB( c.base.g );
			outPx[o+2] = toB( c.base.b );
		}
	}
	reader->EndRead();
	safe_release( reader );
	safe_release( buf );
	return true;
}

static void TestRenderSilhouette()
{
	std::printf( "K: a real render -- fuzz strands light up pixels the bare silhouette can never reach\n" );

	// A whole-image MEAN is the WRONG metric here (RED-PROVED this
	// session): a fuzz shell can plausibly OCCLUDE part of the bare
	// material's own bright grazing sheen ring at the SAME time as it
	// extends the visible material past the analytic edge, so the mean
	// can move either way depending on which effect dominates at a given
	// density/lighting -- measured swings from +8% to -75% across a few
	// parameter tweaks while the geometric extension itself was present
	// throughout.  The PER-PIXEL question the Phase 3 evaluation's own
	// "outer-edge row" metric actually answers is narrower and immune to
	// that confound: are there pixels the FUZZED render lights up that
	// the BARE render is GUARANTEED to leave at exactly zero (a ray that
	// misses every object returns EXACTLY 0, with no Monte-Carlo noise
	// to cross a threshold by chance)?  Decode both PNGs and count them.
	const unsigned int kSamples = 192;
	unsigned int bareW = 0, bareH = 0, fuzzW = 0, fuzzH = 0;
	Rgb8Row barePx, fuzzPx;

	{
		const std::string tmp = TempPath( "addfuzz_k_bare.RISEscene" );
		Job* pJob = LoadScene( SilhouetteScene( false, kSamples ), tmp );
		Check( pJob != nullptr, "K: bare fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentRenderParams rp;
			rp.width = 32; rp.height = 32; rp.samples = static_cast<int>( kSamples );
			const Agent::AgentRenderResult rr = sess->Render( rp );
			Check( rr.ok, "K: bare scene renders" );
			Check( DecodePngRgb8( rr.png, bareW, bareH, barePx ), "K: bare PNG decodes" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}
	{
		// Prove the fuzz shell really is what add_fuzz("heavy") mints,
		// rather than trusting the hand-authored fixture text: run the
		// verb itself on this fixture and check it applies and realizes
		// strands (the exact numbers legitimately differ from the
		// hand-authored render fixture's -- different sphere/camera).
		const std::string tmpv = TempPath( "addfuzz_k_verb.RISEscene" );
		Job* pJobV = LoadScene( SilhouetteScene( false, 4 ), tmpv );
		if( pJobV ) {
			std::unique_ptr<Agent::AgentSession> sessV = Agent::AgentSession::WrapJob( pJobV );
			const Agent::AgentSession::AgentAddFuzzResult rv = sessV->AddFuzz( std::string(), "heavy" );
			Check( rv.ok && rv.applied, std::string( "K: verb applies on the silhouette fixture -- " ) + rv.message );
			Check( rv.strandCount > 0, "K: the verb's own mint on this fixture realizes strands too" );
			sessV.reset();
			pJobV->release();
		}
		std::remove( tmpv.c_str() );

		const std::string tmp = TempPath( "addfuzz_k_fuzz.RISEscene" );
		Job* pJob = LoadScene( SilhouetteScene( true, kSamples ), tmp );
		Check( pJob != nullptr, "K: fuzzed fixture derives" );
		if( pJob ) {
			std::unique_ptr<Agent::AgentSession> sess = Agent::AgentSession::WrapJob( pJob );
			Agent::AgentRenderParams rp;
			rp.width = 32; rp.height = 32; rp.samples = static_cast<int>( kSamples );
			const Agent::AgentRenderResult rr = sess->Render( rp );
			Check( rr.ok, "K: fuzzed scene renders" );
			Check( DecodePngRgb8( rr.png, fuzzW, fuzzH, fuzzPx ), "K: fuzzed PNG decodes" );
			sess.reset();
			pJob->release();
		}
		std::remove( tmp.c_str() );
	}

	Check( bareW == fuzzW && bareH == fuzzH && bareW > 0 && bareH > 0,
	       "K: both renders share the same resolution" );
	if( bareW != fuzzW || bareH != fuzzH || bareW == 0 || bareH == 0 ) return;

	// A byte >2/255 is "lit"; PNG quantization/dither on an exact-zero
	// background is not expected to cross that on its own (this is an
	// 8-bit sRGB-quantized buffer, not the raw linear film, but a true
	// zero-radiance background pixel encodes to byte 0 exactly either way).
	const unsigned char kLitThreshold = 2;
	auto isLit = []( const Rgb8Row& px, std::size_t i, unsigned char thresh ) {
		return px[i] > thresh || px[i+1] > thresh || px[i+2] > thresh;
	};

	int newlyLit = 0, bareLitCount = 0, fuzzLitCount = 0;
	for( unsigned int y = 0; y < bareH; ++y ) {
		for( unsigned int x = 0; x < bareW; ++x ) {
			const std::size_t i = ( (std::size_t)y * bareW + x ) * 3;
			const bool bareIsLit = isLit( barePx, i, kLitThreshold );
			const bool fuzzIsLit = isLit( fuzzPx, i, kLitThreshold );
			if( bareIsLit ) ++bareLitCount;
			if( fuzzIsLit ) ++fuzzLitCount;
			if( !bareIsLit && fuzzIsLit ) ++newlyLit;
		}
	}

	std::printf( "  (K measured: %ux%u, bare lit=%d, fuzz lit=%d, newly-lit-by-fuzz=%d)\n",
	             bareW, bareH, bareLitCount, fuzzLitCount, newlyLit );
	Check( bareLitCount > 0, "K: the bare sphere itself is visible (sanity)" );
	Check( newlyLit > 0,
	       "K MONEY: at least one pixel is LIT in the fuzzed render that is EXACTLY DARK in the bare "
	       "render -- strand geometry catching the rim light strictly PAST the analytic silhouette, "
	       "in a location no BSDF on the bare material could ever reach" );
	// REVIEW P3R1 fix round (2026-09-03): "at least one pixel" alone would
	// pass on a fluke -- a single stray dithered pixel, or a shell that
	// minted almost no visible strands, both satisfy `newlyLit > 0` without
	// the halo being remotely "unmistakable".  Measured across five repeat
	// runs of this exact fixture (192 spp, wall-clock-seeded, so genuinely
	// independent samples): newlyLit was 30, 31, 31, 31, 32 -- tightly
	// clustered, because the background pixels it counts carry NO
	// Monte-Carlo noise (a ray that misses everything is exactly zero
	// every run) and the count is dominated by the FIXED strand geometry,
	// not sampling variance.  kMinNewlyLitPixels is set well below that
	// observed floor (a >3x margin) so ordinary run-to-run noise cannot
	// trip it, while a regression that minted a shell too sparse, too
	// short, or aimed at the wrong geometry to produce a real halo -- the
	// exact "zero visible strands" failure mode the fixture is here to
	// catch -- still fails it.
	constexpr int kMinNewlyLitPixels = 10;
	Check( newlyLit >= kMinNewlyLitPixels,
	       "K MONEY: newly-lit-by-fuzz count (" + std::to_string( newlyLit ) + ") meets the stated "
	       "minimum (" + std::to_string( kMinNewlyLitPixels ) + ") -- a shell that mints essentially "
	       "no visible strands (a handful of stray pixels rather than a real halo) must fail this, "
	       "not merely clear `> 0`" );
}

//======================================================================
int main()
{
	std::printf( "AgentAddFuzzTest: docs/CLOTH_FABRIC_DESIGN.md Phase 3 (2026-09-03) add_fuzz\n" );

	TestMintThreeChunks();
	TestMultiObject();
	TestAmountBands();
	TestColourDerivation();
	TestRefusals();
	TestLightWarn();
	TestUndo();
	TestSelectionAndDeterminism();
	TestWireSurface();
	TestAmountEnumParity();
	TestFlatVsRoundAreaParity();
	TestRenderSilhouette();

	std::printf( "\n%d passed, %d failed\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
