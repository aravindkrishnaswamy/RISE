//////////////////////////////////////////////////////////////////////
//
//  CheckerPainter.cpp - Implenentation of the CheckPainter class
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
#include "CheckerPainter.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

CheckerPainter::CheckerPainter( Scalar dSize_, const IPainter& a_, const IPainter& b_ ) : 
  a( a_ ),
  b( b_ ),
  dSize( dSize_ )
{
	a.addref();
	b.addref();
}

CheckerPainter::~CheckerPainter( )
{
	a.release();
	b.release();
}

inline const IPainter& CheckerPainter::ComputeWhich( const RayIntersectionGeometric& ri ) const
{
	// The checker color depends on the eveness and oddness of 
	// the texture co-ordinates divided by the size
	const int	xquot = int( ceil( ri.ptCoord.x / dSize ) );
	const int	yquot = int( ceil( ri.ptCoord.y / dSize ) );

	const bool	bXIsEven = (xquot % 2 == (xquot<0 ? 1 : 0) ? true : false );
	const bool	bYIsEven = (yquot % 2 == (yquot<0 ? 1 : 0) ? true : false );

	// If they are both the same, return one color, otherwise, return the other
	if( (bXIsEven && bYIsEven) ||
		(!bXIsEven && !bYIsEven) ) {
		return a;
	} else {
		return b;
	}
}

RISEPel CheckerPainter::GetColor( const RayIntersectionGeometric& ri  ) const
{
	return ComputeWhich(ri).GetColor(ri);
}

SpectralPacket CheckerPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	return ComputeWhich(ri).GetSpectrum(ri);
}

Scalar CheckerPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return ComputeWhich(ri).GetColorNM(ri, nm);
}

static const unsigned int SIZE_ID = 100;

IKeyframeParameter* CheckerPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "size" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), SIZE_ID );
		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "size keyframe parameter" );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void CheckerPainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case SIZE_ID:
		{
			dSize = *(Scalar*)val.getValue();
		}
		break;
	}
}

void CheckerPainter::RegenerateData( )
{
}
