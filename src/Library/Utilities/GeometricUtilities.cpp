//////////////////////////////////////////////////////////////////////
//
//  GeometricUtilities.cpp - Implementation of geometric utilities
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 24, 2001
//  Tabs: 4
//  Comments:  Influence by ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GeometricUtilities.h"
#include "../Utilities/OrthonormalBasis3D.h"

using namespace RISE;

#if 0

Point2 GeometricUtilities::PointOnDisk( Scalar R, const Point2& uv )
{
	Scalar r = R * sqrt(uv.x);
	Scalar t = TWO_PI * uv.y;
	return Point2( r*cos(t), r*sin(t) );
}

#else

Point2 GeometricUtilities::PointOnDisk( Scalar R, const Point2& uv )
{
	Scalar		r, t;

	// Convert the canonical random numbers to be numbers
	// in the range of [-1,1];
	Point2		p;
	p.x = uv.x * 2.0 - 1.0;
	p.y = uv.y * 2.0 - 1.0;

	Scalar&		r1 = p.x;
	Scalar&		r2 = p.y;

	if( r1 == 0.0 && r2 == 0.0 )
	{
		return Point2( 0.0, 0.0 );
	}

	// This transformation comes from Peter Shirley, and is summarized
	// nice in Philip Dutre's Global Illumination Compendium
	// page 13
	if( r1 > -r2 )
	{
		if( r1 > r2 )
		{
			r = R * r1;
			t = PI_OV_FOUR * r2 / r1;
		}
		else
		{
			r = R * r2;
			t = PI_OV_FOUR * (2 - r1/r2);
		}
	}
	else
	{
		if( r1 < r2 )
		{
			r = R * -r1;
			t = PI_OV_FOUR * (4 + r2/r1);
		}
		else
		{
			r = R * -r2;
			t = PI_OV_FOUR * (6 - r1/r2);
		}
	}

	return Point2( r*cos(t), r*sin(t) );
}

#endif

Vector3 GeometricUtilities::CreateDiffuseVector( const OrthonormalBasis3D& uvw, const Point2& p )
{
	Scalar phi = TWO_PI * p.x;
#if 1
	Scalar sint = sqrt( p.y );
	Scalar cost = sqrt( 1.0 - p.y );
#else
	Scalar theta = PI_OV_TWO * p.y;
	Scalar sint = sin( theta );
	Scalar cost = cos( theta );
#endif

	return uvw.Transform( Vector3( cos(phi)*sint, sin(phi)*sint, cost ) );
}

Vector3 GeometricUtilities::Perturb( const Vector3& vec, const Scalar down, const Scalar around )
{
	OrthonormalBasis3D uvw;

	uvw.CreateFromU( vec );

	Matrix4 rthere = uvw.GetCanonicalToBasisMatrix();
	Matrix4 rback = uvw.GetBasisToCanonicalMatrix();

	Vector3 a = Vector3Ops::Transform( rthere, vec );

	Matrix4 rx = Matrix4Ops::XRotation( around );
	Matrix4 ry = Matrix4Ops::YRotation( down );

	a = Vector3Ops::Transform( ry, a );
	a = Vector3Ops::Transform( rx, a );
	a = Vector3Ops::Transform( rback, a );
	return a;
}

Point3 GeometricUtilities::CreatePoint3FromSphericalONB( const OrthonormalBasis3D& onb, const Scalar phi, const Scalar theta )
{
	Vector3	a( cos(phi)*sin(theta), sin(phi)*sin(theta), cos(theta) );

	return Point3( 
		onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z, 
		onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z, 
		onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );
}

Point3 GeometricUtilities::CreatePoint3FromSpherical( const Scalar phi, const Scalar theta )
{
	Scalar		sint = sin(theta);
	return Point3( sint*cos(phi), 
					sint*sin(phi),
					cos(theta));
}

bool GeometricUtilities::GetSphericalFromPoint3( const Point3& pt, Scalar& phi, Scalar& theta )
{
	if( pt.x == 0 && pt.y == 0 )
	{
		theta = RISE_INFINITY;
		phi = RISE_INFINITY;
		return false;
	}

	Scalar rad = sqrt(pt.x*pt.x+pt.y*pt.y+pt.z*pt.z);	// Compute the radius, so that all points are normalized
	Scalar OVrad = 1.0 / rad;

	theta = acos(0.9999999999*pt.z*OVrad);				// Compute theta
	Scalar	sint = sin(theta);

	Scalar	inner = (pt.x*OVrad)/(1.0000000001*sint);	// Do this because of error...
	inner = sint == 0.0 ? 0.0 : inner;					// Avoid division by 0
	inner = inner > 1.0 ? 0.9999999 : inner;			// Avoids taking the acos of a > 1 number
	inner = inner <= -1.0 ? -0.9999999 : inner;			// avoids taking the acos of a < -0.999 number

	if( pt.y >= 0 ) {
		phi = acos( inner );
	} else {
		phi = TWO_PI - acos( inner );
	}

	return true;
}

bool GeometricUtilities::GetSphericalFromPoint3ONB( const Point3& pt, const OrthonormalBasis3D& onb, Scalar& phi, Scalar& theta )
{
	Point3 ptCanonical = Point3Ops::Transform( onb.GetBasisToCanonicalMatrix(), pt );
	return GetSphericalFromPoint3( ptCanonical, phi, theta );
}

Vector3 GeometricUtilities::CreatePhongVector( const OrthonormalBasis3D& onb, const Point2& p, const Scalar n )
{
	Scalar	phi = TWO_PI * p.x;
	Scalar	cost = ::pow(1.0-p.y, 1.0 / (n+1.0) );
	Scalar	sint = ::sqrt( 1.0 -cost*cost );
	Vector3	a( cos(phi)*sint, sin(phi)*sint, cost );

	return Vector3( 
		onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z, 
		onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z, 
		onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );
}

Vector3 GeometricUtilities::CreatePhongVector( const Vector3& v, const Point2& p, const Scalar n )
{
	OrthonormalBasis3D	onb;
	onb.CreateFromW( v );
	return CreatePhongVector( onb, p, n );
}

Vector3 GeometricUtilities::CreateHalfPhongVector( const OrthonormalBasis3D& onb, const Point2& p, const Scalar n )
{
	Scalar	phi = TWO_PI * p.x;
	Scalar cost = 2.0 * ::pow(1 - p.y, 2.0/(n+2.0)) - 1.0;
	Scalar sint = ::sqrt(1.0 -cost*cost);
	Vector3	 a( cos(phi)*sint, sin(phi)*sint, cost );

	return Vector3 (
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z, 
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z, 
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );
}

Vector3 GeometricUtilities::CreateAshikminShirleyAnisotropicPhongHalfVector( const OrthonormalBasis3D& onb, const Point2& p, const Scalar Nu, const Scalar Nv )
{
	Scalar	phi = 0;

	if( p.x < 0.25 )
	{
		phi = atan( sqrt((Nu+1.)/(Nv+1)) * tan(PI_OV_TWO * 4.0 * p.x) );		
	}
	else if( p.x < 0.5 )
	{
		Scalar val = 1.0 - 4*(0.5 - p.x);
		phi = atan( sqrt((Nu+1.)/(Nv+1)) * tan(PI_OV_TWO * val) );
		phi = PI - phi;
	}
	else if( p.x < 0.75 )
	{
		Scalar val = 4*(p.x - 0.5);
		phi = atan( sqrt((Nu+1.)/(Nv+1)) * tan(PI_OV_TWO * val) );
		phi += PI;
	}
	else
	{
		Scalar val = 1.0 - 4*(1.0 - p.x);
		phi = atan( sqrt((Nu+1.)/(Nv+1)) * tan(PI_OV_TWO * val) );
		phi = TWO_PI - phi;
	}

	Scalar cos_phi = cos( phi );
	Scalar sin_phi = sin( phi );
	Scalar exponent = 1.0 / (cos_phi*cos_phi*Nu + sin_phi*sin_phi*Nv + 1.0);
	Scalar cos_theta = pow((1.-p.y), (exponent));
	Scalar sin_theta = sqrt( 1.0 - cos_theta*cos_theta );

	Vector3	 a( cos(phi)*sin_theta, sin(phi)*sin_theta, cos_theta );

	return Vector3 (
		  onb.u().x*a.x + onb.v().x*a.y + onb.w().x*a.z, 
	   	  onb.u().y*a.x + onb.v().y*a.y + onb.w().y*a.z, 
		  onb.u().z*a.x + onb.v().z*a.y + onb.w().z*a.z );
}

Point3 GeometricUtilities::PointOnSphere( const Point3& c, const Scalar r, const Point2& coord )
{
	Scalar costheta = 1.0 - 2.0 * coord.x;
	Scalar sintheta = sqrt( 1.0 - costheta * costheta );
	Scalar phi = 2.0 * PI * coord.y;

	Vector3 v(	r * cos(phi) * sintheta,
				r * sin(phi) * sintheta,
				r * costheta );

	return Point3Ops::mkPoint3( c, v );
}

Point3 GeometricUtilities::PointOnEllipsoid( const Point3& c, const Vector3& r, const Point2& coord )
{
	Scalar costheta = 1.0 - 2.0 * coord.x;
	Scalar sintheta = sqrt( 1.0 - costheta * costheta );
	Scalar phi = 2.0 * PI * coord.y;

	Vector3 v(	r.x * cos(phi) * sintheta,
				r.y * sin(phi) * sintheta,
				r.z * costheta );

	return Point3Ops::mkPoint3( c, v );
}


Scalar GeometricUtilities::SphericalPatchArea( 
		const Scalar& theta_begin,
		const Scalar& theta_end,
		const Scalar& phi_begin,
		const Scalar& phi_end,
		const Scalar& radius
		)
{
	Scalar	A[4] =		 {0};
	Scalar	B[4] =		 {0};
	Scalar	C[4] =		 {0};
	Scalar	D[4] =		 {0};

	static const Scalar	PESOA[3] =
	{		0.555555555555556, 
			0.888888888888889, 
			0.555555555555556	};

	static const Scalar	XA[3] =
	{		-0.774596669241483,
			0.0,
			0.774596669241483	};

	Scalar	ae = 0.0;

	A[3] = theta_begin;
	B[3] = theta_end;

	A[2] = phi_begin;
	B[2] = phi_end;

	int i=0, j=0;

	for( i=2; i<=3; i++ ) {
		D[i] = (B[i]-A[i])/2.0;
		C[i] = (A[i]+B[i])/2.0;
	}


	for( i=0; i<=2; i++) {
		for( j=0; j<=2; j++) {
			Scalar	phi = D[2]*XA[i]+C[2];
			Scalar	the = D[3]*XA[j]+C[3];
			Scalar	ct = cos(the);
			Scalar	cp = cos(phi);
			Scalar	st = sin(the);
			Scalar	sp = sin(phi);
			Scalar	nax = (radius*radius)*(st*st)*cp;
			Scalar	nay = (radius*radius)*(st*st)*sp;
			Scalar	naz = (radius*radius)*ct*st;
			Scalar	id = sqrt((nax*nax)+(nay*nay)+(naz*naz));
			ae = ae+PESOA[i]*PESOA[j]*id*D[2]*D[3];
		}
	}

	return ae;
}

bool GeometricUtilities::IsPointInsideSphere( const Point3& point, const Scalar radius, const Point3& center )
{
	Vector3 ptNorm = Vector3Ops::mkVector3( point, center );
	if( Vector3Ops::SquaredModulus(ptNorm) - radius*radius < 0 ) {
		return true;
	}
	
	return false;
}

bool GeometricUtilities::IsPointInsideBox( const Point3& point, const Point3& ptLowerLeft, const Point3& ptUpperRight )
{
	if( point.x > ptLowerLeft.x && point.x < ptUpperRight.x &&
		point.y > ptLowerLeft.y && point.y < ptUpperRight.y &&
		point.z > ptLowerLeft.z && point.z < ptUpperRight.z ) {
		return true;
	}

	return false;
}

void GeometricUtilities::SphereTextureCoord( const Vector3& vUp, const Vector3& vForward, const Point3& ptPoint, Point2& uv )
{
	const Vector3 vNormal = Vector3Ops::Normalize( Vector3( ptPoint.x, ptPoint.y, ptPoint.z ) );
	SphereTextureCoord( vUp, vForward, vNormal, uv );
}

void GeometricUtilities::SphereTextureCoord( 
			const Vector3& vUp,									///< [in] The up vector, these vectors determine how the sphere is wrapped
			const Vector3& vForward,							///< [in] The forward vector
			const Vector3& vNormal,								///< [in] The normal to generate the texture co-ordinate from
			Point2& uv											///< [out] The generated co-ordinate
			)
{
	const Scalar fPhi = Scalar( acos( Vector3Ops::Dot(vUp, vNormal) ) );
	uv.y = fPhi / PI;

	const Scalar fTemp = Vector3Ops::Dot(vNormal, vForward) / Scalar( sin( fPhi ) );
	const Scalar fTheta = Scalar( acos( fTemp ) / (2.0*PI) );

	if( Vector3Ops::Dot( vNormal, Vector3Ops::Cross( vUp, vForward ) ) < 0.0 ) {
		uv.x = 1.0 - fTheta;
	} else {
		uv.x = fTheta;
	}
}

void GeometricUtilities::TorusTextureCoord(
	const Vector3& /*vUp*/,
	const Vector3& /*vForward*/,
	const Point3& ptPoint,
	const Vector3& /*vNormal*/,
	Point2& uv,
	const Scalar dMajorRadius,
	const Scalar dMinorRadius
)
{
	// Inverse of TorusGeometry::TessellateToMesh:
	//   pos = ( (R + r·cos(V))·cos(U),
	//            r·sin(V),
	//           (R + r·cos(V))·sin(U) )
	// with U = u·2π (ring angle around Y) and V = v·2π (tube angle).
	//
	// The pre-fix body used acos(N·vUp) and acos((point.x, point.y, 0)·vForward)
	// — both wrong: acos returns [0, π] so the full circle wrapped onto a
	// half-range, the projection used (x, y) instead of (x, z), and the
	// formula derived UV from the gradient normal rather than the position.
	// Use atan2 of the same components TessellateToMesh uses so the (u, v)
	// emitted here roundtrips through TessellateToMesh's forward formula.

	const Scalar dXZ = std::sqrt( ptPoint.x * ptPoint.x + ptPoint.z * ptPoint.z );

	// Tube angle V = atan2(y, dXZ - R).  cos(V) = (dXZ - R)/r and
	// sin(V) = y/r, so atan2 recovers V uniquely in (-π, π].
	const Scalar tubeOffset = dXZ - dMajorRadius;
	Scalar V = ( dMinorRadius > NEARZERO )
		? std::atan2( ptPoint.y, tubeOffset )
		: 0.0;
	if( V < 0.0 ) {
		V += TWO_PI;
	}
	uv.y = V / TWO_PI;

	// Ring angle U = atan2(z, x).  Both x and z carry the (R + r·cos(V))
	// scale factor identically, so the angle reduces to atan2(z, x).
	Scalar U = std::atan2( ptPoint.z, ptPoint.x );
	if( U < 0.0 ) {
		U += TWO_PI;
	}
	uv.x = U / TWO_PI;
}


void GeometricUtilities::PointOnCylinder( const Point2& can, const int chAxis, const Scalar dRadius, const Scalar dAxisMin, const Scalar dAxisMax, Point3& point )
{
	const Scalar u = can.x;
	const Scalar v = can.y;

	switch( chAxis )
	{
	case 'x':
		point.x = fabs(dAxisMax-dAxisMin)*v + dAxisMin;
		point.y = dRadius*cos(TWO_PI*u);
		point.z = dRadius*sin(TWO_PI*u);
		break;
	case 'y':
		point.z = dRadius*cos(TWO_PI*u);
		point.y = fabs(dAxisMax-dAxisMin)*v + dAxisMin;
		point.x = dRadius*sin(TWO_PI*u);   
		break;
	case 'z':
		point.x = dRadius*cos(TWO_PI*u);
		point.y = dRadius*sin(TWO_PI*u);
		point.z = fabs(dAxisMax-dAxisMin)*v + dAxisMin;
		break;
	};
}

void GeometricUtilities::CylinderTextureCoord( const Point3 point, const int chAxis, const Scalar dOVRadius, const Scalar dAxisMin, const Scalar dAxisMax, Point2& coord )
{
	(void)dOVRadius;  // No longer used — kept for ABI compatibility.

	// Inverse of CylinderGeometry::TessellateToMesh:
	//   axis 'x':  pos = (axial, r·cos(θ), r·sin(θ))
	//   axis 'y':  pos = (r·cos(θ), axial, r·sin(θ))
	//   axis 'z':  pos = (r·cos(θ), r·sin(θ), axial)
	// with θ = u·2π in [0, 2π) and axial = axisMin + v·(axisMax - axisMin).
	//
	// The pre-fix body used acos(z/r) (or x/r, y/r) with a wrap-flip on the
	// "other" radial component, which mapped the angular axis onto a
	// different parametrisation of the circle than TessellateToMesh
	// (off by a quarter turn AND inverted in direction).  Use atan2 of
	// the same two radial components TessellateToMesh uses so the (u, v)
	// emitted here roundtrips through TessellateToMesh's forward formula.
	Scalar a = 0.0;  // the radial coordinate playing the cos role
	Scalar b = 0.0;  // the radial coordinate playing the sin role
	Scalar axial = 0.0;
	switch( chAxis )
	{
	case 'x':
		a = point.y;
		b = point.z;
		axial = point.x;
		break;
	case 'y':
		a = point.x;
		b = point.z;
		axial = point.y;
		break;
	case 'z':
		a = point.x;
		b = point.y;
		axial = point.z;
		break;
	}

	Scalar theta = atan2( b, a );
	if( theta < 0.0 ) {
		theta += TWO_PI;
	}
	coord.x = theta / TWO_PI;

	const Scalar height = dAxisMax - dAxisMin;
	coord.y = (height > NEARZERO) ? (axial - dAxisMin) / height : 0.0;
}

void GeometricUtilities::CylinderNormal( const Point3 point, const int chAxis, Vector3& normal )
{
	switch( chAxis )
	{
	case 'x':
	default:
		normal = Vector3( 0.0, point.y, point.z );
		break;
	case 'y':
		normal = Vector3( point.x, 0.0, point.z );
		break;
	case 'z':
		normal = Vector3( point.x, point.y, 0.0 );
		break;
	};

	normal = Vector3Ops::Normalize(normal);
}

void GeometricUtilities::PointOnTriangle( 
	Vertex* point,									///< [out] Resultant point on triangle
	Normal* normal,									///< [out] Resultant normal on triangle
	TexCoord* coord,								///< [out] Resultant texture coord on triangle
	const Triangle& t,								///< [in] The incoming triangle
	const Scalar a_,								///< [in] Distance in one direction
	const Scalar b_									///< [in] Distance in other direction
	)
{
	Scalar a =  sqrt( 1.0 - a_ );
	Scalar alpha = 1.0 - a;
	Scalar beta = a * b_;

	if( point ) {
		*point = Point3Ops::WeightedAverage3( t.vertices[1], t.vertices[2], t.vertices[0], alpha, beta );
	}

	if( normal ) {
		*normal = Vector3Ops::WeightedAverage3( t.normals[1], t.normals[2], t.normals[0], alpha, beta );
	}

	if( coord ) {
		*coord = Point2Ops::WeightedAverage3( t.coords[1], t.coords[2], t.coords[0], alpha, beta );
	}
}

void GeometricUtilities::PointOnTriangle( 
	Vertex* point,									///< [out] Resultant point on triangle
	Normal* normal,									///< [out] Resultant normal on triangle
	TexCoord* coord,								///< [out] Resultant texture coord on triangle
	const PointerTriangle& t,						///< [in] The incoming pointer triangle
	const Scalar a_,								///< [in] Distance in one direction
	const Scalar b_									///< [in] Distance in other direction
	)
{
	Scalar a =  sqrt( 1.0 - a_ );
	Scalar alpha = 1.0 - a;
	Scalar beta = a * b_;

	if( point ) {
		*point = Point3Ops::WeightedAverage3( *t.pVertices[1], *t.pVertices[2], *t.pVertices[0], alpha, beta );
	}

	if( normal && t.pNormals[1] && t.pNormals[2] && t.pNormals[0] ) {
		*normal = Vector3Ops::WeightedAverage3( *t.pNormals[1], *t.pNormals[2], *t.pNormals[0], alpha, beta );
	} else if( normal ) {
		*normal = Vector3Ops::Normalize(Vector3Ops::Cross( Vector3Ops::mkVector3( *t.pVertices[1], *t.pVertices[0] ), Vector3Ops::mkVector3( *t.pVertices[2], *t.pVertices[0] ) ));
	}

	if( coord && t.pCoords[1] && t.pCoords[2] && t.pCoords[0] ) {
		*coord = Point2Ops::WeightedAverage3( *t.pCoords[1], *t.pCoords[2], *t.pCoords[0], alpha, beta );
	}
}

//! Generates axis aligned bounding box of the given bezier patch
BoundingBox GeometricUtilities::BezierPatchBoundingBox(
	const BezierPatch& patch						///< [in] The bezier patch
	)
{
	Point3 ll = Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3 ur = Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );

	for( int j=0; j<4; j++ ) {
		for( int k=0; k<4; k++ ) {
			const Point3& pt = patch.c[j].pts[k];
			if( pt.x < ll.x ) ll.x = pt.x;
			if( pt.y < ll.y ) ll.y = pt.y;
			if( pt.z < ll.z ) ll.z = pt.z;
			if( pt.x > ur.x ) ur.x = pt.x;
			if( pt.y > ur.y ) ur.y = pt.y;
			if( pt.z > ur.z ) ur.z = pt.z;
		}
	}

	return BoundingBox( ll, ur );
}

//! Generates axis aligned bounding box of the given bilinear patch
BoundingBox GeometricUtilities::BilinearPatchBoundingBox(
	const BilinearPatch& patch						///< [in] The bilinear patch
	)
{
	Point3 ll = Point3( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
	Point3 ur = Point3( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );

	for( int j=0; j<4; j++ ) {
		const Point3& pt = patch.pts[j];
		if( pt.x < ll.x ) ll.x = pt.x;
		if( pt.y < ll.y ) ll.y = pt.y;
		if( pt.z < ll.z ) ll.z = pt.z;
		if( pt.x > ur.x ) ur.x = pt.x;
		if( pt.y > ur.y ) ur.y = pt.y;
		if( pt.z > ur.z ) ur.z = pt.z;
	}

	return BoundingBox( ll, ur );
}

//! Evaluates a bilinear patch for the given u and v
Point3 GeometricUtilities::EvaluateBilinearPatchAt( 
	const BilinearPatch& patch,
	const Scalar u, 
	const Scalar v 
	)
{
	Point3 ret;
	ret.x = ( ( (1.0 - u) * (1.0 - v) * patch.pts[0].x +
			(1.0 - u) *        v  * patch.pts[1].x + 
			u  * (1.0 - v) * patch.pts[2].x +
			u  *        v  * patch.pts[3].x));
	ret.y = (  ( (1.0 - u) * (1.0 - v) * patch.pts[0].y +
			(1.0 - u) *        v  * patch.pts[1].y + 
			u  * (1.0 - v) * patch.pts[2].y +
			u  *        v  * patch.pts[3].y));
	ret.z = (  ( (1.0 - u) * (1.0 - v) * patch.pts[0].z +
			(1.0 - u) *        v  * patch.pts[1].z + 
			u  * (1.0 - v) * patch.pts[2].z +
			u  *        v  * patch.pts[3].z));

	return ret;
}

inline Vector3 BilinearTanU(
	const BilinearPatch& patch,
	const Scalar v )
{
	return Vector3( 
		( 1.0 - v ) * (patch.pts[2].x - patch.pts[0].x) + v * (patch.pts[3].x - patch.pts[1].x),
		( 1.0 - v ) * (patch.pts[2].y - patch.pts[0].y) + v * (patch.pts[3].y - patch.pts[1].y),
		( 1.0 - v ) * (patch.pts[2].z - patch.pts[0].z) + v * (patch.pts[3].z - patch.pts[1].z)
		);
}


inline Vector3 BilinearTanV(
	const BilinearPatch& patch,
	const Scalar u )
{
	return Vector3( 
		( 1.0 - u ) * (patch.pts[1].x - patch.pts[0].x) + u * (patch.pts[3].x - patch.pts[2].x),
		( 1.0 - u ) * (patch.pts[1].y - patch.pts[0].y) + u * (patch.pts[3].y - patch.pts[2].y),
		( 1.0 - u ) * (patch.pts[1].z - patch.pts[0].z) + u * (patch.pts[3].z - patch.pts[2].z)
		);
}


//! Finds the normal of a bilinear patch at the given co-ordinates
Vector3 GeometricUtilities::BilinearPatchNormalAt(
	const BilinearPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	return Vector3Ops::Cross( BilinearTanU(patch,v), BilinearTanV(patch,u) );
}

//
// Bicubic Bezier patch helpers
//
// Bernstein cubic basis:
//   B_0(t) = (1-t)^3,  B_1(t) = 3t(1-t)^2,  B_2(t) = 3t^2(1-t),  B_3(t) = t^3
// Derivative:
//   B'(t) = 3[(B_0', B_1', B_2', B_3')_cubic] — but closed form is simpler:
//   dB_0/dt = -3(1-t)^2,  dB_3/dt = 3t^2
//   dB_1/dt = 3(1-t)^2 - 6t(1-t) = 3(1-t)(1-3t)
//   dB_2/dt = 6t(1-t) - 3t^2     = 3t(2-3t)
//
namespace {
	inline void BernsteinCubic( const Scalar t, Scalar (&B)[4] )
	{
		const Scalar mt  = 1.0 - t;
		const Scalar mt2 = mt*mt;
		const Scalar t2  = t*t;
		B[0] = mt2 * mt;
		B[1] = 3.0 * mt2 * t;
		B[2] = 3.0 * mt  * t2;
		B[3] = t2  * t;
	}

	inline void BernsteinCubicDeriv( const Scalar t, Scalar (&dB)[4] )
	{
		const Scalar mt  = 1.0 - t;
		const Scalar mt2 = mt*mt;
		const Scalar t2  = t*t;
		dB[0] = -3.0 * mt2;
		dB[1] =  3.0 * mt2 - 6.0 * mt * t;   // 3(1-t)(1-3t)
		dB[2] =  6.0 * mt * t - 3.0 * t2;    // 3t(2-3t)
		dB[3] =  3.0 * t2;
	}
}

//! Evaluates a bicubic Bezier patch via direct Bernstein sum.
//! Slightly cheaper than nested de Casteljau and numerically adequate
//! in double precision for patches of bounded extent.
Point3 GeometricUtilities::EvaluateBezierPatchAt(
	const BezierPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	Scalar Bu[4], Bv[4];
	BernsteinCubic( u, Bu );
	BernsteinCubic( v, Bv );

	Scalar x = 0.0, y = 0.0, z = 0.0;
	for( int i = 0; i < 4; i++ ) {
		for( int j = 0; j < 4; j++ ) {
			const Scalar w = Bu[i] * Bv[j];
			const Point3& p = patch.c[i].pts[j];
			x += w * p.x;
			y += w * p.y;
			z += w * p.z;
		}
	}
	return Point3( x, y, z );
}

Vector3 GeometricUtilities::BezierPatchTangentU(
	const BezierPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	Scalar dBu[4], Bv[4];
	BernsteinCubicDeriv( u, dBu );
	BernsteinCubic( v, Bv );

	Scalar x = 0.0, y = 0.0, z = 0.0;
	for( int i = 0; i < 4; i++ ) {
		for( int j = 0; j < 4; j++ ) {
			const Scalar w = dBu[i] * Bv[j];
			const Point3& p = patch.c[i].pts[j];
			x += w * p.x;
			y += w * p.y;
			z += w * p.z;
		}
	}
	return Vector3( x, y, z );
}

Vector3 GeometricUtilities::BezierPatchTangentV(
	const BezierPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	Scalar Bu[4], dBv[4];
	BernsteinCubic( u, Bu );
	BernsteinCubicDeriv( v, dBv );

	Scalar x = 0.0, y = 0.0, z = 0.0;
	for( int i = 0; i < 4; i++ ) {
		for( int j = 0; j < 4; j++ ) {
			const Scalar w = Bu[i] * dBv[j];
			const Point3& p = patch.c[i].pts[j];
			x += w * p.x;
			y += w * p.y;
			z += w * p.z;
		}
	}
	return Vector3( x, y, z );
}

Vector3 GeometricUtilities::BezierPatchNormalAt(
	const BezierPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	return Vector3Ops::Cross(
		BezierPatchTangentU( patch, u, v ),
		BezierPatchTangentV( patch, u, v ) );
}

char GeometricUtilities::WhichSideOfPlane( 
	const Plane& p,
	const PointerTriangle& t
	)
{
	const Scalar d0 = p.Distance( *(t.pVertices[0]) );

	if( d0 < NEARZERO ) {
		const Scalar d1 = p.Distance( *(t.pVertices[1]) );

		if( d1 < NEARZERO ) {
			const Scalar d2 = p.Distance( *(t.pVertices[2]) );

			if( d2 < NEARZERO ) {
				return 0;
			} else {
				return 2;
			}
		} else {
			return 2;
		}

	} else if( d0 > NEARZERO ) {
		const Scalar d1 = p.Distance( *(t.pVertices[1]) );

		if( d1 > NEARZERO ) {
			const Scalar d2 = p.Distance( *(t.pVertices[2]) );

			if( d2 > NEARZERO ) {
				return 1;
			} else {
				return 2;
			}
		} else {
			return 2;
		}

	} else {
		return 2;
	}
}

char GeometricUtilities::WhichSideOfPlane( 
	const Plane& p,
	const Triangle& t
	)
{
	const Scalar d0 = p.Distance( t.vertices[0] );

	if( d0 < NEARZERO ) {
		const Scalar d1 = p.Distance( t.vertices[1] );

		if( d1 < NEARZERO ) {
			const Scalar d2 = p.Distance( t.vertices[2] );

			if( d2 < NEARZERO ) {
				return 0;
			} else {
				return 2;
			}
		} else {
			return 2;
		}

	} else if( d0 > NEARZERO ) {
		const Scalar d1 = p.Distance( t.vertices[1] );

		if( d1 > NEARZERO ) {
			const Scalar d2 = p.Distance( t.vertices[2] );

			if( d2 > NEARZERO ) {
				return 1;
			} else {
				return 2;
			}
		} else {
			return 2;
		}

	} else {
		return 2;
	}
}

char GeometricUtilities::WhichSideOfPlane( 
	const Plane& p,										///< [in] The plane
	const BoundingBox& bb								///< [in] The bounding box to check
	)
{
	const Scalar dLL = p.Distance( bb.ll );

	if( dLL < NEARZERO ) {
		const Scalar dUR = p.Distance( bb.ur );

		if( dUR < NEARZERO ) {
			return 0;
		} else {
			return 2;
		}

	} else if( dLL > NEARZERO ) {
		const Scalar dUR = p.Distance( bb.ur );

		if( dUR > NEARZERO ) {
			return 1;
		} else {
			return 2;
		}

	} else {
		return 0;
	}
}
