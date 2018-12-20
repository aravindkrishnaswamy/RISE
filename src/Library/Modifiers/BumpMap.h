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
			
		public:
			BumpMap( const IFunction2D& func, const Scalar dScale_, const Scalar dWindow_ );
			void Modify( RayIntersectionGeometric& ri ) const;
		};
	}
}

#endif
