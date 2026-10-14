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
#include "../Functions/Polynomial.h"
#include "../Interfaces/ILog.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

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

Vector3 GeometricUtilities::PerturbClipped(
	const Vector3& vec, const Scalar down, const Vector3& clipN,
	const Scalar u, Scalar* outHalfArc )
{
	// See the header for the derivation and the three preconditions.  This
	// is DL-68's azimuth-arc construction with the polar angle supplied by
	// the caller instead of drawn from a cos^N marginal.
	//
	// Review P3: `clipN` MUST be unit.  `cosPhiRaw` is read as a cosine
	// below, so a non-unit `clipN` scales it -- and with |clipN| > 1 the
	// `r_min(1, .)` clamp then reports `cosPhi == 1`, `sinPhi == 0`,
	// `denom == 0`, and the clip is SILENTLY DISABLED (`half` stays PI and
	// the full circle is drawn).  That is a wrong sample, not a loud one,
	// so normalize and say so -- the same treatment
	// `Optics::CalculateRefractedRay` gives its own non-unit inputs.
	Vector3 clipUnit = clipN;
	{
		const Scalar len2 = Vector3Ops::SquaredModulus( clipN );
		if( len2 <= Scalar(1e-24) ) {
			GlobalLog()->PrintEasyError(
				"GeometricUtilities::PerturbClipped:: degenerate clip normal "
				"(|clipN| ~ 0); returning the axis unperturbed." );
			if( outHalfArc ) *outHalfArc = 0;
			return vec;
		}
		if( fabs( len2 - Scalar(1) ) > Scalar(1e-12) ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"GeometricUtilities::PerturbClipped:: non-unit clip normal passed in "
				"(|clipN|=%f), normalizing", (double)sqrt(len2) );
			clipUnit = clipUnit * ( Scalar(1) / sqrt(len2) );
		}
	}

	const Scalar cosPhiRaw = Vector3Ops::Dot( vec, clipUnit );
	if( cosPhiRaw < Scalar(0) ) {
		// Fail loudly rather than manufacture a direction from a corrupted
		// frame -- the same choice TranslucentSPFDetail::SampleClippedPhong
		// made after its own review (a masking clamp there produced a
		// non-unit vector).  Every caller orients first, so this is
		// unreachable from production.
		GlobalLog()->PrintEasyError(
			"GeometricUtilities::PerturbClipped:: precondition violated -- "
			"the axis is not inside the clip half-space (Dot(vec,clipN) < 0); "
			"returning the axis unperturbed." );
		if( outHalfArc ) *outHalfArc = 0;
		return vec;
	}

	const Scalar cosPhi   = r_min( Scalar(1), cosPhiRaw );
	const Scalar cosTheta = cos( down );
	const Scalar sinTheta = sin( down );

	// Tangential component of the clip normal in the lobe's frame.
	Vector3 uAxis = clipUnit - cosPhi*vec;
	const Scalar uLen2 = Vector3Ops::SquaredModulus( uAxis );
	if( uLen2 <= Scalar(1e-12) ) {
		// clipN parallel to the axis: the clip is inactive, the whole cone
		// is valid.  Reproduce the unclipped draw EXACTLY (same Perturb
		// call, same azimuth convention) so an untilted surface is
		// bit-for-bit unchanged.
		if( outHalfArc ) *outHalfArc = PI;
		return GeometricUtilities::Perturb( vec, down, TWO_PI * u );
	}
	uAxis = uAxis * ( Scalar(1) / sqrt(uLen2) );
	const Vector3 vAxis = Vector3Ops::Cross( vec, uAxis );

	const Scalar sinPhi = sqrt( r_max( Scalar(0), Scalar(1) - cosPhi*cosPhi ) );

	// Half-width of the valid azimuth arc, written as a ratio comparison
	// rather than cot(theta)*cot(phi) so that theta -> 0 (cot -> infinity)
	// needs no special case: the "whole circle" branch is exactly
	// `num >= denom`.
	Scalar half = PI;
	const Scalar denom = sinTheta * sinPhi;
	const Scalar num   = cosTheta * cosPhi;
	if( num < denom ) {
		// Review P3: `-num/denom` can exceed 1 when `down > PI/2` -- then
		// `cosTheta < 0`, `num < 0`, and a cone that is entirely OUTSIDE
		// the half-space has no valid azimuth at all.  `acos` of that
		// argument is NaN, and the NaN propagates straight into the
		// returned DIRECTION.  Production never reaches it (every caller
		// gates `down` below PI/2, and `down` is a polar perturbation
		// angle), but a silent NaN direction is not an acceptable failure
		// mode for a shared utility.  Detect the empty arc and fail the
		// same way as a violated axis precondition.
		const Scalar arg = -num/denom;
		if( arg > Scalar(1) ) {
			GlobalLog()->PrintEasyError(
				"GeometricUtilities::PerturbClipped:: the valid azimuth arc is EMPTY "
				"(down > PI/2 with the whole cone outside the clip half-space); "
				"returning the axis unperturbed." );
			if( outHalfArc ) *outHalfArc = 0;
			return vec;
		}
		half = acos( r_max( Scalar(-1), arg ) );
	}
	if( outHalfArc ) *outHalfArc = half;

	if( half >= PI ) {
		// Same bit-for-bit reproduction as above: with the clip inactive at
		// this particular theta the arc IS the full circle, so keep the
		// pre-existing draw rather than an equivalent-but-different one.
		return GeometricUtilities::Perturb( vec, down, TWO_PI * u );
	}

	const Scalar psi = ( Scalar(2)*u - Scalar(1) ) * half;
	return uAxis*(sinTheta*cos(psi)) + vAxis*(sinTheta*sin(psi)) + vec*cosTheta;
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

Vector3 GeometricUtilities::BilinearPatchTangentU( const BilinearPatch& patch, const Scalar v )
{
	return BilinearTanU( patch, v );
}

Vector3 GeometricUtilities::BilinearPatchTangentV( const BilinearPatch& patch, const Scalar u )
{
	return BilinearTanV( patch, u );
}

Vector3 GeometricUtilities::BilinearPatchSecondDerivUV( const BilinearPatch& patch )
{
	// d/dv of BilinearTanU(patch, v) = (1-v)*(pts[2]-pts[0]) + v*(pts[3]-pts[1])
	// is -(pts[2]-pts[0]) + (pts[3]-pts[1]) = pts[0] - pts[1] - pts[2] + pts[3]:
	// the "saddle term" -- zero for a planar/parallelogram patch, constant
	// (independent of u, v) in general.  Equivalently d/du of
	// BilinearTanV(patch, u); both give the same vector by Clairaut's
	// theorem, which is exactly why a bilinear surface's shape operator only
	// ever needs this ONE second-derivative quantity (d2P/du2 = d2P/dv2 = 0
	// identically).
	return Vector3(
		patch.pts[0].x - patch.pts[1].x - patch.pts[2].x + patch.pts[3].x,
		patch.pts[0].y - patch.pts[1].y - patch.pts[2].y + patch.pts[3].y,
		patch.pts[0].z - patch.pts[1].z - patch.pts[2].z + patch.pts[3].z );
}

// ============================================================================
// Convention-agnostic bilinear-surface utilities — canonical (c00, c10, c11, c01)
// layout (i.e. corners at (u, v) = (0, 0), (1, 0), (1, 1), (0, 1)).
//
// Surface:
//   pos(u, v) = c00·(1-u)(1-v) + c10·u(1-v) + c11·uv + c01·(1-u)v
//             = A + B·u + C·v + D·u·v
// where
//   A = c00,  B = c10 - c00,  C = c01 - c00,
//   D = c11 - c10 - c01 + c00   (saddle term; zero for parallelograms)
// ============================================================================

Point3 GeometricUtilities::BilinearForward(
	const Point3& c00, const Point3& c10,
	const Point3& c11, const Point3& c01,
	const Scalar u, const Scalar v )
{
	const Scalar w00 = (1.0 - u) * (1.0 - v);
	const Scalar w10 =        u  * (1.0 - v);
	const Scalar w11 =        u  *        v;
	const Scalar w01 = (1.0 - u) *        v;
	return Point3(
		w00 * c00.x + w10 * c10.x + w11 * c11.x + w01 * c01.x,
		w00 * c00.y + w10 * c10.y + w11 * c11.y + w01 * c01.y,
		w00 * c00.z + w10 * c10.z + w11 * c11.z + w01 * c01.z );
}

Vector3 GeometricUtilities::BilinearTangentU(
	const Point3& c00, const Point3& c10,
	const Point3& c11, const Point3& c01,
	const Scalar v )
{
	// dpos/du = (c10 - c00)·(1 - v) + (c11 - c01)·v
	return Vector3(
		(1.0 - v) * (c10.x - c00.x) + v * (c11.x - c01.x),
		(1.0 - v) * (c10.y - c00.y) + v * (c11.y - c01.y),
		(1.0 - v) * (c10.z - c00.z) + v * (c11.z - c01.z) );
}

Vector3 GeometricUtilities::BilinearTangentV(
	const Point3& c00, const Point3& c10,
	const Point3& c11, const Point3& c01,
	const Scalar u )
{
	// dpos/dv = (c01 - c00)·(1 - u) + (c11 - c10)·u
	return Vector3(
		(1.0 - u) * (c01.x - c00.x) + u * (c11.x - c10.x),
		(1.0 - u) * (c01.y - c00.y) + u * (c11.y - c10.y),
		(1.0 - u) * (c01.z - c00.z) + u * (c11.z - c10.z) );
}

Scalar GeometricUtilities::AccumulateBilinearAreaCDF(
	const Point3& c00, const Point3& c10,
	const Point3& c11, const Point3& c01,
	const unsigned int nCells,
	const Scalar runningTotal,
	std::vector<Scalar>& cdfOut )
{
	// Midpoint quadrature of A = integral_{[0,1]^2} |dpdu x dpdv| du dv.
	// One cell-centre evaluation per cell; the cell's area contribution is
	// J(centre) * (1/nCells)^2.  Exact for parallelograms at any nCells
	// (J is constant), and O(1/nCells^2) for twisted quads.
	const unsigned int n = ( nCells < 1 ) ? 1 : nCells;
	const Scalar cellUV = Scalar( 1.0 ) / Scalar( n );
	const Scalar cellUVArea = cellUV * cellUV;

	Scalar running = runningTotal;
	Scalar patchArea = 0;

	for( unsigned int iv = 0; iv < n; iv++ )
	{
		const Scalar v = ( Scalar( iv ) + Scalar( 0.5 ) ) * cellUV;
		for( unsigned int iu = 0; iu < n; iu++ )
		{
			const Scalar u = ( Scalar( iu ) + Scalar( 0.5 ) ) * cellUV;
			const Vector3 Tu = BilinearTangentU( c00, c10, c11, c01, v );
			const Vector3 Tv = BilinearTangentV( c00, c10, c11, c01, u );
			const Scalar J = Vector3Ops::Magnitude( Vector3Ops::Cross( Tu, Tv ) );
			const Scalar cellArea = J * cellUVArea;
			patchArea += cellArea;
			running += cellArea;
			cdfOut.push_back( running );
		}
	}

	return patchArea;
}

void GeometricUtilities::BilinearCellToUV(
	const unsigned int cellIndex,
	const unsigned int nCells,
	const Scalar rU, const Scalar rV,
	Scalar& outU, Scalar& outV )
{
	const unsigned int n = ( nCells < 1 ) ? 1 : nCells;
	const Scalar cellUV = Scalar( 1.0 ) / Scalar( n );

	const unsigned int iu = cellIndex % n;
	const unsigned int iv = ( cellIndex / n ) % n;

	Scalar fu = rU; if( fu < 0 ) fu = 0; else if( fu > 1 ) fu = 1;
	Scalar fv = rV; if( fv < 0 ) fv = 0; else if( fv > 1 ) fv = 1;

	// Uniform WITHIN the cell's (u, v) sub-square.  The density this induces
	// in surface-area measure is J(centre)/(A_total * J(u,v)), which equals
	// 1/A_total exactly wherever J is locally constant -- everywhere for a
	// parallelogram, and to O(intra-cell J variation) otherwise.  Crucially
	// it integrates to exactly 1 over the surface for ANY nCells, so the
	// estimator remains a proper density; nCells trades only geometric-area
	// accuracy, never normalization.
	outU = ( Scalar( iu ) + fu ) * cellUV;
	outV = ( Scalar( iv ) + fv ) * cellUV;
}

namespace {

inline Scalar AbsScalar( Scalar a ) { return a < 0 ? -a : a; }

}  // anonymous namespace

bool GeometricUtilities::BilinearInverse(
	const Point3& c00, const Point3& c10,
	const Point3& c11, const Point3& c01,
	const Point3& P,
	Scalar& uOut, Scalar& vOut,
	const Scalar tolerance )
{
	// Substitution coefficients of the bilinear form pos = A + B·u + C·v + D·u·v.
	const Vector3 A( c00.x, c00.y, c00.z );
	const Vector3 B( c10.x - c00.x, c10.y - c00.y, c10.z - c00.z );
	const Vector3 C( c01.x - c00.x, c01.y - c00.y, c01.z - c00.z );
	const Vector3 D(
		c11.x - c10.x - c01.x + c00.x,
		c11.y - c10.y - c01.y + c00.y,
		c11.z - c10.z - c01.z + c00.z );

	const Vector3 Q(
		P.x - A.x,
		P.y - A.y,
		P.z - A.z );

	// Choose 2 axes (i, j) for the solve.  We want (i, j) most aligned with
	// the surface — equivalently, exclude the axis k whose component of the
	// patch normal at the centre is largest.
	const Vector3 dpdu_centre(
		B.x + 0.5 * D.x,
		B.y + 0.5 * D.y,
		B.z + 0.5 * D.z );
	const Vector3 dpdv_centre(
		C.x + 0.5 * D.x,
		C.y + 0.5 * D.y,
		C.z + 0.5 * D.z );
	const Vector3 nC = Vector3Ops::Cross( dpdu_centre, dpdv_centre );

	int kSkip = 2;  // default: skip Z
	{
		const Scalar ax = AbsScalar( nC.x );
		const Scalar ay = AbsScalar( nC.y );
		const Scalar az = AbsScalar( nC.z );
		if( ax >= ay && ax >= az ) kSkip = 0;
		else if( ay >= az )         kSkip = 1;
		else                        kSkip = 2;
	}
	int i = (kSkip == 0) ? 1 : 0;
	int j = (kSkip == 2) ? 1 : 2;
	if( j == i ) j = (i == 0) ? 1 : 0;  // pathological guard

	auto axisAt = []( const Vector3& v, int idx ) -> Scalar {
		return idx == 0 ? v.x : (idx == 1 ? v.y : v.z);
	};

	const Scalar Bi = axisAt( B, i ), Bj = axisAt( B, j );
	const Scalar Ci = axisAt( C, i ), Cj = axisAt( C, j );
	const Scalar Di = axisAt( D, i ), Dj = axisAt( D, j );
	const Scalar Qi = axisAt( Q, i ), Qj = axisAt( Q, j );

	// Quadratic in v: α·v² + β·v + γ = 0.
	const Scalar alpha = Cj * Di - Ci * Dj;
	const Scalar beta  = (Cj * Bi - Ci * Bj) + (Dj * Qi - Di * Qj);
	const Scalar gamma = Bj * Qi - Bi * Qj;

	const Scalar coeff[3] = { alpha, beta, gamma };
	Scalar sols[2] = { 0.0, 0.0 };
	const Scalar bound = 1e-4;  // accept just outside [0, 1] for numeric slack
	const int nSol = Polynomial::SolveQuadricWithinRange(
		coeff, sols, -bound, 1.0 + bound );

	if( nSol == 0 ) {
		// No v in [0, 1] satisfies the quadratic.  Either the patch is
		// degenerate or P is off-surface.
		uOut = 0.0;
		vOut = 0.0;
		return false;
	}

	// Patch extent for tolerance scaling.
	const Scalar extent = std::sqrt(
		std::max( Vector3Ops::SquaredModulus( B ),
		std::max( Vector3Ops::SquaredModulus( C ),
		          Vector3Ops::SquaredModulus( D ) ) ) );
	const Scalar absTol = tolerance * std::max( extent, Scalar(1.0) );

	bool found = false;
	// Sentinel: any actual surface residual is finite and far below this.
	// Avoid std::numeric_limits<Scalar>::infinity() because the pre-2026-07-29 macOS build was
	// compiled with -ffinite-math-only / -fno-honor-infinities.
	Scalar bestResidual = RISE_INFINITY;
	Scalar bestU = 0.0, bestV = 0.0;

	for( int s = 0; s < nSol; ++s ) {
		const Scalar v = sols[s];

		// Two extraction formulas for u — pick the one with larger |denom|.
		const Scalar denomI = Bi + Di * v;
		const Scalar denomJ = Bj + Dj * v;
		Scalar u = 0.0;
		if( AbsScalar( denomI ) >= AbsScalar( denomJ ) ) {
			if( AbsScalar( denomI ) < NEARZERO ) continue;
			u = (Qi - Ci * v) / denomI;
		} else {
			if( AbsScalar( denomJ ) < NEARZERO ) continue;
			u = (Qj - Cj * v) / denomJ;
		}

		if( u < -bound || u > 1.0 + bound ) continue;

		// Verify all three axis residuals (the third is the redundancy check).
		const Vector3 reconstructed(
			A.x + B.x * u + C.x * v + D.x * u * v,
			A.y + B.y * u + C.y * v + D.y * u * v,
			A.z + B.z * u + C.z * v + D.z * u * v );
		const Scalar residual = std::sqrt(
			(reconstructed.x - P.x) * (reconstructed.x - P.x) +
			(reconstructed.y - P.y) * (reconstructed.y - P.y) +
			(reconstructed.z - P.z) * (reconstructed.z - P.z) );

		if( residual < bestResidual ) {
			bestResidual = residual;
			bestU = u;
			bestV = v;
			found = true;
		}
	}

	if( !found || bestResidual > absTol ) {
		uOut = 0.0;
		vOut = 0.0;
		return false;
	}

	// Clamp tiny epsilon excursions to [0, 1].
	if( bestU < 0.0 ) bestU = 0.0;
	if( bestU > 1.0 ) bestU = 1.0;
	if( bestV < 0.0 ) bestV = 0.0;
	if( bestV > 1.0 ) bestV = 1.0;

	uOut = bestU;
	vOut = bestV;
	return true;
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

	//! DL-20: second derivatives of the cubic Bernstein basis, obtained by
	//! differentiating BernsteinCubicDeriv's four expressions once more:
	//!   B0''(t) = 6(1-t)          B1''(t) = 18t-12
	//!   B2''(t) = 6-18t           B3''(t) = 6t
	//! (sums to 0 for any t, as it must -- the basis sums to the constant 1).
	inline void BernsteinCubicSecondDeriv( const Scalar t, Scalar (&ddB)[4] )
	{
		const Scalar mt = 1.0 - t;
		ddB[0] =  6.0 * mt;
		ddB[1] = 18.0 * t - 12.0;
		ddB[2] =  6.0 - 18.0 * t;
		ddB[3] =  6.0 * t;
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

Vector3 GeometricUtilities::BezierPatchSecondDerivUU(
	const BezierPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	Scalar ddBu[4], Bv[4];
	BernsteinCubicSecondDeriv( u, ddBu );
	BernsteinCubic( v, Bv );

	Scalar x = 0.0, y = 0.0, z = 0.0;
	for( int i = 0; i < 4; i++ ) {
		for( int j = 0; j < 4; j++ ) {
			const Scalar w = ddBu[i] * Bv[j];
			const Point3& p = patch.c[i].pts[j];
			x += w * p.x;
			y += w * p.y;
			z += w * p.z;
		}
	}
	return Vector3( x, y, z );
}

Vector3 GeometricUtilities::BezierPatchSecondDerivUV(
	const BezierPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	Scalar dBu[4], dBv[4];
	BernsteinCubicDeriv( u, dBu );
	BernsteinCubicDeriv( v, dBv );

	Scalar x = 0.0, y = 0.0, z = 0.0;
	for( int i = 0; i < 4; i++ ) {
		for( int j = 0; j < 4; j++ ) {
			const Scalar w = dBu[i] * dBv[j];
			const Point3& p = patch.c[i].pts[j];
			x += w * p.x;
			y += w * p.y;
			z += w * p.z;
		}
	}
	return Vector3( x, y, z );
}

Vector3 GeometricUtilities::BezierPatchSecondDerivVV(
	const BezierPatch& patch,
	const Scalar u,
	const Scalar v
	)
{
	Scalar Bu[4], ddBv[4];
	BernsteinCubic( u, Bu );
	BernsteinCubicSecondDeriv( v, ddBv );

	Scalar x = 0.0, y = 0.0, z = 0.0;
	for( int i = 0; i < 4; i++ ) {
		for( int j = 0; j < 4; j++ ) {
			const Scalar w = Bu[i] * ddBv[j];
			const Point3& p = patch.c[i].pts[j];
			x += w * p.x;
			y += w * p.y;
			z += w * p.z;
		}
	}
	return Vector3( x, y, z );
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

//! (Moved unchanged from TriangleMeshGeometryIndexed.cpp, DL-382.)
//! DL-143.  Welds `points` to a shared id per PHYSICAL corner, so
//! that watertightness (and anything else that needs "is this the
//! same vertex") can be decided by position rather than by which
//! array slot a per-corner import/tessellation pipeline happened to
//! allocate.  `outWeldedId[i]` is the welded id for `points[i]`;
//! two positions land in the same id iff they are within `eps` of
//! each other under the grid below.
//!
//! Tolerance: `eps = max(1e-9, 1e-6 * bboxDiagonal)`, i.e. relative
//! to THIS mesh's own scale, not an absolute constant -- an absolute
//! epsilon would either be too coarse for a millimetre-scale asset
//! or too fine to absorb import float noise on a kilometre-scale
//! one.  A cell size of `eps` with a 3x3x3 neighbour-cell scan finds
//! every prior welded point within `eps` regardless of which side of
//! a cell boundary two near-duplicate positions fall on; an exact
//! (bit-identical) duplicate -- the common case, both for a
//! deliberately shared corner and for this engine's own
//! `TessellateToMesh` producers, which copy the source position
//! verbatim rather than recomputing it -- is necessarily caught by
//! this same scan (distance 0 <= eps), so no separate exact-match
//! fast path is needed for correctness; it would only change the
//! constant factor, not the result.
//!
//! O(n) expected (each point does one grid-bucket insert/lookup plus
//! a bounded 27-cell scan whose average occupancy is O(1) for a
//! well-formed mesh); see ComputeWatertightness's own build-cost
//! measurement for the wall-clock this adds on a real asset.
//! DL-197.  `outEps`, when non-null, receives the SAME relative
//! epsilon this function used for its own weld -- so a caller that
//! needs a second geometric test at "the same tolerance the weld
//! itself trusted" (the coplanarity test below) doesn't re-derive
//! the formula and risk drifting from it.
bool GeometricUtilities::WeldVertexPositions(
	const std::vector<Point3>& points,
	std::vector<unsigned int>& outWeldedId,
	Scalar* outEps,
	const Scalar minEps,
	bool* outTransitive
	)
{
	outWeldedId.assign( points.size(), 0u );
	if( outTransitive ) { *outTransitive = true; }
	if( points.empty() ) {
		if( outEps ) { *outEps = std::max( Scalar( 1e-9 ), minEps ); }
		return true;
	}

	BoundingBox bbox( points[0], points[0] );
	for( std::size_t i = 0; i < points.size(); ++i ) {
		const Point3& p = points[i];
		if( !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ) return false;
		if( i > 0 ) {
			bbox.Include( p );
		}
	}
	const Vector3 extents = bbox.GetExtents();
	const Scalar diag = Vector3Ops::Magnitude( extents );
	const Scalar eps = std::max( std::max( Scalar( 1e-9 ), Scalar( 1e-6 ) * diag ), minEps );
	// Using a cell size of 32*eps ensures over 74% of query points lie
	// strictly > 1.5*eps inside their home cell on all 3 axes, avoiding
	// any neighbor-cell lookups while keeping bucket lists short.
	const Scalar cell = Scalar( 32 ) * eps;
	if( !std::isfinite(cell) || !(cell > 0) ) return false;
	if( outEps ) { *outEps = eps; }

	// Keep the existing grid for representable absolute coordinates. When
	// translation exceeds its integer range, use a mesh-local origin.
	// Relative coordinates are bounded by diag/eps <= 1e6. Strict bounds
	// below also leave room for the signed +/-1 neighbor additions.
	// Monotonicity of floor(v/cell) means checking bbox extremes suffices
	// for all finite points in bbox.
	const auto safeCoordinate = [cell](Scalar v) {
		const double c=std::floor(double(v/cell));
		return std::isfinite(c) && c > double(std::numeric_limits<std::int64_t>::min())
			&& c < double(std::numeric_limits<std::int64_t>::max());
	};
	const bool localGrid = !safeCoordinate(bbox.ll.x) || !safeCoordinate(bbox.ll.y) || !safeCoordinate(bbox.ll.z)
		|| !safeCoordinate(bbox.ur.x) || !safeCoordinate(bbox.ur.y) || !safeCoordinate(bbox.ur.z);
	const Point3 origin=localGrid?bbox.ll:Point3(0,0,0);
	auto cellCoord = [cell]( const Scalar v ) -> std::int64_t {
		return static_cast<std::int64_t>(std::floor(double(v/cell)));
	};
	// A simple, well-distributed combine for the 3D cell key -- collisions are
	// fine (a false-positive bucket collision only costs an extra distance
	// check, never a wrong weld, since every candidate found is still
	// distance-checked against `eps` below).
	auto cellKey = []( std::int64_t x, std::int64_t y, std::int64_t z ) -> std::uint64_t {
            // Hash products intentionally wrap modulo 2^64. Convert before
            // multiplication so large translated cells have defined arithmetic.
		return static_cast<std::uint64_t>(x) * 73856093ULL
                ^ static_cast<std::uint64_t>(y) * 19349663ULL
                ^ static_cast<std::uint64_t>(z) * 83492791ULL;
	};

	struct CellBucket
	{
		std::uint64_t key = 0;
		unsigned int head = 0xFFFFFFFFu;
		unsigned int tail = 0xFFFFFFFFu;
	};
	std::size_t tableCap = 16;
	while( tableCap < points.size() * 2 ) {
		tableCap <<= 1;
	}
	const std::size_t tableMask = tableCap - 1;
	std::vector<CellBucket> grid( tableCap );
	std::vector<Point3> weldedPos;
	weldedPos.reserve( points.size() );
	std::vector<unsigned int> nextInCell;
	nextInCell.reserve( points.size() );
	const Scalar epsSq = eps * eps;
	const Scalar halfEpsSq = Scalar( 0.25 ) * epsSq;
	const Scalar sepEps = Scalar( 1.5 ) * eps;
	const Scalar sepEpsSq = Scalar( 2.25 ) * epsSq;
	const Scalar interiorMargin = Scalar( 2 ) * eps;
	const Scalar interiorHi = cell - interiorMargin;
	bool transitive = true;

	auto mixKey = []( std::uint64_t k ) -> std::size_t {
		k ^= k >> 33;
		k *= 0xff51afd7ed558ccdULL;
		k ^= k >> 33;
		return static_cast<std::size_t>( k );
	};
	auto findBucketHead = [&]( const std::uint64_t key ) -> unsigned int {
		std::size_t slot = mixKey( key ) & tableMask;
		while( grid[slot].head != 0xFFFFFFFFu ) {
			if( grid[slot].key == key ) {
				return grid[slot].head;
			}
			slot = ( slot + 1 ) & tableMask;
		}
		return 0xFFFFFFFFu;
	};

	for( std::size_t i = 0; i < points.size(); ++i ) {
		const Point3& p = points[i];
		const Scalar rx = p.x - origin.x;
		const Scalar ry = p.y - origin.y;
		const Scalar rz = p.z - origin.z;
		const double fx = std::floor( double( rx / cell ) );
		const double fy = std::floor( double( ry / cell ) );
		const double fz = std::floor( double( rz / cell ) );
		const std::int64_t cx = static_cast<std::int64_t>( fx );
		const std::int64_t cy = static_cast<std::int64_t>( fy );
		const std::int64_t cz = static_cast<std::int64_t>( fz );
		const std::uint64_t homeKey = cellKey( cx, cy, cz );

		std::size_t homeSlot = mixKey( homeKey ) & tableMask;
		while( grid[homeSlot].head != 0xFFFFFFFFu && grid[homeSlot].key != homeKey ) {
			homeSlot = ( homeSlot + 1 ) & tableMask;
		}

		unsigned int foundId = 0xFFFFFFFFu;
		bool exactMatch = false;
		for( unsigned int cand = grid[homeSlot].head; cand != 0xFFFFFFFFu; cand = nextInCell[cand] ) {
			const Point3& wp = weldedPos[cand];
			if( wp.x == p.x && wp.y == p.y && wp.z == p.z ) {
				foundId = cand;
				exactMatch = true;
				break;
			}
			const Vector3 d = Vector3Ops::mkVector3( wp, p );
			const Scalar dSq = Vector3Ops::SquaredModulus( d );
			if( dSq <= epsSq ) {
				if( foundId == 0xFFFFFFFFu ) {
					foundId = cand;
					if( dSq > halfEpsSq ) { transitive = false; }
				} else {
					transitive = false;
				}
			} else if( dSq <= sepEpsSq ) {
				transitive = false;
			}
		}

		if( !exactMatch ) {
			const Scalar dx = rx - Scalar( fx ) * cell;
			const Scalar dy = ry - Scalar( fy ) * cell;
			const Scalar dz = rz - Scalar( fz ) * cell;
			const bool interiorToCell = ( dx > interiorMargin && dx < interiorHi )
				&& ( dy > interiorMargin && dy < interiorHi )
				&& ( dz > interiorMargin && dz < interiorHi );
			if( !interiorToCell ) {
				const Scalar searchRad = transitive ? sepEps : eps;
				const std::int64_t cx0 = cellCoord( rx - searchRad ), cx1 = cellCoord( rx + searchRad );
				const std::int64_t cy0 = cellCoord( ry - searchRad ), cy1 = cellCoord( ry + searchRad );
				const std::int64_t cz0 = cellCoord( rz - searchRad ), cz1 = cellCoord( rz + searchRad );
				for( std::int64_t nz = cz0; nz <= cz1; ++nz ) {
					for( std::int64_t ny = cy0; ny <= cy1; ++ny ) {
						for( std::int64_t nx = cx0; nx <= cx1; ++nx ) {
							if( nx == cx && ny == cy && nz == cz ) {
								continue;
							}
							const std::uint64_t key = cellKey( nx, ny, nz );
							for( unsigned int cand = findBucketHead( key ); cand != 0xFFFFFFFFu; cand = nextInCell[cand] ) {
								const Vector3 d = Vector3Ops::mkVector3( weldedPos[cand], p );
								const Scalar dSq = Vector3Ops::SquaredModulus( d );
								if( dSq <= epsSq ) {
									if( foundId == 0xFFFFFFFFu ) {
										foundId = cand;
										if( dSq > halfEpsSq ) { transitive = false; }
									} else {
										transitive = false;
									}
									if( !transitive ) { break; }
								} else if( dSq <= sepEpsSq ) {
									transitive = false;
								}
							}
							if( foundId != 0xFFFFFFFFu && !transitive ) { break; }
						}
						if( foundId != 0xFFFFFFFFu && !transitive ) { break; }
					}
					if( foundId != 0xFFFFFFFFu && !transitive ) { break; }
				}
			}
		}

		if( foundId == 0xFFFFFFFFu ) {
			foundId = (unsigned int)weldedPos.size();
			weldedPos.push_back( p );
			nextInCell.push_back( 0xFFFFFFFFu );
			if( grid[homeSlot].head == 0xFFFFFFFFu ) {
				grid[homeSlot].key = homeKey;
				grid[homeSlot].head = grid[homeSlot].tail = foundId;
			} else {
				nextInCell[grid[homeSlot].tail] = foundId;
				grid[homeSlot].tail = foundId;
			}
		}
		outWeldedId[i] = foundId;
	}
	if( outTransitive ) { *outTransitive = transitive; }
	return true;
}

//////////////////////////////////////////////////////////////////////
// DL-382: the general open-surface certificate.  Proof and design:
// docs/DL382_OPEN_SHEET_CERTIFICATE.md section 2.
//////////////////////////////////////////////////////////////////////
namespace
{
	using namespace RISE;

	inline Scalar Sq( const Vector3& v ) { return Vector3Ops::SquaredModulus( v ); }

	//! Squared distance from p to triangle abc (closest point by Voronoi
	//! region, Ericson, Real-Time Collision Detection 5.1.5).
	Scalar PointTriangleDistSq( const Point3& p, const Point3& a, const Point3& b, const Point3& c )
	{
		const Vector3 ab = Vector3Ops::mkVector3( b, a );
		const Vector3 ac = Vector3Ops::mkVector3( c, a );
		const Vector3 ap = Vector3Ops::mkVector3( p, a );
		const Scalar d1 = Vector3Ops::Dot( ab, ap ), d2 = Vector3Ops::Dot( ac, ap );
		if( d1 <= 0 && d2 <= 0 ) return Sq( ap );
		const Vector3 bp = Vector3Ops::mkVector3( p, b );
		const Scalar d3 = Vector3Ops::Dot( ab, bp ), d4 = Vector3Ops::Dot( ac, bp );
		if( d3 >= 0 && d4 <= d3 ) return Sq( bp );
		const Scalar vc = d1 * d4 - d3 * d2;
		if( vc <= 0 && d1 >= 0 && d3 <= 0 ) {
			const Scalar v = d1 / ( d1 - d3 );
			return Sq( Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( a, ab * v ) ) );
		}
		const Vector3 cp = Vector3Ops::mkVector3( p, c );
		const Scalar d5 = Vector3Ops::Dot( ab, cp ), d6 = Vector3Ops::Dot( ac, cp );
		if( d6 >= 0 && d5 <= d6 ) return Sq( cp );
		const Scalar vb = d5 * d2 - d1 * d6;
		if( vb <= 0 && d2 >= 0 && d6 <= 0 ) {
			const Scalar w = d2 / ( d2 - d6 );
			return Sq( Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( a, ac * w ) ) );
		}
		const Scalar va = d3 * d6 - d5 * d4;
		if( va <= 0 && ( d4 - d3 ) >= 0 && ( d5 - d6 ) >= 0 ) {
			const Scalar w = ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) );
			return Sq( Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( b, Vector3Ops::mkVector3( c, b ) * w ) ) );
		}
		const Scalar denom = Scalar( 1 ) / ( va + vb + vc );
		const Scalar v = vb * denom, w = vc * denom;
		return Sq( Vector3Ops::mkVector3( p, Point3Ops::mkPoint3( a, ab * v + ac * w ) ) );
	}

	inline Scalar Clamp01( const Scalar x ) { return x < 0 ? Scalar( 0 ) : ( x > 1 ? Scalar( 1 ) : x ); }

	//! Squared distance between segments p1q1 and p2q2 (Ericson 5.1.9).
	Scalar SegmentSegmentDistSq( const Point3& p1, const Point3& q1, const Point3& p2, const Point3& q2 )
	{
		const Vector3 d1 = Vector3Ops::mkVector3( q1, p1 );
		const Vector3 d2 = Vector3Ops::mkVector3( q2, p2 );
		const Vector3 r = Vector3Ops::mkVector3( p1, p2 );
		const Scalar a = Vector3Ops::Dot( d1, d1 ), e = Vector3Ops::Dot( d2, d2 ), f = Vector3Ops::Dot( d2, r );
		Scalar s = 0, t = 0;
		if( a <= 0 && e <= 0 ) {
			return Sq( r );
		}
		if( a <= 0 ) {
			t = Clamp01( f / e );
		} else {
			const Scalar c = Vector3Ops::Dot( d1, r );
			if( e <= 0 ) {
				s = Clamp01( -c / a );
			} else {
				const Scalar b = Vector3Ops::Dot( d1, d2 );
				const Scalar denom = a * e - b * b;
				s = denom > 0 ? Clamp01( ( b * f - c * e ) / denom ) : Scalar( 0 );
				t = ( b * s + f ) / e;
				if( t < 0 ) {
					t = 0;
					s = Clamp01( -c / a );
				} else if( t > 1 ) {
					t = 1;
					s = Clamp01( ( b - c ) / a );
				}
			}
		}
		return Sq( Vector3Ops::mkVector3( Point3Ops::mkPoint3( p1, d1 * s ), Point3Ops::mkPoint3( p2, d2 * t ) ) );
	}

	//! Does segment pq cross triangle abc (Moller-Trumbore on t in
	//! [0,1])?  A segment parallel to the triangle's plane reports no
	//! crossing; every caller pairs this with the edge/endpoint distances,
	//! which see a coplanar overlap (it crosses an edge or has an endpoint
	//! inside).
	bool SegmentCrossesTriangle( const Point3& p, const Point3& q, const Point3& a, const Point3& b, const Point3& c )
	{
		const Vector3 dir = Vector3Ops::mkVector3( q, p );
		const Vector3 e1 = Vector3Ops::mkVector3( b, a );
		const Vector3 e2 = Vector3Ops::mkVector3( c, a );
		const Vector3 h = Vector3Ops::Cross( dir, e2 );
		const Scalar det = Vector3Ops::Dot( e1, h );
		if( det == 0 ) return false;
		const Scalar inv = Scalar( 1 ) / det;
		const Vector3 s = Vector3Ops::mkVector3( p, a );
		const Scalar u = inv * Vector3Ops::Dot( s, h );
		if( u < 0 || u > 1 ) return false;
		const Vector3 qv = Vector3Ops::Cross( s, e1 );
		const Scalar v = inv * Vector3Ops::Dot( dir, qv );
		if( v < 0 || u + v > 1 ) return false;
		const Scalar t = inv * Vector3Ops::Dot( e2, qv );
		return t >= 0 && t <= 1;
	}

	//! Squared distance between segment pq and triangle abc.  If they do
	//! not cross, the closest pair has an endpoint or a triangle edge in it.
	Scalar SegmentTriangleDistSq( const Point3& p, const Point3& q, const Point3* T )
	{
		if( SegmentCrossesTriangle( p, q, T[0], T[1], T[2] ) ) return 0;
		Scalar d = PointTriangleDistSq( p, T[0], T[1], T[2] );
		d = std::min( d, PointTriangleDistSq( q, T[0], T[1], T[2] ) );
		for( int k = 0; k < 3; ++k ) {
			d = std::min( d, SegmentSegmentDistSq( p, q, T[k], T[(k + 1) % 3] ) );
		}
		return d;
	}

	//! Squared distance between two triangles: zero if an edge of one
	//! crosses the other, else the minimum over vertex-face and edge-edge
	//! pairs (where the closest pair of two disjoint triangles lies).
	Scalar TriangleTriangleDistSq( const Point3* A, const Point3* B )
	{
		for( int k = 0; k < 3; ++k ) {
			if( SegmentCrossesTriangle( A[k], A[(k + 1) % 3], B[0], B[1], B[2] ) ) return 0;
			if( SegmentCrossesTriangle( B[k], B[(k + 1) % 3], A[0], A[1], A[2] ) ) return 0;
		}
		Scalar d = RISE_INFINITY;
		for( int k = 0; k < 3; ++k ) {
			d = std::min( d, PointTriangleDistSq( A[k], B[0], B[1], B[2] ) );
			d = std::min( d, PointTriangleDistSq( B[k], A[0], A[1], A[2] ) );
			for( int m = 0; m < 3; ++m ) {
				d = std::min( d, SegmentSegmentDistSq( A[k], A[(k + 1) % 3], B[m], B[(m + 1) % 3] ) );
			}
		}
		return d;
	}

	//! Step 4 for one candidate pair: do triangles i and j meet ONLY in
	//! the welded faces they share, with margin `eps`?
	//!
	//! One shared vertex v: two convex sets containing v that meet
	//! elsewhere meet along a segment from v whose far end lies on the
	//! boundary of one of them, and following the argument through the
	//! edges at v, the far end is always on an edge OPPOSITE v (or a
	//! vertex of one triangle lies in the other, which is on an opposite
	//! edge too).  So "the opposite edges keep their distance" suffices.
	//! A shared edge ab (third vertices c, d): a non-coplanar pair meets
	//! only on ab; a coplanar FOLD puts c or d inside the other triangle
	//! or makes ac / bc cross bd / ad -- all refused.
	bool PairMeetsOnlyInSharedFaces( const Point3* Pi, const unsigned int* Ii,
		const Point3* Pj, const unsigned int* Ij, const Scalar epsSq )
	{
		int sharedI[3], sharedJ[3], s = 0;
		for( int a = 0; a < 3; ++a ) {
			for( int b = 0; b < 3; ++b ) {
				if( Ii[a] == Ij[b] ) {
					sharedI[s] = a;
					sharedJ[s] = b;
					++s;
				}
			}
		}
		if( s == 0 ) {
			return TriangleTriangleDistSq( Pi, Pj ) > epsSq;
		}
		if( s == 1 ) {
			const int vi = sharedI[0], vj = sharedJ[0];
			return SegmentTriangleDistSq( Pi[(vi + 1) % 3], Pi[(vi + 2) % 3], Pj ) > epsSq &&
				SegmentTriangleDistSq( Pj[(vj + 1) % 3], Pj[(vj + 2) % 3], Pi ) > epsSq;
		}
		if( s == 2 ) {
			const int ci = 3 - sharedI[0] - sharedI[1];
			const int dj = 3 - sharedJ[0] - sharedJ[1];
			const Point3& ai = Pi[sharedI[0]];
			const Point3& bi = Pi[sharedI[1]];
			const Point3& aj = Pj[sharedJ[0]];
			const Point3& bj = Pj[sharedJ[1]];
			return PointTriangleDistSq( Pi[ci], Pj[0], Pj[1], Pj[2] ) > epsSq &&
				PointTriangleDistSq( Pj[dj], Pi[0], Pi[1], Pi[2] ) > epsSq &&
				SegmentSegmentDistSq( ai, Pi[ci], bj, Pj[dj] ) > epsSq &&
				SegmentSegmentDistSq( bi, Pi[ci], aj, Pj[dj] ) > epsSq;
		}
		return false;
	}

	struct CellKey
	{
		std::int64_t x, y, z;
		bool operator==( const CellKey& o ) const { return x == o.x && y == o.y && z == o.z; }
	};
	struct CellKeyHash
	{
		std::size_t operator()( const CellKey& k ) const
		{
			std::uint64_t h = static_cast<std::uint64_t>( k.x ) * 73856093ULL
				^ static_cast<std::uint64_t>( k.y ) * 19349663ULL
				^ static_cast<std::uint64_t>( k.z ) * 83492791ULL;
			h ^= h >> 33;
			h *= 0xff51afd7ed558ccdULL;
			h ^= h >> 33;
			return static_cast<std::size_t>( h );
		}
	};
}

bool GeometricUtilities::CheckOpenSurfaceStep4Pair(
	const Point3* Pi,
	const unsigned int* Ii,
	const Point3* Pj,
	const unsigned int* Ij,
	const Scalar epsSq
	)
{
	return PairMeetsOnlyInSharedFaces( Pi, Ii, Pj, Ij, epsSq );
}

GeometricUtilities::OpenSurfaceCertificate GeometricUtilities::IsEmbeddedOpenOrientedSurface(
	const std::vector<Point3>& corners,
	std::vector<unsigned int>* outWeldedId,
	const Scalar minEps,
	const std::vector<unsigned int>* preWeldedId,
	const Scalar preWeldEps
	)
{
	OpenSurfaceCertificate result;
	const std::size_t nTris = corners.size() / 3;
	if( nTris == 0 || corners.size() % 3 != 0 || nTris > 0x3FFFFFFFu ) {
		return result;
	}
	std::vector<unsigned int> localWid;
	Scalar eps = 0;
	const std::vector<unsigned int>* pWid = nullptr;
	if( preWeldedId && preWeldedId->size() == corners.size() && preWeldEps > Scalar( 0 ) ) {
		pWid = preWeldedId;
		eps = std::max( preWeldEps, minEps );
		if( outWeldedId ) {
			*outWeldedId = *preWeldedId;
		}
	} else {
		if( !WeldVertexPositions( corners, localWid, &eps, minEps ) ) {
			return result;
		}
		if( outWeldedId ) {
			*outWeldedId = localWid;
		}
		pWid = &localWid;
	}
	const std::vector<unsigned int>& wid = *pWid;
	const Scalar epsSq = eps * eps;

	// 1. Non-degenerate: distinct welded corners, smallest altitude > eps
	// (altitude = 2 x area / longest edge).
	for( std::size_t t = 0; t < nTris; ++t ) {
		const unsigned int* I = &wid[3 * t];
		if( I[0] == I[1] || I[1] == I[2] || I[0] == I[2] ) {
			result.refusalTriA = result.refusalTriB = t;
			return result;
		}
		const Point3* P = &corners[3 * t];
		const Scalar twiceArea = Vector3Ops::Magnitude( Vector3Ops::Cross(
			Vector3Ops::mkVector3( P[1], P[0] ), Vector3Ops::mkVector3( P[2], P[0] ) ) );
		Scalar longest = 0;
		for( int k = 0; k < 3; ++k ) {
			longest = std::max( longest, Vector3Ops::Magnitude( Vector3Ops::mkVector3( P[(k + 1) % 3], P[k] ) ) );
		}
		if( !( twiceArea > eps * longest ) ) {
			result.refusalTriA = result.refusalTriB = t;
			return result;
		}
	}

	// 2. Manifold edges, consistently oriented (flat open-addressed table).
	struct EdgeUse
	{
		std::uint64_t key = ~std::uint64_t( 0 );
		unsigned int tri[2] = { 0, 0 };
		bool forward[2] = { false, false };
		unsigned int count = 0;
	};
	std::size_t edgeCap = 16;
	while( edgeCap < nTris * 6 ) {
		edgeCap <<= 1;
	}
	const std::size_t edgeMask = edgeCap - 1;
	std::vector<EdgeUse> edgeTable( edgeCap );
	std::vector<EdgeUse*> triEdges( 3 * nTris, nullptr );
	for( std::size_t t = 0; t < nTris; ++t ) {
		for( int k = 0; k < 3; ++k ) {
			const unsigned int a = wid[3 * t + k], b = wid[3 * t + ( k + 1 ) % 3];
			const std::uint64_t key = a < b ? ( ( std::uint64_t( a ) << 32 ) | b ) : ( ( std::uint64_t( b ) << 32 ) | a );
			std::size_t slot = static_cast<std::size_t>( ( key ^ ( key >> 33 ) ) * 0xff51afd7ed558ccdULL ) & edgeMask;
			while( edgeTable[slot].key != ~std::uint64_t( 0 ) && edgeTable[slot].key != key ) {
				slot = ( slot + 1 ) & edgeMask;
			}
			EdgeUse& e = edgeTable[slot];
			if( e.key == ~std::uint64_t( 0 ) ) {
				e.key = key;
			}
			if( e.count >= 2 ) {
				result.topologicalRefusal = true;
				return result;		// non-manifold edge
			}
			e.tri[e.count] = (unsigned int)t;
			e.forward[e.count] = a < b;
			++e.count;
			triEdges[3 * t + k] = &e;
		}
	}
	bool hasBoundaryEdge = false;
	for( std::size_t i = 0; i < 3 * nTris; ++i ) {
		const EdgeUse* e = triEdges[i];
		if( e->count == 1 ) {
			hasBoundaryEdge = true;
		} else if( e->count == 2 && e->forward[0] == e->forward[1] ) {
			result.topologicalRefusal = true;
			return result;			// traversed the same way twice: inconsistent orientation
		}
	}
	if( !hasBoundaryEdge ) {
		result.topologicalRefusal = true;
		return result;				// no boundary edge: closed 2-cycle (H_2 != 0)
	}

	// 2b. Single connected component: all triangles connected via manifold edges.
    // Conservative transport policy: disconnected pieces are refused by
    // this general proof. Author each sheet as its own object; certification
    // of individual pieces does not define their combined medium convention.
	{
		std::vector<unsigned char> visited( nTris, 0 );
		std::vector<unsigned int> q;
		q.reserve( nTris );
		q.push_back( 0 );
		visited[0] = 1;
		for( std::size_t i = 0; i < q.size(); ++i ) {
			const unsigned int t = q[i];
			for( int k = 0; k < 3; ++k ) {
				const EdgeUse* e = triEdges[3 * t + k];
				if( e->count == 2 ) {
					const unsigned int other = ( e->tri[0] == t ) ? e->tri[1] : e->tri[0];
					if( !visited[other] ) {
						visited[other] = 1;
						q.push_back( other );
					}
				}
			}
		}
		if( q.size() != nTris ) {
			result.topologicalRefusal = true;
			return result;			// multiple disconnected components
		}
	}

	// 3. Free-face peeling (H_2 = 0).
	{
		std::vector<unsigned char> alive( nTris, 1 );
		std::vector<unsigned int> queue;
		queue.reserve( nTris );
		for( std::size_t t = 0; t < nTris; ++t ) {
			for( int k = 0; k < 3; ++k ) {
				if( triEdges[3 * t + k]->count == 1 ) {
					queue.push_back( (unsigned int)t );
					break;
				}
			}
		}
		std::size_t removed = 0;
		for( std::size_t q = 0; q < queue.size(); ++q ) {
			const unsigned int t = queue[q];
			if( !alive[t] ) {
				continue;
			}
			alive[t] = 0;
			++removed;
			for( int k = 0; k < 3; ++k ) {
				EdgeUse* e = triEdges[3 * t + k];
				--e->count;
				if( e->count == 1 ) {
					const unsigned int other = alive[e->tri[0]] ? e->tri[0] : e->tri[1];
					if( alive[other] ) {
						queue.push_back( other );
					}
				}
			}
		}
		if( removed != nTris ) {
			result.topologicalRefusal = true;
			return result;			// a closed 2-cycle survives
		}
	}

	// 4. Embedded: candidate pairs from a uniform grid over eps-inflated
	// triangle boxes; each pair is tested once, in the cell holding the
	// low corner of the two boxes' overlap.
	{
		std::vector<Point3> lo( nTris ), hi( nTris );
		Point3 gLo( RISE_INFINITY, RISE_INFINITY, RISE_INFINITY );
		Scalar meanExtent = 0;
		for( std::size_t t = 0; t < nTris; ++t ) {
			const Point3* P = &corners[3 * t];
			Point3 l = P[0], h = P[0];
			for( int k = 1; k < 3; ++k ) {
				l.x = std::min( l.x, P[k].x ); l.y = std::min( l.y, P[k].y ); l.z = std::min( l.z, P[k].z );
				h.x = std::max( h.x, P[k].x ); h.y = std::max( h.y, P[k].y ); h.z = std::max( h.z, P[k].z );
			}
			l = Point3( l.x - eps, l.y - eps, l.z - eps );
			h = Point3( h.x + eps, h.y + eps, h.z + eps );
			lo[t] = l;
			hi[t] = h;
			gLo.x = std::min( gLo.x, l.x ); gLo.y = std::min( gLo.y, l.y ); gLo.z = std::min( gLo.z, l.z );
			meanExtent += std::max( h.x - l.x, std::max( h.y - l.y, h.z - l.z ) );
		}
		Scalar cell = meanExtent / Scalar( nTris );
		if( !( cell > 0 ) || !std::isfinite( cell ) ) {
			return result;
		}
		const auto coord = [&]( const Scalar v, const Scalar o ) -> std::int64_t {
			return static_cast<std::int64_t>( std::floor( double( ( v - o ) / cell ) ) );
		};
		// Bound the insertions: a few huge triangles in a fine mesh would
		// otherwise touch very many cells; coarsen until the total is linear.
		const double budget = 64.0 * double( nTris ) + 4096.0;
		double finalTotal = 0;
		for( int attempt = 0; attempt < 64; ++attempt ) {
			double total = 0;
			for( std::size_t t = 0; t < nTris && total <= budget; ++t ) {
				total += double( coord( hi[t].x, gLo.x ) - coord( lo[t].x, gLo.x ) + 1 )
					* double( coord( hi[t].y, gLo.y ) - coord( lo[t].y, gLo.y ) + 1 )
					* double( coord( hi[t].z, gLo.z ) - coord( lo[t].z, gLo.z ) + 1 );
			}
			finalTotal = total;
			if( total <= budget ) {
				break;
			}
			cell *= 2;
		}
		struct CellSlot
		{
			CellKey key{ 0, 0, 0 };
			unsigned int head = 0xFFFFFFFFu;
			unsigned int tail = 0xFFFFFFFFu;
			unsigned int count = 0;
		};
		const std::size_t estInsertions = ( finalTotal > 0.0 && finalTotal <= budget )
			? static_cast<std::size_t>( finalTotal )
			: nTris * 4;
		std::size_t cellCap = 16;
		while( cellCap < nTris * 4 ) {
			cellCap <<= 1;
		}
		std::size_t cellMask = cellCap - 1;
		std::vector<CellSlot> cellTable( cellCap );
		std::size_t usedCells = 0;
		std::vector<unsigned int> multiCellSlots;
		multiCellSlots.reserve( nTris );
		std::vector<unsigned int> entryTri;
		entryTri.reserve( estInsertions );
		std::vector<unsigned int> entryNext;
		entryNext.reserve( estInsertions );
		std::vector<CellKey> triLoCell( nTris );

		for( std::size_t t = 0; t < nTris; ++t ) {
			const std::int64_t x0 = coord( lo[t].x, gLo.x ), x1 = coord( hi[t].x, gLo.x );
			const std::int64_t y0 = coord( lo[t].y, gLo.y ), y1 = coord( hi[t].y, gLo.y );
			const std::int64_t z0 = coord( lo[t].z, gLo.z ), z1 = coord( hi[t].z, gLo.z );
			triLoCell[t] = CellKey{ x0, y0, z0 };
			for( std::int64_t z = z0; z <= z1; ++z ) {
				for( std::int64_t y = y0; y <= y1; ++y ) {
					for( std::int64_t x = x0; x <= x1; ++x ) {
						if( usedCells * 2 >= cellCap ) {
							cellCap <<= 1;
							cellMask = cellCap - 1;
							std::vector<CellSlot> newTable( cellCap );
							multiCellSlots.clear();
							for( const CellSlot& old : cellTable ) {
								if( old.head == 0xFFFFFFFFu ) continue;
								std::size_t s = CellKeyHash()( old.key ) & cellMask;
								while( newTable[s].head != 0xFFFFFFFFu ) {
									s = ( s + 1 ) & cellMask;
								}
								newTable[s] = old;
								if( old.count >= 2 ) {
									multiCellSlots.push_back( static_cast<unsigned int>( s ) );
								}
							}
							cellTable.swap( newTable );
						}
						const CellKey ck{ x, y, z };
						std::size_t slotIdx = CellKeyHash()( ck ) & cellMask;
						while( cellTable[slotIdx].head != 0xFFFFFFFFu && !( cellTable[slotIdx].key == ck ) ) {
							slotIdx = ( slotIdx + 1 ) & cellMask;
						}
						const unsigned int nodeIdx = static_cast<unsigned int>( entryTri.size() );
						entryTri.push_back( static_cast<unsigned int>( t ) );
						entryNext.push_back( 0xFFFFFFFFu );
						CellSlot& slot = cellTable[slotIdx];
						if( slot.head == 0xFFFFFFFFu ) {
							slot.key = ck;
							slot.head = slot.tail = nodeIdx;
							slot.count = 1;
							++usedCells;
						} else {
							entryNext[slot.tail] = nodeIdx;
							slot.tail = nodeIdx;
							++slot.count;
							if( slot.count == 2 ) {
								multiCellSlots.push_back( static_cast<unsigned int>( slotIdx ) );
							}
						}
					}
				}
			}
		}
		for( const unsigned int slotIdx : multiCellSlots ) {
			const CellSlot& slot = cellTable[slotIdx];
			for( unsigned int a = slot.head; a != 0xFFFFFFFFu; a = entryNext[a] ) {
				const unsigned int i = entryTri[a];
				const Point3& loi = lo[i];
				const Point3& hii = hi[i];
				const CellKey& ci = triLoCell[i];
				for( unsigned int b = entryNext[a]; b != 0xFFFFFFFFu; b = entryNext[b] ) {
					const unsigned int j = entryTri[b];
					const Point3& loj = lo[j];
					const Point3& hij = hi[j];
					if( loi.x > hij.x || loj.x > hii.x ||
						loi.y > hij.y || loj.y > hii.y ||
						loi.z > hij.z || loj.z > hii.z ) {
						continue;	// inflated boxes apart: the gap exceeds eps
					}
					const CellKey& cj = triLoCell[j];
					if( std::max( ci.x, cj.x ) != slot.key.x ||
						std::max( ci.y, cj.y ) != slot.key.y ||
						std::max( ci.z, cj.z ) != slot.key.z ) {
						continue;	// tested in the reference cell
					}
					if( !PairMeetsOnlyInSharedFaces(
						&corners[3 * i], &wid[3 * i],
						&corners[3 * j], &wid[3 * j], epsSq ) ) {
						result.refusalTriA = i;
						result.refusalTriB = j;
						return result;
					}
				}
			}
		}
	}

	// Certified.  The signed volume about the box centre is the diagnostic
	// of section 2.1 (never a transport input).
	Point3 bbLo = corners[0], bbHi = corners[0];
	for( const Point3& p : corners ) {
		bbLo.x = std::min( bbLo.x, p.x ); bbLo.y = std::min( bbLo.y, p.y ); bbLo.z = std::min( bbLo.z, p.z );
		bbHi.x = std::max( bbHi.x, p.x ); bbHi.y = std::max( bbHi.y, p.y ); bbHi.z = std::max( bbHi.z, p.z );
	}
	const Point3 centre( 0.5 * ( bbLo.x + bbHi.x ), 0.5 * ( bbLo.y + bbHi.y ), 0.5 * ( bbLo.z + bbHi.z ) );
	Scalar vol = 0;
	for( std::size_t t = 0; t < nTris; ++t ) {
		const Point3* P = &corners[3 * t];
		vol += Vector3Ops::Dot( Vector3Ops::mkVector3( P[0], centre ),
			Vector3Ops::Cross( Vector3Ops::mkVector3( P[1], centre ), Vector3Ops::mkVector3( P[2], centre ) ) );
	}
	result.signedVolumeAboutCentre = vol / Scalar( 6 );
	result.certified = true;
	return result;
}

namespace
{
	struct WeldPointLess
	{
		bool operator()( const Point3& a, const Point3& b ) const
		{
			if( a.x != b.x ) return a.x < b.x;
			if( a.y != b.y ) return a.y < b.y;
			return a.z < b.z;
		}
	};

	struct HalfEdgeRec
	{
		unsigned int u, v, tri;
		bool operator<( const HalfEdgeRec& o ) const
		{
			if( u != o.u ) return u < o.u;
			if( v != o.v ) return v < o.v;
			return tri < o.tri;
		}
	};

	struct TriKey
	{
		std::array<unsigned int, 3> v;
		bool operator<( const TriKey& o ) const { return v < o.v; }
		bool operator==( const TriKey& o ) const { return v == o.v; }
	};

	inline bool PointFinite( const Point3& p )
	{
		return std::isfinite( p.x ) && std::isfinite( p.y ) && std::isfinite( p.z );
	}

	inline bool PointEqualExact( const Point3& a, const Point3& b )
	{
		return a.x == b.x && a.y == b.y && a.z == b.z;
	}

	inline Point3 CanonicalAddPoints( const Point3& a, const Point3& b )
	{
		const WeldPointLess less;
		if( less( a, b ) ) {
			return Point3( a.x + b.x, a.y + b.y, a.z + b.z );
		}
		return Point3( b.x + a.x, b.y + a.y, b.z + a.z );
	}

	inline Point3 LerpSym4( const Point3& p0, const Point3& p1, unsigned int k )
	{
		if( k == 0 ) return p0;
		if( k == 4 ) return p1;
		if( k == 2 ) {
			const Point3 s = CanonicalAddPoints( p0, p1 );
			return Point3( Scalar( 0.5 ) * s.x, Scalar( 0.5 ) * s.y, Scalar( 0.5 ) * s.z );
		}
		const Point3& nearPt = ( k == 1 ) ? p0 : p1;
		const Point3& farPt  = ( k == 1 ) ? p1 : p0;
		return Point3(
			Scalar( 0.25 ) * ( Scalar( 3 ) * nearPt.x + farPt.x ),
			Scalar( 0.25 ) * ( Scalar( 3 ) * nearPt.y + farPt.y ),
			Scalar( 0.25 ) * ( Scalar( 3 ) * nearPt.z + farPt.z ) );
	}

	inline Point3 EvalBilinearGrid4( const BilinearPatch& patch, unsigned int iu, unsigned int iv )
	{
		if( iu == 0 ) return LerpSym4( patch.pts[0], patch.pts[1], iv );
		if( iu == 4 ) return LerpSym4( patch.pts[2], patch.pts[3], iv );
		if( iv == 0 ) return LerpSym4( patch.pts[0], patch.pts[2], iu );
		if( iv == 4 ) return LerpSym4( patch.pts[1], patch.pts[3], iu );
		const Point3 col0 = LerpSym4( patch.pts[0], patch.pts[1], iv );
		const Point3 col1 = LerpSym4( patch.pts[2], patch.pts[3], iv );
		return LerpSym4( col0, col1, iu );
	}

	inline Point3 CubicSym4( const Point3& p0, const Point3& p1, const Point3& p2, const Point3& p3, unsigned int k )
	{
		if( k == 0 ) return p0;
		if( k == 4 ) return p3;
		if( k == 2 ) {
			const Point3 s03 = CanonicalAddPoints( p0, p3 );
			const Point3 s12 = CanonicalAddPoints( p1, p2 );
			return Point3(
				Scalar( 0.125 ) * s03.x + Scalar( 0.375 ) * s12.x,
				Scalar( 0.125 ) * s03.y + Scalar( 0.375 ) * s12.y,
				Scalar( 0.125 ) * s03.z + Scalar( 0.375 ) * s12.z );
		}
		const Point3& a0 = ( k == 1 ) ? p0 : p3;
		const Point3& a1 = ( k == 1 ) ? p1 : p2;
		const Point3& a2 = ( k == 1 ) ? p2 : p1;
		const Point3& a3 = ( k == 1 ) ? p3 : p0;
		return Point3(
			Scalar( 1.0 / 64.0 ) * ( Scalar( 27 ) * ( a0.x + a1.x ) + Scalar( 9 ) * a2.x + a3.x ),
			Scalar( 1.0 / 64.0 ) * ( Scalar( 27 ) * ( a0.y + a1.y ) + Scalar( 9 ) * a2.y + a3.y ),
			Scalar( 1.0 / 64.0 ) * ( Scalar( 27 ) * ( a0.z + a1.z ) + Scalar( 9 ) * a2.z + a3.z ) );
	}

	inline Point3 EvalBezierGrid4( const BezierPatch& patch, unsigned int iu, unsigned int iv )
	{
		if( iu == 0 ) return CubicSym4( patch.c[0].pts[0], patch.c[0].pts[1], patch.c[0].pts[2], patch.c[0].pts[3], iv );
		if( iu == 4 ) return CubicSym4( patch.c[3].pts[0], patch.c[3].pts[1], patch.c[3].pts[2], patch.c[3].pts[3], iv );
		if( iv == 0 ) return CubicSym4( patch.c[0].pts[0], patch.c[1].pts[0], patch.c[2].pts[0], patch.c[3].pts[0], iu );
		if( iv == 4 ) return CubicSym4( patch.c[0].pts[3], patch.c[1].pts[3], patch.c[2].pts[3], patch.c[3].pts[3], iu );
		Point3 r[4];
		for( int i = 0; i < 4; ++i ) {
			r[i] = CubicSym4( patch.c[i].pts[0], patch.c[i].pts[1], patch.c[i].pts[2], patch.c[i].pts[3], iv );
		}
		return CubicSym4( r[0], r[1], r[2], r[3], iu );
	}

	inline Point3 BlossomCubic( const Point3 P[4], Scalar t1, Scalar t2, Scalar t3 )
	{
		Point3 a[3], b[2];
		for( int k = 0; k < 3; ++k ) a[k] = Point3Ops::WeightedAverage2( P[k + 1], P[k], t1 );
		for( int k = 0; k < 2; ++k ) b[k] = Point3Ops::WeightedAverage2( a[k + 1], a[k], t2 );
		return Point3Ops::WeightedAverage2( b[1], b[0], t3 );
	}

	inline void SubCubicSegment( const Point3 P[4], Scalar s0, Scalar s1, Point3 Q[4] )
	{
		Q[0] = BlossomCubic( P, s0, s0, s0 );
		Q[1] = BlossomCubic( P, s0, s0, s1 );
		Q[2] = BlossomCubic( P, s0, s1, s1 );
		Q[3] = BlossomCubic( P, s1, s1, s1 );
	}

	inline BezierPatch SubdivideBezierPatchRange(
		const BezierPatch& p,
		Scalar u0, Scalar u1,
		Scalar v0, Scalar v1 )
	{
		Point3 R[4][4];
		BezierPatch out;
		for( int i = 0; i < 4; ++i ) {
			SubCubicSegment( p.c[i].pts, v0, v1, R[i] );
		}
		for( int l = 0; l < 4; ++l ) {
			const Point3 col[4] = { R[0][l], R[1][l], R[2][l], R[3][l] };
			Point3 sub[4];
			SubCubicSegment( col, u0, u1, sub );
			for( int k = 0; k < 4; ++k ) {
				out.c[k].pts[l] = sub[k];
			}
		}
		return out;
	}

	inline BilinearPatch SubdivideBilinearPatchRange(
		const BilinearPatch& p,
		Scalar u0, Scalar u1,
		Scalar v0, Scalar v1 )
	{
		BilinearPatch out;
		out.pts[0] = GeometricUtilities::EvaluateBilinearPatchAt( p, u0, v0 );
		out.pts[1] = GeometricUtilities::EvaluateBilinearPatchAt( p, u0, v1 );
		out.pts[2] = GeometricUtilities::EvaluateBilinearPatchAt( p, u1, v0 );
		out.pts[3] = GeometricUtilities::EvaluateBilinearPatchAt( p, u1, v1 );
		return out;
	}

	inline void ComputeBezierNormalBernsteinCoeffs( const BezierPatch& subPatch, Vector3 (&c)[6][6] )
	{
		static const Scalar C2[3] = { 1, 2, 1 };
		static const Scalar C3[4] = { 1, 3, 3, 1 };
		static const Scalar C5[6] = { 1, 5, 10, 10, 5, 1 };
		Vector3 du[3][4], dv[4][3];
		for( int i = 0; i < 3; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				du[i][j] = Vector3Ops::mkVector3( subPatch.c[i + 1].pts[j], subPatch.c[i].pts[j] );
			}
		}
		for( int k = 0; k < 4; ++k ) {
			for( int l = 0; l < 3; ++l ) {
				dv[k][l] = Vector3Ops::mkVector3( subPatch.c[k].pts[l + 1], subPatch.c[k].pts[l] );
			}
		}
		for( int a = 0; a < 6; ++a ) {
			for( int b = 0; b < 6; ++b ) {
				c[a][b] = Vector3( 0, 0, 0 );
			}
		}
		for( int i = 0; i < 3; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				for( int k = 0; k < 4; ++k ) {
					for( int l = 0; l < 3; ++l ) {
						const Scalar w = ( C2[i] * C3[k] / C5[i + k] ) * ( C3[j] * C2[l] / C5[j + l] );
						c[i + k][j + l] = c[i + k][j + l] + Vector3Ops::Cross( du[i][j], dv[k][l] ) * w;
					}
				}
			}
		}
	}

	inline bool BoxesOverlapClosed( const BoundingBox& a, const BoundingBox& b )
	{
		return !( a.ur.x < b.ll.x || b.ur.x < a.ll.x ||
		          a.ur.y < b.ll.y || b.ur.y < a.ll.y ||
		          a.ur.z < b.ll.z || b.ur.z < a.ll.z );
	}

	bool BezierSubPatchesDisjointRecursive(
		const BezierPatch& a,
		const BezierPatch& b,
		int depth )
	{
		const BoundingBox boxA = GeometricUtilities::BezierPatchBoundingBox( a );
		const BoundingBox boxB = GeometricUtilities::BezierPatchBoundingBox( b );
		if( !BoxesOverlapClosed( boxA, boxB ) ) {
			return true;
		}
		if( depth <= 0 ) {
			return false;
		}
		BezierPatch subA[4], subB[4];
		for( int iu = 0; iu < 2; ++iu ) {
			for( int iv = 0; iv < 2; ++iv ) {
				const Scalar u0 = Scalar( 0.5 ) * Scalar( iu );
				const Scalar u1 = u0 + Scalar( 0.5 );
				const Scalar v0 = Scalar( 0.5 ) * Scalar( iv );
				const Scalar v1 = v0 + Scalar( 0.5 );
				subA[2 * iu + iv] = SubdivideBezierPatchRange( a, u0, u1, v0, v1 );
				subB[2 * iu + iv] = SubdivideBezierPatchRange( b, u0, u1, v0, v1 );
			}
		}
		for( int i = 0; i < 4; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				if( !BezierSubPatchesDisjointRecursive( subA[i], subB[j], depth - 1 ) ) {
					return false;
				}
			}
		}
		return true;
	}

	bool BilinearSubPatchesDisjointRecursive(
		const BilinearPatch& a,
		const BilinearPatch& b,
		int depth )
	{
		const BoundingBox boxA = GeometricUtilities::BilinearPatchBoundingBox( a );
		const BoundingBox boxB = GeometricUtilities::BilinearPatchBoundingBox( b );
		if( !BoxesOverlapClosed( boxA, boxB ) ) {
			return true;
		}
		if( depth <= 0 ) {
			return false;
		}
		BilinearPatch subA[4], subB[4];
		for( int iu = 0; iu < 2; ++iu ) {
			for( int iv = 0; iv < 2; ++iv ) {
				const Scalar u0 = Scalar( 0.5 ) * Scalar( iu );
				const Scalar u1 = u0 + Scalar( 0.5 );
				const Scalar v0 = Scalar( 0.5 ) * Scalar( iv );
				const Scalar v1 = v0 + Scalar( 0.5 );
				subA[2 * iu + iv] = SubdivideBilinearPatchRange( a, u0, u1, v0, v1 );
				subB[2 * iu + iv] = SubdivideBilinearPatchRange( b, u0, u1, v0, v1 );
			}
		}
		for( int i = 0; i < 4; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				if( !BilinearSubPatchesDisjointRecursive( subA[i], subB[j], depth - 1 ) ) {
					return false;
				}
			}
		}
		return true;
	}

	// Checks whether the welded triangle mesh (3 corners per triangle in `corners`)
	// is a single-component, consistently oriented, embedded closed 2-manifold with
	// positive enclosed volume and no coplanar oppositely-wound adjacent faces.
	bool IsEmbeddedClosedOrientedTriangulation( const std::vector<Point3>& corners )
	{
		const std::size_t nTris = corners.size() / 3;
		if( nTris < 4 ) {
			return false;
		}
		std::vector<unsigned int> wid;
		Scalar eps = 0;
		if( !GeometricUtilities::WeldVertexPositions( corners, wid, &eps ) ) {
			return false;
		}
		const Scalar epsSq = eps * eps;
		const Scalar fourEpsSq = Scalar( 4 ) * epsSq;

		std::vector<HalfEdgeRec> halfEdges;
		std::vector<TriKey> triKeys;
		std::vector<Vector3> faceNormals( nTris );
		halfEdges.reserve( nTris * 3 );
		triKeys.reserve( nTris );

		for( std::size_t t = 0; t < nTris; ++t ) {
			const unsigned int a = wid[3 * t + 0];
			const unsigned int b = wid[3 * t + 1];
			const unsigned int c = wid[3 * t + 2];
			if( a == b || b == c || c == a ) {
				return false;
			}
			const Point3& pa = corners[3 * t + 0];
			const Point3& pb = corners[3 * t + 1];
			const Point3& pc = corners[3 * t + 2];
			const Vector3 ab = Vector3Ops::mkVector3( pb, pa );
			const Vector3 ac = Vector3Ops::mkVector3( pc, pa );
			const Vector3 bc = Vector3Ops::mkVector3( pc, pb );
			const Vector3 n = Vector3Ops::Cross( ab, ac );
			const Scalar nSq = Vector3Ops::SquaredModulus( n );
			const Scalar maxEdgeSq = std::max( Vector3Ops::SquaredModulus( ab ),
				std::max( Vector3Ops::SquaredModulus( ac ), Vector3Ops::SquaredModulus( bc ) ) );
			if( !( nSq > maxEdgeSq * fourEpsSq ) ) {
				return false;
			}
			faceNormals[t] = n * ( Scalar( 1 ) / std::sqrt( nSq ) );
			halfEdges.push_back( { a, b, static_cast<unsigned int>( t ) } );
			halfEdges.push_back( { b, c, static_cast<unsigned int>( t ) } );
			halfEdges.push_back( { c, a, static_cast<unsigned int>( t ) } );
			TriKey tk = { { a, b, c } };
			std::sort( tk.v.begin(), tk.v.end() );
			triKeys.push_back( tk );
		}

		std::sort( triKeys.begin(), triKeys.end() );
		if( std::adjacent_find( triKeys.begin(), triKeys.end() ) != triKeys.end() ) {
			return false;
		}
		std::sort( halfEdges.begin(), halfEdges.end() );
		for( std::size_t i = 0; i + 1 < halfEdges.size(); ++i ) {
			if( halfEdges[i].u == halfEdges[i + 1].u && halfEdges[i].v == halfEdges[i + 1].v ) {
				return false;
			}
		}

		std::vector<std::array<unsigned int, 3> > adj( nTris, { { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu } } );
		std::vector<unsigned char> adjCount( nTris, 0 );
		for( const HalfEdgeRec& he : halfEdges ) {
			const HalfEdgeRec revKey = { he.v, he.u, 0 };
			const auto it = std::lower_bound( halfEdges.begin(), halfEdges.end(), revKey );
			if( it == halfEdges.end() || it->u != he.v || it->v != he.u ) {
				return false; // Every edge in a closed manifold must have an opposite half-edge.
			}
			adj[he.tri][adjCount[he.tri]++] = it->tri;
			if( he.u < he.v ) {
				const unsigned int t1 = he.tri;
				const unsigned int t2 = it->tri;
				if( Vector3Ops::Dot( faceNormals[t1], faceNormals[t2] ) < Scalar( 0 ) ) {
					for( int k = 0; k < 3; ++k ) {
						const unsigned int w = wid[3 * t2 + k];
						if( w != he.u && w != he.v ) {
							const Scalar dist = std::fabs( Vector3Ops::Dot(
								Vector3Ops::mkVector3( corners[3 * t2 + k], corners[3 * t1] ),
								faceNormals[t1] ) );
							if( dist <= eps ) {
								return false; // Coplanar oppositely-wound seam (folded sheet).
							}
						}
					}
				}
			}
		}

		// Connectedness check.
		std::vector<unsigned char> visited( nTris, 0 );
		std::vector<unsigned int> queue;
		queue.reserve( nTris );
		visited[0] = 1;
		queue.push_back( 0 );
		for( std::size_t head = 0; head < queue.size(); ++head ) {
			const unsigned int t = queue[head];
			for( unsigned char k = 0; k < adjCount[t]; ++k ) {
				const unsigned int nb = adj[t][k];
				if( !visited[nb] ) {
					visited[nb] = 1;
					queue.push_back( nb );
				}
			}
		}
		if( queue.size() != nTris ) {
			return false;
		}

		// Step 4 embedding check on all triangle pairs with overlapping inflated AABBs.
		for( std::size_t i = 0; i < nTris; ++i ) {
			for( std::size_t j = i + 1; j < nTris; ++j ) {
				if( !PairMeetsOnlyInSharedFaces(
					&corners[3 * i], &wid[3 * i],
					&corners[3 * j], &wid[3 * j], epsSq ) ) {
					return false;
				}
			}
		}

		// Positive enclosed volume check (outward-wound solid).
		Point3 bbLo = corners[0], bbHi = corners[0];
		for( const Point3& p : corners ) {
			bbLo.x = std::min( bbLo.x, p.x ); bbLo.y = std::min( bbLo.y, p.y ); bbLo.z = std::min( bbLo.z, p.z );
			bbHi.x = std::max( bbHi.x, p.x ); bbHi.y = std::max( bbHi.y, p.y ); bbHi.z = std::max( bbHi.z, p.z );
		}
		const Point3 centre( 0.5 * ( bbLo.x + bbHi.x ), 0.5 * ( bbLo.y + bbHi.y ), 0.5 * ( bbLo.z + bbHi.z ) );
		Scalar vol = 0;
		for( std::size_t t = 0; t < nTris; ++t ) {
			const Point3* P = &corners[3 * t];
			vol += Vector3Ops::Dot( Vector3Ops::mkVector3( P[0], centre ),
				Vector3Ops::Cross( Vector3Ops::mkVector3( P[1], centre ), Vector3Ops::mkVector3( P[2], centre ) ) );
		}
		return vol > eps * eps * eps;
	}

	template<std::size_t N>
	struct PatchSideCurve
	{
		std::array<Point3, N> pts;
		unsigned int patchIdx = 0;
		unsigned char sideIdx = 0;
		bool collapsed = false;
	};

	template<std::size_t N>
	bool ValidateAndMatchPatchSides(
		const std::vector<std::array<PatchSideCurve<N>, 4> >& patchSides,
		bool& outAllMatchedClosed,
		bool& outPeelsToEmptyOpen )
	{
		outAllMatchedClosed = false;
		outPeelsToEmptyOpen = false;
		const std::size_t nPatches = patchSides.size();
		if( nPatches == 0 ) {
			return false;
		}

		const WeldPointLess less;
		struct EndpointKey
		{
			Point3 a;
			Point3 b;
			bool operator<( const EndpointKey& o ) const
			{
				const WeldPointLess cmp;
				if( cmp( a, o.a ) ) return true;
				if( cmp( o.a, a ) ) return false;
				return cmp( b, o.b );
			}
			bool operator==( const EndpointKey& o ) const
			{
				return PointEqualExact( a, o.a ) && PointEqualExact( b, o.b );
			}
		};

		struct SideEntry
		{
			EndpointKey key;
			unsigned int patchIdx;
			unsigned char sideIdx;
			std::array<Point3, N> pts;
			bool operator<( const SideEntry& o ) const
			{
				return key < o.key;
			}
		};

		std::vector<SideEntry> activeSides;
		activeSides.reserve( nPatches * 4 );

		for( std::size_t p = 0; p < nPatches; ++p ) {
			unsigned int collapsedCount = 0;
			for( int s = 0; s < 4; ++s ) {
				const PatchSideCurve<N>& sc = patchSides[p][s];
				if( sc.collapsed ) {
					++collapsedCount;
					continue;
				}
				const Point3& pStart = sc.pts[0];
				const Point3& pEnd   = sc.pts[N - 1];
				if( PointEqualExact( pStart, pEnd ) ) {
					return false; // Non-collapsed side loops back to its start corner.
				}
				EndpointKey key;
				if( less( pStart, pEnd ) ) {
					key.a = pStart; key.b = pEnd;
				} else {
					key.a = pEnd;   key.b = pStart;
				}
				activeSides.push_back( { key, sc.patchIdx, sc.sideIdx, sc.pts } );
			}
			if( collapsedCount >= 2 ) {
				return false;
			}
		}

		std::sort( activeSides.begin(), activeSides.end() );
		std::vector<std::vector<unsigned int> > patchAdj( nPatches );
		std::vector<unsigned char> unmatchedCount( nPatches, 0 );
		std::size_t totalUnmatched = 0;

		std::size_t i = 0;
		while( i < activeSides.size() ) {
			std::size_t j = i + 1;
			while( j < activeSides.size() && activeSides[j].key == activeSides[i].key ) {
				++j;
			}
			const std::size_t count = j - i;
			if( count > 2 ) {
				return false; // Non-manifold seam (>2 patches meeting at one boundary).
			}
			if( count == 2 ) {
				const SideEntry& sA = activeSides[i];
				const SideEntry& sB = activeSides[i + 1];
				if( sA.patchIdx == sB.patchIdx ) {
					return false; // Self-glued patch seam.
				}
				// Exact opposite-orientation curve control-point match required.
				for( std::size_t k = 0; k < N; ++k ) {
					if( !PointEqualExact( sA.pts[k], sB.pts[N - 1 - k] ) ) {
						return false; // Either opposing winding (same direction) or mismatched interior control points (crack).
					}
				}
				patchAdj[sA.patchIdx].push_back( sB.patchIdx );
				patchAdj[sB.patchIdx].push_back( sA.patchIdx );
			} else {
				++unmatchedCount[activeSides[i].patchIdx];
				++totalUnmatched;
			}
			i = j;
		}

		// Require a single seam-connected patch component.
		std::vector<unsigned char> visited( nPatches, 0 );
		std::vector<unsigned int> queue;
		queue.reserve( nPatches );
		visited[0] = 1;
		queue.push_back( 0 );
		for( std::size_t head = 0; head < queue.size(); ++head ) {
			const unsigned int u = queue[head];
			for( const unsigned int v : patchAdj[u] ) {
				if( !visited[v] ) {
					visited[v] = 1;
					queue.push_back( v );
				}
			}
		}
		if( queue.size() != nPatches ) {
			return false;
		}

		if( totalUnmatched == 0 ) {
			outAllMatchedClosed = true;
			return true;
		}

		// Free-face peeling on the patch complex.
		std::vector<unsigned char> peeled( nPatches, 0 );
		std::vector<unsigned int> peelQueue;
		peelQueue.reserve( nPatches );
		for( std::size_t p = 0; p < nPatches; ++p ) {
			if( unmatchedCount[p] > 0 ) {
				peeled[p] = 1;
				peelQueue.push_back( static_cast<unsigned int>( p ) );
			}
		}
		for( std::size_t head = 0; head < peelQueue.size(); ++head ) {
			const unsigned int u = peelQueue[head];
			for( const unsigned int v : patchAdj[u] ) {
				if( !peeled[v] ) {
					peeled[v] = 1;
					peelQueue.push_back( v );
				}
			}
		}
		if( peelQueue.size() != nPatches ) {
			return false;
		}
		outPeelsToEmptyOpen = true;
		return true;
	}
}

void GeometricUtilities::CertifyBilinearPatchSet(
	const std::vector<BilinearPatch>& patches,
	bool& outCertifiedWatertight,
	bool& outProvablyOpenSheet,
	bool& outNegativeOpenSignedVolume )
{
	outCertifiedWatertight = false;
	outProvablyOpenSheet = false;
	outNegativeOpenSignedVolume = false;
	if( patches.empty() ) {
		return;
	}

	const std::size_t nPatches = patches.size();
	std::vector<std::array<PatchSideCurve<2>, 4> > patchSides( nPatches );
	for( std::size_t p = 0; p < nPatches; ++p ) {
		const BilinearPatch& bp = patches[p];
		for( int k = 0; k < 4; ++k ) {
			if( !PointFinite( bp.pts[k] ) ) {
				return;
			}
		}
		// Canonical RISE corner mapping: pts[0]=(0,0), pts[1]=(0,1), pts[2]=(1,0), pts[3]=(1,1).
		const Point3 c00 = bp.pts[0];
		const Point3 c01 = bp.pts[1];
		const Point3 c10 = bp.pts[2];
		const Point3 c11 = bp.pts[3];
		patchSides[p][0] = { { c00, c10 }, static_cast<unsigned int>( p ), 0, PointEqualExact( c00, c10 ) };
		patchSides[p][1] = { { c10, c11 }, static_cast<unsigned int>( p ), 1, PointEqualExact( c10, c11 ) };
		patchSides[p][2] = { { c11, c01 }, static_cast<unsigned int>( p ), 2, PointEqualExact( c11, c01 ) };
		patchSides[p][3] = { { c01, c00 }, static_cast<unsigned int>( p ), 3, PointEqualExact( c01, c00 ) };
	}

	bool allMatchedClosed = false;
	bool peelsToEmptyOpen = false;
	if( !ValidateAndMatchPatchSides<2>( patchSides, allMatchedClosed, peelsToEmptyOpen ) ) {
		return;
	}

	struct BilinearSubRec
	{
		BilinearPatch subPatch;
		Vector3 coeffs[4];
		Vector3 unitN;
		unsigned int wCorners[4];
	};

	std::vector<BilinearSubRec> subRecs;
	subRecs.reserve( nPatches * 16 );
	std::vector<Point3> gridPoints;
	gridPoints.reserve( nPatches * 25 );

	for( std::size_t p = 0; p < nPatches; ++p ) {
		const BilinearPatch& bp = patches[p];
		// Check whole-patch analytic normal Bernstein coefficients at the 4 corners.
		const Vector3 nCorners[4] = {
			GeometricUtilities::BilinearPatchNormalAt( bp, 0.0, 0.0 ),
			GeometricUtilities::BilinearPatchNormalAt( bp, 1.0, 0.0 ),
			GeometricUtilities::BilinearPatchNormalAt( bp, 0.0, 1.0 ),
			GeometricUtilities::BilinearPatchNormalAt( bp, 1.0, 1.0 )
		};
		Vector3 patchSum = nCorners[0] + nCorners[1] + nCorners[2] + nCorners[3];
		const Scalar patchSumSq = Vector3Ops::SquaredModulus( patchSum );
		if( !( patchSumSq > Scalar( 1e-24 ) ) ) {
			return;
		}
		const Vector3 patchDir = patchSum * ( Scalar( 1 ) / std::sqrt( patchSumSq ) );
		unsigned int positiveCorners = 0;
		for( int c = 0; c < 4; ++c ) {
			const Scalar lenSq = Vector3Ops::SquaredModulus( nCorners[c] );
			if( lenSq > Scalar( 1e-24 ) ) {
				if( !( Vector3Ops::Dot( nCorners[c], patchDir ) > Scalar( 1e-6 ) * std::sqrt( lenSq ) ) ) {
					return; // Analytic bilinear normal reverses or folds within the patch.
				}
				++positiveCorners;
			}
		}
		if( positiveCorners < 3 ) {
			return;
		}

		for( unsigned int iv = 0; iv <= 4; ++iv ) {
			for( unsigned int iu = 0; iu <= 4; ++iu ) {
				gridPoints.push_back( EvalBilinearGrid4( bp, iu, iv ) );
			}
		}
	}

	std::vector<unsigned int> gridWeldId;
	Scalar gridEps = 0;
	if( !GeometricUtilities::WeldVertexPositions( gridPoints, gridWeldId, &gridEps ) ) {
		return;
	}

	std::vector<Point3> triCorners;
	triCorners.reserve( nPatches * 32 * 3 );
	Vector3 globalNormalSum( 0, 0, 0 );

	for( std::size_t p = 0; p < nPatches; ++p ) {
		const BilinearPatch& bp = patches[p];
		const std::size_t baseGrid = p * 25;
		for( unsigned int iv = 0; iv < 4; ++iv ) {
			const Scalar v0 = Scalar( 0.25 ) * Scalar( iv );
			const Scalar v1 = v0 + Scalar( 0.25 );
			for( unsigned int iu = 0; iu < 4; ++iu ) {
				const Scalar u0 = Scalar( 0.25 ) * Scalar( iu );
				const Scalar u1 = u0 + Scalar( 0.25 );
				BilinearSubRec rec;
				rec.subPatch = SubdivideBilinearPatchRange( bp, u0, u1, v0, v1 );
				rec.coeffs[0] = GeometricUtilities::BilinearPatchNormalAt( bp, u0, v0 );
				rec.coeffs[1] = GeometricUtilities::BilinearPatchNormalAt( bp, u1, v0 );
				rec.coeffs[2] = GeometricUtilities::BilinearPatchNormalAt( bp, u0, v1 );
				rec.coeffs[3] = GeometricUtilities::BilinearPatchNormalAt( bp, u1, v1 );
				const Vector3 sum = rec.coeffs[0] + rec.coeffs[1] + rec.coeffs[2] + rec.coeffs[3];
				const Scalar sumSq = Vector3Ops::SquaredModulus( sum );
				if( !( sumSq > Scalar( 1e-24 ) ) ) {
					return;
				}
				rec.unitN = sum * ( Scalar( 1 ) / std::sqrt( sumSq ) );
				globalNormalSum = globalNormalSum + rec.unitN;

				const std::size_t i00 = baseGrid + iv * 5 + iu;
				const std::size_t i10 = baseGrid + iv * 5 + ( iu + 1 );
				const std::size_t i01 = baseGrid + ( iv + 1 ) * 5 + iu;
				const std::size_t i11 = baseGrid + ( iv + 1 ) * 5 + ( iu + 1 );
				rec.wCorners[0] = gridWeldId[i00];
				rec.wCorners[1] = gridWeldId[i10];
				rec.wCorners[2] = gridWeldId[i01];
				rec.wCorners[3] = gridWeldId[i11];
				subRecs.push_back( rec );

				if( rec.wCorners[0] != rec.wCorners[1] &&
				    rec.wCorners[1] != rec.wCorners[2] &&
				    rec.wCorners[2] != rec.wCorners[0] ) {
					triCorners.push_back( gridPoints[i00] );
					triCorners.push_back( gridPoints[i10] );
					triCorners.push_back( gridPoints[i01] );
				}
				if( rec.wCorners[1] != rec.wCorners[3] &&
				    rec.wCorners[3] != rec.wCorners[2] &&
				    rec.wCorners[2] != rec.wCorners[1] ) {
					triCorners.push_back( gridPoints[i10] );
					triCorners.push_back( gridPoints[i11] );
					triCorners.push_back( gridPoints[i01] );
				}
			}
		}
	}

	bool isGlobalGraph = false;
	if( peelsToEmptyOpen && Vector3Ops::SquaredModulus( globalNormalSum ) > Scalar( 1e-24 ) ) {
		const Vector3 gDir = Vector3Ops::Normalize( globalNormalSum );
		isGlobalGraph = true;
		for( const BilinearSubRec& rec : subRecs ) {
			for( int c = 0; c < 4; ++c ) {
				const Scalar lenSq = Vector3Ops::SquaredModulus( rec.coeffs[c] );
				if( lenSq > Scalar( 1e-24 ) &&
				    !( Vector3Ops::Dot( rec.coeffs[c], gDir ) > Scalar( 1e-6 ) * std::sqrt( lenSq ) ) ) {
					isGlobalGraph = false;
					break;
				}
			}
			if( !isGlobalGraph ) break;
		}
	}

	if( !isGlobalGraph ) {
		for( std::size_t i = 0; i < subRecs.size(); ++i ) {
			for( std::size_t j = i + 1; j < subRecs.size(); ++j ) {
				bool shareCorner = false;
				for( int a = 0; a < 4 && !shareCorner; ++a ) {
					for( int b = 0; b < 4; ++b ) {
						if( subRecs[i].wCorners[a] == subRecs[j].wCorners[b] ) {
							shareCorner = true;
							break;
						}
					}
				}
				if( shareCorner ) {
					const Vector3 pairSum = subRecs[i].unitN + subRecs[j].unitN;
					const Scalar pairSumSq = Vector3Ops::SquaredModulus( pairSum );
					if( !( pairSumSq > Scalar( 1e-12 ) ) ) {
						return;
					}
					const Vector3 pairDir = pairSum * ( Scalar( 1 ) / std::sqrt( pairSumSq ) );
					for( int c = 0; c < 4; ++c ) {
						const Scalar lenSqI = Vector3Ops::SquaredModulus( subRecs[i].coeffs[c] );
						if( lenSqI > Scalar( 1e-24 ) &&
						    !( Vector3Ops::Dot( subRecs[i].coeffs[c], pairDir ) > Scalar( 1e-6 ) * std::sqrt( lenSqI ) ) ) {
							return;
						}
						const Scalar lenSqJ = Vector3Ops::SquaredModulus( subRecs[j].coeffs[c] );
						if( lenSqJ > Scalar( 1e-24 ) &&
						    !( Vector3Ops::Dot( subRecs[j].coeffs[c], pairDir ) > Scalar( 1e-6 ) * std::sqrt( lenSqJ ) ) ) {
							return;
						}
					}
				} else {
					if( !BilinearSubPatchesDisjointRecursive( subRecs[i].subPatch, subRecs[j].subPatch, 3 ) ) {
						return;
					}
				}
			}
		}
	}

	if( allMatchedClosed ) {
		outCertifiedWatertight = IsEmbeddedClosedOrientedTriangulation( triCorners );
	} else if( peelsToEmptyOpen ) {
		const OpenSurfaceCertificate cert = IsEmbeddedOpenOrientedSurface( triCorners );
		if( cert.certified ) {
			outProvablyOpenSheet = true;
			outNegativeOpenSignedVolume = ( cert.signedVolumeAboutCentre < Scalar( 0 ) );
		}
	}
}

void GeometricUtilities::CertifyBezierPatchSet(
	const std::vector<BezierPatch>& patches,
	bool& outCertifiedWatertight,
	bool& outProvablyOpenSheet,
	bool& outNegativeOpenSignedVolume )
{
	outCertifiedWatertight = false;
	outProvablyOpenSheet = false;
	outNegativeOpenSignedVolume = false;
	if( patches.empty() ) {
		return;
	}

	const std::size_t nPatches = patches.size();
	std::vector<std::array<PatchSideCurve<4>, 4> > patchSides( nPatches );
	for( std::size_t p = 0; p < nPatches; ++p ) {
		const BezierPatch& bp = patches[p];
		for( int i = 0; i < 4; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				if( !PointFinite( bp.c[i].pts[j] ) ) {
					return;
				}
			}
		}
		const std::array<Point3, 4> s0 = { bp.c[0].pts[0], bp.c[1].pts[0], bp.c[2].pts[0], bp.c[3].pts[0] };
		const std::array<Point3, 4> s1 = { bp.c[3].pts[0], bp.c[3].pts[1], bp.c[3].pts[2], bp.c[3].pts[3] };
		const std::array<Point3, 4> s2 = { bp.c[3].pts[3], bp.c[2].pts[3], bp.c[1].pts[3], bp.c[0].pts[3] };
		const std::array<Point3, 4> s3 = { bp.c[0].pts[3], bp.c[0].pts[2], bp.c[0].pts[1], bp.c[0].pts[0] };
		auto isCollapsed4 = []( const std::array<Point3, 4>& c ) {
			return PointEqualExact( c[0], c[1] ) && PointEqualExact( c[1], c[2] ) && PointEqualExact( c[2], c[3] );
		};
		patchSides[p][0] = { s0, static_cast<unsigned int>( p ), 0, isCollapsed4( s0 ) };
		patchSides[p][1] = { s1, static_cast<unsigned int>( p ), 1, isCollapsed4( s1 ) };
		patchSides[p][2] = { s2, static_cast<unsigned int>( p ), 2, isCollapsed4( s2 ) };
		patchSides[p][3] = { s3, static_cast<unsigned int>( p ), 3, isCollapsed4( s3 ) };
	}

	bool allMatchedClosed = false;
	bool peelsToEmptyOpen = false;
	if( !ValidateAndMatchPatchSides<4>( patchSides, allMatchedClosed, peelsToEmptyOpen ) ) {
		return;
	}

	struct BezierSubRec
	{
		BezierPatch subPatch;
		Vector3 coeffs[6][6];
		Vector3 unitN;
		unsigned int wCorners[4];
	};

	std::vector<BezierSubRec> subRecs;
	subRecs.reserve( nPatches * 16 );
	std::vector<Point3> gridPoints;
	gridPoints.reserve( nPatches * 25 );

	for( std::size_t p = 0; p < nPatches; ++p ) {
		const BezierPatch& bp = patches[p];
		for( unsigned int iv = 0; iv <= 4; ++iv ) {
			for( unsigned int iu = 0; iu <= 4; ++iu ) {
				gridPoints.push_back( EvalBezierGrid4( bp, iu, iv ) );
			}
		}
	}

	std::vector<unsigned int> gridWeldId;
	Scalar gridEps = 0;
	if( !GeometricUtilities::WeldVertexPositions( gridPoints, gridWeldId, &gridEps ) ) {
		return;
	}

	std::vector<Point3> triCorners;
	triCorners.reserve( nPatches * 32 * 3 );
	Vector3 globalNormalSum( 0, 0, 0 );

	for( std::size_t p = 0; p < nPatches; ++p ) {
		const BezierPatch& bp = patches[p];
		const std::size_t baseGrid = p * 25;
		for( unsigned int iv = 0; iv < 4; ++iv ) {
			const Scalar v0 = Scalar( 0.25 ) * Scalar( iv );
			const Scalar v1 = v0 + Scalar( 0.25 );
			for( unsigned int iu = 0; iu < 4; ++iu ) {
				const Scalar u0 = Scalar( 0.25 ) * Scalar( iu );
				const Scalar u1 = u0 + Scalar( 0.25 );
				BezierSubRec rec;
				rec.subPatch = SubdivideBezierPatchRange( bp, u0, u1, v0, v1 );
				ComputeBezierNormalBernsteinCoeffs( rec.subPatch, rec.coeffs );

				Vector3 sum( 0, 0, 0 );
				for( int a = 0; a < 6; ++a ) {
					for( int b = 0; b < 6; ++b ) {
						sum = sum + rec.coeffs[a][b];
					}
				}
				const Scalar sumSq = Vector3Ops::SquaredModulus( sum );
				if( !( sumSq > Scalar( 1e-24 ) ) ) {
					return;
				}
				rec.unitN = sum * ( Scalar( 1 ) / std::sqrt( sumSq ) );
				globalNormalSum = globalNormalSum + rec.unitN;

				// Analytic Bernstein normal-cone check on the sub-patch:
				// Every non-collapsed Bernstein coefficient must have strictly positive
				// projection onto rec.unitN, proving the analytic sub-patch has non-vanishing
				// Jacobian and is a smooth single-valued graph over the plane perp to rec.unitN.
				unsigned int positiveCount = 0;
				for( int a = 0; a < 6; ++a ) {
					for( int b = 0; b < 6; ++b ) {
						const Scalar lenSq = Vector3Ops::SquaredModulus( rec.coeffs[a][b] );
						const bool onCollapsedBoundary =
							( iv == 0 && b == 0 && patchSides[p][0].collapsed ) ||
							( iu == 3 && a == 5 && patchSides[p][1].collapsed ) ||
							( iv == 3 && b == 5 && patchSides[p][2].collapsed ) ||
							( iu == 0 && a == 0 && patchSides[p][3].collapsed );
						if( lenSq <= Scalar( 1e-24 ) ) {
							if( !onCollapsedBoundary ) {
								return;
							}
							continue;
						}
						if( !( Vector3Ops::Dot( rec.coeffs[a][b], rec.unitN ) > Scalar( 1e-6 ) * std::sqrt( lenSq ) ) ) {
							return;
						}
						++positiveCount;
					}
				}
				if( positiveCount < 24 ) {
					return;
				}

				const std::size_t i00 = baseGrid + iv * 5 + iu;
				const std::size_t i10 = baseGrid + iv * 5 + ( iu + 1 );
				const std::size_t i01 = baseGrid + ( iv + 1 ) * 5 + iu;
				const std::size_t i11 = baseGrid + ( iv + 1 ) * 5 + ( iu + 1 );
				rec.wCorners[0] = gridWeldId[i00];
				rec.wCorners[1] = gridWeldId[i10];
				rec.wCorners[2] = gridWeldId[i01];
				rec.wCorners[3] = gridWeldId[i11];
				subRecs.push_back( rec );

				if( rec.wCorners[0] != rec.wCorners[1] &&
				    rec.wCorners[1] != rec.wCorners[2] &&
				    rec.wCorners[2] != rec.wCorners[0] ) {
					triCorners.push_back( gridPoints[i00] );
					triCorners.push_back( gridPoints[i10] );
					triCorners.push_back( gridPoints[i01] );
				}
				if( rec.wCorners[1] != rec.wCorners[3] &&
				    rec.wCorners[3] != rec.wCorners[2] &&
				    rec.wCorners[2] != rec.wCorners[1] ) {
					triCorners.push_back( gridPoints[i10] );
					triCorners.push_back( gridPoints[i11] );
					triCorners.push_back( gridPoints[i01] );
				}
			}
		}
	}

	bool isGlobalGraph = false;
	if( peelsToEmptyOpen && Vector3Ops::SquaredModulus( globalNormalSum ) > Scalar( 1e-24 ) ) {
		const Vector3 gDir = Vector3Ops::Normalize( globalNormalSum );
		isGlobalGraph = true;
		for( const BezierSubRec& rec : subRecs ) {
			for( int a = 0; a < 6 && isGlobalGraph; ++a ) {
				for( int b = 0; b < 6; ++b ) {
					const Scalar lenSq = Vector3Ops::SquaredModulus( rec.coeffs[a][b] );
					if( lenSq > Scalar( 1e-24 ) &&
					    !( Vector3Ops::Dot( rec.coeffs[a][b], gDir ) > Scalar( 1e-6 ) * std::sqrt( lenSq ) ) ) {
						isGlobalGraph = false;
						break;
					}
				}
			}
		}
	}

	if( !isGlobalGraph ) {
		for( std::size_t i = 0; i < subRecs.size(); ++i ) {
			for( std::size_t j = i + 1; j < subRecs.size(); ++j ) {
				bool shareCorner = false;
				for( int a = 0; a < 4 && !shareCorner; ++a ) {
					for( int b = 0; b < 4; ++b ) {
						if( subRecs[i].wCorners[a] == subRecs[j].wCorners[b] ) {
							shareCorner = true;
							break;
						}
					}
				}
				if( shareCorner ) {
					const Vector3 pairSum = subRecs[i].unitN + subRecs[j].unitN;
					const Scalar pairSumSq = Vector3Ops::SquaredModulus( pairSum );
					if( !( pairSumSq > Scalar( 1e-12 ) ) ) {
						return;
					}
					const Vector3 pairDir = pairSum * ( Scalar( 1 ) / std::sqrt( pairSumSq ) );
					for( int a = 0; a < 6; ++a ) {
						for( int b = 0; b < 6; ++b ) {
							const Scalar lenSqI = Vector3Ops::SquaredModulus( subRecs[i].coeffs[a][b] );
							if( lenSqI > Scalar( 1e-24 ) &&
							    !( Vector3Ops::Dot( subRecs[i].coeffs[a][b], pairDir ) > Scalar( 1e-6 ) * std::sqrt( lenSqI ) ) ) {
								return;
							}
							const Scalar lenSqJ = Vector3Ops::SquaredModulus( subRecs[j].coeffs[a][b] );
							if( lenSqJ > Scalar( 1e-24 ) &&
							    !( Vector3Ops::Dot( subRecs[j].coeffs[a][b], pairDir ) > Scalar( 1e-6 ) * std::sqrt( lenSqJ ) ) ) {
								return;
							}
						}
					}
				} else {
					if( !BezierSubPatchesDisjointRecursive( subRecs[i].subPatch, subRecs[j].subPatch, 3 ) ) {
						return;
					}
				}
			}
		}
	}

	if( allMatchedClosed ) {
		outCertifiedWatertight = IsEmbeddedClosedOrientedTriangulation( triCorners );
	} else if( peelsToEmptyOpen ) {
		const OpenSurfaceCertificate cert = IsEmbeddedOpenOrientedSurface( triCorners );
		if( cert.certified ) {
			outProvablyOpenSheet = true;
			outNegativeOpenSignedVolume = ( cert.signedVolumeAboutCentre < Scalar( 0 ) );
		}
	}
}

