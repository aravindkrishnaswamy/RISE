//////////////////////////////////////////////////////////////////////
//
//  IReadBuffer.h - Interface to a read buffer, thus any buffer
//    wishing to provide read services must implement this interface
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

#ifndef I_READ_BUFFER_
#define I_READ_BUFFER_

#include "IBuffer.h"

namespace RISE
{
	//! A Buffer that can be read from
	/// \sa IBuffer
	/// \sa IWriteBuffer
	class IReadBuffer : public virtual IBuffer
	{
	protected:
		IReadBuffer() {};
		virtual ~IReadBuffer() {};

	public:
		//
		// These are the methods any read buffer must implement
		//

		//
		// Getters - These getters some some kind of information from the buffer
		//           All getters automatically advance the pointer, if you try to
		//			 read past the end, it will just return 0, null or some 
		//           equivalent in DEBUG
		//

		virtual char getChar() = 0;
		virtual unsigned char getUChar() = 0;

		virtual short getWord() = 0;
		virtual unsigned short getUWord() = 0;

		virtual int getInt() = 0;
		virtual unsigned int getUInt() = 0;

		virtual float getFloat() = 0;
		virtual double getDouble() = 0;

		//! Copies from the buffer to the pointer provided
		/// \return TRUE if the copy was successful, FALSE otherwise
		virtual bool getBytes( 
			void* pDest,									///< [out] Destination buffer to fill with data
			unsigned int amount								///< [in] Amount of bytes read
			) = 0;

		//! Reads a line of text (will read from the current point until a new line
		/// \return The number of characters read
		virtual int getLine( 
			char* pDest,									///< [out] Desination string
			unsigned int max								///< [in] Maximum number of characters to read
			) = 0;
	};
}

#endif
