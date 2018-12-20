//////////////////////////////////////////////////////////////////////
//
//  DAGObjectManager.cpp - Implementation of the DAGObjectManager class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 6, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DAGObjectManager.h"
#include <string>

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// DAGObjectManager::DAGNode
//////////////////////////////////////////////////////////////////////
DAGObjectManager::DAGNode::DAGNode()
{
}

DAGObjectManager::DAGNode::~DAGNode()
{
	Shutdown();
}

void DAGObjectManager::DAGNode::Shutdown()
{
	ChildrenListType::iterator i, e;
	for( i=children.begin(), e=children.end(); i!=e; i++ ) {
		delete i->second;
	}

	children.clear();
	objects.clear();
}

bool DAGObjectManager::DAGNode::AddItem( IObjectPriv* pObject, const std::vector<String>& names, const std::vector<String>::const_iterator& location )
{
	// To determine if we add the particular item to this node check if there is a next node, if there isn't, then add it here
	if( (location+1) == names.end() ) {
		// Add it here

		// But make sure it isn't already here
		if( objects.find( *location ) == objects.end() ) {
			objects[*location] = pObject;
			return true;
		} else {
			GlobalLog()->PrintEx( eLog_Error, "DAGNode::AddItem:: Trying to add object that already exists here '%s'", (*location).c_str() );
			return false;
		}
	}

	// Otherwise, find a child with the name at the current location and tell it to add
	ChildrenListType::const_iterator	elem = children.find( *location );

	if( elem == children.end() ) {
		GlobalLog()->PrintEx( eLog_Warning, "DAGNode::AddItem:: Could not find proper node to add child '%s'", (*location).c_str() );
	} else {
		// pass it down
		return elem->second->AddItem( pObject, names, location+1 );
	}

	return false;
}

bool DAGObjectManager::DAGNode::RemoveItem( const std::vector<String>& names, const std::vector<String>::const_iterator& location )
{
	GlobalLog()->PrintEasyError( "Calling unimplemented function: DAGNode::RemoveItem" );
	return false;
}

//////////////////////////////////////////////////////////////////////
// DAGObjectManager
//////////////////////////////////////////////////////////////////////

DAGObjectManager::DAGObjectManager( )
{
}

DAGObjectManager::~DAGObjectManager( )
{
}

void DAGObjectManager::SeperateNames( std::vector<String>& names, const char * szName )
{
	std::string		st( szName );

	unsigned int	x = 0;
	std::string			mine = st;

	for(;;)
	{
		x = mine.find_first_not_of( "\\" );

		if( x == std::string::npos ) {
			break;
		}

		mine = mine.substr( x, mine.size() );
		x = mine.find_first_of( "\\" );

		if( x == std::string::npos ) {
			x = mine.size();
		}

		String s( mine.substr( 0, x ).c_str() );
		names.push_back( s );

		if( x == mine.size() ) {
			break;
		}

		mine = mine.substr( x+1, mine.size() );

		if( mine.size() == 0 ) {
			break;
		}
	}
}

bool DAGObjectManager::AddItem( IObjectPriv* pObject, const char * szName )
{
	if( !pObject || !szName ) {
		return false;
	}

	// Check for a begining slash, if there is one, then we need to parse the "path", otherwise
	// its in plain object manager mode
	if( szName[0] == '\\' ) {
		// DAG mode
		std::vector<String>	names;
		SeperateNames( names, szName );
		if( names.size() >= 1 ) {
			if( !root.AddItem( pObject, names, names.begin() ) ) {
				return false;
			}
		} else {
			return false;
		}
	}

	return GenericManager<IObjectPriv>::AddItem( pObject, szName );
}


bool DAGObjectManager::RemoveItem( const char * szName )
{
	if( !szName ) {
		return false;
	}

	// Check for a begining slash, if there is one, then we need to parse the "path", otherwise
	// its in plain object manager mode
	if( szName[0] == '\\' ) {
		// DAG mode
		std::vector<String>	names;
		SeperateNames( names, szName );
		if( names.size() >= 1 ) {
			if( root.RemoveItem( names, names.begin() ) ) {
				return GenericManager<IObjectPriv>::RemoveItem( szName );
			}
		}
		return false;
	}
	
	return GenericManager<IObjectPriv>::RemoveItem( szName );
}

void DAGObjectManager::Shutdown()
{
	root.Shutdown();
	GenericManager<IObjectPriv>::Shutdown();
}

void DAGObjectManager::IntersectRay( RayIntersection& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	// Tricky
}

bool DAGObjectManager::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// Tricky
	return false;
}

void DAGObjectManager::EnumerateObjects( IEnumCallback<IObject>& pFunc ) const
{
	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		if( i->second.first->IsWorldVisible() ) {
			pFunc( *i->second.first );
		}
	}
}

void DAGObjectManager::EnumerateObjects( IEnumCallback<IObjectPriv>& pFunc ) const
{
	GenericManager<IObjectPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		if( i->second.first->IsWorldVisible() ) {
			pFunc( *i->second.first );
		}
	}
}

