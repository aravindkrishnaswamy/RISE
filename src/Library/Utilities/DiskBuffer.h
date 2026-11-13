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
		class DiskBuffer : public virtual IBuffer, public virtual Reference, public IBuffer64
		{
		public:
			//
			// These methods are to satisfy the IBuffer interface
			//
			unsigned int HowFarToEnd( ) const override;
			unsigned int Size( ) const override;
			bool EndOfBuffer( ) const override;

			bool seek( const eSeek type, const int amount ) override;
			unsigned int getCurPos( ) const override;
			std::uint64_t Size64() const override;
			std::uint64_t getCurPos64() const override;
			bool seek64( eSeek type, std::int64_t amount ) override;

		protected:
			// Default destructor
			DiskBuffer();
			~DiskBuffer() override;

			struct CloseFile {
				void operator()( FILE* file ) const { std::fclose( file ); }
			};
			std::unique_ptr<FILE, CloseFile> hFile;
			std::string filename;			///< Name of the file
		};
	}
}

#endif

