//////////////////////////////////////////////////////////////////////
//
//  SceneEditorBasicsTest.cpp - Phase 1 unit tests for the interactive
//    scene editor's data structures.
//
//  Covers the value-typed core (SceneEdit, EditHistory, Cancellable-
//  ProgressCallback) without requiring full scene construction.  The
//  end-to-end invariant-chain integration test (load a scene, apply
//  edits, render, undo, compare matrices) ships in Phase 2 alongside
//  the SceneEditController.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cassert>
#include <cstring>

#include "../src/Library/SceneEditor/SceneEdit.h"
#include "../src/Library/SceneEditor/EditHistory.h"
#include "../src/Library/SceneEditor/CancellableProgressCallback.h"

using namespace RISE;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition )
	{
		passCount++;
	}
	else
	{
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// 1. SceneEdit value-type behaviour
//////////////////////////////////////////////////////////////////////

static void TestSceneEditOpClassification()
{
	std::cout << "Testing SceneEdit op classification..." << std::endl;

	// Object ops
	Check( SceneEdit::IsObjectOp( SceneEdit::TranslateObject ),    "TranslateObject is object op" );
	Check( SceneEdit::IsObjectOp( SceneEdit::RotateObjectArb ),    "RotateObjectArb is object op" );
	Check( SceneEdit::IsObjectOp( SceneEdit::SetObjectPosition ),  "SetObjectPosition is object op" );
	Check( SceneEdit::IsObjectOp( SceneEdit::SetObjectScale ),     "SetObjectScale is object op" );

	// Camera ops are NOT object ops
	Check( !SceneEdit::IsObjectOp( SceneEdit::OrbitCamera ),       "OrbitCamera is NOT object op" );
	Check( !SceneEdit::IsObjectOp( SceneEdit::PanCamera ),         "PanCamera is NOT object op" );

	// Camera ops
	Check( SceneEdit::IsCameraOp( SceneEdit::OrbitCamera ),        "OrbitCamera is camera op" );
	Check( SceneEdit::IsCameraOp( SceneEdit::SetCameraTransform ), "SetCameraTransform is camera op" );

	// Object ops are NOT camera ops
	Check( !SceneEdit::IsCameraOp( SceneEdit::TranslateObject ),   "TranslateObject is NOT camera op" );

	// Time op is neither
	Check( !SceneEdit::IsObjectOp( SceneEdit::SetSceneTime ),      "SetSceneTime is NOT object op" );
	Check( !SceneEdit::IsCameraOp( SceneEdit::SetSceneTime ),      "SetSceneTime is NOT camera op" );

	// Composite markers
	Check( SceneEdit::IsCompositeMarker( SceneEdit::CompositeBegin ), "CompositeBegin is composite marker" );
	Check( SceneEdit::IsCompositeMarker( SceneEdit::CompositeEnd ),   "CompositeEnd is composite marker" );
	Check( !SceneEdit::IsCompositeMarker( SceneEdit::TranslateObject ), "TranslateObject is NOT composite marker" );
}

static void TestSceneEditDefaultCtor()
{
	std::cout << "Testing SceneEdit default ctor..." << std::endl;

	SceneEdit e;
	Check( e.s == 0,         "default scalar is zero" );
	Check( e.prevTime == 0,  "default prevTime is zero" );

	// Default prevTransform is identity — verify diagonal.
	Check( e.prevTransform._00 == 1.0 && e.prevTransform._11 == 1.0
	    && e.prevTransform._22 == 1.0 && e.prevTransform._33 == 1.0,
	    "default prevTransform is identity (diagonal)" );
	Check( e.prevTransform._01 == 0.0 && e.prevTransform._10 == 0.0
	    && e.prevTransform._12 == 0.0 && e.prevTransform._21 == 0.0,
	    "default prevTransform is identity (off-diagonal sample)" );
}

//////////////////////////////////////////////////////////////////////
// 2. EditHistory behaviour
//////////////////////////////////////////////////////////////////////

static SceneEdit MakeTranslate( const char* name, Scalar dx )
{
	SceneEdit e;
	e.op = SceneEdit::TranslateObject;
	e.objectName = name;
	e.v3a = Vector3( dx, 0, 0 );
	return e;
}

static void TestEditHistoryPushPop()
{
	std::cout << "Testing EditHistory push/pop..." << std::endl;

	EditHistory h;
	Check( h.UndoDepth() == 0, "fresh history undo depth 0" );
	Check( h.RedoDepth() == 0, "fresh history redo depth 0" );

	h.Push( MakeTranslate( "sphere1", 1.0 ) );
	h.Push( MakeTranslate( "sphere1", 2.0 ) );
	h.Push( MakeTranslate( "sphere2", 3.0 ) );
	Check( h.UndoDepth() == 3, "after 3 pushes undo depth = 3" );

	SceneEdit popped;
	Check( h.PopForUndo( popped ), "first PopForUndo succeeds" );
	Check( popped.op == SceneEdit::TranslateObject,    "popped op is TranslateObject" );
	Check( std::strcmp( popped.objectName.c_str(), "sphere2" ) == 0,
	       "popped name is sphere2 (LIFO order)" );
	Check( h.UndoDepth() == 2, "after one undo, undo depth = 2" );
	Check( h.RedoDepth() == 1, "after one undo, redo depth = 1" );

	// Redo
	Check( h.PopForRedo( popped ), "PopForRedo succeeds" );
	Check( std::strcmp( popped.objectName.c_str(), "sphere2" ) == 0,
	       "redo restores sphere2" );
	Check( h.UndoDepth() == 3, "after redo, undo depth = 3" );
	Check( h.RedoDepth() == 0, "after redo, redo depth = 0" );

	// New push clears redo stack
	h.Push( MakeTranslate( "sphere3", 4.0 ) );
	SceneEdit dummy;
	h.PopForUndo( dummy );
	Check( h.RedoDepth() == 1, "redo non-empty after undo" );
	h.Push( MakeTranslate( "sphere4", 5.0 ) );
	Check( h.RedoDepth() == 0, "Push clears redo stack" );

	// Empty pop fails
	EditHistory empty;
	Check( !empty.PopForUndo( dummy ), "PopForUndo on empty fails" );
	Check( !empty.PopForRedo( dummy ), "PopForRedo on empty fails" );
}

static void TestEditHistoryDirtyTracking()
{
	std::cout << "Testing EditHistory dirty-object tracking..." << std::endl;

	EditHistory h;

	String sphere1( "sphere1" );
	String sphere2( "sphere2" );
	String sphere3( "sphere3" );

	Check( !h.IsObjectDirty( sphere1 ), "fresh history: sphere1 not dirty" );

	h.Push( MakeTranslate( "sphere1", 1.0 ) );
	Check( h.IsObjectDirty( sphere1 ),  "after push: sphere1 dirty" );
	Check( !h.IsObjectDirty( sphere2 ), "after push: sphere2 not dirty" );

	h.Push( MakeTranslate( "sphere2", 1.0 ) );
	Check( h.IsObjectDirty( sphere1 ),  "after both pushes: sphere1 still dirty" );
	Check( h.IsObjectDirty( sphere2 ),  "after both pushes: sphere2 dirty" );
	Check( !h.IsObjectDirty( sphere3 ), "sphere3 still not dirty" );

	// Camera op should NOT mark any object dirty.
	SceneEdit cam;
	cam.op = SceneEdit::OrbitCamera;
	cam.v3a = Vector3( 0.1, 0.05, 0 );
	h.Push( cam );
	Check( !h.IsObjectDirty( sphere3 ), "camera op doesn't dirty sphere3" );

	// Composite markers shouldn't dirty.
	SceneEdit cb;
	cb.op = SceneEdit::CompositeBegin;
	cb.objectName = "drag";
	h.Push( cb );
	Check( !h.IsObjectDirty( String("drag") ), "composite-begin label is not an object name" );

	h.Clear();
	Check( !h.IsObjectDirty( sphere1 ), "after Clear: sphere1 not dirty" );
	Check( h.UndoDepth() == 0,           "after Clear: undo empty" );
	Check( h.RedoDepth() == 0,           "after Clear: redo empty" );
}

static void TestEditHistoryCapacity()
{
	std::cout << "Testing EditHistory capacity bound..." << std::endl;

	const unsigned int CAP = 8;
	EditHistory h( CAP );
	for( unsigned int i = 0; i < CAP * 3; ++i )
	{
		char name[32];
		std::snprintf( name, sizeof(name), "obj%u", i );
		h.Push( MakeTranslate( name, static_cast<Scalar>(i) ) );
	}
	Check( h.UndoDepth() == CAP, "history bounded at maxEntries" );
}

//////////////////////////////////////////////////////////////////////
// 3. CancellableProgressCallback behaviour
//////////////////////////////////////////////////////////////////////

namespace {

class CountingProgress : public IProgressCallback
{
public:
	CountingProgress() : nCalls( 0 ), shouldContinue( true ) {}
	bool Progress( const double, const double ) override
	{
		++nCalls;
		return shouldContinue;
	}
	void SetTitle( const char* ) override {}

	int  nCalls;
	bool shouldContinue;
};

}  // namespace

static void TestCancellableProgressBasic()
{
	std::cout << "Testing CancellableProgressCallback..." << std::endl;

	CountingProgress inner;
	CancellableProgressCallback cancel( &inner );

	Check( !cancel.IsCancelRequested(), "fresh cancel callback not cancelled" );
	Check( cancel.Progress( 0.5, 1.0 ), "Progress returns true when not cancelled" );
	Check( inner.nCalls == 1, "inner callback was invoked" );

	cancel.RequestCancel();
	Check( cancel.IsCancelRequested(), "after RequestCancel, IsCancelRequested true" );
	Check( !cancel.Progress( 0.6, 1.0 ), "Progress returns false when cancelled" );
	// When cancelled, the inner callback should NOT be consulted (avoid
	// cost of the inner sink during shutdown).
	Check( inner.nCalls == 1, "inner callback NOT invoked when cancelled" );

	cancel.Reset();
	Check( !cancel.IsCancelRequested(), "after Reset, not cancelled" );
	Check( cancel.Progress( 0.7, 1.0 ), "Progress returns true after Reset" );
	Check( inner.nCalls == 2, "inner callback invoked after Reset" );
}

static void TestCancellableProgressInnerVeto()
{
	std::cout << "Testing CancellableProgressCallback inner-callback veto..." << std::endl;

	CountingProgress inner;
	inner.shouldContinue = false;  // Inner says "abort"

	CancellableProgressCallback cancel( &inner );

	Check( !cancel.Progress( 0.5, 1.0 ),
	       "when inner says abort, Progress returns false" );
	// External cancel atomic remains clean — inner's "abort" doesn't
	// retroactively trip our flag.
	Check( !cancel.IsCancelRequested(),
	       "inner abort does NOT trip external cancel flag" );
}

static void TestCancellableProgressNullInner()
{
	std::cout << "Testing CancellableProgressCallback with null inner..." << std::endl;

	CancellableProgressCallback cancel( 0 );
	Check( cancel.Progress( 0.5, 1.0 ),
	       "with null inner, Progress returns true (no veto)" );
	cancel.RequestCancel();
	Check( !cancel.Progress( 0.6, 1.0 ),
	       "with null inner, Progress respects cancel flag" );

	// SetTitle should be a no-op on null inner — just check it doesn't crash.
	cancel.SetTitle( "hello" );
}

//////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== SceneEditor Basics Test (Phase 1) ===" << std::endl;

	TestSceneEditOpClassification();
	TestSceneEditDefaultCtor();
	TestEditHistoryPushPop();
	TestEditHistoryDirtyTracking();
	TestEditHistoryCapacity();
	TestCancellableProgressBasic();
	TestCancellableProgressInnerVeto();
	TestCancellableProgressNullInner();

	std::cout << "\n=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;

	return failCount == 0 ? 0 : 1;
}
