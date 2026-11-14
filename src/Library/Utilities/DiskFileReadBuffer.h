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
			unsigned int Size() const override { return DiskBuffer::Size(); }
			std::uint64_t Size64() const override { return nSize; }

			//
			// This is for the IReadBuffer interface
			//

			char getChar() override;
			unsigned char getUChar() override;

			short getWord() override;
			unsigned short getUWord() override;

			int getInt() override;
			unsigned int getUInt() override;

			float getFloat() override;
			double getDouble() override;

			bool getBytes( void* pDest, unsigned int amount ) override;

			int getLine( char* pDest, unsigned int max ) override;

		protected:
			// Default destructor
			virtual ~DiskFileReadBuffer( );

			std::uint64_t	nSize;		///< Readable end, reduced when I/O exposes an earlier EOF.
		private:
			void RecordReadEnd();
		};
	}
}

#endif

