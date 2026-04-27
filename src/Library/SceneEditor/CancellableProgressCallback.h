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

		// IProgressCallback
		bool Progress( const double progress, const double total ) override;
		void SetTitle( const char* title ) override;

	private:
		std::atomic<IProgressCallback*> mInner;
		std::atomic<bool>               mCancelled;
	};
}

#endif
