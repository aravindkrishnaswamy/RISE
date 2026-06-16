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

	//! Re-maps a per-frame [0,1] progress fraction into one frame's
	//! slot of a whole-animation [0,1] bar.  An animation driver
	//! rendering frame `frameIdx` of `totalFrames` wraps the real
	//! progress sink with this adapter so a per-frame render (which
	//! natively reports 0..1 for ITS frame) advances the OVERALL bar
	//! across [frameIdx/totalFrames, (frameIdx+1)/totalFrames] instead
	//! of restarting 0..100% every frame.
	//!
	//! Used by the MLT rasterizers, whose phase-based (bootstrap /
	//! chains / rounds) per-frame progress can't use
	//! PixelBasedRasterizerHelper's tile-weighted movie-wide
	//! accounting.  PT/BDPT/VCM report movie-wide progress directly via
	//! mProgressBase/mProgressTotal and do NOT need this wrapper.
	//!
	//! Passing (frameIdx=0, totalFrames=1) is the identity map, so the
	//! single-specific-frame render path reports a plain 0..100%.
	//! `SetTitle` / `IsCancelled` forward to the inner sink unchanged.
	class FrameSlotProgressCallback : public IProgressCallback
	{
	public:
		FrameSlotProgressCallback( IProgressCallback* inner, const unsigned int frameIdx, const unsigned int totalFrames )
		  : mInner( inner )
		  , mFrameIdx( frameIdx )
		  , mTotalFrames( totalFrames > 0 ? totalFrames : 1 )
		{
		}

		bool Progress( const double progress, const double total ) override
		{
			double f = ( total > 0.0 ) ? ( progress / total ) : 0.0;
			if( f < 0.0 ) f = 0.0;
			if( f > 1.0 ) f = 1.0;
			const double whole = ( static_cast<double>( mFrameIdx ) + f )
			                   / static_cast<double>( mTotalFrames );
			return mInner ? mInner->Progress( whole, 1.0 ) : true;
		}

		void SetTitle( const char* title ) override
		{
			if( mInner ) { mInner->SetTitle( title ); }
		}

		bool IsCancelled() const override
		{
			return mInner ? mInner->IsCancelled() : false;
		}

	private:
		IProgressCallback*	mInner;
		unsigned int		mFrameIdx;
		unsigned int		mTotalFrames;
	};
}

#endif

