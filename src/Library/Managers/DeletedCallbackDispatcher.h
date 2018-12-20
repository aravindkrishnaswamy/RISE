//////////////////////////////////////////////////////////////////////
//
//  DeletedCallbackDispatcher.h - Declaration of the templated
//   Deleted item callback dispatcher, which dispatches deletion
//   information from managers to back classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 25, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DELETED_CALLBACK_DISPATCHER_
#define DELETED_CALLBACK_DISPATCHER_

#include "../Interfaces/IDeletedCallback.h"
#include "../Utilities/RTime.h"

namespace RISE
{
	namespace Implementation
	{
		template< class Item >
		class DeletedCallbackDispatcher : public virtual IDeletedCallback<Item>
		{
		public:
			virtual bool ItemToast( const Item& item ) = 0;

			bool operator()( const Item& item )
			{
				return ItemToast( item );
			}

			bool operator==( const IDeletedCallback<Item>& other )
			{
				return (this==&other);
			}
		};
	}
}

#endif

