//////////////////////////////////////////////////////////////////////
//
//  CompositeSPF.h - Defines a SPF that that composes two SPFs
//    together
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
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class CompositeSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			virtual ~CompositeSPF( );

			const ISPF&	top;				// top
			const ISPF& bottom;				// bottom
			const unsigned int max_recur;	// maximum level of absolute recusion before terminating

			const unsigned int max_reflection_recursion;		// maximum level of reflection recursion
			const unsigned int max_refraction_recursion;		// maximum level of refraction recursion
			const unsigned int max_diffuse_recursion;			// maximum level of diffuse recursion
			const unsigned int max_translucent_recursion;		// maximum level of translucent recursion

			const Scalar thickness;			// thickness of each of the layers

			bool	ShouldScatteredRayBePropagated(
					const ScatteredRay::ScatRayType type,
					const unsigned int steps
					) const;

			void	ProcessTopLayer(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const RISEPel& importance,									///< [in] Importance from prevous pass
					const RandomNumberGenerator& random,				///< Random number generator for the MC process
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack* const ior_stack								///< [in/out] Index of refraction stack
					) const;

			void	ProcessBottomLayer(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const RISEPel& importance,									///< [in] Importance from prevous pass
					const RandomNumberGenerator& random,				///< Random number generator for the MC process
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack* const ior_stack								///< [in/out] Index of refraction stack
					) const;

			void	ProcessTopLayerNM(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const Scalar importance,									///< [in] Importance from prevous pass
					const RandomNumberGenerator& random,				///< Random number generator for the MC process
					const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack* const ior_stack								///< [in/out] Index of refraction stack
					) const;

			void	ProcessBottomLayerNM(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const Scalar importance,									///< [in] Importance from prevous pass
					const RandomNumberGenerator& random,				///< Random number generator for the MC process
					const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack* const ior_stack								///< [in/out] Index of refraction stack
					) const;

		public:
			CompositeSPF( 
				const ISPF& top_, 
				const ISPF& bottom_, 
				const unsigned int max_recur_,
				const unsigned int max_reflection_recursion_,		// maximum level of reflection recursion
				const unsigned int max_refraction_recursion_,		// maximum level of refraction recursion
				const unsigned int max_diffuse_recursion_,			// maximum level of diffuse recursion
				const unsigned int max_translucent_recursion_,		// maximum level of translucent recursion
				const Scalar thickness_								// thickness between the materials
				);

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.  
			void	Scatter( 
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const RandomNumberGenerator& random,				///< [in] Random number generator
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const IORStack* const ior_stack								///< [in/out] Index of refraction stack
					) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into 
			//! account spectral affects.  
			void	ScatterNM( 
				const RayIntersectionGeometric& ri,								///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,					///< [in] Random number generator
				const Scalar nm,												///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,								///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack									///< [in/out] Index of refraction stack
				) const;
		};
	}
}

#endif
