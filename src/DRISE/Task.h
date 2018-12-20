//////////////////////////////////////////////////////////////////////
//
//  Task.h - This is the basic simple task we will use for the time
//    being.  Once we have some cooler IO routines, we will use
//    spiffier tasks.  This simple one simply makes each task action
//    one scanline.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TASK_
#define TASK_

#include "ITask.h"

#include "../Library/Interfaces/IRasterizerOutput.h"
#include "../Library/Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Task : public virtual ITask, public virtual Reference
		{
		protected:
			virtual ~Task();

			char			szSceneFileName[1024];		// The scene file we are to rasterizer
			unsigned int	nResX;						// X resolution to rasterizer at
			unsigned int	nResY;						// Y resolution to rasterizer at
			char			szOutputFileName[1024];		// The output file we are to write to when complete

			unsigned int	nXGranularity;				// How much should we break up the rasterizing in x ?
			unsigned int	nYGranularity;				// How much should we break up the rasterizing in y ?

			unsigned int	nTotalActionsSendOut;		// Total number of work units we have put out so far
			bool			bFinishedSendingOut;		// Are we done sending out work units ?

			unsigned int	nLastXPixel;				// Represent the last work unit we handed out 
			unsigned int	nLastScanline;				//

			unsigned int	nNumActionsComplete;		// Number of work units we've handed out so far

			IRasterImage*	pOutputImage;				// The results buffer

			unsigned int	taskLife;					// When did someone start working on this task?

			IRasterizerOutput*	pRasterizerOutput;		// For fancy updates on the server side everything stuff
														// is updated

		public:
			Task( const char * scene, unsigned int x, unsigned int y, const char * output, unsigned int granx, unsigned int grany );

			//
			// Interface implementations
			//
			bool GetNewTaskAction( TaskActionID& task_id, IMemoryBuffer& task_data );
			bool GetTaskAction( TaskActionID task_id, IMemoryBuffer& task_data );
			bool FinishedTaskAction( const TaskActionID id, IMemoryBuffer& results );
		};
	}
}

#endif

