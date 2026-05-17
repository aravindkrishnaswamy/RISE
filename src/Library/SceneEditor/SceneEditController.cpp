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
#include "ObjectIntrospection.h"
#include "LightIntrospection.h"
#include "RasterizerIntrospection.h"
#include "FilmIntrospection.h"
#include "MaterialIntrospection.h"
#include "../Interfaces/IMaterialManager.h"
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
#include "../Interfaces/IEnumCallback.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IRayCaster.h"
#include "../Intersection/RayIntersection.h"
#include "../Rendering/InteractivePelRasterizer.h"
#include "../Rendering/FrameStore.h"  // L6e-3 — per-pass interactive FrameStore
#include "../Rendering/Rasterizer.h"  // L6e-3 — Implementation::Rasterizer for SetFrameStore
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
, mSelectionCategory( Category::None )
, mSelectionName()
// `mSectionExpanded` and `mSelectionByCategory` are value-init'd via
// default-member-init (bool defaults to false; String defaults to
// empty) so they need no explicit ctor entry.  Listed here as a
// documentation reminder.
, mSceneEpoch( NextEpoch().fetch_add( 1, std::memory_order_acq_rel ) )
, mLastPx( 0, 0 )
, mPointerDown( false )
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
	// Per-category arrays — bool array members are NOT
	// default-init'd to false (would be indeterminate otherwise).
	// String members default-init correctly to empty.
	for( int i = 0; i < kNumCategories; ++i ) {
		mSectionExpanded[i] = false;
	}
	// Phase 3: install material + shader manager hooks so
	// SetObjectMaterial / SetObjectShader edits can resolve names
	// at apply time.  Test harnesses that build a SceneEditor
	// directly (without the controller) skip this and the editor
	// degrades to "transform / camera ops only" mode.
	mEditor.SetMaterialManager( mJob.GetMaterials() );
	mEditor.SetShaderManager( mJob.GetShaders() );
	// Phase 4: plumb the painter managers so SceneEdit::
	// SetMaterialProperty can resolve painter-name strings (the
	// panel's painter-rebind value) into the actual IPainter*
	// / IScalarPainter* the material expects on its slot.
	mEditor.SetPainterManager( mJob.GetPainters() );
	mEditor.SetScalarPainterManager( mJob.GetScalarPainters() );
	// Plumb IJob so SceneEdit::SetObjectInteriorMedium can resolve
	// medium names through IJob::GetMedium and recover prev-state via
	// IJob::EnumerateMediumNames.  IJobPriv inherits IJob virtually,
	// so the upcast is implicit.
	mEditor.SetJob( &mJob );
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
		if( mSelectionCategory == Category::Object && mSelectionName.size() > 1 )
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
		edit.op = SceneEdit::TranslateObject;
		edit.objectName = mSelectionName;
		// Phase 3 will project the screen-space delta into world space
		// using the camera + depth-at-pick.  For Phase 2 we use a
		// simple scaled mapping so the test can drive the controller
		// without the camera math.
		edit.v3a = Vector3( delta.x * 0.01, -delta.y * 0.01, 0 );
		break;

	case Tool::RotateObject:
		if( !haveObject ) return;
		edit.op = SceneEdit::RotateObjectArb;
		edit.objectName = mSelectionName;
		edit.v3a = Vector3( 0, 1, 0 );  // y-axis (placeholder for Phase 3)
		edit.s   = delta.x * 0.005;
		break;

	case Tool::ScaleObject:
		if( !haveObject ) return;
		{
			// Convert vertical drag to a scale factor delta.  Up-drag
			// shrinks (negative dy), down-drag grows.  Apply as an
			// incremental "set scale to current * (1 + k*dy)" — but
			// without a getter for current scale we fall back to a
			// modest stretch op for Phase 2 placeholder.
			edit.op = SceneEdit::SetObjectStretch;
			edit.objectName = mSelectionName;
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
		if( mSelectionCategory == Category::Object && mSelectionName.size() > 1 )
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

void SceneEditController::Undo()
{
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

	const bool didWork = mEditor.Undo();
	if( didWork ) {
		// If Undo removed the entity our panel selection pointed at
		// (e.g. Undo of AddCamera removes the clone we just made
		// active), the (category, name) tuple is stale — the panel
		// would otherwise render a header for an entity that no
		// longer exists.  Reset to category-only (empty name) so
		// the panel falls back to "section open, no row picked"
		// behaviour and shows the active-camera fallback rows.
		if( !SelectionStillResolves( mJob, mSelectionCategory, mSelectionName ) ) {
			mSelectionName = String();
			// Clear the per-cat entry too so the panel's per-section
			// state stays consistent with the primary.  Don't touch
			// mSectionExpanded — the user's section remains open
			// with no entity picked (matches "header click" UX).
			const int idx = static_cast<int>( mSelectionCategory );
			if( idx > 0 && idx < kNumCategories ) {
				mSelectionByCategory[idx] = String();
			}
		}
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
	std::unique_lock<std::mutex> lk( mMutex );
	if( mRendering.load( std::memory_order_acquire ) ) {
		mCancelProgress.RequestCancel();
		mCancelCount.fetch_add( 1, std::memory_order_acq_rel );
	}
	mCV.wait( lk, [&]{ return !mRendering.load( std::memory_order_acquire ); } );

	const bool didWork = mEditor.Redo();
	if( didWork ) {
		// Symmetric with Undo: if Redo brought back a removed
		// entity the selection had pointed at, the tuple may
		// re-resolve cleanly.  If it doesn't, drop to category-
		// only.  (Today the only Redo path that adds an entity is
		// AddCamera, which uses the original name — so the
		// resolution typically succeeds.)
		if( !SelectionStillResolves( mJob, mSelectionCategory, mSelectionName ) ) {
			mSelectionName = String();
			// Clear the per-cat entry too so the panel's per-section
			// state stays consistent with the primary.  Don't touch
			// mSectionExpanded — the user's section remains open
			// with no entity picked (matches "header click" UX).
			const int idx = static_cast<int>( mSelectionCategory );
			if( idx > 0 && idx < kNumCategories ) {
				mSelectionByCategory[idx] = String();
			}
		}
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
	case Category::Object:
	case Category::Light:
	case Category::Material:
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

}  // namespace

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
		  || cat == Category::Film )
		&& entityName.size() > 1;   // empty name = just expand, no swap

	if( needsRenderSerialization )
	{
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
	//     Materials section with the object's bound material name.
	//     Empty material binding still expands Materials (the user
	//     sees the section open with a "(unset)" combo).
	// (b) Material direct pick (non-empty) -> clear AND collapse
	//     the Object section per the user-confirmed rule.
	if( cat == Category::Object ) {
		if( entityName.size() > 1 ) {
			const int matIdx = static_cast<int>( Category::Material );
			mSelectionByCategory[ matIdx ] = FindObjectMaterialName( mJob, entityName );
			mSectionExpanded[ matIdx ]     = true;
		}
		// Note: a "section header click" on Object with empty name
		// does NOT auto-expand Material — the user explicitly
		// opened just Object.  Material expands when an entity is
		// actually picked.
	} else if( cat == Category::Material && entityName.size() > 1 ) {
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
	if( i < 0 || i >= 7 ) return 0;
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
		if( !mEditor.Apply( edit ) ) return false;
		KickRender();
		return true;
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
