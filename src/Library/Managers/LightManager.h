//////////////////////////////////////////////////////////////////////
//
//  LightManager.h - Definition of the LightManager class.  This is
//  what managers those hacky lights people in computer graphics
//  all the time.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LIGHT_MANAGER_
#define LIGHT_MANAGER_

#include "../Interfaces/ILightManager.h"
#include "GenericManager.h"

namespace RISE
{
	namespace Implementation
	{
		class LightManager : public virtual ILightManager, public virtual GenericManager<ILightPriv>
		{
		protected:
			virtual ~LightManager();

			LightsList cachedLights;

			void RebuildCachedLightsList()
			{
				cachedLights.clear();
				GenericManager<ILightPriv>::ItemListType::const_iterator		i, e;
				for( i=items.begin(), e=items.end(); i!=e; i++ ) {
					cachedLights.push_back( i->second.first );
				}
			}

		public:
			LightManager();

			// Override AddItem/RemoveItem to keep cachedLights in sync
			bool AddItem( ILightPriv* pItem, const char* szName )
			{
				bool ret = GenericManager<ILightPriv>::AddItem( pItem, szName );
				if( ret ) {
					RebuildCachedLightsList();
				}
				return ret;
			}

			bool RemoveItem( const char* szName )
			{
				bool ret = GenericManager<ILightPriv>::RemoveItem( szName );
				if( ret ) {
					RebuildCachedLightsList();
				}
				return ret;
			}

			void ComputeDirectLighting(
				const RayIntersectionGeometric& ri,
				const IRayCaster& pCaster,
				const IBSDF& brdf,
				const bool bReceivesShadows,
				RISEPel& amount
				) const;

			//! Returns the list of all the lights
			const LightsList& getLights() const
			{
				return cachedLights;
			}
		};
	}
}

#endif
