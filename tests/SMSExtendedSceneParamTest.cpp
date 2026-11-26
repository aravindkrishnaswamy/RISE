//////////////////////////////////////////////////////////////////////
//
//  SMSExtendedSceneParamTest.cpp - the `sms_extended` scene parameter.
//
//  Extended SMS (docs/SMS_EXTENDED_DESIGN.md) used to be reachable only
//  through ManifoldSolverConfig::extendedMode, set by tests.  The scene
//  parameter `sms_extended` on pathtracing_{pel,spectral}_rasterizer
//  forwards SMSConfig::extended -> RISE_API_CreatePathTracing*RasterizerEx
//  -> extendedMode.  This test pins:
//
//   A. Parse: `sms_extended TRUE` yields extendedMode == true in the
//      built rasterizer's solver; absent or FALSE yields false (Pel and
//      spectral chunks).
//   B. Render: a scene with `sms_extended TRUE` renders BIT-IDENTICALLY
//      to the same scene loaded without it whose integrator is then
//      rebuilt from its own solver config with extendedMode set
//      internally -- i.e. the parameter changes exactly that one flag.
//      A control requires the extended render to differ from legacy, so
//      the fixture actually exercises the extended estimator.
//
//////////////////////////////////////////////////////////////////////

#include "SMSRenderTestSupport.h"
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Rendering/PathTracingSpectralRasterizer.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Job.h"

#include <functional>

// Protected-member access without instantiating anything: a pointer to
// member formed in a derived class may be applied to a base object.
struct PelPeek : PathTracingPelRasterizer {
	static PathTracingIntegrator*& Integrator( PathTracingPelRasterizer& r ) {
		return r.*( &PelPeek::pIntegrator );
	}
};
struct SpectralPeek : PathTracingSpectralRasterizer {
	static PathTracingIntegrator*& Integrator( PathTracingSpectralRasterizer& r ) {
		return r.*( &SpectralPeek::pIntegrator );
	}
};
struct IntegratorPeek : PathTracingIntegrator {
	static const StabilityConfig& Stability( const PathTracingIntegrator& i ) {
		return i.*( &IntegratorPeek::stabilityConfig );
	}
	static unsigned int MaxDepth( const PathTracingIntegrator& i ) {
		return i.*( &IntegratorPeek::mMaxPathDepth );
	}
	static bool IndirectOnly( const PathTracingIntegrator& i ) {
		return i.*( &IntegratorPeek::mIndirectOnly );
	}
	static bool Clay( const PathTracingIntegrator& i ) {
		return i.*( &IntegratorPeek::mClayOverride );
	}
};

// A glass ball above a floor, lit by an omni light: extended mode's
// estimator A (delta light).  Small so the render is cheap.
static std::string Scene( const char* rasterizer, const char* extendedLine )
{
	std::string s =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n";
	s += std::string( rasterizer ) + "\n{\n"
		"\tsamples 4\n"
		"\toidn_denoise FALSE\n"
		"\tprogressive_rendering FALSE\n"
		"\tpixel_filter box\n"
		"\tsms_enabled TRUE\n"
		"\tsms_max_chain_depth 4\n";
	s += extendedLine;
	s += "}\n"
		"film\n{\n\twidth 16\n\theight 12\n}\n"
		"pinhole_camera\n{\n\tlocation 0 2.0 3\n\tlookat 0 0.2 0\n\tup 0 1 0\n\tfov 45.0\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.8 0.75 0.65\n}\n"
		"uniformcolor_painter\n{\n\tname pnt_glass\n\tcolor 1.0 1.0 1.0\n}\n"
		"lambertian_material\n{\n\tname floor_mat\n\treflectance pnt_floor\n}\n"
		"perfectrefractor_material\n{\n\tname glass_mat\n\trefractance pnt_glass\n\tior 1.5\n}\n"
		"sphere_geometry\n{\n\tname sphere_geom\n\tradius 0.3\n}\n"
		"clippedplane_geometry\n{\n\tname floor_geom\n\tpta -4.0 0.0 -4.0\n\tptb -4.0 0.0 4.0\n\tptc 4.0 0.0 4.0\n\tptd 4.0 0.0 -4.0\n}\n"
		"standard_object\n{\n\tname floor\n\tgeometry floor_geom\n\tmaterial floor_mat\n}\n"
		"standard_object\n{\n\tname glass_ball\n\tgeometry sphere_geom\n\tposition 0 0.6 0\n\tmaterial glass_mat\n}\n"
		"omni_light\n{\n\tname lamp\n\tpower 20\n\tcolor 1 1 1\n\tposition 0 1.6 0\n}\n";
	return s;
}

// Load `text` through the CST path, optionally mutate the job, render
// with the SMSRenderTestSupport salting and capture.
static RenderResult LoadAndRender( const std::string& text, const char* tag,
	const std::function<bool( IJobPriv& )>& prepare )
{
	RenderResult r{ {}, 0, 0, 0, 0, false };
	if( !ConfigureTestWorker() ) return r;
	const std::string path = TestTempPath( "smsextparam_" + std::string( tag ) + "_" + std::to_string( ::getpid() ) + ".RISEscene" );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return r;
		ofs << text;
	}
	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) { std::remove( path.c_str() ); return r; }
	if( pJob->LoadAsciiSceneViaCst( path.c_str() ) && prepare( *pJob ) ) {
		pJob->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
		pJob->GetRasterizer()->AddRasterizerOutput( pCap );
		const unsigned int seed = g_seedBase;   // identical salt for every render
		std::srand( seed );
		GlobalRNG() = RandomNumberGenerator( seed );
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( seed, kSaltTag ) );
		const bool ok = pJob->Rasterize();
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		if( ok && !pCap->pixels.empty() ) {
			double sum = 0;
			for( const RISEColor& c : pCap->pixels ) sum += ( c.base.r + c.base.g + c.base.b ) / 3.0;
			r.mean = sum / double( pCap->pixels.size() );
			r.hash = HashPixels( pCap->pixels );
			r.pixels = pCap->pixels;
			r.ok = std::isfinite( r.mean );
		}
		safe_release( pCap );
	}
	safe_release( pJob );
	std::remove( path.c_str() );
	return r;
}

static PathTracingIntegrator* JobIntegrator( IJobPriv& job, bool spectral )
{
	Job* pConcrete = dynamic_cast<Job*>( &job );
	if( !pConcrete ) return nullptr;
	IRasterizer* pR = pConcrete->GetRasterizer();
	if( spectral ) {
		auto* p = dynamic_cast<PathTracingSpectralRasterizer*>( pR );
		return p ? SpectralPeek::Integrator( *p ) : nullptr;
	}
	auto* p = dynamic_cast<PathTracingPelRasterizer*>( pR );
	return p ? PelPeek::Integrator( *p ) : nullptr;
}

static void TestParse()
{
	std::cout << "A. parse" << std::endl;
	struct Row { const char* chunk; bool spectral; const char* line; bool expect; const char* label; };
	const Row rows[] = {
		{ "pathtracing_pel_rasterizer",      false, "",                     false, "pel default" },
		{ "pathtracing_pel_rasterizer",      false, "\tsms_extended FALSE\n", false, "pel FALSE" },
		{ "pathtracing_pel_rasterizer",      false, "\tsms_extended TRUE\n",  true,  "pel TRUE" },
		{ "pathtracing_spectral_rasterizer", true,  "",                     false, "spectral default" },
		{ "pathtracing_spectral_rasterizer", true,  "\tsms_extended TRUE\n",  true,  "spectral TRUE" },
	};
	for( const Row& row : rows ) {
		const std::string text = Scene( row.chunk, row.line );
		const std::string path = TestTempPath( "smsextparam_parse_" + std::to_string( ::getpid() ) + ".RISEscene" );
		{ std::ofstream ofs( path ); ofs << text; }
		IJobPriv* pJob = nullptr;
		bool got = !row.expect, found = false;
		if( RISE_CreateJobPriv( &pJob ) && pJob && pJob->LoadAsciiSceneViaCst( path.c_str() ) ) {
			PathTracingIntegrator* pI = JobIntegrator( *pJob, row.spectral );
			if( pI && pI->GetSolver() ) {
				found = true;
				got = pI->GetSolver()->GetConfig().extendedMode;
			}
		}
		safe_release( pJob );
		std::remove( path.c_str() );
		std::cout << "  " << row.label << ": solver " << ( found ? "found" : "MISSING" )
			<< ", extendedMode=" << got << std::endl;
		Check( found, std::string( row.label ) + ": rasterizer exposes an SMS solver" );
		Check( got == row.expect, std::string( row.label ) + ": extendedMode matches the parameter" );
	}
}

static void TestRenderParity()
{
	std::cout << "B. render parity (scene param vs internal flag)" << std::endl;
	const auto noop = []( IJobPriv& ) { return true; };
	const RenderResult viaParam = LoadAndRender(
		Scene( "pathtracing_pel_rasterizer", "\tsms_extended TRUE\n" ), "param", noop );

	// Same scene WITHOUT the parameter; rebuild the integrator from its own
	// solver config with extendedMode set internally.
	const RenderResult viaInternal = LoadAndRender(
		Scene( "pathtracing_pel_rasterizer", "" ), "internal",
		[]( IJobPriv& job ) {
			Job* pConcrete = dynamic_cast<Job*>( &job );
			auto* pR = pConcrete ? dynamic_cast<PathTracingPelRasterizer*>( pConcrete->GetRasterizer() ) : nullptr;
			if( !pR ) return false;
			PathTracingIntegrator*& slot = PelPeek::Integrator( *pR );
			if( !slot || !slot->GetSolver() ) return false;
			ManifoldSolverConfig cfg = slot->GetSolver()->GetConfig();
			if( cfg.extendedMode ) return false;      // must start legacy
			cfg.extendedMode = true;
			PathTracingIntegrator* pNew = new PathTracingIntegrator( cfg, IntegratorPeek::Stability( *slot ) );
			pNew->SetMaxPathDepth( IntegratorPeek::MaxDepth( *slot ) );
			pNew->SetIndirectOnly( IntegratorPeek::IndirectOnly( *slot ) );
			pNew->SetClayOverride( IntegratorPeek::Clay( *slot ) );
			safe_release( slot );
			slot = pNew;
			return true;
		} );

	const RenderResult legacy = LoadAndRender(
		Scene( "pathtracing_pel_rasterizer", "" ), "legacy", noop );

	std::cout << "  param   ok=" << viaParam.ok << " mean=" << viaParam.mean << " hash=" << viaParam.hash << std::endl;
	std::cout << "  internal ok=" << viaInternal.ok << " mean=" << viaInternal.mean << " hash=" << viaInternal.hash << std::endl;
	std::cout << "  legacy  ok=" << legacy.ok << " mean=" << legacy.mean << " hash=" << legacy.hash << std::endl;
	Check( viaParam.ok && viaInternal.ok && legacy.ok, "all three renders completed" );
	Check( viaParam.ok && viaInternal.ok && viaParam.hash == viaInternal.hash,
		"sms_extended TRUE is bit-identical to the internal extendedMode flag" );
	Check( viaParam.ok && legacy.ok && viaParam.hash != legacy.hash,
		"control: extended render differs from legacy SMS (fixture exercises extended mode)" );
}

int main()
{
	std::cout << "SMSExtendedSceneParamTest" << std::endl;
	TestParse();
	TestRenderParity();
	std::cout << "\nSMSExtendedSceneParamTest: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
