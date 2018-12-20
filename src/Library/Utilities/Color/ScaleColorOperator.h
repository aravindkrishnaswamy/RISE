//////////////////////////////////////////////////////////////////////
//
//  ScaleColorOperator.h - Scales a color value by a given factor
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2005
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCALE_OPERATOR_
#define SCALE_OPERATOR_

#include "../../Interfaces/IOneColorOperator.h"
#include "../Reference.h"

namespace RISE
{
	class ScaleColorOperator : 
		public virtual IOneColorOperator, 
		public virtual Implementation::Reference
	{
	protected:
		const RISEColor scale;

	public:
		ScaleColorOperator( 
			const RISEColor& scale_ 
			) : 
		scale( scale_ )
		{
		}

		bool PerformOperation( RISEColor& c  ) const
		{
			c = c * scale;
			return true;
		}
	};
}

#endif
