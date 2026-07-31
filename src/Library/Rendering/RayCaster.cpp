//////////////////////////////////////////////////////////////////////
//
//  RayCaster.cpp - Implementation of the RayCaster class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../Interfaces/IContinuationClosure.h"
#include <atomic>
#include <cstring>   // review-p2d: std::strcmp for the reserved "environment" solo name
#include "RayCaster.h"
#include "LuminaryManager.h"
#include "EnvironmentSampler.h"
#include "AOVBuffers.h"
#include "../Lights/LightSampler.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/MediumTracking.h"
#include "../Utilities/MediumTransport.h"
#include "../Utilities/IORStackSeeding.h"
#include "../Utilities/IndependentSampler.h"
#include "../Utilities/PathGuidingField.h"
#include "../Utilities/PathTransportUtilities.h"
#include "../Utilities/EquiangularSampler.h"
#include "../Utilities/OptimalMISAccumulator.h"
#include "../Utilities/MISWeights.h"
#include "../Utilities/Optics.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IGeometry.h"
#include "../Materials/NullBoundaryMaterial.h"
#include "../Shaders/SSS/SSSContainment.h"
#include "../Scene.h"					// concrete Scene for the light-generation read (#2b(a))

#define ENABLE_MAX_RECURSION

//#define ENABLE_TERMINATION_MESSAGES

//
// Unbiased Russian Roulette in the RayCaster.  When rs.importance
// drops below this threshold, a proportional survival test fires
// before the intersection/shading work.  Survivors have the
// returned radiance scaled by 1/pSurvive to compensate for the
// killed paths, maintaining an unbiased estimator.
//
// This is a secondary safety net; the primary RR lives in
// PathTracingShaderOp.  The shader-level RR handles the common
// case (throughput < 1 after a few bounces).  This catches
// extreme low-importance rays that survive the shader RR.
//
#define ENABLE_RAYCASTER_RR
static const RISE::Scalar RC_RR_THRESHOLD = 0.01;

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	inline Scalar RayCasterRRSurvivalProbability( const Scalar importance )
	{
		return importance < RC_RR_THRESHOLD && importance > 0.0 ?
			importance / RC_RR_THRESHOLD : 1.0;
	}

	// Shader-dispatch renderers enter path tracing through RayCaster rather
	// than PathTracingIntegrator::IntegrateRay. Capture the raw camera
	// intersection here, before medium sampling, transparency recursion,
	// alpha continuation, x-ray resolution, or shader dispatch can replace it.
	// PixelAOV is per camera sample; the first cast wins and all recursive casts
	// observe primaryDepthCaptured=true.
	inline void CapturePrimaryAOV(
		const RuntimeContext& rc,
		const RayIntersection& ri )
	{
		if( !rc.pAOV || rc.pAOV->primaryDepthCaptured ) return;
		if( IsExactNullBoundaryMaterial( ri.pMaterial ) ) return;
		rc.pAOV->primaryDepthCaptured = true;
		rc.pAOV->depth = ri.geometric.bHit ? ri.geometric.range : Scalar( 0 );
		if( !ri.geometric.bHit || rc.aovPrefilterMode != OidnPrefilter::Fast ) return;

		// Fast guides describe the same raw surface even if a participating
		// medium scatters before it. Apply the modifier to a copy so the guide
		// normal/albedo match eventual surface shading without perturbing the
		// transport intersection before its normal modifier site.
		RayIntersectionGeometric aovGeom( ri.geometric );
		if( ri.pModifier ) ri.pModifier->Modify( aovGeom );
		rc.pAOV->normal = aovGeom.vNormal;
		rc.pAOV->albedo = rc.pathTracingClayOverride
			? RISEPel( 0.5, 0.5, 0.5 )
			: ( ( ri.pMaterial && ri.pMaterial->GetBSDF() )
				? ri.pMaterial->GetBSDF()->albedo( aovGeom )
				: RISEPel( 1, 1, 1 ) );
		rc.pAOV->valid = true;
	}

	inline void OffsetCapturedPrimaryAOVDepth(
		const RuntimeContext& rc,
		const Scalar distance,
		const bool unresolvedAtEntry
		)
	{
		if( unresolvedAtEntry && rc.pAOV &&
			rc.pAOV->primaryDepthCaptured && rc.pAOV->depth > 0.0 ) {
			rc.pAOV->depth += distance;
		}
	}

	// Analog no-scatter survival weight (mirrors PathTracingIntegrator's
	// PTSurvivalWeight).  SampleDistance{,NM} is an ANALOG estimator: reaching
	// the surface / escaping WITHOUT a scatter event is a stochastic SURVIVAL
	// outcome whose probability already carries the Beer-Lambert factor.
	// Multiplying throughput by the full per-channel Tr again would double-count
	// attenuation (a pure absorber would render exp(-2*sigma_a*d), ~2x too
	// thick).  The correct weight is Tr / pSurvival, where pSurvival is the
	// DETERMINISTIC no-scatter survival pdf from
	// IMedium::EvalDistancePdf( ray, dist, /*scattered=*/false, dist ).
	//
	// For a HomogeneousMedium this is byte-identical to the previous
	// MinValue(Tr) form: EvalDistancePdf(false) returns exp(-sigma_t_max*dist) =
	// MinValue(EvalTransmittance).  For a HeterogeneousMedium, EvalTransmittance
	// is a STOCHASTIC ratio-tracking estimate, so MinValue(EvalTransmittance)
	// would be a random denominator (biased ratio-of-random-estimates);
	// HeterogeneousMedium overrides EvalDistancePdf with a deterministic Simpson
	// optical depth, so Tr / pSurvival is the correct unbiased weight there.
	// A non-positive survival pdf means "no attenuation to apply" -> identity.
	inline RISE::RISEPel RayCasterSurvivalWeight(
		const RISE::RISEPel& Tr, const RISE::Scalar pSurvival )
	{
		if( pSurvival > 0 ) {
			return Tr * ( RISE::Scalar( 1 ) / pSurvival );
		}
		return RISE::RISEPel( 1, 1, 1 );
	}

	inline RISE::Scalar FullSegmentAdditiveEmissionNM(
		const RISE::IMedium& medium,
		const RISE::Ray& ray,
		const RISE::Scalar segmentStart,
		const RISE::Scalar segmentEnd,
		const RISE::Scalar nm,
		const RISE::Scalar uniformXi )
	{
		const RISE::Scalar segmentLength = segmentEnd - segmentStart;
		if( segmentLength <= 0.0 ) return 0.0;
		if( medium.IsHomogeneous() ) {
			const RISE::MediumCoefficientsNM coeff = medium.GetCoefficientsNM( ray.origin, nm );
			if( coeff.emission == 0.0 ) return 0.0;
			if( coeff.sigma_t > 0.0 ) {
				const RISE::Scalar transmittanceToStart = exp( -coeff.sigma_t * segmentStart );
				return transmittanceToStart * coeff.emission *
					( -expm1( -coeff.sigma_t * segmentLength ) ) / coeff.sigma_t;
			}
			return coeff.emission * segmentLength;
		}

		const RISE::Scalar t = segmentStart + uniformXi * segmentLength;
		const RISE::MediumCoefficientsNM coeff = medium.GetCoefficientsNM(
			ray.PointAtLength( t ), nm );
		if( coeff.emission == 0.0 ) return 0.0;
		const RISE::Scalar logTr = medium.EvalLogDistancePdfNM(
			ray, t, false, t, nm );
		return segmentLength * exp( logTr ) * coeff.emission;
	}

	inline bool MediumSegmentInterval(
		const RISE::IMedium& medium,
		const RISE::Ray& ray,
		const RISE::Scalar maxDist,
		RISE::Scalar& segmentStart,
		RISE::Scalar& segmentEnd )
	{
		RISE::Point3 bbMin;
		RISE::Point3 bbMax;
		if( !medium.GetBoundingBox( bbMin, bbMax ) ) {
			segmentStart = 0.0;
			segmentEnd = maxDist;
			return segmentEnd > segmentStart;
		}

		segmentStart = 0.0;
		segmentEnd = maxDist;
		for( unsigned int axis = 0; axis < 3; ++axis ) {
			const RISE::Scalar origin = ray.origin[axis];
			const RISE::Scalar direction = ray.Dir()[axis];
			if( fabs( direction ) <= 1e-20 ) {
				if( origin < bbMin[axis] || origin > bbMax[axis] ) return false;
				continue;
			}
			const RISE::Scalar inverseDirection = 1.0 / direction;
			RISE::Scalar t0 = (bbMin[axis] - origin) * inverseDirection;
			RISE::Scalar t1 = (bbMax[axis] - origin) * inverseDirection;
			if( t0 > t1 ) {
				const RISE::Scalar temporary = t0;
				t0 = t1;
				t1 = temporary;
			}
			segmentStart = fmax( segmentStart, t0 );
			segmentEnd = fmin( segmentEnd, t1 );
			if( segmentStart >= segmentEnd ) return false;
		}
		return segmentEnd > segmentStart;
	}
}

RayCaster::RayCaster(
	const bool seeRadianceMap,
	const unsigned int maxR,
	const IShader& pDefaultShader_,
	const bool showLuminaires
	) :
  pScene( 0 ),
  pDefaultShader( pDefaultShader_ ),
  pLuminaryManager( 0 ),
  pLightSampler( 0 ),
  pLumSampling( 0 ),
  bConsiderRMapAsBackground( seeRadianceMap ),
  nMaxRecursions( maxR ),
  bShowLuminaires( showLuminaires ),
  dPendingLightRRThreshold( 0 ),
  bPendingUseLightBVH( false ),
  iPendingRISCandidates( -1 ),
  builtLightGeneration( 0 ),
  bTransparentShadows( false ),
  dRadianceScaleOverride( -1.0 ),		// negative = no override (use the map's own scale)
  bWantsWireEdgeInfo( false ),
  bXrayViewResolve( false ),
	bFirePelDiagnosticEmitted( false ),
  iPendingSoloKind( 0 ),
  pendingSoloLight( 0 ),
  pendingSoloLuminary( 0 )
{
	pDefaultShader.addref();
}

const IShader& RayCaster::SelectShader( const RayIntersection& ri ) const
{
	return ri.pShader ? *ri.pShader : pDefaultShader;
}

RayCaster::~RayCaster( )
{
	safe_release( pScene );

	pDefaultShader.release();

	safe_release( pLumSampling );
	safe_release( pLightSampler );
	safe_release( pLuminaryManager );
}

namespace {
	// Diagnostic counter; see RayCaster::GetSamplerRebuildCount in the
	// header.  Single-threaded — AttachScene runs at the pre-parallel
	// scene-setup seam, never from inside the rasterize.
	// Diagnostic counter (atomic: AttachScene normally runs single-threaded
	// at the scene-setup seam, but two casters/documents could attach
	// concurrently -- relaxed atomics keep the count race-free). (P2c)
	std::atomic<unsigned int> s_samplerRebuildCount{ 0 };

	// Read the concrete Scene's light/structure generation (#2b(a)).  The
	// IScene/IScenePriv abstract interface deliberately does NOT carry this
	// (adding a virtual there would break new-caller -> old-implementation
	// vtable ABI — see abi-preserving-api-evolution).  We downcast to the
	// concrete Scene here at the one call site instead.  An out-of-tree
	// IScene that isn't a RISE::Implementation::Scene yields 0 (a constant),
	// so AttachScene's `liveGen != builtLightGeneration` check is never
	// satisfied for it after the first build — i.e. such a scene keeps the
	// exact pre-#2b(a) same-pointer fast-path behaviour (no regression).
	unsigned int SceneLightGeneration( const RISE::IScene* pScene )
	{
		const RISE::Implementation::Scene* concrete =
			dynamic_cast<const RISE::Implementation::Scene*>( pScene );
		return concrete ? concrete->GetLightTopologyGeneration() : 0u;
	}

	// Realize-pass dispatch: calls obj.Realize() on each world-visible
	// object.  Object::Realize() bakes its (deferred) geometry; CSGObject::
	// Realize() cascades into its world-invisible, un-enumerated operands.
	// Realize() is const + idempotent; for cheap geometries it is a no-op,
	// for a DisplacedGeometry it tessellates + bakes its mesh (and cascades
	// to its base).  This is the Phase-1 root set — objects -> geometry ->
	// (cascaded) base.  Runs single-threaded inside AttachScene before the
	// parallel rasterize, the seam at which the scene becomes immutable.
	class RealizeGeometryDispatch : public RISE::IEnumCallback<RISE::IObject>
	{
	public:
		bool operator()( const RISE::IObject& obj )
		{
			obj.Realize();
			return true;
		}
	};
}

void RayCaster::AttachScene( const IScene* pScene_ )
{
	// ----------------------------------------------------------------
	// REALIZE PASS (Phase 1, 2026-06-13).  Single-threaded materialize of
	// every render-reachable geometry's deferred build work BEFORE the
	// luminary / light-sampler setup, the spatial-structure build, and the
	// parallel rasterize.  Root set = the object manager's objects -> their
	// geometry (which cascades to any displaced base).  The scene is
	// immutable during the parallel pass, so this is the correct (and only
	// safe) place to bake — NOT lazily on the const ray-intersect hot path.
	//
	// This runs on EVERY AttachScene call, INCLUDING the same-scene-pointer
	// re-attach below that early-returns.  Reason: an interactive editor can
	// swap a fresh (unrealized) geometry onto an object and re-render the
	// SAME scene pointer; the early-return would otherwise skip realizing it,
	// and the subsequent PrepareForRendering() TLAS build would query an
	// empty bounding box.  Realize() is idempotent — already-baked
	// geometries no-op — so the repeated walk is cheap.
	if( pScene_ ) {
		const IObjectManager* pObjMan = pScene_->GetObjects();
		if( pObjMan ) {
			RealizeGeometryDispatch realizeDispatch;
			pObjMan->EnumerateObjects( realizeDispatch );
		}
	}

	// Same Scene pointer: the contents may still have changed IN PLACE
	// (a restore or an in-place light edit on the live scene — see
	// Scene::BumpLightTopologyGeneration / RestoreFromSnapshot).  Compare
	// the live light/structure generation against the one our cached
	// samplers were built with.  Unchanged -> O(1) fast path (so a
	// production render that re-attaches every pass pays nothing).
	// Advanced -> rebuild ONLY the light samplers (the realize pass above
	// + the caller's PrepareForRendering already refresh geometry/TLAS);
	// nothing else on the caster needs to change.
	if( pScene == pScene_ ) {
		if( pScene ) {
			const unsigned int liveGen = SceneLightGeneration( pScene );
			if( liveGen != builtLightGeneration ) {
				RebuildLightSamplers();
				builtLightGeneration = liveGen;
			}
		}
		return;
	}

	if( pScene_ ) {
		safe_release( pScene );

		pScene = pScene_;
		pScene->addref();

		RebuildLightSamplers();
		builtLightGeneration = SceneLightGeneration( pScene );
	}
}


// Rebuild the cached LuminaryManager / LightSampler / EnvironmentSampler
// from the currently-attached `pScene`.  Extracted from AttachScene so the
// first-attach (new scene pointer) and the same-pointer-generation-advanced
// rebuild paths share ONE implementation and cannot drift.  `pScene` must
// be non-null and already set by the caller.
void RayCaster::RebuildLightSamplers()
{
	s_samplerRebuildCount.fetch_add( 1, std::memory_order_relaxed );

	safe_release( pLuminaryManager );

	LuminaryManager* pConcreteLumMgr = new LuminaryManager();
	pLuminaryManager = pConcreteLumMgr;
	GlobalLog()->PrintNew( pLuminaryManager, __FILE__, __LINE__, "luminary manager" );
	pLuminaryManager->AttachScene( pScene );

	if( pLumSampling ) {
		pLuminaryManager->SetLuminaireSampling( pLumSampling );
	}

	// Create and prepare the unified light sampler
	safe_release( pLightSampler );
	pLightSampler = new LightSampler();
	GlobalLog()->PrintNew( pLightSampler, __FILE__, __LINE__, "light sampler" );

	// Apply pending settings before Prepare() which builds
	// internal data structures that depend on them.
	if( bPendingUseLightBVH )
	{
		pLightSampler->SetUseLightBVH( true );
	}

	pLightSampler->Prepare( *pScene, pConcreteLumMgr->getLuminaries() );

	// Apply any pending light-sample RR threshold
	if( dPendingLightRRThreshold > 0 )
	{
		pLightSampler->SetLightSampleRRThreshold( dPendingLightRRThreshold );
	}

	// Re-apply any previously-set RIS candidate count so a same-pointer
	// rebuild doesn't silently drop it (the fresh LightSampler defaults to 0).
	if( iPendingRISCandidates >= 0 )
	{
		pLightSampler->SetRISCandidates( (unsigned int)iPendingRISCandidates );
	}

	// Re-apply any previously-set light-solo target so a same-pointer
	// rebuild doesn't silently drop it (the fresh LightSampler defaults to
	// no solo).  Identity pointers (pendingSoloLight / pendingSoloLuminary)
	// are stable across the rebuild -- they name scene-owned objects, not
	// anything the LightSampler itself allocates.
	if( iPendingSoloKind == 1 )
	{
		pLightSampler->SetSoloLight( pendingSoloLight );
	}
	else if( iPendingSoloKind == 2 )
	{
		pLightSampler->SetSoloLuminary( pendingSoloLuminary );
	}
	else if( iPendingSoloKind == 3 )
	{
		pLightSampler->SetSoloEnvironment();
	}

	// Build environment importance sampler if a global radiance map exists
	const IRadianceMap* pEnvMap = pScene->GetGlobalRadianceMap();
	if( pEnvMap )
	{
		// A `> modify rasterizer radiance_scale` override (set via
		// Job::SetActiveRasterizerRadianceScale -> SetRadianceScale)
		// takes precedence over the map's own scale.  Negative means
		// "no override".  The same override is also pushed into the
		// radiance map (the direct-view background), keeping NEE and the
		// background in sync; a direct SetRadianceScale() that skips that
		// dual-write would drive only NEE — but we resolve from the
		// member to keep this caster the authoritative source for the
		// NEE (environment-sampler) scale regardless of attach order.
		const Scalar dEnvScale =
			( dRadianceScaleOverride >= 0.0 ) ? dRadianceScaleOverride : pEnvMap->GetScale();

		EnvironmentSampler* pEnvSampler = new EnvironmentSampler(
			pEnvMap->GetPainter(),
			dEnvScale,
			pEnvMap->GetTransform(),
			64
			);
		GlobalLog()->PrintNew( pEnvSampler, __FILE__, __LINE__, "environment sampler" );
		pEnvSampler->Build();

		if( pEnvSampler->IsValid() )
		{
			pLightSampler->SetEnvironmentSampler( pEnvMap, pEnvSampler );
			GlobalLog()->PrintEasyEvent( "Environment importance sampler built successfully" );
		}
		else
		{
			GlobalLog()->PrintEasyWarning( "Environment map is black, importance sampling disabled" );
		}

		// LightSampler::SetEnvironmentSampler addrefs if valid, so
		// release our local reference.
		safe_release( pEnvSampler );
	}
}

unsigned int RayCaster::GetSamplerRebuildCount() { return s_samplerRebuildCount.load( std::memory_order_relaxed ); }
void         RayCaster::ResetSamplerRebuildCount() { s_samplerRebuildCount.store( 0, std::memory_order_relaxed ); }

void RayCaster::ResolveXrayView_( RayIntersection& ri ) const
{
	// Original primary ray + origin, needed only if at least one skip
	// happens (see the total-distance recompute below).
	const Ray originalRay = ri.geometric.ray;
	const Point3 originalOrigin = originalRay.origin;
	bool bAnySkip = false;

	// Adaptive skip epsilon -- FP-representability-based, NOT object- or
	// camera-scaled (P1 fix).  The minimum offset that reliably escapes
	// self-intersection at a point is a small multiple of the FP ulp AT
	// THAT POINT: double precision only represents ~15-17 significant
	// decimal digits, so the smallest meaningful step away from
	// `ptIntersection` is proportional to the magnitude of its largest
	// component, not to any object or camera measurement.
	//
	// External review round 4, item 1 (P1): the first cut of this fix used
	// max-abs-COMPONENT (any of x/y/z, regardless of the ray's direction),
	// which is TRANSVERSE-coordinate-coupled -- a hit point with a huge
	// coordinate on an axis the ray barely travels along (e.g. an object
	// sitting at world X=1e12, hit by a ray travelling along Y or Z) still
	// picks up a nudge sized off that huge transverse X value, even though
	// escaping self-intersection along the ray's OWN direction needs almost
	// nothing.  At (1e12,0,0) with dir (0,1,0), the old formula advances
	// ~1.4e-2 along Y and skips clean over an opaque layer sitting a mere
	// 1e-3 behind the glass.  The representability bound only needs to
	// cover ulp error ALONG THE RAY -- so weight each axis's contribution
	// by how much the ray actually moves along it:
	//
	//   eps0 = max( 1e-12, kUlpFactor * ( |P.x|*|dir.x| + |P.y|*|dir.y| + |P.z|*|dir.z| ) )
	//   kUlpFactor = 64 * DBL_EPSILON ~= 1.4e-14
	//
	// A large TRANSVERSE coordinate (orthogonal to travel) now contributes
	// ~0 to the nudge, exactly matching how little ulp error a step along
	// the ray actually needs to clear.  The cases where intersection
	// arithmetic suffers catastrophic cancellation from that large
	// transverse coordinate (t-error ~ ulp(bigCoord), independent of the
	// nudge direction) are NOT this formula's job to cover -- they are
	// absorbed by the degenerate-re-hit retry-doubling below, whose budget
	// (`kMaxRetries`) was raised from 12 to 40 for exactly this division of
	// labor: this seed handles the common (ray-aligned ulp) case exactly,
	// the doubling ladder absorbs the rarer cancellation-dominated cases by
	// growing past whatever the transverse cancellation actually needs.
	//
	// `curEps` is the nudge actually used for the pending step; it is
	// recomputed fresh at the start of every NEW step and only carried
	// over (doubled) across loop iterations that are retries of the SAME
	// step -- see the degenerate-rehit check below.
	Scalar curEps = 0;
	bool bRetryStep = false;

	// Real skips (successful nudge-and-rehit onto a DIFFERENT surface) and
	// retries (doubling curEps to escape a degenerate self-hit on the SAME
	// surface) are bounded SEPARATELY.  Previously both consumed the same
	// 16-iteration budget, so a glass stack that needed a couple of
	// retries per pane could exhaust the budget before reaching the
	// opaque backstop.  `kMaxRetries` = 40 (raised from 12 in review round
	// 4 -- see the eps0 derivation above) lets curEps double up to
	// ~2^40 ~= 1e12x from its ulp-scale start, comfortably covering the
	// transverse-cancellation range a large-coordinate hit point can carry
	// (each retry is one cheap intersection test, and the cap only binds
	// in pathological scenes), while `kMaxSkips` stays at the original 16.
	constexpr int kMaxSkips = 16;
	constexpr int kMaxRetries = 40;
	constexpr Scalar kUlpFactor = 64.0 * 2.2204460492503131e-16; // 64 * DBL_EPSILON
	int skip = 0;
	int retry = 0;

	// Recovers the TRUE surface facing from a geometric normal that may
	// have been oriented to oppose the incoming ray (double-sided triangle
	// meshes -- see RayIntersectionGeometric::bGeomNormalOrientedToRay's
	// doc comment).  A raw `Dot(vGeomNormal, dir)` on such a hit is always
	// negative on BOTH a true entry and a true exit of a double-sided
	// surface, which collapses the facing test below to the same sign on
	// both -- exactly the case this facing test exists to distinguish.
	// Un-flip via the recorded per-hit flag before dotting; geometries
	// that never flip (the flag stays false) get the raw dot back
	// unchanged.
	auto trueGeomFacing = []( const RayIntersectionGeometric& g, const Vector3& d ) -> Scalar
	{
		const Scalar raw = Vector3Ops::Dot( g.vGeomNormal, d );
		return g.bGeomNormalOrientedToRay ? -raw : raw;
	};

	while( skip < kMaxSkips )
	{
		if( !ri.geometric.bHit || !ri.pMaterial || !ri.pMaterial->CouldLightPassThrough() )
		{
			break;
		}

		if( !pScene || !pScene->GetObjects() )
		{
			break;
		}

		const Vector3 dir = ri.geometric.ray.Dir();

		// Facing of the face we are TRYING TO LEAVE, captured before the
		// continuation cast below -- see the degenerate-self-hit predicate
		// further down (external review round 6, item 1; recovered via
		// `trueGeomFacing` since round 7, item 1, to see past the double-
		// sided orient-to-ray flip -- see that helper's comment above).
		// `ri` does not change across retries of the same step, so this is
		// stable for the whole step; recomputing it each iteration is cheap
		// and keeps the value tied to whichever `ri` is currently live
		// without a stale carry-over risk.
		const Scalar prevFacing = trueGeomFacing( ri.geometric, dir );

		if( !bRetryStep )
		{
			// Nudge off the surface along the SAME direction (no bending).
			// Direction-weighted representability -- see the eps0 derivation
			// above: each axis's contribution to the ulp bound is scaled by
			// how much the ray actually travels along it, so a large
			// TRANSVERSE coordinate (one the ray barely moves along) does
			// NOT inflate the nudge.
			const Point3& p = ri.geometric.ptIntersection;
			const Scalar dirWeightedAbs =
				std::fabs( p.x ) * std::fabs( dir.x ) +
				std::fabs( p.y ) * std::fabs( dir.y ) +
				std::fabs( p.z ) * std::fabs( dir.z );
			// Floor 1e-9, NOT SURFACE_INTERSEC_ERROR (1e-12): Object::
			// IntersectRay publishes hit points backed off by
			// SURFACE_INTERSEC_ERROR in OBJECT-LOCAL units, so a
			// local->world stretch along the ray AMPLIFIES the published
			// point's standoff from the physical surface (10x scale ->
			// ~1e-11 world gap; external review r5).  A 1e-9 floor clears
			// the standoff outright for scales up to ~1e3; larger
			// stretches are caught by the widened degenerate-re-hit
			// window below and the retry-doubling ladder.
			curEps = std::max( Scalar(1e-9), kUlpFactor * dirWeightedAbs );
		}
		bRetryStep = false;

		const Point3 origin = Point3Ops::mkPoint3( ri.geometric.ptIntersection, dir * curEps );

		RayIntersection next( Ray( origin, dir ), ri.geometric.rast );
		next.geometric.PropagateCastInputs( ri.geometric );   // wireframe edge requests etc. survive the skip
		// Screen-space differentials (Landing 2, Igehy 1999) -- external
		// review P2: a straight-line skip doesn't bend the ray, so the
		// auxiliary-ray DIRECTION offsets (rxDir/ryDir) carry over exactly,
		// but the ORIGIN offsets do NOT -- they are ABSOLUTE offsets from
		// the CENTRAL ray's own origin (RayDifferentials.h), and the
		// central ray's origin just moved by `t` (the full distance from
		// ri's ray origin to this nudge point, i.e. its hit range plus the
		// nudge itself -- NOT just curEps).  First-order transfer along a
		// straight segment (Igehy 1999 Section 3.1, the same formula PBRT
		// uses to propagate a RayDifferential through free space): the
		// auxiliary ray advances by `t` along ITS OWN (offset) direction,
		// so re-expressed relative to the new central origin,
		// rxOrigin_new = rxOrigin_old + t * rxDir_old (ditto for ry); the
		// direction offsets themselves are unchanged by a pure translation.
		// A verbatim copy (the previous code here) is only correct when
		// `t` is negligible -- true for a specular bounce's own nudge, but
		// NOT here, where `t` includes the full hit range, which can be
		// arbitrarily large.  Without this, textured/normal-mapped geometry
		// resolved behind x-ray glass would either fall back to
		// hasDifferentials=false (no transfer at all) or -- worse -- carry
		// a stale, wrong footprint silently.
		next.geometric.ray.hasDifferentials = ri.geometric.ray.hasDifferentials;
		if( next.geometric.ray.hasDifferentials )
		{
			const Scalar t = ri.geometric.range + curEps;
			next.geometric.ray.diffs.rxOrigin =
				ri.geometric.ray.diffs.rxOrigin + ri.geometric.ray.diffs.rxDir * t;
			next.geometric.ray.diffs.ryOrigin =
				ri.geometric.ray.diffs.ryOrigin + ri.geometric.ray.diffs.ryDir * t;
			next.geometric.ray.diffs.rxDir = ri.geometric.ray.diffs.rxDir;
			next.geometric.ray.diffs.ryDir = ri.geometric.ray.diffs.ryDir;
		}
		pScene->GetObjects()->IntersectRay( next, /*bHitFrontFaces*/true, /*bHitBackFaces*/true, /*bComputeExitInfo*/false );

		if( !next.geometric.bHit )
		{
			break;   // keep `ri` -- the last transmissive hit, an honest answer over a black hole
		}

		// Degenerate re-hit: the continuation ray landed back on the SAME
		// object at a range shorter than the nudge that was supposed to
		// clear it -- a grazing-angle / coplanar epsilon straddle, not a
		// genuine second surface.  Double the nudge and retry the SAME
		// step (same `ri`) rather than accepting the spurious self-hit.
		// Retries are bounded by their OWN budget (kMaxRetries), separate
		// from the real-skip budget (kMaxSkips) -- see the derivation
		// above -- so exhausting it here just falls through to accept the
		// current `ri` as the honest answer, same as the black-hole break
		// above.
		// Degenerate self-re-hit window: the published-point standoff is
		// SURFACE_INTERSEC_ERROR in OBJECT-LOCAL units, amplified by the
		// object's local->world stretch along the ray -- which this caster
		// cannot see.  So a same-object re-hit can legitimately appear at
		// range >> curEps and still be the SAME face (external review r5:
		// 10x-stretched glass box -> ~1e-11 re-hit vs 1e-12 eps, accepted
		// as a "real" skip and looping to the cap).  Widen generously --
		// BUT a wide range window alone cannot distinguish "re-hit the same
		// face we tried to leave" from "hit the object's own genuine exit
		// face, which happens to sit inside that same window" (external
		// review round 6, item 1): a shell/box THINNER than the window
		// (e.g. depth ~1e-7, well under the 1e-6 floor) puts its real exit
		// face inside `selfHitWindow` too.  Treating that exit as a
		// self-hit and retrying re-anchors the walk at the ENTRY face with
		// a DOUBLED eps -- which can leap over both the genuine exit AND an
		// opaque layer sitting just behind it, silently resolving to
		// whatever is further back (or nothing).
		//
		// Facing test: a true re-hit of the face we tried to leave faces
		// the SAME way we left it (the nudge landed back on its own front,
		// or bounced straight back off it) -- `next`'s geometric normal and
		// `ri`'s (captured in `prevFacing` above, before this cast) both
		// have a NON-ZERO-magnitude dot with `dir` of the SAME SIGN.  A
		// genuine exit face of a thin shell faces the OPPOSITE way (the ray
		// is leaving through the far side, so its outward normal points
		// backward along `dir` relative to how the entry face's normal
		// did) -- opposite sign.  So only classify as a degenerate self-hit
		// when the signs agree; an opposite-signed near hit is a real exit
		// face and falls through to the normal acceptance path below,
		// re-anchoring the walk there (a SMALL nudge from the exit next
		// step) instead of leaping from the entry with a doubled eps.
		//
		// (a) Double-sided triangle meshes DO orient `vGeomNormal` toward
		// the incoming ray on every face -- entry and exit alike -- which
		// would make the raw dot-product facing test collapse to the same
		// sign on both (external review round 7, item 1: this was the r6
		// degradation case, not a hypothetical).  It is now handled
		// losslessly: those geometries set `bGeomNormalOrientedToRay` at
		// the same site they flip `vGeomNormal`, and `trueGeomFacing`
		// above un-flips before dotting, so `prevFacing` / `nextFacing`
		// carry the REAL surface facing regardless of the orient-to-ray
		// convention.  The only remaining degradation case is a
		// hypothetical geometry that flips its geometric normal to face
		// the ray WITHOUT setting the flag -- that (undetectable from
		// here) case still degrades to the OLD range-only retry behavior,
		// same as round 5's fallback and never worse.
		//
		// (b) Correction to the round-5 note this replaces: that note
		// argued misclassifying a genuinely-close SECOND surface as a
		// self-hit "costs nothing (the walk would skip it anyway)".  That
		// is only true when the retry's doubled-eps nudge still lands
		// short of whatever comes next.  For a shell/box thinner than the
		// nudge, it is NOT free -- the retry re-anchors the origin at the
		// ENTRY face (not the exit), and the doubled eps can carry the
		// walk clean past the exit face and past a thin opaque layer
		// immediately behind it, so the false positive silently discards a
		// real surface instead of paying for one extra intersection test.
		// The facing test above closes exactly that gap.
		const Scalar selfHitWindow = std::max( Scalar(1e-6), curEps * Scalar(16.0) );
		const Scalar nextFacing = trueGeomFacing( next.geometric, dir );
		const bool bDegenerateSelfHit =
			next.pObject == ri.pObject &&
			next.geometric.range < selfHitWindow &&
			( prevFacing * nextFacing ) > 0;
		if( bDegenerateSelfHit )
		{
			if( retry < kMaxRetries )
			{
				curEps *= 2.0;
				++retry;
				bRetryStep = true;
				continue;
			}
			break;
		}

		bAnySkip = true;
		ri = next;
		++skip;
		retry = 0;   // fresh retry budget for the NEXT step
	}

	if( bAnySkip )
	{
		// Depth (and any other range-reading consumer) must see the TOTAL
		// distance from the ORIGINAL ray's origin to the resolved hit --
		// not the resolved hit's own last-segment range, and not a sum of
		// per-segment ranges.  Restoring the original ray here means every
		// downstream consumer (shader, depth accumulator, etc.) gets this
		// for free with no x-ray-specific knowledge.
		ri.geometric.ray = originalRay;
		ri.geometric.range = Point3Ops::Distance( ri.geometric.ptIntersection, originalOrigin );
	}
}

bool RayCaster::CastRay(
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
			) const
{
	IORStack ior_stack( 1.0 );
	if( pLightSampler && pLightSampler->SceneHasNullBoundaries() && pScene ) {
		IORStackSeeding::SeedFromPoint( ior_stack, ray.origin, *pScene );
	}
	return CastRay( rc, rast, ray, c, rs, distance, pRadianceMap, ior_stack );
}

bool RayCaster::CastRay(
			const RuntimeContext& rc,							///< [in] The runtime context
			const RasterizerState& rast,						///< [in] Current state of the rasterizer
			const Ray& ray,										///< [in] Ray to cast
			RISEPel& c,											///< [out] RISEColor for the ray
			const RAY_STATE& rs,								///< [in] The ray state
			Scalar* distance,									///< [in] If there was a hit, how far?
			const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
			const IORStack& ior_stack							///< [in/out] Index of refraction stack
			) const
{
	return CastRayImpl_( rc, rast, ray, c, rs, distance, pRadianceMap,
		ior_stack, false );
}

bool RayCaster::CastRayImpl_(
			const RuntimeContext& rc,
			const RasterizerState& rast,
			const Ray& ray,
			RISEPel& c,
			const RAY_STATE& rs,
			Scalar* distance,
			const IRadianceMap* pRadianceMap,
			const IORStack& ior_stack,
			const bool skipEntryGates
			) const
{
	const bool primaryAOVUnresolvedAtEntry =
		rc.pAOV && !rc.pAOV->primaryDepthCaptured;
	// Fire has no Pel transport until Phase-A step 7.  Diagnose before
	// recursion/RR gates so every RGB entry route fails loudly.
	const IMedium* entryMedium = MediumTracking::GetCurrentMedium( ior_stack, pScene );
	const bool sceneHasFire = pLightSampler && pLightSampler->SceneHasFireMedia();
	if( sceneHasFire || ( entryMedium && entryMedium->IsFireMedium() ) ) {
		bool expected = false;
		if( bFirePelDiagnosticEmitted.compare_exchange_strong( expected, true ) ) {
			GlobalLog()->PrintEasyError(
				"RayCaster::CastRay:: fire media require spectral rendering until the Phase-A Pel preview lands" );
		}
		c = RISEPel( 0, 0, 0 );
		if( distance ) *distance = 0.0;
		return false;
	}

#ifdef ENABLE_MAX_RECURSION
	if( !skipEntryGates && rs.depth > nMaxRecursions )
	{
#ifdef ENABLE_TERMINATION_MESSAGES
		GlobalLog()->PrintEasyInfo( "FORCED RECURSION TERMINATION" );
#endif

		return false;
	}
#endif

	// Unbiased Russian roulette: decide before the expensive
	// intersection work, compensate the returned radiance after.
	Scalar rrCompensation = 1.0;
#ifdef ENABLE_RAYCASTER_RR
	if( !skipEntryGates && rs.importance < RC_RR_THRESHOLD && rs.importance > 0 )
	{
		const Scalar pSurvive = rs.importance / RC_RR_THRESHOLD;
		if( rc.random.CanonicalRandom() >= pSurvive ) {
			return false;
		}
		rrCompensation = 1.0 / pSurvive;
	}
#endif

	bool bReturn = false;

	// Cast the ray into the scene
	RayIntersection	ri( ray, rast );
	ri.geometric.glossyFilterWidth = rs.glossyFilterWidth;
	ri.geometric.bWantsWireEdgeInfo = bWantsWireEdgeInfo;
	if( skipEntryGates ) ri.geometric.minimumSurfaceRange = 0.0;
	pScene->GetObjects()->IntersectRay( ri, true, true, skipEntryGates );
	CapturePrimaryAOV( rc, ri );

	bool bHit = ri.geometric.bHit;

	if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
		if( ri.pMaterial && !IsExactNullBoundaryMaterial( ri.pMaterial ) &&
			ri.pMaterial->GetEmitter() ) {
			bHit = bShowLuminaires;
		}
	}

	// GUI render modes (docs/gui/RENDER_MODES.md "X-ray axis"): resolve
	// through transmissive surfaces to the first opaque hit BEFORE medium
	// transport / the modifier site / shading, so every consumer past
	// this point (including depth) sees the resolved hit with zero
	// x-ray-specific knowledge.  Production casters never set the flag.
	if( bXrayViewResolve && bHit ) {
		ResolveXrayView_( ri );
		bHit = ri.geometric.bHit;

		// Re-apply the same luminaire-suppression check to the RESOLVED
		// hit: the check above only ran on the ORIGINAL hit, so an
		// emitter sitting behind the glass would otherwise escape
		// eRayView's "hide luminaires" preview setting even though a
		// directly-visible emitter at the same spot would be suppressed.
		if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
			if( ri.pMaterial && !IsExactNullBoundaryMaterial( ri.pMaterial ) &&
				ri.pMaterial->GetEmitter() ) {
				bHit = bShowLuminaires;
			}
		}
	}

	// ----------------------------------------------------------------
	// Medium transport: determine if the ray is traveling through a
	// participating medium and handle absorption/scattering.
	//
	// Resolution order (matching Cycles volume stack):
	//   1. Check innermost object on IORStack's enclosure state for
	//      interior medium
	//   2. Fall back to scene's global medium
	//   3. No medium (vacuum) — skip medium transport entirely
	//
	// When a medium is present:
	//   a. Sample free-flight distance from the medium
	//   b. If scatter event occurs before surface hit:
	//      - NEE at scatter point (in-scattering)
	//      - Sample phase function for continuation direction
	//      - Recursively CastRay from scatter point
	//      - Return (skip surface shading)
	//   c. If surface hit through medium:
	//      - Apply transmittance to surface shading result
	// ----------------------------------------------------------------
	const IObject* pMediumObject = 0;
	const IMedium* pMedium = MediumTracking::GetCurrentMediumWithObject( ior_stack, pScene, pMediumObject );

	// G6: stamp the ambient (incident-medium) IOR so a GGX conductor shaded via
	// the RayCaster shader path sees the surrounding medium's IOR rather than
	// hardcoded air.  Read before SetCurrentObject (which does not push).  Guard
	// to air (1.0).
	{
		const Scalar ambIOR = ior_stack.top();
		ri.geometric.ambientIOR = ( ambIOR > 0.0 ) ? ambIOR : 1.0;
	}

	// Strategy-selection factor for a no-scatter outcome (see the EQ-MIS block
	// below).  Declared in the outer scope because the survival sites that
	// consume it (surface-hit / escape) live outside the if(pMedium) block.
	// 0.5 only in the DT no-scatter branch under equiangular MIS; 1.0 otherwise.
	Scalar noScatterPdfScale = 1.0;
	const VolumeEmissionSegmentState incomingVolumeSegmentState =
		CurrentVolumeEmissionSegmentState();

	if( pMedium )
	{
		const Scalar maxDist = bHit
			? ( IsExactNullBoundaryMaterial( ri.pMaterial )
				? ri.geometric.surfaceRange : ri.geometric.range )
			: RISE_INFINITY;

		IndependentSampler mediumSampler( rc.random );
		bool scattered = false;
		Scalar t_m = 0;

		// ----------------------------------------------------------------
		// Equiangular MIS: one-sample MIS between delta tracking and
		// equiangular sampling toward positional lights.
		//
		// When positional (point/spot) lights exist:
		//   With prob 0.5: delta tracking → t, pdf_dt
		//   With prob 0.5: equiangular toward random light → t, pdf_eq
		//   combined_pdf = 0.5 * pdf_dt + 0.5 * pdf_eq
		//   throughput = Tr * sigma_s / combined_pdf
		//
		// When no positional lights: plain delta tracking (unchanged).
		//
		// Reference: Kulla, Fajardo, "Importance Sampling Techniques
		// for Path Tracing in Participating Media", EGSR 2012.
		// ----------------------------------------------------------------
		const bool useEquiangularMIS = !IsSSSContainmentActive() && pLightSampler &&
			pLightSampler->IsEquiangularPivotDistributionValid() &&
			pLightSampler->GetEquiangularPivotEntryCount() > 0;
		VolumeEmissionPivotState equiangularPivots;
		const bool equiangularPivotsReady = useEquiangularMIS &&
			pLightSampler->ResolveVolumeEmissionPivots(
				mediumSampler, incomingVolumeSegmentState.pivots, equiangularPivots );
		Scalar combinedPdf = 0;		// Deterministic MIS denominator
		bool useExplicitThroughput = false;
		bool equiangularZeroContrib = false;	// True when equiangular strategy samples zero density

		if( equiangularPivotsReady )
		{
			// Equiangular sampling requires an explicitly bounded segment.  A
			// surface hit bounds it directly; otherwise a bounded medium AABB may.
			bool equiangularSegmentBounded = bHit;
			Scalar eqTNear = 0;
			Scalar eqTFar = maxDist;
			{
				Point3 bbMin, bbMax;
				if( pMedium->GetBoundingBox( bbMin, bbMax ) )
				{
					// Intersect ray with medium AABB to find entry/exit
					Scalar tEntry = 0, tExit = maxDist;
					// Use the same slab intersection as MajorantGrid
					const Scalar invX = (fabs(ray.Dir().x) > 1e-20) ? 1.0 / ray.Dir().x : 0;
					const Scalar invY = (fabs(ray.Dir().y) > 1e-20) ? 1.0 / ray.Dir().y : 0;
					const Scalar invZ = (fabs(ray.Dir().z) > 1e-20) ? 1.0 / ray.Dir().z : 0;

					bool aabbHit = true;
					if( invX != 0 ) {
						Scalar t0 = (bbMin.x - ray.origin.x) * invX;
						Scalar t1 = (bbMax.x - ray.origin.x) * invX;
						if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
						tEntry = fmax( tEntry, t0 );
						tExit = fmin( tExit, t1 );
					} else if( ray.origin.x < bbMin.x || ray.origin.x > bbMax.x ) {
						aabbHit = false;
					}
					if( aabbHit && invY != 0 ) {
						Scalar t0 = (bbMin.y - ray.origin.y) * invY;
						Scalar t1 = (bbMax.y - ray.origin.y) * invY;
						if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
						tEntry = fmax( tEntry, t0 );
						tExit = fmin( tExit, t1 );
					} else if( ray.origin.y < bbMin.y || ray.origin.y > bbMax.y ) {
						aabbHit = false;
					}
					if( aabbHit && invZ != 0 ) {
						Scalar t0 = (bbMin.z - ray.origin.z) * invZ;
						Scalar t1 = (bbMax.z - ray.origin.z) * invZ;
						if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
						tEntry = fmax( tEntry, t0 );
						tExit = fmin( tExit, t1 );
					} else if( ray.origin.z < bbMin.z || ray.origin.z > bbMax.z ) {
						aabbHit = false;
					}

					if( aabbHit && tEntry < tExit )
					{
						eqTNear = fmax( 0.0, tEntry );
						eqTFar = fmin( maxDist, tExit );
						equiangularSegmentBounded = true;
					}
				}
			}

			if( !equiangularSegmentBounded || eqTFar <= eqTNear )
			{
				// Medium AABB doesn't intersect ray — fall back to plain delta tracking
				t_m = pMedium->SampleDistance( ray, maxDist, mediumSampler, scattered );
			}
			else
			{
				Point3 selectedPivot;
				Scalar selectedPivotPdf = 0.0;
				const bool selectedPivotOk = pLightSampler->SampleEquiangularPivot(
					equiangularPivots, mediumSampler.Get1D(), selectedPivot,
					selectedPivotPdf );
				const Scalar xiStrategy = selectedPivotOk ? mediumSampler.Get1D() : 0.0;

				if( !selectedPivotOk )
				{
					t_m = pMedium->SampleDistance(
						ray, maxDist, mediumSampler, scattered );
				}
				else if( xiStrategy < 0.5 )
				{
					// Delta tracking strategy
					IMedium::DistanceSample ds = pMedium->SampleDistanceWithPdf(
						ray, maxDist, mediumSampler );
					t_m = ds.t;
					scattered = ds.scattered;

					if( scattered )
					{
						// MIS balance heuristic with deterministic densities.
						// pdf_dt uses majorant transmittance T_bar (via DDA),
						// not stochastic ratio-tracking T_real, so the MIS
						// denominator is deterministic.  See EvalDistancePdf
						// comment for rationale.
						const Scalar pdf_dt = pMedium->EvalDistancePdf(
							ray, t_m, true, maxDist );
						const Scalar pdf_eq = pLightSampler->EquiangularDistancePdf(
							equiangularPivots, ray, eqTNear, eqTFar, true, t_m );

						combinedPdf = 0.5 * pdf_dt + 0.5 * pdf_eq;
						useExplicitThroughput = true;
					}
					else
					{
						// No-scatter outcome under equiangular MIS: reachable only
						// via this delta-tracking strategy (chosen with prob 0.5).
						// The survival sites divide by the extra 0.5.
						noScatterPdfScale = 0.5;
					}
				}
				else
				{
					// Equiangular strategy: sample distance toward selected light.
					// Unlike delta tracking, equiangular ONLY proposes scatter
					// events — it cannot produce a "no scatter" (transmission)
					// result.  When the sample lands at a zero-density point or
					// outside the medium, the contribution is zero and we must
					// NOT fall through to the surface/transmission path.
					EquiangularSampling::Sample eqSample =
						EquiangularSampling::SampleDistance(
							ray, selectedPivot, eqTNear, eqTFar,
							true, mediumSampler.Get1D() );
					t_m = eqSample.t;

					if( t_m > eqTNear && t_m < maxDist )
					{
						const Point3 eqPt = ray.PointAtLength( t_m );
						const MediumCoefficients eqCoeff = pMedium->GetCoefficients( eqPt );

						if( ColorMath::MaxValue( eqCoeff.sigma_t ) > 0 )
						{
							scattered = true;

							const Scalar pdf_dt = pMedium->EvalDistancePdf(
								ray, t_m, true, maxDist );
							const Scalar pdf_eq = pLightSampler->EquiangularDistancePdf(
								equiangularPivots, ray, eqTNear, eqTFar, true, t_m );

							combinedPdf = 0.5 * pdf_dt + 0.5 * pdf_eq;
							useExplicitThroughput = true;
						}
						else
						{
							// Zero density: equiangular scatter proposal contributes
							// zero (sigma_s = 0).  This is a valid zero-weight sample,
							// not a transmission event.
							equiangularZeroContrib = true;
						}
					}
					else
					{
						// Sample outside medium range: zero contribution.
						equiangularZeroContrib = true;
					}
				}
			}
		}
		else
		{
			t_m = pMedium->SampleDistance( ray, maxDist, mediumSampler, scattered );
		}

		// Equiangular zero-contribution: the equiangular strategy
		// sampled a point with no density.  Return zero for this
		// sample — do NOT proceed to surface shading, as this is a
		// scatter-measure sample, not a surface-measure sample.
		if( equiangularZeroContrib )
		{
			if( distance ) *distance = 0;
			if( rrCompensation != 1.0 ) c = c * rrCompensation;
			return false;
		}

		if( scattered )
		{
			// Medium scatter event before surface hit.
			// Compute scatter point and evaluate in-scattering + continuation.
			//
			// Phase function convention: 'wo' is the travel direction of
			// the arriving photon (= ray.Dir()), NOT the toward-viewer
			// direction.  This matches Cycles' convention (which negates
			// sd->wi before passing to HG) and RISE's own BioSpec usage
			// (GenericHumanTissueSPF passes ri.ray.Dir() to SampleWithG).
			// For forward scattering (g > 0), Sample(wo) returns
			// directions close to wo — i.e., the photon continues
			// roughly in its original travel direction.
			const Point3 scatterPt = ray.PointAtLength( t_m );
			const Vector3 wo = ray.Dir();

			const MediumCoefficients coeff = pMedium->GetCoefficients( scatterPt );
			const RISEPel Tr = pMedium->EvalTransmittance( ray, t_m );
			RISEPel throughput( 0, 0, 0 );

			if( useExplicitThroughput && combinedPdf > 0 )
			{
				// MIS throughput: Tr * sigma_s / combined_pdf
				// where combined_pdf = 0.5 * pdf_dt + 0.5 * pdf_eq.
				// Both pdf_dt and pdf_eq are deterministic: pdf_dt uses
				// majorant transmittance T_bar (DDA), pdf_eq is analytic.
				// The stochastic Tr in the numerator is correct — it's
				// the integrand estimate, not a technique density.
				throughput = Tr * coeff.sigma_s * (1.0 / combinedPdf);
			}
			else
			{
				// Original throughput (no MIS): cancel the delta tracking
				// PDF terms analytically.
				//   PDF = sigma_t_max * exp(-sigma_t_majorant * integral)
				//   Transmittance = exp(-sigma_t * t) per channel
				//   Net weight per channel = sigma_s / sigma_t_max
				//     (after canceling the exp terms)
				const Scalar sigma_t_max = ColorMath::MaxValue( coeff.sigma_t );
				if( sigma_t_max > 0 ) {
					const Scalar Tr_scalar = ColorMath::MinValue( Tr );
					if( Tr_scalar > 0 ) {
						throughput = Tr * coeff.sigma_s * (1.0 / (sigma_t_max * Tr_scalar));
					}
				}
			}

			// Retain one collision closure across NEE adapters, continuation,
			// and guiding.  Pel fire resolves to null until the preview step;
			// ordinary media borrow their legacy stateless phase.
			MediumTransport::CollisionPhaseClosure phaseClosure(
				*pMedium, scatterPt, 0.0, false );
			const IPhaseFunction* pPhase = phaseClosure.Get();

			// 1. NEE at scatter point (in-scattering from lights)
			RISEPel Ld = MediumTransport::EvaluateInScattering(
				scatterPt, wo, pMedium, pPhase, *this, pLightSampler,
				mediumSampler, rast, pMediumObject, &ior_stack );

			// 2. Phase-function continuation (indirect in-scattering)
			// Volume bounces are bounded independently of the general
			// depth limit to prevent excessive scattering in dense media.
			static const unsigned int nMaxVolumeBounces = 64;
			RISEPel Li( 0, 0, 0 );
			Scalar phasePdf = 0;
			Vector3 wi( 0, 0, 0 );
			if( pPhase && rs.depth < nMaxRecursions &&
				rs.volumeBounces < nMaxVolumeBounces )
			{
				// Sample the continuation direction — optionally guided
				Scalar guidingMISWeight = 1.0;
				Scalar effectivePdf = 0;
				wi = pPhase->Sample( wo, mediumSampler );
				phasePdf = pPhase->Pdf( wo, wi );
				effectivePdf = phasePdf;

#ifdef RISE_ENABLE_OPENPGL
				// Volume guiding: one-sample MIS between guiding
				// distribution and phase function.
				if( rc.pGuidingField && rc.pGuidingField->IsTrained() &&
					rc.guidingAlpha > 0 &&
					rs.depth < rc.maxGuidingDepth )
				{
					static thread_local Implementation::GuidingVolumeDistributionHandle volGuideHandle;

					const Scalar alpha = rc.guidingAlpha;
					if( rc.pGuidingField->InitVolumeDistribution(
						volGuideHandle, scatterPt, mediumSampler.Get1D() ) )
					{
						// Apply HG product if the phase function is anisotropic
						const Scalar meanCosine = pPhase->GetMeanCosine();
						if( fabs( meanCosine ) > 1e-6 )
						{
							rc.pGuidingField->ApplyHGProduct(
								volGuideHandle, wo, meanCosine );
						}

						const Scalar xiG = mediumSampler.Get1D();
						if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
						{
							// Sample from guiding distribution.
							Scalar guidePdf = 0;
							const Point2 xi2D( mediumSampler.Get1D(), mediumSampler.Get1D() );
							wi = rc.pGuidingField->SampleVolume( volGuideHandle, xi2D, guidePdf );

							if( guidePdf > 0 )
							{
								phasePdf = pPhase->Pdf( wo, wi );
							}
							effectivePdf = PathTransportUtilities::GuidingSelectedMixturePdf(
								alpha, guidePdf, phasePdf, true );
							guidingMISWeight = effectivePdf > 0 ? phasePdf / effectivePdf : 0;
						}
						else
						{
							// Keep phase-sampled direction, but reweight for combined PDF
							const Scalar guidePdf = rc.pGuidingField->PdfVolume( volGuideHandle, wi );
							effectivePdf = PathTransportUtilities::GuidingSelectedMixturePdf(
								alpha, guidePdf, phasePdf, false );
							guidingMISWeight = effectivePdf > 0 ? phasePdf / effectivePdf : 0;
						}
					}
				}
#endif // RISE_ENABLE_OPENPGL

				if( PathTransportUtilities::IsPositiveFiniteDensity( effectivePdf ) )
				{
					const Ray scatterRay( scatterPt, wi );

					RAY_STATE rs2;
					rs2.depth = rs.depth + 1;
					rs2.importance = rs.importance * ColorMath::MaxValue( throughput ) * guidingMISWeight;
					rs2.considerEmission = true;
					rs2.type = rs.type;
					rs2.volumeBounces = rs.volumeBounces + 1;
					rs2.bsdfPdf = effectivePdf;

					Scalar hitDist = 0;
					CastRay( rc, rast, scatterRay, Li, rs2, &hitDist,
						pRadianceMap, ior_stack );

#ifdef RISE_ENABLE_OPENPGL
				// Record volume training sample for the guiding field.
				// Use effectivePdf (= combinedPdf when guiding was applied)
				// so that weight = luminance / pdf matches the actual
				// sampling distribution used to generate the direction.
					if( rc.pGuidingField &&
						rc.pGuidingField->IsCollectingTrainingSamples() &&
						effectivePdf > NEARZERO )
					{
						const Scalar lum = ColorMath::MaxValue( Li );
						if( lum > 0 )
						{
							rc.pGuidingField->AddVolumeSample(
								scatterPt, wi,
								hitDist > 0 ? hitDist : 1.0,
								effectivePdf,
								lum,
								false );
						}
						else
						{
							rc.pGuidingField->AddZeroValueVolumeSample(
								scatterPt, wi );
						}
					}
#endif // RISE_ENABLE_OPENPGL

					Li = Li * guidingMISWeight;
				}
			}

			// Combine: throughput * (Ld + Li) + emission
			c = throughput * (Ld + Li);

			// Volumetric emission contribution along segment [0, t_m].
			// The integral is: Le * integral_0^t Tr(0->s) ds
			//
			// For homogeneous media (constant sigma_t):
			//   = emission * (1 - exp(-sigma_t * t)) / sigma_t
			//
			// For heterogeneous media, sigma_t varies spatially.
			// We use the effective optical depth tau = -ln(Tr) from
			// the ray-marched transmittance and compute:
			//   emission * (1 - Tr) * t / tau
			// This is exact when sigma_t is constant along the segment
			// and a reasonable approximation otherwise.
			if( ColorMath::MaxValue( coeff.emission ) > 0 )
			{
				RISEPel emissionContrib( 0, 0, 0 );
				for( int ch = 0; ch < 3; ch++ )
				{
					if( Tr[ch] < 1.0 - 1e-10 )
					{
						// Use effective optical depth from ray-marched Tr
						const Scalar tau = -log( fmax( Tr[ch], 1e-30 ) );
						emissionContrib[ch] = coeff.emission[ch] *
							(1.0 - Tr[ch]) * t_m / tau;
					}
					else
					{
						// Nearly transparent: emission accumulates linearly
						emissionContrib[ch] = coeff.emission[ch] * t_m;
					}
				}
				c = c + emissionContrib;
			}

			if( distance ) {
				*distance = t_m;
			}

			// Apply RR compensation
			if( rrCompensation != 1.0 ) {
				c = c * rrCompensation;
			}

			return true;
		}
		// else: no scatter — ray passes through to surface or background.
		// Apply transmittance after shading below.
		//
		// Accumulate volumetric emission along the non-scatter segment.
		// Without this, purely absorptive emissive media (sigma_s = 0)
		// would never contribute emission since scatter events never occur.
		if( !scattered )
		{
			// Use midpoint of the segment for coefficient evaluation,
			// which is a better approximation than ray origin for
			// heterogeneous media where density varies spatially.
			const Scalar segDist = bHit ? maxDist : Scalar(1000.0);
			const Point3 midPt = ray.PointAtLength( segDist * 0.5 );
			const MediumCoefficients coeff = pMedium->GetCoefficients( midPt );
			if( ColorMath::MaxValue( coeff.emission ) > 0 )
			{
				// Use ray-marched transmittance for the emission integral
				// to handle heterogeneous extinction correctly.
				const RISEPel Tr_seg = pMedium->EvalTransmittance( ray, segDist );
				RISEPel emissionContrib( 0, 0, 0 );
				for( int ch = 0; ch < 3; ch++ )
				{
					if( Tr_seg[ch] < 1.0 - 1e-10 )
					{
						// Effective optical depth from ray-marched Tr
						const Scalar tau = -log( fmax( Tr_seg[ch], 1e-30 ) );
						emissionContrib[ch] = coeff.emission[ch] *
							(1.0 - Tr_seg[ch]) * segDist / tau;
					}
					else
					{
						emissionContrib[ch] = coeff.emission[ch] * segDist;
					}
				}
				c = c + emissionContrib;
			}
		}
	}

	if( bHit )
	{
		if( IsExactNullBoundaryMaterial( ri.pMaterial ) )
		{
			IORStack nextStack( ior_stack );
			ApplyExactNullBoundaryTransition( ri.pMaterial, ri.pObject, nextStack );
			const Ray nextRay = ContinueExactNullBoundaryRay(
				ray, ri.geometric.surfaceRange );

			RISEPel survival( 1, 1, 1 );
			Scalar noEventProbability = 1.0;
			if( pMedium ) {
				noEventProbability = noScatterPdfScale * pMedium->EvalDistancePdf(
					ray, ri.geometric.surfaceRange, false,
					ri.geometric.surfaceRange );
				survival = RayCasterSurvivalWeight(
					pMedium->EvalTransmittance( ray, ri.geometric.surfaceRange ),
					noEventProbability );
			}
			const VolumeEmissionSegmentState downstreamVolumeSegmentState =
				AdvanceVolumeEmissionSegmentState(
					incomingVolumeSegmentState,noEventProbability,
					ri.geometric.surfaceRange);
			RISEPel downstream( 0, 0, 0 );
			Scalar downstreamDistance = 0;
			bool downstreamHit = false;
			{
				const VolumeEmissionSegmentStateScope volumeStateScope(
					downstreamVolumeSegmentState);
				downstreamHit = CastRayImpl_(
					rc, rast, nextRay, downstream, rs, &downstreamDistance,
					pRadianceMap, nextStack, true );
			}
			OffsetCapturedPrimaryAOVDepth(
				rc, ri.geometric.surfaceRange, primaryAOVUnresolvedAtEntry );
			const RISEPel segmentSource = pMedium ? c : RISEPel( 0, 0, 0 );
			c = segmentSource + survival * downstream;
			if( rrCompensation != 1.0 ) c = c * rrCompensation;

			if( distance ) {
				*distance = downstreamDistance >= RISE_INFINITY
					? RISE_INFINITY
					: ri.geometric.surfaceRange + downstreamDistance;
			}
			return downstreamHit || ColorMath::MaxValue( segmentSource ) != 0.0;
		}

		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Set the current object on the IOR stack
		ior_stack.SetCurrentObject( ri.pObject );

		// Apply shade by calling the appropriate shader
		SelectShader( ri ).Shade( rc, ri, *this, rs, c, ior_stack );

		// Analog no-scatter survival weight (see RayCasterSurvivalWeight):
		// reaching this surface without a scatter event is a survival outcome
		// whose probability already carries Beer-Lambert, so weight by
		// Tr / pSurvival (deterministic no-scatter survival pdf), NOT the full
		// Tr (which would double-count attenuation).
		if( pMedium ) {
			c = c * RayCasterSurvivalWeight(
				pMedium->EvalTransmittance( ray, ri.geometric.range ),
				noScatterPdfScale * pMedium->EvalDistancePdf( ray, ri.geometric.range, false, ri.geometric.range ) );
		}

		if( distance ) {
			*distance = ri.geometric.range;
		}

		bReturn = true;
	} else if( pRadianceMap ) {
		c = pRadianceMap->GetRadiance( ray, rast );

		// Analog no-scatter survival weight for the escape-to-background path
		// (Tr / pSurvival, not full Tr — see above).
		if( pMedium ) {
			c = c * RayCasterSurvivalWeight(
				pMedium->EvalTransmittance( ray, RISE_INFINITY ),
				noScatterPdfScale * pMedium->EvalDistancePdf( ray, RISE_INFINITY, false, RISE_INFINITY ) );
		}
	} else if( pScene->GetGlobalRadianceMap() ) {
		c = pScene->GetGlobalRadianceMap()->GetRadiance( ray, rast );

		// Apply MIS weight for BSDF-sampled environment hit vs env NEE
		if( pLightSampler && rs.bsdfPdf > 0 )
		{
			const EnvironmentSampler* pES = pLightSampler->GetEnvironmentSampler();
			if( pES )
			{
				const Scalar envPdf = pES->Pdf( ray.Dir() );
				if( envPdf > 0 )
				{
					// Optimal MIS training: use full integrand Le * BSDF * cos.
					// bsdfTimesCos carries RGB BSDF*cos from the scatter site;
					// component-wise multiply with Le then scalarize.
					if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
					{
						const Scalar fLum = ColorMath::MaxValue( c * rs.bsdfTimesCos );
						const Scalar f2 = fLum * fLum;
						if( f2 > 0 && rs.bsdfPdf > 0 )
						{
							const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->Accumulate(
								rast.x, rast.y,
								f2, rs.bsdfPdf, kTechniqueBSDF );
						}
					}

					Scalar w_bsdf;
					if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
					{
						const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
						w_bsdf = MISWeights::OptimalMIS2Weight( rs.bsdfPdf, envPdf, alpha );
					}
					else
					{
						w_bsdf = PathTransportUtilities::PowerHeuristic( rs.bsdfPdf, envPdf );
					}
					c = c * w_bsdf;
				}
			}
		}

		// Analog no-scatter survival weight for the escape-to-environment path
		// (Tr / pSurvival, not full Tr — see above).
		if( pMedium ) {
			c = c * RayCasterSurvivalWeight(
				pMedium->EvalTransmittance( ray, RISE_INFINITY ),
				noScatterPdfScale * pMedium->EvalDistancePdf( ray, RISE_INFINITY, false, RISE_INFINITY ) );
		}

		if( distance && bConsiderRMapAsBackground ) {
			*distance = RISE_INFINITY;
		}

		bReturn = bConsiderRMapAsBackground;
	}

	// Apply RR compensation to the returned radiance so the caller's
	// estimator (throughput * c) remains unbiased.
	if( rrCompensation != 1.0 ) {
		c = c * rrCompensation;
	}

	return bReturn;
}

//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
bool RayCaster::CastRayNM( 
	const RuntimeContext& rc,							///< [in] The runtime context
	const RasterizerState& rast,						///< [in] Current state of the rasterizer
	const Ray& ray,										///< [in] Ray to cast
	Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
	const RAY_STATE& rs,								///< [in] The ray state
	const Scalar nm,									///< [in] Wavelength to cast
	Scalar* distance,									///< [in] If there was a hit, how far?
	const IRadianceMap* pRadianceMap					///< [in] Radiance map to use in case there is no hit
	) const
{
	IORStack ior_stack( 1.0 );
	if( pLightSampler && pLightSampler->SceneHasNullBoundaries() && pScene ) {
		IORStackSeeding::SeedFromPoint( ior_stack, ray.origin, *pScene );
	}
	return CastRayNM( rc, rast, ray, c, rs, nm, distance, pRadianceMap, ior_stack );
}

//! Tells the ray caster to cast the specified ray into the scene for the specific wavelength
/// \return TRUE if the cast ray results in an intersection, FALSE otherwise
bool RayCaster::CastRayNM(
    const RuntimeContext& rc,							///< [in] The runtime context
	const RasterizerState& rast,						///< [in] Current state of the rasterizer
	const Ray& ray,										///< [in] Ray to cast
	Scalar& c,											///< [out] Amplitude of spectral function for the given wavelength
	const RAY_STATE& rs,								///< [in] The ray state
	const Scalar nm,									///< [in] Wavelength to cast
	Scalar* distance,									///< [in] If there was a hit, how far?
	const IRadianceMap* pRadianceMap,					///< [in] Radiance map to use in case there is no hit
	const IORStack& ior_stack							///< [in/out] Index of refraction stack
	) const
{
	return CastRayNMImpl_( rc, rast, ray, c, rs, nm, distance,
		pRadianceMap, ior_stack, false );
}

bool RayCaster::CastRayNMImpl_(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& ray,
	Scalar& c,
	const RAY_STATE& rs,
	const Scalar nm,
	Scalar* distance,
	const IRadianceMap* pRadianceMap,
	const IORStack& ior_stack,
	const bool skipEntryGates,
	const bool skipEntryRoulette,
	const bool sourceOnlySegment,
	Scalar* sameSegmentMediumSource
	) const
{
	if( sameSegmentMediumSource ) *sameSegmentMediumSource = 0.0;
	const bool primaryAOVUnresolvedAtEntry =
		rc.pAOV && !rc.pAOV->primaryDepthCaptured;
	bool depthGateDeferredForEmission = sourceOnlySegment;
#ifdef ENABLE_MAX_RECURSION
	if( !skipEntryGates && rs.depth > nMaxRecursions )
	{
		// The current ray may begin in vacuum and cross an exact null boundary
		// into a source-carrying enclosure.  Intersect before terminating so
		// that same-segment enclosure transitions remain observable.
		depthGateDeferredForEmission = true;
	}
#endif

	// Decide continuation roulette before the expensive intersection work,
	// but keep any medium-source score outside that estimator.  A rejected
	// continuation still samples and scores a finite emission event.
	Scalar rrContinuationCompensation = 1.0;
	bool rrContinuationRejected = false;
#ifdef ENABLE_RAYCASTER_RR
	if( !skipEntryGates && !skipEntryRoulette && !sourceOnlySegment &&
		!depthGateDeferredForEmission &&
		rs.importance < RC_RR_THRESHOLD && rs.importance > 0 )
	{
		const Scalar pSurvive = RayCasterRRSurvivalProbability(rs.importance);
		if( rc.random.CanonicalRandom() >= pSurvive ) {
			// As with the depth gate, a source can begin only after an exact
			// null-boundary transition.  Defer the rejected continuation until
			// the first real downstream vertex.
			rrContinuationRejected = true;
		} else {
			rrContinuationCompensation = 1.0 / pSurvive;
		}
	}
#endif

	// Cast the ray into the scene
	RayIntersection	ri( ray, rast );
	ri.geometric.glossyFilterWidth = rs.glossyFilterWidth;
	ri.geometric.bWantsWireEdgeInfo = bWantsWireEdgeInfo;
	if( skipEntryGates ) ri.geometric.minimumSurfaceRange = 0.0;
	pScene->GetObjects()->IntersectRay( ri, true, true, skipEntryGates );
	CapturePrimaryAOV( rc, ri );

	bool bHit = ri.geometric.bHit;

	if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
		if( ri.pMaterial && !IsExactNullBoundaryMaterial( ri.pMaterial ) &&
			ri.pMaterial->GetEmitter() ) {
			bHit = bShowLuminaires;
		}
	}

	// GUI render modes (docs/gui/RENDER_MODES.md "X-ray axis"): see
	// CastRay's identical call site for the rationale.
	if( bXrayViewResolve && bHit ) {
		ResolveXrayView_( ri );
		bHit = ri.geometric.bHit;

		// Re-apply the same luminaire-suppression check to the RESOLVED
		// hit -- see CastRay's identical call site for the rationale.
		if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
			if( ri.pMaterial && !IsExactNullBoundaryMaterial( ri.pMaterial ) &&
				ri.pMaterial->GetEmitter() ) {
				bHit = bShowLuminaires;
			}
		}
	}

	bool bReturn = false;

	// Medium transport (spectral variant)
	const IObject* pMediumObject = 0;
	const IMedium* pMedium = MediumTracking::GetCurrentMediumWithObject( ior_stack, pScene, pMediumObject );

	// G6: stamp the ambient (incident-medium) IOR (per-wavelength n(λ) in NM) so
	// a GGX conductor shaded via the RayCaster spectral shader path uses the
	// surrounding medium's IOR rather than hardcoded air.  Read before
	// SetCurrentObject (which does not push).  Guard to air (1.0).
	{
		const Scalar ambIOR = ior_stack.top();
		ri.geometric.ambientIOR = ( ambIOR > 0.0 ) ? ambIOR : 1.0;
	}

	// Strategy-selection factor for a no-scatter outcome (see the EQ-MIS block
	// below).  Declared in the outer scope because the survival sites that
	// consume it (surface-hit / escape) live outside the if(pMedium) block.
	// 0.5 only in the DT no-scatter branch under equiangular MIS; 1.0 otherwise.
	Scalar noScatterPdfScale_NM = 1.0;
	Scalar additiveEmissionNM = 0.0;
	const VolumeEmissionSegmentState incomingVolumeSegmentState =
		CurrentVolumeEmissionSegmentState();

	if( pMedium )
	{
		const Scalar maxDist = bHit
			? ( IsExactNullBoundaryMaterial( ri.pMaterial )
				? ri.geometric.surfaceRange : ri.geometric.range )
			: RISE_INFINITY;

		IndependentSampler mediumSampler( rc.random );
		Scalar segmentStart = 0.0;
		Scalar segmentEnd = 0.0;
		if( MediumSegmentInterval(
			*pMedium, ray, maxDist, segmentStart, segmentEnd ) ) {
			// Homogeneous integration is exact and consumes no random number.
			// A heterogeneous source can start anywhere on the segment, so its
			// independent uniform draw must never be gated by one point query.
			const Scalar additiveXi = pMedium->IsHomogeneous()
				? 0.0 : rc.random.CanonicalRandom();
			additiveEmissionNM = FullSegmentAdditiveEmissionNM(
				*pMedium, ray, segmentStart, segmentEnd, nm, additiveXi );
			IndependentSampler chemSampler( rc.random );
			additiveEmissionNM += pMedium->EstimateChemEmissionSegmentNM(
				ray, segmentStart, segmentEnd, nm, chemSampler );
		}
		bool scattered = false;
		Scalar t_m = 0;

		// Equiangular MIS (spectral variant, see RGB path for details)
		const bool useEquiangularMIS_NM = !IsSSSContainmentActive() && pLightSampler &&
			pLightSampler->IsEquiangularPivotDistributionValid() &&
			pLightSampler->GetEquiangularPivotEntryCount() > 0;
		VolumeEmissionPivotState equiangularPivots_NM;
		const bool equiangularPivotsReady_NM = useEquiangularMIS_NM &&
			pLightSampler->ResolveVolumeEmissionPivots(
				mediumSampler, incomingVolumeSegmentState.pivots,
				equiangularPivots_NM );
		Scalar combinedPdf_NM = 0;
		bool useExplicitThroughput_NM = false;
		bool equiangularZeroContrib_NM = false;
		Scalar logCombinedPdf_NM = 0.0;

		if( equiangularPivotsReady_NM )
		{
			bool equiangularSegmentBounded = bHit;
			Scalar eqTNear = 0;
			Scalar eqTFar = maxDist;
			{
				Point3 bbMin, bbMax;
				if( pMedium->GetBoundingBox( bbMin, bbMax ) )
				{
					Scalar tEntry = 0, tExit = maxDist;
					const Scalar invX = (fabs(ray.Dir().x) > 1e-20) ? 1.0 / ray.Dir().x : 0;
					const Scalar invY = (fabs(ray.Dir().y) > 1e-20) ? 1.0 / ray.Dir().y : 0;
					const Scalar invZ = (fabs(ray.Dir().z) > 1e-20) ? 1.0 / ray.Dir().z : 0;
					bool aabbHit = true;
					if( invX != 0 ) {
						Scalar t0 = (bbMin.x - ray.origin.x) * invX;
						Scalar t1 = (bbMax.x - ray.origin.x) * invX;
						if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
						tEntry = fmax( tEntry, t0 ); tExit = fmin( tExit, t1 );
					} else if( ray.origin.x < bbMin.x || ray.origin.x > bbMax.x ) {
						aabbHit = false;
					}
					if( aabbHit && invY != 0 ) {
						Scalar t0 = (bbMin.y - ray.origin.y) * invY;
						Scalar t1 = (bbMax.y - ray.origin.y) * invY;
						if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
						tEntry = fmax( tEntry, t0 ); tExit = fmin( tExit, t1 );
					} else if( ray.origin.y < bbMin.y || ray.origin.y > bbMax.y ) {
						aabbHit = false;
					}
					if( aabbHit && invZ != 0 ) {
						Scalar t0 = (bbMin.z - ray.origin.z) * invZ;
						Scalar t1 = (bbMax.z - ray.origin.z) * invZ;
						if( t0 > t1 ) { const Scalar tmp = t0; t0 = t1; t1 = tmp; }
						tEntry = fmax( tEntry, t0 ); tExit = fmin( tExit, t1 );
					} else if( ray.origin.z < bbMin.z || ray.origin.z > bbMax.z ) {
						aabbHit = false;
					}
					if( aabbHit && tEntry < tExit ) {
						eqTNear = fmax( 0.0, tEntry );
						eqTFar = fmin( maxDist, tExit );
						equiangularSegmentBounded = true;
					}
				}
			}

			if( !equiangularSegmentBounded || eqTFar <= eqTNear )
			{
				t_m = pMedium->SampleDistanceNM( ray, maxDist, nm, mediumSampler, scattered );
			}
			else
			{
				Point3 selectedPivot;
				Scalar selectedPivotPdf = 0.0;
				const bool selectedPivotOk = pLightSampler->SampleEquiangularPivot(
					equiangularPivots_NM, mediumSampler.Get1D(), selectedPivot,
					selectedPivotPdf );
				const Scalar xiStrategy = selectedPivotOk ? mediumSampler.Get1D() : 0.0;

				if( !selectedPivotOk )
				{
					t_m = pMedium->SampleDistanceNM(
						ray, maxDist, nm, mediumSampler, scattered );
				}
				else if( xiStrategy < 0.5 )
				{
					IMedium::DistanceSample ds = pMedium->SampleDistanceWithPdfNM(
						ray, maxDist, nm, mediumSampler );
					t_m = ds.t;
					scattered = ds.scattered;

					if( scattered )
					{
						const Scalar pdf_dt = pMedium->EvalDistancePdfNM(
							ray, t_m, true, maxDist, nm );
						const Scalar pdf_eq = pLightSampler->EquiangularDistancePdf(
							equiangularPivots_NM, ray, eqTNear, eqTFar, true, t_m );
						combinedPdf_NM = 0.5 * pdf_dt + 0.5 * pdf_eq;
						const MISWeights::LogDensity logDensity =
							pLightSampler->EvaluateVolumeEmissionDistanceLogDensityNM(
								*pMedium,ray,maxDist,bHit,&equiangularPivots_NM,
								nm,t_m,true);
						logCombinedPdf_NM = logDensity.hasSupport ?
							logDensity.value : -RISE_INFINITY;
						useExplicitThroughput_NM = true;
					}
					else
					{
						// No-scatter outcome under equiangular MIS: reachable only
						// via this delta-tracking strategy (chosen with prob 0.5).
						noScatterPdfScale_NM = 0.5;
					}
				}
				else
				{
					EquiangularSampling::Sample eqSample =
						EquiangularSampling::SampleDistance(
							ray, selectedPivot, eqTNear, eqTFar,
							true, mediumSampler.Get1D() );
					t_m = eqSample.t;

					if( t_m > eqTNear && t_m < maxDist )
					{
						const Point3 eqPt = ray.PointAtLength( t_m );
						const MediumCoefficientsNM eqCoeff = pMedium->GetCoefficientsNM( eqPt, nm );

						if( eqCoeff.sigma_t > 0 )
						{
							scattered = true;

							const Scalar pdf_dt = pMedium->EvalDistancePdfNM(
								ray, t_m, true, maxDist, nm );
							const Scalar pdf_eq = pLightSampler->EquiangularDistancePdf(
								equiangularPivots_NM, ray, eqTNear, eqTFar, true, t_m );

							combinedPdf_NM = 0.5 * pdf_dt + 0.5 * pdf_eq;
							const MISWeights::LogDensity logDensity =
								pLightSampler->EvaluateVolumeEmissionDistanceLogDensityNM(
									*pMedium,ray,maxDist,bHit,&equiangularPivots_NM,
									nm,t_m,true);
							logCombinedPdf_NM = logDensity.hasSupport ?
								logDensity.value : -RISE_INFINITY;
							useExplicitThroughput_NM = true;
						}
						else
						{
							equiangularZeroContrib_NM = true;
						}
					}
					else
					{
						equiangularZeroContrib_NM = true;
					}
				}
			}
		}
		else
		{
			t_m = pMedium->SampleDistanceNM( ray, maxDist, nm, mediumSampler, scattered );
		}

			if( equiangularZeroContrib_NM )
			{
				c = additiveEmissionNM;
				if( sameSegmentMediumSource ) *sameSegmentMediumSource = c;
				if( distance ) *distance = 0;
				return c != 0.0;
			}

		if( scattered )
		{
			// Phase function convention: wo = travel direction (see RGB path comment)
			const Point3 scatterPt = ray.PointAtLength( t_m );
			const Vector3 wo = ray.Dir();

			const MediumCoefficientsNM coeff = pMedium->GetCoefficientsNM( scatterPt, nm );
			const Scalar Tr = pMedium->EvalTransmittanceNM( ray, t_m, nm );
			Scalar throughput = 0;
			Scalar thermalEmission = 0.0;
			if( pMedium->IsFireMedium() && coeff.sigma_t > 0.0 ) {
				const Scalar epsilonThermal = pMedium->GetThermalEmissionNM( scatterPt, nm );
				if( epsilonThermal != 0.0 ) {
					if( useExplicitThroughput_NM ) {
						const Scalar logPdfDt = pMedium->EvalLogDistancePdfNM(
							ray, t_m, true, maxDist, nm );
						const Scalar logTrDet = logPdfDt - log( coeff.sigma_t );
						thermalEmission = epsilonThermal *
							exp( logTrDet - logCombinedPdf_NM );
					} else {
						// Pure per-wavelength delta tracking: p=sigma_t*T,
						// so epsilon*T/p cancels analytically before division.
						thermalEmission = epsilonThermal / coeff.sigma_t;
					}
				}
			}
			if( thermalEmission != 0.0 &&
				incomingVolumeSegmentState.competitionAvailable ) {
				const Scalar logDistancePdf = useExplicitThroughput_NM ?
					logCombinedPdf_NM : pMedium->EvalLogDistancePdfNM(
						ray,t_m,true,maxDist,nm);
				const MISWeights::LogDensity logPMarch =
					MISWeights::VolumeEmissionMarchLogDensityAtCollision(
						incomingVolumeSegmentState,logDistancePdf,t_m);
				const Scalar pV = pLightSampler ?
					pLightSampler->VolumeEmissionPdf(*pMedium,scatterPt) : 0.0;
				const MISWeights::LogDensity logPV = MISWeights::MakeLogDensity(pV);
				thermalEmission *= MISWeights::VolumeEmissionMarchFamilyWeightFromLogDensities(
					logPMarch,logPV,
					incomingVolumeSegmentState.competitionAvailable,
					incomingVolumeSegmentState.continuationSingular);
			}

			// Score medium sources before recursion-depth and RR termination.
			// Those gates suppress surface/scattering continuation, not the
			// existence of this finite-event source sample.
			if( rrContinuationRejected ) {
				c = additiveEmissionNM + thermalEmission;
				if( sameSegmentMediumSource ) *sameSegmentMediumSource = c;
				if( distance ) *distance = t_m;
				return c != 0.0;
			}
			if( depthGateDeferredForEmission ) {
				c = additiveEmissionNM + thermalEmission;
				if( sameSegmentMediumSource ) *sameSegmentMediumSource = c;
				if( distance ) *distance = t_m;
				return c != 0.0;
			}

			if( useExplicitThroughput_NM && combinedPdf_NM > 0 )
			{
				// MIS throughput: Tr * sigma_s / combined_pdf
				// combined_pdf was computed deterministically in strategy
				// selection using majorant transmittance (see RGB path).
				throughput = Tr * coeff.sigma_s / combinedPdf_NM;
			}
			else if( coeff.sigma_t > 0 )
			{
				// Original: sigma_s / sigma_t (single-scattering albedo)
				throughput = coeff.sigma_s / coeff.sigma_t;
			}

			static const unsigned int nMaxVolumeBounces = 64;
			const bool volumeNEECompetes = !IsSSSContainmentActive() && pLightSampler &&
				pLightSampler->GetVolumeEmissionMediumCount() > 0 &&
				MediumTransport::IsContinuationPhaseClosureNMPreflightAllowlisted(
					*pMedium);
			const MediumContinuationAvailability mediumAvailability =
				ResolveMediumContinuationAvailability(
					rs.depth < nMaxRecursions,rs.volumeBounces,nMaxVolumeBounces);

			// A competing vertex acquires its exact continuation factory only
			// after the closed exact-type preflight.  Preview-style unsupported
			// vertices retain the legacy closure with volume NEE disabled.
			MediumTransport::CollisionPhaseClosure phaseClosure(
				*pMedium, scatterPt, nm, true, volumeNEECompetes );
			const IPhaseFunction* pPhase = phaseClosure.Get();
			const Scalar counterfactualRRSurvival =
				RayCasterRRSurvivalProbability(rs.importance*throughput);
			VolumeEmissionVertexSample volumeVertexSample;
			bool volumeEndpointAttempted = false;
			Scalar volumeLd = 0.0;
			if( volumeNEECompetes && pPhase ) {
				pLightSampler->SampleVolumeEmissionVertex(
					mediumSampler,volumeVertexSample);
				volumeEndpointAttempted = volumeVertexSample.WasEndpointAttempted();
				volumeLd = pLightSampler->EvaluateVolumeDirectLightingFromPhaseClosureNM(
					scatterPt,wo,*pPhase,mediumAvailability,
					counterfactualRRSurvival,nm,volumeVertexSample,pMedium,
					pMediumObject,&ior_stack);
			}

			// NEE at scatter point
			Scalar Ld = MediumTransport::EvaluateInScatteringNM(
				scatterPt, wo, pMedium, pPhase, nm, *this, pLightSampler,
				mediumSampler, rast, pMediumObject, &ior_stack );
			Ld += volumeLd;

			// Phase-function continuation
			Scalar Li = 0;
			Scalar phasePdf = 0;
			Vector3 wi( 0, 0, 0 );
			const bool marchAllowed = volumeNEECompetes ?
				mediumAvailability.marchAllowed :
				(rs.depth < nMaxRecursions &&
					rs.volumeBounces < nMaxVolumeBounces);
			if( pPhase && marchAllowed )
			{
				Scalar guidingMISWeight = 1.0;
				Scalar effectivePdf = 0;
				wi = pPhase->Sample( wo, mediumSampler );
				phasePdf = pPhase->Pdf( wo, wi );
				effectivePdf = phasePdf;

#ifdef RISE_ENABLE_OPENPGL
				// Volume guiding (spectral): one-sample MIS
				if( !volumeNEECompetes && rc.pGuidingField &&
					rc.pGuidingField->IsTrained() &&
					rc.guidingAlpha > 0 &&
					rs.depth < rc.maxGuidingDepth )
				{
					static thread_local Implementation::GuidingVolumeDistributionHandle volGuideHandleNM;

					const Scalar alpha = rc.guidingAlpha;
					if( rc.pGuidingField->InitVolumeDistribution(
						volGuideHandleNM, scatterPt, mediumSampler.Get1D() ) )
					{
						// Apply HG product if the phase function is anisotropic
						const Scalar meanCosine = pPhase->GetMeanCosine();
						if( fabs( meanCosine ) > 1e-6 )
						{
							rc.pGuidingField->ApplyHGProduct(
								volGuideHandleNM, wo, meanCosine );
						}

						const Scalar xiG = mediumSampler.Get1D();
						if( PathTransportUtilities::ShouldUseGuidedSample( alpha, xiG ) )
						{
							Scalar guidePdf = 0;
							const Point2 xi2D( mediumSampler.Get1D(), mediumSampler.Get1D() );
							wi = rc.pGuidingField->SampleVolume( volGuideHandleNM, xi2D, guidePdf );

							if( guidePdf > 0 )
							{
								phasePdf = pPhase->Pdf( wo, wi );
							}
							effectivePdf = PathTransportUtilities::GuidingSelectedMixturePdf(
								alpha, guidePdf, phasePdf, true );
							guidingMISWeight = effectivePdf > 0 ? phasePdf / effectivePdf : 0;
						}
						else
						{
							const Scalar guidePdf = rc.pGuidingField->PdfVolume( volGuideHandleNM, wi );
							effectivePdf = PathTransportUtilities::GuidingSelectedMixturePdf(
								alpha, guidePdf, phasePdf, false );
							guidingMISWeight = effectivePdf > 0 ? phasePdf / effectivePdf : 0;
						}
					}
				}
#endif // RISE_ENABLE_OPENPGL

				if( PathTransportUtilities::IsPositiveFiniteDensity( effectivePdf ) )
				{
					const Ray scatterRay( scatterPt, wi );

					RAY_STATE rs2;
					rs2.depth = rs.depth + 1;
					rs2.importance = rs.importance * throughput * guidingMISWeight;
					rs2.considerEmission = true;
					rs2.type = rs.type;
					rs2.volumeBounces = rs.volumeBounces + 1;
					rs2.bsdfPdf = effectivePdf;

					Scalar continuationCompensation = 1.0;
					bool continuationSurvived = true;
					if( volumeNEECompetes && mediumAvailability.vertexAllowed &&
						counterfactualRRSurvival < 1.0 ) {
						continuationSurvived = rc.random.CanonicalRandom() <
							counterfactualRRSurvival;
						if( continuationSurvived && counterfactualRRSurvival > 0.0 ) {
							continuationCompensation = 1.0/counterfactualRRSurvival;
						}
					}

					Scalar hitDist = 0;
					if( continuationSurvived ) {
						const Scalar marchDirectionPdf =
							mediumAvailability.vertexAllowed ?
								phasePdf*counterfactualRRSurvival : phasePdf;
						const VolumeEmissionSegmentState downstreamVolumeState(
							volumeEndpointAttempted,false,
							volumeEndpointAttempted && volumeVertexSample.HasPivots() ?
								&volumeVertexSample.Pivots() : 0,
							marchDirectionPdf,0.0,0.0);
						const VolumeEmissionSegmentStateScope volumeStateScope(
							downstreamVolumeState);
						CastRayNMImpl_( rc, rast, scatterRay, Li, rs2, nm, &hitDist,
							pRadianceMap, ior_stack, false,
							volumeNEECompetes,
							volumeNEECompetes && !mediumAvailability.vertexAllowed );
						Li *= continuationCompensation;
					}

#ifdef RISE_ENABLE_OPENPGL
				// Record volume training sample (spectral path).
				// Use effectivePdf (= combinedPdf when guiding was applied)
				// so that weight = luminance / pdf matches the actual
				// sampling distribution.
					if( !volumeNEECompetes && rc.pGuidingField &&
						rc.pGuidingField->IsCollectingTrainingSamples() &&
						effectivePdf > NEARZERO )
					{
						if( Li > 0 )
						{
							rc.pGuidingField->AddVolumeSample(
								scatterPt, wi,
								hitDist > 0 ? hitDist : 1.0,
								effectivePdf,
								Li,
								false );
						}
						else
						{
							rc.pGuidingField->AddZeroValueVolumeSample(
								scatterPt, wi );
						}
					}
#endif // RISE_ENABLE_OPENPGL

					Li = Li * guidingMISWeight;
				}
			}

			c = additiveEmissionNM + thermalEmission +
				rrContinuationCompensation * throughput * (Ld + Li);
			if( sameSegmentMediumSource ) {
				*sameSegmentMediumSource = additiveEmissionNM + thermalEmission;
			}

			if( distance ) {
				*distance = t_m;
			}

			return true;
		}
	}

	if( bHit && IsExactNullBoundaryMaterial( ri.pMaterial ) )
	{
		const bool downstreamSourceOnly = sourceOnlySegment ||
			depthGateDeferredForEmission || rrContinuationRejected;
		IORStack nextStack( ior_stack );
			ApplyExactNullBoundaryTransition( ri.pMaterial, ri.pObject, nextStack );
			const Ray nextRay = ContinueExactNullBoundaryRay(
				ray, ri.geometric.surfaceRange );

			Scalar survival = 1.0;
			Scalar noEventProbability = 1.0;
			if( pMedium ) {
				const Scalar Tr = pMedium->EvalTransmittanceNM(
					ray, ri.geometric.surfaceRange, nm );
				noEventProbability = noScatterPdfScale_NM *
					pMedium->EvalDistancePdfNM(
						ray, ri.geometric.surfaceRange, false,
						ri.geometric.surfaceRange, nm );
				survival = noEventProbability > 0.0 ?
					Tr / noEventProbability : 0.0;
			}
			const VolumeEmissionSegmentState downstreamVolumeSegmentState =
				AdvanceVolumeEmissionSegmentState(
					incomingVolumeSegmentState,noEventProbability,
					ri.geometric.surfaceRange);
			Scalar downstream = 0.0;
			Scalar downstreamSameSegmentSource = 0.0;
			Scalar downstreamDistance = 0.0;
			bool downstreamHit = false;
			{
				const VolumeEmissionSegmentStateScope volumeStateScope(
					downstreamVolumeSegmentState);
				downstreamHit = CastRayNMImpl_(
					rc, rast, nextRay, downstream, rs, nm, &downstreamDistance,
					pRadianceMap, nextStack, true, downstreamSourceOnly,
					downstreamSourceOnly, &downstreamSameSegmentSource );
			}
			OffsetCapturedPrimaryAOVDepth(
				rc, ri.geometric.surfaceRange, primaryAOVUnresolvedAtEntry );
			const Scalar localSameSegmentSource = additiveEmissionNM +
				survival * downstreamSameSegmentSource;
			c = localSameSegmentSource +
				rrContinuationCompensation * survival *
					( downstream - downstreamSameSegmentSource );
			if( sameSegmentMediumSource ) {
				*sameSegmentMediumSource = localSameSegmentSource;
			}
			if( distance ) {
				*distance = downstreamDistance >= RISE_INFINITY
					? RISE_INFINITY
					: ri.geometric.surfaceRange + downstreamDistance;
			}
			return downstreamHit || additiveEmissionNM != 0.0;
	}

	if( rrContinuationRejected ) {
		c = additiveEmissionNM;
		if( sameSegmentMediumSource ) *sameSegmentMediumSource = c;
		if( distance ) *distance = 0.0;
		return additiveEmissionNM != 0.0;
	}

	if( depthGateDeferredForEmission ) {
		c = additiveEmissionNM;
		if( sameSegmentMediumSource ) *sameSegmentMediumSource = c;
		if( distance ) *distance = 0.0;
		return additiveEmissionNM != 0.0;
	}

	if( bHit ) {
		// If there is an intersection modifier, then get it to modify
		// the intersection information
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// Set the current object on the IOR stack
		ior_stack.SetCurrentObject( ri.pObject );

		// A non-null interface ends the originating medium-march strategy.
		// Recursive surface continuations start a fresh family; carrying the
		// phase density, pivots, or competition bit across this boundary would
		// assign support to a march that the straight volume connection cannot
		// follow.
		{
			const VolumeEmissionSegmentState resetVolumeState;
			const VolumeEmissionSegmentStateScope volumeStateScope(
				resetVolumeState);
			c = SelectShader( ri ).ShadeNM( rc, ri, *this, rs, nm, ior_stack );
		}

		// Analog no-scatter survival: reaching this surface without a scatter
		// event is a survival outcome whose probability already carries
		// Beer-Lambert.  The correct weight is Tr / pSurvival, where pSurvival is
		// the DETERMINISTIC no-scatter survival pdf EvalDistancePdfNM(false).  For
		// a HomogeneousMedium both equal exp(-sigma_t(nm)*d), so the weight is
		// exactly 1 (byte-identical to applying no factor).  For a
		// HeterogeneousMedium, EvalTransmittanceNM is a STOCHASTIC ratio-tracking
		// estimate while EvalDistancePdfNM is a deterministic Simpson optical
		// depth, so the ratio is the correct unbiased weight (NOT unity).
		if( pMedium ) {
			const Scalar Tr = pMedium->EvalTransmittanceNM( ray, ri.geometric.range, nm );
			const Scalar pSurvival = noScatterPdfScale_NM * pMedium->EvalDistancePdfNM(
				ray, ri.geometric.range, false, ri.geometric.range, nm );
			if( pSurvival > 0 ) {
				c = c * ( Tr / pSurvival );
			}
		}

		if( distance ) {
			*distance = ri.geometric.range;
		}

		bReturn = true;
	} else if( pRadianceMap ) {
		c = pRadianceMap->GetRadianceNM( ray, rast, nm );

		// Analog no-scatter survival weight for the escape-to-background path:
		// Tr / pSurvival (deterministic no-scatter survival pdf; = 1 for
		// homogeneous, correct for heterogeneous — see the surface-hit case).
		if( pMedium ) {
			const Scalar Tr = pMedium->EvalTransmittanceNM( ray, RISE_INFINITY, nm );
			const Scalar pSurvival = noScatterPdfScale_NM * pMedium->EvalDistancePdfNM(
				ray, RISE_INFINITY, false, RISE_INFINITY, nm );
			if( pSurvival > 0 ) {
				c = c * ( Tr / pSurvival );
			}
		}
	} else if( pScene->GetGlobalRadianceMap() ) {
		c = pScene->GetGlobalRadianceMap()->GetRadianceNM( ray, rast, nm );

		// Apply MIS weight for BSDF-sampled environment hit vs env NEE (spectral)
		if( pLightSampler && rs.bsdfPdf > 0 )
		{
			const EnvironmentSampler* pES = pLightSampler->GetEnvironmentSampler();
			if( pES )
			{
				const Scalar envPdf = pES->Pdf( ray.Dir() );
				if( envPdf > 0 )
				{
					// Optimal MIS training (spectral env BSDF-hit): use
					// full integrand Le * BSDF * cos.  For NM, all channels
					// of bsdfTimesCos carry the same scalar value.
					if( rc.pOptimalMIS && !rc.pOptimalMIS->IsReady() )
					{
						const Scalar fVal = c * rs.bsdfTimesCos.r;
						const Scalar f2 = fVal * fVal;
						if( f2 > 0 && rs.bsdfPdf > 0 )
						{
							const_cast<OptimalMISAccumulator*>(rc.pOptimalMIS)->Accumulate(
								rast.x, rast.y,
								f2, rs.bsdfPdf, kTechniqueBSDF );
						}
					}

					Scalar w_bsdf;
					if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
					{
						const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
						w_bsdf = MISWeights::OptimalMIS2Weight( rs.bsdfPdf, envPdf, alpha );
					}
					else
					{
						w_bsdf = PathTransportUtilities::PowerHeuristic( rs.bsdfPdf, envPdf );
					}
					c = c * w_bsdf;
				}
			}
		}

		// Analog no-scatter survival weight for the escape-to-environment path:
		// Tr / pSurvival (deterministic no-scatter survival pdf; = 1 for
		// homogeneous, correct for heterogeneous — see the surface-hit case).
		if( pMedium ) {
			const Scalar Tr = pMedium->EvalTransmittanceNM( ray, RISE_INFINITY, nm );
			const Scalar pSurvival = noScatterPdfScale_NM * pMedium->EvalDistancePdfNM(
				ray, RISE_INFINITY, false, RISE_INFINITY, nm );
			if( pSurvival > 0 ) {
				c = c * ( Tr / pSurvival );
			}
		}

		if( distance && bConsiderRMapAsBackground ) {
			*distance = RISE_INFINITY;
		}

		bReturn = bConsiderRMapAsBackground;
	}

	// Roulette applies only to surface/background continuation.  The arbitrary
	// additive source is an independent full-segment estimate and is neither
	// gated nor reweighted by the continuation decision.
	if( rrContinuationCompensation != 1.0 ) {
		c = c * rrContinuationCompensation;
	}
	c += additiveEmissionNM;
	if( sameSegmentMediumSource ) {
		*sameSegmentMediumSource = additiveEmissionNM;
	}

	return bReturn || additiveEmissionNM != 0.0;
}

bool RayCaster::CastShadowRay( const Ray& ray, const Scalar dHowFar ) const
{
	if( !pScene ) {
		GlobalLog()->PrintSourceError( "RayCaster::CastRay_IntersectionOnly:: No scene", __FILE__, __LINE__ );
		return false;
	}

	// Walk closest hits so an exact NullBoundaryMaterial is transparent by
	// class even when its object authors casts_shadows=TRUE.  Ordinary objects
	// retain the historical casts_shadows behavior.  The absolute-parameter
	// progress rule has no crossing cap and cannot silently darken a valid
	// chain of null enclosures.
	Scalar segmentStart = 0.0;
	for( ;; )
	{
		if( !(segmentStart < dHowFar) ) return false;
		const Ray segmentRay( ray.PointAtLength( segmentStart ), ray.Dir() );
		RayIntersection ri( segmentRay, nullRasterizerState );
		ri.geometric.minimumSurfaceRange =
			std::nextafter( segmentStart, dHowFar ) - segmentStart;
		pScene->GetObjects()->IntersectRay( ri, true, true, true );
		if( !ri.geometric.bHit || ri.geometric.surfaceRange >= dHowFar - segmentStart ) {
			return false;
		}

		const Scalar boundary = segmentStart + ri.geometric.surfaceRange;
		if( !std::isfinite( boundary ) || !(boundary > segmentStart) ) {
			GlobalLog()->PrintEasyError(
				"RayCaster::CastShadowRay: malformed or non-progressing boundary hit" );
			return true;
		}

		if( !IsExactNullBoundaryMaterial( ri.pMaterial ) &&
			ri.pObject && ri.pObject->DoesCastShadows() ) {
			return true;
		}
		segmentStart = boundary;
	}
}

// ================================================================
// CastShadowRayTransmittance — TRANSPARENT (Fresnel-attenuated)
// shadow ray.
//
// Walks the shadow segment hit-by-hit.  At each interface that is a
// PERFECT-SPECULAR TRANSMISSIVE DIELECTRIC, the ray passes STRAIGHT
// through (no refractive bend) and the running transmittance is
// multiplied by the per-interface Fresnel transmittance (1 - F) (and
// the dielectric's per-channel transmittance tint, which is 1 for
// clear glass).  Any other hit fully blocks.
//
// APPROXIMATION — this is the industry-standard "transparent shadow"
// shortcut (Arnold / RenderMan / Cycles `transparent` shadow path):
//   * Propagation is STRAIGHT — refractive bending of the shadow ray
//     is ignored, so the light's apparent position is not displaced.
//   * Internal MULTI-BOUNCE (rays that would reflect inside the shell
//     and re-emerge) is ignored; only the direct (1-F)-per-interface
//     transmission is accounted for.
//   * One REPRESENTATIVE eta per interface is used (see below).
// It trades a small physical inaccuracy for a large NEE-variance
// reduction when a lit surface sits under a thin transparent shell
// (e.g. a watch dial under a sapphire crystal).  Used ONLY by the
// unidirectional PT integrator; BDPT / VCM / MLT keep binary shadows.
//
// eta source:
//   * NM path  (bNM == true):  the hero-wavelength IOR reported by
//     IMaterial::GetSpecularInfoNM(...).ior — exact per-wavelength
//     dispersion at the sampled wavelength.
//   * RGB path (bNM == false): the scalar IOR reported by
//     IMaterial::GetSpecularInfo(...).ior, which is the v[0] channel
//     of the material's IOR painter — the same representative scalar a
//     primary RGB ray uses for its (non-dispersive) Fresnel split.
//
// The entering-vs-exiting side is taken from the sign of
// dot(rayDir, geometricNormal): entering air->medium uses
// (Ni=outerIOR, Nt=mediumIOR); exiting medium->air uses the swap.
// A local IOR stack is maintained so NESTED dielectrics (e.g. a
// double-domed crystal, or glass-in-glass) get the right relative eta
// at each crossing.  Total internal reflection (no real transmitted
// direction) blocks that path (transmittance -> 0 -> fully occluded).
// ================================================================
bool RayCaster::CastShadowRayTransmittance(
	const Ray& ray,
	const Scalar dHowFar,
	const bool bNM,
	const Scalar nm,
	RISEPel& transmittance
	) const
{
	transmittance = RISEPel( 1.0, 1.0, 1.0 );

	if( !pScene ) {
		GlobalLog()->PrintSourceError( "RayCaster::CastShadowRayTransmittance:: No scene", __FILE__, __LINE__ );
		// Conservative: treat as occluded so we never leak unshadowed light.
		return true;
	}

	// Cap on interface crossings; past this, conservatively report
	// blocked rather than spend unbounded work on a pathological stack
	// of nested dielectrics.
	// (counts interface CROSSINGS, not objects: a meniscus shell is 2,
	// nested glass-in-glass 4; 32 leaves headroom before the safe-but-
	// darkening conservative block kicks in.)
	static const unsigned int kMaxCrossings = 32;

	// Small step-off so the next IntersectRay does not re-hit the
	// surface we just crossed.  Matches the order of magnitude of the
	// shadow-ray end epsilon (dist - 0.001) used at the NEE call sites.
	static const Scalar kStepEps = 1.0e-4;

	const Vector3 dir = ray.Dir();

	// Local IOR stack so nested dielectrics resolve the correct
	// relative eta per crossing.  Starts in air (1.0); the
	// GetSpecularInfo IOR-side logic uses containsCurrent() so we keep
	// the stack's current-object pointer in sync as we cross.
	IORStack ior_stack( 1.0 );
	if( pLightSampler && pLightSampler->SceneHasNullBoundaries() && pScene ) {
		IORStackSeeding::SeedFromPoint( ior_stack, ray.origin, *pScene );
	}

	Point3 origin = ray.origin;
	Scalar remaining = dHowFar;
	bool rejectZeroDistanceBoundary = false;

	for( unsigned int crossing = 0; crossing < kMaxCrossings; )
	{
		Ray segRay( origin, dir );
		RayIntersection ri( segRay, nullRasterizerState );
		if( rejectZeroDistanceBoundary ) {
			ri.geometric.minimumSurfaceRange = 0.0;
		}
		pScene->GetObjects()->IntersectRay(
			ri, true, true, rejectZeroDistanceBoundary );
		rejectZeroDistanceBoundary = false;

		if( !ri.geometric.bHit || ri.geometric.range >= remaining )
		{
			// Reached the light with no further occluder along the
			// remaining segment — the accumulated transmittance is final.
			return false;
		}

		// There is a hit strictly before the light.  Decide whether it
		// is a perfect-specular transmissive dielectric we can pass
		// through, or an occluder that fully blocks.
		ior_stack.SetCurrentObject( ri.pObject );

		if( IsExactNullBoundaryMaterial( ri.pMaterial ) )
		{
			const Scalar advance = ri.geometric.surfaceRange;
			ApplyExactNullBoundaryTransition( ri.pMaterial, ri.pObject, ior_stack );
			origin = segRay.PointAtLength( advance );
			remaining -= advance;
			if( remaining <= 0.0 ) return false;
			rejectZeroDistanceBoundary = true;
			continue;
		}

		SpecularInfo info;
		if( ri.pMaterial )
		{
			info = bNM
				? ri.pMaterial->GetSpecularInfoNM( ri.geometric, ior_stack, nm )
				: ri.pMaterial->GetSpecularInfo( ri.geometric, ior_stack );
		}

		// Capability test.  Only a CLEAR transmissive dielectric boundary
		// (light enters a NON-scattering medium and exits the far side) may
		// pass a shadow ray straight through.  info.clearTransmission is set
		// ONLY by DielectricSPF / PerfectRefractorSPF.  It is false for the
		// default IMaterial/ISPF (valid=false: opaque/diffuse/rough), for
		// pure mirrors (canRefract=false), for PolishedSPF (a coat over a
		// substrate that refracted light hits -> must block), and for smooth
		// SubSurface/SSS materials (isSpecular && canRefract are true for the
		// boundary, but the volume behind it scatters -> must block, NOT pass
		// with bare Fresnel).  The clearTransmission gate closes that
		// SSS/coat false-positive that isSpecular && canRefract alone allowed.
		const bool bTransmissiveDielectric =
			info.valid && info.isSpecular && info.canRefract && info.clearTransmission;

		if( !bTransmissiveDielectric )
		{
			// Opaque / diffuse / rough / mirror occluder — fully blocks.
			return true;
		}

		// --- Per-interface Fresnel transmittance (straight-through) ---
		//
		// cosI is measured against the GEOMETRIC normal.  Entering when
		// the ray travels into the surface (dot < 0), exiting otherwise.
		const Vector3 geomN = ri.geometric.vGeomNormal;
		const Scalar cosRaw = Vector3Ops::Dot( dir, geomN );
		const bool bEntering = ( cosRaw < 0.0 );

		const Scalar mediumIOR = info.ior > NEARZERO ? info.ior : Scalar(1.0);

		// Relative IORs for this crossing.  On entry: outside -> medium;
		// on exit: medium -> outside (outside taken from the stack just
		// below the current object, falling back to air).
		Scalar Ni, Nt;
		if( bEntering )
		{
			Ni = ior_stack.top();		// current outside medium (air, or an enclosing dielectric)
			Nt = mediumIOR;
		}
		else
		{
			Ni = mediumIOR;
			// Outside IOR after we leave this medium: peek the stack with
			// the current object popped.  Guard the pop on containsCurrent()
			// so a shadow ray that ORIGINATES inside a dielectric (the
			// entry face was never crossed by this walk) cleanly reads air
			// as the outside medium instead of logging a spurious
			// underflow warning.
			IORStack peek( ior_stack );
			if( peek.containsCurrent() ) {
				peek.pop();
			}
			Nt = peek.top();
		}

		// Straight-through Fresnel: compute the would-be refracted
		// direction (Snell) purely to obtain cos(theta_t) for the
		// unpolarized dielectric reflectance.  TIR (no real transmitted
		// direction) means the light cannot pass straight through this
		// interface — block it.
		const Vector3 fresnelNormal = bEntering ? geomN : -geomN;
		Vector3 vRefr = dir;
		Scalar F;
		if( Optics::CalculateRefractedRay( fresnelNormal, Ni, Nt, vRefr ) )
		{
			F = Optics::CalculateDielectricReflectance( dir, vRefr, fresnelNormal, Ni, Nt );
		}
		else
		{
			// Total internal reflection: no transmitted path.
			transmittance = RISEPel( 0, 0, 0 );
			return true;
		}

		const Scalar T = 1.0 - F;
		if( T <= 0.0 )
		{
			transmittance = RISEPel( 0, 0, 0 );
			return true;
		}

		// Per-interface Fresnel transmittance T = 1 - F only.  We do NOT
		// fold the dielectric's tau tint here: the BSDF path applies tau as
		// Beer-Lambert pow(tau, in-medium-distance), whereas a flat per-
		// interface tau would be applied twice (entry + exit) with no path-
		// length term -- wrong for tinted glass and inconsistent between the
		// RGB and NM paths.  Matching Arnold / RenderMan / Cycles transparent
		// shadows, attenuate by Fresnel only (exact for clear glass, tau=1);
		// colored-glass shadow tint is a documented non-goal.  RGB == NM.
		transmittance = transmittance * T;

		// Early-out once the segment is effectively opaque to avoid
		// pointless further intersection work.
		if( ColorMath::MaxValue( transmittance ) <= NEARZERO )
		{
			transmittance = RISEPel( 0, 0, 0 );
			return true;
		}

		// Maintain the IOR stack across the crossing so the next
		// interface sees the correct enclosing medium.  The exit pop is
		// guarded on containsCurrent() for the originates-inside-a-
		// dielectric case (see the exit-peek note above).
		if( bEntering ) {
			ior_stack.push( mediumIOR );
		} else if( ior_stack.containsCurrent() ) {
			ior_stack.pop();
		}

		// Advance past this interface and continue toward the light.
		// Step the origin to the hit point plus a small epsilon along
		// the (unchanged) travel direction; shrink the remaining range
		// accordingly.
		// Scale-relative step-off max(kStepEps, range*1e-5): stays above
		// floating-point hit-position error at any scene scale (a fixed
		// 1e-4 under-steps in very large scenes), while the 1e-4 floor
		// covers small scenes.  Far below the thinnest real feature.
		const Scalar relStep = ri.geometric.range * Scalar(1.0e-5);
		const Scalar advance = ri.geometric.range + ( relStep > kStepEps ? relStep : kStepEps );
		origin = segRay.PointAtLength( advance );
		remaining -= advance;

		if( remaining <= 0.0 )
		{
			// Stepped at or past the light — nothing more occludes.
			return false;
		}
		++crossing;
	}

	// Crossing cap reached — conservatively report blocked.
	return true;
}

// ================================================================
// CastShadowRayAuto — flag-aware NEE shadow occlusion.
//
// One source of truth for "shadow test that honors transparent_shadows":
// when the flag is on, walk the segment with the Fresnel-transmittance test
// (clear dielectrics attenuate rather than block); when off, the binary test.
// Used by BOTH the LightSampler NEE evaluators (omni / spot / area) and the
// directional / ambient Step-1 lights, so the flag applies uniformly across
// light types.  It is geometry-agnostic: it forwards to the same closest-hit
// traversal that analytic primitives and SDFs both use, so primitive and SDF
// dielectrics occlude (flag off) or transmit (flag on) identically.
// ================================================================
bool RayCaster::CastShadowRayAuto(
	const Ray& ray,
	const Scalar dHowFar,
	const bool bNM,
	const Scalar nm,
	RISEPel& transmittance
	) const
{
	if( bTransparentShadows ) {
		return CastShadowRayTransmittance( ray, dHowFar, bNM, nm, transmittance );
	}
	transmittance = RISEPel( 1.0, 1.0, 1.0 );
	return CastShadowRay( ray, dHowFar );
}

void RayCaster::SetRISCandidates( const unsigned int M )
{
	// Retain so a same-pointer sampler rebuild (#2b(a)) re-applies it; see
	// iPendingRISCandidates.
	iPendingRISCandidates = (int)M;
	if( pLightSampler )
	{
		pLightSampler->SetRISCandidates( M );
	}
}

void RayCaster::SetLightSampleRRThreshold( const Scalar threshold )
{
	dPendingLightRRThreshold = threshold;
	if( pLightSampler )
	{
		pLightSampler->SetLightSampleRRThreshold( threshold );
	}
}

void RayCaster::SetUseLightBVH( const bool enable )
{
	bPendingUseLightBVH = enable;
	if( pLightSampler )
	{
		pLightSampler->SetUseLightBVH( enable );
	}
}

namespace
{
	// GUI render modes (docs/gui/RENDER_MODES.md §3 "light solo"): collects
	// every enumerated name into a ", "-joined, quoted list for an honest
	// "unresolved name" error -- same shape as AgentSession's `view`
	// resolution NameCollector.
	struct NameJoinCollector : public IEnumCallback<const char*>
	{
		std::string* out;
		bool operator()( const char* const& n ) override
		{
			if( !out->empty() ) *out += ", ";
			*out += "\"";
			*out += ( n ? n : "" );
			*out += "\"";
			return true;
		}
	};
}

bool RayCaster::SetSoloLightByName( const char* name, std::string* pAvailableNames )
{
	if( !name || !*name )
	{
		ClearSoloLight();
		return true;
	}

	if( !pScene )
	{
		if( pAvailableNames ) *pAvailableNames = "";
		return false;
	}

	// Try an explicit (non-mesh) light first -- ILightManager::GetItem is
	// an exact-name lookup, so any match (zero- or nonzero-exitance) wins
	// outright.
	if( const ILightManager* pLightMgr = pScene->GetLights() )
	{
		if( ILightPriv* pLight = pLightMgr->GetItem( name ) )
		{
			iPendingSoloKind = 1;
			pendingSoloLight = pLight;
			pendingSoloLuminary = 0;
			if( pLightSampler )
			{
				pLightSampler->SetSoloLight( pendingSoloLight );
			}
			return true;
		}
	}

	// Fall back to a named mesh object whose material is emissive (a
	// luminary).  A named object that exists but is NOT emissive falls
	// through to the "unresolved" branch below -- naming a non-light
	// object is not a valid solo target, and silently treating it as one
	// would render a scene with EVERY light off rather than the honest
	// "unknown name" failure.
	if( const IObjectManager* pObjMgr = pScene->GetObjects() )
	{
		if( IObjectPriv* pObj = pObjMgr->GetItem( name ) )
		{
			if( pObj->GetMaterial() && pObj->GetMaterial()->GetEmitter() )
			{
				iPendingSoloKind = 2;
				pendingSoloLight = 0;
				pendingSoloLuminary = pObj;
				if( pLightSampler )
				{
					pLightSampler->SetSoloLuminary( pendingSoloLuminary );
				}
				return true;
			}
		}
	}

	// Reserved name: the ENVIRONMENT is a light source, and light solo's
	// correctness property is the partition identity
	// solo(A) + solo(B) == all -- which is unstatable without a way to
	// name the env half.  Resolved LAST, so an authored light or emissive
	// object literally named "environment" always wins; only a scene that
	// actually HAS a radiance map accepts it, so the name fails honestly
	// (with the available list) on scenes where there is no environment to
	// solo.
	if( pScene->GetGlobalRadianceMap() && std::strcmp( name, "environment" ) == 0 )
	{
		iPendingSoloKind = 3;
		pendingSoloLight = 0;
		pendingSoloLuminary = 0;
		if( pLightSampler )
		{
			pLightSampler->SetSoloEnvironment();
		}
		return true;
	}

	// Unresolved: build the honest available-name list (every light name
	// UNION every emissive object name, plus the reserved "environment"
	// when the scene has a radiance map) for the caller's error message.
	if( pAvailableNames )
	{
		pAvailableNames->clear();
		NameJoinCollector collector;
		collector.out = pAvailableNames;
		if( const ILightManager* pLightMgr = pScene->GetLights() )
		{
			pLightMgr->EnumerateItemNames( collector );
		}
		if( const IObjectManager* pObjMgr = pScene->GetObjects() )
		{
			struct EmissiveNameCollector : public IEnumCallback<const char*>
			{
				const IObjectManager* pObjMgr;
				std::string* out;
				bool operator()( const char* const& n ) override
				{
					if( IObjectPriv* pObj = pObjMgr->GetItem( n ) )
					{
						if( pObj->GetMaterial() && pObj->GetMaterial()->GetEmitter() )
						{
							if( !out->empty() ) *out += ", ";
							*out += "\"";
							*out += ( n ? n : "" );
							*out += "\"";
						}
					}
					return true;
				}
			} emissiveCollector;
			emissiveCollector.pObjMgr = pObjMgr;
			emissiveCollector.out = pAvailableNames;
			pObjMgr->EnumerateItemNames( emissiveCollector );
		}
		if( pScene->GetGlobalRadianceMap() )
		{
			if( !pAvailableNames->empty() ) *pAvailableNames += ", ";
			*pAvailableNames += "\"environment\"";
		}
	}

	return false;
}

void RayCaster::ClearSoloLight()
{
	iPendingSoloKind = 0;
	pendingSoloLight = 0;
	pendingSoloLuminary = 0;
	if( pLightSampler )
	{
		pLightSampler->ClearSolo();
	}
}

// review-p2d P3-5: restore a snapshot taken by CaptureSoloState.  The kind
// switch MUST enumerate every SoloKind -- a missing case here silently
// downgrades to "no solo", which is precisely the enum-translation trap this
// codebase has been bitten by before, so it is written as an exhaustive
// switch with a default that clears rather than a chain of ifs.
void RayCaster::RestoreSoloState( const SoloStateSnapshot& snap )
{
	switch( snap.kind )
	{
	case 1:
		iPendingSoloKind = 1;
		pendingSoloLight = snap.light;
		pendingSoloLuminary = 0;
		if( pLightSampler ) pLightSampler->SetSoloLight( pendingSoloLight );
		break;
	case 2:
		iPendingSoloKind = 2;
		pendingSoloLight = 0;
		pendingSoloLuminary = snap.luminary;
		if( pLightSampler ) pLightSampler->SetSoloLuminary( pendingSoloLuminary );
		break;
	case 3:
		iPendingSoloKind = 3;
		pendingSoloLight = 0;
		pendingSoloLuminary = 0;
		if( pLightSampler ) pLightSampler->SetSoloEnvironment();
		break;
	default:
		ClearSoloLight();
		break;
	}
}

// ================================================================
// CastRayHWSS — HWSS wavelength bundle ray casting.
//
// Performs a SINGLE scene intersection shared by all wavelengths,
// then dispatches to ShadeHWSS which routes through
// PerformOperationHWSS.  This enables hero-wavelength directional
// sharing: the hero drives BSDF sampling / NEE direction and
// companions evaluate throughput at the hero's geometric direction.
//
// Participating media: falls back to per-wavelength CastRayNM
// because volumetric scattering (equiangular MIS, free-flight
// sampling) is wavelength-dependent and would require a separate
// multi-wavelength medium transport implementation.
// ================================================================
bool RayCaster::CastRayHWSS(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& ray,
	Scalar c[SampledWavelengths::N],
	const RAY_STATE& rs,
	SampledWavelengths& swl,
	Scalar* distance,
	const IRadianceMap* pRadianceMap,
	const IORStack& ior_stack
	) const
{
	if( pLightSampler && pLightSampler->SceneHasNullBoundaries() &&
		pScene && !ior_stack.topObject() ) {
		IORStack seededStack( ior_stack );
		IORStackSeeding::SeedFromPoint( seededStack, ray.origin, *pScene );
		return CastRayHWSSImpl_( rc, rast, ray, c, rs, swl, distance,
			pRadianceMap, seededStack, false );
	}
	return CastRayHWSSImpl_( rc, rast, ray, c, rs, swl, distance,
		pRadianceMap, ior_stack, false );
}

bool RayCaster::CastRayHWSSImpl_(
	const RuntimeContext& rc,
	const RasterizerState& rast,
	const Ray& ray,
	Scalar c[SampledWavelengths::N],
	const RAY_STATE& rs,
	SampledWavelengths& swl,
	Scalar* distance,
	const IRadianceMap* pRadianceMap,
	const IORStack& ior_stack,
	const bool skipEntryGates
	) const
{
	const bool primaryAOVUnresolvedAtEntry =
		rc.pAOV && !rc.pAOV->primaryDepthCaptured;
	for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		c[i] = 0;

	// Check for participating medium before recursion and Russian-roulette
	// gates.  The per-wavelength fallback owns both gates and keeps a local
	// fire-source score ahead of continuation termination.
	const IMedium* pMedium = MediumTracking::GetCurrentMedium( ior_stack, pScene );
	const bool sceneHasFire = pLightSampler && pLightSampler->SceneHasFireMedia();
	if( sceneHasFire || pMedium )
	{
		bool anyHit = false;
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ )
		{
			c[i] = 0;
			if( !swl.terminated[i] )
			{
				bool hit = CastRayNMImpl_( rc, rast, ray, c[i], rs,
					swl.lambda[i], distance, pRadianceMap, ior_stack,
					skipEntryGates );
				if( hit ) anyHit = true;
			}
		}
		return anyHit;
	}

#ifdef ENABLE_MAX_RECURSION
	if( !skipEntryGates && rs.depth > nMaxRecursions )
		return false;
#endif

	// Russian roulette using hero importance (applied only on the
	// non-medium path — medium fallback delegates to CastRayNM
	// which does its own RR).
	Scalar rrCompensation = 1.0;
#ifdef ENABLE_RAYCASTER_RR
	if( !skipEntryGates && rs.importance < RC_RR_THRESHOLD && rs.importance > 0 )
	{
		const Scalar pSurvive = rs.importance / RC_RR_THRESHOLD;
		if( rc.random.CanonicalRandom() >= pSurvive )
			return false;
		rrCompensation = 1.0 / pSurvive;
	}
#endif

	// Single scene intersection shared by all wavelengths
	RayIntersection ri( ray, rast );
	ri.geometric.glossyFilterWidth = rs.glossyFilterWidth;
	ri.geometric.bWantsWireEdgeInfo = bWantsWireEdgeInfo;
	if( skipEntryGates ) ri.geometric.minimumSurfaceRange = 0.0;
	pScene->GetObjects()->IntersectRay( ri, true, true, skipEntryGates );
	CapturePrimaryAOV( rc, ri );

	bool bHit = ri.geometric.bHit;

	if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
		if( ri.pMaterial && !IsExactNullBoundaryMaterial( ri.pMaterial ) &&
			ri.pMaterial->GetEmitter() ) {
			bHit = bShowLuminaires;
		}
	}

	// GUI render modes (docs/gui/RENDER_MODES.md "X-ray axis"): same
	// pre-shade structure as CastRay/CastRayNM, so the resolve is wired
	// here too for completeness -- though in practice no HWSS caster is
	// ever a view-mode/preview caster (those are Pel-only), so
	// bXrayViewResolve is never set on a caster that reaches this path.
	if( bXrayViewResolve && bHit ) {
		ResolveXrayView_( ri );
		bHit = ri.geometric.bHit;

		// Re-apply the same luminaire-suppression check to the RESOLVED
		// hit -- see CastRay's identical call site for the rationale.
		if( bHit && rs.type == IRayCaster::RAY_STATE::eRayView ) {
			if( ri.pMaterial && !IsExactNullBoundaryMaterial( ri.pMaterial ) &&
				ri.pMaterial->GetEmitter() ) {
				bHit = bShowLuminaires;
			}
		}
	}

	bool bReturn = false;

	if( bHit ) {
		if( IsExactNullBoundaryMaterial( ri.pMaterial ) )
		{
			IORStack nextStack( ior_stack );
			ApplyExactNullBoundaryTransition( ri.pMaterial, ri.pObject, nextStack );
			const Ray nextRay = ContinueExactNullBoundaryRay(
				ray, ri.geometric.surfaceRange );

			Scalar downstream[SampledWavelengths::N];
			Scalar downstreamDistance = 0.0;
			const bool downstreamHit = CastRayHWSSImpl_(
				rc, rast, nextRay, downstream, rs, swl, &downstreamDistance,
				pRadianceMap, nextStack, true );
			OffsetCapturedPrimaryAOVDepth(
				rc, ri.geometric.surfaceRange, primaryAOVUnresolvedAtEntry );
			for( unsigned int i = 0; i < SampledWavelengths::N; ++i ) {
				c[i] = downstream[i] * rrCompensation;
			}
			if( distance ) {
				*distance = downstreamDistance >= RISE_INFINITY
					? RISE_INFINITY
					: ri.geometric.surfaceRange + downstreamDistance;
			}
			return downstreamHit;
		}

		// Intersection modifier
		if( ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		// IOR stack (shared geometry)
		ior_stack.SetCurrentObject( ri.pObject );

		// Dispatch to ShadeHWSS — this routes through
		// PerformOperationHWSS, enabling hero-wavelength
		// directional sharing in PathTracingShaderOp.
		SelectShader( ri ).ShadeHWSS( rc, ri, *this, rs, c, swl, ior_stack );

		if( distance ) {
			*distance = ri.geometric.range;
		}

		bReturn = true;
	} else if( pRadianceMap ) {
		// Local radiance map: evaluate per wavelength
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
			if( !swl.terminated[i] ) {
				c[i] = pRadianceMap->GetRadianceNM( ray, rast, swl.lambda[i] );
			}
		}
	} else if( pScene->GetGlobalRadianceMap() ) {
		// Global radiance map (environment): per wavelength with
		// MIS weights using the hero's BSDF pdf.
		const IRadianceMap* pGlobalRM = pScene->GetGlobalRadianceMap();
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
			if( !swl.terminated[i] ) {
				c[i] = pGlobalRM->GetRadianceNM( ray, rast, swl.lambda[i] );
			}
		}

		// Environment MIS: use hero's bsdfPdf for the balance
		// heuristic (all wavelengths share the same geometric
		// direction, so the same PDF applies).
		if( pLightSampler && rs.bsdfPdf > 0 )
		{
			const EnvironmentSampler* pES = pLightSampler->GetEnvironmentSampler();
			if( pES )
			{
				const Scalar envPdf = pES->Pdf( ray.Dir() );
				if( envPdf > 0 )
				{
					Scalar w_bsdf;
					if( rc.pOptimalMIS && rc.pOptimalMIS->IsReady() )
					{
						const Scalar alpha = rc.pOptimalMIS->GetAlpha( rast.x, rast.y );
						w_bsdf = MISWeights::OptimalMIS2Weight( rs.bsdfPdf, envPdf, alpha );
					}
					else
					{
						w_bsdf = PathTransportUtilities::PowerHeuristic( rs.bsdfPdf, envPdf );
					}
					for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
						c[i] *= w_bsdf;
					}
				}
			}
		}

		if( distance && bConsiderRMapAsBackground ) {
			*distance = RISE_INFINITY;
		}

		bReturn = bConsiderRMapAsBackground;
	}

	// RR compensation
	if( rrCompensation != 1.0 ) {
		for( unsigned int i = 0; i < SampledWavelengths::N; i++ ) {
			c[i] *= rrCompensation;
		}
	}

	return bReturn;
}

//! Sets the luminaire sampler
void RayCaster::SetLuminaireSampling(
	ISampling2D* pLumSam							///< [in] Kernel to use for luminaire sampling
	)
{
	safe_release( pLumSampling );

	if( pLumSam ) {
		pLumSampling = pLumSam;
		pLumSampling->addref();
	}
}
