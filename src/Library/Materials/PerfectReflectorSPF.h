//////////////////////////////////////////////////////////////////////
//
//  PerfectReflectorSPF.h - A SPF which
//  reflects the incoming vector perfectly...
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  This is NOT a PHYSICALLY BASED material!
//  Its really only for test purposes
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PERFECT_REFLECTOR_SPF_
#define PERFECT_REFLECTOR_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PerfectReflectorSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			const IPainter&		reflectivity;

			virtual ~PerfectReflectorSPF();

		public:
			PerfectReflectorSPF( const IPainter& R_ );

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack* ior_stack
				) const
			{
				SpecularInfo info;
				info.isSpecular = true;
				info.canRefract = false;
				info.ior = 1.0;
				info.valid = true;
				return info;
			}

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.
			void	Scatter(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into
			//! account spectral affects.
			void	ScatterNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Returns the PDF for sampling the given outgoing direction (always 0 for delta distributions)
			Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo, const IORStack* const ior_stack ) const;

			//! Returns the spectral PDF for sampling the given outgoing direction (always 0 for delta distributions)
			Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, const Scalar nm, const IORStack* const ior_stack ) const;
		};
	}
}

#endif
