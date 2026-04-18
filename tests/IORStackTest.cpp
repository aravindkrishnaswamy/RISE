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

#include "../src/Library/Utilities/IORStack.h"
#include "TestStubObject.h"

using namespace RISE;

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

static void TestPushPop()
{
	std::cout << "Running TestPushPop..." << std::endl;

	StubObject stub;
	IORStack stack( 1.0 );
	stack.SetCurrentObject( &stub );

	assert( !stack.containsCurrent() );
	stack.push( 1.5 );
	assert( stack.top() == 1.5 );
	assert( stack.containsCurrent() );
	assert( stack.topObject() == &stub );

	stack.pop();
	assert( stack.top() == 1.0 );
	assert( !stack.containsCurrent() );

	std::cout << "TestPushPop passed!" << std::endl;
}

static void TestCopyPreservesTop()
{
	std::cout << "Running TestCopyPreservesTop..." << std::endl;

	StubObject stub;
	IORStack original( 1.0 );
	original.SetCurrentObject( &stub );
	original.push( 1.5 );

	IORStack copy( original );
	assert( copy.top() == 1.5 );
	assert( copy.containsCurrent() );

	IORStack assigned( 1.0 );
	assigned = original;
	assert( assigned.top() == 1.5 );

	std::cout << "TestCopyPreservesTop passed!" << std::endl;
}

static void TestCannotPopEnvironment()
{
	std::cout << "Running TestCannotPopEnvironment..." << std::endl;

	StubObject stub;
	IORStack stack( 1.0 );
	stack.SetCurrentObject( &stub );

	// pop() on a stack with only the environment entry must be a
	// no-op: top() remains the environment IOR.
	stack.pop();
	assert( stack.top() == 1.0 );

	std::cout << "TestCannotPopEnvironment passed!" << std::endl;
}

int main()
{
	TestEnvironmentIOR();
	TestPushPop();
	TestCopyPreservesTop();
	TestCannotPopEnvironment();
	std::cout << "\nAll IORStackTest tests passed!" << std::endl;
	return 0;
}
