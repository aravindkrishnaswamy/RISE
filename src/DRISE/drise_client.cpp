//////////////////////////////////////////////////////////////////////
//
//  DRISE_Client.cpp - Contains the entry point for the Distributed
//    R.I.S.E. client implementation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
#include "pch.h"

#include <iostream>
#include "../Library/RISE_API.h"
#include "../Library/Interfaces/IJobPriv.h"
#include "../Library/Interfaces/ILogPriv.h"
#include "../Library/Utilities/Threads/Threads.h"
#include "../Library/Utilities/Communications/ClientSocketCommunicator.h"
#include "../Library/Utilities/MediaPathLocator.h"
#include "../Library/Parsers/StdOutProgress.h"
#include "WorkerServerConnection.h"

using namespace RISE;

static const char* secret_code = "DRISE_HANDSHAKE";

//
// Windows version has a windows window that shows the render progress
//
#ifdef _WIN32
#include "../Library/Rendering/Win32WindowRasterizerOutput.h"
#include <windows.h>
#endif

//
// Worker version of client specific stuff
//

#undef INFINITE
#include "../Library/Interfaces/IRasterizerOutput.h"
#include "../Library/Utilities/Reference.h"
#include "../Library/Utilities/MemoryBuffer.h"

class StoreRasterizerOutput : public virtual IRasterizerOutput, public virtual Implementation::Reference
{
public:
	unsigned int			width;
	unsigned int			height;
	RISEColor*		pStoredOutput;

protected:
	virtual ~StoreRasterizerOutput( )
	{
		if( pStoredOutput ) {
			GlobalLog()->PrintDelete( pStoredOutput, __FILE__, __LINE__ );
			delete [] pStoredOutput;
		}
	}
	
public:
	StoreRasterizerOutput() :
	  width( 0 ),
	  height( 0 ),
	  pStoredOutput( 0 )
	{
	}

	virtual void OutputIntermediateImage( 
		const IRasterImage& pImage,
		const Rect* pRegion
		)
	{
		// We don't really do anything for intermediate output
	}
	
	virtual void OutputImage( 
		const IRasterImage& pImage,
		const Rect* pRegion, 
		const unsigned int frame
		)
	{
		if( !pStoredOutput ) {
			// Allocate our store
			width = pImage.GetWidth();
			height = pImage.GetHeight();
			pStoredOutput = new RISEColor[ width * height ];
			GlobalLog()->PrintNew( pStoredOutput, __FILE__, __LINE__, "store" );
		}

		// Copy
		if( pRegion ) {
			for( unsigned int y = pRegion->top; y<= pRegion->bottom; y++ ) {
				for( unsigned int x = pRegion->left; x<= pRegion->right; x++ ) {
					pStoredOutput[ y*width + x ] = pImage.GetPEL( x, y );
				}
			}
		} else {
			for( unsigned int y = 0; y < height; y++ ) {
				for( unsigned int x = 0; x < width; x++ ) {
					pStoredOutput[ y*width + x ] = pImage.GetPEL( x, y );
				}
			}
		}
	}
};

bool DoWorkerJob_Image( IMemoryBuffer* pBuffer, IMemoryBuffer*& pCompletedTaskBuffer )
{
	char szFileName[1024] = {0};

	pBuffer->getBytes( szFileName, 1024 );
	unsigned int xstart = pBuffer->getUInt();
	unsigned int xend = pBuffer->getUInt();
	unsigned int nScanlineStart = pBuffer->getUInt();
	unsigned int nScanlineEnd = pBuffer->getUInt();

	static IJobPriv* pJob = 0;

	static StoreRasterizerOutput* sro = new StoreRasterizerOutput();
	GlobalLog()->PrintNew( sro, __FILE__, __LINE__, "store rasterizer output" );
	

	static char szLastFileName[1024] = {0};

	if( strcmp( szFileName, szLastFileName ) == 0 ) {
		// Same scene, no need to reload...
	} else {
		// First try and load the scene
		std::cout << "Working on scene file: " << szFileName << std::endl;

		if( pJob ) {
			safe_release( pJob );
			safe_release( sro );
			sro = new StoreRasterizerOutput( );
			GlobalLog()->PrintNew( sro, __FILE__, __LINE__, "store rasterizer output" );
		}

		RISE_CreateJobPriv( &pJob );

		StdOutProgress progress( "Parsing scene: " );
		pJob->SetProgress( &progress );
		if( !pJob->LoadAsciiScene( szFileName ) ) {
			GlobalLog()->PrintEasyError( "ERROR! Given scene file doesn't exist on this machine, aborting" );
			safe_release( pJob );
			return false;
		}

		strncpy( szLastFileName, szFileName, 1024 );

		// Detach all existing render outputs
		pJob->GetRasterizer()->FreeRasterizerOutputs();

		// Now attach a new renderoutput, our render output
		pJob->GetRasterizer()->AddRasterizerOutput( sro );

#ifdef WIN32
		/* You don't really want this
		Sleep(30);
//		pRise->ParseLine( "add renderoutput window" );
		IRasterizerOutput* pWinRO = new Implementation::Win32WindowRasterizerOutput(
			pJob->GetScene()->GetCamera()->GetWidth(), pJob->GetScene()->GetCamera()->GetHeight(),
			50, 50, "D.R.I.S.E. Client Render Window" );

		pJob->GetRasterizer()->AddRasterizerOutput( pWinRO );
		safe_release( pWinRO );
		Sleep(30);
		*/
#endif
	}
	// Render the scanline
	unsigned int total_height = nScanlineEnd-nScanlineStart+1;
	unsigned int total_width = xend-xstart+1;

	pJob->RasterizeRegion( xstart, nScanlineStart, xend, nScanlineEnd );

	//
	// Copy the output to the completed task buffer
	//
	pCompletedTaskBuffer = new Implementation::MemoryBuffer( sizeof( RISEColor ) * total_width * total_height + sizeof( unsigned int ) * 4 );
	pCompletedTaskBuffer->setUInt( xstart );
	pCompletedTaskBuffer->setUInt( xend );
	pCompletedTaskBuffer->setUInt( nScanlineStart );
	pCompletedTaskBuffer->setUInt( nScanlineEnd );

	unsigned int rend_width = pJob->GetScene()->GetCamera()->GetWidth();
//	unsigned int rend_height = pJob->GetScene()->GetCamera()->GetHeight();

	for( unsigned int y=nScanlineStart; y<=nScanlineEnd; y++ ) {
		pCompletedTaskBuffer->setBytes( &sro->pStoredOutput[y*rend_width+xstart], sizeof( RISEColor ) * total_width );
	}

	return true;
}

bool DoWorkerJob_Animation( IMemoryBuffer* pBuffer, IMemoryBuffer*& pCompletedTaskBuffer )
{
	char szFileName[1024] = {0};

	pBuffer->getBytes( szFileName, 1024 );
	const unsigned int frame = pBuffer->getUInt();

	static IJobPriv* pJob = 0;

	static StoreRasterizerOutput* sro = new StoreRasterizerOutput();
	GlobalLog()->PrintNew( sro, __FILE__, __LINE__, "store rasterizer output" );
	
	static char szLastFileName[1024] = {0};

	if( strcmp( szFileName, szLastFileName ) == 0 ) {
		// Same scene, no need to reload...
	} else {
		// First try and load the scene
		std::cout << "Working on scene file: " << szFileName << std::endl;

		if( pJob ) {
			safe_release( pJob );
			safe_release( sro );
			sro = new StoreRasterizerOutput( );
			GlobalLog()->PrintNew( sro, __FILE__, __LINE__, "store rasterizer output" );
		}

		RISE_CreateJobPriv( &pJob );

		StdOutProgress progress( "Parsing scene: " );
		pJob->SetProgress( &progress );
		if( !pJob->LoadAsciiScene( szFileName ) ) {
			GlobalLog()->PrintEasyError( "ERROR! Given scene file doesn't exist on this machine, aborting" );
			safe_release( pJob );
			return false;
		}

		strncpy( szLastFileName, szFileName, 1024 );

		// Detach all existing render outputs
		pJob->GetRasterizer()->FreeRasterizerOutputs();

		// Now attach a new renderoutput, our render output
		pJob->GetRasterizer()->AddRasterizerOutput( sro );

#ifdef WIN32
		/* You don't really want this
		Sleep(30);
//		pRise->ParseLine( "add renderoutput window" );
		IRasterizerOutput* pWinRO = new Implementation::Win32WindowRasterizerOutput(
			pJob->GetScene()->GetCamera()->GetWidth(), pJob->GetScene()->GetCamera()->GetHeight(),
			50, 50, "D.R.I.S.E. Client Render Window" );

		pJob->GetRasterizer()->AddRasterizerOutput( pWinRO );
		safe_release( pWinRO );
		Sleep(30);
		*/
#endif
	}

	// Render the frame
	pJob->RasterizeAnimationUsingOptions( frame );

	//
	// Copy the output to the completed task buffer
	//
	const unsigned int total_width = pJob->GetScene()->GetCamera()->GetWidth();
	const unsigned int total_height = pJob->GetScene()->GetCamera()->GetHeight();
	pCompletedTaskBuffer = new Implementation::MemoryBuffer( sizeof( RISEColor ) * total_width * total_height + sizeof( unsigned int ) * 4 );
	
	pCompletedTaskBuffer->setBytes( &sro->pStoredOutput[0], sizeof( RISEColor ) * total_width * total_height );

	return true;
}

bool DoWorkerJob( IMemoryBuffer* pBuffer, IMemoryBuffer*& pCompletedTaskBuffer )
{
	// First lets parse the buffer to figure out what scene to render	
	const char job_type = pBuffer->getChar();
	
	switch( job_type )
	{
	case 0:
		return DoWorkerJob_Image( pBuffer, pCompletedTaskBuffer );
		break;
	case 1:
		return DoWorkerJob_Animation( pBuffer, pCompletedTaskBuffer );
		break;
	default:
		GlobalLog()->PrintEasyError( "ERROR! Unknown type of task buffer" );
		break;
	};

	return false;


}

void DoWorkerSpecificStuff()
{
	//
	// Ok so first, lets answer all the server's questions until it gives us a job, then we 
	//    will go off and do the job
	//
	IMemoryBuffer* pCompletedTaskBuffer = 0;

	unsigned int curtaskid = 0;
	unsigned int curtaskactionid = 0;
	IMemoryBuffer* pBuffer = 0;

	RISESLEEP	sleep = Threading::riseCreateSleep();

	// Read the options file
	IOptions* pOptions = 0;
	RISE_API_CreateOptionsParser( &pOptions, "drise.options" );

	String server_name = pOptions->ReadString( "server_name", String("default") );
	int port_number = pOptions->ReadInt( "port_number", 41337 );

	pOptions->release();

	while( 1 ) {
		// We try and contact the server
		ICommunicator*		pComm = new ClientSocketCommunicator( server_name.c_str(), port_number, SOCK_STREAM );
		GlobalLog()->PrintNew( pComm, __FILE__, __LINE__, "communicator" );

		WorkerServerConnection*	pConnection = new WorkerServerConnection( pComm );
		GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "server connection" );

		if( !pConnection->PerformHandshaking( secret_code ) ) {
			GlobalLog()->PrintEasyError( "Failed to handshake with server, abandoning" );
			safe_release( pConnection );
			safe_release( pComm );
			return;
		}

		while( pComm->IsConnectionOpen() ) {

			int nProcessStatus = pConnection->ProcessServerRequest( curtaskid, curtaskactionid, pBuffer,
				                               curtaskid, curtaskactionid, pCompletedTaskBuffer );
			if( nProcessStatus == -1 ) {
				// Bad
				safe_release( pConnection );
				safe_release( pComm );
				return;
			}

			if( nProcessStatus == 1 ) {
				// Yay! we got a job
				break;
			}
		}

		// If there is a buffer, then we must have a job, so disconnect from the server and do the job
		pComm->CloseConnection();
		pComm->release();
		safe_release( pConnection );

		// Now do the job
		safe_release( pCompletedTaskBuffer );

		if( !pBuffer ) {
//			GlobalLog()->PrintEasyInfo( "The server has no jobs currently, try again later" );
			Threading::riseSleep( sleep, 30000 );			
//			Threading::riseSleep( sleep, 500 );
		} else {
			DoWorkerJob( pBuffer, pCompletedTaskBuffer );
		}

		safe_release( pBuffer );
	}

	Threading::riseDestroySleep( sleep );
}

#ifdef WIN32

DWORD	WINAPI	WorkProc( LPVOID lpParameter )
{
	DoWorkerSpecificStuff();

	return 1;
}

int APIENTRY WinMain(HINSTANCE hInstance,
                     HINSTANCE /*hPrevInstance*/,
                     LPSTR     lpCmdLine,
                     int       /*nCmdShow*/)
{
	SetGlobalLogFileName( "DRISE_Client_Log.txt" );

	int major, minor, revision, build;
	bool isdebug;

	RISE_API_GetVersion( &major, &minor, &revision, &build, &isdebug );

	GlobalLog()->PrintEasyEvent( "============================================================" );
	GlobalLog()->PrintEasyEvent( "                                                            " );
	GlobalLog()->PrintEasyEvent( "  D.R.I.S.E - Distributed Realistic Image Synthesis Engine  " );
	GlobalLog()->PrintEasyEvent( "                         CLIENT                             " );
	GlobalLog()->PrintEx( eLog_Event, 
		                         "                   v. %d.%d.%d build %d", major, minor, revision, build );
	GlobalLog()->PrintEx( eLog_Event, 
                                 "            built on %s at %s", __DATE__, __TIME__ );
	if( isdebug ) {
		GlobalLog()->PrintEx( eLog_Event, 
								"                       DEBUG version" );
	}
	GlobalLog()->PrintEasyEvent( "           (c) 2001-2005 Aravind Krishnaswamy               " );
	GlobalLog()->PrintEasyEvent( "                                                            " );
	GlobalLog()->PrintEasyEvent( "============================================================" );


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

#ifdef _WIN32
	{
		char myfilename[1024] = {0};
		GetModuleFileName( NULL, myfilename, 1023 );
		char mypath[1024] = {0};
		{
			char drive[_MAX_PATH] = {0};
			char dir[_MAX_PATH] = {0};
			_splitpath( myfilename, drive, dir, 0, 0 );
			_makepath( mypath, drive, dir, 0, 0 );
		}

		GlobalMediaPathLocator().AddPath( mypath );
	}
#endif

	SetPriorityClass( GetCurrentProcess(), IDLE_PRIORITY_CLASS );

	srand( GetTickCount() );

	// Start communications
	SocketComm::InitializeSocketCommunications();
	
	CreateThread( 0, 0, WorkProc, 0, 0, 0 );

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return msg.wParam;
}

#else 

int main( int argc, char** argv )
{
	SetGlobalLogFileName( "DRISE_Client_Log.txt" );

	int major, minor, revision, build;
	bool isdebug;

	RISE_API_GetVersion( &major, &minor, &revision, &build, &isdebug );

	std::cout << "============================================================" << std::endl;
	std::cout << "                                                            " << std::endl;
	std::cout << "  D.R.I.S.E - Distributed Realistic Image Synthesis Engine  " << std::endl;
	std::cout << "                         CLIENT                             " << std::endl;
	std::cout << "              v." << major  << "." << minor << "." << revision << " build " << build << std::endl;
	std::cout << "      built on " << __DATE__ << " at " << __TIME__ << std::endl;
	if( isdebug ) {
		std::cout <<  
			     "             DEBUG version" << std::endl;
	}
	std::cout << "     (c) 2001-2005 Aravind Krishnaswamy        " << std::endl;
	std::cout << "                                                            " << std::endl;
	std::cout << "============================================================" << std::endl;

	// Start communications
	SocketComm::InitializeSocketCommunications();

	// Do the handshaking
	DoWorkerSpecificStuff();
	
	return 0;
}

#endif
