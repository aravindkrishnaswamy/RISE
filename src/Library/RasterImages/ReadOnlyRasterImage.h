//////////////////////////////////////////////////////////////////////
//
//  ReadOnlyRasterImage.h - Definition of a raster image
//    that is read only and passes all access down to a rasterimage
//    reader
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 24, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ReadOnlyRasterImage_
#define ReadOnlyRasterImage_

#include "../Interfaces/IRasterImage.h"
#include "../Interfaces/IRasterImageReader.h"
#include "../Utilities/PEL.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		template< class ColorBase >
		class ReadOnlyRasterImage_Template : 
			public virtual IRasterImage, 
			public virtual Reference
		{
		protected:
			typedef Color_Template<ColorBase>		PEL;

			IRasterImageReader* pReader;
			unsigned int width, height;

			virtual ~ReadOnlyRasterImage_Template( )
			{
				if( pReader ) {
					pReader->EndRead(); 
				}
				safe_release( pReader );
			}

		public:
			// Default Constructor
			ReadOnlyRasterImage_Template( ) : 
			pReader( 0 ),
			width( 0 ),
			height( 0 )
			{
			};

			void Clear( const RISEColor& p, const Rect* rc )
			{
				// Unsupported
			}

			void DumpImage( IRasterImageWriter* pWriter ) const
			{
				if( pWriter && pReader )
				{
					pWriter->BeginWrite( width, height );
					RISEColor color;
					for( unsigned int j=0; j<height; j++ ) {
						for( unsigned int i=0; i<width; i++ ) {
							pReader->ReadColor( color, i, j );
							pWriter->WriteColor( color, i, j );
						}
					}
			
					pWriter->EndWrite();
				}
			}

			void LoadImage( IRasterImageReader* pReader_ )
			{
				if( pReader ) {
					pReader->EndRead();
				}
				safe_release( pReader );
				pReader = pReader_;

				if( pReader )
				{
					pReader->addref();
					if( !pReader->BeginRead( width, height ) ){
						safe_release( pReader );
					}

					// Thats it!  we can now access this reader for its lifetime
				}
			}

			// For IReadOnlyRasterImage
			RISEColor GetPEL( const unsigned int x, const unsigned int y ) const
			{
				// Get the PEL directly from the underlying IRaster
				RISEColor ret;
				if( pReader ) {
					pReader->ReadColor( ret, x, y );
				}

				return ret;
			}

			void SetPEL( const unsigned int x, const unsigned int y, const RISEColor& p )
			{
				// Do nothing!  we are read only dammit!
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


	typedef	Implementation::ReadOnlyRasterImage_Template<RISEColor> 	RISEReadOnlyRasterImage;

}

#endif

