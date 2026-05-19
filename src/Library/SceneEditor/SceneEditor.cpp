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
#include "CameraIntrospection.h"
#include "../Interfaces/IObjectPriv.h"
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
#include "../Interfaces/IKeyframable.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Cameras/CameraCommon.h"
#include <cmath>

using namespace RISE;

SceneEditor::SceneEditor( IScenePriv& scene )
: mScene( scene )
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
class BoundingBoxAccumulator : public IEnumCallback<IObject>
{
public:
	BoundingBox bbox;
	bool        hasAny;

	BoundingBoxAccumulator() : hasAny( false ) {}

	bool operator()( const IObject& obj ) override {
		const BoundingBox b = obj.getBoundingBox();
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
	if( const IObjectManager* objs = mScene.GetObjects() ) {
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
	return mScene.GetCausticPelMap()       != 0
	    || mScene.GetGlobalPelMap()        != 0
	    || mScene.GetTranslucentPelMap()   != 0
	    || mScene.GetCausticSpectralMap()  != 0
	    || mScene.GetGlobalSpectralMap()   != 0
	    || mScene.GetShadowMap()           != 0;
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
	const IObjectManager* objs = mScene.GetObjects();
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

void SceneEditor::ApplyObjectOpForward( IObjectPriv& obj, const SceneEdit& edit )
{
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
			if( mat ) obj.AssignMaterial( *mat );
		}
		break;
	case SceneEdit::SetObjectShader:
		if( mShaderManager ) {
			IShader* sh = mShaderManager->GetItem( edit.propertyValue.c_str() );
			if( sh ) obj.AssignShader( *sh );
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
		}
		break;
	default:
		// Caller guarantees IsObjectOp(edit.op) — this is a coding
		// error if reached, but we silently no-op rather than crash.
		break;
	}
}

void SceneEditor::RestoreObjectTransform( IObjectPriv& obj, const Matrix4& prev )
{
	// ITransformable composes its final matrix as
	//   m_mxFinalTrans = position * orientation * stretch * scale * stack
	// So to restore an arbitrary captured matrix we zero out every
	// component (ClearAllTransforms) and push the captured matrix
	// onto the stack.  After FinalizeTransformations, the final
	// matrix equals prev.
	obj.ClearAllTransforms();
	obj.PushTopTransStack( prev );
}

void SceneEditor::RunObjectInvariantChain( IObjectPriv& obj )
{
	obj.FinalizeTransformations();
	obj.ResetRuntimeData();
	const IObjectManager* objs = mScene.GetObjects();
	if( objs )
	{
		objs->InvalidateSpatialStructure();
	}
}

bool SceneEditor::Apply( const SceneEdit& editIn )
{
	SceneEdit edit = editIn;

	// Composite markers: just push, no mutation, no scope change.
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

	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;

		// Capture state BEFORE mutation.  Transform-shaped ops use
		// `prevTransform`; property-shaped ops (material / shader /
		// shadow flags) use `prevPropertyValue` (or `prevShadowFlags`
		// for the bit-packed bool pair) so the appropriate per-op
		// restorer in Undo can replay the prior state.
		//
		// EXCEPTION: `ScaleObjectFromAnchor` carries a controller-
		// supplied anchor matrix captured at drag-START — overwriting
		// it on every per-frame Apply would replace the anchor with
		// the previous frame's already-scaled state, so the cumulative
		// drag factor would compound across frames.  The controller
		// pre-populates `prevTransform` once at OnPointerDown; we keep
		// it through every frame of the drag.
		if( edit.op != SceneEdit::ScaleObjectFromAnchor ) {
			edit.prevTransform = obj->GetFinalTransformMatrix();
		}

		switch( edit.op ) {
		case SceneEdit::SetObjectMaterial:
			// Validate the new name resolves to a registered material
			// BEFORE pushing history.  Without this, a stale or
			// mistyped name would silently no-op in
			// ApplyObjectOpForward yet still report success and
			// poison the undo stack with a no-op entry.
			if( !mMaterialManager
			 || !mMaterialManager->GetItem( edit.propertyValue.c_str() ) )
			{
				return false;
			}
			edit.prevPropertyValue = FindManagerName( mMaterialManager, obj->GetMaterial() );
			break;
		case SceneEdit::SetObjectShader:
			// Same validation as material — prevent silent no-ops on
			// stale shader names.
			if( !mShaderManager
			 || !mShaderManager->GetItem( edit.propertyValue.c_str() ) )
			{
				return false;
			}
			edit.prevPropertyValue = FindManagerName( mShaderManager, obj->GetShader() );
			break;
		case SceneEdit::SetObjectShadowFlags:
			edit.prevShadowFlags = static_cast<Scalar>(
				( obj->DoesCastShadows()    ? 1 : 0 )
			  | ( obj->DoesReceiveShadows() ? 2 : 0 ) );
			break;
		case SceneEdit::SetObjectInteriorMedium:
			// Empty propertyValue OR the literal "none" = "clear the
			// interior medium" (parser-parity sentinel; see
			// SceneEdit.h doc).  Non-"none" non-empty must resolve to
			// a registered medium.  Validate BEFORE pushing history —
			// same rationale as material / shader: silently no-op'ing
			// a stale/mistyped name would still push a poison undo
			// entry.
			if( edit.propertyValue.size() > 1 && edit.propertyValue != String( "none" ) ) {
				if( !mJob || !mJob->GetMedium( edit.propertyValue.c_str() ) ) {
					GlobalLog()->PrintEx( eLog_Warning,
						"SceneEditor: interior_medium edit rejected — `%s` is not a registered medium",
						edit.propertyValue.c_str() );
					return false;
				}
			}
			// Reverse-lookup the prior medium for undo.  Empty result
			// = "no medium was bound"; undo's ClearInteriorMedium path
			// handles that round-trip cleanly.
			edit.prevPropertyValue = FindMediumName( mJob, obj->GetInteriorMedium() );
			break;
		default:
			break;
		}

		ApplyObjectOpForward( *obj, edit );
		// Property-style ops don't need the spatial-structure rebuild
		// that transform changes do.  Run the invariant chain only
		// for transform ops to avoid an unnecessary BSP invalidation
		// per material/shader/shadow click.
		const bool isTransformOp =
			edit.op == SceneEdit::TranslateObject
		 || edit.op == SceneEdit::RotateObjectArb
		 || edit.op == SceneEdit::SetObjectPosition
		 || edit.op == SceneEdit::SetObjectOrientation
		 || edit.op == SceneEdit::SetObjectScale
		 || edit.op == SceneEdit::SetObjectStretch
		 || edit.op == SceneEdit::ScaleObjectFromAnchor;
		if( isTransformOp ) {
			RunObjectInvariantChain( *obj );
		}
		mHistory.Push( edit );
		mLastScope = Dirty_ObjectTransform;
		return true;
	}

	if( SceneEdit::IsCameraOp( edit.op ) )
	{
		ICamera* baseCam = mScene.GetCameraMutable();
		if( !baseCam ) return false;

		// All standard concrete cameras (PinholeCamera, ThinLensCamera,
		// FisheyeCamera, OrthographicCamera) inherit from CameraCommon.
		// External-implementor cameras would fall back to no-op which
		// is consistent with skeleton-mode behaviour.
		Implementation::CameraCommon* cam =
			dynamic_cast<Implementation::CameraCommon*>( baseCam );
		if( !cam )
		{
			mLastScope = Dirty_Camera;
			mHistory.Push( edit );
			return true;
		}

		// Capture state BEFORE mutation, for undo.  Record everything
		// regardless of which op this is — most ops touch only one of
		// these but capturing all five is cheap (a few words) and
		// keeps Undo's restore path uniform.
		//
		// Use GetRestLocation() (vPosition) for prevCameraPos — NOT
		// GetLocation() (post-orbit frame.origin).  RestoreCameraTransform
		// passes prevCameraPos to SetLocation, which writes vPosition;
		// if we captured the post-orbit position and wrote it back as
		// vPosition, Recompute would re-apply target_orientation on
		// top of an already-orbited point, breaking undo whenever the
		// camera has non-zero target_orientation.
		edit.prevCameraPos          = cam->GetRestLocation();
		edit.prevCameraLookAt       = cam->GetStoredLookAt();
		edit.prevCameraUp           = cam->GetStoredUp();
		edit.prevCameraTargetOrient = cam->GetTargetOrientation();
		edit.prevCameraOrient       = cam->GetEulerOrientation();

		ApplyCameraOpForward( *cam, edit, SceneScale() );
		cam->RegenerateData();

		mLastScope = Dirty_Camera;
		mHistory.Push( edit );
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		// Capture previous time from our local cache (IScene does not
		// expose GetSceneTime); apply the new time and update the cache.
		edit.prevTime = mLastSetTime;
		mScene.SetSceneTimeForPreview( edit.s );
		mLastSetTime = edit.s;
		mLastScope = mScenePhotonsExist ? Dirty_TimeAndPhotons : Dirty_Time;
		mHistory.Push( edit );
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = mScene.GetCameraMutable();
		if( !baseCam ) return false;

		// Capture the prev value through the same formatter the
		// panel uses for display, so undo round-trips losslessly
		// through CameraIntrospection::SetProperty.
		edit.prevPropertyValue = CameraIntrospection::GetPropertyValue( *baseCam, edit.objectName );

		if( !CameraIntrospection::SetProperty( *baseCam, edit.objectName, edit.propertyValue ) )
		{
			return false;   // parse failure or read-only
		}
		mLastScope = Dirty_Camera;
		mHistory.Push( edit );
		return true;
	}

	if( edit.op == SceneEdit::AddCamera )
	{
		// objectName carries the NEW camera's name.  cameraSnapshot
		// carries the full source-camera state at Apply time.
		// prevPropertyValue captures the previously-active camera's
		// name so undo can restore the prior selection (Add*Camera
		// auto-promotes the new camera to active under the "last
		// added wins" policy, so without the capture we wouldn't
		// know which camera was active before the clone).
		if( !mJob ) return false;
		if( edit.objectName.size() <= 1 ) return false;

		edit.prevPropertyValue = String( mJob->GetActiveCameraName().c_str() );

		if( !CameraIntrospection::AddCameraFromSnapshot( *mJob, edit.objectName, edit.cameraSnapshot ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor: AddCamera failed for `%s` (duplicate name or unknown type)",
				edit.objectName.c_str() );
			return false;
		}
		// Add*Camera auto-promoted the new camera; ensure the active
		// matches `edit.objectName` for the caller's expectation.
		mJob->SetActiveCamera( edit.objectName.c_str() );

		mLastScope = Dirty_Camera;
		mHistory.Push( edit );
		return true;
	}

	if( edit.op == SceneEdit::SetMaterialProperty )
	{
		// objectName carries the material's manager-registered name;
		// propertyName the slot identifier ("reflectance", "ior",
		// etc.); propertyValue the painter name (IPainter or
		// IScalarPainter depending on slot type).  Caller (the
		// controller's Material SetProperty branch) is responsible
		// for cancel-and-park around this call — painter rebinds
		// release the prior painter, which could free a painter a
		// worker is mid-sample on.
		if( !mMaterialManager ) return false;
		IMaterial* mat = mMaterialManager->GetItem( edit.objectName.c_str() );
		if( !mat ) return false;

		// Composed materials reject up-front — see SceneEdit.h doc
		// + IJob::IsMaterialComposed.  Without this gate, the
		// MaterialIntrospection::SetSlot dispatch would succeed
		// even though the rebinding would break the painter graph
		// the composition relies on.
		if( mJob && mJob->IsMaterialComposed( edit.objectName.c_str() ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor: SetMaterialProperty rejected on `%s` — composed material "
				"(PBR-MR / GGX-Emissive); rebinding a slot would break the painter graph. "
				"Edit upstream painters instead.",
				edit.objectName.c_str() );
			return false;
		}

		// Resolve the slot's pipe (IPainter vs IScalarPainter) by
		// asking MaterialIntrospection what it expects.  The two
		// pipes are NOT interchangeable per CLAUDE.md's IScalarPainter
		// section — binding an IPainter into an IScalarPainter slot
		// would silently JH-uplift inline-numeric values, breaking
		// physical-scalar inputs.
		const MaterialSlotRef cur = MaterialIntrospection::GetSlot( *mat, edit.propertyName );
		if( cur.kind == MaterialSlotRef::None ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor: SetMaterialProperty rejected — `%s` has no slot named `%s`",
				edit.objectName.c_str(), edit.propertyName.c_str() );
			return false;
		}

		// Resolve the new painter name through the appropriate
		// manager.  Capture prev binding via reverse-lookup BEFORE
		// the mutation so undo can replay losslessly.  CRITICAL:
		// reject the edit if the prior binding has no recoverable
		// manager-registered name — an empty prev would push an
		// undo entry that silently no-ops on the way back, leaving
		// the user with no way to revert.  The GUI's introspection
		// layer (MaterialIntrospection::BuildPainterSlot) already
		// marks unregistered slots read-only so the GUI path can't
		// reach here, but programmatic callers via
		// RISE_API_SceneEditController_SetPropertyForCategory can
		// still trigger this gate.
		const IPainter*       newPainter       = 0;
		const IScalarPainter* newScalarPainter = 0;
		if( cur.kind == MaterialSlotRef::Painter ) {
			if( !mPainterManager ) return false;
			newPainter = mPainterManager->GetItem( edit.propertyValue.c_str() );
			if( !newPainter ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty rejected — painter `%s` is not registered",
					edit.propertyValue.c_str() );
				return false;
			}
			// Reverse-lookup the prior painter's name for undo.
			edit.prevPropertyValue = FindManagerName( mPainterManager, cur.painter );
			if( edit.prevPropertyValue.size() <= 1 ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty on `%s.%s` rejected — "
					"current painter has no manager-registered name, so undo "
					"cannot restore the prior binding.  Register the painter "
					"with IPainterManager before editing the slot.",
					edit.objectName.c_str(), edit.propertyName.c_str() );
				return false;
			}
		} else {  // ScalarPainter
			if( !mScalarPainterManager ) return false;
			newScalarPainter = mScalarPainterManager->GetItem( edit.propertyValue.c_str() );
			if( !newScalarPainter ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty rejected — scalar_painter `%s` is not registered",
					edit.propertyValue.c_str() );
				return false;
			}
			edit.prevPropertyValue = FindManagerName( mScalarPainterManager, cur.scalarPainter );
			if( edit.prevPropertyValue.size() <= 1 ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: SetMaterialProperty on `%s.%s` rejected — "
					"current scalar_painter has no manager-registered name, "
					"so undo cannot restore the prior binding.  Register the "
					"scalar_painter with IScalarPainterManager before editing.",
					edit.objectName.c_str(), edit.propertyName.c_str() );
				return false;
			}
		}

		if( !MaterialIntrospection::SetSlot( *mat, edit.propertyName, newPainter, newScalarPainter ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor: SetMaterialProperty internal dispatch failed for `%s.%s`",
				edit.objectName.c_str(), edit.propertyName.c_str() );
			return false;
		}

		mLastScope = Dirty_Camera;   // no spatial-structure invalidation
		mHistory.Push( edit );
		return true;
	}

	if( edit.op == SceneEdit::SetLightProperty )
	{
		// objectName carries the light's manager name; propertyName
		// the property identifier ("position" / "energy" / etc.).
		ILightManager* lights = const_cast<ILightManager*>( mScene.GetLights() );
		if( !lights ) return false;
		ILightPriv* light = lights->GetItem( edit.objectName.c_str() );
		if( !light ) return false;

		// `shootphotons` is a non-keyframable bool — the keyframe
		// machinery has no parameter for it, so dispatch to the
		// dedicated ILight::SetCanGeneratePhotons setter instead.
		// Capture prev as "true"/"false" so undo replays through the
		// same path.  No RegenerateData call: the flag is read lazily
		// during the next photon-mapping pass; nothing to bake.
		if( edit.propertyName == String( "shootphotons" ) ) {
			edit.prevPropertyValue = String( light->CanGeneratePhotons() ? "true" : "false" );
			bool newVal = false;
			if( !ParseLenientBool( edit.propertyValue, newVal ) ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor: shootphotons edit rejected — `%s` is not a recognised boolean (try true/false/yes/no/1/0)",
					edit.propertyValue.c_str() );
				return false;
			}
			light->SetCanGeneratePhotons( newVal );
			mLastScope = Dirty_Camera;
			mHistory.Push( edit );
			return true;
		}

		// Capture prev value through the same readback the introspection
		// panel uses, so undo replays losslessly via the keyframe path.
		edit.prevPropertyValue = ReadLightProperty( *light, edit.propertyName );

		// Translate chunk-name → keyframe-name before dispatching:
		// the panel surfaces chunk vocabulary (power/inner/outer)
		// while ILight::KeyframeFromParameters expects keyframe
		// vocabulary (energy/inner_angle/outer_angle).
		IKeyframeParameter* p = light->KeyframeFromParameters(
			ChunkNameToKeyframeName( edit.propertyName ), edit.propertyValue );
		if( !p ) return false;
		light->SetIntermediateValue( *p );
		safe_release( p );
		// SetIntermediateValue stages new field values; flush to the
		// final transform / runtime state before the next render.
		light->RegenerateData();

		mLastScope = Dirty_Camera;   // no spatial structure invalidation; same scope as camera
		mHistory.Push( edit );
		return true;
	}

	if( edit.op == SceneEdit::SetMediumProperty )
	{
		// objectName carries the medium's manager name; propertyName
		// the slot identifier ("absorption" / "scattering" /
		// "emission"); propertyValue the "r g b" string.  Caller
		// (controller's Medium SetProperty branch) handles the
		// cancel-and-park gate — setters re-derive sigma_t cache
		// which is read by distance-sampling workers.
		if( !mJob ) return false;
		const IMedium* medConst = mJob->GetMedium( edit.objectName.c_str() );
		if( !medConst ) return false;
		IMedium* medium = const_cast<IMedium*>( medConst );

		// Capture prev BEFORE mutating so undo replays losslessly.
		// Empty prev means the slot/medium combo isn't supported
		// (e.g. HeterogeneousMedium "absorption") — reject up-front
		// rather than push a phantom edit.
		edit.prevPropertyValue = ReadMediumProperty( *medium, edit.propertyName );
		if( edit.prevPropertyValue.size() <= 1 ) return false;

		if( !ApplyMediumPropertyValue( *medium, edit.propertyName, edit.propertyValue ) ) {
			return false;
		}

		mLastScope = Dirty_Camera;   // medium edit affects rendering but no spatial reseed
		mHistory.Push( edit );
		return true;
	}

	return false;
}

bool SceneEditor::Undo()
{
	SceneEdit edit;
	if( !mHistory.PopForUndo( edit ) ) return false;

	// Walk back through composite groups: if the popped entry is a
	// CompositeEnd marker, repeatedly undo until and including the
	// matching CompositeBegin.
	if( edit.op == SceneEdit::CompositeEnd )
	{
		// The CompositeEnd has already moved to the redo stack (good).
		// Now pop and invert each entry until we hit the matching
		// CompositeBegin (also good — moves to redo stack).
		Implementation::CameraCommon* cam = 0;
		bool sawObjectOp     = false;
		bool sawCameraOp     = false;
		bool sawTimeOp       = false;
		bool sawPropertyOp   = false;
		while( true )
		{
			SceneEdit inner;
			if( !mHistory.PopForUndo( inner ) ) break;
			if( inner.op == SceneEdit::CompositeBegin ) break;
			// Undo the inner edit.
			if( SceneEdit::IsObjectOp( inner.op ) )
			{
				IObjectPriv* obj = FindObject( inner.objectName );
				if( obj )
				{
					switch( inner.op ) {
					case SceneEdit::SetObjectMaterial:
						if( mMaterialManager && inner.prevPropertyValue.size() > 1 ) {
							IMaterial* mat = mMaterialManager->GetItem( inner.prevPropertyValue.c_str() );
							if( mat ) obj->AssignMaterial( *mat );
						}
						break;
					case SceneEdit::SetObjectShader:
						if( mShaderManager && inner.prevPropertyValue.size() > 1 ) {
							IShader* sh = mShaderManager->GetItem( inner.prevPropertyValue.c_str() );
							if( sh ) obj->AssignShader( *sh );
						}
						break;
					case SceneEdit::SetObjectShadowFlags: {
						const int flags = static_cast<int>( inner.prevShadowFlags );
						obj->SetShadowParams( ( flags & 1 ) != 0, ( flags & 2 ) != 0 );
						break;
					}
					case SceneEdit::SetObjectInteriorMedium:
						if( inner.prevPropertyValue.size() <= 1 ) {
							obj->ClearInteriorMedium();
						} else if( mJob ) {
							const IMedium* med = mJob->GetMedium( inner.prevPropertyValue.c_str() );
							if( med ) obj->AssignInteriorMedium( *med );
						}
						break;
					default:
						RestoreObjectTransform( *obj, inner.prevTransform );
						RunObjectInvariantChain( *obj );
						break;
					}
				}
				sawObjectOp = true;
			}
			else if( SceneEdit::IsCameraOp( inner.op ) )
			{
				if( !cam )
				{
					ICamera* baseCam = mScene.GetCameraMutable();
					cam = baseCam ? dynamic_cast<Implementation::CameraCommon*>( baseCam ) : 0;
				}
				if( cam )
				{
					RestoreCameraTransform( *cam, inner );
				}
				sawCameraOp = true;
			}
			else if( inner.op == SceneEdit::SetSceneTime )
			{
				// The composite's first SetSceneTime captured prevTime
				// from mLastSetTime (the pre-composite value).  Walking
				// back inner-edits LIFO means the LAST iteration here
				// is the first SetSceneTime in the composite — its
				// prevTime is the canonical "before-composite" value.
				mScene.SetSceneTimeForPreview( inner.prevTime );
				mLastSetTime = inner.prevTime;
				sawTimeOp = true;
			}
			else if( inner.op == SceneEdit::SetCameraProperty )
			{
				// Defer RegenerateData: the trailing cam->RegenerateData()
				// below covers it.  Use a per-name scratch path that
				// updates the camera fields without re-baking the basis
				// matrix on every iteration.
				ICamera* baseCam = mScene.GetCameraMutable();
				if( baseCam )
				{
					CameraIntrospection::SetProperty( *baseCam,
						inner.objectName, inner.prevPropertyValue );
				}
				sawPropertyOp = true;
			}
			else if( inner.op == SceneEdit::SetLightProperty )
			{
				// Mirror the single-edit Undo path: shootphotons through
				// the direct setter, everything else through keyframe.
				ILightManager* lights = const_cast<ILightManager*>( mScene.GetLights() );
				ILightPriv* light = lights ? lights->GetItem( inner.objectName.c_str() ) : 0;
				if( light ) {
					if( inner.propertyName == String( "shootphotons" ) ) {
						bool prevVal = false;
						ParseLenientBool( inner.prevPropertyValue, prevVal );
						light->SetCanGeneratePhotons( prevVal );
					} else {
						IKeyframeParameter* p = light->KeyframeFromParameters(
							ChunkNameToKeyframeName( inner.propertyName ), inner.prevPropertyValue );
						if( p ) {
							light->SetIntermediateValue( *p );
							safe_release( p );
							light->RegenerateData();
						}
					}
				}
				sawPropertyOp = true;
			}
			else if( inner.op == SceneEdit::SetMediumProperty )
			{
				if( mJob ) {
					const IMedium* medConst = mJob->GetMedium( inner.objectName.c_str() );
					if( medConst && inner.prevPropertyValue.size() > 1 ) {
						IMedium* medium = const_cast<IMedium*>( medConst );
						ApplyMediumPropertyValue( *medium, inner.propertyName, inner.prevPropertyValue );
					}
				}
				sawPropertyOp = true;
			}
			else if( inner.op == SceneEdit::AddCamera )
			{
				if( mJob ) {
					mJob->RemoveCamera( inner.objectName.c_str() );
					if( inner.prevPropertyValue.size() > 1 ) {
						mJob->SetActiveCamera( inner.prevPropertyValue.c_str() );
					}
				}
				sawCameraOp = true;
			}
		}
		if( cam ) cam->RegenerateData();

		// Pick the most-significant scope contained in the composite
		// so the controller's downstream invalidation logic gets the
		// right signal.  Object > Time-with-photons > Time > Camera.
		if( sawObjectOp )                          mLastScope = Dirty_ObjectTransform;
		else if( sawTimeOp && mScenePhotonsExist ) mLastScope = Dirty_TimeAndPhotons;
		else if( sawTimeOp )                       mLastScope = Dirty_Time;
		else if( sawCameraOp || sawPropertyOp )    mLastScope = Dirty_Camera;
		else                                       mLastScope = Dirty_None;
		return true;
	}

	// Single edit (not part of a composite).
	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;

		// Property-style ops restore from per-op prev fields; transform
		// ops restore the captured matrix.  Switch on op so each path
		// runs only the work it needs.
		switch( edit.op ) {
		case SceneEdit::SetObjectMaterial:
			if( mMaterialManager && edit.prevPropertyValue.size() > 1 ) {
				IMaterial* mat = mMaterialManager->GetItem( edit.prevPropertyValue.c_str() );
				if( mat ) obj->AssignMaterial( *mat );
			}
			break;
		case SceneEdit::SetObjectShader:
			if( mShaderManager && edit.prevPropertyValue.size() > 1 ) {
				IShader* sh = mShaderManager->GetItem( edit.prevPropertyValue.c_str() );
				if( sh ) obj->AssignShader( *sh );
			}
			break;
		case SceneEdit::SetObjectShadowFlags: {
			const int flags = static_cast<int>( edit.prevShadowFlags );
			obj->SetShadowParams( ( flags & 1 ) != 0, ( flags & 2 ) != 0 );
			break;
		}
		case SceneEdit::SetObjectInteriorMedium:
			if( edit.prevPropertyValue.size() <= 1 ) {
				// Empty prev = object had no medium before; restore
				// "no medium" state via ClearInteriorMedium.
				obj->ClearInteriorMedium();
			} else if( mJob ) {
				const IMedium* med = mJob->GetMedium( edit.prevPropertyValue.c_str() );
				if( med ) obj->AssignInteriorMedium( *med );
			}
			break;
		default:
			// Transform op.
			RestoreObjectTransform( *obj, edit.prevTransform );
			RunObjectInvariantChain( *obj );
			break;
		}
		mLastScope = Dirty_ObjectTransform;
		return true;
	}

	if( SceneEdit::IsCameraOp( edit.op ) )
	{
		ICamera* baseCam = mScene.GetCameraMutable();
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
		mScene.SetSceneTimeForPreview( edit.prevTime );
		mLastSetTime = edit.prevTime;
		mLastScope = mScenePhotonsExist ? Dirty_TimeAndPhotons : Dirty_Time;
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = mScene.GetCameraMutable();
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
		// Inverse: rebind the slot to the captured prev painter
		// name.  If prev is empty (e.g. the slot was previously
		// bound to a painter that wasn't registered in the
		// manager — unusual but possible for in-process-only
		// painters), the undo silently no-ops since we have no
		// painter pointer to restore.
		if( !mMaterialManager ) return false;
		IMaterial* mat = mMaterialManager->GetItem( edit.objectName.c_str() );
		if( !mat ) return false;
		if( edit.prevPropertyValue.size() <= 1 ) return true;

		// Re-resolve the slot's pipe to pick the right manager for
		// the prev-name lookup.  (We don't store the pipe kind in
		// the SceneEdit struct; the material's slot type is stable
		// across the op, so re-querying is reliable.)
		const MaterialSlotRef cur = MaterialIntrospection::GetSlot( *mat, edit.propertyName );
		if( cur.kind == MaterialSlotRef::Painter && mPainterManager ) {
			const IPainter* prev = mPainterManager->GetItem( edit.prevPropertyValue.c_str() );
			if( prev ) MaterialIntrospection::SetSlot( *mat, edit.propertyName, prev, 0 );
		} else if( cur.kind == MaterialSlotRef::ScalarPainter && mScalarPainterManager ) {
			const IScalarPainter* prev = mScalarPainterManager->GetItem( edit.prevPropertyValue.c_str() );
			if( prev ) MaterialIntrospection::SetSlot( *mat, edit.propertyName, 0, prev );
		}
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetLightProperty )
	{
		ILightManager* lights = const_cast<ILightManager*>( mScene.GetLights() );
		if( !lights ) return false;
		ILightPriv* light = lights->GetItem( edit.objectName.c_str() );
		if( !light ) return false;
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

	mLastScope = Dirty_Camera;
	return true;
}

bool SceneEditor::Redo()
{
	SceneEdit edit;
	if( !mHistory.PopForRedo( edit ) ) return false;

	if( edit.op == SceneEdit::CompositeBegin )
	{
		// Re-apply the composite group: pop forwards through
		// the redo stack until matching CompositeEnd.
		Implementation::CameraCommon* cam = 0;
		bool sawObjectOp = false, sawCameraOp = false, sawTimeOp = false, sawPropertyOp = false;
		while( true )
		{
			SceneEdit inner;
			if( !mHistory.PopForRedo( inner ) ) break;
			if( inner.op == SceneEdit::CompositeEnd ) break;
			if( SceneEdit::IsObjectOp( inner.op ) )
			{
				IObjectPriv* obj = FindObject( inner.objectName );
				if( obj )
				{
					ApplyObjectOpForward( *obj, inner );
					// Spatial structure only needs invalidating for
					// transform ops; property-style ops (material /
					// shader / shadow / interior_medium) leave the
					// bbox alone.  Symmetric with Apply()'s gate.
					const bool isTransformOp =
						inner.op == SceneEdit::TranslateObject
					 || inner.op == SceneEdit::RotateObjectArb
					 || inner.op == SceneEdit::SetObjectPosition
					 || inner.op == SceneEdit::SetObjectOrientation
					 || inner.op == SceneEdit::SetObjectScale
					 || inner.op == SceneEdit::SetObjectStretch
					 || inner.op == SceneEdit::ScaleObjectFromAnchor;
					if( isTransformOp ) RunObjectInvariantChain( *obj );
				}
				sawObjectOp = true;
			}
			else if( SceneEdit::IsCameraOp( inner.op ) )
			{
				if( !cam )
				{
					ICamera* baseCam = mScene.GetCameraMutable();
					cam = baseCam ? dynamic_cast<Implementation::CameraCommon*>( baseCam ) : 0;
				}
				if( cam ) ApplyCameraOpForward( *cam, inner, SceneScale() );
				sawCameraOp = true;
			}
			else if( inner.op == SceneEdit::SetSceneTime )
			{
				mScene.SetSceneTimeForPreview( inner.s );
				mLastSetTime = inner.s;
				sawTimeOp = true;
			}
			else if( inner.op == SceneEdit::SetCameraProperty )
			{
				ICamera* baseCam = mScene.GetCameraMutable();
				if( baseCam )
				{
					CameraIntrospection::SetProperty( *baseCam,
						inner.objectName, inner.propertyValue );
				}
				sawPropertyOp = true;
			}
			else if( inner.op == SceneEdit::AddCamera )
			{
				if( mJob ) {
					CameraIntrospection::AddCameraFromSnapshot(
						*mJob, inner.objectName, inner.cameraSnapshot );
					mJob->SetActiveCamera( inner.objectName.c_str() );
				}
				sawCameraOp = true;
			}
			else if( inner.op == SceneEdit::SetLightProperty )
			{
				// Mirror the single-edit Redo path: shootphotons via
				// the direct setter, everything else via keyframe.
				ILightManager* lights = const_cast<ILightManager*>( mScene.GetLights() );
				ILightPriv* light = lights ? lights->GetItem( inner.objectName.c_str() ) : 0;
				if( light ) {
					if( inner.propertyName == String( "shootphotons" ) ) {
						bool newVal = false;
						ParseLenientBool( inner.propertyValue, newVal );
						light->SetCanGeneratePhotons( newVal );
					} else {
						IKeyframeParameter* p = light->KeyframeFromParameters(
							ChunkNameToKeyframeName( inner.propertyName ), inner.propertyValue );
						if( p ) {
							light->SetIntermediateValue( *p );
							safe_release( p );
							light->RegenerateData();
						}
					}
				}
				sawPropertyOp = true;
			}
			else if( inner.op == SceneEdit::SetMediumProperty )
			{
				if( mJob ) {
					const IMedium* medConst = mJob->GetMedium( inner.objectName.c_str() );
					if( medConst ) {
						IMedium* medium = const_cast<IMedium*>( medConst );
						ApplyMediumPropertyValue( *medium, inner.propertyName, inner.propertyValue );
					}
				}
				sawPropertyOp = true;
			}
		}
		if( cam ) cam->RegenerateData();
		if( sawObjectOp )                          mLastScope = Dirty_ObjectTransform;
		else if( sawTimeOp && mScenePhotonsExist ) mLastScope = Dirty_TimeAndPhotons;
		else if( sawTimeOp )                       mLastScope = Dirty_Time;
		else if( sawCameraOp || sawPropertyOp )    mLastScope = Dirty_Camera;
		else                                       mLastScope = Dirty_None;
		return true;
	}

	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;
		ApplyObjectOpForward( *obj, edit );
		// Property-style ops don't move geometry — symmetric with
		// Apply()'s isTransformOp gate.  Pre-Phase-1 this path ran
		// the chain unconditionally, costing a spurious BSP
		// invalidation per material/shader/shadow redo.
		const bool isTransformOp =
			edit.op == SceneEdit::TranslateObject
		 || edit.op == SceneEdit::RotateObjectArb
		 || edit.op == SceneEdit::SetObjectPosition
		 || edit.op == SceneEdit::SetObjectOrientation
		 || edit.op == SceneEdit::SetObjectScale
		 || edit.op == SceneEdit::SetObjectStretch
		 || edit.op == SceneEdit::ScaleObjectFromAnchor;
		if( isTransformOp ) RunObjectInvariantChain( *obj );
		mLastScope = Dirty_ObjectTransform;
		return true;
	}

	if( SceneEdit::IsCameraOp( edit.op ) )
	{
		ICamera* baseCam = mScene.GetCameraMutable();
		if( !baseCam ) return false;
		Implementation::CameraCommon* cam =
			dynamic_cast<Implementation::CameraCommon*>( baseCam );
		if( !cam ) return true;
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
			return false;
		}
		mJob->SetActiveCamera( edit.objectName.c_str() );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetMaterialProperty )
	{
		// Re-apply propertyValue (the post-edit binding).  Same
		// dispatch shape as the Undo branch but using the new
		// painter name instead of the prev one.
		if( !mMaterialManager ) return false;
		IMaterial* mat = mMaterialManager->GetItem( edit.objectName.c_str() );
		if( !mat ) return false;
		const MaterialSlotRef cur = MaterialIntrospection::GetSlot( *mat, edit.propertyName );
		if( cur.kind == MaterialSlotRef::Painter && mPainterManager ) {
			const IPainter* p = mPainterManager->GetItem( edit.propertyValue.c_str() );
			if( p ) MaterialIntrospection::SetSlot( *mat, edit.propertyName, p, 0 );
		} else if( cur.kind == MaterialSlotRef::ScalarPainter && mScalarPainterManager ) {
			const IScalarPainter* p = mScalarPainterManager->GetItem( edit.propertyValue.c_str() );
			if( p ) MaterialIntrospection::SetSlot( *mat, edit.propertyName, 0, p );
		}
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetLightProperty )
	{
		ILightManager* lights = const_cast<ILightManager*>( mScene.GetLights() );
		if( !lights ) return false;
		ILightPriv* light = lights->GetItem( edit.objectName.c_str() );
		if( !light ) return false;
		// shootphotons re-replays through the direct setter to match
		// the Apply / Undo paths.
		if( edit.propertyName == String( "shootphotons" ) ) {
			bool newVal = false;
			ParseLenientBool( edit.propertyValue, newVal );  // value was already validated by Apply
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
		ApplyMediumPropertyValue( *medium, edit.propertyName, edit.propertyValue );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		mScene.SetSceneTimeForPreview( edit.s );
		mLastSetTime = edit.s;
		mLastScope = mScenePhotonsExist ? Dirty_TimeAndPhotons : Dirty_Time;
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = mScene.GetCameraMutable();
		if( !baseCam ) return false;
		CameraIntrospection::SetProperty( *baseCam, edit.objectName, edit.propertyValue );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( SceneEdit::IsCompositeMarker( edit.op ) )
	{
		return true;
	}

	mLastScope = Dirty_Camera;
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
