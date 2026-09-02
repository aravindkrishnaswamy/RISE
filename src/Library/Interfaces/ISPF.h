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
		RISEPel		kray;						///< Blending factor for this ray
		Scalar		krayNM;						///< Blending factor for a particular wavelength of light for spectral processing
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
		/// @return  krayNM >= 0 on success, or < 0 if not implemented
		///          (caller should fall back to BSDF evaluation).
		///
		/// Default: returns -1 (not implemented).  SPFs whose lobes are
		/// not fully represented by the material's IBSDF must override.
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
	};
}

#include "../Intersection/RayIntersectionGeometric.h"

#endif

