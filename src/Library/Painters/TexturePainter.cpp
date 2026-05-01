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
	// Returns straight (un-premultiplied) RGB.  Earlier revisions
	// returned `c.base * c.a` — that's premultiplied alpha, which
	// disagrees with the glTF straight-alpha convention and double-
	// dims textured baseColor under alphaMode = BLEND once the shader-
	// op also weights by alpha.  For RGB images the loader fills
	// `c.a = 1` so this is a no-op; for RGBA images callers that need
	// premultiplied output should explicitly multiply with a
	// ChannelPainter(CHAN_A) via a ProductPainter.
	if( pRIA ) {
		RISEColor c;
		pRIA->GetPEL( ri.ptCoord.y, ri.ptCoord.x, c );
		return c.base;
	} else {
		return RISEPel(1.0,1.0,1.0);
	}
}

Scalar TexturePainter::GetAlpha( const RayIntersectionGeometric& ri ) const
{
	// Returns the straight A channel of an RGBA texture (1.0 for RGB).
	// Used by the alpha-aware painter chain in the glTF importer for
	// alphaMode = MASK / BLEND, and by callers that want to recreate
	// the legacy premultiplied behaviour explicitly.
	if( pRIA ) {
		RISEColor c;
		pRIA->GetPEL( ri.ptCoord.y, ri.ptCoord.x, c );
		return c.a;
	}
	return Scalar(1);
}
