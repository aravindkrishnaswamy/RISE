//////////////////////////////////////////////////////////////////////
//
//  MCPClientConnection.cpp - Implements the MCP client 
//    connection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 26, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MCPClientConnection.h"

using namespace RISE;

MCPClientConnection::MCPClientConnection( ICommunicator* pCommunicator_ ) : 
  ClientConnection( pCommunicator_ )
{
}

MCPClientConnection::~MCPClientConnection()
{
}

void MCPClientConnection::PerformClientTasks()
{
	// The basic type does nothing, disconnect
	Disconnect();
}

