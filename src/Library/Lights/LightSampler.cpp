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
  cachedTotalExitance( 0 )
{
}

LightSampler::~LightSampler()
{
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
					lightEntries.push_back( entry );
				}
			}
		}
	}

	// Add mesh luminaries with nonzero exitance
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

	// O(1) selection proportional to exitance
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
	// Step 2: O(1) stochastic selection of one light with nonzero
	// exitance using the alias table
	// ----------------------------------------------------------------
	if( !aliasTable.IsValid() )
	{
		return result;
	}

	const unsigned int idx = aliasTable.Sample( sampler.Get1D() );
	const LightEntry& entry = lightEntries[idx];
	const Scalar pdfSelect = static_cast<Scalar>( aliasTable.Pdf( idx ) );

	if( entry.pLight )
	{
		// Selected a non-mesh (delta) light.
		// Delegate to the light's own direct-lighting method
		// which handles position, shadow rays, and BRDF evaluation.
		// Divide by selection probability.
		RISEPel amount( 0, 0, 0 );
		entry.pLight->ComputeDirectLighting( ri, caster, brdf,
			pShadingObject ? pShadingObject->DoesReceiveShadows() : true,
			amount );
		result = result + amount * (1.0 / pdfSelect);
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

				// Divide by pdfSelect to correct for random light selection
				result = result + contrib * (1.0 / pdfSelect);
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
	// Spectral NEE: uses alias table for O(1) selection.
	// Non-mesh lights do not yet have spectral ComputeDirectLighting
	// variants — matching existing PathTracingShaderOp behavior.
	// ----------------------------------------------------------------
	if( !aliasTable.IsValid() )
	{
		return result;
	}

	const unsigned int idx = aliasTable.Sample( sampler.Get1D() );
	const LightEntry& entry = lightEntries[idx];
	const Scalar pdfSelect = static_cast<Scalar>( aliasTable.Pdf( idx ) );

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

	result = contrib * (1.0 / pdfSelect);
	return result;
}
