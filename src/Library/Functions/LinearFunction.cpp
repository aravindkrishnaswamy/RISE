//////////////////////////////////////////////////////////////////////
//
//  LinearFunction.cpp - Implementation of a linear function
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
#include "LinearFunction.h"

using namespace RISE;

LinearFunction::LinearFunction( ) : 
  a( 1 ), b( 0 ) 
{
}

LinearFunction::LinearFunction( const Scalar a_, const Scalar b_ ) : 
  a( a_ ), b( b_ )
{
}

LinearFunction::~LinearFunction( )
{
}

Scalar LinearFunction::Evaluate( const Scalar variable ) const
{
	return variable * a + b;
}

int LinearFunction::Solve( Scalar& sol ) const
{
	sol = -b / a;
	return 1;
}


