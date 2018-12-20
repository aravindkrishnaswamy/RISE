//////////////////////////////////////////////////////////////////////
//
//  DiskFileReadBuffer.h - Defines a buffer that reads a file from 
//    disk.  This is a read-only buffer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2003
//  Tabs: 4
//  Comments:
//
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../Interfaces/IReadBuffer.h"
#include "DiskBuffer.h"
#include "Reference.h"

#ifndef DISKFILEREAD_BUFFER_
#define DISKFILEREAD_BUFFER_

namespace RISE
{
	namespace Implementation
	{
		class DiskFileReadBuffer : public virtual IReadBuffer, public virtual DiskBuffer
		{
		public:
			//
			// Constructors
			//

			// This is the only constructor
			DiskFileReadBuffer( const char * file_name );

			//
			// This is for the IReadBuffer interface
			//

			char getChar();
			unsigned char getUChar();

			short getWord();
			unsigned short getUWord();

			int getInt();
			unsigned int getUInt();

			float getFloat();
			double getDouble();

			bool getBytes( void* pDest, unsigned int amount );

			int getLine( char* pDest, unsigned int max );

		protected:
			// Default destructor
			virtual ~DiskFileReadBuffer( );

			unsigned int	nSize;		///< Size of the file we are reading from
		};
	}
}

#endif

