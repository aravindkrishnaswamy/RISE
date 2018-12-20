//////////////////////////////////////////////////////////////////////
//
//  DynamicProperties.cpp - Implementation of the DynamicProperties
//  class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 30, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#define HACK_
#include "DynamicProperties.h"
#undef HACK_

using namespace RISE;

DynamicProperties::DynamicProperties( )
{
}

DynamicProperties::~DynamicProperties( )
{
}

bool DynamicProperties::SetProperty( const char* szKey, void* data, DynPropFreeFunc func ) const
{
	/*
	DP_Pair temp;
	strcpy( temp.szName, szKey );
	temp.data = data;
	temp.func = func;
	propslist.push_back( temp );
	*/
	return true;
}

bool DynamicProperties::RemoveProperty( const char* szKey ) const
{
	/*
	std::vector<DP_Pair>::iterator i=propslist.begin();
	std::vector<DP_Pair>::iterator e=propslist.end();
	for( ; i!=e; i++  )
	{
		DP_Pair& temp = *i;
		if( strcmp( szKey, temp.szName ) == 0 )
		{
			if( temp.func ) {
				(*temp.func)(temp.szName, temp.data);
			}
			propslist.erase( i );
			return true;
		}
	}
	*/
	return false;
}

void* DynamicProperties::GetProperty( const char* szKey ) const
{
	/*
	std::vector<DP_Pair>::const_iterator i=propslist.begin();
	std::vector<DP_Pair>::const_iterator e=propslist.end();
	for( ; i!=e; i++  )
	{
		const DP_Pair& temp = *i;
		if( strcmp( szKey, temp.szName ) == 0 ) {
			return temp.data;
		}
	}
	*/
	return NULL;
}
