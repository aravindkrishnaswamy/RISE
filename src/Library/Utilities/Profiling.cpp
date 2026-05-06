//////////////////////////////////////////////////////////////////////
//
//  Profiling.cpp - Definition of the global profiling counters,
//  phase timing buckets, and the report printing function.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Profiling.h"

#ifdef RISE_ENABLE_PROFILING

#include "../Interfaces/ILog.h"
#include <cstdarg>
#include <cstdio>

namespace RISE
{
	ProfilingCounters g_profilingCounters;
	std::atomic<unsigned long long> g_phaseNanos[kPhase_Count];

	namespace
	{
		const char* kPhaseNames[kPhase_Count] = {
			"Render (total wall)",
			"AccelBuild",
			"GeomPrimary  (IntersectRay)",
			"GeomShadow   (IntersectShadowRay)",
			"BSDFScatter  (ISPF::Scatter*)",
			"RadianceMap  (env lookup)",
			"TexturePainter (GetColor/GetAlpha)"
		};
	}

	void PrintProfilingReport()
	{
		const ProfilingCounters& c = g_profilingCounters;

		char buf[256];

		auto line = [&]( const char* text ) {
			GlobalLog()->PrintEasyInfo( text );
			fprintf( stderr, "%s\n", text );
		};

		auto linef = [&]( const char* fmt, ... ) {
			va_list args;
			va_start( args, fmt );
			vsnprintf( buf, sizeof(buf), fmt, args );
			va_end( args );
			GlobalLog()->PrintEasyInfo( buf );
			fprintf( stderr, "%s\n", buf );
		};

		fprintf( stderr, "\n" );
		line( "" );
		line( "============================================" );
		line( "  RISE Profiling Report" );
		line( "============================================" );

		// --- Phase wall-clock breakdown ---------------------------------
		const unsigned long long renderNs = g_phaseNanos[kPhase_Render].load();
		line( "  Phase wall-clock (sum across all worker threads):" );
		linef( "    %-36s  %12s  %8s  %8s",
			"phase", "ms", "%render", "%CPUbusy" );

		// CPU-busy denominator = sum of all per-leaf phases (not Render
		// itself, which is the wall-clock outer wrapper).  This gives
		// the share of *measured worker CPU time* spent in each phase,
		// useful when threads are well-saturated.
		unsigned long long busyNs = 0;
		for( int i = 1; i < kPhase_Count; ++i ) {
			busyNs += g_phaseNanos[i].load();
		}

		for( int i = 0; i < kPhase_Count; ++i ) {
			const unsigned long long ns = g_phaseNanos[i].load();
			const double ms = ns / 1.0e6;
			const double pctRender = renderNs > 0 ? (100.0 * ns / renderNs) : 0.0;
			const double pctBusy   = (i > 0 && busyNs > 0) ? (100.0 * ns / busyNs) : 0.0;
			if( i == 0 ) {
				linef( "    %-36s  %12.1f  %8s  %8s",
					kPhaseNames[i], ms, "100.0%", "-" );
			} else {
				linef( "    %-36s  %12.1f  %7.2f%%  %7.2f%%",
					kPhaseNames[i], ms, pctRender, pctBusy );
			}
		}

		line(  "  ---" );

		// --- Ray counts -------------------------------------------------
		linef( "  Pixels resolved:             %llu", c.nPixelsResolved.load() );
		linef( "  Samples accumulated:         %llu", c.nSamplesAccumulated.load() );
		linef( "  Primary/scatter rays:        %llu", c.nPrimaryRays.load() );
		linef( "  Shadow rays:                 %llu", c.nShadowRays.load() );
		linef( "  Misses (env hits):           %llu", c.nMisses.load() );
		if( c.nPixelsResolved.load() > 0 ) {
			const double r = (double)c.nPrimaryRays.load() / (double)c.nPixelsResolved.load();
			linef( "  Primary rays / pixel:        %.2f", r );
		}
		if( c.nPrimaryRays.load() > 0 ) {
			const double s = (double)c.nShadowRays.load() / (double)c.nPrimaryRays.load();
			linef( "  Shadow rays / primary ray:   %.2f", s );
		}
		line(  "  ---" );

		// --- Object/triangle/BVH ---------------------------------------
		linef( "  Object intersection tests:   %llu", c.nObjectIntersectionTests.load() );
		linef( "  Object intersection hits:    %llu", c.nObjectIntersectionHits.load() );
		if( c.nObjectIntersectionTests.load() > 0 ) {
			linef( "  Object hit ratio:            %.2f%%",
				100.0 * c.nObjectIntersectionHits.load() / c.nObjectIntersectionTests.load() );
		}
		line(  "  ---" );
		linef( "  Triangle intersection tests: %llu", c.nTriangleIntersectionTests.load() );
		linef( "  Triangle intersection hits:  %llu", c.nTriangleIntersectionHits.load() );
		if( c.nTriangleIntersectionTests.load() > 0 ) {
			linef( "  Triangle hit ratio:          %.2f%%",
				100.0 * c.nTriangleIntersectionHits.load() / c.nTriangleIntersectionTests.load() );
		}
		line(  "  ---" );
		linef( "  Sphere intersection tests:   %llu", c.nSphereIntersectionTests.load() );
		linef( "  Sphere intersection hits:    %llu", c.nSphereIntersectionHits.load() );
		linef( "  Box intersection tests:      %llu", c.nBoxIntersectionTests.load() );
		linef( "  Box intersection hits:       %llu", c.nBoxIntersectionHits.load() );
		line(  "  ---" );
		linef( "  BSP node traversals:         %llu", c.nBSPNodeTraversals.load() );
		linef( "  Octree node traversals:      %llu", c.nOctreeNodeTraversals.load() );
		linef( "  BBox intersection tests:     %llu", c.nBBoxIntersectionTests.load() );
		if( c.nTriangleIntersectionTests.load() > 0 &&
		    (c.nBSPNodeTraversals.load() + c.nOctreeNodeTraversals.load()) > 0 ) {
			linef( "  Avg tri tests per traversal: %.1f",
				(double)c.nTriangleIntersectionTests.load() /
				(c.nBSPNodeTraversals.load() + c.nOctreeNodeTraversals.load()) );
		}
		line(  "  ---" );

		// --- Shading / texture / shadow cache --------------------------
		linef( "  BSDF scatter calls:          %llu", c.nBSDFScatterCalls.load() );
		linef( "  Texture-painter samples:     %llu", c.nTexturePainterSamples.load() );
		linef( "  Radiance-map lookups:        %llu", c.nRadianceMapLookups.load() );
		line(  "  ---" );
		linef( "  Shadow cache hits:           %llu", c.nShadowCacheHits.load() );
		linef( "  Shadow cache misses:         %llu", c.nShadowCacheMisses.load() );
		if( c.nShadowCacheHits.load() + c.nShadowCacheMisses.load() > 0 ) {
			linef( "  Shadow cache hit ratio:      %.2f%%",
				100.0 * c.nShadowCacheHits.load() / (c.nShadowCacheHits.load() + c.nShadowCacheMisses.load()) );
		}
		line( "============================================" );
	}
}

#else
// When RISE_ENABLE_PROFILING is not defined, the entire translation unit
// would compile to zero symbols, producing an empty object file.  Under
// `-flto` that emits empty LLVM bitcode, which `ranlib` then warns about
// (`librise.a(Profiling.o) has no symbols`).  An anonymous-namespace
// constant gets eliminated by LTO before reaching the bitcode anchor;
// only an externally-linked symbol forced through dead-code-elimination
// is preserved by both clang's optimiser and LTO.
//
// Cross-compiler: GCC/Clang use `__attribute__((used))`, MSVC has no
// equivalent (and doesn't need one — its archive format never warns about
// empty TUs from `lib.exe`/link.exe).  Guard accordingly so the file
// builds clean on every supported toolchain.
namespace RISE {
#if defined(__GNUC__) || defined(__clang__)
	__attribute__((used))
#endif
	extern "C" const char kRiseProfilingDisabledAnchor = 0;
}
#endif
