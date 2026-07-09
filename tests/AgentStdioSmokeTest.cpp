//////////////////////////////////////////////////////////////////////
//
//  AgentStdioSmokeTest.cpp - Facet 5 (agentic surface) slice 0c: the
//    CLI-transport regression guard for clean stdout.
//
//  The in-process AgentFirstSliceTest drives AgentRpcDispatcher directly,
//  so it CANNOT catch stdout poisoning -- the banner, the RISE_MEDIA_PATH
//  warning, and the GlobalLog console sink only touch stdout in the REAL
//  `rise` process.  This test spawns the ACTUAL `./bin/rise --agent-stdio
//  <scene>` binary (fork + two pipes + execl), pipes two JSON-RPC request
//  lines to its stdin, and asserts on its stdout:
//
//    * the FIRST stdout line parses as a valid JSON-RPC 2.0 envelope --
//      i.e. NOT the ASCII banner, NOT the RISE_MEDIA_PATH warning, NOT a
//      GlobalLog line.  (This is exactly what a stdout-poisoning bug would
//      break: a client reads the banner as its first "frame".)
//    * each of the two response lines parses and carries the matching id.
//    * the child exits 0 on stdin EOF.
//
//  The child's stderr is redirected to /dev/null so ONLY stdout is judged
//  (warnings/events legitimately go to stderr in agent mode).  One of the
//  two requests is a `render` (which emits eLog_Event during a render), so
//  the test proves stdout stays clean across a scene LOAD *and* a render.
//
//  POSIX-only (fork/pipe/execl).  On Windows the whole body compiles to a
//  trivial pass -- run_all_tests.ps1 is a separate harness.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/Json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

#ifndef _WIN32

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

// The same inline native-v7 scene the in-process test uses (kept tiny: 4
// spp, 16x16, OIDN off), written to a temp path so the child can load it
// with NO RISE_MEDIA_PATH.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 16\n\theight 16\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n";

// Split the child's stdout blob into newline-delimited lines.
static void SplitLines( const std::string& blob, std::vector<std::string>& out )
{
	std::string cur;
	for( char c : blob ) {
		if( c == '\n' ) { out.push_back( cur ); cur.clear(); }
		else cur += c;
	}
	if( !cur.empty() ) out.push_back( cur );
}

int main()
{
	std::printf( "=== AgentStdioSmokeTest (Facet 5 slice 0c: real CLI stdout is clean) ===\n" );

	// The binary is invoked relative to the test's cwd (run_all_tests.sh runs
	// from the repo root, so ./bin/rise resolves).  A missing binary is a HARD
	// FAIL, not a silent skip -- skipping would mask the very bug this guards.
	const char* kBin = "./bin/rise";
	{
		std::ifstream probe( kBin, std::ios::binary );
		Check( probe.good(), std::string( "rise binary exists at " ) + kBin );
		if( !probe.good() ) {
			std::printf( "  (build the CLI with `make -C build/make/rise all` before running this test)\n" );
			std::printf( "=== AgentStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
			return 1;
		}
	}

	// Write the scene to a temp path.
	std::string scenePath;
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		scenePath = dir + "rise_agent_stdio_smoke.RISEscene";
		std::ofstream f( scenePath.c_str(), std::ios::binary );
		Check( (bool)f, "wrote the smoke scene to a temp file" );
		f << kScene;
		f.close();
	}

	// Two request lines: read_document (proves stdout is clean across a scene
	// LOAD, the load-emits-log case) + render (emits eLog_Event -> the stronger
	// check that a render's logging does not leak to stdout).
	const std::string requests =
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"read_document\",\"params\":{}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"render\",\"params\":{}}\n";

	int inPipe[2]  = { -1, -1 };   // parent writes -> child stdin
	int outPipe[2] = { -1, -1 };   // child stdout -> parent reads
	Check( pipe( inPipe ) == 0, "created stdin pipe" );
	Check( pipe( outPipe ) == 0, "created stdout pipe" );
	if( inPipe[0] < 0 || outPipe[0] < 0 ) {
		std::printf( "=== AgentStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
		return 1;
	}

	const pid_t pid = fork();
	Check( pid >= 0, "fork() succeeded" );
	if( pid < 0 ) {
		std::printf( "=== AgentStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
		return 1;
	}

	if( pid == 0 ) {
		// ---- child ----
		dup2( inPipe[0], STDIN_FILENO );
		dup2( outPipe[1], STDOUT_FILENO );
		// Redirect stderr to /dev/null so ONLY stdout is judged.
		const int devnull = open( "/dev/null", O_WRONLY );
		if( devnull >= 0 ) dup2( devnull, STDERR_FILENO );
		close( inPipe[0] );  close( inPipe[1] );
		close( outPipe[0] ); close( outPipe[1] );
		if( devnull >= 0 ) close( devnull );
		// Ensure RISE_MEDIA_PATH is unset so we exercise the no-media path.
		unsetenv( "RISE_MEDIA_PATH" );
		execl( kBin, kBin, "--agent-stdio", scenePath.c_str(), (char*)nullptr );
		// execl only returns on failure.
		_exit( 127 );
	}

	// ---- parent ----
	close( inPipe[0] );
	close( outPipe[1] );

	// Write the requests, then close stdin to signal EOF (clean exit).
	const ssize_t wrote = write( inPipe[1], requests.data(), requests.size() );
	Check( wrote == (ssize_t)requests.size(), "wrote both request lines to child stdin" );
	close( inPipe[1] );

	// Drain the child's stdout to EOF.
	std::string stdoutBlob;
	{
		char buf[4096];
		ssize_t n;
		while( ( n = read( outPipe[0], buf, sizeof( buf ) ) ) > 0 )
			stdoutBlob.append( buf, (size_t)n );
	}
	close( outPipe[0] );

	// Reap the child and check its exit status.
	int status = 0;
	waitpid( pid, &status, 0 );
	Check( WIFEXITED( status ) && WEXITSTATUS( status ) == 0,
	       "child exited 0 on stdin EOF" );

	// Split stdout into lines and assert cleanliness.
	std::vector<std::string> lines;
	SplitLines( stdoutBlob, lines );
	Check( lines.size() >= 2, "child emitted at least 2 stdout lines (one per request)" );

	// (1) The FIRST stdout line must be a valid JSON-RPC envelope -- NOT the
	// banner, NOT the warning, NOT a log line.  This is what a stdout-poison
	// bug would break.
	if( !lines.empty() ) {
		JsonValue env; std::string err;
		const bool ok = JsonParse( lines[0], env, err );
		Check( ok, "FIRST stdout line parses as JSON (no banner/warning/log ahead of it)" );
		Check( ok && env.isObject() && env.get( "jsonrpc" ).asString() == "2.0",
		       "FIRST stdout line is a JSON-RPC 2.0 envelope (stdout is clean from byte 0)" );
		Check( ok && env.get( "id" ).asNumber( -999 ) == 1.0,
		       "FIRST response echoes id==1 (read_document)" );
	}

	// (2) The second response line parses and carries id==2 (render).
	if( lines.size() >= 2 ) {
		JsonValue env; std::string err;
		const bool ok = JsonParse( lines[1], env, err );
		Check( ok, "SECOND stdout line parses as JSON" );
		Check( ok && env.get( "id" ).asNumber( -999 ) == 2.0,
		       "SECOND response echoes id==2 (render)" );
	}

	std::remove( scenePath.c_str() );

	std::printf( "=== AgentStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}

#else  // _WIN32

int main()
{
	std::printf( "=== AgentStdioSmokeTest: POSIX-only (fork/pipe/execl); trivially passing on Windows ===\n" );
	(void)g_pass; (void)g_fail;
	return 0;
}

#endif
