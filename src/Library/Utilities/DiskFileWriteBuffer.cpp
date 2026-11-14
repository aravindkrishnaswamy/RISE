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

namespace
{
	thread_local bool failRecoveryStreamAdoption = false;
}

bool RISE::Implementation::ForTest_SetRecoveryStreamAdoptionFailure( bool enabled )
{
	const bool previous = failRecoveryStreamAdoption;
	failRecoveryStreamAdoption = enabled;
	return previous;
}

#ifdef _WIN32
namespace
{
#ifdef _MSC_VER
	using NativeFileStatus = struct __stat64;
#else
	using NativeFileStatus = struct _stat64;
#endif
}
#endif

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
		// Validate one connected tree from the root down. Reopening complete
		// paths from the leaf upward could inspect different trees if an unsafe
		// ancestor were substituted between checks. Descend only through the
		// descriptor of a directory that has already passed the trust policy.
		struct Directory {
			int descriptor;
			~Directory() { if( descriptor >= 0 ) ::close(descriptor); }
		} current{ ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
		if( current.descriptor < 0 ) {
			error = std::error_code(errno, std::generic_category());
			return false;
		}
		if( !IsTrustedStagingDirectory(current.descriptor, error) ) return false;
		for( const auto& component : parent.relative_path() ) {
			if( component.empty() ) continue;
			Directory next{ ::openat(current.descriptor, component.c_str(),
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) };
			if( next.descriptor < 0 ) {
				error = std::error_code(errno, std::generic_category());
				return false;
			}
			if( !IsTrustedStagingDirectory(next.descriptor, error) ) return false;
			const int closed = ::close(current.descriptor);
			current.descriptor = -1;
			if( closed != 0 ) {
				error = std::error_code(errno, std::generic_category());
				return false;
			}
			current.descriptor = next.descriptor;
			next.descriptor = -1;
		}
		const int closed = ::close(current.descriptor);
		current.descriptor = -1;
		if( closed != 0 ) {
			error = std::error_code(errno, std::generic_category());
			return false;
		}
		return true;
	}
}
#endif

DiskFileWriteBuffer::DiskFileWriteBuffer( const char* file_name ) :
	DiskFileWriteBuffer(file_name, nullptr, OpenPolicy::Direct)
{
}

DiskFileWriteBuffer::DiskFileWriteBuffer( const char* file_name, std::error_code* openFailure, OpenPolicy policy, bool* created )
{
	if( openFailure ) openFailure->clear();
	if( created ) *created = false;
	if( !file_name ) return;
	filename = file_name;
	int openError = 0;
	if( policy != OpenPolicy::Direct ) {
		const bool exclusive = policy == OpenPolicy::ExclusiveStaging || policy == OpenPolicy::ExclusiveRecovery;
		// Staging requires a newly created leaf. Regular destination opens
		// defer truncation until the opened descriptor has been validated.
		// POSIX staging uses a private directory and 0666 & ~umask to preserve
		// new-output modes.
#ifdef _WIN32
		const int flags = _O_WRONLY | _O_CREAT | _O_BINARY | _O_NOINHERIT |
			(exclusive ? _O_EXCL : 0);
		const int descriptor = ::_open(filename.c_str(), flags, _S_IREAD | _S_IWRITE);
		if( descriptor >= 0 ) {
			if( created && exclusive ) *created = true;
			if( policy == OpenPolicy::RegularOutput ) {
				NativeFileStatus info{};
				if( ::_fstat64(descriptor, &info) != 0 ) openError = errno;
				else if( (info.st_mode & _S_IFMT) != _S_IFREG ) openError = EINVAL;
			}
			if( !openError ) {
				if( policy == OpenPolicy::ExclusiveRecovery && failRecoveryStreamAdoption ) openError = ENOMEM;
				else {
					hFile.reset(::_fdopen(descriptor, "wb"));
					if( !hFile ) openError = errno;
				}
			}
			if( !hFile ) ::_close(descriptor);
			else if( policy == OpenPolicy::RegularOutput ) {
				const auto truncated = ::_chsize_s(descriptor, 0);
				if( truncated != 0 ) {
					openError = truncated;
					hFile.reset();
				}
			}
		} else openError = errno;
#else
		// Regular destinations may change type after the initial path probe.
		// Do not block on a FIFO or truncate anything before validating the
		// opened descriptor; preserve user-authored symlink destinations.
		const int flags = O_WRONLY | O_CREAT | O_CLOEXEC |
			(exclusive ? O_EXCL | O_NOFOLLOW : O_NONBLOCK);
		const int descriptor = ::open(filename.c_str(), flags, policy == OpenPolicy::ExclusiveRecovery ? 0600 : 0666);
		if( descriptor >= 0 ) {
			if( created && exclusive ) *created = true;
			if( policy == OpenPolicy::RegularOutput ) {
				struct stat info{};
				if( ::fstat(descriptor, &info) != 0 ) openError = errno;
				else if( !S_ISREG(info.st_mode) ) openError = EINVAL;
			}
			if( !openError ) {
				if( policy == OpenPolicy::ExclusiveRecovery && failRecoveryStreamAdoption ) openError = ENOMEM;
				else {
					hFile.reset(::fdopen(descriptor, "wb"));
					if( !hFile ) openError = errno;
				}
			}
			if( !hFile ) ::close(descriptor);
			else if( policy == OpenPolicy::RegularOutput && ::ftruncate(descriptor, 0) != 0 ) {
				openError = errno;
				hFile.reset();
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

bool DiskFileWriteBuffer::WriteNewFile( const std::string& path,
	const std::function<bool(DiskFileWriteBuffer&)>& write, WriteDiagnostics* diagnostics )
{
	namespace fs = std::filesystem;
	WriteDiagnostics result;
	struct Report {
		WriteDiagnostics& result;
		WriteDiagnostics* destination;
		~Report() { if( destination ) *destination = result; }
	} report{ result, diagnostics };
	fs::path target(path);
	const auto fail = [&]( const char* operation, std::error_code error ) {
		result.operation = operation;
		result.error = error ? error : std::make_error_code(std::errc::io_error);
		GlobalLog()->PrintEx(eLog_Error, "DiskFileWriteBuffer:: %s '%s' failed: %s (%d)", operation,
			target.string().c_str(), result.error.message().c_str(), result.error.value());
		return false;
	};
	std::error_code error;
#ifndef _WIN32
	const auto parent = fs::canonical(target.has_parent_path() ? target.parent_path() : fs::path("."), error);
	if( error || !CheckStagingParents(parent, error) ) return fail("trust automatic output parent", error);
	target = parent / target.filename();
#else
	target = fs::absolute(target, error);
	if( error ) return fail("locate automatic output", error);
#endif
	// No status/canonical lookup of the leaf: exclusive creation itself rejects
	// pre-existing files, symlinks and hardlinks without following or reusing them.
	const auto release = []( DiskFileWriteBuffer* buffer ) { buffer->release(); };
	std::unique_ptr<DiskFileWriteBuffer, decltype(release)> buffer(nullptr, release);
	const fs::path requested = target;
	static std::atomic<unsigned long long> nextRecoveryId{0};
	const auto removeCreatedOutput = [&] {
		fs::remove(target, error);
		if( error ) {
			result.cleanupPath = target.string();
			result.cleanupError = error;
			GlobalLog()->PrintEx(eLog_Warning, "DiskFileWriteBuffer:: Could not remove partial automatic output '%s': %s (%d)",
				result.cleanupPath.c_str(), error.message().c_str(), error.value());
		}
	};
	for( unsigned int attempt = 0; attempt < 16; ++attempt ) {
		if( attempt ) target = requested.parent_path() / (requested.stem().string() + "-" +
			std::to_string(nextRecoveryId.fetch_add(1, std::memory_order_relaxed) + 1) + requested.extension().string());
		bool created = false;
		buffer.reset(new DiskFileWriteBuffer(target.string().c_str(), nullptr, OpenPolicy::ExclusiveRecovery, &created));
		GlobalLog()->PrintNew(buffer.get(), __FILE__, __LINE__, "automatic recovery write buffer");
		if( buffer->ReadyToWrite() ) break;
		if( created ) {
			// Exclusive OS creation succeeded, but adopting that descriptor as
			// a FILE failed. Only this newly created candidate belongs to us.
			removeCreatedOutput();
			return fail("adopt automatic output stream", buffer->writeError);
		}
		if( buffer->writeError != std::errc::file_exists || attempt == 15 )
			return fail("create automatic output exclusively", buffer->writeError);
	}
	bool encoded = false;
	try {
		encoded = write(*buffer);
	} catch( const std::exception& failure ) {
		GlobalLog()->PrintEx(eLog_Error, "DiskFileWriteBuffer:: Encoding automatic output '%s' failed: %s",
			target.string().c_str(), failure.what());
	}
	const bool finished = buffer->Finish(true);
	if( !finished || !encoded ) {
		fail(finished ? "encode automatic output" : "finish automatic output", buffer->writeError);
		// This call created the leaf (beneath trusted parents on POSIX). An
		// open failure returns above, so old candidates are never removed.
		removeCreatedOutput();
		return false;
	}
	result.recoveryPath = target.string();
	return true;
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
		std::error_code* openFailure = nullptr, OpenPolicy openPolicy = OpenPolicy::RegularOutput ) {
		const auto release = []( DiskFileWriteBuffer* buffer ) { buffer->release(); };
		std::unique_ptr<DiskFileWriteBuffer, decltype(release)> buffer(
			new DiskFileWriteBuffer( file.string().c_str(), openFailure, openPolicy ), release );
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
		NativeFileStatus opened{};
		const int inspected = ::_fstat64(probe, &opened);
		const auto probeError = inspected != 0 ? std::error_code(errno, std::generic_category()) :
			((opened.st_mode & _S_IFMT) != _S_IFREG ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});
		if( probeError ) {
			::_close(probe);
			return fail("inspect opened destination", target, probeError);
		}
		if( ::_close(probe) != 0 ) return fail("close destination probe", target, std::error_code(errno, std::generic_category()));
#else
		const int probe = ::open(target.string().c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
		if( probe < 0 ) return fail("probe destination write access", target, std::error_code(errno, std::generic_category()));
		struct stat opened{};
		const int inspected = ::fstat(probe, &opened);
		const auto probeError = inspected != 0 ? std::error_code(errno, std::generic_category()) :
			(!S_ISREG(opened.st_mode) ? std::make_error_code(std::errc::invalid_argument) : std::error_code{});
		if( probeError ) {
			::close(probe);
			return fail("inspect opened destination", target, probeError);
		}
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
	if( !writeFile( staging, write, &stagingOpenFailure, OpenPolicy::ExclusiveStaging ) ) {
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
