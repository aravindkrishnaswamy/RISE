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
	#ifndef getpid
		#define getpid _getpid
	#endif
#else
	#include <unistd.h>
	#include <sys/wait.h>
#endif

namespace RISE
{
namespace TestSupport
{
	inline std::string ScratchTestRoot( const char* suitePrefix )
	{
		const char* base = std::getenv( "TMPDIR" );
		if( !base ) base = std::getenv( "TMP" );
		std::string dir = base ? base : "/tmp";
		// Forward slashes throughout so paths can be embedded directly inside
		// JSON string literals without Windows backslash escape issues.
		for( char& c : dir ) if( c == '\\' ) c = '/';
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		dir += suitePrefix;
		dir += std::to_string( (long)getpid() );
		return dir;
	}

	inline std::string ScratchRunDir( const char* suitePrefix, const char* leaf )
	{
		std::string dir = ScratchTestRoot( suitePrefix ) + "/" + leaf;
		std::error_code ec;
		std::filesystem::remove_all( dir, ec );
		return dir;
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
