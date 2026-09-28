//////////////////////////////////////////////////////////////////////
//
//  CompositeSPF.h - Defines a SPF that that composes two SPFs
//    together, and the layered BSDF that prices the same transport
//    (DL-24, 2026-09-28).
//
//  THE MODEL.  A composite is two boundary SPFs (top, bottom) separated
//  by an absorbing gap of `thickness`.  Light that crosses the top
//  interface performs a random walk between the two boundaries until it
//  leaves through one of them.  That transport is split into three
//  disjoint classes, each estimated by exactly one technique:
//
//    DIRECT   -- the top interface's own response at the entry vertex
//                (its up-going lobes).  Sampled by the top's own SPF,
//                priced by the top's own BSDF / density.
//
//    COVERED  -- walked transport whose LAST event before exiting the
//                top is either (a) a non-delta scatter at a bottom
//                layer that has a BSDF, followed by a DELTA exit
//                through the top, or (b) a non-delta exit through a top
//                layer that has a BSDF.  This is the whole of the
//                coat-over-substrate regime (a dielectric over a
//                Lambertian / GGX / Oren-Nayar / translucent base).  It
//                is priced by a position-free Monte-Carlo layered
//                evaluator (Guo et al. 2018; PBRT-v4 LayeredBxDF::f):
//                the walk runs from the entry direction, and at every
//                bottom visit it CONNECTS to the requested exit
//                direction through the top's delta refraction (term a)
//                and at every top visit it evaluates the top's own
//                exit BSDF (term b).  It is sampled from a KNOWN density
//                (a mixture of a cosine hemisphere and the bottom's own
//                sampler), so `Pdf` reports the exact density of what
//                `Scatter` emits.
//
//    WALKER   -- everything else: all-delta walks (glass over glass or
//                a mirror), walks whose last non-delta event is followed
//                by more delta events (glass over a polished coat),
//                layers with no BSDF (skin), transmission out through the
//                BOTTOM, and walks entered from below.  These are sampled
//                by an unbiased single-path random walk and emitted
//                DELTA-TAGGED: no NEE / connection strategy prices them,
//                so the MIS partition is exact and the emitted weight is
//                its own estimator.
//
//  The evaluator's walk is seeded from a hash of (incoming direction,
//  outgoing direction, position), so `value(w)` is a deterministic
//  function of its arguments: the kray of an emitted covered ray is
//  EXACTLY `value(dir) * cos / Pdf(dir)`, and the HWSS companion weight
//  of that ray is reconstructible from (ri, dir, nm) (DL-221).
//
//  ENERGY (DL-24).  Neither walk is truncated.  The recursion budgets
//  (`max_recursion`, `max_*_recursion`) used to DROP every continuation
//  past them -- which dropped the internal Fresnel/TIR reflection series
//  at the top interface's underside and lost 57 % of a lossless
//  coat-over-white furnace at normal incidence.  They now mark the depth
//  at which Russian roulette may begin, with its survival compensated,
//  so they trade variance for cost and no longer remove energy.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPOSITE_SPF_
#define COMPOSITE_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		struct CompositeSPFImpl;

		class CompositeSPF : public virtual ISPF, public virtual Reference
		{
			friend struct CompositeSPFImpl;

		protected:
			virtual ~CompositeSPF( );

			const ISPF&	top;				// top
			const ISPF& bottom;				// bottom
			const IBSDF* pTopBSDF;			// top layer's BSDF, or 0 (prices the DIRECT class and term (b))
			const IBSDF* pBottomBSDF;		// bottom layer's BSDF, or 0 (prices term (a))
			const unsigned int max_recur;	// depth past which Russian roulette may terminate a walk (DL-24: no longer a hard cut)

			const unsigned int max_reflection_recursion;		// per-type Russian-roulette onset depth
			const unsigned int max_refraction_recursion;		// per-type Russian-roulette onset depth
			const unsigned int max_diffuse_recursion;			// per-type Russian-roulette onset depth
			const unsigned int max_translucent_recursion;		// per-type Russian-roulette onset depth

			const Scalar thickness;			// thickness of each of the layers

			//! Beer-Lambert extinction coefficient for the inter-layer gap.
			//! A PHYSICAL SCALAR, not a colour: it is an inverse length that
			//! is routinely authored well above 1 (the shipped scene uses
			//! 8.0 on blue).  Typed `IScalarPainter` for exactly the reason
			//! docs/ISCALARPAINTER_REFACTOR.md gives -- `IPainter::GetColorNM`
			//! routes through the Jakob-Hanika ALBEDO uplift, which is
			//! bounded to [0,1], so every extinction above ~1 used to
			//! saturate to ~1.0 in every SPECTRAL rasterizer while the RGB
			//! walk used the authored value.  `IScalarPainter` never touches
			//! colourspace.  Mirrors `TranslucentSPF::pExtinction`.
			const IScalarPainter& extinction;

			//! The walk carries TWO stacks -- `outside` (without this object's
			//! IOR-stack entry, i.e. the medium above the top interface) and
			//! `gap` (with the entry the top interface pushed).  A single
			//! stack cannot work: IORStack keys its entries on the IObject*,
			//! which is shared by both layers, so the top's push is
			//! indistinguishable from an entry of the bottom's.
			//!
			//! EvalStack picks which one a layer's Scatter() sees: a
			//! DOWN-going ray is arriving from the medium above that layer and
			//! must see `outside` (so a stack-sensitive bottom layer reads
			//! "entering from outside"); an UP-going ray is arriving from
			//! inside and must see `gap` (so a dielectric top layer takes its
			//! from-inside branch and refracts OUT).  Full failure-mode
			//! history in the block comment in CompositeSPF.cpp.
			static const IORStack& EvalStack(
					const RayIntersectionGeometric& ri,							///< [in] The intersection whose ray direction selects the stack
					const IORStack& outside_stack,								///< [in] Stack without this object's entry
					const IORStack& gap_stack									///< [in] Stack of the inter-layer gap
					);

			//! True when a continuation of `type` leaving the walk event at
			//! `steps` has passed its budget and may be Russian-rouletted.
			//! (Pre-DL-24 this predicate DROPPED the continuation outright.)
			bool	IsRouletteEligible(
					const ScatteredRay::ScatRayType type,
					const unsigned int steps
					) const;

		public:
			//! Cosine floor for the gap crossing -- see the definition's
			//! comment in CompositeSPF.cpp for why it is 1e-3.
			static const Scalar kMinCosTheta;

			//! Hard safety cap on the number of layer events in one walk.
			//! Russian roulette ends every walk that loses energy long
			//! before this; only a LOSSLESS trapping pair (a mirror facing
			//! down over an albedo-1 bottom) reaches it, and that energy
			//! can never leave the layer stack in the first place.
			static const unsigned int kMaxWalkEvents;

			//! The slant distance a ray travels crossing the inter-layer gap:
			//! `thickness / max(|dir . normal|, kMinCosTheta)`.
			//!
			//! Public (rather than a file-static in the .cpp) so
			//! tests/CompositeExtinctionTest can check the grazing limit
			//! directly -- the behaviour it guards fires for cos < 1e-3, which
			//! is a ~1e-6 fraction of any cosine-weighted lobe and therefore
			//! unreachable by a Monte-Carlo test at any practical sample count.
			static Scalar GapPathLength(
					const Vector3& dir,									///< [in] Normalised direction crossing the gap
					const Vector3& normal,								///< [in] The slab normal (the top interface's onb.w())
					const Scalar thickness								///< [in] Perpendicular gap thickness; a negative or NaN value yields 0
					);

			CompositeSPF(
				const ISPF& top_,
				const ISPF& bottom_,
				const unsigned int max_recur_,
				const unsigned int max_reflection_recursion_,		// Russian-roulette onset for reflection continuations
				const unsigned int max_refraction_recursion_,		// Russian-roulette onset for refraction continuations
				const unsigned int max_diffuse_recursion_,			// Russian-roulette onset for diffuse continuations
				const unsigned int max_translucent_recursion_,		// Russian-roulette onset for translucent continuations
				const Scalar thickness_,							// thickness between the materials
				const IScalarPainter& extinction_,					// extinction coefficient for absorption between layers (physical scalar)
				const IBSDF* pTopBSDF_ = 0,							// top layer's BSDF (CompositeMaterial passes it); 0 = none
				const IBSDF* pBottomBSDF_ = 0						// bottom layer's BSDF (CompositeMaterial passes it); 0 = none
				);

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.
			//! Emits AT MOST ONE ray (see the file header for which class).
			void	Scatter(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					ISampler& sampler,									///< [in] Sampler
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const IORStack& ior_stack								///< [in/out] Index of refraction stack
					) const;

			//! Spectral twin of Scatter.
			void	ScatterNM(
				const RayIntersectionGeometric& ri,								///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,										///< [in] Sampler
				const Scalar nm,												///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,								///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack									///< [in/out] Index of refraction stack
				) const;

			//! The density of the NON-DELTA directions Scatter emits (the
			//! DL-67 Slice 0 contract): the top's own density over its
			//! up-going lobes plus the covered class's known proposal
			//! mixture.  Delta-tagged emissions (the top's delta lobes and
			//! the walker class) carry no density, as for every SPF.
			Scalar Pdf(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details
				const Vector3& wo,											///< [in] Outgoing scattered direction
				const IORStack& ior_stack								///< [in] Index of refraction stack
				) const;

			//! Spectral version of Pdf
			Scalar PdfNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details
				const Vector3& wo,											///< [in] Outgoing scattered direction
				const Scalar nm,											///< [in] Wavelength
				const IORStack& ior_stack								///< [in] Index of refraction stack
				) const;

			//! DL-221.  Every NON-DELTA ray this SPF emits carries
			//! `kray = value(dir) * cos / Pdf(dir)` with `value` the
			//! deterministic layered evaluator, so its companion weight
			//! is `valueNM(dir; nm) * cos / pdfHero` -- returned here as
			//! the aggregate spectral value.  -1 (fall back) only when the
			//! top layer's selection probabilities are not deterministic
			//! (see CompositeSPF.cpp, "PER-BRANCH MODE").
			Scalar EvaluateLobeFNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			//! DL-221, delta rays.  A DIRECT delta ray (the top's own
			//! delta reflection) is reconstructed from the top layer at
			//! `nm`; a WALKER ray is a stochastic multi-event path and
			//! declines (-1), which reaches the named fallback below.
			Scalar EvaluateKrayNM(
				const RayIntersectionGeometric& ri,
				const Vector3& outDir,
				ScatteredRay::ScatRayType rayType,
				Scalar nm,
				const IORStack& ior_stack
				) const;

			using ISPF::EvaluateKrayNM;

			//! DL-125 / DL-221.  Still named, because the WALKER class
			//! (delta-tagged multi-event paths) cannot be reconstructed
			//! from (ri, outDir, nm); the fallback is reached ONLY for
			//! those rays and for a top with non-deterministic selection
			//! probabilities.  Every covered and every direct ray is
			//! reconstructed exactly.
			const char* PerLobeDensityFallbackName() const
			{
				return "CompositeSPF";
			}

			//! The layered evaluator.  Returns the NON-DELTA response of the
			//! composite for light arriving from `vLightIn` scattered toward
			//! `-ri.ray.Dir()`: the top's own BSDF at the entry vertex plus
			//! the covered walked class.  Zero for an entry from below and
			//! for any direction on the far side (both are walker-only).
			//! `pStack` is the caller's LIVE IOR stack, or 0.
			RISEPel EvaluateLayered(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri,
				const IORStack* pStack
				) const;

			Scalar EvaluateLayeredNM(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri,
				const Scalar nm,
				const IORStack* pStack
				) const;

			//! True when at least one layer has a BSDF, i.e. the covered
			//! class can be priced at all.  CompositeMaterial presents a
			//! CompositeBSDF exactly when this is true.
			bool HasLayeredValue() const { return pTopBSDF || pBottomBSDF; }
		};

		//! The BSDF of a composite: the SAME function the composite's
		//! Scatter prices its non-delta emissions with, so NEE, BDPT/VCM
		//! connections and the BSDF-sampled continuation estimate one
		//! integral (DL-157, "one function per side").
		class CompositeBSDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			const CompositeSPF& spf;
			const IBSDF* pAlbedoSource;		// for the OIDN albedo AOV only

			virtual ~CompositeBSDF();

		public:
			CompositeBSDF( const CompositeSPF& spf_, const IBSDF* pAlbedoSource_ );

			RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			Scalar  valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			RISEPel valueStateful( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const IORStack* pIORStack ) const;
			Scalar  valueStatefulNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm, const IORStack* pIORStack ) const;
			RISEPel albedo( const RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
