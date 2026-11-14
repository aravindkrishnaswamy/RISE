//////////////////////////////////////////////////////////////////////
//
//  CallbackOutputRenderScopeTest.cpp - Regression test for the callback
//    rasterizer output's frame-sized conversion buffers surviving a
//    render that never reaches a final delivery.
//
//  Job::AddCallbackRasterizerOutput installs a dispatcher that converts
//  each delivered region into a FULL-FRAME buffer (16 bytes/pixel for
//  float delivery) reused across a render's intermediate tile deliveries.
//  Before the fix the buffer was freed only after a final OutputImage, so
//  a render that threw (receiver or another output), was cancelled from
//  the progress callback, or was a cancelled animation kept it for the
//  Job's lifetime, and a retry at the same size/format REUSED it without
//  re-zeroing: the receiver, which is handed the whole frame with only the
//  valid rect alongside, could read the previous render's pixels.
//
//  Every case drives the real IJob entry points (Rasterize,
//  RasterizeRegion, RasterizeAnimation) on a real scene, then asserts
//    (a) no attached callback dispatcher owns a conversion buffer, and
//    (b) a retry render (a small RasterizeRegion) never shows the
//        receiver a nonzero pixel outside the rects already delivered IN
//        THAT render (the stale-content detector inspects the whole
//        buffer on every delivery).
//  Both receiver formats (RGBA16, RGBA32F) and two integrators (PT, BDPT)
//  are covered.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>

#if defined(_WIN32)
	#include <process.h>
	#define RISE_GETPID _getpid
#else
	#include <unistd.h>
	#define RISE_GETPID getpid
#endif

#include "../src/Library/Interfaces/IJob.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IJobRasterizerOutput.h"
#include "../src/Library/Interfaces/IProgressCallback.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Utilities/RadianceMapConfig.h"
#include "../src/Library/Utilities/PixelFilterConfig.h"
#include "../src/Library/Utilities/PathGuidingField.h"
#include "../src/Library/Utilities/AdaptiveSamplingConfig.h"
#include "../src/Library/Utilities/StabilityConfig.h"
#include "../src/Library/Utilities/ProgressiveConfig.h"
#include "../src/Library/Utilities/OidnConfig.h"
#include "../src/Library/Utilities/Reference.h"
#include "TestTempDir.h"

using namespace RISE;

namespace RISE
{
	bool RISE_CreateJobPriv( IJobPriv** ppi );
	namespace Implementation {
		bool ForTest_CallbackOutputsHoldRenderBuffers( const IRasterizer& rasterizer );
		size_t ForTest_JobCallbackOutputRegistrySize( IJob& job );
	}
}

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& name )
{
	if( condition ) {
		++passCount;
	} else {
		++failCount;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Receiver: inspects the whole delivered buffer on every call.
//////////////////////////////////////////////////////////////////////
struct InspectingReceiver final : IJobRasterizerOutput
{
	bool wantsFloat = true;
	// Receiver calls are serialized by the dispatcher's mutex, but the
	// progress callback reads `deliveries` from other render workers.
	std::atomic<unsigned int> deliveries{ 0 };	// this render
	unsigned int throwOnDelivery = 0;	// 1-based; 0 = never
	unsigned int staleDeliveries = 0;	// deliveries that showed foreign content
	unsigned long long nonzeroDelivered = 0;
	unsigned int w = 0, h = 0;
	std::vector<unsigned char> mask;	// pixels delivered so far this render

	void BeginRender() { deliveries = 0; staleDeliveries = 0; nonzeroDelivered = 0; mask.clear(); w = h = 0; }

	bool PremultipliedAlpha() override { return false; }
	int GetColorSpace() override { return 0; }
	bool WantsFloat32() override { return wantsFloat; }

	template<class T>
	void Inspect( const T* data, unsigned int width, unsigned int height,
		unsigned int top, unsigned int left, unsigned int bottom, unsigned int right )
	{
		++deliveries;
		if( w != width || h != height ) {
			w = width; h = height; mask.assign( size_t(width)*height, 0 );
		}
		bool stale = false;
		for( unsigned int y = 0; y < height; ++y ) {
			for( unsigned int x = 0; x < width; ++x ) {
				const size_t i = size_t(y)*width + x;
				const bool inRect = y >= top && y <= bottom && x >= left && x <= right;
				bool nonzero = false;
				for( int c = 0; c < 4; ++c ) nonzero = nonzero || data[i*4+c] != 0;
				if( inRect ) {
					if( nonzero ) ++nonzeroDelivered;
				} else if( !mask[i] && nonzero ) {
					stale = true;
				}
			}
		}
		if( stale ) ++staleDeliveries;
		for( unsigned int y = top; y <= bottom && y < height; ++y )
			for( unsigned int x = left; x <= right && x < width; ++x )
				mask[size_t(y)*width + x] = 1;
		if( throwOnDelivery && deliveries.load() == throwOnDelivery ) {
			throw std::runtime_error( "receiver failure" );
		}
	}

	void OutputImageRGBA16( const unsigned short* d, unsigned int width, unsigned int height,
		unsigned int top, unsigned int left, unsigned int bottom, unsigned int right ) override
	{
		Inspect( d, width, height, top, left, bottom, right );
	}
	void OutputImageRGBA32F( const float* d, unsigned int width, unsigned int height,
		unsigned int top, unsigned int left, unsigned int bottom, unsigned int right ) override
	{
		Inspect( d, width, height, top, left, bottom, right );
	}
};

//! An output listed AFTER the callback output; throws on a chosen
//! intermediate delivery, i.e. after the callback output already succeeded.
struct ThrowingOutput final : IRasterizerOutput, Implementation::Reference
{
	// Non-callback outputs are called concurrently from render workers.
	std::atomic<unsigned int> calls{ 0 };
	std::atomic<unsigned int> throwOnCall{ 0 };
	void OutputIntermediateImage( const IRasterImage&, const Rect* ) override
	{
		const unsigned int n = ++calls;
		const unsigned int t = throwOnCall.load();
		if( t && n == t ) throw std::runtime_error( "other output failure" );
	}
	void OutputImage( const IRasterImage&, const Rect*, const unsigned int ) override {}
};

//! Cancels once the receiver has seen `cancelAfter` deliveries.
struct CancellingProgress final : IProgressCallback
{
	const InspectingReceiver* receiver = nullptr;
	std::atomic<unsigned int> cancelAfter{ 0 };	// 0 = never cancel
	std::atomic<unsigned int> calls{ 0 };
	bool Progress( const double, const double ) override
	{
		++calls;
		const unsigned int c = cancelAfter.load();
		return !( c && receiver && receiver->deliveries.load() >= c );
	}
	void SetTitle( const char* ) override {}
};

static const char* kSceneBody =
	"film\n{\n\twidth 256\n\theight 256\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 30.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad\n\tpta -1 -1 0\n\tptb 1 -1 0\n\tptc 1 1 0\n\tptd -1 1 0\n}\n\n"
	"standard_object\n{\n\tname obj_quad\n\tgeometry quad\n\tmaterial mat_diffuse\n}\n\n"
	"omni_light\n{\n\tname l_omni\n\tpower 4.0\n\tcolor 1.0 1.0 1.0\n\tposition 0.0 0.0 5.0\n}\n";

static std::string WriteScene( const char* rasterizerChunk, const char* tag )
{
	std::string s( "RISE ASCII SCENE 7\n" );
	s += "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n";
	s += rasterizerChunk;
	s += "\n";
	s += kSceneBody;
	char path[1024];
	std::snprintf( path, sizeof(path), "%s/callback_scope_%s_%d.RISEscene",
		RISE::TestSupport::ScratchBaseDir().c_str(), tag, static_cast<int>( RISE_GETPID() ) );
	std::ofstream ofs( path );
	if( !ofs.is_open() ) return std::string();
	ofs << s;
	return std::string( path );
}

enum class Mode { ReceiverThrows, ProgressCancels, AnimationCancelled, OtherOutputThrows, Success };

static const char* ModeName( Mode m )
{
	switch( m ) {
	case Mode::ReceiverThrows: return "receiver throws on an intermediate delivery";
	case Mode::ProgressCancels: return "progress callback cancels mid-render";
	case Mode::AnimationCancelled: return "animation cancelled mid-frame";
	case Mode::OtherOutputThrows: return "a later output throws after the callback output succeeded";
	case Mode::Success: return "successful render";
	}
	return "?";
}

static void RunCase( const char* integrator, const char* rasterizerChunk, Mode mode, bool wantsFloat )
{
	const std::string label = std::string( integrator ) + ( wantsFloat ? " RGBA32F: " : " RGBA16: " ) + ModeName( mode );
	const std::string path = WriteScene( rasterizerChunk, integrator );
	Check( !path.empty(), label + ": scene written" );
	if( path.empty() ) return;

	IJobPriv* job = nullptr;
	if( !RISE_CreateJobPriv( &job ) || !job ) { Check( false, label + ": job created" ); return; }
	const bool loaded = job->LoadAsciiSceneViaCst( path.c_str() );
	std::remove( path.c_str() );
	Check( loaded, label + ": scene loaded" );
	if( !loaded ) { safe_release( job ); return; }

	InspectingReceiver receiver;
	receiver.wantsFloat = wantsFloat;
	Check( job->AddCallbackRasterizerOutput( &receiver ), label + ": callback output attached" );

	ThrowingOutput* other = new ThrowingOutput();
	job->GetRasterizer()->AddRasterizerOutput( other );

	CancellingProgress progress;
	progress.receiver = &receiver;
	job->SetProgress( &progress );

	// For the cancel cases, render the same job uncancelled first so the
	// cancelled render's delivery count has a reference.
	unsigned int uncancelledDeliveries = 0;
	const auto renderOnce = [&]() {
		if( mode == Mode::AnimationCancelled ) job->RasterizeAnimation( 0.0, 1.0, 3, false, false );
		else job->Rasterize();
	};
	if( mode == Mode::ProgressCancels || mode == Mode::AnimationCancelled ) {
		receiver.BeginRender();
		try { renderOnce(); } catch( const std::exception& ) {}
		uncancelledDeliveries = receiver.deliveries.load();
	}

	receiver.BeginRender();
	if( mode == Mode::ReceiverThrows ) receiver.throwOnDelivery = 2;
	if( mode == Mode::OtherOutputThrows ) other->throwOnCall = 2;
	if( mode == Mode::ProgressCancels || mode == Mode::AnimationCancelled ) progress.cancelAfter = 1;

	std::string error;
	try {
		renderOnce();
	} catch( const std::exception& e ) {
		error = e.what();
	}

	if( mode == Mode::ReceiverThrows ) {
		Check( error == "receiver failure", label + ": receiver failure propagated (got '" + error + "')" );
	} else if( mode == Mode::OtherOutputThrows ) {
		Check( error == "other output failure", label + ": other-output failure propagated (got '" + error + "')" );
	} else {
		Check( error.empty(), label + ": render returned without throwing (got '" + error + "')" );
	}
	Check( receiver.nonzeroDelivered > 0, label + ": first render delivered nonzero pixels" );
	if( mode == Mode::ProgressCancels || mode == Mode::AnimationCancelled ) {
		const unsigned int cancelled = receiver.deliveries.load();
		std::cout << "  " << label << ": " << cancelled << " deliveries cancelled vs "
			<< uncancelledDeliveries << " uncancelled" << std::endl;
		Check( uncancelledDeliveries >= 16u && cancelled < uncancelledDeliveries,
			label + ": cancel cut deliveries short (" + std::to_string( cancelled ) + " cancelled vs "
			+ std::to_string( uncancelledDeliveries ) + " uncancelled)" );
	}

	const bool heldAfterFirst = Implementation::ForTest_CallbackOutputsHoldRenderBuffers( *job->GetRasterizer() );
	Check( !heldAfterFirst, label + ": no callback conversion buffer held after the render" );

	// Retry: a small region render must never expose earlier content.
	receiver.BeginRender();
	receiver.throwOnDelivery = 0;
	other->throwOnCall = 0;
	progress.cancelAfter = 0;
	error.clear();
	bool retryOk = false;
	try {
		retryOk = job->RasterizeRegion( 4, 4, 11, 11 );
	} catch( const std::exception& e ) {
		error = e.what();
	}
	Check( retryOk && error.empty(), label + ": retry region render succeeded (" + error + ")" );
	Check( receiver.deliveries > 0, label + ": retry delivered" );
	Check( receiver.staleDeliveries == 0, label + ": retry never shows previous-render pixels outside the delivered rects ("
		+ std::to_string( receiver.staleDeliveries ) + " of " + std::to_string( receiver.deliveries.load() ) + " deliveries stale)" );
	Check( !Implementation::ForTest_CallbackOutputsHoldRenderBuffers( *job->GetRasterizer() ),
		label + ": no callback conversion buffer held after the retry" );

	job->SetProgress( 0 );
	safe_release( other );
	safe_release( job );
}

static IJobPriv* LoadJob( const char* rasterizerChunk, const char* tag, const std::string& label )
{
	const std::string path = WriteScene( rasterizerChunk, tag );
	IJobPriv* job = nullptr;
	if( path.empty() || !RISE_CreateJobPriv( &job ) || !job ) { Check( false, label + ": job created" ); return nullptr; }
	const bool loaded = job->LoadAsciiSceneViaCst( path.c_str() );
	std::remove( path.c_str() );
	Check( loaded, label + ": scene loaded" );
	if( !loaded ) { safe_release( job ); return nullptr; }
	return job;
}

static bool ReloadJob( IJobPriv& job, const char* rasterizerChunk, const char* tag )
{
	const std::string path = WriteScene( rasterizerChunk, tag );
	if( path.empty() ) return false;
	job.ClearAll();
	const bool loaded = job.LoadAsciiSceneViaCst( path.c_str() );
	std::remove( path.c_str() );
	return loaded;
}

static bool SetPixelPel( IJobPriv& job )
{
	return job.SetPixelBasedPelRasterizer( 1, 1, 2, "global", RadianceMapConfig(), "none", 0.0,
		PixelFilterConfig(), true, false, OidnQuality::Auto, OidnDevice::Auto, OidnPrefilter::Fast,
		PathGuidingConfig(), AdaptiveSamplingConfig(), StabilityConfig(), ProgressiveConfig() );
}

//! The Job's callback-output registry follows the lifetime of the
//! rasterizers its dispatchers are attached to.
static void TestRegistryLifetime( const char* kPT )
{
	const std::string label = "registry lifetime";
	IJobPriv* job = LoadJob( kPT, "registry", label );
	if( !job ) return;
	InspectingReceiver receiver;

	// (a) ClearAll + reload cycles never grow the registry.
	bool cyclesOk = true;
	for( int cycle = 0; cycle < 4; ++cycle ) {
		cyclesOk = cyclesOk && job->AddCallbackRasterizerOutput( &receiver )
			&& Implementation::ForTest_JobCallbackOutputRegistrySize( *job ) == 1;
		receiver.BeginRender();
		job->RasterizeRegion( 0, 0, 7, 7 );
		cyclesOk = cyclesOk && receiver.deliveries.load() > 0;
		cyclesOk = cyclesOk && ReloadJob( *job, kPT, "registry" );
		const size_t after = Implementation::ForTest_JobCallbackOutputRegistrySize( *job );
		Check( after == 0, label + ": ClearAll + reload drops the old dispatcher (cycle " + std::to_string( cycle )
			+ ", registry " + std::to_string( after ) + ")" );
	}
	Check( cyclesOk, label + ": each cycle attaches, renders through and holds exactly one entry" );

	// (b) Replacing a registered rasterizer drops the entries attached to it.
	Check( SetPixelPel( *job ), label + ": pixelpel rasterizer set" );
	job->AddCallbackRasterizerOutput( &receiver );
	Check( Implementation::ForTest_JobCallbackOutputRegistrySize( *job ) == 1, label + ": one entry on pixelpel" );
	Check( SetPixelPel( *job ), label + ": pixelpel rasterizer replaced" );
	Check( Implementation::ForTest_JobCallbackOutputRegistrySize( *job ) == 0,
		label + ": replacing the rasterizer drops its dispatcher (registry "
		+ std::to_string( Implementation::ForTest_JobCallbackOutputRegistrySize( *job ) ) + ")" );

	// (c) RemoveRasterizerOutputs drops only the ACTIVE rasterizer's entries.
	Check( job->SetActiveRasterizer( "pathtracing_pel_rasterizer" ), label + ": PT active" );
	job->AddCallbackRasterizerOutput( &receiver );			// on PT
	Check( job->SetActiveRasterizer( "pixelpel_rasterizer" ), label + ": pixelpel active" );
	job->AddCallbackRasterizerOutput( &receiver );			// on pixelpel
	Check( Implementation::ForTest_JobCallbackOutputRegistrySize( *job ) == 2, label + ": two entries" );
	job->RemoveRasterizerOutputs();
	Check( Implementation::ForTest_JobCallbackOutputRegistrySize( *job ) == 1,
		label + ": RemoveRasterizerOutputs drops only the active rasterizer's entry (registry "
		+ std::to_string( Implementation::ForTest_JobCallbackOutputRegistrySize( *job ) ) + ")" );
	Check( job->SetActiveRasterizer( "pathtracing_pel_rasterizer" ), label + ": PT active again" );
	receiver.BeginRender();
	bool ok = false;
	try { ok = job->RasterizeRegion( 0, 0, 7, 7 ); } catch( const std::exception& ) {}
	Check( ok && receiver.deliveries.load() > 0 && receiver.staleDeliveries == 0,
		label + ": the surviving PT dispatcher still delivers" );
	Check( !Implementation::ForTest_CallbackOutputsHoldRenderBuffers( *job->GetRasterizer() ),
		label + ": no buffer held after it" );

	safe_release( job );
}

//! Release-on-ENTRY in isolation: a render that bypasses the Job entry
//! points (a direct rasterizer call, as SceneEditController's legacy
//! production render does) and fails leaves the buffer held, because no
//! exit guard covers it; the next Job-entry render must start clean.
static void TestReleaseOnEntry( const char* integrator, const char* chunk, bool wantsFloat )
{
	const std::string label = std::string( "release on entry " ) + integrator + ( wantsFloat ? " RGBA32F" : " RGBA16" );
	IJobPriv* job = LoadJob( chunk, "entry", label );
	if( !job ) return;
	InspectingReceiver receiver;
	receiver.wantsFloat = wantsFloat;
	job->AddCallbackRasterizerOutput( &receiver );
	receiver.BeginRender();
	receiver.throwOnDelivery = 2;
	bool threw = false;
	try { job->GetRasterizer()->RasterizeScene( *job->GetScene(), 0, 0 ); }
	catch( const std::exception& ) { threw = true; }
	Check( threw && Implementation::ForTest_CallbackOutputsHoldRenderBuffers( *job->GetRasterizer() ),
		label + ": a failed direct rasterizer call leaves the buffer held (setup)" );
	receiver.BeginRender();
	receiver.throwOnDelivery = 0;
	bool ok = false;
	try { ok = job->RasterizeRegion( 4, 4, 11, 11 ); } catch( const std::exception& ) {}
	Check( ok && receiver.deliveries.load() > 0, label + ": next Job-entry render delivered" );
	Check( receiver.staleDeliveries == 0, label + ": next Job-entry render starts clean ("
		+ std::to_string( receiver.staleDeliveries ) + " stale deliveries)" );
	safe_release( job );
}

int main()
{
	std::cout << "CallbackOutputRenderScopeTest" << std::endl;
	const char* kPT =
		"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\toidn_denoise FALSE\n}\n";
	const char* kBDPT =
		"bdpt_pel_rasterizer\n{\n\tsamples 1\n\tmax_eye_depth 2\n\tmax_light_depth 2\n\toidn_denoise FALSE\n}\n";
	const Mode modes[] = { Mode::ReceiverThrows, Mode::ProgressCancels, Mode::AnimationCancelled,
		Mode::OtherOutputThrows, Mode::Success };
	TestRegistryLifetime( kPT );
	for( bool f : { true, false } ) {
		TestReleaseOnEntry( "PT", kPT, f );
		TestReleaseOnEntry( "BDPT", kBDPT, f );
	}
	for( Mode m : modes ) {
		for( bool f : { true, false } ) {
			RunCase( "PT", kPT, m, f );
			RunCase( "BDPT", kBDPT, m, f );
		}
	}
	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
