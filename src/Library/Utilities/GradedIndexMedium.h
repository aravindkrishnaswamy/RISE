//////////////////////////////////////////////////////////////////////
//
//  GradedIndexMedium.h - the interior-segment basic-radiance factor
//    for a medium whose `ior` varies with WORLD POSITION (DL-09).
//
//  Derivation, factor tables and scope:
//    docs/DL09_GRADED_INDEX_INTERIOR_FACTOR.md.
//
//  THE MODEL.  RISE traces straight segments.  Basic radiance L/n^2 is
//  continuous across every interface (debt 30, `RadianceEtaScale`) AND
//  along every straight segment inside a graded medium.  A walk therefore
//  pays, per segment inside a graded medium, in the walk's own order:
//
//      RADIANCE walk (camera-rooted):      (n_start / n_end)^2
//      IMPORTANCE walk (light-rooted):     (n_end / n_start)^2
//      connection eye vertex <-> light:    (n_eye / n_light)^2
//
//  All three are the same per-segment factor (n_cameraside/n_lightside)^2
//  written in each walk's own direction.  An importance walk needs it
//  EXPLICITLY here, unlike at an interface: at an interface the refraction
//  map's Jacobian supplies the n^2 through the light walk's sample density,
//  and a straight graded segment has no direction map to supply it.
//
//  THE MECHANISM.  The IOR stack's TOP entry is the walk's tracked index.
//  Every time a walk reaches a new vertex while its top medium is graded,
//  `Advance` multiplies (top/n(x))^2 (radiance) or (n(x)/top)^2
//  (importance) and re-records top <- n(x).  Consequences:
//
//    * `RadianceEtaScale` at the object's EXIT hit reads the updated top,
//      i.e. the fresh exit-point index: the "exit-read switch" of the
//      DL-09 row is not a separate change here and CANNOT be applied
//      without the interior factor, or vice versa (the two failed rounds).
//    * Factors TELESCOPE.  A vertex the update does not visit (a medium
//      scatter vertex, a site out of scope) breaks no later factor: the
//      next Advance pays (top/n_next)^2 from whatever the stack holds.
//      Only a gather AT such a vertex would see a stale top -- and every
//      gather here uses the same stack top as its starting index, so even
//      that telescopes to the right total (derivation doc §4).
//    * Constant-index media: `GetGradedIORField()` is null, `Advance`
//      returns before evaluating anything, and nothing is multiplied --
//      renders are bit-identical.
//
//  WHERE n IS DEFINED.  Only for an `ior` painter that is a world-position
//  field (`IScalarPainter::IsWorldPositionField`, e.g. a scalar
//  `scalar_painter { expression ... P ... }`).  UV/normal/signal-driven
//  forms have no value at an interior point; such a medium keeps the
//  pre-DL-09 accounting (entry and exit factors only), which nets
//  correctly on every completed through-trip.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GRADED_INDEX_MEDIUM_
#define GRADED_INDEX_MEDIUM_

#include "IORStack.h"
#include "FiniteMath.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
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
			if( !pObj ) {
				return 0;
			}
			const IMaterial* pMat = pObj->GetMaterial();
			return pMat ? pMat->GetGradedIORField() : 0;
		}

		//! The graded ior field of a stack's innermost medium, or null.
		inline const IScalarPainter* TopField( const IORStack& stack )
		{
			return FieldOf( stack.topObject() );
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

		//! Advance a walk to world point `p` (the END of the segment the walk
		//! just traced).  Returns true when the walk's innermost medium is a
		//! graded field, in which case `scale` is the segment's factor and the
		//! stack's top now holds n(p); returns false (scale 1, stack
		//! untouched) otherwise -- including a non-positive or non-finite
		//! painter value, which a scene can author and which must degrade to
		//! "no factor", never to an infinite or zero throughput.
		inline bool Advance( IORStack& stack, const Point3& p, const TransportMode mode, Scalar& scale )
		{
			scale = Scalar( 1 );
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

		//! What a path VERTEX records for later connections: the graded
		//! medium the walk was in when it arrived (null if none) and the
		//! tracked index there (the stack top, which telescopes with the
		//! vertex's throughput -- see the file header).
		inline void RecordVertex( const IORStack& stack, const IObject*& pMedium, Scalar& n )
		{
			if( TopField( stack ) ) {
				pMedium = stack.topObject();
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
			return ConnectionScale( pStack->topObject(), pStack->top(),
				pStack->topObject(), EvalAt( *pField, lightPoint ) );
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
