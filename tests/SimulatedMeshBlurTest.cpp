//////////////////////////////////////////////////////////////////////
//
//  SimulatedMeshBlurTest.cpp - Simulation M3b: deformation motion blur of
//    a simulated mesh is the time-average of static renders over the
//    shutter (the MotionBlurTimeAverageTest gate, DL-457), and a rigidly
//    translating simulated mesh blurs like the same motion done by the
//    existing transform blur.
//
//    The test writes its own mesh.v1 cache (no solver): a 9 x 9 black sheet
//    over a white environment, frames at t = -1.5, 0.5, 2.5 (interval 2), so
//    the whole shutter [0, 1] of the frame at t = 0.5 reads the middle frame
//    and its velocities (the nearest-frame model of SimulatedMeshSource).
//
//      d  a DEFORMING sheet (translation + stretch velocity field): blurred frame vs
//         the mean of static renders at the midpoints of 8 sub-intervals, PT
//         and BDPT
//      r  a RIGID translation: the simulated mesh's velocity keys vs an
//         indexed mesh carried by a keyframed object (transform blur), PT
//      e  an EMISSIVE simulated mesh (no blur) lighting a floor vs an indexed
//         mesh at the same pose: the light tables follow the installed frame
//
//    Gate (as DL-457): |blurred - reference| <= t_crit x combined standard
//    error (Welch), family alpha 0.01 Bonferroni.  Also asserts the blurred
//    renders took the multi-threaded time-indexed path (DL-465):
//    TimeIndexedFrame::TryCreate must not fall back because of the mesh.
//
//    Usage: SimulatedMeshBlurTest [seed] [all|d|r|e] [spp] [repeats]
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Rendering/TimeIndexedMotionBlur.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;
namespace fs = std::filesystem;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	unsigned int g_seedBase = 3311u;
	unsigned int g_renderIndex = 0;
	int passCount = 0, failCount = 0;
	int g_spp = 32;
	int g_repeats = 3;			//!< static renders per reference time
	int g_blurRepeats = 8;		//!< blurred renders per estimate
	int g_comparisons = 1;
	const double kFamilyAlpha = 0.01;
	fs::path g_scratch;

	double BetaCF( const double a, const double b, const double x )
	{
		const double tiny = 1e-300;
		double c = 1, d = 1 - ( a + b ) * x / ( a + 1 );
		if( std::fabs( d ) < tiny ) d = tiny;
		d = 1 / d;
		double h = d;
		for( int m = 1; m <= 300; m++ ) {
			const int m2 = 2 * m;
			double aa = m * ( b - m ) * x / ( ( a + m2 - 1 ) * ( a + m2 ) );
			d = 1 + aa * d; if( std::fabs( d ) < tiny ) d = tiny;
			c = 1 + aa / c; if( std::fabs( c ) < tiny ) c = tiny;
			d = 1 / d; h *= d * c;
			aa = -( a + m ) * ( a + b + m ) * x / ( ( a + m2 ) * ( a + m2 + 1 ) );
			d = 1 + aa * d; if( std::fabs( d ) < tiny ) d = tiny;
			c = 1 + aa / c; if( std::fabs( c ) < tiny ) c = tiny;
			d = 1 / d;
			const double del = d * c;
			h *= del;
			if( std::fabs( del - 1 ) < 1e-14 ) break;
		}
		return h;
	}
	double IncompleteBeta( const double a, const double b, const double x )
	{
		if( x <= 0 ) return 0;
		if( x >= 1 ) return 1;
		const double bt = std::exp( std::lgamma( a + b ) - std::lgamma( a ) - std::lgamma( b ) + a * std::log( x ) + b * std::log( 1 - x ) );
		return x < ( a + 1 ) / ( a + b + 2 ) ? bt * BetaCF( a, b, x ) / a : 1 - bt * BetaCF( b, a, 1 - x ) / b;
	}
	double TwoSidedP( const double t, const double dof ) { return IncompleteBeta( 0.5 * dof, 0.5, dof / ( dof + t * t ) ); }
	double CriticalT( const double alpha, const double dof )
	{
		double lo = 0, hi = 1000;
		for( int i = 0; i < 200; i++ ) {
			const double mid = 0.5 * ( lo + hi );
			if( TwoSidedP( mid, dof ) > alpha ) lo = mid; else hi = mid;
		}
		return 0.5 * ( lo + hi );
	}

	void Check( const bool condition, const std::string& name )
	{
		if( condition ) { passCount++; return; }
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}

	std::string Fmt( const char* f, ... )
	{
		char buf[8192];
		va_list ap;
		va_start( ap, f );
		std::vsnprintf( buf, sizeof( buf ), f, ap );
		va_end( ap );
		return std::string( buf );
	}

	class CapturingRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
	protected:
		virtual ~CapturingRasterizerOutput() {}
	public:
		void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
		void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
		{
			pixels.resize( img.GetWidth() * img.GetHeight() );
			for( unsigned int y = 0; y < img.GetHeight(); y++ )
				for( unsigned int x = 0; x < img.GetWidth(); x++ )
					pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
		}
	};

	//! The single frame at `frameTime` through RasterizeAnimation (the
	//! prepared path installs the mesh frame for it).
	bool RenderOnce( const std::string& sceneText, const double frameTime, double& mean )
	{
		const fs::path path = g_scratch / "blur.RISEscene";
		{ std::ofstream ofs( path ); ofs << sceneText; }
		bool ok = false;
		IJobPriv* pJob = nullptr;
		if( RISE_CreateJobPriv( &pJob ) && pJob ) {
			if( pJob->LoadAsciiSceneViaCst( path.string().c_str() ) ) {
				pJob->RemoveRasterizerOutputs();
				CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
				GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
				pJob->GetRasterizer()->AddRasterizerOutput( pCap );
				SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( g_seedBase + g_renderIndex, 0x3b3u ) );
				std::srand( g_seedBase + g_renderIndex++ );
				const bool bRendered = pJob->RasterizeAnimation( frameTime, frameTime + 1.0, 1, false, false );
				SobolSamplerTestHooks::ValueSalt().store( 0u );
				if( bRendered && !pCap->pixels.empty() ) {
					double sum = 0; ok = true;
					for( const RISEColor& c : pCap->pixels ) {
						const double v = ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
						if( !std::isfinite( v ) ) { ok = false; break; }
						sum += v;
					}
					mean = sum / double( pCap->pixels.size() );
				}
				safe_release( pCap );
			}
			safe_release( pJob );
		}
		return ok;
	}

	struct Stat { double mean, se; bool ok; };

	Stat RenderN( const std::string& text, const double frameTime, const int n )
	{
		Stat s{ 0, 0, true };
		std::vector<double> v;
		for( int i = 0; i < n; i++ ) {
			double m = 0;
			if( !RenderOnce( text, frameTime, m ) ) { s.ok = false; return s; }
			v.push_back( m );
		}
		for( double x : v ) s.mean += x;
		s.mean /= n;
		double ss = 0;
		for( double x : v ) ss += ( x - s.mean ) * ( x - s.mean );
		s.se = std::sqrt( ss / ( n - 1 ) / n );
		return s;
	}

	//////////////////////////////////////////////////////////////////
	// The cache: a 9 x 9 sheet at z = 0.5 with per-vertex velocity v(x, y).
	const int kN = 9;
	const double kStart = -1.5, kInterval = 2.0, kRef = 0.5;	// frames at -1.5, 0.5, 2.5

	std::uint32_t Bits( const double v ) { const float f = float( v ); std::uint32_t b; std::memcpy( &b, &f, 4 ); return b; }

	void Rest( const int i, const int j, double& x, double& y )
	{
		x = -0.8 + 1.6 * i / ( kN - 1 );
		y = -0.6 + 1.2 * j / ( kN - 1 );
	}

	//! kDeforming: a translation plus an anisotropic stretch, so the sheet's
	//! area -- the pixel mean against the white environment -- grows
	//! quadratically over the shutter (x0.42 at open, x1.82 at close) and the
	//! blurred mean differs from the nominal pose's by ~10 standard errors;
	//! else a pure translation.
	enum Motion { kDeforming, kRigid, kExpanding };
	void Velocity( const Motion motion, const double x, const double y, double v[3] )
	{
		switch( motion ) {
		case kDeforming: v[0] = 1.2 + 0.8 * x; v[1] = 0.6 * y; v[2] = 0.0; break;
		case kRigid: v[0] = 1.6; v[1] = 0.0; v[2] = 0.0; break;
		// Isotropic growth about the origin: the bind frame (t = -1.5) is half
		// the size, a quarter of the area, of the frame at t = 0.5.
		case kExpanding: v[0] = 0.25 * x; v[1] = 0.25 * y; v[2] = 0.0; break;
		}
	}

	//! Counter-clockwise from +z (the front faces the camera); `down` flips it.
	std::vector<std::uint32_t> Indices( const bool down = false )
	{
		std::vector<std::uint32_t> idx;
		for( int j = 0; j + 1 < kN; ++j ) for( int i = 0; i + 1 < kN; ++i ) {
			const std::uint32_t a = j * kN + i, b = a + 1, c = a + kN, d = c + 1;
			if( down ) idx.insert( idx.end(), { a, c, b, b, c, d } );
			else idx.insert( idx.end(), { a, b, c, b, d, c } );
		}
		return idx;
	}

	bool WriteCache( const fs::path& root, const Motion motion, const bool down = false )
	{
		std::error_code ec;
		fs::remove_all( root, ec );
		fs::create_directories( root.parent_path(), ec );
		Simulation::CacheDescription d;
		d.key = std::string( 64, "def"[motion] ); d.epoch = std::string( 64, "123"[motion] );
		d.producer = "m3b-blur-fixture"; d.producerVersion = "1";
		// Scene time t maps to simulation time startTime + (t - sceneOrigin):
		// with sceneOrigin = startTime the two clocks agree.
		d.count = 3; d.startTime = kStart; d.sceneOrigin = kStart; d.interval = kInterval; d.hold = true;
		d.outputs = { { "surface", "mesh.v1", { { "position", "m", "f32x3" }, { "velocity", "m/s", "f32x3" } } } };
		Simulation::CacheWriter writer;
		std::string error;
		if( !writer.Begin( root, d, "", false, 64u << 20, error ) ) { std::printf( "  cache: %s\n", error.c_str() ); return false; }
		for( int k = 0; k < 3; ++k ) {
			const double s = kStart + k * kInterval;
			Simulation::MeshFrame f;
			f.velocityUnits = "m/s";
			for( int j = 0; j < kN; ++j ) for( int i = 0; i < kN; ++i ) {
				double x, y, v[3]; Rest( i, j, x, y ); Velocity( motion, x, y, v );
				f.positions.push_back( Bits( x + v[0] * ( s - kRef ) ) );
				f.positions.push_back( Bits( y + v[1] * ( s - kRef ) ) );
				f.positions.push_back( Bits( 0.5 ) );
				for( int c = 0; c < 3; ++c ) f.velocities.push_back( Bits( v[c] ) );
			}
			f.indices = Indices( down );
			Simulation::Bytes bytes;
			if( !Simulation::EncodeMesh( f, 64u << 20, bytes, error ) || !writer.Append( k, "surface", s, bytes, error ) ) {
				std::printf( "  cache: %s\n", error.c_str() );
				return false;
			}
		}
		return writer.Seal( error );
	}

	//! The middle frame as an inline indexed mesh (the rigid row's reference).
	std::string IndexedSheet( const bool down = false )
	{
		std::string s = "indexedmesh_geometry\n{\n\tname sheet_static\n";
		for( int j = 0; j < kN; ++j ) for( int i = 0; i < kN; ++i ) {
			double x, y; Rest( i, j, x, y );
			s += Fmt( "\tvertex %.9g %.9g 0.5\n", double( float( x ) ), double( float( y ) ) );
		}
		const std::vector<std::uint32_t> idx = Indices( down );
		for( std::size_t t = 0; t < idx.size(); t += 3 ) s += Fmt( "\ttriangle %u %u %u\n", idx[t], idx[t+1], idx[t+2] );
		return s + "\tdouble_sided TRUE\n}\n\n";
	}

	std::string Padding()
	{
		std::string s;
		for( int i = 0; i < 40; i++ ) {
			s += Fmt( "standard_object\n{\n\tname pad%d\n\tgeometry geo_unit\n\tmaterial black_mat\n\tposition %g %g -5\n\tscale 0.5 0.5 0.5\n}\n\n",
				i, -40.0 + 2.0 * ( i % 8 ), -40.0 + 2.0 * ( i / 8 ) );
		}
		return s;
	}

	std::string Env() { return "\tradiance_map white\n\tradiance_scale 1.0\n\tradiance_background TRUE\n"; }
	std::string RasPT() { return "pathtracing_pel_rasterizer\n{\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n" + Env() + "}\n\n"; }
	std::string RasBDPT() { return "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n" + Env() + "}\n\n"; }

	std::string Scene( const std::string& body, const std::string& rasterizer, const double exposure )
	{
		return std::string( "RISE ASCII SCENE 7\n\n" ) +
			"film\n{\n\twidth 16\n\theight 16\n}\n\n" +
			Fmt( "orthographic_camera\n{\n\tlocation 0 0 1\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 4\n\texposure %g\n}\n\n", exposure ) +
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n"
			"lambertian_material\n{\n\tname black_mat\n\treflectance black\n}\n\n"
			"sphere_geometry\n{\n\tname geo_unit\n\tradius 1\n}\n\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n" +
			body + Padding() + rasterizer;
	}

	std::string SimulatedSheet( const fs::path& cache, const char* material = "black_mat" )
	{
		return "simulated_mesh_geometry\n{\n\tname sheet_sim\n\tcache " + cache.string() + "\n\tdouble_sided TRUE\n}\n\n"
			"standard_object\n{\n\tname sheet\n\tgeometry sheet_sim\n\tmaterial " + material + "\n}\n\n";
	}

	struct Welch { double z, crit, se; };
	Welch Compare( const Stat& a, const double refMean, const double refVar, const double refDofDen )
	{
		const double va = a.se * a.se;
		const double var = va + refVar;
		const double dofDen = va * va / ( g_blurRepeats - 1 ) + refDofDen;
		const double dof = dofDen > 0 ? var * var / dofDen : 1e6;
		Welch w;
		w.se = std::sqrt( var );
		w.z = w.se > 0 ? ( a.mean - refMean ) / w.se : 0;
		w.crit = CriticalT( kFamilyAlpha / g_comparisons, dof );
		return w;
	}

	//! Row d: blurred deforming sheet vs the time-average of statics.
	void RowDeforming( const fs::path& cache )
	{
		const double kFrame = 0.5, kExposure = 1.0;
		const int times = 8;
		struct Integrator { const char* name; std::string ras; };
		for( const Integrator& ig : { Integrator{ "PT", RasPT() }, Integrator{ "BDPT", RasBDPT() } } ) {
			const unsigned long long frames0 = TimeIndexedFrame::CreatedCount();
			const Stat blur = RenderN( Scene( SimulatedSheet( cache ), ig.ras, kExposure ), kFrame, g_blurRepeats );
			const unsigned long long timeIndexed = TimeIndexedFrame::CreatedCount() - frames0;
			double refMean = 0, refVar = 0, refDofDen = 0;
			bool ok = blur.ok;
			for( int k = 0; k < times && ok; k++ ) {
				const double t = kFrame - 0.5 * kExposure + ( k + 0.5 ) * kExposure / times;
				const Stat s = RenderN( Scene( SimulatedSheet( cache ), ig.ras, 0.0 ), t, g_repeats );
				ok = ok && s.ok;
				refMean += s.mean / times;
				const double v = s.se * s.se / ( double( times ) * times );
				refVar += v;
				refDofDen += v * v / ( g_repeats - 1 );
			}
			const Welch w = Compare( blur, refMean, refVar, refDofDen );
			std::printf( "  %-4s d: deforming sheet  blurred %.6f +/- %.6f  time-average %.6f +/- %.6f  ratio %.4f  t %+.2f (crit %.2f)  time-indexed frames %llu\n",
				ig.name, blur.mean, blur.se, refMean, std::sqrt( refVar ), refMean > 0 ? blur.mean / refMean : 0.0, w.z, w.crit, timeIndexed );
			Check( ok && blur.mean > 0, std::string( "renders complete: " ) + ig.name + " d" );
			Check( ok && w.crit * w.se <= 0.22 * refMean, std::string( "precision cap (crit*se <= 22% of the mean): " ) + ig.name + " d" );
			Check( ok && std::fabs( blur.mean - refMean ) <= w.crit * w.se,
				std::string( "deformation blur = time average of static renders: " ) + ig.name + " d" );
			Check( timeIndexed == unsigned( g_blurRepeats ),
				std::string( "every blurred frame took the multi-threaded time-indexed path: " ) + ig.name + " d" );
			// The statics differ from each other: the reference really moves.
			double m0 = 0, m1 = 0;
			RenderOnce( Scene( SimulatedSheet( cache ), ig.ras, 0.0 ), 0.0, m0 );
			RenderOnce( Scene( SimulatedSheet( cache ), ig.ras, 0.0 ), 1.0, m1 );
			std::printf( "       static at shutter open %.6f, close %.6f\n", m0, m1 );
		}
	}

	//! Row r: rigid translation, velocity keys vs the transform blur.
	void RowRigid( const fs::path& cache )
	{
		const double kFrame = 0.5, kExposure = 1.0;
		const std::string transformBody = IndexedSheet() +
			"standard_object\n{\n\tname sheet\n\tgeometry sheet_static\n\tmaterial black_mat\n\tposition -0.8 0 0\n}\n\n"
			"timeline\n{\n\telement_type object\n\telement sheet\n\tparam position\n\tinterpolator linear\n\ttime 0\n\tvalue -0.8 0 0\n\ttime 1\n\tvalue 0.8 0 0\n}\n\n";
		const unsigned long long frames0 = TimeIndexedFrame::CreatedCount();
		const Stat sim = RenderN( Scene( SimulatedSheet( cache ), RasPT(), kExposure ), kFrame, g_blurRepeats );
		const unsigned long long timeIndexed = TimeIndexedFrame::CreatedCount() - frames0;
		const Stat xf = RenderN( Scene( transformBody, RasPT(), kExposure ), kFrame, g_blurRepeats );
		const double va = sim.se * sim.se, vb = xf.se * xf.se;
		const double se = std::sqrt( va + vb );
		const double dofDen = va * va / ( g_blurRepeats - 1 ) + vb * vb / ( g_blurRepeats - 1 );
		const double dof = dofDen > 0 ? ( va + vb ) * ( va + vb ) / dofDen : 1e6;
		const double z = se > 0 ? ( sim.mean - xf.mean ) / se : 0;
		const double crit = CriticalT( kFamilyAlpha / g_comparisons, dof );
		// Both statics at the frame time agree exactly in coverage (same pose).
		double s0 = 0, s1 = 0;
		RenderOnce( Scene( SimulatedSheet( cache ), RasPT(), 0.0 ), 0.25, s0 );
		RenderOnce( Scene( transformBody, RasPT(), 0.0 ), 0.25, s1 );
		std::printf( "  PT   r: rigid translation  simulated %.6f +/- %.6f  transform blur %.6f +/- %.6f  ratio %.4f  t %+.2f (crit %.2f)  time-indexed frames %llu\n",
			sim.mean, sim.se, xf.mean, xf.se, xf.mean > 0 ? sim.mean / xf.mean : 0.0, z, crit, timeIndexed );
		std::printf( "       static at t = 0.25: simulated %.6f, transformed %.6f\n", s0, s1 );
		Check( sim.ok && xf.ok && sim.mean > 0, "renders complete: r" );
		Check( sim.ok && xf.ok && std::fabs( sim.mean - xf.mean ) <= crit * se,
			"rigid deformation blur = transform blur of the same motion: PT r" );
		Check( timeIndexed == unsigned( g_blurRepeats ), "rigid row took the time-indexed path" );
	}

	//! Row e: an EMISSIVE simulated mesh (no blur) lights the floor from the
	//! installed frame, not the bind frame: the light-selection tables and the
	//! area-sampling CDF follow the per-frame install.  The cache's bind frame
	//! has a quarter of the installed frame's area (a light power, area pdf or
	//! sampling CDF left at the bind frame is off by 4x); the reference is an
	//! indexed mesh at the installed pose.
	void RowEmissive( const fs::path& cache )
	{
		const std::string lit =
			"uniformcolor_painter\n{\n\tname grey\n\tcolor 0.5 0.5 0.5\n}\n\n"
			"lambertian_material\n{\n\tname floor_mat\n\treflectance grey\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 4\n\tmaterial none\n}\n\n"
			"clippedplane_geometry\n{\n\tname geo_floor\n\tpta -2.5 -2.5 0\n\tptb 2.5 -2.5 0\n\tptc 2.5 2.5 0\n\tptd -2.5 2.5 0\n\tdoublesided FALSE\n}\n\n"
			"standard_object\n{\n\tname floor\n\tgeometry geo_floor\n\tmaterial floor_mat\n}\n\n";
		const std::string noEnv = "pathtracing_pel_rasterizer\n{\n\tsamples " + std::to_string( g_spp ) + "\n\toidn_denoise FALSE\n\tpixel_filter box\n}\n\n";
		const std::string indexed = lit + IndexedSheet( true ) + "standard_object\n{\n\tname sheet\n\tgeometry sheet_static\n\tmaterial lum\n}\n\n";
		const Stat sim = RenderN( Scene( lit + SimulatedSheet( cache, "lum" ), noEnv, 0.0 ), 0.5, g_blurRepeats );
		const Stat ref = RenderN( Scene( indexed, noEnv, 0.0 ), 0.5, g_blurRepeats );
		const double va = sim.se * sim.se, vb = ref.se * ref.se;
		const double se = std::sqrt( va + vb );
		const double dofDen = va * va / ( g_blurRepeats - 1 ) + vb * vb / ( g_blurRepeats - 1 );
		const double dof = dofDen > 0 ? ( va + vb ) * ( va + vb ) / dofDen : 1e6;
		const double z = se > 0 ? ( sim.mean - ref.mean ) / se : 0;
		const double crit = CriticalT( kFamilyAlpha / g_comparisons, dof );
		std::printf( "  PT   e: emissive sheet  simulated %.6f +/- %.6f  indexed %.6f +/- %.6f  ratio %.4f  t %+.2f (crit %.2f)\n",
			sim.mean, sim.se, ref.mean, ref.se, ref.mean > 0 ? sim.mean / ref.mean : 0.0, z, crit );
		Check( sim.ok && ref.ok && sim.mean > 0, "renders complete: e" );
		Check( sim.ok && ref.ok && std::fabs( sim.mean - ref.mean ) <= crit * se,
			"emissive simulated mesh lights the scene from the installed frame: PT e" );
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	if( argc > 1 ) g_seedBase = unsigned( std::strtoul( argv[1], nullptr, 10 ) );
	const std::string only = argc > 2 ? argv[2] : "all";
	if( argc > 3 ) g_spp = std::atoi( argv[3] );
	if( argc > 4 ) g_blurRepeats = std::atoi( argv[4] );
	std::cout << "SimulatedMeshBlurTest (simulation M3b)\n";
	g_scratch = RISE::TestSupport::ScratchRunDir( "rise_simulated_mesh_blur_", "suite" );
	std::error_code ec;
	fs::create_directories( g_scratch, ec );
	const bool rowD = only == "all" || only.find( 'd' ) != std::string::npos;
	const bool rowR = only == "all" || only.find( 'r' ) != std::string::npos;
	const bool rowE = only == "all" || only.find( 'e' ) != std::string::npos;
	g_comparisons = ( rowD ? 2 : 0 ) + ( rowR ? 1 : 0 ) + ( rowE ? 1 : 0 );
	if( g_comparisons == 0 ) g_comparisons = 1;
	const fs::path deforming = g_scratch / "deforming", rigid = g_scratch / "rigid", emitter = g_scratch / "emitter";
	Check( WriteCache( deforming, kDeforming ) && WriteCache( rigid, kRigid ) && WriteCache( emitter, kExpanding, true ), "write the fixture caches" );
	if( rowD ) RowDeforming( deforming );
	if( rowR ) RowRigid( rigid );
	if( rowE ) RowEmissive( emitter );
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_simulated_mesh_blur_" ), ec );
	std::cout << "\n" << passCount << " passed, " << failCount << " failed\n";
	return failCount == 0 ? 0 : 1;
}
