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
#include "../Interfaces/ILog.h"
#include "../Utilities/Reference.h"
#include "../Utilities/MediaPathLocator.h"
#include <limits>
#include <type_traits>

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
			delete [] m_pData;
			m_pData = 0;

			m_nWidth = 0;
			m_nHeight = 0;
			m_nDepth = 0;
		};

	public:
		//
		// Protected member functions
		//

		//! Construct an in-memory volume for painter-baked physical fields.
		//! Floating-point volumes store authored SI values directly; integer
		//! volumes retain the historical normalized [0,1] interpretation.
		Volume(
			unsigned int width,
			unsigned int height,
			unsigned int depth
			) :
		m_pData( 0 ),
		m_nWidth( width ),
		m_nHeight( height ),
		m_nDepth( depth ),
		m_nSliceSize( 0 ),
		m_nWidthOV2( 0 ),
		m_nHeightOV2( 0 ),
		m_nDepthOV2( 0 ),
		m_OVMaxValue( 1.0 )
		{
			static const unsigned long long kMaxVolumeBytes = 2ULL * 1024 * 1024 * 1024;
			const unsigned long long maxByBytes = kMaxVolumeBytes / sizeof( T );
			const unsigned long long maxVoxels = maxByBytes < 0x7fffffffULL ? maxByBytes : 0x7fffffffULL;
			const unsigned long long slice64 =
				static_cast<unsigned long long>( width ) * static_cast<unsigned long long>( height );
			const unsigned long long count64 = slice64 * static_cast<unsigned long long>( depth );
			if( width == 0 || height == 0 || depth == 0 ||
				slice64 > maxVoxels || count64 > maxVoxels ) {
				GlobalLog()->PrintEx( eLog_Error,
					"Volume:: Invalid or too-large in-memory volume dimensions %ux%ux%u (empty volume)",
					width, height, depth );
				m_nWidth = m_nHeight = m_nDepth = 0;
				return;
			}

			m_nSliceSize = static_cast<int>( slice64 );
			m_nWidthOV2 = m_nWidth >> 1;
			m_nHeightOV2 = m_nHeight >> 1;
			m_nDepthOV2 = m_nDepth >> 1;
			m_pData = new T[static_cast<size_t>( count64 )]();
			if( !std::is_floating_point<T>::value ) {
				m_OVMaxValue = 1.0 / Scalar( std::numeric_limits<T>::max() );
			}
		}

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

			// Guard the voxel-buffer sizing against a scene-driven integer
			// overflow: width/height/depth are scene parameters, so the 32-bit
			// `m_nDepth*m_nHeight*m_nWidth` product (and the per-slice
			// `width*height` fread offset, and m_nSliceSize) can wrap to a
			// tiny allocation the slice fill loop then writes past.  On failure
			// degrade to an EMPTY volume (all GetValue -> 0, m_pData null)
			// rather than allocating a wrapped-small buffer.
			//
			// IMPORTANT: validate the RAW UNSIGNED ctor params (width/height/
			// zstart/zend), NOT the int members m_nWidth/m_nHeight/m_nDepth --
			// those are narrowed from the unsigned params in the init list
			// above, so a dim >= 2^31 would be negative and `(unsigned long
			// long)m_nWidth` would SIGN-EXTEND to ~2^64, wrapping the product
			// back to a small value that sneaks past the guard.
			//
			// Two constraints: (a) the byte allocation stays under the ceiling,
			// and (b) the voxel COUNT fits a positive signed int, because
			// m_nSliceSize and the linear index `az*m_nSliceSize+ay*m_nWidth+ax`
			// in GetValue() are signed int -- a count of 2^31 would wrap
			// m_nSliceSize to INT_MIN and drive an OOB read.  Take the tighter
			// of the two (0x7fffffff == INT_MAX).
			static const unsigned long long kMaxVolumeBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB
			const unsigned long long maxByBytes = kMaxVolumeBytes / sizeof( T );
			const unsigned long long maxVoxels = maxByBytes < 0x7fffffffULL ? maxByBytes : 0x7fffffffULL;
			const unsigned long long slice64 = (unsigned long long)width * (unsigned long long)height;
			const bool depthValid = ( zend >= zstart );
			const unsigned long long depth64 =
				depthValid ? ( (unsigned long long)zend - (unsigned long long)zstart + 1ULL ) : 0ULL;
			if( width == 0 || height == 0 || !depthValid || depth64 == 0 ||
				slice64 > maxVoxels ||
				slice64 * depth64 > maxVoxels ) {
				GlobalLog()->PrintEx( eLog_Error,
					"Volume:: Invalid or too-large volume dimensions %ux%ux(z %u..%u) (empty volume)",
					width, height, zstart, zend );
				m_pData = 0;
				m_nWidth = m_nHeight = m_nDepth = 0;
				m_nSliceSize = 0;
				m_nWidthOV2 = m_nHeightOV2 = m_nDepthOV2 = 0;
				return;
			}

			// Value-initialise to zero: any slice whose file is missing or
			// short-reads then stays ZERO rather than exposing
			// uninitialised voxels (garbage from new[]).
			m_pData = new T[ (size_t)( slice64 * depth64 ) ]();

			for( unsigned int i=zstart, cnt=0; i<=zend; i++, cnt++ )
			{
				static const int MAX_BUFFER_SIZE = 1024;
				char buffer[MAX_BUFFER_SIZE] = {0};

				snprintf( buffer, MAX_BUFFER_SIZE, szFilePattern, i );
				FILE* f = fopen( GlobalMediaPathLocator().Find(buffer).c_str(), "rb" );

				if( f ) {
					const size_t want = static_cast<size_t>(m_nWidth) * static_cast<size_t>(m_nHeight);
					const size_t got  = fread( &m_pData[cnt*width*height], sizeof( T ), want, f );
					fclose( f );
					if( got < want ) {
						// Truncated slice: the unread remainder is already
						// zero from the value-init above.
						GlobalLog()->PrintEx( eLog_Error,
							"Volume:: Short read on slice `%s`: got %u of %u voxels (remainder zeroed)",
							buffer, static_cast<unsigned>(got), static_cast<unsigned>(want) );
					}
				} else {
					GlobalLog()->PrintEx( eLog_Error,
						"Volume:: Failed to open slice file `%s` (slice zeroed)", buffer );
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

		//! Set one centered-coordinate voxel in an in-memory volume.
		//! Returns false for an out-of-range coordinate or an empty volume.
		bool SetValue( const int x, const int y, const int z, const T value )
		{
			const int az = z + m_nDepthOV2;
			const int ay = y + m_nHeightOV2;
			const int ax = x + m_nWidthOV2;
			if( !m_pData || az >= m_nDepth || ay >= m_nHeight || ax >= m_nWidth ||
				az < 0 || ay < 0 || ax < 0 ) {
				return false;
			}
			m_pData[az*m_nSliceSize+ay*m_nWidth+ax] = value;
			return true;
		}
	};
}

#endif
