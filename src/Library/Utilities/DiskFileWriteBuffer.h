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
#include <filesystem>
#include <system_error>

#ifndef DISKFILEWRITE_BUFFER_
#define DISKFILEWRITE_BUFFER_

namespace RISE
{
	namespace Implementation
	{
		// Thread-local fault hook for recovery-file FILE adoption regressions.
		// Returns the prior value for scoped restoration.
		bool ForTest_SetRecoveryStreamAdoptionFailure( bool enabled );
		// The POSIX staging-ancestor check on its own (true on Windows): an absolute canonical
		// directory passes when every ancestor is owner-only, or, under an opt-in trusted cache
		// root (Simulation::FindTrustedCacheRoot), when everything below the root is.
		bool ForTest_CheckStagingParents( const std::filesystem::path& parent, std::error_code& error );

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
			// Retained completed staging files request owner-read permission after
			// failed publication, without changing successful destination modes.
			// Adjustment failures are logged separately; primary diagnostics and
			// recoveryPath remain intact (WriteDiagnostics layout is unchanged).
			static bool WriteAtomically( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy );
			static bool WriteAtomically( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy,
				WriteDiagnostics* diagnostics );
			// Explicit UTF8 pathname opt-in. Existing string APIs retain native
			// pathname decoding. The same transaction/policies are used; returned
			// recoveryPath/cleanupPath and path diagnostics use UTF8 for this API.
			static bool WriteAtomicallyUTF8( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write,
				StagingPolicy policy = StagingPolicy::Required, WriteDiagnostics* diagnostics = nullptr );
			// Automatic recovery names must never resolve or overwrite existing
			// leaves. Try the requested name first; EEXIST tries a monotonic suffix
			// before its extension, up to 16 candidates. Other failures never retry.
			// POSIX requires trusted parents and creates with 0600 reduced by umask.
			// Success reports the actual absolute artifact in recoveryPath.
			static bool WriteNewFile( const std::string& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write, WriteDiagnostics* diagnostics = nullptr );


		protected:
			// Default destructor
			virtual ~DiskFileWriteBuffer( );

		private:
			enum class OpenPolicy { Direct, ExclusiveStaging, ExclusiveRecovery, RegularOutput };
			DiskFileWriteBuffer( const char* file_name, std::error_code* openFailure, OpenPolicy policy, bool* created = nullptr );
			DiskFileWriteBuffer( const std::filesystem::path& nativePath, const std::string& displayName,
				std::error_code* openFailure, OpenPolicy policy, bool* created = nullptr );
			void OpenNative( const std::filesystem::path& nativePath, const std::string& displayName,
				std::error_code* openFailure, OpenPolicy policy, bool* created );
			static bool WriteAtomicallyNative( const std::filesystem::path& path,
				const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy,
				WriteDiagnostics* diagnostics, bool utf8Diagnostics );
			bool hasWritten = false;
			bool writeFailed = false;
			std::error_code writeError;
			std::uint64_t logicalSize = 0;
			std::uint64_t cursor = 0;
		};
	}
}

#endif

