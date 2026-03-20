//////////////////////////////////////////////////////////////////////
//
//  Profiling.h - Lightweight instrumented counters for performance
//  analysis of the rendering pipeline.
//
//  Controlled by the RISE_ENABLE_PROFILING preprocessor define.
//  When disabled, all macros expand to nothing with zero overhead.
//
//  Usage:
//    Build with -DRISE_ENABLE_PROFILING to enable counters.
//    Call RISE_PROFILE_REPORT() at the end of rendering to print
//    all accumulated statistics via GlobalLog and stdout.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_PROFILING_
#define RISE_PROFILING_

#ifdef RISE_ENABLE_PROFILING

#include <atomic>

namespace RISE
{
	struct ProfilingCounters
	{
		// Ray counts by type
		std::atomic<unsigned long long> nPrimaryRays{0};
		std::atomic<unsigned long long> nShadowRays{0};

		// Object-level intersection
		std::atomic<unsigned long long> nObjectIntersectionTests{0};
		std::atomic<unsigned long long> nObjectIntersectionHits{0};

		// Triangle intersection
		std::atomic<unsigned long long> nTriangleIntersectionTests{0};
		std::atomic<unsigned long long> nTriangleIntersectionHits{0};

		// Acceleration structure traversal
		std::atomic<unsigned long long> nBSPNodeTraversals{0};
		std::atomic<unsigned long long> nOctreeNodeTraversals{0};
		std::atomic<unsigned long long> nBBoxIntersectionTests{0};

		// Sphere intersection
		std::atomic<unsigned long long> nSphereIntersectionTests{0};
		std::atomic<unsigned long long> nSphereIntersectionHits{0};

		// Box intersection (standalone geometry, not BBox tests)
		std::atomic<unsigned long long> nBoxIntersectionTests{0};
		std::atomic<unsigned long long> nBoxIntersectionHits{0};

		void Reset()
		{
			nPrimaryRays = 0;
			nShadowRays = 0;
			nObjectIntersectionTests = 0;
			nObjectIntersectionHits = 0;
			nTriangleIntersectionTests = 0;
			nTriangleIntersectionHits = 0;
			nBSPNodeTraversals = 0;
			nOctreeNodeTraversals = 0;
			nBBoxIntersectionTests = 0;
			nSphereIntersectionTests = 0;
			nSphereIntersectionHits = 0;
			nBoxIntersectionTests = 0;
			nBoxIntersectionHits = 0;
		}
	};

	// Single global instance
	extern ProfilingCounters g_profilingCounters;

	// Prints the profiling report to the log and stdout
	void PrintProfilingReport();
}

// Increment macros — each is a single relaxed atomic increment
#define RISE_PROFILE_INC(counter) \
	(RISE::g_profilingCounters.counter.fetch_add(1, std::memory_order_relaxed))

#define RISE_PROFILE_RESET() \
	(RISE::g_profilingCounters.Reset())

#define RISE_PROFILE_REPORT(log) \
	RISE::PrintProfilingReport()

#else // RISE_ENABLE_PROFILING not defined

#define RISE_PROFILE_INC(counter)    ((void)0)
#define RISE_PROFILE_RESET()         ((void)0)
#define RISE_PROFILE_REPORT(log)     ((void)0)

#endif // RISE_ENABLE_PROFILING
#endif // RISE_PROFILING_
