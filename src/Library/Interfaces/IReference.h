//////////////////////////////////////////////////////////////////////
//
//  IReference.h - Utility class which other classes extent to have
//    reference counting capabilities
//
//  Interface class                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 28, 2001
//  Tabs: 4
//  Comments: Pulled ffrom original IntelliFX
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IREFERENCE_
#define IREFERENCE_

#include "ILog.h"

namespace RISE
{
	class IReference
	{
	protected:
		IReference(){};
		virtual ~IReference(){};

	public:
		//! Adds a reference
		virtual void addref() const = 0;

		//! Removes a reference
		/// \return TRUE if the object was deleted as a result of the remove ref, FALSE otherwise
		virtual bool release() const = 0;

		/// \return The number of references held on this object
		virtual unsigned int refcount() const = 0;
	};

	//! Deletes the pointer safely
	template<typename T> inline void safe_delete(T*&t){
		if(t) {				
			GlobalLog()->PrintDelete( t, __FILE__, __LINE__ );
			delete t;
		}
		t=0;
	}

	//! Releases the reference to the reference counted object
	template<typename T> inline void safe_release(T*&t){
		if(t) {
			t->release();
		}
		t=0;
	}

	//! Calls shutdown, then releases the reference counted object
	template<typename T> inline void safe_shutdown_and_release(T*&t){
		if(t) {
			t->Shutdown();
			t->release();
		}
		t=0;
	}
}

#endif
