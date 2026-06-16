//////////////////////////////////////////////////////////////////////
//
//  TorusGeometry.cpp - Implementation of the torus class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 12, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TorusGeometry.h"
#include "GeometryUtilities.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

TorusGeometry::TorusGeometry( 
	const Scalar dMajorRadius, 
	const Scalar dMinorRadius 
	) : 
  m_dMajorRadius( dMajorRadius ),
  m_dMinorRadius( dMinorRadius)
{
	RegenerateData();
}

TorusGeometry::~TorusGeometry( )
{
}

bool TorusGeometry::TessellateToMesh(
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

	const Scalar R = m_p0;  // ring-center radius
	const Scalar r = m_p1;  // tube radius

	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v     = Scalar(j) / Scalar(nV);
		const Scalar thetaV = v * TWO_PI;
		const Scalar cosV  = cos(thetaV);
		const Scalar sinV  = sin(thetaV);

		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u      = Scalar(i) / Scalar(nU);
			const Scalar thetaU = u * TWO_PI;
			const Scalar cosU   = cos(thetaU);
			const Scalar sinU   = sin(thetaU);

			const Point3 pos(
				(R + r * cosV) * cosU,
				r * sinV,
				(R + r * cosV) * sinU );

			const Vector3 nrm = Vector3Ops::Normalize( Vector3(
				cosV * cosU,
				sinV,
				cosV * sinU ) );

			vertices.push_back( pos );
			normals.push_back( nrm );
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

void TorusGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool /*bHitFrontFaces*/, const bool /*bHitBackFaces*/, const bool bComputeExitInfo ) const
{
	// Do some inside / outside opto

	HIT	h;
	RayTorusIntersection( ri.ray, h, m_dMajorRadius, m_dMinorRadius, m_sqrP0 );

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );

		// Calculate the normal by taking the gradient (partial derivatives)
		// Graphics Gems II pp. 256
		
		Scalar d = sqrt( ri.ptIntersection.x*ri.ptIntersection.x + ri.ptIntersection.z*ri.ptIntersection.z );
		Scalar f = (2.0 * (d - m_p0)) / (d*m_sqrP1);

		ri.vNormal.x = ri.ptIntersection.x * f;
		ri.vNormal.y = (ri.ptIntersection.y * 2)/(m_sqrP1);
		ri.vNormal.z = ri.ptIntersection.z * f;

		ri.vNormal = Vector3Ops::Normalize(ri.vNormal);
		ri.vGeomNormal = ri.vNormal;	// analytical surface: shading == geometric

		if( bComputeExitInfo && ri.range2 != 0 )
		{
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			Scalar d2 = sqrt( ri.ptExit.x*ri.ptExit.x + ri.ptExit.z*ri.ptExit.z );
			Scalar f2 = (2.0 * (d2 - m_p0)) / (d2*m_sqrP1);

			ri.vNormal2.x = ri.ptExit.x * f2;
			ri.vNormal2.y = (ri.ptExit.y * 2)/(m_sqrP1);
			ri.vNormal2.z = ri.ptExit.z * f2;

			ri.vNormal2 = Vector3Ops::Normalize(ri.vNormal2);
			ri.vGeomNormal2 = ri.vNormal2;	// analytical: shading == geometric
		}

		GeometricUtilities::TorusTextureCoord(
			Vector3( 0.0, 1.0, 0.0 ),
			Vector3( -1.0, 0.0, 0.0 ),
			ri.ptIntersection,
			ri.vNormal,
			ri.ptCoord,
			m_p0,
			m_p1
			);
	}
}

bool TorusGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool /*bHitFrontFaces*/, const bool /*bHitBackFaces*/ ) const
{
	// Do some inside / outside opto

	HIT	h;
	RayTorusIntersection( ray, h, m_dMajorRadius, m_dMinorRadius, m_sqrP0 );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void TorusGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3(0,0,0);
	radius = m_dMajorRadius+m_dMinorRadius;
}

BoundingBox TorusGeometry::GenerateBoundingBox() const
{
	return BoundingBox(
		Point3( -m_dMajorRadius-m_dMinorRadius, -m_dMajorRadius-m_dMinorRadius, -m_dMajorRadius-m_dMinorRadius ),
		Point3( m_dMajorRadius+m_dMinorRadius, m_dMajorRadius+m_dMinorRadius, m_dMajorRadius+m_dMinorRadius ) );
}

void TorusGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// R = m_p0 (center of tube ring), r = m_p1 (tube radius)
	const Scalar R = m_p0;
	const Scalar r = m_p1;

	// Sample u uniformly on [0, 2*PI)
	const Scalar u = TWO_PI * prand.x;

	// Sample v with probability density proportional to the area element
	// (R + r*cos(v)) by inverting its CDF
	//     F(v) = (R*v + r*sin(v)) / (2*PI*R)
	// with a bisection-safeguarded Newton iteration on
	//     g(v) = R*v + r*sin(v) - 2*PI*R*xi,   g'(v) = R + r*cos(v) >= R - r.
	//
	// This replaces a rejection sampler whose retry path re-hashed its
	// [0,1) float candidates with an INTEGER-hash multiplier (2^-32),
	// collapsing every rejected first draw — mean rejection rate
	// r/(R+r), e.g. 27% at minorratio 0.38 — onto the single tube
	// angle v = 2*PI*0.618..., i.e. a point-mass on one circle of the
	// tube.  Estimators dividing by the claimed uniform 1/GetArea()
	// pdf then carried a circle-shaped spatial bias, and integrators
	// that mix light-sampled strategies with different MIS shares
	// disagreed on torus-emitter scenes (VCM floor pool ~0.90x of
	// PT/BDPT, 2026-06-10).  CDF inversion is exact, deterministic
	// from prand.y alone, and needs no retries.  prand.z is unused.
	Scalar v;
	{
		const Scalar xi = prand.y;
		const Scalar target = TWO_PI * R * xi;
		Scalar lo = 0;
		Scalar hi = TWO_PI;
		v = TWO_PI * xi;	// exact for r == 0; good first guess otherwise
		for( int i = 0; i < 32; i++ )
		{
			const Scalar g = R * v + r * sin(v) - target;
			if( g > 0 ) {
				hi = v;
			} else {
				lo = v;
			}
			const Scalar gp = R + r * cos(v);
			Scalar next = ( gp > 0 ) ? ( v - g / gp ) : v;
			if( !( next > lo && next < hi ) ) {
				next = 0.5 * ( lo + hi );	// bisection fallback keeps the bracket
			}
			if( fabs( next - v ) < 1e-12 ) {
				v = next;
				break;
			}
			v = next;
		}
	}

	const Scalar cosU = cos(u);
	const Scalar sinU = sin(u);
	const Scalar cosV = cos(v);
	const Scalar sinV = sin(v);

	if( point ) {
		point->x = (R + r * cosV) * cosU;
		point->y = r * sinV;
		point->z = (R + r * cosV) * sinU;
	}

	if( normal ) {
		normal->x = cosV * cosU;
		normal->y = sinV;
		normal->z = cosV * sinU;
		*normal = Vector3Ops::Normalize(*normal);
	}

	if( coord ) {
		// Compute normal for texture coord lookup (needed even if normal output not requested)
		Vector3 nrm;
		nrm.x = cosV * cosU;
		nrm.y = sinV;
		nrm.z = cosV * sinU;
		nrm = Vector3Ops::Normalize(nrm);

		Point3 pt;
		pt.x = (R + r * cosV) * cosU;
		pt.y = r * sinV;
		pt.z = (R + r * cosV) * sinU;

		GeometricUtilities::TorusTextureCoord(
			Vector3( 0.0, 1.0, 0.0 ),
			Vector3( -1.0, 0.0, 0.0 ),
			pt,
			nrm,
			*coord,
			m_p0,
			m_p1
			);
	}
}

SurfaceDerivatives TorusGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;

	const Scalar R = m_p0;
	const Scalar r = m_p1;

	// Recover surface parameters from object-space point
	// u = azimuthal angle around Y axis
	const Scalar u = atan2( objSpacePoint.z, objSpacePoint.x );

	// d = distance from point to Y axis minus R = signed radial offset
	const Scalar dXZ = sqrt( objSpacePoint.x * objSpacePoint.x + objSpacePoint.z * objSpacePoint.z );
	const Scalar d = dXZ - R;

	// v = angle in the tube cross-section
	const Scalar v = atan2( objSpacePoint.y, d );

	const Scalar cosU = cos(u);
	const Scalar sinU = sin(u);
	const Scalar cosV = cos(v);
	const Scalar sinV = sin(v);
	const Scalar Rr = R + r * cosV;

	// Position partial derivatives.
	//
	// Convention: (dpdu, dpdv, n) must be right-handed per
	// docs/GEOMETRY_DERIVATIVES.md.  At the outer equator (u=0, v=0) the
	// "natural" ordering (dpdu = tangent-to-ring, dpdv = tangent-to-tube)
	// produces (dpdu × dpdv) antiparallel to the outward normal — that's
	// left-handed.  Swap the parametrization (use tube angle as u and
	// ring angle as v) so the frame is right-handed at every point on
	// the torus.
	sd.dpdu = Vector3( -r * sinV * cosU, r * cosV, -r * sinV * sinU );  // tube tangent
	sd.dpdv = Vector3( -Rr * sinU, 0.0, Rr * cosU );                     // ring tangent

	// Normal partial derivatives (swap to match position-derivative swap).
	// Unit normal on torus: N = (cosV*cosU, sinV, cosV*sinU)
	sd.dndu = Vector3( -sinV * cosU, cosV, -sinV * sinU );
	sd.dndv = Vector3( -cosV * sinU, 0.0, cosV * cosU );

	sd.uv = Point2( v, u );  // u,v semantics swapped to match derivative swap
	sd.valid = true;

	return sd;
}

Scalar TorusGeometry::GetArea( ) const
{
	return 4.0 * PI * PI * m_p0 * m_p1;
}

static const unsigned int MAJORRADIUS_ID = 100;
static const unsigned int MINORRADIUS_ID = 101;

IKeyframeParameter* TorusGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "major_radius" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), MAJORRADIUS_ID );
	} else if( name == "minor_radius" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), MINORRADIUS_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void TorusGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case MAJORRADIUS_ID:
		{
			m_dMajorRadius = *(Scalar*)val.getValue();
		}
		break;
	case MINORRADIUS_ID:
		{
			m_dMinorRadius = *(Scalar*)val.getValue();
		}
		break;

	}
}

void TorusGeometry::RegenerateData( )
{
	// Standard torus parameterisation matching the scene spec:
	//   majorradius = ring radius (distance from torus axis to ring centre)
	//   minorradius = tube radius
	//   Implicit:  (sqrt(x² + z²) − R)² + y² = r²
	//
	// Prior to 2026-04, this code computed m_p0 = (R + r)/2 and
	// m_p1 = (R − r)/2 — representing a torus whose OUTER boundary sat
	// at d=R and INNER boundary at d=r, with ring radius (R+r)/2.  That
	// model is internally consistent (solver + normal use matching m_p0,
	// m_sqrP1) but it does NOT match either (a) the scene-file intent
	// stated in the comments ("ring radius 0.3, tube radius 0.075") or
	// (b) TessellateToMesh, which uses m_p0 as the ring radius and m_p1
	// as the tube radius when placing vertices.  The disagreement went
	// unnoticed because both SMS and VCM rendered *some* torus — just
	// a smaller, fatter one than the user asked for.
	m_p0 = m_dMajorRadius;
	m_p1 = m_dMinorRadius;
	m_sqrP0 = m_p0 * m_p0;
	m_sqrP1 = m_p1 * m_p1;
}
