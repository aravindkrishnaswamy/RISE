//////////////////////////////////////////////////////////////////////
//
//  IManager.h - Interface for managers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 30, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IMANAGER_
#define IMANAGER_

#include "IReference.h"
#include "IEnumCallback.h"
#include "IDeletedCallback.h"

namespace RISE
{
	template< class Type >
	class IManager : public virtual IReference
	{
	protected:
		IManager(){};
		virtual ~IManager(){};

	public:
		//
		// Part of the standard interface
		//

		//! Removes the item from this manager
		virtual bool RemoveItem( 
			const char * szName						///< [in] Name of item to remove
			) = 0;	

		//! Orders the manager to free everything, shutdown
		virtual void Shutdown() = 0;

		/// \return Number of items being managed
		virtual unsigned int getItemCount() const = 0;

		//! Enumerates the items using the callback function
		virtual void EnumerateItemNames(
			IEnumCallback<const char*>& pFunc		///< [in] Functor to call with each item being managed
			) const = 0;

			//! Adds an item to this manager
		virtual bool AddItem( 
			Type* pItem,							///< [in] The item to manage
			const char * szName						///< [in] Name this element will be known by
			) = 0;

		//! Retreives an item
		virtual Type* GetItem( 
			const char * szName						///< [in] Name of requested element
			) const = 0;

		//! Requests long term use of an item
		virtual Type* RequestItemUse(
			const char * szName,					///< [in] Name of requested element
			IDeletedCallback<Type>& pFunc,			///< [in] Callback functor to call when the element is deleted
			IReference& pCaller						///< [in] Reference to the caller so you don't disappear on us
			) = 0;						

		//! No longer using a particular item
		virtual bool NoLongerUsingItem( 
			Type& pItem,							///< [in] Element you are no longer using
			IDeletedCallback<Type>& pFunc			///< [in] Callback functor, used to figure out who you are
			) = 0;
	};
}

#endif
