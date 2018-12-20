//////////////////////////////////////////////////////////////////////
//
//  drise_submitter.cpp - Contains the entry point for the Distributed
//    R.I.S.E. simple job submitter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 1, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
#include "pch.h"
#include "../Library/Utilities/Communications/ClientSocketCommunicator.h"
#include "../Library/Interfaces/ILogPriv.h"
#include "SubmitterServerConnection.h"
#include "../Library/RISE_API.h"
#include "../Library/Utilities/MediaPathLocator.h"

#include <iostream>

using namespace RISE;

static const char* secret_code = "DRISE_HANDSHAKE";

//
// Test submitter version of client specific stuff
//
void DoSubmitterSpecificStuff_Image( const char * szFileName, unsigned int x, unsigned int y, const char * szOutFile, unsigned int granx, unsigned int grany )
{
	// Read the options file
	IOptions* pOptions = 0;
	RISE_API_CreateOptionsParser( &pOptions, "drise.options" );

	String server_name = pOptions->ReadString( "server_name", String("default") );
	int port_number = pOptions->ReadInt( "port_number", 41337 );

	pOptions->release();

	// We try and contact the server
	ICommunicator*		pComm = new ClientSocketCommunicator( server_name.c_str(), port_number, SOCK_STREAM );
	GlobalLog()->PrintNew( pComm, __FILE__, __LINE__, "communicator" );

	SubmitterServerConnection* pConnection = new SubmitterServerConnection( pComm );
	GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "server connection" );

	if( !pConnection->PerformHandshaking( secret_code ) ) {
		GlobalLog()->PrintEasyError( "Failed to handshake with server, abandoning" );
	}

	if( !pConnection->ProcessServerRequest() ) {
		GlobalLog()->PrintEasyError( "Could tell the server we are a job submitter" );
	}

	GlobalLog()->PrintEasyEvent( "Attempting to submit job" );

	if( pConnection->SubmitJob( szFileName, x, y, szOutFile, granx, grany ) ) {
		GlobalLog()->PrintEasyEvent( "Job successfully submitted" );
	} else {
		GlobalLog()->PrintEasyEvent( "FAILED to submit job" );
	}

	pConnection->release();
	pComm->release();
}

void DoSubmitterSpecificStuff_Animation( const char * szFileName, unsigned int x, unsigned int y, const char * szOutFile, unsigned int frames )
{
	// Read the options file
	IOptions* pOptions = 0;
	RISE_API_CreateOptionsParser( &pOptions, "drise.options" );

	String server_name = pOptions->ReadString( "server_name", String("default") );
	int port_number = pOptions->ReadInt( "port_number", 41337 );

	pOptions->release();

	// We try and contact the server
	ICommunicator*		pComm = new ClientSocketCommunicator( server_name.c_str(), port_number, SOCK_STREAM );
	GlobalLog()->PrintNew( pComm, __FILE__, __LINE__, "communicator" );

	SubmitterServerConnection* pConnection = new SubmitterServerConnection( pComm );
	GlobalLog()->PrintNew( pConnection, __FILE__, __LINE__, "server connection" );

	if( !pConnection->PerformHandshaking( secret_code ) ) {
		GlobalLog()->PrintEasyError( "Failed to handshake with server, abandoning" );
	}

	if( !pConnection->ProcessServerRequest() ) {
		GlobalLog()->PrintEasyError( "Could tell the server we are a job submitter" );
	}

	GlobalLog()->PrintEasyEvent( "Attempting to submit job" );

	if( pConnection->SubmitAnimationJob( szFileName, x, y, szOutFile, frames ) ) {
		GlobalLog()->PrintEasyEvent( "Job successfully submitted" );
	} else {
		GlobalLog()->PrintEasyEvent( "FAILED to submit job" );
	}

	pConnection->release();
	pComm->release();
}


int main( int argc, char** argv )
{
	SetGlobalLogFileName( "DRISE_SimpleJobSubmitter_Log.txt" );

	// Start communications
	SocketComm::InitializeSocketCommunications();

	if( argc == 7 ) {
		DoSubmitterSpecificStuff_Image( argv[1], atoi(argv[2]), atoi(argv[3]), argv[4], atoi(argv[5]), atoi(argv[6]) );
	} else if( argc == 6 ) {
		DoSubmitterSpecificStuff_Animation( argv[1], atoi(argv[2]), atoi(argv[3]), argv[4], atoi(argv[5]) );
	} else {
		std::cout << "Usage: <file> <xres> <yres> <outfile> <xgranularity> <ygranularity>   -or-" << std::endl;
		std::cout << "       <file> <xres> <yres> <outfile> <frames>" << std::endl;
	}
	
	return 0;
}


