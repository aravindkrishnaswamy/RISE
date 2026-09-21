//////////////////////////////////////////////////////////////////////
//
//  ISPF.h - Defines the interface to a SPF.  The SPF is the scattering
//    probability function, which describes the distribution of 
//    reflected and transmitted rays for a material
//
//    This distribution follows a given PDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISPF_
#define ISPF_

#include "../Utilities/Color/Color.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/IORStack.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/Ray.h"
#include "IReference.h"
#include "SpecularInfo.h"
#include <vector>

namespace RISE
{
	class RayIntersectionGeometric;

	//! Describes a scattered ray from a point on the surface
	struct ScatteredRay
	{
		//! Types of scattered rays
		enum ScatRayType
		{
			eRayUnknown			= 0,		///< Unknown type of ray
			eRayDiffuse			= 1,		///< Diffuse ray
			eRayReflection		= 2,		///< Some kind of reflection ray
			eRayRefraction		= 3,		///< Some kind of refraction ray
			eRayTranslucent		= 4			///< Some kind of translucent ray
		};


		Ray			ray;						///< The actual Ray

		//! Blending factor for this ray -- BSDF * cos / pdf, Fresnel and
		//! Beer's-law attenuation included.
		//!
		//! CONTRACT (debt 30, 2026-09-12): kray and krayNM EXCLUDE the
		//! eta^2 basic-radiance factor of a medium change.  An SPF does not
		//! know whether the walk consuming it carries radiance or
		//! importance, and threading a `TransportMode` through
		//! `ISPF::Scatter` would touch ~60 implementations; so a
		//! RADIANCE-mode consumer must multiply by
		//! `RISE::RadianceEtaScale( walkStack, scat.ior_stack )` (`scat` the
		//! chosen `ScatteredRay` -- the field lives on a `ScatteredRay`, not
		//! on the `ScatteredRayContainer` itself; helper declared in
		//! Utilities/IORStack.h) when it folds kray into its throughput, and
		//! an IMPORTANCE-mode consumer must not.  See
		//! docs/REFRACTIVE_RADIANCE_SCALING.md for the site table.
		RISEPel		kray;
		Scalar		krayNM;						///< Same, for a single wavelength (spectral processing).  Same eta^2 contract as kray.
		ScatRayType	type;						///< Type of ray
		Scalar		pdf;						///< Sampling PDF for this scattered direction (solid angle measure)
		bool		isDelta;					///< True if this ray was sampled from a delta distribution (perfect mirror/refraction)
		bool		delete_stack;				///< Should the IOR stack be deleted ?
		IORStack	*ior_stack;					///< Index of refraction stack for this ray

		ScatteredRay() :
		  kray( RISEPel(0,0,0) ),
		  krayNM( 0 ),
		  type( eRayUnknown ),
		  pdf( 0 ),
		  isDelta( false ),
		  delete_stack( true ),
		  ior_stack( 0 )
		{}

		virtual ~ScatteredRay()
		{
			if( delete_stack ) {
				safe_delete( ior_stack );
			}
		}
	};

	//! This is a class that contains a scattered rays
	class ScatteredRayContainer
	{
	public:
		//! Maximum number of scattered rays one container can hold.
		//!
		//! An `AddScatteredRay` past this returns false and the ray is
		//! DISCARDED, so the cap is an energy loss, not a queueing delay --
		//! which is why it is a named constant used by the array bound, the
		//! bounds check, and the `cdf[]` / `valid[]` scratch arrays of the
		//! three RandomlySelect* functions alike (the literal 6 used to be
		//! duplicated across all five sites, so raising it meant finding
		//! every copy).
		//!
		//! Sized 12 from measurement, not intuition.  The demanding producer
		//! is `CompositeSPF`'s two-layer random walk over a DISPERSIVE top
		//! dielectric: a per-channel IOR makes `DielectricSPF::Scatter` run
		//! `DoSingleRGBComponent` three times, so the top interface emits up
		//! to 3 up-going Fresnel lobes (3 exits) plus 3 down-going refracted
		//! rays, and each of those returns through the top interface for up
		//! to 3 more exits -- 3 + 3x3 = 12 over a DIFFUSE substrate.  Measured
		//! over 200 000 draws of that fixture (budgets 4/2/2/2/2, the
		//! LayeredWhiteFurnaceTest set): max 12 attempted per Scatter, and
		//! at capacity 6, 21.7 % of all exit rays were dropped (45.96 % of
		//! Scatter calls lost at least one).  Non-dispersive stacks peak at
		//! 3-4 even at deep budgets, so they never came near either bound.
		//!
		//! 12 is a bound for the coat-over-diffuse regime, NOT a guarantee:
		//! a dispersive top over a NON-diffuse bottom adds one down-exit per
		//! refracted ray, so dispersive/dielectric reaches 15 even at the
		//! parser's default budgets (max_recursion 3, per-type 3), and a
		//! dispersive top at deep budgets measured 30.  Producers that can
		//! overflow must check the return value rather than assume success
		//! (CompositeSPF warns once per process when it does).
		//!
		//! Cost of the larger array: every slot is constructed and destroyed
		//! with the container, so construct+destruct went ~19 -> ~37 ns per
		//! container (microbenchmark, Config.OSX flags).  At ~1e8 Scatter
		//! calls per 1024x576x64spp render that is ~2 s of CPU on ~250 s --
		//! under 1 %, below the wall-clock noise floor, but not zero.
		static const unsigned int kCapacity = 12;

	protected:
		mutable ScatteredRay	rays[kCapacity];
		unsigned int	freeidx;

	public:
		ScatteredRayContainer();
		virtual ~ScatteredRayContainer();

		//! Adds a scattered ray.
		/// \return true if the ray was stored; FALSE if the container was
		///         already at kCapacity, in which case the ray is dropped
		///         entirely (and keeps ownership of its own IOR stack).
		bool AddScatteredRay(
			ScatteredRay& ray											///< [in] Scattered ray to add
			);

		//! Returns the number of scattered rays
		inline unsigned int Count() const { return freeidx; };

		//! Returns the requested ray
		inline ScatteredRay& operator[] ( const unsigned int i ) const { return rays[i]; };

		//! From the rays stored, randomly returns one given a value
		ScatteredRay* RandomlySelect(
			const double random,										///< [in] Random number to use in ray selection
			const bool bNM												///< [in] Should the spectral values be used when selecting?
			) const;

		//! From the rays stored, randomly returns a non diffuse ray
		ScatteredRay* RandomlySelectNonDiffuse( 
			const double random,										///< [in] Random number to use in ray selection
			const bool bNM												///< [in] Should the spectral values be used when selecting?
			) const;

		//! From the rays stored, randomly returns a diffuse ray
		ScatteredRay* RandomlySelectDiffuse( 
			const double random,										///< [in] Random number to use in ray selection
			const bool bNM												///< [in] Should the spectral values be used when selecting?
			) const;
	};

	//! Represents the Scattering Probability Function
	//! The SPF describes how light is scattered.  It typically returns some reflected
	//! and transimitted rays.  The SPF is constructed based on a Probability Distribution
	//! Function for such a surface. 
	class ISPF : public virtual IReference
	{
	protected:
		ISPF(){}
		virtual ~ISPF(){}

	public:

		//! Given parameters describing the intersection of a ray with a surface, this will return
		//! the reflected and transmitted rays along with attenuation factors.  
		virtual void	Scatter(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,											///< [in] Sampler
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack									///< [in/out] Index of refraction stack
			) const = 0;

		//! Given parameters describing the intersection of a ray with a surface, this will return
		//! the reflected and transmitted rays along with attenuation factors which taking into
		//! account spectral affects.
		virtual void	ScatterNM(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,											///< [in] Sampler
			const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack									///< [in/out] Index of refraction stack
			) const = 0;

		//! Evaluates the PDF (probability density function) for scattering from the incoming
		//! direction (given by ri.ray.Dir()) to the outgoing direction wo.
		//! Returns the PDF value in solid angle measure.
		//! For delta distributions (perfect reflection/refraction), returns 0.
		/// \return PDF value in solid angle measure [1/sr]
		virtual Scalar	Pdf(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details (incoming direction is ri.ray.Dir())
			const Vector3& wo,											///< [in] Outgoing scattered direction to evaluate PDF for
			const IORStack& ior_stack									///< [in] Index of refraction stack
			) const { return 0; }

		//! Spectral version of Pdf evaluation
		/// \return PDF value in solid angle measure [1/sr]
		virtual Scalar	PdfNM(
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details (incoming direction is ri.ray.Dir())
			const Vector3& wo,											///< [in] Outgoing scattered direction to evaluate PDF for
			const Scalar nm,											///< [in] Wavelength
			const IORStack& ior_stack									///< [in] Index of refraction stack
			) const { return 0; }

		//! Returns specular (delta distribution) information for this SPF.
		//! Default: non-specular.  Overridden by DielectricSPF, PerfectRefractorSPF,
		//! PerfectReflectorSPF, PolishedSPF to report their specular nature and IOR.
		virtual SpecularInfo GetSpecularInfo(
			const RayIntersectionGeometric& ri,
			const IORStack& ior_stack
			) const
		{
			return SpecularInfo();
		}

		//! Spectral variant of GetSpecularInfo.
		virtual SpecularInfo GetSpecularInfoNM(
			const RayIntersectionGeometric& ri,
			const IORStack& ior_stack,
			const Scalar nm
			) const
		{
			return GetSpecularInfo( ri, ior_stack );
		}

		/// Evaluate the spectral throughput weight (krayNM) for a
		/// previously sampled scattered ray at a different wavelength.
		///
		/// Given a ray that was produced by ScatterNM at some hero
		/// wavelength, this method returns what krayNM would be for
		/// the same geometric event at wavelength @a nm.
		///
		/// @a rayType identifies the lobe (eRayDiffuse, eRayReflection,
		/// etc.) so the match is correct even if the container layout
		/// changes across wavelengths.
		///
		/// @return krayNM >= 0 on success, or < 0 if unimplemented or
		///         this lobe is unsupported (caller falls back to BSDF).
		///
		/// Default: returns -1. Override when the aggregate BSDF paired
		/// with the emitted ray's density cannot recover its response.
		/// Per-lobe conditional densities can require selected-lobe
		/// evaluation even when IBSDF represents every lobe (DL-125);
		/// sampler response absent from IBSDF also requires an override.
		/// Aggregate-density rays with matching aggregate response may
		/// use the fallback.
		///
		/// TWO CONTRACT NOTES AN IMPLEMENTER MUST READ (DL-125 review
		/// round 1).
		///
		/// 1. THE `ri` MAY BE SYNTHETIC. PT's HWSS body and the two
		///    BDPT generator ladders pass the live sampler record.
		///    RecomputeSubpathThroughputNM rebuilds the frame/painter
		///    state and places its ray origin at the stored live incoming
		///    distance (including ray advances), so Translucent's Beer
		///    factor is recoverable there too (DL-222 closed). This is
		///    distance preservation, not a claim of every original field:
		///    glossyFilterWidth remains 0. Ordinary connection records
		///    still have zero-length incoming rays (DL-223); that separate
		///    BSDF path does not inherit this replay-only reconstruction.
		///
		/// 2. THE DENSITY CONVENTION IS `p_I(nm)`, NOT `p_I(heroNM)`, AND
		///    THAT IS A KNOWN RESIDUAL (DL-216).  The direction was drawn
		///    from the HERO wavelength's density, so the strictly
		///    unbiased companion weight is `f_I(nm) cos / p_I(heroNM)`;
		///    every implementation here returns `f_I(nm) cos / p_I(nm)`.
		///    The two COINCIDE wherever the lobe's DENSITY is
		///    wavelength-independent -- every reflectance-only chromatic
		///    material, i.e. the overwhelmingly common case and the one
		///    DL-125's measurements exercise -- and differ only under a
		///    chromatic SHAPE painter (`exponent`, `isotropy`, `alpha`,
		///    `roughness`).  `HairSPF`'s own override states the same
		///    premise.  Do not "fix" this in isolation: closing it means
		///    carrying the hero density alongside (an extra argument),
		///    and is tracked as DL-216.
		virtual Scalar EvaluateKrayNM(
			const RayIntersectionGeometric& ri,
			const Vector3& outDir,
			ScatteredRay::ScatRayType rayType,
			Scalar nm,
			const IORStack& ior_stack
			) const
		{
			return -1;
		}

		/// DL-125.  The class name to report when the HWSS companion
		/// ladder's `EvaluateKrayNM` fallback is reached for an SPF the
		/// fallback is NOT exact for; 0 (the default) for every SPF
		/// where it IS exact or unreachable.
		///
		/// The fallback is aggregate BSDF evaluation times `cos / pS->pdf` --
		/// the material's AGGREGATE spectral BSDF over the ONE selected
		/// lobe's density. Aggregate-density rays with response matching
		/// the aggregate BSDF use this pairing (CoatedSPF / FabricSPF /
		/// WeaveSPF and the single-emit GGXSPF / CookTorranceSPF).
		/// A per-lobe conditional density can instead mispair that summed
		/// response. CompositeSPF still declines (DL-221):
		/// its stochastic two-layer walk cannot be recovered from these
		/// arguments. TranslucentSPF now evaluates its normal entry/exit
		/// lobes (DL-222 closed), but retains a diagnostic identity for
		/// unsupported lobe types. Overriding this method keeps any such
		/// fallback visible instead of silently taking a wrong number.
		virtual const char* PerLobeDensityFallbackName() const
		{
			return 0;
		}
	};

	//! DL-125.  One-shot (per process, per class) warning when the HWSS
	//! companion ladder falls back to the aggregate-BSDF pairing for an
	//! SPF that names itself through `PerLobeDensityFallbackName()`.
	//! A no-op for every other SPF, and never more than one log line per
	//! class however many million vertices are shaded.
	//! Defined in Materials/ScatteredRayContainer.cpp.
	void NotePerLobeDensityCompanionFallback( const ISPF* pSPF );
}

#include "../Intersection/RayIntersectionGeometric.h"

#endif

