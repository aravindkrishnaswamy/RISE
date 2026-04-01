//////////////////////////////////////////////////////////////////////
//
//  MediumTracking.h - Utility for determining the current
//    participating medium at a point along a ray
//
//  Uses the IOR stack to determine which object the ray is inside,
//  then queries that object's interior medium.  Falls back to the
//  scene's global medium if no object medium is found.
//
//  This mirrors Blender/Cycles' volume stack lookup pattern:
//  the innermost enclosing object's volume shader takes priority,
//  with the world volume as fallback.
//
//  Header-only utility — no .cpp file required.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MEDIUM_TRACKING_
#define MEDIUM_TRACKING_

#include "IORStack.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IMedium.h"

namespace RISE
{
	namespace MediumTracking
	{
		/// Determine the current medium for a ray given the IOR stack and scene.
		///
		/// Resolution order (matching Cycles volume stack):
		///   1. Check the innermost enclosing object (IOR stack top)
		///      for an interior medium
		///   2. Fall back to the scene's global medium
		///   3. Return NULL (vacuum) if neither exists
		///
		/// \return Pointer to the current medium, or NULL for vacuum
		inline const IMedium* GetCurrentMedium(
			const IORStack* ior_stack,					///< [in] Current IOR stack (may be NULL)
			const IScene* pScene						///< [in] Scene (may be NULL)
			)
		{
			if( ior_stack ) {
				const IObject* pObj = ior_stack->topObject();
				if( pObj ) {
					const IMedium* m = pObj->GetInteriorMedium();
					if( m ) {
						return m;
					}
				}
			}
			return pScene ? pScene->GetGlobalMedium() : 0;
		}
	}
}

#endif
