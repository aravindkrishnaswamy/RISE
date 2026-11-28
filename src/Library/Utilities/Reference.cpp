//////////////////////////////////////////////////////////////////////
//
//  Reference.cpp - Thread-safe intrusive reference counting base
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 21, 2002
//  Tabs: 4
//  Comments: Starts at refcount 1; addref/release/refcount synchronize on
//    `mutex`. addref() ignores already-zero objects to prevent resurrection.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <mutex>
#include "Reference.h"
#include "../Interfaces/ILog.h"

using namespace RISE::Implementation;


Reference::Reference() : 
  m_nRefcount( 1 )
{
}

Reference::~Reference()
{
}

void Reference::addref() const
{
	const std::lock_guard<const RMutex> lock( mutex );
	if( m_nRefcount > 0 ) {
		m_nRefcount++;
	}
}

// Decrements refcount under mutex, releases the lock BEFORE logging and
// `delete this` (avoiding destroying a locked mutex), and returns true
// while references remain (false once destroyed or already zero).
bool Reference::release() const
{
	bool destroy;
	{
		const std::lock_guard<const RMutex> lock( mutex );
		if( m_nRefcount == 0 ) return false;
		destroy = --m_nRefcount == 0;
	}

	if( destroy ) {
		GlobalLog()->PrintDelete( static_cast<const IReference*>(this), __FILE__, __LINE__ );
		delete this;
	}
	return !destroy;
}

unsigned int Reference::refcount() const
{
	const std::lock_guard<const RMutex> lock( mutex );
	return m_nRefcount;
}
