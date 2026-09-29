//////////////////////////////////////////////////////////////////////
//
//  PolishedBRDF.h - The ONE reflectance function of `polished_material`
//    (a Fresnel dielectric coat over a diffuse substrate), shared by
//    its BSDF and its SPF.  DL-285.
//
//  THE MODEL.  At a hit, let n be the shading normal oriented to the
//  side of the surface the incoming ray arrived on (the GEOMETRIC side
//  decides -- `PolishedSPF`'s long-standing bBackface convention -- and
//  the shading normal flips with it), wi the direction toward the viewer,
//  wo the direction toward the light, ci = wi.n, co = wo.n,
//  r = reflect(-wi, n) the mirror direction, cos(alpha) = wo.r, and
//  F(mu) the unpolarized dielectric Fresnel reflectance of the coat
//  (`Optics::CalculateDielectricReflectanceCosine`, outer medium = the
//  IOR-stack top, inner = the coat `ior`):
//
//    f(wi, wo) = f_coat + f_sub                     (both zero unless
//                                                    ci > 0, co > 0 and
//                                                    wo is on the ray-
//                                                    anchored geometric
//                                                    side)
//    f_coat    = tau * min(F(ci), F(co)) * P(cos alpha) * 2 / (ci + co)
//    f_sub     = Rd * (1 - F(ci)) * (1 - F(co)) / (pi * T_avg)
//
//  P is the coat lobe's own normalized density about r (Phong
//  (N+1)/(2 pi) cos^N alpha for `scattering` N, or the Henyey-Greenstein
//  phase function truncated to the forward hemisphere about r when
//  `henyey-greenstein` is set); T_avg = 1 - F_avg = 2 int (1-F(mu)) mu dmu
//  is the coat's hemispherical transmittance.  A delta coat (N >= 1e6,
//  or g >= 1) is the mirror lobe `tau F(ci)` at r and is NOT part of f.
//
//  WHY THIS FORM (DL-127's ruling: the non-reciprocal side is the wrong
//  one).  The pre-DL-285 SPF priced the coat `tau F(ci)` and the
//  substrate `Rd (1 - F(ci))`, i.e. the implied BRDF
//  `tau F(ci) P / co + Rd (1-F(ci)) / pi`, which is NOT reciprocal (both
//  terms depend on the incident angle alone), while the BSDF was a bare
//  `Rd / pi`.  The model above is the reciprocal one closest to it:
//    * f(wi,wo) == f(wo,wi) exactly: cos(alpha) is symmetric (reflection
//      is an isometric involution), and every other factor is a
//      symmetric function of (ci, co).
//    * The substrate keeps the pre-DL-285 DIRECTIONAL albedo exactly:
//      int f_sub co dw = Rd (1 - F(ci)) for every ci (that is what the
//      1/T_avg normalization is for); only its angular distribution
//      moves (the (1-F(co)) exit transmission).
//    * The coat equals the pre-DL-285 lobe at its peak (co = ci) and is
//      ENERGY-BOUNDED: min(F(ci),F(co)) <= F(ci), and the pairing
//      wo <-> 2(wo.r)r - wo (rotation by pi about r) preserves P and
//      sends co to 2 ci cos(alpha) - co, so for the concave increasing
//      g(x) = 2x/(ci+x),  g(co) + g(co') <= 2 g(ci cos alpha) <= 2 and
//      int P g <= int P = 1.  Hence the total directional albedo is
//      <= tau F(ci) + Rd (1 - F(ci)) <= max(tau, Rd) <= 1.
//    Measured cost of reciprocity: the coat's directional albedo drops
//    where the lobe is wide or the view grazing (the (co < ci) half of
//    the lobe is down-weighted); measured in the DL-285 ledger row
//    (docs/DEBT_LEDGER.md) and docs/DL67_GUIDED_GENERATING_DENSITY.md §8.
//
//  PolishedSPF samples this same function lobe by lobe (each emitted ray
//  carries its own lobe's `f_I co / p_I`), so `IBSDF::value` and the
//  SPF's `kray` describe one function by construction.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `debt-dl285`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POLISHED_BRDF_
#define POLISHED_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"
#include "../Utilities/OrthonormalBasis3D.h"

namespace RISE
{
	namespace Implementation
	{
		//! Everything the model needs at one query, resolved once.  RGB
		//! queries fill three channels; single-wavelength (NM) queries
		//! fill one.  The coat has one COMPONENT (every channel shares one
		//! lobe shape) unless the `scattering` painter varies per channel,
		//! in which case component c carries channel c alone.
		struct PolishedLobes
		{
			bool				valid;			///< ci > 0: the model is non-zero at this query
			Vector3				n;				///< Side-oriented shading normal
			OrthonormalBasis3D	onb;			///< Frame about n (substrate sampling)
			Vector3				geomN;			///< Ray-anchored geometric normal (horizon gate)
			Vector3				wi;				///< Toward the viewer
			Vector3				rv;				///< Mirror direction
			Scalar				ci;
			int					nch;			///< 3 (RGB) or 1 (NM)
			Scalar				outer;			///< Index of the medium the ray arrived through
			Scalar				tau[3];
			Scalar				rd[3];
			Scalar				eta[3];			///< Coat index per channel
			Scalar				Fi[3];			///< F(ci) per channel
			Scalar				Tavg[3];		///< Hemispherical transmittance of the coat per channel
			bool				etaShared;		///< Every channel's coat index is identical
			int					K;				///< Coat components (1, or 3 when scattering varies per channel)
			Scalar				scat[3];		///< Phong exponent or HG asymmetry per component
			bool				delta[3];		///< Component is a mirror
			Scalar				hgMass[3];		///< Forward-hemisphere mass of a truncated HG component
			Scalar				hgC0[3];		///< HG CDF at cos(alpha) = 0
			bool				hg;
			int					nDelta;			///< Number of delta components
			int					nGlossy;		///< Number of non-delta components
			bool				emitDiffuse;	///< Some channel transmits into the substrate (F(ci) < 1)
		};

		class PolishedBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			const IPainter*			pRd;		///< Substrate reflectance (colour)
			const IScalarPainter*	pTau;		///< Coat transmittance / coverage (physical scalar)
			const IScalarPainter*	pNt;		///< Coat index of refraction (physical scalar)
			const IScalarPainter*	pScat;		///< Phong exponent or HG asymmetry (physical scalar)
			const bool				bHG;

			virtual ~PolishedBRDF();

			RISEPel EvalRGB( const Vector3& wo, const RayIntersectionGeometric& ri, Scalar outer ) const;
			Scalar  EvalNM( const Vector3& wo, const RayIntersectionGeometric& ri, Scalar nm, Scalar outer ) const;

		public:
			PolishedBRDF(
				const IPainter& Rd_,
				const IScalarPainter& tau_,
				const IScalarPainter& Nt_,
				const IScalarPainter& s,
				const bool hg
				);

			inline const IPainter&       GetDiffuseReflectance() const { return *pRd; }
			inline const IScalarPainter& GetTransmittance()      const { return *pTau; }
			inline const IScalarPainter& GetIOR()                const { return *pNt; }
			inline const IScalarPainter& GetScattering()         const { return *pScat; }
			inline bool                  GetHG()                 const { return bHG; }
			void SetDiffuseReflectance( const IPainter& v );
			void SetTransmittance( const IScalarPainter& v );
			void SetIOR( const IScalarPainter& v );
			void SetScattering( const IScalarPainter& v );

			//! Resolves the model at @a ri.  @a nm < 0 selects the RGB
			//! pipe.  @a outer is the index of the medium the incoming ray
			//! travelled through (the IOR-stack top).
			void Resolve( const RayIntersectionGeometric& ri, Scalar outer, Scalar nm, PolishedLobes& L ) const;

			// ---- the model's pieces, shared with PolishedSPF ----------

			//! Unpolarized coat Fresnel at incidence cosine mu.
			static Scalar Fresnel( Scalar mu, Scalar outer, Scalar coat );
			//! 1 - F_avg: the coat's hemispherical (cosine-weighted) transmittance.
			static Scalar HemisphericalTransmittance( Scalar outer, Scalar coat );
			//! Density of coat component k about r, at cos(alpha) (0 for cos(alpha) <= 0).
			static Scalar ComponentDensity( const PolishedLobes& L, int k, Scalar cosAlpha );
			//! cos(alpha) drawn from component k by the SAMPLER's inverse CDF at u.
			static Scalar ComponentCosAlpha( const PolishedLobes& L, int k, Scalar u );
			//! The glossy coat's sampling density at wo: the uniform mixture over glossy components.
			static Scalar GlossyCoatDensity( const PolishedLobes& L, const Vector3& wo );
			//! Is wo inside the model's support (co > 0 and on the geometric side)?
			static bool Accepted( const PolishedLobes& L, const Vector3& wo );
			//! Per-channel glossy-coat BRDF value (0 for delta channels).
			static void CoatF( const PolishedLobes& L, const Vector3& wo, Scalar out[3] );
			//! Per-channel substrate BRDF value.
			static void SubstrateF( const PolishedLobes& L, const Vector3& wo, Scalar out[3] );
			//! Per-channel kray of the glossy coat ray at wo: f_coat co / GlossyCoatDensity.
			static void CoatKray( const PolishedLobes& L, const Vector3& wo, Scalar out[3] );
			//! The glossy coat's kray at exit cosine @a co for a single-
			//! component coat (K == 1), where the lobe density cancels:
			//! tau min(F(ci), F(co)) 2 co / (ci + co).  Zero otherwise.
			static void CoatKrayAtExitCosine( const PolishedLobes& L, Scalar co, Scalar out[3] );
			//! Per-channel kray of the substrate ray at wo: f_sub co / (co / pi).
			static void SubstrateKray( const PolishedLobes& L, const Vector3& wo, Scalar out[3] );
			//! Per-channel kray of the delta coat ray: tau F(ci) on delta channels.
			static void DeltaKray( const PolishedLobes& L, Scalar out[3] );
			//! The reduction `ScatteredRayContainer::RandomlySelect` applies:
			//! max over RGB channels, the value itself for NM.
			static Scalar Reduce( const PolishedLobes& L, const Scalar k[3] );

			// ---- IBSDF -----------------------------------------------

			RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			RISEPel valueStateful( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const IORStack* pIORStack ) const;
			Scalar  valueStatefulNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* pIORStack ) const;
			RISEPel albedo( const RayIntersectionGeometric& ri ) const;
			bool hemisphericalAlbedo( const RayIntersectionGeometric& ri, RISEPel& out ) const;
			bool hemisphericalAlbedoNM( const RayIntersectionGeometric& ri, const Scalar nm, Scalar& out ) const;
		};
	}
}

#endif
