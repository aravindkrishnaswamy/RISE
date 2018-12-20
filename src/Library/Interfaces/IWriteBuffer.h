//////////////////////////////////////////////////////////////////////
//
//  IWriteBuffer.h - Interface to a write buffer, thus any buffer
//    wishing to provide write services must implement this interface
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 14, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_WRITE_BUFFER_
#define I_WRITE_BUFFER_

#include "IBuffer.h"

namespace RISE
{
	//! A Buffer that can be written to
	/// \sa IBuffer
	/// \sa IReadBuffer
	class IWriteBuffer : public virtual IBuffer
	{
	protected:
		IWriteBuffer() {};
		virtual ~IWriteBuffer() {};

	public:
		//
		// These are the methods any write buffer must implement
		//

		//
		// Setters - These setters set some kind of information into the
		//           buffer.  Note that the pointer will advance, the sets
		//           will fail in debug if its bigger than the buffer

		virtual bool setChar( const char ch ) = 0;
		virtual bool setUChar( const unsigned char ch ) = 0;

		virtual bool setWord( const short sh ) = 0;
		virtual bool setUWord( const unsigned short sh ) = 0;

		virtual bool setInt( const int n ) = 0;
		virtual bool setUInt( const unsigned int n ) = 0;

		virtual bool setFloat( const float f ) = 0;
		virtual bool setDouble( const double d ) = 0;

		// Copies from the pointer provided and into the buffer
		virtual bool setBytes( const void* pSource, unsigned int amount ) = 0;

		// Clears the buffer
		virtual void Clear() = 0;


		//
		// These are the resizing functions
		//

		//! This function tells the buffer to resize itself..
		//! Note that if the given size is smaller, the buffer doesn't resize
		//! Also, note that the memory cursor stays at the same location after
		//! resizing.  If we are told to resize, are smaller than before AND 
		//! are forced to resize AND the memory location is invalid, the memory
		//! location returns itself to the begining, so just be careful if you
		//! are using this
		/// \return TRUE if the buffer was actually resized, FALSE otherwise
		virtual bool Resize( 
			unsigned int new_size,						///< [in] New size of the buffer
			bool bForce = false							///< [in] Should recreation be forced?
			) = 0;

		// This function assures that there is enough room to write the given number of
		// bytes from the current pointer
		/// \return TRUE if the buffer was actually resized, FALSE otherwise
		virtual bool ResizeForMore( 
			unsigned int more_bytes						///< [in] Number of bytes to ensure
			) = 0;
	};
}

#endif
