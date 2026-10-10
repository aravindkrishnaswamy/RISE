//////////////////////////////////////////////////////////////////////
//
//  Reference.h - Utility class which other classes extent to have
//  reference counting capabilities
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 26, 2001
//  Tabs: 4
//  Comments: Pulled ffrom original IntelliFX
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef REFERENCE_
#define REFERENCE_

#include "../Interfaces/IReference.h"
#include "Threads/Threads.h"

namespace RISE
{
	namespace Implementation
	{
		class Reference : public virtual IReference
		{
		protected:
			mutable unsigned int m_nRefcount;

			RMutex mutex;

			virtual ~Reference();
			Reference();

			//! A COPY is a new object: it starts with its own count of one and
			//! its own mutex.  (The implicit copy would have duplicated the
			//! count and, worse, the mutex HANDLE, destroying it twice.)  Used
			//! by the DL-465 per-thread clones of cameras and lights.
			Reference( const Reference& ) : IReference(), m_nRefcount( 1 ) {}

			//! Assignment between reference-counted objects has no meaning (it
			//! would copy the count and the mutex handle).  Deleted, so no
			//! Reference subclass can be assigned by accident (DL-465 review).
			Reference& operator=( const Reference& ) = delete;

		public:
			virtual void addref() const;
			virtual bool release() const;
			virtual unsigned int refcount() const;
		};
	}
}


#endif
