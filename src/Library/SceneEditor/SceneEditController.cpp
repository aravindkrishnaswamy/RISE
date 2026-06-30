//////////////////////////////////////////////////////////////////////
//
//  SceneEditController.cpp - Implementation.
//
//  Cancel-restart loop:
//
//    Render thread:
//      while (running) {
//        wait until edit_pending OR !running
//        edit_pending = false
//        cancel_progress.Reset()      // clear stale flag from prior iteration
//        rendering = true
//        DoOneRenderPass()            // ~ms to seconds, polls cancel_progress
//        rendering = false
//      }
//
//    UI thread (called from each Editor::Apply):
//      KickRender:
//        edit_pending = true
//        if (rendering)
//            cancel_progress.RequestCancel()  // ++cancel_count
//        cv.notify_one()
//
//  Stop() trips both running=false and the cancel flag, then joins.
//
//  See docs/INTERACTIVE_EDITOR_PLAN.md §4.6.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SceneEditController.h"
#include "../Utilities/Transformable.h"   // F6: CaptureTransformState at gizmo drag-start
#include "ObjectIntrospection.h"
#include "LightIntrospection.h"
#include "RasterizerIntrospection.h"
#include "FilmIntrospection.h"
#include "MaterialIntrospection.h"
#include "MediaIntrospection.h"
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IScenePriv.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IKeyframable.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/IFilm.h"
#include "../RISE_API.h"			// for RISE_API_CreateFilm (preview-scale Film swap)
#include "../Cameras/CameraCommon.h"
#include "../Cameras/PinholeCamera.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IRayCaster.h"
#include "../Intersection/RayIntersection.h"
#include "../Rendering/InteractivePelRasterizer.h"
#include "../Rendering/FrameStore.h"  // L6e-3 — per-pass interactive FrameStore
#include "../Rendering/Rasterizer.h"  // L6e-3 — Implementation::Rasterizer for SetFrameStore
#include "../Scene.h"                 // concrete Scene (transitive scene-state includes); transactional rollback no longer uses CreateSnapshot/RestoreFromSnapshot
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/RuntimeContext.h"
#include <chrono>
#include <cstdio>
#include <ctime>          // for time() used by CloneActiveCamera dedup fallback
#include <string>

using namespace RISE;

// Out-of-class definitions for the static constexpr members so they have
// linkage in non-optimized builds.  In C++17+ static constexpr members
// are implicitly inline, but Xcode's -O0 Development config still emits
// an out-of-line reference when one of them is bound to a const-ref
// parameter (e.g. std::chrono::milliseconds(kRefineWakeMs) — the
// duration constructor takes its argument by const&).  Without these
// definitions, that path produces an "Undefined symbol kRefineWakeMs"
// linker error in Development builds.  Optimized configs constant-fold
// the reference and link cleanly even without these definitions, but
// the definitions are harmless there.
constexpr unsigned int SceneEditController::kPreviewScaleMin;
constexpr unsigned int SceneEditController::kPreviewScaleMax;
constexpr unsigned int SceneEditController::kPreviewScaleMotionStart;
constexpr int          SceneEditController::kTargetMs;
constexpr int          SceneEditController::kSlowMs;
constexpr int          SceneEditController::kFastMs;
constexpr int          SceneEditController::kRefineIdleMs;
constexpr int          SceneEditController::kRefineWakeMs;
constexpr int          SceneEditController::kScrubWatchdogMs;
constexpr int          SceneEditController::kNumCategories;

// Process-global epoch counter — incremented on every controller
// construction so each new controller starts at a unique mSceneEpoch
// value.  Without this, a scene-reload (destroys controller A, builds
// controller B) would have B start at the same epoch the platform UI
// already cached from A, and the per-section entity lists would not
// re-pull.  Atomic so concurrent host-bridge constructions on different
// threads (test harnesses) don't race.
static std::atomic<unsigned int>& NextEpoch() {
	static std::atomic<unsigned int> s_next( 1 );
	return s_next;
}

SceneEditController::SceneEditController( IJobPriv& job, IRasterizer* interactiveRasterizer )
: mJob( job )
, mInteractiveRasterizer( interactiveRasterizer )
, mInteractiveImpl( dynamic_cast<Implementation::InteractivePelRasterizer*>( interactiveRasterizer ) )
, mEditor( *job.GetScene() )
, mTool( Tool::Select )
// Photoshop-style per-category memory.  Initialize each slot to
// its category default so first-time-click on any slot has a
// meaningful tool to fall back to.  Element [Select] = Select,
// [Camera] = OrbitCamera, [ObjectTransform] = TranslateObject.
, mLastSubToolPerCategory{ Tool::Select, Tool::OrbitCamera, Tool::TranslateObject }
, mGizmoDrag()                              // zero-initialized; `active` defaults to false
, mSelectionCategory( Category::None )
, mSelectionName()
// `mSectionExpanded` and `mSelectionByCategory` are value-init'd via
// default-member-init (bool defaults to false; String defaults to
// empty) so they need no explicit ctor entry.  Listed here as a
// documentation reminder.
, mSceneEpoch( NextEpoch().fetch_add( 1, std::memory_order_acq_rel ) )
, mLastPx( 0, 0 )
, mPointerDown( false )
, mGestureOpenedComposite( false )
, mScrubOpenedComposite( false )
, mScrubInProgress( false )
, mPreviewSink( 0 )
, mProgressSink( 0 )
, mLogSink( 0 )
, mInteractiveFrameStore( 0 )
, mCancelProgress( 0 )
, mRenderThread()
, mMutex()
, mCV()
, mRunning( false )
, mEditPending( false )
, mRendering( false )
, mSaving( false )
, mCancelCount( 0 )
, mRenderCount( 0 )
, mFullResW( 0 )
, mFullResH( 0 )
, mPreviewScale( 1 )
, mLastEditTimeMs( 0 )
, mInRefinementPass( false )
, mPolishState( static_cast<int>( PolishState::None ) )
, mTxnOpen( false )
, mTxnBaseline()
{
	if( mInteractiveRasterizer )
	{
		mInteractiveRasterizer->addref();
	}
	// Per-category arrays — bool array members are NOT
	// default-init'd to false (would be indeterminate otherwise).
	// String members default-init correctly to empty.
	for( int i = 0; i < kNumCategories; ++i ) {
		mSectionExpanded[i] = false;
	}
	// Bind the editor to the Job's scene + managers (see RebindEditorToJob -- also re-run after a whole-scene
	// re-derive).  Test harnesses that build a SceneEditor directly skip this and degrade to "transform /
	// camera ops only" mode.
	RebindEditorToJob();
}

// Re-point the editor at the Job's CURRENT scene + managers.  Called at construction AND after every whole-scene
// rebuild: a scene_variant switch's Job::RederiveCstWithVariant does ClearAll() (releasing the Scene + all
// managers) then re-derives fresh ones -- without this re-bind, mEditor's cached scene + manager pointers dangle
// into freed storage and the next edit/gizmo/undo is a use-after-free.
void SceneEditController::RebindEditorToJob()
{
	mEditor.RebindScene( *mJob.GetScene() );
	mEditor.SetMaterialManager( mJob.GetMaterials() );          // Phase 3: name resolution for SetObjectMaterial/Shader
	mEditor.SetShaderManager( mJob.GetShaders() );
	mEditor.SetPainterManager( mJob.GetPainters() );            // Phase 4: painter-name -> IPainter*/IScalarPainter*
	mEditor.SetScalarPainterManager( mJob.GetScalarPainters() );
	mEditor.SetJob( &mJob );                                    // medium-name resolution (IJobPriv : IJob)
}

namespace {

// Milliseconds since the steady-clock epoch.  Used to track edit
// idleness for refinement and resume-snap decisions.
inline long long NowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
}

inline bool IsCameraMotionTool( SceneEditController::Tool t )
{
	using T = SceneEditController::Tool;
	return t == T::OrbitCamera
	    || t == T::PanCamera
	    || t == T::ZoomCamera
	    || t == T::RollCamera;
}

inline bool IsObjectMotionTool( SceneEditController::Tool t )
{
	using T = SceneEditController::Tool;
	return t == T::TranslateObject
	    || t == T::RotateObject
	    || t == T::ScaleObject;
}

// Property-string parsers used by the Object / Light branches of
// `SetProperty`.  Anonymous-namespace helpers in the codebase's
// older C++ idiom — explicit return types, no captures, no `auto`.
inline bool ParsePropertyVec3( const String& valueStr, Vector3& out )
{
	double x = 0;
	double y = 0;
	double z = 0;
	if( std::sscanf( valueStr.c_str(), "%lf %lf %lf", &x, &y, &z ) != 3 ) return false;
	out = Vector3( Scalar( x ), Scalar( y ), Scalar( z ) );
	return true;
}

inline bool ParsePropertyScalar( const String& valueStr, Scalar& out )
{
	double v = 0;
	if( std::sscanf( valueStr.c_str(), "%lf", &v ) != 1 ) return false;
	out = Scalar( v );
	return true;
}

inline bool ParsePropertyBool( const String& valueStr, bool& out )
{
	if( valueStr == String( "true" )  || valueStr == String( "1" ) )
	{
		out = true;
		return true;
	}
	if( valueStr == String( "false" ) || valueStr == String( "0" ) )
	{
		out = false;
		return true;
	}
	return false;
}

// --- Gizmo math ------------------------------------------------------
//
// World→screen projection through the camera's `mxTrans` matrix (the
// same matrix `GenerateRay` uses to map screen → world).  Returns
// `(sx, sy)` in TARGET-pixel WIDGET-Y-DOWN space — the stable full-
// res image space that platform overlays and pointer events both
// align with.  Rescaling matters during fast drags: the controller
// swaps the scene's Film to a subsampled size for the duration of
// each in-flight render pass (see `DoOneRenderPass`'s preview-scale
// path), which rebuilds `mxTrans` against the CURRENT subsampled
// dims.  The projection therefore lands in current-pixel space; we
// must rescale to target-pixel space so the handles stay locked to
// the object on screen during the drag's low-res frames.
//
// Derivation (intermediate values in mxTrans's source space —
// current-pixel-Y-UP per `PixelBasedPelRasterizer.cpp:614`'s
// `Point2(x, height - y)` feed of `GenerateRay`):
//
//   `mxTrans` maps screen point S=(sx,sy_image,0) → world point T.
//   The ray fired through S has origin O and direction (O - T) (see
//   `PinholeCamera::GenerateRay`'s use of `mkVector3(origin, transP)`
//   = origin - transP — the screen plane sits BEHIND the pinhole at
//   sensor-plane distance 1).  Ray equation: P = O + t·(O - T) for
//   t > 0 in-front-of-camera.
//
//   Solving for T gives T = O - (P - O)/t.  Apply `invMxTrans`.  Let
//   A = invM·P and B = invM·O.  Then (sx, sy_image, 0) = B + (1/t)·
//   (B - A), and the z-component constraint gives 1/t = B.z / (A.z -
//   B.z).  For the standard pinhole construction in
//   `CameraCommon::Recompute`, B.z == 1.  In-front check is `A.z > B.z`.
//
// Rescale (current → target): `sx_target = sx_current · (targetW /
// currentW)`, `sy_target_image = sy_current · (targetH / currentH)`.
// Then flip Y around target height: `sy_widget = targetH -
// sy_target_image`.  Returns false for behind-camera points, points
// AT the eye, degenerate matrices, or non-positive dims.
inline bool ProjectWorldToScreen_(
	const Matrix4& mxTrans,
	const Point3&  origin,
	const Point3&  worldPos,
	double         currentWidth,
	double         currentHeight,
	double         targetWidth,
	double         targetHeight,
	double&        outSx,
	double&        outSy )
{
	const Scalar det = Matrix4Ops::Determinant( mxTrans );
	if( det == 0.0 ) return false;
	if( !( currentWidth > 0.0 ) || !( currentHeight > 0.0 ) ) return false;
	if( !( targetWidth  > 0.0 ) || !( targetHeight  > 0.0 ) ) return false;
	const Matrix4 inv = Matrix4Ops::Inverse( mxTrans );
	const Point3 A = Point3Ops::Transform( inv, worldPos );
	const Point3 B = Point3Ops::Transform( inv, origin );
	const Scalar denom = A.z - B.z;
	if( denom == 0.0 ) return false;
	const Scalar invT = B.z / denom;
	if( !( invT > 0.0 ) ) return false;               // behind camera or AT eye
	const double sx_current       = static_cast<double>( B.x + invT * ( B.x - A.x ) );
	const double sy_image_current = static_cast<double>( B.y + invT * ( B.y - A.y ) );
	const double sx               = sx_current * ( targetWidth  / currentWidth  );
	const double sy_image_target  = sy_image_current * ( targetHeight / currentHeight );
	const double sy               = targetHeight - sy_image_target;   // → widget-Y-DOWN
	if( !std::isfinite( sx ) || !std::isfinite( sy ) ) return false;
	outSx = sx;
	outSy = sy;
	return true;
}

// Constants controlling handle layout.  Screen-space lengths are in
// the camera's CURRENT image-pixel space — platform overlays scale
// them to widget space using the same `fullW`/`fullH` normalisation
// they apply to pointer events.  Chosen by hand to give comfortable
// click targets on a 1280×720 viewport with a 1.5× HiDPI factor;
// can be re-tuned without breaking the math or the C-API.
constexpr double kAxisArrowLengthPx = 80.0;   // tip distance from pivot
constexpr double kAxisPlaneOffsetPx = 40.0;   // plane-handle offset along each axis
constexpr double kAxisRingRadiusPx  = 80.0;   // rotation ring radius
constexpr double kCenterRadiusPx    = 16.0;   // screen-center / uniform-scale glyph
constexpr double kAxisHitRadiusPx   = 14.0;   // hit-test radius for axis arrows / cubes
constexpr double kPlaneHitRadiusPx  = 18.0;   // hit-test radius for plane / ring tangent
constexpr double kRingHitRadiusPx   = 10.0;   // tolerance around the projected ring circumference

// `ProjectWorldToScreen_` is derived for the standard pinhole `m3 ·
// m2 · m1` matrix chain in `CameraCommon::Recompute` (translate to
// screen origin, FOV-based stretch, basis transform) and the
// perspective ray semantics `mkVector3(origin, transP)` in
// `PinholeCamera::GenerateRay`.  Other camera types — orthographic
// (parallel rays, separate inverse math in `CameraUtilities.cpp`),
// thin-lens (`mxTrans = frame * ComputeScaleFromAR()`; no `m1` shift),
// fisheye (non-linear projection) — don't satisfy that derivation,
// so the gizmo would silently misalign or disappear off-screen.  We
// gate strictly on PinholeCamera until proper per-type projections
// land.  Returns true iff `cam` is a usable PinholeCamera.
inline bool IsGizmoSupportedCamera_( const ICamera* cam )
{
	return dynamic_cast<const Implementation::PinholeCamera*>( cam ) != 0;
}

// Probe the world axes at the pivot to capture their screen-space
// directions (pixels per world unit).  `outAxisDir[a][0]` is the
// x-component of world axis `a` projected at the pivot, etc.
// `outAxisOk[a]` is false when the axis is colinear with view at the
// pivot (the projection collapses to a single point).
inline void ProbeAxesAtPivot_(
	const Matrix4& mxTrans,
	const Point3&  origin,
	const Point3&  pivotWorld,
	double         currentWidth,
	double         currentHeight,
	double         targetWidth,
	double         targetHeight,
	double         cx,
	double         cy,
	double         outAxisDirX[3],
	double         outAxisDirY[3],
	bool           outAxisOk[3] )
{
	for( int a = 0; a < 3; ++a ) {
		Vector3 v( 0, 0, 0 );
		switch( a ) {
		case 0: v = Vector3( 1, 0, 0 ); break;
		case 1: v = Vector3( 0, 1, 0 ); break;
		case 2: v = Vector3( 0, 0, 1 ); break;
		}
		const Point3 axisWorld(
			pivotWorld.x + v.x, pivotWorld.y + v.y, pivotWorld.z + v.z );
		double ax = 0, ay = 0;
		const bool ok = ProjectWorldToScreen_(
			mxTrans, origin, axisWorld,
			currentWidth, currentHeight, targetWidth, targetHeight, ax, ay );
		if( !ok ) { outAxisOk[a] = false; continue; }
		const double dx = ax - cx;
		const double dy = ay - cy;
		const double mag = std::sqrt( dx*dx + dy*dy );
		if( !( mag > 0.0 ) || !std::isfinite( mag ) ) {
			outAxisOk[a] = false;
			continue;
		}
		outAxisDirX[a] = dx;       // target-pixels per world unit along x (widget-Y-DOWN)
		outAxisDirY[a] = dy;
		outAxisOk[a]   = true;
	}
}

// Construct the per-tool gizmo handles for an Object pivot at
// `pivotWorld` viewed through `(mxTrans, origin)`.  `outHandles` is
// cleared first; on failure (pivot doesn't project) it remains empty.
//
// Handle ordering MATTERS for hit-test priority: the controller's
// hit-test (B3) iterates front-to-back and accepts the first hit, so
// CENTER glyphs go FIRST (they sit on top of axis arrows visually).
// Within axes, X / Y / Z order is canonical.
inline void BuildGizmoHandles_(
	SceneEditController::Tool                       tool,
	const Matrix4&                                  mxTrans,
	const Point3&                                   origin,
	const Point3&                                   pivotWorld,
	double                                          currentWidth,
	double                                          currentHeight,
	double                                          targetWidth,
	double                                          targetHeight,
	std::vector<SceneEditController::GizmoHandle>&  outHandles )
{
	using Kind = SceneEditController::GizmoHandle::Kind;
	using T    = SceneEditController::Tool;
	outHandles.clear();

	double cx = 0, cy = 0;
	if( !ProjectWorldToScreen_( mxTrans, origin, pivotWorld,
		currentWidth, currentHeight, targetWidth, targetHeight, cx, cy ) ) return;

	// Probe each world axis with a fixed world-space delta so we can
	// derive the screen-space direction of that axis at the pivot.
	// Direction in screen-space = normalised (axisProj - pivotProj).
	// World-axis-only convention (per the locked design).
	const double kAxisProbeWorld = 1.0;
	double axisDirX[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
	bool   axisOk[3]      = { false, false, false };

	for( int a = 0; a < 3; ++a ) {
		Vector3 v( 0, 0, 0 );
		switch( a ) {
		case 0: v = Vector3( kAxisProbeWorld, 0, 0 ); break;
		case 1: v = Vector3( 0, kAxisProbeWorld, 0 ); break;
		case 2: v = Vector3( 0, 0, kAxisProbeWorld ); break;
		}
		const Point3 axisWorld(
			pivotWorld.x + v.x, pivotWorld.y + v.y, pivotWorld.z + v.z );
		double ax = 0, ay = 0;
		if( !ProjectWorldToScreen_( mxTrans, origin, axisWorld,
			currentWidth, currentHeight, targetWidth, targetHeight, ax, ay ) ) continue;
		double dx = ax - cx;
		double dy = ay - cy;
		const double mag = std::sqrt( dx*dx + dy*dy );
		if( !( mag > 0.0 ) || !std::isfinite( mag ) ) continue;
		axisDirX[a][0] = dx / mag;
		axisDirX[a][1] = dy / mag;
		axisOk[a] = true;
	}

	auto pushHandle = [&]( int kind, int axis, double sx, double sy, double r ) {
		SceneEditController::GizmoHandle h;
		h.kind = kind;
		h.axis = axis;
		h.screenX = sx;
		h.screenY = sy;
		h.screenRadius = r;
		outHandles.push_back( h );
	};

	switch( tool ) {
	case T::TranslateObject:
		// Center first (front-to-back priority).
		pushHandle( static_cast<int>( Kind::ScreenCenter ), -1, cx, cy, kCenterRadiusPx );
		// Axis-plane handles at the midpoint of each axis pair.
		// `axis` field stores the axis NOT in the plane: YZ plane → axis=0,
		// XZ plane → axis=1, XY plane → axis=2.
		for( int a = 0; a < 3; ++a ) {
			const int b = ( a + 1 ) % 3;
			const int c = ( a + 2 ) % 3;
			if( !axisOk[b] || !axisOk[c] ) continue;
			const double sx = cx + ( axisDirX[b][0] + axisDirX[c][0] ) * kAxisPlaneOffsetPx;
			const double sy = cy + ( axisDirX[b][1] + axisDirX[c][1] ) * kAxisPlaneOffsetPx;
			pushHandle( static_cast<int>( Kind::AxisPlane ), a, sx, sy, kPlaneHitRadiusPx );
		}
		// Axis arrows last so they're hit-tested AFTER planes (planes
		// sit closer to centre and would otherwise eat clicks meant
		// for the longer arrow shafts).
		for( int a = 0; a < 3; ++a ) {
			if( !axisOk[a] ) continue;
			const double sx = cx + axisDirX[a][0] * kAxisArrowLengthPx;
			const double sy = cy + axisDirX[a][1] * kAxisArrowLengthPx;
			pushHandle( static_cast<int>( Kind::AxisArrow ), a, sx, sy, kAxisHitRadiusPx );
		}
		break;

	case T::RotateObject:
		// View-aligned screen ring first — outermost; the user clicks
		// "outside" the world-axis rings to trigger view-axis spin.
		pushHandle( static_cast<int>( Kind::ScreenRing ), -1, cx, cy, kAxisRingRadiusPx + 20.0 );
		// World-axis rings.  Stored centre is the pivot's projection;
		// `screenRadius` is the ring radius in pixels.  The platform
		// overlay draws an ellipse from the world-space ring projected
		// (B5/B6/B7); hit-test (B3) uses distance from the projected
		// ellipse approximation.
		for( int a = 0; a < 3; ++a ) {
			if( !axisOk[a] ) continue;
			pushHandle( static_cast<int>( Kind::AxisRing ), a, cx, cy, kAxisRingRadiusPx );
		}
		break;

	case T::ScaleObject:
		// Uniform-scale cube at center first.
		pushHandle( static_cast<int>( Kind::UniformScaleCube ), -1, cx, cy, kCenterRadiusPx );
		// Per-axis scale cubes at the tip of each world axis arrow.
		for( int a = 0; a < 3; ++a ) {
			if( !axisOk[a] ) continue;
			const double sx = cx + axisDirX[a][0] * kAxisArrowLengthPx;
			const double sy = cy + axisDirX[a][1] * kAxisArrowLengthPx;
			pushHandle( static_cast<int>( Kind::AxisScaleHandle ), a, sx, sy, kAxisHitRadiusPx );
		}
		break;

	default:
		// Not an object-transform tool — no gizmo.
		break;
	}
}

}  // namespace

SceneEditController::~SceneEditController()
{
	Stop();
	if( mInteractiveRasterizer )
	{
		mInteractiveRasterizer->release();
		mInteractiveRasterizer = 0;
	}
	// L6e-3 — release our per-pass FrameStore.  Stop() already joined
	// the render thread, so no DoOneRenderPass is in flight by the
	// time we reach here.
	if( mInteractiveFrameStore )
	{
		mInteractiveFrameStore->release();
		mInteractiveFrameStore = 0;
	}
	// Inverse-edit rollback holds NO snapshot — a transaction left open
	// at teardown needs no resource release; the (uncommitted) live edits
	// simply remain, exactly as they would after a commit.
}

// Lifecycle -----------------------------------------------------------

void SceneEditController::Start()
{
	bool expected = false;
	if( !mRunning.compare_exchange_strong( expected, true ) )
	{
		return;  // already running
	}

	// Refresh the cancellable progress with whatever sink is installed
	// at Start time.  Sinks set later via Set*Sink() take effect on the
	// NEXT Start() — this is intentional, mirrors the existing platform
	// adapter convention of installing sinks at construction.
	mCancelProgress.SetInner( mProgressSink );
	mCancelProgress.Reset();
	mCancelCount.store( 0, std::memory_order_release );
	mRenderCount.store( 0, std::memory_order_release );

	// Prime the stable full-res dimensions cache with the camera's
	// current dims.  Bridges read these via GetCameraDimensions for
	// pointer-event coord conversion; if Start runs before any render
	// pass has fired, the bridges still need a valid reference.
	// DoOneRenderPass refreshes these on every pass before swapping
	// in the preview-scale dims, so a scene reload picks up new dims
	// on the next render without further bookkeeping.
	if( const IScene* scene = mJob.GetScene() ) {
		if( const IFilm* film = scene->GetFilm() ) {
			mFullResW.store( film->GetWidth(),  std::memory_order_release );
			mFullResH.store( film->GetHeight(), std::memory_order_release );
		}
	}

	mRenderThread = std::thread( &SceneEditController::RenderLoop, this );
}

void SceneEditController::Stop()
{
	bool expected = true;
	if( !mRunning.compare_exchange_strong( expected, false ) )
	{
		return;  // not running
	}

	// Trip cancel BEFORE notifying.  Hold mMutex around the wakeup to
	// prevent a lost wakeup: if we notified outside the lock, the
	// render thread could be between its predicate check and the
	// kernel park inside cv.wait, and miss the notify forever — which
	// would deadlock join() below.  C++ guarantees no missed wakeups
	// only when the notifier has held the mutex used by the waiter.
	mCancelProgress.RequestCancel();
	{
		std::lock_guard<std::mutex> lk( mMutex );
	}
	mCV.notify_one();

	if( mRenderThread.joinable() )
	{
		mRenderThread.join();
	}
}

bool SceneEditController::IsRunning() const
{
	return mRunning.load( std::memory_order_acquire );
}

// Sinks ---------------------------------------------------------------

void SceneEditController::SetPreviewSink( IRasterizerOutput* sink )  { mPreviewSink  = sink; }
void SceneEditController::SetProgressSink( IProgressCallback* sink ) { mProgressSink = sink; }
void SceneEditController::SetLogSink( ILogPrinter* sink )            { mLogSink      = sink; }

// Tool state ----------------------------------------------------------

SceneEditController::ToolCategory SceneEditController::CategoryForTool( Tool t )
{
	switch( t ) {
	case Tool::Select:           return ToolCategory::Select;
	case Tool::TranslateObject:  return ToolCategory::ObjectTransform;
	case Tool::RotateObject:     return ToolCategory::ObjectTransform;
	case Tool::ScaleObject:      return ToolCategory::ObjectTransform;
	case Tool::OrbitCamera:      return ToolCategory::Camera;
	case Tool::PanCamera:        return ToolCategory::Camera;
	case Tool::ZoomCamera:       return ToolCategory::Camera;
	case Tool::RollCamera:       return ToolCategory::Camera;
	case Tool::ScrubTimeline:    return ToolCategory::Select;  // timeline lives in
	                                                            // the bottom bar, not
	                                                            // the main toolbar —
	                                                            // fall back to Select
	                                                            // for the slot membership
	                                                            // query.
	}
	return ToolCategory::Select;
}

SceneEditController::Tool SceneEditController::DefaultSubToolForCategory( ToolCategory cat )
{
	switch( cat ) {
	case ToolCategory::Select:          return Tool::Select;
	case ToolCategory::Camera:          return Tool::OrbitCamera;     ///< most-used camera tool
	case ToolCategory::ObjectTransform: return Tool::TranslateObject; ///< most common transform
	}
	return Tool::Select;
}

SceneEditController::Tool SceneEditController::GetLastSubToolForCategory( ToolCategory cat ) const
{
	const int idx = static_cast<int>( cat );
	if( idx < 0 || idx >= kNumToolCategories ) return DefaultSubToolForCategory( cat );
	return mLastSubToolPerCategory[ idx ];
}

void SceneEditController::SetTool( Tool t )
{
	mTool = t;
	// Photoshop-style memory: remember this sub-tool as the
	// category's last-used.  Single-click on the slot will resume
	// this tool; the flyout always offers the full set.
	const int idx = static_cast<int>( CategoryForTool( t ) );
	if( idx >= 0 && idx < kNumToolCategories ) {
		mLastSubToolPerCategory[ idx ] = t;
	}
}

SceneEditController::Tool SceneEditController::CurrentTool() const { return mTool; }

// Gizmo handle math ---------------------------------------------------

void SceneEditController::RefreshGizmoHandles()
{
	mGizmoHandles.clear();

	// Only Object-transform tools draw gizmos.
	if( CategoryForTool( mTool ) != ToolCategory::ObjectTransform ) return;

	// Object selection required.
	const String objName = mSelectionByCategory[ static_cast<int>( Category::Object ) ];
	if( objName.empty() ) return;
	const IScene* scene = mJob.GetScene();
	if( !scene ) return;
	const IObjectManager* objs = scene->GetObjects();
	if( !objs ) return;
	IObjectPriv* obj = objs->GetItem( objName.c_str() );
	if( !obj ) return;

	const Matrix4 objM = obj->GetFinalTransformMatrix();
	const Point3 pivotWorld( objM._30, objM._31, objM._32 );

	const ICamera* cam = scene->GetCamera();
	if( !cam ) return;
	// Gizmo projection is derived for the standard PinholeCamera
	// matrix chain; other camera types are skipped (see
	// `IsGizmoSupportedCamera_` for the rationale).
	if( !IsGizmoSupportedCamera_( cam ) ) return;

	// Stable full-res target dims — what the platform overlay uses
	// as `surface` size and what pointer events normalize through.
	unsigned int stableW = 0, stableH = 0;
	if( !GetCameraDimensions( stableW, stableH ) || stableW == 0 || stableH == 0 ) return;

	// CURRENT camera frame dims — what `mxTrans` projects into.
	// During an in-flight render pass these are subsampled (preview-
	// scale swap in `DoOneRenderPass`); between passes they equal
	// the stable dims.  `CameraCommon::GetWidth/Height` are
	// non-virtual getters on the concrete camera (note that ICamera
	// itself doesn't expose dims since the 2026-05 Film refactor).
	const Implementation::CameraCommon* camC =
		dynamic_cast<const Implementation::CameraCommon*>( cam );
	const unsigned int curW = camC ? camC->GetWidth()  : stableW;
	const unsigned int curH = camC ? camC->GetHeight() : stableH;
	if( curW == 0 || curH == 0 ) return;

	BuildGizmoHandles_( mTool, cam->GetMatrix(), cam->GetLocation(),
	                    pivotWorld,
	                    static_cast<double>( curW ),    static_cast<double>( curH ),
	                    static_cast<double>( stableW ), static_cast<double>( stableH ),
	                    mGizmoHandles );
}

unsigned int SceneEditController::GizmoHandleCount() const
{
	return static_cast<unsigned int>( mGizmoHandles.size() );
}

int SceneEditController::GizmoHandleKind( unsigned int idx ) const
{
	if( idx >= mGizmoHandles.size() ) return 0;
	return mGizmoHandles[ idx ].kind;
}

int SceneEditController::GizmoHandleAxis( unsigned int idx ) const
{
	if( idx >= mGizmoHandles.size() ) return -1;
	return mGizmoHandles[ idx ].axis;
}

double SceneEditController::GizmoHandleScreenX( unsigned int idx ) const
{
	if( idx >= mGizmoHandles.size() ) return 0.0;
	return mGizmoHandles[ idx ].screenX;
}

double SceneEditController::GizmoHandleScreenY( unsigned int idx ) const
{
	if( idx >= mGizmoHandles.size() ) return 0.0;
	return mGizmoHandles[ idx ].screenY;
}

double SceneEditController::GizmoHandleScreenRadius( unsigned int idx ) const
{
	if( idx >= mGizmoHandles.size() ) return 0.0;
	return mGizmoHandles[ idx ].screenRadius;
}

bool SceneEditController::ForTest_ProjectWorldToScreen(
	double wx, double wy, double wz,
	double& outSx, double& outSy ) const
{
	const IScene* scene = mJob.GetScene();
	const ICamera* cam = scene ? scene->GetCamera() : 0;
	if( !cam ) return false;
	if( !IsGizmoSupportedCamera_( cam ) ) return false;
	unsigned int stableW = 0, stableH = 0;
	if( !GetCameraDimensions( stableW, stableH ) || stableW == 0 || stableH == 0 ) return false;
	const Implementation::CameraCommon* camC =
		dynamic_cast<const Implementation::CameraCommon*>( cam );
	const unsigned int curW = camC ? camC->GetWidth()  : stableW;
	const unsigned int curH = camC ? camC->GetHeight() : stableH;
	if( curW == 0 || curH == 0 ) return false;
	return ProjectWorldToScreen_(
		cam->GetMatrix(), cam->GetLocation(),
		Point3( Scalar( wx ), Scalar( wy ), Scalar( wz ) ),
		static_cast<double>( curW ),    static_cast<double>( curH ),
		static_cast<double>( stableW ), static_cast<double>( stableH ),
		outSx, outSy );
}

bool SceneEditController::ForTest_GetSelectionPivotWorld(
	double& wx, double& wy, double& wz ) const
{
	const String objName = mSelectionByCategory[ static_cast<int>( Category::Object ) ];
	if( objName.empty() ) return false;
	const IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	const IObjectManager* objs = scene->GetObjects();
	if( !objs ) return false;
	IObjectPriv* obj = objs->GetItem( objName.c_str() );
	if( !obj ) return false;
	const Matrix4 m = obj->GetFinalTransformMatrix();
	wx = static_cast<double>( m._30 );
	wy = static_cast<double>( m._31 );
	wz = static_cast<double>( m._32 );
	return true;
}

int SceneEditController::GizmoHandleAt( const Point2& px ) const
{
	using K = GizmoHandle::Kind;
	int hitIdx = -1;
	double hitDist2 = 0;
	for( unsigned int i = 0; i < mGizmoHandles.size(); ++i ) {
		const GizmoHandle& h = mGizmoHandles[i];
		const double dx = static_cast<double>( px.x ) - h.screenX;
		const double dy = static_cast<double>( px.y ) - h.screenY;
		const double dist2 = dx*dx + dy*dy;
		// Ring handles: hit-test the CIRCUMFERENCE at distance
		// `screenRadius`, not the disc.  All other kinds use a disc
		// of radius `screenRadius`.
		const bool isRing =
			h.kind == static_cast<int>( K::AxisRing ) ||
			h.kind == static_cast<int>( K::ScreenRing );
		bool inside = false;
		double effDist2 = dist2;
		if( isRing ) {
			const double dist = std::sqrt( dist2 );
			const double ringErr = std::fabs( dist - h.screenRadius );
			inside = ringErr < kRingHitRadiusPx;
			effDist2 = ringErr * ringErr;
		} else {
			inside = dist2 < h.screenRadius * h.screenRadius;
		}
		if( !inside ) continue;
		// Front-to-back priority: first hit wins.  Earlier handles
		// in the array are conceptually "on top" — center/plane glyphs
		// hit-tested before axis arrows, matching the visual stacking
		// the platform overlay draws.
		if( hitIdx < 0 || effDist2 < hitDist2 ) {
			hitIdx = static_cast<int>( i );
			hitDist2 = effDist2;
			break;  // front-to-back: take the first array hit
		}
	}
	return hitIdx;
}

bool SceneEditController::IsGizmoDragActive() const
{
	return mGizmoDrag.active;
}

int SceneEditController::ActiveGizmoKind() const
{
	return mGizmoDrag.active ? mGizmoDrag.kind : -1;
}

int SceneEditController::ActiveGizmoAxis() const
{
	return mGizmoDrag.active ? mGizmoDrag.axis : -1;
}

// Pointer events ------------------------------------------------------

void SceneEditController::OnPointerDown( const Point2& px )
{
	mPointerDown.store( true, std::memory_order_release );
	mLastPx = px;
	mLastEditTimeMs.store( NowMs(), std::memory_order_release );

	// P1: defensively close any composite a PRIOR pointer gesture left open (a
	// lost pointer-up, or a double-down with no intervening up).  Without this the
	// orphaned composite would NEST under the new gesture and only one would close
	// on pointer-up, leaving mCompositeDepth >= 1 forever -- IsCompositeOpen() then
	// permanently blocks transactions and history grows unbounded.
	if( mGestureOpenedComposite ) {
		mEditor.EndComposite();
		mGestureOpenedComposite = false;
	}

	// Bump preview scale so each render pass completes within the
	// 30Hz budget while the user is dragging.  DoOneRenderPass will
	// adapt further based on measured wall-clock per frame.
	bool isMotionTool = false;

	switch( mTool )
	{
	case Tool::Select:
		// Pick whichever object is under the cursor.  Sets mSelected
		// to the hit object's name (or empty if no hit) so the
		// properties panel can switch to Object mode.  Coords are
		// taken as-is in image-pixel space — the bridge already
		// passes pixel coords; HiDPI / aspect-fit conversion is the
		// bridge's job.
		PickAt( px );
		break;

	case Tool::TranslateObject:
	case Tool::RotateObject:
	case Tool::ScaleObject:
		if( mSelectionCategory == Category::Object && mSelectionName.size() > 1 )
		{
			mEditor.BeginComposite( "Drag" );
			mGestureOpenedComposite = true;   // P1: record what this gesture opened
			isMotionTool = true;

			// Gizmo hit-test.  Refresh the handle array against the
			// CURRENT camera + object state, then check whether the
			// pointer landed on any handle.  On hit, capture the
			// drag-start state so OnPointerMove can convert pointer
			// pixel deltas to constrained world deltas without each
			// frame re-probing the camera (which would let the math
			// drift if the camera moved mid-drag).
			RefreshGizmoHandles();
			const int hit = GizmoHandleAt( px );
			mGizmoDrag.active = false;
			if( hit >= 0 ) {
				const GizmoHandle& h = mGizmoHandles[ hit ];
				mGizmoDrag.kind = h.kind;
				mGizmoDrag.axis = h.axis;
				mGizmoDrag.anchorPxX = static_cast<double>( px.x );
				mGizmoDrag.anchorPxY = static_cast<double>( px.y );

				// Capture pivot (world) + projection.
				double wx = 0, wy = 0, wz = 0;
				if( ForTest_GetSelectionPivotWorld( wx, wy, wz ) ) {
					mGizmoDrag.pivotWorld = Point3( wx, wy, wz );

					// Capture the object's drag-start transform matrix
					// as the anchor for `ScaleObjectFromAnchor` (and
					// available to any other anchor-based op).  Apply
					// composes the per-frame factor on TOP of this
					// matrix via `ClearAllTransforms` +
					// `PushTopTransStack(anchor)` +
					// `PushTopTransStack(Stretch(factor))`, so the
					// final composition is `anchor · Stretch(factor)`.
					// This is what makes scale drag correct on objects
					// with non-trivial transform stacks (matrix imports
					// from glTF, prior SetObjectScale, etc.) —
					// decomposing column magnitudes and writing them
					// back as `SetObjectStretch` would double-apply.
					const IScene* sceneForObj = mJob.GetScene();
					const IObjectManager* objs = sceneForObj ? sceneForObj->GetObjects() : 0;
					IObjectPriv* obj = objs ? objs->GetItem( mSelectionName.c_str() ) : 0;
					if( obj ) {
						mGizmoDrag.dragStartMatrix = obj->GetFinalTransformMatrix();
						// F6: also capture the component-decomposed state so undo of
						// the ScaleObjectFromAnchor restores COMPONENTS (not a stack-
						// collapsed matrix) and a later absolute setter replaces the
						// right component instead of composing with the anchor.
						mGizmoDrag.dragStartStateValid = false;
						if( Implementation::Transformable* tt = dynamic_cast<Implementation::Transformable*>( obj ) ) {
							mGizmoDrag.dragStartState      = tt->CaptureTransformState();
							mGizmoDrag.dragStartStateValid = true;
						}
					} else {
						mGizmoDrag.dragStartMatrix = Matrix4Ops::Identity();
						mGizmoDrag.dragStartStateValid = false;
					}

					const IScene* scene = mJob.GetScene();
					const ICamera* cam = scene ? scene->GetCamera() : 0;
					unsigned int stableW = 0, stableH = 0;
					const bool stableOk = GetCameraDimensions( stableW, stableH )
					                    && stableW > 0 && stableH > 0;
					if( cam && stableOk && IsGizmoSupportedCamera_( cam ) ) {
						const Implementation::CameraCommon* camC =
							dynamic_cast<const Implementation::CameraCommon*>( cam );
						const unsigned int curW = camC ? camC->GetWidth()  : stableW;
						const unsigned int curH = camC ? camC->GetHeight() : stableH;
						if( curW > 0 && curH > 0 ) {
							const double curWd    = static_cast<double>( curW );
							const double curHd    = static_cast<double>( curH );
							const double stableWd = static_cast<double>( stableW );
							const double stableHd = static_cast<double>( stableH );
							double cx = 0, cy = 0;
							if( ProjectWorldToScreen_(
								cam->GetMatrix(), cam->GetLocation(),
								mGizmoDrag.pivotWorld,
								curWd, curHd, stableWd, stableHd, cx, cy ) )
							{
								mGizmoDrag.pivotScreenX = cx;
								mGizmoDrag.pivotScreenY = cy;
								ProbeAxesAtPivot_(
									cam->GetMatrix(), cam->GetLocation(),
									mGizmoDrag.pivotWorld,
									curWd, curHd, stableWd, stableHd, cx, cy,
									mGizmoDrag.axisDirX, mGizmoDrag.axisDirY,
									mGizmoDrag.axisOk );
								// For ring drags, record the pointer-down angle
								// so per-frame deltas come from `atan2(now) -
								// atan2(last)`.  All Y values are widget-Y-DOWN
								// to match the pointer-event convention.
								mGizmoDrag.prevAngle = std::atan2(
									static_cast<double>( px.y ) - cy,
									static_cast<double>( px.x ) - cx );
								mGizmoDrag.active = true;
							}
						}
					}
				}
			}
		}
		break;

	case Tool::OrbitCamera:
	case Tool::PanCamera:
	case Tool::ZoomCamera:
	case Tool::RollCamera:
		mEditor.BeginComposite( "Camera" );
		mGestureOpenedComposite = true;   // P1: record what this gesture opened
		isMotionTool = true;
		// Auto-promote the Cameras section in the accordion when the
		// user starts a camera-manipulation gesture.  The previous
		// (pre-accordion) panel auto-flipped to Camera mode whenever
		// one of these tools was active; the accordion's selection-
		// driven panel mode preserves that UX by writing a Camera
		// selection here.  Phase 4b: route through SetSelection
		// (empty name) so mSectionExpanded[Camera] and
		// mSelectionByCategory[Camera] update along with the
		// primary tuple.  Empty-name avoids the cancel-and-park +
		// SetActiveCamera round-trip — the Camera section opens
		// with the active camera as the dropdown's fallback.
		if( mSelectionCategory != Category::Camera ) {
			SetSelection( Category::Camera, String() );
		}
		break;

	case Tool::ScrubTimeline:
		// Direct controls (OnTimeScrubBegin/Scrub/End) handle this.
		break;
	}

	if( isMotionTool )
	{
		mPreviewScale.store( kPreviewScaleMotionStart, std::memory_order_release );
	}
}

void SceneEditController::OnPointerMove( const Point2& px )
{
	if( !mPointerDown.load( std::memory_order_acquire ) ) return;

	// Resume-after-pause snap.  If the pointer has been still long
	// enough that the idle-refinement loop walked the scale toward
	// 1, the very next frame at scale=1 would freeze the viewport
	// during a fast drag — bump the scale back up to the motion
	// start so the during-motion adaptation has headroom to ramp.
	const long long now = NowMs();
	const long long sinceLast = now - mLastEditTimeMs.load( std::memory_order_acquire );
	if( sinceLast > kRefineIdleMs
	 && ( IsCameraMotionTool( mTool ) || IsObjectMotionTool( mTool ) ) )
	{
		const unsigned int s = mPreviewScale.load( std::memory_order_acquire );
		if( s < kPreviewScaleMotionStart )
		{
			mPreviewScale.store( kPreviewScaleMotionStart, std::memory_order_release );
		}
	}

	const Vector2 delta( px.x - mLastPx.x, px.y - mLastPx.y );
	mLastPx = px;

	SceneEdit edit;

	// Object-tool guards: each translate/rotate/scale tool needs a
	// picked object to act on.  `mSelectionName` is a RISE::String
	// (std::vector<char> + trailing NUL); an empty name has size()==1
	// (just the NUL), so the `> 1` check matches the existing Phase-2
	// convention.  Combined with the category check it rejects any
	// non-Object selection, e.g. when the user has the Translate tool
	// armed but selected a camera in the accordion.
	const bool haveObject =
		mSelectionCategory == Category::Object && mSelectionName.size() > 1;

	switch( mTool )
	{
	case Tool::TranslateObject:
		if( !haveObject ) return;
		edit.objectName = mSelectionName;
		if( mGizmoDrag.active ) {
			// Constrained drag — math driven by the captured handle
			// kind / axis.  Conversion from pixel delta to world delta
			// uses the at-drag-start screen-space velocities of the
			// world axes (`mGizmoDrag.axisDir{X,Y}[a]` = pixels per
			// world unit along axis `a`).
			using K = GizmoHandle::Kind;
			Vector3 worldDelta( 0, 0, 0 );
			if( mGizmoDrag.kind == static_cast<int>( K::AxisArrow ) ) {
				const int a = mGizmoDrag.axis;
				if( a < 0 || a > 2 || !mGizmoDrag.axisOk[a] ) return;
				const double adx = mGizmoDrag.axisDirX[a];
				const double ady = mGizmoDrag.axisDirY[a];
				const double mag2 = adx*adx + ady*ady;
				if( mag2 == 0 ) return;
				const double wa = ( static_cast<double>( delta.x ) * adx
				                  + static_cast<double>( delta.y ) * ady ) / mag2;
				worldDelta = ( a == 0 ) ? Vector3( wa, 0, 0 )
				           : ( a == 1 ) ? Vector3( 0, wa, 0 )
				           :              Vector3( 0, 0, wa );
			}
			else if( mGizmoDrag.kind == static_cast<int>( K::AxisPlane ) ) {
				// Plane spanned by the two axes NOT == mGizmoDrag.axis.
				const int a = mGizmoDrag.axis;
				if( a < 0 || a > 2 ) return;
				const int b = ( a + 1 ) % 3;
				const int c = ( a + 2 ) % 3;
				if( !mGizmoDrag.axisOk[b] || !mGizmoDrag.axisOk[c] ) return;
				// Solve 2x2:  [adx_b adx_c] [wb]   [dx]
				//             [ady_b ady_c] [wc] = [dy]
				const double m00 = mGizmoDrag.axisDirX[b];
				const double m01 = mGizmoDrag.axisDirX[c];
				const double m10 = mGizmoDrag.axisDirY[b];
				const double m11 = mGizmoDrag.axisDirY[c];
				const double det = m00*m11 - m01*m10;
				if( det == 0 ) return;
				const double dx = static_cast<double>( delta.x );
				const double dy = static_cast<double>( delta.y );
				const double wb = (  m11 * dx - m01 * dy ) / det;
				const double wc = ( -m10 * dx + m00 * dy ) / det;
				worldDelta = Vector3( 0, 0, 0 );
				if( b == 0 ) worldDelta.x += wb;
				else if( b == 1 ) worldDelta.y += wb;
				else worldDelta.z += wb;
				if( c == 0 ) worldDelta.x += wc;
				else if( c == 1 ) worldDelta.y += wc;
				else worldDelta.z += wc;
			}
			else if( mGizmoDrag.kind == static_cast<int>( K::ScreenCenter ) ) {
				// Minimum-norm 3-DoF solve: ds = A·W where A is the
				// 2x3 matrix of axisDir columns.  Returns the smallest
				// W (in world space) producing the observed pixel
				// delta.  Skipped axes (degenerate at the pivot) get
				// zero rows so the solve naturally excludes them.
				double m00 = 0, m01 = 0, m11 = 0;  // A·A^T (symmetric)
				for( int a = 0; a < 3; ++a ) {
					if( !mGizmoDrag.axisOk[a] ) continue;
					m00 += mGizmoDrag.axisDirX[a] * mGizmoDrag.axisDirX[a];
					m01 += mGizmoDrag.axisDirX[a] * mGizmoDrag.axisDirY[a];
					m11 += mGizmoDrag.axisDirY[a] * mGizmoDrag.axisDirY[a];
				}
				const double det = m00 * m11 - m01 * m01;
				if( det == 0 ) return;
				const double dx = static_cast<double>( delta.x );
				const double dy = static_cast<double>( delta.y );
				// λ = (A·A^T)^{-1} · ds
				const double lx = (  m11 * dx - m01 * dy ) / det;
				const double ly = ( -m01 * dx + m00 * dy ) / det;
				// W = A^T · λ
				worldDelta = Vector3( 0, 0, 0 );
				for( int a = 0; a < 3; ++a ) {
					if( !mGizmoDrag.axisOk[a] ) continue;
					const double w = mGizmoDrag.axisDirX[a] * lx
					               + mGizmoDrag.axisDirY[a] * ly;
					if( a == 0 ) worldDelta.x = w;
					else if( a == 1 ) worldDelta.y = w;
					else worldDelta.z = w;
				}
			}
			else {
				// Unrecognized handle for Translate tool — no-op.
				return;
			}
			edit.op = SceneEdit::TranslateObject;
			edit.v3a = worldDelta;
		} else {
			// No gizmo handle captured: legacy free-drag math.  Same
			// placeholder used by pre-gizmo builds — kept for the
			// "no overlay drawn yet" period BEFORE the platform UIs
			// land their gizmo renderers (B5/B6/B7).  Once those land,
			// a drag that doesn't hit a handle is intentionally a
			// no-op (matches Unity / Maya gizmo conventions).
			edit.op = SceneEdit::TranslateObject;
			edit.v3a = Vector3( delta.x * 0.01, -delta.y * 0.01, 0 );
		}
		break;

	case Tool::RotateObject:
		if( !haveObject ) return;
		edit.objectName = mSelectionName;
		if( mGizmoDrag.active ) {
			using K = GizmoHandle::Kind;
			// Angle of pointer around the projected pivot.  Delta is
			// taken from the previous-frame angle so cumulative drag
			// integrates naturally; wraparound is handled by clamping
			// the delta into (-π, +π].
			const double ax = static_cast<double>( px.x ) - mGizmoDrag.pivotScreenX;
			const double ay = static_cast<double>( px.y ) - mGizmoDrag.pivotScreenY;
			const double angleNow = std::atan2( ay, ax );
			double dAngle = angleNow - mGizmoDrag.prevAngle;
			while( dAngle > 3.14159265358979 )  dAngle -= 6.28318530717958;
			while( dAngle < -3.14159265358979 ) dAngle += 6.28318530717958;
			mGizmoDrag.prevAngle = angleNow;

			Vector3 worldAxis( 0, 0, 0 );
			if( mGizmoDrag.kind == static_cast<int>( K::AxisRing ) ) {
				const int a = mGizmoDrag.axis;
				if( a < 0 || a > 2 ) return;
				worldAxis = ( a == 0 ) ? Vector3( 1, 0, 0 )
				          : ( a == 1 ) ? Vector3( 0, 1, 0 )
				          :              Vector3( 0, 0, 1 );
			}
			else if( mGizmoDrag.kind == static_cast<int>( K::ScreenRing ) ) {
				// View-axis spin: rotate around the camera→pivot
				// direction in world.  Approximates the optical axis
				// (exact when the pivot is dead-centre on screen;
				// usable elsewhere).
				const IScene* scene = mJob.GetScene();
				const ICamera* cam = scene ? scene->GetCamera() : 0;
				if( !cam ) return;
				const Point3 camPos = cam->GetLocation();
				const Vector3 fwd = Vector3Ops::Normalize(
					Vector3Ops::mkVector3( mGizmoDrag.pivotWorld, camPos ) );
				worldAxis = fwd;
			}
			else {
				return;
			}
			edit.op = SceneEdit::RotateObjectArb;
			edit.v3a = worldAxis;
			edit.s   = Scalar( dAngle );
		} else {
			edit.op = SceneEdit::RotateObjectArb;
			edit.v3a = Vector3( 0, 1, 0 );  // y-axis (legacy placeholder)
			edit.s   = delta.x * 0.005;
		}
		break;

	case Tool::ScaleObject:
		if( !haveObject ) return;
		edit.objectName = mSelectionName;
		if( mGizmoDrag.active ) {
			using K = GizmoHandle::Kind;
			// Drag math is unchanged: per-frame the controller
			// computes a per-axis FACTOR (relative to drag-start)
			// from cumulative pointer travel along the axis's
			// screen-space direction, with the factor mapped
			// exponentially — `factor = 2^(travel / 80px)` — so
			// dragging 80 px along an axis doubles its scale,
			// -80 px halves.  Strictly positive (no flip-inside-
			// out at zero).
			//
			// What changed (P1 fix): instead of writing back an
			// ABSOLUTE `SetObjectStretch` derived from the object's
			// initial column magnitudes (which double-applied any
			// existing transform-stack scale on glTF / quaternion /
			// matrix-imported objects), we emit
			// `ScaleObjectFromAnchor` carrying the factor in `v3a`
			// and the anchor matrix in `prevTransform`.  Apply
			// composes those as `anchor · Stretch(factor)`,
			// preserving whatever drag-start state the object had
			// no matter how it was authored.
			const double anchorDx = static_cast<double>( px.x ) - mGizmoDrag.anchorPxX;
			const double anchorDy = static_cast<double>( px.y ) - mGizmoDrag.anchorPxY;
			const double kRefPx = 80.0;
			const double kLog2  = 0.6931471805599453;
			Vector3 factor( 1, 1, 1 );
			if( mGizmoDrag.kind == static_cast<int>( K::AxisScaleHandle ) ) {
				const int a = mGizmoDrag.axis;
				if( a < 0 || a > 2 || !mGizmoDrag.axisOk[a] ) return;
				const double adx = mGizmoDrag.axisDirX[a];
				const double ady = mGizmoDrag.axisDirY[a];
				const double mag = std::sqrt( adx*adx + ady*ady );
				if( !( mag > 0 ) ) return;
				const double pxAlong = ( anchorDx * adx + anchorDy * ady ) / mag;
				const Scalar f = Scalar( std::exp( pxAlong / kRefPx * kLog2 ) );
				if( a == 0 ) factor.x = f;
				else if( a == 1 ) factor.y = f;
				else factor.z = f;
			}
			else if( mGizmoDrag.kind == static_cast<int>( K::UniformScaleCube ) ) {
				const Scalar f = Scalar( std::exp( anchorDx / kRefPx * kLog2 ) );
				factor.x = f;
				factor.y = f;
				factor.z = f;
			}
			else {
				return;
			}
			edit.op = SceneEdit::ScaleObjectFromAnchor;
			edit.v3a = factor;
			// Pre-populate prevTransform with the captured anchor
			// matrix.  `SceneEditor::Apply` skips its usual
			// re-capture for this op so the anchor stays stable
			// across every frame of the drag.
			edit.prevTransform = mGizmoDrag.dragStartMatrix;
			// F6: carry the drag-start component state; RestoreObjectTransform
			// prefers it over the collapsed-matrix fallback.
			if( mGizmoDrag.dragStartStateValid ) {
				edit.prevTransformState = mGizmoDrag.dragStartState;
				edit.hasTransformState  = true;
			}
		} else {
			// Legacy free-drag (no handle hit): per-frame absolute
			// reset — broken for accumulation but kept for non-gizmo
			// drag back-compat.  Gizmo overlay landed B5+ surfaces
			// the handles so users shouldn't normally hit this path.
			edit.op = SceneEdit::SetObjectStretch;
			const Scalar f = 1.0 + delta.y * 0.005;
			edit.v3a = Vector3( f, f, f );
		}
		break;

	case Tool::OrbitCamera:
	case Tool::PanCamera:
	case Tool::ZoomCamera:
		// Pixel deltas; SceneEditor::ApplyCameraOpForward owns the
		// pixel-to-radian / scene-scale conversion.
		edit.op = ( mTool == Tool::OrbitCamera ) ? SceneEdit::OrbitCamera
		      : ( mTool == Tool::PanCamera )    ? SceneEdit::PanCamera
		      :                                    SceneEdit::ZoomCamera;
		edit.v3a = Vector3( delta.x, delta.y, 0 );
		break;

	case Tool::RollCamera:
		// Roll has only one degree of freedom (rotation around the
		// camera→look-at axis).  Use horizontal pixel delta only;
		// SceneEditor reads it from `s` and converts to radians.
		edit.op = SceneEdit::RollCamera;
		edit.s  = delta.x;
		break;

	default:
		return;
	}

	// Object-transform edits mutate scene geometry (transform matrix
	// → world bounding box → top-level BVH leaf pointers), so they
	// MUST land while no render is in flight — otherwise a worker
	// thread mid-traversal sees a freed BVH leaf entry on the next
	// lazy rebuild and crashes with a stale `IObjectPriv*` deref
	// inside `RayElementIntersection`.  Camera ops are exempt
	// because they only mutate camera state (read at GenerateRay
	// time, not during traversal) and don't invalidate the BVH.
	//
	// Pattern matches Undo / Redo / SetProperty: take the mutex,
	// request cancel if a pass is running, wait until the render
	// thread flips `mRendering` to false, then mutate.
	if( IsObjectMotionTool( mTool ) ) {
		// Inline park-and-apply: cancel any in-flight render and wait
		// for it to drain before mutating scene geometry — object
		// transforms invalidate the top-level BVH which a worker
		// mid-traversal can be reading.  Mirrors the pattern Undo /
		// Redo / SetProperty use.
		//
		// Stamps `mLastEditTimeMs` and clears `mPolishState` in
		// addition to bumping `mEditPending` so the render loop's
		// idle-refinement gate sees a fresh edit timestamp (without
		// this it can decide the user has been idle since pointer-
		// down and walk the preview scale back toward full-res mid-
		// drag, freezing the viewport for ~seconds before the next
		// pass at scale=1 starts).  Clearing `mPolishState` also
		// cancels any polish pass queued by the previous gesture's
		// pointer-up — symmetric with `KickRender()` (line 2331).
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		const bool ok = mEditor.Apply( edit );
		if( ok ) {
			mLastEditTimeMs.store( NowMs(), std::memory_order_release );
			mPolishState.store( static_cast<int>( PolishState::None ),
			                    std::memory_order_release );
			mEditPending.store( true, std::memory_order_release );
			lk.unlock();
			mCV.notify_one();
		}
	} else if( mEditor.Apply( edit ) ) {
		KickRender();
	}
}

void SceneEditController::OnPointerUp( const Point2& px )
{
	(void)px;
	if( !mPointerDown.load( std::memory_order_acquire ) ) return;
	mPointerDown.store( false, std::memory_order_release );

	// P1: close the composite the GESTURE opened on pointer-DOWN, regardless of
	// any tool/selection change mid-gesture.  Deciding from the CURRENT tool (the
	// old switch here) could STRAND an open composite -- permanently blocking
	// transactions (IsCompositeOpen) + growing history unbounded -- or double-
	// close when the tool changed away from a motion tool.
	if( mGestureOpenedComposite ) {
		mEditor.EndComposite();
		mGestureOpenedComposite = false;
	}
	// P5 Slice 3 expansion (object transform): a gizmo drag accumulated per-frame transform edits (each a cheap
	// direct mutate under the per-frame park).  Commit the NET transform to the CST as the authoritative `matrix`
	// param now -- ONCE -- under a render-thread park, because a commit RE-DERIVES (on a variant scene it ClearAll's
	// the live scene, which must not race a worker mid-traversal).  No-op when nothing is pending (camera drags,
	// legacy-loaded scenes).
	if( mEditor.HasPendingCstObjectTransforms() ) {
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		mEditor.CommitPendingCstObjectTransforms();
		mEditPending.store( true, std::memory_order_release );
		mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
		lk.unlock();
		mCV.notify_one();
	}
	// Always clear the drag state (incl. the armed-but-no-motion case).
	mGizmoDrag.active = false;

	// Whether to queue the 4-SPP polish pass after the regular 1-SPP final pass.
	const bool wasMotion =
		IsCameraMotionTool( mTool ) || IsObjectMotionTool( mTool );

	// Mouse up — return to full resolution so the user sees a sharp
	// final image.  Kick the render thread so the scale=1 pass runs
	// immediately rather than waiting for the next edit.  Then queue
	// the polish pass: KickRender resets polish state to None, so we
	// store FinalRegularRunning AFTER kicking — RenderLoop's post-pass
	// logic will see this state and chain the 4-SPP polish pass.
	mPreviewScale.store( kPreviewScaleMin, std::memory_order_release );
	KickRender();
	if( wasMotion ) {
		mPolishState.store( static_cast<int>( PolishState::FinalRegularRunning ),
		                    std::memory_order_release );
	}
}

// Direct controls -----------------------------------------------------

void SceneEditController::OnTimeScrubBegin()
{
	// P1: close any composite a PRIOR scrub left open (a missing OnTimeScrubEnd or a
	// repeated Begin) before opening a new one -- otherwise scrubs NEST and one stays
	// open forever (permanent IsCompositeOpen block + the open group defeats history
	// trimming).  Mirrors the pointer-gesture orphan guard in OnPointerDown.
	if( mScrubOpenedComposite ) {
		mEditor.EndComposite();
		mScrubOpenedComposite = false;
	}
	mEditor.BeginComposite( "Scrub" );
	mScrubOpenedComposite = true;
	mPreviewScale.store( kPreviewScaleMotionStart, std::memory_order_release );
}

void SceneEditController::OnTimeScrub( Scalar t )
{
	// Time-scrub mutations cascade through the animator's observer
	// chain — a keyframed DisplacedGeometry, for example, destroys
	// its TriangleMeshGeometryIndexed (and the BSP tree inside) and
	// rebuilds at the new time.  If the render thread is mid-
	// IntersectRay on that BSP when the destruction lands, we get a
	// straight UAF (matches the crash report: main-thread
	// ~BSPTreeNodeSAH recursion racing the render thread's
	// IntersectRay through the same BSP).
	//
	// Pattern: hold mMutex across the whole sequence so the render
	// thread can neither be running while we mutate nor begin a new
	// pass between our mutation and the kick.  Steps:
	//   1. Trip cancel so an in-flight pass returns from RasterizeScene.
	//   2. cv.wait until that pass finishes (mRendering goes false
	//      under mMutex).  The wait releases mMutex while parked,
	//      lets the render thread acquire it briefly to flip the
	//      flag, and reacquires it before predicate-passing.
	//   3. STILL HOLDING mMutex, mutate the scene.  The render thread
	//      can't acquire mMutex (it needs it to set rendering=true
	//      for the next pass) so it can't begin a new pass while we
	//      mutate.
	//   4. Set editPending and drop the lock; the render thread's
	//      cv.wait predicate sees it and runs a fresh pass on the
	//      mutated scene.
	//
	// Cancel latency is bounded by the rasterizer's tile-boundary
	// cancel check (~1ms on the cheap pixelpel preview), so the
	// main thread's wait here is short.
	//
	// Pan / Zoom / Orbit mutations don't go through this path: they
	// touch only the camera (cheap writes) and at worst invalidate
	// the spatial structure (deferred rebuild on the next render).
	// Time-scrub is the only mutation that can destroy live geometry
	// while the render thread is reading it.
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) )
	{
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	// Holding mMutex across mEditor.Apply: the render thread is
	// parked on its own cv.wait (or blocked acquiring mMutex to
	// transition rendering=true) — neither path can read scene
	// state while we mutate.
	SceneEdit edit;
	edit.op = SceneEdit::SetSceneTime;
	edit.s  = t;
	const bool ok = mEditor.Apply( edit );

	// Inline the KickRender effect under the held lock — store
	// editPending, then notify after dropping the lock.  Keeping
	// this inside the same critical section means the render
	// thread sees the mutated scene and the editPending flag
	// atomically.  Mirror KickRender's polish-state cancellation
	// too: a polish pass queued by a prior OnPointerUp would
	// otherwise run AFTER the scrub's mutation, applying the
	// 4-SPP / max-recursion-2 polish to a scene that's at a
	// different time than when the polish was queued.
	if( ok )
	{
		mPolishState.store( static_cast<int>( PolishState::None ),
		                    std::memory_order_release );
		mEditPending.store( true, std::memory_order_release );
		mLastEditTimeMs.store( NowMs(), std::memory_order_release );
	}
	lk.unlock();

	if( ok )
	{
		mCV.notify_one();
	}
}

void SceneEditController::OnTimeScrubEnd()
{
	// P1: close only the composite THIS scrub opened -- a stray End with no open scrub
	// must not under-flow the composite depth.
	if( mScrubOpenedComposite ) {
		mEditor.EndComposite();
		mScrubOpenedComposite = false;
	}
	mPreviewScale.store( kPreviewScaleMin, std::memory_order_release );
	KickRender();
}

void SceneEditController::BeginPropertyScrub()
{
	// Set a SEPARATE flag from mPointerDown so a panel scrub
	// doesn't clobber an in-flight viewport drag's pointer-down
	// state.  The during-motion adaptation in DoOneRenderPass and
	// the resume-snap in OnPointerMove read either flag (logical
	// OR) so the same preview-scale machinery fires for either
	// gesture.  Without this separation, EndPropertyScrub flipping
	// mPointerDown=false would silently freeze a concurrent orbit
	// drag — the next OnPointerMove would early-return because
	// mPointerDown is false, even though the user's mouse is still
	// down on the viewport.
	mScrubInProgress.store( true, std::memory_order_release );
	mPreviewScale.store( kPreviewScaleMotionStart, std::memory_order_release );
	mLastEditTimeMs.store( NowMs(), std::memory_order_release );
}

void SceneEditController::EndPropertyScrub()
{
	// Restore full resolution and queue one final render so the
	// post-scrub frame is sharp.  The render-loop's idle-refinement
	// pass will further refine on top.  Note: the watchdog in the
	// render thread will also clear mScrubInProgress if this End
	// gets dropped (gesture interrupt, view torn down mid-drag) —
	// so the worst case is a brief preview-quality dip, not a
	// permanent stuck-low-quality state.
	mScrubInProgress.store( false, std::memory_order_release );
	mPreviewScale.store( kPreviewScaleMin, std::memory_order_release );
	KickRender();
}

namespace {

// Verify the controller's (category, name) selection still names an
// entity that exists in the live scene.  Returns true if the tuple
// is still valid; false if the referenced entity has gone away
// (Undo of AddCamera, future RemoveObject op, etc.).  Used after
// Undo/Redo to detect when the panel needs to drop a stale entity
// pointer.
bool SelectionStillResolves( const IJobPriv& job,
	SceneEditController::Category cat, const String& name )
{
	if( name.size() <= 1 ) return true;   // empty name = section-only, no entity to check
	const IScene* scene = const_cast<IJobPriv&>( job ).GetScene();
	if( !scene ) return true;             // skeleton mode — nothing to validate
	using Cat = SceneEditController::Category;
	switch( cat ) {
	case Cat::Camera: {
		const ICameraManager* m = scene->GetCameras();
		return m && m->GetItem( name.c_str() ) != 0;
	}
	case Cat::Object: {
		const IObjectManager* m = scene->GetObjects();
		return m && const_cast<IObjectManager*>( m )->GetItem( name.c_str() ) != 0;
	}
	case Cat::Light: {
		const ILightManager* m = scene->GetLights();
		return m && const_cast<ILightManager*>( m )->GetItem( name.c_str() ) != 0;
	}
	case Cat::Material: {
		const IMaterialManager* m = const_cast<IJobPriv&>( job ).GetMaterials();
		return m && const_cast<IMaterialManager*>( m )->GetItem( name.c_str() ) != 0;
	}
	case Cat::Rasterizer:
	case Cat::Film:
	case Cat::None:
	default:
		return true;
	}
}

}  // namespace

void SceneEditController::DropStaleSelection_()
{
	// If the (category,name) selection points at an entity that no longer resolves
	// (removed externally, or by an undone AddCamera), reset to category-only so the
	// panel doesn't render a header for a gone entity.  Don't touch mSectionExpanded.
	if( !SelectionStillResolves( mJob, mSelectionCategory, mSelectionName ) ) {
		mSelectionName = String();
		const int idx = static_cast<int>( mSelectionCategory );
		if( idx > 0 && idx < kNumCategories ) {
			mSelectionByCategory[idx] = String();
		}
	}
}

void SceneEditController::Undo()
{
	// Latent-guard (re-review): refuse user Undo while a transaction is open --
	// undoing PAST the baseline would break RollbackTransaction's revert
	// guarantee (and ClearRedo would then drop a pre-baseline edit).  The
	// rollback path uses mEditor.Undo() directly, bypassing this guard.
	if( mTxnOpen ) {
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController::Undo refused: a transaction is open; use Rollback/EndTransaction." );
		return;
	}
	// Cancel-and-park around Undo: many ops mutate state the render
	// thread reads per-pixel (camera pointers via AddCamera/RemoveCamera,
	// light keyframe state, material/shader pointers via the property
	// panel).  Without parking, an Undo that fires while a pass is in
	// flight can free a pointer the worker is mid-deref.  Forward
	// Apply paths (Light/Object/Film/Rasterizer branches in
	// SetProperty + the CloneActiveCamera path) already park; the
	// inverse path needs the same protection.
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	const unsigned int beforeUndoDepth = mEditor.History().UndoDepth();
	const bool ok = mEditor.Undo();
	// P1-#2 follow-up: mEditor.Undo() returns false on a PARTIAL composite revert,
	// but the scene WAS mutated (the undo stack advanced) so we must still refresh.
	// "did work" = succeeded OR the undo stack changed; only an empty-stack no-op skips.
	const bool didWork = ok || ( mEditor.History().UndoDepth() != beforeUndoDepth );
	// P5 Slice 3 expansion (object transform): an undone transform noted its object -> commit the RESTORED matrix
	// to the CST under this park so undo stays Document-consistent (else a later D2 would re-apply the dragged pose).
	if( mEditor.HasPendingCstObjectTransforms() ) mEditor.CommitPendingCstObjectTransforms();
	// P1: re-validate the selection UNCONDITIONALLY -- a stale selection (selected
	// entity gone, e.g. removed externally) must clear on ANY undo attempt, incl. an
	// atomic no-op composite undo (didWork == false -> the gated refresh is skipped).
	DropStaleSelection_();
	if( didWork ) {
		// Re-derive auto-synced Material / Medium section selections
		// from the (potentially restored) Object binding.  Forward
		// path updates these in SetProperty after the Apply succeeds;
		// undo restores the underlying object state but doesn't
		// touch the per-category panel selection, so without this
		// resync the Material/Media sections keep showing the post-
		// edit binding's NAME while the object now has the pre-edit
		// binding's content.  Cheap to re-look-up unconditionally —
		// it's two reverse-lookups against the registered manager.
		ResyncObjectBoundSections_();
		mEditPending.store( true, std::memory_order_release );
		// Bump epoch — Undo of AddCamera removes an entity, which
		// changes the Camera category's entity list.  Cheap to bump
		// unconditionally; covers future structural-undo ops too.
		mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
		lk.unlock();
		mCV.notify_one();
	}
}

void SceneEditController::Redo()
{
	if( mTxnOpen ) {
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController::Redo refused: a transaction is open." );
		return;
	}
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	const unsigned int beforeRedoDepth = mEditor.History().RedoDepth();
	const bool ok = mEditor.Redo();
	// P1-#2 follow-up: see Undo -- a partial composite redo still mutated the scene.
	const bool didWork = ok || ( mEditor.History().RedoDepth() != beforeRedoDepth );
	// P5 Slice 3 expansion (object transform): a redone transform noted its object -> commit the re-applied matrix
	// to the CST under this park (symmetric with Undo).
	if( mEditor.HasPendingCstObjectTransforms() ) mEditor.CommitPendingCstObjectTransforms();
	DropStaleSelection_();   // P1: see Undo -- re-validate selection on any redo attempt
	if( didWork ) {
		// Re-derive auto-synced Material / Medium section selections
		// from the (potentially re-applied) Object binding — same
		// rationale as Undo's resync.
		ResyncObjectBoundSections_();
		mEditPending.store( true, std::memory_order_release );
		mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
		lk.unlock();
		mCV.notify_one();
	}
}

Scalar SceneEditController::LastSceneTime() const
{
	return mEditor.LastSceneTime();
}

// Production render ---------------------------------------------------

bool SceneEditController::RequestProductionRender()
{
	const bool wasRunning = IsRunning();
	if( wasRunning )
	{
		Stop();
	}

	IRasterizer* prod = mJob.GetRasterizer();
	if( !prod )
	{
		// Without a production rasterizer there's nothing to do; the
		// caller should ensure one is configured by the time they
		// click "Render".  We restart interactive and report failure.
		if( wasRunning ) Start();
		return false;
	}

	const IScene* scene = mJob.GetScene();
	if( scene )
	{
		// Run a FULL SetSceneTime(t) at the most recently scrubbed
		// time before invoking the production rasterizer.  The
		// preview path (called from OnTimeScrub) uses
		// SetSceneTimeForPreview, which deliberately skips photon-map
		// regeneration to keep scrubbing responsive.  Without this
		// full pass, hitting Render after scrubbing produces a frame
		// at the scrubbed time but with caustics from the scene's
		// initial time — visibly stale for any photon-mapped scene.
		// SetSceneTime triggers Regenerate on every populated photon
		// map; for non-photon scenes it's effectively a cheap reset
		// of the per-object runtime data and is harmless.
		scene->SetSceneTime( mEditor.LastSceneTime() );

		// PrepareForRendering rebuilds spatial structure if invalidated.
		scene->GetObjects()->PrepareForRendering();
	}

	bool ok = false;
	if( scene )
	{
		// Running this synchronously on the calling (UI) thread is
		// the contract: production renders are long, the platform UI
		// shows a modal during the call, and re-enabling the
		// interactive controls before the production render finishes
		// would race with the production rasterizer.
		prod->RasterizeScene( *scene, /*pRect*/0, /*seq*/0 );
		ok = true;
	}

	if( wasRunning ) Start();
	return ok;
}

// Transactional rollback (inverse-edit; NOT snapshot/restore) ---------
//
// RollbackTransaction reverts the transaction's edits by applying their
// INVERSES down to the BeginTransaction undo depth (driving
// SceneEditor::Undo), then clears the redo stack.  It does NOT use the
// deep-clone snapshot/restore primitive (see the EXPERIMENTAL note on
// Scene::CreateSnapshot in Scene.h and the header doc for why).

SceneEditController::EditorStateSnapshot SceneEditController::CaptureEditorState() const
{
	EditorStateSnapshot st;
	st.historyMarker     = mEditor.History().NextSeq();
	st.dirty             = mEditor.CaptureDirtyState();
	st.selectionCategory = mSelectionCategory;
	st.selectionName     = mSelectionName;
	st.selectionByCategory.assign( mSelectionByCategory, mSelectionByCategory + kNumCategories );
	st.sectionExpanded.assign( mSectionExpanded, mSectionExpanded + kNumCategories );
	return st;
}

void SceneEditController::RestoreEditorState( const EditorStateSnapshot& st, bool restoreDirty )
{
	// P1-#1: on a FULL rollback restore the dirty channels to the pre-transaction
	// baseline (fires the dirty-changed listener); on a PARTIAL rollback leave the
	// residual-dirty state so the Save button stays lit.  Selection is ALWAYS
	// restored.  NOTE (P1-#3 review): redo-stack restore is NOT done here -- this is
	// a general capture/restore primitive (also called directly by tests) and must
	// not carry a hidden history side effect; RollbackTransaction restores the redo
	// stack itself, explicitly, only on a full revert.
	if( restoreDirty ) {
		mEditor.RestoreDirtyState( st.dirty );
	}
	if( st.selectionByCategory.size() == static_cast<size_t>( kNumCategories ) ) {
		for( int i = 0; i < kNumCategories; ++i ) {
			mSelectionByCategory[i] = st.selectionByCategory[i];
			mSectionExpanded[i]     = st.sectionExpanded[i];
		}
	}
	mSelectionCategory = st.selectionCategory;
	mSelectionName     = st.selectionName;
	if( !SelectionStillResolves( mJob, mSelectionCategory, mSelectionName ) ) {
		mSelectionName = String();
		const int sidx = static_cast<int>( mSelectionCategory );
		if( sidx > 0 && sidx < kNumCategories ) { mSelectionByCategory[sidx] = String(); }
	}
	ResyncObjectBoundSections_();
}

bool SceneEditController::BeginTransaction()
{
	// Inverse-edit rollback works through the SceneEditor for ANY scene
	// the editor can mutate, so there is no concrete-Scene precondition
	// and no snapshot to capture.  Recording the current undo depth is
	// the entire setup: RollbackTransaction later applies the inverse
	// edits down to exactly this depth.
	//
	// No cancel-and-park here: Begin touches no scene state the render
	// thread reads (it only reads the editor's history depth on the UI
	// thread).  Begin/Rollback/End are all UI-thread calls, so they
	// cannot race each other.
	//
	// Nesting is unsupported: a Begin-over-Begin REPLACES the baseline
	// (the new call wins).  The edits made under the prior (now-dropped)
	// baseline simply become un-bracketed — they remain undoable through
	// the normal Undo path, they are just no longer part of a rollback
	// unit.  This matches the single-gesture model.
	// Re-review finding A: refuse to open a transaction while the editor
	// is mid-composite (BeginComposite without EndComposite).  The baseline
	// depth would land INSIDE the composite group, and a single composite
	// Undo() during rollback walks the whole group back PAST the baseline,
	// consuming the pre-baseline CompositeBegin and corrupting the
	// surrounding undo history.  A transaction must bracket WHOLE edits.
	if( mEditor.IsCompositeOpen() )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::BeginTransaction refused: a SceneEditor "
			"composite is open; close it before opening a transaction." );
		return false;
	}
	mTxnOpen              = true;
	mTxnBaseline = CaptureEditorState();   // H1: one owned baseline
	mEditor.History().SnapshotRedoForRollback();   // P1-#3: so a full rollback restores the pre-transaction redo stack
	mEditor.History().SnapshotUndoForRollback();   // P1: ...and the pre-transaction undo stack (cap-evicted records)
	return true;
}

bool SceneEditController::IsTransactionOpen() const
{
	return mTxnOpen;
}

bool SceneEditController::RollbackTransaction()
{
	if( !mTxnOpen ) return false;

	// Cancel-and-park around the inverse-edit applies — SceneEditor::Undo
	// mutates live scene state the render thread reads per-pixel (object
	// transforms, light keyframe state, material/shader pointers, camera
	// pose).  Same pattern as Undo / SetProperty: trip the rasterizer
	// cancel flag, wait for the in-flight pass to drain under mMutex,
	// revert with the lock held, then notify.
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	// Revert by applying the inverse edits down to the transaction
	// baseline depth.  Each mEditor.Undo() reverts one logical unit on
	// the SAME live instances the forward edits touched: a CLOSED
	// composite reverts as a group in a single call (the composite walk
	// in SceneEditor::Undo), while individual edits and an unmatched
	// CompositeBegin (a rollback fired mid-gesture) each revert one
	// record at a time.  Light edits + emissive-material rebinds bump
	// the scene light-topology generation inside Undo, so a reused
	// RayCaster rebuilds its LightSampler.  Undo moves each reverted
	// record onto the redo stack; we clear that residue below.
	//
	// Loop guards (honest partial-rollback): stop if Undo reports no
	// progress (empty stack, or a target entity removed out from under
	// an edit) so we never spin, and treat a non-empty remaining gap as
	// a partial rollback the caller is told about via the return value.
	// The depth can also start BELOW baseline if the gesture exceeded
	// the EditHistory bound and older records were trimmed — that too is
	// an honest partial.
	bool fullyReverted = true;
	unsigned long long topSeq = 0;
	// F2: undo while the TOP edit's seq is at/above the transaction marker.
	// Seq is monotonic + trim-immune, so this is correct even when the
	// 1024-cap pinned UndoDepth (the old depth-only baseline silently
	// no-op'd at the cap).  Each Undo reverts one logical unit (a closed
	// composite reverts as a group).
	while( mEditor.History().PeekUndoSeq( topSeq ) && topSeq >= mTxnBaseline.historyMarker )
	{
		if( !mEditor.Undo() ) { fullyReverted = false; break; }   // target gone -> honest partial
	}
	// P5 Slice 3 expansion (object transform): the Undo loop reverted the live objects, NOTING any transform-
	// touched object into the editor's pending set.  Commit them now (lk held -> parked, the re-derive can't race
	// a worker) so the retained CST matches the REVERTED live transforms -- a mid-scrub SetProperty(Object) may
	// have committed the scrubbed matrix to the CST, and without this re-sync the Document would keep the
	// rejected pose (a later D2 would then re-apply it).  Also drains the set so no stale snapshot leaks past the
	// rollback.
	if( mEditor.HasPendingCstObjectTransforms() ) mEditor.CommitPendingCstObjectTransforms();
	// F2: if the cap trimmed a transaction edit (seq >= marker) off the
	// front, the revert could not be complete -- report it honestly.
	if( mEditor.History().DidTrim() && mEditor.History().MaxTrimmedSeq() >= mTxnBaseline.historyMarker ) {
		fullyReverted = false;
	}

	// A rolled-back gesture must NOT be redoable: drop the redo residue
	// the inverse-applies left behind.  (The undo stack is already back
	// at — or as close as reachable to — the baseline depth.)
	mEditor.History().ClearRedo();

	// The composite the gesture may have opened is now meaningless —
	// reset the editor's composite depth so a later EndComposite (a
	// tool cleanup path) doesn't push an orphan CompositeEnd.  A
	// rollback can fire mid-gesture (before the matching EndComposite),
	// so force the depth back to a clean zero.
	mEditor.ForceCompositeDepthZero();

	// P1-#6: a rollback can fire mid-gesture.  Reset the controller's
	// interaction state too -- otherwise the next pointer move resumes the
	// rejected gesture outside the (now-closed) transaction.
	mPointerDown.store( false, std::memory_order_release );
	mGizmoDrag.active = false;
	mScrubInProgress.store( false, std::memory_order_release );
	mGestureOpenedComposite = false;   // P1: a mid-gesture rollback also clears the open-composite flag
	mScrubOpenedComposite   = false;   // P1: ...and the scrub-composite flag

	// F7: restore the dirty channels + selection to the pre-transaction
	// baseline so a fully reverted document doesn't keep showing unsaved
	// changes (Undo RE-MARKS dirty; created entities are never un-marked),
	// then re-run the selection/panel resync the controller's Undo does.
	RestoreEditorState( mTxnBaseline, fullyReverted );   // H1 + P1-#1: dirty only on full revert; selection always
	// P1-#3: a FULL rollback restores the pre-transaction redo stack the first edit
	// cleared; a PARTIAL leaves it empty (ClearRedo above) since those redo entries
	// are no longer coherent with the residual state.
	if( fullyReverted ) {
		mEditor.History().RestoreRedoFromSnapshot();
		mEditor.History().RestoreUndoFromSnapshot();   // P1: restore any pre-txn undo record evicted at the cap
	}

	// Close the transaction.
	mTxnOpen              = false;
	mEditor.History().ClearRollbackSnapshots();   // P1 review: free the rollback snapshots now the txn is closed

	// Re-render the reverted state.  Inline the KickRender effect under
	// the held lock (store editPending, notify after unlock) so the
	// render thread sees the reverted scene + the pending flag together,
	// matching the OnTimeScrub / object-drag park-and-apply idiom.
	mPolishState.store( static_cast<int>( PolishState::None ),
									  std::memory_order_release );
	mEditPending.store( true, std::memory_order_release );
	mLastEditTimeMs.store( NowMs(), std::memory_order_release );
	lk.unlock();
	mCV.notify_one();

	// A rollback that reverted structural edits (e.g. undo of AddCamera)
	// changes the category entity lists; bump the epoch so platform UIs
	// re-pull (cheap, and covers the conflict/AI-reject cases).
	mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
	return fullyReverted;
}

bool SceneEditController::EndTransaction()
{
	if( !mTxnOpen ) return false;
	// Commit is record-only: the edits were applied + recorded live
	// during the transaction (the shipping flow), so committing just
	// closes the transaction.  No re-apply, no revert; the redo stack is
	// left intact so normal Undo/Redo of the committed edits still works.
	mTxnOpen              = false;
	mEditor.History().ClearRollbackSnapshots();   // P1 review: free the rollback snapshots now the txn is closed
	return true;
}

// Selection -----------------------------------------------------------

SceneEditController::Category SceneEditController::GetSelectionCategory() const
{
	return mSelectionCategory;
}

String SceneEditController::GetSelectionName() const
{
	return mSelectionName;
}

String SceneEditController::SelectedObjectName() const
{
	// Legacy accessor — kept for the pointer-event handlers that
	// already used it as a "do I have an object to operate on" guard.
	// Returns empty unless the active selection is in the Objects
	// category, so a Camera or Rasterizer selection doesn't accidentally
	// satisfy the object-tool check.
	if( mSelectionCategory != Category::Object ) return String();
	return mSelectionName;
}

unsigned int SceneEditController::SceneEpoch() const
{
	return mSceneEpoch.load( std::memory_order_acquire );
}

namespace {

// Walk a manager's enumerated names into a vector.  Used by both the
// CategoryEntityCount/Name accessors below and by RefreshProperties'
// camera-picker preset list.  Tiny helper, but it stops the
// EnumerateItemNames-then-callback dance from leaking into every
// caller.
class CollectNamesCallback : public IEnumCallback<const char*>
{
public:
	std::vector<String> names;
	bool operator()( const char* const& name ) override {
		if( name ) names.emplace_back( name );
		return true;
	}
};

}  // namespace

unsigned int SceneEditController::CategoryEntityCount( Category cat ) const
{
	const IScene* scene = mJob.GetScene();
	if( !scene ) return 0;
	switch( cat ) {
	case Category::Camera: {
		const ICameraManager* m = scene->GetCameras();
		return m ? m->getItemCount() : 0;
	}
	case Category::Rasterizer: {
		// Eagerly enumerate the registry by walking the available types
		// the job exposes.  Job::GetRasterizerTypeCount is the source
		// of truth — even types that haven't been instantiated yet are
		// listed (the platform UI shows them; switching to one not yet
		// in the registry is rejected at SetSelection time in phase 1).
		return mJob.GetRasterizerTypeCount();
	}
	case Category::Object: {
		const IObjectManager* m = scene->GetObjects();
		return m ? m->getItemCount() : 0;
	}
	case Category::Light: {
		const ILightManager* m = scene->GetLights();
		return m ? m->getItemCount() : 0;
	}
	case Category::Film: {
		// A scene has exactly one Film, but the accordion's dropdown
		// is used as a quick-pick preset selector: each entry is a
		// (width, height) the user can apply with one click.  The
		// list is fixed (see FilmIntrospection::kFilmPresets) — 480x270
		// through 4K UHD.  Hand-tuning to a non-preset resolution is
		// done via the width / height property rows below the dropdown.
		return scene->GetFilm() ? FilmIntrospection::PresetCount() : 0;
	}
	case Category::Material: {
		// Read-only Phase 2 surface — list every registered material
		// so the user can pick one and see its type + (Lambertian
		// only) reflectance painter binding.
		const IMaterialManager* m = mJob.GetMaterials();
		return m ? m->getItemCount() : 0;
	}
	case Category::Medium: {
		// Media live in `Job::mediaMap` rather than a real manager.
		// Enumerate via `IJob::EnumerateMediumNames` and count.
		struct Count : public IEnumCallback<const char*> {
			unsigned int n = 0;
			bool operator()( const char* const& name ) override {
				if( name ) ++n;
				return true;
			}
		};
		Count cb;
		mJob.EnumerateMediumNames( cb );
		return cb.n;
	}
	case Category::Animation: {
		return mJob.GetAnimationCount();
	}
	case Category::SceneVariant: {
		// The variant SWITCH needs the retained CST Document to re-derive; a legacy-loaded scene (no Document) or
		// a scene with no declared variants offers nothing to switch -> 0 rows (no pickable entry that would
		// silently no-op).  Otherwise: the declared variants + the synthetic "(base)" at index 0.
		if( !mJob.HasRetainedCstDocument() || mJob.GetSceneVariantCount() == 0 ) return 0;
		return mJob.GetSceneVariantCount() + 1u;
	}
	case Category::None:
	default:
		return 0;
	}
}

String SceneEditController::CategoryEntityName( Category cat, unsigned int idx ) const
{
	const IScene* scene = mJob.GetScene();
	if( !scene ) return String();
	switch( cat ) {
	case Category::Camera: {
		const ICameraManager* m = scene->GetCameras();
		if( !m ) return String();
		CollectNamesCallback cb;
		m->EnumerateItemNames( cb );
		if( idx >= cb.names.size() ) return String();
		return cb.names[idx];
	}
	case Category::Rasterizer: {
		// Job's per-index getter returns std::string (the IJob API
		// uses std::string for rasterizer type names since they're
		// fixed strings).  Convert into RISE::String here.
		const std::string s = mJob.GetRasterizerTypeName( idx );
		return String( s.c_str() );
	}
	case Category::Object: {
		const IObjectManager* m = scene->GetObjects();
		if( !m ) return String();
		CollectNamesCallback cb;
		m->EnumerateItemNames( cb );
		if( idx >= cb.names.size() ) return String();
		return cb.names[idx];
	}
	case Category::Light: {
		const ILightManager* m = scene->GetLights();
		if( !m ) return String();
		CollectNamesCallback cb;
		m->EnumerateItemNames( cb );
		if( idx >= cb.names.size() ) return String();
		return cb.names[idx];
	}
	case Category::Film: {
		// Quick-pick preset by index — see FilmIntrospection.cpp's
		// kFilmPresets for the canonical list of labels and dims.
		if( !scene->GetFilm() ) return String();
		const FilmPreset* p = FilmIntrospection::PresetAt( idx );
		return p ? String( p->label ) : String();
	}
	case Category::Material: {
		const IMaterialManager* m = mJob.GetMaterials();
		if( !m ) return String();
		CollectNamesCallback cb;
		const_cast<IMaterialManager*>( m )->EnumerateItemNames( cb );
		if( idx >= cb.names.size() ) return String();
		return cb.names[idx];
	}
	case Category::Medium: {
		CollectNamesCallback cb;
		mJob.EnumerateMediumNames( cb );
		if( idx >= cb.names.size() ) return String();
		return cb.names[idx];
	}
	case Category::Animation: {
		char buf[256] = { 0 };
		if( !mJob.GetAnimationName( idx, buf, sizeof(buf) ) ) return String();
		return String( buf );
	}
	case Category::SceneVariant: {
		if( idx == 0 ) return String( "(base)" );   // index 0 = the no-variant default
		char buf[256] = { 0 };
		if( !mJob.GetSceneVariantName( idx - 1, buf, sizeof(buf) ) ) return String();
		return String( buf );
	}
	case Category::None:
	default:
		return String();
	}
}

String SceneEditController::CategoryActiveName( Category cat ) const
{
	switch( cat ) {
	case Category::Camera: {
		const IScene* scene = mJob.GetScene();
		return scene ? scene->GetActiveCameraName() : String();
	}
	case Category::Rasterizer: {
		const std::string s = mJob.GetActiveRasterizerName();
		return String( s.c_str() );
	}
	case Category::Film: {
		// If the current Film matches one of the canonical preset dims
		// exactly, return that preset's label so the accordion's
		// dropdown highlights it.  Otherwise return empty — common when
		// the Film was screen-fit by ScaleFilmToFit, where the resulting
		// dims don't usually land on a preset (e.g. 800 x 450 isn't a
		// preset).  Empty leaves the dropdown unselected, signalling
		// "custom size" — the user sees the actual dims in the property
		// rows below.
		const IScene* scene = mJob.GetScene();
		if( !scene ) return String();
		const IFilm* film = scene->GetFilm();
		if( !film ) return String();
		const int idx = FilmIntrospection::FindPresetByDims(
			film->GetWidth(), film->GetHeight() );
		if( idx < 0 ) return String();
		const FilmPreset* p = FilmIntrospection::PresetAt(
			static_cast<unsigned int>( idx ) );
		return p ? String( p->label ) : String();
	}
	case Category::Animation: {
		char buf[256] = { 0 };
		if( !mJob.GetActiveAnimationName( buf, sizeof(buf) ) ) return String();
		return String( buf );
	}
	case Category::SceneVariant: {
		char buf[256] = { 0 };
		mJob.GetActiveSceneVariant( buf, sizeof(buf) );
		return buf[0] ? String( buf ) : String( "(base)" );   // empty active => the base default
	}
	case Category::Object:
	case Category::Light:
	case Category::Material:
	case Category::Medium:
	case Category::None:
	default:
		return String();
	}
}

namespace {

// Look up the material name currently bound to the named object.
// Used by `SetSelection` to auto-fill the Materials section when an
// Object is picked.  Returns empty when the object isn't registered,
// has no material bound, or its material isn't registered with the
// manager under a recoverable name (the latter is degenerate; would
// require a programmatic AssignMaterial with an unregistered IMaterial*).
String FindObjectMaterialName( const IJobPriv& job, const String& objName )
{
	if( objName.size() <= 1 ) return String();
	const IScene* scene = const_cast<IJobPriv&>( job ).GetScene();
	if( !scene ) return String();
	const IObjectManager* objs = scene->GetObjects();
	if( !objs ) return String();
	const IObject* obj = const_cast<IObjectManager*>( objs )->GetItem( objName.c_str() );
	if( !obj ) return String();
	const IMaterial* mat = obj->GetMaterial();
	if( !mat ) return String();
	IMaterialManager* mats = const_cast<IJobPriv&>( job ).GetMaterials();
	if( !mats ) return String();
	struct Cb : public IEnumCallback<const char*> {
		IMaterialManager*    mgr;
		const IMaterial*     target;
		String               found;
		bool operator()( const char* const& name ) override {
			if( mgr->GetItem( name ) == target ) { found = String( name ); return false; }
			return true;
		}
	};
	Cb cb;
	cb.mgr    = mats;
	cb.target = mat;
	mats->EnumerateItemNames( cb );
	return cb.found;
}

// Look up the interior medium name currently bound to the named
// object.  Used by `SetSelection` to auto-fill the Media section
// when an Object is picked, mirroring the Material auto-sync.
// Returns empty when the object has no interior medium bound, or
// the medium isn't registered under a recoverable name.
String FindObjectInteriorMediumName( const IJobPriv& job, const String& objName )
{
	if( objName.size() <= 1 ) return String();
	const IScene* scene = const_cast<IJobPriv&>( job ).GetScene();
	if( !scene ) return String();
	const IObjectManager* objs = scene->GetObjects();
	if( !objs ) return String();
	const IObject* obj = const_cast<IObjectManager*>( objs )->GetItem( objName.c_str() );
	if( !obj ) return String();
	const IMedium* med = obj->GetInteriorMedium();
	if( !med ) return String();
	// Media don't have a IManager<T> — reverse-lookup via the IJob
	// EnumerateMediumNames / GetMedium pair (same shape as
	// FindMediumName in SceneEditor.cpp).
	struct Cb : public IEnumCallback<const char*> {
		const IJobPriv* job;
		const IMedium*  target;
		String          found;
		bool operator()( const char* const& name ) override {
			if( job->GetMedium( name ) == target ) { found = String( name ); return false; }
			return true;
		}
	};
	Cb cb;
	cb.job    = &job;
	cb.target = med;
	job.EnumerateMediumNames( cb );
	return cb.found;
}

}  // namespace

void SceneEditController::ResyncObjectBoundSections_()
{
	// Forward path: SetProperty's Object branch pins the auto-synced
	// Material / Medium section names after a successful Apply.
	// Undo / Redo restore the underlying object binding but don't
	// touch the per-category panel selection — so without this
	// helper, the Material / Media sections keep showing the post-
	// edit binding's NAME while the object now has the pre-edit
	// binding's content (or vice versa for Redo).
	//
	// The fix: re-read the bound material + interior medium from
	// the currently-pinned Object and update the per-cat selection
	// state to match.  No-op if no Object is pinned.
	const int objIdx = static_cast<int>( Category::Object );
	if( objIdx < 0 || objIdx >= kNumCategories ) return;
	const String objName = mSelectionByCategory[ objIdx ];
	if( objName.size() <= 1 ) return;

	const int matIdx = static_cast<int>( Category::Material );
	const int medIdx = static_cast<int>( Category::Medium );
	mSelectionByCategory[ matIdx ] = FindObjectMaterialName( mJob, objName );
	mSelectionByCategory[ medIdx ] = FindObjectInteriorMediumName( mJob, objName );
}

String SceneEditController::GetSelectionNameForCategory( Category cat ) const
{
	const int i = static_cast<int>( cat );
	if( i < 0 || i >= kNumCategories ) return String();
	return mSelectionByCategory[i];
}

bool SceneEditController::IsSectionExpanded( Category cat ) const
{
	const int i = static_cast<int>( cat );
	if( i <= 0 || i >= kNumCategories ) return false;   // None (=0) is never "expanded"
	return mSectionExpanded[i];
}

void SceneEditController::CollapseSection( Category cat )
{
	const int i = static_cast<int>( cat );
	if( i <= 0 || i >= kNumCategories ) return;   // None is a no-op
	mSectionExpanded[i] = false;
	mSelectionByCategory[i] = String();
	// If the collapsed section was the primary, fall back to any
	// other still-expanded section with a non-empty selection; if
	// no remaining candidate, drop to None.  This keeps the
	// "primary tuple" coherent for callers that use GetSelectionCategory.
	if( mSelectionCategory == cat ) {
		mSelectionCategory = Category::None;
		mSelectionName     = String();
		for( int j = 1; j < kNumCategories; ++j ) {
			if( mSectionExpanded[j] && mSelectionByCategory[j].size() > 1 ) {
				mSelectionCategory = static_cast<Category>( j );
				mSelectionName     = mSelectionByCategory[j];
				break;
			}
		}
	}
}

bool SceneEditController::SetSelection( Category cat, const String& entityName )
{
	// Category::None: clear every per-category selection + the
	// expanded flags AND the primary tuple.  No side effect on
	// the scene.  The panel's "collapse this section without
	// affecting others" flow is `CollapseSection`, not this.
	if( cat == Category::None ) {
		for( int i = 0; i < kNumCategories; ++i ) {
			mSelectionByCategory[i] = String();
			mSectionExpanded[i]     = false;
		}
		mSelectionCategory = Category::None;
		mSelectionName     = String();
		return true;
	}

	// Camera / Rasterizer activations are real scene mutations — they
	// rebind the rasterizer's view of the scene, which the render
	// thread reads per-pixel.  Same cancel-and-park serialization as
	// the existing SetProperty("active_camera") path uses.  Film
	// preset picks call SetFilm under the same lock (rebuilds the
	// FrameStore + resyncs every camera).  Object / Light selections
	// are pure UI state and don't need the lock.
	const bool needsRenderSerialization =
		( cat == Category::Camera || cat == Category::Rasterizer
		  || cat == Category::Film || cat == Category::Animation
		  || cat == Category::SceneVariant )
		&& entityName.size() > 1;   // empty name = just expand, no swap

	if( needsRenderSerialization )
	{
		if( mTxnOpen ) {
			GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: active camera/rasterizer/animation/film switch refused inside an open transaction (not undoable -> rollback cannot revert it)." );
			return false;
		}
		
		// Resolve the Film preset BEFORE taking the lock.  A miss is
		// not an error — empty-name we already excluded above, but a
		// non-empty unknown label (e.g. stale UI state) should fall
		// through to the UI-only path rather than holding the lock for
		// nothing.
		const FilmPreset* filmPreset = nullptr;
		if( cat == Category::Film ) {
			const int presetIdx = FilmIntrospection::FindPresetByLabel( entityName );
			if( presetIdx < 0 ) {
				mSelectionCategory = cat;
				mSelectionName     = entityName;
				return true;
			}
			filmPreset = FilmIntrospection::PresetAt(
				static_cast<unsigned int>( presetIdx ) );
		}

		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) )
		{
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		// Apply the activation while the lock is held so the render
		// thread can't read pRasterizer / GetCamera mid-swap.
		bool ok = true;
		if( cat == Category::Camera )
		{
			IScenePriv* sp = mJob.GetScene();
			ok = sp && sp->SetActiveCamera( entityName.c_str() );
		}
		else if( cat == Category::Rasterizer )
		{
			ok = mJob.SetActiveRasterizer( entityName.c_str() );
		}
		else if( cat == Category::Animation )
		{
			// Activating a named animation changes which timelines drive the
			// scene; the next render evaluates the new active animation.
			ok = mJob.SetActiveAnimation( entityName.c_str() );
		}
		else if( cat == Category::SceneVariant )
		{
			// A variant switch RE-BAKES the scene (new materials), unlike the other activations -> re-derive the
			// retained CST Document with the forced variant + bump the epoch so the panels re-read the changed
			// structure.  "(base)" is the synthetic no-variant entry.
			// Note: the full re-derive resets the other live activations to the document's values -- the active camera to
			// the variant's active_camera (if it sets one) else the base camera; the active rasterizer/animation to their
			// authored defaults.  Intended (a mode switch), but a surprise worth flagging.  The prior undo history
			// survives the switch.  A stale post-switch undo of a CST material edit no longer relies on the serial
			// guard (Slice 3 skips it for SetMaterialProperty on a retained-CST scene): it replays BY NAME through
			// Job::ApplyCstParamEdit against the CURRENT document and re-derives (the D2 full re-derive, since a
			// variant scene refuses the incremental), reverting that param within the active variant -- a real
			// revert, not a no-op.  Direct-mutation (object/camera/light) undos still hit the serial guard.
			ok = mJob.RederiveCstWithVariant( entityName == String( "(base)" ) ? "none" : entityName.c_str() );
			if( ok ) {
				RebindEditorToJob();   // the re-derive replaced the Scene + managers -> re-point mEditor (else UAF)
				mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
			}
		}
		else if( cat == Category::Film && filmPreset )
		{
			// Preserve the scene's pixelAR — the preset is just the
			// new (width, height); pixelAR stays whatever the user
			// authored (or the screen-fit / property-panel-edit set).
			const IScene* sceneRef = mJob.GetScene();
			const IFilm*  filmRef  = sceneRef ? sceneRef->GetFilm() : nullptr;
			const double  pAR      = filmRef ? filmRef->GetPixelAR() : 1.0;
			ok = mJob.SetFilm( filmPreset->width, filmPreset->height, pAR );

			// Refresh the full-res dim cache inside the critical
			// section (same pattern as SetProperty(Film)).  Bridge
			// callers reading GetCameraDimensions in the brief
			// unlock-and-notify window need to see the new dims.
			if( ok ) {
				const IScene* sceneAfter = mJob.GetScene();
				const IFilm*  filmAfter  = sceneAfter ? sceneAfter->GetFilm() : nullptr;
				if( filmAfter ) {
					mFullResW.store( filmAfter->GetWidth(),
						std::memory_order_release );
					mFullResH.store( filmAfter->GetHeight(),
						std::memory_order_release );
				}
			}
		}
		if( !ok ) return false;

		mSelectionCategory = cat;
		mSelectionName     = entityName;
		mSelectionByCategory[ static_cast<int>( cat ) ] = entityName;
		mSectionExpanded[ static_cast<int>( cat ) ]     = true;
		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	// UI-only path (Object / Light / Material, or empty-name
	// expand-only for Camera / Rasterizer / Film).  Setting
	// `mSectionExpanded[cat] = true` is the key fix from the
	// post-Phase-4b regression: a header click sends empty name,
	// and without this flag the panel saw "nothing picked" and
	// rendered the section collapsed.
	const int catIdx = static_cast<int>( cat );
	mSelectionCategory = cat;
	mSelectionName     = entityName;
	mSelectionByCategory[ catIdx ] = entityName;
	mSectionExpanded[ catIdx ]     = true;

	// Phase 4b auto-sync rules:
	// (a) Object pick (non-empty) -> auto-fill AND auto-expand the
	//     Materials AND Media sections with the object's bound
	//     material / interior-medium names.  Empty binding still
	//     expands the section (the user sees an open section with
	//     a "(unset)" combo).
	// (b) Material direct pick (non-empty) -> clear AND collapse
	//     the Object section per the user-confirmed rule.
	// (c) Medium direct pick (non-empty) -> same as (b): collapse
	//     Object, since the user is editing media independently.
	if( cat == Category::Object ) {
		if( entityName.size() > 1 ) {
			const int matIdx = static_cast<int>( Category::Material );
			mSelectionByCategory[ matIdx ] = FindObjectMaterialName( mJob, entityName );
			mSectionExpanded[ matIdx ]     = true;

			const int medIdx = static_cast<int>( Category::Medium );
			mSelectionByCategory[ medIdx ] = FindObjectInteriorMediumName( mJob, entityName );
			mSectionExpanded[ medIdx ]     = true;
		}
		// Note: a "section header click" on Object with empty name
		// does NOT auto-expand Material/Media — the user explicitly
		// opened just Object.  They expand when an entity is picked.
	} else if( cat == Category::Material && entityName.size() > 1 ) {
		const int objIdx = static_cast<int>( Category::Object );
		mSelectionByCategory[ objIdx ] = String();
		mSectionExpanded[ objIdx ]     = false;
	} else if( cat == Category::Medium && entityName.size() > 1 ) {
		const int objIdx = static_cast<int>( Category::Object );
		mSelectionByCategory[ objIdx ] = String();
		mSectionExpanded[ objIdx ]     = false;
	}
	return true;
}

bool SceneEditController::GetAnimationOptions( double& timeStart, double& timeEnd,
                                               unsigned int& numFrames ) const
{
	bool doFields = false, invertFields = false;
	return mJob.GetAnimationOptions( timeStart, timeEnd, numFrames, doFields, invertFields );
}


bool SceneEditController::GetCameraDimensions( unsigned int& w, unsigned int& h ) const
{
	const unsigned int cachedW = mFullResW.load( std::memory_order_acquire );
	const unsigned int cachedH = mFullResH.load( std::memory_order_acquire );
	if( cachedW == 0 || cachedH == 0 ) {
		// Cache not yet primed (Start hasn't run, or no camera was
		// attached at Start time).  Fall back to the camera's current
		// dims — caller gets less stable values but at least
		// non-zero ones, which is better than failing the pointer
		// event entirely.  This path runs only at startup, before
		// any render pass refreshes the cache.
		if( const IScene* scene = mJob.GetScene() ) {
			if( const IFilm* film = scene->GetFilm() ) {
				w = film->GetWidth();
				h = film->GetHeight();
				return w > 0 && h > 0;
			}
		}
		return false;
	}
	w = cachedW;
	h = cachedH;
	return true;
}

void SceneEditController::ForTest_SetSelection( Category cat, const String& name )
{
	// Test bypass: write selection state directly without going through
	// the cancel-and-park serialization in SetSelection.  Tests run with
	// no live render thread, so there's nothing to serialize against.
	// Phase 4b: must also update the per-category state arrays so test
	// assertions on `IsSectionExpanded` / `GetSelectionNameForCategory`
	// (and RefreshProperties' per-cat snapshot population) see the
	// same state a UI-driven SetSelection would produce.  We don't
	// apply the Object→Material auto-sync here — tests that want it
	// can call SetSelection directly.
	mSelectionCategory = cat;
	mSelectionName     = name;
	const int idx = static_cast<int>( cat );
	if( idx > 0 && idx < kNumCategories ) {
		mSelectionByCategory[idx] = name;
		mSectionExpanded[idx]     = true;
	}
}

unsigned int SceneEditController::ForTest_GetCancelCount() const
{
	return mCancelCount.load( std::memory_order_acquire );
}

unsigned int SceneEditController::ForTest_GetRenderCount() const
{
	return mRenderCount.load( std::memory_order_acquire );
}

bool SceneEditController::ForTest_WaitForRenders( unsigned int count, unsigned int timeoutMs )
{
	const auto deadline = std::chrono::steady_clock::now()
	                    + std::chrono::milliseconds( timeoutMs );
	while( mRenderCount.load( std::memory_order_acquire ) < count )
	{
		if( std::chrono::steady_clock::now() >= deadline ) return false;
		std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
	}
	return true;
}

// Picking -------------------------------------------------------------

namespace {

// Enumerate-callback that records the manager's name corresponding to
// a given IObject* identity.  IObject doesn't expose its registered
// name, so we have to walk the manager and compare pointers.
class FindObjectNameCallback : public IEnumCallback<const char*>
{
public:
	const IObject*    target;
	IObjectManager*   manager;
	String            foundName;

	FindObjectNameCallback( const IObject* t, IObjectManager* m )
	: target( t ), manager( m ) {}

	bool operator()( const char* const& name ) override {
		if( !target || !manager ) return false;  // stop
		const IObject* candidate = manager->GetItem( name );
		if( candidate == target ) {
			foundName = String( name );
			return false;  // found, stop enumeration
		}
		return true;  // keep going
	}
};

}  // namespace

void SceneEditController::PickAt( const Point2& px )
{
	// Pick the topmost object under the click and route the result
	// through SetSelection so the accordion auto-expands to Objects
	// AND (per Phase 4b auto-sync) the Materials section auto-fills
	// with the object's bound material.  No hit ⇒ clear via
	// SetSelection(None) which collapses every expanded section.
	// Crucial: must NOT write to mSelectionCategory/mSelectionName
	// directly here — that bypasses the per-category state
	// (mSectionExpanded + mSelectionByCategory) the panel reads,
	// leaving the new selection invisible to the GUI.

	const IScene* scene = mJob.GetScene();
	if( !scene ) { SetSelection( Category::None, String() ); return; }

	const ICamera* cam = scene->GetCamera();
	if( !cam ) { SetSelection( Category::None, String() ); return; }

	IObjectManager* objs = const_cast<IObjectManager*>( scene->GetObjects() );
	if( !objs ) { SetSelection( Category::None, String() ); return; }

	// Spatial structure must be current before IntersectRay — defensive
	// in case nothing has rendered yet (rebuild is a no-op when valid).
	objs->PrepareForRendering();

	// Generate a primary camera ray for this pixel.  RuntimeContext
	// here is just enough to satisfy the GenerateRay signature — the
	// simple pinhole / thinlens path doesn't read it.  Picking is
	// best-effort; if more exotic cameras need a real rc later, plumb.
	//
	// Y-flip: the camera projection inverts Y between the rasterizer's
	// screen-pixel space (where the IRasterImage is laid out) and the
	// world-up direction the user perceives.  When the rasterized image
	// is displayed in the platform viewport, the visual top corresponds
	// to "image-pixel space large-Y", and the visual bottom to
	// small-Y — the opposite of the top-left view-coords the bridge
	// hands us.  Without this flip, clicking visually low picks objects
	// rendered visually high (camera-projection inversion).
	const Scalar pickPxY = static_cast<Scalar>( scene->GetFilm()->GetHeight() ) - px.y;

	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, /*threaded*/false );
	Ray r;
	if( !cam->GenerateRay( rc, r, Point2( px.x, pickPxY ) ) ) {
		SetSelection( Category::None, String() );
		return;
	}

	RasterizerState rast;
	RayIntersection ri( r, rast );
	objs->IntersectRay( ri, /*frontFace*/true, /*backFace*/false, /*exit*/false );

	if( ri.geometric.bHit && ri.pObject ) {
		// IObject has no GetName() — walk the manager to recover the
		// registered name corresponding to the hit pointer.  O(n);
		// fine for click cadence and typical scene sizes.
		FindObjectNameCallback cb( ri.pObject, objs );
		objs->EnumerateItemNames( cb );
		if( cb.foundName.size() > 1 ) {
			// SetSelection(Object, name) is the right path:
			// (a) updates the primary tuple,
			// (b) updates mSelectionByCategory[Object] + sets the
			//     expanded flag,
			// (c) auto-syncs Materials section to the object's
			//     bound material (Phase 4b auto-sync rule).
			SetSelection( Category::Object, cb.foundName );
			return;
		}
	}

	// No hit (or hit on an unregistered object) — collapse the panel
	// entirely, matching pre-Phase-4b behaviour.  If users prefer
	// "no-hit = no change to selection", flip this to a no-op and
	// the Object/Material sections will stay where they were.
	SetSelection( Category::None, String() );
}

// Render thread -------------------------------------------------------

void SceneEditController::KickRender()
{
	// Stamp the edit time first so the render loop sees a fresh
	// value when it wakes — the refinement-vs-edit branch in
	// RenderLoop reads mLastEditTimeMs only after seeing
	// mEditPending=true, so this store-before-edit-pending is the
	// happens-before edge.
	mLastEditTimeMs.store( NowMs(), std::memory_order_release );

	// Any user-driven kick cancels a pending polish — the user is
	// editing again, so the polish pass would just render a state
	// that's about to be invalidated.  OnPointerUp explicitly sets
	// FinalRegularRunning AFTER calling KickRender, so its polish
	// queue survives this reset.
	mPolishState.store( static_cast<int>( PolishState::None ), std::memory_order_release );

	// Hold mMutex across the store-then-notify so the render thread
	// cannot be parked between its predicate check and cv.wait.
	// Without this, a kick that lands in that window is silently
	// dropped (lost wakeup) and the user-visible symptom is "drag
	// the camera, viewport doesn't update until the next drag tick."
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mEditPending.store( true, std::memory_order_release );
	}
	if( mRendering.load( std::memory_order_acquire ) )
	{
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.notify_one();
}

// Phase 6.5 (docs/ROUND_TRIP_SAVE_PLAN.md §9.9): write dirty edits to
// disk using SaveEngine.  Three-step cancel-and-park dance:
//   1. Park the render thread (cancel in-flight + wait for
//      mRendering=false), set mSaving=true.
//   2. Run SaveEngine outside the lock (file IO is slow).
//   3. Reacquire lock, clear mSaving, capture any error, notify the
//      render loop.
//
// V1 has no `mScaleFromAnchorSet` reference exposed on the live
// SceneEditor (it's const-accessed via ScaleFromAnchorSet()) — the
// SaveEngine takes a non-const reference to clear post-save.  We
// copy into a local, pass it to the engine, then mirror its clear
// back onto the editor's state via ClearDirtyState() on success.
// Phase B can wire the live reference once the SaveEngine is plumbed
// into the cancel-and-park path more thoroughly.
SaveResult SceneEditController::RequestSave( const std::string& filePath )
{
	// ---- Step 1: park render thread + mark save in flight -----------
	{
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load() ) {
			mCancelProgress.RequestCancel();
		}
		mCV.wait( lk, [&]{ return !mRendering.load(); } );
		mSaving.store( true );
	}

	// ---- Step 2: run SaveEngine WITHOUT the lock --------------------
	// File IO can take a few ms; holding mMutex across that would
	// stall any other UI-state transition that needs the lock.
	std::unordered_set<std::string> sfaCopy = mEditor.ScaleFromAnchorSet();
	SaveEngine engine(
		mJob,
		*mJob.GetSourceSpanIndex(),
		*mJob.GetOverrideSpanIndex(),
		*mJob.GetBaseTransformSnapshot(),
		*mJob.GetLoadedTransformSnapshot(),
		mEditor.Dirty(),
		sfaCopy );
	SaveResult result = engine.Save( filePath );

	// ---- Step 3: publish results + release render loop --------------
	// notify_one runs INSIDE the lock_guard scope (P2.3 review fix):
	// without that, the render thread could enter cv.wait between our
	// lock-release and the notify, having last observed mSaving=true,
	// and miss the wakeup until kRefineWakeMs timeout fires.  Holding
	// the mutex around the wakeup matches the pattern used in Stop()
	// and OnTimeScrub.
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mSaving.store( false );
		if( Succeeded( result.status ) ) {
			// Engine cleared mEditor.Dirty() already (it holds a non-
			// const ref).  Clear the SFA set on the editor side too,
			// since the engine cleared its local copy.
			mEditor.ClearDirtyState();
			mLastSaveError.clear();
		} else {
			mLastSaveError = result.errorMessage;
		}
		mCV.notify_one();
	}

	return result;
}

std::string SceneEditController::LastSaveError() const
{
	// Snapshot under the lock for the diagnostic-logger thread-safety
	// concern (P2.2 review fix): a reader holding the result by
	// reference across a subsequent RequestSave would see a torn
	// std::string mid-mutation.  Returning by value (after a locked
	// copy) eliminates that.
	std::lock_guard<std::mutex> lk( mMutex );
	return mLastSaveError;
}

void SceneEditController::RenderLoop()
{
	// Initial render so the user sees something on Start.
	mEditPending.store( true, std::memory_order_release );

	while( mRunning.load( std::memory_order_acquire ) )
	{
		bool isExplicitEdit;
		{
			std::unique_lock<std::mutex> lk( mMutex );

			// If we're in idle-refinement mode (pointer held OR
			// property scrub active, but the scale hasn't reached 1
			// yet), wait at most kRefineWakeMs so we periodically
			// poll for "user has paused, step the scale down".
			// Otherwise wait indefinitely for the next edit or a
			// stop signal.  Both flags qualify because either gesture
			// drives the same adaptive-scaling logic.
			const bool gestureActive =
				mPointerDown.load( std::memory_order_acquire )
			 || mScrubInProgress.load( std::memory_order_acquire );
			const bool refineMode =
				gestureActive
			 && mPreviewScale.load( std::memory_order_acquire ) > kPreviewScaleMin;
			// Phase 6.5: mSaving gates the render loop closed.  When
			// a save is in flight, we don't start a new render pass
			// (file IO + render-thread frame reads would race).  The
			// save wraps up quickly (ms for typical scene files);
			// loop-resume is automatic via the post-save cv.notify_one.
			auto pred = [&]{
				return ( mEditPending.load( std::memory_order_acquire )
				      && !mSaving.load( std::memory_order_acquire ) )
				    || !mRunning.load( std::memory_order_acquire );
			};
			if( refineMode )
			{
				mCV.wait_for( lk, std::chrono::milliseconds( kRefineWakeMs ), pred );
			}
			else
			{
				mCV.wait( lk, pred );
			}

			isExplicitEdit = mEditPending.exchange( false, std::memory_order_acq_rel );
		}

		if( !mRunning.load( std::memory_order_acquire ) ) break;

		// Property-scrub watchdog: if a scrub gesture began but no
		// edits have arrived for kScrubWatchdogMs, presume the End
		// callback was lost (SwiftUI gesture interrupted by parent
		// re-render, Compose pointerInput torn down, Qt mouse-up
		// over a different window) and clear the flag.  Without
		// this, a missed End would leave mScrubInProgress=true
		// forever and the adaptive scaler would keep the preview
		// at low quality even after the user released.  The
		// idle-refinement walk-down below ALSO recovers visually,
		// but only after this flag clears (otherwise gestureActive
		// stays true and the during-motion adaptation re-bumps the
		// scale on every refinement pass).
		if( mScrubInProgress.load( std::memory_order_acquire ) ) {
			const long long sinceEditMs =
				NowMs() - mLastEditTimeMs.load( std::memory_order_acquire );
			if( sinceEditMs > kScrubWatchdogMs ) {
				mScrubInProgress.store( false, std::memory_order_release );
			}
		}

		if( !isExplicitEdit )
		{
			// Refinement tick — the wait_for timed out without an
			// edit landing.  Step the scale down one level and run
			// a pass at the finer resolution, but only if the user
			// has been quiet for at least kRefineIdleMs (otherwise
			// a fast-mid-drag burst of edits without timing changes
			// would refine prematurely).  If not enough time has
			// elapsed yet, loop back to wait again.
			const long long now = NowMs();
			const long long sinceEdit = now - mLastEditTimeMs.load( std::memory_order_acquire );
			if( sinceEdit < kRefineIdleMs ) continue;

			const unsigned int s = mPreviewScale.load( std::memory_order_acquire );
			if( s <= kPreviewScaleMin ) continue;
			mPreviewScale.store( s / 2, std::memory_order_release );
			mInRefinementPass = true;
		}
		else
		{
			mInRefinementPass = false;
		}

		// Polish-pass configuration.  Only meaningful when the
		// rasterizer is the InteractivePelRasterizer (mInteractiveImpl
		// is null in test mode).  We snapshot the polish state BEFORE
		// the pass so the post-pass logic can see what we ran.
		const PolishState polishStateBefore =
			static_cast<PolishState>( mPolishState.load( std::memory_order_acquire ) );
		const bool isPolishPass = ( polishStateBefore == PolishState::PolishQueued );
		if( mInteractiveImpl ) {
			Implementation::InteractivePelRasterizer::PreviewDenoiseMode denoiseMode =
				Implementation::InteractivePelRasterizer::PreviewDenoise_Off;
			if( isPolishPass ) {
				denoiseMode = Implementation::InteractivePelRasterizer::PreviewDenoise_Balanced;
			} else if( mInRefinementPass &&
			           mPreviewScale.load( std::memory_order_acquire ) <= 2 ) {
				denoiseMode = Implementation::InteractivePelRasterizer::PreviewDenoise_Fast;
			}
			mInteractiveImpl->SetPreviewDenoiseMode( denoiseMode );
			mInteractiveImpl->SetSampleCount( isPolishPass ? kPolishSampleCount : 1 );
		}

		// Both rendering transitions are serialised under mMutex so
		// OnTimeScrub (which mutates scene state that DoOneRenderPass
		// is reading) can hold the lock across its wait+mutate+kick
		// window and be sure the render thread can't observe an
		// in-progress mutation.  Setting rendering=true while holding
		// the lock blocks the render thread from beginning a new
		// pass while the main thread is partway through a mutation
		// it queued before the previous pass's notify woke us.
		{
			std::lock_guard<std::mutex> lk( mMutex );
			mCancelProgress.Reset();
			mRendering.store( true, std::memory_order_release );
		}

		DoOneRenderPass();
		mRenderCount.fetch_add( 1, std::memory_order_acq_rel );

		// Set rendering=false and notify any waiter (OnTimeScrub uses
		// this to wait until the in-flight pass observes the cancel
		// and returns — without it, the animator's keyframe-driven
		// mesh/BSP rebuilds race with an in-flight IntersectRay).
		{
			std::lock_guard<std::mutex> lk( mMutex );
			mRendering.store( false, std::memory_order_release );
		}
		mCV.notify_all();

		// Polish-pass post-roll.  Three transitions land here:
		//
		//   1. We just finished a polish pass.  Restore the rasterizer
		//      to 1-SPP / preview caster and reset the state machine.
		//      If the user moved during the polish (KickRender already
		//      reset state to None), we still want to restore — checking
		//      isPolishPass (the local snapshot) is robust against that
		//      race.
		//
		//   2. We just finished the regular 1-SPP scale=1 final pass
		//      that OnPointerUp queued.  Transition to PolishQueued and
		//      flag mEditPending so the loop runs once more.
		//
		//   3. Anything else: leave state alone.
		if( isPolishPass )
		{
			if( mInteractiveImpl ) {
				mInteractiveImpl->SetSampleCount( 1 );
				mInteractiveImpl->SetPreviewDenoiseMode(
					Implementation::InteractivePelRasterizer::PreviewDenoise_Off );
			}
			// Don't blindly clobber state to None — KickRender (user
			// edit during polish) may have already reset it.  Only
			// transition out of PolishQueued.
			int expected = static_cast<int>( PolishState::PolishQueued );
			mPolishState.compare_exchange_strong(
				expected, static_cast<int>( PolishState::None ),
				std::memory_order_acq_rel );
		}
		else if( polishStateBefore == PolishState::FinalRegularRunning )
		{
			// Only transition if the state hasn't been reset by a
			// concurrent user edit.  CAS guards against the race.
			int expected = static_cast<int>( PolishState::FinalRegularRunning );
			if( mPolishState.compare_exchange_strong(
					expected, static_cast<int>( PolishState::PolishQueued ),
					std::memory_order_acq_rel ) )
			{
				// Queue the polish pass.  Don't update mLastEditTimeMs
				// — this is a controller-internal kick, not a user
				// edit, and refinement / resume-snap logic should not
				// be confused by it.
				std::lock_guard<std::mutex> lk( mMutex );
				mEditPending.store( true, std::memory_order_release );
				mCV.notify_one();
			}
		}
	}
}

// Properties panel ----------------------------------------------------

SceneEditController::PanelMode SceneEditController::CurrentPanelMode() const
{
	// Selection drives the panel mode directly (Category and PanelMode
	// share numeric values so the cast is a no-op).  Camera-motion
	// tools no longer auto-promote to Camera mode — picking a Camera
	// in the accordion is the explicit way to inspect / edit camera
	// properties.  This keeps the rule simple: the property panel
	// shows whatever is selected.
	switch( mSelectionCategory ) {
	case Category::Camera:     return PanelMode::Camera;
	case Category::Rasterizer: return PanelMode::Rasterizer;
	case Category::Object:
		// Only treat as Object mode if a real entity name is set
		// (matches the legacy "size > 1" guard — an accordion
		// header click can land us in Object category with empty
		// name, which means "expand the section, no row picked").
		return mSelectionName.size() > 1 ? PanelMode::Object : PanelMode::None;
	case Category::Light:
		return mSelectionName.size() > 1 ? PanelMode::Light : PanelMode::None;
	case Category::Film:
		// Single Film per scene — auto-select on section expand so
		// the user doesn't have to click through a one-entry list.
		// Properties resolve against scene->GetFilm() regardless of
		// the selection name.
		return PanelMode::Film;
	case Category::Material:
		// Same guard as Object/Light — empty name means "section open,
		// no row picked yet"; we show nothing in the panel until the
		// user clicks a material.
		return mSelectionName.size() > 1 ? PanelMode::Material : PanelMode::None;
	case Category::Medium:
		// Same guard as Object/Light/Material — empty name means
		// "section open, no row picked yet"; we render nothing until
		// the user clicks a medium row.
		return mSelectionName.size() > 1 ? PanelMode::Medium : PanelMode::None;
	case Category::Animation:
		// No editable properties — selection just activates the path.
		return PanelMode::None;
	case Category::SceneVariant:
		// No editable properties -- selection re-derives the scene with that variant active.
		return PanelMode::None;
	case Category::None:
	default:
		return PanelMode::None;
	}
}

String SceneEditController::CurrentPanelHeader() const
{
	switch( CurrentPanelMode() ) {
	case PanelMode::Camera: {
		// Show the active camera's name when one is selected, fall
		// back to "Cameras" otherwise.  Mirrors the macOS / Windows
		// convention used by the existing Camera panel header.
		if( mSelectionName.size() > 1 ) {
			std::string s = "Camera: ";
			s += mSelectionName.c_str();
			return String( s.c_str() );
		}
		return String( "Cameras" );
	}
	case PanelMode::Rasterizer: {
		std::string s = "Rasterizer: ";
		s += mSelectionName.c_str();
		return String( s.c_str() );
	}
	case PanelMode::Object: {
		// String concatenation is awkward on RISE::String (a vector
		// of char) — build via std::string and convert at the end.
		std::string s = "Object: ";
		s += mSelectionName.c_str();
		return String( s.c_str() );
	}
	case PanelMode::Light: {
		std::string s = "Light: ";
		s += mSelectionName.c_str();
		return String( s.c_str() );
	}
	case PanelMode::Film:
		return String( "Output Settings" );
	case PanelMode::Material: {
		std::string s = "Material: ";
		s += mSelectionName.c_str();
		return String( s.c_str() );
	}
	case PanelMode::Medium: {
		std::string s = "Medium: ";
		s += mSelectionName.c_str();
		return String( s.c_str() );
	}
	case PanelMode::None:
	default:
		return String();
	}
}

void SceneEditController::RefreshProperties()
{
	mProperties.clear();
	for( int i = 0; i < kNumCategories; ++i ) mPropertiesByCategory[i].clear();

	const IScene* scene = mJob.GetScene();
	if( !scene ) return;

	// Build per-category property rows for every category that has
	// a non-empty selection.  Each section in the panel renders its
	// own rows from `mPropertiesByCategory[cat]`.  For back-compat
	// with the single-tuple PropertyXxx accessors, also populate
	// `mProperties` from the PRIMARY category's rows.
	auto buildRowsFor = [&]( Category cat, const String& selName ) -> std::vector<CameraProperty> {
		std::vector<CameraProperty> out;
		switch( cat ) {
		case Category::Camera: {
			const ICamera* cam = 0;
			if( selName.size() > 1 ) {
				const ICameraManager* cams = scene->GetCameras();
				if( cams ) cam = cams->GetItem( selName.c_str() );
			}
			if( !cam ) cam = scene->GetCamera();
			if( !cam ) break;
			out = CameraIntrospection::Inspect( *cam );
			break;
		}
		case Category::Rasterizer:
			out = RasterizerIntrospection::Inspect( mJob, selName );
			break;
		case Category::Object: {
			IObjectManager* objs = const_cast<IObjectManager*>( scene->GetObjects() );
			if( !objs ) break;
			const IObject* obj = objs->GetItem( selName.c_str() );
			if( !obj ) break;
			out = ObjectIntrospection::Inspect( selName, *obj,
				mJob.GetMaterials(), mJob.GetShaders(), &mJob );
			break;
		}
		case Category::Light: {
			const ILightManager* lights = scene->GetLights();
			if( !lights ) break;
			const ILightPriv* light = lights->GetItem( selName.c_str() );
			if( !light ) break;
			out = LightIntrospection::Inspect( selName, *light );
			break;
		}
		case Category::Film: {
			const IFilm* film = scene->GetFilm();
			if( !film ) break;
			out = FilmIntrospection::Inspect( *film );
			break;
		}
		case Category::Material: {
			const IMaterialManager* mats = mJob.GetMaterials();
			if( !mats ) break;
			const IMaterial* mat =
				const_cast<IMaterialManager*>( mats )->GetItem( selName.c_str() );
			if( !mat ) break;
			out = MaterialIntrospection::Inspect( selName, *mat,
				mJob.GetPainters(), mJob.GetScalarPainters(), &mJob );
			break;
		}
		case Category::Medium: {
			const IMedium* med = mJob.GetMedium( selName.c_str() );
			if( !med ) break;
			out = MediaIntrospection::Inspect( selName, *med );
			break;
		}
		case Category::Animation: {
			// Named animations expose one editable property: the frame count of
			// the ACTIVE animation (picking one in the list activates it).  More
			// frames = a longer, smoother rendered/previewed clip; fewer =
			// shorter.  The scene fixes the time range; only the sampling count
			// is user-tunable here.
			if( mJob.GetAnimationCount() == 0 ) break;
			double ts = 0, te = 1; unsigned int nf = 30; bool df = false, invf = false;
			if( !mJob.GetAnimationOptions( ts, te, nf, df, invf ) ) break;
			CameraProperty row;
			row.name        = String( "frames" );
			row.kind        = ValueKind::UInt;
			row.value       = String( std::to_string( nf ).c_str() );
			row.description = String( "Number of frames the animation renders and the preview Play button loops over.  More frames = a longer, smoother clip; fewer = shorter." );
			row.editable    = true;
			row.unitLabel   = String( "frames" );
			out.push_back( row );
			break;
		}
		case Category::None:
		default:
			break;
		}
		return out;
	};

	// Per-category snapshots.  Build rows for every section whose
	// expanded flag is true — that includes "header just clicked,
	// no entity picked yet" (the section's combo renders, and
	// Camera/Rasterizer/Film fall back to their active-entity
	// rows so users see something rather than a blank section).
	for( int i = 1; i < kNumCategories; ++i ) {
		if( !mSectionExpanded[i] ) continue;
		const Category cat = static_cast<Category>( i );
		mPropertiesByCategory[i] = buildRowsFor( cat, mSelectionByCategory[i] );
	}

	// Back-compat single-tuple snapshot drives the existing
	// PropertyCount() / PropertyName(idx) / ... accessors.  Routes
	// the primary category's rows.
	switch( CurrentPanelMode() ) {
	case PanelMode::Camera:
		mProperties = mPropertiesByCategory[ static_cast<int>( Category::Camera ) ];
		break;
	case PanelMode::Rasterizer:
		mProperties = mPropertiesByCategory[ static_cast<int>( Category::Rasterizer ) ];
		break;
	case PanelMode::Object:
		mProperties = mPropertiesByCategory[ static_cast<int>( Category::Object ) ];
		break;
	case PanelMode::Light:
		mProperties = mPropertiesByCategory[ static_cast<int>( Category::Light ) ];
		break;
	case PanelMode::Film:
		mProperties = mPropertiesByCategory[ static_cast<int>( Category::Film ) ];
		break;
	case PanelMode::Material:
		mProperties = mPropertiesByCategory[ static_cast<int>( Category::Material ) ];
		break;
	case PanelMode::Medium:
		mProperties = mPropertiesByCategory[ static_cast<int>( Category::Medium ) ];
		break;
	case PanelMode::None:
	default:
		break;
	}
}

unsigned int SceneEditController::PropertyCount() const
{
	return static_cast<unsigned int>( mProperties.size() );
}

String SceneEditController::PropertyName( unsigned int idx ) const
{
	if( idx >= mProperties.size() ) return String();
	return mProperties[idx].name;
}

String SceneEditController::PropertyValue( unsigned int idx ) const
{
	if( idx >= mProperties.size() ) return String();
	return mProperties[idx].value;
}

String SceneEditController::PropertyDescription( unsigned int idx ) const
{
	if( idx >= mProperties.size() ) return String();
	return mProperties[idx].description;
}

int SceneEditController::PropertyKind( unsigned int idx ) const
{
	if( idx >= mProperties.size() ) return -1;
	return static_cast<int>( mProperties[idx].kind );
}

bool SceneEditController::PropertyEditable( unsigned int idx ) const
{
	if( idx >= mProperties.size() ) return false;
	return mProperties[idx].editable;
}

unsigned int SceneEditController::PropertyPresetCount( unsigned int idx ) const
{
	if( idx >= mProperties.size() ) return 0;
	return static_cast<unsigned int>( mProperties[idx].presets.size() );
}

String SceneEditController::PropertyPresetLabel( unsigned int idx, unsigned int presetIdx ) const
{
	if( idx >= mProperties.size() ) return String();
	const auto& presets = mProperties[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].label.c_str() );
}

String SceneEditController::PropertyPresetValue( unsigned int idx, unsigned int presetIdx ) const
{
	if( idx >= mProperties.size() ) return String();
	const auto& presets = mProperties[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].value.c_str() );
}

String SceneEditController::PropertyUnitLabel( unsigned int idx ) const
{
	if( idx >= mProperties.size() ) return String();
	return mProperties[idx].unitLabel;
}

// -------------------------------------------------------------------
// Phase 4b — per-category property accessors.  Each looks up the
// correct mPropertiesByCategory[cat] vector and forwards to the
// matching CameraProperty field.  Bounds-check the category enum and
// the row index; out-of-range returns sensible empty / -1 values
// (matches the single-tuple accessors above).
// -------------------------------------------------------------------

namespace {
inline const std::vector<RISE::CameraProperty>* PropsForCat(
	const std::vector<RISE::CameraProperty>* arr, RISE::SceneEditController::Category cat )
{
	const int i = static_cast<int>( cat );
	if( i < 0 || i >= 9 ) return 0;   // 9 == kNumCategories (None..Animation)
	return &arr[i];
}
}

unsigned int SceneEditController::PropertyCountFor( Category cat ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return v ? static_cast<unsigned int>( v->size() ) : 0u;
}

String SceneEditController::PropertyNameFor( Category cat, unsigned int idx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].name : String();
}

String SceneEditController::PropertyValueFor( Category cat, unsigned int idx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].value : String();
}

String SceneEditController::PropertyDescriptionFor( Category cat, unsigned int idx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].description : String();
}

int SceneEditController::PropertyKindFor( Category cat, unsigned int idx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return ( v && idx < v->size() ) ? static_cast<int>( (*v)[idx].kind ) : -1;
}

bool SceneEditController::PropertyEditableFor( Category cat, unsigned int idx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].editable : false;
}

unsigned int SceneEditController::PropertyPresetCountFor( Category cat, unsigned int idx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return ( v && idx < v->size() ) ? static_cast<unsigned int>( (*v)[idx].presets.size() ) : 0u;
}

String SceneEditController::PropertyPresetLabelFor( Category cat, unsigned int idx, unsigned int presetIdx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	if( !v || idx >= v->size() ) return String();
	const auto& presets = (*v)[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].label.c_str() );
}

String SceneEditController::PropertyPresetValueFor( Category cat, unsigned int idx, unsigned int presetIdx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	if( !v || idx >= v->size() ) return String();
	const auto& presets = (*v)[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].value.c_str() );
}

String SceneEditController::PropertyUnitLabelFor( Category cat, unsigned int idx ) const
{
	const auto* v = PropsForCat( mPropertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].unitLabel : String();
}

namespace {

// Generate a unique camera name from a user-proposed base + the
// existing camera-name set in the scene.  If `proposed` isn't taken,
// returns it verbatim; otherwise appends "_2", "_3", ... until an
// unused name is found.  At most 1000 suffix attempts before
// fallback to a timestamp-style suffix; in practice this branch is
// unreachable since the scene won't accumulate thousands of clones.
String UniqueCameraName( const String& proposed, const ICameraManager& cams )
{
	if( proposed.size() <= 1 ) {
		// Empty proposal — default to "camera_copy"
		return String( "camera_copy" );
	}
	if( !cams.GetItem( proposed.c_str() ) ) return proposed;
	char buf[256];
	for( int i = 2; i < 1000; ++i ) {
		std::snprintf( buf, sizeof(buf), "%s_%d", proposed.c_str(), i );
		if( !cams.GetItem( buf ) ) return String( buf );
	}
	std::snprintf( buf, sizeof(buf), "%s_%lld", proposed.c_str(), static_cast<long long>( std::time( 0 ) ) );
	return String( buf );
}

}  // namespace

bool SceneEditController::CloneActiveCamera(
	const String& proposedName, char* outName, unsigned int outLen )
{
	if( !outName || outLen == 0 ) return false;
	outName[0] = '\0';

	IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	ICameraManager* cams = const_cast<ICameraManager*>( scene->GetCameras() );
	if( !cams ) return false;

	// Cancel-and-park BEFORE capturing the snapshot.  The render
	// thread reads + writes the active camera's stored fields
	// concurrently (e.g. an in-flight orbit drag updates
	// target_orientation while we'd otherwise be reading it for the
	// snapshot).  Parking serialises the read against rendering AND
	// against any other UI-thread tool that re-issues an edit; the
	// snapshot fields are doubles / Vector* which would tear under a
	// concurrent write.  Also serialises against `Job::Add*Camera`
	// mutating the ICameraManager (any future second clone path).
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	const ICamera* activeCam = scene->GetCamera();
	if( !activeCam ) return false;

	// Capture inside the lock — see comment above.
	CameraSnapshot snapshot;
	if( !CameraIntrospection::CaptureCameraSnapshot( *activeCam, snapshot ) ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::CloneActiveCamera: source camera type not clonable "
			"(currently only non-ONB Pinhole / ThinLens / Fisheye / Orthographic round-trip)" );
		return false;
	}

	// Pick a unique name under the lock so concurrent clones can't
	// pick the same dedup suffix.
	const String finalName = UniqueCameraName( proposedName, *cams );

	// Buffer-size precheck: refuse to mutate the scene if the chosen
	// name won't fit in the caller's buffer.  An after-the-fact check
	// would leave a registered orphan camera while reporting failure,
	// which is the worst-of-both outcome (the GUIs would show a
	// "couldn't add" alert next to a new camera entry in the list).
	// finalName.size() includes the trailing NUL (RString convention),
	// so the buffer needs at least that many bytes.
	if( finalName.size() > outLen ) {
		outName[0] = '\0';
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::CloneActiveCamera: outName buffer too small for `%s` (need %u, got %u) — not adding camera",
			finalName.c_str(), static_cast<unsigned>( finalName.size() ), outLen );
		return false;
	}

	SceneEdit edit;
	edit.op             = SceneEdit::AddCamera;
	edit.objectName     = finalName;
	edit.cameraSnapshot = snapshot;

	const bool ok = mEditor.Apply( edit );
	if( !ok ) return false;

	// Write back the chosen name — buffer fits by the precheck above.
	{
		const char* s = finalName.c_str();
		const size_t needed = finalName.size();
		for( size_t i = 0; i < needed; ++i ) outName[i] = s[i];
	}

	// Bump the scene epoch so platform UIs poll a fresh camera list.
	mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );

	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::SetPropertyForCategory( Category cat, const String& name, const String& valueStr )
{
	// Hacky-but-pragmatic implementation: SetProperty's existing
	// switch reads `mSelectionCategory` + `mSelectionName` inline
	// throughout its ~150-line body.  Rather than parameterize that
	// body over (cat, selName) — which would force every per-
	// category branch to take both — temporarily swap the primary
	// tuple for the call and restore on return.  Both fields are
	// UI-thread-only writes (no render-thread reads), so the
	// temporary mutation isn't racy.  Future cleanup: refactor
	// SetProperty's body into a helper that takes (cat, selName)
	// explicitly and drop this swap.
	const Category savedCat  = mSelectionCategory;
	const String   savedName = mSelectionName;
	const int idx = static_cast<int>( cat );
	mSelectionCategory = cat;
	mSelectionName     = ( idx >= 0 && idx < kNumCategories ) ? mSelectionByCategory[idx] : String();
	const bool ok = SetProperty( name, valueStr );
	mSelectionCategory = savedCat;
	mSelectionName     = savedName;
	return ok;
}

bool SceneEditController::SetProperty( const String& name, const String& valueStr )
{
	switch( mSelectionCategory ) {

	case Category::Camera: {
		// Cameras: route through the editor's transactional Apply path
		// so panel edits land in the undo history alongside drag-driven
		// camera edits.  SceneEditor::Apply for SetCameraProperty
		// captures the prev value, applies the new one via
		// CameraIntrospection, and pushes a SceneEdit so undo/redo
		// work end-to-end.
		SceneEdit edit;
		edit.op = SceneEdit::SetCameraProperty;
		edit.objectName = name;            // overload: holds the property name
		edit.propertyValue = valueStr;
		// P5 Slice 3 expansion: on a CST-loaded scene a camera property edit ROUTES THROUGH THE CST, whose D2 full
		// re-derive (variant scene) ClearAll's the live scene -- so cancel-and-park the render thread around the
		// Apply, exactly like Light / Object / Material.  (Pre-expansion camera edits only mutated camera state,
		// read at GenerateRay time and BVH-exempt, so they ran unparked; the CST route changed that surface and the
		// park was not added with it.)
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		const bool ok = mEditor.Apply( edit );
		if( ok ) {
			mEditPending.store( true, std::memory_order_release );
			lk.unlock();
			mCV.notify_one();
		}
		return ok;
	}

	case Category::Animation: {
		// The only editable animation property is the active animation's frame
		// count.  num_frames is pure playback metadata — it doesn't alter the
		// in-flight render's pixels (only renderanimation's frame count and the
		// preview-play loop step), so no cancel-and-park is needed.  Apply by
		// re-declaring the ACTIVE animation with the new count (DeclareAnimation
		// upserts; make_active=false leaves the active selection unchanged).
		if( !( name == String( "frames" ) ) ) return false;
		unsigned int newFrames = 0;
		if( sscanf( valueStr.c_str(), "%u", &newFrames ) != 1 || newFrames < 1 ) return false;
		char nameBuf[256] = { 0 };
		if( !mJob.GetActiveAnimationName( nameBuf, sizeof(nameBuf) ) ) return false;
		double ts = 0, te = 1; unsigned int nf = 30; bool df = false, invf = false;
		if( !mJob.GetAnimationOptions( ts, te, nf, df, invf ) ) return false;
		if( mTxnOpen ) {
			GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: animation frame-count edit refused inside an open transaction (not undoable -> rollback cannot revert it)." );
			return false;
		}
		return mJob.DeclareAnimation( nameBuf, ts, te, newFrames, df, invf, false );
	}

	case Category::Object: {
		// Route the panel's editable rows to the matching SceneEdit
		// op.  Every path goes through SceneEditor::Apply so undo /
		// redo work end-to-end alongside drag-driven transform edits.
		const IScene* scene = mJob.GetScene();
		if( !scene ) return false;
		if( mSelectionName.size() <= 1 ) return false;

		SceneEdit edit;
		edit.objectName = mSelectionName;

		if( name == String( "position" ) ) {
			if( !ParsePropertyVec3( valueStr, edit.v3a ) ) return false;
			edit.op = SceneEdit::SetObjectPosition;
		}
		else if( name == String( "orientation" ) ) {
			Vector3 deg;
			if( !ParsePropertyVec3( valueStr, deg ) ) return false;
			edit.op  = SceneEdit::SetObjectOrientation;
			edit.v3a = Vector3( deg.x * DEG_TO_RAD, deg.y * DEG_TO_RAD, deg.z * DEG_TO_RAD );
		}
		else if( name == String( "scale" ) ) {
			// Descriptor surfaces `scale` as DoubleVec3 (per-axis),
			// matching the standard_object chunk syntax.  Routes
			// through SetObjectStretch for per-axis precision.
			if( !ParsePropertyVec3( valueStr, edit.v3a ) ) return false;
			edit.op = SceneEdit::SetObjectStretch;
		}
		else if( name == String( "scale_uniform" ) ) {
			// Optional uniform-scale shortcut for callers that prefer
			// a single Double.  Not in the descriptor; available for
			// programmatic use.
			if( !ParsePropertyScalar( valueStr, edit.s ) ) return false;
			edit.op = SceneEdit::SetObjectScale;
		}
		else if( name == String( "stretch" ) ) {
			// Phase 3 alias kept for backward-compat.
			if( !ParsePropertyVec3( valueStr, edit.v3a ) ) return false;
			edit.op = SceneEdit::SetObjectStretch;
		}
		else if( name == String( "material" ) ) {
			edit.op = SceneEdit::SetObjectMaterial;
			edit.propertyValue = valueStr;
		}
		else if( name == String( "geometry" ) ) {
			// Runtime geometry swap (mirrors material).  The cancel-and-
			// park below covers it: AssignGeometry safe_releases the old
			// geometry pointer that render workers read per-intersection.
			edit.op = SceneEdit::SetObjectGeometry;
			edit.propertyValue = valueStr;
		}
		else if( name == String( "shader" ) ) {
			edit.op = SceneEdit::SetObjectShader;
			edit.propertyValue = valueStr;
		}
		else if( name == String( "interior_medium" ) ) {
			edit.op = SceneEdit::SetObjectInteriorMedium;
			edit.propertyValue = valueStr;
		}
		else if( name == String( "casts_shadows" ) || name == String( "receives_shadows" ) ) {
			IObjectManager* objs = const_cast<IObjectManager*>( scene->GetObjects() );
			const IObject* obj = objs ? objs->GetItem( mSelectionName.c_str() ) : 0;
			if( !obj ) return false;
			bool castsB = obj->DoesCastShadows();
			bool recvsB = obj->DoesReceiveShadows();
			bool newVal = false;
			if( !ParsePropertyBool( valueStr, newVal ) ) return false;
			if( name == String( "casts_shadows" ) )    castsB = newVal;
			if( name == String( "receives_shadows" ) ) recvsB = newVal;
			edit.op = SceneEdit::SetObjectShadowFlags;
			edit.s  = static_cast<Scalar>( ( castsB ? 1 : 0 ) | ( recvsB ? 2 : 0 ) );
		}
		else {
			return false;
		}

		// Cancel-and-park around the Apply: object property edits
		// (material / shader / interior_medium / shadow flags)
		// swap pointers the render thread reads per-shading-call.
		// `AssignMaterial / AssignShader / AssignInteriorMedium`
		// safe_release the previous pointer; without parking, a
		// worker mid-shade through the old pointer would race a
		// destructor.  Same pattern Light / Rasterizer / Film use.
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		const bool ok = mEditor.Apply( edit );
		if( !ok ) return false;

		// P5 Slice 3 expansion (object transform): a PANEL transform edit (position / orientation / scale) noted the
		// object for a `matrix`-param commit; flush it here under the SAME park (the commit re-derives).
		if( mEditor.HasPendingCstObjectTransforms() ) mEditor.CommitPendingCstObjectTransforms();

		// Phase 4b auto-sync follow-through: when the user changes
		// the selected Object's material binding (or interior medium)
		// via the property panel, the Material/medium row in the
		// auto-synced section must follow.  Without this, the
		// Material section keeps showing the OLD material's
		// properties while the Object now uses the new one.
		// Mirrors the Object-pick auto-fill in SetSelection, but
		// only updates the per-cat selection — we leave the
		// expanded flag alone so a user who collapsed Materials
		// doesn't have it pop back open on every edit.
		if( edit.op == SceneEdit::SetObjectMaterial ) {
			mSelectionByCategory[ static_cast<int>( Category::Material ) ] = valueStr;
		}
		if( edit.op == SceneEdit::SetObjectInteriorMedium ) {
			// Empty / "none" clears the Medium row (the parser also
			// accepts "none" as the unbind sentinel).  Anything else
			// pins the Medium section's selection to the new binding.
			mSelectionByCategory[ static_cast<int>( Category::Medium ) ] =
				( valueStr.size() <= 1 || valueStr == String( "none" ) )
				? String()
				: valueStr;
		}

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	case Category::Light: {
		// Phase 3: light edits route through `SceneEditor::Apply` with
		// the new `SetLightProperty` op so undo/redo work end-to-end
		// alongside object/camera edits.  The Apply path captures the
		// prev value, calls KeyframeFromParameters + SetIntermediateValue
		// + RegenerateData on the forward path, and replays the prev
		// value through the same machinery on undo.
		if( mSelectionName.size() <= 1 ) return false;
		IScenePriv* scene = mJob.GetScene();
		if( !scene ) return false;

		SceneEdit edit;
		edit.op            = SceneEdit::SetLightProperty;
		edit.objectName    = mSelectionName;   // light entity name
		edit.propertyName  = name;             // "position" / "energy" / etc.
		edit.propertyValue = valueStr;

		// Cancel-and-park around the SceneEditor::Apply call: light
		// mutations change geometry-relevant state the render thread
		// reads per-pixel.  Same pattern camera-switch uses.
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		const bool ok = mEditor.Apply( edit );
		if( !ok ) return false;

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	case Category::Rasterizer: {
		// Phase 3: the introspection layer surfaces editable rows for
		// type-specific params (samples, max_eye_depth, etc.) and the
		// Job side keeps a per-rasterizer params snapshot.  Editing
		// re-instantiates the rasterizer with the modified value while
		// preserving every other parameter.  No undo support yet —
		// rebuilding a rasterizer is a heavy operation; Phase 4 may
		// add it via a dedicated SetRasterizerProperty SceneEdit op.
		if( mSelectionName.size() <= 1 ) return false;

		if( mTxnOpen ) {
			GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: rasterizer property edit refused inside an open transaction (not undoable -> rollback cannot revert it)." );
			return false;
		}
		// Cancel-and-park: rasterizer rebuild releases the old instance
		// and constructs a new one.  The render thread reads
		// `pRasterizer` per-pixel; we need it parked.  Same pattern as
		// SetActiveRasterizer's selection path.
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		const bool ok = mJob.SetRasterizerParameter(
			mSelectionName.c_str(), name.c_str(), valueStr.c_str() );
		if( !ok ) return false;

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	case Category::Film: {
		if( mTxnOpen ) {
			GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: film property edit refused inside an open transaction (not undoable -> rollback cannot revert it)." );
			return false;
		}
		// SetFilm replaces the Scene's IFilm, resyncs every camera's
		// projection, and reallocates Job's FrameStore — that's a
		// scene mutation the render thread reads per-pixel, so the
		// same cancel-and-park pattern as Rasterizer/Light applies.
		// No undo support (matches Rasterizer); Phase 4 may add a
		// dedicated SetFilmProperty SceneEdit op.
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		const bool ok = FilmIntrospection::SetProperty( mJob, name, valueStr );
		if( !ok ) return false;

		// Refresh the full-res dim cache so bridge callers reading
		// `GetCameraDimensions` between the unlock-and-notify below and
		// the next render pass starting see the NEW dims (the render
		// thread also refreshes them at the top of `DoOneRenderPass`).
		// Without this, the brief window between SetFilm and the next
		// pass exposes a stale cache that maps pointer events through
		// the old projection.
		const IScene* sceneRef = mJob.GetScene();
		const IFilm*  filmRef  = sceneRef ? sceneRef->GetFilm() : nullptr;
		if( filmRef ) {
			mFullResW.store( filmRef->GetWidth(),  std::memory_order_release );
			mFullResH.store( filmRef->GetHeight(), std::memory_order_release );
		}

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	case Category::Material: {
		// Phase 4: route through SceneEdit::SetMaterialProperty so
		// the edit goes through the undo/redo + composite history.
		// Cancel-and-park: material edits release the prior painter
		// (potentially destroying it if no one else holds a ref),
		// which the render thread may be mid-sample on.  Same lock
		// pattern Light / Object / Film use for the same reason.
		if( mSelectionName.size() <= 1 ) return false;

		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		SceneEdit edit;
		edit.op            = SceneEdit::SetMaterialProperty;
		edit.objectName    = mSelectionName;
		edit.propertyName  = name;
		edit.propertyValue = valueStr;

		const bool ok = mEditor.Apply( edit );
		if( !ok ) return false;

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	case Category::Medium: {
		// Route through SceneEdit::SetMediumProperty.  Same cancel-
		// and-park as Material — medium setters re-derive sigma_t and
		// sigma_t_max caches that the render thread reads via
		// SampleDistance / EvalTransmittance.
		if( mSelectionName.size() <= 1 ) return false;

		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		SceneEdit edit;
		edit.op            = SceneEdit::SetMediumProperty;
		edit.objectName    = mSelectionName;
		edit.propertyName  = name;
		edit.propertyValue = valueStr;

		const bool ok = mEditor.Apply( edit );
		if( !ok ) return false;

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	case Category::None:
	default:
		return false;
	}
}

// L6e-3 — Allocate or reuse `mInteractiveFrameStore` to match the
// requested per-pass dims, then push to the interactive rasterizer
// via `SetFrameStore`.
//
// Lifecycle: same `FrameStore::Spec` defaults as Job's
// `EnsureJobFrameStore_locked` (tileEdge=32, beauty channel only).
// Reuse on dim-match avoids the FrameStore alloc + observer-thrash
// cost across passes that don't change scale.  When dims change
// (preview-scale ramp / camera resize), release the old store +
// allocate a fresh one — `SetFrameStore` on the rasterizer fires
// `OnRasterizerFrameStoreChanged` on the rasterizer's outs (the
// preview sink, just re-attached above), which in turn drives the
// interactive VFS's `BindFrameStore` rebind on the platform side
// (`ViewportPreviewSink::OnRasterizerFrameStoreChanged` in
// RISEViewportBridge.mm).
//
// Threading: called from the render thread (the one that drives
// `RasterizeScene`).  `SetFrameStore` is single-threaded relative to
// `RasterizeScene` per the Rasterizer threading contract — same as
// Job's `PushJobFrameStoreToRasterizers`.
void SceneEditController::EnsureInteractiveFrameStore_( unsigned int width, unsigned int height )
{
	if( !mInteractiveRasterizer ) return;
	if( width == 0 || height == 0 ) return;

	// Same-dim short-circuit: reuse the existing FrameStore.
	if( mInteractiveFrameStore &&
	    mInteractiveFrameStore->Width()  == width &&
	    mInteractiveFrameStore->Height() == height )
	{
		Implementation::Rasterizer* r =
			dynamic_cast<Implementation::Rasterizer*>( mInteractiveRasterizer );
		if( r ) {
			if( r->GetFrameStore() != mInteractiveFrameStore ) {
				// Pointer changed (rare; suggests something else
				// swapped mFrameStore between passes — Job push?).
				// Restore via the standard SetFrameStore path.
				r->SetFrameStore( mInteractiveFrameStore );
			} else {
				// Same pointer.  `FreeRasterizerOutputs` (just
				// above in DoOneRenderPass) cleared the rasterizer's
				// outs list, so the freshly-attached preview sink
				// hasn't received the `OnRasterizerFrameStoreChanged`
				// for THIS pointer.  `ReannounceFrameStore` re-fires
				// the dispatch on the current outs list without the
				// SetFrameStore(nullptr)→SetFrameStore(fs) toggle —
				// avoids a tear-down/rebuild cycle of bound observers
				// (BridgeObserver lifecycle on the VFS side).  See
				// L6e-3 adversarial review P0.
				r->ReannounceFrameStore();
			}
		}
		return;
	}

	// Dim changed — release old, allocate new.
	if( mInteractiveFrameStore ) {
		mInteractiveFrameStore->release();
		mInteractiveFrameStore = 0;
	}

	FrameStoreOutput::FrameStoreSpec spec;
	spec.width    = width;
	spec.height   = height;
	// L8 round 10 — interactive uses an 8-pixel FrameStore tileEdge,
	// NOT the 32 the Job's production FrameStore uses.
	//
	// Why the divergence: `PixelBasedRasterizerHelper::RasterizeScene`
	// rounds the rasterizer's adaptive block size UP to a multiple of
	// the FrameStore's tileEdge (`AlignTileSizeToFrameStore`, round
	// 8) to prevent two workers from competing for the same
	// FrameStore tile.  With tileEdge=32, a preview-scale render at
	// 100x75 (1/8 of 800x600) gets only 4x3=12 blocks — fewer
	// cancellation checkpoints, and CenterOut leaves the outer ring
	// of tiles unrendered when the next pointer event cancels the
	// pass.  User-visible symptom: "only the centre of the image
	// manages to update; edges lag" — the "low resolution drop
	// downs" that pre-round-8 produced (small chunky tiles filling
	// the whole image quickly) disappear, replaced by larger tiles
	// that only the centre of the image has time to render.
	//
	// tileEdge=8 makes the alignment effectively a no-op for the
	// interactive path: `ComputeTileSize` already returns multiples
	// of 8, so the rounding doesn't change the rasterizer's block
	// size.  100x75 at tile=8 gives ~13x10=130 blocks, which
	// CenterOut spirals through quickly — when the user's next
	// pointer event cancels, far more of the image has rendered.
	// At this granularity the per-FrameStore-tile mutex still
	// prevents data races, but blocks are small enough that two
	// workers landing on the same FS tile briefly serialise (~8 px
	// of work) rather than waiting for a 32x32 block to finish.
	//
	// Production keeps tileEdge=32 — its renders run to completion
	// (no per-drag cancellation), and larger tiles amortise the
	// mutex / observer-dispatch overhead better at full image
	// resolutions.  The asymmetry is intentional.
	spec.tileEdge = 8;
	mInteractiveFrameStore = new Implementation::FrameStore( spec );
	// new returns refcount 1; that's our owned reference.

	Implementation::Rasterizer* r =
		dynamic_cast<Implementation::Rasterizer*>( mInteractiveRasterizer );
	if( r ) {
		r->SetFrameStore( mInteractiveFrameStore );
	}
}

void SceneEditController::DoOneRenderPass()
{
	if( !mInteractiveRasterizer )
	{
		// Test/skeleton mode — no rasterizer wired up.  Sleep briefly
		// so the cancel-restart timing in tests has a window.
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
		return;
	}

	// Install the cancellable progress and preview sink.
	mInteractiveRasterizer->SetProgressCallback( &mCancelProgress );
	if( mPreviewSink )
	{
		mInteractiveRasterizer->FreeRasterizerOutputs();
		mInteractiveRasterizer->AddRasterizerOutput( mPreviewSink );
	}

	const IScene* scene = mJob.GetScene();
	if( !scene ) return;

	// ----- Adaptive resolution scaling --------------------------------
	// While the user is mid-drag, render at a lower resolution so each
	// pass completes within ~33ms and the viewport stays responsive.
	// We swap the camera's frame dimensions before RasterizeScene and
	// restore them after, so:
	//   - The rasterizer allocates a smaller IRasterImage and runs
	//     1/scale^2 the pixel work.
	//   - The preview sink dispatches that smaller image; the platform
	//     widget upscales for display.
	//   - On pointer-up, mPreviewScale is reset to 1 (see OnPointerUp /
	//     OnTimeScrubEnd) and the next render delivers a sharp full-res
	//     frame.
	//
	// Adaptation: after each render we compare elapsed-ms against the
	// 30Hz target and bump the divisor up or down by 2× for the next
	// pass, clamped to [kPreviewScaleMin, kPreviewScaleMax].  Single-
	// step ramping prevents oscillation.
	const unsigned int scale = mPreviewScale.load( std::memory_order_acquire );

	// Refresh the full-res dims cache BEFORE any swap.  This is the
	// canonical moment when the camera is at its rest dims (the
	// previous pass restored them, and no swap has happened yet for
	// this pass).  A scene reload between passes is picked up here.
	// Bridges reading via GetCameraDimensions now see stable dims
	// independent of the swap window below.
	if( const IScene* scene = mJob.GetScene() ) {
		if( const IFilm* refFilm = scene->GetFilm() ) {
			mFullResW.store( refFilm->GetWidth(),  std::memory_order_release );
			mFullResH.store( refFilm->GetHeight(), std::memory_order_release );
		}
	}

	// Preview-scale path: mutate the existing Film's dims in place
	// for the duration of the pass, then mutate them back.  Uses
	// IScenePriv::ResizeFilm — zero allocation per frame, unlike
	// SetFilm which would allocate a fresh IFilm at every transition.
	// Scene::ResizeFilm re-syncs every camera's Frame too, so the
	// rasterizer's pixel grid and the cameras' projection matrices
	// stay in lockstep across the swap.
	//
	// RAII restore: the rest-dims must be restored even if the
	// rasterize call returns by an unusual path (currently only
	// cancellation; future bugs / std::bad_alloc / etc. shouldn't
	// strand Film at scaled dims forever, which would render the
	// viewport at low res after a single failed pass).
	// L8 round-18c — BUG FIX.  This RAII guard previously assigned
	// itself via aggregate-init syntax:
	//
	//     restoreGuard = { sp, restW, restH, restPAR, true };
	//
	// That expression constructs a TEMPORARY `FilmDimRestore`
	// (armed=true), copy-assigns it into `restoreGuard`, and then
	// destroys the temporary at end-of-full-expression.  The
	// temporary's destructor runs synchronously WHILE WE'RE STILL
	// IN THE SCALE-SWAP CODE, and because the temporary's `armed`
	// is true, it immediately calls `sp->ResizeFilm(restW, restH)`
	// — restoring the Film to the FULL-RES dims a single line
	// after we shrunk it.  `RasterizeScene` then reads pFilm at
	// full-res and renders the entire 800x600 image regardless of
	// `mPreviewScale`.
	//
	// User-visible symptom: low-res preview ladder never engages
	// during fast camera rotations; the user sees only CenterOut-
	// preempted high-res partials.  edit-diag confirmed scale=32
	// but raster-diag showed film=800x600 on every pass.
	//
	// Fix: set the guard's fields individually (no temporary).
	// Alternatively the struct could `=delete` its copy ops or
	// take an explicit Arm() method, but field-by-field is the
	// smallest surgical change.
	struct FilmDimRestore {
		IScenePriv*   sp;
		unsigned int  w, h;
		Scalar        pAR;
		bool          armed;
		~FilmDimRestore() { if( armed && sp ) sp->ResizeFilm( w, h, pAR ); }
	} restoreGuard{ nullptr, 0, 0, Scalar( 1 ), false };

	if( scale > 1 )
	{
		IScenePriv* sp = mJob.GetScene();
		if( sp )
		{
			const IFilm* curFilm = sp->GetFilm();
			if( curFilm )
			{
				const unsigned int restW   = curFilm->GetWidth();
				const unsigned int restH   = curFilm->GetHeight();
				const Scalar       restPAR = curFilm->GetPixelAR();
				const unsigned int sw = restW / scale > 0 ? restW / scale : 1;
				const unsigned int sh = restH / scale > 0 ? restH / scale : 1;
				sp->ResizeFilm( sw, sh, restPAR );
				// Arm the guard via field-by-field assignment — see
				// long comment above for why aggregate-init via
				// `restoreGuard = { ... }` was wrong.
				restoreGuard.sp    = sp;
				restoreGuard.w     = restW;
				restoreGuard.h     = restH;
				restoreGuard.pAR   = restPAR;
				restoreGuard.armed = true;
			}
		}
	}

	// L6e-3 — Make sure the interactive rasterizer has a FrameStore
	// at the CURRENT (post-scale-swap) dims so per-pixel writes
	// during `RasterizeScene` land in the canonical store.  Pre-L6e-3
	// the rasterizer's mFrameStore was the Job-allocated full-res
	// store; preview-scale renders fell back to mPersistentImage
	// (dim mismatch in `AcquireRenderImage`), starving direct VFS
	// observers.  Post-L6e-3, the interactive VFS observes
	// `mInteractiveFrameStore` and sees fresh content per pass —
	// see ViewportPreviewSink::OnRasterizerFrameStoreChanged in
	// RISEViewportBridge.mm.
	//
	// Post-master-merge — origin's camera-Film split moved per-pass
	// dims out of CameraCommon and into the scene-level Film
	// (`IScenePriv::ResizeFilm` mutates Film in place + re-syncs all
	// cameras).  The "current pass dims" is therefore the Film's
	// width/height post-ResizeFilm.
	if( const IScene* scene = mJob.GetScene() ) {
		const IFilm* curFilm = scene->GetFilm();
		const unsigned int passW = curFilm ? curFilm->GetWidth()  : 0;
		const unsigned int passH = curFilm ? curFilm->GetHeight() : 0;
		EnsureInteractiveFrameStore_( passW, passH );
	}

	const auto t0 = std::chrono::steady_clock::now();
	mInteractiveRasterizer->RasterizeScene( *scene, /*pRect*/0, /*seq*/0 );
	const auto elapsed = std::chrono::steady_clock::now() - t0;
	// restoreGuard's destructor runs at the end of this scope and
	// restores rest dims whether we exited normally, via cancellation,
	// or (hypothetically) via a propagated exception.

	// Adapt for the NEXT pass — only while the user is actively
	// dragging AND this wasn't an idle-refinement pass.  Skipping
	// the adaptation during refinement is critical: a refinement
	// pass at scale=2 might take 80ms (>kTargetMs) on a heavy
	// scene, and the during-motion adaptation would yo-yo the
	// scale right back up to 4 — which the next refinement tick
	// would walk back down — producing endless oscillation.
	// Refinement is the authority on scale once the user pauses;
	// the during-motion adaptation only runs while edits are
	// actively driving the loop.  At rest (pointer up), leave
	// scale at whatever OnPointerUp set (kPreviewScaleMin).
	//
	// Cancelled-pass handling.  A cancelled pass's elapsed time is
	// NOT a measurement of rasterizer speed — it's a measurement of
	// input cadence: how long after the pass started did the next
	// pointer event arrive.  At scale=1 on a 200ms-per-pass scene,
	// a 60Hz pointer stream cancels every pass at ~16ms, after only
	// the centre-out tile sequence has reached a handful of central
	// tiles.  If we treat that 16ms elapsed as "fast" and adapt
	// DOWN, we lock the loop into "scale=1, only the centre ever
	// updates during a continuous drag" — which is exactly what
	// users see if this gate is missing.
	//
	// So: only adapt DOWN on passes that COMPLETED.  Cancellation
	// itself is a "we're not keeping up" signal — bump scale UP one
	// level (when elapsed sits inside the target band; the slow
	// branches above already handle long elapsed times).
	const bool gestureActive =
		mPointerDown.load( std::memory_order_acquire )
	 || mScrubInProgress.load( std::memory_order_acquire );
	if( gestureActive && !mInRefinementPass )
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( elapsed ).count();
		const bool wasCancelled = mCancelProgress.IsCancelRequested();
		unsigned int next = scale;
		if( ms > kSlowMs && next < kPreviewScaleMax )
		{
			// Way over budget — jump two levels at once.  Cap at the
			// max so a 500ms scale-1 pass on a path-tracer doesn't
			// overflow.  Without this fast jump, going from 4 → 32
			// for a really heavy scene takes three slow frames, each
			// of which freezes the viewport for >100ms.
			next *= 4;
			if( next > kPreviewScaleMax ) next = kPreviewScaleMax;
		}
		else if( ms > kTargetMs && next < kPreviewScaleMax )
		{
			next *= 2;
		}
		else if( wasCancelled && next < kPreviewScaleMax )
		{
			// Pass got cancelled before completing the tile sequence
			// — outer tiles never painted.  See the long comment
			// above.  Bumping scale up is what eventually finds the
			// level where pass_time < input_cadence and the WHOLE
			// image refreshes between cancels.
			next *= 2;
			if( next > kPreviewScaleMax ) next = kPreviewScaleMax;
		}
		else if( !wasCancelled && ms < kFastMs && next > kPreviewScaleMin )
		{
			// Only step DOWN on a pass that actually completed — see
			// comment above.  Cancelled-pass elapsed times are
			// input-cadence measurements, not headroom signals.
			next /= 2;
		}
		if( next != scale )
		{
			mPreviewScale.store( next, std::memory_order_release );
		}
	}
}
