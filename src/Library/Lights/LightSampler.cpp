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
#include "PointLight.h"
#include "SpotLight.h"
#include <typeinfo>
#include <cmath>
#include <vector>
#include <optional>
#include "../Utilities/ISampler.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IContinuationClosure.h"
#include "../Interfaces/IEmitter.h"
#include "../Materials/NullBoundaryMaterial.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/IRayCaster.h"
#include "../Rendering/RayCaster.h"		// concrete RayCaster — dynamic_cast target for transparent (Fresnel-attenuated) shadow rays
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/GradedIndexMedium.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/PathTransportUtilities.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/OptimalMISAccumulator.h"
#include "../Utilities/MISWeights.h"
#include "../Utilities/EquiangularSampler.h"
#include "../Shaders/SSS/SSSContainment.h"
#include "../Interfaces/ISurfaceSignalProvider.h"	// SurfaceCurvatureDemand / SurfaceSignalDemand -- the emitter-probe gate
#include "../Objects/CSGObject.h"
#include "../Interfaces/IObjectManager.h"		// the `signals.pScene` the probe stamps

using namespace RISE;
using namespace RISE::Implementation;

namespace {
template<class Light> Scalar PhysicalProductNM(const void* light,const Vector3& out,const Scalar nm,
    std::initializer_list<PhysicalLightSpectrum::ProductTerm> factors,
    std::initializer_list<PhysicalLightSpectrum::ProductTerm> divisors)
{
    return static_cast<const Light*>(light)->PhysicalEmissionProductNM(out,nm,factors,divisors);
}
void BindPhysicalProduct(LightEntry& entry)
{
    // Unknown subclasses keep their virtual emitted-radiance semantics.
    if(typeid(*entry.pLight)==typeid(PointLight)) {
        const auto* point=dynamic_cast<const PointLight*>(entry.pLight);
        if(point->GetPhysicalSpectrum()) {entry.physicalLight=point;entry.physicalProductNM=&PhysicalProductNM<PointLight>;}
    } else if(typeid(*entry.pLight)==typeid(SpotLight)) {
        const auto* spot=dynamic_cast<const SpotLight*>(entry.pLight);
        if(spot->GetPhysicalSpectrum()) {entry.physicalLight=spot;entry.physicalProductNM=&PhysicalProductNM<SpotLight>;}
    }
}
}

// Effective bindings follow the same override rule as CSG intersection:
// an explicit material ends inheritance, including an OPAQUE override.
// Start only at visible scene roots; hidden operands are reachable here only
// through their composite. Snapshot CSG preserves this same operand tree.
static bool HasEffectiveAlphaCoverage( const IObject* object )
{
    if( !object ) return false;
    struct AlphaScan : public IEnumCallback<IMaterial>
    {
        bool found = false;
        bool operator()( const IMaterial& material ) override
        {
            if( material.HasAlphaCoverage() ) {
                found = true;
                return false;
            }
            return true;
        }
    } scan;
    object->EnumerateEffectiveMaterials( scan );
    return scan.found;
}

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
//
// DL-05: @a bDeltaLight is true ONLY at the delta-light arm (omni /
// spot).  It lets that arm's shadow ray see through a thin weave's delta
// gap lobe (RayCaster::CastShadowRayAuto); the mesh-luminary and env
// arms pass false and keep a binary shadow there, because PT's
// BSDF-sampled continuation already reaches those lights THROUGH the gap
// at MIS weight 1 -- seeing through it here too would count the path
// twice.  No BSDF-sampled strategy can ever hit a delta light, so for the
// delta arm this shadow ray is the path's only estimator.
// ----------------------------------------------------------------
static bool ShadowOccludedRGB(
	const IRayCaster& caster,
	const Ray& ray,
	const Scalar dHowFar,
	RISEPel& transmittance,
	const bool bDeltaLight,		// DL-05: see RayCaster::CastShadowRayAuto
    ISampler& sampler, MediumBoundaryHits* boundaries, Scalar physicalDistance = -1,
	GradedIndexMedium::ShadowSegmentTrack* pGradedTrack = 0,	// DL-292: see RayCaster::CastShadowRayAuto
	const Point3* pSegmentEnd = 0,
	const bool bSMSCoversLight = false	// DL-344: SMS was evaluated at THIS point for this light (see RayCaster::CastShadowRayAuto)
	)
{
	// Delegate to RayCaster::CastShadowRayAuto, the single source of truth for
	// the binary-vs-Fresnel-transmittance choice (shared with the directional /
	// ambient Step-1 lights so the flag is honored uniformly across light types).
	const RayCaster* pRC = dynamic_cast<const RayCaster*>( &caster );
	if( pRC )
	{
		return pRC->CastShadowRayAutoSampled( ray, dHowFar, false, 0.0, transmittance, bDeltaLight, sampler, boundaries, physicalDistance, 0, pGradedTrack, pSegmentEnd , bSMSCoversLight );
	}
	transmittance = RISEPel( 1.0, 1.0, 1.0 );
	return caster.CastShadowRaySampled( ray, dHowFar, sampler, boundaries, physicalDistance );
}

static bool ShadowOccludedNM(
	const IRayCaster& caster,
	const Ray& ray,
	const Scalar dHowFar,
	const Scalar nm,
	Scalar& transmittance,
	const bool bDeltaLight,		// DL-05: see RayCaster::CastShadowRayAuto
    ISampler& sampler, MediumBoundaryHits* boundaries, Scalar physicalDistance = -1,
	GradedIndexMedium::ShadowSegmentTrack* pGradedTrack = 0,	// DL-292: see RayCaster::CastShadowRayAuto
	const Point3* pSegmentEnd = 0,
	const bool bSMSCoversLight = false	// DL-344: SMS was evaluated at THIS point for this light (see RayCaster::CastShadowRayAuto)
	)
{
	// Delegate to RayCaster::CastShadowRayAuto (see ShadowOccludedRGB).
	const RayCaster* pRC = dynamic_cast<const RayCaster*>( &caster );
	if( pRC )
	{
		RISEPel t( 1.0, 1.0, 1.0 );
		const bool occluded = pRC->CastShadowRayAutoSampled( ray, dHowFar, true, nm, t, bDeltaLight, sampler, boundaries, physicalDistance, 0, pGradedTrack, pSegmentEnd , bSMSCoversLight );
		transmittance = t.r;	// NM path fills all 3 channels equally
		return occluded;
	}
	transmittance = 1.0;
	return caster.CastShadowRaySampled( ray, dHowFar, sampler, boundaries, physicalDistance );
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
	MarchObserver& marchObserver,
	//! Accepted medium-boundary records from the SAME stochastic-alpha
	//! visibility decision as the shadow ray (master's alpha-coverage
	//! path).  When non-null the walk consumes these records in ray order
	//! instead of re-intersecting the scene, so a medium boundary the
	//! shadow ray alpha-rejected is not counted here either.  The records
	//! hold only medium-bearing objects; null keeps the exact
	//! re-intersecting walk.
	const MediumBoundaryHits* boundaries = 0
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

	// Record-mode ordering: the records are collected along the caller's
	// segment, but may have been collected from the opposite end.
	const bool reverseBoundaries = boundaries && boundaries->size() > 1 &&
		Vector3Ops::Dot( Vector3Ops::mkVector3( boundaries->back().BoundaryPoint(),
			boundaries->front().BoundaryPoint() ), ray.Dir() ) < 0;
	size_t boundaryStep = 0;

	// The boundary this walk last crossed, and in which direction.  A closed
	// boundary cannot be crossed twice in a row in the same direction, so a
	// live hit on the same object with the same facing is that face being
	// re-reported from the exact point the walk is standing on (local-frame
	// rounding can leave its root just above the representability bound).
	const IObject* pLastCrossObj = 0;
	bool bLastCrossEntering = false;

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
		// After the first segment the origin is an exact boundary this walk
		// computed (see RayIntersectionGeometric::bExactBoundaryOrigin).
		ri.geometric.bExactBoundaryOrigin = segStart > 0;
		if( boundaries ) {
			if( boundaryStep < boundaries->size() ) {
				ri = (*boundaries)[ reverseBoundaries
					? boundaries->size()-1-boundaryStep : boundaryStep ];
				++boundaryStep;
				// Records keep their own ray; re-express the exact boundary
				// point as a local distance from this segment's start.
				Scalar local = Vector3Ops::Dot( Vector3Ops::mkVector3(
					ri.BoundaryPoint(), ray.origin ), ray.Dir() ) - segStart;
				if( local < 0 ) local = 0;	// coincident records: zero-length segment
				ri.geometric.surfaceRange = local;
			} else {
				ri.geometric.bHit = false;
			}
		} else {
			pObjects->IntersectRay( ri, true, true, true );
		}

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
		// Records may legitimately coincide (a finite list cannot stall);
		// a live intersection must strictly progress.
		if( !pHitObj || !std::isfinite( boundaryDist ) ||
			( boundaries ? (boundaryDist < segStart) : !(boundaryDist > segStart) ) ) {
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

		// DL-70 / DL-97: medium-stack push/pop uses the TRUE, ray-independent
		// geometric side (`TrueGeomFacing`).  A hit whose normal is
		// RAY-DERIVED (HairGeometry) has no true side and no interior, and
		// its chord is zero-length by contract, so it leaves the topology
		// unchanged (see tests/HairInteriorMediumSkipTest.cpp).
		if( !ri.geometric.HasTrueGeomSide() ) {
			segStart = boundaryDist;
			continue;
		}
		const IMedium* pObjMedium = pHitObj->GetInteriorMedium();
		const Scalar ndotd = ri.geometric.TrueGeomFacing( ray.Dir() );
		if( pHitObj == pLastCrossObj &&
			( ndotd < 0 ) == bLastCrossEntering ) {
			// Re-report of the face just crossed: no topology change.  This
			// covers both live queries and replayed alpha-sampled records,
			// whose collection steps one ulp past each accepted boundary and
			// can record the same face again when its root is still ahead.
			segStart = boundaryDist;
			continue;
		}
		pLastCrossObj = pHitObj;
		bLastCrossEntering = ndotd < 0;
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
	const IORStack* pOriginStack,
	const MediumBoundaryHits* boundaries
	)
{
	RISEPel Tr;
	NoMarchProposalObserver marchObserver;
	EvaluateShadowMediumTransmittanceImpl(
		ray, maxDist, pOriginMedium, pOriginMediumObject, pScene,
		bSceneHasObjectMedia, ShadowTransmittanceRGB(), Tr, pOriginStack, 0,
		eShadowBoundaryIgnoreGeometry, marchObserver, boundaries );
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
	const IORStack* pOriginStack,
	const MediumBoundaryHits* boundaries
	)
{
	Scalar Tr;
	NoMarchProposalObserver marchObserver;
	EvaluateShadowMediumTransmittanceImpl(
		ray, maxDist, pOriginMedium, pOriginMediumObject, pScene,
		bSceneHasObjectMedia, ShadowTransmittanceNM( nm ), Tr, pOriginStack, 0,
		eShadowBoundaryIgnoreGeometry, marchObserver, boundaries );
	return Tr;
}


using PathTransportUtilities::PowerHeuristic;

//////////////////////////////////////////////////////////////////////
//
// THE EMITTER-RECORD PROBE
// (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5, slice S3)
//
// THE PROBLEM, in one sentence: `IObject::UniformRandomPoint` returns a
// position, a normal and a UV, so every emitter record built from one is
// missing exactly the fields the geometry-signal builtins read -- and an
// emissive material keyed on `curv` / `occlusion(r)` / `proximity(r)`
// therefore read its LIVE value when a camera ray hit the emitter and its
// NEUTRAL value when NEE or a light-subpath root reached the same point on
// the same surface in the same frame.
//
// THE FIX, in one sentence: fire ONE object-level closest-hit at the
// luminary along the ray the record is built for, and if it lands on the
// sampled point, keep the record's shading payload.
//
// WHY AN INTERSECTION AND NOT A RECONSTRUCTION.  Every alternative is a
// fabrication.  `derivatives` would have to be re-derived from the
// geometry's parameterisation (which `UniformRandomPoint` does not report
// -- it returns a UV, not the chart the UV belongs to, and the two differ
// on every composite); the own-surface `signals` half carries a
// `primId` + barycentrics that only the intersector knows; the
// cross-object triple needs the WORLD point that the winning record
// carries.  The one thing in the engine that produces all three
// consistently is `Object::IntersectRay`, so that is what we call.  This
// is the same reasoning that put the S1 fix in `BDPTVertex` rather than
// in a recomputation at the rebuild site.
//
// WHAT THE PROBE COSTS, and when.  One object-level closest-hit per NEE
// sample and per light-subpath root -- an `IObject::IntersectRay` against
// ONE object, not a scene traversal.  It is paid ONLY when
// `SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any()`, i.e. only
// when some live compiled expression in the process actually calls one of
// the six signal builtins.  Both are relaxed atomic loads that are already
// on the per-hit path elsewhere; with the gate closed the rendered output
// is unchanged, no extra ray is cast, and the luminary's bounding box is
// not even read.  (An earlier draft said "takes the pre-S3 code path
// byte-for-byte" -- an overclaim: the sites still default-construct a
// payload and still fill `ptObjIntersec`, which is UNGATED by design.  The
// checkable claim is the one above.)
//
// WHAT IS NOT GATED, and must not be.  `ptObjIntersec` -- the expression
// VM's `Po` -- is filled at every emitter-record site by
// `LightSampler::EmitterObjectPoint`, with no gate and no ray, because
// `Po` is read by painters that register no signal demand
// (`expression_painter` exposes it, `voronoi3d_painter` and
// `mapping_painter` key on it) and this gate is PROCESS-WIDE.  Riding it
// on the payload, as this slice's first draft did, made an emissive
// material keyed on `Po` render 2.94x differently depending on whether
// some unrelated painter elsewhere in the process kept a demand alive.
// Filling it unconditionally also closes a PRE-EXISTING inconsistency
// that predates the slice and reaches plain PT: a camera ray that HIT the
// emitter got a live `Po`, every NEE record built from a sampled point
// got `(0,0,0)`.  CLOSED FOR AN `Object`, NOT FOR A CSG COMPOSITE: a
// composite has no single object frame, so `EmitterObjectPoint` returns
// the record's existing `(0,0,0)` while a direct hit on the same
// composite still gets the WINNING OPERAND's object point through
// `CSGObject::AdoptCsgSurfacePayload`.  That residual is untouched by
// this slice and is named wherever the closure is claimed (round-2
// transport review, H2 P2-7).
//
// WHAT THE GATE-CLOSED PATH STILL COSTS, stated rather than implied.  The
// probe itself costs nothing: `EmitterProbeWanted()` is two relaxed
// atomic loads and every call site asks it before building anything.  The
// `Po` fill is NOT free and NOT gated -- per emitter record it is two
// virtual calls (`GetGeometry`, `GetFinalInverseTransformMatrix`), a
// 128-byte `Matrix4` returned BY VALUE, and one 4x4 point transform, on
// the NEE path and on `ManifoldSolver`'s per-shading-point `SampleLight`
// alike.  That is the honest ungated cost, and it is the price of `Po`
// not depending on a process-wide gate.  (Until the round-2 review the
// two NEE sites ALSO default-constructed a 416-byte
// `EmitterSurfacePayload` per light sample with the gate closed, and
// fetched the scene's object manager; both are now inside the gate.)
//
// WHAT THE PROBE COSTS, MEASURED.  PT, 160x160, 256 spp, one small SDF
// sphere luminary that every visible receiver point NEE-samples every
// sample -- a deliberate worst case, since nothing in the frame dilutes
// the per-light-sample probe.  Interleaved 5-run means on an
// Apple-silicon Mac: probe ON 985 +- 23 ms, probe forced off with
// everything else identical 837 +- 29 ms => +149 ms, +17.8 %.  The gate
// CLOSED (the same scene with the signal baked to a literal, so no
// painter registers a demand) runs at 821 +- 20 ms, within one stddev of
// the probe-off figure -- i.e. the gate itself costs nothing measurable.
//
// THE GATE LIVES IN `ProbeEmitterSurface` itself, not only at the call
// sites, so a new site cannot be added without it.  The sites ask it as
// well, but only to skip allocating a payload they will not use.
//
//////////////////////////////////////////////////////////////////////

namespace
{
	//! THE SCALE the probe's constants are relative to: the luminary's
	//! WORLD bounding-box diagonal, the same characteristic length
	//! `scaleHint` and the `radiusFraction` convention use.
	//!
	//! \return the diagonal, or 0 when there is no usable one -- an
	//!         UNBOUNDED luminary (an infinite plane) has no finite
	//!         diagonal and therefore no meaningful tolerance, and a
	//!         degenerate one has no scale at all.  Callers REFUSE on 0
	//!         rather than fall back to an absolute length.
	Scalar EmitterProbeScale( const IObject* pLum )
	{
		if( !pLum ) {
			return 0;
		}
		const BoundingBox bb = pLum->getBoundingBox();
		const Vector3 diagV = Vector3Ops::mkVector3( bb.ur, bb.ll );
		const Scalar diag = Vector3Ops::Magnitude( diagV );
		if( !RISE::IsFiniteDouble( static_cast<double>( diag ) ) || diag <= 0 ) {
			return 0;
		}
		return diag;
	}
}

bool LightSampler::EmitterProbeWanted()
{
	// The design's first fixed constraint: two relaxed atomic loads, and
	// everything downstream of them -- the bounding-box query, the
	// standoff derivation, the ray -- is behind this.  `ProbeEmitterSurface`
	// asks it itself; it is public only so a hot call site can also skip
	// the 416-byte payload it would otherwise default-construct.
	return SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any();
}

bool LightSampler::ProbeEmitterSurface(
	const IObject*			pLum,
	const IObjectManager*	pObjects,
	const Point3&			ptOnLum,
	const Vector3&			normal,
	EmitterSurfacePayload&	outPayload
	)
{
	// ONE PROBE, ONE REFUSAL PREDICATE.  Until the round-2 transport
	// review (H2 P1-1) there were TWO public entry points: this
	// normal-aligned one, used by `SampleLight` (and therefore by the BDPT
	// light-subpath root, its two NM twins and VCM's light vertex), and an
	// along-`vToLight` one used by the two NEE sites.  They refused in
	// different places -- which meant PT could read an emitter's signals
	// live while BDPT and VCM read them neutral on the same surface in the
	// same frame, or the reverse.  The along-ray entry point is gone and
	// the NEE sites call this.
	//
	// Gate FIRST, so the bounding-box query below is not paid by a scene
	// with no signal consumer.
	if( !EmitterProbeWanted() ) {
		return false;
	}
	const Scalar diag = EmitterProbeScale( pLum );
	if( diag <= 0 ) {
		return false;
	}

	const Scalar nrm = Vector3Ops::Magnitude( normal );
	if( !( nrm > NEARZERO ) ) {
		return false;
	}
	const Vector3 nUnit = normal * ( Scalar( 1 ) / nrm );

	const Vector3 dir( -nUnit.x, -nUnit.y, -nUnit.z );

	const Scalar tol = kEmitterProbeAcceptFraction * diag;

	// THE STANDOFF, DERIVED FROM THE GEOMETRY.
	//
	// This slice's first draft used a flat `kEmitterProbeStandoffFraction *
	// diag` and justified it as "~9 orders above every geometry's
	// SelfHitRootFloor".  `SelfHitRootFloor` is VIRTUAL with eleven in-tree
	// overrides, and that claim only held for the interface default
	// (`NEARZERO * (1 + |o|_1)`).  `SDFGeometry`'s is
	// `min( 2 * m_eps / shrink / cosI, 0.5 * diagonal )` with
	// `m_eps = max( diagonal * m_epsFrac, 1e-6 )`: 4e-3 of the diagonal at
	// `epsilon 0.002`, 1e-3 of it -- exactly `kEmitterProbeStandoffFraction`
	// itself, not "far above" it -- for a part authored `scale 0.1 1 1` at
	// the DEFAULT epsilon, and up to 0.5 of it at the documented
	// grazing/shrink worst case.  Below its own floor the SDF treats the
	// probe origin as spawned ON the surface and marches it forward in
	// `4 * m_eps` steps until it clears the band -- straight past the face
	// it was aimed at.  MEASURED on tests/SignalEmitterRecordTest's family
	// E before this was fixed: BDPT 37.4 %, VCM 35.1 % off their baked
	// controls.
	//
	// FRAME.  `IObject::SelfHitRootFloor` answers in the luminary's OWN
	// LOCAL frame and in that frame's units, so the query is transformed in
	// and the answer transformed back -- dividing by the direction stretch
	// `s = |M_inv * dir|`, exactly the conversion
	// `CSGObject::SelfHitRootFloor` performs on each operand.  The origin
	// handed in is `ptOnLum` rather than the (not yet known) standoff
	// point, which is the right argument anyway: the floors that depend on
	// the origin depend on it only through its COORDINATE MAGNITUDE, and a
	// point and a point one floor away from it agree there to many digits.
	// The normal is carried by the inverse-transpose, i.e. the transpose of
	// the forward matrix.
	//
	// THE CUSHION IS NOT SLOP, and it is not the acceptance tolerance
	// either.  The floor is a bound measured from the TRUE surface, and
	// what the caller holds is a SAMPLED point that may not be on it --
	// `UniformRandomPoint` Newton-projects, it does not solve exactly.
	// `SDFGeometry::March` steps off when `|Map(o)| <= surfBand`, and `Map`
	// is positive OUTSIDE: with `o = ptOnLum + s * n` and the sample's true
	// displacement a signed `delta` (positive = outside), `Map(o) = s +
	// delta`, so `margin * floor` alone under-shoots on roughly half the
	// samples -- the half whose projection landed INSIDE (`delta < 0`,
	// clearance lowered to `s - |delta|`); a projection that landed OUTSIDE
	// raises the clearance instead and is the safe half.  The first
	// implementation used
	// `kEmitterProbeAcceptFraction * diag` (1 % of the diagonal) for this,
	// which both over-paid by 10x and made `kEmitterProbeStandoffFraction`
	// unreachable.  `kEmitterProbeStandoffCushionFraction` is the measured
	// replacement; see its declaration for the measurement and for what the
	// resulting interception band costs.
	Scalar standoff = kEmitterProbeStandoffFraction * diag;
	{
		const Matrix4 mxInv = pLum->GetFinalInverseTransformMatrix();
		const Vector3 dLocalUnnorm = Vector3Ops::Transform( mxInv, dir );
		const Scalar sStretch = Vector3Ops::Magnitude( dLocalUnnorm );
		if( sStretch > NEARZERO ) {
			const Point3 oLocal = Point3Ops::Transform( mxInv, ptOnLum );
			const Vector3 dLocal(
				dLocalUnnorm.x / sStretch,
				dLocalUnnorm.y / sStretch,
				dLocalUnnorm.z / sStretch );
			const Matrix4 mxFwdT = Matrix4Ops::Transpose( pLum->GetFinalTransformMatrix() );
			const Vector3 nLocalUnnorm = Vector3Ops::Transform( mxFwdT, nUnit );
			const Scalar nLocalMag = Vector3Ops::Magnitude( nLocalUnnorm );
			const Vector3 nLocal = ( nLocalMag > NEARZERO )
				? Vector3( nLocalUnnorm.x / nLocalMag,
				           nLocalUnnorm.y / nLocalMag,
				           nLocalUnnorm.z / nLocalMag )
				: nUnit;
			const Scalar floorWorld =
				pLum->SelfHitRootFloor( oLocal, dLocal, nLocal ) / sStretch;
			if( !RISE::IsFiniteDouble( static_cast<double>( floorWorld ) ) ) {
				return false;			// a floor we cannot reason about
			}
			standoff = std::max( standoff,
				kEmitterProbeStandoffMargin * floorWorld
					+ kEmitterProbeStandoffCushionFraction * diag );
		}
	}

	// REFUSE rather than clamp when the derived standoff is wider than the
	// luminary itself: the origin would then sit outside the shape along a
	// ray that has to cross the whole of it, the acceptance test could only
	// ever see a far face, and "change nothing" is the honest answer.
	//
	// WHICH GEOMETRY CAN ACTUALLY REACH THIS, for a UNIFORMLY-TRANSFORMED
	// luminary.  Not an SDF, despite what an earlier draft of this comment
	// said: with the cushion at 0.1 % of the diagonal,
	// `1.01 * floor + 0.001 * diag > diag` needs `floor > 0.989 * diag`,
	// and `SDFGeometry` caps its own LOCAL floor at `0.5 * localDiagonal`
	// -- which only bounds `floorWorld` the same way when `sStretch` is 1.
	// A CSG LUMINARY CAN: `CSGObject::SelfHitRootFloor` returns the worst
	// OPERAND's floor (converted by that operand's own direction stretch)
	// with NO cap by the composite's bounding box, so an operand that is
	// mostly clipped away -- a large SDF intersected down to a small solid
	// -- contributes a floor measured against ITS OWN diagonal while `diag`
	// here is the composite's.  A NON-UNIFORMLY SCALED SDF luminary reaches
	// it too, by the same division: `floorWorld = floor_local / sStretch`
	// above, and a shrink-poisoned part inside an object stretched thin
	// along the probe direction can push `sStretch` small enough to send
	// `floorWorld` past `diag` even though `floor_local` never left its own
	// `0.5 * localDiagonal` cap.
	if( !( standoff > 0 ) || standoff > diag ) {
		return false;
	}

	// Stand off along +n and fire back along -n.  The travel limit is twice
	// the standoff plus the acceptance tolerance: enough to reach the
	// surface and a little past it.  It is ADVISORY, not a hard clamp --
	// `Object::IntersectRay` applies `dHowFar` to its bounding-box pre-test
	// and then lets the geometry march as far as it likes -- so the
	// acceptance test, not this limit, is what rejects a far-side hit.
	const Point3 origin(
		ptOnLum.x + nUnit.x * standoff,
		ptOnLum.y + nUnit.y * standoff,
		ptOnLum.z + nUnit.z * standoff );
	const Ray probe( origin, dir );
	const Scalar probeMaxDist = standoff * Scalar( 2 ) + tol;

	// The closest-hit itself.  Both faces are admitted because a luminary
	// may legitimately be sampled on either side (a two-sided emissive
	// quad); no exit info is needed.  A LOCAL record, exactly as
	// ObjectManager::RayElementIntersection uses one, so nothing this
	// function does can touch a caller's record on a refusal.
	RayIntersection probeRI( probe, nullRasterizerState );
	pLum->IntersectRay( probeRI, probeMaxDist, true, true, false );
	if( !probeRI.geometric.bHit ) {
		return false;
	}

	// DID IT LAND ON THE SAMPLED POINT?  See the declaration for why "a hit
	// on the right object" is not enough.
	//
	// WHAT THE 1 % OF `diag` TOLERANCE IS ACTUALLY ABSORBING, now that the
	// probe is normal-aligned everywhere.  Not a surface-extraction
	// disagreement: this slice's first draft claimed an SDF luminary's
	// sampled point comes off a marching-cubes extraction while the probe
	// lands on the ray-marched zero set, and that is not how
	// `SDFGeometry::UniformRandomPoint` works -- it Newton-projects each
	// sample onto the SAME ray-marched zero set (measured residual ~3e-5 of
	// the diagonal; this is the one place that number is stated).  And NOT
	// incidence either, which is what the second draft claimed: the probe
	// line runs along the sampled point's own normal and therefore passes
	// through that point exactly, meeting the surface at incidence 1.  What
	// is left is the projection residual plus the intersector's own
	// perpendicular surface band at normal incidence -- for an SDF its hit
	// epsilon over the field's Lipschitz shrink, for every analytic
	// primitive ulp-scale.
	//
	// THE MARGIN AGAINST A WRONG-PART HIT is two orders: two distinct parts
	// of a non-convex luminary are O(diag) apart and this admits
	// 1e-2 * diag.  What the tolerance does NOT protect against is a second
	// surface of the same luminary inside the standoff band along +n; see
	// `kEmitterProbeStandoffCushionFraction`.
	const Vector3 miss = Vector3Ops::mkVector3(
		probeRI.geometric.ptIntersection, ptOnLum );
	if( Vector3Ops::Magnitude( miss ) > tol ) {
		return false;
	}

	// THE CROSS-OBJECT TRIPLE, stamped with EXACTLY the values
	// `ObjectManager::IntersectRay` stamps at the tail of a real traversal
	// (`pScene` = the manager that owns the scene, `pSelf` = the record's
	// own `pObject` so the two identities cannot disagree, `ptWorld` =
	// `ptIntersection`).  This record never went through the manager --
	// the probe deliberately asks ONE object rather than traversing --
	// which is precisely why the triple has to be stamped here; without
	// it `proximity()` / `interior()` would read their neutral 0 off a
	// record whose own-surface half is live, which is a worse lie than
	// the all-neutral record it replaces.
	//
	// `pObjects` may be null (a caller with no scene): then the triple
	// stays empty and the cross-object pair honestly reads neutral while
	// `curv` and the own-surface three still read live.
	probeRI.geometric.signals.pScene  = pObjects;
	probeRI.geometric.signals.pSelf   = probeRI.pObject;
	probeRI.geometric.signals.ptWorld = probeRI.geometric.ptIntersection;

	// `ptObjIntersec` is deliberately NOT taken from the probe -- see
	// `EmitterSurfacePayload`'s declaration.  Every call site fills it
	// UNGATED through `LightSampler::EmitterObjectPoint`, which needs no
	// ray at all, so `Po` cannot depend on this process-wide gate.
	outPayload.derivatives   = probeRI.geometric.derivatives;
	outPayload.channel       = probeRI.geometric.signals;
	outPayload.txFootprint   = probeRI.geometric.txFootprint;
	outPayload.valid         = true;
	return true;
}

Point3 LightSampler::EmitterObjectPoint(
	const IObject*	pLum,
	const Point3&	ptWorld,
	const Point3&	fallback
	)
{
	// NO GATE and NO RAY -- see the declaration.  `Po` is not signal state.
	if( !pLum ) {
		return fallback;
	}
	// A luminary with no single geometry has no single OBJECT FRAME either:
	// `CSGObject::IntersectRay` leaves `ptObjIntersec` in the winning CHILD
	// operand's frame and clears `pmxWorldToObject` for exactly this
	// reason.  Which child wins is a property of the HIT, not of the point,
	// so there is nothing honest to compute here -- keep what the record
	// already had.
	if( !pLum->GetGeometry() ) {
		return fallback;
	}
	return Point3Ops::Transform( pLum->GetFinalInverseTransformMatrix(), ptWorld );
}

bool LightSampler::LuminaryIsTwoSided( const IObject* pLum )
{
	if( !pLum ) {
		return false;
	}
	const IGeometry* pGeom = pLum->GetGeometry();
	return pGeom && pGeom->IsDoubleSided();
}

void LightSampler::ApplyEmitterSurface(
	RayIntersectionGeometric&		rig,
	const EmitterSurfacePayload&	payload
	)
{
	if( !payload.valid ) {
		// THE FALLBACK, and it is deliberately a no-op: the caller's
		// hand-built record keeps the `derivatives`, `signals` and
		// `txFootprint` it already had, so a refused or gated-off probe
		// leaves the rendered output unchanged.  (Not "bit-for-bit the
		// pre-S3 render", as an earlier draft said: the call sites also
		// write `ptObjIntersec`, which is ungated by design and lands
		// whether or not this function does anything.)
		return;
	}
	rig.derivatives   = payload.derivatives;
	// FORWARDS the object manager's own convention rather than replacing a
	// stamp: this record never passed through ObjectManager::IntersectRay
	// (a sampled point is not a traversal), and `payload.channel` was
	// written by `ProbeEmitterSurface` above with exactly the values that
	// function stamps.  This is the sole reason LightSampler.cpp appears in
	// SourceHygieneTest's `signals` writer set -- see its comment table.
	rig.signals       = payload.channel;
	rig.txFootprint   = payload.txFootprint;
	// NOT `ptObjIntersec`: the call sites set it themselves, ungated.
}

void LightSampler::FillEmitterRecord(
	RayIntersectionGeometric&	rig,
	const Point3&				position,
	const Vector3&				normal,
	const Point2&				coord,
	const Point3&				ptObjIntersec,
	const IObject*				pLum,
	const ICamera*				pCamera
	)
{
	rig.bHit          = true;
	rig.ptIntersection = position;
	rig.vNormal       = normal;
	// `normal` is `UniformRandomPoint`'s normal (the interpolated vertex
	// normal on a mesh luminary with per-vertex normals, the face normal
	// otherwise -- `GeometricUtilities::PointOnTriangle`).  No Phong/bump
	// modifier runs on an emitter record, so mirroring it keeps the record
	// self-consistent for the downstream cosines.
	rig.vGeomNormal   = normal;
	// Sampled emitter records orient `vNormal`/`vGeomNormal` toward the
	// emitting side while `rig.ray` points outward along the emitted ray, so
	// stamp `arrivalGeomFacing = -1` (virtual arrival opposes `vGeomNormal`)
	// to keep `GeomNormalOpposesArrival()` consistent with direct hits.
	rig.arrivalGeomFacing = -1;
	rig.ptCoord       = coord;
	rig.onb.CreateFromW( normal );
	rig.ptObjIntersec = ptObjIntersec;
	if( pLum ) {
		rig.pObjectInfo = pLum->GetObjectInfo();
		rig.pmxWorldToObject = pLum->GetFinalInverseTransformMatrixPtr();
		rig.pmxObjectToWorld = pLum->GetFinalTransformMatrixPtr();
	}
	if( pCamera ) {
		rig.pCamera = pCamera;
	}
}

bool LightSampler::AcceptEmitterAlpha(
    const IObject* luminary, const IObjectManager* objects,
    const Point3& position, const RayIntersectionGeometric& context,
    ISampler& sampler, const RasterizerState* raster )
{
    const IMaterial* material = luminary ? luminary->GetMaterial() : nullptr;
    if( !material || !material->HasAlphaCoverage() ) return true;
    RayIntersectionGeometric alphaRI(context);
    alphaRI.ptIntersection = position;
    alphaRI.signals.pScene = objects;
    alphaRI.signals.pSelf = luminary;
    alphaRI.signals.ptWorld = position;
    if( luminary ) {
        if( !alphaRI.pObjectInfo ) alphaRI.pObjectInfo = luminary->GetObjectInfo();
        if( !alphaRI.pmxWorldToObject ) alphaRI.pmxWorldToObject = luminary->GetFinalInverseTransformMatrixPtr();
        if( !alphaRI.pmxObjectToWorld ) alphaRI.pmxObjectToWorld = luminary->GetFinalTransformMatrixPtr();
    }
    if( raster ) alphaRI.rast = *raster;
    return material->AcceptAlpha(alphaRI, sampler);
}

namespace
{
	//! True iff two exitances agree to floating-point rounding.  The emitter's
	//! cached `averageRadiantExitance()` is `(sum of 100 painter reads) *
	//! (scale/100)` and a point read is `painter * scale`: equal quantities for
	//! a constant painter, but NOT bit-equal for most values (0.1-style
	//! constants do not survive the 100-term sum exactly).  A bit-exact compare
	//! here declared every such constant emitter "different from its own
	//! average" and flipped it to non-uniform.
	inline bool SameExitanceUpToRounding( const RISEPel& a, const RISEPel& b )
	{
		static const Scalar kRel = 1e-9;
		for( int c = 0; c < 3; c++ ) {
			const Scalar scaleMag = r_max( fabs( a[c] ), fabs( b[c] ) );
			if( fabs( a[c] - b[c] ) > kRel * scaleMag ) {
				return false;
			}
		}
		return true;
	}
}

LightSampler::LuminaryExitance LightSampler::AverageLuminaryExitance( const IObject* pLum )
{
	LuminaryExitance result;
	result.average = RISEPel( 0, 0, 0 );
	result.uniform = true;

	const IMaterial* pMaterial = pLum ? pLum->GetMaterial() : 0;
	const IEmitter* pEmitter = pMaterial ? pMaterial->GetEmitter() : 0;
	if( !pEmitter ) {
		return result;
	}
	result.average = pEmitter->averageRadiantExitance();

	// A luminary with no directly-owned geometry (a CSG composite) or no area
	// has no surface to sample: the emitter's own average stands.
	if( !pLum->GetGeometry() || !( pLum->GetArea() > 0 ) ) {
		return result;
	}

	// Deterministic: a 10x10 stratified grid over the two surface coordinates
	// (the same 100 points -- and the same cell centres -- the emitter's own UV
	// grid used, which is what makes a UV-keyed painter on a parametric plane
	// average identically), and a golden-ratio sequence on the third
	// coordinate, which a triangle mesh uses to pick its triangle by area.
	// No render RNG is consumed.
	static const int kGrid = 10;
	RISEPel sum( 0, 0, 0 );
	RISEPel first( 0, 0, 0 );
	bool uniform = true;
	for( int gy = 0; gy < kGrid; gy++ ) for( int gx = 0; gx < kGrid; gx++ ) {
		const int k = gy * kGrid + gx;
		const Scalar z = Scalar( k + 1 ) * Scalar( 0.6180339887498949 );
		const Point3 prand( ( Scalar( gx ) + Scalar( 0.5 ) ) / Scalar( kGrid ),
			( Scalar( gy ) + Scalar( 0.5 ) ) / Scalar( kGrid ), z - Scalar( int( z ) ) );

		Point3 position;
		Vector3 normal;
		Point2 coord;
		pLum->UniformRandomPoint( &position, &normal, &coord, prand );

		RayIntersectionGeometric rig( Ray(), nullRasterizerState );
		FillEmitterRecord( rig, position, normal, coord, EmitterObjectPoint( pLum, position, rig.ptObjIntersec ), pLum );

		const RISEPel v = pEmitter->radiantExitanceAt( rig );
		if( k == 0 ) {
			first = v;
		} else if( !( v.r == first.r && v.g == first.g && v.b == first.b ) ) {
			uniform = false;
		}
		sum = sum + v;
	}

	if( !uniform ) {
		result.average = sum * ( Scalar( 1 ) / Scalar( kGrid * kGrid ) );
		result.uniform = false;
	} else if( !SameExitanceUpToRounding( first, result.average ) ) {
		// Spatially uniform across this luminary (e.g. keyed on per-object
		// ObjectInfo or Location), yet different from the object-free
		// `averageRadiantExitance()`: record this object's true exitance and
		// mark `uniform = false` so callers like PhotonTracer evaluate
		// `radiantExitanceAt(rig)` rather than the object-free average.
		result.average = first;
		result.uniform = false;
	}
	return result;
}

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

void LightSampler::CopyMotionSweepFrom(const LightSampler& source, const IScene& scene)
{
    if (source.pPreparedScene != &scene) return;
    sweptSceneBox = source.sweptSceneBox;
    sweptSceneBoxValid = source.sweptSceneBoxValid;
    sweptLights = source.sweptLights;
    sweptLuminaries = source.sweptLuminaries;
    bMotionSweep = source.bMotionSweep;
}

void LightSampler::SetSpectralSourceDomain(std::shared_ptr<const SpectralSourceDomain> domain)
{
    spectralDomain = std::move(domain);
    if (spectralDomain) {
        risCandidates = 0;
        lightSampleRRThreshold = 0;
        bUseLightBVH = false;
        delete pLightBVH;
        pLightBVH = nullptr;
    }
}

SpectralSourceDomain::Mass LightSampler::EstimateSpectralPower(const ILightPriv& light) const
{
    SpectralSourceDomain::Mass sum;
    // Angular quadrature, not a proof of blackness. The final PMF has a
    // uniform component even if a narrow spot or spectral line is missed.
    for (unsigned int y=0;y<8;++y) for (unsigned int x=0;x<16;++x) {
        const Scalar z = 1-2*(Scalar(y)+.5)/8;
        const Scalar phi = TWO_PI*(Scalar(x)+.5)/16;
        const Scalar r = sqrt(r_max(Scalar(0),1-z*z));
        const Vector3 direction(r*cos(phi),r*sin(phi),z);
        const auto m = spectralDomain->Integrate([&](double nm) { return light.emittedRadianceNM(direction,nm); });
        if (m.positive) sum.AddLog(m.logarithm);
    }
    sum.Multiply(4*PI/128);
    return sum;
}

SpectralSourceDomain::Mass LightSampler::EstimateSpectralPower(const IObject& object) const
{
    SpectralSourceDomain::Mass sum;
    const IEmitter* emitter = object.GetMaterial()->GetEmitter();
    for (unsigned int y=0;y<10;++y) for (unsigned int x=0;x<10;++x) {
        const unsigned int k = y*10+x;
        const Scalar z = Scalar(k+1)*Scalar(.6180339887498949);
        Point3 position; Vector3 normal; Point2 coord;
        object.UniformRandomPoint(&position,&normal,&coord,
            Point3((Scalar(x)+.5)/10,(Scalar(y)+.5)/10,z-Scalar(int(z))));
        RayIntersectionGeometric rig(Ray(),nullRasterizerState);
        FillEmitterRecord(rig,position,normal,coord,EmitterObjectPoint(&object,position,rig.ptObjIntersec),&object,
            pPreparedScene ? pPreparedScene->GetCamera() : nullptr);
        rig.signals.pScene = pPreparedScene ? pPreparedScene->GetObjects() : nullptr;
        rig.signals.pSelf = &object;
        rig.signals.ptWorld = position;
        if (EmitterProbeWanted()) {
            EmitterSurfacePayload surface;
            ProbeEmitterSurface(&object,rig.signals.pScene,position,normal,surface);
            ApplyEmitterSurface(rig,surface);
        }
        const auto m = spectralDomain->Integrate([&](double nm) { return emitter->radiantExitanceAtNM(rig,nm); });
        if (m.positive) sum.AddLog(m.logarithm);
    }
    sum.Multiply(1.0/100);
    sum.Multiply(object.GetArea());
    sum.Multiply(EmitterSides::FaceCount(LuminaryIsTwoSided(&object)));
    return sum;
}

void LightSampler::SetUseLightBVH(
	const bool enable
	)
{
	bUseLightBVH = spectralDomain ? false : enable;
}

void LightSampler::SetRISCandidates(
	const unsigned int M
	)
{
	risCandidates = spectralDomain ? 0 : M;
}

void LightSampler::SetLightSampleRRThreshold(
	const Scalar threshold
	)
{
	lightSampleRRThreshold = spectralDomain ? 0 : threshold;
}

void LightSampler::SetEnvironmentSampler(
	const IRadianceMap* pEnvMap,
	const EnvironmentSampler* pSampler
	)
{
    // Retain both incoming objects before releasing either previous owner.
    // Self-assignment is valid when this sampler owns the final reference;
    // either old object can also be the only owner of the other incoming one.
    if (pEnvMap) pEnvMap->addref();
    if (pSampler) pSampler->addref();
    if (pEnvSampler) pEnvSampler->release();
    if (pEnvironmentMap) pEnvironmentMap->release();
    pEnvironmentMap = pEnvMap;
    pEnvSampler = pSampler;
	RecomputeEnvSelectProbability();
}

void LightSampler::RecomputeEnvSelectProbability()
{
    // Cache the actual class PMF used by root sampling and every MIS query.
    // Legacy proxy power guides importance, but midpoint estimates cannot
    // establish absence. Mixed present classes retain a uniform support share.
	const bool bEnvExists = pEnvSampler && pEnvSampler->IsValid() &&
		pEnvironmentMap && cachedSceneRadius > 0;
    if (spectralDomain) {
        if (!bEnvExists) cachedEnvSelectProb = 0;
        else if (!aliasTable.IsValid()) cachedEnvSelectProb = 1;
        else {
            auto environmentPower = pEnvSampler->SpectralPower();
            environmentPower.Multiply(PI);
            environmentPower.Multiply(cachedSceneRadius);
            environmentPower.Multiply(cachedSceneRadius);
            cachedEnvSelectProb = spectralDomain->Probabilities({spectralFinitePower,environmentPower})[1];
        }
        return;
    }
    const Scalar aliasWeight = static_cast<Scalar>(aliasTable.TotalWeight());
    if (!RISE::IsFiniteDouble(aliasWeight) || aliasWeight < 0)
        throw std::invalid_argument("legacy finite-light power proxy is nonfinite or negative");
    if (bEnvExists) {
        const Scalar angularPower = pEnvSampler->TotalLuminance();
        if (!RISE::IsFiniteDouble(angularPower) || angularPower < 0 ||
            !RISE::IsFiniteDouble(cachedSceneRadius))
            throw std::invalid_argument("legacy environment power/radius proxy is invalid");
        if (!aliasTable.IsValid()) { cachedEnvSelectProb = 1; return; }
        // Power products and their sum can overflow although the ratio is
        // ordinary. Form log masses and a stable logistic ratio; an estimated
        // zero environment mass contributes zero importance, never zero support.
        Scalar probability = 0;
        if (angularPower > 0) {
            const Scalar logEnvironment = std::log(angularPower)+std::log(PI)+2*std::log(cachedSceneRadius);
            const Scalar logFinite = std::log(aliasWeight);
            if (logEnvironment >= logFinite) {
                probability = 1/(1+std::exp(logFinite-logEnvironment));
            } else {
                const Scalar relative = std::exp(logEnvironment-logFinite);
                probability = relative/(1+relative);
            }
        }
        // Mix 5% uniformly across the two present root classes. This is proposal
        // policy, not fabricated physical power, and keeps the remapped legacy
        // class intervals away from 0/1 even for extreme finite power ratios.
        const Scalar support = EnvironmentSampler::UniformSupportFraction();
        cachedEnvSelectProb = support/2+(1-support)*probability;
    } else {
        cachedEnvSelectProb = 0;
    }
}

//////////////////////////////////////////////////////////////////////
// DL-463: motion-blur sweep of the light tables.
//////////////////////////////////////////////////////////////////////

Scalar LightSampler::LuminarySelectionWeight( const IObject* pLum )
{
	const RISEPel power = AverageLuminaryExitance( pLum ).average * pLum->GetArea() *
		EmitterSides::FaceCount( LuminaryIsTwoSided( pLum ) );
	return ColorMath::MaxValue( power );
}

Scalar LightSampler::SweptSelectionWeight( const MotionSweepRecord& rec, const Scalar nominal )
{
	// The mean power over the shutter samples: any positive weight is an
	// unbiased pmf (both sides read this table), but a light that emits
	// at ANY sample time must keep a positive one, or NEE / light tracing
	// never select it while it shines.
	if( rec.samples > 0 && rec.anyPositive ) {
		const Scalar mean = rec.exitanceSum / Scalar( rec.samples );
		if( mean > 0 ) {
			return mean;
		}
	}
	return nominal;
}

void LightSampler::ApplySweptBounds( LightEntry& entry, const MotionSweepRecord& rec )
{
	if( !rec.boundsValid || !rec.coneValid ) {
		return;
	}
	entry.hasMotionBounds = true;
	// Padded by half the largest change between consecutive samples (the
	// TLAS sweep's rule): exact for motion linear between samples, and it
	// covers a light that curves between them by up to that much.
	entry.motionBounds = rec.bounds;
	entry.motionBounds.ll = Point3( rec.bounds.ll.x - rec.boxPad[0], rec.bounds.ll.y - rec.boxPad[1], rec.bounds.ll.z - rec.boxPad[2] );
	entry.motionBounds.ur = Point3( rec.bounds.ur.x + rec.boxPad[0], rec.bounds.ur.y + rec.boxPad[1], rec.bounds.ur.z + rec.boxPad[2] );
	entry.motionConeAxis = rec.coneAxis;
	entry.motionConeHalfAngle = rec.coneHalfAngle >= PI ? rec.coneHalfAngle :
		r_min( Scalar( PI ), rec.coneHalfAngle + rec.anglePad );
}

void LightSampler::ClearMotionSweep()
{
	sweptLights.clear();
	sweptLuminaries.clear();
	sweptSceneBoxValid = false;
	bMotionSweep = false;
}

namespace
{
	void AccumulateSweepRecord(
		LightSampler::MotionSweepRecord& rec,
		const Scalar weight,
		const BoundingBox& box,
		const OrientationCone& cone
		)
	{
		if( weight > 0 && RISE::IsFiniteDouble( weight ) ) {
			rec.exitanceSum += weight;
			rec.anyPositive = true;
		}
		rec.samples++;
		if( rec.boundsValid ) {
			rec.bounds.Include( box );
			const Scalar cur[6] = { box.ll.x, box.ll.y, box.ll.z, box.ur.x, box.ur.y, box.ur.z };
			const Scalar last[6] = { rec.lastBox.ll.x, rec.lastBox.ll.y, rec.lastBox.ll.z, rec.lastBox.ur.x, rec.lastBox.ur.y, rec.lastBox.ur.z };
			for( int a = 0; a < 6; a++ ) {
				const Scalar d = std::fabs( cur[a] - last[a] );
				// A non-finite or huge corner (an infinite luminary) is not
				// padded: its box already spans the axis.
				if( std::isfinite( d ) && d < RISE_INFINITY*0.25 ) {
					rec.boxPad[a % 3] = r_max( rec.boxPad[a % 3], Scalar( 0.5 ) * d );
				}
			}
		} else {
			rec.bounds = box;
			rec.boundsValid = true;
		}
		rec.lastBox = box;
		if( rec.coneValid ) {
			const Scalar c = r_max( Scalar( -1 ), r_min( Scalar( 1 ), Vector3Ops::Dot( rec.lastAxis, cone.axis ) ) );
			rec.anglePad = r_max( rec.anglePad, Scalar( 0.5 ) * std::acos( c ) );
			OrientationCone prev;
			prev.axis = rec.coneAxis;
			prev.halfAngle = rec.coneHalfAngle;
			const OrientationCone merged = OrientationCone::Merge( prev, cone );
			rec.coneAxis = merged.axis;
			rec.coneHalfAngle = merged.halfAngle;
		} else {
			rec.coneAxis = cone.axis;
			rec.coneHalfAngle = cone.halfAngle;
			rec.coneValid = true;
		}
		rec.lastAxis = cone.axis;
	}
}

void LightSampler::AccumulateMotionSample(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries
	)
{
	const ILightManager* pLightMgr = scene.GetLights();
	const std::size_t nLights = pLightMgr ? pLightMgr->getLights().size() : 0;
	if( !bMotionSweep || sweptLights.size() != nLights || sweptLuminaries.size() != luminaries.size() ) {
		sweptLights.assign( nLights, MotionSweepRecord() );
		sweptLuminaries.assign( luminaries.size(), MotionSweepRecord() );
		bMotionSweep = true;
		sweptSceneBoxValid = false;
	}

	if( pLightMgr ) {
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		std::size_t i = 0;
		for( ILightManager::LightsList::const_iterator m = lights.begin(); m != lights.end(); ++m, ++i ) {
			const ILightPriv* l = *m;
			BoundingBox box( l->position(), l->position() );
			box.EnsureBoxHasVolume();
			OrientationCone cone;
			cone.axis = l->emissionDirection();
			cone.halfAngle = l->emissionConeHalfAngle();
			AccumulateSweepRecord( sweptLights[i], ColorMath::MaxValue( l->radiantExitance() ), box, cone );
		}
	}

	// The per-sample luminary weight costs AverageLuminaryExitance (100
	// surface points) per luminary per sweep sample; past a modest count
	// it is skipped -- the frame-time weight (or the floor, below) is an
	// equally unbiased pmf, only the shutter mean's variance benefit is lost.
	static const std::size_t kMaxSweptLuminaryWeights = 64;
	const bool bWeighLuminaries = luminaries.size() <= kMaxSweptLuminaryWeights;
	for( std::size_t li = 0; li < luminaries.size(); li++ ) {
		const IObject* pLum = luminaries[li].pLum;
		if( !pLum->GetMaterial() || !pLum->GetMaterial()->GetEmitter() ) {
			continue;
		}
		BoundingBox box = pLum->getBoundingBox();
		box.EnsureBoxHasVolume();
		AccumulateSweepRecord( sweptLuminaries[li], bWeighLuminaries ? LuminarySelectionWeight( pLum ) : Scalar( 0 ),
			box, OrientationCone::FullSphere() );
	}

	// The env-emission disc encloses the scene over the whole shutter.
	struct SceneBoxScan : public IEnumCallback<IObject>
	{
		BoundingBox& box;
		bool& valid;
		SceneBoxScan( BoundingBox& b, bool& v ) : box( b ), valid( v ) {}
		bool operator()( const IObject& obj )
		{
			if( obj.IsWorldVisible() ) {
				const BoundingBox b = obj.getBoundingBox();
				if( valid ) {
					box.Include( b.ll );
					box.Include( b.ur );
				} else {
					box = b;
					valid = true;
				}
			}
			return true;
		}
	};
	if( scene.GetObjects() ) {
		SceneBoxScan scan( sweptSceneBox, sweptSceneBoxValid );
		scene.GetObjects()->EnumerateObjects( scan );
	}
}

void LightSampler::Prepare(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries
	)
{
    try {

	pPreparedScene = &scene;
	pPreparedLuminaries = &luminaries;
    std::vector<SpectralSourceDomain::Mass> spectralPowers;
    spectralFinitePower = SpectralSourceDomain::Mass();
    if (spectralDomain) {
        // Policy is re-applied on every preparation, not just first attach.
        risCandidates = 0; lightSampleRRThreshold = 0; bUseLightBVH = false;
    }

	// Build the combined light table from non-mesh lights and mesh luminaries
	lightEntries.clear();
	luminaryEntryIndex.clear();

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
		const bool bSweptLights = bMotionSweep && sweptLights.size() == lights.size();
		unsigned int lightIdx = 0;
		for( m=lights.begin(), n=lights.end(); m!=n; m++, lightIdx++ )
		{
			const ILightPriv* l = *m;
			Scalar exitance = spectralDomain ? 0 : ColorMath::MaxValue( l->radiantExitance() );
			const MotionSweepRecord* pSwept = !spectralDomain && bSweptLights ? &sweptLights[lightIdx] : 0;
			if( pSwept ) {
				exitance = SweptSelectionWeight( *pSwept, exitance );
			}
			// DL-463: under a sweep a positional light dark at every sample
			// may still shine BETWEEN them (an interpolator's overshoot);
			// keep it at a floor weight (set below) rather than pmf 0.
			const bool bFloor = pSwept && !( exitance > 0 ) && l->IsPositionalLight();
			if( spectralDomain ? l->IsPositionalLight() : (exitance > 0 || bFloor) )
			{
				LightEntry entry;
				entry.pLight = l;
                BindPhysicalProduct(entry);
				entry.lumIndex = 0;
				entry.exitance = exitance;
				entry.position = l->position();
				entry.twoSided = false;
				if( pSwept ) {
					ApplySweptBounds( entry, *pSwept );
				}
				if (spectralDomain) spectralPowers.push_back(EstimateSpectralPower(*l));
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
	const bool bSweptLums = bMotionSweep && sweptLuminaries.size() == luminaries.size();
	for( unsigned int li = 0; li < luminaries.size(); li++ )
	{
		const IEmitter* pEmitter = luminaries[li].pLum->GetMaterial()->GetEmitter();
		if( pEmitter )
		{
			// DL-320: a double-sided luminary radiates from BOTH faces, so its
			// total power -- the selection weight the alias table, the light
			// BVH and RIS all read, and the partner `pdfSelect` every hit-side
			// MIS weight reads back through `PdfSelectLuminary` -- is twice
			// the one-sided `M * A`.  Any PMF would be unbiased as long as
			// both sides read the same one; this one is the physical power.
			// DL-431: the surface mean over this luminary's own points, not
			// the emitter's construction-time estimate at P = Po = 0.
			const bool twoSided = LuminaryIsTwoSided( luminaries[li].pLum );
			Scalar exitance = spectralDomain ? 0 : LuminarySelectionWeight( luminaries[li].pLum );
			const MotionSweepRecord* pSwept = !spectralDomain && bSweptLums ? &sweptLuminaries[li] : 0;
			if( pSwept ) {
				exitance = SweptSelectionWeight( *pSwept, exitance );
			}
			const bool bFloor = pSwept && !( exitance > 0 );
			if( spectralDomain ? (luminaries[li].pLum->GetGeometry() && luminaries[li].pLum->GetGeometry()->CanBeAreaLight() && luminaries[li].pLum->GetArea() > 0) : (exitance > 0 || bFloor) )
			{
				LightEntry entry;
				entry.pLight = 0;
				entry.lumIndex = li;
				entry.exitance = exitance;
				entry.twoSided = twoSided;
				if( pSwept ) {
					ApplySweptBounds( entry, *pSwept );
				}

				// Sample a representative position on the luminary surface
				Point3 repPos;
				Vector3 repNormal;
				Point2 repCoord;
				luminaries[li].pLum->UniformRandomPoint(
					&repPos, &repNormal, &repCoord, centerSeed );
				entry.position = repPos;

				luminaryEntryIndex[ luminaries[li].pLum ] = static_cast<unsigned int>( lightEntries.size() );
				if (spectralDomain) spectralPowers.push_back(EstimateSpectralPower(*luminaries[li].pLum));
				lightEntries.push_back( entry );
			}
		}
	}

    if (spectralDomain) {
        const auto probabilities = spectralDomain->Probabilities(spectralPowers);
        for (size_t i=0;i<lightEntries.size();++i) {
            lightEntries[i].exitance = probabilities[i];
            if (spectralPowers[i].positive) spectralFinitePower.AddLog(spectralPowers[i].logarithm);
        }
    }
	// DL-463: the floor weight of swept lights that read dark at every
	// sample: 1e-3 of the mean positive weight (1 when none is positive) --
	// any positive weight is an unbiased pmf; this one costs ~nothing.
	if (!spectralDomain) {
		Scalar sum = 0;
		unsigned int n = 0;
		for( std::size_t i = 0; i < lightEntries.size(); i++ ) {
			if( lightEntries[i].exitance > 0 ) {
				sum += lightEntries[i].exitance;
				n++;
			}
		}
		const Scalar floorWeight = n > 0 ? Scalar( 1e-3 ) * sum / Scalar( n ) : Scalar( 1 );
		for( std::size_t i = 0; i < lightEntries.size(); i++ ) {
			if( !( lightEntries[i].exitance > 0 ) ) {
				lightEntries[i].exitance = floorWeight;
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
	if( !spectralDomain && !bUseLightBVH && risCandidates == 0 && lightEntries.size() > 8 )
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
			bool alpha;
			std::vector<const IMedium*> emitters;
			MediaScan() : found(false), foundFire(false), foundNullBoundary(false), alpha(false) {}
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
				// Continue: alpha is an independent scene capability.
				alpha |= HasEffectiveAlphaCoverage( &obj );
				return true;
			}
		};
		MediaScan scan;
		scene.GetObjects()->EnumerateObjects( scan );
		bSceneHasObjectMedia = scan.found;
		bSceneHasObjectFireMedia = scan.foundFire;
		bSceneHasNullBoundaries = scan.foundNullBoundary;
		bSceneHasAlphaCoverage = scan.alpha;
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
		if( bMotionSweep && sweptSceneBoxValid ) {
			// DL-463: the env-emission disc must enclose every pose of the shutter.
			scan.bbox.Include( sweptSceneBox.ll );
			scan.bbox.Include( sweptSceneBox.ur );
			scan.seen = true;
		}
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

	// DL-463: a sweep describes one frame's shutter; it is consumed here.
	ClearMotionSweep();
    } catch (...) {
        pPreparedScene = nullptr;
        pPreparedLuminaries = nullptr;
        lightEntries.clear(); luminaryEntryIndex.clear(); positionalLightIndices.clear();
        aliasTable.Build({}); volumeEmissionAlias.Build({}); equiangularPivotAlias.Build({});
        volumeEmissionMedia.clear(); positionalLightBandPowers.clear(); volumeEmissionPowerProxies.clear();
        cachedTotalExitance = 0; positionalLightTotalExitance = 0;
        cachedEnvSelectProb = 0; cachedSceneRadius = 0;
        SetEnvironmentSampler(nullptr,nullptr);
        delete pLightBVH; pLightBVH = nullptr;
        bSceneHasObjectMedia = false; bSceneHasObjectFireMedia = false;
        bSceneHasNullBoundaries = false; bSceneHasAlphaCoverage = false;
        volumeEmissionDistributionValid = false; equiangularPivotDistributionValid = false;
        ClearMotionSweep();
        throw;
    }
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
	// Reset the probe payload too, so a caller that REUSES a LightSample
	// cannot inherit the previous sample's surface (a different point, or
	// a different luminary entirely).  `valid` false is the honest state
	// for every light kind but the mesh-luminary branch below.
	//
	// ONE STORE, not a 416-byte default-construct (168 + 136 + 104 + a
	// bool, measured at this tree's HEAD): every consumer gates on
	// `valid`, so clearing the flag makes the other three members
	// unreachable and assigning them is pure cost.  `SampleLight` is on
	// the per-light-sample path AND on `ManifoldSolver`'s per-shading-point
	// path, which is why this is worth a line of comment.
	sample.surface.valid = false;
	// `ptObjIntersec` is UNGATED (see `EmitterObjectPoint`), so it does
	// need a real reset -- to the `(0,0,0)` every hand-built emitter
	// record carried before this slice, which is what a delta light, an
	// env sample and a CSG-composite luminary keep.
	sample.ptObjIntersec = Point3( 0, 0, 0 );

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
    // Spectral mixed roots use a fresh conditional draw. Reusing a tiny
    // class interval would collapse its 32-bit Sobol conditional support.
    // The legacy no-domain dimension layout remains unchanged.
    const bool independentConditional = spectralDomain && bEnvExists && bAliasValid;
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
		const Scalar u_env = independentConditional ? sampler.Get1D() : u_top / denom;
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
	const Scalar u_alias = independentConditional ? sampler.Get1D() : (( aliasSpan > 0 ) ?
		( ( u_top - cachedEnvSelectProb ) / aliasSpan ) :
		u_top);
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

		// DL-320: which FACE this sample leaves from.  A one-sided emitter
		// has one face, the winding normal `UniformRandomPoint` returned.  A
		// double-sided one (`entry.twoSided`) radiates from both, exactly as
		// every strategy that HITS it sees it, so the face is chosen with
		// probability 1/2 -- by remapping the FIRST direction coordinate
		// rather than drawing a new one, so the sampler's dimension budget is
		// unchanged and a one-sided emitter's draw is bit-identical.
		// `sample.normal` becomes the EMITTING face's normal: every
		// consumer that dots it with `sample.direction` (the light-subpath
		// root's cosine, `Le`, the BDPT/VCM rebuilds) then sees the same
		// one-face record it always did.  Consumers that evaluate this point
		// toward some OTHER direction (BDPT s = 1 / t = 1, VCM NEE) resolve
		// the face for that direction themselves (`EmitterSides::FaceToward`).
		const Vector3 windingNormal = sample.normal;
		Point2 dirRand = sampler.Get2D();
		if( entry.twoSided ) {
			if( dirRand.x < Scalar( 0.5 ) ) {
				dirRand.x = dirRand.x * Scalar( 2 );
			} else {
				dirRand.x = dirRand.x * Scalar( 2 ) - Scalar( 1 );
				sample.normal = -sample.normal;
			}
		}

		// Build an orthonormal basis around the emitting face's normal
		// and sample a cosine-weighted hemisphere direction
		OrthonormalBasis3D onb;
		onb.CreateFromW( sample.normal );

		sample.direction = GeometricUtilities::CreateDiffuseVector( onb, dirRand );

		// pdfDirection = cos(theta) / pi for a cosine-weighted hemisphere,
		// halved for a double-sided emitter (the face choice): the SAME
		// function `EmitterSides::CosineEmissionPdf` every hit-side MIS
		// partner evaluates.
		sample.pdfDirection = EmitterSides::CosineEmissionPdf(
			entry.twoSided, windingNormal, sample.direction );

		// THE SHADING PAYLOAD for this sampled point (slice S3 of
		// docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5).  Probed ONCE
		// here and carried on the sample, because consumers build records
		// from this one point and every one of them must read the same
		// channel: this `rig`, `GenerateLightSubpathImpl`'s NM hero `Le`
		// rebuild, its HWSS companion-wavelength twin `rigW`, the BDPT
		// `type == LIGHT` root vertex, `VCMIntegrator`'s light-vertex
		// NEE record, and SMS's solved-chain emission queries (DL-347).
		// Probing per consumer would add redundant rays and --
		// worse -- could disagree between the hero and its companion
		// wavelengths, which is the spectral form of the very defect this
		// slice closes.
		//
		// The probe is NORMAL-ALIGNED: no single direction is "the"
		// direction this record is viewed from (the consumers each
		// look from somewhere else), so it stands off along the sampled
		// normal and fires back onto the point.  Since the round-2
		// transport review the two NEE sites use this SAME entry point, so
		// PT, BDPT and VCM now refuse in exactly the same places.
		//
		// Gated inside the probe; `valid` stays false on every delta light
		// and env sample (this branch is the only one that can fill it) and
		// on refusal, and `ApplyEmitterSurface` is then a no-op.
		//
		// Probed along the WINDING normal, not the emitting face's: the
		// payload describes the surface point, which is the same point from
		// either face, and this keeps it identical to the pre-DL-320 record
		// (and to the NEE arms, which probe along the winding normal too).
		ProbeEmitterSurface(
			lumEntry.pLum, scene.GetObjects(),
			sample.position, windingNormal, sample.surface );

		// `Po`, on the other hand, is NOT gated and costs no ray -- it is
		// read by painters that register no signal demand.  Carried on the
		// sample for the same consumers, for the same reason.
		sample.ptObjIntersec = EmitterObjectPoint(
			lumEntry.pLum, sample.position, sample.ptObjIntersec );

		// DL-44: the surface UV `UniformRandomPoint` returned for this
		// sample, carried for the same rebuild sites `ptObjIntersec`
		// is (see `LightSample::ptCoord`'s doc comment).  Ungated and
		// ray-free, like `Po` above -- it is simply `coord`, already
		// computed a few lines up.
		sample.ptCoord = coord;

		// Compute emitted radiance at this point in this direction
		RayIntersectionGeometric rig( Ray( sample.position, sample.direction ), nullRasterizerState );
		rig.vNormal = sample.normal;
		// sample.normal is what `UniformRandomPoint` returned.  ON A MESH
		// LUMINARY THAT IS THE INTERPOLATED VERTEX NORMAL when the mesh
		// carries per-vertex normals, and the face normal only when it does
		// not: `GeometricUtilities::PointOnTriangle` barycentrically averages
		// `t.normals[]` and falls back to the cross product only for a
		// triangle with no normals at all.  (Four comments in this file, in
		// VCMIntegrator.cpp and in BDPTIntegrator.cpp used to call it "the
		// geometric face normal" flatly; corrected in the S3 review.)  No
		// Phong/bump MODIFIER runs on an emitter record either way, so
		// mirroring it into `vGeomNormal` keeps the record self-consistent --
		// which is the property the downstream cosines need.
		// DL-298: ONE shared fill (world position `P`, normals, `onb` --
		// built from `sample.normal`, identical to the local `onb` above --
		// UV and `Po`), the same function every other emitter-evaluation
		// record uses.  Before it, this record left `ptIntersection` at the
		// default (0,0,0), so a `P`-keyed emission painter read the origin.
		FillEmitterRecord( rig, sample.position, sample.normal, coord, sample.ptObjIntersec, lumEntry.pLum, scene.GetCamera() );
		ApplyEmitterSurface( rig, sample.surface );

		if (!AcceptEmitterAlpha(lumEntry.pLum, scene.GetObjects(), sample.position, rig, sampler)) return false;
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

//
// PdfSelectLight / PdfSelectLuminary -- the selection pmf of SampleLight()
// (DL-348, 2026-10-02).
//
// These are the MIS partner densities of the strategies SampleLight() roots:
// BDPT/VCM/MLT NEE (s = 1), light tracing (t = 1), the s >= 2 connections
// and VCM merging all select their light with SampleLight()'s single
// env-vs-alias roll.  The light BVH is NEVER consulted there -- it drives
// PT's own NEE only (EvaluateDirectLighting, whose partner is
// CachedPdfSelectLuminary).  These queries used to return the BVH's
// shading-point-dependent pmf whenever the BVH was built (`light_bvh`
// defaults TRUE, built for 2+ lights), so the eye-hits-emitter strategy
// (VCM EvaluateS0Impl, BDPT s = 0) weighted against a selection density
// its competitors never used: a partition-of-unity violation, VCM 0.92 /
// BDPT 0.96 of the closed form with four equal quads under an
// orthographic camera (VCMStrategyBalanceTest topology Y).  One light
// (no BVH) and one multi-quad mesh luminary (one table entry) were
// immune.  The shading point / normal arguments are therefore unused;
// the signature is kept so callers state where the alternative would
// have selected from.
//
Scalar LightSampler::PdfSelectLight(
	const IScene& /*scene*/,
	const LuminaryManager::LuminariesList& /*luminaries*/,
	const ILight& light,
	const Point3& /*shadingPoint*/,
	const Vector3& /*shadingNormal*/
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
			// 128% of PT (Session 9 follow-up bug).  Alias-only:
			// SampleLight() never consults the light BVH (DL-348).
			const Scalar aliasShare = Scalar( 1 ) - cachedEnvSelectProb;
			return aliasShare *
				static_cast<Scalar>( aliasTable.Pdf( i ) );
		}
	}

	return 0;
}

Scalar LightSampler::PdfSelectLuminary(
	const IScene& /*scene*/,
	const LuminaryManager::LuminariesList& /*luminaries*/,
	const IObject& luminary,
	const Point3& /*shadingPoint*/,
	const Vector3& /*shadingNormal*/
	) const
{
	if( !pPreparedLuminaries )
	{
		return 0;
	}

	// Find the matching entry in the light table
	const int entryIndex = FindLuminaryIndex( &luminary );	// one hash lookup, not a scan
	if( entryIndex >= 0 )
	{
		const unsigned int i = static_cast<unsigned int>( entryIndex );
		if( !lightEntries[i].pLight )
		{
			if( (*pPreparedLuminaries)[lightEntries[i].lumIndex].pLum == &luminary )
			{
				// Continuous-PMF rescale — see `PdfSelectLight` above
				// for the full rationale; alias-only (DL-348).
				const Scalar aliasShare = Scalar( 1 ) - cachedEnvSelectProb;
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

	// One hash lookup: this is asked on every MIS-weighted emitter hit.
	const std::unordered_map<const IObject*, unsigned int>::const_iterator it =
		luminaryEntryIndex.find( pLuminary );
	return it == luminaryEntryIndex.end() ? -1 : static_cast<int>( it->second );
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

	const int entryIndex = FindLuminaryIndex( &luminary );	// one hash lookup, not a scan
	if( entryIndex >= 0 )
	{
		const unsigned int i = static_cast<unsigned int>( entryIndex );
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
// table and the light BVH one draw is made and a self draw
// contributes nothing: no retry, no correction factor, so every
// other light keeps its selection pmf.  The light a non-convex
// emitter casts on itself is then estimated only by the scattered
// ray that returns to the same object, which the hit side gives
// full weight (RAY_STATE::pNEESelfObject, DL-633).
//
// MIS:
// - Delta lights (point/spot): w = 1 (no alternative strategy).
// - Area lights, RIS OFF: power heuristic using the selection pmf
//   this draw used (the light BVH's when built, else the alias
//   table's; converted to solid angle) vs BSDF PDF.
//   CachedPdfSelectLuminary returns the same value on the BSDF-hit
//   side (PathTracingIntegrator; EmissionShaderOp for a path-tracer
//   continuation), or 0 when the luminary cannot be selected, and
//   the hit then keeps full weight.
// - Area lights, RIS ON: w = 1 (no MIS).  The exact finite-M
//   technique density is intractable, so the BSDF-hit emitter
//   contribution is suppressed on the hit side instead, for every
//   luminary RIS can select.
//

namespace
{
	//! DL-09 (docs/DL09_GRADED_INDEX_INTERIOR_FACTOR.md §3(ii)): the NEE
	//! shadow segment from the shading point (whose walk's stack top has
	//! been advanced to it, or telescopes to it) to the light point carries
	//! the graded-index factor the BSDF-sampled continuation pays on
	//! arrival at that light, so the two MIS strategies price one
	//! integrand.  Exactly 1 when the caller does not track graded media
	//! (null stack) or no graded medium lies on the segment.
	//!
	//! DL-292: when a hit-by-hit shadow walk ran (transparent shadows, or a
	//! delta light's walk through a thin weave's gap), its track followed
	//! the segment's actual boundary CROSSINGS and priced every graded
	//! piece of it -- that is the answer.  Otherwise the segment was clear
	//! (a binary test) and lies in one medium region, which
	//! ConnectionScaleToPoint prices.  Before DL-292 this keyed on the
	//! shadow transmittance (`!= 1` => "crossed a surface" => factor 1):
	//! a weave gap's `g != 1` with no crossing, a nested object's
	//! through-trip and a real exit's graded piece all went unpriced.
	inline Scalar GradedNEESegmentScale(
		const IORStack* pGradedStack,
		const Point3& lightPoint,
		const GradedIndexMedium::ShadowSegmentTrack* pTrack )
	{
		if( pTrack && pTrack->Walked() ) {
			return pTrack->Scale();
		}
		if( !pGradedStack ) {
			return Scalar( 1 );
		}
		return GradedIndexMedium::ConnectionScaleToPoint( pGradedStack, lightPoint );
	}

	//! DL-292: a shadow-segment track is built only when a graded-index
	//! medium can exist in this process (GradedIndexDemand, one relaxed
	//! atomic load) and the caller tracks graded media -- so a
	//! constant-index render never copies a stack for it.
	inline std::optional<GradedIndexMedium::ShadowSegmentTrack> MakeGradedShadowTrack( const IORStack* pGradedStack )
	{
		if( pGradedStack && GradedIndexDemand::Any() ) {
			return GradedIndexMedium::ShadowSegmentTrack( *pGradedStack );
		}
		return std::nullopt;
	}
}

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
	const IGuidedNEEPdfBlend* pGuidedBlend,
	const IORStack* pMisIorStack,
	const Scalar neeTrainingScale,
	const bool bBsdfSamplingPartnerExists,
	const IORStack* pGradedIndexStack,
	const bool bSMSCoversDeltaLights,
	const IORStack* pMediumStack
	) const
{
    MediumBoundaryHits boundaryHits;
	RISEPel result( 0, 0, 0 );

	if( !pPreparedScene )
	{
		return result;
	}

	// ================================================================
	// FULL-SPHERE NEE (see IMaterial::ScattersFullSphere).
	// ================================================================
	// Every `cosSurface` / `cosEnv` below is the SIGNED cosine between
	// the shadow direction and the VIEW-FACING shading normal, and every
	// gate that consumes it rejects `<= 0`. A two-sided BRDF/SPF flips
	// its shading hemisphere when relief puts the viewer below Ns;
	// RayFacingShadingCosine reproduces that orientation (DL-224).
	// An ordinary BRDF's `value()` is zero below this oriented horizon -- the rejected
	// directions carry no transport, so NEE loses nothing by skipping
	// them and the BSDF-sampling strategy has nothing to partner with.
	//
	// It is WRONG for a BSDF with support over the full sphere (Chiang
	// hair: the TT / TTs lobes exit the FAR side of the fibre).  There
	// the below-horizon half carries real transport, NEE never samples
	// it, and yet the BSDF-sampling side still multiplies its hits there
	// by `w_bsdf = p_b^2 / (p_b^2 + p_l^2) < 1` -- because that side's
	// MIS partner computation (PathTracingIntegrator's emitter-hit and
	// env-escape blocks) has no shading-surface cosine gate of its own
	// and cannot tell that NEE declined to fire.  The two strategies
	// therefore sum to `w_bsdf < 1` instead of 1 over the entire
	// transmissive hemisphere, and the estimator reads systematically
	// UNDER by exactly the missing `w_nee`.
	//
	// MIS PARTITION, DERIVED.  For a direction `wi` below the horizon
	// with the capability ON:
	//   * env NEE now has density `p_l(wi) = envPdf(wi)`.  The
	//     EnvironmentSampler's importance map covers the whole sphere,
	//     so `p_l > 0` there -- the strategy was always ABLE to reach
	//     these directions; only this gate stopped it.
	//   * BSDF sampling has density `p_b(wi) = pMaterial->Pdf(wi)`,
	//     which for hair is normalized over the SPHERE (HairSPF), so
	//     `p_b > 0` there too.
	//   * NEE applies `w_nee = PowerHeuristic(p_l, p_b)` here, and the
	//     BSDF-escape block applies `w_bsdf = PowerHeuristic(p_b, p_l)`
	//     at the same `wi` with the same two densities -- identical
	//     arguments, so `w_nee + w_bsdf == 1` exactly.  The partition
	//     closes on the full sphere.
	//   * The light-table area-light row is the same argument with
	//     `p_l = pdfAlias * d^2 / (A * cosLight)`; its partner is
	//     PathTracingIntegrator's `p_nee = pdfSelect * d^2/(A*cosLight)`
	//     from `CachedPdfSelectLuminary`, which likewise applies no
	//     shading-surface cosine gate.
	//   * The delta-position light row (point / spot / omni) has NO MIS
	//     partner at all -- a delta light cannot be BSDF-sampled, so NEE
	//     carries it at `w = 1` on both hemispheres.  Relaxing that gate
	//     adds the previously-missing below-horizon direct lighting at
	//     full weight; there is no partition to violate.  This is the
	//     row that closes the point-lit backlit-hair PT-vs-BDPT gap
	//     (BDPT's s == 1 row already uses `fabs` at the eye vertex --
	//     BDPTIntegrator.cpp's `absCosEye` and BDPTUtilities.h's
	//     `GeometricTerm`/`GeometricTermSurfaceMedium`, all three
	//     unconditionally `fabs` -- which is precisely why BDPT could
	//     reach this transport and PT could not).
	//
	// SAFETY.  The capability defaults FALSE, and where it is FALSE
	// the opposite VIEW-FACING shading hemisphere remains rejected.
	// The DL-224 orientation change is neutral when Ns faces the view;
	// a tilted Ns facing away now follows the SPF's flipped frame.  Granting the capability to a material
	// whose `value()` does NOT transmit would be a real bias (NEE would
	// light its back faces at full weight), which is why this is opt-in
	// per material rather than a blanket `fabs`.
	const bool bFullSphere = ( pMaterial != 0 && pMaterial->ScattersFullSphere() );

	// ================================================================
	// SHADOW GATE (see the twin in EvaluateDirectLightingNM).
	// ================================================================
	// `pShadingObject` is the object being shaded, consulted only for its
	// `DoesReceiveShadows()` opt-out.  It is NULL for a MEDIUM SCATTER
	// VERTEX: MediumTransport::EvaluateInScattering{,NM} -- the only
	// caller that passes 0 -- has no object to name, and always pairs the
	// 0 with `isVolumeScatter == true`.  Every surface caller passes
	// `ri.pObject`.
	//
	// A NULL therefore means "no object opted out", not "skip the shadow
	// ray".  Step 1 (ambient / directional) always spelled it that way;
	// Steps 2 and 3 used to spell it `pShadingObject && ...`, so every
	// NEE row at a volume scatter vertex -- delta light, mesh area light,
	// AND environment -- silently skipped its visibility test and
	// reported UNOCCLUDED radiance.  Fog behind a wall was lit by the
	// light in front of it; in a furnace with geometry, a medium vertex's
	// env-NEE integrated the whole sphere while its phase-sampled MIS
	// partner (which really does stop at the surface) integrated only the
	// unoccluded part, so the two strategies summed to more than 1 by
	// exactly the blocked solid angle's share.  Measured on
	// VolumeEnvFurnaceTest's fog-box-plus-white-floor scene: the volume
	// vertices' env-NEE read 0.412 against its partner's 0.298, i.e.
	// +38 %, which is the whole of that scene's +6.2 % over-unity.
	//
	// Scenes with no geometry at all (VolumeEnvFurnaceTest cells 1-6) are
	// unaffected: the shadow ray they now cast can hit nothing.
	const bool bReceivesShadows =
		pShadingObject ? pShadingObject->DoesReceiveShadows() : true;
    // Records are authoritative only for a completed query in this scene.
    // Shadow-disabled rays still need alpha-thinned medium boundaries, but
    // must pass ordinary blockers. Visibility-enabled rays reuse their one
    // sampled boundary decision, never draw coverage a second time.
    MediumBoundaryHits* sampledBoundaries = bSceneHasObjectMedia && bSceneHasAlphaCoverage ? &boundaryHits : nullptr;
    const auto mediumRecords = [&](const Ray& ray, Scalar distance) -> const MediumBoundaryHits* {
        if (sampledBoundaries && !bReceivesShadows) {
            pPreparedScene->GetObjects()->CollectMediumBoundaryHitsSampled(ray, distance, sampler, boundaryHits, pPreparedScene->GetCamera());
        }
        return sampledBoundaries;
    };


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
			if( spectralDomain ? !l->IsPositionalLight() : exitance <= 0 )
			{
				RISEPel amount( 0, 0, 0 );
				// FULL-SPHERE NEE, DirectionalLight sibling (residual
				// wave 2 item D): Step 1 is the path a groom lit by a
				// `directional_light` goes through (radiantExitance()
				// == 0), so it needs the same `bFullSphere` this
				// function's other two NEE sites already carry.
				//
				// VOLUME RECEIVER (residual-ledger item 10 of
				// docs/PT_ENV_MIS_DOUBLECOUNT.md) -- part A of 2.
				// `isVolumeScatter` tells the light that `ri.vNormal` is
				// the synthetic `wo` MediumTransport::EvaluateInScattering
				// installed, not a surface normal, so it must deliver
				// RADIANCE ONLY: no receiver cosine, no hemisphere
				// rejection.  Steps 2 and 3 below already spell this
				// inline (`cosSurface = 1.0` under `isVolumeScatter`);
				// Step 1 could not, because the arithmetic lives behind
				// this virtual.  See ILight.h for the derivation.
				// DL-157 P1: the LIVE stack, so this Step-1 arm prices a
				// stateful BSDF on the same SIDE the three sampled-light
				// arms below already do.
				if (sampledBoundaries) sampledBoundaries->clear();
                l->ComputeDirectLightingSampled( ri, caster, brdf,
					bReceivesShadows,
					amount, bFullSphere, isVolumeScatter, pMisIorStack, sampler, sampledBoundaries );

				// VOLUME RECEIVER -- part B of 2: MEDIUM ATTENUATION.
				//
				// The shadow ray for a Step-1 light is cast INSIDE the
				// light (`CastShadowRayAuto`), which answers only the
				// binary/Fresnel visibility question and never reaches
				// `EvalShadowTransmittance`.  So a directional light's
				// contribution was not attenuated by the medium it
				// crosses -- unlike every row in Steps 2 and 3, each of
				// which post-multiplies exactly this factor.
				//
				// Applied HERE rather than threaded into the ILight
				// virtual on purpose: doing it there would need
				// `IMedium*`, `IObject*`, `IScene*` and a bool pushed
				// through a virtual on every light, plus lifting the
				// file-static `EvalShadowTransmittance` out of this TU
				// into a shared one (which trips the five-build-project
				// source-file rule).  All four values are already in
				// scope right here.
				//
				// Per light kind:
				//   * Directional -- attenuate along `emissionDirection()`
				//     (FROM surface TO light, per DirectionalLight.h's
				//     convention) with maxDist = RISE_INFINITY, matching
				//     the env-NEE row below exactly: same "light at
				//     infinity" case, same walk limit.
				//   * Ambient -- SKIPPED.  It is a directionless constant
				//     with no ray to attenuate.
				//   * Anything else with zero exitance (an out-of-tree
				//     subclass) -- SKIPPED, conservatively preserving its
				//     existing behaviour.
				//
				// NOT gated on `isVolumeScatter`: Steps 2 and 3 do not
				// gate it either, and a SURFACE inside fog has the very
				// same missing attenuation.  In a scene with no media at
				// all `EvalShadowTransmittance` early-returns (1,1,1), so
				// media-free scenes stay bit-identical by construction.
				//
				// CONSEQUENCE FOR A GLOBAL MEDIUM, stated plainly: with
				// an unbounded global medium the fast path evaluates
				// `EvalTransmittance( ray, RISE_INFINITY )`, i.e.
				// exp(-sigma_t * DBL_MAX) == 0, so a directional light in
				// an unbounded global medium is fully extinguished.  That
				// is physically correct for an infinite medium and is
				// precisely what env-NEE has always done below -- but it
				// IS a visible change for any scene pairing a global
				// medium with a directional light.
				if( l->lightType() == ILight::LightType::Directional &&
					ColorMath::MaxValue( amount ) > 0 )
				{
					const Ray rayToLight( ri.ptIntersection, l->emissionDirection() );
					amount = amount * EvalShadowTransmittance( rayToLight,
						RISE_INFINITY, pMedium, pMediumObject,
						pPreparedScene, bSceneHasObjectMedia, pMediumStack, mediumRecords(rayToLight, RISE_INFINITY) );
				}

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
				//
				// DL-155: this is still a valid NEE attempt (it
				// legitimately drew a direction and found nothing to
				// connect to) -- count it, or M_nee is inflated by
				// 1/P(not self-hit) at self-luminous shading points.
				if( pOptimalMIS && !pOptimalMIS->IsReady() )
				{
					const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
						ri.rast.x, ri.rast.y, kTechniqueNEE );
				}
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
				//
				// DL-155: count the attempt (see the BVH branch above).
				if( pOptimalMIS && !pOptimalMIS->IsReady() )
				{
					const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
						ri.rast.x, ri.rast.y, kTechniqueNEE );
				}
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
				//
				// DL-155: count the attempt (see the BVH branch above).
				if( pOptimalMIS && !pOptimalMIS->IsReady() )
				{
					const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
						ri.rast.x, ri.rast.y, kTechniqueNEE );
				}
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
			const Scalar cosSurfaceSigned = isVolumeScatter ? Scalar(1.0) :
				ri.RayFacingShadingCosine( vToLight );
			// FULL-SPHERE NEE site 1 of 3 (RGB): delta-position light.
			// No MIS partner exists for a delta light, so the relaxed
			// gate simply restores the below-horizon direct term at
			// w = 1.  See the derivation at the top of this function.
			const Scalar cosSurface = bFullSphere ?
				std::fabs( cosSurfaceSigned ) : cosSurfaceSigned;

			if( !isVolumeScatter && cosSurface <= 0 ) break;

			// Shadow test.  shadowT carries the Fresnel transmittance
			// when transparent shadows are enabled (else 1,1,1).
			RISEPel shadowT( 1.0, 1.0, 1.0 );
			std::optional<GradedIndexMedium::ShadowSegmentTrack> gradedTrack = MakeGradedShadowTrack( pGradedIndexStack );
			if( bReceivesShadows )
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				if( ShadowOccludedRGB( caster, rayToLight, dist - 0.001, shadowT, true /*DL-05: delta light*/ , sampler, sampledBoundaries, dist,
						gradedTrack ? &*gradedTrack : 0, &lightPos, bSMSCoversDeltaLights ) )
					break;
			}

			// emittedRadiance expects the outgoing direction FROM the
			// light; -vToLight is the light-to-surface direction.
			const RISEPel Le = entry.pLight->emittedRadiance( -vToLight );
			const RISEPel fBSDF = brdf.valueStateful( vToLight, ri, pMisIorStack );
			const Scalar invDistSq = 1.0 / (dist * dist);

			// Delta-position light: w = 1 (no MIS needed).  Fold in the
			// transparent-shadow Fresnel transmittance (1,1,1 when off).
			RISEPel amount = Le * fBSDF * cosSurface * invDistSq * shadowT;

			// Apply medium transmittance along shadow ray.
			// Multi-medium shadow transmittance (origin, per-object,
			// and global media; the segment walk has no fixed depth/count cap).
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				const RISEPel Tr = EvalShadowTransmittance( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, pMediumStack, mediumRecords(rayToLight, dist) );
				amount = amount * Tr;
			}

			// DL-09: the connection segment's graded-index factor.
			result = result + amount * ( ( risWeight / pdfAlias ) *
				GradedNEESegmentScale( pGradedIndexStack, lightPos, gradedTrack ? &*gradedTrack : 0 ) );
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
			const Scalar cosSurfaceSigned = isVolumeScatter ? Scalar(1.0) :
				ri.RayFacingShadingCosine( vToLight );
			// FULL-SPHERE NEE site 2 of 3 (RGB): mesh area light.  The
			// |cos| feeds THREE consumers below, and all three need it:
			// the `cosSurface > 0` gate (else the row never fires), the
			// light-sample RR `estimate` (a NEGATIVE estimate would make
			// pSurvive negative and terminate every below-horizon draw),
			// and `contrib` itself (the transported cosine factor).
			// `cosLight` is the EMITTER-side cosine and is deliberately
			// NOT touched -- a one-sided emitter genuinely emits nothing
			// backwards, which is a property of the light, not of the
			// receiving BSDF.
			const Scalar cosSurface = bFullSphere ?
				std::fabs( cosSurfaceSigned ) : cosSurfaceSigned;
			// DL-320: the FACE of the luminary this sample leaves from toward
			// the receiver.  A double-sided emitter radiates from both faces
			// (every strategy that HITS it sees the ray-facing normal), so the
			// face toward `-vToLight` emits; a one-sided emitter keeps the
			// winding normal and its back face stays dark (`cosLight <= 0`).
			// Area-measure density is unchanged (1/area); the hit-side MIS
			// partner (`fabs(cosLight)`, PathTracingIntegrator.cpp) already
			// describes this two-faced density.
			//
			// `lumNormal` is flipped IN PLACE to that face, so every later use
			// (the record, `Le`, the MIS cosine) reads the emitting face and a
			// one-sided luminary runs the pre-DL-320 code unchanged; the
			// surface-payload probe keeps the winding normal (`lumWinding`).
			const Vector3 lumWinding = lumNormal;
			if( entry.twoSided && Vector3Ops::Dot( -vToLight, lumNormal ) < 0 ) {
				lumNormal = -lumNormal;
			}
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
				std::optional<GradedIndexMedium::ShadowSegmentTrack> gradedTrack = MakeGradedShadowTrack( pGradedIndexStack );
				if( bReceivesShadows )
				{
					const Ray rayToLight( ri.ptIntersection, vToLight );
					shadowed = ShadowOccludedRGB( caster, rayToLight, dist - 0.001, meshShadowT, false /*DL-05: area light -- see CastShadowRayAuto*/ , sampler, sampledBoundaries, dist,
						gradedTrack ? &*gradedTrack : 0, &ptOnLum );
				}

				if( !shadowed )
				{
					// Emitted radiance at sampled point
					RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
					lumri.vNormal = lumNormal;
					// `lumNormal` is `UniformRandomPoint`'s normal (negated
					// only for a double-sided emitter's back face, DL-320):
					// the INTERPOLATED VERTEX normal on a mesh luminary that
					// has per-vertex normals, the face normal when it has
					// none (see `GeometricUtilities::PointOnTriangle`).
					// Mirror it so the record is self-consistent; no
					// modifier runs here to make the two differ.
					// DL-298: the shared fill -- world position `P`
					// (the RGB NEE record used to leave it at (0,0,0)),
					// normals, `onb`, UV, and `Po` (ungated and ray-free,
					// so it lands whether or not a signal painter exists
					// anywhere in the process; this also closed a
					// PRE-EXISTING PT inconsistency where a camera ray that
					// HIT the emitter read a live `Po` and this record read
					// (0,0,0)).  CSG composites keep the zero fallback.
					FillEmitterRecord( lumri, ptOnLum, lumNormal, lumCoord,
						EmitterObjectPoint( lumEntry.pLum, ptOnLum, lumri.ptObjIntersec ),
						lumEntry.pLum, pPreparedScene ? pPreparedScene->GetCamera() : 0 );

					// THE SHADING PAYLOAD for this sampled point (slice S3
					// of docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5).
					//
					// THE SAME PROBE THE LIGHT-SUBPATH ROOT USES, since the
					// round-2 transport review (H2 P1-1).  It used to fire
					// along `vToLight` -- the ray this record is built FOR
					// -- through a second public entry point that refused
					// in different places from the normal-aligned one, so
					// PT and the bidirectional families disagreed about
					// which emitters read their signals live.  The
					// concrete failure that entry point had and this one
					// does not is the WRONG-SURFACE WINDOW: aimed from the
					// shading point, the closest hit along the ray can be a
					// different part of a concave luminary, and then the
					// probe refuses and PT alone reads neutral.
					// tests/SignalEmitterRecordTest's family F measured
					// that at PT 23 % off its baked control with BDPT and
					// VCM inside 0.1 % of theirs.
					//
					// This is NOT the shadow ray and does not replace it:
					// the shadow test above stops short of the luminary and
					// asks the whole scene; this asks ONE object and only
					// for its shading payload.
					//
					// THE SHADOW-TEST-FIRST ORDERING IS KEPT, but it is now
					// a COST ordering, not a correctness bound, and that is
					// a change worth stating.  While the probe fired along
					// `vToLight` with no travel limit, the shadow test
					// genuinely narrowed its wrong-surface window (nothing
					// of the luminary stood between the two points, so the
					// closest hit was the near face by construction).  The
					// normal-aligned probe never looks along that line, and
					// bounds its own window instead -- with the acceptance
					// tolerance and a travel limit of twice the standoff.
					// What the ordering still buys is that a sample which
					// contributes nothing never pays for a ray.
					//
					// THE GATE IS ASKED HERE TOO, which the probe would do
					// anyway.  The point is to keep the 416-byte
					// `EmitterSurfacePayload` -- and the object-manager
					// fetch -- out of the per-sample loop when no painter
					// in the process reads a signal (round-2 review, H2
					// P2-6).  The probe still gates itself, so a future
					// site that forgets this cannot read a fabricated
					// record.
					//
					// The NM twin below runs the identical calls; they
					// share this one implementation precisely so an RGB / NM
					// drift is not expressible.
					if( EmitterProbeWanted() ) {
						EmitterSurfacePayload lumSurface;
						ProbeEmitterSurface(
							lumEntry.pLum,
							pPreparedScene ? pPreparedScene->GetObjects() : 0,
							ptOnLum, lumWinding, lumSurface );
						ApplyEmitterSurface( lumri, lumSurface );
					}

					const RISEPel Le = AcceptEmitterAlpha(lumEntry.pLum, pPreparedScene ? pPreparedScene->GetObjects() : nullptr, ptOnLum, lumri, sampler, &ri.rast)
                        ? pEmitter->emittedRadiance( lumri, -vToLight, lumNormal ) : RISEPel(0,0,0);

					const Scalar geom = area * cosLight / (dist * dist);
					RISEPel contrib = Le * cosSurface * geom * brdf.valueStateful( vToLight, ri, pMisIorStack ) * meshShadowT;

					// Apply medium transmittance along shadow ray.
					// Multi-medium shadow transmittance.
					{
						const Ray rayToLight( ri.ptIntersection, vToLight );
						const RISEPel Tr = EvalShadowTransmittance( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, pMediumStack, mediumRecords(rayToLight, dist) );
						contrib = contrib * Tr;
					}

					// MIS weight: applied when the selection PDF is
					// tractable (alias table or BVH).  When RIS is ON
					// (no BVH), the exact finite-M technique density is
					// intractable, so we use w=1 and the hit side suppresses
					// the BSDF-hit contribution of every emitter RIS can select
					// (PathTracingIntegrator PART 1).
					const bool bCanDoMIS = (pLightBVH && pLightBVH->IsBuilt()) ||
						risCandidates == 0;
					// DL-171/DL-209: no competing BSDF-sampled strategy in
					// this caller's chain means no MIS partner to weigh
					// against -- take this NEE sample whole (skip straight
					// to the unweighted `result +=` below).  `pMaterial`
					// itself stays available outside this gate (bFullSphere
					// above already read it).
					if( bCanDoMIS && pMaterial && bBsdfSamplingPartnerExists && area > 0 && cosLight > 0 )
					{
						// Convert selection PDF to solid angle.
						// For both alias-table and BVH, pdfAlias is the
						// probability of selecting this light.
						const Scalar p_light = pdfAlias * (dist * dist) / (area * cosLight);
						static const IORStack defaultIOR( 1.0 );
						// DL-74 P2 (round-3 review): the aggregate pdf is the
						// MIS-PARTNER density, so it has to be evaluated under
						// the SAME IOR stack the BSDF-sampling side evaluates it
						// under -- `PathTracingIntegrator`'s
						// `PTEvalPdfAtSurface(pSPF, ..., iorStack)` uses the LIVE
						// stack.  This arm used a `static const IORStack(1.0)`
						// sentinel, so for any material whose pdf reads the stack
						// (`PolishedSPF::Pdf` via `top()`, `TranslucentSPF::Pdf`
						// via `containsCurrent()`) the two sides evaluated
						// DIFFERENT functions and the pair stopped summing to one
						// -- with guiding OFF as much as on.  `pMisIorStack` is
						// the caller's live stack; null keeps the historical
						// sentinel for callers that have none.
						const IORStack& misIOR = pMisIorStack ? *pMisIorStack : defaultIOR;
						// DL-74 (P1-B): the emitter-HIT side of this pair
						// (PathTracingIntegrator's PART 1) weights with the
						// guided nominal density, so this arm has to use
						// the same function of direction or the pair stops
						// summing to one -- exactly the asymmetry the env
						// arm below had.  DL-74 P2-2: the blend runs
						// OUTSIDE the `> 0` gate -- a direction the
						// material's own pdf cannot reach is still a
						// direction the GUIDED MIXTURE reaches, and the
						// hook is what knows that (see
						// IGuidedNEEPdfBlend's contract).
						Scalar p_bsdf = pMaterial->Pdf( vToLight, ri, misIOR );
						if( pGuidedBlend && !isVolumeScatter ) {
							p_bsdf = pGuidedBlend->Blend( vToLight, p_bsdf );
						}

						// Optimal MIS training: accumulate second moment
						// for a successful NEE hit.  contrib includes the
						// geometry factor (area*cosLight/dist^2), so the
						// matching pdf is pdfAlias (area measure), not
						// p_light (solid angle).  This ensures
						// f2/pdf^2 = (contrib/pdfAlias)^2 = (f/p_nee)^2.
						if( pOptimalMIS && !pOptimalMIS->IsReady() )
						{
							// DL-72 P2-3: the accumulator's moment is of the
							// INTEGRAND, and for a caller that scales this
							// result afterwards (the BSSRDF entry NEE, whose
							// partner trains the full exit throughput) the
							// integrand is `neeTrainingScale * contrib`.
							// 1 everywhere else, so unchanged there.
							// DL-84 round 7 (the REALIZED-MOMENT convention,
							// docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md
							// "Round 7"): `AccumulateCount` for this attempt
							// fired BEFORE the light-sample roulette above, and
							// a killed sample contributes a counted zero -- so
							// the moment paired with it must be of the estimator
							// the FILM sees, which carries
							// `rrSurvivalCompensation = 1/q`.  Without it this
							// arm trains `q * E_pre` instead of the realized
							// `E_pre / q`, exactly the defect round 7 fixed on
							// the BSDF side.  `rrSurvivalCompensation` is 1
							// whenever `light_rr_threshold` is 0 (the default),
							// so every scene that does not use that knob is
							// unchanged.
							// Round-2 review (P3-4): `risWeight` is the SAME kind
							// of per-sample realized-estimator factor as
							// `rrSurvivalCompensation` -- `result` below multiplies
							// by both in the same product
							// (`rrSurvivalCompensation * risWeight / pdfAlias`), so
							// the trained moment must carry both too, or a scene
							// that engages RIS (`risCandidates > 0`) would train an
							// incomplete moment by the identical mechanism DL-84
							// fixed for RR.  Folded in here.  `risWeight` is
							// exactly 1.0 whenever `risCandidates == 0` (the
							// default -- RIS is an opt-in scene knob,
							// `SelectLightRIS` is never called otherwise), so this
							// is currently a no-op for every scene that doesn't
							// turn RIS on.
							const Scalar lum = neeTrainingScale * rrSurvivalCompensation *
								risWeight * ColorMath::MaxValue( contrib );
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

					// DL-09: the connection segment's graded-index factor,
					// applied AFTER the optimal-MIS training above so the
					// trained moment excludes it exactly as the BSDF-side
					// partner's trained `bsdfTimesCos` (set before the
					// segment) does.
					result = result + contrib * ( ( rrSurvivalCompensation * risWeight / pdfAlias ) *
						GradedNEESegmentScale( pGradedIndexStack, ptOnLum, gradedTrack ? &*gradedTrack : 0 ) );
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

		const Scalar cosEnvSigned = isVolumeScatter ? Scalar(1.0) :
			ri.RayFacingShadingCosine( envDir );
		// FULL-SPHERE NEE site 3 of 3 (RGB): environment map.  This is
		// the site the hair white-furnace measures directly -- with the
		// gate on, a sigma_a == 0 groom reads ~0.988 instead of 1.0
		// (~0.883 once the medulla lobes broaden the below-horizon
		// share); with it relaxed both read ~1.002.
		const Scalar cosEnv = bFullSphere ?
			std::fabs( cosEnvSigned ) : cosEnvSigned;

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
			if( bReceivesShadows )
			{
				const Ray rayToEnv( ri.ptIntersection, envDir );
				envShadowed = ShadowOccludedRGB( caster, rayToEnv, RISE_INFINITY, envShadowT, false /*DL-05: env light -- see CastShadowRayAuto*/ , sampler, sampledBoundaries );
			}

			if( !envShadowed )
			{
				const Ray envRay( ri.ptIntersection, envDir );
				const RISEPel Le = pEnvironmentMap->GetRadiance( envRay, nullRasterizerState );
				const RISEPel f = brdf.valueStateful( envDir, ri, pMisIorStack );
				RISEPel envContrib = Le * f * (cosEnv / envPdf) * envShadowT;

				// Apply medium transmittance for environment ray.
				// Multi-medium shadow transmittance.
				// Use a large but finite distance for the walk limit
				// (environment rays go to infinity, but media are bounded).
				{
					const RISEPel Tr = EvalShadowTransmittance( envRay, RISE_INFINITY, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, pMediumStack, mediumRecords(envRay, RISE_INFINITY) );
					envContrib = envContrib * Tr;
				}

				// Optimal MIS training: accumulate second moment for
				// successful env NEE hit.  envContrib = Le*BSDF*cos*Tr/envPdf,
				// so the full integrand f = envContrib * envPdf.
				if( pOptimalMIS && !pOptimalMIS->IsReady() && pMaterial )
				{
					const RISEPel fullIntegrand = envContrib * envPdf;
					// DL-72 P2-3: see the area-light arm above.
					const Scalar lum = neeTrainingScale * ColorMath::MaxValue( fullIntegrand );
					const Scalar f2 = lum * lum;
					if( f2 > 0 && envPdf > 0 )
					{
						const_cast<OptimalMISAccumulator*>(pOptimalMIS)->Accumulate(
							ri.rast.x, ri.rast.y,
							f2, envPdf, kTechniqueNEE );
					}
				}

				// MIS: power heuristic (or optimal MIS) against BSDF PDF.
				//
				// DL-73 (docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md) /
				// DL-74 (docs/DL74_ENV_NEE_GUIDING_PARTITION.md):
				// `pMaterial->Pdf(...)` is the RAW, un-guided sampling pdf --
				// at a VOLUME vertex (`pMaterial` is a `MediumScatterMaterial`
				// adapter, see MediumTransport.cpp) this correctly matches
				// what `RayCaster.cpp`'s env-escape weight also uses (both
				// raw `phasePdf`), so that pairing is UNBIASED (DL-73, ruled
				// not a debt) -- `pGuidedBlend` is only ever non-null from
				// PathTracingIntegrator's SURFACE NEE call site (never the
				// volume one), and is additionally gated on `!isVolumeScatter`
				// here as a second, local safety net so this call can never
				// touch the volume case's correctness. At a SURFACE vertex
				// under active OpenPGL guiding, the raw pdf alone WAS
				// different from what `PathTracingIntegrator.cpp`'s matching
				// escape weight uses -- that mismatch broke the MIS partition
				// of unity (DL-74). `pGuidedBlend` (default null; see
				// IGuidedNEEPdfBlend's doc) replaces the raw pdf with the
				// lobe-INDEPENDENT NOMINAL density
				// `alpha_nom*guide(w) + (1-alpha_nom)*p_aggregate(w)` that the
				// escape side now also stores as its MIS partner
				// (`RAY_STATE::bsdfMisPdf`), for the SAME direction. Because
				// that nominal density is a property of the vertex and not of
				// the lobe or of the guiding MODE, it closes the partition for
				// one-sample-MIS and RIS guiding alike (DL-83 closed with
				// DL-74).
				//
				// DL-171/DL-209: see the area-light arm above -- no
				// competing BSDF-sampled strategy in this chain means no
				// partner to weigh against; skip straight to `result +=
				// envContrib` unweighted.
				if( pMaterial && bBsdfSamplingPartnerExists )
				{
					static const IORStack defaultIOR( 1.0 );
					// DL-74 P2: live stack, not the 1.0 sentinel -- see the
					// area-light arm above for the derivation.
					const IORStack& misIOR = pMisIorStack ? *pMisIorStack : defaultIOR;
					Scalar pBsdf = pMaterial->Pdf( envDir, ri, misIOR );
					// DL-74 P2-2 (round-4 review): the blend runs OUTSIDE
					// the `pBsdf > 0` gate.  Round 3 kept it inside on the
					// rationale that a zero aggregate pdf means "the
					// BSDF-sampling technique never generates this
					// direction" -- true with guiding OFF, FALSE with it
					// on, because the mixture's guide term still does.
					// Leaving it inside gave weight 1 to BOTH sides for
					// every such direction (reachable wherever the
					// material's sampling support is narrower than its
					// BSDF's: the full-sphere BSDFs whose hemisphere
					// rejection is disabled, and any partial-support lobe
					// -- row (h)).  The hook itself decides what a zero
					// aggregate pdf means; see IGuidedNEEPdfBlend.
					if( pGuidedBlend && !isVolumeScatter )
					{
						pBsdf = pGuidedBlend->Blend( envDir, pBsdf );
					}
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
	RecordSSSContainedVolumePivotAttempt();
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
	RecordSSSContainedVolumeEndpointAttempt();
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
	const Scalar cosine = fabs(Vector3Ops::Dot(direction,ri.vNormal));
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
	const IGuidedNEEPdfBlend* pGuidedBlend,
	const IORStack* pMisIorStack,
	const Scalar neeTrainingScale,
	const bool bBsdfSamplingPartnerExists,
	const IORStack* pGradedIndexStack,
	const bool bSMSCoversDeltaLights,
	const IORStack* pMediumStack
	) const
{
    MediumBoundaryHits boundaryHits;
	Scalar result = 0;

	if( !pPreparedScene || !pPreparedLuminaries )
	{
		return result;
	}

	// FULL-SPHERE NEE -- spectral twin.  Read the RGB
	// EvaluateDirectLighting's block comment for the derivation; the
	// three sites below mirror its three one-for-one, with
	// `PdfNM` standing in for `Pdf` in the MIS partition argument.
	// Kept as a separate local (rather than hoisted to a member or a
	// helper) so the two functions stay independently readable, which
	// is the convention the rest of this RGB/NM pair already follows.
	const bool bFullSphere = ( pMaterial != 0 && pMaterial->ScattersFullSphere() );

	// SHADOW GATE -- NM twin.  Full derivation of why a NULL
	// `pShadingObject` means "nobody opted out" rather than "skip the
	// shadow ray" is in EvaluateDirectLighting above; the short version is
	// that MediumTransport::EvaluateInScatteringNM is the only caller that
	// passes 0 and it is always a volume scatter vertex, whose NEE rows
	// were consequently unshadowed.
	const bool bReceivesShadows =
		pShadingObject ? pShadingObject->DoesReceiveShadows() : true;
    // Records are authoritative only for a completed query in this scene.
    // Shadow-disabled rays still need alpha-thinned medium boundaries, but
    // must pass ordinary blockers. Visibility-enabled rays reuse their one
    // sampled boundary decision, never draw coverage a second time.
    MediumBoundaryHits* sampledBoundaries = bSceneHasObjectMedia && bSceneHasAlphaCoverage ? &boundaryHits : nullptr;
    const auto mediumRecords = [&](const Ray& ray, Scalar distance) -> const MediumBoundaryHits* {
        if (sampledBoundaries && !bReceivesShadows) {
            pPreparedScene->GetObjects()->CollectMediumBoundaryHitsSampled(ray, distance, sampler, boundaryHits, pPreparedScene->GetCamera());
        }
        return sampledBoundaries;
    };


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
			if( spectralDomain ? !l->IsPositionalLight() : exitance <= 0 )
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
				// FULL-SPHERE NEE, DirectionalLight sibling (residual
				// wave 2 item D) -- see the RGB Step 1 site's comment.
				// VOLUME RECEIVER (residual-ledger item 10) -- spectral
				// twin of the RGB Step-1 site; both halves of that fix
				// (the `isVolumeScatter` flag and the medium
				// transmittance) are derived in full there.
				// DL-157 P1 -- see the RGB Step-1 site.
				if (sampledBoundaries) sampledBoundaries->clear();
                Scalar leNM = l->ComputeDirectLightingSampledNM( ri, caster, brdf,
					bReceivesShadows,
					nm, bFullSphere, isVolumeScatter, pMisIorStack, sampler, sampledBoundaries );

				if( leNM > 0 &&
					l->lightType() == ILight::LightType::Directional )
				{
					const Ray rayToLight( ri.ptIntersection, l->emissionDirection() );
					leNM *= EvalShadowTransmittanceNM( rayToLight,
						RISE_INFINITY, pMedium, pMediumObject,
						pPreparedScene, bSceneHasObjectMedia, nm, pMediumStack, mediumRecords(rayToLight, RISE_INFINITY) );
				}

				result += leNM;
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
				// DL-155: count the attempt -- see the RGB variant's
				// BVH branch for the derivation.
				if( pOptimalMIS && !pOptimalMIS->IsReady() )
				{
					const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
						ri.rast.x, ri.rast.y, kTechniqueNEE );
				}
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
				// DL-155: count the attempt -- see the RGB variant.
				if( pOptimalMIS && !pOptimalMIS->IsReady() )
				{
					const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
						ri.rast.x, ri.rast.y, kTechniqueNEE );
				}
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
				// DL-155: count the attempt -- see the RGB variant.
				if( pOptimalMIS && !pOptimalMIS->IsReady() )
				{
					const_cast<OptimalMISAccumulator*>(pOptimalMIS)->AccumulateCount(
						ri.rast.x, ri.rast.y, kTechniqueNEE );
				}
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
			// Non-mesh (delta-position) light. Recover its actual NM source
			// evaluator below: cached uplift for RGB authoring, or the
			// immutable authored SPD for a physical source.
			const Point3 lightPos = entry.pLight->position();
			Vector3 vToLight;
            Scalar dist;
            PhysicalLightSpectrum::ProductTerm distance;
            if(entry.physicalProductNM) {
                if(!PhysicalLightSpectrum::DirectionBetween(lightPos,ri.ptIntersection,vToLight,distance,dist)) {
                    GlobalLog()->PrintEasyError("Physical point/spot NEE requires a finite nonzero representable point distance.");
                    break;
                }
            } else {
                vToLight=Vector3Ops::mkVector3(lightPos,ri.ptIntersection);
                dist=Vector3Ops::NormalizeMag(vToLight);
            }
			const Scalar cosSurfaceSigned = isVolumeScatter ? Scalar(1.0) :
				ri.RayFacingShadingCosine( vToLight );
			// FULL-SPHERE NEE site 1 of 3 (NM): delta-position light.
			const Scalar cosSurface = bFullSphere ?
				std::fabs( cosSurfaceSigned ) : cosSurfaceSigned;

			if( !isVolumeScatter && cosSurface <= 0 ) break;

			// Shadow test.  shadowTNM carries the Fresnel transmittance
			// when transparent shadows are enabled (else 1.0).
			Scalar shadowTNM = 1.0;
			std::optional<GradedIndexMedium::ShadowSegmentTrack> gradedTrack = MakeGradedShadowTrack( pGradedIndexStack );
			if( bReceivesShadows )
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				if( ShadowOccludedNM( caster, rayToLight, entry.physicalProductNM ? dist : dist - 0.001, nm, shadowTNM, true /*DL-05: delta light*/ , sampler, sampledBoundaries, dist,
						gradedTrack ? &*gradedTrack : 0, &lightPos, bSMSCoversDeltaLights ) )
					break;
			}

            if(entry.physicalProductNM) {
                const Ray rayToLight(ri.ptIntersection,vToLight);
                const Scalar mediumTr=EvalShadowTransmittanceNM(rayToLight,dist,pMedium,pMediumObject,pPreparedScene,
                    bSceneHasObjectMedia,nm,pMediumStack,mediumRecords(rayToLight,dist));
                const Scalar graded=GradedNEESegmentScale(pGradedIndexStack,lightPos,gradedTrack ? &*gradedTrack : 0);
                const Scalar response=brdf.valueStatefulNM(vToLight,ri,nm,pMisIorStack);
                result+=entry.physicalProductNM(entry.physicalLight,-vToLight,nm,
                    {response,cosSurface,shadowTNM,risWeight,mediumTr,graded},{distance,distance,pdfAlias});
                break;
            }

			// emittedRadiance expects the outgoing direction FROM the
			// light; -vToLight is the light-to-surface direction.
			// Stage C slice 2: ask the light for its radiance AT `nm`.  This
			// used to be `ColorMath::Luminance( emittedRadiance(...) )` --
			// one Rec.709 luma scalar reused at every wavelength, which made
			// every coloured point / spot light spectrally GREY in NM renders
			// and tinted even a white one by the flat-spectrum chromaticity
			// (1.20, 0.95, 0.91).  This is the NEE row PT actually takes for
			// delta-position lights.
			const Scalar LeNM = entry.pLight->emittedRadianceNM( -vToLight, nm );
			const Scalar invDistSq = 1.0 / (dist * dist);
			const Scalar fBSDF = brdf.valueStatefulNM( vToLight, ri, nm, pMisIorStack );

			// Delta-position light: w = 1 (no MIS needed).  Fold in the
			// transparent-shadow Fresnel transmittance (1.0 when off).
			Scalar neeContrib = LeNM * fBSDF * cosSurface * invDistSq * shadowTNM
				   * (risWeight / pdfAlias);

			// Apply medium transmittance along shadow ray.
			// Multi-medium shadow transmittance.
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				neeContrib *= EvalShadowTransmittanceNM( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm, pMediumStack, mediumRecords(rayToLight, dist) );
			}

			// DL-09: the connection segment's graded-index factor.
			result += neeContrib *
				GradedNEESegmentScale( pGradedIndexStack, lightPos, gradedTrack ? &*gradedTrack : 0 );
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
		const Scalar cosSurfaceSigned = isVolumeScatter ? Scalar(1.0) :
			ri.RayFacingShadingCosine( vToLight );
		// FULL-SPHERE NEE site 2 of 3 (NM): mesh area light.  Same three
		// consumers as the RGB twin -- the `cosSurface <= 0` bail below,
		// the RR `estimate`, and `contrib`.  `cosLight` stays signed.
		const Scalar cosSurface = bFullSphere ?
			std::fabs( cosSurfaceSigned ) : cosSurfaceSigned;
		// DL-320: the FACE of the luminary this sample leaves from toward
		// the receiver.  A double-sided emitter radiates from both faces
		// (every strategy that HITS it sees the ray-facing normal), so the
		// face toward `-vToLight` emits; a one-sided emitter keeps the
		// winding normal and its back face stays dark (`cosLight <= 0`).
		// Area-measure density is unchanged (1/area); the hit-side MIS
		// partner (`fabs(cosLight)`, PathTracingIntegrator.cpp) already
		// describes this two-faced density.
		//
		// `lumNormal` is flipped IN PLACE to that face, so every later use
		// (the record, `Le`, the MIS cosine) reads the emitting face and a
		// one-sided luminary runs the pre-DL-320 code unchanged; the
		// surface-payload probe keeps the winding normal (`lumWinding`).
		const Vector3 lumWinding = lumNormal;
		if( entry.twoSided && Vector3Ops::Dot( -vToLight, lumNormal ) < 0 ) {
			lumNormal = -lumNormal;
		}
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
		std::optional<GradedIndexMedium::ShadowSegmentTrack> gradedTrack = MakeGradedShadowTrack( pGradedIndexStack );
		if( bReceivesShadows )
		{
			const Ray rayToLight( ri.ptIntersection, vToLight );
			if( ShadowOccludedNM( caster, rayToLight, dist - 0.001, nm, meshShadowTNM, false /*DL-05: area light -- see CastShadowRayAuto*/ , sampler, sampledBoundaries, dist,
						gradedTrack ? &*gradedTrack : 0, &ptOnLum ) )
			{
				break;
			}
		}

		RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
		lumri.vNormal = lumNormal;
		// `lumNormal` is `UniformRandomPoint`'s normal (negated only for a
		// double-sided emitter's back face, DL-320) -- the INTERPOLATED
		// VERTEX normal on a mesh luminary with per-vertex normals, the face
		// normal otherwise (see the RGB twin above and
		// `GeometricUtilities::PointOnTriangle`); mirror it so the record is
		// self-consistent.
		// DL-298: the same shared fill as the RGB twin above (world
		// position, normals, `onb`, UV, `Po`).
		FillEmitterRecord( lumri, ptOnLum, lumNormal, lumCoord,
			EmitterObjectPoint( lumEntry.pLum, ptOnLum, lumri.ptObjIntersec ),
			lumEntry.pLum, pPreparedScene ? pPreparedScene->GetCamera() : 0 );

		// THE SHADING PAYLOAD -- the NM twin of the RGB site above, calling
		// the SAME functions with the same arguments, including the same
		// normal-aligned probe and the same gate fast-path.  See that site
		// for the full rationale (slice S3 of
		// docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5).
		if( EmitterProbeWanted() ) {
			EmitterSurfacePayload lumSurface;
			ProbeEmitterSurface(
				lumEntry.pLum,
				pPreparedScene ? pPreparedScene->GetObjects() : 0,
				ptOnLum, lumWinding, lumSurface );
			ApplyEmitterSurface( lumri, lumSurface );
		}
		const Scalar Le = AcceptEmitterAlpha(lumEntry.pLum, pPreparedScene ? pPreparedScene->GetObjects() : nullptr, ptOnLum, lumri, sampler, &ri.rast)
            ? pEmitter->emittedRadianceNM( lumri, -vToLight, lumNormal, nm ) : 0;

		const Scalar geom = area * cosLight / (dist * dist);
		Scalar contrib = Le * cosSurface * geom * brdf.valueStatefulNM( vToLight, ri, nm, pMisIorStack ) * meshShadowTNM;

		// Multi-medium shadow transmittance.
		{
			const Ray rayToLight( ri.ptIntersection, vToLight );
			contrib *= EvalShadowTransmittanceNM( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm, pMediumStack, mediumRecords(rayToLight, dist) );
		}

		// MIS when selection PDF is tractable (alias table or BVH)
		const bool bCanDoMIS_NM = (pLightBVH && pLightBVH->IsBuilt()) ||
			risCandidates == 0;
		// DL-171/DL-209 -- see the RGB area-light arm's twin.
		if( bCanDoMIS_NM && pMaterial && bBsdfSamplingPartnerExists && area > 0 && cosLight > 0 )
		{
			const Scalar p_light = pdfAlias * (dist * dist) / (area * cosLight);
			static const IORStack defaultIOR( 1.0 );
			// DL-74 (P1-B): NM twin of the RGB area-light MIS block -- see
			// that block's comment for the derivation.  DL-74 P2: and its
			// live-IOR-stack rule, for the same reason.
			const IORStack& misIOR = pMisIorStack ? *pMisIorStack : defaultIOR;
			Scalar p_bsdf = pMaterial->PdfNM( vToLight, ri, nm, misIOR );
			// DL-74 P2-2: outside the `> 0` gate -- see the RGB copy.
			if( pGuidedBlend && !isVolumeScatter ) {
				p_bsdf = pGuidedBlend->Blend( vToLight, p_bsdf );
			}

			// Optimal MIS training (spectral NEE): contrib includes
			// the geometry factor, so use pdfAlias (area measure)
			// to match.  f2/pdfAlias^2 = (contrib/pdfAlias)^2.
			if( pOptimalMIS && !pOptimalMIS->IsReady() )
			{
				// DL-72 P2-3: NM twin -- see the RGB area-light arm.
				// DL-84 round 7: `rrSurvivalCompensation` likewise -- NM twin
				// of the realized-moment note in the RGB arm.
				// Round-2 review (P3-4): `risWeight` folded in -- NM twin of
				// the RGB arm's fix; a no-op while `risCandidates == 0`
				// (the default).
				const Scalar scaled = neeTrainingScale * rrSurvivalCompensation * risWeight * contrib;
				const Scalar f2 = scaled * scaled;
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

		// DL-09: the connection segment's graded-index factor (after
		// training -- see the RGB twin).
		result += contrib * ( ( rrSurvivalCompensation * risWeight / pdfAlias ) *
			GradedNEESegmentScale( pGradedIndexStack, ptOnLum, gradedTrack ? &*gradedTrack : 0 ) );
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

		const Scalar cosEnvSigned = isVolumeScatter ? Scalar(1.0) :
			ri.RayFacingShadingCosine( envDir );
		// FULL-SPHERE NEE site 3 of 3 (NM): environment map.
		const Scalar cosEnv = bFullSphere ?
			std::fabs( cosEnvSigned ) : cosEnvSigned;

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
			if( bReceivesShadows )
			{
				const Ray rayToEnv( ri.ptIntersection, envDir );
				envShadowed = ShadowOccludedNM( caster, rayToEnv, RISE_INFINITY, nm, envShadowTNM, false /*DL-05: env light -- see CastShadowRayAuto*/ , sampler, sampledBoundaries );
			}

			if( !envShadowed )
			{
				const Ray envRay( ri.ptIntersection, envDir );
				const Scalar Le = pEnvironmentMap->GetRadianceNM( envRay, nullRasterizerState, nm );
				const Scalar f = brdf.valueStatefulNM( envDir, ri, nm, pMisIorStack );
				Scalar envContrib = Le * f * cosEnv / envPdf * envShadowTNM;

				// Apply medium transmittance for environment ray.
				// Multi-medium shadow transmittance.
				{
					envContrib *= EvalShadowTransmittanceNM( envRay, RISE_INFINITY, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm, pMediumStack, mediumRecords(envRay, RISE_INFINITY) );
				}

				// Optimal MIS training (spectral env NEE): use full
				// integrand.  envContrib = Le*BSDF*cos*Tr/envPdf,
				// so fullIntegrand = envContrib * envPdf.
				if( pOptimalMIS && !pOptimalMIS->IsReady() && pMaterial )
				{
					// DL-72 P2-3: NM twin -- see the RGB area-light arm.
					const Scalar fullIntegrand = neeTrainingScale * envContrib * envPdf;
					const Scalar f2 = fullIntegrand * fullIntegrand;
					if( f2 > 0 && envPdf > 0 )
					{
						const_cast<OptimalMISAccumulator*>(pOptimalMIS)->Accumulate(
							ri.rast.x, ri.rast.y,
							f2, envPdf, kTechniqueNEE );
					}
				}

				// DL-73 (docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md) /
				// DL-74 (docs/DL74_ENV_NEE_GUIDING_PARTITION.md):
				// NM twin of the RGB env-NEE MIS block above -- see that
				// block's comment for the full derivation. `pGuidedBlend` is
				// only ever non-null from PathTracingIntegrator's SURFACE NM
				// NEE call site (never the volume one) and is additionally
				// gated on `!isVolumeScatter` here as a local safety net, so
				// this can never disturb DL-73's confirmed-unbiased volume
				// pairing.
				//
				// DL-171/DL-209 -- see the RGB env arm's twin.
				if( pMaterial && bBsdfSamplingPartnerExists )
				{
					static const IORStack defaultIOR( 1.0 );
					// DL-74 P2: live stack, not the 1.0 sentinel.
					const IORStack& misIOR = pMisIorStack ? *pMisIorStack : defaultIOR;
					Scalar pBsdf = pMaterial->PdfNM( envDir, ri, nm, misIOR );
					// DL-74 P2-2: blend OUTSIDE the `> 0` gate -- see the
					// RGB copy for the derivation.
					if( pGuidedBlend && !isVolumeScatter )
					{
						pBsdf = pGuidedBlend->Blend( envDir, pBsdf );
					}
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
