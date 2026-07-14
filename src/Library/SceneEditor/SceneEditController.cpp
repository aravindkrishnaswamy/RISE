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
#include "../Cst/Cst.h"                   // Shared-undo U1: DocFindByNameAnyRole / DocResolveNodeId (prior-value capture)
#include "../Utilities/Transformable.h"   // F6: CaptureTransformState at gizmo drag-start
#include "ObjectIntrospection.h"
#include "LightIntrospection.h"
#include "RasterizerIntrospection.h"
#include "FilmIntrospection.h"
#include "MaterialIntrospection.h"
#include "MediaIntrospection.h"
#include "PainterIntrospection.h"     // Entity-creation slice: Category::Painter property rows
#include "EntityTemplates.h"          // Entity-creation slice: Add-Entity template registry
#include "../Interfaces/IMaterialManager.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IScalarPainterManager.h"
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
#include "../Interfaces/IRasterImageWriter.h"  // Toolkit slice 1 — BufferRasterImageWriter adapter for CopyInteractiveFrame
#include "../Rendering/Rasterizer.h"  // L6e-3 — Implementation::Rasterizer for SetFrameStore
#include "../Scene.h"                 // concrete Scene (transitive scene-state includes); transactional rollback no longer uses CreateSnapshot/RestoreFromSnapshot
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/RuntimeContext.h"
#include <algorithm>      // Model-B F2 slice S2a: std::min for WaitForRenderJob's bounded wait_until slices
#include <chrono>
#include <cstdio>
#include <ctime>          // for time() used by CloneActiveCamera dedup fallback
#include <exception>      // Model-B F2 slice S2a: std::exception_ptr / current_exception / rethrow_exception
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

namespace {

// ===================== Render-camera override (Tier 2 / Direction B §5.5) ======
//
// The foundational mechanism for the non-destructive view features (B2 axis
// snaps, B1 named-view restore, B3 free-fly): render the interactive preview
// through a viewport-private OVERRIDE camera without mutating Scene::pActiveCamera
// and without touching production render.  The override is installed on the
// interactive rasterizer via PixelBasedRasterizerHelper::SetViewportCameraOverride
// (consulted at its RasterizeScene camera-read site) -- NOT by wrapping the IScene,
// because the caster stores + addrefs the scene across passes (a wrapper would
// dangle) and the render path downcasts the scene to IScenePriv/Scene (photon-map
// build, light-sampler regen) which a wrapper would defeat.  Overriding at the
// camera-read site keeps the REAL scene flowing to the caster.  The pose is
// realized into a standalone camera below.

// Realize a CameraSnapshot into a STANDALONE ICamera (NOT added to any manager),
// sized to the scene's current Film.  Parallel to CameraIntrospection::
// AddCameraFromSnapshot but using the RISE_API_Create*Camera factories (which yield
// a standalone, addref'd camera) instead of the manager-adding IJob::Add*Camera.
// Returns an addref'd ICamera (caller owns the ref) or nullptr on an unknown type /
// factory failure / no film.
ICamera* RealizeStandaloneCamera( const CameraSnapshot& s, const IScene& scene )
{
	const IFilm* film = scene.GetFilm();
	if( !film ) return nullptr;
	const unsigned int xres    = film->GetWidth();
	const unsigned int yres    = film->GetHeight();
	const Scalar       pixelAR = ( s.pixelAR > 0 ) ? Scalar( s.pixelAR ) : film->GetPixelAR();

	ICamera* pCam = nullptr;
	switch( s.type ) {
	case CameraSnapshot::Pinhole:
		RISE_API_CreatePinholeCamera( &pCam,
			Point3( s.location ), Point3( s.lookat ), Vector3( s.up ),
			s.fov, xres, yres, pixelAR, s.exposure, s.scanningRate, s.pixelRate,
			Vector3( s.orientation ), Vector2( s.target_orientation ),
			s.iso, s.fstop );
		break;
	case CameraSnapshot::ThinLens:
		RISE_API_CreateThinlensCamera( &pCam,
			Point3( s.location ), Point3( s.lookat ), Vector3( s.up ),
			s.sensorSize, s.focalLength, s.fstop, s.focusDistance, s.sceneUnitMeters,
			xres, yres, pixelAR, s.exposure, s.scanningRate, s.pixelRate,
			Vector3( s.orientation ), Vector2( s.target_orientation ),
			s.apertureBlades, s.apertureRotation, s.anamorphicSqueeze,
			s.tiltX, s.tiltY, s.shiftX, s.shiftY, s.iso );
		break;
	case CameraSnapshot::Fisheye:
		RISE_API_CreateFisheyeCamera( &pCam,
			Point3( s.location ), Point3( s.lookat ), Vector3( s.up ),
			xres, yres, pixelAR, s.exposure, s.scanningRate, s.pixelRate,
			Vector3( s.orientation ), Vector2( s.target_orientation ),
			s.fisheyeScale );
		break;
	case CameraSnapshot::Orthographic:
		RISE_API_CreateOrthographicCamera( &pCam,
			Point3( s.location ), Point3( s.lookat ), Vector3( s.up ),
			xres, yres, Vector2( s.viewportScale ), pixelAR,
			s.exposure, s.scanningRate, s.pixelRate,
			Vector3( s.orientation ), Vector2( s.target_orientation ) );
		break;
	default:
		return nullptr;
	}
	return pCam;   // addref'd by the factory (or nullptr on failure)
}

}  // namespace

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
, mSuppressInitialRender( false )
, mRendering( false )
, mSaving( false )
, mAgentRenderBlocksInteractive( false )
, mCancelCount( 0 )
, mRenderCount( 0 )
// Model-B F2 slice S2a fix: every mint site does `mNextRenderJobId +=
// kControllerRenderJobIdStride` BEFORE using the result (see
// RunPreviewRenderParked / RenderLoop) -- so starting the counter AT the
// stride made the FIRST minted id stride*2 (4), not stride (2), silently
// contradicting the class's own documented "starts at 2" contract (see
// kControllerRenderJobIdStride's doc + RenderJobId's doc).  Starting one
// stride EARLY (0) makes the first `+= stride` land exactly on
// kControllerRenderJobIdStride (2), matching the doc.  0 is safe here
// specifically because it is ALSO kInvalidRenderJobId -- this bare 0 is
// never itself handed out as a job id (every consumer reads the POST-
// increment value), so there is no collision with the "no id assigned"
// sentinel.
, mJobStatusMutex()
, mNextRenderJobId( 0 )
, mCurrentRenderJob()
, mAgentRenderThread()
, mAgentRenderCV()
, mAgentRenderDoneCV()
, mAgentRenderStop( false )
, mAgentRenderPending( false )
, mAgentRenderFn()
, mAgentRenderJobId( kInvalidRenderJobId )
, mAgentRenderPinned( false )
, mAgentRenderException()
, mAgentRenderNextTicket( 0 )
, mAgentRenderServingTicket( 0 )
, mAgentRenderWaitingSyncCount( 0 )
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

	// Model-B F2 slice S2a: spawn the dedicated agent-render worker HERE
	// (constructor), not in Start() -- it is independent of the
	// interactive render thread's Start()/Stop() lifecycle (a caller may
	// submit an agent render before ever calling Start(), or after
	// Stop()'d the interactive loop; the two threads are orthogonal
	// -- the worker only ever contends with the interactive loop through
	// the SAME mMutex/CancelAndParkRender_ critical section, exactly
	// like RunPreviewRenderParked already does from whatever thread
	// calls it).  LONG-LIVED: started once here, joined once in Stop()
	// (which ~SceneEditController calls unconditionally), so any future
	// one-time-per-thread initialization a submitted `fn` might rely on
	// runs exactly once for the life of this controller.
	mAgentRenderThread = std::thread( &SceneEditController::AgentRenderWorkerLoop_, this );
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
	// NOTE (2026-07-12): deliberately NOT scrubbing the Job's progress slot here (e.g. via
	// mJob.ClearProgressIfCurrent( &mCancelProgress )) -- this dtor CANNOT touch mJob: several
	// owners (the test suites' `controller.Stop(); pJob->release();` pattern, with the stack
	// controller destroyed after the release) legitimately destroy the Job first, so a scrub here
	// is a use-after-free on mJob itself (SIGSEGV, caught by AgentRenderAsyncTest when it was
	// tried).  The original motivation -- a stale &mCancelProgress left installed by
	// RunProductionRenderComposed's outside-the-slot prior-capture -- was CLOSED the same day by
	// the capture-inside-the-slot redesign (the composed closure now ExchangeProgress-captures its
	// restore value inside the coordinator turn); the only residual way a stale &mCancelProgress
	// survives in the slot is an exception escaping a restore itself (logged loudly in both
	// restore guards: RunProductionRenderComposed's ProgressRestoreGuard here and
	// AgentSession::RenderCore_'s in AgentSession.cpp).
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
	// Free-fly override camera (Tier 2 §5.5).  Stop() joined the render thread
	// above, so no DoOneRenderPass is reading it.
	if( mViewportOverrideCamera )
	{
		mViewportOverrideCamera->release();
		mViewportOverrideCamera = 0;
	}
	// Inverse-edit rollback holds NO snapshot — a transaction left open
	// at teardown needs no resource release; the (uncommitted) live edits
	// simply remain, exactly as they would after a commit.
}

// Lifecycle -----------------------------------------------------------

void SceneEditController::Start( bool suppressInitialRender )
{
	// Review-round-1 P1: serialize against StopInteractive()'s join so a
	// concurrent Resume/Start can never move-assign onto a still-joinable
	// mRenderThread (std::terminate) -- see mLifecycleMutex's doc.
	std::lock_guard<std::mutex> lifecycleLk( mLifecycleMutex );

	bool expected = false;
	if( !mRunning.compare_exchange_strong( expected, true ) )
	{
		return;  // already running
	}

	// Any Start un-pauses — see PauseRefinement()'s declaration doc.  A
	// paused viewport that is being (re)started is genuinely live again,
	// and a stale paused flag would make GetRefinementStatus lie.
	mRefinementPaused.store( false, std::memory_order_release );

	// Record the one-shot "skip the initial render" request before the
	// render thread is spawned, so RenderLoop observes it on entry.
	// std::thread construction below is a synchronization point, so the
	// release store is visible to the new thread without further
	// fencing.  Stored AFTER the running-CAS so a no-op Start() on an
	// already-running controller can't clobber the flag.
	mSuppressInitialRender.store( suppressInitialRender,
	                              std::memory_order_release );

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

void SceneEditController::StopInteractive()
{
	// Review-round-1 P1: see Start() / mLifecycleMutex's doc.
	std::lock_guard<std::mutex> lifecycleLk( mLifecycleMutex );

	bool expected = true;
	if( !mRunning.compare_exchange_strong( expected, false ) )
	{
		return;  // interactive loop was not running
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

void SceneEditController::PauseRefinement()
{
	// Set the flag FIRST so a status poll during the join below already
	// reads Paused; harmless when StopInteractive turns out to be a
	// no-op (not running) — Resume/Start will clear it.
	mRefinementPaused.store( true, std::memory_order_release );
	StopInteractive();
}

void SceneEditController::ResumeRefinement()
{
	if( !mRefinementPaused.load( std::memory_order_acquire ) )
	{
		return;  // wasn't paused
	}
	// Start() clears the flag itself (any Start un-pauses) and is
	// idempotent, so no exchange dance is needed here.  A normal
	// (un-suppressed) start repaints promptly; the idle-refinement
	// ladder then walks back to full quality.
	Start( false );
}

bool SceneEditController::IsRefinementPaused() const
{
	return mRefinementPaused.load( std::memory_order_acquire );
}

SceneEditController::RefinementPhase
SceneEditController::GetRefinementStatus( unsigned int& outScaleDivisor ) const
{
	outScaleDivisor = mPreviewScale.load( std::memory_order_acquire );
	if( mRefinementPaused.load( std::memory_order_acquire ) )
	{
		return RefinementPhase::Paused;
	}
	const PolishState polish =
		static_cast<PolishState>( mPolishState.load( std::memory_order_acquire ) );
	const bool rendering = mRendering.load( std::memory_order_acquire );
	if( rendering )
	{
		if( polish == PolishState::PolishQueued )
		{
			return RefinementPhase::Polishing;
		}
		return outScaleDivisor > kPreviewScaleMin ? RefinementPhase::Refining
		                                          : RefinementPhase::Rendering;
	}
	if( outScaleDivisor > kPreviewScaleMin )
	{
		return RefinementPhase::Refining;   // ladder still walking down
	}
	if( polish != PolishState::None )
	{
		return RefinementPhase::Polishing;  // polish queued, pass not yet started
	}
	return RefinementPhase::Idle;
}

void SceneEditController::SetInteractiveRegion( unsigned int left, unsigned int top,
                                                unsigned int right, unsigned int bottom )
{
	// Refuse a degenerate box rather than silently swapping — the
	// bridges send normalized coords.
	if( right < left || bottom < top )
	{
		return;
	}
	const std::uint64_t kMax16 = 0xffffu;
	const std::uint64_t l = left   < kMax16 ? left   : kMax16;
	const std::uint64_t t = top    < kMax16 ? top    : kMax16;
	const std::uint64_t r = right  < kMax16 ? right  : kMax16;
	const std::uint64_t b = bottom < kMax16 ? bottom : kMax16;
	mInteractiveRegionPacked.store( ( l << 48 ) | ( t << 32 ) | ( r << 16 ) | b,
	                                std::memory_order_release );
	mInteractiveRegionActive.store( true, std::memory_order_release );
	// Start refining inside the box right away.
	KickRender();
}

void SceneEditController::ClearInteractiveRegion()
{
	if( !mInteractiveRegionActive.exchange( false, std::memory_order_acq_rel ) )
	{
		return;
	}
	// Repaint full-frame so pixels outside the (former) box catch up
	// with any edits that landed while the region restricted passes.
	KickRender();
}

bool SceneEditController::GetInteractiveRegion( unsigned int& left, unsigned int& top,
                                                unsigned int& right, unsigned int& bottom ) const
{
	if( !mInteractiveRegionActive.load( std::memory_order_acquire ) )
	{
		return false;
	}
	const std::uint64_t packed = mInteractiveRegionPacked.load( std::memory_order_acquire );
	left   = static_cast<unsigned int>( ( packed >> 48 ) & 0xffffu );
	top    = static_cast<unsigned int>( ( packed >> 32 ) & 0xffffu );
	right  = static_cast<unsigned int>( ( packed >> 16 ) & 0xffffu );
	bottom = static_cast<unsigned int>(   packed         & 0xffffu );
	return true;
}

bool SceneEditController::InteractiveRasterizerHonorsRegion() const
{
	return mInteractiveRasterizer ? mInteractiveRasterizer->HonorsRegion() : false;
}

String SceneEditController::UndoLabel() const
{
	return String( mEditor.History().LabelForUndo() );
}

String SceneEditController::RedoLabel() const
{
	return String( mEditor.History().LabelForRedo() );
}

void SceneEditController::Stop()
{
	// Model-B F2 slice S4 fix round 5: THE ORDER BELOW IS LOAD-BEARING --
	// agent-worker retirement MUST run to completion BEFORE
	// StopInteractive()'s interactive teardown, not after.
	//
	// Fix round 4 put StopInteractive() first and the agent-worker
	// retirement second, on the (incorrect) claim that this preserved
	// the pre-split monolithic order.  It did not: the monolithic Stop()
	// always retired the agent worker FIRST (stop flag -> notifies ->
	// CancelAgentRender_ -> join) and only THEN did the interactive
	// teardown (cancel + mMutex notify + join mRenderThread).  With
	// StopInteractive() first, its RequestCancel() (which shares
	// mCancelProgress with the agent path) can abort the slot's
	// occupying render early, freeing the slot, BEFORE mAgentRenderStop
	// is set -- so a SubmitAgentRenderSync caller already queued on the
	// fairness ticket can see "slot free && my turn" go true and proceed
	// to run a FULL render during teardown instead of observing the stop
	// flag and refusing honestly.  This was caught by
	// RunQueuedSyncWaiterUnblocksOnStopTest (test (h) in
	// tests/AgentRenderAsyncTest.cpp): with the round-4 order, the
	// queued waiter's eventual result was a stray success instead of the
	// expected honest refusal, deterministically (3/3 on a quiet
	// machine).  Restoring "agent worker first, then StopInteractive()"
	// makes the queued waiter observe mAgentRenderStop before the
	// interactive cancel can ever free the slot it's waiting on, exactly
	// as the original monolithic Stop() guaranteed.
	//
	// Model-B F2 slice S2a: the agent-render worker is INDEPENDENT of
	// mRunning/the interactive loop's Start()/Stop() lifecycle (it is
	// spawned unconditionally in the ctor -- see that comment), so its
	// teardown must NOT be gated behind StopInteractive()'s mRunning CAS,
	// or a caller that never called Start() (headless-controller
	// agent-only use) would leak the joinable thread past
	// ~SceneEditController's Stop() call.  Guarded instead by
	// mAgentRenderThread.joinable(), which is false after the first
	// Stop() -- so a second Stop() call (or the dtor's unconditional
	// Stop()) is a correct no-op here too, same idempotency shape as the
	// mRenderThread teardown inside StopInteractive().
	// Same lost-wakeup discipline as the interactive-loop teardown:
	// trip the stop flag, hold the lock the WORKER ACTUALLY WAITS ON
	// around the notify so a worker between its predicate check and the
	// kernel park cannot miss it.
	//
	// Fix-round-1 P1-1: this used to lock mMutex here -- but
	// AgentRenderWorkerLoop_'s wait (mAgentRenderCV.wait) blocks on
	// mAgentRenderSlotMutex, NOT mMutex (see that CV's member doc).
	// Holding the WRONG mutex around the notify gives no lost-wakeup
	// guarantee at all for THIS wakeup (C++ only promises no missed
	// wakeup when the notifier holds the SAME mutex the waiter blocks
	// on) -- a worker between its predicate check and the kernel park
	// could miss the notify and this Stop() would then hang forever on
	// the join() below whenever that race actually landed.  Also flip
	// the stop flag AFTER taking the slot lock so a submission that is
	// mid-check (holding mAgentRenderSlotMutex, about to test the flag)
	// is strictly ordered against this store.
	{
		std::lock_guard<std::mutex> slotLk( mAgentRenderSlotMutex );
		mAgentRenderStop.store( true, std::memory_order_release );
	}
	mAgentRenderCV.notify_all();
	// Fix-round-1 P2-C: cancel an IN-FLIGHT agent render so Stop() does
	// not stall for the render's full natural duration -- mirrors the
	// interactive loop's own "trip cancel before joining" idiom in
	// StopInteractive() below.  Harmless no-op if no agent render is in
	// flight (or if the worker hasn't installed the progress hook this
	// cancels -- see CancelAgentRender_'s doc).
	CancelAgentRender_();
	// Fix-round-1 P1-1: also wake any SubmitAgentRenderSync callers
	// currently queued on the fairness ticket (mAgentRenderDoneCV) --
	// their wait predicate now ALSO checks mAgentRenderStop (see
	// SubmitAgentRenderSync), so this notify lets a sync waiter that was
	// queued before Stop() unblock with an honest refusal instead of
	// waiting out its full timeoutMs.  Because this runs BEFORE
	// StopInteractive() below, the waiter observes mAgentRenderStop no
	// later than it observes the slot freeing up -- see the load-bearing
	// order note at the top of this function.
	{
		std::lock_guard<std::mutex> slotLk( mAgentRenderSlotMutex );
	}
	mAgentRenderDoneCV.notify_all();
	if( mAgentRenderThread.joinable() )
	{
		mAgentRenderThread.join();
	}

	// Model-B F2 slice S4 fix round 4: the interactive-loop half lives in
	// StopInteractive() (see its header doc) so a platform shell that
	// only wants to pause the viewport ahead of a production render can
	// do so WITHOUT permanently retiring the agent-render worker above.
	// Full Stop() does both, agent worker FIRST as established above,
	// THEN the interactive teardown -- see the load-bearing order note
	// at the top of this function for why the order (not just the union
	// of the two teardowns) matters.
	StopInteractive();
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
	if( mEditor.HasPendingCstObjectTransforms() || mEditor.HasPendingCstCameraPose() ) {
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		// INVARIANT: the object and camera pending sets are MUTUALLY EXCLUSIVE here -- a gesture composite is
		// single-tool ("Drag" object-motion XOR "Camera" camera-motion) and every commit boundary drains BOTH
		// sets, so they are never simultaneously populated.  That matters because the object commit may take a D2
		// (ClearAll + re-derive), which would rebuild the live CAMERA from the Document's pre-drag pose -- if a
		// camera pose were ALSO pending, the camera commit below would then read+record that reset pose.  Should a
		// future composite ever span both categories, snapshot BOTH (object matrices + camera pose strings) BEFORE
		// routing EITHER (CommitPendingCstObjectTransforms already snapshots its matrices first; the camera commit
		// would need the same).
		//
		// A2: OnPointerUp is a `void` gesture-end callback with no caller to report a bool to, so the commit
		// returns are legitimately ignored here (unlike SetProperty, which now propagates them) -- a route failure
		// is already logged by the commit itself (SceneEditor::CommitPendingCstObjectTransforms /
		// CommitPendingCstCameraPose), and the render kick below repaints the live scene regardless via
		// `mEditPending` set unconditionally after this block.
		mEditor.CommitPendingCstObjectTransforms();   // object gizmo drag -> `matrix` param
		mEditor.CommitPendingCstCameraPose();          // camera orbit/pan/zoom/roll -> pose params
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
	case Cat::Painter: {
		// Entity-creation slice: check BOTH painter pipes (the union
		// CategoryEntityName draws from) -- a name resolving in either
		// manager means the selection still points at a live entity.
		IJobPriv& jobRef = const_cast<IJobPriv&>( job );
		if( IPainterManager* pm = jobRef.GetPainters() ) {
			if( pm->GetItem( name.c_str() ) != 0 ) return true;
		}
		if( IScalarPainterManager* spm = jobRef.GetScalarPainters() ) {
			if( spm->GetItem( name.c_str() ) != 0 ) return true;
		}
		return false;
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
	//
	// A2: Undo() reports `didWork` -- whether history moved -- not whether the CST recording of that move
	// succeeded; those are different questions (a partial CST-commit failure here still means the LIVE undo
	// happened and the caller should treat it as "did work"). A commit failure is logged by the commit itself.
	// BE PRECISE about the consequence (the two failure shapes have OPPOSITE Document states): code 0 means the
	// Document did NOT record the undone transform, and a later full re-derive (D2) rebuilds the live scene FROM
	// the Document -- i.e. the D2 REVERTS the live undo rather than recording it (the divergence resolves in the
	// data-losing direction); code 3 means the Document DID record it but the re-derive diagnosed. Ignoring the
	// return here is the same known, accepted limitation OnPointerUp carries for the gesture-end commit -- an
	// oversight it is not, but neither is a later D2 a safe recovery path.
	if( mEditor.HasPendingCstObjectTransforms() ) mEditor.CommitPendingCstObjectTransforms();
	if( mEditor.HasPendingCstCameraPose() ) mEditor.CommitPendingCstCameraPose();
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
	//
	// A2: same rationale as Undo above -- `didWork` is about history motion (redo advanced), not about whether
	// this CST recording succeeded; a commit failure is logged by the commit itself and is a known, accepted
	// limitation, consistent with OnPointerUp and Undo. See Undo's comment for the precise per-code consequence
	// (code 0: a later D2 REVERTS the unrecorded live transform; code 3: recorded but diagnosed).
	if( mEditor.HasPendingCstObjectTransforms() ) mEditor.CommitPendingCstObjectTransforms();
	if( mEditor.HasPendingCstCameraPose() ) mEditor.CommitPendingCstCameraPose();
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
	// Design brief A4 (review-round-1 P1): a region NEVER leaks into or
	// past a production render.  This is the SECOND live production
	// entry point (the Mac shell's -requestProductionRender lands here;
	// the composed path goes through SubmitProductionRenderSync, which
	// clears too) — both must uphold the invariant or the post-render
	// interactive restart refines only a stale box.  Kick-free store:
	// the interactive loop is being stopped right below anyway.
	mInteractiveRegionActive.store( false, std::memory_order_release );

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
		// Round-2 P2: don't silently un-pause — a Pause that raced this
		// production render would otherwise be clobbered by the restart
		// (Start() clears mRefinementPaused).  Mirrors ResumeRefinement.
		if( wasRunning && !IsRefinementPaused() ) Start();
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

	// Round-2 P2: same paused-guard as the early-exit restart above.
	if( wasRunning && !IsRefinementPaused() ) Start();
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
	// stack itself, explicitly, only on a full revert.  ONE DirtyTracker-layer
	// asymmetry does ride along: DirtyTracker::RestoreState OR-merges the CST-head
	// boolean instead of plain-copying it (see the F7 doc in DirtyTracker.h), so
	// a future non-rollback consumer cannot use this primitive to force that flag
	// back to clean.
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
	//
	// A2: unlike OnPointerUp/Undo/Redo (void or history-motion-scoped returns), RollbackTransaction already
	// reports an honest `fullyReverted` bool for partial reverts (trimmed history, a target entity gone) -- a
	// commit failure here belongs in that SAME honesty contract: the commit did not CLEANLY record the revert
	// (code 0: not recorded at all -- a later D2 would replay the stale Document over the reverted live scene;
	// code 3: recorded but the re-derive diagnosed), which is exactly the not-fully-reverted condition
	// `fullyReverted` exists to report. Fold it in via `&=` (both commits still run unconditionally; a route failure is
	// independently logged by the commit itself).
	if( mEditor.HasPendingCstObjectTransforms() ) fullyReverted &= mEditor.CommitPendingCstObjectTransforms();
	if( mEditor.HasPendingCstCameraPose() ) fullyReverted &= mEditor.CommitPendingCstCameraPose();
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

// Entity-creation slice: Category::Painter's entity list is the UNION of
// the two painter pipes (IPainterManager colour painters +
// IScalarPainterManager physical-scalar painters -- see CLAUDE.md's
// IScalarPainter refactor note: material slots route by physical meaning,
// not syntactic resemblance, so a scene legitimately has same-named-or-not
// entries in both managers).  Sorted stably so index-by-index enumeration
// (CategoryEntityCount then repeated CategoryEntityName(idx)) is
// deterministic across calls even though neither manager's own
// EnumerateItemNames documents a stable order.
std::vector<String> CollectPainterUnionNames( IJobPriv& job )
{
	CollectNamesCallback cb;
	if( IPainterManager* m = job.GetPainters() ) m->EnumerateItemNames( cb );
	if( IScalarPainterManager* m = job.GetScalarPainters() ) m->EnumerateItemNames( cb );
	std::stable_sort( cb.names.begin(), cb.names.end(),
		[]( const String& a, const String& b ) { return std::string( a.c_str() ) < std::string( b.c_str() ); } );
	return cb.names;
}

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
	case Category::Painter: {
		return static_cast<unsigned int>( CollectPainterUnionNames( mJob ).size() );
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
	case Category::Painter: {
		const std::vector<String> names = CollectPainterUnionNames( mJob );
		if( idx >= names.size() ) return String();
		return names[idx];
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
	case Category::Painter:
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

					// CST-route the preset (Model-B P5): record width+height in the retained Document so a SAVE /
					// future D2 preserves the new resolution (the live SetFilm above already mutated the scene).
					// pixelAR is UNCHANGED by a preset (preserved above) -> pass nullptr (no diff to that param).
					// Read the LIVE post-SetFilm dims (authoritative, post-clamp).  A Document-record failure is a
					// SOFT warning logged inside ApplyCstFilmEdit; it must NOT fail the preset (the live edit stands).
					if( mJob.HasRetainedCstDocument() ) {
						char wStr[64]; char hStr[64];
						std::snprintf( wStr, sizeof(wStr), "%u", filmAfter->GetWidth() );
						std::snprintf( hStr, sizeof(hStr), "%u", filmAfter->GetHeight() );
						mJob.ApplyCstFilmEdit( wStr, hStr, nullptr );
					}
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

// Cancel-and-park the render thread.  Caller holds mMutex via `lk`.  This
// is the single source of truth for the "trip cancel + wait for the pass to
// drain" idiom that every scene-mutating edit path inlines; keeping it in
// one place means a new caller (the Facet-5 agent commit) cannot drift from
// the established park semantics (the same acquire-order predicate the
// SetProperty branches use).
void SceneEditController::CancelAndParkRender_( std::unique_lock<std::mutex>& lk )
{
	if( mRendering.load( std::memory_order_acquire ) )
	{
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
}

// Fix-round-1 P2-C: trip the cancel signal for whatever is currently
// running -- the interactive pass (mRendering) if any, AND (unconditionally,
// since we have no direct visibility here into "is the agent worker's fn
// actually calling Rasterize() right now") mCancelProgress itself, which
// AgentRenderProgress() hands to the agent-render fn as its Job progress
// callback.  This does NOT take mMutex and does NOT wait -- a caller that
// needs "cancelled AND drained" calls this THEN separately waits.
//
// Round-2 P3-a doc fix: the claim this replaced ("does not linger and
// false-cancel a LATER, unrelated render... DrainAsyncRender_ detaches
// before any further submission is possible") is WRONG for the
// AgentSession::DrainAsyncRender_ caller -- that is exactly the round-2
// P1-2 bug (a stale, unpublished mAsyncOutstandingJobId could survive a
// fast render's completion and cause a LATER drain to call this method
// against a controller now running a wholly unrelated render).  Narrowed
// to what is actually true:
//   * Stop() caller: safe by construction.  This call trips the SAME flag
//     RenderLoop's next-pass Reset() clears at pass-start, and Stop() sets
//     mAgentRenderStop (under mAgentRenderSlotMutex, before this call) so
//     no NEW agent render can be submitted afterward (SubmitAgentRenderAsync
//     / SubmitAgentRenderSync both refuse post-Stop()) -- there is no
//     later render for a lingering cancel to false-trip.
//   * AgentSession::DrainAsyncRender_ caller (post round-2 P1-2 fix): safe
//     for a DIFFERENT reason -- DrainAsyncRender_ only reaches this call
//     when mAsyncOutstandingJobId is genuinely nonzero, and P1-2's
//     publish-before-clear ordering (RenderAsync holds mAsyncCacheMutex
//     across both SubmitAgentRenderAsync and the id publish) guarantees
//     that id can no longer go stale-while-still-set the way it could
//     before the fix -- so a drain's cancel targets a render that is
//     either genuinely still outstanding (correctly cancelled) or has
//     already been retired (comparing-cleared by the worker's
//     OutstandingGuard), never a DIFFERENT later render wearing the old
//     id.
//   * AgentSession::CancelAsyncRender caller (Model-B F2 slice S2b,
//     THIRD caller): fire-and-forget from the Swift chat driver's
//     `render_cancel` RPC verb (ChatViewModel.cancelAnyOutstandingChatRender),
//     fired whenever a NEWER intent (Stop, a production render starting,
//     the scene closing, a provider switch) needs to end an outstanding
//     chat-driven agent render without waiting behind it.  Unlike the
//     other two callers, this one does NOT follow up with a wait for the
//     same job id -- it trips the flag and returns immediately (see
//     AgentSession::CancelAsyncRender's and the RPC handler's own docs).
//     Safe by the SAME broad argument that makes this method safe to call
//     on an idle controller at all: at worst, a stale/racing cancel here
//     trips mCancelProgress for whatever happens to be running RIGHT NOW
//     -- which, in the fire-and-forget case, is either the render this
//     call actually meant to cancel, or (in a narrow race window) some
//     OTHER in-flight pass that starts an instant later. Cancelling an
//     unrelated in-flight pass is routine self-healing churn in this
//     codebase, not a correctness bug: the interactive viewport already
//     calls RequestCancel() on every edit, and every rasterizer pass
//     (interactive or agent) is written to restart cleanly after a
//     cancel. So the worst case from this third caller is "a render
//     restarts one pass sooner than it otherwise would have" -- the same
//     harmless outcome the Stop()/DrainAsyncRender_ callers rely on,
//     just without their own immediate follow-up wait.  Best practice for
//     a FUTURE caller that DOES need "cancelled AND drained" remains
//     "cancel immediately followed by a wait for the SAME job id", exactly
//     as the first two callers already do (Stop() joins the worker
//     thread; DrainAsyncRender_ calls WaitForRenderJob, unbounded --
//     round-2 P1-1); a fire-and-forget caller like this third one is
//     explicitly opting OUT of that guarantee because its use case
//     (an interrupting newer intent) doesn't need it.
void SceneEditController::CancelAgentRender_()
{
	mCancelProgress.RequestCancel();
}

namespace {

// Shared-undo U1: the value of a chunk's first `pname` param, mirroring the exact CST tree shape ParseChunk
// builds (a Param kid whose first Token kid is the pname/role and whose subsequent kids are the pvalue Token(s)
// interleaved with inter-value Trivia) -- this is the SAME shape Job.cpp's file-local S2ChunkParamValue walks;
// duplicated here (rather than exposed from Job.cpp) because it is a pure Cst::Node tree read with no Job state
// dependency, and the controller already holds a `const RISE::Cst::Document*` via IJob::GetCstDocument().
// `*outPresent` reports whether the param was found AT ALL (distinct from "found but empty" -- occ=0 only,
// matching the agent path's fixed occ=0 convention).
//
// P1-1 fix (round 1): a multi-token value (e.g. `color 1 1 1`, a vector/tuple) is SEVERAL pvalue Token kids, one
// per whitespace-separated token, each preceded by an inter-value Trivia kid holding the separating whitespace
// (see the anonymous-namespace ParseChunk loop in Cst.cpp ~150-161). The old walk kept only the FIRST pvalue
// token ("1" out of "1 1 1"), so an Undo of a multi-token param re-set it to a truncated single-component value
// (measured: a `color 1 1 1` emitter param round-tripped through capture+undo landed as r=5 g=0 b=0 instead of
// 5 5 5). Cst::ParamNodeValue (Cst.cpp ~183, file-local to that TU) is the correct accessor -- it is NOT
// reachable from here (anonymous-namespace / not declared in Cst.h) -- so this walk is rewritten to match its
// join semantics EXACTLY, token-by-token: once the first pvalue Token is seen, every subsequent kid's `text`
// (Trivia AND Token alike) is appended verbatim until the Param node ends, reproducing "1 1 1" (not "111" or
// "1") for a 3-token value.
std::string AgentReadFirstParamValue( const RISE::Cst::NodeRef& chunk, const char* pname, bool* outPresent )
{
	if( outPresent ) *outPresent = false;
	if( !chunk ) return std::string();
	for( const auto& kid : chunk->kids )
	{
		if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
		std::string nm, val;
		bool inVal = false;
		for( const auto& tk : kid->kids )
		{
			if( !tk ) continue;
			if( !inVal && tk->kind == RISE::Cst::NodeKind::Token && tk->role == "pname" && nm.empty() ) { nm = tk->text; continue; }
			if( !inVal && tk->kind == RISE::Cst::NodeKind::Token && tk->role == "pvalue" ) inVal = true;
			if( inVal ) val += tk->text;
		}
		if( nm == pname )
		{
			if( outPresent ) *outPresent = true;
			return val;
		}
	}
	return std::string();
}

}  // namespace

// Shared-undo U1: capture the CURRENT value of `param` on the entity resolved by (entityName, entityKind) from
// the retained CST Document -- BEFORE an agent edit mutates it.  `*outPresent` = false means the param is
// ABSENT (a defaulted slot the scene text omits; the coming edit will INSERT it, so the inverse is a REMOVE, not
// a re-SET -- see SceneEdit::prevValueWasAbsent).  Resolution uses the SAME DocFindByNameAnyRole call and
// camera-unique-fallback rule as Job::ApplyCstParamEditImpl_, so a value read here always matches whatever the
// upcoming ApplyCstParamEditChecked call will resolve against (both run under the SAME mMutex hold in
// ApplyAgentParamEdit -- no TOCTOU between the read and the apply).  Returns false (no capture) if there is no
// retained Document or the entity does not resolve -- callers must not push a history record in that case (the
// caller's ApplyCstParamEditChecked will independently reject with code 0, so no mutation happens either).
bool SceneEditController::CaptureAgentPriorParamValue_(
	const String& entityName, const String& entityKind, const String& param,
	String& outPrevValue, bool& outWasAbsent )
{
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return false;
	const bool uniqueFallback = ( entityKind.size() > 1 && std::string( entityKind.c_str() ) == "camera" );
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, entityName.c_str(), nullptr, entityKind.size() > 1 ? entityKind.c_str() : "", uniqueFallback );
	if( id == 0 ) return false;
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return false;
	bool present = false;
	const std::string val = AgentReadFirstParamValue( chunk, param.c_str(), &present );
	outWasAbsent = !present;
	outPrevValue = present ? String( val.c_str() ) : String();
	return true;
}

// Shared-undo U2: capture the exact bytes + top-level index of the chunk `ApplyAgentRemoveChunk` is about to
// erase -- BEFORE the coming `Job::ApplyCstRemoveChunk` call mutates the Document.  Resolution mirrors Job's
// own (DocFindByNameAnyRole with the SAME camera-unique-fallback rule), so the captured chunk is guaranteed to
// be the one the remove is about to act on (both run under the SAME mMutex hold in ApplyAgentChunkCrud_ -- no
// TOCTOU).  `outBytes` captures the chunk's own text PLUS the immediately-following top-level item (if any) --
// unconditionally, regardless of whether DocEraseChunkTidy will actually end up tidying that neighbour away --
// then the CALLER (ApplyAgentChunkCrud_, after the real remove has run) trims it down to only the items the
// erase ACTUALLY dropped, by diffing the Document's item count before vs after.  Returns false (no capture) if
// there is no retained Document or the target does not resolve -- the caller's ApplyCstRemoveChunk will
// independently reject with a negative/0 code, so no mutation happens either.
bool SceneEditController::CaptureAgentChunkForRemoveUndo_(
	const String& target, const String& kind,
	String& outBytes, int& outIndex, bool& outWasRasterizer )
{
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return false;
	const bool uniqueFallback = ( kind.size() > 1 && std::string( kind.c_str() ) == "camera" );
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, target.c_str(), nullptr, kind.size() > 1 ? kind.c_str() : "", uniqueFallback );
	if( id == 0 ) return false;
	const int idx = RISE::Cst::DocIndexOfNodeId( *doc, id, nullptr );
	if( idx < 0 ) return false;
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return false;

	std::string bytes = RISE::Cst::SerializeNode( chunk );
	const RISE::Cst::NodeRef next = RISE::Cst::DocResolveNodeId( *doc, RISE::Cst::DocNodeIdAt( *doc, idx + 1 ) );
	if( next ) bytes += RISE::Cst::SerializeNode( next );   // conservative over-capture; trimmed post-remove by the caller

	outBytes  = String( bytes.c_str() );
	outIndex  = idx;
	const std::string& role = chunk->role;
	outWasRasterizer = ( role.size() > 11 && role.compare( role.size() - 11, 11, "_rasterizer" ) == 0 );
	return true;
}

// Facet 5 slice 1b: the render-thread-SAFE agent commit.  See the header for
// the contract.  The ENTIRE ApplyCstParamEdit + rebind + version-bump runs
// under mMutex while the render thread is parked, so no reader can observe
// the D2 transient {0,0} head-version or a half-rebuilt Scene.  Callable from
// any thread.
SceneEditController::AgentCommitResult SceneEditController::ApplyAgentParamEdit(
	const String& entityName,
	const String& entityKind,
	const String& param,
	const String& value,
	const RISE::Cst::CstHeadVersion* baseVersionOrNull )
{
	AgentCommitResult r;

	// F5 polish A1 round 2: refuse a MID-TRANSACTION agent commit BEFORE any
	// mutation or park -- the same rule as the existing mTxnOpen refusals
	// on the other entry points (SetSelection's active-switch; SetProperty's
	// animation frame-count / rasterizer / film; Undo/Redo's history
	// navigation).
	// An agent commit's Document mutation has NO EditHistory record, so
	// RollbackTransaction's inverse-edit Undo loop can never revert it; a
	// FULL rollback would then RestoreEditorState the pre-transaction dirty
	// baseline OVER the commit's dirty mark while the mutated Document
	// survives -> HasUnsavedChanges()==false -> a close-without-prompt
	// silently LOSES the agent's edit.  (DirtyTracker::RestoreState's
	// OR-merge shields only the uncategorized CST-head BOOLEAN channel;
	// a KNOWN-kind mark -- e.g. a material edit -> EntityCategory::Material
	// -- lands in mEntityDirty, which restores by plain copy.)  Surfaced
	// HONESTLY as a plain "rejected" (NOT a conflict -- the head did not
	// move; the agent should simply retry once the gesture completes).
	// Thread contract: mTxnOpen is a plain bool that mMutex does NOT cover
	// (Begin/EndTransaction write it with no lock held; see the member doc
	// in the header) -- the check is race-free today because the
	// transaction API and the in-app agent path both run on the main
	// thread; a future async agent transport must revisit this (marshal
	// the commit to the main thread or synchronize the flag).
	if( mTxnOpen )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent commit refused inside an open transaction (no EditHistory record -> rollback cannot revert it)." );
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		// The ONE transient reject: the identical commit succeeds once the
		// gesture completes, so mark it retriable for the wire client (the
		// permanent rejects below keep the default false).
		r.retriable = true;
		r.message = String( "editor transaction in progress -- retry after the gesture completes" );
		{
			// Keep this function's "every head read is under mMutex"
			// invariant (no torn 16-byte read against a concurrent
			// parked edit on another thread).  No park needed -- we
			// mutate nothing, so the in-flight pass keeps rendering.
			std::lock_guard<std::mutex> hlk( mMutex );
			r.headVersion = mJob.GetCstHeadVersion();
		}
		return r;
	}

	// Cancel-and-park the render thread, THEN hold mMutex across the WHOLE
	// commit -- the pre-flight refusals, the conflict pre-check,
	// ApplyCstParamEdit (which on a D2 ClearAll's + rebuilds the Scene and
	// transiently zeroes the head-version to {0,0}), the RebindEditorToJob,
	// and the post-commit head read.  Keeping EVERY mCstHeadVersion read under
	// the lock is what the slice-1a reviewer required: no render-thread reader
	// observes the {0,0} transient or a half-rebuilt Scene, and -- since a
	// concurrent parked edit on ANOTHER thread is the real writer of the head
	// -- there is no torn read of the 16-byte CstHeadVersion either.
	std::unique_lock<std::mutex> lk( mMutex );
	CancelAndParkRender_( lk );

	// Pre-flight refusals (under the lock): a Job with no retained Document,
	// or an empty field.  The reported current head is a stable, non-torn
	// read.  A reject lands no edit; it cancelled the in-flight pass, but the
	// render loop's refinement watchdog (wait_for(kRefineWakeMs)) resumes the
	// convergence walk, so -- as on the conflict path -- no kick is needed.
	if( !mJob.HasRetainedCstDocument() )
	{
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.headVersion = mJob.GetCstHeadVersion();
		r.message = String( "no retained CST Document -- agent commit needs a CST-loaded head" );
		return r;
	}
	if( entityName.size() <= 1 || param.size() <= 1 || value.size() <= 1 )
	{
		// RString::size() counts the trailing NUL, so a non-empty string has
		// size >= 2; size <= 1 is the empty case.
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.headVersion = mJob.GetCstHeadVersion();
		r.message = String( "target, param, and value must all be non-empty" );
		return r;
	}

	// Optimistic-concurrency CONFLICT precondition (slice 1a), checked here
	// UNDER THE LOCK (not before parking) so the current head we compare
	// against is stable -- a concurrent edit through another parked path
	// cannot move it between our read and the ApplyCstParamEdit below.  A
	// stale patch must NEVER touch the Document.
	if( baseVersionOrNull )
	{
		const RISE::Cst::CstHeadVersion cur = mJob.GetCstHeadVersion();
		if( *baseVersionOrNull != cur )
		{
			r.applied     = false;
			r.conflict    = true;
			r.rawCode     = 0;
			r.status      = String( "conflict" );
			r.headVersion = cur;
			char buf[160];
			std::snprintf( buf, sizeof( buf ),
				"baseHeadVersion does not match the current head (revision %llu) -- re-read and re-propose",
				static_cast<unsigned long long>( cur.revision ) );
			r.message = String( buf );
			return r;   // lk unlocks; render thread was parked but no edit landed -- no kick needed
		}
	}

	// Shared-undo U1: capture the entity's CURRENT value of `param` (or its
	// ABSENCE) from the retained Document BEFORE the coming apply mutates it --
	// under the SAME mMutex hold as the apply itself (no TOCTOU: nothing else
	// can move the head between this read and ApplyCstParamEditChecked below).
	// `haveCapture` is false only when there is no retained Document or the
	// entity does not resolve -- in that case ApplyCstParamEditChecked below
	// will independently reject with code 0 (no mutation, so no history push
	// either); the capture failing is never itself a reason to refuse the edit.
	String prevValue;
	bool   prevWasAbsent = false;
	const bool haveCapture = CaptureAgentPriorParamValue_( entityName, entityKind, param, prevValue, prevWasAbsent );

	// The SAME edit the GUI property panel makes, but routed directly (we
	// already hold the park the GUI path takes via mEditor.Apply) and through
	// the CHECKED variant (round-2 P1-A root gate): an agent edit may RETARGET
	// references, so it must not commit a head that no longer derives in
	// document order (a forward reference the incremental path's live-manager
	// validation cannot see -- the head's bytes would fail to reload).  The
	// GUI SetProperty / gizmo path (mEditor.Apply -> ApplyCstParamEdit) keeps
	// the UNGATED fast path: its edits are value-only and latency-sensitive.
	// `occ = 0` = the first occurrence of the param on the entity.  The call
	// re-derives the live Job itself (incremental or D2 full re-derive) and
	// bumps the head revision on success.
	const int code = mJob.ApplyCstParamEditChecked(
		entityName.c_str(),
		entityKind.size() <= 1 ? nullptr : entityKind.c_str(),
		param.c_str(),
		/*occ=*/0,
		value.c_str() );

	// A D2 full re-derive (codes 2 AND 3) ClearAll'd + rebuilt the Scene +
	// managers -- the editor's cached scene/manager pointers now dangle into
	// freed storage.  Re-point them (the SAME rebind the variant-switch path
	// relies on) BEFORE releasing the lock, so the next edit / gizmo / undo
	// through mEditor does not use-after-free.  We call ApplyCstParamEdit
	// DIRECTLY (not through mEditor.Apply), so mEditor does NOT self-rebind
	// -- the controller must do it here.  Both 2 and 3 replaced the managers.
	if( code == 2 || code == 3 )
	{
		RebindEditorToJob();
	}

	r.rawCode = code;
	switch( code )
	{
		case 1:
			r.applied = true;
			r.status  = String( "applied" );
			r.message = String( "applied incrementally (managers untouched)" );
			break;
		case 2:
			r.applied = true;
			r.status  = String( "applied" );
			r.message = String( "applied via a full re-derive (Scene + managers were replaced)" );
			break;
		case 3:
			// Replaced-but-diagnosed: the source contract treats 3 as a
			// failure, so applied is FALSE -- but the Document WAS mutated and
			// the managers WERE replaced (we rebound above), so the message
			// says so plainly (a caller must not assume nothing changed).
			r.applied = false;
			r.status  = String( "diagnosed" );
			r.message = String( "edit NOT a clean success: the Document was mutated and the live managers were "
			                    "replaced, BUT the full re-derive emitted diagnostics (see log) -- do NOT treat as applied" );
			break;
		case 0:
		default:
			r.applied = false;
			r.status  = String( "rejected" );
			r.message = String( "edit rejected (entity/param not found or the edit would not derive) -- head unchanged" );
			break;
	}

	// Post-commit head (revision bumped on a clean apply / diagnosed; the
	// unchanged current head on a reject) -- read UNDER the lock so it is
	// coherent with the (possibly just-rebuilt) Job.
	r.headVersion = mJob.GetCstHeadVersion();

	// Model-B (code-3 re-render fix): kick the render whenever the live Scene CHANGED (code 1/2/3), DECOUPLED from
	// the "clean apply" success flag.  A diagnosed re-derive (code 3) ClearAll'd + rebuilt the Scene+managers from
	// the mutated Document (best-effort, diagnostics logged) -- the viewport now shows a DIFFERENT scene than
	// before, so a fresh pass is REQUIRED, else the user sees stale pre-edit pixels.  Codes 1 (incremental) and 2
	// (replaced-clean) also mutated the live scene.  Only code 0 (reject / conflict / no-op -- live scene INTACT)
	// skips the kick.  The kick fires under the SAME held mMutex the clean-apply path used (no second lock, no
	// post-unlock kick), mirroring the GUI layer's `ok || changed` decoupling.  r.applied / r.status stay UNCHANGED
	// (code 3 remains applied=false / "diagnosed"); this only decouples the render-kick from success.
	if( code == 1 || code == 2 || code == 3 )
	{
		// F5 slice 1b (data-loss fix): the agent path mutated the retained
		// CST head DIRECTLY via ApplyCstParamEdit -- it bypassed
		// mEditor.Apply, so the GUI's per-edit dirty-mark
		// (MarkEditEntityDirty inside Apply) never ran.  Without this the
		// editor still believes the scene is CLEAN, the Save button stays
		// disabled, and a close-without-prompt path silently LOSES the
		// agent's edit.  A diagnosed code-3 ALSO mutated the Document
		// (revision bumped), so it is genuinely unsaved and must mark too.
		// Do it UNDER the held mMutex, BEFORE the kick's lk.unlock(), so
		// the dirty state and the head-version move together.  NOTE: the
		// dirty-changed LISTENER fires on THIS (the calling) thread; a
		// future 1c background-transport agent must marshal it to the UI
		// dispatch queue (a 1c concern, not this fix).  Code 0
		// (reject / conflict -- head unchanged) does NOT reach here, so it
		// never marks dirty.
		mEditor.MarkCstHeadDirty(
			entityName.c_str(),
			entityKind.size() <= 1 ? nullptr : entityKind.c_str() );

		// Shared-undo follow-up (P2 fix): an agent commit that mutates an
		// EMISSIVE material's slot (exitance / scale / any emission-affecting
		// param) never used to bump the scene's light-topology generation --
		// only the GUI property panel's SetMaterialProperty arm
		// (MarkEditEntityDirty) did that.  A reused RayCaster's LightSampler
		// alias table then kept the STALE selection weight from before the
		// edit (light SELECTION biased toward the old emission footprint;
		// per-sample Le stays live so the estimator itself is unbiased).
		// Resolve material-ness from entityKind the same way MarkCstHeadDirty
		// just did above; see BumpSceneLightGenerationForAgentParamEdit's doc
		// for the conservative-bump rule on empty/unknown kinds.  Harmless to
		// call unconditionally (codes 2/3 already got a fresh alias table via
		// RebindEditorToJob above -- an extra bump there is a moot no-op, not
		// a correctness issue).
		mEditor.BumpSceneLightGenerationForAgentParamEdit(
			entityName.c_str(),
			entityKind.size() <= 1 ? nullptr : entityKind.c_str() );

		// Shared-undo U1: push an EditHistory record so this agent edit is a
		// first-class undo/redo citizen -- the headline gap this slice closes
		// (previously an agent commit left NO history record, so a human
		// Cmd-Z either skipped it entirely or undid an OLDER human edit
		// instead).  Pushed for EVERY mutating code, INCLUDING 3
		// (diagnosed-but-mutated): the Document WAS mutated (DocSetOrAddParamValue
		// committed, the head revision bumped, MarkCstHeadDirty above just
		// marked it unsaved) -- an un-revertable mutated Document would be
		// strictly worse than a revertable one, and capture-before-apply above
		// already captured the correct prior state regardless of how the
		// re-derive that follows it turns out.  `haveCapture` should always be
		// true here (ApplyCstParamEditChecked just resolved this same entity
		// under this same lock hold to reach a mutating code), but a defensive
		// skip-with-log guards against ever pushing a garbage/empty capture.
		if( haveCapture )
		{
			mEditor.PushAgentCstParamEdit( entityName, entityKind, param, value, prevValue, prevWasAbsent );
		}
		else
		{
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController: agent commit on `%s`.`%s` applied (code %d) but the prior-value capture "
				"failed -- NO undo history record was pushed for this edit (should not happen: the apply just "
				"resolved the same entity under the same lock).",
				entityName.c_str(), param.c_str(), code );
		}

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
	}
	// else (code 0): lk unlocks on scope exit; the render thread stays parked-then-idle
	// (it will resume its prior wait; a rejected/conflict edit changed nothing).
	return r;
}

// Model-B F5 slice S2: the two chunk-CRUD verbs.  Thin wrappers -- the whole
// reviewed commit pattern lives in ApplyAgentChunkCrud_ below.
SceneEditController::AgentCommitResult SceneEditController::ApplyAgentInsertChunk(
	const String& chunkText,
	const RISE::Cst::CstHeadVersion* baseVersionOrNull )
{
	return ApplyAgentChunkCrud_( /*isInsert*/ true, chunkText, String(), baseVersionOrNull );
}

SceneEditController::AgentCommitResult SceneEditController::ApplyAgentRemoveChunk(
	const String& target,
	const String& kind,
	const RISE::Cst::CstHeadVersion* baseVersionOrNull )
{
	return ApplyAgentChunkCrud_( /*isInsert*/ false, target, kind, baseVersionOrNull );
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: proposal staging + owner-approval state machine.
//
// StageProposal is deliberately CHEAP and INERT: it only appends to
// mProposals under mMutex -- no cancel-and-park (nothing here touches the
// Document or the live Scene, so there is nothing for the render thread to
// race), no conflict check (that happens once, at APPROVAL time in
// ResolveProposal -- staging against a since-moved head is fine; it is
// APPLYING against a since-moved head that would be unsafe), no EditHistory
// record (an inert proposal is not yet a commit).
//
// S5a hardening round: baseVersion is stamped from mJob.GetCstHeadVersion()
// HERE, under the SAME mMutex hold that mints the id and enqueues -- not
// pre-read unlocked by the caller (AgentSession) beforehand.  mJob is a
// reference (IJobPriv&), always valid, so reading it here is exactly the
// same "head read under mMutex" contract ApplyAgentParamEdit's own doc
// requires (see that method's comment block, ~line 3193): no torn 16-byte
// read against a concurrent commit / render-thread write on another thread.
//////////////////////////////////////////////////////////////////////
std::uint64_t SceneEditController::StageProposal( const AgentProposal& proposal,
                                                   RISE::Cst::CstHeadVersion* outStagedVersion )
{
	std::lock_guard<std::mutex> lk( mMutex );

	// Secure-MCP slice 6: the PENDING-depth cap, checked FIRST and BEFORE
	// touching mNextProposalId or mProposals at all -- a full queue is a
	// clean refusal (return 0, nothing enqueued, nothing mutated), never a
	// silent prune of someone else's still-pending proposal. See
	// kMaxPendingProposals's doc for why 32 is a generous backstop, not a
	// limit a well-behaved client should expect to hit.
	std::size_t pendingCount = 0;
	for( const AgentProposal& existing : mProposals )
	{
		if( existing.status == String( "pending" ) ) ++pendingCount;
	}
	if( pendingCount >= kMaxPendingProposals )
	{
		return 0;   // refused -- 0 is never a real id (see the doc above)
	}

	AgentProposal p = proposal;
	p.id     = mNextProposalId++;
	p.status = String( "pending" );
	if( !p.hasExplicitBaseVersion )
	{
		// The common case: the proposing session did not pin an explicit
		// baseHeadVersion, so the head-at-stage-time IS the base -- read it
		// now, under mMutex, instead of trusting whatever (potentially
		// racy) value the caller put in proposal.baseVersion.
		p.baseVersion = mJob.GetCstHeadVersion();
	}
	const std::uint64_t id = p.id;
	if( outStagedVersion ) *outStagedVersion = p.baseVersion;

	// Secure-MCP slice 6: the TOTAL-storage cap. If accepting this new
	// proposal would push mProposals past kMaxProposalHistory, evict the
	// SINGLE OLDEST resolved (non-pending) entry first -- bounding the
	// audit trail's storage without ever discarding a still-pending
	// proposal. An evictable resolved entry is GUARANTEED to exist here:
	// pendingCount was just proven < kMaxPendingProposals <= kMaxProposalHistory,
	// so if mProposals.size() is already at the history cap, at least
	// (kMaxProposalHistory - kMaxPendingProposals + 1) of those entries are
	// resolved, not pending.
	if( mProposals.size() >= kMaxProposalHistory )
	{
		for( std::vector<AgentProposal>::iterator it = mProposals.begin(); it != mProposals.end(); ++it )
		{
			if( it->status != String( "pending" ) )
			{
				mProposals.erase( it );
				break;
			}
		}
	}

	mProposals.push_back( p );
	return id;
}

std::vector<SceneEditController::AgentProposal> SceneEditController::ListProposals() const
{
	std::lock_guard<std::mutex> lk( mMutex );
	return mProposals;   // copy out under the lock -- the caller's snapshot is stable
}

bool SceneEditController::ResolveProposal( std::uint64_t id, bool approve, AgentCommitResult* outResult )
{
	// The whole resolve -- find, re-check, and (on approve) apply -- runs
	// under ONE mMutex hold, so there is no window between "the base
	// version matched" and "the apply actually ran" for a concurrent commit
	// (staged or direct, on any thread) to move the head out from under us.
	// ApplyAgent{ParamEdit,InsertChunk,RemoveChunk} below take mMutex
	// THEMSELVES (cancel-and-park + the whole commit), so we must NOT hold
	// mMutex while calling them -- mMutex is a plain, non-recursive
	// std::mutex (re-entering it is an immediate self-deadlock, not a
	// stall-and-retry; see CancelAndParkRender_'s doc).  So this method
	// takes mMutex ONLY for the find + status-transition step, releases it,
	// then (on a version match) calls the normal ApplyAgent* entry point --
	// which takes mMutex again for its OWN cancel-and-park critical section.
	//
	// This does NOT reopen the TOCTOU window the single-lock design above
	// was written to close: the invariant that matters is "the version we
	// COMPARE is the version we APPLY AGAINST", and ApplyAgent* re-derives
	// that itself -- baseVersionOrNull is passed straight through to the
	// SAME optimistic-concurrency gate a direct (non-staged) commit already
	// uses, checked UNDER ApplyAgent*'s OWN mMutex hold, atomically with
	// its own apply.  A second mover between our pre-check here and
	// ApplyAgent*'s internal check is not a bug: it is caught by
	// ApplyAgent*'s own gate and folded into this method's conflict
	// handling below, exactly as if OUR pre-check had raced it.  What we do
	// here first (a cheap pre-check before even attempting the call) is
	// belt-and-suspenders for a fast, honest status on the COMMON case
	// (proposal already known-stale) without ever needing to hold mMutex
	// across a nested ApplyAgent* call.
	AgentProposal snapshot;
	{
		std::lock_guard<std::mutex> lk( mMutex );
		bool found = false;
		for( auto& p : mProposals )
		{
			if( p.id == id )
			{
				if( p.status != String( "pending" ) ) return false;   // already resolved -- refuse a second resolve
				snapshot = p;
				found = true;
				break;
			}
		}
		if( !found ) return false;

		if( !approve )
		{
			for( auto& p : mProposals ) if( p.id == id ) { p.status = String( "rejected" ); break; }
			// Secure-MCP slice 5b fix round (P2-2): a reject never applies
			// anything, but the caller-visible AgentCommitResult must still
			// report the REAL current head -- leaving it default-constructed
			// {0,0} is indistinguishable from the "no session / unknown id"
			// sentinel a caller uses to detect a refusal that never even
			// found the proposal.  mMutex is still held here (this whole
			// find+reject block runs under the SAME lock acquired above), so
			// this is a safe, non-torn read of the 16-byte CstHeadVersion --
			// no concurrent commit on another thread can be mutating it
			// mid-read.
			if( outResult )
			{
				outResult->status      = String( "rejected" );
				outResult->headVersion = mJob.GetCstHeadVersion();
				outResult->message     = String( "proposal rejected (no mutation)" );
			}
			return true;   // apply never ran -- applied/rawCode/retriable stay at their defaults (false/0/false)
		}
	}

	// APPROVE: re-check the base version against the CURRENT head.  This is
	// the hard invariant -- a stale proposal must NEVER apply, no matter how
	// long it sat on the queue.  ApplyAgent* below performs this SAME check
	// internally (baseVersionOrNull, non-null), atomically with its own
	// apply, under ITS mMutex hold -- so passing snapshot.baseVersion
	// through is both the honest re-check this slice promises AND reuses
	// the exact, already-reviewed conflict gate rather than duplicating it.
	AgentCommitResult cr;
	switch( snapshot.kind )
	{
		case AgentProposalKind::ParamEdit:
			cr = ApplyAgentParamEdit( snapshot.target, snapshot.entityKind, snapshot.param, snapshot.value, &snapshot.baseVersion );
			break;
		case AgentProposalKind::InsertChunk:
			cr = ApplyAgentInsertChunk( snapshot.chunkText, &snapshot.baseVersion );
			break;
		case AgentProposalKind::RemoveChunk:
			cr = ApplyAgentRemoveChunk( snapshot.target, snapshot.entityKind, &snapshot.baseVersion );
			break;
	}

	{
		std::lock_guard<std::mutex> lk( mMutex );
		for( auto& p : mProposals )
		{
			if( p.id == id )
			{
				// Fold ApplyAgent*'s own result into the proposal's final
				// status: a clean apply -> "applied"; a version conflict
				// (caught by ApplyAgent*'s own gate) -> "conflict"; anything
				// else (rejected / diagnosed pre-flight refusal surfaced by
				// the underlying call) -> "conflict" is reserved for the
				// version-mismatch case specifically, so map those other
				// outcomes to "rejected" -- they are not what this
				// invariant is about, but they are still NOT an apply.
				if( cr.applied )                         p.status = String( "applied" );
				else if( cr.status == String( "conflict" ) ) p.status = String( "conflict" );
				else                                      p.status = String( "rejected" );
				break;
			}
		}
	}

	if( outResult ) *outResult = cr;
	return true;
}

// Facet 5 (preview-render safety): park the render thread and run a
// NON-mutating (from the Document's point of view) callback under mMutex.
// Same mTxnOpen refusal as the agent commit paths (parking here would stall
// an in-progress gesture); otherwise cancel-and-park exactly like
// ApplyAgentParamEdit / ApplyAgentChunkCrud_ and invoke `fn` synchronously
// with the render thread drained. `fn` runs BEFORE the lock releases, so
// DoOneRenderPass cannot observe (or race) whatever transient Film/camera
// state `fn` sets up -- the caller is responsible for restoring that state
// itself before returning from `fn` (this method has no notion of what `fn`
// changed; it only guarantees exclusivity while `fn` runs).
bool SceneEditController::RunPreviewRenderParked( const std::function<void()>& fn )
{
	// Back-compat forwarder: the pre-slice-S1 callers (AgentSession's
	// override-render window) don't care about render identity, so route
	// through the identity-tracking overload with the class/label this
	// method has always implicitly meant (an agent-driven preview render)
	// and discard the assigned id.  Behaviourally IDENTICAL to the old
	// body -- same mTxnOpen refusal, same cancel-and-park, same `fn`
	// invocation under the same lock hold.
	return RunPreviewRenderParked( fn, RenderClass::AgentPreview, String(), nullptr );
}

// Model-B F2 slice S1: identity-tracking variant.  The render-job record
// lives under the SAME mMutex hold CancelAndParkRender_ already takes --
// no new lock, no new synchronization primitive.  Behavior (the
// mTxnOpen refusal, the cancel-and-park, `fn`'s single synchronous
// invocation, the no-kick-on-return contract) is UNCHANGED from the base
// overload; this only adds bookkeeping around the existing critical
// section.
//
// `fn` must NOT call CurrentRenderJob() or any other mMutex-taking method
// on this controller -- mMutex is a plain (non-recursive) std::mutex and
// is already held across this whole call; re-entering it from `fn` is an
// immediate deadlock, not a retry.
//
// Pre-S2 hardening: `fn` is AgentSession's doRenderWork, which calls
// mJob->Rasterize() -- a DOCUMENTED real throw site (OIDN; see
// AgentSession.h's Render(AgentRenderParams) doc + the SinkUnwindGuard in
// AgentSession.cpp).  Before this fix, `mCurrentRenderJob.active = false`
// sat AFTER the `fn()` call in plain sequential code -- a throw out of
// `fn` skipped it, so `active` stayed true FOREVER (until the next
// RunPreviewRenderParked / RenderLoop pass happened to overwrite it): a
// permanent false "render in flight" for any Status()-style consumer.
// mMutex itself is fine (std::unique_lock unwinds via RAII), but the
// bookkeeping flag is plain data with no RAII of its own.  Fix: a tiny
// scope guard (matching the AgentSession.cpp SinkUnwindGuard house idiom)
// whose destructor flips `active` false on EVERY exit -- normal return OR
// exception unwind -- runs under the lock already held here, so no new
// locking in the destructor.
bool SceneEditController::RunPreviewRenderParked(
	const std::function<void()>& fn,
	RenderClass                  renderClass,
	const String&                clientLabel,
	RenderJobId*                 outJobId )
{
	if( mTxnOpen )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: preview render refused inside an open transaction (would stall the gesture)." );
		return false;
	}

	std::unique_lock<std::mutex> lk( mMutex );
	CancelAndParkRender_( lk );

	RenderJobId jobId;
	{
		// Model-B F2 slice S2a fix: mJobStatusMutex, NOT mMutex, guards
		// mCurrentRenderJob/mNextRenderJobId -- see that member's doc for
		// why (a status reader must never block behind mMutex's
		// render-duration hold).  This brief nested acquisition is safe:
		// mJobStatusMutex is never held here across anything but these
		// three field writes.
		std::lock_guard<std::mutex> statusLk( mJobStatusMutex );
		jobId = mNextRenderJobId += kControllerRenderJobIdStride;
		mCurrentRenderJob.id          = jobId;
		mCurrentRenderJob.renderClass = renderClass;
		mCurrentRenderJob.active      = true;
		mCurrentRenderJob.clientLabel = clientLabel;   // Fix-round-1 P3-c: now actually surfaced (was previously discarded via `(void)clientLabel`)
	}

	// outJobId is assigned BEFORE fn() runs, not after: the job record
	// above already reflects a real, already-started render (id minted,
	// active=true) regardless of whether `fn` later throws, so "this job
	// existed and ran" is the honest answer even on the throw path -- an
	// id names a call that ran, not a call that succeeded (mirrors
	// AgentRenderResult::renderJobId's field doc: a FAILED render still
	// carries its renderJobId). This does NOT change the documented
	// untouched-on-REFUSAL contract above (the mTxnOpen early-return still
	// leaves *outJobId untouched and the counter un-advanced) -- it only
	// resolves the previously-undocumented throw case in favour of
	// "assigned", since the alternative (assigning only after a
	// successful fn()) would silently swallow the id of a render that
	// genuinely ran and genuinely failed.
	if( outJobId ) *outJobId = jobId;

	// RAII: flips `active` false on EVERY exit from here down, including
	// an exception unwinding out of fn() -- takes mJobStatusMutex itself
	// (briefly) rather than relying on mMutex, which `lk` holds for the
	// whole render.  `lk` holding mMutex for this whole scope already
	// makes this record safe from RenderLoop's mint (RenderLoop's own
	// mint site briefly takes mMutex too, so it cannot interleave) --
	// but fix-round-3 (churn UAF) makes EVERY clear site
	// ownership-checked on principle, so this guard can never clobber a
	// later job's record even if that invariant is ever weakened.
	struct ActiveFlipGuard {
		SceneEditController& self;
		RenderJobId          jobId;
		~ActiveFlipGuard() {
			std::lock_guard<std::mutex> statusLk( self.mJobStatusMutex );
			if( self.mCurrentRenderJob.id == jobId )
			{
				self.mCurrentRenderJob.active = false;
			}
		}
	} activeFlipGuard{ *this, jobId };

	if( fn ) fn();

	// Unlock (end of scope) resumes the render loop's normal wait/cycle --
	// no kick needed: we made no Document/scene edit, so the interactive
	// loop's own refinement watchdog resumes exactly where it left off.
	return true;
}

SceneEditController::RenderJobStatus SceneEditController::CurrentRenderJob() const
{
	// Model-B F2 slice S2a fix: mJobStatusMutex, not mMutex -- see that
	// member's doc.  Reading via mMutex would block this call for an
	// entire in-flight render's duration.
	std::lock_guard<std::mutex> lk( mJobStatusMutex );
	return mCurrentRenderJob;
}

// Model-B F2 slice S2a -------------------------------------------------

// The dedicated agent-render worker's loop.  Long-lived: one instance of
// this loop runs for the whole life of the controller (see the ctor's
// std::thread construction + Stop()'s teardown).  Waits for a submission
// or shutdown (guarded by the NARROW mAgentRenderSlotMutex -- see that
// member's doc for why this is a SEPARATE lock from mMutex), pulls the
// closure out of the slot, THEN acquires mMutex and runs it under the
// SAME critical section RunPreviewRenderParked uses -- CancelAndParkRender_
// -- so the interactive thread is parked for the render's whole duration
// exactly as it is for a RunPreviewRenderParked caller.  mMutex is
// released again BEFORE the slot is cleared, so a fresh submission's
// quick mAgentRenderSlotMutex-only check is never blocked behind a
// render in progress.
void SceneEditController::AgentRenderWorkerLoop_()
{
	for(;;)
	{
		std::function<void()> fn;
		RenderJobId thisOccupantJobId = kInvalidRenderJobId;
		{
			std::unique_lock<std::mutex> slotLk( mAgentRenderSlotMutex );
			mAgentRenderCV.wait( slotLk, [&]{
				return mAgentRenderPending || mAgentRenderStop.load( std::memory_order_acquire );
			} );

			if( !mAgentRenderPending )
			{
				// Woken for shutdown with nothing queued -- exit.
				if( mAgentRenderStop.load( std::memory_order_acquire ) ) return;
				continue;   // spurious wake guard (shouldn't happen given the predicate above)
			}

			// Take the submission out of the slot.  mAgentRenderPending
			// STAYS true for the render's whole duration (it means
			// "occupied", not "queued-but-not-started") -- it is cleared
			// only once the render actually finishes, further down.
			fn = std::move( mAgentRenderFn );
			mAgentRenderFn = nullptr;
			// Fix-round-3 (churn UAF): snapshot the id THIS occupant was
			// minted with -- mAgentRenderJobId is stable from here until
			// this loop iteration clears mAgentRenderPending (no new
			// submission can land while the slot is occupied), so this
			// read outside slotLk is safe, same as the mAgentRenderFn move
			// just above.  Used below to ownership-check the completion
			// write instead of clearing unconditionally.
			thisOccupantJobId = mAgentRenderJobId;
		}   // slotLk released here -- a concurrent SubmitAgentRenderAsync's
		    // occupancy check is never blocked by the render that follows.

		// Fix-round-4 P2 RED-PROVE test seam -- no-op in production (see
		// the declaration doc).  This is the narrow real window a test
		// needs: the submission's mint + flag-set already happened (in
		// SubmitAgentRenderAsync_Locked, before this thread was even
		// woken), but this thread has not yet raced RenderLoop's own mint
		// block for mMutex.
		ForTest_OnAgentWorkerAboutToParkRender();

		std::exception_ptr caught;
		{
			// Cancel-and-park the interactive thread -- identical critical
			// section to RunPreviewRenderParked (same CancelAndParkRender_,
			// same mMutex hold for the render's whole duration).  The job
			// record was already populated by the submitter
			// (SubmitAgentRenderAsync) before this thread ever saw the
			// submission, so no re-mint here.
			std::unique_lock<std::mutex> renderLk( mMutex );
			CancelAndParkRender_( renderLk );

			// Fix-round-1 P2-C: clear any STALE cancel flag left by whatever
			// just drained (the interactive pass CancelAndParkRender_ may
			// have just cancelled, or a PRIOR agent render that itself was
			// cancelled) before this fresh render's `fn` runs -- otherwise a
			// cancel from an unrelated previous render would instantly
			// self-cancel this new one.  Mirrors RenderLoop's own per-pass
			// Reset() (see that call site).  Safe here: mMutex is held for
			// the whole render (this is the ONLY place that can touch
			// mCancelProgress until fn() returns), so no concurrent reader
			// can observe a torn Reset-then-cancel window.
			mCancelProgress.Reset();

			// RAII: flips the job record's `active` false AND releases the
			// interactive-loop gate on EVERY exit, including a throw out
			// of `fn` -- mirrors RunPreviewRenderParked's ActiveFlipGuard,
			// plus the S2a gate release.  The job-record write takes
			// mJobStatusMutex (NOT mMutex -- see that member's doc: a
			// status reader must never block behind mMutex's
			// render-duration hold); mAgentRenderBlocksInteractive is a
			// plain atomic, cleared here under mMutex (renderLk, already
			// held) so RenderLoop's next mMutex acquisition sees the gate
			// release with proper ordering.
			struct ActiveFlipGuard {
				SceneEditController& self;
				RenderJobId          jobId;
				~ActiveFlipGuard() {
					{
						// Fix-round-3 (churn UAF): ownership-checked clear --
						// see RenderLoop's completion site for the full
						// mechanism this guards against.  Here the
						// vulnerability runs the OTHER direction: this
						// worker's mMutex hold (via CancelAndParkRender_,
						// spanning the whole render) already excludes any
						// INTERLEAVED interactive pass, so in practice this
						// guard's own id always still matches -- but a
						// caller must never assume that from the write
						// site alone, so every clear site uses the same
						// compare-then-clear on principle.
						std::lock_guard<std::mutex> statusLk( self.mJobStatusMutex );
						if( self.mCurrentRenderJob.id == jobId )
						{
							self.mCurrentRenderJob.active = false;
						}
					}
					self.mAgentRenderBlocksInteractive.store( false, std::memory_order_release );
				}
			} activeFlipGuard{ *this, thisOccupantJobId };

			try {
				if( fn ) fn();
			}
			catch( ... ) {
				// Captured, NOT rethrown here -- this is a background
				// worker thread; rethrowing would terminate() the process
				// the instant it left this try block uncaught.
				// SubmitAgentRenderSync picks this up (via
				// mAgentRenderException below) and rethrows it on the
				// CALLING thread after completion; a SubmitAgentRenderAsync
				// caller that never syncs on this exception simply never
				// sees it thrown (matches AgentRenderResult's existing
				// "ok=false" convention -- async failures are reported
				// through result plumbing, not C++ exceptions, once they
				// cross a thread boundary).
				caught = std::current_exception();
			}
		}   // renderLk released here -- BEFORE the slot is cleared below,
		    // so mMutex is never held while touching the slot.

		// Wake the interactive loop NOW that mAgentRenderBlocksInteractive
		// is clear -- it may have a pending edit queued up behind the
		// gate (see RenderLoop's pred / continue check).
		mCV.notify_all();

		// Free the slot + publish the exception, all under the narrow slot
		// mutex, then notify.  (Fix-round-1 P3-b: this used to also bump
		// mAgentRenderCompletedGeneration -- deleted; it was write-only
		// dead state nothing ever read.  mAgentRenderServingTicket, bumped
		// by SubmitAgentRenderSync's fairness wait below, already serves
		// as the slot's monotonic occupancy counter.)
		{
			std::lock_guard<std::mutex> slotLk( mAgentRenderSlotMutex );
			mAgentRenderException = caught;
			mAgentRenderPending   = false;
			// Model-B F2 S3 fix round (P3-b): defensive clear alongside
			// mAgentRenderPending above.  Benign today -- every reader of
			// mAgentRenderPinned (SubmitAgentRenderAsync_Locked's pinned-
			// occupant check) is gated behind `if( mAgentRenderPending )`,
			// so a stale `true` left here after the slot frees can never
			// be observed as "a pinned job is occupying the slot" once
			// mAgentRenderPending is false.  Clearing it anyway removes
			// the standing proof obligation ("mAgentRenderPinned is only
			// meaningful while mAgentRenderPending is true" -- see that
			// field's own doc) instead of leaving it as an invariant a
			// future reader has to re-derive from first principles.
			mAgentRenderPinned    = false;
		}
		mAgentRenderDoneCV.notify_all();
	}
}

// Round-2 P2-C: the mint-and-claim core, factored out of SubmitAgentRenderAsync
// so SubmitAgentRenderSync can inline it WITHOUT releasing mAgentRenderSlotMutex
// between releasing its fairness ticket and claiming the slot -- see the header
// doc and SubmitAgentRenderSync's own comment for the exact cross-thread window
// this closes.  ASSUMES `slotLk` already holds mAgentRenderSlotMutex; never
// locks or unlocks it itself.  `bypassFairQueueCheck` is true ONLY for
// SubmitAgentRenderSync's own inlined call (see this method's declaration doc).
bool SceneEditController::SubmitAgentRenderAsync_Locked(
	std::unique_lock<std::mutex>& slotLk,
	std::function<void()>         fn,
	const String&                 clientLabel,
	RenderJobId*                  outJobId,
	bool                          bypassFairQueueCheck,
	bool                          pinned,
	RenderClass                   renderClass )
{
	(void)slotLk;   // proves the caller holds the lock; not touched here

	// Fix-round-1 P1-1: refuse HONESTLY once Stop() has been called
	// (or is concurrently being called -- the flag is set under this
	// SAME lock in Stop(), so this read is strictly ordered against
	// that write).  Before this fix, a post-Stop() submission still
	// flipped mAgentRenderPending true and handed the slot to a
	// worker that may already have exited (or is about to) --
	// SubmitAgentRenderSync's caller then waited on mAgentRenderDoneCV
	// FOREVER (nobody left to notify it), and the slot stayed
	// poisoned (mAgentRenderPending stuck true) for any later caller
	// on this now-dead controller.
	if( mAgentRenderStop.load( std::memory_order_acquire ) )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent render submission refused -- controller stopped." );
		return false;
	}

	if( mAgentRenderPending )
	{
		// Model-B F2 slice S3: a PINNED occupant refuses EVERY new
		// submission (async or sync, pinned or not) with a DEDICATED log
		// line -- the single-slot message just below still covers the
		// ordinary preview-occupant case.  Checked before the generic
		// single-slot refusal (which would otherwise fire first and mask
		// which reason applies) but AFTER mAgentRenderPending is
		// confirmed true (mAgentRenderPinned is only meaningful while a
		// job actually occupies the slot).
		if( mAgentRenderPinned )
		{
			GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent render submission refused -- a PINNED render is in flight (never silently superseded; retry after it completes)." );
			return false;
		}
		// Single-slot: a submission is already queued or running.
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent render submission refused -- another agent render is already queued or running (single-slot policy)." );
		return false;
	}

	// Fix-round-1 P1-2: an async submitter must never jump a
	// SYNCHRONOUS caller's fair place in line -- SubmitAgentRenderSync
	// callers queue via a FIFO ticket (see that method) and are owed
	// the NEXT free slot in arrival order.  Refuse here (rather than
	// let this async submission win the slot out from under them)
	// whenever one or more sync callers are currently waiting.
	//
	// Round-2 P2-C: SubmitAgentRenderSync's OWN inlined call passes
	// `bypassFairQueueCheck=true` -- by the time it reaches here it has
	// ALREADY verified (under this SAME continuously-held lock) that it
	// is genuinely its turn (its ticket == mAgentRenderServingTicket) and
	// is about to release its own ticket a few lines up the call chain;
	// checking mAgentRenderWaitingSyncCount here would just be asking
	// "is MY OWN not-yet-released ticket still registered", which is
	// always true and would make every sync submission refuse itself.
	if( !bypassFairQueueCheck && mAgentRenderWaitingSyncCount > 0 )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent render submission refused -- queued waiters exist (fair-slot reservation for synchronous callers)." );
		return false;
	}

	// mCurrentRenderJob/mNextRenderJobId are guarded by mJobStatusMutex
	// (NOT mMutex -- see that member's doc: mMutex is held by the
	// worker for the render's WHOLE DURATION, so a status reader
	// using mMutex would block for the entire render instead of
	// observing it in flight; this was a real bug caught by this
	// slice's own flaky test).  mAgentRenderBlocksInteractive is a
	// SEPARATE plain atomic that RenderLoop checks WHILE holding
	// mMutex -- set it under a brief, nested mMutex acquisition here
	// so RenderLoop's mint block and this write are strictly
	// ordered (RenderLoop cannot observe the gate false while this
	// job is being minted).  Nesting mMutex INSIDE the already-held
	// slot lock is safe here specifically because the single-slot
	// policy above guarantees no render is currently in flight (the
	// worker only holds mMutex WHILE a render runs, and a render can
	// only start after mAgentRenderPending flips true, which happens
	// later in this same critical section) -- so this brief, nested
	// mMutex acquisition can never block behind the worker's
	// render-duration hold.  No other code path acquires mMutex then
	// tries to acquire mAgentRenderSlotMutex (the worker and
	// WaitForRenderJob both release one before acquiring the other),
	// so this nesting order (slot outer, mMutex inner) is the ONLY
	// nesting order used anywhere and cannot deadlock.
	RenderJobId jobId = kInvalidRenderJobId;
	{
		std::lock_guard<std::mutex> statusLk( mJobStatusMutex );
		jobId = mNextRenderJobId += kControllerRenderJobIdStride;
		mCurrentRenderJob.id          = jobId;
		mCurrentRenderJob.renderClass = renderClass;
		mCurrentRenderJob.active      = true;
		mCurrentRenderJob.clientLabel = clientLabel;   // Fix-round-1 P3-c: surface who submitted this job
		mCurrentRenderJob.pinned      = pinned;        // Model-B F2 slice S3
	}
	{
		// Model-B F2 slice S2a: claim the interactive-loop gate for
		// the FULL duration of this render (mint through worker
		// completion) -- see mAgentRenderBlocksInteractive's doc for
		// the exact race this closes (RenderLoop's own mint block
		// stomping mCurrentRenderJob in the gap between this mint and
		// the worker actually starting).
		std::lock_guard<std::mutex> renderLk( mMutex );
		mAgentRenderBlocksInteractive.store( true, std::memory_order_release );
	}

	mAgentRenderFn          = std::move( fn );
	mAgentRenderJobId       = jobId;
	mAgentRenderException   = nullptr;
	mAgentRenderPending     = true;
	mAgentRenderPinned      = pinned;   // Model-B F2 slice S3 -- guarded by mAgentRenderSlotMutex, same as the rest of the slot bookkeeping

	if( outJobId ) *outJobId = jobId;
	return true;
}

// Submit `fn` to run on the dedicated worker; return immediately after a
// brief mAgentRenderSlotMutex hold (NOT mMutex -- see that member's doc)
// that mints the job id and hands the slot to the worker.  See the header
// doc for the single-slot / refusal contract.
bool SceneEditController::SubmitAgentRenderAsync(
	std::function<void()> fn,
	const String&         clientLabel,
	RenderJobId*          outJobId,
	bool                  pinned,
	RenderClass           renderClass )
{
	if( mTxnOpen )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent render submission refused inside an open transaction (would stall the gesture)." );
		return false;
	}

	bool accepted = false;
	{
		std::unique_lock<std::mutex> slotLk( mAgentRenderSlotMutex );
		accepted = SubmitAgentRenderAsync_Locked( slotLk, std::move( fn ), clientLabel, outJobId,
			/*bypassFairQueueCheck=*/false, pinned, renderClass );
	}
	if( !accepted ) return false;

	mAgentRenderCV.notify_all();
	return true;
}

// The synchronous convenience wrapper: FAIRLY wait for the slot (see the
// header doc's fairness-ticket writeup), submit, then block until the
// worker signals completion of THIS job (waiting on mAgentRenderDoneCV,
// guarded by mAgentRenderSlotMutex, for the slot to free up), then
// rethrow whatever exception the worker captured (if any) so this call's
// throw contract matches a direct synchronous invocation of `fn`.
bool SceneEditController::SubmitAgentRenderSync(
	std::function<void()> fn,
	const String&         clientLabel,
	RenderJobId*          outJobId,
	unsigned int          timeoutMs,
	bool                  pinned,
	RenderClass           renderClass )
{
	// Fix-round-1 P1-2: claim a FIFO ticket before attempting to submit,
	// so a burst of concurrent async submitters cannot systematically
	// starve this call (SubmitAgentRenderAsync refuses outright whenever
	// mAgentRenderWaitingSyncCount > 0, so once we're in the queue no
	// NEW async submission can steal the slot ahead of us -- only the
	// occupant that was ALREADY running when we arrived can still finish
	// first, which is correct FIFO behaviour, not starvation).
	unsigned long long myTicket = 0;
	{
		std::lock_guard<std::mutex> slotLk( mAgentRenderSlotMutex );
		myTicket = mAgentRenderNextTicket++;
		++mAgentRenderWaitingSyncCount;
	}

	// RAII: whatever happens below (success, refusal, or a fairness-wait
	// timeout), this ticket's turn must be released exactly once so the
	// waiter behind it (if any) is never stranded.
	struct TicketGuard {
		SceneEditController& self;
		bool                 released = false;
		~TicketGuard() { Release(); }
		void Release() {
			if( released ) return;
			released = true;
			{
				std::lock_guard<std::mutex> slotLk( self.mAgentRenderSlotMutex );
				++self.mAgentRenderServingTicket;
				--self.mAgentRenderWaitingSyncCount;
			}
			self.mAgentRenderDoneCV.notify_all();
		}
	} ticketGuard{ *this };

	// Round-2 P2-C fix: hold mAgentRenderSlotMutex CONTINUOUSLY from the
	// fairness wait's wake-up THROUGH the ticket release THROUGH the
	// inline slot claim below -- closing the round-1 fix's own residual
	// gap.  The round-1 comment this replaces argued "this is all
	// single-threaded from here -- no window for another thread's
	// SubmitAgentRenderAsync to run between these two lines on OUR
	// thread", which is simply wrong for a genuinely concurrent caller:
	// releasing the lock after ticketGuard.Release() and re-acquiring it
	// (via the old round-trip through the public SubmitAgentRenderAsync)
	// left exactly the window an async-spam thread on ANOTHER thread
	// needs -- it can observe mAgentRenderWaitingSyncCount drop to 0 (our
	// release) and win SubmitAgentRenderAsync's slot-occupancy race
	// before this thread gets back around to claiming it, stealing the
	// turn this ticket was just fairly awarded.  Fix: never let go of
	// mAgentRenderSlotMutex between "it is now genuinely our turn" and
	// "the slot is now ours" -- release the ticket and claim the slot
	// under the SAME continuous hold via the shared
	// SubmitAgentRenderAsync_Locked helper (bypassFairQueueCheck=true,
	// since our own not-yet-released ticket would otherwise self-refuse
	// -- see that method's doc).
	bool         accepted = false;
	RenderJobId  jobId    = kInvalidRenderJobId;
	{
		std::unique_lock<std::mutex> slotLk( mAgentRenderSlotMutex );
		const bool onTime = mAgentRenderDoneCV.wait_for( slotLk, std::chrono::milliseconds( timeoutMs ), [&]{
			// Fix-round-1 P1-1: also wake (and give up) on Stop() --
			// mirrors SubmitAgentRenderAsync's own stop refusal, so a
			// sync waiter queued before Stop() is called unblocks
			// honestly instead of waiting out its full timeoutMs.
			return mAgentRenderStop.load( std::memory_order_acquire )
				|| ( !mAgentRenderPending && myTicket == mAgentRenderServingTicket );
		} );
		if( !onTime )
		{
			GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: SubmitAgentRenderSync fairness wait timed out after %ums.", timeoutMs );
			return false;
		}
		const bool stoppedBeforeOurTurn = mAgentRenderStop.load( std::memory_order_acquire );

		// Release OUR ticket now -- still under the SAME lock hold the
		// wait just woke up under, so no OTHER thread's
		// SubmitAgentRenderAsync can observe the released ticket count
		// and act on it until this critical section (which claims the
		// slot a few lines below, still under `slotLk`) has finished.
		++mAgentRenderServingTicket;
		--mAgentRenderWaitingSyncCount;
		ticketGuard.released = true;   // TicketGuard's dtor must not double-release

		if( !stoppedBeforeOurTurn )
		{
			accepted = SubmitAgentRenderAsync_Locked( slotLk, std::move( fn ), clientLabel, &jobId,
				/*bypassFairQueueCheck=*/true, pinned, renderClass );
		}
	}   // slotLk released here -- AFTER both the ticket release and the slot claim
	mAgentRenderDoneCV.notify_all();   // release the NEXT queued waiter (mirrors TicketGuard::Release's own notify)
	if( accepted ) mAgentRenderCV.notify_all();   // wake the worker -- mirrors SubmitAgentRenderAsync's own post-claim notify

	if( !accepted )
	{
		return false;   // either Stop() landed just before our turn, or SubmitAgentRenderAsync_Locked refused (same honest causes as the async entry point)
	}

	if( outJobId ) *outJobId = jobId;

	std::exception_ptr caught;
	{
		std::unique_lock<std::mutex> slotLk( mAgentRenderSlotMutex );
		// Wait until the slot this submission occupied is no longer
		// pending -- i.e. the worker has finished running it.  This is
		// UNBOUNDED (not governed by timeoutMs, which only bounds the
		// fairness wait above) -- a caller that reached the front of the
		// queue always gets its own render's result, however long the
		// render takes, matching the pre-fix contract.  Note this can no
		// longer alias onto a DIFFERENT later submission's completion:
		// with the fairness fix, no other submission (sync OR async) can
		// be accepted into the slot between our SubmitAgentRenderAsync
		// call above and the worker picking it up, because the slot was
		// free and immediately claimed by us.
		mAgentRenderDoneCV.wait( slotLk, [&]{ return !mAgentRenderPending; } );
		caught = mAgentRenderException;
		mAgentRenderException = nullptr;
	}
	if( caught ) std::rethrow_exception( caught );
	return true;
}

// Model-B F2 slice S4: a thin, class-tagged forward onto SubmitAgentRenderSync
// -- see this method's header doc for why a Production submission shares
// EVERY line of the single-slot fairness/refusal/park machinery with an
// agent submission and differs ONLY in the RenderClass tag a status reader
// observes.  `fn` still runs on the dedicated worker thread (uniform with
// agent renders); this call still blocks the caller until `fn` completes.
bool SceneEditController::SubmitProductionRenderSync(
	std::function<void()> fn,
	const String&         clientLabel,
	RenderJobId*          outJobId,
	unsigned int          queueTimeoutMs )
{
	// Design brief A4: a region NEVER leaks into a production render.
	// The production path renders full-frame regardless (it does not
	// read mInteractiveRegion*), but clearing here also stops the
	// post-render interactive restart from refining only a stale box
	// over the finished production image — and keeps the UI badge
	// state honest (the bridges re-query GetInteractiveRegion).
	// Review-round-1 P2: clear WITHOUT the KickRender() the public
	// ClearInteractiveRegion() performs — the shells stop the
	// interactive loop before submitting production work, and waking
	// it here would invite exactly the two-rasterizers-one-scene race
	// that convention prevents.  No repaint is needed either: the
	// production result replaces the framebuffer wholesale.
	mInteractiveRegionActive.store( false, std::memory_order_release );
	return SubmitAgentRenderSync( std::move( fn ), clientLabel, outJobId, queueTimeoutMs,
		/*pinned=*/false, RenderClass::Production );
}

// Fix-round S4-1 (throw-path UAF) + Fix-round 2 (Windows cancel regression):
// see the header doc on the declaration for the full rationale, the
// guard-order proof, the prior-vs-inner contract, and what this replaces in
// the two platform shells.
bool SceneEditController::RunProductionRenderComposed(
	IJobPriv&                     job,
	SceneEditController*          controller,
	const String&                 clientLabel,
	IProgressCallback*            guiProgress,
	const std::function<bool()>& doRasterize,
	unsigned int                  queueTimeoutMs )
{
	if( !controller )
	{
		return doRasterize();
	}

	// RAII restore of the Job's progress-callback slot -- matches the house
	// shape used throughout this file / AgentSession.cpp (Arm/Disarm,
	// restore-on-every-exit-including-a-throw).
	class ProgressRestoreGuard
	{
	public:
		ProgressRestoreGuard( IJobPriv& j, IProgressCallback* priorValue )
			: mJob( j ), mPriorValue( priorValue ), mArmed( false )
		{
		}

		void Arm()    { mArmed = true; }
		void Disarm() { mArmed = false; }

		~ProgressRestoreGuard()
		{
			if( !mArmed ) return;
			try {
				mJob.SetProgress( mPriorValue );
			}
			catch( ... ) {
				GlobalLog()->PrintEx( eLog_Error,
					"SceneEditController::RunProductionRenderComposed: exception escaped the "
					"production-render progress-hook restore -- the Job's progress callback may "
					"be left stale" );
			}
		}

	private:
		ProgressRestoreGuard( const ProgressRestoreGuard& );             // deleted
		ProgressRestoreGuard& operator=( const ProgressRestoreGuard& );  // deleted

		IJobPriv&           mJob;
		IProgressCallback*  mPriorValue;
		bool                mArmed;
	};

	// Fix-round S4-1: the sibling guard the P1 review found missing --
	// clears mCancelProgress's `inner` back to null on every exit, including
	// a throw out of `doRasterize`, so the shared CancellableProgressCallback
	// never outlives the platform-side progress object it was pointed at.
	// Armed in the same breath as ProgressRestoreGuard, immediately after
	// SetInner(prior) below -- see the declaration's header doc for the
	// mMutex-exclusion proof that the destruction order between this guard
	// and ProgressRestoreGuard doesn't matter for correctness.
	class InnerResetGuard
	{
	public:
		explicit InnerResetGuard( CancellableProgressCallback& c )
			: mCoord( c ), mArmed( false )
		{
		}

		void Arm()    { mArmed = true; }
		void Disarm() { mArmed = false; }

		~InnerResetGuard()
		{
			if( !mArmed ) return;
			mCoord.SetInner( nullptr );
		}

	private:
		InnerResetGuard( const InnerResetGuard& );             // deleted
		InnerResetGuard& operator=( const InnerResetGuard& );  // deleted

		CancellableProgressCallback& mCoord;
		bool                         mArmed;
	};

	bool result = false;
	SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;
	const bool submitted = controller->SubmitProductionRenderSync(
		[&]() {
			// Slot-ownership hardening (2026-07-12): capture the RESTORE
			// value HERE, inside the coordinator slot, NOT at function entry
			// on the submitting thread.  The entry-time read ran BEFORE the
			// fairness wait, so it could observe a TRANSIENT occupant -- an
			// agent render's coordProgress installed from the coordinator
			// worker -- and the restore below would then re-install that
			// occupant's callback permanently: it stays in the slot after
			// the render (each later composed render faithfully re-captures
			// and re-restores it), and a Job outliving its controller then
			// carries a dangling pointer into freed controller storage --
			// the next Rasterize would install and drive freed memory.
			// Inside the slot the read is race-free: every other slot
			// writer (agent install/restore, another composed render) runs
			// serialized in this same critical section, and GUI-thread
			// clears are conditional (ClearProgressIfCurrent on their OWN
			// adapter).  See the declaration's header doc for the full
			// prior-vs-inner contract.
			CancellableProgressCallback* coordProgress = static_cast<CancellableProgressCallback*>(
				controller->AgentRenderProgress() );
			// Fix-round 2: compose in the CALLER-SUPPLIED gui progress sink,
			// not job.GetProgress() -- see the declaration's header doc for
			// why those two are not interchangeable on Windows.  SetInner
			// runs BEFORE the exchange below so coordProgress is fully wired
			// by the time it becomes live in the slot.
			coordProgress->SetInner( guiProgress );

			// Review round-2 P1 (same one-atomic-exchange rule as
			// AgentSession's install site -- see the comment there): the
			// prior capture and the coordProgress install are ONE
			// ExchangeProgress call, so a platform adapter's conditional
			// clear can never "succeed" against a pointer this render has
			// already captured as its restore value.
			IProgressCallback* const priorProgress = job.ExchangeProgress( coordProgress );

			ProgressRestoreGuard progressGuard( job, priorProgress );
			progressGuard.Arm();
			InnerResetGuard innerGuard( *coordProgress );
			innerGuard.Arm();

			result = doRasterize();

			// Ordinary-path tail: explicit restore + Disarm, mirroring the
			// house convention elsewhere in this file -- the destructors
			// above are then no-ops here, only actually firing on a throw
			// out of doRasterize().
			job.SetProgress( priorProgress );
			progressGuard.Disarm();
			coordProgress->SetInner( nullptr );
			innerGuard.Disarm();
		},
		clientLabel,
		&jobId,
		queueTimeoutMs );

	if( !submitted )
	{
		// Refused (open transaction, Stop() in progress/racing, a pinned
		// occupant, queued waiters, or a fairness-wait timeout) -- a
		// `controller` exists here (the null-controller case returned
		// directly at the top of this function), and a refusal means it is
		// BUSY or STOPPED, not absent.  Calling `doRasterize` directly in
		// that situation is exactly the pre-S4, uncoordinated shape S4 was
		// written to eliminate: the refusal can legitimately be a slot
		// that is CURRENTLY OCCUPIED by another render (that is the whole
		// reason SubmitProductionRenderSync said no), so an uncoordinated
		// fallback call here could run concurrently with that occupant --
		// two threads inside Rasterize() at once, the exact invariant I1
		// this coordinator exists to prevent (RunDirectRasterizeRedProveTest
		// in tests/AgentRenderAsyncTest.cpp demonstrates the resulting
		// concurrency>=2 on the uncoordinated path). It would also run
		// with zero progress/cancel wiring: `guiProgress` is silently
		// discarded, so the platform's Cancel button and progress/ETA UI
		// go dead for this render -- harmless on macOS (whose
		// BlockProgressCallback is installed persistently on the Job, so a
		// concurrent render happens to still tick it) but not on Windows,
		// where RenderEngine.cpp deliberately skips installing its own
		// callback on the Job whenever a controller is attached (see
		// startRender's comment), so nothing would receive progress/cancel
		// at all for this render.
		//
		// Decision: refuse honestly instead of racing an uncoordinated
		// render.  `doRasterize` is NOT invoked; both platform shells
		// already treat a `false`/`NO` return from this function as an
		// ordinary render failure (mac: RenderViewModel.swift sets
		// `renderState = .error(...)`; Windows: RenderEngine::startRender's
		// completion handler sets `Error` and emits `errorOccurred`) -- so
		// callers already have a well-formed way to surface this without
		// this function inventing a THIRD outcome.
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::RunProductionRenderComposed: production render submission to "
			"the coordinator was refused (busy or stopped) -- refusing rather than running an "
			"uncoordinated fallback Rasterize() call; progress/cancel for this render fall back "
			"to the platform's own callback, if any, since the coordinator never claimed the slot." );
		return false;
	}
	return result;
}

SceneEditController::RenderJobLookup SceneEditController::GetRenderJobStatus( RenderJobId id ) const
{
	RenderJobLookup out;
	if( id == kInvalidRenderJobId ) return out;
	// Parity contract (ce9f5e03): ODD ids are AgentSession's session-local
	// space, never minted by this controller -- reject outright rather
	// than aliasing onto whatever this controller's counter last assigned.
	if( ( id % kControllerRenderJobIdStride ) != 0 ) return out;

	// Model-B F2 slice S2a fix: mJobStatusMutex, not mMutex -- see that
	// member's doc.  This is the exact fix for the flaky "never observed
	// active" bug: mMutex is held by the worker for the whole render, so
	// using it here would block every status poll until the render
	// finished.
	std::lock_guard<std::mutex> lk( mJobStatusMutex );
	if( mCurrentRenderJob.id != id ) return out;   // not the current/most-recent job this controller knows about
	out.found  = true;
	out.status = mCurrentRenderJob;
	return out;
}

// Fix-round-3 (churn UAF): see the header doc for the full contract.  Brief
// mAgentRenderSlotMutex hold, mirroring every other slot-bookkeeping read.
bool SceneEditController::AgentRenderSlotIdleFor_( RenderJobId id ) const
{
	std::lock_guard<std::mutex> slotLk( mAgentRenderSlotMutex );
	return !( mAgentRenderPending && mAgentRenderJobId == id );
}

bool SceneEditController::WaitForRenderJob( RenderJobId id, unsigned int timeoutMs ) const
{
	if( id == kInvalidRenderJobId ) return false;
	if( ( id % kControllerRenderJobIdStride ) != 0 ) return false;   // ODD session-local id -- not ours to wait on

	// mCurrentRenderJob is guarded by mJobStatusMutex (NOT mMutex -- see
	// that member's doc: mMutex is held by the worker for the render's
	// WHOLE DURATION, so polling via mMutex would block this call for the
	// entire render instead of noticing completion promptly -- the same
	// bug class this slice's own flaky test caught for GetRenderJobStatus).
	// The worker's completion signal (mAgentRenderDoneCV) is guarded by
	// the SEPARATE mAgentRenderSlotMutex.  Poll mCurrentRenderJob under
	// mJobStatusMutex in a short loop, sleeping on mAgentRenderSlotMutex's
	// CV between checks.
	//
	// Fix-round-3 (churn UAF): the status record (`active`) is NOT trusted
	// alone anymore.  Fix-round-3's OTHER half (ownership-checked clears at
	// every write site) closes the clobber at the source, but this read
	// side adds a SECOND, independent guard: even if some future write site
	// regresses the ownership check, a caller draining an AGENT job can
	// never observe a false-complete, because completion here additionally
	// requires the agent slot to be idle for `id` -- the ONE piece of state
	// the worker itself mutates strictly AFTER its closure has returned
	// (see AgentRenderSlotIdleFor_'s doc).  For an INTERACTIVE-class id (or
	// any id the agent slot was never tracking), the slot-idle check is
	// vacuously true, so this adds no new waiting for that case.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeoutMs );
	for(;;)
	{
		bool statusSaysDone = false;
		{
			std::lock_guard<std::mutex> lk( mJobStatusMutex );
			statusSaysDone = ( mCurrentRenderJob.id != id ) || !mCurrentRenderJob.active;
		}
		if( statusSaysDone && AgentRenderSlotIdleFor_( id ) ) return true;
		if( timeoutMs == 0 ) return false;              // poll-once contract: no wait
		const auto now = std::chrono::steady_clock::now();
		if( now >= deadline ) return false;
		// Bound each wait slice so we re-check mCurrentRenderJob under
		// mMutex periodically rather than relying on a single CV wait
		// (mAgentRenderDoneCV is signalled by the worker, but the
		// interactive loop's own completion of the SAME id -- if this
		// id happens to be superseded by an interactive pass -- notifies
		// mCV instead; polling on a short bound covers both without
		// taking a dependency on the wrong CV).
		std::unique_lock<std::mutex> slotLk( mAgentRenderSlotMutex );
		const auto sliceEnd = std::min( deadline, now + std::chrono::milliseconds( 20 ) );
		mAgentRenderDoneCV.wait_until( slotLk, sliceEnd );
	}
}

// Model-B F5 slice S2: the shared render-thread-SAFE chunk-CRUD commit.  This
// mirrors ApplyAgentParamEdit step for step (see that method's comments for
// the full rationale of each stage); the differences are (a) the Job primitive
// (ApplyCstInsertChunk / ApplyCstRemoveChunk -- both ALWAYS D2-class, never
// code 1), (b) Job's negative pre-derive refusal codes (-1/-2), normalized
// here to rawCode=0 / status="rejected" with a verb-specific message, and
// (c) the dirty-mark identity, which comes from Job's parsed/resolved
// keyword+name echo rather than the caller's (entityName, entityKind).
SceneEditController::AgentCommitResult SceneEditController::ApplyAgentChunkCrud_(
	bool isInsert,
	const String& a,
	const String& b,
	const RISE::Cst::CstHeadVersion* baseVersionOrNull )
{
	AgentCommitResult r;
	const char* verb = isInsert ? "insert" : "remove";

	// Mid-transaction refusal FIRST, before any mutation or park -- an agent
	// chunk edit has no EditHistory record, so RollbackTransaction could never
	// revert it (same rule + rationale as ApplyAgentParamEdit; same
	// main-thread contract on the unsynchronized mTxnOpen read).
	if( mTxnOpen )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent chunk %s refused inside an open transaction (no EditHistory record -> rollback cannot revert it).", verb );
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.retriable = true;   // the ONE transient reject: retry after the gesture completes
		r.message = String( "editor transaction in progress -- retry after the gesture completes" );
		{
			// Every head read stays under mMutex (no torn 16-byte read).
			std::lock_guard<std::mutex> hlk( mMutex );
			r.headVersion = mJob.GetCstHeadVersion();
		}
		return r;
	}

	// Cancel-and-park, then hold mMutex across the WHOLE commit (pre-flight,
	// conflict gate, the Job apply -- whose D2 ClearAll transiently zeroes the
	// head-version -- the rebind, and the post-commit head read).
	std::unique_lock<std::mutex> lk( mMutex );
	CancelAndParkRender_( lk );

	if( !mJob.HasRetainedCstDocument() )
	{
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.headVersion = mJob.GetCstHeadVersion();
		r.message = String( "no retained CST Document -- agent commit needs a CST-loaded head" );
		return r;
	}
	if( a.size() <= 1 )
	{
		// RString::size() counts the trailing NUL; size <= 1 is the empty case.
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.headVersion = mJob.GetCstHeadVersion();
		r.message = isInsert ? String( "chunkText must be non-empty" )
		                     : String( "target must be non-empty" );
		return r;
	}

	// Optimistic-concurrency CONFLICT precondition, under the lock so the head
	// we compare against is stable.  A stale patch must NEVER touch the Document.
	if( baseVersionOrNull )
	{
		const RISE::Cst::CstHeadVersion cur = mJob.GetCstHeadVersion();
		if( *baseVersionOrNull != cur )
		{
			r.applied     = false;
			r.conflict    = true;
			r.rawCode     = 0;
			r.status      = String( "conflict" );
			r.headVersion = cur;
			char buf[160];
			std::snprintf( buf, sizeof( buf ),
				"baseHeadVersion does not match the current head (revision %llu) -- re-read and re-propose",
				static_cast<unsigned long long>( cur.revision ) );
			r.message = String( buf );
			return r;
		}
	}

	// Shared-undo U2: for a REMOVE, capture the exact bytes + position of the chunk about to be erased --
	// BEFORE the coming Job::ApplyCstRemoveChunk call mutates the Document (no TOCTOU: both run under this
	// same mMutex hold).  Also snapshot the pre-remove item count so the post-remove trim below knows exactly
	// how many top-level items the erase ACTUALLY dropped (1 = chunk only; 2 = chunk + its tidied separator).
	// `haveChunkCapture` false means the target didn't resolve -- ApplyCstRemoveChunk below will independently
	// refuse (negative code), so no history push happens either.
	String capturedBytes;
	int    capturedIndex = 0;
	bool   capturedWasRasterizer = false;
	bool   haveChunkCapture = false;
	int    preRemoveItemCount = 0;
	if( !isInsert )
	{
		haveChunkCapture = CaptureAgentChunkForRemoveUndo_( a, b, capturedBytes, capturedIndex, capturedWasRasterizer );
		preRemoveItemCount = RISE::Cst::DocItemCount( *mJob.GetCstDocument() );
	}

	// The Job primitive (parsing/resolution + duplicate/reference validation +
	// dry-run-guarded full re-derive all live THERE, under this lock).
	char kwBuf[128];   kwBuf[0] = '\0';
	char nameBuf[256]; nameBuf[0] = '\0';
	char diagBuf[512]; diagBuf[0] = '\0';
	// Shared-undo U2 / round-1 P1-A: the top-level index of the splice's OWN LEADING ITEM, filled by
	// ApplyCstInsertChunk itself (the EXACT `at` it used, whichever branch committed) -- NEVER a post-hoc
	// name/kind re-resolution.  A re-resolve-by-(name,kind) after the fact is UNSOUND: a variant-tagged
	// overlay chunk legitimately shares its base chunk's (kind,name) (ApplyCstInsertChunk's own duplicate
	// rule permits it), so DocFindByNameAnyRole sees 2 matches and correctly returns 0 with no kind hint to
	// narrow by -- there IS no principled fallback index in that case, so this stays -1 (Undo skips honestly)
	// rather than guess.
	int insertedAt = -1;
	int code;
	if( isInsert )
	{
		code = mJob.ApplyCstInsertChunk( a.c_str(),
		                                 kwBuf, sizeof( kwBuf ),
		                                 nameBuf, sizeof( nameBuf ),
		                                 diagBuf, sizeof( diagBuf ),
		                                 &insertedAt );
	}
	else
	{
		code = mJob.ApplyCstRemoveChunk( a.c_str(),
		                                 b.size() <= 1 ? nullptr : b.c_str(),
		                                 kwBuf, sizeof( kwBuf ),
		                                 diagBuf, sizeof( diagBuf ) );
		// The remove target IS the chunk name (bare-name addressing).
		std::snprintf( nameBuf, sizeof( nameBuf ), "%s", a.c_str() );
	}
	r.chunkKeyword = String( kwBuf );
	r.chunkName    = String( nameBuf );

	// Shared-undo U2: trim the conservative two-item over-capture down to only what the erase ACTUALLY
	// dropped, by diffing the item count across the just-completed remove (only meaningful when the remove
	// landed -- codes 2/3 below).  `droppedCount` is 1 (chunk only) or 2 (chunk + its tidied separator);
	// re-serialize the (now-current, post-remove) Document is NOT an option here (the items are gone) -- but
	// `capturedBytes` already holds BOTH items' text concatenated in order, so re-parsing it and re-emitting
	// only the first `droppedCount` items' bytes yields exactly the substring that was actually removed.
	if( !isInsert && haveChunkCapture && ( code == 2 || code == 3 ) )
	{
		const int postRemoveItemCount = RISE::Cst::DocItemCount( *mJob.GetCstDocument() );
		const int droppedCount = preRemoveItemCount - postRemoveItemCount;
		if( droppedCount == 1 )
		{
			// Only the chunk itself was dropped -- re-parse the two-item capture and keep just item 0's bytes.
			const RISE::Cst::Document capDoc = RISE::Cst::ParseToCst( std::string( capturedBytes.c_str() ) );
			const RISE::Cst::NodeRef  first  = RISE::Cst::DocResolveNodeId( capDoc, RISE::Cst::DocNodeIdAt( capDoc, 0 ) );
			capturedBytes = String( RISE::Cst::SerializeNode( first ).c_str() );
		}
		// droppedCount == 2 (or, defensively, anything else): keep the full two-item capture as-is -- either
		// it is exactly right (2) or the diff was unexpected (should not happen; erring toward the
		// conservative superset is safer than under-capturing and losing bytes on Undo).
	}

	// A chunk CRUD that landed is ALWAYS a D2 full re-derive (codes 2/3): the
	// Scene + managers were replaced, so re-point the editor's cached pointers
	// BEFORE releasing the lock (same rebind rule as ApplyAgentParamEdit).
	if( code == 2 || code == 3 )
	{
		RebindEditorToJob();
	}

	// Fold the code.  Negative pre-derive refusals normalize to rawCode 0 --
	// the wire contract keeps rawCode in {0,1,2,3} -- with a specific message.
	r.rawCode = ( code < 0 ) ? 0 : code;
	switch( code )
	{
		case 2:
			r.applied = true;
			r.status  = String( "applied" );
			r.message = isInsert
				? String( "chunk inserted via a full re-derive (Scene + managers were replaced)" )
				: String( "chunk removed via a full re-derive (Scene + managers were replaced)" );
			break;
		case 3:
			r.applied = false;
			r.status  = String( "diagnosed" );
			r.message = String( "edit NOT a clean success: the Document was mutated and the live managers were "
			                    "replaced, BUT the full re-derive emitted diagnostics (see log) -- do NOT treat as applied" );
			break;
		case -1:
		{
			r.applied = false;
			r.status  = String( "rejected" );
			std::string m;
			if( isInsert ) {
				m = "insert rejected: chunkText must parse to exactly ONE complete chunk (`keyword { ... }`, braces on their own lines; no scene header/directives)";
				if( diagBuf[0] ) { m += ": "; m += diagBuf; }
			} else if( diagBuf[0] ) {
				// A diagnosed -1 (e.g. the kind-verification refusal: the name
				// resolved but to a DIFFERENT kind) carries its own reason.
				m = std::string( "remove rejected: " ) + diagBuf + " -- head unchanged";
			} else {
				m = "remove rejected: no chunk named '";
				m += a.c_str();
				m += "' found -- head unchanged";
			}
			r.message = String( m.c_str() );
			break;
		}
		case -2:
		{
			r.applied = false;
			r.status  = String( "rejected" );
			std::string m;
			if( isInsert ) {
				// Round-3: a "reserved name"-prefixed diag (Job's `name none`
				// refusal) is NOT a chunk collision -- surface the real cause
				// verbatim instead of the misleading "already exists" claim.
				// Prefix kept in lockstep with Job::ApplyCstInsertChunk (and
				// the identical fold in AgentSession.cpp).
				const std::string dstr = diagBuf[0] ? std::string( diagBuf ) : std::string();
				if( dstr.compare( 0, 13, "reserved name" ) == 0 ) {
					m = "insert rejected: " + dstr + " -- head unchanged";
				} else {
					m = "insert rejected: a chunk with the same kind and name already exists";
					if( diagBuf[0] ) { m += " ("; m += diagBuf; m += ")"; }
					m += " -- head unchanged";
				}
			} else {
				// Round-2 P3: the hint is CONDITIONAL on whether the caller
				// already narrowed -- repeating "pass `kind`" when kind WAS
				// passed is a dead-end instruction.
				const bool kindPassed = ( b.size() > 1 );
				m = "remove rejected: name '";
				m += a.c_str();
				m += kindPassed
					? "' is ambiguous even under that kind -- pass a more specific kind"
					: "' is ambiguous -- pass `kind` to narrow";
				if( diagBuf[0] ) { m += " ("; m += diagBuf; m += ")"; }
			}
			r.message = String( m.c_str() );
			break;
		}
		case 0:
		default:
		{
			r.applied = false;
			r.status  = String( "rejected" );
			// Round-2 P1-A: name BOTH would-not-derive causes honestly -- the
			// old "likely still REFERENCED" wording sent agents chasing a
			// phantom consumer when the real cause was an order-invalid head.
			std::string m = isInsert
				? std::string( "insert rejected: the chunk would not derive in context -- head unchanged" )
				: std::string( "remove rejected: removing '" ) + a.c_str() + "' would not derive (it is likely still REFERENCED by another chunk, or the remaining document no longer derives in order -- read_document and validate to inspect) -- head unchanged";
			if( diagBuf[0] ) { m += ": "; m += diagBuf; }
			r.message = String( m.c_str() );
			break;
		}
	}

	// Post-commit head, read under the lock (coherent with a just-rebuilt Job).
	r.headVersion = mJob.GetCstHeadVersion();

	// Dirty-mark + re-render kick whenever the live Scene CHANGED (codes 2/3;
	// a diagnosed code-3 ALSO mutated the Document -- see ApplyAgentParamEdit's
	// data-loss rationale).  MarkCstHeadDirty tolerates an empty name (boolean
	// CST-head channel) and an unknown kind, so an unnamed inserted chunk
	// (e.g. `film`) still marks the editor unsaved.
	if( code == 2 || code == 3 )
	{
		mEditor.MarkCstHeadDirty( nameBuf, kwBuf[0] ? kwBuf : nullptr );

		// Shared-undo U2: push an EditHistory record so this agent chunk-CRUD commit is a first-class
		// undo/redo citizen -- the headline gap this slice closes (previously Cmd-Z skipped a chunk insert/
		// remove entirely, and the chunk-CRUD-is-history-invisible hazard was the documented reason
		// RouteCstParamEditChecked_ exists).  Pushed for EVERY mutating code, INCLUDING 3 (diagnosed-but-
		// mutated), matching ApplyAgentParamEdit's own rationale: the Document WAS mutated (revision bumped,
		// MarkCstHeadDirty above just marked it unsaved) -- an un-revertable mutated Document is strictly
		// worse than a revertable one.  For an INSERT, `a` IS the chunk text the agent supplied (Redo replays
		// it verbatim).  For a REMOVE, `haveChunkCapture` should always be true here (the capture ran under
		// this same lock hold against the SAME Document ApplyCstRemoveChunk just resolved against to reach a
		// mutating code); a defensive skip-with-log guards against ever pushing a garbage/empty capture.
		if( isInsert )
		{
			// P1-A fix: `insertedAt` is ApplyCstInsertChunk's OWN out-param -- the exact top-level index of
			// the splice's leading item ([leadSep][chunk][trailSep], `insertedAt` == the lead separator's own
			// index), never a post-hoc re-resolution.  It is ALWAYS >= 0 on a landed insert (codes 2/3 are the
			// only way this branch is reached, and the primitive sets it unconditionally on both of its landing
			// paths) -- but if it somehow were not (defensive; should not happen), pushing a GUESSED index
			// would let a future Undo silently splice-and-remove the WRONG three items (no identity check at
			// the primitive level beyond bounds).  SKIP the push honestly instead, matching the remove branch's
			// own haveChunkCapture-false skip below.
			if( insertedAt >= 0 )
			{
				mEditor.PushAgentChunkCrudEdit( /*isInsert*/ true, r.chunkName, r.chunkKeyword, a,
				                                /*docIndex*/ insertedAt, /*wasRasterizer*/ false );
			}
			else
			{
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditController: agent chunk insert of `%s` applied (code %d) but ApplyCstInsertChunk "
					"did not report its committed splice index -- NO undo history record was pushed for this "
					"insert (should not happen: the primitive sets it unconditionally on every landing code).",
					r.chunkName.c_str(), code );
			}
		}
		else if( haveChunkCapture )
		{
			mEditor.PushAgentChunkCrudEdit( /*isInsert*/ false, r.chunkName, r.chunkKeyword, capturedBytes,
			                                capturedIndex, capturedWasRasterizer );
		}
		else
		{
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController: agent chunk remove on `%s` applied (code %d) but the prior-bytes capture "
				"failed -- NO undo history record was pushed for this remove (should not happen: the apply just "
				"resolved the same target under the same lock).",
				a.c_str(), code );
		}

		// A chunk was added/removed -- the entity set changed, so bump the
		// scene epoch that live GUI outliners cache against to re-enumerate.
		// Covers GUI entity creation (Instantiate/Duplicate/RemoveEntity)
		// AND agent-driven insert_chunk/remove_chunk uniformly, since both
		// route through here.
		mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
	}
	return r;
}

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

// Phase 6.5 (docs/ROUND_TRIP_SAVE_PLAN.md §9.9): save the scene via
// SaveEngine::Save -- an unconditional whole-Document SerializeCst
// (NoOps on byte-equality; dirty state selects nothing, it only gates
// the Save button).  Three-step cancel-and-park dance:
//   1. Park the render thread (cancel in-flight + wait for
//      mRendering=false), set mSaving=true.
//   2. Run SaveEngine outside the lock (file IO is slow).
//   3. Reacquire lock, clear mSaving, capture any error, notify the
//      render loop.
//
// The live SceneEditor exposes `mScaleFromAnchorSet` const-only
// (ScaleFromAnchorSet()), while the SaveEngine takes a non-const
// reference it clears post-save.  We copy into a local, pass that to
// the engine, then mirror its clear back onto the editor's state via
// ClearDirtyState() on success.
SaveResult SceneEditController::RequestSave( const std::string& filePath )
{
	// ---- Step 1: park render thread + mark save in flight -----------
	// Editor live-sync follow-up: remember whether we CANCELLED an
	// in-flight pass to get here — that pass's frame is left partially
	// updated, and with debounced auto-save (UI refinement item 1)
	// this now happens routinely, not just on a manual Save click.
	// Step 3 re-kicks a render when it does, so a save never strands a
	// half-painted viewport.
	bool interruptedPass = false;
	{
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load() ) {
			mCancelProgress.RequestCancel();
			interruptedPass = true;
		}
		mCV.wait( lk, [&]{ return !mRendering.load(); } );
		mSaving.store( true );
	}

	// Fix-round-6 RED-PROVE test seam -- no-op in production (see the
	// declaration doc).  Sits right after mSaving flips true and mMutex
	// releases, so a test override can block here to hold mSaving true
	// for as long as it needs, giving RenderLoop's mint-site re-check a
	// deterministic (rather than IO-timing-dependent) window to prove
	// itself against.
	ForTest_OnSaveEngineAboutToRun();

	// ---- Step 2: run SaveEngine WITHOUT the lock --------------------
	// File IO can take a few ms; holding mMutex across that would
	// stall any other UI-state transition that needs the lock.
	std::unordered_set<std::string> sfaCopy = mEditor.ScaleFromAnchorSet();
	SaveEngine engine(
		mJob,
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
		if( interruptedPass ) {
			// Repaint the pass this save cancelled (KickRender's shape:
			// flag + notify under the SAME mutex the waiter blocks on).
			mEditPending.store( true, std::memory_order_release );
		}
		mCV.notify_one();
	}

	return result;
}

String SceneEditController::SerializedSceneText() const
{
	std::lock_guard<std::mutex> lk( mMutex );
	if( !mJob.HasRetainedCstDocument() ) return String();
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return String();
	const std::string bytes = RISE::Cst::SerializeCst( *doc );
	return String( bytes.c_str() );
}

void SceneEditController::GetSceneTextVersion( std::uint64_t& outUuid,
                                               std::uint64_t& outRevision ) const
{
	std::lock_guard<std::mutex> lk( mMutex );
	const RISE::Cst::CstHeadVersion v = mJob.GetCstHeadVersion();
	outUuid     = v.uuid;
	outRevision = v.revision;
}

namespace {

// "Reveal in scene file": the CST role-kind-suffix + unique-fallback DocFindByNameAnyRole
// needs to resolve a Category's entity name to its chunk, mirroring the SAME table
// SceneEditor::ClassifyCstEntityKind walks in REVERSE (CST role string -> EntityCategory) --
// see that function's suffix list (endsWith "_camera"/"_light"/"_material"/"_medium", or the
// bare "camera"/"material" role). Object's role is exactly "standard_object" (no suffix
// family), so its "suffix" is the whole role name -- DocFindByNameAnyRole's `role ==
// roleKindSuffix` arm matches it directly.
//
// Animation/SceneVariant are ALWAYS-NAMED top-level chunks (role "animation" /
// "scene_variant" exactly) with no cross-category collision risk, so no unique-fallback
// is needed for them either.
//
// Rasterizer and Film are declared NOT addressable here, on purpose: CategoryEntityName /
// CategoryActiveName for Rasterizer return REGISTRY TYPE names (the picker list of
// available rasterizer kinds), not a chunk's `name` param -- a rasterizer chunk in the
// scene is normally unnamed and singleton (the "_rasterizer"-suffixed active integrator).
// Film's CategoryActiveName returns a synthetic PRESET LABEL ("1080p" etc.), not a name
// either. Resolving either against DocFindByNameAnyRole's `bareName` parameter would be
// searching for the wrong string. A future extension COULD special-case Rasterizer/Film
// through the SAME unique-fallback path SceneEditController::Category::Camera already
// uses for an unnamed sole camera (both chunks are typically singleton `role ==
// roleKindSuffix` chunks) -- deferred; not wired today, so EntitySourceLocation refuses
// them cleanly (returns false) rather than guessing.
bool RoleKindSuffixForCategory( SceneEditController::Category cat, std::string& outSuffix, bool& outUniqueFallback )
{
	using Category = SceneEditController::Category;
	outUniqueFallback = false;
	switch( cat ) {
	case Category::Camera:       outSuffix = "camera";         outUniqueFallback = true; return true;   // same unnamed-active-camera fallback as CaptureAgentPriorParamValue_
	case Category::Object:       outSuffix = "standard_object"; return true;
	case Category::Light:        outSuffix = "light";           return true;
	case Category::Material:     outSuffix = "material";        return true;
	case Category::Medium:       outSuffix = "medium";          return true;
	case Category::Animation:    outSuffix = "animation";       return true;
	case Category::SceneVariant: outSuffix = "scene_variant";   return true;
	case Category::Painter:      outSuffix = "painter";         return true;   // matches every "*_painter" chunk keyword (endsWith), same convention as ClassifyCstEntityKind's "_material"/"_light"/"_medium" suffixes
	case Category::Rasterizer:
	case Category::Film:
	case Category::None:
	default: return false;   // no chunk-name addressing scheme for this category -- see comment above
	}
}

}  // namespace

bool SceneEditController::EntitySourceLocation( Category cat, const String& name,
                                                 std::uint64_t& outByteOffset, std::uint32_t& outLine ) const
{
	std::lock_guard<std::mutex> lk( mMutex );
	if( !mJob.HasRetainedCstDocument() ) return false;
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return false;

	std::string suffix; bool uniqueFallback = false;
	if( !RoleKindSuffixForCategory( cat, suffix, uniqueFallback ) ) return false;
	if( name.size() <= 1 && !uniqueFallback ) return false;   // an empty name only resolves via the unique-fallback (unnamed sole entity of its kind)

	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( *doc, name.c_str(), nullptr, suffix, uniqueFallback );
	if( id == 0 ) return false;   // absent, or ambiguous -- refuse rather than guess (same contract as DocFindByNameAnyRole itself)

	RISE::Cst::NodeRef item;
	const int idx = RISE::Cst::DocIndexOfNodeId( *doc, id, &item );
	if( idx < 0 ) return false;   // resolved id is not (any longer) a top-level item

	const size_t off = RISE::Cst::DocByteOffsetOfItem( *doc, idx );
	if( off == static_cast<size_t>( -1 ) ) return false;

	// Line number: 1-based count of '\n' bytes in the serialized PREFIX before `off`.
	// SerializeCst has no partial/prefix-only form, so this walks the WHOLE doc's bytes
	// (O(doc bytes)) -- documented above as fine at click/selection cadence, not a
	// per-frame path.
	const std::string full = RISE::Cst::SerializeCst( *doc );
	if( off > full.size() ) return false;   // defensive: should be unreachable given DocByteOffsetOfItem's own bound
	unsigned int line = 1;
	for( size_t i = 0; i < off; ++i ) if( full[i] == '\n' ) ++line;

	outByteOffset = static_cast<std::uint64_t>( off );
	outLine       = line;
	return true;
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
	// Initial render so the user sees something on Start — UNLESS the
	// caller asked us to keep the current on-screen image (Start( true )).
	// The GUI does that when it restarts the interactive viewport right
	// after a production render: the finished render is already on screen
	// and a fresh preview pass would immediately overwrite it (the
	// "render flashes then flips back to the live preview" bug).  When
	// suppressed we stay parked on the cv.wait below until the first real
	// edit / gesture arrives, so the production image survives until the
	// user actually interacts.  Consume the one-shot flag either way.
	if( !mSuppressInitialRender.exchange( false, std::memory_order_acq_rel ) )
	{
		mEditPending.store( true, std::memory_order_release );
	}

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
			// Model-B F2 slice S2a: mAgentRenderBlocksInteractive is the
			// SAME gate for the agent-render worker's window -- see that
			// member's doc for the race it closes.  Loop-resume is
			// automatic via the worker's cv.notify_all() when it clears
			// the flag.
			auto pred = [&]{
				return ( mEditPending.load( std::memory_order_acquire )
				      && !mSaving.load( std::memory_order_acquire )
				      && !mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
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

		// Model-B F2 slice S2a: the idle-refinement path's wait_for can
		// return on a bare TIMEOUT (pred never satisfied) even while
		// mAgentRenderBlocksInteractive is set -- pred only gates the
		// EXPLICIT-edit wake, not the timeout wake.  Re-check explicitly
		// here, before ANY mint/pass-start logic below, so a refinement
		// tick can never race the agent-render worker's window either.
		// mEditPending was NOT consumed on this path (exchange above
		// only fires when pred was satisfied), so looping back costs
		// nothing -- the edit (if any) is still pending.  Contrast with
		// the mint-site re-check below (Fix-round-5): THAT site is
		// reached after the exchange has already run and DID consume a
		// pending edit whenever isExplicitEdit is true, so bouncing there
		// needs an explicit restore this site does not.
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) continue;

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

			unsigned int s = mPreviewScale.load( std::memory_order_acquire );
			if( s <= kPreviewScaleMin ) continue;
			// Review-round-1 P2: CAS, not a blind store.  A gesture-end
			// reset (OnPointerUp / OnTimeScrubEnd / EndPropertyScrub all
			// store kPreviewScaleMin from the UI thread) landing between
			// the load above and a blind store(s/2) would be silently
			// overwritten -- leaving the scale resting above min with no
			// future wake to walk it down (the refine-mode predicate
			// needs an active gesture), i.e. a stuck-coarse preview and
			// a status readout stuck on "Refining".  On CAS failure the
			// reset (or a new gesture's motion bump) won; just loop.
			if( !mPreviewScale.compare_exchange_strong(
					s, s / 2,
					std::memory_order_acq_rel, std::memory_order_acquire ) )
			{
				continue;
			}
			mInRefinementPass = true;
			// P2 (accepted, not fixed): if the mint-site re-check further
			// below bounces this same iteration (mAgentRenderBlocksInteractive
			// set), this halving already landed but the pass that would have
			// rendered at the new scale is skipped for this iteration.  The
			// next refinement tick halves again rather than rendering the
			// skipped level once more -- self-healing (the preview still
			// converges to kPreviewScaleMin, just one tick later than usual)
			// and only reachable while an agent render overlaps a live
			// idle-refinement walk-down, so it is left as-is rather than
			// adding a second piece of state to track "redo this level".
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

		// Fix-round-4 P2 RED-PROVE test seam -- no-op in production (see
		// the declaration doc).  Sits at the END of the unlocked window
		// between the line-4473 gate snapshot and the mint block's own
		// in-lock re-check just below, so a test override can block here
		// to force an agent mint to land in that exact gap.
		ForTest_OnAboutToMintInteractivePass();

		// Both rendering transitions are serialised under mMutex so
		// OnTimeScrub (which mutates scene state that DoOneRenderPass
		// is reading) can hold the lock across its wait+mutate+kick
		// window and be sure the render thread can't observe an
		// in-progress mutation.  Setting rendering=true while holding
		// the lock blocks the render thread from beginning a new
		// pass while the main thread is partway through a mutation
		// it queued before the previous pass's notify woke us.
		// Fix-round-3 (churn UAF): remember the id THIS PASS mints so the
		// completion write below can be OWNERSHIP-CHECKED -- see that
		// site's comment for the clobber this closes.
		//
		// Fix-round-4 P2: the line-4473 check above is a SNAPSHOT taken
		// before ~60 unlocked lines of refinement/polish bookkeeping ran
		// -- SubmitAgentRenderAsync_Locked can mint its own AgentPreview
		// record into mCurrentRenderJob and only THEN set
		// mAgentRenderBlocksInteractive in the gap between that snapshot
		// and this mMutex acquisition, and nothing between the two sites
		// re-checked the flag.  Result: this block would mint a fresh
		// Interactive record unconditionally and stomp the agent job's
		// record the moment it landed -- the same clobber the mint-site
		// doc (mAgentRenderBlocksInteractive's declaration comment) claims
		// is structurally impossible.  Re-check the flag HERE, inside the
		// same mMutex hold this block mints under, immediately before the
		// mint.  This closes the window without changing what the flag
		// means: it still gates "may an interactive pass mint/start", it
		// now just gates it at the ONE site that actually mints, not only
		// at an earlier snapshot that unlocked code could invalidate.
		//
		// Fix-round-5 (lost-edit wedge): UNLIKE the line-4473 bounce, this
		// site's `continue` is NOT free.  The exchange at line 4467
		// already consumed mEditPending (isExplicitEdit is its result) --
		// by the time we reach this re-check, the flag is false regardless
		// of whether the wake was a real edit.  Bouncing here without
		// restoring it drops the edit on the floor: the agent worker's
		// completion path (AgentRenderWorkerLoop_) clears
		// mAgentRenderBlocksInteractive and notify_all()s, but never sets
		// mEditPending, so the wake predicate stays false and this loop
		// parks forever -- the user's edit never gets an interactive pass,
		// and the viewport stays stale until some UNRELATED later edit
		// happens to arrive.  Restore the flag before bouncing so the next
		// wake re-exchanges it exactly as if this iteration never ran.
		// Re-set it as an explicit edit (not a refinement tick): the
		// original wake that produced `isExplicitEdit` here was itself
		// either a real edit (true) or a refinement-tick timeout (false,
		// and mEditPending was already false going in -- restoring false
		// is a no-op). mLastEditTimeMs is untouched on purpose: it was
		// already stamped at the ORIGINAL edit-submission call site (e.g.
		// KickRender/OnTimeScrub), strictly before mEditPending was set --
		// see KickRender's own comment for that happens-before edge -- so
		// it is still correct for the retried iteration and restamping it
		// here would just be a no-op race with a concurrent submitter.
		//
		// Fix-round-6 (save-vs-render race): the line-4473 gate snapshot
		// ALSO reads mSaving as part of `pred`, but that's the same kind
		// of snapshot Fix-round-4 P2 already found unsafe for the agent
		// flag -- taken before ~60 unlocked lines run, and RequestSave can
		// acquire mMutex and set mSaving=true anywhere in that window (its
		// own step 1 does exactly that under mMutex, same as
		// SubmitAgentRenderAsync_Locked's mint+flag-set).  Nothing between
		// the line-4473 snapshot and this mMutex acquisition re-checked
		// mSaving, so an interactive pass could mint and run DoOneRenderPass
		// (reading frame-store state) CONCURRENTLY with SaveEngine::Save()'s
		// unlocked file IO reading that same state -- structurally the
		// identical clobber class as the agent-flag race, just against the
		// save path instead of the agent-render path.  Re-check mSaving HERE
		// too, inside the same mMutex hold, with the identical conditional-
		// restore treatment: this bounce ALSO runs after the line-4467
		// exchange has already consumed mEditPending, so it needs the same
		// restore-if-explicit-edit rescue Fix-round-5 added for the agent
		// flag, or a save that lands in this exact window would wedge the
		// loop the same way.  Loop-resume is automatic: RequestSave's step 3
		// clears mSaving and notify_one()s under mMutex (see that method),
		// so the restored edit (or a genuine refinement-tick re-arrival)
		// re-wakes this loop the moment the save publishes its result.
		RenderJobId thisPassJobId = kInvalidRenderJobId;
		{
			std::lock_guard<std::mutex> lk( mMutex );
			if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire )
			 || mSaving.load( std::memory_order_acquire ) )
			{
				if( isExplicitEdit )
				{
					mEditPending.store( true, std::memory_order_release );
				}
				continue;
			}
			mCancelProgress.Reset();
			mRendering.store( true, std::memory_order_release );
			// Model-B F2 slice S1: render-identity bookkeeping.  Model-B
			// F2 slice S2a fix: mCurrentRenderJob/mNextRenderJobId moved
			// to their OWN mJobStatusMutex (nested here) -- see that
			// member's doc for why (a status reader must never block
			// behind mMutex's render-duration hold; this class's own
			// mMutex hold here is brief -- just this block -- but the
			// AGENT worker's mMutex hold spans the whole render, so the
			// job-status record needs a lock that NEITHER path ever holds
			// that long).  This is the interactive preview pass, so
			// RenderClass::Interactive always.
			{
				std::lock_guard<std::mutex> statusLk( mJobStatusMutex );
				mCurrentRenderJob.id          = mNextRenderJobId += kControllerRenderJobIdStride;
				mCurrentRenderJob.renderClass = RenderClass::Interactive;
				mCurrentRenderJob.active      = true;
				mCurrentRenderJob.clientLabel = String();   // Fix-round-1 P3-c: an Interactive-class job has no client label -- clear any stale value left by a prior AgentPreview job record
				thisPassJobId = mCurrentRenderJob.id;
			}
		}

		// Fix-round-4 P3-1: RAII active-flip guard around the pass itself,
		// matching the shape RunPreviewRenderParked's ActiveFlipGuard and
		// the agent worker's ActiveFlipGuard already use -- WITHOUT this,
		// an exception out of DoOneRenderPass() (OIDN denoise is a
		// DOCUMENTED real throw site -- see PixelBasedRasterizerHelper.cpp's
		// FrameStoreBulkBracket comment -- and the refinement/polish paths
		// above do turn preview denoise on) would skip both the
		// `mRendering.store( false, ... )` and the ownership-checked
		// `active = false` clear below, leaving THIS pass's job record
		// (and mRendering) stuck true forever -- every later status poll
		// for this job would report "still running" permanently, and
		// CancelAndParkRender_ callers (OnTimeScrub, RunPreviewRenderParked,
		// the agent worker, Stop()) would block forever waiting for
		// mRendering to go false.  Scoped as tightly as possible (this
		// nested block, not the whole loop body) so its destructor fires
		// at EXACTLY the same point the old unconditional completion write
		// used to -- right after DoOneRenderPass()/mRenderCount, still
		// BEFORE the polish-pass post-roll below -- so this fix changes
		// nothing about the non-exceptional timing other callers observe.
		// Honest reachability note: RenderLoop runs as `mRenderThread`'s
		// top-level entry point with no surrounding try/catch (see the
		// ctor's `std::thread( &RenderLoop, this )`), so on the common
		// standard-library implementations an exception that unwinds all
		// the way out of a thread's entry function invokes
		// std::terminate() WITHOUT running stack unwinding (this guard's
		// destructor would not get a chance to run either) -- today's
		// actual failure mode for an uncaught throw here is process
		// termination, not a stuck flag.  The guard is added anyway, for
		// the same reason RunPreviewRenderParked's and the agent worker's
		// already exist: it is cheap, it makes all three job-opening sites
		// structurally consistent, and it is the correct shape regardless
		// of whether a future refactor (or a platform where thread-entry
		// exceptions DO unwind) changes today's terminate-not-unwind
		// behaviour.
		{
			struct ActiveFlipGuard {
				SceneEditController& self;
				RenderJobId          jobId;
				~ActiveFlipGuard() {
					std::lock_guard<std::mutex> lk( self.mMutex );
					self.mRendering.store( false, std::memory_order_release );
					{
						// Same ownership-checked clear as fix-round-3 (churn
						// UAF) used unconditionally at this same site -- see
						// that fix's history for the clobber this guards
						// against.
						std::lock_guard<std::mutex> statusLk( self.mJobStatusMutex );
						if( self.mCurrentRenderJob.id == jobId )
						{
							self.mCurrentRenderJob.active = false;
						}
					}
					self.mCV.notify_all();
				}
			} activeFlipGuard{ *this, thisPassJobId };

			DoOneRenderPass();
			mRenderCount.fetch_add( 1, std::memory_order_acq_rel );
		}

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
	case Category::Painter:
		// Entity-creation slice: no dedicated PanelMode::Painter this
		// slice (PropertyCountFor/PropertyNameFor/etc. read
		// mPropertiesByCategory[Category::Painter] directly, indexed by
		// Category rather than by the "current panel" mirror below --
		// see buildRowsFor's Painter case and this method's own header
		// doc on Category::Painter).
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
		case Category::Painter: {
			// Entity-creation slice: rows are CST-chunk-sourced (there is
			// no live per-parameter getter surface on IPainter/
			// IScalarPainter) -- see PainterIntrospection.h's header doc.
			if( selName.size() <= 1 ) break;
			out = PainterIntrospection::Inspect( mJob.GetCstDocument(), selName );
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
	// Pre-existing bug fixed in passing: this bound was hardcoded to 9
	// (stale even before this change -- SceneVariant=9 was already
	// silently excluded from PropertyCountFor/PropertyNameFor/etc.).
	// kNumCategories is private (this is a free function, not a
	// member), so mirror its value here with an explicit comment
	// rather than hardcoding a now-also-stale literal.
	if( i < 0 || i >= 11 ) return 0;   // 11 == SceneEditController::kNumCategories (None..Painter)
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

// ===================== Free-fly viewport pose (Tier 2 §5.3-5.5) =================

bool SceneEditController::EnterFreeFlyFromActiveCamera()
{
	// Capture the active camera's full pose+optics, then set it as the transient
	// pose (SetViewportPose realizes the override).  The capture is done under the
	// same cancel-and-park CloneActiveCamera uses (the snapshot fields tear under a
	// concurrent orbit-drag write), so read it there rather than here.
	CameraSnapshot snap;
	{
		std::unique_lock<std::mutex> lk( mMutex );
		const IScene* scene = mJob.GetScene();
		if( !scene ) return false;
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		const ICamera* activeCam = scene->GetCamera();
		if( !activeCam ) return false;
		if( !CameraIntrospection::CaptureCameraSnapshot( *activeCam, snap ) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController::EnterFreeFlyFromActiveCamera: active camera kind not realizable "
				"(non-ONB Pinhole / ThinLens / Fisheye / Orthographic only)" );
			return false;
		}
	}
	return SetViewportPose( snap );
}

bool SceneEditController::SetViewportPose( const CameraSnapshot& pose )
{
	std::unique_lock<std::mutex> lk( mMutex );
	const IScene* scene = mJob.GetScene();
	if( !scene ) return false;

	// Cancel-and-park: the render thread reads mViewportOverrideCamera at the top
	// of a pass; we must swap it while no pass is in flight.
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	ICamera* realized = RealizeStandaloneCamera( pose, *scene );
	if( !realized ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::SetViewportPose: could not realize a camera for the pose (unknown kind / no film)" );
		return false;
	}

	// Swap in the new override, releasing any prior one.
	if( mViewportOverrideCamera ) mViewportOverrideCamera->release();
	mViewportOverrideCamera = realized;   // owns the factory's addref
	mViewportPose           = pose;
	mViewportPoseActive     = true;

	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::ExitFreeFly()
{
	std::unique_lock<std::mutex> lk( mMutex );
	if( !mViewportPoseActive ) return false;

	// Park before releasing the override the render thread reads.
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	if( mViewportOverrideCamera ) {
		mViewportOverrideCamera->release();
		mViewportOverrideCamera = nullptr;
	}
	mViewportPoseActive = false;

	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::IsFreeFlyActive() const
{
	std::lock_guard<std::mutex> lk( mMutex );
	return mViewportPoseActive;
}

bool SceneEditController::GetViewportPose( CameraSnapshot& out ) const
{
	std::lock_guard<std::mutex> lk( mMutex );
	if( !mViewportPoseActive ) return false;
	out = mViewportPose;
	return true;
}

// ===================== Axis snaps + Home (Tier 2 §4.2) =========================

bool SceneEditController::SnapViewToAxis( int axis, bool negative )
{
	if( axis < 0 || axis > 2 ) return false;

	// Source pose: the current free-fly pose, or (auto-enter) the active camera.
	CameraSnapshot src;
	if( IsFreeFlyActive() ) {
		if( !GetViewportPose( src ) ) return false;
	} else {
		if( !EnterFreeFlyFromActiveCamera() ) return false;
		if( !GetViewportPose( src ) ) return false;   // now seeded from the active camera
	}

	// Pivot = the current lookat; preserve the eye->pivot distance and the optics.
	const double pivot[3] = { src.lookat[0], src.lookat[1], src.lookat[2] };
	const double dx = src.location[0] - pivot[0];
	const double dy = src.location[1] - pivot[1];
	const double dz = src.location[2] - pivot[2];
	double dist = std::sqrt( dx*dx + dy*dy + dz*dz );
	if( dist < 1e-6 ) dist = 5.0;   // degenerate (eye == pivot) -> a sane default distance

	const double sign = negative ? -1.0 : 1.0;

	CameraSnapshot snap = src;   // keep kind + optics
	snap.location[0] = pivot[0]; snap.location[1] = pivot[1]; snap.location[2] = pivot[2];
	snap.location[axis] += sign * dist;
	snap.lookat[0] = pivot[0]; snap.lookat[1] = pivot[1]; snap.lookat[2] = pivot[2];
	// Up: world-Y for the X/Z axes; for the Y axis (top/bottom) use -/+Z so up is
	// not parallel to the view direction.
	snap.up[0] = 0.0; snap.up[1] = 0.0; snap.up[2] = 0.0;
	if( axis == 1 ) snap.up[2] = negative ? 1.0 : -1.0;
	else            snap.up[1] = 1.0;
	// A clean axis-aligned view drops any extra euler / target orientation.
	snap.orientation[0] = snap.orientation[1] = snap.orientation[2] = 0.0;
	snap.target_orientation[0] = snap.target_orientation[1] = 0.0;

	return SetViewportPose( snap );
}

bool SceneEditController::SetHomeView()
{
	CameraSnapshot cur;
	if( IsFreeFlyActive() ) {
		if( !GetViewportPose( cur ) ) return false;
	} else {
		// Capture the active camera under the park (its fields tear under a
		// concurrent orbit-drag write — same reason CloneActiveCamera parks).
		std::unique_lock<std::mutex> lk( mMutex );
		const IScene* scene = mJob.GetScene();
		if( !scene ) return false;
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		const ICamera* cam = scene->GetCamera();
		if( !cam || !CameraIntrospection::CaptureCameraSnapshot( *cam, cur ) ) return false;
	}
	std::lock_guard<std::mutex> lk( mMutex );
	mHomeView = cur;
	mHasHomeView = true;
	return true;
}

bool SceneEditController::GoToHomeView()
{
	CameraSnapshot home;
	{
		std::lock_guard<std::mutex> lk( mMutex );
		if( !mHasHomeView ) return false;
		home = mHomeView;
	}
	return SetViewportPose( home );   // takes mMutex itself
}

bool SceneEditController::HasHomeView() const
{
	std::lock_guard<std::mutex> lk( mMutex );
	return mHasHomeView;
}

namespace {

// Generalizes UniqueCameraName (above) across every category: build the
// existing-name set via the SAME CategoryEntityCount/CategoryEntityName
// union every picker list already reads, then append a numeric suffix on
// collision.  Not under mMutex itself -- callers that need the pick to be
// race-free against a concurrent insert take the lock around the WHOLE
// pick-then-insert sequence (InstantiateEntityTemplate / DuplicateEntity
// call this, then immediately ApplyAgentInsertChunk, which does its own
// cancel-and-park; a name collision from a true race is vanishingly
// unlikely in an interactive single-user editor and would simply surface
// as an ApplyAgentInsertChunk duplicate-name rejection, not corruption).
String UniqueEntityName( const SceneEditController& ctrl, SceneEditController::Category cat, const String& proposed )
{
	if( proposed.size() <= 1 ) return String( "entity" );
	std::vector<std::string> existing;
	const unsigned int n = ctrl.CategoryEntityCount( cat );
	existing.reserve( n );
	for( unsigned int i = 0; i < n; ++i ) existing.emplace_back( ctrl.CategoryEntityName( cat, i ).c_str() );

	auto taken = [&]( const std::string& nm ) {
		return std::find( existing.begin(), existing.end(), nm ) != existing.end();
	};
	const std::string base( proposed.c_str() );
	if( !taken( base ) ) return proposed;
	char buf[256];
	for( int i = 2; i < 1000; ++i ) {
		std::snprintf( buf, sizeof(buf), "%s_%d", base.c_str(), i );
		if( !taken( buf ) ) return String( buf );
	}
	std::snprintf( buf, sizeof(buf), "%s_%lld", base.c_str(), static_cast<long long>( std::time( 0 ) ) );
	return String( buf );
}

// Collect every distinct entity name a template will create for a given
// instance name: the top-level @NAME@ plus every @NAME@_<suffix> sub-chunk
// name baked into the template text (e.g. an Object template's
// `@NAME@_geo` geometry, a GGX material's `@NAME@_rd`/`@NAME@_rs`
// painters).  Used to pick an instance name whose ENTIRE name set is free
// BEFORE any insert, so a leftover orphan sub-chunk can't make a
// multi-chunk Add fail mid-sequence with a confusing raw duplicate-name
// message (a name the user never chose).
std::vector<std::string> TemplateInstanceNames( const EntityTemplateDef& tpl, const std::string& instanceName )
{
	std::vector<std::string> names;
	auto addUnique = [&]( const std::string& n ) {
		if( std::find( names.begin(), names.end(), n ) == names.end() ) names.push_back( n );
	};
	if( tpl.hasNamedIdentity ) addUnique( instanceName );
	const std::string token = "@NAME@";
	for( const std::string& chunk : tpl.chunkTexts )
	{
		size_t pos = 0;
		while( ( pos = chunk.find( token, pos ) ) != std::string::npos )
		{
			size_t e = pos + token.size();
			// Capture a trailing `_<identifier>` suffix if present, so
			// `@NAME@_geo` yields the suffix "_geo" and bare `@NAME@`
			// yields "".  Identifier chars: [A-Za-z0-9_].
			while( e < chunk.size() )
			{
				const char c = chunk[e];
				const bool ident = ( c == '_' )
				                || ( c >= 'a' && c <= 'z' )
				                || ( c >= 'A' && c <= 'Z' )
				                || ( c >= '0' && c <= '9' );
				if( !ident ) break;
				++e;
			}
			addUnique( instanceName + chunk.substr( pos + token.size(), e - ( pos + token.size() ) ) );
			pos = e;
		}
	}
	return names;
}

// True if ANY of `names` is already used by SOME chunk in `doc` (any
// kind).  Deliberately kind-agnostic: the real collision rule is
// per-(kind,name), so this over-refuses in the rare cross-kind name-clash
// case (a candidate is skipped when an unrelated-kind chunk shares one of
// its names) -- harmless, since we just advance to the next numeric
// suffix and get a valid unique name, never a confusing refusal.
bool AnyNameTakenDocWide( const RISE::Cst::Document& doc, const std::vector<std::string>& names )
{
	for( const std::string& nm : names )
	{
		int occ = 0;
		RISE::Cst::DocFindByNameAnyRole( doc, nm, &occ, std::string(), false );
		if( occ > 0 ) return true;
	}
	return false;
}

// In-place replace every occurrence of `token` with `value` in `text`.
void ReplaceAllTokens( std::string& text, const std::string& token, const std::string& value )
{
	if( token.empty() ) return;
	size_t pos = 0;
	while( ( pos = text.find( token, pos ) ) != std::string::npos ) {
		text.replace( pos, token.size(), value );
		pos += value.size();
	}
}

// Duplicate recipe (see SceneEditController::DuplicateEntity's doc): find
// the line whose first token is exactly the chunk param name `name` and
// replace the remainder of that line (its value) with `newName`.  Chunk
// text here is Cst::SerializeNode output, not scene-authored bytes, so
// this doesn't need to survive arbitrary user formatting -- just the
// serializer's own -- but it tolerates leading whitespace / trailing CR
// defensively.  Returns false if no `name` param line is found (the
// caller then refuses the duplicate rather than silently inserting a
// same-named clone).
bool ReplaceFirstNameParamLine( std::string& chunkText, const std::string& newName )
{
	size_t pos = 0;
	while( pos < chunkText.size() )
	{
		size_t lineEnd = chunkText.find( '\n', pos );
		const size_t end = ( lineEnd == std::string::npos ) ? chunkText.size() : lineEnd;
		size_t i = pos;
		while( i < end && ( chunkText[i] == ' ' || chunkText[i] == '\t' ) ) ++i;
		if( chunkText.compare( i, 4, "name" ) == 0
		 && ( i + 4 == end || chunkText[i+4] == ' ' || chunkText[i+4] == '\t' ) )
		{
			size_t v = i + 4;
			while( v < end && ( chunkText[v] == ' ' || chunkText[v] == '\t' ) ) ++v;
			size_t ve = end;
			while( ve > v && ( chunkText[ve-1] == '\r' || chunkText[ve-1] == ' ' || chunkText[ve-1] == '\t' ) ) --ve;
			chunkText.replace( v, ve - v, newName );
			return true;
		}
		if( lineEnd == std::string::npos ) break;
		pos = lineEnd + 1;
	}
	return false;
}

}   // namespace

unsigned int SceneEditController::EntityTemplateCount( Category cat ) const
{
	return EntityTemplates::Count( cat );
}

String SceneEditController::EntityTemplateLabel( Category cat, unsigned int idx ) const
{
	const EntityTemplateDef* t = EntityTemplates::At( cat, idx );
	return t ? String( t->label.c_str() ) : String();
}

SceneEditController::AgentCommitResult SceneEditController::InstantiateEntityTemplate(
	Category cat, unsigned int idx, String* outName )
{
	if( outName ) *outName = String();

	const EntityTemplateDef* tpl = EntityTemplates::At( cat, idx );
	if( !tpl )
	{
		AgentCommitResult r;
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.message = String( "unknown entity template index" );
		std::lock_guard<std::mutex> hlk( mMutex );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}

	// Pick an instance name whose WHOLE name set (top-level + every
	// derived sub-chunk name the template bakes in) is free before we
	// insert anything, under mMutex so the doc read is coherent.  A
	// !hasNamedIdentity template (hosek_wilkie sky) creates a single
	// unnamed chunk -- no dedup, its fixed result name.
	String instanceName;
	if( !tpl->hasNamedIdentity )
	{
		instanceName = String( tpl->fixedResultName.c_str() );
	}
	else
	{
		std::lock_guard<std::mutex> nlk( mMutex );
		const RISE::Cst::Document* doc = mJob.GetCstDocument();
		// In-category taken-name set (top-level namespace), same source
		// as UniqueEntityName; the doc-wide probe below covers the
		// derived sub-chunk names UniqueEntityName can't see.
		std::vector<std::string> inCat;
		const unsigned int nc = CategoryEntityCount( cat );
		inCat.reserve( nc );
		for( unsigned int i = 0; i < nc; ++i ) inCat.emplace_back( CategoryEntityName( cat, i ).c_str() );
		auto inCatTaken = [&]( const std::string& nm ) {
			return std::find( inCat.begin(), inCat.end(), nm ) != inCat.end();
		};
		const std::string base = tpl->baseName.empty() ? std::string( "entity" ) : tpl->baseName;
		auto candidateClear = [&]( const std::string& cand ) {
			if( inCatTaken( cand ) ) return false;
			if( doc && AnyNameTakenDocWide( *doc, TemplateInstanceNames( *tpl, cand ) ) ) return false;
			return true;
		};
		std::string chosen = base;
		if( !candidateClear( chosen ) )
		{
			char buf[256];
			chosen.clear();
			for( int i = 2; i < 1000; ++i )
			{
				std::snprintf( buf, sizeof(buf), "%s_%d", base.c_str(), i );
				if( candidateClear( buf ) ) { chosen = buf; break; }
			}
			if( chosen.empty() )
			{
				std::snprintf( buf, sizeof(buf), "%s_%lld", base.c_str(), static_cast<long long>( std::time( 0 ) ) );
				chosen = buf;
			}
		}
		instanceName = String( chosen.c_str() );
	}

	// Resolve @MATERIAL@ for Object templates: reuse an existing
	// material if the scene has one, else bootstrap a bundled default
	// uniformcolor_painter + lambertian_material pair first (both
	// individually undoable, same as every other inserted chunk).
	std::string materialName;
	if( tpl->needsMaterial )
	{
		// Reuse the first REAL material.  Job::InitializeContainers
		// registers a `"none"` sentinel (the null material) in the
		// manager, so "manager non-empty" does NOT mean "the scene has
		// a usable material" -- binding a fresh object to `none` would
		// silently shade black.  Skip the sentinel explicitly.
		const unsigned int matCount = CategoryEntityCount( Category::Material );
		for( unsigned int mi = 0; mi < matCount; ++mi )
		{
			const std::string cand( CategoryEntityName( Category::Material, mi ).c_str() );
			if( !cand.empty() && cand != "none" )
			{
				materialName = cand;
				break;
			}
		}
		if( materialName.empty() )
		{
			// Dedup the bootstrap names too -- a scene with no materials
			// can still already contain a painter named "default_gray"
			// (same ChunkCategory::Painter namespace), which would turn
			// the bootstrap insert into a duplicate-name refusal.
			const std::string painterName(
				UniqueEntityName( *this, Category::Painter, String( "default_gray" ) ).c_str() );
			const std::string lambertianName(
				UniqueEntityName( *this, Category::Material, String( "default_lambertian" ) ).c_str() );
			const AgentCommitResult pr = ApplyAgentInsertChunk(
				String( EntityTemplates::DefaultPainterChunkText( painterName ).c_str() ), nullptr );
			if( !pr.applied ) return pr;
			const AgentCommitResult mr = ApplyAgentInsertChunk(
				String( EntityTemplates::DefaultLambertianChunkText( lambertianName, painterName ).c_str() ), nullptr );
			if( !mr.applied ) return mr;
			materialName = lambertianName;
		}
	}

	std::string textureFile;
	if( tpl->needsTexture )
	{
		textureFile = EntityTemplates::EnsureDefaultTextureFile();
		if( textureFile.empty() )
		{
			AgentCommitResult r;
			r.applied = false;
			r.rawCode = 0;
			r.status  = String( "rejected" );
			r.message = String( "could not create the default placeholder texture file" );
			std::lock_guard<std::mutex> hlk( mMutex );
			r.headVersion = mJob.GetCstHeadVersion();
			return r;
		}
	}

	// Sequential, independently-atomic inserts -- see the header doc's
	// undo-granularity caveat.
	AgentCommitResult last;
	for( const std::string& chunkTemplate : tpl->chunkTexts )
	{
		std::string text = chunkTemplate;
		ReplaceAllTokens( text, "@NAME@", std::string( instanceName.c_str() ) );
		if( tpl->needsMaterial ) ReplaceAllTokens( text, "@MATERIAL@", materialName );
		if( tpl->needsTexture )  ReplaceAllTokens( text, "@TEXTURE@", textureFile );
		last = ApplyAgentInsertChunk( String( text.c_str() ), nullptr );
		if( !last.applied ) return last;
	}

	if( outName ) *outName = instanceName;
	return last;
}

SceneEditController::AgentCommitResult SceneEditController::DuplicateEntity(
	Category cat, const String& name, String* outName )
{
	if( outName ) *outName = String();
	AgentCommitResult r;

	std::string suffix; bool uniqueFallback = false;
	if( !RoleKindSuffixForCategory( cat, suffix, uniqueFallback ) )
	{
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.message = String( "category has no chunk-name addressing scheme to duplicate from" );
		std::lock_guard<std::mutex> hlk( mMutex );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}

	std::string bytes;
	{
		std::lock_guard<std::mutex> hlk( mMutex );
		const RISE::Cst::Document* doc = mJob.GetCstDocument();
		if( !doc )
		{
			r.applied = false; r.rawCode = 0; r.status = String( "rejected" );
			r.message = String( "no retained CST Document" );
			r.headVersion = mJob.GetCstHeadVersion();
			return r;
		}
		const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
			*doc, name.c_str(), nullptr, suffix, uniqueFallback );
		if( id == 0 )
		{
			r.applied = false; r.rawCode = 0; r.status = String( "rejected" );
			r.message = String( "entity not found" );
			r.headVersion = mJob.GetCstHeadVersion();
			return r;
		}
		const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
		if( !chunk )
		{
			r.applied = false; r.rawCode = 0; r.status = String( "rejected" );
			r.message = String( "entity chunk not resolvable" );
			r.headVersion = mJob.GetCstHeadVersion();
			return r;
		}
		bytes = RISE::Cst::SerializeNode( chunk );
	}

	const String newName = UniqueEntityName( *this, cat, name );
	if( !ReplaceFirstNameParamLine( bytes, std::string( newName.c_str() ) ) )
	{
		r.applied = false; r.rawCode = 0; r.status = String( "rejected" );
		r.message = String( "could not locate a `name` parameter to substitute in the duplicated chunk" );
		std::lock_guard<std::mutex> hlk( mMutex );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}

	r = ApplyAgentInsertChunk( String( bytes.c_str() ), nullptr );
	if( r.applied && outName ) *outName = newName;
	return r;
}

SceneEditController::AgentCommitResult SceneEditController::RemoveEntity( Category cat, const String& name )
{
	std::string suffix; bool uniqueFallback = false;
	if( !RoleKindSuffixForCategory( cat, suffix, uniqueFallback ) )
	{
		AgentCommitResult r;
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.message = String( "category has no chunk-name addressing scheme to remove from" );
		std::lock_guard<std::mutex> hlk( mMutex );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}
	return ApplyAgentRemoveChunk( name, String( suffix.c_str() ), nullptr );
}

// ============================ Environment / IBL ============================
//
// See docs/gui/ENVIRONMENT_SECTION.md.  The IBL environment is a scene-level
// singleton (NOT an ILight): an hdr_painter/exr_painter supplies the image and
// four radiance_* params on the ACTIVE rasterizer chunk bind it as the global
// radiance map.  READS come from the live active-rasterizer snapshot
// (GetRasterizerParameter, radiance_*-aware since this change); WRITES apply
// LIVE (SetRasterizerParameter -> rebuild) AND mirror to the CST
// (Job::ApplyCstEnvironmentEdit) so they survive a save.

namespace {

// POSITIVE allow-list of the rasterizer kinds on which a live radiance-map edit
// actually lands.  A live edit goes SetRasterizerParameter -> RebuildRasterizer,
// which reconstructs the rasterizer from its snapshot -- so a kind is env-editable
// ONLY if (a) RebuildRasterizer has a case for it AND (b) that factory takes a
// radianceMapConfig.  This is deliberately an allow-list, not a deny-list: three
// kinds fail one of those conditions and would SILENTLY drop a scale/orient/
// background edit if we advertised them editable --
//   - mlt_rasterizer / mlt_spectral_rasterizer: RebuildRasterizer handles them
//     but SetMLTRasterizer / SetMLTSpectralRasterizer take NO radianceMapConfig;
//   - pixelpel_rasterizer / pixelintegratingspectral_rasterizer: they ACCEPT
//     radiance_* in scene files, but RebuildRasterizer has no case for them
//     (returns false), so SetRasterizerParameter fails.
// Any of those reports editable=false so the GUI keeps the env controls disabled
// rather than offering an edit that no-ops.  (The pixel* rebuild gap is a broader
// pre-existing limitation of the rasterizer-param edit path, tracked separately.)
inline bool RasterizerSupportsRadianceMapEdit( const std::string& kind )
{
	return kind == "pathtracing_pel_rasterizer"
	    || kind == "pathtracing_spectral_rasterizer"
	    || kind == "bdpt_pel_rasterizer"
	    || kind == "bdpt_spectral_rasterizer"
	    || kind == "vcm_pel_rasterizer"
	    || kind == "vcm_spectral_rasterizer"
	    || kind == "auto_rasterizer"
	    || kind == "auto_spectral_rasterizer";
}

}  // namespace

bool SceneEditController::GetEnvironment( EnvironmentInfo& out ) const
{
	out = EnvironmentInfo();

	std::lock_guard<std::mutex> lk( mMutex );

	const std::string activeName = mJob.GetActiveRasterizerName();
	if( activeName.empty() ) return false;

	out.editable = RasterizerSupportsRadianceMapEdit( activeName );

	// Live snapshot's radiance binding (FormatRasterizerParam handles these;
	// radiance_map is "" when unbound / "none").
	const std::string mapName = mJob.GetRasterizerParameter( activeName.c_str(), "radiance_map" );
	const std::string scaleS  = mJob.GetRasterizerParameter( activeName.c_str(), "radiance_scale" );
	const std::string bgS     = mJob.GetRasterizerParameter( activeName.c_str(), "radiance_background" );
	const std::string orientS = mJob.GetRasterizerParameter( activeName.c_str(), "radiance_orient" );

	if( !scaleS.empty() ) { double v = 1.0; if( std::sscanf( scaleS.c_str(), "%lf", &v ) == 1 ) out.scale = v; }
	out.background = ( bgS != "false" );   // default true
	if( !orientS.empty() ) {
		double x = 0, y = 0, z = 0;
		if( std::sscanf( orientS.c_str(), "%lf %lf %lf", &x, &y, &z ) == 3 ) {
			out.orientDeg[0] = x; out.orientDeg[1] = y; out.orientDeg[2] = z;
		}
	}

	if( !mapName.empty() ) {
		out.hasEnvironment = true;
		out.painterName = String( mapName.c_str() );
		// Resolve the bound painter's `file` param from the retained CST.
		const RISE::Cst::Document* doc = mJob.GetCstDocument();
		if( doc ) {
			const RISE::Cst::NodeId pid =
				RISE::Cst::DocFindByNameAnyRole( *doc, mapName, nullptr, "painter", false );
			if( pid != 0 ) {
				const RISE::Cst::NodeRef pchunk = RISE::Cst::DocResolveNodeId( *doc, pid );
				bool present = false;
				const std::string f = AgentReadFirstParamValue( pchunk, "file", &present );
				if( present ) out.file = String( f.c_str() );
			}
		}
	} else {
		// No radiance_map painter bound -- but a procedural sky (hosek) or an
		// ambient light may still have installed a global radiance map.
		const IScene* scene = mJob.GetScene();
		if( scene && scene->GetGlobalRadianceMap() != nullptr ) out.proceduralSky = true;
	}
	return true;
}

bool SceneEditController::SetEnvironmentRadianceParam_( const char* paramName, const std::string& value,
	bool* outPersisted )
{
	if( outPersisted ) *outPersisted = false;

	if( mTxnOpen ) {
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: environment edit refused inside an open transaction (not undoable -> rollback cannot revert it)." );
		return false;
	}

	std::unique_lock<std::mutex> lk( mMutex );
	const std::string activeName = mJob.GetActiveRasterizerName();
	if( activeName.empty() || !RasterizerSupportsRadianceMapEdit( activeName ) ) return false;

	// Cancel-and-park: a rasterizer rebuild releases the old instance and
	// constructs a new one; the render thread reads `pRasterizer` per-pixel, so
	// it must be parked (same pattern as the Category::Rasterizer SetProperty case).
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	const bool ok = mJob.SetRasterizerParameter( activeName.c_str(), paramName, value.c_str() );
	if( !ok ) return false;

	// Mirror to the CST so the edit survives a save (Document-only, no re-derive;
	// safe under the lock -- pCstDocument is swapped here too).  A 0 return means
	// the active rasterizer has no unique chunk in the Document, so the edit will
	// NOT persist -- reported via outPersisted so callers can be honest.
	const int cstResult = mJob.ApplyCstEnvironmentEdit( paramName, value.c_str() );
	if( outPersisted ) *outPersisted = ( cstResult != 0 );

	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::SetEnvironmentScale( double scale )
{
	char buf[64];
	std::snprintf( buf, sizeof(buf), "%g", scale );
	return SetEnvironmentRadianceParam_( "radiance_scale", buf );
}

bool SceneEditController::SetEnvironmentBackground( bool background )
{
	return SetEnvironmentRadianceParam_( "radiance_background", background ? "true" : "false" );
}

bool SceneEditController::SetEnvironmentOrient( double xDeg, double yDeg, double zDeg )
{
	char buf[96];
	std::snprintf( buf, sizeof(buf), "%g %g %g", xDeg, yDeg, zDeg );
	return SetEnvironmentRadianceParam_( "radiance_orient", buf );
}

bool SceneEditController::SetEnvironmentFile( const String& absPath )
{
	// Resolve the currently-bound painter, then edit its `file` via the Painter
	// CST-param path (a full re-derive that reloads the texture -> live + persists).
	std::string painterName;
	{
		std::lock_guard<std::mutex> lk( mMutex );
		const std::string activeName = mJob.GetActiveRasterizerName();
		if( activeName.empty() ) return false;
		painterName = mJob.GetRasterizerParameter( activeName.c_str(), "radiance_map" );
	}
	if( painterName.empty() ) return false;   // no environment bound to swap
	// ApplyAgentParamEdit takes mMutex itself -> must be called with our lock released.
	AgentCommitResult r = ApplyAgentParamEdit(
		String( painterName.c_str() ), String( "painter" ), String( "file" ), absPath, nullptr );
	return r.applied;
}

SceneEditController::AgentCommitResult SceneEditController::AddEnvironment( const String& hdriPath, String* outPainterName )
{
	AgentCommitResult rej;
	rej.applied = false;
	rej.rawCode = 0;
	rej.status  = String( "rejected" );

	const std::string path = std::string( hdriPath.c_str() );
	if( path.empty() ) {
		rej.message = String( "environment file path is empty" );
		std::lock_guard<std::mutex> hlk( mMutex );
		rej.headVersion = mJob.GetCstHeadVersion();
		return rej;
	}

	// Refuse if the active rasterizer takes no radiance map, or an environment is
	// already bound (contract: AddEnvironment is "create when none exists"; the
	// bound-replacement path is SetEnvironmentFile / RemoveEnvironment-then-Add).
	std::string activeName;
	bool alreadyBound = false;
	{
		std::lock_guard<std::mutex> lk( mMutex );
		activeName = mJob.GetActiveRasterizerName();
		if( !activeName.empty() ) {
			alreadyBound = !mJob.GetRasterizerParameter( activeName.c_str(), "radiance_map" ).empty();
		}
	}
	if( activeName.empty() || !RasterizerSupportsRadianceMapEdit( activeName ) ) {
		rej.message = String( "the active rasterizer takes no environment map (use a PT/BDPT/VCM rasterizer)" );
		std::lock_guard<std::mutex> hlk( mMutex );
		rej.headVersion = mJob.GetCstHeadVersion();
		return rej;
	}
	if( alreadyBound ) {
		rej.message = String( "an environment is already bound; swap its file or remove it first" );
		std::lock_guard<std::mutex> hlk( mMutex );
		rej.headVersion = mJob.GetCstHeadVersion();
		return rej;
	}

	// Painter kind from the file extension (.exr -> exr_painter, else hdr_painter).
	std::string ext;
	{
		const size_t dot = path.rfind( '.' );
		if( dot != std::string::npos ) {
			ext = path.substr( dot + 1 );
			for( char& c : ext ) if( c >= 'A' && c <= 'Z' ) c = char( c + 32 );
		}
	}
	const std::string kind = ( ext == "exr" ) ? "exr_painter" : "hdr_painter";

	// Unique painter name (doc-wide), base "env_hdri".
	std::string painterName = "env_hdri";
	{
		std::lock_guard<std::mutex> lk( mMutex );
		const RISE::Cst::Document* doc = mJob.GetCstDocument();
		if( doc ) {
			std::vector<std::string> probe( 1, painterName );
			if( AnyNameTakenDocWide( *doc, probe ) ) {
				char buf[64];
				for( int i = 2; i < 100000; ++i ) {
					std::snprintf( buf, sizeof(buf), "env_hdri_%d", i );
					probe[0] = buf;
					if( !AnyNameTakenDocWide( *doc, probe ) ) { painterName = buf; break; }
				}
			}
		}
	}

	// Insert the painter chunk (a D2 re-derive).  RISE uses hard tabs; the parser
	// requires the chunk braces on their own lines.
	const std::string chunkText = kind + "\n{\n\tname " + painterName + "\n\tfile " + path + "\n}\n";
	AgentCommitResult ins = ApplyAgentInsertChunk( String( chunkText.c_str() ), nullptr );
	if( !ins.applied ) return ins;   // insert rejected (e.g. malformed / dup) -> surface verbatim

	// Bind radiance_map on the active rasterizer (live rebuild + CST mirror).
	bool bindPersisted = false;
	if( !SetEnvironmentRadianceParam_( "radiance_map", painterName, &bindPersisted ) ) {
		// The painter DID land but the LIVE binding failed -- honest partial result
		// (the orphan painter chunk is reusable / removable, not a silent loss).
		if( outPainterName ) *outPainterName = String( painterName.c_str() );
		AgentCommitResult r = ins;
		r.status  = String( "partial" );
		r.message = String( "HDRI painter inserted but binding radiance_map to the active rasterizer failed" );
		std::lock_guard<std::mutex> hlk( mMutex );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}
	if( !bindPersisted ) {
		// The live bind succeeded (viewport shows the HDRI) but the CST mirror
		// could NOT record it -- the active rasterizer has no unique chunk in the
		// Document (e.g. a rasterizer kind switched to but never authored in the
		// scene file).  Surface this honestly so the user isn't told "applied"
		// while a save would drop the binding and orphan the painter.
		if( outPainterName ) *outPainterName = String( painterName.c_str() );
		AgentCommitResult r = ins;
		r.status  = String( "partial" );
		r.message = String( "environment applied live but NOT recorded in the scene file -- the active rasterizer has no chunk to bind radiance_map to (a save would drop it)" );
		std::lock_guard<std::mutex> hlk( mMutex );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}

	if( outPainterName ) *outPainterName = String( painterName.c_str() );
	AgentCommitResult r = ins;
	r.status = String( "applied" );
	{
		std::lock_guard<std::mutex> hlk( mMutex );
		r.headVersion = mJob.GetCstHeadVersion();
	}
	return r;
}

bool SceneEditController::RemoveEnvironment()
{
	// Precheck: only unbind when a radiance_map is ACTUALLY bound.  This matters
	// for correctness, not just cosmetics: the live-map clear below must NOT fire
	// when nothing was bound, because the global map might then belong to a
	// procedural sky (hosek) or ambient light this method must not clobber.
	{
		std::lock_guard<std::mutex> lk( mMutex );
		const std::string activeName = mJob.GetActiveRasterizerName();
		if( activeName.empty() ) return false;
		if( mJob.GetRasterizerParameter( activeName.c_str(), "radiance_map" ).empty() ) return false;
	}

	// Unbind: live radiance_map "" -> the snapshot name becomes "none" and the
	// rebuild installs no NEW map; the CST mirror erases all four radiance_* params.
	if( !SetEnvironmentRadianceParam_( "radiance_map", std::string(), nullptr ) ) return false;

	// The rebuild with name=="none" does NOT un-install the previously-installed
	// global map (rasterizer setup only ever INSTALLS one) -- so the stale
	// IRadianceMap would keep rendering.  Clear it explicitly now.  Safe because
	// the precheck confirmed a radiance_map painter was bound, so the live map is
	// that painter's, not a procedural sky's.  Same cancel-and-park as the setters.
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
	mJob.ClearGlobalRadianceMap();
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
		// Model-B (code-3 re-render fix): re-render whenever the live Scene CHANGED, decoupled from clean success.
		// A diagnosed CST re-derive (Job code 3) REPLACED the Scene+managers but Apply returns ok=false -- the
		// viewport must still repaint the replaced scene, else the user sees stale pre-edit pixels.  Kick on
		// `ok || changed`; `return ok` keeps the edit's failure semantics unchanged for the caller.
		if( ok || mEditor.CstLiveSceneChangedInLastApply() ) {
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
		if( !ok ) {
			// Model-B (code-3 re-render fix): a diagnosed CST re-derive (Job code 3) REPLACED the live Scene+managers
			// but Apply reports failure (ok=false).  The viewport must still repaint the replaced scene (else stale
			// pre-edit pixels), so kick the render under the SAME held lock the success path uses -- but SKIP the
			// transform-commit + auto-sync follow-through (those presuppose a clean edit) and return failure.
			//
			// A2: this bool is the ONLY error channel SetProperty hands back, on purpose -- it's the shared
			// cross-platform GUI surface (Mac / Windows / Android panels all route property edits through this one
			// method), so it can't carry a platform-specific rich diagnostic. Job's code 3 ("should-not-happen",
			// diagnosed re-derive) is already fully logged above and via CstLiveSceneChangedInLastApply's caller;
			// the agent surface (ApplyAgentParamEdit) already returns a structured status="diagnosed" for clients
			// that DO want to distinguish it. A richer GUI-facing diagnostic (e.g. surfacing WHICH code fired,
			// not just ok/changed) belongs to the future F2 ValidationReport design, not a bolt-on here.
			if( mEditor.CstLiveSceneChangedInLastApply() ) {
				mEditPending.store( true, std::memory_order_release );
				lk.unlock();
				mCV.notify_one();
			}
			return false;
		}

		// P5 Slice 3 expansion (object transform): a PANEL transform edit (position / orientation / scale) noted the
		// object for a `matrix`-param commit; flush it here under the SAME park (the commit re-derives).
		// A2 (code-3 parity for the commit follow-through): both commits return false on an internal route failure
		// (incl. a diagnosed code-3 re-derive), and either already logged the specifics + set
		// mCstLiveSceneChanged on a live mutation (1/2/3) -- capture the returns so a commit failure is reported
		// honestly instead of silently folding into the Apply's clean `true`.  Use `&` (not `&&`) so BOTH commits
		// always run even if the object one fails -- short-circuiting would skip a pending camera-pose commit that
		// has nothing to do with the object commit's outcome.
		bool commitsOk = true;
		if( mEditor.HasPendingCstObjectTransforms() ) commitsOk &= mEditor.CommitPendingCstObjectTransforms();
		if( mEditor.HasPendingCstCameraPose() ) commitsOk &= mEditor.CommitPendingCstCameraPose();

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
		// A2: report the commit follow-through's outcome too -- the auto-sync above already ran unconditionally
		// (it reflects the Apply, which DID succeed, not the commit), and the render kick above is unconditional
		// (the live scene must repaint either way).  Only the boolean SetProperty hands back to the caller changes:
		// `ok` was already true here, so this reduces to `commitsOk`, spelled out for clarity at the call site.
		return ok && commitsOk;
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
		// Model-B (code-3 re-render fix): re-render when the live Scene CHANGED even if Apply reports failure -- a
		// diagnosed CST re-derive (Job code 3) replaced the Scene+managers, so the viewport must repaint the
		// replaced scene rather than leave stale pixels.  Kick on `ok || changed`, return the unchanged success bool.
		if( !ok && !mEditor.CstLiveSceneChangedInLastApply() ) return false;

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return ok;
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

		// CST-route the edit (Model-B P5): the live SetFilm above already mutated the scene, but on a CST-default
		// load the retained Document still carries the AUTHORED dims -- a SAVE serializes that Document and a future
		// D2 re-derives from it, so without recording the new dim here the edit is LIVE-ONLY and lost on save /
		// reverted on the next D2.  Patch ONLY the edited param (the other two nullptr -> minimal diff), reading the
		// LIVE post-SetFilm value (authoritative, post-clamp).  A Document-record failure is a SOFT warning (the live
		// edit already succeeded) -- it must NOT fail the user-facing edit, but it is logged loudly inside ApplyCstFilmEdit.
		if( mJob.HasRetainedCstDocument() && filmRef ) {
			char vbuf[64];
			const char* w = nullptr; const char* h = nullptr; const char* p = nullptr;
			const std::string n( name.c_str() );
			if( n == "width" ) {
				std::snprintf( vbuf, sizeof(vbuf), "%u", filmRef->GetWidth() );  w = vbuf;
			} else if( n == "height" ) {
				std::snprintf( vbuf, sizeof(vbuf), "%u", filmRef->GetHeight() ); h = vbuf;
			} else if( n == "pixelAR" ) {
				// %.17g (NOT %.6g) so the edited double round-trips bit-for-bit -- matches the house
				// scalar-serialization convention (FormatMatrix16 / FormatVec3 in SceneEditor.cpp, and the
				// Cst.cpp derive path, all use %.17g).  %.6g truncated e.g. 1.7777778 -> 1.77778 (Delta~2e-6).
				std::snprintf( vbuf, sizeof(vbuf), "%.17g", filmRef->GetPixelAR() ); p = vbuf;
			}
			if( w || h || p ) mJob.ApplyCstFilmEdit( w, h, p );
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
		// Model-B (code-3 re-render fix): re-render when the live Scene CHANGED even if Apply reports failure -- a
		// diagnosed CST re-derive (Job code 3) replaced the Scene+managers, so the viewport must repaint.  Kick on
		// `ok || changed`, return the unchanged success bool.
		if( !ok && !mEditor.CstLiveSceneChangedInLastApply() ) return false;

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return ok;
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
		// Model-B (code-3 re-render fix): re-render when the live Scene CHANGED even if Apply reports failure -- a
		// diagnosed CST re-derive (Job code 3) replaced the Scene+managers, so the viewport must repaint.  Kick on
		// `ok || changed`, return the unchanged success bool.
		if( !ok && !mEditor.CstLiveSceneChangedInLastApply() ) return false;

		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return ok;
	}

	case Category::Painter: {
		// Entity-creation slice: painters have no per-material-style
		// live setter surface (see PainterIntrospection.h's header
		// doc), so route straight through the generic agent CST
		// param-edit path -- the "RouteCstParamEdit_-class" path
		// Material/Light/Medium use on CST scenes, generalized: it
		// already cancel-and-parks, captures the prior value for
		// undo/redo (PushAgentCstParamEdit), marks dirty, bumps light
		// generation if needed, and kicks the re-render itself, so no
		// separate park/SceneEdit/Apply dance is needed here.
		if( mSelectionName.size() <= 1 ) return false;
		const AgentCommitResult r = ApplyAgentParamEdit(
			mSelectionName, String( "painter" ), name, valueStr, nullptr );
		return r.applied;
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
	//
	// Toolkit slice 1 (read_viewport) note: this branch does NOT take
	// mInteractiveFrameStoreMutex.  It only READS the mInteractiveFrameStore
	// pointer (and its immutable Width()/Height()) on the render thread
	// itself, sequenced after that same thread's own prior write -- it never
	// reassigns the pointer.  A concurrent CopyInteractiveFrame reader also
	// only reads-and-addrefs the pointer under the mutex; two concurrent
	// pointer READS are not a data race, so no lock is needed here.  Only the
	// dims-changed branch below (which release-frees and reassigns the
	// pointer) races the reader and therefore takes the mutex.
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

	// Dim changed — allocate the new store FIRST (outside the leaf lock),
	// then swap the pointer under mInteractiveFrameStoreMutex below.
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
	Implementation::FrameStore* fresh = new Implementation::FrameStore( spec );
	// new returns refcount 1; that's our owned reference.

	// Toolkit slice 1 (read_viewport): leaf-lock the pointer swap ONLY.
	// mInteractiveFrameStore is read cross-thread by CopyInteractiveFrame,
	// so the release-old / reassign sequence is a genuine data race without
	// this lock.  A reader that addref'd the OLD store before we release it
	// here keeps its own reference alive (refcount stays >= 1), so its
	// in-flight DumpImage sees a stable snapshot; new render passes write to
	// `fresh`.  The lock guards ONLY the pointer; the store CONTENT is
	// guarded by FrameStore's own per-tile shared_mutex.  See the member's
	// leaf-lock note in the header.
	{
		std::lock_guard<std::mutex> lk( mInteractiveFrameStoreMutex );
		if( mInteractiveFrameStore ) {
			mInteractiveFrameStore->release();
		}
		mInteractiveFrameStore = fresh;
	}

	// SetFrameStore reads mInteractiveFrameStore on the render thread,
	// sequenced after the reassign above -- same no-lock-needed reasoning as
	// the same-dims short-circuit.
	Implementation::Rasterizer* r =
		dynamic_cast<Implementation::Rasterizer*>( mInteractiveRasterizer );
	if( r ) {
		r->SetFrameStore( mInteractiveFrameStore );
	}
}

// Toolkit slice 1 (read_viewport): file-local IRasterImageWriter adapter
// that captures DumpImage's WriteColor stream into a caller-supplied
// std::vector<RISEColor> (row-major).  Stack-allocated by
// CopyInteractiveFrame; the IReference addref/release/refcount are no-ops
// because DumpImage never manages the writer's lifetime and the object
// outlives the single synchronous DumpImage call by construction.
namespace {
	class BufferRasterImageWriter : public IRasterImageWriter
	{
	public:
		BufferRasterImageWriter( std::vector<RISEColor>& out,
		                         unsigned int& outW, unsigned int& outH )
			: mOut( out ), mW( outW ), mH( outH ) {}

		void BeginWrite( const unsigned int width, const unsigned int height ) override
		{
			mW = width;
			mH = height;
			mOut.assign( static_cast<std::size_t>( width ) * height, RISEColor() );
		}
		void WriteColor( const RISEColor& c, const unsigned int x, const unsigned int y ) override
		{
			mOut[ static_cast<std::size_t>( y ) * mW + x ] = c;
		}
		void EndWrite() override {}

		// IReference: stack-owned, lifetime not managed by DumpImage.
		void         addref()   const override {}
		bool         release()  const override { return false; }
		unsigned int refcount() const override { return 1; }

	private:
		std::vector<RISEColor>& mOut;
		unsigned int&           mW;
		unsigned int&           mH;
	};
}

bool SceneEditController::CopyInteractiveFrame(
	std::vector<RISEColor>& outPixels,
	unsigned int& outWidth,
	unsigned int& outHeight ) const
{
	outPixels.clear();
	outWidth  = 0;
	outHeight = 0;

	// Snapshot + addref the interactive store pointer under the leaf mutex
	// (mirrors ViewportFrameStore::SnapshotFrameStore) so a concurrent
	// EnsureInteractiveFrameStore_ reassignment can't free it under us.
	Implementation::FrameStore* snap = nullptr;
	{
		std::lock_guard<std::mutex> lk( mInteractiveFrameStoreMutex );
		if( mInteractiveFrameStore ) {
			mInteractiveFrameStore->addref();
			snap = mInteractiveFrameStore;
		}
	}
	if( !snap ) return false;   // no interactive frame yet (no render has run)

	const unsigned int w = static_cast<unsigned int>( snap->Width() );
	const unsigned int h = static_cast<unsigned int>( snap->Height() );
	if( w == 0 || h == 0 ) {
		snap->release();
		return false;
	}

	// Coherent copy: DumpImage acquires EVERY tile's shared_lock up front,
	// so an in-flight interactive render pass writing tiles cannot tear the
	// result.  This is the SAME mechanism ViewportFrameStore::SaveAs uses --
	// never a raw GetPEL loop (unlocked = torn/racy).
	BufferRasterImageWriter writer( outPixels, outWidth, outHeight );
	snap->AsBeautyRasterImage().DumpImage( &writer );

	snap->release();
	return outWidth != 0 && outHeight != 0 && !outPixels.empty();
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

	// ----- Interactive region-of-interest (design brief A4) -----------
	// Applied only at full resolution: coarse ladder passes render the
	// whole frame so pixels outside the box never go stale at a
	// mismatched scale, while the persistent frame store keeps the last
	// full-res content outside the box as passes refine inside it.
	// Coords are INCLUSIVE (Rect convention) and clamped to the film.
	Rect regionRect( 0, 0, 0, 0 );
	const Rect* pRegion = 0;
	if( scale == 1 && mInteractiveRegionActive.load( std::memory_order_acquire ) )
	{
		unsigned int rl = 0, rt = 0, rr = 0, rb = 0;
		if( GetInteractiveRegion( rl, rt, rr, rb ) )
		{
			// Review-round-1 P2: read the film dims FRESH here (render
			// thread; at scale==1 no swap has run this pass) instead of
			// the mFullResW/H cache, so a rect can never be validated
			// against stale dims and re-expose BoundsFromRect's
			// width-2 underflow on a shrunken film.
			const IFilm* regionFilm = scene->GetFilm();
			const unsigned int w = regionFilm ? regionFilm->GetWidth()  : 0;
			const unsigned int h = regionFilm ? regionFilm->GetHeight() : 0;
			if( w > 0 && h > 0 && rl < w && rt < h )
			{
				regionRect = Rect( rt, rl,
				                   rb < h ? rb : h - 1,
				                   rr < w ? rr : w - 1 );
				pRegion = &regionRect;
			}
		}
	}

	// Free-fly (Tier 2 §5.5): install (or clear) the viewport-private render-camera
	// override on the interactive helper for THIS pass.  The interactive
	// RasterizeScene then renders THROUGH the override instead of
	// Scene::pActiveCamera, while the REAL scene still flows to the caster (so the
	// render path's IScenePriv/Scene downcasts -- photon-map build, light-sampler
	// regen -- keep working) and production render (a separate rasterizer, never
	// given an override) is untouched.  Render-thread-only: the UI thread swaps
	// mViewportOverrideCamera only while parked, and we set the helper here.  We
	// UNCONDITIONALLY set it every pass (override or nullptr), so a stale override
	// can never leak into a non-free-fly pass.
	if( Implementation::PixelBasedRasterizerHelper* helper =
			dynamic_cast<Implementation::PixelBasedRasterizerHelper*>( mInteractiveRasterizer ) )
	{
		const ICamera* overrideCam = ( mViewportPoseActive ? mViewportOverrideCamera : nullptr );
		if( overrideCam )
		{
			// Keep the override's raster dims in lock-step with THIS pass's film:
			// the preview-scale swap resized the film and re-synced MANAGER cameras
			// via ResizeFilm, but the override isn't in the manager, so sync it
			// here (SetDimensionsAndPixelAR calls RegenerateData internally).  Safe
			// to mutate the override on the render thread: the UI thread parks
			// before it ever releases/replaces the override.
			if( const IFilm* passFilm = scene->GetFilm() )
			{
				if( Implementation::CameraCommon* cc = dynamic_cast<Implementation::CameraCommon*>( mViewportOverrideCamera ) )
				{
					cc->SetDimensionsAndPixelAR( passFilm->GetWidth(), passFilm->GetHeight(), passFilm->GetPixelAR() );
				}
			}
		}
		helper->SetViewportCameraOverride( overrideCam );
	}

	const auto t0 = std::chrono::steady_clock::now();
	mInteractiveRasterizer->RasterizeScene( *scene, pRegion, /*seq*/0 );
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
