//////////////////////////////////////////////////////////////////////
//
//  SceneEditor.cpp - Implementation of the SceneEditor mutator.
//    The invariant chain for a transform edit is:
//
//      1. Look up the IObjectPriv* via ObjectManager::GetItem(name)
//      2. Capture obj->GetLocalTransformMatrix() as prevTransform
//         (this is what we'll restore on undo).  LOCAL, not world: undo
//         must restore what the object itself carries, so that restoring
//         a child does not bake its parent's transform into it (87).
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
#include "../Objects/CSGObject.h"   // container-vs-CSG discrimination on the runtime binders
#include <vector>   // P1: atomic composite undo/redo rollback buffer
#include "CameraIntrospection.h"
#include "ChunkDescriptorRegistry.h"   // doc 88 S4: ClassifyCstEntityKind's painter arm consults the descriptor category
#include "ObjectIntrospection.h"
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
#include "../Utilities/FiniteMath.h"
#include "../Cameras/CameraCommon.h"
#include "../Scene.h"   // concrete Scene for the #2b(a) light-generation bump
#include <cmath>

using namespace RISE;

namespace {

struct DirtyCallbackFrame
{
	const void* state;
	DirtyCallbackFrame* previous;
};

thread_local DirtyCallbackFrame* gDirtyCallbackFrame = nullptr;

class DirtyCallbackScope
{
public:
	explicit DirtyCallbackScope( const void* state )
		: mFrame{ state, gDirtyCallbackFrame }
	{
		gDirtyCallbackFrame = &mFrame;
	}
	~DirtyCallbackScope() { gDirtyCallbackFrame = mFrame.previous; }
private:
	DirtyCallbackFrame mFrame;
};

bool IsInDirtyCallback( const void* state )
{
	for( DirtyCallbackFrame* frame = gDirtyCallbackFrame;
	     frame; frame = frame->previous )
	{
		if( frame->state == state ) return true;
	}
	return false;
}

}  // namespace

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

bool SceneEditor::ClassifyCstEntityKind( const std::string& kind, EntityCategory& outCategory )
{
	// Model-B F2 S3 fix round (P3-a): the ONE place that maps a CST
	// chunk-kind string to its EntityCategory -- see the header doc for
	// why this used to be two independently-maintained copies.
	auto endsWith = []( const std::string& s, const char* suffix ) {
		const std::string suf( suffix );
		return s.size() >= suf.size()
		    && s.compare( s.size() - suf.size(), suf.size(), suf ) == 0;
	};
	if( kind == "standard_object" ) {
		outCategory = EntityCategory::Object;
		return true;
	}
	if( kind == "camera" || endsWith( kind, "_camera" ) ) {
		outCategory = EntityCategory::Camera;
		return true;
	}
	if( endsWith( kind, "_light" ) ) {
		outCategory = EntityCategory::Light;
		return true;
	}
	if( kind == "material" || endsWith( kind, "_material" ) ) {
		outCategory = EntityCategory::Material;
		return true;
	}
	if( endsWith( kind, "_medium" ) ) {
		outCategory = EntityCategory::Medium;
		return true;
	}
	// doc 88 S4: painters.  Two arms, for two genuinely different callers.
	//
	// The BARE "painter" form is not defensive padding -- it is the form the
	// GUI property panel actually sends.  SceneEditController::
	// SetPropertyInner_'s Category::Painter arm calls ApplyAgentParamEditInner_
	// with entityKind = "painter", i.e. the ROLE-KIND SUFFIX that
	// DocFindByNameAnyRole narrows on, not a chunk keyword -- so a
	// suffix-only test misses EVERY panel-originated painter edit and drops it
	// back into the coarse CST-head boolean.  (Same shape as the `camera` and
	// `material` arms above, which accept their bare form for the same
	// reason.)  The "*_painter" suffix covers the ~30 real keywords, which is
	// what the AGENT surface passes.
	if( kind == "painter" || endsWith( kind, "_painter" ) ) {
		outCategory = EntityCategory::Painter;
		return true;
	}
	// ...and the DESCRIPTOR CATEGORY is the authority for a painter whose
	// keyword does not carry the suffix.  `expression_function2d` is
	// registered ChunkCategory::Painter and IS reachable as a painter through
	// every other surface (Cst::RoleMatchesKindConstraint, the
	// Category::Painter panel list), so stopping at the suffix would classify
	// it "unknown" -- the exact divergence CstIntrospection's own defensive
	// re-check comment warns about, one layer down.
	//
	// Deliberately NARROWER than Cst::RoleMatchesKindConstraint(role,
	// "painter"), which matches the Painter|Function UI UNION: a
	// `piecewise_linear_function` is an IFunction1D, never registered in
	// either painter manager, so tagging it Painter would name a dirty entity
	// that PainterIntrospection::PipesFor reports as "(not registered)".
	// Functions stay unrecognized and keep the conservative fallbacks their
	// callers already apply.
	if( !kind.empty() ) {
		if( const ChunkDescriptor* cd = DescriptorForKeyword( String( kind.c_str() ) ) ) {
			if( cd->category == ChunkCategory::Painter ) {
				outCategory = EntityCategory::Painter;
				return true;
			}
		}
	}
	return false;   // empty / unrecognized -- caller applies its own fallback policy
}

void SceneEditor::BumpSceneLightGenerationForAgentParamEdit(
	const char* entityName, const char* entityKind )
{
	// Shared-undo follow-up (P2 fix): see the doc comment in SceneEditor.h --
	// resolve material-ness from the CST chunk-kind string via the SAME
	// ClassifyCstEntityKind MarkCstHeadDirty's kind dispatch uses, then
	// bump iff the resolved material is emissive (matching
	// MarkEditEntityDirty's SetMaterialProperty arm, the GUI-path
	// counterpart this closes the gap with).  Empty/unrecognized kinds
	// bump CONSERVATIVELY -- a spurious bump costs one alias-table
	// rebuild; a missed bump on an actually-emissive target is a
	// rendering-correctness bug.
	const std::string kind = entityKind ? entityKind : std::string();
	EntityCategory category;
	const bool isKnown = ClassifyCstEntityKind( kind, category );
	if( isKnown && category == EntityCategory::Material )
	{
		if( mMaterialManager && entityName )
		{
			BumpSceneLightGenerationIfMaterialEmits(
				mMaterialManager->GetItem( entityName ) );
		}
	}
	else if( !isKnown || category == EntityCategory::Painter )
	{
		// Empty / unrecognized kind: bump conservatively (see the tradeoff
		// note above and in the header doc comment).
		//
		// doc 88 S4 -- PAINTER RIDES THE CONSERVATIVE ARM ON PURPOSE, and this
		// clause is the whole reason the S4 dirty-category change is not a
		// silent rendering regression.  Before S4 a painter kind was
		// UNRECOGNIZED, so it fell here and bumped.  Making it a KNOWN
		// category would otherwise have moved it to the "known but not
		// Material -> no bump" path -- and a painter is precisely the thing
		// that can change emission without being a material: an emissive
		// material's `exitance` slot is BOUND TO A PAINTER, so editing that
		// painter's `color` (or an expression painter's body, or a ramp's
		// input) changes Le while the material chunk itself never moves.
		// The LightSampler's alias-table weight is baked at Prepare() from
		// that emission footprint, so a missed bump leaves light SELECTION
		// biased toward the pre-edit brightness.  Resolving painter->material
		// emissiveness properly means walking every material's slots for a
		// binding to this painter name (and through intermediate painter
		// graphs -- blend/ramp/scalar chains), which is exactly the kind of
		// reverse-dependency walk the asymmetric tradeoff above exists to
		// avoid paying: one spurious alias-table rebuild is cheap, a stale
		// one is a correctness bug.
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
, mDirtyNotificationState( std::make_shared<DirtyNotificationState>() )
{
	mScenePhotonsExist = ComputeScenePhotonsExist();
}

SceneEditor::~SceneEditor()
{
	const std::shared_ptr<DirtyNotificationState> state = mDirtyNotificationState;
	state->ownerAlive.store( false, std::memory_order_release );
	CloseDirtyChangedListener();
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
	static bool HasNonFinite( const Point3& p )
	{
		return !RISE::IsFiniteDouble( p.x ) || !RISE::IsFiniteDouble( p.y ) || !RISE::IsFiniteDouble( p.z );
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
			if( HasNonFinite( newRest ) || HasNonFinite( newLook ) ) break;   // refuse non-finite propagation
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
			if( HasNonFinite( newRest ) ) break;   // refuse non-finite propagation
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
		if( e.prevCameraWasONB ) {
			cam.RestoreInteractiveONBPose(
				e.prevCameraPos, e.prevCameraONBU,
				e.prevCameraONBV, e.prevCameraONBW );
			return;
		}
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

//! 87: the ONE definition of "which standard_object params are surface
//! bindings a container cannot carry".  Exactly what the derive drops on a
//! container -- Job.cpp's DropContainerSurfaceBindings_ (material / modifier /
//! shader / radiance_map) plus the `interior_medium` the standard_object
//! parser skips separately.  Shared with SceneEditController's agent-commit
//! gate deliberately: two copies of this list is how the next binding param
//! ends up gated on one path and not the other.
namespace RISE {
bool IsObjectSurfaceBindingParamName( const String& param )
{
	static const char* kBindings[] = { "material", "modifier", "shader", "radiance_map", "interior_medium" };
	for( size_t i = 0; i < sizeof( kBindings ) / sizeof( kBindings[0] ); ++i ) {
		if( param == String( kBindings[i] ) ) return true;
	}
	return false;
}
}  // namespace RISE

namespace {

// The Cst param readers concatenate a value's TOKENS together with their
// separating / trailing trivia (Cst::ParamValueAsParsed's own doc says so),
// so a value can arrive with whitespace attached.  Compare -- and store --
// on the bare word.  Same shape as SceneEditController's TrimAsciiSpace_.
std::string TrimTrivia_( const std::string& s )
{
	size_t b = 0, e = s.size();
	while( b < e && ( s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n' ) ) ++b;
	while( e > b && ( s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n' ) ) --e;
	return s.substr( b, e - b );
}

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

Scalar FinalColumnLength_( const Matrix4& m, int column )
{
	const Scalar* v = &m._00 + column * 4;
	return std::sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
}

//! Replace an object's LOCAL transform -- what its authoring chunk stores --
//! with `m`.  Under 87 the object's WORLD matrix becomes `parentWorld * m` at
//! the next finalize; for an unparented object (parentWorld == identity) the
//! two are the same and this behaves exactly as it always did.
//!
//! Every caller below therefore computes `m` from GetLocalTransformMatrix(),
//! never from GetFinalTransformMatrix().  That is deliberate: the absolute
//! setters this feeds (SetObjectOrientation, SetObjectScale / SetObjectStretch,
//! ScaleObjectFromAnchor) are the transform PANEL's ops, and the panel edits
//! the node's own authored values.  The gizmo's DELTA ops are the world-space
//! ones, and they go through PushWorldOp_ instead.
void ReplaceFinalTransform_( IObjectPriv& obj, const Matrix4& m )
{
	if( Implementation::Transformable* transformable =
		dynamic_cast<Implementation::Transformable*>( &obj ) ) {
		transformable->SetFinalTransformMatrix( m );
	} else {
		obj.ClearAllTransforms();
		obj.PushTopTransStack( m );
	}
}

//! doc 89 slice C -- THE MIRROR-FREE FRAME, and why every editor op that derives a
//! new matrix FROM the current one has to do its arithmetic in it.
//!
//! `GetLocalTransformMatrix()` ENDS IN THE NODE'S OWN MIRROR: the composed local
//! transform is `P * O * Stretch * Scale * M`.  `SetFinalTransformMatrix` (through
//! `Transformable::ReplaceFinalStack_`) clears P / O / Stretch / Scale and the stack
//! but deliberately LEAVES `m_mxMirror` standing, so the next finalize re-appends it:
//! `SetFinalTransformMatrix(X)` means `local == X * M`, NOT `local == X`.
//!
//! THAT CONTRACT IS CORRECT AND MUST NOT BE CHANGED, because it is what makes the
//! round-trip to disk work.  `CommitPendingCstObjectTransforms` writes the chunk's
//! `matrix` param with the mirror STRIPPED, while the chunk's own `mirror` line
//! survives (a mirror is not part of standard_object's matrix > quaternion >
//! orientation precedence chain -- it composes with whichever of them wins).  The
//! next derive hands that stored matrix to `Job::AddObjectMatrix` and re-issues the
//! `mirror` line separately, and only the "finalize re-appends M" rule puts the two
//! back together.  Teaching `ReplaceFinalStack_` to consume the mirror instead would
//! make every saved scene with a `matrix` + `mirror` pair re-derive UN-reflected --
//! so the fix belongs HERE, on the editor side of that boundary, where it cannot
//! reach the derive path at all.
//!
//! WHAT GOES WRONG WITHOUT IT: an op that reads `P*O*St*Sc*M`, multiplies a diagonal
//! into it, and hands the product back gets the M it read AND the M finalize appends
//! -- and `M * S * M == S` for the diagonal S every one of these ops builds, so the
//! two reflections CANCEL.  The object silently loses its reflection, and then
//! PERSISTS un-reflected, because the commit strips a mirror that is no longer in the
//! matrix.  A reflection is its own inverse, so `X * M` un-applies it EXACTLY -- no
//! fitting, no tolerance -- which is the same right-multiply the read path
//! (ObjectIntrospection::ReadObjectParam) and the commit path already use.
Matrix4 MirrorOf_( const IObjectPriv& obj )
{
	// Identity for a node with no mirror (Transformable keeps `m_mxMirror` at
	// identity while `m_mirrorAxis` is -1) and for anything that is not a
	// Transformable at all -- which is where the mirror lives, as a non-virtual
	// member, exactly as `SetFinalTransformMatrix` does.
	const Implementation::Transformable* tf =
		dynamic_cast<const Implementation::Transformable*>( &obj );
	return tf ? tf->GetMirrorMatrix() : Matrix4Ops::Identity();
}

//! The node's LOCAL matrix with its own mirror UN-APPLIED: `P * O * Stretch * Scale`.
//! This is the matrix a position / orientation / scale decomposition has to see --
//! `DecomposeFinalAffine` builds a PROPER (det +1) frame and has nowhere to put a
//! reflection, so decomposing the un-corrected matrix invents a phantom 180-degree
//! rotation and folds the reflection into the residual.
Matrix4 MirrorFreeLocal_( const IObjectPriv& obj )
{
	return obj.GetLocalTransformMatrix() * MirrorOf_( obj );
}

//! Is `obj` an 87 CONTAINER -- a pure transform node with no surface?  A
//! CSGObject also has null geometry (its shape comes from its operands), so
//! "no geometry" alone is not the test.  Mirrors Job.cpp's IsContainerObject_;
//! kept local because SceneEditor has no business including Job.
bool IsContainerNodeForEdit_( const IObjectPriv& obj )
{
	if( obj.GetGeometry() ) return false;
	return dynamic_cast<const Implementation::CSGObject*>( &obj ) == 0;
}



//! Apply a WORLD-space operation `worldOp` to an object whose transform stack
//! is LOCAL.  Conjugating by the parent world transform,
//! `parentWorld^-1 * worldOp * parentWorld`, is what makes the composed result
//! come out as `worldOp * world_old` -- i.e. the gizmo moves the object by
//! exactly the world delta the pointer described, whatever frame its parent is
//! in.  For a root, parentWorld is identity and this is `worldOp` verbatim, so
//! the unparented path is byte-identical to the pre-87 code.
//! \return FALSE when the parent world transform is not invertible, in which
//! case NOTHING is pushed.  WorldToLocal returns its input unchanged in that
//! state, and pushing that would compose `P * worldOp * P * local` -- an extra
//! parent factor, which for a far-from-origin container is a teleport, and
//! which CommitPendingCstObjectTransforms would then write into the chunk.
//! Refusing mirrors what 86 section 3 got right about its own singular-`G`
//! case: a diagnostic beats a corrupt matrix.  Note the refusal is NOT limited
//! to subtrees that are already non-renderable: an extreme but non-degenerate
//! anisotropy (a ground-plane container at `scale 100000 0.00001 100000`, say)
//! renders perfectly well and is still refused here, because conjugating a
//! world delta through a frame that ill-conditioned would keep almost none of
//! double's digits.  The diagnostic says which of the two it is.
bool PushWorldOp_( IObjectPriv& obj, const String& objectName, const Matrix4& worldOp,
                   std::set<std::string>& alreadyWarned )
{
	if( !obj.IsParentWorldInvertible() ) {
		// ONCE per object, not once per pointer-move: this fires from inside a
		// gizmo drag, which would otherwise emit ~60 identical errors a second.
		// The set is the CALLER's per-editor member, matching
		// ObjectManager::danglingParentWarned.  A function-local `static` would
		// be shared by every SceneEditor in the process and mutated with no
		// lock -- and RISE_API_CreateSceneEditController is public surface for
		// exactly the multi-instance embedding that makes that a data race on
		// std::set's internal tree, not merely a duplicated log line.
		const std::string key( objectName.c_str() );
		if( alreadyWarned.insert( key ).second ) {
			GlobalLog()->PrintEx( eLog_Error,
				"SceneEditor:: world-space transform op REFUSED for `%s` -- its parent chain composes to a "
				"frame that cannot be inverted usefully: an ancestor is either COLLAPSED (a zero or "
				"near-zero `scale`, so the subtree has no world extent) or extremely ANISOTROPIC (axis "
				"scales differing by more than ~1e9, which renders fine but leaves too few digits to "
				"conjugate a world delta through).  Rebalance that ancestor's `scale`.", key.c_str() );
		}
		return false;
	}
	// VERIFY THE RESULT as well as trusting the predicate.
	//
	// THE TWO GATES ARE COMPLEMENTARY, NOT REDUNDANT.  An earlier revision of
	// this comment claimed they catch the same inputs and that this measurement
	// "cannot be wrong about rank, conditioning or scale".  Measured against 13
	// real parent frames, 7 disagreed -- so the claim was false, and it was
	// dangerous in the specific way it was wrong: it told the next reader the
	// predicate was redundant, and deleting it would silently drop the whole
	// conditioning bound.
	//
	//   This check catches GROSS arithmetic failure -- a conjugation that did
	//   not happen at all.  Its sensitivity is NOT unconditional, and the limit
	//   is worth naming precisely because it is the same one formulation (1) in
	//   Transformable.cpp was rejected for: the residual is normalised by ONE
	//   Frobenius norm over all 16 entries, so it scales as
	//   |worldOp| / |parentWorld|.  Measured with the predicate bypassed, a
	//   fully rank-1 parent gives 0.71 at translation 1, 3e-6 at 1e6 and 3e-10
	//   at 1e10 -- i.e. a far-from-origin collapsed frame would slip through
	//   this check alone.  It is likewise blind to CONDITIONING: the residual
	//   SHRINKS as the axis spread grows (1.0e-16 at 1e10, 4.8e-24 at 1e14),
	//   because error along a direction the parent compresses never reaches the
	//   norm that divides it.
	//
	//   IsParentWorldInvertible() carries the CONDITIONING bound -- the half
	//   this cannot see.
	//
	// So the PREDICATE is the primary gate -- it normalises the linear part
	// first and is therefore sensitive to rank and conditioning independently
	// of translation and scale -- and this is the backstop that measures the
	// arithmetic actually about to be committed.  Neither subsumes the other,
	// and the predicate has been wrong five times, which is why the backstop
	// exists.  Nothing here reaches the object unless BOTH agree.
	//
	// IsParentWorldInvertible() is a PREDICTION about a matrix, and predicting
	// this has been got wrong five times in review -- an absolute residual
	// epsilon, a componentwise backward error, a Hadamard ratio, a condition
	// number built from an adjugate quotient, and a version that validated a
	// normalised 3x3 while storing a raw 4x4 inverse -- each defeated by a
	// different input class.  So do not rely on the prediction alone for the
	// part that can be MEASURED: whether THIS conjugation came out right.
	//
	// The contract is `parentWorld * localOp == worldOp * parentWorld`, which
	// is just the definition of the conjugate rearranged.  Comparing those two
	// products tests the exact arithmetic that is about to be committed, in the
	// exact frame it will be committed in, and cannot be wrong about rank,
	// conditioning or scale -- there is nothing left to be wrong about.  The
	// tolerance is RELATIVE to the magnitudes involved, so it means the same
	// thing for a millimetre scene and an astronomical one.
	const Matrix4 parentWorld = obj.GetParentWorldTransformMatrix();
	const Matrix4 localOp     = obj.WorldToLocal( worldOp * parentWorld );
	{
		const Matrix4 lhs = parentWorld * localOp;   // what the next finalize will produce
		const Matrix4 rhs = worldOp * parentWorld;   // what the caller asked for
		const Scalar* l = &lhs._00;
		const Scalar* r = &rhs._00;
		double diff = 0, scale = 0;
		for( int k = 0; k < 16; ++k ) {
			const double a2 = static_cast<double>( l[k] );
			const double b2 = static_cast<double>( r[k] );
			diff  += ( a2 - b2 ) * ( a2 - b2 );
			scale += b2 * b2;
		}
		diff  = std::sqrt( diff );
		scale = std::sqrt( scale );
		// `!(<=)` also rejects NaN, which a non-finite parent produces here.
		if( !( diff <= 1e-9 * ( scale > 0 ? scale : 1.0 ) ) ) {
			const std::string key( objectName.c_str() );
			if( alreadyWarned.insert( key ).second ) {
				GlobalLog()->PrintEx( eLog_Error,
					"SceneEditor:: world-space transform op REFUSED for `%s` -- the conjugation into its "
					"parent frame did not verify (relative residual %g).  An ancestor's `scale` COLLAPSES "
					"the frame (a zero or near-zero axis), so a world-space delta has no local "
					"equivalent.", key.c_str(), scale > 0 ? diff / scale : diff );
			}
			return false;
		}
	}
	// SUCCESS clears the object's warn-once entry, so if the author rebalances
	// the offending ancestor and a LATER op on the same object is refused for a
	// different reason, that refusal is reported rather than swallowed.
	alreadyWarned.erase( std::string( objectName.c_str() ) );
	obj.PushBottomTransStack( localOp );
	return true;
}

Matrix4 ScaleFreeAffineBase_( const Matrix4& current )
{
	Matrix4 result = current;
	Matrix4 rotation, residual;
	ObjectIntrospection::DecomposeFinalAffine( current, rotation, residual );
	Scalar* columns[3] = { &result._00, &result._10, &result._20 };
	const Scalar* frameColumns[3] = { &rotation._00, &rotation._10, &rotation._20 };
	for( int column = 0; column < 3; ++column ) {
		const Scalar length = FinalColumnLength_( current, column );
		if( length > 0 ) {
			columns[column][0] /= length;
			columns[column][1] /= length;
			columns[column][2] /= length;
		} else {
			// Recover the missing direction from the proper frame inferred
			// from the surviving columns. A matching world axis can be parallel
			// to another rotated column and leave the matrix singular.
			columns[column][0] = frameColumns[column][0];
			columns[column][1] = frameColumns[column][1];
			columns[column][2] = frameColumns[column][2];
		}
	}
	return result;
}

void SetAbsoluteStretch_( IObjectPriv& obj, const Vector3& target )
{
	// LOCAL: an absolute scale is a property of the node's own transform.  A
	// parented node inherits its parent's scale on top of this, which is what
	// a scene graph is for.
	//
	// MIRROR-FREE (doc 89 slice C, see MirrorFreeLocal_): the base and the stretch
	// are both what the chunk's `scale` line means, and finalize appends the mirror
	// to the product.  Reading the un-corrected local matrix instead would carry an
	// M into `base`, and `M * Stretch * M == Stretch` -- so `scale 2 1 1` on a
	// `mirror x` object would come out at det +2 where -2 is correct, and the commit
	// would then write an un-mirrored pose to disk beside a surviving `mirror x`.
	const Matrix4 base = ScaleFreeAffineBase_( MirrorFreeLocal_( obj ) );
	ReplaceFinalTransform_( obj, base * Matrix4Ops::Stretch( target ) );
}

}  // namespace

bool SceneEditor::ApplyObjectOpForward( IObjectPriv& obj, const SceneEdit& edit, bool isReplay )
{
	bool ok = true;   // P1: false if a binding op's forward target name no longer resolves
	switch( edit.op )
	{
	case SceneEdit::TranslateObject:
		// The gizmo's translate drag produces a WORLD-space delta (see
		// SceneEditController::OnPointerMove), and Transformable::TranslateObject
		// would push it onto the LOCAL stack -- under a rotated or scaled parent
		// that moves the object somewhere else entirely.  Conjugate first.
		ok = PushWorldOp_( obj, edit.objectName, Matrix4Ops::Translation( edit.v3a ), mWorldOpRefusalWarned );
		break;
	case SceneEdit::RotateObjectArb:
		{
			// The editor's rotation gizmo is centred on the selected object's
			// world-space pivot and advertises WORLD-axis rings. The legacy
			// Transformable::RotateObjectArbAxis appends a bare rotation to the
			// transform stack; FinalizeTransformations left-multiplies that
			// rotation, which rotates the translation column too and makes an
			// off-origin object orbit (0,0,0). Conjugate the world rotation by
			// the current pivot so only the basis changes:
			//
			//     T(pivot) * R(worldAxis) * T(-pivot) * currentTransform
			//
			// PushBottomTransStack makes this the last stack entry applied, hence
			// the leftmost factor in the finalized matrix. This preserves any
			// existing scale/shear/stack representation and keeps undo capture
			// lossless while matching the gizmo's visible pivot.
			// The pivot and the axis are both WORLD quantities (the gizmo draws
			// world-axis rings around the object's world origin), so `aboutPivot`
			// is a world operation and goes through PushWorldOp_ -- which pushes
			// its parent-frame conjugate onto the LOCAL stack.
			const Matrix4 current = obj.GetFinalTransformMatrix();
			const Vector3 pivot( current._30, current._31, current._32 );
			const Matrix4 aboutPivot =
				Matrix4Ops::Translation( pivot )
			  * Matrix4Ops::Rotation( edit.v3a, edit.s )
			  * Matrix4Ops::Translation( Vector3( -pivot.x, -pivot.y, -pivot.z ) );
			ok = PushWorldOp_( obj, edit.objectName, aboutPivot, mWorldOpRefusalWarned );
		}
		break;
	case SceneEdit::SetObjectPosition:
		{
			// SetObjectPosition is an ABSOLUTE setter -- the transform panel's
			// `position` row, i.e. the chunk's own authored value -- so under 87
			// it is a LOCAL contract, parent-relative.  For a root (the only
			// case that existed before 87) local == world and this is the same
			// absolute world contract it always was.  Editor rotations and
			// translations can live in the outer transform stack, so replacing
			// only the inner position component would still let those matrices
			// move the requested point: apply the correction to the composed
			// LOCAL origin instead, regardless of transform provenance.
			const Matrix4 current = obj.GetLocalTransformMatrix();
			obj.TranslateObject( Vector3(
				edit.v3a.x - current._30,
				edit.v3a.y - current._31,
				edit.v3a.z - current._32 ) );
		}
		break;
	case SceneEdit::SetObjectOrientation:
		{
			// Absolute setter -> LOCAL, for the same reason as SetObjectPosition.
			//
			// MIRROR-FREE (doc 89 slice C, see MirrorFreeLocal_), and here it is not
			// only about the double-M cancellation: `DecomposeFinalAffine` builds a
			// PROPER frame, so handed a reflection-bearing matrix it invents a
			// phantom rotation (an unrotated `mirror x` object decomposes to
			// `orientation 0 180 0`) and buries the reflection in the residual.  The
			// READ path un-applies the mirror for exactly that reason, so committing
			// the value the panel just showed has to decompose the SAME matrix the
			// panel decomposed -- otherwise a no-op Enter on a correct value
			// un-reflects the object and adds the phantom 180 degrees.
			const Matrix4 current = MirrorFreeLocal_( obj );
			const Vector3 position(
				current._30,
				current._31,
				current._32 );
			Matrix4 currentRotation, affineResidual;
			ObjectIntrospection::DecomposeFinalAffine(
				current, currentRotation, affineResidual );
			const Matrix4 desired = Matrix4Ops::Translation( position )
				* ( Matrix4Ops::XRotation( edit.v3a.x )
				  * Matrix4Ops::YRotation( edit.v3a.y )
				  * Matrix4Ops::ZRotation( edit.v3a.z ) )
				* affineResidual;
			ReplaceFinalTransform_( obj, desired );
		}
		break;
	case SceneEdit::SetObjectScale:
		SetAbsoluteStretch_( obj, Vector3( edit.s, edit.s, edit.s ) );
		break;
	case SceneEdit::SetObjectStretch:
		SetAbsoluteStretch_( obj, edit.v3a );
		break;
	case SceneEdit::ScaleObjectFromAnchor:
		// Keep the scaled result in the authoritative-final representation.
		// A raw Clear+Push sequence leaves component-mode active, so a later
		// public/keyframed absolute setter composes underneath this matrix.
		//
		// `edit.prevTransform` is the captured LOCAL matrix, so it too ends in the
		// node's mirror -- un-apply it before the stretch (doc 89 slice C, see
		// MirrorFreeLocal_) or the gizmo's scale drag lands a uniform x2 at det +8
		// instead of -8 on a mirrored object.
		ReplaceFinalTransform_(
			obj, edit.prevTransform * MirrorOf_( obj ) * Matrix4Ops::Stretch( edit.v3a ) );
		break;
	case SceneEdit::SetObjectMaterial:
		if( mMaterialManager ) {
			IMaterial* mat = mMaterialManager->GetItem( edit.propertyValue.c_str() );
			if( mat && !isReplay && IsContainerNodeForEdit_( obj ) ) {
				// 87: a CONTAINER has no surface, so it takes no material -- the
				// same rule Job::AddObject applies at derive time, and the one
				// that keeps "null geometry + emissive material" a csg_object-
				// only shape for the agent's non-sampling-emitter audit.
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor:: `%s` is a container node (no geometry), so it takes no material; "
					"bind the material to a child that has geometry", edit.objectName.c_str() );
				ok = false;
			} else if( mat ) {
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
		if( !isReplay && IsContainerNodeForEdit_( obj ) ) {
			// 87: a container has no surface, so it takes no surface binding --
			// the same rule the derive applies (DropContainerSurfaceBindings_).
			// The pre-routing gate in ApplyForwardMutation covers every binding
			// op, but only on a scene with a retained CST Document; this is the
			// API / Blender / PRISE path, which has none.
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor:: `%s` is a container node (no geometry), so it takes no shader; bind it to a "
				"child that has geometry",
				edit.objectName.c_str() );
			ok = false;
		} else if( mShaderManager ) {
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
		if( !isReplay && IsContainerNodeForEdit_( obj )
		 && !( edit.propertyValue.size() <= 1 || edit.propertyValue == String( "none" ) ) ) {
			// 87: no surface means no interior to be inside of -- see the
			// shader arm above.  CLEARING is still allowed, so an object that
			// became a container can be tidied up.
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditor:: `%s` is a container node (no geometry), so it takes no interior medium (no "
				"surface means no interior to be inside of); bind it to a child that has geometry",
				edit.objectName.c_str() );
			ok = false;
		} else if( edit.propertyValue.size() <= 1 || edit.propertyValue == String( "none" ) ) {
			obj.ClearInteriorMedium();
		} else if( mJob ) {
			const IMedium* med = mJob->GetMedium( edit.propertyValue.c_str() );
			if( med ) obj.AssignInteriorMedium( *med );
			else      ok = false;   // P1: forward medium removed
		} else {
			ok = false;
		}
		break;
	case SceneEdit::SetObjectMirror:
		// doc 89 slice C.  Routed through IJob so the ONE decode of the axis
		// string, the ONE csg refusal and the ONE "no real change -> no churn"
		// early-out live in Job::SetObjectMirror, shared with the parser -- the
		// live path and the derive path can then never disagree about what
		// `mirror z` means.  DELIBERATELY NO CONTAINER GATE: a container is
		// nothing but a transform, and mirroring a whole assembly in one edit is
		// the feature (see the standard_object parser's own note).
		//
		// Empty / "none" clears, matching the chunk's spelling and the panel's
		// blank row.  The spatial-structure invalidation and the light-generation
		// bump are Job's; the caller's OpNeedsSpatialRebuild branch covers the
		// editor-side TLAS churn for the same reason SetObjectGeometry relies on it.
		if( mJob ) {
			const String axis = ( edit.propertyValue.size() <= 1 )
				? String( "none" ) : edit.propertyValue;
			ok = mJob->SetObjectMirror( edit.objectName.c_str(), axis.c_str() );
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
			if( g ) {
				// 87: giving a CONTAINER geometry would make it a real shape, so
				// it would have to become world-visible too -- otherwise it is a
				// hidden node WITH geometry, which is exactly the fingerprint
				// ObjectManager::SetObjectParent uses to identify a CSG operand.
				//
				// That state is UNREACHABLE from here: CaptureForApply refuses a
				// SetObjectGeometry on an object whose current geometry has no
				// name, and a container's is null.  The visibility flip is done
				// anyway, because relying on a guard three call frames away to
				// keep this branch correct is how the dead-guard bugs in this
				// arc happened.  The DERIVE path's equivalent is
				// ApplyGeometryOrContainer_ in Job.cpp.
				const bool wasContainer = ( obj.GetGeometry() == 0 );
				obj.AssignGeometry( *g );
				if( wasContainer ) obj.SetWorldVisible( true );
			}
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
	// Prefer the exact V2 state captured at edit time.  It preserves component
	// matrices, ordered stack entries, and authoritative matrix-edit metadata,
	// so both later setters and stack Push/Pop operations retain their original
	// semantics. Fall back to a collapsed final matrix only for targets that do
	// not expose the concrete Transformable implementation.
	if( edit.hasTransformState ) {
		if( Implementation::Transformable* t = dynamic_cast<Implementation::Transformable*>( &obj ) ) {
			if( t->RestoreTransformStateV2( edit.prevTransformState ) ) return;
		}
	}
	// Fallback (ITransformable composes local = position * orientation *
	// stretch * scale * stack; zero components + push the captured matrix).
	// `prevTransform` is the captured LOCAL matrix, and the stack is local, so
	// this restores the node's own transform and leaves its parent link's
	// contribution to the next compose.
	//
	// doc 89 slice C: `ClearAllTransforms` also clears the MIRROR (it is a local
	// transform building block, and Job::AddObject's re-apply depends on that clearing
	// -- an edit that DELETED the `mirror` line has to actually un-reflect the object).
	// The captured `prevTransform` already has the reflection BAKED IN, so clear-then-
	// push alone restores the right world matrix but leaves `mirrorAxis == none`: the
	// commit path then finds no mirror to STRIP, writes the reflection into the `matrix`
	// param with no `mirror` line beside it, and the next edit in this session reflects
	// what is already reflected.  Re-issue the axis and un-apply its matrix from what we
	// push (M * M == I), so both the pose and the authored axis come back.
	//
	// THE AXIS IS READ OFF THE LIVE NODE when the edit carries no V2 snapshot, and that
	// is the case that actually reaches this fallback: `ScaleObjectFromAnchor` -- the
	// gizmo's scale drag -- deliberately skips CaptureForApply's snapshot block (its
	// `prevTransform` is the controller's drag-start anchor, which must not be
	// overwritten per frame), so `hasTransformState` is FALSE for exactly the op whose
	// undo lands here.  A transform op never changes the mirror axis, so the live value
	// IS the value to restore.  The snapshot is preferred when there is one, since it is
	// the state this edit promised to restore.
	Implementation::Transformable* t = dynamic_cast<Implementation::Transformable*>( &obj );
	const int restoreAxis = !t ? -1
		: ( edit.hasTransformState ? edit.prevTransformState.mirrorAxis : t->GetMirrorAxis() );
	obj.ClearAllTransforms();
	Matrix4 restored = edit.prevTransform;
	// Guarded on the RETURN, not assumed: a snapshot whose axis is out of range is
	// exactly why RestoreTransformStateV2 above may have refused, and a refused
	// SetMirrorAxis must leave `restored` reflection-bearing (world-correct, axis lost)
	// rather than un-apply a mirror that is not there.
	if( t && restoreAxis >= 0 && t->SetMirrorAxis( restoreAxis ) ) {
		restored = restored * t->GetMirrorMatrix();
	}
	obj.PushTopTransStack( restored );
}

void SceneEditor::RunObjectInvariantChain( IObjectPriv& obj )
{
	obj.FinalizeTransformations();
	obj.ResetRuntimeData();
	const IObjectManager* objs = mScene->GetObjects();
	if( objs )
	{
		// 87: re-bake the whole authored graph, not just the edited node.
		// obj's finalize above updated ITS world matrix; every DESCENDANT is
		// still holding the parent world it was last composed against, along
		// with the three caches derived from it (inverse-transpose, tangent
		// sign, world-area Jacobian).  Without this walk a gizmo drag on a
		// container moves the container and leaves its subtree behind -- and
		// the InvalidateSpatialStructure below would then rebuild the TLAS
		// from those stale boxes.  The derive-time compose cannot cover this:
		// a live edit does not re-derive, and on a scene with no retained CST
		// Document (or a refused commit) it never will.
		//
		// COST, stated honestly rather than asserted away.  On a FLAT scene this
		// is free -- ComposeWorldTransforms returns immediately when the graph
		// has no links and none are outstanding, which is every scene that
		// existed before 87.  On a scene with even one parent link it is
		// O(N log N) over the WHOLE object list -- three heap containers built
		// per call, plus a serial lookup per parented child -- once per edit,
		// and an edit here means a gizmo-drag pointer-move.  That is a real cost and it is not hidden:
		// the named refinement is a subtree-scoped compose (walk down from the
		// edited node only, since its own parent's world is already current),
		// which is a strictly smaller version of this same walk.  It is not
		// built yet because correctness came first and because the next render
		// pays a whole TLAS rebuild for the same edit regardless.
		const bool subtreeMoved = objs->ComposeWorldTransforms();
		objs->InvalidateSpatialStructure();
		if( subtreeMoved ) {
			// The DESCENDANTS the walk moved are not `obj`, so the
			// emitter check below -- which reads the edited object's own
			// material -- cannot see that one of them emits.  For a
			// CONTAINER it is a guaranteed no-op: a container carries no
			// material at all.  So dragging a container that parents a
			// lamp would re-render the lamp's geometry at its new place
			// while the light sampler kept sampling the old one.  Same
			// argument, and the same conservative answer, as the
			// incremental apply's gate in Cst.cpp: one extra sampler
			// rebuild beats a silent geometry/light desync.
			BumpSceneLightGeneration();
		}
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
	// inline.  AddCamera (a created entity — historically the
	// byte-splice era's "no source span" case, now just the Phase C
	// created channel below) and SetSceneTime (transient) are
	// intentionally NOT part of Phase B's per-category channel.
	switch( edit.op )
	{
	case SceneEdit::SetObjectGeometry:
	case SceneEdit::SetObjectMaterial:
	case SceneEdit::SetObjectShader:
	case SceneEdit::SetObjectShadowFlags:
	case SceneEdit::SetObjectInteriorMedium:
	case SceneEdit::SetObjectMirror:   // doc 89 slice C: a standard_object param edit like the rest
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
		// Phase C: mark the new entity created.  Historically the
		// byte-splice save engine emitted a fresh chunk for it (a
		// created entity has no source span); that machinery went
		// with Slice 6d — today the mark only feeds dirty state.
		// objectName carries the new camera's name.
		mDirtyTracker.MarkEntityCreated( EntityCategory::Camera,
			std::string( edit.objectName.c_str() ) );
		break;
	case SceneEdit::SetAgentCstParam:
		// P1-4 fix (round 1): the FORWARD agent commit marks dirty via
		// MarkCstHeadDirty (SceneEditController::ApplyAgentParamEdit calls it
		// directly, since that mutation lands OUTSIDE SceneEditor::Apply --
		// see PushAgentCstParamEdit's doc). But MarkEditEntityDirty (THIS
		// function) is the one Undo()/Redo() call for every op via
		// ApplyRevertMutation/ApplyForwardMutation's shared callers -- falling
		// to `default: break` here left an agent edit's Undo/Redo with NO
		// dirty mark of its own. Harmless when the entity was already dirty
		// from the original forward edit (the per-entity channel is a set,
		// so re-marking is a no-op) -- but after a SAVE clears the tracker,
		// an Undo or Redo of an agent edit mutates the Document away from the
		// saved bytes with nothing to flip HasUnsavedChanges() back on,
		// so a close-without-prompt silently discards the reverted/redone
		// edit. Route through the SAME MarkCstHeadDirty the forward path
		// uses (kind-aware category dispatch; unknown/empty kind falls to
		// the CST-head boolean channel) so Undo/Redo match the forward mark
		// exactly.
		MarkCstHeadDirty( edit.objectName.c_str(),
			edit.cstEntityKind.size() > 1 ? edit.cstEntityKind.c_str() : nullptr );
		break;
	case SceneEdit::AgentInsertChunk:
	case SceneEdit::AgentRemoveChunk:
	case SceneEdit::AgentRemoveChunks:
	case SceneEdit::AgentReplaceGeometry:
	case SceneEdit::AgentDuplicateNode:
		// doc-88 S20: AgentDuplicateNode rides the SAME arm as AgentReplaceGeometry, and for the same reason
		// -- it too carries a real `cstEntityKind` (the copy's chunk keyword) and a real entity name (the
		// copy's name), so MarkCstHeadDirty routes it to that entity's own channel.
		// R2: AgentReplaceGeometry rides the SAME arm; unlike the batch remove it DOES carry a real
		// `cstEntityKind` ("standard_object") and a real entity name, so MarkCstHeadDirty routes it to that
		// object's own channel -- the same channel the forward commit marked.
		// R1a: AgentRemoveChunks rides the SAME arm -- its `cstEntityKind` is deliberately EMPTY (see the op
		// doc), so MarkCstHeadDirty routes it to the boolean CST-head channel rather than a per-entity mark.
		// Shared-undo U2: same rationale as SetAgentCstParam above -- the FORWARD chunk-CRUD commit marks
		// dirty via MarkCstHeadDirty directly (SceneEditController::ApplyAgentChunkCrud_), since that mutation
		// also lands OUTSIDE SceneEditor::Apply.  Undo/Redo of a chunk-CRUD op route through THIS function via
		// ApplyRevertMutation/ApplyForwardMutation's shared callers, so without this case a chunk-CRUD Undo/Redo
		// after a Save would leave HasUnsavedChanges() false -- the same data-loss gap P1-4 closed for param edits.
		MarkCstHeadDirty( edit.objectName.c_str(),
			edit.cstEntityKind.size() > 1 ? edit.cstEntityKind.c_str() : nullptr );
		break;
	default:
		break;
	}
}

void SceneEditor::FireDirtyChangedIfTransitioned()
{
	// Document-first ADR phase 1: record-only.  The listener itself runs in
	// DrainDirtyNotification, called by the controller AFTER releasing its
	// mutex -- so a mark under any lock can never deadlock a re-entrant
	// listener nor block the mutating thread on shell work.
	// Round-5 review P1: every mutation site runs under its own lock with a
	// stable Document, so this is also where the derived-dirty CACHE is
	// recomputed race-free (the lock-free C-API query reads only the cache).
	RecomputeHeadDiffers_();
	RecomputeHasUnsavedChangesCached_();
	const std::shared_ptr<DirtyNotificationState> state = mDirtyNotificationState;
	state->currentHasUnsavedChanges.store(
		mHasUnsavedChangesCached.load( std::memory_order_acquire ),
		std::memory_order_release );
	state->pending.store( true, std::memory_order_release );
}

SceneEditor::DirtyNotificationDeferral::~DirtyNotificationDeferral()
{
	if( !mState ) return;
	if( mState->deferDepth.fetch_sub( 1, std::memory_order_acq_rel ) == 1 )
		SceneEditor::DrainDirtyNotificationState_( mState );
}

SceneEditor::DirtyNotificationDeferral SceneEditor::DeferDirtyNotifications()
{
	const std::shared_ptr<DirtyNotificationState> state = mDirtyNotificationState;
	state->deferDepth.fetch_add( 1, std::memory_order_acq_rel );
	return DirtyNotificationDeferral( state );
}

bool SceneEditor::DrainDirtyNotification()
{
	const std::shared_ptr<DirtyNotificationState> state =
		mDirtyNotificationState;
	return DrainDirtyNotificationState_( state );
}

bool SceneEditor::IsInDirtyChangedCallbackOnThisThread() const
{
	return IsInDirtyCallback( mDirtyNotificationState.get() );
}

void SceneEditor::SetDirtyChangedListener( DirtyChangedFn fn )
{
	DirtyChangedPtr retired = ExchangeDirtyChangedListener( std::move( fn ) );
	// The last shared owner may live on this setter thread after it waited for
	// an in-flight callback's snapshot to retire.  Keep lifecycle rejection TLS
	// active through that arbitrary capture destructor as well.
	DirtyCallbackScope retirementScope( mDirtyNotificationState.get() );
	retired.reset();
}

bool SceneEditor::DrainDirtyNotificationState_(
	const std::shared_ptr<DirtyNotificationState>& state )
{
	// Round-5 review P1 (listener OUTSIDE all locks + ordered delivery):
	// single-deliverer loop. The complete dirty value was published by the
	// mutator while its controller lock held, so this state-only routine
	// neither races DirtyTracker containers nor borrows the SceneEditor.
	// Lifecycle destruction is rejected until the callback and its copied
	// std::function target have both retired.
	if( state->deferDepth.load( std::memory_order_acquire ) != 0 )
		return state->ownerAlive.load( std::memory_order_acquire );
	if( !state->pending.load( std::memory_order_acquire ) )
		return state->ownerAlive.load( std::memory_order_acquire );
	if( state->delivering.exchange( true, std::memory_order_acq_rel ) )
		return state->ownerAlive.load( std::memory_order_acquire );   // active deliverer will pick it up
	struct DeliveringGuard
	{
		explicit DeliveringGuard(
			const std::shared_ptr<DirtyNotificationState>& stateIn )
			: state( stateIn ), active( true )
		{
		}
		~DeliveringGuard()
		{
			Release();
		}
		void Release()
		{
			if( active ) {
				state->delivering.store( false, std::memory_order_release );
				active = false;
			}
		}
		std::shared_ptr<DirtyNotificationState> state;
		bool active;
	};
	DeliveringGuard deliveringGuard( state );
	struct CallbackFlightGuard
	{
		explicit CallbackFlightGuard(
			const std::shared_ptr<DirtyNotificationState>& stateIn )
			: state( stateIn )
		{
		}
		~CallbackFlightGuard()
		{
			std::lock_guard<std::mutex> lk( state->mutex );
			if( state->callbacksInFlight > 0 ) --state->callbacksInFlight;
			if( state->callbacksInFlight == 0 ) {
				state->callbackThread = std::thread::id();
				state->callbackCV.notify_all();
			}
		}
		std::shared_ptr<DirtyNotificationState> state;
	};
	struct CopiedListenerRetireGuard
	{
		explicit CopiedListenerRetireGuard( DirtyChangedPtr& listenerIn )
			: listener( listenerIn ) {}
		~CopiedListenerRetireGuard()
		{
			listener.reset();
		}
		DirtyChangedPtr& listener;
	};
	for( ;; )
	{
		if( !state->pending.exchange( false, std::memory_order_acq_rel ) ) break;
		bool fire = false;
		const bool now = state->currentHasUnsavedChanges.load( std::memory_order_acquire );
		DirtyChangedPtr listenerCopy;
		{
			std::lock_guard<std::mutex> lk( state->mutex );
			if( now != state->prevHasUnsavedChanges )
			{
				state->prevHasUnsavedChanges = now;
				fire = true;
				listenerCopy = state->listener;
				if( listenerCopy ) {
					++state->callbacksInFlight;
					state->callbackThread = std::this_thread::get_id();
				}
			}
		}
		if( fire && listenerCopy ) {
			CallbackFlightGuard flight( state );
			DirtyCallbackScope callbackScope( state.get() );
			CopiedListenerRetireGuard retireCopy( listenerCopy );
			try {
				(*listenerCopy)( now );   // NO lock held
			} catch( const std::exception& e ) {
				GlobalLog()->PrintEx( eLog_Error,
					"SceneEditor: dirty-changed listener threw; notification contained: %s",
					e.what() );
			} catch( ... ) {
				GlobalLog()->PrintEx( eLog_Error,
					"SceneEditor: dirty-changed listener threw a non-standard exception; notification contained." );
			}
			if( !state->ownerAlive.load( std::memory_order_acquire ) ) {
				return false;
			}
		}
	}
	deliveringGuard.Release();
	// Close the race where another thread flagged pending after our last
	// exchange but saw us still delivering: one bounded retry.  A residual
	// miss self-heals at the next drain (per-frame catch-all).
	if( state->pending.load( std::memory_order_acquire )
	 && !state->delivering.exchange( true, std::memory_order_acq_rel ) )
	{
		DeliveringGuard retryDeliveringGuard( state );
		if( state->pending.exchange( false, std::memory_order_acq_rel ) )
		{
			bool fire = false;
			const bool now = state->currentHasUnsavedChanges.load( std::memory_order_acquire );
			DirtyChangedPtr listenerCopy;
			{
				std::lock_guard<std::mutex> lk( state->mutex );
				if( now != state->prevHasUnsavedChanges )
				{
					state->prevHasUnsavedChanges = now;
					fire = true;
					listenerCopy = state->listener;
					if( listenerCopy ) {
						++state->callbacksInFlight;
						state->callbackThread = std::this_thread::get_id();
					}
				}
			}
			if( fire && listenerCopy ) {
				CallbackFlightGuard flight( state );
				DirtyCallbackScope callbackScope( state.get() );
				CopiedListenerRetireGuard retireCopy( listenerCopy );
				try {
					(*listenerCopy)( now );
				} catch( const std::exception& e ) {
					GlobalLog()->PrintEx( eLog_Error,
						"SceneEditor: dirty-changed listener threw; notification contained: %s",
						e.what() );
				} catch( ... ) {
					GlobalLog()->PrintEx( eLog_Error,
						"SceneEditor: dirty-changed listener threw a non-standard exception; notification contained." );
				}
				if( !state->ownerAlive.load( std::memory_order_acquire ) ) {
					return false;
				}
			}
		}
	}
	return state->ownerAlive.load( std::memory_order_acquire );
}

void SceneEditor::CaptureSavedHeadVersion_()
{
	// GetCstHeadVersion lives on IJobPriv (mJob is stored as IJob*); the
	// downcast is safe in-tree (the controller always binds the concrete
	// Job).  A null / non-priv job leaves the baseline defaulted (uuid 0),
	// matching HeadDiffersFromSaved_'s legacy-scene arm.
	if( const IJobPriv* priv = dynamic_cast<const IJobPriv*>( mJob ) )
		mSavedHeadVersion = priv->GetCstHeadVersion();
	else
		mSavedHeadVersion = RISE::Cst::CstHeadVersion();
	RecomputeHeadDiffers_();   // round-5 P1: the cache must track the new baseline
	RecomputeHasUnsavedChangesCached_();
}

void SceneEditor::RecomputeHeadDiffers_()
{
	const IJobPriv* priv = dynamic_cast<const IJobPriv*>( mJob );
	bool differs = false;
	if( priv )
	{
		const RISE::Cst::CstHeadVersion cur = priv->GetCstHeadVersion();
		if( cur.uuid != 0 ) differs = ( cur != mSavedHeadVersion );   // legacy scene (uuid 0): no floor
	}
	mHeadDiffersCached.store( differs, std::memory_order_release );
}

void SceneEditor::RecomputeHasUnsavedChangesCached_()
{
	const bool dirty = HeadDiffersFromSaved_()
		|| mDirtyTracker.HasAnyDirty()
		|| !mScaleFromAnchorSet.empty();
	mHasUnsavedChangesCached.store( dirty, std::memory_order_release );
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
// P5 Slice 3 expansion (object): an object BINDING op re-points a standard_object reference slot -- it maps
// 1:1 to a standard_object param, so it routes per-op (like material/light).  TRANSFORM ops are committed to
// the authoritative `matrix` param at the composite/edit boundary instead (see CommitPendingCstObjectTransforms).
static inline bool IsObjectBindingOp( SceneEdit::Op op )
{
	return op == SceneEdit::SetObjectMaterial
	    || op == SceneEdit::SetObjectShader
	    || op == SceneEdit::SetObjectGeometry
	    || op == SceneEdit::SetObjectInteriorMedium;
}

// The standard_object param role each binding op writes.
static inline const char* ObjectBindingRole( SceneEdit::Op op )
{
	switch( op ) {
	case SceneEdit::SetObjectMaterial:       return "material";
	case SceneEdit::SetObjectShader:         return "shader";
	case SceneEdit::SetObjectGeometry:       return "geometry";
	case SceneEdit::SetObjectInteriorMedium: return "interior_medium";
	default:                                 return "";
	}
}

// doc 89 slice C: `mirror` is a standard_object PARAM edit that routes per-op through the same
// RouteCstParamEdit_ as the bindings -- but it is NOT a binding.  It names no other chunk (so
// there is no manager to reverse-look-up and no "the referent was deleted" undo failure), and it
// is LEGAL ON A CONTAINER, which is the whole feature.  `IsObjectBindingOp`'s other job is driving
// the container refusal in ApplyForwardMutation, so joining that predicate would refuse exactly
// the case `mirror` exists for.  Hence a second predicate over both, used at the two ROUTING
// sites only.
static inline bool IsObjectCstParamOp( SceneEdit::Op op )
{
	return IsObjectBindingOp( op ) || op == SceneEdit::SetObjectMirror;
}

static inline const char* ObjectCstParamRole( SceneEdit::Op op )
{
	return ( op == SceneEdit::SetObjectMirror ) ? "mirror" : ObjectBindingRole( op );
}

// P5 Slice 3 expansion (object transform): a TRANSFORM edit (panel absolute OR gizmo delta) is committed to the
// standard_object `matrix` param (authoritative, lossless) at the composite/edit boundary -- NOT per-op (a gizmo
// drag emits one per frame, which would be N full re-derives).  See CommitPendingCstObjectTransforms.
static inline bool IsObjectTransformOp( SceneEdit::Op op )
{
	return op == SceneEdit::TranslateObject
	    || op == SceneEdit::RotateObjectArb
	    || op == SceneEdit::SetObjectPosition
	    || op == SceneEdit::SetObjectOrientation
	    || op == SceneEdit::SetObjectScale
	    || op == SceneEdit::SetObjectStretch
	    || op == SceneEdit::ScaleObjectFromAnchor;
}

// Serialise a Matrix4 as the 16 col-major doubles the standard_object `matrix` param expects.  RISE's Matrix4
// stores fields `_<col><row>` contiguously in declaration order (_00,_01,..,_33), and the load-time parser maps
// matrix[k] -> the k-th field in that same order (Job::AddObjectMatrix), so emitting the fields in declaration
// order is a ROUND-TRIP-EXACT col-major encoding.  %.17g preserves every double bit-for-bit.
static std::string FormatMatrix16( const Matrix4& m )
{
	const Scalar v[16] = {
		m._00, m._01, m._02, m._03,
		m._10, m._11, m._12, m._13,
		m._20, m._21, m._22, m._23,
		m._30, m._31, m._32, m._33 };
	std::string out;
	char buf[ 32 ];
	for( int i = 0; i < 16; ++i ) {
		std::snprintf( buf, sizeof( buf ), "%.17g", static_cast<double>( v[i] ) );
		if( i ) out += ' ';
		out += buf;
	}
	return out;
}

// Format a Vector3 as "x y z" at full precision.
static std::string FormatVec3( const Vector3& v )
{
	char buf[ 96 ];
	std::snprintf( buf, sizeof( buf ), "%.17g %.17g %.17g", static_cast<double>( v.x ), static_cast<double>( v.y ), static_cast<double>( v.z ) );
	return std::string( buf );
}

static std::string FormatPoint3( const Point3& p )
{
	char buf[ 96 ];
	std::snprintf( buf, sizeof( buf ), "%.17g %.17g %.17g", static_cast<double>( p.x ), static_cast<double>( p.y ), static_cast<double>( p.z ) );
	return std::string( buf );
}

// P5 Slice 3 expansion (csg transform): decompose a RIGID (translate+rotate, UNIT-scale) transform into
// position + Euler(XYZ, DEGREES), matching RISE's P*O composition (O = XRot*YRot*ZRot, Transformable::SetOrientation).
// Returns false on shear / non-unit scale / gimbal-lock / reflection.  A self-verifying local decompose;
// originally it mirrored the byte-splice SaveEngine's §9.5 TryDecompose, deleted in Slice 6d.  It REBUILDS the
// matrix RISE's way and compares, so a future composition-convention change fails SAFE (returns false -> the
// csg edit is refused, never mis-saved).  Used to commit a csg_object transform via its position/orientation params (csg
// authors no matrix/scale param).  Non-unit scale returns false because csg has no scale param to persist it.
//
// `outWhy` (optional) receives the reason the decompose refused -- and it names EVERY independent defect that
// fired, not just the first one to be reached.  The refusal the author reads is built from it, so an editor
// gate that reports every one of them as "gimbal-lock" tells someone who ran a pure TRANSLATE, on an object
// they never rotated, that their rotation hit a singularity.
//
// The SIX rejections (not-affine, SHEAR, REFLECTION, GIMBAL-LOCK, non-unit SCALE, reproduce-mismatch) are NOT
// mutually exclusive, which is why first-one-wins was not enough on its own.  `2 * Ry(90)` trips both the scale
// test and the gimbal test; reporting only the scale, with a trailing "the translate/rotate itself is fine",
// told the author something FALSE -- remove the scale and the gesture is still refused.  That clause is gone,
// and the reasons are ACCUMULATED so a multi-cause matrix is described as it actually is.
//
// The ORDER of the accumulated list is deliberate too.  A non-unit column magnitude is the check most likely
// to fire INCIDENTALLY -- a classic shear (col1 = (1,1,0)) has magnitude sqrt(2), so the author's real defect
// is the shear and the "scale" is a side effect of it -- so the scale reason is reported LAST when it shares
// the list.  Alone (much the commonest case) it is still the whole message.  And REFLECTION names the negative
// `scale` component that commonly produces it, since `scale -1 1 1` passes the magnitude test outright and the
// author would otherwise never see the param they wrote mentioned at all.
static bool DecomposeRigid( const Matrix4& M, Vector3& outPos, Vector3& outOrientDeg, std::string* outWhy = 0 )
{
	struct Local {   // one place that both accumulates the reasons and returns false, so the two cannot drift
		static void Add( std::string& w, const char* why ) { if( !w.empty() ) w += "; and "; w += why; }
		static bool No( std::string* out, const std::string& w ) { if( out ) *out = w; return false; }
	};
	std::string why;
	if( outWhy ) outWhy->clear();
	if( std::fabs( M._03 ) > 1e-9 || std::fabs( M._13 ) > 1e-9 ||
	    std::fabs( M._23 ) > 1e-9 || std::fabs( M._33 - 1.0 ) > 1e-9 )
		Local::Add( why, "the transform is not affine (its bottom row is not 0 0 0 1)" );
	const Vector3 c0( M._00, M._01, M._02 );
	const Vector3 c1( M._10, M._11, M._12 );
	const Vector3 c2( M._20, M._21, M._22 );
	const double s0 = Vector3Ops::Magnitude( c0 );
	const double s1 = Vector3Ops::Magnitude( c1 );
	const double s2 = Vector3Ops::Magnitude( c2 );
	const bool nonUnitScale = ( std::fabs( s0 - 1.0 ) > 1e-6 || std::fabs( s1 - 1.0 ) > 1e-6 || std::fabs( s2 - 1.0 ) > 1e-6 );
	// A zero-length axis cannot be normalised, so the orientation tests below have nothing to run on: dividing
	// by it poisons r0/r1/r2 with inf/NaN, and every comparison that follows silently evaluates FALSE against a
	// NaN, so they would contribute nothing but the risk of a garbage reason.  Report SINGULAR and stop.
	//
	// SINGULAR IS THEREFORE REPORTED ALONE -- it is the whole message for `scale 0 1 1`, and this early-out is
	// the reason.  That is a MESSAGE-QUALITY choice, not a correctness one, and an earlier comment here claimed
	// otherwise on both counts.  Delete this block and the gesture is still refused, by `nonUnitScale`, which is
	// unconditionally true whenever a column has zero magnitude (|0 - 1| > 1e-6); only the wording changes.  It
	// is kept because "its transform is SINGULAR (an axis of zero length)" tells the author what is actually
	// wrong, where "a non-unit SCALE is baked in" describes a degenerate matrix as if it were a resizeable one.
	if( s0 < 1e-12 || s1 < 1e-12 || s2 < 1e-12 ) {
		Local::Add( why, "its transform is SINGULAR (an axis of zero length), which position+orientation cannot express" );
		return Local::No( outWhy, why );
	}
	const Vector3 r0( c0.x / s0, c0.y / s0, c0.z / s0 );
	const Vector3 r1( c1.x / s1, c1.y / s1, c1.z / s1 );
	const Vector3 r2( c2.x / s2, c2.y / s2, c2.z / s2 );
	const double tol = 1e-6;
	if( std::fabs( Vector3Ops::Dot( r0, r1 ) ) > tol ||
	    std::fabs( Vector3Ops::Dot( r0, r2 ) ) > tol ||
	    std::fabs( Vector3Ops::Dot( r1, r2 ) ) > tol )
		Local::Add( why, "its transform contains SHEAR, which position+orientation cannot express" );
	if( Vector3Ops::Dot( r0, Vector3Ops::Cross( r1, r2 ) ) < 0.0 )
		Local::Add( why, "its transform contains a REFLECTION -- a negative determinant, from a `mirror` axis or a negative `scale` component -- which position+orientation cannot express" );
	// Euler extraction for RISE's Rx*Ry*Rz (the cell-by-cell derivation originally lived in the byte-splice
	// SaveEngine's §9.5 TryDecompose, deleted in Slice 6d).
	const double sin_y = r2.x;
	if( std::fabs( sin_y ) >= 1.0 - 1e-9 )
		Local::Add( why, "its ROTATION is at GIMBAL-LOCK (~90 degrees about Y), which Euler position+orientation cannot express" );
	if( nonUnitScale )   // reported LAST: see the ordering note above
		Local::Add( why, "a non-unit SCALE is baked into its transform, and a csg_object has no `scale` param to carry one" );
	if( !why.empty() ) return Local::No( outWhy, why );
	const double cos_y = std::sqrt( 1.0 - sin_y * sin_y );
	const double y_rad = std::atan2( sin_y, cos_y );
	const double x_rad = std::atan2( -r2.y, r2.z );
	const double z_rad = std::atan2( -r1.x, r0.x );
	// Rebuild EXACTLY the way Transformable composes and compare (binds correctness to RISE's actual rule).
	const Vector3 pos( M._30, M._31, M._32 );
	const Matrix4 cand = Matrix4Ops::Translation( pos )
	                   * ( Matrix4Ops::XRotation( x_rad ) * Matrix4Ops::YRotation( y_rad ) * Matrix4Ops::ZRotation( z_rad ) );
	for( int col = 0; col < 4; ++col ) for( int row = 0; row < 4; ++row ) {
		const Scalar* cp = &cand._00; const Scalar* mp = &M._00;
		const int k = col * 4 + row;
		if( std::fabs( static_cast<double>( cp[k] ) - static_cast<double>( mp[k] ) ) > 1e-6 )
			return Local::No( outWhy, std::string( "its transform does not reproduce from position+orientation (RISE composes position * XRot * YRot * ZRot)" ) );
	}
	outPos       = pos;
	outOrientDeg = Vector3( x_rad * 180.0 / PI, y_rad * 180.0 / PI, z_rad * 180.0 / PI );
	return true;
}

// True for the object transform ops a non-matrix object (csg) CAN represent: pure translate / rotate (no scale).
static inline bool IsObjectTranslateOrRotateOp( SceneEdit::Op op )
{
	return op == SceneEdit::TranslateObject
	    || op == SceneEdit::RotateObjectArb
	    || op == SceneEdit::SetObjectPosition
	    || op == SceneEdit::SetObjectOrientation;
}

static inline bool IsCstRoutedOp( SceneEdit::Op op )
{
	// CST-routed = the entity is RE-DERIVED (not mutated in place) on a CST-loaded scene, so the remove+re-add
	// serial guard must SKIP it (its serial legitimately changes each edit) and it applies/reverts/redoes BY
	// NAME.  Material/light/camera + object bindings + object shadow flags route PER-OP; object TRANSFORM ops
	// commit to the `matrix` param at the composite/edit boundary -- all of IsObjectOp is re-derived on a CST
	// scene, so all of it skips the guard.
	return op == SceneEdit::SetMaterialProperty
	    || op == SceneEdit::SetLightProperty
	    || op == SceneEdit::SetCameraProperty
	    || op == SceneEdit::SetMediumProperty
	    || op == SceneEdit::SetAgentCstParam    // shared-undo U1: agent edits re-derive by name too
	    || op == SceneEdit::AgentInsertChunk    // shared-undo U2: agent chunk CRUD re-derives (full re-derive) too
	    || op == SceneEdit::AgentRemoveChunk
	    || op == SceneEdit::AgentRemoveChunks   // R1a: the batch remove re-derives by name too
	    || op == SceneEdit::AgentReplaceGeometry   // R2: the composite geometry replace re-derives wholesale
	    || op == SceneEdit::AgentDuplicateNode     // doc-88 S20: the positioned duplicate re-derives wholesale too
	    || SceneEdit::IsObjectOp( op )
	    || SceneEdit::IsCameraOp( op );   // camera DRAG ops re-derive at the pose-commit boundary too
}

// P5 Slice 3 expansion (medium): only the homogeneous_medium chunk params that EXIST are routable -- absorption
// and scattering.  `emission` is editable on the live HomogeneousMedium but the homogeneous_medium SCENE chunk
// has NO emission param (see MediaIntrospection), so routing it would insert a param the descriptor-driven
// parser rejects on re-derive -> the edit would be wrongly REFUSED.  Emission therefore falls through to the
// direct mutate (live-but-transient -- a pre-existing, documented limitation, not closed here).
static inline bool IsCstRoutableMediumProp( const String& prop )
{
	return prop == String( "absorption" ) || prop == String( "scattering" );
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
	const Scalar lastSetTime = mLastSetTime.load( std::memory_order_acquire );
	if( mScene && lastSetTime != 0 ) mScene->SetSceneTimeForPreview( lastSetTime );
}

// P5 Slice 3 expansion: shared CST-routing for a property edit (material/light/...).  Mirrors the material
// branch -- DocSetOrAddParamValue + re-derive via Job::ApplyCstParamEdit; rebind on a D2 (result >=2); a
// diagnosed re-derive (3) rebinds but reports failure.
bool SceneEditor::RouteCstParamEdit_( const char* entityName, const char* entityKind, const char* role, const char* value )
{
	const int r = mJob->ApplyCstParamEdit( entityName, entityKind, role, 0, value );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;   // code 1/2/3 all MUTATED the live scene -> the controller must re-render
	return r == 1 || r == 2;
}

// Light-colour CST composite (2026-09-02, round 2) -- see the header doc for the contract.  THE BUG this
// closes: the panel's `color` row is `ILight::emissionColor()`, i.e. the light's LIVE, already-converted LINEAR
// RISEPel.  The CST route writes those digits verbatim into the chunk and re-derives -- and the re-derive
// reads them through whatever `colorspace` the chunk still spells.  On a chunk carrying `colorspace sRGB`
// (which is EVERY light `tools/migrate_scenes_light_colorspace.py` touched, since that is exactly how the
// migrator preserved pre-2026-09-02 looks) the linear digits get gamma-DECODED a SECOND time: nudging a
// swatch that reads (1, 0.0331, 0.0194) up to (1, 0.05, 0.02) lands the light on (1, 0.0039, 0.0016) --
// an order of magnitude the wrong way, and it compounds on every subsequent nudge.
//
// The fix is to make the chunk agree with the panel rather than to re-encode the value: write
// `colorspace Rec709RGB_Linear` alongside the colour, converting the chunk to the linear convention the
// first time anyone edits its colour.  Look-preserving (the digits written ARE the light's current linear
// colour, so the light does not move except by the edit the user asked for).  Re-encoding the panel value
// back into sRGB was the alternative and is worse: it silently keeps a second, invisible convention alive in
// the Document, and it is lossy (the OETF round-trip is not exact at the ends of the range).
//
// This function answers the QUESTION ("does this edit need the composite, and what did the chunk say before
// it?"); RouteCstLightColorComposite_ performs the write.  The two ORIGINAL texts it hands back are what the
// history entry carries so Undo can put the chunk back BYTE-IDENTICALLY -- including the `sRGB` spelling.
// That is load-bearing rather than tidy: an AGENT history entry captures RAW CHUNK TEXT (see
// SceneEditController::CaptureAgentPriorParamValue_), so an Undo that left the chunk linear would make an
// older agent entry replay sRGB digits under a linear chunk -- the light lands ~6x too bright, in the
// headline shared-undo (agent + user) flow.
//
// Read LAST occurrence for the presence/convention test (Cst::ParamValueAsParsed) -- that is the one the
// derive reads, the same last-wins rule CstIntrospection's rows follow.  Read the values to RESTORE at
// occurrence 0 (Cst::ParamValueAtOccurrence), because occurrence 0 is where the write will land; the two
// coincide for every chunk this composite can actually fire on, since Job::ApplyCstParamEdits REFUSES a
// chunk that spells either param twice (its duplicate-occurrence guard) before anything is written.
bool SceneEditor::LightColorCompositeState_( const char* lightName, const String& propertyName,
                                             String& outPrevColorText, String& outPrevColorSpaceText ) const
{
	outPrevColorText      = String();
	outPrevColorSpaceText = String();
	if( !( propertyName == String( "color" ) ) ) return false;   // only the colour row carries the convention
	if( !mJob || !lightName || !lightName[0] ) return false;
	const RISE::Cst::Document* doc = mJob->GetCstDocument();
	if( !doc ) return false;                                     // legacy (no-Document) scene: nothing to convert
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( *doc, lightName, nullptr, "light", /*uniqueFallback*/ false );
	if( id == 0 ) return false;   // not addressable in the Document -- let the colour write report that itself
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return false;

	// Absent == the descriptor default, which IS linear -- nothing to convert.
	bool present = false;
	const std::string cspace = RISE::Cst::ParamValueAsParsed( chunk, "colorspace", &present );
	if( !present || TrimTrivia_( cspace ) == "Rec709RGB_Linear" ) return false;

	// A chunk carrying `colorspace` but NO `color` line still needs the composite -- the panel's write
	// INSERTS a colour line, and inserting linear digits under a non-linear `colorspace` is the very
	// double-decode this closes.  Its original colour is the descriptor default `0 0 0` (see the four
	// light parsers' `double color[3] = {0,0,0}`), so that is what Undo restores -- and `0 0 0` is a
	// FIXED POINT of every colour space the language accepts (the sRGB and ProPhoto EOTFs map 0 to 0;
	// the ROMM/Rec.709 matrices are linear), so writing it back under the restored spelling reproduces
	// the original light EXACTLY.  This is the one shape where Undo is not byte-identical: the chunk
	// gains an explicit `color 0 0 0` line where it previously relied on the default.  Restoring
	// "absent" would need a param REMOVE, which is not expressible in a value-write batch, and the
	// explicit line means exactly what the absent one did.
	bool colorPresent = false;
	std::string prevColor = RISE::Cst::ParamValueAtOccurrence( chunk, "color", 0, &colorPresent );
	if( !colorPresent || TrimTrivia_( prevColor ).empty() ) prevColor = "0 0 0";
	bool cspacePresent = false;
	const std::string prevCspace = RISE::Cst::ParamValueAtOccurrence( chunk, "colorspace", 0, &cspacePresent );
	if( !cspacePresent ) return false;

	outPrevColorText      = String( TrimTrivia_( prevColor ).c_str() );
	outPrevColorSpaceText = String( TrimTrivia_( prevCspace ).c_str() );
	if( outPrevColorText.size() <= 1 || outPrevColorSpaceText.size() <= 1 ) return false;
	return true;
}

// The write half of the composite.  ONE Job::ApplyCstParamEdits call = one Document copy, both params
// validated against the pristine head BEFORE either is written, one derive.  The two-call version this
// replaced re-derived after EACH write with no rollback, so a refused colour write (a malformed value from
// the public SetPropertyForCategory entry, or a chunk with duplicate `color` lines) left the chunk converted
// to linear with the AUTHORED sRGB digits still in place -- the light jumped to those digits read as linear,
// nothing was pushed to history, and a re-render was kicked on a state no undo could reach.
bool SceneEditor::RouteCstLightColorComposite_( const char* lightName, const char* colorValue, const char* colorSpaceValue )
{
	std::vector< std::pair< std::string, std::string > > pairs;
	// `colorspace` first only for readability of the resulting diagnostic; the batch is atomic, so order
	// carries no failure semantics (unlike the two-call version it replaced, where order was the only
	// mitigation available).
	pairs.push_back( std::make_pair( std::string( "colorspace" ), std::string( colorSpaceValue ) ) );
	pairs.push_back( std::make_pair( std::string( "color" ),      std::string( colorValue ) ) );
	const int r = mJob->ApplyCstParamEdits( lightName, "light", pairs );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;   // code 1/2/3 all MUTATED the live scene -> the controller must re-render
	return r == 1 || r == 2;
}

// Shared-undo U1: the inverse of an agent param edit that INSERTED a previously-absent param (SetAgentCstParam's
// prevValueWasAbsent case) -- removes the param instead of re-setting a (nonexistent) prior value.  Mirrors
// RouteCstParamEdit_ exactly (rebind on >=2, mCstLiveSceneChanged on != 0); only the Job entry point differs
// (ApplyCstParamRemoveChecked, always full-derivability-gated -- agent-originated Document mutations never take
// the ungated fast path).  `occ` (P1-2 fix, round 1) selects WHICH occurrence to remove (0 = first).  The sole
// caller (ApplyRevertMutation's SetAgentCstParam arm) passes the edit's OWN recorded occurrence
// (SceneEdit::cstParamOcc, doc 88 S4b) -- 0 for every param the document spells once, which is what that caller
// hardcoded before S4b, and the real index for an occurrence-addressed row edit.
//
// P1-3 fix (round 1): the return is keyed on MUTATION, not cleanliness -- r==1 (incremental), r==2 (clean full
// re-derive), and r==3 (full re-derive that DIAGNOSED) ALL mutated + rebound the retained Document (a code-3
// DeriveEditedCstDocument_ commits the Document and replaces the Scene/managers UNCONDITIONALLY -- fdiags is
// only checked to pick the return code, not to gate the commit); only r==0 (reject) left the head untouched.
// The pre-fix `r == 1 || r == 2` treated a code-3 revert as "didn't happen", but SceneEditor::Undo()/Redo() had
// ALREADY moved the entry across the undo/redo stacks (PopForUndo/PopForRedo) before calling this -- a false
// return there triggers RestoreLastUndoFromRedo()/RestoreLastRedoFromUndo(), moving the entry BACK even though
// the Document was already mutated. That leaves history and Document disagreeing about whether the revert
// happened, and a retried Cmd-Z double-reverts (or wedges) the entry. Callers that need to know the diagnosed
// case for logging read `outDiagnosed` (non-null only where SceneEditor.cpp needs it); everyone else can ignore
// it -- the honest "did the mutation land" answer is the return value alone.
bool SceneEditor::RouteCstParamRemove_( const char* entityName, const char* entityKind, const char* role, int occ, bool* outDiagnosed )
{
	if( outDiagnosed ) *outDiagnosed = false;
	const int r = mJob->ApplyCstParamRemoveChecked( entityName, entityKind, role, occ );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;
	if( r == 3 && outDiagnosed ) *outDiagnosed = true;
	return r >= 1;
}

// Shared-undo U1: the FULL-DERIVABILITY-GATED twin of RouteCstParamEdit_, used ONLY by SetAgentCstParam's
// Undo/Redo (both directions, not just the absent-param Remove arm above).  Every other CST-routed op's
// Undo/Redo (SetMaterialProperty, SetLightProperty, SetCameraProperty, SetMediumProperty) safely uses the
// ungated RouteCstParamEdit_ because a GUI value edit has no adjacent mutation channel that can silently
// invalidate a previously-proven-safe reference between the original Apply and a later Redo.  SetAgentCstParam
// does NOT have that safety: Model-B F5's agent chunk-CRUD verbs (SceneEditController::ApplyAgentChunkCrud_ ->
// Job::ApplyCstInsertChunk / ApplyCstRemoveChunk) mutate the retained Document WITHOUT ever touching mHistory
// (no Push, no redo-stack clear -- see that method's own comment on why: chunk CRUD has no EditHistory record
// at all yet, slice U2's scope).  So a chunk insert/remove/reorder can land BETWEEN an agent param edit and a
// later Undo/Redo of it without invalidating the redo entry, changing whether a previously-forward-safe
// reference is still forward-safe.  Re-applying a stale value through the UNGATED incremental-only path in that
// window would validate only against the live managers and could silently re-commit a head that fails to
// re-derive from serialized bytes -- exactly what the gate exists to prevent.  Cost: one extra throwaway
// full-derive dry-run per Undo/Redo of an agent edit (not the GUI hot path) -- acceptable for a discrete,
// infrequent operation.
//
// P1-3 fix (round 1): same mutation-keyed return as RouteCstParamRemove_ above -- see its doc for the full
// rationale (a code-3 diagnosed re-derive still mutated + rebound the Document, so it must count as "the
// revert/redo happened" for the history-stack bookkeeping, even though it is reported to the CALLER of the
// forward-path ApplyAgentParamEdit as a failure).  `outDiagnosed` (non-null only where the caller logs it)
// reports the code-3 case; the return value alone answers "did the mutation land".
bool SceneEditor::RouteCstParamEditChecked_( const char* entityName, const char* entityKind, const char* role, const char* value, int occ, bool* outDiagnosed )
{
	if( outDiagnosed ) *outDiagnosed = false;
	const int r = mJob->ApplyCstParamEditChecked( entityName, entityKind, role, occ, value );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;
	if( r == 3 && outDiagnosed ) *outDiagnosed = true;
	return r >= 1;
}

namespace {
// doc 88 S4b round 1 (P1 fix): whitespace-NORMALISE a param value the same way a Document WRITE does, not
// just trim its ends.  Cst::WithParamValue (the sole writer behind ApplyCstParamEditChecked) re-tokenises
// the incoming value on ANY whitespace run and re-emits the tokens joined by a SINGLE space -- so interior
// spacing is not preserved by a write and therefore cannot carry drift information either.  A column-aligned
// value (`stop 0.5   0.44 0.54 0.64`) or a slider commit that sends the rest-of-line bytes verbatim reads
// back with its ORIGINAL interior spacing (the read side, ParamValueAtOccurrence, is a pure token-join over
// whatever trivia the Document actually holds -- untouched until the next write), while `expectedValue` was
// captured before that first write already went through the single-space join.  Comparing those two
// verbatim -- as an ends-only trim did -- makes Undo/Redo of a perfectly legitimate, unchanged column-aligned
// or slider-committed line refuse as "drifted".  The fix: split both sides into whitespace-delimited tokens
// and rejoin with a single space each -- exactly what a write would have produced -- before comparing.  This
// keeps TOKEN-level drift detection fully intact (a token added, removed, or changed still fails the
// comparison); only the whitespace BETWEEN tokens, which no write path preserves, stops being load-bearing.
std::string NormalizeParamValueWs_( const std::string& s )
{
	std::string out;
	size_t i = 0, n = s.size();
	bool first = true;
	while( i < n ) {
		while( i < n && ( s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' ) ) ++i;
		const size_t st = i;
		while( i < n && !( s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' ) ) ++i;
		if( i > st ) {
			if( !first ) out += ' ';
			out.append( s, st, i - st );
			first = false;
		}
	}
	return out;
}
}  // namespace

// doc 88 S4b: the occurrence-addressed undo/redo drift guard -- see the header doc for the contract.
// Resolution is CaptureAgentPriorParamValue_'s / ApplyCstParamEditImpl_'s verbatim (same
// DocFindByNameAnyRole call, same kind-addressed-singleton fallback rule, same DocTransformOwnerId owner
// walk), because a guard that resolved a DIFFERENT chunk than the write would either wave through a real
// drift or refuse a perfectly good Undo.  The camera-unique fallback is deliberately NOT reproduced: this
// guard only ever runs for an occurrence-addressed edit, which today is reachable only from the Painter
// category (SceneEditController's bracket parser is scoped there), and a painter chunk is always named.
// The `entityName[0]=='\0' && kind` singleton arm IS reproduced, so a future occurrence-addressed edit on
// an unnamed singleton chunk resolves the same way the write would.
bool SceneEditor::OccurrenceEditStillAddressable_( const SceneEdit& edit, const char* expectedValue, const char* direction ) const
{
	if( !edit.cstParamOccAddressed ) return true;   // every pre-S4b edit: unguarded, exactly as before
	if( !mJob ) return false;
	const RISE::Cst::Document* doc = mJob->GetCstDocument();
	if( !doc ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditor::%s:: occurrence-addressed edit on `%s`.`%s[%d]` cannot be verified -- no retained CST "
			"Document; refused rather than writing blind.",
			direction, edit.objectName.c_str(), edit.propertyName.c_str(), edit.cstParamOcc );
		return false;
	}

	const std::string ekind( edit.cstEntityKind.size() > 1 ? edit.cstEntityKind.c_str() : "" );
	const char* bareName = edit.objectName.size() > 1 ? edit.objectName.c_str() : "";
	const bool uniqueFallback = ( bareName[0] == '\0' && !ekind.empty() );
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( *doc, bareName, nullptr, ekind, uniqueFallback );
	if( id == 0 ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditor::%s:: occurrence-addressed edit on `%s`.`%s[%d]` refused -- the entity no longer resolves "
			"in the CST Document (removed or renamed since the edit).",
			direction, edit.objectName.c_str(), edit.propertyName.c_str(), edit.cstParamOcc );
		return false;
	}
	const std::string role( edit.propertyName.c_str() );
	const RISE::Cst::NodeId ownerId = RISE::Cst::DocTransformOwnerId( *doc, id, role );
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, ownerId );
	if( !chunk ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditor::%s:: occurrence-addressed edit on `%s`.`%s[%d]` refused -- the owning chunk no longer "
			"resolves.", direction, edit.objectName.c_str(), edit.propertyName.c_str(), edit.cstParamOcc );
		return false;
	}

	// (a) EXISTENCE.  `stop[2]` on a chunk that now has two stops has nowhere to land: DocSetOrAddParamValue
	// would no-op (its INSERT arm is occurrence-0-only), Job would report code 0, and the caller would already
	// fail -- but silently, and only after the derive gate had run.  Say so here instead.
	const int count = RISE::Cst::ParamOccurrenceCount( chunk, role );
	if( edit.cstParamOcc < 0 || edit.cstParamOcc >= count ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditor::%s:: occurrence-addressed edit on `%s`.`%s[%d]` refused -- `%s` now has %d occurrence(s) "
			"on that chunk, so occurrence %d no longer exists.  The repeatable parameter's LAYOUT changed since "
			"the edit (an agent chunk edit, or another occurrence edit); refusing rather than writing a "
			"neighbouring line.",
			direction, edit.objectName.c_str(), edit.propertyName.c_str(), edit.cstParamOcc,
			role.c_str(), count, edit.cstParamOcc );
		return false;
	}

	// (b) IDENTITY.  Existence alone is not enough: an INSERT before this occurrence keeps the count high
	// while shifting every later line down by one, so occurrence N exists and is the WRONG line.  Compare
	// against the value this direction is entitled to overwrite -- the U2 expectedChunkBytes philosophy, at
	// param-line granularity.  Whitespace-normalised (token-split + single-space rejoin) on both sides
	// because a Document write re-tokenises and single-space-joins the value while the captured string and
	// the live-read string each carry whatever separators their own origin left in place -- see
	// NormalizeParamValueWs_'s doc above for why interior spacing cannot be a drift signal.
	const std::string current = NormalizeParamValueWs_( RISE::Cst::ParamValueAtOccurrence( chunk, role, edit.cstParamOcc ) );
	const std::string expect  = NormalizeParamValueWs_( expectedValue ? expectedValue : "" );
	if( current != expect ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditor::%s:: occurrence-addressed edit on `%s`.`%s[%d]` refused -- that line now reads `%s` but "
			"this %s expected `%s`.  The repeatable parameter drifted since the edit; refusing rather than "
			"overwriting a line this history entry never wrote.",
			direction, edit.objectName.c_str(), edit.propertyName.c_str(), edit.cstParamOcc,
			current.c_str(), direction, expect.c_str() );
		return false;
	}
	return true;
}

// Shared-undo U2: route an AgentInsertChunk/AgentRemoveChunk Undo or Redo through Job's chunk-CRUD
// primitives.  Every one of Job::ApplyCstInsertChunk / ApplyCstRemoveChunk / ApplyCstRestoreChunkAt is
// ALREADY full-derivability-gated (they route through the shared RederiveCstDocumentFull_ tail, which
// ALWAYS dry-runs before committing -- unlike ApplyCstParamEdit's OPTIONAL gate, chunk CRUD has no ungated
// fast path to begin with), so -- unlike RouteCstParamEditChecked_/RouteCstParamRemove_, which exist
// specifically to select the gated Job entry point over an ungated sibling -- this helper's job is purely
// dispatch: pick the right Job call for (which op, which direction) and fold its 0/2/3 return the same
// mutation-keyed way (r>=1 => "the mutation landed", including a diagnosed code-3).  Never 1 for any of
// the three calls (a chunk-CRUD verb is always D2-class), so `r >= 1` and `r >= 2` are equivalent here;
// written as `r != 0` to mirror the sibling helpers' shape.
bool SceneEditor::RouteAgentChunkCrud_( const SceneEdit& edit, bool forInsertOp, bool forward, bool* outDiagnosed )
{
	if( outDiagnosed ) *outDiagnosed = false;
	if( !mJob ) return false;
	const char* kind = edit.cstEntityKind.size() > 1 ? edit.cstEntityKind.c_str() : nullptr;
	int r = 0;
	if( forInsertOp && forward )
	{
		// Redo of an insert: re-insert the SAME captured bytes.  Deterministic: Undo (the remove below)
		// restored the pre-insert Document byte-identically, so re-running the positioned insert on that
		// identical Document lands the same way it did the first time.
		char kwBuf[128]; kwBuf[0] = '\0';
		char nameBuf[256]; nameBuf[0] = '\0';
		char diagBuf[512]; diagBuf[0] = '\0';
		r = mJob->ApplyCstInsertChunk( edit.propertyValue.c_str(), kwBuf, sizeof( kwBuf ), nameBuf, sizeof( nameBuf ), diagBuf, sizeof( diagBuf ) );
		if( r < 0 ) r = 0;   // fold malformed/duplicate refusals into the ordinary "would not derive" bucket
	}
	else if( forInsertOp && !forward )
	{
		// Undo of an insert: remove EXACTLY the 3 items ApplyCstInsertChunk spliced ([leadSep][chunk]
		// [trailSep]) at the captured EXACT index -- NOT the general Cst::DocEraseChunkTidy heuristic a
		// manual remove_chunk uses (which collapses at most ONE adjacent separator and -- correctly, by its
		// own contract -- leaves a residual blank line on a chunk that had TWO fresh separators inserted
		// around it; see AgentChunkCrudTest.cpp's T3).  This keeps Undo-of-insert byte-identical to the
		// pre-insert Document, matching Undo-of-remove's own byte-identity bar.  A still-REFERENCED chunk
		// (something now names it) fails the dry-run inside ApplyCstRemoveItemsAt's re-derive tail and
		// refuses honestly (r==0), leaving the Document untouched -- the derivability gate this whole slice
		// exists to enforce (SAME guarantee ApplyCstRemoveChunk gives; just a different Document-splice
		// mechanism to reach it).
		// Round-1 P2: also pass `edit.propertyValue` (the agent's exact original chunk text) as
		// `expectedChunkBytes` -- ApplyCstRemoveItemsAt is otherwise a pure positional splice with NO identity
		// check, so an out-of-band mutation between this insert and its Undo that shifts `agentChunkIndex` to
		// a STALE-but-in-range position (a different, unrelated triple now sitting there) would otherwise let
		// the dry-run below happily commit deleting the WRONG content whenever that wrong triple happens to
		// derive cleanly (any well-formed, unreferenced triple does).  A mismatch refuses honestly (r==0,
		// Document byte-unchanged) instead.
		char diagBuf[512]; diagBuf[0] = '\0';
		r = mJob->ApplyCstRemoveItemsAt( edit.agentChunkIndex, /*count*/3, diagBuf, sizeof( diagBuf ),
		                                  edit.propertyValue.c_str() );
	}
	else if( !forInsertOp && !forward )
	{
		// Undo of a remove: splice the captured bytes back at the captured EXACT index.
		char diagBuf[512]; diagBuf[0] = '\0';
		r = mJob->ApplyCstRestoreChunkAt( edit.propertyValue.c_str(), edit.agentChunkIndex,
		                                  /*restoreActiveRasterizer*/ !edit.agentChunkWasRasterizer,
		                                  diagBuf, sizeof( diagBuf ) );
	}
	else
	{
		// Redo of a remove: re-remove by (name, kind) -- deterministic for the same reason insert-Redo is:
		// Undo restored the pre-remove Document byte-identically first.
		char kwBuf[128]; kwBuf[0] = '\0';
		char diagBuf[512]; diagBuf[0] = '\0';
		r = mJob->ApplyCstRemoveChunk( edit.objectName.c_str(), kind, kwBuf, sizeof( kwBuf ), diagBuf, sizeof( diagBuf ) );
		if( r < 0 ) r = 0;
	}
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;
	if( r == 3 && outDiagnosed ) *outDiagnosed = true;
	return r >= 1;
}

// R1a (2026-08-09, batched remove_chunks): the AgentRemoveChunks sibling of RouteAgentChunkCrud_ -- see the
// header doc for why it is a separate helper.  Both directions end in a Job primitive that is ALREADY
// full-derivability-gated (both route through the shared RederiveCstDocumentFull_ tail, which always dry-runs
// before committing), so this helper is pure dispatch + the same 0/2/3 fold every sibling uses.
bool SceneEditor::RouteAgentRemoveChunksBatch_( const SceneEdit& edit, bool forward, bool* outDiagnosed )
{
	if( outDiagnosed ) *outDiagnosed = false;
	if( !mJob ) return false;
	int r = 0;
	if( !forward )
	{
		// UNDO: restore the byte-exact pre-batch document text wholesale.  `restoreActiveRasterizer` is the
		// INVERSE of "the batch removed a rasterizer chunk", mirroring AgentRemoveChunk's own P1-B rule --
		// re-introducing ANY `*_rasterizer` chunk changes the document's last-wins activation, so the
		// pre-erase active rasterizer must NOT be restored over it.
		char diagBuf[512]; diagBuf[0] = '\0';
		r = mJob->ApplyCstReplaceDocumentText( edit.propertyValue.c_str(),
		                                       /*restoreActiveRasterizer*/ !edit.agentChunkWasRasterizer,
		                                       diagBuf, sizeof( diagBuf ) );
	}
	else
	{
		// REDO: re-run the whole batch by (kind, name), parsed back out of the recorded descriptor lines.
		// Deterministic for the same reason the singular remove-Redo is: Undo restored the pre-batch Document
		// byte-identically first, so the same targets resolve to the same chunks.
		std::vector<std::string> names, kinds;
		{
			const std::string lines( edit.prevPropertyValue.c_str() );
			std::size_t at = 0;
			for( ;; )
			{
				const std::size_t nl = lines.find( '\n', at );
				const std::string line = lines.substr( at, ( nl == std::string::npos ) ? std::string::npos : nl - at );
				if( !line.empty() )
				{
					const std::size_t tab = line.find( '\t' );
					// A line with no tab would be a malformed record (never produced by the sole caller);
					// read the whole line as the name with no kind rather than drop the target silently.
					if( tab == std::string::npos ) { kinds.push_back( std::string() ); names.push_back( line ); }
					else { kinds.push_back( line.substr( 0, tab ) ); names.push_back( line.substr( tab + 1 ) ); }
				}
				if( nl == std::string::npos ) break;
				at = nl + 1;
			}
		}
		if( names.empty() ) return false;
		std::vector<const char*> namePtrs, kindPtrs;
		namePtrs.reserve( names.size() );
		kindPtrs.reserve( kinds.size() );
		for( std::size_t i = 0; i < names.size(); ++i ) {
			namePtrs.push_back( names[i].c_str() );
			kindPtrs.push_back( kinds[i].empty() ? nullptr : kinds[i].c_str() );
		}
		char kwBuf[1024]; kwBuf[0] = '\0';
		char diagBuf[512]; diagBuf[0] = '\0';
		r = mJob->ApplyCstRemoveChunks( &namePtrs[0], &kindPtrs[0], static_cast<int>( namePtrs.size() ),
		                                kwBuf, sizeof( kwBuf ), diagBuf, sizeof( diagBuf ), nullptr );
		if( r < 0 ) r = 0;   // fold not-found/ambiguous refusals into the ordinary "would not derive" bucket
	}
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;
	if( r == 3 && outDiagnosed ) *outDiagnosed = true;
	return r >= 1;
}

// R2 (2026-08-10, replace_geometry_scaffold): the WHOLE-DOCUMENT prior/post text-swap route -- the
// AgentReplaceGeometry sibling of RouteAgentRemoveChunksBatch_, see the header doc.  Both directions call
// the SAME already-full-derivability-gated Job primitive on DIFFERENT recorded bytes, so this is pure
// dispatch + the same 0/2/3 fold every sibling uses.  `restoreActiveRasterizer` is unconditionally TRUE:
// neither op's composite creates or erases a `*_rasterizer` chunk, so the document's last-wins activation is
// invariant across the swap (see each op doc's `agentChunkWasRasterizer` field note).
//
// doc-88 Phase 3 S20: SHARED with AgentDuplicateNode (the canvas's positioned Duplicate-node fork), which
// has the identical undo shape -- hence the rename off the geometry-specific name.  The op only selects the
// LOG LABEL; the mechanism is byte-for-byte the same, which is the point of recording a document PAIR
// rather than a verb to replay.
bool SceneEditor::RouteAgentDocumentSwap_( const SceneEdit& edit, bool forward, bool* outDiagnosed )
{
	if( outDiagnosed ) *outDiagnosed = false;
	if( !mJob ) return false;
	const String& text = forward ? edit.prevPropertyValue : edit.propertyValue;
	if( text.size() <= 1 ) return false;   // RString::size() counts the trailing NUL; <=1 is empty
	const bool dup = ( edit.op == SceneEdit::AgentDuplicateNode );
	char diagBuf[512]; diagBuf[0] = '\0';
	const int r = mJob->ApplyCstReplaceDocumentText( text.c_str(), /*restoreActiveRasterizer*/ true,
	                                                 diagBuf, sizeof( diagBuf ),
	                                                 dup ? ( forward ? "duplicate_graph_node (redo)"
	                                                                 : "duplicate_graph_node (undo)" )
	                                                     : ( forward ? "replace_geometry_scaffold (redo)"
	                                                                 : "replace_geometry_scaffold (undo)" ) );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;
	if( r == 3 && outDiagnosed ) *outDiagnosed = true;
	return r >= 1;
}

// P5 Slice 3 expansion (object): a shadow-flags edit maps to TWO standard_object bool params, so route both
// (casts_shadows then receives_shadows).  `flags` bit0 = casts, bit1 = receives (the SetObjectShadowFlags
// encoding).  Each route re-derives; RouteCstParamEdit_ rebinds on a D2, and the retained Document is re-retained
// across the ClearAll (reset DURING ClearAll, re-retained immediately after inside DeriveEditedCstDocument_), so
// the second route sees the first param already applied.  Returns false if EITHER route fails.
bool SceneEditor::RouteObjectShadowFlagsToCst_( const String& objectName, int flags )
{
	const char* casts = ( flags & 1 ) ? "true" : "false";
	const char* recvs = ( flags & 2 ) ? "true" : "false";
	bool ok = RouteCstParamEdit_( objectName.c_str(), "standard_object", "casts_shadows", casts );
	ok = RouteCstParamEdit_( objectName.c_str(), "standard_object", "receives_shadows", recvs ) && ok;
	return ok;
}

// P5 Slice 3 expansion (object transform): a transform op direct-mutates the live object now (cheap, so a gizmo
// drag stays responsive) and NOTES the object for a deferred `matrix`-param commit at the composite/edit
// boundary.  Only on a CST-loaded scene (else there is no Document to keep in sync).
void SceneEditor::NoteCstObjectTransform_( const String& name )
{
	mPendingCstObjMatrix.insert( std::string( name.c_str() ) );
}

// Route ONE object's net transform as the standard_object `matrix` param (Job strips the dead component params
// first).  Rebinds this editor on a D2 full re-derive (result >=2), mirroring RouteCstParamEdit_.
bool SceneEditor::ApplyCstObjectMatrix_( const std::string& name, const std::string& matrix16 )
{
	const int r = mJob->ApplyCstObjectMatrixEdit( name.c_str(), matrix16.c_str() );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;   // code 1/2/3 all MUTATED the live scene -> the controller must re-render
	return r == 1 || r == 2;
}

// Route ONE csg_object's translate+rotate as its position + orientation params (rebinds on a D2).
bool SceneEditor::ApplyCstObjectComponents_( const std::string& name, const std::string& position, const std::string& orientation )
{
	const int r = mJob->ApplyCstObjectComponentsEdit( name.c_str(), position.c_str(), orientation.c_str() );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;   // code 1/2/3 all MUTATED the live scene -> the controller must re-render
	return r == 1 || r == 2;
}

// P5 Slice 3 expansion (object transform): commit every pending object's LOCAL transform -- what its chunk
// authors, NOT its composed world matrix (87; see the body) -- to the retained CST
// as the authoritative `matrix` param.  Called by the CONTROLLER at a render-thread-PARKED boundary (a commit
// re-derives, which on a variant scene ClearAll's the live scene -- it must NOT race a render worker).  The
// per-frame gizmo edits only NOTE objects (NoteCstObjectTransform_); this single flush at drag-end / panel-edit /
// undo / redo does the one re-derive.  Snapshot EVERY target matrix BEFORE routing any -- the first route's D2
// rebuilds the scene from the Document, which would reset a not-yet-routed object's live transform to its (stale)
// Document value.  Returns false if any route failed.  Two failure shapes, OPPOSITE Document states (A2):
// code 0 = the Document did NOT record the live transform -- a later full re-derive (D2) rebuilds the live
// scene FROM the Document and therefore REVERTS the unrecorded transform (the divergence resolves by LOSING
// the live edit, not by recording it); code 3 = the Document DID record it but the live re-derive diagnosed.
// Either way the live edit stands right now and the failure is logged.
bool SceneEditor::CommitPendingCstObjectTransforms()
{
	if( mPendingCstObjMatrix.empty() ) return true;
	// Snapshot EACH pending object's route BEFORE applying ANY -- the first route's D2 rebuilds the scene from the
	// Document, which would reset a not-yet-routed object's live transform to its (stale) Document value.  Per
	// object: kind 1 (standard_object) commits the full `matrix`; kind 2 (csg_object) commits decomposed position +
	// orientation (the editor's apply-gate + its post-mutate decomposability check already refused any csg
	// scale/shear/gimbal-lock, so DecomposeRigid is guaranteed to succeed here).
	struct Route { std::string name; int kind; std::string a; std::string b; bool ready; };
	std::vector<Route> work;
	work.reserve( mPendingCstObjMatrix.size() );
	for( std::unordered_set<std::string>::const_iterator it = mPendingCstObjMatrix.begin(); it != mPendingCstObjMatrix.end(); ++it ) {
		IObjectPriv* obj = FindObject( String( it->c_str() ) );
		if( !obj ) continue;
		// 87: commit the LOCAL matrix -- what the chunk actually authors.  The
		// re-derive re-composes `parentWorld * local`, so committing the WORLD
		// matrix of a parented object would bake its parent's transform into
		// its own chunk and the next derive would apply that transform a SECOND
		// time.  That is precisely the failure 86 hit (§3, "the GUI transform
		// commit SQUARED the group matrix") and had to paper over with a G^-1
		// division that could not be done at all when G was singular.  Reading
		// the local matrix back needs no inverse and has no degenerate case.
		Matrix4 M;
		// doc 89 slice C: STRIP THE MIRROR before committing.  The local matrix is
		// `P * O * Stretch * Scale * Mirror`, but `mirror` is NOT one of the params
		// ApplyCstObjectMatrixEdit strips beside the `matrix` it writes -- it is not
		// in standard_object's matrix > quaternion > orientation precedence chain at
		// all, it composes with whichever of them wins.  So a `matrix` that already
		// contained the reflection would be re-multiplied by the chunk's surviving
		// `mirror` line on the next derive and the object would come back reflected
		// TWICE.  A reflection is its own inverse, so right-multiplying by the same
		// mirror recovers `P * O * Stretch * Scale` EXACTLY -- no fitting, no
		// tolerance -- which is precisely what the chunk's other transform params
		// mean and what the derive will re-compose the mirror onto.
		//
		// (The kind-2 / csg branch below needs no equivalent: DecomposeRigid REFUSES
		// a negative determinant outright, and the apply-time gate above has already
		// restored and refused the gesture before it could reach here -- and
		// Job::SetObjectMirror refuses a csg a mirror in the first place.)
		//
		// The SAME right-multiply the editor's own ops use, through the same helper:
		// MirrorFreeLocal_ is where the other half of this -- why `SetFinalTransformMatrix`
		// takes a mirror-free matrix and finalize re-appends M -- is written up.
		M = MirrorFreeLocal_( *obj );
		const int kind = mJob->CstObjectTransformKind( it->c_str() );
		Route r; r.name = *it; r.kind = kind; r.ready = false;
		if( kind == 1 ) {
			r.a = FormatMatrix16( M );
			r.ready = true;
		} else if( kind == 2 ) {
			Vector3 pos, orientDeg;
			if( DecomposeRigid( M, pos, orientDeg ) ) {
				r.a = FormatVec3( pos );
				r.b = FormatVec3( orientDeg );
				r.ready = true;
			}
		}
		work.push_back( r );
	}
	mPendingCstObjMatrix.clear();
	bool ok = true;
	for( size_t i = 0; i < work.size(); ++i ) {
		bool routed = false;
		if( work[i].ready ) {
			if( work[i].kind == 1 )      routed = ApplyCstObjectMatrix_( work[i].name, work[i].a );
			else if( work[i].kind == 2 ) routed = ApplyCstObjectComponents_( work[i].name, work[i].a, work[i].b );
		}
		if( !routed ) {
			ok = false;
			GlobalLog()->PrintEx( eLog_Error, "SceneEditor:: object transform commit to the CST failed for `%s` -- the live edit stands; EITHER the Document did not record it (a later full re-derive will REVERT the live transform) OR it was recorded but the re-derive diagnosed; see the preceding log lines", work[i].name.c_str() );
		}
	}
	return ok;
}

// P5 Slice 3 expansion (camera drag): note that the active camera's pose changed, for a deferred commit.
void SceneEditor::NoteCstCameraDrag_( const String& camName )
{
	mPendingCstCameraName = std::string( camName.c_str() );
}

// Commit the dragged camera's NET pose to the retained CST.  Called by the CONTROLLER at a render-thread-PARKED
// boundary (the commit re-derives).  Reads the REST pose params via CameraIntrospection (NOT the post-orbit
// composed position) so the route reconstructs the same pose -- see Job::ApplyCstCameraPoseEdit.
bool SceneEditor::CommitPendingCstCameraPose()
{
	if( mPendingCstCameraName.empty() ) return true;
	const std::string camName = mPendingCstCameraName;
	mPendingCstCameraName.clear();
	if( !mScene ) return false;
	const ICameraManager* cams = mScene->GetCameras();
	const ICamera* cam = cams ? cams->GetItem( camName.c_str() ) : 0;
	// A recorded manager name is an identity boundary: if it vanished, fail
	// closed instead of committing the pending pose onto the active camera.
	// The empty-name legacy case may still use the sole active camera.
	if( !cam && camName.empty() ) cam = mScene->GetCamera();
	if( !cam ) return false;
	const Implementation::CameraCommon* common =
		dynamic_cast<const Implementation::CameraCommon*>( cam );
	if( !common ) return false;
	// Use full-precision serialization for the net pose.  The properties
	// panel's display formatter is intentionally concise; feeding it into a
	// CST re-derive quantized a camera origin by several micro-units, so the
	// manager camera no longer exactly matched the just-rendered pane pose.
	const String loc(
		FormatPoint3( common->GetRestLocation() ).c_str() );
	const String lookat(
		FormatPoint3( common->GetStoredLookAt() ).c_str() );
	const String up(
		FormatVec3( common->GetStoredUp() ).c_str() );
	const Vector3 orientation = common->GetEulerOrientation();
	const String orient(
		FormatVec3( Vector3(
			orientation.x * RAD_TO_DEG,
			orientation.y * RAD_TO_DEG,
			orientation.z * RAD_TO_DEG ) ).c_str() );
	const Vector2 targetOrientation = common->GetTargetOrientation();
	const String target(
		FormatVec3( Vector3(
			targetOrientation.x * RAD_TO_DEG,
			targetOrientation.y * RAD_TO_DEG, 0 ) ).c_str() );
	String basisW;
	String basisV;
	const OrthonormalBasis3D basis = common->GetCurrentBasis();
	basisW = String( FormatVec3( basis.w() ).c_str() );
	basisV = String( FormatVec3( basis.v() ).c_str() );
	const int r = mJob->ApplyCstCameraPoseEditWithBasis(
		camName.c_str(), loc.c_str(), lookat.c_str(), up.c_str(),
		orient.c_str(), target.c_str(), basisW.c_str(), basisV.c_str() );
	if( r >= 2 ) RebindToJob_();
	if( r != 0 ) mCstLiveSceneChanged = true;   // code 1/2/3 all MUTATED the live scene -> the controller must re-render
	if( r == 0 )
		GlobalLog()->PrintEx( eLog_Error, "SceneEditor:: camera pose commit to the CST failed for `%s` -- the Document did NOT record the live pose (a later full re-derive will REVERT it)", camName.c_str() );
	else if( r == 3 )
		GlobalLog()->PrintEx( eLog_Error, "SceneEditor:: camera pose commit for `%s` recorded in the Document but the re-derive DIAGNOSED (see log) -- not a clean success", camName.c_str() );
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
			// LOCAL (87): RestoreObjectTransform's fallback pushes this straight
			// back onto the LOCAL transform stack, and ScaleObjectFromAnchor
			// multiplies it by a stretch and hands the product to
			// ReplaceFinalTransform_, which is also LOCAL.  Capturing the WORLD
			// matrix here would bake the parent's transform into the child on
			// the first undo.  For a root the two are identical.
			edit.prevTransform = obj->GetLocalTransformMatrix();
			if( const Implementation::Transformable* t = dynamic_cast<const Implementation::Transformable*>( obj ) ) {
				edit.prevTransformState = t->CaptureTransformStateV2();
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
		case SceneEdit::SetObjectMirror: {
			// doc 89 slice C.  The prior value is read straight off the node -- an
			// AXIS, not a name -- so unlike the binding ops there is no manager
			// reverse-lookup that can come back empty, and no "the referent was
			// deleted" undo failure to guard against.  A node that is not a
			// Transformable cannot carry a mirror at all, and Job::SetObjectMirror
			// would refuse the forward edit for that same reason, so capture "none"
			// and let the apply produce the diagnostic.
			const Implementation::Transformable* tf =
				dynamic_cast<const Implementation::Transformable*>( obj );
			const int axis = tf ? tf->GetMirrorAxis() : -1;
			edit.prevPropertyValue = String(
				axis == 0 ? "x" : axis == 1 ? "y" : axis == 2 ? "z" : "none" );
			break;
		}
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
		// A controller may explicitly route a gesture to an INACTIVE manager
		// camera (SceneCameraNamed).  Preserve that identity; only legacy
		// callers with no target supplied inherit the active camera.
		ICamera* baseCam = 0;
		if( edit.cameraTargetName.size() > 1 ) {
			ICameraManager* cameras = mScene->GetCamerasMutable();
			baseCam = cameras
			        ? cameras->GetItem( edit.cameraTargetName.c_str() )
			        : 0;
		} else {
			baseCam = mScene->GetCameraMutable();
			edit.cameraTargetName = mScene->GetActiveCameraName();
		}
		if( !baseCam ) return false;
		Implementation::CameraCommon* cam =
			dynamic_cast<Implementation::CameraCommon*>( baseCam );
		if( !cam ) return true;   // skeleton camera: nothing to capture; forward arm no-ops
		edit.prevCameraPos          = cam->GetRestLocation();
		edit.prevCameraLookAt       = cam->GetStoredLookAt();
		edit.prevCameraUp           = cam->GetStoredUp();
		edit.prevCameraTargetOrient = cam->GetTargetOrientation();
		edit.prevCameraOrient       = cam->GetEulerOrientation();
		edit.prevCameraWasONB       = cam->IsFromONB();
		if( edit.prevCameraWasONB ) {
			const OrthonormalBasis3D basis = cam->GetCurrentBasis();
			edit.prevCameraONBU = basis.u();
			edit.prevCameraONBV = basis.v();
			edit.prevCameraONBW = basis.w();
		}
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		edit.prevTime = mLastSetTime.load( std::memory_order_acquire );
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
		// Light-colour CST composite (round 2): a `color` edit on a chunk that spells some OTHER colour
		// space converts it to the linear convention as part of the SAME atomic edit -- so the history
		// entry must also carry the chunk's ORIGINAL `color` and `colorspace` TEXT, or Undo could restore
		// the value but not the convention it was written under.  `prevPropertyValue` above cannot serve:
		// it is the live RISEPel `%g`-formatted, i.e. the DECODED colour, which written back under a
		// restored `colorspace sRGB` would decode a second time.  FALSE for every other case (already
		// linear, no chunk, legacy scene) -- those keep the single-param route untouched.
		edit.lightCstColorSpaceComposite =
			( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) )
			&& LightColorCompositeState_( edit.objectName.c_str(), edit.propertyName,
			                              edit.prevCstColorText, edit.prevCstColorSpaceText );
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

// Shared-undo U1: see the header doc.  This is the ONE place a SceneEdit is pushed onto mHistory WITHOUT going
// through Apply's CaptureForApply + ApplyForwardMutation -- the mutation already landed (the caller applied it
// via Job::ApplyCstParamEditChecked before calling this), so there is nothing left to capture or mutate here.
void SceneEditor::PushAgentCstParamEdit(
	const String& entityName, const String& entityKind, const String& param,
	const String& newValue, const String& prevValue, bool prevValueWasAbsent,
	int occ, bool occAddressed )
{
	SceneEdit edit;
	edit.op                 = SceneEdit::SetAgentCstParam;
	edit.objectName         = entityName;
	edit.cstEntityKind      = entityKind;
	edit.propertyName       = param;
	edit.propertyValue      = newValue;
	edit.prevPropertyValue  = prevValue;
	edit.prevValueWasAbsent = prevValueWasAbsent;
	// doc 88 S4b: which occurrence the forward mutation wrote, and whether it was
	// addressed BY occurrence (which arms the undo/redo drift guard).  Both default
	// to the pre-S4b meaning for every caller that does not pass them.
	edit.cstParamOcc          = occ;
	edit.cstParamOccAddressed = occAddressed;
	// capturedTargetSerial stays 0 (default): SetAgentCstParam is a CST-routed op (IsCstRoutedOp), so
	// ApplyForwardMutation/ApplyRevertMutation's identity-serial guard is skipped for it regardless -- it
	// applies/reverts/redoes BY NAME, like every other CST-routed property op.
	mHistory.Push( edit );
}

// Shared-undo U2: see the header doc.  Same shape as PushAgentCstParamEdit -- the forward mutation (Job::
// ApplyCstInsertChunk / ApplyCstRemoveChunk) already landed via SceneEditController::ApplyAgentChunkCrud_
// before this call; there is nothing left to capture or mutate here.
void SceneEditor::PushAgentChunkCrudEdit(
	bool isInsert, const String& chunkName, const String& chunkKind, const String& chunkBytes,
	int docIndex, bool wasRasterizer )
{
	SceneEdit edit;
	edit.op                      = isInsert ? SceneEdit::AgentInsertChunk : SceneEdit::AgentRemoveChunk;
	edit.objectName               = chunkName;
	edit.cstEntityKind            = chunkKind;
	edit.propertyValue            = chunkBytes;
	edit.agentChunkIndex          = docIndex;
	edit.agentChunkWasRasterizer  = wasRasterizer;
	// capturedTargetSerial stays 0 (default): both ops are CST-routed (IsCstRoutedOp, extended below) -- they
	// apply/revert/redo BY NAME (or by captured position for a remove's Undo), never a stale pointer.
	mHistory.Push( edit );
}

// R1a (2026-08-09, batched remove_chunks): see the header doc.  Same shape as PushAgentChunkCrudEdit -- the
// forward mutation (Job::ApplyCstRemoveChunks) already landed via SceneEditController::ApplyAgentRemoveChunks
// before this call, so this only records what a later Undo/Redo needs.  ONE record per batch is the whole
// point: a single Cmd-Z restores every chunk the batch removed.
void SceneEditor::PushAgentRemoveChunksEdit(
	const String& displayTargets, const String& priorDocText, const String& redoTargetLines,
	bool anyWasRasterizer )
{
	SceneEdit edit;
	edit.op                      = SceneEdit::AgentRemoveChunks;
	edit.objectName              = displayTargets;    // DISPLAY ONLY -- see the op doc
	edit.propertyValue           = priorDocText;      // Undo payload (byte-exact pre-batch document)
	edit.prevPropertyValue       = redoTargetLines;   // Redo descriptor (`kind\tname` per line)
	edit.agentChunkWasRasterizer = anyWasRasterizer;
	// cstEntityKind deliberately left EMPTY and agentChunkIndex at its default -- see the op doc.
	// capturedTargetSerial stays 0 (default): CST-routed, same as its singular sibling.
	mHistory.Push( edit );
}

// R2 (2026-08-10, replace_geometry_scaffold): see the header doc.  Same shape as PushAgentRemoveChunksEdit --
// the forward mutation (Job::ApplyCstReplaceDocumentText) already landed via
// SceneEditController::ApplyAgentReplaceGeometry before this call, so this only records what a later
// Undo/Redo needs.  ONE record for the whole composite is the point of the verb.
void SceneEditor::PushAgentReplaceGeometryEdit(
	const String& objectName, const String& priorDocText, const String& postDocText )
{
	SceneEdit edit;
	edit.op                      = SceneEdit::AgentReplaceGeometry;
	edit.objectName              = objectName;        // a REAL addressable entity -- see the op doc
	edit.cstEntityKind           = String( "standard_object" );
	edit.propertyValue           = priorDocText;      // Undo payload (byte-exact pre-composite document)
	edit.prevPropertyValue       = postDocText;       // Redo payload (byte-exact post-composite document)
	edit.agentChunkWasRasterizer = false;             // invariant for this verb -- see the op doc
	// agentChunkIndex stays at its default (a whole-document swap has no single index).
	// capturedTargetSerial stays 0 (default): CST-routed, same as every other agent op.
	mHistory.Push( edit );
}

// doc-88 Phase 3 S20 (docs/gui/NODE_GRAPH_CANVAS.md sect. 6): see the header doc.  Same shape and same
// rationale as PushAgentReplaceGeometryEdit above -- the forward mutation (Job::ApplyCstReplaceDocumentText)
// already landed via SceneEditController::DuplicateGraphNode before this call, so this only records what a
// later Undo/Redo needs.  ONE record, so one Cmd-Z un-forks the copy.
void SceneEditor::PushAgentDuplicateNodeEdit(
	const String& copyName, const String& copyKeyword,
	const String& priorDocText, const String& postDocText )
{
	SceneEdit edit;
	edit.op                      = SceneEdit::AgentDuplicateNode;
	edit.objectName              = copyName;          // a REAL addressable entity -- see the op doc
	edit.cstEntityKind           = copyKeyword;
	edit.propertyValue           = priorDocText;      // Undo payload (byte-exact pre-duplicate document)
	edit.prevPropertyValue       = postDocText;       // Redo payload (byte-exact post-duplicate document)
	edit.agentChunkWasRasterizer = false;             // invariant for this verb -- see the op doc
	// agentChunkIndex stays at its default (a whole-document swap has no single index).
	// capturedTargetSerial stays 0 (default): CST-routed, same as every other agent op.
	mHistory.Push( edit );
}

bool SceneEditor::Apply( const SceneEdit& editIn )
{
	DirtyChangeNotifier _notifier( this );
	SceneEdit edit = editIn;

	// Model-B (code-3 re-render fix): clear the "live scene changed by a CST re-derive" signal for THIS Apply.
	// The route helpers below (RouteCstParamEdit_ / ApplyCstObjectMatrix_ / ApplyCstObjectComponents_ / the camera
	// pose commit) OR-in true when Job::DeriveEditedCstDocument_ returns a mutating code (1/2/3 -- including a
	// DIAGNOSED code-3 that replaced the Scene but reports failure).  The controller reads it after Apply to kick
	// the render even when Apply's success bool is false, so a diagnosed edit repaints the replaced scene rather
	// than leaving stale pixels.  Reset ONLY here (route helpers never reset), so it reflects "any mutating derive
	// happened during this whole Apply", surviving composite walks that route multiple params.
	mCstLiveSceneChanged = false;

	// Composite markers: push, adjust depth, no mutation.  (MarkEditEntityDirty's
	// switch has no case for either marker -- it is a no-op on them either way --
	// so skipping the call here changes nothing observable for this arm.)
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
	if( !ApplyForwardMutation( edit, /*isReplay*/false ) )  return false;   // the ONE creation call site

	// S5 fix (sibling of Undo()/Redo()'s P3-e): route property-shaped edits
	// into the per-category dirty channel only AFTER the forward mutation has
	// actually landed, not up front.  CaptureForApply and ApplyForwardMutation
	// can both refuse (SetMaterialProperty's slot-typing/kind-mismatch arms at
	// ~2753/2761/2769 among them -- all GUI-reachable from the property panel)
	// leaving the Document byte-identical; marking dirty before that point
	// flipped HasUnsavedChanges() on a refusal that changed nothing, the exact
	// data-loss-prompt gap P3-e already closed for Undo/Redo.  One consequence
	// worth naming: SetMaterialProperty's arm reads back through
	// mMaterialManager->GetItem(edit.objectName) inside MarkEditEntityDirty
	// (-> BumpSceneLightGenerationIfMaterialEmits), so that read now happens
	// AFTER the property mutation instead of before.  This is invariant for
	// the light-gen bump either way: GetEmitter()'s emissive-or-not is
	// TYPE-determined (which IMaterial subclass got constructed), and a
	// property-slot edit on an EXISTING material instance can never change
	// its C++ type -- only SetObjectMaterial (a different edit op entirely,
	// with its own case above) swaps which material an object points at.  So
	// "emissive before" and "emissive after" always agree for this op, and
	// reading post-mutation costs nothing beyond making the mark itself
	// contingent on success, which is the whole point of the fix.
	MarkEditEntityDirty( edit );
	mHistory.Push( edit );
	return true;
}

bool SceneEditor::Undo()
{
	DirtyChangeNotifier _notifier( this );
	SceneEdit edit;
	if( !mHistory.PopForUndo( edit ) ) return false;

	// Phase B: re-mark the (single-edit) entity dirty — undo after a save
	// must put the touched entity back into the dirty set.  Composite inner
	// edits are marked inside the walk-back loop.
	//
	// P3-e fix (round 1): the mark is made AFTER ApplyRevertMutation actually
	// lands, not before (both here and in the composite inner loop below,
	// and symmetrically in Redo()).  S4b's occurrence drift guard made a
	// REFUSED revert a designed, non-exceptional outcome (a legitimately
	// stale occurrence index, not a bug) rather than the rare corrupted-
	// history case this path previously assumed -- marking dirty before the
	// attempt flipped HasUnsavedChanges() on a refusal that changed nothing,
	// which meant closing the app after a refused Undo prompted "unsaved
	// changes" for a Document that was, in fact, byte-identical to what was
	// last saved.  `edit` itself (the CompositeEnd/CompositeBegin trigger
	// record popped just above, when this IS a composite) carries no case in
	// MarkEditEntityDirty's switch either way, so deferring its mark changes
	// nothing about that arm.
	//
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
			if( !ApplyRevertMutation( inner ) ) { failed = true; break; }   // P1: stop + roll back atomically
			MarkEditEntityDirty( inner );   // P3-e: only after the revert actually landed
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
			// isReplay: re-applying what we just reverted is a replay, not a new
			// edit.  The return value is deliberately ignored here (this
			// rollback is best-effort by design), so a gate firing would not
			// wedge -- it would silently fail to restore an inner.  Lossy
			// rather than unrecoverable, but wrong either way.  NOT covered by
			// a test: reaching it needs an inner revert to fail AFTER a later
			// one succeeded, which needs a captured dependency deliberately
			// destroyed out of history.
			//
			// P3 (S5): every `inner` in `reverted` already had MarkEditEntityDirty
			// called on it at line ~3034, back when its individual revert landed --
			// this rollback re-applies the MUTATION but does not (and cannot cheaply)
			// UN-mark the per-entity dirty channel those calls set.  So a composite
			// undo that fails partway leaves the touched entities' dirty bits set even
			// though the net document change, once the rollback re-applies finish, is
			// zero.  Accepted over-marking, not the under-marking P3-e closed: the
			// failure path is already the rare, best-effort, untested-by-design branch
			// documented above, and an entity spuriously flagged dirty costs a redundant
			// republish -- never a lost edit or a wrong "no unsaved changes" prompt,
			// which is the failure mode P3-e/S5 exist to prevent.  Symmetric in Redo()'s
			// composite failure arm below.
			for( std::vector<SceneEdit>::reverse_iterator it = reverted.rbegin(); it != reverted.rend(); ++it ) {
				ApplyForwardMutation( *it, /*isReplay*/true );
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
	// (e.g. a captured prior dependency vanished, or the S4b occurrence drift guard
	// refused), restore it to the undo stack -- a failed undo must NOT advance the
	// depth, make the un-reverted edit redo-able, or (P3-e) mark anything dirty: the
	// Document did not change, so HasUnsavedChanges() must not either.
	if( !ApplyRevertMutation( edit ) ) {
		mHistory.RestoreLastUndoFromRedo();
		return false;
	}
	MarkEditEntityDirty( edit );   // P3-e: only after the revert actually landed
	return true;
}

// 87 -- A CREATION RULE MUST NOT BE APPLIED TO A HISTORY REPLAY.
//
// The container rule ("a node with no geometry takes no surface binding") is
// enforced on every FORWARD path: the derive, the IJob setters, this editor's
// forward mutation, the agent param commit, the agent chunk insert.  It is
// deliberately NOT enforced here, nor on Redo -- Redo shares
// ApplyForwardMutation, which is why that function takes an `isReplay` flag
// that suppresses exactly these gates and nothing else.  (An earlier draft of
// this comment claimed the Redo path was ungated when it was not; the flag is
// what makes the claim true rather than aspirational.)  Two review rounds put a
// gate on THIS function; both were wrong, and the second was wrong in a way
// that cost the user their scene:
//
//   1. `set_param(O, geometry, none)`  -- O becomes a container; the derive
//      warns and drops its material, but the DOCUMENT still carries it.
//   2. `set_param(O, material, none)`  -- permitted, deliberately: a container
//      that acquired a stale binding must stay tidyable.
//   3. Cmd-Z.  The gated revert refused, so PopForUndo's record was pushed
//      BACK onto the undo stack -- and every later Cmd-Z re-popped it and
//      re-failed.  Step 1 and everything OLDER than it became permanently
//      unreachable.  The user could not get back to the scene they authored.
//
// A refused revert wedges the undo stack; there is no escape, because unlike
// the redo direction no new edit clears it.  And the thing being refused is
// not the creation of a novel state -- it is the RESTORATION of a document
// state that existed moments earlier, one the derive already knows how to
// tolerate (warn, drop the binding, carry on) because that is the contract for
// every scene file ever authored.  Trading a cosmetic load-time warning for an
// unrecoverable history is the wrong trade.
//
// The alternative considered and rejected: skip the write and let the undo
// consume its record instead of restoring it.  (Note this is NOT what the
// `restored = false` arms below do -- those return false, so Undo() restores
// the record, i.e. they are the same wedge shape.  They are legacy-path-only:
// on a retained-CST scene every binding op returns above the switch.)  That avoids the wedge but makes undo
// LOSSY -- undoing further to restore the `geometry` would then leave the
// object a leaf with no material, when the authored scene had one.  Undo must
// be lossless.
//
// So: the forward gates stop the agent CREATING this state.  History replay is
// exempt.  Do not add a gate here.
bool SceneEditor::ApplyRevertMutation( const SceneEdit& edit )
{

	// P1: identity guard -- refuse if the captured target was removed and a DIFFERENT
	// instance re-registered under the same name (serial mismatch); applying the
	// captured state to the replacement would corrupt it.  capturedTargetSerial==0
	// means the op tracks no identity (medium/time/marker/legacy) -> no check.
	// SKIP on the CST edit-model for the CST-routed ops (IsCstRoutedOp -- now material/light/camera-property +
	// medium + every IsObjectOp + every IsCameraOp drag): that path RE-DERIVES the entity (object transforms
	// mutate the live object then RECOMMIT as the `matrix` param at the boundary; camera drags recommit the pose
	// params), so its serial legitimately changes each edit, and it applies/reverts/redoes BY NAME (never the
	// stale pointer) -- the serial guard is both moot and would FALSELY trip.  (Medium ops carry serial==0
	// anyway, so the guard never reaches them regardless.)  The guard still applies ONLY on a LEGACY (no-Document)
	// scene, where these ops DO mutate the captured instance in place.
	if( edit.capturedTargetSerial != 0 &&
	    !( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) &&
	    ResolveTargetSerial( edit ) != edit.capturedTargetSerial )
		return false;
	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;

		// 87 -- DELIBERATELY NO CONTAINER GATE HERE.  See the block comment
		// above ApplyRevertMutation for why a creation rule must not be
		// applied to a history replay.

		// P5 Slice 3 expansion (object): CST-route the INVERSE per-op object edit too (replays the PREV value
		// through the same CST path), so undo stays Document-consistent.  A cleared prior binding routes "none"
		// (the standard_object unbind sentinel the load-time parser honours: material/shader/interior "none" == 0).
		if( mJob && mJob->HasRetainedCstDocument() ) {
			if( IsObjectCstParamOp( edit.op ) ) {
				String val;
				switch( edit.op ) {
				case SceneEdit::SetObjectMaterial:
				case SceneEdit::SetObjectShader:
					val = edit.prevBindingWasNull ? String( "none" ) : edit.prevPropertyValue;
					break;
				case SceneEdit::SetObjectInteriorMedium:
				case SceneEdit::SetObjectMirror:
					// doc 89 slice C: an empty prev is "no mirror", and `mirror none` is
					// the chunk's own spelling for it (PartE of ObjectMirrorTest pins that
					// it derives clean) -- so the undo of a first mirror WRITES a param
					// rather than removing one, which is the same shape the interior-medium
					// unbind uses and needs no RouteCstParamRemove_.
					val = ( edit.prevPropertyValue.size() <= 1 ) ? String( "none" ) : edit.prevPropertyValue;
					break;
				default:   // SetObjectGeometry -- prev is always a real registered name (CaptureForApply rejected otherwise)
					val = edit.prevPropertyValue;
					break;
				}
				if( !RouteCstParamEdit_( edit.objectName.c_str(), "standard_object", ObjectCstParamRole( edit.op ), val.c_str() ) ) return false;
				mLastScope = Dirty_ObjectTransform;
				return true;
			}
			if( edit.op == SceneEdit::SetObjectShadowFlags ) {
				if( !RouteObjectShadowFlagsToCst_( edit.objectName, static_cast<int>( edit.prevShadowFlags ) ) ) return false;
				mLastScope = Dirty_ObjectTransform;
				return true;
			}
		}

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
				if( g ) {
					// 87: same visibility flip the FORWARD arm does, for the same
					// reason and with the same caveat -- the container case is
					// unreachable from here (CaptureForApply refuses a
					// SetObjectGeometry whose prior geometry has no name), but
					// the forward arm does it anyway rather than lean on a guard
					// three frames away, and this is its sibling site.  Reached,
					// the omission would leave a HIDDEN object WITH geometry:
					// exactly the fingerprint ObjectManager::SetObjectParent
					// reads as "CSG operand", so the node would afterwards be
					// refused a parent with a diagnostic naming the wrong cause.
					const bool wasContainer = ( obj->GetGeometry() == 0 );
					obj->AssignGeometry( *g );
					if( wasContainer ) obj->SetWorldVisible( true );
				}
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
		case SceneEdit::SetObjectMirror:
			// doc 89 slice C.  An EXPLICIT arm, not the `default:` below: that one
			// restores the captured TRANSFORM state, which for a mirror edit is the
			// state the mirror edit never touched -- it would leave the reflection
			// standing and report success.  Replays the captured axis through the
			// same IJob entry point the forward edit used, so the finalize /
			// TLAS-invalidation / light-generation bump are identical in both
			// directions.  "none" is the clear, and it always resolves (the axis is
			// a literal, not a reference), so the only failure here is a missing Job.
			if( mJob ) {
				restored = mJob->SetObjectMirror( edit.objectName.c_str(),
					edit.prevPropertyValue.size() <= 1 ? "none" : edit.prevPropertyValue.c_str() );
			} else {
				restored = false;
			}
			RunObjectInvariantChain( *obj );   // rebake this node's subtree + the TLAS leaf
			mDirtyTracker.MarkDirty( std::string( edit.objectName.c_str() ) );
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
			// P5 Slice 3 expansion (object transform): the inverse transform changed the live object too -> note it
			// for the same deferred `matrix`-param commit, so undo stays Document-consistent.
			if( mJob && mJob->HasRetainedCstDocument() && IsObjectTransformOp( edit.op ) )
				NoteCstObjectTransform_( edit.objectName );
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
		// Legacy active SceneCamera navigation deliberately leaves an
		// explicit-ONB camera untouched.  Its inverse must be equally inert:
		// restoring the captured pose and noting a CST drag here would
		// canonicalize the authored ONB chunk merely by pressing Undo.
		if( cam->IsFromONB() && !edit.allowONBPoseEdit ) {
			mLastScope = Dirty_Camera;
			return true;
		}
		RestoreCameraTransform( *cam, edit );
		cam->RegenerateData();
		// P5 Slice 3 expansion (camera drag): the inverse changed the live camera pose too -> note it for the same
		// deferred pose commit, so undo stays Document-consistent.
		if( mJob && mJob->HasRetainedCstDocument() )
			NoteCstCameraDrag_( edit.cameraTargetName );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		// Restore the time captured before the edit.  Use the preview
		// path (no photon regen) so undo is fast.
		mScene->SetSceneTimeForPreview( edit.prevTime );
		mLastSetTime.store( edit.prevTime, std::memory_order_release );
		mLastScope = mScenePhotonsExist ? Dirty_TimeAndPhotons : Dirty_Time;
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = ResolveEditedCamera( edit );
		if( !baseCam ) return false;
		// P5 Slice 3 expansion: CST-route the inverse camera edit too.
		if( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) {
			if( !RouteCstParamEdit_( edit.cameraTargetName.c_str(), "camera", edit.objectName.c_str(), edit.prevPropertyValue.c_str() ) ) return false;
			mLastScope = Dirty_Camera;
			return true;
		}
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
		// Model-B P5 (camera-clone CST insert -- undo): on a CST scene, REMOVE the cloned camera's chunk from the
		// retained Document FIRST (the inverse of the forward insert), so the undo stays Document-consistent -- a
		// subsequent D2 must NOT resurrect the undone clone.  Document-only (no re-derive); redo re-inserts via the
		// forward branch.  No-op on a legacy scene.
		if( mJob->HasRetainedCstDocument() ) {
			if( mJob->ApplyCstRemoveCameraChunk( edit.objectName.c_str() ) != 1 ) {
				GlobalLog()->PrintEx( eLog_Error,
					"SceneEditor: undo of camera clone `%s` could not remove its CST chunk (live camera removed, but the Document still holds it -- a re-derive would bring it back)",
					edit.objectName.c_str() );
			}
		}
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
			// Light-colour CST composite (round 2), revert half.  When the forward half converted the
			// chunk, put BOTH original texts back VERBATIM in one atomic edit: the chunk becomes
			// byte-identical to what it was (the `colorspace sRGB` spelling included) and the light returns
			// to its original DECODED colour.  Note what is NOT written here: `prevPropertyValue`, which
			// was captured off ILight::emissionColor() and is therefore the light's decoded LINEAR value --
			// writing that back under a restored `colorspace sRGB` would decode it a second time and undo
			// would land somewhere the light has never been.
			//
			// Restoring the SPELLING is what keeps shared undo (agent + user) sound: an agent history entry
			// captures RAW CHUNK TEXT, so an older agent entry undone AFTER this one must find the chunk in
			// the convention its digits were captured under.
			if( edit.lightCstColorSpaceComposite ) {
				if( !RouteCstLightColorComposite_( edit.objectName.c_str(), edit.prevCstColorText.c_str(),
				                                   edit.prevCstColorSpaceText.c_str() ) ) return false;
				mLastScope = Dirty_Camera;
				return true;
			}
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
		// P5 Slice 3 expansion (medium): CST-route the inverse absorption/scattering edit too.
		if( mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) && IsCstRoutableMediumProp( edit.propertyName ) ) {
			if( !RouteCstParamEdit_( edit.objectName.c_str(), "medium", edit.propertyName.c_str(), edit.prevPropertyValue.c_str() ) ) return false;
			mLastScope = Dirty_Camera;
			return true;
		}
		ApplyMediumPropertyValue( *medium, edit.propertyName, edit.prevPropertyValue );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetAgentCstParam )
	{
		// Shared-undo U1 (Undo direction): the whole point of this op -- see SceneEdit.h's doc and
		// PushAgentCstParamEdit.  Two shapes: the param was ABSENT before the agent edit (it got INSERTED by
		// DocSetOrAddParamValue) -> the inverse is a REMOVE, not a re-SET of a value that never existed; else
		// re-apply the captured prior value.  BOTH arms use the FULL-DERIVABILITY-GATED route (RouteCstParamRemove_
		// / RouteCstParamEditChecked_), NOT the ungated RouteCstParamEdit_ every other CST-routed property revert
		// uses above -- see RouteCstParamEditChecked_'s doc for why an agent-originated edit cannot safely share
		// the GUI's ungated fast path here (Model-B F5's agent chunk-CRUD verbs can mutate the Document between
		// this edit's original Apply and this Undo without leaving an mHistory record to invalidate).
		if( !mJob ) return false;
		const char* kind = edit.cstEntityKind.size() > 1 ? edit.cstEntityKind.c_str() : nullptr;
		bool diagnosed = false;
		// doc 88 S4b (drift guard): for an OCCURRENCE-ADDRESSED edit, verify that occurrence
		// `edit.cstParamOcc` still exists AND still holds the value this edit wrote, BEFORE routing a
		// write at that index.  A repeatable param's layout can move between the edit and this Undo with
		// nothing invalidating the history entry (agent chunk CRUD leaves no record; another occurrence
		// edit does not touch this one), and writing occurrence N blindly would then clobber a neighbour.
		// Refuse honestly instead: returning false makes Undo() restore this entry to the undo stack, so
		// the history keeps the edit and the user can retry once the drift is resolved.  A NON-occurrence-
		// addressed edit (every pre-S4b caller) short-circuits to true inside the guard.
		if( !OccurrenceEditStillAddressable_( edit, edit.propertyValue.c_str(), "Undo" ) ) return false;
		// `occ` -- pre-S4b this was hardcoded 0 (the agent path's fixed convention); the field carries the
		// same 0 for every one of those edits, and the real occurrence for a `<role>[<index>]` row edit.
		if( edit.prevValueWasAbsent ) {
			if( !RouteCstParamRemove_( edit.objectName.c_str(), kind, edit.propertyName.c_str(), edit.cstParamOcc, &diagnosed ) ) return false;
		} else {
			if( !RouteCstParamEditChecked_( edit.objectName.c_str(), kind, edit.propertyName.c_str(), edit.prevPropertyValue.c_str(), edit.cstParamOcc, &diagnosed ) ) return false;
		}
		if( diagnosed )
			GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Undo:: agent edit on `%s`.`%s` reverted via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean revert",
			                       edit.objectName.c_str(), edit.propertyName.c_str() );
		// Shared-undo follow-up (P2 fix): mirror the forward commit's light-gen
		// bump (SceneEditController::ApplyAgentParamEdit) on the Undo arm too --
		// same stale-alias-table hazard applies to a REVERTED emissive-material
		// edit.  See BumpSceneLightGenerationForAgentParamEdit's doc.
		BumpSceneLightGenerationForAgentParamEdit(
			edit.objectName.c_str(), kind );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AgentRemoveChunks )
	{
		// R1a (2026-08-09, Undo direction): ONE record, whole batch -- restore the byte-exact pre-batch
		// document text.  See the AgentRemoveChunks op doc + RouteAgentRemoveChunksBatch_.
		if( !mJob ) return false;
		bool diagnosed = false;
		if( !RouteAgentRemoveChunksBatch_( edit, /*forward*/ false, &diagnosed ) ) return false;
		if( diagnosed )
			GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Undo:: agent batch chunk remove of `%s` reverted via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean revert",
			                       edit.objectName.c_str() );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AgentReplaceGeometry || edit.op == SceneEdit::AgentDuplicateNode )
	{
		// R2 (2026-08-10, Undo direction): ONE record, whole composite -- restore the byte-exact pre-call
		// document text.  See the AgentReplaceGeometry op doc + RouteAgentDocumentSwap_.
		// doc-88 S20: AgentDuplicateNode rides the SAME arm -- identical payload shape, identical inverse.
		if( !mJob ) return false;
		bool diagnosed = false;
		if( !RouteAgentDocumentSwap_( edit, /*forward*/ false, &diagnosed ) ) return false;
		if( diagnosed )
			GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Undo:: agent %s on `%s` reverted via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean revert",
			                       ( edit.op == SceneEdit::AgentDuplicateNode ) ? "graph-node duplicate" : "geometry replacement",
			                       edit.objectName.c_str() );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AgentInsertChunk || edit.op == SceneEdit::AgentRemoveChunk )
	{
		// Shared-undo U2 (Undo direction): the inverse-patch doctrine -- Undo of an insert REMOVES the chunk
		// (occurrence-aware, by name+kind); Undo of a remove RESTORES the captured exact bytes at the captured
		// exact index.  See SceneEdit.h's op docs + RouteAgentChunkCrud_ for the full per-shape rationale.
		if( !mJob ) return false;
		const bool forInsertOp = ( edit.op == SceneEdit::AgentInsertChunk );
		bool diagnosed = false;
		if( !RouteAgentChunkCrud_( edit, forInsertOp, /*forward*/ false, &diagnosed ) ) return false;
		if( diagnosed )
			GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Undo:: agent chunk %s on `%s` reverted via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean revert",
			                       forInsertOp ? "insert" : "remove", edit.objectName.c_str() );
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

bool SceneEditor::ApplyForwardMutation( const SceneEdit& edit, bool isReplay )
{

	// P1: identity guard -- refuse if the captured target was removed and a DIFFERENT
	// instance re-registered under the same name (serial mismatch); applying the
	// captured state to the replacement would corrupt it.  capturedTargetSerial==0
	// means the op tracks no identity (medium/time/marker/legacy) -> no check.
	// SKIP on the CST edit-model for the CST-routed ops (IsCstRoutedOp -- now material/light/camera-property +
	// medium + every IsObjectOp + every IsCameraOp drag): that path RE-DERIVES the entity (object transforms
	// mutate the live object then RECOMMIT as the `matrix` param at the boundary; camera drags recommit the pose
	// params), so its serial legitimately changes each edit, and it applies/reverts/redoes BY NAME (never the
	// stale pointer) -- the serial guard is both moot and would FALSELY trip.  (Medium ops carry serial==0
	// anyway, so the guard never reaches them regardless.)  The guard still applies ONLY on a LEGACY (no-Document)
	// scene, where these ops DO mutate the captured instance in place.
	if( edit.capturedTargetSerial != 0 &&
	    !( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) &&
	    ResolveTargetSerial( edit ) != edit.capturedTargetSerial )
		return false;
	if( SceneEdit::IsObjectOp( edit.op ) )
	{
		IObjectPriv* obj = FindObject( edit.objectName );
		if( !obj ) return false;
		int cstKind = -1;   // CST object-transform kind (set in the gate below): -1 non-CST, 0 none, 1 matrix, 2 components
		// P5 Slice 3 expansion (object): on a CST-loaded scene, route the PER-OP object edits (reference BINDINGS +
		// shadow flags) through the canonical CST so the Document stays complete (a later D2 re-derive can't lose
		// them).  The re-derive rebuilds the object from the edited chunk (bbox / TLAS / light-gen included), so we
		// route + return instead of the direct mutate.  TRANSFORM ops fall through to the direct mutate here and are
		// committed to the authoritative `matrix` param at the composite/edit boundary (Stage B).
		if( mJob && mJob->HasRetainedCstDocument() ) {
			// 87: refuse a surface binding on a CONTAINER *before* routing it.
			// The gate in ApplyObjectOpForward is downstream of this early
			// return, so on a CST scene the panel would otherwise write a
			// `material` line into the container's chunk, the derive would drop
			// it with a warning on this and every later derive, and the edit
			// would report SUCCESS -- leaving the Document permanently carrying
			// a param that can never take effect.
			// CLEARING stays allowed: an object that BECAME a container can be
			// tidied up, and refusing the clear would refuse the one edit that
			// removes the stale line the derive warns about on every load.
			//
			// On THIS route `"none"` is the unbind sentinel for material and
			// shader as well as for the medium -- it is what the revert path a
			// few hundred lines up writes, and what the load-time parser honours
			// (`material=="none" ? 0 : ...`).  That is route-specific: on the
			// direct-mutate arm below, `"none"` resolves through the manager to
			// the REGISTERED `none` material, which is a real bind and is
			// correctly refused there.
			const bool clearingBinding =
				( edit.propertyValue.size() <= 1 || edit.propertyValue == String( "none" ) );
			if( !isReplay
			 && IsObjectBindingOp( edit.op ) && edit.op != SceneEdit::SetObjectGeometry
			 && !clearingBinding
			 && IsContainerNodeForEdit_( *obj ) ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditor:: `%s` is a container node (no geometry), so it takes no surface binding; "
					"bind it to a child that has geometry", edit.objectName.c_str() );
				return false;
			}
			if( IsObjectCstParamOp( edit.op ) ) {
				String val = edit.propertyValue;
				if( ( edit.op == SceneEdit::SetObjectInteriorMedium || edit.op == SceneEdit::SetObjectMirror )
				 && ( val.size() <= 1 || val == String( "none" ) ) ) val = String( "none" );
				if( !RouteCstParamEdit_( edit.objectName.c_str(), "standard_object", ObjectCstParamRole( edit.op ), val.c_str() ) ) return false;
				mDirtyTracker.MarkDirty( std::string( edit.objectName.c_str() ) );
				mLastScope = Dirty_ObjectTransform;
				return true;
			}
			if( edit.op == SceneEdit::SetObjectShadowFlags ) {
				if( !RouteObjectShadowFlagsToCst_( edit.objectName, static_cast<int>( edit.s ) ) ) return false;
				mDirtyTracker.MarkDirty( std::string( edit.objectName.c_str() ) );
				mLastScope = Dirty_ObjectTransform;
				return true;
			}
			// A TRANSFORM edit must be COMMITTABLE to the CST, or it would mutate the live object then fail the
			// deferred commit -- leaving the live transform DIVERGED from the un-committable CST, which a later D2
			// would silently revert (data-loss).  standard_object -> `matrix` (any transform); csg_object ->
			// position/orientation (translate+rotate ONLY -- it has no scale param).  Refuse anything else HERE,
			// before the live mutate.  (Bindings above already routed -- csg has material/shader params.)
			if( IsObjectTransformOp( edit.op ) ) {
				cstKind = mJob->CstObjectTransformKind( edit.objectName.c_str() );
				const bool committable = ( cstKind == 1 ) || ( cstKind == 2 && IsObjectTranslateOrRotateOp( edit.op ) );
				if( !committable ) {
					if( mLastNonRoutableTransformObj != std::string( edit.objectName.c_str() ) ) {
						mLastNonRoutableTransformObj = std::string( edit.objectName.c_str() );
						// 87 step 3: a SYNTHESIZED entry has no chunk of its own name, so
						// "no CST `matrix` param and not a csg_object" is a non-sequitur --
						// literally true and completely unhelpful, because the author never
						// wrote a chunk for this entry at all.  Name the INSTANCING chunk,
						// which IS the thing they can edit.  Provenance is a map lookup; the
						// entry name is an opaque token and must never be picked apart.
						std::string why = ( cstKind == 2 )
							? std::string( "csg_object has no scale param -- only translate/rotate are committable" )
							: std::string( "object has no CST `matrix` param and is not a csg_object" );
						const IObjectManager* objsForProv = mScene ? mScene->GetObjects() : 0;
						const char* instancingChunk = 0;
						const char* sourceNode = 0;
						const bool hasProv = objsForProv
						 && objsForProv->GetObjectProvenance( edit.objectName.c_str(), &instancingChunk, &sourceNode );
						if( hasProv && instancingChunk && instancingChunk[0]
						 && std::string( instancingChunk ) != std::string( edit.objectName.c_str() ) ) {
							why = std::string( "it is an INSTANCE synthesized by `" ) + instancingChunk
							    + "` and has no chunk of its own -- move `" + instancingChunk + "` instead";
						} else if( cstKind == 2 && hasProv && sourceNode && sourceNode[0] ) {
							// 87 step 3a COLLAPSE case.  The chunk the author wrote is a
							// `standard_object`; only its `source` makes it commit like a
							// csg_object.  The generic kind-2 reason above names a chunk type
							// that appears NOWHERE in what they wrote and never mentions the
							// one line that produced the restriction -- so name the `source`
							// and where it lands.  Keyed on the provenance row's SOURCE field,
							// NOT on `instancingChunk != objectName`: in the collapse case the
							// row is `I -> (I, S)`, so that test is false by construction and
							// the branch above cannot fire here.  (Every other provenance row --
							// a subtree clone `I.X`, a repetition `I[i,j]` -- has
							// `instancingChunk != objectName` and so takes the branch above.)
							why = std::string( "its `source` names `" ) + sourceNode
							    + "`, which resolves to a csg_object -- that chunk type has no `scale` param, so "
							      "only translate/rotate are committable on this instance";
						}
						GlobalLog()->PrintEx( eLog_Warning, "SceneEditor:: object `%s` transform cannot be saved on a CST-loaded scene (%s); edit refused", edit.objectName.c_str(), why.c_str() );
					}
					return false;
				}
			}
		}
		const bool fwdOk = ApplyObjectOpForward( *obj, edit, isReplay );   // P1: false if a redo target vanished
		// POST-MUTATE: the op-level gate above admits a csg (kind 2) translate/rotate, but the RESULT can still be
		// non-decomposable -- a rotate can land on GIMBAL-LOCK (~90 deg about Y), and a translate on an object that
		// already carries a non-unit SCALE (from an `override_object`, say) leaves that scale in the local matrix.
		// The committable guarantee is therefore matrix-level, not op-level -- so VERIFY it here, after the mutate:
		// if the csg result is not decomposable, RESTORE the object (the edit carries the captured prev state) and
		// reject, so the live transform never diverges from the un-committable CST.  Makes the commit-time
		// DecomposeRigid in CommitPendingCstObjectTransforms a guaranteed success.
		if( fwdOk && cstKind == 2 ) {
			obj->FinalizeTransformations();   // ApplyObjectOpForward updates components/stack but not m_mxFinalTrans; compose it before reading
			// Check the matrix the COMMIT will actually decompose -- the LOCAL
			// one (see CommitPendingCstObjectTransforms) -- so the gate passes
			// or refuses on the same matrix that gets written.
			Vector3 dpos, dorient;
			// Report WHICH rejection(s) fired.  A single "gimbal-lock / non-decomposable" line for all six told an
			// author who ran a pure TRANSLATE, and never rotated anything, that their rotation hit a singularity --
			// while the actual blocker (commonly a non-unit `scale` on a same-named override_object) went unnamed.
			// DecomposeRigid names every defect it detected, since more than one can be true of one matrix.
			std::string decomposeWhy;
			if( !DecomposeRigid( obj->GetLocalTransformMatrix(), dpos, dorient, &decomposeWhy ) ) {
				RestoreObjectTransform( *obj, edit );
				RunObjectInvariantChain( *obj );
				if( mLastNonRoutableTransformObj != std::string( edit.objectName.c_str() ) ) {
					mLastNonRoutableTransformObj = std::string( edit.objectName.c_str() );
					GlobalLog()->PrintEx( eLog_Warning, "SceneEditor:: object `%s` transform is not committable to a csg_object (%s); edit refused", edit.objectName.c_str(), decomposeWhy.c_str() );
				}
				mLastScope = Dirty_ObjectTransform;
				return false;
			}
		}
		// Property-style ops don't move geometry — symmetric with
		// Apply()'s spatial-rebuild gate.  Pre-Phase-1 this path ran
		// the chain unconditionally, costing a spurious BSP
		// invalidation per material/shader/shadow redo.
		//
		// 87: gated on fwdOk too.  A transform op can now FAIL -- PushWorldOp_
		// refuses a world-space delta on a node whose parent chain is not
		// invertible -- and an op that mutated nothing must not throw away the
		// TLAS or mark the scene dirty.  Before hierarchy the two transform ops
		// that route through it could not fail at all.
		const bool needsSpatialRebuild = fwdOk && SceneEdit::OpNeedsSpatialRebuild( edit.op );
		if( needsSpatialRebuild ) {
			RunObjectInvariantChain( *obj );
			// Phase 6.3 (§7.3): single-op redo marks dirty.
			mDirtyTracker.MarkDirty( std::string( edit.objectName.c_str() ) );
			if( edit.op == SceneEdit::ScaleObjectFromAnchor ) {
				mScaleFromAnchorSet.insert( std::string( edit.objectName.c_str() ) );
			}
		}
		// P5 Slice 3 expansion (object transform): on a CST scene, NOTE this object for a deferred `matrix`-param
		// commit (the controller flushes it at a parked boundary -- per-op routing would be N re-derives per drag).
		if( fwdOk && mJob && mJob->HasRetainedCstDocument() && IsObjectTransformOp( edit.op ) )
			NoteCstObjectTransform_( edit.objectName );
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
		// T3 is deliberately scoped to the named-pane route.  Explicit-ONB
		// active SceneCamera gestures historically leave the fixed basis
		// unchanged; do not broaden that legacy behavior.
		if( cam->IsFromONB() && !edit.allowONBPoseEdit ) {
			mLastScope = Dirty_Camera;
			return true;
		}
		const bool keepExplicitONB = cam->IsFromONB();
		cam->BeginInteractivePoseEdit();
		ApplyCameraOpForward( *cam, edit, SceneScale() );
		cam->RegenerateData();
		if( keepExplicitONB ) {
			// The shared camera math operates in lookAt/up space.  Preserve
			// the manager camera's authored representation by immediately
			// folding the realized result back into its explicit frame; this
			// also keeps programmatic/no-CST ONB cameras explicit between
			// events.  The retained-CST commit serializes W/V afterward.
			const Point3 origin = cam->GetLocation();
			const OrthonormalBasis3D basis = cam->GetCurrentBasis();
			cam->RestoreInteractiveONBPose(
				origin, basis.u(), basis.v(), basis.w() );
			cam->RegenerateData();
		}
		// P5 Slice 3 expansion (camera drag): on a CST scene, note the
		// explicitly recorded camera for a deferred pose commit (the
		// controller flushes it at a parked boundary -- per-op routing would
		// be N re-derives per drag).
		if( mJob && mJob->HasRetainedCstDocument() )
			NoteCstCameraDrag_( edit.cameraTargetName );
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
		// Model-B P5 (camera-clone CST insert): on a CST scene, also INSERT a faithful camera chunk into the
		// retained Document so the clone SURVIVES a future D2 re-derive AND a save->reload (else it's a live-only
		// camera the Document doesn't know about, dropped the first time the scene re-derives from the CST).  This
		// is purely a Document-only edit (the live camera is already registered above) -- no re-derive, no rebind.
		// Serves forward AND redo (both route here).  Resolve the just-added live camera by name to introspect its
		// authorable params.  On a legacy (no-Document) scene this is a no-op (HasRetainedCstDocument false).
		if( mJob->HasRetainedCstDocument() ) {
			const ICameraManager* cams = mScene ? mScene->GetCameras() : 0;
			const ICamera* newCam = cams ? cams->GetItem( edit.objectName.c_str() ) : 0;
			if( newCam ) {
				const std::string chunk = CameraIntrospection::BuildCameraChunkText( *newCam, edit.objectName );
				if( chunk.empty() || mJob->ApplyCstInsertCameraChunk( chunk.c_str() ) != 1 ) {
					GlobalLog()->PrintEx( eLog_Error,
						"SceneEditor: camera clone `%s` could not be recorded in the CST Document (live clone stands, but a future re-derive / save would lose it)",
						edit.objectName.c_str() );
				}
			} else {
				GlobalLog()->PrintEx( eLog_Error,
					"SceneEditor: camera clone `%s` not resolvable post-add for CST insert (live clone stands, but the Document is out of sync)",
					edit.objectName.c_str() );
			}
		}
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
			// Light-colour CST composite (round 2): a `color` edit on a chunk spelling a NON-linear colour
			// space writes `colorspace Rec709RGB_Linear` AND the new digits as ONE atomic Document edit,
			// else the re-derive decodes the panel's already-linear digits again.  All-or-nothing: a refusal
			// leaves the chunk untouched, so the caller's `return false` is a clean failure.
			//
			// Asked of the DOCUMENT rather than read off `edit.lightCstColorSpaceComposite`, because this
			// function also serves REDO -- where the chunk's state is whatever the preceding Undo restored
			// (sRGB again), not what it was at the original capture.  The capture-time flag governs the
			// REVERT arm (which needs the captured texts); the forward arm needs today's truth.
			String unusedColor, unusedSpace;
			if( LightColorCompositeState_( edit.objectName.c_str(), edit.propertyName, unusedColor, unusedSpace ) ) {
				if( !RouteCstLightColorComposite_( edit.objectName.c_str(), edit.propertyValue.c_str(), "Rec709RGB_Linear" ) ) return false;
				mLastScope = Dirty_Camera;
				return true;
			}
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
		// P5 Slice 3 expansion (medium): route absorption/scattering through the CST so the Document stays complete
		// (a later D2 can't lose them); emission has no chunk param -> direct mutate (documented transient).
		if( mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) && IsCstRoutableMediumProp( edit.propertyName ) ) {
			if( !RouteCstParamEdit_( edit.objectName.c_str(), "medium", edit.propertyName.c_str(), edit.propertyValue.c_str() ) ) return false;
			mLastScope = Dirty_Camera;
			return true;
		}
		if( !ApplyMediumPropertyValue( *medium, edit.propertyName, edit.propertyValue ) ) return false;   // H2-S3: surface failure so Apply can reject
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetSceneTime )
	{
		mScene->SetSceneTimeForPreview( edit.s );
		mLastSetTime.store( edit.s, std::memory_order_release );
		mLastScope = mScenePhotonsExist ? Dirty_TimeAndPhotons : Dirty_Time;
		return true;
	}

	if( edit.op == SceneEdit::SetCameraProperty )
	{
		ICamera* baseCam = ResolveEditedCamera( edit );
		if( !baseCam ) return false;
		// P5 Slice 3 expansion: CST-route the camera edit (entity = the active camera by name, or the unique
		// camera chunk by position for an UNNAMED camera) so the Document stays complete.
		if( mJob && mJob->HasRetainedCstDocument() && IsCstRoutedOp( edit.op ) ) {
			if( !RouteCstParamEdit_( edit.cameraTargetName.c_str(), "camera", edit.objectName.c_str(), edit.propertyValue.c_str() ) ) return false;
			mLastScope = Dirty_Camera;
			return true;
		}
		if( !CameraIntrospection::SetProperty( *baseCam, edit.objectName, edit.propertyValue ) ) return false;   // H2-S3: surface parse failure so Apply can reject
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::SetAgentCstParam )
	{
		// Shared-undo U1 (Redo direction): re-apply the new value the same way
		// the ORIGINAL agent commit did -- DocSetOrAddParamValue inserts-or-sets,
		// so this is correct whether or not the param existed at the time of the
		// original edit.  The FIRST apply of a fresh agent edit never reaches
		// here (the mutation already landed via Job::ApplyCstParamEditChecked
		// before the history push -- see PushAgentCstParamEdit); this arm is
		// exercised by Redo only.  Uses the FULL-DERIVABILITY-GATED
		// RouteCstParamEditChecked_, NOT the ungated RouteCstParamEdit_ -- see
		// that helper's doc for why an agent-originated edit cannot safely
		// share the GUI property panel's ungated fast path here.
		if( !mJob ) return false;
		{
			bool diagnosed = false;
			// doc 88 S4b (drift guard, Redo direction): the state this Redo is entitled to overwrite is
			// what the Undo left behind -- the PRIOR value for an ordinary edit.  (An edit whose param was
			// ABSENT before it cannot be occurrence-addressed: an occ>0 line has no INSERT arm and an
			// occurrence row only exists for a line the document already spells, so `prevValueWasAbsent`
			// and `cstParamOccAddressed` are never both true; the guard is still correct if they ever were
			// -- an absent param reads as the empty string, which is what prevPropertyValue holds.)
			if( !OccurrenceEditStillAddressable_( edit, edit.prevPropertyValue.c_str(), "Redo" ) ) return false;
			if( !RouteCstParamEditChecked_( edit.objectName.c_str(),
			                                 edit.cstEntityKind.size() > 1 ? edit.cstEntityKind.c_str() : nullptr,
			                                 edit.propertyName.c_str(), edit.propertyValue.c_str(),
			                                 edit.cstParamOcc, &diagnosed ) ) return false;
			if( diagnosed )
				GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Redo:: agent edit on `%s`.`%s` re-applied via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean redo",
				                       edit.objectName.c_str(), edit.propertyName.c_str() );
		}
		// Shared-undo follow-up (P2 fix): mirror the forward commit's light-gen
		// bump on the Redo arm too -- same stale-alias-table hazard applies to
		// a RE-APPLIED emissive-material edit.  See
		// BumpSceneLightGenerationForAgentParamEdit's doc.
		BumpSceneLightGenerationForAgentParamEdit(
			edit.objectName.c_str(),
			edit.cstEntityKind.size() > 1 ? edit.cstEntityKind.c_str() : nullptr );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AgentRemoveChunks )
	{
		// R1a (2026-08-09, Redo direction): re-run the whole batch by (kind, name).  The FIRST apply of a
		// fresh batch never reaches here (the mutation already landed via Job::ApplyCstRemoveChunks before
		// the history push -- see PushAgentRemoveChunksEdit); this arm is exercised by Redo only.
		if( !mJob ) return false;
		bool diagnosed = false;
		if( !RouteAgentRemoveChunksBatch_( edit, /*forward*/ true, &diagnosed ) ) return false;
		if( diagnosed )
			GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Redo:: agent batch chunk remove of `%s` re-applied via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean redo",
			                       edit.objectName.c_str() );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AgentReplaceGeometry || edit.op == SceneEdit::AgentDuplicateNode )
	{
		// R2 (2026-08-10, Redo direction): re-install the byte-exact POST-composite document text.  The FIRST
		// apply of a fresh composite never reaches here (the mutation already landed via
		// Job::ApplyCstReplaceDocumentText before the history push -- see PushAgentReplaceGeometryEdit); this
		// arm is exercised by Redo only.
		// doc-88 S20: AgentDuplicateNode rides the SAME arm -- see the Undo twin above.
		if( !mJob ) return false;
		bool diagnosed = false;
		if( !RouteAgentDocumentSwap_( edit, /*forward*/ true, &diagnosed ) ) return false;
		if( diagnosed )
			GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Redo:: agent %s on `%s` re-applied via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean redo",
			                       ( edit.op == SceneEdit::AgentDuplicateNode ) ? "graph-node duplicate" : "geometry replacement",
			                       edit.objectName.c_str() );
		mLastScope = Dirty_Camera;
		return true;
	}

	if( edit.op == SceneEdit::AgentInsertChunk || edit.op == SceneEdit::AgentRemoveChunk )
	{
		// Shared-undo U2 (Redo direction): re-apply the ORIGINAL verb (re-insert the captured bytes / re-remove
		// by name).  The FIRST apply of a fresh chunk-CRUD commit never reaches here (the mutation already
		// landed via Job::ApplyCstInsertChunk/ApplyCstRemoveChunk before the history push -- see
		// PushAgentChunkCrudEdit); this arm is exercised by Redo only.
		if( !mJob ) return false;
		const bool forInsertOp = ( edit.op == SceneEdit::AgentInsertChunk );
		{
			bool diagnosed = false;
			if( !RouteAgentChunkCrud_( edit, forInsertOp, /*forward*/ true, &diagnosed ) ) return false;
			if( diagnosed )
				GlobalLog()->PrintEx( eLog_Error, "SceneEditor::Redo:: agent chunk %s on `%s` re-applied via a full re-derive that DIAGNOSED (see log) -- the Document WAS mutated and rebound (history still advances); not a clean redo",
				                       forInsertOp ? "insert" : "remove", edit.objectName.c_str() );
		}
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

	// Phase B: re-mark the (single-edit) entity dirty on redo.  Composite
	// inner edits are marked inside the replay loop.
	//
	// P3-e fix (round 1): symmetric to Undo() -- the mark happens AFTER
	// ApplyForwardMutation actually lands, not before; see Undo()'s doc for
	// the full rationale (a refused replay -- e.g. the S4b occurrence drift
	// guard -- must not flip HasUnsavedChanges() when nothing changed).

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
			if( !ApplyForwardMutation( inner, /*isReplay*/true ) ) { failed = true; break; }   // P1: stop + roll back atomically
			MarkEditEntityDirty( inner );   // P3-e: only after the replay actually landed
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
			//
			// P3 (S5): symmetric to Undo()'s composite-failure arm above -- each `inner`
			// in `applied` already had MarkEditEntityDirty called when its own forward
			// step landed, and this rollback does not retract that mark.  Same accepted
			// over-marking, same reasoning: see Undo()'s composite-failure comment.
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
	// isReplay: a Redo is a history replay, so the 87 container gates are
	// suppressed -- see ApplyForwardMutation's doc and the block comment above
	// ApplyRevertMutation.  A refused redo is escapable (any new edit clears the
	// redo stack) where a refused undo is not, but the argument is the same and
	// the pair must be symmetric or the comment that says so is a lie.
	if( !ApplyForwardMutation( edit, /*isReplay*/true ) ) {
		mHistory.RestoreLastRedoFromUndo();
		return false;
	}
	MarkEditEntityDirty( edit );   // P3-e: only after the replay actually landed
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

// P3a slice 3: public forwarder to the file-local canonical camera-op math
// (see the header doc).  Thin by design -- ONE implementation.
void RISE::SceneEditor::ApplyCameraOpToCamera(
	Implementation::CameraCommon& cam, const SceneEdit& e, const Scalar sceneScale )
{
	ApplyCameraOpForward( cam, e, sceneScale );
}
