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

	// Add non-mesh lights with nonzero exitance
	const ILightManager* pLightMgr = scene.GetLights();
	if( pLightMgr )
	{
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		ILightManager::LightsList::const_iterator m, n;
		for( m=lights.begin(), n=lights.end(); m!=n; m++ )
		{
			const ILightPriv* l = *m;
			if( l->CanGeneratePhotons() )
			{
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
	}

	// Add mesh luminaries with nonzero exitance
	// Use a fixed seed point (0.5, 0.5, 0.5) to get a representative
	// surface position for distance estimates in RIS
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
// RIS light selection
//
// Draws M candidates from the global alias table (proposal q)
// and resamples one proportional to a spatially-aware target
// weight: w_i = exitance_i / max(dist_i^2, epsilon).
//
// Returns two values:
//   pdfSelect  = alias-table PDF q(j) of the selected light
//   risWeight  = RIS correction: (1/M) * sum(W_i) / W_j
//
// The caller's estimator should be:
//   result = integrand(j) / pdfSelect * risWeight
//
// This gives the unbiased 1-sample RIS estimator:
//   f(j) * (1/M) * sum(W_i) / target(j)
//
// Using the alias-table PDF for pdfSelect keeps MIS weights
// consistent with CachedPdfSelectLuminary (which also returns
// the alias-table PDF for the BSDF-hit evaluation path).
//

unsigned int LightSampler::SelectLightRIS(
	const Point3& shadingPoint,
	ISampler& sampler,
	Scalar& pdfSelect,
	Scalar& risWeight
	) const
{
	const unsigned int M = risCandidates;
	const unsigned int N = static_cast<unsigned int>( lightEntries.size() );

	// Clamp M to available lights and stack array size
	const unsigned int numCandidates = r_min( r_min( M, N ), 64u );

	if( numCandidates <= 1 )
	{
		const unsigned int idx = aliasTable.Sample( sampler.Get1D() );
		pdfSelect = static_cast<Scalar>( aliasTable.Pdf( idx ) );
		risWeight = 1.0;
		return idx;
	}

	// Draw M candidates and compute resampling weights
	unsigned int candidates[64];
	Scalar resamplingWeights[64];
	Scalar totalWeight = 0;

	const Scalar minDistSq = 1e-4;

	for( unsigned int c = 0; c < numCandidates; c++ )
	{
		const unsigned int idx = aliasTable.Sample( sampler.Get1D() );
		candidates[c] = idx;

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
		pdfSelect = static_cast<Scalar>( aliasTable.Pdf( candidates[0] ) );
		risWeight = 1.0;
		return candidates[0];
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

	// Alias-table PDF for MIS consistency
	pdfSelect = static_cast<Scalar>( aliasTable.Pdf( selected ) );

	// RIS correction factor: risWeight = (1/M) * sum(W_i) / W_j
	//
	// The caller computes: f(j) / q(j) * risWeight
	//   = f(j) / q(j) * (1/M) * sum(W_i) / (target_j/q_j)
	//   = f(j) * (1/M) * sum(W_i) / target_j
	// which is the correct unbiased 1-sample RIS estimator.
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

Scalar LightSampler::CachedPdfSelectLuminary(
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

//
// Unified direct lighting evaluation
//
// The estimator for a selected area light is:
//
//   result = Le * cos_surface * cos_light * (area / dist^2) * brdf * w / pdfSelect
//
// where w is the MIS weight using the combined PDF:
//
//   p_light = pdfSelect * dist^2 / (area * cos_light)   [solid angle]
//   p_bsdf  = material->Pdf(direction)                  [solid angle]
//   w       = p_light^2 / (p_light^2 + p_bsdf^2)
//
// For delta (point/spot) lights there is no alternative sampling
// strategy, so w = 1.  The contribution is the light's own
// ComputeDirectLighting result divided by pdfSelect.
//
// When RIS is enabled (risCandidates > 0), pdfSelect is computed
// by SelectLightRIS which draws M candidates from the alias table
// and resamples one proportional to exitance/dist^2.
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
	// Step 2: Select one light with nonzero exitance.
	// Uses RIS when enabled, otherwise plain alias table.
	// ----------------------------------------------------------------
	if( !aliasTable.IsValid() )
	{
		return result;
	}

	unsigned int idx;
	Scalar pdfSelect;
	Scalar risWeight = 1.0;

	if( risCandidates > 0 )
	{
		idx = SelectLightRIS( ri.ptIntersection, sampler, pdfSelect, risWeight );
	}
	else
	{
		idx = aliasTable.Sample( sampler.Get1D() );
		pdfSelect = static_cast<Scalar>( aliasTable.Pdf( idx ) );
	}

	const LightEntry& entry = lightEntries[idx];

	if( entry.pLight )
	{
		// Selected a non-mesh (delta) light.
		// Delegate to the light's own direct-lighting method
		// which handles position, shadow rays, and BRDF evaluation.
		// Divide by selection probability, multiply by RIS correction.
		RISEPel amount( 0, 0, 0 );
		entry.pLight->ComputeDirectLighting( ri, caster, brdf,
			pShadingObject ? pShadingObject->DoesReceiveShadows() : true,
			amount );
		result = result + amount * (risWeight / pdfSelect);
	}
	else
	{
		// Selected a mesh luminary
		const LuminaryManager::LUM_ELEM& lumEntry = (*pPreparedLuminaries)[entry.lumIndex];
		const IEmitter* pEmitter = lumEntry.pLum->GetMaterial()->GetEmitter();

		// Skip self-illumination
		if( lumEntry.pLum == pShadingObject )
		{
			return result;
		}

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

				// Unshadowed contribution (area-measure integrand converted
				// to solid angle via the Jacobian area*cosLight/dist^2)
				const Scalar geom = area * cosLight / (dist * dist);
				RISEPel contrib = Le * cosSurface * geom * brdf.value( vToLight, ri );

				// MIS weight using combined selection + solid-angle PDF
				// pdfSelect is the alias-table PDF (consistent with
				// CachedPdfSelectLuminary used on the BSDF-hit side)
				if( pMaterial && area > 0 && cosLight > 0 )
				{
					const Scalar p_light = pdfSelect * (dist * dist) / (area * cosLight);
					const Scalar p_bsdf = pMaterial->Pdf( vToLight, ri, 0 );

					if( p_bsdf > 0 )
					{
						const Scalar w = PowerHeuristic( p_light, p_bsdf );
						contrib = contrib * w;
					}
					// else p_bsdf=0 → w=1.0 (no change needed)
				}

				// Divide by pdfSelect and multiply by RIS correction
				result = result + contrib * (risWeight / pdfSelect);
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

	// ----------------------------------------------------------------
	// Spectral NEE: uses RIS or alias table for selection.
	// Non-mesh lights do not yet have spectral ComputeDirectLighting
	// variants — matching existing PathTracingShaderOp behavior.
	// ----------------------------------------------------------------
	if( !aliasTable.IsValid() )
	{
		return result;
	}

	unsigned int idx;
	Scalar pdfSelect;
	Scalar risWeight = 1.0;

	if( risCandidates > 0 )
	{
		idx = SelectLightRIS( ri.ptIntersection, sampler, pdfSelect, risWeight );
	}
	else
	{
		idx = aliasTable.Sample( sampler.Get1D() );
		pdfSelect = static_cast<Scalar>( aliasTable.Pdf( idx ) );
	}

	const LightEntry& entry = lightEntries[idx];

	if( entry.pLight )
	{
		// Selected a non-mesh light — no spectral evaluation
		// available, return zero (energy is not lost; it is
		// recovered by the BSDF-sampled continuation path).
		return result;
	}

	// Selected a mesh luminary
	const LuminaryManager::LUM_ELEM& lumEntry = (*pPreparedLuminaries)[entry.lumIndex];
	const IEmitter* pEmitter = lumEntry.pLum->GetMaterial()->GetEmitter();

	// Skip self-illumination
	if( lumEntry.pLum == pShadingObject )
	{
		return result;
	}

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

	if( cosLight <= 0 || cosSurface <= 0 )
	{
		return result;
	}

	// Shadow test
	if( pShadingObject && pShadingObject->DoesReceiveShadows() )
	{
		const Ray rayToLight( ri.ptIntersection, vToLight );
		if( caster.CastShadowRay( rayToLight, dist - 0.001 ) )
		{
			return result;
		}
	}

	// Emitted radiance at sampled point (spectral)
	RayIntersectionGeometric lumri( Ray( ptOnLum, -vToLight ), nullRasterizerState );
	lumri.vNormal = lumNormal;
	lumri.ptCoord = lumCoord;
	lumri.onb.CreateFromW( lumNormal );

	const Scalar Le = pEmitter->emittedRadianceNM( lumri, -vToLight, lumNormal, nm );

	// Unshadowed contribution
	const Scalar geom = area * cosLight / (dist * dist);
	Scalar contrib = Le * cosSurface * geom * brdf.valueNM( vToLight, ri, nm );

	// MIS weight using combined selection + solid-angle PDF
	if( pMaterial && area > 0 && cosLight > 0 )
	{
		const Scalar p_light = pdfSelect * (dist * dist) / (area * cosLight);
		const Scalar p_bsdf = pMaterial->PdfNM( vToLight, ri, nm, 0 );

		if( p_bsdf > 0 )
		{
			const Scalar w = PowerHeuristic( p_light, p_bsdf );
			contrib = contrib * w;
		}
	}

	result = contrib * (risWeight / pdfSelect);
	return result;
}
