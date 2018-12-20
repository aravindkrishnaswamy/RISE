//////////////////////////////////////////////////////////////////////
//
//  LinearFunction.h - Declaration of a linear function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LINEARFUNCTION_
#define LINEARFUNCTION_

#include "../Interfaces/IFunction1D.h"

namespace RISE
{
	class LinearFunction : public IFunction1D
	{
	public:
		//
		// Linear functions are of the form y = ax + b
		//
		Scalar		a, b;

		LinearFunction( );
		LinearFunction( const Scalar a_, const Scalar b_ );
		virtual ~LinearFunction( );

		virtual Scalar Evaluate( const Scalar variable ) const;

		virtual int Solve( Scalar& sol ) const;
	};
}


#endif
