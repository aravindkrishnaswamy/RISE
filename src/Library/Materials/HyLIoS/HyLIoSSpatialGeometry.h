//////////////////////////////////////////////////////////////////////
//
//  HyLIoSSpatialGeometry.h - Finite spatial skin domains for P7
//    nonlocal HyLIoS transport:
//      1. PlanarPolygonDomain: extruded 2D polygon footprint with
//         explicit absorbing side walls (`DomainBoundaryLoss`).
//      2. ConcentricSphereDomain / ConcentricCylinderDomain: analytic
//         concentric curved bodies (`R_outer > H_total`).
//      3. IntrinsicLayerVolumeDomain: single outer surface + validated
//         intrinsic `(u, v, depth_m)` layer coordinates.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_HYLIOS_SPATIAL_GEOMETRY_H
#define RISE_HYLIOS_SPATIAL_GEOMETRY_H

#include "HyLIoSSlabTransport.h"
#include "../../Utilities/Math3D/Math3D.h"
#include <string>
#include <utility>
#include <vector>

namespace RISE::HyLIoS {

enum class SpatialDomainKind {
	PlanarPolygon,
	ConcentricSphere,
	ConcentricCylinder,
	IntrinsicLayerVolume
};

enum class DomainMapOutcome {
	OuterSurfaceExit,    ///< Valid exit through the outer stratum corneum boundary
	DomainBoundaryLoss   ///< Exited through an explicit absorbing lateral side wall
};

struct SpatialExitMapping {
	DomainMapOutcome outcome = DomainMapOutcome::DomainBoundaryLoss;
	Point3  worldPos;
	Vector3 worldNormal;
	Vector3 worldDir;
	double  lateralDisplacementMetres = 0.0;
};

struct SurfaceFrame3D {
	Point3  origin;
	Vector3 T;
	Vector3 B;
	Vector3 N;
};

//! 1. Planar extruded 2D polygon footprint with explicit absorbing side walls.
class PlanarPolygonDomain final {
public:
	//! `polygonMetres`: simple 2D polygon vertices `(x_m, y_m)` in the local tangent plane.
	//! `totalThicknessMetres`: total 6-layer skin thickness `H_total > 0`.
	//! `physicalScale`: scene units per metre (`> 0`).
	PlanarPolygonDomain(
		std::vector<std::pair<double, double>> polygonMetres,
		double totalThicknessMetres,
		double physicalScale = 1.0
		);

	//! Convenience rectangular footprint `[-halfW, +halfW] x [-halfH, +halfH]` in metres.
	static PlanarPolygonDomain MakeRectangle(
		double halfWidthMetres,
		double halfHeightMetres,
		double totalThicknessMetres,
		double physicalScale = 1.0
		);

	bool ContainsLateralPoint( double xMetres, double yMetres ) const noexcept;

	//! Maps a local slab exit `(dxMetres, dyMetres)` and outgoing local direction `localDir`
	//! (+Z outward) back to world space relative to the entry frame.
	//! If `(entryX + dxMetres, entryY + dyMetres)` falls outside the polygon, returns
	//! `DomainBoundaryLoss` (never a bogus outer surface exit).
	SpatialExitMapping MapExit(
		const SurfaceFrame3D& entryFrame,
		double entryXMetres,
		double entryYMetres,
		double dxMetres,
		double dyMetres,
		const SlabVector& localDir
		) const noexcept;

	double TotalThicknessMetres() const noexcept { return totalThicknessMetres_; }
	double PhysicalScale() const noexcept { return physicalScale_; }

private:
	std::vector<std::pair<double, double>> poly_;
	double totalThicknessMetres_;
	double physicalScale_;
};

//! 2a. Analytic concentric layered sphere (`R_outer > H_total`).
class ConcentricSphereDomain final {
public:
	ConcentricSphereDomain(
		const Point3& centerWorld,
		double outerRadiusMetres,
		double totalThicknessMetres,
		double physicalScale = 1.0
		);

	//! Maps lateral tangent-plane displacement `(dxMetres, dyMetres)` along the sphere's
	//! geodesic arc from `entryFrame` to an exact outer-surface exit `(P_exit, N_exit, D_exit)`.
	SpatialExitMapping MapExit(
		const SurfaceFrame3D& entryFrame,
		double dxMetres,
		double dyMetres,
		const SlabVector& localDir
		) const noexcept;

	double OuterRadiusMetres() const noexcept { return outerRadiusMetres_; }
	double TotalThicknessMetres() const noexcept { return totalThicknessMetres_; }
	double PhysicalScale() const noexcept { return physicalScale_; }

private:
	Point3 center_;
	double outerRadiusMetres_;
	double totalThicknessMetres_;
	double physicalScale_;
};

//! 2b. Analytic concentric layered cylinder (`R_outer > H_total`, axis along `axisUnit`).
class ConcentricCylinderDomain final {
public:
	ConcentricCylinderDomain(
		const Point3& axisOriginWorld,
		const Vector3& axisDirectionWorld,
		double outerRadiusMetres,
		double halfLengthMetres,
		double totalThicknessMetres,
		double physicalScale = 1.0
		);

	//! Maps lateral displacement onto the cylinder outer surface; exceeding `[-halfLength, +halfLength]`
	//! along the cylinder axis triggers `DomainBoundaryLoss` (absorbing end caps).
	SpatialExitMapping MapExit(
		const SurfaceFrame3D& entryFrame,
		double dxMetres,
		double dyMetres,
		const SlabVector& localDir
		) const noexcept;

	double OuterRadiusMetres() const noexcept { return outerRadiusMetres_; }
	double HalfLengthMetres() const noexcept { return halfLengthMetres_; }
	double TotalThicknessMetres() const noexcept { return totalThicknessMetres_; }

private:
	Point3  axisOrigin_;
	Vector3 axisDir_;
	double  outerRadiusMetres_;
	double  halfLengthMetres_;
	double  totalThicknessMetres_;
	double  physicalScale_;
};

//! 3. Validated intrinsic layer-coordinate domain `(u, v, depth_m)` over a single outer surface.
struct IntrinsicLayerChart {
	double uMin = 0.0, uMax = 1.0;
	double vMin = 0.0, vMax = 1.0;
	double metresPerU = 0.1;     ///< Local metric scale |dP/du| in metres (> 0)
	double metresPerV = 0.1;     ///< Local metric scale |dP/dv| in metres (> 0)
	double maxCurvatureInvM = 0.0; ///< Upper bound on |kappa| in m^-1; requires maxCurvature * H_total < 1
};

class IntrinsicLayerVolumeDomain final {
public:
	IntrinsicLayerVolumeDomain(
		const IntrinsicLayerChart& chart,
		double totalThicknessMetres,
		double physicalScale = 1.0
		);

	//! Validates `(u, v, depthMetres)` lies inside the chart and `[0, H_total]`.
	bool ValidateCoordinate( double u, double v, double depthMetres ) const noexcept;

	//! Maps lateral displacement `(dxMetres, dyMetres)` in `(u, v)` chart coordinates.
	//! Exits outside `[uMin, uMax] x [vMin, vMax]` report `DomainBoundaryLoss`.
	SpatialExitMapping MapExit(
		const SurfaceFrame3D& entryFrame,
		double entryU,
		double entryV,
		double dxMetres,
		double dyMetres,
		const SlabVector& localDir
		) const noexcept;

	const IntrinsicLayerChart& Chart() const noexcept { return chart_; }

private:
	IntrinsicLayerChart chart_;
	double totalThicknessMetres_;
	double physicalScale_;
};

} // namespace RISE::HyLIoS

#endif // RISE_HYLIOS_SPATIAL_GEOMETRY_H
