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

		public:
			virtual void addref() const;
			virtual bool release() const;
			virtual unsigned int refcount() const;
		};
	}
}


#endif
