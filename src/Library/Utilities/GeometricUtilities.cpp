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
#include <map>
#include <atomic>
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

//! (Moved from TriangleMeshGeometryIndexed.cpp, DL-382.)
//! DL-143.  Welds `points` to a shared id per PHYSICAL corner, so
//! that watertightness (and anything else that needs "is this the
//! same vertex") can be decided by position rather than by which
//! array slot a per-corner import/tessellation pipeline happened to
//! allocate.  `outWeldedId[i]` is the welded id for `points[i]`.
//!
//! Tolerance: `eps = max(1e-9, 1e-6 * bboxDiagonal, minEps)`, i.e.
//! relative to THIS point set's own scale, not an absolute constant --
//! an absolute epsilon would either be too coarse for a millimetre-scale
//! asset or too fine to absorb import float noise on a kilometre-scale
//! one.
//!
//! The partition (DL-620 review, P3-2).  Points are visited in input
//! order.  A point joins the NEAREST existing representative within
//! `eps` (ties: the lowest id); with none within `eps` it becomes a
//! new representative, numbered in creation order.  The result is
//! therefore a pure function of the input sequence -- it does not
//! depend on the grid's cell size, hash-table capacity or bucket scan
//! order (an earlier first-match scan did, so two equally valid grids
//! could produce two different partitions for the same mesh).  The
//! weld is NOT an equivalence relation on raw positions: two points
//! within `eps` of each other can land in different classes, and two
//! points in one class can be up to `2 eps` apart.
//!
//! `outTransitive` (when non-null) reports whether the weld was
//! UNAMBIGUOUS: every point lies within `eps/2` of its representative
//! and representatives are pairwise more than `1.5 eps` apart.  Then
//! every point had exactly one representative within `eps`, so no
//! tie-break or visiting-order choice influenced its class, and points
//! in DIFFERENT classes are at least `eps/2` apart (not `eps`: a
//! transitive weld still does not certify that the classes are
//! `eps`-separated).  Callers use it to decide when an earlier weld's
//! labels can be compared against a new one.
//!
//! Grid: cell `6 eps` over coordinates RELATIVE to the bounding box's
//! low corner (always, DL-620 review P3-4: relative coordinates lie in
//! [0, diagonal], so the cell arithmetic's rounding error is a few ulps
//! of the diagonal -- about 1e-10 of `eps` -- however far the mesh is
//! translated; absolute coordinates at 1e9 x the mesh size made
//! `rx - floor(rx/cell)*cell` lose whole multiples of `eps`).  The home
//! cell is scanned first; an exact (bit-identical) duplicate, the common
//! case for shared corners, is its own nearest representative at
//! distance 0 and ends the scan there.  Otherwise, unless the point's
//! search box (`1.5 eps`, or `eps` once the weld is known to be
//! ambiguous, plus the rounding bound) lies inside its home cell, every
//! cell overlapping the box is scanned.  The cell is small (`6 eps`)
//! because each visited cell is scanned in full and representatives are
//! only `eps` apart: the per-cell count is what a dense mesh can blow up.
//!
//! O(n) expected.  DL-197: `outEps`, when non-null, receives the SAME
//! epsilon this function used, so a caller that needs a second
//! geometric test at "the tolerance the weld itself trusted" does not
//! re-derive the formula.
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
	// Cell size (DL-620 review round 2, P2).  Each lookup scans every
	// representative in the cells it visits, and representatives are only
	// guaranteed to be more than eps apart, so a cell of k eps can hold
	// ~k^2 representatives on a surface and ~k^3 in a volume.  32 eps let a
	// vertex spacing just above eps (the DL-150 regime: a dense model whose
	// epsilon one stray far vertex has inflated) put ~1,000 representatives
	// in a cell and slowed the weld by ~20x on a 490k-vertex sheet.  Smaller
	// cells cost hash lookups instead: the 1.5 eps search box spans up to
	// two cells per axis, and only a point more than that far inside its
	// home cell on every axis can skip its neighbours.  Measured on 490k
	// vertices (OpenSheetCertificateTiming, weld alone, n=5): 3 eps reads
	// 18 / 48 ms (dense / ordinary sheet), 6 eps 15 / 21 ms, 8 eps 36 / 24
	// ms, 32 eps 237 / 12 ms.  6 eps keeps the dense case bounded (about
	// 30 representatives per cell on a surface) for ~10 ms on an ordinary
	// one.
	const Scalar cell = Scalar( 6 ) * eps;
	if( !std::isfinite( diag ) || !std::isfinite( cell ) || !( cell > 0 ) ) return false;
	if( outEps ) { *outEps = eps; }

	// Relative coordinates lie in [0, extent] with extent <= diag <= 1e6 eps,
	// so cell indices are at most ~167,000 (or 0 when minEps dominates): no
	// integer range concern.  `slack` bounds the rounding of the relative
	// coordinate, of its quotient by `cell` and of `fx * cell` -- all a few
	// ulps of a magnitude <= extent + cell -- and is added to the search
	// radius below.
	const Point3 origin = bbox.ll;
	const Scalar maxExtent = std::max( extents.x, std::max( extents.y, extents.z ) );
	const Scalar slack = Scalar( 64 ) * std::numeric_limits<Scalar>::epsilon() * ( maxExtent + cell );
	if( !( maxExtent / cell < Scalar( 1e15 ) ) ) return false;
	auto cellCoord = [cell]( const Scalar v ) -> std::int64_t {
		return static_cast<std::int64_t>( std::floor( double( v / cell ) ) );
	};
	// A simple, well-distributed combine for the 3D cell key -- collisions are
	// fine (a false-positive bucket collision only costs an extra distance
	// check, never a wrong weld, since every candidate found is still
	// distance-checked against `eps` below).
	auto cellKey = []( std::int64_t x, std::int64_t y, std::int64_t z ) -> std::uint64_t {
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
		const std::int64_t cx = cellCoord( rx );
		const std::int64_t cy = cellCoord( ry );
		const std::int64_t cz = cellCoord( rz );
		const std::uint64_t homeKey = cellKey( cx, cy, cz );

		std::size_t homeSlot = mixKey( homeKey ) & tableMask;
		while( grid[homeSlot].head != 0xFFFFFFFFu && grid[homeSlot].key != homeKey ) {
			homeSlot = ( homeSlot + 1 ) & tableMask;
		}

		// Nearest representative within eps (ties: lowest id), plus the
		// ambiguity bookkeeping behind `outTransitive`.
		unsigned int bestId = 0xFFFFFFFFu;
		Scalar bestSq = 0;
		unsigned int withinEps = 0;
		bool exactMatch = false;
		const auto consider = [&]( const unsigned int cand ) {
			const Point3& wp = weldedPos[cand];
			if( wp.x == p.x && wp.y == p.y && wp.z == p.z ) {
				bestId = cand;
				bestSq = 0;
				exactMatch = true;
				return;
			}
			const Scalar dSq = Vector3Ops::SquaredModulus( Vector3Ops::mkVector3( wp, p ) );
			if( dSq <= epsSq ) {
				++withinEps;
				if( bestId == 0xFFFFFFFFu || dSq < bestSq || ( dSq == bestSq && cand < bestId ) ) {
					bestId = cand;
					bestSq = dSq;
				}
			} else if( dSq <= sepEpsSq ) {
				transitive = false;
			}
		};

		for( unsigned int cand = grid[homeSlot].head; cand != 0xFFFFFFFFu && !exactMatch; cand = nextInCell[cand] ) {
			consider( cand );
		}

		if( !exactMatch ) {
			// Within eps decides membership; within 1.5 eps decides only
			// the transitivity report, so once that is false eps suffices.
			const Scalar searchRad = ( transitive ? sepEps : eps ) + slack;
			// A point whose search box lies inside its home cell (with the
			// rounding slack once more for `cx * cell`, an integer below 2^53
			// times `cell`, rounded once) has already seen every candidate.
			const Scalar inLo = searchRad + slack;
			const Scalar inHi = cell - inLo;
			const Scalar dx = rx - Scalar( cx ) * cell;
			const Scalar dy = ry - Scalar( cy ) * cell;
			const Scalar dz = rz - Scalar( cz ) * cell;
			const bool insideHome = dx > inLo && dx < inHi && dy > inLo && dy < inHi && dz > inLo && dz < inHi;
			const std::int64_t cx0 = cellCoord( rx - searchRad ), cx1 = insideHome ? cx0 - 1 : cellCoord( rx + searchRad );
			const std::int64_t cy0 = cellCoord( ry - searchRad ), cy1 = cellCoord( ry + searchRad );
			const std::int64_t cz0 = cellCoord( rz - searchRad ), cz1 = cellCoord( rz + searchRad );
			for( std::int64_t nz = cz0; nz <= cz1 && !exactMatch; ++nz ) {
				for( std::int64_t ny = cy0; ny <= cy1 && !exactMatch; ++ny ) {
					for( std::int64_t nx = cx0; nx <= cx1 && !exactMatch; ++nx ) {
						if( nx == cx && ny == cy && nz == cz ) {
							continue;
						}
						for( unsigned int cand = findBucketHead( cellKey( nx, ny, nz ) );
							cand != 0xFFFFFFFFu && !exactMatch; cand = nextInCell[cand] ) {
							consider( cand );
						}
					}
				}
			}
			if( withinEps > 1 || ( withinEps == 1 && bestSq > halfEpsSq ) ) {
				transitive = false;
			}
		}

		unsigned int foundId = bestId;
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
	// At most 3 nTris distinct edges (a manifold mesh has about 1.5 nTris),
	// so a capacity of at least 4 nTris keeps the worst-case load below 3/4
	// and the typical one near 3/8 (DL-620 review P3-1: 6 nTris cost ~200 MB
	// at 980k triangles for a typical load of 0.18).
	std::size_t edgeCap = 16;
	while( edgeCap < nTris * 4 ) {
		edgeCap <<= 1;
	}
	const std::size_t edgeMask = edgeCap - 1;
	std::vector<EdgeUse> edgeTable( edgeCap );
	std::vector<unsigned int> triEdgeSlot( 3 * nTris, 0u );
	const auto triEdge = [&]( const std::size_t i ) -> EdgeUse* { return &edgeTable[triEdgeSlot[i]]; };
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
			triEdgeSlot[3 * t + k] = static_cast<unsigned int>( slot );
		}
	}
	bool hasBoundaryEdge = false;
	for( std::size_t i = 0; i < 3 * nTris; ++i ) {
		const EdgeUse* e = triEdge( i );
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
				const EdgeUse* e = triEdge( 3 * t + k );
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
				if( triEdge( 3 * t + k )->count == 1 ) {
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
				EdgeUse* e = triEdge( 3 * t + k );
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
		Point3 gHi( -RISE_INFINITY, -RISE_INFINITY, -RISE_INFINITY );
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
			if( !std::isfinite( l.x ) || !std::isfinite( l.y ) || !std::isfinite( l.z ) ||
				!std::isfinite( h.x ) || !std::isfinite( h.y ) || !std::isfinite( h.z ) ) {
				return result;	// P3-5: non-finite geometry is never certified
			}
			lo[t] = l;
			hi[t] = h;
			gLo.x = std::min( gLo.x, l.x ); gLo.y = std::min( gLo.y, l.y ); gLo.z = std::min( gLo.z, l.z );
			gHi.x = std::max( gHi.x, h.x ); gHi.y = std::max( gHi.y, h.y ); gHi.z = std::max( gHi.z, h.z );
			meanExtent += std::max( h.x - l.x, std::max( h.y - l.y, h.z - l.z ) );
		}
		// DL-620 review P3-5: the grid index of every box corner must be a
		// finite integer well inside int64 before it is cast (an out-of-range
		// float-to-integer conversion is undefined behaviour), and the
		// insertion count must actually meet its budget.  Steps 2b-3 have
		// proved the surface connected through welded corners, and welded
		// corners lie within 2 eps of each other, so along any axis the span
		// is at most the sum of all box extents: span <= nTris * cell < 2^30
		// cells.  A wider span means the weld labels join geometrically
		// distant corners (only a caller-supplied weld can do that) or the
		// coordinates are degenerate; like non-finite or overflowing input,
		// or a budget that cannot be met, it is REFUSED (conservative) rather
		// than tested on a grid whose single huge cell would make the pair
		// search quadratic.
		const Scalar span = std::max( gHi.x - gLo.x, std::max( gHi.y - gLo.y, gHi.z - gLo.z ) );
		Scalar cell = meanExtent / Scalar( nTris );
		if( !( cell > 0 ) || !std::isfinite( cell ) || !std::isfinite( span ) ) {
			return result;
		}
		// 2^40 cells per axis keeps every index, and the +1 / product
		// arithmetic below, exact in double and far inside int64.
		if( !( span / cell <= Scalar( 1099511627776.0 ) ) ) {
			return result;
		}
		const auto coord = [&]( const Scalar v, const Scalar o ) -> std::int64_t {
			return static_cast<std::int64_t>( std::floor( double( ( v - o ) / cell ) ) );
		};
		// Bound the insertions: a few huge triangles in a fine mesh would
		// otherwise touch very many cells; coarsen until the total is linear.
		// Doubling ends with at most 8 cells per triangle once the cell
		// exceeds the span, so 64 attempts always suffice for valid input.
		const double budget = 64.0 * double( nTris ) + 4096.0;
		double finalTotal = 0;
		bool withinBudget = false;
		for( int attempt = 0; attempt < 64; ++attempt ) {
			double total = 0;
			for( std::size_t t = 0; t < nTris && total <= budget; ++t ) {
				total += double( coord( hi[t].x, gLo.x ) - coord( lo[t].x, gLo.x ) + 1 )
					* double( coord( hi[t].y, gLo.y ) - coord( lo[t].y, gLo.y ) + 1 )
					* double( coord( hi[t].z, gLo.z ) - coord( lo[t].z, gLo.z ) + 1 );
			}
			finalTotal = total;
			if( total <= budget ) {
				withinBudget = true;
				break;
			}
			cell *= 2;
			if( !std::isfinite( cell ) ) {
				break;
			}
		}
		if( !withinBudget ) {
			result.budgetRefusal = true;
			return result;
		}
		// DL-620 review round 2, P3-a.  Entry nodes, cell slots and the
		// 0xFFFFFFFF end-of-list sentinel are 32-bit: past 2^31 insertions a
		// node index (or, at half load, a slot index) could wrap or alias the
		// sentinel and silently drop pairs from the test.  The budget allows
		// up to 64 nTris + 4096 entries, which exceeds that for nTris above
		// ~33.5M, so refuse there rather than test a truncated list.
		if( !( finalTotal < 2147483648.0 ) ) {
			return result;
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
		// Occupied cells are typically about nTris / 2 (measured: 488,601 for
		// a 980,000-triangle sheet), and the table doubles whenever it
		// reaches half load, so start at nTris rather than 4 nTris (DL-620
		// review P3-1: 168 MB of mostly empty slots at that size).
		std::size_t cellCap = 16;
		while( cellCap < nTris ) {
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
		// DL-620 review round 2, P3-b.  The insertion budget bounds entries,
		// not pairs: when a few huge triangles force the cell to double far
		// past the size of the small ones, many small triangles share one
		// cell and the all-pairs scan below goes quadratic (a 1e5-wide
		// triangle on a 180k-triangle unit sheet puts the whole sheet in one
		// or two cells: ~1.6e10 pairs).  Count the candidate pairs first and
		// refuse past a linear budget.  Refusal is the conservative answer:
		// this certificate never claims a surface it has not fully tested.
		{
			double pairs = 0;
			for( const unsigned int slotIdx : multiCellSlots ) {
				const double c = double( cellTable[slotIdx].count );
				pairs += 0.5 * c * ( c - 1.0 );
			}
			if( pairs > 1024.0 * double( nTris ) + 16777216.0 ) {
				result.budgetRefusal = true;
				return result;
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
	//////////////////////////////////////////////////////////////////////
	// DL-616: patch-set certifier.
	//
	// The claim either output makes is about the WHOLE set, so every step
	// below is a proof, and anything a step cannot prove is a refusal:
	//
	//  T. TOPOLOGY from exact seam identity only.  Every side is a cubic
	//     control polygon; two sides are glued iff one is the bitwise
	//     reverse of the other (the reversal is the orientation check).
	//     A side shared by more than two patches, a self-glued patch, a
	//     side whose endpoints coincide (collapsed / looping) or a patch
	//     with two coincident corners is refused.  Nothing is welded by a
	//     tolerance: two corners that merely coincide in space without a
	//     glued seam between them are DIFFERENT vertices, so the geometric
	//     steps below must prove them apart -- which is how a T-junction,
	//     a pinch vertex or a corner that touches without a seam is
	//     refused rather than silently stitched.
	//  G. EMBEDDING on an n x n grid of sub-patches per patch (n = 4, then
	//     8 on a geometric refusal).  Sub-patch vertices are identified by
	//     union-find over the glued seams (exact topology again), and
	//     glued sub-patch boundaries are ASSIGNED bitwise equal (after a
	//     tolerance sanity check), never left to two rounding sequences.
	//     Then:
	//      - every sub-patch is INJECTIVE: if every cross product
	//        du_i x dv_k of a u- and a v-derivative control vector has a
	//        positive component along one direction V, the map is
	//        injective, because P(x) - P(y) = a du + b dv with a in the
	//        convex hull of the u-derivatives and b in that of the
	//        v-derivatives (mean-value theorem along the segment), and
	//        (a x b).V is bilinear, so its minimum over the two hulls is
	//        attained at control vectors and is positive -- a x b != 0;
	//      - every pair of sub-patches sharing a SIDE is injective as a
	//        union: the two nets, rotated so the shared side is v = 1 of
	//        one and v = 0 of the other, parameterize one continuous
	//        piecewise-polynomial map of a rectangle, and the same
	//        argument applies to the pooled derivative vectors;
	//      - every pair sharing only a VERTEX X is strictly separated by
	//        a plane through X: every control point but the corner on one
	//        side, every one of the other's on the other side; a convex
	//        combination with weight off the corner is then strictly on
	//        its own side, so the two meet only at X;
	//      - every other pair is DISJOINT, proved by recursive subdivision
	//        of control-point boxes (inflated by a relative epsilon).
	//     Together: the map from the glued topological complex to space is
	//     injective, i.e. an embedding.
	//  C. CLASSIFICATION.  The complex is connected.  No free side: an
	//     embedded closed oriented surface bounds a region, and the EXACT
	//     signed volume (1/3 of the integral of P.(Pu x Pv), integrated in
	//     Bernstein form) gives its orientation; positive certifies
	//     watertight, negative or tiny is refused.  With a free side: a
	//     connected compact surface with non-empty boundary encloses no
	//     volume -> provably open; the signed volume about the bbox centre
	//     drives the inward-winding warning.
	//
	// Cost: sub-patch pairs come from a sort-and-sweep over boxes, and a
	// work budget refuses (never certifies) a set it cannot finish.
	//////////////////////////////////////////////////////////////////////

	typedef std::array<Point3, 4> PatchCurve;

	inline bool PatchPointFinite( const Point3& p )
	{
		return std::isfinite( p.x ) && std::isfinite( p.y ) && std::isfinite( p.z );
	}

	inline bool PatchPointEqual( const Point3& a, const Point3& b )
	{
		return a.x == b.x && a.y == b.y && a.z == b.z;
	}

	inline bool PatchPointLess( const Point3& a, const Point3& b )
	{
		if( a.x != b.x ) return a.x < b.x;
		if( a.y != b.y ) return a.y < b.y;
		return a.z < b.z;
	}

	inline bool PatchCurveEqual( const PatchCurve& a, const PatchCurve& b )
	{
		for( int k = 0; k < 4; ++k ) {
			if( !PatchPointEqual( a[k], b[k] ) ) return false;
		}
		return true;
	}

	inline PatchCurve ReversePatchCurve( const PatchCurve& c )
	{
		return PatchCurve{ { c[3], c[2], c[1], c[0] } };
	}

	inline Point3 BlossomCubic( const PatchCurve& P, Scalar t1, Scalar t2, Scalar t3 )
	{
		Point3 a[3], b[2];
		for( int k = 0; k < 3; ++k ) a[k] = Point3Ops::WeightedAverage2( P[k + 1], P[k], t1 );
		for( int k = 0; k < 2; ++k ) b[k] = Point3Ops::WeightedAverage2( a[k + 1], a[k], t2 );
		return Point3Ops::WeightedAverage2( b[1], b[0], t3 );
	}

	//! Control polygon of the cubic `P` restricted to [s0, s1] (blossoming).
	//! The endpoints are exact at s = 0 and s = 1.
	inline PatchCurve SubCubicSegment( const PatchCurve& P, Scalar s0, Scalar s1 )
	{
		if( PatchPointEqual( P[0], P[1] ) && PatchPointEqual( P[0], P[2] ) && PatchPointEqual( P[0], P[3] ) ) {
			return P;
		}
		return PatchCurve{ {
			BlossomCubic( P, s0, s0, s0 ),
			BlossomCubic( P, s0, s0, s1 ),
			BlossomCubic( P, s0, s1, s1 ),
			BlossomCubic( P, s1, s1, s1 ) } };
	}

	inline BezierPatch SubdivideBezierPatchRange(
		const BezierPatch& p,
		Scalar u0, Scalar u1,
		Scalar v0, Scalar v1 )
	{
		PatchCurve R[4];
		BezierPatch out;
		for( int i = 0; i < 4; ++i ) {
			const PatchCurve row = { { p.c[i].pts[0], p.c[i].pts[1], p.c[i].pts[2], p.c[i].pts[3] } };
			R[i] = SubCubicSegment( row, v0, v1 );
		}
		for( int l = 0; l < 4; ++l ) {
			const PatchCurve col = { { R[0][l], R[1][l], R[2][l], R[3][l] } };
			const PatchCurve sub = SubCubicSegment( col, u0, u1 );
			for( int k = 0; k < 4; ++k ) {
				out.c[k].pts[l] = sub[k];
			}
		}
		return out;
	}

	//! Bernstein coefficients N[a][b] (degree 5 x 5) of (Pu x Pv) / 9.
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

	// Net corners / sides, counter-clockwise in (u, v): corner k is where
	// side k starts; side k runs from corner k to corner k+1.
	inline void NetCornerIndex( int k, int& i, int& j )
	{
		static const int I[4] = { 0, 3, 3, 0 };
		static const int J[4] = { 0, 0, 3, 3 };
		i = I[k]; j = J[k];
	}

	inline const Point3& NetCorner( const BezierPatch& q, int k )
	{
		int i, j;
		NetCornerIndex( k, i, j );
		return q.c[i].pts[j];
	}

	inline void NetSideIndex( int s, int m, int& i, int& j )
	{
		switch( s ) {
		default:
		case 0: i = m;     j = 0;     break;
		case 1: i = 3;     j = m;     break;
		case 2: i = 3 - m; j = 3;     break;
		case 3: i = 0;     j = 3 - m; break;
		}
	}

	inline bool IsOnNetSide( int s, int i, int j )
	{
		return ( s == 0 && j == 0 ) ||
		       ( s == 1 && i == 3 ) ||
		       ( s == 2 && j == 3 ) ||
		       ( s == 3 && i == 0 );
	}

	inline PatchCurve NetSide( const BezierPatch& q, int s )
	{
		PatchCurve c;
		for( int m = 0; m < 4; ++m ) {
			int i, j;
			NetSideIndex( s, m, i, j );
			c[m] = q.c[i].pts[j];
		}
		return c;
	}

	inline void SetNetSide( BezierPatch& q, int s, const PatchCurve& c )
	{
		for( int m = 0; m < 4; ++m ) {
			int i, j;
			NetSideIndex( s, m, i, j );
			q.c[i].pts[j] = c[m];
		}
	}

	//! Rotate the parameter domain a quarter turn r times (orientation
	//! preserving): the result's side k is the input's side (k + r) % 4
	//! and its corner k is the input's corner (k + r) % 4.
	inline BezierPatch RotateNet( const BezierPatch& q, int r )
	{
		BezierPatch cur = q;
		for( int t = 0; t < ( r & 3 ); ++t ) {
			BezierPatch nxt;
			for( int i = 0; i < 4; ++i ) {
				for( int j = 0; j < 4; ++j ) {
					nxt.c[i].pts[j] = cur.c[3 - j].pts[i];
				}
			}
			cur = nxt;
		}
		return cur;
	}

	struct PatchCertWork
	{
		std::size_t	remaining;
		bool		exhausted = false;
		bool Spend( std::size_t units )
		{
			if( units > remaining ) {
				remaining = 0;
				exhausted = true;
				return false;
			}
			remaining -= units;
			return true;
		}
	};

	inline BoundingBox InflatedNetBox( const BezierPatch& q, Scalar inflate )
	{
		BoundingBox b = GeometricUtilities::BezierPatchBoundingBox( q );
		b.ll = Point3( b.ll.x - inflate, b.ll.y - inflate, b.ll.z - inflate );
		b.ur = Point3( b.ur.x + inflate, b.ur.y + inflate, b.ur.z + inflate );
		return b;
	}

	//! Closed comparison on inflated boxes: touching reads as overlapping.
	inline bool PatchBoxesOverlap( const BoundingBox& a, const BoundingBox& b )
	{
		return !( a.ur.x < b.ll.x || b.ur.x < a.ll.x ||
		          a.ur.y < b.ll.y || b.ur.y < a.ll.y ||
		          a.ur.z < b.ll.z || b.ur.z < a.ll.z );
	}

	//! TRUE only when the two nets' surfaces are proved disjoint.
	bool NetsProvablyDisjoint(
		const BezierPatch& a, int levelA,
		const BezierPatch& b, int levelB,
		Scalar inflate,
		PatchCertWork& work )
	{
		if( !work.Spend( 1 ) ) {
			return false;
		}
		const BoundingBox boxA = InflatedNetBox( a, inflate );
		const BoundingBox boxB = InflatedNetBox( b, inflate );
		if( !PatchBoxesOverlap( boxA, boxB ) ) {
			return true;
		}
		static const int kMaxLevel = 5;
		const Scalar diagA = Vector3Ops::Magnitude( Vector3Ops::mkVector3( boxA.ur, boxA.ll ) );
		const Scalar diagB = Vector3Ops::Magnitude( Vector3Ops::mkVector3( boxB.ur, boxB.ll ) );
		const bool splitA = ( levelA < kMaxLevel ) && ( diagA >= diagB || levelB >= kMaxLevel );
		const bool splitB = !splitA && ( levelB < kMaxLevel );
		if( !splitA && !splitB ) {
			return false;
		}
		const BezierPatch& big = splitA ? a : b;
		for( int iu = 0; iu < 2; ++iu ) {
			for( int iv = 0; iv < 2; ++iv ) {
				const Scalar u0 = Scalar( 0.5 ) * Scalar( iu );
				const Scalar v0 = Scalar( 0.5 ) * Scalar( iv );
				const BezierPatch sub = SubdivideBezierPatchRange( big, u0, u0 + Scalar( 0.5 ), v0, v0 + Scalar( 0.5 ) );
				const bool ok = splitA ?
					NetsProvablyDisjoint( sub, levelA + 1, b, levelB, inflate, work ) :
					NetsProvablyDisjoint( a, levelA, sub, levelB + 1, inflate, work );
				if( !ok ) {
					return false;
				}
			}
		}
		return true;
	}

	//! The injectivity proof (see the header comment): every active
	//! u-derivative control vector crossed with every active v-derivative
	//! control vector, over all the nets given, has a strictly positive
	//! component along one direction.  On a net with a collapsed pole side
	//! `collapsedSides[q]`, the three tangent-difference vectors along the
	//! collapsed side are identically zero and carry zero weight in the
	//! factored Bernstein derivative, so the remaining 9 x 12 pairs govern
	//! injectivity on the quotient domain.  `outV` receives that direction.
	bool CrossConeProof( const BezierPatch* nets, const int* collapsedSides, int count, Vector3& outV )
	{
		Vector3 du[2 * 12], dv[2 * 12];
		int nu = 0, nv = 0;
		for( int q = 0; q < count; ++q ) {
			const BezierPatch& net = nets[q];
			const int cs = collapsedSides ? collapsedSides[q] : -1;
			for( int i = 0; i < 3; ++i ) {
				for( int j = 0; j < 4; ++j ) {
					if( ( cs == 0 && j == 0 ) || ( cs == 2 && j == 3 ) ) {
						continue;
					}
					du[nu++] = Vector3Ops::mkVector3( net.c[i + 1].pts[j], net.c[i].pts[j] );
				}
			}
			for( int i = 0; i < 4; ++i ) {
				for( int j = 0; j < 3; ++j ) {
					if( ( cs == 3 && i == 0 ) || ( cs == 1 && i == 3 ) ) {
						continue;
					}
					dv[nv++] = Vector3Ops::mkVector3( net.c[i].pts[j + 1], net.c[i].pts[j] );
				}
			}
		}
		Vector3 sum( 0, 0, 0 );
		for( int a = 0; a < nu; ++a ) {
			for( int b = 0; b < nv; ++b ) {
				const Vector3 c = Vector3Ops::Cross( du[a], dv[b] );
				const Scalar len = Vector3Ops::Magnitude( c );
				if( !( len > 0 ) ) {
					return false;	// a vanishing or parallel derivative pair: not provably immersed
				}
				sum = sum + c * ( Scalar( 1 ) / len );
			}
		}
		const Scalar sumLen = Vector3Ops::Magnitude( sum );
		if( !( sumLen > 0 ) || !std::isfinite( sumLen ) ) {
			return false;
		}
		const Vector3 V = sum * ( Scalar( 1 ) / sumLen );
		for( int a = 0; a < nu; ++a ) {
			for( int b = 0; b < nv; ++b ) {
				const Vector3 c = Vector3Ops::Cross( du[a], dv[b] );
				if( !( Vector3Ops::Dot( c, V ) > Scalar( 1e-12 ) * Vector3Ops::Magnitude( c ) ) ) {
					return false;
				}
			}
		}
		outV = V;
		return true;
	}

	//! TRUE when `w` puts every control point of `a` not at `X` (either the
	//! single corner `ka` or the 4 control points of its collapsed side
	//! `csA` at `X`) strictly on the positive side of the plane through
	//! `X`, and every control point of `b` not at `X` strictly on the
	//! negative side.
	bool PlaneSeparatesAtCorner(
		const BezierPatch& a, int csA, int ka,
		const BezierPatch& b, int csB, int kb,
		const Point3& X,
		const Vector3& w )
	{
		const Scalar wLen = Vector3Ops::Magnitude( w );
		if( !( wLen > 0 ) || !std::isfinite( wLen ) ) {
			return false;
		}
		int ia, ja, ib, jb;
		NetCornerIndex( ka, ia, ja );
		NetCornerIndex( kb, ib, jb );
		for( int i = 0; i < 4; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				const bool skipA = csA >= 0 ? IsOnNetSide( csA, i, j ) : ( i == ia && j == ja );
				if( !skipA ) {
					const Vector3 d = Vector3Ops::mkVector3( a.c[i].pts[j], X );
					if( !( Vector3Ops::Dot( d, w ) > Scalar( 1e-12 ) * wLen * Vector3Ops::Magnitude( d ) ) ) {
						return false;
					}
				}
				const bool skipB = csB >= 0 ? IsOnNetSide( csB, i, j ) : ( i == ib && j == jb );
				if( !skipB ) {
					const Vector3 d = Vector3Ops::mkVector3( b.c[i].pts[j], X );
					if( !( -Vector3Ops::Dot( d, w ) > Scalar( 1e-12 ) * wLen * Vector3Ops::Magnitude( d ) ) ) {
						return false;
					}
				}
			}
		}
		return true;
	}

	//! Unit radial edge directions leaving corner `k` (or collapsed pole
	//! side `csAtX >= 0`) of net `q`.
	inline void CornerRadialEdges( const BezierPatch& q, int csAtX, int k, Vector3& e1, Vector3& e2 )
	{
		const Point3& X = NetCorner( q, csAtX >= 0 ? csAtX : k );
		const PatchCurve out = NetSide( q, csAtX >= 0 ? ( ( csAtX + 1 ) & 3 ) : k );
		const PatchCurve in = NetSide( q, csAtX >= 0 ? ( ( csAtX + 3 ) & 3 ) : ( ( k + 3 ) & 3 ) );
		e1 = Vector3Ops::mkVector3( out[1], X );
		e2 = Vector3Ops::mkVector3( in[2], X );
		const Scalar l1 = Vector3Ops::Magnitude( e1 );
		const Scalar l2 = Vector3Ops::Magnitude( e2 );
		if( l1 > 0 ) e1 = e1 * ( Scalar( 1 ) / l1 );
		if( l2 > 0 ) e2 = e2 * ( Scalar( 1 ) / l2 );
	}

	inline Vector3 NetCentroidOffset( const BezierPatch& q, const Point3& X )
	{
		Vector3 s( 0, 0, 0 );
		for( int i = 0; i < 4; ++i ) {
			for( int j = 0; j < 4; ++j ) {
				s = s + Vector3Ops::mkVector3( q.c[i].pts[j], X );
			}
		}
		const Scalar l = Vector3Ops::Magnitude( s );
		return l > 0 ? s * ( Scalar( 1 ) / l ) : s;
	}

	bool CornerPairSeparated(
		const BezierPatch& a, int csA, int ka,
		const BezierPatch& b, int csB, int kb,
		const Vector3& normalHint )
	{
		const Point3& X = NetCorner( a, csA >= 0 ? csA : ka );
		if( !PatchPointEqual( X, NetCorner( b, csB >= 0 ? csB : kb ) ) ) {
			return false;
		}
		Vector3 e1A, e2A, e1B, e2B;
		CornerRadialEdges( a, csA, ka, e1A, e2A );
		CornerRadialEdges( b, csB, kb, e1B, e2B );
		Vector3 bisA = e1A + e2A;
		Vector3 bisB = e1B + e2B;
		const Scalar la = Vector3Ops::Magnitude( bisA );
		const Scalar lb = Vector3Ops::Magnitude( bisB );
		if( la > 0 ) bisA = bisA * ( Scalar( 1 ) / la );
		if( lb > 0 ) bisB = bisB * ( Scalar( 1 ) / lb );
		const Vector3 candidates[4] = {
			bisA - bisB,
			NetCentroidOffset( a, X ) - NetCentroidOffset( b, X ),
			e2A - e1B,
			e1A - e2B
		};
		const Vector3 normalCandidates[4] = {
			normalHint,
			Vector3Ops::Cross( e1A, e2A ) + Vector3Ops::Cross( e1B, e2B ),
			Vector3Ops::Cross( e2A - e1A, e1B - e1A ),
			Vector3Ops::Cross( e2B - e1B, e1A - e1B )
		};
		for( const Vector3& w : candidates ) {
			if( PlaneSeparatesAtCorner( a, csA, ka, b, csB, kb, X, w ) ) {
				return true;
			}
		}
		for( const Vector3& nh : normalCandidates ) {
			const Scalar nLen = Vector3Ops::Magnitude( nh );
			if( !( nLen > 0 ) || !std::isfinite( nLen ) ) {
				continue;
			}
			const Vector3 n = nh * ( Scalar( 1 ) / nLen );
			for( const Vector3& w : candidates ) {
				// The same direction with the shared normal / pole axis
				// projected out, so curvature or cone slope away from the
				// tangent plane cannot tip a point across the separating plane.
				const Vector3 wt = w - n * Vector3Ops::Dot( w, n );
				if( PlaneSeparatesAtCorner( a, csA, ka, b, csB, kb, X, wt ) ) {
					return true;
				}
			}
			auto projUnit = [&]( const Vector3& e ) {
				const Vector3 p = e - n * Vector3Ops::Dot( e, n );
				const Scalar pl = Vector3Ops::Magnitude( p );
				return pl > 0 ? p * ( Scalar( 1 ) / pl ) : p;
			};
			const Vector3 u1A = projUnit( e1A ), u2A = projUnit( e2A );
			const Vector3 u1B = projUnit( e1B ), u2B = projUnit( e2B );
			if( PlaneSeparatesAtCorner( a, csA, ka, b, csB, kb, X, u2A - u1B ) ||
			    PlaneSeparatesAtCorner( a, csA, ka, b, csB, kb, X, u1A - u2B ) ) {
				return true;
			}
		}
		return false;
	}

	//! Exact signed volume terms of one patch about `c`:
	//! (1/3) * integral over [0,1]^2 of (P - c).(Pu x Pv), from the
	//! Bernstein product integral  int B3_i B5_a = C3_i C5_a / (9 C8_{i+a}).
	inline void AccumulatePatchVolume( const BezierPatch& p, const Point3& c, Scalar& vol, Scalar& absSum )
	{
		static const Scalar C3[4] = { 1, 3, 3, 1 };
		static const Scalar C5[6] = { 1, 5, 10, 10, 5, 1 };
		static const Scalar C8[9] = { 1, 8, 28, 56, 70, 56, 28, 8, 1 };
		Vector3 N[6][6];
		ComputeBezierNormalBernsteinCoeffs( p, N );
		for( int i = 0; i < 4; ++i ) {
			for( int a = 0; a < 6; ++a ) {
				const Scalar Iia = C3[i] * C5[a] / ( Scalar( 9 ) * C8[i + a] );
				for( int j = 0; j < 4; ++j ) {
					const Vector3 pc = Vector3Ops::mkVector3( p.c[i].pts[j], c );
					for( int b = 0; b < 6; ++b ) {
						const Scalar Ijb = C3[j] * C5[b] / ( Scalar( 9 ) * C8[j + b] );
						// Pu x Pv = 9 * sum N_ab B5_a B5_b, and the volume is a third of the integral
						const Scalar t = Scalar( 3 ) * Vector3Ops::Dot( pc, N[a][b] ) * Iia * Ijb;
						vol += t;
						absSum += std::fabs( t );
					}
				}
			}
		}
	}

	class UnionFindNodes
	{
	public:
		explicit UnionFindNodes( std::size_t n ) : parent( n )
		{
			for( std::size_t i = 0; i < n; ++i ) parent[i] = static_cast<unsigned int>( i );
		}
		unsigned int Find( unsigned int x )
		{
			while( parent[x] != x ) {
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		}
		void Unite( unsigned int a, unsigned int b )
		{
			a = Find( a ); b = Find( b );
			if( a != b ) parent[a < b ? b : a] = ( a < b ? a : b );
		}
	private:
		std::vector<unsigned int> parent;
	};

	// Work units per certification attempt (one per candidate pair, per
	// recursive box test, per hashed cell).  A set that needs more is
	// REFUSED, never certified.  Measured worst case so far (a 600-turn
	// coil of 2400 patches whose tilted turns overlap in every box, both
	// grid attempts exhausting the budget): 1.1 s in total.  The costlier
	// per-unit steps (pooled cone, corner separation) are bounded by a
	// small multiple of the sub-patch count, itself capped below.
	static const std::size_t kPatchCertWorkBudget = std::size_t( 20000000 );

	// Cap on sub-patch records per attempt (memory; see CertifyBezierNets).
	static const std::size_t kMaxSubPatches = std::size_t( 1 ) << 18;

	std::atomic<unsigned int> s_patchCertificateEpoch( 1 );

	struct PatchSetVerdict
	{
		bool		watertight = false;
		bool		open = false;
		bool		negative = false;
		const char*	reason = "";
		bool		geometric = false;	//!< refused by an embedding step (a finer grid may succeed)
		unsigned int	grid = 0;		//!< sub-patch grid of the attempt that decided
		std::size_t		workUsed = 0;	//!< work units that attempt spent
	};

	struct SubPatchRec
	{
		BezierPatch		net;
		BoundingBox		box;
		Vector3			V;
		unsigned int	corner[4];			//!< union-find vertex id of each corner
		int				collapsedSide = -1;	//!< 0..3 when one side of this sub-patch is collapsed to a pole
	};

	//! Inspects a patch's four sides and corners for pole-fan topology:
	//! at most one side may have all four control points bitwise equal to a
	//! single pole position (`outSide` in `0..3`, or `-1` when all four
	//! corners are distinct), no non-constant side may loop (`c[0] == c[3]`
	//! with `c[1] != c[0]` or `c[2] != c[0]`), non-adjacent corners must be
	//! distinct, and on a polar patch none of the 12 control points outside
	//! `outSide` may coincide with the pole.
	bool FindCollapsedSide( const BezierPatch& p, int& outSide )
	{
		outSide = -1;
		for( int s = 0; s < 4; ++s ) {
			const PatchCurve c = NetSide( p, s );
			if( PatchPointEqual( c[0], c[3] ) ) {
				if( !PatchPointEqual( c[0], c[1] ) || !PatchPointEqual( c[0], c[2] ) ) {
					return false;
				}
				if( outSide >= 0 ) {
					return false;
				}
				outSide = s;
			}
		}
		for( int k = 0; k < 4; ++k ) {
			for( int m = k + 1; m < 4; ++m ) {
				if( PatchPointEqual( NetCorner( p, k ), NetCorner( p, m ) ) ) {
					const bool isCollapsedPair = ( outSide >= 0 ) &&
						( ( k == outSide && m == ( ( outSide + 1 ) & 3 ) ) ||
						  ( k == 0 && m == 3 && outSide == 3 ) );
					if( !isCollapsedPair ) {
						return false;
					}
				}
			}
		}
		if( outSide >= 0 ) {
			const Point3 pole = NetCorner( p, outSide );
			for( int i = 0; i < 4; ++i ) {
				for( int j = 0; j < 4; ++j ) {
					if( !IsOnNetSide( outSide, i, j ) && PatchPointEqual( p.c[i].pts[j], pole ) ) {
						return false;
					}
				}
			}
		}
		return true;
	}

	//! Verifies that at every corner / pole position shared by one or more
	//! patches, the incident wedges are linked by matched non-collapsed
	//! seams into a single manifold fan (either one closed cycle of glued
	//! radial seams or one open chain with two boundary radial sides).
	//! Rejects tip-to-tip double cones, closed cones with extra flaps, and
	//! multi-fan pinch vertices.
	bool CheckManifoldVertexLinks(
		const std::vector<BezierPatch>& patches,
		const std::vector<int>& collapsedSide,
		const std::vector<int>& match )
	{
		struct Incidence
		{
			Point3			pos;
			unsigned int	inSide;		//!< non-collapsed side entering `pos` around patch boundary
			unsigned int	outSide;	//!< non-collapsed side leaving `pos` around patch boundary
		};
		std::vector<Incidence> inc;
		inc.reserve( patches.size() * 4 );
		for( std::size_t p = 0; p < patches.size(); ++p ) {
			const int cs = collapsedSide[p];
			for( int k = 0; k < 4; ++k ) {
				if( cs >= 0 && k == ( ( cs + 1 ) & 3 ) ) {
					continue;
				}
				Incidence rec;
				rec.pos = NetCorner( patches[p], k );
				rec.inSide = static_cast<unsigned int>( p * 4 + ( ( k + 3 ) & 3 ) );
				rec.outSide = static_cast<unsigned int>( p * 4 + ( ( k == cs ) ? ( ( cs + 1 ) & 3 ) : k ) );
				inc.push_back( rec );
			}
		}
		std::sort( inc.begin(), inc.end(), []( const Incidence& a, const Incidence& b ) {
			if( PatchPointLess( a.pos, b.pos ) ) return true;
			if( PatchPointLess( b.pos, a.pos ) ) return false;
			return a.inSide < b.inSide;
		} );
		for( std::size_t i = 0; i < inc.size(); ) {
			std::size_t j = i + 1;
			while( j < inc.size() && PatchPointEqual( inc[j].pos, inc[i].pos ) ) {
				++j;
			}
			const std::size_t M = j - i;
			std::vector<int> next( M, -1 );
			std::vector<int> inDeg( M, 0 );
			std::size_t edges = 0;
			for( std::size_t a = 0; a < M; ++a ) {
				const int m = match[inc[i + a].outSide];
				if( m < 0 ) {
					continue;
				}
				for( std::size_t b = 0; b < M; ++b ) {
					if( inc[i + b].inSide == static_cast<unsigned int>( m ) ) {
						next[a] = static_cast<int>( b );
						++inDeg[b];
						++edges;
						break;
					}
				}
			}
			if( edges == M ) {
				std::size_t cur = 0, visited = 0;
				do {
					cur = static_cast<std::size_t>( next[cur] );
					++visited;
				} while( cur != 0 && visited <= M );
				if( visited != M ) {
					return false;
				}
			} else if( edges + 1 == M ) {
				int start = -1;
				for( std::size_t a = 0; a < M; ++a ) {
					if( inDeg[a] == 0 ) {
						start = static_cast<int>( a );
						break;
					}
				}
				if( start < 0 ) {
					return false;
				}
				std::size_t visited = 0;
				for( int cur = start; cur >= 0 && visited <= M; cur = next[static_cast<std::size_t>( cur )] ) {
					++visited;
				}
				if( visited != M ) {
					return false;
				}
			} else {
				return false;
			}
			i = j;
		}
		return true;
	}

	PatchSetVerdict CertifyBezierNets( const std::vector<BezierPatch>& patches, unsigned int n, std::size_t workBudget )
	{
		PatchSetVerdict r;
		const std::size_t nPatches = patches.size();
		if( nPatches == 0 ) {
			r.reason = "empty patch set";
			return r;
		}
		r.grid = n;
		// Memory cap, checked BEFORE anything is allocated: a sub-patch
		// record is ~470 bytes, so kMaxSubPatches bounds the dominant
		// allocation near 125 MB (the node arrays add ~30 % at most).  A
		// set past it is refused, never certified.
		if( nPatches > kMaxSubPatches / ( std::size_t( n ) * n ) ) {
			r.reason = "patch set too large to certify at this sub-patch grid";
			return r;
		}

		// Finite control points; global box and scale.
		Point3 lo = patches[0].c[0].pts[0], hi = lo;
		Scalar maxAbs = 0;
		for( const BezierPatch& p : patches ) {
			for( int i = 0; i < 4; ++i ) {
				for( int j = 0; j < 4; ++j ) {
					const Point3& q = p.c[i].pts[j];
					if( !PatchPointFinite( q ) ) {
						r.reason = "non-finite control point";
						return r;
					}
					lo = Point3( std::min( lo.x, q.x ), std::min( lo.y, q.y ), std::min( lo.z, q.z ) );
					hi = Point3( std::max( hi.x, q.x ), std::max( hi.y, q.y ), std::max( hi.z, q.z ) );
					maxAbs = std::max( maxAbs, std::max( std::fabs( q.x ), std::max( std::fabs( q.y ), std::fabs( q.z ) ) ) );
				}
			}
		}
		const Scalar diag = Vector3Ops::Magnitude( Vector3Ops::mkVector3( hi, lo ) );
		if( !( diag > 0 ) || !std::isfinite( diag ) ) {
			r.reason = "degenerate extent";
			return r;
		}
		// One positional tolerance for the whole proof.  It absorbs the
		// rounding of the sub-patch nets against the surface the renderer
		// intersects (subdivision differs from it by a few hundred ulps),
		// and the glued-seam assignment below may move a boundary control
		// point by up to the same amount -- so the disjointness boxes must
		// be inflated by at least that much, or a seam correction could
		// carry a net across a gap the boxes claimed was clear.
		const Scalar seamTol = Scalar( 1e-9 ) * ( diag + maxAbs );
		const Scalar inflate = seamTol;

		// T: sides and exact seam matching.
		struct SideRec
		{
			Point3			a, b;	//!< endpoints in lexicographic order
			unsigned int	id;		//!< patch * 4 + side
		};
		std::vector<int> collapsedSide( nPatches, -1 );
		std::vector<SideRec> sides;
		sides.reserve( nPatches * 4 );
		for( std::size_t p = 0; p < nPatches; ++p ) {
			if( !FindCollapsedSide( patches[p], collapsedSide[p] ) ) {
				r.reason = "patch with coincident corners (collapsed side or loop)";
				return r;
			}
			for( int s = 0; s < 4; ++s ) {
				if( s == collapsedSide[p] ) {
					continue;
				}
				const PatchCurve c = NetSide( patches[p], s );
				SideRec rec;
				const bool fwd = PatchPointLess( c[0], c[3] );
				rec.a = fwd ? c[0] : c[3];
				rec.b = fwd ? c[3] : c[0];
				rec.id = static_cast<unsigned int>( p * 4 + s );
				sides.push_back( rec );
			}
		}
		std::sort( sides.begin(), sides.end(), []( const SideRec& x, const SideRec& y ) {
			if( PatchPointLess( x.a, y.a ) ) return true;
			if( PatchPointLess( y.a, x.a ) ) return false;
			if( PatchPointLess( x.b, y.b ) ) return true;
			if( PatchPointLess( y.b, x.b ) ) return false;
			return x.id < y.id;
		} );
		std::vector<int> match( nPatches * 4, -1 );
		std::size_t freeSides = 0;
		for( std::size_t i = 0; i < sides.size(); ) {
			std::size_t j = i + 1;
			while( j < sides.size() && PatchPointEqual( sides[j].a, sides[i].a ) && PatchPointEqual( sides[j].b, sides[i].b ) ) {
				++j;
			}
			if( j - i > 2 ) {
				r.reason = "non-manifold seam (more than two sides share endpoints)";
				return r;
			}
			if( j - i == 2 ) {
				const unsigned int s0 = sides[i].id, s1 = sides[i + 1].id;
				if( s0 / 4 == s1 / 4 ) {
					r.reason = "self-glued patch";
					return r;
				}
				const PatchCurve c0 = NetSide( patches[s0 / 4], s0 % 4 );
				const PatchCurve c1 = NetSide( patches[s1 / 4], s1 % 4 );
				if( !PatchCurveEqual( c0, ReversePatchCurve( c1 ) ) ) {
					r.reason = "sides share endpoints but are not one reversed seam (crack, curve mismatch or inconsistent winding)";
					return r;
				}
				match[s0] = static_cast<int>( s1 );
				match[s1] = static_cast<int>( s0 );
			} else {
				++freeSides;
			}
			i = j;
		}
		if( !CheckManifoldVertexLinks( patches, collapsedSide, match ) ) {
			r.reason = "non-manifold pole or pinch vertex";
			return r;
		}

		// A single seam-connected component.
		{
			std::vector<unsigned char> seen( nPatches, 0 );
			std::vector<unsigned int> stack( 1, 0 );
			seen[0] = 1;
			std::size_t reached = 1;
			while( !stack.empty() ) {
				const unsigned int p = stack.back();
				stack.pop_back();
				for( int s = 0; s < 4; ++s ) {
					const int m = match[p * 4 + s];
					if( m >= 0 && !seen[m / 4] ) {
						seen[m / 4] = 1;
						++reached;
						stack.push_back( static_cast<unsigned int>( m / 4 ) );
					}
				}
			}
			if( reached != nPatches ) {
				r.reason = "patch set is not one seam-connected piece";
				return r;
			}
		}

		// Vertex identities of the sub-patch grid, from collapsed poles and glued seams.
		const unsigned int n1 = n + 1;
		const std::size_t nodesPerPatch = std::size_t( n1 ) * n1;
		auto nodeAt = [&]( std::size_t p, unsigned int iu, unsigned int iv ) {
			return static_cast<unsigned int>( p * nodesPerPatch + iv * n1 + iu );
		};
		auto sideNode = [&]( std::size_t p, int s, unsigned int k ) {
			switch( s ) {
			default:
			case 0: return nodeAt( p, k, 0 );
			case 1: return nodeAt( p, n, k );
			case 2: return nodeAt( p, n - k, n );
			case 3: return nodeAt( p, 0, n - k );
			}
		};
		UnionFindNodes uf( nPatches * nodesPerPatch );
		for( std::size_t p = 0; p < nPatches; ++p ) {
			const int cs = collapsedSide[p];
			if( cs >= 0 ) {
				const unsigned int poleNode = sideNode( p, cs, 0 );
				for( unsigned int k = 1; k <= n; ++k ) {
					uf.Unite( poleNode, sideNode( p, cs, k ) );
				}
			}
		}
		for( std::size_t id = 0; id < match.size(); ++id ) {
			const int m = match[id];
			if( m > static_cast<int>( id ) ) {
				for( unsigned int k = 0; k <= n; ++k ) {
					uf.Unite( sideNode( id / 4, static_cast<int>( id % 4 ), k ),
					          sideNode( static_cast<std::size_t>( m ) / 4, m % 4, n - k ) );
				}
			}
		}

		// G1: sub-patch nets.  Glued boundaries are then made BITWISE
		// equal by assignment, not by hoping two rounding sequences agree
		// (the build's -ffast-math may contract the same expression
		// differently at different inline sites): every vertex takes its
		// first-seen position and every glued sub-patch side its owner's
		// control points.  Before assignment each copy must agree with the
		// owner to `seamTol` -- a sanity check only, since the glue itself
		// was proved exactly on the input control points above.
		std::vector<SubPatchRec> subs( nPatches * n * n );
		auto subIndex = [&]( std::size_t p, unsigned int iu, unsigned int iv ) {
			return ( p * n + iv ) * n + iu;
		};
		for( std::size_t p = 0; p < nPatches; ++p ) {
			const int cs = collapsedSide[p];
			for( unsigned int iv = 0; iv < n; ++iv ) {
				for( unsigned int iu = 0; iu < n; ++iu ) {
					SubPatchRec& rec = subs[subIndex( p, iu, iv )];
					rec.net = SubdivideBezierPatchRange( patches[p],
						Scalar( iu ) / Scalar( n ), Scalar( iu + 1 ) / Scalar( n ),
						Scalar( iv ) / Scalar( n ), Scalar( iv + 1 ) / Scalar( n ) );
					rec.collapsedSide =
						( cs == 0 && iv == 0 ) ? 0 :
						( cs == 1 && iu + 1 == n ) ? 1 :
						( cs == 2 && iv + 1 == n ) ? 2 :
						( cs == 3 && iu == 0 ) ? 3 : -1;
					const unsigned int cn[4] = {
						nodeAt( p, iu, iv ), nodeAt( p, iu + 1, iv ),
						nodeAt( p, iu + 1, iv + 1 ), nodeAt( p, iu, iv + 1 ) };
					for( int k = 0; k < 4; ++k ) {
						rec.corner[k] = uf.Find( cn[k] );
					}
					for( int k = 0; k < 4; ++k ) {
						for( int m = k + 1; m < 4; ++m ) {
							const bool isPolePair = ( rec.collapsedSide >= 0 ) &&
								( ( k == rec.collapsedSide && m == ( ( rec.collapsedSide + 1 ) & 3 ) ) ||
								  ( k == 0 && m == 3 && rec.collapsedSide == 3 ) );
							if( !isPolePair && rec.corner[k] == rec.corner[m] ) {
								r.reason = "sub-patch with identified corners";
								return r;
							}
						}
					}
				}
			}
		}
		{
			std::vector<Point3> nodePos( nPatches * nodesPerPatch );
			std::vector<unsigned char> nodePosSet( nPatches * nodesPerPatch, 0 );
			for( SubPatchRec& rec : subs ) {
				for( int k = 0; k < 4; ++k ) {
					int ci, cj;
					NetCornerIndex( k, ci, cj );
					Point3& pos = rec.net.c[ci].pts[cj];
					const unsigned int root = rec.corner[k];
					if( !nodePosSet[root] ) {
						nodePosSet[root] = 1;
						nodePos[root] = pos;
					} else {
						if( !( Vector3Ops::Magnitude( Vector3Ops::mkVector3( pos, nodePos[root] ) ) <= seamTol ) ) {
							r.reason = "glued seam nodes disagree in position";
							return r;
						}
						pos = nodePos[root];
					}
				}
				if( rec.collapsedSide >= 0 ) {
					const Point3& polePos = nodePos[rec.corner[rec.collapsedSide]];
					for( int m = 0; m < 4; ++m ) {
						int si, sj;
						NetSideIndex( rec.collapsedSide, m, si, sj );
						if( !( Vector3Ops::Magnitude( Vector3Ops::mkVector3( rec.net.c[si].pts[sj], polePos ) ) <= seamTol ) ) {
							r.reason = "glued seam nodes disagree in position";
							return r;
						}
						rec.net.c[si].pts[sj] = polePos;
					}
				}
			}
		}
		// Glued sub-patch side pairs: (owner, side) -> (partner, side),
		// the partner's side being the owner's reversed.
		auto glueSide = [&]( std::size_t owner, int so, std::size_t partner, int sp ) {
			const PatchCurve co = NetSide( subs[owner].net, so );
			const PatchCurve cp = NetSide( subs[partner].net, sp );
			for( int m = 0; m < 4; ++m ) {
				if( !( Vector3Ops::Magnitude( Vector3Ops::mkVector3( cp[m], co[3 - m] ) ) <= seamTol ) ) {
					return false;
				}
			}
			SetNetSide( subs[partner].net, sp, ReversePatchCurve( co ) );
			return true;
		};
		for( std::size_t p = 0; p < nPatches; ++p ) {
			for( unsigned int iv = 0; iv < n; ++iv ) {
				for( unsigned int iu = 0; iu < n; ++iu ) {
					if( iu + 1 < n && !glueSide( subIndex( p, iu, iv ), 1, subIndex( p, iu + 1, iv ), 3 ) ) {
						r.reason = "glued seam sides disagree in position";
						return r;
					}
					if( iv + 1 < n && !glueSide( subIndex( p, iu, iv ), 2, subIndex( p, iu, iv + 1 ), 0 ) ) {
						r.reason = "glued seam sides disagree in position";
						return r;
					}
				}
			}
		}
		auto sideSub = [&]( std::size_t p, int s, unsigned int k ) {
			switch( s ) {
			default:
			case 0: return subIndex( p, k, 0 );
			case 1: return subIndex( p, n - 1, k );
			case 2: return subIndex( p, n - 1 - k, n - 1 );
			case 3: return subIndex( p, 0, n - 1 - k );
			}
		};
		for( std::size_t id = 0; id < match.size(); ++id ) {
			const int m = match[id];
			if( m > static_cast<int>( id ) ) {
				for( unsigned int k = 0; k < n; ++k ) {
					if( !glueSide( sideSub( id / 4, static_cast<int>( id % 4 ), k ), static_cast<int>( id % 4 ),
					               sideSub( static_cast<std::size_t>( m ) / 4, m % 4, n - 1 - k ), m % 4 ) ) {
						r.reason = "glued seam sides disagree in position";
						return r;
					}
				}
			}
		}
		for( SubPatchRec& rec : subs ) {
			rec.box = InflatedNetBox( rec.net, inflate );
			if( !CrossConeProof( &rec.net, &rec.collapsedSide, 1, rec.V ) ) {
				r.reason = "sub-patch not provably injective (degenerate, folded or too curved)";
				r.geometric = true;
				return r;
			}
		}

		// G2: pairwise embedding.  Candidate pairs come from a uniform
		// grid hash (cell = the median sub-patch box extent): every pair
		// whose boxes overlap shares at least one cell, and is tested once,
		// in the cell holding the low corner of the two boxes' overlap.  A
		// box spanning more than kMaxCellsPerBox cells is tested against
		// every sub-patch instead.  Every candidate costs one work unit, so
		// a dense set exhausts the budget and is refused.
		PatchCertWork work;
		work.remaining = workBudget;
		const std::size_t nSubs = subs.size();
		std::vector<Scalar> extents( nSubs );
		for( std::size_t i = 0; i < nSubs; ++i ) {
			const BoundingBox& bb = subs[i].box;
			extents[i] = std::max( bb.ur.x - bb.ll.x, std::max( bb.ur.y - bb.ll.y, bb.ur.z - bb.ll.z ) );
		}
		std::nth_element( extents.begin(), extents.begin() + nSubs / 2, extents.end() );
		Scalar cell = extents[nSubs / 2];
		const Scalar boxSpan = diag + Scalar( 4 ) * inflate;
		if( !( cell > boxSpan * Scalar( 1.0 / 1048576.0 ) ) ) {
			cell = boxSpan * Scalar( 1.0 / 1048576.0 );	// keeps every index below 2^21
		}
		const Point3 gridLo( lo.x - Scalar( 2 ) * inflate, lo.y - Scalar( 2 ) * inflate, lo.z - Scalar( 2 ) * inflate );
		struct CellRange { std::uint32_t lo[3], hi[3]; };
		std::vector<CellRange> ranges( nSubs );
		auto cellIndex = [&]( Scalar v, Scalar origin ) {
			const Scalar t = std::floor( ( v - origin ) / cell );
			return static_cast<std::uint32_t>( std::min( Scalar( 2097151 ), std::max( Scalar( 0 ), t ) ) );
		};
		auto cellKey = []( std::uint32_t x, std::uint32_t y, std::uint32_t z ) {
			return ( std::uint64_t( x ) << 42 ) | ( std::uint64_t( y ) << 21 ) | std::uint64_t( z );
		};
		static const std::uint64_t kMaxCellsPerBox = 64;
		std::unordered_map<std::uint64_t, std::vector<unsigned int> > cells;
		std::vector<unsigned int> oversize;
		for( std::size_t i = 0; i < nSubs; ++i ) {
			const BoundingBox& bb = subs[i].box;
			CellRange& cr = ranges[i];
			cr.lo[0] = cellIndex( bb.ll.x, gridLo.x ); cr.hi[0] = cellIndex( bb.ur.x, gridLo.x );
			cr.lo[1] = cellIndex( bb.ll.y, gridLo.y ); cr.hi[1] = cellIndex( bb.ur.y, gridLo.y );
			cr.lo[2] = cellIndex( bb.ll.z, gridLo.z ); cr.hi[2] = cellIndex( bb.ur.z, gridLo.z );
			const std::uint64_t count = std::uint64_t( cr.hi[0] - cr.lo[0] + 1 ) *
				( cr.hi[1] - cr.lo[1] + 1 ) * ( cr.hi[2] - cr.lo[2] + 1 );
			if( count > kMaxCellsPerBox ) {
				oversize.push_back( static_cast<unsigned int>( i ) );
				continue;
			}
			if( !work.Spend( count ) ) {
				r.reason = "certification work budget exhausted";
				r.workUsed = workBudget;
				return r;
			}
			for( std::uint32_t x = cr.lo[0]; x <= cr.hi[0]; ++x ) {
				for( std::uint32_t y = cr.lo[1]; y <= cr.hi[1]; ++y ) {
					for( std::uint32_t z = cr.lo[2]; z <= cr.hi[2]; ++z ) {
						cells[cellKey( x, y, z )].push_back( static_cast<unsigned int>( i ) );
					}
				}
			}
		}
		std::vector<unsigned char> isOversize( nSubs, 0 );
		for( const unsigned int i : oversize ) isOversize[i] = 1;

		// The per-pair proof; FALSE (with r filled) refuses the set.
		auto testPair = [&]( const SubPatchRec& A, const SubPatchRec& B ) -> bool {
			if( !PatchBoxesOverlap( A.box, B.box ) ) {
				return true;
			}
			unsigned int sharedVerts[4];
			int shared = 0;
			for( int a = 0; a < 4; ++a ) {
				for( int b = 0; b < 4; ++b ) {
					if( A.corner[a] == B.corner[b] ) {
						bool dup = false;
						for( int k = 0; k < shared; ++k ) {
							if( sharedVerts[k] == A.corner[a] ) {
								dup = true;
								break;
							}
						}
						if( !dup ) {
							sharedVerts[shared++] = A.corner[a];
						}
					}
				}
			}
			if( shared == 0 ) {
				if( !NetsProvablyDisjoint( A.net, 0, B.net, 0, inflate, work ) ) {
					r.reason = work.exhausted ? "certification work budget exhausted" :
						"sub-patches without a shared vertex are not provably disjoint (touching, crossing or T-junction)";
					r.geometric = !work.exhausted;
					return false;
				}
			} else if( shared == 1 ) {
				const unsigned int v = sharedVerts[0];
				const int csA_at_v = ( A.collapsedSide >= 0 && A.corner[A.collapsedSide] == v ) ? A.collapsedSide : -1;
				const int csB_at_v = ( B.collapsedSide >= 0 && B.corner[B.collapsedSide] == v ) ? B.collapsedSide : -1;
				int ka = ( csA_at_v >= 0 ) ? csA_at_v : -1;
				int kb = ( csB_at_v >= 0 ) ? csB_at_v : -1;
				for( int a = 0; a < 4 && ka < 0; ++a ) {
					if( A.corner[a] == v ) ka = a;
				}
				for( int b = 0; b < 4 && kb < 0; ++b ) {
					if( B.corner[b] == v ) kb = b;
				}
				if( !CornerPairSeparated( A.net, csA_at_v, ka, B.net, csB_at_v, kb, A.V + B.V ) ) {
					r.reason = "sub-patches sharing one vertex are not provably separated there";
					r.geometric = true;
					return false;
				}
			} else if( shared == 2 ) {
				// The two shared vertices must be the ends of one non-collapsed
				// side of each, traversed in opposite directions.
				int sa = -1, sb = -1;
				for( int s = 0; s < 4 && sa < 0; ++s ) {
					if( s == A.collapsedSide ) continue;
					for( int t = 0; t < 4; ++t ) {
						if( t == B.collapsedSide ) continue;
						if( A.corner[s] == B.corner[( t + 1 ) & 3] && A.corner[( s + 1 ) & 3] == B.corner[t] ) {
							sa = s; sb = t;
							break;
						}
					}
				}
				if( sa < 0 ) {
					r.reason = "sub-patches share two vertices that are not one oppositely-traversed side";
					return false;
				}
				const int rotA = ( sa + 2 ) & 3;
				const int rotB = sb & 3;
				BezierPatch pair[2] = { RotateNet( A.net, rotA ), RotateNet( B.net, rotB ) };
				const int pairCS[2] = {
					( A.collapsedSide >= 0 ) ? ( ( A.collapsedSide - rotA ) & 3 ) : -1,
					( B.collapsedSide >= 0 ) ? ( ( B.collapsedSide - rotB ) & 3 ) : -1
				};
				for( int i = 0; i < 4; ++i ) {
					if( !PatchPointEqual( pair[0].c[i].pts[3], pair[1].c[i].pts[0] ) ) {
						r.reason = "glued sub-patch sides disagree in position";
						return false;
					}
				}
				Vector3 V;
				if( !CrossConeProof( pair, pairCS, 2, V ) ) {
					r.reason = "sub-patches sharing a side are not provably injective as a union (fold or sharp crease)";
					r.geometric = true;
					return false;
				}
			} else {
				r.reason = "sub-patches share more than one side";
				return false;
			}
			return true;
		};

		// Visit cells in key order so the first refusal (and its reason)
		// does not depend on the hash table's iteration order.
		std::vector<std::uint64_t> keys;
		keys.reserve( cells.size() );
		for( const auto& entry : cells ) keys.push_back( entry.first );
		std::sort( keys.begin(), keys.end() );
		for( const std::uint64_t key : keys ) {
			const std::vector<unsigned int>& list = cells[key];
			for( std::size_t ii = 0; ii < list.size(); ++ii ) {
				for( std::size_t jj = ii + 1; jj < list.size(); ++jj ) {
					const CellRange& ra = ranges[list[ii]];
					const CellRange& rb = ranges[list[jj]];
					// Test each pair once: in the cell at the low corner of
					// the overlap of the two cell ranges.
					if( cellKey( std::max( ra.lo[0], rb.lo[0] ), std::max( ra.lo[1], rb.lo[1] ),
					             std::max( ra.lo[2], rb.lo[2] ) ) != key ) {
						continue;
					}
					if( !work.Spend( 1 ) ) {
						r.reason = "certification work budget exhausted";
						r.workUsed = workBudget;
						return r;
					}
					if( !testPair( subs[list[ii]], subs[list[jj]] ) ) {
						r.workUsed = workBudget - work.remaining;
						return r;
					}
				}
			}
		}
		for( const unsigned int i : oversize ) {
			for( std::size_t j = 0; j < nSubs; ++j ) {
				if( j == i || ( isOversize[j] && j < i ) ) {
					continue;
				}
				if( !work.Spend( 1 ) ) {
					r.reason = "certification work budget exhausted";
					r.workUsed = workBudget;
					return r;
				}
				if( !testPair( subs[i], subs[j] ) ) {
					r.workUsed = workBudget - work.remaining;
					return r;
				}
			}
		}
		r.workUsed = workBudget - work.remaining;

		// C: classification by the exact signed volume.
		const Point3 centre( Scalar( 0.5 ) * ( lo.x + hi.x ), Scalar( 0.5 ) * ( lo.y + hi.y ), Scalar( 0.5 ) * ( lo.z + hi.z ) );
		Scalar vol = 0, absSum = 0;
		for( const BezierPatch& p : patches ) {
			AccumulatePatchVolume( p, centre, vol, absSum );
		}
		if( freeSides == 0 ) {
			if( !( vol > Scalar( 1e-9 ) * absSum ) ) {
				r.reason = "closed surface is inward-wound or encloses no resolvable volume";
				return r;
			}
			r.watertight = true;
			return r;
		}
		r.open = true;
		r.negative = vol < 0;
		return r;
	}

	PatchSetVerdict CertifyBezierNetsAdaptive( const std::vector<BezierPatch>& patches, std::size_t workBudget )
	{
		if( workBudget == 0 ) {
			workBudget = kPatchCertWorkBudget;
		}
		PatchSetVerdict r = CertifyBezierNets( patches, 4, workBudget );
		if( !r.watertight && !r.open && r.geometric ) {
			r = CertifyBezierNets( patches, 8, workBudget );
		}
		return r;
	}

	//! The cubic control polygon of the segment E0 -> E1, computed from
	//! the lexicographically smaller end so the glued partner (E1 -> E0)
	//! gets the bitwise reverse; the ends are exact.
	inline PatchCurve ElevatedSegment( const Point3& E0, const Point3& E1 )
	{
		if( PatchPointEqual( E0, E1 ) ) {
			const PatchCurve c = { { E0, E0, E0, E0 } };
			return c;
		}
		const bool fwd = !PatchPointLess( E1, E0 );
		const Point3& A = fwd ? E0 : E1;
		const Point3& B = fwd ? E1 : E0;
		const PatchCurve c = { {
			A,
			Point3Ops::WeightedAverage2( B, A, Scalar( 1 ) / Scalar( 3 ) ),
			Point3Ops::WeightedAverage2( B, A, Scalar( 2 ) / Scalar( 3 ) ),
			B } };
		return fwd ? c : ReversePatchCurve( c );
	}

	//! Exact degree elevation of a bilinear patch (pts[0] = (0,0),
	//! pts[1] = (0,1), pts[2] = (1,0), pts[3] = (1,1)) to a bicubic net
	//! with the same (u, v) and hence the same normal Pu x Pv.
	BezierPatch ElevateBilinear( const BilinearPatch& b )
	{
		BezierPatch q;
		const PatchCurve v0 = ElevatedSegment( b.pts[0], b.pts[2] );	// side v = 0, u increasing
		const PatchCurve u1 = ElevatedSegment( b.pts[2], b.pts[3] );	// side u = 1, v increasing
		const PatchCurve v1 = ElevatedSegment( b.pts[1], b.pts[3] );	// v = 1, u increasing
		const PatchCurve u0 = ElevatedSegment( b.pts[0], b.pts[1] );	// u = 0, v increasing
		for( int j = 0; j < 4; ++j ) {
			q.c[0].pts[j] = u0[j];
			q.c[3].pts[j] = u1[j];
		}
		for( int i = 0; i < 4; ++i ) {
			q.c[i].pts[0] = v0[i];
			q.c[i].pts[3] = v1[i];
		}
		for( int i = 1; i < 3; ++i ) {
			for( int j = 1; j < 3; ++j ) {
				q.c[i].pts[j] = Point3Ops::WeightedAverage2( q.c[3].pts[j], q.c[0].pts[j], Scalar( i ) / Scalar( 3 ) );
			}
		}
		return q;
	}

	void PublishVerdict(
		const PatchSetVerdict& v,
		bool& outCertifiedWatertight,
		bool& outProvablyOpenSheet,
		bool& outNegativeOpenSignedVolume,
		GeometricUtilities::PatchSetCertDiagnostics* diag )
	{
		outCertifiedWatertight = v.watertight;
		outProvablyOpenSheet = v.open;
		outNegativeOpenSignedVolume = v.open && v.negative;
		if( diag ) {
			diag->reason = v.reason;
			diag->grid = v.grid;
			diag->workUsed = v.workUsed;
		}
	}
}

namespace
{
	//! Bilinear patches as their exact degree elevations, with every side
	//! sharing an endpoint pair given ONE canonical polygon (two bilinear
	//! sides with the same endpoints ARE the same segment, but their
	//! elevated interior points come from different call sites).  FALSE on
	//! a non-finite corner.
	bool BilinearNets( const std::vector<BilinearPatch>& patches, std::vector<BezierPatch>& nets )
	{
		nets.clear();
		nets.reserve( patches.size() );
		for( const BilinearPatch& b : patches ) {
			for( int k = 0; k < 4; ++k ) {
				if( !PatchPointFinite( b.pts[k] ) ) {
					return false;
				}
			}
			nets.push_back( ElevateBilinear( b ) );
		}
		struct Key
		{
			Point3 a, b;
			bool operator<( const Key& o ) const
			{
				if( PatchPointLess( a, o.a ) ) return true;
				if( PatchPointLess( o.a, a ) ) return false;
				return PatchPointLess( b, o.b );
			}
		};
		std::map<Key, PatchCurve> canonical;
		for( BezierPatch& q : nets ) {
			for( int s = 0; s < 4; ++s ) {
				const PatchCurve c = NetSide( q, s );
				const bool fwd = !PatchPointLess( c[3], c[0] );
				const Key key = { fwd ? c[0] : c[3], fwd ? c[3] : c[0] };
				const PatchCurve oriented = fwd ? c : ReversePatchCurve( c );
				const auto it = canonical.insert( std::make_pair( key, oriented ) ).first;
				SetNetSide( q, s, fwd ? it->second : ReversePatchCurve( it->second ) );
			}
		}
		return true;
	}

	//! Topological openness of a patch set from EXACT seam identity alone
	//! (no geometry, no embedding proof): +1 at least one non-collapsed
	//! side has no glued partner; 0 every non-collapsed side is glued to
	//! exactly one reversed partner and every vertex/pole link is a single
	//! manifold cycle (no free boundary); -1 undecidable (empty,
	//! non-finite, a non-constant loop side, more than one collapsed side
	//! on a patch, a side shared by more than two patches, two sides
	//! sharing endpoints without being one reversed seam, or a non-manifold
	//! pole/pinch vertex).
	int NetSetOpenness( const std::vector<BezierPatch>& patches )
	{
		if( patches.empty() ) {
			return -1;
		}
		for( const BezierPatch& p : patches ) {
			for( int i = 0; i < 4; ++i ) {
				for( int j = 0; j < 4; ++j ) {
					if( !PatchPointFinite( p.c[i].pts[j] ) ) {
						return -1;
					}
				}
			}
		}
		struct SideKey
		{
			Point3 a, b;
			unsigned int id;
		};
		std::vector<int> collapsedSide( patches.size(), -1 );
		std::vector<SideKey> sides;
		sides.reserve( patches.size() * 4 );
		for( std::size_t p = 0; p < patches.size(); ++p ) {
			if( !FindCollapsedSide( patches[p], collapsedSide[p] ) ) {
				return -1;
			}
			for( int s = 0; s < 4; ++s ) {
				if( s == collapsedSide[p] ) {
					continue;
				}
				const PatchCurve c = NetSide( patches[p], s );
				const bool fwd = PatchPointLess( c[0], c[3] );
				sides.push_back( { fwd ? c[0] : c[3], fwd ? c[3] : c[0], static_cast<unsigned int>( p * 4 + s ) } );
			}
		}
		std::sort( sides.begin(), sides.end(), []( const SideKey& x, const SideKey& y ) {
			if( PatchPointLess( x.a, y.a ) ) return true;
			if( PatchPointLess( y.a, x.a ) ) return false;
			if( PatchPointLess( x.b, y.b ) ) return true;
			if( PatchPointLess( y.b, x.b ) ) return false;
			return x.id < y.id;
		} );
		std::vector<int> match( patches.size() * 4, -1 );
		bool anyFree = false;
		for( std::size_t i = 0; i < sides.size(); ) {
			std::size_t j = i + 1;
			while( j < sides.size() && PatchPointEqual( sides[j].a, sides[i].a ) && PatchPointEqual( sides[j].b, sides[i].b ) ) {
				++j;
			}
			if( j - i > 2 ) {
				return -1;
			}
			if( j - i == 2 ) {
				const unsigned int s0 = sides[i].id, s1 = sides[i + 1].id;
				if( s0 / 4 == s1 / 4 ) {
					return -1;
				}
				const PatchCurve c0 = NetSide( patches[s0 / 4], s0 % 4 );
				const PatchCurve c1 = NetSide( patches[s1 / 4], s1 % 4 );
				if( !PatchCurveEqual( c0, ReversePatchCurve( c1 ) ) ) {
					return -1;
				}
				match[s0] = static_cast<int>( s1 );
				match[s1] = static_cast<int>( s0 );
			} else {
				anyFree = true;
			}
			i = j;
		}
		if( !CheckManifoldVertexLinks( patches, collapsedSide, match ) ) {
			return -1;
		}
		return anyFree ? 1 : 0;
	}
}

void GeometricUtilities::CertifyBilinearPatchSet(
	const std::vector<BilinearPatch>& patches,
	bool& outCertifiedWatertight,
	bool& outProvablyOpenSheet,
	bool& outNegativeOpenSignedVolume,
	GeometricUtilities::PatchSetCertDiagnostics* diag )
{
	std::vector<BezierPatch> nets;
	if( !BilinearNets( patches, nets ) ) {
		PatchSetVerdict v;
		v.reason = "non-finite control point";
		PublishVerdict( v, outCertifiedWatertight, outProvablyOpenSheet, outNegativeOpenSignedVolume, diag );
		return;
	}
	PublishVerdict( CertifyBezierNetsAdaptive( nets, diag ? diag->workBudget : 0 ),
		outCertifiedWatertight, outProvablyOpenSheet, outNegativeOpenSignedVolume, diag );
}

void GeometricUtilities::CertifyBezierPatchSet(
	const std::vector<BezierPatch>& patches,
	bool& outCertifiedWatertight,
	bool& outProvablyOpenSheet,
	bool& outNegativeOpenSignedVolume,
	GeometricUtilities::PatchSetCertDiagnostics* diag )
{
	PublishVerdict( CertifyBezierNetsAdaptive( patches, diag ? diag->workBudget : 0 ),
		outCertifiedWatertight, outProvablyOpenSheet, outNegativeOpenSignedVolume, diag );
}

int GeometricUtilities::BezierPatchSetOpenness( const std::vector<BezierPatch>& patches )
{
	return NetSetOpenness( patches );
}

int GeometricUtilities::BilinearPatchSetOpenness( const std::vector<BilinearPatch>& patches )
{
	std::vector<BezierPatch> nets;
	if( !BilinearNets( patches, nets ) ) {
		return -1;
	}
	return NetSetOpenness( nets );
}

int GeometricUtilities::TriangleMeshOpenness( const std::vector<Point3>& corners )
{
	const std::size_t nTris = corners.size() / 3;
	if( nTris == 0 || corners.size() % 3 != 0 || nTris > 0x3FFFFFFFu ) {
		return -1;
	}
	for( const Point3& p : corners ) {
		if( !std::isfinite( p.x ) || !std::isfinite( p.y ) || !std::isfinite( p.z ) ) {
			return -1;
		}
	}
	std::vector<unsigned int> wid;
	if( !WeldVertexPositions( corners, wid ) ) {
		return -1;
	}

	for( std::size_t t = 0; t < nTris; ++t ) {
		const unsigned int* I = &wid[3 * t];
		if( I[0] == I[1] || I[1] == I[2] || I[0] == I[2] ) {
			return -1;
		}
		const Point3* P = &corners[3 * t];
		const Scalar twiceArea = Vector3Ops::Magnitude( Vector3Ops::Cross(
			Vector3Ops::mkVector3( P[1], P[0] ), Vector3Ops::mkVector3( P[2], P[0] ) ) );
		Scalar longest = 0;
		for( int k = 0; k < 3; ++k ) {
			longest = std::max( longest, Vector3Ops::Magnitude( Vector3Ops::mkVector3( P[(k + 1) % 3], P[k] ) ) );
		}
		if( !( twiceArea > NEARZERO * longest ) ) {
			return -1;
		}
	}

	struct EdgeUse
	{
		std::uint64_t key = ~std::uint64_t( 0 );
		unsigned int tri[2] = { 0, 0 };
		unsigned int opp[2] = { 0, 0 };
		bool forward[2] = { false, false };
		unsigned int count = 0;
	};
	std::size_t edgeCap = 16;
	while( edgeCap < nTris * 4 ) {
		edgeCap <<= 1;
	}
	const std::size_t edgeMask = edgeCap - 1;
	std::vector<EdgeUse> edgeTable( edgeCap );
	std::vector<unsigned int> triEdgeSlot( 3 * nTris, 0u );
	const auto triEdge = [&]( const std::size_t i ) -> EdgeUse* { return &edgeTable[triEdgeSlot[i]]; };
	for( std::size_t t = 0; t < nTris; ++t ) {
		for( int k = 0; k < 3; ++k ) {
			const unsigned int a = wid[3 * t + k];
			const unsigned int b = wid[3 * t + ( k + 1 ) % 3];
			const unsigned int c = wid[3 * t + ( k + 2 ) % 3];
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
				return -1;
			}
			e.tri[e.count] = static_cast<unsigned int>( t );
			e.opp[e.count] = c;
			e.forward[e.count] = ( a < b );
			++e.count;
			triEdgeSlot[3 * t + k] = static_cast<unsigned int>( slot );
		}
	}

	bool anyFree = false;
	for( std::size_t i = 0; i < 3 * nTris; ++i ) {
		const EdgeUse* e = triEdge( i );
		if( e->count == 1 ) {
			anyFree = true;
		} else if( e->count == 2 ) {
			if( e->forward[0] == e->forward[1] || e->opp[0] == e->opp[1] ) {
				return -1;
			}
		}
	}
	if( !anyFree ) {
		return 0;
	}

	std::vector<unsigned char> alive( nTris, 1 );
	std::vector<unsigned int> queue;
	queue.reserve( nTris );
	for( std::size_t t = 0; t < nTris; ++t ) {
		for( int k = 0; k < 3; ++k ) {
			if( triEdge( 3 * t + k )->count == 1 ) {
				queue.push_back( static_cast<unsigned int>( t ) );
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
			EdgeUse* e = triEdge( 3 * t + k );
			--e->count;
			if( e->count == 1 ) {
				const unsigned int other = alive[e->tri[0]] ? e->tri[0] : e->tri[1];
				if( alive[other] ) {
					queue.push_back( other );
				}
			}
		}
	}
	return ( removed == nTris ) ? 1 : -1;
}

unsigned int GeometricUtilities::PatchCertificateEpoch()
{
	return s_patchCertificateEpoch.load( std::memory_order_acquire );
}

void GeometricUtilities::BumpPatchCertificateEpoch()
{
	s_patchCertificateEpoch.fetch_add( 1, std::memory_order_acq_rel );
}
