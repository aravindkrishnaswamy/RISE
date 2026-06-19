//////////////////////////////////////////////////////////////////////
//
//  Animator.h - Implementation of the Animator class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ANIMATOR_
#define ANIMATOR_

#include "../Interfaces/IAnimator.h"
#include "../Utilities/Reference.h"
#include "ElementTimeline.h"
#include <map>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Animator :
			public virtual IAnimator,
			public virtual Reference
		{
		protected:
			virtual ~Animator();

			typedef std::map<IKeyframable*,ElementTimeline*> ElementList;
			ElementList elements;

			//! A named animation = a name plus its playback options.  The set
			//! of timelines belonging to it lives in the per-element
			//! ElementTimelines, keyed by this name.
			struct NamedAnimation
			{
				String			name;
				double			time_start;
				double			time_end;
				unsigned int	num_frames;
				bool			do_fields;
				bool			invert_fields;
			};
			typedef std::vector<NamedAnimation> AnimationList;
			AnimationList	animations;			///< Declared named animations, in scene order
			String			activeAnimation;	///< Name of the active animation (blank => resolve to first/default)
			bool			activeWasSet;		///< Whether an explicit active animation was chosen

			//! Ensures a named animation exists (auto-creates with default
			//! options).  Blank name => the implicit default animation.  The
			//! very first animation created becomes active unless an explicit
			//! active animation was already chosen.  Returns its index.
			unsigned int EnsureAnimation( const String& name );

			//! Resolves the effective active animation name (handles the
			//! blank/never-set and no-animations cases).
			String ResolveActiveName() const;

		public:
			Animator();

			// Legacy entry point (routes to the implicit default animation).
			bool InsertKeyframe(
				IKeyframable* pElem,									///< [in] The element to keyframe
				const String& param,									///< [in] The name of the parameter
				const String& value,									///< [in] The value for this keyframe
				const Scalar time,										///< [in] The time at which to set the keyframe
				const String* interp,									///< [in] Interpolator to use between this keyframe and the next (can be NULL)
				const String* interp_param								///< [in] Parameters for the interpolator
				);

			void EvaluateAtTime( const Scalar time );

			inline bool AreThereAnyKeyframedObjects(){ return (elements.size()>0); }

			// Named animation paths
			bool InsertKeyframeForAnimation(
				IKeyframable* pElem,
				const String& param,
				const String& value,
				const Scalar time,
				const String* interp,
				const String* interp_param,
				const String& animation
				);
			bool DeclareAnimation(
				const String& name,
				const double time_start,
				const double time_end,
				const unsigned int num_frames,
				const bool do_fields,
				const bool invert_fields,
				const bool make_active
				);
			bool SetActiveAnimationByName( const String& name );
			bool SetActiveAnimationByIndex( const unsigned int index );
			unsigned int GetAnimationCount() const;
			String GetAnimationName( const unsigned int index ) const;
			unsigned int GetActiveAnimationIndex() const;
			String GetActiveAnimationName() const;
			bool GetActiveAnimationOptions(
				double& time_start,
				double& time_end,
				unsigned int& num_frames,
				bool& do_fields,
				bool& invert_fields
				) const;
		};
	}
}

#endif
