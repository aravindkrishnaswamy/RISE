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
#include "../Utilities/SurfaceCurvature.h"

using namespace RISE;
using namespace RISE::Implementation;

EllipsoidGeometry::EllipsoidGeometry( const Vector3& vRadius ) :
  m_vRadius( vRadius )
{
	RegenerateData();
}

void EllipsoidGeometry::EllipsoidUVFromPosition( const Point3& pt, Point2& uv ) const
{
	const Scalar a = m_vRadius.x;
	const Scalar b = m_vRadius.y;
	const Scalar c = m_vRadius.z;

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

	// Semi-axes = m_vRadius (per-axis radii; like sphere_geometry's radius)
	const Scalar a = m_vRadius.x;
	const Scalar b = m_vRadius.y;
	const Scalar c = m_vRadius.z;

	const Scalar ooA2 = (a > NEARZERO) ? 1.0 / (a*a) : 0.0;
	const Scalar ooB2 = (b > NEARZERO) ? 1.0 / (b*b) : 0.0;
	const Scalar ooC2 = (c > NEARZERO) ? 1.0 / (c*c) : 0.0;

	// DL-116 sibling fix (see SphereGeometry::TessellateToMesh for the full
	// rationale, reproduced briefly here): a pole row (j==0 north, j==nV
	// south) collapses to a SINGLE 3D position with a SINGLE normal --
	// `sinPhi == 0` there, so `pos = (0, +-b, 0)` regardless of `i`, and the
	// gradient normal `(pos.x*ooA2, pos.y*ooB2, pos.z*ooC2)` normalizes to
	// `(0, sign(b), 0)`, likewise independent of `i`.  The `u` forced to 0.0
	// below (pre-existing code, kept) already canonicalizes the texcoord to
	// `(0, v)` for every column too, so a pole row's (position, normal,
	// texcoord) triple is IDENTICAL across every `i` -- there is nothing a
	// second, third, ... index at that row could ever distinguish.  Emit ONE
	// shared vertex per pole instead of `rowStride` coincident ones, and skip
	// the one wedge triangle per pole cell that vertex makes degenerate (the
	// other triangle in that wedge, always the real non-zero-area one, is
	// unchanged).
	unsigned int northPoleIdx = 0;
	unsigned int southPoleIdx = 0;

	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v      = Scalar(j) / Scalar(nV);
		const Scalar phi    = v * PI;
		const Scalar sinPhi = sin(phi);
		const Scalar cosPhi = cos(phi);

		// Collapse pole u so every pole vertex gets the same displacement height
		// (see SphereGeometry::TessellateToMesh for the reasoning).
		const bool atPole = (j == 0) || (j == nV);

		if( atPole ) {
			const Scalar u        = 0.0;
			const Scalar theta    = 0.0;
			const Scalar sinTheta = sin(theta);
			const Scalar cosTheta = cos(theta);

			const Point3 pos(
				a * -sinPhi * cosTheta,
				b * cosPhi,
				c * sinPhi * sinTheta );

			const Vector3 nrm = Vector3Ops::Normalize( Vector3(
				pos.x * ooA2,
				pos.y * ooB2,
				pos.z * ooC2 ) );

			const unsigned int poleIdx = static_cast<unsigned int>( vertices.size() );
			vertices.push_back( pos );
			normals.push_back( nrm );
			coords.push_back( Point2( u, v ) );
			if( j == 0 ) { northPoleIdx = poleIdx; } else { southPoleIdx = poleIdx; }
			continue;
		}

		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u        = Scalar(i) / Scalar(nU);
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

	// Combined (position, normal, texcoord) index for (row j, column i): the
	// pole rows collapse to their single shared entry regardless of i; every
	// other row keeps the ORIGINAL per-column indexing (rowStride wide),
	// offset by the two pole rows now contributing one entry each instead of
	// rowStride.
	const auto Index = [&]( unsigned int j, unsigned int i ) -> unsigned int {
		if( j == 0 )  { return northPoleIdx; }
		if( j == nV ) { return southPoleIdx; }
		return baseIdx + 1 + ( j - 1 ) * rowStride + i;
	};

	for( unsigned int j = 0; j < nV; j++ ) {
		for( unsigned int i = 0; i < nU; i++ ) {
			const unsigned int a_idx = Index( j,   i     );
			const unsigned int b_idx = Index( j,   i + 1 );
			const unsigned int c_idx = Index( j+1, i     );
			const unsigned int d_idx = Index( j+1, i + 1 );

			// At the north pole row (j==0) a_idx==b_idx (the shared pole
			// entry), so the first triangle is fully degenerate; symmetric
			// at the south pole row (j+1==nV), where c_idx==d_idx makes the
			// SECOND triangle degenerate instead.  Skip exactly the one
			// that collapses; away from the poles this is the original
			// two-triangles-per-wedge assembly, unchanged.
			const bool bNorthPoleWedge = ( j == 0 );
			const bool bSouthPoleWedge = ( j + 1 == nV );

			if( !bNorthPoleWedge ) {
				tris.push_back( MakeIndexedTriangleSameIdx( a_idx, c_idx, b_idx ) );
			}
			if( !bSouthPoleWedge ) {
				tris.push_back( MakeIndexedTriangleSameIdx( b_idx, c_idx, d_idx ) );
			}
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
		ri.vGeomNormal = ri.vNormal;	// analytical surface: shading == geometric

		if( bComputeExitInfo ) {
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			ri.vNormal2 = Vector3Ops::Normalize(
				Vector3( Q._00*ri.ptExit.x,
						 Q._11*ri.ptExit.y,
						 Q._22*ri.ptExit.z )
				);
			ri.vGeomNormal2 = ri.vNormal2;	// analytical: shading == geometric
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

		// PHASE-1 GEOMETRY-DERIVED SHADING SIGNALS
		// (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 5.4) -- see the identical
		// block in SphereGeometry::IntersectRay for the full rationale:
		// publish the closed-form Weingarten map this primitive already knows,
		// ungated (a few trig calls on data the hit produced); gate only
		// `scaleHint`, which nothing but the expression VM's `curv` reads.
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
	// Semi-axes = m_vRadius (per-axis radii)
	const Scalar a = m_vRadius.x;
	const Scalar b = m_vRadius.y;
	const Scalar c = m_vRadius.z;

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

	// Semi-axes = m_vRadius (per-axis radii)
	const Scalar a = m_vRadius.x;
	const Scalar b = m_vRadius.y;
	const Scalar c = m_vRadius.z;

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

	// THE CHART MAP (docs/GEOMETRY_DERIVATIVES.md "The texcoord chart
	// map").  `EllipsoidUVFromPosition` -- the single source of ptCoord
	// for both IntersectRay and UniformRandomPoint -- emits
	//     s = atan2(z/c, -x/a) / (2*PI),   t = acos(y/b) / PI
	// while the parameters differentiated above are
	//     u = phi = atan2(z/c, x/a),       v = theta = acos(y/b).
	// The x-negation is a rotation by PI in the azimuth: the texture s
	// runs backwards from the derivative phi, s = (PI - phi)/(2*PI)
	// modulo the [0, 2*PI) wrap, exactly as on the sphere.
	sd.dsdu = -1.0 / TWO_PI;
	sd.dsdv = 0.0;
	sd.dtdu = 0.0;
	sd.dtdv = 1.0 / PI;
	sd.texChartValid = true;

	return sd;
}

bool EllipsoidGeometry::ComputeAnalyticalDerivatives(
	const Point2& uv,
	Scalar        /*smoothing*/,   // no high-frequency detail to attenuate
	Point3&       outPosition,
	Vector3&      outNormal,
	Vector3&      outDpdu,
	Vector3&      outDpdv,
	Vector3&      outDndu,
	Vector3&      outDndv
	) const
{
	// Parameterisation MUST match EllipsoidGeometry::TessellateToMesh and
	// EllipsoidGeometry::IntersectRay's position-based UV inverse:
	//   theta = 2π·u   (uv.x ∈ [0, 1] wraps the equator)
	//   phi   = π·v    (uv.y ∈ [0, 1] from north pole to south pole)
	//   P_x   = -a·sin(phi)·cos(theta)
	//   P_y   =  b·cos(phi)
	//   P_z   =  c·sin(phi)·sin(theta)
	// Otherwise the (u, v) coming out of the on-mesh path or from
	// IntersectRay wouldn't agree with what we feed in here.
	//
	// Semi-axes = m_vRadius (per-axis radii).
	const Scalar a = m_vRadius.x;
	const Scalar b = m_vRadius.y;
	const Scalar c = m_vRadius.z;

	const Scalar theta = uv.x * TWO_PI;
	const Scalar phi   = uv.y * PI;
	const Scalar st = sin( theta );
	const Scalar ct = cos( theta );
	const Scalar sp = sin( phi );
	const Scalar cp = cos( phi );

	outPosition = Point3(
		-a * sp * ct,
		 b *      cp,
		 c * sp * st );

	// Tangent vectors via direct differentiation:
	//   dP/dtheta = (a·sp·st, 0, c·sp·ct)         (since dP_x/dtheta = -a·sp·(-st) = a·sp·st)
	//   dP/dphi   = (-a·cp·ct, -b·sp, c·cp·st)
	//   dP/du     = (dP/dtheta) × 2π
	//   dP/dv     = (dP/dphi)   × π
	outDpdu = Vector3(
		TWO_PI * a * sp * st,
		0.0,
		TWO_PI * c * sp * ct );
	outDpdv = Vector3(
		PI * (-a) * cp * ct,
		PI * (-b) * sp,
		PI *   c  * cp * st );

	// Gradient-based outward normal: G = (x/a², y/b², z/c²); N = G/|G|.
	const Scalar invA2 = (a > NEARZERO) ? 1.0 / (a*a) : 0.0;
	const Scalar invB2 = (b > NEARZERO) ? 1.0 / (b*b) : 0.0;
	const Scalar invC2 = (c > NEARZERO) ? 1.0 / (c*c) : 0.0;

	const Vector3 G(
		outPosition.x * invA2,
		outPosition.y * invB2,
		outPosition.z * invC2 );
	const Scalar Gmag = Vector3Ops::Magnitude( G );
	if( Gmag <= NEARZERO ) {
		// Pole degeneracy or zero-radius axis — caller should treat as failed.
		outNormal = Vector3( 0, 1, 0 );
		outDndu   = Vector3( 0, 0, 0 );
		outDndv   = Vector3( 0, 0, 0 );
		return false;
	}
	outNormal = G * (1.0 / Gmag);

	// dN/du, dN/dv via central FD on the closed-form gradient-normal.
	// The ellipsoid is C∞-smooth, so FD with small uv-step is numerically
	// equivalent to the analytical derivative, at the cost of 4 trig
	// evaluations.  Hand-deriving d(G/|G|)/du is long and error-prone.
	auto evalUnitNormal = [&]( Scalar u_, Scalar v_ ) -> Vector3 {
		const Scalar t  = u_ * TWO_PI;
		const Scalar p  = v_ * PI;
		const Scalar st_ = sin( t );
		const Scalar ct_ = cos( t );
		const Scalar sp_ = sin( p );
		const Scalar cp_ = cos( p );
		const Vector3 G_(
			(-a * sp_ * ct_) * invA2,
			( b *       cp_) * invB2,
			( c * sp_ * st_) * invC2 );
		const Scalar gm = Vector3Ops::Magnitude( G_ );
		return ( gm > NEARZERO ) ? ( G_ * (1.0 / gm) ) : Vector3( 0, 1, 0 );
	};
	const Scalar epsUv = 1.0e-3;
	const Vector3 N_uplus  = evalUnitNormal( uv.x + epsUv, uv.y );
	const Vector3 N_uminus = evalUnitNormal( uv.x - epsUv, uv.y );
	const Vector3 N_vplus  = evalUnitNormal( uv.x, uv.y + epsUv );
	const Vector3 N_vminus = evalUnitNormal( uv.x, uv.y - epsUv );
	const Scalar inv2eps = 1.0 / (2.0 * epsUv);
	outDndu = ( N_uplus  - N_uminus ) * inv2eps;
	outDndv = ( N_vplus  - N_vminus ) * inv2eps;
	return true;
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
	Q._00 = 1.0/(m_vRadius.x*m_vRadius.x);
	Q._11 = 1.0/(m_vRadius.y*m_vRadius.y);
	Q._22 = 1.0/(m_vRadius.z*m_vRadius.z);
	Q._33 = -1.0;

	m_OVmaxRadius = 1.0 / (r_max( r_max(m_vRadius.x, m_vRadius.y), m_vRadius.z ));

	// Build marginal CDF for theta to enable area-uniform sampling.
	// For the parametric ellipsoid r(theta,phi) = (a*sinT*cosP, b*sinT*sinP, c*cosT),
	// the area element is:
	//   dA = sinT * sqrt(b^2*c^2*sin^2T*cos^2P + a^2*c^2*sin^2T*sin^2P + a^2*b^2*cos^2T) dT dP
	// We numerically integrate over phi to get the marginal M(theta), then build the CDF.
	const Scalar a = m_vRadius.x;
	const Scalar b = m_vRadius.y;
	const Scalar c = m_vRadius.z;

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


namespace
{
	//! DL-15.  The classic Lagrange-multiplier construction for the
	//! nearest point on a triaxial ellipsoid {(x/a)^2+(y/b)^2+(z/c)^2=1}
	//! to an EXTERIOR point q: the nearest point is
	//!   y_i = q_i * a_i^2 / (a_i^2 + t)
	//! where t > 0 is the unique root of
	//!   F(t) = sum_i (a_i q_i)^2 / (a_i^2+t)^2 - 1 .
	//! F is a sum of strictly-decreasing, convex terms on t > -min(a_i^2),
	//! so it is itself strictly decreasing there: F(0) = sum (q_i/a_i)^2 - 1
	//! (the same quantity as the caller's own "is q exterior" test) is
	//! positive by the caller's precondition, and F(t) -> -1 as t -> +inf,
	//! so a unique root exists in (0, +inf) and a safeguarded Newton
	//! iteration (clamped back into a shrinking bisection bracket
	//! whenever a step would leave it) converges quickly from any bracket
	//! containing it.
	inline Scalar EllipsoidLagrangeF(
		Scalar a2x, Scalar a2y, Scalar a2z, Scalar qx, Scalar qy, Scalar qz, Scalar t )
	{
		const Scalar dx = a2x + t, dy = a2y + t, dz = a2z + t;
		return ( a2x * qx * qx ) / ( dx * dx )
			 + ( a2y * qy * qy ) / ( dy * dy )
			 + ( a2z * qz * qz ) / ( dz * dz )
			 - Scalar( 1 );
	}

	inline Scalar EllipsoidLagrangeDF(
		Scalar a2x, Scalar a2y, Scalar a2z, Scalar qx, Scalar qy, Scalar qz, Scalar t )
	{
		const Scalar dx = a2x + t, dy = a2y + t, dz = a2z + t;
		return Scalar( -2 ) * (
			  ( a2x * qx * qx ) / ( dx * dx * dx )
			+ ( a2y * qy * qy ) / ( dy * dy * dy )
			+ ( a2z * qz * qz ) / ( dz * dz * dz ) );
	}

	//! Refines the crude scaled-sphere upper bound into a near-exact one
	//! for an EXTERIOR point (caller guarantees `g(q) = sum (q_i/a_i)^2 >
	//! 1`).  Returns false (leaving `outDist` untouched) on any
	//! non-finite intermediate, so the caller's crude bound survives as
	//! the fallback.
	//!
	//! THE SAFETY ARGUMENT DOES NOT DEPEND ON HOW WELL NEWTON CONVERGES.
	//! After solving for `t` (well or poorly -- a handful of iterations is
	//! plenty in practice, but nothing below RELIES on full convergence),
	//! the candidate point is rescaled by `kOutwardPad / sqrt(g(y))`.
	//! That forces `g(y_scaled) == kOutwardPad^2 > 1` exactly (up to
	//! ordinary double rounding in one sqrt and one multiply, which
	//! `kOutwardPad`'s margin swamps by ~7 orders of magnitude) --
	//! i.e. the rescaled candidate lies genuinely OUTSIDE the ellipsoid,
	//! REGARDLESS of how close `t` came to the true root.  Any point
	//! outside (or on) the ellipsoid is, by definition, at least as far
	//! from `q` as the true nearest SURFACE point, so the resulting
	//! distance is a valid upper bound by construction; the Newton
	//! iterations exist only to make that bound TIGHT, not to make it
	//! safe.
	bool RefineExteriorDistanceToEllipsoid(
		Scalar a, Scalar b, Scalar c, const Point3& q, Scalar& outDist )
	{
		const Scalar a2x = a * a, a2y = b * b, a2z = c * c;
		const Scalar aMax2 = std::max( a2x, std::max( a2y, a2z ) );
		if( !( aMax2 > Scalar( 0 ) ) ) {
			return false;
		}

		// Bracket [lo, hi]: F(lo=0) > 0 is the caller's own "exterior"
		// precondition; grow hi by doubling until F(hi) <= 0, which must
		// happen (F -> -1 as t -> infinity) short of the guard -- the
		// guard exists only to bound a NaN/inf runaway, not because this
		// loop is expected to need many iterations in practice.
		Scalar lo = Scalar( 0 );
		Scalar hi = aMax2;
		for( int guard = 0; guard < 200; ++guard ) {
			const Scalar fhi = EllipsoidLagrangeF( a2x, a2y, a2z, q.x, q.y, q.z, hi );
			if( !RISE::IsFiniteDouble( (double)fhi ) ) {
				return false;
			}
			if( fhi <= Scalar( 0 ) ) {
				break;
			}
			hi *= Scalar( 2 );
			if( guard == 199 ) {
				return false;	// did not bracket -- let the caller's crude bound stand
			}
		}

		// A FEW safeguarded Newton iterations (Newton when it stays
		// inside the current bracket, bisection otherwise): F is smooth
		// and monotone, so this is the classic "rtsafe" pattern and
		// converges to double precision in well under 30 steps for any
		// input this function is reachable with.
		Scalar t = lo;
		for( int iter = 0; iter < 30; ++iter ) {
			const Scalar f = EllipsoidLagrangeF( a2x, a2y, a2z, q.x, q.y, q.z, t );
			if( f > Scalar( 0 ) ) { lo = t; } else { hi = t; }
			const Scalar df = EllipsoidLagrangeDF( a2x, a2y, a2z, q.x, q.y, q.z, t );
			Scalar tNext = ( df < Scalar( 0 ) ) ? ( t - f / df ) : ( Scalar( 0.5 ) * ( lo + hi ) );
			if( !( tNext > lo ) || !( tNext < hi ) ) {
				tNext = Scalar( 0.5 ) * ( lo + hi );
			}
			t = tNext;
		}

		const Scalar dx = a2x + t, dy = a2y + t, dz = a2z + t;
		Scalar yx = q.x * a2x / dx;
		Scalar yy = q.y * a2y / dy;
		Scalar yz = q.z * a2z / dz;

		const Scalar rx = yx / a, ry = yy / b, rz = yz / c;
		const Scalar gy = rx * rx + ry * ry + rz * rz;
		if( !( gy > Scalar( 0 ) ) || !RISE::IsFiniteDouble( (double)gy ) ) {
			return false;
		}

		// The outward pad: independent of how well `t` converged, this
		// guarantees g(y_scaled) = kOutwardPad^2 > 1 -- see the function
		// comment. 1e-9 is ~7 orders of magnitude above double rounding
		// noise in the sqrt/multiply below and ~9 orders below any radius
		// this signal is ever queried with, so it costs nothing an author
		// could observe.
		const Scalar kOutwardPad = Scalar( 1 ) + Scalar( 1e-9 );
		const Scalar scale = kOutwardPad / std::sqrt( gy );
		yx *= scale; yy *= scale; yz *= scale;

		const Scalar ddx = yx - q.x, ddy = yy - q.y, ddz = yz - q.z;
		const Scalar dist = std::sqrt( ddx * ddx + ddy * ddy + ddz * ddz );
		if( !RISE::IsFiniteDouble( (double)dist ) ) {
			return false;
		}
		outDist = dist;
		return true;
	}
}

//! IGeometry::DistanceToSurface -- an UPPER BOUND, and (DL-15) now a
//! near-exact one rather than a crude one.
//!
//! WHERE THE CRUDE BOUND CAME FROM, kept as the fallback and the safety
//! net.  There is no *closed form* for the distance from a point to an
//! ellipsoid -- it is the root of a sextic -- but the map `diag(a,b,c)`
//! that carries the unit sphere onto this ellipsoid gives one for free:
//! evaluate the exact unit-sphere distance at the pulled-back point, then
//! push the answer forward by the LARGEST semi-axis. For a linear map
//! `M`, the image of the unit-sphere minimiser is A surface point of the
//! ellipsoid, at distance at most `sigmaMax * d_unit` from the query
//! point -- and the true distance is the minimum over ALL surface
//! points, so it can only be smaller. That bound is exact along the
//! largest-semi-axis direction and loose by up to the full semi-axis
//! ratio everywhere else (DL-15; docs/DEBT_LEDGER.md, worked example: a
//! 4:1 ellipsoid at true distance 2.75 reported 11.0).
//!
//! THE FIX.  `RefineExteriorDistanceToEllipsoid` (above) solves the
//! standard Lagrange-multiplier construction for the near-exact nearest
//! point by safeguarded Newton, then rescales it to land marginally
//! OUTSIDE the true surface before measuring -- which is what makes the
//! result a valid upper bound REGARDLESS of how well the iteration
//! converged (see that function's own comment). The two bounds are
//! independently safe, so this takes their MINIMUM: the refined one is
//! tighter whenever it is reachable, and a refinement failure (a
//! non-finite intermediate, or the bracket search running out) silently
//! falls back to the crude one rather than ever regressing past it.
bool EllipsoidGeometry::DistanceToSurface( const Point3& ptObject, const Scalar maxDistObject, Scalar& outDist ) const
{
	(void)maxDistObject;

	const Scalar a = m_vRadius.x, b = m_vRadius.y, c = m_vRadius.z;
	if( !( a > Scalar( 0 ) ) || !( b > Scalar( 0 ) ) || !( c > Scalar( 0 ) ) ) {
		return false;		// a flattened ellipsoid is not a surface this form describes
	}

	const Scalar qx = ptObject.x / a, qy = ptObject.y / b, qz = ptObject.z / c;
	const Scalar rq = std::sqrt( qx*qx + qy*qy + qz*qz );
	// CLAMPED AT ZERO rather than fabs: a point inside the ellipsoid reads
	// 0 ("interpenetration IS contact"), and the pulled-back point is
	// inside the unit sphere exactly when the original is inside the
	// ellipsoid, so the map does the inside test for free.  The
	// refinement below only ever applies to the EXTERIOR case: an
	// interior point's "nearest exterior surface point" is not the
	// quantity this convention reports at all.
	const Scalar sgnUnit = rq - Scalar( 1 );
	const Scalar dUnit = ( sgnUnit > Scalar( 0 ) ) ? sgnUnit : Scalar( 0 );

	const Scalar sigmaMax = std::max( a, std::max( b, c ) );
	const Scalar dCrude = dUnit * sigmaMax;
	if( !RISE::IsFiniteDouble( (double)dCrude ) ) {
		return false;
	}

	Scalar d = dCrude;
	if( sgnUnit > Scalar( 0 ) ) {
		Scalar dRefined = Scalar( 0 );
		if( RefineExteriorDistanceToEllipsoid( a, b, c, ptObject, dRefined )
		 && RISE::IsFiniteDouble( (double)dRefined ) && dRefined >= Scalar( 0 ) ) {
			d = std::min( d, dRefined );
		}
	}

	outDist = d;
	return true;
}

//! IGeometry::SignedDistanceLower -- the SIGN exactly, the MAGNITUDE as a
//! lower bound, and the flag NEVER set.
//!
//! The pulled-back point is inside the unit sphere exactly when the
//! original is inside the ellipsoid, so `|q| - 1` carries the exact sign.
//! Its magnitude scales into ellipsoid space by a factor between the
//! smallest and the largest semi-axis, so `x min(a,b,c)` is a LOWER bound
//! on both sides -- the mirror of the unsigned query's `x max`, and the
//! reason a composite reaching an ellipsoid can only take the STRICT arm.
bool EllipsoidGeometry::SignedDistanceLower( const Point3& ptObject, const Scalar maxDistObject,
	Scalar& outSigned, bool& outExact ) const
{
	(void)maxDistObject;
	outExact = false;

	const Scalar a = m_vRadius.x, b = m_vRadius.y, c = m_vRadius.z;
	if( !( a > Scalar( 0 ) ) || !( b > Scalar( 0 ) ) || !( c > Scalar( 0 ) ) ) {
		return false;
	}

	const Scalar qx = ptObject.x / a, qy = ptObject.y / b, qz = ptObject.z / c;
	const Scalar rq = std::sqrt( qx*qx + qy*qy + qz*qz );
	const Scalar sgnUnit = rq - Scalar( 1 );

	const Scalar sigmaMin = std::min( a, std::min( b, c ) );
	const Scalar sgn = sgnUnit * sigmaMin;
	if( !RISE::IsFiniteDouble( (double)sgn ) ) {
		return false;
	}
	outSigned = sgn;
	return true;
}
