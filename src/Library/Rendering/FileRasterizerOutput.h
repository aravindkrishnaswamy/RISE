//////////////////////////////////////////////////////////////////////
//
//  FileRasterizerOutput.h - A rasterizer output object that writes to a file
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments: Region updates are not supported by the file rasterizer output
//            the entire image is dumped 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FILE_RASTERIZEROUTPUT_
#define FILE_RASTERIZEROUTPUT_

#include "../Interfaces/IRasterizerOutput.h"
#include "../Utilities/Reference.h"
#include "../RasterImages/EXRCompression.h"
#include "DisplayTransform.h"

namespace RISE
{
	namespace Implementation
	{
		static const char extensions[7][6] = { "tga", "ppm", "png", "hdr", "tiff", "rgbea", "exr" };

		// Forward declarations for the L3 lazy-allocated chain.
		class FrameStore;
		class FrameSink;
		class FileEncoderObserver;

		class FileRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
		{
		public:
			enum FRO_TYPE
			{
				TGA		= 0,
				PPM		= 1,
				PNG		= 2,
				HDR		= 3,
				TIFF	= 4,
				RGBEA	= 5,
				EXR		= 6
			};

			//! HDR formats are radiometric: tone-mapping them
			//! corrupts the archival radiance.  Used by
			//! WriteImageToFile to decide whether to wrap the writer
			//! in a DisplayTransformWriter.
			static bool IsHDRFormat( const FRO_TYPE t )
			{
				return t == HDR || t == RGBEA || t == EXR;
			}

		protected:
			char				szPattern[1024];
			bool				bMultiple;
			FRO_TYPE			type;
			unsigned char		bpp;
			COLOR_SPACE			color_space;

			// New (Landing 1) — display pipeline
			Scalar				exposureEV;			///< Exposure offset in EV stops (0 = no scaling)
			DISPLAY_TRANSFORM	display_transform;	///< Tone curve (None for HDR types)

			// Landing 5 — photographic exposure compensation supplied
			// by the scene's camera (via SetCameraExposureCompensationEV
			// at frame start).  Stacks ADDITIVELY with `exposureEV` on
			// LDR outputs; ignored on HDR archival outputs.  Defaults
			// to 0 so non-physical cameras (and any output that the
			// rasterizer never calls SetCameraExposureCompensationEV
			// on) keep pre-L5 behaviour bit-identically.
			Scalar				cameraExposureEV;       ///< HDR-zeroed local value
			//! L8 review round 2 — Raw camera EV (NOT HDR-zeroed).
			//! Used to repopulate `framestore_->Meta()` when binding
			//! to a fresh canonical (in OnRasterizerFrameStoreChanged
			//! and EnsureChain) so HDR FROs don't write zero Meta
			//! and clobber LDR FROs sharing the same canonical
			//! store.  Encoder-side per-format gate
			//! (`IsHDRFormat()` in FrameEncoders.cpp) handles
			//! HDR-vs-LDR EV-application logic at READ time.
			Scalar				rawCameraExposureEV;

			// New (Landing 1) — EXR-specific knobs (ignored for non-EXR types)
			EXR_COMPRESSION		exr_compression;
			bool				exr_with_alpha;

			// L3 — lazy-allocated FrameStore + sink + observer chain.
			// Allocated on first OutputImage / OutputPreDenoisedImage /
			// OutputDenoisedImage call (we need the rasterizer's image
			// dims, which aren't known at construction time).  The
			// trio is reused across subsequent frames in the
			// bMultiple animation case: only the per-frame OnFrameComplete
			// callback advances; the FrameStore buffer is overwritten
			// in place by FrameSink::CopyImageIntoStore.
			//
			// L8 — when the rasterizer pushes a CANONICAL FrameStore via
			// `OnRasterizerFrameStoreChanged` (post-L6e-2b), the
			// internal chain is torn down + the FileEncoderObserver
			// re-registered on the canonical store.  In bound mode
			// `framestore_` aliases the canonical pointer (single
			// addref held by us as a defensive ref); `framesink_`
			// stays null (no copy needed); `encoderObserver_` is
			// registered on the canonical store and fires when the
			// rasterizer drives `MarkFrameComplete` (post-L6f).
			// Output*Image methods become no-ops in bound mode —
			// observers fire from the rasterizer side, not the
			// IRasterizerOutput chain.
			FrameStore*           framestore_       = nullptr;
			FrameSink*            framesink_        = nullptr;
			FileEncoderObserver*  encoderObserver_  = nullptr;

			//! L8 — true when `framestore_` is the canonical
			//! FrameStore pushed via `OnRasterizerFrameStoreChanged`
			//! (vs an internal-allocated one in legacy mode).  Drives
			//! the Output*Image short-circuit and changes teardown
			//! semantics (in bound mode, framestore_ is an alias —
			//! the addref lives on `boundCanonical_`).
			bool                  boundToCanonical_ = false;
			FrameStore*           boundCanonical_   = nullptr;  ///< addref'd in bound mode; null otherwise

			virtual ~FileRasterizerOutput( );

			//! Lazy-allocate the FrameStore + FrameSink +
			//! FileEncoderObserver chain on first Output* call.
			//! After this returns, framestore_/framesink_/observer_
			//! are non-null and the observer is registered on the
			//! store.  Idempotent: subsequent calls with the same
			//! dims are no-ops.  Asserts in debug if dims change
			//! across calls (would require reallocating the chain).
			//!
			//! L8 — only used in LEGACY (no-canonical-bind) mode.
			//! When `boundToCanonical_` is true the chain is managed
			//! by `OnRasterizerFrameStoreChanged` instead.
			void EnsureChain( unsigned int width, unsigned int height );

			//! L8 — Tear down the current observer + (if internal
			//! mode) sink + framestore.  Used by both the dtor and
			//! `OnRasterizerFrameStoreChanged` when transitioning
			//! between bound/internal modes.
			void TeardownChain_();

			//! L8 — Build a FileEncoderObserver bound to `store` and
			//! register it.  Caller has either just allocated `store`
			//! (legacy/internal mode) or just received it from the
			//! rasterizer (bound mode).  Returns false on encoder
			//! lookup failure.
			bool BuildAndAttachObserver_( FrameStore* store );

		public:
			FileRasterizerOutput(
				const char* szPattern_,
				const bool bMultiple_,
				const FRO_TYPE type_,
				const unsigned char bpp_,
				const COLOR_SPACE color_space_,
				const Scalar exposureEV_,
				const DISPLAY_TRANSFORM display_transform_,
				const EXR_COMPRESSION exr_compression_,
				const bool exr_with_alpha_
				);

			void	OutputIntermediateImage( const IRasterImage& pImage, const Rect* pRegion ) override;
			void	OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;
			void	OutputPreDenoisedImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;
			void	OutputDenoisedImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame ) override;

			//! Landing 5: receive the scene camera's photographic exposure
			//! compensation in EV stops.  Called by the rasterizer at frame
			//! start; stacked with the static `exposureEV` parameter at
			//! WriteImageToFile time.  HDR formats clear it to 0 (consistent
			//! with their treatment of `exposureEV` from Landing 1).
			void			SetCameraExposureCompensationEV( Scalar ev ) override;

			//! L8 — `OnRasterizerFrameStoreChanged` notification handler
			//! (post-L6e-2b virtual on `IRasterizerOutput`).  Switches
			//! FileRasterizerOutput into BOUND mode: the
			//! FileEncoderObserver registers on the rasterizer's
			//! canonical FrameStore directly, the legacy FrameSink
			//! intermediary is removed, and `Output*Image` methods
			//! short-circuit to no-ops (the rasterizer drives Mark*
			//! observer events directly post-L6f).
			//!
			//! Pre-L8 the FileRasterizerOutput chain was:
			//!   rasterizer → IRasterizerOutput::OutputImage →
			//!     FrameSink::CopyImageIntoStore (internal FrameStore) →
			//!     MarkFrameComplete (internal store) →
			//!     FileEncoderObserver (registered on internal store) →
			//!     IFrameEncoder → DiskFileWriteBuffer.
			//!
			//! Post-L8 (bound mode):
			//!   rasterizer's canonical mFrameStore →
			//!     MarkFrameComplete (canonical, post-L6f) →
			//!     FileEncoderObserver (registered on canonical) →
			//!     IFrameEncoder → DiskFileWriteBuffer.
			//!
			//! Eliminates the per-frame full-image copy
			//! (CopyImageIntoStore) AND the duplicate FrameStore
			//! allocation (one per FRO became zero).  At 4K the
			//! per-frame copy was ~50 MB of memory bandwidth.
			//!
			//! Idempotent on same-pointer: matching pointer + already-
			//! bound = no-op.  Null pointer = revert to legacy lazy
			//! mode.
			void OnRasterizerFrameStoreChanged( Implementation::FrameStore* framestore ) override;
		};
	}
}

#endif
