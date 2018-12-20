//////////////////////////////////////////////////////////////////////
//
//  QuarticFunction.h - Declaration of a cubic function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 12, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef QUARTICFUNCTION_
#define QUARTICFUNCTION_

#include "../Interfaces/IFunction1D.h"

namespace RISE
{
	class QuarticFunction : public IFunction1D
	{
	public:
		//
		// Quartic functions are of the form y = ax^4 + bx^3 + cx^2 + dx + e
		//
		Scalar		a, b, c, d, e;

		QuarticFunction( );
		QuarticFunction( const Scalar a_, const Scalar b_, const Scalar c_, const Scalar d_, const Scalar e_ );
		virtual ~QuarticFunction( );

		virtual Scalar Evaluate( const Scalar variable ) const;

		virtual int Solve( Scalar (&sol)[4] ) const;
	};
}

#endif
