//////////////////////////////////////////////////////////////////////
//
//  KleinBottleParametricFunctions.cpp - implements parametric
//    immersion of klein bottles in an R3 space
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 26, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "KleinBottleParametricFunctions.h"

using namespace RISE;
using namespace RISE::Implementation;

KleinBottleClassicParametricSurface::KleinBottleClassicParametricSurface()
{
	u_start = v_start = 0;
	u_end = v_end = TWO_PI;
}

bool KleinBottleClassicParametricSurface::Evaluate( Point3& ret, const Scalar u, const Scalar v )
{
	if( 0 <= u && u < PI ) {
		ret.x = 6.0*cos(u)*(1.0+sin(u))+4.0*(1.0-0.5*cos(u))*cos(u)*cos(v);
		ret.y = 16.0*sin(u)+4.0*(1.0-0.5*cos(u))*sin(u)* cos(v);
	} else {
		ret.x = 6.0*cos(u)*(1.0+sin(u))+4.0*(1.0-0.5*cos(u))*cos(v+PI);
		ret.y = 16.0*sin(u);
	}
	
	ret.z = 4.0*(1.0-0.5*cos(u))*sin(v);

	return true;
}

KleinBottleNordstrandParametricSurface::KleinBottleNordstrandParametricSurface()
{
	u_start = v_start = 0;
	u_end = FOUR_PI;
	v_end = TWO_PI;
}

bool KleinBottleNordstrandParametricSurface::Evaluate( Point3& ret, const Scalar u, const Scalar v )
{
	ret.x = cos(u) * (cos(u/2)*(sqrt(2.0)+cos(v))+(sin(u/2)*sin(v)*cos(v)));
	ret.y = sin(u) * (cos(u/2)*(sqrt(2.0)+cos(v))+(sin(u/2)*sin(v)*cos(v)));
	ret.z = -1.0*sin(u/2)*(sqrt(2.0)+cos(v))+cos(u/2)*sin(v)*cos(v);
	return true;
}


KleinBottleFigure8ParametricSurface::KleinBottleFigure8ParametricSurface( const Scalar a_ ) : 
  a( a_ )
{
	u_start = v_start = 0;
	u_end = v_end = TWO_PI;
}

bool KleinBottleFigure8ParametricSurface::Evaluate( Point3& ret, const Scalar u, const Scalar v )
{
	ret.x = cos(u) * (a+cos(u/2)*sin(v) - sin(u/2)*sin(2*v));
	ret.y = sin(u) * (a+cos(u/2)*sin(v) - sin(u/2)*sin(2*v));
	ret.z = sin(u/2)*sin(v) + cos(u/2)*sin(2*v);

	return true;
}