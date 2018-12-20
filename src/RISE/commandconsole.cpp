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
#include "../Library/RISE_API.h"
#include "../Library/Interfaces/ILogPriv.h"
#include "../Library/Interfaces/IOptions.h"
#include "../Library/Interfaces/IJobPriv.h"
#include "../Library/Utilities/RTime.h"
#include "../Library/Utilities/MediaPathLocator.h"
#include "../Library/Parsers/StdOutProgress.h"

//
// Windows version has a windows window that shows the render progress
//
#ifdef _WIN32
#include "../Library/Rendering/Win32WindowRasterizerOutput.h"
#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
#define _WIN32_WINNT 0x0400		// require NT4 or greater for TryEnterCriticalSection
#include <windows.h>
#endif

using namespace RISE;

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
	
	// We should initialize the options file
	IOptions& options = GlobalOptions();

	// Lets get this job rolling
	IJobPriv* pJob = 0;
	RISE_CreateJobPriv( &pJob );

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
		} else {
#ifdef _WIN32

//			SetPriorityClass( GetCurrentProcess(), IDLE_PRIORITY_CLASS );

			char fullname[_MAX_PATH] = {0};
			GetLongPathName( argv[1], fullname, _MAX_PATH );
			std::cout << "Loading ascii scene file: " << fullname << std::endl;

			StdOutProgress progress( "Parsing scene: " );
			pJob->SetProgress( &progress );
			if( pJob->LoadAsciiScene( argv[1] ) ) {
				const ICamera* pCamera = pJob->GetScene()->GetCamera();
				if( pCamera ) {

					// Don't do this if it is disabled in the options file
					if( !options.ReadBool( "no_windowed_rasterizer_output", false ) ) {
						IRasterizerOutput* pWinRO = new Implementation::Win32WindowRasterizerOutput(
							pCamera->GetWidth(), pCamera->GetHeight(),
							50, 50, "R.I.S.E. Render Window" );

						GlobalLog()->PrintNew( pWinRO, __FILE__, __LINE__, "window RO" );
						pJob->GetRasterizer()->AddRasterizerOutput( pWinRO );
						safe_release( pWinRO );
					}
				}
			}

#else
			std::cout << "Loading ascii scene file: " << argv[1] << std::endl;
			pJob->LoadAsciiScene( argv[1] );
#endif
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

	// Delete objects
	safe_release( commandParser );
	safe_release( pJob );

	GlobalLogCleanupAndShutdown();


	return 1;
}

