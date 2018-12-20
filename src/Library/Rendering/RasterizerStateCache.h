//////////////////////////////////////////////////////////////////////
//
//  RasterizerStateCache.h - Definition of the rasterizer state
//    cache, which caches a rasterizer state for a particular
//    object, and can later tell you if the rasterizer state
//    for that object has changed
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 1, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RASTERIZER_STATE_CACHE_H
#define RASTERIZER_STATE_CACHE_H

#include "../Interfaces/IObject.h"
#include "../Interfaces/IReference.h"
#include <map>

namespace RISE
{
	class RasterizerStateCache
	{
	protected:
		struct RASTVALUE
		{
			RasterizerState	state;
			RISEPel				color;
		};

		typedef std::map<const IObject*,RASTVALUE> ObjectRasterizerStateListType;
		ObjectRasterizerStateListType states;

	public:
		RasterizerStateCache()
		{
		}

		~RasterizerStateCache()
		{
		}

		void SetState( 
			const RISEPel& c, 
			const IObject* pObject,
			const RasterizerState& rast
			)
		{
			RASTVALUE val;
			val.state = rast;
			val.color = c;
			states[pObject] = val;
		}

		bool HasStateChanged( 
			RISEPel& c,
			const IObject* pObject,
			const RasterizerState& rast
			)
		{
			// First check to see if the object exists
			ObjectRasterizerStateListType::iterator it = states.find( pObject );

			if( it == states.end() ) {
				// The object doesn't even exist!
				return true;
			}

			// Otherwise check if the states have changed
			if (it->second.state == rast) {
				c = it->second.color;
				return false;
			}

			return true;
		}
	};
}

#endif
