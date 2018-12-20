//////////////////////////////////////////////////////////////////////
//
//  QuadraticFunction.h - Declaration of a quadratic function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef QUADRATICFUNCTION_
#define QUADRATICFUNCTION_

#include "../Interfaces/IFunction1D.h"

namespace RISE
{
	class QuadraticFunction : public IFunction1D
	{
	public:
		//
		// Quadratic functions are of the form y = ax^2 + bx + c
		//
		Scalar		a, b, c;

		QuadraticFunction( );
		QuadraticFunction( const Scalar a_, const Scalar b_, const Scalar c_ );
		virtual ~QuadraticFunction( );

		virtual Scalar Evaluate( const Scalar variable ) const;
		virtual int Solve( Scalar (&sol)[2] ) const;
	};
}


#endif
