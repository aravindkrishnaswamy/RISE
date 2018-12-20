//////////////////////////////////////////////////////////////////////
//
//  IAnimator.h - Interface to a timeline, which holds a bunch
//    of keyframes for some keyframable element
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IANIMATOR_
#define IANIMATOR_

#include "IReference.h"
#include "IKeyframable.h"
#include "../Utilities/RString.h"

namespace RISE
{
	//! The animator stores all the current keyframed elements and their
	//! timelines
	class IAnimator : public virtual IReference
	{
	protected:
		IAnimator(){};
		virtual ~IAnimator(){};

	public:
		//! Inserts a keyframe at the indicated time
		virtual bool InsertKeyframe( 
			IKeyframable* pElem,									///< [in] The element to keyframe
			const String& param,									///< [in] The name of the parameter
			const String& value,									///< [in] The value for this keyframe
			const Scalar time,										///< [in] The time at which to set the keyframe
			const String* interp,									///< [in] Interpolator to use between this keyframe and the next (can be NULL)
			const String* interp_param								///< [in] Parameters for the interpolator
			) = 0;

		//! Evaluates the entire animation at the given time
		virtual void EvaluateAtTime( const Scalar time ) = 0;

		//! Tells us whether anything is keyframed
		virtual bool AreThereAnyKeyframedObjects() = 0;
	};
}

#endif

