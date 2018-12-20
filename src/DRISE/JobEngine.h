//////////////////////////////////////////////////////////////////////
//
//  JobEngine.h - Job engine implementation class, this particular
//    job engine will use a really simple scheduler
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef JOB_ENGINE
#define JOB_ENGINE

#include "IJobEngine.h"
#include "../Library/Utilities/Reference.h"
#include "../Library/Utilities/Threads/Threads.h"
#include <map>
#include <deque>

namespace RISE
{
	namespace Implementation
	{
		class JobEngine : public virtual IJobEngine, public virtual Implementation::Reference
		{
		public:
			struct ACTION_T
			{
				ITask::TaskActionID		actionID;
				unsigned int			timeAssigned;

				inline bool operator ==( const ACTION_T& a )
				{
					return (actionID==a.actionID);
				}

			};
			typedef std::deque<ACTION_T>	TaskActionListType;

		protected:
			RMutex		engineMutex;

			// This stores the list of jobs
			struct TASK_T
			{
				ITask*					pTask;
				TaskActionListType		activeActions;
			};

			typedef std::map<TaskID,TASK_T>	TaskMapType;
			TaskMapType		tasks;

			TaskID nTaskIDCounter;

			virtual ~JobEngine();

		public:
			JobEngine();

			void AddTask( ITask* pTask );
			bool GetNewTaskAction( TaskID& taskid, ITask::TaskActionID& taskeventid, IMemoryBuffer& buffer );
			void FinishedTaskAction( TaskID taskid, ITask::TaskActionID taskeventid, IMemoryBuffer& buffer );
		};
	}
}

#endif
