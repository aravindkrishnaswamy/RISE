//////////////////////////////////////////////////////////////////////
//
//  DielectricSPF.h - Defines a dielectric SPF, which is
//  is basically like glass.
//
//  All three parameters (tau, ior, scattering) are PHYSICAL SCALARS
//  carried by `IScalarPainter`, NOT colors carried by `IPainter`.
//  See docs/ISCALARPAINTER_REFACTOR.md — the old IPainter routing
//  forced inline-numeric scattering values through the JH spectral
//  uplift, producing wrong values per wavelength and rendering glass
//  spheres as speckled invisible blobs in every spectral rasterizer.
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
#include "../Interfaces/IScalarPainter.h"
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

			//! Pointer storage so the interactive editor can rebind
			//! via Set*.  See LambertianBRDF for pattern.
			const IScalarPainter*		pTau;			// Transmittance (per-channel + spectral)
			const IScalarPainter*		pRIndex;		// Index of refraction (spectral for dispersion)
			const IScalarPainter*		pScat;			// Scattering function (Phong cone width or HG asymmetry)
			const bool					bHG;			// Use Henyey-Greenstein phase function scattering

			// Optional anti-reflective thin-film COATING (data-based).  When
			// arThickness > 0 the bare air<->medium Fresnel reflectance is
			// replaced by the single-film Airy reflectance of an (ambient /
			// AR-film / substrate) stack via ThinFilm::ReflectanceConductor:
			// e.g. a MgF2 (n~1.38) quarter-wave (~100 nm) coating drops a
			// sapphire crystal's 7.7%/surface glare to ~0.5% with the
			// characteristic purple bloom.  Default 0 => NO coating =>
			// bit-identical bare-Fresnel dielectric (back-compat).
			const Scalar				arN;			// AR film real index (0 = none)
			const Scalar				arK;			// AR film extinction (~0)
			const Scalar				arThickness;	// AR film thickness nm (0 = none)

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
				const Scalar nm,
				const IORStack& ior_stack								///< [in/out] Index of refraction stack
				) const;

			void DoSingleRGBComponent(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const Point2& random,										///< [in] Two canonical random numbers
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack,							///< [in/out] Index of refraction stack
				const int oneofthree,
				const Scalar newIOR,
				const Scalar scattering,
				const Scalar cosine,
				const Scalar nm
				) const;

		public:
			DielectricSPF(
				const IScalarPainter& tau_,
				const IScalarPainter& ri,
				const IScalarPainter& s,
				const bool hg,
				Scalar arN_ = 0, Scalar arK_ = 0, Scalar arThickness_ = 0
				);

			//! Read-back + rebind for the interactive editor.
			inline const IScalarPainter& GetTransmittance() const { return *pTau; }
			inline const IScalarPainter& GetIOR()           const { return *pRIndex; }
			inline const IScalarPainter& GetScattering()    const { return *pScat; }
			//! Read-back of the baked construction-time scalars (HG-phase
			//! flag + anodization/AR-film parameters).  No setters — these
			//! are fixed at construction.  Used by the snapshot clone to
			//! faithfully reconstruct the material.
			inline bool   GetHG()                 const { return bHG; }
			inline Scalar GetAnodizationN()       const { return arN; }
			inline Scalar GetAnodizationK()       const { return arK; }
			inline Scalar GetAnodizationThickness() const { return arThickness; }
			void SetTransmittance( const IScalarPainter& v );
			void SetIOR( const IScalarPainter& v );
			void SetScattering( const IScalarPainter& v );

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const
			{
				SpecularInfo info;
				info.isSpecular = true;
				info.canRefract = true;
				info.ior = pRIndex->GetValuesAt( ri ).v[0];
				const ScalarTriple t = pTau->GetValuesAt( ri );
				info.attenuation = RISEPel( t.v[0], t.v[1], t.v[2] );
				info.valid = true;
				info.clearTransmission = true;
				return info;
			}

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack,
				const Scalar nm
				) const
			{
				SpecularInfo info;
				info.isSpecular = true;
				info.canRefract = true;
				info.ior = pRIndex->GetValueAtNM( ri, nm );
				info.valid = true;
				info.clearTransmission = true;
				return info;
			}

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

			//! Returns the PDF for sampling the given outgoing direction (always 0 for delta distributions)
			Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo, const IORStack& ior_stack ) const;

			//! Returns the spectral PDF for sampling the given outgoing direction (always 0 for delta distributions)
			Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, const Scalar nm, const IORStack& ior_stack ) const;
		};
	}
}

#endif
