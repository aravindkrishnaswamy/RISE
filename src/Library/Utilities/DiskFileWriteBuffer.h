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
#include <functional>

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
			unsigned int Size() const override;
			unsigned int getCurPos() const override;
			bool seek( eSeek type, int amount ) override;

			//
			// This is for the IWriteBuffer interface
			//

			bool setChar( const char ch ) override;
			bool setUChar( const unsigned char ch ) override;

			bool setWord( const short sh ) override;
			bool setUWord( const unsigned short sh ) override;

			bool setInt( const int n ) override;
			bool setUInt( const unsigned int n ) override;

			bool setFloat( const float f ) override;
			bool setDouble( const double d ) override;

			bool setBytes( const void* pSource, unsigned int amount ) override;


			void Clear( ) override;
			bool Resize( unsigned int new_size, bool bForce ) override;
			bool ResizeForMore( unsigned int more_bytes ) override;

			bool ReadyToWrite() const;
			// Close the output and report buffered write errors.
			bool Finish( bool requireOutput = false );
			// Publish a completed, nonempty output beside the destination.
			static bool WriteAtomically( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write );


		protected:
			// Default destructor
			virtual ~DiskFileWriteBuffer( );

		private:
			bool hasWritten = false;
			bool writeFailed = false;
			unsigned int logicalSize = 0;
			unsigned int cursor = 0;
		};
	}
}

#endif

