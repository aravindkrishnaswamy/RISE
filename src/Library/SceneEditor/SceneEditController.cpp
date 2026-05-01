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
#include "../Interfaces/IScene.h"
#include "../Interfaces/IScenePriv.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/ICameraManager.h"
#include "../Cameras/CameraCommon.h"
#include "../Interfaces/IEnumCallback.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IRayCaster.h"
#include "../Intersection/RayIntersection.h"
#include "../Rendering/InteractivePelRasterizer.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/RuntimeContext.h"
#include <chrono>
#include <cstdio>
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

// Synthetic property-row name used by RefreshProperties + SetProperty
// to surface the "active camera" picker at the top of the camera
// panel.  Two reasons it lives in a named constant:
//
//   1. It's referenced from two places (the row injector and the
//      SetProperty intercept) — DRY beats two stringly-typed checks.
//   2. The name is reserved.  A user who authors a real camera with
//      this name in their .RISEscene would shadow the picker;
//      callers should treat it as part of the editor's vocabulary
//      rather than user input.  We don't reject it at AddCamera time
//      because doing so would couple the parser to editor state — an
//      unwelcome dependency direction.
static const char* const kActiveCameraSyntheticRow = "active_camera";

SceneEditController::SceneEditController( IJobPriv& job, IRasterizer* interactiveRasterizer )
: mJob( job )
, mInteractiveRasterizer( interactiveRasterizer )
, mInteractiveImpl( dynamic_cast<Implementation::InteractivePelRasterizer*>( interactiveRasterizer ) )
, mEditor( *job.GetScene() )
, mTool( Tool::Select )
, mSelected()
, mLastPx( 0, 0 )
, mPointerDown( false )
, mScrubInProgress( false )
, mPreviewSink( 0 )
, mProgressSink( 0 )
, mLogSink( 0 )
, mCancelProgress( 0 )
, mRenderThread()
, mMutex()
, mCV()
, mRunning( false )
, mEditPending( false )
, mRendering( false )
, mCancelCount( 0 )
, mRenderCount( 0 )
, mFullResW( 0 )
, mFullResH( 0 )
, mPreviewScale( 1 )
, mLastEditTimeMs( 0 )
, mInRefinementPass( false )
, mPolishState( static_cast<int>( PolishState::None ) )
{
	if( mInteractiveRasterizer )
	{
		mInteractiveRasterizer->addref();
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

}  // namespace

SceneEditController::~SceneEditController()
{
	Stop();
	if( mInteractiveRasterizer )
	{
		mInteractiveRasterizer->release();
		mInteractiveRasterizer = 0;
	}
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
		if( const ICamera* cam = scene->GetCamera() ) {
			mFullResW.store( cam->GetWidth(),  std::memory_order_release );
			mFullResH.store( cam->GetHeight(), std::memory_order_release );
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

void SceneEditController::SetTool( Tool t ) { mTool = t; }
SceneEditController::Tool SceneEditController::CurrentTool() const { return mTool; }

// Pointer events ------------------------------------------------------

void SceneEditController::OnPointerDown( const Point2& px )
{
	mPointerDown.store( true, std::memory_order_release );
	mLastPx = px;
	mLastEditTimeMs.store( NowMs(), std::memory_order_release );

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
		if( mSelected.size() > 1 )
		{
			mEditor.BeginComposite( "Drag" );
			isMotionTool = true;
		}
		break;

	case Tool::OrbitCamera:
	case Tool::PanCamera:
	case Tool::ZoomCamera:
	case Tool::RollCamera:
		mEditor.BeginComposite( "Camera" );
		isMotionTool = true;
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

	// `mSelected` is a RISE::String (std::vector<char> + trailing NUL).
	// An empty name has size()==1 (just the NUL), a one-char name has
	// size()==2.  The `> 1` checks below treat size()==1 (or 0) as
	// "no selection" — do NOT replace with `.empty()`/`size() > 0`,
	// the inversion would translate every drag with no object selected
	// into a NULL-name FindObject lookup.
	switch( mTool )
	{
	case Tool::TranslateObject:
		if( mSelected.size() <= 1 ) return;
		edit.op = SceneEdit::TranslateObject;
		edit.objectName = mSelected;
		// Phase 3 will project the screen-space delta into world space
		// using the camera + depth-at-pick.  For Phase 2 we use a
		// simple scaled mapping so the test can drive the controller
		// without the camera math.
		edit.v3a = Vector3( delta.x * 0.01, -delta.y * 0.01, 0 );
		break;

	case Tool::RotateObject:
		if( mSelected.size() <= 1 ) return;
		edit.op = SceneEdit::RotateObjectArb;
		edit.objectName = mSelected;
		edit.v3a = Vector3( 0, 1, 0 );  // y-axis (placeholder for Phase 3)
		edit.s   = delta.x * 0.005;
		break;

	case Tool::ScaleObject:
		if( mSelected.size() <= 1 ) return;
		{
			// Convert vertical drag to a scale factor delta.  Up-drag
			// shrinks (negative dy), down-drag grows.  Apply as an
			// incremental "set scale to current * (1 + k*dy)" — but
			// without a getter for current scale we fall back to a
			// modest stretch op for Phase 2 placeholder.
			edit.op = SceneEdit::SetObjectStretch;
			edit.objectName = mSelected;
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

	if( mEditor.Apply( edit ) )
	{
		KickRender();
	}
}

void SceneEditController::OnPointerUp( const Point2& px )
{
	(void)px;
	if( !mPointerDown.load( std::memory_order_acquire ) ) return;
	mPointerDown.store( false, std::memory_order_release );

	// Whether to queue the 4-SPP polish pass after the regular
	// 1-SPP final pass.  Only motion / scrub-driven drags warrant
	// it; Select-tool taps don't move anything.
	const bool wasMotion =
		IsCameraMotionTool( mTool ) || IsObjectMotionTool( mTool );

	switch( mTool )
	{
	case Tool::TranslateObject:
	case Tool::RotateObject:
	case Tool::ScaleObject:
		if( mSelected.size() > 1 )
		{
			mEditor.EndComposite();
		}
		break;
	case Tool::OrbitCamera:
	case Tool::PanCamera:
	case Tool::ZoomCamera:
	case Tool::RollCamera:
		mEditor.EndComposite();
		break;
	default:
		break;
	}

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
	mEditor.BeginComposite( "Scrub" );
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
	mEditor.EndComposite();
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

void SceneEditController::Undo()
{
	if( mEditor.Undo() ) KickRender();
}

void SceneEditController::Redo()
{
	if( mEditor.Redo() ) KickRender();
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

// Selection -----------------------------------------------------------

String SceneEditController::SelectedObjectName() const
{
	return mSelected;
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
			if( const ICamera* cam = scene->GetCamera() ) {
				w = cam->GetWidth();
				h = cam->GetHeight();
				return w > 0 && h > 0;
			}
		}
		return false;
	}
	w = cachedW;
	h = cachedH;
	return true;
}

void SceneEditController::ForTest_SetSelected( const String& name )
{
	mSelected = name;
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
	// Pick the topmost object under the click and store its name in
	// `mSelected`.  Empty selection ⇒ panel switches to None mode.
	mSelected = String();

	const IScene* scene = mJob.GetScene();
	if( !scene ) return;

	const ICamera* cam = scene->GetCamera();
	if( !cam ) return;

	IObjectManager* objs = const_cast<IObjectManager*>( scene->GetObjects() );
	if( !objs ) return;

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
	const Scalar pickPxY = static_cast<Scalar>( cam->GetHeight() ) - px.y;

	RuntimeContext rc( GlobalRNG(), RuntimeContext::PASS_NORMAL, /*threaded*/false );
	Ray r;
	if( !cam->GenerateRay( rc, r, Point2( px.x, pickPxY ) ) ) return;

	RasterizerState rast;
	RayIntersection ri( r, rast );
	objs->IntersectRay( ri, /*frontFace*/true, /*backFace*/false, /*exit*/false );

	if( ri.geometric.bHit && ri.pObject ) {
		// IObject has no GetName() — walk the manager to recover the
		// registered name corresponding to the hit pointer.  O(n);
		// fine for click cadence and typical scene sizes.
		FindObjectNameCallback cb( ri.pObject, objs );
		objs->EnumerateItemNames( cb );
		mSelected = cb.foundName;
	}
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
			auto pred = [&]{
				return mEditPending.load( std::memory_order_acquire )
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
	// Camera tools are unconditionally Camera mode — even with no
	// selection, the camera always exists once a scene is loaded.
	if( IsCameraMotionTool( mTool ) ) {
		return PanelMode::Camera;
	}
	// Select tool with a picked object → Object mode.  Empty
	// `mSelected` (size <= 1: at minimum the trailing NUL) means
	// nothing is picked.
	if( mTool == Tool::Select && mSelected.size() > 1 ) {
		return PanelMode::Object;
	}
	// Anything else: empty panel.
	return PanelMode::None;
}

String SceneEditController::CurrentPanelHeader() const
{
	switch( CurrentPanelMode() ) {
	case PanelMode::Camera:
		// Plural "Cameras" — the panel's first row is a dropdown
		// that lists every camera in the scene's manager and lets
		// the user pick which one is active.  See RefreshProperties
		// for how the row is synthesised.
		return String( "Cameras" );
	case PanelMode::Object: {
		// String concatenation is awkward on RISE::String (a vector
		// of char) — build via std::string and convert at the end.
		std::string s = "Object: ";
		s += mSelected.c_str();
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
	const IScene* scene = mJob.GetScene();
	if( !scene ) return;

	switch( CurrentPanelMode() ) {
	case PanelMode::Camera: {
		// First row: synthetic "active_camera" dropdown listing every
		// camera in the scene's manager.  This row predates the
		// camera-specific introspection so the user always sees the
		// picker at the top of the panel — and so changing it
		// triggers a property re-fetch (the per-camera rows are
		// type-specific: a pinhole camera shows fov, a thinlens
		// shows sensor_size + fstop + ... + focus_distance).
		const ICameraManager* cams = scene->GetCameras();
		if( cams && cams->getItemCount() > 0 ) {
			CameraProperty pickerRow;
			pickerRow.name        = String( kActiveCameraSyntheticRow );
			pickerRow.kind        = ValueKind::Enum;
			pickerRow.value       = scene->GetActiveCameraName();
			pickerRow.description = String( "Switch which camera the rasterizer renders through.  Lists every camera registered with the scene." );
			// Editable so the existing presets-menu UI path
			// surfaces; the underlying SetActiveCamera rejects
			// unknown names rather than crashing, so a typo just
			// no-ops.  All three platform UIs only render the
			// presets dropdown inside the editable branch.
			pickerRow.editable    = true;
			// Build presets {label = name, value = name} from the
			// manager's enumeration order (lexicographic by name).
			struct CollectNames : public IEnumCallback<const char*> {
				std::vector<ParameterPreset>* out;
				bool operator()( const char* const& name ) override {
					ParameterPreset preset;
					preset.label = std::string( name );
					preset.value = std::string( name );
					out->push_back( preset );
					return true;
				}
			};
			CollectNames cb;
			cb.out = &pickerRow.presets;
			cams->EnumerateItemNames( cb );
			mProperties.push_back( pickerRow );
		}

		const ICamera* cam = scene->GetCamera();
		if( !cam ) return;
		std::vector<CameraProperty> camProps = CameraIntrospection::Inspect( *cam );
		mProperties.insert( mProperties.end(), camProps.begin(), camProps.end() );
		return;
	}
	case PanelMode::Object: {
		// Basic object inspector — name + world-position derived
		// from the object's final-transform matrix.  Read-only for
		// now; full descriptor-driven object editing is future
		// work (would need object-side ChunkDescriptors and a
		// SetObjectProperty SceneEdit op).
		IObjectManager* objs = const_cast<IObjectManager*>( scene->GetObjects() );
		if( !objs ) return;
		const IObject* obj = objs->GetItem( mSelected.c_str() );
		if( !obj ) return;

		const Matrix4 m = obj->GetFinalTransformMatrix();
		// Translation lives in row 3 of RISE's row-major Matrix4
		// (Math3D::Translation populates `_30/_31/_32`; PointsOps::
		// Transform adds these to the rotated point — see
		// Math3D/PointsOps.h).
		const Scalar tx = m._30;
		const Scalar ty = m._31;
		const Scalar tz = m._32;

		auto addRow = [&]( const char* name, const String& value, const char* desc ) {
			CameraProperty p;
			p.name        = String( name );
			p.value       = value;
			p.description = String( desc );
			p.kind        = ValueKind::String;
			p.editable    = false;
			mProperties.push_back( p );
		};

		addRow( "Name", mSelected,
		        "The name of the picked object as declared in the .RISEscene file." );

		{
			char buf[128];
			std::snprintf( buf, sizeof(buf), "%g %g %g",
			              static_cast<double>( tx ),
			              static_cast<double>( ty ),
			              static_cast<double>( tz ) );
			addRow( "Position", String( buf ),
			        "World-space position derived from the final-transform matrix." );
		}

		return;
	}
	case PanelMode::None:
	default:
		// Leave mProperties empty.
		return;
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

bool SceneEditController::SetProperty( const String& name, const String& valueStr )
{
	// "active_camera" is a synthetic row injected by RefreshProperties;
	// CameraIntrospection doesn't know about it, and switching cameras
	// is a navigation action (not a value mutation), so it bypasses the
	// transactional undo path on purpose.  After the switch, Refresh
	// will re-emit the per-camera rows for the newly-active camera —
	// the platform UI calls propertySnapshot() on every selection
	// change, so the panel rebuilds itself.
	if( name == kActiveCameraSyntheticRow ) {
		IScenePriv* scene = mJob.GetScene();
		if( !scene ) return false;
		// Switching the active camera is a structural mutation —
		// the render thread reads the active camera per-pixel, so
		// we can't swap it under an in-flight pass without inviting
		// torn reads or use-after-free on the old camera.  Serialize
		// using the same cancel-and-park pattern as OnTimeScrub:
		//   1. Trip cancel so any in-flight RasterizeScene returns.
		//   2. cv.wait until mRendering is false under mMutex.
		//   3. Mutate the scene while still holding mMutex (the
		//      render thread can't begin a new pass without
		//      acquiring this mutex first).
		//   4. Drop the lock and KickRender so the next pass picks
		//      up the new active camera.
		std::unique_lock<std::mutex> lk( mMutex );
		if( mRendering.load( std::memory_order_acquire ) )
		{
			mCancelProgress.RequestCancel();
			mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
		}
		mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

		if( !scene->SetActiveCamera( valueStr.c_str() ) ) return false;
		mEditPending.store( true, std::memory_order_release );
		lk.unlock();
		mCV.notify_one();
		return true;
	}

	// Route through the editor's transactional Apply path so panel
	// edits land in the undo history alongside drag-driven camera
	// edits.  SceneEditor::Apply for SetCameraProperty captures the
	// prev value, applies the new one via CameraIntrospection, and
	// pushes a SceneEdit so undo/redo work end-to-end.
	SceneEdit edit;
	edit.op = SceneEdit::SetCameraProperty;
	edit.objectName = name;            // overload: holds the property name
	edit.propertyValue = valueStr;
	if( !mEditor.Apply( edit ) ) return false;

	KickRender();
	return true;
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
		if( const ICamera* refCam = scene->GetCamera() ) {
			mFullResW.store( refCam->GetWidth(),  std::memory_order_release );
			mFullResH.store( refCam->GetHeight(), std::memory_order_release );
		}
	}

	Implementation::CameraCommon* cam = nullptr;
	unsigned int origW = 0, origH = 0;
	if( scale > 1 )
	{
		ICamera* baseCam = mJob.GetScene()->GetCameraMutable();
		cam = dynamic_cast<Implementation::CameraCommon*>( baseCam );
		if( cam )
		{
			origW = cam->GetWidth();
			origH = cam->GetHeight();
			const unsigned int sw = origW / scale > 0 ? origW / scale : 1;
			const unsigned int sh = origH / scale > 0 ? origH / scale : 1;
			cam->SetDimensions( sw, sh );
		}
	}

	const auto t0 = std::chrono::steady_clock::now();
	mInteractiveRasterizer->RasterizeScene( *scene, /*pRect*/0, /*seq*/0 );
	const auto elapsed = std::chrono::steady_clock::now() - t0;

	if( cam )
	{
		// Restore original dimensions so the production rasterizer (and
		// any UI reading width/height) sees the canonical values.
		cam->SetDimensions( origW, origH );
	}

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
