//////////////////////////////////////////////////////////////////////
//
//  CubicFunction.cpp - Implementation of a cubic function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 21, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CubicFunction.h"
#include "Polynomial.h"

using namespace RISE;

CubicFunction::CubicFunction( ) : 
  a( 1 ), b( 0 ), c( 0 ), d( 0 )
{
}

CubicFunction::CubicFunction( const Scalar a_, const Scalar b_, const Scalar c_, const Scalar d_ ) : 
  a( a_ ), b( b_ ), c( c_ ), d( d_ )
{
}

CubicFunction::~CubicFunction( )
{
}

Scalar CubicFunction::Evaluate( const Scalar variable ) const
{
	Scalar x2 = variable*variable;
	Scalar x3 = x2 * variable;
	return a*x3 + b * x2 + c*variable + d;
}

int CubicFunction::Solve( Scalar (&sol)[3] ) const
{
	Scalar	coeff[4];
	coeff[0] = a; coeff[1] = b; coeff[2] = c; coeff[3] = d;
	return Polynomial::SolveCubic( coeff, sol );
}
