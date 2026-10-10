//////////////////////////////////////////////////////////////////////
//
//  TimeIndexedView.h - DL-465: the per-thread lookup that lets one
//    shared scene element answer at the calling render thread's own
//    motion-blur sample time.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 10, 2026
//  Tabs: 4
//  Comments:
//
//  WHAT THIS IS.  A motion-blurred frame gives every pixel sample its own
//  continuous shutter time.  Historically the sample moved the SHARED
//  scene there (IAnimator::EvaluateAtTime), so such a frame could only
//  render on one thread.  Under DL-465 every render thread instead owns a
//  private clone of each element that moves during the frame (object
//  transforms, the camera, delta lights), re-posed at that thread's sample
//  time; the shared element forwards its render-time queries to the
//  calling thread's clone.  Geometry, materials, the TLAS (built from
//  shutter-swept bounds, DL-457) and the light-selection tables (swept,
//  DL-463) stay shared and immutable.
//
//  HOW A SHARED ELEMENT FINDS ITS CLONE.  During such a pass the frame
//  assigns each moving element a SLOT (a plain int on the element, written
//  on the calling thread before the workers start and reset after they
//  join), and each worker installs a ThreadSlots table here.  An element
//  with slot >= 0 asks Lookup<T>( slot ); a null answer (no table on this
//  thread, or the element is not moving) means "answer from yourself".
//  A static element's slot is -1, so its cost is one predictable branch on
//  a member and nothing else -- static scenes are bit-identical.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TIME_INDEXED_VIEW_
#define TIME_INDEXED_VIEW_

namespace RISE
{
	class ICamera;

	namespace TimeIndexed
	{
		//! One render thread's table: slots[i] is this thread's clone of the
		//! element holding slot i, stored as `const void*` converted from
		//! the element's OWN concrete type (Object, PointLight, ...) and read
		//! back as exactly that type.  `camera` is the thread's clone of the
		//! scene's active camera when the camera moves, else null.
		struct ThreadSlots
		{
			const void* const*	slots;
			unsigned int		count;
			const ICamera*		camera;
		};

		//! The calling thread's table, or null (every non-render thread, and
		//! a render thread outside a time-indexed pass).
		inline thread_local const ThreadSlots* tlsSlots = nullptr;

		//! The calling thread's clone for `slot`, or null.  `T` must be the
		//! type the clone was stored as (see ThreadSlots).
		template< class T >
		inline const T* Lookup( const int slot )
		{
			if( slot < 0 ) {
				return nullptr;
			}
			const ThreadSlots* t = tlsSlots;
			if( !t || static_cast<unsigned int>( slot ) >= t->count ) {
				return nullptr;
			}
			return static_cast<const T*>( t->slots[slot] );
		}

		//! A time-indexed slot for an element that is COPY-constructed into
		//! its per-thread clones (lights): the copy is never itself slotted,
		//! or the clone would forward every query back to itself.
		struct TimeSlot
		{
			int value;
			TimeSlot() : value( -1 ) {}
			TimeSlot( const TimeSlot& ) : value( -1 ) {}
			TimeSlot& operator=( const TimeSlot& ) { value = -1; return *this; }
		};

		//! The calling thread's clone of the active camera, or null.
		inline const ICamera* CameraOverride()
		{
			const ThreadSlots* t = tlsSlots;
			return t ? t->camera : nullptr;
		}
	}
}

//! Forward a const query to the calling thread's clone of this element, if
//! it has one (DL-465).  For classes holding a TimeIndexed::TimeSlot named
//! m_timeSlot; `call` is the member call to make on the clone.
#define RISE_TIME_INDEXED_FORWARD( Type, call ) \
	do { \
		if( const Type* rise_time_clone_ = ::RISE::TimeIndexed::Lookup<Type>( m_timeSlot.value ) ) { \
			return rise_time_clone_->call; \
		} \
	} while( 0 )

#endif
