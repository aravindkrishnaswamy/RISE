//////////////////////////////////////////////////////////////////////
//
//  IJobEngine.h - The job engine is the core of the master server's
//    operations.  It keeps track of all the jobs and allows 
//    other threads to update these jobs
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IJOB_ENGINE_
#define IJOB_ENGINE_

#include "../Library/Interfaces/IReference.h"
#include "ITask.h"

namespace RISE
{
	class IJobEngine : public virtual IReference
	{
	protected:
		IJobEngine(){};
		virtual ~IJobEngine(){};

	public:
		typedef unsigned int TaskID;
		//
		// Interface to a job engine
		//

		// Adds a new task to the engine
		// !@@ Add task priority
		virtual void AddTask( ITask* pTask ) = 0;

		// Asks for a piece of work from the engine
		virtual bool GetNewTaskAction( TaskID& taskid, ITask::TaskActionID& taskeventid, IMemoryBuffer& buffer ) = 0;

		// Says it has completed the following piece of work
		virtual void FinishedTaskAction( TaskID taskid, ITask::TaskActionID taskeventid, IMemoryBuffer& buffer ) = 0;
	};

	extern IJobEngine& MasterJobEngine();
}

#endif
