//////////////////////////////////////////////////////////////////////
//
//  IEnumCallback.h - Defines the functor interface that all classes
//    who want enumeration services must implement
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 25, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_ENUM_CALLBACK_
#define I_ENUM_CALLBACK_

namespace RISE
{
	template< class T >
	class IEnumCallback
	{
	public:
		virtual ~IEnumCallback(){}
		virtual bool operator() ( const T& ) = 0;
	};
}

#endif

