//////////////////////////////////////////////////////////////////////
//
//  WorkerConnection.h - This object is supposed to represent
//    a worker for the lifetime that it is connected to the 
//    scheduler
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 4, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WORKER_CONNECTION_
#define WORKER_CONNECTION_

#include "../DRISE/ClientConnection.h"
#include "../Interfaces/IRasterImage.h"
#include "../Utilities/String.h"
#include <deque>

struct MODEL_INFO
{
	Point3D				ptPosition;
	Vector3D			vOrientation;
	String				material;
	String				szFilename;
};

class WorkerConnection : public ClientConnection
{
protected:
	virtual ~WorkerConnection();

	unsigned int	nWorkerID;
	IFXMUTEX		mutComm;

public:
	WorkerConnection( ICommunicator* pCommunicator_ );

	bool SendSceneFile( const char * szFilename );
	bool SendSceneModels( const std::deque<MODEL_INFO>&	szModels );
	bool SendCell( const Rect& rc );
	bool SendIncompleteRay( IMemoryBuffer& buffer );
	bool SendRenderComplete( );

	bool WaitForRenderCompiete( );

	bool GetWorkerType( int& type );

	bool ProcessWorkerRequest();

	unsigned int GetQueueSize( );

	inline void SetWorkerID( const unsigned int id ){ nWorkerID = id; };
	inline unsigned int WorkerID(){ return nWorkerID; };
};

#endif



