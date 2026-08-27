//////////////////////////////////////////////////////////////////////
//
//  HairMaterialChunkTest.cpp - Contract test for the `hair_material`
//    chunk (Slice B of the hair/fur registration arc,
//    docs/HAIR_FUR_DESIGN.md section 4.2): registers the already-
//    implemented `HairMaterial` (src/Library/Materials/HairBSDF.{h,cpp},
//    HairMaterial.h) end-to-end so scenes can author `hair_material`
//    chunks.
//
//  WHAT THIS TEST OWNS (the parser / Job / registration surface, NOT
//  the BCSDF math -- that is HairBSDFTest.cpp's job):
//
//    1. EACH COLOUR TIER PARSES.  `color`, `sigma_a`, `eumelanin` alone,
//       `pheomelanin` alone, and both melanin slots together all
//       register a material whose GetBSDF()/GetSPF() are real,
//       non-null HairBRDF/HairSPF instances.
//
//    2. DEFAULTS APPLIED.  A chunk that omits beta_m/beta_n/alpha/ior
//       must behave EXACTLY like one that binds them explicitly to the
//       documented defaults (0.3/0.3/2.0/1.55) -- proved by asking the
//       two resulting HairBRDFs the same `value()` question and
//       checking bit-identical answers, not by inspecting internals.
//
//    3. TIER EXCLUSIVITY, AT PARSE TIME.  Zero tiers bound is a
//       rejection naming all three options; two-or-more tiers bound
//       (color+sigma_a) is also a rejection.  Both diagnostics are
//       captured from stdout (GlobalLog's eLog_Console sink includes
//       eLog_Error) and checked for the specific wording, not just the
//       boolean parse failure.
//
//    4. NAMED SCALAR_PAINTER BINDING actually changes behaviour.
//       Binding `beta_m` to a named `scalar_painter` at 0.6 must
//       produce a HairBRDF whose `value()` differs from the beta_m=0.3
//       default -- proving the named binding reaches the model, not
//       just that the chunk parses.
//
//    5. UNKNOWN / MISTYPED PAINTER NAME diagnostics.  A scalar slot
//       (`beta_m`) bound to a name that resolves to a legacy `IPainter`
//       chunk gets the established "bound to an IPainter" diagnostic;
//       bound to a name that resolves to nothing at all gets the
//       established "neither a registered scalar_painter nor an inline
//       numeric literal" diagnostic.  Both wordings are owned by
//       src/Library/Parsers/ChunkDescriptor.h's kScalarBoundToIPainterFmt
//       / kScalarUnknownFmt and reproduced here only as the substrings
//       that distinguish the two branches.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#ifdef _WIN32
	#include <process.h>
	#include <io.h>
	#define getpid _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>			// getpid(), dup(), dup2(), close()
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Materials/HairBSDF.h"
#include "../src/Library/Materials/HairMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

namespace {

//////////////////////////////////////////////////////////////////////
// Scene plumbing -- SkeletonGeometryChunkTest's pattern, unchanged.
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_hairmat_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << body;
	f.close();
	return path;
}

bool ParseBodyInto( const std::string& tag, const std::string& body, IJobPriv& job )
{
	const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );
	remove( path.c_str() );
	return ok;
}

//! Runs `ParseBodyInto` with stdout captured (GlobalLog's eLog_Console
//! sink includes eLog_Error, so a Job::AddHairMaterial diagnostic lands
//! there).  Same fd-dup/dup2 technique as
//! SkeletonGeometryChunkTest.cpp's degenerate-bone capture.
bool ParseBodyCapturing( const std::string& tag, const std::string& body, IJobPriv& job,
                          std::string& capturedOutput )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_hairmat_stdout_" + tag + "_" + pidbuf + ".txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

	const bool ok = ParseBodyInto( tag, body, job );

	std::fflush( stdout );
	if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
	if( capFile ) std::fclose( capFile );

	std::ifstream ifs( capPath.c_str() );
	if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); capturedOutput = oss.str(); }
	remove( capPath.c_str() );

	return ok;
}

//! Minimal `scalar_painter { name X value V }` fragment.
std::string ScalarPainter( const char* name, double value )
{
	std::ostringstream oss;
	oss << "scalar_painter\n{\n\tname\t" << name << "\n\tvalue\t" << value << "\n}\n";
	return oss.str();
}

//! Minimal `uniformcolor_painter` fragment -- used as the "bound to a
//! legacy IPainter chunk" negative fixture for a scalar slot.
std::string UniformColorPainter( const char* name )
{
	std::ostringstream oss;
	oss << "uniformcolor_painter\n{\n\tname\t" << name << "\n\tcolor\t0.5 0.5 0.5\n}\n";
	return oss.str();
}

//! `hair_material` fragment.  Any parameter left null/omitted is not
//! emitted, so the chunk's own "none" / documented default applies.
std::string HairMaterialChunk( const char* name,
                                const char* color = 0, const char* sigma_a = 0,
                                const char* eumelanin = 0, const char* pheomelanin = 0,
                                const char* beta_m = 0, const char* beta_n = 0,
                                const char* alpha = 0, const char* ior = 0 )
{
	std::ostringstream oss;
	oss << "hair_material\n{\n\tname\t" << name << "\n";
	if( color )       oss << "\tcolor\t"       << color       << "\n";
	if( sigma_a )     oss << "\tsigma_a\t"     << sigma_a     << "\n";
	if( eumelanin )   oss << "\teumelanin\t"   << eumelanin   << "\n";
	if( pheomelanin ) oss << "\tpheomelanin\t" << pheomelanin << "\n";
	if( beta_m )      oss << "\tbeta_m\t"      << beta_m      << "\n";
	if( beta_n )      oss << "\tbeta_n\t"      << beta_n      << "\n";
	if( alpha )       oss << "\talpha\t"       << alpha       << "\n";
	if( ior )         oss << "\tior\t"         << ior         << "\n";
	oss << "}\n";
	return oss.str();
}

//! Synthetic fibre intersection -- mirrors HairBSDFTest.cpp's
//! MakeFibreHit (deliberately duplicated here rather than shared: this
//! file is not allowed to touch HairBSDFTest.cpp, and the fixture is
//! short enough that a private copy is clearer than a cross-file
//! dependency).  Tangent = +X (onb.u()), normal = +Z (onb.w()),
//! bitangent = +Y (onb.v()); ptCoord.y encodes h = 2v - 1.
RayIntersectionGeometric MakeFibreHit( const double thetaO, const double phiO, const double h )
{
	const Vector3 wo( sin(thetaO), cos(thetaO) * cos(phiO), cos(thetaO) * sin(phiO) );
	const Ray inRay( Point3( wo.x, wo.y, wo.z ), Vector3( -wo.x, -wo.y, -wo.z ) );
	RasterizerState rs = {0, 0};
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal     = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromWU( Vector3( 0, 0, 1 ), Vector3( 1, 0, 0 ) );
	ri.ptCoord  = Point2( 0.5, 0.5 * ( h + 1.0 ) );
	ri.ptCoord1 = ri.ptCoord;

	return ri;
}

//! Retrieves `name` from the material manager and casts down to
//! HairBRDF via IMaterial::GetBSDF().  Returns 0 on any failure.
const HairBRDF* FetchHairBRDF( IJobPriv& job, const char* name )
{
	IMaterial* mat = job.GetMaterials() ? job.GetMaterials()->GetItem( name ) : 0;
	if( !mat ) return 0;
	return dynamic_cast<const HairBRDF*>( mat->GetBSDF() );
}

} // namespace

//////////////////////////////////////////////////////////////////////
// 1 -- each colour tier parses and registers a real HairBRDF/HairSPF
//////////////////////////////////////////////////////////////////////

void TestEachTierParses()
{
	std::cout << "Test: each of the three colour tiers parses and registers a real HairMaterial" << std::endl;

	struct Case { const char* tag; std::string body; };
	std::vector<Case> cases;

	{
		std::string body = UniformColorPainter( "pc" ) + HairMaterialChunk( "m_color", /*color*/ "pc" );
		cases.push_back( Case{ "color", body } );
	}
	{
		std::string body = ScalarPainter( "ps", 0.5 ) + HairMaterialChunk( "m_sigma", 0, /*sigma_a*/ "ps" );
		cases.push_back( Case{ "sigma_a", body } );
	}
	{
		std::string body = ScalarPainter( "peu", 1.3 ) + HairMaterialChunk( "m_eu", 0, 0, /*eumelanin*/ "peu" );
		cases.push_back( Case{ "eumelanin", body } );
	}
	{
		std::string body = ScalarPainter( "pph", 0.4 ) + HairMaterialChunk( "m_ph", 0, 0, 0, /*pheomelanin*/ "pph" );
		cases.push_back( Case{ "pheomelanin", body } );
	}
	{
		std::string body = ScalarPainter( "peu2", 0.8 ) + ScalarPainter( "pph2", 0.3 ) +
			HairMaterialChunk( "m_both", 0, 0, "peu2", "pph2" );
		cases.push_back( Case{ "both-melanin", body } );
	}

	for( std::size_t i = 0; i < cases.size(); ++i ) {
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, cases[i].tag + std::string(": job created") ); continue; }
		const bool ok = ParseBodyInto( cases[i].tag, cases[i].body, *job );
		Check( ok, std::string( "tier " ) + cases[i].tag + ": chunk parses" );

		const std::string matName = std::string("m_") +
			( cases[i].tag == std::string("color") ? "color" :
			  cases[i].tag == std::string("sigma_a") ? "sigma" :
			  cases[i].tag == std::string("eumelanin") ? "eu" :
			  cases[i].tag == std::string("pheomelanin") ? "ph" : "both" );

		IMaterial* mat = ( ok && job->GetMaterials() ) ? job->GetMaterials()->GetItem( matName.c_str() ) : 0;
		Check( mat != 0, std::string( "tier " ) + cases[i].tag + ": material registered under its own name" );
		if( mat ) {
			Check( dynamic_cast<const HairBRDF*>( mat->GetBSDF() ) != 0,
			       std::string( "tier " ) + cases[i].tag + ": GetBSDF() is a real HairBRDF" );
			Check( dynamic_cast<HairSPF*>( mat->GetSPF() ) != 0,
			       std::string( "tier " ) + cases[i].tag + ": GetSPF() is a real HairSPF" );
		}
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 2 -- defaults applied: omitting beta_m/beta_n/alpha/ior must behave
// EXACTLY like binding them to the documented defaults (0.3/0.3/2.0/1.55).
//////////////////////////////////////////////////////////////////////

void TestDefaultsApplied()
{
	std::cout << "Test: omitted beta_m/beta_n/alpha/ior behave exactly like the documented 0.3/0.3/2.0/1.55 defaults" << std::endl;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	std::string body;
	body += ScalarPainter( "eu_shared", 1.0 );
	body += HairMaterialChunk( "m_implicit", 0, 0, "eu_shared" );	// omits beta_m/beta_n/alpha/ior
	body += ScalarPainter( "bm_explicit", 0.3 );
	body += ScalarPainter( "bn_explicit", 0.3 );
	body += ScalarPainter( "al_explicit", 2.0 );
	body += ScalarPainter( "ir_explicit", 1.55 );
	body += HairMaterialChunk( "m_explicit", 0, 0, "eu_shared", 0,
	                           "bm_explicit", "bn_explicit", "al_explicit", "ir_explicit" );

	const bool ok = ParseBodyInto( "defaults", body, *job );
	Check( ok, "defaults: both fixtures parse" );

	const HairBRDF* implicitBRDF = FetchHairBRDF( *job, "m_implicit" );
	const HairBRDF* explicitBRDF = FetchHairBRDF( *job, "m_explicit" );
	Check( implicitBRDF != 0 && explicitBRDF != 0, "defaults: both HairBRDFs retrievable" );

	if( implicitBRDF && explicitBRDF ) {
		// Several (thetaO, phiO, h) cells -- if even one differs, the
		// omitted-parameter path is NOT reaching the documented default.
		const double cells[][3] = {
			{ 0.2, 0.9, 0.0 }, { 0.5, 1.7, 0.3 }, { -0.3, 2.4, -0.4 }, { 0.05, 0.1, 0.6 },
		};
		bool allMatch = true;
		for( std::size_t i = 0; i < sizeof(cells)/sizeof(cells[0]); ++i ) {
			const RayIntersectionGeometric ri = MakeFibreHit( cells[i][0], cells[i][1], cells[i][2] );
			const Vector3 wi = Vector3Ops::Normalize( Vector3( 0.3, 0.8, -0.2 ) );
			const RISEPel vImplicit = implicitBRDF->value( wi, ri );
			const RISEPel vExplicit = explicitBRDF->value( wi, ri );
			for( int c = 0; c < 3; ++c ) {
				if( vImplicit[(unsigned int)c] != vExplicit[(unsigned int)c] ) {
					allMatch = false;
					std::cout << "    cell " << i << " channel " << c << ": implicit="
					          << vImplicit[(unsigned int)c] << " explicit=" << vExplicit[(unsigned int)c] << std::endl;
				}
			}
			const RISEPel aImplicit = implicitBRDF->albedo( ri );
			const RISEPel aExplicit = explicitBRDF->albedo( ri );
			for( int c = 0; c < 3; ++c ) {
				if( aImplicit[(unsigned int)c] != aExplicit[(unsigned int)c] ) allMatch = false;
			}
		}
		Check( allMatch, "defaults: MONEY ASSERTION -- omitted beta_m/beta_n/alpha/ior give BIT-IDENTICAL "
		       "value()/albedo() to explicit 0.3/0.3/2.0/1.55 across several (theta,phi,h) cells" );
	}

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 3 -- tier exclusivity at parse time: zero tiers, and two-or-more tiers.
//////////////////////////////////////////////////////////////////////

void TestZeroTiersRejected()
{
	std::cout << "Test: zero colour tiers bound is a parse-time rejection naming all three options" << std::endl;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	std::string capturedOutput;
	const bool ok = ParseBodyCapturing( "zero_tier", HairMaterialChunk( "m_zero" ), *job, capturedOutput );
	Check( !ok, "zero-tier: fixture is REJECTED (parse fails)" );
	Check( capturedOutput.find( "hair_material" ) != std::string::npos &&
	       capturedOutput.find( "`m_zero`" ) != std::string::npos &&
	       capturedOutput.find( "color" ) != std::string::npos &&
	       capturedOutput.find( "sigma_a" ) != std::string::npos &&
	       capturedOutput.find( "eumelanin" ) != std::string::npos &&
	       capturedOutput.find( "pheomelanin" ) != std::string::npos &&
	       capturedOutput.find( "got 0 bound" ) != std::string::npos,
	       "zero-tier: MONEY ASSERTION -- the diagnostic names `m_zero` and all three options "
	       "(color, sigma_a, eumelanin/pheomelanin) and reports `got 0 bound`" );

	safe_release( job );
}

void TestTwoTiersRejected()
{
	std::cout << "Test: two-or-more colour tiers bound is a parse-time rejection" << std::endl;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	std::string body = UniformColorPainter( "pc2" ) + ScalarPainter( "ps2", 0.5 ) +
		HairMaterialChunk( "m_two", "pc2", "ps2" );	// color AND sigma_a both bound
	std::string capturedOutput;
	const bool ok = ParseBodyCapturing( "two_tier", body, *job, capturedOutput );
	Check( !ok, "two-tier: fixture is REJECTED (parse fails)" );
	Check( capturedOutput.find( "hair_material" ) != std::string::npos &&
	       capturedOutput.find( "`m_two`" ) != std::string::npos &&
	       capturedOutput.find( "got 2 bound" ) != std::string::npos,
	       "two-tier: MONEY ASSERTION -- the diagnostic names `m_two` and reports `got 2 bound` "
	       "(color=yes, sigma_a=yes)" );

	// A melanin pair (both eumelanin AND pheomelanin) together with `color`
	// is ALSO two tiers (melanin counts as ONE, but color+melanin is two) --
	// checked separately so the "melanin pair == one tier" rule is proven
	// from BOTH directions (Case 1 above already proves each of eumelanin-
	// alone / pheomelanin-alone / both-together individually registers).
	IJobPriv* job2 = nullptr;
	if( !RISE_CreateJobPriv( &job2 ) || !job2 ) { Check( false, "job2 created" ); return; }
	std::string body2 = UniformColorPainter( "pc3" ) + ScalarPainter( "peu3", 1.0 ) + ScalarPainter( "pph3", 0.5 ) +
		HairMaterialChunk( "m_colormelanin", "pc3", 0, "peu3", "pph3" );
	std::string capturedOutput2;
	const bool ok2 = ParseBodyCapturing( "color_plus_melanin", body2, *job2, capturedOutput2 );
	Check( !ok2, "color+melanin: fixture is REJECTED (color tier + melanin tier = 2)" );
	Check( capturedOutput2.find( "got 2 bound" ) != std::string::npos,
	       "color+melanin: diagnostic reports `got 2 bound` (proves melanin pair counts as ONE tier, "
	       "not two, since color=yes + melanin=yes = 2, not 3)" );

	safe_release( job );
	safe_release( job2 );
}

//////////////////////////////////////////////////////////////////////
// 4 -- a named scalar_painter binding for beta_m actually changes
// behaviour (not just "the chunk parses").
//////////////////////////////////////////////////////////////////////

void TestNamedScalarPainterBindingChangesBehaviour()
{
	std::cout << "Test: a named scalar_painter bound to beta_m actually reaches the model" << std::endl;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created" ); return; }

	std::string body;
	body += ScalarPainter( "eu_bm", 1.0 );
	body += ScalarPainter( "bm_default", 0.3 );
	body += ScalarPainter( "bm_named", 0.6 );
	body += HairMaterialChunk( "m_bm_default", 0, 0, "eu_bm", 0, "bm_default" );
	body += HairMaterialChunk( "m_bm_named",   0, 0, "eu_bm", 0, "bm_named" );

	const bool ok = ParseBodyInto( "bm_binding", body, *job );
	Check( ok, "beta_m binding: both fixtures parse" );

	const HairBRDF* defaultBRDF = FetchHairBRDF( *job, "m_bm_default" );
	const HairBRDF* namedBRDF   = FetchHairBRDF( *job, "m_bm_named" );
	Check( defaultBRDF != 0 && namedBRDF != 0, "beta_m binding: both HairBRDFs retrievable" );

	if( defaultBRDF && namedBRDF ) {
		// A near-grazing longitudinal angle is where M_p's roughness
		// dependence is most pronounced -- if the named 0.6 binding did
		// not reach the model, this cell would be bit-identical to the
		// 0.3 default (as TestDefaultsApplied proves it IS for the
		// truly-omitted case).
		const RayIntersectionGeometric ri = MakeFibreHit( 0.6, 1.1, 0.1 );
		const Vector3 wi = Vector3Ops::Normalize( Vector3( 0.5, 0.6, -0.3 ) );
		const RISEPel vDefault = defaultBRDF->value( wi, ri );
		const RISEPel vNamed   = namedBRDF->value( wi, ri );
		bool anyDiffer = false;
		for( int c = 0; c < 3; ++c ) {
			if( vDefault[(unsigned int)c] != vNamed[(unsigned int)c] ) anyDiffer = true;
		}
		std::cout << "    value(default beta_m=0.3) = (" << vDefault[0] << "," << vDefault[1] << "," << vDefault[2]
		          << ")  value(named beta_m=0.6) = (" << vNamed[0] << "," << vNamed[1] << "," << vNamed[2] << ")" << std::endl;
		Check( anyDiffer, "beta_m binding: MONEY ASSERTION -- the named scalar_painter (0.6) produces a "
		       "DIFFERENT value() than the 0.3 default, proving the binding reaches HairScatteringBase::Resolve" );
	}

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 5 -- unknown / mistyped painter name diagnostics on a scalar slot.
//////////////////////////////////////////////////////////////////////

void TestUnknownAndIPainterBoundDiagnostics()
{
	std::cout << "Test: beta_m bound to an IPainter chunk / to an unknown name gets the established diagnostics" << std::endl;

	// (a) beta_m bound to a name that resolves to a legacy IPainter chunk.
	{
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (a)" ); return; }
		std::string body = ScalarPainter( "eu_ip", 1.0 ) + UniformColorPainter( "legacy_ip" ) +
			HairMaterialChunk( "m_ipainter", 0, 0, "eu_ip", 0, /*beta_m*/ "legacy_ip" );
		std::string capturedOutput;
		const bool ok = ParseBodyCapturing( "bound_ipainter", body, *job, capturedOutput );
		Check( !ok, "IPainter-bound beta_m: fixture is REJECTED" );
		Check( capturedOutput.find( "hair_material" ) != std::string::npos &&
		       capturedOutput.find( "`beta_m`" ) != std::string::npos &&
		       capturedOutput.find( "bound to" ) != std::string::npos &&
		       capturedOutput.find( "IPainter" ) != std::string::npos &&
		       capturedOutput.find( "legacy_ip" ) != std::string::npos,
		       "IPainter-bound beta_m: MONEY ASSERTION -- the established \"bound to `IPainter` chunk\" "
		       "diagnostic fires, naming `beta_m` and `legacy_ip`" );
		safe_release( job );
	}

	// (b) beta_m bound to a name that resolves to nothing at all.
	{
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, "job created (b)" ); return; }
		std::string body = ScalarPainter( "eu_unk", 1.0 ) +
			HairMaterialChunk( "m_unknown", 0, 0, "eu_unk", 0, /*beta_m*/ "totally_unregistered_name" );
		std::string capturedOutput;
		const bool ok = ParseBodyCapturing( "unknown_name", body, *job, capturedOutput );
		Check( !ok, "unknown beta_m name: fixture is REJECTED" );
		Check( capturedOutput.find( "hair_material" ) != std::string::npos &&
		       capturedOutput.find( "`beta_m`" ) != std::string::npos &&
		       capturedOutput.find( "totally_unregistered_name" ) != std::string::npos &&
		       capturedOutput.find( "neither a registered scalar_painter nor an inline" ) != std::string::npos,
		       "unknown beta_m name: MONEY ASSERTION -- the established \"neither a registered "
		       "scalar_painter nor an inline numeric literal\" diagnostic fires, naming `beta_m` and the value" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== HairMaterialChunkTest ===" << std::endl;

	TestEachTierParses();
	TestDefaultsApplied();
	TestZeroTiersRejected();
	TestTwoTiersRejected();
	TestNamedScalarPainterBindingChangesBehaviour();
	TestUnknownAndIPainterBoundDiagnostics();

	std::cout << "=== Results: " << passCount << " passed, " << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
