//////////////////////////////////////////////////////////////////////
//
//  MemoryTracker.cpp - Implements the memory tracker
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

#include "pch.h"
#include "MemoryTracker.h"
#include <fstream>

//#define TRACK_PEDANTIC

using namespace RISE;

MemoryTracker::MemoryTracker()
{
//	allocations.reserve( 1024 );

	output = new std::ofstream( "MemoryTracker.txt" );

	total_allocations = 0;
	total_deallocations = 0;
}

MemoryTracker::~MemoryTracker()
{
	// On de-allocations, we find and dump all leaks
	DumpAllLeakedMemory( );

	allocations.clear();

	if( output ) {
		delete output;
		output = 0;
	}
}

void MemoryTracker::NewAllocation( const void* ptr, const char * szFile, const int nLine, const char * szMessage )
{
#ifdef TRACK_PEDANTIC
	// First check to make sure the allocation doesn't already exist
	AllocationsRec::iterator it = allocations.find( ptr );
	if( it != allocations.end() ) {
		const ALLOC_RECORD& r = it->second;
		*output << "New Allocation:: pointer already exists, ptr:" << r.ptr << ", File: " << r.szFile << " , Line:" << r.line << std::endl;
		return;
	}
#endif

	// Add it
	ALLOC_RECORD rec;
	rec.ptr = (void*)ptr;
	strncpy( rec.szFile, szFile, 256 );
	rec.line = nLine;

	if( szMessage ) {
		strncpy( rec.message, szMessage, 64 );
	}

	allocations[ptr] = rec;
	total_allocations++;
}

void MemoryTracker::FreeAllocation( const void* ptr, const char * szFile, const int nLine )
{
	// Find this pointer in our list
	AllocationsRec::iterator it = allocations.find( ptr );

	if( it == allocations.end() ) {
		*output << "Specified Allocation does not exist!, ptr:" << ptr << ", File: " << szFile << " , Line:" << nLine << std::endl;
		return;
	}
	
	// Erase that iterator
	allocations.erase( it );
	total_deallocations++;

#ifdef TRACK_PEDANTIC
	// Just to be sure check again
	it = allocations.find( ptr );
	if( it != allocations.end() ) {
		// Print some error
		const ALLOC_RECORD& r = it->second;
		*output << "Deleted pointer persists after deletion!, ptr:" << r.ptr << ", File: " << r.szFile << " , Line:" << r.line << std::endl;
	}
#endif
}

void MemoryTracker::DumpAllLeakedMemory( )
{
	*output << "Total allocations: " << total_allocations << ", total deallocations: " << total_deallocations << std::endl;
	*output << "Dumping all potentially leaked memory: (detected " << allocations.size() << " leaks)" << std::endl;
	output->flush();
	// Dump all leaked memory to our file
	AllocationsRec::iterator it;
	for( it=allocations.begin(); it !=allocations.end(); it++ ) {
		const ALLOC_RECORD& ar = it->second;
		
		*output << "leak, ptr:0x" << ar.ptr << ", File: " << ar.szFile << " , Line:" << ar.line << " \"" << ar.message << "\"" << std::endl;
	}
	*output << "Finished leak dump" << std::endl;
}
