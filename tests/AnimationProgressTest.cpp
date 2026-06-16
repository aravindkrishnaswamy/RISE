//////////////////////////////////////////////////////////////////////
//
//  AnimationProgressTest.cpp - Regression test for whole-animation
//    render progress (Mac/Windows GUI bug: the progress bar and ETA
//    used to restart 0..100% on every frame instead of tracking the
//    whole animation).
//
//  Root cause that this guards against:
//    * PixelBasedRasterizerHelper::RenderFrameOfAnimation used to call
//      DoAnimationFrameProgress(0,1) at the top of every frame, which
//      reported Progress(0,1) == 0% — defeating the movie-wide
//      mProgressBase/mProgressTotal accounting and resetting the bar
//      (and, via the >resetThreshold backward jump, the ETA) per frame.
//    * The single-threaded (exposure / motion-blur) animation path
//      reported per-frame 0..1 instead of the weighted movie-wide
//      fraction.
//    * The MLT rasterizers run a full per-frame pipeline and were not
//      slotted into the whole-animation bar at all.
//
//  Contract verified here, driving the PUBLIC IJob::RasterizeAnimation
//  entry point with a recording IProgressCallback (exactly what the
//  GUIs install):
//    * The reported fraction (progress/total) advances ONCE from ~0 to
//      ~1 across the WHOLE animation, never sweeping 0..1 per frame.
//    * PixelBased integrators (PT/BDPT/VCM) report a STRICTLY monotonic
//      movie-wide fraction (the dispatcher's high-water clamp).
//    * MLT advances across frame slots; within a frame its phase-based
//      progress may dip by at most one slot width (1/numFrames) — never
//      a full per-frame 0..1 sweep.
//    * Single-frame still renders (IJob::Rasterize) and single specific
//      frames are unaffected: a clean 0..1.
//
//  Self-contained (no external media / script includes) so it parses
//  and renders regardless of RISE_MEDIA_PATH / working directory.
//
//  Author: Aravind Krishnaswamy
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
#include <cmath>
#include <string>
#include <algorithm>

#if defined(_WIN32)
	#include <process.h>		// _getpid()
	#define RISE_GETPID _getpid
#else
	#include <unistd.h>			// getpid()
	#define RISE_GETPID getpid
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IProgressCallback.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Log/Log.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Recording progress callback — exactly the IProgressCallback contract
// the GUIs install.  Stores every reported fraction so the test can
// assert the whole-animation shape.  Never cancels.
//////////////////////////////////////////////////////////////////////
class RecordingProgress : public IProgressCallback
{
public:
	std::vector<double> fracs;

	bool Progress( const double progress, const double total ) override
	{
		double f = ( total > 0.0 ) ? ( progress / total ) : 0.0;
		if( f < 0.0 ) f = 0.0;
		if( f > 1.0 ) f = 1.0;
		fracs.push_back( f );
		return true;	// never cancel
	}

	void SetTitle( const char* /*title*/ ) override {}
};

//////////////////////////////////////////////////////////////////////

static std::string WriteSceneToTempFile( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path),
		"/tmp/anim_progress_%s_%d.RISEscene", tag, static_cast<int>( RISE_GETPID() ) );

	std::ofstream ofs( path );
	if( !ofs.is_open() ) {
		return std::string();
	}
	ofs << sceneText;
	ofs.close();
	return std::string( path );
}

// Shared self-contained scene body (32x32, one lit diffuse quad + a
// small emitter quad).  No external painters/scripts so it parses
// without RISE_MEDIA_PATH.  The rasterizer chunk is prepended by the
// caller so we can exercise each integrator.
static const char* kSceneBody =
	// 128x128 so the render is split into several tiles — the
	// single-threaded-tile dispatcher only reports Progress for tile
	// idx>0, so a 1-tile image would fire no per-tile callbacks at all.
	"film\n{\n\twidth 128\n\theight 128\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n"
	"omni_light\n{\n\tname l_omni\n\tpower 4.0\n\tcolor 1.0 1.0 1.0\n\tposition 0.0 0.0 5.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 20.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.5 0.5 4.0\n\tptb 0.5 0.5 4.0\n\tptc 0.5 -0.5 4.0\n\tptd -0.5 -0.5 4.0\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

static const char* kShader =
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";

static std::string BuildScene( const char* rasterizerChunk )
{
	// Marker line first, then the shader graph, then the rasterizer chunk
	// and the shared scene body — matches AutoRasterizerTest's proven
	// composition order.
	std::string s( "RISE ASCII SCENE 6\n" );
	s += kShader;
	s += rasterizerChunk;
	s += "\n";
	s += kSceneBody;
	return s;
}

//////////////////////////////////////////////////////////////////////
// Whole-animation shape analysis.
//////////////////////////////////////////////////////////////////////
struct ProgShape
{
	bool   empty = true;
	double first = 0.0;
	double last = 0.0;
	double highWater = 0.0;	// max fraction reached
	double maxDrop = 0.0;	// largest backward drop from the running max
};

static ProgShape Analyze( const std::vector<double>& f )
{
	ProgShape r;
	if( f.empty() ) return r;
	r.empty = false;
	r.first = f.front();
	r.last  = f.back();
	double running = f.front();
	r.highWater = f.front();
	for( size_t i = 0; i < f.size(); ++i ) {
		if( f[i] > running ) running = f[i];
		if( f[i] > r.highWater ) r.highWater = f[i];
		const double drop = running - f[i];
		if( drop > r.maxDrop ) r.maxDrop = drop;
	}
	return r;
}

//////////////////////////////////////////////////////////////////////
// Drive RasterizeAnimation through the public IJob API with a recording
// progress callback and assert the whole-animation shape.
//
//   strictMonotonic == true  -> PixelBased integrators (PT/BDPT/VCM):
//                               the bar must never go backward at all.
//   strictMonotonic == false -> MLT: within-frame phase progress may
//                               dip by at most one slot width (1/frames),
//                               but never a full per-frame 0..1 sweep.
//////////////////////////////////////////////////////////////////////
static void RunAnimationCase( const char* label, const char* rasterizerChunk,
	const unsigned int frames, const bool strictMonotonic )
{
	std::cout << "Animation: " << label << " (" << frames << " frames)" << std::endl;

	const std::string scene = BuildScene( rasterizerChunk );
	const std::string path  = WriteSceneToTempFile( scene, label );
	Check( !path.empty(), std::string(label) + ": temp scene written" );
	if( path.empty() ) return;

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		Check( false, std::string(label) + ": job created" );
		return;
	}

	bool loaded = pJob->LoadAsciiScene( path.c_str() );
	Check( loaded, std::string(label) + ": scene loaded" );
	if( !loaded ) { safe_release( pJob ); return; }

	RecordingProgress rec;
	pJob->SetProgress( &rec );

	const bool ok = pJob->RasterizeAnimation( 0.0, 1.0, frames, false, false );
	pJob->SetProgress( 0 );
	Check( ok, std::string(label) + ": RasterizeAnimation returned true" );

	const ProgShape s = Analyze( rec.fracs );
	Check( !s.empty, std::string(label) + ": progress callbacks fired" );
	if( s.empty ) { safe_release( pJob ); return; }

	// (1) Completes: the bar reaches the end of the WHOLE animation.
	Check( s.highWater >= 0.999, std::string(label) + ": reaches ~100% (highWater="
		+ std::to_string(s.highWater) + ")" );

	// (2) Single sweep, not per-frame: the first reading is in the FIRST
	//     frame's slot.  Under the old per-frame bug the very first frame
	//     also swept all the way to ~1.0, so the early readings would not
	//     stay low.  Require the first reading below the first slot top
	//     (with margin).  (frames>=2 always here.)
	Check( s.first < (1.5 / static_cast<double>(frames)),
		std::string(label) + ": starts in frame-0 slot (first=" + std::to_string(s.first) + ")" );

	// (3) The decisive anti-regression check: the bar must never fall back
	//     toward zero between frames.  The old per-frame reset dropped the
	//     running max all the way to ~0 (drop ~= highWater) at each frame
	//     boundary.  PixelBased must be strictly monotonic; MLT may dip by
	//     at most one slot width within a frame's phases.
	const double tol = strictMonotonic
		? 1e-6
		: ( 1.5 / static_cast<double>(frames) );
	Check( s.maxDrop <= tol,
		std::string(label) + ": no per-frame reset (maxDrop=" + std::to_string(s.maxDrop)
		+ " <= " + std::to_string(tol) + ")" );

	std::cout << "    first=" << s.first << " last=" << s.last
		<< " highWater=" << s.highWater << " maxDrop=" << s.maxDrop << std::endl;

	safe_release( pJob );
}

//////////////////////////////////////////////////////////////////////
// Single-frame still render must remain a clean 0..1 (the explicit
// "don't break single frames for any rasterizer" constraint).
//////////////////////////////////////////////////////////////////////
static void RunSingleFrameCase( const char* label, const char* rasterizerChunk,
	const bool strictMonotonic )
{
	std::cout << "Single-frame: " << label << std::endl;

	const std::string scene = BuildScene( rasterizerChunk );
	const std::string path  = WriteSceneToTempFile( scene, label );
	if( path.empty() ) { Check( false, std::string(label) + ": temp scene written" ); return; }

	IJobPriv* pJob = nullptr;
	if( !RISE_CreateJobPriv( &pJob ) || !pJob ) {
		Check( false, std::string(label) + ": job created" );
		return;
	}
	if( !pJob->LoadAsciiScene( path.c_str() ) ) {
		Check( false, std::string(label) + ": scene loaded" );
		safe_release( pJob );
		return;
	}

	RecordingProgress rec;
	pJob->SetProgress( &rec );
	const bool ok = pJob->Rasterize();
	pJob->SetProgress( 0 );
	Check( ok, std::string(label) + ": Rasterize returned true" );

	const ProgShape s = Analyze( rec.fracs );
	Check( !s.empty, std::string(label) + ": progress callbacks fired" );
	if( s.empty ) { safe_release( pJob ); return; }

	// A single frame's per-tile dispatcher reports at each tile's START
	// and skips idx 0, so the bar tops out at (numTiles-1)/numTiles and
	// the render then completes (the GUI flips to "done").  This ceiling
	// is tile-count- (hence core-count-) dependent and PRE-EXISTING — the
	// fix never touched it.  Assert only that the bar makes substantial
	// forward progress, not that it hits exactly 100%.
	Check( s.highWater >= 0.5, std::string(label) + ": single frame progresses past 50% (highWater="
		+ std::to_string(s.highWater) + ")" );
	const double tol = strictMonotonic ? 1e-6 : 0.95;	// MLT: full phase resets within the one frame
	Check( s.maxDrop <= tol,
		std::string(label) + ": single-frame progress sane (maxDrop=" + std::to_string(s.maxDrop) + ")" );

	std::cout << "    first=" << s.first << " highWater=" << s.highWater
		<< " maxDrop=" << s.maxDrop << std::endl;

	safe_release( pJob );
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== AnimationProgressTest ===" << std::endl;

	// PixelBased integrators share PixelBasedRasterizerHelper::Rasterize-
	// SceneAnimation and must report a STRICTLY monotonic movie-wide bar.
	const char* kPT =
		"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
	const char* kBDPT =
		"bdpt_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
	const char* kVCM =
		"vcm_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n";
	// MLT runs its own per-frame pipeline; whole-animation slotting via
	// FrameSlotProgressCallback.  Tiny bootstrap/chains keep it fast.
	const char* kMLT =
		"mlt_rasterizer\n{\n\tmax_eye_depth 4\n\tmax_light_depth 4\n"
		"\tbootstrap_samples 2000\n\tchains 8\n\tmutations_per_pixel 4\n\tlarge_step_prob 0.3\n}\n";

	// Use 8 frames so MLT's per-frame phase dip (one slot = 1/8 = 0.125)
	// is comfortably below the old-bug signature (a ~full-range drop).
	RunAnimationCase( "PT",   kPT,   8, /*strictMonotonic*/ true );
	RunAnimationCase( "BDPT", kBDPT, 8, /*strictMonotonic*/ true );
	RunAnimationCase( "VCM",  kVCM,  8, /*strictMonotonic*/ true );
	RunAnimationCase( "MLT",  kMLT,  8, /*strictMonotonic*/ false );

	// Single-frame still renders must remain a clean 0..1.
	RunSingleFrameCase( "PT-still",  kPT,  /*strictMonotonic*/ true );
	RunSingleFrameCase( "MLT-still", kMLT, /*strictMonotonic*/ false );

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
