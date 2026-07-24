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
#include "../Parsers/ChunkParserRegistry.h"  // source-traceability reverse: authoritative keyword -> ChunkCategory
#include <map>                            // source-traceability reverse: cached keyword -> Category map
#include "../Utilities/Transformable.h"   // F6: CaptureTransformState at gizmo drag-start
#include "ObjectIntrospection.h"
#include "LightIntrospection.h"
#include "RasterizerIntrospection.h"
#include "FilmIntrospection.h"
#include "MaterialIntrospection.h"
#include "MediaIntrospection.h"
#include "CstIntrospection.h"         // Generic descriptor+CST property rows (Painter, Geometry, ...) -- GUI redesign 2026-07-22
#include "ChunkDescriptorRegistry.h"  // DescriptorForKeyword -- dangling-reference guard (external-review P1, 2026-07-22)
#include "EntityTemplates.h"          // Entity-creation slice: Add-Entity template registry
#include "FileIdentity.h"             // immutable loaded-file identity captured with save snapshots
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
#include "../Utilities/FiniteMath.h"
#include <algorithm>      // Model-B F2 slice S2a: std::min for WaitForRenderJob's bounded wait_until slices
#include <chrono>
#include <cstdio>
#include <ctime>          // for time() used by CloneActiveCamera dedup fallback
#include <exception>      // Model-B F2 slice S2a: std::exception_ptr / current_exception / rethrow_exception
#include <memory>
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

//! Resolve a manager-registered scene camera and copy its complete
//! CameraSnapshot.  Callers serialize scene/manager lifetime with mMutex.
bool CaptureNamedSceneCameraSnapshot(
	const IScene& scene, const String& name, CameraSnapshot& out )
{
	if( name.size() <= 1 ) return false;
	const ICameraManager* cameras = scene.GetCameras();
	const ICamera* camera = cameras ? cameras->GetItem( name.c_str() ) : nullptr;
	if( !camera ) return false;
	if( CameraIntrospection::CaptureCameraSnapshot( *camera, out ) ) return true;

	// ONB pinholes cannot be persisted through CameraSnapshot because the CST
	// form carries an explicit basis.  A viewport override only needs
	// ray-equivalent state, however.  PinholeCamera::Recompute derives W from
	// (lookAt - location), so location+W and basis V exactly reproduce the
	// live camera's realized orientation via the ordinary pinhole factory.
	const Implementation::CameraCommon* common =
		dynamic_cast<const Implementation::CameraCommon*>( camera );
	const Implementation::PinholeCamera* pinhole =
		dynamic_cast<const Implementation::PinholeCamera*>( camera );
	if( !common || !pinhole || !common->IsFromONB() ) return false;

	const Point3 loc = common->GetRestLocation();
	const OrthonormalBasis3D basis = common->GetCurrentBasis();
	const Vector3 v = basis.v();
	const Vector3 w = basis.w();
	out = CameraSnapshot();
	out.type         = CameraSnapshot::Pinhole;
	out.location[0]  = loc.x;
	out.location[1]  = loc.y;
	out.location[2]  = loc.z;
	out.lookat[0]    = loc.x + w.x;
	out.lookat[1]    = loc.y + w.y;
	out.lookat[2]    = loc.z + w.z;
	out.up[0]        = v.x;
	out.up[1]        = v.y;
	out.up[2]        = v.z;
	out.exposure     = common->GetExposureTimeStored();
	out.scanningRate = common->GetScanningRateStored();
	out.pixelRate    = common->GetPixelRateStored();
	out.pixelAR      = common->GetPixelAR();
	out.fov          = pinhole->GetFovStored();
	out.iso          = pinhole->GetIsoStored();
	out.fstop        = pinhole->GetFstop();
	return true;
}

bool IsCameraChunkKind( const String& kind )
{
	const std::string value( kind.c_str() );
	return value == "camera"
	    || ( value.size() > 7
	      && value.compare( value.size() - 7, 7, "_camera" ) == 0 );
}

}  // namespace

SceneEditController::SceneEditController( IJobPriv& job, IRasterizer* interactiveRasterizer )
: mJob( job )
, mInteractiveRasterizer( interactiveRasterizer )
, mInteractiveImpl( dynamic_cast<Implementation::InteractivePelRasterizer*>( interactiveRasterizer ) )
, mVariantRasterizer( 0 )
, mViewportRenderMode( Implementation::ViewportRenderMode::Preview )
, mViewportXray( true )   // DEFAULT ON (2026-07-17 user decision) -- see the header doc
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
, mProposalMutex()
, mSuppressInitialRender( false )
, mRendering( false )
, mSaving( false )
, mAgentRenderBlocksInteractive( false )
, mDirectRenderStateMutex()
, mDirectRenderDoneCV()
, mDirectRenderCallCount( 0 )
, mDirectRenderStopping( false )
, mDirectRenderCancelRequested( false )
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
, mAgentRenderCancelRequested( false )
, mRenderOwnsScene( false )
, mAgentRenderJobId( kInvalidRenderJobId )
, mAgentRenderPinned( false )
, mAgentRenderException()
, mAgentRenderNextTicket( 0 )
, mAgentRenderServingTicket( 0 )
, mAgentRenderWaitingSyncCount( 0 )
, mFullResW( 0 )
, mFullResH( 0 )
, mPreviewScale( 1 )
, mPreviewScalePinned( false )
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

	// GUI render modes P1 (docs/gui/RENDER_MODES.md §5): every whole-scene
	// (re)bind resets the viewport to Preview.  A scene silently opening
	// (or a scene_variant switch silently landing) in depth/normals/etc.
	// would read as a broken render -- the ratified rule is "every scene
	// opens in preview".  Callers of this method already hold the
	// cancel-and-parked mMutex (or, at construction, there is no render
	// thread yet to race) -- see this method's own doc -- so mutating the
	// interactive rasterizer's caster here is safe by the SAME contract
	// SetViewportRenderMode itself relies on.  This does NOT call the
	// public setter (which re-takes mMutex; RebindEditorToJob's
	// non-constructor callers already hold it, so re-entering would
	// deadlock on the non-recursive mutex) -- it inlines the equivalent
	// raw state mutation instead.
	if( mInteractiveImpl && mViewportRenderMode != Implementation::ViewportRenderMode::Preview )
	{
		mInteractiveImpl->SetViewModeCaster( nullptr );
		mViewportRenderMode = Implementation::ViewportRenderMode::Preview;
	}
	// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): a whole-scene
	// (re)bind also leaves any active BeautyVariant mode -- release the
	// ephemeral pipeline (it was built against the OLD scene) and unpin the
	// preview-scale divisor, exactly like the caster-swap reset above.  Same
	// safety contract: callers already hold the cancel-and-parked mMutex (or,
	// at construction, there is no render thread yet).
	if( mVariantRasterizer )
	{
		mVariantRasterizer->release();
		mVariantRasterizer = 0;
		// P2 review fix: also restore mPreviewScale itself, not just the
		// pin below -- leaving a variant mode's pinned divisor (e.g.
		// quarter-res) in place left the viewport stuck at that resolution
		// until the next gesture-driven mPreviewScale mutation
		// (OnPointerDown/Move/Up etc.) happened to overwrite it.  Gated on
		// "a variant WAS active" (not unconditional) so a whole-scene
		// rebind that was never in a variant mode doesn't disturb whatever
		// adaptive scale a live gesture already set.  Same fix as
		// SetViewportRenderMode's non-variant branch below -- see that
		// site's comment.
		mPreviewScale.store( kPreviewScaleMin, std::memory_order_release );
	}
	mPreviewScalePinned.store( false, std::memory_order_release );
	// review-r2-B P1: the CURRENT pane's resources live in the REGISTERS
	// (its slot is empty by the class invariant), so the slot loop below
	// cannot reach them -- and stamping mCurrentPane = 0 without a switch
	// made the next mint SAVE those stale registers into slot 0,
	// clobbering pane 0's state with another pane's old-scene camera.
	// Release the register half inline, alongside the mVariantRasterizer
	// register release above (the frame-store register may stay: it is
	// scene-agnostic pixels, re-dimensioned and repainted on the next
	// pass).
	if( mViewportOverrideCamera )
	{
		mViewportOverrideCamera->release();
		mViewportOverrideCamera = nullptr;
	}
	mViewportPoseActive = false;
	// review-r1 P1: the UNSCHEDULED panes' slots hold resources realized
	// against the OLD scene/managers (override cameras, frame stores,
	// variant pipelines, casters).  The register resets above predate the
	// pane slots -- reusing a stale slot against the new scene is the same
	// lifecycle violation one indirection away.  Release everything and
	// force the reconcile (realizedMode back to value-init Preview;
	// configDirty so the vantage re-realizes; the CONFIGS survive
	// deliberately -- they are the user's desired state, not realized
	// resources).
	for( unsigned int paneIdx = 0; paneIdx < kViewportPaneCount; ++paneIdx )
	{
		PaneRenderState& slot = mPaneRender[paneIdx];
		if( slot.overrideCamera )    { slot.overrideCamera->release();    slot.overrideCamera = nullptr; }
		if( slot.frameStore )        { slot.frameStore->release();        slot.frameStore = nullptr; }
		if( slot.variantRasterizer ) { slot.variantRasterizer->release(); slot.variantRasterizer = nullptr; }
		if( slot.viewModeCaster )    { slot.viewModeCaster->release();    slot.viewModeCaster = nullptr; }
		slot.realizedMode       = Implementation::ViewportRenderMode {};
		slot.poseActive         = false;
		slot.configDirty        = true;
		slot.dirty              = true;
		slot.previewScaleSaved  = 1;
		slot.previewPinnedSaved = false;
	}
	mCurrentPane = 0;
	// X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): reset alongside
	// the mode reset above, UNCONDITIONALLY -- runs every whole-scene
	// (re)bind, not just the mode-was-non-Preview branch above.  DEFAULT
	// ON (2026-07-17 user decision): reset to true, and stamp it onto the
	// interactive rasterizer directly (same "inline the equivalent raw
	// state mutation" rationale as the SetViewModeCaster call above -- the
	// public SetXrayView setter itself just does the propagation, no
	// locking, so calling it here is safe under the same parked-render
	// contract).  This is also the "first attach" stamp: RebindEditorToJob
	// runs once in the constructor, immediately after mInteractiveImpl is
	// set, so the interactive rasterizer starts see-through before any
	// render ever happens.
	mViewportXray = true;
	if( mInteractiveImpl ) {
		mInteractiveImpl->SetXrayView( mViewportXray );
	}
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
	if( !RISE::IsFiniteDouble( sx ) || !RISE::IsFiniteDouble( sy ) ) return false;
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
		if( !( mag > 0.0 ) || !RISE::IsFiniteDouble( mag ) ) {
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
		if( !( mag > 0.0 ) || !RISE::IsFiniteDouble( mag ) ) continue;
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
	// Round-7 (P2-3, destructor quiescence-barrier ordering): detach the dirty
	// listener FIRST.  SetDirtyChangedListener is a quiescence barrier (waits
	// for callbacksInFlight == 0 unless called from the callback thread itself,
	// so the T17/T18 destroy-from-callback pattern cannot self-deadlock).
	// Without this, the only barrier ran in ~SceneEditor -- but mEditor is
	// declared BEFORE this class's mutexes, so member destruction (reverse
	// declaration order) tears the mutexes down before that barrier runs: a
	// cross-thread RISE_API_DestroySceneEditController racing a synchronously
	// re-entrant dirty callback could touch destroyed mutexes.  In-tree shells
	// are immune (they marshal async), but the C-ABI advertises hardened
	// re-entry, so the barrier must precede EVERYTHING in teardown.  After it
	// returns the listener is empty, so no further callback can re-enter this
	// controller while the rest of the dtor (and member destruction) proceeds.
	mEditor.SetDirtyChangedListener( SceneEditor::DirtyChangedFn() );
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
	// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): release the
	// ephemeral BeautyVariant pipeline if one is still installed.  Stop()
	// already joined the render thread above, so DoOneRenderPass is not
	// reading mVariantRasterizer by the time we reach here.  This dtor
	// never touches mJob (see the NOTE just above) -- releasing an
	// IRasterizer is pure rasterizer/caster refcounting, no Job access.
	if( mVariantRasterizer )
	{
		mVariantRasterizer->release();
		mVariantRasterizer = 0;
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
	// P3a slice 2: release every pane slot's saved resources.  Stop()
	// joined the render thread above, so no SwitchToPaneLocked_ is in
	// flight; the CURRENT pane's resources live in the registers released
	// above (a slot's fields are null while its pane is scheduled -- the
	// switch moves ownership register<->slot), so this never double-
	// releases.  Same no-mJob rule as the rest of this dtor: all pure
	// refcounting.
	for( unsigned int i = 0; i < kViewportPaneCount; ++i )
	{
		PaneRenderState& slot = mPaneRender[i];
		if( slot.overrideCamera )    { slot.overrideCamera->release();    slot.overrideCamera = nullptr; }
		if( slot.frameStore )        { slot.frameStore->release();        slot.frameStore = nullptr; }
		if( slot.variantRasterizer ) { slot.variantRasterizer->release(); slot.variantRasterizer = nullptr; }
		if( slot.viewModeCaster )    { slot.viewModeCaster->release();    slot.viewModeCaster = nullptr; }
		if( slot.paneSink )          { slot.paneSink->release();          slot.paneSink = nullptr; }   // P3a slice 3
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

	// Full Stop() is terminal for this controller because it also retires
	// the dedicated agent-render worker.  In particular, a legacy
	// RequestProductionRender finishing concurrently with Stop must not
	// resurrect only the interactive half after teardown has completed.
	if( mAgentRenderStop.load( std::memory_order_acquire ) ) return;

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
	{
		std::lock_guard<std::mutex> lk( mMutex );
		if( const IScene* scene = mJob.GetScene() ) {
			if( const IFilm* film = scene->GetFilm() ) {
				mFullResW.store( film->GetWidth(),  std::memory_order_release );
				mFullResH.store( film->GetHeight(), std::memory_order_release );
			}
		}
	}

	mRenderThread = std::thread( &SceneEditController::RenderLoop, this );
}

void SceneEditController::StopInteractive()
{
	// Review-round-1 P1: see Start() / mLifecycleMutex's doc.
	std::lock_guard<std::mutex> lifecycleLk( mLifecycleMutex );

	const bool wasRunning = mRunning.exchange( false, std::memory_order_acq_rel );
	if( wasRunning )
	{
		// Trip cancel BEFORE notifying.  Hold mMutex around the wakeup to
		// prevent a lost wakeup: if we notified outside the lock, the
		// render thread could be between its predicate check and the
		// kernel park inside cv.wait, and miss the notify forever — which
		// would deadlock join() below.  C++ guarantees no missed wakeups
		// only when the notifier has held the mutex used by the waiter.
		// Serialize against a direct render's admission/reset handshake. If a
		// direct occupant is already active, preserve this explicit lifecycle
		// cancellation across the Reset() it performs after parking the current
		// interactive pass. A direct call admitted later is not a target of this
		// StopInteractive invocation.
		{
			std::lock_guard<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
			std::lock_guard<std::mutex> directLk( mDirectRenderStateMutex );
			if( mDirectRenderCallCount > 0 )
			{
				mDirectRenderCancelRequested.store( true, std::memory_order_release );
			}
			mCancelProgress.RequestCancel();
		}
		{
			std::lock_guard<std::mutex> lk( mMutex );
		}
		mCV.notify_one();

		if( mRenderThread.joinable() )
		{
			mRenderThread.join();
		}
	}

	// T0 review: lifecycle teardown is a gesture-cancellation boundary.
	// Window/view teardown can discard PointerUp / EndPropertyScrub, and a
	// stale gesture flag would permanently pin a later Start() to one pane
	// (as well as refusing coordinated renders).  The render thread is joined,
	// so finalize any live edit, close the matching composites, and publish a
	// deferred edit for the next Start() to repaint every visible pane.
	// This cleanup is intentionally idempotent even when `wasRunning` is
	// false: property/pointer edits are supported while refinement is paused,
	// so Pause -> gesture -> lost release -> repeated Stop must sanitize the
	// interaction before Resume starts a new render thread.
	std::unique_lock<std::recursive_mutex> cleanupAdmission(
		mRenderAdmissionMutex, std::defer_lock );
	// Admission is held only for the brief claim/refusal transition, never for
	// a render's duration.  Wait it out even on an idempotent stop: a paused
	// lost gesture may be the very reason a concurrent render claim refuses,
	// and returning on try_lock failure would leave neither side clearing it.
	// The owner-state check below is what avoids the render-duration mMutex
	// wait after a claim succeeds.
	cleanupAdmission.lock();
	if( mRenderOwnsScene.load( std::memory_order_acquire )
	 || mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
	{
		// Coordinated renders can hold mMutex for their whole duration.  They
		// cannot coexist with a live gesture (the same admission gate refuses
		// one side), so a repeated already-stopped call has nothing to clean and
		// must return promptly instead of waiting minutes.  Saving is
		// deliberately NOT an owner here: it writes an immutable snapshot with
		// mMutex released, can coexist with a paused lost gesture, and its final
		// head-equality check correctly leaves the scene dirty if this cleanup
		// commits that gesture while IO is in flight.
		return;
	}
	{
		std::lock_guard<std::mutex> lk( mMutex );
		const bool hadPending =
			mEditor.HasPendingCstObjectTransforms()
			|| mEditor.HasPendingCstCameraPose();
		const bool hadInteraction =
			mPointerDown.load( std::memory_order_acquire )
		 || mScrubInProgress.load( std::memory_order_acquire )
		 || mGestureOpenedComposite
		 || mScrubOpenedComposite
		 || hadPending;
		if( hadInteraction )
		{
			if( hadPending )
			{
				mEditor.CommitPendingCstObjectTransforms();
				mEditor.CommitPendingCstCameraPose();
				mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
			}
			if( mGestureOpenedComposite )
			{
				mEditor.EndComposite();
				mGestureOpenedComposite = false;
			}
			if( mScrubOpenedComposite )
			{
				mEditor.EndComposite();
				mScrubOpenedComposite = false;
			}
			mPointerDown.store( false, std::memory_order_release );
			mScrubInProgress.store( false, std::memory_order_release );
			mGizmoDrag.active = false;
			SetPreviewScaleIfUnpinned_( kPreviewScaleMin );
			mPolishState.store( static_cast<int>( PolishState::None ),
			                    std::memory_order_release );
			mLastEditTimeMs.store( NowMs(), std::memory_order_release );
			mEditPending.store( true, std::memory_order_release );
		}
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
// review-r2-B P3 (N-up honesty note): phase/scale below read the live
// REGISTERS, which describe whichever pane is CURRENTLY scheduled -- in a
// multi-pane layout that may be a background pane, not the primary the
// user is watching.  Single layout (every shipped GUI today) is
// unaffected.  A per-pane status query ships with P3a slice 3; do not
// wire the un-indexed call to a per-pane label.
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
	// External review P3 fix: a BeautyVariant mode PINS outScaleDivisor to
	// its own fixed resolution (mPreviewScalePinned -- see its header doc
	// and OnPointerUp's matching P2 fix), by design, for the lifetime of
	// the mode.  That pin is not the ladder's "still walking down toward 1"
	// signal the `outScaleDivisor > kPreviewScaleMin` tests below encode
	// for the non-variant case -- there IS no ladder or polish chain for a
	// variant pass (fixed spp/OIDN baked into CreateBeautyVariantPipeline),
	// so without this carve-out a fully-settled variant render (divisor
	// pinned e.g. to 4, mRendering false) would permanently read
	// divisor(4) > kPreviewScaleMin(1) and report Refining forever.  Derive
	// the honest two-state phase for a pinned divisor directly from
	// mRendering instead: Rendering while the pass is in flight, Idle
	// (RefinementStatusFormatter.swift's "Settled") once it completes.
	// Both GUIs read this phase as a plain passthrough int (see
	// RISE_API_SceneEditController_GetRefinementStatus /
	// RISEViewportBridge.refinementPhaseWithScaleDivisor: /
	// ViewportBridge.cpp) into RefinementStatusFormatter's case 0/1 --
	// already-correct text ("Settled"/"Rendering") for this phase, so no
	// GUI-side change is needed for this fix.
	if( mPreviewScalePinned.load( std::memory_order_acquire ) )
	{
		return rendering ? RefinementPhase::Rendering : RefinementPhase::Idle;
	}
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
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return;
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
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return;
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
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return String();
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return String();
	return String( mEditor.History().LabelForUndo() );
}

String SceneEditController::RedoLabel() const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return String();
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return String();
	return String( mEditor.History().LabelForRedo() );
}

void SceneEditController::Stop()
{
	// Close direct caller registration before any terminal teardown. A
	// caller that entered earlier is already counted even if it is still
	// waiting for render admission; a caller arriving later refuses before
	// touching any other controller state.
	{
		std::lock_guard<std::mutex> directLk( mDirectRenderStateMutex );
		mDirectRenderStopping = true;
	}

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

	// RunPreviewRenderParked executes on an external caller's thread rather
	// than mAgentRenderThread. Registration was closed at function entry;
	// now drain every earlier caller through its final controller touch.
	// Do NOT hold admission here: one of those counted callers may already be
	// waiting for that mutex and must be allowed to acquire it, observe the
	// terminal agent-stop flag, and retire.
	std::unique_lock<std::mutex> directLk( mDirectRenderStateMutex );
	mDirectRenderDoneCV.wait( directLk, [&] {
		return mDirectRenderCallCount == 0;
	} );
}

bool SceneEditController::IsRunning() const
{
	return mRunning.load( std::memory_order_acquire );
}

// Sinks ---------------------------------------------------------------

void SceneEditController::SetPreviewSink( IRasterizerOutput* sink )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return;
	// Legacy sinks remain caller-owned and are normally installed before
	// Start().  But SetPaneSink(0,X) can install a CONTROLLER-owned pane-0
	// override at runtime, and the render thread reads mPaneRender[0].paneSink
	// in DoOneRenderPass.  user-review P2-C: PARK + take mMutex before the swap
	// (exactly as SetPaneSink does) so a runtime legacy-replaces-override mix
	// can't ->release() the sink out from under an in-flight pass.  When called
	// at attach (the shipping path) no render is running, so the park is a
	// no-op.
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
	// A pane-0 SetPaneSink override is controller-owned, so a later legacy
	// replacement must release it rather than leave pane 0 attached to a stale
	// retained sink.
	if( mPaneRender[0].paneSink ) {
		mPaneRender[0].paneSink->release();
		mPaneRender[0].paneSink = nullptr;
	}
	mPreviewSink = sink;
}
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

// Returns the camera the interactive viewport is actually rendering through:
// the free-fly override register when active, else the scene's active camera.
//
// CONCURRENCY INVARIANT (user-review P1#2): this reads the non-atomic
// mViewportOverrideCamera / mViewportPoseActive registers, which the RENDER
// thread swaps and ->release()s inside SwitchToPaneLocked_ (always under
// mMutex).  Callers fall into two classes, both safe:
//   1. GESTURE-PINNED readers -- OnPointerDown/PickAt/OnPointerMove -- run only
//      while mPointerDown is set, so PickNextVisiblePaneLocked_ pins the render
//      to mCurrentPane and SwitchToPaneLocked_ early-outs (no register write).
//      Their unlocked reads are race-free ONLY because a pane's configDirty
//      (which would defeat that early-out and force a re-realize) is set solely
//      by the UI-thread pane-config mutators -- serialized with the UI-thread
//      gesture.  If a NON-UI thread ever gains the ability to set a visible
//      pane's configDirty (e.g. a future writable agent RPC), those readers
//      must move under mMutex.
//   2. NON-gesture readers -- RefreshGizmoHandles / RefreshNavGizmo -- take
//      mMutex themselves (the render can rotate freely there).
const ICamera* SceneEditController::EffectiveViewportCamera_( const IScene* scene ) const
{
	if( mViewportPoseActive && mViewportOverrideCamera ) return mViewportOverrideCamera;
	return scene ? scene->GetCamera() : nullptr;
}

// user-review P1-5 / P2-1: the effective camera of a SPECIFIC pane.  REQUIRES
// mMutex held (the gizmo/nav refreshers hold it).  For the CURRENT pane the
// live registers are authoritative; for any other pane its realized override
// lives in the slot.  Reading the slot is safe here ONLY because mMutex is
// held -- SwitchToPaneLocked_ (which swaps + ->release()s slot cameras) also
// runs under mMutex, so the returned pointer can't be freed under us.
const ICamera* SceneEditController::PaneEffectiveCameraLocked_( unsigned int pane, const IScene* scene ) const
{
	if( pane >= kViewportPaneCount ) pane = 0;
	const bool  active = ( pane == mCurrentPane ) ? mViewportPoseActive : mPaneRender[pane].poseActive;
	ICamera* ov = ( pane == mCurrentPane ) ? mViewportOverrideCamera : mPaneRender[pane].overrideCamera;
	if( active && ov ) return ov;
	return scene ? scene->GetCamera() : nullptr;
}

void SceneEditController::RefreshGizmoHandles()
{
	mGizmoHandles.clear();

	// user-review P1#2 (round 2, concurrency): EffectiveViewportCamera_ /
	// PaneEffectiveCameraLocked_ read the free-fly override register the render
	// thread swaps and ->release()s inside SwitchToPaneLocked_ (run under mMutex
	// at the pass boundary), and the gizmo projection reads the camera frame
	// dims DoOneRenderPass swaps under mCameraFrameMutex.  This NON-gesture
	// overlay refresh (the C-ABI RefreshGizmoHandles the shell calls on every
	// gizmoRefreshTrigger) races both, so it must hold both locks -- but it runs
	// on the paint/UI thread, so it TRY-locks and, on a miss, returns with
	// mGizmoHandles already CLEARED (a one-frame gizmo drop during active
	// refinement) rather than blocking a repaint behind an in-flight pass.  Refuse
	// while a production/agent render owns the scene (gizmos hidden then).
	// The GESTURE caller (OnPointerDown) does NOT come through here -- it parks
	// the render and calls RefreshGizmoHandlesLocked_ directly so its hit-test
	// is reliable even while a preview pass is refining (user-review P2-A).
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return;
	std::unique_lock<std::mutex> cameraLk( mCameraFrameMutex, std::try_to_lock );
	if( !cameraLk.owns_lock() ) return;

	RefreshGizmoHandlesLocked_();
}

// REQUIRES mMutex AND mCameraFrameMutex held (see the header).
void SceneEditController::RefreshGizmoHandlesLocked_()
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

	// user-review P2-1: the object gizmo is DRAWN on the primary pane, so
	// project its handles through the PRIMARY pane's camera -- not whatever
	// pane the scheduler last rotated in (which would misplace the handles and
	// make the next click miss).  Safe: we hold mMutex.  During a drag the
	// primary IS the current pane (OnPanePointerDown force-switched to it), so
	// this matches the gesture-time EffectiveViewportCamera_ reads.
	const ICamera* cam = PaneEffectiveCameraLocked_( mPrimaryPane, scene );
	if( !cam ) return;
	// Gizmo projection is derived for the standard PinholeCamera
	// matrix chain; other camera types are skipped (see
	// `IsGizmoSupportedCamera_` for the rationale).
	if( !IsGizmoSupportedCamera_( cam ) ) return;

	// Stable full-res target dims — what the platform overlay uses
	// as `surface` size and what pointer events normalize through.
	unsigned int stableW = 0, stableH = 0;
	if( !GetCameraDimensionsLocked_( stableW, stableH ) || stableW == 0 || stableH == 0 ) return;

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
	const ICamera* cam = EffectiveViewportCamera_( scene );   // user-review P1#2
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

// Navigation axis-ball gizmo (Tier 2 §4) ------------------------------
//
// Lays out six nubs (±X/±Y/±Z) around a ball the platform draws in a
// viewport corner.  Reuses the proven `ProjectWorldToScreen_` probe to get
// each world axis's screen-space direction through the CURRENT interactive
// camera (the free-fly override when active, else the scene's active camera);
// facing (toward/away) comes from comparing the two endpoints' along-view
// depth in inv-camera space.  Pinhole-gated for the same reason the object
// gizmo is (`ProjectWorldToScreen_`'s derivation is pinhole-only).  Reads
// camera state only — no scene mutation, zero render cost.  user-review P1#2
// (round 2): the override pointer is ALSO swapped/->release()d by the render
// thread inside SwitchToPaneLocked_, so an unlocked read here would race it
// (data race + UAF of the dereferenced camera) -- the "same thread only"
// justification that used to sit here was false.  Serialize under mMutex, and
// refuse while a production/agent render owns the scene, exactly like the
// object gizmo's RefreshGizmoHandles.

bool SceneEditController::RefreshNavGizmo( double centerX, double centerY,
                                           double ballRadius, double nubRadius )
{
	mNavGizmoNubs.clear();
	if( !( ballRadius > 0.0 ) || !( nubRadius > 0.0 ) ) return false;
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	std::unique_lock<std::mutex> cameraLk( mCameraFrameMutex, std::try_to_lock );
	if( !cameraLk.owns_lock() ) return false;

	const IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	// user-review P1-5: the nav ball is drawn on the PRIMARY pane, so lay it
	// out from the PRIMARY pane's camera + pose -- not the scheduler's current
	// pane.  Safe: we hold mMutex (see the header note above).
	const unsigned int t = mPrimaryPane;
	const bool tActive = ( t == mCurrentPane ) ? mViewportPoseActive : mPaneRender[t].poseActive;
	const ICamera* cam = PaneEffectiveCameraLocked_( t, scene );
	if( !cam || !IsGizmoSupportedCamera_( cam ) ) return false;

	// A world pivot the camera is guaranteed to look at (projects on-screen):
	// the free-fly pose's lookat, else the active camera's captured lookat.
	CameraSnapshot snap;
	if( tActive ) snap = ( t == mCurrentPane ) ? mViewportPose : mPaneRender[t].poseSaved;
	else if( !CameraIntrospection::CaptureCameraSnapshot( *cam, snap ) ) return false;
	const Point3 pivot( Scalar( snap.lookat[0] ), Scalar( snap.lookat[1] ), Scalar( snap.lookat[2] ) );

	unsigned int stableW = 0, stableH = 0;
	if( !GetCameraDimensionsLocked_( stableW, stableH ) || stableW == 0 || stableH == 0 ) return false;
	const Implementation::CameraCommon* camC =
		dynamic_cast<const Implementation::CameraCommon*>( cam );
	const unsigned int curW = camC ? camC->GetWidth()  : stableW;
	const unsigned int curH = camC ? camC->GetHeight() : stableH;
	if( curW == 0 || curH == 0 ) return false;

	const Matrix4 mxTrans = cam->GetMatrix();
	const Point3  origin  = cam->GetLocation();

	double px = 0, py = 0;
	if( !ProjectWorldToScreen_( mxTrans, origin, pivot,
		double( curW ), double( curH ), double( stableW ), double( stableH ), px, py ) ) return false;

	const Matrix4 inv  = Matrix4Ops::Inverse( mxTrans );
	const Point3  eyeV = Point3Ops::Transform( inv, origin );   // eye in inv-camera space

	auto probeDir = [&]( const Point3& p, double& outDx, double& outDy ) -> bool {
		double sx = 0, sy = 0;
		if( !ProjectWorldToScreen_( mxTrans, origin, p,
			double( curW ), double( curH ), double( stableW ), double( stableH ), sx, sy ) ) return false;
		const double tx = sx - px, ty = sy - py;
		const double mag = std::sqrt( tx * tx + ty * ty );
		if( !( mag > 1e-6 ) || !RISE::IsFiniteDouble( mag ) ) return false;
		outDx = tx / mag; outDy = ty / mag;
		return true;
	};

	std::vector<NavGizmoNub> nubs;
	nubs.reserve( 6 );
	for( int a = 0; a < 3; ++a ) {
		Vector3 v( a == 0 ? 1.0 : 0.0, a == 1 ? 1.0 : 0.0, a == 2 ? 1.0 : 0.0 );
		const Point3 pPlus ( pivot.x + v.x, pivot.y + v.y, pivot.z + v.z );
		const Point3 pMinus( pivot.x - v.x, pivot.y - v.y, pivot.z - v.z );

		// Screen direction of the +axis (proven-correct probe from the gizmo
		// builder).  When the +axis endpoint is degenerate (view-aligned, so
		// no screen direction, or off-screen) fall back to the negated −axis
		// direction; if both are degenerate the axis points straight at/away
		// from the eye, so both nubs sit at the ball center (dx=dy=0) — the
		// front one draws as a dot, the back one hides behind it.  Either way
		// the axis still emits its two nubs, so a view looking straight down an
		// axis never makes the gizmo vanish.
		double dx = 0.0, dy = 0.0;
		double td = 0.0, te = 0.0;
		if( probeDir( pPlus, td, te ) )        { dx = td;  dy = te;  }
		else if( probeDir( pMinus, td, te ) )  { dx = -td; dy = -te; }

		// Facing: the endpoint nearer the eye (smaller along-view depth) faces
		// the viewer.  `inv`-space z is the along-view coordinate (see
		// ProjectWorldToScreen_: denom = A.z - B.z).
		const Point3 aPlus  = Point3Ops::Transform( inv, pPlus );
		const Point3 aMinus = Point3Ops::Transform( inv, pMinus );
		const double depthPlus  = std::fabs( double( aPlus.z )  - double( eyeV.z ) );
		const double depthMinus = std::fabs( double( aMinus.z ) - double( eyeV.z ) );
		const bool positiveFaces = depthPlus <= depthMinus;

		NavGizmoNub plus;
		plus.axis = a; plus.negative = false;
		plus.screenX = centerX + dx * ballRadius;
		plus.screenY = centerY + dy * ballRadius;
		plus.screenRadius = nubRadius;
		plus.facing = positiveFaces;

		NavGizmoNub minus;
		minus.axis = a; minus.negative = true;
		minus.screenX = centerX - dx * ballRadius;
		minus.screenY = centerY - dy * ballRadius;
		minus.screenRadius = nubRadius;
		minus.facing = !positiveFaces;

		nubs.push_back( plus );
		nubs.push_back( minus );
	}

	mNavGizmoNubs.swap( nubs );   // always 6 nubs once the pivot projects
	return true;
}

unsigned int SceneEditController::NavGizmoNubCount() const
{
	return static_cast<unsigned int>( mNavGizmoNubs.size() );
}

bool SceneEditController::NavGizmoNubInfo( unsigned int idx, int& outAxis, bool& outNegative,
                                           double& outScreenX, double& outScreenY,
                                           double& outScreenRadius, bool& outFacing ) const
{
	if( idx >= mNavGizmoNubs.size() ) return false;
	const NavGizmoNub& n = mNavGizmoNubs[ idx ];
	outAxis         = n.axis;
	outNegative     = n.negative;
	outScreenX      = n.screenX;
	outScreenY      = n.screenY;
	outScreenRadius = n.screenRadius;
	outFacing       = n.facing;
	return true;
}

int SceneEditController::NavGizmoNubAt( double px, double py ) const
{
	int    hitIdx    = -1;
	double hitDist2  = 0.0;
	bool   hitFacing = false;
	for( unsigned int i = 0; i < mNavGizmoNubs.size(); ++i ) {
		const NavGizmoNub& n = mNavGizmoNubs[ i ];
		const double dx = px - n.screenX;
		const double dy = py - n.screenY;
		const double d2 = dx * dx + dy * dy;
		if( d2 > n.screenRadius * n.screenRadius ) continue;
		// Front-facing nubs (drawn on top) win over back-facing ones; among
		// equal facing, the nub whose center is nearer the pointer wins.
		const bool better = ( hitIdx < 0 )
		                 || ( n.facing && !hitFacing )
		                 || ( n.facing == hitFacing && d2 < hitDist2 );
		if( better ) { hitIdx = static_cast<int>( i ); hitDist2 = d2; hitFacing = n.facing; }
	}
	return hitIdx;
}

// Pointer events ------------------------------------------------------

void SceneEditController::OnPointerDown( const Point2& px )
{
	// P3a slice 2: pointer input is pane-0-scoped until slice 3 adds pane
	// routing, but the rotation may have another pane's registers loaded.
	// Force the context back to pane 0 BEFORE flagging the gesture --
	// ordering matters: once mPointerDown is true, PickNext pins to
	// mCurrentPane, so the switch must happen first.  Same park shape as
	// every setter; costs at most one quantum's cancel latency.
	//
	// review-r1 P1 + review-r2-A P1: while a production/agent render owns
	// the scene, REFUSE the gesture entirely -- early-return before ANY
	// state is touched, exactly like OnPointerMove/OnPointerUp.  The
	// round-1 shape (skip only the pane switch, then proceed) left the
	// IN-FLIGHT gesture bound to whatever pane the rotation had loaded:
	// once the render finished mid-drag, OnPointerMove mutated THAT
	// pane's override camera and OnPointerUp reset ITS ladder -- the
	// user's drag landed on a pane they never touched, checkpointed
	// permanently at the next switch.  Refusing is honest and cheap: the
	// interactive loop is blocked out for the render's duration anyway,
	// and the user's next pointer-down re-enters normally.
	if( mRenderOwnsScene.load( std::memory_order_acquire ) )
	{
		return;
	}
	{
		// Take the short admission lock BEFORE mMutex and inspect the gate
		// before attempting the potentially-blocking controller lock.  A
		// render that already won therefore makes this callback return
		// promptly; while this lock is held, no new render can claim
		// admission during the cancel-and-park wait below.
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		{
			return;
		}
		std::unique_lock<std::mutex> lk( mMutex );
		// P3a slice 3: legacy un-indexed input targets pane 0 UNLESS a
		// pane-indexed Down armed a pane just before forwarding here
		// (mGesturePaneArmed, consumed exactly once).
		if( !mGesturePaneArmed ) {
			mGesturePane = 0;
		}
		mGesturePaneArmed = false;
		// P3a slice 3: generalized from "switch to 0" -- the gesture
		// targets mGesturePane (0 for all legacy un-indexed input;
		// OnPanePointerDown stamps it, under this same mutex, before
		// forwarding here).  The pin then holds the gestured pane.
		if( mCurrentPane != mGesturePane )
		{
			if( mRendering.load( std::memory_order_acquire ) ) {
				mCancelProgress.RequestCancel();
				mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
			}
			mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
			SwitchToPaneLocked_( mGesturePane );
		}
		mPointerDown.store( true, std::memory_order_release );
	}
	mLastPx = px;
	mLastEditTimeMs.store( NowMs(), std::memory_order_release );

	// P1: defensively close any composite a PRIOR pointer gesture left open (a
	// lost pointer-up, or a double-down with no intervening up).  Without this the
	// orphaned composite would NEST under the new gesture and only one would close
	// on pointer-up, leaving mCompositeDepth >= 1 forever -- IsCompositeOpen() then
	// permanently blocks transactions and history grows unbounded.
	if( mGestureOpenedComposite ) {
		std::lock_guard<std::mutex> lk( mMutex );
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
			{
				std::lock_guard<std::mutex> lk( mMutex );
				mEditor.BeginComposite( "Drag" );
			}
			mGestureOpenedComposite = true;   // P1: record what this gesture opened
			isMotionTool = true;

			// Gizmo hit-test.  Refresh the handle array against the
			// CURRENT camera + object state, then check whether the
			// pointer landed on any handle.  On hit, capture the
			// drag-start state so OnPointerMove can convert pointer
			// pixel deltas to constrained world deltas without each
			// frame re-probing the camera (which would let the math
			// drift if the camera moved mid-drag).
			//
			// user-review P2-A: this GRAB must get the handle array
			// reliably.  The public RefreshGizmoHandles() try-locks
			// mCameraFrameMutex and DROPS the handles if a preview pass
			// holds it -- for a same-pane gizmo grab (no earlier park)
			// that silently ignored the click.  PARK the render so both
			// locks are free (no pass can start while we hold mMutex),
			// then compute the handles under the held locks.  A blocking
			// acquire of mCameraFrameMutex here is deadlock-free precisely
			// because the park guarantees it is already free.
			{
				std::unique_lock<std::mutex> ghLk( mMutex );
				if( mRendering.load( std::memory_order_acquire ) ) {
					mCancelProgress.RequestCancel();
					mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
				}
				mCV.wait( ghLk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
				std::lock_guard<std::mutex> ghCamLk( mCameraFrameMutex );   // free while parked
				RefreshGizmoHandlesLocked_();
			}
			const int hit = GizmoHandleAt( px );
			mGizmoDrag.active = false;
			if( hit >= 0 ) {
				const GizmoHandle& h = mGizmoHandles[ hit ];
				mGizmoDrag.kind = h.kind;
				mGizmoDrag.axis = h.axis;
				mGizmoDrag.anchorPxX = static_cast<double>( px.x );
				mGizmoDrag.anchorPxY = static_cast<double>( px.y );

				// Capture pivot (world) + projection.
				// Re-park and keep mMutex across every live scene/object/camera
				// read below. A background agent D2 may replace all of those
				// managers between the handle refresh and this capture.
				std::unique_lock<std::mutex> captureLk( mMutex );
				CancelAndParkRender_( captureLk );
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
					const ICamera* cam = EffectiveViewportCamera_( scene );   // user-review P1#2
					unsigned int stableW = 0, stableH = 0;
					const bool stableOk = GetCameraDimensionsLocked_( stableW, stableH )
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
		{
			std::lock_guard<std::mutex> lk( mMutex );
			mEditor.BeginComposite( "Camera" );
		}
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
		SetPreviewScaleIfUnpinned_( kPreviewScaleMotionStart );
	}
}

void SceneEditController::OnPointerMove( const Point2& px )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;  // render-owns-scene guard
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
			SetPreviewScaleIfUnpinned_( kPreviewScaleMotionStart );
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
				Point3 camPos;
				{
					std::lock_guard<std::mutex> sceneLk( mMutex );
					const IScene* scene = mJob.GetScene();
					const ICamera* cam = EffectiveViewportCamera_( scene );
					if( !cam ) return;
					camPos = cam->GetLocation();
				}
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
	// P3a slice 3: a camera-motion gesture on an ordinary SECONDARY pane
	// flies that pane's realized override camera.  Kind 3 is deliberately
	// different: it stays bound and edits the named manager camera through
	// SceneEditor's normal capture/history path.  Pane 0 falls through to
	// the classic active-camera edit.
	if( IsCameraMotionTool( mTool ) && mGesturePane != 0 ) {
		bool editBoundCamera = false;
		{
			std::lock_guard<std::mutex> lk( mMutex );
			if( mPaneConfigs[mGesturePane].vantageKind
			 == PaneVantageKind::SceneCameraNamed )
			{
				edit.cameraTargetName =
					mPaneConfigs[mGesturePane].sceneCameraRef;
				edit.allowONBPoseEdit = true;
				editBoundCamera = true;
			}
		}
		if( !editBoundCamera ) {
			ApplyPaneFlyOp_( edit );
			return;
		}
	}

	if( IsObjectMotionTool( mTool ) || IsCameraMotionTool( mTool ) ) {
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
			if( IsCameraMotionTool( mTool ) )
			{
				const String targetName =
					edit.cameraTargetName.size() > 1
					? edit.cameraTargetName
					: String( mJob.GetActiveCameraName().c_str() );
				InvalidatePanesBoundToSceneCameraLocked_(
					targetName );
			}
			mLastEditTimeMs.store( NowMs(), std::memory_order_release );
			mPolishState.store( static_cast<int>( PolishState::None ),
			                    std::memory_order_release );
			mEditPending.store( true, std::memory_order_release );
			lk.unlock();
			mCV.notify_one();
		}
	}
}

void SceneEditController::OnPointerUp( const Point2& px )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;  // render-owns-scene guard
	// Closing mPointerDown is itself an admission transition.  Keep the
	// admission lock through CST/composite finalization and final-pass arming
	// so a coordinated render cannot claim the scene in between.
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return;
	(void)px;
	if( !mPointerDown.load( std::memory_order_acquire ) ) return;

	const bool wasMotion =
		IsCameraMotionTool( mTool ) || IsObjectMotionTool( mTool );
	{
		// Finalize and arm the whole gesture under one controller critical
		// section.  In particular, pointerDown=false, the all-pane edit wake,
		// and FinalRegularRunning MUST be one atomic scheduler transition:
		// otherwise the render thread can switch to a dirty sibling between a
		// KickRender and the polish store, landing the marker on the wrong
		// pane's live register after that sibling already snapshotted None.
		// Keep its composite OPEN while waiting for the render to park: wait()
		// releases mMutex, and the open composite makes an agent D2 that enters
		// during that interval refuse/retry instead of replacing the scene
		// before the pending live transform/pose is captured into the CST.
		std::unique_lock<std::mutex> lk( mMutex );
		const bool hasPending =
			mEditor.HasPendingCstObjectTransforms()
			|| mEditor.HasPendingCstCameraPose();
		if( hasPending ) {
			if( mRendering.load( std::memory_order_acquire ) ) {
				mCancelProgress.RequestCancel();
				mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
			}
			mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
			// P5 Slice 3 expansion (object transform): a gizmo drag accumulated
			// per-frame live edits. Commit the NET transform/pose to the CST once,
			// while the render is parked and agent D2 is excluded.
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
			mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
		}

		// Close the composite only AFTER the persistence flush.  This also
		// handles an armed-but-no-motion gesture (no pending CST channel).
		if( mGestureOpenedComposite ) {
			mEditor.EndComposite();
			mGestureOpenedComposite = false;
		}

		// Always clear the drag state (including armed-without-motion), snap
		// to final quality, and resume the deferred all-pane rotation.
		mGizmoDrag.active = false;
		SetPreviewScaleIfUnpinned_( kPreviewScaleMin );
		mLastEditTimeMs.store( NowMs(), std::memory_order_release );
		const PolishState finalState =
			( wasMotion && !mPreviewScalePinned.load( std::memory_order_acquire ) )
			? PolishState::FinalRegularRunning : PolishState::None;
		mPolishState.store( static_cast<int>( finalState ), std::memory_order_release );
		mPointerDown.store( false, std::memory_order_release );
		mEditPending.store( true, std::memory_order_release );
		if( mRendering.load( std::memory_order_acquire ) )
		{
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
	}
	mCV.notify_one();
	ForTest_OnPointerUpAfterFinalRenderArmed();
}

// Direct controls -----------------------------------------------------

bool SceneEditController::OnTimeScrubBegin()
{
	// P1: close any composite a PRIOR scrub left open (a missing OnTimeScrubEnd or a
	// repeated Begin) before opening a new one -- otherwise scrubs NEST and one stays
	// open forever (permanent IsCompositeOpen block + the open group defeats history
	// trimming).  Mirrors the pointer-gesture orphan guard in OnPointerDown.
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return false;
		std::lock_guard<std::mutex> lk( mMutex );
		if( mScrubOpenedComposite ) {
			mEditor.EndComposite();
			mScrubOpenedComposite = false;
		}
		mEditor.BeginComposite( "Scrub" );
		mScrubOpenedComposite = true;
	}
	SetPreviewScaleIfUnpinned_( kPreviewScaleMotionStart );
	return true;
}

bool SceneEditController::OnTimeScrub( Scalar t )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	// Check before mMutex so a queued or running coordinated render never
	// turns a UI scrub callback into a render-duration wait.  Holding the
	// admission lock through the mutation also closes the wait() release
	// window in which a render could otherwise claim the gate.
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
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
		// Timelines may animate any manager camera, including inactive ones
		// displayed by SceneCameraNamed panes.  Their standalone overrides
		// must be discarded so reconcile captures the new evaluated pose.
		InvalidateAllSceneCameraNamedPanesLocked_();
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
	return ok;
}

bool SceneEditController::OnTimeScrubEnd()
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	// P1: close only the composite THIS scrub opened -- a stray End with no open scrub
	// must not under-flow the composite depth.
	if( mScrubOpenedComposite ) {
		std::lock_guard<std::mutex> lk( mMutex );
		mEditor.EndComposite();
		mScrubOpenedComposite = false;
	}
	SetPreviewScaleIfUnpinned_( kPreviewScaleMin );
	KickRender();
	return true;
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
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return;
		std::lock_guard<std::mutex> lk( mMutex );
		SetPreviewScaleIfUnpinned_( kPreviewScaleMotionStart );
		mLastEditTimeMs.store( NowMs(), std::memory_order_release );
		mScrubInProgress.store( true, std::memory_order_release );
	}
}

void SceneEditController::EndPropertyScrub()
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return;
	// Restore full resolution and queue one final render so the
	// post-scrub frame is sharp.  The render-loop's idle-refinement
	// pass will further refine on top.  Note: the watchdog in the
	// render thread will also clear mScrubInProgress if this End
	// gets dropped (gesture interrupt, view torn down mid-drag) —
	// so the worst case is a brief preview-quality dip, not a
	// permanent stuck-low-quality state.
	mScrubInProgress.store( false, std::memory_order_release );
	SetPreviewScaleIfUnpinned_( kPreviewScaleMin );
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
	case Cat::Geometry: {
		// GUI redesign 2026-07-22: geometry is name-managed like Material --
		// a removed/undone geometry must drop the stale selection so the
		// panel doesn't render a header for an entity that no longer exists.
		IGeometryManager* m = const_cast<IJobPriv&>( job ).GetGeometries();
		return m && m->GetItem( name.c_str() ) != 0;
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

void SceneEditController::UndoInner_()
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;  // render-owns-scene guard
	// Latent-guard (re-review): refuse user Undo while a transaction is open --
	// undoing PAST the baseline would break RollbackTransaction's revert
	// guarantee (and ClearRedo would then drop a pre-baseline edit).  The
	// rollback path uses mEditor.Undo() directly, bypassing this guard.
	if( mTxnOpen.load( std::memory_order_acquire ) ) {
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
	if( mTxnOpen.load( std::memory_order_acquire ) ) {
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController::Undo refused: a transaction opened before the undo acquired the mutation lock." );
		return;
	}
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
		InvalidateAllSceneCameraNamedPanesLocked_();
		mEditPending.store( true, std::memory_order_release );
		// Bump epoch — Undo of AddCamera removes an entity, which
		// changes the Camera category's entity list.  Cheap to bump
		// unconditionally; covers future structural-undo ops too.
		mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
		lk.unlock();
		mCV.notify_one();
	}
}

void SceneEditController::RedoInner_()
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;  // render-owns-scene guard
	if( mTxnOpen.load( std::memory_order_acquire ) ) {
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController::Redo refused: a transaction is open." );
		return;
	}
	std::unique_lock<std::mutex> lk( mMutex );
	if( mTxnOpen.load( std::memory_order_acquire ) ) {
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController::Redo refused: a transaction opened before the redo acquired the mutation lock." );
		return;
	}
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
		InvalidateAllSceneCameraNamedPanesLocked_();
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
		// Pause only the interactive loop.  Stop() also permanently retires
		// the controller's long-lived agent-render worker, so using it here
		// made the first legacy production render disable every later agent
		// render for this controller.
		StopInteractive();
	}

	// Route even this legacy synchronous arm through the tracked production
	// slot. Stop() retires and joins that worker, so it cannot return while
	// this render still borrows the controller or Job. The older direct
	// mMutex-held call was mutually exclusive with edits, but invisible to
	// Stop() once StopInteractive had already made mRunning false.
	bool ok = false;
	try
	{
		ok = RunProductionRenderComposed(
			mJob, this, String( "legacy_production_render" ),
			/*guiProgress=*/nullptr,
			[&]() -> bool {
				IRasterizer* prod = mJob.GetRasterizer();
				const IScene* scene = mJob.GetScene();
				if( !prod || !scene ) return false;

				// Run a FULL SetSceneTime(t) at the most recently scrubbed
				// time before invoking the production rasterizer. The preview
				// path deliberately skips photon-map regeneration.
				scene->SetSceneTime( mEditor.LastSceneTime() );
				scene->GetObjects()->PrepareForRendering();
				prod->RasterizeScene( *scene, /*pRect*/0, /*seq*/0 );
				return true;
			} );
	}
	catch( const std::exception& e )
	{
		// This method is exposed through a bool C ABI and Objective-C
		// bridge. Never let renderer exceptions cross either boundary.
		GlobalLog()->PrintEx( eLog_Error,
			"SceneEditController::RequestProductionRender failed with an exception: %s",
			e.what() );
	}
	catch( ... )
	{
		GlobalLog()->PrintEx( eLog_Error,
			"SceneEditController::RequestProductionRender failed with an unknown exception." );
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
	std::unique_lock<std::recursive_mutex> admissionLk(
		mRenderAdmissionMutex, std::try_to_lock );
	if( !admissionLk.owns_lock()
	 || mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		return EditorStateSnapshot();
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return EditorStateSnapshot();
	return CaptureEditorStateLocked_();
}

SceneEditController::EditorStateSnapshot SceneEditController::CaptureEditorStateLocked_() const
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
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return;
		std::lock_guard<std::mutex> lk( mMutex );
		RestoreEditorStateLocked_( st, restoreDirty );
	}
	// Document-first phase 1: this PUBLIC entry point holds no controller
	// lock -- drain the dirty->clean notification RestoreDirtyState flagged.
	// (RollbackTransaction calls the Locked_ body directly under mMutex and
	// drains at its own post-unlock tail.)
	if( !mEditor.DrainDirtyNotification() ) return;
}

void SceneEditController::RestoreEditorStateLocked_( const EditorStateSnapshot& st, bool restoreDirty )
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
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::BeginTransaction refused: a coordinated "
			"agent/production render is queued or running." );
		return false;
	}
	std::lock_guard<std::mutex> lk( mMutex );
	if( mEditor.IsCompositeOpen() )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::BeginTransaction refused: a SceneEditor "
			"composite is open; close it before opening a transaction." );
		return false;
	}
	mTxnBaseline = CaptureEditorStateLocked_();   // H1: one owned baseline
	mTxnOpen.store( true, std::memory_order_release );
	mEditor.History().SnapshotRedoForRollback();   // P1-#3: so a full rollback restores the pre-transaction redo stack
	mEditor.History().SnapshotUndoForRollback();   // P1: ...and the pre-transaction undo stack (cap-evicted records)
	return true;
}

bool SceneEditController::IsTransactionOpen() const
{
	return mTxnOpen.load( std::memory_order_acquire );
}

bool SceneEditController::RollbackTransaction()
{
	if( !mTxnOpen.load( std::memory_order_acquire ) ) return false;

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
	RestoreEditorStateLocked_( mTxnBaseline, fullyReverted );   // H1 + P1-#1 (phase 1: Locked_ -- we hold mMutex; the tail drain notifies)
	// P1-#3: a FULL rollback restores the pre-transaction redo stack the first edit
	// cleared; a PARTIAL leaves it empty (ClearRedo above) since those redo entries
	// are no longer coherent with the residual state.
	if( fullyReverted ) {
		mEditor.History().RestoreRedoFromSnapshot();
		mEditor.History().RestoreUndoFromSnapshot();   // P1: restore any pre-txn undo record evicted at the cap
	}

	// Close the transaction.
	mTxnOpen.store( false, std::memory_order_release );
	mEditor.History().ClearRollbackSnapshots();   // P1 review: free the rollback snapshots now the txn is closed

	// Re-render the reverted state.  Inline the KickRender effect under
	// the held lock (store editPending, notify after unlock) so the
	// render thread sees the reverted scene + the pending flag together,
	// matching the OnTimeScrub / object-drag park-and-apply idiom.
	// The reverted history may include camera edits whose forward
	// configDirty was already consumed by a pane pass.
	InvalidateAllSceneCameraNamedPanesLocked_();
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
	mEditor.DrainDirtyNotification();   // Document-first phase 1: post-unlock drain
	return fullyReverted;
}

bool SceneEditController::EndTransaction()
{
	std::lock_guard<std::mutex> lk( mMutex );
	if( !mTxnOpen.load( std::memory_order_acquire ) ) return false;
	// Commit is record-only: the edits were applied + recorded live
	// during the transaction (the shipping flow), so committing just
	// closes the transaction.  No re-apply, no revert; the redo stack is
	// left intact so normal Undo/Redo of the committed edits still works.
	mTxnOpen.store( false, std::memory_order_release );
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

// Round-4 structural fix (external review): the PUBLIC enumeration getters
// serve a SNAPSHOT with stale-fallback refresh -- see the header doc on
// RefreshEnumSnapshot_.  This replaces round-3's blocking wrappers, whose
// blocking lock could DEADLOCK a C-ABI dirty-changed callback that calls
// back into the controller (the listener fires on the mutating thread with
// mMutex held, and the C contract cannot forbid re-entry by out-of-tree
// clients).  A try_lock refresh + stale fallback is deadlock-proof, block-
// proof, UAF-proof (live reads only under the lock), and flicker-proof
// (stale beats empty -- including during a production/agent render, where
// the outliner now keeps its last-known list instead of blanking).
void SceneEditController::RefreshEnumSnapshot_( Category cat ) const
{
	const int ci = static_cast<int>( cat );
	if( ci <= 0 || ci >= kNumCategories ) return;
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;   // render owns the scene: serve stale
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return;                                      // contended (or re-entrant): serve stale
	std::vector<String> names;
	const unsigned int n = CategoryEntityCountLocked_( cat );
	names.reserve( n );
	for( unsigned int i = 0; i < n; ++i ) names.push_back( CategoryEntityNameLocked_( cat, i ) );
	String active = CategoryActiveNameLocked_( cat );
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );              // leaf lock: never held across mMutex
	mUi.entityNames[ci].swap( names );
	mUi.activeNames[ci] = active;
}

unsigned int SceneEditController::CategoryEntityCount( Category cat ) const
{
	const int ci = static_cast<int>( cat );
	if( ci <= 0 || ci >= kNumCategories ) return 0;
	RefreshEnumSnapshot_( cat );
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	return static_cast<unsigned int>( mUi.entityNames[ci].size() );
}

String SceneEditController::CategoryEntityName( Category cat, unsigned int idx ) const
{
	const int ci = static_cast<int>( cat );
	if( ci <= 0 || ci >= kNumCategories ) return String();
	RefreshEnumSnapshot_( cat );
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	return idx < mUi.entityNames[ci].size() ? mUi.entityNames[ci][idx] : String();
}

String SceneEditController::CategoryActiveName( Category cat ) const
{
	const int ci = static_cast<int>( cat );
	if( ci <= 0 || ci >= kNumCategories ) return String();
	RefreshEnumSnapshot_( cat );
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	return mUi.activeNames[ci];
}

unsigned int SceneEditController::CategoryEntityCountLocked_( Category cat ) const
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
	case Category::Geometry: {
		// GUI redesign 2026-07-22: geometry is fully name-managed
		// (IGeometryManager on the Job) -- enumerate exactly like Medium.
		struct Count : public IEnumCallback<const char*> {
			unsigned int n = 0;
			bool operator()( const char* const& name ) override {
				if( name ) ++n;
				return true;
			}
		};
		Count cb;
		mJob.EnumerateGeometryNames( cb );
		return cb.n;
	}
	case Category::None:
	default:
		return 0;
	}
}

String SceneEditController::CategoryEntityNameLocked_( Category cat, unsigned int idx ) const
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
	case Category::Geometry: {
		CollectNamesCallback cb;
		mJob.EnumerateGeometryNames( cb );
		if( idx >= cb.names.size() ) return String();
		return cb.names[idx];
	}
	case Category::None:
	default:
		return String();
	}
}

String SceneEditController::CategoryActiveNameLocked_( Category cat ) const
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
	case Category::Geometry:
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

bool SceneEditController::SetSelectionInner_( Category cat, const String& entityName )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	// External-review P1 (2026-07-22): the C-ABI passes a RAW int category
	// (RISE_API_..._SetSelection casts an arbitrary int to Category), and every
	// path below indexes the fixed mSelectionByCategory / mSectionExpanded
	// arrays by static_cast<int>(cat) with NO bounds check.  An out-of-range
	// value (-1, or >= kNumCategories) is an out-of-bounds array WRITE that
	// corrupts controller memory.  Reject before any indexing.  None==0 is a
	// valid in-range value handled immediately below.
	{
		const int ci = static_cast<int>( cat );
		if( ci < 0 || ci >= kNumCategories ) return false;
	}
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
		if( mTxnOpen.load( std::memory_order_acquire ) ) {
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
						if( mJob.ApplyCstFilmEdit( wStr, hStr, nullptr ) != 0 )
							mEditor.MarkCstHeadDirty( "", nullptr );   // round-3 sibling fix + round-4 gate: see the Film SetProperty arm
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
			// External-review round 3 (2026-07-22): the two Find helpers read the
			// LIVE object manager, racing an any-thread agent D2 manager swap ->
			// UAF.  Resolve both names under a brief mMutex hold, then write the
			// UI-state members with the lock released (main-thread-only members;
			// the lock protects the manager reads, not them).
			String matName, medName;
			{
				std::lock_guard<std::mutex> slk( mMutex );
				matName = FindObjectMaterialName( mJob, entityName );
				medName = FindObjectInteriorMediumName( mJob, entityName );
			}
			const int matIdx = static_cast<int>( Category::Material );
			mSelectionByCategory[ matIdx ] = matName;
			mSectionExpanded[ matIdx ]     = true;

			const int medIdx = static_cast<int>( Category::Medium );
			mSelectionByCategory[ medIdx ] = medName;
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
	// Agent edits may re-derive the Job from any thread, replacing the
	// animation and its manager graph.  Query only in the same critical
	// section as every D2 replacement so the returned values came from one
	// still-live graph.  This is a frequently-polled UI getter, so follow
	// the stale/fallback getter convention and never block a repaint behind
	// production ownership or a contended editor operation.
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
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
		if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;
		std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
		if( !lk.owns_lock() ) return false;
		return GetCameraDimensionsLocked_( w, h );
	}
	w = cachedW;
	h = cachedH;
	return true;
}

bool SceneEditController::GetCameraDimensionsLocked_( unsigned int& w,
	                                                   unsigned int& h ) const
{
	const unsigned int cachedW = mFullResW.load( std::memory_order_acquire );
	const unsigned int cachedH = mFullResH.load( std::memory_order_acquire );
	if( cachedW > 0 && cachedH > 0 ) {
		w = cachedW;
		h = cachedH;
		return true;
	}
	if( const IScene* scene = mJob.GetScene() ) {
		if( const IFilm* film = scene->GetFilm() ) {
			w = film->GetWidth();
			h = film->GetHeight();
			return w > 0 && h > 0;
		}
	}
	return false;
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

	String pickedName;
	{
		// Picking reads and prepares the live object manager and camera.
		// Hold the mutation mutex and park rendering so neither a render
		// nor an any-thread D2 commit can replace/use those structures
		// concurrently. Copy only the resolved name out of this scope.
		std::unique_lock<std::mutex> lk( mMutex );
		CancelAndParkRender_( lk );
		const IScene* scene = mJob.GetScene();
		const ICamera* cam = EffectiveViewportCamera_( scene );
		IObjectManager* objs =
			scene ? const_cast<IObjectManager*>( scene->GetObjects() ) : nullptr;
		const IFilm* film = scene ? scene->GetFilm() : nullptr;
		if( scene && cam && objs && film ) {
			objs->PrepareForRendering();
			const Scalar pickPxY =
				static_cast<Scalar>( film->GetHeight() ) - px.y;
			RuntimeContext rc(
				GlobalRNG(), RuntimeContext::PASS_NORMAL, /*threaded*/false );
			Ray ray;
			if( cam->GenerateRay( rc, ray, Point2( px.x, pickPxY ) ) ) {
				RasterizerState rast;
				RayIntersection ri( ray, rast );
				objs->IntersectRay(
					ri, /*frontFace*/true, /*backFace*/false, /*exit*/false );
				if( ri.geometric.bHit && ri.pObject ) {
					FindObjectNameCallback cb( ri.pObject, objs );
					objs->EnumerateItemNames( cb );
					pickedName = cb.foundName;
				}
			}
		}
	}

	if( pickedName.size() > 1 ) {
		// SetSelection takes mMutex itself; invoke it only after the pick
		// scope releases the non-recursive lock.
		SetSelection( Category::Object, pickedName );
		return;
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
// callback. This does NOT take the render-duration mMutex and does NOT wait
// (it only takes the narrow slot mutex to retain a queued cancellation) -- a
// caller that needs "cancelled AND drained" calls this THEN separately waits.
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
	// Serialize the target choice with submit/slot-release. The shared
	// progress flag still handles a currently-running render immediately;
	// the per-occupant bit closes the queued-before-start window where the
	// worker's mandatory Reset() would otherwise erase this cancellation.
	std::lock_guard<std::mutex> slotLk( mAgentRenderSlotMutex );
	std::lock_guard<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderPending )
	{
		mAgentRenderCancelRequested.store( true, std::memory_order_release );
	}
	{
		std::lock_guard<std::mutex> directLk( mDirectRenderStateMutex );
		if( mDirectRenderCallCount > 0 )
		{
			mDirectRenderCancelRequested.store( true, std::memory_order_release );
		}
	}
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
	// Phase 3: kind-addressed singletons -- same generalized rule as
	// Job::ApplyCstParamEditImpl_ (empty name + kind = the sole chunk of that
	// kind), so the prior-value capture resolves the SAME chunk the edit will.
	// Round-7: same GATED camera fallback as Job::ApplyCstParamEditImpl_
	// (DocCameraUniqueFallbackPermitted) -- the capture must stay in lockstep
	// with the edit's resolution, including the typo-refusal direction.
	const std::string ekind( entityKind.size() > 1 ? entityKind.c_str() : "" );
	const char* bareName = entityName.size() > 1 ? entityName.c_str() : "";
	const bool uniqueFallback = ( ekind == "camera" )
		? RISE::Cst::DocCameraUniqueFallbackPermitted( *doc, bareName, mJob.GetActiveCameraName() )
		: ( entityName.size() <= 1 && !ekind.empty() );
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, bareName, nullptr, ekind, uniqueFallback );
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
	const std::string kindText = kind.size() > 1 ? kind.c_str() : "";
	const std::string targetText = target.c_str();
	// Mirror Job::ApplyCstRemoveChunk's descriptor-aware positional
	// addressing. APPEND-class chunks such as timeline/keyframe are
	// unnamed and removable by keyword when exactly one exists. They need
	// the same pre-remove capture as the unnamed-camera case or the
	// successful removal would have no undo record.
	bool targetNamesAChunk = false;
	if( kindText.empty() ) {
		int nameOccurrences = 0;
		RISE::Cst::DocFindByNameAnyRole(
			*doc, targetText, &nameOccurrences, "", false );
		targetNamesAChunk = nameOccurrences > 0;
	}
	const ChunkDescriptor* removeDescriptor = DescriptorForKeyword(
		String( kindText.empty() ? targetText.c_str() : kindText.c_str() ) );
	const bool unnamedRepeatable =
		removeDescriptor && removeDescriptor->unnamedRepeatable && !targetNamesAChunk;
	// Round-7: same GATED camera fallback as Job::ApplyCstRemoveChunk (lockstep
	// with the remove this capture precedes).
	const bool uniqueFallback =
		( kindText == "camera"
			? RISE::Cst::DocCameraUniqueFallbackPermitted( *doc, targetText, mJob.GetActiveCameraName() )
			: false )
		|| unnamedRepeatable;
	const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole(
		*doc, targetText, nullptr, kindText, uniqueFallback );
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
SceneEditController::AgentCommitResult SceneEditController::ApplyAgentParamEditInner_(
	const String& entityName,
	const String& entityKind,
	const String& param,
	const String& value,
	const RISE::Cst::CstHeadVersion* baseVersionOrNull )
{
	AgentCommitResult r;

	// F5 polish A1 round 2: refuse a MID-TRANSACTION agent commit before any
	// mutation or park.
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
	// BeginTransaction publishes the flag while holding this same mutex.
	// The under-lock recheck closes the false->Begin->commit TOCTOU that an
	// atomic pre-flight check alone would still permit.
	if( mTxnOpen.load( std::memory_order_acquire )
	 || mEditor.IsCompositeOpen() )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent commit refused while an editor transaction/gesture is open." );
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.retriable = true;
		r.message = String( "editor transaction or gesture in progress -- retry after it completes" );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}
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
	// Document-first ADR phase 3: an EMPTY target with a non-empty kind is the
	// KIND-ADDRESSED SINGLETON form (the sole `film` / `<kind>_rasterizer` /
	// unnamed-camera chunk) -- Job::ApplyCstParamEditImpl_ resolves it via the
	// generalized unique-in-kind fallback.  Only the both-empty form is invalid.
	if( ( entityName.size() <= 1 && entityKind.size() <= 1 )
	 || param.size() <= 1 || value.size() <= 1 )
	{
		// RString::size() counts the trailing NUL, so a non-empty string has
		// size >= 2; size <= 1 is the empty case.
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.headVersion = mJob.GetCstHeadVersion();
		r.message = String( "param and value must be non-empty, and target may be empty only with a kind (singleton addressing)" );
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

		if( entityKind.size() <= 1 )
		{
			// `kind` is optional on propose_patch.  A name-only camera edit
			// can resolve and apply incrementally, but this layer does not
			// receive Job's resolved descriptor.  Conservatively reconcile
			// every named-camera pane; non-camera name-only edits pay only a
			// small extra pane-camera lookup.
			InvalidateAllSceneCameraNamedPanesLocked_();
		}
		else if( IsCameraChunkKind( entityKind ) )
		{
			InvalidatePanesBoundToSceneCameraLocked_( entityName );
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
	AgentCommitResult r;
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		{
			r.status = String( "rejected" );
			r.retriable = true;
			r.message = String( "render queued or in progress -- retry after it completes" );
			return r;
		}
		r = ApplyAgentChunkCrud_( /*isInsert*/ true, chunkText, String(), baseVersionOrNull );
	}
	mEditor.DrainDirtyNotification();   // Document-first phase 1: post-unlock drain
	return r;
}

SceneEditController::AgentCommitResult SceneEditController::ApplyAgentRemoveChunk(
	const String& target,
	const String& kind,
	const RISE::Cst::CstHeadVersion* baseVersionOrNull )
{
	AgentCommitResult r;
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		{
			r.status = String( "rejected" );
			r.retriable = true;
			r.message = String( "render queued or in progress -- retry after it completes" );
			return r;
		}
		r = ApplyAgentChunkCrud_( /*isInsert*/ false, target, kind, baseVersionOrNull );
	}
	mEditor.DrainDirtyNotification();   // Document-first phase 1: post-unlock drain
	return r;
}

//////////////////////////////////////////////////////////////////////
// Secure-MCP slice 5a: proposal staging + owner-approval state machine.
//
// StageProposal is deliberately CHEAP and INERT: it only appends to
// mProposals under its leaf mutex -- no cancel-and-park (nothing here mutates
// the Document or live Scene), no conflict check (that happens at APPROVAL
// time), no EditHistory record (an inert proposal is not yet a commit).
//
// S5a hardening round: baseVersion is stamped from mJob.GetCstHeadVersion()
// HERE under render admission, not pre-read unlocked by AgentSession. When
// the render gate is clear we also take mMutex, exactly like a commit. When
// the gate is set, every controller write funnel refuses under this same
// admission lock and the render only reads the retained Document, so the
// head is immutable and can be captured without waiting behind the render's
// duration-long mMutex hold.
//////////////////////////////////////////////////////////////////////
std::uint64_t SceneEditController::StageProposal( const AgentProposal& proposal,
                                                   RISE::Cst::CstHeadVersion* outStagedVersion )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	RISE::Cst::CstHeadVersion stagedHead;
	if( !proposal.hasExplicitBaseVersion )
	{
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		{
			// A direct/coordinated render owns the scene but cannot mutate
			// its retained Document. Admission excludes every controller
			// commit until this head capture and enqueue are complete.
			stagedHead = mJob.GetCstHeadVersion();
		}
		else
		{
			std::lock_guard<std::mutex> sceneLk( mMutex );
			stagedHead = mJob.GetCstHeadVersion();
		}
	}

	std::lock_guard<std::mutex> proposalLk( mProposalMutex );

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
		// baseHeadVersion, so the admission-protected head-at-stage-time IS
		// the base instead of whatever potentially-racy value the caller
		// placed in proposal.baseVersion.
		p.baseVersion = stagedHead;
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
	std::lock_guard<std::mutex> proposalLk( mProposalMutex );
	return mProposals;   // copy out under the leaf lock -- stable and render-independent
}

bool SceneEditController::ResolveProposal( std::uint64_t id, bool approve, AgentCommitResult* outResult )
{
	auto dirtyNotificationDeferral = mEditor.DeferDirtyNotifications();
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	// Admission serializes resolvers and controller commits. Proposal
	// bookkeeping itself uses a leaf mutex, while ApplyAgent* remains the
	// authoritative optimistic check-and-apply under mMutex. This avoids
	// coupling inert queue reads/writes to a production render's long scene
	// lock without weakening the base-version invariant.
	AgentProposal snapshot;
	RISE::Cst::CstHeadVersion rejectedHead;
	if( !approve )
	{
		std::lock_guard<std::mutex> sceneLk( mMutex );
		rejectedHead = mJob.GetCstHeadVersion();
	}
	{
		std::lock_guard<std::mutex> proposalLk( mProposalMutex );
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
			// found the proposal. rejectedHead was captured under mMutex
			// while the outer admission lock excluded every concurrent
			// commit, so it is a safe, non-torn current head.
			if( outResult )
			{
				outResult->status      = String( "rejected" );
				outResult->headVersion = rejectedHead;
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
		std::lock_guard<std::mutex> proposalLk( mProposalMutex );
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
namespace {
// RAII scope that marks "a production/agent render owns the scene" for the
// duration of the render closure running under the mMutex render-hold.  Set
// around BOTH fn() sites (RunPreviewRenderParked sync-inline + the async
// AgentRenderWorkerLoop_).  A UI-callable mMutex method checks the same flag
// FIRST and no-ops/returns default instead of blocking on mMutex.  bool (not a
// counter) is correct: fn() is job->Rasterize(), which never re-enters either
// render path, so these two scopes never nest.
struct RenderOwnershipScope {
	explicit RenderOwnershipScope( std::atomic<bool>& f ) : mFlag( f ) {
		mFlag.store( true, std::memory_order_release );
	}
	~RenderOwnershipScope() { mFlag.store( false, std::memory_order_release ); }
	RenderOwnershipScope( const RenderOwnershipScope& ) = delete;
	RenderOwnershipScope& operator=( const RenderOwnershipScope& ) = delete;
private:
	std::atomic<bool>& mFlag;
};
}  // namespace

bool SceneEditController::RunPreviewRenderParked(
	const std::function<void()>& fn,
	RenderClass                  renderClass,
	const String&                clientLabel,
	RenderJobId*                 outJobId )
{
	// Register before touching admission or any other controller state.
	// Terminal Stop() closes this registration under the same leaf mutex and
	// then waits for every successfully-registered caller to make its final
	// controller touch. This includes a caller blocked before admission.
	{
		std::lock_guard<std::mutex> directLk( mDirectRenderStateMutex );
		if( mDirectRenderStopping )
		{
			return false;
		}
		++mDirectRenderCallCount;
	}
	struct DirectRenderCallGuard
	{
		SceneEditController& self;
		~DirectRenderCallGuard()
		{
			// LAST access to self on every return/throw path.
			std::lock_guard<std::mutex> directLk( self.mDirectRenderStateMutex );
			--self.mDirectRenderCallCount;
			if( self.mDirectRenderCallCount == 0 )
			{
				self.mDirectRenderDoneCV.notify_all();
			}
		}
	} directCallGuard{ *this };

	ForTest_OnDirectRenderBeforeAdmission();

	if( mTxnOpen.load( std::memory_order_acquire ) )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: preview render refused inside an open transaction (would stall the gesture)." );
		return false;
	}

	// Admission is a short outer lock, not a render-duration lock. Claim the
	// gate BEFORE waiting for mMutex, then release admission immediately.
	// This is load-bearing for UI responsiveness: an interactive BeautyVariant
	// pass can hold mRendering for much longer than the cheap preview path. If
	// this thread waited for that pass while still holding admission, every UI
	// callback serialized by admission would queue behind the whole pass.
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderStop.load( std::memory_order_acquire ) )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController: preview render refused -- controller stopped." );
		return false;
	}
	// The dedicated agent/production worker claims this gate before it
	// acquires mMutex. A direct camera/film-override preview must not slip
	// into that queued window and overwrite the coordinated job record.
	// Check BEFORE mMutex: a running coordinated render holds mMutex for
	// its whole duration, so checking afterward would wait for the render
	// only to refuse.
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController: preview render refused while another coordinated render is queued or running." );
		return false;
	}
	// Every controller path that opens a transaction/gesture/save takes this
	// same admission lock. Check those published states before claiming the
	// gate, so a render never strands an already-open gesture while it parks
	// the interactive pass. IsCompositeOpen additionally catches direct
	// SceneEditor test/tooling use that did not originate in the controller.
	if( mTxnOpen.load( std::memory_order_acquire )
	 || mSaving.load( std::memory_order_acquire )
	 || mPointerDown.load( std::memory_order_acquire )
	 || mScrubInProgress.load( std::memory_order_acquire )
	 || mEditor.IsCompositeOpen() )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController: preview render refused while an editor transaction, gesture, or save is open." );
		return false;
	}
	mDirectRenderCancelRequested.store( false, std::memory_order_release );
	mAgentRenderBlocksInteractive.store( true, std::memory_order_release );
	struct DirectRenderAdmissionGuard
	{
		SceneEditController& self;
		~DirectRenderAdmissionGuard()
		{
			// Round-6 review P1 (lost wakeup): clear the gate and notify UNDER
			// mMutex.  This guard destructs AFTER `lk` below (reverse declaration
			// order releases mMutex first), so an unlocked store+notify could land
			// exactly between RenderLoop's predicate check and its kernel park --
			// the classic missed-wakeup window (see the RenderLoop doc: notifiers
			// must hold the waiter's mutex).  The agent worker's release already
			// follows this discipline (AgentRenderWorkerLoop_'s renderLk hold);
			// this mirrors it.  mMutex is provably NOT held here on every path
			// (`lk` is destroyed first), so the re-acquire cannot self-deadlock.
			std::lock_guard<std::mutex> g( self.mMutex );
			self.mAgentRenderBlocksInteractive.store( false, std::memory_order_release );
			self.mCV.notify_all();
		}
	} directAdmissionGuard{ *this };
	admissionLk.unlock();

	// Trip cancellation without first taking mMutex. The render loop needs
	// that mutex only for its completion transition; acquiring it before the
	// cancel would make this admission handshake wait for the pass naturally.
	bool interactiveCancelSent = false;
	if( mRendering.load( std::memory_order_acquire ) )
	{
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		interactiveCancelSent = true;
	}
	std::unique_lock<std::mutex> lk( mMutex );
	if( !interactiveCancelSent && mRendering.load( std::memory_order_acquire ) )
	{
		// A pass may have crossed its final gate check just before our gate
		// claim and published mRendering while we were acquiring mMutex.
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	// The cancellation above belonged to the interactive pass we just
	// drained, not to this fresh direct render. Scope the shared token per
	// render exactly as AgentRenderWorkerLoop_ does; otherwise a camera/film
	// override render installs an already-cancelled callback on the Job and
	// immediately returns a partial/empty "render cancelled" result. Any
	// cancellation arriving after this reset remains visible to fn(). An
	// explicit cancellation that arrived before Reset() is re-applied from
	// the direct-occupant latch.
	mCancelProgress.Reset();
	if( mDirectRenderCancelRequested.load( std::memory_order_acquire ) )
	{
		mCancelProgress.RequestCancel();
	}

	// Defensive post-park validation. Controller-owned state cannot have
	// opened after the gate claim (all such paths now refuse under
	// admission), but direct SceneEditor test/tooling access is outside that
	// contract and must still fail closed rather than render mid-composite.
	if( mTxnOpen.load( std::memory_order_acquire )
	 || mSaving.load( std::memory_order_acquire )
	 || mPointerDown.load( std::memory_order_acquire )
	 || mScrubInProgress.load( std::memory_order_acquire )
	 || mEditor.IsCompositeOpen() )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController: preview render refused because editor state changed during admission." );
		return false;
	}

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

	{
		RenderOwnershipScope ownScope_( mRenderOwnsScene );
		if( fn ) fn();
	}

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
			// Reset() (see that call site). A concurrent CancelAgentRender_()
			// is retained through the slot-scoped atomic below; the shared
			// callback's own flag is atomic, so Reset/RequestCancel has no
			// torn state even though the cancel caller does not take mMutex.
			mCancelProgress.Reset();
			// A cancellation can land after the submission has claimed the
			// slot but before this worker reaches its per-render Reset().
			// Preserve that request for THIS occupant; otherwise Reset() would
			// silently turn a queued cancellation into a full render.
			if( mAgentRenderCancelRequested.load( std::memory_order_acquire ) )
			{
				mCancelProgress.RequestCancel();
			}

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
				RenderOwnershipScope ownScope_( mRenderOwnsScene );
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
			mAgentRenderCancelRequested.store( false, std::memory_order_release );
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

	// Claim the interactive gate and check transaction state under the
	// same brief mMutex hold. This closes both directions of the race:
	// an already-open transaction refuses this submission, while a
	// successfully claimed submission makes a later BeginTransaction
	// refuse until the worker clears the gate at completion.
	//
	// Nesting mMutex INSIDE the already-held slot lock is safe because
	// the single-slot check above guarantees no coordinated render is in
	// flight. This remains the sole lock order used by this machinery.
	{
		// Model-B F2 slice S2a: claim the interactive-loop gate for
		// the FULL duration of this render (mint through worker
		// completion) -- see mAgentRenderBlocksInteractive's doc for
		// the exact race this closes (RenderLoop's own mint block
		// stomping mCurrentRenderJob in the gap between this mint and
		// the worker actually starting).
		std::lock_guard<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		// Direct renders publish the gate before taking their
		// render-duration mMutex hold. Inspect it first so a coordinated
		// submission refuses immediately instead of waiting for that
		// direct render to finish merely to discover the conflict.
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		{
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController: render submission refused -- another direct/coordinated render has admission." );
			return false;
		}
		std::lock_guard<std::mutex> renderLk( mMutex );
		if( mTxnOpen.load( std::memory_order_acquire ) )
		{
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController: render submission refused inside an open transaction." );
			return false;
		}
		if( mSaving.load( std::memory_order_acquire ) )
		{
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController: render submission refused while a save is in progress." );
			return false;
		}
		if( mPointerDown.load( std::memory_order_acquire )
		 || mScrubInProgress.load( std::memory_order_acquire )
		 || mEditor.IsCompositeOpen() )
		{
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController: render submission refused while an editor gesture is open." );
			return false;
		}
		mAgentRenderBlocksInteractive.store( true, std::memory_order_release );
	}

	// Status readers use mJobStatusMutex rather than mMutex so they remain
	// non-blocking during a render. Mint only after the gate claim succeeds;
	// a transaction refusal must not publish a phantom active job.
	RenderJobId jobId = kInvalidRenderJobId;
	{
		std::lock_guard<std::mutex> statusLk( mJobStatusMutex );
		jobId = mNextRenderJobId += kControllerRenderJobIdStride;
		mCurrentRenderJob.id          = jobId;
		mCurrentRenderJob.renderClass = renderClass;
		mCurrentRenderJob.active      = true;
		mCurrentRenderJob.clientLabel = clientLabel;
		mCurrentRenderJob.pinned      = pinned;
	}

	mAgentRenderFn          = std::move( fn );
	mAgentRenderCancelRequested.store( false, std::memory_order_release );
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
	if( mTxnOpen.load( std::memory_order_acquire ) )
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

	// Cancel-and-park, then hold mMutex across the WHOLE commit (pre-flight,
	// conflict gate, the Job apply -- whose D2 ClearAll transiently zeroes the
	// head-version -- the rebind, and the post-commit head read).
	std::unique_lock<std::mutex> lk( mMutex );
	if( mTxnOpen.load( std::memory_order_acquire )
	 || mEditor.IsCompositeOpen() )
	{
		GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: agent chunk %s refused while an editor transaction/gesture is open.", verb );
		r.applied = false;
		r.rawCode = 0;
		r.status  = String( "rejected" );
		r.retriable = true;
		r.message = String( "editor transaction or gesture in progress -- retry after it completes" );
		r.headVersion = mJob.GetCstHeadVersion();
		return r;
	}
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
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return;
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
//   1. Park the render thread, snapshot the retained Document + loaded-file
//      identity + head version under mMutex, set mSaving=true.
//   2. Serialize/write the immutable snapshots outside the lock.
//   3. Reacquire the lock, publish file identity and clear dirty ONLY if
//      the live head still matches the serialized snapshot, then resume.
//
// The immutable snapshot is the save/read counterpart of the controller's
// write funnel: a concurrent editor mutation can replace pCstDocument safely
// while IO runs, and the version comparison keeps that newer head dirty.
SaveResult SceneEditController::RequestSave( const std::string& filePath )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) {  // render-owns-scene guard
		SaveResult roR; roR.status = SaveResult::Status::Refused;
		roR.filePath = filePath; roR.errorMessage = "save refused: a render is in progress";
		return roR;
	}
	// ---- Step 1: park render thread + mark save in flight -----------
	// Editor live-sync follow-up: remember whether we CANCELLED an
	// in-flight pass to get here — that pass's frame is left partially
	// updated, and with debounced auto-save (UI refinement item 1)
	// this now happens routinely, not just on a manual Save click.
	// Step 3 re-kicks a render when it does, so a save never strands a
	// half-painted viewport.
	bool interruptedPass = false;
	RISE::Cst::CstHeadVersion headAtSerialize;
	std::unique_ptr<RISE::Cst::Document> documentSnapshot;
	RISE::FileIdentity loadedFileIdentitySnapshot;
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) {
			SaveResult busy;
			busy.status = SaveResult::Status::Refused;
			busy.filePath = filePath;
			busy.errorMessage = "save refused: a render is queued or in progress";
			return busy;
		}
		std::unique_lock<std::mutex> lk( mMutex );
		// Exactly one save may own this controller. Besides making the
		// mSaving boolean an honest render barrier, this gives concurrent
		// callers a deterministic ordering instead of allowing two stale
		// snapshots to race their writes and file-identity publication.
		if( mSaving.load( std::memory_order_acquire ) ) {
			SaveResult busy;
			busy.status = SaveResult::Status::Refused;
			busy.filePath = filePath;
			busy.errorMessage = "save refused: another save is already in progress";
			return busy;
		}
		if( mRendering.load() ) {
			mCancelProgress.RequestCancel();
			interruptedPass = true;
		}
		mCV.wait( lk, [&]{ return !mRendering.load(); } );
		mSaving.store( true, std::memory_order_release );
		// Round-5 review P0: remember WHICH head the engine is about to
		// serialize.  Step 3 baselines "clean" ONLY if the head is still this
		// one -- a mutation that lands while IO runs can then never be silently
		// baselined as saved.
		headAtSerialize = mJob.GetCstHeadVersion();
		const RISE::Cst::Document* document = mJob.GetCstDocument();
		if( document ) {
			documentSnapshot.reset( new RISE::Cst::Document( *document ) );
			loadedFileIdentitySnapshot = mJob.GetCstLoadFileIdentity();
		}
	}

	// Fix-round-6 RED-PROVE test seam -- no-op in production (see the
	// declaration doc).  Sits right after mSaving flips true and mMutex
	// releases, so a test override can block here to hold mSaving true
	// for as long as it needs, giving RenderLoop's mint-site re-check a
	// deterministic (rather than IO-timing-dependent) window to prove
	// itself against.
	std::unique_ptr<SaveEngine> saveEngine;
	if( documentSnapshot ) {
		// Reserve the canonical target-path lease BEFORE the test hook (and
		// therefore before any IO).  A different controller that requests the
		// same target while this save is paused is refused deterministically.
		saveEngine.reset( new SaveEngine(
			*documentSnapshot, loadedFileIdentitySnapshot, filePath ) );
	}
	ForTest_OnSaveEngineAboutToRun();

	// ---- Step 2: run SaveEngine WITHOUT the lock --------------------
	// File IO can take a few ms; holding mMutex across it would stall UI
	// state. The engine receives no live Job/editor references.
	SaveResult result;
	if( saveEngine ) {
		result = saveEngine->Save();
	} else {
		result.status = SaveResult::Status::Failed;
		result.filePath = filePath;
		result.errorMessage =
			"cannot save '" + filePath + "': the job has no retained CST "
			"Document to serialize. Load the scene via the CST path before saving.";
		GlobalLog()->PrintEx( eLog_Error,
			"SceneEditController::RequestSave: no retained CST Document for '%s' -- nothing to serialize",
			filePath.c_str() );
	}

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
			// The successful write is now the on-disk identity baseline even
			// when a concurrent edit moved the live head: the file contains the
			// old snapshot and the next save of the newer head must not mistake
			// our own write for an external modification.
			mJob.RefreshCstLoadFileIdentity( filePath.c_str() );
			// Baseline-clean ONLY when the live head is still the serialized
			// one. A moved head means the file is a valid OLD snapshot, so the
			// newer scene stays dirty and its tracker state remains untouched.
			if( mJob.GetCstHeadVersion() == headAtSerialize ) {
				mEditor.ClearDirtyState();
			} else {
				GlobalLog()->PrintEx( eLog_Info,
					"SceneEditController::RequestSave: the Document head moved during the save -- "
					"the written file snapshots the PRE-edit head, so the scene stays DIRTY." );
			}
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
	// Document-first phase 1: the save-success ClearDirtyState above only
	// flagged the transition -- drain it here, outside the lock scope, so the
	// Save button disables promptly on the dirty->clean flip.
	mEditor.DrainDirtyNotification();

	return result;
}

String SceneEditController::SerializedSceneText() const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return String();  // render-owns-scene guard
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return String();
	if( !mJob.HasRetainedCstDocument() ) return String();
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return String();
	const std::string bytes = RISE::Cst::SerializeCst( *doc );
	return String( bytes.c_str() );
}

void SceneEditController::GetSceneTextVersion( std::uint64_t& outUuid,
                                               std::uint64_t& outRevision ) const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;  // render-owns-scene guard
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return;
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
	case Category::Camera:       outSuffix = "camera";         outUniqueFallback = true; return true;   // fallback ELIGIBLE -- use sites gate it via DocCameraUniqueFallbackPermitted (round-7), same rule as CaptureAgentPriorParamValue_
	case Category::Object:       outSuffix = "standard_object"; return true;
	case Category::Light:        outSuffix = "light";           return true;
	case Category::Material:     outSuffix = "material";        return true;
	case Category::Medium:       outSuffix = "medium";          return true;
	case Category::Animation:    outSuffix = "animation";       return true;
	case Category::SceneVariant: outSuffix = "scene_variant";   return true;
	case Category::Painter:      outSuffix = "painter";         return true;   // matches every "*_painter" chunk keyword (endsWith), same convention as ClassifyCstEntityKind's "_material"/"_light"/"_medium" suffixes
	case Category::Geometry:     outSuffix = "geometry";        return true;   // matches every "*_geometry" chunk keyword (endsWith) -- GUI redesign 2026-07-22
	case Category::Rasterizer:
	case Category::Film:
	case Category::None:
	default: return false;   // no chunk-name addressing scheme for this category -- see comment above
	}
}

}  // namespace

namespace {

// Source traceability (forward): resolve a (Category, name) UI address to its
// top-level CST chunk NodeId, covering the named categories + the unnamed-active
// Camera (via RoleKindSuffixForCategory's unique-fallback) AND the unnamed
// unique-in-kind singletons Film / Rasterizer (which RoleKindSuffixForCategory
// rejects, since they have no chunk-`name` addressing).  `activeRasterizerKind` is
// the active rasterizer's keyword (its CST role) for the Rasterizer singleton.
// `activeCameraName` is the LIVE active camera's registered name, feeding the
// round-7 gated camera fallback (DocCameraUniqueFallbackPermitted).
RISE::Cst::NodeId ResolveSourceChunkId( const RISE::Cst::Document& doc,
	SceneEditController::Category cat, const std::string& name, const std::string& activeRasterizerKind,
	const std::string& activeCameraName )
{
	std::string suffix; bool uf = false;
	if( RoleKindSuffixForCategory( cat, suffix, uf ) ) {
		if( cat == SceneEditController::Category::Camera && uf )
			uf = RISE::Cst::DocCameraUniqueFallbackPermitted( doc, name, activeCameraName );
		if( name.empty() && !uf ) return 0;   // an empty name resolves only via the unique-fallback
		return RISE::Cst::DocFindByNameAnyRole( doc, name, nullptr, suffix, uf );
	}
	// Singletons RoleKindSuffixForCategory rejects.  Film is a single unnamed
	// chunk (resolve by kind).  Rasterizer chunks are unnamed and identified by
	// KIND (their keyword), so `name` here carries the rasterizer KIND: empty ->
	// the ACTIVE rasterizer (the Environment section's (Rasterizer,"") convention);
	// non-empty -> that specific kind (so a reverse ref into a NON-active rasterizer
	// chunk round-trips to the SAME chunk instead of collapsing to the active one).
	if( cat == SceneEditController::Category::Film )
		return RISE::Cst::DocFindByNameAnyRole( doc, "", nullptr, "film", true );
	if( cat == SceneEditController::Category::Rasterizer ) {
		const std::string kind = name.empty() ? activeRasterizerKind : name;
		if( kind.empty() ) return 0;
		return RISE::Cst::DocFindByNameAnyRole( doc, "", nullptr, kind, true );
	}
	return 0;
}

// Source traceability (reverse): a CST chunk keyword (its role) -> the UI Category,
// for the text->UI direction.  DRIFT-PROOF: classifies by the parser registry's
// AUTHORITATIVE per-chunk ChunkCategory (built once, cached) rather than a
// hand-maintained suffix list -- a hand list silently misses non-suffix entity
// keywords the forward path resolves by name (csg_object / override_object ->
// Object, expression_function2d -> Painter, ...).  Category::None for a keyword
// whose chunk isn't a UI-addressable entity/singleton (Geometry / Shader / ShaderOp
// / Modifier / RasterizerOutput / PhotonMap / PhotonGather / IrradianceCache).
SceneEditController::Category CategoryForChunkKeyword( const std::string& kw )
{
	using Cat = SceneEditController::Category;
	static const std::map<std::string, Cat> kMap = []() {
		auto toCat = []( ChunkCategory cc ) -> Cat {
			switch( cc ) {
			case ChunkCategory::Painter:      return Cat::Painter;
			case ChunkCategory::Geometry:     return Cat::Geometry;   // GUI redesign 2026-07-22: geometry is now UI-addressable
			// NOTE: ChunkCategory::Function is NOT mapped to Painter.  The only two
			// Function chunks (piecewise_linear_function / _function2d) register into
			// the Function1D/2D managers, NOT the painter managers, so they never
			// appear in the Painter UI list -- mapping them to Painter would surface
			// a ref no Painter row can select.  (expression_function2d is registered
			// as ChunkCategory::Painter, so it is still covered by the Painter arm.)
			case ChunkCategory::Material:     return Cat::Material;
			case ChunkCategory::Camera:       return Cat::Camera;
			case ChunkCategory::Film:         return Cat::Film;
			case ChunkCategory::Object:       return Cat::Object;    // standard_object / csg_object / override_object
			case ChunkCategory::Medium:       return Cat::Medium;
			case ChunkCategory::Rasterizer:   return Cat::Rasterizer;
			case ChunkCategory::Light:        return Cat::Light;
			case ChunkCategory::Animation:    return Cat::Animation;
			case ChunkCategory::SceneVariant: return Cat::SceneVariant;
			default:                          return Cat::None;      // not a UI-addressable category
			}
		};
		std::map<std::string, Cat> m;
		const std::vector<ChunkParserEntry> entries = CreateAllChunkParsers();
		for( const ChunkParserEntry& e : entries ) {
			if( !e.parser ) continue;
			const Cat c = toCat( e.parser->Describe().category );
			if( c != Cat::None && !e.keyword.empty() ) m[ e.keyword ] = c;
		}
		return m;
	}();
	const auto it = kMap.find( kw );
	return ( it != kMap.end() ) ? it->second : Cat::None;
}

// Line (1-based) + column (1-based) of a byte offset in the serialized doc text.
void LineColOfOffset( const std::string& full, size_t off, unsigned int& outLine, unsigned int& outCol )
{
	unsigned int line = 1, col = 1;
	const size_t n = ( off <= full.size() ) ? off : full.size();
	for( size_t i = 0; i < n; ++i ) {
		if( full[i] == '\n' ) { ++line; col = 1; } else ++col;
	}
	outLine = line;
	outCol  = col;
}

}  // namespace

bool SceneEditController::EntitySourceLocation( Category cat, const String& name,
                                                 std::uint64_t& outByteOffset, std::uint32_t& outLine ) const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	if( !mJob.HasRetainedCstDocument() ) return false;
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return false;

	std::string suffix; bool uniqueFallback = false;
	if( !RoleKindSuffixForCategory( cat, suffix, uniqueFallback ) ) return false;
	if( cat == Category::Camera && uniqueFallback )   // round-7 gated camera fallback (typo-refusal)
		uniqueFallback = RISE::Cst::DocCameraUniqueFallbackPermitted(
			*doc, name.size() > 1 ? name.c_str() : "", mJob.GetActiveCameraName() );
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

bool SceneEditController::ResolveSourceSpan( Category cat, const String& name, const String& param,
                                             int occ, SourceSpan& out ) const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	out = SourceSpan();

	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	if( !mJob.HasRetainedCstDocument() ) return false;
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return false;

	const std::string activeRast = mJob.GetActiveRasterizerName();
	const RISE::Cst::NodeId chunkId =
		ResolveSourceChunkId( *doc, cat, std::string( name.c_str() ), activeRast,
		                      mJob.GetActiveCameraName() );
	if( chunkId == 0 ) return false;   // unresolvable ref (removed entity / ambiguous / no such singleton)

	size_t off = 0, len = 0;
	const std::string p( param.c_str() );
	if( p.empty() ) {
		// Whole-chunk span: byte offset of the chunk, no explicit length (the caller
		// may highlight the chunk's first line).
		const int idx = RISE::Cst::DocIndexOfNodeId( *doc, chunkId, nullptr );
		if( idx < 0 ) return false;
		off = RISE::Cst::DocByteOffsetOfItem( *doc, idx );
		if( off == static_cast<size_t>( -1 ) ) return false;
		len = 0;
	} else {
		// Param-granular span: the tight `role value…` run of the occ-th match.
		if( !RISE::Cst::DocByteRangeOfParam( *doc, chunkId, p, occ, &off, &len ) ) return false;
	}

	// Byte offset -> line/column over the serialized prefix (O(doc bytes); click cadence).
	const std::string full = RISE::Cst::SerializeCst( *doc );
	if( off > full.size() ) return false;   // defensive
	unsigned int line = 1, col = 1;
	LineColOfOffset( full, off, line, col );

	out.present    = true;
	out.byteOffset = static_cast<std::uint64_t>( off );
	out.byteLength = static_cast<std::uint64_t>( len );
	out.line       = line;
	out.column     = col;
	return true;
}

bool SceneEditController::SourceRefAtByteOffset( std::uint64_t offset, Category& outCat,
                                                 String& outName, String& outParam,
                                                 int* outOccurrence ) const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	outCat   = Category::None;
	outName  = String();
	outParam = String();
	if( outOccurrence ) *outOccurrence = 0;

	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	if( !mJob.HasRetainedCstDocument() ) return false;
	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return false;

	// Locate the chunk (and param, if the offset is on one) containing `offset`.
	// DocParamAtByteOffset returns the Param node (or null) and fills the enclosing
	// chunk; when the offset is inside a chunk but not on a param, chunk is still set.
	RISE::Cst::NodeRef chunk;
	int v = 0;
	RISE::Cst::NodeRef paramNode =
		RISE::Cst::DocParamAtByteOffset( *doc, static_cast<size_t>( offset ), &chunk, &v );

	if( !chunk ) {
		// Offset not resolved to a param's chunk (e.g. on the chunk header/braces):
		// fall back to the top-level item at this offset.
		RISE::Cst::NodeRef item; size_t start = 0; int v2 = 0;
		const int idx = RISE::Cst::DocItemAtByteOffset( *doc, static_cast<size_t>( offset ), &item, &start, &v2 );
		if( idx < 0 || !item ) return false;
		chunk = item;
	}
	if( !chunk || chunk->kind != RISE::Cst::NodeKind::Chunk ) return false;   // inter-chunk trivia / EOF

	const Category cat = CategoryForChunkKeyword( chunk->role );
	if( cat == Category::None ) return false;   // not a UI-addressable entity/singleton chunk

	// override_object is a legacy MODIFIER whose `name` param REFERENCES an existing
	// object (it modifies, it doesn't declare) -- so its name is present in the Object
	// enumeration but this chunk is NOT that entity.  Reject it so a click inside an
	// override_object block doesn't jump to the referenced standard_object elsewhere.
	if( chunk->role == "override_object" ) return false;

	auto ends = []( const std::string& s, const char* suf ) {
		const std::string t( suf );
		return s.size() >= t.size() && s.compare( s.size() - t.size(), t.size(), t ) == 0;
	};

	// Name.  Rasterizer chunks are unnamed and addressed by KIND, so the name IS
	// the keyword (a ref into a non-active rasterizer then round-trips).  Every
	// other category takes the name from ChunkNamePath ("keyword/name" -> segment
	// after the last '/'; an unnamed chunk yields empty).
	std::string nm;
	if( cat == Category::Rasterizer ) {
		nm = chunk->role;
	} else {
		const std::string np = RISE::Cst::ChunkNamePath( chunk );
		const size_t slash = np.rfind( '/' );
		if( slash != std::string::npos ) nm = np.substr( slash + 1 );
	}

	// ADDRESSABILITY: registry ChunkCategory is a PARSE grouping, not a statement
	// that a chunk is a UI-SELECTABLE entity -- several config/selector chunks carry
	// an entity category (scene_options/camera_defaults -> Camera; light_rr_threshold
	// -> Rasterizer; timeline/animation_options -> Animation; piecewise -> Function).
	// A chunk is reverse-addressable iff it's a real row the UI can select: a named
	// entity present in its category's LIVE enumeration, or the film / a rasterizer /
	// a camera SINGLETON identified by its keyword.  This is grounded in "can the
	// user actually select this?" and rejects every non-entity family robustly (no
	// keyword blocklist, no forward-round-trip that role-keyed configs can defeat).
	auto nameInEnum = [&]( Category c, const std::string& name ) {
		if( name.empty() ) return false;
		const unsigned int n = CategoryEntityCountLocked_( c );   // Locked_ twin: this probe runs under mMutex (round-3 getter split)
		for( unsigned int i = 0; i < n; ++i )
			if( std::string( CategoryEntityNameLocked_( c, i ).c_str() ) == name ) return true;
		return false;
	};

	bool addressable = false;
	switch( cat ) {
	case Category::Film:
		addressable = ( chunk->role == "film" );                 // the film singleton (not camera_defaults etc.)
		nm.clear();
		break;
	case Category::Rasterizer:
		addressable = ends( chunk->role, "_rasterizer" );        // a real rasterizer kind, NOT light_rr_threshold
		break;
	case Category::Camera:
		// A camera chunk (keyword) -- excludes scene_options/camera_defaults.  Named
		// -> must be in the camera list; unnamed -> the active camera (addressable).
		// KNOWN EDGE (accepted): with 2+ UNNAMED cameras, a click in a non-active one
		// still maps to (Camera,"") = the active camera -- the CST has no name to
		// distinguish them and the (Camera,"") convention resolves the active one.
		// Very narrow (multiple unnamed cameras); a net improvement over the prior
		// guard, which over-rejected the active unnamed camera whenever >=2 existed.
		if( chunk->role == "camera" || ends( chunk->role, "_camera" ) )
			addressable = nm.empty() ? true : nameInEnum( Category::Camera, nm );
		break;
	default:
		// Object / Light / Material / Medium / Painter / Animation / SceneVariant:
		// a genuine entity is enumerated in its category's live UI list (excludes the
		// unnamed animation_options/timeline and the non-painter piecewise chunks;
		// maps the active_scene_variant selector to the declared variant it names).
		addressable = nameInEnum( cat, nm );
		break;
	}
	if( !addressable ) return false;

	outCat  = cat;
	outName = String( nm.c_str() );
	if( paramNode && paramNode->kind == RISE::Cst::NodeKind::Param ) {
		outParam = String( paramNode->role.c_str() );
		// Occurrence: index of this param among same-role Param siblings (so a click
		// inside the k-th repeat of a repeatable param round-trips to occ=k).
		if( outOccurrence ) {
			int occ = 0;
			for( const RISE::Cst::NodeRef& kid : chunk->kids ) {
				if( !kid || kid->kind != RISE::Cst::NodeKind::Param || kid->role != paramNode->role ) continue;
				if( kid.get() == paramNode.get() ) break;   // reached the matched param
				++occ;
			}
			*outOccurrence = occ;
		}
	}
	return true;
}

std::string SceneEditController::LastSaveError() const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return std::string();  // render-owns-scene guard
	// Snapshot under the lock for the diagnostic-logger thread-safety
	// concern (P2.2 review fix): a reader holding the result by
	// reference across a subsequent RequestSave would see a torn
	// std::string mid-mutation.  Returning by value (after a locked
	// copy) eliminates that.
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return std::string();
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
		bool rotationTick = false;   // P3a slice 2: consumed alongside mEditPending below
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
				// P3a slice 2: mPanePassPending is the rotation's wake flag
				// (a completed quantum left OTHER visible panes with work).
				// Same gating as mEditPending -- a rotation pass must not
				// start while a save or agent render owns the window.
				return ( ( mEditPending.load( std::memory_order_acquire )
				        || mPanePassPending.load( std::memory_order_acquire ) )
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
			// P3a slice 2: consume the rotation flag under the SAME lock
			// hold, and mark every visible pane dirty on a real edit (the
			// scene changed under all of them).  Both run while mMutex is
			// held -- MarkAllVisiblePanesDirtyLocked_ requires it.
			rotationTick = mPanePassPending.exchange( false, std::memory_order_acq_rel );
			if( isExplicitEdit )
			{
				MarkAllVisiblePanesDirtyLocked_();
			}
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
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		{
			// P3a slice 2: UNLIKE mEditPending (not consumed on this
			// timeout path -- see the comment above), rotationTick WAS
			// consumed by the exchange; dropping it here would strand the
			// remaining panes' work until an unrelated edit.  Restore it.
			if( rotationTick )
			{
				mPanePassPending.store( true, std::memory_order_release );
			}
			continue;
		}

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
		{
			// Re-check the flag AND age under the same lock BeginPropertyScrub
			// uses to publish its refreshed timestamp + true flag.  Without
			// this serialization, a stale watchdog observation could clear a
			// newly-begun scrub that refreshed the timestamp concurrently.
			std::lock_guard<std::mutex> lk( mMutex );
			if( mScrubInProgress.load( std::memory_order_acquire ) )
			{
				const long long sinceEditMs =
					NowMs() - mLastEditTimeMs.load( std::memory_order_acquire );
				if( sinceEditMs > kScrubWatchdogMs )
				{
					// A lost EndPropertyScrub must perform the same scheduler
					// recovery as the real End: restore final quality and mint
					// an all-visible-pane repaint now.  Merely clearing the pin
					// strands dirty siblings forever for a pinned BeautyVariant,
					// because its idle-refinement branch intentionally skips.
					mScrubInProgress.store( false, std::memory_order_release );
					SetPreviewScaleIfUnpinned_( kPreviewScaleMin );
					mPolishState.store( static_cast<int>( PolishState::None ),
					                    std::memory_order_release );
					MarkAllVisiblePanesDirtyLocked_();
					rotationTick = true;
				}
			}
		}

		if( !isExplicitEdit && !rotationTick )
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

			// GUI render modes P2a: a BeautyVariant mode's PINNED divisor
			// must never be walked down toward 1 -- see mPreviewScalePinned's
			// header doc.
			if( mPreviewScalePinned.load( std::memory_order_acquire ) ) continue;

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
		// is null in test mode).  `polishStateBefore` / `isPolishPass`
		// are snapshotted -- and APPLIED to mInteractiveImpl via
		// SetPreviewDenoiseMode/SetSampleCount -- inside the mint lock
		// below, not here.  External review P1: this used to run
		// UNLOCKED at this exact spot, but mRendering is FALSE for this
		// entire pre-mint window (it only flips true inside that lock),
		// so a concurrent SetViewportRenderMode / SetViewportXray --
		// which take mMutex then call CancelAndParkRender_, whose wait
		// returns IMMEDIATELY when mRendering is already false -- could
		// mutate the SAME InteractivePelRasterizer (SetViewModeCaster /
		// SetXrayView) concurrently with an unlocked SetPreviewDenoiseMode
		// / SetSampleCount call here.  InteractivePelRasterizer has no
		// internal synchronization of its own (every Set* is documented
		// as "called only under the controller's parked-render
		// discipline") so that was a genuine data race on its plain
		// (non-atomic) fields.  See the mint-lock block below for where
		// this now runs and why moving it there also closes the P2
		// stale-PolishQueued-across-mode-switch bug (SetViewportRenderMode
		// / SetViewportXray's own mPolishState reset).
		PolishState polishStateBefore = PolishState::None;
		bool isPolishPass = false;

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
		unsigned int thisPassPane = 0;
		{
			std::lock_guard<std::mutex> lk( mMutex );
			if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire )
			 || mSaving.load( std::memory_order_acquire ) )
			{
				if( isExplicitEdit )
				{
					mEditPending.store( true, std::memory_order_release );
				}
				// P3a slice 2: same conditional-restore treatment for the
				// rotation flag (Fix-round-5's reasoning applies verbatim).
				if( rotationTick )
				{
					mPanePassPending.store( true, std::memory_order_release );
				}
				continue;
			}

			// P3a slice 2: pick + context-switch to the next pane INSIDE
			// the mint lock, BEFORE the polish snapshot below -- the
			// snapshot must read the TARGET pane's restored registers
			// (mPolishState, mVariantRasterizer), not the previous pane's.
			// mRendering is false for this whole block (it only flips true
			// at the mint), so the parked-registers precondition holds.
			// Gestures pin the pick to mCurrentPane (see PickNext's doc).
			SwitchToPaneLocked_( PickNextVisiblePaneLocked_() );
			thisPassPane = mCurrentPane;
			// Clear the pane's dirty AT MINT: if this pass is cancelled by
			// a new edit, the edit path re-marks every pane dirty anyway.
			// Cancellation by a direct/agent render has no edit path, so
			// the post-pass cancellation branch below re-dirties this exact
			// snapshotted pane before moving on.
			mPaneRender[mCurrentPane].dirty = false;
			// P3a slice 3: stamp the pane's surface dims for the pass --
			// registers, read by DoOneRenderPass's film-swap block.
			mCurrentPaneSurfaceW = mPaneConfigs[mCurrentPane].surfaceW;
			mCurrentPaneSurfaceH = mPaneConfigs[mCurrentPane].surfaceH;
			// External review P1 fix: snapshot mPolishState and apply it
			// to mInteractiveImpl HERE, inside the SAME mMutex hold
			// SetViewportRenderMode / SetViewportXray take before calling
			// CancelAndParkRender_ -- see the (now-empty) snapshot site
			// above this block for the race this closes.  Serialising the
			// read AND the mutate under this one lock also closes the P2
			// sibling bug: a mode/x-ray switch's own mPolishState reset
			// (see those setters) can no longer race a stale unlocked
			// isPolishPass snapshot taken just before it -- whichever
			// thread wins this mMutex acquisition runs its read+mutate to
			// completion before the other can observe or change any of
			// this state.
			polishStateBefore =
				static_cast<PolishState>( mPolishState.load( std::memory_order_acquire ) );
			isPolishPass = ( polishStateBefore == PolishState::PolishQueued );
			// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): SKIP this
			// InteractivePelRasterizer-specific config entirely while a
			// BeautyVariant mode is active -- mInteractiveImpl is not the
			// rasterizer DoOneRenderPass will drive this pass (mVariantRasterizer
			// is), and the variant pipeline's OIDN/sample-count are FIXED at
			// construction (CreateBeautyVariantPipeline), not per-pass tunable.
			// A variant pass is a plain full pass at that fixed config; the
			// polish/denoise state machine (isPolishPass, mPolishState) is left
			// running as-is (it's a no-op against mVariantRasterizer either way)
			// so leaving the mode later finds it in a consistent state.
			if( mInteractiveImpl && !mVariantRasterizer ) {
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
		ForTest_OnInteractivePassMinted();

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
		bool passWasCancelled = false;
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
			// Snapshot before ActiveFlipGuard publishes mRendering=false.
			// A parked render is waiting on that transition and may reset
			// the shared token immediately afterward.
			passWasCancelled = mCancelProgress.IsCancelRequested();
		}

		if( passWasCancelled )
		{
			// A canceled quantum did not produce a complete frame for the
			// pane whose dirty bit was consumed at mint. Requeue that pane
			// unless StopInteractive is tearing the loop down. Hidden panes
			// remain dirty for their next reveal but do not arm a pointless
			// visible-pane rotation.
			std::lock_guard<std::mutex> lk( mMutex );
			if( mRunning.load( std::memory_order_acquire ) )
			{
				mPaneRender[thisPassPane].dirty = true;
				if( thisPassPane < PaneCountForLayout( mViewportLayout ) )
				{
					mPanePassPending.store( true, std::memory_order_release );
				}
				mCV.notify_one();
			}
			// A canceled FinalRegularRunning/PolishQueued pass is not a
			// completed quality transition. Skip the polish post-roll; the
			// replacement quantum will perform it after real completion.
			continue;
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
			// External review P1 sibling fix: this ALSO mutates the
			// shared InteractivePelRasterizer, and it runs after
			// ActiveFlipGuard's destructor has already flipped
			// mRendering back to false -- the identical unlocked-window
			// hazard as the pre-pass snapshot block above, just on the
			// completion side.  Take mMutex for this short configuration
			// section (NOT across DoOneRenderPass -- that already ran)
			// so it can't interleave with SetViewportRenderMode /
			// SetViewportXray's own mInteractiveImpl mutations.
			{
				std::lock_guard<std::mutex> lk( mMutex );
				if( mInteractiveImpl ) {
					mInteractiveImpl->SetSampleCount( 1 );
					mInteractiveImpl->SetPreviewDenoiseMode(
						Implementation::InteractivePelRasterizer::PreviewDenoise_Off );
				}
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
				//
				// review-r2-B P2: routed via mPanePassPending, NOT
				// mEditPending -- the edit flag's consumption marks every
				// visible pane dirty (a scene-edit semantic), so the old
				// routing made EVERY gesture-end spuriously re-render all
				// settled panes (up to 3 wasted passes per gesture in
				// Quad).  The rotation flag wakes the loop without the
				// invalidation; PickNext then selects this pane via its
				// polishQueued priority.
				std::lock_guard<std::mutex> lk( mMutex );
				mPanePassPending.store( true, std::memory_order_release );
				mCV.notify_one();
			}
		}

		// P3a slice 2: end-of-quantum rotation arm.  When OTHER visible
		// panes still have work (dirty from an edit that invalidated all,
		// or a saved PolishQueued), arm mPanePassPending so the wait
		// predicate falls straight through and the next iteration switches
		// to them (PickNextVisiblePaneLocked_'s priority order).  Under
		// mMutex: AnyVisiblePaneHasWorkLocked_ reads pane slots.  No
		// notify needed -- this is the loop's own thread; the predicate is
		// evaluated on wait entry.
		//
		// T0 (2026-07-24): DO suppress the self-arm while a gesture pins the
		// scheduler.  The old code armed for dirty sibling panes, then
		// PickNextVisiblePaneLocked_ honored gesture exclusivity and picked
		// the CURRENT pane again.  Its dirty bit had already been consumed
		// at mint, but the siblings stayed dirty, so completion armed again:
		// an infinite render loop on the pinned pane until pointer-up.  A
		// BeautyVariant made this look like a hard GUI hang because every
		// useless iteration was a fixed 12-SPP/OIDN pass.  Pointer-up publishes
		// its final edit wake atomically with gesture closure, and
		// EndPropertyScrub calls KickRender(); both resume deferred sibling
		// rotation immediately.  Each in-gesture edit already carries its own
		// explicit kick, so no useful pass is lost by waiting here.
		{
			std::lock_guard<std::mutex> lk( mMutex );
			const bool gestureActive =
				mPointerDown.load( std::memory_order_acquire )
			 || mScrubInProgress.load( std::memory_order_acquire );
			if( !gestureActive && AnyVisiblePaneHasWorkLocked_() )
			{
				mPanePassPending.store( true, std::memory_order_release );
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
		// mUi.propertiesByCategory[Category::Painter] directly, indexed by
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
	// Document-first ADR phase 1: per-frame catch-all drain (shells call this
	// every frame) -- a mutation path that missed an explicit drain degrades
	// to a one-frame notification delay instead of a lost transition.  Runs
	// BEFORE the try_lock below, i.e. outside any controller lock.
	if( !mEditor.DrainDirtyNotification() ) return;
	// External-review P1 (2026-07-22): the row build below reads LIVE scene /
	// CST / manager state (buildRowsFor across every expanded category --
	// geometry enumeration, material-slot classification, CST introspection,
	// live candidate-name enumeration).  A concurrent agent commit on another
	// thread D2-rederives under mMutex, freeing + replacing those managers ->
	// use-after-free.  Serialize under mMutex.  During a production/agent
	// render the render OWNS the scene and holds mMutex for its whole duration;
	// follow the getter convention (see the mRenderOwnsScene member doc) and
	// keep the prior rows rather than block the UI thread -- the scene is
	// read-only then, so the panel simply shows the last snapshot until the
	// render ends.  No internal caller holds mMutex here (only the C-ABI, on
	// the UI thread), so the lock cannot self-deadlock.
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return;
	// Round-3 P1 (deadlock fix): try_lock, NOT a blocking guard.  The dirty-
	// changed listener fires UNDER the held mMutex (ApplyAgentParamEdit ->
	// MarkCstHeadDirty); a listener that synchronously refreshed the panel
	// would self-deadlock on a blocking lock, and even the shells' deferred
	// listeners can land a refresh while a background agent commit still
	// holds mMutex -- blocking would freeze the UI thread for the commit's
	// duration.  On contention keep the PRIOR rows (panel shows the last
	// snapshot); the shells refresh every frame/epoch, so it self-heals.
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return;
	// Round-5 review P1: per-frame self-heal of the cached derived-dirty
	// floor -- the Document is stable under this try_lock, so any mutation
	// path that missed its Fire is corrected within a frame.
	mEditor.RefreshDerivedDirty();

	// Document-first ADR phase 2: build into LOCALS (under the mMutex
	// try_lock above), publish under a brief leaf mUiSnapshotMutex hold at
	// the end -- readers never see a half-built panel and never take mMutex.
	std::vector<CameraProperty> primaryRows;
	std::vector<CameraProperty> byCat[kNumCategories];
	auto publish = [&]() {
		std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
		mUi.properties.swap( primaryRows );
		for( int i = 0; i < kNumCategories; ++i ) mUi.propertiesByCategory[i].swap( byCat[i] );
	};

	const IScene* scene = mJob.GetScene();
	if( !scene ) { publish(); return; }   // skeleton: publish the EMPTY panel (matches the old clear-then-return)

	// Build per-category property rows for every category that has
	// a non-empty selection.  Each section in the panel renders its
	// own rows from `mUi.propertiesByCategory[cat]`.  For back-compat
	// with the single-tuple PropertyXxx accessors, also populate
	// `mUi.properties` from the PRIMARY category's rows.
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
			// GUI redesign 2026-07-22: jump-to-definition metadata only
			// (no value/editability change) -- see AnnotateReferenceRows.
			CstIntrospection::AnnotateReferenceRows( out, mJob.GetCstDocument(), selName, "standard_object" );
			break;
		}
		case Category::Light: {
			const ILightManager* lights = scene->GetLights();
			if( !lights ) break;
			const ILightPriv* light = lights->GetItem( selName.c_str() );
			if( !light ) break;
			out = LightIntrospection::Inspect( selName, *light );
			// GUI redesign 2026-07-22: jump-to-definition metadata only
			// (no value/editability change) -- see AnnotateReferenceRows.
			CstIntrospection::AnnotateReferenceRows( out, mJob.GetCstDocument(), selName, "light" );
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
			// GUI redesign 2026-07-22: merge in the generic descriptor+CST
			// rows -- read-only live rows with a CST-backed param become
			// editable (the SetMaterialProperty edit route writes the CST
			// anyway), descriptor params the live module never surfaced
			// appear, and Reference rows gain jump-to-definition metadata.
			// No-op on a programmatic/legacy scene (no CST chunk).
			CstIntrospection::AugmentWithCstRows( out, mJob.GetCstDocument(), mJob,
				selName, "material" );
			break;
		}
		case Category::Medium: {
			const IMedium* med = mJob.GetMedium( selName.c_str() );
			if( !med ) break;
			out = MediaIntrospection::Inspect( selName, *med );
			// GUI redesign 2026-07-22: jump-to-definition metadata only
			// (no value/editability change) -- see AnnotateReferenceRows.
			CstIntrospection::AnnotateReferenceRows( out, mJob.GetCstDocument(), selName, "medium" );
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
			// IScalarPainter) -- see CstIntrospection.h's header doc.
			if( selName.size() <= 1 ) break;
			out = CstIntrospection::Inspect( mJob.GetCstDocument(), mJob, selName,
				"painter", "Painter chunk keyword" );
			break;
		}
		case Category::Geometry: {
			// GUI redesign 2026-07-22: same generic descriptor+CST surface
			// as Painter -- geometry chunks have no live per-parameter
			// getter either (a sphere's radius is baked at construction);
			// the CST is the editable source of truth, and an edit
			// re-derives the geometry + every object referencing it.
			if( selName.size() <= 1 ) break;
			out = CstIntrospection::Inspect( mJob.GetCstDocument(), mJob, selName,
				"geometry", "Geometry chunk keyword" );
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
		byCat[i] = buildRowsFor( cat, mSelectionByCategory[i] );
	}

	// Back-compat single-tuple snapshot drives the existing
	// PropertyCount() / PropertyName(idx) / ... accessors.  Routes
	// the primary category's rows.
	switch( CurrentPanelMode() ) {
	case PanelMode::Camera:
		primaryRows = byCat[ static_cast<int>( Category::Camera ) ];
		break;
	case PanelMode::Rasterizer:
		primaryRows = byCat[ static_cast<int>( Category::Rasterizer ) ];
		break;
	case PanelMode::Object:
		primaryRows = byCat[ static_cast<int>( Category::Object ) ];
		break;
	case PanelMode::Light:
		primaryRows = byCat[ static_cast<int>( Category::Light ) ];
		break;
	case PanelMode::Film:
		primaryRows = byCat[ static_cast<int>( Category::Film ) ];
		break;
	case PanelMode::Material:
		primaryRows = byCat[ static_cast<int>( Category::Material ) ];
		break;
	case PanelMode::Medium:
		primaryRows = byCat[ static_cast<int>( Category::Medium ) ];
		break;
	case PanelMode::None:
	default:
		// GUI redesign 2026-07-22: Painter and Geometry have no dedicated
		// PanelMode value (their rows are the generic descriptor+CST
		// surface), but the SHELLS' single-panel path reads the PRIMARY
		// snapshot -- without this routing, selecting a painter in the
		// outliner showed an EMPTY panel (a real pre-existing gap the
		// per-category accessors masked in tests), and geometry would
		// have inherited it.  Deliberately NOT extended to Animation /
		// SceneVariant: their empty primary snapshot drives the shells'
		// explanatory "selecting activates..." message.
		if( mSelectionCategory == Category::Painter
		 || mSelectionCategory == Category::Geometry )
		{
			primaryRows = byCat[ static_cast<int>( mSelectionCategory ) ];
		}
		break;
	}

	publish();
}

unsigned int SceneEditController::PropertyCount() const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	return static_cast<unsigned int>( mUi.properties.size() );
}

String SceneEditController::PropertyName( unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return String();
	return mUi.properties[idx].name;
}

String SceneEditController::PropertyValue( unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return String();
	return mUi.properties[idx].value;
}

String SceneEditController::PropertyDescription( unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return String();
	return mUi.properties[idx].description;
}

int SceneEditController::PropertyKind( unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return -1;
	return static_cast<int>( mUi.properties[idx].kind );
}

bool SceneEditController::PropertyEditable( unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return false;
	return mUi.properties[idx].editable;
}

unsigned int SceneEditController::PropertyPresetCount( unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return 0;
	return static_cast<unsigned int>( mUi.properties[idx].presets.size() );
}

String SceneEditController::PropertyPresetLabel( unsigned int idx, unsigned int presetIdx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return String();
	const auto& presets = mUi.properties[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].label.c_str() );
}

String SceneEditController::PropertyPresetValue( unsigned int idx, unsigned int presetIdx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return String();
	const auto& presets = mUi.properties[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].value.c_str() );
}

String SceneEditController::PropertyUnitLabel( unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	if( idx >= mUi.properties.size() ) return String();
	return mUi.properties[idx].unitLabel;
}

// -------------------------------------------------------------------
// Jump-to-definition (GUI redesign, 2026-07-22).  See the header doc.
// -------------------------------------------------------------------

namespace {

// ChunkCategory (the descriptor's target vocabulary) -> UI Category.
// Only categories with a UI section map; Function maps to Painter
// because colour painters dual-register as functions (Cst.cpp PASS A)
// and function chunks have no UI section of their own -- a function
// reference that resolves against the painter union IS selectable
// there.  Unmappable kinds (Shader / ShaderOp / Modifier / ...) return
// None: the jump is unavailable until those grow UI sections.
inline RISE::SceneEditController::Category UiCategoryForChunkCategory( RISE::ChunkCategory cc )
{
	using Cat = RISE::SceneEditController::Category;
	switch( cc )
	{
	case RISE::ChunkCategory::Painter:   return Cat::Painter;
	case RISE::ChunkCategory::Function:  return Cat::Painter;
	case RISE::ChunkCategory::Material:  return Cat::Material;
	case RISE::ChunkCategory::Geometry:  return Cat::Geometry;
	case RISE::ChunkCategory::Medium:    return Cat::Medium;
	case RISE::ChunkCategory::Object:    return Cat::Object;
	case RISE::ChunkCategory::Camera:    return Cat::Camera;
	case RISE::ChunkCategory::Light:     return Cat::Light;
	default:                             return Cat::None;
	}
}

}  // namespace

bool SceneEditController::PropertyJumpTarget(
	unsigned int idx, Category& outCat, String& outName ) const
{
	outCat = Category::None;
	outName = String();
	// Phase 2: COPY the row out under the leaf lock, resolve after releasing
	// it -- ResolveRowJumpTarget_ try_locks mMutex, and the leaf must never
	// be held while acquiring another lock.
	CameraProperty row;
	{
		std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
		if( idx >= mUi.properties.size() ) return false;
		row = mUi.properties[idx];
	}
	return ResolveRowJumpTarget_( row, outCat, outName );
}

bool SceneEditController::PropertyJumpTargetFor(
	Category cat, unsigned int idx, Category& outCat, String& outName ) const
{
	outCat = Category::None;
	outName = String();
	const int ci = static_cast<int>( cat );
	if( ci <= 0 || ci >= kNumCategories ) return false;
	CameraProperty row;   // phase 2: copy-then-resolve (see PropertyJumpTarget)
	{
		std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
		const std::vector<CameraProperty>& rows = mUi.propertiesByCategory[ci];
		if( idx >= rows.size() ) return false;
		row = rows[idx];
	}
	return ResolveRowJumpTarget_( row, outCat, outName );
}

// Shared resolution body.  First-wins across the row's declared
// referenceCategories, probing the LIVE managers -- the same category
// order ComputeChunkRefs resolves reference edges in, so the jump lands
// on the element the renderer actually bound.
bool SceneEditController::ResolveRowJumpTarget_(
	const CameraProperty& row, Category& outCat, String& outName ) const
{
	if( row.kind != ValueKind::Reference ) return false;
	if( row.value.size() <= 1 ) return false;
	if( row.referenceCategories.empty() ) return false;
	// The `none` sentinel is the registered null painter/geometry, not an
	// authored binding -- an optional reference left unset resolves to it.
	// Jumping there would land on a non-CST placeholder entity, so treat it
	// as "no target" (the shells then hide the menu item).
	if( std::string( row.value.c_str() ) == "none" ) return false;
	// jump-UAF fix (external review 2026-07-22): CandidateNamesForChunkCategory
	// enumerates the LIVE managers, which an agent CST edit on another thread
	// can swap mid re-derive (ClearAll under mMutex).  try_lock so we either
	// enumerate under a stable manager set or bail (no jump this click) rather
	// than racing a manager free -- and NEVER block, since a production/agent
	// render holds mMutex for its whole duration (a blocking guard would freeze
	// the UI).  A jump missed during an active render is retried on the next click.
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	for( ChunkCategory rc : row.referenceCategories )
	{
		const Category ui = UiCategoryForChunkCategory( rc );
		if( ui == Category::None ) continue;
		const std::vector<String> names =
			CstIntrospection::CandidateNamesForChunkCategory( mJob, rc );
		for( const String& n : names )
		{
			if( n == row.value )
			{
				outCat  = ui;
				outName = row.value;
				return true;
			}
		}
	}
	return false;
}

// External-review P1 (2026-07-22): general dangling-reference guard for GUI
// property edits.  See the header doc.  Rejects ONLY the precisely-
// diagnosable runtime-only case; inline literals + CST-backed names + `none`
// all pass.  Reads the CST doc + live managers under a brief mMutex hold (same
// race as the jump / material-classification reads: an agent commit can swap
// them mid re-derive); SetProperty already cleared the mRenderOwnsScene gate
// before calling this, so the hold contends only with brief interactive
// per-pass / agent-edit holds -- never a whole production render.
bool SceneEditController::WouldPersistDanglingReference_(
	Category cat, const String& entityName, const String& paramName, const String& value )
{
	if( value.size() <= 1 ) return false;
	if( std::string( value.c_str() ) == "none" ) return false;   // null sentinel is always valid

	std::lock_guard<std::mutex> lk( mMutex );

	const RISE::Cst::Document* doc = mJob.GetCstDocument();
	if( !doc ) return false;   // legacy scene (no retained CST) -> nothing to persist into

	// Resolve the entity's chunk + the edited param's descriptor.
	const RISE::Cst::NodeId id = ResolveSourceChunkId(
		*doc, cat, std::string( entityName.c_str() ), std::string(),
		mJob.GetActiveCameraName() );
	if( id == 0 ) return false;
	const RISE::Cst::NodeRef chunk = RISE::Cst::DocResolveNodeId( *doc, id );
	if( !chunk ) return false;
	const ChunkDescriptor* cd = DescriptorForKeyword( String( chunk->role.c_str() ) );
	if( !cd ) return false;
	const ParameterDescriptor* pd = nullptr;
	const std::string want( paramName.c_str() );
	for( const ParameterDescriptor& p : cd->parameters )
		if( p.name == want ) { pd = &p; break; }
	if( !pd || pd->kind != ValueKind::Reference ) return false;   // not a reference param

	// CST-resolvable?  Round-3 P1 fix: the earlier any-role name probe let a
	// same-named chunk of the WRONG category (a geometry named like the runtime
	// painter) or a LATER-declared chunk vouch for the value -- both save a
	// non-derivable scene.  Mirror the resolver instead: the target must be a
	// chunk whose registry category satisfies one of the param's declared
	// referenceCategories (including BuildReferenceNamespace's two documented
	// dual-registers: a colour painter doubles as a Function2D, and a
	// piecewise_linear_function doubles as a colour painter), and it must be
	// declared BEFORE the referencing chunk (DeriveToJob is sequential -- a
	// forward reference is not registered yet at bind time).
	const int refCount = RISE::Cst::DocItemCount( *doc );
	int refIdx = -1;
	for( int i = 0; i < refCount; ++i )
		if( RISE::Cst::DocNodeIdAt( *doc, i ) == id ) { refIdx = i; break; }
	if( refIdx < 0 ) return false;   // cannot locate the referencer -> defer to the derive path
	auto categorySatisfies = [&]( ChunkCategory chunkCat, const std::string& role, ChunkCategory rc ) {
		if( chunkCat == rc ) return true;
		if( rc == ChunkCategory::Function && chunkCat == ChunkCategory::Painter
		 && role != "scalar_painter" ) return true;   // colour painter dual-registers as Function2D
		if( rc == ChunkCategory::Painter && role == "piecewise_linear_function" )
			return true;                                 // 1D function dual-registers as a colour painter
		return false;
	};
	const std::string wantName( value.c_str() );
	for( int i = 0; i < refIdx; ++i )
	{
		const RISE::Cst::NodeRef ch = RISE::Cst::DocResolveNodeId( *doc, RISE::Cst::DocNodeIdAt( *doc, i ) );
		if( !ch || ch->kind != RISE::Cst::NodeKind::Chunk ) continue;
		const std::string np = RISE::Cst::ChunkNamePath( ch );
		const size_t slash = np.rfind( '/' );
		if( slash == std::string::npos ) continue;                      // unnamed: not referenceable
		if( np.compare( slash + 1, std::string::npos, wantName ) != 0 ) continue;
		const ChunkDescriptor* tcd = DescriptorForKeyword( String( ch->role.c_str() ) );
		if( !tcd ) continue;
		for( ChunkCategory rc : pd->referenceCategories )
			if( categorySatisfies( tcd->category, ch->role, rc ) )
				return false;   // category-correct, earlier-declared CST target -> persistable
	}

	// No category-correct, earlier-declared CST target.  Reject ONLY if `value`
	// is a RUNTIME-registered entity
	// of an allowed referenceCategory -- that confirms it is a reference NAME (not
	// an inline literal like `ior 1.7`) that renders now but would DANGLE on
	// save/reload.  A value resolving in NEITHER the CST nor a manager is an
	// inline literal / not-yet-created name; leave it to the existing derive path
	// (conservative -- never a false rejection of a legitimate edit).
	for( ChunkCategory rc : pd->referenceCategories )
	{
		const std::vector<String> names =
			CstIntrospection::CandidateNamesForChunkCategory( mJob, rc );
		for( const String& n : names )
			if( n == value ) return true;   // registered runtime-only -> dangling reference
	}
	return false;
}

// -------------------------------------------------------------------
// Phase 4b — per-category property accessors.  Each looks up the
// correct mUi.propertiesByCategory[cat] vector and forwards to the
// matching CameraProperty field.  Bounds-check the category enum and
// the row index; out-of-range returns sensible empty / -1 values
// (matches the single-tuple accessors above).
// -------------------------------------------------------------------

namespace {
inline const std::vector<RISE::CameraProperty>* PropsForCat(
	const std::vector<RISE::CameraProperty>* arr, RISE::SceneEditController::Category cat )
{
	const int i = static_cast<int>( cat );
	// GUI redesign 2026-07-22: this bound went stale TWICE as a mirrored
	// literal (9 pre-SceneVariant, then 11 silently excluding the new
	// Category::Geometry).  kNumCategories is now PUBLIC -- use it, never
	// mirror it.
	if( i < 0 || i >= RISE::SceneEditController::kNumCategories ) return 0;
	return &arr[i];
}
}

unsigned int SceneEditController::PropertyCountFor( Category cat ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return v ? static_cast<unsigned int>( v->size() ) : 0u;
}

String SceneEditController::PropertyNameFor( Category cat, unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].name : String();
}

String SceneEditController::PropertyValueFor( Category cat, unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].value : String();
}

String SceneEditController::PropertyDescriptionFor( Category cat, unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].description : String();
}

int SceneEditController::PropertyKindFor( Category cat, unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return ( v && idx < v->size() ) ? static_cast<int>( (*v)[idx].kind ) : -1;
}

bool SceneEditController::PropertyEditableFor( Category cat, unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].editable : false;
}

unsigned int SceneEditController::PropertyPresetCountFor( Category cat, unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return ( v && idx < v->size() ) ? static_cast<unsigned int>( (*v)[idx].presets.size() ) : 0u;
}

String SceneEditController::PropertyPresetLabelFor( Category cat, unsigned int idx, unsigned int presetIdx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	if( !v || idx >= v->size() ) return String();
	const auto& presets = (*v)[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].label.c_str() );
}

String SceneEditController::PropertyPresetValueFor( Category cat, unsigned int idx, unsigned int presetIdx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	if( !v || idx >= v->size() ) return String();
	const auto& presets = (*v)[idx].presets;
	if( presetIdx >= presets.size() ) return String();
	return String( presets[presetIdx].value.c_str() );
}

String SceneEditController::PropertyUnitLabelFor( Category cat, unsigned int idx ) const
{
	std::lock_guard<std::mutex> sk( mUiSnapshotMutex );
	const auto* v = PropsForCat( mUi.propertiesByCategory, cat );
	return ( v && idx < v->size() ) ? (*v)[idx].unitLabel : String();
}

namespace {

// Convert a UI-provided camera label into one token the CST scene grammar can
// write without quoting.  Camera names are later emitted as `name <value>`;
// whitespace, comments, and braces would otherwise alter the generated chunk
// and leave the live manager and retained Document with different cameras.
// Keep this at the one shared clone/stamp/promote choke point so every
// externally proposed camera name obeys the same serialization contract.
String CanonicalCameraName( const String& proposed )
{
	std::string out;
	for( const char* p = proposed.c_str(); *p; ++p ) {
		const unsigned char c = static_cast<unsigned char>( *p );
		const bool safe = ( c >= 'a' && c <= 'z' ) ||
			( c >= 'A' && c <= 'Z' ) ||
			( c >= '0' && c <= '9' ) || c == '_' || c == '-' || c == '.';
		out += safe ? static_cast<char>( c ) : '_';
	}
	return out.empty() ? String( "camera_copy" ) : String( out.c_str() );
}

// Generate a unique camera name from a canonicalized user-proposed base + the
// existing camera-name set in the scene.  If the base isn't taken, returns it;
// otherwise appends "_2", "_3", ... until an unused name is found.  At most
// 1000 suffix attempts before fallback to a timestamp-style suffix; in
// practice this branch is unreachable since the scene won't accumulate
// thousands of clones.
String UniqueCameraName( const String& proposed, const ICameraManager& cams )
{
	const String base = CanonicalCameraName( proposed );
	const std::string baseText( base.c_str() );
	const size_t kMaxPayloadBytes = 255;   // GUI C ABI output buffers are 256 bytes incl. NUL
	const std::string initial = baseText.substr( 0, kMaxPayloadBytes );
	if( !cams.GetItem( initial.c_str() ) ) return String( initial.c_str() );

	// Reserve suffix bytes BEFORE truncating the base.  Formatting the suffix
	// into a fixed buffer after a 255-byte base would erase the suffix, return
	// the already-occupied original name, and make a second promotion fail.
	for( int i = 2; i < 1000; ++i ) {
		const std::string suffix = "_" + std::to_string( i );
		const std::string candidate =
			baseText.substr( 0, kMaxPayloadBytes - suffix.size() ) + suffix;
		if( !cams.GetItem( candidate.c_str() ) ) return String( candidate.c_str() );
	}
	const std::string suffix = "_" + std::to_string( static_cast<long long>( std::time( 0 ) ) );
	const std::string candidate =
		baseText.substr( 0, kMaxPayloadBytes - suffix.size() ) + suffix;
	return String( candidate.c_str() );
}

}  // namespace

bool SceneEditController::CloneActiveCamera(
	const String& proposedName, char* outName, unsigned int outLen )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( !outName || outLen == 0 ) return false;
	outName[0] = '\0';

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

	IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	ICameraManager* cams = const_cast<ICameraManager*>( scene->GetCameras() );
	if( !cams ) return false;
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
	// A bound pane retains its desired name after deletion.  If a clone
	// recreates that name, wake the pane off its deletion fallback.
	InvalidatePanesBoundToSceneCameraLocked_( finalName );

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

bool SceneEditController::StampViewToNewCamera(
	const String& proposedName, char* outName, unsigned int outLen, unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( !outName || outLen == 0 ) return false;
	outName[0] = '\0';

	// Park before reading mViewportPose + mutating the ICameraManager (same
	// reason CloneActiveCamera parks: the render thread and other UI-thread
	// edits touch these).  The pose fields are the value we stamp.
	std::unique_lock<std::mutex> lk( mMutex );
	// review round 2: the stamp button lives on the nav overlay, which is drawn
	// on the PRIMARY pane -- stamp THAT pane's pose, not whatever pane the
	// scheduler last rotated in.  Resolve + bounds-guard the pane BEFORE the
	// park (the Pane* C-ABI wrappers forward `pane` raw; an out-of-range index
	// would OOB-index mPaneRender/mPaneConfigs) so a pathological call fails
	// closed without needlessly cancelling an in-flight preview pass.
	const unsigned int t = ( pane == kViewportNavPrimary ) ? mPrimaryPane : pane;
	if( t >= PaneCountForLayout( mViewportLayout ) ) return false;
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	ICameraManager* cams = const_cast<ICameraManager*>( scene->GetCameras() );
	if( !cams ) return false;
	// Register-or-slot read (see GetViewportPose); safe under mMutex with the
	// render parked above.
	const bool poseActive = ( mCurrentPane == t ) ? mViewportPoseActive
	                                              : mPaneRender[t].poseActive;
	// Nothing transient to stamp when not free-flying.
	if( !poseActive ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::StampViewToNewCamera: no active free-fly pose to stamp "
			"(navigate the viewport first, then stamp)" );
		return false;
	}
	const CameraSnapshot snapshot = ( mCurrentPane == t ) ? mViewportPose
	                                                      : mPaneRender[t].poseSaved;

	// Pick a unique name under the lock so concurrent adds can't collide.
	const String finalName = UniqueCameraName( proposedName, *cams );

	// Buffer precheck: refuse to mutate the scene if the chosen name won't fit
	// (an after-the-fact check would leave a registered orphan while reporting
	// failure — same rationale as CloneActiveCamera).
	if( finalName.size() > outLen ) {
		outName[0] = '\0';
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::StampViewToNewCamera: outName buffer too small for `%s` (need %u, got %u) — not adding camera",
			finalName.c_str(), static_cast<unsigned>( finalName.size() ), outLen );
		return false;
	}

	SceneEdit edit;
	edit.op             = SceneEdit::AddCamera;
	edit.objectName     = finalName;
	edit.cameraSnapshot = snapshot;

	if( !mEditor.Apply( edit ) ) return false;
	InvalidatePanesBoundToSceneCameraLocked_( finalName );

	{
		const char* s = finalName.c_str();
		const size_t needed = finalName.size();
		for( size_t i = 0; i < needed; ++i ) outName[i] = s[i];
	}

	mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

// ===================== Free-fly viewport pose (Tier 2 §5.3-5.5) =================

bool SceneEditController::EnterFreeFlyFromActiveCamera( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	// Capture the active camera's full pose+optics, then set it as the transient
	// pose (SetViewportPose realizes the override).  The capture is done under the
	// same cancel-and-park CloneActiveCamera uses (the snapshot fields tear under a
	// concurrent orbit-drag write), so read it there rather than here.
	CameraSnapshot snap;
	{
		std::unique_lock<std::mutex> lk( mMutex );
		const IScene* scene = mJob.GetScene();
		if( !scene ) return false;
		const unsigned int t = ( pane == kViewportNavPrimary ) ? mPrimaryPane : pane;
		if( t >= PaneCountForLayout( mViewportLayout ) ) return false;
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		// user-review P1-5: seed the free-fly from the TARGET PANE's currently-
		// displayed view, not unconditionally the scene camera.  A pane showing
		// a NAMED VIEW (or a per-pane free-fly) must snap/home from THAT view --
		// the old `scene->GetCamera()` seed made an axis-snap on a named-view
		// primary jump to the scene camera.  Resolve from the pane's CONFIG so
		// it works even before the pane's vantage has been realized into its
		// registers.  Pane 0 (no config vantage) keeps the scene-camera / active-
		// override seed via PaneEffectiveCameraLocked_.
		bool haveSeed = false;
		if( t != 0 )
		{
			const PaneConfig& pc = mPaneConfigs[t];
			if( pc.vantageKind == PaneVantageKind::NamedView )
			{
				CameraSnapshot live;
				if( FindNamedViewPose( pc.namedViewRef, live ) ) { snap = live; }
				else                                             { snap = pc.pose; }   // deletion fallback
				haveSeed = true;
			}
			else if( pc.vantageKind == PaneVantageKind::SceneCameraNamed )
			{
				CameraSnapshot live;
				if( CaptureNamedSceneCameraSnapshot( *scene, pc.sceneCameraRef, live ) ) snap = live;
				else                                                                     snap = pc.pose;
				haveSeed = true;
			}
			else if( pc.vantageKind == PaneVantageKind::FreeFly )
			{
				snap = pc.pose;
				haveSeed = true;
			}
		}
		if( !haveSeed )
		{
			const ICamera* activeCam = PaneEffectiveCameraLocked_( t, scene );
			if( !activeCam ) return false;
			if( !CameraIntrospection::CaptureCameraSnapshot( *activeCam, snap ) ) {
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditController::EnterFreeFlyFromActiveCamera: active camera kind not realizable "
					"(non-ONB Pinhole / ThinLens / Fisheye / Orthographic only)" );
				return false;
			}
		}
	}
	return SetViewportPose( snap, pane );   // review round 2: forward the target pane
}

bool SceneEditController::SetViewportPose( const CameraSnapshot& pose, unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::mutex> lk( mMutex );
	const IScene* scene = mJob.GetScene();
	if( !scene ) return false;

	// user-review P1#6 (round 2): the nav/free-fly funnel targets the pane the
	// caller names -- the PRIMARY pane for the nav overlay (default sentinel,
	// resolved here under the lock), or an EXPLICIT pane for the pane-N alias
	// forwarders.  Single-viewport mPrimaryPane==0 so the default path is the
	// classic behavior.  Bounds-guard the resolved pane BEFORE the park (the
	// Pane* C-ABI wrappers forward `pane` raw; an out-of-range index would
	// OOB-index mPaneRender/mPaneConfigs) so a pathological call fails closed
	// without needlessly cancelling an in-flight preview pass.
	const unsigned int targetPane = ( pane == kViewportNavPrimary ) ? mPrimaryPane : pane;
	if( targetPane >= PaneCountForLayout( mViewportLayout ) ) return false;
	if( ( mPointerDown.load( std::memory_order_acquire )
	   || mScrubInProgress.load( std::memory_order_acquire ) )
	 && targetPane != mCurrentPane )
	{
		return false;   // gesture exclusivity: never switch live pane registers mid-gesture
	}

	// Cancel-and-park: the render thread reads mViewportOverrideCamera at the top
	// of a pass; we must swap it while no pass is in flight.
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
	if( mCurrentPane != targetPane || mPaneRender[targetPane].configDirty )
		SwitchToPaneLocked_( targetPane );

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
	// user-review P1#6: for a SECONDARY primary pane, mirror the pointer-
	// drag free-fly conversion into the pane's CONFIG so a subsequent
	// configDirty re-realize (e.g. a render-mode change on that pane)
	// reproduces this pose instead of snapping back to the scene camera.
	// Pane 0 has no config vantage (its state is purely register/slot).
	if( targetPane != 0 ) {
		mPaneConfigs[targetPane].vantageKind  = PaneVantageKind::FreeFly;
		mPaneConfigs[targetPane].pose         = pose;
		mPaneConfigs[targetPane].namedViewRef = String();
		mPaneConfigs[targetPane].sceneCameraRef = String();
	}

	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::ExitFreeFly( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::mutex> lk( mMutex );
	// user-review P1#6 (round 2): exit free-fly on the caller's pane --
	// PRIMARY for the nav overlay (default sentinel), explicit for the
	// pane-N alias.  Register-or-slot read, same rule as IsFreeFlyActive.
	const unsigned int targetPane = ( pane == kViewportNavPrimary ) ? mPrimaryPane : pane;
	// user-review (round 2): bounds-guard the RESOLVED pane -- the Pane* C-ABI
	// wrappers forward `pane` raw, so an out-of-range index would OOB-index
	// mPaneRender/mPaneConfigs here.  Fail closed, like every other pane setter.
	if( targetPane >= PaneCountForLayout( mViewportLayout ) ) return false;
	if( ( mPointerDown.load( std::memory_order_acquire )
	   || mScrubInProgress.load( std::memory_order_acquire ) )
	 && targetPane != mCurrentPane )
	{
		return false;   // gesture exclusivity: never switch live pane registers mid-gesture
	}
	{
		const bool active = ( mCurrentPane == targetPane )
			? mViewportPoseActive : mPaneRender[targetPane].poseActive;
		if( !active ) return false;
	}

	// Park before releasing the override the render thread reads.
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
	// user-review P1#6: make the primary pane current before clearing the
	// registers the render thread reads.
	if( mCurrentPane != targetPane || mPaneRender[targetPane].configDirty )
		SwitchToPaneLocked_( targetPane );

	if( mViewportOverrideCamera ) {
		mViewportOverrideCamera->release();
		mViewportOverrideCamera = nullptr;
	}
	mViewportPoseActive = false;
	// user-review P1#6: a SECONDARY primary pane goes back to TRACKING the
	// scene camera (exactly SetPaneVantageSceneCamera's config), so a later
	// configDirty re-realize reproduces the scene camera rather than the
	// now-cleared free-fly pose.  Pane 0 keeps its register-only semantics.
	if( targetPane != 0 ) {
		mPaneConfigs[targetPane].vantageKind  = PaneVantageKind::SceneCamera;
		mPaneConfigs[targetPane].pose         = CameraSnapshot();
		mPaneConfigs[targetPane].namedViewRef = String();
		mPaneConfigs[targetPane].sceneCameraRef = String();
	}

	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::IsFreeFlyActive( unsigned int pane ) const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	// user-review P1#6 (round 2): the caller's pane -- PRIMARY for the nav
	// overlay (default sentinel), explicit for the pane-N alias.  The state
	// lives in the REGISTERS only while that pane is scheduled; otherwise the
	// last context switch saved it to that pane's slot.  Read the live one.
	const unsigned int t = ( pane == kViewportNavPrimary ) ? mPrimaryPane : pane;
	// user-review (round 2): bounds-guard the RESOLVED pane -- the Pane* C-ABI
	// wrappers forward `pane` raw, so an out-of-range index would OOB-index
	// mPaneRender/mPaneConfigs here.  Fail closed, like every other pane setter.
	if( t >= kViewportPaneCount ) return false;
	return ( mCurrentPane == t ) ? mViewportPoseActive : mPaneRender[t].poseActive;
}

bool SceneEditController::GetViewportPose( CameraSnapshot& out, unsigned int pane ) const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	// user-review P1#6 (round 2): caller's pane register-or-slot read (see
	// IsFreeFlyActive), PRIMARY by default, explicit for the pane-N alias.
	const unsigned int t = ( pane == kViewportNavPrimary ) ? mPrimaryPane : pane;
	// user-review (round 2): bounds-guard the RESOLVED pane -- the Pane* C-ABI
	// wrappers forward `pane` raw, so an out-of-range index would OOB-index
	// mPaneRender/mPaneConfigs here.  Fail closed, like every other pane setter.
	if( t >= kViewportPaneCount ) return false;
	const bool active = ( mCurrentPane == t ) ? mViewportPoseActive : mPaneRender[t].poseActive;
	if( !active ) return false;
	out = ( mCurrentPane == t ) ? mViewportPose : mPaneRender[t].poseSaved;
	return true;
}

// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): see mPreviewScalePinned's
// header doc.  Every gesture-driven mPreviewScale mutation site (OnPointerDown/
// Move/Up, OnTimeScrubBegin/End, Begin/EndPropertyScrub) and the render loop's
// own idle-refinement/during-motion adaptation route through this instead of a
// bare `mPreviewScale.store(...)`.
void SceneEditController::SetPreviewScaleIfUnpinned_( unsigned int v )
{
	if( mPreviewScalePinned.load( std::memory_order_acquire ) ) return;
	mPreviewScale.store( v, std::memory_order_release );
}

// ===================== Viewport render modes (P1, docs/gui/RENDER_MODES.md §5) =====

// See the header doc.  Callers must already hold mMutex with the render
// parked, and must have already checked `mode` is a valid casterFactory
// mode.
bool SceneEditController::InstallViewModeCaster_( Implementation::ViewportRenderMode mode )
{
	IRayCaster* caster = 0;
	if( !Implementation::CreateInteractiveViewModeCaster( mode, &caster ) )
	{
		// Shouldn't happen for a viewportSelectable, non-Preview mode --
		// the factory covers exactly {Normals,Depth,Facets,Wireframe} --
		// but fail closed rather than install nothing silently.
		return false;
	}
	const Implementation::ViewportRenderModeInfo* info =
		Implementation::FindViewportRenderModeInfo( mode );
	mInteractiveImpl->SetViewModeCaster( caster, info && info->wantsDenoise );
	// review-r1 P2: record the caster in SLOT 0 (keeping our addref)
	// instead of dropping the local ref -- pane 0's setter and the
	// scheduler's reconcile must share ONE source of truth, or the first
	// rotation away and back sees realizedMode != wantMode and rebuilds,
	// discarding depth's live auto-window calibration (the exact invariant
	// the per-pane caster slots exist to protect).
	if( mPaneRender[0].viewModeCaster ) { mPaneRender[0].viewModeCaster->release(); }
	mPaneRender[0].viewModeCaster = caster;   // ownership: our addref moves to the slot
	mPaneRender[0].realizedMode   = mode;
	return true;
}

bool SceneEditController::SetViewportRenderMode( const char* name )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( !mInteractiveImpl ) return false;   // skeleton mode -- no interactive rasterizer wired up

	const Implementation::ViewportRenderModeInfo* info =
		Implementation::FindViewportRenderModeByName( name );
	if( !info || !info->viewportSelectable )
	{
		// Unknown name, OR a registered-but-not-selectable mode (today:
		// only "objectmap" -- its own palette-lifecycle pipeline, not a
		// plain caster swap; see docs/gui/RENDER_MODES.md §4).
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::SetViewportRenderMode: unknown or non-selectable mode name \"%s\"",
			name ? name : "(null)" );
		return false;
	}

	// Cancel-and-park: the render thread's DoOneRenderPass reads pCaster
	// (via mInteractiveImpl) at RasterizeScene time; we must swap it while
	// no pass is in flight.  Read mViewportRenderMode under the SAME lock
	// that guards every OTHER mutation of it (SetViewportRenderMode /
	// RebindEditorToJob) -- no unsynchronized read of the member, mirroring
	// SetViewportPose's mViewportOverrideCamera contract above.
	std::unique_lock<std::mutex> lk( mMutex );
	if( ( mPointerDown.load( std::memory_order_acquire )
	   || mScrubInProgress.load( std::memory_order_acquire ) )
	 && mCurrentPane != 0 )
	{
		// Pane-0 mode changes force pane 0 into the live register set.  Doing
		// that during a secondary-pane gesture would make the next Move edit
		// pane 0's camera/scale while recording the result into the gestured
		// pane's config.  Refuse until release instead of breaking exclusivity.
		return false;
	}
	if( info->mode == mViewportRenderMode )
	{
		return true;   // no-op -- already the active mode
	}
	CancelAndParkRender_( lk );
	// P3a slice 2: pane 0's mode/caster/variant registers are the target --
	// make pane 0 current first (no-op when it already is).
	if( mCurrentPane != 0 ) SwitchToPaneLocked_( 0 );

	// External review P2 fix: retire any queued/in-flight polish pass
	// BEFORE swapping the caster -- otherwise a polish pass queued for
	// the OLD mode (mPolishState == PolishQueued) survives the switch,
	// and the render loop's NEXT pass runs a 4-SPP Balanced-denoise
	// "polish" of the NEW mode/x-ray state, then completes the state
	// machine against a pass it never configured for.  Same retirement
	// idiom KickRender / OnTimeScrub / the object-motion edit path use
	// (store PolishState::None) -- see those sites -- plus an explicit
	// SetSampleCount(1) here (mirroring the render loop's own post-
	// polish restore) so the sticky multi-SPP kernel/polish-caster
	// state on mInteractiveImpl doesn't linger into the caster swap
	// below.  Both run while we still hold `lk`, so this is atomic
	// with the mode switch itself -- no window for the render loop to
	// observe a stale PolishQueued for this mode.
	mPolishState.store( static_cast<int>( PolishState::None ), std::memory_order_release );
	mInteractiveImpl->SetSampleCount( 1 );

	// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): a BeautyVariant
	// target (deep_reflect/direct) is a SEPARATE ephemeral pipeline the
	// render loop drives instead of mInteractiveRasterizer -- NOT a caster
	// swap on it.  mInteractiveImpl's own caster is deliberately left
	// UNTOUCHED while a variant mode is active (still whatever Preview/data-
	// mode caster was installed before we entered it), so switching AWAY
	// from a variant mode later finds it exactly as it was.
	if( Implementation::IsBeautyVariantMode( info->mode ) )
	{
		// review-p3 P2-c: recover the PRODUCTION rasterizer's actual
		// configured default shader instead of letting
		// CreateBeautyVariantPipeline fall back to its own generic internal
		// DefaultPathTracing default -- see AgentSession.cpp's twin of this
		// fix for the full rationale.  Sound, not a guess:
		// GetRasterizerParameter's "shader" case reads back the exact
		// resolved shader NAME every Set*Rasterizer call stamped into its
		// own registry snapshot at construction time.  Falls back to null
		// when no rasterizer is active yet or the resolved name doesn't
		// exist in the shader manager.
		// (Extracted to BuildVariantRasterizer_ for P3a slice 2 -- the
		// per-pane scheduler's lazy build shares this exact implementation,
		// including the production-default-shader recovery.)
		IRasterizer* variantRast = nullptr;
		if( !BuildVariantRasterizer_( info->mode, &variantRast ) )
		{
			// lk unlocks; render thread was parked but nothing was
			// mutated, so no repaint kick is needed.
			return false;
		}
		// Fail-closed order: the NEW pipeline built successfully above
		// BEFORE we release any PRIOR one, so a failed build (handled
		// above) never leaves the controller without its old variant
		// pipeline mid-switch.
		if( mVariantRasterizer )
		{
			mVariantRasterizer->release();
		}
		mVariantRasterizer = variantRast;   // ownership transferred
		// Pin the preview-scale divisor to the mode's fixed resolution --
		// see mPreviewScalePinned's header doc for why every other
		// mPreviewScale mutation site respects this flag.
		mPreviewScale.store(
			info->variantScaleDivisor > 0 ? info->variantScaleDivisor : 1,
			std::memory_order_release );
		mPreviewScalePinned.store( true, std::memory_order_release );
	}
	else
	{
		// Leaving a variant mode (if one was active): release the ephemeral
		// pipeline and unpin the resolution ladder so it resumes its normal
		// adaptive behaviour.
		if( mVariantRasterizer )
		{
			mVariantRasterizer->release();
			mVariantRasterizer = 0;
			// P2 review fix: restore mPreviewScale itself, not just the pin
			// -- previously only mPreviewScalePinned was cleared here, so a
			// variant mode's pinned divisor (e.g. quarter-res for
			// deep_reflect) stayed in mPreviewScale after leaving the mode,
			// and the viewport rendered at that stale resolution until a
			// pointer gesture (OnPointerDown/Move/Up etc.) happened to
			// overwrite it.  Gated on "a variant WAS active" so a plain
			// non-variant-to-non-variant switch (e.g. normals -> depth)
			// doesn't disturb whatever adaptive scale a live gesture
			// already set.
			mPreviewScale.store( kPreviewScaleMin, std::memory_order_release );
		}
		mPreviewScalePinned.store( false, std::memory_order_release );

		if( info->mode != Implementation::ViewportRenderMode::Preview )
		{
			if( !InstallViewModeCaster_( info->mode ) )
			{
				// lk unlocks; render thread was parked but nothing was
				// mutated, so no repaint kick is needed.
				return false;
			}
		}
		else
		{
			// nullptr for Preview -- SetViewModeCaster restores the platform-
			// installed preview/polish casters (see its own doc).
			mInteractiveImpl->SetViewModeCaster( nullptr );
			// review-r1 P2: slot-0 bookkeeping for the Preview branch too --
			// release the held data-mode caster (a DIRECT mode change resets
			// calibration by design; only ROTATION preserves it) and record
			// Preview so the scheduler's reconcile agrees with reality.
			if( mPaneRender[0].viewModeCaster ) {
				mPaneRender[0].viewModeCaster->release();
				mPaneRender[0].viewModeCaster = nullptr;
			}
			mPaneRender[0].realizedMode = Implementation::ViewportRenderMode {};
		}
	}
	mViewportRenderMode = info->mode;
	// review-r1 P2: the variant branch above never touches slot-0's caster
	// (contract: leaving a variant later finds the data caster as it was)
	// but realizedMode must reflect what is ACTUALLY realized, or the
	// scheduler reconciles against a stale answer.  For variants the
	// authoritative realization is the register mVariantRasterizer +
	// this record.
	if( Implementation::IsBeautyVariantMode( info->mode ) ) {
		mPaneRender[0].realizedMode = info->mode;
	}

	// Kick a repaint -- same inline store-then-notify SetViewportPose uses
	// (KickRender() takes mMutex itself, so it can't be called while we
	// still hold `lk`).
	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

const char* SceneEditController::GetViewportRenderMode() const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return "preview";  // render-owns-scene guard
	Implementation::ViewportRenderMode mode;
	{
		// The ownership flag is deliberately a fast path, not a lock hand-off:
		// an agent worker can acquire mMutex immediately before publishing it.
		// UI readers must therefore decline a busy controller instead of waiting
		// through the whole production render in that narrow window.
		std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
		if( !lk.owns_lock() ) return "preview";
		mode = mViewportRenderMode;
	}
	const Implementation::ViewportRenderModeInfo* info =
		Implementation::FindViewportRenderModeInfo( mode );
	return info ? info->name : "preview";
}

// ===================== X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis") ======

bool SceneEditController::SetViewportXray( bool xray )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( !mInteractiveImpl ) return false;   // skeleton mode -- no interactive rasterizer wired up

	// Same lock/park discipline as SetViewportRenderMode above.
	std::unique_lock<std::mutex> lk( mMutex );
	if( xray == mViewportXray )
	{
		return true;   // no-op -- already the current flag
	}
	CancelAndParkRender_( lk );

	// External review P2 fix: same stale-PolishQueued retirement as
	// SetViewportRenderMode above -- an x-ray toggle mid-polish must
	// not let the queued polish pass survive to run (at the old x-ray
	// state) against a state machine that no longer expects it.  See
	// that function's comment for the full rationale; both stores run
	// under this SAME `lk` hold, atomic with the flag flip below.
	mPolishState.store( static_cast<int>( PolishState::None ), std::memory_order_release );
	mInteractiveImpl->SetSampleCount( 1 );

	// X-ray resolution lives in the caster layer now -- no caster rebuild,
	// just stamp the flag onto every caster the rasterizer holds (applies
	// uniformly across every mode, INCLUDING Preview).
	mInteractiveImpl->SetXrayView( xray );
	mViewportXray = xray;

	// Kick a repaint -- same inline store-then-notify SetViewportRenderMode
	// uses.
	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::GetViewportXray() const
{
	// render-owns-scene guard -- report the DEFAULT (x-ray is default-ON;
	// see the header doc), not a stale false, while a production/agent
	// render transiently owns the scene.
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return true;
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return true;
	return mViewportXray;
}

// ===================== Axis snaps + Home (Tier 2 §4.2) =========================

bool SceneEditController::SnapViewToAxis( int axis, bool negative, unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( axis < 0 || axis > 2 ) return false;

	// Source pose: the target pane's current free-fly pose, or (auto-enter) its
	// currently-displayed view.  `pane` defaults to the sentinel -> the PRIMARY
	// pane (what the nav overlay is drawn on); the pane-0 C-ABI alias passes 0.
	CameraSnapshot src;
	if( IsFreeFlyActive( pane ) ) {
		if( !GetViewportPose( src, pane ) ) return false;
	} else {
		if( !EnterFreeFlyFromActiveCamera( pane ) ) return false;
		if( !GetViewportPose( src, pane ) ) return false;   // now seeded from the pane's view
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

	return SetViewportPose( snap, pane );
}

bool SceneEditController::SetHomeView( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::mutex> lk( mMutex );
	const IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	const unsigned int t = ( pane == kViewportNavPrimary ) ? mPrimaryPane : pane;
	if( t >= PaneCountForLayout( mViewportLayout ) ) return false;

	CameraSnapshot cur;
	const bool active = ( t == mCurrentPane ) ? mViewportPoseActive : mPaneRender[t].poseActive;
	if( active ) {
		cur = ( t == mCurrentPane ) ? mViewportPose : mPaneRender[t].poseSaved;
	}
	else {
		// Capture the target pane's displayed view only after the pass parks.
		// Resolve a configured named/free-fly secondary directly so Home works
		// before its first scheduled realization.
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		bool havePose = false;
		if( t != 0 ) {
			const PaneConfig& pc = mPaneConfigs[t];
			if( pc.vantageKind == PaneVantageKind::NamedView ) {
				CameraSnapshot live;
				if( FindNamedViewPose( pc.namedViewRef, live ) ) cur = live;
				else                                             cur = pc.pose;
				havePose = true;
			}
			else if( pc.vantageKind == PaneVantageKind::SceneCameraNamed ) {
				CameraSnapshot live;
				if( CaptureNamedSceneCameraSnapshot( *scene, pc.sceneCameraRef, live ) ) cur = live;
				else                                                                     cur = pc.pose;
				havePose = true;
			}
			else if( pc.vantageKind == PaneVantageKind::FreeFly ) {
				cur = pc.pose;
				havePose = true;
			}
		}
		if( !havePose ) {
			const ICamera* cam = PaneEffectiveCameraLocked_( t, scene );
			if( !cam || !CameraIntrospection::CaptureCameraSnapshot( *cam, cur ) ) return false;
		}
	}
	mHomeView = cur;
	mHasHomeView = true;
	return true;
}

bool SceneEditController::GoToHomeView( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	CameraSnapshot home;
	{
		std::lock_guard<std::mutex> lk( mMutex );
		if( !mHasHomeView ) return false;
		home = mHomeView;
	}
	return SetViewportPose( home, pane );   // takes mMutex itself; applies to the target pane
}

bool SceneEditController::HasHomeView() const
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	return mHasHomeView;
}

// ===================== Named Views (Tier 2 §3, + P2a `render{view:}`) ==========
//
// Session/UI bookmarks.  Originally UI-thread-only (never read by the render
// thread), so mNamedViews needed no lock of its own.  GUI render modes P2a's
// `render{view:}` surface (FindNamedViewPose, below) added the FIRST reader
// reachable from a non-UI thread -- the agent-render worker
// (SubmitAgentRenderAsync's dedicated thread) -- so mNamedViewsMutex now
// guards every touch of the vector.  Capturing the CURRENT view separately
// needs the cancel-and-park when it reads the active camera's fields (they
// tear under a concurrent orbit-drag), same as SetHomeView / CloneActiveCamera
// -- that capture happens OUTSIDE mNamedViewsMutex (CaptureCurrentView takes
// mMutex itself), so no two locks are ever held at once here.

bool SceneEditController::CaptureCurrentView( CameraSnapshot& out )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	// Free-fly active: the transient pose IS the current view (GetViewportPose
	// locks internally).  Otherwise capture the active scene camera under the
	// cancel-and-park (its fields tear under a concurrent orbit-drag write —
	// same reason SetHomeView / CloneActiveCamera park).
	// user-review (round 2): the named-view CAPTURE surface is global and
	// predates N-up -- keep it on PANE 0 (the legacy viewport alias), matching
	// the §7.4 "unindexed = pane 0" contract, rather than silently retargeting
	// the primary pane.
	if( IsFreeFlyActive( 0 ) )
		return GetViewportPose( out, 0 );

	std::unique_lock<std::mutex> lk( mMutex );
	const IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
	const ICamera* cam = scene->GetCamera();
	if( !cam ) return false;
	return CameraIntrospection::CaptureCameraSnapshot( *cam, out );
}

bool SceneEditController::CaptureNamedView( const String& name )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	// Both platform bridges enumerate names through a 256-byte caller-owned
	// buffer.  Rejecting an overlong name here keeps count and list positions
	// identical, rather than creating an invisible slot whose later actions
	// would target the wrong view.
	if( name.size() > 256 ) return false;   // RString size() includes the NUL
	CameraSnapshot snap;
	if( !CaptureCurrentView( snap ) ) return false;
	NamedView v;
	v.name = name;
	v.pose = snap;
	std::lock_guard<std::mutex> lk( mNamedViewsMutex );
	mNamedViews.push_back( v );
	return true;
}

unsigned int SceneEditController::NamedViewCount() const
{
	std::lock_guard<std::mutex> lk( mNamedViewsMutex );
	return static_cast<unsigned int>( mNamedViews.size() );
}

bool SceneEditController::NamedViewName( unsigned int idx, char* out, unsigned int outLen ) const
{
	if( !out || outLen == 0 ) return false;
	out[0] = '\0';
	std::lock_guard<std::mutex> lk( mNamedViewsMutex );
	if( idx >= mNamedViews.size() ) return false;
	const String& nm = mNamedViews[ idx ].name;
	if( nm.size() > outLen ) return false;   // RString size() includes the NUL
	const char* s = nm.c_str();
	const size_t n = nm.size();
	for( size_t i = 0; i < n; ++i ) out[i] = s[i];
	return true;
}

bool SceneEditController::RestoreNamedView( unsigned int idx )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	CameraSnapshot pose;
	{
		std::lock_guard<std::mutex> lk( mNamedViewsMutex );
		if( idx >= mNamedViews.size() ) return false;
		pose = mNamedViews[ idx ].pose;
	}
	return SetViewportPose( pose, 0 );   // user-review (round 2): pane-0 alias (global named-view restore)
}

//! review-r2 P1: a pane bound to a named view must FOLLOW the view.  The
//! §7.2 propagation is event-driven: Update/DeleteNamedView call this to
//! mark every bound pane for reconcile (configDirty releases the realized
//! camera at the next switch; the deletion case then falls back to the
//! config's snapshot pose via the existing FindNamedViewPose-miss path).
//!
//! LOCK ORDER (user-review P2-B correction): the ESTABLISHED order is
//! mMutex -> mNamedViewsMutex, NEVER the reverse.  `FindNamedViewPose`
//! (which takes mNamedViewsMutex) is legitimately called WHILE holding
//! mMutex at several sites (SwitchToPaneLocked_, OnPanePointerDown, and the
//! named-view seed in EnterFreeFlyFromActiveCamera / SetHomeView) -- so the
//! two ARE nested,
//! one-directionally, and that is safe.  This function takes ONLY mMutex, so
//! the CALLER must NOT already hold mNamedViewsMutex (both named-view
//! mutators release it before calling here) -- taking mNamedViewsMutex then
//! mMutex is the forbidden reverse order that would deadlock.
void SceneEditController::InvalidatePanesBoundToNamedView_( const String& name )
{
	std::lock_guard<std::mutex> lk( mMutex );
	bool any = false;
	for( unsigned int i = 1; i < kViewportPaneCount; ++i )
	{
		if( mPaneConfigs[i].vantageKind == PaneVantageKind::NamedView
		 && mPaneConfigs[i].namedViewRef == name.c_str() )
		{
			mPaneRender[i].configDirty = true;
			mPaneRender[i].dirty       = true;
			any = true;
		}
	}
	if( any )
	{
		mPanePassPending.store( true, std::memory_order_release );
		mCV.notify_one();
	}
}

void SceneEditController::InvalidatePanesBoundToSceneCameraLocked_(
	const String& name )
{
	if( name.size() <= 1 ) return;
	const unsigned int visible = PaneCountForLayout( mViewportLayout );
	bool visibleMatch = false;
	for( unsigned int i = 1; i < kViewportPaneCount; ++i )
	{
		if( mPaneConfigs[i].vantageKind == PaneVantageKind::SceneCameraNamed
		 && mPaneConfigs[i].sceneCameraRef == name.c_str() )
		{
			mPaneRender[i].configDirty = true;
			mPaneRender[i].dirty       = true;
			visibleMatch = visibleMatch || i < visible;
		}
	}
	if( visibleMatch )
	{
		mPanePassPending.store( true, std::memory_order_release );
	}
}

void SceneEditController::InvalidateAllSceneCameraNamedPanesLocked_()
{
	const unsigned int visible = PaneCountForLayout( mViewportLayout );
	bool visibleMatch = false;
	for( unsigned int i = 1; i < kViewportPaneCount; ++i )
	{
		if( mPaneConfigs[i].vantageKind == PaneVantageKind::SceneCameraNamed )
		{
			mPaneRender[i].configDirty = true;
			mPaneRender[i].dirty       = true;
			visibleMatch = visibleMatch || i < visible;
		}
	}
	if( visibleMatch )
	{
		mPanePassPending.store( true, std::memory_order_release );
	}
}

bool SceneEditController::UpdateNamedView( unsigned int idx )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	String targetName;
	{
		std::lock_guard<std::mutex> lk( mNamedViewsMutex );
		if( idx >= mNamedViews.size() ) return false;
		// review-r2-A P1: capture the STABLE IDENTITY (the name) here.  The
		// old re-check after the park only caught vector SHRINKAGE -- a
		// concurrent delete BEFORE idx shifts the vector so mNamedViews[idx]
		// names a DIFFERENT view while idx < size() still holds, and the
		// pose landed on the wrong view (and, post-round-1, the pane
		// propagation invalidated panes bound to the wrong view too).
		targetName = mNamedViews[ idx ].name;
	}
	CameraSnapshot snap;
	if( !CaptureCurrentView( snap ) ) return false;
	String updatedName;
	{
		std::lock_guard<std::mutex> lk( mNamedViewsMutex );
		// review-r3 P1: identity = POSITION AND NAME, not name-only.
		// Named views have no uniqueness constraint (CaptureNamedView
		// push_backs unconditionally), so a name-only first-match scan
		// updated the FIRST same-named view -- wrong-view corruption in a
		// plain single-threaded call, worse than the race it replaced.
		// The positional check with the name as a tamper-witness gives
		// every guarantee the round-2 fix claimed: a shrink makes idx OOB
		// or the name mismatch (clean false); a shift makes the name
		// mismatch (clean false); duplicates update EXACTLY the entry the
		// caller indexed.
		// (review-r4 residual, documented not fixed: a shift where a
		// SAME-NAMED neighbour slides into idx would pass the witness --
		// unreachable today because name-mutating calls are single-writer
		// UI-thread-only; revisit if that invariant ever changes.)
		if( idx >= mNamedViews.size() || !( mNamedViews[idx].name == targetName.c_str() ) )
		{
			return false;
		}
		mNamedViews[idx].pose = snap;
		updatedName = targetName;
	}
	// review-r2 P1: panes bound to this view must follow it.  Called AFTER
	// releasing mNamedViewsMutex (lock order -- see the helper's doc).
	InvalidatePanesBoundToNamedView_( updatedName );
	return true;
}

bool SceneEditController::DeleteNamedView( unsigned int idx )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	String deletedName;
	{
		std::lock_guard<std::mutex> lk( mNamedViewsMutex );
		if( idx >= mNamedViews.size() ) return false;
		deletedName = mNamedViews[ idx ].name;
		mNamedViews.erase( mNamedViews.begin() + idx );
	}
	// review-r2 P1: a bound pane's next reconcile finds the name gone and
	// falls back to the config's snapshot pose (the existing
	// FindNamedViewPose-miss path in SwitchToPaneLocked_) -- but only if
	// something MARKS it for reconcile.  Same lock-order rule as Update.
	InvalidatePanesBoundToNamedView_( deletedName );
	return true;
}

bool SceneEditController::FindNamedViewPose( const String& name, CameraSnapshot& outPose ) const
{
	std::lock_guard<std::mutex> lk( mNamedViewsMutex );
	for( std::size_t i = 0; i < mNamedViews.size(); ++i ) {
		if( mNamedViews[i].name == name.c_str() ) {
			outPose = mNamedViews[i].pose;
			return true;
		}
	}
	return false;
}

// ======================================================================
// N-up multi-viewport pane model (RENDER_MODES.md §7, P3a slice 1).
//
// DESIRED-STATE layer: pane 0 aliases the existing single-viewport state;
// panes 1-3 store validated configuration that SwitchToPaneLocked_'s
// reconcile realizes at the next rotation switch.  Setters follow the
// house discipline (render-owns-scene guard, then mMutex; no park needed
// for panes 1-3 because the render thread reads mPaneConfigs only inside
// the mint lock -- pane 0 setters forward to existing methods that park).
// ======================================================================

// PaneConfig::mode value-initializes to enumerator 0 in the header (the enum
// is forward-declared there); pin the assumption so a registry reorder is a
// compile error, not a silent default-mode change.
static_assert( static_cast<int>( Implementation::ViewportRenderMode::Preview ) == 0,
	"PaneConfig::mode's value-init default assumes Preview == 0" );

unsigned int SceneEditController::PaneCountForLayout( ViewportLayout layout )
{
	switch( layout )
	{
	case ViewportLayout::Single:     return 1;
	case ViewportLayout::TwoH:       return 2;
	case ViewportLayout::OnePlusTwo: return 3;
	case ViewportLayout::Quad:       return 4;
	}
	return 1;   // unreachable; fail-closed to today's single viewport
}

bool SceneEditController::SetViewportLayout( ViewportLayout layout )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	switch( layout )
	{
	case ViewportLayout::Single: case ViewportLayout::TwoH:
	case ViewportLayout::OnePlusTwo: case ViewportLayout::Quad:
		break;
	default:
		return false;   // fail-closed on an out-of-range int cast
	}
	std::unique_lock<std::mutex> lk( mMutex );
	if( layout == mViewportLayout ) return true;   // no-op contract
	const unsigned int oldVisible = PaneCountForLayout( mViewportLayout );
	mViewportLayout = layout;
	const unsigned int newVisible = PaneCountForLayout( layout );
	// §7.2: a shrink that hides the primary falls back to pane 0.
	if( mPrimaryPane >= newVisible ) {
		mPrimaryPane = 0;
	}
	// Publish the coherent pair for paint/timer readers that must never wait
	// behind an agent/production render's whole-duration mMutex hold.  Primary
	// is stored first so a shrink can never be observed as a new small layout
	// paired with its now-hidden old primary.
	mPrimaryPaneSnapshot.store( mPrimaryPane, std::memory_order_release );
	mViewportLayoutSnapshot.store( static_cast<int>( mViewportLayout ),
	                              std::memory_order_release );
	// review-r2 P1: a GROW must wake the render loop -- without this,
	// newly-revealed panes (dirty since construction or since being
	// hidden) stay blank until an unrelated edit happens to wake it
	// (the loop sleeps on a predicate that only sees mEditPending /
	// mPanePassPending).  §7.3 invalidation matrix row 3.
	if( newVisible > oldVisible )
	{
		for( unsigned int i = oldVisible; i < newVisible; ++i ) {
			mPaneRender[i].dirty = true;
		}
		mPanePassPending.store( true, std::memory_order_release );
		mCV.notify_one();
	}
	// user-review P2#1: a SHRINK that hides the CURRENTLY-scheduled pane (or
	// pins a gesture to a now-invisible pane) must not leave it rendering --
	// its pixels would be published as "the current frame" (and a gesture
	// would keep driving an off-screen pane).  Park the in-flight pass, drop
	// an orphaned gesture, switch the scheduler to the (visible) primary, and
	// mark that primary dirty so it repaints.
	else if( newVisible < oldVisible )
	{
		const bool pointerGestureActive =
			mPointerDown.load( std::memory_order_acquire );
		const bool propertyScrubActive =
			mScrubInProgress.load( std::memory_order_acquire );
		const bool currentHidden = ( mCurrentPane >= newVisible );
		// mGesturePane belongs only to pointer gestures.  A property scrub
		// has no pane token and pins the currently scheduled pane; consulting a
		// stale pointer gesture's pane here spuriously terminated an unrelated
		// later scrub when a layout shrink hid that old pane.
		const bool gestureHidden =
			pointerGestureActive && ( mGesturePane >= newVisible );
		if( currentHidden || gestureHidden )
		{
			// Park FIRST so the gesture finalization + the switch below run
			// with no pass in flight.
			if( mRendering.load( std::memory_order_acquire ) ) {
				mCancelProgress.RequestCancel();
				mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
			}
			mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
			if( gestureHidden ) {
				// user-review P1-2: the gesture's pane is gone -- FINALIZE the
				// interrupted gesture EXACTLY as OnPointerUp would.  The first
				// cut only cleared mPointerDown, which made the eventual
				// pointer-up early-return (`!mPointerDown`), STRANDING the open
				// composite (poisons IsCompositeOpen + every future transaction,
				// grows history unbounded) and leaving the drag's live-only
				// transforms unrecorded in the CST.  We're parked -- the exact
				// precondition OnPointerUp's commit block requires.  COMMIT (not
				// roll back): the drag's live mutations already happened;
				// recording them keeps the scene + its undo history consistent.
				if( mGestureOpenedComposite ) {
					mEditor.EndComposite();
					mGestureOpenedComposite = false;
				}
					if( mEditor.HasPendingCstObjectTransforms() || mEditor.HasPendingCstCameraPose() ) {
						mEditor.CommitPendingCstObjectTransforms();
						mEditor.CommitPendingCstCameraPose();
						// Commit can re-derive the live scene.  Route it through the
						// same all-visible-pane invalidation path as an ordinary
						// pointer-up, not merely the new primary's one-pane repaint.
						mEditPending.store( true, std::memory_order_release );
						mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
					}
					mGizmoDrag.active = false;
					mPointerDown.store( false, std::memory_order_release );
					if( !propertyScrubActive )
					{
						SetPreviewScaleIfUnpinned_( kPreviewScaleMin );
					}
					mPolishState.store( static_cast<int>( PolishState::None ),
					                    std::memory_order_release );
			}
			if( mCurrentPane >= newVisible ) {
				SwitchToPaneLocked_( mPrimaryPane );
			}
			if( propertyScrubActive )
			{
				// The pointer gesture ended because its pane disappeared, but
				// an independent property scrub remains live.  Re-establish its
				// motion divisor on the newly loaded visible-pane registers;
				// otherwise the next scrub tick runs an accidental full-res pass.
				SetPreviewScaleIfUnpinned_( kPreviewScaleMotionStart );
			}
			mPaneRender[mPrimaryPane].dirty = true;
			mPanePassPending.store( true, std::memory_order_release );
			mCV.notify_one();
		}
	}
	return true;
}

SceneEditController::ViewportLayout SceneEditController::GetViewportLayout() const
{
	const ViewportLayout layout = static_cast<ViewportLayout>(
		mViewportLayoutSnapshot.load( std::memory_order_acquire ) );
	switch( layout )
	{
	case ViewportLayout::Single: case ViewportLayout::TwoH:
	case ViewportLayout::OnePlusTwo: case ViewportLayout::Quad:
		return layout;
	}
	return ViewportLayout::Single;   // fail closed on corrupted external state
}

// user-review P1-3: fills the pane-set snapshot reading every field DIRECTLY
// (no getters, no lock) -- the caller (AgentSession::ReadViewport, inside its
// RunPreviewRenderParked closure) already holds mMutex with the render parked,
// so this is atomic with the frame copied in the same closure.  The register-
// or-slot reads mirror IsFreeFlyActive / GetPaneVantage exactly.
void SceneEditController::SnapshotPaneSetForParkedRead( PaneSetSnapshot& out ) const
{
	out.layout  = static_cast<int>( mViewportLayout );
	out.primary = mPrimaryPane;
	const unsigned int visible = PaneCountForLayout( mViewportLayout );
	for( unsigned int i = 0; i < kViewportPaneCount; ++i )
	{
		out.panes[i].visible = ( i < visible );
		out.panes[i].mode    = ( i == 0 ) ? mViewportRenderMode : mPaneConfigs[i].mode;
		if( i == 0 )
		{
			// Pane 0's vantage lives in the registers (or slot 0 when a
			// secondary is scheduled) -- same rule as GetPaneVantage(0).
			const bool freeFly = ( mCurrentPane == 0 ) ? mViewportPoseActive
			                                            : mPaneRender[0].poseActive;
			out.panes[i].vantageKind = freeFly ? PaneVantageKind::FreeFly
			                                   : PaneVantageKind::SceneCamera;
			out.panes[i].namedView   = String();
		}
		else
		{
			out.panes[i].vantageKind = mPaneConfigs[i].vantageKind;
			out.panes[i].namedView =
				( mPaneConfigs[i].vantageKind == PaneVantageKind::SceneCameraNamed )
				? mPaneConfigs[i].sceneCameraRef : mPaneConfigs[i].namedViewRef;
		}
	}
}

bool SceneEditController::SetPrimaryPane( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	std::lock_guard<std::mutex> lk( mMutex );
	if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;   // invalid or hidden
	mPrimaryPane = pane;
	mPrimaryPaneSnapshot.store( pane, std::memory_order_release );
	return true;
}

unsigned int SceneEditController::GetPrimaryPane() const
{
	const unsigned int pane = mPrimaryPaneSnapshot.load( std::memory_order_acquire );
	return pane < kViewportPaneCount ? pane : 0;
}

bool SceneEditController::SetPaneRenderMode( unsigned int pane, const char* name )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( !name ) return false;
	if( pane == 0 ) {
		// Alias contract: pane 0 IS the existing viewport.  Forward so the
		// caster swap / variant-rasterizer lifecycle / park discipline all
		// run exactly as today (including that method's own guards).
		return SetViewportRenderMode( name );
	}
	const Implementation::ViewportRenderModeInfo* info =
		Implementation::FindViewportRenderModeByName( name );
	if( !info || !info->viewportSelectable ) return false;
	std::lock_guard<std::mutex> lk( mMutex );
	if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;   // hidden panes refuse (§7.4)
	mPaneConfigs[pane].mode = info->mode;
	// P3a slice 2: desired-state edit -- the scheduler reconciles at the
	// next switch (configDirty), and the pane needs a fresh pass.  Wake the
	// loop via the rotation flag (NOT mEditPending: this is not a scene
	// edit; other panes' content is still valid).
	mPaneRender[pane].configDirty = true;
	mPaneRender[pane].dirty       = true;
	mPanePassPending.store( true, std::memory_order_release );
	mCV.notify_one();
	return true;
}

const char* SceneEditController::GetPaneRenderMode( unsigned int pane ) const
{
	if( pane == 0 ) return GetViewportRenderMode();   // already render-owns-scene safe
	if( pane >= kViewportPaneCount ) return "preview";   // fail-closed default
	// user-review P1#4: a production/agent render holds mMutex for its ENTIRE
	// duration; the macOS refinement pill polls the mode for EVERY pane every
	// 0.5s.  Taking mMutex here would block that poll -- and the whole UI --
	// until the render returns.  Answer without mMutex under that window:
	// pane setters are no-op'd while render-owns-scene, so mPaneConfigs[pane]
	// .mode has NO concurrent writer (same guarantee GetPaneRefinementStatus
	// relies on).
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) {
		const Implementation::ViewportRenderModeInfo* info =
			Implementation::FindViewportRenderModeInfo( mPaneConfigs[pane].mode );
		return info ? info->name : "preview";
	}
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return "preview";
	const Implementation::ViewportRenderModeInfo* info =
		Implementation::FindViewportRenderModeInfo( mPaneConfigs[pane].mode );
	return info ? info->name : "preview";
}

bool SceneEditController::SetPaneVantageSceneCamera( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( pane == 0 ) {
		// Alias contract: "track the scene camera" for pane 0 means leaving
		// free-fly.  ExitFreeFly returns false when free-fly is not active,
		// but for THIS surface already-tracking is a satisfied postcondition,
		// not a failure -- mirror the no-op-true contract of the other
		// setters.  (ExitFreeFly still runs its own guards when active.)
		if( !IsFreeFlyActive( 0 ) ) return true;   // review round 2: pane-0 alias, explicit 0
		return ExitFreeFly( 0 );
	}
	std::lock_guard<std::mutex> lk( mMutex );
	if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;
	mPaneConfigs[pane].vantageKind  = PaneVantageKind::SceneCamera;
	mPaneConfigs[pane].namedViewRef = String();
	mPaneConfigs[pane].sceneCameraRef = String();
	mPaneRender[pane].configDirty = true;   // P3a slice 2 -- see SetPaneRenderMode
	mPaneRender[pane].dirty       = true;
	mPanePassPending.store( true, std::memory_order_release );
	mCV.notify_one();
	return true;
}

bool SceneEditController::SetPaneVantageSceneCameraNamed(
	unsigned int pane, const char* name )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	// Pane 0 is deliberately the active-camera editing surface.  Binding it
	// to another named camera would create a second, ambiguous camera switch
	// path beside SetActiveCamera.
	if( pane == 0 || !name || !*name ) return false;
	std::unique_lock<std::mutex> lk( mMutex );
	if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;
	// Rendering resizes and regenerates manager cameras while a pass is
	// active.  Park before reading pose/optics/basis so the snapshot cannot
	// data-race those writes.
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
	const IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	CameraSnapshot pose;
	if( !CaptureNamedSceneCameraSnapshot( *scene, String( name ), pose ) ) return false;

	mPaneConfigs[pane].vantageKind    = PaneVantageKind::SceneCameraNamed;
	mPaneConfigs[pane].pose           = pose;
	mPaneConfigs[pane].namedViewRef   = String();
	mPaneConfigs[pane].sceneCameraRef = String( name );
	mPaneRender[pane].configDirty = true;
	mPaneRender[pane].dirty       = true;
	mPanePassPending.store( true, std::memory_order_release );
	mCV.notify_one();
	return true;
}

bool SceneEditController::SetPaneVantageNamedView( unsigned int pane, const char* name )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( !name || !*name ) return false;
	CameraSnapshot pose;
	if( !FindNamedViewPose( String( name ), pose ) ) return false;   // must exist NOW (§7.2)
	if( pane == 0 ) {
		// Alias contract: land the pose in the free-fly override exactly the
		// way RestoreNamedView does (SetViewportPose parks + swaps + kicks).
		return SetViewportPose( pose, 0 );   // review round 2: pane-0 alias, explicit 0
	}
	std::lock_guard<std::mutex> lk( mMutex );
	if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;
	mPaneConfigs[pane].vantageKind  = PaneVantageKind::NamedView;
	mPaneConfigs[pane].namedViewRef = String( name );
	mPaneConfigs[pane].sceneCameraRef = String();
	mPaneConfigs[pane].pose         = pose;   // deletion fallback snapshot
	mPaneRender[pane].configDirty = true;   // P3a slice 2 -- see SetPaneRenderMode
	mPaneRender[pane].dirty       = true;
	mPanePassPending.store( true, std::memory_order_release );
	mCV.notify_one();
	return true;
}

bool SceneEditController::OnPanePointerDown( unsigned int pane, const Point2& px )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // r2/r4 contract
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		// Refuse before mMutex and before primary-pane / FreeFly state is
		// touched.  The lock remains held across the interactive-render
		// wait so a coordinated render cannot claim the scene mid-handoff.
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return false;
		std::unique_lock<std::mutex> lk( mMutex );
		if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;   // hidden/invalid

		// §7.8 ratified decision 1, applied PRECISELY (review-s3 P1): "a
		// non-navigation click in any pane makes it primary -- navigation
		// drags NEVER steal primary."  A camera-motion tool's down IS the
		// start of a navigation drag, so it must not promote; the pane
		// still becomes the RENDER TARGET below (gesture exclusivity),
		// which is a separate concern from primary/gizmo ownership.  The
		// first cut promoted unconditionally -- a ratified-decision
		// breach the slice-3 review caught (and the first version of
		// scheduler scenario (h) had enshrined).
		if( !IsCameraMotionTool( mTool ) )
		{
			mPrimaryPane = pane;
			mPrimaryPaneSnapshot.store( pane, std::memory_order_release );
		}

		// Park UNCONDITIONALLY (review-s3 P1): the FreeFly conversion
		// below mutates mViewportOverrideCamera / mViewportPose(Active) --
		// the registers the render thread reads on the documented "swapped
		// only while parked" contract.  The first cut parked only when a
		// SWITCH was needed, so a re-click on the already-current pane
		// mutated them concurrently with an in-flight pass (a genuine data
		// race; non-crashing today only by the SceneCamera=>null-override
		// invariant coincidence).  Park always; switch conditionally.
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
		if( mCurrentPane != pane || mPaneRender[pane].configDirty )
		{
			SwitchToPaneLocked_( pane );
		}

		// Kind 3's retained snapshot is display-only after deletion.  Camera
		// tools must resolve the desired manager identity NOW and fail closed;
		// otherwise the generic editor fallback could touch the active camera
		// while the pane still claims to be bound to another name.
		if( pane != 0 && IsCameraMotionTool( mTool )
		 && mPaneConfigs[pane].vantageKind == PaneVantageKind::SceneCameraNamed )
		{
			const IScene* scene = mJob.GetScene();
			const ICameraManager* cameras = scene ? scene->GetCameras() : nullptr;
			if( !cameras
			 || !cameras->GetItem( mPaneConfigs[pane].sceneCameraRef.c_str() ) )
			{
				return false;
			}
		}

		// §7.2: a camera-motion gesture on a SECONDARY pane still tracking
		// the scene camera converts the pane to per-pane FreeFly, seeded
		// from the scene camera it was showing.  The scene camera itself
		// is never mutated by secondary-pane navigation.  (Pane 0 keeps
		// the classic direct-camera-edit semantics -- see §7.2's 2026-07-21
		// amendment.)
		if( pane != 0 && IsCameraMotionTool( mTool )
		 && mPaneConfigs[pane].vantageKind == PaneVantageKind::SceneCamera )
		{
			const IScene* scene = mJob.GetScene();
			const ICamera* activeCam = scene ? scene->GetCamera() : nullptr;
			CameraSnapshot snap;
			if( activeCam && CameraIntrospection::CaptureCameraSnapshot( *activeCam, snap ) )
			{
				if( ICamera* realized = RealizeStandaloneCamera( snap, *scene ) )
				{
					if( mViewportOverrideCamera ) mViewportOverrideCamera->release();
					mViewportOverrideCamera = realized;   // owns the factory addref
					mViewportPose           = snap;
					mViewportPoseActive     = true;
					mPaneConfigs[pane].vantageKind = PaneVantageKind::FreeFly;
					mPaneConfigs[pane].pose        = snap;
				}
			}
		}

		// Arm the gesture pane for the forwarded un-indexed Down (one-shot
		// -- see mGesturePaneArmed's doc).  SAFE ONLY under the class-wide
		// "pointer events are UI-thread-only" contract: the handoff spans
		// a lock release, so two threads driving pointer events into one
		// controller could misroute a gesture's pane.  No in-tree caller
		// does (both C-ABI shims are UI-thread); revisit with any
		// multi-window shell (review-s3 P3, defense-in-depth note).
		mGesturePane      = pane;
		mGesturePaneArmed = true;
		// Publish the gesture exclusion before dropping admission.  The
		// forwarded legacy OnPointerDown repeats this store, but this first
		// one prevents a render from slipping into the lock-release handoff:
		// any submission that wins before the forward sees pointerDown and
		// refuses, leaving the pane routing intact.
		mPointerDown.store( true, std::memory_order_release );
	}
	OnPointerDown( px );
	return true;
}

bool SceneEditController::OnPanePointerMove( unsigned int pane, const Point2& px )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	{
		std::lock_guard<std::mutex> lk( mMutex );
		// Move/Up must keep the same fail-closed visible-pane contract as
		// Down.  Accepting a hidden or mismatched event could otherwise mutate
		// or finish the gesture that belongs to another pane.
		if( !mPointerDown.load( std::memory_order_acquire )
		 || pane >= PaneCountForLayout( mViewportLayout )
		 || pane != mGesturePane ) return false;
	}
	OnPointerMove( px );
	return true;
}

bool SceneEditController::OnPanePointerUp( unsigned int pane, const Point2& px )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	{
		std::lock_guard<std::mutex> lk( mMutex );
		if( !mPointerDown.load( std::memory_order_acquire )
		 || pane >= PaneCountForLayout( mViewportLayout )
		 || pane != mGesturePane ) return false;
	}
	OnPointerUp( px );
	return true;
}

bool SceneEditController::PaneEnterFreeFly( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( pane == 0 ) return EnterFreeFlyFromActiveCamera( 0 );   // alias contract (review round 2: explicit 0)
	CameraSnapshot snap;
	{
		std::unique_lock<std::mutex> lk( mMutex );
		if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;
		const IScene* scene = mJob.GetScene();
		if( !scene ) return false;
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		// Convert the view this pane is displaying, not blindly the scene
		// camera.  In particular, a NamedView must keep its framing when the
		// user selects Enter Free-Fly from the pane chrome.
		bool haveSeed = false;
		const PaneConfig& pc = mPaneConfigs[pane];
		if( pc.vantageKind == PaneVantageKind::NamedView ) {
			CameraSnapshot live;
			if( FindNamedViewPose( pc.namedViewRef, live ) ) snap = live;
			else                                             snap = pc.pose;
			haveSeed = true;
		}
		else if( pc.vantageKind == PaneVantageKind::SceneCameraNamed ) {
			CameraSnapshot live;
			if( CaptureNamedSceneCameraSnapshot( *scene, pc.sceneCameraRef, live ) ) snap = live;
			else                                                                     snap = pc.pose;
			haveSeed = true;
		}
		else if( pc.vantageKind == PaneVantageKind::FreeFly ) {
			snap = pc.pose;
			haveSeed = true;
		}
		if( !haveSeed ) {
			const ICamera* activeCam = scene->GetCamera();
			if( !activeCam || !CameraIntrospection::CaptureCameraSnapshot( *activeCam, snap ) ) return false;
		}
		mPaneConfigs[pane].vantageKind  = PaneVantageKind::FreeFly;
		mPaneConfigs[pane].pose         = snap;
		mPaneConfigs[pane].namedViewRef = String();
		mPaneConfigs[pane].sceneCameraRef = String();
		mPaneRender[pane].configDirty = true;   // realize at the next switch
		mPaneRender[pane].dirty       = true;
		mPanePassPending.store( true, std::memory_order_release );
	}
	mCV.notify_one();
	return true;
}

bool SceneEditController::PaneExitFreeFly( unsigned int pane )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( pane == 0 ) return ExitFreeFly( 0 );   // alias contract (review round 2: explicit 0)
	// Back to tracking the scene camera -- exactly SetPaneVantageSceneCamera.
	return SetPaneVantageSceneCamera( pane );
}

bool SceneEditController::GetPaneRefinementStatus( unsigned int pane, int& outPhase,
                                                   unsigned int& outScaleDivisor ) const
{
	if( pane >= kViewportPaneCount ) return false;
	// user-review P1#5: a production/agent render holds mMutex for its
	// ENTIRE duration (render-owns-scene).  Taking mMutex here would block
	// the UI's 0.5s status poll -- and thus the whole UI, including cancel
	// -- until the render returns.  Answer without mMutex under that
	// window, exactly like every other UI-facing accessor's guard.  The
	// coarse "Rendering" is honest: a render IS in flight.
	if( mRenderOwnsScene.load( std::memory_order_acquire ) )
	{
		outScaleDivisor = mPreviewScale.load( std::memory_order_acquire );
		outPhase = 1;   // Rendering
		return true;
	}
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) {
		outScaleDivisor = mPreviewScale.load( std::memory_order_acquire );
		outPhase = 1;   // Rendering: a render is acquiring/holding the controller.
		return true;
	}
	if( pane == mCurrentPane )
	{
		// Live registers -- delegate to the existing single status so the
		// phase semantics (variant carve-out, paused, polish) stay ONE
		// implementation.  Safe under mMutex: GetRefinementStatus reads
		// atomics only, no lock of its own.
		outPhase = static_cast<int>( GetRefinementStatus( outScaleDivisor ) );
		return true;
	}
	// Unscheduled: derive the honest coarse answer from the saved slot.
	// review-s3 P1: Paused is GLOBAL and must be checked FIRST, mirroring
	// GetRefinementStatus's own ordering -- without this an unscheduled
	// dirty pane reported Rendering while refinement was paused,
	// contradicting the "same contract as the un-indexed call" doc.
	const PaneRenderState& slot = mPaneRender[pane];
	outScaleDivisor = slot.previewScaleSaved;
	if( mRefinementPaused.load( std::memory_order_acquire ) )
	{
		outPhase = 4;   // Paused
	}
	else if( slot.dirty )
	{
		outPhase = 1;   // Rendering (a pass is owed)
	}
	else if( slot.polishSaved == static_cast<int>( PolishState::PolishQueued ) )
	{
		outPhase = 3;   // Polishing (queued)
	}
	else
	{
		outPhase = 0;   // Idle / settled
	}
	return true;
}

bool SceneEditController::ApplyPaneFlyOp_( const SceneEdit& edit )
{
	std::unique_lock<std::mutex> lk( mMutex );
	if( !mViewportPoseActive || !mViewportOverrideCamera ) return false;
	Implementation::CameraCommon* cam =
		dynamic_cast<Implementation::CameraCommon*>( mViewportOverrideCamera );
	if( !cam ) return false;
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	SceneEditor::ApplyCameraOpToCamera( *cam, edit, mEditor.SceneScale() );
	cam->RegenerateData();
	// review-s3 P3: CHECKED capture into a temp -- committing a failed
	// capture would leave a Frankenstein pose (fresh shared fields, stale
	// type-specifics).  On failure the camera still moved (renders
	// correctly); the pose record keeps its last good value and the next
	// re-realization uses that -- logged so drift is diagnosable.
	CameraSnapshot flownPose;
	if( CameraIntrospection::CaptureCameraSnapshot( *cam, flownPose ) )
	{
		mViewportPose = flownPose;
		if( mGesturePane < kViewportPaneCount ) {
			mPaneConfigs[mGesturePane].pose = flownPose;
		}
	}
	else
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::ApplyPaneFlyOp_: pose re-capture failed -- pane pose record keeps its last good value" );
	}

	// Pane-LOCAL invalidation: only the flown pane's content changed.
	// Stamp the edit clock so the during-motion scale adaptation engages
	// (same reason the object-motion path stamps it).
	mLastEditTimeMs.store( NowMs(), std::memory_order_release );
	mPolishState.store( static_cast<int>( PolishState::None ), std::memory_order_release );
	mPaneRender[mGesturePane].dirty = true;
	mPanePassPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
}

bool SceneEditController::SetPaneSurfaceDims( unsigned int pane, unsigned int w, unsigned int h )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	// Either both zero (reset to film dims) or both nonzero.
	if( ( w == 0 ) != ( h == 0 ) ) return false;
	std::lock_guard<std::mutex> lk( mMutex );
	if( pane >= PaneCountForLayout( mViewportLayout ) ) return false;   // hidden panes refuse (§7.4)
	if( mPaneConfigs[pane].surfaceW == w && mPaneConfigs[pane].surfaceH == h ) return true;
	mPaneConfigs[pane].surfaceW = w;
	mPaneConfigs[pane].surfaceH = h;
	mPaneRender[pane].dirty = true;
	mPanePassPending.store( true, std::memory_order_release );
	mCV.notify_one();
	return true;
}

bool SceneEditController::SetPaneSink( unsigned int pane, IRasterizerOutput* pSink )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::mutex> lk( mMutex );
	if( pane >= kViewportPaneCount ) return false;   // sinks may be pre-wired for hidden panes
	// review-s3 P1: PARK before the swap -- the render thread reads
	// paneSink unlocked in DoOneRenderPass's attach block (and the
	// rasterizer addrefs it there).  Swapping mid-pass could release the
	// old sink to zero while the pass was about to addref it: an
	// unconditional-by-construction use-after-free.  Same cancel-and-park
	// shape as every register mutator; sink swaps are rare (shell
	// startup / teardown), so the cancel cost is irrelevant.
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
	// SetPaneSink has an owning public contract for EVERY pane, including pane
	// 0.  Keep that retained override in the pane slot; DoOneRenderPass falls
	// back to mPreviewSink only when the slot is empty.  SetPreviewSink clears
	// this slot when a legacy caller later replaces pane 0's sink.
	if( pSink ) pSink->addref();
	if( mPaneRender[pane].paneSink ) mPaneRender[pane].paneSink->release();
	mPaneRender[pane].paneSink = pSink;
	return true;
}

bool SceneEditController::GetPaneVantage( unsigned int pane, PaneVantageKind& outKind,
                                          String& outNamedView ) const
{
	if( pane >= kViewportPaneCount ) return false;
	if( pane == 0 ) {
		// Alias view: free-fly active => FreeFly, else SceneCamera.  Pane 0's
		// named-view assignment lands in the free-fly pose (see above), so it
		// reports FreeFly -- the honest answer about what is rendering.
		outKind = IsFreeFlyActive( 0 ) ? PaneVantageKind::FreeFly
		                            : PaneVantageKind::SceneCamera;
		outNamedView = String();
		return true;
	}
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;
	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;
	outKind      = mPaneConfigs[pane].vantageKind;
	outNamedView = ( outKind == PaneVantageKind::SceneCameraNamed )
		? mPaneConfigs[pane].sceneCameraRef : mPaneConfigs[pane].namedViewRef;
	return true;
}

// ======================================================================
// P3a slice 2: the context-switch scheduler (RENDER_MODES.md §7.3).
// ======================================================================

// PaneRenderState::realizedMode value-inits to 0; pin as Preview like
// PaneConfig::mode.
static_assert( static_cast<int>( Implementation::ViewportRenderMode::Preview ) == 0,
	"PaneRenderState::realizedMode's value-init default assumes Preview == 0" );

//! Extracted from SetViewportRenderMode (P3a slice 2) -- see the header doc.
bool SceneEditController::BuildVariantRasterizer_(
	Implementation::ViewportRenderMode mode, IRasterizer** out )
{
	if( !out ) return false;
	// review-p3 P2-c: recover the PRODUCTION rasterizer's actual configured
	// default shader instead of letting CreateBeautyVariantPipeline fall
	// back to its own generic internal DefaultPathTracing default -- see
	// AgentSession.cpp's twin of this fix for the full rationale.
	IShader* pProductionDefaultShader = nullptr;
	{
		const std::string activeRastName = mJob.GetActiveRasterizerName();
		if( !activeRastName.empty() ) {
			const std::string shaderName = mJob.GetRasterizerParameter(
				activeRastName.c_str(), "shader" );
			if( !shaderName.empty() && mJob.GetShaders() ) {
				pProductionDefaultShader = mJob.GetShaders()->GetItem( shaderName.c_str() );
			}
		}
	}

	IRasterizer* variantRast   = nullptr;
	IRayCaster*  variantCaster = nullptr;
	if( !Implementation::CreateBeautyVariantPipeline(
			mode, &variantRast, &variantCaster, pProductionDefaultShader ) )
	{
		return false;
	}
	// The rasterizer holds its own addref'd reference to the caster
	// internally (see the factory's doc) -- our local caster handle is not
	// needed once construction succeeds.
	safe_release( variantCaster );
	*out = variantRast;
	return true;
}

//! REQUIRES mMutex held AND the render parked (callers: the mint lock after
//! CancelAndParkRender_-equivalent quiescence -- mRendering is false inside
//! the mint lock by construction; the pane-0 setters after their own park).
//!
//! Saves the working registers into mPaneRender[mCurrentPane], then loads
//! mPaneRender[pane] into the registers, reconciling the slot against its
//! CONFIGURED mode/vantage when the config changed while unscheduled
//! (configDirty).  Register set: mPolishState, mViewportOverrideCamera/
//! Pose/PoseActive, mInteractiveFrameStore, mVariantRasterizer, and the
//! view-mode caster installed on mInteractiveImpl.
void SceneEditController::SwitchToPaneLocked_( unsigned int pane )
{
	if( pane >= kViewportPaneCount ) return;
	const unsigned int from = mCurrentPane;
	if( pane == from && !mPaneRender[pane].configDirty ) return;   // nothing to do

	// ---- SAVE registers -> slot[from] --------------------------------
	PaneRenderState& fromSlot = mPaneRender[from];
	fromSlot.polishSaved = mPolishState.load( std::memory_order_acquire );
	// review-r1 P1: the resolution ladder + variant pin are registers too.
	// Without saving them, a variant pane rotated in via the scheduler ran
	// UNPINNED (idle refinement walked its fixed divisor toward full res),
	// and leaving one left the pin stuck on the NEXT pane.
	fromSlot.previewScaleSaved  = mPreviewScale.load( std::memory_order_acquire );
	fromSlot.previewPinnedSaved = mPreviewScalePinned.load( std::memory_order_acquire );
	fromSlot.overrideCamera = mViewportOverrideCamera;   // ownership moves to the slot
	fromSlot.poseActive     = mViewportPoseActive;
	fromSlot.poseSaved      = mViewportPose;
	{
		std::lock_guard<std::mutex> fsLk( mInteractiveFrameStoreMutex );
		fromSlot.frameStore     = mInteractiveFrameStore;   // ownership moves
		mInteractiveFrameStore  = nullptr;
	}
	fromSlot.variantRasterizer = mVariantRasterizer;
	mVariantRasterizer         = nullptr;
	mViewportOverrideCamera    = nullptr;
	mViewportPoseActive        = false;

	// ---- RECONCILE slot[pane] against its configured state -----------
	PaneRenderState& slot = mPaneRender[pane];
	const Implementation::ViewportRenderMode wantMode =
		( pane == 0 ) ? mViewportRenderMode : mPaneConfigs[pane].mode;
	if( slot.configDirty || slot.realizedMode != wantMode )
	{
		// Mode changed while unscheduled: retire the stale realization.
		// Fail-closed ORDER on the variant rebuild below matches
		// SetViewportRenderMode's (build new before releasing old is not
		// needed here -- the slot is not live while unscheduled, so a
		// failed build simply leaves the pane falling back to Preview,
		// logged, rather than half-realized).
		if( slot.viewModeCaster )    { slot.viewModeCaster->release();    slot.viewModeCaster = nullptr; }
		if( slot.variantRasterizer ) { slot.variantRasterizer->release(); slot.variantRasterizer = nullptr; }
		slot.realizedMode = wantMode;
	}
	if( pane != 0 && slot.configDirty )
	{
		// Vantage reconcile: drop the stale realized camera; re-realize
		// below from the configured vantage.
		if( slot.overrideCamera ) { slot.overrideCamera->release(); slot.overrideCamera = nullptr; }
		slot.poseActive = false;
	}
	// review-r2-A P1: the configDirty CLEAR must be unconditional.  It used
	// to live inside the pane!=0 vantage block above, so pane 0's flag --
	// set by RebindEditorToJob on every scene rebind -- could NEVER clear:
	// the same-pane early-out stopped firing and EVERY subsequent mint
	// pass re-entered the mode-reconcile, tearing down and rebuilding
	// pane 0's caster/variant on every pass forever (depth's auto-window
	// could never converge again -- the exact invariant the slots exist
	// to protect, reopened by the round-1 fix).
	slot.configDirty = false;

	// Lazy vantage realization for panes 1-3 (pane 0's vantage is the
	// existing free-fly machinery -- its registers were saved/loaded
	// verbatim above/below).
	if( pane != 0 && !slot.overrideCamera &&
	    mPaneConfigs[pane].vantageKind != PaneVantageKind::SceneCamera )
	{
		CameraSnapshot pose = mPaneConfigs[pane].pose;
		if( mPaneConfigs[pane].vantageKind == PaneVantageKind::NamedView )
		{
			// Live re-resolve by name (§7.2); the config snapshot is the
			// deletion fallback.
			CameraSnapshot live;
			if( FindNamedViewPose( mPaneConfigs[pane].namedViewRef, live ) ) pose = live;
		}
		else if( mPaneConfigs[pane].vantageKind == PaneVantageKind::SceneCameraNamed )
		{
			// Same desired-state pattern as NamedView, but resolve from the
			// live camera manager.  Camera edits mark bound panes configDirty;
			// camera deletion therefore re-enters here and uses the captured
			// binding-time snapshot when the manager lookup fails.
			if( const IScene* scene = mJob.GetScene() )
			{
				CameraSnapshot live;
				if( CaptureNamedSceneCameraSnapshot(
						*scene, mPaneConfigs[pane].sceneCameraRef, live ) )
				{
					pose = live;
				}
			}
		}
		if( const IScene* scene = mJob.GetScene() )
		{
			if( ICamera* cam = RealizeStandaloneCamera( pose, *scene ) )
			{
				slot.overrideCamera = cam;   // owns the factory addref
				slot.poseActive     = true;
				slot.poseSaved      = pose;
			}
			else
			{
				GlobalLog()->PrintEx( eLog_Warning,
					"SceneEditController::SwitchToPaneLocked_: pane %u vantage could not be realized -- falling back to the scene camera", pane );
			}
		}
	}

	// Lazy mode realization: data-mode caster (persisted per-pane so depth's
	// auto-window survives rotation) or variant pipeline.
	const bool wantVariant = Implementation::IsBeautyVariantMode( wantMode );
	if( mInteractiveImpl )
	{
		if( wantVariant )
		{
			if( !slot.variantRasterizer )
			{
				IRasterizer* variantRast = nullptr;
				if( !BuildVariantRasterizer_( wantMode, &variantRast ) )
				{
					GlobalLog()->PrintEx( eLog_Warning,
						"SceneEditController::SwitchToPaneLocked_: pane %u variant pipeline build failed -- falling back to Preview", pane );
				}
				slot.variantRasterizer = variantRast;   // may stay null (fallback)
			}
			// Variant mode leaves mInteractiveImpl's caster untouched (same
			// contract as SetViewportRenderMode's variant branch).
			mInteractiveImpl->SetViewModeCaster( nullptr );
			// review-r1 P1: the variant pin (mirrors SetViewportRenderMode's
			// variant branch).  Stamped on the SLOT's saved registers so the
			// LOAD below (which runs after this reconcile block) carries it
			// into the live registers.
			{
				const Implementation::ViewportRenderModeInfo* vinfo =
					Implementation::FindViewportRenderModeInfo( wantMode );
				slot.previewScaleSaved  = ( vinfo && vinfo->variantScaleDivisor > 0 )
					? vinfo->variantScaleDivisor : 1;
				slot.previewPinnedSaved = true;
			}
		}
		else if( wantMode != Implementation::ViewportRenderMode::Preview )
		{
			if( !slot.viewModeCaster )
			{
				IRayCaster* caster = nullptr;
				if( Implementation::CreateInteractiveViewModeCaster( wantMode, &caster ) )
				{
					slot.viewModeCaster = caster;   // keep OUR addref; rasterizer adds its own below
				}
			}
			const Implementation::ViewportRenderModeInfo* info =
				Implementation::FindViewportRenderModeInfo( wantMode );
			mInteractiveImpl->SetViewModeCaster( slot.viewModeCaster,
				info && info->wantsDenoise );
		}
		else
		{
			mInteractiveImpl->SetViewModeCaster( nullptr );   // Preview
		}
		// review-r1 P1: a NON-variant pane must never inherit a variant
		// pin (the stuck-pin leak).  The slot's saved pin is authoritative
		// only for variant panes; force it clear here.
		if( !wantVariant )
		{
			slot.previewPinnedSaved = false;
		}
		// X-ray is a GLOBAL toggle stamped onto "every caster the
		// rasterizer currently holds" -- a slot caster that was NOT
		// installed when SetViewportXray last ran carries a stale flag.
		// Re-stamp after every install so the flag is always current.
		mInteractiveImpl->SetXrayView( mViewportXray );
	}

	// ---- LOAD slot[pane] -> registers --------------------------------
	mPolishState.store( slot.polishSaved, std::memory_order_release );
	// review-r1 P1: restore the ladder/pin, then let the mode arms below
	// OVERRIDE for variant panes (divisor+pin) exactly as
	// SetViewportRenderMode's variant branch does for pane 0.
	mPreviewScale.store( slot.previewScaleSaved, std::memory_order_release );
	mPreviewScalePinned.store( slot.previewPinnedSaved, std::memory_order_release );
	mViewportOverrideCamera = slot.overrideCamera;   // ownership moves back
	mViewportPoseActive     = slot.poseActive;
	mViewportPose           = slot.poseSaved;
	slot.overrideCamera     = nullptr;
	{
		std::lock_guard<std::mutex> fsLk( mInteractiveFrameStoreMutex );
		mInteractiveFrameStore = slot.frameStore;    // may be null => lazily allocated next pass
		mInteractiveFrameStorePane = pane;           // user-review P1#3: store now belongs to `pane`
		slot.frameStore        = nullptr;
	}
	mVariantRasterizer      = slot.variantRasterizer;
	slot.variantRasterizer  = nullptr;
	mCurrentPane            = pane;
}

//! REQUIRES mMutex held.  Priority (§7.3): gesture pins the current pane;
//! else primary-dirty, secondaries-dirty by index, primary-polish,
//! secondaries-polish; else the current pane (no work -- caller checks
//! AnyVisiblePaneHasWorkLocked_ before treating the answer as work).
unsigned int SceneEditController::PickNextVisiblePaneLocked_() const
{
	if( mPointerDown.load( std::memory_order_acquire )
	 || mScrubInProgress.load( std::memory_order_acquire ) )
	{
		return mCurrentPane;
	}
	const unsigned int visible = PaneCountForLayout( mViewportLayout );
	auto polishQueued = [&]( unsigned int p ) -> bool {
		const int st = ( p == mCurrentPane )
			? mPolishState.load( std::memory_order_acquire )
			: mPaneRender[p].polishSaved;
		return st == static_cast<int>( PolishState::PolishQueued );
	};
	if( mPrimaryPane < visible && mPaneRender[mPrimaryPane].dirty ) return mPrimaryPane;
	for( unsigned int i = 0; i < visible; ++i ) {
		if( mPaneRender[i].dirty ) return i;
	}
	if( mPrimaryPane < visible && polishQueued( mPrimaryPane ) ) return mPrimaryPane;
	for( unsigned int i = 0; i < visible; ++i ) {
		if( polishQueued( i ) ) return i;
	}
	return mCurrentPane;
}

//! REQUIRES mMutex held.
bool SceneEditController::AnyVisiblePaneHasWorkLocked_() const
{
	const unsigned int visible = PaneCountForLayout( mViewportLayout );
	for( unsigned int i = 0; i < visible; ++i ) {
		if( mPaneRender[i].dirty ) return true;
		const int st = ( i == mCurrentPane )
			? mPolishState.load( std::memory_order_acquire )
			: mPaneRender[i].polishSaved;
		if( st == static_cast<int>( PolishState::PolishQueued ) ) return true;
	}
	return false;
}

//! REQUIRES mMutex held.
void SceneEditController::MarkAllVisiblePanesDirtyLocked_()
{
	const unsigned int visible = PaneCountForLayout( mViewportLayout );
	for( unsigned int i = 0; i < visible; ++i ) {
		mPaneRender[i].dirty = true;
	}
}

bool SceneEditController::PromoteNamedViewToCamera(
	unsigned int idx, const String& proposedName, char* outName, unsigned int outLen )
{
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( !outName || outLen == 0 ) return false;
	outName[0] = '\0';
	CameraSnapshot snapshot;
	{
		std::lock_guard<std::mutex> lk( mNamedViewsMutex );
		if( idx >= mNamedViews.size() ) return false;
		snapshot = mNamedViews[ idx ].pose;   // copy before park
	}

	// Park before mutating the ICameraManager (same rationale as stamp/clone).
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	IScene* scene = mJob.GetScene();
	if( !scene ) return false;
	ICameraManager* cams = const_cast<ICameraManager*>( scene->GetCameras() );
	if( !cams ) return false;
	const String finalName = UniqueCameraName( proposedName, *cams );
	if( finalName.size() > outLen ) {
		outName[0] = '\0';
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController::PromoteNamedViewToCamera: outName buffer too small for `%s` (need %u, got %u) — not adding camera",
			finalName.c_str(), static_cast<unsigned>( finalName.size() ), outLen );
		return false;
	}

	SceneEdit edit;
	edit.op             = SceneEdit::AddCamera;
	edit.objectName     = finalName;
	edit.cameraSnapshot = snapshot;
	if( !mEditor.Apply( edit ) ) return false;

	{
		const char* s = finalName.c_str();
		const size_t needed = finalName.size();
		for( size_t i = 0; i < needed; ++i ) outName[i] = s[i];
	}

	mSceneEpoch.fetch_add( 1, std::memory_order_acq_rel );
	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	return true;
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
	auto dirtyNotificationDeferral = mEditor.DeferDirtyNotifications();
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) {
		AgentCommitResult roR; roR.applied = false; roR.rawCode = 0;
		roR.status = String( "rejected" ); roR.message = String( "scene is render-locked" );
		return roR;
	}
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
		const unsigned int nc = CategoryEntityCountLocked_( cat );   // under nlk (round-3 getter split)
		inCat.reserve( nc );
		for( unsigned int i = 0; i < nc; ++i ) inCat.emplace_back( CategoryEntityNameLocked_( cat, i ).c_str() );
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
	auto dirtyNotificationDeferral = mEditor.DeferDirtyNotifications();
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) {
		AgentCommitResult roR; roR.applied = false; roR.rawCode = 0;
		roR.status = String( "rejected" ); roR.message = String( "scene is render-locked" );
		return roR;
	}
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
		if( cat == Category::Camera && uniqueFallback )   // round-7 gated camera fallback (typo-refusal)
			uniqueFallback = RISE::Cst::DocCameraUniqueFallbackPermitted(
				*doc, name.size() > 1 ? name.c_str() : "", mJob.GetActiveCameraName() );
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
	auto dirtyNotificationDeferral = mEditor.DeferDirtyNotifications();
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) {
		AgentCommitResult roR; roR.applied = false; roR.rawCode = 0;
		roR.status = String( "rejected" ); roR.message = String( "scene is render-locked" );
		return roR;
	}
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
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	out = EnvironmentInfo();

	std::unique_lock<std::mutex> lk( mMutex, std::try_to_lock );
	if( !lk.owns_lock() ) return false;

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
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
	if( outPersisted ) *outPersisted = false;

	if( mTxnOpen.load( std::memory_order_acquire ) ) {
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

	// Round-4 review P1 (transactional ordering; matches the Rasterizer arm):
	// validate recordability BEFORE the live edit -- an environment edit the
	// Document cannot carry would mark dirty, then Save would clear the flag
	// while serializing nothing and a reload would revert it.  Refuse instead.
	// Legacy scenes (no retained Document) keep live-only editing.
	const bool hasDoc = mJob.HasRetainedCstDocument();
	if( hasDoc ) {
		const RISE::Cst::Document* doc = mJob.GetCstDocument();
		const RISE::Cst::NodeId rid = doc
			? RISE::Cst::DocFindByNameAnyRole( *doc, "", nullptr, activeName.c_str(), true )
			: 0;
		if( rid == 0 ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"SceneEditController: environment edit `%s` refused -- the scene has no unique `%s` chunk "
				"in its retained Document, so the edit could not survive a save/reload.",
				paramName, activeName.c_str() );
			if( outPersisted ) *outPersisted = false;
			return false;
		}
	}

	const bool ok = mJob.SetRasterizerParameter( activeName.c_str(), paramName, value.c_str() );
	if( !ok ) return false;

	// Mirror to the CST (Document-only, no re-derive; safe under the lock --
	// pCstDocument is swapped here too).  The pre-flight guarantees the chunk
	// resolves; gate the dirty mark on the actual record anyway (the flag must
	// never claim a change the Document does not carry).
	const int cstResult = hasDoc ? mJob.ApplyCstEnvironmentEdit( paramName, value.c_str() ) : 0;
	if( outPersisted ) *outPersisted = ( cstResult != 0 );
	if( cstResult != 0 ) mEditor.MarkCstHeadDirty( "", nullptr );

	mEditPending.store( true, std::memory_order_release );
	lk.unlock();
	mCV.notify_one();
	// Round-5 review P1: this is a DIRECT mutation entry point (the public
	// SetEnvironmentScale/Background/Orient wrappers) -- drain here so a
	// C-API / headless caller that never polls RefreshProperties still gets
	// the dirty notification.
	admissionLk.unlock();
	mEditor.DrainDirtyNotification();
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
	auto dirtyNotificationDeferral = mEditor.DeferDirtyNotifications();
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
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
	auto dirtyNotificationDeferral = mEditor.DeferDirtyNotifications();
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) {
		AgentCommitResult roR; roR.applied = false; roR.rawCode = 0;
		roR.status = String( "rejected" ); roR.message = String( "scene is render-locked" );
		return roR;
	}
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
	auto dirtyNotificationDeferral = mEditor.DeferDirtyNotifications();
	std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
	if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) ) return false;
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
	const int idx = static_cast<int>( cat );
	if( idx < 0 || idx >= kNumCategories ) return false;
	const String targetName = mSelectionByCategory[idx];
	bool ok = false;
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return false;
		ok = SetPropertyInner_( cat, targetName, name, valueStr );
	}
	// The primary selection was never overwritten, so a synchronous dirty
	// callback sees coherent UI state and any re-entrant SetSelection it makes
	// remains authoritative.
	mEditor.DrainDirtyNotification();
	return ok;
}

// ---------------------------------------------------------------------
// Document-first ADR phase 1: public mutation entry points = inner body +
// post-unlock drain of the deferred dirty notification.  The inner bodies
// release mMutex before returning, so the drain (which may run the shell's
// dirty-changed listener) never executes under a controller lock.  Idempotent
// and cheap when nothing changed (atomic test-and-clear).
// ---------------------------------------------------------------------
bool SceneEditController::SetProperty( const String& name, const String& valueStr )
{
	bool ok = false;
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return false;
		ok = SetPropertyInner_( mSelectionCategory, mSelectionName, name, valueStr );
	}
	mEditor.DrainDirtyNotification();
	return ok;
}

bool SceneEditController::SetSelection( Category cat, const String& entityName )
{
	bool ok = false;
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return false;
		ok = SetSelectionInner_( cat, entityName );
	}
	mEditor.DrainDirtyNotification();
	return ok;
}

void SceneEditController::Undo()
{
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return;
		UndoInner_();
	}
	mEditor.DrainDirtyNotification();
}

void SceneEditController::Redo()
{
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
			return;
		RedoInner_();
	}
	mEditor.DrainDirtyNotification();
}

SceneEditController::AgentCommitResult SceneEditController::ApplyAgentParamEdit(
	const String& entityName, const String& entityKind, const String& param,
	const String& value, const RISE::Cst::CstHeadVersion* baseVersionOrNull )
{
	AgentCommitResult r;
	{
		std::unique_lock<std::recursive_mutex> admissionLk( mRenderAdmissionMutex );
		if( mAgentRenderBlocksInteractive.load( std::memory_order_acquire ) )
		{
			r.status = String( "rejected" );
			r.retriable = true;
			r.message = String( "render queued or in progress -- retry after it completes" );
			return r;
		}
		r = ApplyAgentParamEditInner_(
			entityName, entityKind, param, value, baseVersionOrNull );
	}
	mEditor.DrainDirtyNotification();
	return r;
}

bool SceneEditController::SetPropertyInner_(
	Category targetCategory, const String& targetName,
	const String& name, const String& valueStr )
{
	if( mRenderOwnsScene.load( std::memory_order_acquire ) ) return false;  // render-owns-scene guard
	// External-review P1 (2026-07-22): general dangling-reference guard.  Reject
	// BEFORE any arm mutates the scene when this edit would persist a reference
	// to a runtime-only entity (no CST chunk) -- it would render now but fail on
	// save/reload / a later full re-derive.  Covers every reference-carrying
	// category uniformly (material slot rebinds, object material/geometry/shader
	// bindings, painter/geometry child refs); inline literals + CST-backed names
	// + `none` are untouched.  See WouldPersistDanglingReference_.
	if( targetName.size() > 1
	 && WouldPersistDanglingReference_( targetCategory, targetName, name, valueStr ) )
	{
		GlobalLog()->PrintEx( eLog_Warning,
			"SceneEditController: edit of `%s.%s` rejected -- `%s` names a runtime-only entity with "
			"no CST chunk, so persisting it would write a DANGLING reference that fails on save/reload. "
			"Bind a scene-defined entity instead.",
			targetName.c_str(), name.c_str(), valueStr.c_str() );
		return false;
	}
	switch( targetCategory ) {

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
			const String editedCamera = targetName.size() > 1
				? targetName : String( mJob.GetActiveCameraName().c_str() );
			InvalidatePanesBoundToSceneCameraLocked_( editedCamera );
			mEditPending.store( true, std::memory_order_release );
			lk.unlock();
			mCV.notify_one();
		}
		return ok;
	}

	case Category::Animation: {
		// The panel labels the descriptor's persisted `num_frames` field as
		// `frames`. Route it through the same checked CST transaction path as
		// other descriptor value edits so the live animation, retained
		// Document, dirty state, undo/redo, and save/reload all agree.
		if( !( name == String( "frames" ) ) ) return false;
		unsigned int newFrames = 0;
		if( sscanf( valueStr.c_str(), "%u", &newFrames ) != 1 || newFrames < 1 ) return false;
		if( targetName.size() <= 1 ) return false;
		const AgentCommitResult r = ApplyAgentParamEditInner_(
			targetName, String( "animation" ), String( "num_frames" ),
			valueStr, nullptr );
		return r.applied;
	}

	case Category::Object: {
		// Route the panel's editable rows to the matching SceneEdit
		// op.  Every path goes through SceneEditor::Apply so undo /
		// redo work end-to-end alongside drag-driven transform edits.
		if( targetName.size() <= 1 ) return false;

		SceneEdit edit;
		edit.objectName = targetName;
		bool shadowEdit = false;
		bool editCastsShadows = false;
		bool shadowValue = false;

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
			if( !ParsePropertyBool( valueStr, shadowValue ) ) return false;
			shadowEdit = true;
			editCastsShadows = ( name == String( "casts_shadows" ) );
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

		if( shadowEdit ) {
			const IScene* scene = mJob.GetScene();
			IObjectManager* objs =
				scene ? const_cast<IObjectManager*>( scene->GetObjects() ) : nullptr;
			const IObject* obj =
				objs ? objs->GetItem( targetName.c_str() ) : nullptr;
			if( !obj ) return false;
			bool castsShadows = obj->DoesCastShadows();
			bool receivesShadows = obj->DoesReceiveShadows();
			if( editCastsShadows ) castsShadows = shadowValue;
			else receivesShadows = shadowValue;
			edit.op = SceneEdit::SetObjectShadowFlags;
			edit.s = static_cast<Scalar>(
				( castsShadows ? 1 : 0 ) | ( receivesShadows ? 2 : 0 ) );
		}

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
		if( targetName.size() <= 1 ) return false;

		SceneEdit edit;
		edit.op            = SceneEdit::SetLightProperty;
		edit.objectName    = targetName;   // light entity name
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
		// Document-first ADR phase 3: rasterizer edits ride the TRANSACTION
		// path. targetName is the rasterizer chunk keyword; the empty
		// target + kind form addresses the sole chunk of that kind (Job's
		// generalized unique-in-kind resolve).  ApplyAgentParamEdit does the
		// whole coherent sequence -- park, dry-run-gated Document edit,
		// re-derive (which re-realizes the rasterizer from its chunk), undo
		// capture, gated dirty mark, re-render kick -- so the bespoke
		// live-rebuild + record + mark sequence this arm used to hand-roll
		// (and mis-roll: rounds 3-4 found both a lost-on-save and a
		// saved-but-lost bug in it) is GONE, and rasterizer edits gain
		// undo/redo.  An unrecordable edit (no unique chunk of this kind)
		// rejects inside the path -- same outcome as the round-4 pre-flight.
		// Legacy scenes (no retained Document) keep the LIVE-only rebuild.
		if( targetName.size() <= 1 ) return false;
		if( !mJob.HasRetainedCstDocument() ) {
			if( mTxnOpen.load( std::memory_order_acquire ) ) {
				GlobalLog()->PrintEx( eLog_Warning, "SceneEditController: rasterizer property edit refused inside an open transaction (live-only on a legacy scene -> not undoable)." );
				return false;
			}
			std::unique_lock<std::mutex> lk( mMutex );
			if( mRendering.load( std::memory_order_acquire ) ) {
				mCancelProgress.RequestCancel();
				mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
			}
			mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );
			const bool ok = mJob.SetRasterizerParameter(
				targetName.c_str(), name.c_str(), valueStr.c_str() );
			if( !ok ) return false;
			mEditPending.store( true, std::memory_order_release );
			lk.unlock();
			mCV.notify_one();
			return true;
		}
		const AgentCommitResult r = ApplyAgentParamEditInner_(
			String( "" ), targetName, name, valueStr, nullptr );
		return r.applied;
	}

	case Category::Film: {
		if( mTxnOpen.load( std::memory_order_acquire ) ) {
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
			if( w || h || p ) {
				// Round-3 sibling fix + round-4 gate: mark dirty ONLY when the
				// Document actually recorded the change (ApplyCstFilmEdit inserts
				// a film chunk when absent, so 0 is pathological -- but the flag
				// must never claim a change the Document does not carry, else
				// Save clears dirty while serializing nothing).  Empty name ->
				// the CST-head boolean channel; the listener fires under this
				// held mMutex (see the SetDirtyChangedListener contract).
				if( mJob.ApplyCstFilmEdit( w, h, p ) != 0 )
					mEditor.MarkCstHeadDirty( "", nullptr );
			}
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
		if( targetName.size() <= 1 ) return false;

		// GUI redesign 2026-07-22 (external-review P1): the material panel now
		// surfaces BOTH painter/scalar-painter SLOTS and the scalar VALUE params
		// merged in from the descriptor+CST by AugmentWithCstRows (ar_film_ior,
		// and -- crucially -- physical scalars like a dielectric `ior` whose
		// slot is an IScalarPainter pipe but whose scene value is an INLINE
		// NUMBER `ior 1.5`, not a painter name).  SetMaterialProperty only
		// REBINDS a slot: it looks the value up as a painter NAME and rejects
		// anything unregistered ("scalar_painter `1.7` is not registered").  So
		// route to SetMaterialProperty ONLY when the entered value actually
		// names a registered painter in the slot's pipe (a genuine rebind);
		// every other edit -- an inline numeric, or a non-slot descriptor param
		// (GetSlot==None) -- is a VALUE edit that must go through the generic CST
		// param-edit path (which cancel-and-parks + captures undo/redo itself),
		// exactly like Painter/Geometry.  Reading the managers here (pre-park)
		// matches the Object arm's pre-park scene reads; ApplyAgentParamEdit
		// re-resolves by name under its own lock, so the brief TOCTOU is benign.
		// Classify UNDER a brief mMutex hold (external-review P1): the manager
		// reads (GetMaterials/GetSlot/GetPainters) would otherwise race an agent
		// commit's D2 manager swap -> UAF.  The routing decision is acted on with
		// the lock RELEASED (ApplyAgentParamEdit re-locks itself; the slot-rebind
		// path re-parks below) -- both re-resolve by name, so the release window
		// is a benign routing TOCTOU, not a correctness gap.
		bool rebindSlot = false;
		{
			std::lock_guard<std::mutex> clk( mMutex );
			const IMaterialManager* mats = mJob.GetMaterials();
			const IMaterial* mat = mats
				? const_cast<IMaterialManager*>( mats )->GetItem( targetName.c_str() )
				: nullptr;
			const MaterialSlotRef slot = mat
				? MaterialIntrospection::GetSlot( *mat, name )
				: MaterialSlotRef();
			if( slot.kind == MaterialSlotRef::Painter ) {
				IPainterManager* pm = mJob.GetPainters();
				rebindSlot = pm && pm->GetItem( valueStr.c_str() ) != nullptr;
			} else if( slot.kind == MaterialSlotRef::ScalarPainter ) {
				IScalarPainterManager* spm = mJob.GetScalarPainters();
				rebindSlot = spm && spm->GetItem( valueStr.c_str() ) != nullptr;
			}
		}
		if( !rebindSlot ) {
			const AgentCommitResult r = ApplyAgentParamEditInner_(
				targetName, String( "material" ), name, valueStr, nullptr );
			return r.applied;
		}

		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		SceneEdit edit;
		edit.op            = SceneEdit::SetMaterialProperty;
		edit.objectName    = targetName;
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
		if( targetName.size() <= 1 ) return false;

		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) ) {
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		SceneEdit edit;
		edit.op            = SceneEdit::SetMediumProperty;
		edit.objectName    = targetName;
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
		// live setter surface (see CstIntrospection.h's header
		// doc), so route straight through the generic agent CST
		// param-edit path -- the "RouteCstParamEdit_-class" path
		// Material/Light/Medium use on CST scenes, generalized: it
		// already cancel-and-parks, captures the prior value for
		// undo/redo (PushAgentCstParamEdit), marks dirty, bumps light
		// generation if needed, and kicks the re-render itself, so no
		// separate park/SceneEdit/Apply dance is needed here.
		if( targetName.size() <= 1 ) return false;
		const AgentCommitResult r = ApplyAgentParamEditInner_(
			targetName, String( "painter" ), name, valueStr, nullptr );
		return r.applied;
	}

	case Category::Geometry: {
		// GUI redesign 2026-07-22: identical route to Painter -- the
		// generic agent CST param-edit path (cancel-and-parks, captures
		// the prior value for undo/redo, marks dirty, re-derives the
		// geometry AND every object referencing it, kicks the re-render).
		if( targetName.size() <= 1 ) return false;
		const AgentCommitResult r = ApplyAgentParamEditInner_(
			targetName, String( "geometry" ), name, valueStr, nullptr );
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
void SceneEditController::EnsureInteractiveFrameStore_( unsigned int width, unsigned int height, IRasterizer* activeRast )
{
	if( !activeRast ) return;
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
			dynamic_cast<Implementation::Rasterizer*>( activeRast );
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
		mInteractiveFrameStorePane = mCurrentPane;   // user-review P1#3: fresh store is for the current pane
	}

	// SetFrameStore reads mInteractiveFrameStore on the render thread,
	// sequenced after the reassign above -- same no-lock-needed reasoning as
	// the same-dims short-circuit.
	Implementation::Rasterizer* r =
		dynamic_cast<Implementation::Rasterizer*>( activeRast );
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
	unsigned int& outHeight,
	unsigned int* outSourcePane ) const
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
			// user-review P1#3: capture the owning pane UNDER THE SAME LOCK
			// so (frame, pane) are consistent.
			if( outSourcePane ) *outSourcePane = mInteractiveFrameStorePane;
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

	// ResizeFilm temporarily rewrites every camera's frame dimensions and
	// projection matrix for this pass.  Overlay projection readers use the
	// same mutex with try_lock, so they skip one refresh rather than reading a
	// half-updated CameraCommon or blocking the UI while rasterization runs.
	std::unique_lock<std::mutex> cameraFrameLk( mCameraFrameMutex );

	// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): while a
	// BeautyVariant mode (deep_reflect/direct) is active, `mVariantRasterizer`
	// is a wholly SEPARATE, controller-owned production-class PT pipeline the
	// render loop drives INSTEAD of mInteractiveRasterizer -- not a caster
	// swap on it.  Race-free by construction: mVariantRasterizer is mutated
	// only under mMutex with the render parked (SetViewportRenderMode /
	// RebindEditorToJob), and this function only runs while mRendering is
	// true (i.e. NOT parked) -- the same contract mInteractiveImpl's own
	// caster-swap state already relies on.
	IRasterizer* const activeRast = mVariantRasterizer ? mVariantRasterizer : mInteractiveRasterizer;

	// Install the cancellable progress and preview sink.  DoOnePass re-adds
	// its sink every pass (FreeRasterizerOutputs then AddRasterizerOutput),
	// so the variant rasterizer needs the SAME per-pass attachment the
	// preview rasterizer gets -- it starts every pass with an empty outs
	// list otherwise.
	activeRast->SetProgressCallback( &mCancelProgress );
	// P3a slice 3: attach the CURRENT pane's sink for this quantum; fall
	// back to the legacy single sink (which remains pane 0's default and
	// the only sink either shipped GUI wires today).  mPaneRender[...]
	// .paneSink is written only under mMutex (SetPaneSink) and this read
	// runs on the render thread between quanta -- same swapped-registers
	// contract as the caster state above.  mCurrentPane is stable for the
	// pass (stamped at the mint).
	{
		// user-review P1#1: ALWAYS clear the outputs first.  If this ran
		// only inside `if( sinkForPass )`, a pane with no sink of its own
		// would leave the PREVIOUS pane's output attached and publish its
		// pixels into it.  And the mPreviewSink fallback is legacy pane-0
		// behaviour ONLY: a SECONDARY pane with no sink must render to
		// nothing, never into pane 0's sink (that was "secondary pixels
		// published to pane 0").
		activeRast->FreeRasterizerOutputs();
		// Pane 0 retains the legacy preview sink as its fallback.  An explicit
		// SetPaneSink(0, ...) override wins while installed; secondary panes
		// use only their own sinks and otherwise render to nothing.
		IRasterizerOutput* sinkForPass = ( mCurrentPane == 0 )
			? ( mPaneRender[0].paneSink ? mPaneRender[0].paneSink : mPreviewSink )
			: mPaneRender[mCurrentPane].paneSink;
		if( sinkForPass )
		{
			activeRast->AddRasterizerOutput( sinkForPass );
		}
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

	// P3a slice 3: a pane with explicit surface dims renders at THOSE dims
	// (divided by the preview scale) instead of the film's rest dims -- the
	// same ResizeFilm swap+restore mechanism, different base.  0 = film
	// dims (pane 0's default; single-viewport behaviour unchanged).  The
	// swap now also engages at scale==1 when pane dims differ from film
	// dims, so a half-width pane genuinely renders half-width.
	const unsigned int paneW = mCurrentPaneSurfaceW;
	const unsigned int paneH = mCurrentPaneSurfaceH;
	{
		IScenePriv* spProbe = mJob.GetScene();
		const IFilm* fProbe = spProbe ? spProbe->GetFilm() : nullptr;
		const bool paneDimsDiffer = paneW && fProbe &&
			( paneW != fProbe->GetWidth() || paneH != fProbe->GetHeight() );
		if( paneDimsDiffer && scale <= 1 )
		{
			const unsigned int restW   = fProbe->GetWidth();
			const unsigned int restH   = fProbe->GetHeight();
			const Scalar       restPAR = fProbe->GetPixelAR();
			spProbe->ResizeFilm( paneW, paneH, restPAR );
			restoreGuard.sp    = spProbe;
			restoreGuard.w     = restW;
			restoreGuard.h     = restH;
			restoreGuard.pAR   = restPAR;
			restoreGuard.armed = true;
		}
	}

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
				// P3a slice 3: pane dims override the base when set.
				const unsigned int baseW = paneW ? paneW : restW;
				const unsigned int baseH = paneH ? paneH : restH;
				const unsigned int sw = baseW / scale > 0 ? baseW / scale : 1;
				const unsigned int sh = baseH / scale > 0 ? baseH / scale : 1;
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
		EnsureInteractiveFrameStore_( passW, passH, activeRast );
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
			dynamic_cast<Implementation::PixelBasedRasterizerHelper*>( activeRast ) )
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
	activeRast->RasterizeScene( *scene, pRegion, /*seq*/0 );
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
	// GUI render modes P2a: a BeautyVariant mode's PINNED divisor must
	// never be adapted away by the during-motion ladder either -- see
	// mPreviewScalePinned's header doc.
	const bool gestureActive =
		mPointerDown.load( std::memory_order_acquire )
	 || mScrubInProgress.load( std::memory_order_acquire );
	if( gestureActive && !mInRefinementPass
	 && !mPreviewScalePinned.load( std::memory_order_acquire ) )
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
