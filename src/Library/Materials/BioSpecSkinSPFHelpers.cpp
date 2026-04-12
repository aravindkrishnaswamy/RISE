//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinSPF.cpp - Implementation of the helper functions
//    in the BioSpecSkinSPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BioSpecSkinSPF.h"
#include "HenyeyGreensteinPhaseFunction.h"
#include "../Utilities/Optics.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

//! Scatters according to a Trowbridge-Reitz PDF
Vector3 BioSpecSkinSPF::TrowbridgeReitz_Scattering(
	ISampler& sampler,		///< [in] Sampler
	const Vector3& incoming,							///< [in] The direction of the incoming ray
	const Scalar aspect_ratio							///< [in] Aspect ratio of the cells
	)
{
	const Scalar a = aspect_ratio*aspect_ratio;
	const Scalar d = sampler.Get1D();

	const Scalar alpha = acos(sqrt((1.0 - d) / (d * (a - 1.0) + 1.0)));		// polar angle perturbation
	const Scalar beta = sampler.Get1D() * TWO_PI;			// azimuthal angle perturbation
	
	return Vector3Ops::Normalize(GeometricUtilities::Perturb( incoming, alpha, beta ));
}

//! Scatters accoding to a lookup which is described in the function
Vector3 BioSpecSkinSPF::Scattering_From_TableLookup( 
	const Scalar nm,										///< [in] The wavelength to do the lookup for
	ISampler& sampler,			///< [in] Sampler
	const Vector3& incoming,								///< [in] The direction of the incoming ray
	const IFunction2D& pFunc								///< [in] Function to use for doing the scattering
	)
{
	// Get the angle back from the function and perturb
	const Scalar alpha = pFunc.Evaluate( nm, sampler.Get1D() * 100.0 ) * DEG_TO_RAD;
	const Scalar beta = sampler.Get1D() * TWO_PI;

	return Vector3Ops::Normalize(GeometricUtilities::Perturb( incoming, alpha, beta ));
}

//! Scatters according to the Henyey-Greenstein phase function
Vector3 BioSpecSkinSPF::Scattering_From_HenyeyGreenstein(
	ISampler& sampler,			///< [in] Sampler
	const Vector3& incoming,								///< [in] The direction of the incoming ray
	const Scalar g											///< [in] The asymmetry factor
	)
{
	// Delegate to the canonical HG phase function implementation
	return HenyeyGreensteinPhaseFunction::SampleWithG( incoming, sampler, g );
}

//! Scatters a ray at the dermis (both papillary and reticular layer)
Vector3 BioSpecSkinSPF::Dermis_Scattering( 
	ISampler& sampler,			///< [in] Sampler
	const Vector3& incoming								///< [in] The direction of the incoming ray
	)
{
	// Dermal scattering uses cosine-weighted distribution around the layer normal (BioSpec paper Eq. 6)
	const Scalar alpha = acos( sqrt(sampler.Get1D()) );
	const Scalar beta = sampler.Get1D() * TWO_PI;

	return GeometricUtilities::Perturb( incoming, alpha, beta );
}

//! Scatters a ray by using the Rayleigh phase function
Vector3 BioSpecSkinSPF::Rayleigh_Phase_Function_Scattering( 
	ISampler& sampler,			///< [in] Sampler
	const Vector3& incoming								///< [in] The direction of the incoming ray
	)
{
	// Use rejection sampling to compute the phase function
	// We must sample a uniform direction on the sphere (uniform cos_alpha)
	// and then reject based on the Rayleigh phase function probability.
	Scalar cos_alpha = 0;
	Scalar ran = 0;
	do {
		cos_alpha = 1.0 - 2.0 * sampler.Get1D();
		ran = sampler.Get1D() * 1.5;
	} while (ran > 0.75 * (1.0 + cos_alpha*cos_alpha));

	const Scalar alpha = acos(cos_alpha);
	const Scalar beta = sampler.Get1D() * TWO_PI;

	return GeometricUtilities::Perturb( incoming, alpha, beta );
}

//! Refraction between two layers of skin (at their boundaries)
/// \return The absolute reflectance
Scalar BioSpecSkinSPF::Boundary_Refraction (
	const Vector3& incoming,								///< [in] Direction of incoming ray
	Vector3& outgoing,										///< [out] Direction of outgoing ray
	const Vector3& n,										///< [in] The normal
	const Scalar ior_from,									///< [in] Index of refraction of where coming from
	const Scalar ior_to										///< [in] Index of refraction of where going to
	)
{
	// IOR backwards ?
	const bool		bFromInside = ((-Vector3Ops::Dot(n, incoming)) < NEARZERO);

	outgoing = incoming;

	if( bFromInside )
	{
		if( Optics::CalculateRefractedRay( -n, ior_to, ior_from, outgoing ) ) {
			return Optics::CalculateDielectricReflectance( incoming, outgoing, -n, ior_to, ior_from );
		} else {
			return 1.0;
		}
	}
	else
	{
		if( Optics::CalculateRefractedRay( n, ior_from, ior_to, outgoing ) ) {
			return Optics::CalculateDielectricReflectance( incoming, outgoing, n, ior_from, ior_to );
		} else {
			return 1.0;
		}
	}
}


