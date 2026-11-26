//////////////////////////////////////////////////////////////////////
//
//  DataDrivenSPF.h - The SPF of datadriven_material (DL-325).  It
//    samples the SAME tabulated BRDF DataDrivenBSDF evaluates, so the
//    sampled function and the evaluated BSDF are one function:
//    kray = f(wi,wo) cos(wo) / pdf with pdf = cos(wo)/pi.
//
//    The table is sampled by cosine-weighted importance sampling over
//    the front hemisphere of the shading normal.  That is the only
//    support the tabulated function has: DataDrivenBSDF::value is
//    REFLECTION-ONLY (it returns 0 whenever the view or the light lies
//    behind the shading normal; the file's BTDF patches are parsed but
//    never evaluated), so there is no transmission lobe to sample and
//    the SPF emits at most ONE non-delta ray per Scatter().  A single
//    emitted ray means the aggregate density Pdf() and the density the
//    emitted ray carries are the same function, which is what DL-69's
//    selected-lobe pairing, DL-103's escape partner and the BDPT/VCM
//    MIS recurrence all require.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `debt-dl325`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DATADRIVEN_SPF_H
#define DATADRIVEN_SPF_H

#include "../Interfaces/ISPF.h"
#include "../Utilities/Reference.h"
#include "DataDrivenBSDF.h"

namespace RISE
{
	namespace Implementation
	{
		class DataDrivenSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			virtual ~DataDrivenSPF();

			//! The tabulated BSDF this SPF samples (addref'd).  The SPF
			//! evaluates kray FROM it, never from a private copy of the table.
			const DataDrivenBSDF*	pBSDF;

			//! The hemisphere the table is defined on, in one place for
			//! Scatter / Pdf / the NM evaluators: the shading-normal side the
			//! ray arrived on, with the geometric-horizon gate Lambertian uses.
			//! \return false when the arriving ray is on the table's blind side
			//!         (the BSDF is 0 there, so there is nothing to sample)
			bool FrontFrame(
				const RayIntersectionGeometric& ri,
				OrthonormalBasis3D& onbOut,
				Vector3& geomNOut
				) const;

			Scalar PdfImpl( const RayIntersectionGeometric& ri, const Vector3& wo ) const;

		public:
			DataDrivenSPF( const DataDrivenBSDF& bsdf );

			void	Scatter(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
				) const;

			void	ScatterNM(
				const RayIntersectionGeometric& ri,
				ISampler& sampler,
				const Scalar nm,
				ScatteredRayContainer& scattered,
				const IORStack& ior_stack
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

			//! f(wo; nm), the tabulated BSDF's spectral value (single lobe)
			Scalar	EvaluateLobeFNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			//! f(wo; nm) cos / Pdf(wo): what ScatterNM stamps as krayNM
			Scalar	EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;
		};
	}
}

#endif
