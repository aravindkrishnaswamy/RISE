//////////////////////////////////////////////////////////////////////
//
//  SphereGeometry.cpp - Implementation of the sphere class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SphereGeometry.h"
#include "GeometryUtilities.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/SurfaceCurvature.h"

using namespace RISE;
using namespace RISE::Implementation;

SphereGeometry::SphereGeometry( Scalar dRadius ) : 
  m_dRadius( dRadius ), m_dSqrRadius( dRadius*dRadius), m_dOVRadius( 1.0 / dRadius )
{
}

SphereGeometry::~SphereGeometry( )
{
}

bool SphereGeometry::TessellateToMesh(
	IndexTriangleListType& tris,
	VerticesListType&      vertices,
	NormalsListType&       normals,
	TexCoordsListType&     coords,
	const unsigned int     detail ) const
{
	if( detail < 3 ) {
		return false;
	}

	const unsigned int nU = detail;
	const unsigned int nV = detail;
	const unsigned int baseIdx = static_cast<unsigned int>( vertices.size() );
	const unsigned int rowStride = nU + 1;

	// Parameterization matches GeometricUtilities::SphereTextureCoord(vUp=Y, vForward=-X):
	//   phi   = v * PI   (0 at north pole (+Y), PI at south pole (-Y))
	//   theta = u * 2*PI (0 at -X, going through +Z, +X, -Z)
	// Position on unit sphere:
	//   dir = (-sin(phi)*cos(theta), cos(phi), sin(phi)*sin(theta))
	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v      = Scalar(j) / Scalar(nV);
		const Scalar phi    = v * PI;
		const Scalar sinPhi = sin(phi);
		const Scalar cosPhi = cos(phi);

		// At the poles (j=0 north, j=nV south) every i-vertex collapses to the same
		// 3D position with the same normal.  If we let u vary across pole vertices,
		// a non-constant displacement function evaluates to different heights per
		// pole vertex and the cap triangles fan out into radial spikes.  Collapse
		// u to a canonical value at the poles so every pole vertex gets the same
		// displacement and the cap stays closed.
		const bool atPole = (j == 0) || (j == nV);

		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u        = atPole ? 0.0 : Scalar(i) / Scalar(nU);
			const Scalar theta    = u * TWO_PI;
			const Scalar sinTheta = sin(theta);
			const Scalar cosTheta = cos(theta);

			const Vector3 dir( -sinPhi * cosTheta, cosPhi, sinPhi * sinTheta );
			const Point3  pos( m_dRadius * dir.x, m_dRadius * dir.y, m_dRadius * dir.z );

			vertices.push_back( pos );
			normals.push_back( dir );
			coords.push_back( Point2( u, v ) );
		}
	}

	for( unsigned int j = 0; j < nV; j++ ) {
		for( unsigned int i = 0; i < nU; i++ ) {
			const unsigned int a = baseIdx + j     * rowStride + i;
			const unsigned int b = baseIdx + j     * rowStride + (i + 1);
			const unsigned int c = baseIdx + (j+1) * rowStride + i;
			const unsigned int d = baseIdx + (j+1) * rowStride + (i + 1);

			tris.push_back( MakeIndexedTriangleSameIdx( a, c, b ) );
			tris.push_back( MakeIndexedTriangleSameIdx( b, c, d ) );
		}
	}

	return true;
}

void SphereGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInSphere = GeometricUtilities::IsPointInsideSphere( ri.ray.origin, m_dRadius, Point3(0,0,0) );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInSphere ) {
		return;
	}

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInSphere ) {
		return;
	}

	HIT	h;
	RaySphereIntersection( ri.ray, h, m_dRadius );

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );
		ri.vNormal = Vector3Ops::Normalize(Vector3Ops::mkVector3( ri.ptIntersection, Point3(0,0,0) ));
		ri.vGeomNormal = ri.vNormal;	// analytical surface: shading == geometric

		if( bComputeExitInfo ) {
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			ri.vNormal2 = Vector3Ops::Normalize(Vector3Ops::mkVector3( ri.ptExit, Point3(0,0,0) ));
			ri.vGeomNormal2 = ri.vNormal2;	// analytical: shading == geometric
		}

		// Calculate UV co-ordinates using spherical mapping
		GeometricUtilities::SphereTextureCoord( 
			Vector3( 0.0, 1.0, 0.0 ),
			Vector3( -1.0, 0.0, 0.0 ), 
			ri.vNormal, ri.ptCoord );

		// PHASE-1 GEOMETRY-DERIVED SHADING SIGNALS
		// (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 5.4).  Publish the genuine
		// closed-form Weingarten map this primitive already knows how to
		// compute, so consumers reading `ri.derivatives` -- the expression VM's
		// `curv`, NormalMap's UV-aligned tangent frame, SMS's Newton Jacobian --
		// get an EXACT curvature/tangent frame here instead of the honest zero
		// (or the ONB / finite-difference fallback) an unpopulated record used
		// to force.  UNGATED: this is a handful of trig calls on data the hit
		// already produced, closed-form and exact.  Contrast the SDF family's
		// direct curvature, which costs ~18 extra field evaluations per hit and
		// IS gated on SurfaceCurvatureDemand.
		//
		// `scaleHint` (the dimensionless `curv`'s normalizer) is the ONE part
		// that IS gated -- nothing but `curv` reads it, and
		// Object::IntersectRay folds the world scale into it afterward.
		{
			const SurfaceDerivatives sd = ComputeSurfaceDerivatives( ri.ptIntersection, ri.vNormal );
			if( sd.valid ) {
				ri.derivatives.dpdu = sd.dpdu;
				ri.derivatives.dpdv = sd.dpdv;
				ri.derivatives.dndu = sd.dndu;
				ri.derivatives.dndv = sd.dndv;
				ri.derivatives.valid = true;
				// The texcoord chart map travels with the derivatives: the
				// footprint solve needs to know how this primitive's own (u, v)
				// parameters relate to the (s, t) stamped into ptCoord above.
				// Without it SolveFootprintUV publishes radians where the
				// texture sampler expects [0, 1] -- see the chart-map comment on
				// SurfaceDerivativesInfo.
				ri.derivatives.dsdu = sd.dsdu;
				ri.derivatives.dsdv = sd.dsdv;
				ri.derivatives.dtdu = sd.dtdu;
				ri.derivatives.dtdv = sd.dtdv;
				ri.derivatives.texChartValid = sd.texChartValid;
				if( SurfaceCurvatureDemand::Any() ) {
					ri.derivatives.scaleHint = SurfaceCurvature::ScaleHintFromBoundingBox( GenerateBoundingBox() );
				}
				// docs/CLOTH_FABRIC_DESIGN.md 9.1: this dpdu is a genuine
				// surface parameterization with no discontinuous fallback
				// branch (unlike the mesh sites), so hand it to the shading
				// ONB unconditionally on sd.valid.
				ri.bShadingTangentFromGeometry = true;
				ri.vShadingTangent             = sd.dpdu;	// object space
				ri.bHasShadingTangent          = true;
			}
		}
	}
}

bool SphereGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInSphere = GeometricUtilities::IsPointInsideSphere( ray.origin, m_dRadius, Point3(0,0,0) );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInSphere ) {
		return false;
	}

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInSphere ) {
		return false;
	}

	HIT	h;
	RaySphereIntersection( ray, h, m_dRadius );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void SphereGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3(0,0,0);
	radius = m_dRadius;
}

BoundingBox SphereGeometry::GenerateBoundingBox() const
{
	return BoundingBox( 
		Point3( -m_dRadius, -m_dRadius, -m_dRadius ),
		Point3( m_dRadius, m_dRadius, m_dRadius ) );
}

void SphereGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	Point3 pt = GeometricUtilities::PointOnSphere( Point3(0,0,0), m_dRadius, Point2( prand.x, prand.y ) );

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		*normal = Vector3Ops::Normalize(Vector3Ops::mkVector3(pt,Point3(0,0,0)));
	}

	if( coord ) {
		if( normal ) {
			GeometricUtilities::SphereTextureCoord( Vector3( 0.0, 1.0, 0.0 ), Vector3( -1.0, 0.0, 0.0 ), *normal, *coord );
		} else {
			GeometricUtilities::SphereTextureCoord( Vector3( 0.0, m_dOVRadius, 0.0 ), Vector3( -m_dOVRadius, 0.0, 0.0 ), pt, *coord );
		}
	}
}

Scalar SphereGeometry::GetArea( ) const
{
	return (FOUR_PI * m_dSqrRadius);
}

SurfaceDerivatives SphereGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;

	const Scalar r = m_dRadius;

	// Recover spherical coordinates
	// phi = azimuthal angle (around Y axis, in XZ plane)
	const Scalar phi = atan2( objSpacePoint.z, objSpacePoint.x );

	// theta = polar angle from +Y axis
	const Scalar cosTheta = r > NEARZERO ? objSpacePoint.y / r : 0.0;
	const Scalar clampedCosTheta = cosTheta > 1.0 ? 1.0 : (cosTheta < -1.0 ? -1.0 : cosTheta);
	const Scalar theta = acos( clampedCosTheta );
	const Scalar sinTheta = sin( theta );

	const Scalar cosPhi = cos(phi);
	const Scalar sinPhi = sin(phi);

	// Position partial derivatives
	// dpdu = dP/dphi, dpdv = dP/dtheta
	sd.dpdu = Vector3( -r * sinTheta * sinPhi, 0.0, r * sinTheta * cosPhi );
	sd.dpdv = Vector3( r * clampedCosTheta * cosPhi, -r * sinTheta, r * clampedCosTheta * sinPhi );

	// Normal = (x,y,z)/r, so dN/d* = (dP/d*)/r
	sd.dndu = Vector3( -sinTheta * sinPhi, 0.0, sinTheta * cosPhi );
	sd.dndv = Vector3( clampedCosTheta * cosPhi, -sinTheta, clampedCosTheta * sinPhi );

	sd.uv = Point2( phi, theta );
	sd.valid = true;

	// THE CHART MAP (docs/GEOMETRY_DERIVATIVES.md "The texcoord chart
	// map").  `IntersectRay` stamps ptCoord from
	// GeometricUtilities::SphereTextureCoord( vUp = +Y, vForward = -X ),
	// whose algebra reduces -- on BOTH sides of its N.z sign branch --
	// to
	//     s = (PI - phi) / (2*PI),    t = theta / PI
	// with the same phi = atan2(z, x) and theta = acos(y/r) recovered
	// above.  (The branch computes acos(-cos phi) = PI - |phi| and then
	// mirrors for phi < 0; both cases collapse to the single affine
	// expression, which is why there is no seam-side case here.)  The
	// azimuth therefore runs BACKWARDS relative to the derivative
	// parameter -- s = 0 sits at -X and increases toward +Z -- so dsdu
	// is negative, and 2*pi / pi are the scale factors that were
	// missing when the solve published radians as though they were
	// [0, 1] texture coordinates.
	sd.dsdu = -1.0 / TWO_PI;
	sd.dsdv = 0.0;
	sd.dtdu = 0.0;
	sd.dtdv = 1.0 / PI;
	sd.texChartValid = true;

	return sd;
}

static const unsigned int RADIUS_ID = 100;

IKeyframeParameter* SphereGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "radius" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), RADIUS_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void SphereGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case RADIUS_ID:
		{
			m_dRadius = *(Scalar*)val.getValue();
			m_dOVRadius = (m_dRadius != 0) ? 1.0/m_dRadius : 0;
			m_dSqrRadius = m_dRadius*m_dRadius;
		}
		break;
	}
}

void SphereGeometry::RegenerateData( )
{
}

//! IGeometry::DistanceToSurface -- EXACT outside, and 0 everywhere inside.
//!
//! The sphere is centred at the object-space origin, so the signed field is
//! `|p| - R` and the exact distance to the SURFACE from OUTSIDE is that
//! value.  It is CLAMPED AT ZERO rather than passed through `fabs`, and the
//! difference is the signal's contract, not a detail: `proximity` reads
//! INTERPENETRATION AS CONTACT (design §2), so every point inside the solid
//! answers 0 -- `fabs` would instead report the depth, which for a point
//! near the centre of a large neighbour is a large number and would read as
//! "nothing nearby" at the very place contact is deepest.  The clamp costs
//! nothing: the signed field's own sign is the inside test.
bool SphereGeometry::DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const
{
	(void)maxDistObject;
	const Scalar r = Vector3Ops::Magnitude( Vector3( ptObject.x, ptObject.y, ptObject.z ) );
	// CLAMPED AT ZERO, not fabs: a point INSIDE the sphere reads 0, because
	// the signal's contract is "interpenetration IS contact"
	// (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 2), not "how far inside".  The
	// signed field's own sign is the inside test, so this costs nothing.
	const Scalar signed_ = r - m_dRadius;
	const Scalar d = ( signed_ > Scalar( 0 ) ) ? signed_ : Scalar( 0 );
	if( !RISE::IsFiniteDouble( (double)d ) ) {
		return false;
	}
	outDist = d;
	return true;
}

//! IGeometry::SignedDistanceLower -- the SAME field, UNCLAMPED.
//!
//! `|p| - R` is the exact signed distance to the sphere on both sides, so
//! this is the one place where "lower bound" and "the distance" coincide,
//! and the exactness flag says so -- which is what lets a CSG composite's
//! BOUNDARY ARM admit a landing exactly on a sphere operand's surface
//! (docs/CROSS_OBJECT_PROXIMITY_DESIGN.md §5.6).
//!
//! A ZERO OR NEGATIVE RADIUS REFUSES rather than answering `|p|` with the
//! flag set: a degenerate operand's closure is lower-dimensional, the
//! composite renders nothing there, and the boundary arm's closure
//! argument needs interior points near the landing.
bool SphereGeometry::SignedDistanceLower( const Point3& ptObject, const Scalar maxDistObject,
	Scalar& outSigned, bool& outExact ) const
{
	(void)maxDistObject;
	outExact = false;
	if( !( m_dRadius > Scalar( 0 ) ) ) {
		return false;
	}
	const Scalar r = Vector3Ops::Magnitude( Vector3( ptObject.x, ptObject.y, ptObject.z ) );
	const Scalar sgn = r - m_dRadius;
	if( !RISE::IsFiniteDouble( (double)sgn ) ) {
		return false;
	}
	outSigned = sgn;
	outExact  = true;
	return true;
}
