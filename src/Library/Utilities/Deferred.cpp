//////////////////////////////////////////////////////////////////////
//
//  Deferred.cpp - Definition of the render-parallelism depth gauge.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-06-13
//  Tabs: 4
//  Comments:
//
//    Home of g_renderParallelDepth (declared extern in Deferred.h).
//    The rasterizer brackets the parallel pixel loop with a
//    RenderParallelScope; Deferred<T>::force() asserts the gauge is
//    zero in debug builds.  See Deferred.h for the contract.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Deferred.h"

namespace RISE
{
	std::atomic<int> g_renderParallelDepth( 0 );
}
