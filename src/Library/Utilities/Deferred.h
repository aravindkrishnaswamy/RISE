//////////////////////////////////////////////////////////////////////
//
//  Deferred.h - A lazily-realized value wrapper.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-06-13
//  Tabs: 4
//  Comments:
//
//    Deferred<T> wraps an expensive-to-produce T* behind a thunk that
//    runs at most ONCE, at a controlled single-threaded "realize"
//    point — NOT lazily on first hot-path access.  RISE's scene is
//    immutable during the parallel rasterize, so any work that would
//    mutate scene state (e.g. tessellating + baking a displaced mesh)
//    must be materialized BEFORE the parallel pixel loop.  The realize
//    pass lives in RayCaster::AttachScene, which already runs
//    single-threaded per render.
//
//    Ownership: Deferred is OWNERSHIP-NEUTRAL.  The thunk PRODUCES the
//    T* (typically `new T(...)` or an addref); the OWNING CONSUMER is
//    responsible for releasing/deleting that T* on invalidate(),
//    rebuild, or its own destruction.  force() and invalidate() never
//    delete the held pointer — doing so would double-free against the
//    consumer's own lifetime management.  See DisplacedGeometry, which
//    keeps the realized mesh in its existing m_pMesh member and uses
//    Deferred purely as the realized-flag + thunk wrapper (so there is
//    a single owner of the mesh, m_pMesh, and Deferred holds a borrowed
//    copy of the same pointer).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DEFERRED_
#define DEFERRED_

#include <atomic>
#include <cassert>
#include <functional>

namespace RISE
{
	//! Render-parallelism depth gauge.
	//!
	//! Bracketed (++/--) around the parallel pixel loop by the rasterizer
	//! (PixelBasedRasterizerHelper::RasterizeScenePass).  In a DEBUG build
	//! Deferred<T>::force() asserts this is zero, catching any attempt to
	//! realize work while the immutable-scene parallel pass is live.  In a
	//! RELEASE build the assert compiles out — the consumer's own
	//! guard-and-fail (e.g. DisplacedGeometry returning a no-hit when its
	//! mesh is null) is the fallback.
	//!
	//! Defined in Deferred.cpp.  It is an int (not a bool) so nested or
	//! re-entrant rasterize passes increment/decrement correctly.
	extern std::atomic<int> g_renderParallelDepth;

	//! RAII bracket for g_renderParallelDepth.  Construct on entry to a
	//! parallel rasterize region; the increment is released on scope exit
	//! even if the region throws or returns early.
	class RenderParallelScope
	{
	public:
		RenderParallelScope()  { g_renderParallelDepth.fetch_add( 1, std::memory_order_relaxed ); }
		~RenderParallelScope() { g_renderParallelDepth.fetch_sub( 1, std::memory_order_relaxed ); }

		RenderParallelScope( const RenderParallelScope& ) = delete;
		RenderParallelScope& operator=( const RenderParallelScope& ) = delete;
	};

	//! A value that is produced on demand, exactly once, at a controlled
	//! single-threaded realize point.
	//!
	//! NOT thread-safe by design: force() is intended to be called from a
	//! single-threaded realize pass (the freeze guard asserts this in
	//! debug).  peek() is the lock-free hot-path accessor and returns the
	//! realized pointer or null if force() has not yet run.
	template <typename T>
	class Deferred
	{
	public:
		Deferred() : m_thunk(), m_value( 0 ), m_realized( false ) {}

		explicit Deferred( std::function<T*()> thunk )
			: m_thunk( std::move( thunk ) ), m_value( 0 ), m_realized( false )
		{
		}

		//! Install (or replace) the producing thunk.  Does NOT realize;
		//! call force() to run it.  Leaves the realized state untouched —
		//! pair with invalidate() if you want the new thunk to run.
		void setThunk( std::function<T*()> thunk )
		{
			m_thunk = std::move( thunk );
		}

		//! Realize the value if not already realized, then return it.
		//! Single-threaded; lock-free.  Idempotent — the thunk runs at
		//! most once between invalidate() calls.  In DEBUG, asserts we are
		//! not inside the parallel rasterize region.
		T* force()
		{
			assert( g_renderParallelDepth.load( std::memory_order_relaxed ) == 0 &&
				"Deferred::force() called during the parallel render — realization must happen single-threaded before the rasterize pass" );
			if( !m_realized ) {
				m_value = m_thunk ? m_thunk() : 0;
				m_realized = true;
			}
			return m_value;
		}

		//! Hot-path raw access.  Returns the realized pointer, or null if
		//! force() has not run since construction / the last invalidate().
		//! Never runs the thunk; safe to call from the parallel pass.
		T* peek() const { return m_value; }

		//! True once force() has run (and not been invalidated since).
		bool isRealized() const { return m_realized; }

		//! Reset to the unrealized state so the next force() re-runs the
		//! thunk.  OWNERSHIP-NEUTRAL: does NOT delete/release the currently
		//! held value — the owning consumer must release the old pointer
		//! BEFORE (or after) calling this, using its own lifetime
		//! management.  Clearing m_value here only drops Deferred's
		//! borrowed copy; it does not free the object.
		void invalidate()
		{
			m_value = 0;
			m_realized = false;
		}

	private:
		std::function<T*()> m_thunk;
		T*                  m_value;
		bool                m_realized;
	};
}

#endif
