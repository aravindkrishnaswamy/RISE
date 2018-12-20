//////////////////////////////////////////////////////////////////////
//
//  SchedulerConnection.h - Defines a worker that is talking to the
//    master scheduler
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 4, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCHEDULER_CONNECTION_
#define SCHEDULER_CONNECTION_

#include "../DRISE/ServerConnection.h"
#include "../Utilities/String.h"
#include "WorkQueue.h"

struct MODEL_INFO
{
	Point3D				ptPosition;
	Vector3D			vOrientation;
	String				material;
	String				szFilename;
};

class WorkerRenderer;

class SchedulerConnection : public ServerConnection
{
protected:
	virtual ~SchedulerConnection();

public:
	SchedulerConnection( ICommunicator* pCommunicator );

	bool GetSceneFilename( String& filename );
	bool GetSceneModels( std::deque<MODEL_INFO>& models );

	bool SendWorkerType( int workertype );

	// Returns:
	//  -1 = some kind of fatal error
	//   1 = if we are to keep processing requests, 
	//   2 = if we are done with the rendering, move on
	int ProcessSchedulerRequest( WorkerRenderer* pRenderer, WorkQueue& workqueue );

	// This sends the results to the server
	bool SendResult( const unsigned int x, const unsigned int y, const IFXColor& c );
	bool SendRenderComplete();
	bool SendUnresolvedRay( const Ray& ray, const unsigned int x, const unsigned int y, IMemoryBuffer* pTraversalBuf );
};

#include "WorkerRenderer.h"

#endif



