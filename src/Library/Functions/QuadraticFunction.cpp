//////////////////////////////////////////////////////////////////////
//
//  QuadraticFunction.cpp - Implementation of a quadratic function
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
#include "QuadraticFunction.h"
#include "Polynomial.h"

using namespace RISE;

QuadraticFunction::QuadraticFunction( ) : 
  a( 1 ), b( 0 ), c( 0 ) 
{
}

QuadraticFunction::QuadraticFunction( Scalar a_, Scalar b_, Scalar c_ ) : 
  a( a_ ), b( b_ ), c( c_ )
{
}

QuadraticFunction::~QuadraticFunction( )
{
}

Scalar QuadraticFunction::Evaluate( const Scalar variable ) const
{
	return variable * variable * a + b * variable + c;
}

int QuadraticFunction::Solve( Scalar (&sol)[2] ) const
{
	Scalar	coeff[3];
	coeff[0] = a; coeff[1] = b; coeff[2] = c;
	return Polynomial::SolveQuadric( coeff, sol );
}


