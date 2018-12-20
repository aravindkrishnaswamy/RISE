//////////////////////////////////////////////////////////////////////
//
//  IMemoryBuffer.h - Interface to a memory buffer, which is a 
//    read and write buffer, but also has some other functions
//    unique to memory buffers like getting point information
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_MEMORY_BUFFER_H_
#define I_MEMORY_BUFFER_H_

#include "IReadBuffer.h"
#include "IWriteBuffer.h"

namespace RISE
{
	class IMemoryBuffer : public virtual IReadBuffer, public virtual IWriteBuffer
	{
	protected:
		IMemoryBuffer() {};
		virtual ~IMemoryBuffer() {};

	public:
		//! This returns the pointer at the begining of the buffer
		virtual char * Pointer( ) = 0;

		//! This returns a const pointer at the begining of the buffer
		virtual const char * Pointer( ) const = 0;

		//! This returns the pointer at the cursor location
		virtual char * PointerAtCursor( ) = 0;

		//! This returns a const pointer at the cursor location
		virtual const char * PointerAtCursor( ) const = 0;
		
		//! Tells us if the buffer owns the memory?
		/// \return TRUE if memory is owned by this class, FALSE otherwise
		virtual bool DoOwnMemory() const = 0;
	};
}

#endif

