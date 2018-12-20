//////////////////////////////////////////////////////////////////////
//
//  DRISE_Server.cpp - Contains the entry point for the Distributed
//    R.I.S.E. server implementation
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
#include "../Library/Interfaces/ILogPriv.h"
#include "../Library/Utilities/Threads/Threads.h"
#include "../Library/Utilities/Communications/ServerSocketCommunicator.h"
#include "../Library/Utilities/MediaPathLocator.h"
#include "ClientConnection.h"
#include "WorkerClientConnection.h"
#include "SubmitterClientConnection.h"
#include "MCPClientConnection.h"

using namespace RISE;

static const char* secret_code = "DRISE_HANDSHAKE";

void* HandleConnectionProc( void* v )
{
	// The socket for the client
	ICommunicator*	pComm = (ICommunicator*)(v);

	// We just opened a new connection, we should do some handshaking
	if( pComm->IsConnectionOpen() )
	{
		ClientConnection*	pConnection = new ClientConnection( pComm );
		GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "client connection" );

		if( !pConnection->PerformHandshaking( secret_code ) ) {
			GlobalLog()->PrintEasyError( "Failed to handshake with client, abandoning" );
		}
				
		// We should first give the client an opportunity to 
		// identify itself.  

		//
		// We have three different types of clients:
		// - worker client, purely a worker
		// - job submission client, a client that submits jobs
		// - master control program (MCP) client, in which case the server responds to 
		//   all kinds of queries from this kind of client
		//
		CLIENT_TYPE ctype = pConnection->GetClientType();

		switch( ctype )
		{
		case eClient_Worker:
//			GlobalLog()->PrintEasyEvent( "Talking to worker client" );
			pConnection->release();
			pConnection = new WorkerClientConnection( pComm );
			GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "worker client connection" );
			break;

		case eClient_Submitter:
			GlobalLog()->PrintEasyEvent( "Talking to job submission client" );
			pConnection->release();
			pConnection = new SubmitterClientConnection( pComm );
			GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "submitter client connection" );
			break;

		case eClient_MCP:
			GlobalLog()->PrintEasyEvent( "Talking to master control program" );
			pConnection->release();
			pConnection = new MCPClientConnection( pComm );
			GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "MCP client connection" );
			break;

		default:
			GlobalLog()->PrintEasyEvent( "Unknown client type" );
			break;
		}

		// Now just sit back and let each of the client types do their thing
		pConnection->PerformClientTasks();
		pConnection->release();

	} else {
		GlobalLog()->PrintEasyError( "Broken connection to client, terminiating session" );
	}

	pComm->release();
	return 0;
}

#ifdef WIN32

DWORD	WINAPI	WorkProc( LPVOID lpParameter )
{
	SOCKET	sock = (SOCKET)lpParameter;

	// All a server does is sit back and wait for a connection
	while( 1 )
	{
		SocketCommunicator* pComm = new ServerSocketCommunicator( sock );
		GlobalLog()->PrintNew( pComm, __FILE__, __LINE__, "communicator" );

		if( !pComm->IsConnectionOpen() ) {
			break;
		}

		// Once we have a connection, we start a thread to handle it, forget it about it, and
		// sit back and wait for a connection again
		RISETHREADID tid;
		Threading::riseCreateThread( HandleConnectionProc, (ICommunicator*)pComm, 0, 0, &tid );

//		Threading::riseWaitUntilThreadFinishes( tid, 0 );
	}

	return 1;
}

int APIENTRY WinMain(HINSTANCE hInstance,
                     HINSTANCE /*hPrevInstance*/,
                     LPSTR     lpCmdLine,
                     int       /*nCmdShow*/)
{
	SetGlobalLogFileName( "DRISE_Server_Log.txt" );

	int major, minor, revision, build;
	bool isdebug;

	RISE_API_GetVersion( &major, &minor, &revision, &build, &isdebug );

	GlobalLog()->PrintEasyEvent( "============================================================" );
	GlobalLog()->PrintEasyEvent( "                                                            " );
	GlobalLog()->PrintEasyEvent( "  D.R.I.S.E - Distributed Realistic Image Synthesis Engine  " );
	GlobalLog()->PrintEasyEvent( "                         SERVER                             " );
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

	// Start communications
	SocketComm::InitializeSocketCommunications();

	// Read the options file
	IOptions* pOptions = 0;
	RISE_API_CreateOptionsParser( &pOptions, "drise.options" );
	int port_number = pOptions->ReadInt( "port_number", 41337 );
	pOptions->release();

	SOCKET	sock = SocketComm::EstablishConnection( port_number, IPPROTO_TCP, SOCK_STREAM );

	CreateThread( 0, 0, WorkProc, (void*)sock, 0, 0 );


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
	SetGlobalLogFileName( "DRISE_Server_Log.txt" );

	int major, minor, revision, build;
	bool isdebug;

	RISE_API_GetVersion( &major, &minor, &revision, &build, &isdebug );

	std::cout << "============================================================" << std::endl;
	std::cout << "                                                            " << std::endl;
	std::cout << "  D.R.I.S.E - Distributed Realistic Image Synthesis Engine  " << std::endl;
	std::cout << "                         SERVER                             " << std::endl;
	std::cout << "              v." << major  << "." << minor << "." << revision << " build " << build << std::endl;
	std::cout << "      built on " << __DATE__ << " at " << __TIME__ << std::endl;
	if( isdebug ) {
		std::cout <<  
			     "             DEBUG version" << std::endl;
	}
	std::cout << "     (c) 2001-2005 Aravind Krishnaswamy        " << std::endl;
	std::cout << "============================================================" << std::endl;


	// Start communications
	SocketComm::InitializeSocketCommunications();
	
	// Read the options file
	IOptions* pOptions = 0;
	RISE_API_CreateOptionsParser( &pOptions, "drise.options" );
	int port_number = pOptions->ReadInt( "port_number", 41337 );
	pOptions->release();

	SOCKET	sock = SocketComm::EstablishConnection( port_number, IPPROTO_TCP, SOCK_STREAM   );

	// All a server does is sit back and wait for a connection
	while( 1 )
	{
//		GlobalLog()->PrintEasyInfo( "Waiting for client" );
		SocketCommunicator* pComm = new ServerSocketCommunicator( sock );
		GlobalLog()->PrintNew( pComm, __FILE__, __LINE__, "communicator" );

		if( !pComm->IsConnectionOpen() ) {
			break;
		}

		// Once we have a connection, we start a thread to handle it, forget it about it, and
		// sit back and wait for a connection again
		RISETHREADID tid;
		Threading::riseCreateThread( HandleConnectionProc, (ICommunicator*)pComm, 0, 0, &tid );
		Threading::riseWaitUntilThreadFinishes( tid, 0 );
	}

	return 1;
}

#endif

