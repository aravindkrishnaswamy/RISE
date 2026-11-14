//////////////////////////////////////////////////////////////////////
//
//  WriteOnlyRasterImage.h - Definition of a raster image
//    that is write only and write all access down to a rasterimage
//    writer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 24, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WriteOnlyRasterImage_
#define WriteOnlyRasterImage_

#include <algorithm>
#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRasterImageWriter.h"
#include "../Utilities/PEL.h"
#include "../Utilities/Reference.h"
#include "../Interfaces/ILog.h"
#include <exception>
#include <memory>
#include <utility>

namespace RISE
{
	namespace Implementation
	{
		template< class ColorBase >
		class WriteOnlyRasterImage_Template : 
			public virtual IRasterImage, 
			public virtual Reference
		{
		protected:
			typedef Color_Template<ColorBase>		PEL;

			mutable IRasterImageWriter* pWriter;
			unsigned int width, height;

			virtual ~WriteOnlyRasterImage_Template( )
			{
				// Explicit DumpImage(nullptr) reports finalization failures to
				// the caller. Destruction must release the writer safely.
				try {
					FinishWriter();
				} catch( const std::exception& failure ) {
					GlobalLog()->PrintEx( eLog_Error,
						"WriteOnlyRasterImage: finalization failed: %s", failure.what() );
				} catch( ... ) {
					GlobalLog()->PrintEasyError( "WriteOnlyRasterImage: finalization failed" );
				}
			}

			void FinishWriter() const
			{
				const auto release = []( IRasterImageWriter* writer ) { writer->release(); };
				std::unique_ptr<IRasterImageWriter, decltype(release)> writer(
					std::exchange( pWriter, nullptr ), release );
				if( writer ) writer->EndWrite();
			}

		public:
			// Default Constructor
			WriteOnlyRasterImage_Template( 
				const unsigned int w, 
				const unsigned int h
				) : 
			pWriter( 0 ),
			width( w ),
			height( h )
			{
			};

			void Clear( const RISEColor& p, const Rect* rc )
			{
				if( pWriter ) {
					// Same inclusive-region contract as RISERasterImage::Clear:
					// endpoints are clamped to the image (UINT_MAX = "to the
					// edge"); empty images and wholly outside or inverted
					// regions write nothing.
					if( !width || !height ) return;
					unsigned int startx = 0, endx = width-1, starty = 0, endy = height-1;
					if( rc ) {
						if( rc->left > rc->right || rc->top > rc->bottom ||
							rc->left >= width || rc->top >= height ) return;
						startx = rc->left;
						endx = std::min( rc->right, width-1 );
						starty = rc->top;
						endy = std::min( rc->bottom, height-1 );
					}

					for( unsigned int j=starty; j<=endy; j++ ) {
						for( unsigned int i=startx; i<=endx; i++ ) {
							pWriter->WriteColor( p, i, j );
						}
					}
				}
			}

			void DumpImage( IRasterImageWriter* pWriter_ ) const
			{
				// Acquire the incoming reference first: it can be the same
				// writer currently held only by this image.
				const auto release = []( IRasterImageWriter* writer ) { writer->release(); };
				std::unique_ptr<IRasterImageWriter, decltype(release)> next( pWriter_, release );
				if( next ) next->addref();
				FinishWriter();
				if( next ) next->BeginWrite( width, height );
				pWriter = next.release();
			}

			void LoadImage( IRasterImageReader* )
			{
				// Unsupported
			}

			RISEColor GetPEL( const unsigned int x, const unsigned int y ) const
			{
				// Return nothing
				return RISEColor();
			}

			// For IWriteOnlyRasterImage
			void SetPEL( const unsigned int x, const unsigned int y, const RISEColor& p )
			{
				// Yay! the only work we do which is to write stuff
				if( pWriter ) {
					pWriter->WriteColor( p, x, y );
				}
			}

			inline unsigned int GetWidth( ) const
			{
				return width;
			}

			inline unsigned int GetHeight( ) const
			{
				return height;
			}
		};
	}


	typedef	Implementation::WriteOnlyRasterImage_Template<RISEColor> 	RISEWriteOnlyRasterImage;
}

#endif

