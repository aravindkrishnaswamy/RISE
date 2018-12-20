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

	for( TimelineList::iterator it = timelines.begin(); it!=timelines.end(); it++ ) {
		GlobalLog()->PrintDelete( it->second, __FILE__, __LINE__ );
		delete it->second;
	}

	timelines.clear();
}

bool ElementTimeline::InsertKeyframe( 
	const String& parameter, 
	const String& value,
	const Scalar time,
	IFullInterpolator<KeyframeParameterDispatch>* pInterp
	)
{
	// Try to see if the given paramter already exists
	TimelineList::iterator it = timelines.find( parameter );

	if( it == timelines.end() ) {
		// Doesn't exist, so create a new one
		Timeline* tl = new Timeline( pElement, parameter );
		GlobalLog()->PrintNew( tl, __FILE__, __LINE__, "timeline" );
		timelines[parameter] = tl;
		return tl->InsertKeyframe( value, time, pInterp );
	}

	// Already exists, so just add it to that one
	return it->second->InsertKeyframe( value, time, pInterp );
}

void ElementTimeline::EvaluateAtTime( const Scalar time )
{
	// Go through all the timelines and if they are in the duration of processing, 
	// get them to do their thing
	for( TimelineList::iterator it=timelines.begin(); it!=timelines.end(); it++ ) {
//		Scalar start, end;
//		it->second->GetTimeRange( start, end );
//		if( time >= start && time <= end ) {
			it->second->EvaluateAtTime( time );
//		}
	}
	pElement->RegenerateData();
}


