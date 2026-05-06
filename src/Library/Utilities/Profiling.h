//////////////////////////////////////////////////////////////////////
//
//  Profiling.h - Lightweight instrumented counters AND wall-clock
//  phase timers for performance analysis of the rendering pipeline.
//
//  Controlled by the RISE_ENABLE_PROFILING preprocessor define.
//  When disabled, all macros expand to nothing with zero overhead.
//
//  Two facilities:
//    - Counters: RISE_PROFILE_INC(counter)     (atomic increment)
//    - Phase timers: RISE_PROFILE_PHASE(name)  (RAII scoped wall-clock)
//
//  Usage:
//    Build with -DRISE_ENABLE_PROFILING to enable instrumentation.
//    Call RISE_PROFILE_REPORT() at the end of rendering to print
//    all accumulated statistics via GlobalLog and stderr.
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
#include <chrono>

namespace RISE
{
	struct ProfilingCounters
	{
		// Ray counts by type
		std::atomic<unsigned long long> nPrimaryRays{0};
		std::atomic<unsigned long long> nShadowRays{0};
		std::atomic<unsigned long long> nMisses{0};

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

		// Shadow cache
		std::atomic<unsigned long long> nShadowCacheHits{0};
		std::atomic<unsigned long long> nShadowCacheMisses{0};

		// Per-render denominators
		std::atomic<unsigned long long> nPixelsResolved{0};
		std::atomic<unsigned long long> nSamplesAccumulated{0};

		// Painter / texture sampling
		std::atomic<unsigned long long> nTexturePainterSamples{0};

		// BSDF / scatter
		std::atomic<unsigned long long> nBSDFScatterCalls{0};

		// Radiance-map (environment) lookups
		std::atomic<unsigned long long> nRadianceMapLookups{0};

		void Reset()
		{
			nPrimaryRays = 0;
			nShadowRays = 0;
			nMisses = 0;
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
			nShadowCacheHits = 0;
			nShadowCacheMisses = 0;
			nPixelsResolved = 0;
			nSamplesAccumulated = 0;
			nTexturePainterSamples = 0;
			nBSDFScatterCalls = 0;
			nRadianceMapLookups = 0;
		}
	};

	// Single global instance
	extern ProfilingCounters g_profilingCounters;

	// Phase identifiers — sites in the render pipeline we want wall-clock
	// breakdowns for.  Add new phases at the end (preserves enum order
	// if anyone caches indices) and add the matching string in Profiling.cpp.
	enum ProfilingPhase
	{
		kPhase_Render = 0,			// Wraps RasterizeScene's main pass
		kPhase_AccelBuild,			// BVH / octree build (one-time)
		kPhase_GeomPrimary,			// IntersectRay (camera + scatter rays)
		kPhase_GeomShadow,			// IntersectShadowRay (NEE)
		kPhase_BSDFScatter,			// ISPF::Scatter / ScatterNM
		kPhase_RadianceMap,			// IRadianceMap::GetRadiance (env lookups)
		kPhase_TexturePainter,		// TexturePainter::GetColor / GetAlpha
		kPhase_Count
	};

	extern std::atomic<unsigned long long> g_phaseNanos[kPhase_Count];

	inline void AddPhaseNanos( ProfilingPhase phase, unsigned long long ns )
	{
		g_phaseNanos[phase].fetch_add( ns, std::memory_order_relaxed );
	}

	// RAII scoped wall-clock timer.  Captures the start time on
	// construction and adds the elapsed nanoseconds to the named phase
	// bucket on destruction.  Uses steady_clock (monotonic, ~25 ns
	// per now() call on Windows) so a single use adds ~50 ns + one
	// relaxed atomic fetch_add.  Don't drop these into ultra-hot tight
	// loops (per-AABB-test); they're tuned for outer leaf calls
	// (one-per-ray, one-per-texture-sample).
	class ScopedPhaseTimer
	{
		std::chrono::steady_clock::time_point start;
		ProfilingPhase phase;
	public:
		explicit ScopedPhaseTimer( ProfilingPhase p )
			: start( std::chrono::steady_clock::now() ), phase( p ) {}

		~ScopedPhaseTimer()
		{
			const auto end = std::chrono::steady_clock::now();
			const auto ns = (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>( end - start ).count();
			AddPhaseNanos( phase, ns );
		}

		ScopedPhaseTimer( const ScopedPhaseTimer& ) = delete;
		ScopedPhaseTimer& operator=( const ScopedPhaseTimer& ) = delete;
	};

	// Prints the profiling report (counters + phase timings) to log + stderr.
	void PrintProfilingReport();
}

// Increment macros — each is a single relaxed atomic increment
#define RISE_PROFILE_INC(counter) \
	(RISE::g_profilingCounters.counter.fetch_add(1, std::memory_order_relaxed))

#define RISE_PROFILE_ADD(counter, n) \
	(RISE::g_profilingCounters.counter.fetch_add((n), std::memory_order_relaxed))

#define RISE_PROFILE_RESET() \
	(RISE::g_profilingCounters.Reset())

#define RISE_PROFILE_REPORT(log) \
	RISE::PrintProfilingReport()

// RAII scoped phase timer.  Use as: RISE_PROFILE_PHASE(GeomPrimary);
// The created object lives for the enclosing block scope and accumulates
// elapsed nanoseconds into g_phaseNanos[kPhase_<name>] on destruction.
#define RISE_PROFILE_PHASE(name) \
	RISE::ScopedPhaseTimer _risePhaseTimer_##name(RISE::kPhase_##name)

#else // RISE_ENABLE_PROFILING not defined

#define RISE_PROFILE_INC(counter)    ((void)0)
#define RISE_PROFILE_ADD(counter, n) ((void)(n))
#define RISE_PROFILE_RESET()         ((void)0)
#define RISE_PROFILE_REPORT(log)     ((void)0)
#define RISE_PROFILE_PHASE(name)     ((void)0)

#endif // RISE_ENABLE_PROFILING
#endif // RISE_PROFILING_
