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
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include "../Library/RISE_API.h"
#include "../Library/Utilities/Log/StreamPrinter.h"
#include "../Library/Agent/AgentSession.h"
#include "../Library/Agent/AgentRpc.h"
#include "../Library/Agent/AgentMcpAdapter.h"
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
static int RunAgentStdio( const char* sceneArg )
{
	std::unique_ptr<RISE::Agent::AgentSession> session;
	if( sceneArg && sceneArg[0] ) {
		session = RISE::Agent::AgentSession::LoadFromFile( sceneArg );
		// A failed load is NOT fatal: report it on stderr and continue with a
		// null session (read_schema-less methods still error cleanly) so a CI
		// caller gets structured errors rather than a silent process death.
		if( !session ) {
			std::cerr << "rise --agent-stdio: could not load scene '" << sceneArg
			          << "' (not native-v7, or a derive error); continuing with no head.\n";
		}
	}

	RISE::Agent::AgentRpcDispatcher dispatcher( std::move( session ) );

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
// the identical 12 agent verbs (initialize / tools/list / tools/call
// replace the raw JSON-RPC method names an MCP client would otherwise
// have to know).  Same stdout hygiene as RunAgentStdio (the caller's
// early argv pre-scan already covers `--mcp` alongside `--agent-stdio`,
// so the banner/log suppression applies identically).
//
// MCP notifications (a request with no `id` field at all, e.g.
// `notifications/initialized`) get NO response line -- HandleLine
// signals that by returning an EMPTY string; this loop must skip writing
// a line in that case rather than emitting a blank line (which would
// itself be an invalid/ambiguous frame on the protocol pipe).
static int RunAgentMcpStdio( const char* sceneArg )
{
	std::unique_ptr<RISE::Agent::AgentSession> session;
	if( sceneArg && sceneArg[0] ) {
		session = RISE::Agent::AgentSession::LoadFromFile( sceneArg );
		if( !session ) {
			std::cerr << "rise --agent-stdio --mcp: could not load scene '" << sceneArg
			          << "' (not native-v7, or a derive error); continuing with no head.\n";
		}
	}

	RISE::Agent::AgentMcpAdapter adapter( std::move( session ) );

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

int main( int argc, char** argv )
{
	// Facet 5 slice 0c: pre-scan argv for `--agent-stdio` BEFORE anything is
	// written to stdout.  In agent mode stdout is a line-delimited JSON-RPC
	// transport and MUST carry protocol frames ONLY -- so the ASCII banner,
	// the RISE_MEDIA_PATH warning, and (below) the GlobalLog console sink are
	// all gated / redirected off stdout.  The authoritative flag parse still
	// happens later; this early scan only decides "is stdout a protocol pipe".
	bool agentMode = false;
	for( int ai = 1; ai < argc; ai++ ) {
		if( strcmp( argv[ai], "--agent-stdio" ) == 0 ) { agentMode = true; break; }
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
		// Agent mode: keep stdout clean, but the guidance is still useful --
		// route it to stderr so a caller diagnosing missing resources sees it.
		std::cerr << "rise --agent-stdio: 'RISE_MEDIA_PATH' is not set; "
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
	//   --width N         override Film width (pixels; positive integer)
	//   --height N        override Film height (pixels; positive integer)
	//   --pixel-ar X      override Film pixel aspect ratio (positive float)
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
		const int rc = cliMcp ? RunAgentMcpStdio( sceneArg ) : RunAgentStdio( sceneArg );
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

