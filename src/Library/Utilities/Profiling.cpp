//////////////////////////////////////////////////////////////////////
//
//  Profiling.cpp - Definition of the global profiling counters
//  and the report printing function.
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
		linef( "  Primary rays:                %llu", c.nPrimaryRays.load() );
		linef( "  Shadow rays:                 %llu", c.nShadowRays.load() );
		line(  "  ---" );
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
		line( "============================================" );
	}
}

#endif
