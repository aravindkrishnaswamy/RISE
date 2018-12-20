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

namespace RISE
{
	namespace Implementation
	{
		template< class ColorBase >
		class RasterImage_Template : public virtual IRasterImage, public virtual Reference
		{
		protected:
			typedef Color_Template<ColorBase>		PEL;
			PEL*									pData;
			unsigned int							width;
			unsigned int							height;
			unsigned int							stride;

			bool									bICreatedData;
			RasterImage_Template<ColorBase>*		pParent;			// pointer to parent raster image

			virtual ~RasterImage_Template( )
			{
				if( pData && bICreatedData ) {
					GlobalLog()->PrintDelete( pData, __FILE__, __LINE__ );
					delete [] pData;
				}

				safe_release( pParent );

				pData = 0;
				pParent = 0;
			}

		public:
			// Default Constructor
			RasterImage_Template( ) : 
			pData( 0 ),
			width( 0 ),
			height( 0 ),
			stride( 0 ),
			bICreatedData(false),
			pParent( 0 )
			{
			};

			// Constructor for creating a raster image of a defined size, with each Pel set to given color value
			RasterImage_Template( const unsigned int w, const unsigned int h, const PEL& p ) : 
			pData( 0 ),
			width( w ),
			height( h ),
			stride( w ),
			bICreatedData( true ),
			pParent( 0 )
			{
				if( width>0 && height>0 ) {
					pData = new PEL[width*height];
					GlobalLog()->PrintNew( pData, __FILE__, __LINE__, "data" );
				}

				unsigned int dataPtr = 0;

				for( unsigned int j=0; j<height; j++, dataPtr+=stride ) {
					for( unsigned int i=0; i<width; i++ ) {
						pData[dataPtr+i] = p;
					}
				}
			}

			// Copy constructor, NOTE: DATA IS COPIED in this instance!
			RasterImage_Template( const RasterImage_Template<ColorBase>& s ) : 
			pData( 0 ),
			width( s.width ),
			height( s.height ),
			stride( s.stride ),
			bICreatedData( true ),
			pParent( 0 )
			{
				pData = new PEL[width*height];
				GlobalLog()->PrintNew( pData, __FILE__, __LINE__, "data" );

				unsigned int dataPtr = 0;
				
				for( int j=0; j<height; j++, dataPtr+=stride ) {
					for( int i=0; i<width; i++ ) {
						pData[dataPtr+i] = s.pData[dataPtr+i];
					}
				}
			}

			// Copy constructor, NOTE: DATA IS NOT COPIED in this instance!
			RasterImage_Template( RasterImage_Template<ColorBase>* s ) : 
			pData( s->pData ),
			width( s->width ),
			height( s->height ),
			stride( s->stride ),
			bICreatedData( false ),
			pParent( s )
			{
				s->addref();
			}
			  
			// Clipped raster image, DATA IS NOT COPIED!
			RasterImage_Template( RasterImage_Template<ColorBase>* s, const Rect& clip ) : 
			pData( &s->pData[clip.top*s->stride+clip.left] ),
			width( clip.right-clip.left ),
			height( clip.bottom-clip.top ),
			stride( s->stride ),
			bICreatedData( false ),
			pParent( s )
			{
				// Do some sanity checks!
				s->addref();
			}

			// Constructor from a void*, this just makes an image out of some random bytes...
			RasterImage_Template( void* pBytes, const unsigned int w, const unsigned int h, bool bOwnData ) : 
			pData( (PEL*)pBytes ),
			width( w ),
			height( h ),
			stride( w ),
			bICreatedData( bOwnData ),
			pParent( 0 )
			{
			}

			inline		PEL&	GetPel( const unsigned int x, const unsigned int y ) const
			{
				return pData[y*stride+x];
			}

			inline		void SetPel( const unsigned int x, const unsigned int y, const PEL& p )
			{
				pData[y*stride+x] = p;
			}

			inline		void	Clear( const RISEColor& p, const Rect* rc )
			{
				PEL sep( p.base, p.a );
				
				unsigned int startx = 0, endx = width-1, starty = 0, endy = height-1;
				if( rc ) {
					startx = rc->left;
					endx = rc->right;
					starty = rc->top;
					endy = rc->bottom;
				}

				for( unsigned int j=starty; j<=endy; j++ ) {
					const unsigned int dataPtr = j*stride;
					for( unsigned int i=startx; i<=endx; i++ ) {
						pData[dataPtr+i] = sep;
					}
				}
			}

			inline		void	Clear( const PEL& p, const int* f=0 )
			{
				unsigned int dataPtr = 0;
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
					unsigned int dataPtr = 0;
					for( unsigned int j=0; j<height; j++, dataPtr+=stride ) {
						for( unsigned int i=0; i<width; i++ ) {
							pWriter->WriteColor( pData[dataPtr+i], i, j );
						}
					}
			
					pWriter->EndWrite();
				}
			}

			void	LoadImage( IRasterImageReader* pReader )
			{
				unsigned int w, h;
				
				if( pReader )
				{
					if( pReader->BeginRead( w, h ) )
					{
						if( w!=width || h!=height )
						{
							// Re-allocate ourselves				
							if( pData && bICreatedData ) {
								GlobalLog()->PrintDelete( pData, __FILE__, __LINE__ );
								delete [] pData;
								pData = 0;
							}
							
							safe_release( pParent );

							width = w;
							height = h;
							stride = width;
							bICreatedData = true;
							pData = new PEL[width*height];
							GlobalLog()->PrintNew( pData, __FILE__, __LINE__, "data" );
						}

						unsigned int dataPtr = 0;
						for( unsigned int j=0; j<height; j++, dataPtr+=stride ) {
							for( unsigned int i=0; i<width; i++ ) {
								RISEColor c;
								pReader->ReadColor( c, i, j );
								pData[dataPtr+i] = c;
							}
						}

						pReader->EndRead( );
					} 
					else if( !pData )
					{
						// Setup some dummy data
						width = 1;
						height = 1;
						bICreatedData = true;
						pData = new PEL[width*height];
						GlobalLog()->PrintNew( pData, __FILE__, __LINE__, "data" );
					}
				}
			}

			// For IRasterImage
			RISEColor	GetPEL( const unsigned int x, const unsigned int y ) const
			{
				return RISEColor( pData[y*stride+x].base, pData[y*stride+x].a );
			}

			void	SetPEL( const unsigned int x, const unsigned int y, const RISEColor& p )
			{
				pData[y*stride+x] = Color_Template<ColorBase>( p.base, p.a );
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
