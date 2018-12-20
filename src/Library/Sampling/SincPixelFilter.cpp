//////////////////////////////////////////////////////////////////////
//
//  SincPixelFilter.cpp -  Implementation of the gaussian filter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 9, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SincPixelFilter.h"

using namespace RISE;
using namespace RISE::Implementation;

SincPixelFilter::SincPixelFilter( const Scalar window, const Scalar scale_ ) : scale( scale_ )
{
	dKernelWidth = window;
	dKernelHeight = window;
}

SincPixelFilter::~SincPixelFilter()
{
}
/*
Scalar SincPixelFilter::warp( const RandomNumberGenerator& random, const Point2& canonical, Point2& warped ) const
{
	Scalar in_interval_x, in_interval_y;
	
	do
	{
		Scalar x = rng.CanonicalRandom();	
		if( x < 2.0/3.0 ) {
			in_interval_x = 3.0/2.0 - HALF * sqrt(9.0-12.0*x);
		} else {
			in_interval_x = THIRD * 1.0/(1.0-x);
		}
	} while( in_interval_x > dKernelWidth );


	do
	{
		Scalar y = rng.CanonicalRandom();	
		if( y < 2.0/3.0 ) {
			in_interval_y = 3.0/2.0 - HALF * sqrt(9.0-12.0*y);
		} else {
			in_interval_y = THIRD * 1.0/(1.0-y);
		}
	} while( in_interval_y > dKernelHeight );
	
	// Pick which side
	warped.x = rng.CanonicalRandom() < 0.5 ? -in_interval_x : in_interval_x;
	warped.y = rng.CanonicalRandom() < 0.5 ? -in_interval_y : in_interval_y;

	Scalar diff_x;
	if( in_interval_x <= 1.0 ) {
		diff_x = -2.0/3.0 * in_interval_x + 1.0;
	} else {
		diff_x = THIRD * (1.0/(in_interval_x*in_interval_x));
	}
	const Scalar sinc_x= sin(PI*in_interval_x)/(PI*in_interval_x);

	Scalar diff_y;
	if( in_interval_y <= 1.0 ) {
		diff_y = -2.0/3.0 * in_interval_y + 1.0;
	} else {
		diff_y = THIRD * (1.0/(in_interval_y*in_interval_y));
	}
	const Scalar sinc_y= sin(PI*in_interval_y)/(PI*in_interval_y);

	return (sinc_x/diff_x)*(sinc_y/diff_y);
}
*/
#if 1
Scalar SincPixelFilter::warp( const RandomNumberGenerator& random, const Point2& canonical, Point2& warped ) const
{
	// Here's how we do this, take the canonical random number and use (1/1-x)-1 to convert
	// it into the interval (0..infinity)
	Scalar in_interval_x = (1.0 / (1.0-random.CanonicalRandom())) - 1.0;
	Scalar in_interval_y = (1.0 / (1.0-random.CanonicalRandom())) - 1.0;
	
	/*
	if( dKernelWidth < 10000.0 ) {
		while( in_interval > dKernelWidth ) {
			// Make sure we get a sample within the filter size
			in_interval = (1.0 / (1.0-rng.CanonicalRandom())) - 1;
		}
	}
	*/

	warped.x = random.CanonicalRandom() < 0.5 ? -in_interval_x : in_interval_x;
	warped.y = random.CanonicalRandom() < 0.5 ? -in_interval_y : in_interval_y;

	// Take the value and substitute into the sinc function, thats the weight
	return (sin(in_interval_x*scale) / (in_interval_x*scale)) * 
		(sin(in_interval_y*scale) / (in_interval_y*scale));
//	return (sin(in_interval_x*PI_OV_TWO*scale)) * 
//		(sin(in_interval_y*PI_OV_TWO*scale));
}
#endif


