//////////////////////////////////////////////////////////////////////
//
//  OIDNDenoiser.h - Wrapper around Intel Open Image Denoise for
//  post-process denoising. Filter methods require RISE_ENABLE_OIDN;
//  buffer conversion helpers are available without OIDN.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OIDN_DENOISER_H_
#define OIDN_DENOISER_H_

#include "../Interfaces/IRasterImage.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/OidnConfig.h"

namespace RISE
{
	class IScene;
	class IRayCaster;

	namespace Implementation
	{
		class AOVBuffers;

		/// Stateful OIDN denoise context. Consecutive calls with unchanged
		/// configuration and shared-buffer pointers reuse committed handles.
		/// The device also survives filter-only rebuilds while its requested
		/// backend is unchanged. Held by Rasterizer for its lifetime.
		///
		/// Stateless helpers (ImageToFloatBuffer, FloatBufferToImage,
		/// CollectFirstHitAOVs) remain static — they don't touch any
		/// OIDN device state and are safe to call without an instance.
		class OIDNDenoiser
		{
		public:
			OIDNDenoiser();
			~OIDNDenoiser();

			OIDNDenoiser( const OIDNDenoiser& ) = delete;
			OIDNDenoiser& operator=( const OIDNDenoiser& ) = delete;

			/// Converts an IRasterImage (double-precision RISEColor pixels)
			/// to an interleaved float RGB buffer for OIDN consumption.
			static void ImageToFloatBuffer(
				const IRasterImage& img,
				float* buf,
				unsigned int w,
				unsigned int h
				);

			/// Converts an interleaved float RGB buffer back into an
			/// IRasterImage, promoting float to double-precision channels.
			static void FloatBufferToImage(
				const float* buf,
				IRasterImage& img,
				unsigned int w,
				unsigned int h
				);

#ifdef RISE_ENABLE_OIDN
			/// Read while idle: cached preset (High before the first successful setup).
			OidnQuality GetLastResolvedQuality() const;
			/// Read while idle: Auto before device creation, otherwise actual CPU/GPU.
			OidnDevice GetLastResolvedDevice() const;
			/// Successful backend creations; unchanged requests reuse the device.
			unsigned int GetDeviceGeneration() const;

			/// Runs the OIDN RT filter on the given buffers.
			/// beautyBuffer is the noisy input (w*h*3 floats, HDR).
			/// albedoBuffer and normalBuffer are optional (may be NULL).
			/// outputBuffer receives the denoised result (may alias beautyBuffer).
			/// requestedQuality selects the OIDN quality preset; Auto picks
			/// from a deterministic work estimate (DL-360; see docs/OIDN.md).
			/// requestedDevice picks the OIDN backend (Auto / CPU / GPU);
			/// see docs/OIDN.md OIDN-P0-3 for fallback semantics.
			/// requestedPrefilter selects between Fast (cleanAux=true on
			/// the beauty filter, no prefilter pass) and Accurate (run
			/// dedicated prefilter passes on each aux buffer first, then
			/// the beauty filter).  See docs/OIDN.md OIDN-P1-1.
			/// workPerMegapixel = configured/adaptive spp * family policy weight.
			/// It is a scene-static policy estimate, never measured wall-clock.
			void Denoise(
				float* beautyBuffer,
				const float* albedoBuffer,
				const float* normalBuffer,
				unsigned int w,
				unsigned int h,
				float* outputBuffer,
				OidnQuality requestedQuality,
				OidnDevice requestedDevice,
				OidnPrefilter requestedPrefilter,
				double workPerMegapixel
				);

			/// Collects first-hit albedo and normal AOVs by casting one
			/// primary ray per pixel through the scene.  Stateless —
			/// kept static because nothing about AOV collection benefits
			/// from device caching.
			static void CollectFirstHitAOVs(
				const IScene& scene,
				IRayCaster& caster,
				AOVBuffers& aovBuffers,
				unsigned int samplesPerPixel = 4
				);

			/// Runs the full denoise pipeline on an image using the
			/// given AOV buffers.  Allocates temporary float buffers,
			/// converts, denoises, and writes back.  See Denoise() for
			/// requestedQuality / requestedDevice / requestedPrefilter
			/// semantics. workPerMegapixel is independent of image/region area,
			/// elapsed time or early stopping.
			void ApplyDenoise(
				IRasterImage& image,
				const AOVBuffers& aovBuffers,
				unsigned int w,
				unsigned int h,
				OidnQuality requestedQuality,
				OidnDevice requestedDevice,
				OidnPrefilter requestedPrefilter,
				double workPerMegapixel
				);

			/// Region-restricted counterpart used by RasterizeRegion. The OIDN
			/// working image and guide planes are cropped to the inclusive bounds,
			/// and only those pixels are written back to `image`.
			void ApplyDenoiseRegion(
				IRasterImage& image,
				const AOVBuffers& aovBuffers,
				unsigned int fullWidth,
				unsigned int fullHeight,
				unsigned int left,
				unsigned int top,
				unsigned int right,
				unsigned int bottom,
				OidnQuality requestedQuality,
				OidnDevice requestedDevice,
				OidnPrefilter requestedPrefilter,
				double workPerMegapixel
				);
#endif

		private:
#ifdef RISE_ENABLE_OIDN
			// Opaque pImpl: holds oidn::DeviceRef, oidn::FilterRef,
			// oidn::BufferRef handles plus the cache key.  Defined in
			// OIDNDenoiser.cpp so this header doesn't drag oidn.hpp
			// into every transitively-including TU.
			struct State;
			State* mState;
#endif
		};
	}
}

#endif
