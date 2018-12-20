//////////////////////////////////////////////////////////////////////
//
//  JobEngine.cpp - Implementation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "JobEngine.h"
#include <algorithm>
#include "../Library/Interfaces/ILog.h"
#include "../Library/Utilities/RTime.h"

namespace RISE
{
	namespace Implementation
	{
		JobEngine::JobEngine() : 
			nTaskIDCounter( 1 )
		{
		}

		JobEngine::~JobEngine()
		{
		}

		void JobEngine::AddTask( ITask* pTask )
		{
			engineMutex.lock();
			{
				// Add the task to our local list
				
				// Generate a new ID
				TaskID	newid = nTaskIDCounter;
				nTaskIDCounter++;

				pTask->addref();
				tasks[newid].pTask = pTask;
			}
			engineMutex.unlock();
		}

		static bool TimeCompare( const JobEngine::ACTION_T& lhs, const JobEngine::ACTION_T& rhs )
		{
			return lhs.timeAssigned > rhs.timeAssigned;
		}

		bool JobEngine::GetNewTaskAction( TaskID& taskid, ITask::TaskActionID& taskeventid, IMemoryBuffer& buffer )
		{
			bool bSuccess = false;

			engineMutex.lock();
			{
				// Just hit the first one for now

				// Hit all the tasks in order
				TaskMapType::iterator it = tasks.begin();
				for( ; it!=tasks.end(); it++ ) {
					if( it->second.pTask->GetNewTaskAction( taskeventid, buffer ) ) {
						taskid = it->first;
						ACTION_T	item;
						item.actionID = taskeventid;
						item.timeAssigned = GetMilliseconds();
						it->second.activeActions.push_back( item );
						bSuccess = true;
						break;
					}
				}

				if( !bSuccess ) {
					// Re assign something we are still waiting for
					// To find a reassignment, start at the top again
					it = tasks.begin();
					for( ; it!=tasks.end(); it++ ) {
						// Find the oldest task...
						if( it->second.activeActions.size() > 0 ) {
							std::sort( it->second.activeActions.begin(), it->second.activeActions.begin(), &TimeCompare );

							if( it->second.pTask->GetTaskAction( it->second.activeActions.front().actionID, buffer ) ) {
								taskid = it->first;
								taskeventid = it->second.activeActions.front().actionID;
								it->second.activeActions.front().timeAssigned = GetMilliseconds();							taskid = it->first;
								bSuccess = true;
								break;
							}
						}
					}
				}
			}
			engineMutex.unlock();

			return bSuccess;
		}

		void JobEngine::FinishedTaskAction( TaskID taskid, ITask::TaskActionID taskeventid, IMemoryBuffer& buffer )
		{
			engineMutex.lock();
			{
				// Tell the the task that its particular action is finished
				TaskMapType::iterator it = tasks.find(taskid);

				if( it != tasks.end() )
				{
					// Remove it from the action list
					ACTION_T finder;
					finder.actionID = taskeventid;

					TaskActionListType::iterator	elem = std::find( it->second.activeActions.begin(), it->second.activeActions.end(), finder );
					
					if( elem != it->second.activeActions.end() )
					{
						it->second.activeActions.erase( elem );

						if( it->second.pTask->FinishedTaskAction( taskeventid, buffer ) ) {

							if( it->second.activeActions.size() != 0 ) {
								GlobalLog()->PrintEasyError( "Error! the task says its done, but there are still actions in the engine list!" );
							}

							safe_release( it->second.pTask );
							tasks.erase( it );
						}
					}
				}
			}
			engineMutex.unlock();
		}
	}

	extern IJobEngine& MasterJobEngine()
	{
		static Implementation::JobEngine*	engine=0;

		if( !engine ) {
			engine = new Implementation::JobEngine();
		}

		return *engine;
	}
}

