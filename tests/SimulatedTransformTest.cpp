// Simulation M6: `standard_object { transform_source <simulation>.<output>[<body>] }`.
//   A. the object's pose follows a transform.v1 cache (translation + rotation):
//      at frame times exactly, between them to the quarter-interval keys'
//      interpolation; its authored pose is the pose at the first frame.
//   B. a blurred render of the simulated body equals, within noise, the same
//      motion keyframed by `timeline` chunks, and the time average of
//      unblurred renders over the shutter; the blur takes the time-indexed
//      (multi-threaded) path.
//   C. refusals: unknown simulation, an output that is not transform.v1, an
//      unknown body; a missing cache leaves the object authored.
#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IAnimator.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Rendering/TimeIndexedMotionBlur.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationTransform.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Job.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace RISE;
using namespace RISE::Implementation;
namespace fs = std::filesystem;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	int failures = 0;
	unsigned int g_render = 0;
	fs::path g_scratch;
	void Check( const bool ok, const std::string& what, const std::string& detail = "" )
	{
		if( !ok ) { ++failures; std::cout << "  FAIL: " << what << " " << detail << std::endl; }
	}
	std::string Fmt( const char* f, ... )
	{
		char buf[4096];
		va_list ap; va_start( ap, f ); std::vsnprintf( buf, sizeof( buf ), f, ap ); va_end( ap );
		return buf;
	}

	// The motion: x(t) = (-0.8 + 0.8 t, 0, 0), a rotation of 1.2 t rad about z, frames at t = 0, 0.5, ..., 2.
	const double kV = 0.8, kW = 1.2, kInterval = 0.5;
	const int kFrames = 5;
	double X( double t ) { return -0.8 + kV*t; }

	bool WriteCache( const fs::path& root )
	{
		std::error_code ec;
		fs::remove_all( root, ec );
		Simulation::CacheDescription d;
		d.key = std::string( 64, 'a' ); d.epoch = std::string( 64, 'b' );
		d.producer = "m6-transform-fixture"; d.producerVersion = "1";
		d.count = kFrames; d.interval = kInterval;
		d.outputs = { { "bodies", Simulation::TransformCodec, { { "position", "m", "f64x3" } } } };
		Simulation::CacheWriter writer;
		std::string error;
		if( !writer.Begin( root, d, "", false, 64u << 20, error ) ) { std::cout << "  cache: " << error << "\n"; return false; }
		for( int k = 0; k < kFrames; ++k ) {
			const double t = k*kInterval;
			Simulation::TransformFrame f;
			f.time = t;
			Simulation::TransformBody b;
			b.name = "slab";
			b.position = {{ X( t ), 0, 0 }};
			b.orientation = {{ std::cos( 0.5*kW*t ), 0, 0, std::sin( 0.5*kW*t ) }};
			b.velocity = {{ kV, 0, 0 }};
			b.angularVelocity = {{ 0, 0, kW }};
			f.bodies = { b };
			Simulation::Bytes bytes;
			if( !Simulation::EncodeTransforms( f, 64u << 20, bytes, error ) || !writer.Append( k, "bodies", t, bytes, error ) ) return false;
		}
		return writer.Seal( error );
	}

	class Capture : public virtual IRasterizerOutput, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
	protected:
		virtual ~Capture() {}
	public:
		void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
		void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
		{
			pixels.resize( img.GetWidth()*img.GetHeight() );
			for( unsigned int y = 0; y < img.GetHeight(); y++ ) for( unsigned int x = 0; x < img.GetWidth(); x++ )
				pixels[y*img.GetWidth() + x] = img.GetPEL( x, y );
		}
	};

	bool Render( const std::string& text, double frameTime, double& mean )
	{
		const fs::path path = g_scratch/"t.RISEscene";
		{ std::ofstream o( path ); o << text; }
		bool ok = false;
		IJobPriv* job = nullptr;
		if( RISE_CreateJobPriv( &job ) && job ) {
			if( job->LoadAsciiSceneViaCst( path.string().c_str() ) ) {
				job->RemoveRasterizerOutputs();
				Capture* cap = new Capture();
				GlobalLog()->PrintNew( cap, __FILE__, __LINE__, "capture" );
				job->GetRasterizer()->AddRasterizerOutput( cap );
				SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 911u + g_render, 0x3b3u ) );
				std::srand( 911u + g_render++ );
				if( job->RasterizeAnimation( frameTime, frameTime + 1.0, 1, false, false ) && !cap->pixels.empty() ) {
					double sum = 0;
					for( const RISEColor& c : cap->pixels ) sum += ( c.base.r + c.base.g + c.base.b )*c.a/3.0;
					mean = sum/double( cap->pixels.size() );
					ok = true;
				}
				SobolSamplerTestHooks::ValueSalt().store( 0u );
				safe_release( cap );
			}
			safe_release( job );
		}
		return ok;
	}

	std::string Scene( const std::string& body, double exposure )
	{
		return std::string( "RISE ASCII SCENE 7\n\n" ) + "film\n{\n\twidth 24\n\theight 24\n}\n\n" +
			Fmt( "orthographic_camera\n{\n\tlocation 0 0 2\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 3 3\n\texposure %g\n}\n\n", exposure ) +
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n"
			"lambertian_material\n{\n\tname black_mat\n\treflectance black\n}\n\n"
			"box_geometry\n{\n\tname geo_slab\n\twidth 1.2\n\theight 0.3\n\tdepth 0.1\n}\n\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" + body +
			"pathtracing_pel_rasterizer\n{\n\tsamples 16\n\toidn_denoise FALSE\n\tpixel_filter box\n\tradiance_map white\n"
			"\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n";
	}
	std::string Simulated( const fs::path& cache, const char* source = "spin.bodies[slab]" )
	{
		return "simulation_cache\n{\n\tname spin\n\tcache " + cache.string() + "\n}\n\n"
			"standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tmaterial black_mat\n\tposition -0.8 0 0\n"
			"\ttransform_source " + source + "\n}\n\n";
	}
	// The same motion as `timeline` keyframes (every output time and a quarter interval either side).
	std::string Keyframed()
	{
		std::string s = "standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tmaterial black_mat\n\tposition -0.8 0 0\n}\n\n";
		for( const char* param : { "position", "orientation" } ) {
			s += Fmt( "timeline\n{\n\telement_type object\n\telement slab\n\tparam %s\n", param );
			for( int k = 0; k < kFrames; ++k ) for( double d : { -0.25*kInterval, 0.0, 0.25*kInterval } ) {
				const double t = k*kInterval + d;
				if( t < 0 || t > ( kFrames - 1 )*kInterval ) continue;
				s += Fmt( "\n\ttime %.17g\n", t );
				s += std::string( param ) == "position" ? Fmt( "\tvalue %.17g 0 0\n", X( t ) ) : Fmt( "\tvalue 0 0 %.17g\n", kW*t*180/3.14159265358979323846 );
			}
			s += "}\n\n";
		}
		return s;
	}
}

int main()
{
	g_scratch = RISE::TestSupport::ScratchRunDir( "rise_sim_transform_source_", "suite" );
	std::error_code ec;
	fs::create_directories( g_scratch, ec );
	fs::permissions( g_scratch, fs::perms::owner_all, fs::perm_options::replace, ec );
	const fs::path cache = g_scratch/"spin";
	Check( WriteCache( cache ), "write the transform cache" );
	// ---------------------------------------------------------------- A. poses
	{
		const fs::path path = g_scratch/"a.RISEscene";
		{ std::ofstream o( path ); o << Scene( Simulated( cache ), 0 ); }
		IJobPriv* job = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( path.string().c_str() ), "the scene loads" );
		Job* j = job ? dynamic_cast<Job*>( job ) : nullptr;
		if( j ) {
			IAnimator* animator = j->GetScene()->GetAnimator();
			IObject* obj = const_cast<IObjectManager*>( j->GetScene()->GetObjects() )->GetItem( "slab" );
			Check( animator && obj && animator->AreThereAnyKeyframedObjects(), "the simulated transform is keyframed" );
			double worstFrame = 0, worstBetween = 0;
			for( int k = 0; animator && obj && k <= 32; ++k ) {
				const double t = k*2.0/32;
				animator->EvaluateAtTime( t );
				const Matrix4 m = obj->GetFinalTransformMatrix();
				const double c = std::cos( kW*t ), s = std::sin( kW*t );
				const double err = std::max( { std::fabs( m._30 - X( t ) ), std::fabs( m._31 ), std::fabs( m._00 - c ), std::fabs( m._01 - s ),
					std::fabs( m._10 + s ), std::fabs( m._11 - c ) } );
				( std::fabs( std::fmod( t, 0.125 ) ) < 1e-12 ? worstFrame : worstBetween ) =
					std::max( std::fabs( std::fmod( t, 0.125 ) ) < 1e-12 ? worstFrame : worstBetween, err );
			}
			std::cout << "  pose error at the keys " << worstFrame << ", between keys " << worstBetween << "\n";
			Check( worstFrame < 1e-9, "exact pose at frame and quarter-interval times" );
			Check( worstBetween < 2e-3, "pose between keys to the interpolation's error" );
		}
		if( job ) safe_release( job );
	}
	// ---------------------------------------------------------------- B. motion blur
	{
		const double frame = 1.0, shutter = 0.4;
		const unsigned long long before = TimeIndexedFrame::CreatedCount();
		double sim = 0, key = 0;
		const int n = 4;
		bool ok = true;
		for( int r = 0; r < n; ++r ) {
			double a = 0, b = 0;
			ok = ok && Render( Scene( Simulated( cache ), shutter ), frame, a ) && Render( Scene( Keyframed(), shutter ), frame, b );
			sim += a; key += b;
		}
		const unsigned long long indexed = TimeIndexedFrame::CreatedCount() - before;
		sim /= n; key /= n;
		// The time average of unblurred renders over the shutter [frame, frame + shutter].
		double avg = 0;
		const int m = 8;
		for( int k = 0; k < m; ++k ) {
			double x = 0;
			ok = ok && Render( Scene( Simulated( cache ), 0 ), frame + shutter*( k + 0.5 )/m, x );
			avg += x/m;
		}
		std::cout << "  blurred mean: simulated " << sim << ", keyframed " << key << ", time average " << avg <<
			"; time-indexed frames " << indexed << "\n";
		Check( ok, "renders" );
		Check( std::fabs( sim - key ) < 0.01, "the simulated body blurs like the same motion keyframed" );
		Check( std::fabs( sim - avg ) < 0.02, "the blur equals the time average over the shutter" );
		Check( indexed >= 2*n, "blurred frames take the time-indexed (multi-threaded) path" );
	}
	// ---------------------------------------------------------------- C. refusals
	{
		auto loads = [&]( const std::string& body ) {
			const fs::path path = g_scratch/"c.RISEscene";
			{ std::ofstream o( path ); o << Scene( body, 0 ); }
			IJobPriv* job = nullptr;
			const bool ok = RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( path.string().c_str() );
			if( job ) safe_release( job );
			return ok;
		};
		Check( !loads( Simulated( cache, "nope.bodies[slab]" ) ), "an unknown simulation is refused" );
		Check( !loads( Simulated( cache, "spin.bodies[ball]" ) ), "an unknown body is refused" );
		Check( !loads( Simulated( cache, "spin.other[slab]" ) ), "an unknown output is refused" );
		Check( !loads( Simulated( cache, "spin.bodies" ) ), "a malformed source is refused" );
		Check( loads( Simulated( g_scratch/"never_baked" ) ), "a missing cache leaves the object authored" );
	}
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_sim_transform_source_" ), ec );
	if( failures ) { std::cout << failures << " failure(s)\n"; return 1; }
	std::cout << "SimulatedTransformTest: all checks passed\n";
	return 0;
}
