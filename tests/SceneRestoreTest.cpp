//////////////////////////////////////////////////////////////////////
//
//  SceneRestoreTest.cpp (feature/gui-snapshot-prototype, increment 2a):
//    decisive proof that Scene::RestoreFromSnapshot() swaps a snapshot's
//    render-read state back INTO the live scene, leaves it render-valid,
//    and does NOT consume the snapshot (clone-on-restore, so the same
//    snapshot is reusable for repeated rollback / publish).
//
//  Each test is built to FAIL if restore were a no-op (live state would
//  still reflect the post-snapshot mutation) OR if restore consumed the
//  snapshot (the second restore in T-reusable would diverge / crash).
//
//    T-roundtrip   - mutate the live object transform + a material
//                    painter-slot + a light's energy; restore; assert all
//                    three snap back to the snapshot's values.
//    T-reusable    - restore the SAME snapshot twice (mutate-restore-
//                    mutate-restore); assert identical both times — proves
//                    clone-on-restore (the snapshot is not consumed).
//    T-publish-over- snapshot a working copy W; mutate the live scene to a
//                    DIFFERENT state; restore(W); assert live == W.
//    T-render-valid- after a restore, build the TLAS (PrepareForRendering)
//                    and fire an IntersectRay that should hit a restored
//                    object; assert the hit lands on the RESTORED object
//                    instance (proves the TLAS rebuilt over the swapped
//                    objects — no stale structure / dangling pointer) and
//                    that the live light count is consistent post-restore.
//
//  Construction path mirrors SceneSnapshotTest.cpp:
//    Job + AddSphereGeometry + AddObject + AddLambertianMaterial.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <chrono>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/ILightPriv.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/Utilities/RadianceMapConfig.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

//////////////////////////////////////////////////////////////////////
// Scene builders
//////////////////////////////////////////////////////////////////////

// A scene with N named spheres (forces the TLAS / BVH path when N > 4),
// a Lambertian material bound to "p_red", and a point light.  Returns the
// Job; caller releases.  `firstX` controls obj0 ("alpha")'s x so the
// render-valid test can aim a ray at it.
static Job* MakeRestoreScene()
{
	Job* pJob = new Job();

	// Two painters so the material slot can be rebound red -> blue.
	const double red[3]  = { 0.8, 0.1, 0.1 };
	const double blue[3] = { 0.1, 0.1, 0.8 };
	pJob->AddUniformColorPainter( "p_red",  red,  "Rec709RGB_Linear" );
	pJob->AddUniformColorPainter( "p_blue", blue, "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "mat", "p_red" );

	pJob->AddSphereGeometry( "geom", 1.0 );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };

	// obj "alpha" sits on the +X axis at x=8 so a ray from (20,0,0) toward
	// -X hits it.  Other filler objects sit far off the X axis so they
	// don't occlude that probe ray, but DO populate the BVH (need > 4
	// objects to leave the linear-loop fallback and exercise the TLAS).
	const double posAlpha[3] = { 8, 0, 0 };
	pJob->AddObject( "alpha", "geom", "mat", nullptr, nullptr,
		nilRMap, posAlpha, orient, one3, true, true );

	for( int i = 0; i < 5; ++i ) {
		char nm[32];
		std::snprintf( nm, sizeof(nm), "filler%d", i );
		const double pos[3] = { -20.0, (double)( 4 * ( i + 1 ) ), 0.0 };
		pJob->AddObject( nm, "geom", "mat", nullptr, nullptr,
			nilRMap, pos, orient, one3, true, true );
	}

	// A point light off to the side.
	const double lc[3] = { 1.0, 0.9, 0.8 };
	const double lp[3] = { 0, 10, 0 };
	pJob->AddPointOmniLight( "key", /*power*/ 5.0, lc, lp, /*shootPhotons*/ false );

	return pJob;
}

// Resolve the live object's final-transform x for the named object.
static double LiveObjX( IObjectManager* objs, const char* name )
{
	IObjectPriv* o = objs->GetItem( name );
	if( !o ) { return -1.0e30; }
	return (double)o->GetFinalTransformMatrix()._30;
}

// Resolve the live light's energy (the first / only light "key").
static double LiveLightEnergy( ILightManager* lights, const char* name )
{
	ILightPriv* l = lights->GetItem( name );
	if( !l ) { return -1.0e30; }
	return (double)l->emissionEnergy();
}

// Resolve the painter currently bound to the reflectance slot of the
// material ATTACHED TO the named live object.  RISE materials live on
// objects (Job::AddObject calls object->AssignMaterial); the renderer
// reads the material through the object, and Scene::RestoreFromSnapshot
// restores the per-object material clone — so the object's material is
// the render-faithful surface to assert against, NOT the material
// manager's naming registry (which restore intentionally does not
// touch).  Note: pre-restore, the live object and the manager's "mat"
// are the SAME instance (AssignMaterial shares it), so an editor
// SetSlot on the manager instance bleeds into the live object's render
// material — exactly the case restore must roll back.
static const IPainter* ObjMatSlot( IObjectManager* objs, const char* objName )
{
	IObjectPriv* o = objs->GetItem( objName );
	if( !o ) { return nullptr; }
	const IMaterial* m = o->GetMaterial();
	if( !m ) { return nullptr; }
	const MaterialSlotRef r = MaterialIntrospection::GetSlot( *m, String( "reflectance" ) );
	return ( r.kind == MaterialSlotRef::Painter ) ? r.painter : nullptr;
}

// Apply the three canonical live mutations: translate "alpha" by +dx,
// rebind the material reflectance to `toPainter`, set light "key" energy.
static void MutateLive(
	IObjectManager* objs, IMaterialManager* mats, ILightManager* lights,
	double dx, const IPainter* toPainter, const char* energyStr )
{
	IObjectPriv* alpha = objs->GetItem( "alpha" );
	if( alpha ) {
		alpha->TranslateObject( Vector3( dx, 0, 0 ) );
		alpha->FinalizeTransformations();
	}
	IMaterial* m = mats->GetItem( "mat" );
	if( m && toPainter ) {
		MaterialIntrospection::SetSlot( *m, String( "reflectance" ), toPainter, nullptr );
	}
	ILightPriv* l = lights->GetItem( "key" );
	if( l ) {
		IKeyframeParameter* p =
			l->KeyframeFromParameters( String( "energy" ), String( energyStr ) );
		if( p ) {
			l->SetIntermediateValue( *p );
			safe_release( p );
			l->RegenerateData();
		}
	}
}

//////////////////////////////////////////////////////////////////////
// T-roundtrip
//////////////////////////////////////////////////////////////////////

static void TestRestoreRoundtrip()
{
	std::cout << "Test: restore round-trips object xform + material slot + light energy"
	          << std::endl;

	Job* pJob = MakeRestoreScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	Check( pScene != nullptr, "[rt] scene downcasts to concrete Scene" );
	if( !pScene ) { pJob->release(); return; }

	IObjectManager*   objs   = pJob->GetObjects();
	IMaterialManager* mats   = pJob->GetMaterials();
	ILightManager*    lights = pJob->GetLights();
	IPainterManager*  pnts   = pJob->GetPainters();
	Check( objs && mats && lights && pnts, "[rt] managers resolvable" );
	if( !objs || !mats || !lights || !pnts ) { pJob->release(); return; }

	const IPainter* pRed  = pnts->GetItem( "p_red" );
	const IPainter* pBlue = pnts->GetItem( "p_blue" );

	// Snapshot-time state.
	const double    snapX      = LiveObjX( objs, "alpha" );       // 8
	const double    snapEnergy = LiveLightEnergy( lights, "key" );// 5
	const IPainter* snapSlot   = ObjMatSlot( objs, "alpha" );      // p_red
	Check( std::abs( snapX - 8.0 ) < 1e-9, "[rt] alpha starts at x=8" );
	Check( std::abs( snapEnergy - 5.0 ) < 1e-9, "[rt] light starts at energy 5" );
	Check( snapSlot == pRed, "[rt] material slot starts at p_red" );

	SceneSnapshot* snap = pScene->CreateSnapshot();
	Check( snap != nullptr, "[rt] snapshot non-null" );
	if( !snap ) { pJob->release(); return; }

	// Mutate the live scene AWAY from the snapshot.
	MutateLive( objs, mats, lights, /*dx*/ 10.0, /*to*/ pBlue, /*energy*/ "99.0" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 18.0 ) < 1e-9,
	       "[rt] live alpha moved to x=18 before restore" );
	Check( std::abs( LiveLightEnergy( lights, "key" ) - 99.0 ) < 1e-9,
	       "[rt] live light energy 99 before restore" );
	Check( ObjMatSlot( objs, "alpha" ) == pBlue,
	       "[rt] live material slot p_blue before restore" );

	// Restore.
	auto t0 = std::chrono::high_resolution_clock::now();
	pScene->RestoreFromSnapshot( *snap );
	auto t1 = std::chrono::high_resolution_clock::now();
	const double restoreUs =
		std::chrono::duration<double, std::micro>( t1 - t0 ).count();

	// IMPORTANT: restore REPLACES the manager contents with fresh clones,
	// so re-resolve the live entities by name from the (same) managers.
	const double    postX      = LiveObjX( objs, "alpha" );
	const double    postEnergy = LiveLightEnergy( lights, "key" );
	const IPainter* postSlot   = ObjMatSlot( objs, "alpha" );

	// ASSERT: every mutated field is back to the snapshot's value.  A
	// no-op restore would leave x=18 / energy=99 / p_blue and FAIL.
	Check( std::abs( postX - 8.0 ) < 1e-9,
	       "[rt] RESTORED alpha x back to 8 (was 18)" );
	Check( std::abs( postEnergy - 5.0 ) < 1e-9,
	       "[rt] RESTORED light energy back to 5 (was 99)" );
	Check( postSlot == pRed,
	       "[rt] RESTORED material slot back to p_red (was p_blue)" );

	// Object + light counts preserved across the swap.
	Check( objs->getItemCount() == 6, "[rt] 6 objects after restore" );
	Check( lights->getItemCount() == 1, "[rt] 1 light after restore" );

	std::cout << "  [cost] RestoreFromSnapshot() for 6-object scene: "
	          << restoreUs << " us" << std::endl;

	safe_release( snap );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-reusable: restore the SAME snapshot twice -> identical results.
//////////////////////////////////////////////////////////////////////

static void TestRestoreReusable()
{
	std::cout << "Test: same snapshot restores identically twice (not consumed)"
	          << std::endl;

	Job* pJob = MakeRestoreScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[reuse] scene downcast" ); pJob->release(); return; }

	IObjectManager*   objs   = pJob->GetObjects();
	IMaterialManager* mats   = pJob->GetMaterials();
	ILightManager*    lights = pJob->GetLights();
	IPainterManager*  pnts   = pJob->GetPainters();
	const IPainter* pRed  = pnts->GetItem( "p_red" );
	const IPainter* pBlue = pnts->GetItem( "p_blue" );

	SceneSnapshot* snap = pScene->CreateSnapshot();
	Check( snap != nullptr, "[reuse] snapshot non-null" );
	if( !snap ) { pJob->release(); return; }

	// --- Round 1: mutate, restore, capture result. ---
	MutateLive( objs, mats, lights, 10.0, pBlue, "99.0" );
	pScene->RestoreFromSnapshot( *snap );
	const double    x1      = LiveObjX( objs, "alpha" );
	const double    e1      = LiveLightEnergy( lights, "key" );
	const IPainter* s1      = ObjMatSlot( objs, "alpha" );
	const unsigned  oc1     = objs->getItemCount();
	const unsigned  lc1     = lights->getItemCount();

	// --- Round 2: mutate DIFFERENTLY, restore the SAME snapshot. ---
	MutateLive( objs, mats, lights, -3.0, pRed, "0.25" );
	pScene->RestoreFromSnapshot( *snap );
	const double    x2      = LiveObjX( objs, "alpha" );
	const double    e2      = LiveLightEnergy( lights, "key" );
	const IPainter* s2      = ObjMatSlot( objs, "alpha" );
	const unsigned  oc2     = objs->getItemCount();
	const unsigned  lc2     = lights->getItemCount();

	// ASSERT: both restores produced byte-identical render-read state.
	// If restore had consumed the snapshot, round 2 would install nothing
	// (counts collapse) or diverge.
	Check( std::abs( x1 - 8.0 ) < 1e-9 && std::abs( x2 - 8.0 ) < 1e-9 && std::abs( x1 - x2 ) < 1e-12,
	       "[reuse] alpha x identical (8) after both restores" );
	Check( std::abs( e1 - 5.0 ) < 1e-9 && std::abs( e2 - 5.0 ) < 1e-9 && std::abs( e1 - e2 ) < 1e-12,
	       "[reuse] light energy identical (5) after both restores" );
	Check( s1 == pRed && s2 == pRed && s1 == s2,
	       "[reuse] material slot identical (p_red) after both restores" );
	Check( oc1 == 6 && oc2 == 6 && lc1 == 1 && lc2 == 1,
	       "[reuse] object/light counts identical (6/1) after both restores" );

	safe_release( snap );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-publish-over: restore a working copy onto a divergent live scene.
//////////////////////////////////////////////////////////////////////

static void TestRestorePublishOver()
{
	std::cout << "Test: restore publishes a working copy over a divergent live scene"
	          << std::endl;

	Job* pJob = MakeRestoreScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[pub] scene downcast" ); pJob->release(); return; }

	IObjectManager*   objs   = pJob->GetObjects();
	IMaterialManager* mats   = pJob->GetMaterials();
	ILightManager*    lights = pJob->GetLights();
	IPainterManager*  pnts   = pJob->GetPainters();
	const IPainter* pRed  = pnts->GetItem( "p_red" );
	const IPainter* pBlue = pnts->GetItem( "p_blue" );

	// Establish a DISTINCTIVE "working copy" state first: move alpha to
	// x=12, slot p_blue, energy 42 — then snapshot THAT as W.
	MutateLive( objs, mats, lights, /*dx from 8*/ 4.0, pBlue, "42.0" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - 12.0 ) < 1e-9, "[pub] working copy alpha at x=12" );

	SceneSnapshot* W = pScene->CreateSnapshot();
	Check( W != nullptr, "[pub] working-copy snapshot W non-null" );
	if( !W ) { pJob->release(); return; }

	// Now drive the LIVE scene to a totally different state.
	MutateLive( objs, mats, lights, /*dx from 12*/ -20.0, pRed, "1.0" );
	Check( std::abs( LiveObjX( objs, "alpha" ) - ( -8.0 ) ) < 1e-9, "[pub] live diverged to x=-8" );
	Check( ObjMatSlot( objs, "alpha" ) == pRed, "[pub] live diverged to p_red" );
	Check( std::abs( LiveLightEnergy( lights, "key" ) - 1.0 ) < 1e-9, "[pub] live diverged to energy 1" );

	// Publish W over the live scene.
	pScene->RestoreFromSnapshot( *W );

	// ASSERT: live == W (the working copy), NOT the diverged state.
	Check( std::abs( LiveObjX( objs, "alpha" ) - 12.0 ) < 1e-9,
	       "[pub] PUBLISHED alpha x == W (12), not diverged (-8)" );
	Check( ObjMatSlot( objs, "alpha" ) == pBlue,
	       "[pub] PUBLISHED material slot == W (p_blue), not diverged (p_red)" );
	Check( std::abs( LiveLightEnergy( lights, "key" ) - 42.0 ) < 1e-9,
	       "[pub] PUBLISHED light energy == W (42), not diverged (1)" );

	safe_release( W );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-render-valid: after restore, the TLAS rebuilds over the swapped
// objects and a ray hits the RESTORED object (no stale structure).
//////////////////////////////////////////////////////////////////////

static void TestRestoreRenderValid()
{
	std::cout << "Test: scene is render-valid after restore (TLAS rebuilt, ray hits restored object)"
	          << std::endl;

	Job* pJob = MakeRestoreScene();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[rv] scene downcast" ); pJob->release(); return; }

	IObjectManager* objs   = pJob->GetObjects();
	ILightManager*  lights = pJob->GetLights();
	Check( objs->getItemCount() == 6, "[rv] 6 objects (forces BVH path, >4)" );

	// Force the BVH to build over the ORIGINAL contents (so a stale
	// structure with dangling element pointers exists pre-restore).  Then
	// confirm a probe ray hits the original "alpha" at x=8.
	objs->PrepareForRendering();
	{
		RayIntersection ri( Ray( Point3( 20, 0, 0 ), Vector3( -1, 0, 0 ) ), nullRasterizerState );
		objs->IntersectRay( ri, true, true, false );
		Check( ri.geometric.bHit, "[rv] pre-restore probe ray hits something" );
		Check( ri.pObject == (const IObject*)objs->GetItem( "alpha" ),
		       "[rv] pre-restore hit is the original alpha" );
	}

	// Snapshot, then mutate the live scene so the SNAPSHOT and live differ.
	// Move alpha OFF the y=0 probe line (the probe ray runs infinitely
	// along -X at y=0, so an X-only move would never take alpha off it —
	// lift it +50 in Y so the ray clears it).
	SceneSnapshot* snap = pScene->CreateSnapshot();
	{
		IObjectPriv* alpha = objs->GetItem( "alpha" );
		alpha->TranslateObject( Vector3( 0, 50, 0 ) );
		alpha->FinalizeTransformations();
	}
	// A bare transform edit does NOT auto-invalidate the TLAS (the
	// PrepareForRendering `!pBVH` guard would keep the stale structure);
	// the editor's invariant chain invalidates explicitly after a move.
	// Do the same so this mid-test "misses" check actually exercises the
	// mutated geometry — and so a genuinely stale BVH (built over the
	// soon-to-be-released mutated clones) exists for restore to clear.
	objs->InvalidateSpatialStructure();
	objs->PrepareForRendering();   // rebuild BVH over the mutated contents
	{
		// Now the ray should MISS (alpha lifted to y=50, off the y=0 probe).
		RayIntersection ri( Ray( Point3( 20, 0, 0 ), Vector3( -1, 0, 0 ) ), nullRasterizerState );
		objs->IntersectRay( ri, true, true, false );
		Check( !ri.geometric.bHit, "[rv] post-mutation probe ray misses (alpha moved away)" );
	}

	// RESTORE.  This must drop the stale BVH (built over the now-released
	// mutated clones) so the next PrepareForRendering builds a fresh one
	// over the RESTORED objects.
	pScene->RestoreFromSnapshot( *snap );
	safe_release( snap );   // drop the snapshot; restore must NOT depend on it surviving

	// Re-resolve the restored "alpha" instance (a fresh clone).
	IObjectPriv* restoredAlpha = objs->GetItem( "alpha" );
	Check( restoredAlpha != nullptr, "[rv] restored alpha resolvable by name" );
	Check( std::abs( (double)restoredAlpha->GetFinalTransformMatrix()._30 - 8.0 ) < 1e-9,
	       "[rv] restored alpha is back at x=8" );

	// Build the TLAS over the restored contents and fire the probe ray.
	objs->PrepareForRendering();
	{
		RayIntersection ri( Ray( Point3( 20, 0, 0 ), Vector3( -1, 0, 0 ) ), nullRasterizerState );
		objs->IntersectRay( ri, true, true, false );
		// DECISIVE: the ray hits, AND the hit object is the freshly-
		// restored alpha instance (proves the BVH was rebuilt over the
		// swapped objects — a stale BVH would point at the released
		// pre-restore object, failing identity or crashing).
		Check( ri.geometric.bHit, "[rv] post-restore probe ray hits the restored alpha" );
		Check( ri.pObject == (const IObject*)restoredAlpha,
		       "[rv] post-restore hit IS the restored alpha instance (TLAS rebuilt, no dangling ptr)" );
	}

	// Light-list consistency post-restore.  The LightSampler itself is
	// owned by the RayCaster (rebuilt in AttachScene, with a same-scene-
	// pointer early-return — see Scene::RestoreFromSnapshot's caveat), so
	// this test asserts the surface restore DOES guarantee: the live
	// light manager's list (the input AttachScene's LightSampler::Prepare
	// reads) is intact, AND the LightManager's cachedLights cache (used
	// by getLights()) is in sync — proving restore cleared + repopulated
	// through the virtual Add/RemoveItem overrides, not a cache-stale
	// Shutdown().  An empty / stale list here would mean an unlit scene.
	Check( lights->getItemCount() == 1, "[rv] 1 light after restore" );
	Check( lights->getLights().size() == 1,
	       "[rv] LightManager cachedLights consistent (1) — restore kept the cache in sync" );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

int main()
{
	std::cout << "=== SceneRestoreTest ===" << std::endl;

	TestRestoreRoundtrip();
	TestRestoreReusable();
	TestRestorePublishOver();
	TestRestoreRenderValid();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
