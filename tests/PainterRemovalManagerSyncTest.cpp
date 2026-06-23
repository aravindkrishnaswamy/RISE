//////////////////////////////////////////////////////////////////////
//
//  PainterRemovalManagerSyncTest.cpp - Regression tests for the painter
//    dual-registration contract: every Add*Painter registers the SAME
//    painter object in BOTH pPntManager (its primary home) and
//    pFunc2DManager (a secondary IFunction2D index, since a painter
//    IS-A IFunction2D).  Job::RemovePainter must reverse BOTH adds, or
//    a removed painter stays resolvable as a 2D function and a re-add of
//    the same name collides in the function-2D manager.
//
//    The fix is identity-gated: the secondary func2D entry is only
//    dropped when it IS the very painter being removed -- never a
//    standalone `function_2d` chunk that happens to share the name (those
//    register into the same pFunc2DManager namespace).
//
//    See Job::RemovePainter (src/Library/Job.cpp).
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstring>
#include <iostream>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IFunction1D.h"
#include "../src/Library/Interfaces/IFunction1DManager.h"
#include "../src/Library/Interfaces/IFunction2D.h"
#include "../src/Library/Interfaces/IFunction2DManager.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) { passCount++; }
	else { failCount++; std::cout << "  FAIL: " << testName << std::endl; }
}

// Convenience: does the painter manager resolve `name`?
static bool InPainters( Job* job, const char* name )
{
	return job->GetPainters()->GetItem( name ) != nullptr;
}

// Convenience: does the function-2D manager resolve `name`?
static bool InFunc2D( Job* job, const char* name )
{
	return job->GetFunction2Ds()->GetItem( name ) != nullptr;
}

static bool AddPainter( Job* job, const char* name )
{
	const double grey[3] = { 0.5, 0.5, 0.5 };
	// cspace == nullptr -> treated as sRGB (matches the scene-file default).
	return job->AddUniformColorPainter( name, grey, nullptr );
}

//////////////////////////////////////////////////////////////////////
// 1. A painter is registered in BOTH managers, and RemovePainter clears
//    BOTH.  This is the core of the bug: pre-fix, the func2D entry leaked.
//////////////////////////////////////////////////////////////////////

static void TestRemoveClearsBothManagers()
{
	std::cout << "Testing RemovePainter clears both pPntManager and pFunc2DManager..." << std::endl;
	Job* job = new Job();

	Check( AddPainter( job, "p1" ),   "AddUniformColorPainter(p1) succeeds" );
	Check( InPainters( job, "p1" ),   "p1 present in painter manager after add" );
	Check( InFunc2D( job, "p1" ),     "p1 present in function-2D manager after add (dual-register)" );

	Check( job->RemovePainter( "p1" ), "RemovePainter(p1) succeeds" );
	Check( !InPainters( job, "p1" ),  "p1 gone from painter manager after remove" );
	Check( !InFunc2D( job, "p1" ),    "p1 gone from function-2D manager after remove (no stale entry)" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// 2. Remove-then-re-add under the same name must succeed with no
//    "Item of same name already exists" rejection in EITHER manager.
//    This is the regression the bug caused (the CST incremental re-derive
//    and SceneEditor runtime painter edits hit exactly this path).
//////////////////////////////////////////////////////////////////////

static void TestReAddSameNameSucceeds()
{
	std::cout << "Testing remove + re-add of the same painter name succeeds..." << std::endl;
	Job* job = new Job();

	Check( AddPainter( job, "p2" ),    "first AddUniformColorPainter(p2)" );
	Check( job->RemovePainter( "p2" ), "RemovePainter(p2)" );

	// Pre-fix this returned true at the Job level but left pFunc2DManager
	// dirty; the second add's func2D registration silently failed, and the
	// painter was only half-registered.  Post-fix both managers are clean,
	// so a full re-registration succeeds.
	Check( AddPainter( job, "p2" ),    "re-add AddUniformColorPainter(p2) succeeds" );
	Check( InPainters( job, "p2" ),    "p2 present in painter manager after re-add" );
	Check( InFunc2D( job, "p2" ),      "p2 present in function-2D manager after re-add" );

	// The decisive check: the func2D entry must be the NEWLY re-added
	// painter, not a stale entry left by the first add.  Pre-fix the first
	// RemovePainter left the original painter in pFunc2DManager, so the
	// re-add's func2D registration was refused and the func2D name still
	// resolved to the OLD (removed) object -- a different pointer.
	IPainter*    reAdded = job->GetPainters()->GetItem( "p2" );
	IFunction2D* asFunc2D = job->GetFunction2Ds()->GetItem( "p2" );
	Check( reAdded != nullptr && asFunc2D != nullptr &&
	       asFunc2D == static_cast<IFunction2D*>( reAdded ),
	       "func2D 'p2' resolves to the re-added painter (no stale object)" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// 3. Identity gate: a standalone function_2d that SHARES a name with a
//    painter must NOT be clobbered when the painter is removed.
//
//    Setup forces the coexistence the gate must defend against:
//      - AddPiecewiseLinearFunction("f1") registers func1D "f1".
//      - AddPiecewiseLinearFunction2D("shared", {"f1"}) registers a
//        standalone IFunction2D "shared" in pFunc2DManager.
//      - AddUniformColorPainter("shared") then succeeds in pPntManager
//        (no painter "shared" yet) but its func2D dual-register is
//        REFUSED ("shared" already taken by the standalone func) -- the
//        Add*Painter code ignores that return value, so painter "shared"
//        and standalone-func2D "shared" now coexist under one name.
//    RemovePainter("shared") must drop ONLY the painter, leaving the
//    standalone function_2d intact.
//////////////////////////////////////////////////////////////////////

static void TestStandaloneFunc2DNotClobbered()
{
	std::cout << "Testing standalone function_2d sharing a name survives RemovePainter..." << std::endl;
	Job* job = new Job();

	const double x[2] = { 0.0, 1.0 };
	const double y[2] = { 0.0, 1.0 };
	Check( job->AddPiecewiseLinearFunction( "f1", x, y, 2, false, 0 ),
	       "AddPiecewiseLinearFunction(f1)" );

	char f1name[] = "f1";
	char* yNames[2] = { f1name, f1name };
	Check( job->AddPiecewiseLinearFunction2D( "shared", x, yNames, 2 ),
	       "AddPiecewiseLinearFunction2D(shared) -> standalone func2D" );
	Check( InFunc2D( job, "shared" ),  "standalone func2D 'shared' present" );
	Check( !InPainters( job, "shared" ), "no painter 'shared' yet" );

	// Painter add succeeds in the painter manager; its func2D dual-register
	// is refused (name taken by the standalone func) and ignored.
	Check( AddPainter( job, "shared" ), "AddUniformColorPainter(shared) succeeds in painter manager" );
	Check( InPainters( job, "shared" ), "painter 'shared' now present" );
	Check( InFunc2D( job, "shared" ),   "func2D 'shared' still the standalone (not the painter)" );

	// The critical assertion: removing the painter must NOT delete the
	// standalone function_2d that merely shares the name.
	Check( job->RemovePainter( "shared" ), "RemovePainter(shared) succeeds" );
	Check( !InPainters( job, "shared" ),   "painter 'shared' removed" );
	Check( InFunc2D( job, "shared" ),      "standalone func2D 'shared' SURVIVES (identity gate)" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// 4. A painter that is NOT dual-registered into pFunc2DManager removes
//    cleanly (the func2D step is a safe no-op).  AddPiecewiseLinearFunction
//    creates a spectral painter in pPntManager + its SOURCE func1D in
//    pFunc1DManager (a different object, NOT in pFunc2DManager).
//    RemovePainter drops the painter; the primary func1D is intentionally
//    left in place (it is independently referenceable, e.g. by
//    AddPiecewiseLinearFunction2D).  This locks the deliberate scope of the
//    fix -- and the KNOWN LIMITATION it leaves: because the source func1D is
//    retained, a remove-then-re-add of this painter type still collides on
//    the func1D re-registration (see Job::RemovePainter).  That is a separate
//    per-sub-type rollback, out of scope for the func2D-index fix.
//////////////////////////////////////////////////////////////////////

static void TestNonFunc2DPainterRemovesCleanly()
{
	std::cout << "Testing removal of a painter not homed in pFunc2DManager..." << std::endl;
	Job* job = new Job();

	const double x[2] = { 0.0, 1.0 };
	const double y[2] = { 0.0, 1.0 };
	Check( job->AddPiecewiseLinearFunction( "spec", x, y, 2, false, 0 ),
	       "AddPiecewiseLinearFunction(spec)" );
	Check( InPainters( job, "spec" ), "spectral painter 'spec' present in painter manager" );
	Check( !InFunc2D( job, "spec" ),  "'spec' NOT in function-2D manager (func1D-homed)" );
	Check( job->GetFunction1Ds()->GetItem( "spec" ) != nullptr,
	       "'spec' source function1D present in function-1D manager" );

	Check( job->RemovePainter( "spec" ), "RemovePainter(spec) succeeds (func2D step is a no-op)" );
	Check( !InPainters( job, "spec" ),   "painter 'spec' removed" );
	// Intentional scope: the primary source function1D is NOT removed.
	Check( job->GetFunction1Ds()->GetItem( "spec" ) != nullptr,
	       "source function1D 'spec' intentionally retained" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// 5. Repeated add/remove cycle stays balanced across both managers.
//    A leaked func2D entry would compound and surface here (and under
//    asan/tsan if the suite is run with them).
//////////////////////////////////////////////////////////////////////

static void TestRepeatedCycleStaysBalanced()
{
	std::cout << "Testing repeated add/remove cycle keeps both managers in sync..." << std::endl;
	Job* job = new Job();

	const unsigned int baseFunc2D   = job->GetFunction2Ds()->getItemCount();
	const unsigned int basePainters = job->GetPainters()->getItemCount();

	for( int i = 0; i < 32; ++i ) {
		char nm[16];
		std::snprintf( nm, sizeof(nm), "cyc_%d", i );
		Check( AddPainter( job, nm ),  "cycle add" );
		Check( InPainters( job, nm ),  "cycle: in painter mgr" );
		Check( InFunc2D( job, nm ),    "cycle: in func2D mgr" );
		Check( job->RemovePainter( nm ), "cycle remove" );
		Check( !InPainters( job, nm ), "cycle: out of painter mgr" );
		Check( !InFunc2D( job, nm ),   "cycle: out of func2D mgr" );
	}

	Check( job->GetFunction2Ds()->getItemCount() == baseFunc2D,
	       "function-2D manager count returned to baseline (no leak)" );
	Check( job->GetPainters()->getItemCount() == basePainters,
	       "painter manager count returned to baseline (no leak)" );

	job->release();
}

//////////////////////////////////////////////////////////////////////
// 6. Unknown / null name returns false and does not disturb state.
//////////////////////////////////////////////////////////////////////

static void TestUnknownAndNullName()
{
	std::cout << "Testing RemovePainter on unknown / null name..." << std::endl;
	Job* job = new Job();

	Check( AddPainter( job, "keep" ),     "add a painter to keep" );

	Check( !job->RemovePainter( "nope" ), "RemovePainter(unknown) returns false" );
	Check( !job->RemovePainter( nullptr ),"RemovePainter(null) returns false" );

	// The unrelated painter is untouched in both managers.
	Check( InPainters( job, "keep" ),     "unrelated painter still in painter mgr" );
	Check( InFunc2D( job, "keep" ),       "unrelated painter still in func2D mgr" );

	job->release();
}

//////////////////////////////////////////////////////////////////////

int main( int /*argc*/, char* /*argv*/[] )
{
	std::cout << "=== PainterRemovalManagerSyncTest ===" << std::endl;

	TestRemoveClearsBothManagers();
	TestReAddSameNameSucceeds();
	TestStandaloneFunc2DNotClobbered();
	TestNonFunc2DPainterRemovesCleanly();
	TestRepeatedCycleStaysBalanced();
	TestUnknownAndNullName();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
