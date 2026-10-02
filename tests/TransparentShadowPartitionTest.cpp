//////////////////////////////////////////////////////////////////////
//
//  TransparentShadowPartitionTest.cpp - Regression guard for DL-344:
//    `transparent_shadows TRUE` must not count light reached through a
//    delta transmitter twice.
//
//  The `transparent_shadows` walk lets a PT NEE shadow ray pass a clear
//  specular dielectric STRAIGHT (no bend) with its Fresnel transmittance.
//  PT's BSDF-sampled continuation ALSO reaches an area light or the
//  environment through the same dielectric (it refracts, and its MIS
//  partner is reset to "none" at the delta vertex, so it lands at weight
//  1).  Letting the area/env NEE arm through as well adds a second
//  estimator of the same transport on top of a full-weight one: an
//  index-1.0 box around a white-furnace subject read 1.14-1.17x open air.
//
//  The ruling (docs/DL344_TRANSPARENT_SHADOW_PARTITION.md) is DL-05's:
//  only a DELTA light's shadow ray sees through a clear dielectric,
//  because no BSDF-sampled strategy can ever hit a delta light, so that
//  ray is the path's only estimator; area/env NEE keeps a binary shadow
//  there.  With SMS enabled, also not for a delta light SMS samples (omni,
//  spot: SMS estimates their light through a specular caster, so the
//  straight walk would double count it); a directional light, which SMS
//  never samples, keeps the walk.
//
//  Rows (all reference-free ratios of paired renders; every render is
//  salted -- SobolSamplerTestHooks::ValueSalt -- so repeats are
//  independent randomized-QMC replicates):
//    A  index-1.0 perfect-refractor box around camera AND subject, white
//       environment, transparent_shadows TRUE: boxed / open air == 1 for
//       Lambertian, random-walk SSS, diffusion SSS (PT pel) and a
//       Lambertian under PT spectral.  Red pre-fix (+11..+17 %).
//    B  the same box, an AREA emitter outside it, black environment:
//       boxed / open air == 1.  Red pre-fix.
//    C  a 1.5 perfect-refractor box around the subject only, white
//       environment: transparent_shadows TRUE / FALSE == 1 (FALSE is
//       plain PT: binary NEE, continuation refracts).  Red pre-fix.
//    D  the index-1.0 box with an OMNI light outside it: boxed / open air
//       == 1.  A control -- the feature this flag exists for (a delta
//       light seen through an index-matched transmitter, whose only
//       estimator is this shadow ray); green before AND after.
//    E1 PT + SMS, omni light, the index-1.0 box / open air: what SMS
//       returns through an invisible caster, against a reference.  Pinned
//       at 0.5 (DL-413: SMS returns half a delta light); red pre-fix (SMS
//       plus the walk, about 1.5).
//    E2 PT + SMS, omni light through the 1.5 box: flag TRUE / FALSE == 1.
//       Red pre-fix (2.46x); equal by construction after.
//    F  a DIRECTIONAL light through the 1.5 box: PT + SMS / PT, flag
//       TRUE, == 1 -- SMS never samples a directional light, so turning
//       the walk off for it under SMS (the slice's first revision) read 0.
//
//  Usage: TransparentShadowPartitionTest [--trials N] [--only <substr>]
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

	enum class Model { Lambertian, RandomWalk, Diffusion };
	enum class Integrator { PT, PTSpectral, PTSMS };
	enum class Light { Env, Area, Omni, Directional };
	enum class Wall { None, MatchedRoom, GlassAroundSubject };

	struct SceneSpec
	{
		Model model;
		Integrator integrator;
		Light light;
		Wall wall;
		bool transparentShadows;
		unsigned int samples;
	};

	std::string BuildScene( const SceneSpec& s )
	{
		std::ostringstream o;
		o << "RISE ASCII SCENE 7\n";
		o << "film\n{\n\twidth 32\n\theight 32\n}\n\n";
		o << "orthographic_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 2 2\n}\n\n";
		o << "uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n";
		switch( s.model ) {
		case Model::Lambertian:
			o << "lambertian_material\n{\n\tname subject\n\treflectance white\n}\n\n";
			break;
		case Model::RandomWalk:
			o << "randomwalk_sss_material\n{\n\tname subject\n\tior 1.5\n\tabsorption 0\n\tscattering 2\n\tg 0\n\troughness 0\n\tmax_bounces 8192\n}\n\n";
			break;
		case Model::Diffusion:
			o << "subsurfacescattering_material\n{\n\tname subject\n\tior 1.5\n\tabsorption 0\n\tscattering 200\n\tg 0\n\troughness 0\n}\n\n";
			break;
		}
		o << "sphere_geometry\n{\n\tname subject_geo\n\tradius 1\n}\n\n";
		o << "standard_object\n{\n\tname subject_obj\n\tgeometry subject_geo\n\tmaterial subject\n}\n\n";

		// The walls.  MatchedRoom: an ideal index-1.0 refractor around the
		// camera and the subject (camera at z = 4, sphere radius 1), with
		// any light outside it (y = 3 > 2).  It changes no direction, so
		// with the flag doing what it should it is invisible.
		// GlassAroundSubject: an index-1.5 refractor box enclosing only
		// the sphere; the light (y = 3) and the camera (z = 4) are outside.
		if( s.wall == Wall::MatchedRoom ) {
			o << "perfectrefractor_material\n{\n\tname wall_mat\n\tior 1.0\n\trefractance white\n}\n\n"
			  << "box_geometry\n{\n\tname wall_geo\n\twidth 4\n\theight 4\n\tdepth 10\n}\n\n"
			  << "standard_object\n{\n\tname wall\n\tgeometry wall_geo\n\tmaterial wall_mat\n}\n\n";
		} else if( s.wall == Wall::GlassAroundSubject ) {
			o << "perfectrefractor_material\n{\n\tname wall_mat\n\tior 1.5\n\trefractance white\n}\n\n"
			  << "box_geometry\n{\n\tname wall_geo\n\twidth 3\n\theight 3\n\tdepth 3\n}\n\n"
			  << "standard_object\n{\n\tname wall\n\tgeometry wall_geo\n\tmaterial wall_mat\n}\n\n";
		}

		if( s.light == Light::Area ) {
			// 2 x 2 one-sided luminaire at y = 3 facing down.
			o << "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance white\n\tscale 4.0\n\tmaterial none\n}\n\n"
			  << "clippedplane_geometry\n{\n\tname geo_emit\n\tpta -1 3 -1\n\tptb 1 3 -1\n\tptc 1 3 1\n\tptd -1 3 1\n\tdoublesided FALSE\n}\n\n"
			  << "standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n";
		} else if( s.light == Light::Omni ) {
			o << "omni_light\n{\n\tname lgt\n\tposition 0.5 3 0.5\n\tcolor 1 1 1\n\tpower 20\n}\n\n";
		} else if( s.light == Light::Directional ) {
			o << "directional_light\n{\n\tname lgt\n\tdirection 0.3 1 0.2\n\tcolor 1 1 1\n\tpower 3\n}\n\n";
		}

		o << "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
		const std::string env = ( s.light == Light::Env ) ? "\tradiance_map white\n\tradiance_scale 1\n\tradiance_background TRUE\n" : "";
		const std::string ts = std::string( "\ttransparent_shadows " ) + ( s.transparentShadows ? "TRUE" : "FALSE" ) + "\n";
		if( s.integrator == Integrator::PTSpectral ) {
			o << "pathtracing_spectral_rasterizer\n{\n\tsamples " << s.samples
			  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n" << ts << env;
		} else {
			o << "pathtracing_pel_rasterizer\n{\n\tsamples " << s.samples
			  << "\n\trr_min_depth 8\n\tpixel_filter box\n\toidn_denoise FALSE\n\tpathguiding FALSE\n\tadaptive_max_samples 0\n"
			  << ts << env;
		}
		if( s.integrator == Integrator::PTSMS ) {
			o << "\tsms_enabled TRUE\n";
		}
		o << "}\n\n";
		o << "file_rasterizeroutput\n{\n\tpattern rendered/dl344_unused\n\ttype EXR\n\tbpp 32\n\tcolor_space Rec709RGB_Linear\n}\n";
		return o.str();
	}

	std::string WriteScene( const std::string& text, const char* tag )
	{
		std::ostringstream p;
		p << "tests/dl344_" << tag << "_" << getpid() << ".RISEscene";
		std::ofstream f( p.str().c_str() );
		if( !f ) return std::string();
		f << text;
		return p.str();
	}

	//! One salted render; returns the image mean (RGB average) or a
	//! negative value on failure.
	double RenderMean( const std::string& path, unsigned int seed )
	{
		IJobPriv* job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) return -1;
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) { safe_release( job ); return -1; }
		job->RemoveRasterizerOutputs();
		CapturingRasterizerOutput* cap = new CapturingRasterizerOutput();
		GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "dl344 capture" );
		job->GetRasterizer()->AddRasterizerOutput( cap );
		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( seed, 0x344u ) );
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

	//! Renders `test` and `ref` with paired seeds and gates the ratio of
	//! their means against 1 within `band`.
	//! @a expected is the ratio the row gates against (1 unless the row
	//! pins a known defect).
	void RatioRow( const std::string& label, const SceneSpec& test, const SceneSpec& ref,
		double band, unsigned int trials, unsigned int& seed, const std::string& only,
		double expected = 1.0 )
	{
		if( !only.empty() && label.find( only ) == std::string::npos ) return;
		const std::string tPath = WriteScene( BuildScene( test ), "test" );
		const std::string rPath = WriteScene( BuildScene( ref ), "ref" );
		Check( !tPath.empty() && !rPath.empty(), label + ": scene files written" );
		std::vector<double> tv, rv, ratios;
		bool allValid = true;
		for( unsigned int t = 0; t < trials; ++t ) {
			const unsigned int s = seed++;
			const double a = RenderMean( tPath, s );
			const double b = RenderMean( rPath, s );
			if( !( a > 0 ) || !( b > 0 ) ) allValid = false;
			tv.push_back( a );
			rv.push_back( b );
			ratios.push_back( a / b );
		}
		std::remove( tPath.c_str() );
		std::remove( rPath.c_str() );
		Check( allValid, label + ": every render finite and non-black" );
		if( !allValid ) return;
		const Stats st = Summarize( tv ), sr = Summarize( rv ), sq = Summarize( ratios );
		const double ratio = st.mean / sr.mean;
		std::cout << std::setprecision( 6 ) << "  " << label << ": test " << st.mean << " +/- " << st.sd
			<< "  ref " << sr.mean << " +/- " << sr.sd << "  ratio " << ratio
			<< " (per-pair sd " << sq.sd << ", band " << band << ")" << std::endl;
		Check( std::fabs( ratio - expected ) < band, label + ": ratio within band of expected" );
	}
}

int main( int argc, char** argv )
{
	unsigned int trials = 4;
	std::string only;
	for( int i = 1; i < argc; ++i ) {
		const std::string a( argv[i] );
		if( a == "--only" && i + 1 < argc ) only = argv[++i];
		else if( a == "--trials" && i + 1 < argc ) trials = static_cast<unsigned int>( std::atoi( argv[++i] ) );
	}
	if( trials < 2 ) trials = 2;

	std::cout << "=== DL-344 transparent-shadow partition, n=" << trials << " ===" << std::endl;
	unsigned int seed = 34400;

	// A: index-1.0 room, white environment, flag on.
	struct ARow { const char* name; Model model; Integrator integrator; unsigned int spp; double band; };
	const ARow aRows[] = {
		{ "A: lambertian/PT",         Model::Lambertian, Integrator::PT,         64,  0.02 },
		{ "A: random_walk/PT",        Model::RandomWalk, Integrator::PT,         64,  0.02 },
		{ "A: diffusion/PT",          Model::Diffusion,  Integrator::PT,         64,  0.02 },
		{ "A: lambertian/PTSpectral", Model::Lambertian, Integrator::PTSpectral, 128, 0.03 },
	};
	for( const ARow& r : aRows ) {
		RatioRow( std::string( r.name ) + " index-1.0 room / open air (env)",
			{ r.model, r.integrator, Light::Env, Wall::MatchedRoom, true, r.spp },
			{ r.model, r.integrator, Light::Env, Wall::None, true, r.spp },
			r.band, trials, seed, only );
	}

	// B: index-1.0 room, area emitter outside it.
	RatioRow( "B: lambertian/PT index-1.0 room / open air (area)",
		{ Model::Lambertian, Integrator::PT, Light::Area, Wall::MatchedRoom, true, 256 },
		{ Model::Lambertian, Integrator::PT, Light::Area, Wall::None, true, 256 },
		0.02, trials, seed, only );

	// C: 1.5 glass around the subject, white environment.
	RatioRow( "C: lambertian/PT glass-1.5 flag TRUE / FALSE (env)",
		{ Model::Lambertian, Integrator::PT, Light::Env, Wall::GlassAroundSubject, true, 64 },
		{ Model::Lambertian, Integrator::PT, Light::Env, Wall::GlassAroundSubject, false, 64 },
		0.02, trials, seed, only );

	// D: control -- omni light seen through the index-1.0 room.
	RatioRow( "D: lambertian/PT index-1.0 room / open air (omni, control)",
		{ Model::Lambertian, Integrator::PT, Light::Omni, Wall::MatchedRoom, true, 64 },
		{ Model::Lambertian, Integrator::PT, Light::Omni, Wall::None, true, 64 },
		0.01, trials, seed, only );

	// E1: PT + SMS, omni light through the index-1.0 room / the same with
	// no room: what SMS returns through an invisible caster, against a
	// reference.  KNOWN DEFECT DL-413 PINNED at 0.5: SMS delivers HALF a
	// delta light's direct light through a specular caster (0.4994 here;
	// dielectric or perfect refractor, ior 1.0 or 1.01, biased or not).
	// When DL-413 is fixed, move `expected` to 1.  Pre-fix (SMS plus the
	// walk) it read about 1.5, so the pin is red there too.
	RatioRow( "E1: lambertian/PT+SMS index-1.0 room / open air (omni) [DL-413 pin 0.5]",
		{ Model::Lambertian, Integrator::PTSMS, Light::Omni, Wall::MatchedRoom, true, 64 },
		{ Model::Lambertian, Integrator::PTSMS, Light::Omni, Wall::None, true, 64 },
		0.02, trials, seed, only, 0.5 );

	// E2: PT + SMS, omni light through 1.5 glass, flag TRUE / FALSE.  Equal
	// by construction once SMS-sampled lights keep a binary shadow; its
	// value is the pre-fix red (2.46x), which E1 above checks against a
	// reference.
	RatioRow( "E2: lambertian/PT+SMS glass-1.5 flag TRUE / FALSE (omni)",
		{ Model::Lambertian, Integrator::PTSMS, Light::Omni, Wall::GlassAroundSubject, true, 64 },
		{ Model::Lambertian, Integrator::PTSMS, Light::Omni, Wall::GlassAroundSubject, false, 64 },
		0.02, trials, seed, only );

	// F: a DIRECTIONAL light through 1.5 glass.  SMS never samples it (it is
	// not in LightSampler's SampleLight table), so PT + SMS with the flag
	// must equal plain PT with the flag; a global "flag off under SMS"
	// rule rendered it black.
	RatioRow( "F: lambertian/PT+SMS / PT, flag TRUE, glass-1.5 (directional)",
		{ Model::Lambertian, Integrator::PTSMS, Light::Directional, Wall::GlassAroundSubject, true, 64 },
		{ Model::Lambertian, Integrator::PT, Light::Directional, Wall::GlassAroundSubject, true, 64 },
		0.02, trials, seed, only );

	std::cout << "=== " << passCount << " passed, " << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
