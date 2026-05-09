//////////////////////////////////////////////////////////////////////
//
//  IRenderObserver.h - Notification interface for FrameStore events.
//
//  Replaces the data-handoff role of IRasterizerOutput.  Observers
//  attach to a FrameStore via FrameStore::AddObserver — NOT to the
//  rasterizer.  This means observers survive a rasterizer swap (e.g.
//  PT → BDPT in the GUI) automatically; only a FrameStore replacement
//  (resolution change, scene reload) requires reattachment, and that
//  is coordinated by the Job that owns the FrameStore.  See
//  docs/FRAMESTORE_DESIGN.md §7.5.
//
//  Callbacks fire from the rasterizer's worker thread (today) or
//  from FrameStore::EndTile / MarkFrameComplete (Phase 2).  They
//  carry NO pixel data — observers consume the FrameStore directly.
//  This means:
//    - the render thread never blocks waiting for an observer to
//      finish (file write, network send, UI repaint queue);
//    - the seqlock model lets observers and the rasterizer run
//      concurrently with no mutex.
//  Each platform's GUI bridge marshals from the render thread to the
//  UI thread via Qt signals / dispatch_async / Compose
//  LaunchedEffect; observers don't need to do that internally.
//
//  Author: design landing L1
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef IRENDEROBSERVER_
#define IRENDEROBSERVER_

#include <cstdint>
#include "IRasterImage.h"  // for Rect

namespace RISE
{
	class IRenderObserver
	{
	public:
		virtual ~IRenderObserver() {}

		//! Called from the rasterizer worker thread after a tile
		//! is committed to its FrameStore.  The Rect is in pixel
		//! coordinates (top, left, bottom, right).  `generation`
		//! is the FrameStore's global generation counter at the
		//! moment the tile was committed; observers can use it to
		//! gate "frame dirty" repaints.
		virtual void OnTileComplete( const Rect& /*roi*/, uint64_t /*generation*/ ) {}

		//! Called after the final tile of a frame.  Observers that
		//! perform once-per-frame work (file encode, network mirror)
		//! listen here.
		virtual void OnFrameComplete( unsigned /*frame*/, uint64_t /*generation*/ ) {}

		//! Called when OIDN denoising is enabled, before the
		//! denoiser pass runs (FrameStore holds the un-denoised
		//! beauty buffer at this point).  File-based observers may
		//! choose to write a "_predenoise" variant here.
		virtual void OnPreDenoiseComplete( unsigned /*frame*/, uint64_t /*generation*/ ) {}

		//! Called after the OIDN denoiser pass overwrites the
		//! beauty buffer with the denoised result.
		virtual void OnDenoiseComplete( unsigned /*frame*/, uint64_t /*generation*/ ) {}

		//! Called at frame start with the scene camera's
		//! photographic exposure compensation in EV stops.
		//! Observers that bake this into their own ViewTransform
		//! (instead of consuming FrameStore.Meta().cameraExposureEV
		//! directly) listen here.
		virtual void OnCameraExposureCompensation( double /*ev*/ ) {}
	};
}

#endif
