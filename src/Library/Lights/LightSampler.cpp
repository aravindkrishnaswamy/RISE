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
	cachedTotalExitance = ComputeTotalExitance( scene, luminaries );
}

Scalar LightSampler::ComputeTotalExitance(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries
	) const
{
	Scalar total = 0;

	// Accumulate exitance from non-mesh lights
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
				total += ColorMath::MaxValue( l->radiantExitance() );
			}
		}
	}

	// Accumulate exitance from mesh luminaries
	LuminaryManager::LuminariesList::const_iterator i, e;
	for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ )
	{
		const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
		if( pEmitter )
		{
			const Scalar area = i->pLum->GetArea();
			const RISEPel power = pEmitter->averageRadiantExitance() * area;
			total += ColorMath::MaxValue( power );
		}
	}

	return total;
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

	// Compute total exitance across all light sources
	const Scalar total_exitance = ComputeTotalExitance( scene, luminaries );

	if( total_exitance <= 0 )
	{
		return false;
	}

	// Select a light proportional to its exitance
	const Scalar xi = sampler.Get1D() * total_exitance;
	Scalar cumulative = 0;

	// Try non-mesh lights first
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
				cumulative += exitance;

				if( cumulative >= xi )
				{
					// Selected this non-mesh light
					sample.pLight = l;
					sample.pdfSelect = exitance / total_exitance;
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

					return true;
				}
			}
		}
	}

	// Try mesh luminaries
	LuminaryManager::LuminariesList::const_iterator i, e;
	for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ )
	{
		const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
		if( !pEmitter )
		{
			continue;
		}

		const Scalar area = i->pLum->GetArea();
		const RISEPel power = pEmitter->averageRadiantExitance() * area;
		const Scalar exitance = ColorMath::MaxValue( power );
		cumulative += exitance;

		if( cumulative >= xi )
		{
			// Selected this mesh luminary
			sample.pLuminary = i->pLum;
			sample.pdfSelect = exitance / total_exitance;
			sample.isDelta = false;

			// Sample a uniform random point on the luminary surface
			const Point3 prand(
				sampler.Get1D(),
				sampler.Get1D(),
				sampler.Get1D()
				);
			Point2 coord;
			i->pLum->UniformRandomPoint(
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

			return true;
		}
	}

	// Should not reach here if total_exitance > 0, but handle edge case
	return false;
}

Scalar LightSampler::PdfSelectLight(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const ILight& light
	) const
{
	const Scalar exitance = ColorMath::MaxValue( light.radiantExitance() );
	if( exitance <= 0 )
	{
		return 0;
	}

	const Scalar total_exitance = ComputeTotalExitance( scene, luminaries );
	if( total_exitance <= 0 )
	{
		return 0;
	}

	return exitance / total_exitance;
}

Scalar LightSampler::CachedPdfSelectLuminary(
	const IObject& luminary
	) const
{
	if( cachedTotalExitance <= 0 )
	{
		return 0;
	}

	const IMaterial* pMat = luminary.GetMaterial();
	if( !pMat )
	{
		return 0;
	}

	const IEmitter* pEmitter = pMat->GetEmitter();
	if( !pEmitter )
	{
		return 0;
	}

	const Scalar area = luminary.GetArea();
	const Scalar exitance = ColorMath::MaxValue( pEmitter->averageRadiantExitance() * area );
	if( exitance <= 0 )
	{
		return 0;
	}

	return exitance / cachedTotalExitance;
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
	// Step 2: Stochastic selection of one light with nonzero exitance
	// ----------------------------------------------------------------
	if( cachedTotalExitance <= 0 )
	{
		return result;
	}

	const Scalar xi = sampler.Get1D() * cachedTotalExitance;
	Scalar cumulative = 0;

	// Try non-mesh lights first
	if( pLightMgr )
	{
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		ILightManager::LightsList::const_iterator m, n;
		for( m=lights.begin(), n=lights.end(); m!=n; m++ )
		{
			const ILightPriv* l = *m;
			if( !l->CanGeneratePhotons() )
			{
				continue;
			}

			const Scalar exitance = ColorMath::MaxValue( l->radiantExitance() );
			if( exitance <= 0 )
			{
				continue;
			}

			cumulative += exitance;

			if( cumulative >= xi )
			{
				// Selected this non-mesh (delta) light.
				// Delegate to the light's own direct-lighting method
				// which handles position, shadow rays, attenuation,
				// and BRDF evaluation.  Divide by selection probability.
				const Scalar pdfSelect = exitance / cachedTotalExitance;
				RISEPel amount( 0, 0, 0 );
				l->ComputeDirectLighting( ri, caster, brdf,
					pShadingObject ? pShadingObject->DoesReceiveShadows() : true,
					amount );
				result = result + amount * (1.0 / pdfSelect);
				return result;
			}
		}
	}

	// Try mesh luminaries
	if( pPreparedLuminaries )
	{
		LuminaryManager::LuminariesList::const_iterator i, e;
		for( i=pPreparedLuminaries->begin(), e=pPreparedLuminaries->end(); i!=e; i++ )
		{
			const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
			if( !pEmitter )
			{
				continue;
			}

			const Scalar area = i->pLum->GetArea();
			const RISEPel power = pEmitter->averageRadiantExitance() * area;
			const Scalar exitance = ColorMath::MaxValue( power );
			cumulative += exitance;

			if( cumulative >= xi )
			{
				// Selected this mesh luminary
				const Scalar pdfSelect = exitance / cachedTotalExitance;

				// Skip self-illumination
				if( i->pLum == pShadingObject )
				{
					return result;
				}

				// Sample a uniform random point on the luminary surface
				const Point3 ptRand( sampler.Get1D(), sampler.Get1D(), sampler.Get1D() );
				Point3 ptOnLum;
				Vector3 lumNormal;
				Point2 lumCoord;
				i->pLum->UniformRandomPoint( &ptOnLum, &lumNormal, &lumCoord, ptRand );

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
				return result;
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
	// Spectral NEE: mesh luminaries only (non-mesh lights do not yet
	// have spectral ComputeDirectLighting variants — matching existing
	// PathTracingShaderOp behavior).
	// ----------------------------------------------------------------
	if( cachedTotalExitance <= 0 )
	{
		return result;
	}

	const Scalar xi = sampler.Get1D() * cachedTotalExitance;
	Scalar cumulative = 0;

	// Skip non-mesh lights in cumulative scan (they participate in
	// selection weight but spectral evaluation is not yet available)
	const ILightManager* pLightMgr = pPreparedScene->GetLights();
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
				cumulative += exitance;

				if( cumulative >= xi )
				{
					// Selected a non-mesh light — no spectral evaluation
					// available, return zero (energy is not lost; it is
					// recovered by the BSDF-sampled continuation path).
					return result;
				}
			}
		}
	}

	// Try mesh luminaries
	LuminaryManager::LuminariesList::const_iterator i, e;
	for( i=pPreparedLuminaries->begin(), e=pPreparedLuminaries->end(); i!=e; i++ )
	{
		const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
		if( !pEmitter )
		{
			continue;
		}

		const Scalar area = i->pLum->GetArea();
		const RISEPel power = pEmitter->averageRadiantExitance() * area;
		const Scalar exitance = ColorMath::MaxValue( power );
		cumulative += exitance;

		if( cumulative >= xi )
		{
			const Scalar pdfSelect = exitance / cachedTotalExitance;

			// Skip self-illumination
			if( i->pLum == pShadingObject )
			{
				return result;
			}

			// Sample a uniform random point on the luminary surface
			const Point3 ptRand( sampler.Get1D(), sampler.Get1D(), sampler.Get1D() );
			Point3 ptOnLum;
			Vector3 lumNormal;
			Point2 lumCoord;
			i->pLum->UniformRandomPoint( &ptOnLum, &lumNormal, &lumCoord, ptRand );

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
	}

	return result;
}
Scalar LightSampler::PdfSelectLuminary(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const IObject& luminary
	) const
{
	const IMaterial* pMat = luminary.GetMaterial();
	if( !pMat )
	{
		return 0;
	}

	const IEmitter* pEmitter = pMat->GetEmitter();
	if( !pEmitter )
	{
		return 0;
	}

	const Scalar area = luminary.GetArea();
	const Scalar exitance = ColorMath::MaxValue( pEmitter->averageRadiantExitance() * area );
	if( exitance <= 0 )
	{
		return 0;
	}

	const Scalar total_exitance = ComputeTotalExitance( scene, luminaries );
	if( total_exitance <= 0 )
	{
		return 0;
	}

	return exitance / total_exitance;
}
