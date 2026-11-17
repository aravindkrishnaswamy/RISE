//////////////////////////////////////////////////////////////////////
//
//  ModifierStack.cpp - Implementation of the modifier-composition
//  class.  See ModifierStack.h for the what and the why, and
//  docs/RELIEF_MODIFIER_DESIGN.md section 4 for the full design.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ModifierStack.h"

using namespace RISE;
using namespace RISE::Implementation;

ModifierStack::ModifierStack(
	const IRayIntersectionModifier* const* members_,
	const unsigned int count
	)
{
	members.reserve( count );
	for( unsigned int i = 0; i < count; i++ ) {
		members.push_back( members_[i] );
		members_[i]->addref();
	}
}

ModifierStack::~ModifierStack()
{
	for( std::vector<const IRayIntersectionModifier*>::const_iterator it = members.begin(); it != members.end(); ++it ) {
		(*it)->release();
	}
}

void ModifierStack::Modify( RayIntersectionGeometric& ri ) const
{
	// Authored order, each member seeing the PREVIOUS one's vNormal/onb --
	// exactly as if the object's single modifier slot held a hand-written
	// chain of `members.size()` Modify calls.  See ModifierStack.h for the
	// order-semantics table (normal_map -> relief, relief -> normal_map,
	// ... -> glint) and the nesting guarantee.
	for( std::vector<const IRayIntersectionModifier*>::const_iterator it = members.begin(); it != members.end(); ++it ) {
		(*it)->Modify( ri );
	}
}
