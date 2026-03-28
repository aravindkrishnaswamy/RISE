//////////////////////////////////////////////////////////////////////
//
//  PolishedSPF.h - A polished SPF is a diffuse substrate
//  with a thin dielectric covering
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POLISHED_SPF_
#define POLISHED_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PolishedSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			const IPainter&				Rd;					// Reflectance of diffuse substrate
			const IPainter&				tau;				// Transmittance of the dielectric
			const IPainter&				Nt;					// Index of refraction of dielectric coating
			const IPainter&				scat;				// Scattering function (either Phong or HG)
			const bool					bHG;				// Use Henyey-Greenstein phase function scattering

			virtual ~PolishedSPF( );

			Scalar GenerateScatteredRayFromPolish(
				ScatteredRay& dielectric,
				const Vector3 normal,										///< [in] Normal
				const Vector3 reflected,									///< [in] Reflected ray
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const Point2& random,										///< [in] Random numbers
				const Scalar phongN,
				const Scalar ior,
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

		public:
			PolishedSPF(
				const IPainter& Rd_,
				const IPainter& tau_,
				const IPainter& Nt_,
				const IPainter& s,
				const bool hg
				);

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack* ior_stack
				) const
			{
				SpecularInfo info;
				const Scalar s = scat.GetColor( ri )[0];
				info.isSpecular = bHG ? (s >= 1.0) : (s >= 1000000.0);
				info.canRefract = true;
				info.ior = Nt.GetColor( ri )[0];
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
				const Scalar s = scat.GetColorNM( ri, nm );
				info.isSpecular = bHG ? (s >= 1.0) : (s >= 1000000.0);
				info.canRefract = true;
				info.ior = Nt.GetColorNM( ri, nm );
				info.valid = true;
				return info;
			}

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.
			void	Scatter(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into
			//! account spectral affects.
			void	ScatterNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			Scalar	Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack* const ior_stack
				) const;

			Scalar	PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack* const ior_stack
				) const;
		};
	}
}

#endif
