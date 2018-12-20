//////////////////////////////////////////////////////////////////////
//
//  CubicFunction.h - Declaration of a cubic function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CUBICFUNCTION_
#define CUBICFUNCTION_

#include "../Interfaces/IFunction1D.h"

namespace RISE
{
	class CubicFunction : public IFunction1D
	{
	public:
		//
		// Cubic functions are of the form y = ax^3 + bx^2 + cx + d
		//
		Scalar		a, b, c, d;

		CubicFunction( );
		CubicFunction( const Scalar a_, const Scalar b_, const Scalar c_, const Scalar d_ );
		virtual ~CubicFunction( );

		virtual Scalar Evaluate( const Scalar variable ) const;

		virtual int Solve( Scalar (&sol)[3] ) const;
	};
}


#endif
