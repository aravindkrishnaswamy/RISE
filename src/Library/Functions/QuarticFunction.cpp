//////////////////////////////////////////////////////////////////////
//
//  QuarticFunction.cpp - Implementation of a quartic function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 12, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "QuarticFunction.h"
#include "Polynomial.h"

using namespace RISE;

QuarticFunction::QuarticFunction( ) : 
  a( 1 ), b( 0 ), c( 0 ), d( 0 ), e( 0 )
{
}

QuarticFunction::QuarticFunction( const Scalar a_, const Scalar b_, const Scalar c_, const Scalar d_, const Scalar e_ ) : 
  a( a_ ), b( b_ ), c( c_ ), d( d_ ), e( e_ )
{
}

QuarticFunction::~QuarticFunction( )
{
}

Scalar QuarticFunction::Evaluate( const Scalar variable ) const
{
	Scalar x2 = variable*variable;
	Scalar x3 = x2 * variable;
	Scalar x4 = x3 * variable;
	return a*x4 + b*x3 + c*x2 + d*variable + e;
}

int QuarticFunction::Solve( Scalar (&sol)[4] ) const
{
	Scalar	coeff[5];
	coeff[0] = a; coeff[1] = b; coeff[2] = c; coeff[3] = d; coeff[4] = e;
	return Polynomial::SolveQuartic( coeff, sol );
}


