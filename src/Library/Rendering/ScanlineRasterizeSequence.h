//////////////////////////////////////////////////////////////////////
//
//  ScanlineRasterizeSequence.h - Rasterize sequence that merely
//    returns scanlines
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCANLINE_RASTERIZE_SEQUENCE_
#define SCANLINE_RASTERIZE_SEQUENCE_

#include "../Interfaces/IRasterizeSequence.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class ScanlineRasterizeSequence : public virtual IRasterizeSequence, public virtual Reference
		{
		protected:
			virtual ~ScanlineRasterizeSequence()
			{
			}

			unsigned int cur;

			unsigned int startx;
			unsigned int endx;
			unsigned int starty;
			unsigned int endy;

		public:
			ScanlineRasterizeSequence( 
				) : 
			cur( 0 ),
			startx( 0 ),
			endx( 0 ),
			starty( 0 ),
			endy( 0 )
			{
			}

			void Begin( const unsigned int startx_, const unsigned int endx_, const unsigned int starty_, const unsigned int endy_ )
			{
				startx = startx_;
				endx = endx_;
				starty = starty_;
				endy = endy_;
				cur = 0;
			}

			// Begin's ends are INCLUSIVE (BoundsFromRect), and Rect is
			// inclusive: one region per row, rows starty..endy.  The region
			// must be exactly one row -- neighbouring regions sharing a row
			// would render it twice with concurrent writes to the same
			// pixels, and overlapping preview regions break the
			// disjoint-region contract of GetIntermediateOutputImage.
			virtual int  NumRegions()
			{
				return static_cast<int>( endy - starty + 1 );
			}

			Rect GetNextRegion()
			{
				const unsigned int row = cur++ + starty;
				return Rect( row, startx, row, endx );
			}

			void End()
			{
			}
		};
	}
}

#endif

