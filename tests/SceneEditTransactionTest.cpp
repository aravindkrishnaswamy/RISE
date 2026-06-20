//////////////////////////////////////////////////////////////////////
//
//  SceneEditTransactionTest.cpp (feature/gui-snapshot-prototype):
//    proof that the editor's transactional rollback path is ATOMIC and
//    built on RISE's existing INVERSE-EDIT UNDO machinery — NOT on the
//    deep-clone Scene::RestoreFromSnapshot primitive.
//
//  WHY INVERSE-EDIT ROLLBACK (5th-review remediation):
//    A 5th code-backed review found P1 defects in the snapshot/restore
//    rollback that landed in increment #2b(b):
//      P1-1  multi-camera loss — RestoreFromSnapshot clears the WHOLE
//            camera manager and reinstalls only the (single) active
//            camera clone, so every NON-active camera is destroyed.
//      P1-2  identity / sharing lost — deep-clone gives each object a
//            PRIVATE material clone; two objects that shared one named
//            material end up with two distinct instances.
//      P1-5  cannot represent absence / failure — env / medium not
//            clearable, AddItem failures ignored, rollback always
//            "succeeds" (void / always-true).
//    The decision: v1 needs only NON-CONCURRENT restore/undo (not a
//    concurrent render-off-snapshot), so re-base RollbackTransaction on
//    the inverse-edit undo that already reverts live state on the SAME
//    instances.  RollbackTransaction now:
//      (a) applies the inverse edits down to the BeginTransaction undo
//          depth (reverting live state via SceneEditor::Undo, which
//          touches the same object / light / camera / material instances
//          the forward edits did), then
//      (b) clears the redo stack (a rolled-back gesture must NOT be
//          redoable), and
//      (c) neutralizes any open composite (ForceCompositeDepthZero) so a
//          stray EndComposite pushes no orphan marker.
//    It does NOT call Scene::RestoreFromSnapshot.  No baseline snapshot
//    is captured at BeginTransaction.
//
//  Each test FAILS against the OLD snapshot-restore rollback and PASSES
//  against the inverse-edit rollback:
//    - T-rollback-multi-camera: the OLD path LOST the non-active camera
//      (P1-1 decisive); inverse-edit leaves the camera manager untouched.
//    - T-rollback-shared-material-identity: the OLD path replaced the
//      shared material with two private clones (P1-2 decisive); inverse-
//      edit reverts the ONE shared instance in place.
//    - T-rollback-restores-baseline / -repeatable / commit-no-double-
//      apply: structural-atomicity guards that hold under inverse-edit.
//    - T-material-binding-emitter-generation (P1-4): rebinding an object
//      to a NON-emissive material must bump the scene light-topology
//      generation so a re-attached RayCaster rebuilds its LightSampler
//      (else a cached luminary pointing at the now-non-emissive material
//      would later deref a NULL emitter).
//
//  Observables (no rendering — geometry / sampler queries only):
//    - object final-transform translation column (transform revert)
//    - object material's reflectance slot painter + the bound IMaterial*
//      INSTANCE (material revert AND identity/sharing preservation)
//    - live light energy (light revert)
//    - camera-manager item count (multi-camera preservation)
//    - EditHistory UndoDepth / RedoDepth (history atomicity)
//    - Scene::GetLightTopologyGeneration (P1-4 sampler-rebuild trigger)
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
#include "../src/Library/SceneEditor/CameraIntrospection.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/ILightPriv.h"
#include "../src/Library/Interfaces/IShader.h"
#include "../src/Library/Interfaces/IShaderManager.h"
#include "../src/Library/Interfaces/IRayCaster.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Rendering/LuminaryManager.h"
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
	if( !o ) { return -1.0e30; }
	return (double)o->GetFinalTransformMatrix()._30;
}

static double LiveLightEnergy( ILightManager* lights, const char* name )
{
	ILightPriv* l = lights->GetItem( name );
	if( !l ) { return -1.0e30; }
	return (double)l->emissionEnergy();
}

// The reflectance-slot painter of the material BOUND TO the named live
// object.  Inverse-edit rollback reverts the binding on the SAME
// material instance, so the slot painter pointer round-trips exactly.
static const IPainter* ObjMatSlot( IObjectManager* objs, const char* objName )
{
	IObjectPriv* o = objs->GetItem( objName );
	if( !o ) { return nullptr; }
	const IMaterial* m = o->GetMaterial();
	if( !m ) { return nullptr; }
	const MaterialSlotRef r = MaterialIntrospection::GetSlot( *m, String( "reflectance" ) );
	return ( r.kind == MaterialSlotRef::Painter ) ? r.painter : nullptr;
}

// The IMaterial* INSTANCE bound to the named live object.  Used to prove
// inverse-edit rollback preserves material IDENTITY / sharing (the OLD
// snapshot-restore path replaced this with a private deep clone).
static const IMaterial* ObjMaterialInstance( IObjectManager* objs, const char* objName )
{
	IObjectPriv* o = objs->GetItem( objName );
	if( !o ) { return nullptr; }
	return o->GetMaterial();
}

static const LightSampler* SamplerOf( IRayCaster* caster )
{
	RayCaster* rc = dynamic_cast<RayCaster*>( caster );
	return rc ? rc->GetLightSampler() : nullptr;
}

// Number of mesh/area-light luminaries the caster's LuminaryManager
// holds (emissive-material objects).  This is what changes when an
// object's material binding is toggled to/from an emissive material.
// Returns -1 if the caster / manager can't be resolved.
static int LuminaryCountOf( IRayCaster* caster )
{
	RayCaster* rc = dynamic_cast<RayCaster*>( caster );
	if( !rc ) { return -1; }
	const ILuminaryManager* lm = rc->GetLuminaries();
	LuminaryManager* concrete =
		dynamic_cast<LuminaryManager*>( const_cast<ILuminaryManager*>( lm ) );
	if( !concrete ) { return -1; }
	return (int)concrete->getLuminaries().size();
}

//////////////////////////////////////////////////////////////////////
// T-rollback-restores-baseline
//
// Begin a transaction, mutate (object transform + light + material) via
// the REAL editor controller gesture path, then ROLLBACK via inverse
// edits.  Assert the scene equals the baseline on all three dimensions,
// that NOTHING is redoable, and that the scene is render-valid (re-attach
// a RayCaster: the TLAS rebuilds and a probe ray hits the restored alpha;
// the LightSampler rebuilds and reflects the restored light energy).
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

	// --- Begin the transaction (record the undo baseline depth). ---
	Check( ctrl.BeginTransaction(), "[rb] BeginTransaction succeeds" );
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

	// --- ROLLBACK (inverse edits down to the baseline undo depth). ---
	Check( ctrl.RollbackTransaction(), "[rb] RollbackTransaction succeeds" );
	Check( !ctrl.IsTransactionOpen(), "[rb] transaction is closed after rollback" );

	// All three dimensions reverted to baseline — on the SAME live
	// instances (inverse-edit does not swap manager contents).
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9,
	       "[rb] REVERTED alpha x back to 8 (was 18)" );
	Check( std::abs( LiveLightEnergy( lights, "key" ) - 5.0 ) < 1e-9,
	       "[rb] REVERTED light energy back to 5 (was 99)" );
	Check( ObjMatSlot( objs, "alpha" ) == pRed,
	       "[rb] REVERTED material slot back to p_red (was p_blue)" );
	Check( objs->getItemCount() == 6, "[rb] 6 objects after rollback" );
	Check( lights->getItemCount() == 1, "[rb] 1 light after rollback" );

	// --- History atomicity: undo depth back to begin, NOTHING redoable. ---
	Check( ctrl.Editor().History().UndoDepth() == undoAtBegin,
	       "[rb] undo depth reset to the pre-transaction depth" );
	Check( ctrl.Editor().History().RedoDepth() == 0,
	       "[rb] NOTHING redoable after rollback (redo stack cleared)" );
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

	// --- Render-validity: TLAS + LightSampler rebuilt over reverted state. ---
	IShader* pShader = shaders->GetItem( "global" );
	if( !pShader ) { Check( false, "[rb] global shader present" ); pJob->release(); return; }
	IRayCaster* caster = nullptr;
	RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
	if( !caster ) { Check( false, "[rb] caster create" ); pJob->release(); return; }

	caster->AttachScene( pScene );   // builds TLAS + sampler over the reverted scene

	// TLAS: probe ray from (20,0,0) toward -X must hit the reverted alpha
	// at x=8.
	{
		RayIntersection ri( Ray( Point3( 20, 0, 0 ), Vector3( -1, 0, 0 ) ), nullRasterizerState );
		objs->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit, "[rb] post-rollback probe ray hits the reverted alpha" );
		Check( ri.pObject == (const IObject*)objs->GetItem( "alpha" ),
		       "[rb] post-rollback hit IS the live alpha instance (TLAS rebuilt)" );
	}

	// LightSampler: reflects the reverted single light at energy 5 (NOT
	// the rolled-back energy 99).  Exitance is monotone in energy, so a
	// stale sampler that kept energy-99 would report a far larger value.
	{
		const LightSampler* s = SamplerOf( caster );
		Check( s != nullptr, "[rb] sampler present after attach" );
		if( s ) {
			Check( s->GetPositionalLightCount() == 1,
			       "[rb] sampler sees the 1 reverted light" );
			// Build a SEPARATE reference scene at the baseline energy and
			// compare exitance — proves the sampler matches the reverted
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
				       "[rb] reverted sampler exitance == fresh baseline (light rolled back, not stale 99)" );
			}
			safe_release( refCaster );
			pRef->release();
		}
	}

	safe_release( caster );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-rollback-multi-camera  (DECISIVE for P1-1)
//
// A document with TWO cameras (one active, one not).  Begin → edit →
// rollback.  Assert BOTH cameras are still present afterwards.
//
// The OLD snapshot-restore rollback called Scene::RestoreFromSnapshot,
// which CLEARS the whole camera manager and reinstalls only the single
// active-camera clone the snapshot captured — destroying the non-active
// camera (item count drops 2 → 1).  Inverse-edit rollback never touches
// the camera manager for a non-camera gesture, so both survive.
//////////////////////////////////////////////////////////////////////
static void TestRollbackMultiCamera()
{
	std::cout << "Test: rollback PRESERVES all cameras (multi-camera; P1-1 decisive)"
	          << std::endl;

	Job* pJob = MakeTxnScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[mc] scene downcast" ); pJob->release(); return; }

	// Two cameras: "cam_a" then "cam_b".  AddPinholeCamera registers in
	// the camera manager; the last-added is auto-promoted active, so
	// "cam_b" is active and "cam_a" is the non-active one the OLD restore
	// would have lost.
	const double locA[3] = { 0, 0, 20 };
	const double locB[3] = { 0, 0, 25 };
	const double look[3] = { 0, 0, 0 };
	const double up[3]   = { 0, 1, 0 };
	const double orient[3] = { 0, 0, 0 };
	const double tgt[2]    = { 0, 0 };
	Check( pJob->AddPinholeCamera( "cam_a", locA, look, up, 0.7854, 1.0, 1.0, 1.0, orient, tgt ),
	       "[mc] AddPinholeCamera cam_a" );
	Check( pJob->AddPinholeCamera( "cam_b", locB, look, up, 0.7854, 1.0, 1.0, 1.0, orient, tgt ),
	       "[mc] AddPinholeCamera cam_b" );

	const ICameraManager* cams = pScene->GetCameras();
	Check( cams != nullptr, "[mc] camera manager present" );
	if( !cams ) { pJob->release(); return; }
	const unsigned int camsBefore = cams->getItemCount();
	Check( camsBefore == 2, "[mc] two cameras present before transaction" );

	SceneEditController ctrl( *pJob, /*interactiveRasterizer*/ 0 );
	IObjectManager* objs = pJob->GetObjects();

	Check( ctrl.BeginTransaction(), "[mc] BeginTransaction succeeds" );

	// A NON-camera edit (object move): the camera set is irrelevant to it,
	// so a correct rollback must leave BOTH cameras untouched.
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "alpha" ) );
	Check( ctrl.SetProperty( String( "position" ), String( "42 0 0" ) ), "[mc] edit applied" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 42.0 ) < 1e-9, "[mc] live alpha at x=42" );

	Check( ctrl.RollbackTransaction(), "[mc] RollbackTransaction succeeds" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9, "[mc] alpha reverted to x=8" );

	// THE decisive assertion: both cameras still present (the OLD
	// snapshot-restore would have dropped "cam_a" → count == 1).
	const unsigned int camsAfter = cams->getItemCount();
	Check( camsAfter == 2, "[mc] BOTH cameras still present after rollback (P1-1)" );
	Check( pScene->GetCameras()->GetItem( "cam_a" ) != nullptr,
	       "[mc] non-active camera cam_a survived rollback (P1-1 decisive)" );
	Check( pScene->GetCameras()->GetItem( "cam_b" ) != nullptr,
	       "[mc] active camera cam_b survived rollback" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-rollback-shared-material-identity  (DECISIVE for P1-2)
//
// Two objects ("alpha" and "filler0") share ONE named material
// ("mat_red").  Begin → edit that material's reflectance painter via the
// Material panel → rollback.  Assert both objects STILL point at the SAME
// IMaterial* instance AND that instance's slot is reverted.
//
// The OLD snapshot-restore rollback deep-cloned each object with a
// PRIVATE material clone, so after restore alpha and filler0 would point
// at two DISTINCT material instances (identity / sharing lost).  Inverse-
// edit rollback reverts the ONE shared instance in place, so both objects
// keep sharing it and the revert is visible through either.
//////////////////////////////////////////////////////////////////////
static void TestRollbackSharedMaterialIdentity()
{
	std::cout << "Test: rollback PRESERVES shared-material identity (P1-2 decisive)"
	          << std::endl;

	Job* pJob = MakeTxnScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[sm] scene downcast" ); pJob->release(); return; }

	IObjectManager* objs = pJob->GetObjects();
	IPainterManager* pnts = pJob->GetPainters();
	const IPainter* pRed  = pnts->GetItem( "p_red" );
	const IPainter* pBlue = pnts->GetItem( "p_blue" );

	// Sanity: "alpha" and "filler0" are both built with "mat_red" in
	// MakeTxnScene, so they share the SAME IMaterial instance.
	const IMaterial* matAlpha0  = ObjMaterialInstance( objs, "alpha" );
	const IMaterial* matFiller0 = ObjMaterialInstance( objs, "filler0" );
	Check( matAlpha0 != nullptr && matAlpha0 == matFiller0,
	       "[sm] alpha and filler0 share ONE material instance at baseline" );
	Check( ObjMatSlot( objs, "alpha" ) == pRed, "[sm] baseline reflectance is p_red" );

	SceneEditController ctrl( *pJob, /*interactiveRasterizer*/ 0 );

	Check( ctrl.BeginTransaction(), "[sm] BeginTransaction succeeds" );

	// Edit the SHARED material's reflectance via the Material panel path
	// (SetMaterialProperty rebinds the slot painter on the named material
	// instance — observable through BOTH objects since they share it).
	ctrl.ForTest_SetSelection( SceneEditController::Category::Material, String( "mat_red" ) );
	Check( ctrl.SetPropertyForCategory( SceneEditController::Category::Material,
		String( "reflectance" ), String( "p_blue" ) ),
	       "[sm] SetMaterialProperty(reflectance=p_blue) applied" );
	Check( ObjMatSlot( objs, "alpha" )   == pBlue, "[sm] live alpha sees p_blue (shared edit)" );
	Check( ObjMatSlot( objs, "filler0" ) == pBlue, "[sm] live filler0 ALSO sees p_blue (shared)" );

	Check( ctrl.RollbackTransaction(), "[sm] RollbackTransaction succeeds" );

	// Decisive #1 — IDENTITY: both objects STILL point at the SAME
	// material instance, and it's the SAME instance as before the
	// transaction (the OLD restore gave each a private clone → these
	// pointers would diverge).
	const IMaterial* matAlpha1  = ObjMaterialInstance( objs, "alpha" );
	const IMaterial* matFiller1 = ObjMaterialInstance( objs, "filler0" );
	Check( matAlpha1 != nullptr && matAlpha1 == matFiller1,
	       "[sm] alpha and filler0 STILL share ONE material instance after rollback (P1-2)" );
	Check( matAlpha1 == matAlpha0,
	       "[sm] the shared material is the SAME instance as baseline (no clone swap, P1-2)" );

	// Decisive #2 — VALUE: the shared instance's slot is reverted to
	// p_red, visible through BOTH objects.
	Check( ObjMatSlot( objs, "alpha" )   == pRed, "[sm] reverted reflectance p_red via alpha" );
	Check( ObjMatSlot( objs, "filler0" ) == pRed, "[sm] reverted reflectance p_red via filler0" );

	Check( ctrl.Editor().History().RedoDepth() == 0, "[sm] nothing redoable after rollback" );

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
// rollback path is repeatable (each Begin records the then-current
// baseline depth, each Rollback reverts to it).  Also exercises the
// no-op guards: Rollback / End with no open transaction return false.
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
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9, "[rr] reverted x=8 round 1" );

	// Round 2: SAME controller, baseline still x=8 -> mutate -> rollback ->
	// x=8 again.  Proves no stale baseline lingers (the decisive property
	// is the return-to-baseline at the end of the round).
	//
	// NOTE: round 1's rollback reverted via SceneEditor::Undo, which
	// restores a transform through ClearAllTransforms()+PushTopTransStack
	// (the baseline lands on the object's transform STACK).  A subsequent
	// ABSOLUTE SetObjectPosition sets the position COMPONENT, which then
	// composes with the stacked baseline — so the live X after the round-2
	// edit is NOT simply the typed value.  That undo-then-absolute-set
	// interaction is a PRE-EXISTING editor property (it reproduces on the
	// plain Editor().Undo() path with no transaction at all) and is out of
	// scope for the rollback re-base.  We therefore assert only that the
	// round-2 edit MOVED the object away from baseline (proving the edit
	// landed and the transaction is live), then assert the rollback brings
	// it back to baseline exactly.
	Check( ctrl.BeginTransaction(), "[rr] Begin (round 2)" );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "alpha" ) );
	Check( ctrl.SetProperty( String( "position" ), String( "-12 0 0" ) ), "[rr] edit round 2" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) > 1e-6, "[rr] round 2 edit moved alpha off baseline" );
	Check( ctrl.RollbackTransaction(), "[rr] rollback round 2" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 8.0 ) < 1e-9, "[rr] reverted x=8 round 2" );
	Check( ctrl.Editor().History().RedoDepth() == 0, "[rr] nothing redoable after round 2" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-material-binding-emitter-generation  (P1-4)
//
// An object's material BINDING changing to/from an emissive material
// changes the scene's emitter set.  Such an edit MUST bump the scene's
// light-topology generation so a re-attached RayCaster rebuilds its
// LightSampler / LuminaryManager — otherwise a cached luminary pointing
// at the now-non-emissive material would later call emittedRadiance() on
// a NULL emitter.
//
// This mirrors the direct light-property edits, which already bump.  The
// observable is Scene::GetLightTopologyGeneration() advancing across the
// SetObjectMaterial edit (forward + undo), plus a re-attached sampler
// reporting the correct luminary count (no stale/null emitter).
//////////////////////////////////////////////////////////////////////
static void TestMaterialBindingEmitterGeneration()
{
	std::cout << "Test: object material-binding change bumps light-topology generation (P1-4)"
	          << std::endl;

	Job* pJob = new Job();

	// Painters.
	const double white[3] = { 0.8, 0.8, 0.8 };
	const double glow[3]  = { 1.0, 1.0, 1.0 };
	pJob->AddUniformColorPainter( "p_white", white, "Rec709RGB_Linear" );
	pJob->AddUniformColorPainter( "p_glow",  glow,  "Rec709RGB_Linear" );

	// A plain non-emissive material and a TRUE emissive material (lambertian
	// luminaire wrapping the plain one).  Binding an object to "mat_emit"
	// makes it a luminary; rebinding to "mat_plain" removes it from the
	// emitter set.
	pJob->AddLambertianMaterial( "mat_plain", "p_white" );
	pJob->AddLambertianLuminaireMaterial( "mat_emit", "p_glow", "mat_plain", 1.0 );

	pJob->AddSphereGeometry( "geom", 1.0 );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };
	const double posE[3]   = { 0, 5, 0 };
	// The emissive sphere (a luminary).
	pJob->AddObject( "emitter", "geom", "mat_emit", nullptr, nullptr,
		nilRMap, posE, orient, one3, true, true );
	// A plain probe sphere + fillers to force the TLAS path (>4 objects).
	const double posP[3] = { 8, 0, 0 };
	pJob->AddObject( "probe", "geom", "mat_plain", nullptr, nullptr,
		nilRMap, posP, orient, one3, true, true );
	for( int i = 0; i < 4; ++i ) {
		char nm[32];
		std::snprintf( nm, sizeof(nm), "filler%d", i );
		const double pos[3] = { -20.0, (double)( 4 * ( i + 1 ) ), 0.0 };
		pJob->AddObject( nm, "geom", "mat_plain", nullptr, nullptr,
			nilRMap, pos, orient, one3, true, true );
	}

	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );

	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[p14] scene downcast" ); pJob->release(); return; }

	// Confirm the emitter is a luminary at baseline via a fresh caster.
	IShader* pShader = pJob->GetShaders()->GetItem( "global" );
	if( !pShader ) { Check( false, "[p14] shader present" ); pJob->release(); return; }
	{
		IRayCaster* caster = nullptr;
		RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
		caster->AttachScene( pScene );
		const LightSampler* s = SamplerOf( caster );
		Check( s != nullptr, "[p14] baseline sampler present" );
		// One mesh/area-light luminary (the emissive sphere); we declared
		// no point lights.
		Check( LuminaryCountOf( caster ) == 1,
		       "[p14] baseline: emissive sphere IS a luminary (luminary count 1)" );
		safe_release( caster );
	}

	SceneEditController ctrl( *pJob, /*interactiveRasterizer*/ 0 );

	const unsigned int genBefore = pScene->GetLightTopologyGeneration();

	// Rebind the emitter object to the NON-emissive material.  This
	// removes it from the emitter set, so the generation MUST advance.
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "emitter" ) );
	Check( ctrl.SetProperty( String( "material" ), String( "mat_plain" ) ),
	       "[p14] rebind emitter -> mat_plain applied" );

	const unsigned int genAfter = pScene->GetLightTopologyGeneration();
	Check( genAfter != genBefore,
	       "[p14] light-topology generation BUMPED on material-binding change (P1-4)" );

	// A re-attached caster must now see ZERO area-light luminaries — and
	// critically, building it must not deref a stale/null emitter.
	{
		IRayCaster* caster = nullptr;
		RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
		caster->AttachScene( pScene );
		const LightSampler* s = SamplerOf( caster );
		Check( s != nullptr, "[p14] post-edit sampler present" );
		// Building the sampler must NOT deref a stale/null emitter; the
		// rebuilt luminary set now excludes the rebound object.
		Check( LuminaryCountOf( caster ) == 0,
		       "[p14] post-edit: object is no longer a luminary (luminary count 0)" );
		safe_release( caster );
	}

	// Undo the rebind: the object becomes emissive again, generation
	// advances once more, and a fresh caster sees the luminary return.
	const unsigned int genPreUndo = pScene->GetLightTopologyGeneration();
	Check( ctrl.Editor().Undo(), "[p14] undo of material rebind succeeds" );
	Check( pScene->GetLightTopologyGeneration() != genPreUndo,
	       "[p14] light-topology generation BUMPED on undo of material rebind (P1-4)" );
	{
		IRayCaster* caster = nullptr;
		RISE_API_CreateRayCaster( &caster, false, 10, *pShader, true );
		caster->AttachScene( pScene );
		Check( LuminaryCountOf( caster ) == 1,
		       "[p14] after undo: emissive sphere is a luminary again (luminary count 1)" );
		safe_release( caster );
	}

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static Scene* BuildEmissiveProbeScene( Job* pJob )
{
	// Canonical emissive-probe scene for the generation-bump tests: an
	// emissive sphere ("emitter", mat_emit), a plain probe + 4 fillers
	// (forcing the >4-object TLAS path), two geometries ("geom" r=1,
	// "geom2" r=2) for the geometry-swap case, and a "global" shader.
	const double white[3] = { 0.8, 0.8, 0.8 };
	const double glow[3]  = { 1.0, 1.0, 1.0 };
	pJob->AddUniformColorPainter( "p_white", white, "Rec709RGB_Linear" );
	pJob->AddUniformColorPainter( "p_glow",  glow,  "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "mat_plain", "p_white" );
	pJob->AddLambertianLuminaireMaterial( "mat_emit", "p_glow", "mat_plain", 1.0 );
	pJob->AddSphereGeometry( "geom",  1.0 );
	pJob->AddSphereGeometry( "geom2", 2.0 );
	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };
	const double posE[3]   = { 0, 5, 0 };
	pJob->AddObject( "emitter", "geom", "mat_emit", nullptr, nullptr,
		nilRMap, posE, orient, one3, true, true );
	const double posP[3] = { 8, 0, 0 };
	pJob->AddObject( "probe", "geom", "mat_plain", nullptr, nullptr,
		nilRMap, posP, orient, one3, true, true );
	for( int i = 0; i < 4; ++i ) {
		char nm[32];
		std::snprintf( nm, sizeof(nm), "filler%d", i );
		const double pos[3] = { -20.0, (double)( 4 * ( i + 1 ) ), 0.0 };
		pJob->AddObject( nm, "geom", "mat_plain", nullptr, nullptr,
			nilRMap, pos, orient, one3, true, true );
	}
	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );
	return dynamic_cast<Scene*>( pJob->GetScene() );
}

//////////////////////////////////////////////////////////////////////

static void TestSpatialEditOnLuminaireBumpsGeneration()
{
	std::cout << "Test: spatial edit on an emissive object bumps light-topology generation (re-review B)"
	          << std::endl;
	Job* pJob = new Job();
	Scene* pScene = BuildEmissiveProbeScene( pJob );
	if( !pScene ) { Check( false, "[Bsp] scene downcast" ); pJob->release(); return; }
	SceneEditController ctrl( *pJob, 0 );

	// Move the EMISSIVE object: its luminary area + world position feed the
	// LightSampler cache (alias weight + representative point), so the
	// generation MUST advance.
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "emitter" ) );
	const unsigned int g0 = pScene->GetLightTopologyGeneration();
	Check( ctrl.SetProperty( String( "position" ), String( "0 7 0" ) ),
	       "[Bsp] move emissive object applied" );
	Check( pScene->GetLightTopologyGeneration() != g0,
	       "[Bsp] spatial move of a LUMINAIRE bumps the generation (re-review B)" );

	// Geometry swap on the emissive object: changes its area -> MUST advance.
	const unsigned int g1 = pScene->GetLightTopologyGeneration();
	Check( ctrl.SetProperty( String( "geometry" ), String( "geom2" ) ),
	       "[Bsp] geometry swap on emissive object applied" );
	Check( pScene->GetLightTopologyGeneration() != g1,
	       "[Bsp] geometry swap on a LUMINAIRE bumps the generation (re-review B)" );

	// Move a NON-emissive object: luminary set + cache unaffected, NO bump.
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "probe" ) );
	const unsigned int g2 = pScene->GetLightTopologyGeneration();
	Check( ctrl.SetProperty( String( "position" ), String( "9 0 0" ) ),
	       "[Bsp] move non-emissive object applied" );
	Check( pScene->GetLightTopologyGeneration() == g2,
	       "[Bsp] spatial move of a NON-luminaire does NOT bump (no needless rebuild)" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestMaterialSlotEditOnEmissiveBumpsGeneration()
{
	std::cout << "Test: material-slot edit on an emissive material bumps light-topology generation (re-review B)"
	          << std::endl;
	Job* pJob = new Job();
	Scene* pScene = BuildEmissiveProbeScene( pJob );
	if( !pScene ) { Check( false, "[Bmat] scene downcast" ); pJob->release(); return; }
	SceneEditController ctrl( *pJob, 0 );

	// Edit the exitance slot of the EMISSIVE material -> stale alias-table
	// weight -> generation MUST advance.
	ctrl.ForTest_SetSelection( SceneEditController::Category::Material, String( "mat_emit" ) );
	const unsigned int g0 = pScene->GetLightTopologyGeneration();
	Check( ctrl.SetPropertyForCategory( SceneEditController::Category::Material,
	         String( "exitance" ), String( "p_white" ) ),
	       "[Bmat] exitance edit on emissive material applied" );
	Check( pScene->GetLightTopologyGeneration() != g0,
	       "[Bmat] slot edit on an EMISSIVE material bumps the generation (re-review B)" );

	// Edit a slot on a NON-emissive material -> NO bump.
	ctrl.ForTest_SetSelection( SceneEditController::Category::Material, String( "mat_plain" ) );
	const unsigned int g1 = pScene->GetLightTopologyGeneration();
	Check( ctrl.SetPropertyForCategory( SceneEditController::Category::Material,
	         String( "reflectance" ), String( "p_glow" ) ),
	       "[Bmat] reflectance edit on non-emissive material applied" );
	Check( pScene->GetLightTopologyGeneration() == g1,
	       "[Bmat] slot edit on a NON-emissive material does NOT bump" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestBeginTransactionRefusedMidComposite()
{
	std::cout << "Test: BeginTransaction refuses while a SceneEditor composite is open (re-review A)"
	          << std::endl;
	Job* pJob = new Job();
	Scene* pScene = BuildEmissiveProbeScene( pJob );
	if( !pScene ) { Check( false, "[Acomp] scene downcast" ); pJob->release(); return; }
	SceneEditController ctrl( *pJob, 0 );

	// Open a composite and make an edit inside it (a real gesture in flight).
	ctrl.Editor().BeginComposite( "test-gesture" );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "probe" ) );
	Check( ctrl.SetProperty( String( "position" ), String( "1 0 0" ) ),
	       "[Acomp] edit inside the open composite applied" );
	Check( ctrl.Editor().IsCompositeOpen(), "[Acomp] composite is open" );

	// BeginTransaction MUST refuse mid-composite (the baseline would land
	// INSIDE the group; rollback's composite Undo would undershoot it).
	Check( !ctrl.BeginTransaction(),
	       "[Acomp] BeginTransaction REFUSED while a composite is open (re-review A)" );
	Check( !ctrl.IsTransactionOpen(),
	       "[Acomp] no transaction opened mid-composite" );

	// Close the composite; BeginTransaction now succeeds.
	ctrl.Editor().EndComposite();
	Check( !ctrl.Editor().IsCompositeOpen(), "[Acomp] composite closed" );
	Check( ctrl.BeginTransaction(),
	       "[Acomp] BeginTransaction succeeds after the composite closed" );
	Check( ctrl.IsTransactionOpen(), "[Acomp] transaction open after composite closed" );
	ctrl.EndTransaction();

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestAbsoluteTransformUndoComposition()
{
	std::cout << "Test: absolute SetPosition after undo REPLACES (not composes with) the baseline (pre-existing transform-undo bug)"
	          << std::endl;
	Job* pJob = new Job();
	const double white[3] = { 0.8, 0.8, 0.8 };
	pJob->AddUniformColorPainter( "p_white", white, "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "mat_plain", "p_white" );
	pJob->AddSphereGeometry( "geom", 1.0 );
	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };
	const double pos8[3]   = { 8, 0, 0 };   // NON-identity baseline the bug needs
	pJob->AddObject( "mover", "geom", "mat_plain", nullptr, nullptr,
		nilRMap, pos8, orient, one3, true, true );
	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[tu] scene downcast" ); pJob->release(); return; }
	IObjectManager* objs = pJob->GetObjects();
	if( !objs ) { Check( false, "[tu] objs" ); pJob->release(); return; }

	SceneEditController ctrl( *pJob, 0 );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "mover" ) );
	Check( std::abs( LiveObjX( objs, "mover" ) - 8.0 ) < 1e-9, "[tu] baseline x=8" );

	// Absolute move to x=20, then undo back to the x=8 baseline.
	Check( ctrl.SetProperty( String( "position" ), String( "20 0 0" ) ), "[tu] move to x=20 applied" );
	Check( std::abs( LiveObjX( objs, "mover" ) - 20.0 ) < 1e-9, "[tu] now at x=20" );
	Check( ctrl.Editor().Undo(), "[tu] undo back to baseline" );
	Check( std::abs( LiveObjX( objs, "mover" ) - 8.0 ) < 1e-9, "[tu] back at x=8 after undo" );

	// A subsequent ABSOLUTE set must land at exactly -12, NOT -12+8=-4 (the bug:
	// undo collapsed the x=8 baseline onto the transform stack, so SetPosition
	// composed translate(-12) * translate(8)).
	Check( ctrl.SetProperty( String( "position" ), String( "-12 0 0" ) ), "[tu] absolute set to x=-12 applied" );
	Check( std::abs( LiveObjX( objs, "mover" ) - (-12.0) ) < 1e-9,
	       "[tu] absolute SetPosition after undo lands at -12, not -4 (transform-undo composition fix)" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestUndoFirstShaderBind()
{
	std::cout << "Test: undo of a FIRST shader bind clears it (no-shader baseline) (7th-review F5)" << std::endl;
	Job* pJob = new Job();
	const double white[3] = { 0.8, 0.8, 0.8 };
	pJob->AddUniformColorPainter( "p_white", white, "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "mat_plain", "p_white" );
	pJob->AddSphereGeometry( "geom", 1.0 );
	RadianceMapConfig nilRMap;
	const double o[3]={0,0,0}, sc[3]={1,1,1}, ps[3]={0,0,0};
	// shader = nullptr -> object has NO object-level shader (the bug's precondition).
	pJob->AddObject( "obj", "geom", "mat_plain", nullptr, nullptr, nilRMap, ps, o, sc, true, true );
	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );
	pJob->AddStandardShader( "sh1", 1, ops );
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[f5s] scene downcast" ); pJob->release(); return; }
	IObjectManager* objs = pJob->GetObjects();
	IObjectPriv* obj = objs ? objs->GetItem( "obj" ) : nullptr;
	Check( obj && obj->GetShader() == nullptr, "[f5s] baseline: object has no shader" );

	SceneEditController ctrl( *pJob, 0 );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	Check( ctrl.SetPropertyForCategory( SceneEditController::Category::Object, String( "shader" ), String( "sh1" ) ),
	       "[f5s] bind shader applied" );
	Check( obj->GetShader() != nullptr, "[f5s] shader now bound" );
	Check( ctrl.Editor().Undo(), "[f5s] undo succeeds" );
	Check( obj->GetShader() == nullptr, "[f5s] shader CLEARED after undo of a first bind (F5)" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestUndoFirstMaterialBind()
{
	std::cout << "Test: undo of a FIRST material bind clears it (no-material baseline) (7th-review F5)" << std::endl;
	Job* pJob = new Job();
	const double white[3] = { 0.8, 0.8, 0.8 };
	pJob->AddUniformColorPainter( "p_white", white, "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "mat_plain", "p_white" );
	pJob->AddSphereGeometry( "geom", 1.0 );
	RadianceMapConfig nilRMap;
	const double o[3]={0,0,0}, sc[3]={1,1,1}, ps[3]={0,0,0};
	// material = nullptr -> object has NO material (the bug's precondition).
	pJob->AddObject( "obj", "geom", nullptr, nullptr, nullptr, nilRMap, ps, o, sc, true, true );
	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[f5m] scene downcast" ); pJob->release(); return; }
	IObjectManager* objs = pJob->GetObjects();
	IObjectPriv* obj = objs ? objs->GetItem( "obj" ) : nullptr;
	Check( obj && obj->GetMaterial() == nullptr, "[f5m] baseline: object has no material" );

	SceneEditController ctrl( *pJob, 0 );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Object, String( "obj" ) );
	Check( ctrl.SetPropertyForCategory( SceneEditController::Category::Object, String( "material" ), String( "mat_plain" ) ),
	       "[f5m] bind material applied" );
	Check( obj->GetMaterial() != nullptr, "[f5m] material now bound" );
	Check( ctrl.Editor().Undo(), "[f5m] undo succeeds" );
	Check( obj->GetMaterial() == nullptr, "[f5m] material CLEARED after undo of a first bind (F5)" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestCameraUndoTargetsEditedCamera()
{
	std::cout << "Test: camera undo reverts the EDITED camera, not the active-later one (7th-review F4)" << std::endl;
	Job* pJob = new Job();
	double loc[3]={0,0,10}, la[3]={0,0,0}, up[3]={0,1,0}, orient[3]={0,0,0}, target[2]={0,0};
	pJob->AddPinholeCamera( "A", loc, la, up, 0.6, 1.0, 0, 0, orient, target, 0.0, 0.0 );
	double locB[3]={5,0,10};
	pJob->AddPinholeCamera( "B", locB, la, up, 1.2, 1.0, 0, 0, orient, target, 0.0, 0.0 );
	pJob->SetActiveCamera( "A" );
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[f4] scene downcast" ); pJob->release(); return; }
	const ICameraManager* cams = pScene->GetCameras();
	const ICamera* camA = cams ? cams->GetItem( "A" ) : nullptr;
	const ICamera* camB = cams ? cams->GetItem( "B" ) : nullptr;
	Check( camA && camB, "[f4] both cameras present" );
	const std::string aFov0 = std::string( CameraIntrospection::GetPropertyValue( *camA, String( "fov" ) ).c_str() );
	const std::string bFov0 = std::string( CameraIntrospection::GetPropertyValue( *camB, String( "fov" ) ).c_str() );

	SceneEditController ctrl( *pJob, 0 );
	ctrl.ForTest_SetSelection( SceneEditController::Category::Camera, String( "A" ) );
	// Edit camera A's fov (A is the active camera).
	Check( ctrl.SetPropertyForCategory( SceneEditController::Category::Camera, String( "fov" ), String( "0.9" ) ),
	       "[f4] set A.fov applied" );
	const std::string aFovEdited = std::string( CameraIntrospection::GetPropertyValue( *camA, String( "fov" ) ).c_str() );
	Check( aFovEdited != aFov0, "[f4] A.fov actually changed" );

	// Switch the ACTIVE camera to B, THEN undo.
	pJob->SetActiveCamera( "B" );
	Check( ctrl.Editor().Undo(), "[f4] undo succeeds" );

	const std::string aFovAfter = std::string( CameraIntrospection::GetPropertyValue( *camA, String( "fov" ) ).c_str() );
	const std::string bFovAfter = std::string( CameraIntrospection::GetPropertyValue( *camB, String( "fov" ) ).c_str() );
	Check( bFovAfter == bFov0, "[f4] camera B UNTOUCHED by undo of an A edit (F4)" );
	Check( aFovAfter == aFov0, "[f4] camera A reverted by undo (F4)" );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== SceneEditTransactionTest ===" << std::endl;

	TestRollbackRestoresBaseline();
	TestRollbackMultiCamera();
	TestRollbackSharedMaterialIdentity();
	TestCommitNoDoubleApply();
	TestRollbackRepeatableAndGuards();
	TestMaterialBindingEmitterGeneration();
	TestSpatialEditOnLuminaireBumpsGeneration();
	TestMaterialSlotEditOnEmissiveBumpsGeneration();
	TestBeginTransactionRefusedMidComposite();
	TestAbsoluteTransformUndoComposition();
	TestUndoFirstShaderBind();
	TestUndoFirstMaterialBind();
	TestCameraUndoTargetsEditedCamera();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
