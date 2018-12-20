//////////////////////////////////////////////////////////////////////
//
//  RTime.h - A bunch of utility functions for accessing time.  Whether
//  its for determining how much time has passed or whether its 
//  determining the actual system time, use the functions here.
//  Also includes the definition of a timer class which is used
//  for timing purposes...  I may add other utilities here
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 15, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RTIME_
#define RTIME_

namespace RISE
{
	class Timer
	{
	protected:
		unsigned int startTime;
		unsigned int endTime;
		unsigned int interval;

	public:
		Timer( );
		virtual ~Timer( );

		void start();
		void stop();

		unsigned int getInterval() const;
	};

	extern unsigned int GetMilliseconds();
}

#endif
