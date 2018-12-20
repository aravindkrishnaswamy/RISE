//////////////////////////////////////////////////////////////////////
//
//  IDeletedCallback.h - Defines the functor interface that all classes
//    who want to be notified by deleteion from a manager must
//    implement
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 25, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_DELETED_CALLBACK_
#define I_DELETED_CALLBACK_

namespace RISE
{
	template< class T >
	class IDeletedCallback
	{
	public:
		virtual ~IDeletedCallback(){}
		virtual bool operator() ( const T& ) = 0;
		virtual bool operator== ( const IDeletedCallback<T>& other ) = 0;
	};
}

#endif

