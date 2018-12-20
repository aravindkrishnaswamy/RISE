//////////////////////////////////////////////////////////////////////
//
//  RuntimeContext.h - Declaration of the runtime context.
//    Currently the runtime context is just a POD to store important
//    run-time dependent (i.e. thread dependent) variables like
//    random number generators (since they store thread specific state
//    information
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 18, 2006
//  Tabs: 4
//  Comments: This was taken directly from ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RUNTIME_CONTEXT_
#define RUNTIME_CONTEXT_

#include "../Interfaces/IReference.h"
#include "RandomNumbers.h"
#include "../Rendering/RasterizerStateCache.h"
#include <map>

namespace RISE
{
	// This is this currently just a really simple POD, no reference counting
	// and no real methods, this may however change
	struct RuntimeContext
	{
		enum PASS
		{
			PASS_NORMAL,					// Normal rendering pass
			PASS_IRRADIANCE_CACHE			// Pass to fill the irradiance cache with data
		};

		const RandomNumberGenerator& random;					// A regular random number generator
		const PASS pass;										// The current pass
		const bool bThreaded;									// Are we rendering with multiple threads

		typedef std::map<const IReference*,RasterizerStateCache*> StateCacheMapType;
		mutable StateCacheMapType								stateCaches;

		RuntimeContext(
			const RandomNumberGenerator& random_,
			const PASS pass_,
			const bool bThreaded_
			) : 
		  random( random_ ),
		  pass( pass_ ),
		  bThreaded( bThreaded_ )
		{}

	    ~RuntimeContext()
		{
			// Delete the rasterizer state caches
			StateCacheMapType::iterator it;
			for( it=stateCaches.begin(); it!=stateCaches.end(); it++ ) {
				delete it->second;
			}

			stateCaches.clear();
		}

		// State cache stuff
		void StateCache_SetState(
			const IReference* pObj,
			const RISEPel& c, 
			const IObject* pObject,
			const RasterizerState& rast
			) const
		{
			RasterizerStateCache* pCache = 0;
			StateCacheMapType::iterator it = stateCaches.find( pObj );
			if( it == stateCaches.end() ) {
				pCache = new RasterizerStateCache();
			} else {
				pCache = it->second;
			}

			pCache->SetState( c, pObject, rast );
		}

		bool StateCache_HasStateChanged(
			const IReference* pObj,
			RISEPel& c,
			const IObject* pObject,
			const RasterizerState& rast
			) const
		{
			RasterizerStateCache* pCache = 0;
			StateCacheMapType::iterator it = stateCaches.find( pObj );
			if( it == stateCaches.end() ) {
				pCache = new RasterizerStateCache();
				stateCaches[pObj] = pCache;
				return true;
			} else {
				pCache = it->second;
			}

			return pCache->HasStateChanged( c, pObject, rast );
		}
	};
}

#endif


