//////////////////////////////////////////////////////////////////////
//
//  RenderETAEstimator.h - Estimates elapsed and remaining time for a
//    render from the existing IProgressCallback stream. Pure utility,
//    independent of any renderer; each UI (macOS, Windows, Android)
//    instantiates one and feeds it the same (progress, total) pairs
//    it already receives from IProgressCallback::Progress.
//
//  Algorithm notes:
//    The rasterizer dispatches tiles to a worker pool that atomically
//    fetches the next tile and serializes the Progress() callback
//    through a mutex. Serialization does not preserve tile-index
//    order, so Progress() calls arrive out of order by a few
//    positions. In addition, very small dt between consecutive
//    callbacks (sub-ms, from lock contention) makes any
//    instantaneous-rate estimate spike hard. Previous EMA-based
//    designs were contaminated by both effects and produced wild
//    fluctuations in the displayed ETA.
//
//    The current design avoids instantaneous rate entirely:
//      (1) Track the maximum progress fraction seen. Out-of-order
//          backward samples are ignored; only large backward jumps
//          (> resetThreshold) are treated as real resets (e.g. MLT
//          bootstrap ending and the mutation phase restarting).
//      (2) Compute raw remaining = elapsed * (1 - pMax) / pMax — the
//          classic "at the rate we've averaged so far, how much is
//          left" formula. Stable on noisy callback streams.
//      (3) Smooth the raw estimate with an EMA; between updates the
//          displayed value decreases by the real wall-clock elapsed
//          (countdown). The UI therefore sees a steady timer, not a
//          recomputed estimate that jitters with every callback.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-04-17
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RENDER_ETA_ESTIMATOR_
#define RENDER_ETA_ESTIMATOR_

#include <string>

namespace RISE
{
	class RenderETAEstimator
	{
	public:
		// Tunables. Defaults chosen for tile-granularity progress
		// streams (10-100 Hz) on renders of 1 s to many minutes.
		struct Config
		{
			double warmupSeconds;   // suppress ETA below this elapsed
			double warmupFraction;  // suppress ETA below this progress
			// Backward progress smaller than this is treated as a
			// reordering artifact (concurrent workers race through the
			// rasterizer's serialization lock) and ignored. A backward
			// jump larger than this re-baselines timing — the only
			// callers that produce this are genuine restarts like the
			// MLT bootstrap -> mutation phase transition.
			double resetThreshold;
			// EMA factor used when pulling the smoothed display value
			// toward each new raw estimate. Smaller = smoother, slower
			// to track rate changes.
			double displayAlpha;
			Config();
		};

		RenderETAEstimator();
		explicit RenderETAEstimator( const Config& cfg );
		virtual ~RenderETAEstimator();

		// Clears internal state and records the current time as t0.
		void Begin();

		// Feed the same (progress, total) pair received from
		// IProgressCallback::Progress. Safe to call at any rate and
		// from any thread the caller serializes against RemainingSeconds.
		void Update( const double progress, const double total );

		// Seconds since Begin(). Zero if Begin() was never called.
		double ElapsedSeconds() const;

		// Last observed maximum progress in [0,1].
		double ProgressFraction() const;

		// Writes the current displayed remaining-seconds value into
		// outSeconds and returns true when valid, false during warmup
		// or before any forward progress.
		bool RemainingSeconds( double& outSeconds ) const;

		// Helper shared by all UIs: formats a non-negative duration as
		// "HH:MM:SS" when >= 1 hour (zero-padded hours), otherwise
		// "M:SS". Negative values clamp to zero.
		static std::string FormatDuration( const double seconds );

		// Testing hook — override to drive the estimator from a
		// synthetic clock.
		virtual double NowSeconds() const;

	private:
		Config cfg;
		double t0;              // time of Begin() (resets on large backward jump)
		double pMax;            // maximum progress fraction seen in [0,1]
		double displayedRem;    // EMA-smoothed remaining seconds
		double tLastCorrect;    // NowSeconds() when displayedRem was last updated
		bool   started;         // Begin() has been called
		bool   displayedInit;   // displayedRem has been initialized
	};
}

#endif
