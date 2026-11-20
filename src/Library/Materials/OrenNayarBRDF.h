//////////////////////////////////////////////////////////////////////
//
//  OrenNayarBRDF.h - Defines the Oren-Nayar BRDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef ORENNAYAR_BRDF_
#define ORENNAYAR_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class OrenNayarBRDF :
			public virtual IBSDF,
			public virtual Reference
		{
		protected:
			virtual ~OrenNayarBRDF();

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pReflectance;
			const IScalarPainter*	pRoughness;

			//! false: the classic Oren-Nayar model (ComputeFactor below; the
			//! default, so existing scenes do not move).  true: the
			//! energy-preserving EON model (EONValue below).  Fixed at
			//! construction, like GGX's fresnel mode.
			const bool				bEnergyPreserving;

		public:
			OrenNayarBRDF(
				const IPainter& reflectance,
				const IScalarPainter& roughness,
				const bool energy_preserving = false
				);

			template< class T >
			static void ComputeFactor( T& L1, T& L2, const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& roughness );

			//! The energy-preserving Oren-Nayar model, EON (Portsmouth, Kutz
			//! and Hill, "EON: A practical energy-preserving rough diffuse
			//! BRDF", 2025; the OpenPBR diffuse, and Cycles' Diffuse BSDF as in
			//! Blender 5.3; 4.3 through at least the 5.2 alpha of 2026-03-17
			//! carry one more factor of the colour in the multiple-scattering
			//! term, `sqr(albedo)`; the 5.2 release was not checked).  It is Fujii's improved Oren-Nayar single-scatter
			//! term plus a multiple-scattering term that restores the energy
			//! the single-scatter term loses:
			//!
			//!   f = (rho/pi) (A + B s/t)
			//!     + (rho_ms/pi) (1 - E(mu_i)) (1 - E(mu_o)) / (1 - Eavg)
			//!
			//!   A = 1/(1 + (1/2 - 2/(3 pi)) sigma),  B = sigma A,
			//!   s = wi.wo - mu_i mu_o,  t = max(mu_i, mu_o) if s > 0 else 1,
			//!   E(mu) = A + (B/pi) G(mu)   (the single-scatter directional albedo),
			//!   Eavg  = A + (2/3 - 28/(15 pi)) B,
			//!   rho_ms = rho^2 Eavg / (1 - rho (1 - Eavg))   for rho in [0, 1].
			//!
			//! sigma is the roughness clamped to [0, 1] (NOT the classic model's
			//! slope angle in radians).  Above 1 the term follows Cycles,
			//! rho sat(rho) Eavg / (1 - sat(rho) (1 - Eavg)) with sat the clamp to
			//! [0, 1], so an over-bright colour scales linearly.  At rho = 1 the
			//! directional albedo is exactly 1 at every angle and roughness (the
			//! two terms' albedos are E(mu) and 1 - E(mu)).  Reciprocal.
			//! Returns the BRDF value (the 1/pi included); zero below either
			//! horizon, with the same geometric-horizon gate as ComputeFactor.
			template< class T >
			static T EONValue( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& rho, const T& roughness );

			//! EON for one channel, from the cosines and s = wi.wo - mu_i mu_o.
			static Scalar EONChannel( const Scalar rho, const Scalar roughness, const Scalar muI, const Scalar muO, const Scalar s );

			//! EON single-scatter directional albedo E(mu) = A + (B/pi) G(mu).
			static Scalar EONSingleScatterAlbedo( const Scalar roughness, const Scalar mu );

			//! EON bihemispherical (white-sky) albedo, closed form:
			//! rho Eavg + rho_ms (1 - Eavg).
			static Scalar EONBihemisphericalAlbedo( const Scalar rho, const Scalar roughness );

			inline bool IsEnergyPreserving() const { return bEnergyPreserving; }

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;
			virtual bool hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const;
			virtual bool hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const;

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetReflectance() const { return *pReflectance; }
			inline const IScalarPainter& GetRoughness()   const { return *pRoughness; }
			void SetReflectance( const IPainter& v );
			void SetRoughness( const IScalarPainter& v );
		};
	}
}

#endif

