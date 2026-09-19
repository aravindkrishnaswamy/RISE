//////////////////////////////////////////////////////////////////////
//
//  TranslucentSPF.h - Defines a SPF that is partially
//  transparent (like a lampshade)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSLUCENT_SPF_
#define TRANSLUCENT_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		//! DL-68 review P3-b: `SampleClippedPhong` is TranslucentSPF.cpp's
		//! internal exact clipped-cos^N-lobe sampler (see the long
		//! derivation comment there).  Every production call site orients
		//! `axis` into the `clipN` half-space first (`OrientedLobeAxis`),
		//! so the `Dot(axis,clipN) < 0` failure path below is UNREACHABLE
		//! from `TranslucentSPF::Scatter`/`ScatterNM`.  This declaration
		//! exists solely so `TranslucentClippedPhongContractTest` (and
		//! `TranslucentEntryHorizonTest`'s sub-test 9) can drive that
		//! precondition-violation path directly -- the function itself
		//! stays defined in TranslucentSPF.cpp (already compiled into the
		//! library), so exposing it here costs nothing on the production
		//! call path.
		namespace TranslucentSPFDetail
		{
			//! Exact, unconditional two-draw sample of a `cos^N` Phong
			//! lobe about `axis`, clipped to the half-space
			//! `Dot(w,clipN) > 0`.  PRECONDITION: `Dot(axis,clipN) >= 0`
			//! (the caller must orient `axis` into the half-space first,
			//! e.g. via `OrientedLobeAxis`).  Returns false -- without
			//! writing a meaningful `outDir`/`outPdf` -- when that
			//! precondition is violated, rather than silently masking
			//! `cosPhi` to 0 (the pre-P3-b behaviour, which built the
			//! (axis,uAxis,vAxis) frame from the WRONG cosPhi and emitted
			//! a NON-UNIT `outDir`: reviewer-measured |outDir|=0.722,
			//! reported pdf=0.4502, at Dot(axis,clipN)=-0.5).
			bool SampleClippedPhong(
				const Vector3& axis, const Vector3& clipN, const Scalar N,
				const Scalar u1, const Scalar u2,
				Vector3& outDir, Scalar& outPdf );

			//! DL-112 review P1-2 (2026-09-17): these moved up out of
			//! TranslucentSPF.cpp's anonymous namespace because
			//! `TranslucentBSDF::value`/`valueNM` must renormalize the
			//! entry front-reflection lobe by the SAME valid fraction the
			//! sampler and `Pdf()` use -- a second copy would be exactly
			//! the sampler/evaluator drift DL-112 exists to close.
			//! Definitions unchanged; the derivations stay in
			//! TranslucentSPF.cpp, above `SampleValidDiffuseExit`.

			//! Orient a lobe axis into the half-space `Dot(w,halfSpace)>0`
			//! so the clipped constructions can assume `cos(phi) >= 0`
			//! (and therefore `P(valid) >= 0.5`).
			inline Vector3 OrientedLobeAxis( const Vector3& n, const Vector3& halfSpace )
			{
				return ( Vector3Ops::Dot( n, halfSpace ) >= Scalar(0) ) ? n : -n;
			}

			//! The fraction of a cosine-weighted hemisphere about `n` that
			//! survives the clip `Dot(w,geomN)>0`: Malley's-method disk
			//! projection makes it exactly `(1+cos(phi))/2`.
			inline Scalar ExitValidFraction( const Vector3& n, const Vector3& geomN )
			{
				const Scalar cosPhi = r_max( Scalar(-1), r_min( Scalar(1), Vector3Ops::Dot(n,geomN) ) );
				return (Scalar(1)+cosPhi) * Scalar(0.5);
			}

			//! Below this the valid region has effectively vanished and
			//! sampler, density and VALUE must all report "no lobe"
			//! together rather than one dividing by a near-zero fraction.
			//! Unreachable from a production call once the axis is
			//! oriented (P(valid) >= 0.5).
			const Scalar kExitVanishThreshold = Scalar(1e-4);

			//! DL-157 / DL-41 / DL-38 (2026-09-18) -- ONE FUNCTION PER SIDE.
			//!
			//! `TranslucentSPF::Scatter`/`ScatterNM` emit at most two KINDS
			//! of lobe, and which two depends on which side of the surface
			//! the walk is on:
			//!
			//!   ENTRY (`!ior_stack.containsCurrent()`)
			//!     front reflection  clipped COSINE about `OrientedLobeAxis(n, geomN)`,
			//!                       clipped to `Dot(w, geomN) > 0`, kray = ref      [DL-112]
			//!     transmission      clipped PHONG about `OrientedLobeAxis(n,-geomN)`,
			//!                       clipped to `Dot(w,-geomN) > 0`, kray = tau      [DL-68]
			//!   EXIT (`ior_stack.containsCurrent()`)
			//!     diffuse exit      clipped COSINE about `OrientedExitNormal(n, geomNRaw)`,
			//!                       clipped to `Dot(w, geomNRaw) > 0`, kray = B*(1-s) [DL-45]
			//!     backscatter       clipped PHONG about `OrientedLobeAxis(n, geomN)`,
			//!                       clipped to `Dot(w, geomN) > 0`, kray = B*s        [DL-68]
			//!
			//! with `B = exp(-ext * |ri.ray.origin - ri.ptIntersection|)`
			//! the Beer extinction over the interior segment (DL-01's
			//! "pTrans is charged once at entry; each interior segment then
			//! pays only Beer") and `s` the scattering split.
			//!
			//! `Pdf`/`PdfNM` (the MIS-partner density, which must describe
			//! what `Scatter` + `ScatteredRayContainer::RandomlySelect`
			//! actually generate) and `TranslucentBSDF::value`/`valueNM`
			//! (which must return each lobe's own `kray * pdf / |cos|`, so
			//! that a BSDF-sampled continuation and an NEE / BDPT
			//! connection estimate the same integral) are therefore two
			//! readings of ONE per-hit lobe set.  `BuildLobeSet` below is
			//! that set, rebuilt without sampling.
			//!
			//! WHAT THAT DOES AND DOES NOT GUARANTEE (review P3).  `Pdf`
			//! and `value` both go through `BuildLobeSet`, so those two
			//! cannot drift from EACH OTHER.  `Scatter`/`ScatterNM` do
			//! NOT call it -- they still build their lobes inline, because
			//! they interleave the construction with the sampler draws and
			//! with the IOR-stack push/pop, and `TranslucentSpectralParityTest`
			//! pins several of those draws bit-for-bit -- so the sampler
			//! and this set are kept in step by TESTS, not by construction:
			//! `TranslucentLobeConsistencyTest`'s gate 1 (per-lobe
			//! `kray == value*cos/pdf` over the sampler's own draws), gate
			//! 3 (a total variation against a histogram of what
			//! `Scatter` + `RandomlySelect` really returned) and gate 5
			//! (`EvaluateKrayNM` against `ScatterNM`'s own `krayNM`).
			//! A change to any lobe here must be made in BOTH places; the
			//! gates are what catch it if it is not.
			//!
			//! TWO PROPERTIES THIS MATERIAL HAS THAT MAKE THE DENSITY EXACT
			//! IN CLOSED FORM, where `SchlickSPF` / `IsotropicPhongSPF` /
			//! `AshikminShirleyAnisotropicPhongSPF` needed DL-67/DL-98/DL-99's
			//! replay quadrature:
			//!   (1) every lobe's `kray` is DIRECTION-INDEPENDENT (a painter
			//!       read, times a Beer factor that depends only on the
			//!       INCOMING segment), so `RandomlySelect`'s realized
			//!       probability really is the raw weight ratio
			//!       `MaxValue(kray_I) / sum_J MaxValue(kray_J)` -- the same
			//!       reason DL-98/DL-99 recorded both Ward SPFs as immune to
			//!       that pattern;
			//!   (2) on each side the two lobes live in COMPLEMENTARY
			//!       half-spaces (`Dot(w, geomN) > 0` against its exact
			//!       complement), so they never overlap and no direction is
			//!       priced by two of them.
			//! Neither is an accident of the current painters; both are
			//! structural, and (1) is what a future direction-dependent
			//! `kray` here would break.
			struct Lobe
			{
				bool    isPhong;        //!< false = clipped cosine, true = clipped cos^N
				Vector3 axis;           //!< ALWAYS +n or -n (every lobe here is built on the shading normal)
				Vector3 clipN;          //!< the half-space the lobe is conditioned on
				Scalar  N;              //!< Phong exponent (ignored when !isPhong)
				RISEPel kray;           //!< the RGB transport weight the sampler stamps
				Scalar  krayNM;         //!< its spectral twin
				Scalar  selectWeight;   //!< exactly what `RandomlySelect`'s CDF uses for this ray
			};

			//! At most four: the RGB per-channel-exponent branch emits one
			//! Phong ray per colour channel alongside the single cosine lobe.
			struct LobeSet
			{
				Lobe    lobes[4];
				int     count;
				Scalar  totalSelectWeight;
				Vector3 n;              //!< the shading normal the integrators take their cosine against
				LobeSet() : count(0), totalSelectWeight(0), n(0,0,1) {}
			};

			//! Rebuild -- WITHOUT drawing anything -- exactly the set of
			//! lobes `TranslucentSPF::Scatter` (`bNM == false`) /
			//! `ScatterNM` (`bNM == true`) would emit at this hit, including
			//! the same emission gates (a zero painter, a vanished valid
			//! region, a zero scattering split) and the same per-channel
			//! exponent split.
			//!
			//! `pIorStack` is the LIVE stack where the caller has one; the
			//! entry-vs-exit branch is then decided by exactly the
			//! `containsCurrent()` test `Scatter` uses, so density, value and
			//! sampler cannot land in different branches (DL-157(b)).  Where
			//! the caller has none (an AOV probe, the legacy final-gather /
			//! ambient-occlusion ops, an interactive preview), pass null and
			//! the side is inferred GEOMETRICALLY from
			//! `Dot(geomNRaw, ri.ray.Dir())` -- exact for any closed object
			//! and for a double-sided mesh (whose reported geometric normal
			//! is recovered through `UnflippedGeomNormal()` first), and the
			//! best answer available when no stack exists.
			void BuildLobeSet(
				const IPainter& refFront,
				const IPainter& trans,
				const IScalarPainter& extinction,
				const IScalarPainter& phongN,
				const IScalarPainter& scattering,
				const RayIntersectionGeometric& ri,
				const IORStack* pIorStack,
				const bool bNM,
				const Scalar nm,
				LobeSet& out );

			//! Evaluate one lobe at `w`.  Returns false when `w` is outside
			//! the lobe's support (the SAME support the sampler has: inside
			//! the axis hemisphere AND across the clip plane), in which case
			//! neither output is written.
			//!
			//! `outPdf` is the solid-angle density.  `outFOverKray` is
			//! `pdf / |cos(w, n)|`, i.e. the lobe's BRDF divided by its own
			//! `kray` -- computed with the cosine CANCELLED analytically
			//! rather than divided out, which is exact because every lobe's
			//! axis is `+n` or `-n` and the support forces
			//! `Dot(w, axis) == |cos(w, n)|`.
			bool EvalLobe( const Lobe& lobe, const Vector3& w,
				Scalar& outPdf, Scalar& outFOverKray );
		}

		class TranslucentSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			virtual ~TranslucentSPF( );

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*					pRefFront;			// Reflectance (color)
			const IPainter*					pTrans;				// Transmittance of the primary layer (color)
			const IScalarPainter*			pExtinction;		// Extinction factor (physical scalar)
			const IScalarPainter*			pN;					// Phong exponent (physical scalar)
			const IScalarPainter*			pScat;				// Multiple scattering factor (physical scalar)


		public:
			TranslucentSPF( const IPainter& rF, const IPainter& T, const IScalarPainter& ext, const IScalarPainter& N_, const IScalarPainter& scat );

			//! Read-back + rebind for the interactive editor.
			inline const IPainter&       GetRefFront()   const { return *pRefFront; }
			inline const IPainter&       GetTrans()      const { return *pTrans; }
			inline const IScalarPainter& GetExtinction() const { return *pExtinction; }
			inline const IScalarPainter& GetN()          const { return *pN; }
			inline const IScalarPainter& GetScat()       const { return *pScat; }
			void SetRefFront( const IPainter& v );
			void SetTrans( const IPainter& v );
			void SetExtinction( const IScalarPainter& v );
			void SetN( const IScalarPainter& v );
			void SetScat( const IScalarPainter& v );

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

			//! Evaluates the PDF for scattering into direction wo
			Scalar	Pdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const IORStack& ior_stack
				) const;

			//! DL-222 (opened on the concurrent `debt-dl125` branch; see the
			//! definition's own comment).  PT's and BDPT's HWSS COMPANION
			//! lanes ask an SPF what a ray it already sampled at the hero
			//! wavelength would have weighed at a companion one, and fall
			//! back to `value*cos/pdf` when the SPF declines.  That
			//! fallback is DL-125's, and for this material it was the one
			//! place a per-wavelength `kray` was reconstructed from a
			//! function rather than read off the lobe that produced it.
			//! `BuildLobeSet` makes the direct answer nearly free, so give
			//! it.
			Scalar	EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			//! Spectral version of Pdf
			Scalar	PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

			//! Diagnostic identity if an unsupported lobe declines companion
			//! evaluation. DL-157 implements the normal entry/exit lobes
			//! through EvaluateKrayNM, closing DL-222.
			const char* PerLobeDensityFallbackName() const
			{
				return "TranslucentSPF";
			}

		private:
			//! The shared body of `Pdf`/`PdfNM` (DL-41): the aggregate
			//! density of the direction `Scatter`/`ScatterNM` +
			//! `RandomlySelect` return, over every lobe this side of the
			//! surface can emit.  See the definition's own comment.
			Scalar	AggregatePdf(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const bool bNM,
				const Scalar nm,
				const IORStack& ior_stack
				) const;
		};
	}
}

#endif
