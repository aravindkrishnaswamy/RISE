//////////////////////////////////////////////////////////////////////
//
//  AnimationRasterizerParityTest.cpp - DL-458 regression.
//
//  An ANIMATION render (IJob::RasterizeAnimation ->
//  IRasterizer::RasterizeSceneAnimation) must produce the same image
//  as a still render (IJob::Rasterize -> RasterizeScene) of the same
//  scene state, for every rasterizer.
//
//  DL-458: BDPTRasterizerBase overrode only RasterizeScene, and that is
//  where it handed the integrator the RayCaster's LightSampler and
//  created the t==1 SplatFilm.  RasterizeSceneAnimation fell through to
//  PixelBasedRasterizerHelper's generic driver, which never did either:
//  BDPT animation frames carried no NEE, no light subpaths and no
//  splats (~1e-3 of PT on an omni-lit quad).
//
//  Cases, per rasterizer (PT/BDPT/VCM pel+spectral, MLT pel+spectral):
//    A. no timeline, RasterizeAnimation(0,0,1)   vs Rasterize
//    B. no timeline, RasterizeAnimation(0,1,2)   both frames vs Rasterize
//    F. (BDPT/VCM) interlaced fields, RasterizeAnimation(0,0,1,fields)
//       vs Rasterize -- DL-462 (second field reset the splat film)
//    C. keyframed omni light moving left -> right over two frames: each
//       animation frame vs a STILL of a static scene with the light at
//       that frame's position, plus a left/right asymmetry check.
//  Each comparison: 4 salted renders per side, |diff| <= 3 combined SE
//  (plus a 2% relative floor so a near-deterministic estimator cannot
//  fail on a sub-ulp-scale SE).
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice dl458)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <cmath>
#include <string>
#include <algorithm>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/PSSMLTSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAILED: " << testName << std::endl;
	}
}

namespace
{
	//! Per-frame image statistics: whole-image mean and the left/right
	//! half means (luma = r+g+b, alpha-weighted).
	struct FrameStat
	{
		double mean = 0, left = 0, right = 0;
		bool valid = false;
	};

	class CapturingOutput
		: public virtual IRasterizerOutput
		, public virtual Reference
	{
	public:
		std::map<unsigned int, FrameStat> frames;

	protected:
		virtual ~CapturingOutput() {}

	public:
		void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}

		void OutputImage( const IRasterImage& img, const Rect*, const unsigned int frame ) override
		{
			const unsigned int w = img.GetWidth(), h = img.GetHeight();
			double sum = 0, sl = 0, sr = 0;
			bool finite = true;
			for( unsigned int y = 0; y < h; y++ ) {
				for( unsigned int x = 0; x < w; x++ ) {
					const RISEColor c = img.GetPEL( x, y );
					const double l = ( c.base.r + c.base.g + c.base.b ) * c.a;
					if( !std::isfinite( l ) ) { finite = false; }
					sum += l;
					if( x < w / 2 ) sl += l; else sr += l;
				}
			}
			FrameStat s;
			const double n = double( w ) * double( h );
			s.mean = sum / n;
			s.left = sl / ( n * 0.5 );
			s.right = sr / ( n * 0.5 );
			s.valid = finite && n > 0;
			frames[frame] = s;
		}
	};

	std::string WriteScene( const std::string& text, const std::string& tag )
	{
		char path[512];
		std::snprintf( path, sizeof(path), "/tmp/dl458_anim_%s_%d.RISEscene",
			tag.c_str(), static_cast<int>( ::getpid() ) );
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return std::string();
		ofs << text;
		return std::string( path );
	}

	//! Renders `scenePath` once with the given salt.  frames==0 -> still
	//! (IJob::Rasterize); otherwise RasterizeAnimation(t0, t1, frames).
	std::map<unsigned int, FrameStat> RenderOnce(
		const std::string& scenePath, uint32_t salt,
		unsigned int frames, double t0, double t1, bool fields = false )
	{
		std::map<unsigned int, FrameStat> out;
		IJobPriv* pJob = nullptr;
		if( !RISE_CreateJobPriv( &pJob ) || !pJob ) return out;
		if( !pJob->LoadAsciiSceneViaCst( scenePath.c_str() ) ) {
			safe_release( pJob );
			return out;
		}
		pJob->RemoveRasterizerOutputs();
		CapturingOutput* pCap = new CapturingOutput();
		GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "capture" );
		pJob->GetRasterizer()->AddRasterizerOutput( pCap );

		SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 0x458u, salt ) );
		// DL-468: MLT's chains are otherwise a deterministic function of
		// the scene, so every repeat returned the identical image (se 0).
		PSSMLTSamplerTestHooks::SeedSalt().store( SobolSequence::HashCombine( 0x468u, salt ) | 1u );
		std::srand( 1000u + salt );
		if( frames == 0 ) {
			pJob->Rasterize();
		} else {
			pJob->RasterizeAnimation( t0, t1, frames, fields, false );
		}
		SobolSamplerTestHooks::ValueSalt().store( 0u );
		PSSMLTSamplerTestHooks::SeedSalt().store( 0u );

		out = pCap->frames;
		safe_release( pCap );
		safe_release( pJob );
		return out;
	}

	struct Stat { double m = 0, se = 0; int n = 0; };

	Stat Summarize( const std::vector<double>& v )
	{
		Stat s;
		s.n = int( v.size() );
		if( s.n == 0 ) return s;
		for( double x : v ) s.m += x;
		s.m /= s.n;
		double var = 0;
		for( double x : v ) var += ( x - s.m ) * ( x - s.m );
		var = s.n > 1 ? var / ( s.n - 1 ) : 0;
		s.se = std::sqrt( var / s.n );
		return s;
	}

	bool Agree( const Stat& a, const Stat& b, std::string& detail )
	{
		const double d = std::fabs( a.m - b.m );
		const double se = std::sqrt( a.se * a.se + b.se * b.se );
		const double tol = 3.0 * se + 0.02 * std::max( std::fabs( a.m ), std::fabs( b.m ) );
		char buf[256];
		std::snprintf( buf, sizeof(buf), "(%.6g +/- %.3g vs %.6g +/- %.3g, |d|=%.3g tol=%.3g)",
			a.m, a.se, b.m, b.se, d, tol );
		detail = buf;
		return a.n > 0 && b.n > 0 && d <= tol && a.m > 0 && b.m > 0;
	}

	// Salted repeats per render set.  MLT's single-render spread on this
	// rig is several percent (MLT-spectral ~6 %), so its cases take 12
	// (DL-468); the other rasterizers keep 4.
	int kRepeats = 4;

	//////////////////////////////////////////////////////////////////
	// Scene pieces
	//////////////////////////////////////////////////////////////////
	const char* kShader =
		"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

	std::string Body( bool withEmitter, const char* omniPos, bool keyframed )
	{
		std::string s;
		s += "film\n{\n\twidth 24\n\theight 24\n}\n\n";
		s += "pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n";
		s += "uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.6 0.6 0.6\n}\n\n";
		s += "lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n";
		s += "clippedplane_geometry\n{\n\tname quad\n\tpta -2 -2 0\n\tptb 2 -2 0\n\tptc 2 2 0\n\tptd -2 2 0\n}\n\n";
		s += "standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n";
		s += std::string( "omni_light\n{\n\tname l_omni\n\tpower 6.0\n\tcolor 1.0 1.0 1.0\n\tposition " ) + omniPos + "\n}\n\n";
		if( withEmitter ) {
			s += "uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n";
			s += "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 4.0\n\tmaterial none\n}\n\n";
			s += "clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.4 1.6 2.0\n\tptb 0.4 1.6 2.0\n\tptc 0.4 0.8 2.0\n\tptd -0.4 0.8 2.0\n}\n\n";
			s += "standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n\n";
		}
		if( keyframed ) {
			s += "timeline\n{\n\telement_type light\n\telement l_omni\n\tparam position\n\n"
			     "\ttime 0.0\n\tvalue -1.5 0 1.0\n\n"
			     "\ttime 1.0\n\tvalue 1.5 0 1.0\n}\n\n";
		}
		return s;
	}

	//! Case E body: emitter-only (no delta light), the emitter's object
	//! scale either static or keyframed 1 -> 0.5 (area 4x smaller).
	std::string BodyE( const char* scale, bool keyframed )
	{
		std::string s;
		s += "film\n{\n\twidth 24\n\theight 24\n}\n\n";
		s += "pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n";
		s += "uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.6 0.6 0.6\n}\n\n";
		s += "lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n";
		s += "clippedplane_geometry\n{\n\tname quad\n\tpta -2 -2 0\n\tptb 2 -2 0\n\tptc 2 2 0\n\tptd -2 2 0\n}\n\n";
		s += "standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n";
		s += "uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n";
		s += "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 4.0\n\tmaterial none\n}\n\n";
		s += "clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 0\n\tptb 0.6 0.6 0\n\tptc 0.6 -0.6 0\n\tptd -0.6 -0.6 0\n}\n\n";
		s += std::string( "standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n\tposition 0 0 1.2\n\tscale " ) + scale + "\n}\n\n";
		if( keyframed ) {
			s += "timeline\n{\n\telement_type object\n\telement obj_emit\n\tparam scale\n\n"
			     "\ttime 0.0\n\tvalue 1 1 1\n\n"
			     "\ttime 1.0\n\tvalue 0.5 0.5 0.5\n}\n\n";
		}
		return s;
	}

	//! Case F body: a diffuse quad lit by an omni light behind the camera
	//! both directly and through a perfect mirror further behind it.  The
	//! mirrored component (L - S - D - E with a delta light) is reachable
	//! by BDPT/VCM ONLY through light tracing (t == 1 splats), so a
	//! dropped splat layer shows as a ~20% deficit here (it is a few
	//! percent at most on the case-A scene).
	std::string BodyF()
	{
		std::string s;
		s += "film\n{\n\twidth 24\n\theight 24\n}\n\n";
		s += "pinhole_camera\n{\n\tlocation 0 0 4\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n";
		s += "uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.6 0.6 0.6\n}\n\n";
		s += "lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n";
		s += "uniformcolor_painter\n{\n\tname pnt_white\n\tcolor 1.0 1.0 1.0\n}\n\n";
		s += "perfectreflector_material\n{\n\tname mat_mirror\n\treflectance pnt_white\n}\n\n";
		s += "clippedplane_geometry\n{\n\tname quad\n\tpta -2 -2 0\n\tptb 2 -2 0\n\tptc 2 2 0\n\tptd -2 2 0\n}\n\n";
		s += "standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n";
		s += "clippedplane_geometry\n{\n\tname mirror\n\tpta -4 -4 5\n\tptb 4 -4 5\n\tptc 4 4 5\n\tptd -4 4 5\n}\n\n";
		s += "standard_object\n{\n\tname obj_mirror\n\tgeometry mirror\n\tmaterial mat_mirror\n}\n\n";
		s += "omni_light\n{\n\tname l_omni\n\tpower 60.0\n\tcolor 1.0 1.0 1.0\n\tposition 0 0 4.6\n}\n\n";
		return s;
	}

	struct RastCase
	{
		const char* label;
		const char* chunk;
	};

	const RastCase kCases[] = {
		{ "PT",  "pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" },
		{ "PT-spectral", "pathtracing_spectral_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" },
		{ "BDPT", "bdpt_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" },
		{ "BDPT-spectral", "bdpt_spectral_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" },
		{ "VCM", "vcm_pel_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" },
		{ "VCM-spectral", "vcm_spectral_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n\n" },
		{ "MLT", "mlt_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tbootstrap_samples 20000\n\tchains 64\n\tmutations_per_pixel 16\n\tlarge_step_prob 0.3\n\toidn_denoise FALSE\n}\n\n" },
		{ "MLT-spectral", "mlt_spectral_rasterizer\n{\n\tmax_eye_depth 3\n\tmax_light_depth 3\n\tbootstrap_samples 20000\n\tchains 64\n\tmutations_per_pixel 16\n\tlarge_step_prob 0.3\n\toidn_denoise FALSE\n}\n\n" },
	};

	std::string Scene( const RastCase& rc, const std::string& body )
	{
		return std::string( "RISE ASCII SCENE 7\n" ) + kShader + rc.chunk + body;
	}

	//! Collects per-frame means (or left/right) over kRepeats salted renders.
	std::vector<std::vector<FrameStat>> Collect(
		const std::string& path, unsigned int frames, double t0, double t1, uint32_t saltBase,
		bool fields = false )
	{
		const unsigned int nf = frames == 0 ? 1 : frames;
		std::vector<std::vector<FrameStat>> perFrame( nf );
		for( int r = 0; r < kRepeats; r++ ) {
			const std::map<unsigned int, FrameStat> m = RenderOnce( path, saltBase + r, frames, t0, t1, fields );
			for( unsigned int f = 0; f < nf; f++ ) {
				auto it = m.find( f );
				if( it != m.end() && it->second.valid ) perFrame[f].push_back( it->second );
			}
		}
		return perFrame;
	}

	Stat MeanOf( const std::vector<FrameStat>& v, int which )
	{
		std::vector<double> x;
		for( const FrameStat& s : v ) x.push_back( which == 0 ? s.mean : ( which == 1 ? s.left : s.right ) );
		return Summarize( x );
	}

	void RunCase( const RastCase& rc )
	{
		std::cout << "== " << rc.label << std::endl;
		kRepeats = ( std::string( rc.label ).compare( 0, 3, "MLT" ) == 0 ) ? 12 : 4;
		std::string detail;

		// --- A/B: static scene (emitter + omni), no timeline ---
		const std::string staticPath = WriteScene( Scene( rc, Body( true, "0 0 1.5", false ) ), std::string( rc.label ) + "_static" );
		const auto still = Collect( staticPath, 0, 0, 0, 100 );
		const Stat sStill = MeanOf( still[0], 0 );

		const auto animA = Collect( staticPath, 1, 0.0, 0.0, 200 );
		const Stat sA = MeanOf( animA[0], 0 );
		const bool okA = Agree( sStill, sA, detail );
		std::cout << "  A still vs anim(1 frame, exposure 0) " << detail << std::endl;
		Check( okA, std::string( rc.label ) + ": A no-timeline single-frame animation matches still " + detail );

		// D: absolute check against a PT still of the same scene.  A and B
		// alone cannot see a defect that drops the same layer from the
		// still AND the animation (e.g. the splat film composited in a
		// shared Flush* override); PT has no splat layer.  5% band: the
		// integrators agree to ~0.3% on this scene, while losing BDPT's /
		// VCM's t==1 splat layer or NEE costs 15-99%.  Every rasterizer,
		// MLT-spectral included since DL-461 (it used to render at ~0.26x
		// in both paths: missing spectral integral normalization).
		{
			static Stat ptRef;
			static bool havePT = false;
			if( !havePT ) {
				const std::string ptPath = WriteScene( Scene( kCases[0], Body( true, "0 0 1.5", false ) ), "PT_ref" );
				ptRef = MeanOf( Collect( ptPath, 0, 0, 0, 1000 )[0], 0 );
				std::remove( ptPath.c_str() );
				havePT = true;
			}
			const double rel = ptRef.m > 0 ? sA.m / ptRef.m : 0;
			std::cout << "  D anim(1 frame) / PT still = " << rel << "  (anim " << sA.m << " +/- " << sA.se << ", PT " << ptRef.m << " +/- " << ptRef.se << ", n " << sA.n << ")" << std::endl;
			// DL-468: gate on the measured spread, never on one
			// realization.  MLT used to be a deterministic function of
			// the scene (se exactly 0), so this row gated ONE chain set
			// whose single-render sd is ~6 % on this 24 x 24 rig for
			// MLT-spectral (hwss FALSE chroma noise under a Y-only
			// target): measured 16 salted repeats read 0.995 +/- 0.015
			// (no bias), while the unsalted value at bootstrap 20000..20010
			// ranged 0.916 .. 1.008.  The tolerance is 3 combined
			// relative standard errors, floored at the 5 % band the row
			// was designed with (dropping a splat layer or NEE costs
			// 15-99 %, the MLT cases run 12 repeats, giving MLT-spectral ~5 % at 3 se).
			const double seRel = ( ptRef.m > 0 && sA.m > 0 )
				? rel * std::sqrt( ( sA.se / sA.m ) * ( sA.se / sA.m ) + ( ptRef.se / ptRef.m ) * ( ptRef.se / ptRef.m ) )
				: 0;
			const double tolD = std::max( 3.0 * seRel, 0.05 );
			std::cout << "  D tolerance " << tolD << " (3 se = " << 3.0 * seRel << ")" << std::endl;
			Check( sA.n > 1 && std::fabs( rel - 1.0 ) < tolD, std::string( rc.label ) + ": D animation frame agrees with the PT still within max(3 se, 5%) (ratio " + std::to_string( rel ) + ")" );
		}

		// --- F: interlaced fields (DL-462).  Each field is its own
		// RenderFrameOfAnimation call into ONE image flushed once; the
		// second field's PreRenderSetup used to delete the first field's
		// splat film, so BDPT/VCM splat layers came out at ~half energy.
		// Static scene (BodyF: a mirror-reflected delta light only light
		// tracing reaches), so the field frame must match the still.
		{
			const std::string lbl( rc.label );
			if( lbl == "BDPT" || lbl == "BDPT-spectral" || lbl == "VCM" || lbl == "VCM-spectral" ) {
				// VCM's merges also reach the mirrored path, which dilutes the
				// splat share; the VC-only variant (vm_enabled FALSE) leaves
				// light tracing as its only strategy, as in BDPT.
				const bool isVCM = lbl.compare( 0, 3, "VCM" ) == 0;
				for( int variant = 0; variant < ( isVCM ? 2 : 1 ); variant++ ) {
					std::string chunk( rc.chunk );
					std::string tag = lbl;
					if( variant == 1 ) {
						chunk.insert( chunk.find( "{\n" ) + 2, "\tvm_enabled FALSE\n" );
						tag += " (VC only)";
					}
					const std::string fPath = WriteScene(
						std::string( "RISE ASCII SCENE 7\n" ) + kShader + chunk + BodyF(),
						lbl + "_f" + std::to_string( variant ) );
					const Stat sFStill = MeanOf( Collect( fPath, 0, 0, 0, 1000 )[0], 0 );
					const Stat sF = MeanOf( Collect( fPath, 1, 0.0, 0.0, 1100, true )[0], 0 );
					std::remove( fPath.c_str() );
					const bool okF = Agree( sFStill, sF, detail );
					std::cout << "  F " << tag << " still vs anim(1 frame, interlaced fields) " << detail << std::endl;
					Check( okF, tag + ": F interlaced-field animation frame matches still " + detail );
				}
			}
		}

		const auto animB = Collect( staticPath, 2, 0.0, 1.0, 300 );
		for( unsigned int f = 0; f < 2; f++ ) {
			const Stat sB = MeanOf( animB[f], 0 );
			const bool okB = Agree( sStill, sB, detail );
			std::cout << "  B still vs anim frame " << f << " " << detail << std::endl;
			Check( okB, std::string( rc.label ) + ": B two-frame animation frame " + std::to_string( f ) + " matches still " + detail );
		}

		// --- C: keyframed omni light (omni only: BDPT/VCM/MLT reach it
		// only through NEE / light subpaths, never by an eye hit) ---
		const std::string animPath  = WriteScene( Scene( rc, Body( false, "-1.5 0 1.0", true ) ), std::string( rc.label ) + "_kf" );
		const std::string leftPath  = WriteScene( Scene( rc, Body( false, "-1.5 0 1.0", false ) ), std::string( rc.label ) + "_left" );
		const std::string rightPath = WriteScene( Scene( rc, Body( false, "1.5 0 1.0", false ) ), std::string( rc.label ) + "_right" );
		const auto kf = Collect( animPath, 2, 0.0, 1.0, 400 );
		const auto refL = Collect( leftPath, 0, 0, 0, 500 );
		const auto refR = Collect( rightPath, 0, 0, 0, 600 );
		for( unsigned int f = 0; f < 2; f++ ) {
			const auto& ref = ( f == 0 ) ? refL[0] : refR[0];
			for( int w = 1; w <= 2; w++ ) {
				const Stat a = MeanOf( kf[f], w );
				const Stat b = MeanOf( ref, w );
				const bool ok = Agree( b, a, detail );
				const char* half = ( w == 1 ) ? "left" : "right";
				std::cout << "  C frame " << f << " " << half << " still(static light) vs anim " << detail << std::endl;
				Check( ok, std::string( rc.label ) + ": C keyframed-light frame " + std::to_string( f ) + " " + half + " half matches still " + detail );
			}
		}
		const Stat l0 = MeanOf( kf[0], 1 ), r0 = MeanOf( kf[0], 2 );
		const Stat l1 = MeanOf( kf[1], 1 ), r1 = MeanOf( kf[1], 2 );
		Check( l0.m > 1.5 * r0.m, std::string( rc.label ) + ": C frame 0 lit on the left" );
		Check( r1.m > 1.5 * l1.m, std::string( rc.label ) + ": C frame 1 lit on the right" );

		// --- E: keyframed luminary SIZE (area changes per frame) ---
		const std::string ePath  = WriteScene( Scene( rc, BodyE( "1 1 1", true ) ), std::string( rc.label ) + "_e" );
		const std::string e0Path = WriteScene( Scene( rc, BodyE( "1 1 1", false ) ), std::string( rc.label ) + "_e0" );
		const std::string e1Path = WriteScene( Scene( rc, BodyE( "0.5 0.5 0.5", false ) ), std::string( rc.label ) + "_e1" );
		const auto ek = Collect( ePath, 2, 0.0, 1.0, 700 );
		const auto e0 = Collect( e0Path, 0, 0, 0, 800 );
		const auto e1 = Collect( e1Path, 0, 0, 0, 900 );
		for( unsigned int f = 0; f < 2; f++ ) {
			const Stat a = MeanOf( ek[f], 0 );
			const Stat b = MeanOf( ( f == 0 ? e0 : e1 )[0], 0 );
			const bool ok = Agree( b, a, detail );
			std::cout << "  E frame " << f << " still(static size) vs anim " << detail << std::endl;
			Check( ok, std::string( rc.label ) + ": E keyframed-luminary-size frame " + std::to_string( f ) + " matches still " + detail );
		}
		std::remove( ePath.c_str() );
		std::remove( e0Path.c_str() );
		std::remove( e1Path.c_str() );

		std::remove( staticPath.c_str() );
		std::remove( animPath.c_str() );
		std::remove( leftPath.c_str() );
		std::remove( rightPath.c_str() );
	}
}

int main( int argc, char** argv )
{
	std::cout << "AnimationRasterizerParityTest (DL-458)" << std::endl;
	const std::string only = argc > 1 ? argv[1] : "";
	for( const RastCase& rc : kCases ) {
		if( !only.empty() && only != rc.label ) continue;
		RunCase( rc );
	}
	std::cout << "\nAnimationRasterizerParityTest: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
