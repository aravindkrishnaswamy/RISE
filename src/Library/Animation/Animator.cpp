//////////////////////////////////////////////////////////////////////
//
//  Animator.cpp - Implementation of the Animator class
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
#include "Animator.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/SimpleInterpolators.h"
#include "../Utilities/CubicInterpolator.h"
#include "../Utilities/HermiteInterpolator.h"
#include <cstring>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Name of the implicit animation that owns untagged keyframes and the
	//! legacy animation_options.  Single-token (no whitespace) so it cannot
	//! collide with a scene-authored name through the chunk tokenizer.
	const char* const DEFAULT_ANIM_NAME = "(default)";

	//! A String is blank if it has no characters (covers both a
	//! default-constructed String and one built from "").
	inline bool IsBlankName( const RISE::String& s )
	{
		return strlen( s.c_str() ) == 0;
	}
}

Animator::Animator() :
  activeWasSet( false )
{
}

Animator::~Animator()
{
	for( ElementList::iterator it = elements.begin(); it!=elements.end(); it++ ) {
		it->first->release();
		GlobalLog()->PrintDelete( it->second, __FILE__, __LINE__ );
		delete it->second;
	}

	elements.clear();
}

unsigned int Animator::EnsureAnimation( const String& name )
{
	const String n = IsBlankName( name ) ? String( DEFAULT_ANIM_NAME ) : name;

	for( unsigned int i=0; i<animations.size(); i++ ) {
		if( animations[i].name == n.c_str() ) {
			return i;
		}
	}

	NamedAnimation na;
	na.name = n;
	na.time_start = 0.0;
	na.time_end = 1.0;
	na.num_frames = 30;
	na.do_fields = false;
	na.invert_fields = false;
	animations.push_back( na );

	// The first animation created becomes active unless an explicit active
	// animation has already been chosen.
	if( !activeWasSet && IsBlankName( activeAnimation ) ) {
		activeAnimation = n;
	}

	return (unsigned int)animations.size()-1;
}

String Animator::ResolveActiveName() const
{
	if( !IsBlankName( activeAnimation ) ) {
		return activeAnimation;
	}
	if( !animations.empty() ) {
		return animations[0].name;
	}
	return String( DEFAULT_ANIM_NAME );
}

bool Animator::InsertKeyframe(
	IKeyframable* pElem,
	const String& param,
	const String& value,
	const Scalar time,
	const String* interp,
	const String* interp_param
	)
{
	return InsertKeyframeForAnimation( pElem, param, value, time, interp, interp_param, String( DEFAULT_ANIM_NAME ) );
}

bool Animator::InsertKeyframeForAnimation(
	IKeyframable* pElem,
	const String& param,
	const String& value,
	const Scalar time,
	const String* interp,
	const String* interp_param,
	const String& animation
	)
{
	const String anim = IsBlankName( animation ) ? String( DEFAULT_ANIM_NAME ) : animation;
	EnsureAnimation( anim );

	IFullInterpolator<KeyframeParameterDispatch>* pInterp = 0;
	if( interp ) {
		// Find out what kind of interpolator
		if( *interp == "linear" ) {
			pInterp = new LinearInterpolator<KeyframeParameterDispatch>();
			GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "linear interpolator" );
		} else if( *interp == "cosine" ) {
			pInterp = new CosineInterpolator<KeyframeParameterDispatch>();
			GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "cosine interpolator" );
		} else if( *interp == "catmull-rom" ) {
			pInterp = new CatmullRomCubicInterpolator<KeyframeParameterDispatch>();
			GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "catmullrom interpolator" );
		} else if( *interp == "b-spline" ) {
			pInterp = new UniformBSplineCubicInterpolator<KeyframeParameterDispatch>();
			GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "bspline interpolator" );
		} else if( *interp == "hermite" ) {
			Scalar tension=0, bias=0, continuity=0;
			if( interp_param ) {
				sscanf( interp_param->c_str(), "%lf %lf %lf", &tension, &bias, &continuity );
			}
			pInterp = new HermiteInterpolator<KeyframeParameterDispatch>( tension, bias, continuity );
			GlobalLog()->PrintNew( pInterp, __FILE__, __LINE__, "hermite interpolator" );
		} else {
			GlobalLog()->PrintEx( eLog_Warning, "Animator::InsertKeyframeForAnimation: Unknown type of interpolator \'%s\'", interp->c_str() );
		}
	}

	bool bRet;

	ElementList::iterator it = elements.find( pElem );
	if( it == elements.end() ) {
		// Doesn't exist, so create a new one
		ElementTimeline* tm = new ElementTimeline( pElem );
		GlobalLog()->PrintNew( tm, __FILE__, __LINE__, "element timeline" );
		elements[pElem] = tm;
		pElem->addref();
		bRet = tm->InsertKeyframe( anim, param, value, time, pInterp );
	} else {
		bRet = it->second->InsertKeyframe( anim, param, value, time, pInterp );
	}

	safe_release( pInterp );

	return bRet;
}

void Animator::EvaluateAtTime( const Scalar time )
{
	// EvaluateAtTime is called once per temporal sample during motion blur,
	// so bind the active name by reference (no per-call String allocation).
	// activeAnimation is guaranteed non-blank once any keyframe exists
	// (EnsureAnimation sets it); the fallbacks below are purely defensive.
	const String& active =
		!IsBlankName( activeAnimation ) ? activeAnimation :
		( !animations.empty() ? animations[0].name : activeAnimation );

	for( ElementList::iterator it=elements.begin(); it!=elements.end(); it++ ) {
		it->second->EvaluateAtTimeForAnimation( time, active );
	}
}

bool Animator::DeclareAnimation(
	const String& name,
	const double time_start,
	const double time_end,
	const unsigned int num_frames,
	const bool do_fields,
	const bool invert_fields,
	const bool make_active
	)
{
	const String n = IsBlankName( name ) ? String( DEFAULT_ANIM_NAME ) : name;
	const unsigned int idx = EnsureAnimation( n );

	animations[idx].time_start = time_start;
	animations[idx].time_end = time_end;
	animations[idx].num_frames = num_frames;
	animations[idx].do_fields = do_fields;
	animations[idx].invert_fields = invert_fields;

	if( make_active ) {
		activeAnimation = n;
		activeWasSet = true;
	}

	return true;
}

bool Animator::SetActiveAnimationByName( const String& name )
{
	const String n = IsBlankName( name ) ? String( DEFAULT_ANIM_NAME ) : name;
	for( unsigned int i=0; i<animations.size(); i++ ) {
		if( animations[i].name == n.c_str() ) {
			activeAnimation = n;
			activeWasSet = true;
			return true;
		}
	}
	GlobalLog()->PrintEx( eLog_Warning, "Animator::SetActiveAnimationByName: unknown animation \'%s\'", n.c_str() );
	return false;
}

bool Animator::SetActiveAnimationByIndex( const unsigned int index )
{
	if( index >= animations.size() ) {
		return false;
	}
	activeAnimation = animations[index].name;
	activeWasSet = true;
	return true;
}

unsigned int Animator::GetAnimationCount() const
{
	return (unsigned int)animations.size();
}

String Animator::GetAnimationName( const unsigned int index ) const
{
	if( index >= animations.size() ) {
		return String();
	}
	return animations[index].name;
}

unsigned int Animator::GetActiveAnimationIndex() const
{
	const String n = ResolveActiveName();
	for( unsigned int i=0; i<animations.size(); i++ ) {
		if( animations[i].name == n.c_str() ) {
			return i;
		}
	}
	return 0;
}

String Animator::GetActiveAnimationName() const
{
	if( animations.empty() ) {
		return String();
	}
	return ResolveActiveName();
}

bool Animator::GetActiveAnimationOptions(
	double& time_start,
	double& time_end,
	unsigned int& num_frames,
	bool& do_fields,
	bool& invert_fields
	) const
{
	if( animations.empty() ) {
		return false;
	}
	const String n = ResolveActiveName();
	for( unsigned int i=0; i<animations.size(); i++ ) {
		if( animations[i].name == n.c_str() ) {
			time_start = animations[i].time_start;
			time_end = animations[i].time_end;
			num_frames = animations[i].num_frames;
			do_fields = animations[i].do_fields;
			invert_fields = animations[i].invert_fields;
			return true;
		}
	}
	return false;
}
