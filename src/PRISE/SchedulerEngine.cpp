//////////////////////////////////////////////////////////////////////
//
//  SchedulerEngine.cpp - Implementation of the scheduler engine
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SchedulerEngine.h"
#include "../Interfaces/ILog.h"
#include "../Rendering/FileRasterizerOutput.h"
#include "../Utilities/Time.h"
#include "../Utilities/MemoryBuffer.h"

// The minimum number of items that should be in a worker's queue 
static const unsigned int minWorkerQueueThresh = 1000;

// If a worker's queue is any more full than this, then try to cache some 
// of its data to other workers
static const unsigned int maxWorkerQueueSize = 20;

// How long to sleep when assigning, in ms if all the workers queues are too full
static const unsigned int timeToSleepWhenAssigning = 1;

namespace Implementation
{

SchedulerEngine::SchedulerEngine() :
  nWorkersToWaitFor( 0 ),
  nRendersSoFar( 0 ),
  nResX( 0 ),
  nResY( 0 ),
  nGranularityX( 0 ),
  nGranularityY( 0 ),
  nLastXPixel( 0 ),
  nLastScanline( 0 ),
  sceneLife( 0 ),
  pMesh( 0 ),
  nNumPixelsDone( 0 ),
  nTotalPixels( 0 )
{
	mutWorkerList = Threads::ifxCreateMutex();
	mutResults = Threads::ifxCreateMutex();
	sleep = Threads::ifxCreateSleep();
}

SchedulerEngine::~SchedulerEngine()
{
	WorkerListType::iterator i;
	for( i=workers.begin(); i!=workers.end(); i++ ) {
		i->pConnection->RemoveRef();
	}

	workers.clear();

	if( pOutputImage ) {
		pOutputImage->RemoveRef();
		pOutputImage = 0;
	}

	if( pMesh ) {
		pMesh->RemoveRef();
		pMesh = 0;
	}

	Threads::ifxDestroyMutex( mutWorkerList );
	Threads::ifxDestroyMutex( mutResults );
	Threads::ifxDestroySleep( sleep );
}

bool SchedulerEngine::AddNewWorker( WorkerConnection* pConnection )
{
	if( !pConnection ) {
		return false;
	}

	// Add this new worker to our internal list
	Threads::ifxMutexLock( mutWorkerList );
	{
		pConnection->AddRef();
		WORKER	elem;
		elem.pConnection = pConnection;
		elem.queueSize = 0;
		pConnection->SetWorkerID( workers.size() );
		workers.push_back( elem );
	}
	Threads::ifxMutexUnlock( mutWorkerList );

	return true;
}

void SchedulerEngine::IncrementRenderConnections()
{
	Threads::ifxMutexLock( mutWorkerList );
	{
		nRendersSoFar++;
	}
	Threads::ifxMutexUnlock( mutWorkerList );
}

void SchedulerEngine::WaitForWorkers( const unsigned int nWorkers )
{
	nWorkersToWaitFor = nWorkers;
}

void SchedulerEngine::SetScene( const char* szFilename, const char* szOutputfilename, unsigned int width, unsigned int height, unsigned int granx, unsigned int grany )
{
	sceneFileName = szFilename;
	outputFileName = szOutputfilename;
	nResX = width;
	nResY = height;
	nGranularityX = granx;
	nGranularityY = grany;
	nTotalPixels = nResX * nResY;

	pOutputImage = new IFXRasterImage( nResX, nResY, IFXColor( 0, 0, 0, 0 ) );
}

void SchedulerEngine::AddModelToScene( const Point3D ptPosition, const Vector3D vOrientation, const char * szMaterialname, const char* szFilename )
{
	MODEL_INFO		mi;
	mi.ptPosition = ptPosition;
	mi.vOrientation = vOrientation;
	mi.material = szMaterialname;
	mi.szFilename = szFilename;
	sceneModels.push_back( mi );
}

bool SchedulerEngine::ReadyToGo()
{
	bool bret = false;
	Threads::ifxMutexLock( mutWorkerList );
	{
		bret = ((workers.size() == nWorkersToWaitFor) && (nWorkersToWaitFor == nRendersSoFar));
	}
	Threads::ifxMutexUnlock( mutWorkerList );

	return bret;
}

void SchedulerEngine::Engage()
{
	//
	// This is where all the action happens!
	//

	// First load the mesh, but don't load the 2nd level octree, we will need this
	// information to figure out where unresolved rays go
	{
		pMesh = new PRISEMeshGeometry(0,0,0,0);
		GlobalLog()->PrintNew( pMesh, __FILE__, __LINE__, "geometry" );

		char szModelFileName[1024] = {0};
		sprintf( szModelFileName, "%s_%d.prisemesh", sceneModels.front().szFilename.c_str(), -1 );

		Implementation::MemoryBuffer*			pBuffer = new Implementation::MemoryBuffer( szModelFileName );
		GlobalLog()->PrintNew( pBuffer, __FILE__, __LINE__, "buffer" );

		pMesh->Deserialize( *pBuffer );
		pBuffer->RemoveRef();
	}

	// First send the scene file to all the workers
	WorkerListType::iterator i;
	for( i=workers.begin(); i!=workers.end(); i++ ) {
		if( !i->pConnection->SendSceneFile( sceneFileName.c_str() ) ) {
			GlobalLog()->PrintEasyError( "Failed to send scene file to worker" );
			return;
		}

		// Now send the models
		if( !i->pConnection->SendSceneModels( sceneModels ) ) {
			GlobalLog()->PrintEasyError( "Failed to send the scene models to the worker" );
			return;
		}

	}

	//
	// First manually fill the queues of each worker, each worker basically gets one cell
	//
	Rect	rc;
	for( i=workers.begin(); i!=workers.end(); i++ ) {
		for( int x=0; x<1; x++ ) {
			if( !GetNewCell( rc ) ) {
				i=workers.end();
				i--;
				break;
			}

			i->pConnection->SendCell( rc );
		}
	}

	sceneLife = GetMilliseconds();

	// Now we keep going until the job is done
	RefreshWorkerQueueStatus();

	bool bCellSent = false;
	while( GetNewCell( rc ) )
	{
		bCellSent = false;
		while( !bCellSent )
		{
			// Look for a worker who's queue is less than the mininum threshold
			for( i=workers.begin(); i!=workers.end(); i++ ) {
				if( i->queueSize < minWorkerQueueThresh ) {
					// Give it the job
					i->pConnection->SendCell( rc );
					i->queueSize += (rc.right-rc.left)*(rc.bottom-rc.top);
					bCellSent = true;
					break;
				}
			}

			if( !bCellSent ) {
				// Sleep for a while
				Threads::ifxSleep( sleep, timeToSleepWhenAssigning );
				RefreshWorkerQueueStatus();
			}
		}
	}

	while( nNumPixelsDone < nTotalPixels )  {
		// sleep
		Threads::ifxSleep( sleep, 500 );
	}

	// Wait until all pixels are done, tell all the clients that once they are done processing
	// their current queues, they are also done
	for( i=workers.begin(); i!=workers.end(); i++ ) {
		if( !i->pConnection->SendRenderComplete() ) {
			GlobalLog()->PrintEasyError( "Failed to tell worker rendering was complete" );
			return;
		}
	}

	// Now we need to wait for all the clients to finish
	for( i=workers.begin(); i!=workers.end(); i++ ) {
		if( !i->pConnection->WaitForRenderCompiete() ) {
			GlobalLog()->PrintEasyError( "Failed to wait for workers to tell us they are done" );
			return;
		}
	}


	unsigned int		timeforScene = GetMilliseconds() - sceneLife;

	unsigned int		mins = timeforScene/1000/60;
	unsigned int		secs = (timeforScene-(mins*1000*60))/1000;
	unsigned int		ms = timeforScene % 1000;

	GlobalLog()->PrintEx( eLog_Event, TYPICAL_PRIORITY, "Scene complete, took %d minutes, %d seconds and %d ms", mins, secs, ms );

	// Now output it to a file
#ifdef NO_PNG_SUPPORT
	FileRasterizerOutput* fro = new FileRasterizerOutput( outputFileName.c_str(), false, FileRasterizerOutput::TGA, true );
#else
	FileRasterizerOutput* fro = new FileRasterizerOutput( outputFileName.c_str(), false, FileRasterizerOutput::PNG, true );
#endif
	fro->OutputImage( pOutputImage, 0 );		
}

bool SchedulerEngine::GetNewCell( Rect& rc )
{
	if( nLastScanline >= nResY-1 ) {		
		return false;
	}

	rc.left = nLastXPixel;
	rc.right = rc.left+nGranularityX-1;
	rc.top = nLastScanline;
	rc.bottom = rc.top+nGranularityY-1;

	if( rc.right >= nResX ) {
		rc.right = nResX-1;
	}

	nLastXPixel = rc.right+1;

	if( rc.right == nResX-1 ) {
		// Move us to the next y block
		nLastXPixel = 0;
		nLastScanline = rc.bottom+1;

		if( nLastScanline >= nResY ) {
			nLastScanline = nResY-1;
		}
	}

	if( rc.bottom >= nResY ) {
		rc.bottom = nResY-1;
	}

	nTotalCellsSendOut++;
	
	return true;
}

void SchedulerEngine::RefreshWorkerQueueStatus()
{
	// This contacts all the workers and gets the size
	// of their queues
	WorkerListType::iterator i;
	for( i=workers.begin(); i!=workers.end(); i++ ) {
		WORKER& w = *i;
		w.queueSize = w.pConnection->GetQueueSize();
	}
}

void SchedulerEngine::WorkerResult( const unsigned int x, const unsigned int y, const IFXColor& c )
{
	Threads::ifxMutexLock( mutResults );
	{
		// No mutex required, since each pixel is done by a seperate processor
		pOutputImage->SetPel( x, nResY-y-1, c );
		nNumPixelsDone++;
	}
	Threads::ifxMutexUnlock( mutResults );
}

void SchedulerEngine::UnresolvedRay( IMemoryBuffer& buffer )
{
	// First find out which node is unresolved so that we can send it to that processor
	// This is stored in the buffer as the callstack, its the last element
	buffer.seek( MemoryBuffer::START, buffer.Size()-sizeof(unsigned int)-1 );
	unsigned int nCallStack = buffer.getUInt();

	int CPU = pMesh->CPUFromCallStack( nCallStack );

	if( CPU == -1 ) {
		GlobalLog()->PrintEasyError( "CPUFromCallStack returned -1, fatal error" );
	}

	// Otherwise, add to that worker's queue
	buffer.seek( MemoryBuffer::START, 0 );
	workers[CPU].pConnection->SendIncompleteRay( buffer );
}
	
} // implementation

extern ISchedulerEngine& MasterSchedulerEngine()
{
	static Implementation::SchedulerEngine*	engine=0;

	if( !engine ) {
		engine = new Implementation::SchedulerEngine();
	}

	return *engine;
}

