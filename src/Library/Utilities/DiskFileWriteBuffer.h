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
#include <cstdint>
#include <system_error>

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

			// Direct construction retains legacy stream/device output semantics.
			DiskFileWriteBuffer( const char * file_name );
			// Legacy IBuffer queries saturate at UINT_MAX for large files.
			unsigned int Size() const override;
			std::uint64_t Size64() const override { return logicalSize; }
			std::uint64_t getCurPos64() const override { return cursor; }
			unsigned int getCurPos() const override;
			bool seek( eSeek type, int amount ) override;
			bool seek64( eSeek type, std::int64_t amount ) override;

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
			enum class StagingPolicy { Required, AllowInPlace };
			struct WriteDiagnostics {
				std::string operation;
				std::error_code error;
				std::string recoveryPath;
				std::string cleanupPath;
				std::error_code cleanupError;
			};
			// Publish a completed, nonempty output beside the destination.
			static bool WriteAtomically( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write );
			// AllowInPlace copies completed staging bytes into existing files,
			// preserving their inode, hardlinks and metadata. New files use rename.
			// Callers must serialize writes to the same destination (including
			// hardlink aliases); in-place writes are not atomic or mutually locked.
			// Helper destination opens validate a regular file before truncating;
			// a concurrent FIFO replacement cannot block POSIX publication.
			// Permission-denied staging creation/open permits direct encoding.
			// POSIX staging also requires trusted parent/ancestor ownership and
			// permissions; macOS extended ALLOW ACLs require direct image output.
			// Required rejects these unsafe automatic staging locations.
			// Failed in-place publication retains completed staging bytes for recovery;
			// the destination may be partial. Required always publishes by rename.
			static bool WriteAtomically( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy );
			static bool WriteAtomically( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy,
				WriteDiagnostics* diagnostics );


		protected:
			// Default destructor
			virtual ~DiskFileWriteBuffer( );

		private:
			enum class OpenPolicy { Direct, ExclusiveStaging, RegularOutput };
			DiskFileWriteBuffer( const char* file_name, std::error_code* openFailure, OpenPolicy policy );
			bool hasWritten = false;
			bool writeFailed = false;
			std::error_code writeError;
			std::uint64_t logicalSize = 0;
			std::uint64_t cursor = 0;
		};
	}
}

#endif

