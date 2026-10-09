//////////////////////////////////////////////////////////////////////
//
//  VCMVolumeMergeTest.cpp - DL-469: VCM merges at MEDIUM vertices too.
//
//  Before DL-469 VCM merged on surfaces only while its MIS recurrence
//  counted a merge at every medium vertex (the surface factor), so
//    A  fog behind delta walls lit by a point light and seen through
//       them -- single scattering sandwiched between delta surfaces --
//       had NO strategy at all (fog box: VCM 0.58 of PT), and
//    B  every medium path lost the MIS mass of that phantom merge
//       whenever surface merging was on (global fog, forced merge
//       radius: VCM 0.977 of PT).
//  The fix makes the counted strategy real (a 3-D volume merge with its
//  own MIS factor eta_v; VCMIntegrator.cpp "volume merging").
//
//  Rows (salted, n renders each, ratio of means, VCM / PT):
//    A  an index-1.0 perfect-refractor box carrying isotropic fog, no
//       other surface, omni light outside, camera outside.
//    B  a global absorbing/scattering medium over a Lambertian floor,
//       omni light, VCM with merge_radius 0.05 (surface merging on).
//    C  the ledger scene (tests/SMSMediumTransmittanceTest.cpp row A: a
//       Lambertian sphere inside A's box) -- printed only: its VCM
//       variance is heavy-tailed (the sphere's light reaches it only by
//       surface merging at the auto radius; per-pair sd up to 0.23 at
//       n = 8).  Measured at n = 48: 1.004 +/- 0.017 (pre-fix 0.80).
//
//  Usage: VCMVolumeMergeTest [--trials n]
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
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
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

namespace
{
	int passCount = 0;
	int failCount = 0;

	void Check( bool ok, const std::string& what )
	{
		if( ok ) { ++passCount; }
		else { ++failCount; std::cout << "  FAIL: " << what << std::endl; }
	}

	class CapturingRasterizerOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
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

	const char* kOutput =
		"file_rasterizeroutput\n{\n\tpattern rendered/dl469_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

	std::string Rasterizer( bool vcm, unsigned int spp, const char* extra )
	{
		std::ostringstream o;
		o << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		if( vcm ) {
			o << "vcm_pel_rasterizer\n{\n\tsamples " << spp
			  << "\n\tmax_eye_depth 32\n\tmax_light_depth 32\n\tpixel_filter box\n\toidn_denoise FALSE\n" << extra << "}\n\n";
		} else {
			o << "pathtracing_pel_rasterizer\n{\n\tsamples " << spp
			  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\tpathguiding FALSE\n"
			     "\tadaptive_max_samples 0\n\ttransparent_shadows TRUE\n}\n\n";
		}
		return o.str();
	}

	//! Rows A / C: fog in an index-1.0 delta box, optionally around a sphere.
	std::string FogBoxScene( bool vcm, unsigned int spp, bool sphere )
	{
		std::ostringstream o;
		o << "RISE ASCII SCENE 7\n";
		o << "film\n{\n\twidth 24\n\theight 24\n}\n\n";
		o << "pinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 18\n}\n\n";
		o << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n";
		if( sphere ) {
			o << "lambertian_material\n{\n\tname subject\n\treflectance white\n}\n\n";
			o << "sphere_geometry\n{\n\tname subject_geo\n\tradius 1\n}\n\n";
			o << "standard_object\n{\n\tname subject_obj\n\tgeometry subject_geo\n\tmaterial subject\n}\n\n";
		}
		o << "perfectrefractor_material\n{\n\tname wall_mat\n\tior 1.0\n\trefractance white\n}\n\n"
		  << "homogeneous_medium\n{\n\tname fog\n\tabsorption 0 0 0\n\tscattering 0.5 0.5 0.5\n\tphase isotropic\n}\n\n"
		  << "box_geometry\n{\n\tname wall_geo\n\twidth 3\n\theight 3\n\tdepth 3\n}\n\n"
		  << "standard_object\n{\n\tname wall\n\tgeometry wall_geo\n\tmaterial wall_mat\n\tinterior_medium fog\n}\n\n";
		o << "omni_light\n{\n\tname lgt\n\tposition 0.5 3 0.5\n\tcolor 1 1 1\n\tpower 20\n}\n\n";
		o << Rasterizer( vcm, spp, "" ) << kOutput;
		return o.str();
	}

	//! Row B: global medium over a floor; VCM surface merging forced on.
	std::string GlobalFogScene( bool vcm, unsigned int spp )
	{
		std::ostringstream o;
		o << "RISE ASCII SCENE 7\n";
		o << "film\n{\n\twidth 24\n\theight 24\n}\n\n";
		o << "pinhole_camera\n{\n\tlocation 0 1 5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30\n}\n\n";
		o << "uniformcolor_painter\n{\n\tname grey\n\tcolor 0.8 0.8 0.8\n\tcolorspace Rec709RGB_Linear\n}\n\n";
		o << "lambertian_material\n{\n\tname fl\n\treflectance grey\n}\n\n";
		o << "clippedplane_geometry\n{\n\tname floor_geo\n\tpta -3 -1 3\n\tptb 3 -1 3\n\tptc 3 -1 -3\n\tptd -3 -1 -3\n\tdoublesided FALSE\n}\n\n";
		o << "standard_object\n{\n\tname floor\n\tgeometry floor_geo\n\tmaterial fl\n}\n\n";
		o << "homogeneous_medium\n{\n\tname fog\n\tabsorption 0.05 0.05 0.05\n\tscattering 0.3 0.3 0.3\n\tphase isotropic\n}\n\n"
		  << "global_medium\n{\n\tmedium fog\n}\n\n";
		o << "omni_light\n{\n\tname lgt\n\tposition 0.5 1.5 0.5\n\tcolor 1 1 1\n\tpower 20\n}\n\n";
		o << Rasterizer( vcm, spp, "\tmerge_radius 0.05\n" ) << kOutput;
		return o.str();
	}

	std::string WriteScene( const std::string& text, const char* tag )
	{
		std::ostringstream p;
		p << "tests/dl469_" << tag << "_" << getpid() << ".RISEscene";
		std::ofstream f( p.str().c_str() );
		if( !f ) return std::string();
		f << text;
		return p.str();
	}

	double RenderMean( const std::string& text, unsigned int seed )
	{
		const std::string path = WriteScene( text, "scene" );
		if( path.empty() ) return -1;
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) { std::remove( path.c_str() ); return -1; }
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) { safe_release( job ); std::remove( path.c_str() ); return -1; }
		std::remove( path.c_str() );
		job->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "dl469 capture" );
		job->GetRasterizer()->AddRasterizerOutput( cap );
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( seed, 0x469u ) );
		std::srand( seed );
		const bool rendered = job->Rasterize();
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		double mean = -1;
		if( rendered && !cap->pixels.empty() ) {
			double sum = 0;
			bool finite = true;
			for( const RISEColor& c : cap->pixels ) {
				const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
				if( !std::isfinite( v ) ) { finite = false; break; }
				sum += v;
			}
			if( finite ) mean = sum / double( cap->pixels.size() );
		}
		safe_release( cap );
		safe_release( job );
		return mean;
	}

	struct Stats { double mean; double sd; };
	Stats Summarize( const std::vector<double>& v )
	{
		double m = 0;
		for( double x : v ) m += x;
		m /= double( v.size() );
		double var = 0;
		for( double x : v ) var += ( x - m ) * ( x - m );
		var /= double( v.size() > 1 ? v.size() - 1 : 1 );
		return { m, std::sqrt( var ) };
	}

	//! Paired salted renders; returns the ratio of means and prints it.
	//! Gates |ratio - expected| < band when band > 0.
	double RatioRow( const std::string& label, const std::string& test, const std::string& ref,
		double expected, double band, unsigned int trials, unsigned int& seed, const std::string& only )
	{
		if( !only.empty() && label.find( only ) == std::string::npos ) return -1;
		std::vector<double> tv, rv, ratios;
		bool valid = true;
		for( unsigned int t = 0; t < trials; ++t ) {
			const unsigned int s = seed++;
			const double a = RenderMean( test, s );
			const double b = RenderMean( ref, s );
			if( !( a > 0 ) || !( b > 0 ) ) valid = false;
			tv.push_back( a ); rv.push_back( b ); ratios.push_back( a / b );
		}
		Check( valid, label + ": every render finite and non-black" );
		if( !valid ) return -1;
		const Stats st = Summarize( tv ), sr = Summarize( rv ), sq = Summarize( ratios );
		const double ratio = st.mean / sr.mean;
		std::cout << std::setprecision( 6 ) << "  " << label << ": test " << st.mean << " +/- " << st.sd
			<< "  ref " << sr.mean << " +/- " << sr.sd << "  ratio " << ratio << " (expected " << expected
			<< ", per-pair sd " << sq.sd << ", band " << band << ")" << std::endl;
		if( band > 0 ) Check( std::fabs( ratio - expected ) < band, label + ": ratio within band" );
		return ratio;
	}
}

int main( int argc, char** argv )
{
	unsigned int trials = 8;
	for( int i = 1; i < argc; ++i ) {
		const std::string a( argv[i] );
		if( a == "--trials" && i + 1 < argc ) trials = static_cast<unsigned int>( std::atoi( argv[++i] ) );
	}
	if( trials < 2 ) trials = 2;
	std::cout << "=== DL-469 VCM volume merging, n=" << trials << " ===" << std::endl;
	unsigned int seed = 46900;
	const std::string only;

	// A: pre-fix 0.58 (the single-scatter share had no strategy).
	RatioRow( "A: fog box, VCM / PT",
		FogBoxScene( true, 512, false ), FogBoxScene( false, 64, false ), 1.0, 0.06, trials, seed, only );
	// B: pre-fix 0.977 (phantom medium merge in the MIS denominators).
	RatioRow( "B: global fog, forced surface merging, VCM / PT",
		GlobalFogScene( true, 64 ), GlobalFogScene( false, 128 ), 1.0, 0.015, trials, seed, only );
	// C: the ledger row; pre-fix 0.80 (printed: heavy-tailed, see header).
	RatioRow( "C: ledger fog box with sphere, VCM / PT (printed)",
		FogBoxScene( true, 1024, true ), FogBoxScene( false, 64, true ), 1.0, 0.0, trials, seed, only );

	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
