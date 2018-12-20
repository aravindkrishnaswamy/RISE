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

#ifdef WIN32
	#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
	#define _WIN32_WINNT 0x0400		// require NT4 or greater for TryEnterCriticalSection
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
		return v.tv_sec*1000 + v.tv_usec/1000;
	#endif
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
	startTime = GetMilliseconds();
}

void Timer::stop()
{
	endTime = GetMilliseconds();
	interval = endTime-startTime;
}

unsigned int Timer::getInterval() const
{
	return interval;
}
