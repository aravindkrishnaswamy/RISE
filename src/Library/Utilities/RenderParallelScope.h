//////////////////////////////////////////////////////////////////////
//
//  RenderParallelScope.h - Render-parallelism freeze gauge.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-06-13
//  Tabs: 4
//  Comments:
//
//    RISE's scene is immutable during the parallel rasterize, so any
//    deferred build work (e.g. tessellating + baking a displaced mesh in
//    DisplacedGeometry::Realize) must be materialized BEFORE the parallel
//    pixel loop -- at the single-threaded realize pass in
//    RayCaster::AttachScene, NOT lazily on the const hot path.
//
//    g_renderParallelDepth is bracketed (++/--) around each parallel
//    render loop by an RAII RenderParallelScope.  The deferred-realization
//    code asserts the gauge is zero in DEBUG builds, catching any attempt
//    to realize work while the immutable-scene parallel pass is live.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RENDER_PARALLEL_SCOPE_
#define RENDER_PARALLEL_SCOPE_

#include <atomic>

namespace RISE
{
	//! Render-parallelism depth gauge.  Bracketed (++/--) around each
	//! parallel render loop by RenderParallelScope.  DisplacedGeometry::
	//! Realize() asserts this is zero in DEBUG, catching any attempt to
	//! realize deferred build work mid-render.  An int (not bool) so nested
	//! or re-entrant rasterize passes increment/decrement correctly.
	//! Defined in RenderParallelScope.cpp.
	extern std::atomic<int> g_renderParallelDepth;

	//! RAII bracket for g_renderParallelDepth.  Construct on entry to a
	//! parallel rasterize region; the decrement is released on scope exit
	//! even if the region throws or returns early.
	class RenderParallelScope
	{
	public:
		RenderParallelScope()  { g_renderParallelDepth.fetch_add( 1, std::memory_order_seq_cst ); }
		~RenderParallelScope() { g_renderParallelDepth.fetch_sub( 1, std::memory_order_seq_cst ); }

		RenderParallelScope( const RenderParallelScope& ) = delete;
		RenderParallelScope& operator=( const RenderParallelScope& ) = delete;
	};
}

#endif
