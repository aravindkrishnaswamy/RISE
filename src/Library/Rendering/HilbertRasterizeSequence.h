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

			//! Requested recursion depth.  Never modified by Begin: the
			//! depth actually used is chosen per range (see Begin).
			const unsigned int depth;
			unsigned int cur;
			bool fellBack;

			enum
			{
				UP,
				LEFT,
				DOWN,
				RIGHT,
				NOWHERE
			};

			//! The num x num cell grid being walked, row-major from the
			//! TOP row.  The walk's y counts rows from the BOTTOM.  Every
			//! index is bounds-checked; a cell outside the grid, or visited
			//! twice, marks the walk bad (Begin then falls back).
			struct Walk
			{
				const BlocksList& grid;
				const unsigned int num;
				BlocksList out;
				std::vector<char> seen;
				bool bad;
				Walk( const BlocksList& g, unsigned int n ) : grid( g ), num( n ), seen( g.size(), 0 ), bad( false ) {}

				void move( int& x, int& y, int direction )
				{
					if( x >= 0 && y >= 0 && static_cast<unsigned int>( x ) < num && static_cast<unsigned int>( y ) < num ) {
						const size_t idx = static_cast<size_t>( num - 1 - static_cast<unsigned int>( y ) ) * num + static_cast<unsigned int>( x );
						if( idx < grid.size() && !seen[idx] ) {
							seen[idx] = 1;
							out.push_back( grid[idx] );
						} else {
							bad = true;
						}
					} else {
						bad = true;
					}

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

				void hilbert( int& x, int& y, unsigned int level, int direction=UP )
				{
					if( bad || level == 0 ) {
						return;
					}
					if (level==1) {
						switch (direction) {
							case LEFT:
								move(x, y, RIGHT);
								move(x, y, DOWN);
								move(x, y, LEFT);
							break;
							case RIGHT:
								move(x, y, LEFT);
								move(x, y, UP);
								move(x, y, RIGHT);
							break;
							case UP:
								move(x, y, DOWN);
								move(x, y, RIGHT);
								move(x, y, UP);
							break;
							case DOWN:
								move(x, y, UP);
								move(x, y, LEFT);
								move(x, y, DOWN);
							break;
						}
					} else {
						switch (direction) {
							case LEFT:
								hilbert(x, y, level-1,UP);
								move(x, y, RIGHT);
								hilbert(x, y, level-1,LEFT);
								move(x, y, DOWN);
								hilbert(x, y, level-1,LEFT);
								move(x, y, LEFT);
								hilbert(x, y, level-1,DOWN);
							break;
							case RIGHT:
								hilbert(x, y, level-1,DOWN);
								move(x, y, LEFT);
								hilbert(x, y, level-1,RIGHT);
								move(x, y, UP);
								hilbert(x, y, level-1,RIGHT);
								move(x, y, RIGHT);
								hilbert(x, y, level-1,UP);
							break;
							case UP:
								hilbert(x, y, level-1,LEFT);
								move(x, y, DOWN);
								hilbert(x, y, level-1,UP);
								move(x, y, RIGHT);
								hilbert(x, y, level-1,UP);
								move(x, y, UP);
								hilbert(x, y, level-1,RIGHT);
							break;
							case DOWN:
								hilbert(x, y, level-1,RIGHT);
								move(x, y, UP);
								hilbert(x, y, level-1,DOWN);
								move(x, y, LEFT);
								hilbert(x, y, level-1,DOWN);
								move(x, y, DOWN);
								hilbert(x, y, level-1,LEFT);
							break;
						}
					}
				}
			};

			//! The historical per-axis split, kept so full-frame output is
			//! unchanged wherever it used to be valid: blocks of `bsize`
			//! (with its odd extent rule) from the range origin.  Returns
			//! false unless it yields exactly `num` non-empty spans.
			static bool LegacySpans( const unsigned int s, const unsigned int e, const unsigned int bsize,
				const unsigned int num, std::vector<unsigned int>& starts, std::vector<unsigned int>& ends )
			{
				starts.clear();
				ends.clear();
				const unsigned int range = e - s;	// inclusive size - 1
				if( range == 0 || bsize == 0 ) {
					return false;
				}
				const unsigned int w = bsize <= range ? bsize-1 : range-1;
				unsigned long long pos = s;
				for(;;) {
					unsigned long long last = pos + w;
					if( last > e ) {
						last = e;
					}
					starts.push_back( static_cast<unsigned int>( pos ) );
					ends.push_back( static_cast<unsigned int>( last ) );
					if( last >= e || starts.size() > num ) {
						break;
					}
					pos = last + 1;
				}
				return starts.size() == num;
			}

			//! num near-equal non-empty spans (requires num <= e-s+1).
			static void EvenSpans( const unsigned int s, const unsigned int e, const unsigned int num,
				std::vector<unsigned int>& starts, std::vector<unsigned int>& ends )
			{
				starts.clear();
				ends.clear();
				const unsigned long long size = static_cast<unsigned long long>( e ) - s + 1;
				for( unsigned int i = 0; i < num; ++i ) {
					starts.push_back( static_cast<unsigned int>( s + size * i / num ) );
					ends.push_back( static_cast<unsigned int>( s + size * ( i + 1 ) / num - 1 ) );
				}
			}

			//! The historical depth / block-size choice (formerly mutating
			//! the members), on locals.  Returns the depth; bwidth/bheight
			//! are the block sizes for the legacy split.
			static unsigned int LegacyDepth( unsigned int d, const unsigned int W, const unsigned int H,
				unsigned int& bwidth, unsigned int& bheight )
			{
				for(;;) {
					const unsigned int num = 1u << d;
					bwidth = W/num;
					bheight = H/num;
					bool retry = false;
					if( num > 1 && bwidth*num < W ) {
						const unsigned int old_w = bwidth;
						bwidth = W/(num-1);
						if( bwidth*num%W == bwidth ) {
							bwidth--;
						}
						if( bwidth == old_w ) {
							retry = true;
						}
					}
					if( !retry && num > 1 && bheight*num < H ) {
						const unsigned int old_h = bheight;
						bheight = H/(num-1);
						if( bheight*num%H == bheight ) {
							bheight--;
						}
						if( bheight == old_h ) {
							retry = true;
						}
					}
					if( !retry || d == 0 ) {
						return d;
					}
					d--;
				}
			}

		public:
			HilbertRasterizeSequence( 
				const unsigned int depth_
				) : 
			depth( depth_ ),
			cur( 0 ),
			fellBack( false )
			{
			}

			//! True when the last Begin could not walk the Hilbert curve
			//! and returned the grid in row order instead (never expected;
			//! exposed for tests).
			bool LastBeginFellBack() const { return fellBack; }

			void Begin( const unsigned int startx, const unsigned int endx, const unsigned int starty, const unsigned int endy )
			{
				blocks.clear();
				cur = 0;
				fellBack = false;
				if( endx < startx || endy < starty ) {
					return;
				}

				const unsigned int W = endx - startx + 1;
				const unsigned int H = endy - starty + 1;

				// The grid is num x num cells, num = 2^d, and every cell must
				// be non-empty.  Use the historical choice when its split
				// gives exactly num spans on both axes (full-frame output is
				// then unchanged); otherwise split each axis into num
				// near-equal spans, with d reduced until 2^d fits the
				// smaller dimension.
				unsigned int d = depth < 15 ? depth : 15;
				unsigned int bwidth = 0, bheight = 0;
				d = LegacyDepth( d, W, H, bwidth, bheight );
				std::vector<unsigned int> xs, xe, ys, ye;
				const unsigned int legacyNum = 1u << d;
				if( !( d > 0 &&
				       LegacySpans( startx, endx, bwidth, legacyNum, xs, xe ) &&
				       LegacySpans( starty, endy, bheight, legacyNum, ys, ye ) ) )
				{
					const unsigned int smaller = W < H ? W : H;
					while( d > 0 && ( 1u << d ) > smaller ) {
						d--;
					}
					EvenSpans( startx, endx, 1u << d, xs, xe );
					EvenSpans( starty, endy, 1u << d, ys, ye );
				}
				const unsigned int num = 1u << d;

				BlocksList grid;
				grid.reserve( static_cast<size_t>( num ) * num );
				for( unsigned int j = 0; j < num; ++j ) {
					for( unsigned int i = 0; i < num; ++i ) {
						grid.push_back( Rect( ys[j], xs[i], ye[j], xe[i] ) );
					}
				}

				if( num == 1 ) {
					blocks.swap( grid );
					return;
				}

				Walk walk( grid, num );
				int x = 0, y = static_cast<int>( num ) - 1;
				walk.hilbert( x, y, d );
				walk.move( x, y, NOWHERE );

				if( !walk.bad && walk.out.size() == grid.size() ) {
					blocks.swap( walk.out );
				} else {
					// Log an error
					GlobalLog()->PrintEasyError( "Fatal error during the hilbert space filling curve computation, rendering plain blocks" );
					fellBack = true;
					blocks.swap( grid );
				}
			}

			virtual int  NumRegions()
			{
				return static_cast<int>(blocks.size());
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

