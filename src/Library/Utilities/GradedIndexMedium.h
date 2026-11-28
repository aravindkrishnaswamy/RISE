//////////////////////////////////////////////////////////////////////
//
//  GradedIndexMedium.h - Interior-segment basic-radiance factor for
//  media whose IOR varies with world position (DL-09; docs/DEBT_LEDGER.md).
//
//  Basic radiance L/n^2 is continuous across interfaces (RadianceEtaScale)
//  and along straight segments inside a graded medium:
//    Radiance walk (camera-rooted):   (n_start / n_end)^2
//    Importance walk (light-rooted):  (n_end / n_start)^2
//    Connection (eye <-> light):      (n_eye / n_light)^2
//  Unlike interfaces, straight segments have no refraction Jacobian, so
//  importance walks apply (n_end / n_start)^2 explicitly.
//
//  The IOR stack top tracks the walk's index; `Advance` multiplies the
//  segment factor and updates top <- n(p). Factors telescope across
//  unvisited vertices, and exit hits read the updated top (exit-read and
//  interior factor are coupled). Applies only to `IScalarPainter`s with
//  `IsWorldPositionField()`; constant-index media are no-ops.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GRADED_INDEX_MEDIUM_
#define GRADED_INDEX_MEDIUM_

#include <atomic>
#include "IORStack.h"
#include "FiniteMath.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	//! Process-wide atomic gate counting live world-position `IScalarPainter`s.
	//! When zero, GradedIndexMedium short-circuits after one relaxed atomic
	//! load. Same RAII/thread-safety pattern as `SurfaceCurvatureDemand`.
	namespace GradedIndexDemand
	{
		inline std::atomic<int>& Counter()
		{
			static std::atomic<int> counter( 0 );
			return counter;
		}

		inline bool Any()
		{
			return Counter().load( std::memory_order_relaxed ) > 0;
		}

		class Registration
		{
		public:
			explicit Registration( bool active = false ) : m_active( active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration( const Registration& other ) : m_active( other.m_active )
			{
				if( m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
			}
			Registration& operator=( const Registration& other )
			{
				if( this != &other ) {
					if( other.m_active ) Counter().fetch_add( 1, std::memory_order_relaxed );
					if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
					m_active = other.m_active;
				}
				return *this;
			}
			~Registration()
			{
				if( m_active ) Counter().fetch_sub( 1, std::memory_order_relaxed );
			}
			bool IsActive() const { return m_active; }

		private:
			bool m_active;
		};
	}

	namespace GradedIndexMedium
	{
		enum TransportMode
		{
			eRadiance,		///< camera-rooted walk
			eImportance		///< light-rooted walk
		};

		//! The graded ior field of the medium `pObj` encloses, or null.
		inline const IScalarPainter* FieldOf( const IObject* pObj )
		{
			if( !pObj || !GradedIndexDemand::Any() ) {
				return 0;
			}
			const IMaterial* pMat = pObj->GetMaterial();
			return pMat ? pMat->GetGradedIORField() : 0;
		}

		//! The graded ior field of a stack's innermost medium, or null.
		//! The graded index is an OPTICAL property paired with the stack's
		//! top() IOR, so every lookup here keys on the optical top object
		//! (opticalTopObject); the enclosure top (topObject) can be an
		//! enclosure-only null boundary that carries no index.
		inline const IScalarPainter* TopField( const IORStack& stack )
		{
			return FieldOf( stack.opticalTopObject() );
		}

		//! The field's value at world point `p`.  The record carries only
		//! the point: IsWorldPositionField() guarantees nothing else is read.
		inline Scalar EvalAt( const IScalarPainter& field, const Point3& p )
		{
			RayIntersectionGeometric rig( Ray( p, Vector3( 0, 0, 1 ) ), nullRasterizerState );
			rig.ptIntersection = p;
			return field.GetValuesAt( rig ).v[0];
		}

		inline bool IsUsableIOR( const Scalar n )
		{
			return n > Scalar( 0 ) && IsFiniteDouble( n );
		}

		//! Re-record every enclosing graded medium below the stack's top at `p`
		//! with no factor, so a nested object's exit uses n(exit) reciprocally
		//! (the entry/exit interface factors telescope the skipped segment).
		//! Callers that price no interfaces (`ShadowSegmentTrack`) must skip
		//! this. Invariant: every key below the top is a real `IObject*`.
		inline void RecordEnclosingAt( IORStack& stack, const Point3& p )
		{
			const std::size_t depth = stack.Depth();
			if( depth < 3 ) {
				return;		// environment + innermost only: nothing enclosing
			}
			for( std::size_t i = 1; i + 1 < depth; i++ ) {
				const IScalarPainter* pField = FieldOf( stack.ObjectAt( i ) );
				if( !pField ) {
					continue;
				}
				const Scalar n = EvalAt( *pField, p );
				if( IsUsableIOR( n ) ) {
					stack.SetIORAt( i, n );
				}
			}
		}

		//! Advance a walk to world point `p`. When the innermost medium is
		//! graded and both indices are finite/positive, updates top <- n(p),
		//! writes the segment factor to `scale`, and returns true; otherwise
		//! returns false with `scale = 1` and stack untouched.
		inline bool Advance( IORStack& stack, const Point3& p, const TransportMode mode, Scalar& scale,
			const bool bRecordEnclosing = true )
		{
			scale = Scalar( 1 );
			if( !GradedIndexDemand::Any() ) {
				return false;
			}
			if( bRecordEnclosing ) {
				RecordEnclosingAt( stack, p );
			}
			const IScalarPainter* pField = TopField( stack );
			if( !pField ) {
				return false;
			}
			const Scalar nPrev = stack.top();
			const Scalar nNew = EvalAt( *pField, p );
			if( !IsUsableIOR( nPrev ) || !IsUsableIOR( nNew ) ) {
				return false;
			}
			if( nNew != nPrev ) {
				const Scalar r = ( mode == eRadiance ) ? ( nPrev / nNew ) : ( nNew / nPrev );
				scale = r * r;
				stack.SetTopIOR( nNew );
			}
			return true;
		}

		//! Re-record the innermost graded medium's index at the walk's
		//! STARTING point (a seed point: camera or light position).  No factor
		//! -- the walk has not moved.  No-op for a constant-index medium.
		inline void RecordAt( IORStack& stack, const Point3& p )
		{
			const IScalarPainter* pField = TopField( stack );
			if( !pField ) {
				return;
			}
			const Scalar n = EvalAt( *pField, p );
			if( IsUsableIOR( n ) ) {
				stack.SetTopIOR( n );
			}
		}

		//! Sync a mid-segment rebuilt stack (e.g. HWSS per-lane replay) to the
		//! walk's tracked top index when both share the same innermost graded
		//! object (wavelength-independent), so the next `Advance` telescopes
		//! from the walk's reference point rather than the rebuild midpoint.
		inline void AdoptTrackedTop( IORStack& rebuilt, const IORStack& walk )
		{
			if( TopField( rebuilt ) && rebuilt.opticalTopObject() == walk.opticalTopObject() &&
				IsUsableIOR( walk.top() ) ) {
				rebuilt.SetTopIOR( walk.top() );
			}
		}

		//! What a path VERTEX records for later connections: the graded
		//! medium the walk was in when it arrived (null if none) and the
		//! tracked index there (the stack top, which telescopes with the
		//! vertex's throughput -- see the file header).
		inline void RecordVertex( const IORStack& stack, const IObject*& pMedium, Scalar& n )
		{
			if( TopField( stack ) ) {
				pMedium = stack.opticalTopObject();
				n = stack.top();
			} else {
				pMedium = 0;
				n = Scalar( 0 );
			}
		}

		//! Connection factor between an eye-side endpoint and a light-side
		//! endpoint that each recorded (medium, n): (n_eye/n_light)^2 when
		//! both lie in the SAME graded medium, else exactly 1.  An unoccluded
		//! connection cannot cross a delta dielectric boundary, so two
		//! endpoints that see each other are in the same medium region.
		inline Scalar ConnectionScale(
			const IObject* pEyeMedium, const Scalar nEye,
			const IObject* pLightMedium, const Scalar nLight )
		{
			if( !pEyeMedium || pEyeMedium != pLightMedium ||
				!IsUsableIOR( nEye ) || !IsUsableIOR( nLight ) || nEye == nLight ) {
				return Scalar( 1 );
			}
			const Scalar r = nEye / nLight;
			return r * r;
		}

		//! NEE connection factor from an eye-side vertex whose walk stack is
		//! `pStack` (its top already advanced to the vertex) to a light
		//! point `lightPoint` in the same medium: (top / n(lightPoint))^2.
		//! Exactly 1 when there is no stack or its top medium is not graded.
		inline Scalar ConnectionScaleToPoint( const IORStack* pStack, const Point3& lightPoint )
		{
			if( !pStack ) {
				return Scalar( 1 );
			}
			const IScalarPainter* pField = TopField( *pStack );
			if( !pField ) {
				return Scalar( 1 );
			}
			return ConnectionScale( pStack->opticalTopObject(), pStack->top(),
				pStack->opticalTopObject(), EvalAt( *pField, lightPoint ) );
		}

		//! Same, from a recorded (medium, n) eye vertex to a light point.
		inline Scalar ConnectionScaleToPoint( const IObject* pEyeMedium, const Scalar nEye, const Point3& lightPoint )
		{
			const IScalarPainter* pField = FieldOf( pEyeMedium );
			if( !pField ) {
				return Scalar( 1 );
			}
			return ConnectionScale( pEyeMedium, nEye, pEyeMedium, EvalAt( *pField, lightPoint ) );
		}

		//! Tracks straight-segment graded-index factors along a hit-by-hit NEE
		//! shadow walk (`RayCaster::WalkShadowSegment`) across transparent or
		//! pass-through boundaries. Mirrors entry/exit stack transitions and
		//! calls `Advance(..., bRecordEnclosing=false)` at each crossing and
		//! at the light; interfaces themselves are not priced (matching the
		//! transparent-shadow straight-propagation approximation).
		class ShadowSegmentTrack
		{
		public:
			explicit ShadowSegmentTrack( const IORStack& shadingStack ) :
			  stack( shadingStack ), scale( 1 ), walked( false )
			{}

			//! The walk crossed the boundary of `pObj` at `p` (entering it
			//! when `bEntering`), with `mediumIOR` the index the walk's own
			//! Fresnel used for that object at `p`.
			void Crossing( const Point3& p, const IObject* pObj, const bool bEntering, const Scalar mediumIOR )
			{
				Scalar s;
				if( Advance( stack, p, eRadiance, s, false ) ) {
					scale *= s;
				}
				if( !pObj ) {
					return;
				}
				stack.SetCurrentObject( pObj );
				if( bEntering ) {
					stack.push( mediumIOR );
				} else if( stack.containsCurrent() ) {
					stack.pop();
				}
			}

			//! The walk reached the light at `lightPoint` unoccluded.
			void Finish( const Point3& lightPoint )
			{
				Scalar s;
				if( Advance( stack, lightPoint, eRadiance, s, false ) ) {
					scale *= s;
				}
				walked = true;
			}

			//! True once Finish ran: the hit-by-hit walk followed the whole
			//! segment and `Scale()` prices it.  False when no walk ran (a
			//! binary shadow test found the segment clear -- then there is
			//! no crossing and `ConnectionScaleToPoint` is the answer).
			bool Walked() const { return walked; }
			Scalar Scale() const { return scale; }

		private:
			IORStack stack;
			Scalar scale;
			bool walked;
		};

		//! Same, in the other direction: from an eye-side POINT (a camera
		//! aperture point) to a recorded (medium, n) light vertex:
		//! (n(eyePoint)/n_light)^2.
		inline Scalar ConnectionScaleFromPoint( const Point3& eyePoint, const IObject* pLightMedium, const Scalar nLight )
		{
			const IScalarPainter* pField = FieldOf( pLightMedium );
			if( !pField ) {
				return Scalar( 1 );
			}
			return ConnectionScale( pLightMedium, EvalAt( *pField, eyePoint ), pLightMedium, nLight );
		}
	}
}

#endif
