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

#include <iostream>
#include <type_traits>
#include <vector>

#include "../src/Library/Utilities/IORStack.h"
#include "TestStubObject.h"

using namespace RISE;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* name )
{
	if( condition ) {
		++passCount;
	} else {
		++failCount;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

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
	Check( air.top() == 1.0, "air environment IOR" );
	Check( !air.containsCurrent(), "air contains no current object" );
	Check( air.topObject() == 0, "air has no top object" );

	IORStack water( 1.333 );
	Check( water.top() == 1.333, "water environment IOR" );

	std::cout << "TestEnvironmentIOR passed!" << std::endl;
}

static void TestPushPop( const IObject* stub )
{
	std::cout << "Running TestPushPop..." << std::endl;

	IORStack stack( 1.0 );
	stack.SetCurrentObject( stub );

	Check( !stack.containsCurrent(), "current object absent before push" );
	stack.push( 1.5 );
	Check( stack.top() == 1.5, "push updates top IOR" );
	Check( stack.containsCurrent(), "push records current object" );
	Check( stack.topObject() == stub, "push records top object" );

	stack.pop();
	Check( stack.top() == 1.0, "pop restores environment IOR" );
	Check( !stack.containsCurrent(), "pop removes current object" );

	std::cout << "TestPushPop passed!" << std::endl;
}

static void TestCopyPreservesTop( const IObject* stub )
{
	std::cout << "Running TestCopyPreservesTop..." << std::endl;

	IORStack original( 1.0 );
	original.SetCurrentObject( stub );
	original.push( 1.5 );

	IORStack copy( original );
	Check( copy.top() == 1.5, "copy preserves top IOR" );
	Check( copy.containsCurrent(), "copy preserves current object" );

	IORStack assigned( 1.0 );
	assigned = original;
	Check( assigned.top() == 1.5, "assignment preserves top IOR" );

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
	Check( stack.top() == 1.0, "environment entry cannot be popped" );

	std::cout << "TestCannotPopEnvironment passed!" << std::endl;
}

static void AssertEnclosures(
	const IORStack& stack,
	const std::vector<const IObject*>& expected
	)
{
	std::vector<const IObject*> actual;
	stack.AppendObjectStack( actual );
	Check( actual == expected, "enclosure sequence matches" );
	Check( stack.topObject() == ( expected.empty() ? 0 : expected.back() ),
		"top object matches enclosure tail" );
	Check( stack.DebugOpticalStackIsEnclosureSubsequence(),
		"optical stack remains an enclosure subsequence" );
}

static void TestDielectricOnlyBehaviorIsUnchanged(
	const IObject* outer,
	const IObject* inner
	)
{
	std::cout << "Running TestDielectricOnlyBehaviorIsUnchanged..." << std::endl;

	IORStack stack( 1.0 );
	AssertEnclosures( stack, std::vector<const IObject*>() );

	stack.SetCurrentObject( outer );
	Check( !stack.containsCurrent(), "outer absent before optical push" );
	stack.push( 1.5 );
	Check( stack.top() == 1.5, "outer optical push updates IOR" );
	Check( stack.containsCurrent(), "outer present after optical push" );
	AssertEnclosures( stack, std::vector<const IObject*>( 1, outer ) );

	stack.SetCurrentObject( inner );
	Check( !stack.containsCurrent(), "inner absent before optical push" );
	stack.push( 1.33 );
	Check( stack.top() == 1.33, "inner optical push updates IOR" );
	Check( stack.containsCurrent(), "inner present after optical push" );
	std::vector<const IObject*> nested;
	nested.push_back( outer );
	nested.push_back( inner );
	AssertEnclosures( stack, nested );

	stack.pop();
	Check( stack.top() == 1.5, "inner pop restores outer IOR" );
	Check( !stack.containsCurrent(), "inner absent after pop" );
	AssertEnclosures( stack, std::vector<const IObject*>( 1, outer ) );

	stack.SetCurrentObject( outer );
	stack.pop();
	Check( stack.top() == 1.0, "outer pop restores environment IOR" );
	Check( !stack.containsCurrent(), "outer absent after pop" );
	AssertEnclosures( stack, std::vector<const IObject*>() );

	std::cout << "TestDielectricOnlyBehaviorIsUnchanged passed!" << std::endl;
}

static void TestEnclosureOnlyState(
	const IObject* optical,
	const IObject* enclosureOnly
	)
{
	std::cout << "Running TestEnclosureOnlyState..." << std::endl;

	IORStack stack( 1.0 );
	stack.SetCurrentObject( optical );
	stack.push( 1.5 );
	stack.SetCurrentObject( enclosureOnly );
	stack.pushEnclosure();

	Check( stack.top() == 1.5, "enclosure-only push preserves optical IOR" );
	Check( !stack.containsCurrent(), "enclosure-only object is not optical" );
	Check( stack.containsCurrentEnclosure(), "enclosure-only object is tracked" );
	std::vector<const IObject*> nested;
	nested.push_back( optical );
	nested.push_back( enclosureOnly );
	AssertEnclosures( stack, nested );

	IORStack copy( stack );
	Check( copy.top() == 1.5, "enclosure copy preserves optical IOR" );
	Check( !copy.containsCurrent(), "enclosure copy preserves optical absence" );
	Check( copy.containsCurrentEnclosure(), "enclosure copy preserves membership" );
	AssertEnclosures( copy, nested );

	IORStack assigned( 1.0 );
	assigned = stack;
	Check( assigned.top() == 1.5, "enclosure assignment preserves optical IOR" );
	Check( !assigned.containsCurrent(), "enclosure assignment preserves optical absence" );
	Check( assigned.containsCurrentEnclosure(),
		"enclosure assignment preserves membership" );
	AssertEnclosures( assigned, nested );

	stack.popEnclosure();
	Check( stack.top() == 1.5, "enclosure pop preserves optical IOR" );
	Check( !stack.containsCurrentEnclosure(), "enclosure pop removes membership" );
	AssertEnclosures( stack, std::vector<const IObject*>( 1, optical ) );

	stack.SetCurrentObject( optical );
	stack.pop();
	Check( stack.top() == 1.0, "final optical pop restores environment" );
	AssertEnclosures( stack, std::vector<const IObject*>() );

	std::cout << "TestEnclosureOnlyState passed!" << std::endl;
}

int main()
{
	StubObject* stub = new StubObject();
	StubObject* secondStub = new StubObject();
	stub->addref();
	secondStub->addref();

	TestEnvironmentIOR();
	TestPushPop( stub );
	TestCopyPreservesTop( stub );
	TestCannotPopEnvironment( stub );
	TestDielectricOnlyBehaviorIsUnchanged( stub, secondStub );
	TestEnclosureOnlyState( stub, secondStub );

	stub->release();
	secondStub->release();

	std::cout << "\n" << passCount << " passed, " << failCount << " failed." << std::endl;
	return failCount == 0 ? 0 : 1;
}
