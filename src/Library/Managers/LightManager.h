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
#include <functional>

namespace RISE
{
	namespace Implementation
	{
		class LightManager : public virtual ILightManager, public virtual GenericManager<ILightPriv>
		{
		protected:
			virtual ~LightManager();

			LightsList cachedLights;

			//! H3 (P-INVALIDATE): fired on add/remove so the Scene self-invalidates
			//! its light-topology generation -- no light mutator (incl. future ones)
			//! has to remember to bump.  Installed by Job when it wires the manager
			//! to the Scene.
			std::function<void()> mOnLightSetChanged;

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

			//! H3 (P-INVALIDATE): install the self-invalidation callback (Scene bump).
			void SetOnLightSetChanged( std::function<void()> cb ) { mOnLightSetChanged = std::move( cb ); }

			// Override AddItem/RemoveItem to keep cachedLights in sync
			bool AddItem( ILightPriv* pItem, const char* szName )
			{
				bool ret = GenericManager<ILightPriv>::AddItem( pItem, szName );
				if( ret ) {
					RebuildCachedLightsList();
					if( mOnLightSetChanged ) mOnLightSetChanged();   // H3: self-invalidate
				}
				return ret;
			}

			bool RemoveItem( const char* szName )
			{
				bool ret = GenericManager<ILightPriv>::RemoveItem( szName );
				if( ret ) {
					RebuildCachedLightsList();
					if( mOnLightSetChanged ) mOnLightSetChanged();   // H3: self-invalidate
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
