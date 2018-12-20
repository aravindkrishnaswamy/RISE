//////////////////////////////////////////////////////////////////////
//
//  SchedulerEngine.h - Declaration of the scheduler engine
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 7, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SCHEDULER_ENGINE_
#define SCHEDULER_ENGINE_

#include "ISchedulerEngine.h"
#include "PRISEMeshGeometry.h"
#include "../Utilities/Reference.h"
#include "../Utilities/String.h"
#include "../Interfaces/IRasterImage.h"		// for Rect
#include "../RasterImages/RasterImage.h"
#include <list>

namespace Implementation
{
	class SchedulerEngine : public virtual ISchedulerEngine, public virtual Reference
	{
	protected:
		struct WORKER
		{
			WorkerConnection*	pConnection;	// Connection object for this worker
			unsigned int		queueSize;		// Number of cells in this objects queue (or best guess)
		};

		typedef std::deque<WORKER>		WorkerListType;
		typedef std::deque<MODEL_INFO>	ModelsNameListType;

		WorkerListType		workers;
		IFXMUTEX			mutWorkerList;		// Mutex for accessing the list of workers
		IFXMUTEX			mutResults;			// Mutex for accessing the results
		IFXSLEEP			sleep;				// To help us sleep

		unsigned int		nWorkersToWaitFor;	// How many workers are to wait for before starting work
		unsigned int		nRendersSoFar;		// How many workers have connectioned back with their rendering thread
		String				sceneFileName;		// Name of the scene we are working on
		ModelsNameListType	sceneModels;		// The names of the files that contain meshes for this scene
		String				outputFileName;		// Name of the output file
		unsigned int		nResX;				// X Resolution of scene
		unsigned int		nResY;				// Y Resolution of scene
		unsigned int		nGranularityX;		// Granularity in X
		unsigned int		nGranularityY;		// Granularity in Y

		unsigned int		nLastXPixel;		// Represent the cell we handed out 
		unsigned int		nLastScanline;		//

		unsigned int		nTotalCellsSendOut;	// The number of cells sent out

		IFXRasterImage*		pOutputImage;		// The results buffer

		unsigned int		sceneLife;			// When did we start working on the current scene

		PRISEMeshGeometry*	pMesh;				// The mesh everyone is working on

		unsigned int		nNumPixelsDone;		// Number of completed pixels
		unsigned int		nTotalPixels;		// Total number of pixels to be processed

		virtual ~SchedulerEngine();

		bool GetNewCell( Rect& rc );
		void RefreshWorkerQueueStatus();

	public:
		SchedulerEngine();

		virtual bool AddNewWorker( WorkerConnection* pConnection );
		virtual void IncrementRenderConnections( );
		virtual void WaitForWorkers( const unsigned int nWorkers );
		virtual void SetScene( const char* szFilename, const char* szOutputfilename, unsigned int width, unsigned int height, unsigned int granx, unsigned int grany );
		virtual void AddModelToScene( const Point3D ptPosition, const Vector3D vOrientation, const char * szMaterialname, const char* szFilename );
		virtual bool ReadyToGo();
		virtual void Engage();

		virtual void WorkerResult( const unsigned int x, const unsigned int y, const IFXColor& c );
		virtual void UnresolvedRay( IMemoryBuffer& buffer );
	};
}

#endif

