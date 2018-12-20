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

			mutable LightsList templist;

		public:
			LightManager();

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
				templist.clear();
				GenericManager<ILightPriv>::ItemListType::const_iterator		i, e;
				for( i=items.begin(), e=items.end(); i!=e; i++ ) {
					templist.push_back( i->second.first );
				}
				return templist;
			}
		};
	}
}

#endif
