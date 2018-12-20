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
#include "../Utilities/Ray.h"
#include "IReference.h"
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
		bool		delete_stack;				///< Should the IOR stack be deleted ?
		IORStack	*ior_stack;					///< Index of refraction stack for this ray

		ScatteredRay() :
		  kray( RISEPel(0,0,0) ),
		  krayNM( 0 ),
		  type( eRayUnknown ), 
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
	protected:
		mutable ScatteredRay	rays[6];
		unsigned int	freeidx;
			
	public:
		ScatteredRayContainer();
		virtual ~ScatteredRayContainer();

		//! Adds a scattered ray
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
			const RandomNumberGenerator& random,				///< [in] Random number generator
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack* const ior_stack								///< [in/out] Index of refraction stack
			) const = 0;

		//! Given parameters describing the intersection of a ray with a surface, this will return
		//! the reflected and transmitted rays along with attenuation factors which taking into 
		//! account spectral affects.  
		virtual void	ScatterNM( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			const RandomNumberGenerator& random,				///< [in] Random number generator
			const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack* const ior_stack								///< [in/out] Index of refraction stack
			) const = 0;
	};
}

#include "../Intersection/RayIntersectionGeometric.h"

#endif

