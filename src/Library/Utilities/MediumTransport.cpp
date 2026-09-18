//////////////////////////////////////////////////////////////////////
//
//  MediumTransport.cpp - Implementation of medium transport utilities
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MediumTransport.h"
#include "../Lights/LightSampler.h"
#include "../Intersection/RayIntersectionGeometric.h"

using namespace RISE;
using namespace RISE::MediumTransport;


//
// MediumScatterBSDF
//

MediumScatterBSDF::MediumScatterBSDF(
	const IPhaseFunction* pPhase,
	const Vector3& wo
	) :
m_pPhase( pPhase ),
m_wo( wo )
{
}

RISEPel MediumScatterBSDF::value(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri
	) const
{
	// Phase function is isotropic w.r.t. the surface normal —
	// it depends only on the angle between incoming and outgoing
	// directions.  No cosine-weighted hemisphere clamping.
	const Scalar p = m_pPhase->Evaluate( vLightIn, m_wo );
	return RISEPel( p, p, p );
}

Scalar MediumScatterBSDF::valueNM(
	const Vector3& vLightIn,
	const RayIntersectionGeometric& ri,
	const Scalar nm
	) const
{
	return m_pPhase->Evaluate( vLightIn, m_wo );
}


//
// MediumScatterMaterial
//

MediumScatterMaterial::MediumScatterMaterial(
	const IPhaseFunction* pPhase,
	const Vector3& wo
	) :
m_pPhase( pPhase ),
m_wo( wo )
{
}

Scalar MediumScatterMaterial::Pdf(
	const Vector3& vToLight,
	const RayIntersectionGeometric& ri,
	const IORStack& ior_stack
	) const
{
	// DL-73 (ruled not a debt, see docs/DL72_RAYCASTER_BSDFTIMESCOS_TRAINING.md
	// "DL-73 RULED NOT A DEBT"): this is the RAW, un-guided phase pdf --
	// deliberately NOT combined with any OpenPGL volume-guiding mixture.
	// RayCaster.cpp's volume phase-scatter continuation records this SAME
	// raw value as its MIS-PARTNER density -- `rs2.bsdfMisPdf = phasePdf`
	// since DL-74 split the two roles apart (`rs2.bsdfPdf` now carries the
	// guided mixture the direction was really drawn from, which the
	// optimal-MIS moment estimator needs).  So the two sides of the
	// env-NEE/env-escape MIS pair, and of the area-NEE/emitter-hit pair,
	// feed PowerHeuristic the identical (phasePdf, p_light) pair and sum to
	// exactly 1.  Combining this with the guide pdf would break that
	// partition -- do not do it.
	return m_pPhase->Pdf( vToLight, m_wo );
}

Scalar MediumScatterMaterial::PdfNM(
	const Vector3& vToLight,
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	return m_pPhase->Pdf( vToLight, m_wo );
}


//
// In-scattering evaluation
//

RISEPel MediumTransport::EvaluateInScattering(
	const Point3& scatterPoint,
	const Vector3& wo,
	const IMedium* pMedium,
	const IRayCaster& caster,
	const Implementation::LightSampler* pLightSampler,
	ISampler& sampler,
	const RasterizerState& rast,
	const IObject* pMediumObject,
	Scalar neeTrainingScale
	)
{
	if( !pMedium || !pLightSampler )
	{
		return RISEPel( 0, 0, 0 );
	}

	const IPhaseFunction* pPhase = pMedium->GetPhaseFunction();
	if( !pPhase )
	{
		return RISEPel( 0, 0, 0 );
	}

	// Create adapter BSDF and material for the phase function
	MediumScatterBSDF scatterBSDF( pPhase, wo );
	MediumScatterMaterial scatterMaterial( pPhase, wo );

	// Construct a synthetic intersection at the scatter point.
	// The "normal" is set to the outgoing direction — this ensures
	// that cosine terms in the light sampler (dot(vToLight, normal))
	// produce the correct geometric factor for isotropic scattering.
	// For phase functions this cosine factor is already accounted
	// for in the phase function evaluation, so we set the normal
	// to the outgoing direction and rely on the phase function value.
	RayIntersectionGeometric scatterRI( Ray( scatterPoint, wo ), rast );
	scatterRI.bHit = true;
	scatterRI.ptIntersection = scatterPoint;
	scatterRI.vNormal = wo;
	scatterRI.onb.CreateFromW( wo );

	// DL-185: `neeTrainingScale` forwards CastRay's LOCAL cast-level RR
	// compensation for THIS call (default 1 for every other caller) --
	// scales the optimal-MIS training integrand only, never the returned
	// in-scattered radiance itself.
	return pLightSampler->EvaluateDirectLighting(
		scatterRI, scatterBSDF, &scatterMaterial,
		caster, sampler, 0, pMedium, true, pMediumObject,
		0, 0, neeTrainingScale );
}

Scalar MediumTransport::EvaluateInScatteringNM(
	const Point3& scatterPoint,
	const Vector3& wo,
	const IMedium* pMedium,
	const Scalar nm,
	const IRayCaster& caster,
	const Implementation::LightSampler* pLightSampler,
	ISampler& sampler,
	const RasterizerState& rast,
	const IObject* pMediumObject,
	Scalar neeTrainingScale
	)
{
	if( !pMedium || !pLightSampler )
	{
		return 0;
	}

	const IPhaseFunction* pPhase = pMedium->GetPhaseFunction();
	if( !pPhase )
	{
		return 0;
	}

	MediumScatterBSDF scatterBSDF( pPhase, wo );
	MediumScatterMaterial scatterMaterial( pPhase, wo );

	RayIntersectionGeometric scatterRI( Ray( scatterPoint, wo ), rast );
	scatterRI.bHit = true;
	scatterRI.ptIntersection = scatterPoint;
	scatterRI.vNormal = wo;
	scatterRI.onb.CreateFromW( wo );

	// DL-185 -- see the RGB twin above.
	return pLightSampler->EvaluateDirectLightingNM(
		scatterRI, scatterBSDF, &scatterMaterial,
		nm, caster, sampler, 0, pMedium, true, pMediumObject,
		0, 0, neeTrainingScale );
}
