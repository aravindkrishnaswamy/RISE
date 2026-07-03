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

			// Optional anti-reflective thin-film COATING (data-based),
			// generalized to an N-LAYER stack.  When arStack.nLayers > 0 the
			// bare air<->medium Fresnel reflectance is replaced by the Airy
			// (1 layer) / characteristic-matrix TMM (N layers) reflectance of
			// an (ambient / film-1 / ... / film-N / substrate) stack via
			// ThinFilm::ReflectanceConductorStack.  A single MgF2 (n~1.38)
			// quarter-wave (~100 nm) layer drops a sapphire crystal's
			// 7.7%/surface glare to ~0.5% but leaves the characteristic single-
			// layer PURPLE bloom; a multi-layer broadband stack reflects far
			// fainter AND colour-neutral (as real premium AR coatings do).
			// nLayers == 0 => NO coating => bit-identical bare-Fresnel
			// dielectric (back-compat).  Layers are authored in AMBIENT ->
			// SUBSTRATE order (outermost, air-side, first).
		public:
			//! Max stack-allocated AR layers; must match ThinFilm::kMaxFilms
			//! (a static_assert in DielectricSPF.cpp enforces it).  Public so
			//! the .cpp's file-local stack helper and the static_assert can see
			//! the cap and the POD layer type.
			static const int			kMaxARLayers = 8;
			struct ARStack
			{
				int		nLayers;					//!< 0 = no coating
				Scalar	n[kMaxARLayers];			//!< real index, ambient->substrate
				Scalar	k[kMaxARLayers];			//!< extinction (>=0)
				Scalar	t[kMaxARLayers];			//!< thickness, nm
			};
		protected:
			const ARStack				arStack;

			//! Copies up to kMaxARLayers (n,k,t) triples into an ARStack.
			//! Null arrays or nLayers<=0 yield an empty (no-coating) stack.
			static ARStack BuildARStack( const Scalar* n, const Scalar* k,
			                             const Scalar* t, int nLayers );

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
				const Scalar* arN_ = 0, const Scalar* arK_ = 0, const Scalar* arThickness_ = 0,
				int arNLayers_ = 0
				);

			//! Read-back + rebind for the interactive editor.
			inline const IScalarPainter& GetTransmittance() const { return *pTau; }
			inline const IScalarPainter& GetIOR()           const { return *pRIndex; }
			inline const IScalarPainter& GetScattering()    const { return *pScat; }
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
