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

using namespace RISE;
using namespace RISE::Implementation;

Animator::Animator()
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

bool Animator::InsertKeyframe( 
	IKeyframable* pElem,									///< [in] The element to keyframe
	const String& param,									///< [in] The name of the parameter
	const String& value,									///< [in] The value for this keyframe
	const Scalar time,										///< [in] The time at which to set the keyframe
	const String* interp,									///< [in] Interpolator to use between this keyframe and the next (can be NULL)
	const String* interp_param								///< [in] Parameters for the interpolator
	)
{
	ElementList::iterator it = elements.find( pElem );

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
			GlobalLog()->PrintEx( eLog_Warning, "Animator::InsertKeyframe: Unknown type of interpolator \'%s\'", interp->c_str() );
		}
	}

	bool bRet;

	if( it == elements.end() ) {
		// Doesn't exist, so create a new one
		ElementTimeline* tm = new ElementTimeline( pElem );
		GlobalLog()->PrintNew( tm, __FILE__, __LINE__, "element timeline" );
		elements[pElem] = tm;
		pElem->addref();
		bRet = tm->InsertKeyframe( param, value, time, pInterp );
	} else {
		bRet = it->second->InsertKeyframe( param, value, time, pInterp );
	}

	safe_release( pInterp );

	return bRet;
}

void Animator::EvaluateAtTime( const Scalar time )
{
	for( ElementList::iterator it=elements.begin(); it!=elements.end(); it++ ) {
		it->second->EvaluateAtTime( time );
	}
}
