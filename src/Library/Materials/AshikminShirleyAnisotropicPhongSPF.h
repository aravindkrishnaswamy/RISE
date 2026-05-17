//////////////////////////////////////////////////////////////////////
//
//  AshikminShirleyAnisotropicPhongSPF.h - Defines the SPF component
//  of the Shirley / Ashikhmin Anisotropic Phong BRDF model
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ASHIKMINSHIRLEY_ANISOTROPIC_PHONG_SPF_
#define ASHIKMINSHIRLEY_ANISOTROPIC_PHONG_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AshikminShirleyAnisotropicPhongSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			virtual ~AshikminShirleyAnisotropicPhongSPF( );

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IScalarPainter*	pNu;			// Phong exponent (physical scalar)
			const IScalarPainter*	pNv;

			const IPainter*			pRd;			// diffuse reflectance
			const IPainter*			pRs;			// specular reflectance

		public:
			AshikminShirleyAnisotropicPhongSPF( const IScalarPainter& Nu_, const IScalarPainter& Nv_, const IPainter& Rd_, const IPainter& Rs_ );

			//! Read-back + rebind for the interactive editor.
			inline const IScalarPainter& GetNu() const { return *pNu; }
			inline const IScalarPainter& GetNv() const { return *pNv; }
			inline const IPainter&       GetRd() const { return *pRd; }
			inline const IPainter&       GetRs() const { return *pRs; }
			void SetNu( const IScalarPainter& v );
			void SetNv( const IScalarPainter& v );
			void SetRd( const IPainter& v );
			void SetRs( const IPainter& v );

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.
			void	Scatter(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into
			//! account spectral affects.
			void	ScatterNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const;

			Scalar	Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const;

			Scalar	PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;
		};
	}
}

#endif

