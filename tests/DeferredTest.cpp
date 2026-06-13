//////////////////////////////////////////////////////////////////////
//
//  DeferredTest.cpp - Unit tests for the Deferred<T> lazy-realize
//  wrapper and the render-parallelism freeze guard.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: 2026-06-13
//  Tabs: 4
//  Comments:
//
//    Exercises the Phase-1 deferred-realization primitive in isolation:
//      - the thunk runs at most ONCE across many force()/peek() calls;
//      - peek() is null before the first force();
//      - isRealized() transitions false -> true on force();
//      - invalidate() returns to the unrealized state and re-forces;
//      - the RenderParallelScope gauge brackets correctly.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include "../src/Library/Utilities/Deferred.h"

using namespace RISE;

namespace {
	// A trivial heap value whose construction count we can observe.
	struct Widget
	{
		int id;
		explicit Widget( int i ) : id( i ) {}
	};

	int g_buildCount = 0;	// incremented by the thunk each time it runs
}

//-----------------------------------------------------------------------------
// Test 1: the thunk runs exactly once across many force()/peek() calls, and
// peek() is null until the first force().
//-----------------------------------------------------------------------------
static void TestForceRunsThunkOnce()
{
	std::cout << "Test 1: force() runs the thunk exactly once; peek() null before force...\n";

	g_buildCount = 0;
	Widget* owned = 0;	// the test owns the produced Widget (ownership-neutral Deferred)

	Deferred<Widget> d( [&]() -> Widget* {
		++g_buildCount;
		owned = new Widget( 42 );
		return owned;
	} );

	// Before any force(): not realized, peek() is null, thunk hasn't run.
	assert( !d.isRealized() );
	assert( d.peek() == 0 );
	assert( g_buildCount == 0 );

	// First force() runs the thunk and returns the value.
	Widget* w1 = d.force();
	assert( w1 != 0 );
	assert( w1->id == 42 );
	assert( d.isRealized() );
	assert( g_buildCount == 1 );

	// Many more force()/peek() calls must NOT re-run the thunk.
	for( int i = 0; i < 100; ++i ) {
		Widget* wf = d.force();
		Widget* wp = d.peek();
		assert( wf == w1 );
		assert( wp == w1 );
	}
	assert( g_buildCount == 1 );	// still exactly one build

	delete owned;
}

//-----------------------------------------------------------------------------
// Test 2: invalidate() returns to the unrealized state; the next force()
// re-runs the thunk (a second, distinct build).
//-----------------------------------------------------------------------------
static void TestInvalidateReForces()
{
	std::cout << "Test 2: invalidate() re-forces (thunk runs again)...\n";

	g_buildCount = 0;
	Widget* current = 0;

	Deferred<Widget> d( [&]() -> Widget* {
		++g_buildCount;
		current = new Widget( g_buildCount );	// id encodes which build this is
		return current;
	} );

	Widget* w1 = d.force();
	assert( d.isRealized() );
	assert( g_buildCount == 1 );
	assert( w1->id == 1 );

	// Owner releases the old value BEFORE invalidate (Deferred is
	// ownership-neutral and will NOT free it).
	Widget* old = d.peek();
	d.invalidate();
	assert( !d.isRealized() );
	assert( d.peek() == 0 );		// borrowed copy dropped
	assert( g_buildCount == 1 );	// invalidate alone does not re-run
	delete old;

	// Next force() re-runs the thunk -> a second build.  (We do NOT assert
	// the new pointer differs from the old: the allocator may legitimately
	// re-use the just-freed address.  The build COUNT and the id encode that
	// the thunk genuinely re-ran.)
	(void)w1;
	Widget* w2 = d.force();
	assert( d.isRealized() );
	assert( g_buildCount == 2 );
	assert( w2->id == 2 );

	delete current;
}

//-----------------------------------------------------------------------------
// Test 3: a default-constructed Deferred with no thunk forces to null
// (ownership-neutral, no crash), and setThunk() installs one.
//-----------------------------------------------------------------------------
static void TestNullThunkAndSetThunk()
{
	std::cout << "Test 3: empty thunk forces to null; setThunk installs a producer...\n";

	Deferred<Widget> d;	// no thunk
	assert( !d.isRealized() );
	assert( d.peek() == 0 );

	Widget* w0 = d.force();
	assert( w0 == 0 );			// empty thunk -> null
	assert( d.isRealized() );	// but realized (we ran the "build")

	// Re-arm with a real thunk: needs invalidate() to take effect.
	g_buildCount = 0;
	Widget* owned = 0;
	d.setThunk( [&]() -> Widget* { ++g_buildCount; owned = new Widget( 7 ); return owned; } );
	assert( d.peek() == 0 );		// setThunk does not realize
	assert( g_buildCount == 0 );

	d.invalidate();
	Widget* w1 = d.force();
	assert( w1 != 0 && w1->id == 7 );
	assert( g_buildCount == 1 );

	delete owned;
}

//-----------------------------------------------------------------------------
// Test 4: the RenderParallelScope gauge increments/decrements the global
// freeze flag (so the DEBUG assert in force() would fire mid-render).
//-----------------------------------------------------------------------------
static void TestRenderParallelScopeGauge()
{
	std::cout << "Test 4: RenderParallelScope brackets the freeze gauge...\n";

	assert( g_renderParallelDepth.load() == 0 );
	{
		RenderParallelScope s1;
		assert( g_renderParallelDepth.load() == 1 );
		{
			RenderParallelScope s2;	// nested
			assert( g_renderParallelDepth.load() == 2 );
		}
		assert( g_renderParallelDepth.load() == 1 );
	}
	assert( g_renderParallelDepth.load() == 0 );
}

int main()
{
	TestForceRunsThunkOnce();
	TestInvalidateReForces();
	TestNullThunkAndSetThunk();
	TestRenderParallelScopeGauge();

	std::cout << "All Deferred tests passed.\n";
	return 0;
}
