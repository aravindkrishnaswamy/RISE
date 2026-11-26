//////////////////////////////////////////////////////////////////////
//
//  PreviewScheduler.h - Decouple progressive preview cadence from
//    iteration cadence.  Without this, VCM on small scenes resolves
//    the splat film and writes an intermediate PNG every iteration
//    (~25×/sec on a Cornell box), and those barriers (resolve, I/O,
//    CountDone) serialize 20 worker threads waiting for the main
//    thread.
//
//    Preview target is user-configurable (default 7.5 s).  The final
//    pass always forces a preview so the user sees the final image.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 16, 2026
//  Tabs: 4
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_PREVIEW_SCHEDULER_
#define RISE_PREVIEW_SCHEDULER_

#include <chrono>

namespace RISE
{
	namespace Implementation
	{
		class PreviewScheduler
		{
		public:
			explicit PreviewScheduler( double targetIntervalSeconds );

			/// Return true if the caller should perform the preview
			/// pipeline (resolve + OutputIntermediateImage + CountDone)
			/// this turn.  Always returns true the first time it's
			/// called so the user sees something quickly.
			bool ShouldRunPreview();

			/// Reset the timer.  Call once after you actually run the
			/// preview so the next tick waits a full interval.
			void MarkPreviewRan();

		private:
			double                                  targetInterval;
			std::chrono::steady_clock::time_point   lastPreview;
			bool                                    firstCall;
		};
	}
}

#endif
