//////////////////////////////////////////////////////////////////////
//
//  OIDNDenoiser.cpp - Intel OIDN denoiser wrapper implementation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "OIDNDenoiser.h"
#include "AOVBuffers.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Intersection/RayIntersection.h"
#include "../Utilities/RuntimeContext.h"
#include "../Utilities/ThreadPool.h"
#include "../Utilities/RandomNumbers.h"

#include <algorithm>
#include <chrono>
#include <vector>

#ifdef RISE_ENABLE_OIDN
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include <OpenImageDenoise/oidn.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

using namespace RISE;
using namespace RISE::Implementation;

#ifdef RISE_ENABLE_OIDN
struct OIDNDenoiser::State
{
	// OIDN handles.  All are reference-counted smart pointers; default-
	// constructed as null and lazily filled in on first denoise.
	oidn::DeviceRef		device;
	oidn::FilterRef		filter;			// beauty filter
	oidn::FilterRef		albedoFilter;	// aux prefilter (Accurate mode only)
	oidn::FilterRef		normalFilter;	// aux prefilter (Accurate mode only)
	oidn::BufferRef		colorBuf;
	oidn::BufferRef		outputBuf;
	oidn::BufferRef		albedoBuf;
	oidn::BufferRef		normalBuf;
	std::vector<float>	beautyStaging;
	std::vector<float>	denoisedStaging;

	// Cache key.  When any of these change between Denoise calls the
	// filter is torn down and rebuilt; otherwise we just memcpy data
	// into the existing buffers and re-execute the committed network.
	bool				initialized;
	unsigned int		width;
	unsigned int		height;
	bool				hasAlbedo;
	bool				hasNormal;
	OidnQuality			resolvedQuality;	// post-Auto resolution
	OidnPrefilter		prefilter;			// Fast vs Accurate

	State()
	  : initialized( false )
	  , width( 0 )
	  , height( 0 )
	  , hasAlbedo( false )
	  , hasNormal( false )
	  , resolvedQuality( OidnQuality::High )
	  , prefilter( OidnPrefilter::Fast )
	{}
};
#endif

OIDNDenoiser::OIDNDenoiser()
#ifdef RISE_ENABLE_OIDN
  : mState( new State() )
#endif
{
}

OIDNDenoiser::~OIDNDenoiser()
{
#ifdef RISE_ENABLE_OIDN
	delete mState;
#endif
}

void OIDNDenoiser::ImageToFloatBuffer(
	const IRasterImage& img,
	float* buf,
	unsigned int w,
	unsigned int h
	)
{
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			const RISEColor c = img.GetPEL( x, y );
			const unsigned int idx = (y * w + x) * 3;
			buf[idx + 0] = static_cast<float>( c.base.r );
			buf[idx + 1] = static_cast<float>( c.base.g );
			buf[idx + 2] = static_cast<float>( c.base.b );
		}
	}
}

void OIDNDenoiser::FloatBufferToImage(
	const float* buf,
	IRasterImage& img,
	unsigned int w,
	unsigned int h
	)
{
	// Preserve the source pixel's alpha — OIDN runs on RGB only, so the
	// coverage mask painted by the rasterizer (alpha = 0 for "no hit",
	// alpha = 1 for fully covered, fractional at silhouettes) must come
	// through untouched.  Overwriting it with 1.0 turns transparent
	// background pixels into hard opaque black, visible as a sharp
	// rectangle behind objects in scenes with no environment / sky.
	for( unsigned int y = 0; y < h; y++ ) {
		for( unsigned int x = 0; x < w; x++ ) {
			const unsigned int idx = (y * w + x) * 3;
			const Chel alpha = img.GetPEL( x, y ).a;
			img.SetPEL( x, y, RISEColor(
				RISEPel(
					static_cast<Chel>( buf[idx + 0] ),
					static_cast<Chel>( buf[idx + 1] ),
					static_cast<Chel>( buf[idx + 2] ) ),
				alpha ) );
		}
	}
}

#ifdef RISE_ENABLE_OIDN

namespace
{
	// Auto-quality heuristic thresholds (s / megapixel).  See docs/OIDN.md
	// (OIDN-P0-1) for derivation and OIDN-P0-3 for the Metal-backend
	// recalibration note.  Tuned against Apple Silicon CPU; faster
	// devices will leave the heuristic underspending.
	static constexpr double kAutoFastUntilSecPerMP     = 3.0;
	static constexpr double kAutoBalancedUntilSecPerMP = 20.0;

	OidnQuality ResolveAutoQuality(
		double renderSeconds,
		unsigned int w,
		unsigned int h,
		double& outR,			// render seconds per megapixel (for logging)
		double& outMP			// megapixels (for logging)
		)
	{
		outMP = ( static_cast<double>( w ) * static_cast<double>( h ) ) / 1.0e6;
		outR  = ( outMP > 0.0 ) ? renderSeconds / outMP : 0.0;
		if( outR < kAutoFastUntilSecPerMP ) {
			return OidnQuality::Fast;
		}
		if( outR < kAutoBalancedUntilSecPerMP ) {
			return OidnQuality::Balanced;
		}
		return OidnQuality::High;
	}

	const char* OidnQualityName( OidnQuality q )
	{
		switch( q ) {
			case OidnQuality::High:     return "HIGH";
			case OidnQuality::Balanced: return "BALANCED";
			case OidnQuality::Fast:     return "FAST";
			case OidnQuality::Auto:     return "AUTO";
		}
		return "?";
	}

	int OidnQualityAsFilterInt( OidnQuality q )
	{
		// OIDN's quality codes (`OIDN_QUALITY_*` / `oidn::Quality::*`).
		// Cast to plain int so we don't depend on a specific
		// `Filter::set(const char*, oidn::Quality)` overload that older
		// OIDN headers may not expose.
		switch( q ) {
			case OidnQuality::High:     return static_cast<int>( oidn::Quality::High );
			case OidnQuality::Balanced: return static_cast<int>( oidn::Quality::Balanced );
			case OidnQuality::Fast:     return static_cast<int>( oidn::Quality::Fast );
			case OidnQuality::Auto:     return static_cast<int>( oidn::Quality::High );
		}
		return static_cast<int>( oidn::Quality::High );
	}

	// OIDN error callback.  Routes OIDN's diagnostics through RISE's
	// log system instead of stderr.  Registered immediately after the
	// device is created and before its first commit, so any errors
	// during commit / setImage / set / execute funnel through here
	// instead of being silently dropped if the caller forgets to poll
	// `device.getError()`.  The synchronous `getError()` polls in
	// Denoise() are kept too — together they catch warnings (callback)
	// and confirm clean state per call (poll).
	//
	// Cancellation is intentionally NOT propagated to OIDN — see
	// docs/OIDN.md (OIDN-P1-3) for the project invariant.  Even if a
	// progress monitor is wired up later it must always return `true`.
	void OidnErrorCallback( void* /*userPtr*/, oidn::Error code, const char* message )
	{
		if( code == oidn::Error::None ) {
			return;
		}

		const char* codeName = "?";
		bool        isFatal  = true;
		switch( code ) {
			case oidn::Error::None:                break;	// handled above
			case oidn::Error::Unknown:             codeName = "Unknown";             break;
			case oidn::Error::InvalidArgument:     codeName = "InvalidArgument";     break;
			case oidn::Error::InvalidOperation:    codeName = "InvalidOperation";    break;
			case oidn::Error::OutOfMemory:         codeName = "OutOfMemory";         break;
			case oidn::Error::UnsupportedHardware: codeName = "UnsupportedHardware"; break;
			case oidn::Error::Cancelled:           codeName = "Cancelled"; isFatal = false; break;
		}

		GlobalLog()->PrintEx(
			isFatal ? eLog_Error : eLog_Warning,
			"OIDN [%s]: %s",
			codeName,
			message ? message : "(no message)" );
	}

	// Friendly name for the actual selected OIDN device type.  Read
	// from `device.get<int>("type")` after the device is committed,
	// so it reflects what OIDN actually picked when DeviceType::Default
	// was requested (Metal on Apple Silicon, CPU on hardware without
	// a supported GPU backend, etc.).
	const char* OidnDeviceTypeName( int t )
	{
		switch( t ) {
			case static_cast<int>( oidn::DeviceType::CPU ):   return "CPU";
			case static_cast<int>( oidn::DeviceType::SYCL ):  return "SYCL";
			case static_cast<int>( oidn::DeviceType::CUDA ):  return "CUDA";
			case static_cast<int>( oidn::DeviceType::HIP ):   return "HIP";
			case static_cast<int>( oidn::DeviceType::Metal ): return "Metal";
			default:                                          return "?";
		}
	}

	// Try to create + commit a single-type OIDN device.  Returns a
	// committed device on success, an empty DeviceRef on any failure
	// (newDevice returned null OR commit emitted an error).  Errors
	// here are deliberately consumed without going through our user-
	// facing log channel — fall-back from a failed first attempt to
	// CPU is a normal-flow signal, not a user-visible error.  The
	// permanent error callback is registered by the caller AFTER a
	// device is in hand so subsequent errors do route to the log.
	oidn::DeviceRef TryCreateOidnDevice( oidn::DeviceType type )
	{
		oidn::DeviceRef d = oidn::newDevice( type );
		if( !d ) {
			return oidn::DeviceRef();
		}
		d.commit();
		const char* err = 0;
		if( d.getError( err ) != oidn::Error::None ) {
			return oidn::DeviceRef();
		}
		return d;
	}

	// Resolve `OidnDevice` -> a committed `oidn::DeviceRef`, with
	// fall-back behaviour described in OidnConfig.h:
	//   Auto -> Default; on failure CPU.            (silent fallback)
	//   GPU  -> Default; on failure or CPU-only     (explicit warn)
	//           OIDN install, fall back to CPU
	//           with a warning so the request isn't
	//           silently downgraded.
	//   CPU  -> CPU only.
	// Returns an empty DeviceRef if even CPU fails (catastrophic).
	//
	// Subtlety: `oidn::DeviceType::Default` does NOT fail when no GPU
	// backend is loadable — it returns a CPU device.  So a successful
	// Default doesn't mean we got a GPU; we have to introspect the
	// returned device's `type` and warn for OidnDevice::GPU when it
	// turns out to be CPU.  Metal device dylibs ship as a separate
	// `libOpenImageDenoise_device_metal.dylib` next to the main OIDN
	// library; if it's missing from the install path Default silently
	// picks CPU.
	oidn::DeviceRef ResolveOidnDevice( OidnDevice requested )
	{
		if( requested == OidnDevice::CPU ) {
			return TryCreateOidnDevice( oidn::DeviceType::CPU );
		}

		// Auto / GPU: try Default first.  OIDN picks the fastest
		// available backend (Metal on Apple Silicon when the metal
		// device dylib is present).
		oidn::DeviceRef d = TryCreateOidnDevice( oidn::DeviceType::Default );
		if( d ) {
			const int actualType = d.get<int>( "type" );
			const bool isCPU = ( actualType == static_cast<int>( oidn::DeviceType::CPU ) );
			if( isCPU && requested == OidnDevice::GPU ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"OIDN: GPU device requested but unavailable in this OIDN install; using CPU" );
			}
			return d;
		}

		// Default failed entirely (rare — would mean no devices at
		// all).  Try CPU as a last-ditch fallback so we don't drop
		// the denoise pass.
		if( requested == OidnDevice::GPU ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"OIDN: GPU device requested but unavailable; falling back to CPU" );
		}
		return TryCreateOidnDevice( oidn::DeviceType::CPU );
	}
}

void OIDNDenoiser::Denoise(
	float* beautyBuffer,
	const float* albedoBuffer,
	const float* normalBuffer,
	unsigned int w,
	unsigned int h,
	float* outputBuffer,
	OidnQuality requestedQuality,
	OidnDevice requestedDevice,
	OidnPrefilter requestedPrefilter,
	double renderSecondsBeforeDenoise
	)
{
	// Resolve Auto via the render-time / megapixels heuristic; explicit
	// presets pass through unchanged.  When Auto fires, log the inputs
	// and the picked preset so the threshold constants can be tuned
	// from real-world telemetry without re-running the render.
	OidnQuality resolvedQuality = requestedQuality;
	if( requestedQuality == OidnQuality::Auto ) {
		double r = 0.0, mp = 0.0;
		resolvedQuality = ResolveAutoQuality( renderSecondsBeforeDenoise, w, h, r, mp );
		GlobalLog()->PrintEx( eLog_Event,
			"OIDN auto: render=%.2fs, image=%ux%u (%.2f MP), r=%.2f s/MP -> %s",
			renderSecondsBeforeDenoise, w, h, mp, r,
			OidnQualityName( resolvedQuality ) );
	}

	const bool hasAlbedo = ( albedoBuffer != 0 );
	const bool hasNormal = ( normalBuffer != 0 );
	const size_t bufBytes = static_cast<size_t>( w ) * h * 3 * sizeof( float );
	const bool dimsChanged = ( mState->width != w || mState->height != h );

	// Cache key match?  If yes, skip the (expensive) device.commit() and
	// filter.commit() steps and just memcpy + execute.  If no, tear down
	// the filter and rebuild — buffers only get reallocated when the
	// dimensions changed, otherwise they're reused.
	const bool needsRebuild = !mState->initialized
		|| dimsChanged
		|| mState->hasAlbedo != hasAlbedo
		|| mState->hasNormal != hasNormal
		|| mState->resolvedQuality != resolvedQuality
		|| mState->prefilter != requestedPrefilter;

	if( needsRebuild ) {
		// Lazy device creation.  Only happens once per OIDNDenoiser
		// lifetime regardless of how many cache rebuilds follow — the
		// device is dimension-agnostic.  Resolution honours the
		// `requestedDevice` knob with fall-back semantics documented
		// in OidnConfig.h.
		//
		// Buffers go through `buffer.write` / `buffer.read` (rather
		// than the older `memcpy(buf.getData(), …)` pattern) because
		// GPU backends do not guarantee that buffer storage is
		// host-mapped — for those, `getData()` returns NULL and a
		// direct memcpy would segfault.  The C++ wrapper's read/write
		// pick the correct path per device and reduce to memcpy on
		// CPU, so there's no slowdown on the CPU path.
		//
		// We register the error callback AFTER ResolveOidnDevice has
		// returned a working device.  ResolveOidnDevice deliberately
		// swallows first-attempt errors (e.g. "Metal not available")
		// since they are normal-flow signals during fallback, not
		// user-visible errors.  Registering the callback only on the
		// final device avoids logging spurious failure messages.
		// `OIDN_VERBOSE=N` env var still controls OIDN's own
		// diagnostic verbosity (we don't override via API).
		if( !mState->device ) {
			mState->device = ResolveOidnDevice( requestedDevice );
			if( !mState->device ) {
				GlobalLog()->PrintEx( eLog_Error,
					"OIDN: failed to create any device (CPU fallback also failed); skipping denoise" );
				return;
			}
			mState->device.setErrorFunction( OidnErrorCallback, 0 );

			const int actualType = mState->device.get<int>( "type" );
			GlobalLog()->PrintEx( eLog_Event,
				"OIDN: creating %s device (one-time per rasterizer)",
				OidnDeviceTypeName( actualType ) );
		}

		// Reallocate buffers only when dimensions change.  Toggling aux
		// presence reuses existing color/output buffers but allocates
		// or releases the aux buffers.
		if( dimsChanged || !mState->colorBuf ) {
			mState->colorBuf  = mState->device.newBuffer( bufBytes );
			mState->outputBuf = mState->device.newBuffer( bufBytes );
		}
		if( hasAlbedo && ( dimsChanged || !mState->albedoBuf ) ) {
			mState->albedoBuf = mState->device.newBuffer( bufBytes );
		} else if( !hasAlbedo ) {
			mState->albedoBuf = oidn::BufferRef();
		}
		if( hasNormal && ( dimsChanged || !mState->normalBuf ) ) {
			mState->normalBuf = mState->device.newBuffer( bufBytes );
		} else if( !hasNormal ) {
			mState->normalBuf = oidn::BufferRef();
		}

		// Build a fresh beauty filter.  Replacing the FilterRef here
		// releases the previous filter's network state via OIDN's
		// reference counting; setImage / set / commit on the new one
		// binds it to the (possibly reused) buffers.
		mState->filter = mState->device.newFilter( "RT" );
		mState->filter.setImage( "color",  mState->colorBuf,  oidn::Format::Float3, w, h );
		mState->filter.setImage( "output", mState->outputBuf, oidn::Format::Float3, w, h );
		if( hasAlbedo ) {
			mState->filter.setImage( "albedo", mState->albedoBuf, oidn::Format::Float3, w, h );
		}
		if( hasNormal ) {
			mState->filter.setImage( "normal", mState->normalBuf, oidn::Format::Float3, w, h );
		}
		mState->filter.set( "hdr", true );
		mState->filter.set( "quality", OidnQualityAsFilterInt( resolvedQuality ) );

		// `cleanAux=true` tells OIDN the aux buffers are noise-free
		// per pixel, so it skips its internal spatial prefilter on
		// them.  Either path here can satisfy that:
		//   • Fast prefilter — aux comes from the deterministic 4-spp
		//     retrace (or first-hit inline accumulation, which is
		//     identical per-sample in practice).  Already clean.
		//   • Accurate prefilter — aux is noisy from inline path
		//     accumulation, but we run dedicated `RT` filter passes
		//     on each aux channel below, in-place, BEFORE the beauty
		//     filter executes.  After the prefilter passes the aux
		//     IS clean from the beauty filter's perspective.
		if( hasAlbedo || hasNormal ) {
			mState->filter.set( "cleanAux", true );
		}
		mState->filter.commit();

		// Build per-aux prefilter handles for Accurate mode.  Both
		// run in-place on the same buffers the beauty filter reads —
		// when execution time comes around we run them first, then
		// the beauty filter sees already-prefiltered (= clean) aux.
		// Quality and HDR settings differ from the beauty filter:
		// albedo / normal are bounded ([0,1] / [-1,1]) so hdr=false.
		if( requestedPrefilter == OidnPrefilter::Accurate ) {
			if( hasAlbedo ) {
				mState->albedoFilter = mState->device.newFilter( "RT" );
				mState->albedoFilter.setImage( "color",  mState->albedoBuf, oidn::Format::Float3, w, h );
				mState->albedoFilter.setImage( "output", mState->albedoBuf, oidn::Format::Float3, w, h );
				mState->albedoFilter.set( "quality", OidnQualityAsFilterInt( resolvedQuality ) );
				mState->albedoFilter.commit();
			} else {
				mState->albedoFilter = oidn::FilterRef();
			}
			if( hasNormal ) {
				mState->normalFilter = mState->device.newFilter( "RT" );
				mState->normalFilter.setImage( "color",  mState->normalBuf, oidn::Format::Float3, w, h );
				mState->normalFilter.setImage( "output", mState->normalBuf, oidn::Format::Float3, w, h );
				mState->normalFilter.set( "quality", OidnQualityAsFilterInt( resolvedQuality ) );
				mState->normalFilter.commit();
			} else {
				mState->normalFilter = oidn::FilterRef();
			}
		} else {
			// Fast mode: release any prefilter handles from a previous
			// Accurate run so the cache state is consistent on toggle.
			mState->albedoFilter = oidn::FilterRef();
			mState->normalFilter = oidn::FilterRef();
		}

		const char* errorMessage = 0;
		if( mState->device.getError( errorMessage ) != oidn::Error::None ) {
			GlobalLog()->PrintEx( eLog_Error,
				"OIDN denoiser build error: %s",
				errorMessage ? errorMessage : "(no message)" );
			// Leave initialized=false so the next call retries from
			// scratch instead of executing on a possibly-broken filter.
			mState->initialized = false;
			return;
		}

		const char* auxStr = "none";
		if( hasAlbedo && hasNormal ) auxStr = "albedo+normal";
		else if( hasAlbedo )         auxStr = "albedo";
		else if( hasNormal )         auxStr = "normal";

		GlobalLog()->PrintEx( eLog_Event,
			"OIDN cache: rebuild filter (%ux%u q=%s aux=%s prefilter=%s)",
			w, h, OidnQualityName( resolvedQuality ), auxStr,
			( requestedPrefilter == OidnPrefilter::Accurate ) ? "accurate" : "fast" );

		mState->initialized     = true;
		mState->width           = w;
		mState->height          = h;
		mState->hasAlbedo       = hasAlbedo;
		mState->hasNormal       = hasNormal;
		mState->resolvedQuality = resolvedQuality;
		mState->prefilter       = requestedPrefilter;
	} else {
		GlobalLog()->PrintEx( eLog_Event,
			"OIDN cache: hit (%ux%u q=%s prefilter=%s)",
			w, h, OidnQualityName( resolvedQuality ),
			( mState->prefilter == OidnPrefilter::Accurate ) ? "accurate" : "fast" );
	}

	// Copy the per-render data into the cached buffers.  Use
	// `buffer.write` rather than `memcpy(getData(), …)` so the same
	// code path works on both CPU (where getData() is host-mapped)
	// and GPU devices (where it isn't, and a direct memcpy would
	// segfault).  On CPU the C++ wrapper reduces to memcpy under the
	// hood, so there's no slowdown.
	mState->colorBuf.write( 0, bufBytes, beautyBuffer );
	if( hasAlbedo ) {
		mState->albedoBuf.write( 0, bufBytes, albedoBuffer );
	}
	if( hasNormal ) {
		mState->normalBuf.write( 0, bufBytes, normalBuffer );
	}

	// Accurate mode: prefilter the (noisy) aux buffers in-place so
	// the beauty filter sees clean aux.  Both filters write back into
	// the same albedoBuf / normalBuf the beauty filter reads, so
	// there's no extra I/O — just two extra `execute()` calls.  On
	// Metal at 1080p this adds ~80-150 ms per call (smaller networks
	// than the beauty filter).
	if( requestedPrefilter == OidnPrefilter::Accurate ) {
		if( hasAlbedo && mState->albedoFilter ) {
			mState->albedoFilter.execute();
		}
		if( hasNormal && mState->normalFilter ) {
			mState->normalFilter.execute();
		}
	}

	// Execute the cached beauty filter.  No commit() needed — that's
	// already been done either on this call (rebuild branch) or on a
	// previous call that this hit re-uses.
	mState->filter.execute();

	const char* errorMessage = 0;
	if( mState->device.getError( errorMessage ) != oidn::Error::None ) {
		GlobalLog()->PrintEx( eLog_Error,
			"OIDN denoiser execute error: %s",
			errorMessage ? errorMessage : "(no message)" );
		// Don't copy back garbage; subsequent renders will retry with
		// the same cached state.  If the error persists we'd see it
		// here on every render, which is the desired escalation.
		return;
	}

	mState->outputBuf.read( 0, bufBytes, outputBuffer );
}

void OIDNDenoiser::CollectFirstHitAOVs(
	const IScene& scene,
	IRayCaster& caster,
	AOVBuffers& aovBuffers,
	unsigned int samplesPerPixel
	)
{
	const ICamera* pCamera = scene.GetCamera();
	if( !pCamera ) {
		return;
	}

	const IObjectManager* pObjects = scene.GetObjects();
	if( !pObjects ) {
		return;
	}

	const unsigned int width = pCamera->GetWidth();
	const unsigned int height = pCamera->GetHeight();

	// Multi-sample per pixel so the AOVs match the beauty's effective
	// projection.  Each call to GenerateRay() re-samples the aperture
	// (thin-lens cameras), so accumulating N samples gives natural DOF
	// blur in the AOV; subpixel jitter additionally smooths geometry
	// edges.  Without this, OIDN sees a sharp AOV alongside a blurred
	// beauty and "recovers" the sharp signal as if it were noise — DOF
	// gets undone and silhouette edges sharpen unnaturally.  4 samples
	// is the sweet spot: enough aperture coverage to track the beauty's
	// blur, cheap enough that the retrace stays under a second on
	// scenes with thin-lens cameras.
	if( samplesPerPixel == 0 ) {
		samplesPerPixel = 1;
	}
	const Scalar invSamples = Scalar( 1.0 ) / Scalar( samplesPerPixel );

	// Parallelize over rows.  Each row uses a thread-local RNG so the
	// process-wide GlobalRNG isn't contended by N workers calling
	// CanonicalRandom() concurrently.
	GlobalThreadPool().ParallelFor( height, [&]( unsigned int y ) {
		static thread_local RandomNumberGenerator tl_rng;
		RuntimeContext rc( tl_rng, RuntimeContext::PASS_NORMAL, false );

		for( unsigned int x = 0; x < width; x++ )
		{
			for( unsigned int s = 0; s < samplesPerPixel; ++s )
			{
				const Scalar jx = tl_rng.CanonicalRandom();
				const Scalar jy = tl_rng.CanonicalRandom();
				Point2 ptOnScreen( x + jx, ( height - y ) - jy );

				Ray ray;
				// Per OIDN docs, sky / miss pixels should report
				// albedo (1,1,1) and normal (0,0,0).  Accumulating
				// those for missing samples — instead of silently
				// dropping them — correctly blends the AOV across
				// silhouette / DOF-soft edges to match the beauty's
				// blend of surface + background.  Without this OIDN
				// sees a sharp surface AOV behind a blurred beauty and
				// "recovers" the sharp signal, undoing aperture
				// defocus at those edges.
				Vector3 sampleNormal( 0, 0, 0 );
				RISEPel sampleAlbedo( 1, 1, 1 );
				if( pCamera->GenerateRay( rc, ray, ptOnScreen ) ) {
					RasterizerState rast;
					rast.x = x;
					rast.y = y;
					RayIntersection ri( ray, rast );
					pObjects->IntersectRay( ri, true, true, false );
					if( ri.geometric.bHit ) {
						sampleNormal = ri.geometric.vNormal;
						// Delta / transparent surfaces (GetBSDF()==NULL)
						// use white per OIDN documentation.
						sampleAlbedo = ( ri.pMaterial && ri.pMaterial->GetBSDF() )
							? ri.pMaterial->GetBSDF()->albedo( ri.geometric )
							: RISEPel( 1, 1, 1 );
					}
				}
				aovBuffers.AccumulateAlbedo( x, y, sampleAlbedo, 1.0 );
				aovBuffers.AccumulateNormal( x, y, sampleNormal, 1.0 );
			}
			aovBuffers.Normalize( x, y, invSamples );
		}
	} );
}

void OIDNDenoiser::ApplyDenoise(
	IRasterImage& image,
	const AOVBuffers& aovBuffers,
	unsigned int w,
	unsigned int h,
	OidnQuality requestedQuality,
	OidnDevice requestedDevice,
	OidnPrefilter requestedPrefilter,
	double renderSecondsBeforeDenoise
	)
{
	GlobalLog()->PrintEx( eLog_Event, "Running OIDN denoiser (%ux%u)...", w, h );

	const auto t_begin = std::chrono::steady_clock::now();

	const size_t floatCount = static_cast<size_t>( w ) * h * 3;
	mState->beautyStaging.resize( floatCount );
	mState->denoisedStaging.resize( floatCount );
	ImageToFloatBuffer( image, mState->beautyStaging.data(), w, h );
	std::copy(
		mState->beautyStaging.begin(),
		mState->beautyStaging.end(),
		mState->denoisedStaging.begin() );
	Denoise(
		mState->beautyStaging.data(),
		aovBuffers.GetAlbedoPtr(),
		aovBuffers.GetNormalPtr(),
		w, h,
		mState->denoisedStaging.data(),
		requestedQuality,
		requestedDevice,
		requestedPrefilter,
		renderSecondsBeforeDenoise );
	FloatBufferToImage( mState->denoisedStaging.data(), image, w, h );

	const auto t_end = std::chrono::steady_clock::now();
	const double elapsedMs = std::chrono::duration<double, std::milli>(
		t_end - t_begin ).count();
	GlobalLog()->PrintEx( eLog_Event, "OIDN denoising complete. (%.1f ms)", elapsedMs );
}

#endif // RISE_ENABLE_OIDN
