//////////////////////////////////////////////////////////////////////
//
//  IKeyframable.h - The interface that any class which wants to be
//    keyframable must implement
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 25, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IKEYFRAMABLE_
#define IKEYFRAMABLE_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/RString.h"
#include "IReference.h"

namespace RISE
{
	class IKeyframeParameter : public virtual IReference
	{
	protected:
		IKeyframeParameter(){};
		virtual ~IKeyframeParameter(){};

		unsigned int	paramid;			// Used by the IKeyframable class to figure out which param this actually is

	public:
		// These methods are required for interpolation
		virtual void Add( IKeyframeParameter& result, const IKeyframeParameter& a, const IKeyframeParameter& b ) = 0;
		virtual void Subtract( IKeyframeParameter& result, const IKeyframeParameter& a, const IKeyframeParameter& b ) = 0;
		virtual void ScalarMult( IKeyframeParameter& result, const IKeyframeParameter& a, const Scalar& t ) = 0;

		// Clones this parameter type
		virtual IKeyframeParameter* Clone() const = 0;

		virtual void* getValue( ) const = 0;
		virtual void setValue( void* v ) = 0;

		inline unsigned int getID() const { return paramid; }
			
	};

	class IKeyframable : public virtual IReference
	{
	protected:
		IKeyframable(){};
		virtual ~IKeyframable(){};

	public:

		//! This function takes an two input strings, one which describes the element
		//! to keyframe, the other is the value at the keyframe
		virtual IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) = 0;

		//! Tells the keyframable element to set a new interpolated value
		virtual void SetIntermediateValue( const IKeyframeParameter& val ) = 0;

		//! Tells the keyframable element that all values are set and that it should now use
		//! the new keyframed values to regenerate its data and prepare for rendering
		virtual void RegenerateData( ) = 0;
	};
}

#endif

