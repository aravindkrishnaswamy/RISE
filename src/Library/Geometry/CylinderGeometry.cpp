//////////////////////////////////////////////////////////////////////
//
//  CylinderGeometry.cpp - Implementation of the cylinder class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 20, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CylinderGeometry.h"
#include "GeometryUtilities.h"
#include "../Intersection/RayPrimitiveIntersections.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Animation/KeyframableHelper.h"

using namespace RISE;
using namespace RISE::Implementation;

CylinderGeometry::CylinderGeometry( const int chAxis, const Scalar dRadius, const Scalar dHeight, const bool capped ) :
  m_chAxis( chAxis ),
  m_dRadius( dRadius ),
  m_dHeight( dHeight ),
  m_bCapped( capped )
{
	RegenerateData();
}

CylinderGeometry::~CylinderGeometry( )
{
}

bool CylinderGeometry::TessellateToMesh(
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

	const Scalar height = m_dAxisMax - m_dAxisMin;

	for( unsigned int j = 0; j <= nV; j++ ) {
		const Scalar v     = Scalar(j) / Scalar(nV);
		const Scalar axial = m_dAxisMin + v * height;

		for( unsigned int i = 0; i <= nU; i++ ) {
			const Scalar u        = Scalar(i) / Scalar(nU);
			const Scalar theta    = u * TWO_PI;
			const Scalar cosTheta = cos(theta);
			const Scalar sinTheta = sin(theta);

			Point3  pos;
			Vector3 nrm;
			switch( m_chAxis ) {
				case 'x':
					pos = Point3( axial,             m_dRadius * cosTheta, m_dRadius * sinTheta );
					nrm = Vector3( 0.0,              cosTheta,             sinTheta );
					break;
				case 'y':
					pos = Point3( m_dRadius * cosTheta, axial,             m_dRadius * sinTheta );
					nrm = Vector3( cosTheta,            0.0,               sinTheta );
					break;
				case 'z':
				default:
					pos = Point3( m_dRadius * cosTheta, m_dRadius * sinTheta, axial );
					nrm = Vector3( cosTheta,            sinTheta,             0.0 );
					break;
			}

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

	if( m_bCapped ) {
		// Append a triangle fan for each end cap.  Fresh vertices carry the
		// axis-aligned cap normal and the planar disk UV matching
		// FillSurfaceNormalUV, so the tessellation is a closed solid consistent
		// with the analytic intersection.
		const Scalar caps[2]    = { m_dAxisMin, m_dAxisMax };
		const Scalar capSgn[2]  = { -1.0,       1.0        };

		// Handedness of the (ra, rb) radial parametrisation w.r.t. +axis:
		//   x: (ra,rb)=(y,z), cross(+y,+z)=+x  -> +1
		//   y: (ra,rb)=(x,z), cross(+x,+z)=-y  -> -1  (left-handed)
		//   z: (ra,rb)=(x,y), cross(+x,+y)=+z  -> +1
		// A CCW-in-(ra,rb) fan therefore yields a face normal along hf*axis.
		const Scalar hf = ( m_chAxis == 'y' ) ? -1.0 : 1.0;

		for( int cap = 0; cap < 2; cap++ ) {
			const Scalar  axial = caps[cap];
			const Scalar  sgn   = capSgn[cap];

			Vector3 nrm;
			Point3  center;
			switch( m_chAxis ) {
				case 'x': nrm = Vector3( sgn, 0.0, 0.0 ); center = Point3( axial, 0.0, 0.0 ); break;
				case 'y': nrm = Vector3( 0.0, sgn, 0.0 ); center = Point3( 0.0, axial, 0.0 ); break;
				case 'z':
				default:  nrm = Vector3( 0.0, 0.0, sgn ); center = Point3( 0.0, 0.0, axial ); break;
			}

			const unsigned int centerIdx = static_cast<unsigned int>( vertices.size() );
			vertices.push_back( center );
			normals.push_back( nrm );
			coords.push_back( Point2( 0.5, 0.5 ) );

			const unsigned int rimBase = static_cast<unsigned int>( vertices.size() );
			for( unsigned int i = 0; i <= nU; i++ ) {
				const Scalar theta = ( Scalar(i) / Scalar(nU) ) * TWO_PI;
				const Scalar ra    = m_dRadius * cos( theta );
				const Scalar rb    = m_dRadius * sin( theta );

				Point3 pos;
				switch( m_chAxis ) {
					case 'x': pos = Point3( axial, ra, rb ); break;
					case 'y': pos = Point3( ra, axial, rb ); break;
					case 'z':
					default:  pos = Point3( ra, rb, axial ); break;
				}

				vertices.push_back( pos );
				normals.push_back( nrm );
				coords.push_back( Point2( 0.5*(cos(theta) + 1.0), 0.5*(sin(theta) + 1.0) ) );
			}

			// Wind so the geometric face normal points outward (= sgn*axis).  A
			// CCW-in-(ra,rb) fan (center, r0, r1) has face normal hf*axis, so use
			// it when sgn*hf > 0 and the reversed winding otherwise.  Per-vertex
			// normals are outward regardless, but keeping the winding consistent
			// makes the winding-derived geometric normal correct too.
			const bool ccw = ( sgn * hf > 0.0 );
			for( unsigned int i = 0; i < nU; i++ ) {
				const unsigned int r0 = rimBase + i;
				const unsigned int r1 = rimBase + i + 1;
				if( ccw ) {
					tris.push_back( MakeIndexedTriangleSameIdx( centerIdx, r0, r1 ) );
				} else {
					tris.push_back( MakeIndexedTriangleSameIdx( centerIdx, r1, r0 ) );
				}
			}
		}
	}

	return true;
}

bool CylinderGeometry::IntersectCappedSolid( const Ray& ray, Scalar& tNear, int& surfNear, Scalar& tFar, int& surfFar ) const
{
	// Decompose the ray into (axial, radialA, radialB) for the cylinder axis.
	// The radial pair is only used for the (rotation-invariant) side quadratic
	// and cap-disk membership, so its internal ordering is immaterial here; the
	// axial component is the one that must lie along the cylinder axis.  The
	// pairing below matches FillSurfaceNormalUV's decomposition so cap UVs are
	// consistent between intersection and area sampling.
	Scalar axO, raO, rbO, axD, raD, rbD;
	switch( m_chAxis )
	{
	case 'x':
		axO = ray.origin.x; raO = ray.origin.y; rbO = ray.origin.z;
		axD = ray.Dir().x;  raD = ray.Dir().y;  rbD = ray.Dir().z;
		break;
	case 'y':
		axO = ray.origin.y; raO = ray.origin.x; rbO = ray.origin.z;
		axD = ray.Dir().y;  raD = ray.Dir().x;  rbD = ray.Dir().z;
		break;
	case 'z':
	default:
		axO = ray.origin.z; raO = ray.origin.x; rbO = ray.origin.y;
		axD = ray.Dir().z;  raD = ray.Dir().x;  rbD = ray.Dir().y;
		break;
	}

	// A finite capped cylinder is convex, so a ray crosses its boundary at most
	// twice.  Gather every valid candidate (up to 2 side + 2 cap) and take the
	// nearest as entry, the farthest as exit — robust to grazing/rim cases.
	Scalar	candT[4];
	int		candS[4];
	int		n = 0;

	// --- Side wall: infinite cylinder of radius m_dRadius about the axis ---
	// Reduced quadratic A t^2 + 2 B t + C = 0 (B is the half coefficient),
	// matching Ray?CylinderIntersection's convention.
	const Scalar A = raD*raD + rbD*rbD;
	if( A > NEARZERO ) {
		const Scalar B = raO*raD + rbO*rbD;
		const Scalar C = raO*raO + rbO*rbO - m_dRadius*m_dRadius;
		const Scalar disc = B*B - A*C;
		if( disc >= 0.0 ) {
			const Scalar sq   = sqrt( disc );
			const Scalar inva = 1.0 / A;
			const Scalar tt[2] = { (-B - sq)*inva, (-B + sq)*inva };
			for( int i = 0; i < 2; i++ ) {
				const Scalar t = tt[i];
				if( t > NEARZERO ) {
					const Scalar ax = axO + t*axD;
					if( ax >= m_dAxisMin && ax <= m_dAxisMax ) {
						candT[n] = t; candS[n] = SURF_SIDE; n++;
					}
				}
			}
		}
	}

	// --- End caps: planes axial = axisMin / axisMax, disk of radius m_dRadius ---
	if( fabs(axD) > NEARZERO ) {
		const Scalar invAxD   = 1.0 / axD;
		const Scalar planes[2] = { m_dAxisMin, m_dAxisMax };
		const int    surfs[2]  = { SURF_CAPMIN, SURF_CAPMAX };
		const Scalar r2        = m_dRadius*m_dRadius;
		for( int i = 0; i < 2; i++ ) {
			const Scalar t = (planes[i] - axO)*invAxD;
			if( t > NEARZERO ) {
				const Scalar ra = raO + t*raD;
				const Scalar rb = rbO + t*rbD;
				if( ra*ra + rb*rb <= r2 ) {
					candT[n] = t; candS[n] = surfs[i]; n++;
				}
			}
		}
	}

	if( n == 0 ) {
		return false;
	}

	// Nearest candidate is always the entry (or, when the origin is inside, the
	// single exit crossing).
	int iMin = 0;
	for( int i = 1; i < n; i++ ) {
		if( candT[i] < candT[iMin] ) iMin = i;
	}
	tNear = candT[iMin]; surfNear = candS[iMin];

	// Distinguish "origin inside the solid" (one crossing ahead => leave the exit
	// range at 0, matching the open-tube convention) from "origin outside"
	// (entry + exit).  Use an explicit inside test rather than counting distinct
	// candidates: when the exit lands exactly on the rim, the side wall and a cap
	// disk BOTH list that single point at all-but-identical t, and a
	// count-distinct test would wrongly report a phantom entry+exit segment of
	// FP-rounding width.
	const bool originInside =
		( raO*raO + rbO*rbO <= m_dRadius*m_dRadius ) &&
		( axO >= m_dAxisMin && axO <= m_dAxisMax );

	if( originInside ) {
		tFar = 0.0; surfFar = surfNear;
	} else {
		int iMax = 0;
		for( int i = 1; i < n; i++ ) {
			if( candT[i] > candT[iMax] ) iMax = i;
		}
		if( iMax != iMin ) {
			tFar = candT[iMax]; surfFar = candS[iMax];
		} else {
			// A single distinct crossing from outside (a grazing/tangent touch):
			// no meaningful exit — leave the exit range at 0 (open-tube convention).
			tFar = 0.0; surfFar = surfNear;
		}
	}
	return true;
}

void CylinderGeometry::FillSurfaceNormalUV( const Point3& pt, int surf, Vector3& normal, Point2* coord ) const
{
	if( surf == SURF_SIDE ) {
		GeometricUtilities::CylinderNormal( pt, m_chAxis, normal );
		if( coord ) {
			GeometricUtilities::CylinderTextureCoord( pt, m_chAxis, m_dOVRadius, m_dAxisMin, m_dAxisMax, *coord );
		}
		return;
	}

	// End cap: outward axis-aligned normal; a planar disk UV in [0,1]^2 keyed off
	// the two radial coordinates (u,v) = ( (ra/r+1)/2, (rb/r+1)/2 ).  This is the
	// same mapping TessellateToMesh stamps on the cap fan, so the two agree.
	const Scalar sgn = (surf == SURF_CAPMAX) ? 1.0 : -1.0;
	Scalar ra = 0.0, rb = 0.0;
	switch( m_chAxis )
	{
	case 'x': normal = Vector3( sgn, 0.0, 0.0 ); ra = pt.y; rb = pt.z; break;
	case 'y': normal = Vector3( 0.0, sgn, 0.0 ); ra = pt.x; rb = pt.z; break;
	case 'z':
	default:  normal = Vector3( 0.0, 0.0, sgn ); ra = pt.x; rb = pt.y; break;
	}

	if( coord ) {
		*coord = Point2( 0.5*(ra*m_dOVRadius + 1.0), 0.5*(rb*m_dOVRadius + 1.0) );
	}
}

void CylinderGeometry::IntersectRay( RayIntersectionGeometric& ri, const bool bHitFrontFaces, const bool bHitBackFaces, const bool bComputeExitInfo ) const
{
	if( m_bCapped )
	{
		Scalar	tNear = 0.0, tFar = 0.0;
		int		surfNear = SURF_SIDE, surfFar = SURF_SIDE;
		if( IntersectCappedSolid( ri.ray, tNear, surfNear, tFar, surfFar ) )
		{
			// Honour the front/back-face filter like Sphere / Box / SDF (the
			// IGeometry contract): the capped cylinder is CONVEX, so a from-outside
			// ray has an entry (FRONT: outward normal opposes the ray) then an exit
			// (BACK); a from-inside ray has only the exit ahead.  Report the NEAREST
			// crossing whose facing the caller asked for -- stepping past a
			// disallowed nearer crossing to the farther one -- so a front-only ray
			// started inside doesn't report the back exit and (false,false) misses.
			const Vector3& dir = ri.ray.Dir();
			const bool   hasFar = ( tFar > tNear );

			Vector3 nNear; FillSurfaceNormalUV( ri.ray.PointAtLength( tNear ), surfNear, nNear, 0 );
			const bool frontNear = ( nNear.x*dir.x + nNear.y*dir.y + nNear.z*dir.z ) < 0.0;

			bool   got     = ( frontNear && bHitFrontFaces ) || ( !frontNear && bHitBackFaces );
			Scalar tHit    = tNear;  int surfHit   = surfNear;
			Scalar tOther  = tFar;   int surfOther = surfFar;   // the remaining crossing = exit info

			if( !got && hasFar )
			{
				Vector3 nFar; FillSurfaceNormalUV( ri.ray.PointAtLength( tFar ), surfFar, nFar, 0 );
				const bool frontFar = ( nFar.x*dir.x + nFar.y*dir.y + nFar.z*dir.z ) < 0.0;
				if( ( frontFar && bHitFrontFaces ) || ( !frontFar && bHitBackFaces ) ) {
					tHit = tFar; surfHit = surfFar; tOther = 0.0; surfOther = surfFar;  // nothing beyond the exit
					got  = true;
				}
			}

			if( got )
			{
				ri.bHit   = true;
				ri.range  = tHit;
				ri.range2 = ( tOther > tHit ) ? tOther : 0.0;   // 0 = no exit beyond (open-tube sentinel)

				ri.ptIntersection = ri.ray.PointAtLength( ri.range );
				FillSurfaceNormalUV( ri.ptIntersection, surfHit, ri.vNormal, &ri.ptCoord );
				ri.vGeomNormal = ri.vNormal;	// analytical surface: shading == geometric

				if( bComputeExitInfo ) {
					ri.ptExit = ri.ray.PointAtLength( ri.range2 );
					FillSurfaceNormalUV( ri.ptExit, ( ri.range2 > 0.0 ) ? surfOther : surfHit, ri.vNormal2, 0 );
					ri.vGeomNormal2 = ri.vNormal2;	// analytical: shading == geometric
				}
			}
			else
			{
				ri.bHit   = false;
				ri.range  = RISE_INFINITY;
				ri.range2 = RISE_INFINITY;
			}
		}
		else
		{
			ri.bHit   = false;
			ri.range  = RISE_INFINITY;
			ri.range2 = RISE_INFINITY;
		}
		return;
	}

	// --- Open-tube path (m_bCapped == false): side wall only, unchanged ---

	/* IMPLEMENT THIS OPTO!
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInCylinder = IsPointInsideCylinder( ri.ray.origin, m_dRadius, m_ptCenter );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInCylinder )
		return;

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInCylinder )
		return;
	*/

	HIT	h;
	bool bHitFarSide = false;
	switch( m_chAxis )
	{
	case 'x':
	default:
		RayXCylinderIntersection( ri.ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'y':
		RayYCylinderIntersection( ri.ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'z':
		RayZCylinderIntersection( ri.ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	}

	ri.bHit = h.bHit;
	ri.range = h.dRange;
	ri.range2 = h.dRange2;

	// Now compute the normal and texture mapping co-ordinates.
	//
	// Normal convention: ALWAYS outward (away from the cylinder axis),
	// matching SphereGeometry / EllipsoidGeometry / other analytic
	// primitives.  Earlier revisions flipped the normal to face the
	// ray (i.e. inward at back-face hits) but that breaks
	// `DielectricSPF::GenerateScatteredRay`'s post-refraction
	// direction check at lines 141-145 (DielectricSPF.cpp), which
	// assumes the standard "vNormal points out of the medium" path-
	// tracer convention.  With the inward-pointing normal at a
	// back-face exit, the refracted-ray sign test rejects every
	// valid exit, leaving only Fresnel reflection and rendering the
	// cylinder dielectric as solid black after a few bounces.
	// `Optics::CalculateRefractedRay` / `CalculateDielectricReflectance`
	// are sign-symmetric (they flip the normal internally if it
	// faces the ray), so leaving the normal outward at back-face
	// hits is consistent across both code paths.
	if( ri.bHit )
	{
		ri.ptIntersection = ri.ray.PointAtLength( ri.range );
		GeometricUtilities::CylinderNormal( ri.ptIntersection, m_chAxis, ri.vNormal );
		ri.vGeomNormal = ri.vNormal;	// analytical surface: shading == geometric

		if( bComputeExitInfo ) {
			ri.ptExit = ri.ray.PointAtLength( ri.range2 );
			GeometricUtilities::CylinderNormal( ri.ptExit, m_chAxis, ri.vNormal2 );
			ri.vGeomNormal2 = ri.vNormal2;	// analytical: shading == geometric
		}

		// Calculate UV co-ordinates
		GeometricUtilities::CylinderTextureCoord( ri.ptIntersection, m_chAxis, m_dOVRadius, m_dAxisMin, m_dAxisMax, ri.ptCoord );
	}
}

bool CylinderGeometry::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool bHitFrontFaces, const bool bHitBackFaces ) const
{
	if( m_bCapped )
	{
		Scalar	tNear = 0.0, tFar = 0.0;
		int		surfNear = SURF_SIDE, surfFar = SURF_SIDE;
		if( IntersectCappedSolid( ray, tNear, surfNear, tFar, surfFar ) ) {
			// IntersectCappedSolid only returns candidates with t > NEARZERO.
			// Honour the front/back-face filter (IGeometry contract, and consistent
			// with the detailed IntersectRay above): occlude only if a crossing of
			// the requested facing lies within dHowFar.  Front = outward normal
			// opposes the ray (entry); back = exit.
			const Vector3& dir = ray.Dir();
			Vector3 nNear; FillSurfaceNormalUV( ray.PointAtLength( tNear ), surfNear, nNear, 0 );
			const bool frontNear = ( nNear.x*dir.x + nNear.y*dir.y + nNear.z*dir.z ) < 0.0;
			if( ( ( frontNear && bHitFrontFaces ) || ( !frontNear && bHitBackFaces ) ) && tNear <= dHowFar )
				return true;

			if( tFar > tNear ) {
				Vector3 nFar; FillSurfaceNormalUV( ray.PointAtLength( tFar ), surfFar, nFar, 0 );
				const bool frontFar = ( nFar.x*dir.x + nFar.y*dir.y + nFar.z*dir.z ) < 0.0;
				if( ( ( frontFar && bHitFrontFaces ) || ( !frontFar && bHitBackFaces ) ) && tFar <= dHowFar )
					return true;
			}
			return false;
		}
		return false;
	}

	/* IMPLEMENT THIS OPTO!
	// If the point is inside the sphere and we are to ONLY hit the front faces, then
	// we cannot possible hit a front face, so beat it!
	bool RayBeginsInCylinder = IsPointInsideCylinder( ray.origin, m_dRadius, m_ptCenter );
	if( bHitFrontFaces && !bHitBackFaces && RayBeginsInCylinder )
		return false;

	// Accordingly, if we are outside the sphere but we are not supposed to hit front faces and 
	// only back faces, then we can't hit anything!
	if( !bHitFrontFaces && bHitBackFaces && !RayBeginsInCylinder )
		return false;
		*/

	HIT	h;
	bool bHitFarSide = false;
	switch( m_chAxis )
	{
	case 'x':
		RayXCylinderIntersection( ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'y':
		RayYCylinderIntersection( ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	case 'z':
		RayZCylinderIntersection( ray, h, m_dAxisMin, m_dAxisMax, 0, 0, m_dRadius, bHitFarSide );
		break;
	}

	if( h.bHit && (h.dRange < NEARZERO || h.dRange > dHowFar) ) {
		h.bHit = false;
	}

	return h.bHit;
}

void CylinderGeometry::GenerateBoundingSphere( Point3& ptCenter, Scalar& radius ) const
{
	ptCenter = Point3( 0, 0, 0 );
	radius = m_dRadius > m_dHeight ? m_dRadius : m_dHeight;
}

BoundingBox CylinderGeometry::GenerateBoundingBox() const
{
	// Bounding box will depend on the axis of the cylinder
	Point3 ll, ur;
	switch( m_chAxis )
	{
	case 'x':
		ll = Point3( m_dAxisMin, -m_dRadius, -m_dRadius );
		ur = Point3( m_dAxisMax, m_dRadius, m_dRadius );
		break;
	case 'y':
		ll = Point3( -m_dRadius, m_dAxisMin, -m_dRadius );
		ur = Point3( m_dRadius, m_dAxisMax, m_dRadius );
		break;
	case 'z':
		ll = Point3( -m_dRadius, -m_dRadius, m_dAxisMin );
		ur = Point3( m_dRadius, m_dRadius, m_dAxisMax );
		break;
	}

	return BoundingBox( ll, ur );
}

void CylinderGeometry::UniformRandomPoint( Point3* point, Vector3* normal, Point2* coord, const Point3& prand ) const
{
	// For the capped solid, split the sample between the side wall and the two
	// caps in proportion to their areas so the point density stays uniform and
	// consistent with GetArea() (required for correct area-light / MIS weights).
	if( m_bCapped ) {
		const Scalar sideA = 2.0 * PI * m_dRadius * m_dHeight;
		const Scalar capA  = PI * m_dRadius * m_dRadius;			// per cap
		const Scalar total = sideA + 2.0 * capA;
		const Scalar sel   = prand.z * total;

		if( sel >= sideA ) {
			// A cap.  Pick which end, then sample the disk uniformly:
			//   r = R*sqrt(u),  theta = 2*pi*v   (area-uniform on the disk).
			const bool   bMaxCap = ( sel >= sideA + capA );
			const Scalar rr = m_dRadius * sqrt( prand.x );
			const Scalar th = TWO_PI * prand.y;
			const Scalar ra = rr * cos( th );
			const Scalar rb = rr * sin( th );
			const Scalar axial = bMaxCap ? m_dAxisMax : m_dAxisMin;

			Point3 pt;
			switch( m_chAxis )
			{
			case 'x': pt = Point3( axial, ra, rb ); break;
			case 'y': pt = Point3( ra, axial, rb ); break;
			case 'z':
			default:  pt = Point3( ra, rb, axial ); break;
			}

			if( point )  { *point = pt; }
			if( normal ) { FillSurfaceNormalUV( pt, bMaxCap ? SURF_CAPMAX : SURF_CAPMIN, *normal, 0 ); }
			if( coord )  {
				Vector3 tmp;
				FillSurfaceNormalUV( pt, bMaxCap ? SURF_CAPMAX : SURF_CAPMIN, tmp, coord );
			}
			return;
		}
		// else fall through to the side-wall sample below.
	}

	Point3 pt;
	GeometricUtilities::PointOnCylinder( Point2(prand.x,prand.y), m_chAxis, m_dRadius, m_dAxisMin, m_dAxisMax, pt );

	if( point ) {
		*point = pt;
	}

	if( normal ) {
		GeometricUtilities::CylinderNormal( pt, m_chAxis, *normal );
	}

	if( coord ) {
		GeometricUtilities::CylinderTextureCoord( pt, m_chAxis, m_dOVRadius, m_dAxisMin, m_dAxisMax, *coord );
	}
}

SurfaceDerivatives CylinderGeometry::ComputeSurfaceDerivatives( const Point3& objSpacePoint, const Vector3& objSpaceNormal ) const
{
	SurfaceDerivatives sd;

	const Scalar r = m_dRadius;

	// End-cap points (only possible when m_bCapped): the shading frame lies in
	// the cap plane.  A cap is identified by an axis-aligned normal (axial
	// component ~1); the side wall always has a zero axial component.  Build a
	// right-handed frame for the planar disk UV of FillSurfaceNormalUV:
	//   u = (ra/r + 1)/2,  v = (rb/r + 1)/2   =>   dP/du = 2r along +ra, dP/dv = 2r along +rb.
	if( m_bCapped ) {
		Scalar axialN = 0.0;
		switch( m_chAxis ) {
			case 'x': axialN = objSpaceNormal.x; break;
			case 'y': axialN = objSpaceNormal.y; break;
			case 'z':
			default:  axialN = objSpaceNormal.z; break;
		}

		if( fabs(axialN) > 0.5 ) {
			const Scalar twoR = 2.0 * r;
			Vector3 dpdu, dpdv;
			Scalar  ra = 0.0, rb = 0.0;
			switch( m_chAxis ) {
				case 'x':
					dpdu = Vector3( 0.0, twoR, 0.0 ); dpdv = Vector3( 0.0, 0.0, twoR );
					ra = objSpacePoint.y; rb = objSpacePoint.z;
					break;
				case 'y':
					dpdu = Vector3( twoR, 0.0, 0.0 ); dpdv = Vector3( 0.0, 0.0, twoR );
					ra = objSpacePoint.x; rb = objSpacePoint.z;
					break;
				case 'z':
				default:
					dpdu = Vector3( twoR, 0.0, 0.0 ); dpdv = Vector3( 0.0, twoR, 0.0 );
					ra = objSpacePoint.x; rb = objSpacePoint.y;
					break;
			}

			// Orient (dpdu, dpdv, n) right-handed: swap if the frame is left-handed
			// (happens on the -axis cap where the outward normal is negated).
			if( Vector3Ops::Dot( Vector3Ops::Cross( dpdu, dpdv ), objSpaceNormal ) < 0.0 ) {
				Vector3 tmp = dpdu; dpdu = dpdv; dpdv = tmp;
			}

			sd.dpdu = dpdu;
			sd.dpdv = dpdv;
			sd.dndu = Vector3( 0.0, 0.0, 0.0 );
			sd.dndv = Vector3( 0.0, 0.0, 0.0 );
			sd.uv   = Point2( 0.5*(ra*m_dOVRadius + 1.0), 0.5*(rb*m_dOVRadius + 1.0) );
			sd.valid = true;
			return sd;
		}
	}

	// Extract the two radial coordinates and the axial coordinate based on axis orientation
	Scalar radA, radB, axial;
	switch( m_chAxis )
	{
	case 'x':
		radA = objSpacePoint.y;
		radB = objSpacePoint.z;
		axial = objSpacePoint.x;
		break;
	case 'z':
		radA = objSpacePoint.x;
		radB = objSpacePoint.y;
		axial = objSpacePoint.z;
		break;
	case 'y':
	default:
		radA = objSpacePoint.x;
		radB = objSpacePoint.z;
		axial = objSpacePoint.y;
		break;
	}

	const Scalar theta = atan2( radB, radA );
	const Scalar cosT = cos(theta);
	const Scalar sinT = sin(theta);

	// Build derivatives then assign to the correct components based on axis
	// For a Y-axis cylinder:
	//   P(theta,h) = (r*cos(theta), h, r*sin(theta))
	//   dpdu = (-r*sin(theta), 0, r*cos(theta))
	//   dpdv = (0, 1, 0)
	//   N = (cos(theta), 0, sin(theta))
	//   dndu = (-sin(theta), 0, cos(theta))
	//   dndv = (0, 0, 0)
	// For other axes, permute accordingly.

	// Convention: (dpdu, dpdv, n) must be right-handed per
	// docs/GEOMETRY_DERIVATIVES.md.  Swap (theta, axial) ordering so that
	// u = axial (along the cylinder axis) and v = theta (around the axis);
	// then (dpdu × dpdv) · n > 0 at every side point.
	// Convention: (dpdu, dpdv, n) must be right-handed per
	// docs/GEOMETRY_DERIVATIVES.md.  With u = axial and v = theta, the
	// rotation direction of dpdv around the axis depends on axis choice
	// due to the axis permutation (each case's sign worked out below).
	switch( m_chAxis )
	{
	case 'x':
		// P = (h, r*cos(theta), r*sin(theta)).  v rotates CW viewed from +X
		// to make the frame right-handed with the outward normal.
		sd.dpdu = Vector3( 1.0, 0.0, 0.0 );
		sd.dpdv = Vector3( 0.0, r * sinT, -r * cosT );
		sd.dndu = Vector3( 0.0, 0.0, 0.0 );
		sd.dndv = Vector3( 0.0, sinT, -cosT );
		break;
	case 'z':
		// P = (r*cos(theta), r*sin(theta), h).  v rotates CW viewed from +Z.
		sd.dpdu = Vector3( 0.0, 0.0, 1.0 );
		sd.dpdv = Vector3( r * sinT, -r * cosT, 0.0 );
		sd.dndu = Vector3( 0.0, 0.0, 0.0 );
		sd.dndv = Vector3( sinT, -cosT, 0.0 );
		break;
	case 'y':
	default:
		// P = (r*cos(theta), h, r*sin(theta)).  v rotates CCW viewed from +Y.
		sd.dpdu = Vector3( 0.0, 1.0, 0.0 );
		sd.dpdv = Vector3( -r * sinT, 0.0, r * cosT );
		sd.dndu = Vector3( 0.0, 0.0, 0.0 );
		sd.dndv = Vector3( -sinT, 0.0, cosT );
		break;
	}

	sd.uv = Point2( axial, theta );  // matches the u↔axial, v↔theta swap
	sd.valid = true;

	return sd;
}

Scalar CylinderGeometry::GetArea( ) const
{
	const Scalar side = 2.0 * PI * m_dRadius * m_dHeight;
	if( m_bCapped ) {
		// Two end-cap disks, each of area PI r^2.
		return side + 2.0 * PI * m_dRadius * m_dRadius;
	}
	return side;
}

static const unsigned int RADIUS_ID = 100;
static const unsigned int HEIGHT_ID = 101;

IKeyframeParameter* CylinderGeometry::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	// Check the name and see if its something we recognize
	if( name == "pta" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), RADIUS_ID );
	} else if( name == "ptb" ) {
		p = new Parameter<Scalar>( atof(value.c_str()), HEIGHT_ID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void CylinderGeometry::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case RADIUS_ID:
		{
			m_dRadius = *(Scalar*)val.getValue();
		}
		break;
	case HEIGHT_ID:
		{
			m_dHeight = *(Scalar*)val.getValue();
		}
		break;
	}
}

void CylinderGeometry::RegenerateData( )
{
	m_dAxisMin = -m_dHeight/2;
	m_dAxisMax = m_dHeight/2;

	if( m_dRadius > 0 ) {
		m_dOVRadius = 1.0 / m_dRadius;
	} else {
		GlobalLog()->PrintSourceError( "CylinderGeometry:: m_dRadius is <= 0", __FILE__, __LINE__ );
	}
}

