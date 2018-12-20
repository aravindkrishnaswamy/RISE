//////////////////////////////////////////////////////////////////////
//
//  LightManager.cpp - Implementation of the LightManager class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LightManager.h"

using namespace RISE;
using namespace RISE::Implementation;

LightManager::LightManager( )
{
}

LightManager::~LightManager( )
{
}

void LightManager::ComputeDirectLighting( 
	const RayIntersectionGeometric& ri,
	const IRayCaster& pCaster,
	const IBSDF& brdf, 
	const bool bReceivesShadows,
    RISEPel& amount
	) const
{
	amount = RISEPel(0.0);

	GenericManager<ILightPriv>::ItemListType::const_iterator		i, e;
	for( i=items.begin(), e=items.end(); i!=e; i++ ) {
		// Accrue the light values
		RISEPel p;
		i->second.first->ComputeDirectLighting( ri, pCaster, brdf, bReceivesShadows, p );
		amount = amount + p;
	}
}
