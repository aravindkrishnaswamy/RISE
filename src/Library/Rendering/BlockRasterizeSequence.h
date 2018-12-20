//////////////////////////////////////////////////////////////////////
//
//  BlockRasterizeSequence.h - Rasterize sequence that returns a
//    series of blocks
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BLOCK_RASTERIZE_SEQUENCE_
#define BLOCK_RASTERIZE_SEQUENCE_

#include "../Interfaces/IRasterizeSequence.h"
#include "../Utilities/Reference.h"
#include <vector>
#include <algorithm>

namespace RISE
{
	namespace Implementation
	{
		static unsigned int g_width;
		static unsigned int g_height;

		inline static bool CompDistanceCenter( const Rect& lhs, const Rect& rhs )
		{
			const unsigned int lhsx = (lhs.right+lhs.left)/2-g_width/2;
			const unsigned int lhsy = (lhs.bottom+lhs.top)/2-g_height/2;

			const unsigned int rhsx = (rhs.right+rhs.left)/2-g_width/2;
			const unsigned int rhsy = (rhs.bottom+rhs.top)/2-g_height/2;

			const unsigned int dlhs = lhsx*lhsx+lhsy*lhsy; 
			const unsigned int drhs = rhsx*rhsx+rhsy*rhsy;

			return dlhs < drhs;
		}

		inline static bool CompDistanceTopLeft( const Rect& lhs, const Rect& rhs )
		{
			const unsigned int lhsx = (lhs.right+lhs.left)/2;
			const unsigned int lhsy = (lhs.bottom+lhs.top)/2;

			const unsigned int rhsx = (rhs.right+rhs.left)/2;
			const unsigned int rhsy = (rhs.bottom+rhs.top)/2;

			const unsigned int dlhs = lhsx*lhsx+lhsy*lhsy; 
			const unsigned int drhs = rhsx*rhsx+rhsy*rhsy;

			return dlhs < drhs;
		}

		inline static bool CompDistanceBottomRight( const Rect& lhs, const Rect& rhs )
		{
			const unsigned int lhsx = (lhs.right+lhs.left)/2;
			const unsigned int lhsy = (lhs.bottom+lhs.top)/2;

			const unsigned int rhsx = (rhs.right+rhs.left)/2;
			const unsigned int rhsy = (rhs.bottom+rhs.top)/2;

			const unsigned int dlhs = lhsx*lhsx+lhsy*lhsy; 
			const unsigned int drhs = rhsx*rhsx+rhsy*rhsy;

			return dlhs > drhs;
		}

		inline static bool CompDistanceLeftToRight( const Rect& lhs, const Rect& rhs )
		{
			const unsigned int lhsx = (lhs.right+lhs.left)/2;
			const unsigned int lhsy = (lhs.bottom+lhs.top)/2;

			const unsigned int rhsx = (rhs.right+rhs.left)/2;
			const unsigned int rhsy = (rhs.bottom+rhs.top)/2;

			if( lhsx == rhsx ) {
				return lhsy<rhsy;
			}
			return lhsx<rhsx;
		}

		inline static bool CompDistanceRightToLeft( const Rect& lhs, const Rect& rhs )
		{
			const unsigned int lhsx = (lhs.right+lhs.left)/2;
			const unsigned int lhsy = (lhs.bottom+lhs.top)/2;

			const unsigned int rhsx = (rhs.right+rhs.left)/2;
			const unsigned int rhsy = (rhs.bottom+rhs.top)/2;

			if( lhsx == rhsx ) {
				return lhsy>rhsy;
			}
			return lhsx>rhsx;
		}

		inline static bool CompDistanceTopToBottom( const Rect& lhs, const Rect& rhs )
		{
			const unsigned int lhsx = (lhs.right+lhs.left)/2;
			const unsigned int lhsy = (lhs.bottom+lhs.top)/2;

			const unsigned int rhsx = (rhs.right+rhs.left)/2;
			const unsigned int rhsy = (rhs.bottom+rhs.top)/2;

			if( lhsy == rhsy ) {
				return lhsx<rhsx;
			}
			return lhsy<rhsy;
		}

		inline static bool CompDistanceBottomToTop( const Rect& lhs, const Rect& rhs )
		{
			const unsigned int lhsx = (lhs.right+lhs.left)/2;
			const unsigned int lhsy = (lhs.bottom+lhs.top)/2;

			const unsigned int rhsx = (rhs.right+rhs.left)/2;
			const unsigned int rhsy = (rhs.bottom+rhs.top)/2;

			if( lhsy == rhsy ) {
				return lhsx>rhsx;
			}
			return lhsy>rhsy;
		}

		class BlockRasterizeSequence : public virtual IRasterizeSequence, public virtual Reference
		{
		protected:
			virtual ~BlockRasterizeSequence()
			{
			}

			typedef std::vector<Rect>	BlocksList;
			BlocksList	blocks;

			unsigned int cur;
			const unsigned int bwidth;
			const unsigned int bheight;
			const char type;

		public:
			BlockRasterizeSequence( 
				const unsigned int block_width,
				const unsigned int block_height,
				const char type_
				) : 
			cur( 0 ),
			bwidth( block_width ),
			bheight( block_height ),
			type( type_ )
			{
			}

			void Begin( const unsigned int startx, const unsigned int endx, const unsigned int starty, const unsigned int endy )
			{
				blocks.clear();
				// Precompute and store all the blocks, then we can just easily return them

				// Just return the blocks left to right
				const unsigned int w = bwidth <= (endx-startx) ? bwidth-1 : (endx-startx)-1;
				const unsigned int h = bheight <= (endy-starty) ? bheight-1 : (endy-starty)-1;

				unsigned int nLastY = 0;
				unsigned int nLastX = 0;

				for(;;)
				{
					unsigned int nEndy = nLastY+h;
					unsigned int nEndx = nLastX+w;

					if( nEndy > endy ) {
						nEndy = endy;
					}

					if( nEndx > endx ) {
						nEndx = endx;
					}

					blocks.push_back( Rect( nLastY, nLastX, nEndy, nEndx ) );

					if( nEndy == endy && 
						nEndx == endx ) {
						// very last block was just done, so break
						break;
					}

					if( nEndx == endx ) {
						// Last column in some row
						nLastY = nEndy+1;
						nLastX = 0;
					} else {
						nLastX = nEndx+1;
					}
				}

				g_width = endx-startx;
				g_height = endy-starty;

				// Order the blocks
				switch( type )
				{
				default:
				case 0:
					std::sort( blocks.begin(), blocks.end(), CompDistanceCenter );
					break;
				case 1:
//					std::random_shuffle( blocks.begin(), blocks.end() );
					break;
				case 2:
					std::sort( blocks.begin(), blocks.end(), CompDistanceTopLeft );
					break;
				case 3:
					std::sort( blocks.begin(), blocks.end(), CompDistanceBottomRight );
					break;
				case 4:
					break;
				case 5:
					std::sort( blocks.begin(), blocks.end(), CompDistanceLeftToRight );
					break;
				case 6:
					std::sort( blocks.begin(), blocks.end(), CompDistanceTopToBottom );
					break;
				case 7:
					std::sort( blocks.begin(), blocks.end(), CompDistanceRightToLeft );
					break;
				case 8:
					std::sort( blocks.begin(), blocks.end(), CompDistanceBottomToTop );
					break;
				}

				cur = 0;
			}

			virtual int  NumRegions()
			{
				return blocks.size();
			}

			Rect GetNextRegion()
			{
				return blocks[cur++];	
			}

			void End()
			{
				blocks.clear();
			}
		};
	}
}

#endif

