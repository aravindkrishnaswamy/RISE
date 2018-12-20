//////////////////////////////////////////////////////////////////////
//
//  ITask.h - A task is basically one "job" that the job engine
//    must perform
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ITASK_
#define ITASK_

#include "../Library/Interfaces/IReference.h"
#include "../Library/Utilities/MemoryBuffer.h"

namespace RISE
{
	class ITask : public virtual IReference
	{
	protected:
		ITask(){};
		virtual ~ITask(){};

	public:
		//
		// Task interface, these are what tasks must support
		//
		typedef unsigned int TaskActionID;

		// This gets a new action for this task that makes it one
		// step closer to completing the task.  The actual data for
		// the task action is stored in the memory buffer, in addition
		// we also get an identifier which is used when the taskaction is complete
		// return true if a new taskaction was assinged
		// return false otherwise, means, no more taskactions left
		virtual bool GetNewTaskAction( TaskActionID& task_id, IMemoryBuffer& task_data ) = 0;

		// This functions gets the task action for the given TaskActionID, we expect
		// a task to be consistent for the same TaskActionIDs
		virtual bool GetTaskAction( TaskActionID task_id, IMemoryBuffer& task_data ) = 0;

		// This tells the task that it is complete the given task action
		// return true if that means this task is complete
		// return false if waiting for more taskactions
		virtual bool FinishedTaskAction( const TaskActionID id, IMemoryBuffer& results ) = 0;
	};
}

#endif

