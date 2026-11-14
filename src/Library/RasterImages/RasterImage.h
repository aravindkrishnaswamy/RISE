//////////////////////////////////////////////////////////////////////
//
//  RasterImage.h - Definition of an internal raster image
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 24, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RASTERIMAGE_
#define RASTERIMAGE_

#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IOneColorOperator.h"
#include "../Interfaces/ITwoColorOperator.h"
#include "../Utilities/PEL.h"
#include "../Utilities/Reference.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

namespace RISE
{
	namespace Implementation
	{
		template< class ColorBase >
		class RasterImage_Template : public virtual IRasterImage, public virtual Reference
		{
		protected:
			using PEL = Color_Template<ColorBase>;
			PEL* pData = nullptr;
			unsigned int width = 0;
			unsigned int height = 0;
			unsigned int stride = 0;

			virtual ~RasterImage_Template() = default;

		private:
			// A view retains the allocation it references even after its source
			// image is reloaded. Borrowed external pixels remain caller-owned.
			std::shared_ptr<PEL[]> storage;

			struct ReleaseSource {
				void operator()( RasterImage_Template* source ) const { source->release(); }
			};
			std::unique_ptr<RasterImage_Template, ReleaseSource> borrowedSource;

			void RetainBacking( RasterImage_Template* source )
			{
				storage = source->storage;
				if( !storage && source->pData ) {
					auto* owner = source->borrowedSource ? source->borrowedSource.get() : source;
					owner->addref();
					borrowedSource.reset( owner );
				}
			}

			static std::shared_ptr<PEL[]> Adopt( PEL* data )
			{
				if( !data ) return {};
				return std::shared_ptr<PEL[]>( data, []( PEL* pixels ) {
					GlobalLog()->PrintDelete( pixels, __FILE__, __LINE__ );
					delete [] pixels;
				} );
			}

			static std::shared_ptr<PEL[]> Allocate( unsigned int w, unsigned int h, unsigned int rowStride )
			{
				if( !w || !h ) return {};
				const size_t limit = std::numeric_limits<size_t>::max() / sizeof(PEL);
				if( w > limit || h - 1 > (limit - w) / rowStride )
					throw std::length_error( "Raster image dimensions exceed addressable storage" );
				auto pixels = Adopt( new PEL[static_cast<size_t>(h - 1) * rowStride + w] );
				GlobalLog()->PrintNew( pixels.get(), __FILE__, __LINE__, "raster pixels" );
				return pixels;
			}

			void Replace( std::shared_ptr<PEL[]> pixels, unsigned int w, unsigned int h )
			{
				storage = std::move(pixels);
				pData = storage.get();
				borrowedSource.reset();
				width = w;
				height = h;
				stride = w;
			}

		public:
			RasterImage_Template() = default;

			RasterImage_Template( unsigned int w, unsigned int h, const PEL& value )
			{
				Replace( Allocate( w, h, w ), w, h );
				if( pData ) std::fill_n( pData, static_cast<size_t>(w) * h, value );
			}

			RasterImage_Template( const RasterImage_Template& source ) :
				Reference( source ),
				width( source.width ), height( source.height ), stride( source.stride ),
				storage( Allocate( width, height, stride ) )
			{
				pData = storage.get();
				if( !pData ) return;
				for( unsigned int y = 0; y < height; ++y ) {
					const size_t offset = static_cast<size_t>(y) * stride;
					std::copy_n( source.pData + offset, width, pData + offset );
				}
			}

			RasterImage_Template( RasterImage_Template* source )
			{
				if( !source ) return;
				RetainBacking( source );
				pData = source->pData;
				width = source->width;
				height = source->height;
				stride = source->stride;
			}

			// View rectangles use an exclusive right and bottom, as before.
			RasterImage_Template( RasterImage_Template* source, const Rect& clip )
			{
				if( !source || !source->pData || clip.left >= clip.right || clip.top >= clip.bottom ||
					clip.right > source->width || clip.bottom > source->height ) return;
				RetainBacking( source );
				stride = source->stride;
				pData = source->pData + static_cast<size_t>(clip.top) * stride + clip.left;
				width = clip.right - clip.left;
				height = clip.bottom - clip.top;
			}

			RasterImage_Template( void* pixels, unsigned int w, unsigned int h, bool owns ) :
				pData( static_cast<PEL*>(pixels) ), width( pixels ? w : 0 ), height( pixels ? h : 0 ),
				stride( width ), storage( owns ? Adopt(pData) : std::shared_ptr<PEL[]>() )
			{
			}

			inline		PEL&	GetPel( const unsigned int x, const unsigned int y ) const
			{
				return pData[static_cast<size_t>(y)*stride+x];
			}

			inline		void SetPel( const unsigned int x, const unsigned int y, const PEL& p )
			{
				pData[static_cast<size_t>(y)*stride+x] = p;
			}

			inline		void	Clear( const RISEColor& p, const Rect* rc )
			{
				if( !width || !height ) return;
				PEL sep( p.base, p.a );
				
				unsigned int startx = 0, endx = width-1, starty = 0, endy = height-1;
				if( rc ) {
					startx = rc->left;
					endx = std::min( rc->right, width - 1 );
					starty = rc->top;
					endy = std::min( rc->bottom, height - 1 );
				}

				for( unsigned int j=starty; j<=endy; j++ ) {
					const size_t dataPtr = static_cast<size_t>(j)*stride;
					for( unsigned int i=startx; i<=endx; i++ ) {
						pData[dataPtr+i] = sep;
					}
				}
			}

			inline		void	Clear( const PEL& p, const int* f=0 )
			{
				size_t dataPtr = 0;
				for( unsigned int j=0; j<height; j++, dataPtr+=stride ) {
					for( unsigned int i=0; i<width; i++ ) {
						pData[dataPtr+i] = p;
					}
				}
			}

			void	DumpImage( IRasterImageWriter* pWriter ) const
			{
				if( pWriter )
				{
					pWriter->BeginWrite( width, height );
					size_t dataPtr = 0;
					for( unsigned int j=0; j<height; j++, dataPtr+=stride ) {
						for( unsigned int i=0; i<width; i++ ) {
							pWriter->WriteColor( pData[dataPtr+i], i, j );
						}
					}
			
					pWriter->EndWrite();
				}
			}

			void LoadImage( IRasterImageReader* reader )
			{
				if( !reader ) return;
				unsigned int w = 0, h = 0;
				if( !reader->BeginRead( w, h ) ) {
					if( !pData ) Replace( Allocate( 1, 1, 1 ), 1, 1 );
					return;
				}
				try {
					if( w != width || h != height ) Replace( Allocate( w, h, w ), w, h );
					for( unsigned int y = 0; y < height; ++y ) {
						const size_t offset = static_cast<size_t>(y) * stride;
						for( unsigned int x = 0; x < width; ++x ) {
							RISEColor color;
							reader->ReadColor( color, x, y );
							pData[offset+x] = color;
						}
					}
				} catch( ... ) {
					try { reader->EndRead(); } catch( ... ) {}
					throw;
				}
				reader->EndRead();
			}

			// For IRasterImage
			RISEColor	GetPEL( const unsigned int x, const unsigned int y ) const
			{
				return RISEColor( pData[static_cast<size_t>(y)*stride+x].base, pData[static_cast<size_t>(y)*stride+x].a );
			}

			void	SetPEL( const unsigned int x, const unsigned int y, const RISEColor& p )
			{
				pData[static_cast<size_t>(y)*stride+x] = Color_Template<ColorBase>( p.base, p.a );
			}

			inline unsigned int GetWidth( ) const
			{
				return width;
			}

			inline unsigned int GetHeight( ) const
			{
				return height;
			}

			inline unsigned int GetStride( ) const
			{
				return stride;
			}
		};
	}

	typedef	Implementation::RasterImage_Template<RISEPel>		RISERasterImage;
}

#endif
