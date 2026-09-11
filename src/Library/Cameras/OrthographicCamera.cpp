//////////////////////////////////////////////////////////////////////
//
//  OrthographicCamera.cpp - Implementation of the pinhole camera
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
#include "OrthographicCamera.h"
#include "CameraTransforms.h"

using namespace RISE;
using namespace RISE::Implementation;

void OrthographicCamera::Recompute( const unsigned int width, const unsigned int height )
{
	if( from_onb ) {
		frame.SetOrigin( vPosition );
	} else {
		Vector3 vNewUp = vUp;
		Point3 ptNewPosition = vPosition;

		CameraTransforms::AdjustCameraForThetaPhi( target_orientation, vPosition, vLookAt, vUp, ptNewPosition, vNewUp );

		Vector3 vForward = Vector3Ops::Normalize(Vector3Ops::mkVector3( vLookAt, ptNewPosition ));
		CameraTransforms::AdjustCameraForOrientation( vForward, vNewUp, vForward, vNewUp, orientation );

		OrthonormalBasis3D	onb;
		onb.CreateFromWV( vForward, vNewUp );

		frame = Frame( onb, ptNewPosition, width, height );
	}

	const Matrix4 m1 = Matrix4Ops::Translation( Vector3( -0.5 * Scalar(width), -0.5 * Scalar(height), -1.0 ) );
	const Matrix4 m2 = ComputeScaleFromAR( );
	const Matrix4 m3 = frame.GetTransformationMatrix();
	
	mxTrans = m3 * m2 * m1;
}

OrthographicCamera::OrthographicCamera( 
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const Point3& vLookAt_,				///< [in] A point in the world camera is looking at
	const Vector3& vUp_,				///< [in] What is considered up in the world
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Vector2& vpScale,				///< [in] Viewport scale factor
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate,			///< [in] Scanning rate of the camera
	const Scalar pixelRate,				///< [in] Pixel rate of the camera
	const Vector3& orientation_,		///< [in] Orientation (Pitch,Roll,Yaw)
	const Vector2& target_orientation_	///< [in] Orientation relative to a target
	) : 
  CameraCommon(
	  vPosition_,
	  vLookAt_, 
	  vUp_,
	  pixelAR_,
	  exposure_,
	  scanningRate,
	  pixelRate,
	  orientation_,
	  target_orientation_ ),
  viewportScale( vpScale )
{
	Recompute( width, height );
}

OrthographicCamera::OrthographicCamera( 
	const OrthonormalBasis3D& basis,	///< [in] Basis from which to derive basis vectors
	const Point3& vPosition_,			///< [in] Location of the camera in the world
	const unsigned int width,			///< [in] Width of the frame of the camera
	const unsigned int height,			///< [in] Height of the frame of the camera
	const Vector2& vpScale,				///< [in] Viewport scale factor
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate,			///< [in] Scanning rate of the camera
	const Scalar pixelRate				///< [in] Pixel rate of the camera
	) : 
  CameraCommon( 
	  vPosition_,
	  pixelAR_, 
	  exposure_, 
	  scanningRate, 
	  pixelRate
	  ),
  viewportScale( vpScale )
{
	frame = Frame( basis, vPosition, width, height );

	const Matrix4 m1 = Matrix4Ops::Translation( Vector3( -0.5 * Scalar(width), -0.5 * Scalar(height), 0.0 ) );
	const Matrix4 m2 = ComputeScaleFromAR( );
	const Matrix4 m3 = frame.GetTransformationMatrix();

	mxTrans = m3 * m2 * m1;
}

OrthographicCamera::~OrthographicCamera( )
{
}

bool OrthographicCamera::GenerateRay( const RuntimeContext& rc, Ray& ray, const Point2& ptOnScreen ) const
{
	// The per-film-sample origin offset, as a function of the raster
	// coordinate.  Factored out of the `ray.Set` below ONLY so the
	// differential rays can re-enter the identical expression: the
	// auxiliary ray must be the ray this camera would actually
	// generate for the neighbouring pixel, whatever that expression
	// is, rather than a separately-derived pixel pitch that could
	// drift from it.
	//
	// `x`/`y` are film-plane coordinates; they must be expressed in
	// the CAMERA's basis (frame.GetBasis().u()/.v()), not the world
	// x/y axes.  PinholeCamera/ThinLensCamera do the equivalent
	// routing by pushing the screen point through `mxTrans` (which
	// embeds the frame's rotation); here the offset is built directly
	// from the basis vectors because `mxTrans` operates in raster
	// pixel units via `m1`/`m2` (see Recompute) rather than the
	// fractional-of-viewportScale units `x`/`y` use.
	//
	// Sign: `OrthonormalBasis3D::CreateFromWV(w,v)` sets
	// U = normalize(cross(v,W)) = -cross(W,v), i.e. U is the NEGATION
	// of the conventional "camera right" vector cross(forward,up) --
	// and V = cross(W,U) recovers (the orthogonalized) input `up`
	// exactly.  So for a camera looking down -Z with +Y up,
	// U = -worldX and V = +worldY (verified against
	// `OrthonormalBasis3D::CreateFromWV`'s cross-product definitions),
	// and `x*U + y*V` reduces to `x*(-worldX) + y*worldY` ==
	// `Vector3(-x, y, 0)` -- the exact old (world-axis) expression.
	// That identity holds regardless of orientation (U/V are always
	// -right/up in world space, by the same construction), so this
	// is bit-identical to the old expression for -Z/+Y and correctly
	// rotates the offset into the film plane for every other
	// orientation (e.g. a top-down camera, where V is a horizontal
	// world axis, not world Y).
	const OrthonormalBasis3D& basis = frame.GetBasis();
	auto originOffset = [&]( const Scalar screenX, const Scalar screenY ) -> Vector3 {
		const Scalar x = (frame.GetWidth()/2-screenX)/Scalar(frame.GetWidth()) * viewportScale.x;
		const Scalar y = (screenY - frame.GetHeight()/2)/Scalar(frame.GetHeight()) * viewportScale.y;
		return basis.u() * x + basis.v() * y;
	};

	const Vector3 o = originOffset( ptOnScreen.x, ptOnScreen.y );
	Vector3 v( 0, 0, 1 );

	ray.Set(
		Point3Ops::mkPoint3( frame.GetOrigin(), o ),
		Vector3Ops::Normalize(Vector3Ops::Transform(mxTrans,v))
		);

	// Ray differentials.  An orthographic camera's rays are parallel,
	// so a one-FULL-pixel step (Igehy's convention, matching
	// PinholeCamera) moves only the ORIGIN — by exactly one pixel of
	// viewport pitch — and leaves the direction alone.  That makes the
	// footprint at a hit the viewport pitch divided by the cosine of
	// incidence, which is the correct answer for a parallel projection
	// and is independent of distance (unlike the pinhole's d·theta).
	// Must follow `ray.Set`, which clears hasDifferentials.
	ray.diffs.rxOrigin = originOffset( ptOnScreen.x + Scalar( 1 ), ptOnScreen.y ) - o;
	ray.diffs.ryOrigin = originOffset( ptOnScreen.x, ptOnScreen.y + Scalar( 1 ) ) - o;
	ray.diffs.rxDir = Vector3( 0, 0, 0 );	// parallel projection: shared direction
	ray.diffs.ryDir = Vector3( 0, 0, 0 );
	ray.hasDifferentials = true;

	return true;
}

Matrix4 OrthographicCamera::ComputeScaleFromAR( ) const
{
	const Scalar ar = Scalar(frame.GetWidth()) / Scalar(frame.GetHeight()) * pixelAR;
	return Matrix4Ops::Stretch( Vector3( ar, -1.0, 1.0 ) );
}
