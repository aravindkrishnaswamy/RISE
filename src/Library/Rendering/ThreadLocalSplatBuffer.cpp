//////////////////////////////////////////////////////////////////////
//
//  ThreadLocalSplatBuffer.cpp
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ThreadLocalSplatBuffer.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

void ThreadLocalSplatBuffer::FlushInto( SplatFilm& film )
{
	if( records.empty() ) {
		return;
	}

	// Sort by pixelIndex so consecutive records fall into adjacent
	// rows (and, within a row, adjacent columns).  This groups splats
	// by row so we take each row-mutex exactly once.
	std::sort( records.begin(), records.end(),
		[]( const Record& a, const Record& b ) {
			return a.pixelIndex < b.pixelIndex;
		} );

	// Walk grouped records and commit row-at-a-time.  SplatFilm's
	// public Splat() re-acquires the mutex per call which defeats
	// the point — instead we reuse its batch commit (added below).
	film.BatchCommit( records.data(), records.size() );

	records.clear();
}

ThreadLocalSplatBuffer& RISE::Implementation::GetThreadLocalSplatBuffer()
{
	thread_local ThreadLocalSplatBuffer buf;
	return buf;
}

void RISE::Implementation::FlushCallingThreadSplatBuffer()
{
	ThreadLocalSplatBuffer& buf = GetThreadLocalSplatBuffer();
	buf.FlushBound();
}
