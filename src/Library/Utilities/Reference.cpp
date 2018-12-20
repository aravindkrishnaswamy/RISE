//////////////////////////////////////////////////////////////////////
//
//  Reference.cpp - Implements the removeref function
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
	mutex.lock();
	if( m_nRefcount > 0 ) {
		m_nRefcount++;
	}
	mutex.unlock();
}

bool Reference::release() const
{
	bool bValid = false;

	mutex.lock();
	
	bValid = m_nRefcount > 0;

	if( bValid )
	{
		bool bDestroy = false;
		
		if( m_nRefcount > 0 ) {
			m_nRefcount--;
			bDestroy = !(m_nRefcount > 0);
		}

		mutex.unlock();

		if( bDestroy ) {
			GlobalLog()->PrintDelete( (IReference*)this, __FILE__, __LINE__ );
			delete this;
		}
		return !bDestroy;
	}
	return false; 
}

unsigned int Reference::refcount() const
{
	return m_nRefcount > 0 ? m_nRefcount : 0;
}
