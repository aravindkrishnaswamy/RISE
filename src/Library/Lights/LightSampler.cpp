//////////////////////////////////////////////////////////////////////
//
//  LightSampler.cpp - Implementation of the LightSampler utility
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LightSampler.h"
#include <cmath>
#include <vector>
#include "../Utilities/ISampler.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IContinuationClosure.h"
#include "../Interfaces/IEmitter.h"
#include "../Materials/NullBoundaryMaterial.h"
#include "../Interfaces/IRayCaster.h"
#include "../Rendering/RayCaster.h"		// concrete RayCaster — dynamic_cast target for transparent (Fresnel-attenuated) shadow rays
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/PathTransportUtilities.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/OptimalMISAccumulator.h"
#include "../Utilities/MISWeights.h"
#include "../Utilities/EquiangularSampler.h"

using namespace RISE;
using namespace RISE::Implementation;

// ----------------------------------------------------------------
// Transparent (Fresnel-attenuated) shadow-ray helpers.
//
// These wrap the NEE occlusion test so a single call decides between
// the BINARY shadow test (default) and the Fresnel-transmittance walk
// (when the concrete RayCaster has `transparent_shadows` enabled — a
// unidirectional-PT-only opt-in; BDPT / VCM / MLT keep binary
// shadows).
//
// Contract (matches CastShadowRay's "true == occluded"):
//   return true   → light is FULLY occluded; @a transmittance is 0.
//   return false  → light is reachable; @a transmittance carries the
//                   accumulated per-interface Fresnel transmittance
//                   (1,1,1 when the path was clear, or in binary mode).
//
// The transparent path activates ONLY when:
//   (a) the caster is the concrete RayCaster (dynamic_cast succeeds), AND
//   (b) RayCaster::GetTransparentShadows() is true.
// Any other IRayCaster implementation, or the flag being off, falls
// back to the binary test with transmittance = 1 — so default
// behaviour is byte-identical to before this feature.
// ----------------------------------------------------------------
static bool ShadowOccludedRGB(
	const IRayCaster& caster,
	const Ray& ray,
	const Scalar dHowFar,
	RISEPel& transmittance
	)
{
	// Delegate to RayCaster::CastShadowRayAuto, the single source of truth for
	// the binary-vs-Fresnel-transmittance choice (shared with the directional /
	// ambient Step-1 lights so the flag is honored uniformly across light types).
	const RayCaster* pRC = dynamic_cast<const RayCaster*>( &caster );
	if( pRC )
	{
		return pRC->CastShadowRayAuto( ray, dHowFar, false, 0.0, transmittance );
	}
	transmittance = RISEPel( 1.0, 1.0, 1.0 );
	return caster.CastShadowRay( ray, dHowFar );
}

static bool ShadowOccludedNM(
	const IRayCaster& caster,
	const Ray& ray,
	const Scalar dHowFar,
	const Scalar nm,
	Scalar& transmittance
	)
{
	// Delegate to RayCaster::CastShadowRayAuto (see ShadowOccludedRGB).
	const RayCaster* pRC = dynamic_cast<const RayCaster*>( &caster );
	if( pRC )
	{
		RISEPel t( 1.0, 1.0, 1.0 );
		const bool occluded = pRC->CastShadowRayAuto( ray, dHowFar, true, nm, t );
		transmittance = t.r;	// NM path fills all 3 channels equally
		return occluded;
	}
	transmittance = 1.0;
	return caster.CastShadowRay( ray, dHowFar );
}

// ----------------------------------------------------------------
// Shadow-ray medium transmittance.  Each boundary-delimited segment is
// evaluated immediately from its own origin in the active medium.  The
// active-object stack and boundary loop are dynamic: there is no nesting,
// crossing-count, or low-transmittance cutoff.  Malformed/non-progressing
// boundary topology fails explicitly instead of returning a partial product.
// ----------------------------------------------------------------

struct ShadowMediumStack
{
	struct Entry
	{
		const IObject* pObj;
		const IMedium* pMedium;
	};

	std::vector<Entry> entries;

	bool push( const IObject* pObj, const IMedium* pMedium )
	{
		for( std::vector<Entry>::const_iterator i = entries.begin(); i != entries.end(); ++i ) {
			if( i->pObj == pObj ) return false;
		}
		Entry entry = { pObj, pMedium };
		entries.push_back( entry );
		return true;
	}

	bool remove( const IObject* pObj )
	{
		for( std::vector<Entry>::iterator i = entries.begin(); i != entries.end(); ++i ) {
			if( i->pObj == pObj ) {
				entries.erase( i );
				return true;
			}
		}
		return false;
	}

	bool contains( const IObject* pObj ) const
	{
		for( std::vector<Entry>::const_iterator i = entries.begin(); i != entries.end(); ++i ) {
			if( i->pObj == pObj ) return true;
		}
		return false;
	}

	const IMedium* top() const
	{
		return entries.empty() ? 0 : entries.back().pMedium;
	}
};

struct ShadowTransmittanceRGB
{
	typedef RISEPel Value;
	Value Identity() const { return RISEPel( 1, 1, 1 ); }
	Value Zero() const { return RISEPel( 0, 0, 0 ); }
	bool IsValid( const Value& value ) const
	{
		return IsFiniteDouble( value.r ) && IsFiniteDouble( value.g ) &&
			IsFiniteDouble( value.b ) && value.r >= 0.0 && value.g >= 0.0 &&
			value.b >= 0.0;
	}
	Value Evaluate( const IMedium& medium, const Ray& ray, const Scalar dist ) const
	{
		return medium.EvalTransmittance( ray, dist );
	}
};

struct ShadowTransmittanceNM
{
	typedef Scalar Value;
	const Scalar nm;
	explicit ShadowTransmittanceNM( const Scalar wavelength ) : nm( wavelength ) {}
	Value Identity() const { return 1.0; }
	Value Zero() const { return 0.0; }
	bool IsValid( const Value value ) const
	{
		return IsFiniteDouble( value ) && value >= 0.0;
	}
	Value Evaluate( const IMedium& medium, const Ray& ray, const Scalar dist ) const
	{
		return medium.EvalTransmittanceNM( ray, dist, nm );
	}
};

enum ShadowBoundaryPolicy
{
	eShadowBoundaryIgnoreGeometry,
	eShadowBoundaryExactNullOnly
};

struct NoMarchProposalObserver
{
	bool RequiresGeometry() const { return false; }
	bool Accumulate(
		const IMedium*, const Ray&, const Scalar, const Scalar,
		const bool, const bool ) { return true; }
	void Blocked() {}
};

class VolumeMarchProposalObserverNM
{
public:
	VolumeMarchProposalObserverNM(
		const LightSampler& sampler,
		const Scalar nm,
		const VolumeEmissionPivotState& pivots,
		const Scalar directionPdf ) :
		sampler_( sampler ), nm_( nm ), pivots_( pivots ),
		logDirection_( MISWeights::MakeLogDensity(directionPdf) ),
		logBoundarySurvival_( 0.0 ), endpointSeen_( false ), supported_( true )
	{}

	bool RequiresGeometry() const { return true; }

	bool Accumulate(
		const IMedium* pMedium,
		const Ray& ray,
		const Scalar travelDistance,
		const Scalar proposalMaxDist,
		const bool surfaceBounded,
		const bool endpointSegment )
	{
		if( !supported_ ) return true;
		if( !pMedium ) {
			if( endpointSegment ) supported_ = false;
			return true;
		}
		const MISWeights::LogDensity distanceDensity =
			sampler_.EvaluateVolumeEmissionDistanceLogDensityNM(
				*pMedium,ray,proposalMaxDist,surfaceBounded,&pivots_,nm_,
				travelDistance,endpointSegment);
		if( !distanceDensity.hasSupport ) {
			supported_ = false;
			return true;
		}
		if( endpointSegment ) {
			endpointSeen_ = true;
			logEndpointDistance_ = distanceDensity;
		} else {
			logBoundarySurvival_ += distanceDensity.value;
			if( !RISE::IsFiniteDouble(logBoundarySurvival_) ||
				logBoundarySurvival_ > 0.0 ) supported_ = false;
		}
		return true;
	}

	void Blocked() { supported_ = false; }

	MISWeights::LogDensity Finish( const Scalar endpointDistance ) const
	{
		if( !supported_ || !endpointSeen_ || !logDirection_.hasSupport ||
			!logEndpointDistance_.hasSupport ||
			!RISE::IsFiniteDouble(endpointDistance) || endpointDistance <= 0.0 ) {
			return MISWeights::LogDensity();
		}
		return MISWeights::MakeLogDensityFromLogValue(
			logDirection_.value + logBoundarySurvival_ +
			logEndpointDistance_.value - 2.0*log(endpointDistance) );
	}

private:
	const LightSampler& sampler_;
	const Scalar nm_;
	const VolumeEmissionPivotState& pivots_;
	MISWeights::LogDensity logDirection_;
	Scalar logBoundarySurvival_;
	MISWeights::LogDensity logEndpointDistance_;
	bool endpointSeen_;
	bool supported_;
};

template<typename Evaluator>
bool AccumulateShadowMediumSegment(
	const Evaluator& evaluator,
	const IMedium& medium,
	const Ray& ray,
	const Scalar dist,
	typename Evaluator::Value& transmittance
	)
{
	const typename Evaluator::Value segment =
		evaluator.Evaluate( medium, ray, dist );
	if( !evaluator.IsValid( segment ) ) {
		GlobalLog()->PrintEasyError(
			"EvaluateShadowMediumTransmittance: medium returned an invalid segment transmittance" );
		return false;
	}
	const typename Evaluator::Value accumulated = transmittance * segment;
	if( !evaluator.IsValid( accumulated ) ) {
		GlobalLog()->PrintEasyError(
			"EvaluateShadowMediumTransmittance: accumulated transmittance became invalid" );
		return false;
	}
	transmittance = accumulated;
	return true;
}

template<typename Evaluator, typename MarchObserver>
bool EvaluateShadowMediumTransmittanceImpl(
	const Ray& ray,
	const Scalar maxDist,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IScene* pScene,
	const bool bSceneHasObjectMedia,
	const Evaluator& evaluator,
	typename Evaluator::Value& outTr,
	const IORStack* pOriginStack,
	const IMedium** pEndpointMedium,
	const ShadowBoundaryPolicy boundaryPolicy,
	MarchObserver& marchObserver
	)
{
	typename Evaluator::Value Tr = evaluator.Identity();
	outTr = evaluator.Zero();
	if( pEndpointMedium ) *pEndpointMedium = 0;
	if( maxDist <= 0 ) {
		outTr = Tr;
		if( pEndpointMedium ) *pEndpointMedium = pOriginMedium;
		return true;
	}
	if( !pScene ) {
		GlobalLog()->PrintEasyError(
			"EvaluateShadowMediumTransmittance: no scene for a positive-length walk" );
		return false;
	}

	const IMedium* pGlobalMedium = pScene->GetGlobalMedium();
	const IObjectManager* pObjects = pScene->GetObjects();

	// Quick exit: no media anywhere
	if( !marchObserver.RequiresGeometry() &&
		boundaryPolicy==eShadowBoundaryIgnoreGeometry &&
		!pOriginMedium && !pGlobalMedium && !bSceneHasObjectMedia ) {
		outTr = Tr;
		return true;
	}

	// Fast path: no per-object media in scene, just apply
	// origin/global medium for the full distance.
	if( !marchObserver.RequiresGeometry() &&
		boundaryPolicy==eShadowBoundaryIgnoreGeometry && !bSceneHasObjectMedia ) {
		const IMedium* pMedium = pOriginMedium ? pOriginMedium : pGlobalMedium;
		if( pMedium ) {
			if( !AccumulateShadowMediumSegment(
				evaluator, *pMedium, ray, maxDist, Tr ) ) return false;
		}
		outTr = Tr;
		if( pEndpointMedium ) *pEndpointMedium = pMedium;
		return true;
	}

	if( !pObjects ) {
		GlobalLog()->PrintEasyError(
			"EvaluateShadowMediumTransmittance: scene reports object media without an object manager" );
		return false;
	}
	ShadowMediumStack stack;
	if( pOriginStack ) {
		std::vector<const IObject*> enclosingObjects;
		pOriginStack->AppendObjectStack( enclosingObjects );
		for( std::vector<const IObject*>::const_iterator i = enclosingObjects.begin();
			i != enclosingObjects.end(); ++i ) {
			const IMedium* pEnclosingMedium = (*i)->GetInteriorMedium();
			// Retain medium-less enclosing objects as sentinels.  MediumTracking
			// resolves an enclosure-stack top without an interior medium to the world
			// medium, not to a lower object's medium; the sentinel preserves that
			// state until its exit restores the next outer entry.
			if( !stack.push( *i, pEnclosingMedium ) ) {
				GlobalLog()->PrintEasyError(
					"EvaluateShadowMediumTransmittance: duplicate object in origin medium stack" );
				return false;
			}
		}
		const IMedium* pResolvedOrigin = stack.top();
		if( !pResolvedOrigin ) pResolvedOrigin = pGlobalMedium;
		if( pResolvedOrigin != pOriginMedium ) {
			GlobalLog()->PrintEasyError(
				"EvaluateShadowMediumTransmittance: origin medium disagrees with supplied stack" );
			return false;
		}
	} else if( pOriginMedium && pOriginMediumObject ) {
		if( !stack.push( pOriginMediumObject, pOriginMedium ) ) {
			GlobalLog()->PrintEasyError(
				"EvaluateShadowMediumTransmittance: duplicate origin-medium object" );
			return false;
		}
	}
	const IMedium* pFallbackMedium = pOriginStack
		? pGlobalMedium
		: (pOriginMedium && !pOriginMediumObject ? pOriginMedium : pGlobalMedium);

	Scalar segStart = 0;
	for( ;; )
	{
		if( !(segStart < maxDist) ) {
			marchObserver.Blocked();
			outTr = Tr;
			if( pEndpointMedium ) {
				*pEndpointMedium = stack.top() ? stack.top() : pFallbackMedium;
			}
			return true;
		}

		// Query from the exact prior boundary.  The geometry layer already
		// rejects the zero-distance root and returns the next representable
		// crossing.  A fixed step-off is not admissible here: it can jump over
		// an arbitrarily thin but otherwise resolvable medium segment.
		const Ray castRay( ray.PointAtLength( segStart ), ray.Dir() );
		const Scalar castMax = maxDist - segStart;
		RasterizerState nullRast = {0};
		RayIntersection ri( castRay, nullRast );
		// Reject only local distances too small to advance the absolute ray
		// parameter by one representable value.  This is a representability
		// bound derived from the current parameter, not a scene-space epsilon.
		ri.geometric.minimumSurfaceRange =
			std::nextafter( segStart, maxDist ) - segStart;
		pObjects->IntersectRay( ri, true, true, true );

		if( !ri.geometric.bHit ) {
			const Scalar remaining = maxDist - segStart;
			const IMedium* pActive = stack.top();
			if( !pActive ) pActive = pFallbackMedium;
			if( pActive && remaining > 0 ) {
				const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
				if( !AccumulateShadowMediumSegment(
					evaluator, *pActive, segRay, remaining, Tr ) ) return false;
			}
			const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
			if( !marchObserver.Accumulate(
				pActive,segRay,remaining,RISE_INFINITY,false,true) ) return false;
			outTr = Tr;
			if( pEndpointMedium ) *pEndpointMedium = pActive;
			return true;
		}
		if( ri.geometric.surfaceRange >= RISE_INFINITY ) {
			GlobalLog()->PrintEasyError(
				"EvaluateShadowMediumTransmittance: hit lacks an exact geometric-boundary range" );
			return false;
		}
		if( ri.geometric.surfaceRange >= castMax ) {
			const Scalar remaining = maxDist - segStart;
			const IMedium* pActive = stack.top();
			if( !pActive ) pActive = pFallbackMedium;
			if( pActive && remaining > 0 ) {
				const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
				if( !AccumulateShadowMediumSegment(
					evaluator, *pActive, segRay, remaining, Tr ) ) return false;
			}
			const Scalar proposalMaxDist = IsExactNullBoundaryMaterial(ri.pMaterial)
				? ri.geometric.surfaceRange : ri.geometric.range;
			const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
			if( !marchObserver.Accumulate(
				pActive,segRay,remaining,proposalMaxDist,true,true) ) return false;
			outTr = Tr;
			if( pEndpointMedium ) *pEndpointMedium = pActive;
			return true;
		}

		const IObject* pHitObj = ri.pObject;
		const Scalar boundaryDist = segStart + ri.geometric.surfaceRange;
		if( !pHitObj || !std::isfinite( boundaryDist ) || !(boundaryDist > segStart) ) {
			GlobalLog()->PrintEx( eLog_Error,
				"EvaluateShadowMediumTransmittance: malformed or non-progressing boundary hit (start %.17g, local %.17g, absolute %.17g)",
				segStart, ri.geometric.range, boundaryDist );
			return false;
		}
		if( boundaryPolicy==eShadowBoundaryExactNullOnly &&
			!IsExactNullBoundaryMaterial(ri.pMaterial) ) {
			// Volume-emission NEE and its march competitor have common support
			// only through exact null boundaries.  Shadow flags and transparent-
			// shadow settings are deliberately irrelevant here: every other
			// interface terminates the originating vertex's strategy family.
			outTr = evaluator.Zero();
			marchObserver.Blocked();
			return true;
		}

		const Scalar segLen = boundaryDist - segStart;
		const IMedium* pActive = stack.top();
		if( !pActive ) pActive = pFallbackMedium;
		if( pActive ) {
			const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
			if( !AccumulateShadowMediumSegment(
				evaluator, *pActive, segRay, segLen, Tr ) ) return false;
		}
		const Ray proposalRay( ray.PointAtLength( segStart ), ray.Dir() );
		if( !marchObserver.Accumulate(
			pActive,proposalRay,segLen,segLen,true,false) ) return false;

		const IMedium* pObjMedium = pHitObj->GetInteriorMedium();
		const Scalar rawFacing = Vector3Ops::Dot(
			ri.geometric.vGeomNormal, ray.Dir() );
		const Scalar ndotd = ri.geometric.bGeomNormalOrientedToRay
			? -rawFacing : rawFacing;
		bool topologyOK = true;
		if( ndotd < 0 ) {
			// A medium-less CLOSED object is a world-medium cavity: retain a
			// null sentinel until its paired exit.  Open non-medium surfaces
			// remain visibility-only and do not alter attenuation topology.
			if( pObjMedium || ri.geometric.surfaceRange2 < RISE_INFINITY ) {
				topologyOK = stack.push( pHitObj, pObjMedium );
			}
		} else if( stack.contains( pHitObj ) || pObjMedium ) {
			topologyOK = stack.remove( pHitObj );
		}
		if( !topologyOK ) {
			GlobalLog()->PrintEasyError(
				"EvaluateShadowMediumTransmittance: inconsistent medium-boundary topology" );
			return false;
		}

		segStart = boundaryDist;
	}
}

bool RISE::Implementation::EvaluateShadowMediumTransmittance(
	const Ray& ray,
	const Scalar maxDist,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IScene* pScene,
	const bool bSceneHasObjectMedia,
	RISEPel& outTr,
	const IORStack* pOriginStack
	)
{
	NoMarchProposalObserver marchObserver;
	return EvaluateShadowMediumTransmittanceImpl(
		ray, maxDist, pOriginMedium, pOriginMediumObject, pScene,
		bSceneHasObjectMedia, ShadowTransmittanceRGB(), outTr, pOriginStack, 0,
		eShadowBoundaryIgnoreGeometry, marchObserver );
}

bool RISE::Implementation::EvaluateShadowMediumTransmittanceNM(
	const Ray& ray,
	const Scalar maxDist,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IScene* pScene,
	const bool bSceneHasObjectMedia,
	const Scalar nm,
	Scalar& outTr,
	const IORStack* pOriginStack
	)
{
	NoMarchProposalObserver marchObserver;
	return EvaluateShadowMediumTransmittanceImpl(
		ray, maxDist, pOriginMedium, pOriginMediumObject, pScene,
		bSceneHasObjectMedia, ShadowTransmittanceNM( nm ), outTr, pOriginStack, 0,
		eShadowBoundaryIgnoreGeometry, marchObserver );
}

static RISEPel EvalShadowTransmittance(
	const Ray& ray,
	const Scalar maxDist,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IScene* pScene,
	const bool bSceneHasObjectMedia,
	const IORStack* pOriginStack
	)
{
	RISEPel Tr;
	EvaluateShadowMediumTransmittance(
		ray, maxDist, pOriginMedium, pOriginMediumObject, pScene,
		bSceneHasObjectMedia, Tr, pOriginStack );
	return Tr;
}

static Scalar EvalShadowTransmittanceNM(
	const Ray& ray,
	const Scalar maxDist,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IScene* pScene,
	const bool bSceneHasObjectMedia,
	const Scalar nm,
	const IORStack* pOriginStack
	)
{
	Scalar Tr;
	EvaluateShadowMediumTransmittanceNM(
		ray, maxDist, pOriginMedium, pOriginMediumObject, pScene,
		bSceneHasObjectMedia, nm, Tr, pOriginStack );
	return Tr;
}

using PathTransportUtilities::PowerHeuristic;

LightSampler::LightSampler() :
  pPreparedScene( 0 ),
  pPreparedLuminaries( 0 ),
  cachedTotalExitance( 0 ),
  positionalLightTotalExitance( 0 ),
  risCandidates( 0 ),
  lightSampleRRThreshold( 0 ),
  bSceneHasObjectMedia( false ),
  bSceneHasObjectFireMedia( false ),
  bSceneHasNullBoundaries( false ),
  volumeEmissionDistributionValid( true ),
  equiangularPivotDistributionValid( true ),
  pLightBVH( 0 ),
  bUseLightBVH( false ),
  pEnvSampler( 0 ),
  pEnvironmentMap( 0 ),
  cachedEnvSelectProb( 0 ),
  cachedSceneCenter( 0, 0, 0 ),
  cachedSceneRadius( 0 ),
  pOptimalMIS( 0 ),
  soloKind( SoloKind::None ),
  soloLight( 0 ),
  soloLuminaryObject( 0 )
{
}

LightSampler::~LightSampler()
{
	if( pEnvSampler ) {
		pEnvSampler->release();
		pEnvSampler = 0;
	}
	// Pre-2026-05 the raw `pEnvironmentMap` was never addref'd,
	// so the destructor only had to nullify the pointer.  Reviewer
	// #2 flagged a real dangling-pointer risk: `Scene::SetGlobalRadianceMap`
	// releases the old map, so a mid-render env-map swap would
	// invalidate `pEnvironmentMap` while workers might still be
	// inside `SampleEnvLightEmission` calling `GetRadiance(...)`.
	// `SetEnvironmentSampler` now addrefs the map; release here.
	if( pEnvironmentMap ) {
		pEnvironmentMap->release();
		pEnvironmentMap = 0;
	}
	delete pLightBVH;
	pLightBVH = 0;
}

void LightSampler::SetUseLightBVH(
	const bool enable
	)
{
	bUseLightBVH = enable;
}

void LightSampler::SetRISCandidates(
	const unsigned int M
	)
{
	risCandidates = M;
}

void LightSampler::SetLightSampleRRThreshold(
	const Scalar threshold
	)
{
	lightSampleRRThreshold = threshold;
}

void LightSampler::SetEnvironmentSampler(
	const IRadianceMap* pEnvMap,
	const EnvironmentSampler* pSampler
	)
{
	if( pEnvSampler ) {
		pEnvSampler->release();
	}
	// Addref `pEnvironmentMap` so it can't go away from under us via
	// `Scene::SetGlobalRadianceMap` swapping the scene-level pointer
	// mid-render (which `release()`s its old map).  Workers inside
	// `SampleEnvLightEmission` call `pEnvironmentMap->GetRadiance(...)`;
	// without the addref a concurrent swap dangles the pointer and
	// crashes.  Same shape as the existing `pEnvSampler` ownership.
	if( pEnvironmentMap ) {
		pEnvironmentMap->release();
	}
	pEnvironmentMap = pEnvMap;
	if( pEnvironmentMap ) {
		pEnvironmentMap->addref();
	}
	pEnvSampler = pSampler;
	if( pEnvSampler ) {
		pEnvSampler->addref();
	}
	RecomputeEnvSelectProbability();
}

void LightSampler::RecomputeEnvSelectProbability()
{
	// Continuous-PMF env selection (2026-05-29 architectural fix —
	// see docs/PRE_PHASE1_STATUS.md Session 9).  Replaces the prior
	// binary 0-or-1 `EnvSelectProbability()` that crowded env out of
	// NEE in mixed scenes — root cause of the documented BDPT/VCM
	// env-IBL deficit (env+omni / env+mesh at ~85 % of PT because env
	// never participated in light-subpath emission or s=1 NEE MIS
	// bookkeeping).  envWeight is the env totalLuminance × scene-disc
	// area — dimensionally comparable to mesh exitance × area.  In
	// env-only scenes (alias table empty) this gives 1.0 — identical
	// to the old binary behaviour.  In env+other-lights scenes it's
	// fractional and env participates via the env-vs-alias roll in
	// `SampleLight()`.  Called from both `SetEnvironmentSampler` and
	// `Prepare` so the cache stays valid regardless of which gets
	// invoked first (RayCaster::AttachScene calls Prepare first, then
	// SetEnvironmentSampler — and the order may differ elsewhere).
	const bool bEnvExists = pEnvSampler && pEnvSampler->IsValid() &&
		pEnvironmentMap && cachedSceneRadius > 0;
	if( bEnvExists ) {
		const Scalar discArea = PI * cachedSceneRadius * cachedSceneRadius;
		const Scalar envWeight = pEnvSampler->TotalLuminance() * discArea;
		const Scalar aliasWeight = static_cast<Scalar>(
			aliasTable.TotalWeight() );
		const Scalar totalWeight = envWeight + aliasWeight;
		cachedEnvSelectProb = ( totalWeight > 0 ) ?
			( envWeight / totalWeight ) : Scalar( 0 );
	} else {
		cachedEnvSelectProb = Scalar( 0 );
	}
}

void LightSampler::Prepare(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries
	)
{
	pPreparedScene = &scene;
	pPreparedLuminaries = &luminaries;

	// Build the combined light table from non-mesh lights and mesh luminaries
	lightEntries.clear();

	// Add non-mesh lights with nonzero exitance.
	// NOTE: we test exitance > 0, NOT CanGeneratePhotons().
	// CanGeneratePhotons() is a photon-mapping flag (controlled by
	// the scene-side "shootphotons" parameter).  A light with
	// shootphotons=FALSE must still participate in direct lighting
	// (NEE) and light-subpath starts (BDPT/MLT).
	const ILightManager* pLightMgr = scene.GetLights();
	if( pLightMgr )
	{
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		ILightManager::LightsList::const_iterator m, n;
		for( m=lights.begin(), n=lights.end(); m!=n; m++ )
		{
			const ILightPriv* l = *m;
			const Scalar exitance = ColorMath::MaxValue( l->radiantExitance() );
			if( exitance > 0 )
			{
				LightEntry entry;
				entry.pLight = l;
				entry.lumIndex = 0;
				entry.exitance = exitance;
				entry.position = l->position();
				lightEntries.push_back( entry );
			}
		}
	}

	// Add mesh luminaries with nonzero exitance.
	// Use a fixed seed point (0.5, 0.5, 0.5) to get a representative
	// surface position for distance estimates in RIS.
	//
	// NOTE: These positions are cached once during Prepare() and not
	// updated per sample.  For scenes with animated/moving emissive
	// geometry (where transforms are re-evaluated per sample in
	// PixelBasedPelRasterizer), the cached positions can go stale.
	// This degrades RIS spatial weighting quality (variance issue,
	// not a correctness bug — the RIS estimator remains unbiased
	// regardless of position accuracy; only the variance reduction
	// suffers).  A future fix could refresh positions per sample or
	// per scanline when animation is detected.
	const Point3 centerSeed( 0.5, 0.5, 0.5 );
	for( unsigned int li = 0; li < luminaries.size(); li++ )
	{
		const IEmitter* pEmitter = luminaries[li].pLum->GetMaterial()->GetEmitter();
		if( pEmitter )
		{
			const Scalar area = luminaries[li].pLum->GetArea();
			const RISEPel power = pEmitter->averageRadiantExitance() * area;
			const Scalar exitance = ColorMath::MaxValue( power );
			if( exitance > 0 )
			{
				LightEntry entry;
				entry.pLight = 0;
				entry.lumIndex = li;
				entry.exitance = exitance;

				// Sample a representative position on the luminary surface
				Point3 repPos;
				Vector3 repNormal;
				Point2 repCoord;
				luminaries[li].pLum->UniformRandomPoint(
					&repPos, &repNormal, &repCoord, centerSeed );
				entry.position = repPos;

				lightEntries.push_back( entry );
			}
		}
	}

	// Build the positional light list for equiangular sampling.
	// Only point and spot lights have meaningful spatial positions.
	positionalLightIndices.clear();
	positionalLightTotalExitance = 0;
	for( unsigned int i = 0; i < lightEntries.size(); i++ )
	{
		if( lightEntries[i].pLight && lightEntries[i].pLight->IsPositionalLight() )
		{
			positionalLightIndices.push_back( i );
			positionalLightTotalExitance += lightEntries[i].exitance;
		}
	}

	// Build alias table from the exitance weights
	std::vector<double> weights( lightEntries.size() );
	for( unsigned int i = 0; i < lightEntries.size(); i++ )
	{
		weights[i] = static_cast<double>( lightEntries[i].exitance );
	}

	aliasTable.Build( weights );
	cachedTotalExitance = static_cast<Scalar>( aliasTable.TotalWeight() );

	// Auto-enable RIS when there are enough lights that spatial
	// selection matters.  With fewer lights, the alias table alone
	// is sufficient and RIS overhead is wasted.
	// (Skipped when light BVH is enabled — BVH provides superior
	// spatial selection with tractable MIS.)
	if( !bUseLightBVH && risCandidates == 0 && lightEntries.size() > 8 )
	{
		risCandidates = 8;
	}

	// Build the light BVH when enabled and there are lights to sample.
	if( bUseLightBVH && lightEntries.size() > 1 )
	{
		delete pLightBVH;
		pLightBVH = new LightBVH();
		pLightBVH->Build( lightEntries, luminaries );
		GlobalLog()->PrintEx( eLog_Info, "LightSampler:: Built light BVH over %d lights (%d nodes)",
			(int)lightEntries.size(), (int)pLightBVH->NumNodes() );
	}
	else
	{
		delete pLightBVH;
		pLightBVH = 0;
	}

	// Scan objects for interior media.  This flag gates the
	// multi-medium shadow transmittance walk — when no objects
	// have media and there's no global medium, shadow transmittance
	// evaluation is skipped entirely.
	bSceneHasObjectMedia = false;
	bSceneHasObjectFireMedia = false;
	bSceneHasNullBoundaries = false;
	volumeEmissionMedia.clear();
	{
		struct MediaScan : public IEnumCallback<IObject>
		{
			bool found;
			bool foundFire;
			bool foundNullBoundary;
			std::vector<const IMedium*> emitters;
			MediaScan() : found(false), foundFire(false), foundNullBoundary(false) {}
			void AddEmitter( const IMedium* medium )
			{
				if( !medium || medium->GetThermalEmissionImportance() <= 0.0 ) return;
				for( std::vector<const IMedium*>::const_iterator i = emitters.begin();
					i != emitters.end(); ++i ) {
					if( *i == medium ) return;
				}
				emitters.push_back( medium );
			}
			bool operator()( const IObject& obj )
			{
				if( IsExactNullBoundaryMaterial( obj.GetMaterial() ) ) {
					foundNullBoundary = true;
				}
				const IMedium* medium = obj.GetInteriorMedium();
				if( medium ) {
					found = true;
					if( medium->IsFireMedium() ) {
						foundFire = true;
					}
					AddEmitter( medium );
				}
				return true;
			}
		};
		MediaScan scan;
		scene.GetObjects()->EnumerateObjects( scan );
		bSceneHasObjectMedia = scan.found;
		bSceneHasObjectFireMedia = scan.foundFire;
		bSceneHasNullBoundaries = scan.foundNullBoundary;
		const IMedium* globalMedium = scene.GetGlobalMedium();
		scan.AddEmitter( globalMedium );
		volumeEmissionMedia.swap( scan.emitters );
	}
	volumeEmissionDistributionValid = true;
	std::vector<double> volumeWeights( volumeEmissionMedia.size(), 0.0 );
	double maxVolumeWeight = 0.0;
	for( unsigned int i = 0; i < volumeEmissionMedia.size(); ++i ) {
		volumeWeights[i] = static_cast<double>(
			volumeEmissionMedia[i]->GetThermalEmissionImportance() );
		if( !RISE::IsFiniteDouble(volumeWeights[i]) || volumeWeights[i] <= 0.0 ) {
			volumeEmissionDistributionValid = false;
		} else {
			if( volumeWeights[i] > maxVolumeWeight ) maxVolumeWeight = volumeWeights[i];
		}
	}
	if( volumeEmissionDistributionValid && !volumeWeights.empty() ) {
		for( unsigned int i = 0; i < volumeWeights.size(); ++i ) {
			volumeWeights[i] /= maxVolumeWeight;
			if( volumeWeights[i] <= 0.0 ||
				!RISE::IsFiniteDouble(volumeWeights[i]) ) {
				volumeEmissionDistributionValid = false;
			}
		}
	}
	if( volumeEmissionDistributionValid ) {
		volumeEmissionAlias.Build( volumeWeights );
		for( unsigned int i = 0; i < volumeWeights.size(); ++i ) {
			const Scalar mediumPdf = static_cast<Scalar>(
				volumeEmissionAlias.Pdf(i) );
			const Scalar minPointPdf =
				volumeEmissionMedia[i]->GetMinimumPositiveThermalEmissionPdf();
			const Scalar minLabeledPdf = mediumPdf * minPointPdf;
			if( mediumPdf <= 0.0 || minPointPdf <= 0.0 ||
				minLabeledPdf <= 0.0 || !RISE::IsFiniteDouble(minLabeledPdf) ) {
				volumeEmissionDistributionValid = false;
				break;
			}
		}
	}
	if( !volumeEmissionDistributionValid ) {
		GlobalLog()->PrintEasyError(
			"LightSampler:: thermal-emission medium weights exceed the representable labeled-density range" );
		volumeEmissionAlias.Build( std::vector<double>() );
	}

	// Build the equiangular pivot mixture in one common watt-dimensioned
	// measure.  Positional lights use their visible-band power; media use
	// A_m=4*pi*s^2*W_m.  Normalize by the largest finite proxy before the
	// alias build so mixed astronomical/tiny powers cannot overflow a sum.
	positionalLightBandPowers.assign( positionalLightIndices.size(), 0.0 );
	volumeEmissionPowerProxies.assign( volumeEmissionMedia.size(), 0.0 );
	std::vector<double> pivotWeights(
		positionalLightIndices.size()+volumeEmissionMedia.size(), 0.0 );
	double maxPivotWeight = 0.0;
	equiangularPivotDistributionValid = volumeEmissionDistributionValid;
	for( unsigned int i = 0; i < positionalLightIndices.size(); ++i ) {
		const LightEntry& entry = lightEntries[positionalLightIndices[i]];
		const Scalar power = entry.pLight ? entry.pLight->EstimateVisibleBandPower() : 0.0;
		positionalLightBandPowers[i] = power;
		if( !RISE::IsFiniteDouble(power) || power < 0.0 ) {
			equiangularPivotDistributionValid = false;
		} else {
			pivotWeights[i] = static_cast<double>(power);
			if( pivotWeights[i] > maxPivotWeight ) maxPivotWeight = pivotWeights[i];
		}
	}
	for( unsigned int i = 0; i < volumeEmissionMedia.size(); ++i ) {
		const Scalar power = volumeEmissionMedia[i]->GetThermalEmissionPowerProxy();
		volumeEmissionPowerProxies[i] = power;
		const unsigned int entry = static_cast<unsigned int>(
			positionalLightIndices.size()) + i;
		if( !RISE::IsFiniteDouble(power) || power <= 0.0 ) {
			equiangularPivotDistributionValid = false;
		} else {
			pivotWeights[entry] = static_cast<double>(power);
			if( pivotWeights[entry] > maxPivotWeight ) maxPivotWeight = pivotWeights[entry];
		}
	}
	if( maxPivotWeight <= 0.0 ) {
		pivotWeights.clear();
	} else if( equiangularPivotDistributionValid ) {
		for( unsigned int i = 0; i < pivotWeights.size(); ++i ) {
			const bool hadPositiveWeight = pivotWeights[i] > 0.0;
			pivotWeights[i] /= maxPivotWeight;
			if( hadPositiveWeight && pivotWeights[i] <= 0.0 ) {
				equiangularPivotDistributionValid = false;
			}
		}
	}
	if( equiangularPivotDistributionValid ) {
		equiangularPivotAlias.Build( pivotWeights );
	} else {
		equiangularPivotAlias.Build( std::vector<double>() );
		GlobalLog()->PrintEasyError(
			"LightSampler:: equiangular pivot powers are invalid" );
	}

	// Compute scene bounding sphere from visible objects' world AABBs.
	// Used by `SampleEnvLightEmission` to place the env-light emission
	// disc outside the scene.  Cheap one-time enumeration here is the
	// right surface — re-computing per sample would be wasteful.
	//
	// Threading: `Prepare()` runs once during `RayCaster::AttachScene`
	// before any worker thread enters render mode, so cachedSceneCenter
	// / cachedSceneRadius are written from a single thread and then
	// read read-only by `SampleEnvLightEmission`.
	{
		struct SceneBBoxScan : public IEnumCallback<IObject>
		{
			BoundingBox bbox;
			bool seen;
			SceneBBoxScan() :
				bbox( Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY ),
					  Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY ) ),
				seen( false )
			{}
			bool operator()( const IObject& obj )
			{
				if( obj.IsWorldVisible() ) {
					const BoundingBox b = obj.getBoundingBox();
					bbox.Include( b.ll );
					bbox.Include( b.ur );
					seen = true;
				}
				return true;
			}
		};
		SceneBBoxScan scan;
		scene.GetObjects()->EnumerateObjects( scan );
		if( scan.seen ) {
			cachedSceneCenter = scan.bbox.GetCenter();
			const Vector3 extents = scan.bbox.GetExtents();
			// Radius of the *enclosing* sphere = half the AABB diagonal
			// length.  The disc emitter sits at distance `radius` along
			// the sampled sky direction; making the disc this large
			// guarantees every emission ray that points back toward the
			// scene will physically cross the bounding sphere and have
			// a chance to hit geometry — a smaller radius would clip
			// some valid grazing-angle rays.
			//
			// Reviewer #2 flagged: scenes with `infinite_plane_geometry`
			// can produce `±RISE_INFINITY` AABB corners → the magnitude
			// here becomes `inf`, which then propagates through the
			// disc-position math into NaN ray origins, and finally into
			// `IObjectManager::IntersectRay` which is not defined on
			// non-finite origins (RISE_BVH's float-precision slab test
			// happily accepts NaN comparisons that always return false,
			// dropping all hits silently).  Clamp to a generous but
			// finite cap (1e6 world units = 1000 km if metres) so
			// pathological scenes degrade to "env-NEE works through PT
			// but BDPT/VCM emission may be slightly under-sampled" rather
			// than to NaN-corrupted renders.
			const Scalar rawRadius = Scalar( 0.5 ) * Vector3Ops::Magnitude( extents );
			const Scalar kRadiusCap = Scalar( 1.0e6 );
			cachedSceneRadius =
				( !RISE::IsFiniteDouble( rawRadius ) || rawRadius > kRadiusCap )
					? kRadiusCap
					: rawRadius;
			// Also defend the centre: an infinite-extent object can
			// land ll = -INF and ur = +INF, whose midpoint is NaN.
			if( !RISE::IsFiniteDouble( cachedSceneCenter.x ) ||
				!RISE::IsFiniteDouble( cachedSceneCenter.y ) ||
				!RISE::IsFiniteDouble( cachedSceneCenter.z ) )
			{
				cachedSceneCenter = Point3( 0, 0, 0 );
			}
		} else {
			cachedSceneCenter = Point3( 0, 0, 0 );
			cachedSceneRadius = Scalar( 0 );
		}
	}

	// Continuous-PMF env selection — see `RecomputeEnvSelectProbability`.
	// Called here at end of Prepare so the cache reflects the freshly-
	// built alias table; SetEnvironmentSampler also calls it so the
	// cache is correct regardless of Prepare/SetEnvironmentSampler order.
	RecomputeEnvSelectProbability();
}

//
// RIS light selection with self-exclusion
//
// Draws M candidates from the global alias table (proposal q)
// and resamples one proportional to a spatially-aware target
// weight: w_i = exitance_i / max(dist_i^2, epsilon).
//
// When selfIdx >= 0, that entry's resampling weight is forced
// to zero so self-illumination is excluded from selection
// without wasting the sample.
//
// Returns two values:
//   pdfAlias   = alias-table PDF q(j) (for estimator weight)
//   risWeight  = RIS correction: (1/M) * sum(W_i) / W_j
//
// The caller's estimator should be:
//   result = integrand(j) * risWeight / pdfAlias
//
// When RIS is active, MIS with BSDF sampling is disabled
// (w_nee = 1) because the exact finite-M technique density
// is intractable.
//

unsigned int LightSampler::SelectLightRIS(
	const Point3& shadingPoint,
	ISampler& sampler,
	Scalar& pdfAlias,
	Scalar& risWeight,
	const int selfIdx
	) const
{
	const unsigned int M = risCandidates;
	const unsigned int N = static_cast<unsigned int>( lightEntries.size() );

	// Clamp M to available lights and stack array size
	const unsigned int numCandidates = r_min( r_min( M, N ), 64u );

	// Draw M candidates and compute resampling weights.
	// If a candidate matches selfIdx, its weight is zeroed
	// so it can never be selected.
	unsigned int candidates[64];
	Scalar resamplingWeights[64];
	Scalar totalWeight = 0;

	const Scalar minDistSq = 1e-4;

	for( unsigned int c = 0; c < numCandidates; c++ )
	{
		const unsigned int idx = aliasTable.Sample( sampler.Get1D() );
		candidates[c] = idx;

		if( static_cast<int>(idx) == selfIdx )
		{
			resamplingWeights[c] = 0;
			continue;
		}

		const LightEntry& entry = lightEntries[idx];
		const Scalar proposal = static_cast<Scalar>( aliasTable.Pdf( idx ) );

		const Vector3 toLight = Vector3Ops::mkVector3(
			entry.position, shadingPoint );
		const Scalar distSq = r_max( Vector3Ops::Dot( toLight, toLight ), minDistSq );
		const Scalar target = entry.exitance / distSq;

		const Scalar W = (proposal > 0) ? (target / proposal) : 0;
		resamplingWeights[c] = W;
		totalWeight += W;
	}

	if( totalWeight <= 0 )
	{
		// All candidates were self or had zero weight — signal
		// failure to the caller.  Return N (out-of-bounds sentinel).
		pdfAlias = 1.0;
		risWeight = 0.0;
		return N;
	}

	// Select one candidate proportional to resampling weights
	Scalar xi = sampler.Get1D() * totalWeight;
	unsigned int selected = candidates[numCandidates - 1];
	Scalar selectedWeight = resamplingWeights[numCandidates - 1];

	for( unsigned int c = 0; c < numCandidates; c++ )
	{
		xi -= resamplingWeights[c];
		if( xi <= 0 )
		{
			selected = candidates[c];
			selectedWeight = resamplingWeights[c];
			break;
		}
	}

	// Alias-table PDF (for estimator weight)
	pdfAlias = static_cast<Scalar>( aliasTable.Pdf( selected ) );

	// RIS correction factor: risWeight = (1/M) * sum(W_i) / W_j
	//
	// With self excluded, sum(W_i) only accumulates non-self
	// candidates, and W_j > 0 (self is never selected).
	// The estimator f(j) * risWeight / q(j) is unbiased for
	// sum_{k != self} f(k) because:
	//   E[(1/M) * sum(W_i)] = sum_{k != self} target(k)
	risWeight = (selectedWeight > 0)
		? (totalWeight / (static_cast<Scalar>(numCandidates) * selectedWeight))
		: 1.0;

	return selected;
}

bool LightSampler::SampleLight(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	ISampler& sampler,
	LightSample& sample
	) const
{
	// Initialize the sample
	sample.pLight = 0;
	sample.pLuminary = 0;
	sample.pEnvLight = 0;
	sample.isDelta = false;
	sample.Le = RISEPel( 0, 0, 0 );
	sample.pdfPosition = 0;
	sample.pdfDirection = 0;
	sample.pdfSelect = 0;

	const bool bEnvExists = pEnvSampler && pEnvSampler->IsValid() &&
		pEnvironmentMap && cachedSceneRadius > 0;
	const bool bAliasValid = aliasTable.IsValid();

	if( !bEnvExists && !bAliasValid ) {
		return false;
	}

	// Single uniform random for env-vs-alias selection (continuous-PMF
	// architectural fix 2026-05-29 — see Prepare()'s block + docs/
	// PRE_PHASE1_STATUS.md Session 9).  Re-mapped into either the env-
	// direction's u1 sub-interval (env path) or the alias-table's u
	// sub-interval (alias path); net Get1D() consumption per call is
	// identical to the prior binary-PMF flow, preserving Sobol / QMC
	// dimension layout (the 2026-05-26 attempt added env as an alias-
	// table entry and consumed an extra Get1D() — caused a severe
	// spectral env-only regression that this wrapper avoids).
	const Scalar u_top = sampler.Get1D();

	const bool bChooseEnv = bEnvExists &&
		( !bAliasValid || u_top < cachedEnvSelectProb );

	if( bChooseEnv ) {
		// Re-map u_top into [0, 1) for env-direction's u1.  In env-
		// only scenes cachedEnvSelectProb == 1.0 and u_env == u_top
		// (identity, no information loss).  In mixed scenes the
		// re-map stretches the [0, envSelectProb) sub-interval back
		// to [0, 1) so the env importance sampler sees a properly-
		// distributed uniform input.
		const Scalar denom = ( cachedEnvSelectProb > 0 ) ?
			cachedEnvSelectProb : Scalar( 1 );
		const Scalar u_env = u_top / denom;
		if( !SampleEnvLightEmission( u_env, sampler, sample ) ) {
			return false;
		}
		// Overwrite the env-only-default pdfSelect with the actual
		// global selection probability (continuous PMF in mixed
		// scenes; 1.0 in env-only).
		sample.pdfSelect = cachedEnvSelectProb;
		return true;
	}

	// Alias-table path.  Re-map [envSelectProb, 1) -> [0, 1) for the
	// alias-table selection draw.  In env-not-exists scenes u_top is
	// the alias draw directly (cachedEnvSelectProb == 0 -> identity).
	const Scalar aliasSpan = Scalar( 1 ) - cachedEnvSelectProb;
	const Scalar u_alias = ( aliasSpan > 0 ) ?
		( ( u_top - cachedEnvSelectProb ) / aliasSpan ) :
		u_top;
	const unsigned int idx = aliasTable.Sample( u_alias );
	const LightEntry& entry = lightEntries[idx];
	// pdfSelect is the JOINT probability of (alias path was taken) ×
	// (alias table selected this entry).  In env-not-exists scenes
	// the first factor is 1.0; in mixed scenes it's (1 - envSelectProb)
	// so total selection probability across env + alias-table sums to 1.
	sample.pdfSelect = aliasSpan *
		static_cast<Scalar>( aliasTable.Pdf( idx ) );

	if( entry.pLight )
	{
		// Non-mesh (delta) light
		const ILightPriv* l = entry.pLight;
		sample.pLight = l;
		sample.isDelta = true;

		// Generate a random photon using the light's own method
		const Point3 ptrand(
			sampler.Get1D(),
			sampler.Get1D(),
			sampler.Get1D()
			);
		const Ray photonRay = l->generateRandomPhoton( ptrand );

		sample.position = photonRay.origin;
		sample.direction = photonRay.Dir();
		sample.normal = photonRay.Dir();

		// Emitted radiance in the sampled direction
		sample.Le = l->emittedRadiance( photonRay.Dir() );

		// For delta-position lights, pdfPosition = 1
		sample.pdfPosition = 1.0;

		// Query the light's own directional PDF
		sample.pdfDirection = l->pdfDirection( photonRay.Dir() );
	}
	else
	{
		// Mesh luminary
		const LuminaryManager::LUM_ELEM& lumEntry = (*pPreparedLuminaries)[entry.lumIndex];
		sample.pLuminary = lumEntry.pLum;
		sample.isDelta = false;

		const IEmitter* pEmitter = lumEntry.pLum->GetMaterial()->GetEmitter();
		const Scalar area = lumEntry.pLum->GetArea();

		// Sample a uniform random point on the luminary surface
		const Point3 prand(
			sampler.Get1D(),
			sampler.Get1D(),
			sampler.Get1D()
			);
		Point2 coord;
		lumEntry.pLum->UniformRandomPoint(
			&sample.position,
			&sample.normal,
			&coord,
			prand
			);

		// pdfPosition = 1 / area (uniform sampling on surface)
		sample.pdfPosition = (area > 0) ? (Scalar(1.0) / area) : 0;

		// Build an orthonormal basis around the surface normal
		// and sample a cosine-weighted hemisphere direction
		OrthonormalBasis3D onb;
		onb.CreateFromW( sample.normal );

		const Point2 dirRand = sampler.Get2D();
		sample.direction = GeometricUtilities::CreateDiffuseVector( onb, dirRand );

		// pdfDirection = cos(theta) / pi for cosine-weighted hemisphere
		const Scalar cosTheta = Vector3Ops::Dot( sample.direction, sample.normal );
		sample.pdfDirection = (cosTheta > 0) ? (cosTheta * INV_PI) : 0;

		// Compute emitted radiance at this point in this direction
		RayIntersectionGeometric rig( Ray( sample.position, sample.direction ), nullRasterizerState );
		rig.vNormal = sample.normal;
		// sample.normal is geometric (UniformRandomPoint on luminary
		// mesh; no Phong/bump on emitter surfaces); mirror.
		rig.vGeomNormal = sample.normal;
		rig.ptCoord = coord;
		rig.onb = onb;

		sample.Le = pEmitter->emittedRadiance( rig, sample.direction, sample.normal );
	}

	return true;
}

//
// SampleEnvLightEmission
//
// PBRT-style infinite-area-light emission sampling for IBL-only scenes
// (no explicit luminaries; only world Environment Texture).  Lets
// BDPT/VCM/MLT generate light subpaths from the HDRI; without it,
// `SampleLight` returns false and the rasterizer produces fully-black
// renders on env-only scenes.
//
// Coordinate convention used here:
//
//   `wi` is the world-space direction FROM the scene TOWARD the sky
//   (i.e. what `EnvironmentSampler::Sample` returns — "looking at the
//   env map from inside the scene").
//
//   The light emission direction is the inverse: emission flows FROM
//   the sky INTO the scene, so `sample.direction = -wi`.
//
//   The emission "disc" sits on the scene's bounding sphere on the
//   `+wi` side (between the scene and the sky in that direction), with
//   its normal pointing back into the scene (= -wi).  This gives
//   `cosAtLight = |dot(direction, normal)| = |dot(-wi, -wi)| = 1`,
//   simplifying BDPT's `Le * cosAtLight / pdfEmit` accumulation.
//
// PDF derivations:
//
//   pdfDirection = pEnvSampler->Pdf(wi)              [solid-angle, sr⁻¹]
//   pdfPosition  = 1 / (π * r²)                      [area density on disc]
//   pdfSelect    = 1                                 [env is the only light]
//
// Throughput sanity-check:
//
//   For a uniform-radiance env map L_env, a disc emitter of area πr²
//   emits total power Φ = L_env · π · (4π) = 4π² · L_env · r²····
//   approximately, modulo cosine factors at the receiving surface.
//   The PT estimate of irradiance at a point inside the scene tends
//   to L_env · π (Lambertian-receiver normalisation), and the s>=2
//   BDPT estimate via this emission sampling integrates to the same
//   value — verified at low spp by comparing PT-only vs BDPT renders
//   of a single Lambertian sphere under a uniform-radiance env map.
//
// Returns false when the env sampler is missing / invalid or the
// scene has no geometry to bound (cachedSceneRadius == 0).  Caller
// (`SampleLight`) then returns false too and the rasterizer skips
// this light subpath.
bool LightSampler::SampleEnvLightEmission(
	const Scalar u1,
	ISampler& sampler,
	LightSample& sample
	) const
{
	if( !pEnvSampler || !pEnvironmentMap || cachedSceneRadius <= 0 ) {
		return false;
	}

	// 1. Importance-sample a sky direction from the env map.
	//    `u1` is externally-supplied (re-mapped from the env-vs-alias
	//    selection roll in `SampleLight`); `u2` is fresh from sampler.
	//    Net Get1D() consumption per `SampleLight` is preserved.
	const Scalar u2 = sampler.Get1D();
	Vector3 wi;
	Scalar pdfDir = 0;
	pEnvSampler->Sample( u1, u2, wi, pdfDir );

	if( pdfDir <= 0 ) {
		return false;
	}

	// `wi` is unit (the env sampler guarantees this).  Re-normalise
	// as a safety net against any future env-sampler regression
	// (cheap, one sqrt).
	wi = Vector3Ops::Normalize( wi );

	// 2. Build an orthonormal basis around wi for disc sampling.
	OrthonormalBasis3D onb;
	onb.CreateFromW( wi );

	// 3. Sample uniform point on a disc of radius cachedSceneRadius
	//    perpendicular to wi, then offset the disc centre by
	//    `+cachedSceneRadius * wi` so the disc sits on the *sky-side*
	//    of the bounding sphere (the side that `wi` points toward
	//    from the scene centre).  Combined with the emission
	//    direction `-wi`, rays travel from the sky-side disc back
	//    through the scene — opposite of "PBRT's far-side disc"
	//    convention some references use, but consistent with
	//    `wi` here being "the direction FROM the scene TOWARD the
	//    sky" (matches `EnvironmentSampler::Sample`'s output).
	//    Adversarial review #1 misread the convention — we keep
	//    `+wi` here; empirically a `-wi` flip puts the disc on the
	//    opposite side of the scene and emission rays travel away
	//    from geometry, producing all-black renders.
	const Scalar diskU = sampler.Get1D();
	const Scalar diskV = sampler.Get1D();
	const Scalar r = cachedSceneRadius * std::sqrt( diskU );
	const Scalar phi = TWO_PI * diskV;
	const Vector3 discOffset =
		onb.u() * ( r * std::cos( phi ) ) +
		onb.v() * ( r * std::sin( phi ) );
	const Point3 discCentre(
		cachedSceneCenter.x + wi.x * cachedSceneRadius,
		cachedSceneCenter.y + wi.y * cachedSceneRadius,
		cachedSceneCenter.z + wi.z * cachedSceneRadius );
	sample.position = Point3(
		discCentre.x + discOffset.x,
		discCentre.y + discOffset.y,
		discCentre.z + discOffset.z );

	// 4. Disc emits in direction -wi (back into the scene).  Disc
	//    normal also points into the scene (= -wi).
	sample.normal = Vector3( -wi.x, -wi.y, -wi.z );
	sample.direction = sample.normal;  // same vector, by construction

	// 5. Sample emitted radiance from the env map in direction wi.
	//    `IRadianceMap::GetRadiance` takes a Ray; we build a synthetic
	//    one pointing into the sky from `sample.position`.  The
	//    rasterizer state isn't needed for env-map lookups (no LOD /
	//    differentials).
	RasterizerState nullRast = {0};
	Ray skyProbe( sample.position, wi );
	sample.Le = pEnvironmentMap->GetRadiance( skyProbe, nullRast );

	// 6. PDFs (see method-header comment).
	const Scalar discArea = PI * cachedSceneRadius * cachedSceneRadius;
	sample.pdfPosition = ( discArea > 0 ) ? ( Scalar( 1 ) / discArea ) : Scalar( 0 );
	sample.pdfDirection = pdfDir;
	sample.pdfSelect = 1.0;

	// 7. Vertex-typing: env-light has no IObject / ILight backing.
	//    BDPT connection strategies that branch on `pLight` /
	//    `pLuminary` (see BDPTIntegrator.cpp s>=1 connect-to-light
	//    code) fall through and contribute zero — that's acceptable
	//    here because direct env-NEE on eye vertices still flows
	//    through the standard env-sampler path, so the dropped s>=1
	//    contribution doesn't cause a NEE blind spot; it just means
	//    the s>=1 light-subpath strategy is under-represented in MIS
	//    weights for env-light paths.  The s>=2 strategies (which
	//    don't touch pLight / pLuminary) work fully and are what
	//    unblocks the IBL-only render from showing as fully black.
	sample.pLight = 0;
	sample.pLuminary = 0;
	sample.pEnvLight = pEnvironmentMap;  // marks this as env-emission for the NM path
	sample.isDelta = false;

	return true;
}

Scalar LightSampler::PdfSelectLight(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const ILight& light,
	const Point3& shadingPoint,
	const Vector3& shadingNormal
	) const
{
	// Find the matching entry in the light table
	for( unsigned int i = 0; i < lightEntries.size(); i++ )
	{
		if( lightEntries[i].pLight == &light )
		{
			// Continuous-PMF rescale (2026-05-29 follow-up).
			// `SampleLight()` returns env with probability
			// `cachedEnvSelectProb` and otherwise samples the alias
			// table; for alias-table entries the joint pdfSelect is
			// `(1 - cachedEnvSelectProb) × aliasTable.Pdf(i)`.  The
			// query methods MUST mirror that scaling, otherwise MIS
			// weights that compare "what would the alternative NEE
			// strategy have produced" against the actual SampleLight
			// pdf are inconsistent — and VCM env+mesh over-counts at
			// 128% of PT (Session 9 follow-up bug).  The LightBVH
			// branch needs the same factor for the same reason.
			const Scalar aliasShare = Scalar( 1 ) - cachedEnvSelectProb;
			if( pLightBVH && pLightBVH->IsBuilt() )
			{
				return aliasShare *
					pLightBVH->Pdf( i, shadingPoint, shadingNormal );
			}
			return aliasShare *
				static_cast<Scalar>( aliasTable.Pdf( i ) );
		}
	}

	return 0;
}

Scalar LightSampler::PdfSelectLuminary(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const IObject& luminary,
	const Point3& shadingPoint,
	const Vector3& shadingNormal
	) const
{
	if( !pPreparedLuminaries )
	{
		return 0;
	}

	// Find the matching entry in the light table
	for( unsigned int i = 0; i < lightEntries.size(); i++ )
	{
		if( !lightEntries[i].pLight )
		{
			if( (*pPreparedLuminaries)[lightEntries[i].lumIndex].pLum == &luminary )
			{
				// Continuous-PMF rescale — see `PdfSelectLight` above
				// for the full rationale.
				const Scalar aliasShare = Scalar( 1 ) - cachedEnvSelectProb;
				if( pLightBVH && pLightBVH->IsBuilt() )
				{
					return aliasShare *
						pLightBVH->Pdf( i, shadingPoint, shadingNormal );
				}
				return aliasShare *
					static_cast<Scalar>( aliasTable.Pdf( i ) );
			}
		}
	}

	return 0;
}

int LightSampler::FindLuminaryIndex(
	const IObject* pLuminary
	) const
{
	if( !pLuminary || !pPreparedLuminaries )
	{
		return -1;
	}

	for( unsigned int i = 0; i < lightEntries.size(); i++ )
	{
		if( !lightEntries[i].pLight &&
			(*pPreparedLuminaries)[lightEntries[i].lumIndex].pLum == pLuminary )
		{
			return static_cast<int>( i );
		}
	}

	return -1;
}

Scalar LightSampler::CachedPdfSelectLuminary(
	const IObject& luminary,
	const Point3& shadingPoint,
	const Vector3& shadingNormal
	) const
{
	if( !pPreparedLuminaries )
	{
		return 0;
	}

	for( unsigned int i = 0; i < lightEntries.size(); i++ )
	{
		if( !lightEntries[i].pLight )
		{
			if( (*pPreparedLuminaries)[lightEntries[i].lumIndex].pLum == &luminary )
			{
				// LIGHT-SOLO: the soloed luminary's selection pdf is 1.0
				// (NEE's Step-2 bypass uses pdfAlias=1.0 too — see
				// EvaluateDirectLighting's solo branch) so the integrator's
				// BSDF-hit MIS partner agrees with NEE's own selection
				// distribution.  A non-target luminary's emission is
				// suppressed to black entirely by the integrator's PART-1
				// solo gate (PathTracingIntegrator.cpp) before this pdf is
				// even consulted, so its exact value here is moot — this
				// branch still returns the honest alias/BVH pdf below for
				// callers that query it directly (diagnostics, tests).
				if( soloKind != SoloKind::None )
				{
					return IsSoloTargetLuminary( &luminary ) ? Scalar( 1.0 ) : Scalar( 0.0 );
				}

				// ALIAS-ONLY selection pdf — do NOT apply the
				// (1 - cachedEnvSelectProb) continuous-PMF factor here.
				// This is PT's BSDF-hit MIS competitor for a MESH-emitter
				// hit, and PT direct lighting samples mesh emitters from the
				// alias table INDEPENDENTLY (EvaluateDirectLighting: pdfAlias
				// = aliasTable.Pdf / pLightBVH->Pdf, alias-only) with the
				// environment handled by a SEPARATE env-NEE strategy.  The
				// matching NEE-side weight (`p_light` at the mesh-luminary
				// site below) uses that same alias-only pdfAlias, so the
				// BSDF-hit `p_nee` (PathTracingIntegrator) must too.  The joint
				// env-vs-alias factor belongs ONLY to SampleLight()'s single
				// selection roll consumed by BDPT/VCM (PdfSelectLight) —
				// applying it here made the two PT MIS techniques disagree by
				// (1-envSelectProb), overweighting mesh-emitter hits whenever
				// an HDRI coexists with mesh emitters.  Codex review Finding 3.
				if( pLightBVH && pLightBVH->IsBuilt() )
				{
					return pLightBVH->Pdf( i, shadingPoint, shadingNormal );
				}
				return static_cast<Scalar>( aliasTable.Pdf( i ) );
			}
		}
	}

	return 0;
}

//
// Unified direct lighting evaluation
//
// Selects one light with nonzero exitance and evaluates its
// shadowed, BRDF-weighted contribution.
//
// SELF-EXCLUSION:
// When the shading object is a luminary in the light table,
// it is excluded from selection.  For RIS this is done by
// zeroing the self entry's resampling weight.  For the alias
// table a rejection draw is used with a (1-p_self) correction.
//
// MIS:
// - Delta lights (point/spot): w = 1 (no alternative strategy).
// - Area lights, RIS OFF: power heuristic using the alias-table
//   selection PDF (converted to solid angle) vs BSDF PDF.
//   CachedPdfSelectLuminary returns the same alias-table PDF
//   on the BSDF-hit side in PathTracingShaderOp.
// - Area lights, RIS ON: w = 1 (no MIS).  The exact finite-M
//   technique density is intractable, so the BSDF-hit emitter
//   contribution is suppressed in PathTracingShaderOp instead.
//

RISEPel LightSampler::EvaluateDirectLighting(
	const RayIntersectionGeometric& ri,
	const IBSDF& brdf,
	const IMaterial* pMaterial,
	const IRayCaster& caster,
	ISampler& sampler,
	const IObject* pShadingObject,
	const IMedium* pMedium,
	const bool isVolumeScatter,
	const IObject* pMediumObject,
	const IORStack* pMediumStack
	) const
{
	RISEPel result( 0, 0, 0 );

	if( !pPreparedScene )
	{
		return result;
	}

	const ILightManager* pLightMgr = pPreparedScene->GetLights();

	// ----------------------------------------------------------------
	// Step 1: Deterministic evaluation of lights with zero exitance
	// (ambient, directional).  These cannot participate in
	// proportional selection.
	// ----------------------------------------------------------------
	if( pLightMgr )
	{
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		ILightManager::LightsList::const_iterator m, n;
		for( m=lights.begin(), n=lights.end(); m!=n; m++ )
		{
			const ILightPriv* l = *m;
			// LIGHT-SOLO: skip every light that is not the soloed
			// identity.  A soloed non-mesh light with nonzero exitance
			// falls through this `continue` and is picked up by the
			// Step-2 bypass below instead; a soloed zero-exitance light
			// (ambient/directional) is evaluated right here, exactly as
			// it would be with no lights at all besides it.
			if( soloKind != SoloKind::None && !IsSoloTargetLight( l ) )
			{
				continue;
			}
			const Scalar exitance = ColorMath::MaxValue( l->radiantExitance() );
			if( exitance <= 0 )
			{
				RISEPel amount( 0, 0, 0 );
				l->ComputeDirectLighting( ri, caster, brdf,
					pShadingObject ? pShadingObject->DoesReceiveShadows() : true,
					amount );
				result = result + amount;
			}
		}
	}

	// ----------------------------------------------------------------
	// Step 2: Select one light with nonzero exitance, excluding self.
	// The light-table block is wrapped in do/while(false) so that
	// early exits (empty table, self-hit, RR termination) break out
	// to the environment NEE below rather than returning.
	// ----------------------------------------------------------------
	do {
		// Find self in the light table (for exclusion) -- needed by
		// every branch below, including the solo bypass.
		const int selfIdx = FindLuminaryIndex( pShadingObject );

		unsigned int idx;
		Scalar pdfAlias;
		Scalar risWeight = 1.0;

		if( soloKind != SoloKind::None )
		{
			// LIGHT-SOLO bypass: skip BVH/RIS/alias-table selection
			// entirely.  Directly locate the soloed entry (if any) and
			// evaluate it with selection pdf = 1.0 -- CachedPdfSelectLuminary
			// returns the SAME 1.0 for this identity (see its solo branch)
			// so NEE and the integrator's BSDF-hit MIS partner agree, which
			// is what keeps the combined estimator unbiased under solo.  A
			// solo target that is NOT in lightEntries (a zero-exitance
			// light, already handled by Step 1 above; or an Environment
			// target) has no entry here -- break to env NEE.
			int soloIdx = -1;
			if( soloKind == SoloKind::Light )
			{
				for( unsigned int i = 0; i < lightEntries.size(); i++ )
				{
					if( lightEntries[i].pLight == soloLight )
					{
						soloIdx = static_cast<int>( i );
						break;
					}
				}
			}
			else if( soloKind == SoloKind::Luminary )
			{
				soloIdx = FindLuminaryIndex( soloLuminaryObject );
			}
			// SoloKind::Environment: soloIdx stays -1, handled entirely by
			// the env-NEE block below.

			if( soloIdx < 0 || soloIdx == selfIdx )
			{
				break;
			}

			idx = static_cast<unsigned int>( soloIdx );
			pdfAlias = 1.0;
			risWeight = 1.0;
		}
		else if( !aliasTable.IsValid() )
		{
			break;
		}
		else if( pLightBVH && pLightBVH->IsBuilt() )
		{
			// Light BVH: importance-weighted selection with full MIS.
			Scalar bvhPdf;
			idx = pLightBVH->Sample( ri.ptIntersection, ri.vNormal,
				sampler.Get1D(), bvhPdf );

			if( bvhPdf <= 0 || static_cast<int>(idx) == selfIdx )
			{
				// Zero PDF means no light has importance at this
				// shading point (e.g. all spotlights point away).
				// Treat identically to self-exclusion: consume the
				// remaining random numbers and skip to env NEE.
				sampler.Get1D();
				sampler.Get1D();
				sampler.Get1D();
				break;
			}

			pdfAlias = bvhPdf;
		}
		else if( risCandidates > 0 )
		{
			// RIS with self excluded via zeroed resampling weight.
			// Returns N (out-of-bounds) when every candidate is self.
			idx = SelectLightRIS( ri.ptIntersection, sampler, pdfAlias, risWeight, selfIdx );

			if( idx >= static_cast<unsigned int>( lightEntries.size() ) )
			{
				// All RIS candidates were self — consume the 3 random
				// numbers that the area-light path would use (sampler
				// dimension alignment) and break to env NEE.
				sampler.Get1D();
				sampler.Get1D();
				sampler.Get1D();
				break;
			}
		}
		else
		{
			// Single alias-table draw with exact self-exclusion.
			//
			// Draw one sample.  If it is self, return zero for the
			// light-table contribution.  No retry loop, no correction
			// factor needed.
			//
			// Proof of unbiasedness:
			//   E[estimator] = sum_{j!=self} p(j) * f(j)/p(j) + p_self * 0
			//                = sum_{j!=self} f(j)
			// which is exactly the self-excluded integral we want.
			//
			// The only caveat is higher variance than rejection sampling
			// (a fraction p_self of samples are wasted), but this is
			// exact for any p_self and requires only one random number.
			idx = aliasTable.Sample( sampler.Get1D() );

			if( static_cast<int>(idx) == selfIdx )
			{
				// Self-hit: consume the 3 random numbers that the
				// area-light path would have used (sampler dimension
				// alignment) and break to env NEE.
				sampler.Get1D();
				sampler.Get1D();
				sampler.Get1D();
				break;
			}

			pdfAlias = static_cast<Scalar>( aliasTable.Pdf( idx ) );
		}

		const LightEntry& entry = lightEntries[idx];

		if( entry.pLight )
		{
			// Selected a non-mesh (delta) light.
			// w = 1 (no alternative sampling strategy).
			// Computed inline (not via ComputeDirectLighting) so that
			// volume scatter points skip hemisphere rejection and
			// cosine weighting — matching the NM path.
			const Point3 lightPos = entry.pLight->position();
			Vector3 vToLight = Vector3Ops::mkVector3( lightPos, ri.ptIntersection );
			const Scalar dist = Vector3Ops::NormalizeMag( vToLight );
			const Scalar cosSurface = isVolumeScatter ? Scalar(1.0) :
				Vector3Ops::Dot( vToLight, ri.vNormal );

			if( !isVolumeScatter && cosSurface <= 0 ) break;

			// Shadow test.  shadowT carries the Fresnel transmittance
			// when transparent shadows are enabled (else 1,1,1).
			RISEPel shadowT( 1.0, 1.0, 1.0 );
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				if( ShadowOccludedRGB( caster, rayToLight, dist - 0.001, shadowT ) )
					break;
			}

			// emittedRadiance expects the outgoing direction FROM the
			// light; -vToLight is the light-to-surface direction.
			const RISEPel Le = entry.pLight->emittedRadiance( -vToLight );
			const RISEPel fBSDF = brdf.value( vToLight, ri );
			const Scalar invDistSq = 1.0 / (dist * dist);

			// Delta-position light: w = 1 (no MIS needed).  Fold in the
			// transparent-shadow Fresnel transmittance (1,1,1 when off).
			RISEPel amount = Le * fBSDF * cosSurface * invDistSq * shadowT;

			// Apply medium transmittance along shadow ray.
			// Multi-medium shadow transmittance (origin, per-object,
			// and global media; the segment walk has no fixed depth/count cap).
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				const RISEPel Tr = EvalShadowTransmittance( rayToLight, dist, pMedium,
					pMediumObject, pPreparedScene, bSceneHasObjectMedia, pMediumStack );
				amount = amount * Tr;
			}

			result = result + amount * (risWeight / pdfAlias);
		}
		else
		{
			// Selected a mesh luminary (guaranteed != self by exclusion)
			const LuminaryManager::LUM_ELEM& lumEntry = (*pPreparedLuminaries)[entry.lumIndex];
			const IEmitter* pEmitter = lumEntry.pLum->GetMaterial()->GetEmitter();

			const Scalar area = lumEntry.pLum->GetArea();

			// Sample a uniform random point on the luminary surface
			const Point3 ptRand( sampler.Get1D(), sampler.Get1D(), sampler.Get1D() );
			Point3 ptOnLum;
			Vector3 lumNormal;
			Point2 lumCoord;
			lumEntry.pLum->UniformRandomPoint( &ptOnLum, &lumNormal, &lumCoord, ptRand );

			// Geometry
			Vector3 vToLight = Vector3Ops::mkVector3( ptOnLum, ri.ptIntersection );
			const Scalar dist = Vector3Ops::NormalizeMag( vToLight );
			// For volume scatter points there is no surface normal:
			// the phase function handles angular distribution, and we
			// must integrate over the full sphere, not just a hemisphere.
			// cosSurface is forced to 1.0 to cancel the multiplication below
			// and skip hemisphere rejection.
			const Scalar cosSurface = isVolumeScatter ? Scalar(1.0) :
				Vector3Ops::Dot( vToLight, ri.vNormal );
			const Scalar cosLight = Vector3Ops::Dot( -vToLight, lumNormal );

			// Optimal MIS training: count every NEE attempt including
			// geometry-rejected samples (back-face, below hemisphere).
			// These are valid zero-valued draws for the denominator.
			if( pOptimalMIS && !pOptimalMIS->IsReady() )
			{
				const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
					ri.rast.x, ri.rast.y, kTechniqueNEE );
			}

			if( cosLight > 0 && (isVolumeScatter || cosSurface > 0) )
			{
				// Light-sample Russian roulette: estimate the geometric
				// contribution before the expensive shadow ray.  When
				// the estimate is below the threshold, probabilistically
				// terminate.  Survivors are divided by the survival
				// probability to maintain unbiasedness.
				Scalar rrSurvivalCompensation = 1.0;
				if( lightSampleRRThreshold > 0 )
				{
					const Scalar estimate = entry.exitance * cosSurface *
						area * cosLight / (dist * dist);
					const Scalar pSurvive = r_min(
						estimate / lightSampleRRThreshold, Scalar(1.0) );
					if( pSurvive < Scalar(1.0) )
					{
						if( sampler.Get1D() >= pSurvive )
						{
							break;
						}
						rrSurvivalCompensation = Scalar(1.0) / pSurvive;
					}
				}

				// Shadow test.  meshShadowT carries the Fresnel transmittance
				// when transparent shadows are enabled (else 1,1,1).
				bool shadowed = false;
				RISEPel meshShadowT( 1.0, 1.0, 1.0 );
				if( pShadingObject && pShadingObject->DoesReceiveShadows() )
				{
					const Ray rayToLight( ri.ptIntersection, vToLight );
					shadowed = ShadowOccludedRGB( caster, rayToLight, dist - 0.001, meshShadowT );
				}

				if( !shadowed )
				{
					// Emitted radiance at sampled point
					RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
					lumri.vNormal = lumNormal;
					// Luminary normal is the geometric face normal; mirror.
					lumri.vGeomNormal = lumNormal;
					lumri.ptCoord = lumCoord;
					lumri.onb.CreateFromW( lumNormal );

					const RISEPel Le = pEmitter->emittedRadiance( lumri, -vToLight, lumNormal );

					const Scalar geom = area * cosLight / (dist * dist);
					RISEPel contrib = Le * cosSurface * geom * brdf.value( vToLight, ri ) * meshShadowT;

					// Apply medium transmittance along shadow ray.
					// Multi-medium shadow transmittance.
					{
						const Ray rayToLight( ri.ptIntersection, vToLight );
						const RISEPel Tr = EvalShadowTransmittance( rayToLight, dist, pMedium,
							pMediumObject, pPreparedScene, bSceneHasObjectMedia, pMediumStack );
						contrib = contrib * Tr;
					}

					// MIS weight: applied when the selection PDF is
					// tractable (alias table or BVH).  When RIS is ON
					// (no BVH), the exact finite-M technique density is
					// intractable, so we use w=1 and suppress the BSDF-hit
					// emitter contribution in PathTracingShaderOp.
					const bool bCanDoMIS = (pLightBVH && pLightBVH->IsBuilt()) ||
						risCandidates == 0;
					if( bCanDoMIS && pMaterial && area > 0 && cosLight > 0 )
					{
						// Convert selection PDF to solid angle.
						// For both alias-table and BVH, pdfAlias is the
						// probability of selecting this light.
						const Scalar p_light = pdfAlias * (dist * dist) / (area * cosLight);
						static const IORStack defaultIOR( 1.0 );
						const Scalar p_bsdf = pMaterial->Pdf( vToLight, ri, defaultIOR );

						// Optimal MIS training: accumulate second moment
						// for a successful NEE hit.  contrib includes the
						// geometry factor (area*cosLight/dist^2), so the
						// matching pdf is pdfAlias (area measure), not
						// p_light (solid angle).  This ensures
						// f2/pdf^2 = (contrib/pdfAlias)^2 = (f/p_nee)^2.
						if( pOptimalMIS && !pOptimalMIS->IsReady() )
						{
							const Scalar lum = ColorMath::MaxValue( contrib );
							const Scalar f2 = lum * lum;
							if( f2 > 0 && pdfAlias > 0 )
							{
								const_cast<OptimalMISAccumulator*>(pOptimalMIS)->Accumulate(
									ri.rast.x, ri.rast.y,
									f2, pdfAlias, kTechniqueNEE );
							}
						}

						if( p_bsdf > 0 )
						{
							Scalar w;
							if( pOptimalMIS && pOptimalMIS->IsReady() )
							{
								const Scalar alpha = pOptimalMIS->GetAlpha( ri.rast.x, ri.rast.y );
								w = MISWeights::OptimalMIS2Weight( p_light, p_bsdf, 1.0 - alpha );
							}
							else
							{
								w = PowerHeuristic( p_light, p_bsdf );
							}
							contrib = contrib * w;
						}
					}

					result = result + contrib * (rrSurvivalCompensation * risWeight / pdfAlias);
				}
			}
		}
	} while( false );

	// Environment map NEE: sample one direction from the HDR
	// importance map and evaluate the shadowed, MIS-weighted
	// contribution.  This is independent of the light-table NEE
	// above (separate strategy with its own MIS against BSDF).
	// LIGHT-SOLO Stage 3: env-NEE runs only when solo is inactive OR
	// the environment IS the soloed light -- every other solo target
	// (an explicit light or a mesh luminary) must see zero contribution
	// from the env map, exactly like every other non-target light.
	if( ( soloKind == SoloKind::None || soloKind == SoloKind::Environment ) &&
		pEnvSampler && pEnvSampler->IsValid() && pEnvironmentMap )
	{
		Vector3 envDir;
		Scalar envPdf;
		pEnvSampler->Sample( sampler.Get1D(), sampler.Get1D(), envDir, envPdf );

		const Scalar cosEnv = isVolumeScatter ? Scalar(1.0) :
			Vector3Ops::Dot( envDir, ri.vNormal );

		// Optimal MIS training: count every env NEE attempt that produced
		// a valid sample (envPdf > 0), including below-hemisphere samples
		// which are zero-valued draws for the denominator.
		if( envPdf > 0 && pOptimalMIS && !pOptimalMIS->IsReady() )
		{
			const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
				ri.rast.x, ri.rast.y, kTechniqueNEE );
		}

		if( (isVolumeScatter || cosEnv > 0) && envPdf > 0 )
		{
			// Shadow test: cast ray to infinity.  envShadowT carries the
			// Fresnel transmittance when transparent shadows are on (else 1,1,1).
			bool envShadowed = false;
			RISEPel envShadowT( 1.0, 1.0, 1.0 );
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToEnv( ri.ptIntersection, envDir );
				envShadowed = ShadowOccludedRGB( caster, rayToEnv, RISE_INFINITY, envShadowT );
			}

			if( !envShadowed )
			{
				const Ray envRay( ri.ptIntersection, envDir );
				const RISEPel Le = pEnvironmentMap->GetRadiance( envRay, nullRasterizerState );
				const RISEPel f = brdf.value( envDir, ri );
				RISEPel envContrib = Le * f * (cosEnv / envPdf) * envShadowT;

				// Apply medium transmittance for environment ray.
				// Multi-medium shadow transmittance.
				// Use a large but finite distance for the walk limit
				// (environment rays go to infinity, but media are bounded).
				{
					const RISEPel Tr = EvalShadowTransmittance( envRay, RISE_INFINITY, pMedium,
						pMediumObject, pPreparedScene, bSceneHasObjectMedia, pMediumStack );
					envContrib = envContrib * Tr;
				}

				// Optimal MIS training: accumulate second moment for
				// successful env NEE hit.  envContrib = Le*BSDF*cos*Tr/envPdf,
				// so the full integrand f = envContrib * envPdf.
				if( pOptimalMIS && !pOptimalMIS->IsReady() && pMaterial )
				{
					const RISEPel fullIntegrand = envContrib * envPdf;
					const Scalar lum = ColorMath::MaxValue( fullIntegrand );
					const Scalar f2 = lum * lum;
					if( f2 > 0 && envPdf > 0 )
					{
						const_cast<OptimalMISAccumulator*>(pOptimalMIS)->Accumulate(
							ri.rast.x, ri.rast.y,
							f2, envPdf, kTechniqueNEE );
					}
				}

				// MIS: power heuristic (or optimal MIS) against BSDF PDF
				if( pMaterial )
				{
					static const IORStack defaultIOR( 1.0 );
					const Scalar pBsdf = pMaterial->Pdf( envDir, ri, defaultIOR );
					if( pBsdf > 0 )
					{
						Scalar w;
						if( pOptimalMIS && pOptimalMIS->IsReady() )
						{
							const Scalar alpha = pOptimalMIS->GetAlpha( ri.rast.x, ri.rast.y );
							w = MISWeights::OptimalMIS2Weight( envPdf, pBsdf, 1.0 - alpha );
						}
						else
						{
							w = PowerHeuristic( envPdf, pBsdf );
						}
						envContrib = envContrib * w;
					}
				}

				result = result + envContrib;
			}
		}
	}

	return result;
}

bool LightSampler::SampleVolumeEmission(
	ISampler& sampler,
	VolumeEmissionSample& sample
	) const
{
	sample = VolumeEmissionSample();
	if( !volumeEmissionAlias.IsValid() || volumeEmissionMedia.empty() ) return false;
	const unsigned int mediumIndex = volumeEmissionAlias.Sample( sampler.Get1D() );
	if( mediumIndex >= volumeEmissionMedia.size() ) return false;
	sample.pMedium = volumeEmissionMedia[mediumIndex];
	sample.mediumSelectionPdf = static_cast<Scalar>(
		volumeEmissionAlias.Pdf( mediumIndex ) );
	if( !sample.pMedium || sample.mediumSelectionPdf <= 0.0 ||
		!sample.pMedium->SampleThermalEmission(
			sampler, sample.point, sample.pointPdf ) ) {
		sample = VolumeEmissionSample();
		return false;
	}
	sample.pdf = sample.mediumSelectionPdf * sample.pointPdf;
	return RISE::IsFiniteDouble( sample.pdf ) && sample.pdf > 0.0;
}

Scalar LightSampler::VolumeEmissionPdf(
	const IMedium& medium,
	const Point3& point
	) const
{
	if( !volumeEmissionAlias.IsValid() ) return 0.0;
	for( unsigned int i = 0; i < volumeEmissionMedia.size(); ++i ) {
		if( volumeEmissionMedia[i] == &medium ) {
			return static_cast<Scalar>( volumeEmissionAlias.Pdf(i) ) *
				medium.ThermalEmissionPdf( point );
		}
	}
	return 0.0;
}

bool LightSampler::SampleVolumeEmissionPivots(
	ISampler& sampler,
	VolumeEmissionPivotState& pivots
	) const
{
	pivots.mediumPivots.clear();
	pivots.mediumPivots.reserve( volumeEmissionMedia.size() );
	for( unsigned int i = 0; i < volumeEmissionMedia.size(); ++i ) {
		Point3 point;
		Scalar ignoredPdf = 0.0;
		if( !volumeEmissionMedia[i] ||
			!volumeEmissionMedia[i]->SampleThermalEmission(
				sampler, point, ignoredPdf ) || ignoredPdf <= 0.0 ) {
			pivots.mediumPivots.clear();
			return false;
		}
		pivots.mediumPivots.push_back( point );
	}
	return true;
}

bool LightSampler::ResolveVolumeEmissionPivots(
	ISampler& sampler,
	const VolumeEmissionPivotState* sharedPivots,
	VolumeEmissionPivotState& pivots
	) const
{
	if( sharedPivots ) {
		if( sharedPivots->mediumPivots.size() != volumeEmissionMedia.size() ) {
			pivots.mediumPivots.clear();
			return false;
		}
		pivots = *sharedPivots;
		return true;
	}
	return SampleVolumeEmissionPivots( sampler, pivots );
}

bool LightSampler::SampleVolumeEmissionVertex(
	ISampler& sampler,
	VolumeEmissionVertexSample& sample
	) const
{
	sample = VolumeEmissionVertexSample();
	sample.pivotsReady = SampleVolumeEmissionPivots( sampler, sample.pivots );
	if( !sample.pivotsReady ) return false;
	sample.endpointAttempted = true;
	sample.endpointReady = SampleVolumeEmission( sampler, sample.endpoint );
	return sample.endpointReady;
}

bool LightSampler::SampleEquiangularPivot(
	const VolumeEmissionPivotState& pivots,
	const Scalar xi,
	Point3& pivot,
	Scalar& selectionPdf
	) const
{
	selectionPdf = 0.0;
	if( !equiangularPivotDistributionValid ||
		!equiangularPivotAlias.IsValid() ||
		pivots.mediumPivots.size() != volumeEmissionMedia.size() ) return false;
	const unsigned int entry = equiangularPivotAlias.Sample( xi );
	selectionPdf = static_cast<Scalar>( equiangularPivotAlias.Pdf(entry) );
	if( selectionPdf <= 0.0 ) return false;
	if( entry < positionalLightIndices.size() ) {
		pivot = GetPositionalLightPosition( entry );
		return true;
	}
	const unsigned int mediumIndex = entry -
		static_cast<unsigned int>( positionalLightIndices.size() );
	if( mediumIndex >= pivots.mediumPivots.size() ) return false;
	pivot = pivots.mediumPivots[mediumIndex];
	return true;
}

Scalar LightSampler::EquiangularDistancePdf(
	const VolumeEmissionPivotState& pivots,
	const Ray& ray,
	const Scalar tMin,
	const Scalar tMax,
	const bool segmentBounded,
	const Scalar t
	) const
{
	if( !equiangularPivotDistributionValid ||
		!equiangularPivotAlias.IsValid() ||
		pivots.mediumPivots.size() != volumeEmissionMedia.size() ) return 0.0;

	Scalar density = 0.0;
	const unsigned int positionalCount = static_cast<unsigned int>(
		positionalLightIndices.size() );
	const unsigned int entryCount = equiangularPivotAlias.Size();
	for( unsigned int i = 0; i < entryCount; ++i ) {
		const Point3& pivot = i < positionalCount
			? GetPositionalLightPosition(i)
			: pivots.mediumPivots[i-positionalCount];
		const Scalar selectionPdf = static_cast<Scalar>(
			equiangularPivotAlias.Pdf(i) );
		density += selectionPdf * EquiangularSampling::Pdf(
			ray, pivot, tMin, tMax, segmentBounded, t );
	}
	return RISE::IsFiniteDouble(density) && density >= 0.0 ? density : 0.0;
}

static bool ResolveEquiangularSegmentInterval(
	const IMedium& medium,
	const Ray& ray,
	const Scalar maxDist,
	const bool surfaceBounded,
	Scalar& tNear,
	Scalar& tFar
	)
{
	bool bounded = surfaceBounded;
	tNear = 0.0;
	tFar = maxDist;
	Point3 bbMin, bbMax;
	if( medium.GetBoundingBox(bbMin,bbMax) ) {
		Scalar tEntry = 0.0;
		Scalar tExit = maxDist;
		const Scalar invX = fabs(ray.Dir().x) > 1e-20 ? 1.0/ray.Dir().x : 0.0;
		const Scalar invY = fabs(ray.Dir().y) > 1e-20 ? 1.0/ray.Dir().y : 0.0;
		const Scalar invZ = fabs(ray.Dir().z) > 1e-20 ? 1.0/ray.Dir().z : 0.0;
		bool hit = true;
		if( invX != 0.0 ) {
			Scalar a = (bbMin.x-ray.origin.x)*invX;
			Scalar b = (bbMax.x-ray.origin.x)*invX;
			if( a > b ) { const Scalar swap = a; a = b; b = swap; }
			tEntry = fmax(tEntry,a);
			tExit = fmin(tExit,b);
		} else if( ray.origin.x < bbMin.x || ray.origin.x > bbMax.x ) hit = false;
		if( hit && invY != 0.0 ) {
			Scalar a = (bbMin.y-ray.origin.y)*invY;
			Scalar b = (bbMax.y-ray.origin.y)*invY;
			if( a > b ) { const Scalar swap = a; a = b; b = swap; }
			tEntry = fmax(tEntry,a);
			tExit = fmin(tExit,b);
		} else if( hit && (ray.origin.y < bbMin.y || ray.origin.y > bbMax.y) ) hit = false;
		if( hit && invZ != 0.0 ) {
			Scalar a = (bbMin.z-ray.origin.z)*invZ;
			Scalar b = (bbMax.z-ray.origin.z)*invZ;
			if( a > b ) { const Scalar swap = a; a = b; b = swap; }
			tEntry = fmax(tEntry,a);
			tExit = fmin(tExit,b);
		} else if( hit && (ray.origin.z < bbMin.z || ray.origin.z > bbMax.z) ) hit = false;
		if( hit && tEntry < tExit ) {
			tNear = fmax(0.0,tEntry);
			tFar = fmin(maxDist,tExit);
			bounded = true;
		}
	}
	return bounded && tFar > tNear;
}

MISWeights::LogDensity LightSampler::EvaluateVolumeEmissionDistanceLogDensityNM(
	const IMedium& medium,
	const Ray& ray,
	const Scalar proposalMaxDist,
	const bool surfaceBounded,
	const VolumeEmissionPivotState* pivots,
	const Scalar nm,
	const Scalar eventDistance,
	const bool scattered
	) const
{
	if( !RISE::IsFiniteDouble(eventDistance) || eventDistance <= 0.0 ||
		!RISE::IsFiniteDouble(proposalMaxDist) || proposalMaxDist <= 0.0 ||
		eventDistance > proposalMaxDist ) return MISWeights::LogDensity();
	const MISWeights::LogDensity deltaTracking =
		MISWeights::MakeLogDensityFromLogValue(
			medium.EvalLogDistancePdfNM(
				ray,eventDistance,scattered,proposalMaxDist,nm) );
	const bool hasSharedPivots = pivots &&
		pivots->mediumPivots.size()==volumeEmissionMedia.size();
	Scalar tNear = 0.0;
	Scalar tFar = proposalMaxDist;
	const bool useEquiangular = hasSharedPivots &&
		equiangularPivotDistributionValid && equiangularPivotAlias.IsValid() &&
		equiangularPivotAlias.Size() > 0 &&
		ResolveEquiangularSegmentInterval(
			medium,ray,proposalMaxDist,surfaceBounded,tNear,tFar);
	if( !useEquiangular ) return deltaTracking;
	if( !scattered ) {
		return deltaTracking.hasSupport ? MISWeights::LogDensity(
			true,deltaTracking.value-log(2.0)) : MISWeights::LogDensity();
	}
	const Scalar equiangularPdf = EquiangularDistancePdf(
		*pivots,ray,tNear,tFar,true,eventDistance);
	return MISWeights::EqualMixtureLogDensity(
		deltaTracking,MISWeights::MakeLogDensity(equiangularPdf));
}

bool LightSampler::EvaluateVolumeEmissionConnectionNM(
	const Ray& ray,
	const Scalar endpointDistance,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IORStack* pOriginStack,
	const Scalar nm,
	const VolumeEmissionPivotState& pivots,
	const Scalar directionPdf,
	Scalar& outTransmittance,
	const IMedium** pEndpointMedium,
	MISWeights::LogDensity& outMarchDensity
	) const
{
	outTransmittance = 0.0;
	outMarchDensity = MISWeights::LogDensity();
	if( pEndpointMedium ) *pEndpointMedium = 0;
	if( !pPreparedScene || !RISE::IsFiniteDouble(endpointDistance) ||
		endpointDistance <= 0.0 ) return false;
	VolumeMarchProposalObserverNM marchObserver(
		*this,nm,pivots,directionPdf);
	const bool walked = EvaluateShadowMediumTransmittanceImpl(
		ray,endpointDistance,pOriginMedium,pOriginMediumObject,pPreparedScene,
		bSceneHasObjectMedia,ShadowTransmittanceNM(nm),outTransmittance,
		pOriginStack,pEndpointMedium,eShadowBoundaryExactNullOnly,marchObserver);
	if( walked ) outMarchDensity = marchObserver.Finish(endpointDistance);
	return walked;
}

Scalar LightSampler::GetEquiangularPivotPower(
	const unsigned int index
	) const
{
	if( index < positionalLightBandPowers.size() ) {
		return positionalLightBandPowers[index];
	}
	const unsigned int mediumIndex = index -
		static_cast<unsigned int>( positionalLightBandPowers.size() );
	return mediumIndex < volumeEmissionPowerProxies.size()
		? volumeEmissionPowerProxies[mediumIndex] : 0.0;
}

Scalar LightSampler::EvaluateVolumeDirectLightingNM(
	const RayIntersectionGeometric& ri,
	const IBSDF& receiver,
	const Scalar nm,
	ISampler& sampler,
	const IMedium* pMedium,
	const bool isVolumeScatter,
	const IObject* pMediumObject,
	const IORStack* pMediumStack
	) const
{
	if( !pPreparedScene || !pPreparedLuminaries ) return 0.0;
	VolumeEmissionSample endpoint;
	if( !SampleVolumeEmission( sampler, endpoint ) ) return 0.0;

	Vector3 direction = Vector3Ops::mkVector3(
		endpoint.point, ri.ptIntersection );
	const Scalar distance = Vector3Ops::NormalizeMag( direction );
	if( !RISE::IsFiniteDouble( distance ) || distance <= 0.0 ) return 0.0;
	const Scalar receiverCosine = isVolumeScatter ? Scalar(1.0) :
		Vector3Ops::Dot( direction, ri.vNormal );
	if( receiverCosine <= 0.0 ) return 0.0;

	const Ray shadowRay( ri.ptIntersection, direction );
	Scalar transmittance = 0.0;
	const IMedium* endpointMedium = 0;
	NoMarchProposalObserver marchObserver;
	if( !EvaluateShadowMediumTransmittanceImpl(
		shadowRay, distance, pMedium, pMediumObject, pPreparedScene,
		bSceneHasObjectMedia, ShadowTransmittanceNM(nm), transmittance,
		pMediumStack, &endpointMedium, eShadowBoundaryExactNullOnly,
		marchObserver ) ) return 0.0;
	// Innermost-exclusive support: an outer medium's labeled draw can land
	// inside a nested medium.  It remains a valid sample at the original pdf,
	// but its contribution is zero rather than being renormalized.
	if( endpointMedium != endpoint.pMedium ) return 0.0;

	const Scalar emission = endpoint.pMedium->GetThermalEmissionNM(
		endpoint.point, nm );
	if( emission <= 0.0 || transmittance <= 0.0 ) return 0.0;
	const Scalar response = receiver.valueNM( direction, ri, nm );
	if( response <= 0.0 ) return 0.0;
	return response * receiverCosine * transmittance * emission /
		(distance * distance * endpoint.pdf);
}

Scalar LightSampler::EvaluateVolumeDirectLightingFromClosureNM(
	const RayIntersectionGeometric& ri,
	const IContinuationClosureNM& closure,
	const ContinuationAvailability& availability,
	const Scalar nm,
	const VolumeEmissionVertexSample& vertexSample,
	const IMedium* pMedium,
	const IObject* pMediumObject,
	const IORStack* pMediumStack
	) const
{
	if( !vertexSample.HasPivots() || !vertexSample.WasEndpointAttempted() ||
		!vertexSample.HasEndpoint() ) return 0.0;
	const VolumeEmissionSample& endpoint = vertexSample.Endpoint();
	if( !endpoint.pMedium || !RISE::IsFiniteDouble(endpoint.pdf) ||
		endpoint.pdf <= 0.0 ) return 0.0;
	Vector3 direction = Vector3Ops::mkVector3(
		endpoint.point,ri.ptIntersection);
	const Scalar distance = Vector3Ops::NormalizeMag(direction);
	if( !RISE::IsFiniteDouble(distance) || distance <= 0.0 ) return 0.0;
	const Scalar cosine = Vector3Ops::Dot(direction,ri.vNormal);
	if( cosine <= 0.0 ) return 0.0;

	const unsigned int fullMask = closure.GetLobeMask();
	const unsigned int marchMask = availability.marchMask&fullMask;
	const unsigned int directOnlyMask = fullMask&~marchMask;
	const Scalar responseMarch = closure.EvaluateSubset(marchMask,direction);
	const Scalar responseDirectOnly =
		closure.EvaluateSubset(directOnlyMask,direction);
	if( responseMarch <= 0.0 && responseDirectOnly <= 0.0 ) return 0.0;
	const bool terminalSourceOnly = availability.vertexMask != marchMask;
	const Scalar directionPdf = terminalSourceOnly ?
		closure.PdfMarchMarginal(marchMask,direction) :
		closure.PdfReachMarginal(marchMask,direction);

	Scalar transmittance = 0.0;
	const IMedium* endpointMedium = 0;
	MISWeights::LogDensity marchDensity;
	if( !EvaluateVolumeEmissionConnectionNM(
		Ray(ri.ptIntersection,direction),distance,pMedium,pMediumObject,
		pMediumStack,nm,vertexSample.Pivots(),directionPdf,transmittance,
		&endpointMedium,marchDensity) ) return 0.0;
	if( endpointMedium != endpoint.pMedium || transmittance <= 0.0 ) return 0.0;
	const Scalar emission = endpoint.pMedium->GetThermalEmissionNM(
		endpoint.point,nm);
	if( emission <= 0.0 ) return 0.0;
	const MISWeights::LogDensity neeDensity =
		MISWeights::MakeLogDensity(endpoint.pdf);
	const Scalar neeWeight = MISWeights::VolumeEmissionFamilyWeightFromLogDensities(
		neeDensity,marchDensity);
	const Scalar response = responseMarch*neeWeight + responseDirectOnly;
	return response*cosine*transmittance*emission/
		(distance*distance*endpoint.pdf);
}

Scalar LightSampler::EvaluateVolumeDirectLightingFromPhaseClosureNM(
	const Point3& scatterPoint,
	const Vector3& incomingDirection,
	const IPhaseFunction& phaseClosure,
	const MediumContinuationAvailability& availability,
	const Scalar rouletteSurvivalProbability,
	const Scalar nm,
	const VolumeEmissionVertexSample& vertexSample,
	const IMedium* pMedium,
	const IObject* pMediumObject,
	const IORStack* pMediumStack
	) const
{
	if( !vertexSample.HasPivots() || !vertexSample.WasEndpointAttempted() ||
		!vertexSample.HasEndpoint() ) return 0.0;
	const VolumeEmissionSample& endpoint = vertexSample.Endpoint();
	if( !endpoint.pMedium || !RISE::IsFiniteDouble(endpoint.pdf) ||
		endpoint.pdf <= 0.0 ) return 0.0;
	Vector3 direction = Vector3Ops::mkVector3(endpoint.point,scatterPoint);
	const Scalar distance = Vector3Ops::NormalizeMag(direction);
	if( !RISE::IsFiniteDouble(distance) || distance <= 0.0 ) return 0.0;
	const Scalar phaseResponse = phaseClosure.Evaluate(
		incomingDirection,direction);
	if( !RISE::IsFiniteDouble(phaseResponse) || phaseResponse <= 0.0 ) return 0.0;

	Scalar responseMarch = 0.0;
	Scalar responseDirectOnly = phaseResponse;
	Scalar directionPdf = 0.0;
	if( availability.marchAllowed ) {
		responseMarch = phaseResponse;
		responseDirectOnly = 0.0;
		const Scalar phasePdf = phaseClosure.Pdf(incomingDirection,direction);
		const bool terminalSourceOnly = availability.vertexAllowed !=
			availability.marchAllowed;
		if( terminalSourceOnly ) {
			directionPdf = phasePdf;
		} else if( RISE::IsFiniteDouble(rouletteSurvivalProbability) &&
			rouletteSurvivalProbability >= 0.0 &&
			rouletteSurvivalProbability <= 1.0 ) {
			directionPdf = phasePdf*rouletteSurvivalProbability;
		}
	}

	Scalar transmittance = 0.0;
	const IMedium* endpointMedium = 0;
	MISWeights::LogDensity marchDensity;
	if( !EvaluateVolumeEmissionConnectionNM(
		Ray(scatterPoint,direction),distance,pMedium,pMediumObject,pMediumStack,
		nm,vertexSample.Pivots(),directionPdf,transmittance,&endpointMedium,
		marchDensity) ) return 0.0;
	if( endpointMedium != endpoint.pMedium || transmittance <= 0.0 ) return 0.0;
	const Scalar emission = endpoint.pMedium->GetThermalEmissionNM(
		endpoint.point,nm);
	if( emission <= 0.0 ) return 0.0;
	const Scalar neeWeight = MISWeights::VolumeEmissionFamilyWeightFromLogDensities(
		MISWeights::MakeLogDensity(endpoint.pdf),marchDensity);
	return (responseMarch*neeWeight+responseDirectOnly)*transmittance*emission/
		(distance*distance*endpoint.pdf);
}

Scalar LightSampler::EvaluateDirectLightingNM(
	const RayIntersectionGeometric& ri,
	const IBSDF& brdf,
	const IMaterial* pMaterial,
	const Scalar nm,
	const IRayCaster& caster,
	ISampler& sampler,
	const IObject* pShadingObject,
	const IMedium* pMedium,
	const bool isVolumeScatter,
	const IObject* pMediumObject,
	const IORStack* pMediumStack
	) const
{
	Scalar result = 0;

	if( !pPreparedScene || !pPreparedLuminaries )
	{
		return result;
	}

	// ----------------------------------------------------------------
	// Step 1: Deterministic evaluation of lights with zero exitance
	// (ambient, directional).  These cannot participate in
	// proportional selection.  Mirrors the RGB path's deterministic
	// pass.
	// ----------------------------------------------------------------
	const ILightManager* pLightMgr = pPreparedScene->GetLights();
	if( pLightMgr )
	{
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		ILightManager::LightsList::const_iterator m, n;
		for( m=lights.begin(), n=lights.end(); m!=n; m++ )
		{
			const ILightPriv* l = *m;
			// LIGHT-SOLO: see the RGB variant's identical filter above.
			if( soloKind != SoloKind::None && !IsSoloTargetLight( l ) )
			{
				continue;
			}
			const Scalar exitance = ColorMath::MaxValue( l->radiantExitance() );
			if( exitance <= 0 )
			{
				// Per-NM direct lighting evaluation — uses brdf.valueNM
				// (per-wavelength BSDF) so the surface's spectral
				// character is preserved.  The previous implementation
				// computed the RGB direct lighting through brdf.value
				// then projected to luminance — the per-NM BSDF was
				// never queried, so a saturated green surface (whose
				// JH spectrum peaks at green wavelengths) under ambient
				// light was rendered as near-white because the
				// integrated contribution was a flat scalar (Y of green
				// RGB) regardless of wavelength.  See AmbientLight.h /
				// DirectionalLight.cpp for the per-NM implementations.
				result += l->ComputeDirectLightingNM( ri, caster, brdf,
					pShadingObject ? pShadingObject->DoesReceiveShadows() : true,
					nm );
			}
		}
	}

	// ----------------------------------------------------------------
	// Step 2: Select one light with nonzero exitance, excluding self.
	// ----------------------------------------------------------------
	do {
		// Find self in the light table (for exclusion) -- needed by
		// every branch below, including the solo bypass (NM variant).
		const int selfIdx = FindLuminaryIndex( pShadingObject );

		unsigned int idx;
		Scalar pdfAlias;
		Scalar risWeight = 1.0;

		if( soloKind != SoloKind::None )
		{
			// LIGHT-SOLO bypass: skip BVH/RIS/alias-table selection
			// entirely.  Directly locate the soloed entry (if any) and
			// evaluate it with selection pdf = 1.0 -- CachedPdfSelectLuminary
			// returns the SAME 1.0 for this identity (see its solo branch)
			// so NEE and the integrator's BSDF-hit MIS partner agree, which
			// is what keeps the combined estimator unbiased under solo.  A
			// solo target that is NOT in lightEntries (a zero-exitance
			// light, already handled by Step 1 above; or an Environment
			// target) has no entry here -- break to env NEE.
			int soloIdx = -1;
			if( soloKind == SoloKind::Light )
			{
				for( unsigned int i = 0; i < lightEntries.size(); i++ )
				{
					if( lightEntries[i].pLight == soloLight )
					{
						soloIdx = static_cast<int>( i );
						break;
					}
				}
			}
			else if( soloKind == SoloKind::Luminary )
			{
				soloIdx = FindLuminaryIndex( soloLuminaryObject );
			}
			// SoloKind::Environment: soloIdx stays -1, handled entirely by
			// the env-NEE block below.

			if( soloIdx < 0 || soloIdx == selfIdx )
			{
				break;
			}

			idx = static_cast<unsigned int>( soloIdx );
			pdfAlias = 1.0;
			risWeight = 1.0;
		}
		else if( !aliasTable.IsValid() )
		{
			break;
		}
		else if( pLightBVH && pLightBVH->IsBuilt() )
		{
			// Light BVH: importance-weighted selection with full MIS.
			Scalar bvhPdf;
			idx = pLightBVH->Sample( ri.ptIntersection, ri.vNormal,
				sampler.Get1D(), bvhPdf );

			if( bvhPdf <= 0 || static_cast<int>(idx) == selfIdx )
			{
				sampler.Get1D();
				sampler.Get1D();
				sampler.Get1D();
				break;
			}

			pdfAlias = bvhPdf;
		}
		else if( risCandidates > 0 )
		{
			idx = SelectLightRIS( ri.ptIntersection, sampler, pdfAlias, risWeight, selfIdx );

			if( idx >= static_cast<unsigned int>( lightEntries.size() ) )
			{
				sampler.Get1D();
				sampler.Get1D();
				sampler.Get1D();
				break;
			}
		}
		else
		{
			// Single alias-table draw with exact self-exclusion.
			// See RGB variant for proof of unbiasedness.
			idx = aliasTable.Sample( sampler.Get1D() );

			if( static_cast<int>(idx) == selfIdx )
			{
				sampler.Get1D();
				sampler.Get1D();
				sampler.Get1D();
				break;
			}

			pdfAlias = static_cast<Scalar>( aliasTable.Pdf( idx ) );
		}

		const LightEntry& entry = lightEntries[idx];

		if( entry.pLight )
		{
			// Non-mesh (delta-position) light — spectral approximation
			// via CIE luminance of the RGB emitted radiance.  Matches
			// the BDPT integrator's spectral handling of non-mesh lights.
			const Point3 lightPos = entry.pLight->position();
			Vector3 vToLight = Vector3Ops::mkVector3( lightPos, ri.ptIntersection );
			const Scalar dist = Vector3Ops::NormalizeMag( vToLight );
			const Scalar cosSurface = isVolumeScatter ? Scalar(1.0) :
				Vector3Ops::Dot( vToLight, ri.vNormal );

			if( !isVolumeScatter && cosSurface <= 0 ) break;

			// Shadow test.  shadowTNM carries the Fresnel transmittance
			// when transparent shadows are enabled (else 1.0).
			Scalar shadowTNM = 1.0;
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				if( ShadowOccludedNM( caster, rayToLight, dist - 0.001, nm, shadowTNM ) )
					break;
			}

			// emittedRadiance expects the outgoing direction FROM the
			// light; -vToLight is the light-to-surface direction.
			const RISEPel LeRGB = entry.pLight->emittedRadiance( -vToLight );
			const Scalar LeNM = ColorMath::Luminance( LeRGB );
			const Scalar invDistSq = 1.0 / (dist * dist);
			const Scalar fBSDF = brdf.valueNM( vToLight, ri, nm );

			// Delta-position light: w = 1 (no MIS needed).  Fold in the
			// transparent-shadow Fresnel transmittance (1.0 when off).
			Scalar neeContrib = LeNM * fBSDF * cosSurface * invDistSq * shadowTNM
				   * (risWeight / pdfAlias);

			// Apply medium transmittance along shadow ray.
			// Multi-medium shadow transmittance.
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				neeContrib *= EvalShadowTransmittanceNM( rayToLight, dist, pMedium,
					pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm, pMediumStack );
			}

			result = neeContrib;
			break;
		}

		const LuminaryManager::LUM_ELEM& lumEntry = (*pPreparedLuminaries)[entry.lumIndex];
		const IEmitter* pEmitter = lumEntry.pLum->GetMaterial()->GetEmitter();

		const Scalar area = lumEntry.pLum->GetArea();

		const Point3 ptRand( sampler.Get1D(), sampler.Get1D(), sampler.Get1D() );
		Point3 ptOnLum;
		Vector3 lumNormal;
		Point2 lumCoord;
		lumEntry.pLum->UniformRandomPoint( &ptOnLum, &lumNormal, &lumCoord, ptRand );

		Vector3 vToLight = Vector3Ops::mkVector3( ptOnLum, ri.ptIntersection );
		const Scalar dist = Vector3Ops::NormalizeMag( vToLight );
		const Scalar cosSurface = isVolumeScatter ? Scalar(1.0) :
			Vector3Ops::Dot( vToLight, ri.vNormal );
		const Scalar cosLight = Vector3Ops::Dot( -vToLight, lumNormal );

		// Optimal MIS training: count every spectral NEE attempt
		// including geometry-rejected samples (back-face, below
		// hemisphere) — valid zero-valued draws for the denominator.
		if( pOptimalMIS && !pOptimalMIS->IsReady() )
		{
			const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
				ri.rast.x, ri.rast.y, kTechniqueNEE );
		}

		if( cosLight <= 0 || (!isVolumeScatter && cosSurface <= 0) )
		{
			break;
		}

		// Light-sample Russian roulette (spectral path)
		Scalar rrSurvivalCompensation = 1.0;
		if( lightSampleRRThreshold > 0 )
		{
			const Scalar estimate = entry.exitance * cosSurface *
				area * cosLight / (dist * dist);
			const Scalar pSurvive = r_min(
				estimate / lightSampleRRThreshold, Scalar(1.0) );
			if( pSurvive < Scalar(1.0) )
			{
				if( sampler.Get1D() >= pSurvive )
				{
					break;
				}
				rrSurvivalCompensation = Scalar(1.0) / pSurvive;
			}
		}

		// Shadow test.  meshShadowTNM carries the Fresnel transmittance
		// when transparent shadows are enabled (else 1.0).
		Scalar meshShadowTNM = 1.0;
		if( pShadingObject && pShadingObject->DoesReceiveShadows() )
		{
			const Ray rayToLight( ri.ptIntersection, vToLight );
			if( ShadowOccludedNM( caster, rayToLight, dist - 0.001, nm, meshShadowTNM ) )
			{
				break;
			}
		}

		RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
		lumri.vNormal = lumNormal;
		// Luminary normal is geometric; mirror.
		lumri.vGeomNormal = lumNormal;
		lumri.ptCoord = lumCoord;
		lumri.onb.CreateFromW( lumNormal );

		const Scalar Le = pEmitter->emittedRadianceNM( lumri, -vToLight, lumNormal, nm );

		const Scalar geom = area * cosLight / (dist * dist);
		Scalar contrib = Le * cosSurface * geom * brdf.valueNM( vToLight, ri, nm ) * meshShadowTNM;

		// Multi-medium shadow transmittance.
		{
			const Ray rayToLight( ri.ptIntersection, vToLight );
			contrib *= EvalShadowTransmittanceNM( rayToLight, dist, pMedium,
				pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm, pMediumStack );
		}

		// MIS when selection PDF is tractable (alias table or BVH)
		const bool bCanDoMIS_NM = (pLightBVH && pLightBVH->IsBuilt()) ||
			risCandidates == 0;
		if( bCanDoMIS_NM && pMaterial && area > 0 && cosLight > 0 )
		{
			const Scalar p_light = pdfAlias * (dist * dist) / (area * cosLight);
			static const IORStack defaultIOR( 1.0 );
			const Scalar p_bsdf = pMaterial->PdfNM( vToLight, ri, nm, defaultIOR );

			// Optimal MIS training (spectral NEE): contrib includes
			// the geometry factor, so use pdfAlias (area measure)
			// to match.  f2/pdfAlias^2 = (contrib/pdfAlias)^2.
			if( pOptimalMIS && !pOptimalMIS->IsReady() )
			{
				const Scalar f2 = contrib * contrib;
				if( f2 > 0 && pdfAlias > 0 )
				{
					const_cast<OptimalMISAccumulator*>(pOptimalMIS)->Accumulate(
						ri.rast.x, ri.rast.y,
						f2, pdfAlias, kTechniqueNEE );
				}
			}

			if( p_bsdf > 0 )
			{
				Scalar w;
				if( pOptimalMIS && pOptimalMIS->IsReady() )
				{
					const Scalar alpha = pOptimalMIS->GetAlpha( ri.rast.x, ri.rast.y );
					w = MISWeights::OptimalMIS2Weight( p_light, p_bsdf, 1.0 - alpha );
				}
				else
				{
					w = PowerHeuristic( p_light, p_bsdf );
				}
				contrib = contrib * w;
			}
		}

		result = contrib * (rrSurvivalCompensation * risWeight / pdfAlias);
	} while( false );

	// Environment map NEE (spectral path)
	// LIGHT-SOLO Stage 3: env-NEE runs only when solo is inactive OR
	// the environment IS the soloed light -- every other solo target
	// (an explicit light or a mesh luminary) must see zero contribution
	// from the env map, exactly like every other non-target light.
	if( ( soloKind == SoloKind::None || soloKind == SoloKind::Environment ) &&
		pEnvSampler && pEnvSampler->IsValid() && pEnvironmentMap )
	{
		Vector3 envDir;
		Scalar envPdf;
		pEnvSampler->Sample( sampler.Get1D(), sampler.Get1D(), envDir, envPdf );

		const Scalar cosEnv = isVolumeScatter ? Scalar(1.0) :
			Vector3Ops::Dot( envDir, ri.vNormal );

		// Optimal MIS training: count every spectral env NEE attempt that
		// produced a valid sample (envPdf > 0), including below-hemisphere
		// samples which are zero-valued draws for the denominator.
		if( envPdf > 0 && pOptimalMIS && !pOptimalMIS->IsReady() )
		{
			const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
				ri.rast.x, ri.rast.y, kTechniqueNEE );
		}

		if( (isVolumeScatter || cosEnv > 0) && envPdf > 0 )
		{
			// envShadowTNM carries the Fresnel transmittance when
			// transparent shadows are enabled (else 1.0).
			bool envShadowed = false;
			Scalar envShadowTNM = 1.0;
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToEnv( ri.ptIntersection, envDir );
				envShadowed = ShadowOccludedNM( caster, rayToEnv, RISE_INFINITY, nm, envShadowTNM );
			}

			if( !envShadowed )
			{
				const Ray envRay( ri.ptIntersection, envDir );
				const Scalar Le = pEnvironmentMap->GetRadianceNM( envRay, nullRasterizerState, nm );
				const Scalar f = brdf.valueNM( envDir, ri, nm );
				Scalar envContrib = Le * f * cosEnv / envPdf * envShadowTNM;

				// Apply medium transmittance for environment ray.
				// Multi-medium shadow transmittance.
				{
					envContrib *= EvalShadowTransmittanceNM( envRay, RISE_INFINITY, pMedium,
						pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm, pMediumStack );
				}

				// Optimal MIS training (spectral env NEE): use full
				// integrand.  envContrib = Le*BSDF*cos*Tr/envPdf,
				// so fullIntegrand = envContrib * envPdf.
				if( pOptimalMIS && !pOptimalMIS->IsReady() && pMaterial )
				{
					const Scalar fullIntegrand = envContrib * envPdf;
					const Scalar f2 = fullIntegrand * fullIntegrand;
					if( f2 > 0 && envPdf > 0 )
					{
						const_cast<OptimalMISAccumulator*>(pOptimalMIS)->Accumulate(
							ri.rast.x, ri.rast.y,
							f2, envPdf, kTechniqueNEE );
					}
				}

				if( pMaterial )
				{
					static const IORStack defaultIOR( 1.0 );
					const Scalar pBsdf = pMaterial->PdfNM( envDir, ri, nm, defaultIOR );
					if( pBsdf > 0 )
					{
						Scalar w;
						if( pOptimalMIS && pOptimalMIS->IsReady() )
						{
							const Scalar alpha = pOptimalMIS->GetAlpha( ri.rast.x, ri.rast.y );
							w = MISWeights::OptimalMIS2Weight( envPdf, pBsdf, 1.0 - alpha );
						}
						else
						{
							w = PowerHeuristic( envPdf, pBsdf );
						}
						envContrib = envContrib * w;
					}
				}

				result += envContrib;
			}
		}
	}

	return result;
}
