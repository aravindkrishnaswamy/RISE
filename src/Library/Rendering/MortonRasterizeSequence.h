//////////////////////////////////////////////////////////////////////
//
//  MortonRasterizeSequence.h - Rasterize sequence that returns tiles
//    ordered by a Morton (Z-order) space-filling curve for optimal
//    cache locality during rendering.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MORTON_RASTERIZE_SEQUENCE_
#define MORTON_RASTERIZE_SEQUENCE_

#include "../Interfaces/IRasterizeSequence.h"
#include "../Utilities/Reference.h"
#include <vector>
#include <algorithm>
#include <cstdint>

namespace RISE
{
	namespace Implementation
	{
		class MortonRasterizeSequence : public virtual IRasterizeSequence, public virtual Reference
		{
		protected:
			virtual ~MortonRasterizeSequence()
			{
			}

			typedef std::vector<Rect>	TilesList;
			TilesList	tiles;

			unsigned int cur;
			const unsigned int tileSize;

			/// Interleave the lower 16 bits of x with zeros to produce a 32-bit Morton code half.
			static inline uint32_t ExpandBits( uint32_t x )
			{
				x = (x | (x << 8)) & 0x00FF00FFu;
				x = (x | (x << 4)) & 0x0F0F0F0Fu;
				x = (x | (x << 2)) & 0x33333333u;
				x = (x | (x << 1)) & 0x55555555u;
				return x;
			}

			/// Compute the Morton code (Z-order) for a 2D tile coordinate.
			static inline uint32_t MortonCode( uint32_t tileX, uint32_t tileY )
			{
				return (ExpandBits(tileX) << 1) | ExpandBits(tileY);
			}

		public:
			MortonRasterizeSequence(
				const unsigned int tileSize_
				) :
			cur( 0 ),
			tileSize( tileSize_ > 0 ? tileSize_ : 32 )
			{
			}

			void Begin( const unsigned int startx, const unsigned int endx, const unsigned int starty, const unsigned int endy )
			{
				tiles.clear();

				const unsigned int imgWidth = endx - startx + 1;
				const unsigned int imgHeight = endy - starty + 1;

				// Number of tiles in each dimension
				const unsigned int tilesX = (imgWidth + tileSize - 1) / tileSize;
				const unsigned int tilesY = (imgHeight + tileSize - 1) / tileSize;

				// Build tile list with Morton codes for sorting
				struct MortonTile {
					uint32_t	mortonCode;
					Rect		rect;
				};
				std::vector<MortonTile> mortonTiles;
				mortonTiles.reserve( tilesX * tilesY );

				for( unsigned int ty = 0; ty < tilesY; ty++ )
				{
					for( unsigned int tx = 0; tx < tilesX; tx++ )
					{
						const unsigned int left   = startx + tx * tileSize;
						const unsigned int top    = starty + ty * tileSize;
						unsigned int right  = left + tileSize - 1;
						unsigned int bottom = top + tileSize - 1;

						// Clamp to image bounds
						if( right > endx )   right = endx;
						if( bottom > endy )  bottom = endy;

						MortonTile mt = { MortonCode( tx, ty ), Rect( top, left, bottom, right ) };
						mortonTiles.push_back( mt );
					}
				}

				// Sort tiles by Morton code for Z-order traversal
				std::sort( mortonTiles.begin(), mortonTiles.end(),
					[]( const MortonTile& a, const MortonTile& b ) {
						return a.mortonCode < b.mortonCode;
					}
				);

				// Extract sorted rects
				tiles.reserve( mortonTiles.size() );
				for( size_t i = 0; i < mortonTiles.size(); i++ ) {
					tiles.push_back( mortonTiles[i].rect );
				}

				cur = 0;
			}

			virtual int NumRegions()
			{
				return static_cast<int>(tiles.size());
			}

			Rect GetNextRegion()
			{
				return tiles[cur++];
			}

			void End()
			{
				tiles.clear();
			}
		};
	}
}

#endif
