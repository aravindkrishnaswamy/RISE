//////////////////////////////////////////////////////////////////////
//
//  Volume.h - A 3D matrix of volume data
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef VOLUME_
#define VOLUME_

#include "../Interfaces/IVolume.h"
#include "../Utilities/Reference.h"
#include "../Utilities/MediaPathLocator.h"

namespace RISE
{
	template <class T>
	class Volume :
		public virtual IVolume,
		public virtual Implementation::Reference
	{
	protected:

		//
		// Protected member variables
		//
		T*	m_pData;
		int	m_nWidth;
		int	m_nHeight;
		int	m_nDepth;
		int	m_nSliceSize;

		int	m_nWidthOV2;
		int	m_nHeightOV2;
		int	m_nDepthOV2;

		Scalar m_OVMaxValue;

		//
		// Protected member functions
		//
		virtual ~Volume( )
		{
			safe_delete( m_pData );

			m_nWidth = 0;
			m_nHeight = 0;
			m_nDepth = 0;
		};

	public:
		//
		// Protected member functions
		//

		Volume(
			const char * szFilePattern, 
			unsigned int width, 
			unsigned int height,
			unsigned int zstart, 
			unsigned int zend 
			) : 
		m_pData( 0 ),
		m_nWidth( width ), 
		m_nHeight( height ),
		m_nDepth( zend-zstart+1 ),
		m_nSliceSize( width*height )
		{
			m_OVMaxValue = 1.0 / ( Scalar( 1 << (sizeof( T )*8) ) - 1.0 );

			m_pData = new T[m_nDepth*m_nHeight*m_nWidth];

			for( unsigned int i=zstart, cnt=0; i<=zend; i++, cnt++ )
			{
				char buffer[1024] = {0};

				sprintf( buffer, szFilePattern, i );
				FILE* f = fopen( GlobalMediaPathLocator().Find(buffer).c_str(), "rb" );

				if( f ) {
					fread( &m_pData[cnt*width*height], sizeof( T ), m_nWidth*m_nHeight, f );
					fclose( f );
				}
			}

			m_nWidthOV2 = m_nWidth>>1;
			m_nHeightOV2 = m_nHeight>>1;
			m_nDepthOV2 = m_nDepth>>1;
		}


		unsigned int Width( ) const
		{
			return m_nWidth;
		}

		unsigned int Height( ) const
		{
			return m_nHeight;
		}

		unsigned int Depth( ) const
		{
			return m_nDepth;
		}

		Scalar GetValue( const int x, const int y, const int z ) const
		{
			// Volumes are centered at the middle
			const int az = z + m_nDepthOV2;
			const int ay = y + m_nHeightOV2;
			const int ax = x + m_nWidthOV2;

			if( az >= m_nDepth || ay >= m_nHeight || ax >= m_nWidth || 
				az < 0 || ay < 0 || ax < 0 ) {
				return 0;
			}

			return Scalar(m_pData[az*m_nSliceSize+ay*m_nWidth+ax]) * m_OVMaxValue;
		}
	};
}

#endif

