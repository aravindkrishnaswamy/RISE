//////////////////////////////////////////////////////////////////////
//
//  PRISE_Scheduler.cpp - Contains the entry point for the Parallel
//    R.I.S.E. scheduler implementation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 5, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifdef WIN32
#include "../src/UI/ConsoleWin32.h"
#endif

#include <iostream>
#include "../src/Version.h"
#include "../src/Interfaces/ILogPriv.h"
#include "../src/Utilities/Threads/Threads.h"
#include "../src/Utilities/Communications/ServerSocketCommunicator.h"
#include "../src/PRISE/WorkerConnection.h"
#include "../src/PRISE/ISchedulerEngine.h"

static const char* secret_code = "PRISE_HANDSHAKE";
static const unsigned short port_number = 41337;

static const unsigned int num_workers = 2;
static const char * scene_filename = "../scenes/prisemeshes.scn";
static const char * output_filename = "../rendered/prise_meshes";
static const unsigned int width = 512;
static const unsigned int height = 512;
static const unsigned int granx = 64;
static const unsigned int grany = 64;

void* HandleRendererThreadConnectionProc( void* v )
{
	// The communicator
	WorkerConnection*	pConnection = (WorkerConnection*)(v);

	// All we do is sit back and process requests from the client
	while( 1 ) {
		if( !pConnection->ProcessWorkerRequest() ) {
			break;
		}
	}

	pConnection->RemoveRef();

	return 0;
}

//
// Handles a worker that has connected to the scheduler
//
void* HandleWorkerConnectionProc( void* v )
{
	// The communicator for the client
	ICommunicator*	pComm = (ICommunicator*)(v);

	// We just opened a new connection, we should do some handshaking
	if( pComm->IsConnectionOpen() )
	{
		WorkerConnection*	pConnection = new WorkerConnection( pComm );
		GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "worker connection" );

		if( !pConnection->PerformHandshaking( secret_code ) ) {
			GlobalLog()->PrintEasyError( "Failed to handshake with worker, abandoning" );
		}

		// Find out if we are dealing with a new worker or with an existing worker's
		// rendering thread
		int type=0;
		if( !pConnection->GetWorkerType( type ) ) {
			GlobalLog()->PrintEasyError( "Failed to get worker type" );
		}
	
		if( type == 1 ) {
			// Add this worker to the global list, and we're done!
			if( !MasterSchedulerEngine().AddNewWorker( pConnection ) ) {
				GlobalLog()->PrintEasyError( "Error adding new worker.  Something terribly wrong with master scheduler engine" );
			}
		} else if( type == 2 ) {
			// This is the rendering thread, and all it does is tell us stuff, so 
			// spawn a thread to listen to it and we're done
			pConnection->AddRef();
			MasterSchedulerEngine().IncrementRenderConnections();
			Threads::ifxCreateThread( HandleRendererThreadConnectionProc, pConnection, 0, 0, 0 );
		} else {
			GlobalLog()->PrintEasyError( "Unknown worker type" );
		}

		pConnection->RemoveRef();

	} else {
		GlobalLog()->PrintEasyError( "Broken connection to worker, terminiating session" );
	}

	pComm->RemoveRef();
	return 0;
}

void ReceiveAllWorkers( const SOCKET sock )
{
	MasterSchedulerEngine().WaitForWorkers( num_workers );

	// Now we sit back and wait for the appropriate number of connections
	while( !MasterSchedulerEngine().ReadyToGo() )
	{
		SocketCommunicator* pComm = new ServerSocketCommunicator( sock );
		GlobalLog()->PrintNew( pComm, __FILE__, __LINE__, "communicator" );

		if( !pComm->IsConnectionOpen() ) {
			break;
		}

		// Once we have a connection, we start a thread to handle it, forget it about it, and
		// sit back and wait for a connection again
		IFXTHREADID tid;
		Threads::ifxCreateThread( HandleWorkerConnectionProc, (ICommunicator*)pComm, 0, 0, &tid );
		Threads::ifxWaitUntilThreadFinishes( tid, 0 );
	}

}

#ifdef WIN32

DWORD	WINAPI	WorkProc( LPVOID lpParameter )
{
	SOCKET	sock = (SOCKET)lpParameter;

	MasterSchedulerEngine().SetScene( scene_filename, output_filename, width, height, granx, grany );
//	MasterSchedulerEngine().AddModelToScene( Point3D(0.0,0.0,0.0), Vector3D( -90.0, 0.0, -20.0 ), "white", "../models/risemesh/skeleton" );
	MasterSchedulerEngine().AddModelToScene( Point3D(0.0,0.0,0.0), Vector3D( -90.0, 0.0, -20.0 ), "white", "../models/risemesh/crazytorusknot2cpu" );
//	MasterSchedulerEngine().AddModelToScene( Point3D(-6.5,-7.0,0.0), Vector3D( -90.0, 0.0, -10.0 ), "gold", "../models/risemesh/teapot" );

	ReceiveAllWorkers( sock );
	
	// Run the job
	MasterSchedulerEngine().Engage();
	
	return 1;
}

int APIENTRY WinMain(HINSTANCE hInstance,
                     HINSTANCE /*hPrevInstance*/,
                     LPSTR     lpCmdLine,
                     int       /*nCmdShow*/)
{
	SetGlobalLogFileName( "PRISE_Scheduler_Log.txt" );

	ConsoleWin32* pA = new ConsoleWin32( eLog_Win32Console, TYPICAL_PRIORITY, HIGHEST_PRIORITY );
	GlobalLog()->PrintNew( pA, __FILE__, __LINE__, "Win32 Console printer" );
	const char *szWndTitle = "Parallel Realistic Image Synthesis Engine (P.R.I.S.E) Scheduler Console";
	char szWindowName[1024];
	sprintf( szWindowName, "%s v. %d.%d.%d build %d", szWndTitle, RISE_VER_MAJOR_VERSION, RISE_VER_MINOR_VERSION, RISE_VER_REVISION_VERSION, RISE_VER_BUILD_VERSION );
	pA->Init( szWindowName, hInstance );
	GlobalLogPriv()->AddPrinter( pA );
	pA->RemoveRef();

	GlobalLog()->PrintEasyEvent( "============================================================" );
	GlobalLog()->PrintEasyEvent( "                                                            " );
	GlobalLog()->PrintEasyEvent( "    P.R.I.S.E - Parallel Realistic Image Synthesis Engine   " );
#ifdef _DEBUG
	GlobalLog()->PrintEasyEvent( "                  SCHEDULER -- DEBUG BUILD                  " );
#else
	GlobalLog()->PrintEasyEvent( "                        SCHEDULER                           " );
#endif
	GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, 
		                         "                   v. %d.%d.%d build %d", RISE_VER_MAJOR_VERSION, RISE_VER_MINOR_VERSION, RISE_VER_REVISION_VERSION, RISE_VER_BUILD_VERSION );
	GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, 
                                 "            built on %s at %s", __DATE__, __TIME__ );
	GlobalLog()->PrintEasyEvent( "           (c) 2001-2004 Aravind Krishnaswamy               " );
	GlobalLog()->PrintEasyEvent( "                                                            " );
	GlobalLog()->PrintEasyEvent( "============================================================" );


	// Start communications
	SocketComm::InitializeSocketCommunications();

	SOCKET	sock = SocketComm::EstablishConnection( port_number, IPPROTO_TCP, SOCK_STREAM );	

	if( sock == BAD_SOCKET ) {
		GlobalLog()->PrintEasyError( "Couldn't establish proper connection to socket" );
	} else {
		CreateThread( 0, 0, WorkProc, (void*)sock, 0, 0 );
	}

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 1;
}

#else

int main( int argc, char** argv )
{
	SetGlobalLogFileName( "PRISE_Scheduler_Log.txt" );

	std::cout << "============================================================" << std::endl;
	std::cout << "                                                            " << std::endl;
	std::cout << "    P.R.I.S.E - Parallel Realistic Image Synthesis Engine   " << std::endl;
#ifdef _DEBUG
	std::cout << "                  SCHEDULER -- DEBUG BUILD                  " << std::endl;
#else
	std::cout << "                        SCHEDULER                           " << std::endl;
#endif
	std::cout << "                   v." << RISE_VER_MAJOR_VERSION  << "." << RISE_VER_MINOR_VERSION << "." << RISE_VER_REVISION_VERSION << " build " << RISE_VER_BUILD_VERSION << std::endl;
	std::cout << "            built on " << __DATE__ << " at " << __TIME__ << std::endl;
	std::cout << "           (c) 2001-2004 Aravind Krishnaswamy               " << std::endl;
	std::cout << "                                                            " << std::endl;
	std::cout << "============================================================" << std::endl;


	// Start communications
	SocketComm::InitializeSocketCommunications();

	SOCKET	sock = SocketComm::EstablishConnection( port_number, IPPROTO_TCP, SOCK_STREAM );

	if( sock == BAD_SOCKET ) {
		GlobalLog()->PrintEasyError( "Fatal error, EstablishConnection returned bad socket" );
	} else {
		MasterSchedulerEngine().SetScene( scene_filename, output_filename, width, height, granx, grany );
		MasterSchedulerEngine().AddModelToScene( Point3D(0.0,0.0,0.0), Vector3D( -90.0, 0.0, -20.0 ), "white", "../models/risemesh/crazytorusknot2cpu" );
		ReceiveAllWorkers( sock );
		MasterSchedulerEngine().Engage();
	}

	return 1;
}

#endif

