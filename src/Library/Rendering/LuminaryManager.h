//////////////////////////////////////////////////////////////////////
//
//  LuminaryManager.h - Defines a class which handles luminary
//  materials.  It automatically samples them properly and all
//  and returns the light contribution of all the luminaries
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 30, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LUMINARY_MANAGER_
#define LUMINARY_MANAGER_

#include "../Interfaces/ILuminaryManager.h"
#include "../Utilities/Reference.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class LuminaryManager : 
			public virtual ILuminaryManager, 
			public virtual Implementation::Reference
		{
		public:
			struct LUM_ELEM
			{
				const IObject*					pLum;

				LUM_ELEM() : pLum( 0 ) {}
			};

			typedef std::vector<LUM_ELEM> LuminariesList;

		protected:
			virtual ~LuminaryManager( );

			ISampling2D*			pLumSampling;
			LuminariesList			luminaries;
			Scalar					dOVcSamples;
			const bool				bRandomlySelect;							// Randomly selects only one luminaire to sample

			RISEPel ComputeDirectLightingForLuminary(
				const RayIntersectionGeometric& ri,
				const IObject& pObject,
				const Point2& ptLum,
				const IBSDF& pBRDF,
				const RandomNumberGenerator& random,
				const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
				const bool bShadowRays
				) const;

			Scalar ComputeDirectLightingForLuminaryNM( 
				const RayIntersectionGeometric& ri,
				const IObject& pObject,
				const Point2& ptLum,
				const IBSDF& pBRDF,
				const Scalar nm,
				const RandomNumberGenerator& random,
				const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
				const bool bShadowRays
				) const;

		public:
			LuminaryManager(
				const bool randomly_select
				);

			//! Binds the luminary manager to a particular scene
			void AttachScene(
				const IScene* pScene											///< [in] Scene to bind to
				);

			//! Adds the object to the list of luminaries
			void AddToLuminaryList(
				const IObject& pObject											///< [in] Object to add
				);

			//! Sets up luminaire sampling
			void SetLuminaireSampling( 
				ISampling2D* pLumSam											///< [in] Sampling kernel to use when the luminaire needs to be sampled
				);

			//! Computes direct lighting for all luminaires
			/// \return Direct lighting value as an RISEPel
			RISEPel ComputeDirectLighting( 
				const RayIntersection& ri,										///< [in] Intersection information at point we computing lighting for
				const IBSDF& pBRDF,												///< [in] BRDF of the material
				const RandomNumberGenerator& random,							///< [in] Random number generator
				const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
				const IShadowPhotonMap* pShadowMap								///< [in] Shadow photon map for speeding up shadow checks
				) const;

			//! Computes direct lighting for a single wavelength
			/// \return Direct lighting value for the particular wavelength as a scalar
			Scalar ComputeDirectLightingNM( 
				const RayIntersection& ri,										///< [in] Intersection information at point we computing lighting for
				const IBSDF& pBRDF,												///< [in] BRDF of the material
				const Scalar nm,												///< [in] Wavelength
				const RandomNumberGenerator& random,							///< [in] Random number generator
				const IRayCaster& caster,										///< [in] Ray Caster to use for shadow checks
				const IShadowPhotonMap* pShadowMap								///< [in] Shadow photon map for speeding up shadow checks
				) const;

			//! Returns the list of luminaries
			const LuminariesList& getLuminaries( ){ return luminaries; };
		};
	}
}

#endif

