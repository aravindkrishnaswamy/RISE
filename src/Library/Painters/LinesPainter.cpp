//////////////////////////////////////////////////////////////////////
//
//  LinesPainter.cpp - Implementation of the LinesPainter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 21, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LinesPainter.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

LinesPainter::LinesPainter( Scalar dSize_, const IPainter& a_, const IPainter& b_, bool bVertical_ ) : 
  a( a_ ),
  b( b_ ),
  dSize( dSize_ ),
  bVertical( bVertical_ )
{
	a.addref();
	b.addref();
}

LinesPainter::~LinesPainter( )
{
	a.release();
	b.release();
}

inline const IPainter& LinesPainter::ComputeWhich( const RayIntersectionGeometric& ri ) const
{
	// The lines patten depends on the modulus of one of the co-ordinates with
	// the size
	const Scalar*		pCoordToCheck = 0;

	if( bVertical ) {
		pCoordToCheck = &ri.ptCoord.x;
	} else {
		pCoordToCheck = &ri.ptCoord.y;
	}

	const int		quot = int( *pCoordToCheck / dSize );

	return (quot % 2 == 0 ? a : b );
}

RISEPel LinesPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	return ComputeWhich(ri).GetColor(ri);
}

SpectralPacket LinesPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	return ComputeWhich(ri).GetSpectrum(ri);
}

Scalar LinesPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	return ComputeWhich(ri).GetColorNM(ri, nm);
}

static const unsigned int SIZE_ID = 100;

IKeyframeParameter* LinesPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "size" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), SIZE_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void LinesPainter::SetIntermediateValue( const IKeyframeParameter& val )
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

void LinesPainter::RegenerateData( )
{
}


