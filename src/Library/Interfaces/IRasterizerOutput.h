//////////////////////////////////////////////////////////////////////
//
//  IRasterizerOutput.h - Defines an interface to output raster images from
//  rasterizers.  This is where the results of an image from a 
//  IRasterizer get stored
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIZEROUTPUT_
#define IRASTERIZEROUTPUT_

#include "IReference.h"
#include "IRasterImage.h"

namespace RISE
{
	namespace Implementation
	{
		// Forward decl — full type in `Rendering/FrameStore.h`.
		class FrameStore;
	}

	class IRasterizerOutput : public virtual IReference
	{
	protected:
		IRasterizerOutput( ){};
		virtual ~IRasterizerOutput( ){};

	public:
		//! Outputs an intermediate scanline of rasterized data
		virtual void OutputIntermediateImage( 
			const IRasterImage& pImage,					///< [in] Rasterized image
			const Rect* pRegion							///< [in] Rasterized region, if its NULL then the entire image should be output
			) = 0;

		//! A full rasterization was complete, and the full image should be output
		virtual void OutputImage(
			const IRasterImage& pImage,					///< [in] Rasterized image
			const Rect* pRegion,						///< [in] Rasterized region, if its NULL then the entire image should be output
			const unsigned int frame					///< [in] The frame we are outputting
			) = 0;

		//! Pre-denoised (but fully splatted) image, emitted by rasterizers
		//! only when OIDN denoising is enabled.  File-based outputs should
		//! write this using the normal filename; other outputs default to a
		//! no-op so they only observe the denoised final via OutputImage.
		virtual void OutputPreDenoisedImage(
			const IRasterImage& /*pImage*/,
			const Rect* /*pRegion*/,
			const unsigned int /*frame*/
			)
		{
		}

		//! Denoised final image, emitted by rasterizers only when OIDN
		//! denoising is enabled.  File-based outputs should write this with
		//! "_denoised" appended to the filename stem.  The default
		//! implementation forwards to OutputImage so non-file outputs
		//! continue to observe the denoised final (preserving existing
		//! window/store/callback behavior).
		virtual void OutputDenoisedImage(
			const IRasterImage& pImage,
			const Rect* pRegion,
			const unsigned int frame
			)
		{
			OutputImage( pImage, pRegion, frame );
		}

		//! Landing 5: rasterizer-supplied photographic exposure
		//! compensation in EV stops (queried from the scene's camera
		//! at frame start; see ICamera::GetExposureCompensationEV).
		//! LDR file outputs sum this with their own static
		//! `exposure_compensation` parameter to produce the total EV
		//! applied at write time.  HDR archival outputs (EXR / RGBE)
		//! ignore it to preserve "linear radiance ground truth"
		//! semantics from Landing 1.  Default no-op for outputs that
		//! don't need the camera-side EV (window display, in-memory
		//! store, etc. — they show / hold linear radiance).
		virtual void SetCameraExposureCompensationEV( Scalar /*ev*/ ) {}

		//! L6e-2b — Notification fired by `Rasterizer::SetFrameStore`
		//! whenever the rasterizer's canonical FrameStore is
		//! installed or swapped (initial Job push, camera-dim
		//! change, active-camera switch).  Outputs that consume the
		//! FrameStore directly (e.g. `ViewportFrameStore` after
		//! L6e-2a's `BindFrameStore` migration) override this to
		//! re-bind to the new store.  The default no-op preserves
		//! existing behaviour for outputs that don't care about the
		//! rasterizer's canonical FrameStore (file outputs, callback
		//! sinks, legacy IRasterizerOutput consumers).
		//!
		//! `framestore` may be nullptr (Job has cleared the rasterizer's
		//! FrameStore — typically pre-scene-load).  Outputs should
		//! treat nullptr as "fall back to internal-managed mode" if
		//! they have one.
		//!
		//! Threading: called from whatever thread invokes
		//! `Rasterizer::SetFrameStore` — typically the main thread
		//! during Job's `PushJobFrameStoreToRasterizers`.  Outputs
		//! must be thread-safe internally; the rasterizer holds no
		//! lock during this dispatch.
		virtual void OnRasterizerFrameStoreChanged( Implementation::FrameStore* /*framestore*/ ) {}
	};
}

#endif
