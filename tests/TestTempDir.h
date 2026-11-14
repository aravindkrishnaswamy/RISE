//////////////////////////////////////////////////////////////////////
//
//  TestTempDir.h - Shared per-process temporary directory helpers and
//    cross-process collision-hazard verification for tests.
//
//    Two concurrent copies of a test binary (stray earlier run, manual
//    run alongside the suite, or a repeat-run flake-chase loop) must
//    never share a fixed scratch directory root where remove_all()
//    could delete another in-flight process's fixtures or results.
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_TESTS_TEST_TEMP_DIR_H
#define RISE_TESTS_TEST_TEMP_DIR_H

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
	#include <process.h>
#else
	#include <unistd.h>
	#include <sys/wait.h>
	#include <sys/stat.h>
	#include <fcntl.h>
	#include <cerrno>
	#include <vector>
#endif

namespace RISE
{
namespace TestSupport
{
	inline long CurrentProcessId()
	{
#ifdef _WIN32
		return static_cast<long>( _getpid() );
#else
		return static_cast<long>( getpid() );
#endif
	}
	inline std::string ScratchBaseDir()
	{
		const char* base = std::getenv( "TMPDIR" );
		if( !base || !*base ) base = std::getenv( "TEMP" );
		if( !base || !*base ) base = std::getenv( "TMP" );
		std::string dir;
		if( base && *base ) {
			dir = base;
		} else {
			std::error_code ec;
			const std::filesystem::path sysTmp = std::filesystem::temp_directory_path( ec );
			if( !ec && !sysTmp.empty() ) {
				dir = sysTmp.generic_string();
			} else {
				dir = ".";
			}
		}
		// Forward slashes throughout so paths can be embedded directly inside
		// JSON string literals and .RISEscene chunks without backslash escape issues.
		for( char& c : dir ) {
			if( c == '\\' ) c = '/';
		}
		while( dir.size() > 1 && dir.back() == '/' ) {
			if( dir.size() == 3 && dir[1] == ':' ) break;
			dir.pop_back();
		}
		return dir;
	}

	inline std::string ScratchPath( const std::string& leaf )
	{
		std::string dir = ScratchBaseDir();
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		if( !leaf.empty() && leaf.front() == '/' ) return dir + leaf.substr( 1 );
		return dir + leaf;
	}

	inline std::string ScratchTestRoot( const char* suitePrefix )
	{
		return ScratchPath( std::string( suitePrefix ) + std::to_string( CurrentProcessId() ) );
	}

	inline std::string ScratchRunDir( const char* suitePrefix, const char* leaf )
	{
		std::string dir = ScratchTestRoot( suitePrefix ) + "/" + leaf;
		std::error_code ec;
		std::filesystem::remove_all( dir, ec );
		return dir;
	}

	// Small same-filesystem probe before any multi-GiB hole is created. POSIX
	// does not guarantee sparse allocation; unsupported filesystems skip only
	// the large-file sections, keeping ordinary packet/codec coverage active.
	inline bool SupportsSparseFiles()
	{
#ifndef _WIN32
		static const bool supported = [] {
			std::string pattern = ScratchPath("rise_sparse_probe_XXXXXX");
			std::vector<char> name(pattern.begin(), pattern.end());
			name.push_back(0);
			const int fd = mkstemp(name.data());
			if( fd < 0 ) {
				std::printf("SKIP: cannot create scratch sparse-allocation probe\n");
				return false;
			}
			unlink(name.data());
			struct stat info{};
			constexpr off_t length = 1024 * 1024;
			const bool sparse = lseek(fd, length - 1, SEEK_SET) == length - 1 &&
				write(fd, "x", 1) == 1 && fsync(fd) == 0 && fstat(fd, &info) == 0 &&
				info.st_size == length && info.st_blocks * 512 < length / 4;
			close(fd);
			if( !sparse ) std::printf("SKIP: scratch filesystem does not support sparse fixtures\n");
			return sparse;
		}();
		return supported;
#else
		return false;
#endif
	}

	// A parent owns the directory so it can remove even a crashed child's
	// multi-GiB fixtures. Call before creating threads or constructing globals
	// with exit callbacks; the test body and its callbacks execute in the child.
	template<typename Body>
	inline int WithOwnedScratch( Body&& body )
	{
#ifndef _WIN32
		std::string pattern = ScratchPath("rise_owned_scratch_XXXXXX");
		std::vector<char> name(pattern.begin(), pattern.end());
		name.push_back(0);
		if( !mkdtemp(name.data()) ) return 1;
		std::fflush(nullptr);
		const auto child = fork();
		if( child == 0 ) {
			if( setenv("TMPDIR", name.data(), 1) != 0 ) _exit(1);
			std::exit(body());
		}
		int status = 0;
		pid_t waited = -1;
		if( child > 0 ) {
			do { waited = waitpid(child, &status, 0); } while( waited < 0 && errno == EINTR );
		}
		std::error_code error;
		// A failed permission fixture may leave child directories read-only.
		std::filesystem::permissions(name.data(), std::filesystem::perms::owner_all,
			std::filesystem::perm_options::add, error);
		for( std::filesystem::recursive_directory_iterator it(name.data(), error), end;
			!error && it != end; it.increment(error) ) {
			if( std::filesystem::is_directory(it->symlink_status(error)) )
				std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
					std::filesystem::perm_options::add, error);
		}
		if( !error ) std::filesystem::remove_all(name.data(), error);
		if( child < 0 || waited != child || error ) return 1;
		return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#else
		return body();
#endif
	}

	template<typename CheckFn>
	inline void VerifyScratchRootDiffersAcrossProcesses( const char* suitePrefix, CheckFn&& check )
	{
#ifndef _WIN32
		std::printf( "Collision-hazard check: ScratchTestRoot() differs across processes...\n" );
		int fds[2];
		if( pipe( fds ) != 0 ) { check( false, "collision check: pipe() succeeded" ); return; }
		const pid_t child = fork();
		if( child < 0 ) {
			check( false, "collision check: fork() succeeded" );
			close( fds[0] ); close( fds[1] );
			return;
		}
		if( child == 0 ) {
			close( fds[0] );
			const std::string childRoot = ScratchTestRoot( suitePrefix );
			ssize_t w = write( fds[1], childRoot.data(), childRoot.size() );
			(void)w;
			close( fds[1] );
			_exit( 0 );
		}
		close( fds[1] );
		std::string childRoot;
		char buf[4096]; ssize_t n;
		while( ( n = read( fds[0], buf, sizeof( buf ) ) ) > 0 ) childRoot.append( buf, (size_t)n );
		close( fds[0] );
		int status = 0;
		waitpid( child, &status, 0 );

		const std::string parentRoot = ScratchTestRoot( suitePrefix );
		check( !childRoot.empty(), "collision check: child reported a non-empty scratch root" );
		check( childRoot != parentRoot,
			"collision check: parent (" + parentRoot + ") and child (" + childRoot + ") scratch roots differ" );
#else
		(void)suitePrefix;
		(void)check;
		std::printf( "Collision-hazard check: skipped on Windows (no fork()); pid-suffix logic is still exercised by every ScratchRunDir() call above.\n" );
#endif
	}
}
}

#endif
