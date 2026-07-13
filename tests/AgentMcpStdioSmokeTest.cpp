//////////////////////////////////////////////////////////////////////
//
//  AgentMcpStdioSmokeTest.cpp - Secure-MCP slice 1: the CLI-transport
//    regression guard for `rise --agent-stdio --mcp <scene>`.
//
//  Mirrors AgentStdioSmokeTest.cpp's fork/pipe/execl pattern exactly, but
//  drives the MCP envelope instead of raw JSON-RPC: spawns the ACTUAL
//  `./bin/rise --agent-stdio --mcp <scene>` binary, pipes
//  initialize -> notifications/initialized -> tools/list -> one
//  tools/call through its stdin, and asserts on its stdout:
//
//    * the FIRST stdout line parses as a valid JSON-RPC 2.0 envelope
//      (NOT the banner / RISE_MEDIA_PATH warning / a GlobalLog line) --
//      same stdout-poisoning guard as the raw-JSON-RPC smoke test.
//    * the initialize response carries the expected id and a
//      protocolVersion/capabilities/serverInfo shape.
//    * notifications/initialized produces NO stdout line at all (the
//      line count check below accounts for exactly 3 response lines for
//      4 REQUEST lines sent -- the notification is the one with no
//      response).
//    * tools/list lists the 16 tools.
//    * the tools/call response carries the matching id.
//    * the child exits 0 on stdin EOF.
//
//  The child's stderr is redirected to /dev/null so ONLY stdout is
//  judged.  POSIX-only (fork/pipe/execl); on Windows the whole body
//  compiles to a trivial pass, matching AgentStdioSmokeTest.cpp.
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

// Same tiny inline native-v7 scene the raw-JSON-RPC stdio smoke test uses.
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
	std::printf( "=== AgentMcpStdioSmokeTest (Secure-MCP slice 1: real CLI --mcp stdout is clean) ===\n" );

	const char* kBin = "./bin/rise";
	{
		std::ifstream probe( kBin, std::ios::binary );
		Check( probe.good(), std::string( "rise binary exists at " ) + kBin );
		if( !probe.good() ) {
			std::printf( "  (build the CLI with `make -C build/make/rise all` before running this test)\n" );
			std::printf( "=== AgentMcpStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
			return 1;
		}
	}

	std::string scenePath;
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir.back() != '/' ) dir += '/';
		scenePath = dir + "rise_agent_mcp_stdio_smoke.RISEscene";
		std::ofstream f( scenePath.c_str(), std::ios::binary );
		Check( (bool)f, "wrote the smoke scene to a temp file" );
		f << kScene;
		f.close();
	}

	// Four request lines: initialize, notifications/initialized (a TRUE
	// notification -- no "id" field at all, so it must produce NO response
	// line), tools/list, and one tools/call (read_document -- cheap,
	// exercises the wrapped-dispatcher round trip without a render).
	const std::string requests =
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{}}}\n"
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n"
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"read_document\",\"arguments\":{}}}\n";

	int inPipe[2]  = { -1, -1 };
	int outPipe[2] = { -1, -1 };
	Check( pipe( inPipe ) == 0, "created stdin pipe" );
	Check( pipe( outPipe ) == 0, "created stdout pipe" );
	if( inPipe[0] < 0 || outPipe[0] < 0 ) {
		std::printf( "=== AgentMcpStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
		return 1;
	}

	const pid_t pid = fork();
	Check( pid >= 0, "fork() succeeded" );
	if( pid < 0 ) {
		std::printf( "=== AgentMcpStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
		return 1;
	}

	if( pid == 0 ) {
		// ---- child ----
		dup2( inPipe[0], STDIN_FILENO );
		dup2( outPipe[1], STDOUT_FILENO );
		const int devnull = open( "/dev/null", O_WRONLY );
		if( devnull >= 0 ) dup2( devnull, STDERR_FILENO );
		close( inPipe[0] );  close( inPipe[1] );
		close( outPipe[0] ); close( outPipe[1] );
		if( devnull >= 0 ) close( devnull );
		unsetenv( "RISE_MEDIA_PATH" );
		execl( kBin, kBin, "--agent-stdio", "--mcp", scenePath.c_str(), (char*)nullptr );
		_exit( 127 );
	}

	// ---- parent ----
	close( inPipe[0] );
	close( outPipe[1] );

	const ssize_t wrote = write( inPipe[1], requests.data(), requests.size() );
	Check( wrote == (ssize_t)requests.size(), "wrote all 4 request lines to child stdin" );
	close( inPipe[1] );

	std::string stdoutBlob;
	{
		char buf[4096];
		ssize_t n;
		while( ( n = read( outPipe[0], buf, sizeof( buf ) ) ) > 0 )
			stdoutBlob.append( buf, (size_t)n );
	}
	close( outPipe[0] );

	int status = 0;
	waitpid( pid, &status, 0 );
	Check( WIFEXITED( status ) && WEXITSTATUS( status ) == 0,
	       "child exited 0 on stdin EOF" );

	std::vector<std::string> lines;
	SplitLines( stdoutBlob, lines );

	// Exactly 3 response lines for 4 REQUEST lines sent: the notification
	// (notifications/initialized) produces NONE.  This is the key MCP-vs-
	// raw-JSON-RPC behavioural difference this smoke test exists to catch
	// at the REAL CLI/process boundary (the in-process test already covers
	// it against the adapter directly; this proves the stdio LOOP in
	// commandconsole.cpp actually skips writing a line for an empty
	// HandleLine return, rather than emitting a blank line onto the
	// protocol pipe).
	Check( lines.size() == 3,
	       "child emitted EXACTLY 3 stdout lines for 4 requests (the notification produced none)" );

	// (1) The FIRST stdout line must be a valid JSON-RPC envelope -- NOT
	// the banner, NOT the warning, NOT a log line.
	if( !lines.empty() ) {
		JsonValue env; std::string err;
		const bool ok = JsonParse( lines[0], env, err );
		Check( ok, "FIRST stdout line parses as JSON (no banner/warning/log ahead of it)" );
		Check( ok && env.isObject() && env.get( "jsonrpc" ).asString() == "2.0",
		       "FIRST stdout line is a JSON-RPC 2.0 envelope (stdout is clean from byte 0)" );
		Check( ok && env.get( "id" ).asNumber( -999 ) == 1.0,
		       "FIRST response echoes id==1 (initialize)" );
		Check( ok && !env.has( "error" ), "initialize response is a success" );
		Check( ok && env.get( "result" ).get( "protocolVersion" ).asString() == "2025-03-26",
		       "initialize response carries protocolVersion 2025-03-26" );
		Check( ok && env.get( "result" ).get( "serverInfo" ).get( "name" ).asString() == "rise",
		       "initialize response carries serverInfo.name == \"rise\"" );
	}

	// (2) The SECOND response line is tools/list (id==2), NOT the
	// notification (which produced no line at all).
	if( lines.size() >= 2 ) {
		JsonValue env; std::string err;
		const bool ok = JsonParse( lines[1], env, err );
		Check( ok, "SECOND stdout line parses as JSON" );
		Check( ok && env.get( "id" ).asNumber( -999 ) == 2.0,
		       "SECOND response echoes id==2 (tools/list -- the notification in between produced no line)" );
		if( ok ) {
			const JsonValue& tools = env.get( "result" ).get( "tools" );
			Check( tools.isArray() && tools.size() == 16,
			       "tools/list response lists exactly the 16 agent verbs" );
		}
	}

	// (3) The THIRD response line is the tools/call (id==3).
	if( lines.size() >= 3 ) {
		JsonValue env; std::string err;
		const bool ok = JsonParse( lines[2], env, err );
		Check( ok, "THIRD stdout line parses as JSON" );
		Check( ok && env.get( "id" ).asNumber( -999 ) == 3.0,
		       "THIRD response echoes id==3 (tools/call)" );
		if( ok ) {
			Check( !env.has( "error" ), "tools/call(read_document) response is a JSON-RPC success" );
			Check( !env.get( "result" ).get( "isError" ).asBool( true ),
			       "tools/call(read_document) response isError == false" );
		}
	}

	std::remove( scenePath.c_str() );

	std::printf( "=== AgentMcpStdioSmokeTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}

#else  // _WIN32

int main()
{
	std::printf( "=== AgentMcpStdioSmokeTest: POSIX-only (fork/pipe/execl); trivially passing on Windows ===\n" );
	(void)g_pass; (void)g_fail;
	return 0;
}

#endif
