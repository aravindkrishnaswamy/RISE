//////////////////////////////////////////////////////////////////////
//
//  Message.h - Contains definitions for structs that define messages
//    also contains an enum for all the possible messages
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MESSAGE_
#define MESSAGE_

namespace RISE
{
	enum MESSAGE_TYPE {
		eMessage_None			= 0,				// No message
		eMessage_Header			= 1,				// Header only
		eMessage_Handshake		= 2,				// Handshake
		eMessage_Version		= 3,				// Version information
		eMessage_EverythingOK	= 4,				// Tells the client that everything is ok, and we can start talking
		eMessage_GetClientType	= 5,				// The server is requesting client type
		eMessage_ClientType		= 6,				// The client is sending its client type
		eMessage_Disconnect		= 7,				// Asked to disconnect
		eMessage_GetCompJobs	= 8,				// Asks the client to send completed jobs
		eMessage_CompletedJobs	= 9,				// Client is sending all compelted jobs
		eMessage_GetClientID	= 10,				// Asks the client for its ID if it has one
		eMessage_ClientID		= 11,				// Client is returning its ID
		eMessage_SubmitJobBasic	= 12,				// Client wants to submit a basic dumb and dirty job
		eMessage_SubmitOK		= 13,				// Server tells the client submission is ok
		eMessage_TaskIDs		= 14,				// Client/Server is sending the task id and task action ID for a new/completed job
		eMessage_CompTaskAction = 15,				// Client is sending a completed task action
		eMessage_HowMuchAction  = 16,				// Server is asking the client how many task actions it wants
		eMessage_ActionCount    = 17,				// Client is telling the server how much action it wants
		eMessage_TaskAction		= 18,				// Server is giving the client a single task action
		eMessage_SceneFile		= 19,				// Scheduler is sending worker the name of the scene to work on
		eMessage_NewCell		= 20,				// Scheduler is sending worker a new cell to add to its queue
		eMessage_DoneRendering	= 21,				// Scheduler is telling worker that we are done rendering to start sending results ASAP
		eMessage_GetQueueSize	= 22,				// Scheduler wants to know how big the worker queue is
		eMessage_QueueSize		= 23,				// Worker is telling scheduler the size of its queue
		eMessage_ModelCount		= 24,				// Scheduler is telling worker the number of models in scene
		eMessage_Model			= 25,				// Scheduler is telling worker the filename and details of the model(s)
		eMessage_WorkerType		= 26,				// The type of worker connection
		eMessage_WorkerResult	= 27,				// A computed resultant value from a worker
		eMessage_UnresolvedRay	= 28,				// An unresolved ray from a worker
		eMessage_SubmitJobAnim  = 29				// Client wants to submit an animation job
	};

	//
	// The message header is there for all messages, it tells us what kind of message we are
	// dealing with.  Note that most messages are simply char*, which must be mapped to a 
	// memory buffer before being read.  The reason they are mapped to memory buffers is so that
	// we can avoid endian related issues.
	//
	struct MESSAGE_HEADER
	{
		char		chType;							// Which message is this.  Why a char ?, to avoid endian issues
													// besides 255 should be enough.... for now... 
		char		bufferSize[4];					// Tells us how big the buffer containing the actual message is
	};
}

#endif

