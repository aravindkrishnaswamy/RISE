//////////////////////////////////////////////////////////////////////
//
//  IridescentPainter.cpp - Implenentation of the CheckPainter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "IridescentPainter.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

IridescentPainter::IridescentPainter( const IPainter& a_, const IPainter& b_, const Scalar bias_ ) : 
  a( a_ ),
  b( b_ ),
  bias( bias_/2.0 )
{
	pScalarInterp = new RealLinearInterpolator();
	GlobalLog()->PrintNew( pScalarInterp, __FILE__, __LINE__, "Scalar Interpolator" );

	pInterp = new LinearInterpolator<RISEPel>();
	GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "Color Interpolator" );

	a.addref();
	b.addref();
}

IridescentPainter::~IridescentPainter( )
{
	safe_release( pScalarInterp );
	safe_release( pInterp );

	a.release();
	b.release();
}

RISEPel IridescentPainter::GetColor( const RayIntersectionGeometric& ri  ) const
{
	Scalar rdotv = fabs(Vector3Ops::Dot(ri.ray.dir,ri.vNormal)) + bias;
	rdotv = rdotv < 0.0 ? 0.0 : rdotv;
	rdotv = rdotv > 1.0 ? 1.0 : rdotv;
	return pInterp->InterpolateValues( a.GetColor(ri), b.GetColor(ri), rdotv );
}

Scalar IridescentPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar rdotv = fabs(Vector3Ops::Dot(ri.ray.dir,ri.vNormal)) + bias;
	rdotv = rdotv < 0.0 ? 0.0 : rdotv;
	rdotv = rdotv > 1.0 ? 1.0 : rdotv;
	return pScalarInterp->InterpolateValues( a.GetColorNM(ri,nm), b.GetColorNM(ri,nm), rdotv );
}

static const unsigned int BIAS_ID = 100;

IKeyframeParameter* IridescentPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "bias" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), BIAS_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void IridescentPainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case BIAS_ID:
		{
			bias = *(Scalar*)val.getValue() * 0.5;
		}
		break;
	}
}

void IridescentPainter::RegenerateData( )
{
}
