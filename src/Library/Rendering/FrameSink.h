//////////////////////////////////////////////////////////////////////
//
//  FrameSink.h - IRasterizerOutput adapter that copies the
//  rasterizer's IRasterImage into a FrameStore.
//
//  This is the Phase 1 ingest path (per docs/FRAMESTORE_DESIGN.md
//  §5):  the rasterizer continues to push pixel data via the
//  existing IRasterizerOutput::Output{,Intermediate,PreDenoised,
//  Denoised}Image entry points; FrameSink translates those events
//  into FrameStore tile writes and fires the corresponding
//  IRenderObserver Mark* callbacks (OnTileComplete on each tile,
//  then OnFrameComplete / OnPreDenoiseComplete / OnDenoiseComplete
//  at end-of-frame).
//
//  Once Phase 2 lands (rasterizers write directly into FrameStore),
//  FrameSink will become a notification-only adapter — no copies.
//
//  Construction: FrameSink takes a FrameStore* whose dimensions
//  must already match the rasterizer's image size.  The shim
//  FileRasterizerOutput allocates the FrameStore lazily on first
//  OutputImage call (since the rasterizer's image dims aren't
//  known at FileRasterizerOutput construction time) and constructs
//  FrameSink at the same time.
//
//  Author: design landing L3
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAMESINK_H_
#define FRAMESINK_H_

#include "../Interfaces/IRasterizerOutput.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class FrameStore;

		class FrameSink : public virtual IRasterizerOutput,
		                  public virtual Reference
		{
		public:
			//! Construct with a non-null FrameStore that the sink
			//! will populate.  Does NOT take ownership; caller
			//! retains the FrameStore reference (lifetime via the
			//! existing Reference / addref / release machinery).
			//! The sink takes its own addref on the store internally
			//! so the store survives even if the caller's reference
			//! goes away first.
			explicit FrameSink( FrameStore* store );

			// IRasterizerOutput
			void OutputIntermediateImage(
				const IRasterImage& pImage,
				const Rect*         pRegion ) override;

			void OutputImage(
				const IRasterImage& pImage,
				const Rect*         pRegion,
				const unsigned int  frame ) override;

			void OutputPreDenoisedImage(
				const IRasterImage& pImage,
				const Rect*         pRegion,
				const unsigned int  frame ) override;

			void OutputDenoisedImage(
				const IRasterImage& pImage,
				const Rect*         pRegion,
				const unsigned int  frame ) override;

			void SetCameraExposureCompensationEV( Scalar ev ) override;

		protected:
			virtual ~FrameSink();

		private:
			//! Copy `src` (or the `region` sub-rect of it) into the
			//! FrameStore's beauty + alpha channels.  Uses
			//! FrameStore::CopyTileFromRasterImage for each
			//! affected tile so the per-tile lock is held during
			//! the write.  Pixels outside the FrameStore bounds
			//! are silently dropped.
			void CopyImageIntoStore(
				const IRasterImage& src,
				const Rect*         region );

			FrameStore* store_;  // refcount addref'd in ctor
		};
	}
}

#endif
