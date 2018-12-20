//////////////////////////////////////////////////////////////////////
//
//  IRasterizeSequence.h - This class basically feed the rasterizer
//    a sequence of blocks to rasterize.  It allows the user to
//    control the order in which parts of the scene are rendered
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIZE_SEQUENCE_
#define IRASTERIZE_SEQUENCE_

#include "IReference.h"
#include "IRasterImage.h"	// For Rect

namespace RISE
{
	class IRasterizeSequence : public virtual IReference
	{
	protected:
		IRasterizeSequence(){};
		virtual ~IRasterizeSequence(){};

	public:
		virtual void Begin( const unsigned int startx, const unsigned int endx, const unsigned int starty, const unsigned int endy ) = 0;
		virtual int  NumRegions() = 0;
		virtual Rect GetNextRegion() = 0;
		virtual void End() = 0;
	};
}

#endif
