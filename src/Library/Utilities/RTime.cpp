//////////////////////////////////////////////////////////////////////
//
//  Time.cpp - Implementation of the Timer class and time 
//  functions   
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 15, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RTime.h"
#include <chrono>

#ifdef WIN32
	#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
	#include <windows.h>
#else
	#include <sys/time.h>
#endif

using namespace RISE;

namespace RISE
{
	unsigned int GetMilliseconds()
	{
	#ifdef WIN32
		return GetTickCount();
	#else
		timeval		v;
		gettimeofday( &v, 0 );
		return static_cast<unsigned int>(v.tv_sec*1000 + v.tv_usec/1000);
	#endif
	}
}

namespace
{
	unsigned int MonotonicMilliseconds()
	{
		using namespace std::chrono;
		return static_cast<unsigned int>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() );
	}
}

Timer::Timer() : 
  startTime( 0 ),
  endTime( 0 ),
  interval( 0 )
{
}

Timer::~Timer( )
{
}

void Timer::start()
{
	interval = 0;
	startTime = MonotonicMilliseconds();
}

void Timer::stop()
{
	endTime = MonotonicMilliseconds();
	interval = endTime-startTime;
}

unsigned int Timer::getInterval() const
{
	return interval;
}
