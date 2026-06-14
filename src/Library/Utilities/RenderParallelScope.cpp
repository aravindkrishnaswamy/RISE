//////////////////////////////////////////////////////////////////////
//
//  RenderParallelScope.cpp - Definition of the render-parallelism depth
//  gauge.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-06-13
//  Tabs: 4
//  Comments:
//
//    Home of g_renderParallelDepth (declared extern in
//    RenderParallelScope.h).  See the header for the freeze-guard contract.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RenderParallelScope.h"

namespace RISE
{
	std::atomic<int> g_renderParallelDepth( 0 );
}
