//////////////////////////////////////////////////////////////////////
//
//  IProgressCallback.h - Defines the interface that all classes
//    who want to be notified about progress must implement
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 25, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_PROGRESS_CALLBACK_
#define I_PROGRESS_CALLBACK_

namespace RISE
{
	class IProgressCallback
	{
	public:
		virtual ~IProgressCallback(){}

		// Return TRUE to continue, return FALSE to abort whatever operation
		// we are getting progress for
		virtual bool Progress( const double progress, const double total ) = 0;
		virtual void SetTitle( const char* title ) = 0;

		//! L8 round 15 — Query-only cancellation check.  Returns
		//! `true` iff the caller wants the in-flight operation to
		//! abort, WITHOUT publishing a fresh progress reading.
		//!
		//! Why this exists: `Progress(num, denom)` doubles as both a
		//! progress publisher AND a cancellation signal (via its
		//! return value).  Callers that want to query cancellation
		//! mid-block — e.g. `PixelBasedRasterizerHelper`'s 100 ms
		//! intra-block flush — can't easily pass a "current" progress
		//! reading because they don't know how far the dispatcher has
		//! already published.  Calling `Progress(mProgressBase, ...)`
		//! reports BACKWARD vs the dispatcher's per-block
		//! `Progress(mProgressBase + idx*weight, ...)`, producing a
		//! progress bar that bounces backward in the UI.
		//!
		//! `IsCancelled` lets the rasterizer query the cancel state
		//! directly without touching the progress reading.  Default
		//! `false` so non-cancellable IProgressCallback impls
		//! (printers, log forwarders) don't need to know.
		//! `CancellableProgressCallback` overrides to return the
		//! actual flag.
		virtual bool IsCancelled() const { return false; }
	};
}

#endif

