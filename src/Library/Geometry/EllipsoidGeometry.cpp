//////////////////////////////////////////////////////////////////////
//
//  EllipsoidGeometry.cpp - Implementation of the ellipsoid class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 7, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "EllipsoidGeometry.h"
#include "GeometryUtilities.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

EllipsoidGeometry::EllipsoidGeometry( const Vector3& vRadius ) :
  m_vRadius( vRadius )
{
	RegenerateData();
}

void EllipsoidGeometry::EllipsoidUVFromPosition( const Point3& pt, Point2& uv ) const
{
	const Scalar a = m_vRadius.x * 0.5;
	const Scalar b = m_vRadius.y * 0.5;
	const Scalar c = m_vRadius.z * 0.5;

	const Scalar yn = (b > NEARZERO) ? pt.y / b : 0.0;
	const Scalar clampedYn = yn > 1.0 ? 1.0 : (yn < -1.0 ? -1.0 : yn);
	const Scalar phi = acos( clampedYn );

	const Scalar xn = (a > NEARZERO) ? pt.x / a : 0.0;
	const Scalar zn = (c > NEARZERO) ? pt.z / c : 0.0;
	Scalar theta = atan2( zn, -xn );
	if( theta < 0.0 ) {
		theta += TWO_PI;
	}

	uv.x = theta / TWO_PI;
	uv.y = phi / PI;
}

EllipsoidGeometry::~EllipsoidGeometry( )
{
}

bool EllipsoidGeometry::TessellateToMesh(
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

	// Semi-axes (m_vRadius stores diameters per the convention used in Q and UniformRandomPoint)
	const Scalar a = m_vRadius.x * 0.5;
	const Scalar b = m_vRadius.y * 0.5;
	const Scalar c = m_vRadius.z * 0.5;

	const Scalar ooA2 = (a > NEARZERO) ? 1.0 / (a*a) : 0.0;
	const Scalar ooB2 = (b > NEARZERO) ? 1.0 / (b*b) : 0.0;
	const Scalar ooC2 = (c > NEARZERO) ? 1.0 / (c*c) : 0.0;

	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v      = Scalar(j) / Scalar(nV);
		const Scalar phi    = v * PI;
		const Scalar sinPhi = sin(phi);
		const Scalar cosPhi = cos(phi);

		// Collapse pole u so every pole vertex gets the same displacement height
		// (see SphereGeometry::TessellateToMesh for the reasoning).
		const bool atPole = (j == 0) || (j == nV);

		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u        = atPole ? 0.0 : Scalar(i) / Scalar(nU);
			const Scalar theta    = u * TWO_PI;
			const Scalar sinTheta = sin(theta);
			const Scalar cosTheta = cos(theta);

			const Point3 pos(
				a * -sinPhi * cosTheta,
				b * cosPhi,
				c * sinPhi * sinTheta );

			// Gradient-based normal: (x/a^2, y/b^2, z/c^2) normalized
			const Vector3 nrm = Vector3Ops::Normalize( Vector3(
				pos.x * ooA2,
				pos.y * ooB2,
				pos.z * ooC2 ) );

			vertices.push_back( pos );
			normals.push_back( nrm );
			coords.push_back( Point2( u, v ) );
		}
	}

	for( unsigned int j = 0; j < nV; j++ ) {
		for( unsigned int i = 0; i < nU; i++ ) {
			const unsigned int a_idx = baseIdx + j     * rowStride + i;
			const unsigned int b_idx = baseIdx + j     * rowStride + (i + 1);
			const unsigned int c_idx = baseIdx + (j+1) * rowStride + i;
			const unsigned int d_idx = baseIdx + (j+1) * rowStride + (i + 1);

			tris.push_back( MakeIndexedTriangleSameIdx( a_idx, c_idx, b_idx ) );
			tris.push_back( MakeIndexedTriangleSameIdx( b_idx, c_idx, d_idx ) );
		}
	}

	return true;
}

void EllipsoidGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	HIT	h;
	RayQuadricIntersection( ri.ray, h, Point3(0,0,0), Q );

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );
		ri.vNormal = Vector3Ops::Normalize(
			Vector3( Q._00*ri.ptIntersection.x, 
					 Q._11*ri.ptIntersection.y,
					 Q._22*ri.ptIntersection.z )
			);

		if( bComputeExitInfo ) {
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			ri.vNormal2 = Vector3Ops::Normalize(
				Vector3( Q._00*ri.ptExit.x,
						 Q._11*ri.ptExit.y,
						 Q._22*ri.ptExit.z )
				);
		}

		// Position-based inverse parameterization that matches TessellateToMesh:
		//   pos = (a*-sin(phi)*cos(theta), b*cos(phi), c*sin(phi)*sin(theta))
		// so phi = acos(P_y/b) and theta = atan2(P_z/c, -P_x/a).
		//
		// Using SphereTextureCoord on the gradient-normal collapses to v ≈ 0.5
		// for unequal semi-axes (the gradient-derived "y component" lives in a
		// tiny band around zero) AND derives (u, v) from the gradient direction
		// rather than the position, so the result does not match
		// TessellateToMesh's parameterization even when the magnitudes are
		// reasonable.  Compute (u, v) directly from the position instead.
		EllipsoidUVFromPosition( ri.ptIntersection, ri.ptCoord );
	}
}

bool EllipsoidGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	HIT	h;
	RayQuadricIntersection( ray, h, Point3(0,0,0), Q );

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void EllipsoidGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3(0,0,0);
	radius = r_max( r_max(m_vRadius.x, m_vRadius.y), m_vRadius.z );
}

BoundingBox EllipsoidGeometry::GenerateBoundingBox() const
{
	return BoundingBox( 
		Point3( -m_vRadius.x, -m_vRadius.y, -m_vRadius.z ),
		Point3( m_vRadius.x, m_vRadius.y, m_vRadius.z ) );
}

void EllipsoidGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// Semi-axes (m_vRadius stores diameters, Q uses half of those)
	const Scalar a = m_vRadius.x * 0.5;
	const Scalar b = m_vRadius.y * 0.5;
	const Scalar c = m_vRadius.z * 0.5;

	// Use the precomputed marginal CDF to sample theta with area-uniform distribution.
	// Binary search for the CDF bin containing prand.x.
	const Scalar* it = std::lower_bound( m_thetaCDF + 1, m_thetaCDF + THETA_CDF_SIZE + 1, prand.x );
	int idx = int(it - m_thetaCDF) - 1;
	if( idx < 0 ) idx = 0;
	if( idx >= (int)THETA_CDF_SIZE ) idx = THETA_CDF_SIZE - 1;

	// Linearly interpolate within the bin to get theta
	const Scalar binWidth = m_thetaCDF[idx + 1] - m_thetaCDF[idx];
	const Scalar t = (binWidth > 0.0) ? (prand.x - m_thetaCDF[idx]) / binWidth : 0.5;
	const Scalar theta = PI * (idx + t) / Scalar(THETA_CDF_SIZE);

	const Scalar sinTheta = sin(theta);
	const Scalar cosTheta = cos(theta);
	const Scalar phi = TWO_PI * prand.y;

	// Point on the ellipsoid surface using the correct semi-axes
	const Point3 pt( a * sinTheta * cos(phi),
					  b * sinTheta * sin(phi),
					  c * cosTheta );

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		// Gradient of the implicit form x^2/a^2 + y^2/b^2 + z^2/c^2 = 1
		*normal = Vector3Ops::Normalize(
					Vector3(	Q._00*pt.x,
								Q._11*pt.y,
								Q._22*pt.z )
					);
	}

	if( coord ) {
		// Match the position-based parameterization used in IntersectRay and
		// TessellateToMesh.  The previous SphereTextureCoord call passed
		// m_OVmaxRadius-scaled "axis" vectors that were not unit vectors, so
		// the dot products with the gradient-normal were tiny and v collapsed
		// to ≈ 0.5 for every random sample.
		EllipsoidUVFromPosition( pt, *coord );
	}
}

SurfaceDerivatives EllipsoidGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;

	// Semi-axes: m_vRadius stores diameters
	const Scalar a = m_vRadius.x * 0.5;
	const Scalar b = m_vRadius.y * 0.5;
	const Scalar c = m_vRadius.z * 0.5;

	// Recover surface parameters from object-space point
	// P(theta,phi) = (a*sin(theta)*cos(phi), b*cos(theta), c*sin(theta)*sin(phi))
	// phi = atan2(z/c, x/a)
	const Scalar xn = (a > NEARZERO) ? objSpacePoint.x / a : 0.0;
	const Scalar zn = (c > NEARZERO) ? objSpacePoint.z / c : 0.0;
	const Scalar phi = atan2( zn, xn );

	// theta = acos(clamp(y/b, -1, 1))
	const Scalar yn = (b > NEARZERO) ? objSpacePoint.y / b : 0.0;
	const Scalar clampedYn = yn > 1.0 ? 1.0 : (yn < -1.0 ? -1.0 : yn);
	const Scalar theta = acos( clampedYn );
	const Scalar sinTheta = sin( theta );
	const Scalar cosTheta = clampedYn;  // cos(acos(x)) = x

	const Scalar cosPhi = cos(phi);
	const Scalar sinPhi = sin(phi);

	// Position partial derivatives
	// dpdu = dP/dphi
	sd.dpdu = Vector3( -a * sinTheta * sinPhi, 0.0, c * sinTheta * cosPhi );
	// dpdv = dP/dtheta
	sd.dpdv = Vector3( a * cosTheta * cosPhi, -b * sinTheta, c * cosTheta * sinPhi );

	// Normal derivatives via the gradient of the implicit surface
	// N_unnorm = (2x/a^2, 2y/b^2, 2z/c^2) = gradient of (x/a)^2+(y/b)^2+(z/c)^2
	// Equivalently N_unnorm = (Q._00*x, Q._11*y, Q._22*z) since Q._ii = 1/semi_i^2
	//
	// dN_unnorm/dphi = (Q._00 * dpdu.x, Q._11 * dpdu.y, Q._22 * dpdu.z)
	// dN_unnorm/dtheta = (Q._00 * dpdv.x, Q._11 * dpdv.y, Q._22 * dpdv.z)
	//
	// For the normalized normal N = N_unnorm / |N_unnorm|, we use:
	// dN/du = (dN_unnorm/du - N * dot(N, dN_unnorm/du)) / |N_unnorm|
	const Vector3 Nu( Q._00 * sd.dpdu.x, Q._11 * sd.dpdu.y, Q._22 * sd.dpdu.z );
	const Vector3 Nv( Q._00 * sd.dpdv.x, Q._11 * sd.dpdv.y, Q._22 * sd.dpdv.z );

	const Vector3 Nunnorm( Q._00 * objSpacePoint.x, Q._11 * objSpacePoint.y, Q._22 * objSpacePoint.z );
	const Scalar lenN = Vector3Ops::Magnitude( Nunnorm );

	if( lenN > NEARZERO )
	{
		const Scalar invLen = 1.0 / lenN;
		const Vector3 N = Nunnorm * invLen;

		const Scalar dotNNu = Vector3Ops::Dot( N, Nu );
		const Scalar dotNNv = Vector3Ops::Dot( N, Nv );

		sd.dndu = (Nu - N * dotNNu) * invLen;
		sd.dndv = (Nv - N * dotNNv) * invLen;
	}

	sd.uv = Point2( phi, theta );
	sd.valid = true;

	return sd;
}

Scalar EllipsoidGeometry::GetArea( ) const
{
	// This is an approximation taken from:
	// http://home.att.net/~numericana/answer/ellipsoid.htm
	const Scalar p = log(3.0)/log(2.0);
	
	const Scalar ap = pow( m_vRadius.x, p );
	const Scalar bp = pow( m_vRadius.y, p );
	const Scalar cp = pow( m_vRadius.z, p );

	return TWO_PI * (ap*bp + ap*cp + bp*cp);
}

static const unsigned int RADII_ID = 100;

IKeyframeParameter* EllipsoidGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "radii" ) {
		Vector3 v;
		if( sscanf( value.c_str(), "%lf %lf %lf", &v.x, &v.y, &v.z ) == 3 ) {
			p = new Vector3Keyframe( v, RADII_ID );
		}
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void EllipsoidGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case RADII_ID:
		{
			m_vRadius = *(Vector3*)val.getValue();
		}
		break;
	}
}

void EllipsoidGeometry::RegenerateData( )
{
	Q = Matrix4Ops::Identity();
	Q._00 = 1.0/((m_vRadius.x/2)*(m_vRadius.x/2));
	Q._11 = 1.0/((m_vRadius.y/2)*(m_vRadius.y/2));
	Q._22 = 1.0/((m_vRadius.z/2)*(m_vRadius.z/2));
	Q._33 = -1.0;

	m_OVmaxRadius = 1.0 / (r_max( r_max(m_vRadius.x, m_vRadius.y), m_vRadius.z ));

	// Build marginal CDF for theta to enable area-uniform sampling.
	// For the parametric ellipsoid r(theta,phi) = (a*sinT*cosP, b*sinT*sinP, c*cosT),
	// the area element is:
	//   dA = sinT * sqrt(b^2*c^2*sin^2T*cos^2P + a^2*c^2*sin^2T*sin^2P + a^2*b^2*cos^2T) dT dP
	// We numerically integrate over phi to get the marginal M(theta), then build the CDF.
	const Scalar a = m_vRadius.x * 0.5;
	const Scalar b = m_vRadius.y * 0.5;
	const Scalar c = m_vRadius.z * 0.5;

	const Scalar a2 = a*a, b2 = b*b, c2 = c*c;

	m_thetaCDF[0] = 0.0;
	static const unsigned int PHI_STEPS = 64;

	for( unsigned int i = 0; i < THETA_CDF_SIZE; i++ )
	{
		const Scalar theta = PI * (i + 0.5) / Scalar(THETA_CDF_SIZE);
		const Scalar sinT = sin(theta);
		const Scalar cosT = cos(theta);
		const Scalar sin2T = sinT * sinT;
		const Scalar cos2T = cosT * cosT;

		// Numerically integrate the area element magnitude over phi
		Scalar phiSum = 0.0;
		for( unsigned int j = 0; j < PHI_STEPS; j++ )
		{
			const Scalar phi = TWO_PI * (j + 0.5) / Scalar(PHI_STEPS);
			const Scalar cosP = cos(phi);
			const Scalar sinP = sin(phi);

			phiSum += sqrt(
				b2*c2*sin2T*cosP*cosP +
				a2*c2*sin2T*sinP*sinP +
				a2*b2*cos2T
			);
		}

		// Strip area = sinT * (avg over phi) * 2pi * dTheta
		m_thetaCDF[i+1] = m_thetaCDF[i] +
			sinT * (phiSum / Scalar(PHI_STEPS)) * TWO_PI * (PI / Scalar(THETA_CDF_SIZE));
	}

	// Normalize CDF to [0,1]
	const Scalar totalCDF = m_thetaCDF[THETA_CDF_SIZE];
	if( totalCDF > 0.0 ) {
		for( unsigned int i = 1; i <= THETA_CDF_SIZE; i++ ) {
			m_thetaCDF[i] /= totalCDF;
		}
	}
}

