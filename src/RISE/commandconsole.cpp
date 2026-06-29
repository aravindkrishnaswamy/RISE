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
#include <mutex>
#include <thread>
#include <vector>
#include "../Library/RISE_API.h"
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

// P5 (Model-B): route a USER-scene load through the canonical CST path when RISE_LOAD_VIA_CST is set
// in the environment (else the legacy streaming parser).  The CST path refuses non-native-v7 / derive-
// error scenes (returns false -- NO silent fallback, so a failed CST load is visible, not masked).  This
// is the opt-in hook for rendering scene_variant scenes (the variant bake is CST-native); Slice-5 flips
// CST-load to the default (fallback policy per docs/agentic-redesign/62).
static bool LoadScenePerEnv( IJobPriv* pJob, const char* sceneArg )
{
	if( getenv( "RISE_LOAD_VIA_CST" ) ) {
		std::cout << "[RISE_LOAD_VIA_CST] loading via the Model-B CST path" << std::endl;
		return pJob->LoadAsciiSceneViaCst( sceneArg );
	}
	return pJob->LoadAsciiScene( sceneArg );
}

//
// Console version
//

double DoPerformanceRating()
{
	IJobPriv* pJob = 0;
	RISE_CreateJobPriv( &pJob );
	pJob->LoadAsciiScene( "scenes/pr.RISEscene" );

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

int main( int argc, char** argv )
{
	// Setup the media path locator
	const char* szmediapath = getenv( "RISE_MEDIA_PATH" );

	if( szmediapath ) {
		GlobalMediaPathLocator().AddPath( szmediapath );
	} else {
		std::cout << std::endl;
		std::cout << "Warning! the 'RISE_MEDIA_PATH' environment variable is not set." << std::endl;
		std::cout << "unless you have been very carefull with explicit media pathing," << std::endl;
		std::cout << "certain resources may not load.  See the README for information" << std::endl;
		std::cout << "on setting this path." << std::endl;
		std::cout << std::endl;
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

		if( strcmp(a, "--width") == 0 ) {
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

