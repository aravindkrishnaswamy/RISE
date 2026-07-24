//////////////////////////////////////////////////////////////////////
//
//  CancellableProgressCallback.h - IProgressCallback wrapper that
//    ANDs an external atomic cancel flag with an inner caller-supplied
//    progress callback.  The render thread installs this on the
//    rasterizer; the UI thread calls RequestCancel() on every edit
//    to trip the cancel flag.  The dispatcher polls the standard
//    Progress() contract and observes false when cancelled, then
//    sets RasterizeBlockDispatcher::cancelled to abort tile fetch.
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md §4.5.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_CANCELLABLEPROGRESSCALLBACK_
#define RISE_CANCELLABLEPROGRESSCALLBACK_

#include "../Interfaces/IProgressCallback.h"
#include <atomic>

namespace RISE
{
	class CancellableProgressCallback : public IProgressCallback
	{
	public:
		//! @param inner  may be null (in which case only the cancel
		//!               flag governs the return value of Progress).
		explicit CancellableProgressCallback( IProgressCallback* inner );
		virtual ~CancellableProgressCallback();

		//! Replace the inner sink.  The pointer is stored atomically
		//! so concurrent Progress() calls on tile worker threads
		//! observe a consistent value (release/acquire pairing).
		void SetInner( IProgressCallback* inner );

		//! Trip the cancel flag.  Safe to call from any thread.
		//! Subsequent Progress() calls return false until Reset().
		void RequestCancel();

		//! Clear the cancel flag.  Called by the render thread at
		//! the start of each render pass so a stale cancel from
		//! a prior pass doesn't pre-cancel a fresh one.
		void Reset();

		//! True if RequestCancel was called and Reset has not.
		bool IsCancelRequested() const;

		//! True if either the coordinator cancel flag was requested OR the
		//! caller-supplied inner callback refused progress / reported
		//! cancellation since the last Reset().  Completion consumers use
		//! this stronger predicate to avoid publishing a partial frame when
		//! a platform Cancel button lives entirely in the inner callback.
		bool WasCancellationObserved() const;

		// IProgressCallback
		bool Progress( const double progress, const double total ) override;
		void SetTitle( const char* title ) override;
		//! L8 round 15 — override `IsCancelled` so `PixelBasedRasterizerHelper`'s
		//! intra-block cancellation check can query without publishing a
		//! stale progress reading (which bounced the UI progress bar
		//! backward).  Checks the atomic cancel flag AND the inner sink.
		//!
		//! Production-pause fix (2026-07-12): the inner consultation is
		//! LOAD-BEARING, not decorative.  The GUI shells' production
		//! renders route through the controller, which wraps the shell's
		//! progress callback in THIS class — and the shells' pause gate
		//! lives in that inner callback's Progress()/IsCancelled().
		//! Without forwarding, the workers' per-tile + 100 ms intra-block
		//! IsCancelled() polls never reached the gate, so "Pause Render"
		//! only took effect at whole-block Progress() boundaries — at
		//! production sample counts that read as "pause doesn't work"
		//! (field report).  Forwarding parks the workers (and observes a
		//! cancelled-while-paused inner) within one intra-block poll.
		//! Cost when idle: one extra atomic load + a virtual call to a
		//! one-atomic-load default.
		bool IsCancelled() const override
		{
			if( IsCancelRequested() )
			{
				return true;
			}
			IProgressCallback* inner = mInner.load( std::memory_order_acquire );
			if( inner && inner->IsCancelled() )
			{
				mInnerCancelled.store( true, std::memory_order_release );
				return true;
			}
			return false;
		}

	private:
		std::atomic<IProgressCallback*> mInner;
		std::atomic<bool>               mCancelled;
		mutable std::atomic<bool>       mInnerCancelled;
	};
}

#endif
