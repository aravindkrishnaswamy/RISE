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

void OIDNDenoiser::Denoise(
	float* beautyBuffer,
	const float* albedoBuffer,
	const float* normalBuffer,
	unsigned int w,
	unsigned int h,
	float* outputBuffer
	)
{
	// Use CPU device explicitly to ensure host pointers are accessible.
	// The default device may select a GPU backend (SYCL/HIP) on some
	// platforms, which requires device-allocated buffers.  Falling back
	// to CPU keeps the code portable and avoids the
	// "image data not accessible by the device" error.
	oidn::DeviceRef device = oidn::newDevice( oidn::DeviceType::CPU );
	device.commit();

	const size_t pixelCount = static_cast<size_t>( w ) * h;
	const size_t bufBytes   = pixelCount * 3 * sizeof( float );

	// Wrap host memory in OIDNBuffers so all device types can access it
	oidn::BufferRef colorBuf  = device.newBuffer( bufBytes );
	oidn::BufferRef outputBuf = device.newBuffer( bufBytes );

	std::memcpy( colorBuf.getData(), beautyBuffer, bufBytes );

	oidn::FilterRef filter = device.newFilter( "RT" );
	filter.setImage( "color",  colorBuf,  oidn::Format::Float3, w, h );
	filter.setImage( "output", outputBuf, oidn::Format::Float3, w, h );

	oidn::BufferRef albedoBuf, normalBuf;
	if( albedoBuffer ) {
		albedoBuf = device.newBuffer( bufBytes );
		std::memcpy( albedoBuf.getData(), albedoBuffer, bufBytes );
		filter.setImage( "albedo", albedoBuf, oidn::Format::Float3, w, h );
	}
	if( normalBuffer ) {
		normalBuf = device.newBuffer( bufBytes );
		std::memcpy( normalBuf.getData(), normalBuffer, bufBytes );
		filter.setImage( "normal", normalBuf, oidn::Format::Float3, w, h );
	}

	filter.set( "hdr", true );
	if( albedoBuffer || normalBuffer ) {
		// IBSDF::albedo() returns a real directional-hemispherical
		// reflectance estimate (per-pixel constant, no MC noise), and
		// CollectFirstHitAOVs multi-samples for proper aperture / DOF
		// coverage — so the aux buffers genuinely are clean and OIDN
		// should use them directly without spatial pre-filtering.
		filter.set( "cleanAux", true );
	}
	filter.commit();
	filter.execute();

	const char* errorMessage;
	if( device.getError( errorMessage ) != oidn::Error::None ) {
		GlobalLog()->PrintEx( eLog_Error, "OIDN denoiser error: %s", errorMessage );
	} else {
		// Copy denoised result back to caller's buffer
		std::memcpy( outputBuffer, outputBuf.getData(), bufBytes );
	}
}

void OIDNDenoiser::CollectFirstHitAOVs(
	const IScene& scene,
	IRayCaster& caster,
	AOVBuffers& aovBuffers
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
	const unsigned int aovSamplesPerPixel = 4;
	const Scalar invSamples = Scalar( 1.0 ) / Scalar( aovSamplesPerPixel );

	// Parallelize over rows.  Each row uses a thread-local RNG so the
	// process-wide GlobalRNG isn't contended by N workers calling
	// CanonicalRandom() concurrently.
	GlobalThreadPool().ParallelFor( height, [&]( unsigned int y ) {
		static thread_local RandomNumberGenerator tl_rng;
		RuntimeContext rc( tl_rng, RuntimeContext::PASS_NORMAL, false );

		for( unsigned int x = 0; x < width; x++ )
		{
			for( unsigned int s = 0; s < aovSamplesPerPixel; ++s )
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
	unsigned int h
	)
{
	GlobalLog()->PrintEx( eLog_Event, "Running OIDN denoiser (%ux%u)...", w, h );

	std::vector<float> beautyBuf( w * h * 3 );
	std::vector<float> denoisedBuf( w * h * 3 );
	ImageToFloatBuffer( image, beautyBuf.data(), w, h );
	Denoise(
		beautyBuf.data(),
		aovBuffers.GetAlbedoPtr(),
		aovBuffers.GetNormalPtr(),
		w, h,
		denoisedBuf.data() );
	FloatBufferToImage( denoisedBuf.data(), image, w, h );

	GlobalLog()->PrintEx( eLog_Event, "OIDN denoising complete." );
}

#endif // RISE_ENABLE_OIDN
