//////////////////////////////////////////////////////////////////////
//
//  HilbertRasterizeSequence.h - Rasterize sequence that returns a
//    series of blocks
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef HILBERT_RASTERIZE_SEQUENCE_
#define HILBERT_RASTERIZE_SEQUENCE_

#include "../Interfaces/IRasterizeSequence.h"
#include "../Utilities/Reference.h"
#include <vector>
#include <algorithm>

namespace RISE
{
	namespace Implementation
	{
		class HilbertRasterizeSequence : public virtual IRasterizeSequence, public virtual Reference
		{
		protected:
			virtual ~HilbertRasterizeSequence()
			{
			}

			typedef std::vector<Rect>	BlocksList;
			BlocksList	blocks;

			unsigned int depth;
			unsigned int num;
			unsigned int cur;

		public:
			HilbertRasterizeSequence( 
				const unsigned int depth_
				) : 
			depth( depth_ ),
			num( 1<<depth_ ),
			cur( 0 )
			{
			}

			enum
			{
				UP,
				LEFT,
				DOWN,
				RIGHT,
				NOWHERE
			};

			void move( BlocksList& b, int& x, int& y, int direction )
			{
				b.push_back( blocks[(num-y-1)*num + x] );

				switch( direction ) {
				case LEFT:
					x--;
					break;
				case RIGHT:
					x++;
					break;
				case UP:
					y++;
					break;
				case DOWN:
					y--;
					break;
				}
			}

			void hilbert(BlocksList& b, int& x, int& y, int level,int direction=UP)
			{
				if (level==1) {
					switch (direction) {
						case LEFT:
							move(b, x, y, RIGHT);
							move(b, x, y, DOWN);
							move(b, x, y, LEFT);
						break;
						case RIGHT:
							move(b, x, y, LEFT);
							move(b, x, y, UP);
							move(b, x, y, RIGHT);
						break;
						case UP:
							move(b, x, y, DOWN);
							move(b, x, y, RIGHT);
							move(b, x, y, UP);
						break;
						case DOWN:
							move(b, x, y, UP);
							move(b, x, y, LEFT);
							move(b, x, y, DOWN);
						break;
					}
				} else {
					switch (direction) {
						case LEFT:
							hilbert(b, x, y, level-1,UP);
							move(b, x, y, RIGHT);
							hilbert(b, x, y, level-1,LEFT);
							move(b, x, y, DOWN);
							hilbert(b, x, y, level-1,LEFT);
							move(b, x, y, LEFT);
							hilbert(b, x, y, level-1,DOWN);
						break;
						case RIGHT:
							hilbert(b, x, y, level-1,DOWN);
							move(b, x, y, LEFT);
							hilbert(b, x, y, level-1,RIGHT);
							move(b, x, y, UP);
							hilbert(b, x, y, level-1,RIGHT);
							move(b, x, y, RIGHT);
							hilbert(b, x, y, level-1,UP);
						break;
						case UP:
							hilbert(b, x, y, level-1,LEFT);
							move(b, x, y, DOWN);
							hilbert(b, x, y, level-1,UP);
							move(b, x, y, RIGHT);
							hilbert(b, x, y, level-1,UP);
							move(b, x, y, UP);
							hilbert(b, x, y, level-1,RIGHT);
						break;
						case DOWN:
							hilbert(b, x, y, level-1,RIGHT);
							move(b, x, y, UP);
							hilbert(b, x, y, level-1,DOWN);
							move(b, x, y, LEFT);
							hilbert(b, x, y, level-1,DOWN);
							move(b, x, y, DOWN);
							hilbert(b, x, y, level-1,LEFT);
						break;
					}
				}
			}

			void DoHilbert()
			{
				int x=0, y=num-1;
				BlocksList b2;

				hilbert( b2, x, y, depth );
				move( b2, x, y, NOWHERE );

				if( b2.size() == blocks.size() ) {
					std::swap( blocks, b2 );
				} else {
					// Log an error
					GlobalLog()->PrintEasyError( "Fatal error during the hilbert space filling curve computation, rendering plain blocks" );
				}			
			}

			void Begin( const unsigned int startx, const unsigned int endx, const unsigned int starty, const unsigned int endy )
			{
				blocks.clear();
				// Precompute and store all the blocks, then we can just easily return them

				// We must generate the blocks as a square of powers of two based on the 
				// recursion count
				// i.e. if recursion is 2, then we generate 4 blocks... etc
				tryagain:
				unsigned int bwidth = (endx-startx+1)/num;
				unsigned int bheight = (endy-starty+1)/num;

				if( bwidth*num < (endx-startx+1) ) {
					const unsigned int old_w = bwidth;
					bwidth = (endx-startx+1)/(num-1);

					if( bwidth*num%(endx-startx+1) == bwidth ) {
						// After correct things are perfect ?  sheesh!
						bwidth--;
					}

					if( bwidth == old_w ) {
						depth--;
						num = 1<<depth;
						goto tryagain;
					}
				}

				if( bheight*num < (endy-starty+1) ) {
					const unsigned int old_h = bheight;
					bheight = (endy-starty+1)/(num-1);

					if( bheight*num%(endy-starty+1) == bheight ) {
						// After correct things are perfect ?  sheesh!
						bheight--;
					}

					if( bheight == old_h ) {
						depth--;
						num = 1<<depth;
						goto tryagain;
					}
				}

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

				DoHilbert();

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

