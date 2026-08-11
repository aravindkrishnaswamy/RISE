//////////////////////////////////////////////////////////////////////
//
//  NullBoundaryMaterial.cpp - Exact null-boundary material identity.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "NullBoundaryMaterial.h"
#include "../Utilities/IORStack.h"
#include <typeinfo>

using namespace RISE;
using namespace RISE::Implementation;

IBSDF* NullBoundaryMaterial::GetBSDF() const
{
	return 0;
}

ISPF* NullBoundaryMaterial::GetSPF() const
{
	return 0;
}

IEmitter* NullBoundaryMaterial::GetEmitter() const
{
	return 0;
}

bool RISE::Implementation::IsExactNullBoundaryMaterial(
	const IMaterial* pMaterial
	)
{
	return pMaterial && typeid(*pMaterial) == typeid(NullBoundaryMaterial);
}

bool RISE::Implementation::ApplyExactNullBoundaryTransition(
	const IMaterial* pMaterial,
	const IObject* pObject,
	IORStack& stack
	)
{
	if( !IsExactNullBoundaryMaterial( pMaterial ) || !pObject ) {
		return false;
	}

	stack.SetCurrentObject( pObject );
	if( stack.containsCurrentEnclosure() ) {
		stack.popEnclosure();
	} else {
		stack.pushEnclosure();
	}
	return true;
}

Ray RISE::Implementation::ContinueExactNullBoundaryRay(
	const Ray& ray,
	const Scalar distance
	)
{
	Ray continued( ray );
	if( continued.hasDifferentials ) {
		continued.diffs.rxOrigin = continued.diffs.rxOrigin +
			continued.diffs.rxDir * distance;
		continued.diffs.ryOrigin = continued.diffs.ryOrigin +
			continued.diffs.ryDir * distance;
	}
	continued.Advance( distance );
	return continued;
}
