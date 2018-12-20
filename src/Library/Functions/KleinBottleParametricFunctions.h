//////////////////////////////////////////////////////////////////////
//
//  KleinBottleParametricFunctions.h - parametric forms for 
//    klein bottles
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef KLEIN_BOTTLE_PARAMETRIC_FUNCTIONS_
#define KLEIN_BOTTLE_PARAMETRIC_FUNCTIONS_

#include "ParametricSurface.h"

namespace RISE
{
	namespace Implementation
	{
		//! This is an immersion of the classic Klein bottle surface in R3
		class KleinBottleClassicParametricSurface : public virtual ParametricSurface
		{
		protected:
			virtual ~KleinBottleClassicParametricSurface(){};

		public:
			KleinBottleClassicParametricSurface();

			//! Evalutes the surface at the given co-ordinates, returning a point on the surface
			/// \return TRUE if the given value evaluates to something on the surface, FALSE otherwise
			bool Evaluate( 
				Point3& ret,							///< [out] The point on the surface
				const Scalar u,							///< [in] First co-ordinate
				const Scalar v							///< [in] Second co-ordinate
				);
		};

		//! This is an immersion derived from Nordstrand
		class KleinBottleNordstrandParametricSurface : public virtual ParametricSurface
		{
		protected:
			virtual ~KleinBottleNordstrandParametricSurface(){};

		public:
			KleinBottleNordstrandParametricSurface();

			//! Evalutes the surface at the given co-ordinates, returning a point on the surface
			/// \return TRUE if the given value evaluates to something on the surface, FALSE otherwise
			bool Evaluate( 
				Point3& ret,							///< [out] The point on the surface
				const Scalar u,							///< [in] First co-ordinate
				const Scalar v							///< [in] Second co-ordinate
				);
		};

		//! This is an immersion of a klein bottle in figure 8
		class KleinBottleFigure8ParametricSurface : public virtual ParametricSurface
		{
		protected:
			virtual ~KleinBottleFigure8ParametricSurface(){};

			Scalar		a;

		public:
			KleinBottleFigure8ParametricSurface( const Scalar a_ );

			//! Evalutes the surface at the given co-ordinates, returning a point on the surface
			/// \return TRUE if the given value evaluates to something on the surface, FALSE otherwise
			bool Evaluate( 
				Point3& ret,							///< [out] The point on the surface
				const Scalar u,							///< [in] First co-ordinate
				const Scalar v							///< [in] Second co-ordinate
				);
		};
	}
}

#endif