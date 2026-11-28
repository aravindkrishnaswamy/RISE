//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSpatialGeometry.cpp - Strict-FP implementation of finite
//    spatial skin domains for P7 nonlocal HyLIoS transport.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#if defined(__FAST_MATH__)
#error "HyLIoSSpatialGeometry.cpp must be compiled without -ffast-math"
#endif

#include "pch.h"
#include "HyLIoSSpatialGeometry.h"
#include <cmath>
#include <stdexcept>

namespace RISE::HyLIoS {

namespace {

void ValidatePositiveFinite( const double val, const char* name )
{
	if( !std::isfinite( val ) || val <= 0.0 ) {
		throw std::invalid_argument( std::string( "HyLIoSSpatialGeometry: " ) + name + " must be finite and positive" );
	}
}

Vector3 TransformLocalDir( const SurfaceFrame3D& frame, const SlabVector& d ) noexcept
{
	return Vector3Ops::Normalize(
		frame.T * Scalar( d.x ) +
		frame.B * Scalar( d.y ) +
		frame.N * Scalar( d.z ) );
}

} // namespace

//////////////////////////////////////////////////////////////////////
// PlanarPolygonDomain
//////////////////////////////////////////////////////////////////////

PlanarPolygonDomain::PlanarPolygonDomain(
	std::vector<std::pair<double, double>> polygonMetres,
	const double totalThicknessMetres,
	const double physicalScale
	) :
  poly_( std::move( polygonMetres ) ),
  totalThicknessMetres_( totalThicknessMetres ),
  physicalScale_( physicalScale )
{
	if( poly_.size() < 3 ) {
		throw std::invalid_argument( "PlanarPolygonDomain: polygon requires at least 3 vertices" );
	}
	for( const auto& v : poly_ ) {
		if( !std::isfinite( v.first ) || !std::isfinite( v.second ) ) {
			throw std::invalid_argument( "PlanarPolygonDomain: non-finite polygon vertex" );
		}
	}
	ValidatePositiveFinite( totalThicknessMetres_, "totalThicknessMetres" );
	ValidatePositiveFinite( physicalScale_, "physicalScale" );
}

PlanarPolygonDomain PlanarPolygonDomain::MakeRectangle(
	const double halfWidthMetres,
	const double halfHeightMetres,
	const double totalThicknessMetres,
	const double physicalScale
	)
{
	ValidatePositiveFinite( halfWidthMetres, "halfWidthMetres" );
	ValidatePositiveFinite( halfHeightMetres, "halfHeightMetres" );
	return PlanarPolygonDomain(
		{
			{ -halfWidthMetres, -halfHeightMetres },
			{  halfWidthMetres, -halfHeightMetres },
			{  halfWidthMetres,  halfHeightMetres },
			{ -halfWidthMetres,  halfHeightMetres }
		},
		totalThicknessMetres,
		physicalScale );
}

bool PlanarPolygonDomain::ContainsLateralPoint( const double x, const double y ) const noexcept
{
	if( !std::isfinite( x ) || !std::isfinite( y ) ) {
		return false;
	}
	// Standard even-odd ray-casting test; boundary points treated as inside.
	bool inside = false;
	const std::size_t n = poly_.size();
	for( std::size_t i = 0, j = n - 1; i < n; j = i++ ) {
		const double xi = poly_[i].first, yi = poly_[i].second;
		const double xj = poly_[j].first, yj = poly_[j].second;
		const bool intersect = ( ( yi > y ) != ( yj > y ) ) &&
			( x < ( xj - xi ) * ( y - yi ) / ( yj - yi ) + xi );
		if( intersect ) {
			inside = !inside;
		}
	}
	return inside;
}

SpatialExitMapping PlanarPolygonDomain::MapExit(
	const SurfaceFrame3D& entryFrame,
	const double entryXMetres,
	const double entryYMetres,
	const double dxMetres,
	const double dyMetres,
	const SlabVector& localDir
	) const noexcept
{
	SpatialExitMapping out;
	const double exitX = entryXMetres + dxMetres;
	const double exitY = entryYMetres + dyMetres;
	out.lateralDisplacementMetres = std::hypot( dxMetres, dyMetres );
	if( !ContainsLateralPoint( exitX, exitY ) ) {
		out.outcome = DomainMapOutcome::DomainBoundaryLoss;
		return out;
	}

	out.outcome = DomainMapOutcome::OuterSurfaceExit;
	const Scalar sx = Scalar( dxMetres * physicalScale_ );
	const Scalar sy = Scalar( dyMetres * physicalScale_ );
	out.worldPos = Point3Ops::mkPoint3( entryFrame.origin, entryFrame.T * sx + entryFrame.B * sy );
	out.worldNormal = entryFrame.N;
	out.worldDir = TransformLocalDir( entryFrame, localDir );
	return out;
}

//////////////////////////////////////////////////////////////////////
// ConcentricSphereDomain
//////////////////////////////////////////////////////////////////////

ConcentricSphereDomain::ConcentricSphereDomain(
	const Point3& centerWorld,
	const double outerRadiusMetres,
	const double totalThicknessMetres,
	const double physicalScale
	) :
  center_( centerWorld ),
  outerRadiusMetres_( outerRadiusMetres ),
  totalThicknessMetres_( totalThicknessMetres ),
  physicalScale_( physicalScale )
{
	ValidatePositiveFinite( outerRadiusMetres_, "outerRadiusMetres" );
	ValidatePositiveFinite( totalThicknessMetres_, "totalThicknessMetres" );
	ValidatePositiveFinite( physicalScale_, "physicalScale" );
	if( outerRadiusMetres_ <= totalThicknessMetres_ ) {
		throw std::invalid_argument( "ConcentricSphereDomain: outerRadiusMetres must strictly exceed totalThicknessMetres" );
	}
}

SpatialExitMapping ConcentricSphereDomain::MapExit(
	const SurfaceFrame3D& entryFrame,
	const double dxMetres,
	const double dyMetres,
	const SlabVector& localDir
	) const noexcept
{
	SpatialExitMapping out;
	const double distM = std::hypot( dxMetres, dyMetres );
	out.lateralDisplacementMetres = distM;
	const double RWorld = outerRadiusMetres_ * physicalScale_;

	// Radial unit vector at entry from sphere center.
	const Vector3 rEntry = Vector3Ops::Normalize( Vector3Ops::mkVector3( entryFrame.origin, center_ ) );
	if( distM <= 1e-15 ) {
		out.outcome = DomainMapOutcome::OuterSurfaceExit;
		out.worldPos = Point3Ops::mkPoint3( center_, rEntry * Scalar( RWorld ) );
		out.worldNormal = rEntry;
		SurfaceFrame3D exitFrame{ out.worldPos, entryFrame.T, entryFrame.B, rEntry };
		out.worldDir = TransformLocalDir( exitFrame, localDir );
		return out;
	}

	// Geodesic arc angle theta = distM / R along tangent unit vector `uTan`.
	const double theta = distM / outerRadiusMetres_;
	const Vector3 uTan = Vector3Ops::Normalize(
		entryFrame.T * Scalar( dxMetres / distM ) +
		entryFrame.B * Scalar( dyMetres / distM ) );
	const double c = std::cos( theta );
	const double s = std::sin( theta );

	const Vector3 rExit = Vector3Ops::Normalize( rEntry * Scalar( c ) + uTan * Scalar( s ) );
	const Vector3 tExit = Vector3Ops::Normalize( -rEntry * Scalar( s ) + uTan * Scalar( c ) );
	const Vector3 bExit = Vector3Ops::Normalize( Vector3Ops::Cross( rExit, tExit ) );

	out.outcome = DomainMapOutcome::OuterSurfaceExit;
	out.worldPos = Point3Ops::mkPoint3( center_, rExit * Scalar( RWorld ) );
	out.worldNormal = rExit;

	// Express localDir in the parallel-transported frame at exit.
	const double dirAlongU = ( localDir.x * dxMetres + localDir.y * dyMetres ) / distM;
	const double dirPerpU  = ( -localDir.x * dyMetres + localDir.y * dxMetres ) / distM;
	out.worldDir = Vector3Ops::Normalize(
		tExit * Scalar( dirAlongU ) +
		bExit * Scalar( dirPerpU ) +
		rExit * Scalar( localDir.z ) );
	return out;
}

//////////////////////////////////////////////////////////////////////
// ConcentricCylinderDomain
//////////////////////////////////////////////////////////////////////

ConcentricCylinderDomain::ConcentricCylinderDomain(
	const Point3& axisOriginWorld,
	const Vector3& axisDirectionWorld,
	const double outerRadiusMetres,
	const double halfLengthMetres,
	const double totalThicknessMetres,
	const double physicalScale
	) :
  axisOrigin_( axisOriginWorld ),
  axisDir_( Vector3Ops::Normalize( axisDirectionWorld ) ),
  outerRadiusMetres_( outerRadiusMetres ),
  halfLengthMetres_( halfLengthMetres ),
  totalThicknessMetres_( totalThicknessMetres ),
  physicalScale_( physicalScale )
{
	ValidatePositiveFinite( outerRadiusMetres_, "outerRadiusMetres" );
	ValidatePositiveFinite( halfLengthMetres_, "halfLengthMetres" );
	ValidatePositiveFinite( totalThicknessMetres_, "totalThicknessMetres" );
	ValidatePositiveFinite( physicalScale_, "physicalScale" );
	if( outerRadiusMetres_ <= totalThicknessMetres_ ) {
		throw std::invalid_argument( "ConcentricCylinderDomain: outerRadiusMetres must strictly exceed totalThicknessMetres" );
	}
}

SpatialExitMapping ConcentricCylinderDomain::MapExit(
	const SurfaceFrame3D& entryFrame,
	const double dxMetres,
	const double dyMetres,
	const SlabVector& localDir
	) const noexcept
{
	SpatialExitMapping out;
	out.lateralDisplacementMetres = std::hypot( dxMetres, dyMetres );

	// Decompose entry position relative to cylinder axis.
	const Vector3 rel = Vector3Ops::mkVector3( entryFrame.origin, axisOrigin_ );
	const double zEntryWorld = static_cast<double>( Vector3Ops::Dot( rel, axisDir_ ) );
	const Vector3 radialEntry = Vector3Ops::Normalize( rel - axisDir_ * Scalar( zEntryWorld ) );
	const Vector3 circEntry = Vector3Ops::Normalize( Vector3Ops::Cross( axisDir_, radialEntry ) );

	// Project tangent-plane displacement onto axial and circumferential axes.
	const Vector3 dispWorld =
		entryFrame.T * Scalar( dxMetres * physicalScale_ ) +
		entryFrame.B * Scalar( dyMetres * physicalScale_ );
	const double dzWorld = static_cast<double>( Vector3Ops::Dot( dispWorld, axisDir_ ) );
	const double dCircWorld = static_cast<double>( Vector3Ops::Dot( dispWorld, circEntry ) );

	const double zExitMetres = ( zEntryWorld + dzWorld ) / physicalScale_;
	if( std::fabs( zExitMetres ) > halfLengthMetres_ ) {
		out.outcome = DomainMapOutcome::DomainBoundaryLoss;
		return out;
	}

	const double dPhi = ( dCircWorld / physicalScale_ ) / outerRadiusMetres_;
	const double c = std::cos( dPhi );
	const double s = std::sin( dPhi );
	const Vector3 radialExit = Vector3Ops::Normalize( radialEntry * Scalar( c ) + circEntry * Scalar( s ) );
	const Vector3 circExit   = Vector3Ops::Normalize( Vector3Ops::Cross( axisDir_, radialExit ) );

	const double RWorld = outerRadiusMetres_ * physicalScale_;
	out.outcome = DomainMapOutcome::OuterSurfaceExit;
	out.worldPos = Point3Ops::mkPoint3(
		Point3Ops::mkPoint3( axisOrigin_, axisDir_ * Scalar( zEntryWorld + dzWorld ) ),
		radialExit * Scalar( RWorld ) );
	out.worldNormal = radialExit;

	// Rotate localDir into (circExit, axisDir_, radialExit) right-handed frame.
	const double tCirc = static_cast<double>( Vector3Ops::Dot( entryFrame.T, circEntry ) );
	const double tAxis = static_cast<double>( Vector3Ops::Dot( entryFrame.T, axisDir_ ) );
	const double bCirc = static_cast<double>( Vector3Ops::Dot( entryFrame.B, circEntry ) );
	const double bAxis = static_cast<double>( Vector3Ops::Dot( entryFrame.B, axisDir_ ) );
	const double dCirc = localDir.x * tCirc + localDir.y * bCirc;
	const double dAxis = localDir.x * tAxis + localDir.y * bAxis;
	out.worldDir = Vector3Ops::Normalize(
		circExit * Scalar( dCirc ) +
		axisDir_ * Scalar( dAxis ) +
		radialExit * Scalar( localDir.z ) );
	return out;
}

//////////////////////////////////////////////////////////////////////
// IntrinsicLayerVolumeDomain
//////////////////////////////////////////////////////////////////////

IntrinsicLayerVolumeDomain::IntrinsicLayerVolumeDomain(
	const IntrinsicLayerChart& chart,
	const double totalThicknessMetres,
	const double physicalScale
	) :
  chart_( chart ),
  totalThicknessMetres_( totalThicknessMetres ),
  physicalScale_( physicalScale )
{
	if( !std::isfinite( chart_.uMin ) || !std::isfinite( chart_.uMax ) || chart_.uMax <= chart_.uMin ||
		!std::isfinite( chart_.vMin ) || !std::isfinite( chart_.vMax ) || chart_.vMax <= chart_.vMin )
	{
		throw std::invalid_argument( "IntrinsicLayerVolumeDomain: chart UV bounds must be finite and non-degenerate" );
	}
	ValidatePositiveFinite( chart_.metresPerU, "metresPerU" );
	ValidatePositiveFinite( chart_.metresPerV, "metresPerV" );
	ValidatePositiveFinite( totalThicknessMetres_, "totalThicknessMetres" );
	ValidatePositiveFinite( physicalScale_, "physicalScale" );
	if( !std::isfinite( chart_.maxCurvatureInvM ) || chart_.maxCurvatureInvM < 0.0 ||
		chart_.maxCurvatureInvM * totalThicknessMetres_ >= 1.0 )
	{
		throw std::invalid_argument( "IntrinsicLayerVolumeDomain: curvature * totalThicknessMetres must be < 1 (non-singular Jacobian)" );
	}
}

bool IntrinsicLayerVolumeDomain::ValidateCoordinate(
	const double u,
	const double v,
	const double depthMetres
	) const noexcept
{
	return std::isfinite( u ) && std::isfinite( v ) && std::isfinite( depthMetres ) &&
		u >= chart_.uMin && u <= chart_.uMax &&
		v >= chart_.vMin && v <= chart_.vMax &&
		depthMetres >= 0.0 && depthMetres <= totalThicknessMetres_;
}

SpatialExitMapping IntrinsicLayerVolumeDomain::MapExit(
	const SurfaceFrame3D& entryFrame,
	const double entryU,
	const double entryV,
	const double dxMetres,
	const double dyMetres,
	const SlabVector& localDir
	) const noexcept
{
	SpatialExitMapping out;
	out.lateralDisplacementMetres = std::hypot( dxMetres, dyMetres );
	const double exitU = entryU + dxMetres / chart_.metresPerU;
	const double exitV = entryV + dyMetres / chart_.metresPerV;
	if( !ValidateCoordinate( exitU, exitV, 0.0 ) ) {
		out.outcome = DomainMapOutcome::DomainBoundaryLoss;
		return out;
	}

	out.outcome = DomainMapOutcome::OuterSurfaceExit;
	out.worldPos = Point3Ops::mkPoint3(
		entryFrame.origin,
		entryFrame.T * Scalar( dxMetres * physicalScale_ ) +
		entryFrame.B * Scalar( dyMetres * physicalScale_ ) );
	out.worldNormal = entryFrame.N;
	out.worldDir = TransformLocalDir( entryFrame, localDir );
	return out;
}

} // namespace RISE::HyLIoS
