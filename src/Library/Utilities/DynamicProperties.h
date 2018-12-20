//////////////////////////////////////////////////////////////////////
//
//  DynamicProperties.h - Gives a class the ability to specify and 
//  query properties at run time
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 30, 2001
//  Tabs: 4
//  Comments: DISABLED by AK Nov. 26, the entire thing needs to be
//    thought out better
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DYNAMICPROPERTIES_
#define DYNAMICPROPERTIES_

#include <vector>
#include <map>

#ifndef HACK_
#error "Don't include DynamicProperties.h, its borken"
#endif

namespace RISE
{
	typedef void (* DynPropFreeFunc)(const char * strkey, void *data);

	class DynamicProperties
	{
	protected:
		virtual ~DynamicProperties();

		/*
		typedef std::vector<char>						String;
		typedef std::pair<NOTIFICATIONCALLBACK,void*>	CallbackType;

		mutable std::map<String,CallbackType> propslist;
		*/

	public:
		DynamicProperties();

		virtual bool SetProperty( const char* szKey, void* data, DynPropFreeFunc func ) const;
		virtual bool RemoveProperty( const char* szKey ) const;
		virtual void* GetProperty( const char* szKey ) const;
	};
}

#undef MAX_PROP_NAME

#endif
