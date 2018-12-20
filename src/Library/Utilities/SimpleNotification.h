//////////////////////////////////////////////////////////////////////
//
//  SimpleNotification.h - Provides the class that extends this
//  really simple notification features.  Note that this just a 
//  temporary measure until a real more powerful listener
//  system can be implemented.  
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 22, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SIMPLE_NOTIFICATION_
#define SIMPLE_NOTIFICATION_

#include <map>
#include "../Interfaces/ILog.h"

namespace RISE
{
	class INotificationCallback
	{
	public:
		virtual bool operator()() = 0;
	};

	typedef unsigned int NOTIFYTOKEN;
	static const NOTIFYTOKEN BADTOKEN = 0;

	class SimpleNotification
	{
	protected:
		typedef std::map<NOTIFYTOKEN,INotificationCallback*>			NotifyType;

		NotifyType						notifylist;
		unsigned int					nTokenCounter;

		SimpleNotification() : nTokenCounter(0) {};
		virtual ~SimpleNotification(){ }

		bool NotifyAll( ) const
		{
			NotifyType::const_iterator	i, e;

			for( i=notifylist.begin(), e=notifylist.end(); i != e; i++ ) {
				(*i->second)();
			}

			return true;
		}

	public:
		NOTIFYTOKEN AddToNotificationList( INotificationCallback* pFunc )
		{
			if( pFunc ) {
				nTokenCounter++;
				notifylist[nTokenCounter] = pFunc;
				return nTokenCounter;
			} else {
				GlobalLog()->PrintEasyError( "AddToNotificationList:: Gave bad callback func" );
				return BADTOKEN;
			}
		}

		bool RemoveFromNotificationList( NOTIFYTOKEN& tok )
		{
			if( tok == BADTOKEN ) {
				GlobalLog()->PrintEasyError( "RemoveFromNotificationList:: Gave bad token" );
				return false;
			}

			std::pair<NotifyType::iterator,NotifyType::iterator> ranges = notifylist.equal_range( tok );

			if( ranges.first == ranges.second ) {
				GlobalLog()->PrintEx( eLog_Error, "Given token %u, not found", tok );
				return false;
			}

			notifylist.erase( ranges.first, ranges.second );

			tok = BADTOKEN;
			return true;
		}
	};
}

#endif
