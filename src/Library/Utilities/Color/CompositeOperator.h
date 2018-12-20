//////////////////////////////////////////////////////////////////////
//
//  CompositeOperator.h - Composites one color on top of the 
//  other
// 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 20, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPOSITE_OPERATOR_
#define COMPOSITE_OPERATOR_

#include "../../Interfaces/ITwoColorOperator.h"
#include "../Reference.h"

namespace RISE
{
	class CompositeOperator : 
		public virtual ITwoColorOperator, 
		public virtual Implementation::Reference
	{
	public:

		static void	Composite( RISEColor& cDest, const RISEColor& cTop, const RISEColor& cBottom )
		{
			if( cTop.a == 1.0 || cBottom.a == 0.0 ) {
				cDest = cTop;
				return;
			}

			if( cTop.a == 0.0 ) {
				cDest = cBottom;
				return;
			}

			if( cBottom.a == 1.0 ) {
				const Scalar OMAlpha = 1.0 - cTop.a;
				cDest.base = cBottom.base * OMAlpha + cTop.base * cTop.a;
				cDest.a = 1.0;
			} else {
				const Scalar temp = cBottom.a * (1.0-cTop.a);
				const Scalar alpha = cDest.a = temp + cTop.a;
				cDest.base = (cTop.base*cTop.a + cBottom.base*temp) * (1.0/alpha);
				cDest.a = alpha;
			}
		}

		static RISEColor Composite( const RISEColor& cTop, const RISEColor& cBottom )
		{
			RISEColor ret;
			Composite( ret, cTop, cBottom );
			return ret;
		}

		bool PerformOperation( RISEColor& dest, const RISEColor& src ) const
		{
			// Just pass it off the static function
			Composite( dest, dest, src );
			return true;
		}
	};
}

#endif
