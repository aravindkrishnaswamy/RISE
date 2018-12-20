//////////////////////////////////////////////////////////////////////
//
//  DiskFileWriteBuffer.h - Defines a buffer that write to a file on
//    disk
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

#include "../Interfaces/IWriteBuffer.h"
#include "DiskBuffer.h"
#include "Reference.h"

#ifndef DISKFILEWRITE_BUFFER_
#define DISKFILEWRITE_BUFFER_

namespace RISE
{
	namespace Implementation
	{
		class DiskFileWriteBuffer : public virtual IWriteBuffer, public virtual DiskBuffer
		{
		public:
			//
			// Constructors
			//

			// There is only one constructor
			DiskFileWriteBuffer( const char * file_name );

			//
			// This is for the IWriteBuffer interface
			//

			bool setChar( const char ch );
			bool setUChar( const unsigned char ch );

			bool setWord( const short sh );
			bool setUWord( const unsigned short sh );

			bool setInt( const int n );
			bool setUInt( const unsigned int n );

			bool setFloat( const float f );
			bool setDouble( const double d );

			bool setBytes( const void* pSource, unsigned int amount );


			void Clear( );
			bool Resize( unsigned int new_size, bool bForce );
			bool ResizeForMore( unsigned int more_bytes );

			bool ReadyToWrite() const;


		protected:
			// Default destructor
			virtual ~DiskFileWriteBuffer( );
		};
	}
}

#endif

