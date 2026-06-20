//////////////////////////////////////////////////////////////////////
//
//  SceneSnapshotTest.cpp (feature/gui-snapshot-prototype):
//    decisive proof that Scene::CreateSnapshot() takes a GENUINELY
//    IMMUTABLE snapshot of the scene's mutable wrapper state, such that
//    later in-place mutation of the LIVE scene does NOT bleed into the
//    snapshot.
//
//  It disproves the rejected "just addref the live instances" snapshot
//  design: addref preserves lifetime, not state, and the editor mutates
//  the very instances it would have addref'd.  The validated design
//  CLONES the mutable wrappers (object transform building-blocks +
//  finalized matrices + bindings; active-camera pose) AND clones the
//  mutable LEAVES the editor edits in place (material painter-slot
//  bindings; the shared shader runtime cache), addref-sharing only the
//  genuinely-immutable sub-leaves (geometry, painters, textures).
//
//  Adversarial coverage (each FAILS on the addref-only design):
//    T-transform / T-camera  - cloned transform + camera pose are
//                              independent of later live mutation.
//    T-material              - a snapshot's REAL material's painter slot
//                              is unchanged after the LIVE material is
//                              rebound the way the editor rebinds it
//                              (MaterialIntrospection::SetSlot).
//    T-csg                   - a CSGObject survives the snapshot AS a
//                              CSGObject (operands + operation intact),
//                              not sliced to a plain Object.
//    negative-control        - a bare addref'd live handle DOES observe
//                              the mutation (this is WHY clone is needed).
//
//  Construction path mirrors the existing SceneEditor tests:
//    Job + AddSphereGeometry + AddObject + RISE_API_CreatePinholeCamera.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdio>

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
#include "../src/Library/Interfaces/IMedium.h"
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Interfaces/IPhaseFunction.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Cameras/CameraCommon.h"
#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Objects/CSGObject.h"
#include "../src/Library/Materials/HomogeneousMedium.h"
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

// Build a Job with a pinhole camera, a "global" shader, and two spheres
// each with its own geometry + transform.  Same recipe the SceneEditor
// tests use.
static Job* MakeSceneWithTwoObjectsAndCamera()
{
	Job* pJob = new Job();

	// Active camera at (0,0,5) looking at origin.
	ICamera* pCam = nullptr;
	if( RISE_API_CreatePinholeCamera(
		&pCam,
		Point3( 0, 0, 5 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ), 64, 64,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ) )
	{
		pJob->GetScene()->AddCamera( "default", pCam );
		pCam->release();
	}

	const char* ops[] = { "DefaultDirectLighting" };
	pJob->AddStandardShader( "global", 1, ops );

	pJob->AddSphereGeometry( "geomA", 1.0 );
	pJob->AddSphereGeometry( "geomB", 2.0 );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };

	// obj0 ("alpha") starts at (1, 2, 3).
	const double posA[3] = { 1, 2, 3 };
	pJob->AddObject( "alpha", "geomA",
		/*material*/ nullptr, /*modifier*/ nullptr, /*shader*/ nullptr,
		nilRMap, posA, orient, one3, true, true );

	// obj1 ("beta") starts at (-4, 0, 0).
	const double posB[3] = { -4, 0, 0 };
	pJob->AddObject( "beta", "geomB",
		/*material*/ nullptr, /*modifier*/ nullptr, /*shader*/ nullptr,
		nilRMap, posB, orient, one3, true, true );

	return pJob;
}

// The objects are enumerated by the manager in lexicographic name order
// (std::map), so index 0 == "alpha", index 1 == "beta".  Find by name to
// be robust regardless of ordering.
static size_t IndexOfObject( const SceneSnapshot& snap, const char* name )
{
	for( size_t i = 0; i < snap.GetObjectCount(); ++i ) {
		if( strcmp( snap.GetObjectName( i ).c_str(), name ) == 0 ) {
			return i;
		}
	}
	return static_cast<size_t>( -1 );
}

//////////////////////////////////////////////////////////////////////
// THE DECISIVE TEST
//////////////////////////////////////////////////////////////////////

static void TestSnapshotIsIndependentOfLiveMutation()
{
	std::cout << "Test: snapshot is independent of later live mutation"
	          << std::endl;

	Job* pJob = MakeSceneWithTwoObjectsAndCamera();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	Check( pScene != nullptr, "scene downcasts to concrete Scene" );
	if( !pScene ) { pJob->release(); return; }

	IObjectManager* objs = pJob->GetObjects();
	Check( objs != nullptr && objs->getItemCount() == 2,
	       "two objects registered" );

	// --- (2) Take the snapshot, timing it. ---
	auto t0 = std::chrono::high_resolution_clock::now();
	SceneSnapshot* snap = pScene->CreateSnapshot();
	auto t1 = std::chrono::high_resolution_clock::now();
	const double snapUs =
		std::chrono::duration<double, std::micro>( t1 - t0 ).count();

	Check( snap != nullptr, "CreateSnapshot returns non-null" );
	if( !snap ) { pJob->release(); return; }

	Check( snap->GetObjectCount() == 2, "snapshot captured 2 objects" );
	Check( snap->HasCamera(),           "snapshot captured a camera" );

	const size_t iAlpha = IndexOfObject( *snap, "alpha" );
	Check( iAlpha != static_cast<size_t>( -1 ), "snapshot has object 'alpha'" );

	// --- (3) Read PRE-mutation values out of the snapshot. ---
	const Matrix4 snapAlpha = snap->GetObjectFinalTransform( iAlpha );
	const Point3  snapCamPos = snap->GetCameraPosition();

	// Sanity: snapshot reflects the authored pose at snap time.
	Check( std::abs( (double)snapAlpha._30 - 1.0 ) < 1e-9
	    && std::abs( (double)snapAlpha._31 - 2.0 ) < 1e-9
	    && std::abs( (double)snapAlpha._32 - 3.0 ) < 1e-9,
	    "snapshot alpha translation == authored (1,2,3)" );
	Check( std::abs( (double)snapCamPos.x - 0.0 ) < 1e-9
	    && std::abs( (double)snapCamPos.z - 5.0 ) < 1e-9,
	    "snapshot camera position == authored (0,0,5)" );

	// --- Negative control: a bare addref'd handle to the SAME live
	//     object (what the rejected design would have stored). ---
	IObjectPriv* liveAlpha = objs->GetItem( "alpha" );
	Check( liveAlpha != nullptr, "live alpha resolvable" );
	liveAlpha->addref();   // simulate "snapshot just addref'd the instance"
	const Matrix4 addrefAlphaBefore = liveAlpha->GetFinalTransformMatrix();
	Check( std::abs( (double)addrefAlphaBefore._30 - 1.0 ) < 1e-9,
	       "addref'd handle sees (1,..) before mutation" );

	// --- (4) Mutate the LIVE scene AFTER the snapshot. ---
	// Translate alpha by (10,0,0) and finalize (mirrors what
	// SceneEditor's TranslateObject + RunObjectInvariantChain do).
	liveAlpha->TranslateObject( Vector3( 10, 0, 0 ) );
	liveAlpha->FinalizeTransformations();

	// Move the camera (mirrors editor camera edits via GetCameraMutable +
	// the CameraCommon setters).
	ICamera* liveCam = pScene->GetCameraMutable();
	Check( liveCam != nullptr, "live camera resolvable" );
	CameraCommon* liveCamCC = dynamic_cast<CameraCommon*>( liveCam );
	Check( liveCamCC != nullptr, "live camera downcasts to CameraCommon" );
	if( liveCamCC ) {
		liveCamCC->SetLocation( Point3( 99, 88, 77 ) );
		liveCamCC->RegenerateData();
	}

	// --- Confirm the live object/camera actually moved. ---
	const Matrix4 liveAlphaAfter = liveAlpha->GetFinalTransformMatrix();
	Check( std::abs( (double)liveAlphaAfter._30 - 11.0 ) < 1e-9,
	       "LIVE alpha tx now 11 (1+10) after mutation" );

	// --- (5) ASSERT: the snapshot is UNCHANGED and now DIFFERS from
	//     the live values.  Under the rejected addref design these
	//     would FAIL. ---
	const Matrix4 snapAlphaAfter = snap->GetObjectFinalTransform( iAlpha );
	Check( std::abs( (double)snapAlphaAfter._30 - 1.0 ) < 1e-9,
	       "SNAPSHOT alpha tx STILL 1 after live mutation (immutable)" );
	Check( std::abs( (double)snapAlphaAfter._30
	               - (double)liveAlphaAfter._30 ) > 1.0,
	       "SNAPSHOT alpha tx DIFFERS from live (1 vs 11)" );

	const Point3 snapCamPosAfter = snap->GetCameraPosition();
	Check( std::abs( (double)snapCamPosAfter.x - 0.0 ) < 1e-9
	    && std::abs( (double)snapCamPosAfter.z - 5.0 ) < 1e-9,
	    "SNAPSHOT camera position STILL (0,0,5) after live mutation" );
	if( liveCamCC ) {
		const Point3 liveCamPos = liveCamCC->GetRestLocation();
		Check( std::abs( (double)snapCamPosAfter.x
		               - (double)liveCamPos.x ) > 1.0,
		       "SNAPSHOT camera position DIFFERS from live (0 vs 99)" );
	}

	// --- (6) Negative control payoff: the bare addref'd handle DID
	//     observe the live mutation (this is WHY clone is required). ---
	const Matrix4 addrefAlphaAfter = liveAlpha->GetFinalTransformMatrix();
	Check( std::abs( (double)addrefAlphaAfter._30 - 11.0 ) < 1e-9,
	       "NEGATIVE CONTROL: addref'd handle sees 11 (mutation bled through)" );
	liveAlpha->release();   // drop the simulated addref

	// --- (7) Report the measured snapshot cost. ---
	std::cout << "  [cost] CreateSnapshot() for 2-object scene: "
	          << snapUs << " us" << std::endl;

	safe_release( snap );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-material: a snapshot's material's painter slot is INDEPENDENT of a
// later in-place rebind on the LIVE material.
//
// The editor rebinds a material's painter slot in place via
// MaterialIntrospection::SetSlot (the exact path SceneEditor::
// SetMaterialProperty uses).  Under the rejected "addref the material"
// snapshot design the snapshot would share the very LambertianMaterial
// instance the editor mutates, so the rebind would bleed into the
// snapshot.  This test pins that it does NOT.
//////////////////////////////////////////////////////////////////////

static void TestSnapshotMaterialSlotIsImmutable()
{
	std::cout << "Test: snapshot material painter-slot is immutable vs live rebind"
	          << std::endl;

	Job* pJob = new Job();

	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	Check( pScene != nullptr, "[mat] scene downcasts to concrete Scene" );
	if( !pScene ) { pJob->release(); return; }

	// Two registered painters so we can rebind from one to the other.
	const double red[3]  = { 0.8, 0.1, 0.1 };
	const double blue[3] = { 0.1, 0.1, 0.8 };
	pJob->AddUniformColorPainter( "p_red",  red,  "Rec709RGB_Linear" );
	pJob->AddUniformColorPainter( "p_blue", blue, "Rec709RGB_Linear" );

	// A REAL Lambertian material with a non-null painter slot, bound to
	// "p_red" at snapshot time.
	pJob->AddLambertianMaterial( "mat", "p_red" );

	pJob->AddSphereGeometry( "geom", 1.0 );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };
	const double pos[3]    = { 0, 0, 0 };
	pJob->AddObject( "obj", "geom",
		/*material*/ "mat", /*modifier*/ nullptr, /*shader*/ nullptr,
		nilRMap, pos, orient, one3, true, true );

	// Resolve the live material + the two painters (by pointer identity).
	IMaterialManager* mats = pJob->GetMaterials();
	IPainterManager*  pnts = pJob->GetPainters();
	Check( mats != nullptr && pnts != nullptr, "[mat] managers resolvable" );
	if( !mats || !pnts ) { pJob->release(); return; }

	IMaterial*      liveMat = mats->GetItem( "mat" );
	const IPainter* pRed    = pnts->GetItem( "p_red" );
	const IPainter* pBlue   = pnts->GetItem( "p_blue" );
	Check( liveMat && pRed && pBlue, "[mat] live material + both painters resolvable" );
	if( !liveMat || !pRed || !pBlue ) { pJob->release(); return; }

	// Sanity: the slot starts bound to p_red.
	const MaterialSlotRef pre = MaterialIntrospection::GetSlot( *liveMat, String( "reflectance" ) );
	Check( pre.kind == MaterialSlotRef::Painter && pre.painter == pRed,
	       "[mat] live slot starts bound to p_red" );

	// --- Take the snapshot. ---
	SceneSnapshot* snap = pScene->CreateSnapshot();
	Check( snap != nullptr && snap->GetObjectCount() == 1, "[mat] snapshot captured the object" );
	if( !snap ) { pJob->release(); return; }

	// The snapshot's object MUST carry a material (non-null) — proves the
	// clone preserved the binding rather than dropping it.
	const IMaterial* snapMat = snap->GetObjectMaterial( 0 );
	Check( snapMat != nullptr, "[mat] snapshot object has a material" );

	// Read the snapshot's material slot BEFORE the live rebind.
	if( snapMat ) {
		const MaterialSlotRef snapPre =
			MaterialIntrospection::GetSlot( *snapMat, String( "reflectance" ) );
		Check( snapPre.kind == MaterialSlotRef::Painter && snapPre.painter == pRed,
		       "[mat] snapshot slot reads p_red at snapshot time" );
	}

	// --- Mutate the LIVE material the way the editor does: rebind the
	//     reflectance slot from p_red to p_blue. ---
	const bool rebound =
		MaterialIntrospection::SetSlot( *liveMat, String( "reflectance" ), pBlue, nullptr );
	Check( rebound, "[mat] live SetSlot(reflectance -> p_blue) succeeded" );

	// Confirm the LIVE material actually changed.
	const MaterialSlotRef livePost =
		MaterialIntrospection::GetSlot( *liveMat, String( "reflectance" ) );
	Check( livePost.kind == MaterialSlotRef::Painter && livePost.painter == pBlue,
	       "[mat] LIVE slot now bound to p_blue after rebind" );

	// --- ASSERT: the snapshot's material slot is UNCHANGED (still p_red).
	//     Under the addref-only design snapMat == liveMat, so this reads
	//     p_blue and FAILS. ---
	if( snapMat ) {
		const MaterialSlotRef snapPost =
			MaterialIntrospection::GetSlot( *snapMat, String( "reflectance" ) );
		Check( snapPost.kind == MaterialSlotRef::Painter && snapPost.painter == pRed,
		       "[mat] SNAPSHOT slot STILL p_red after live rebind (immutable)" );
		Check( snapPost.painter != livePost.painter,
		       "[mat] SNAPSHOT slot DIFFERS from live (p_red vs p_blue)" );
		// The snapshot must also own a DIFFERENT material instance than the
		// live one — sharing the instance is the root defect.
		Check( snapMat != liveMat,
		       "[mat] snapshot material is a distinct instance from live" );
	}

	safe_release( snap );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-csg: a CSGObject survives CreateSnapshot AS a CSGObject.
//
// Object::CloneSnapshot() was non-virtual and Scene::CreateSnapshot
// dynamic_cast'd to the base Object, so a CSGObject was sliced to a
// plain Object (operands + operation lost, null geometry behaviour).
// This test pins that the cloned entity is still a CSGObject with
// intact operands/operation and a sane (non-degenerate) bounding box.
//////////////////////////////////////////////////////////////////////

static void TestSnapshotPreservesCSGType()
{
	std::cout << "Test: snapshot preserves CSGObject polymorphic type"
	          << std::endl;

	Job* pJob = new Job();

	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	Check( pScene != nullptr, "[csg] scene downcasts to concrete Scene" );
	if( !pScene ) { pJob->release(); return; }

	// Two operand spheres + a CSG union of them.
	pJob->AddSphereGeometry( "sgeomA", 1.0 );
	pJob->AddSphereGeometry( "sgeomB", 1.0 );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };
	const double posA[3]   = { -0.5, 0, 0 };
	const double posB[3]   = {  0.5, 0, 0 };

	pJob->AddObject( "operA", "sgeomA",
		nullptr, nullptr, nullptr, nilRMap, posA, orient, one3, true, true );
	pJob->AddObject( "operB", "sgeomB",
		nullptr, nullptr, nullptr, nilRMap, posB, orient, one3, true, true );

	// op 0 == Union.  Place the CSG object itself off-origin so a sane
	// transform is captured.
	const double csgPos[3] = { 0, 3, 0 };
	const bool csgMade = pJob->AddCSGObject(
		"csg", "operA", "operB", /*op*/ 0,
		/*material*/ nullptr, /*modifier*/ nullptr, /*shader*/ nullptr,
		nilRMap, csgPos, orient, true, true );
	Check( csgMade, "[csg] AddCSGObject succeeded" );

	IObjectManager* objs = pJob->GetObjects();
	Check( objs != nullptr, "[csg] object manager resolvable" );
	if( !objs ) { pJob->release(); return; }

	// Sanity: the LIVE "csg" entity is a CSGObject.
	IObjectPriv* liveCsg = objs->GetItem( "csg" );
	Check( liveCsg != nullptr, "[csg] live csg resolvable" );
	Check( dynamic_cast<CSGObject*>( liveCsg ) != nullptr,
	       "[csg] live csg IS a CSGObject" );

	// --- Take the snapshot. ---
	SceneSnapshot* snap = pScene->CreateSnapshot();
	Check( snap != nullptr, "[csg] snapshot non-null" );
	if( !snap ) { pJob->release(); return; }

	// Find the snapshot's clone of "csg" by name.
	size_t iCsg = static_cast<size_t>( -1 );
	for( size_t i = 0; i < snap->GetObjectCount(); ++i ) {
		if( strcmp( snap->GetObjectName( i ).c_str(), "csg" ) == 0 ) { iCsg = i; break; }
	}
	Check( iCsg != static_cast<size_t>( -1 ), "[csg] snapshot has entity 'csg'" );

	const Object* clonedCsg = ( iCsg != static_cast<size_t>( -1 ) )
		? snap->GetClonedObject( iCsg ) : 0;
	Check( clonedCsg != nullptr, "[csg] cloned csg object retrievable" );

	// --- ASSERT: the clone is STILL a CSGObject (not sliced to Object). ---
	const CSGObject* clonedAsCsg = dynamic_cast<const CSGObject*>( clonedCsg );
	Check( clonedAsCsg != nullptr,
	       "[csg] SNAPSHOT clone IS a CSGObject (NOT sliced to plain Object)" );

	// --- ASSERT: operands + operation intact => non-degenerate bbox.
	//     A sliced plain Object has null geometry; CSGObject::getBoundingBox
	//     returns an empty (zero-volume) box when operands are missing.  A
	//     correct union of two unit spheres has a real extent. ---
	if( clonedAsCsg ) {
		const BoundingBox bbox = clonedAsCsg->getBoundingBox();
		const double dx = (double)bbox.ur.x - (double)bbox.ll.x;
		const double dy = (double)bbox.ur.y - (double)bbox.ll.y;
		const double dz = (double)bbox.ur.z - (double)bbox.ll.z;
		Check( dx > 1.0 && dy > 1.0 && dz > 1.0,
		       "[csg] cloned CSG bbox is non-degenerate (operands intact)" );
		// The captured transform placed the CSG at y=3, so the bbox should
		// be centered around y=3, confirming the transform came across too.
		const double cy = 0.5 * ( (double)bbox.ll.y + (double)bbox.ur.y );
		Check( std::abs( cy - 3.0 ) < 0.5,
		       "[csg] cloned CSG bbox centered at the captured transform (y~3)" );
	}

	safe_release( snap );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-light: a snapshot's light is INDEPENDENT of a later in-place edit
// on the LIVE light.
//
// The editor edits a light's properties in place via SceneEditor::
// SetLightProperty, which resolves the live ILightPriv from the light
// manager and calls light->KeyframeFromParameters(name,value) ->
// light->SetIntermediateValue(*p) -> light->RegenerateData() (see
// SceneEditor.cpp SetLightProperty).  Under the rejected "addref the
// live light" snapshot design the snapshot would share that very
// instance, so the edit would bleed into the snapshot.  This test pins
// that it does NOT — the snapshot owns an independent clone.
//////////////////////////////////////////////////////////////////////

static void TestSnapshotLightIsImmutable()
{
	std::cout << "Test: snapshot light is immutable vs live in-place edit"
	          << std::endl;

	Job* pJob = new Job();

	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	Check( pScene != nullptr, "[light] scene downcasts to concrete Scene" );
	if( !pScene ) { pJob->release(); return; }

	// A point omni light with a distinctive energy + colour, positioned
	// off-origin so the transform-derived position must come across too.
	const double srgb[3] = { 0.20, 0.40, 0.80 };
	const double pos[3]  = { 3, 4, 5 };
	pJob->AddPointOmniLight( "key", /*power*/ 7.0, srgb, pos, /*shootPhotons*/ false );

	ILightManager* lights = const_cast<ILightManager*>( pScene->GetLights() );
	Check( lights != nullptr && lights->getItemCount() == 1,
	       "[light] one light registered" );
	if( !lights ) { pJob->release(); return; }

	ILightPriv* liveLight = lights->GetItem( "key" );
	Check( liveLight != nullptr, "[light] live light resolvable" );
	if( !liveLight ) { pJob->release(); return; }

	// Snapshot-time values read off the LIVE light.
	const Scalar  preEnergy = liveLight->emissionEnergy();
	const RISEPel preColor  = liveLight->emissionColor();
	const Point3  prePos    = liveLight->position();
	Check( std::abs( (double)preEnergy - 7.0 ) < 1e-9,
	       "[light] live energy == authored 7" );
	Check( std::abs( (double)prePos.x - 3.0 ) < 1e-9
	    && std::abs( (double)prePos.y - 4.0 ) < 1e-9
	    && std::abs( (double)prePos.z - 5.0 ) < 1e-9,
	    "[light] live position == authored (3,4,5)" );

	// --- Take the snapshot. ---
	SceneSnapshot* snap = pScene->CreateSnapshot();
	Check( snap != nullptr, "[light] snapshot non-null" );
	if( !snap ) { pJob->release(); return; }

	Check( snap->GetLightCount() == 1, "[light] snapshot captured 1 light" );
	const ILight* snapLight = snap->GetLight( 0 );
	Check( snapLight != nullptr, "[light] snapshot exposes the light" );
	Check( snapLight != static_cast<const ILight*>( liveLight ),
	       "[light] snapshot light is a DISTINCT instance from live" );

	// The snapshot reproduces the authored emission + position (render-
	// faithful), proving the clone copied the transform-derived position
	// and not just the constructor params.
	if( snapLight ) {
		Check( std::abs( (double)snapLight->emissionEnergy() - 7.0 ) < 1e-9,
		       "[light] snapshot energy reproduces authored 7" );
		const Point3 snapPos = snapLight->position();
		Check( std::abs( (double)snapPos.x - 3.0 ) < 1e-9
		    && std::abs( (double)snapPos.y - 4.0 ) < 1e-9
		    && std::abs( (double)snapPos.z - 5.0 ) < 1e-9,
		    "[light] snapshot position reproduces authored (3,4,5)" );
		const RISEPel snapColor = snapLight->emissionColor();
		Check( std::abs( (double)snapColor[0] - (double)preColor[0] ) < 1e-9
		    && std::abs( (double)snapColor[1] - (double)preColor[1] ) < 1e-9
		    && std::abs( (double)snapColor[2] - (double)preColor[2] ) < 1e-9,
		    "[light] snapshot colour reproduces authored colour" );
	}

	// --- Mutate the LIVE light the way the editor does (SetLightProperty
	//     -> KeyframeFromParameters / SetIntermediateValue). ---
	IKeyframeParameter* pEnergy =
		liveLight->KeyframeFromParameters( String( "energy" ), String( "99.0" ) );
	Check( pEnergy != nullptr, "[light] live KeyframeFromParameters(energy) ok" );
	if( pEnergy ) {
		liveLight->SetIntermediateValue( *pEnergy );
		safe_release( pEnergy );
		liveLight->RegenerateData();
	}

	// Confirm the LIVE light actually changed.
	Check( std::abs( (double)liveLight->emissionEnergy() - 99.0 ) < 1e-9,
	       "[light] LIVE energy now 99 after edit" );

	// --- ASSERT: the snapshot's light is UNCHANGED.  Under the addref-only
	//     design snapLight == liveLight, so this reads 99 and FAILS. ---
	if( snapLight ) {
		Check( std::abs( (double)snapLight->emissionEnergy() - 7.0 ) < 1e-9,
		       "[light] SNAPSHOT energy STILL 7 after live edit (immutable)" );
		Check( std::abs( (double)snapLight->emissionEnergy()
		               - (double)liveLight->emissionEnergy() ) > 1.0,
		       "[light] SNAPSHOT energy DIFFERS from live (7 vs 99)" );
	}

	safe_release( snap );

	// --- Spot-light coverage: the clone must reproduce the cone AND the
	//     transform-derived direction (vDirection is re-derived from target
	//     + the restored position in FinalizeTransformations), not just the
	//     emission scalars. ---
	const double spotSrgb[3] = { 0.9, 0.8, 0.7 };
	const double spotFoc[3]  = { 0, 0, 0 };
	const double spotPos[3]  = { 0, 10, 0 };
	pJob->AddPointSpotLight( "spot", /*power*/ 5.0, spotSrgb, spotFoc,
		/*inner*/ 0.2, /*outer*/ 0.5, spotPos, /*shootPhotons*/ false );

	ILightPriv* liveSpot = lights->GetItem( "spot" );
	Check( liveSpot != nullptr, "[light] live spot resolvable" );
	SceneSnapshot* snap2 = pScene->CreateSnapshot();
	Check( snap2 != nullptr, "[light] second snapshot non-null" );
	if( snap2 && liveSpot ) {
		// Find the spot clone by matching position (names aren't captured
		// per-light in this increment; position disambiguates).
		const ILight* snapSpot = 0;
		for( size_t i = 0; i < snap2->GetLightCount(); ++i ) {
			const ILight* l = snap2->GetLight( i );
			if( l && std::abs( (double)l->position().y - 10.0 ) < 1e-9 ) { snapSpot = l; break; }
		}
		Check( snapSpot != nullptr, "[light] snapshot contains the spot clone" );
		if( snapSpot ) {
			const Vector3 ld = liveSpot->emissionDirection();
			const Vector3 sd = snapSpot->emissionDirection();
			Check( std::abs( (double)sd.x - (double)ld.x ) < 1e-9
			    && std::abs( (double)sd.y - (double)ld.y ) < 1e-9
			    && std::abs( (double)sd.z - (double)ld.z ) < 1e-9,
			    "[light] spot clone reproduces transform-derived direction" );
			Check( std::abs( (double)snapSpot->emissionInnerAngle() - 0.2 ) < 1e-9
			    && std::abs( (double)snapSpot->emissionOuterAngle() - 0.5 ) < 1e-9,
			    "[light] spot clone reproduces inner/outer cone angles" );
			Check( std::abs( (double)snapSpot->position().y - 10.0 ) < 1e-9,
			    "[light] spot clone reproduces transform-derived position" );
		}
	}
	safe_release( snap2 );

	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-camera-faithful: the snapshot reproduces the active camera's full
// per-type parameters, not just its pose.
//
// The spike captured only a CameraPoseSnapshot (position / lookAt / up /
// orientation), which loses the thin-lens photographic parameters
// (fstop / focal length / sensor size / focus distance / ...).  A render
// through such a snapshot would have the wrong depth of field.  This test
// builds a thinlens_camera with distinctive params and asserts the
// snapshot's cloned camera reproduces them.
//////////////////////////////////////////////////////////////////////

static void TestSnapshotCameraIsRenderFaithful()
{
	std::cout << "Test: snapshot camera reproduces full thin-lens params"
	          << std::endl;

	Job* pJob = new Job();

	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	Check( pScene != nullptr, "[cam] scene downcasts to concrete Scene" );
	if( !pScene ) { pJob->release(); return; }

	// Distinctive thin-lens parameters.
	const Scalar kSensor = Scalar( 36.0 );   // mm
	const Scalar kFocal  = Scalar( 85.0 );   // mm
	const Scalar kFstop  = Scalar( 1.8 );
	const Scalar kFocus  = Scalar( 12.5 );   // scene units

	ICamera* pCam = nullptr;
	const bool made = RISE_API_CreateThinlensCamera(
		&pCam,
		Point3( 1, 2, 9 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		kSensor, kFocal, kFstop, kFocus, /*sceneUnitMeters*/ Scalar( 1 ),
		128, 96,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
		/*apertureBlades*/ 6, /*apertureRotation*/ Scalar( 0.3 ),
		/*anamorphicSqueeze*/ Scalar( 1.0 ),
		/*tiltX*/ Scalar( 0 ), /*tiltY*/ Scalar( 0 ),
		/*shiftX*/ Scalar( 0 ), /*shiftY*/ Scalar( 0 ) );
	Check( made && pCam != nullptr, "[cam] thinlens camera created" );
	if( !pCam ) { pJob->release(); return; }
	pScene->AddCamera( "default", pCam );
	pCam->release();

	// --- Take the snapshot. ---
	SceneSnapshot* snap = pScene->CreateSnapshot();
	Check( snap != nullptr && snap->HasCamera(), "[cam] snapshot has a camera" );
	if( !snap ) { pJob->release(); return; }

	// Render-faithful proof: the snapshot exposes a cloned ICamera whose
	// concrete type AND photographic params match the live camera.
	const ICamera* snapCam = snap->GetClonedCamera();
	Check( snapCam != nullptr, "[cam] snapshot exposes a cloned camera" );

	const Implementation::ThinLensCamera* tl =
		dynamic_cast<const Implementation::ThinLensCamera*>( snapCam );
	Check( tl != nullptr,
	       "[cam] cloned camera IS a ThinLensCamera (type preserved, not pose-only)" );
	if( tl ) {
		Check( std::abs( (double)tl->GetSensorSize()          - (double)kSensor ) < 1e-9,
		       "[cam] snapshot sensor size reproduces 36mm" );
		Check( std::abs( (double)tl->GetFocalLengthStored()   - (double)kFocal ) < 1e-9,
		       "[cam] snapshot focal length reproduces 85mm" );
		Check( std::abs( (double)tl->GetFstop()               - (double)kFstop ) < 1e-9,
		       "[cam] snapshot fstop reproduces f/1.8" );
		Check( std::abs( (double)tl->GetFocusDistanceStored() - (double)kFocus ) < 1e-9,
		       "[cam] snapshot focus distance reproduces 12.5" );
		Check( tl->GetApertureBlades() == 6,
		       "[cam] snapshot aperture blades reproduces 6" );
		Check( std::abs( (double)tl->GetApertureRotation() - 0.3 ) < 1e-9,
		       "[cam] snapshot aperture rotation reproduces 0.3" );
		// Dimensions must follow Film (so a render enumerates the right grid).
		Check( tl->GetWidth() == 128 && tl->GetHeight() == 96,
		       "[cam] snapshot camera frame dims reproduce 128x96" );
	}

	safe_release( snap );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// T-scene-complete: a snapshot of a scene with a light + a film + a
// global environment radiance map + a global medium exposes ALL of them,
// so an isolated render off the snapshot would be faithful.
//
// The spike captured only cloned objects + the active camera pose; a
// render also reads lights, film, environment, and global medium.  This
// test pins that the snapshot now surfaces each of them.
//////////////////////////////////////////////////////////////////////

static void TestSnapshotIsRenderComplete()
{
	std::cout << "Test: snapshot captures lights + film + env + medium"
	          << std::endl;

	Job* pJob = new Job();

	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	Check( pScene != nullptr, "[complete] scene downcasts to concrete Scene" );
	if( !pScene ) { pJob->release(); return; }

	// --- A light. ---
	const double srgb[3] = { 1.0, 0.9, 0.7 };
	const double dir[3]  = { 0, 1, 0 };
	pJob->AddDirectionalLight( "sun", /*power*/ 3.0, srgb, dir );

	// --- A film (replace the qHD default with a distinctive size). ---
	IFilm* pFilm = nullptr;
	RISE_API_CreateFilm( &pFilm, 320, 240, Scalar( 1 ) );
	Check( pFilm != nullptr, "[complete] film created" );
	if( pFilm ) {
		pScene->SetFilm( pFilm );
		pFilm->release();
	}

	// --- A global environment radiance map (painter-backed). ---
	const double envCol[3] = { 0.3, 0.5, 0.7 };
	pJob->AddUniformColorPainter( "envpaint", envCol, "Rec709RGB_Linear" );
	const IPainter* pEnvPaint = pJob->GetPainters()->GetItem( "envpaint" );
	Check( pEnvPaint != nullptr, "[complete] env painter resolvable" );
	if( pEnvPaint ) {
		IRadianceMap* pRMap = nullptr;
		RISE_API_CreateRadianceMap( &pRMap, *pEnvPaint, Scalar( 1.0 ) );
		Check( pRMap != nullptr, "[complete] radiance map created" );
		if( pRMap ) {
			pScene->SetGlobalRadianceMap( pRMap );
			pRMap->release();
		}
	}

	// --- A global homogeneous medium. ---
	IPhaseFunction* pPhase = nullptr;
	RISE_API_CreateIsotropicPhaseFunction( &pPhase );
	Check( pPhase != nullptr, "[complete] phase function created" );
	if( pPhase ) {
		IMedium* pMedium = nullptr;
		const RISEPel sigA( 0.1, 0.2, 0.3 );
		const RISEPel sigS( 0.4, 0.5, 0.6 );
		RISE_API_CreateHomogeneousMedium( &pMedium, sigA, sigS, *pPhase );
		Check( pMedium != nullptr, "[complete] homogeneous medium created" );
		if( pMedium ) {
			pScene->SetGlobalMedium( pMedium );
			pMedium->release();
		}
		pPhase->release();
	}

	// --- Take the snapshot. ---
	SceneSnapshot* snap = pScene->CreateSnapshot();
	Check( snap != nullptr, "[complete] snapshot non-null" );
	if( !snap ) { pJob->release(); return; }

	// (1) Lights captured.
	Check( snap->GetLightCount() == 1, "[complete] snapshot captured the light" );
	Check( snap->GetLight( 0 ) != nullptr, "[complete] snapshot light handle non-null" );

	// (2) Film captured with the right dims (so a render enumerates 320x240).
	Check( snap->HasFilm(), "[complete] snapshot captured a film" );
	Check( snap->GetFilmWidth() == 320 && snap->GetFilmHeight() == 240,
	       "[complete] snapshot film dims == 320x240" );

	// (3) Environment / global radiance map captured.
	Check( snap->HasEnvironment(), "[complete] snapshot captured the environment" );
	Check( snap->GetGlobalRadianceMap() != nullptr,
	       "[complete] snapshot env handle non-null" );

	// (4) Global medium captured.
	Check( snap->GetGlobalMedium() != nullptr,
	       "[complete] snapshot captured the global medium" );
	// And it must be an INDEPENDENT clone (homogeneous medium is editor-
	// mutable in place) — verify the coefficients reproduce the authored
	// values via the public read-back surface.
	const HomogeneousMedium* snapHom =
		dynamic_cast<const HomogeneousMedium*>( snap->GetGlobalMedium() );
	Check( snapHom != nullptr, "[complete] snapshot medium is a HomogeneousMedium" );
	if( snapHom ) {
		const RISEPel a = snapHom->GetAbsorption();
		const RISEPel s = snapHom->GetScattering();
		Check( std::abs( (double)a[0] - 0.1 ) < 1e-9
		    && std::abs( (double)a[2] - 0.3 ) < 1e-9
		    && std::abs( (double)s[0] - 0.4 ) < 1e-9
		    && std::abs( (double)s[2] - 0.6 ) < 1e-9,
		    "[complete] snapshot medium coefficients reproduce authored values" );
		// Independent instance from the live medium.
		Check( snapHom != dynamic_cast<const HomogeneousMedium*>( pScene->GetGlobalMedium() ),
		       "[complete] snapshot medium is a distinct instance from live" );
	}

	safe_release( snap );
	pJob->release();
}

//////////////////////////////////////////////////////////////////////
// Cost measurement (not an assertion): warm CreateSnapshot() time for a
// ~100-object scene with lights + film + env + medium, to put a number on
// the snapshot cost.  Reported, not gated — absolute timings are machine-
// dependent.
//////////////////////////////////////////////////////////////////////

static void MeasureSnapshotCostLargeScene()
{
	std::cout << "Cost: CreateSnapshot() for a ~100-object scene" << std::endl;

	Job* pJob = new Job();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { pJob->release(); return; }

	// One shared geometry + one Lambertian material; 100 distinct objects.
	pJob->AddSphereGeometry( "cgeom", 1.0 );
	const double grey[3] = { 0.5, 0.5, 0.5 };
	pJob->AddUniformColorPainter( "cpaint", grey, "Rec709RGB_Linear" );
	pJob->AddLambertianMaterial( "cmat", "cpaint" );

	RadianceMapConfig nilRMap;
	const double orient[3] = { 0, 0, 0 };
	const double one3[3]   = { 1, 1, 1 };
	const unsigned int N = 100;
	for( unsigned int i = 0; i < N; ++i ) {
		char nm[32];
		std::snprintf( nm, sizeof(nm), "o%03u", i );
		const double pos[3] = { (double)( i % 10 ), (double)( i / 10 ), 0.0 };
		pJob->AddObject( nm, "cgeom", "cmat", nullptr, nullptr,
			nilRMap, pos, orient, one3, true, true );
	}

	// A handful of lights + film + a camera so the cost reflects the full
	// render-faithful capture, not just objects.
	const double lc[3] = { 1, 1, 1 };
	const double lp[3] = { 5, 5, 5 };
	pJob->AddPointOmniLight( "L0", 4.0, lc, lp, false );
	const double ld[3] = { 0, 1, 0 };
	pJob->AddDirectionalLight( "L1", 2.0, lc, ld );

	ICamera* pCam = nullptr;
	if( RISE_API_CreatePinholeCamera( &pCam,
		Point3( 5, 5, 30 ), Point3( 5, 5, 0 ), Vector3( 0, 1, 0 ),
		Scalar( 0.785398 ), 256, 256,
		Scalar( 1 ), Scalar( 0 ), Scalar( 0 ), Scalar( 0 ),
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ) )
	{
		pScene->AddCamera( "default", pCam );
		pCam->release();
	}

	// Warm once (touch caches), then time a few and report the best.
	{ SceneSnapshot* w = pScene->CreateSnapshot(); safe_release( w ); }

	double best = 1e300;
	const int trials = 5;
	for( int t = 0; t < trials; ++t ) {
		auto t0 = std::chrono::high_resolution_clock::now();
		SceneSnapshot* snap = pScene->CreateSnapshot();
		auto t1 = std::chrono::high_resolution_clock::now();
		const double us = std::chrono::duration<double, std::micro>( t1 - t0 ).count();
		if( us < best ) best = us;
		// sanity (asserted): everything captured
		Check( snap && snap->GetObjectCount() == N && snap->GetLightCount() == 2
		    && snap->HasFilm() && snap->HasCamera(),
		    "[cost] large-scene snapshot fully captured" );
		safe_release( snap );
	}
	std::cout << "  [cost] warm CreateSnapshot() for " << N
	          << " objects + 2 lights + film + camera: " << best << " us (best of "
	          << trials << ")" << std::endl;

	pJob->release();
}

//////////////////////////////////////////////////////////////////////

static void TestSnapshotEnvIsImmutable()
{
	std::cout << "Test: a held snapshot's environment is immutable vs live radiance-scale edits (7th-review F8)" << std::endl;
	Job* pJob = new Job();
	Scene* pScene = dynamic_cast<Scene*>( pJob->GetScene() );
	if( !pScene ) { Check( false, "[f8] scene downcast" ); pJob->release(); return; }
	const double envCol[3] = { 0.3, 0.5, 0.7 };
	pJob->AddUniformColorPainter( "envpaint", envCol, "Rec709RGB_Linear" );
	const IPainter* pEnvPaint = pJob->GetPainters()->GetItem( "envpaint" );
	if( !pEnvPaint ) { Check( false, "[f8] env painter" ); pJob->release(); return; }
	IRadianceMap* pRMap = nullptr;
	RISE_API_CreateRadianceMap( &pRMap, *pEnvPaint, Scalar( 1.0 ) );
	if( !pRMap ) { Check( false, "[f8] radiance map created" ); pJob->release(); return; }
	pScene->SetGlobalRadianceMap( pRMap );
	pRMap->release();

	SceneSnapshot* snap = pScene->CreateSnapshot();
	if( !snap ) { Check( false, "[f8] snapshot non-null" ); pJob->release(); return; }
	const IRadianceMap* snapMap = snap->GetGlobalRadianceMap();
	Check( snapMap && std::abs( (double)snapMap->GetScale() - 1.0 ) < 1e-9, "[f8] snapshot env scale captured as 1.0" );

	// Mutate the LIVE map's scale in place (what SetActiveRasterizerRadianceScale does).
	IRadianceMap* liveMap = pScene->GetGlobalRadianceMapMutable();
	Check( liveMap != nullptr, "[f8] live mutable map present" );
	if( liveMap ) liveMap->SetScale( Scalar( 5.0 ) );

	// The snapshot must be UNAFFECTED (independent clone, not addref-shared).
	Check( std::abs( (double)snap->GetGlobalRadianceMap()->GetScale() - 1.0 ) < 1e-9,
	       "[f8] snapshot env scale UNCHANGED after live SetScale (F8)" );
	safe_release( snap );
	pJob->release();
}

int main()
{
	std::cout << "=== SceneSnapshotTest ===" << std::endl;

	TestSnapshotIsIndependentOfLiveMutation();
	TestSnapshotMaterialSlotIsImmutable();
	TestSnapshotPreservesCSGType();
	TestSnapshotLightIsImmutable();
	TestSnapshotCameraIsRenderFaithful();
	TestSnapshotIsRenderComplete();
	MeasureSnapshotCostLargeScene();

	TestSnapshotEnvIsImmutable();

	std::cout << std::endl
	          << passCount << " passed, " << failCount << " failed."
	          << std::endl;
	return failCount == 0 ? 0 : 1;
}
