//////////////////////////////////////////////////////////////////////
//
//  PRISE_MPI_Worker.cpp - Contains the entry point for the Parallel
//    R.I.S.E. worker implementation which uses MPI to communicate
//    between the workers to render massive scenes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 10, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include "../src/Version.h"
#include "../src/Interfaces/ILogPriv.h"
#include "../src/Utilities/Log/Log.h"
#include "../src/Utilities/Log/StreamPrinter.h"
#include "../src/Utilities/Threads/Threads.h"
#include "../src/Utilities/Communications/ClientSocketCommunicator.h"
#include "../src/PRISE/SchedulerConnection.h"
#include "../src/PRISE/WorkerRenderer.h"
#include "../src/PRISE/WorkQueue.h"

#include "mpi.h"

static const char* secret_code = "PRISE_HANDSHAKE";
static const unsigned short port_number = 41337;

IFXMUTEX	mutRenderingComplete;
static WorkQueue*	workqueue = new WorkQueue();

using namespace Implementation;

void* RendererFunc( void* v )
{
	WorkerRenderer* pRenderer = (WorkerRenderer*) v;

	// We keep going, we keep this thread running under two conditions:
	//   1.  There is more stuff to render
	//   2.  There is no more stuff to render HOWEVER the finished signal
	//         hasn't been set, meaning there will be more

	bool exit = false;
	while( !exit )
	{
		// Otherwise chomp on the queue
		Rect	rc;
		while( workqueue->GetFront( rc ) ) {
			pRenderer->Render( rc );
		}

		// This means we are done
		if( ifxMutexTryLock( mutRenderingComplete ) ) {
			exit = true;
		} else {
			GlobalLog()->PrintEasyInfo( "Worker starved" );
		}
	}

	return 0;
}

void DoWorkerSpecificStuff()
{
	// Connection is always local
	ICommunicator*		pComm = new ClientSocketCommunicator( "127.0.0.1", port_number, SOCK_STREAM );
	GlobalLog()->PrintNew( pComm, __FILE__, __LINE__, "communicator" );

	SchedulerConnection*	pConnection = new SchedulerConnection( pComm );
	GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "scheduler connection" );

	if( !pConnection->PerformHandshaking( secret_code ) ) {
		GlobalLog()->PrintEasyError( "Failed to handshake with scheduler, abandoning" );
		pConnection->RemoveRef();
		pComm->RemoveRef();
		return;
	}

	GlobalLog()->PrintEasyEvent( "Talking to scheduler" );

	// The scheduler is going to wait for everyone to arrive before doling out work, so 
	// we just sit back and wait for the name of the file we are going to work on
	String filename;
	if( !pConnection->GetSceneFilename( filename ) ) {
		GlobalLog()->PrintEasyError( "Failed to get filename from scheduler, quitting" );
		return;
	}

	// Create the worker renderer and pass the filename to that
	WorkerRenderer*		pRenderer = new WorkerRenderer( filename.c_str() );

	mutRenderingComplete = ifxCreateMutex();
	ifxMutexLock( mutRenderingComplete );

	// Spawn the renderer into its own thread that continues to work, while we sit
	// here and wait for new tiles or new rays from the scheduler
	IFXTHREADID	tid;
	ifxCreateThread( RendererFunc, pRenderer, 0, 0, &tid ); 

	while( pComm->IsConnectionOpen() ) {
		// Prccess requests from the scheduler, much of our time should
		// be spent blocked here
		int ret = pConnection->ProcessSchedulerRequest( *workqueue );

		if( ret == -1 ) {
			// Something went fatally wrong
			GlobalLog()->PrintEasyError( "Fatal error processing scheduler request, quitting" );
			break;
		}

		if( ret == 2 ) {
			// We are done rendering, transmit the results
			ifxMutexUnlock( mutRenderingComplete );
			break;
		}
	}

	//
	// Now we have to wait for the rendering thread to complete
	//
	ifxWaitUntilThreadFinishes( tid, 0 ); 
	ifxDestroyMutex( mutRenderingComplete );

	// Now we return all our processed data to the server.
	pConnection->SendResults( *workqueue, *pRenderer );
	pRenderer->RemoveRef();

	// Now we're done!
	GlobalLog()->PrintEasyEvent( "Work complete" );
}

int main( int argc, char** argv )
{
	// Intialize MPI, get our rank and size
	int rank,size;

	MPI_Init( &argc, &argv );
	MPI_Comm_rank( MPI_COMM_WORLD, &rank );
	MPI_Comm_size( MPI_COMM_WORLD, &size );

	// Disable aspects of the logger, so that different processes don't conflict on the same log file
	GlobalLogPriv()->RemoveAllPrinters();

	char buf[1024];
	sprintf( buf, "PRISE_Worker_Log_Proc%d.txt", rank );
	std::ofstream*		fs = new std::ofstream( buf );
	StreamPrinter*		pB = new StreamPrinter( fs, true );
	GlobalLog()->PrintNew( pB, __FILE__, __LINE__, "file stream printer" );
	GlobalLogPriv()->AddPrinter( pB );
	pB->RemoveRef();

	// If we are processor 0, then add a standard out printer
	if( rank == 0 ) {
		StreamPrinter*	pA = new StreamPrinter( &std::cout, true, eLog_Win32Console, TYPICAL_PRIORITY, HIGHEST_PRIORITY );
		GlobalLog()->PrintNew( pA, __FILE__, __LINE__, "cout stream printer" );
		GlobalLogPriv()->AddPrinter( pA );
		pA ->RemoveRef();
	}

	if( rank == 0 ) {
		std::cout << "============================================================" << std::endl;
		std::cout << "                                                            " << std::endl;
		std::cout << "    P.R.I.S.E - Parallel Realistic Image Synthesis Engine   " << std::endl;
#ifdef _DEBUG
		std::cout << "                   WORKER -- DEBUG BUILD                    " << std::endl;
#else
		std::cout << "                         WORKER                             " << std::endl;
#endif
		std::cout << "          Message Passing Interface (MPI) version           " << std::endl;
		std::cout << "                   v." << RISE_VER_MAJOR_VERSION  << "." << RISE_VER_MINOR_VERSION << "." << RISE_VER_REVISION_VERSION << " build " << RISE_VER_BUILD_VERSION << std::endl;
		std::cout << "            built on " << __DATE__ << " at " << __TIME__ << std::endl;
		std::cout << "           (c) 2001-2004 Aravind Krishnaswamy               " << std::endl;
		std::cout << "                                                            " << std::endl;
		std::cout << "============================================================" << std::endl;
	}

	// Start communications
	InitializeSocketCommunications();
	DoWorkerSpecificStuff();

	// Free MPI
	MPI_Finalize();
	
	return 0;
}

