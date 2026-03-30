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
#include "../Intersection/RayIntersectionGeometric.h"

using namespace RISE;
using namespace RISE::Implementation;

/// Power heuristic weight: w = pa^2 / (pa^2 + pb^2)
static inline Scalar PowerHeuristic( const Scalar pa, const Scalar pb )
{
	const Scalar pa2 = pa * pa;
	return pa2 / (pa2 + pb * pb);
}

LightSampler::LightSampler() :
  pPreparedScene( 0 ),
  pPreparedLuminaries( 0 ),
  cachedTotalExitance( 0 ),
  risCandidates( 0 )
{
}

LightSampler::~LightSampler()
{
}

void LightSampler::SetRISCandidates(
	const unsigned int M
	)
{
	risCandidates = M;
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
	const IObject* pShadingObject
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
	// ----------------------------------------------------------------
	if( !aliasTable.IsValid() )
	{
		return result;
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
			// dimension alignment) and return zero.
			sampler.Get1D();
			sampler.Get1D();
			sampler.Get1D();
			return result;
		}
	}
	else
	{
		// Single alias-table draw with exact self-exclusion.
		//
		// Draw one sample.  If it is self, return zero immediately.
		// No retry loop, no correction factor needed.
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
			// alignment) and return zero.
			sampler.Get1D();
			sampler.Get1D();
			sampler.Get1D();
			return result;
		}

		pdfAlias = static_cast<Scalar>( aliasTable.Pdf( idx ) );
	}

	const LightEntry& entry = lightEntries[idx];

	if( entry.pLight )
	{
		// Selected a non-mesh (delta) light.
		// w = 1 (no alternative sampling strategy).
		RISEPel amount( 0, 0, 0 );
		entry.pLight->ComputeDirectLighting( ri, caster, brdf,
			pShadingObject ? pShadingObject->DoesReceiveShadows() : true,
			amount );
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
		const Scalar cosSurface = Vector3Ops::Dot( vToLight, ri.vNormal );
		const Scalar cosLight = Vector3Ops::Dot( -vToLight, lumNormal );

		if( cosLight > 0 && cosSurface > 0 )
		{
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

				result = result + contrib * (risWeight / pdfAlias);
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
	const IObject* pShadingObject
	) const
{
	Scalar result = 0;

	if( !pPreparedScene || !pPreparedLuminaries )
	{
		return result;
	}

	if( !aliasTable.IsValid() )
	{
		return result;
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
			return result;
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
			return result;
		}

		pdfAlias = static_cast<Scalar>( aliasTable.Pdf( idx ) );
	}

	const LightEntry& entry = lightEntries[idx];

	if( entry.pLight )
	{
		// Non-mesh light — no spectral evaluation available
		return result;
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
	const Scalar cosSurface = Vector3Ops::Dot( vToLight, ri.vNormal );
	const Scalar cosLight = Vector3Ops::Dot( -vToLight, lumNormal );

	if( cosLight <= 0 || cosSurface <= 0 )
	{
		return result;
	}

	if( pShadingObject && pShadingObject->DoesReceiveShadows() )
	{
		const Ray rayToLight( ri.ptIntersection, vToLight );
		if( caster.CastShadowRay( rayToLight, dist - 0.001 ) )
		{
			return result;
		}
	}

	RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
	lumri.vNormal = lumNormal;
	lumri.ptCoord = lumCoord;
	lumri.onb.CreateFromW( lumNormal );

	const Scalar Le = pEmitter->emittedRadianceNM( lumri, -vToLight, lumNormal, nm );

	const Scalar geom = area * cosLight / (dist * dist);
	Scalar contrib = Le * cosSurface * geom * brdf.valueNM( vToLight, ri, nm );

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

	result = contrib * (risWeight / pdfAlias);
	return result;
}
