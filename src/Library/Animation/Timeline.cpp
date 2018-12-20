//////////////////////////////////////////////////////////////////////
//
//  Timeline.cpp - Implementation of the Timeline class
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
#include "Timeline.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Interfaces/ILog.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

Timeline::Timeline( 
	IKeyframable* pElem,			///< [in] Element we are keyframing
	const String& param 
	) : 
  pElement( pElem ),
  paramname( param ),
  start( 0 ),
  end( 0 )
{
	if( pElement ) {
		pElement->addref();
	}

	pInterp = new LinearInterpolator<KeyframeParameterDispatch>();
	GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "timeline default interpolator" );
}

Timeline::~Timeline()
{
	safe_release( pElement );
	safe_release( pInterp );

	for( KeyframesList::iterator it = keyframes.begin(); it!=keyframes.end(); it++ ) {
		GlobalLog()->PrintDelete( (*it), __FILE__, __LINE__ );
		delete (*it);
	}

	keyframes.clear();
}

bool Timeline::InsertKeyframe( 
	const String& value, 
	const Scalar time,
	IFullInterpolator<KeyframeParameterDispatch>* pInterp
	)
{
	IKeyframeParameter* pKF = pElement->KeyframeFromParameters( paramname, value );

	if( !pKF ) {
		GlobalLog()->PrintEasyError( "Timeline::InsertKeyframe: Element didn't want to add a keyframe here" );
		return false;
	}

	Keyframe* kf = new Keyframe( *pKF, time, pInterp );
	GlobalLog()->PrintNew( kf, __FILE__, __LINE__, "keyframe" );
	safe_release( pKF );

	keyframes.push_back( kf );
	std::sort( keyframes.begin(), keyframes.end(), &Keyframe::KeyframeCompare );
	EvaluateRange();
	CreateRuntimeKeyframesList();
	return true;
}

void Timeline::EvaluateRange( )
{
	if( keyframes.size() < 1 )
	{
		start = end = 0;
		return;
	}
	else if( keyframes.size() < 2 )
	{
		start = end = (*(keyframes.begin()))->time;
		return;
	}

	start = (*(keyframes.begin()))->time;
	end = (*(keyframes.end()-1))->time;
}

void Timeline::CreateRuntimeKeyframesList()
{
	runtimekeyframes.clear();
	runtimekeyframes.reserve( keyframes.size() );

	for( KeyframesList::const_iterator it = keyframes.begin(); it != keyframes.end(); it++ ) {
		runtimekeyframes.push_back( KeyframeParameterDispatch((*it)->param) );
	}
}

void Timeline::GetTimeRange( Scalar& begin, Scalar& end )
{
	begin = this->start;
	end = this->end;
}

void Timeline::EvaluateAtTime( const Scalar time )
{
	// Use an interpolator and evaluate at the current time
	// There is only one keyframe, which means we are asymptotic...
	// We only render the one frame we are to render
	if( start == end ) {
		if( time == start ) {
			pElement->SetIntermediateValue( (*keyframes.begin())->param );
			return;
		}
	}

	if( time <= start ) {
		pElement->SetIntermediateValue( (*keyframes.begin())->param );
		return;
	}

	if( time >= end ) {
		pElement->SetIntermediateValue( (*(keyframes.end()-1))->param );
		return;
	}

	// Otherwise we need to find the two keyframes we are to interpolate
	// and call the interpolator to interpolate them
	KeyframesList::iterator	i, e;
	RuntimeKeyframesList::iterator m=runtimekeyframes.begin();

	for( i=keyframes.begin(), e=keyframes.end()-1; i!=e; i++, m++ )
	{
		if( time >= (*i)->time &&
			time <= (*(i+1))->time
			)
		{
			// We've found our keyframes
			// We need to store the result somewhere, we'll ask a keyframeparameter
			// to clone itself so we can store it there
			const Keyframe* before = *i;
			const Keyframe* after = *(i+1);

			const Scalar d = (time - before->time) / (after->time - before->time);

			RuntimeKeyframesList::const_iterator s = m;
			RuntimeKeyframesList::const_iterator t = (m+1);

			if( before->pInterp ) {
				KeyframeParameterDispatch result = before->pInterp->Interpolate2Values( runtimekeyframes, s, t, d );
				pElement->SetIntermediateValue( result.GetParam() );
			} else {
				KeyframeParameterDispatch result = pInterp->Interpolate2Values( runtimekeyframes, s, t, d );
				pElement->SetIntermediateValue( result.GetParam() );
			}
			return;
		}
	}
}

