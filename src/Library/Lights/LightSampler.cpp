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
#include "../Utilities/ISampler.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IEmitter.h"
#include "../Interfaces/IRayCaster.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Intersection/RayIntersection.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/PathTransportUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

// ----------------------------------------------------------------
// Shadow-ray medium transmittance
//
// Walks the shadow ray boundary-by-boundary through the scene,
// maintaining a small stack of active per-object media.  At each
// intersection with a medium-bearing object:
//
//   Front-face hit → push that object's medium onto the stack
//   Back-face hit  → remove that object's medium from the stack
//
// Between consecutive boundaries the active medium (stack top, or
// global medium when the stack is empty) determines the
// transmittance for that segment.  This correctly handles:
//
//   - Disjoint media (ray enters A, exits A, enters B, exits B)
//   - Nested media   (ray inside A, enters B, exits B, exits A)
//   - Overlapping    (ray enters A, enters B, exits A, exits B)
//
// The innermost per-object medium always takes priority, matching
// MediumTracking's IOR-stack-based resolution and Cycles' volume
// stack semantics.
//
// Performance:
//   - Scenes with no media: one quick-exit check, zero scene queries
//   - Scenes with media: one scene IntersectRay per boundary crossed.
//     Bounded by MAX_WALK_STEPS and early Tr < 1e-6 termination.
//
// Bounds:
//   - Nesting depth: MAX_DEPTH = 4.  If more than 4 per-object media
//     overlap at one point, entries beyond the 4th are silently
//     dropped (under-attenuation).
//   - Boundary count: MAX_WALK_STEPS = 16.  If the ray crosses more
//     than 16 medium boundaries, the walk stops and the remaining
//     distance uses only the stack state at that point (potential
//     under-attenuation for segments not yet discovered).
//   Both limits are conservative for realistic scenes.
//
// Limitation: global-medium transmittance for the non-object
// segments uses a single EvalTransmittance call with the summed
// vacuum distance, which is exact for homogeneous global media
// but approximate for heterogeneous global media.
// ----------------------------------------------------------------

/// Small fixed-capacity stack of active per-object media along a
/// shadow ray.  Supports push, removal by object pointer (for
/// non-LIFO exit order in overlapping geometry), and top() query.
/// Nesting depth beyond 4 is extremely rare in practice.
struct ShadowMediumStack
{
	static const int MAX_DEPTH = 4;

	struct Entry
	{
		const IObject* pObj;
		const IMedium* pMedium;
	};

	Entry entries[MAX_DEPTH];
	int   count;

	ShadowMediumStack() : count( 0 ) {}

	void push( const IObject* pObj, const IMedium* pMedium )
	{
		if( count < MAX_DEPTH ) {
			entries[count].pObj = pObj;
			entries[count].pMedium = pMedium;
			count++;
		}
	}

	/// Remove the entry matching pObj.  Handles non-LIFO removal
	/// when overlapping objects exit in a different order than they
	/// were entered.
	void remove( const IObject* pObj )
	{
		for( int i = 0; i < count; i++ ) {
			if( entries[i].pObj == pObj ) {
				for( int j = i; j < count - 1; j++ ) {
					entries[j] = entries[j + 1];
				}
				count--;
				return;
			}
		}
	}

	/// Returns the innermost (most recently pushed) per-object
	/// medium, or NULL if the stack is empty (global medium or
	/// vacuum is active).
	const IMedium* top() const
	{
		return count > 0 ? entries[count - 1].pMedium : 0;
	}

	bool empty() const { return count == 0; }
};

/// Evaluate transmittance along a shadow ray, accounting for
/// nested, overlapping, and disjoint per-object media as well
/// as the global medium.
///
/// Uses a boundary walk with a medium stack (see block comment
/// above).  Bounded by MAX_DEPTH (4 simultaneous overlapping
/// media) and MAX_WALK_STEPS (16 boundary crossings); rays
/// that exceed either limit will under-attenuate silently.
static RISEPel EvalShadowTransmittance(
	const Ray& ray,
	const Scalar maxDist,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IScene* pScene,
	const bool bSceneHasObjectMedia
	)
{
	RISEPel Tr( 1, 1, 1 );
	if( !pScene || maxDist <= 0 ) {
		return Tr;
	}

	const IMedium* pGlobalMedium = pScene->GetGlobalMedium();
	const IObjectManager* pObjects = pScene->GetObjects();

	// Quick exit: no media anywhere
	if( !pOriginMedium && !pGlobalMedium && !bSceneHasObjectMedia ) {
		return Tr;
	}

	// Fast path: no per-object media in scene, just apply
	// origin/global medium for the full distance.
	if( !bSceneHasObjectMedia ) {
		const IMedium* pMedium = pOriginMedium ? pOriginMedium : pGlobalMedium;
		if( pMedium ) {
			return pMedium->EvalTransmittance( ray, maxDist );
		}
		return Tr;
	}

	static const Scalar WALK_EPSILON = 1e-5;
	static const int MAX_WALK_STEPS = 16;

	// Initialize medium stack with the origin medium (if per-object)
	ShadowMediumStack stack;
	if( pOriginMedium && pOriginMediumObject ) {
		stack.push( pOriginMediumObject, pOriginMedium );
	}

	// Walk boundary-by-boundary.  Each iteration finds the nearest
	// intersection, applies the active medium's transmittance for
	// the segment leading up to it, then updates the stack.
	Scalar segStart = 0;
	Scalar objectCoveredDist = 0;

	if( pObjects )
	{
		for( int step = 0; step < MAX_WALK_STEPS && segStart < maxDist; step++ )
		{
			// Cast from segStart + epsilon to avoid re-hitting the
			// boundary we just processed.  On the first iteration
			// (segStart == 0) we still add epsilon to avoid self-
			// intersection at the shading point.
			const Scalar castStart = segStart + WALK_EPSILON;
			if( castStart >= maxDist ) {
				break;
			}

			const Point3 castOrigin = ray.PointAtLength( castStart );
			const Ray castRay( castOrigin, ray.Dir() );
			const Scalar castMax = maxDist - castStart;

			RasterizerState nullRast = {0};
			RayIntersection ri( castRay, nullRast );
			pObjects->IntersectRay( ri, true, true, false );

			if( !ri.geometric.bHit || ri.geometric.range >= castMax ) {
				// No more boundaries before maxDist.
				// Apply the active medium for [segStart, maxDist].
				const Scalar remaining = maxDist - segStart;
				if( remaining > 0 ) {
					const IMedium* pActive = stack.top();
					if( pActive ) {
						const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
						Tr = Tr * pActive->EvalTransmittance( segRay, remaining );
						objectCoveredDist += remaining;
					}
				}
				segStart = maxDist;
				break;
			}

			const IObject* pHitObj = ri.pObject;
			if( !pHitObj ) {
				break;
			}

			// Absolute distance along the original ray to this boundary
			const Scalar boundaryDist = castStart + ri.geometric.range;

			// Apply the active medium for the segment [segStart, boundaryDist]
			const Scalar segLen = boundaryDist - segStart;
			if( segLen > 0 ) {
				const IMedium* pActive = stack.top();
				if( pActive ) {
					const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
					Tr = Tr * pActive->EvalTransmittance( segRay, segLen );
					objectCoveredDist += segLen;
				}
			}

			// Update the stack based on this boundary.
			// Only medium-bearing objects affect the stack; non-medium
			// objects are ignored (the walk advances past them).
			const IMedium* pObjMedium = pHitObj->GetInteriorMedium();
			if( pObjMedium ) {
				const Scalar ndotd = Vector3Ops::Dot( ri.geometric.vNormal, ray.Dir() );
				if( ndotd < 0 ) {
					// Front-face: entering this object
					stack.push( pHitObj, pObjMedium );
				} else {
					// Back-face: exiting this object
					stack.remove( pHitObj );
				}
			}

			segStart = boundaryDist;

			// Early termination when transmittance is negligible
			if( ColorMath::MaxValue( Tr ) < 1e-6 ) {
				return RISEPel( 0, 0, 0 );
			}
		}
	}

	// Handle remaining distance if the walk ended before maxDist
	// (either max steps reached, or no pObjects)
	if( segStart < maxDist ) {
		const Scalar remaining = maxDist - segStart;
		if( remaining > 0 ) {
			const IMedium* pActive = stack.top();
			if( pActive ) {
				const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
				Tr = Tr * pActive->EvalTransmittance( segRay, remaining );
				objectCoveredDist += remaining;
			}
		}
	}

	// Apply global medium transmittance for segments where no
	// per-object medium was active.  The global medium is the
	// fallback when the stack is empty, so it covers exactly
	// maxDist minus the per-object distance.
	if( pGlobalMedium ) {
		const Scalar globalDist = maxDist - objectCoveredDist;
		if( globalDist > WALK_EPSILON ) {
			// For homogeneous global media this is exact (transmittance
			// depends only on total distance).  For heterogeneous global
			// media this is approximate — a per-segment evaluation would
			// be needed, but heterogeneous global media are uncommon.
			Tr = Tr * pGlobalMedium->EvalTransmittance( ray, globalDist );
		}
	}

	return Tr;
}

/// Spectral variant of EvalShadowTransmittance.
/// Same boundary walk, medium stack, and depth/step bounds;
/// rays exceeding MAX_DEPTH or MAX_WALK_STEPS will under-
/// attenuate silently.  Operates on scalar transmittance at
/// a single wavelength.
static Scalar EvalShadowTransmittanceNM(
	const Ray& ray,
	const Scalar maxDist,
	const IMedium* pOriginMedium,
	const IObject* pOriginMediumObject,
	const IScene* pScene,
	const bool bSceneHasObjectMedia,
	const Scalar nm
	)
{
	Scalar Tr = 1;
	if( !pScene || maxDist <= 0 ) {
		return Tr;
	}

	const IMedium* pGlobalMedium = pScene->GetGlobalMedium();
	const IObjectManager* pObjects = pScene->GetObjects();

	if( !pOriginMedium && !pGlobalMedium && !bSceneHasObjectMedia ) {
		return Tr;
	}

	if( !bSceneHasObjectMedia ) {
		const IMedium* pMedium = pOriginMedium ? pOriginMedium : pGlobalMedium;
		if( pMedium ) {
			return pMedium->EvalTransmittanceNM( ray, maxDist, nm );
		}
		return Tr;
	}

	static const Scalar WALK_EPSILON = 1e-5;
	static const int MAX_WALK_STEPS = 16;

	ShadowMediumStack stack;
	if( pOriginMedium && pOriginMediumObject ) {
		stack.push( pOriginMediumObject, pOriginMedium );
	}

	Scalar segStart = 0;
	Scalar objectCoveredDist = 0;

	if( pObjects )
	{
		for( int step = 0; step < MAX_WALK_STEPS && segStart < maxDist; step++ )
		{
			const Scalar castStart = segStart + WALK_EPSILON;
			if( castStart >= maxDist ) {
				break;
			}

			const Point3 castOrigin = ray.PointAtLength( castStart );
			const Ray castRay( castOrigin, ray.Dir() );
			const Scalar castMax = maxDist - castStart;

			RasterizerState nullRast = {0};
			RayIntersection ri( castRay, nullRast );
			pObjects->IntersectRay( ri, true, true, false );

			if( !ri.geometric.bHit || ri.geometric.range >= castMax ) {
				const Scalar remaining = maxDist - segStart;
				if( remaining > 0 ) {
					const IMedium* pActive = stack.top();
					if( pActive ) {
						const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
						Tr *= pActive->EvalTransmittanceNM( segRay, remaining, nm );
						objectCoveredDist += remaining;
					}
				}
				segStart = maxDist;
				break;
			}

			const IObject* pHitObj = ri.pObject;
			if( !pHitObj ) {
				break;
			}

			const Scalar boundaryDist = castStart + ri.geometric.range;

			const Scalar segLen = boundaryDist - segStart;
			if( segLen > 0 ) {
				const IMedium* pActive = stack.top();
				if( pActive ) {
					const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
					Tr *= pActive->EvalTransmittanceNM( segRay, segLen, nm );
					objectCoveredDist += segLen;
				}
			}

			const IMedium* pObjMedium = pHitObj->GetInteriorMedium();
			if( pObjMedium ) {
				const Scalar ndotd = Vector3Ops::Dot( ri.geometric.vNormal, ray.Dir() );
				if( ndotd < 0 ) {
					stack.push( pHitObj, pObjMedium );
				} else {
					stack.remove( pHitObj );
				}
			}

			segStart = boundaryDist;

			if( Tr < 1e-6 ) {
				return 0;
			}
		}
	}

	if( segStart < maxDist ) {
		const Scalar remaining = maxDist - segStart;
		if( remaining > 0 ) {
			const IMedium* pActive = stack.top();
			if( pActive ) {
				const Ray segRay( ray.PointAtLength( segStart ), ray.Dir() );
				Tr *= pActive->EvalTransmittanceNM( segRay, remaining, nm );
				objectCoveredDist += remaining;
			}
		}
	}

	if( pGlobalMedium ) {
		const Scalar globalDist = maxDist - objectCoveredDist;
		if( globalDist > WALK_EPSILON ) {
			Tr *= pGlobalMedium->EvalTransmittanceNM( ray, globalDist, nm );
		}
	}

	return Tr;
}

using PathTransportUtilities::PowerHeuristic;

LightSampler::LightSampler() :
  pPreparedScene( 0 ),
  pPreparedLuminaries( 0 ),
  cachedTotalExitance( 0 ),
  risCandidates( 0 ),
  lightSampleRRThreshold( 0 ),
  bSceneHasObjectMedia( false ),
  pEnvSampler( 0 ),
  pEnvironmentMap( 0 )
{
}

LightSampler::~LightSampler()
{
	if( pEnvSampler ) {
		pEnvSampler->release();
		pEnvSampler = 0;
	}
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
	pEnvironmentMap = pEnvMap;
	pEnvSampler = pSampler;
	if( pEnvSampler ) {
		pEnvSampler->addref();
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
	if( risCandidates == 0 && lightEntries.size() > 8 )
	{
		risCandidates = 8;
	}

	// Scan objects for interior media.  This flag gates the
	// multi-medium shadow transmittance walk — when no objects
	// have media and there's no global medium, shadow transmittance
	// evaluation is skipped entirely.
	bSceneHasObjectMedia = false;
	{
		struct MediaScan : public IEnumCallback<IObject>
		{
			bool found;
			MediaScan() : found(false) {}
			bool operator()( const IObject& obj )
			{
				if( obj.GetInteriorMedium() ) {
					found = true;
					return false;  // stop enumeration
				}
				return true;  // continue
			}
		};
		MediaScan scan;
		scene.GetObjects()->EnumerateObjects( scan );
		bSceneHasObjectMedia = scan.found;
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
	sample.isDelta = false;
	sample.Le = RISEPel( 0, 0, 0 );
	sample.pdfPosition = 0;
	sample.pdfDirection = 0;
	sample.pdfSelect = 0;

	if( !aliasTable.IsValid() )
	{
		return false;
	}

	// O(1) selection proportional to exitance (no RIS for emission sampling)
	const unsigned int idx = aliasTable.Sample( sampler.Get1D() );
	const LightEntry& entry = lightEntries[idx];
	sample.pdfSelect = static_cast<Scalar>( aliasTable.Pdf( idx ) );

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
		rig.ptCoord = coord;
		rig.onb = onb;

		sample.Le = pEmitter->emittedRadiance( rig, sample.direction, sample.normal );
	}

	return true;
}

Scalar LightSampler::PdfSelectLight(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const ILight& light
	) const
{
	// Find the matching entry in the light table
	for( unsigned int i = 0; i < lightEntries.size(); i++ )
	{
		if( lightEntries[i].pLight == &light )
		{
			return static_cast<Scalar>( aliasTable.Pdf( i ) );
		}
	}

	return 0;
}

Scalar LightSampler::PdfSelectLuminary(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const IObject& luminary
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
				return static_cast<Scalar>( aliasTable.Pdf( i ) );
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
	const IObject& luminary
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
	const IObject* pMediumObject
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
		if( !aliasTable.IsValid() )
		{
			break;
		}

		// Find self in the light table (for exclusion)
		const int selfIdx = FindLuminaryIndex( pShadingObject );

		unsigned int idx;
		Scalar pdfAlias;
		Scalar risWeight = 1.0;

		if( risCandidates > 0 )
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

			// Shadow test
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				if( caster.CastShadowRay( rayToLight, dist - 0.001 ) )
					break;
			}

			// emittedRadiance expects the outgoing direction FROM the
			// light; -vToLight is the light-to-surface direction.
			const RISEPel Le = entry.pLight->emittedRadiance( -vToLight );
			const RISEPel fBSDF = brdf.value( vToLight, ri );
			const Scalar invDistSq = 1.0 / (dist * dist);

			// Delta-position light: w = 1 (no MIS needed).
			RISEPel amount = Le * fBSDF * cosSurface * invDistSq;

			// Apply medium transmittance along shadow ray.
			// Multi-medium shadow transmittance (origin, per-object,
			// and global media; bounded by stack depth and step count).
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				const RISEPel Tr = EvalShadowTransmittance( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia );
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

				// Shadow test
				bool shadowed = false;
				if( pShadingObject && pShadingObject->DoesReceiveShadows() )
				{
					const Ray rayToLight( ri.ptIntersection, vToLight );
					shadowed = caster.CastShadowRay( rayToLight, dist - 0.001 );
				}

				if( !shadowed )
				{
					// Emitted radiance at sampled point
					RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
					lumri.vNormal = lumNormal;
					lumri.ptCoord = lumCoord;
					lumri.onb.CreateFromW( lumNormal );

					const RISEPel Le = pEmitter->emittedRadiance( lumri, -vToLight, lumNormal );

					const Scalar geom = area * cosLight / (dist * dist);
					RISEPel contrib = Le * cosSurface * geom * brdf.value( vToLight, ri );

					// Apply medium transmittance along shadow ray.
					// Multi-medium shadow transmittance.
					{
						const Ray rayToLight( ri.ptIntersection, vToLight );
						const RISEPel Tr = EvalShadowTransmittance( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia );
						contrib = contrib * Tr;
					}

					// MIS weight: only when RIS is OFF.  When RIS is ON
					// the exact finite-M technique density is intractable,
					// so we use w=1 here and suppress the BSDF-hit emitter
					// contribution in PathTracingShaderOp.
					if( risCandidates == 0 && pMaterial && area > 0 && cosLight > 0 )
					{
						// Convert alias-table selection PDF to solid angle.
						// Self-exclusion uses single-draw skip-self, so the
						// selection PDF is the raw alias-table p(j) — no
						// correction needed (see proof above).
						const Scalar p_light = pdfAlias * (dist * dist) / (area * cosLight);
						const Scalar p_bsdf = pMaterial->Pdf( vToLight, ri, 0 );

						if( p_bsdf > 0 )
						{
							const Scalar w = PowerHeuristic( p_light, p_bsdf );
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
	if( pEnvSampler && pEnvSampler->IsValid() && pEnvironmentMap )
	{
		Vector3 envDir;
		Scalar envPdf;
		pEnvSampler->Sample( sampler.Get1D(), sampler.Get1D(), envDir, envPdf );

		const Scalar cosEnv = isVolumeScatter ? Scalar(1.0) :
			Vector3Ops::Dot( envDir, ri.vNormal );
		if( (isVolumeScatter || cosEnv > 0) && envPdf > 0 )
		{
			// Shadow test: cast ray to infinity
			bool envShadowed = false;
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToEnv( ri.ptIntersection, envDir );
				envShadowed = caster.CastShadowRay( rayToEnv, RISE_INFINITY );
			}

			if( !envShadowed )
			{
				const Ray envRay( ri.ptIntersection, envDir );
				const RISEPel Le = pEnvironmentMap->GetRadiance( envRay, nullRasterizerState );
				const RISEPel f = brdf.value( envDir, ri );
				RISEPel envContrib = Le * f * (cosEnv / envPdf);

				// Apply medium transmittance for environment ray.
				// Multi-medium shadow transmittance.
				// Use a large but finite distance for the walk limit
				// (environment rays go to infinity, but media are bounded).
				{
					const RISEPel Tr = EvalShadowTransmittance( envRay, RISE_INFINITY, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia );
					envContrib = envContrib * Tr;
				}

				// MIS: power heuristic against BSDF PDF
				if( pMaterial )
				{
					const Scalar pBsdf = pMaterial->Pdf( envDir, ri, 0 );
					if( pBsdf > 0 )
					{
						const Scalar w = PowerHeuristic( envPdf, pBsdf );
						envContrib = envContrib * w;
					}
				}

				result = result + envContrib;
			}
		}
	}

	return result;
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
	const IObject* pMediumObject
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
			const Scalar exitance = ColorMath::MaxValue( l->radiantExitance() );
			if( exitance <= 0 )
			{
				// Evaluate using luminance approximation of RGB contribution
				RISEPel amount( 0, 0, 0 );
				l->ComputeDirectLighting( ri, caster, brdf,
					pShadingObject ? pShadingObject->DoesReceiveShadows() : true,
					amount );
				result += ColorMath::Luminance( amount );
			}
		}
	}

	// ----------------------------------------------------------------
	// Step 2: Select one light with nonzero exitance, excluding self.
	// ----------------------------------------------------------------
	do {
		if( !aliasTable.IsValid() )
		{
			break;
		}

		const int selfIdx = FindLuminaryIndex( pShadingObject );

		unsigned int idx;
		Scalar pdfAlias;
		Scalar risWeight = 1.0;

		if( risCandidates > 0 )
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

			// Shadow test
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				if( caster.CastShadowRay( rayToLight, dist - 0.001 ) )
					break;
			}

			// emittedRadiance expects the outgoing direction FROM the
			// light; -vToLight is the light-to-surface direction.
			const RISEPel LeRGB = entry.pLight->emittedRadiance( -vToLight );
			const Scalar LeNM = ColorMath::Luminance( LeRGB );
			const Scalar invDistSq = 1.0 / (dist * dist);
			const Scalar fBSDF = brdf.valueNM( vToLight, ri, nm );

			// Delta-position light: w = 1 (no MIS needed).
			Scalar neeContrib = LeNM * fBSDF * cosSurface * invDistSq
				   * (risWeight / pdfAlias);

			// Apply medium transmittance along shadow ray.
			// Multi-medium shadow transmittance.
			{
				const Ray rayToLight( ri.ptIntersection, vToLight );
				neeContrib *= EvalShadowTransmittanceNM( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm );
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

		if( pShadingObject && pShadingObject->DoesReceiveShadows() )
		{
			const Ray rayToLight( ri.ptIntersection, vToLight );
			if( caster.CastShadowRay( rayToLight, dist - 0.001 ) )
			{
				break;
			}
		}

		RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
		lumri.vNormal = lumNormal;
		lumri.ptCoord = lumCoord;
		lumri.onb.CreateFromW( lumNormal );

		const Scalar Le = pEmitter->emittedRadianceNM( lumri, -vToLight, lumNormal, nm );

		const Scalar geom = area * cosLight / (dist * dist);
		Scalar contrib = Le * cosSurface * geom * brdf.valueNM( vToLight, ri, nm );

		// Multi-medium shadow transmittance.
		{
			const Ray rayToLight( ri.ptIntersection, vToLight );
			contrib *= EvalShadowTransmittanceNM( rayToLight, dist, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm );
		}

		// MIS only when RIS is OFF
		if( risCandidates == 0 && pMaterial && area > 0 && cosLight > 0 )
		{
			const Scalar p_light = pdfAlias * (dist * dist) / (area * cosLight);
			const Scalar p_bsdf = pMaterial->PdfNM( vToLight, ri, nm, 0 );

			if( p_bsdf > 0 )
			{
				const Scalar w = PowerHeuristic( p_light, p_bsdf );
				contrib = contrib * w;
			}
		}

		result = contrib * (rrSurvivalCompensation * risWeight / pdfAlias);
	} while( false );

	// Environment map NEE (spectral path)
	if( pEnvSampler && pEnvSampler->IsValid() && pEnvironmentMap )
	{
		Vector3 envDir;
		Scalar envPdf;
		pEnvSampler->Sample( sampler.Get1D(), sampler.Get1D(), envDir, envPdf );

		const Scalar cosEnv = isVolumeScatter ? Scalar(1.0) :
			Vector3Ops::Dot( envDir, ri.vNormal );
		if( (isVolumeScatter || cosEnv > 0) && envPdf > 0 )
		{
			bool envShadowed = false;
			if( pShadingObject && pShadingObject->DoesReceiveShadows() )
			{
				const Ray rayToEnv( ri.ptIntersection, envDir );
				envShadowed = caster.CastShadowRay( rayToEnv, RISE_INFINITY );
			}

			if( !envShadowed )
			{
				const Ray envRay( ri.ptIntersection, envDir );
				const Scalar Le = pEnvironmentMap->GetRadianceNM( envRay, nullRasterizerState, nm );
				const Scalar f = brdf.valueNM( envDir, ri, nm );
				Scalar envContrib = Le * f * cosEnv / envPdf;

				// Apply medium transmittance for environment ray.
				// Multi-medium shadow transmittance.
				{
					envContrib *= EvalShadowTransmittanceNM( envRay, RISE_INFINITY, pMedium, pMediumObject, pPreparedScene, bSceneHasObjectMedia, nm );
				}

				if( pMaterial )
				{
					const Scalar pBsdf = pMaterial->PdfNM( envDir, ri, nm, 0 );
					if( pBsdf > 0 )
					{
						const Scalar w = PowerHeuristic( envPdf, pBsdf );
						envContrib = envContrib * w;
					}
				}

				result += envContrib;
			}
		}
	}

	return result;
}
