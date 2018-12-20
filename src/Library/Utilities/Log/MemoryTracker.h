//////////////////////////////////////////////////////////////////////
//
//  MemoryTracker.h - Defines the MemoryTracker class, which tracks
//    when memory is allocated and deallocated, it checks to
//    make sure there are no leaks and makes sure there's no 
//    funny business going on...
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 21, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MEMORY_TRACKER_
#define MEMORY_TRACKER_

#include <map>
#include <iostream>

namespace RISE
{
	class MemoryTracker
	{
	protected:

		struct ALLOC_RECORD
		{
			void * ptr;
			char szFile[256];
			int  line;
			char message[64];
		};

		typedef std::map<const void*, ALLOC_RECORD> AllocationsRec;
		AllocationsRec allocations;
		std::ostream*	output;

		unsigned int	total_allocations;
		unsigned int	total_deallocations;

	public:
		MemoryTracker();
		virtual ~MemoryTracker();

		void NewAllocation( const void* ptr, const char * szFile, const int nLine, const char * szMessage );
		void FreeAllocation( const void* ptr, const char * szFile, const int nLine );

		void DumpAllLeakedMemory( );
	};
}

#endif
