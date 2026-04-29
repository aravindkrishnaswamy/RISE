//////////////////////////////////////////////////////////////////////
//
//  OIDNDenoiser.h - Wrapper around Intel Open Image Denoise for
//  post-process denoising of rendered images.  Entire file is
//  compiled only when RISE_ENABLE_OIDN is defined.
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

		/// Stateful OIDN denoise context.  Caches the OIDN device,
		/// filter, and per-buffer handles across calls; cross-render
		/// reuse on the same rasterizer pays the device.commit() and
		/// filter.commit() cost only once per cache key (resolution ×
		/// quality × aux presence).  Held by the Rasterizer base for
		/// the rasterizer's lifetime.
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
			/// Runs the OIDN RT filter on the given buffers.
			/// beautyBuffer is the noisy input (w*h*3 floats, HDR).
			/// albedoBuffer and normalBuffer are optional (may be NULL).
			/// outputBuffer receives the denoised result (may alias beautyBuffer).
			/// requestedQuality selects the OIDN quality preset; Auto picks
			/// from the render-time heuristic (see docs/OIDN.md OIDN-P0-1).
			/// renderSecondsBeforeDenoise drives the Auto heuristic.
			void Denoise(
				float* beautyBuffer,
				const float* albedoBuffer,
				const float* normalBuffer,
				unsigned int w,
				unsigned int h,
				float* outputBuffer,
				OidnQuality requestedQuality,
				double renderSecondsBeforeDenoise
				);

			/// Collects first-hit albedo and normal AOVs by casting one
			/// primary ray per pixel through the scene.  Stateless —
			/// kept static because nothing about AOV collection benefits
			/// from device caching.
			static void CollectFirstHitAOVs(
				const IScene& scene,
				IRayCaster& caster,
				AOVBuffers& aovBuffers
				);

			/// Runs the full denoise pipeline on an image using the
			/// given AOV buffers.  Allocates temporary float buffers,
			/// converts, denoises, and writes back.  requestedQuality
			/// selects the OIDN quality preset; Auto picks from the
			/// render-time heuristic.  renderSecondsBeforeDenoise is
			/// wall-clock from rasterizer start to immediately before
			/// the denoise filter runs.
			void ApplyDenoise(
				IRasterImage& image,
				const AOVBuffers& aovBuffers,
				unsigned int w,
				unsigned int h,
				OidnQuality requestedQuality,
				double renderSecondsBeforeDenoise
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
