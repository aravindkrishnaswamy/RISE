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
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include "PolishedBRDF.h"

namespace RISE
{
	namespace Implementation
	{
		class PolishedSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			//! The ONE reflectance function (DL-285).  The SPF samples it
			//! lobe by lobe and owns no parameters of its own: every
			//! painter lives in the BRDF, so `PolishedMaterial::GetBSDF()`
			//! (which returns this same object) and this sampler cannot
			//! drift apart.
			PolishedBRDF*				pBRDF;

			virtual ~PolishedSPF( );

			void ScatterImpl(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			Scalar PdfImpl(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

		public:
			PolishedSPF(
				const IPainter& Rd_,
				const IScalarPainter& tau_,
				const IScalarPainter& Nt_,
				const IScalarPainter& s,
				const bool hg
				);

			//! The shared reflectance function (PolishedMaterial::GetBSDF()).
			inline PolishedBRDF&         GetBRDF()               const { return *pBRDF; }

			//! Read-back + rebind for the interactive editor (forwarded to
			//! the shared BRDF, so BSDF and SPF stay one function).
			inline const IPainter&       GetDiffuseReflectance() const { return pBRDF->GetDiffuseReflectance(); }
			inline const IScalarPainter& GetTransmittance()      const { return pBRDF->GetTransmittance(); }
			inline const IScalarPainter& GetIOR()                const { return pBRDF->GetIOR(); }
			inline const IScalarPainter& GetScattering()         const { return pBRDF->GetScattering(); }
			//! Read-back of the baked HG-phase flag (no setter — it is
			//! fixed at construction).  Used by the snapshot clone to
			//! faithfully reconstruct the material.
			inline bool                  GetHG()                 const { return pBRDF->GetHG(); }
			inline void SetDiffuseReflectance( const IPainter& v )       { pBRDF->SetDiffuseReflectance( v ); }
			inline void SetTransmittance( const IScalarPainter& v )      { pBRDF->SetTransmittance( v ); }
			inline void SetIOR( const IScalarPainter& v )                { pBRDF->SetIOR( v ); }
			inline void SetScattering( const IScalarPainter& v )         { pBRDF->SetScattering( v ); }

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const
			{
				SpecularInfo info;
				const Scalar s = pBRDF->GetScattering().GetValuesAt( ri ).v[0];
				info.isSpecular = pBRDF->GetHG() ? (s >= 1.0) : (s >= 1000000.0);
				info.canRefract = true;
				info.ior = pBRDF->GetIOR().GetValuesAt( ri ).v[0];
				info.valid = true;
				return info;
			}

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack,
				const Scalar nm
				) const
			{
				SpecularInfo info;
				const Scalar s = pBRDF->GetScattering().GetValueAtNM( ri, nm );
				info.isSpecular = pBRDF->GetHG() ? (s >= 1.0) : (s >= 1000000.0);
				info.canRefract = true;
				info.ior = pBRDF->GetIOR().GetValueAtNM( ri, nm );
				info.valid = true;
				return info;
			}

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.
			void	Scatter(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into
			//! account spectral affects.
			void	ScatterNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,									///< [in] Sampler
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack								///< [in] Index of refraction stack
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

			//! DL-216. The SELECTED lobe's own spectral BSDF value f_I(wo; nm)
			//! in [1/sr], without cosine or density division.
			Scalar EvaluateLobeFNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			/// HWSS companion evaluation: the selected lobe's own krayNM
			/// at @a nm -- `f_I cos / p_I` for the glossy coat and the
			/// substrate (both direction-dependent since DL-285), and
			/// `tau F(ci)` for a delta coat.
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			//! DL-216. Unbiased companion weight evaluated with the HERO
			//! wavelength's sampling density: f_I(nm) * cos_o / pdfHero.
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack,
				Scalar pdfHero
				) const;
		};
	}
}

#endif
