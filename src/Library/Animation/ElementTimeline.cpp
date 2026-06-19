//////////////////////////////////////////////////////////////////////
//
//  ElementTimeline.cpp - Implementation of the ElementTimeline class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ElementTimeline.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

ElementTimeline::ElementTimeline(
			IKeyframable* pElem				///< [in] Element we are keyframing
			) :
  pElement( pElem )
{
	if( pElement ) {
		pElement->addref();
	}
}


ElementTimeline::~ElementTimeline()
{
	safe_release( pElement );

	for( AnimationTimelineList::iterator a = animations.begin(); a!=animations.end(); a++ ) {
		for( TimelineList::iterator it = a->second.begin(); it!=a->second.end(); it++ ) {
			GlobalLog()->PrintDelete( it->second, __FILE__, __LINE__ );
			delete it->second;
		}
		a->second.clear();
	}

	animations.clear();
}

bool ElementTimeline::InsertKeyframe(
	const String& animation,
	const String& parameter,
	const String& value,
	const Scalar time,
	IFullInterpolator<KeyframeParameterDispatch>* pInterp
	)
{
	// Find (or create) the parameter->Timeline map for this animation
	TimelineList& tl = animations[animation];

	// Try to see if the given parameter already exists in that animation
	TimelineList::iterator it = tl.find( parameter );

	if( it == tl.end() ) {
		// Doesn't exist, so create a new one
		Timeline* timeline = new Timeline( pElement, parameter );
		GlobalLog()->PrintNew( timeline, __FILE__, __LINE__, "timeline" );
		tl[parameter] = timeline;
		return timeline->InsertKeyframe( value, time, pInterp );
	}

	// Already exists, so just add it to that one
	return it->second->InsertKeyframe( value, time, pInterp );
}

void ElementTimeline::EvaluateAtTimeForAnimation( const Scalar time, const String& animation )
{
	AnimationTimelineList::iterator a = animations.find( animation );
	if( a == animations.end() ) {
		// This element has no timelines in the active animation; leave it
		// untouched (do NOT RegenerateData).
		return;
	}

	// Go through all the timelines for this animation and get them to do
	// their thing.
	for( TimelineList::iterator it=a->second.begin(); it!=a->second.end(); it++ ) {
		it->second->EvaluateAtTime( time );
	}
	pElement->RegenerateData();
}
