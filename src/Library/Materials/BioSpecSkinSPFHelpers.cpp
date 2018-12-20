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
#include "../Utilities/Optics.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

//! Scatters according to a Trowbridge-Reitz PDF
Vector3 BioSpecSkinSPF::TrowbridgeReitz_Scattering(
	const RandomNumberGenerator& random,		///< [in] Random number generator
	const Vector3& incoming,							///< [in] The direction of the incoming ray
	const Scalar aspect_ratio							///< [in] Aspect ratio of the cells
	)
{
	const Scalar a = aspect_ratio*aspect_ratio;
	const Scalar b = a*a;
	const Scalar c = 1/(a-1);
	const Scalar d = random.CanonicalRandom();

	const Scalar alpha = acos(sqrt(((a/sqrt(b-b*d+d))-1)*c));		// polar angle perturbation
	const Scalar beta = random.CanonicalRandom() * TWO_PI;			// azimuthal angle perturbation
	
	return Vector3Ops::Normalize(GeometricUtilities::Perturb( incoming, alpha, beta ));
}

//! Scatters accoding to a lookup which is described in the function
Vector3 BioSpecSkinSPF::Scattering_From_TableLookup( 
	const Scalar nm,										///< [in] The wavelength to do the lookup for
	const RandomNumberGenerator& random,			///< [in] Random number generator
	const Vector3& incoming,								///< [in] The direction of the incoming ray
	const IFunction2D& pFunc								///< [in] Function to use for doing the scattering
	)
{
	// Get the angle back from the function and perturb
	const Scalar alpha = pFunc.Evaluate( nm, random.CanonicalRandom() * 100.0 ) * DEG_TO_RAD;
	const Scalar beta = random.CanonicalRandom() * TWO_PI;

	return Vector3Ops::Normalize(GeometricUtilities::Perturb( incoming, alpha, beta ));
}

//! Scatters according to the Henyey-Greenstein phase function
Vector3 BioSpecSkinSPF::Scattering_From_HenyeyGreenstein(
	const RandomNumberGenerator& random,			///< [in] Random number generator
	const Vector3& incoming,								///< [in] The direction of the incoming ray
	const Scalar g											///< [in] The asymmetry factor
	)
{
	const Scalar inner = (1.0 - g*g) / (1 - g + 2*g*random.CanonicalRandom());
	const Scalar hny_phase = acos( (1/(2.0*g)) * (1 + g*g - inner*inner) );

	// Use the warping function to perturb the reflected ray using HG phase function
	return Vector3Ops::Normalize(GeometricUtilities::Perturb(
		incoming,
		hny_phase,
		TWO_PI * random.CanonicalRandom() ));
}

//! Scatters a ray at the dermis (both papillary and reticular layer)
Vector3 BioSpecSkinSPF::Dermis_Scattering( 
	const RandomNumberGenerator& random,			///< [in] Random number generator
	const Vector3& incoming								///< [in] The direction of the incoming ray
	)
{
	// Dermal scattering is merely diffuse
	const Scalar alpha = acos( sqrt(random.CanonicalRandom()) );
	const Scalar beta = random.CanonicalRandom() * TWO_PI;

	return GeometricUtilities::Perturb( incoming, alpha, beta );
}

//! Scatters a ray by using the Rayleigh phase function
Vector3 BioSpecSkinSPF::Rayleigh_Phase_Function_Scattering( 
	const RandomNumberGenerator& random,			///< [in] Random number generator
	const Vector3& incoming								///< [in] The direction of the incoming ray
	)
{
	// Use rejection sampling to compute the phase function
	Scalar alpha = 0;
	Scalar ran = 0;
	do {
		alpha = random.CanonicalRandom() * PI;
		ran = random.CanonicalRandom() * 1.5;
	} while (ran > 0.75 * (1.0+cos(alpha)*cos(alpha)));

	const Scalar beta = random.CanonicalRandom() * TWO_PI;

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


