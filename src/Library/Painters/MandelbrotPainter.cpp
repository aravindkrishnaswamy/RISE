//////////////////////////////////////////////////////////////////////
//
//  MandelbrotPainter.cpp - Implementation of the Mandelbrot
//  Painter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2001
//  Tabs: 4
//  Comments:  This implementation is similar to Dan McCormick's
//				from Swish
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MandelbrotPainter.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

MandelbrotPainter::MandelbrotPainter( 
									 const IPainter& cA_,
									 const IPainter& cB_,
									 const Scalar lower_x_,
									 const Scalar upper_x_,
									 const Scalar lower_y_,
									 const Scalar upper_y_,
									 const Scalar exp_
									 ) : 
  a( cA_ ),
  b( cB_ ),
  lower_x( lower_x_ ),
  upper_x( upper_x_ ),
  lower_y( lower_y_ ),
  upper_y( upper_y_ ),
  x_range( upper_x - lower_x ),
  y_range( upper_y - lower_y ),
  exp( exp_ ),
  pInterp( 0 )
{
	pScalarInterp = new RealLinearInterpolator();
	GlobalLog()->PrintNew( pScalarInterp, __FILE__, __LINE__, "Scalar Interpolator" );

	pInterp = new LinearInterpolator<RISEPel>();
	GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "Color Interpolator" );

	a.addref();
	b.addref();
}

MandelbrotPainter::~MandelbrotPainter( )
{
	safe_release( pScalarInterp );
	safe_release( pInterp );

	a.release();
	b.release();
}

static int count_mandelbrot_steps( const Scalar xn, const Scalar yn, const Scalar x, const Scalar y, const int count_so_far )
{
	const Scalar xnsq = xn * xn;
	const Scalar ynsq = yn * yn;

	const Scalar mag = xnsq + ynsq;

	if( count_so_far >= 256 ) {
		return count_so_far;
	}

	if( mag > 4 ) {
		return count_so_far;
	}

	return count_mandelbrot_steps( xnsq - ynsq + x, 2 * xn * yn + y, x, y, count_so_far + 1 );
}

inline Scalar MandelbrotPainter::ComputeD( const RayIntersectionGeometric& ri ) const
{
	const Scalar	x = ri.ptCoord.x*x_range + lower_x;
	const Scalar	y = ri.ptCoord.y*y_range + lower_y;

	const int count = count_mandelbrot_steps( x, y, x, y, 0 );
	Scalar	d = 1.0 - (count / 256.0);

	if( (exp > (1.0+NEARZERO)) || (exp < (1.0-NEARZERO)) ) {
		d = pow( d, exp );
	}

	return d;
}

RISEPel MandelbrotPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	return pInterp->InterpolateValues( a.GetColor(ri), b.GetColor(ri), ComputeD(ri) );
}

Scalar MandelbrotPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return pScalarInterp->InterpolateValues( a.GetColorNM(ri,nm), b.GetColorNM(ri,nm), ComputeD(ri) );
}

Scalar MandelbrotPainter::Evaluate( const Scalar x_, const Scalar y_ ) const
{
	const Scalar	x = x_*x_range + lower_x;
	const Scalar	y = y_*y_range + lower_y;

	const int count = count_mandelbrot_steps( x, y, x, y, 0 );
	Scalar	d = 1.0 - (count / 256.0);

	if( (exp > (1.0+NEARZERO)) || (exp < (1.0-NEARZERO)) ) {
		d = pow( d, exp );
	}

	return d;
}

static const unsigned int UPPERX_ID = 100;
static const unsigned int LOWERX_ID = 101;
static const unsigned int UPPERY_ID = 102;
static const unsigned int LOWERY_ID = 103;
static const unsigned int EXP_ID = 104;

IKeyframeParameter* MandelbrotPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "xend" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), UPPERX_ID );
	} else if( name == "xstart" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), LOWERX_ID );
	} else if( name == "yend" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), UPPERY_ID );
	} else if( name == "ystart" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), LOWERY_ID );
	} else if( name == "exp" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), EXP_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void MandelbrotPainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case UPPERX_ID:
		{
			upper_x = *(Scalar*)val.getValue();
		}
		break;
	case LOWERX_ID:
		{
			lower_x = *(Scalar*)val.getValue();
		}
		break;
	case UPPERY_ID:
		{
			upper_y = *(Scalar*)val.getValue();
		}
		break;
	case LOWERY_ID:
		{
			lower_y = *(Scalar*)val.getValue();
		}
		break;
	case EXP_ID:
		{
			exp = *(Scalar*)val.getValue();
		}
		break;
	}
}

void MandelbrotPainter::RegenerateData( )
{
	x_range = ( upper_x - lower_x );
	y_range = ( upper_y - lower_y );
}

