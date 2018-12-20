//////////////////////////////////////////////////////////////////////
//
//  ShiftColorOperator.h - Shifts a color value by a given factor
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 20, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SHIFT_OPERATOR_
#define SHIFT_OPERATOR_

#include "../../Interfaces/IOneColorOperator.h"
#include "../Reference.h"

namespace RISE
{
	class ShiftColorOperator : 
		public virtual IOneColorOperator, 
		public virtual Implementation::Reference
	{
	protected:
		const RISEColor shift;

	public:
		ShiftColorOperator( 
			const RISEColor& shift_ 
			) : 
		shift( shift_ )
		{
		}

		bool PerformOperation( RISEColor& c ) const
		{
			c = c + shift;
			return true;
		}
	};
}

#endif
