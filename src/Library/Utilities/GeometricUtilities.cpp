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
bool GeometricUtilities::WeldVertexPositions( const std::vector<Point3>& points, std::vector<unsigned int>& outWeldedId, Scalar* outEps )
{
	outWeldedId.assign( points.size(), 0u );
	if( points.empty() ) {
		if( outEps ) { *outEps = Scalar( 1e-9 ); }
		return true;
	}

	BoundingBox bbox( points[0], points[0] );
	for( const auto& p : points ) {
		if( !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ) return false;
	}
	for( std::size_t i = 1; i < points.size(); ++i ) {
		bbox.Include( points[i] );
	}
	const Vector3 extents = bbox.GetExtents();
	const Scalar diag = Vector3Ops::Magnitude( extents );
	const Scalar eps = std::max( Scalar( 1e-9 ), Scalar( 1e-6 ) * diag );
	const Scalar cell = eps;
	if( !std::isfinite(cell) || !(cell > 0) ) return false;
	if( outEps ) { *outEps = eps; }

	// Keep the existing grid for representable absolute coordinates. When
	// translation exceeds its integer range, use a mesh-local origin.
	// Relative coordinates are bounded by diag/eps <= 1e6. Strict bounds
	// below also leave room for the signed +/-1 neighbor additions.
	const auto safeCoordinate = [cell](Scalar v) {
		const double c=std::floor(double(v/cell));
		return std::isfinite(c) && c > double(std::numeric_limits<std::int64_t>::min())
			&& c < double(std::numeric_limits<std::int64_t>::max());
	};
	bool localGrid=false;
	for( const auto& p : points ) {
		if( !safeCoordinate(p.x) || !safeCoordinate(p.y) || !safeCoordinate(p.z) ) {localGrid=true;break;}
	}
	const Point3 origin=localGrid?bbox.ll:Point3(0,0,0);
	auto cellCoord = [cell]( const Scalar v ) -> std::int64_t {
		return static_cast<std::int64_t>(std::floor(double(v/cell)));
	};
	// A simple, well-distributed combine for the 3D cell key -- collisions are
	// fine (unordered_map handles them; a false-positive bucket collision only
	// costs an extra distance check, never a wrong weld, since every candidate
	// found is still distance-checked against `eps` below).
	auto cellKey = []( std::int64_t x, std::int64_t y, std::int64_t z ) -> std::uint64_t {
            // Hash products intentionally wrap modulo 2^64. Convert before
            // multiplication so large translated cells have defined arithmetic.
		return static_cast<std::uint64_t>(x) * 73856093ULL
                ^ static_cast<std::uint64_t>(y) * 19349663ULL
                ^ static_cast<std::uint64_t>(z) * 83492791ULL;
	};

	std::unordered_map<std::uint64_t, std::vector<unsigned int>> grid;
	grid.reserve( points.size() );
	std::vector<Point3> weldedPos;
	weldedPos.reserve( points.size() );
	const Scalar epsSq = eps * eps;

	for( std::size_t i = 0; i < points.size(); ++i ) {
		const Point3& p = points[i];
		const std::int64_t cx = cellCoord( p.x-origin.x );
		const std::int64_t cy = cellCoord( p.y-origin.y );
		const std::int64_t cz = cellCoord( p.z-origin.z );

		unsigned int foundId = 0xFFFFFFFFu;
		for( int dz = -1; dz <= 1 && foundId == 0xFFFFFFFFu; ++dz ) {
			for( int dy = -1; dy <= 1 && foundId == 0xFFFFFFFFu; ++dy ) {
				for( int dx = -1; dx <= 1 && foundId == 0xFFFFFFFFu; ++dx ) {
					const std::uint64_t key = cellKey( cx + dx, cy + dy, cz + dz );
					std::unordered_map<std::uint64_t, std::vector<unsigned int>>::const_iterator git = grid.find( key );
					if( git == grid.end() ) { continue; }
					const std::vector<unsigned int>& candidates = git->second;
					for( std::size_t c = 0; c < candidates.size(); ++c ) {
						const unsigned int cand = candidates[c];
						const Vector3 d = Vector3Ops::mkVector3( weldedPos[cand], p );
						if( Vector3Ops::SquaredModulus( d ) <= epsSq ) {
							foundId = cand;
							break;
						}
					}
				}
			}
		}

		if( foundId == 0xFFFFFFFFu ) {
			foundId = (unsigned int)weldedPos.size();
			weldedPos.push_back( p );
			grid[ cellKey( cx, cy, cz ) ].push_back( foundId );
		}
		outWeldedId[i] = foundId;
	}
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
			return static_cast<std::size_t>( static_cast<std::uint64_t>( k.x ) * 73856093ULL
				^ static_cast<std::uint64_t>( k.y ) * 19349663ULL
				^ static_cast<std::uint64_t>( k.z ) * 83492791ULL );
		}
	};
}

GeometricUtilities::OpenSurfaceCertificate GeometricUtilities::IsEmbeddedOpenOrientedSurface(
	const std::vector<Point3>& corners
	)
{
	OpenSurfaceCertificate result;
	const std::size_t nTris = corners.size() / 3;
	if( nTris == 0 || corners.size() % 3 != 0 || nTris > 0x3FFFFFFFu ) {
		return result;
	}
	std::vector<unsigned int> wid;
	Scalar eps = 0;
	if( !WeldVertexPositions( corners, wid, &eps ) ) {
		return result;
	}
	const Scalar epsSq = eps * eps;

	// 1. Non-degenerate: distinct welded corners, smallest altitude > eps
	// (altitude = 2 x area / longest edge).
	for( std::size_t t = 0; t < nTris; ++t ) {
		const unsigned int* I = &wid[3 * t];
		if( I[0] == I[1] || I[1] == I[2] || I[0] == I[2] ) {
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
			return result;
		}
	}

	// 2. Manifold edges, consistently oriented.
	struct EdgeUse
	{
		unsigned int tri[2] = { 0, 0 };
		bool forward[2] = { false, false };
		unsigned int count = 0;
	};
	std::unordered_map<std::uint64_t, EdgeUse> edges;
	edges.reserve( nTris * 2 );
	std::vector<EdgeUse*> triEdges( 3 * nTris, nullptr );
	for( std::size_t t = 0; t < nTris; ++t ) {
		for( int k = 0; k < 3; ++k ) {
			const unsigned int a = wid[3 * t + k], b = wid[3 * t + ( k + 1 ) % 3];
			const std::uint64_t key = a < b ? ( ( std::uint64_t( a ) << 32 ) | b ) : ( ( std::uint64_t( b ) << 32 ) | a );
			EdgeUse& e = edges[key];
			if( e.count >= 2 ) {
				return result;		// non-manifold edge
			}
			e.tri[e.count] = (unsigned int)t;
			e.forward[e.count] = a < b;
			++e.count;
			triEdges[3 * t + k] = &e;	// element addresses are stable in unordered_map
		}
	}
	for( const auto& kv : edges ) {
		if( kv.second.count == 2 && kv.second.forward[0] == kv.second.forward[1] ) {
			return result;			// traversed the same way twice: inconsistent orientation
		}
	}

	// 2b. Single connected component: all triangles connected via manifold edges.
	// A collection of disconnected sheets in one mesh can bound an interior or
	// have inconsistent facing; each open sheet must be its own object.
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
		for( int attempt = 0; attempt < 64; ++attempt ) {
			double total = 0;
			for( std::size_t t = 0; t < nTris && total <= budget; ++t ) {
				total += double( coord( hi[t].x, gLo.x ) - coord( lo[t].x, gLo.x ) + 1 )
					* double( coord( hi[t].y, gLo.y ) - coord( lo[t].y, gLo.y ) + 1 )
					* double( coord( hi[t].z, gLo.z ) - coord( lo[t].z, gLo.z ) + 1 );
			}
			if( total <= budget ) {
				break;
			}
			cell *= 2;
		}
		std::unordered_map<CellKey, std::vector<unsigned int>, CellKeyHash> grid;
		grid.reserve( nTris * 2 );
		for( std::size_t t = 0; t < nTris; ++t ) {
			const std::int64_t x0 = coord( lo[t].x, gLo.x ), x1 = coord( hi[t].x, gLo.x );
			const std::int64_t y0 = coord( lo[t].y, gLo.y ), y1 = coord( hi[t].y, gLo.y );
			const std::int64_t z0 = coord( lo[t].z, gLo.z ), z1 = coord( hi[t].z, gLo.z );
			for( std::int64_t z = z0; z <= z1; ++z ) {
				for( std::int64_t y = y0; y <= y1; ++y ) {
					for( std::int64_t x = x0; x <= x1; ++x ) {
						grid[CellKey{ x, y, z }].push_back( (unsigned int)t );
					}
				}
			}
		}
		for( const auto& kv : grid ) {
			const std::vector<unsigned int>& list = kv.second;
			for( std::size_t a = 0; a < list.size(); ++a ) {
				const unsigned int i = list[a];
				for( std::size_t b = a + 1; b < list.size(); ++b ) {
					const unsigned int j = list[b];
					if( lo[i].x > hi[j].x || lo[j].x > hi[i].x ||
						lo[i].y > hi[j].y || lo[j].y > hi[i].y ||
						lo[i].z > hi[j].z || lo[j].z > hi[i].z ) {
						continue;	// inflated boxes apart: the gap exceeds eps
					}
					const CellKey ref{ coord( std::max( lo[i].x, lo[j].x ), gLo.x ),
						coord( std::max( lo[i].y, lo[j].y ), gLo.y ),
						coord( std::max( lo[i].z, lo[j].z ), gLo.z ) };
					if( !( ref == kv.first ) ) {
						continue;	// tested in the reference cell
					}
					if( !PairMeetsOnlyInSharedFaces( &corners[3 * i], &wid[3 * i], &corners[3 * j], &wid[3 * j], epsSq ) ) {
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
