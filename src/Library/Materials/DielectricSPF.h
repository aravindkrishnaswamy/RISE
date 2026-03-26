//////////////////////////////////////////////////////////////////////
//
//  DielectricSPF.h - Defines a dielectric SPF, which is
//  is basically like glass
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DIELECTRIC_SPF_
#define DIELECTRIC_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class DielectricSPF :
			public virtual ISPF,
			public virtual Reference
		{
		protected:
			virtual ~DielectricSPF( );

			const IPainter&				tau;			// Transmittance
			const IPainter&				rIndex;			// Index of refraction
			const IPainter&				scat;			// Scattering function (either Phong or HG)
			const bool					bHG;			// Use Henyey-Greenstein phase function scattering

			Scalar GenerateScatteredRay(
				ScatteredRay& dielectric,									///< [out] Scattered dielectric ray
				ScatteredRay& fresnel,										///< [out] Scattered fresnel or reflected ray
				bool& bDielectric,											///< [out] Dielectric ray exists?
				bool& bFresnel,												///< [out] Fresnel ray exists?
				const bool bFromInside,
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const Point2& random,										///< [in] Two canonical random numbers
				const Scalar scatfunc,
				const Scalar rIndex,
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			void DoSingleRGBComponent(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const Point2& random,										///< [in] Two canonical random numbers
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack,							///< [in/out] Index of refraction stack
				const int oneofthree,
				const Scalar newIOR,
				const Scalar scattering,
				const Scalar cosine
				) const;

		public:
			DielectricSPF(
				const IPainter& tau_,
				const IPainter& ri,
				const IPainter& s,
				const bool hg
				);

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack* ior_stack
				) const
			{
				SpecularInfo info;
				info.isSpecular = true;
				info.canRefract = true;
				info.ior = rIndex.GetColor( ri )[0];
				info.valid = true;
				return info;
			}

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric& ri,
				const IORStack* ior_stack,
				const Scalar nm
				) const
			{
				SpecularInfo info;
				info.isSpecular = true;
				info.canRefract = true;
				info.ior = rIndex.GetColorNM( ri, nm );
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
