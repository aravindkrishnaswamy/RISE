//////////////////////////////////////////////////////////////////////
//
//  IORStackTest.cpp - Unit tests for IORStack invariants.
//
//  Regression guard for a bug where passing `0` to a function
//  expecting `const IORStack&` silently constructed IORStack(0)
//  via an implicit Scalar -> IORStack conversion, producing an
//  environment IOR of 0 and "Ni=0" refraction errors on spectral
//  dielectric scenes (hwss_prism_dispersion_pt.RISEscene).  The
//  constructor is now `explicit`, and this test codifies that
//  requirement at compile time.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <iostream>
#include <type_traits>
#include <utility>

#include "../src/Library/Utilities/IORStack.h"
#include "TestStubObject.h"

using namespace RISE;

static_assert(
	std::is_nothrow_move_constructible<IORStack>::value,
	"IORStack must be nothrow-move-constructible so std::vector<IORStack> moves without copying"
);
static_assert(
	std::is_nothrow_move_assignable<IORStack>::value,
	"IORStack must be nothrow-move-assignable"
);

// Compile-time guard: the Scalar constructor must be `explicit`, so
// that passing a bare `0` / `1.0` to a `const IORStack&` parameter
// is a compile error rather than silently constructing IORStack(0).
static_assert(
	!std::is_convertible<Scalar, IORStack>::value,
	"IORStack(Scalar) must be explicit to prevent implicit construction "
	"from 0 at call sites that expect const IORStack&"
);
static_assert(
	!std::is_convertible<int, IORStack>::value,
	"IORStack must not be implicitly constructible from integer literal 0"
);
static_assert(
	std::is_constructible<IORStack, Scalar>::value,
	"IORStack must still be directly constructible from a Scalar"
);

static void TestEnvironmentIOR()
{
	std::cout << "Running TestEnvironmentIOR..." << std::endl;

	IORStack air( 1.0 );
	assert( air.top() == 1.0 );
	assert( !air.containsCurrent() );
	assert( air.topObject() == 0 );

	IORStack water( 1.333 );
	assert( water.top() == 1.333 );

	std::cout << "TestEnvironmentIOR passed!" << std::endl;
}

static void TestPushPop( const IObject* stub )
{
	std::cout << "Running TestPushPop..." << std::endl;

	IORStack stack( 1.0 );
	stack.SetCurrentObject( stub );

	assert( !stack.containsCurrent() );
	stack.push( 1.5 );
	assert( stack.top() == 1.5 );
	assert( stack.containsCurrent() );
	assert( stack.topObject() == stub );

	stack.pop();
	assert( stack.top() == 1.0 );
	assert( !stack.containsCurrent() );

	std::cout << "TestPushPop passed!" << std::endl;
}

static void TestCopyPreservesTop( const IObject* stub )
{
	std::cout << "Running TestCopyPreservesTop..." << std::endl;

	IORStack original( 1.0 );
	original.SetCurrentObject( stub );
	original.push( 1.5 );

	IORStack copy( original );
	assert( copy.top() == 1.5 );
	assert( copy.containsCurrent() );

	IORStack assigned( 1.0 );
	assigned = original;
	assert( assigned.top() == 1.5 );

	std::cout << "TestCopyPreservesTop passed!" << std::endl;
}

static void TestCannotPopEnvironment( const IObject* stub )
{
	std::cout << "Running TestCannotPopEnvironment..." << std::endl;

	IORStack stack( 1.0 );
	stack.SetCurrentObject( stub );

	// pop() on a stack with only the environment entry must be a
	// no-op: top() remains the environment IOR.
	stack.pop();
	assert( stack.top() == 1.0 );

	std::cout << "TestCannotPopEnvironment passed!" << std::endl;
}

static void TestMoveInlineAndSpilled( const IObject* stub )
{
	std::cout << "Running TestMoveInlineAndSpilled..." << std::endl;

	// 1. Inline stack (<= 4 entries) move construction & move assignment.
	IORStack inlineSrc( 1.0 );
	inlineSrc.SetCurrentObject( stub );
	inlineSrc.SetCrossingEtaFrom( 1.25 );
	inlineSrc.push( 1.5 );

	IORStack inlineMoved( std::move( inlineSrc ) );
	assert( inlineMoved.Depth() == 2 );
	assert( inlineMoved.top() == 1.5 );
	assert( inlineMoved.containsCurrent() );
	assert( inlineMoved.CrossingEtaFrom() == 1.25 );
	// Moved-from stack retains a valid 1-element environment invariant.
	assert( inlineSrc.Depth() == 1 );
	assert( inlineSrc.top() == 1.0 );
	assert( inlineSrc.CrossingEtaFrom() == 0 );

	// 2. Spilled stack (> 4 entries + ContextList) move construction & assignment.
	StubObject* stubs[6];
	for( int i = 0; i < 6; ++i ) {
		stubs[i] = new StubObject();
	}
	{
		IORStack spilledSrc( 1.001 );
		IORStack::EntryContext ctx;
		ctx.bHasContext = true;
		ctx.bOpenSheet = true;
		for( int i = 0; i < 6; ++i ) {
			spilledSrc.SetCurrentObject( stubs[i] );
			if( i == 2 ) {
				spilledSrc.pushOpenSheet( Scalar( 1.3 + 0.05 * i ), ctx );
			} else {
				spilledSrc.push( Scalar( 1.3 + 0.05 * i ) );
			}
		}
		assert( spilledSrc.Depth() == 7 );

		IORStack spilledMoved( std::move( spilledSrc ) );
		assert( spilledMoved.Depth() == 7 );
		assert( spilledMoved.EnvironmentIOR() == Scalar( 1.001 ) );
		assert( spilledMoved.isOpenSheetAt( 3 ) );
		assert( spilledMoved.ContextAt( 3 ).bHasContext );
		assert( spilledSrc.Depth() == 1 );
		assert( spilledSrc.top() == Scalar( 1.001 ) );

		IORStack target( 1.0 );
		target = std::move( spilledMoved );
		assert( target.Depth() == 7 );
		assert( target.isOpenSheetAt( 3 ) );
		assert( target.ContextAt( 3 ).bHasContext );
		assert( spilledMoved.Depth() == 1 );
		assert( spilledMoved.top() == Scalar( 1.001 ) );
	}
	for( int i = 0; i < 6; ++i ) {
		stubs[i]->release();
	}

	std::cout << "TestMoveInlineAndSpilled passed!" << std::endl;
}

int main()
{
	StubObject* stub = new StubObject();
	stub->addref();

	TestEnvironmentIOR();
	TestPushPop( stub );
	TestCopyPreservesTop( stub );
	TestCannotPopEnvironment( stub );
	TestMoveInlineAndSpilled( stub );

	stub->release();

	std::cout << "\nAll IORStackTest tests passed!" << std::endl;
	return 0;
}
