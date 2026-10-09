//////////////////////////////////////////////////////////////////////
//
//  Timeline.h - Implementation of the Timeline class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TIMELINE_
#define TIMELINE_

#include "Keyframe.h"
#include "../Interfaces/IKeyframable.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Timeline
		{
		protected:
			IKeyframable* pElement;						///< The element we are keyframing
			String paramname;							///< Parameter name we are keyframing
			typedef std::vector<Keyframe*> KeyframesList;
			KeyframesList keyframes;

			typedef std::vector<KeyframeParameterDispatch> RuntimeKeyframesList;
			RuntimeKeyframesList runtimekeyframes;

			IFullInterpolator<KeyframeParameterDispatch>* pInterp;	// Default interpolator (linear)

			Scalar					start;
			Scalar					end;

			void EvaluateRange();
			void CreateRuntimeKeyframesList();

		public:
			Timeline(
				IKeyframable* pElem,			///< [in] Element we are keyframing
				const String& param				///< [in] The parameter we are keyframing
				);

			virtual ~Timeline();

			bool InsertKeyframe( 
				const String& value, 
				const Scalar time,
				IFullInterpolator<KeyframeParameterDispatch>* pInterp
				);
			void GetTimeRange( Scalar& begin, Scalar& end );
			void EvaluateAtTime( const Scalar time );

			//! DL-457: appends every keyframe time strictly inside (t0, t1).
			void CollectKeyframeTimes( const Scalar t0, const Scalar t1, std::vector<Scalar>& out ) const
			{
				for( KeyframesList::const_iterator k = keyframes.begin(); k != keyframes.end(); ++k ) {
					if( (*k)->time > t0 && (*k)->time < t1 ) out.push_back( (*k)->time );
				}
			}
		};
	}
}

#endif
