//////////////////////////////////////////////////////////////////////
//
//  TexturePainter.cpp - Implementation of the TexturePainter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  Add wrapping and clamping abilities!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TexturePainter.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

TexturePainter::TexturePainter( IRasterImageAccessor* pRIA_ ) : pRIA( pRIA_ )
{
	if( pRIA ) {
		pRIA->addref();
	} else {
		GlobalLog()->PrintSourceError( "TexturePainter:: Invalid raster image accessor", __FILE__, __LINE__ );
	}
}

TexturePainter::~TexturePainter( )
{
	safe_release( pRIA );
}

RISEPel TexturePainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	if( pRIA ) {
		RISEColor c;
		pRIA->GetPEL( ri.ptCoord.y, ri.ptCoord.x, c );
		return c.base * c.a;
	} else {
		return RISEPel(1.0,1.0,1.0);
	}
}
