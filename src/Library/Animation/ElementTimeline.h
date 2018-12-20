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
		class ElementTimeline
		{
		protected:
			IKeyframable* pElement;						///< The element we are keyframing
			typedef std::map<String,Timeline*> TimelineList;
			TimelineList timelines;		

		public:
			ElementTimeline(
				IKeyframable* pElem				///< [in] Element we are keyframing
				);

			virtual ~ElementTimeline();

			bool InsertKeyframe( 
				const String& parameter, 
				const String& value, 
				const Scalar time,
				IFullInterpolator<KeyframeParameterDispatch>* pInterp
				);
			void EvaluateAtTime( const Scalar time );
		};
	}
}

#endif
