//////////////////////////////////////////////////////////////////////
//
//  ISchedulerEngine.h - Interface to the scheduler engine, specifies
//    primary interface functions common to all schedulers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_SCHEDULER_ENGINE_
#define I_SCHEDULER_ENGINE_

#include "../Interfaces/IReference.h"
#include "WorkerConnection.h"

class ISchedulerEngine : public virtual IReference
{
protected:
	ISchedulerEngine(){};
	virtual ~ISchedulerEngine(){};

public:
	// Adds a new worker to the master engine
	virtual bool AddNewWorker( WorkerConnection* pConnection ) = 0;

	// Tells the scheduler engine that another renderer connection has been made
	virtual void IncrementRenderConnections( ) = 0;

	// Tells the master scheduler to not do any work until this many workers 
	// have been added
	virtual void WaitForWorkers( const unsigned int nWorkers ) = 0;

	// This tells the master scheduler which scene file we are going to be working on
	virtual void SetScene( const char* szFilename, const char* szOutputfilename, unsigned int width, unsigned int height, unsigned int granx, unsigned int grany ) = 0;

	// This tells the master scheduler to add the "parallel model" to the scene
	virtual void AddModelToScene( const Point3D ptPosition, const Vector3D vOrientation, const char * szMaterialname, const char* szFilename ) = 0;

	// Asks the scheduler if its ready to go yet
	virtual bool ReadyToGo() = 0;

	// Tells the scheduler to do its thing!
	virtual void Engage() = 0;

	// This is the result from a worker, in the form of a color and the pixel
	virtual void WorkerResult( const unsigned int x, const unsigned int y, const IFXColor& c ) = 0;

	// This is an unresolved ray from a worker sitting in a memory buffer
	virtual void UnresolvedRay( IMemoryBuffer& buffer ) = 0;
};

extern ISchedulerEngine& MasterSchedulerEngine();

#endif

