//////////////////////////////////////////////////////////////////////
//
//  ElementTimeline.h - Implementation of the ElementTimeline class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ELEMENT_TIMELINE_
#define ELEMENT_TIMELINE_

#include "../Interfaces/IKeyframable.h"
#include "Timeline.h"
#include <map>

namespace RISE
{
	namespace Implementation
	{
		//! Holds all the keyframe timelines for a single element, grouped by
		//! the named animation they belong to.  An element may be keyframed
		//! under several animations (e.g. the same camera parameter follows a
		//! different curve in each named animation); evaluation only ever
		//! touches the timelines of the animation requested.
		class ElementTimeline
		{
		protected:
			IKeyframable* pElement;						///< The element we are keyframing
			typedef std::map<String,Timeline*> TimelineList;			///< parameter -> Timeline
			typedef std::map<String,TimelineList> AnimationTimelineList;	///< animation -> (parameter -> Timeline)
			AnimationTimelineList animations;

		public:
			ElementTimeline(
				IKeyframable* pElem				///< [in] Element we are keyframing
				);

			virtual ~ElementTimeline();

			bool InsertKeyframe(
				const String& animation,		///< [in] Owning named animation
				const String& parameter,
				const String& value,
				const Scalar time,
				IFullInterpolator<KeyframeParameterDispatch>* pInterp
				);

			//! Evaluates only the timelines belonging to the named animation.
			//! If this element has no timelines in that animation it is left
			//! untouched (RegenerateData is not called).
			void EvaluateAtTimeForAnimation( const Scalar time, const String& animation );

			//! DL-465: the same evaluation written into `target` (a clone of
			//! this element) instead of the element itself, then
			//! target.RegenerateData().  Read-only on the timelines; returns
			//! false (target untouched) if the element has no timelines in
			//! the named animation.
			bool EvaluateIntoForAnimation( const Scalar time, const String& animation, IKeyframable& target ) const;

			//! DL-465: names of the parameters keyframed in the named animation.
			void GetAnimatedParameters( const String& animation, std::vector<String>& out ) const;

			//! DL-457: appends the named animation's keyframe times inside (t0, t1).
			void CollectKeyframeTimesForAnimation( const Scalar t0, const Scalar t1, const String& animation, std::vector<Scalar>& out ) const;
		};
	}
}

#endif
