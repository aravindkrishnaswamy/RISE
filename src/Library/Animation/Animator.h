//////////////////////////////////////////////////////////////////////
//
//  Animator.h - Implementation of the Animator class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ANIMATOR_
#define ANIMATOR_

#include "../Interfaces/IAnimator.h"
#include "../Utilities/Reference.h"
#include "ElementTimeline.h"
#include <map>

namespace RISE
{
	namespace Implementation
	{
		class Animator : 
			public virtual IAnimator, 
			public virtual Reference
		{
		protected:
			virtual ~Animator();

			typedef std::map<IKeyframable*,ElementTimeline*> ElementList;
			ElementList elements;		

		public:
			Animator();

			bool InsertKeyframe( 
				IKeyframable* pElem,									///< [in] The element to keyframe
				const String& param,									///< [in] The name of the parameter
				const String& value,									///< [in] The value for this keyframe
				const Scalar time,										///< [in] The time at which to set the keyframe
				const String* interp,									///< [in] Interpolator to use between this keyframe and the next (can be NULL)
				const String* interp_param								///< [in] Parameters for the interpolator
				);

			void EvaluateAtTime( const Scalar time );

			inline bool AreThereAnyKeyframedObjects(){ return (elements.size()>0); }
		};
	}
}

#endif
