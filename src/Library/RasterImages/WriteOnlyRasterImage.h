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

#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRasterImageWriter.h"
#include "../Utilities/PEL.h"
#include "../Utilities/Reference.h"

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
				if( pWriter ) {
					pWriter->EndWrite(); 
				}
				safe_release( pWriter );
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
					unsigned int startx = 0, endx = width-1, starty = 0, endy = height-1;
					if( rc ) {
						startx = rc->left;
						endx = rc->right;
						starty = rc->top;
						endy = rc->bottom;
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
				// Thats who we write to
				if( pWriter ) {
					pWriter->EndWrite();
					pWriter->release();
				}

				pWriter = pWriter_;

				if( pWriter ) {
					pWriter->addref();
					pWriter->BeginWrite( width, height );
				}
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

