//////////////////////////////////////////////////////////////////////
//
//  VCMVolumeMergeFixture.h - Shared fog-box / global-fog scene builders
//    and in-memory linear image capture for VCMVolumeMergeTest and
//    related medium-transport tests (DL-469 / DL-474 / DL-475).
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_TESTS_VCM_VOLUME_MERGE_FIXTURE_H
#define RISE_TESTS_VCM_VOLUME_MERGE_FIXTURE_H

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
	#ifndef getpid
		#define getpid _getpid
	#endif
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
#include "../src/Library/Interfaces/IOptions.h"
#include "../src/Library/Rendering/VCMRasterizerBase.h"

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

	inline void Check( bool ok, const std::string& what )
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

	const char* const kOutput =
		"file_rasterizeroutput\n{\n\tpattern rendered/dl469_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";

	inline std::string Rasterizer( bool vcm, unsigned int spp, const char* extra )
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
	inline std::string FogBoxScene( bool vcm, unsigned int spp, bool sphere )
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

	//! Row E: A's fog box with a Lambertian floor inside it, so one store
	//! holds surface vertices (which set the firefly-clamp thresholds) and
	//! medium vertices (the fog seen through the wall is merge-only).
	inline std::string FogFloorBoxScene( bool vcm, unsigned int spp )
	{
		std::string s = FogBoxScene( vcm, spp, false );
		const std::string floorChunks =
			"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n\tcolorspace Rec709RGB_Linear\n}\n\n"
			"lambertian_material\n{\n\tname fl\n\treflectance grey\n}\n\n"
			"clippedplane_geometry\n{\n\tname floor_geo\n\tpta -1.4 -1.4 1.4\n\tptb 1.4 -1.4 1.4\n\tptc 1.4 -1.4 -1.4\n\tptd -1.4 -1.4 -1.4\n\tdoublesided FALSE\n}\n\n"
			"standard_object\n{\n\tname floor\n\tgeometry floor_geo\n\tmaterial fl\n}\n\n";
		const std::size_t at = s.find( "omni_light" );
		s.insert( at, floorChunks );
		return s;
	}

	//! Row B: global medium over a floor; VCM surface merging forced on.
	inline std::string GlobalFogScene( bool vcm, unsigned int spp )
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

	inline std::string WriteScene( const std::string& text, const char* tag )
	{
		std::ostringstream p;
		p << "tests/dl469_" << tag << "_" << getpid() << ".RISEscene";
		std::ofstream f( p.str().c_str() );
		if( !f ) return std::string();
		f << text;
		return p.str();
	}

	inline double RenderMean( const std::string& text, unsigned int seed )
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
	inline Stats Summarize( const std::vector<double>& v )
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
	inline double RatioRow( const std::string& label, const std::string& test, const std::string& ref,
		double expected, double band, unsigned int trials, unsigned int& seed, const std::string& only,
		double qTest = -1, double qRef = -1 )
	{
		if( !only.empty() && label.find( only ) == std::string::npos ) return -1;
		std::vector<double> tv, rv, ratios, differences;
		bool valid = true;
		for( unsigned int t = 0; t < trials; ++t ) {
			const unsigned int s = seed++;
			if( qTest >= 0 ) VCMRasterizerBase::TestVolumeStoreProbabilityOverride().store( qTest );
			const double a = RenderMean( test, s );
			if( qRef >= 0 ) VCMRasterizerBase::TestVolumeStoreProbabilityOverride().store( qRef );
			const double b = RenderMean( ref, s );
			if( !( a > 0 ) || !( b > 0 ) ) valid = false;
			tv.push_back( a ); rv.push_back( b ); ratios.push_back( a / b ); differences.push_back( a - expected * b );
		}
		Check( valid, label + ": every render finite and non-black" );
		if( !valid ) return -1;
		const Stats st = Summarize( tv ), sr = Summarize( rv ), sq = Summarize( ratios );
		const double ratio = st.mean / sr.mean;
		std::cout << std::setprecision( 6 ) << "  " << label << ": test " << st.mean << " +/- " << st.sd
			<< "  ref " << sr.mean << " +/- " << sr.sd << "  ratio " << ratio << " (expected " << expected
			<< ", per-pair sd " << sq.sd << ", band " << band << ")" << std::endl;
		if( band < 0 ) {
			const Stats diff = Summarize( differences );
			const double se = diff.sd / std::sqrt( double( trials ) );
			std::cout << "  paired difference " << diff.mean << " sd " << diff.sd << " n " << trials << " 3SE " << 3 * se << std::endl;
			Check( std::fabs( diff.mean ) <= 3 * se, label + ": paired difference within 3SE" );
		}
		if( band > 0 ) Check( std::fabs( ratio - expected ) < band, label + ": ratio within band" );
		return ratio;
	}
}

#endif
