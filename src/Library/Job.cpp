//////////////////////////////////////////////////////////////////////
//
//  Job.cpp - Implementation of a job
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 6, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Scene.h"   // P2a: bump light-topology generation on Job-level emitter/env edits
#include "Geometry/SDFGeometry.h"
#include <cstring>
#define _USE_MATH_DEFINES
#include "Job.h"
#include "RISE_API.h"
#include "Rendering/Film.h"		// kDefaultFilm* / kMaxFilm* constants
#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "Importers/GLTFSceneImporter.h"
#include "Shaders/SSS/DonnerJensenSkinSSSShaderOp.h"
#include "Shaders/PathTracingShaderOp.h"
#include "Shaders/DirectLightingShaderOp.h"
#include "Shaders/DistributionTracingShaderOp.h"
#include "Shaders/FinalGatherShaderOp.h"
#include "Rendering/FrameStore.h"
#include "Rendering/Rasterizer.h"
#include "Rendering/RayCaster.h"		// concrete RayCaster — dynamic_cast target for SetTransparentShadows (PT only)
#include "Rendering/PixelBasedRasterizerHelper.h"	// GetRayCaster() — reach the active rasterizer's caster for radiance_scale
#include "Utilities/RString.h"
#include "Utilities/RasterizerDefaults.h"
#include <stdio.h>
#include <set>
#include "Utilities/MediaPathLocator.h"
#include "Utilities/ThreadPool.h"
#include "Interfaces/IOptions.h"
#include "Interfaces/IScalarPainter.h"
#include "Interfaces/IPainterManager.h"
#include "Painters/PainterToScalarAdapter.h"
#include "Intersection/RayIntersectionGeometric.h"
#include <cctype>
#include <cstdlib>

using namespace RISE;

// P2a: a Job-level edit that changes the emitter or environment set must
// bump the Scene's light-topology generation, so a REUSED RayCaster rebuilds
// its LightSampler/EnvironmentSampler on the next AttachScene (mirrors the
// SceneEditor path).  Downcast like SceneEditor::BumpSceneLightGeneration.
static void BumpSceneLightGen( RISE::IScenePriv* pScene )
{
	if( RISE::Implementation::Scene* sc = dynamic_cast<RISE::Implementation::Scene*>( pScene ) )
		sc->BumpLightTopologyGeneration();
}

using namespace RISE::Implementation;

// Forward declarations for scalar-painter resolution helpers — full
// definitions live alongside the first `AddDielectricMaterial` and
// are shared by every material conversion in this file.
static IScalarPainter* ResolveScalarPainterArg(
	IScalarPainterManager* mgr,
	const char* value,
	bool requireSingle = false );
static IScalarPainter* ResolveOrDiagnoseScalar(
	IScalarPainterManager* smgr,
	IPainterManager* pmgr,
	const char* chunkKind,
	const char* chunkName,
	const char* paramName,
	const char* value,
	bool requireSingle = false );

namespace {

// Adapter that exposes an `IPainter` graph as an `IScalarPainter` —
// used internally by `Job::AddPBRMetallicRoughnessMaterial` so it can
// keep composing roughness / anisotropy through `BlendPainter` (which
// is colour-typed) while still feeding the scalar-typed `alphaX` /
// `alphaY` / `ior` / `ext` slots on the new GGX construction surface.
//
// Reads the underlying IPainter via `GetColor` / `GetColorNM`.  This
// preserves the existing PBR painter-graph composition and avoids
// rebuilding it on a scalar-painter substrate (which doesn't yet have
// the equivalent of `BlendPainter`).  PBR scenes have always routed
// these values through the `IPainter` colour-space (zero - point04 -
// roughness² inputs are all bounded < 1, so the JH spectral-uplift
// concerns that motivated the rest of the IScalarPainter refactor
// don't bite here).
// Phase 5 cleanup (2026-05-14): the local `IPainterToScalarAdapter`
// class previously defined here was a duplicate of the public
// `Painters/PainterToScalarAdapter.h` header.  Removed; callers
// below now construct `PainterToScalarAdapter` directly.

} // namespace

namespace RISE
{
	//! Creates a new empty job
	bool RISE_CreateJob(
			IJob** ppi										///< [out] Pointer to recieve the job
			)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Job();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "job" );

		return true;
	}

	bool RISE_CreateJobPriv(
			IJobPriv** ppi										///< [out] Pointer to recieve the job
			)
	{
		if( !ppi ) {
			return false;
		}

		(*ppi) = new Job();
		GlobalLog()->PrintNew( *ppi, __FILE__, __LINE__, "job" );

		return true;
	}
}


Job::Job( )
{
	InitializeContainers();
}

Job::~Job( )
{
	DestroyContainers();
}

// L6b — lazy-allocate (or reuse) the canonical FrameStore.  The
// FrameStore is reused across rasterizer swaps (per docs/FRAMESTORE_-
// DESIGN.md §7.5) — a Set*Rasterizer call with the same active-
// camera dims keeps the existing FrameStore so that file outputs +
// platform UI viewport observers stay attached.  Reallocation
// happens only when:
//   - the FrameStore hasn't been allocated yet, OR
//   - the caller's (width,height) differs from the allocated dims
//     (camera resolution change — rare in production, typical
//     during interactive viewport preview-scale stepping).
//
// On reallocation, the previous FrameStore is released by Job;
// any rasterizer or observer that still holds a counted reference
// keeps it alive until they release.  The new FrameStore is
// returned with refcount = 1 (Reference's default), addref'd by
// the caller (the rasterizer's ctor) when it's stored.
//
// Returns nullptr if width or height are zero — caller should pass
// nullptr to the factory in that case (Phase 1 fallback to internal
// IRasterImage path).
//
// Naming convention "_locked" is forward-looking: when L6c ships the
// helper rewrite, this method will need a chain-mutex pattern
// analogous to ViewportFrameStore's chainMutex_ to coordinate with
// reader threads (UI viewports, encoders).  L6b's single-threaded
// "Job is parked while rasterizer is mutated" contract from
// SceneEditController removes the need for the lock today.
RISE::Implementation::FrameStore* Job::ResolveJobFrameStoreForActiveCamera()
{
	// Camera dims live on the scene-level Film (Phase A 45aa217 split:
	// ICamera no longer exposes GetWidth/GetHeight; the canonical pixel
	// grid is `Scene::GetFilm()`).  Phase B2 (scene format v6) made the
	// `film` chunk the sole authoring surface; Film is always present
	// post-InitializeContainers, so this bail-out is defensive only.
	if( !pScene ) return nullptr;
	const IFilm* film = pScene->GetFilm();
	if( !film ) return nullptr;
	return EnsureJobFrameStore_locked( film->GetWidth(), film->GetHeight() );
}

RISE::Implementation::FrameStore* Job::EnsureJobFrameStore_locked(
	unsigned int width, unsigned int height )
{
	if( width == 0 || height == 0 ) {
		return nullptr;
	}
	if( m_jobFrameStore ) {
		const unsigned int curW =
			static_cast<unsigned int>( m_jobFrameStore->Width() );
		const unsigned int curH =
			static_cast<unsigned int>( m_jobFrameStore->Height() );
		if( curW == width && curH == height ) {
			return m_jobFrameStore;  // reuse — survives rasterizer swap
		}
		// Dims differ → reallocate.  Release Job's reference; any
		// rasterizer / observer still holding a ref keeps the OLD
		// store alive until they themselves release.
		safe_release( m_jobFrameStore );
	}
	RISE::Implementation::FrameStore::Spec spec;
	spec.width    = width;
	spec.height   = height;
	spec.tileEdge = 32;  // matches the rasterizer's typical block size

#ifdef RISE_ENABLE_OIDN
	// L7 — request AOV channels (Albedo + Normal) when OIDN is
	// compiled in.  These are populated by the rasterizer's AOV
	// collection pass (`OIDNDenoiser::CollectFirstHitAOVs` for PT,
	// per-block `Accumulate*` for BDPT) inside the
	// `bDenoisingEnabled` branch — see
	// `PixelBasedRasterizerHelper::PropagateAOVsToFrameStore_` for
	// the contract.  Exposed via `FrameStore::GetChannel<Albedo>()`
	// / `<Normal>()` for downstream consumers (multichannel EXR
	// encoders, denoiser GUIs, AOV-aware viewports).
	//
	// Memory cost (at 4K = 3840×2160 = ~8.3M pixels):
	//   * Albedo: `Channel<RISEPel>` = 3 × `Chel`/double per pixel
	//     = 24 B/pixel × 8.3M ≈ 199 MB.
	//   * Normal: `Channel<Vector3>` = 3 × `Scalar`/double per
	//     pixel = 24 B/pixel × 8.3M ≈ 199 MB.
	//   * Combined: ~400 MB.
	// Acceptable when OIDN is enabled — OIDN's own working
	// buffers are larger.  When OIDN is compiled OUT
	// (`RISE_ENABLE_OIDN=0`), the channels would still be
	// requested but never written → ~400 MB of dead memory.
	// Compile-time gate avoids that regression.
	//
	// Runtime caveat: when OIDN is compiled in but
	// `bDenoisingEnabled=false` for a particular render (user
	// disabled denoise via scene config), AOV propagation is
	// skipped — channels stay zero.  Downstream consumers reading
	// AOVs in that mode see black.  Out-of-scope for L7; future
	// work could add an "AOV-only collect" path that runs
	// independent of denoise, or expose `bAOVPopulated` on the
	// FrameStore so consumers can early-skip empty AOVs.
	//
	// Pre-L7: `aovChannels` left empty → AOV channels in FrameStore
	// stayed null, even though OIDN collected the same data per
	// frame and threw it away after denoise.  L7 retains the data
	// in the canonical store for the lifetime of the FrameStore.
	spec.aovChannels.push_back( FrameStoreOutput::ChannelId::Albedo );
	spec.aovChannels.push_back( FrameStoreOutput::ChannelId::Normal );
#endif  // RISE_ENABLE_OIDN

	m_jobFrameStore = new RISE::Implementation::FrameStore( spec );
	GlobalLog()->PrintEx( eLog_Info,
		"Job:: allocated canonical FrameStore (%ux%u, tileEdge=%u, AOVs=%s).",
		width, height, static_cast<unsigned>( spec.tileEdge ),
#ifdef RISE_ENABLE_OIDN
		"Albedo+Normal"
#else
		"(none; OIDN compiled out)"
#endif
		);
	return m_jobFrameStore;
}

void Job::InitializeContainers()
{
	// Create empty internal objects
	pRasterizer = 0;
	pGlobalProgress = 0;
	lightSampleRRThreshold = 0;

	// Phase 6.1: allocate the round-trip-save metadata containers up
	// front so IJobPriv::GetSourceSpanIndex etc. return stable pointers
	// for the Job's lifetime.  AsciiSceneParser clears + repopulates
	// them on each ParseAndLoadScene call.
	pSourceSpanIndex.reset(  new SourceSpanIndex() );
	pBaseTransforms.reset(   new TransformSnapshot() );
	pLoadedTransforms.reset( new TransformSnapshot() );
	pOverrideSpans.reset(    new OverrideSpanIndex() );

	RISE_API_CreateScene( &pScene );
	RISE_API_CreateGeometryManager( &pGeomManager );
	RISE_API_CreateMaterialManager( &pMatManager );
	RISE_API_CreateShaderManager( &pShaderManager );
	RISE_API_CreateShaderOpManager( &pShaderOpManager );
	RISE_API_CreateCameraManager( &pCameraManager );
	RISE_API_CreatePainterManager( &pPntManager );
	RISE_API_CreateScalarPainterManager( &pScalarPntManager );
	RISE_API_CreateFunction1DManager( &pFunc1DManager );
	RISE_API_CreateFunction2DManager( &pFunc2DManager );
	// Top-level acceleration default: SAH BVH (BVH4-collapsed, SIMD AABB
	// test) over scene objects.  Pre-2026-05 the default was
	// (false, false, 0, 0) — i.e., no top-level structure, so every ray
	// fell through to ObjectManager's linear loop over every IObject in
	// the scene.  Profiling on sponza_new (155 mesh objects) showed
	// ~64 % of CPU in IntersectRay because each ray was paying ~633
	// AABB tests / 6 mesh-BVH descents to reach the actual intersection.
	// Constructor flag `bUseBSPtree` is the historical name; semantically
	// it now means "build a top-level BVH" (the BSPTreeSAH path was
	// removed when ObjectManager moved to BVH<>).  Leaf-target=4 +
	// depth-cap=32 + an explicit higher SAH intersection cost in
	// CreateBVH() bias the build toward small leaves (each leaf hit
	// triggers a per-mesh sub-BVH descent, so leaves should be small).
	// ObjectManager's own gate `items.size() > nMaxObjectsPerNode`
	// keeps tiny scenes (≤4 objects) on the linear path where BVH
	// build/traverse overhead would dominate.
	RISE_API_CreateObjectManager( &pObjectManager, true, false, 4, 32 );
	RISE_API_CreateLightManager( &pLightManager );
	RISE_API_CreateModifierManager( &pModManager );

	pScene->SetCameraManager( pCameraManager );
	pScene->SetObjectManager( pObjectManager );
	pScene->SetLightManager( pLightManager );

	// Default film: quarter HD (qHD) at square pixels.  Chosen as a
	// fast iteration default for test renders — agents and humans
	// authoring scenes don't have to set width/height for a quick
	// preview.  Override via a `film` chunk in the scene file or
	// via the CLI flags --width / --height / --pixel-ar.  Defaults
	// (and upper bound) live in Rendering/Film.h as the single
	// source of truth.
	{
		IFilm* pDefaultFilm = 0;
		RISE_API_CreateFilm( &pDefaultFilm, kDefaultFilmWidth, kDefaultFilmHeight, kDefaultFilmPixelAR );
		pScene->SetFilm( pDefaultFilm );
		safe_release( pDefaultFilm );
	}

	// Adding null painter and null material
	{
		IMaterial* pNullMaterial = 0;
		RISE_API_CreateNullMaterial( &pNullMaterial );
		pMatManager->AddItem( pNullMaterial, "none" );
		safe_release( pNullMaterial );

		IPainter* pNullPainter = 0;
		RISE_API_CreateUniformColorPainter( &pNullPainter, RISEPel(0,0,0) );
		pPntManager->AddItem( pNullPainter, "none" );
		safe_release( pNullPainter );
	}

	// Adding some nice default shader ops
	{
		this->AddReflectionShaderOp( "DefaultReflection" );
		this->AddRefractionShaderOp( "DefaultRefraction" );
		this->AddEmissionShaderOp( "DefaultEmission" );
		this->AddDirectLightingShaderOp( "DefaultDirectLighting", 0 );
		this->AddCausticPelPhotonMapShaderOp( "DefaultCausticPelPhotonMap" );
		this->AddCausticSpectralPhotonMapShaderOp( "DefaultCausticSpectralPhotonMap" );
		this->AddGlobalPelPhotonMapShaderOp( "DefaultGlobalPelPhotonMap" );
		this->AddGlobalSpectralPhotonMapShaderOp( "DefaultGlobalSpectralPhotonMap" );
		this->AddTranslucentPelPhotonMapShaderOp( "DefaultTranslucentPelPhotonMap" );
		this->AddShadowPhotonMapShaderOp( "DefaultShadowPhotonMap" );
		this->AddPathTracingShaderOp( "DefaultPathTracing", false, 20, 1e-5, 10,  true );
	}
}

void Job::DestroyContainers()
{
	safe_shutdown_and_release( pScene );
	// pRasterizer is a borrowed pointer into rasterizerRegistry; the
	// map below owns the addrefs.  Walk the registry and release each
	// entry, then null out the active pointer so subsequent destructor
	// passes can't double-release.
	for( RasterizerRegistry::iterator it = rasterizerRegistry.begin();
	     it != rasterizerRegistry.end(); ++it )
	{
		safe_release( it->second.instance );
	}
	rasterizerRegistry.clear();
	pRasterizer = 0;
	activeRasterizerName.clear();

	// L6b — release the canonical FrameStore.  Each rasterizer that
	// the registry just released also held its own counted reference
	// (per Rasterizer::~Rasterizer in src/Library/Rendering/Rasterizer.cpp
	// L6a-1), so this Job-side release is the LAST holder once the
	// registry is drained — the FrameStore is destroyed here.
	safe_release( m_jobFrameStore );

	safe_shutdown_and_release( pGeomManager );
	safe_shutdown_and_release( pCameraManager );
	safe_shutdown_and_release( pPntManager );
	safe_shutdown_and_release( pScalarPntManager );
	safe_shutdown_and_release( pFunc1DManager );
	safe_shutdown_and_release( pFunc2DManager );
	safe_shutdown_and_release( pLightManager );
	safe_shutdown_and_release( pMatManager );
	safe_shutdown_and_release( pModManager );
	safe_shutdown_and_release( pShaderManager );
	safe_shutdown_and_release( pShaderOpManager );
	safe_shutdown_and_release( pObjectManager );

	// Release all named media
	{
		MediumMap::iterator it;
		for( it = mediaMap.begin(); it != mediaMap.end(); ++it ) {
			safe_release( it->second );
		}
		mediaMap.clear();
	}
}

//
// Core settings
//

//! Resets the acceleration structure
//! WARNING!  Call this before adding objects, otherwise you will LOSE them!
//! \return TRUE if successful, FALSE otherwise
bool Job::SetPrimaryAcceleration(
	const bool bUseBSPtree,									///< [in] Use BSP trees for spatial partitioning
	const bool bUseOctree,									///< [in] Use Octrees for spatial partitioning
	const unsigned int nMaxObjectsPerNode,					///< [in] Maximum number of elements / node
	const unsigned int nMaxTreeDepth						///< [in] Maximum tree depth
	)
{
	if( pObjectManager ) {
		pObjectManager->Shutdown();
		pObjectManager->release();
		pObjectManager = 0;
	}

	RISE_API_CreateObjectManager( &pObjectManager, bUseBSPtree, bUseOctree, nMaxObjectsPerNode, nMaxTreeDepth );
	pScene->SetObjectManager( pObjectManager );

	return true;
}

bool Job::SetLightSampleRRThreshold(
	const double threshold
	)
{
	lightSampleRRThreshold = threshold;
	return true;
}

bool Job::SetFilm(
	const unsigned int width,
	const unsigned int height,
	const double pixelAR
	)
{
	// Reject zero dims and non-finite / non-positive pixelAR.  `NaN <=
	// 0.0` is false (NaN comparisons always return false), so the plain
	// `pixelAR <= 0.0` check used to let NaN through and poison every
	// camera's projection matrix on the resync.  std::isfinite catches
	// NaN AND ±inf in one predicate.
	if( width == 0 || height == 0 || !std::isfinite(pixelAR) || pixelAR <= 0.0 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::SetFilm: zero / non-finite / negative dims rejected (width=%u, height=%u, pixelAR=%g).  "
			"Active Film unchanged.", width, height, pixelAR );
		return false;
	}
	if( width > kMaxFilmWidth || height > kMaxFilmHeight ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::SetFilm: dims exceed sanity bound (width=%u, height=%u; max=%u x %u).  "
			"Active Film unchanged.  If this is intentional, raise kMaxFilm{Width,Height} in Rendering/Film.h.",
			width, height, kMaxFilmWidth, kMaxFilmHeight );
		return false;
	}

	// Same-dim short-circuit: a panel edit that types the current value
	// back (or the parser declaring the same Film via the chunk default)
	// would otherwise allocate a fresh IFilm, swap-release the previous
	// one, and call SetDimensionsAndPixelAR on every camera — wasted
	// work plus a render-thread cancel-and-park that flickers the
	// preview for no reason.  Bail when nothing actually changed.
	if( pScene ) {
		const IFilm* pCurFilm = pScene->GetFilm();
		if( pCurFilm
			&& pCurFilm->GetWidth()   == width
			&& pCurFilm->GetHeight()  == height
			&& pCurFilm->GetPixelAR() == pixelAR )
		{
			return true;
		}
	}

	IFilm* pNewFilm = 0;
	if( !RISE_API_CreateFilm( &pNewFilm, width, height, pixelAR ) ) {
		return false;
	}
	pScene->SetFilm( pNewFilm );
	safe_release( pNewFilm );

	// L6b — Film dims drive the canonical FrameStore.  Without this
	// push, every rasterizer in the registry keeps its old FrameStore
	// at the previous dims, and any consumer reading `Job::GetFrameStore`
	// after SetFilm sees a stale store sized for the previous Film.
	// `PushJobFrameStoreToRasterizers` internally calls
	// `ResolveJobFrameStoreForActiveCamera` → `EnsureJobFrameStore_locked`
	// which reallocates the FrameStore if dims changed and addref-
	// swap-releases each rasterizer's reference in lockstep.  Same
	// pattern `SetActiveCamera` uses for the same reason.
	PushJobFrameStoreToRasterizers();
	return true;
}

bool Job::ScaleFilmToFit(
	const unsigned int maxSurfaceW,
	const unsigned int maxSurfaceH,
	const unsigned int maxLongEdge
	)
{
	if( maxSurfaceW == 0 || maxSurfaceH == 0 || maxLongEdge == 0 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::ScaleFilmToFit: zero argument rejected (surface=%ux%u, longEdge=%u).",
			maxSurfaceW, maxSurfaceH, maxLongEdge );
		return false;
	}
	if( !pScene ) return false;
	const IFilm* pFilm = pScene->GetFilm();
	if( !pFilm ) return false;

	const unsigned int origW   = pFilm->GetWidth();
	const unsigned int origH   = pFilm->GetHeight();
	const double       origPAR = pFilm->GetPixelAR();
	if( origW == 0 || origH == 0 ) return false;

	// Pick the largest scale factor s such that every constraint
	// holds: s*origW <= maxSurfaceW, s*origH <= maxSurfaceH,
	// s*max(origW,origH) <= maxLongEdge.  Capping at 1.0 ensures we
	// only ever shrink — a tiny scene authored at 128 x 128 stays at
	// 128 x 128 and gets stretched by the viewport widget instead of
	// being needlessly re-rendered at a larger resolution.
	const unsigned int longOrig = origW >= origH ? origW : origH;
	const double s = std::min( { 1.0,
	                             double( maxSurfaceW )  / double( origW ),
	                             double( maxSurfaceH )  / double( origH ),
	                             double( maxLongEdge )  / double( longOrig ) } );
	const unsigned int newW = static_cast<unsigned int>( std::max< long long >(
		1, static_cast<long long>( std::round( s * origW ) ) ) );
	const unsigned int newH = static_cast<unsigned int>( std::max< long long >(
		1, static_cast<long long>( std::round( s * origH ) ) ) );

	// SetFilm has its own same-dim short-circuit, but bail early to
	// avoid the GlobalLog noise SetFilm would emit on a successful
	// no-op.
	if( newW == origW && newH == origH ) return true;
	return SetFilm( newW, newH, origPAR );
}


//
// Cameras
//
// Each AddXxxCamera creates the ICamera, registers it under `name` in
// the scene's camera manager (which makes it active by policy), then
// drops the local strong ref.  The manager retains the addref taken
// by AddItem.

// Helper: read xres/yres/pixelAR from the Scene's active Film for the
// camera factories (which still take dims as ctor args for their
// internal Frame projection math).  Single point of truth, cuts
// repetitive `pScene->GetFilm()->GetWidth()` boilerplate in every
// Add*Camera body.  Caller is responsible for SetFilm-ing whatever
// dims they actually want — InitializeContainers installs the qHD
// default so this is always non-null.
void Job::ReadFilmDims( unsigned int& xres, unsigned int& yres, double& pixelAR ) const
{
	const IFilm* pFilm = pScene ? pScene->GetFilm() : nullptr;
	if( pFilm ) {
		xres    = pFilm->GetWidth();
		yres    = pFilm->GetHeight();
		pixelAR = pFilm->GetPixelAR();
	} else {
		xres    = kDefaultFilmWidth;
		yres    = kDefaultFilmHeight;
		pixelAR = kDefaultFilmPixelAR;
	}
}

bool Job::AddPinholeCamera(
	const char* name,
	const double ptLocation[3],
	const double ptLookAt[3],
	const double vUp[3],
	const double fov,
	const double exposure,
	const double scanningRate,
	const double pixelRate,
	const double orientation[3],
	const double target_orientation[2],
	const double iso,
	const double fstop
	)
{
	unsigned int xres, yres; double pixelAR;
	ReadFilmDims( xres, yres, pixelAR );
	ICamera* pCamera = 0;
	RISE_API_CreatePinholeCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation), iso, fstop );
	const bool ok = pScene->AddCamera( name, pCamera );
	safe_release( pCamera );
	return ok;
}

bool Job::AddPinholeCameraONB(
	const char* name,
	const double ONB_U[3],
	const double ONB_V[3],
	const double ONB_W[3],
	const double ptLocation[3],
	const double fov,
	const double exposure,
	const double scanningRate,
	const double pixelRate,
	const double iso,
	const double fstop
	)
{
	unsigned int xres, yres; double pixelAR;
	ReadFilmDims( xres, yres, pixelAR );
	// Phase B2: dims read from Scene's active Film via ReadFilmDims
	// (called above).  Programmatic callers and importers drive Film
	// via SetFilm before AddXxxCamera; the parser does the same via
	// the `film` chunk.
	OrthonormalBasis3D onb = OrthonormalBasis3D( Vector3(ONB_U), Vector3(ONB_V), Vector3(ONB_W) );
	ICamera* pCamera = 0;
	RISE_API_CreatePinholeCameraONB( &pCamera, onb, Point3(ptLocation), fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, iso, fstop );
	const bool ok = pScene->AddCamera( name, pCamera );
	safe_release( pCamera );
	return ok;
}

bool Job::AddThinlensCamera(
	const char* name,
	const double ptLocation[3],
	const double ptLookAt[3],
	const double vUp[3],
	const double sensorSize,
	const double focalLength,
	const double fstop,
	const double focusDistance,
	const double sceneUnitMeters,
	const double exposure,
	const double scanningRate,
	const double pixelRate,
	const double orientation[3],
	const double target_orientation[2],
	const unsigned int apertureBlades,
	const double apertureRotation,
	const double anamorphicSqueeze,
	const double tiltX,
	const double tiltY,
	const double shiftX,
	const double shiftY,
	const double iso
	)
{
	unsigned int xres, yres; double pixelAR;
	ReadFilmDims( xres, yres, pixelAR );
	ICamera* pCamera = 0;
	RISE_API_CreateThinlensCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), sensorSize, focalLength, fstop, focusDistance, sceneUnitMeters, xres, yres, pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation), apertureBlades, apertureRotation, anamorphicSqueeze, tiltX, tiltY, shiftX, shiftY, iso );
	const bool ok = pScene->AddCamera( name, pCamera );
	safe_release( pCamera );
	return ok;
}

bool Job::AddFisheyeCamera(
	const char* name,
	const double ptLocation[3],
	const double ptLookAt[3],
	const double vUp[3],
	const double exposure,
	const double scanningRate,
	const double pixelRate,
	const double orientation[3],
	const double target_orientation[2],
	const double scale
	)
{
	unsigned int xres, yres; double pixelAR;
	ReadFilmDims( xres, yres, pixelAR );
	ICamera* pCamera = 0;
	RISE_API_CreateFisheyeCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), xres, yres, pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation), scale );
	const bool ok = pScene->AddCamera( name, pCamera );
	safe_release( pCamera );
	return ok;
}

bool Job::AddOrthographicCamera(
	const char* name,
	const double ptLocation[3],
	const double ptLookAt[3],
	const double vUp[3],
	const double vpScale[2],
	const double exposure,
	const double scanningRate,
	const double pixelRate,
	const double orientation[3],
	const double target_orientation[2]
	)
{
	unsigned int xres, yres; double pixelAR;
	ReadFilmDims( xres, yres, pixelAR );
	// Phase B2: dims read from Scene's active Film via ReadFilmDims
	// (called above).  Programmatic callers and importers drive Film
	// via SetFilm before AddXxxCamera; the parser does the same via
	// the `film` chunk.
	ICamera* pCamera = 0;
	RISE_API_CreateOrthographicCamera( &pCamera, Point3(ptLocation), Point3(ptLookAt), Vector3(vUp), xres, yres, Vector2(vpScale), pixelAR, exposure, scanningRate, pixelRate, Vector3(orientation), Vector2(target_orientation) );
	const bool ok = pScene->AddCamera( name, pCamera );
	safe_release( pCamera );
	return ok;
}

bool Job::SetActiveCamera( const char* name )
{
	if( !pScene ) return false;
	const bool ok = pScene->SetActiveCamera( name );
	if( ok ) {
		// L6b — when the active camera changes, its dimensions may
		// differ from the previously-active camera's, so the
		// canonical FrameStore needs to be re-resolved (which
		// triggers reallocation in `EnsureJobFrameStore_locked` if
		// dims differ) and pushed to every rasterizer in the
		// registry.  Without this, switching to a camera with
		// different dimensions leaves rasterizers holding the OLD
		// FrameStore + `GetFrameStore()` exposes the stale store.
		PushJobFrameStoreToRasterizers();
	}
	return ok;
}

std::string Job::GetActiveCameraName() const
{
	if( !pScene ) return std::string();
	// IScene::GetActiveCameraName returns a `String` (= std::vector<char>
	// derivative) by value; convert to std::string for IJob's portable
	// signature.  Empty when no camera has ever been added or all of
	// them have been removed.
	const RISE::String s = pScene->GetActiveCameraName();
	return s.empty() ? std::string() : std::string( s.c_str() );
}

bool Job::RemoveCamera( const char* name )
{
	if( !pScene ) return false;
	return pScene->RemoveCamera( name );
}

//
// Adding painters
//


//! Adds a simple checker painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCheckerPainter(
							const char* name,				///< [in] Name of the painter
							const double size,				///< [in] Size of the checkers in texture mapping units
							const char* pa,					///< [in] First painter
							const char* pb					///< [in] Second painter
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateCheckerPainter( &pPainter, size, *pA, *pB );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}



//! Adds a lines painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddLinesPainter(
							const char* name,				///< [in] Name of the painter
							const double size,				///< [in] Size of the lines in texture mapping units
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const bool bvert				///< [in] Are the lines vertical?
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateLinesPainter( &pPainter, size, *pA, *pB, bvert );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a mandelbrot fractal painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddMandelbrotFractalPainter(
							const char* name,				///< [in] Name of the painter
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const double lower_x,
							const double upper_x,
							const double lower_y,
							const double upper_y,
							const double exp
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateMandelbrotFractalPainter( &pPainter, *pA, *pB, lower_x, upper_x, lower_y, upper_y, exp );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a 2D perlin noise painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerlin2DPainter(
							const char* name,				///< [in] Name of the painter
							const double dPersistence,		///< [in] Persistence
							const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const double vScale[2],			///< [in] How much to scale the function by
							const double vShift[2]			///< [in] How much to shift the function by
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}


	IPainter* pPainter = 0;
	RISE_API_CreatePerlin2DPainter( &pPainter, dPersistence, nOctaves, *pA, *pB, Vector2(vScale), Vector2(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a controlled-smoothness radial-bump painter (test/diagnostic).
/// \return TRUE if successful, FALSE otherwise
bool Job::AddControlledSmoothness2DPainter(
							const char* name,
							const char* pa,
							const char* pb,
							const double centerU,
							const double centerV,
							const double radius,
							const double amplitude,
							const unsigned int smoothnessMode
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateControlledSmoothness2DPainter(
		&pPainter, *pA, *pB,
		centerU, centerV, radius, amplitude, smoothnessMode );
	if( !pPainter ) {
		return false;
	}
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a polynomial-based Function2D painter.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPolynomialFunction2DPainter(
							const char* name,
							const char* pa,
							const char* pb,
							const unsigned int polynomialType,
							const double center[2],
							const double scale[2],
							const double amplitude,
							const unsigned int degree,
							const unsigned int powerX,
							const unsigned int powerY,
							const double* pCoeffs,
							const unsigned int nCoeffs
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreatePolynomialFunction2DPainter(
		&pPainter, *pA, *pB,
		polynomialType,
		center[0], center[1],
		scale[0],  scale[1],
		amplitude,
		degree, powerX, powerY,
		pCoeffs, nCoeffs );
	if( !pPainter ) {
		return false;
	}
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a composable Function2D painter that combines two operand
//! Function2Ds per a binary operator.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCompositeFunction2DPainter(
							const char* name,
							const char* pa,
							const char* pb,
							const char* childA,
							const char* childB,
							const unsigned int op,
							const double weightA,
							const double uvScaleA[2],
							const double uvOffsetA[2],
							const double weightB,
							const double uvScaleB[2],
							const double uvOffsetB[2],
							const double lerpT,
							const double outputScale,
							const double outputOffset
							)
{
	IPainter* pColA = pPntManager->GetItem( pa );
	IPainter* pColB = pPntManager->GetItem( pb );

	if( !pColA || !pColB ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"Job::AddCompositeFunction2DPainter '%s': color painter lookup failed (colora='%s', colorb='%s')",
			name, pa, pb );
		return false;
	}

	// Children must already be registered as Function2Ds — i.e. a painter
	// whose implementation derives from `Painter` (which exposes the
	// IFunction2D Evaluate hook).  pPntManager + pFunc2DManager are kept
	// in lockstep by the Add*Painter helpers above, so a failure here
	// means the named painter doesn't actually evaluate as a Function2D.
	IFunction2D* pChildA = pFunc2DManager->GetItem( childA );
	IFunction2D* pChildB = pFunc2DManager->GetItem( childB );

	if( !pChildA || !pChildB ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"Job::AddCompositeFunction2DPainter '%s': child Function2D lookup failed (child_a='%s', child_b='%s'); both children must be Function2D-implementing painters (Perlin2D, Gerstner, ControlledSmoothness2D, ConstantFunction2D, or another composite)",
			name, childA, childB );
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateCompositeFunction2DPainter(
		&pPainter,
		*pColA, *pColB,
		*pChildA, *pChildB,
		op,
		weightA,
		uvScaleA[0], uvScaleA[1], uvOffsetA[0], uvOffsetA[1],
		weightB,
		uvScaleB[0], uvScaleB[1], uvOffsetB[0], uvOffsetB[1],
		lerpT,
		outputScale,
		outputOffset );
	if( !pPainter ) {
		return false;
	}
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a sum-of-sines water-wave painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddGerstnerWavePainter(
							const char* name,
							const char* pa,
							const char* pb,
							const unsigned int numWaves,
							const double medianWavelength,
							const double wavelengthRange,
							const double medianAmplitude,
							const double amplitudePower,
							const double windDir[2],
							const double directionalSpread,
							const double dispersionSpeed,
							const unsigned int seed,
							const double time
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateGerstnerWavePainter(
		&pPainter,
		*pA, *pB,
		numWaves,
		medianWavelength, wavelengthRange,
		medianAmplitude, amplitudePower,
		windDir[0], windDir[1],
		directionalSpread,
		dispersionSpeed,
		seed,
		time );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a 2D perlin noise painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerlin3DPainter(
								const char* name,				///< [in] Name of the painter
								const double dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const char* pa,					///< [in] First painter
								const char* pb,					///< [in] Second painter
								const double vScale[3],			///< [in] How much to scale the function by
								const double vShift[3]			///< [in] How much to shift the function by
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreatePerlin3DPainter( &pPainter, dPersistence, nOctaves, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddWavelet3DPainter(
								const char* name,
								const unsigned int nTileSize,
								const double dPersistence,
								const unsigned int nOctaves,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );
	if( !pA || !pB ) return false;

	IPainter* pPainter = 0;
	RISE_API_CreateWavelet3DPainter( &pPainter, nTileSize, dPersistence, nOctaves, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddReactionDiffusion3DPainter(
								const char* name,
								const unsigned int nGridSize,
								const double dDa,
								const double dDb,
								const double dFeed,
								const double dKill,
								const unsigned int nIterations,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );
	if( !pA || !pB ) return false;

	IPainter* pPainter = 0;
	RISE_API_CreateReactionDiffusion3DPainter( &pPainter, nGridSize, dDa, dDb, dFeed, dKill, nIterations, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddGabor3DPainter(
								const char* name,
								const double dFrequency,
								const double dBandwidth,
								const double vOrientation[3],
								const double dImpulseDensity,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateGabor3DPainter( &pPainter, dFrequency, dBandwidth, Vector3(vOrientation), dImpulseDensity, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddSimplex3DPainter(
								const char* name,
								const double dPersistence,
								const unsigned int nOctaves,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateSimplex3DPainter( &pPainter, dPersistence, nOctaves, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddSDF3DPainter(
								const char* name,
								const unsigned int nType,
								const double dParam1,
								const double dParam2,
								const double dParam3,
								const double dShellThickness,
								const double dNoiseAmplitude,
								const double dNoiseFrequency,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateSDF3DPainter( &pPainter, nType, dParam1, dParam2, dParam3, dShellThickness, dNoiseAmplitude, dNoiseFrequency, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddCurlNoise3DPainter(
								const char* name,
								const double dPersistence,
								const unsigned int nOctaves,
								const double dEpsilon,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateCurlNoise3DPainter( &pPainter, dPersistence, nOctaves, dEpsilon, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddDomainWarp3DPainter(
								const char* name,
								const double dPersistence,
								const unsigned int nOctaves,
								const double dWarpAmplitude,
								const unsigned int nWarpLevels,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateDomainWarp3DPainter( &pPainter, dPersistence, nOctaves, dWarpAmplitude, nWarpLevels, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddPerlinWorley3DPainter(
								const char* name,
								const double dPersistence,
								const unsigned int nOctaves,
								const double dWorleyJitter,
								const double dBlend,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreatePerlinWorley3DPainter( &pPainter, dPersistence, nOctaves, dWorleyJitter, dBlend, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a 3D Worley (cellular) noise painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddWorley3DPainter(
								const char* name,
								const double dJitter,
								const unsigned int nMetric,
								const unsigned int nOutput,
								const char* pa,
								const char* pb,
								const double vScale[3],
								const double vShift[3]
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateWorley3DPainter( &pPainter, dJitter, nMetric, nOutput, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a 3D turbulence noise painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTurbulence3DPainter(
								const char* name,				///< [in] Name of the painter
								const double dPersistence,		///< [in] Persistence
								const unsigned int nOctaves,	///< [in] Number of octaves to use in noise generation
								const char* pa,					///< [in] First painter
								const char* pb,					///< [in] Second painter
								const double vScale[3],			///< [in] How much to scale the function by
								const double vShift[3]			///< [in] How much to shift the function by
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateTurbulence3DPainter( &pPainter, dPersistence, nOctaves, *pA, *pB, Vector3(vScale), Vector3(vShift) );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a spectral color painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSpectralColorPainter(
							const char* name,				///< [in] Name of the painter
							const double* amplitudes,		///< [in] Array that contains the amplitudes
							const double* frequencies,		///< [in] Array that contains the frequencies for the amplitudes
							const double lambda_begin,		///< [in] Begining of the spectral packet
							const double lambda_end,		///< [in] End of the spectral packet
							const unsigned int numfreq,		///< [in] Number of frequencies in the array
							const double scale				///< [in] How much to scale the amplitudes by
							)
{
	IPiecewiseFunction1D* pFunc = 0;
	RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );

	for( unsigned int i=0; i<numfreq; i++ ) {
		pFunc->addControlPoint( std::make_pair( frequencies[i], amplitudes[i] ) );
	}

	SpectralPacket spectrum =	SpectralPacket( lambda_begin, lambda_end, numfreq, pFunc );

	IPainter* pPainter = 0;
	RISE_API_CreateSpectralColorPainter( &pPainter, spectrum, scale );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	safe_release( pFunc );
	return true;
}

static IRasterImageAccessor* RasterImageAccessorFromChar( const char filter_type, IRasterImage& image,
	const char wrap_s = eRasterWrap_ClampToEdge, const char wrap_t = eRasterWrap_ClampToEdge,
	const bool mipmap = true,	// Landing 2: opt out by passing false for vector-quantity textures (normal maps)
	const bool supersample = false )	// Landing 2: footprint stochastic supersampling (lowmem-friendly LOD)
{
	IRasterImageAccessor* pRIA = 0;

	switch( filter_type )
	{
	case 0:
		RISE_API_CreateNNBRasterImageAccessor( &pRIA, image, wrap_s, wrap_t );
		break;
	default:
		GlobalLog()->PrintEasyWarning( "Unknown texture filter type, using bilinear" );
	case 1:
		RISE_API_CreateBiLinRasterImageAccessor( &pRIA, image, wrap_s, wrap_t, mipmap, supersample );
		break;
	case 2:
		RISE_API_CreateCatmullRomBicubicRasterImageAccessor( &pRIA, image, wrap_s, wrap_t );
		break;
	case 3:
		RISE_API_CreateUniformBSplineBicubicRasterImageAccessor( &pRIA, image, wrap_s, wrap_t );
		break;

	};

	return pRIA;
}

//! Adds a texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPNGTexturePainter(
							const char* name,
							const char* filename,
							const char color_space,
							const char filter_type,
							const bool lowmemory,
							const double scale[3],
							const double shift[3],
							const char wrap_s,
							const char wrap_t,
							const bool mipmap,	// Landing 2: build mip pyramid + LOD-aware sampling
							const SpectrumKind spectrumKind	// Landing 3: spectral-uplift role
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IReadBuffer* pReadBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pReadBuffer, filename );

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreatePNGReader( &pImageReader, *pReadBuffer, gc );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	// Landing 2: when both mipmap is requested AND lowmem is set,
	// route to footprint stochastic supersampling instead of building
	// a pyramid (which would defeat lowmem's memory budget).  See
	// docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md for the full
	// trade-off analysis.
	const bool effective_mipmap     = mipmap && !lowmemory;
	const bool effective_supersample = mipmap && lowmemory;
	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage, wrap_s, wrap_t, effective_mipmap, effective_supersample );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA, spectrumKind );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a PNG texture painter from an in-memory byte buffer (e.g., a
//! glTF .glb embedded image bufferView).  Behaves identically to
//! AddPNGTexturePainter except the byte source is supplied directly,
//! avoiding the disk round-trip the importer used to make.  The byte
//! buffer is consumed during decode and may be freed by the caller
//! once this call returns.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddInMemoryPNGTexturePainter(
							const char* name,
							const unsigned char* bytes,
							const size_t numBytes,
							const char color_space,
							const char filter_type,
							const bool lowmemory,
							const double scale[3],
							const double shift[3],
							const char wrap_s,
							const char wrap_t,
							const bool mipmap,
							const SpectrumKind spectrumKind
							)
{
	if( !name || !bytes || numBytes == 0 ) {
		return false;
	}

	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	// Wrap the caller's bytes in an IMemoryBuffer (which is-a IReadBuffer)
	// without taking ownership.  PNGReader copies pixel data into the
	// IRasterImage during LoadImage, so the caller's bytes only need to
	// live for the duration of this function.
	IMemoryBuffer* pMemBuffer = 0;
	RISE_API_CreateCompatibleMemoryBuffer( &pMemBuffer,
		const_cast<char*>( reinterpret_cast<const char*>( bytes ) ),
		static_cast<unsigned int>( numBytes ), false );
	if( !pMemBuffer ) {
		safe_release( pImage );
		return false;
	}

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 0: gc = eColorSpace_Rec709RGB_Linear; break;
	case 1: gc = eColorSpace_sRGB;             break;
	case 2: gc = eColorSpace_ROMMRGB_Linear;   break;
	case 3: gc = eColorSpace_ProPhotoRGB;      break;
	}

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreatePNGReader( &pImageReader, *pMemBuffer, gc );
	pImage->LoadImage( pImageReader );

	// LoadImage doesn't propagate a hard failure for malformed PNG bytes;
	// instead the image is left empty (0 x 0).  Reject here so we don't
	// silently register a working-but-blank painter.
	if( pImage->GetWidth() == 0 || pImage->GetHeight() == 0 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddInMemoryPNGTexturePainter:: PNG decode failed for `%s` "
			"(0x%zu bytes)", name, numBytes );
		safe_release( pImageReader );
		safe_release( pMemBuffer );
		safe_release( pImage );
		return false;
	}

	if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
		IOneColorOperator* pOp = 0;
		RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
		Apply1ColorOperator( *pImage, *pOp );
		safe_release( pOp );
	}
	if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
		IOneColorOperator* pOp = 0;
		RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
		Apply1ColorOperator( *pImage, *pOp );
		safe_release( pOp );
	}

	// Landing 2: when both mipmap is requested AND lowmem is set,
	// route to footprint stochastic supersampling instead of building
	// a pyramid (which would defeat lowmem's memory budget).  See
	// docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md for the full
	// trade-off analysis.
	const bool effective_mipmap     = mipmap && !lowmemory;
	const bool effective_supersample = mipmap && lowmemory;
	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage, wrap_s, wrap_t, effective_mipmap, effective_supersample );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA, spectrumKind );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pMemBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a JPEG texture painter from an in-memory byte buffer.  See
//! AddInMemoryPNGTexturePainter for the rationale and lifetime contract.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddInMemoryJPEGTexturePainter(
							const char* name,
							const unsigned char* bytes,
							const size_t numBytes,
							const char color_space,
							const char filter_type,
							const bool lowmemory,
							const double scale[3],
							const double shift[3],
							const char wrap_s,
							const char wrap_t,
							const bool mipmap,
							const SpectrumKind spectrumKind
							)
{
	if( !name || !bytes || numBytes == 0 ) {
		return false;
	}

	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IMemoryBuffer* pMemBuffer = 0;
	RISE_API_CreateCompatibleMemoryBuffer( &pMemBuffer,
		const_cast<char*>( reinterpret_cast<const char*>( bytes ) ),
		static_cast<unsigned int>( numBytes ), false );
	if( !pMemBuffer ) {
		safe_release( pImage );
		return false;
	}

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 0: gc = eColorSpace_Rec709RGB_Linear; break;
	case 1: gc = eColorSpace_sRGB;             break;
	case 2: gc = eColorSpace_ROMMRGB_Linear;   break;
	case 3: gc = eColorSpace_ProPhotoRGB;      break;
	}

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateJPEGReader( &pImageReader, *pMemBuffer, gc );
	pImage->LoadImage( pImageReader );

	if( pImage->GetWidth() == 0 || pImage->GetHeight() == 0 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddInMemoryJPEGTexturePainter:: JPEG decode failed for `%s` "
			"(0x%zu bytes)", name, numBytes );
		safe_release( pImageReader );
		safe_release( pMemBuffer );
		safe_release( pImage );
		return false;
	}

	if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
		IOneColorOperator* pOp = 0;
		RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
		Apply1ColorOperator( *pImage, *pOp );
		safe_release( pOp );
	}
	if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
		IOneColorOperator* pOp = 0;
		RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
		Apply1ColorOperator( *pImage, *pOp );
		safe_release( pOp );
	}

	// Landing 2: when both mipmap is requested AND lowmem is set,
	// route to footprint stochastic supersampling instead of building
	// a pyramid (which would defeat lowmem's memory budget).  See
	// docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md for the full
	// trade-off analysis.
	const bool effective_mipmap     = mipmap && !lowmemory;
	const bool effective_supersample = mipmap && lowmemory;
	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage, wrap_s, wrap_t, effective_mipmap, effective_supersample );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA, spectrumKind );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pMemBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a JPEG texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddJPEGTexturePainter(
							const char* name,
							const char* filename,
							const char color_space,
							const char filter_type,
							const bool lowmemory,
							const double scale[3],
							const double shift[3],
							const char wrap_s,
							const char wrap_t,
							const bool mipmap,	// Landing 2: build mip pyramid + LOD-aware sampling
							const SpectrumKind spectrumKind	// Landing 3: spectral-uplift role
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IReadBuffer* pReadBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pReadBuffer, filename );

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateJPEGReader( &pImageReader, *pReadBuffer, gc );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	// Landing 2: when both mipmap is requested AND lowmem is set,
	// route to footprint stochastic supersampling instead of building
	// a pyramid (which would defeat lowmem's memory budget).  See
	// docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_2.md for the full
	// trade-off analysis.
	const bool effective_mipmap     = mipmap && !lowmemory;
	const bool effective_supersample = mipmap && lowmemory;
	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage, wrap_s, wrap_t, effective_mipmap, effective_supersample );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA, spectrumKind );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Decodes N PNG/JPEG texture painters in parallel via the global
//! ThreadPool, then registers each with the painter / function-2D
//! managers serially in this thread.  See IJob.h for the full contract.
bool Job::AddTexturePaintersBatch(
								const TexturePainterBatchRequest* requests,
								size_t numRequests,
								bool* outRequestSuccess
								)
{
	if( !requests || numRequests == 0 ) {
		return true;
	}

	// Each request decodes into its own IRasterImage* + IRasterImageAccessor*
	// + IPainter*, fully independent of the other workers' state.  All three
	// stay alive until after the manager AddItem step, then release in
	// reverse order.
	struct Decoded
	{
		IPainter*               pPainter;     // null on decode failure
		IRasterImageAccessor*   pRIA;
		IRasterImage*           pImage;
	};
	std::vector<Decoded> decoded( numRequests, Decoded{ nullptr, nullptr, nullptr } );

	// Parallel decode.  Worker bodies touch only their own slot of
	// `decoded[]` and the constant `requests[i]`; no shared mutable state.
	// Each worker:
	//   1. Allocates an IRasterImage (low-mem variant defers color convert).
	//   2. Wraps the source bytes (file or in-memory) in an IReadBuffer.
	//   3. Creates a PNG or JPEG reader bound to that buffer + the requested
	//      colour space.
	//   4. LoadImage (the heavy libpng/libjpeg work).
	//   5. Optionally applies scale/shift colour ops on the decoded pixels.
	//   6. Builds the IRasterImageAccessor and IPainter for the decoded image.
	//   7. Stores the painter ptr in decoded[i].pPainter; null on failure.
	Implementation::GlobalThreadPool().ParallelFor(
		static_cast<unsigned int>( numRequests ),
		[&]( unsigned int i )
		{
			const TexturePainterBatchRequest& req = requests[i];

			// Argument validation — must be exactly one of (filePath, bytes).
			const bool hasFile  = ( req.filePath  != NULL );
			const bool hasBytes = ( req.bytes     != NULL && req.numBytes > 0 );
			if( hasFile == hasBytes ) {
				GlobalLog()->PrintEx( eLog_Error,
					"Job::AddTexturePaintersBatch:: request `%s` must set exactly "
					"one of filePath or bytes (got %s)",
					req.name ? req.name : "(null)",
					( hasFile && hasBytes ) ? "both" : "neither" );
				return;
			}
			if( !req.name ) {
				GlobalLog()->PrintEasyError(
					"Job::AddTexturePaintersBatch:: request has NULL name; skipping" );
				return;
			}

			IRasterImage* pImage = 0;
			if( req.lowmemory ) {
				RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
			} else {
				RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor( 0, 0, 0, 0 ) );
			}
			if( !pImage ) {
				return;
			}

			// Source buffer.  In-memory path uses an IMemoryBuffer with non-
			// owning pointer (.glb embedded images); on-disk path opens its
			// own file handle (.gltf JSON-form sidecar).  Both produce a
			// buffer the PNG/JPEG readers consume identically.
			IReadBuffer* pReadBuffer = 0;
			if( hasFile ) {
				RISE_API_CreateDiskFileReadBuffer( &pReadBuffer, req.filePath );
			} else {
				IMemoryBuffer* pMemBuffer = 0;
				RISE_API_CreateCompatibleMemoryBuffer( &pMemBuffer,
					const_cast<char*>( reinterpret_cast<const char*>( req.bytes ) ),
					static_cast<unsigned int>( req.numBytes ), false );
				pReadBuffer = pMemBuffer;	// IMemoryBuffer is-a IReadBuffer
			}
			if( !pReadBuffer ) {
				safe_release( pImage );
				return;
			}

			COLOR_SPACE gc = eColorSpace_sRGB;
			switch( req.colorSpace )
			{
			case 0: gc = eColorSpace_Rec709RGB_Linear; break;
			case 1: gc = eColorSpace_sRGB;             break;
			case 2: gc = eColorSpace_ROMMRGB_Linear;   break;
			case 3: gc = eColorSpace_ProPhotoRGB;      break;
			}

			IRasterImageReader* pImageReader = 0;
			if( req.format == 0 ) {
				RISE_API_CreatePNGReader( &pImageReader, *pReadBuffer, gc );
			} else if( req.format == 1 ) {
				RISE_API_CreateJPEGReader( &pImageReader, *pReadBuffer, gc );
			} else {
				GlobalLog()->PrintEx( eLog_Error,
					"Job::AddTexturePaintersBatch:: request `%s` has unknown format byte %d "
					"(expected 0=PNG, 1=JPEG)", req.name, (int)req.format );
				safe_release( pReadBuffer );
				safe_release( pImage );
				return;
			}
			if( !pImageReader ) {
				safe_release( pReadBuffer );
				safe_release( pImage );
				return;
			}

			pImage->LoadImage( pImageReader );

			// LoadImage doesn't propagate a hard error for malformed bytes;
			// it leaves the image at 0x0.  Reject so we don't register an
			// empty painter.
			if( pImage->GetWidth() == 0 || pImage->GetHeight() == 0 ) {
				GlobalLog()->PrintEx( eLog_Error,
					"Job::AddTexturePaintersBatch:: image decode failed for `%s`",
					req.name );
				safe_release( pImageReader );
				safe_release( pReadBuffer );
				safe_release( pImage );
				return;
			}

			if( req.scale[0] != 1 || req.scale[1] != 1 || req.scale[2] != 1 ) {
				IOneColorOperator* pOp = 0;
				RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel( req.scale ), 1.0 ) );
				Apply1ColorOperator( *pImage, *pOp );
				safe_release( pOp );
			}
			if( req.shift[0] != 0 || req.shift[1] != 0 || req.shift[2] != 0 ) {
				IOneColorOperator* pOp = 0;
				RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel( req.shift ), 0 ) );
				Apply1ColorOperator( *pImage, *pOp );
				safe_release( pOp );
			}

			// Landing 2: same lowmem composition as the per-painter path.
			const bool effective_mipmap     = req.mipmap && !req.lowmemory;
			const bool effective_supersample = req.mipmap && req.lowmemory;
			IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( req.filterType, *pImage, req.wrap_s, req.wrap_t, effective_mipmap, effective_supersample );

			IPainter* pPainter = 0;
			RISE_API_CreateTexturePainter( &pPainter, pRIA, req.spectrumKind );

			// Worker done.  pPainter, pRIA, pImage all outlive the manager
			// AddItem step (which holds its own ref); the reader and read-
			// buffer are no longer needed once decode is complete, so release
			// them here.
			safe_release( pImageReader );
			safe_release( pReadBuffer );

			decoded[i].pPainter = pPainter;
			decoded[i].pRIA     = pRIA;
			decoded[i].pImage   = pImage;
		} );

	// Serial registration.  pPntManager / pFunc2DManager are not
	// thread-safe (std::map insert under the hood); only the calling
	// thread touches them here.  Per-request success requires BOTH
	// decode AND manager registration to have succeeded — a duplicate
	// name causes pPntManager->AddItem to refuse the addref, so the
	// painter is never reachable by name even though the decode worked.
	// Report both failure modes as `outRequestSuccess[i] = false`;
	// callers that memoize "this painter is registered" use that flag
	// to decide whether to fast-path future lookups vs fall back to a
	// per-call decode (or a uniform-color sentinel).
	bool allOk = true;
	for( size_t i = 0; i < numRequests; ++i ) {
		Decoded& d = decoded[i];
		bool requestOk = false;
		if( d.pPainter ) {
			// AddItem returns true on success; both managers should
			// agree (same name, same painter, same outcome) but we
			// require the painter manager — the function-2D manager
			// is a secondary index that downstream lookups don't use
			// for material assembly.
			const bool pntOk = pPntManager->AddItem( d.pPainter, requests[i].name );
			pFunc2DManager->AddItem( d.pPainter, requests[i].name );
			requestOk = pntOk;
			safe_release( d.pPainter );
			safe_release( d.pRIA );
			safe_release( d.pImage );
		}
		if( outRequestSuccess ) {
			outRequestSuccess[i] = requestOk;
		}
		if( !requestOk ) {
			allOk = false;
		}
	}
	return allOk;
}

//! Adds a texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddHDRTexturePainter(
							const char* name,				///< [in] Name of the painter
							const char* filename,			///< [in] Name of the file that contains the texture
							const char filter_type,			///< [in] Type of texture filtering
																///     0 - Nearest neighbour
																///     1 - Bilinear
																///     2 - Catmull Rom Bicubic
																///     3 - Uniform BSpline Bicubic
							const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
							const double scale[3],			///< [in] Scale factor for color values
							const double shift[3],			///< [in] Shift factor for color values
							const char wrap_s,				///< [in] U-axis wrap mode (see IJob.h / eRasterWrapMode)
							const char wrap_t,				///< [in] V-axis wrap mode
							const SpectrumKind spectrumKind	///< [in] Spectral-uplift role (default Unbounded for HDR)
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IMemoryBuffer* pReadBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pReadBuffer, filename );

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateHDRReader( &pImageReader, *pReadBuffer );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage, wrap_s, wrap_t );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA, spectrumKind );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds an EXR texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddEXRTexturePainter(
							const char* name,				///< [in] Name of the painter
							const char* filename,			///< [in] Name of the file that contains the texture
							const char color_space,			///< [in] Color space in the file
															///		0 - Rec709 RGB Linear
															///		1 - sRGB profile
															///		2 - ROMM RGB (ProPhotoRGB) linear
															///		3 - ROMM RGB (ProPhotoRGB) non-linear
							const char filter_type,			///< [in] Type of texture filtering
															///     0 - Nearest neighbour
															///     1 - Bilinear
															///     2 - Catmull Rom Bicubic
															///     3 - Uniform BSpline Bicubic
							const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
							const double scale[3],			///< [in] Scale factor for color values
							const double shift[3],			///< [in] Shift factor for color values
							const char wrap_s,				///< [in] U-axis wrap mode (see IJob.h / eRasterWrapMode)
							const char wrap_t,				///< [in] V-axis wrap mode
							const SpectrumKind spectrumKind	///< [in] Spectral-uplift role (default Unbounded for EXR)
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IMemoryBuffer* pReadBuffer = 0;
	RISE_API_CreateMemoryBufferFromFile( &pReadBuffer, filename );

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateEXRReader( &pImageReader, *pReadBuffer, gc );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage, wrap_s, wrap_t );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA, spectrumKind );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a texture painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTIFFTexturePainter(
							const char* name,				///< [in] Name of the painter
							const char* filename,			///< [in] Name of the file that contains the texture
							const char color_space,			///< [in] Color space in the file
															///		0 - Rec709 RGB Linear
															///		1 - sRGB profile
															///		2 - ROMM RGB (ProPhotoRGB) linear
															///		3 - ROMM RGB (ProPhotoRGB) non-linear
							const char filter_type,			///< [in] Type of texture filtering
																///     0 - Nearest neighbour
																///     1 - Bilinear
																///     2 - Catmull Rom Bicubic
																///     3 - Uniform BSpline Bicubic
							const bool lowmemory,			///< [in] low memory mode doesn't do an image convert
							const double scale[3],			///< [in] Scale factor for color values
							const double shift[3],			///< [in] Shift factor for color values
							const char wrap_s,				///< [in] U-axis wrap mode (see IJob.h / eRasterWrapMode)
							const char wrap_t				///< [in] V-axis wrap mode
							)
{
	IRasterImage* pImage = 0;
	if( lowmemory ) {
		RISE_API_CreateReadOnlyRISEColorRasterImage( &pImage );
	} else {
		RISE_API_CreateRISEColorRasterImage( &pImage, 0, 0, RISEColor(0,0,0,0) );
	}

	IReadBuffer* pReadBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pReadBuffer, filename );

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	IRasterImageReader* pImageReader = 0;
	RISE_API_CreateTIFFReader( &pImageReader, *pReadBuffer, gc );

	pImage->LoadImage( pImageReader );

	// Apply the scale and/or shift operators
	{
		if( scale[0] != 1 || scale[1] != 1 || scale[2] != 1 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateScaleColorOperatorRasterImage( &pOp, RISEColor( RISEPel(scale), 1.0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}

		if( shift[0] != 0 || shift[1] != 0 || shift[2] != 0 ) {
			IOneColorOperator* pOp = 0;
			RISE_API_CreateShiftColorOperatorRasterImage( &pOp, RISEColor( RISEPel(shift), 0 ) );
			Apply1ColorOperator( *pImage, *pOp );
			safe_release( pOp );
		}
	}

	IRasterImageAccessor* pRIA = RasterImageAccessorFromChar( filter_type, *pImage, wrap_s, wrap_t );

	IPainter* pPainter = 0;
	RISE_API_CreateTexturePainter( &pPainter, pRIA );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	safe_release( pRIA );
	safe_release( pImageReader );
	safe_release( pReadBuffer );
	safe_release( pImage );

	return true;
}

//! Adds a painter that paints a uniform color
/// \return TRUE if successful, FALSE otherwise
bool Job::AddUniformColorPainter(
							const char* name,				///< [in] Name of the painter
							const double pel[3],			///< [in] Color to paint
							const char* cspace				///< [in] Color space of the given color
							)
{
	IPainter* pPainter = 0;
	if( cspace )
	{
		// Then a type of color is specified
		if( strcmp( cspace, "Rec709RGB_Linear" ) == 0 ) {
			RISE_API_CreateUniformColorPainter( &pPainter, Rec709RGBPel(pel) );
		} else if ( strcmp( cspace, "sRGB" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, sRGBPel(pel) );
		} else if ( strcmp( cspace, "ROMMRGB_Linear" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, ROMMRGBPel(pel) );
		} else if ( strcmp( cspace, "ProPhotoRGB" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, ProPhotoRGBPel(pel) );
		} else if ( strcmp( cspace, "RISERGB" ) == 0  ) {
			RISE_API_CreateUniformColorPainter( &pPainter, RISEPel(pel) );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Unknown color space: %s", cspace );
			return false;
		}
	} else {	// we assume SRGB values by default
		RISE_API_CreateUniformColorPainter( &pPainter, sRGBPel(pel) );
	}

	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddVertexColorPainter(
							const char* name,
							const double fallback[3],
							const char* cspace
							)
{
	IPainter* pPainter = 0;
	if( cspace ) {
		if( strcmp( cspace, "Rec709RGB_Linear" ) == 0 ) {
			RISE_API_CreateVertexColorPainter( &pPainter, Rec709RGBPel(fallback) );
		} else if ( strcmp( cspace, "sRGB" ) == 0  ) {
			RISE_API_CreateVertexColorPainter( &pPainter, sRGBPel(fallback) );
		} else if ( strcmp( cspace, "ROMMRGB_Linear" ) == 0  ) {
			RISE_API_CreateVertexColorPainter( &pPainter, ROMMRGBPel(fallback) );
		} else if ( strcmp( cspace, "ProPhotoRGB" ) == 0  ) {
			RISE_API_CreateVertexColorPainter( &pPainter, ProPhotoRGBPel(fallback) );
		} else if ( strcmp( cspace, "RISERGB" ) == 0  ) {
			RISE_API_CreateVertexColorPainter( &pPainter, RISEPel(fallback) );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Unknown color space: %s", cspace );
			return false;
		}
	} else {
		// Default: treat fallback as sRGB (matches AddUniformColorPainter).
		RISE_API_CreateVertexColorPainter( &pPainter, sRGBPel(fallback) );
	}

	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a painter that paints a voronoi diagram
/// \return TRUE if successful, FALSE otherwise
bool Job::AddVoronoi2DPainter(
							const char* name,				///< [in] Name of the painter
							const double pt_x[],			///< [in] X co-ordinates of generators
							const double pt_y[],			///< [in] Y co-ordinates of generators
							const char** painters,			///< [in] The painters for each generator
							const unsigned int count,		///< [in] Number of the generators
							const char* border,				///< [in] Name of the painter for the border
							const double bsize				///< [in] Size of the border
							)
{
	if( count < 2 ) {
		return false;
	}

	IPainter* pBorder = pPntManager->GetItem( border );

	if( !pBorder ) {
		return false;
	}

	std::vector<Point2> pts;
	std::vector<IPainter*> ptrs;

	for( unsigned int i=0; i<count; i++ ) {
		pts.push_back( Point2( pt_x[i], pt_y[i] ) );
		ptrs.push_back( pPntManager->GetItem( painters[i] ) );
	}

	IPainter* pPainter = 0;
	RISE_API_CreateVoronoi2DPainter( &pPainter, pts, ptrs, *pBorder, bsize );

	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	return true;
}

//! Adds a painter that paints a voronoi diagram in 3D
/// \return TRUE if successful, FALSE otherwise
bool Job::AddVoronoi3DPainter(
							const char* name,				///< [in] Name of the painter
							const double pt_x[],			///< [in] X co-ordinates of generators
							const double pt_y[],			///< [in] Y co-ordinates of generators
							const double pt_z[],			///< [in] Z co-ordinates of generators
							const char** painters,			///< [in] The painters for each generator
							const unsigned int count,		///< [in] Number of the generators
							const char* border,				///< [in] Name of the painter for the border
							const double bsize				///< [in] Size of the border
							)
{
	if( count < 2 ) {
		return false;
	}

	IPainter* pBorder = pPntManager->GetItem( border );

	if( !pBorder ) {
		return false;
	}

	std::vector<Point3> pts;
	std::vector<IPainter*> ptrs;

	for( unsigned int i=0; i<count; i++ ) {
		pts.push_back( Point3( pt_x[i], pt_y[i], pt_z[i] ) );
		ptrs.push_back( pPntManager->GetItem( painters[i] ) );
	}

	IPainter* pPainter = 0;
	RISE_API_CreateVoronoi3DPainter( &pPainter, pts, ptrs, *pBorder, bsize );

	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );

	return true;
}

//! Adds a iridescent painter (a painter whose color changes as viewing angle changes)
/// \return TRUE if successful, FALSE otherwise
bool Job::AddIridescentPainter(
							const char* name,				///< [in] Name of the painter
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const double bias				///< [in] Biases the iridescence to one color or another
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );

	if( !pA || !pB ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateIridescentPainter( &pPainter, *pA, *pB, bias );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Creates a black body radiator painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBlackBodyPainter(
							const char* name,				///< [in] Name of the painter
							const double temperature,		///< [in] Temperature of the radiator in Kelvins
							const double lambda_begin,		///< [in] Where in the spectrum to start creating the spectral packet
							const double lambda_end,		///< [in] Where in the spectrum to end creating the spectral packet
							const unsigned int num_freq,	///< [in] Number of frequencies to use in the spectral packet
							const bool normalize,			///< [in] Should the values be normalized to peak intensity?
							const double scale				///< [in] Value to scale radiant exitance by
							)
{
	IPainter* pPainter = 0;
	RISE_API_CreateBlackBodyPainter( &pPainter, temperature, lambda_begin, lambda_end, num_freq, normalize, scale );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a channel-extraction painter.  See IJob.h for the doc.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddChannelPainter(
							const char* name,
							const char* source,
							const char  channel,
							const double scale,
							const double bias
							)
{
	IPainter* pSrc = pPntManager->GetItem( source );
	if( !pSrc ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddChannelPainter:: source painter `%s` not found", source );
		return false;
	}
	if( channel < 0 || channel > 3 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddChannelPainter:: channel %d out of range (0=R, 1=G, 2=B, 3=A)", (int)channel );
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateChannelPainter( &pPainter, *pSrc, channel, scale, bias );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a TEXCOORD_1 selector painter.  See IJob.h for the doc.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTexCoord1Painter(
							const char* name,
							const char* source
							)
{
	IPainter* pSrc = pPntManager->GetItem( source );
	if( !pSrc ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddTexCoord1Painter:: source painter `%s` not found", source );
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateTexCoord1Painter( &pPainter, *pSrc );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::SetGlobalRadianceMap( IRadianceMap* pRm )
{
	if( !pRm ) {
		return false;
	}
	if( !pScene ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::SetGlobalRadianceMap:: no scene yet" );
		return false;
	}
	pScene->SetGlobalRadianceMap( pRm );
	BumpSceneLightGen( pScene );   // P2a: environment replaced -> env sampler stale
	return true;
}

bool Job::AddHosekWilkieSkylight(
	const double solarElevationDegrees,
	const double solarAzimuthDegrees,
	const double turbidity,
	const double groundAlbedo[3],
	const double skyIntensityScale,
	const double sunIntensityScale,
	const bool   createSun )
{
	IRadianceMap* pRm = 0;
	RISE_API_CreateHosekWilkieRadianceMap(
		&pRm,
		solarElevationDegrees, solarAzimuthDegrees, turbidity,
		RISEPel( groundAlbedo ), skyIntensityScale );
	if( !pRm ) {
		return false;
	}
	SetGlobalRadianceMap( pRm );

	if( createSun ) {
		// RISE convention: direction is FROM surface TO light.
		const double el = solarElevationDegrees * (M_PI / 180.0);
		const double az = solarAzimuthDegrees   * (M_PI / 180.0);
		double dir[3] = {
			std::cos(el) * std::sin(az),
			std::sin(el),
			std::cos(el) * std::cos(az)
		};
		double color[3] = { 1.0, 1.0, 1.0 };
		AddDirectionalLight( "__hw_sun__", sunIntensityScale, color, dir );
	}

	pRm->release();
	return true;
}

//! Adds a UV-transform wrapper painter.  See IJob.h for the doc.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddUVTransformPainter(
							const char* name,
							const char* source,
							const double offset_u,
							const double offset_v,
							const double rotation,
							const double scale_u,
							const double scale_v
							)
{
	IPainter* pSrc = pPntManager->GetItem( source );
	if( !pSrc ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddUVTransformPainter:: source painter `%s` not found", source );
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateUVTransformPainter( &pPainter, *pSrc, offset_u, offset_v, rotation, scale_u, scale_v );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//! Adds a blend painter
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBlendPainter(
							const char* name,				///< [in] Name of the painter
							const char* pa,					///< [in] First painter
							const char* pb,					///< [in] Second painter
							const char* mask				///< [in] Mask painter
							)
{
	IPainter* pA = pPntManager->GetItem( pa );
	IPainter* pB = pPntManager->GetItem( pb );
	IPainter* pMask = pPntManager->GetItem( mask );

	if( !pA || !pB || !pMask ) {
		return false;
	}

	IPainter* pPainter = 0;
	RISE_API_CreateBlendPainter( &pPainter, *pA, *pB, *pMask );
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

//
// Adding materials
//


//! Creates Lambertian material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddLambertianMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref					///< [in] Reflectance Painter
							)
{
	IPainter* pRef = pPntManager->GetItem( ref );
	if( !pRef ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateLambertianMaterial( &pMaterial, *pRef );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );

	return true;
}

//! Creates a Polished material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPolishedMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref,				///< [in] Reflectance of diffuse substrate
							const char* tau,				///< [in] Transmittance of dielectric top
							const char* Nt,					///< [in] Index of refraction of dielectric coating
							const char* scat,				///< [in] Scattering function for dielectric coating (either Phong or HG)
							const bool hg					///< [in] Use Henyey-Greenstein phase function scattering
							)
{
	IPainter* pRef = pPntManager->GetItem( ref );
	if( !pRef ) {
		return false;
	}

	IScalarPainter* pTau     = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "polished_material", name, "tau",        tau );
	IScalarPainter* pRefract = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "polished_material", name, "ior",        Nt );
	IScalarPainter* pScat    = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "polished_material", name, "scattering", scat );

	if( !pTau || !pRefract || !pScat ) {
		safe_release( pTau );
		safe_release( pRefract );
		safe_release( pScat );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreatePolishedMaterial( &pMaterial, *pRef, *pTau, *pRefract, *pScat, hg );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pTau );
	safe_release( pRefract );
	safe_release( pScat );

	return true;
}

// Resolve a `const char*` material-parameter string to an `IScalarPainter*`
// owned by the caller (refcount 1, must be released).  Accepts:
//   - The name of a registered `scalar_painter` chunk → looked up via
//     `pScalarPntManager`.
//   - An inline 3-double triple "r g b" → `RGBScalarPainter`.
//   - An inline single double "v" → `UniformScalarPainter`.
// Returns nullptr if the string is neither a known name nor parseable as
// a numeric literal.
//
// This is the scalar-side equivalent of the named-or-inline pattern
// used elsewhere in Job.cpp for `IPainter`, except it stays in the
// physical-scalar domain — no JH spectral uplift, no colorspace
// conversion.  Numeric literals like `scattering 1000000` round-trip
// exactly through the spectral path.
// Resolve a scalar painter from a `const char*`.  `requireSingle =
// true` rejects per-channel painters (RGB triples, RGBScalarPainter
// names, etc.) — used by material slots that read `.v[0]` only and
// would silently lose the G/B channels.  Returns nullptr on any
// failure; the targeted diagnostic is emitted by
// `ResolveOrDiagnoseScalar` (the caller-facing wrapper).
static IScalarPainter* ResolveScalarPainterArg(
	IScalarPainterManager* mgr,
	const char* value,
	bool requireSingle )
{
	if( !mgr || !value ) return nullptr;

	IScalarPainter* named = mgr->GetItem( value );
	if( named ) {
		if( requireSingle && named->HasPerChannelVariation() ) {
			// Caller emits the diagnostic — return nullptr so the
			// "bound-to-per-channel" branch in ResolveOrDiagnoseScalar
			// can fire with the chunk name + param name.
			return nullptr;
		}
		named->addref();
		return named;
	}

	auto onlyTrailingWhitespace = []( const char* p ) -> bool {
		while( *p ) {
			if( !std::isspace( static_cast<unsigned char>( *p ) ) ) return false;
			++p;
		}
		return true;
	};

	// 3-double inline triple — more specific, tried first.
	{
		char* end1 = nullptr;
		const double r = std::strtod( value, &end1 );
		if( end1 != value ) {
			char* end2 = nullptr;
			const double g = std::strtod( end1, &end2 );
			if( end2 != end1 ) {
				char* end3 = nullptr;
				const double b = std::strtod( end2, &end3 );
				if( end3 != end2 && onlyTrailingWhitespace( end3 ) ) {
					if( requireSingle && !( r == g && g == b ) ) {
						return nullptr;
					}
					IScalarPainter* p = nullptr;
					if( requireSingle ) {
						RISE_API_CreateUniformScalarPainter( &p,
							Scalar( r ) );
					} else {
						RISE_API_CreateRGBScalarPainter( &p,
							Scalar( r ), Scalar( g ), Scalar( b ) );
					}
					return p;
				}
			}
		}
	}

	// Single-double scalar.
	{
		char* end = nullptr;
		const double v = std::strtod( value, &end );
		if( end != value && onlyTrailingWhitespace( end ) ) {
			IScalarPainter* p = nullptr;
			RISE_API_CreateUniformScalarPainter( &p, Scalar( v ) );
			return p;
		}
	}

	return nullptr;
}

// Helper: resolve a scalar parameter and emit a targeted diagnostic
// (distinguishing legacy-`IPainter`-binding, per-channel-painter-in-
// single-slot, and unknown-name) when resolution fails.  Returns the
// resolved painter (caller owns, refcount 1) or nullptr on failure
// with the diagnostic already logged.
//
// `requireSingle = true` is used by material slots that internally
// read `.v[0]` only — without the check, an author binding
// `roughness 1 0 0` or a `values 0.1 0.4 0.9` RGBScalarPainter would
// silently lose the G/B channels in the RGB path but interpolate
// wavelength-dependently in the spectral path, producing physically
// incoherent results.
static IScalarPainter* ResolveOrDiagnoseScalar(
	IScalarPainterManager* smgr,
	IPainterManager* pmgr,
	const char* chunkKind,
	const char* chunkName,
	const char* paramName,
	const char* value,
	bool requireSingle )
{
	IScalarPainter* p = ResolveScalarPainterArg( smgr, value, requireSingle );
	if( p ) return p;

	// Distinguish three failure modes for actionable diagnostics:
	//   (a) the name is bound to a per-channel scalar_painter but
	//       the slot only reads one value;
	//   (b) the name is bound to a legacy `IPainter` chunk; or
	//   (c) the name is unknown.
	if( requireSingle && smgr ) {
		IScalarPainter* named = smgr->GetItem( value );
		if( named && named->HasPerChannelVariation() ) {
			GlobalLog()->PrintEx( eLog_Error,
				"%s `%s`: parameter `%s` is bound to per-channel scalar_painter `%s`, but this slot reads a single scalar — use a wavelength-uniform painter (`value` or `file` / `sellmeier` / etc.) instead.",
				chunkKind, chunkName, paramName, value );
			return nullptr;
		}
	}

	IPainter* legacy = pmgr ? pmgr->GetItem( value ) : nullptr;
	if( legacy ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s `%s`: parameter `%s` is bound to `IPainter` chunk `%s`; this slot now requires a `scalar_painter` (physical scalar, no JH spectral uplift).  See docs/ISCALARPAINTER_REFACTOR.md.",
			chunkKind, chunkName, paramName, value );
	} else if( requireSingle ) {
		// Inline-triple-in-single-slot path: scrub the value to see
		// whether it parses as 3 numbers, and if so emit the specific
		// "per-channel triple supplied where single scalar required"
		// message instead of the generic one.
		char* end1 = nullptr;
		(void)std::strtod( value, &end1 );
		char* end2 = nullptr;
		(void)std::strtod( end1, &end2 );
		char* end3 = nullptr;
		(void)std::strtod( end2, &end3 );
		const bool tripleShaped = ( end3 != end2 && end2 != end1 && end1 != value );
		if( tripleShaped ) {
			GlobalLog()->PrintEx( eLog_Error,
				"%s `%s`: parameter `%s` value `%s` is a per-channel RGB triple but this slot reads a single scalar — use one number, not three.",
				chunkKind, chunkName, paramName, value );
		} else {
			GlobalLog()->PrintEx( eLog_Error,
				"%s `%s`: parameter `%s` value `%s` is neither a registered scalar_painter nor an inline numeric literal — see docs/ISCALARPAINTER_REFACTOR.md",
				chunkKind, chunkName, paramName, value );
		}
	} else {
		GlobalLog()->PrintEx( eLog_Error,
			"%s `%s`: parameter `%s` value `%s` is neither a registered scalar_painter nor an inline numeric literal — see docs/ISCALARPAINTER_REFACTOR.md",
			chunkKind, chunkName, paramName, value );
	}
	return nullptr;
}

//! Creates a Dielectric material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddDielectricMaterial(
							const char* name,				///< [in] Name of the material
							const char* tau,				///< [in] Transmittance painter
							const char* rIndex,				///< [in] Index of refraction
							const char* scat,				///< [in] Scattering function (either Phong or HG)
							const bool hg,					///< [in] Use Henyey-Greenstein phase function scattering
							const Scalar arN,
							const Scalar arK,
							const Scalar arThickness
							)
{
	// tau/ior/scattering are all physical scalars carried by
	// `IScalarPainter`.  Previously they routed through `IPainter` →
	// `UniformColorPainter::GetColorNM` → JH spectral uplift, which
	// silently mangled inline-numeric values like `scattering 1000000`
	// in every spectral rasterizer (clamped to ~1.0, glass rendered
	// invisible).  The scalar painter path never touches colorspace.
	IScalarPainter* pTau  = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "dielectric_material", name, "tau",        tau );
	IScalarPainter* pIor  = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "dielectric_material", name, "ior",        rIndex );
	IScalarPainter* pScat = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "dielectric_material", name, "scattering", scat );

	if( !pTau || !pIor || !pScat ) {
		safe_release( pTau );
		safe_release( pIor );
		safe_release( pScat );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateDielectricMaterial( &pMaterial, *pTau, *pIor, *pScat, hg, arN, arK, arThickness );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pTau );
	safe_release( pIor );
	safe_release( pScat );

	return true;
}

//! Creates a SubSurface Scattering material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSubSurfaceScatteringMaterial(
							const char* name,				///< [in] Name of the material
							const char* ior,				///< [in] Index of refraction
							const char* absorption,			///< [in] Absorption coefficient
							const char* scattering,			///< [in] Scattering coefficient
							const char* g,					///< [in] HG asymmetry parameter
							const char* roughness			///< [in] Surface roughness [0,1]
							)
{
	// ior is a single scalar (BSDF/SPF read `.v[0]` only).  Absorption
	// and scattering are per-channel (the diffusion profile evaluates
	// each RGB channel independently for sigma_a / sigma_s).
	IScalarPainter* pIOR        = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "subsurfacescattering_material", name, "ior",        ior,        /*requireSingle*/ true );
	IScalarPainter* pAbsorption = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "subsurfacescattering_material", name, "absorption", absorption );
	IScalarPainter* pScattering = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "subsurfacescattering_material", name, "scattering", scattering );

	if( !pIOR || !pAbsorption || !pScattering ) {
		safe_release( pIOR );
		safe_release( pAbsorption );
		safe_release( pScattering );
		return false;
	}

	double gVal = atof(g);
	double roughnessVal = atof(roughness);

	IMaterial* pMaterial = 0;
	RISE_API_CreateSubSurfaceScatteringMaterial( &pMaterial, *pIOR, *pAbsorption, *pScattering, gVal, roughnessVal );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pIOR );
	safe_release( pAbsorption );
	safe_release( pScattering );

	return true;
}

//! Creates a Random Walk SSS material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddRandomWalkSSSMaterial(
							const char* name,				///< [in] Name of the material
							const char* ior,				///< [in] Index of refraction
							const char* absorption,			///< [in] Absorption coefficient
							const char* scattering,			///< [in] Scattering coefficient
							const char* g,					///< [in] HG asymmetry parameter
							const char* roughness,			///< [in] Surface roughness [0,1]
							const char* maxBounces			///< [in] Maximum walk steps
							)
{
	IScalarPainter* pIOR        = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "randomwalk_sss_material", name, "ior",        ior,        /*requireSingle*/ true );
	IScalarPainter* pAbsorption = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "randomwalk_sss_material", name, "absorption", absorption );
	IScalarPainter* pScattering = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "randomwalk_sss_material", name, "scattering", scattering );

	if( !pIOR || !pAbsorption || !pScattering ) {
		safe_release( pIOR );
		safe_release( pAbsorption );
		safe_release( pScattering );
		return false;
	}

	double gVal = atof(g);
	double roughnessVal = atof(roughness);
	unsigned int maxBouncesVal = atoi(maxBounces);

	IMaterial* pMaterial = 0;
	RISE_API_CreateRandomWalkSSSMaterial( &pMaterial, *pIOR, *pAbsorption, *pScattering, gVal, roughnessVal, maxBouncesVal );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pIOR );
	safe_release( pAbsorption );
	safe_release( pScattering );

	return true;
}

//! Creates an isotropic phong material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddIsotropicPhongMaterial(
							const char* name,				///< [in] Name of the material
							const char* rd,					///< [in] Diffuse reflectance painter
							const char* rs,					///< [in] Specular reflectance painter
							const char* exponent			///< [in] Phong exponent (physical scalar)
							)
{
	IPainter* pRd = pPntManager->GetItem(rd);
	IPainter* pRs = pPntManager->GetItem(rs);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pExp = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "isotropic_phong_material", name, "exponent", exponent );
	if( !pExp ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateIsotropicPhongMaterial( &pMaterial, *pRd, *pRs, *pExp );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pExp );

	return true;
}

//! Creates the anisotropic phong material of Ashikmin and Shirley
/// \return TRUE if successful, FALSE otherwise
bool Job::AddAshikminShirleyAnisotropicPhongMaterial(
							const char* name,				///< [in] Name of the material
							const char* rd,					///< [in] Diffuse reflectance painter
							const char* rs,					///< [in] Specular reflectance painter
							const char* Nu,					///< [in] Phong exponent in U (physical scalar)
							const char* Nv					///< [in] Phong exponent in V (physical scalar)
							)
{
	IPainter* pRd = pPntManager->GetItem(rd);
	IPainter* pRs = pPntManager->GetItem(rs);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pNu = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ashikminshirley_anisotropicphong_material", name, "Nu", Nu );
	IScalarPainter* pNv = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ashikminshirley_anisotropicphong_material", name, "Nv", Nv );

	if( !pNu || !pNv ) {
		safe_release( pNu );
		safe_release( pNv );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateAshikminShirleyAnisotropicPhongMaterial( &pMaterial, *pRd, *pRs, *pNu, *pNv );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pNu );
	safe_release( pNv );

	return true;
}

//! Creates a perfect reflector
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerfectReflectorMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref					///< [in] Reflectance painter
							)
{
	IPainter* pRd = pPntManager->GetItem(ref);

	if( !pRd ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreatePerfectReflectorMaterial( &pMaterial, *pRd );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );

	return true;
}

//! Creates a perfect refractor
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPerfectRefractorMaterial(
							const char* name,				///< [in] Name of the material
							const char* ref,				///< [in] Amount of refraction painter
							const char* ior					///< [in] Index of refraction (physical scalar)
							)
{
	IPainter* pRd = pPntManager->GetItem(ref);
	if( !pRd ) {
		return false;
	}

	IScalarPainter* pIOR = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager,
		"perfectrefractor_material", name, "ior", ior );
	if( !pIOR ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreatePerfectRefractorMaterial( &pMaterial, *pRd, *pIOR );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pIOR );
	safe_release( pMaterial );

	return true;
}

//! Creates a translucent material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTranslucentMaterial(
							const char* name,				///< [in] Name of the material
							const char* rF,					///< [in] Reflectance painter
							const char* T,					///< [in] Transmittance painter
							const char* ext,				///< [in] Extinction painter
							const char* N,					///< [in] Phong scattering function
							const char* scat				///< [in] Multiple scattering component
							)
{
	IPainter* pRf  = pPntManager->GetItem(rF);
	IPainter* pTau = pPntManager->GetItem(T);

	if( !pRf || !pTau ) {
		return false;
	}

	IScalarPainter* pExt  = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "translucent_material", name, "extinction", ext );
	IScalarPainter* pN    = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "translucent_material", name, "N",          N );
	IScalarPainter* pScat = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "translucent_material", name, "scattering", scat );

	if( !pExt || !pN || !pScat ) {
		safe_release( pExt );
		safe_release( pN );
		safe_release( pScat );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateTranslucentMaterial( &pMaterial, *pRf, *pTau, *pExt, *pN, *pScat );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pExt );
	safe_release( pN );
	safe_release( pScat );
	safe_release( pMaterial );

	return true;
}

bool Job::AddBioSpecSkinMaterial(
	const char* name,
	const char* thickness_SC_,									///< Thickness of the stratum corneum (in cm)
	const char* thickness_epidermis_,							///< Thickness of the epidermis (in cm)
	const char* thickness_papillary_dermis_,					///< Thickness of the papillary dermis (in cm)
	const char* thickness_reticular_dermis_,					///< Thickness of the reticular dermis (in cm)
	const char* ior_SC_,										///< Index of refraction of the stratum corneum
	const char* ior_epidermis_,									///< Index of refraction of the epidermis
	const char* ior_papillary_dermis_,							///< Index of refraction of the papillary dermis
	const char* ior_reticular_dermis_,							///< Index of refraction of the reticular dermis
	const char* concentration_eumelanin_,						///< Average Concentration of eumelanin in the melanosomes
	const char* concentration_pheomelanin_,						///< Average Concentration of pheomelanin in the melanosomes
	const char* melanosomes_in_epidermis_,						///< Percentage of the epidermis made up of melanosomes
	const char* hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
	const char* whole_blood_in_papillary_dermis_,				///< Percentage of the papillary dermis made up of whole blood
	const char* whole_blood_in_reticular_dermis_,				///< Percentage of the reticular dermis made up of whole blood
	const char* bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
	const char* betacarotene_concentration_SC_,					///< Concentration of Beta-Carotene in the stratum corneum
	const char* betacarotene_concentration_epidermis_,			///< Concentration of Beta-Carotene in the epidermis
	const char* betacarotene_concentration_dermis_,				///< Concentration of Beta-Carotene in the dermis
	const char* folds_aspect_ratio_,							///< Aspect ratio of the little folds and wrinkles on the skin surface
	const bool bSubdermalLayer									///< Should the model simulate a perfectly reflecting subdermal layer?
	)
{

	// Resolve every BioSpec parameter to an IScalarPainter (physical
	// scalars).  Every slot here reads `.v[0]` inside the SPF —
	// `requireSingle = true` so per-channel inputs are rejected at
	// parse time instead of silently dropping G/B channels.  If any
	// fails to resolve, release whatever we have and bail with a
	// clear per-parameter diagnostic.
#define RESOLVE_BIOSPEC( x ) \
	IScalarPainter* pnt_##x = ResolveOrDiagnoseScalar( \
		pScalarPntManager, pPntManager, "biospec_skin_material", name, #x, x, /*requireSingle*/ true )

	RESOLVE_BIOSPEC(thickness_SC_);
	RESOLVE_BIOSPEC(thickness_epidermis_);
	RESOLVE_BIOSPEC(thickness_papillary_dermis_);
	RESOLVE_BIOSPEC(thickness_reticular_dermis_);
	RESOLVE_BIOSPEC(ior_SC_);
	RESOLVE_BIOSPEC(ior_epidermis_);
	RESOLVE_BIOSPEC(ior_papillary_dermis_);
	RESOLVE_BIOSPEC(ior_reticular_dermis_);
	RESOLVE_BIOSPEC(concentration_eumelanin_);
	RESOLVE_BIOSPEC(concentration_pheomelanin_);
	RESOLVE_BIOSPEC(melanosomes_in_epidermis_);
	RESOLVE_BIOSPEC(hb_ratio_);
	RESOLVE_BIOSPEC(whole_blood_in_papillary_dermis_);
	RESOLVE_BIOSPEC(whole_blood_in_reticular_dermis_);
	RESOLVE_BIOSPEC(bilirubin_concentration_);
	RESOLVE_BIOSPEC(betacarotene_concentration_SC_);
	RESOLVE_BIOSPEC(betacarotene_concentration_epidermis_);
	RESOLVE_BIOSPEC(betacarotene_concentration_dermis_);
	RESOLVE_BIOSPEC(folds_aspect_ratio_);

#undef RESOLVE_BIOSPEC

	IScalarPainter* all[19] = {
		pnt_thickness_SC_, pnt_thickness_epidermis_, pnt_thickness_papillary_dermis_,
		pnt_thickness_reticular_dermis_, pnt_ior_SC_, pnt_ior_epidermis_,
		pnt_ior_papillary_dermis_, pnt_ior_reticular_dermis_,
		pnt_concentration_eumelanin_, pnt_concentration_pheomelanin_,
		pnt_melanosomes_in_epidermis_, pnt_hb_ratio_,
		pnt_whole_blood_in_papillary_dermis_, pnt_whole_blood_in_reticular_dermis_,
		pnt_bilirubin_concentration_,
		pnt_betacarotene_concentration_SC_, pnt_betacarotene_concentration_epidermis_,
		pnt_betacarotene_concentration_dermis_, pnt_folds_aspect_ratio_,
	};
	for( int i=0; i<19; ++i ) {
		if( !all[i] ) {
			for( int j=0; j<19; ++j ) safe_release( all[j] );
			return false;
		}
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateBioSpecSkinMaterial( &pMaterial,
		*pnt_thickness_SC_,
		*pnt_thickness_epidermis_,
		*pnt_thickness_papillary_dermis_,
		*pnt_thickness_reticular_dermis_,
		*pnt_ior_SC_,
		*pnt_ior_epidermis_,
		*pnt_ior_papillary_dermis_,
		*pnt_ior_reticular_dermis_,
		*pnt_concentration_eumelanin_,
		*pnt_concentration_pheomelanin_,
		*pnt_melanosomes_in_epidermis_,
		*pnt_hb_ratio_,
		*pnt_whole_blood_in_papillary_dermis_,
		*pnt_whole_blood_in_reticular_dermis_,
		*pnt_bilirubin_concentration_,
		*pnt_betacarotene_concentration_SC_,
		*pnt_betacarotene_concentration_epidermis_,
		*pnt_betacarotene_concentration_dermis_,
		*pnt_folds_aspect_ratio_,
		bSubdermalLayer
		);

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	// Release the 19 resolved scalar painters — the SPF holds its
	// own refs (taken in `BioSpecSkinSPF` ctor), so releasing here
	// drops them back to refcount 1 instead of leaking at 2.
	for( int i = 0; i < 19; ++i ) safe_release( all[i] );

	return true;
}

bool Job::AddDonnerJensenSkinBSSRDFMaterial(
	const char* name,
	const char* melanin_fraction_,
	const char* melanin_blend_,
	const char* hemoglobin_epidermis_,
	const char* carotene_fraction_,
	const char* hemoglobin_dermis_,
	const char* epidermis_thickness_,
	const char* ior_epidermis_,
	const char* ior_dermis_,
	const char* blood_oxygenation_,
	const char* roughness
	)
{
	// All 9 Donner-Jensen slots are read as `.v[0]` inside the
	// diffusion profile — `requireSingle = true` rejects per-channel
	// inputs at parse time.
#define RESOLVE_DJ( x ) \
	IScalarPainter* pnt_##x = ResolveOrDiagnoseScalar( \
		pScalarPntManager, pPntManager, "donnerjensenskin_bssrdf_material", name, #x, x, /*requireSingle*/ true )

	RESOLVE_DJ(melanin_fraction_);
	RESOLVE_DJ(melanin_blend_);
	RESOLVE_DJ(hemoglobin_epidermis_);
	RESOLVE_DJ(carotene_fraction_);
	RESOLVE_DJ(hemoglobin_dermis_);
	RESOLVE_DJ(epidermis_thickness_);
	RESOLVE_DJ(ior_epidermis_);
	RESOLVE_DJ(ior_dermis_);
	RESOLVE_DJ(blood_oxygenation_);

#undef RESOLVE_DJ

	IScalarPainter* all_dj[9] = {
		pnt_melanin_fraction_, pnt_melanin_blend_, pnt_hemoglobin_epidermis_,
		pnt_carotene_fraction_, pnt_hemoglobin_dermis_, pnt_epidermis_thickness_,
		pnt_ior_epidermis_, pnt_ior_dermis_, pnt_blood_oxygenation_,
	};
	for( int i=0; i<9; ++i ) {
		if( !all_dj[i] ) {
			for( int j=0; j<9; ++j ) safe_release( all_dj[j] );
			return false;
		}
	}

	const double roughnessVal = atof( roughness );

	IMaterial* pMaterial = 0;
	RISE_API_CreateDonnerJensenSkinBSSRDFMaterial( &pMaterial,
		*pnt_melanin_fraction_,
		*pnt_melanin_blend_,
		*pnt_hemoglobin_epidermis_,
		*pnt_carotene_fraction_,
		*pnt_hemoglobin_dermis_,
		*pnt_epidermis_thickness_,
		*pnt_ior_epidermis_,
		*pnt_ior_dermis_,
		*pnt_blood_oxygenation_,
		roughnessVal
		);

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	// Release the 9 resolved scalar painters — the material's
	// diffusion profile holds its own refs.
	for( int i = 0; i < 9; ++i ) safe_release( all_dj[i] );

	return true;
}

//! Adds a generic human tissue material based on BioSpec
/// \return TRUE if successful, FALSE otherwise
bool Job::AddGenericHumanTissueMaterial(
	const char* name,
	const char* sca,											///< [in] Scattering co-efficient
	const char* g,												///< [in] The g factor in the HG phase function
	const double whole_blood_,									///< Percentage of the tissue made up of whole blood
	const double hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
	const double bilirubin_concentration_,						///< Concentration of Bilirubin in whole blood
	const double betacarotene_concentration_,					///< Concentration of Beta-Carotene in whole blood
	const bool diffuse											///< Is the tissue just completely diffuse?
	)
{
	GlobalLog()->PrintEasyWarning( "Job::AddGenericHumanTissueMaterial:: This is an experiment and has not bee tested or verfied, its use is not recommended" );

	IScalarPainter* pG   = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "generichumantissue_material", name, "g",   g,   /*requireSingle*/ true );
	IScalarPainter* pSca = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "generichumantissue_material", name, "sca", sca, /*requireSingle*/ true );

	if( !pG || !pSca ) {
		safe_release( pG );
		safe_release( pSca );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateGenericHumanTissueMaterial( &pMaterial,
		*pSca,
		*pG,
		whole_blood_,
		hb_ratio_,
		bilirubin_concentration_,
		betacarotene_concentration_,
		diffuse );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );
	safe_release( pSca );
	safe_release( pG );

	return true;
}

//! Adds Composite material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCompositeMaterial(
							const char* name,											///< [in] Name of the material
							const char* top,											///< [in] Name of material on top
							const char* bottom,											///< [in] Name of material on bottom
							const unsigned int max_recur,								///< [in] Maximum recursion level in the random walk process
							const unsigned int max_reflection_recursion,				///< [in] Maximum level of reflection recursion
							const unsigned int max_refraction_recursion,				///< [in] Maximum level of refraction recursion
							const unsigned int max_diffuse_recursion,					///< [in] Maximum level of diffuse recursion
							const unsigned int max_translucent_recursion,				///< [in] Maximum level of translucent recursion
							const double thickness,										///< [in] Thickness between the materials
							const char* extinction										///< [in] Extinction painter name
							)
{
	IMaterial* pTop = pMatManager->GetItem( top );
	IMaterial* pBottom = pMatManager->GetItem( bottom );

	if( !pTop || !pBottom ) {
		return false;
	}

	IPainter* pExt = pPntManager->GetItem( extinction );

	if( !pExt ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateCompositeMaterial( &pMaterial, *pTop, *pBottom, max_recur, max_reflection_recursion, max_refraction_recursion, max_diffuse_recursion, max_translucent_recursion, thickness, *pExt );

	pMatManager->AddItem( pMaterial, name );
	safe_release( pMaterial );

	return true;
}

//! Adds Composite material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddWardIsotropicGaussianMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* alpha											///< [in] Standard deviation (RMS) of surface slope
	)

{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pAlpha = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ward_isotropic_material", name, "alpha", alpha );
	if( !pAlpha ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateWardIsotropicGaussianMaterial( &pMaterial, *pRd, *pRs, *pAlpha );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pAlpha );

	return true;
}

//! Adds Ward's anisotropic elliptical gaussian material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddWardAnisotropicEllipticalGaussianMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* alphax,											///< [in] Standard deviation (RMS) of surface slope in x
	const char* alphay											///< [in] Standard deviation (RMS) of surface slope in y
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pAlphaX = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ward_anisotropic_material", name, "alphax", alphax );
	IScalarPainter* pAlphaY = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ward_anisotropic_material", name, "alphay", alphay );

	if( !pAlphaX || !pAlphaY ) {
		safe_release( pAlphaX );
		safe_release( pAlphaY );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateWardAnisotropicEllipticalGaussianMaterial( &pMaterial, *pRd, *pRs, *pAlphaX, *pAlphaY );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pAlphaX );
	safe_release( pAlphaY );

	return true;
}

//! Adds Cook Torrance material
/// \return TRUE if successful, FALSE otherwise
// Resolves a `const char*` fresnel-mode selector into the FresnelMode enum.
// Empty / NULL strings map to conductor (preserves the no-mode call sites).
// Unknown values fall back to conductor with a per-string-once warning, so
// a scene with a thousand materials sharing the same typo doesn't generate
// a thousand warning lines.
static FresnelMode ResolveFresnelMode( const char* mode )
{
	if( !mode || !mode[0] ) return eFresnelConductor;
	if( strcmp( mode, "conductor"  ) == 0 ) return eFresnelConductor;
	if( strcmp( mode, "schlick_f0" ) == 0 ) return eFresnelSchlickF0;
	if( strcmp( mode, "thinfilm"   ) == 0 ) return eFresnelThinFilmConductor;

	static std::set<std::string> seen;
	if( seen.insert( std::string(mode) ).second ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"Unknown fresnel_mode `%s`; falling back to conductor.  Valid: conductor, schlick_f0, thinfilm", mode );
	}
	return eFresnelConductor;
}

bool Job::AddGGXMaterial(
	const char* name,
	const char* diffuse,
	const char* specular,
	const char* alphaX,
	const char* alphaY,
	const char* ior,
	const char* ext,
	const char* fresnel_mode,
	const char* tangent_rotation,
	const char* film_ior,
	const char* film_extinction,
	const char* film_thickness
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pAlphaX = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_material", name, "alphax", alphaX );
	IScalarPainter* pAlphaY = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_material", name, "alphay", alphaY );
	IScalarPainter* pIOR = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_material", name, "ior", ior );
	IScalarPainter* pExt = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_material", name, "ext", ext );

	if( !pAlphaX || !pAlphaY || !pIOR || !pExt ) {
		safe_release( pAlphaX );
		safe_release( pAlphaY );
		safe_release( pIOR );
		safe_release( pExt );
		return false;
	}

	// Thin-film FILM slots (eFresnelThinFilmConductor).  These are physical
	// scalars (oxide n / k / thickness-nm) and MUST resolve as IScalarPainter
	// (no JH spectral uplift), exactly like ior / ext above.  "none" => no
	// film painter (nullptr passed through).  Resolved unconditionally so a
	// stray film_* on a non-thinfilm material still gets a real diagnostic if
	// it names a bad painter; the thinfilm-mode presence contract is enforced
	// below.
	const FresnelMode resolvedFresnel = ResolveFresnelMode( fresnel_mode );

	IScalarPainter* pFilmIOR = 0;
	IScalarPainter* pFilmExt = 0;
	IScalarPainter* pFilmThk = 0;
	const bool wantFilmIOR = ( film_ior        && std::string( film_ior )        != "none" );
	const bool wantFilmExt = ( film_extinction && std::string( film_extinction ) != "none" );
	const bool wantFilmThk = ( film_thickness  && std::string( film_thickness )  != "none" );

	if( wantFilmIOR ) {
		pFilmIOR = ResolveOrDiagnoseScalar(
			pScalarPntManager, pPntManager, "ggx_material", name, "film_ior", film_ior );
	}
	if( wantFilmExt ) {
		pFilmExt = ResolveOrDiagnoseScalar(
			pScalarPntManager, pPntManager, "ggx_material", name, "film_extinction", film_extinction );
	}
	if( wantFilmThk ) {
		pFilmThk = ResolveOrDiagnoseScalar(
			pScalarPntManager, pPntManager, "ggx_material", name, "film_thickness", film_thickness );
	}

	// If a film_* slot was requested but failed to resolve, that is a hard
	// error (the painter name was bad) regardless of fresnel_mode.
	if( ( wantFilmIOR && !pFilmIOR ) || ( wantFilmExt && !pFilmExt ) || ( wantFilmThk && !pFilmThk ) ) {
		safe_release( pAlphaX ); safe_release( pAlphaY );
		safe_release( pIOR ); safe_release( pExt );
		safe_release( pFilmIOR ); safe_release( pFilmExt ); safe_release( pFilmThk );
		return false;
	}

	// thinfilm-mode presence contract: the P2-B BSDF dereferences the film
	// painters whenever eFresnelThinFilmConductor is active, so film_ior and
	// film_thickness MUST be present (film_extinction defaults to a transparent
	// k = 0 film and is optional).  Reject at parse time with a clear message
	// rather than letting the renderer null-deref.
	if( resolvedFresnel == eFresnelThinFilmConductor && ( !pFilmIOR || !pFilmThk ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"ggx_material `%s`: fresnel_mode `thinfilm` requires both `film_ior` and `film_thickness` (oxide n + thickness-nm scalar_painters); `film_extinction` is optional (default transparent k = 0).  See docs/THIN_FILM_INTERFERENCE.md §7.",
			name ? name : "noname" );
		safe_release( pAlphaX ); safe_release( pAlphaY );
		safe_release( pIOR ); safe_release( pExt );
		safe_release( pFilmIOR ); safe_release( pFilmExt ); safe_release( pFilmThk );
		return false;
	}

	// Landing 8: tangent_rotation is "none" (no rotation, default) OR
	// a painter / scalar string (rotation in radians).  Resolved here
	// to a temporarily-addref'd IPainter*; the GGX{BRDF,SPF} hold their
	// own ref so we drop ours before returning.
	IPainter* pTangentRotation = 0;
	if( tangent_rotation && std::string( tangent_rotation ) != "none" ) {
		pTangentRotation = pPntManager->GetItem( tangent_rotation );
		if( !pTangentRotation ) {
			const double fa = atof( tangent_rotation );
			RISE_API_CreateUniformColorPainter( &pTangentRotation, RISEPel( fa, fa, fa ) );
		} else {
			pTangentRotation->addref();
		}
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateGGXMaterialThinFilm( &pMaterial, *pRd, *pRs, *pAlphaX, *pAlphaY, *pIOR, *pExt,
		resolvedFresnel, pTangentRotation, pFilmIOR, pFilmExt, pFilmThk );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pAlphaX );
	safe_release( pAlphaY );
	safe_release( pIOR );
	safe_release( pExt );
	safe_release( pTangentRotation );
	safe_release( pFilmIOR );
	safe_release( pFilmExt );
	safe_release( pFilmThk );

	return true;
}

//! AddGGXEmissiveMaterial -- GGX with an optional LambertianEmitter folded
//! in.  Passing emissive=="none" / NULL produces the same material as
//! AddGGXMaterial; otherwise the emissive painter feeds a LambertianEmitter
//! at radiance scaled by emissive_scale, and the material's GetEmitter()
//! returns it.  Used by pbr_metallic_roughness_material when the source
//! has an emissiveFactor / emissiveTexture.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddGGXEmissiveMaterial(
	const char* name,
	const char* diffuse,
	const char* specular,
	const char* alphaX,
	const char* alphaY,
	const char* ior,
	const char* ext,
	const char* emissive,
	const double emissive_scale,
	const char* fresnel_mode,
	const char* tangent_rotation,
	const char* film_ior,
	const char* film_extinction,
	const char* film_thickness
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pAlphaX = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_emissive_material", name, "alphax", alphaX );
	IScalarPainter* pAlphaY = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_emissive_material", name, "alphay", alphaY );
	IScalarPainter* pIOR = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_emissive_material", name, "ior", ior );
	IScalarPainter* pExt = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "ggx_emissive_material", name, "ext", ext );

	if( !pAlphaX || !pAlphaY || !pIOR || !pExt ) {
		safe_release( pAlphaX );
		safe_release( pAlphaY );
		safe_release( pIOR );
		safe_release( pExt );
		return false;
	}

	// Resolve the optional emissive painter.  "none" / NULL means no emitter.
	IPainter* pEmissive = 0;
	const bool wantEmissive = ( emissive && std::string( emissive ) != "none" );
	if( wantEmissive ) {
		pEmissive = pPntManager->GetItem( emissive );
		if( !pEmissive ) {
			GlobalLog()->PrintEx( eLog_Error,
				"Job::AddGGXEmissiveMaterial:: emissive painter `%s` not found", emissive );
			safe_release( pAlphaX );
			safe_release( pAlphaY );
			safe_release( pIOR );
			safe_release( pExt );
			return false;
		}
	}

	// Thin-film FILM slots (eFresnelThinFilmConductor) — same resolve +
	// presence-contract logic as AddGGXMaterial above.  IScalarPainter
	// (no JH uplift); "none" => nullptr.
	const FresnelMode resolvedFresnel = ResolveFresnelMode( fresnel_mode );

	IScalarPainter* pFilmIOR = 0;
	IScalarPainter* pFilmExt = 0;
	IScalarPainter* pFilmThk = 0;
	const bool wantFilmIOR = ( film_ior        && std::string( film_ior )        != "none" );
	const bool wantFilmExt = ( film_extinction && std::string( film_extinction ) != "none" );
	const bool wantFilmThk = ( film_thickness  && std::string( film_thickness )  != "none" );

	if( wantFilmIOR ) {
		pFilmIOR = ResolveOrDiagnoseScalar(
			pScalarPntManager, pPntManager, "ggx_emissive_material", name, "film_ior", film_ior );
	}
	if( wantFilmExt ) {
		pFilmExt = ResolveOrDiagnoseScalar(
			pScalarPntManager, pPntManager, "ggx_emissive_material", name, "film_extinction", film_extinction );
	}
	if( wantFilmThk ) {
		pFilmThk = ResolveOrDiagnoseScalar(
			pScalarPntManager, pPntManager, "ggx_emissive_material", name, "film_thickness", film_thickness );
	}

	if( ( wantFilmIOR && !pFilmIOR ) || ( wantFilmExt && !pFilmExt ) || ( wantFilmThk && !pFilmThk ) ) {
		safe_release( pAlphaX ); safe_release( pAlphaY );
		safe_release( pIOR ); safe_release( pExt );
		safe_release( pFilmIOR ); safe_release( pFilmExt ); safe_release( pFilmThk );
		return false;
	}

	if( resolvedFresnel == eFresnelThinFilmConductor && ( !pFilmIOR || !pFilmThk ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"ggx_material `%s`: fresnel_mode `thinfilm` requires both `film_ior` and `film_thickness` (oxide n + thickness-nm scalar_painters); `film_extinction` is optional (default transparent k = 0).  See docs/THIN_FILM_INTERFERENCE.md §7.",
			name ? name : "noname" );
		safe_release( pAlphaX ); safe_release( pAlphaY );
		safe_release( pIOR ); safe_release( pExt );
		safe_release( pFilmIOR ); safe_release( pFilmExt ); safe_release( pFilmThk );
		return false;
	}

	// Landing 8: optional tangent_rotation painter (radians).  Same
	// resolve logic as AddGGXMaterial above.
	IPainter* pTangentRotation = 0;
	if( tangent_rotation && std::string( tangent_rotation ) != "none" ) {
		pTangentRotation = pPntManager->GetItem( tangent_rotation );
		if( !pTangentRotation ) {
			const double fa = atof( tangent_rotation );
			RISE_API_CreateUniformColorPainter( &pTangentRotation, RISEPel( fa, fa, fa ) );
		} else {
			pTangentRotation->addref();
		}
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateGGXEmissiveMaterialThinFilm(
		&pMaterial, *pRd, *pRs, *pAlphaX, *pAlphaY, *pIOR, *pExt, pEmissive, emissive_scale,
		resolvedFresnel, pTangentRotation, pFilmIOR, pFilmExt, pFilmThk );

	const bool added = pMatManager->AddItem( pMaterial, name );
	// Only mark as composed when AddItem actually registered the
	// new material — a duplicate-name failure (existing direct
	// material under the same name) would otherwise mark THAT
	// pre-existing material as composed, falsely freezing its
	// slot rows in the interactive editor.
	if( added ) {
		composedMaterialNames.insert( String( name ) );
	}

	safe_release( pMaterial );
	safe_release( pAlphaX );
	safe_release( pAlphaY );
	safe_release( pIOR );
	safe_release( pExt );
	safe_release( pTangentRotation );
	safe_release( pFilmIOR );
	safe_release( pFilmExt );
	safe_release( pFilmThk );

	return added;
}

//! AddPBRMetallicRoughnessMaterial: glTF-spec PBR material composition.
//!
//! Builds the painter graph that maps glTF metallicRoughness inputs onto
//! ggx_material's interface, then registers a final ggx (with optional
//! emissive) under `name`.  Internal helper painters are registered under
//! `__pbrmr_<name>__<role>` so they don't clash with user names.
//!
//! See IJob.h for the math; see docs/GLTF_IMPORT.md §4 for the design
//! rationale.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPBRMetallicRoughnessMaterial(
	const char* name,
	const char* base_color,
	const char* metallic,
	const char* roughness,
	const double ior,
	const char* emissive,
	const double emissive_scale,
	const char* specular_factor,
	const char* specular_color,
	const char* anisotropy_factor,
	const char* anisotropy_rotation
	)
{
	// Resolve the base_color painter.  Must already exist.
	if( !pPntManager->GetItem( base_color ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddPBRMetallicRoughnessMaterial:: base_color painter `%s` not found", base_color );
		return false;
	}

	// metallic / roughness / specular_* / anisotropy_* can be either a
	// painter name or a scalar string like "0.5".  When the painter
	// manager doesn't have an entry under the given name, fall back to
	// atof() and synthesise a uniform-color painter (named after the
	// role + scalar value) to drive downstream blends.
	std::string nameStr( name );
	const std::string prefix = "__pbrmr_" + nameStr + "__";

	auto resolveOrSynth = [&]( const char* in, const char* role ) -> std::string {
		if( pPntManager->GetItem( in ) ) {
			return std::string( in );
		}
		// Synth uniformcolor from scalar.
		const double v = atof( in );
		const std::string synthName = prefix + std::string( role ) + "_scalar";
		const double pel[3] = { v, v, v };
		AddUniformColorPainter( synthName.c_str(), pel, "Rec709RGB_Linear" );
		return synthName;
	};

	const std::string metallicName  = resolveOrSynth( metallic,  "metallic"  );
	const std::string roughnessName = resolveOrSynth( roughness, "roughness" );

	// Internal helper painters.  All in linear (Rec709 -> ROMM applied at
	// load), since we're treating these as numeric weights / multipliers,
	// not display colours.  The blend painter does per-channel arithmetic
	// without further colour conversion.
	//
	// Note: prior revisions also created an `f96` retention painter that
	// pre-multiplied diffuse by (1 - 0.04).  That's been removed because
	// the GGX BSDF's schlick_f0 mode applies (1 - max(F0)) at evaluation
	// time per the glTF spec (which gives ≈ 0.96 for dielectric F0 = 0.04
	// AND the correct ≈ 0 for metallic F0 = baseColor).  Pre-multiplying
	// would double-apply.
	const double zero[3]    = { 0.0, 0.0, 0.0 };
	const double white[3]   = { 1.0, 1.0, 1.0 };
	const double point04[3] = { 0.04, 0.04, 0.04 };
	const double iorVec[3]  = { ior, ior, ior };

	const std::string nZero     = prefix + "zero";
	const std::string nWhite    = prefix + "white";
	const std::string nPoint04  = prefix + "point04";
	const std::string nF0Diel   = prefix + "f0diel";
	const std::string nIORPnt   = prefix + "ior";

	AddUniformColorPainter( nZero.c_str(),    zero,    "Rec709RGB_Linear" );
	AddUniformColorPainter( nWhite.c_str(),   white,   "Rec709RGB_Linear" );
	AddUniformColorPainter( nPoint04.c_str(), point04, "Rec709RGB_Linear" );
	AddUniformColorPainter( nIORPnt.c_str(),  iorVec,  "Rec709RGB_Linear" );

	// one_minus_met = (1 - metallic) = blend(zero, white, metallic)
	// (BlendPainter convention: blend(a, b, mask) = a*mask + b*(1-mask),
	// so a=zero, b=white, mask=metallic gives white * (1-metallic).)
	const std::string nOneMinusMet = prefix + "one_minus_met";
	AddBlendPainter( nOneMinusMet.c_str(), nZero.c_str(), nWhite.c_str(), metallicName.c_str() );

	// rd = base_color * (1 - metallic) — diffuse-color mixer per glTF spec.
	// The (1 - max(F0)) split is applied inside the BSDF's schlick_f0 path.
	const std::string nRd = prefix + "rd";
	AddBlendPainter( nRd.c_str(), base_color, nZero.c_str(), nOneMinusMet.c_str() );

	// Landing 7 — KHR_materials_specular.  glTF formula:
	//   F0_dielectric = min(0.04 × specular_color × specular_factor, 1.0)
	//   F0            = lerp(F0_dielectric, baseColor, metallic)
	// With defaults specular_factor="1.0" and specular_color="none"
	// (treated as white), F0_dielectric collapses to 0.04 — bit-identical
	// to the pre-L7 path.  When EITHER is non-default, we build the
	// painter chain to compute F0_dielectric per-pixel.
	//
	// We omit the min(..., 1.0) clamp here because (a) defaults stay well
	// below 1, (b) painter-graph clamps don't compose cleanly with the
	// BlendPainter primitive without a new MinPainter, and (c) the BSDF
	// already clamps F at evaluation.  Author scenes that push specular
	// inputs > 1 will get a soft saturation rather than exact spec
	// compliance — acceptable until a use case shows otherwise.
	const bool hasSpecFactor = ( specular_factor && *specular_factor &&
	                              strcmp( specular_factor, "1.0" ) != 0 &&
	                              strcmp( specular_factor, "1" )   != 0 );
	const bool hasSpecColor  = ( specular_color  && *specular_color &&
	                              strcmp( specular_color, "none" )  != 0 );

	if( !hasSpecFactor && !hasSpecColor ) {
		// Default path: F0_dielectric = 0.04 constant (unchanged from
		// pre-L7).  Cheaper than the painter chain since it's one
		// uniform painter lookup vs. three blend evaluations per ray.
		AddUniformColorPainter( nF0Diel.c_str(), point04, "Rec709RGB_Linear" );
	} else {
		// KHR_materials_specular path: build the chain.
		const std::string specFactorName = resolveOrSynth(
			(hasSpecFactor ? specular_factor : "1.0"), "specfactor" );
		const std::string specColorName = hasSpecColor
			? std::string( specular_color )
			: nWhite;	// default tint = no tint

		// f0_aux = 0.04 × specular_color
		// = blend(specular_color, zero, point04) = specular_color * 0.04 + zero * 0.96
		const std::string nF0Aux = prefix + "f0_aux";
		AddBlendPainter( nF0Aux.c_str(), specColorName.c_str(), nZero.c_str(), nPoint04.c_str() );

		// f0_diel = f0_aux × specular_factor
		// = blend(f0_aux, zero, specular_factor) = f0_aux * factor + zero * (1-factor)
		AddBlendPainter( nF0Diel.c_str(), nF0Aux.c_str(), nZero.c_str(), specFactorName.c_str() );
	}

	// rs = lerp(F0_dielectric, base_color, metallic) = blend(base_color, f0diel, metallic).
	// In schlick_f0 mode the BSDF treats this as F0 directly.
	const std::string nRs = prefix + "rs";
	AddBlendPainter( nRs.c_str(), base_color, nF0Diel.c_str(), metallicName.c_str() );

	// rough_sq = roughness * roughness = blend(roughness, zero, roughness)
	const std::string nRoughSq = prefix + "rough_sq";
	AddBlendPainter( nRoughSq.c_str(), roughnessName.c_str(), nZero.c_str(), roughnessName.c_str() );

	// Landing 8 — KHR_materials_anisotropy.  glTF formula:
	//   α_t = mix(α, 1.0, anisotropy²)
	//   α_b = α
	// With default anisotropy_factor="0.0", α_t = α (isotropic) — bit-
	// identical to the pre-L8 path.  Non-zero factor stretches the
	// specular lobe along the tangent (X) axis.
	//
	// anisotropy_rotation is now (round-2 fix) plumbed through to
	// GGX{BRDF,SPF} via AddGGXEmissiveMaterial's `tangent_rotation`
	// parameter; the BSDF rotates the (u, v) basis around w by the
	// painter-evaluated angle before sampling.  The rotation is a
	// no-op when the parameter resolves to 0, so default-disabled
	// anisotropy materials stay bit-identical.
	const bool hasAnisotropy = ( anisotropy_factor && *anisotropy_factor &&
	                              strcmp( anisotropy_factor, "0.0" ) != 0 &&
	                              strcmp( anisotropy_factor, "0" )   != 0 );

	std::string nAlphaX = nRoughSq;
	std::string nAlphaY = nRoughSq;
	if( hasAnisotropy ) {
		const std::string anisoName = resolveOrSynth( anisotropy_factor, "aniso" );

		// aniso_sq = anisotropy²
		const std::string nAnisoSq = prefix + "aniso_sq";
		AddBlendPainter( nAnisoSq.c_str(), anisoName.c_str(), nZero.c_str(), anisoName.c_str() );

		// alpha_t = mix(α, 1, aniso²) = lerp from rough_sq to white as
		// aniso² grows.  blend(white, rough_sq, aniso_sq) = white*aniso² + rough_sq*(1-aniso²).
		const std::string nAlphaT = prefix + "alpha_t";
		AddBlendPainter( nAlphaT.c_str(), nWhite.c_str(), nRoughSq.c_str(), nAnisoSq.c_str() );

		nAlphaX = nAlphaT;	// tangent direction (stretched)
		// nAlphaY stays at nRoughSq (bitangent direction)
	}

	// Build the final GGX material with these inputs.  Extinction is zero
	// (opaque); ior is preserved but unused in schlick_f0 mode.  Emissive
	// flows through unchanged.  Anisotropy rotation flows through as the
	// `tangent_rotation` painter; "0.0" / unset → no rotation.
	//
	// alphaX / alphaY / ior / ext are scalar-typed slots on the new GGX
	// construction surface, but PBR's internal composition lives in the
	// IPainter graph (BlendPainter etc., colour-typed).  We adapt each
	// composed IPainter back into an `IScalarPainter` here via
	// `Painters/PainterToScalarAdapter.h` so the painter graph keeps
	// its existing composition without needing scalar-domain blend ops.
	IPainter* pRdPnt    = pPntManager->GetItem( nRd.c_str() );
	IPainter* pRsPnt    = pPntManager->GetItem( nRs.c_str() );
	IPainter* pAlphaXP  = pPntManager->GetItem( nAlphaX.c_str() );
	IPainter* pAlphaYP  = pPntManager->GetItem( nAlphaY.c_str() );
	IPainter* pIORPnt   = pPntManager->GetItem( nIORPnt.c_str() );
	IPainter* pZeroPnt  = pPntManager->GetItem( nZero.c_str() );

	if( !pRdPnt || !pRsPnt || !pAlphaXP || !pAlphaYP || !pIORPnt || !pZeroPnt ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddPBRMetallicRoughnessMaterial:: internal painter lookup failed for `%s`", name );
		return false;
	}

	// Adapt the four scalar slots.  Each adapter takes its own ref on
	// the underlying IPainter (inside its ctor) and is released after
	// the material steals its own ref via the GGX{BRDF,SPF} ctors.
	// `Reference`-derived objects start at refcount 1, so the local
	// pointer owns that ref; no extra addref() is needed.  (Removing
	// the explicit addref() here in 2026-05 — the prior code left
	// every adapter permanently at refcount = 2, leaking one ref per
	// PBR material instantiated.)
	IScalarPainter* pAlphaXSc = new PainterToScalarAdapter( *pAlphaXP );
	IScalarPainter* pAlphaYSc = new PainterToScalarAdapter( *pAlphaYP );
	IScalarPainter* pIORSc    = new PainterToScalarAdapter( *pIORPnt );
	IScalarPainter* pExtSc    = new PainterToScalarAdapter( *pZeroPnt );

	// Resolve the optional emissive painter.
	IPainter* pEmissive = 0;
	const bool wantEmissive = ( emissive && std::string( emissive ) != "none" );
	if( wantEmissive ) {
		pEmissive = pPntManager->GetItem( emissive );
		if( !pEmissive ) {
			GlobalLog()->PrintEx( eLog_Error,
				"Job::AddPBRMetallicRoughnessMaterial:: emissive painter `%s` not found", emissive );
			safe_release( pAlphaXSc );
			safe_release( pAlphaYSc );
			safe_release( pIORSc );
			safe_release( pExtSc );
			return false;
		}
	}

	// Landing 8: optional tangent rotation painter.
	IPainter* pTangentRotation = 0;
	if( anisotropy_rotation && std::string( anisotropy_rotation ) != "none" ) {
		pTangentRotation = pPntManager->GetItem( anisotropy_rotation );
		if( !pTangentRotation ) {
			const double fa = atof( anisotropy_rotation );
			RISE_API_CreateUniformColorPainter( &pTangentRotation, RISEPel( fa, fa, fa ) );
		} else {
			pTangentRotation->addref();
		}
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateGGXEmissiveMaterial(
		&pMaterial, *pRdPnt, *pRsPnt, *pAlphaXSc, *pAlphaYSc, *pIORSc, *pExtSc,
		pEmissive, emissive_scale,
		ResolveFresnelMode( "schlick_f0" ),
		pTangentRotation );

	const bool added = pMatManager->AddItem( pMaterial, name );
	// Only mark as composed on successful registration — see the
	// matching guard in AddGGXEmissiveMaterial for rationale.
	if( added ) {
		composedMaterialNames.insert( String( name ) );
	}

	safe_release( pMaterial );
	safe_release( pAlphaXSc );
	safe_release( pAlphaYSc );
	safe_release( pIORSc );
	safe_release( pExtSc );
	safe_release( pTangentRotation );

	return added;
}

bool Job::AddSheenMaterial(
	const char* name,
	const char* sheen_color,
	const char* sheen_roughness
	)
{
	IPainter* pColor = pPntManager->GetItem( sheen_color );
	if( !pColor ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddSheenMaterial:: sheen_color painter `%s` not found", sheen_color );
		return false;
	}

	IScalarPainter* pRoughness = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "sheen_material", name, "sheen_roughness", sheen_roughness, /*requireSingle*/ true );
	if( !pRoughness ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateSheenMaterial( &pMaterial, *pColor, *pRoughness );
	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pRoughness );
	return true;
}

bool Job::AddCookTorranceMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* facet,											///< [in] Facet distribution
	const char* ior,											///< [in] IOR delta
	const char* ext												///< [in] Extinction factor
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pFacet = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "cooktorrance_material", name, "facet", facet );
	IScalarPainter* pIOR = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "cooktorrance_material", name, "ior", ior );
	IScalarPainter* pExt = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "cooktorrance_material", name, "ext", ext );

	if( !pFacet || !pIOR || !pExt ) {
		safe_release( pFacet );
		safe_release( pIOR );
		safe_release( pExt );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateCookTorranceMaterial( &pMaterial, *pRd, *pRs, *pFacet, *pIOR, *pExt );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pFacet );
	safe_release( pIOR );
	safe_release( pExt );

	return true;
}

//! Adds Oren-Nayar material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddOrenNayarMaterial(
	const char* name,											///< [in] Name of the material
	const char* reflectance,									///< [in] Reflectance
	const char* roughness										///< [in] Roughness factor (physical scalar)
	)
{
	IPainter* pRef = pPntManager->GetItem(reflectance);

	if( !pRef ) {
		return false;
	}

	IScalarPainter* pRoughness = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "orennayar_material", name, "roughness", roughness );
	if( !pRoughness ) {
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateOrenNayarMaterial( &pMaterial, *pRef, *pRoughness );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pRoughness );

	return true;
}

//! Adds Schlick material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSchlickMaterial(
	const char* name,											///< [in] Name of the material
	const char* diffuse,										///< [in] Diffuse reflectance
	const char* specular,										///< [in] Specular reflectance
	const char* roughness,										///< [in] Roughness factor
	const char* isotropy										///< [in] Isotropy factor
	)
{
	IPainter* pRd = pPntManager->GetItem(diffuse);
	IPainter* pRs = pPntManager->GetItem(specular);

	if( !pRd || !pRs ) {
		return false;
	}

	IScalarPainter* pRoughness = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "schlick_material", name, "roughness", roughness );
	IScalarPainter* pIsotropy = ResolveOrDiagnoseScalar(
		pScalarPntManager, pPntManager, "schlick_material", name, "isotropy", isotropy );

	if( !pRoughness || !pIsotropy ) {
		safe_release( pRoughness );
		safe_release( pIsotropy );
		return false;
	}

	IMaterial* pMaterial = 0;
	RISE_API_CreateSchlickMaterial( &pMaterial, *pRd, *pRs, *pRoughness, *pIsotropy );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
	safe_release( pRoughness );
	safe_release( pIsotropy );

	return true;
}

//! Adds a data driven material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddDataDrivenMaterial(
	const char* name,											///< [in] Name of the material
	const char* filename										///< [in] Filename to load data from
	)
{
	IMaterial* pMaterial = 0;
	RISE_API_CreateDataDrivenMaterial( &pMaterial, filename );

	pMatManager->AddItem( pMaterial, name );

	safe_release( pMaterial );
    return true;
}

//! Creates a lambertian luminaire material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddLambertianLuminaireMaterial(
							const char* name,				///< [in] Name of the material
							const char* radEx,				///< [in] Radiant exitance painter
							const char* mat,				///< [in] Material to use for all non emmission properties
							const double scale				///< [in] Value to scale radiant exitance by
							)
{
	IPainter* pRadEx = pPntManager->GetItem(radEx);
	IMaterial* pMaterial = pMatManager->GetItem(mat);

	if( !pRadEx || !pMaterial ) {
		return false;
	}

	IMaterial* pLumMaterial = 0;
	RISE_API_CreateLambertianLuminaireMaterial( &pLumMaterial, *pRadEx, *pMaterial, scale );

	pMatManager->AddItem( pLumMaterial, name );
	safe_release( pLumMaterial );

	return true;
}

//! Creates a phong luminaire material
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPhongLuminaireMaterial(
							const char* name,				///< [in] Name of the material
							const char* radEx,				///< [in] Radiance exitance painter
							const char* mat,				///< [in] Material to use for all non emmission properties
							const char* N,					///< [in] Phong exponent function
							const double scale				///< [in] Value to scale radiant exitance by
							)
{
	IPainter* pRadEx = pPntManager->GetItem(radEx);
	IMaterial* pMaterial = pMatManager->GetItem(mat);

	if( !pRadEx || !pMaterial ) {
		return false;
	}

	IScalarPainter* pN = ResolveOrDiagnoseScalar( pScalarPntManager, pPntManager, "phongluminaire_material", name, "exponent", N );
	if( !pN ) {
		return false;
	}

	IMaterial* pLumMaterial = 0;
	RISE_API_CreatePhongLuminaireMaterial( &pLumMaterial, *pRadEx, *pMaterial, *pN, scale );

	pMatManager->AddItem( pLumMaterial, name );

	safe_release( pN );
	safe_release( pLumMaterial );

	return true;
}


//
// Adds geometry
//

//! Creates a box located at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBoxGeometry(
						const char* name,					///< [in] Name of the geometry
						const double width,					///< [in] Width of the box
						const double height,				///< [in] Height of the box
						const double depth					///< [in] Depth of the box
						)
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateBoxGeometry( &pGeometry, width, height, depth );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a circular disk at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCircularDiskGeometry(
								const char* name,			///< [in] Name of the geometry
								const double radius,		///< [in] Radius of the disk
								const char axis				///< [in] (x|y|z) Which axis the disk sits on
								)
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateCircularDiskGeometry( &pGeometry, radius, axis );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a clipped plane, defined by four points
/// \return TRUE if successful, FALSE otherwise
bool Job::AddClippedPlaneGeometry(
								 const char* name,			///< [in] Name of the geometry
								 const double ptA[4],		///< [in] Point A of the clipped plane
								 const double ptB[4],		///< [in] Point B of the clipped plane
								 const double ptC[4],		///< [in] Point C of the clipped plane
								 const double ptD[4],		///< [in] Point D of the clipped plane
								 const bool doublesided		///< [in] Is it doublesided?
								 )
{
	IGeometry* pGeometry = 0;
	Point3 pts[4];
	pts[0] = Point3( ptA );
	pts[1] = Point3( ptB );
	pts[2] = Point3( ptC );
	pts[3] = Point3( ptD );
	RISE_API_CreateClippedPlaneGeometry( &pGeometry, pts, doublesided );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a Cylinder at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCylinderGeometry(
							 const char* name,				///< [in] Name of the geometry
							 const char axis,				///< [in] (x|y|z) Which axis the cylinder is sitting on
							 const double radius,			///< [in] Radius of the cylinder
							 const double height			///< [in] Height of the cylinder
							 )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateCylinderGeometry( &pGeometry, axis, radius, height );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates an infinite plane that passes through the origin
/// \return TRUE if successful, FALSE otherwise
/// \todo This needs to be seriously re-evaluated
bool Job::AddInfinitePlaneGeometry(
										  const char* name,	///< [in] Name of the geometry
										  const double xt,	///< [in] How often to tile in X
										  const double yt	///< [in] How often to tile in Y
										  )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateInfinitePlaneGeometry( &pGeometry, xt, yt );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a sphere at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSphereGeometry(
								   const char* name,		///< [in] Name of the geometry
								   const double radius		///< [in] Radius of the sphere
								   )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateSphereGeometry( &pGeometry, radius );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates an ellipsoid at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddEllipsoidGeometry(
									const char* name,		///< [in] Name of the geometry
									const double radii[3]	///< [in] Radii of the ellipse
									)
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateEllipsoidGeometry( &pGeometry, Vector3(radii) );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a torus at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddTorusGeometry(
								  const char* name,			///< [in] Name of the geometry
								  const double majorRad,	///< [in] Major radius
								  const double minorRad		///< [in] Minor radius (as a percentage of the major radius)
								  )
{
	IGeometry* pGeometry = 0;
	RISE_API_CreateTorusGeometry( &pGeometry, majorRad, minorRad );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Adds a signed-distance-field (implicit) geometry from inline part lines
//! (the normal path) or an external parts file (for very large SDFs).
//! Construction (part-grammar parse + token validation + sphere-trace build)
//! is delegated to the C-API factory RISE_API_CreateSDFGeometry -- the single
//! construction boundary -- after applying the media-path search to the file
//! name (inline parts need no path resolution).  The exactly-one-source rule
//! is diagnosed HERE with the geometry's name so scene authors get chunk-level
//! context; the factory re-checks it for direct C-API callers.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddSDFGeometry( const char* name, const char* szFileName, const char* szParts, const unsigned int maxSteps, const double surfaceEpsilonFraction, const unsigned int samplingDetail )
{
	const bool hasFile  = ( szFileName && szFileName[0] && strcmp( szFileName, "none" ) != 0 );
	const bool hasParts = ( szParts && szParts[0] );
	if( hasFile == hasParts ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddSDFGeometry:: `%s`: provide exactly one SDF source -- either inline `part` lines or a `file`%s",
			name ? name : "(unnamed)",
			hasFile ? " (both were given)" : " (neither was given)" );
		return false;
	}
	IGeometry* pGeometry = 0;
	if( !RISE_API_CreateSDFGeometry( &pGeometry,
			hasFile ? GlobalMediaPathLocator().Find(szFileName).c_str() : 0,
			hasParts ? szParts : 0,
			maxSteps, surfaceEpsilonFraction, samplingDetail ) ) {
		return false;   // the factory already logged the source / line / token reason
	}
	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Heightfield SDF geometry: the exact analytic surface z = scale*field(u,v),
//! sphere-traced.  Resolves the named IFunction2D here (chunk-level context in
//! the diagnostic), then delegates construction to the C-API factory
//! RISE_API_CreateSDFHeightfieldGeometry.  Mirrors Job::AddSDFGeometry's
//! manager-add + release lifetime; the field is addref'd by the geometry.
bool Job::AddSDFHeightfieldGeometry( const char* name, const char* heightfieldFunction, const double radius, const double scale, const unsigned int maxSteps, const double surfaceEpsilonFraction, const unsigned int samplingDetail )
{
	IFunction2D* pField = pFunc2DManager->GetItem( heightfieldFunction ? heightfieldFunction : "" );
	if( !pField ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddSDFHeightfieldGeometry:: `%s`: heightfield function2d `%s` not found (declare it first)",
			name ? name : "(unnamed)", heightfieldFunction ? heightfieldFunction : "(none)" );
		return false;
	}
	IGeometry* pGeometry = 0;
	if( !RISE_API_CreateSDFHeightfieldGeometry( &pGeometry, pField, radius, scale, maxSteps, surfaceEpsilonFraction, samplingDetail ) ) {
		return false;   // the factory already logged why
	}
	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

bool Job::AddCartesianDiskGeometry( const char* name, const double radius, const int meshN )
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	if( !RISE_API_CreateCartesianDiskGeometry( &pGeometry, radius, meshN ) ) {
		return false;   // the factory already logged why
	}
	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

bool Job::AddFunction2DColorPainter( const char* name, const char* szFunction, const double scale, const double bias )
{
	IFunction2D* pFunc = pFunc2DManager->GetItem( szFunction ? szFunction : "" );
	if( !pFunc ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddFunction2DColorPainter:: `%s`: source function2d `%s` not found (declare it first)",
			name ? name : "(unnamed)", szFunction ? szFunction : "(none)" );
		return false;
	}
	IPainter* pPainter = 0;
	if( !RISE_API_CreateFunction2DColorPainter( &pPainter, pFunc, scale, bias ) ) {
		return false;
	}
	pPntManager->AddItem( pPainter, name );
	pFunc2DManager->AddItem( pPainter, name );
	safe_release( pPainter );
	return true;
}

bool Job::AddSweepGeometry( const char* name, const SweepDescriptor& desc )
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	if( !RISE_API_CreateSweepGeometry( &pGeometry, desc ) ) {
		return false;   // the factory already logged why
	}
	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

bool Job::AddPathInstancesGeometry( const char* name, const char* szTemplate, const PathInstancesDescriptor& desc )
{
	IGeometry* pTemplate = pGeomManager->GetItem( szTemplate ? szTemplate : "" );
	if( !pTemplate ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddPathInstancesGeometry:: `%s`: template geometry `%s` not found (declare it first)",
			name ? name : "(unnamed)", szTemplate ? szTemplate : "(none)" );
		return false;
	}
	PathInstancesDescriptor d = desc;
	d.pGeometry = pTemplate;
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	if( !RISE_API_CreatePathInstancesGeometry( &pGeometry, d ) ) {
		return false;   // the factory already logged why
	}
	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Adds a triangle mesh geometry from the pointers passed it
/// \return TRUE if successful, FALSE otherwise
bool Job::AddIndexedTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const float* vertices,					///< [in] List of vertices
					const float* normals,					///< [in] List of normals
					const float* coords,					///< [in] Texture co-ordinates
					const unsigned int* vertexface,			///< [in] List of the vertex faces
					const unsigned int* uvwface,			///< [in] List of the texture coord faces
					const unsigned int* normalface,			///< [in] List of normal faces
					const unsigned int numpts,				///< [in] Number of points, normals and texture coords
					const unsigned int numnormals,			///< [in] Number of normals
					const unsigned int numcoords,			///< [in] Number of texture co-ordinate points
					const unsigned int numfaces,			///< [in] Number of faces
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	if( !name || !vertices || !vertexface ) {
		return false;
	}

	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, double_sided, face_normals );

	pGeometry->BeginIndexedTriangles();

	unsigned int i=0;
	for( i=0; i<numpts; i++ ) {
		pGeometry->AddVertex( Vertex( vertices[i*3], vertices[i*3+1], vertices[i*3+2] ) );
	}

	if( normals && !face_normals ) {
		for( i=0; i<numnormals; i++ ) {
			pGeometry->AddNormal( Normal( normals[i*3], normals[i*3+1], normals[i*3+2] ) );
		}
	}

	if( coords ) {
		for( i=0; i<numcoords; i++ ) {
			pGeometry->AddTexCoord( TexCoord( coords[i*3], coords[i*3+1] ) );
		}
	} else {
		pGeometry->AddTexCoord( TexCoord( 0, 0 ) );
	}

	for( i=0; i<numfaces; i++ ) {
		IndexedTriangle tri;
		for( int j=0; j<3; j++ ) {
			tri.iVertices[j] = vertexface[i*3+j];
			if( coords && uvwface ) {
				tri.iCoords[j] = uvwface[i*3+j];
			} else {
				tri.iCoords[j] = 0;
			}

			if( normals && !face_normals && normalface ) {
				tri.iNormals[j] = normalface[i*3+j];
			} else {
				tri.iNormals[j] = tri.iVertices[j];
			}
		}

		pGeometry->AddIndexedTriangle( tri );
	}

	if( !normals && !face_normals ) {
		pGeometry->ComputeVertexNormals();
	}

	pGeometry->DoneIndexedTriangles();

	bool bRet = pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );

	return bRet;
}

//! Creates a triangle mesh geometry from a 3DS file
/// \return TRUE if successful, FALSE otherwise
bool Job::Add3DSTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* filename,					///< [in] The 3DS file to load
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, double_sided, face_normals );

	IReadBuffer* pBuffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &pBuffer, filename );

	ITriangleMeshLoaderIndexed* pLoader = 0;
	RISE_API_Create3DSTriangleMeshLoader( &pLoader, pBuffer );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	safe_release( pBuffer );
	return bRet;
}

//! Creates a triangle mesh geometry from a raw file
/// \return TRUE if successful, FALSE otherwise
bool Job::AddRAWTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const bool double_sided					///< [in] Are the triangles double sided ?
					)
{
	ITriangleMeshGeometry* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometry( &pGeometry, double_sided );

	ITriangleMeshLoader* pLoader = 0;
	RISE_API_CreateRAWTriangleMeshLoader( &pLoader, szFileName );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	return bRet;
}

//! Creates a triangle mesh geometry from a file of version 2
//! The format of the file for this version is different from the one
//! above
/// \return TRUE if successful, FALSE otherwise
bool Job::AddRAW2TriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, double_sided, face_normals );

	ITriangleMeshLoaderIndexed* pLoader = 0;
	RISE_API_CreateRAW2TriangleMeshLoader( &pLoader, szFileName );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	return bRet;
}


//! Creates a triangle mesh geometry from a ply file
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPLYTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const bool double_sided,				///< [in] Are the triangles double sided ?
					const bool bInvertFaces,				///< [in] Should the faces be inverted?
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, double_sided, face_normals );

	ITriangleMeshLoaderIndexed* pLoader = 0;
	RISE_API_CreatePLYTriangleMeshLoader( &pLoader, szFileName, bInvertFaces );

	bool bRet = pLoader->LoadTriangleMesh( pGeometry );
	if( bRet ) {
		pGeomManager->AddItem( pGeometry, name );
	}

	safe_release( pGeometry );

	safe_release( pLoader );
	return bRet;
}

//! Imports a glTF 2.0 scene.  Constructs a GLTFSceneImporter (which runs
//! the cgltf parse + buffer-load + validate once) and delegates to
//! ImportScene.  See docs/GLTF_IMPORT.md and Importers/GLTFSceneImporter.h
//! for the design.
/// \return TRUE if successful, FALSE otherwise
bool Job::ImportGLTFScene(
					const char* filename,
					const char* name_prefix,
					const unsigned int scene_index,
					const bool import_meshes,
					const bool import_materials,
					const bool import_lights,
					const bool import_cameras,
					const bool import_normal_maps,
					const bool lowmem_textures,
					const double lights_intensity_override,
					const double directional_intensity_override,
					const double point_intensity_override,
					const double spot_intensity_override,
					const bool respect_baked_occlusion,
					const double emissive_intensity_scale,
					const double emissive_tint_r,
					const double emissive_tint_g,
					const double emissive_tint_b
					)
{
	GLTFSceneImporter importer( filename );
	if( !importer.IsValid() ) {
		return false;	// constructor already logged the parse failure
	}
	GLTFImportOptions opts;
	opts.namePrefix             = name_prefix;
	opts.sceneIndex             = scene_index;
	opts.importMeshes           = import_meshes;
	opts.importMaterials        = import_materials;
	opts.importLights           = import_lights;
	opts.importCameras          = import_cameras;
	opts.importNormalMaps       = import_normal_maps;
	opts.lowmemTextures         = lowmem_textures;
	opts.lightsIntensityOverride       = lights_intensity_override;
	opts.directionalIntensityOverride  = directional_intensity_override;
	opts.pointIntensityOverride        = point_intensity_override;
	opts.spotIntensityOverride         = spot_intensity_override;
	opts.respectBakedOcclusion         = respect_baked_occlusion;
	opts.emissiveIntensityScale        = emissive_intensity_scale;
	opts.emissiveTint[0]               = emissive_tint_r;
	opts.emissiveTint[1]               = emissive_tint_g;
	opts.emissiveTint[2]               = emissive_tint_b;
	return importer.ImportScene( *this, opts );
}

//! Creates a triangle mesh geometry from a single primitive of a glTF 2.0
//! file (.gltf or .glb).  This is the entry point used by the
//! `gltf_geometry` chunk parser; the bulk `gltf_import` chunk goes through
//! ImportGLTFScene above.  Both routes go through the same
//! GLTFSceneImporter — the cgltf parse runs once per call, in the
//! importer's constructor.  See docs/GLTF_IMPORT.md for the phased plan.
/// \return TRUE if successful, FALSE otherwise
bool Job::AddGLTFTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] .gltf or .glb file to load
					const unsigned int mesh_index,			///< [in] Which mesh in the file (0-based)
					const unsigned int primitive_index,		///< [in] Which primitive within the mesh (0-based)
					const bool double_sided,				///< [in] Are the triangles double sided?
					const bool face_normals,				///< [in] Use face normals rather than vertex normals
					const bool flip_v						///< [in] Flip TEXCOORD V at load
					)
{
	GLTFSceneImporter importer( szFileName );
	if( !importer.IsValid() ) {
		return false;	// constructor already logged the parse failure
	}
	return importer.ImportPrimitive(
		*this, name, mesh_index, primitive_index,
		double_sided, face_normals, flip_v );
}

bool Job::AddPrebuiltTriangleMeshGeometry(
					const char* name,
					ITriangleMeshGeometryIndexed* pGeom
					)
{
	if( !name || !pGeom ) {
		GlobalLog()->PrintEasyError(
			"Job::AddPrebuiltTriangleMeshGeometry:: NULL name or geometry" );
		return false;
	}
	// pGeomManager->AddItem refcounts the geometry; the caller retains
	// its own reference and is responsible for releasing it.
	return pGeomManager->AddItem( pGeom, name );
}

//! Creates a mesh from a .risemesh file
/// \return TRUE if successful, FALSE otherwise
bool Job::AddRISEMeshTriangleMeshGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const bool load_into_memory,			///< [in] Do we load the entire file into memory before reading?
					const bool face_normals					///< [in] Use face normals rather than vertex normals
					)
{
	ITriangleMeshGeometryIndexed* pGeometry = 0;
	RISE_API_CreateTriangleMeshGeometryIndexed( &pGeometry, false, face_normals );

	bool bLoaded = false;

	if( load_into_memory ) {
		IMemoryBuffer* pBuffer = 0;
		RISE_API_CreateMemoryBufferFromFile( &pBuffer, szFileName );

		if( pBuffer && pBuffer->Size() > 0 ) {
			pGeometry->Deserialize( *pBuffer );
			bLoaded = pGeometry->numPoints() > 0;

			if( !bLoaded ) {
				GlobalLog()->PrintEx( eLog_Error, "Job::AddRISEMeshTriangleMeshGeometry:: Failed to deserialize valid geometry from `%s`", szFileName );
			}
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddRISEMeshTriangleMeshGeometry:: Failed to open or read `%s`", szFileName );
		}

		safe_release( pBuffer );
	} else {
		IReadBuffer* pBuffer = 0;
		RISE_API_CreateDiskFileReadBuffer( &pBuffer, szFileName );

		if( pBuffer && pBuffer->Size() > 0 ) {
			pGeometry->Deserialize( *pBuffer );
			bLoaded = pGeometry->numPoints() > 0;

			if( !bLoaded ) {
				GlobalLog()->PrintEx( eLog_Error, "Job::AddRISEMeshTriangleMeshGeometry:: Failed to deserialize valid geometry from `%s`", szFileName );
			}
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddRISEMeshTriangleMeshGeometry:: Failed to open or read `%s`", szFileName );
		}

		safe_release( pBuffer );
	}

	if( !bLoaded ) {
		safe_release( pGeometry );
		return false;
	}

	const bool bRet = pGeomManager->AddItem( pGeometry, name );
	if( !bRet ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddRISEMeshTriangleMeshGeometry:: Failed to add geometry `%s`", name );
	}

	safe_release( pGeometry );

	return bRet;
}

//! Creates a bezier patch geometry (analytic rendering always).
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBezierPatchGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int max_patches,			///< [in] Maximum number of patches per accelerator leaf
					const unsigned char max_recur,			///< [in] Maximum accelerator recursion depth
					const bool use_bsp,						///< [in] Use BSP tree (true) or Octree (false) for the patch accelerator
					const bool bCenterObject				///< [in] Recenter all patch control points around the object-space origin
					)
{
	FILE* inputFile = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "r" );

	if( !inputFile ) {
		GlobalLog()->Print( eLog_Error, "Job::AddBezierPatchGeometry:: Failed to open file" );
		return false;
	}

	IBezierPatchGeometry* pGeometry = 0;
	RISE_API_CreateBezierPatchGeometry( &pGeometry, max_patches, max_recur, use_bsp );

	// Load all patches first so we can optionally recenter them before
	// handing them to the geometry's BSP/Octree build in Prepare().
	BezierPatchesListType loadedPatches;

	char line[4096] = {0};

	if( fgets( (char*)&line, 4096, inputFile ) != NULL ) {
		// Read that first line, it tells us how many
		// patches are dealing with here
		unsigned int	numPatches = 0;
		sscanf( line, "%u", &numPatches );
		loadedPatches.reserve( numPatches );

		for( unsigned int i=0; i<numPatches; i++ )
		{
			// We assume every 16 lines gives us a patch
			BezierPatch		patch;

			for( int j=0; j<4; j++ ) {
				for( int k=0; k<4; k++ ) {
					double x, y, z;
					if( fscanf( inputFile, "%lf %lf %lf", &x, &y, &z ) == EOF ) {
						GlobalLog()->PrintSourceError( "Job::AddBezierPatchGeometry:: Fatal error while reading file.  Nothing will be loaded", __FILE__, __LINE__ );
						fclose( inputFile );
						safe_release( pGeometry );
						return false;
					}

					patch.c[j].pts[k] = Point3( x, y, z );
				}
			}

			loadedPatches.push_back( patch );
		}

		// Recenter the object around the origin by shifting every control
		// point by -bbox_center.  Mirrors the mesh-path CenterObject() in
		// GeometryUtilities — operating on control points is equivalent
		// because the Bezier surface lies inside the convex hull of its
		// control net, so the bbox of control points bounds the surface.
		if( bCenterObject && !loadedPatches.empty() ) {
			Point3 vMin(  RISE_INFINITY,  RISE_INFINITY,  RISE_INFINITY );
			Point3 vMax( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );
			for( BezierPatchesListType::const_iterator it = loadedPatches.begin(); it != loadedPatches.end(); ++it ) {
				for( int j = 0; j < 4; j++ ) {
					for( int k = 0; k < 4; k++ ) {
						const Point3& p = it->c[j].pts[k];
						if( p.x < vMin.x ) vMin.x = p.x;
						if( p.y < vMin.y ) vMin.y = p.y;
						if( p.z < vMin.z ) vMin.z = p.z;
						if( p.x > vMax.x ) vMax.x = p.x;
						if( p.y > vMax.y ) vMax.y = p.y;
						if( p.z > vMax.z ) vMax.z = p.z;
					}
				}
			}
			const Point3 center = Point3Ops::WeightedAverage2( vMax, vMin, 0.5 );
			const Vector3 offset = Vector3Ops::mkVector3( center, Point3(0,0,0) );
			for( BezierPatchesListType::iterator it = loadedPatches.begin(); it != loadedPatches.end(); ++it ) {
				for( int j = 0; j < 4; j++ ) {
					for( int k = 0; k < 4; k++ ) {
						it->c[j].pts[k] = Point3Ops::mkPoint3( it->c[j].pts[k], -offset );
					}
				}
			}
		}

		for( BezierPatchesListType::const_iterator it = loadedPatches.begin(); it != loadedPatches.end(); ++it ) {
			pGeometry->AddPatch( *it );
		}

		pGeometry->Prepare();
	}

	fclose( inputFile );

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//! Creates a bilinear patch geometry
/// \return TRUE if successful, FALSE otherwise
bool Job::AddBilinearPatchGeometry(
					const char* name,						///< [in] Name of the geometry
					const char* szFileName,					///< [in] Name of the file to load from
					const unsigned int max_polys,			///< [in] Maximum number of polygons in one octant node
					const unsigned char max_recur,			///< [in] Maximum depth of the octree or bsp tree
					const bool use_bsp						///< [in] Use a BSP tree rather than an Octree
					)
{
	FILE* inputFile = fopen( GlobalMediaPathLocator().Find(szFileName).c_str(), "r" );

	if( !inputFile ) {
		GlobalLog()->Print( eLog_Error, "Job::AddBilinearPatchGeometry:: Failed to open file" );
		return false;
	}

	IBilinearPatchGeometry* pGeometry = 0;
	RISE_API_CreateBilinearPatchGeometry( &pGeometry, max_polys, max_recur, use_bsp );

	char line[1024] = {0};

	if( fgets( (char*)&line, 1024, inputFile ) != NULL ) {
		// Read that first line, it tells us how many
		// patches are dealing with here
		unsigned int	numPatches = 0;
		sscanf( line, "%u", &numPatches );

		for( unsigned int i=0; i<numPatches; i++ )
		{
			// We assume every 16 lines gives us a patch
			BilinearPatch		patch;

			for( int j=0; j<4; j++ ) {
				// Each line is a control point
				if( fgets( (char*)&line, 1024, inputFile ) == NULL ) {
					GlobalLog()->PrintSourceError( "Job::AddBilinearPatchGeometry:: Fatal error while reading file.  Nothing will be loaded", __FILE__, __LINE__ );
					return false;
				}

				double x, y, z;
				sscanf( line, "%lf %lf %lf", &x, &y, &z );
				patch.pts[j] = Point3( x, y, z );
			}

			pGeometry->AddPatch( patch );
		}

		pGeometry->Prepare();
	}

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

bool Job::AddDisplacedGeometry(
	const char*         name,
	const char*         base_geometry_name,
	const unsigned int  detail,
	const char*         displacement,
	const Scalar        disp_scale,
	const bool          double_sided,
	const bool          face_normals,
	const bool          seam_fold
	)
{
	if( !name || !base_geometry_name ) {
		GlobalLog()->Print( eLog_Error, "Job::AddDisplacedGeometry:: name and base_geometry are required" );
		return false;
	}

	IGeometry* pBase = pGeomManager->GetItem( base_geometry_name );
	if( !pBase ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddDisplacedGeometry:: base geometry `%s` not found", base_geometry_name );
		return false;
	}

	IFunction2D* pFunc = 0;
	if( displacement && strcmp( displacement, "none" ) != 0 ) {
		pFunc = pFunc2DManager->GetItem( displacement );
		if( !pFunc ) {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddDisplacedGeometry:: displacement function `%s` not found", displacement );
			return false;
		}
	}

	IGeometry* pGeometry = 0;
	const bool bOK = RISE_API_CreateDisplacedGeometry(
		&pGeometry, pBase, detail, pFunc, disp_scale,
		double_sided, face_normals, seam_fold );

	if( !bOK || !pGeometry ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddDisplacedGeometry:: failed to create displaced geometry `%s` (base `%s` may not support tessellation)", name, base_geometry_name );
		return false;
	}

	pGeomManager->AddItem( pGeometry, name );
	safe_release( pGeometry );
	return true;
}

//
//  Adds lights
//

//! Creates a infinite point omni light, located at the origin
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPointOmniLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
	const double pos[3],									///< [in] Position of the light
	const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreatePointOmniLight( &pLight, power, sRGBPel(srgb), shootPhotons );
	pLight->SetPosition( Point3( pos ) );
	pLight->FinalizeTransformations();
	pLightManager->AddItem( pLight, name );
	BumpSceneLightGen( pScene );   // P2a: light set changed -> reused caster must rebuild
	safe_release( pLight );
	return true;
}

//! Creates a infinite point spot light
/// \return TRUE if successful, FALSE otherwise
bool Job::AddPointSpotLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
	const double foc[3],									///< [in] Point the center of the light is focussing on
	const double inner,										///< [in] Angle of the inner cone in radians
	const double outer,										///< [in] Angle of the outer cone in radians
	const double pos[3],									///< [in] Position of the light
	const bool shootPhotons									///< [in] Should this light shoot photons for photon mapping?
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreatePointSpotLight( &pLight, power, sRGBPel(srgb), Point3(foc), inner, outer, shootPhotons );
	pLight->SetPosition( Point3( pos ) );
	pLight->FinalizeTransformations();
	pLightManager->AddItem( pLight, name );
	BumpSceneLightGen( pScene );   // P2a: light set changed -> reused caster must rebuild
	safe_release( pLight );
	return true;
}

//! Creates the ambient light
/// \return TRUE if successful, FALSE otherwise
bool Job::AddAmbientLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3]									///< [in] Color of the light in a non-linear colorspace
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreateAmbientLight( &pLight, power, sRGBPel(srgb) );
	pLightManager->AddItem( pLight, name );
	BumpSceneLightGen( pScene );   // P2a: light set changed -> reused caster must rebuild
	safe_release( pLight );
	return true;
}

//! Adds an infinite directional light, shining in a particular direction
/// \return TRUE if successful, FALSE otherwise
bool Job::AddDirectionalLight(
	const char* name,										///< [in] Name of the light
	const double power,										///< [in] Power of the light in watts
	const double srgb[3],									///< [in] Color of the light in a non-linear colorspace
	const double dir[3]										///< [in] Direction of the light
	)
{
	ILightPriv* pLight = 0;
	RISE_API_CreateDirectionalLight( &pLight, power, sRGBPel(srgb), Vector3(dir) );
	pLightManager->AddItem( pLight, name );
	BumpSceneLightGen( pScene );   // P2a: light set changed -> reused caster must rebuild
	safe_release( pLight );
	return true;
}

//
// Adds functions
//

//! Adds a piecewise linear function
bool Job::AddPiecewiseLinearFunction(
	const char* name,										///< [in] Name of the function
	const double x[],										///< [in] X values of the function
	const double y[],										///< [in] Y values of the function
	const unsigned int num,									///< [in] Number of control points in the x and y arrays
	const bool bUseLUTs,									///< [in] Should the function use lookup tables
	const unsigned int lutsize								///< [in] Size of the lookup table
	)
{
	IPiecewiseFunction1D* pFunction = 0;
	RISE_API_CreatePiecewiseLinearFunction1D( &pFunction );

	for( unsigned int i=0; i<num; i++ ) {
		pFunction->addControlPoint( std::make_pair( x[i], y[i] ) );
	}

	if( bUseLUTs ) {
		pFunction->GenerateLUT( lutsize );
		pFunction->setUseLUT( true );
	}

	IPainter* pPainter = 0;
	RISE_API_CreateFunction1DSpectralPainter( &pPainter, *pFunction );

	pPntManager->AddItem( pPainter, name );
	safe_release( pPainter );

	pFunc1DManager->AddItem( pFunction, name );
	safe_release( pFunction );

	return true;
}

//! Adds a 2D piecewise linear function built up of other functions
bool Job::AddPiecewiseLinearFunction2D(
	const char* name,										///< [in] Name of the function
	const double x[],										///< [in] X values of the function
	char** y,												///< [in] Y values which is the name of other function1Ds
	const unsigned int num									///< [in] Number of control points in the x and y arrays
	)
{
	IPiecewiseFunction2D* pFunction = 0;
	RISE_API_CreatePiecewiseLinearFunction2D( &pFunction );

	for( unsigned int i=0; i<num; i++ ) {
		IFunction1D* pFunc = pFunc1DManager->GetItem( y[i] );
		if( !pFunc ) {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddPiecewiseLinearFunction2D:: Failed to find function '%s'", y[i] );
			return false;
		}
		pFunction->addControlPoint( x[i], pFunc );
	}

	pFunc2DManager->AddItem( pFunction, name );
	safe_release( pFunction );

	return true;
}

//
// Adding modifiers
//

bool Job::AddBumpMapModifier(
	const char* name,										///< [in] Name of the modifiers
	const char* func,										///< [in] The function to use as the bump generator
	const double scale,										///< [in] Factor to scale values by
	const double window										///< [in] Size of the window
	)
{
	IFunction2D* pFunc = pFunc2DManager->GetItem( func );
	if( !pFunc ) {
		return false;
	}

	IRayIntersectionModifier* pModifier = 0;
	RISE_API_CreateBumpMapModifier( &pModifier, *pFunc, scale, window );

	pModManager->AddItem( pModifier, name );
	safe_release( pModifier );
	return true;
}

bool Job::AddNormalMapModifier(
	const char* name,										///< [in] Name of the modifier
	const char* painter,									///< [in] Linear-RGB normal-map painter
	const double scale										///< [in] glTF normalTexture.scale
	)
{
	IPainter* pPainter = pPntManager->GetItem( painter );
	if( !pPainter ) {
		GlobalLog()->PrintEx( eLog_Error,
			"Job::AddNormalMapModifier:: painter `%s` not found", painter );
		return false;
	}

	IRayIntersectionModifier* pModifier = 0;
	RISE_API_CreateNormalMapModifier( &pModifier, *pPainter, scale );

	pModManager->AddItem( pModifier, name );
	safe_release( pModifier );
	return true;
}

//
// Adding objects
//

//! Adds an object
/// \return TRUE if successful, FALSE otherwise
bool Job::AddObject(
	const char* name,										///< [in] Name of the object
	const char* geom,										///< [in] Name of the geometry for the object
	const char* material,									///< [in] Name of the material
	const char* modifier,									///< [in] Name of the modifier
	const char* shader,										///< [in] Name of the shader
	const RadianceMapConfig& radianceMapConfig,				///< [in] Per-object radiance map (IBL); `isBackground` ignored here
	const double pos[3],									///< [in] Position of the object
	const double orient[3],									///< [in] Orientation of the object
	const double scale[3],									///< [in] Object scaling
	const bool bCastsShadows,								///< [in] Does the object cast shadows?
	const bool bReceivesShadows								///< [in] Does the object receive shadows?
   )
{
	IGeometry* pGeometry = pGeomManager->GetItem( geom );

	if( !pGeometry ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddObject:: Geometry not found `%s`", geom );
		return false;
	}

	IObjectPriv* object = 0;
	RISE_API_CreateObject( &object, pGeometry );

	object->SetShadowParams( bCastsShadows, bReceivesShadows );

	if( material ) {
		IMaterial* pMat = pMatManager->GetItem(material);
		if( pMat ) {
			object->AssignMaterial( *pMat );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Material not found `%s`", material );
			return false;
		}
	}

	if( modifier ) {
		IRayIntersectionModifier* pMod = pModManager->GetItem(modifier);
		if( pMod ) {
			object->AssignModifier( *pMod );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Modifier not found `%s`", modifier );
			return false;
		}
	}

	if( shader ) {
		IShader* pShader = pShaderManager->GetItem(shader);
		if( pShader ) {
			object->AssignShader( *pShader );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Shader not found `%s`", shader );
			return false;
		}
	}

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* pPnt = pPntManager->GetItem( radianceMapConfig.name.c_str() );
		if( pPnt ) {
			IRadianceMap* pRadianceMap = 0;
			RISE_API_CreateRadianceMap( &pRadianceMap, *pPnt, radianceMapConfig.scale );
			pRadianceMap->SetOrientation( Vector3( radianceMapConfig.orientation ) );
			object->AssignRadianceMap( *pRadianceMap );
			safe_release( pRadianceMap );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Painter for radiance map not found `%s`", radianceMapConfig.name.c_str() );
			return false;
		}
	}

	object->SetPosition( Point3( pos ) );
	object->SetOrientation( Vector3( orient ) );
	object->SetStretch( Vector3( scale[0], scale[1], scale[2] ) );
	object->FinalizeTransformations();

	pObjectManager->AddItem( object, name );
	if( object->GetMaterial() && object->GetMaterial()->GetEmitter() )
		BumpSceneLightGen( pScene );   // P2a: added an emissive object (mesh luminary)
	safe_release( object );

	return true;
}

bool Job::AddObjectMatrix(
	const char* name,
	const char* geom,
	const char* material,
	const char* modifier,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const double matrix[16],
	const bool bCastsShadows,
	const bool bReceivesShadows
	)
{
	IGeometry* pGeometry = pGeomManager->GetItem( geom );
	if( !pGeometry ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddObjectMatrix:: Geometry not found `%s`", geom );
		return false;
	}

	IObjectPriv* object = 0;
	RISE_API_CreateObject( &object, pGeometry );
	object->SetShadowParams( bCastsShadows, bReceivesShadows );

	if( material ) {
		IMaterial* pMat = pMatManager->GetItem(material);
		if( pMat ) {
			object->AssignMaterial( *pMat );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObjectMatrix:: Material not found `%s`", material );
			safe_release( object );
			return false;
		}
	}
	if( modifier ) {
		IRayIntersectionModifier* pMod = pModManager->GetItem(modifier);
		if( pMod ) {
			object->AssignModifier( *pMod );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObjectMatrix:: Modifier not found `%s`", modifier );
			safe_release( object );
			return false;
		}
	}
	if( shader ) {
		IShader* pShader = pShaderManager->GetItem(shader);
		if( pShader ) {
			object->AssignShader( *pShader );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObjectMatrix:: Shader not found `%s`", shader );
			safe_release( object );
			return false;
		}
	}
	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* pPnt = pPntManager->GetItem( radianceMapConfig.name.c_str() );
		if( pPnt ) {
			IRadianceMap* pRadianceMap = 0;
			RISE_API_CreateRadianceMap( &pRadianceMap, *pPnt, radianceMapConfig.scale );
			pRadianceMap->SetOrientation( Vector3( radianceMapConfig.orientation ) );
			object->AssignRadianceMap( *pRadianceMap );
			safe_release( pRadianceMap );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObjectMatrix:: Painter for radiance map not found `%s`", radianceMapConfig.name.c_str() );
			safe_release( object );
			return false;
		}
	}

	// Build a Matrix4 from the 16-double column-major input.  RISE's
	// Matrix4 indexes `_<i><j>` as (column=i, row=j) — confirmed by
	// `Matrix4Ops::Translation`, which writes the translation column
	// to `_30 / _31 / _32` (column 3, rows 0/1/2), and by the matrix
	// multiplication rule `ret._00 = a._00*b._00 + a._10*b._01 + ...`
	// which expands as the standard column-major matmul.  glTF stores
	// `nodes[i].matrix` column-major, so element `c*4 + r` (column c,
	// row r) maps directly to `_<c><r>`.
	Matrix4 mx;
	mx._00 = matrix[ 0]; mx._01 = matrix[ 1]; mx._02 = matrix[ 2]; mx._03 = matrix[ 3];
	mx._10 = matrix[ 4]; mx._11 = matrix[ 5]; mx._12 = matrix[ 6]; mx._13 = matrix[ 7];
	mx._20 = matrix[ 8]; mx._21 = matrix[ 9]; mx._22 = matrix[10]; mx._23 = matrix[11];
	mx._30 = matrix[12]; mx._31 = matrix[13]; mx._32 = matrix[14]; mx._33 = matrix[15];

	// Use the transform stack to push the matrix as the world transform.
	// FinalizeTransformations composes (P*O*Stretch*Scale) * stack-bottom,
	// so we leave P/O/Stretch/Scale as identity and push the supplied
	// matrix onto the (initially empty) stack.
	object->ClearAllTransforms();
	object->PushTopTransStack( mx );
	object->FinalizeTransformations();

	pObjectManager->AddItem( object, name );
	if( object->GetMaterial() && object->GetMaterial()->GetEmitter() )
		BumpSceneLightGen( pScene );   // P2a: added an emissive object (mesh luminary)
	safe_release( object );

	return true;
}

///////////////////////////////////////////////////////////
// Participating media
///////////////////////////////////////////////////////////

bool Job::AddHomogeneousMedium(
	const char* name,										///< [in] Name of the medium
	const double sigma_a[3],								///< [in] Absorption coefficient (linear RGB)
	const double sigma_s[3],								///< [in] Scattering coefficient (linear RGB)
	const char* phase_type,									///< [in] Phase function type ("isotropic" or "hg")
	const double phase_g									///< [in] Asymmetry factor for HG (ignored for isotropic)
	)
{
	// Create the phase function
	IPhaseFunction* pPhase = 0;

	if( strcmp( phase_type, "isotropic" ) == 0 ) {
		RISE_API_CreateIsotropicPhaseFunction( &pPhase );
	} else if( strcmp( phase_type, "hg" ) == 0 ) {
		RISE_API_CreateHenyeyGreensteinPhaseFunction( &pPhase, phase_g );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddHomogeneousMedium:: Unknown phase function type `%s`", phase_type );
		return false;
	}

	// Create the medium
	IMedium* pMedium = 0;
	RISE_API_CreateHomogeneousMedium( &pMedium,
		RISEPel( sigma_a[0], sigma_a[1], sigma_a[2] ),
		RISEPel( sigma_s[0], sigma_s[1], sigma_s[2] ),
		*pPhase );

	safe_release( pPhase );

	// Store in our map
	MediumMap::iterator existing = mediaMap.find( name );
	if( existing != mediaMap.end() ) {
		safe_release( existing->second );
		existing->second = pMedium;
	} else {
		mediaMap[name] = pMedium;
	}

	return true;
}

bool Job::AddHeterogeneousMedium(
	const char* name,
	const double max_sigma_a[3],
	const double max_sigma_s[3],
	const double emission[3],
	const char* phase_type,
	const double phase_g,
	const char* szVolumeFilePattern,
	const unsigned int volWidth,
	const unsigned int volHeight,
	const unsigned int volStartZ,
	const unsigned int volEndZ,
	const char accessor,
	const double bboxMin[3],
	const double bboxMax[3]
	)
{
	// Create the phase function
	IPhaseFunction* pPhase = 0;

	if( strcmp( phase_type, "isotropic" ) == 0 ) {
		RISE_API_CreateIsotropicPhaseFunction( &pPhase );
	} else if( strcmp( phase_type, "hg" ) == 0 ) {
		RISE_API_CreateHenyeyGreensteinPhaseFunction( &pPhase, phase_g );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddHeterogeneousMedium:: Unknown phase function type `%s`", phase_type );
		return false;
	}

	// Create the medium (with or without emission)
	IMedium* pMedium = 0;
	const RISEPel emissionPel( emission[0], emission[1], emission[2] );

	if( ColorMath::MaxValue( emissionPel ) > 0 ) {
		RISE_API_CreateHeterogeneousMediumWithEmission( &pMedium,
			RISEPel( max_sigma_a[0], max_sigma_a[1], max_sigma_a[2] ),
			RISEPel( max_sigma_s[0], max_sigma_s[1], max_sigma_s[2] ),
			emissionPel, *pPhase,
			szVolumeFilePattern, volWidth, volHeight, volStartZ, volEndZ,
			accessor,
			Point3( bboxMin[0], bboxMin[1], bboxMin[2] ),
			Point3( bboxMax[0], bboxMax[1], bboxMax[2] ) );
	} else {
		RISE_API_CreateHeterogeneousMedium( &pMedium,
			RISEPel( max_sigma_a[0], max_sigma_a[1], max_sigma_a[2] ),
			RISEPel( max_sigma_s[0], max_sigma_s[1], max_sigma_s[2] ),
			*pPhase,
			szVolumeFilePattern, volWidth, volHeight, volStartZ, volEndZ,
			accessor,
			Point3( bboxMin[0], bboxMin[1], bboxMin[2] ),
			Point3( bboxMax[0], bboxMax[1], bboxMax[2] ) );
	}

	safe_release( pPhase );

	// Store in our map
	MediumMap::iterator existing = mediaMap.find( name );
	if( existing != mediaMap.end() ) {
		safe_release( existing->second );
		existing->second = pMedium;
	} else {
		mediaMap[name] = pMedium;
	}

	return true;
}

bool Job::AddPainterHeterogeneousMedium(
	const char* name,
	const double max_sigma_a[3],
	const double max_sigma_s[3],
	const double emission[3],
	const char* phase_type,
	const double phase_g,
	const char* density_painter,
	const unsigned int virtualResolution,
	const char colorToScalar,
	const double bboxMin[3],
	const double bboxMax[3]
	)
{
	// Look up the painter
	IPainter* pPainter = pPntManager->GetItem( density_painter );
	if( !pPainter ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddPainterHeterogeneousMedium:: Painter not found `%s`", density_painter );
		return false;
	}

	// Create the phase function
	IPhaseFunction* pPhase = 0;

	if( strcmp( phase_type, "isotropic" ) == 0 ) {
		RISE_API_CreateIsotropicPhaseFunction( &pPhase );
	} else if( strcmp( phase_type, "hg" ) == 0 ) {
		RISE_API_CreateHenyeyGreensteinPhaseFunction( &pPhase, phase_g );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddPainterHeterogeneousMedium:: Unknown phase function type `%s`", phase_type );
		return false;
	}

	// Create the medium (with or without emission)
	IMedium* pMedium = 0;
	const RISEPel emissionPel( emission[0], emission[1], emission[2] );

	if( ColorMath::MaxValue( emissionPel ) > 0 ) {
		RISE_API_CreatePainterHeterogeneousMediumWithEmission( &pMedium,
			RISEPel( max_sigma_a[0], max_sigma_a[1], max_sigma_a[2] ),
			RISEPel( max_sigma_s[0], max_sigma_s[1], max_sigma_s[2] ),
			emissionPel, *pPhase,
			*pPainter, virtualResolution, colorToScalar,
			Point3( bboxMin[0], bboxMin[1], bboxMin[2] ),
			Point3( bboxMax[0], bboxMax[1], bboxMax[2] ) );
	} else {
		RISE_API_CreatePainterHeterogeneousMedium( &pMedium,
			RISEPel( max_sigma_a[0], max_sigma_a[1], max_sigma_a[2] ),
			RISEPel( max_sigma_s[0], max_sigma_s[1], max_sigma_s[2] ),
			*pPhase,
			*pPainter, virtualResolution, colorToScalar,
			Point3( bboxMin[0], bboxMin[1], bboxMin[2] ),
			Point3( bboxMax[0], bboxMax[1], bboxMax[2] ) );
	}

	safe_release( pPhase );

	// Store in our map
	MediumMap::iterator existing = mediaMap.find( name );
	if( existing != mediaMap.end() ) {
		safe_release( existing->second );
		existing->second = pMedium;
	} else {
		mediaMap[name] = pMedium;
	}

	return true;
}

bool Job::SetGlobalMedium(
	const char* name										///< [in] Name of a previously added medium
	)
{
	MediumMap::iterator it = mediaMap.find( name );
	if( it == mediaMap.end() ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetGlobalMedium:: Medium not found `%s`", name );
		return false;
	}

	pScene->SetGlobalMedium( it->second );
	return true;
}

bool Job::SetObjectInteriorMedium(
	const char* object_name,								///< [in] Name of the object
	const char* medium_name									///< [in] Name of the medium
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( object_name );
	if( !pObj ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetObjectInteriorMedium:: Object not found `%s`", object_name );
		return false;
	}

	MediumMap::iterator it = mediaMap.find( medium_name );
	if( it == mediaMap.end() ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetObjectInteriorMedium:: Medium not found `%s`", medium_name );
		return false;
	}

	pObj->AssignInteriorMedium( *(it->second) );
	return true;
}

const IMedium* Job::GetMedium( const char* name ) const
{
	if( !name ) return 0;
	MediumMap::const_iterator it = mediaMap.find( String( name ) );
	return it == mediaMap.end() ? 0 : it->second;
}

void Job::EnumerateMediumNames( IEnumCallback<const char*>& cb ) const
{
	for( MediumMap::const_iterator it = mediaMap.begin(); it != mediaMap.end(); ++it ) {
		const char* n = it->first.c_str();
		if( !cb( n ) ) return;
	}
}

const IGeometry* Job::GetGeometry( const char* name ) const
{
	if( !name || !pGeomManager ) return 0;
	return pGeomManager->GetItem( name );
}

void Job::EnumerateGeometryNames( IEnumCallback<const char*>& cb ) const
{
	if( !pGeomManager ) return;
	pGeomManager->EnumerateItemNames( cb );
}

bool Job::IsMaterialComposed( const char* name ) const
{
	if( !name ) return false;
	return composedMaterialNames.find( String( name ) ) != composedMaterialNames.end();
}

//! Creates a CSG object
/// \return TRUE if successful, FALSE otherwise
bool Job::AddCSGObject(
	const char* name,										///< [in] Name of the object
	const char* objA,										///< [in] Name of the first object
	const char* objB,										///< [in] Name of the second object
	const char op,											///< [in] CSG operation
															///< 0 -> Union
															///< 1 -> Intersection
															///< 2 -> A-B
															///< 3 -> B-A
	const char* material,									///< [in] Name of the material
	const char* modifier,									///< [in] Name of the modifier
	const char* shader,										///< [in] Name of the shader
	const RadianceMapConfig& radianceMapConfig,				///< [in] Per-object radiance map (IBL); `isBackground` ignored here
	const double pos[3],									///< [in] Position of the object
	const double orient[3],									///< [in] Orientation of the object
	const bool bCastsShadows,								///< [in] Does the object cast shadows?
	const bool bReceivesShadows								///< [in] Does the object receive shadows?
	)
{
	IObjectPriv* object = 0;
	RISE_API_CreateCSGObject(
		&object,
		pObjectManager->GetItem(objA),
		pObjectManager->GetItem(objB),
		op );

	object->SetShadowParams( bCastsShadows, bReceivesShadows );

	if( material ) {
		IMaterial* pMat = pMatManager->GetItem(material);
		if( pMat ) {
			object->AssignMaterial( *pMat );
		}
	}

	if( modifier ) {
		IRayIntersectionModifier* pMod = pModManager->GetItem(modifier);
		if( pMod ) {
			object->AssignModifier( *pMod );
		}
	}

	if( shader ) {
		IShader* pShader = pShaderManager->GetItem(shader);
		if( pShader ) {
			object->AssignShader( *pShader );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddObject:: Shader not found `%s`", modifier );
		}
	}

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* pPnt = pPntManager->GetItem( radianceMapConfig.name.c_str() );
		if( pPnt ) {
			IRadianceMap* pRadianceMap = 0;
			RISE_API_CreateRadianceMap( &pRadianceMap, *pPnt, radianceMapConfig.scale );
			pRadianceMap->SetOrientation( Vector3( radianceMapConfig.orientation ) );
			object->AssignRadianceMap( *pRadianceMap );
			safe_release( pRadianceMap );
			safe_release( pPnt );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddCSGObject:: Painter for radiance map not found `%s`", radianceMapConfig.name.c_str() );
		}
	}

	object->SetPosition( Point3( pos ) );
	object->SetOrientation( Vector3( orient ) );
	object->FinalizeTransformations();

	pObjectManager->AddItem( object, name );
	if( object->GetMaterial() && object->GetMaterial()->GetEmitter() )
		BumpSceneLightGen( pScene );   // P2a: added an emissive object (mesh luminary)
	safe_release( object );

	return true;
}

//
// Adds ShaderOps
//
bool Job::AddReflectionShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateReflectionShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddRefractionShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateRefractionShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddEmissionShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateEmissionShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddDirectLightingShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const char* bsdf										///< [in] BSDF to use when computing radiance (overrides object BSDF)
	)
{
	IMaterial* pBSDF = 0;
	if( bsdf ) {
		pBSDF = pMatManager->GetItem( bsdf );
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateDirectLightingShaderOp( &pShaderOp, pBSDF );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddCausticPelPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateCausticPelPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddCausticSpectralPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateCausticSpectralPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddGlobalPelPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateGlobalPelPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddGlobalSpectralPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateGlobalSpectralPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddTranslucentPelPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateTranslucentPelPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddShadowPhotonMapShaderOp(
	const char* name										///< [in] Name of the shaderop
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateShadowPhotonMapShaderOp( &pShaderOp );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddDistributionTracingShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int samples,								///< [in] Number of sample to use in distribution
	const bool irradiancecaching,							///< [in] Should irradiance caching be used if available?
	const bool forcecheckemitters,							///< [in] Force rays allowing to hit emitters even though the material may have a BRDF
	const bool reflections,									///< [in] Should reflections be traced?
	const bool refractions,									///< [in] Should refractions be traced?
	const bool diffuse,										///< [in] Should diffuse rays be traced?
	const bool translucents									///< [in] Should translucent rays be traced?
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateDistributionTracingShaderOp( &pShaderOp, samples, irradiancecaching, forcecheckemitters, reflections, refractions, diffuse, translucents );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddFinalGatherShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numtheta,							///< [in] Number of samples in the theta direction
	const unsigned int numphi,								///< [in] Number of samples in the phi direction
	const bool cachegradients,								///< [in] Should cache gradients be used in the irradiance cache?
	const unsigned int min_effective_contributors,			///< [in] Minimum effective contributors required for interpolation
	const double high_variation_reuse_scale,				///< [in] Minimum reuse scale for bright high-variation cache records
	const bool cache										///< [in] Should the rasterizer state cache be used?
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateFinalGatherShaderOp( &pShaderOp, numtheta, numphi, cachegradients, min_effective_contributors, high_variation_reuse_scale, cache );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddPathTracingShaderOp(
	const char* name,
	const bool smsEnabled,
	const unsigned int smsMaxIterations,
	const double smsThreshold,
	const unsigned int smsMaxChainDepth,
	const bool smsBiased
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreatePathTracingShaderOp( &pShaderOp, smsEnabled, smsMaxIterations, smsThreshold, smsMaxChainDepth, smsBiased );
	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddSMSShaderOp(
	const char* name,
	const unsigned int maxIterations,
	const double threshold,
	const unsigned int maxChainDepth,
	const bool biased
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateSMSShaderOp( &pShaderOp, maxIterations, threshold, maxChainDepth, biased );
	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddAmbientOcclusionShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numtheta,							///< [in] Number of samples in the theta direction
	const unsigned int numphi,								///< [in] Number of samples in the phi direction
	const bool multiplybrdf,								///< [in] Should individual samples be multiplied by the BRDF ?
	const bool irradiance_cache								///< [in] Should the irradiance state cache be used?
	)
{
	IShaderOp* pShaderOp = 0;
	RISE_API_CreateAmbientOcclusionShaderOp( &pShaderOp, numtheta, numphi, multiplybrdf, irradiance_cache );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddSimpleSubSurfaceScatteringShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numPoints,							///< [in] Number of points to use in sampling
	const double error,										///< [in] Error tolerance for bounding the number of point samples
	const unsigned int maxPointsPerNode,					///< [in] Maximum number of points / octree node
	const unsigned char maxDepth,							///< [in] Maximum depth of the octree
	const double irrad_scale,								///< [in] Irradiance scale factor
	const double geometric_scale,							///< [in] Geometric scale factor
	const bool multiplyBSDF,								///< [in] Should the BSDF be evaluated at the point of exitance?
	const bool regenerate,									///< [in] Regenerate the point set on reset calls?
	const char* shader,										///< [in] Shader to use for irradiance calculations
	const bool cache,										///< [in] Should the rasterizer state cache be used?
	const bool low_discrepancy,								///< [in] Should use a low discrepancy sequence during sample point generation?
	const double extinction[3]								///< [in] Extinction in mm^-1
	)
{
	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddSimpleSubSurfaceScatteringShaderOp:: Shader not found '%s'", shader );
		return false;
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateSimpleSubSurfaceScatteringShaderOp( &pShaderOp, numPoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, *pShader, cache, low_discrepancy, RISEPel(extinction) );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddDiffusionApproximationSubSurfaceScatteringShaderOp(
	const char* name,										///< [in] Name of the shaderop
	const unsigned int numPoints,							///< [in] Number of points to use in sampling
	const double error,										///< [in] Error tolerance for bounding the number of point samples
	const unsigned int maxPointsPerNode,					///< [in] Maximum number of points / octree node
	const unsigned char maxDepth,							///< [in] Maximum depth of the octree
	const double irrad_scale,								///< [in] Irradiance scale factor
	const double geometric_scale,							///< [in] Geometric scale factor
	const bool multiplyBSDF,								///< [in] Should the BSDF be evaluated at the point of exitance?
	const bool regenerate,									///< [in] Regenerate the point set on reset calls?
	const char* shader,										///< [in] Shader to use for irradiance calculations
	const bool cache,										///< [in] Should the rasterizer state cache be used?
	const bool low_discrepancy,								///< [in] Should use a low discrepancy sequence during sample point generation?
	const double scattering[3],								///< [in] Scattering coefficient in mm^-1
	const double absorption[3],								///< [in] Absorption coefficient in mm^-1
	const double ior,										///< [in] Index of refraction ratio
	const double g											///< [in] Scattering asymmetry
	)
{
	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddDiffusionApproximationSubSurfaceScatteringShaderOp:: Shader not found '%s'", shader );
		return false;
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateDiffusionApproximationSubSurfaceScatteringShaderOp( &pShaderOp, numPoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, *pShader, cache, low_discrepancy, RISEPel(scattering), RISEPel(absorption), ior, g );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddDonnerJensenSkinSSSShaderOp(
	const char* name,
	const unsigned int numPoints,
	const double error,
	const unsigned int maxPointsPerNode,
	const unsigned char maxDepth,
	const double irrad_scale,
	const char* shader,
	const bool cache,
	const double melanin_fraction,
	const double melanin_blend,
	const double hemoglobin_epidermis,
	const double carotene_fraction,
	const double hemoglobin_dermis,
	const double epidermis_thickness,
	const double ior_epidermis,
	const double ior_dermis,
	const double blood_oxygenation,
	const char* melanin_fraction_offset,
	const char* hemoglobin_epidermis_offset,
	const char* hemoglobin_dermis_offset
	)
{
	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddDonnerJensenSkinSSSShaderOp:: Shader not found '%s'", shader );
		return false;
	}

	// Resolve optional offset painters (null if not found or empty string)
	IPainter* pOffMel = (melanin_fraction_offset && melanin_fraction_offset[0]) ?
		pPntManager->GetItem( melanin_fraction_offset ) : 0;
	IPainter* pOffHbEpi = (hemoglobin_epidermis_offset && hemoglobin_epidermis_offset[0]) ?
		pPntManager->GetItem( hemoglobin_epidermis_offset ) : 0;
	IPainter* pOffHbDerm = (hemoglobin_dermis_offset && hemoglobin_dermis_offset[0]) ?
		pPntManager->GetItem( hemoglobin_dermis_offset ) : 0;

	IShaderOp* pShaderOp = new Implementation::DonnerJensenSkinSSSShaderOp(
		numPoints, error, maxPointsPerNode, maxDepth, irrad_scale,
		*pShader, cache,
		melanin_fraction, melanin_blend, hemoglobin_epidermis,
		carotene_fraction, hemoglobin_dermis, epidermis_thickness,
		ior_epidermis, ior_dermis, blood_oxygenation,
		pOffMel, pOffHbEpi, pOffHbDerm );
	GlobalLog()->PrintNew( pShaderOp, __FILE__, __LINE__, "donner jensen skin sss shaderop" );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddAreaLightShaderOp(
		const char* name,										///< [in] Name of the shaderop
		const double width,										///< [in] Width of the light source
		const double height,									///< [in] Height of the light source
		const double location[3],								///< [in] Where is the light source located
		const double dir[3],									///< [in] What is the light source focussed on
		const unsigned int samples,								///< [in] Number of samples to take
		const char* emm,										///< [in] Emission painter of this light
		const double power,										///< [in] Power scale
		const char* N,											///< [in] Phong factor for focussing the light
		const double hotSpot,									///< [in] Angle in radians of the light's hot spot
		const bool cache										///< [in] Should the rasterizer state cache be used?
		)

{
	IPainter* pEmm = pPntManager->GetItem( emm );
	if( !pEmm ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddAreaLightShaderOp: Painter not found '%s'", emm );
		return false;
	}

	IPainter*		pN = pPntManager->GetItem( N );

	if( !pN )
	{
		double fn = atof(N);
		RISE_API_CreateUniformColorPainter( &pN, RISEPel(fn,fn,fn) );
	} else {
		pN->addref();
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateAreaLightShaderOp( &pShaderOp, width, height, Point3(location), Vector3Ops::Normalize(Vector3(dir)), samples, *pEmm, power, *pN, hotSpot, cache );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	safe_release( pN );
	return true;
}

bool Job::AddTransparencyShaderOp(
		const char* name,										///< [in] Name of the shaderop
		const char* transparency,								///< [in] Transparency painter
		const bool one_sided									///< [in] One sided transparency only (ignore backfaces)
		)
{
	IPainter* pTrans = pPntManager->GetItem( transparency );
	if( !pTrans ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddTransparencyShaderOp: Painter not found '%s'", transparency );
		return false;
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateTransparencyShaderOp( &pShaderOp, *pTrans, one_sided );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

bool Job::AddAlphaTestShaderOp(
		const char* name,
		const char* alpha_painter,
		const double cutoff
		)
{
	IPainter* pAlpha = pPntManager->GetItem( alpha_painter );
	if( !pAlpha ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::AddAlphaTestShaderOp: Painter not found '%s'", alpha_painter );
		return false;
	}

	IShaderOp* pShaderOp = 0;
	RISE_API_CreateAlphaTestShaderOp( &pShaderOp, *pAlpha, cutoff );

	pShaderOpManager->AddItem( pShaderOp, name );
	safe_release( pShaderOp );
	return true;
}

//
// Adds Shaders
//

bool Job::AddStandardShader(
	const char* name,										///< [in] Name of the shader
	const unsigned int count,								///< [in] Number of shaderops
	const char** shaderops									///< [in] All of the shaderops
	)
{
	std::vector<IShaderOp*> shops;

	// Include DefaultEmission first so that emitters are visible in the
	// rendered image.  Skip if:
	// (a) the scene file already includes DefaultEmission explicitly, or
	// (b) any shaderop in the chain handles emission internally
	//     (e.g., MISPathTracingShaderOp has its own emission + MIS logic)
	bool hasEmission = false;
	for( unsigned int i=0; i<count; i++ ) {
		if( strcmp( shaderops[i], "DefaultEmission" ) == 0 ) {
			hasEmission = true;
			break;
		}
		IShaderOp* pSO = pShaderOpManager->GetItem( shaderops[i] );
		if( pSO && pSO->HandlesEmission() ) {
			hasEmission = true;
			break;
		}
	}

	if( !hasEmission ) {
		IShaderOp* pEmission = pShaderOpManager->GetItem( "DefaultEmission" );
		if( pEmission ) {
			shops.push_back( pEmission );
		}
	}

	for( unsigned int i=0; i<count; i++ ) {
		IShaderOp* pShaderOp = pShaderOpManager->GetItem( shaderops[i] );
		if( pShaderOp ) {
			shops.push_back( pShaderOp );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddStandardShader:: The ShaderOp '%s' not found, failed to add shader", shaderops[i] );
			return false;
		}
	}

	// Check for incompatible shader op combinations
	{
		bool hasPathTracing = false;
		bool hasDirectLighting = false;
		bool hasDistributionTracing = false;
		bool hasFinalGather = false;

		for( unsigned int i=0; i<shops.size(); i++ ) {
			if( dynamic_cast<PathTracingShaderOp*>(shops[i]) ) hasPathTracing = true;
			if( dynamic_cast<DirectLightingShaderOp*>(shops[i]) ) hasDirectLighting = true;
			if( dynamic_cast<DistributionTracingShaderOp*>(shops[i]) ) hasDistributionTracing = true;
			if( dynamic_cast<FinalGatherShaderOp*>(shops[i]) ) hasFinalGather = true;
		}

		if( hasPathTracing && hasDirectLighting ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"Shader '%s': PathTracing already includes direct lighting via NEE. "
				"Stacking with DirectLighting will double-count direct illumination.", name );
		}
		if( hasPathTracing && hasDistributionTracing ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"Shader '%s': PathTracing already handles all scattering. "
				"Stacking with DistributionTracing will double-count contributions.", name );
		}
		if( hasPathTracing && hasFinalGather ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"Shader '%s': PathTracing already handles global illumination. "
				"Stacking with FinalGather will double-count indirect lighting.", name );
		}
		if( hasDirectLighting && hasFinalGather ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"Shader '%s': FinalGather includes direct lighting contributions. "
				"Stacking with DirectLighting may double-count direct illumination.", name );
		}
	}

	IShader* pShader = 0;

	RISE_API_CreateStandardShader( &pShader, shops );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

bool Job::AddAdvancedShader(
		const char* name,										///< [in] Name of the shader
		const unsigned int count,								///< [in] Number of shaderops
		const char** shaderops,									///< [in] All of the shaderops
		const unsigned int* mindepths,							///< [in] All of the minimum depths for the shaderops
		const unsigned int* maxdepths,							///< [in] All of the maximum depths for the shaderops
		const char* operations									///< [in] All the operations for the shaderops
		)
{
	std::vector<IShaderOp*> shops;
	std::vector<unsigned int> mins, maxs;

	for( unsigned int i=0; i<count; i++ ) {
		IShaderOp* pShaderOp = pShaderOpManager->GetItem( shaderops[i] );
		if( pShaderOp ) {
			shops.push_back( pShaderOp );
			mins.push_back( mindepths[i] );
			maxs.push_back( maxdepths[i] );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Job::AddStandardShader:: The ShaderOp '%s' not found, failed to add shader", shaderops[i] );
			return false;
		}
	}

	IShader* pShader = 0;

	RISE_API_CreateAdvancedShader( &pShader, shops, mins, maxs, operations );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

bool Job::AddDirectVolumeRenderingShader(
	const char* name,										///< [in] Name of the shader
	const char* szVolumeFilePattern,						///< [in] File pattern for volume data
	const unsigned int width,								///< [in] Width of the volume
	const unsigned int height,								///< [in] Height of the volume
	const unsigned int startz,								///< [in] Starting z value for volume data
	const unsigned int endz,								///< [in] Ending z value for the volume data
	const char accessor,									///< [in] Type of volume accessor
	const char gradient,									///< [in] Type of gradient estimator to use
	const char composite,									///< [in] Type of composite operation to use
	const double dThresholdStart,							///< [in] Start of ISO threshold value (for ISO renderings only)
	const double dThresholdEnd,								///< [in] End of ISO threshold value (for ISO renderings only)
	const char sampler,										///< [in] Type of sampler to use
	const unsigned int samples,								///< [in] Number of samples along the ray to take
	const char* transfer_red,								///< [in] Name of the transfer function for the red channel
	const char* transfer_green,								///< [in] Name of the transfer function for the green channel
	const char* transfer_blue,								///< [in] Name of the transfer function for the blue channel
	const char* transfer_alpha,								///< [in] Name of the transfer function for the alpha channel
	const char* iso_shader									///< [in] Shader to use for ISO surface rendering (optional)
	)
{
	IFunction1D* pRed = pFunc1DManager->GetItem( transfer_red );
	IFunction1D* pGreen = pFunc1DManager->GetItem( transfer_green );
	IFunction1D* pBlue = pFunc1DManager->GetItem( transfer_blue );
	IFunction1D* pAlpha = pFunc1DManager->GetItem( transfer_alpha );

	if( !pRed || !pGreen || !pBlue || !pAlpha ) {
		GlobalLog()->PrintEasyError( "Job::AddDirectVolumeRenderingShader:: Could not find out of the transfer functions" );
		return 0;
	}

	ISampling1D* pSampler = 0;

	switch( sampler ) {
		default:
		case 'u':
			RISE_API_CreateUniformSampling1D( &pSampler, 1.0 );
			break;
		case 'j':
			RISE_API_CreateJitteredSampling1D( &pSampler, 1.0 );
			break;
	}

	pSampler->SetNumSamples( samples );

	IShader* pISOShader = 0;
	if( iso_shader ) {
		pISOShader = pShaderManager->GetItem(iso_shader);
		if( !pISOShader ) {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddDirectVolumeRenderingShader:: Shader not found `%s`", iso_shader );
		}
	}


	IShader* pShader = 0;

	RISE_API_CreateDirectVolumeRenderingShader( &pShader, szVolumeFilePattern, width, height, startz, endz, accessor,
		gradient, composite, dThresholdStart, dThresholdEnd, *pSampler, *pRed, *pGreen, *pBlue, *pAlpha, pISOShader );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

bool Job::AddSpectralDirectVolumeRenderingShader(
	const char* name,										///< [in] Name of the shader
	const char* szVolumeFilePattern,						///< [in] File pattern for volume data
	const unsigned int width,								///< [in] Width of the volume
	const unsigned int height,								///< [in] Height of the volume
	const unsigned int startz,								///< [in] Starting z value for volume data
	const unsigned int endz,								///< [in] Ending z value for the volume data
	const char accessor,									///< [in] Type of volume accessor
	const char gradient,									///< [in] Type of gradient estimator to use
	const char composite,									///< [in] Type of composite operation to use
	const double dThresholdStart,							///< [in] Start of ISO threshold value (for ISO renderings only)
	const double dThresholdEnd,								///< [in] End of ISO threshold value (for ISO renderings only)
	const char sampler,										///< [in] Type of sampler to use
	const unsigned int samples,								///< [in] Number of samples along the ray to take
	const char* transfer_alpha,								///< [in] Name of the transfer function for the alpha channel
	const char* transfer_spectral,							///< [in] Name of the spectral transfer function
	const char* iso_shader									///< [in] Shader to use for ISO surface rendering (optional)
	)
{
	IFunction1D* pAlpha = pFunc1DManager->GetItem( transfer_alpha );

	if( !pAlpha ) {
		GlobalLog()->PrintEasyError( "Job::AddDirectVolumeRenderingShader:: Could not find alpha transfer functions" );
		return 0;
	}

	IFunction2D* pSpectral = pFunc2DManager->GetItem( transfer_spectral );

	if( !pSpectral ) {
		GlobalLog()->PrintEasyError( "Job::AddDirectVolumeRenderingShader:: Could not find spectral transfer functions" );
		return 0;
	}

	ISampling1D* pSampler = 0;

	switch( sampler ) {
		default:
		case 'u':
			RISE_API_CreateUniformSampling1D( &pSampler, 1.0 );
			break;
		case 'j':
			RISE_API_CreateJitteredSampling1D( &pSampler, 1.0 );
			break;
	}

	pSampler->SetNumSamples( samples );

	IShader* pISOShader = 0;
	if( iso_shader ) {
		pISOShader = pShaderManager->GetItem(iso_shader);
		if( !pISOShader ) {
			GlobalLog()->PrintEx( eLog_Warning, "Job::AddDirectVolumeRenderingShader:: Shader not found `%s`", iso_shader );
		}
	}


	IShader* pShader = 0;

	RISE_API_CreateSpectralDirectVolumeRenderingShader( &pShader, szVolumeFilePattern, width, height, startz, endz, accessor,
		gradient, composite, dThresholdStart, dThresholdEnd, *pSampler, *pAlpha, *pSpectral, pISOShader );

	pShaderManager->AddItem( pShader, name );
	safe_release( pShader );
	return true;
}

//
// Sets Rasterization parameters
//

bool GetSamplingAndFilterElements(
	ISampling2D** pPixelSampler,
	ISampling2D** pLumSampler,
	IPixelFilter** pPixelFilter,
	const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
	const unsigned int numLumSamples,						///< [in] Number of samples / lumin
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const PixelFilterConfig& pixelFilterConfig				///< [in] Pixel sampling + reconstruction filter
	)
{
	if( numPixelSamples > 1) {
		const String& sPixelSampler = pixelFilterConfig.pixelSampler;
		const double pixelSamplerParam = pixelFilterConfig.pixelSamplerParam;
		if( sPixelSampler == "nrooks" ) {
			RISE_API_CreateNRooksSampling2D( pPixelSampler, 1.0, 1.0, pixelSamplerParam );
		} else if( sPixelSampler == "uniform" ) {
			RISE_API_CreateUniformSampling2D( pPixelSampler, 1.0, 1.0 );
		} else if( sPixelSampler == "random" ) {
			RISE_API_CreateRandomSampling2D( pPixelSampler, 1.0, 1.0 );
		} else if( sPixelSampler == "stratified" ) {
			RISE_API_CreateStratifiedSampling2D( pPixelSampler, 1.0, 1.0, pixelSamplerParam );
		} else if( sPixelSampler == "poisson" ) {
			RISE_API_CreatePoissonDiskSampling2D( pPixelSampler, 1.0, 1.0, pixelSamplerParam );
		} else if( sPixelSampler == "multijittered" ) {
			RISE_API_CreateMultiJitteredSampling2D( pPixelSampler, 1.0, 1.0 );
		} else if( sPixelSampler == "halton" ) {
			RISE_API_CreateHaltonPointsSampling2D( pPixelSampler, 1.0, 1.0 );
		} else if( sPixelSampler == "sobol" ) {
			RISE_API_CreateSobolSampling2D( pPixelSampler, 1.0, 1.0 );
		} else {
			GlobalLog()->PrintEx( eLog_Error, "Unknown sampler type: `%s`", sPixelSampler.c_str() );
			return false;
		}
		(*pPixelSampler)->SetNumSamples( numPixelSamples );

		const String& sPixelFilter = pixelFilterConfig.filter;
		const double pixelFilterWidth = pixelFilterConfig.width;
		const double pixelFilterHeight = pixelFilterConfig.height;
		const double pixelFilterParamA = pixelFilterConfig.paramA;
		const double pixelFilterParamB = pixelFilterConfig.paramB;
		if( sPixelFilter == "none" ) {
			// Explicit opt-out: leave pPixelFilter null, caller
			// renders without sub-pixel reconstruction.
		} else if( sPixelFilter == "box" ) {
			RISE_API_CreateBoxPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "tent" ) {
			RISE_API_CreateTrianglePixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "gaussian" ) {
			RISE_API_CreateGaussianPixelFilter( pPixelFilter, pixelFilterParamA, pixelFilterParamB );
		} else if( sPixelFilter == "sinc" ) {
			RISE_API_CreateSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_box" ) {
			RISE_API_CreateBoxWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_bartlett" ) {
			RISE_API_CreateBartlettWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_welch" ) {
			RISE_API_CreateWelchWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_hanning" ) {
			RISE_API_CreateHanningWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_hamming" ) {
			RISE_API_CreateHammingWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_blackman" ) {
			RISE_API_CreateBlackmanWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_lanczos" ) {
			RISE_API_CreateLanczosWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "windowed_sinc_kaiser" ) {
			RISE_API_CreateKaiserWindowedSincPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA );
		} else if( sPixelFilter == "cook" ) {
			RISE_API_CreateCookPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight );
		} else if( sPixelFilter == "max" ) {
			RISE_API_CreateMaxPixelFilter( pPixelFilter, pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB );
		} else if( sPixelFilter == "mitchell-netravali" ) {
			RISE_API_CreateMitchellNetravaliPixelFilter( pPixelFilter, pixelFilterParamA, pixelFilterParamB );
		} else if( sPixelFilter == "lanczos" ) {
			RISE_API_CreateLanczosPixelFilter( pPixelFilter );
		} else if( sPixelFilter == "catmull-rom" ) {
			RISE_API_CreateCatmullRomPixelFilter( pPixelFilter );
		} else if( sPixelFilter == "cubic_bspline" ) {
			RISE_API_CreateCubicBSplinePixelFilter( pPixelFilter );
		} else if( sPixelFilter == "quadratic_bspline" ) {
			RISE_API_CreateQuadraticBSplinePixelFilter( pPixelFilter );
		} else {
			// Mirrors the pixel-sampler branch above — a typo in a
			// scene file must fail loudly rather than silently
			// rendering without a filter.  Before this fix an
			// unknown name produced an error log but left
			// pPixelFilter null and returned true, so the render
			// silently proceeded unfiltered.
			GlobalLog()->PrintEx( eLog_Error, "Unknown filter type: `%s`", sPixelFilter.c_str() );
			return false;
		}
	}

	if( numLumSamples > 1 ) {
		if( luminarySampler ) {
			String sLuminarySampler( luminarySampler );
			if( sLuminarySampler == "nrooks" ) {
				RISE_API_CreateNRooksSampling2D( pLumSampler, 1.0, 1.0, luminarySamplerParam );
			} else if( sLuminarySampler == "uniform" ) {
				RISE_API_CreateUniformSampling2D( pLumSampler, 1.0, 1.0 );
			} else if( sLuminarySampler == "random" ) {
				RISE_API_CreateRandomSampling2D( pLumSampler, 1.0, 1.0 );
			} else if( sLuminarySampler == "stratified" ) {
				RISE_API_CreateStratifiedSampling2D( pLumSampler, 1.0, 1.0, luminarySamplerParam );
			} else if( sLuminarySampler == "poisson" ) {
				RISE_API_CreatePoissonDiskSampling2D( pLumSampler, 1.0, 1.0, luminarySamplerParam );
			} else if( sLuminarySampler == "multijittered" ) {
				RISE_API_CreateMultiJitteredSampling2D( pLumSampler, 1.0, 1.0 );
			} else if( sLuminarySampler == "halton" ) {
				RISE_API_CreateHaltonPointsSampling2D( pLumSampler, 1.0, 1.0 );
			} else if( sLuminarySampler == "sobol" ) {
				RISE_API_CreateSobolSampling2D( pLumSampler, 1.0, 1.0 );
			} else {
				GlobalLog()->PrintEx( eLog_Error, "Unknown luminary sampler type: `%s`", luminarySampler );
				return false;
			}
		} else {
			RISE_API_CreateMultiJitteredSampling2D( pLumSampler, 1.0, 1.0 );
		}

		(*pLumSampler)->SetNumSamples( numLumSamples );
	}

	return true;
}

//! Sets the rasterizer type to be pixel based PEL
bool Job::SetPixelBasedPelRasterizer(
	const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
	const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
	const unsigned int maxRecur,							///< [in] Maximum recursion level
	const char* shader,										///< [in] The default shader
	const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
	const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
	const bool oidnDenoise,									///< [in] Should we denoise the output with OIDN?
	const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
	const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
	const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
	const PathGuidingConfig& guidingConfig,					///< [in] Path guiding configuration
	const AdaptiveSamplingConfig& adaptiveConfig,			///< [in] Adaptive sampling configuration
	const StabilityConfig& stabilityConfig,					///< [in] Production stability controls
	const ProgressiveConfig& progressiveConfig				///< [in] Progressive multi-pass rendering configuration
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, numLumSamples,
		luminarySampler, luminarySamplerParam, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetPixelBasedRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, maxRecur, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetPixelBasedPelRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( pLumSampler ) {
		pCaster->SetLuminaireSampling( pLumSampler );
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreatePixelBasedPelRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, pixelFilterConfig.blueNoiseSampler, _jobFs);

	// Always propagate the parsed progressiveConfig — including
	// `enabled=false`, otherwise `progressive_rendering FALSE` in a
	// scene file is silently ignored (the rasterizer's default
	// ProgressiveConfig::enabled is true).
	if( pRaster ) {
		RISE_API_SetRasterizerProgressiveRendering( pRaster, progressiveConfig.enabled, progressiveConfig.samplesPerPass );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.numLumSamples   = numLumSamples;
	snap.maxRecursion    = maxRecur;
	snap.shader          = shader ? shader : "";
	snap.luminarySampler = luminarySampler ? luminarySampler : "none";
	snap.luminarySamplerParam = luminarySamplerParam;
	snap.showLuminaires  = bShowLuminaires;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "pixelpel_rasterizer", pRaster, snap );

	return true;
}

//! Sets the rasterizer type to be pixel based spectral integrating
bool Job::SetPixelBasedSpectralIntegratingRasterizer(
	const unsigned int numPixelSamples,						///< [in] Number of samples / pixel
	const unsigned int numLumSamples,						///< [in] Number of samples / luminaire
	const SpectralConfig& spectralConfig,					///< [in] Spectral wavelength range, bins, and sampling strategy
	const unsigned int maxRecur,							///< [in] Maximum recursion level
	const char* shader,										///< [in] The default shader
	const RadianceMapConfig& radianceMapConfig,				///< [in] Global radiance map (IBL) configuration
	const char* luminarySampler,							///< [in] Type of sampling to use for luminaries
	const double luminarySamplerParam,						///< [in] Parameter for the luminary sampler
	const PixelFilterConfig& pixelFilterConfig,				///< [in] Pixel reconstruction filter
	const bool bShowLuminaires,								///< [in] Should we be able to see the luminaires?
	const bool bIntegrateRGB,								///< [in] Should we use the CIE XYZ spd functions or will they be specified now?
	const unsigned int numSPDvalues,						///< [in] Number of values in the RGB SPD arrays
	const double rgb_spd_frequencies[],						///< [in] Array that contains the RGB SPD frequencies
	const double rgb_spd_r[],								///< [in] Array that contains the RGB SPD amplitudes for red
	const double rgb_spd_g[],								///< [in] Array that contains the RGB SPD amplitudes for green
	const double rgb_spd_b[],								///< [in] Array that contains the RGB SPD amplitudes for blue
	const bool oidnDenoise,									///< [in] Should we denoise the output with OIDN?
	const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
	const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
	const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
	const StabilityConfig& stabilityConfig					///< [in] Production stability controls
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, numLumSamples,
		luminarySampler, luminarySamplerParam, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyInfo( "Job::SetPixelBasedRasterizer:: Specified shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, maxRecur, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetPixelBasedSpectralIntegratingRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( pLumSampler ) {
		pCaster->SetLuminaireSampling( pLumSampler );
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;

	if( bIntegrateRGB ) {

		GlobalLog()->PrintEasyError( "Job::SetPixelBasedSpectralIntegratingRasterizer:: Custom RGB curve integration is no longer supported" );

		/*
		if( rgb_spd_frequencies && rgb_spd_r && rgb_spd_g && rgb_spd_b ) {
			IPiecewiseFunction1D* red = 0;
			IPiecewiseFunction1D* green = 0;
			IPiecewiseFunction1D* blue = 0;

			RISE_API_CreatePiecewiseLinearFunction1D( &red );
			RISE_API_CreatePiecewiseLinearFunction1D( &green );
			RISE_API_CreatePiecewiseLinearFunction1D( &blue );

			red->addControlPoints( numSPDvalues, rgb_spd_frequencies, rgb_spd_r );
			green->addControlPoints( numSPDvalues, rgb_spd_frequencies, rgb_spd_g );
			blue->addControlPoints( numSPDvalues, rgb_spd_frequencies, rgb_spd_b );

			RISE_API_CreatePixelBasedSpectralIntegratingRasterizerRGB( &pRaster, pCaster, pPixelSampler, pPixelFilter, spectralConfig.spectralSamples, spectralConfig.nmBegin, spectralConfig.nmEnd, spectralConfig.numWavelengths, *red, *green, *blue );

			safe_release( red );
			safe_release( green );
			safe_release( blue );
		} else {
			GlobalLog()->PrintEasyWarning( "Job::SetPixelBasedSpectralIntegratingRasterizer:: Asked to integrate RGB but didn't properly specify SPD tables" );
		}
		*/
	} else {
		RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
		RISE_API_CreatePixelBasedSpectralIntegratingRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter, spectralConfig.spectralSamples, spectralConfig.nmBegin, spectralConfig.nmEnd, spectralConfig.numWavelengths, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, stabilityConfig, pixelFilterConfig.blueNoiseSampler, spectralConfig.useHWSS, _jobFs);
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.numLumSamples   = numLumSamples;
	snap.maxRecursion    = maxRecur;
	snap.shader          = shader ? shader : "";
	snap.luminarySampler = luminarySampler ? luminarySampler : "none";
	snap.luminarySamplerParam = luminarySamplerParam;
	snap.showLuminaires  = bShowLuminaires;
	snap.integrateRGB    = bIntegrateRGB;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.spectral        = spectralConfig;
	snap.stability       = stabilityConfig;
	RegisterAndActivateRasterizer( "pixelintegratingspectral_rasterizer", pRaster, snap );

	return true;
}

//! Sets the rasterizer type to be adaptive pixel based PEL
//! Sets the rasterizer type to be BDPT (bidirectional path tracing)
bool Job::SetBDPTPelRasterizer(
	const unsigned int numPixelSamples,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetBDPTPelRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetBDPTPelRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateBDPTPelRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter, maxEyeDepth, maxLightDepth,
		oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, pixelFilterConfig.blueNoiseSampler, _jobFs);

	// Always propagate the parsed progressiveConfig — including
	// `enabled=false`, otherwise `progressive_rendering FALSE` in a
	// scene file is silently ignored (the rasterizer's default
	// ProgressiveConfig::enabled is true).
	if( pRaster ) {
		RISE_API_SetRasterizerProgressiveRendering( pRaster, progressiveConfig.enabled, progressiveConfig.samplesPerPass );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.maxEyeDepth     = maxEyeDepth;
	snap.maxLightDepth   = maxLightDepth;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.pathGuiding     = guidingConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "bdpt_pel_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetBDPTSpectralRasterizer(
	const unsigned int numPixelSamples,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const SpectralConfig& spectralConfig,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetBDPTSpectralRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetBDPTSpectralRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateBDPTSpectralRasterizerAdaptive( &pRaster, pCaster, pPixelSampler, pPixelFilter, maxEyeDepth, maxLightDepth,
		spectralConfig.nmBegin, spectralConfig.nmEnd, spectralConfig.numWavelengths, spectralConfig.spectralSamples,
		oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, pixelFilterConfig.blueNoiseSampler, spectralConfig.useHWSS, _jobFs);

	// Always propagate the parsed progressiveConfig — including
	// `enabled=false`, otherwise `progressive_rendering FALSE` in a
	// scene file is silently ignored (the rasterizer's default
	// ProgressiveConfig::enabled is true).
	if( pRaster ) {
		RISE_API_SetRasterizerProgressiveRendering( pRaster, progressiveConfig.enabled, progressiveConfig.samplesPerPass );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.maxEyeDepth     = maxEyeDepth;
	snap.maxLightDepth   = maxLightDepth;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.spectral        = spectralConfig;
	snap.pathGuiding     = guidingConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "bdpt_spectral_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetVCMPelRasterizer(
	const unsigned int numPixelSamples,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const double mergeRadius,
	const bool enableVC,
	const bool enableVM,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetVCMPelRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetVCMPelRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateVCMPelRasterizer(
		&pRaster,
		pCaster,
		pPixelSampler,
		pPixelFilter,
		maxEyeDepth,
		maxLightDepth,
		mergeRadius,
		enableVC,
		enableVM,
		oidnDenoise,
		oidnQuality,
		oidnDevice,
		oidnPrefilter,
		guidingConfig,
		adaptiveConfig,
		stabilityConfig,
		pixelFilterConfig.blueNoiseSampler, _jobFs);

	// Always propagate the parsed progressiveConfig — including
	// `enabled=false`, otherwise `progressive_rendering FALSE` in a
	// scene file is silently ignored (the rasterizer's default
	// ProgressiveConfig::enabled is true).
	if( pRaster ) {
		RISE_API_SetRasterizerProgressiveRendering( pRaster, progressiveConfig.enabled, progressiveConfig.samplesPerPass );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.maxEyeDepth     = maxEyeDepth;
	snap.maxLightDepth   = maxLightDepth;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.mergeRadius     = mergeRadius;
	snap.enableVC        = enableVC;
	snap.enableVM        = enableVM;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.pathGuiding     = guidingConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "vcm_pel_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetVCMSpectralRasterizer(
	const unsigned int numPixelSamples,
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const SpectralConfig& spectralConfig,
	const double mergeRadius,
	const bool enableVC,
	const bool enableVM,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetVCMSpectralRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetVCMSpectralRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateVCMSpectralRasterizer(
		&pRaster,
		pCaster,
		pPixelSampler,
		pPixelFilter,
		maxEyeDepth,
		maxLightDepth,
		spectralConfig.nmBegin,
		spectralConfig.nmEnd,
		spectralConfig.numWavelengths,
		spectralConfig.spectralSamples,
		mergeRadius,
		enableVC,
		enableVM,
		oidnDenoise,
		oidnQuality,
		oidnDevice,
		oidnPrefilter,
		guidingConfig,
		adaptiveConfig,
		stabilityConfig,
		pixelFilterConfig.blueNoiseSampler,
		spectralConfig.useHWSS, _jobFs);

	// Always propagate the parsed progressiveConfig — including
	// `enabled=false`, otherwise `progressive_rendering FALSE` in a
	// scene file is silently ignored (the rasterizer's default
	// ProgressiveConfig::enabled is true).
	if( pRaster ) {
		RISE_API_SetRasterizerProgressiveRendering( pRaster, progressiveConfig.enabled, progressiveConfig.samplesPerPass );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.maxEyeDepth     = maxEyeDepth;
	snap.maxLightDepth   = maxLightDepth;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.mergeRadius     = mergeRadius;
	snap.enableVC        = enableVC;
	snap.enableVM        = enableVM;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.spectral        = spectralConfig;
	snap.pathGuiding     = guidingConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "vcm_spectral_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetAutoRasterizer(
	const AutoIntegratorChoice integrator,
	const unsigned int numPixelSamples,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig,
	const bool probeEnabled
	)
{
	// The shared setup (sampler/filter, shader, caster, radiance map, RR
	// threshold, light BVH) is integrator-agnostic and IDENTICAL to the
	// PT/BDPT/VCM setters — the dispatcher delegates to a rasterizer built
	// from exactly these inputs, so pinning `auto` to `pt` produces the
	// same image as a bare pathtracing_pel_rasterizer.
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetAutoRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetAutoRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateAutoRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter, integrator,
		oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig,
		pixelFilterConfig.blueNoiseSampler, progressiveConfig, probeEnabled, _jobFs );

	// NOTE: the wrapper applies progressiveConfig to its delegate itself
	// (RISE_API_SetRasterizerProgressiveRendering down-casts to
	// PixelBasedRasterizerHelper, which the wrapper is not) — so we do NOT
	// call it on pRaster here the way the concrete setters do.

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.autoIntegrator  = integrator;
	snap.autoProbeEnabled = probeEnabled;
	snap.numPixelSamples = numPixelSamples;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.pathGuiding     = guidingConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "auto_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetAutoSpectralRasterizer(
	const AutoIntegratorChoice integrator,
	const unsigned int numPixelSamples,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const SpectralConfig& spectralConfig,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig,
	const bool probeEnabled
	)
{
	// Mirrors Job::SetAutoRasterizer; the ONLY differences are the SPECTRAL
	// delegate factory (RISE_API_CreateAutoSpectralRasterizer) and the
	// spectral-core param bundle in place of path-guiding.  The shared setup
	// (sampler/filter, shader, caster, radiance map, RR threshold, light BVH)
	// is integrator-agnostic and identical to the *_spectral_ setters, so
	// pinning `auto` to `pt` produces the same image as pathtracing_spectral.
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetAutoSpectralRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetAutoSpectralRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateAutoSpectralRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter, integrator,
		spectralConfig.nmBegin, spectralConfig.nmEnd, spectralConfig.numWavelengths, spectralConfig.spectralSamples, spectralConfig.useHWSS,
		oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, adaptiveConfig, stabilityConfig,
		pixelFilterConfig.blueNoiseSampler, progressiveConfig, probeEnabled, _jobFs );

	// The wrapper applies progressiveConfig to its delegate itself (see
	// SetAutoRasterizer) — so do NOT call RISE_API_SetRasterizerProgressiveRendering here.

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.autoIntegrator  = integrator;
	snap.autoProbeEnabled = probeEnabled;
	snap.numPixelSamples = numPixelSamples;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.spectral        = spectralConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "auto_spectral_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetPathTracingPelRasterizer(
	const unsigned int numPixelSamples,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const SMSConfig& smsConfig,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PathGuidingConfig& guidingConfig,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetPathTracingPelRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetPathTracingPelRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	// Transparent (Fresnel-attenuated) shadow rays — unidirectional PT
	// opt-in.  Routed through the concrete RayCaster (LightSampler
	// dynamic_casts to it); off by default.  BDPT/VCM/MLT do NOT wire
	// this — their NEE stays binary.
	{
		RISE::Implementation::RayCaster* pConcreteCaster =
			dynamic_cast<RISE::Implementation::RayCaster*>( pCaster );
		if( pConcreteCaster ) {
			pConcreteCaster->SetTransparentShadows( stabilityConfig.transparentShadows );
		}
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreatePathTracingPelRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter,
		smsConfig.enabled, smsConfig.maxIterations, smsConfig.threshold, smsConfig.maxChainDepth, smsConfig.biased, smsConfig.bernoulliTrials, smsConfig.multiTrials, smsConfig.photonCount, smsConfig.twoStage, smsConfig.useLevenbergMarquardt, smsConfig.seedingMode, smsConfig.targetBounces, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, guidingConfig, adaptiveConfig, stabilityConfig, pixelFilterConfig.blueNoiseSampler, _jobFs);

	// Always propagate the parsed progressiveConfig — including
	// `enabled=false`, otherwise `progressive_rendering FALSE` in a
	// scene file is silently ignored (the rasterizer's default
	// ProgressiveConfig::enabled is true).
	if( pRaster ) {
		RISE_API_SetRasterizerProgressiveRendering( pRaster, progressiveConfig.enabled, progressiveConfig.samplesPerPass );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.sms             = smsConfig;
	snap.pathGuiding     = guidingConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "pathtracing_pel_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetPathTracingSpectralRasterizer(
	const unsigned int numPixelSamples,
	const char* shader,
	const RadianceMapConfig& radianceMapConfig,
	const PixelFilterConfig& pixelFilterConfig,
	const bool bShowLuminaires,
	const SpectralConfig& spectralConfig,
	const SMSConfig& smsConfig,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const AdaptiveSamplingConfig& adaptiveConfig,
	const StabilityConfig& stabilityConfig,
	const ProgressiveConfig& progressiveConfig
	)
{
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;

	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, numPixelSamples, 1,
		0, 0, pixelFilterConfig ) )
	{
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetPathTracingSpectralRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, radianceMapConfig.isBackground, 10, *pShader, bShowLuminaires );

	if( !( radianceMapConfig.name == "none" ) ) {
		IPainter* p = pPntManager->GetItem( radianceMapConfig.name.c_str() );

		if( p ) {
			IRadianceMap* pRm = 0;
			RISE_API_CreateRadianceMap( &pRm, *p, radianceMapConfig.scale );
			pRm->SetOrientation( Vector3( radianceMapConfig.orientation ) );

			pScene->SetGlobalRadianceMap( pRm );
			safe_release( pRm );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Job::SetPathTracingSpectralRasterizer:: Global Radiance Map painter not found \'%s\'", p );
		}
	}

	if( lightSampleRRThreshold > 0 ) {
		pCaster->SetLightSampleRRThreshold( lightSampleRRThreshold );
	}

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	// Transparent (Fresnel-attenuated) shadow rays — unidirectional PT
	// opt-in (spectral path).  See the pel PT factory for rationale.
	{
		RISE::Implementation::RayCaster* pConcreteCaster =
			dynamic_cast<RISE::Implementation::RayCaster*>( pCaster );
		if( pConcreteCaster ) {
			pConcreteCaster->SetTransparentShadows( stabilityConfig.transparentShadows );
		}
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreatePathTracingSpectralRasterizer( &pRaster, pCaster, pPixelSampler, pPixelFilter,
		spectralConfig.nmBegin, spectralConfig.nmEnd, spectralConfig.numWavelengths, spectralConfig.spectralSamples,
		smsConfig.enabled, smsConfig.maxIterations, smsConfig.threshold, smsConfig.maxChainDepth, smsConfig.biased, smsConfig.bernoulliTrials, smsConfig.multiTrials, smsConfig.photonCount, smsConfig.twoStage, smsConfig.useLevenbergMarquardt, smsConfig.seedingMode, smsConfig.targetBounces, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter, adaptiveConfig, stabilityConfig, pixelFilterConfig.blueNoiseSampler, spectralConfig.useHWSS, _jobFs);

	// Always propagate the parsed progressiveConfig — including
	// `enabled=false`, otherwise `progressive_rendering FALSE` in a
	// scene file is silently ignored (the rasterizer's default
	// ProgressiveConfig::enabled is true).
	if( pRaster ) {
		RISE_API_SetRasterizerProgressiveRendering( pRaster, progressiveConfig.enabled, progressiveConfig.samplesPerPass );
	}

	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );
	safe_release( pCaster );

	RasterizerParams snap;
	snap.numPixelSamples = numPixelSamples;
	snap.shader          = shader ? shader : "";
	snap.showLuminaires  = bShowLuminaires;
	snap.oidnDenoise     = oidnDenoise;
	snap.oidnQuality     = oidnQuality;
	snap.oidnDevice      = oidnDevice;
	snap.oidnPrefilter   = oidnPrefilter;
	snap.radianceMap     = radianceMapConfig;
	snap.pixelFilter     = pixelFilterConfig;
	snap.sms             = smsConfig;
	snap.spectral        = spectralConfig;
	snap.adaptive        = adaptiveConfig;
	snap.stability       = stabilityConfig;
	snap.progressive     = progressiveConfig;
	RegisterAndActivateRasterizer( "pathtracing_spectral_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetMLTRasterizer(
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const unsigned int nBootstrap,
	const unsigned int nChains,
	const unsigned int nMutationsPerPixel,
	const double largeStepProb,
	const char* shader,
	const bool bShowLuminaires,
	const bool oidnDenoise,									///< [in] Should we denoise the output with OIDN?
	const OidnQuality oidnQuality,							///< [in] OIDN quality preset (Auto = render-time heuristic)
	const OidnDevice oidnDevice,							///< [in] OIDN device backend (Auto = prefer GPU, fall back to CPU)
	const OidnPrefilter oidnPrefilter,						///< [in] OIDN aux source mode (Fast = retrace/first-hit, Accurate = inline first-non-delta + prefilter)
	const PixelFilterConfig& pixelFilterConfig,
	const StabilityConfig& stabilityConfig					///< [in] Production stability controls
	)
{
	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetMLTRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, false, 10, *pShader, bShowLuminaires );

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	// Build the pixel filter.  MLT does not use a conventional pixel
	// sampler (it generates its own film samples via the Markov chain)
	// but it still wants a reconstruction filter — without one, splats
	// land at integer pixels and the image shows aliasing / hard edges.
	// We pass a dummy pixel-sampler count of 2 so GetSamplingAndFilter-
	// Elements actually builds the filter object; the sampler it also
	// creates is stored but never read by the MLT render loop.
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;
	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, 2, 1,
		0, 0.0, pixelFilterConfig ) )
	{
		safe_release( pCaster );
		return false;
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateMLTRasterizerWithFilter( &pRaster, pCaster, maxEyeDepth, maxLightDepth,
		nBootstrap, nChains, nMutationsPerPixel, largeStepProb, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
		pPixelSampler, pPixelFilter, _jobFs);

	safe_release( pCaster );
	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );

	RasterizerParams snap;
	snap.maxEyeDepth        = maxEyeDepth;
	snap.maxLightDepth      = maxLightDepth;
	snap.nBootstrap         = nBootstrap;
	snap.nChains            = nChains;
	snap.nMutationsPerPixel = nMutationsPerPixel;
	snap.largeStepProb      = largeStepProb;
	snap.shader             = shader ? shader : "";
	snap.showLuminaires     = bShowLuminaires;
	snap.oidnDenoise        = oidnDenoise;
	snap.oidnQuality        = oidnQuality;
	snap.oidnDevice         = oidnDevice;
	snap.oidnPrefilter      = oidnPrefilter;
	snap.pixelFilter        = pixelFilterConfig;
	snap.stability          = stabilityConfig;
	RegisterAndActivateRasterizer( "mlt_rasterizer", pRaster, snap );

	return true;
}

bool Job::SetMLTSpectralRasterizer(
	const unsigned int maxEyeDepth,
	const unsigned int maxLightDepth,
	const unsigned int nBootstrap,
	const unsigned int nChains,
	const unsigned int nMutationsPerPixel,
	const double largeStepProb,
	const char* shader,
	const bool bShowLuminaires,
	const SpectralConfig& spectralConfig,
	const bool oidnDenoise,
	const OidnQuality oidnQuality,
	const OidnDevice oidnDevice,
	const OidnPrefilter oidnPrefilter,
	const PixelFilterConfig& pixelFilterConfig,
	const StabilityConfig& stabilityConfig
	)
{
	IShader* pShader = pShaderManager->GetItem( shader );
	if( !pShader ) {
		GlobalLog()->PrintEasyError( "Job::SetMLTSpectralRasterizer:: Default shader not found" );
		return false;
	}

	IRayCaster* pCaster = 0;
	RISE_API_CreateRayCaster( &pCaster, false, 10, *pShader, bShowLuminaires );

	if( stabilityConfig.useLightBVH ) {
		pCaster->SetUseLightBVH( true );
	}

	// See SetMLTRasterizer above for why we request a filter here.
	ISampling2D* pPixelSampler = 0;
	ISampling2D* pLumSampler = 0;
	IPixelFilter* pPixelFilter = 0;
	if( !GetSamplingAndFilterElements( &pPixelSampler, &pLumSampler, &pPixelFilter, 2, 1,
		0, 0.0, pixelFilterConfig ) )
	{
		safe_release( pCaster );
		return false;
	}

	IRasterizer* pRaster = 0;
	RISE::Implementation::FrameStore* _jobFs = ResolveJobFrameStoreForActiveCamera();  // L6b
	RISE_API_CreateMLTSpectralRasterizerWithFilter( &pRaster, pCaster, maxEyeDepth, maxLightDepth,
		nBootstrap, nChains, nMutationsPerPixel, largeStepProb,
		spectralConfig.nmBegin, spectralConfig.nmEnd, spectralConfig.spectralSamples, spectralConfig.useHWSS, oidnDenoise, oidnQuality, oidnDevice, oidnPrefilter,
		pPixelSampler, pPixelFilter, _jobFs);

	safe_release( pCaster );
	safe_release( pPixelSampler );
	safe_release( pLumSampler );
	safe_release( pPixelFilter );

	RasterizerParams snap;
	snap.maxEyeDepth        = maxEyeDepth;
	snap.maxLightDepth      = maxLightDepth;
	snap.nBootstrap         = nBootstrap;
	snap.nChains            = nChains;
	snap.nMutationsPerPixel = nMutationsPerPixel;
	snap.largeStepProb      = largeStepProb;
	snap.shader             = shader ? shader : "";
	snap.showLuminaires     = bShowLuminaires;
	snap.oidnDenoise        = oidnDenoise;
	snap.oidnQuality        = oidnQuality;
	snap.oidnDevice         = oidnDevice;
	snap.oidnPrefilter      = oidnPrefilter;
	snap.spectral           = spectralConfig;
	snap.pixelFilter        = pixelFilterConfig;
	snap.stability          = stabilityConfig;
	RegisterAndActivateRasterizer( "mlt_spectral_rasterizer", pRaster, snap );

	return true;
}

//
// Adds raster outputs
//

//! Creates a file rasterizer output
//! This should be called after a rasterizer has been set
//! Note that setting a new rasterizer after adding file rasterizer outputs will
//! delete existing outputs
/// \return TRUE if successful, FALSE otherwise
bool Job::AddFileRasterizerOutput(
	const char* szPattern,									///< [in] File pattern
	const bool bMultiple,									///< [in] Output multiple files (for animations usually)
	const char type,										///< [in] Type of file
															///		0 - TGA
															///		1 - PPM
															///		2 - PNG
															///		3 - HDR
															///     4 - TIFF
															///		5 - RGBEA
															///		6 - EXR
	const unsigned char bpp,								///< [in] Bits / pixel for the file
	const char color_space,									///< [in] Color space to apply
															///		0 - Rec709 RGB linear
															///		1 - sRGB profile
															///		2 - ROMM RGB (ProPhotoRGB) linear
															///		3 - ROMM RGB (ProPhotoRGB) non-linear
	const double exposureEV,
	const char display_transform,
	const char exr_compression,
	const bool exr_with_alpha
	)
{
	if( !pRasterizer ) {
		return false;
	}

	// L5d — GUI hosts set m_suppressFileRasterizerOutputs at
	// construction so loading a scene with `file_rasterizeroutput`
	// chunks doesn't litter the user's filesystem with auto-generated
	// PNGs / EXRs every time they hit Render in the UI.  We log the
	// dropped pattern + return true so the parser doesn't fail (the
	// scene is otherwise valid; the only side effect omitted is the
	// auto-write itself).  CLI keeps the default false → byte-
	// identical legacy behaviour.
	if( m_suppressFileRasterizerOutputs ) {
		GlobalLog()->PrintEx( eLog_Info,
			"Job:: dropping file_rasterizeroutput \"%s\" "
			"(GUI mode — interactive renders no longer write "
			"to scene-author-specified paths; use File > Save "
			"Rendered Image to write to a user-chosen path).",
			szPattern ? szPattern : "(null)" );
		return true;
	}

	COLOR_SPACE gc = eColorSpace_sRGB;
	switch( color_space )
	{
	case 1:
		gc = eColorSpace_sRGB;
		break;
	case 0:
		gc = eColorSpace_Rec709RGB_Linear;
		break;
	case 2:
		gc = eColorSpace_ROMMRGB_Linear;
		break;
	case 3:
		gc = eColorSpace_ProPhotoRGB;
		break;
	};

	// Map display_transform char code to enum.  Out-of-range falls
	// back to ACES (the documented default) with a warning so the
	// user notices a typo.
	DISPLAY_TRANSFORM dt = eDisplayTransform_ACES;
	switch( display_transform )
	{
	case 0: dt = eDisplayTransform_None;     break;
	case 1: dt = eDisplayTransform_Reinhard; break;
	case 2: dt = eDisplayTransform_ACES;     break;
	case 3: dt = eDisplayTransform_AgX;      break;
	case 4: dt = eDisplayTransform_Hable;    break;
	default:
		GlobalLog()->PrintEx( eLog_Warning,
			"Job::AddFileRasterizerOutput:: unknown display_transform=%d, defaulting to ACES",
			(int)display_transform );
		dt = eDisplayTransform_ACES;
		break;
	}

	// Map exr_compression char code to enum.  Out-of-range falls back to PIZ.
	EXR_COMPRESSION exrc = eExrCompression_Piz;
	switch( exr_compression )
	{
	case 0: exrc = eExrCompression_None; break;
	case 1: exrc = eExrCompression_Zip;  break;
	case 2: exrc = eExrCompression_Piz;  break;
	case 3: exrc = eExrCompression_Dwaa; break;
	default:
		GlobalLog()->PrintEx( eLog_Warning,
			"Job::AddFileRasterizerOutput:: unknown exr_compression=%d, defaulting to PIZ",
			(int)exr_compression );
		exrc = eExrCompression_Piz;
		break;
	}

	IRasterizerOutput* ro = 0;
	RISE_API_CreateFileRasterizerOutput(
		&ro, szPattern, bMultiple, type, bpp, gc,
		(Scalar)exposureEV, dt, exrc, exr_with_alpha );

	pRasterizer->AddRasterizerOutput( ro );
	safe_release( ro );

	return true;
}

namespace RISE {
	namespace Implementation {
		//! This is out dispatcher that handles the rasterizer output calls and pipes them off
		//! to the client application
		class CallbackRasterizerOutputDispatch : public virtual IRasterizerOutput, public virtual Reference
		{
		protected:
			IJobRasterizerOutput&	pObj;
			RGBA16*					pBuffer;
			unsigned int			width;
			unsigned int			height;
			bool					bPremultipliedAlpha;
			COLOR_SPACE				color_space;

			virtual ~CallbackRasterizerOutputDispatch()
			{
				if( pBuffer ) {
					GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
					// Array allocation (`new RGBA16[...]`) requires
					// matching `delete[]`; the historical `delete pBuffer`
					// was undefined behaviour that happened to work on
					// trivial element types under most allocators.
					delete[] pBuffer;
					pBuffer = 0;
				}
			}

		public:

			CallbackRasterizerOutputDispatch( const CallbackRasterizerOutputDispatch& o ) :
			  pObj( o.pObj ), pBuffer( 0 ), width( o.width ), height( o.height ), bPremultipliedAlpha( o.bPremultipliedAlpha ), color_space( o.color_space )
			{
			}

			CallbackRasterizerOutputDispatch( IJobRasterizerOutput& pObj_ ) :
			  pObj( pObj_ ), pBuffer( 0 ), width( 0 ), height( 0 ), bPremultipliedAlpha( false ), color_space( eColorSpace_sRGB )
			{
			}

			//! Outputs an intermediate scanline of rasterized data
			void OutputIntermediateImage(
				const IRasterImage& pImage,					///< [in] Rasterized image
				const Rect* pRegion							///< [in] Rasterized region, if its NULL then the entire image should be output
				)
			{
				// Reallocate the dispatch buffer whenever the image
				// dimensions change, not only on first call.  Without
				// this, an output dispatcher reused across renders with
				// different image dims would either:
				//   - underflow pBuffer (if cached dims < current dims):
				//     the y*width+x indexing uses cached width and the
				//     loop iterates current dims, so writes overrun
				//     pBuffer.
				//   - read-OOB on GetPEL (if cached dims > current dims):
				//     the loop iterates cached dims, but pImage is only
				//     `current dims` big — GetPEL(x>=current_w, y>=current_h)
				//     reads garbage, GetPEL's virtual call may dispatch
				//     through a corrupted vtable on freed pages, and the
				//     downstream Integerize crashes with a PAC fault on
				//     a wild far-address (matching the production-render
				//     crash report).
				// The cheap path (dims match) is unchanged: no realloc,
				// no work.
				const unsigned int curW = pImage.GetWidth();
				const unsigned int curH = pImage.GetHeight();
				if( !pBuffer || curW != width || curH != height ) {
					if( pBuffer ) {
						GlobalLog()->PrintDelete( pBuffer, __FILE__, __LINE__ );
						delete[] pBuffer;
						pBuffer = 0;
					}
					width = curW;
					height = curH;
					pBuffer = new RGBA16[ width*height ];
					GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );
					bPremultipliedAlpha = pObj.PremultipliedAlpha();
					int cp = pObj.GetColorSpace();
					switch( cp )
					{
					case 0:
						color_space = eColorSpace_Rec709RGB_Linear;
						break;
					case 1:
					default:
						color_space = eColorSpace_sRGB;
						break;
					case 2:
						color_space = eColorSpace_ROMMRGB_Linear;
						break;
					case 3:
						color_space = eColorSpace_ProPhotoRGB;
						break;
					};
				}

				// Set the appropriate bytes
				Rect rc( 0, 0, height-1, width-1 );
				if( pRegion ) {
					rc.left = pRegion->left;
					rc.top = pRegion->top;
					rc.right = pRegion->right;
					rc.bottom = pRegion->bottom;
				}

				// HDR-aware fast path: outputs that opt into 32-bit
				// float receive the raw linear `RISEColor.base` values
				// directly, skipping the Integerize quantize-to-uint16
				// step (which would clamp any value > 1.0).  Required
				// for the Blender bridge so Filmic + exposure can
				// tonemap real HDR data instead of seeing a [0, 1]
				// clipped image.  Falls through to the legacy 16-bit
				// path for every existing consumer that doesn't
				// override `WantsFloat32`.
				if( pObj.WantsFloat32() ) {
					std::vector<float> fbuf( static_cast<size_t>( width ) * height * 4, 0.0f );
					for( unsigned int y=rc.top; y<=rc.bottom; y++ ) {
						for( unsigned int x=rc.left; x<= rc.right; x++ ) {
							const RISEColor c = pImage.GetPEL( x, y );
							const size_t idx = ( static_cast<size_t>( y ) * width + x ) * 4;
							// RISEPel == Rec709RGBPel since Stage B of
							// the colour-space migration.  Members are
							// `.base.r/g/b` (Chel = double), `.a`.
							// We deliver them verbatim — the consumer
							// (Blender bridge) declares colour space
							// via GetColorSpace and applies any further
							// transform downstream.
							fbuf[idx+0] = static_cast<float>( c.base.r );
							fbuf[idx+1] = static_cast<float>( c.base.g );
							fbuf[idx+2] = static_cast<float>( c.base.b );
							fbuf[idx+3] = static_cast<float>( c.a );
							if( bPremultipliedAlpha ) {
								const float aa = fbuf[idx+3];
								fbuf[idx+0] *= aa;
								fbuf[idx+1] *= aa;
								fbuf[idx+2] *= aa;
							}
						}
					}
					pObj.OutputImageRGBA32F( fbuf.data(), width, height, rc.top, rc.left, rc.bottom, rc.right );
					return;
				}

				for( unsigned int y=rc.top; y<=rc.bottom; y++ ) {
					for( unsigned int x=rc.left; x<= rc.right; x++ ) {

						RGBA16 conv;

						switch( color_space )
						{
						case eColorSpace_sRGB:
							conv = pImage.GetPEL(x,y).Integerize<sRGBPel,unsigned short>( 65535.0 );
							break;
						case eColorSpace_Rec709RGB_Linear:
							conv = pImage.GetPEL(x,y).Integerize<Rec709RGBPel,unsigned short>( 65535.0 );
							break;
						case eColorSpace_ROMMRGB_Linear:
							conv = pImage.GetPEL(x,y).Integerize<ROMMRGBPel,unsigned short>( 65535.0 );
							break;
						case eColorSpace_ProPhotoRGB:
							conv = pImage.GetPEL(x,y).Integerize<ProPhotoRGBPel,unsigned short>( 65535.0 );
							break;
						};

						if( bPremultipliedAlpha ) {
							pBuffer[y*width+x] = ColorUtils::PremultiplyAlphaRGB<RGBA16,0xFFFF>( conv );
						} else {
							pBuffer[y*width+x] = conv;
						}
					}
				}

				// Send the data off
				pObj.OutputImageRGBA16(	(unsigned short*)pBuffer, width, height, rc.top, rc.left, rc.bottom, rc.right );
			}

			//! A full rasterization was complete, and the full image should be output
			void OutputImage(
				const IRasterImage& pImage,					///< [in] Rasterized image
				const Rect* pRegion,						///< [in] Rasterized region, if its NULL then the entire image should be output
				const unsigned int frame
				)
			{
				OutputIntermediateImage( pImage, pRegion );
			}
		};
	}
}

//! Creates a user callback rasterizer output
//! This should be called after a rasterizer has been set
//! Note that no attemps at reference counting are made, the user
//! better not go delete the object
bool Job::AddCallbackRasterizerOutput(
	IJobRasterizerOutput* pObj
	)
{
	if( !pObj || !pRasterizer ) {
		return false;
	}

	IRasterizerOutput* pRo = new CallbackRasterizerOutputDispatch( *pObj );
	GlobalLog()->PrintNew( pRo, __FILE__, __LINE__, "callback rasterizer output dispatch" );

	pRasterizer->AddRasterizerOutput( pRo );
	safe_release( pRo );

	return true;
}

//
// Photon mapping
//

//! Sets the gather parameters for the caustic pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetCausticPelGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	// Always record on scene: applied to the map after BuildPendingPhotonMaps
	// shoots.  If the map already exists (e.g. loaded from disk), apply now too.
	pScene->QueueCausticPelGatherParams( radius, ellipse_ratio, min, max );
	IPhotonMap* pMap = pScene->GetCausticPelMapMutable();
	if( pMap ) {
		pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	}
	return true;
}

//! Sets the gather parameters for the global pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetGlobalPelGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	pScene->QueueGlobalPelGatherParams( radius, ellipse_ratio, min, max );
	IPhotonMap* pMap = pScene->GetGlobalPelMapMutable();
	if( pMap ) {
		pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	}
	return true;
}

//! Sets the gather parameters for the translucent pel photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetTranslucentPelGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	pScene->QueueTranslucentPelGatherParams( radius, ellipse_ratio, min, max );
	IPhotonMap* pMap = pScene->GetTranslucentPelMapMutable();
	if( pMap ) {
		pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	}
	return true;
}

//! Sets the gather parameters for the caustic spectral photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetCausticSpectralGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max,							///< [in] Total number of photons to shoot
	const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
	)
{
	pScene->QueueCausticSpectralGatherParams( radius, ellipse_ratio, min, max, nm_range );
	ISpectralPhotonMap* pMap = pScene->GetCausticSpectralMapMutable();
	if( pMap ) {
		pMap->SetGatherParamsNM( radius, ellipse_ratio, min, max, nm_range, pGlobalProgress );
	}
	return true;
}

//! Sets the gather parameters for the global spectral photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetGlobalSpectralGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max,							///< [in] Total number of photons to shoot
	const double nm_range							///< [in] Range of wavelengths to search for a NM irradiance estimate
	)
{
	pScene->QueueGlobalSpectralGatherParams( radius, ellipse_ratio, min, max, nm_range );
	ISpectralPhotonMap* pMap = pScene->GetGlobalSpectralMapMutable();
	if( pMap ) {
		pMap->SetGatherParamsNM( radius, ellipse_ratio, min, max, nm_range, pGlobalProgress );
	}
	return true;
}

//! Sets the gather parameters for the shadow photon map
/// \return TRUE if successful, FALSE otherwise
bool Job::SetShadowGatherParameters(
	const double radius,							///< [in] Search radius
	const double ellipse_ratio,						///< [in] Ratio of the gather ellipse (percentage of gather radius)
	const unsigned int min,							///< [in] Maximum number of photons to store
	const unsigned int max							///< [in] Total number of photons to shoot
	)
{
	pScene->QueueShadowGatherParams( radius, ellipse_ratio, min, max );
	IShadowPhotonMap* pMap = pScene->GetShadowMapMutable();
	if( pMap ) {
		pMap->SetGatherParams( radius, ellipse_ratio, min, max, pGlobalProgress );
	}
	return true;
}

//! Sets the irradiance cache parameters
/// \return TRUE if successful, FALSE otherwise
bool Job::SetIrradianceCacheParameters(
	const unsigned int size,			///< [in] Size of the cache
	const double tolerance,				///< [in] Tolerance of the cache
	const double min_spacing,			///< [in] Minimum seperation
	const double max_spacing,			///< [in] Maximum seperation
	const double query_threshold_scale,	///< [in] Scale for the query acceptance threshold
	const double neighbor_spacing_scale	///< [in] Scale for capping reuse radius by local neighbor spacing
	)
{
	IIrradianceCache* pCache = 0;
	RISE_API_CreateIrradianceCache( &pCache, size, tolerance, min_spacing, max_spacing, query_threshold_scale, neighbor_spacing_scale );

    pScene->SetIrradianceCache( pCache );

	safe_release( pCache );
	return true;
}

//! Saves the caustic pel photon map to disk
bool Job::SaveCausticPelPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetCausticPelMapMutable();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a caustic pel photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the global pel photon map to disk
bool Job::SaveGlobalPelPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetGlobalPelMapMutable();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a global pel photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the translucent pel photon map to disk
bool Job::SaveTranslucentPelPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IPhotonMap* pMap = pScene->GetTranslucentPelMapMutable();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a translucent pel photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the caustic spectral photon map to disk
bool Job::SaveCausticSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	const ISpectralPhotonMap* pMap = pScene->GetCausticSpectralMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a caustic spectral photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Saves the global spectral photon map to disk
bool Job::SaveGlobalSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	const ISpectralPhotonMap* pMap = pScene->GetGlobalSpectralMap();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a global spectral photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}

//! Loads the shadow photon map to disk
bool Job::SaveShadowPhotonmap(
	const char* file_name							///< [in] Name of the file to save it to
	)
{
	if( !file_name ) {
		return false;
	}

	IWriteBuffer* buffer = 0;
	RISE_API_CreateDiskFileWriteBuffer( &buffer, file_name );

	IShadowPhotonMap* pMap = pScene->GetShadowMapMutable();

	if( !pMap ) {
		GlobalLog()->PrintEasyError( "Scene doesn't contain a shadow photon map" );
		return false;
	}

	pMap->Serialize( *buffer );

	safe_release( buffer );
	return true;
}


//! Loads the caustic pel photon map from disk
bool Job::LoadCausticPelPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IPhotonMap* pmap = 0;
	RISE_API_CreateCausticPelPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetCausticPelMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the global pel photon map from disk
bool Job::LoadGlobalPelPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IPhotonMap* pmap = 0;
	RISE_API_CreateGlobalPelPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetGlobalPelMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the translucent pel photon map from disk
bool Job::LoadTranslucentPelPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IPhotonMap* pmap = 0;
	RISE_API_CreateTranslucentPelPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetTranslucentPelMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the caustic spectral photon map from disk
bool Job::LoadCausticSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}


	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	ISpectralPhotonMap* pmap = 0;
	RISE_API_CreateCausticSpectralPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetCausticSpectralMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the caustic spectral photon map from disk
bool Job::LoadGlobalSpectralPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}


	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	ISpectralPhotonMap* pmap = 0;
	RISE_API_CreateGlobalSpectralPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetGlobalSpectralMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}

//! Loads the shadow photon map from disk
bool Job::LoadShadowPhotonmap(
	const char* file_name							///< [in] Name of the file to load from
	)
{
	if( !file_name ) {
		return false;
	}

	IReadBuffer* buffer = 0;
	RISE_API_CreateDiskFileReadBuffer( &buffer, file_name );

	IShadowPhotonMap* pmap = 0;
	RISE_API_CreateShadowPhotonMap( &pmap, 0 );

	pmap->Deserialize( *buffer );
	pScene->SetShadowMap( pmap );

	safe_release( pmap );
	safe_release( buffer );
	return true;
}


//
// Commands
//

//! Queues a caustic pel photon-map shoot.  Actual tracing is deferred to the
//! start of RasterizeScene, where Scene::BuildPendingPhotonMaps executes it.
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootCausticPelPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const bool reflect,								///< [in] Should we trace reflected rays?
	const bool refract,								///< [in] Should we trace refracted rays?
	const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	const bool shootFromMeshLights					///< [in] Should we shoot from mesh based lights (luminaries)?
	)
{
	PendingCausticPelShoot req = {};
	req.num = num;
	req.powerScale = power_scale;
	req.maxRecur = maxRecur;
	req.minImportance = minImportance;
	req.branch = branch;
	req.reflect = reflect;
	req.refract = refract;
	req.shootFromNonMeshLights = shootFromNonMeshLights;
	req.temporalSamples = temporal_samples;
	req.regenerate = regenerate;
	req.shootFromMeshLights = shootFromMeshLights;
	pScene->QueueCausticPelPhotonShoot( req );
	return true;
}

//! Queues a global pel photon-map shoot (see ShootCausticPelPhotons for timing).
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootGlobalPelPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	const bool shootFromMeshLights					///< [in] Should we shoot from mesh based lights (luminaries)?
	)
{
	PendingGlobalPelShoot req = {};
	req.num = num;
	req.powerScale = power_scale;
	req.maxRecur = maxRecur;
	req.minImportance = minImportance;
	req.branch = branch;
	req.shootFromNonMeshLights = shootFromNonMeshLights;
	req.temporalSamples = temporal_samples;
	req.regenerate = regenerate;
	req.shootFromMeshLights = shootFromMeshLights;
	pScene->QueueGlobalPelPhotonShoot( req );
	return true;
}

//! Queues a translucent pel photon-map shoot (see ShootCausticPelPhotons for timing).
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootTranslucentPelPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const bool reflect,								///< [in] Should we trace reflected rays?
	const bool refract,								///< [in] Should we trace refracted rays?
	const bool direct_translucent,					///< [in] Should we trace translucent primary interaction rays?
	const bool shootFromNonMeshLights,				///< [in] Should we shoot from non mesh based lights?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate,							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	const bool shootFromMeshLights					///< [in] Should we shoot from mesh based lights (luminaries)?
	)
{
	GlobalLog()->PrintEasyWarning( "The Translucent PhotonMap is deprecated.  You should consider using one of the subsurface scattering shaders instead." );

	PendingTranslucentPelShoot req = {};
	req.num = num;
	req.powerScale = power_scale;
	req.maxRecur = maxRecur;
	req.minImportance = minImportance;
	req.reflect = reflect;
	req.refract = refract;
	req.directTranslucent = direct_translucent;
	req.shootFromNonMeshLights = shootFromNonMeshLights;
	req.temporalSamples = temporal_samples;
	req.regenerate = regenerate;
	req.shootFromMeshLights = shootFromMeshLights;
	pScene->QueueTranslucentPelPhotonShoot( req );
	return true;
}

//! Queues a caustic spectral photon-map shoot (see ShootCausticPelPhotons for timing).
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootCausticSpectralPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const double nm_begin,							///< [in] Wavelength to start shooting photons at
	const double nm_end,							///< [in] Wavelength to end shooting photons at
	const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const bool reflect,								///< [in] Should we trace reflected rays?
	const bool refract,								///< [in] Should we trace refracted rays?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	PendingCausticSpectralShoot req = {};
	req.num = num;
	req.powerScale = power_scale;
	req.maxRecur = maxRecur;
	req.minImportance = minImportance;
	req.nmBegin = nm_begin;
	req.nmEnd = nm_end;
	req.numWavelengths = num_wavelengths;
	req.branch = branch;
	req.reflect = reflect;
	req.refract = refract;
	req.temporalSamples = temporal_samples;
	req.regenerate = regenerate;
	pScene->QueueCausticSpectralPhotonShoot( req );
	return true;
}

//! Queues a global spectral photon-map shoot (see ShootCausticPelPhotons for timing).
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootGlobalSpectralPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const double power_scale,						///< [in] How much to scale light power by
	const unsigned int maxRecur,					///< [in] Maximum level of recursion when tracing the photon
	const double minImportance,						///< [in] Minimum importance when a photon is discarded
	const double nm_begin,							///< [in] Wavelength to start shooting photons at
	const double nm_end,							///< [in] Wavelength to end shooting photons at
	const unsigned int num_wavelengths,				///< [in] Number of wavelengths to shoot photons at
	const bool branch,								///< [in] Should the tracer branch or follow a single path?
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	PendingGlobalSpectralShoot req = {};
	req.num = num;
	req.powerScale = power_scale;
	req.maxRecur = maxRecur;
	req.minImportance = minImportance;
	req.nmBegin = nm_begin;
	req.nmEnd = nm_end;
	req.numWavelengths = num_wavelengths;
	req.branch = branch;
	req.temporalSamples = temporal_samples;
	req.regenerate = regenerate;
	pScene->QueueGlobalSpectralPhotonShoot( req );
	return true;
}

//! Queues a shadow photon-map shoot (see ShootCausticPelPhotons for timing).
/// \return TRUE if successful, FALSE otherwise
bool Job::ShootShadowPhotons(
	const unsigned int num,							///< [in] Number of photons to acquire
	const unsigned int temporal_samples,			///< [in] Number of temporal samples to take for animation frames
	const bool regenerate							///< [in] Should the tracer regenerate a new photon each time the scene time changes?
	)
{
	PendingShadowShoot req = {};
	req.num = num;
	req.temporalSamples = temporal_samples;
	req.regenerate = regenerate;
	pScene->QueueShadowPhotonShoot( req );
	return true;
}

//! Predicts the amount of time in ms it will take to rasterize the current scene
/// \return TRUE if successful, FALSE otherwise
bool Job::PredictRasterizationTime(
	unsigned int num,								///< [in] Number of samples to take when determining how long it will take (higher is more accurate)
	unsigned int* ms,								///< [out] Amount of in ms it would take to rasterize
	unsigned int* actual							///< [out] Actual time it took to do the predicted kernel
	)
{
	if( !pRasterizer ) {
		return false;
	}

	ISampling2D* pSampling = 0;
	RISE_API_CreateNRooksSampling2D( &pSampling, 1.0, 1.0, 1.0 );
	pSampling->SetNumSamples( num );

	unsigned int nMs = pRasterizer->PredictTimeToRasterizeScene( *pScene, *pSampling, actual );

	if( ms ) {
		*ms = nMs;
	}

	safe_release( pSampling );
	return true;
}

static IRasterizeSequence* RasterizeSequenceFromOptions()
{
	// Read the raster sequence options from the options file
	IOptions& options = GlobalOptions();

	const int raster_sequence_type = options.ReadInt( "raster_sequence_type", 4 );
	RISE::String raster_sequence = options.ReadString( "raster_sequence_options", "" );

	IRasterizeSequence* pSeq = 0;
	// parse the options
	// Get the raster sequence type
	switch( raster_sequence_type )
	{
	default:
	case 4:
		// Morton Z-order curve (default) - optimal cache locality
		{
			unsigned int tileSize = 32;
			if( !raster_sequence.empty() ) {
				sscanf( raster_sequence.c_str(), "%u", &tileSize );
			}
			RISE_API_CreateMortonRasterizeSequence( &pSeq, tileSize );
		}
		break;

	case 3:
		// Legacy random (deprecated)
		if( GlobalRNG().CanonicalRandom() < 0.1 ) {
			RISE_API_CreateHilbertRasterizeSequence( &pSeq, 4 );
		} else {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, 64, 64, (char)floor(GlobalRNG().CanonicalRandom()*8.999999) );
		}
		break;

	case 0:
		// Scanline (deprecated)
		RISE_API_CreateScanlineRasterizeSequence( &pSeq );
		break;
	case 1:
		// Block (deprecated)
		unsigned int width, height, type;
		if( sscanf( raster_sequence.c_str(), "%u %u %u", &width, &height, &type ) == 3 ) {
			RISE_API_CreateBlockRasterizeSequence( &pSeq, width, height, (char)type );
		}
		break;
	case 2:
		// Hilbert (deprecated)
		unsigned int depth;
		if( sscanf( raster_sequence.c_str(), "%u", &depth ) == 1 ) {
			RISE_API_CreateHilbertRasterizeSequence( &pSeq, depth );
		}
		break;
	}

	return pSeq;
}

//! Rasterizes the entire scene
/// \return TRUE if successful, FALSE otherwise
bool Job::Rasterize(
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
		}
	}

	pRasterizer->RasterizeScene( *pScene, 0, pSeq );
	safe_release( pSeq );

	return true;
}

//! Rasterizes an animation
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeAnimation(
	const double time_start,						///< [in] Scene time to start rasterizing at
	const double time_end,							///< [in] Scene time to finish rasterizing
	const unsigned int num_frames,					///< [in] Number of frames to rasterize
	const bool do_fields,							///< [in] Should the rasterizer do fields?
	const bool invert_fields						///< [in] Should the fields be temporally inverted?
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
		}
	}

	pRasterizer->RasterizeSceneAnimation( *pScene, time_start, time_end, num_frames, do_fields, invert_fields, 0, 0, pSeq );

	return true;
}

//! Raterizes the scene in this region.  The region values are inclusive!
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeRegion(
	const unsigned int left,						///< [in] Left most pixel
	const unsigned int top,							///< [in] Top most scanline
	const unsigned int right,						///< [in] Right most pixel
	const unsigned int bottom						///< [in] Bottom most scanline
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
		}
	}

	Rect	rc( top, left, bottom, right );
	pRasterizer->RasterizeScene( *pScene, &rc, pSeq );
	safe_release( pSeq );

	return true;
}


//
// Transformation of elements
//

//! Sets the a given object's position
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectPosition(
	const char* name,								///< [in] Name of the object
	const double pos[3]								///< [in] Position of the object
	)
{
	if( !name ) {
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetPosition( Point3( pos ) );
	pObj->FinalizeTransformations();
	return true;
}

//! Sets a given object's orientation
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectOrientation(
	const char* name,								///< [in] Name of the object
	const double orient[3]							///< [in] Orientation of the object
	)
{
	if( !name ) {
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetOrientation( Vector3( orient ) );
	pObj->FinalizeTransformations();
	return true;
}

//! Sets a given object's scale
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectScale(
	const char* name,								///< [in] Name of the object
	const double scale								///< [in] Scaling of the object
	)
{
	if( !name ) {
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetScale( scale );
	pObj->FinalizeTransformations();
	return true;
}

//
// Object modification functions
//

//! Sets the UV generator for an object
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectUVToSpherical(
	const char* name,								///< [in] Name of the object
	const double radius								///< [in] Radius of the sphere
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	IUVGenerator* pUV = 0;
	RISE_API_CreateSphericalUVGenerator( &pUV, radius );

	return pObj->SetUVGenerator( *pUV );
}

//! Sets the UV generator for an object
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectUVToBox(
	const char* name,								///< [in] Name of the object
	const double width,								///< [in] Width of the box
	const double height,							///< [in] Height of the box
	const double depth								///< [in] Depth of the box
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	IUVGenerator* pUV = 0;
	RISE_API_CreateBoxUVGenerator( &pUV, width, height, depth );

	return pObj->SetUVGenerator( *pUV );
}

//! Sets the UV generator for an object
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectUVToCylindrical(
	const char* name,								///< [in] Name of the object
	const double radius,							///< [in] Radius of the cylinder
	const char axis,								///< [in] Axis the cylinder is sitting on
	const double size								///< [in] Size of the cylinder
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	IUVGenerator* pUV = 0;
	RISE_API_CreateCylindricalUVGenerator( &pUV, radius, axis, size );

	return pObj->SetUVGenerator( *pUV );
}

//! Sets the object's surface intersection threshold
/// \return TRUE if successful, FALSE otherwise
bool Job::SetObjectIntersectionError(
	const char* name,								///< [in] Name of the object
	const double error								///< [in] Threshold of error
	)
{
	IObjectPriv* pObj = pObjectManager->GetItem( name );

	if( !pObj ) {
		return false;
	}

	pObj->SetSurfaceIntersecError( error );
	return true;
}

//
// `> modify` runtime-mutation surface.  Each looks an element up by
// manager name (mirroring SetObjectIntersectionError) and mutates it in
// place.  All run before a render while the scene is mutable; the next
// RayCaster::AttachScene re-Prepares the light set and rebuilds the
// environment sampler, so emissive<->non-emissive material swaps and
// radiance-scale changes are picked up with no extra dirty plumbing.
//

bool Job::SetObjectMaterial(
	const char* objName,							///< [in] Name of the object to retarget
	const char* materialName						///< [in] Name of the material to bind
	)
{
	if( !objName || !materialName ) {
		GlobalLog()->PrintEasyError( "Job::SetObjectMaterial:: null object or material name" );
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( objName );
	if( !pObj ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetObjectMaterial:: object not found `%s`", objName );
		return false;
	}

	IMaterial* pMat = pMatManager->GetItem( materialName );
	if( !pMat ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetObjectMaterial:: material not found `%s`", materialName );
		return false;
	}

	// Mirrors the interactive editor's material-swap path
	// (SceneEditor: materialManager->GetItem -> obj.AssignMaterial).
	// AssignMaterial releases the prior material and addrefs the new
	// one.  No acceleration rebuild — a material change cannot move the
	// object's bounding box.
	const IMaterial* prevMat = pObj->GetMaterial();
	pObj->AssignMaterial( *pMat );
	if( ( prevMat && prevMat->GetEmitter() ) || ( pMat && pMat->GetEmitter() ) )
		BumpSceneLightGen( pScene );   // P2a: emitter set may have changed
	return true;
}

bool Job::SetObjectShader(
	const char* objName,							///< [in] Name of the object to retarget
	const char* shaderName							///< [in] Name of the shader to bind
	)
{
	if( !objName || !shaderName ) {
		GlobalLog()->PrintEasyError( "Job::SetObjectShader:: null object or shader name" );
		return false;
	}

	IObjectPriv* pObj = pObjectManager->GetItem( objName );
	if( !pObj ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetObjectShader:: object not found `%s`", objName );
		return false;
	}

	IShader* pShader = pShaderManager->GetItem( shaderName );
	if( !pShader ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetObjectShader:: shader not found `%s`", shaderName );
		return false;
	}

	pObj->AssignShader( *pShader );
	return true;
}

bool Job::SetMaterialEmissionScale(
	const char* materialName,						///< [in] Name of the luminaire material
	const double scale								///< [in] New emission scale
	)
{
	if( !materialName ) {
		GlobalLog()->PrintEasyError( "Job::SetMaterialEmissionScale:: null material name" );
		return false;
	}

	IMaterial* pMat = pMatManager->GetItem( materialName );
	if( !pMat ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetMaterialEmissionScale:: material not found `%s`", materialName );
		return false;
	}

	// IMaterial::SetEmissionScale defaults to a no-op returning false, so
	// non-luminaire materials reject the change here.
	if( !pMat->SetEmissionScale( Scalar( scale ) ) ) {
		GlobalLog()->PrintEx( eLog_Error, "Job::SetMaterialEmissionScale:: material `%s` does not support emission-scale modification (not an emissive material, or its emission is not scale-mutable)", materialName );
		return false;
	}

	BumpSceneLightGen( pScene );   // P2a: exitance changed -> sampler weights stale
	return true;
}

bool Job::SetActiveRasterizerRadianceScale(
	const double scale								///< [in] New environment radiance scale
	)
{
	if( scale < 0 ) {
		GlobalLog()->PrintEasyError( "Job::SetActiveRasterizerRadianceScale:: radiance scale must be >= 0 (negative is nonphysical)" );
		return false;
	}

	if( !pRasterizer ) {
		GlobalLog()->PrintEasyError( "Job::SetActiveRasterizerRadianceScale:: no active rasterizer" );
		return false;
	}

	// Every in-tree pixel-based rasterizer (PT / BDPT / VCM / MLT and the
	// spectral variants) derives from PixelBasedRasterizerHelper, which
	// owns the RayCaster.  Reach the concrete RayCaster through it.
	RISE::Implementation::PixelBasedRasterizerHelper* pHelper =
		dynamic_cast<RISE::Implementation::PixelBasedRasterizerHelper*>( pRasterizer );
	if( !pHelper ) {
		GlobalLog()->PrintEasyError( "Job::SetActiveRasterizerRadianceScale:: active rasterizer has no ray caster" );
		return false;
	}

	RISE::Implementation::RayCaster* pConcreteCaster =
		dynamic_cast<RISE::Implementation::RayCaster*>( pHelper->GetRayCaster() );
	if( !pConcreteCaster ) {
		GlobalLog()->PrintEasyError( "Job::SetActiveRasterizerRadianceScale:: active rasterizer has no ray caster" );
		return false;
	}

	// Drive the environment importance sampler (NEE) scale: the next
	// AttachScene rebuilds the EnvironmentSampler from this override.
	pConcreteCaster->SetRadianceScale( Scalar( scale ) );

	// Keep the direct-view / ray-miss background consistent with NEE by
	// pushing the same scale into the scene's radiance map.  A scene with
	// no global radiance map simply has nothing to update (the override
	// on the caster is still recorded, harmlessly, for symmetry).
	if( pScene ) {
		IRadianceMap* pRm = pScene->GetGlobalRadianceMapMutable();
		if( pRm ) {
			pRm->SetScale( Scalar( scale ) );
		}
	}

	return true;
}

//
// Removal of objects
//

//! Removes the given painter from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemovePainter(
	const char* name								///< [in] Name of the painter to remove
	)
{
	return pPntManager->RemoveItem( name );
}

//! Removes the given material from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveMaterial(
	const char* name								///< [in] Name of the material to remove
	)
{
	const bool removed = pMatManager->RemoveItem( name );
	// Drop the composed-material marker even if removal failed:
	// the manager's RemoveItem rejects unknown names, but if the
	// caller happened to clear a stale marker for a name that was
	// never registered (degenerate), we want the set + manager to
	// stay in sync.  More importantly: a successful removal MUST
	// erase the marker, or a subsequent direct-material add under
	// the same name would inherit the read-only flag.
	if( name ) {
		composedMaterialNames.erase( String( name ) );
	}
	return removed;
}

//! Removes the given geometry from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveGeometry(
	const char* name								///< [in] Name of the geometry to remove
	)
{
	return pGeomManager->RemoveItem( name );
}

//! Removes the given object from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveObject(
	const char* name								///< [in] Name of the object to remove
	)
{
	// P2a: if the removed object was an emissive mesh luminary, the luminary
	// set changes -> bump so a reused caster rebuilds.
	IObjectPriv* pRObj = pObjectManager->GetItem( name );
	const bool wasEmissive = ( pRObj && pRObj->GetMaterial() && pRObj->GetMaterial()->GetEmitter() );
	const bool ok = pObjectManager->RemoveItem( name );
	if( ok && wasEmissive ) BumpSceneLightGen( pScene );
	return ok;
}

//! Removes the given light from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveLight(
	const char* name								///< [in] Name of the light to remove
	)
{
	const bool ok = pLightManager->RemoveItem( name );
	if( ok ) BumpSceneLightGen( pScene );   // P2a: light set changed
	return ok;
}

//! Removes the given modifier from the scene
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveModifier(
	const char* name								///< [in] Name of the modifer to remove
	)
{
	return pModManager->RemoveItem( name );
}

//! Removes all the rasterizer outputs
/// \return TRUE if successful, FALSE otherwise
bool Job::RemoveRasterizerOutputs(
	)
{
	pRasterizer->FreeRasterizerOutputs();
	return true;
}

//! Clears the entire scene, resets everything back to defaults
/// \return TRUE if successful, FALSE otherwise
bool Job::ClearAll(
	)
{
	DestroyContainers();
	InitializeContainers();

	return true;
}


//! Loading an ascii scene description
/// \return TRUE if successful, FALSE otherwise
bool Job::LoadAsciiScene(
	const char* filename							///< [in] Name of the file containing the scene
	)
{
	if( !filename ) {
		return false;
	}

	ISceneParser* sceneParser = 0;
	RISE_API_CreateAsciiSceneParser( &sceneParser, filename );
	bool bRet = sceneParser->ParseAndLoadScene( *this );
	safe_release( sceneParser );

	// L6b — push the canonical FrameStore to every rasterizer in the
	// registry now that scene load is complete and the active camera's
	// dims are finally known.  Most scene files declare the rasterizer
	// chunk BEFORE the camera chunk, so at rasterizer-construction time
	// the ResolveJobFrameStoreForActiveCamera() call returned nullptr.
	// Re-resolve here and SetFrameStore on each registry entry.
	if( bRet ) {
		PushJobFrameStoreToRasterizers();
	}
	return bRet;
}

// L6b — push the canonical FrameStore to every registered rasterizer.
// Called after scene load completes and after SetActiveCamera in case
// the new camera has different dimensions.  Each rasterizer addrefs
// the FrameStore via Rasterizer::SetFrameStore (releases its previous
// reference if any) so the ref count is balanced.  Safe to call when
// no FrameStore can be allocated yet (no active camera) — passes
// nullptr through, which clears any stale FrameStore on each rasterizer.
void Job::PushJobFrameStoreToRasterizers()
{
	RISE::Implementation::FrameStore* fs = ResolveJobFrameStoreForActiveCamera();
	for( RasterizerRegistry::iterator it = rasterizerRegistry.begin();
	     it != rasterizerRegistry.end(); ++it )
	{
		if( !it->second.instance ) continue;

		// Cast to the concrete `Rasterizer` impl to access SetFrameStore
		// + the AcceptsFrameStorePush capability hook.  This is safe —
		// every in-tree rasterizer derives from
		// `Implementation::Rasterizer`, and the registry only holds
		// instances we constructed via RISE_API factories.
		RISE::Implementation::Rasterizer* r =
			dynamic_cast<RISE::Implementation::Rasterizer*>( it->second.instance );
		if( !r ) continue;

		// L6e-1.1 — capability gate via virtual on `Rasterizer`
		// (replaces a brittle string-match on registry name).  MLT
		// (and MLTSpectral via inheritance) opts out — its PSSMLT
		// per-round Resolve allocates a fresh local
		// `RISERasterImage` and never writes into the canonical
		// FrameStore, so a push would leave `GetFrameStore()`
		// returning a perpetually-stale store.  L6d-2 will revisit
		// by either threading per-round Clear+Resolve into the
		// FrameStore beauty, or keeping MLT on the legacy path
		// indefinitely.  See `Rasterizer::AcceptsFrameStorePush`.
		if( !r->AcceptsFrameStorePush() ) continue;

		r->SetFrameStore( fs );
	}
}

//! Runs an ascii script
/// \return TRUE if successful, FALSE otherwise
bool Job::RunAsciiScript(
	const char* filename							///< [in] Name of the file containing the script
	)
{
	if( !filename ) {
		return false;
	}

	IScriptParser* scriptParser = 0;
	RISE_API_CreateAsciiScriptParser( &scriptParser, filename );
	bool bRet = scriptParser->ParseScript( *this );
	safe_release( scriptParser );

	return bRet;
}

//! Tells us whether anything is keyframed
bool Job::AreThereAnyKeyframedObjects()
{
	return pScene->GetAnimator()->AreThereAnyKeyframedObjects();
}

//! Adds a keyframe for the specified element (legacy; routes to the
//! implicit default animation).
bool Job::AddKeyframe(
	const char* element_type,
	const char* element,
	const char* param,
	const char* value,
	const double time,
	const char* interp,
	const char* interp_params
	)
{
	return AddKeyframeToAnimation( element_type, element, param, value, time, interp, interp_params, 0 );
}

//! Adds a keyframe owned by a named animation (NULL/empty animation => default)
bool Job::AddKeyframeToAnimation(
	const char* element_type,
	const char* element,
	const char* param,
	const char* value,
	const double time,
	const char* interp,
	const char* interp_params,
	const char* animation
	)
{
	if( !element_type || !element || !param || !value ) {
		return false;
	}

	IKeyframable* pkf = 0;

	String type( element_type );
	if( type == "object" ) {
		pkf = pObjectManager->GetItem( element );
	} else if( type == "camera" ) {
		pkf = pScene->GetCameraMutable();
	} else if( type == "geometry" ) {
		pkf = pGeomManager->GetItem( element );
	} else if( type == "painter" ) {
		pkf = pPntManager->GetItem( element );
	} else if( type == "light" ) {
		pkf = pLightManager->GetItem( element );
	}

	if( !pkf ) {
		return false;
	}

	String szinterp = String(interp);
	String szinterpparams = String(interp_params);
	String szanimation = String(animation);

	return pScene->GetAnimator()->InsertKeyframeForAnimation( pkf, String(param), String(value), time, interp?&szinterp:0, interp_params?&szinterpparams:0, szanimation );
}

//! Sets animation rasterization options
//! Basically everything that can be passed to RasterizeAnimation can be passed here
//! Then you can just call RasterizeAnimationUsingOptions
bool Job::SetAnimationOptions(
	const double time_start,						///< [in] Scene time to start rasterizing at
	const double time_end,							///< [in] Scene time to finish rasterizing
	const unsigned int num_frames,					///< [in] Number of frames to rasterize
	const bool do_fields,							///< [in] Should the rasterizer do fields?
	const bool invert_fields						///< [in] Should the fields be temporally inverted?
	)
{
	animOptions.time_start = time_start;
	animOptions.time_end = time_end;
	animOptions.num_frames = num_frames;
	animOptions.do_fields = do_fields;
	animOptions.invert_fields = invert_fields;

	// Also seed the implicit default animation so it carries the real time
	// range (named-animation paths read per-animation playback options).
	if( pScene && pScene->GetAnimator() ) {
		pScene->GetAnimator()->DeclareAnimation( String("(default)"), time_start, time_end, num_frames, do_fields, invert_fields, false );
	}

	return true;
}

bool Job::GetAnimationOptions(
	double& time_start,
	double& time_end,
	unsigned int& num_frames,
	bool& do_fields,
	bool& invert_fields
	) const
{
	// Prefer the ACTIVE named animation's options (so the GUI timeline range
	// and renderanimation-using-options follow the selected animation); fall
	// back to the legacy global preset when nothing is declared.
	if( pScene && pScene->GetAnimator() &&
		pScene->GetAnimator()->GetActiveAnimationOptions( time_start, time_end, num_frames, do_fields, invert_fields ) ) {
		return true;
	}

	time_start    = animOptions.time_start;
	time_end      = animOptions.time_end;
	num_frames    = animOptions.num_frames;
	do_fields     = animOptions.do_fields;
	invert_fields = animOptions.invert_fields;
	return true;
}

//! Declares (or updates) a named animation; routes to the animator.
bool Job::DeclareAnimation(
	const char* name,
	const double time_start,
	const double time_end,
	const unsigned int num_frames,
	const bool do_fields,
	const bool invert_fields,
	const bool make_active
	)
{
	return pScene->GetAnimator()->DeclareAnimation( String(name), time_start, time_end, num_frames, do_fields, invert_fields, make_active );
}

bool Job::SetActiveAnimation( const char* name )
{
	return pScene->GetAnimator()->SetActiveAnimationByName( String(name) );
}

bool Job::SetActiveAnimationByIndex( const unsigned int index )
{
	return pScene->GetAnimator()->SetActiveAnimationByIndex( index );
}

unsigned int Job::GetAnimationCount() const
{
	return pScene->GetAnimator()->GetAnimationCount();
}

bool Job::GetAnimationName( const unsigned int index, char* buf, const unsigned int bufLen ) const
{
	if( !buf || bufLen == 0 ) {
		return false;
	}
	if( index >= pScene->GetAnimator()->GetAnimationCount() ) {
		buf[0] = 0;
		return false;
	}
	const String n = pScene->GetAnimator()->GetAnimationName( index );
	const char* src = n.c_str();
	unsigned int i = 0;
	for( ; src[i] && i+1 < bufLen; i++ ) {
		buf[i] = src[i];
	}
	buf[i] = 0;
	return true;
}

unsigned int Job::GetActiveAnimationIndex() const
{
	return pScene->GetAnimator()->GetActiveAnimationIndex();
}

bool Job::GetActiveAnimationName( char* buf, const unsigned int bufLen ) const
{
	if( !buf || bufLen == 0 ) {
		return false;
	}
	const String n = pScene->GetAnimator()->GetActiveAnimationName();
	const char* src = n.c_str();
	unsigned int i = 0;
	for( ; src[i] && i+1 < bufLen; i++ ) {
		buf[i] = src[i];
	}
	buf[i] = 0;
	return ( src[0] != 0 );
}

//! Rasterizes an animation using the global preset options
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeAnimationUsingOptions(
	)

{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions( );
		if( !pSeq ) {
			RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
		}
	}

	double aTs=0, aTe=1; unsigned int aNf=30; bool aDf=false, aInvf=false;
	GetAnimationOptions( aTs, aTe, aNf, aDf, aInvf );

	pRasterizer->RasterizeSceneAnimation( *pScene,
		aTs, aTe, aNf, aDf, aInvf, 0, 0, pSeq );

	return true;
}

//! Rasterizes a frame of an animation using the global preset options
/// \return TRUE if successful, FALSE otherwise
bool Job::RasterizeAnimationUsingOptions(
	const unsigned int frame						///< [in] The frame to rasterize
	)
{
	if( !pRasterizer ) {
		return false;
	}

	IRasterizeSequence* pSeq = 0;

	if( pGlobalProgress ) {
		pRasterizer->SetProgressCallback( pGlobalProgress );

		pSeq = RasterizeSequenceFromOptions();
		if( !pSeq ) {
			RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
		}
	}

	double aTs=0, aTe=1; unsigned int aNf=30; bool aDf=false, aInvf=false;
	GetAnimationOptions( aTs, aTe, aNf, aDf, aInvf );

	pRasterizer->RasterizeSceneAnimation( *pScene,
		aTs, aTe, aNf, aDf, aInvf, 0, &frame, pSeq );

	return true;
}

void Job::SetProgress( IProgressCallback* pProgress ) {
	pGlobalProgress = pProgress;
}

// ============================================================
//  Rasterizer registry — see Job.h / IJob.h for the contract.
//
//  The interactive editor's accordion lists each registered
//  rasterizer in a "Rasterizer" section; clicking one calls
//  SetActiveRasterizer to swap the active pointer without
//  re-instantiating.  The parser registers exactly one entry
//  per Set*Rasterizer call (the type-key matches the .RISEscene
//  chunk name).  Phase-2 will add eager pre-instantiation of
//  the standard 8-type set with sensible defaults so the user
//  can swap freely without having to declare each variant in
//  the scene file.
// ============================================================

void Job::RegisterAndActivateRasterizer( const std::string& name, IRasterizer* pRaster,
	const RasterizerParams& params )
{
	if( !pRaster ) return;

	// Replace any prior entry under this key.  Existing entries hold
	// addrefs we put there ourselves, so safe_release matches.  The
	// active pointer (`pRasterizer`) is just a borrow into the map; if
	// it pointed at the now-released entry we'll fix it up below.
	RasterizerRegistry::iterator it = rasterizerRegistry.find( name );
	if( it != rasterizerRegistry.end() ) {
		if( it->second.instance == pRaster ) {
			// Self-register: same instance under the same key.  Skip
			// the release/store dance — releasing the only addref the
			// map held would delete the instance, leaving us with a
			// dangling pointer when we re-store and re-activate.  Just
			// re-affirm activation and refresh the snapshot.
			it->second.params = params;
			pRasterizer = pRaster;
			activeRasterizerName = name;
			return;
		}
		if( pRasterizer == it->second.instance ) {
			pRasterizer = 0;   // breaks the dangling-borrow
		}
		safe_release( it->second.instance );
		it->second.instance = pRaster;
		it->second.params   = params;
	} else {
		RasterizerEntry entry;
		entry.instance = pRaster;
		entry.params   = params;
		rasterizerRegistry[ name ] = entry;
	}

	// Make the new instance active.  pRaster's reference count is
	// owned by the registry now — pRasterizer is a non-owning borrow.
	pRasterizer = pRaster;
	activeRasterizerName = name;
}

const Job::RasterizerParams* Job::GetRasterizerParams( const std::string& name ) const
{
	RasterizerRegistry::const_iterator it = rasterizerRegistry.find( name );
	if( it == rasterizerRegistry.end() ) return 0;
	return &it->second.params;
}

bool Job::SetActiveRasterizer( const char* name )
{
	if( !name || !*name ) return false;
	const std::string key( name );
	RasterizerRegistry::iterator it = rasterizerRegistry.find( key );
	if( it != rasterizerRegistry.end() ) {
		// Already instantiated — just rebind.
		pRasterizer = it->second.instance; // borrow; addref stays with registry
		activeRasterizerName = key;
		return true;
	}
	// Not in the registry yet.  Try to lazy-build it from the standard
	// types catalogue with sensible defaults.  On success, the
	// Set*Rasterizer path (called from inside the helper) registers AND
	// activates the new instance.
	return InstantiateRasterizerWithDefaults( key );
}

const std::vector<std::string>& Job::StandardRasterizerTypes()
{
	// Display order: Auto (the recommended auto-routing dispatcher) leads,
	// then PT before BDPT before VCM before MLT, with Pel
	// before Spectral within each family.  Matches the conventional
	// "simple → complex" reading the user expects in the accordion.
	static const std::vector<std::string> kTypes = {
		"auto_rasterizer",
		"auto_spectral_rasterizer",
		"pathtracing_pel_rasterizer",
		"pathtracing_spectral_rasterizer",
		"bdpt_pel_rasterizer",
		"bdpt_spectral_rasterizer",
		"vcm_pel_rasterizer",
		"vcm_spectral_rasterizer",
		"mlt_rasterizer",
		"mlt_spectral_rasterizer",
	};
	return kTypes;
}

namespace {

// Build the union of (StandardRasterizerTypes ∪ registered keys) in
// stable order: standard types first (in display order), then any
// registered legacy types (e.g. `pixelpel_rasterizer`,
// `pixelintegratingspectral_rasterizer`) sorted lex.  Used by both
// GetRasterizerTypeCount and GetRasterizerTypeName so the index ↔
// name mapping is consistent.
std::vector<std::string> ComputeRasterizerUnion( const Job::RasterizerRegistry& registry )
{
	const std::vector<std::string>& std_list = Job::StandardRasterizerTypes();
	std::vector<std::string> out;
	out.reserve( std_list.size() + registry.size() );
	out = std_list;
	// Append registry entries that aren't already in the standard list.
	// std::map iterates in key-order, so the appended tail is sorted.
	std::set<std::string> std_set( std_list.begin(), std_list.end() );
	for( Job::RasterizerRegistry::const_iterator it = registry.begin();
	     it != registry.end(); ++it )
	{
		if( std_set.find( it->first ) == std_set.end() ) {
			out.push_back( it->first );
		}
	}
	return out;
}

}  // namespace

unsigned int Job::GetRasterizerTypeCount() const
{
	return static_cast<unsigned int>(
		ComputeRasterizerUnion( rasterizerRegistry ).size() );
}

std::string Job::GetRasterizerTypeName( unsigned int idx ) const
{
	const std::vector<std::string> all = ComputeRasterizerUnion( rasterizerRegistry );
	if( idx >= all.size() ) return std::string();
	return all[idx];
}

namespace {

// Pick a default shader name for lazy-building a rasterizer.  Prefer
// the shader the currently-active rasterizer was built with — but
// IRasterizer doesn't expose its shader name, so we fall back to the
// first shader registered with the manager.  Returns empty string if
// no shader exists; the caller treats that as "can't lazy-build".
std::string PickDefaultShaderName( const IShaderManager* mgr )
{
	if( !mgr ) return std::string();
	struct FirstName : public IEnumCallback<const char*> {
		std::string out;
		bool operator()( const char* const& name ) override {
			out = name ? std::string( name ) : std::string();
			return false;   // stop after first
		}
	};
	FirstName cb;
	mgr->EnumerateItemNames( cb );
	return cb.out;
}

}  // namespace

namespace {

// Per-type rebuild dispatcher.  Reads the supplied snapshot and calls
// the matching Set*Rasterizer, which captures the snapshot back into
// the registry under the same key (replacing the prior instance).
//
// Returns the result of the underlying Set*Rasterizer, or false for
// an unknown type-name.  Each branch passes only the params that
// `Set*Rasterizer` accepts; other snapshot fields are ignored.
bool RebuildRasterizer( Job& job, const std::string& name, const Job::RasterizerParams& p )
{
	if( name == "pathtracing_pel_rasterizer" ) {
		return job.SetPathTracingPelRasterizer(
			p.numPixelSamples, p.shader.c_str(), p.radianceMap, p.pixelFilter,
			p.showLuminaires, p.sms,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pathGuiding, p.adaptive, p.stability, p.progressive );
	}
	if( name == "pathtracing_spectral_rasterizer" ) {
		return job.SetPathTracingSpectralRasterizer(
			p.numPixelSamples, p.shader.c_str(), p.radianceMap, p.pixelFilter,
			p.showLuminaires, p.spectral, p.sms,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.adaptive, p.stability, p.progressive );
	}
	if( name == "bdpt_pel_rasterizer" ) {
		return job.SetBDPTPelRasterizer(
			p.numPixelSamples, p.maxEyeDepth, p.maxLightDepth,
			p.shader.c_str(), p.radianceMap, p.pixelFilter, p.showLuminaires,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pathGuiding, p.adaptive, p.stability, p.progressive );
	}
	if( name == "bdpt_spectral_rasterizer" ) {
		return job.SetBDPTSpectralRasterizer(
			p.numPixelSamples, p.maxEyeDepth, p.maxLightDepth,
			p.shader.c_str(), p.radianceMap, p.pixelFilter, p.showLuminaires,
			p.spectral,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pathGuiding, p.adaptive, p.stability, p.progressive );
	}
	if( name == "vcm_pel_rasterizer" ) {
		return job.SetVCMPelRasterizer(
			p.numPixelSamples, p.maxEyeDepth, p.maxLightDepth,
			p.shader.c_str(), p.radianceMap, p.pixelFilter, p.showLuminaires,
			p.mergeRadius, p.enableVC, p.enableVM,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pathGuiding, p.adaptive, p.stability, p.progressive );
	}
	if( name == "vcm_spectral_rasterizer" ) {
		return job.SetVCMSpectralRasterizer(
			p.numPixelSamples, p.maxEyeDepth, p.maxLightDepth,
			p.shader.c_str(), p.radianceMap, p.pixelFilter, p.showLuminaires,
			p.spectral, p.mergeRadius, p.enableVC, p.enableVM,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pathGuiding, p.adaptive, p.stability, p.progressive );
	}
	if( name == "mlt_rasterizer" ) {
		return job.SetMLTRasterizer(
			p.maxEyeDepth, p.maxLightDepth,
			p.nBootstrap, p.nChains, p.nMutationsPerPixel, p.largeStepProb,
			p.shader.c_str(), p.showLuminaires,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pixelFilter, p.stability );
	}
	if( name == "mlt_spectral_rasterizer" ) {
		return job.SetMLTSpectralRasterizer(
			p.maxEyeDepth, p.maxLightDepth,
			p.nBootstrap, p.nChains, p.nMutationsPerPixel, p.largeStepProb,
			p.shader.c_str(), p.showLuminaires, p.spectral,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pixelFilter, p.stability );
	}
	if( name == "auto_rasterizer" ) {
		return job.SetAutoRasterizer(
			p.autoIntegrator, p.numPixelSamples,
			p.shader.c_str(), p.radianceMap, p.pixelFilter, p.showLuminaires,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.pathGuiding, p.adaptive, p.stability, p.progressive,
			p.autoProbeEnabled );
	}
	if( name == "auto_spectral_rasterizer" ) {
		return job.SetAutoSpectralRasterizer(
			p.autoIntegrator, p.numPixelSamples,
			p.shader.c_str(), p.radianceMap, p.pixelFilter, p.showLuminaires,
			p.spectral,
			p.oidnDenoise, p.oidnQuality, p.oidnDevice, p.oidnPrefilter,
			p.adaptive, p.stability, p.progressive,
			p.autoProbeEnabled );
	}
	return false;
}

// Format a parameter from the snapshot as a parser-formatted string.
// Mirrors what `RasterizerIntrospection` displays so a round-trip
// through SetRasterizerParameter is idempotent.
std::string FormatRasterizerParam( const Job::RasterizerParams& p, const std::string& paramName )
{
	char buf[64];
	if( paramName == "samples" || paramName == "numSamples" ) {
		std::snprintf( buf, sizeof(buf), "%u", p.numPixelSamples );
		return buf;
	}
	if( paramName == "max_eye_depth" || paramName == "maxEyeDepth" ) {
		std::snprintf( buf, sizeof(buf), "%u", p.maxEyeDepth );
		return buf;
	}
	if( paramName == "max_light_depth" || paramName == "maxLightDepth" ) {
		std::snprintf( buf, sizeof(buf), "%u", p.maxLightDepth );
		return buf;
	}
	if( paramName == "show_luminaires" || paramName == "showLuminaires" ) {
		return p.showLuminaires ? "true" : "false";
	}
	if( paramName == "merge_radius" || paramName == "mergeRadius" ) {
		std::snprintf( buf, sizeof(buf), "%g", p.mergeRadius );
		return buf;
	}
	if( paramName == "enable_vc" )      return p.enableVC ? "true" : "false";
	if( paramName == "enable_vm" )      return p.enableVM ? "true" : "false";
	if( paramName == "oidn_denoise" )   return p.oidnDenoise ? "true" : "false";
	if( paramName == "bootstrap_samples" || paramName == "bootstrap" || paramName == "nBootstrap" ) {
		std::snprintf( buf, sizeof(buf), "%u", p.nBootstrap );
		return buf;
	}
	if( paramName == "chains" || paramName == "nChains" ) {
		std::snprintf( buf, sizeof(buf), "%u", p.nChains );
		return buf;
	}
	if( paramName == "mutations_per_pixel" || paramName == "mutations" || paramName == "nMutationsPerPixel" ) {
		std::snprintf( buf, sizeof(buf), "%u", p.nMutationsPerPixel );
		return buf;
	}
	if( paramName == "large_step_prob" || paramName == "largeStepProb" ) {
		std::snprintf( buf, sizeof(buf), "%g", p.largeStepProb );
		return buf;
	}
	if( paramName == "integrator" ) {
		switch( p.autoIntegrator ) {
			case AutoIntegratorChoice::PT:   return "pt";
			case AutoIntegratorChoice::BDPT: return "bdpt";
			case AutoIntegratorChoice::VCM:  return "vcm";
			default:                         return "auto";
		}
	}
	return std::string();
}

// String → typed-value parsers.  Anonymous-namespace helpers in the
// codebase's older C++ idiom (no lambdas, no `auto`-typed locals).
bool ParseRasterizerUInt( const std::string& s, unsigned int& out )
{
	unsigned int v = 0;
	if( std::sscanf( s.c_str(), "%u", &v ) != 1 ) return false;
	out = v;
	return true;
}

bool ParseRasterizerDouble( const std::string& s, double& out )
{
	double v = 0;
	if( std::sscanf( s.c_str(), "%lf", &v ) != 1 ) return false;
	out = v;
	return true;
}

bool ParseRasterizerBool( const std::string& s, bool& out )
{
	if( s == "true" || s == "1" || s == "TRUE" || s == "True" )
	{
		out = true;
		return true;
	}
	if( s == "false" || s == "0" || s == "FALSE" || s == "False" )
	{
		out = false;
		return true;
	}
	return false;
}

// Mutate one field of a snapshot from a string value.  Returns false
// for unknown param names or parse failures.
bool ApplyRasterizerParam( Job::RasterizerParams& p, const std::string& paramName, const std::string& valueStr )
{
	if( paramName == "samples" || paramName == "numSamples" )
		return ParseRasterizerUInt( valueStr, p.numPixelSamples );
	if( paramName == "max_eye_depth" || paramName == "maxEyeDepth" )
		return ParseRasterizerUInt( valueStr, p.maxEyeDepth );
	if( paramName == "max_light_depth" || paramName == "maxLightDepth" )
		return ParseRasterizerUInt( valueStr, p.maxLightDepth );
	if( paramName == "show_luminaires" || paramName == "showLuminaires" )
		return ParseRasterizerBool( valueStr, p.showLuminaires );
	if( paramName == "merge_radius" || paramName == "mergeRadius" )
		return ParseRasterizerDouble( valueStr, p.mergeRadius );
	if( paramName == "enable_vc" )    return ParseRasterizerBool( valueStr, p.enableVC );
	if( paramName == "enable_vm" )    return ParseRasterizerBool( valueStr, p.enableVM );
	if( paramName == "oidn_denoise" ) return ParseRasterizerBool( valueStr, p.oidnDenoise );
	if( paramName == "bootstrap_samples" || paramName == "bootstrap" || paramName == "nBootstrap" )
		return ParseRasterizerUInt( valueStr, p.nBootstrap );
	if( paramName == "chains" || paramName == "nChains" )
		return ParseRasterizerUInt( valueStr, p.nChains );
	if( paramName == "mutations_per_pixel" || paramName == "mutations" || paramName == "nMutationsPerPixel" )
		return ParseRasterizerUInt( valueStr, p.nMutationsPerPixel );
	if( paramName == "large_step_prob" || paramName == "largeStepProb" )
		return ParseRasterizerDouble( valueStr, p.largeStepProb );
	if( paramName == "integrator" ) {
		// auto_rasterizer / auto_spectral_rasterizer integrator pin (Tier 0).
		// Same vocabulary as the chunk parser; unknown -> fail (leave unchanged).
		std::string iv( valueStr );
		for( char& ch : iv ) if( ch >= 'A' && ch <= 'Z' ) ch = (char)( ch + 32 );
		if( iv == "pt" || iv == "pathtracing" || iv == "path_tracing" ) { p.autoIntegrator = AutoIntegratorChoice::PT;   return true; }
		if( iv == "bdpt" ) { p.autoIntegrator = AutoIntegratorChoice::BDPT; return true; }
		if( iv == "vcm" )  { p.autoIntegrator = AutoIntegratorChoice::VCM;  return true; }
		if( iv == "auto" ) { p.autoIntegrator = AutoIntegratorChoice::Auto; return true; }
		return false;
	}
	return false;
}

}  // namespace

bool Job::SetRasterizerParameter( const char* rasterizerName, const char* paramName,
	const char* valueStr )
{
	if( !rasterizerName || !paramName || !valueStr ) return false;
	const std::string name( rasterizerName );
	RasterizerRegistry::iterator it = rasterizerRegistry.find( name );
	if( it == rasterizerRegistry.end() ) return false;

	// Mutate a copy of the snapshot, then rebuild.  The rebuild path
	// re-records into the registry via RegisterAndActivateRasterizer,
	// replacing the old instance.  Active rasterizer follows.
	RasterizerParams modified = it->second.params;
	if( !ApplyRasterizerParam( modified, paramName, valueStr ) ) {
		return false;
	}
	return RebuildRasterizer( *this, name, modified );
}

std::string Job::GetRasterizerParameter( const char* rasterizerName, const char* paramName ) const
{
	if( !rasterizerName || !paramName ) return std::string();
	const std::string name( rasterizerName );
	RasterizerRegistry::const_iterator it = rasterizerRegistry.find( name );
	if( it == rasterizerRegistry.end() ) return std::string();
	return FormatRasterizerParam( it->second.params, paramName );
}

bool Job::InstantiateRasterizerWithDefaults( const std::string& name )
{
	// Pick a shader.  Without one, every Set*Rasterizer below would
	// fail at the shader-lookup step anyway, so bail early with a clear
	// log message.
	const std::string shader = PickDefaultShaderName( pShaderManager );
	if( shader.empty() ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"Job::SetActiveRasterizer: cannot lazy-build '%s' — no shader registered.  Declare a `standard_shader` chunk in your scene before requesting this rasterizer.",
			name.c_str() );
		return false;
	}

	// Default-constructed configs = struct in-class initializers.
	// These are the "no-customisation" values every rasterizer gets
	// when there's no chunk in the scene file.  Per-type scalars
	// (samples, max_eye_depth, etc.) come from the matching
	// *Defaults struct in RasterizerDefaults.h.
	RadianceMapConfig     radianceMapConfig;
	PixelFilterConfig     pixelFilterConfig;
	SMSConfig             smsConfig;
	SpectralConfig        spectralConfig;
	PathGuidingConfig     guidingConfig;
	AdaptiveSamplingConfig adaptiveConfig;
	StabilityConfig       stabilityConfig;
	ProgressiveConfig     progressiveConfig;

	if( name == "auto_rasterizer" ) {
		// Interactive "Auto" selection -> the full smart dispatcher with the
		// Tier-2 probe ENABLED (it self-gates on auto_probe_activation_spp, so
		// previews/low-spp stay on the Tier-1 static best-guess; only final
		// renders pay the probe).  This differs from the scene-file chunk
		// default (probe opt-in) on purpose: picking Auto in a UI means "route
		// me to the best integrator", which needs the probe.
		AutoRasterizerDefaults d;
		return SetAutoRasterizer(
			d.integrator, d.numPixelSamples,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig,
			/*probeEnabled*/ true );
	}
	if( name == "auto_spectral_rasterizer" ) {
		AutoRasterizerDefaults d;
		return SetAutoSpectralRasterizer(
			d.integrator, d.numPixelSamples,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			spectralConfig,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			adaptiveConfig, stabilityConfig, progressiveConfig,
			/*probeEnabled*/ true );
	}
	if( name == "pixelpel_rasterizer" ) {
		PixelPelDefaults d;
		const char* lumSampler = ( d.luminarySampler == "none" ) ? 0 : d.luminarySampler.c_str();
		return SetPixelBasedPelRasterizer(
			d.numPixelSamples, d.numLumSamples, d.maxRecursion,
			shader.c_str(), radianceMapConfig,
			lumSampler, d.luminarySamplerParam,
			pixelFilterConfig, d.showLuminaires,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
	}
	if( name == "pixelintegratingspectral_rasterizer" ) {
		PixelIntegratingSpectralDefaults d;
		const char* lumSampler = ( d.luminarySampler == "none" ) ? 0 : d.luminarySampler.c_str();
		// Lazy build skips RGB-SPD curves — the user can opt in via a
		// scene chunk if they need a custom RGB-to-SPD conversion.
		return SetPixelBasedSpectralIntegratingRasterizer(
			d.numPixelSamples, d.numLumSamples, spectralConfig, d.maxRecursion,
			shader.c_str(), radianceMapConfig,
			lumSampler, d.luminarySamplerParam,
			pixelFilterConfig, d.showLuminaires,
			d.integrateRGB, /*numSPDvalues*/ 0u, /*freq*/ 0, /*r*/ 0, /*g*/ 0, /*b*/ 0,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			stabilityConfig );
	}
	if( name == "pathtracing_pel_rasterizer" ) {
		PathTracingPelDefaults d;
		return SetPathTracingPelRasterizer(
			d.numPixelSamples,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			smsConfig, d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
	}
	if( name == "pathtracing_spectral_rasterizer" ) {
		PathTracingSpectralDefaults d;
		return SetPathTracingSpectralRasterizer(
			d.numPixelSamples,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			spectralConfig, smsConfig,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			adaptiveConfig, stabilityConfig, progressiveConfig );
	}
	if( name == "bdpt_pel_rasterizer" ) {
		BDPTPelDefaults d;
		return SetBDPTPelRasterizer(
			d.numPixelSamples, d.maxEyeDepth, d.maxLightDepth,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
	}
	if( name == "bdpt_spectral_rasterizer" ) {
		BDPTSpectralDefaults d;
		return SetBDPTSpectralRasterizer(
			d.numPixelSamples, d.maxEyeDepth, d.maxLightDepth,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			spectralConfig,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
	}
	if( name == "vcm_pel_rasterizer" ) {
		VCMPelDefaults d;
		return SetVCMPelRasterizer(
			d.numPixelSamples, d.maxEyeDepth, d.maxLightDepth,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			d.mergeRadius, d.enableVC, d.enableVM,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
	}
	if( name == "vcm_spectral_rasterizer" ) {
		VCMSpectralDefaults d;
		return SetVCMSpectralRasterizer(
			d.numPixelSamples, d.maxEyeDepth, d.maxLightDepth,
			shader.c_str(), radianceMapConfig, pixelFilterConfig, d.showLuminaires,
			spectralConfig,
			d.mergeRadius, d.enableVC, d.enableVM,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			guidingConfig, adaptiveConfig, stabilityConfig, progressiveConfig );
	}
	if( name == "mlt_rasterizer" ) {
		MLTDefaults d;
		return SetMLTRasterizer(
			d.maxEyeDepth, d.maxLightDepth,
			d.nBootstrap, d.nChains, d.nMutationsPerPixel, d.largeStepProb,
			shader.c_str(), d.showLuminaires,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			pixelFilterConfig, stabilityConfig );
	}
	if( name == "mlt_spectral_rasterizer" ) {
		MLTSpectralDefaults d;
		return SetMLTSpectralRasterizer(
			d.maxEyeDepth, d.maxLightDepth,
			d.nBootstrap, d.nChains, d.nMutationsPerPixel, d.largeStepProb,
			shader.c_str(), d.showLuminaires, spectralConfig,
			d.oidnDenoise, d.oidnQuality, d.oidnDevice, d.oidnPrefilter,
			pixelFilterConfig, stabilityConfig );
	}

	// Unknown name (not in the standard set).
	return false;
}
