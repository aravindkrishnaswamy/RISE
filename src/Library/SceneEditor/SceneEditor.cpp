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
#include "../Interfaces/IEnumCallback.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Cameras/CameraCommon.h"
#include <cmath>

using namespace RISE;

SceneEditor::SceneEditor( IScenePriv& scene )
: mScene( scene )
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

	// Rodrigues' rotation: rotate v around unit axis k by angle theta.
	//   v_rot = v cos t + (k x v) sin t + k (k . v) (1 - cos t)
	static Vector3 RotateAroundAxis( const Vector3& v, const Vector3& k, Scalar theta )
	{
		const Scalar c = std::cos( theta );
		const Scalar s = std::sin( theta );
		const Scalar dotKV = k.x * v.x + k.y * v.y + k.z * v.z;
		const Scalar oneMc = 1.0 - c;

		Vector3 cross;
		cross.x = k.y * v.z - k.z * v.y;
		cross.y = k.z * v.x - k.x * v.z;
		cross.z = k.x * v.y - k.y * v.x;

		Vector3 r;
		r.x = v.x * c + cross.x * s + k.x * dotKV * oneMc;
		r.y = v.y * c + cross.y * s + k.y * dotKV * oneMc;
		r.z = v.z * c + cross.z * s + k.z * dotKV * oneMc;
		return r;
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

		// Capture state BEFORE mutation.
		edit.prevTransform = obj->GetFinalTransformMatrix();

		ApplyObjectOpForward( *obj, edit );
		RunObjectInvariantChain( *obj );
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
					RestoreObjectTransform( *obj, inner.prevTransform );
					RunObjectInvariantChain( *obj );
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
		RestoreObjectTransform( *obj, edit.prevTransform );
		RunObjectInvariantChain( *obj );
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
					RunObjectInvariantChain( *obj );
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
		RunObjectInvariantChain( *obj );
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
