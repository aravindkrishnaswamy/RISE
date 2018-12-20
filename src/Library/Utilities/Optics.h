//////////////////////////////////////////////////////////////////////
//
//  Optics.h - Defines a bunch of useful functions for dealing with 
//  geometric optics
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 11, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OPTICS_
#define OPTICS_

#include "Math3D/Math3D.h"

namespace RISE
{
	struct Optics
	{
		//! Calculates a reflected ray using Snell's law
		/// \return The reflected ray
		static Vector3 CalculateReflectedRay( 
			const Vector3& vIn,					///< [in] Incoming vector
			const Vector3& vNormal				///< [in] Normal at point of reflection
			);

		//! Calculates simple geometric refraction using Snell's law
		/// \return TRUE if there was refraction and thus a ray was generated, FALSE otherwise, indicating total internal reflection
		static bool CalculateRefractedRay( 
			const Vector3& vNormal,		///< [in] Normal at point of refraction
			const Scalar Ni,			///< [in] Index of refraction of outside (IOR refracting from)
			const Scalar Nt,			///< [in] Index of refraction of inside (IOR refracting to)
			Vector3& vIn				///< [out] The refracted vector
			);

		//! Computes fresnel dielectric reflectance
		/// \return Dielectric refractance value
		static Scalar CalculateDielectricReflectance( 
			const Vector3& v,			///< [in] Incoming vector
			const Vector3& tv,			///< [in] Outgoing vector
			const Vector3& n,			///< [in] Normal at point of reflectance
			const Scalar Ni,			///< [in] Index of refraction of outside (IOR refracting from)
			const Scalar Nt				///< [in] Index of refraction of inside (IOR refracting to)
			);

		//! Computes fresnel conductor reflectance
		/// \return Conductor reflectance value
		template< class T >
		static T CalculateConductorReflectance( 
			const Vector3& v,			///< [in] Incoming vector
			const Vector3& n,			///< [in] Normal at point of reflectance
			const T& Ni,				///< [in] Index of refraction of outside (IOR refracting from)
			const T& Nt,				///< [in] Index of refraction of inside (IOR refracting to)
			const T& kt					///< [in] Extinction
			);

		//! Computes dielectric reflectance using Shlick's method
		/// \return Dielectric refractance value
		template< class T >
		static inline T CalculateFresnelReflectanceSchlick( 
			const T& r,					///< [in] Specularity
			const Scalar cost			///< [in] Cosine of the Angle between half-way vector and outgoing vector
			)
		{
			return (r + (1.0-r)*pow((1.0-cost),5.0));
		}
	};
}

#include "Optics.hpp"

#endif
