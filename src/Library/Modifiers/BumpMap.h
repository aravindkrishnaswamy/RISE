//////////////////////////////////////////////////////////////////////
//
//  BumpMap.h - A bump map applied to an object.  A bump map is
//  basically a RayIntersectionModifer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 17, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BUMP_MAP_
#define BUMP_MAP_

#include "../Interfaces/IFunction2D.h"
#include "../Utilities/Reference.h"
#include "../Interfaces/IRayIntersectionModifier.h"

namespace RISE
{
	namespace Implementation
	{
		class BumpMap :
			public virtual IRayIntersectionModifier,
			public virtual Reference
		{
		protected:
			virtual ~BumpMap( );

			const IFunction2D&	pFunction;		// This is what tells what the actual bump is...
			Scalar				dScale;			// How much to scale the values
			Scalar				dWindow;		// How far to look for the difference
												// large windows will result in blocky bump maps
												// small windows in fine bump maps
			bool				bNormalizeGradient;	// When true, divide the finite difference by
												// (2*dWindow) so dScale is the window-INDEPENDENT
												// gradient multiplier (slope amplitude).  When
												// false (default / legacy), the perturbation is
												// dScale*(f(+w)-f(-w)) -- amplitude couples to the
												// window size.  See docs/skills note in BumpMap.cpp.

		public:
			BumpMap( const IFunction2D& func, const Scalar dScale_, const Scalar dWindow_, const bool bNormalizeGradient_ = false );
			void Modify( RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
