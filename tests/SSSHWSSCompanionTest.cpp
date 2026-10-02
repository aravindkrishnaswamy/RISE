//////////////////////////////////////////////////////////////////////
//
//  SSSHWSSCompanionTest.cpp - DL-357 regression: under hero-wavelength
//    spectral sampling (`hwss TRUE`), BDPT and VCM must price a
//    subsurface jump at every companion wavelength, not broadcast the
//    hero's.
//
//  THE DEFECT
//
//    The generators fold the jump's weight (diffusion: Rd(r) Ft(exit) /
//    pdfSurface; random walk: the walk weight) into the BSSRDF entry
//    vertex's throughput at the HERO wavelength, and store the exit hit
//    delta.  `BDPTIntegrator::RecomputeSubpathThroughputNM` skipped both,
//    so every companion inherited the hero's spatial profile / walk
//    weight and subsurface materials rendered GREY under `hwss TRUE`.
//
//  THE FIX
//
//    Diffusion: re-priced exactly (the entry density is wavelength-
//    independent), ratio [Rd(r;lc) Ft(cos;lc)] / [Rd(r;lh) Ft(cos;lh)].
//    Random walk: secondaries terminated (the walk was sampled from the
//    hero's coefficients and is not recorded), DL-126/DL-201 accounting.
//
//  WHAT IS GATED
//
//    A chromatic sphere in a white furnace, once with diffusion SSS and
//    once with random-walk SSS.  For BDPT and VCM, each channel of the
//    `hwss TRUE` render must match the same integrator's `hwss FALSE`
//    render, and its B/R ratio must match PT spectral's.  Each
//    configuration is the mean of kRepeats salted renders
//    (SobolSamplerTestHooks::ValueSalt + pinned std::srand); bands are
//    measured (docs/DEBT_LEDGER.md DL-357).
//
//    Usage: [--mlt]   (adds printed-only MLT spectral rows)
//
//  Author: RISE debt-cleanup, slice `debt-dl357`
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	int passCount = 0;
	int failCount = 0;
	unsigned int g_renderIndex = 0;
	const unsigned int kSeedBase = 357000u;
	const uint32_t kSaltTag = 0x357u;
	const int kRepeats = 4;

	void Check( const bool condition, const std::string& label )
	{
		if( condition ) {
			passCount++;
		} else {
			failCount++;
			std::cout << "  FAIL: " << label << std::endl;
		}
	}

	class CapturingRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
		CapturingRasterizerOutput() {}
	protected:
		virtual ~CapturingRasterizerOutput() {}
	public:
		virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
		virtual void OutputImage( const IRasterImage& image, const Rect*, const unsigned int ) override
		{
			pixels.resize( size_t( image.GetWidth() ) * image.GetHeight() );
			for( unsigned int y = 0; y < image.GetHeight(); y++ )
				for( unsigned int x = 0; x < image.GetWidth(); x++ )
					pixels[size_t( y ) * image.GetWidth() + x] = image.GetPEL( x, y );
		}
	};

	// Diffusion: the ledger row's fixture.  Random walk: RandomWalkSSS-
	// SpectralColourTest's chromatic medium.
	std::string DiffusionMaterial()
	{
		return "subsurfacescattering_material\n{\n\tname subject\n\tior 1.3\n\tabsorption 0.4 0.1 0.02\n"
		       "\tscattering 4\n\tg 0\n\troughness 0\n}\n\n";
	}

	std::string RandomWalkMaterial()
	{
		return "randomwalk_sss_material\n{\n\tname subject\n\tior 1.3\n\tabsorption 3.0 0.5 0.02\n"
		       "\tscattering 4\n\tg 0\n\troughness 0\n\tmax_bounces 256\n}\n\n";
	}

	// Control: an ordinary chromatic Lambertian sphere (no subsurface jump),
	// carrying whatever hwss TRUE-vs-FALSE estimator offset exists without
	// DL-357.
	std::string LambertianMaterial()
	{
		return "uniformcolor_painter\n{\n\tname tint\n\tcolor 0.25 0.55 0.9\n}\n\n"
		       "lambertian_material\n{\n\tname subject\n\treflectance tint\n}\n\n";
	}

	std::string Rasterizer( const char* kind, const int spp, const bool hwss )
	{
		std::ostringstream s;
		s << kind << "\n{\n";
		if( std::strstr( kind, "mlt" ) ) {
			// MLT has no `samples` and no radiance map: lit by the quad
			// BuildScene adds for it.
			s << "\tmax_eye_depth 16\n\tmax_light_depth 16\n\tbootstrap_samples 20000\n\tchains 64\n"
			  << "\tmutations_per_pixel " << spp << "\n";
		} else {
			s << "\tsamples " << spp << "\n";
			if( std::strstr( kind, "bdpt" ) || std::strstr( kind, "vcm" ) ) {
				s << "\tmax_eye_depth 16\n\tmax_light_depth 16\n";
			} else {
				s << "\trr_min_depth 8\n";
			}
			s << "\tradiance_map env\n\tradiance_background FALSE\n";
		}
		// num_wavelengths 160: hwss FALSE samples a discrete left-endpoint
		// grid whose default 10 nodes misreport the red channel by ~6% on a
		// plain chromatic Lambertian (DL-219's quadrature note); matched
		// quadrature makes the hwss TRUE/FALSE comparison measure transport.
		s << "\thwss " << ( hwss ? "TRUE" : "FALSE" ) << "\n\tnum_wavelengths 160\n";
		s << "\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n";
		return s.str();
	}

	std::string BuildScene( const std::string& material, const std::string& rasterizer )
	{
		std::ostringstream s;
		s << "RISE ASCII SCENE 7\n";
		s << "film\n{\n\twidth 32\n\theight 32\n}\n\n";
		s << "pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n";
		s << "uniformcolor_painter\n{\n\tname env\n\tcolor 1 1 1\n}\n\n";
		s << material;
		s << "sphere_geometry\n{\n\tname subject_geo\n\tradius 0.8\n}\n\n";
		s << "standard_object\n{\n\tname subject_obj\n\tgeometry subject_geo\n\tmaterial subject\n}\n\n";
		if( rasterizer.find( "mlt" ) != std::string::npos ) {
			s << "lambertian_luminaire_material\n{\n\tname light_mat\n\texitance env\n\tscale 1.0\n\tmaterial none\n}\n\n";
			s << "clippedplane_geometry\n{\n\tname light_geo\n\tpta -3 3 -3\n\tptb 3 3 -3\n\tptc 3 3 3\n\tptd -3 3 3\n}\n\n";
			s << "standard_object\n{\n\tname light_obj\n\tgeometry light_geo\n\tmaterial light_mat\n}\n\n";
		}
		s << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		s << rasterizer;
		s << "file_rasterizeroutput\n{\n\tpattern rendered/dl357_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return s.str();
	}

	bool RenderOnce( const std::string& scene, double out[3] )
	{
		char path[512];
		std::snprintf( path, sizeof( path ), "/tmp/dl357_sss_%d.RISEscene", static_cast<int>( ::getpid() ) );
		{
			std::ofstream ofs( path );
			if( !ofs.is_open() ) return false;
			ofs << scene;
		}
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { std::remove( path ); return false; }
		bool ok = job->LoadAsciiSceneViaCst( path );
		std::remove( path );
		if( !ok ) { safe_release( job ); return false; }
		job->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "dl357 capture" );
		job->GetRasterizer()->AddRasterizerOutput( cap );
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( kSeedBase + g_renderIndex, kSaltTag ) );
		std::srand( kSeedBase + g_renderIndex++ );
		ok = job->Rasterize() && !cap->pixels.empty();
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		out[0] = out[1] = out[2] = 0;
		if( ok ) {
			for( const RISEColor& c : cap->pixels ) {
				out[0] += c.base.r * c.a; out[1] += c.base.g * c.a; out[2] += c.base.b * c.a;
			}
			for( int k = 0; k < 3; ++k ) {
				out[k] /= double( cap->pixels.size() );
				ok = ok && std::isfinite( out[k] ) && out[k] > 0;
			}
		}
		safe_release( cap );
		safe_release( job );
		return ok;
	}

	struct Stat { double ch[3]; double chSd[3]; double br; double brSd; bool ok; };

	// Mean over kRepeats salted renders, with the standard error of each mean.
	Stat Measure( const std::string& material, const char* kind, const int spp, const bool hwss, const char* label )
	{
		Stat st = {};
		st.ok = true;
		double sum[4] = { 0, 0, 0, 0 }, sum2[4] = { 0, 0, 0, 0 };
		for( int n = 0; n < kRepeats; ++n ) {
			double c[3];
			if( !RenderOnce( BuildScene( material, Rasterizer( kind, spp, hwss ) ), c ) ) { st.ok = false; return st; }
			const double v[4] = { c[0], c[1], c[2], c[2] / c[0] };
			for( int k = 0; k < 4; ++k ) { sum[k] += v[k]; sum2[k] += v[k] * v[k]; }
		}
		double m[4], se[4];
		for( int k = 0; k < 4; ++k ) {
			m[k] = sum[k] / kRepeats;
			const double var = ( sum2[k] - kRepeats * m[k] * m[k] ) / ( kRepeats - 1 );
			se[k] = std::sqrt( var > 0 ? var / kRepeats : 0 );
		}
		for( int k = 0; k < 3; ++k ) { st.ch[k] = m[k]; st.chSd[k] = se[k]; }
		st.br = m[3]; st.brSd = se[3];
		std::printf( "    %-24s R %.5f G %.5f B %.5f  B/R %.4f +/- %.4f  (se %.2f%% %.2f%% %.2f%%)\n",
			label, m[0], m[1], m[2], m[3], se[3],
			100 * se[0] / m[0], 100 * se[1] / m[1], 100 * se[2] / m[2] );
		return st;
	}

	void RunKind( const char* title, const std::string& material, const int spp,
		const double chBand, const double brBand, const bool withMlt )
	{
		std::cout << title << " (32x32, " << spp << " spp, mean of " << kRepeats << " salted renders)" << std::endl;
		const Stat pt = Measure( material, "pathtracing_spectral_rasterizer", spp, false, "PT spectral" );
		Check( pt.ok, std::string( title ) + ": PT spectral rendered" );
		if( !pt.ok ) return;

		const char* kinds[2] = { "bdpt_spectral_rasterizer", "vcm_spectral_rasterizer" };
		const char* names[2] = { "BDPT", "VCM" };
		for( int i = 0; i < 2; ++i ) {
			const Stat off = Measure( material, kinds[i], spp, false, ( std::string( names[i] ) + " hwss FALSE" ).c_str() );
			const Stat on  = Measure( material, kinds[i], spp, true,  ( std::string( names[i] ) + " hwss TRUE" ).c_str() );
			Check( off.ok && on.ok, std::string( title ) + ": " + names[i] + " rendered" );
			if( !off.ok || !on.ok ) continue;
			char buf[256];
			for( int k = 0; k < 3; ++k ) {
				const double r = on.ch[k] / off.ch[k];
				std::snprintf( buf, sizeof( buf ), "%s: %s hwss TRUE/FALSE channel %d = %.4f (band +/-%.3f)",
					title, names[i], k, r, chBand );
				std::cout << "      " << buf << std::endl;
				Check( std::fabs( r - 1.0 ) <= chBand, buf );
			}
			const double rb = on.br / pt.br;
			std::snprintf( buf, sizeof( buf ), "%s: %s hwss TRUE B/R over PT spectral B/R = %.4f (band +/-%.3f)",
				title, names[i], rb, brBand );
			std::cout << "      " << buf << std::endl;
			Check( std::fabs( rb - 1.0 ) <= brBand, buf );
		}
		if( withMlt ) {
			Measure( material, "mlt_spectral_rasterizer", spp, false, "MLT hwss FALSE (printed)" );
			Measure( material, "mlt_spectral_rasterizer", spp, true,  "MLT hwss TRUE (printed)" );
		}
	}
}

int main( int argc, char** argv )
{
	const bool withMlt = argc >= 2 && std::strcmp( argv[1], "--mlt" ) == 0;
	if( argc >= 2 && std::strcmp( argv[1], "--control" ) == 0 ) {
		RunKind( "lambertian control", LambertianMaterial(), 256, 0.03, 0.03, false );
		return 0;
	}
	// Bands measured on the fixed build -- see docs/DEBT_LEDGER.md DL-357.
	RunKind( "diffusion", DiffusionMaterial(), 256, 0.03, 0.03, withMlt );
	RunKind( "random walk", RandomWalkMaterial(), 256, 0.06, 0.08, withMlt );
	std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
