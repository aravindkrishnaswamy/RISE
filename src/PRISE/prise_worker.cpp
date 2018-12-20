//////////////////////////////////////////////////////////////////////
//
//  PRISE_Worker.cpp - Contains the entry point for the Parallel
//    R.I.S.E. worker implementation
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
#include "../src/Utilities/Communications/ClientSocketCommunicator.h"
#include "../src/PRISE/SchedulerConnection.h"
#include "../src/PRISE/WorkerRenderer.h"
#include "../src/PRISE/WorkQueue.h"

static const char* secret_code = "PRISE_HANDSHAKE";
static const unsigned short port_number = 41337;

IFXMUTEX	mutRenderingComplete;
static WorkQueue*	workqueue = new WorkQueue();

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
		Ray	r;
		unsigned int x, y;
		IMemoryBuffer*		mb = 0;
		while( workqueue->GetFront( r, x, y, mb ) )
		{
			if( mb ) {
				pRenderer->FinishIncompleteRay( r, x, y, mb );
				mb->RemoveRef();
				mb = 0;
			} else {
				pRenderer->Render( r, x, y );				
			}
		}

		// This means we are done
		if( Threads::ifxMutexTryLock( mutRenderingComplete ) ) {
			exit = true;
		} else {
//			GlobalLog()->PrintEasyWarning( "Worker starved" );
		}
	}

	pRenderer->GetScheduler()->SendRenderComplete();

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

	if( !pConnection->SendWorkerType( 1 ) ) {
		GlobalLog()->PrintEasyError( "Failed to send worker type" );
		pConnection->RemoveRef();
		pComm->RemoveRef();
		return;
	}

	// At this point, establish another connection to the scheduler, this will be the renderer's
	// connection
	ICommunicator*		pRenderComm = new ClientSocketCommunicator( "127.0.0.1", port_number, SOCK_STREAM );
	GlobalLog()->PrintNew( pRenderComm, __FILE__, __LINE__, "communicator" );

	SchedulerConnection*	pRenderConnection = new SchedulerConnection( pRenderComm );
	GlobalLog()->PrintNew( pRenderConnection, __FILE__, __LINE__, "scheduler connection" );

	if( !pRenderConnection->PerformHandshaking( secret_code ) ) {
		GlobalLog()->PrintEasyError( "Failed to handshake with scheduler, abandoning" );
		pConnection->RemoveRef();
		pComm->RemoveRef();
		pRenderConnection->RemoveRef();
		pRenderComm->RemoveRef();
		return;
	}

	if( !pRenderConnection->SendWorkerType( 2 ) ) {
		GlobalLog()->PrintEasyError( "Failed to send worker type" );
		pConnection->RemoveRef();
		pComm->RemoveRef();
		pRenderConnection->RemoveRef();
		pRenderComm->RemoveRef();
		return;
	}

	GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, "Talking to scheduler" );

	// The scheduler is going to wait for everyone to arrive before doling out work, so 
	// we just sit back and wait for the name of the file we are going to work on
	String filename;
	if( !pConnection->GetSceneFilename( filename ) ) {
		GlobalLog()->PrintEasyError( "Failed to get filename from scheduler, quitting" );
		return;
	}

	std::deque<MODEL_INFO>	models;
	if( !pConnection->GetSceneModels( models ) ) {
		GlobalLog()->PrintEasyError( "Failed to get models form scheduler, quitting" );
		return;
	}

	// Create the worker renderer and pass the filename to that
	WorkerRenderer*		pRenderer = new WorkerRenderer( filename.c_str(), models, pRenderConnection );

	mutRenderingComplete = Threads::ifxCreateMutex();
	Threads::ifxMutexLock( mutRenderingComplete );

	// Spawn the renderer into its own thread that continues to work, while we sit
	// here and wait for new tiles or new rays from the scheduler
	IFXTHREADID	tid;
	Threads::ifxCreateThread( RendererFunc, pRenderer, 0, 0, &tid ); 

	while( pComm->IsConnectionOpen() ) {
		// Prccess requests from the scheduler, much of our time should
		// be spent blocked here
		int ret = pConnection->ProcessSchedulerRequest( pRenderer, *workqueue );

		if( ret == -1 ) {
			// Something went fatally wrong
			GlobalLog()->PrintEasyError( "Fatal error processing scheduler request, quitting" );
			break;
		}

		if( ret == 2 ) {
			// We are done rendering, transmit the results
			Threads::ifxMutexUnlock( mutRenderingComplete );
			break;
		}
	}

	//
	// Now we have to wait for the rendering thread to complete
	//
	Threads::ifxWaitUntilThreadFinishes( tid, 0 ); 
	Threads::ifxDestroyMutex( mutRenderingComplete );

	// Now we return all our processed data to the server.
	if( !pConnection->SendRenderComplete() ) {
		GlobalLog()->PrintEasyError( "Fatal error could tell scheduler we are done rendering" );
	}
//	pConnection->SendResults( *workqueue, *pRenderer );
	pRenderer->RemoveRef();

	// Now we're done!
	GlobalLog()->PrintEasyEvent( "Work complete" );
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
	SetGlobalLogFileName( "PRISE_Worker_Log.txt" );

	ConsoleWin32* pA = new ConsoleWin32( eLog_Win32Console, TYPICAL_PRIORITY, HIGHEST_PRIORITY );
	GlobalLog()->PrintNew( pA, __FILE__, __LINE__, "Win32 Console printer" );
	const char *szWndTitle = "Parallel Realistic Image Synthesis Engine (P.R.I.S.E) Worker Console";
	char szWindowName[1024];
	sprintf( szWindowName, "%s v. %d.%d.%d build %d", szWndTitle, RISE_VER_MAJOR_VERSION, RISE_VER_MINOR_VERSION, RISE_VER_REVISION_VERSION, RISE_VER_BUILD_VERSION );
	pA->Init( szWindowName, hInstance );
	GlobalLogPriv()->AddPrinter( pA );
	pA->RemoveRef();

	GlobalLog()->PrintEasyEvent( "============================================================" );
	GlobalLog()->PrintEasyEvent( "                                                            " );
	GlobalLog()->PrintEasyEvent( "    P.R.I.S.E - Parallel Realistic Image Synthesis Engine   " );
#ifdef _DEBUG
	GlobalLog()->PrintEasyEvent( "                   WORKER -- DEBUG BUILD                    " );
#else
	GlobalLog()->PrintEasyEvent( "                         WORKER                             " );
#endif
	GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, 
		                         "                   v. %d.%d.%d build %d", RISE_VER_MAJOR_VERSION, RISE_VER_MINOR_VERSION, RISE_VER_REVISION_VERSION, RISE_VER_BUILD_VERSION );
	GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, 
                                 "            built on %s at %s", __DATE__, __TIME__ );
	GlobalLog()->PrintEasyEvent( "           (c) 2001-2004 Aravind Krishnaswamy               " );
	GlobalLog()->PrintEasyEvent( "                                                            " );
	GlobalLog()->PrintEasyEvent( "============================================================" );

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
	SetGlobalLogFileName( "PRISE_Worker_Log.txt" );
	std::cout << "============================================================" << std::endl;
	std::cout << "                                                            " << std::endl;
	std::cout << "    P.R.I.S.E - Parallel Realistic Image Synthesis Engine   " << std::endl;
#ifdef _DEBUG
	std::cout << "                   WORKER -- DEBUG BUILD                    " << std::endl;
#else
	std::cout << "                         WORKER                             " << std::endl;
#endif
	std::cout << "                   v." << RISE_VER_MAJOR_VERSION  << "." << RISE_VER_MINOR_VERSION << "." << RISE_VER_REVISION_VERSION << " build " << RISE_VER_BUILD_VERSION << std::endl;
	std::cout << "            built on " << __DATE__ << " at " << __TIME__ << std::endl;
	std::cout << "           (c) 2001-2004 Aravind Krishnaswamy               " << std::endl;
	std::cout << "                                                            " << std::endl;
	std::cout << "============================================================" << std::endl;

	// Start communications
	SocketComm::InitializeSocketCommunications();
	DoWorkerSpecificStuff();
	
	return 0;
}

#endif

