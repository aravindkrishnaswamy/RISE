//////////////////////////////////////////////////////////////////////
//
//  DiskBuffer.h - This is a utility class that only implements
//    the functions in the IBuffer interface.  It is used as an
//    implementation helper for DiskFileReadBuffer and 
//    DiskFileWriteBuffer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../Interfaces/IBuffer.h"
#include "Reference.h"
#include <cstdio>
#include <string>
#include <memory>

#ifndef DISK_BUFFER_
#define DISK_BUFFER_

namespace RISE
{
	namespace Implementation
	{
		class DiskBuffer : public virtual IBuffer, public virtual Reference
		{
		public:
			//
			// These methods are to satisfy the IBuffer interface
			//
			virtual unsigned int HowFarToEnd( ) const;
			virtual unsigned int Size( ) const;
			virtual bool EndOfBuffer( ) const;

			virtual bool seek( const eSeek type, const int amount );
			virtual unsigned int getCurPos( ) const;

		protected:
			// Default destructor
			DiskBuffer();
			virtual ~DiskBuffer();

			struct CloseFile {
				void operator()( FILE* file ) const { std::fclose( file ); }
			};
			std::unique_ptr<FILE, CloseFile> hFile;
			std::string filename;			///< Name of the file
		};
	}
}

#endif

