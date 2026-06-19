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
	//! timelines.
	//!
	//! NAMED ANIMATION PATHS: a scene may declare several named animations
	//! (each a group of timelines plus playback options); exactly one is
	//! "active" at a time, and EvaluateAtTime() drives only the active one.
	//! Untagged keyframes, the legacy InsertKeyframe() below, and the
	//! `animation_options` chunk all map to an implicit "(default)"
	//! animation, so pre-existing scenes behave exactly as before.
	class IAnimator : public virtual IReference
	{
	protected:
		IAnimator(){};
		virtual ~IAnimator(){};

	public:
		//! Inserts a keyframe at the indicated time (legacy; routes to the
		//! implicit "(default)" animation).
		virtual bool InsertKeyframe(
			IKeyframable* pElem,									///< [in] The element to keyframe
			const String& param,									///< [in] The name of the parameter
			const String& value,									///< [in] The value for this keyframe
			const Scalar time,										///< [in] The time at which to set the keyframe
			const String* interp,									///< [in] Interpolator to use between this keyframe and the next (can be NULL)
			const String* interp_param								///< [in] Parameters for the interpolator
			) = 0;

		//! Evaluates the ACTIVE animation at the given time.
		//! WARNING: This mutates keyframed scene elements (camera, transforms,
		//! painters) through stored pointers. When called during multi-threaded
		//! temporal sampling, this is a pre-existing data race in the animation
		//! system. A proper fix would require per-thread interpolated state.
		virtual void EvaluateAtTime( const Scalar time ) = 0;

		//! Tells us whether anything is keyframed
		virtual bool AreThereAnyKeyframedObjects() = 0;

		//
		// Named animation paths (additive; appended at the end of the vtable
		// so the slots above stay ABI-stable for existing callers).  The only
		// implementer is Implementation::Animator.
		//

		//! Inserts a keyframe owned by a named animation.  An empty/blank
		//! animation name maps to the implicit "(default)" animation.  The
		//! named animation is auto-created (with default options) if it does
		//! not already exist.
		virtual bool InsertKeyframeForAnimation(
			IKeyframable* pElem,									///< [in] The element to keyframe
			const String& param,									///< [in] The name of the parameter
			const String& value,									///< [in] The value for this keyframe
			const Scalar time,										///< [in] The time at which to set the keyframe
			const String* interp,									///< [in] Interpolator to use between this keyframe and the next (can be NULL)
			const String* interp_param,								///< [in] Parameters for the interpolator
			const String& animation									///< [in] Owning named animation (blank => default)
			) = 0;

		//! Declares (or updates the playback options of) a named animation.
		//! If make_active is true, the animation becomes the active one.
		virtual bool DeclareAnimation(
			const String& name,										///< [in] Animation name (blank => default)
			const double time_start,								///< [in] Scene time to start at
			const double time_end,									///< [in] Scene time to finish at
			const unsigned int num_frames,							///< [in] Number of frames
			const bool do_fields,									///< [in] Emit interlaced fields
			const bool invert_fields,								///< [in] Invert field order
			const bool make_active									///< [in] Make this the active animation
			) = 0;

		//! Selects the active animation by name; false if the name is unknown.
		virtual bool SetActiveAnimationByName( const String& name ) = 0;

		//! Selects the active animation by index; false if out of range.
		virtual bool SetActiveAnimationByIndex( const unsigned int index ) = 0;

		//! Number of declared named animations.
		virtual unsigned int GetAnimationCount() const = 0;

		//! Name of the animation at the given index ("" if out of range).
		virtual String GetAnimationName( const unsigned int index ) const = 0;

		//! Index of the active animation (0 if none declared).
		virtual unsigned int GetActiveAnimationIndex() const = 0;

		//! Name of the active animation ("" if none declared).
		virtual String GetActiveAnimationName() const = 0;

		//! Playback options of the active animation; false if none declared.
		virtual bool GetActiveAnimationOptions(
			double& time_start,
			double& time_end,
			unsigned int& num_frames,
			bool& do_fields,
			bool& invert_fields
			) const = 0;
	};
}

#endif
