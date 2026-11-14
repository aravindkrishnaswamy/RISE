//////////////////////////////////////////////////////////////////////
//
//  DiskFileWriteBuffer.cpp - Implementation of the disk file write
//    buffer
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

#include "pch.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <process.h>
#include <io.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif
#include <string.h>
#include "DiskFileWriteBuffer.h"
#include "LittleEndian.h"
#include <cstdint>
#include <exception>
#include <cerrno>
#include "../Interfaces/ILog.h"
#include "MediaPathLocator.h"

using namespace RISE::Implementation;

#ifndef _WIN32
namespace
{
	bool IsTrustedStagingDirectory( int descriptor, std::error_code& error )
	{
		struct stat info{};
		if( ::fstat(descriptor, &info) != 0 ) {
			error = std::error_code(errno, std::generic_category());
			return false;
		}
		// A sticky directory owned by this user or root protects our new entry
		// from other users. A writable nonsticky directory cannot protect it.
		if( !S_ISDIR(info.st_mode) || (info.st_uid != ::geteuid() && info.st_uid != 0) ||
			((info.st_mode & 0022) != 0 && (info.st_mode & S_ISVTX) == 0) ) {
			error = std::make_error_code(std::errc::permission_denied);
			return false;
		}
#ifdef __APPLE__
		// Extended ALLOW entries can grant deletion despite mode 0700, and
		// inheritable entries can expose a newly created staging directory.
		// Keep deny-only ACLs; never rewrite a caller's directory ACL.
		acl_t acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
		if( !acl ) {
			// Darwin uses ENOENT when this opened object has no extended ACL.
			if( errno == ENOENT ) return true;
			error = std::error_code(errno, std::generic_category());
			return false;
		}
		if( ::acl_valid(acl) != 0 ) {
			error = std::error_code(errno, std::generic_category());
			::acl_free(acl);
			return false;
		}
		for( int entryId = ACL_FIRST_ENTRY; ; entryId = ACL_NEXT_ENTRY ) {
			acl_entry_t entry;
			errno = 0;
			if( ::acl_get_entry(acl, entryId, &entry) != 0 ) {
				const int failure = errno;
				::acl_free(acl);
				// Darwin reports end of a valid ACL as EINVAL.
				if( failure == EINVAL ) break;
				error = std::error_code(failure, std::generic_category());
				return false;
			}
			acl_tag_t tag;
			if( ::acl_get_tag_type(entry, &tag) != 0 ) {
				error = std::error_code(errno, std::generic_category());
				::acl_free(acl);
				return false;
			}
			if( tag != ACL_EXTENDED_DENY ) {
				::acl_free(acl);
				error = std::make_error_code(std::errc::permission_denied);
				return false;
			}
		}
#endif
		return true;
	}

	bool CheckStagingParents( const std::filesystem::path& parent, std::error_code& error )
	{
		// Later staging operations use this canonical path. Check every ancestor
		// too: an unsafe ancestor could otherwise let another user substitute an
		// apparently private child directory after we inspect it.
		for( auto directory = parent; ; directory = directory.parent_path() ) {
			const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if( descriptor < 0 ) {
				error = std::error_code(errno, std::generic_category());
				return false;
			}
			const bool trusted = IsTrustedStagingDirectory(descriptor, error);
			const int closed = ::close(descriptor);
			if( !trusted ) return false;
			if( closed != 0 ) {
				error = std::error_code(errno, std::generic_category());
				return false;
			}
			if( directory == directory.root_path() ) return true;
		}
	}
}
#endif

DiskFileWriteBuffer::DiskFileWriteBuffer( const char* file_name ) :
	DiskFileWriteBuffer(file_name, nullptr, false)
{
}

DiskFileWriteBuffer::DiskFileWriteBuffer( const char* file_name, std::error_code* openFailure, bool exclusiveCreate )
{
	if( openFailure ) openFailure->clear();
	if( !file_name ) return;
	filename = file_name;
	int openError = 0;
	if( exclusiveCreate ) {
		// The staging leaf must be newly created. Never truncate or follow an
		// entry planted between directory creation and this open. POSIX uses a
		// private directory and 0666 & ~umask to preserve new-output modes.
#ifdef _WIN32
		const int descriptor = ::_open(filename.c_str(),
			_O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
		if( descriptor >= 0 ) {
			hFile.reset(::_fdopen(descriptor, "wb"));
			if( !hFile ) {
				openError = errno;
				::_close(descriptor);
			}
		} else openError = errno;
#else
		const int descriptor = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
		if( descriptor >= 0 ) {
			hFile.reset(::fdopen(descriptor, "wb"));
			if( !hFile ) {
				openError = errno;
				::close(descriptor);
			}
		} else openError = errno;
#endif
	} else {
		hFile.reset(fopen(filename.c_str(), "wb"));
		if( !hFile ) openError = errno;
	}

	if( !hFile ) {
		writeError = std::error_code(openError, std::generic_category());
		if( openFailure ) *openFailure = writeError;
		GlobalLog()->PrintEx( eLog_Warning, "DiskFileWriteBuffer:: Failed to open file '%s': %s", file_name, writeError.message().c_str() );
	}
}

DiskFileWriteBuffer::~DiskFileWriteBuffer( )
{
	if( hFile ) {
		Finish();
	}
}

bool DiskFileWriteBuffer::setChar( const char ch )
{
	return setBytes( &ch, sizeof(ch) );
}

bool DiskFileWriteBuffer::setUChar( const unsigned char ch )
{
	return setBytes( &ch, sizeof(ch) );
}

bool DiskFileWriteBuffer::setWord( const short sh )
{
	unsigned char bytes[sizeof(std::uint16_t)];
	EncodeLittleEndian<std::uint16_t>( bytes, sh );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setUWord( const unsigned short sh )
{
	unsigned char bytes[sizeof(std::uint16_t)];
	EncodeLittleEndian<std::uint16_t>( bytes, sh );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setInt( const int n )
{
	unsigned char bytes[sizeof(std::uint32_t)];
	EncodeLittleEndian<std::uint32_t>( bytes, n );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setUInt( const unsigned int n )
{
	unsigned char bytes[sizeof(std::uint32_t)];
	EncodeLittleEndian<std::uint32_t>( bytes, n );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setFloat( const float f )
{
	unsigned char bytes[sizeof(std::uint32_t)];
	EncodeLittleEndian<std::uint32_t>( bytes, f );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setDouble( const double d )
{
	unsigned char bytes[sizeof(std::uint64_t)];
	EncodeLittleEndian<std::uint64_t>( bytes, d );
	return setBytes( bytes, sizeof(bytes) );
}

bool DiskFileWriteBuffer::setBytes( const void* pSource, unsigned int amount )
{
	if( !pSource || !hFile || ferror( hFile.get() ) ) {
		writeFailed = true;
		if( !writeError ) writeError = std::make_error_code(!pSource ? std::errc::invalid_argument : std::errc::io_error);
		return false;
	}
	if( amount > std::numeric_limits<std::uint64_t>::max() - cursor ) {
		writeFailed = true;
		writeError = std::make_error_code(std::errc::value_too_large);
		return false;
	}
	errno = 0;
	const size_t written = fwrite( pSource, 1, amount, hFile.get() );
	if( written != amount && !writeError ) writeError = errno ? std::error_code(errno, std::generic_category()) : std::make_error_code(std::errc::io_error);
	cursor += written;
	logicalSize = std::max( logicalSize, cursor );
	hasWritten |= written != 0;
	writeFailed |= written != amount;
	return written == amount;
}

unsigned int DiskFileWriteBuffer::Size() const
{
	return static_cast<unsigned int>(std::min<std::uint64_t>(logicalSize, std::numeric_limits<unsigned int>::max()));
}

unsigned int DiskFileWriteBuffer::getCurPos() const
{
	return static_cast<unsigned int>(std::min<std::uint64_t>(cursor, std::numeric_limits<unsigned int>::max()));
}

bool DiskFileWriteBuffer::seek( eSeek type, int amount )
{
	return seek64( type, amount );
}

bool DiskFileWriteBuffer::seek64( eSeek type, std::int64_t amount )
{
	errno = 0;
	if( !DiskBuffer::seek64( type, amount ) ) {
		writeFailed = true;
		if( !writeError ) writeError = errno ? std::error_code(errno, std::generic_category()) : std::make_error_code(std::errc::io_error);
		return false;
	}
	const auto position = DiskBuffer::getCurPos64();
	if( position == std::numeric_limits<std::uint64_t>::max() ) {
		writeFailed = true;
		if( !writeError ) writeError = std::make_error_code(std::errc::value_too_large);
		return false;
	}
	cursor = static_cast<std::uint64_t>(position);
	return true;
}

void DiskFileWriteBuffer::Clear( )
{
	// Does nothing
	GlobalLog()->PrintEasyWarning( "DiskFileWriteBuffer::Clear called, does nothing, you sure about this?" );
}


bool DiskFileWriteBuffer::Resize( unsigned int , bool )
{
	// We never resize
	return false;
}

bool DiskFileWriteBuffer::ResizeForMore( unsigned int )
{
	return false;
}

bool DiskFileWriteBuffer::ReadyToWrite() const
{
	return static_cast<bool>(hFile);
}

bool DiskFileWriteBuffer::Finish( bool requireOutput )
{
	if( !hFile ) {
		if( !writeError ) writeError = std::make_error_code(std::errc::bad_file_descriptor);
		return false;
	}
	const bool written = !writeFailed && !ferror( hFile.get() ) && (!requireOutput || hasWritten);
	if( !written && !writeError ) writeError = std::make_error_code(std::errc::io_error);
	errno = 0;
	const bool closed = fclose( hFile.release() ) == 0;
	if( !closed && !writeError ) writeError = errno ? std::error_code(errno, std::generic_category()) : std::make_error_code(std::errc::io_error);
	if( !written || !closed ) {
		GlobalLog()->PrintEx( eLog_Error, "DiskFileWriteBuffer:: Failed to write '%s': %s", filename.c_str(), writeError.message().c_str() );
		return false;
	}
	return true;
}

bool DiskFileWriteBuffer::WriteAtomically( const std::string& path,
	const std::function<bool(DiskFileWriteBuffer&)>& write )
{
	return WriteAtomically( path, write, StagingPolicy::Required );
}

bool DiskFileWriteBuffer::WriteAtomically( const std::string& path,
	const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy )
{
	return WriteAtomically(path, write, policy, nullptr);
}

bool DiskFileWriteBuffer::WriteAtomically( const std::string& path,
	const std::function<bool(DiskFileWriteBuffer&)>& write, StagingPolicy policy,
	WriteDiagnostics* diagnostics )
{
	namespace fs = std::filesystem;
	WriteDiagnostics result;
	struct Report {
		WriteDiagnostics& result;
		WriteDiagnostics* destination;
		~Report() { if( destination ) *destination = result; }
	} report{ result, diagnostics };
	const auto fail = [&]( const char* operation, const fs::path& file, std::error_code failure ) {
		if( !failure ) failure = std::make_error_code(std::errc::io_error);
		result.operation = operation;
		result.error = failure;
		GlobalLog()->PrintEx(eLog_Error, "DiskFileWriteBuffer:: %s '%s' failed: %s (%d)",
			operation, file.string().c_str(), failure.message().c_str(), failure.value());
		return false;
	};
	const auto writeFile = [&]( const fs::path& file, const std::function<bool(DiskFileWriteBuffer&)>& encode,
		std::error_code* openFailure = nullptr, bool exclusiveCreate = false ) {
		const auto release = []( DiskFileWriteBuffer* buffer ) { buffer->release(); };
		std::unique_ptr<DiskFileWriteBuffer, decltype(release)> buffer(
			new DiskFileWriteBuffer( file.string().c_str(), openFailure, exclusiveCreate ), release );
		GlobalLog()->PrintNew( buffer.get(), __FILE__, __LINE__, "disk file write buffer" );
		if( !buffer->ReadyToWrite() ) return fail("open output", file, buffer->writeError);
		bool encoded = false;
		try {
			encoded = encode( *buffer );
		} catch( const std::exception& failure ) {
			GlobalLog()->PrintEx( eLog_Error, "DiskFileWriteBuffer:: Encoding `%s` failed: %s", file.string().c_str(), failure.what() );
		}
		const bool finished = buffer->Finish( true );
		if( !finished ) return fail("finish output", file, buffer->writeError);
		if( !encoded ) return fail("encode output", file, std::make_error_code(std::errc::io_error));
		result.operation.clear();
		result.error.clear();
		return true;
	};
	const auto permissionFailure = []( const std::error_code& failure ) {
		return failure == std::errc::permission_denied || failure == std::errc::operation_not_permitted;
	};
	static std::atomic<unsigned long long> nextId{ 0 };
#ifdef _WIN32
	const auto pid = ::_getpid();
#else
	const auto pid = ::getpid();
#endif
	fs::path target( path );
	std::error_code error;
	const auto link = fs::symlink_status( target, error );
	if( error && error != std::errc::no_such_file_or_directory ) return fail("inspect destination", target, error);
	if( fs::is_symlink( link ) ) {
		target = fs::canonical( target, error );
		if( error ) {
			if( error == std::errc::no_such_file_or_directory && policy == StagingPolicy::AllowInPlace )
				return writeFile(fs::path(path), write);
			return fail("resolve destination symlink", path, error);
		}
	}
	const auto previous = fs::status( target, error );
	if( error && error != std::errc::no_such_file_or_directory ) return fail("inspect destination", target, error);
	const bool replacing = fs::exists( previous );
	if( replacing ) {
		if( !fs::is_regular_file( previous ) ) return fail("inspect regular destination", target, std::make_error_code(std::errc::invalid_argument));
		// Probe write access without requiring read permission or changing bytes.
		// An existing-only descriptor also avoids creating a target if it vanishes.
#ifdef _WIN32
		const int probe = ::_open(target.string().c_str(), _O_WRONLY | _O_BINARY | _O_NOINHERIT);
		if( probe < 0 ) return fail("probe destination write access", target, std::error_code(errno, std::generic_category()));
		if( ::_close(probe) != 0 ) return fail("close destination probe", target, std::error_code(errno, std::generic_category()));
#else
		const int probe = ::open(target.string().c_str(), O_WRONLY | O_CLOEXEC);
		if( probe < 0 ) return fail("probe destination write access", target, std::error_code(errno, std::generic_category()));
		if( ::close(probe) != 0 ) return fail("close destination probe", target, std::error_code(errno, std::generic_category()));
#endif
	}

#ifndef _WIN32
	// Automatic temporary names are safe only beneath trusted directories.
	// Explicit image output can retain its historical direct-write semantics
	// when staging is unsafe; Required must fail before invoking the encoder.
	const auto parent = fs::canonical(target.has_parent_path() ? target.parent_path() : fs::path("."), error);
	if( error || !CheckStagingParents(parent, error) ) {
		if( policy == StagingPolicy::AllowInPlace && permissionFailure(error) ) return writeFile(target, write);
		return fail("trust staging parent", target.parent_path(), error);
	}
	target = parent / target.filename();
#endif
	error.clear();
	fs::path directory;
	for( unsigned int attempt = 0; attempt < 16; ++attempt ) {
		directory = target.parent_path() / (".rise-write-" + std::to_string( pid ) + "-" +
			std::to_string( nextId.fetch_add( 1, std::memory_order_relaxed ) ));
#ifdef _WIN32
		if( fs::create_directory(directory, error) ) break;
#else
		// Restrict access in the creation syscall: chmod afterward would leave
		// a window in which a permissive umask exposes the staging leaf.
		if( ::mkdir(directory.c_str(), 0700) == 0 ) {
			error.clear();
			break;
		}
		error = std::error_code(errno, std::generic_category());
#endif
		directory.clear();
		if( error && error != std::errc::file_exists ) break;
	}
	if( directory.empty() ) {
		if( policy == StagingPolicy::AllowInPlace && permissionFailure(error) ) return writeFile(target, write);
		return fail("create staging directory", target.parent_path(), error ? error : std::make_error_code(std::errc::file_exists));
	}

	const fs::path staging = directory / "output";
	struct Cleanup {
		const fs::path& file;
		const fs::path& directory;
		WriteDiagnostics& result;
		bool retain = false;
		~Cleanup() {
			std::error_code error;
			if( retain ) {
				result.recoveryPath = fs::absolute(file, error).string();
				if( error ) result.recoveryPath = file.string();
				GlobalLog()->PrintEx(eLog_Error, "DiskFileWriteBuffer:: Completed output retained for recovery at '%s'", result.recoveryPath.c_str());
				return;
			}
			fs::remove(file, error);
			fs::path remaining = file;
			if( !error ) {
				fs::remove(directory, error);
				remaining = directory;
			}
			if( error ) {
				result.cleanupError = error;
				std::error_code absoluteError;
				result.cleanupPath = fs::absolute(remaining, absoluteError).string();
				if( absoluteError ) result.cleanupPath = remaining.string();
				GlobalLog()->PrintEx(eLog_Warning, "DiskFileWriteBuffer:: Could not remove staging path '%s': %s (%d)",
					result.cleanupPath.c_str(), error.message().c_str(), error.value());
			}
		}
	} cleanup{ staging, directory, result };
	std::error_code stagingOpenFailure;
	if( !writeFile( staging, write, &stagingOpenFailure, true ) ) {
		// No callback ran if opening the staging file failed. Permission-only
		// fallback is safe here; encoding/flush failures must never replay it.
		if( policy == StagingPolicy::AllowInPlace && permissionFailure(stagingOpenFailure) ) return writeFile(target, write);
		return false;
	}
	// Retain a complete encoded image if publication fails. Never replay the
	// encoder after this point, including failure while closing an in-place copy.
	cleanup.retain = true;
	bool copyInPlace = replacing && policy == StagingPolicy::AllowInPlace;
	if( replacing && !copyInPlace ) {
		fs::permissions( staging, previous.permissions(), error );
		if( error ) return fail("set staging permissions", staging, error);
	}
	if( !copyInPlace ) {
		fs::rename(staging, target, error);
		if( !error ) {
			cleanup.retain = false;
			return true;
		}
		copyInPlace = policy == StagingPolicy::AllowInPlace &&
			(permissionFailure(error) || error == std::errc::device_or_resource_busy);
		if( !copyInPlace ) return fail("rename staging output", target, error);
	}

	// The encoder has already succeeded. Copy its completed bytes rather than
	// invoking a potentially stateful callback a second time.
	// In-place publication keeps hardlinks, owner, ACLs and xattrs attached to
	// the existing inode, including writable files whose mount forbids rename.
	fs::permissions(staging, fs::perms::owner_read, fs::perm_options::add, error);
	if( error ) return fail("make staged output readable", staging, error);
	std::unique_ptr<FILE, decltype(&fclose)> input(fopen(staging.string().c_str(), "rb"), &fclose);
	if( !input ) return fail("open staged output for copying", staging, std::error_code(errno, std::generic_category()));
	std::error_code readFailure;
	const bool copied = writeFile( target, [&]( DiskFileWriteBuffer& output ) {
		unsigned char bytes[65536];
		for( ;; ) {
			errno = 0;
			const auto count = fread(bytes, 1, sizeof(bytes), input.get());
			if( ferror(input.get()) ) {
				readFailure = errno ? std::error_code(errno, std::generic_category()) : std::make_error_code(std::errc::io_error);
				return false;
			}
			if( count && !output.setBytes(bytes, static_cast<unsigned int>(count)) ) return false;
			if( count != sizeof(bytes) ) return feof(input.get()) && !ferror(input.get());
		}
	} );
	if( readFailure ) return fail("read staged output for copying", staging, readFailure);
	if( !copied ) return false;
	cleanup.retain = false;
	return true;
}
