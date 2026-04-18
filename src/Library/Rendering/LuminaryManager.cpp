//////////////////////////////////////////////////////////////////////
//
//  LuminaryManager.cpp - Implements the luminary manager
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LuminaryManager.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/ProbabilityDensityFunction.h"

using namespace RISE;
using namespace RISE::Implementation;

LuminaryManager::LuminaryManager(
	const bool randomly_select
	) : 
  pLumSampling( 0 ),
  dOVcSamples( 1 ),
  bRandomlySelect( randomly_select )
{
}

LuminaryManager::~LuminaryManager()
{
	safe_release( pLumSampling );

	LuminariesList::const_iterator	i, e;
	for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ ) {
		i->pLum->release();
	}

	luminaries.clear();
}

namespace RISE {
	namespace Implementation {
		class ObjectEnumerationDispatch : public IEnumCallback<IObject>
		{
		protected:
			ILuminaryManager&	pBackObj;

		public:
			ObjectEnumerationDispatch( ILuminaryManager& pBackObj_ ) : 
			pBackObj( pBackObj_ )
			{
				pBackObj.addref();
			}

			virtual ~ObjectEnumerationDispatch()
			{
				pBackObj.release();
			}

			bool operator()( const IObject& p )
			{
				pBackObj.AddToLuminaryList( p );
				return true;
			}
		};
	}
}

void LuminaryManager::AttachScene( const IScene* pScene )
{
	if( pScene ) {
		// Create the list of luminaries
		luminaries.clear();

		const IObjectManager* pObjMan = pScene->GetObjects();
		ObjectEnumerationDispatch dispatch( *this );
		pObjMan->EnumerateObjects( dispatch );
	} else {
		GlobalLog()->PrintSourceError( "LuminaryManager::AttachScene:: No scene specified", __FILE__, __LINE__ );
	}
}

void LuminaryManager::AddToLuminaryList( const IObject& pObject )
{
	const IMaterial*	pMaterial = pObject.GetMaterial();
	if( pMaterial && pMaterial->GetEmitter() )
	{
		LUM_ELEM elem;
		elem.pLum = &pObject;
		pObject.addref();
		luminaries.push_back( elem );
	}
}

RISEPel LuminaryManager::ComputeDirectLightingForLuminary(
			const RayIntersectionGeometric& ri,
			const IObject& pObject,
			const Point2& ptLum,
			const IBSDF& pBRDF,
			const IMaterial* pMaterial,
			ISampler& sampler,
			const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
			const bool bShadowRays
			) const
{
	// To compute the direct lighting, get a sample point on the luminary
	// and use that to compute the amount of direct lighting
	const Point3	ptRand( ptLum.x, ptLum.y, sampler.Get1D() );
	Point3	ptLocationOnLuminary;
	Vector3 lumNormal;
	Point2	lumCoord;
	pObject.UniformRandomPoint( &ptLocationOnLuminary, &lumNormal, &lumCoord, ptRand );

	RayIntersectionGeometric lumri = ri;
	lumri.ptIntersection = ptLocationOnLuminary;
	lumri.vNormal = lumNormal;
	lumri.ptCoord = lumCoord;
	lumri.onb.CreateFromW( lumNormal );

	Vector3	vToLight = Vector3Ops::mkVector3( ptLocationOnLuminary, ri.ptIntersection );
	const Scalar		fDistFromLight = Vector3Ops::NormalizeMag(vToLight);
	const Scalar		fDot = Vector3Ops::Dot( vToLight, ri.vNormal );

	const Vector3		vFromLight = -vToLight;
	const Scalar		fDotLight = Vector3Ops::Dot( vFromLight, lumNormal );

	if( fDotLight < 0 ) {
		return RISEPel( 0, 0, 0 );
	}

	if( fDot < 0 ) {
		return RISEPel( 0, 0, 0 );
	}

	// Check to see if there is a shadow
	if( pObject.DoesReceiveShadows() && bShadowRays ) {
		const Ray		rayToLight( ri.ptIntersection, vToLight );
		if( caster.CastShadowRay( rayToLight, fDistFromLight-0.001 ) ) {
			return RISEPel(0,0,0);
		}
	}

	const Scalar	area = pObject.GetArea();
	const Scalar	attenuation_size_factor = area / (fDistFromLight * fDistFromLight);
	RISEPel contrib = pObject.GetMaterial()->GetEmitter()->emittedRadiance( lumri, vFromLight, lumNormal ) * fDot * fDotLight * attenuation_size_factor * pBRDF.value(vToLight,ri);

	// MIS weight (power heuristic, exponent = 2)
	// This weights the NEE contribution against what the BSDF sampling
	// strategy would have produced.  The complementary weight is applied
	// in the emission ShaderOp when a BSDF-sampled ray hits an emitter.
	//
	// p_light: probability of sampling this point on the luminary (area measure → solid angle)
	// p_bsdf: probability of the BSDF sampling this direction
	if( pMaterial && area > 0 && fDotLight > 0 )
	{
		const Scalar p_light = (fDistFromLight * fDistFromLight) / (area * fDotLight);
		static const IORStack defaultIOR( 1.0 );
		const Scalar p_bsdf = pMaterial->Pdf( vToLight, ri, defaultIOR );

		if( p_bsdf > 0 )
		{
			const Scalar w_light = (p_light * p_light) / (p_light * p_light + p_bsdf * p_bsdf);
			contrib = contrib * w_light;
		}
		// else p_bsdf=0 → w_light=1.0 (no change needed)
	}

	return contrib;
}

RISEPel LuminaryManager::ComputeDirectLighting(
			const RayIntersection& ri,										///< [in] Intersection information at point we computing lighting for
			const IBSDF& pBRDF,												///< [in] BRDF of the material
			ISampler& sampler,												///< [in] Low-discrepancy sampler
			const IRayCaster& caster,
			const IShadowPhotonMap* pShadowMap								///< [in] Shadow photon map for speeding up shadow checks
			) const
{
	// This computes the lighting that comes directly from a luminary incident on the
	RISEPel	pelRet;
	RandomNumberGenerator rng;  // For ISampling2D::GenerateSamplePoints (interface not yet converted)

	if( bRandomlySelect )
	{
		// Just randomly pick a luminaire for each sample!
		if( pLumSampling )
		{
			ISampling2D::SamplesList2D samples;
			pLumSampling->GenerateSamplePoints(rng,samples);

			ISampling2D::SamplesList2D::const_iterator m, n;
			for( m=samples.begin(), n=samples.end(); m!=n; m++ ) {
				const unsigned int lum = (unsigned int)floor(sampler.Get1D() * luminaries.size());
				pelRet = pelRet + ComputeDirectLightingForLuminary( ri.geometric, *luminaries[lum].pLum, (*m), pBRDF, ri.pMaterial, sampler, caster, true );
			}
		}
		else
		{
			const unsigned int lum = (unsigned int)floor(sampler.Get1D() * luminaries.size());
			const Point2	ptRand = sampler.Get2D();
			pelRet = ComputeDirectLightingForLuminary( ri.geometric, *luminaries[lum].pLum, ptRand, pBRDF, ri.pMaterial, sampler, caster, true );
		}
	}
	else
	{
		LuminariesList::const_iterator	i, e;
		for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ )
		{
			// Make sure a luminary isn't diffuse reflecting itself!
			const LUM_ELEM& elem = *i;
			if( elem.pLum != ri.pObject )
			{
				bool bShadowRays = true;

				if( pShadowMap ) {
					char shadow = 2;
					pShadowMap->ShadowEstimate( shadow, ri.geometric.ptIntersection );

					switch( shadow )
					{
					case 0:
						// No chance for a shadow
	//					pelRet = RISEPel(1,1,1);
	//					continue;
						bShadowRays = false;
						break;
					case 1:
						// No chance for light
						continue;
					case 2:
	//					pelRet = RISEPel(1,0.0,0.0);
	//					continue;
						// Have to do detailed shadow checks
						bShadowRays = true;
						break;
					};
				}

				if( pLumSampling )
				{
					ISampling2D::SamplesList2D samples;
					pLumSampling->GenerateSamplePoints(rng,samples);

					ISampling2D::SamplesList2D::const_iterator m, n;
					RISEPel pelThisLum;
					for( m=samples.begin(), n=samples.end(); m!=n; m++ ) {
						pelThisLum = pelThisLum + ComputeDirectLightingForLuminary( ri.geometric, *elem.pLum, (*m), pBRDF, ri.pMaterial, sampler, caster, bShadowRays );
					}
					pelRet = pelRet + (pelThisLum * dOVcSamples);
				}
				else
				{
					const Point2 ptRand = sampler.Get2D();
					pelRet = pelRet + ComputeDirectLightingForLuminary( ri.geometric, *elem.pLum, ptRand, pBRDF, ri.pMaterial, sampler, caster, bShadowRays );
				}
			}
		}
	}

	return pelRet;
}

Scalar LuminaryManager::ComputeDirectLightingForLuminaryNM(
			const RayIntersectionGeometric& ri,
			const IObject& pObject,
			const Point2& ptLum,
			const IBSDF& pBRDF,
			const IMaterial* pMaterial,
			const Scalar nm,
			ISampler& sampler,
			const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
			const bool bShadowRays
			) const
{
	const Point3	ptRand( ptLum.x, ptLum.y, sampler.Get1D() );
	Point3	ptLocationOnLuminary;
	Vector3 lumNormal;
	Point2	lumCoord;
	pObject.UniformRandomPoint( &ptLocationOnLuminary, &lumNormal, &lumCoord, ptRand );

	RayIntersectionGeometric lumri = ri;
	lumri.ptIntersection = ptLocationOnLuminary;
	lumri.vNormal = lumNormal;
	lumri.ptCoord = lumCoord;
	lumri.onb.CreateFromW( lumNormal );

	Vector3	vToLight = Vector3Ops::mkVector3( ptLocationOnLuminary, ri.ptIntersection );
	const Scalar		fDistFromLight = Vector3Ops::NormalizeMag(vToLight);
	const Scalar		fDot = Vector3Ops::Dot( vToLight, ri.vNormal );

	const Vector3		vFromLight = -vToLight;
	const Scalar		fDotLight = Vector3Ops::Dot( vFromLight, lumNormal );

	if( fDotLight < 0 ) {
		return 0;
	}

	if( fDot < 0 ) {
		return 0;
	}

	if( pObject.DoesReceiveShadows() && bShadowRays ) {
		const Ray		rayToLight( ri.ptIntersection, vToLight );
		if( caster.CastShadowRay( rayToLight, fDistFromLight-0.001 ) ) {
			return 0;
		}
	}

	const Scalar	area = pObject.GetArea();
	const Scalar	attenuation_size_factor = area / (fDistFromLight * fDistFromLight);
	Scalar contrib = pObject.GetMaterial()->GetEmitter()->emittedRadianceNM( lumri, vFromLight, lumNormal, nm ) * fDot * fDotLight * attenuation_size_factor * pBRDF.valueNM(vToLight,ri,nm);

	// MIS weight (power heuristic)
	if( pMaterial && area > 0 && fDotLight > 0 )
	{
		const Scalar p_light = (fDistFromLight * fDistFromLight) / (area * fDotLight);
		static const IORStack defaultIOR( 1.0 );
		const Scalar p_bsdf = pMaterial->PdfNM( vToLight, ri, nm, defaultIOR );

		if( p_bsdf > 0 )
		{
			const Scalar w_light = (p_light * p_light) / (p_light * p_light + p_bsdf * p_bsdf);
			contrib = contrib * w_light;
		}
	}

	return contrib;
}

Scalar LuminaryManager::ComputeDirectLightingNM(
			const RayIntersection& ri,										///< [in] Intersection information at point we computing lighting for
			const IBSDF& pBRDF,												///< [in] BRDF of the material
			const Scalar nm,												///< [in] Wavelength
			ISampler& sampler,												///< [in] Low-discrepancy sampler
			const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
			const IShadowPhotonMap* pShadowMap								///< [in] Shadow photon map for speeding up shadow checks
			) const
{
	// This computes the lighting that comes directly from a luminary incident on the
	Scalar	ret = 0;
	RandomNumberGenerator rng;  // For ISampling2D::GenerateSamplePoints (interface not yet converted)

	if( bRandomlySelect )
	{
		// Just randomly pick a luminaire for each sample
		if( pLumSampling )
		{
			ISampling2D::SamplesList2D samples;
			pLumSampling->GenerateSamplePoints(rng,samples);

			ISampling2D::SamplesList2D::const_iterator m, n;
			for( m=samples.begin(), n=samples.end(); m!=n; m++ ) {
				const unsigned int lum = (unsigned int)floor(sampler.Get1D() * luminaries.size());
				ret = ret + ComputeDirectLightingForLuminaryNM( ri.geometric, *luminaries[lum].pLum, (*m), pBRDF, ri.pMaterial, nm, sampler, caster, true );
			}
		} else {
			const unsigned int lum = (unsigned int)floor(sampler.Get1D() * luminaries.size());
			const Point2 ptRand = sampler.Get2D();
			ret = ComputeDirectLightingForLuminaryNM( ri.geometric, *luminaries[lum].pLum, ptRand, pBRDF, ri.pMaterial, nm, sampler, caster, true );
		}
	}
	else
	{
		LuminariesList::const_iterator	i, e;
		for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ )
		{
			// Make sure a luminary isn't diffuse reflecting itself!
			const LUM_ELEM& elem = *i;
			if( elem.pLum != ri.pObject )
			{
				bool bShadowRays = true;

				if( pShadowMap ) {
					char shadow = 2;
					pShadowMap->ShadowEstimate( shadow, ri.geometric.ptIntersection );

					switch( shadow )
					{
					case 0:
						// No chance for a shadow
						bShadowRays = false;
						break;
					case 1:
						// No chance for light
						continue;
					case 2:
						// Have to do detailed shadow checks
						bShadowRays = true;
						break;
					};
				}

				if( pLumSampling )
				{
					ISampling2D::SamplesList2D samples;
					pLumSampling->GenerateSamplePoints(rng,samples);

					ISampling2D::SamplesList2D::const_iterator m, n;
					Scalar thisLum = 0;
					for( m=samples.begin(), n=samples.end(); m!=n; m++ ) {
						thisLum = thisLum + ComputeDirectLightingForLuminaryNM( ri.geometric, *elem.pLum, (*m), pBRDF, ri.pMaterial, nm, sampler, caster, bShadowRays );
					}
					ret = (thisLum*dOVcSamples);
				} else {
					const Point2 ptRand = sampler.Get2D();
					ret = ret + ComputeDirectLightingForLuminaryNM( ri.geometric, *elem.pLum, ptRand, pBRDF, ri.pMaterial, nm, sampler, caster, bShadowRays );
				}
			}
		}
	}

	return ret;
}

void LuminaryManager::SetLuminaireSampling( ISampling2D* pLumSam )
{
	if( pLumSam )
	{
		// Free the old sampler if it exists
		safe_release( pLumSampling );

		pLumSampling = pLumSam;
		pLumSampling->addref();

		dOVcSamples = 1.0 / Scalar(pLumSampling->GetNumSamples());
	}
}

