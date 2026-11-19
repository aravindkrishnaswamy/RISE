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
#include <utility>

#include "../src/Library/Utilities/IORStack.h"
#include "TestStubObject.h"

using namespace RISE;

namespace
{
	int gPassCount = 0;
	int gFailCount = 0;

	void Check( const bool cond, const char* label )
	{
		if( cond ) {
			++gPassCount;
		} else {
			++gFailCount;
			std::cerr << "FAIL: " << label << std::endl;
		}
	}
}

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
	Check( air.top() == 1.0, "air.top() == 1.0" );
	Check( !air.containsCurrent(), "!air.containsCurrent()" );
	Check( air.topObject() == 0, "air.topObject() == 0" );

	IORStack water( 1.333 );
	Check( water.top() == 1.333, "water.top() == 1.333" );

	std::cout << "TestEnvironmentIOR passed!" << std::endl;
}

static void TestPushPop( const IObject* stub )
{
	std::cout << "Running TestPushPop..." << std::endl;

	IORStack stack( 1.0 );
	stack.SetCurrentObject( stub );

	Check( !stack.containsCurrent(), "!stack.containsCurrent() before push" );
	stack.push( 1.5 );
	Check( stack.top() == 1.5, "stack.top() == 1.5 after push" );
	Check( stack.containsCurrent(), "stack.containsCurrent() after push" );
	Check( stack.topObject() == stub, "stack.topObject() == stub" );

	stack.pop();
	Check( stack.top() == 1.0, "stack.top() == 1.0 after pop" );
	Check( !stack.containsCurrent(), "!stack.containsCurrent() after pop" );

	std::cout << "TestPushPop passed!" << std::endl;
}

static void TestCopyPreservesTop( const IObject* stub )
{
	std::cout << "Running TestCopyPreservesTop..." << std::endl;

	IORStack original( 1.0 );
	original.SetCurrentObject( stub );
	original.push( 1.5 );

	IORStack copy( original );
	Check( copy.top() == 1.5, "copy.top() == 1.5" );
	Check( copy.containsCurrent(), "copy.containsCurrent()" );

	IORStack assigned( 1.0 );
	assigned = original;
	Check( assigned.top() == 1.5, "assigned.top() == 1.5" );

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
	Check( stack.top() == 1.0, "cannot pop environment IOR" );

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

static void TestMoveInlineAndSpilled( const IObject* stub )
{
	std::cout << "Running TestMoveInlineAndSpilled..." << std::endl;

	// 1. Inline stack (<= 4 entries) move construction & move assignment.
	IORStack inlineSrc( 1.0 );
	inlineSrc.SetCurrentObject( stub );
	inlineSrc.SetCrossingEtaFrom( 1.25 );
	inlineSrc.push( 1.5 );

	IORStack inlineMoved( std::move( inlineSrc ) );
	Check( inlineMoved.Depth() == 2, "inlineMoved.Depth() == 2" );
	Check( inlineMoved.top() == 1.5, "inlineMoved.top() == 1.5" );
	Check( inlineMoved.containsCurrent(), "inlineMoved.containsCurrent()" );
	Check( inlineMoved.CrossingEtaFrom() == 1.25, "inlineMoved.CrossingEtaFrom() == 1.25" );
	// Moved-from stack retains a valid 1-element environment invariant.
	Check( inlineSrc.Depth() == 1, "inlineSrc.Depth() == 1" );
	Check( inlineSrc.top() == 1.0, "inlineSrc.top() == 1.0" );
	Check( inlineSrc.CrossingEtaFrom() == 0, "inlineSrc.CrossingEtaFrom() == 0" );

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
		Check( spilledSrc.Depth() == 7, "spilledSrc.Depth() == 7" );

		IORStack spilledMoved( std::move( spilledSrc ) );
		Check( spilledMoved.Depth() == 7, "spilledMoved.Depth() == 7" );
		Check( spilledMoved.EnvironmentIOR() == Scalar( 1.001 ), "spilledMoved.EnvironmentIOR() == 1.001" );
		Check( spilledMoved.isOpenSheetAt( 3 ), "spilledMoved.isOpenSheetAt(3)" );
		Check( spilledMoved.ContextAt( 3 ).bHasContext, "spilledMoved.ContextAt(3).bHasContext" );
		Check( spilledSrc.Depth() == 1, "spilledSrc.Depth() == 1 after move" );
		Check( spilledSrc.top() == Scalar( 1.001 ), "spilledSrc.top() == 1.001 after move" );

		IORStack target( 1.0 );
		target = std::move( spilledMoved );
		Check( target.Depth() == 7, "target.Depth() == 7" );
		Check( target.isOpenSheetAt( 3 ), "target.isOpenSheetAt(3)" );
		Check( target.ContextAt( 3 ).bHasContext, "target.ContextAt(3).bHasContext" );
		Check( spilledMoved.Depth() == 1, "spilledMoved.Depth() == 1 after move assign" );
		Check( spilledMoved.top() == Scalar( 1.001 ), "spilledMoved.top() == 1.001 after move assign" );
	}
	for( int i = 0; i < 6; ++i ) {
		stubs[i]->release();
	}

	std::cout << "TestMoveInlineAndSpilled passed!" << std::endl;
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
	TestMoveInlineAndSpilled( stub );

	stub->release();
	secondStub->release();

	std::cout << "\nIORStackTest: " << gPassCount << " passed, " << gFailCount << " failed." << std::endl;
	return gFailCount == 0 ? 0 : 1;
}
