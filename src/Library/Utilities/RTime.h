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

#include <chrono>
#include <limits>

namespace RISE
{
	inline unsigned int ElapsedMilliseconds( std::chrono::steady_clock::time_point start,
		std::chrono::steady_clock::time_point end )
	{
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
		if( elapsed <= 0 ) return 0;
		const auto max = std::numeric_limits<unsigned int>::max();
		return static_cast<unsigned long long>(elapsed) > max ? max : static_cast<unsigned int>(elapsed);
	}

	// Monotonic elapsed milliseconds, with the legacy unsigned interval interface.
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

	// Legacy platform clock used for seed generation and the POSIX sleep fallback.
	extern unsigned int GetMilliseconds();
}

#endif
