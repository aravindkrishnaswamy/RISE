//////////////////////////////////////////////////////////////////////
//
//  SurfaceCurvature.h - Weingarten shape operator, signed mean
//  curvature, and process-wide curvature consumption gate.
//
//  Phase 1 of docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md (§5.1, §5.4).
//  Header-only:
//    - RISE::SurfaceCurvature::MeanCurvatureFromDerivatives()
//    - RISE::SurfaceCurvature::ShapeOperatorFromSecondDerivatives()
//    - RISE::SurfaceCurvature::ScaleHintFromBoundingBox()
//    - RISE::SurfaceCurvatureDemand (atomic demand gate)
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SURFACE_CURVATURE_
#define SURFACE_CURVATURE_

#include <atomic>
#include <cmath>
#include "Math3D/Math3D.h"
#include "FiniteMath.h"
#include "BoundingBox.h"

namespace RISE
{
	namespace SurfaceCurvature
	{
		//! Signed mean curvature H from first/second fundamental forms:
		//!   E = dpdu.dpdu, F = dpdu.dpdv, G = dpdv.dpdv
		//!   e = dndu.dpdu, f = (dndu.dpdv + dndv.dpdu)/2, g = dndv.dpdv
		//!   H = (e*G - 2*f*F + g*E) / (2*(E*G - F*F))
		//!
		//! Sign convention (negated classical II): convex > 0, concave < 0,
		//! flat = 0 (outward sphere of radius r gives H = +1/r).
		//! Orientation comes solely from dndu/dndv, which RISE keeps
		//! sign-paired with vGeomNormal (docs/GEOMETRY_DERIVATIVES.md) and
		//! bump/normal-map invariant; do not orient off ri.vNormal.
		//! Uses a scale-free relative degeneracy gate (det > 1e-12 * E * G).
		//! \return TRUE and writes outH if well-defined and finite; FALSE
		//!         (outH untouched) otherwise.
		inline bool MeanCurvatureFromDerivatives(
			const Vector3& dpdu,
			const Vector3& dpdv,
			const Vector3& dndu,
			const Vector3& dndv,
			Scalar& outH )
		{
			const Scalar E = Vector3Ops::Dot( dpdu, dpdu );
			const Scalar F = Vector3Ops::Dot( dpdu, dpdv );
			const Scalar G = Vector3Ops::Dot( dpdv, dpdv );

			const Scalar det = E * G - F * F;

			// Scale-free degeneracy gate (also rejects zero-length tangents).
			static const Scalar kRelDegenerate = Scalar( 1e-12 );
			if( !( det > Scalar( 0 ) ) || !( det > kRelDegenerate * E * G ) ) {
				return false;
			}

			const Scalar e = Vector3Ops::Dot( dndu, dpdu );
			const Scalar f = Scalar( 0.5 ) * ( Vector3Ops::Dot( dndu, dpdv ) + Vector3Ops::Dot( dndv, dpdu ) );
			const Scalar g = Vector3Ops::Dot( dndv, dpdv );

			const Scalar H = ( e * G - Scalar( 2 ) * f * F + g * E ) / ( Scalar( 2 ) * det );

			if( !RISE::IsFiniteDouble( static_cast<double>( H ) ) ) {
				return false;
			}

			outH = H;
			return true;
		}

		//! Exact normal derivatives (dndu, dndv) from first and second position
		//! derivatives of a parametric surface:
		//!   Q = dpdu x dpdv,  N = Q / |Q|
		//!   dQ/du = d2Pduu x dpdv + dpdu x d2Pduv,  dQ/dv = d2Pduv x dpdv + dpdu x d2Pdvv
		//!   dN/d{u,v} = (dQ/d{u,v} - N (N . dQ/d{u,v})) / |Q|
		//! Uses unflipped N = normalize(dpdu x dpdv); if the caller negates N,
		//! negate both outputs. Same relative degeneracy gate (|Q|^2 > 1e-12*E*G).
		//! \return TRUE and writes outDndu/outDndv if well-defined and finite.
		inline bool ShapeOperatorFromSecondDerivatives(
			const Vector3& dpdu,
			const Vector3& dpdv,
			const Vector3& d2Pduu,
			const Vector3& d2Pduv,
			const Vector3& d2Pdvv,
			Vector3& outDndu,
			Vector3& outDndv )
		{
			const Vector3 Q = Vector3Ops::Cross( dpdu, dpdv );
			const Scalar qlen2 = Vector3Ops::Dot( Q, Q );

			const Scalar E = Vector3Ops::Dot( dpdu, dpdu );
			const Scalar G = Vector3Ops::Dot( dpdv, dpdv );

			// qlen2 == E*G - F*F (Lagrange's identity).
			static const Scalar kRelDegenerate = Scalar( 1e-12 );
			if( !( qlen2 > Scalar( 0 ) ) || !( qlen2 > kRelDegenerate * E * G ) ) {
				return false;
			}

			const Scalar qlen = std::sqrt( qlen2 );
			const Vector3 N = Q * ( Scalar( 1 ) / qlen );

			const Vector3 dQdu = Vector3Ops::Cross( d2Pduu, dpdv ) + Vector3Ops::Cross( dpdu, d2Pduv );
			const Vector3 dQdv = Vector3Ops::Cross( d2Pduv, dpdv ) + Vector3Ops::Cross( dpdu, d2Pdvv );

			const Scalar invQlen = Scalar( 1 ) / qlen;
			const Vector3 dNdu = ( dQdu - N * Vector3Ops::Dot( N, dQdu ) ) * invQlen;
			const Vector3 dNdv = ( dQdv - N * Vector3Ops::Dot( N, dQdv ) ) * invQlen;

			if( !RISE::IsFiniteDouble( static_cast<double>( dNdu.x ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( dNdu.y ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( dNdu.z ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( dNdv.x ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( dNdv.y ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( dNdv.z ) ) ) {
				return false;
			}

			outDndu = dNdu;
			outDndv = dNdv;
			return true;
		}

		//! Characteristic length (`SurfaceDerivativesInfo::scaleHint`) is the
		//! bounding-box diagonal so dimensionless `curv` is consistent across
		//! geometries. Falls back to 1.0 for unbounded/degenerate/overflowing boxes.
		inline Scalar ScaleHintFromBoundingBox( const BoundingBox& bb )
		{
			const Vector3 ext = bb.GetExtents();
			if( !RISE::IsFiniteDouble( static_cast<double>( ext.x ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( ext.y ) ) ||
			    !RISE::IsFiniteDouble( static_cast<double>( ext.z ) ) ) {
				return Scalar( 1 );
			}
			// Guard against DBL_MAX sentinel before squaring.
			static const Scalar kMaxSaneExtent = Scalar( 1e30 );
			if( std::fabs( ext.x ) > kMaxSaneExtent ||
			    std::fabs( ext.y ) > kMaxSaneExtent ||
			    std::fabs( ext.z ) > kMaxSaneExtent ) {
				return Scalar( 1 );
			}
			const Scalar diag = std::sqrt( ext.x*ext.x + ext.y*ext.y + ext.z*ext.z );
			return ( diag > Scalar( 0 ) ) ? diag : Scalar( 1 );
		}
	}

	//! Process-wide atomic consumption gate for per-hit curvature (e.g. SDF
	//! finite-difference curvature; docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
	//! §5.4) and, OR'd with `SurfaceSignalDemand::Any()`, emitter surface
	//! probing (`LightSampler::ProbeEmitterSurface`;
	//! docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5).
	//!
	//! Expression painters reading `curv`/`curvR` hold an active Registration
	//! (mutated only at painter ctor/dtor; render threads load relaxed).
	//! Conservatively process-wide because `IGeometry` is shared across scenes;
	//! false positives cost work only, never correctness.
	namespace SurfaceCurvatureDemand
	{
		//! Single process-wide counter (function-local static for pre-C++17 parity).
		inline std::atomic<int>& Counter()
		{
			static std::atomic<int> counter( 0 );
			return counter;
		}

		//! True if any live consumer requests per-hit curvature (relaxed load).
		inline bool Any()
		{
			return Counter().load( std::memory_order_relaxed ) > 0;
		}

		//! RAII demand reference (`active=false` is inert).
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
}

#endif
