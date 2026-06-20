//////////////////////////////////////////////////////////////////////
//
//  SceneEditTransactionTest.cpp (feature/gui-snapshot-prototype,
//    increment #2b(b)): proof that the editor's transactional rollback
//    path is ATOMIC and built correctly on the snapshot/restore
//    primitives — a rolled-back gesture leaves the live scene EQUAL to
//    the baseline, leaves NOTHING redoable, and leaves the scene
//    RENDER-VALID (TLAS + LightSampler rebuilt); a committed gesture is
//    applied EXACTLY ONCE (no double-apply) and collapses to one undo
//    entry.
//
//  WHY THESE TESTS EXIST (STEP-1 finding):
//    The shipping interactive drag flow is ALREADY atomic-on-commit —
//    each OnPointerMove Apply mutates the live scene + records history,
//    and OnPointerUp's EndComposite only pushes a marker (NO re-apply,
//    NO double-apply).  There was NO gesture cancel/reject path at all.
//    So this increment ADDS a clean snapshot-based rollback primitive
//    (BeginTransaction / RollbackTransaction / EndTransaction) rather
//    than fixing a non-existent double-apply.  These tests pin the new
//    primitive and guard the unchanged commit flow.
//
//  Each test is built to FAIL if:
//    - rollback were a no-op (live state would still reflect the edits),
//    - rollback left the gesture REDOABLE (the EndComposite();Undo()
//      trap — EditHistory::PopForUndo moves records to the redo stack;
//      our DiscardUndoTo must NOT),
//    - rollback double-counted or left the scene render-invalid,
//    - commit re-applied (double-apply) or recorded != 1 undo entry.
//
//  Observables (no rendering — geometry / sampler queries only):
//    - object final-transform translation column (transform restore)
//    - object material's reflectance slot painter (material restore;
//      addref-shared through the snapshot clone, so identity is stable)
//    - live light energy (light restore)
//    - EditHistory UndoDepth / RedoDepth (history atomicity)
//    - IntersectRay against the rebuilt TLAS + LightSampler exitance on
//      a re-attached RayCaster (render-validity)
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/ILightPriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/Utilities/RadianceMapConfig.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
		std::cout << "  ok:   " << testName << std::endl;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Scene builder — programmatic Job with:
//   - "alpha" sphere on +X (probe target), 5 filler spheres off-axis
//     (forces the BVH / TLAS path, > 4 objects),
//   - two Lambertian materials "mat_red" (p_red) and "mat_blue" (p_blue)
//     so a material binding swap is observable through the bound
//     material's reflectance slot painter,
//   - a point-omni light "key" (surfaces in the LightSampler),
//   - a "global" shader so a RayCaster can be built (we never render).
//////////////////////////////////////////////////////////////////////
static Job* MakeTxnScene()
{
	Job* pJob = new Job();

	const double red[3]  = { 0.8, 0.1, 0.1 };
	const double blue[3] = { 0.1, 0.1, 0.8 };
	pJob->AddUniformColorPainter( "p_red",  red,  "Rec709RGB_Linear" );
	pJob->AddUniformColorPainter( "p_blue", blue, "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "mat_red",  "p_red" );
	pJob->AddLambertianMaterial( "mat_blue", "p_blue" );

	pJob->AddSphereGeometry( "geom", 1.0 );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };

	// "alpha" at x=8 so a ray from (20,0,0) toward -X hits it.
	const double posAlpha[3] = { 8, 0, 0 };
	pJob->AddObject( "alpha", "geom", "mat_red", nullptr, nullptr,
		nilRMap, posAlpha, orient, one3, true, true );

	for( int i = 0; i < 5; ++i ) {
		char nm[32];
		std::snprintf( nm, sizeof(nm), "filler%d", i );
		const double pos[3] = { -20.0, (double)( 4 * ( i + 1 ) ), 0.0 };
		pJob->AddObject( nm, "geom", "mat_red", nullptr, nullptr,
			nilRMap, pos, orient, one3, true, true );
	}

	const double lc[3] = { 1.0, 0.9, 0.8 };
	const double lp[3] = { 0, 10, 0 };
	pJob->AddPointOmniLight( "key", /*power*/ 5.0, lc, lp, /*shootPhotons*/ false );

	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );

	return pJob;
}

// --- observables -----------------------------------------------------

static double LiveObjX( IObjectManager* objs, const char* name )
{
	IObjectPriv* o = objs->GetItem( name );
	if( !o ) { return std::nan( "" ); }
	return (double)o->GetFinalTransformMatrix()._30;
}

static double LiveLightEnergy( ILightManager* lights, const char* name )
{
	ILightPriv* l = lights->GetItem( name );
	if( !l ) { return std::nan( "" ); }
	return (double)l->emissionEnergy();
}

// The reflectance-slot painter of the material BOUND TO the named live
// object.  The snapshot clones the object with a cloned material whose
// sub-painters are addref-SHARED, so the slot painter pointer survives
// the snapshot/restore round trip (mirrors SceneRestoreTest::ObjMatSlot).
static const IPainter* ObjMatSlot( IObjectManager* objs, const char* objName )
{
	IObjectPriv* o = objs->GetItem( objName );
	if( !o ) { return nullptr; }
	const IMaterial* m = o->GetMaterial();
	if( !m ) { return nullptr; }
	const MaterialSlotRef r = MaterialIntrospection::GetSlot( *m, String( "reflectance" ) );
	return ( r.kind == MaterialSlotRef::Painter ) ? r.painter : nullptr;
}

static const LightSampler* SamplerOf( IRayCaster* caster )
{
	RayCaster* rc = dynamic_cast<RayCaster*>( caster );
	return rc ? rc->GetLightSampler() : nullptr;
}

//////////////////////////////////////////////////////////////////////
// T-rollback-restores-baseline
//
// Begin a transaction, mutate (object transform + light + material) via
// the REAL editor controller gesture path, then ROLLBACK.  Assert the
// scene equals the baseline on all three dimensions, that NOTHING is
// redoable, and that the scene is render-valid (re-attach a RayCaster:
// the TLAS rebuilds and a probe ray hits the restored alpha; the
// LightSampler rebuilds and reflects the restored light energy).
//////////////////////////////////////////////////////////////////////
static void TestRollbackRestoresBaseline()
{
	std::cout << "Test: rollback restores baseline (transform + light + material), nothing redoable, render-valid"
	          << std::endl;

	Job* pJob = MakeTxnScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[rb] scene downcast" ); pJob->release(); return; }

	IObjectManager*   objs   = pJob->GetObjects();
	IMaterialManager* mats   = pJob->GetMaterials();
	ILightManager*    lights = pJob->GetLights();
	IPainterManager*  pnts   = pJob->GetPainters();
	IShaderManager*   shaders= pJob->GetShaders();
	if( !objs || !mats || !lights || !pnts || !shaders ) {
		Check( false, "[rb] managers resolvable" ); pJob->release(); return;
	}
	const IPainter* pRed  = pnts->GetItem( "p_red" );
	const IPainter* pBlue = pnts->GetItem( "p_blue" );

	// Editor controller in "queue edits, no rendering" mode (no Start()).
	SceneEditController ctrl( *pJob, /*interactiveRasterizer*/ 0 );

	// Baseline values.
	const double    baseX   = LiveObjX( objs, "alpha" );          // 8
	const double    baseE   = LiveLightEnergy( lights, "key" );   // 5
	const IPainter* baseSlot= ObjMatSlot( objs, "alpha" );         // p_red
	Check( std::abs( baseX - 8.0 ) < 1e-9, "[rb] baseline alpha x=8" );
	Check( std::abs( baseE - 5.0 ) < 1e-9, "[rb] baseline light energy 5" );
	Check( baseSlot == pRed, "[rb] baseline material slot p_red" );

	const unsigned int undoAtBegin = ctrl.Editor().History().UndoDepth();

	// --- Begin the transaction (capture baseline snapshot). ---
	Check( ctrl.BeginTransaction(), "[rb] BeginTransaction succeeds on concrete Scene" );
	Check( ctrl.IsTransactionOpen(), "[rb] transaction is open after Begin" );

	// --- Mutate all three dimensions through the controller gesture path. ---
	// Wrap the edits in a composite (as a drag would) to ALSO prove the
	// rollback cleans up an open composite without leaving an orphan
	// CompositeEnd.
	ctrl.Editor().BeginComposite( "txn-drag" );

	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "alpha" ) );
	Check( ctrl.SetProperty( String( "position" ), String( "18 0 0" ) ),
	       "[rb] SetProperty(position=18 0 0) applied" );
	Check( ctrl.SetProperty( String( "material" ), String( "mat_blue" ) ),
	       "[rb] SetProperty(material=mat_blue) applied" );

	ctrl.ForTest_SetSelection( SceneEditController::Category::Light, String( "key" ) );
	Check( ctrl.SetProperty( String( "energy" ), String( "99.0" ) ),
	       "[rb] SetProperty(energy=99) applied" );

	// Verify the live scene actually moved AWAY from baseline first.
	Check( std::abs( LiveObjX( objs, "alpha" ) - 18.0 ) < 1e-9, "[rb] live alpha moved to x=18" );
	Check( ObjMatSlot( objs, "alpha" ) == pBlue, "[rb] live material slot now p_blue" );
	Check( std::abs( LiveLightEnergy( lights, "key" ) - 99.0 ) < 1e-9, "[rb] live light energy now 99" );
	Check( ctrl.Editor().History().UndoDepth() > undoAtBegin, "[rb] history grew during the transaction" );

	// --- ROLLBACK. ---
	Check( ctrl.RollbackTransaction(), "[rb] RollbackTransaction succeeds" );
	Check( !ctrl.IsTransactionOpen(), "[rb] transaction is closed after rollback" );

	// Re-resolve live entities (restore replaced manager contents with clones).
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9,
	       "[rb] RESTORED alpha x back to 8 (was 18)" );
	Check( std::abs( LiveLightEnergy( lights, "key" ) - 5.0 ) < 1e-9,
	       "[rb] RESTORED light energy back to 5 (was 99)" );
	Check( ObjMatSlot( objs, "alpha" ) == pRed,
	       "[rb] RESTORED material slot back to p_red (was p_blue)" );
	Check( objs->getItemCount() == 6, "[rb] 6 objects after rollback" );
	Check( lights->getItemCount() == 1, "[rb] 1 light after rollback" );

	// --- History atomicity: undo depth back to begin, NOTHING redoable. ---
	Check( ctrl.Editor().History().UndoDepth() == undoAtBegin,
	       "[rb] undo depth reset to the pre-transaction depth" );
	Check( ctrl.Editor().History().RedoDepth() == 0,
	       "[rb] NOTHING redoable after rollback (no EndComposite();Undo() redo residue)" );
	// Decisive: a Redo() must do nothing (the discarded gesture is gone,
	// not merely hidden).  A stale redo entry would re-apply the edits.
	Check( !ctrl.Editor().Redo(),
	       "[rb] Editor().Redo() is a no-op after rollback (gesture truly discarded)" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9,
	       "[rb] alpha STILL at baseline x=8 after attempted Redo (no re-apply)" );

	// The open composite must have been neutralized: a stray EndComposite
	// (tool cleanup that fires after a mid-gesture rollback) must NOT push
	// an orphan marker that corrupts subsequent history.
	const unsigned int undoBeforeStrayEnd = ctrl.Editor().History().UndoDepth();
	ctrl.Editor().EndComposite();   // should be a no-op (depth forced to 0)
	Check( ctrl.Editor().History().UndoDepth() == undoBeforeStrayEnd,
	       "[rb] stray EndComposite after rollback pushes no orphan marker" );

	// --- Render-validity: TLAS + LightSampler rebuilt over restored state. ---
	IShader* pShader = shaders->GetItem( "global" );
	if( !pShader ) { Check( false, "[rb] global shader present" ); pJob->release(); return; }
	IRayCaster* caster = nullptr;
	RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
	if( !caster ) { Check( false, "[rb] caster create" ); pJob->release(); return; }

	caster->AttachScene( pScene );   // builds TLAS + sampler over the RESTORED scene

	// TLAS: probe ray from (20,0,0) toward -X must hit the restored alpha
	// at x=8 (a stale structure built over released mutated clones would
	// miss or hit a dangling pointer).
	{
		RayIntersection ri( Ray( Point3( 20, 0, 0 ), Vector3( -1, 0, 0 ) ), nullRasterizerState );
		objs->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit, "[rb] post-rollback probe ray hits the restored alpha" );
		Check( ri.pObject == (const IObject*)objs->GetItem( "alpha" ),
		       "[rb] post-rollback hit IS the restored alpha instance (TLAS rebuilt)" );
	}

	// LightSampler: reflects the restored single light at energy 5 (NOT
	// the rolled-back energy 99).  Exitance is monotone in energy, so a
	// stale sampler that kept energy-99 would report a far larger value.
	{
		const LightSampler* s = SamplerOf( caster );
		Check( s != nullptr, "[rb] sampler present after attach" );
		if( s ) {
			Check( s->GetPositionalLightCount() == 1,
			       "[rb] sampler sees the 1 restored light" );
			// Build a SEPARATE reference scene at the baseline energy and
			// compare exitance — proves the sampler matches the restored
			// (baseline) light, not the divergent energy-99 state.
			Job* pRef = MakeTxnScene();
			Scene* pRefScene = dynamic_cast<Scene*>( pRef->GetScene() );
			IShader* pRefShader = pRef->GetShaders()->GetItem( "global" );
			IRayCaster* refCaster = nullptr;
			RISE_API_CreateRayCaster( &refCaster, false, 10, *pRefShader, true );
			refCaster->AttachScene( pRefScene );
			const LightSampler* sRef = SamplerOf( refCaster );
			if( s->GetPositionalLightCount() >= 1 && sRef && sRef->GetPositionalLightCount() >= 1 ) {
				const double got = (double)s->GetPositionalLightExitance( 0 );
				const double ref = (double)sRef->GetPositionalLightExitance( 0 );
				Check( std::abs( got - ref ) < 1e-6 * ( 1.0 + std::abs( ref ) ),
				       "[rb] restored sampler exitance == fresh baseline (light rolled back, not stale 99)" );
			}
			safe_release( refCaster );
			pRef->release();
		}
	}

	safe_release( caster );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-commit-no-double-apply
//
// Commit (EndTransaction) a single-edit gesture; assert the result is
// the SINGLE intended edit (not applied twice) and that it is ONE undo
// entry — proving commit is record-only (the shipping flow), and that
// wrapping a gesture in Begin/End does not duplicate the mutation.
//////////////////////////////////////////////////////////////////////
static void TestCommitNoDoubleApply()
{
	std::cout << "Test: commit is record-only — single edit applied once, one undo entry, no double-apply"
	          << std::endl;

	Job* pJob = MakeTxnScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[ci] scene downcast" ); pJob->release(); return; }

	IObjectManager* objs = pJob->GetObjects();
	SceneEditController ctrl( *pJob, /*interactiveRasterizer*/ 0 );

	const double baseX = LiveObjX( objs, "alpha" );   // 8
	Check( std::abs( baseX - 8.0 ) < 1e-9, "[ci] baseline alpha x=8" );

	const unsigned int undoBefore = ctrl.Editor().History().UndoDepth();

	Check( ctrl.BeginTransaction(), "[ci] BeginTransaction succeeds" );

	// One composite-wrapped absolute-position edit: set alpha to x=15.
	ctrl.Editor().BeginComposite( "commit-drag" );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "alpha" ) );
	Check( ctrl.SetProperty( String( "position" ), String( "15 0 0" ) ),
	       "[ci] SetProperty(position=15 0 0) applied" );
	ctrl.Editor().EndComposite();

	// COMMIT.
	Check( ctrl.EndTransaction(), "[ci] EndTransaction succeeds" );
	Check( !ctrl.IsTransactionOpen(), "[ci] transaction closed after commit" );

	// Result is the SINGLE intended edit: x=15 EXACTLY, not 22 (=8 + 7
	// applied twice) or any doubled value.  SetObjectPosition is absolute,
	// so a literal double-apply would still land at 15 — but a double-apply
	// of the underlying mutation or a re-played composite would inflate the
	// UNDO DEPTH, which the next check catches.
	Check( std::abs( LiveObjX( objs, "alpha" ) - 15.0 ) < 1e-9,
	       "[ci] alpha at x=15 after commit (single intended edit, not double-applied)" );

	// Exactly ONE new undo unit (the composite: Begin + edit + End).  A
	// re-apply-on-commit would push a SECOND composite — undo depth would
	// jump by 6 markers/edits instead of 3.  Decisive cross-check: a
	// single Undo of the composite returns to baseline x=8.
	Check( ctrl.Editor().History().UndoDepth() == undoBefore + 3,
	       "[ci] exactly one composite recorded (Begin+edit+End = 3 entries), no re-apply" );
	Check( ctrl.Editor().Undo(), "[ci] single Undo of the committed composite succeeds" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9,
	       "[ci] one Undo returns alpha to baseline x=8 (committed gesture is ONE undo entry)" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-rollback-reusable-and-guards
//
// Two-mutate-two-rollback against the SAME controller proves the
// rollback path is repeatable (each Begin snapshots the then-current
// baseline, each Rollback restores it).  Also exercises the no-op
// guards: Rollback / End with no open transaction return false.
//////////////////////////////////////////////////////////////////////
static void TestRollbackRepeatableAndGuards()
{
	std::cout << "Test: rollback is repeatable + no-transaction guards return false"
	          << std::endl;

	Job* pJob = MakeTxnScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[rr] scene downcast" ); pJob->release(); return; }

	IObjectManager* objs = pJob->GetObjects();
	SceneEditController ctrl( *pJob, /*interactiveRasterizer*/ 0 );

	// Guards before any transaction.
	Check( !ctrl.IsTransactionOpen(), "[rr] no transaction open initially" );
	Check( !ctrl.RollbackTransaction(), "[rr] Rollback with no transaction returns false" );
	Check( !ctrl.EndTransaction(), "[rr] End with no transaction returns false" );

	// Round 1: baseline x=8 -> mutate to x=30 -> rollback -> x=8.
	Check( ctrl.BeginTransaction(), "[rr] Begin (round 1)" );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "alpha" ) );
	Check( ctrl.SetProperty( String( "position" ), String( "30 0 0" ) ), "[rr] edit round 1" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 30.0 ) < 1e-9, "[rr] live x=30 round 1" );
	Check( ctrl.RollbackTransaction(), "[rr] rollback round 1" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9, "[rr] restored x=8 round 1" );

	// Round 2: SAME controller, baseline still x=8 -> mutate to x=-12 ->
	// rollback -> x=8 again.  Proves the snapshot is not consumed / no
	// stale baseline lingers.
	Check( ctrl.BeginTransaction(), "[rr] Begin (round 2)" );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "alpha" ) );
	Check( ctrl.SetProperty( String( "position" ), String( "-12 0 0" ) ), "[rr] edit round 2" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - ( -12.0 ) ) < 1e-9, "[rr] live x=-12 round 2" );
	Check( ctrl.RollbackTransaction(), "[rr] rollback round 2" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9, "[rr] restored x=8 round 2" );
	Check( ctrl.Editor().History().RedoDepth() == 0, "[rr] nothing redoable after round 2" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== SceneEditTransactionTest ===" << std::endl;

	TestRollbackRestoresBaseline();
	TestCommitNoDoubleApply();
	TestRollbackRepeatableAndGuards();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
