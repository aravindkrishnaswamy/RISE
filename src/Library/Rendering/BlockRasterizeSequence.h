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
#include <cstdint>
#include <random>

namespace RISE
{
	namespace Implementation
	{
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
			const std::uint32_t shuffleSeed;

		public:
			BlockRasterizeSequence( 
				const unsigned int block_width,
				const unsigned int block_height,
				const char type_
				) : BlockRasterizeSequence(
					block_width,block_height,type_,std::random_device{}())
			{
			}

			BlockRasterizeSequence(
				const unsigned int block_width,
				const unsigned int block_height,
				const char type_,
				const std::uint32_t shuffle_seed
				) :
			cur( 0 ),
			bwidth( block_width ),
			bheight( block_height ),
			type( type_ ),
			shuffleSeed( shuffle_seed )
			{
			}

			//! Inclusive spans [s,e] of one axis.  Boundaries sit on absolute
			//! multiples of the block size (as for a full frame, where the
			//! range starts at 0), clipped to the range: a crop with an
			//! unaligned origin gets a narrower first span, and no two blocks
			//! share a block-aligned cell -- in particular no FrameStore tile
			//! when the block size is a multiple of the FrameStore tile edge
			//! (AlignTileSizeToFrameStore).  A block size of 0, or one at least
			//! the range, gives one span.
			static void AxisSpans( const unsigned int s, const unsigned int e, const unsigned int block,
				std::vector<unsigned int>& starts, std::vector<unsigned int>& ends )
			{
				starts.clear();
				ends.clear();
				unsigned long long pos = s;
				for(;;) {
					unsigned long long last = e;
					if( block > 0 ) {
						const unsigned long long boundary = ( pos / block + 1 ) * block - 1;
						if( boundary < last ) {
							last = boundary;
						}
					}
					starts.push_back( static_cast<unsigned int>( pos ) );
					ends.push_back( static_cast<unsigned int>( last ) );
					if( last >= e ) {
						break;
					}
					pos = last + 1;
				}
			}

			void Begin( const unsigned int startx, const unsigned int endx, const unsigned int starty, const unsigned int endy )
			{
				blocks.clear();
				cur = 0;
				if( endx < startx || endy < starty ) {
					return;
				}

				// Precompute and store all the blocks, row by row, left to
				// right (type 4 returns them in this order).
				std::vector<unsigned int> xs, xe, ys, ye;
				AxisSpans( startx, endx, bwidth, xs, xe );
				AxisSpans( starty, endy, bheight, ys, ye );
				blocks.reserve( xs.size() * ys.size() );
				for( size_t j = 0; j < ys.size(); ++j ) {
					for( size_t i = 0; i < xs.size(); ++i ) {
						blocks.push_back( Rect( ys[j], xs[i], ye[j], xe[i] ) );
					}
				}

				// Centre of the range.  For a full frame this is the old
				// (width-1)/2, (height-1)/2 with the same unsigned arithmetic, so
				// the centre-out order is unchanged; for a crop it is the crop's
				// own centre (it used to be the frame-relative offset, i.e. off
				// the crop).  Captured per call: no shared state between
				// sequences.
				const unsigned int cx = startx + ( endx - startx ) / 2;
				const unsigned int cy = starty + ( endy - starty ) / 2;
				const auto compCenter = [cx, cy]( const Rect& lhs, const Rect& rhs ) {
					const unsigned int lhsx = (lhs.right+lhs.left)/2-cx;
					const unsigned int lhsy = (lhs.bottom+lhs.top)/2-cy;

					const unsigned int rhsx = (rhs.right+rhs.left)/2-cx;
					const unsigned int rhsy = (rhs.bottom+rhs.top)/2-cy;

					const unsigned int dlhs = lhsx*lhsx+lhsy*lhsy;
					const unsigned int drhs = rhsx*rhsx+rhsy*rhsy;

					return dlhs < drhs;
				};

				// Order the blocks
				switch( type )
				{
				default:
				case 0:
					std::sort( blocks.begin(), blocks.end(), compCenter );
					break;
				case 1:
					{ std::mt19937 rng{shuffleSeed}; std::shuffle( blocks.begin(), blocks.end(), rng ); }
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
