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

			//! Spectral version of Pdf
			Scalar	PdfNM(
				const RayIntersectionGeometric& ri,
				const Vector3& wo,
				const Scalar nm,
				const IORStack& ior_stack
				) const;

			//! DL-222.  This SPF emits TWO lobes per call, each carrying its
			//! OWN conditional density, and does not implement
			//! `ISPF::EvaluateKrayNM` -- so the HWSS companion ladder's
			//! aggregate-BSDF fallback is not exact for it (its `kray`
			//! carries Beer extinction `TranslucentBSDF` omits, and
			//! `Pdf`/`PdfNM` cover neither Phong `cos^N` lobe, DL-41).
			//! Naming ourselves here makes that residual AUDIBLE -- one log
			//! line per process -- instead of silent.  See DL-222 for the
			//! partial-closure recipe.
			const char* PerLobeDensityFallbackName() const
			{
				return "TranslucentSPF";
			}
		};
	}
}

#endif
