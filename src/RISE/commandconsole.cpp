//////////////////////////////////////////////////////////////////////
//
//  commandconsole.cpp - Contains the entry point for RISE application
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: ?? Sometime a while ago ??
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


//
// Common includes
//
#include <iostream>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "../Library/RISE_API.h"
#include "../Library/Utilities/Log/StreamPrinter.h"
#include "../Library/Agent/AgentSession.h"
#include "../Library/Agent/AgentRpc.h"
#include "../Library/Agent/AgentMcpAdapter.h"
#include "../Library/Agent/AgentLoopbackHttpServer.h"
#include "../Library/Agent/AgentEvalRunner.h"
#include "../Library/Agent/ChatHttpTransport.h"
#include <filesystem>
#include "../Library/Interfaces/ILogPriv.h"
#include "../Library/Interfaces/IOptions.h"
#include "../Library/Interfaces/IJobPriv.h"
#include "../Library/Interfaces/IFilm.h"
#include "../Library/Interfaces/IScene.h"
#include "../Library/Utilities/RTime.h"
#include "../Library/Utilities/MediaPathLocator.h"
#include "../Library/Parsers/StdOutProgress.h"
#include "../Library/Rendering/ViewportFrameStore.h"
#include "../Library/Rendering/FrameStore.h"

//
// Windows version has a windows window that shows the render progress
//
#ifdef _WIN32
#include "../Library/Rendering/Win32WindowRasterizerOutput.h"
// Win32WindowRasterizerOutput.h already brings in <windows.h>; the
// SDK / WindowsTargetPlatformVersion picks the _WIN32_WINNT baseline.
#endif

using namespace RISE;

// P5 (Model-B, Slice 6c-3a): scene load is now CST-ONLY.  LoadAsciiSceneAuto classifies the scene (native-v7 ->
// canonical CST path, retaining the Document so scene_variant switching + CST edit/save work; un-migrated ->
// HARD FAIL with a migrator pointer -- the legacy streaming parser was retired).  A derive error on the
// native-v7 branch is a REAL, visible failure (returns false) -- it is NOT masked by a legacy retry.  There is
// no env escape hatch (RISE_FORCE_LEGACY_LOAD and the old opt-in RISE_LOAD_VIA_CST are both gone).
static bool LoadScenePerEnv( IJobPriv* pJob, const char* sceneArg )
{
	return pJob->LoadAsciiSceneAuto( sceneArg );
}

//
// Console version
//

double DoPerformanceRating()
{
	IJobPriv* pJob = 0;
	RISE_CreateJobPriv( &pJob );
	pJob->LoadAsciiSceneAuto( "scenes/pr.RISEscene" );   // Slice 6c-3a: via Auto (CST-only -- native-v7 -> CST, else hard-fail)

	unsigned int		actual_time = 1;
	pJob->PredictRasterizationTime( 10000, 0, &actual_time );

	safe_release( pJob );

	return( 1.0 / (double(actual_time)/6000000.0) );
}

double DoHighIntensityPerformanceRating( double* pStdDev )
{
	// Just computes the performance rating over and over again trying to get a good solution
	std::vector<double>	values;
	double sum=0;
	for( int i=0; i<5; i++ ) {
		double thisvalue = DoPerformanceRating();
		values.push_back( thisvalue );
		sum += thisvalue;
	}

	double mean = sum / double(values.size());

	if( pStdDev ) {
		*pStdDev = 0;
		// Compute standard deviation
		std::vector<double>::const_iterator i, e;
		for( i=values.begin(), e=values.end(); i!=e; i++ ) {
			double diff = (*i)-mean;
			*pStdDev += diff*diff;
		}

		*pStdDev /= double(values.size());
		*pStdDev = sqrt( *pStdDev );
	}

	return mean;
}

double DoHighIntensityPerformanceRating2( double* pStdDev )
{
	// Just computes the performance rating over and over again trying to get a good solution
	std::vector<double>	values;
	double sum=0;
	for( int i=0; i<5; i++ ) {
		double thisvalue = DoPerformanceRating();
		values.push_back( thisvalue );
		sum += thisvalue;
	}

	double mean = sum / double(values.size());

	if( pStdDev ) {
		*pStdDev = 0;
		// Compute standard deviation
		std::vector<double>::const_iterator i, e;
		for( i=values.begin(), e=values.end(); i!=e; i++ ) {
			double diff = (*i)-mean;
			*pStdDev += diff*diff;
		}

		*pStdDev /= double(values.size());
		*pStdDev = sqrt( *pStdDev );

		std::cout << "Computed base standard deviation: " << *pStdDev << std::endl;

		// Now keep taking samples, until the standard deviation falls below 1
		while( (*pStdDev/mean) > 1.0 ) {
			double thisvalue = DoPerformanceRating();
			values.push_back( thisvalue );
			sum += thisvalue;
			mean = sum / double(values.size());

			// Compute standard deviation
			*pStdDev = 0;
			std::vector<double>::const_iterator i, e;
			for( i=values.begin(), e=values.end(); i!=e; i++ ) {
				double diff = (*i)-mean;
				*pStdDev += diff*diff;
			}

			*pStdDev /= double(values.size());
			*pStdDev = sqrt( *pStdDev );

			std::cout << (unsigned int)(values.size())-1 << "th iteration, standard deviation was: " << *pStdDev << "(" << *pStdDev/mean*100 << "% of mean)" << std::endl;
		}
	}

	return mean;
}

// Facet 5 (agentic surface) slice 0c: the `--agent-stdio` read-eval-print
// loop.  INSTEAD of the interactive `render`/`quit` console, RISE reads
// LINE-DELIMITED JSON-RPC 2.0 requests from stdin, dispatches each through
// the SAME AgentRpcDispatcher the end-to-end test drives, and writes one
// response line to stdout (flushed per line).  EOF on stdin -> clean
// exit 0.  An optional scene-file positional arg loads an initial head
// (read+validate+propose+render then operate on it); with no scene the
// dispatcher still speaks valid JSON-RPC.  The STATELESS methods --
// read_schema, read_skill, validate, and read_document -- work with NO head
// (the agent bootstraps a scene from scratch: read the grammar, read the
// authoring skills, validate a candidate, read the empty head); the
// HEAD-BACKED methods -- propose_patch, insert_chunk, remove_chunk, render,
// read_image -- return a "no session" internal error until a head exists.
//
// The trust boundary is the OS process (design §7.2); slice 0c adds NO
// auth token and NO networking -- stdin/stdout only.
//
// Secure-MCP slice 2: `autonomy` is the launch-time posture resolved from
// `--agent-autonomy` BEFORE this function is ever called (main() parses
// and validates it loudly, exiting on a malformed value) -- it is NEVER
// re-derived here or settable by anything the loop reads from stdin.
//
// Secure-MCP slice 5b: the ONE place the wire-level AgentAutonomy posture
// maps to AgentSession's own AgentAuthority (Owner/External) -- see
// AgentRpc.h's file header, "Secure-MCP slice 5b (`Propose` autonomy)", for
// the full three-way mapping this implements: Read -> Owner (nothing
// mutating-shaped reaches the session either way, so authority is moot;
// Owner is the harmless default), Propose -> External (a proposing wire
// client stages rather than commits), Commit -> Owner (today's unrestricted
// in-process behaviour, e.g. the GUI's own dispatcher, which never passes
// through this CLI-only helper at all). Shared by every headless transport
// (RunAgentStdio, RunAgentMcpStdio, RunAgentHttp) so the mapping cannot
// drift between them.
static RISE::Agent::AgentAuthority AuthorityForAutonomy( RISE::Agent::AgentAutonomy autonomy )
{
	return ( autonomy == RISE::Agent::AgentAutonomy::Propose )
		? RISE::Agent::AgentAuthority::External
		: RISE::Agent::AgentAuthority::Owner;
}

static int RunAgentStdio( const char* sceneArg, RISE::Agent::AgentAutonomy autonomy )
{
	std::unique_ptr<RISE::Agent::AgentSession> session;
	if( sceneArg && sceneArg[0] ) {
		session = RISE::Agent::AgentSession::LoadFromFile( sceneArg, AuthorityForAutonomy( autonomy ) );
		// A failed load is NOT fatal: report it on stderr and continue with a
		// null session (read_schema-less methods still error cleanly) so a CI
		// caller gets structured errors rather than a silent process death.
		if( !session ) {
			std::cerr << "rise --agent-stdio: could not load scene '" << sceneArg
			          << "' (not native-v7, or a derive error); continuing with no head.\n";
		}
	}

	RISE::Agent::AgentRpcDispatcher dispatcher( std::move( session ), autonomy );

	std::string line;
	while( std::getline( std::cin, line ) ) {
		// Skip blank lines (a bare newline is not a JSON-RPC request); this
		// keeps the loop robust to a client that pads with newlines.
		bool allWs = true;
		for( char c : line ) { if( c != ' ' && c != '\t' && c != '\r' ) { allWs = false; break; } }
		if( allWs ) continue;

		const std::string response = dispatcher.HandleLine( line );
		std::cout << response << "\n";
		std::cout.flush();   // one response per line, flushed so a spawner sees it immediately
	}
	// EOF on stdin -> clean exit.
	return 0;
}

// Secure-MCP slice 1: `--agent-stdio --mcp` speaks the Model Context
// Protocol over the SAME line-delimited stdin/stdout transport as
// `--agent-stdio` alone, but through AgentMcpAdapter instead of
// AgentRpcDispatcher directly -- a PURE envelope translation layer over
// the identical 14 agent verbs (initialize / tools/list / tools/call
// replace the raw JSON-RPC method names an MCP client would otherwise
// have to know).  Same stdout hygiene as RunAgentStdio (the caller's
// early argv pre-scan already covers `--mcp` alongside `--agent-stdio`,
// so the banner/log suppression applies identically).
//
// Secure-MCP slice 5b: same HONEST LIMITATION as RunAgentHttp below --
// `--agent-autonomy=propose` builds an External-authority session
// (AuthorityForAutonomy) but this function's session is headless (no
// AttachController call anywhere on this path), so a headless propose
// over stdio refuses every mutating verb outright (no controller = no
// queue to stage to). See RunAgentHttp's doc for the full rationale and
// the slice-5c GUI-hosted-server plan that actually makes `propose` useful.
//
// MCP notifications (a request with no `id` field at all, e.g.
// `notifications/initialized`) get NO response line -- HandleLine
// signals that by returning an EMPTY string; this loop must skip writing
// a line in that case rather than emitting a blank line (which would
// itself be an invalid/ambiguous frame on the protocol pipe).
// Secure-MCP slice 2: same `autonomy` contract as RunAgentStdio above --
// resolved and validated in main() before this function is called.
static int RunAgentMcpStdio( const char* sceneArg, RISE::Agent::AgentAutonomy autonomy )
{
	std::unique_ptr<RISE::Agent::AgentSession> session;
	if( sceneArg && sceneArg[0] ) {
		session = RISE::Agent::AgentSession::LoadFromFile( sceneArg, AuthorityForAutonomy( autonomy ) );
		if( !session ) {
			std::cerr << "rise --agent-stdio --mcp: could not load scene '" << sceneArg
			          << "' (not native-v7, or a derive error); continuing with no head.\n";
		}
	}

	RISE::Agent::AgentMcpAdapter adapter( std::move( session ), autonomy );

	std::string line;
	while( std::getline( std::cin, line ) ) {
		bool allWs = true;
		for( char c : line ) { if( c != ' ' && c != '\t' && c != '\r' ) { allWs = false; break; } }
		if( allWs ) continue;

		const std::string response = adapter.HandleLine( line );
		if( response.empty() ) continue;   // a true MCP notification -- no response line
		std::cout << response << "\n";
		std::cout.flush();
	}
	return 0;
}

// Secure-MCP slice 3+4: `--agent-http[=PORT]` serves the SAME MCP envelope
// (AgentMcpAdapter) over a LOOPBACK-ONLY HTTP/1.1 endpoint instead of
// stdin/stdout, so a local MCP client can connect over HTTP. Slice 4 added a
// per-launch bearer token plus Origin/Host validation on top of the slice-3
// infra -- see AgentLoopbackHttpServer.h's file header for the full scope.
// `port` == 0 asks the OS for an ephemeral port (printed once bound, along
// with the bearer token, to stderr -- stdout is not used as a framing
// channel by this transport either, for the same console-hygiene reason
// RunAgentStdio suppresses it).
//
// Same `autonomy` contract as RunAgentStdio/RunAgentMcpStdio: resolved and
// validated in main() before this function is called, and defaults to Read
// at the CLI layer -- an HTTP endpoint is reachable by ANY local process
// that can connect to 127.0.0.1, so treating it as at least as untrusted as
// the stdio transport (arguably more so, since stdio requires the caller to
// have spawned this exact process) is the conservative default.
//
// Secure-MCP slice 5b HONEST LIMITATION: `--agent-autonomy=propose` builds
// an External-authority AgentSession (AuthorityForAutonomy above) so a
// mutating verb reaches AgentSession::ProposePatch/InsertChunk/RemoveChunk
// -- but THIS function's session is built via LoadFromFile, which loads a
// HEADLESS Job with no SceneEditController ever attached
// (session->AttachController is never called anywhere on this path). An
// External session with NO live controller has no proposal queue to stage
// TO (see AgentSession::ProposePatch's doc, "External + NO controller
// attached (headless) -> REFUSED") -- so `--agent-http --agent-autonomy=
// propose` against a scene loaded straight from a file will REFUSE every
// propose_patch/insert_chunk/remove_chunk call, honestly, rather than
// silently doing nothing useful. This is not a bug to route around here:
// the whole point of `propose` is a human Owner who can later approve the
// staged edit, and a bare CLI process parsing a scene file has no such
// human attached to a controller. The REAL propose-and-approve topology is
// the in-app GUI hosting this SAME loopback HTTP server against its OWN
// live SceneEditController (RISEViewportBridge.mm's in-process dispatcher
// today; a GUI-hosted --agent-http External endpoint is the slice-5c
// integration) -- an external wire client proposes into that live
// controller's queue, and the GUI's owner-authority session (or a future
// in-app "review proposals" panel) resolves it. `--agent-autonomy=propose`
// against `rise --agent-http <scene-file>` today is therefore honest but
// not yet USEFUL beyond proving the wire verbs + refusal path -- see the
// AgentProposalWireTest red-prove, which attaches a controller itself (the
// way a future GUI host will) to exercise the path that actually stages.
static int RunAgentHttp( const char* sceneArg, RISE::Agent::AgentAutonomy autonomy, unsigned short port )
{
	std::unique_ptr<RISE::Agent::AgentSession> session;
	if( sceneArg && sceneArg[0] ) {
		session = RISE::Agent::AgentSession::LoadFromFile( sceneArg, AuthorityForAutonomy( autonomy ) );
		if( !session ) {
			std::cerr << "rise --agent-http: could not load scene '" << sceneArg
			          << "' (not native-v7, or a derive error); continuing with no head.\n";
		}
	}

	RISE::Agent::AgentMcpAdapter adapter( std::move( session ), autonomy );
	RISE::Agent::AgentLoopbackHttpServer server( &adapter );

	if( !server.Bind( port ) ) {
		std::cerr << "ERROR: rise --agent-http: failed to bind 127.0.0.1:" << port
		          << " (port in use, or another bind failure).\n";
		return 1;
	}

	// Secure-MCP slice 4 banner: loud, unconditional, to stderr (stderr is
	// the console-hygiene channel this transport uses for anything that is
	// not a JSON-RPC/HTTP response body -- same split RunAgentStdio /
	// RunAgentMcpStdio already use between stdout-as-protocol-pipe and
	// stderr-as-diagnostics, just with the socket standing in for stdout
	// here). The bearer token is printed EXACTLY ONCE, here, to stderr --
	// never to stdout, never to the file log (GlobalLog's LogRequest call
	// in AgentLoopbackHttpServer.cpp logs method/path/status/sizes only,
	// never a header value). Whoever starts this process is the only
	// party meant to see this token; anyone else reading it would have
	// needed to already read this process's own stderr, at which point
	// loopback-only network isolation was already moot.
	std::cerr << "rise --agent-http: listening on http://127.0.0.1:" << server.BoundPort() << "/mcp\n"
	          << "  Bearer token: " << server.ForTest_Token() << "\n"
	          << "  Every request must carry: Authorization: Bearer " << server.ForTest_Token() << "\n"
	          << "  This endpoint is loopback-only (127.0.0.1) AND requires the token above --\n"
	          << "  a real (if single-user, local-only) authentication boundary, not merely\n"
	          << "  network isolation. It is still NOT an internet-facing auth system: do not\n"
	          << "  port-forward or otherwise expose this beyond this machine.\n";
	if( autonomy == RISE::Agent::AgentAutonomy::Commit ) {
		std::cerr << "  --agent-autonomy=commit: this HTTP client has FULL OWNER AUTHORITY -- it\n"
		          << "  commits edits directly (same trust level as the in-app GUI), not staged\n"
		          << "  proposals a human must approve. Only opt into commit for a client you\n"
		          << "  trust as much as yourself.\n";
	}

	server.Serve();
	return 0;
}

// Eval-harness slice E4: `rise --agent-eval <runconfig.json>` -- the LIVE
// headless runner.  Loads a run config (scenarios x providers x repeats),
// drives each (scenario, provider-with-key, repeat) through the REAL
// AgentChatLoop + dispatcher over a REAL platform-TLS transport
// (NSURLSession on macOS / WinHTTP on Windows; an honest "unsupported" stub
// on Linux), and writes a per-run trajectory + result + check-result under
// runDir for E5 to consume.
//
// KEY HYGIENE: the api key is read ONLY here, ONLY via getenv on each
// provider's declared keyEnvVar, and handed to RunEvalMatrix as an injected
// lookup -- getenv stays in THIS TU (the library AgentEvalRunner.cpp never
// reads an env var).  A provider whose env var is unset is SKIPPED with a
// message, never a crash.  The key is never logged, never written to any
// output file, and never handed to the progress log.
//
// This is NOT a stdout-protocol transport (unlike --agent-stdio/--agent-http):
// progress + the final summary go to stderr, and the machine-readable output
// is the per-run JSONL files under runDir.
namespace {
	// Minimal '*'/'?' wildcard match over a single filename (no path
	// separators in the pattern segment -- callers split on the parent dir).
	bool WildcardMatch( const std::string& pat, const std::string& s )
	{
		std::size_t p = 0, si = 0, star = std::string::npos, mark = 0;
		while( si < s.size() ) {
			if( p < pat.size() && ( pat[p] == '?' || pat[p] == s[si] ) ) { ++p; ++si; }
			else if( p < pat.size() && pat[p] == '*' ) { star = p++; mark = si; }
			else if( star != std::string::npos ) { p = star + 1; si = ++mark; }
			else return false;
		}
		while( p < pat.size() && pat[p] == '*' ) ++p;
		return p == pat.size();
	}
}

static int RunAgentEval( const char* runConfigPath )
{
	using namespace RISE::Agent;

	AgentEvalRunConfig config;
	std::string err;
	if( !LoadEvalRunConfig( runConfigPath, config, err ) ) {
		std::cerr << "rise --agent-eval: " << err << "\n";
		return 1;
	}

	// Expand scenario globs (entries with '*'/'?') against the filesystem;
	// pass literal paths through unchanged.
	std::vector<std::string> scenarioPaths;
	for( std::size_t i = 0; i < config.scenarios.size(); ++i ) {
		const std::string& entry = config.scenarios[i];
		if( entry.find( '*' ) == std::string::npos && entry.find( '?' ) == std::string::npos ) {
			scenarioPaths.push_back( entry );
			continue;
		}
		std::filesystem::path pat( entry );
		std::filesystem::path dir = pat.has_parent_path() ? pat.parent_path() : std::filesystem::path( "." );
		const std::string fnamePat = pat.filename().string();
		std::error_code ec;
		std::vector<std::string> matched;
		if( std::filesystem::is_directory( dir, ec ) ) {
			for( std::filesystem::directory_iterator it( dir, ec ), end; it != end; it.increment( ec ) ) {
				if( ec ) break;
				if( !it->is_regular_file( ec ) ) continue;
				const std::string fn = it->path().filename().string();
				if( WildcardMatch( fnamePat, fn ) ) matched.push_back( it->path().string() );
			}
		}
		std::sort( matched.begin(), matched.end() );   // deterministic order
		for( std::size_t m = 0; m < matched.size(); ++m ) scenarioPaths.push_back( matched[m] );
	}

	// De-duplicate by CANONICAL path before loading: a run config can name the
	// same scenario twice (e.g. a literal path PLUS a glob that also matches
	// it).  Loading it twice would land two runs in the same per-run subdir --
	// trajectory.jsonl (append) gets two concatenated sessions and result.jsonl
	// (truncate) silently overwrites.  Canonicalize (resolving `.`/`..`/symlinks;
	// falling back to weakly_canonical then the raw path when a file can't be
	// resolved) and keep only the FIRST occurrence, preserving order.
	{
		std::vector<std::string> deduped;
		std::set<std::string>    seen;
		for( std::size_t i = 0; i < scenarioPaths.size(); ++i ) {
			std::error_code cec;
			std::filesystem::path canon = std::filesystem::canonical( scenarioPaths[i], cec );
			if( cec ) { canon = std::filesystem::weakly_canonical( scenarioPaths[i], cec ); }
			const std::string key = cec ? scenarioPaths[i] : canon.string();
			if( seen.insert( key ).second ) deduped.push_back( scenarioPaths[i] );
			else
				std::cerr << "rise --agent-eval: scenario '" << scenarioPaths[i]
				          << "' duplicates an earlier entry (same canonical path) -- skipping.\n";
		}
		scenarioPaths.swap( deduped );
	}

	if( scenarioPaths.empty() ) {
		std::cerr << "rise --agent-eval: no scenario files matched the run config's \"scenarios\".\n";
		return 1;
	}

	// Load every scenario up front (loud-fail on any malformed one).
	std::vector<AgentEvalScenario> scenarios;
	for( std::size_t i = 0; i < scenarioPaths.size(); ++i ) {
		AgentEvalScenario s;
		std::string serr;
		if( !LoadEvalScenario( scenarioPaths[i], s, serr ) ) {
			std::cerr << "rise --agent-eval: scenario load failed: " << serr << "\n";
			return 1;
		}
		scenarios.push_back( s );
	}

	// The platform transport.  On Linux this is the honest "unsupported"
	// stub -- surface that loudly BEFORE spending any turns, rather than
	// letting every run fail with transport_error.
	std::unique_ptr<IChatHttpTransport> transport = CreateSystemChatHttpTransport();
	{
		ChatHttpRequest probe;
		probe.url = "https://127.0.0.1:0/__rise_agent_eval_probe__";
		const ChatHttpResponse pr = transport->Post( probe );
		if( pr.error.find( kUnsupportedTransportSentinel ) != std::string::npos ) {
			std::cerr << "rise --agent-eval: " << pr.error << "\n"
			          << "  (the live runner needs a platform-TLS transport; this build has none.)\n";
			return 1;
		}
		// Any OTHER probe outcome (a connection refused to 127.0.0.1:0, etc.)
		// is expected and harmless -- it only confirms the transport is a real
		// implementation, not the stub.
	}

	std::cerr << "rise --agent-eval: " << scenarios.size() << " scenario(s) x "
	          << config.providers.size() << " provider(s) x " << config.repeats
	          << " repeat(s); runDir=" << config.runDir << "\n";

	AgentEvalMatrixOptions mo;
	mo.transport = transport.get();
	mo.envLookup = []( const std::string& name ) -> const char* { return getenv( name.c_str() ); };
	mo.log = []( const std::string& m ) { std::cerr << m << "\n"; };

	const AgentEvalMatrixResult mr = RunEvalMatrix( config, scenarios, mo );

	if( !mr.errorMessage.empty() ) {
		std::cerr << "rise --agent-eval: " << mr.errorMessage << "\n";
		return 1;
	}

	std::cerr << "rise --agent-eval: done. providers used=" << mr.providersUsed
	          << " skipped=" << mr.providersSkipped
	          << "; runs executed=" << mr.runsExecuted
	          << " skipped(no-key)=" << mr.runsSkipped
	          << "; results under " << config.runDir << "\n";
	return 0;
}

int main( int argc, char** argv )
{
	// Facet 5 slice 0c: pre-scan argv for `--agent-stdio` BEFORE anything is
	// written to stdout.  In agent mode stdout is a line-delimited JSON-RPC
	// transport and MUST carry protocol frames ONLY -- so the ASCII banner,
	// the RISE_MEDIA_PATH warning, and (below) the GlobalLog console sink are
	// all gated / redirected off stdout.  The authoritative flag parse still
	// happens later; this early scan only decides "is stdout a protocol pipe".
	// Secure-MCP slice 3: `--agent-http[=PORT]` doesn't use stdout as its
	// protocol channel (that's the socket), but the SAME console-hygiene
	// gate is worth reusing here too -- the banner / RISE_MEDIA_PATH warning
	// are noise a caller scripting this transport doesn't want either, and
	// the GlobalLog console-sink redirect (stderr only) matches this
	// transport's own startup-banner/warning choice to use stderr.
	bool agentMode = false;
	for( int ai = 1; ai < argc; ai++ ) {
		if( strcmp( argv[ai], "--agent-stdio" ) == 0 ) { agentMode = true; break; }
		if( strcmp( argv[ai], "--agent-http" ) == 0 ||
		    strncmp( argv[ai], "--agent-http=", strlen( "--agent-http=" ) ) == 0 ) { agentMode = true; break; }
		// Eval-harness E4: `--agent-eval` writes results to files + stderr
		// progress; suppress the banner / route the media warning to stderr
		// like the other agent modes (its stdout is not a protocol pipe, but
		// the banner is still noise a batch caller doesn't want).
		if( strcmp( argv[ai], "--agent-eval" ) == 0 ||
		    strncmp( argv[ai], "--agent-eval=", strlen( "--agent-eval=" ) ) == 0 ) { agentMode = true; break; }
	}

	// Setup the media path locator
	const char* szmediapath = getenv( "RISE_MEDIA_PATH" );

	if( szmediapath ) {
		GlobalMediaPathLocator().AddPath( szmediapath );
	} else if( !agentMode ) {
		std::cout << std::endl;
		std::cout << "Warning! the 'RISE_MEDIA_PATH' environment variable is not set." << std::endl;
		std::cout << "unless you have been very carefull with explicit media pathing," << std::endl;
		std::cout << "certain resources may not load.  See the README for information" << std::endl;
		std::cout << "on setting this path." << std::endl;
		std::cout << std::endl;
	} else {
		// Agent mode (stdio OR http -- this message is shared across both
		// transports, see `agentMode`'s pre-scan above): keep stdout clean,
		// but the guidance is still useful -- route it to stderr so a
		// caller diagnosing missing resources sees it.
		std::cerr << "rise --agent-stdio/--agent-http: 'RISE_MEDIA_PATH' is not set; "
		             "some resources may not load unless explicitly pathed.\n";
	}

	SetGlobalLogFileName( "RISE_Log.txt" );

#ifdef _WIN32
	{
		char myfilename[1024] = {0};
		GetModuleFileName( NULL, myfilename, 1023 );
		char mypath[1024] = {0};
		
		char mylogfilename[1024] = {0};
		{
			char drive[_MAX_PATH] = {0};
			char dir[_MAX_PATH] = {0};
			_splitpath( myfilename, drive, dir, 0, 0 );
			_makepath( mypath, drive, dir, 0, 0 );

			_makepath( mylogfilename, drive, dir, "RISE_Log", "txt" );
			SetGlobalLogFileName( mylogfilename );
		}

		GlobalMediaPathLocator().AddPath( mypath );
	}
#endif

	// Initialize the random number generator
	srand( GetMilliseconds() );

	int major, minor, revision, build;
	bool isdebug;

	RISE_API_GetVersion( &major, &minor, &revision, &build, &isdebug );

	// Banner is a stdout write -- suppress it in agent mode (stdout is the
	// JSON-RPC protocol pipe; the first stdout line MUST be a response frame).
	if( !agentMode ) {
		std::cout << "===============================================" << std::endl;
		std::cout << "                                               " << std::endl;
		std::cout << "  R.I.S.E - Realistic Image Synthesis Engine   " << std::endl;
		std::cout << "              v." << major  << "." << minor << "." << revision << " build " << build << std::endl;
		std::cout << "      built on " << __DATE__ << " at " << __TIME__ << std::endl;
		if( isdebug ) {
			std::cout <<
				     "             DEBUG version" << std::endl;
		}
		std::cout << "     (c) 2001-2006 Aravind Krishnaswamy        " << std::endl;
		std::cout << "                                               " << std::endl;
		std::cout << "===============================================" << std::endl;
	}

	// Agent mode: detach the GlobalLog stdout console sink so no
	// PrintEasyEvent / PrintEx(eLog_Event,...) from scene-load or render
	// interleaves with JSON-RPC responses on stdout.  GlobalLogPriv()->
	// RemoveAllPrinters() drops BOTH default printers (the eLog_Console
	// std::cout StreamPrinter AND the RISE_Log.txt file printer), so we
	// re-install the file printer + a std::cerr console printer.  Net
	// effect: file logging unchanged (RISE_Log.txt), warnings/errors/events
	// go to stderr, and stdout carries protocol frames ONLY.  Non-agent
	// logging is untouched.
	if( agentMode ) {
		GlobalLogPriv()->RemoveAllPrinters();
		{
			std::ofstream* fs = new std::ofstream( "RISE_Log.txt" );
			Implementation::StreamPrinter* pFile =
				new Implementation::StreamPrinter( fs, true, true );
			GlobalLog()->PrintNew( pFile, __FILE__, __LINE__, "agent-mode file printer" );
			GlobalLogPriv()->AddPrinter( pFile );
			safe_release( pFile );

			Implementation::StreamPrinter* pErr =
				new Implementation::StreamPrinter( &std::cerr, true, eLog_Console, false );
			GlobalLog()->PrintNew( pErr, __FILE__, __LINE__, "agent-mode stderr console printer" );
			GlobalLogPriv()->AddPrinter( pErr );
			safe_release( pErr );
		}
	}

	// Lets get this job rolling
	IJobPriv* pJob = 0;
	RISE_CreateJobPriv( &pJob );

	// Parse CLI flags first to extract Film overrides + the positional
	// scene-file argument.  Supported flags (any order, anywhere on the
	// command line):
	//   --width N               override Film width (pixels; positive integer)
	//   --height N              override Film height (pixels; positive integer)
	//   --pixel-ar X            override Film pixel aspect ratio (positive float)
	//   --agent-autonomy=read|propose|commit
	//                           (meaningful with --agent-stdio / --agent-http)
	//                           the headless launch-time posture: `read`
	//                           refuses propose_patch/insert_chunk/
	//                           remove_chunk/resolve_proposal with a
	//                           structured policy error (render and every
	//                           other read/observe verb, including
	//                           list_proposals, still work -- none of them
	//                           mutate the document, so they stay available
	//                           even under `read`); `propose` (Secure-MCP
	//                           slice 5b) lets propose_patch/insert_chunk/
	//                           remove_chunk THROUGH to the session, which
	//                           constructs as AgentAuthority::External (see
	//                           AuthorityForAutonomy above) -- so a mutating
	//                           call STAGES a proposal instead of committing
	//                           one; resolve_proposal stays refused (Owner-
	//                           only; see AgentRpc.h); `commit` is today's
	//                           unrestricted behaviour (AgentAuthority::Owner
	//                           -- every verb commits directly). DEFAULT (flag
	//                           omitted) is `read` -- an agent must opt IN to
	//                           mutation explicitly. An unrecognized value
	//                           (anything but exactly "read", "propose", or
	//                           "commit") is a loud launch-time failure (exit
	//                           1), never a silent default. This flag is
	//                           LAUNCH-TIME ONLY: nothing reachable from
	//                           stdin (a request parameter, a scene file) can
	//                           change it.
	// Values are applied AFTER LoadAsciiScene returns, so they replace
	// whatever the scene file authored.  This is how agents render test
	// scenes at lower resolution without editing scene files.
	//
	// Validation: each numeric flag's value must parse as a strictly
	// positive number.  `--width foo` (non-numeric) and `--width -5`
	// (negative) fail loudly rather than silently consuming the next
	// argv as a malformed override.
	int  cliFilmWidth   = 0;	// 0 = no override
	int  cliFilmHeight  = 0;
	double cliFilmPixelAR = 0.0;
	bool cliArgError = false;
	bool cliAgentStdio = false;	// Facet 5 slice 0c: JSON-RPC stdio transport
	bool cliMcp = false;	// Secure-MCP slice 1: speak MCP (not raw JSON-RPC) over that same transport
	// Secure-MCP slice 3+4: `--agent-http[=PORT]` -- loopback-only HTTP/1.1
	// transport for the SAME MCP envelope, now with a per-launch bearer
	// token + Origin/Host validation (see AgentLoopbackHttpServer.h's file
	// header for the full scope).  `cliAgentHttp` gates the branch;
	// `cliAgentHttpPort` defaults to 8765 (0 = ask the OS for an ephemeral
	// port).  Mutually exclusive with `--agent-stdio`
	// (checked loudly below, after the full parse) -- a caller almost
	// certainly meant only one transport, and silently picking one would
	// hide a genuine invocation mistake.
	bool cliAgentHttp = false;
	unsigned short cliAgentHttpPort = 8765;
	const char* kAgentHttpFlagPrefix = "--agent-http=";
	const std::size_t kAgentHttpFlagPrefixLen = strlen( kAgentHttpFlagPrefix );
	// Eval-harness slice E4: `--agent-eval <runconfig.json>` (or
	// `--agent-eval=<runconfig.json>`) -- the LIVE headless runner.  Mutually
	// exclusive with the stdio/http transports (loudly checked below).
	bool cliAgentEval = false;
	std::string cliAgentEvalPath;
	const char* kAgentEvalFlagPrefix = "--agent-eval=";
	const std::size_t kAgentEvalFlagPrefixLen = strlen( kAgentEvalFlagPrefix );
	// Secure-MCP slice 2: the headless autonomy posture.  DEFAULT (no flag
	// at all) is Read -- the INVERSE of AgentRpcDispatcher's own C++
	// constructor default (Commit, for back-compat with the GUI's live
	// in-process dispatcher and every pre-slice-2 test) -- see AgentRpc.h's
	// class-default-vs-binary-default doc.  `--agent-autonomy=commit`
	// restores today's unrestricted behaviour; any OTHER value (including
	// a missing `=value`, empty value, or unrecognized token) is a LOUD
	// launch-time failure, never a silent fallback to either posture.
	RISE::Agent::AgentAutonomy cliAutonomy = RISE::Agent::AgentAutonomy::Read;
	const char* kAutonomyFlagPrefix = "--agent-autonomy=";
	const std::size_t kAutonomyFlagPrefixLen = strlen( kAutonomyFlagPrefix );
	const char* sceneArg = 0;
	for( int ai = 1; ai < argc; ai++ ) {
		const char* a = argv[ai];
		auto consumeIntFlag = [&]( const char* name, int& dst ) -> bool {
			if( ai+1 >= argc ) {
				std::cerr << "ERROR: " << name << " requires a positive integer; got end-of-args.\n";
				cliArgError = true; return false;
			}
			char* endp = nullptr;
			const long v = strtol( argv[ai+1], &endp, 10 );
			if( endp == argv[ai+1] || *endp != '\0' || v <= 0 ) {
				std::cerr << "ERROR: " << name << " requires a positive integer; got `" << argv[ai+1] << "`.\n";
				cliArgError = true; return false;
			}
			dst = (int)v; ++ai; return true;
		};
		auto consumeDoubleFlag = [&]( const char* name, double& dst ) -> bool {
			if( ai+1 >= argc ) {
				std::cerr << "ERROR: " << name << " requires a positive float; got end-of-args.\n";
				cliArgError = true; return false;
			}
			char* endp = nullptr;
			const double v = strtod( argv[ai+1], &endp );
			if( endp == argv[ai+1] || *endp != '\0' || v <= 0.0 ) {
				std::cerr << "ERROR: " << name << " requires a positive float; got `" << argv[ai+1] << "`.\n";
				cliArgError = true; return false;
			}
			dst = v; ++ai; return true;
		};

		if( strcmp(a, "--agent-stdio") == 0 ) {
			cliAgentStdio = true;
		} else if( strcmp(a, "--mcp") == 0 ) {
			cliMcp = true;
		} else if( strcmp(a, "--agent-eval") == 0 ) {
			// Space form: `--agent-eval <runconfig.json>` -- the next argv is
			// the config path (a REQUIRED value, loud-fail if absent or if it
			// looks like another flag).
			cliAgentEval = true;
			if( ai+1 >= argc || argv[ai+1][0] == '-' ) {
				std::cerr << "ERROR: --agent-eval requires a run-config JSON path; got "
				          << ( ai+1 >= argc ? "end-of-args" : argv[ai+1] ) << ".\n";
				cliArgError = true;
			} else {
				cliAgentEvalPath = argv[++ai];
			}
		} else if( strncmp( a, kAgentEvalFlagPrefix, kAgentEvalFlagPrefixLen ) == 0 ) {
			cliAgentEval = true;
			cliAgentEvalPath = a + kAgentEvalFlagPrefixLen;
			if( cliAgentEvalPath.empty() ) {
				std::cerr << "ERROR: --agent-eval= requires a run-config JSON path after '='.\n";
				cliArgError = true;
			}
		} else if( strcmp(a, "--agent-http") == 0 ) {
			cliAgentHttp = true;   // port stays at its default (8765)
		} else if( strncmp( a, kAgentHttpFlagPrefix, kAgentHttpFlagPrefixLen ) == 0 ) {
			cliAgentHttp = true;
			const std::string val = a + kAgentHttpFlagPrefixLen;
			char* endp = nullptr;
			errno = 0;
			const long v = val.empty() ? -1 : strtol( val.c_str(), &endp, 10 );
			// Strict: the WHOLE remainder after '=' must be digits (endp at
			// the string's end), non-negative, and fit in an unsigned short
			// (0..65535 -- 0 means "OS-assigned ephemeral port", handled by
			// AgentLoopbackHttpServer::Bind). Anything else (empty, a
			// trailing non-digit, a negative sign, or a value out of range)
			// is a loud launch-time failure -- matching --agent-autonomy's
			// own "always loud, never silent" contract for this flag family.
			if( val.empty() || endp != val.c_str() + val.size() || v < 0 || v > 65535 ) {
				std::cerr << "ERROR: --agent-http requires a port in [0,65535]; got `" << val << "`.\n";
				cliArgError = true;
			} else {
				cliAgentHttpPort = static_cast<unsigned short>( v );
			}
		} else if( strncmp( a, kAutonomyFlagPrefix, kAutonomyFlagPrefixLen ) == 0 ) {
			const std::string val = a + kAutonomyFlagPrefixLen;
			if( val == "read" ) {
				cliAutonomy = RISE::Agent::AgentAutonomy::Read;
			} else if( val == "propose" ) {
				cliAutonomy = RISE::Agent::AgentAutonomy::Propose;
			} else if( val == "commit" ) {
				cliAutonomy = RISE::Agent::AgentAutonomy::Commit;
			} else {
				std::cerr << "ERROR: --agent-autonomy requires 'read', 'propose', or 'commit'; got `" << val << "`.\n";
				cliArgError = true;
			}
		} else if( strcmp( a, "--agent-autonomy" ) == 0 ) {
			// Hardening (loud-never-silent): this catches the BARE
			// `--agent-autonomy` token with no `=value` at all -- the SPACE
			// form `--agent-autonomy read` (whose bare first token matches
			// here, and whose second token would otherwise be silently
			// swallowed as the positional scene argument -- a strictly
			// worse silent failure than merely ignoring the flag).  This
			// flag is `=value`-only by design (unlike the space-separated
			// --width/--height/--pixel-ar flags below); the fix is a
			// single loud usage error naming the exact accepted form,
			// matching this flag's own documented "always loud, never
			// silent" contract instead of quietly falling through to the
			// default posture.
			//
			// Fold-in (Secure-MCP slice 3): this used to be a strncmp
			// PREFIX match against every spelling that wasn't the
			// `--agent-autonomy=` form above -- which also (mis)matched
			// an unrelated flag that merely SHARES the prefix, e.g. a
			// hypothetical `--agent-autonomy-verbose` would have been
			// silently swallowed as a malformed `--agent-autonomy` rather
			// than falling through to the positional-argument branch (or
			// a future flag's own handler).  An exact `strcmp` matches
			// ONLY the bare token, so any longer flag that happens to
			// start with this prefix is left for its own branch (or, if
			// none claims it, the ordinary "unrecognized flag" silence
			// the loop already has for non-flag tokens -- unchanged by
			// this fix).
			std::cerr << "ERROR: --agent-autonomy requires the form `--agent-autonomy=read`, "
			             "`--agent-autonomy=propose`, or `--agent-autonomy=commit`; got `" << a << "`.\n";
			cliArgError = true;
		} else if( strcmp(a, "--width") == 0 ) {
			consumeIntFlag( "--width", cliFilmWidth );
		} else if( strcmp(a, "--height") == 0 ) {
			consumeIntFlag( "--height", cliFilmHeight );
		} else if( strcmp(a, "--pixel-ar") == 0 ) {
			consumeDoubleFlag( "--pixel-ar", cliFilmPixelAR );
		} else if( a[0] != '-' && !sceneArg ) {
			sceneArg = a;	// First non-flag positional = scene file
		}
	}
	if( cliArgError ) {
		return 1;	// Fail fast — don't render with a malformed override.
	}
	// Eval-harness slice E4: `--agent-eval` is a batch runner, not a live
	// transport -- reject it combined with either live transport LOUDLY and
	// BEFORE the transport dispatch below (otherwise the stdio/http branch
	// would win silently and the eval intent would be dropped).
	if( cliAgentEval && ( cliAgentStdio || cliAgentHttp ) ) {
		std::cerr << "ERROR: --agent-eval cannot be combined with --agent-stdio or --agent-http "
		             "(it is a batch runner, not a live transport).\n";
		return 1;
	}
	if( cliMcp && !cliAgentStdio ) {
		std::cerr << "WARNING: --mcp requires --agent-stdio; ignoring.\n";
	}
	// Secure-MCP slice 3: `--agent-http` and `--agent-stdio` are two
	// DIFFERENT transports for the same underlying agent surface -- a
	// caller passing both almost certainly meant exactly one of them, and
	// picking one silently would mask a genuine invocation mistake (e.g. a
	// script that meant to switch transports but left the old flag in).
	// Loud failure, matching this flag family's "always loud, never
	// silent" contract.
	if( cliAgentHttp && cliAgentStdio ) {
		std::cerr << "ERROR: --agent-http and --agent-stdio are mutually exclusive; pick one transport.\n";
		return 1;
	}

	// Facet 5 slice 0c: `--agent-stdio` replaces the interactive `render`/
	// `quit` console with a JSON-RPC 2.0 read-eval-print loop over stdin/
	// stdout.  It builds its OWN AgentSession from the positional scene arg
	// (LoadFromFile), so it does not use the `pJob` created above -- release
	// that here and hand control to the stdio loop, which owns its lifetime.
	// EOF on stdin -> clean exit 0.
	// Secure-MCP slice 1: `--mcp` (only meaningful alongside `--agent-stdio`)
	// swaps the raw-JSON-RPC loop for the MCP-envelope loop over the SAME
	// stdin/stdout transport -- same session construction, same stdout
	// hygiene, different dispatcher.
	if( cliAgentStdio ) {
		safe_release( pJob );
		const int rc = cliMcp ? RunAgentMcpStdio( sceneArg, cliAutonomy ) : RunAgentStdio( sceneArg, cliAutonomy );
		GlobalLogCleanupAndShutdown();
		return rc;
	}
	// Secure-MCP slice 3: `--agent-http[=PORT]` -- same MCP envelope, over a
	// loopback HTTP/1.1 socket instead of stdin/stdout. Same pJob release +
	// GlobalLogCleanupAndShutdown discipline as the stdio branch above (this
	// path builds its own AgentSession too and does not touch `pJob`).
	if( cliAgentHttp ) {
		safe_release( pJob );
		const int rc = RunAgentHttp( sceneArg, cliAutonomy, cliAgentHttpPort );
		GlobalLogCleanupAndShutdown();
		return rc;
	}
	// Eval-harness slice E4: `--agent-eval <runconfig.json>` -- the LIVE
	// headless runner.  (The stdio/http exclusion was already enforced loudly
	// right after the arg parse.)  Builds its own Job/AgentSession per run
	// (RunScenarioLive), so it does not use `pJob`; release + hand off, same
	// discipline as the --agent-stdio / --agent-http branches above.
	if( cliAgentEval ) {
		safe_release( pJob );
		const int rc = RunAgentEval( cliAgentEvalPath.c_str() );
		GlobalLogCleanupAndShutdown();
		return rc;
	}

	const bool haveCliFilmOverride =
		( cliFilmWidth > 0 ) || ( cliFilmHeight > 0 ) || ( cliFilmPixelAR > 0.0 );

	// Detect mutually-exclusive flag combos: the perf-rating shortcuts
	// (-pr / -highperf / -highperf2) ignore Film overrides, and a user
	// who passed both probably misunderstood the CLI.
	if( haveCliFilmOverride && argc > 1 ) {
		const char* a1 = argv[1];
		if( strcmp(a1,"-pr")==0 || strcmp(a1,"-highperf")==0 || strcmp(a1,"-highperf2")==0 ) {
			std::cerr << "WARNING: --width / --height / --pixel-ar have no effect with "
			          << a1 << " (the perf-rating path uses a fixed scene).\n";
		}
	}

	// If there was a file specified in the arguments, then assume its an
	// ascii scene file and load it
	if( argc > 1 ) {

		// Check for -perf
		if( strcmp( argv[1], "-pr" ) == 0 ) {
			GlobalLog()->PrintEasyEvent( "Computing performance rating (PR) for this computer..." );
			GlobalLog()->PrintEx( eLog_Event, "  Performance of this computer [%.0f] ", DoPerformanceRating() );
			return 1;
		} else if( strcmp( argv[1], "-highperf" ) == 0 ) {
			GlobalLog()->PrintEasyEvent( "Computing high intensity performance rating for this computer, this could take a while..." );
			double stddev = 0;
			double pr = DoHighIntensityPerformanceRating( &stddev );
			GlobalLog()->PrintEx( eLog_Event, "  High Intensity Performance of this computer [%.0f] with standard deviation of [%.3f]", pr, stddev );
			return 1;
		} else if( strcmp( argv[1], "-highperf2" ) == 0 ) {
			GlobalLog()->PrintEasyEvent( "Computing high intensity performance rating until standard deviation is < 1% of mean for this computer, this could take a while..." );
			double stddev = 0;
			double pr = DoHighIntensityPerformanceRating2( &stddev );
			GlobalLog()->PrintEx( eLog_Event, "  High Intensity Performance of this computer [%.0f] with standard deviation of [%.3f]", pr, stddev );
			return 1;
		} else if( sceneArg ) {
#ifdef _WIN32

//			SetPriorityClass( GetCurrentProcess(), IDLE_PRIORITY_CLASS );

			char fullname[_MAX_PATH] = {0};
			GetLongPathName( sceneArg, fullname, _MAX_PATH );
			std::cout << "Loading ascii scene file: " << fullname << std::endl;

			StdOutProgress progress( "Parsing scene: " );
			pJob->SetProgress( &progress );
			if( LoadScenePerEnv( pJob, sceneArg ) ) {
				// CLI Film overrides — applied AFTER scene parse so they
				// replace whatever the scene file's `film` chunk (or
				// camera-chunk auto-sync) installed.  Partial overrides
				// (only --width or only --height) preserve the
				// non-overridden axes from the current Film.
				if( haveCliFilmOverride ) {
					const IFilm* curFilm = pJob->GetScene()->GetFilm();
					const unsigned int w   = ( cliFilmWidth   > 0 ) ? (unsigned)cliFilmWidth   : ( curFilm ? curFilm->GetWidth()   : 960 );
					const unsigned int h   = ( cliFilmHeight  > 0 ) ? (unsigned)cliFilmHeight  : ( curFilm ? curFilm->GetHeight()  : 540 );
					const double       pAR = ( cliFilmPixelAR > 0 ) ? cliFilmPixelAR           : ( curFilm ? curFilm->GetPixelAR() : 1.0 );
					if( !pJob->SetFilm( w, h, pAR ) ) {
						GlobalLog()->PrintEasyError(
							"CLI Film override failed (invalid width/height/pixelAR); using scene-file values." );
					}
				}
				const ICamera* pCamera = pJob->GetScene()->GetCamera();
				if( pCamera ) {

					// Don't do this if it is disabled in the options file
					if( !GlobalOptions().ReadBool( "no_windowed_rasterizer_output", false ) ) {
						const IFilm* pFilm = pJob->GetScene()->GetFilm();
						IRasterizerOutput* pWinRO = new Implementation::Win32WindowRasterizerOutput(
							pFilm->GetWidth(), pFilm->GetHeight(),
							50, 50, "R.I.S.E. Render Window" );

						GlobalLog()->PrintNew( pWinRO, __FILE__, __LINE__, "window RO" );
						pJob->GetRasterizer()->AddRasterizerOutput( pWinRO );
						safe_release( pWinRO );
					}
				}
			}

#else
			std::cout << "Loading ascii scene file: " << sceneArg << std::endl;
			if( LoadScenePerEnv( pJob, sceneArg ) ) {
				if( haveCliFilmOverride ) {
					const IFilm* curFilm = pJob->GetScene()->GetFilm();
					const unsigned int w   = ( cliFilmWidth   > 0 ) ? (unsigned)cliFilmWidth   : ( curFilm ? curFilm->GetWidth()   : 960 );
					const unsigned int h   = ( cliFilmHeight  > 0 ) ? (unsigned)cliFilmHeight  : ( curFilm ? curFilm->GetHeight()  : 540 );
					const double       pAR = ( cliFilmPixelAR > 0 ) ? cliFilmPixelAR           : ( curFilm ? curFilm->GetPixelAR() : 1.0 );
					pJob->SetFilm( w, h, pAR );
				}
			}
#endif
		}
	}

	// L8 perf — Optional CLI emulation of the GUI bridge's
	// production-render setup.  Enabled by `RISE_GUI_EMUL` env var.
	// Mimics the threading + memory pressure that the real macOS /
	// Windows / Android bridges create around a production render:
	//
	//   * `ViewportFrameStore` attached to the production rasterizer
	//     (so per-tile observer dispatches fire on the bound store).
	//   * Per-tile tile-complete callback wired on a worker thread
	//     (round 17 `OnTileCompleteTry`); try-locks the staging mutex
	//     and emits ONLY the just-completed tile's region — the
	//     just-released tile mutex is the only one this lock-iterates,
	//     so workers can't deadlock each other.
	//   * 30 Hz background poll thread doing the full-image emit
	//     under the staging mutex (round 16 `PollAndEmitIfDirty`).
	//     The full-image emit takes per-tile shared_locks on every
	//     tile; we use the BLOCKING 5-arg `RenderToBuffer` so this
	//     compiles on both the L8 baseline and HEAD (the post-round-14
	//     `nonBlocking` parameter is HEAD-only).  Worst case: the
	//     poll waits for one slow tile.  Workers themselves never
	//     block on the staging mutex (try-lock).
	//
	// Disabled by default — zero impact on standard CLI use.
	const char* envEmul = std::getenv( "RISE_GUI_EMUL" );
	const bool guiEmulEnabled = envEmul && *envEmul;
	Implementation::ViewportFrameStore* guiEmulVfs = nullptr;
	std::mutex                          guiEmulBufferMutex;
	std::vector<uint8_t>                guiEmulBuffer;
	std::atomic<bool>                   guiEmulRunning( false );
	std::atomic<uint64_t>               guiEmulLastSeenGen( 0 );
	std::thread                         guiEmulPollThread;
	unsigned int                        guiEmulW = 0;
	unsigned int                        guiEmulH = 0;

	// Function-scope lambdas — captures are stable for the lifetime
	// of main (and of `guiEmulVfs`).  If these were declared inside
	// the `if(pRas)` block, the SetTileCompleteCallback's reference-
	// captured closure would become a use-after-free when the if
	// block exited (the first such mistake crashed with SIGSEGV
	// before any tile rendered).
	//
	// CRITICAL split: the region-emit (tile callback path) reads
	// ONLY the just-completed tile's ROI; the full-emit (poll path)
	// reads the whole image.  DO NOT call the full-emit from the
	// per-tile worker callback — full-emit takes per-tile
	// shared_locks across every tile, and workers holding their own
	// tiles exclusive will block each other into a fan-out stall.
	auto guiEmulEmitRegion = [&]( const RISE::Rect& roi ) {
		if( !guiEmulVfs || guiEmulW == 0 || guiEmulH == 0 ) return;
		FrameStoreOutput::ViewTransform xf;
		guiEmulVfs->RenderToBuffer(
			guiEmulBuffer.data(),
			static_cast<size_t>( guiEmulW ) * 4,
			roi,
			FrameStoreOutput::TargetFormat::RGBA8_sRGB,
			xf );
	};
	auto guiEmulEmitFull = [&]() {
		if( !guiEmulVfs || guiEmulW == 0 || guiEmulH == 0 ) return;
		RISE::Rect roi( 0, 0, guiEmulH, guiEmulW );
		guiEmulEmitRegion( roi );
	};

	if( guiEmulEnabled && pJob->GetScene() )
	{
		const IFilm* pFilm = pJob->GetScene()->GetFilm();
		if( pFilm ) {
			guiEmulW = pFilm->GetWidth();
			guiEmulH = pFilm->GetHeight();
			guiEmulBuffer.resize( static_cast<size_t>( guiEmulW ) * guiEmulH * 4 );
			guiEmulVfs = new Implementation::ViewportFrameStore();
			IRasterizer* pRas = pJob->GetRasterizer();
			if( pRas ) {
				guiEmulVfs->Attach( pRas );

				guiEmulVfs->SetTileCompleteCallback(
					[&]( const RISE::Rect& roi, uint64_t /*gen*/ ) {
						std::unique_lock<std::mutex> lock( guiEmulBufferMutex, std::try_to_lock );
						if( !lock.owns_lock() ) return;
						guiEmulEmitRegion( roi );
					} );

				guiEmulRunning.store( true, std::memory_order_release );
				guiEmulPollThread = std::thread( [&] {
					while( guiEmulRunning.load( std::memory_order_acquire ) ) {
						std::this_thread::sleep_for( std::chrono::milliseconds( 33 ) );
						if( !guiEmulVfs ) continue;
						const uint64_t gen = guiEmulVfs->Generation();
						if( gen == guiEmulLastSeenGen.load( std::memory_order_acquire ) ) continue;
						std::lock_guard<std::mutex> lock( guiEmulBufferMutex );
						guiEmulEmitFull();
						guiEmulLastSeenGen.store( gen, std::memory_order_release );
					}
				} );

				std::cout << "RISE_GUI_EMUL active: VFS attached @ "
				          << guiEmulW << "x" << guiEmulH
				          << " + 30Hz poll + region-emit tile callback"
				          << std::endl;
			}
		}
	}

	char					line[8192]={0};		// <sigh>....

	// Basically sit here forever
	ICommandParser* commandParser = 0;
	RISE_API_CreateAsciiCommandParser( &commandParser );

	while( 1 )
	{
		std::cout << "\n> ";
		// And parse commands
		std::cin.getline( line, 8192 );

		if( std::cin.fail() ) {
			break;
		}

		if( strcmp( line, "quit" ) == 0 ||
			strcmp( line, "exit" ) == 0 ||
			strcmp( line, "bye" ) == 0 ) {
			std::cout << "Bye!" << std::endl;
			break;
		}

		if( !commandParser->ParseCommand( line, *pJob ) ) {
			std::cout << "\"" << line << "\"" << " Unknown command or command failure" << std::endl;
		}
	}

	// Tear down GUI emulation BEFORE releasing the job (the VFS is
	// attached to the rasterizer, whose lifetime is the job's).
	if( guiEmulRunning.load( std::memory_order_acquire ) ) {
		guiEmulRunning.store( false, std::memory_order_release );
		if( guiEmulPollThread.joinable() ) guiEmulPollThread.join();
	}
	if( guiEmulVfs ) {
		guiEmulVfs->SetTileCompleteCallback( nullptr );
		guiEmulVfs->release();
		guiEmulVfs = nullptr;
	}

	// Delete objects
	safe_release( commandParser );
	safe_release( pJob );

	GlobalLogCleanupAndShutdown();


	return 1;
}

