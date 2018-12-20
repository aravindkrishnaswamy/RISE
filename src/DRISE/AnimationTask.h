//////////////////////////////////////////////////////////////////////
//
//  AnimationTask.h - This is an animation task.  Currently we just
//    make each task action one frame in the animation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ANIMATION_TASK_
#define ANIMATION_TASK_

#include "ITask.h"

#include "../Library/Interfaces/IRasterizerOutput.h"
#include "../Library/Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AnimationTask : public virtual ITask, public virtual Reference
		{
		protected:
			virtual ~AnimationTask();

			char			szSceneFileName[1024];		// The scene file we are to rasterizer
			unsigned int	nResX;						// X resolution to rasterizer at
			unsigned int	nResY;						// Y resolution to rasterizer at
			char			szOutputFileName[1024];		// The output file we are to write to when complete
			unsigned int	nFrames;					// Number of frames in the animation

			unsigned int	nTotalActionsSendOut;		// Total number of work units we have put out so far
			bool			bFinishedSendingOut;		// Are we done sending out work units ?

			unsigned int	nLastFrame;					// Represent the last work unit we handed out 

			unsigned int	nNumActionsComplete;		// Number of work units we've handed out so far

			IRasterImage*	pOutputImage;				// The results buffer

			unsigned int	taskLife;					// When did someone start working on this task?

			IRasterizerOutput*	pRasterizerOutput;		// For fancy updates on the server side everything stuff
														// is updated

		public:
			AnimationTask( const char * scene, const unsigned int x, const unsigned int y, const char * output, const unsigned int frames );

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

