//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.cpp - Implementation of the SceneEditor mutator.
//    The invariant chain for a transform edit is:
//
//      1. Look up the IObjectPriv* via ObjectManager::GetItem(name)
//      2. Capture obj->GetFinalTransformMatrix() as prevTransform
//         (this is what we'll restore on undo)
//      3. Apply the forward op (TranslateObject, RotateObjectArbAxis,
//         SetPosition, ...)
//      4. obj->FinalizeTransformations()  -- recompute world matrix
//      5. obj->ResetRuntimeData()         -- clear per-object caches
//      6. objectMgr->InvalidateSpatialStructure()
//                                          -- next render rebuilds BSP
//      7. Push the edit (with prevTransform) onto the history
//      8. Set LastDirtyScope = Dirty_ObjectTransform
//
//  PrepareForRendering() is deliberately NOT called here.  It is
//  O(n log n) and would thrash on a 60Hz drag.  The orchestrator
//  ensures it runs once per render via the existing rasterizer
//  contract.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SceneEditor.h"
#include <vector>   // P1: atomic composite undo/redo rollback buffer
#include "CameraIntrospection.h"
#include "../Interfaces/IObjectPriv.h"
#include "../Utilities/Transformable.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Interfaces/IShader.h"
#include "../Interfaces/IShaderManager.h"
#include "MaterialIntrospection.h"
#include "MediaIntrospection.h"
#include "../Animation/KeyframableHelper.h"   // ParseStrictVec3
#include "../Interfaces/ILight.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/IJob.h"
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/IKeyframable.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Cameras/CameraCommon.h"
#include "../Scene.h"   // concrete Scene for the #2b(a) light-generation bump
#include <cmath>

using namespace RISE;

void SceneEditor::BumpSceneLightGeneration()
{
	// IScenePriv carries no light-generation surface (keeping that off the
	// abstract interface is deliberate — see Scene::GetLightTopologyGeneration
	// and the abi-preserving-api-evolution skill).  Downcast to the concrete
	// Scene at this single editor call site; out-of-tree scenes no-op.
	if( Implementation::Scene* concrete =
	    dynamic_cast<Implementation::Scene*>( mScene ) )
	{
		concrete->BumpLightTopologyGeneration();
	}
}

void SceneEditor::BumpSceneLightGenerationIfEmitterSetChanged(
	const IMaterial* prevMat, const IMaterial* newMat )
{
	// The emitter set changes iff at least one of the two bindings is
	// emissive: emissive->anything REMOVES (or replaces) a luminary;
	// anything->emissive ADDS one; emissive->emissive changes its
	// exitance.  A non-emissive->non-emissive swap (the common
	// reflectance-only rebind) leaves the luminary set identical, so we
	// skip the bump and its sampler rebuild.  GetEmitter() is null for a
	// non-emissive material; a null material pointer is treated as
	// non-emissive (defensive).
	const bool prevEmits = ( prevMat && prevMat->GetEmitter() );
	const bool newEmits  = ( newMat  && newMat->GetEmitter()  );
	if( prevEmits || newEmits )
	{
		BumpSceneLightGeneration();
	}
}

void SceneEditor::BumpSceneLightGenerationIfMaterialEmits( const IMaterial* mat )
{
	// A SPATIAL edit on an emissive object (area / world position) or a
	// material-SLOT edit on an emissive material (exitance) changes the
	// LightSampler's cached alias-table weight + representative point
	// (baked at Prepare()) WITHOUT changing the emitter SET.  Bump so a
	// reused RayCaster rebuilds its sampler; else light SELECTION is biased
	// toward the stale footprint (the estimator stays unbiased -- per-sample
	// area / Le are read live).  No-op for a null / non-emissive material.
	if( mat && mat->GetEmitter() )
	{
		BumpSceneLightGeneration();
	}
}

SceneEditor::SceneEditor( IScenePriv& scene )
: mScene( &scene )
, mMaterialManager( 0 )
, mShaderManager( 0 )
, mPainterManager( 0 )
, mScalarPainterManager( 0 )
, mJob( 0 )
, mHistory()
, mLastScope( Dirty_None )
, mCompositeDepth( 0 )
, mScenePhotonsExist( false )
, mLastSetTime( 0 )
, mSceneScale( 0 )
{
	mScenePhotonsExist = ComputeScenePhotonsExist();
}

SceneEditor::~SceneEditor()
{
}

namespace {

// Accumulate the axis-aligned union of every object's bounding box.
// Used by SceneEditor::SceneScale() to derive a "characteristic
// length" for the loaded scene that the camera-control rates can be
// scaled by.
//
// Skip objects whose bbox uses the RISE_INFINITY sentinel:
// `InfinitePlaneGeometry` and a few sky-dome shapes return
// `BoundingBox()`, whose default ctor sets `ll = -RISE_INFINITY`,
// `ur = +RISE_INFINITY` (which is `DBL_MAX`, not IEEE +inf — so a
// plain `isfinite` check passes them through).  Unioning such a box
// into the scene bbox makes `ur - ll = 2·DBL_MAX`, which DOES
// overflow to IEEE +inf in `SceneScale()`'s extent computation; the
// resulting `sceneScale = +inf` blows pan/zoom drag speeds, throws
// the camera to z = ±inf on the first drag event, and makes
// subsequent zooms appear to do nothing.  (Pan/Zoom's NaN guard
// doesn't catch ±inf.)  Filtering here preserves "characteristic
// length of the finite scene content" and falls back to the 1.0
// floor in SceneScale() only when literally every object is
// unbounded.  Use half RISE_INFINITY as the threshold so individual
// finite scenes — even very large ones — never get spuriously
// classified as unbounded.
class BoundingBoxAccumulator : public IEnumCallback<IObject>
{
public:
	BoundingBox bbox;
	bool        hasAny;

	BoundingBoxAccumulator() : hasAny( false ) {}

	bool operator()( const IObject& obj ) override {
		const BoundingBox b = obj.getBoundingBox();
		const Scalar      kUnboundedThreshold = RISE_INFINITY * Scalar( 0.5 );
		if( std::fabs( b.ll.x ) > kUnboundedThreshold
		 || std::fabs( b.ll.y ) > kUnboundedThreshold
		 || std::fabs( b.ll.z ) > kUnboundedThreshold
		 || std::fabs( b.ur.x ) > kUnboundedThreshold
		 || std::fabs( b.ur.y ) > kUnboundedThreshold
		 || std::fabs( b.ur.z ) > kUnboundedThreshold ) {
			return true;   // skip unbounded objects (infinite planes, sky domes)
		}
		if( !hasAny ) {
			bbox = b;
			hasAny = true;
		} else {
			if( b.ll.x < bbox.ll.x ) bbox.ll.x = b.ll.x;
			if( b.ll.y < bbox.ll.y ) bbox.ll.y = b.ll.y;
			if( b.ll.z < bbox.ll.z ) bbox.ll.z = b.ll.z;
			if( b.ur.x > bbox.ur.x ) bbox.ur.x = b.ur.x;
			if( b.ur.y > bbox.ur.y ) bbox.ur.y = b.ur.y;
			if( b.ur.z > bbox.ur.z ) bbox.ur.z = b.ur.z;
		}
		return true;   // keep enumerating
	}
};

}  // namespace

Scalar SceneEditor::SceneScale() const
{
	if( mSceneScale > 0 ) return mSceneScale;

	BoundingBoxAccumulator acc;
	if( const IObjectManager* objs = mScene->GetObjects() ) {
		objs->EnumerateObjects( acc );
	}

	Scalar diag = 1.0;
	if( acc.hasAny ) {
		const Vector3 ext = acc.bbox.GetExtents();
		const Scalar  d   = std::sqrt(
			static_cast<double>( ext.x * ext.x + ext.y * ext.y + ext.z * ext.z ) );
		// Floor at 1.0 — pathological scenes (single point, zero
		// extent) shouldn't make the camera-control rates collapse
		// to zero.
		if( d > diag ) diag = d;
	}
	mSceneScale = diag;
	return mSceneScale;
}

bool SceneEditor::ComputeScenePhotonsExist() const
{
	return mScene->GetCausticPelMap()       != 0
	    || mScene->GetGlobalPelMap()        != 0
	    || mScene->GetTranslucentPelMap()   != 0
	    || mScene->GetCausticSpectralMap()  != 0
	    || mScene->GetGlobalSpectralMap()   != 0
	    || mScene->GetShadowMap()           != 0;
}

// The IObjectManager only exposes EnumerateItemNames + GetItem
// (forward name -> ptr).  For SceneEditor we go forward only.
// PickObject (reverse ptr -> name) is added in Phase 2 alongside
// the controller, where we'll cache an enumeration once per
// invalidation cycle.

namespace
{
	// True iff any component is NaN.  Used as a defensive guard when
	// pan/zoom compute newRest from the post-orbit screen basis: if
	// the camera has somehow ended up with a degenerate or NaN basis
	// (e.g. from past-pole orbit accumulation that bypassed the new
	// theta clamp), we'd rather no-op the edit than write NaN into
	// vPosition, where it would survive every future Recompute and
	// surface in the panel as "nan nan nan".
	static bool HasNaN( const Point3& p )
	{
		return std::isnan( p.x ) || std::isnan( p.y ) || std::isnan( p.z );
	}

	// Apply forward camera op to a CameraCommon, given screen-space
	// pixel deltas in v3a.  `sceneScale` is the characteristic length
	// of the current scene (bbox-union diagonal) — pan / zoom use
	// this to scale absolute world-space changes per pixel by overall
	// scene size, so a small scene gets small movements and a large
	// one gets large movements, regardless of camera position.
	// Orbit stays angular (no scene-scale dependency — rotation
	// angles aren't a function of scene extent).  Caller must call
	// RegenerateData() once after the mutation completes.
	static void ApplyCameraOpForward( Implementation::CameraCommon& cam,
	                                  const SceneEdit& e,
	                                  const Scalar sceneScale )
	{
		// Two distinct positions are in play:
		//
		//   - GetLocation()      = frame.GetOrigin()  (POST-orbit, the
		//                          position the rasterizer renders from
		//                          after target_orientation rotates
		//                          vPosition around vLookAt).  Use this
		//                          to derive the screen-space basis so
		//                          pan/zoom directions match what the
		//                          user sees.
		//   - GetRestLocation()  = vPosition          (PRE-orbit, the
		//                          rest position stored in the scene
		//                          file).  Pan/Zoom mutate this.
		//                          Recompute() will re-apply orbit on
		//                          top of the new vPosition; setting
		//                          GetLocation()-based math into
		//                          vPosition would double-apply the
		//                          orbit on every pan, breaking the
		//                          orbit↔pan composition.
		Point3  eyePos  = cam.GetLocation();         // post-orbit eye for screen-basis math
		Point3  restPos = cam.GetRestLocation();     // vPosition — what we mutate for pan/zoom
		Point3  lookAt  = cam.GetStoredLookAt();
		Vector3 up      = Vector3Ops::Normalize( cam.GetStoredUp() );

		// Forward / right / up in world space, derived from the
		// POST-orbit eye → look-at direction so screen-space "right"
		// and "up" match what the user is looking at.  Normalize is
		// safe against a zero vector — it returns the input unchanged
		// — so a degenerate camera (eye == lookAt) just produces a
		// no-op camera op rather than a NaN.
		Vector3 toLookAt;
		toLookAt.x = lookAt.x - eyePos.x;
		toLookAt.y = lookAt.y - eyePos.y;
		toLookAt.z = lookAt.z - eyePos.z;
		Vector3 forward = Vector3Ops::Normalize( toLookAt );
		Vector3 right   = Vector3Ops::Normalize( Vector3Ops::Cross( forward, up ) );
		Vector3 trueUp  = Vector3Ops::Cross( right, forward );

		switch( e.op )
		{
		case SceneEdit::OrbitCamera:
		{
			// v3a.x = horizontal pixel delta, v3a.y = vertical.
			// 1 px ≈ 0.5 deg phi / theta.
			//
			// Mutate the camera's `target_orientation` (the
			// already-existing parameter that drives orbit-around-
			// look-at via CameraTransforms::AdjustCameraForThetaPhi)
			// so the orbit is parametrized as angles instead of a
			// post-rotation world position.  Two payoffs over the
			// previous "rotate vPosition" approach:
			//   - Keyframable: each angle is a scalar that the
			//     existing TARGET_ORIENTATION_ID animator path
			//     interpolates without further work.
			//   - Round-trippable: the .RISEscene file already has
			//     `target_orientation`, so saving back is a
			//     parameter rewrite, no derived-state inversion.
			//
			// vPosition / vLookAt are NOT touched — Recompute()
			// derives the post-orbit position from the angles.
			//
			// Theta clamping: we clamp theta into a band slightly
			// inside ±π/2 (≈±89°) to avoid gimbal lock at the poles.
			// AdjustCameraForThetaPhi now clamps symmetrically too,
			// so this is a defence-in-depth: the math layer enforces
			// the band for rendering, but storing an out-of-band
			// value would make the panel display drift away from
			// what the rasterizer is using.  Keeping the clamp here
			// keeps storage and render in sync.
			static const Scalar kThetaLimit = Scalar( 1.553343 );  // ~89° in rad
			// X axis is negated so the orbit feels like "grab the
			// scene": drag right → the scene rotates right toward
			// the pointer (camera azimuth moves the OPPOSITE way).
			// Same convention as PanCamera's grab-the-world X.  Y
			// (theta / elevation) keeps its sign — drag-down to
			// look-from-above is the established convention.
			const Scalar phiDelta   = -e.v3a.x * 0.0087;  // ~0.5 deg/px (azimuth, grab-world)
			const Scalar thetaDelta =  e.v3a.y * 0.0087; // ~0.5 deg/px (elevation)

			Vector2 t = cam.GetTargetOrientation();
			t.x += thetaDelta;
			if( t.x >  kThetaLimit ) t.x =  kThetaLimit;
			if( t.x < -kThetaLimit ) t.x = -kThetaLimit;
			t.y += phiDelta;     // phi wraps freely — no gimbal-lock issues around the up axis
			cam.SetTargetOrientation( t );
			break;
		}

		case SceneEdit::PanCamera:
		{
			// "Grab the world" pan, consistent on both axes:
			//   - Drag right  (positive dx in image-pixel space)
			//                  → scene appears to drag right with the
			//                  pointer (camera moves LEFT in world).
			//   - Drag down   (positive dy in top-left-origin pixel
			//                  space)
			//                  → scene appears to drag down with the
			//                  pointer (camera moves UP in world).
			//
			// Both axes are negated relative to "move-the-camera"
			// semantics.  Y already had the right sign because the
			// pixel-space Y is inverted relative to world-space Y
			// (top-left origin → +dy is downward in pixels but the
			// existing code adds `+dy * trueUp` which moves the
			// camera UP in world, which is what we want).  X had the
			// opposite sign — `+dx * right` moved the camera right,
			// making the scene appear to drag LEFT under the pointer.
			// Negating dx flips it to grab-the-world feel.
			//
			// We translate the REST position (not the post-orbit eye)
			// because Recompute() re-applies target_orientation to
			// vPosition; setting vPosition to (eye + delta) would
			// double-apply the orbit on every pan.  Translating
			// vPosition AND lookAt by the same delta is invariant
			// under orbit (rotation around lookAt fixes both
			// endpoints under the same translation).
			//
			// Speed scales with the scene's bbox diagonal so small
			// scenes get small per-pixel pan distances and large
			// scenes get large ones.  The factor 0.0015 is tuned for
			// a "standard"-sized scene; values are roughly the same
			// as the previous dist-based formula when dist ≈ scene
			// diagonal (typical camera placement), but no longer
			// depend on the camera's distance from the look-at — so
			// extreme camera placements (very close or very far)
			// don't make pan feel uneven.
			const Scalar speed = sceneScale * 0.0015;
			const Scalar dx    = -e.v3a.x * speed;   // grab-world X
			const Scalar dy    =  e.v3a.y * speed;

			Point3 newRest;
			Point3 newLook;
			newRest.x = restPos.x + right.x * dx + trueUp.x * dy;
			newRest.y = restPos.y + right.y * dx + trueUp.y * dy;
			newRest.z = restPos.z + right.z * dx + trueUp.z * dy;
			newLook.x = lookAt.x  + right.x * dx + trueUp.x * dy;
			newLook.y = lookAt.y  + right.y * dx + trueUp.y * dy;
			newLook.z = lookAt.z  + right.z * dx + trueUp.z * dy;
			if( HasNaN( newRest ) || HasNaN( newLook ) ) break;   // refuse NaN propagation
			cam.SetLocation( newRest );
			cam.SetLookAt( newLook );
			break;
		}

		case SceneEdit::ZoomCamera:
		{
			// Vertical drag dolly: drag down → move closer.
			// Pixel space is top-left origin (positive dy = drag down),
			// so dollyD = +dy * speed moves along forward (toward the
			// look-at).  Clamps below prevent passing through it.
			//
			// Subtle: pan translates BOTH vPosition AND vLookAt, so
			// the rest-space delta passes through the orbit rotation
			// unchanged (orbit fixes lookAt, and translating both
			// endpoints by the same vector commutes with rotation
			// around lookAt).  Zoom moves ONLY vPosition — the
			// rest-space delta gets ROTATED by target_orientation on
			// the way out, so we must compute the dolly direction in
			// REST space so that after Recompute applies orbit, the
			// post-orbit eye actually moves toward the look-at.
			//
			// Concretely: rest_forward = (lookAt - vPosition).
			// Setting vPosition_new = vPosition + dollyD × rest_forward
			// makes post-orbit eye move along R × rest_forward, which
			// equals post_forward (since post_forward is orbit applied
			// to rest_forward).  Using post_forward here would rotate
			// the dolly direction by target_orientation TWICE, sending
			// the camera sideways instead of toward the target whenever
			// the user has orbited.
			//
			// Speed scales with scene size for the same reason as
			// pan above — dollying a small scene shouldn't traverse
			// the same world distance as dollying a large one.
			Vector3 toLookAtRest;
			toLookAtRest.x = lookAt.x - restPos.x;
			toLookAtRest.y = lookAt.y - restPos.y;
			toLookAtRest.z = lookAt.z - restPos.z;
			Vector3 restForward = Vector3Ops::Normalize( toLookAtRest );
			const Scalar speed  = sceneScale * 0.005;
			const Scalar dollyD = e.v3a.y * speed;
			Point3 newRest;
			newRest.x = restPos.x + restForward.x * dollyD;
			newRest.y = restPos.y + restForward.y * dollyD;
			newRest.z = restPos.z + restForward.z * dollyD;
			// Clamp so the rest position doesn't cross through the
			// look-at point.
			Vector3 newOffset;
			newOffset.x = lookAt.x - newRest.x;
			newOffset.y = lookAt.y - newRest.y;
			newOffset.z = lookAt.z - newRest.z;
			if( Vector3Ops::Magnitude( newOffset ) < 1e-3 ) break;
			if( HasNaN( newRest ) ) break;   // refuse NaN propagation
			cam.SetLocation( newRest );
			break;
		}

		case SceneEdit::RollCamera:
		{
			// Horizontal drag = roll around the (camera→look-at)
			// forward axis.  Y is ignored — roll has only one
			// degree of freedom.  Mutates orientation.z (the roll
			// component of the existing pitch / yaw / roll triple
			// applied by AdjustCameraForOrientation).
			//
			// `s` is the pixel delta the controller already
			// converted from v3a.x; we use it directly.  Same
			// 0.0087 rad/px sensitivity as orbit so the feel
			// matches.  No scene-scale dependency — roll is purely
			// angular.
			const Scalar rollDelta = e.s * 0.0087;
			Vector3 o = cam.GetEulerOrientation();
			o.z += rollDelta;
			cam.SetEulerOrientation( o );
			break;
		}

		case SceneEdit::SetCameraTransform:
		{
			// Absolute set: v3a = pos, v3b = lookAt.  Up unchanged.
			Point3 newPos( e.v3a.x, e.v3a.y, e.v3a.z );
			Point3 newLook( e.v3b.x, e.v3b.y, e.v3b.z );
			cam.SetLocation( newPos );
			cam.SetLookAt( newLook );
			break;
		}

		default:
			break;
		}
	}

	static void RestoreCameraTransform( Implementation::CameraCommon& cam, const SceneEdit& e )
	{
		// Restore every captured field, not just the position triple.
		// OrbitCamera mutates target_orientation only; RollCamera
		// mutates orientation only; Pan / Zoom mutate position /
		// look-at only.  Restoring all five fields is correct for
		// every op (no-op restores for fields the forward path
		// didn't touch) and keeps Undo monomorphic.
		cam.SetLocation( e.prevCameraPos );
		cam.SetLookAt( e.prevCameraLookAt );
		cam.SetUp( e.prevCameraUp );
		cam.SetTargetOrientation( e.prevCameraTargetOrient );
		cam.SetEulerOrientation( e.prevCameraOrient );
	}
}

IObjectPriv* SceneEditor::FindObject( const String& name ) const
{
	const IObjectManager* objs = mScene->GetObjects();
	if( !objs ) return 0;
	IObjectPriv* obj = objs->GetItem( name.c_str() );
	return obj;
}

namespace {

// Trim surrounding whitespace + parse the common bool spellings the
// parser's ParseStateBag::GetBool accepts (`true`/`false`/`TRUE`/
// `FALSE`/`1`/`0`/`yes`/`no` + case variants).  Returns `false` on
// parse failure; `out` is overwritten only on success.  Used by the
// SetLightProperty shootphotons branch + its undo/redo counterparts
// so the edit-time vocabulary matches what users can write in their
// scene files.
bool ParseLenientBool( const String& v, bool& out )
{
	// Find first / last non-whitespace.
	const char* p = v.c_str();
	const char* end = p + (v.size() > 0 ? v.size() - 1 : 0);  // RString length excludes trailing NUL
	while( p < end && ( *p == ' ' || *p == '\t' ) ) ++p;
	while( end > p && ( end[-1] == ' ' || end[-1] == '\t' ) ) --end;
	const size_t n = static_cast<size_t>( end - p );
	auto eqi = [&]( const char* s, size_t len ) -> bool {
		if( n != len ) return false;
		for( size_t i = 0; i < n; ++i ) {
			char a = p[i];
			if( a >= 'A' && a <= 'Z' ) a = static_cast<char>( a - 'A' + 'a' );
			if( a != s[i] ) return false;
		}
		return true;
	};
	if( eqi( "true",  4 ) || eqi( "yes", 3 ) || eqi( "1", 1 ) ) { out = true;  return true; }
	if( eqi( "false", 5 ) || eqi( "no",  2 ) || eqi( "0", 1 ) ) { out = false; return true; }
	return false;
}

// Translate a chunk-descriptor parameter name to the keyframe-API name
// that `ILight::KeyframeFromParameters` accepts.  The two namespaces
// diverged historically — chunk names follow the parser vocabulary
// (`power`, `inner`, `outer`, `shootphotons`) while keyframe names
// follow the animator vocabulary (`energy`, `inner_angle`,
// `outer_angle`).  All other names match (`position`, `color`,
// `target`, `direction`) and pass through unchanged.
String ChunkNameToKeyframeName( const String& chunkName )
{
	if( chunkName == String( "power" ) ) return String( "energy" );
	if( chunkName == String( "inner" ) ) return String( "inner_angle" );
	if( chunkName == String( "outer" ) ) return String( "outer_angle" );
	return chunkName;
}

// Read a single light property as a parser-formatted string so undo
// can replay it through the same `KeyframeFromParameters` pipeline
// the forward path uses.  The set of recognised property names MUST
// match every editable row `LightIntrospection` surfaces — if a
// property goes through Apply (forward) but not through ReadLightProperty
// (capture), the captured prev string is empty, undo's keyframe parse
// returns null, the edit is silently dropped from history, and redo
// can't replay it either.  Currently covers: position / power / color
// (all types), target / inner / outer (spot), direction
// (directional).  Numeric values are formatted with %g matching what
// `LightIntrospection` displays, so the round-trip is lossless within
// %g precision.
String ReadLightProperty( const ILight& light, const String& propertyName )
{
	char buf[128];
	if( propertyName == String( "position" ) ) {
		const Point3 p = light.position();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( p.x ), static_cast<double>( p.y ), static_cast<double>( p.z ) );
		return String( buf );
	}
	if( propertyName == String( "power" ) || propertyName == String( "energy" ) ) {
		std::snprintf( buf, sizeof(buf), "%g", static_cast<double>( light.emissionEnergy() ) );
		return String( buf );
	}
	if( propertyName == String( "color" ) ) {
		const RISEPel c = light.emissionColor();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( c.r ), static_cast<double>( c.g ), static_cast<double>( c.b ) );
		return String( buf );
	}
	if( propertyName == String( "target" ) ) {
		const Point3 t = light.emissionTarget();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( t.x ), static_cast<double>( t.y ), static_cast<double>( t.z ) );
		return String( buf );
	}
	if( propertyName == String( "inner" ) || propertyName == String( "inner_angle" ) ) {
		// Full-cone degrees; matches `SpotLight::KeyframeFromParameters`'s
		// degree-to-radian conversion on input.
		const double deg = static_cast<double>( light.emissionInnerAngle() ) * 180.0 / static_cast<double>( PI );
		std::snprintf( buf, sizeof(buf), "%g", deg );
		return String( buf );
	}
	if( propertyName == String( "outer" ) || propertyName == String( "outer_angle" ) ) {
		const double deg = static_cast<double>( light.emissionOuterAngle() ) * 180.0 / static_cast<double>( PI );
		std::snprintf( buf, sizeof(buf), "%g", deg );
		return String( buf );
	}
	if( propertyName == String( "direction" ) ) {
		const Vector3 d = light.emissionDirection();
		std::snprintf( buf, sizeof(buf), "%g %g %g",
			static_cast<double>( d.x ), static_cast<double>( d.y ), static_cast<double>( d.z ) );
		return String( buf );
	}
	return String();
}

// Walk a manager to find the registered name corresponding to a
// pointer.  IMaterial / IShader don't expose GetName, so name
// recovery is an O(n) reverse-lookup.  Cheap at panel-edit cadence;
// cached only via the snapshot we pass into SceneEdit.
template <class MgrT, class ItemT>
String FindManagerName( MgrT* mgr, const ItemT* target )
{
	if( !mgr || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		MgrT* mgr;
		const ItemT* target;
		String found;
		bool operator()( const char* const& name ) override {
			if( mgr->GetItem( name ) == target ) { found = String( name ); return false; }
			return true;
		}
	};
	Cb cb;
	cb.mgr    = mgr;
	cb.target = target;
	mgr->EnumerateItemNames( cb );
	return cb.found;
}

// Reverse-lookup a medium pointer to its registered name through
// IJob's enumeration + lookup pair.  Media live in `Job::mediaMap`
// rather than a real manager (every other entity has an IManager<T>
// subclass), so the lookup goes through the IJob virtuals
// `GetMedium` / `EnumerateMediumNames` rather than a manager
// template.  Same O(N) cost / cadence as `FindManagerName`.
String FindMediumName( const IJob* job, const IMedium* target )
{
	if( !job || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		const IJob*    job;
		const IMedium* target;
		String         found;
		bool operator()( const char* const& name ) override {
			if( job->GetMedium( name ) == target ) { found = String( name ); return false; }
			return true;
		}
	};
	Cb cb;
	cb.job    = job;
	cb.target = target;
	job->EnumerateMediumNames( cb );
	return cb.found;
}

// Reverse-lookup a geometry pointer to its registered name through
// IJob's GetGeometry / EnumerateGeometryNames pair (runtime geometry
// swap).  Same O(N) cost / cadence as FindMediumName.
String FindGeometryName( const IJob* job, const IGeometry* target )
{
	if( !job || !target ) return String();
	struct Cb : public IEnumCallback<const char*> {
		const IJob*      job;
		const IGeometry* target;
		String           found;
		bool operator()( const char* const& name ) override {
			if( job->GetGeometry( name ) == target ) { found = String( name ); return false; }
			return true;
		}
	};
	Cb cb;
	cb.job    = job;
	cb.target = target;
	job->EnumerateGeometryNames( cb );
	return cb.found;
}

// Read a single medium property as a parser-formatted "r g b" string
// so undo can replay it through the same ParseStrictVec3 +
// MediaIntrospection::SetSlotValue pipeline the forward path uses.
// Matches the format LightIntrospection / MediaIntrospection use for
// vec3 rows.  Returns empty for unsupported slot / type — Apply
// detects empty prev and rejects the edit rather than push a phantom
// undo entry.
String ReadMediumProperty( const IMedium& medium, const String& propertyName )
{
	MediumSlotValue v = MediaIntrospection::GetSlotValue( medium, propertyName );
	if( v.kind != MediumSlotValue::Vec3 ) return String();
	char buf[128];
	std::snprintf( buf, sizeof(buf), "%g %g %g", v.v3[0], v.v3[1], v.v3[2] );
	return String( buf );
}

// Apply a medium property value parsed from a "r g b" string via the
// strict parser that rejects NaN / Inf / garbage / trailing junk.
// Returns true on success, false on parse failure or unsupported
// slot (e.g. trying to set "absorption" on a HeterogeneousMedium —
// MediaIntrospection::SetSlotValue refuses).
bool ApplyMediumPropertyValue( IMedium& medium, const String& propertyName, const String& valueStr )
{
	double d[3];
	if( !RISE::Implementation::ParseStrictVec3( valueStr, d ) ) return false;
	MediumSlotValue v;
	v.kind  = MediumSlotValue::Vec3;
	v.v3[0] = d[0]; v.v3[1] = d[1]; v.v3[2] = d[2];
	return MediaIntrospection::SetSlotValue( medium, propertyName, v );
}

}  // namespace

bool SceneEditor::ApplyObjectOpForward( IObjectPriv& obj, const SceneEdit& edit )
{
	bool ok = true;   // P1: false if a binding op's forward target name no longer resolves
	switch( edit.op )
	{
	case SceneEdit::TranslateObject:
		obj.TranslateObject( edit.v3a );
		break;
	case SceneEdit::RotateObjectArb:
		obj.RotateObjectArbAxis( edit.v3a, edit.s );
		break;
	case SceneEdit::SetObjectPosition:
		obj.SetPosition( Point3( edit.v3a.x, edit.v3a.y, edit.v3a.z ) );
		break;
	case SceneEdit::SetObjectOrientation:
		obj.SetOrientation( edit.v3a );
		break;
	case SceneEdit::SetObjectScale:
		obj.SetScale( edit.s );
		break;
	case SceneEdit::SetObjectStretch:
		obj.SetStretch( edit.v3a );
		break;
	case SceneEdit::ScaleObjectFromAnchor:
		// Anchor-based scale: restore the drag-start matrix, then
		// push a stretch-by-factor matrix on top so the result is
		// `prevTransform · Stretch(v3a)`.  See SceneEdit.h's enum
		// comment for why this op exists vs `SetObjectStretch`.
		// Order on the stack matters: PushTop pushes to the FRONT
		// of `m_transformstack`, and `FinalizeTransformations`
		// left-multiplies entries front-to-back into the final
		// matrix, so the second push lands OUTSIDE the first in
		// the final composition (= the factor is applied to the
		// object's local geometry FIRST, then the drag-start
		// transform places the scaled geometry in the world).
		obj.ClearAllTransforms();
		obj.PushTopTransStack( edit.prevTransform );
		obj.PushTopTransStack( Matrix4Ops::Stretch( edit.v3a ) );
		break;
	case SceneEdit::SetObjectMaterial:
		if( mMaterialManager ) {
			IMaterial* mat = mMaterialManager->GetItem( edit.propertyValue.c_str() );
			if( mat ) {
				// P1-4: capture the PRIOR binding before the swap so we can
				// detect an emitter-set change and bump the light-topology
				// generation (a reused RayCaster then rebuilds its
				// LightSampler — else a cached luminary on a now-non-
				// emissive material would deref a NULL emitter).  Covers
				// the forward Apply AND both Redo paths (composite + single)
				// since they all route material binds through here.
				const IMaterial* prevMat = obj.GetMaterial();
				obj.AssignMaterial( *mat );
				BumpSceneLightGenerationIfEmitterSetChanged( prevMat, mat );
			} else {
				ok = false;   // P1: forward target removed since capture -> redo can't bind
			}
		} else {
			ok = false;
		}
		break;
	case SceneEdit::SetObjectShader:
		if( mShaderManager ) {
			IShader* sh = mShaderManager->GetItem( edit.propertyValue.c_str() );
			if( sh ) obj.AssignShader( *sh );
			else     ok = false;   // P1: forward shader removed
		} else {
			ok = false;
		}
		break;
	case SceneEdit::SetObjectShadowFlags: {
		const int flags = static_cast<int>( edit.s );
		obj.SetShadowParams( ( flags & 1 ) != 0, ( flags & 2 ) != 0 );
		break;
	}
	case SceneEdit::SetObjectInteriorMedium:
		// Empty propertyValue OR the literal string "none" is the
		// "clear interior medium" sentinel.  Both match the load-
		// time parser's `interior_medium "none"` behaviour (which
		// short-circuits the SetObjectInteriorMedium call entirely
		// — see AsciiSceneParser.cpp StandardObjectAsciiChunkParser).
		// Non-"none" non-empty resolves through IJob::GetMedium.
		if( edit.propertyValue.size() <= 1 || edit.propertyValue == String( "none" ) ) {
			obj.ClearInteriorMedium();
		} else if( mJob ) {
			const IMedium* med = mJob->GetMedium( edit.propertyValue.c_str() );
			if( med ) obj.AssignInteriorMedium( *med );
			else      ok = false;   // P1: forward medium removed
		} else {
			ok = false;
		}
		break;
	case SceneEdit::SetObjectGeometry:
		// Runtime geometry swap.  Resolve the new geometry name via
		// IJob (same mechanism as interior_medium) and rebind.  The
		// bbox-invalidation / TLAS rebuild runs in the caller's
		// OpNeedsSpatialRebuild branch.
		if( mJob ) {
			const IGeometry* g = mJob->GetGeometry( edit.propertyValue.c_str() );
			if( g ) obj.AssignGeometry( *g );
			else    ok = false;   // P1: forward geometry removed
		} else {
			ok = false;
		}
		break;
	default:
		// Caller guarantees IsObjectOp(edit.op) — this is a coding
		// error if reached, but we silently no-op rather than crash.
		ok = false;
		break;
	}
	return ok;
}

void SceneEditor::RestoreObjectTransform( IObjectPriv& obj, const SceneEdit& edit )
{
	// Prefer the full component-decomposed state captured at edit time: it
	// restores position/orientation/scale/stretch as COMPONENTS, so a later
	// absolute SetPosition/SetOrientation/... replaces the right component
	// instead of composing with a baseline collapsed onto the stack (the
	// pre-existing transform-undo bug).  Fall back to the collapsed-matrix
	// restore for ScaleObjectFromAnchor / non-Transformable targets.
	if( edit.hasTransformState ) {
		if( Implementation::Transformable* t = dynamic_cast<Implementation::Transformable*>( &obj ) ) {
			t->RestoreTransformState( edit.prevTransformState );
			return;
		}
	}
	// Fallback (ITransformable composes final = position * orientation *
	// stretch * scale * stack; zero components + push the captured matrix).
	obj.ClearAllTransforms();
	obj.PushTopTransStack( edit.prevTransform );
}

void SceneEditor::RunObjectInvariantChain( IObjectPriv& obj )
{
	obj.FinalizeTransformations();
	obj.ResetRuntimeData();
	const IObjectManager* objs = mScene->GetObjects();
	if( objs )
	{
		objs->InvalidateSpatialStructure();
	}
	// Re-review finding B: a spatial change to an EMISSIVE object (move /
	// rotate / scale / geometry swap) changes its luminary area + world
	// position, which the LightSampler caches at Prepare().  Bump the light-
	// topology generation so a reused RayCaster rebuilds its sampler; else
	// light SELECTION is biased toward the stale footprint (estimator stays
	// unbiased).  Single choke point for EVERY OpNeedsSpatialRebuild op
	// across Apply / Undo / Redo / composite, so one call covers them all.
	BumpSceneLightGenerationIfMaterialEmits( obj.GetMaterial() );
}

void SceneEditor::MarkEditEntityDirty( const SceneEdit& edit )
{
	// Phase B: route a property-shaped edit into the per-category
	// dirty channel.  Transform ops are deliberately omitted — they
	// mark the object-transform channel (mDirtyTracker.MarkDirty)
	// inline.  AddCamera (new entity, no source span) and SetSceneTime
	// (transient) are intentionally NOT persisted by Phase B.
	switch( edit.op )
	{
	case SceneEdit::SetObjectGeometry:
	case SceneEdit::SetObjectMaterial:
	case SceneEdit::SetObjectShader:
	case SceneEdit::SetObjectShadowFlags:
	case SceneEdit::SetObjectInteriorMedium:
		mDirtyTracker.MarkEntityDirty( EntityCategory::Object,
			std::string( edit.objectName.c_str() ) );
		break;
	case SceneEdit::SetCameraTransform:
	case SceneEdit::OrbitCamera:
	case SceneEdit::PanCamera:
	case SceneEdit::ZoomCamera:
	case SceneEdit::RollCamera:
	case SceneEdit::SetCameraProperty:
	{
		// Camera ops target the ACTIVE camera (the op carries no
		// camera name — SetCameraProperty's objectName is the
		// PROPERTY name).
		const String camName = mScene->GetActiveCameraName();
		if( camName.size() > 0 ) {
			mDirtyTracker.MarkEntityDirty( EntityCategory::Camera,
				std::string( camName.c_str() ) );
		}
		break;
	}
	case SceneEdit::SetLightProperty:
		mDirtyTracker.MarkEntityDirty( EntityCategory::Light,
			std::string( edit.objectName.c_str() ) );
		break;
	case SceneEdit::SetMaterialProperty:
		mDirtyTracker.MarkEntityDirty( EntityCategory::Material,
			std::string( edit.objectName.c_str() ) );
		// Re-review finding B: editing an EMISSIVE material's slot (e.g.
		// exitance) changes the cached alias-table weight; bump light-gen so a
		// reused RayCaster rebuilds its sampler.  MarkEditEntityDirty is the
		// shared per-edit hook (Apply / Undo / Redo / composite all route
		// through it), so one site covers every path.  edit.objectName is the
		// material name for SetMaterialProperty.
		if( mMaterialManager )
		{
			BumpSceneLightGenerationIfMaterialEmits(
				mMaterialManager->GetItem( edit.objectName.c_str() ) );
		}
		break;
	case SceneEdit::SetMediumProperty:
		mDirtyTracker.MarkEntityDirty( EntityCategory::Medium,
			std::string( edit.objectName.c_str() ) );
		break;
	case SceneEdit::AddCamera:
		// Phase C: a newly-created entity has no source span — the
		// save engine emits a fresh chunk for it.  objectName carries
		// the new camera's name.
		mDirtyTracker.MarkEntityCreated( EntityCategory::Camera,
			std::string( edit.objectName.c_str() ) );
		break;
	default:
		break;
	}
}

void SceneEditor::FireDirtyChangedIfTransitioned()
{
	// Cheap O(1) check whether the dirty state's emptiness has
	// flipped since the last time we fired.  Listener installed by
	// the GUI bridge layer; tests that don't install one pay
	// only the bool comparison.
	const bool now = HasUnsavedChanges();
	if( now == mPrevHasUnsavedChanges ) return;
	mPrevHasUnsavedChanges = now;
	if( mDirtyChangedListener ) mDirtyChangedListener( now );
}

namespace {
// RAII guard: any return path out of Apply / Undo / Redo /
// ClearDirtyState fires the dirty-changed listener once.  The
// transition check inside `FireDirtyChangedIfTransitioned` makes
// it a no-op when nothing actually changed.  Local scope-exit
// avoids instrumenting every one of the 27+ return statements
// in Apply / 13+ in Redo by hand.
struct DirtyChangeNotifier
{
	RISE::SceneEditor* self;
	explicit DirtyChangeNotifier( RISE::SceneEditor* s ) : self( s ) {}
	~DirtyChangeNotifier() { self->FireDirtyChangedIfTransitioned(); }
};
}

ICamera* SceneEditor::ResolveEditedCamera( const SceneEdit& e )
{
	// F4: restore the camera that was EDITED (recorded at Apply time), not
	// whatever camera happens to be active now.
	if( e.cameraTargetName.size() > 0 ) {
		if( ICameraManager* cm = mScene->GetCamerasMutable() ) {
			if( ICamera* c = cm->GetItem( e.cameraTargetName.c_str() ) ) return c;
		}
		// P1-#5: a recorded name that no longer resolves means the edited
		// camera was removed.  Return null -- do NOT fall through to the
		// active camera, or undo/rollback of camera A would silently
		// overwrite whatever camera is active now (camera B).
		return 0;
	}
	// Legacy edit with no recorded name: the active camera is the target.
	return mScene->GetCameraMutable();
}

// P5 Slice 3: which SceneEdit ops route through the CST (Job::ApplyCstParamEdit) on a retained-CST scene,
// and so RE-DERIVE their entity (churning its serial) instead of mutating it in place.  The identity
// serial-guard in Apply{Forward,Revert}Mutation is SKIPPED for exactly these (it would falsely trip -- they
// apply/revert/redo BY NAME, never the stale pointer).  As the edit set expands (object/light/camera), add
// each newly-CST-routed op HERE -- one place, both guard sites read it.
static inline bool IsCstRoutedOp( SceneEdit::Op op )
{
	return op == SceneEdit::SetMaterialProperty
	    || op == SceneEdit::SetLightProperty;
}

unsigned long long SceneEditor::ResolveTargetSerial( const SceneEdit& e ) const
{
	// P1: the entity whose STATE this op restores -- compare its serial at capture vs
	// apply to detect a remove+re-add of a DIFFERENT instance under the same name.
	// 0 = no identity tracking: SetMediumProperty (mediums have no RemoveMedium so a
	// name can't be reused), SetSceneTime, AddCamera (its undo removes the entity),
	// composite markers, and legacy camera edits with no recorded name.
	//
	// KNOWN LIMITATION (unreachable today): redo of an entity-CREATING op mints a NEW
	// serial.  AddCamera is the only such op and is currently issued only standalone
	// (CloneActiveCamera, never inside a composite).  If a future composite ever
	// brackets AddCamera + a later op on the created entity, composite REDO would
	// recreate the entity with a fresh serial and this guard would then false-refuse
	// the later op.  When entity-creation becomes composable (the planned outliner),
	// the recreated entity must PRESERVE its identity serial across undo/redo (e.g. a
	// serial-preserving re-add), not just its name.
	if( SceneEdit::IsObjectOp( e.op ) ) {
		const IObjectManager* objs = mScene->GetObjects();
		return objs ? objs->GetItemSerial( e.objectName.c_str() ) : 0;
	}
	if( SceneEdit::IsCameraOp( e.op ) || e.op == SceneEdit::SetCameraProperty ) {
		const ICameraManager* cams = mScene->GetCameras();
		return ( cams && e.cameraTargetName.size() > 0 )
		     ? cams->GetItemSerial( e.cameraTargetName.c_str() ) : 0;
	}
	if( e.op == SceneEdit::SetMaterialProperty ) {
		return mMaterialManager ? mMaterialManager->GetItemSerial( e.objectName.c_str() ) : 0;
	}
	if( e.op == SceneEdit::SetLightProperty ) {
		const ILightManager* lights = mScene->GetLights();
		return lights ? lights->GetItemSerial( e.objectName.c_str() ) : 0;
	}
	return 0;
}

// P5 Slice 3: re-point this editor at the Job's CURRENT scene + managers after a CST D2 full re-derive
// (Job::ApplyCstParamEdit result 2 or 3) ClearAll'd the Job -- the Scene + managers this editor cached are now
// freed.  Mirrors SceneEditController::RebindEditorToJob (which re-binds for a variant switch); here the
// editor re-binds ITSELF, synchronously at the edit site, so no frame above derefs a dangling pointer.
// The Job object itself is unchanged (only its containers), so mJob stays valid.
void SceneEditor::RebindToJob_()
{
	IJobPriv* priv = dynamic_cast<IJobPriv*>( mJob );
	if( !priv ) return;
	if( IScenePriv* sc = priv->GetScene() ) RebindScene( *sc );
	SetMaterialManager( priv->GetMaterials() );
	SetShaderManager( priv->GetShaders() );
	SetPainterManager( priv->GetPainters() );
	SetScalarPainterManager( priv->GetScalarPainters() );
	// A material EDIT must not reset the interactive scrub: the D2 ClearAll+re-derive built a fresh scene at
	// the animator's default time, but the user is parked at mLastSetTime -- re-apply it (the scrub-TIME twin of
	// the camera/rasterizer/animation preservation) so the viewport pose matches the timeline slider; the
	// production path already re-applies LastSceneTime before dispatch.  Skipped at the implicit t=0 (no scrub).
	if( mScene && mLastSetTime != 0 ) mScene->SetSceneTimeForPreview( mLastSetTime );
}

// P5 Slice 3 expansion: shared CST-routing for a property edit (material/light/...).  Mirrors the material
// branch -- DocSetOrAddParamValue + re-derive via Job::ApplyCstParamEdit; rebind on a D2 (result >=2); a
// diagnosed re-derive (3) rebinds but reports failure.
bool SceneEditor::RouteCstParamEdit_( const char* entityName, const char* entityKind, const char* role, const char* value )
{
	const int r = mJob->ApplyCstParamEdit( entityName, entityKind, role, 0, value );
	if( r >= 2 ) RebindToJob_();
	return r == 1 || r == 2;
}

bool SceneEditor::ApplyMaterialSlotByName( const SceneEdit& e, const String& painterName )
{
	// F1: shared SetMaterialProperty restore -- resolves the slot's pipe
	// (Painter vs ScalarPainter) and rebinds it to `painterName`.  Undo passes
	// prevPropertyValue; redo passes propertyValue.
	// P1-#2: return FALSE on any failure (empty name, unregistered painter,
	// unknown slot kind, dispatch failure) so the composite/single walks can
	// report a partial revert -- the old `return true` silently swallowed it.
	if( !mMaterialManager ) return false;
	IMaterial* mat = mMaterialManager->GetItem( e.objectName.c_str() );
	if( !mat ) return false;
	if( painterName.size() <= 1 ) return false;
	// P5 Slice 3 (edit-model pivot): when the Job retains a CST Document (LoadAsciiSceneViaCst), route the
	// slot re-point through a CST param-edit + re-derive so the canonical CST stays the source of truth
	// (Slice 4's save serializes it).  Serves BOTH forward and undo (the inverse re-point replays through
	// here), so the mHistory undo stack works unchanged.  "material" disambiguates a cross-category name
	// clash.  Result 2 or 3 = the D2 full re-derive ClearAll'd + replaced the Scene + managers, so re-point THIS
	// editor's cached pointers before returning (the SetMaterialProperty arm reads mLastScope but not the
	// managers after we return; the next edit/undo would dereference the freed ones) -- else use-after-free.
	// Legacy-loaded scenes (no Document) fall through to the direct MaterialIntrospection::SetSlot below.
	if( mJob && mJob->HasRetainedCstDocument() )
		return RouteCstParamEdit_( e.objectName.c_str(), "material", e.propertyName.c_str(), painterName.c_str() );
	const MaterialSlotRef cur = MaterialIntrospection::GetSlot( *mat, e.propertyName );
	if( cur.kind == MaterialSlotRef::Painter ) {
		if( !mPainterManager ) return false;
		const IPainter* p = mPainterManager->GetItem( painterName.c_str() );
		if( !p ) return false;
		return MaterialIntrospection::SetSlot( *mat, e.propertyName, p, 0 );
	}
	if( cur.kind == MaterialSlotRef::ScalarPainter ) {
		if( !mScalarPainterManager ) return false;
		const IScalarPainter* p = mScalarPainterManager->GetItem( painterName.c_str() );
		if( !p ) return false;
		return MaterialIntrospection::SetSlot( *mat, e.propertyName, 0, p );
	}
	return false;
}

bool SceneEditor::CaptureForApply( SceneEdit& edit )
{
	// H2 Stage 3: the capture/validate half of a first Apply.  No mutation,
	// no history push, no scope -- those are ApplyForwardMutation's job.
	// Returns false (reject) on any validation miss, exactly where the old
	// inline Apply returned false.
	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;

		// Capture transform state for undo (ScaleObjectFromAnchor carries a
		// controller-supplied drag-start anchor in prevTransform -- never
		// overwrite it).
		if( edit.op != SceneEdit::ScaleObjectFromAnchor ) {
			edit.prevTransform = obj->GetFinalTransformMatrix();
			if( const Implementation::Transformable* t = dynamic_cast<const Implementation::Transformable*>( obj ) ) {
				edit.prevTransformState = t->CaptureTransformState();
				edit.hasTransformState  = true;
			}
		}

		switch( edit.op ) {
		case SceneEdit::SetObjectMaterial:
			if( !mMaterialManager
			 || !mMaterialManager->GetItem( edit.propertyValue.c_str() ) )
			{
				return false;
			}
			edit.prevPropertyValue  = FindManagerName( mMaterialManager, obj->GetMaterial() );
			edit.prevBindingWasNull = ( obj->GetMaterial() == 0 );
			// P1-#7: a non-null prior material with no manager-registered name has
			// no representable inverse -> reject rather than push a silent-no-op undo.
			if( !edit.prevBindingWasNull && edit.prevPropertyValue.size() <= 1 ) return false;
			break;
		case SceneEdit::SetObjectShader:
			if( !mShaderManager
			 || !mShaderManager->GetItem( edit.propertyValue.c_str() ) )
			{
				return false;
			}
			edit.prevPropertyValue  = FindManagerName( mShaderManager, obj->GetShader() );
			edit.prevBindingWasNull = ( obj->GetShader() == 0 );
			if( !edit.prevBindingWasNull && edit.prevPropertyValue.size() <= 1 ) return false;   // P1-#7
			break;
		case SceneEdit::SetObjectShadowFlags:
			edit.prevShadowFlags = static_cast<Scalar>(
				( obj->DoesCastShadows()    ? 1 : 0 )
			  | ( obj->DoesReceiveShadows() ? 2 : 0 ) );
			break;
		case SceneEdit::SetObjectInteriorMedium:
			if( edit.propertyValue.size() > 1 && edit.propertyValue != String( "none" ) ) {
				if( !mJob || !mJob->GetMedium( edit.propertyValue.c_str() ) ) {
					GlobalLog()->PrintEx( eLog_Warning,
						"SceneEditor: interior_medium edit rejected -- `%s` is not a registered medium",
						edit.propertyValue.c_str() );
					return false;
				}
			}
			edit.prevPropertyValue = FindMediumName( mJob, obj->GetInteriorMedium() );
			// P1-#7: a non-null prior medium with no registered name has no inverse
			// (undo would wrongly Clear it).  Empty prev is valid ONLY when there
			// genuinely was no medium bound.
			if( obj->GetInteriorMedium() != 0 && edit.prevPropertyValue.size() <= 1 ) return false;
			break;
		case SceneEdit::SetObjectGeometry:
			if( !mJob || !mJob->GetGeometry( edit.propertyValue.c_str() ) ) {
				return false;
			}
			edit.prevPropertyValue = FindGeometryName( mJob, obj->GetGeometry() );
			if( edit.prevPropertyValue.size() <= 1 ) return false;   // P1-#7: prior geometry unregistered -> no inverse
			break;
		default:
			break;
		}
		return true;
	}

	if( SceneEdit::IsCameraOp( edit.op ) )
	{
		ICamera* baseCam = mScene->GetCameraMutable();
		if( !baseCam ) return false;
		// Record the active camera name so ApplyForwardMutation resolves the
		// SAME camera via ResolveEditedCamera (pActiveCamera == GetItem(name)).
		edit.cameraTargetName = mScene->GetActiveCameraName();
		Implementation::CameraCommon* cam =
			dynamic_cast<Implementation::CameraCommon*>( baseCam );
		if( !cam ) return true;   // skeleton camera: nothing to capture; forward arm no-ops
		edit.prevCameraPos          = cam->GetRestLocation();
		edit.prevCameraLookAt       = cam->GetStoredLookAt();
		edit.prevCameraUp           = cam->GetStoredUp();
		edit.prevCameraTargetOrient = cam->GetTargetOrientation();
		edit.prevCameraOrient       = cam->GetEulerOrientation();
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		edit.prevTime = mLastSetTime;
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = mScene->GetCameraMutable();
		if( !baseCam ) return false;
		edit.prevPropertyValue = CameraIntrospection::GetPropertyValue( *baseCam, edit.objectName );
		edit.cameraTargetName  = mScene->GetActiveCameraName();
		// The parse/read-only rejection is fused with the SetProperty mutation;
		// ApplyForwardMutation surfaces it (returns false) so Apply rejects.
		return true;
	}

	if( edit.op == SceneEdit::AddCamera )
	{
		if( !mJob ) return false;
		if( edit.objectName.size() <= 1 ) return false;
		edit.prevPropertyValue = String( mJob->GetActiveCameraName().c_str() );
		return true;
	}

	if( edit.op == SceneEdit::SetMaterialProperty )
	{
		if( !mMaterialManager ) return false;
		IMaterial* mat = mMaterialManager->GetItem( edit.objectName.c_str() );
		if( !mat ) return false;
		if( mJob && mJob->IsMaterialComposed( edit.objectName.c_str() ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor: SetMaterialProperty rejected on `%s` -- composed material "
				"(PBR-MR / GGX-Emissive); rebinding a slot would break the painter graph. "
				"Edit upstream painters instead.",
				edit.objectName.c_str() );
			return false;
		}
		const MaterialSlotRef cur = MaterialIntrospection::GetSlot( *mat, edit.propertyName );
		if( cur.kind == MaterialSlotRef::None ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor: SetMaterialProperty rejected -- `%s` has no slot named `%s`",
				edit.objectName.c_str(), edit.propertyName.c_str() );
			return false;
		}
		if( cur.kind == MaterialSlotRef::Painter ) {
			if( !mPainterManager ) return false;
			if( !mPainterManager->GetItem( edit.propertyValue.c_str() ) ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty rejected -- painter `%s` is not registered",
					edit.propertyValue.c_str() );
				return false;
			}
			edit.prevPropertyValue = FindManagerName( mPainterManager, cur.painter );
			if( edit.prevPropertyValue.size() <= 1 ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty on `%s.%s` rejected -- current painter "
					"has no manager-registered name, so undo cannot restore the prior binding.",
					edit.objectName.c_str(), edit.propertyName.c_str() );
				return false;
			}
		} else {
			if( !mScalarPainterManager ) return false;
			if( !mScalarPainterManager->GetItem( edit.propertyValue.c_str() ) ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty rejected -- scalar_painter `%s` is not registered",
					edit.propertyValue.c_str() );
				return false;
			}
			edit.prevPropertyValue = FindManagerName( mScalarPainterManager, cur.scalarPainter );
			if( edit.prevPropertyValue.size() <= 1 ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty on `%s.%s` rejected -- current scalar_painter "
					"has no manager-registered name, so undo cannot restore the prior binding.",
					edit.objectName.c_str(), edit.propertyName.c_str() );
				return false;
			}
		}
		return true;
	}

	if( edit.op == SceneEdit::SetLightProperty )
	{
		ILightManager* lights = const_cast<ILightManager*>( mScene->GetLights() );
		if( !lights ) return false;
		ILightPriv* light = lights->GetItem( edit.objectName.c_str() );
		if( !light ) return false;
		if( edit.propertyName == String( "shootphotons" ) ) {
			edit.prevPropertyValue = String( light->CanGeneratePhotons() ? "true" : "false" );
			return true;
		}
		edit.prevPropertyValue = ReadLightProperty( *light, edit.propertyName );
		// The keyframe-parse rejection is fused with the mutation;
		// ApplyForwardMutation surfaces it so Apply rejects.
		return true;
	}

	if( edit.op == SceneEdit::SetMediumProperty )
	{
		if( !mJob ) return false;
		const IMedium* medConst = mJob->GetMedium( edit.objectName.c_str() );
		if( !medConst ) return false;
		IMedium* medium = const_cast<IMedium*>( medConst );
		edit.prevPropertyValue = ReadMediumProperty( *medium, edit.propertyName );
		if( edit.prevPropertyValue.size() <= 1 ) return false;
		return true;
	}

	return false;   // unknown op
}

bool SceneEditor::Apply( const SceneEdit& editIn )
{
	DirtyChangeNotifier _notifier( this );
	SceneEdit edit = editIn;

	// Route property-shaped edits into the per-category dirty channel up front
	// (transform ops + composite markers are skipped by the helper).
	MarkEditEntityDirty( edit );

	// Composite markers: push, adjust depth, no mutation.
	if( edit.op == SceneEdit::CompositeBegin )
	{
		++mCompositeDepth;
		mHistory.Push( edit );
		return true;
	}
	if( edit.op == SceneEdit::CompositeEnd )
	{
		if( mCompositeDepth > 0 ) --mCompositeDepth;
		mHistory.Push( edit );
		return true;
	}

	// H2 Stage 3: capture/validate, then the SHARED forward mutation, then
	// push.  Forward logic now lives in exactly one place (ApplyForwardMutation),
	// reused by Redo and the composite-redo loop.  Either half returning false
	// rejects the edit with no mutation and no history entry.
	if( !CaptureForApply( edit ) )       return false;
	// P1: capture the edited entity's identity serial AFTER CaptureForApply has set
	// cameraTargetName etc.  Undo/Redo re-resolve by name + compare to catch a
	// remove+re-add that put a different instance under the same name.
	edit.capturedTargetSerial = ResolveTargetSerial( edit );
	if( !ApplyForwardMutation( edit ) )  return false;
	mHistory.Push( edit );
	return true;
}

bool SceneEditor::Undo()
{
	DirtyChangeNotifier _notifier( this );
	SceneEdit edit;
	if( !mHistory.PopForUndo( edit ) ) return false;

	// Phase B: re-mark the (single-edit) entity dirty — undo after a
	// save must put the touched entity back into the dirty set.
	// Composite inner edits are marked inside the walk-back loop.
	MarkEditEntityDirty( edit );

	// Walk back through composite groups: if the popped entry is a
	// CompositeEnd marker, repeatedly undo until and including the
	// matching CompositeBegin.
	if( edit.op == SceneEdit::CompositeEnd )
	{
		// P1: composite undo is ATOMIC.  Revert inners LIFO; if ANY revert fails,
		// roll back the partial work -- re-apply (forward) what we already reverted
		// AND restore the whole popped group to the undo stack -- so a failed composite
		// undo is a true no-op: live state unchanged, the group stays intact + retryable,
		// and a subsequent transaction rollback won't delete half-moved records.  (The
		// earlier "honest partial" left every record on redo with partially-changed state.)
		// PopForUndo already moved the CompositeEnd trigger to redo, so redoMoves starts at 1.
		bool sawObjectOp = false, sawCameraOp = false, sawTimeOp = false, sawPropertyOp = false;
		int  depth     = 1;   // the CompositeEnd trigger opened one level (LIFO walk-back).
		int  redoMoves = 1;   // records moved undo->redo so far (incl. the trigger), for restore-on-failure.
		bool failed    = false;
		std::vector<SceneEdit> reverted;   // inners successfully reverted, for forward roll-back
		while( true )
		{
			SceneEdit inner;
			if( !mHistory.PopForUndo( inner ) ) break;
			++redoMoves;
			// P1: nesting-aware -- a nested CompositeEnd opens a deeper level going
			// backward; only the matching OUTER CompositeBegin (depth 0) ends the walk.
			if( inner.op == SceneEdit::CompositeEnd )   { ++depth; continue; }
			if( inner.op == SceneEdit::CompositeBegin ) { if( --depth == 0 ) break; continue; }
			MarkEditEntityDirty( inner );
			if( !ApplyRevertMutation( inner ) ) { failed = true; break; }   // P1: stop + roll back atomically
			reverted.push_back( inner );
			if( SceneEdit::IsObjectOp( inner.op ) )                                          sawObjectOp = true;
			else if( SceneEdit::IsCameraOp( inner.op ) || inner.op == SceneEdit::AddCamera ) sawCameraOp = true;
			else if( inner.op == SceneEdit::SetSceneTime )                                   sawTimeOp = true;
			else                                                                             sawPropertyOp = true;
		}
		if( failed )
		{
			// Re-apply the reverts we already did, in original FORWARD order (reverse of the
			// LIFO revert order), then move the whole popped group back redo->undo so the
			// composite is intact + retryable.  Rollback re-applies are best-effort.
			for( std::vector<SceneEdit>::reverse_iterator it = reverted.rbegin(); it != reverted.rend(); ++it ) {
				ApplyForwardMutation( *it );
			}
			for( int k = 0; k < redoMoves; ++k ) mHistory.RestoreLastUndoFromRedo();
			mLastScope = Dirty_None;
			return false;
		}
		mLastScope = AggregateCompositeScope( sawObjectOp, sawCameraOp, sawTimeOp, sawPropertyOp );
		return true;
	}

	// Single edit -> the shared revert dispatcher (same one the composite loop uses).
	// P1: PopForUndo already moved this edit to the redo stack.  If the revert FAILS
	// (e.g. a captured prior dependency vanished), restore it to the undo stack -- a
	// failed undo must NOT advance the depth or make the un-reverted edit redo-able.
	if( !ApplyRevertMutation( edit ) ) {
		mHistory.RestoreLastUndoFromRedo();
		return false;
	}
	return true;
}

bool SceneEditor::ApplyRevertMutation( const SceneEdit& edit )
{

	// P1: identity guard -- refuse if the captured target was removed and a DIFFERENT
	// instance re-registered under the same name (serial mismatch); applying the
	// captured state to the replacement would corrupt it.  capturedTargetSerial==0
	// means the op tracks no identity (medium/time/marker/legacy) -> no check.
	// SKIP on the CST edit-model for the ops routed through ApplyCstParamEdit (IsCstRoutedOp): that path
	// RE-DERIVES the entity on every edit, so its serial legitimately changes each time, and it applies/
	// reverts/redoes BY NAME (never the stale pointer) -- the serial guard is both moot and would FALSELY
	// trip.  Direct-mutation ops (object/camera/light) KEEP the guard even on a CST-loaded scene, since
	// they DO mutate the captured instance in place.
	if( edit.capturedTargetSerial != 0 &&
	    !( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) &&
	    ResolveTargetSerial( edit ) != edit.capturedTargetSerial )
		return false;
	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;

		// P1: a binding revert FAILS if the captured prior dependency (material /
		// shader / geometry / medium) was removed AFTER the edit -- GetItem returns
		// null.  Track that as `restored = false` so the caller treats the undo as
		// PARTIAL (rollback keeps the residual-dirty state) instead of reporting
		// success while the edited binding stays live.  Transform ops restore from
		// the captured matrix and always succeed.
		bool restored = true;
		switch( edit.op ) {
		case SceneEdit::SetObjectMaterial:
			if( edit.prevBindingWasNull ) {
				// F5: undo of a FIRST material bind restores the unbound state.
				const IMaterial* clrPrev = obj->GetMaterial();
				// ClearMaterial is an IObjectPriv virtual (workstream #3) -- clear the slot directly.
				obj->ClearMaterial();
				BumpSceneLightGenerationIfEmitterSetChanged( clrPrev, nullptr );
			} else if( mMaterialManager && edit.prevPropertyValue.size() > 1 ) {
				IMaterial* mat = mMaterialManager->GetItem( edit.prevPropertyValue.c_str() );
				if( mat ) {
					const IMaterial* prevMat = obj->GetMaterial();
					obj->AssignMaterial( *mat );
					BumpSceneLightGenerationIfEmitterSetChanged( prevMat, mat );
				} else {
					restored = false;   // P1: prior material was removed -> cannot restore
				}
			} else {
				restored = false;
			}
			break;
		case SceneEdit::SetObjectShader:
			if( edit.prevBindingWasNull ) {
				obj->ClearShader();   // F5: undo of a FIRST shader bind (ClearShader is now an IObjectPriv virtual, workstream #3)
			} else if( mShaderManager && edit.prevPropertyValue.size() > 1 ) {
				IShader* sh = mShaderManager->GetItem( edit.prevPropertyValue.c_str() );
				if( sh ) obj->AssignShader( *sh );
				else     restored = false;   // P1: prior shader removed
			} else {
				restored = false;
			}
			break;
		case SceneEdit::SetObjectShadowFlags: {
			const int flags = static_cast<int>( edit.prevShadowFlags );
			obj->SetShadowParams( ( flags & 1 ) != 0, ( flags & 2 ) != 0 );
			break;
		}
		case SceneEdit::SetObjectGeometry:
			if( mJob && edit.prevPropertyValue.size() > 1 ) {
				const IGeometry* g = mJob->GetGeometry( edit.prevPropertyValue.c_str() );
				if( g ) obj->AssignGeometry( *g );
				else    restored = false;   // P1: prior geometry removed
			} else {
				restored = false;
			}
			RunObjectInvariantChain( *obj );   // bbox rebuild (safe even if geometry unchanged)
			break;
		case SceneEdit::SetObjectInteriorMedium:
			if( edit.prevPropertyValue.size() <= 1 ) {
				obj->ClearInteriorMedium();   // valid prior state: no medium was bound
			} else if( mJob ) {
				const IMedium* med = mJob->GetMedium( edit.prevPropertyValue.c_str() );
				if( med ) obj->AssignInteriorMedium( *med );
				else      restored = false;   // P1: prior medium removed
			} else {
				restored = false;
			}
			break;
		default:
			// Transform op -- restores from the captured component/matrix state,
			// always succeeds.
			RestoreObjectTransform( *obj, edit );
			RunObjectInvariantChain( *obj );
			mDirtyTracker.MarkDirty( std::string( edit.objectName.c_str() ) );
			if( edit.op == SceneEdit::ScaleObjectFromAnchor ) {
				mScaleFromAnchorSet.insert( std::string( edit.objectName.c_str() ) );
			}
			break;
		}
		mLastScope = Dirty_ObjectTransform;
		return restored;
	}

	if( SceneEdit::IsCameraOp( edit.op ) )
	{
		ICamera* baseCam = ResolveEditedCamera( edit );
		if( !baseCam ) return false;
		Implementation::CameraCommon* cam =
			dynamic_cast<Implementation::CameraCommon*>( baseCam );
		if( !cam ) return true;   // skeleton-camera edit was a no-op
		RestoreCameraTransform( *cam, edit );
		cam->RegenerateData();
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		// Restore the time captured before the edit.  Use the preview
		// path (no photon regen) so undo is fast.
		mScene->SetSceneTimeForPreview( edit.prevTime );
		mLastSetTime = edit.prevTime;
		mLastScope = mScenePhotonsExist ? Dirty_TimeAndPhotons : Dirty_Time;
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = ResolveEditedCamera( edit );
		if( !baseCam ) return false;
		// Replay the captured prev value through the same parser.
		CameraIntrospection::SetProperty( *baseCam, edit.objectName, edit.prevPropertyValue );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AddCamera )
	{
		// Inverse: remove the just-added camera, then restore the
		// pre-Add active camera (RemoveCamera auto-promotes
		// lexicographically, which doesn't match the user's prior
		// selection — explicit SetActiveCamera fixes that).  When
		// the captured prev camera no longer exists (some other code
		// path removed it between Apply and Undo — rare today, but
		// possible once a Phase-4 RemoveCamera op lands), log and
		// leave the auto-promoted active in place rather than
		// silently swallowing the inconsistency.
		if( !mJob ) return false;
		mJob->RemoveCamera( edit.objectName.c_str() );
		if( edit.prevPropertyValue.size() > 1 ) {
			if( !mJob->SetActiveCamera( edit.prevPropertyValue.c_str() ) ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: undo of AddCamera could not restore prior active `%s` (no longer registered); auto-promoted camera remains active",
					edit.prevPropertyValue.c_str() );
			}
		}
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetMaterialProperty )
	{
		const bool ok = ApplyMaterialSlotByName( edit, edit.prevPropertyValue );
		if( ok ) mLastScope = Dirty_Camera;
		return ok;
	}

	if( edit.op == SceneEdit::SetLightProperty )
	{
		ILightManager* lights = const_cast<ILightManager*>( mScene->GetLights() );
		if( !lights ) return false;
		ILightPriv* light = lights->GetItem( edit.objectName.c_str() );
		if( !light ) return false;
		// P5 Slice 3 expansion: CST-route the inverse light edit too (replays through the SAME CST path).
		if( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) {
			if( !RouteCstParamEdit_( edit.objectName.c_str(), "light", edit.propertyName.c_str(), edit.prevPropertyValue.c_str() ) ) return false;
			mLastScope = Dirty_Camera;
			return true;
		}
		// shootphotons round-trips through the direct setter, not the
		// keyframe path.  Match the Apply branch above.
		if( edit.propertyName == String( "shootphotons" ) ) {
			bool prevVal = false;
			ParseLenientBool( edit.prevPropertyValue, prevVal );  // prev was captured by us, can't fail
			light->SetCanGeneratePhotons( prevVal );
			mLastScope = Dirty_Camera;
			return true;
		}
		// Replay prev value through the same keyframe machinery.
		IKeyframeParameter* p = light->KeyframeFromParameters(
			ChunkNameToKeyframeName( edit.propertyName ), edit.prevPropertyValue );
		if( !p ) return false;
		light->SetIntermediateValue( *p );
		safe_release( p );
		light->RegenerateData();
		BumpSceneLightGeneration();   // #2b(a): rebuild caster samplers next render
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetMediumProperty )
	{
		// Inverse: re-apply the captured prev value through the same
		// parser the forward path uses.
		if( !mJob ) return false;
		const IMedium* medConst = mJob->GetMedium( edit.objectName.c_str() );
		if( !medConst ) return false;
		IMedium* medium = const_cast<IMedium*>( medConst );
		if( edit.prevPropertyValue.size() <= 1 ) return true;
		ApplyMediumPropertyValue( *medium, edit.propertyName, edit.prevPropertyValue );
		mLastScope = Dirty_Camera;
		return true;
	}

	// Composite Begin/End popped on its own — degenerate, treat as noop.
	if( SceneEdit::IsCompositeMarker( edit.op ) )
	{
		return true;
	}

	// Unreachable for any valid SceneEdit::Op (every op is handled above).
	// A new op MUST add a branch here + in the sibling dispatcher + CaptureForApply;
	// this defensive default no-ops it as a camera-scope edit rather than crash.
	mLastScope = Dirty_Camera;
	return true;
}

SceneEditor::DirtyScope SceneEditor::AggregateCompositeScope( bool sawObjectOp, bool sawCameraOp, bool sawTimeOp, bool sawPropertyOp ) const
{
	if( sawObjectOp )                     return Dirty_ObjectTransform;
	if( sawTimeOp && mScenePhotonsExist ) return Dirty_TimeAndPhotons;
	if( sawTimeOp )                       return Dirty_Time;
	if( sawCameraOp || sawPropertyOp )    return Dirty_Camera;
	return Dirty_None;
}

bool SceneEditor::ApplyForwardMutation( const SceneEdit& edit )
{

	// P1: identity guard -- refuse if the captured target was removed and a DIFFERENT
	// instance re-registered under the same name (serial mismatch); applying the
	// captured state to the replacement would corrupt it.  capturedTargetSerial==0
	// means the op tracks no identity (medium/time/marker/legacy) -> no check.
	// SKIP on the CST edit-model for the ops routed through ApplyCstParamEdit (IsCstRoutedOp): that path
	// RE-DERIVES the entity on every edit, so its serial legitimately changes each time, and it applies/
	// reverts/redoes BY NAME (never the stale pointer) -- the serial guard is both moot and would FALSELY
	// trip.  Direct-mutation ops (object/camera/light) KEEP the guard even on a CST-loaded scene, since
	// they DO mutate the captured instance in place.
	if( edit.capturedTargetSerial != 0 &&
	    !( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) &&
	    ResolveTargetSerial( edit ) != edit.capturedTargetSerial )
		return false;
	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;
		const bool fwdOk = ApplyObjectOpForward( *obj, edit );   // P1: false if a redo target vanished
		// Property-style ops don't move geometry — symmetric with
		// Apply()'s spatial-rebuild gate.  Pre-Phase-1 this path ran
		// the chain unconditionally, costing a spurious BSP
		// invalidation per material/shader/shadow redo.
		const bool needsSpatialRebuild = SceneEdit::OpNeedsSpatialRebuild( edit.op );
		if( needsSpatialRebuild ) {
			RunObjectInvariantChain( *obj );
			// Phase 6.3 (§7.3): single-op redo marks dirty.
			mDirtyTracker.MarkDirty( std::string( edit.objectName.c_str() ) );
			if( edit.op == SceneEdit::ScaleObjectFromAnchor ) {
				mScaleFromAnchorSet.insert( std::string( edit.objectName.c_str() ) );
			}
		}
		mLastScope = Dirty_ObjectTransform;
		return fwdOk;   // P1: forward-bind failure (vanished redo target) is a partial redo
	}

	if( SceneEdit::IsCameraOp( edit.op ) )
	{
		ICamera* baseCam = ResolveEditedCamera( edit );
		if( !baseCam ) return false;
		Implementation::CameraCommon* cam =
			dynamic_cast<Implementation::CameraCommon*>( baseCam );
		if( !cam ) { mLastScope = Dirty_Camera; return true; }   // H2-S3: skeleton camera no-op, keep Apply's scope
		ApplyCameraOpForward( *cam, edit, SceneScale() );
		cam->RegenerateData();
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AddCamera )
	{
		// Re-create the cloned camera from the captured snapshot.
		// Mirrors Apply: deterministic recreation even if the source
		// has changed in the interim.
		if( !mJob ) return false;
		if( !CameraIntrospection::AddCameraFromSnapshot( *mJob, edit.objectName, edit.cameraSnapshot ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor: AddCamera failed for `%s` (duplicate name or unknown type)",
				edit.objectName.c_str() );   // H2-S3: restore diagnostic lost in the Apply split
			return false;
		}
		mJob->SetActiveCamera( edit.objectName.c_str() );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetMaterialProperty )
	{
		const bool ok = ApplyMaterialSlotByName( edit, edit.propertyValue );
		if( ok ) mLastScope = Dirty_Camera;
		return ok;
	}

	if( edit.op == SceneEdit::SetLightProperty )
	{
		ILightManager* lights = const_cast<ILightManager*>( mScene->GetLights() );
		if( !lights ) return false;
		ILightPriv* light = lights->GetItem( edit.objectName.c_str() );
		if( !light ) return false;
		// P5 Slice 3 expansion: CST-route the light edit (incl. shootphotons -- the re-derive applies it from
		// the chunk param) so the Document stays complete; a later material D2 then can't revert this edit.
		if( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) {
			if( !RouteCstParamEdit_( edit.objectName.c_str(), "light", edit.propertyName.c_str(), edit.propertyValue.c_str() ) ) return false;
			mLastScope = Dirty_Camera;
			return true;
		}
		// shootphotons re-replays through the direct setter to match
		// the Apply / Undo paths.
		if( edit.propertyName == String( "shootphotons" ) ) {
			bool newVal = false;
			if( !ParseLenientBool( edit.propertyValue, newVal ) ) {   // H2-S3: surface parse failure so Apply can reject
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: shootphotons edit rejected -- `%s` is not a recognised boolean (try true/false/yes/no/1/0)",
					edit.propertyValue.c_str() );   // H2-S3: restore diagnostic lost in the Apply split
				return false;
			}
			light->SetCanGeneratePhotons( newVal );
			mLastScope = Dirty_Camera;
			return true;
		}
		// Translate chunk-name → keyframe-name before dispatching:
		// the panel surfaces chunk vocabulary (power/inner/outer)
		// while ILight::KeyframeFromParameters expects keyframe
		// vocabulary (energy/inner_angle/outer_angle).
		IKeyframeParameter* p = light->KeyframeFromParameters(
			ChunkNameToKeyframeName( edit.propertyName ), edit.propertyValue );
		if( !p ) return false;
		light->SetIntermediateValue( *p );
		safe_release( p );
		light->RegenerateData();
		BumpSceneLightGeneration();   // #2b(a): rebuild caster samplers next render
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetMediumProperty )
	{
		// Re-apply propertyValue (the post-edit value).  Same dispatch
		// shape as the Undo branch but using the new value instead of
		// the prev one.
		if( !mJob ) return false;
		const IMedium* medConst = mJob->GetMedium( edit.objectName.c_str() );
		if( !medConst ) return false;
		IMedium* medium = const_cast<IMedium*>( medConst );
		if( !ApplyMediumPropertyValue( *medium, edit.propertyName, edit.propertyValue ) ) return false;   // H2-S3: surface failure so Apply can reject
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		mScene->SetSceneTimeForPreview( edit.s );
		mLastSetTime = edit.s;
		mLastScope = mScenePhotonsExist ? Dirty_TimeAndPhotons : Dirty_Time;
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = ResolveEditedCamera( edit );
		if( !baseCam ) return false;
		if( !CameraIntrospection::SetProperty( *baseCam, edit.objectName, edit.propertyValue ) ) return false;   // H2-S3: surface parse failure so Apply can reject
		mLastScope = Dirty_Camera;
		return true;
	}

	if( SceneEdit::IsCompositeMarker( edit.op ) )
	{
		return true;
	}

	// Unreachable for any valid SceneEdit::Op (every op is handled above).
	// A new op MUST add a branch here + in the sibling dispatcher + CaptureForApply;
	// this defensive default no-ops it as a camera-scope edit rather than crash.
	mLastScope = Dirty_Camera;
	return true;
}

bool SceneEditor::Redo()
{
	DirtyChangeNotifier _notifier( this );
	SceneEdit edit;
	if( !mHistory.PopForRedo( edit ) ) return false;

	// Phase B: re-mark the (single-edit) entity dirty on redo.
	// Composite inner edits are marked inside the replay loop.
	MarkEditEntityDirty( edit );

	if( edit.op == SceneEdit::CompositeBegin )
	{
		bool sawObjectOp = false, sawCameraOp = false, sawTimeOp = false, sawPropertyOp = false;
		int  depth     = 1;   // the CompositeBegin trigger opened one level (forward walk).
		int  undoMoves = 1;   // records moved redo->undo so far (incl. the trigger), for restore-on-failure.
		bool failed    = false;
		std::vector<SceneEdit> applied;   // inners successfully forward-applied, for revert roll-back
		while( true )
		{
			SceneEdit inner;
			if( !mHistory.PopForRedo( inner ) ) break;
			++undoMoves;
			// P1: nesting-aware -- a nested CompositeBegin opens a deeper level going
			// forward; only the matching OUTER CompositeEnd (depth 0) ends the replay.
			if( inner.op == SceneEdit::CompositeBegin ) { ++depth; continue; }
			if( inner.op == SceneEdit::CompositeEnd )   { if( --depth == 0 ) break; continue; }
			MarkEditEntityDirty( inner );
			if( !ApplyForwardMutation( inner ) ) { failed = true; break; }   // P1: stop + roll back atomically
			applied.push_back( inner );
			if( SceneEdit::IsObjectOp( inner.op ) )                                          sawObjectOp = true;
			else if( SceneEdit::IsCameraOp( inner.op ) || inner.op == SceneEdit::AddCamera ) sawCameraOp = true;
			else if( inner.op == SceneEdit::SetSceneTime )                                   sawTimeOp = true;
			else                                                                             sawPropertyOp = true;
		}
		if( failed )
		{
			// P1 (symmetric to Undo): re-revert what we applied (reverse forward order),
			// then move the whole popped group back undo->redo so the composite is intact
			// + retryable.  Rollback re-reverts are best-effort.
			for( std::vector<SceneEdit>::reverse_iterator it = applied.rbegin(); it != applied.rend(); ++it ) {
				ApplyRevertMutation( *it );
			}
			for( int k = 0; k < undoMoves; ++k ) mHistory.RestoreLastRedoFromUndo();
			mLastScope = Dirty_None;
			return false;
		}
		mLastScope = AggregateCompositeScope( sawObjectOp, sawCameraOp, sawTimeOp, sawPropertyOp );
		return true;
	}

	// Single edit -> the shared forward dispatcher.  P1 (symmetric to Undo): PopForRedo
	// already moved this edit to the undo stack.  If the forward mutation FAILS (e.g.
	// the redo's binding target vanished after capture), restore it to the redo stack --
	// a failed redo must NOT advance the depth or leave a phantom no-op edit undoable.
	if( !ApplyForwardMutation( edit ) ) {
		mHistory.RestoreLastRedoFromUndo();
		return false;
	}
	return true;
}
void SceneEditor::BeginComposite( const char* label )
{
	SceneEdit e;
	e.op = SceneEdit::CompositeBegin;
	if( label ) e.objectName = label;
	Apply( e );
}

void SceneEditor::EndComposite()
{
	// Defensive: a stray EndComposite (no matching Begin, or a tool
	// that switched mid-drag and called End on cleanup) would push
	// an orphan marker that confuses Undo (the loop would treat the
	// orphan as a composite boundary and walk back through unrelated
	// history).  Early-return rather than push a CompositeEnd at
	// depth 0.
	if( mCompositeDepth <= 0 ) return;
	SceneEdit e;
	e.op = SceneEdit::CompositeEnd;
	Apply( e );
}
