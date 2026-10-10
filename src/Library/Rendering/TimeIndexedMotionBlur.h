//////////////////////////////////////////////////////////////////////
//
//  TimeIndexedMotionBlur.h - DL-465: multi-threaded per-sample motion
//    blur through per-thread time-indexed scene state.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 10, 2026
//  Tabs: 4
//  Comments:
//
//  THE LOOK IS UNCHANGED.  Every pixel sample still draws its own
//  continuous shutter time and sees the whole scene at exactly that time,
//  as the single-threaded per-sample path always did.  What changes is
//  WHERE the posed state lives: instead of moving the shared scene
//  (IAnimator::EvaluateAtTime, which forced one render thread), each
//  render thread owns a clone of every element that moves during the
//  frame and re-poses only its clones (IAnimator::EvaluateElementAtTimeInto,
//  read-only on the animator).  The shared element forwards its
//  render-time queries to the calling thread's clone through a slot
//  (Animation/TimeIndexedView.h).
//
//  WHAT IS TIME-INDEXED.
//    - Object transforms (plain Object: position / orientation / scale
//      keyframes), and every object the frame's per-sample hierarchy
//      re-compose touches (children of moving parents, DL-457/463), with
//      that re-compose replayed on the clones.  Ray intersection, area
//      sampling (luminaries), area, bounds, proximity queries and the world
//      transform accessors all answer at the thread's sample time; a hit
//      is still reported as the SHARED object, so every identity-keyed
//      consumer (IOR stack, luminary pmf lookups, SMS) is unchanged.
//    - The scene's active camera (any camera keyframe), through
//      Scene::GetCamera().
//    - Point, spot, directional and ambient lights (any of their
//      keyframes): position / target / direction / colour / energy /
//      cone, in NEE, light subpaths and photon emission alike.
//  Shared and immutable during the pass: geometry, materials, painters,
//  the TLAS (built from shutter-swept bounds, DL-457), the light-selection
//  tables and light BVH (rebuilt per frame from the same sweep, DL-463).
//
//  PER SAMPLE vs PER RAY.  Per SAMPLE (once per pixel sample, per moving
//  element, on the sample's own thread): interpolate the keyframes and
//  finalize the clone (the same work EvaluateAtTime did on the shared
//  element), then replay the hierarchy re-compose.  Per RAY: nothing is
//  evaluated -- a moving object's query costs one thread-local load and a
//  forwarded call; a static object's costs one branch on its slot.
//
//  DIAGNOSTICS.  A pose holder counts its own world-area rejection-cap hits
//  (DL-448); TimeIndexedThread folds them into the shared object when it
//  retires, so `Object::WorldAreaRejectionCapHits` covers the pass.  A
//  one-shot warning (area fallback, first cap hit) is printed at most once
//  per holder -- once per render thread per pass -- unless the shared object
//  had already printed it.
//
//  WHAT KEEPS THE SINGLE-THREADED PATH (logged once per frame, naming the
//  element): any other keyframed element (geometry parameters, painters,
//  materials, media, CSG composites and their operands, an object whose
//  shader keeps runtime data), a per-sample hierarchy re-compose no sweep
//  narrowed, and the legacy rasterizers (frozen).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TIME_INDEXED_MOTION_BLUR_
#define TIME_INDEXED_MOTION_BLUR_

#include "../Interfaces/IScene.h"
#include "../Animation/TimeIndexedView.h"
#include "../Utilities/RString.h"
#include <string>
#include <vector>

namespace RISE
{
	class IKeyframable;
	class IAnimator;

	namespace Implementation
	{
		class Object;
		class CameraCommon;

		//! One motion-blur PASS's time-indexed state, built on the rendering
		//! thread before the workers start.  Assigns a slot to every moving
		//! element and clears them again on destruction (after the workers
		//! have joined).
		class TimeIndexedFrame
		{
		public:
			//! Builds the frame's state, or returns null with `reason` set if
			//! some animated state cannot be time-indexed (the caller keeps
			//! the single-threaded per-sample path).
			static TimeIndexedFrame* TryCreate( const IScene& scene, std::string& reason );

			~TimeIndexedFrame();

			//! Frames created so far in this process (a test hook: proves
			//! a render took the time-indexed path).
			static unsigned long long CreatedCount();

			//! Number of slotted elements (camera excluded).
			unsigned int NumSlots() const { return static_cast<unsigned int>( entries.size() ); }

		private:
			friend class TimeIndexedThread;

			enum Kind { kObject, kPointLight, kSpotLight, kDirectionalLight, kAmbientLight };

			struct Entry
			{
				Kind			kind;
				IKeyframable*	shared;		//!< the shared element, as the animator keys it
				const void*		typed;		//!< the shared element as its concrete type
				bool			animated;	//!< has timelines in the active animation
			};

			struct Recompose
			{
				int				node;			//!< slot
				int				parent;			//!< slot, or -1 (then parentShared)
				const Object*	parentShared;	//!< unslotted parent, or null for a root
			};

			TimeIndexedFrame( const IScene& scene );

			int SlotOf( const void* typed ) const;
			static void SetSharedSlot( const Entry& e, const int slot );
			static int SharedSlot( const Entry& e );

			const IScene&				scene;
			IAnimator*					pAnimator;
			std::vector<Entry>			entries;		//!< index == slot
			std::vector<Recompose>		recompose;		//!< parent before child
			IKeyframable*				cameraShared;	//!< the active camera if it moves
			const CameraCommon*			camera;
		};

		//! One render thread's clones for a TimeIndexedFrame.  RAII: the
		//! constructor clones and installs the thread's slot table, the
		//! destructor uninstalls it and releases the clones.  A null frame
		//! makes it a no-op (the static and fallback cases).
		class TimeIndexedThread
		{
		public:
			explicit TimeIndexedThread( const TimeIndexedFrame* frame );
			~TimeIndexedThread();

			//! Poses this thread's clones at `time`.
			void PoseAt( const Scalar time );

			//! The calling thread's installed instance, or null.
			static TimeIndexedThread* Current();

		private:
			TimeIndexedThread( const TimeIndexedThread& );
			TimeIndexedThread& operator=( const TimeIndexedThread& );

			const TimeIndexedFrame*			frame;
			std::vector<const void*>		slots;			//!< per slot, as the concrete type
			std::vector<IKeyframable*>		keyframable;	//!< per slot, the clone as IKeyframable
			std::vector<Object*>			objects;		//!< per slot, the clone if an Object
			std::vector<const IReference*>	owned;
			CameraCommon*					camera;
			TimeIndexed::ThreadSlots		table;
			const TimeIndexed::ThreadSlots*	previousTable;
			TimeIndexedThread*				previousCurrent;
		};
	}
}

#endif
