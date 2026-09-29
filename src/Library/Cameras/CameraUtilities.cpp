//////////////////////////////////////////////////////////////////////
//
//  CameraUtilities.cpp - Implementation of BDPT camera utility
//  functions for inverse projection, importance, and PDF.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CameraUtilities.h"
#include "PinholeCamera.h"
#include "ThinLensCamera.h"
#include "FisheyeCamera.h"
#include "OrthographicCamera.h"

using namespace RISE;
using namespace RISE::Implementation;

//
// Access helpers for protected camera members.
// These use the standard C++ derived-class accessor pattern to reach
// protected members that the BDPT utilities require.
//

struct PinholeAccessor : public PinholeCamera {
	static Scalar GetFov( const PinholeCamera& c ) {
		return static_cast<const PinholeAccessor&>(c).fov;
	}
};

struct FisheyeAccessor : public FisheyeCamera {
	static Scalar GetScale( const FisheyeCamera& c ) {
		return static_cast<const FisheyeAccessor&>(c).scale;
	}
};

struct OrthoAccessor : public OrthographicCamera {
	static Vector2 GetViewportScale( const OrthographicCamera& c ) {
		return static_cast<const OrthoAccessor&>(c).viewportScale;
	}
};

//
// Internal helpers
//

namespace {

	//
	// Extracts the optical axis (forward direction) from the camera's
	// transform matrix.  The z-column of the 3x3 rotation part encodes W.
	//
	static inline Vector3 GetOpticalAxis( const Matrix4& mx )
	{
		return Vector3Ops::Normalize( Vector3( mx._20, mx._21, mx._22 ) );
	}

	//
	// Computes cos(theta) where theta is the angle between the ray
	// direction and the camera's optical axis.
	//
	static inline Scalar ComputeCosTheta( const Matrix4& mx, const Ray& ray )
	{
		const Vector3 optAxis = GetOpticalAxis( mx );
		return fabs( Vector3Ops::Dot( ray.Dir(), optAxis ) );
	}

	//////////////////////////////////////////////////////////////////////////
	// Pinhole camera
	//////////////////////////////////////////////////////////////////////////

	static bool RasterizePinhole(
		const PinholeCamera& cam,
		const Point3& worldPoint,
		Point2& rasterPoint )
	{
		// PinholeCamera::GenerateRay pipeline:
		//   p = (px, py, 0)
		//   transP = Transform(mxTrans, p)
		//   v = mkVector3(origin, transP) = origin - transP
		//   ray = (origin, Normalize(v))
		//
		// mxTrans = m3 * m2 * m1 where:
		//   m1 = Translation(-w/2, -h/2, -1)
		//   m2 = Stretch(h/w*ar, -h/h, 1)  with h = 2*tan(fov/2)
		//   m3 = Trans(origin) * BasisToCanonical
		//
		// To invert: compute direction from camera to world point, then
		// find the screen point (px, py, 0) whose transP lies on the
		// same ray.  We use the inverse of mxTrans and intersect with
		// the z=0 screen plane.

		const Matrix4 mx = cam.GetMatrix();
		const Matrix4 mxInv = Matrix4Ops::Inverse( mx );
		const Point3 camPos = cam.GetLocation();

		// Map camera origin and world point to screen space
		const Point3 E = Point3Ops::Transform( mxInv, camPos );
		const Point3 W = Point3Ops::Transform( mxInv, worldPoint );

		// Find intersection of the screen-space ray E->W with z=0
		const Scalar dz = W.z - E.z;
		if( fabs(dz) < NEARZERO ) {
			return false;
		}

		const Scalar t = -E.z / dz;

		// In screen space, E.z > 0 (camera) and visible world points have
		// W.z > E.z (forward from camera).  The z=0 image plane is on the
		// opposite side of the camera from the world, so the line E→W
		// must be extended BACKWARD (t < 0) to reach z=0.  Points behind
		// the camera have W.z < E.z, giving t > 0.
		if( t >= 0.0 ) {
			return false;	// Behind camera
		}

		const Scalar px = E.x + t * (W.x - E.x);
		const Scalar py = E.y + t * (W.y - E.y);

		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );

		// DL-294: film membership is the splat film's decision.
		if( !BDPTCameraUtilities::InRasterGuardBand( px, py, width, height ) ) {
			return false;
		}

		rasterPoint = Point2( px, py );
		return true;
	}

	//
	// Helper: compute the world-space area of a single pixel on the
	// image plane, and the distance from camera to image plane center.
	// Works for any camera with a standard pinhole-like mxTrans.
	//
	static void ComputePixelAreaAndDistance(
		const CameraCommon& cam,
		Scalar& pixelArea,
		Scalar& distToPlane )
	{
		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );
		const Matrix4 mx = cam.GetMatrix();
		const Point3 camPos = cam.GetLocation();

		const Point3 centerScreen( width * 0.5, height * 0.5, 0.0 );
		const Point3 rightScreen( width * 0.5 + 1.0, height * 0.5, 0.0 );
		const Point3 downScreen( width * 0.5, height * 0.5 + 1.0, 0.0 );

		const Point3 centerWorld = Point3Ops::Transform( mx, centerScreen );
		const Point3 rightWorld = Point3Ops::Transform( mx, rightScreen );
		const Point3 downWorld = Point3Ops::Transform( mx, downScreen );

		const Vector3 dRight = Vector3Ops::mkVector3( rightWorld, centerWorld );
		const Vector3 dDown = Vector3Ops::mkVector3( downWorld, centerWorld );

		pixelArea = Vector3Ops::Magnitude( Vector3Ops::Cross( dRight, dDown ) );
		distToPlane = Vector3Ops::Magnitude( Vector3Ops::mkVector3( centerWorld, camPos ) );
	}

	static Scalar ImportancePinhole(
		const PinholeCamera& cam,
		const Ray& ray )
	{
		// We = d^2 / (A_pixel * W * H * cos^3(theta))
		// where d is the image plane distance, A_pixel is world-space
		// pixel area, and W*H is the total pixel count.

		const Scalar cosTheta = ComputeCosTheta( cam.GetMatrix(), ray );
		if( cosTheta < NEARZERO ) {
			return 0.0;
		}

		Scalar pixelArea, distToPlane;
		ComputePixelAreaAndDistance( cam, pixelArea, distToPlane );

		if( pixelArea < NEARZERO ) {
			return 0.0;
		}

		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );
		const Scalar cos3 = cosTheta * cosTheta * cosTheta;

		return (distToPlane * distToPlane) / (pixelArea * width * height * cos3);
	}

	static Scalar PdfDirectionPinhole(
		const PinholeCamera& cam,
		const Ray& ray )
	{
		// PDF over solid angle for a single pixel:
		// p(omega) = d^2 / (A_pixel_world * cos^3(theta))

		const Scalar cosTheta = ComputeCosTheta( cam.GetMatrix(), ray );
		if( cosTheta < NEARZERO ) {
			return 0.0;
		}

		Scalar pixelArea, distToPlane;
		ComputePixelAreaAndDistance( cam, pixelArea, distToPlane );

		if( pixelArea < NEARZERO ) {
			return 0.0;
		}

		const Scalar cos3 = cosTheta * cosTheta * cosTheta;
		return (distToPlane * distToPlane) / (cos3 * pixelArea);
	}

	//////////////////////////////////////////////////////////////////////////
	// Thin lens camera
	//////////////////////////////////////////////////////////////////////////

	static bool RasterizeThinLens(
		const ThinLensCamera& cam,
		const Point3& worldPoint,
		Point2& rasterPoint )
	{
		// Lens-CENTRE projection: what a pinhole of the same field of
		// view would report.  Delegated to the camera's own inverse of
		// its ray generation rather than re-derived here, so lens
		// SHIFT and focal-plane TILT are inverted instead of silently
		// ignored -- the hand-rolled `px = w/2 - k*x/z` this used to
		// carry knew about neither, and disagreed with GenerateRay for
		// any camera that set them.
		//
		// `BDPTCameraUtilities::RasterizeThrough` is the entry point
		// that passes a SAMPLED aperture point instead of the centre;
		// this one remains the answer for callers that only want the
		// chief-ray pixel (AOV probes, the degenerate-aperture limit).
		return cam.RasterFromLensPoint( worldPoint, Point3( 0, 0, 0 ), rasterPoint );
	}

	static Scalar ImportanceThinLens(
		const ThinLensCamera& cam,
		const Ray& ray )
	{
		// PBRT-v4's thin-lens importance is
		//     We = d^2 / (A_lens * W * H * cos^4(theta)),
		// and its light-tracing estimator divides by the solid-angle
		// density of the sampled lens point, p_w = dist^2 / (A_lens *
		// cos(theta)).  Per the contract on BDPTCameraUtilities::
		// Importance this function returns the PRODUCT of `We` with the
		// aperture cosine and the aperture area -- i.e. what is left
		// once the caller's 1/p_w has cancelled the 1/A_lens:
		//
		//     We * cos(theta) * A_lens = k / (W * H * cos^3(theta))
		//
		// with k = GetImagePlanePixelDensity() = d^2 / pixelAR, d the
		// image-plane distance in PIXELS (see the note in
		// PdfDirectionThinLens for where the 1/pixelAR comes from).
		// A_lens is GONE.  That is the whole of debt 28: a thin lens
		// gathers exactly as much radiance per unit film area as a
		// pinhole of the same field of view (opening up trades depth of
		// field for noise, not exposure), so the t==1 contribution is
		// the pinhole's and only the RASTER POSITION knows about the
		// aperture.  Keeping the 1/A_lens while connecting to the lens
		// centre inflated every splat by 1/(cos(theta) * A_lens) --
		// 2.7e6 for a 15.1 mm f/22 lens in a metres scene.
		//
		// The pinhole limit is therefore continuous and needs no
		// special case: as fstop -> infinity this expression is
		// unchanged, which is exactly the behaviour the f/1e5
		// continuity check in tests/CameraImportanceTest.cpp pins.
		// (It also retires the `reinterpret_cast<const PinholeCamera*>`
		// this branch used to take for a degenerate aperture, which was
		// undefined behaviour AND read a pinhole's fov-baked mxTrans
		// off a thin lens's scene-unit one.)
		const Scalar cosTheta = ComputeCosTheta( cam.GetMatrix(), ray );
		if( cosTheta < NEARZERO ) {
			return 0.0;
		}

		const Scalar k = cam.GetImagePlanePixelDensity();
		if( k <= 0 ) {
			return 0.0;
		}

		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );
		const Scalar cos3 = cosTheta * cosTheta * cosTheta;

		return k / (width * height * cos3);
	}

	static Scalar PdfDirectionThinLens(
		const ThinLensCamera& cam,
		const Ray& ray )
	{
		// Solid-angle density of one pixel's direction GIVEN a point on
		// the lens:  p(omega | lens point) = k / cos^3(theta).
		//
		// It does not depend on WHICH lens point, for zero focal-plane
		// tilt: the film-sample -> focus-point map is then a uniform
		// magnification (oneMinusT is constant across the sensor), so
		// the area Jacobian, and with it the density, is the same from
		// every point on the aperture -- which is what lets the t==1
		// sites keep a `PdfDirection( cam, ray )` signature.  With tilt
		// the magnification varies across the sensor and this is an
		// approximation; that is unchanged from before debt 28, tilt
		// defaults to 0, and it perturbs MIS weights only (never the
		// contribution).
		//
		// `k` carries the 1/pixelAR that the previous `d^2 / cos^3`
		// form dropped: `mxTrans` stretches camera-space x by pixelAR,
		// so the world-space pixel on the image plane is pixelAR times
		// wider than the sensor-local one.  PdfDirectionPinhole, which
		// measures the pixel off the matrix, always had it -- the two
		// cameras now agree exactly at matched field of view.
		const Scalar cosTheta = ComputeCosTheta( cam.GetMatrix(), ray );
		if( cosTheta < NEARZERO ) {
			return 0.0;
		}

		const Scalar k = cam.GetImagePlanePixelDensity();
		if( k <= 0 ) {
			return 0.0;
		}

		const Scalar cos3 = cosTheta * cosTheta * cosTheta;
		return k / cos3;
	}

	//////////////////////////////////////////////////////////////////////////
	// Fisheye camera
	//////////////////////////////////////////////////////////////////////////

	//
	// Helper: transform a world-space direction to the fisheye's local
	// frame and return the normalized local direction.
	//
	static inline Vector3 FisheyeWorldToLocal(
		const Matrix4& mxInv,
		const Vector3& worldDir )
	{
		// Vector3Ops::Transform uses only the 3x3 rotational part.
		// mxInv's 3x3 part = Stretch(1/ar,1,1) * BasisTranspose,
		// which is the inverse of the rotation+stretch used in GenerateRay.
		return Vector3Ops::Normalize( Vector3Ops::Transform( mxInv, worldDir ) );
	}

	static bool RasterizeFisheye(
		const FisheyeCamera& cam,
		const Point3& worldPoint,
		Point2& rasterPoint )
	{
		// FisheyeCamera::GenerateRay forward transform:
		//   x = (scale/2) - scale * px / width
		//   y = scale * py / height - (scale/2)
		//   radius = sqrt(x^2 + y^2);  reject if > 1
		//   v = (x, y, sqrt(1 - radius^2))
		//   dir = Normalize(Transform(mxTrans, v))
		//
		// Inverse: direction -> local v -> (x,y) -> (px, py)

		const Point3 camPos = cam.GetLocation();
		const Vector3 toPoint = Vector3Ops::Normalize(
			Vector3Ops::mkVector3( worldPoint, camPos ) );

		const Matrix4 mxInv = Matrix4Ops::Inverse( cam.GetMatrix() );
		const Vector3 localDir = FisheyeWorldToLocal( mxInv, toPoint );

		if( localDir.z < NEARZERO ) {
			return false;	// Behind camera hemisphere
		}

		// localDir = (x, y, sqrt(1-r^2)) where r = sqrt(x^2+y^2)
		// and indeed x = localDir.x, y = localDir.y since the mapping
		// v.x = radius*cos(theta) = x, v.y = radius*sin(theta) = y.
		const Scalar x = localDir.x;
		const Scalar y = localDir.y;
		const Scalar radius = sqrt( x * x + y * y );

		if( radius > 1.0 ) {
			return false;
		}

		// Invert pixel-to-(x,y) mapping:
		//   x = (scale/2) - scale * px / width
		//   => px = (scale/2 - x) * width / scale
		//
		//   y = scale * py / height - (scale/2)
		//   => py = (y + scale/2) * height / scale

		const Scalar scale = FisheyeAccessor::GetScale( cam );
		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );

		const Scalar px = (scale * 0.5 - x) * width / scale;
		const Scalar py = (y + scale * 0.5) * height / scale;

		// DL-294: film membership is the splat film's decision.
		if( !BDPTCameraUtilities::InRasterGuardBand( px, py, width, height ) ) {
			return false;
		}

		rasterPoint = Point2( px, py );
		return true;
	}

	//! DL-10: Jacobian of the map v -> normalize(Stretch(pixelAR,1,1) * v),
	//! restricted to the unit sphere's tangent planes -- i.e. world-space
	//! solid angle per unit of the UN-stretched local solid angle that
	//! `pixelSolidAngle` below measures.
	//!
	//! `GenerateRay` builds the local hemisphere point v = (x, y,
	//! sqrt(1-r^2)) and sends the world direction through
	//! `Normalize(Transform(mxTrans, v))`, where mxTrans's 3x3 part is
	//! R * Stretch(pixelAR,1,1) for a rotation R.  Since R is orthonormal,
	//! Normalize(R*u) = R*Normalize(u), so the stretch-then-normalize step
	//! happens entirely in the PRE-rotation frame and rotation alone does
	//! not change solid angle.  For an invertible linear map A applied to
	//! a unit vector v and renormalized (n = Av/|Av|), the standard result
	//! (used identically for a uniform-sphere sample warped by a linear
	//! transform, e.g. GGX's stretch-invariant sampling) is that the
	//! solid-angle Jacobian of v -> n is |det(A)| / |Av|^3.  Here
	//! A = diag(pixelAR, 1, 1), so det(A) = pixelAR.
	//!
	//! `v` must already be the UNIT local hemisphere point (what
	//! `FisheyeWorldToLocal` returns) -- `Optics`-style callers never need
	//! to renormalize it first, since `FisheyeWorldToLocal` recovers v
	//! EXACTLY regardless of pixelAR (the two normalize divisions in the
	//! forward map and its inverse cancel algebraically).
	//!
	//! At `pixelAR == 1` this is exactly 1 (A is the identity); special-
	//! cased so the ubiquitous square-pixel path stays bit-identical to
	//! the pre-fix formula rather than 1.0 recovered via division by a
	//! floating-point |Av| that may not be EXACTLY 1.
	static inline Scalar FisheyeStretchJacobian( const Scalar pixelAR, const Vector3& v )
	{
		if( pixelAR == Scalar( 1 ) ) {
			return Scalar( 1 );
		}
		const Scalar sx = pixelAR * v.x;
		const Scalar sMagSq = sx * sx + v.y * v.y + v.z * v.z;
		return pixelAR / ( sMagSq * sqrt( sMagSq ) );
	}

	static Scalar ImportanceFisheye(
		const FisheyeCamera& cam,
		const Ray& ray )
	{
		// The fisheye uses a hemispherical projection:
		//   v = (x, y, sqrt(1 - r^2))  where r = sqrt(x^2+y^2)
		// The solid angle per pixel depends on the cos of the angle
		// from the optical axis:
		//   d(omega)/d(pixel) = scale^2 / (W * H * cosAngle)
		// That is the solid angle in the PRE-STRETCH local frame; the
		// world-space solid angle folds in FisheyeStretchJacobian (DL-10)
		// on top of it, exactly 1 at pixelAR == 1.
		// We = 1 / (d(omega_world)/d(pixel) * W * H)

		const Matrix4 mxInv = Matrix4Ops::Inverse( cam.GetMatrix() );
		const Vector3 localDir = FisheyeWorldToLocal( mxInv, ray.Dir() );

		if( localDir.z < NEARZERO ) {
			return 0.0;
		}

		const Scalar radius = sqrt( localDir.x * localDir.x + localDir.y * localDir.y );
		if( radius > 1.0 ) {
			return 0.0;
		}

		const Scalar scale = FisheyeAccessor::GetScale( cam );
		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );
		const Scalar cosAngle = localDir.z;
		const Scalar pixelSolidAngle = (scale * scale) / (width * height * cosAngle);

		if( pixelSolidAngle < NEARZERO ) {
			return 0.0;
		}

		const Scalar jacobian = FisheyeStretchJacobian( cam.GetPixelAR(), localDir );
		if( jacobian < NEARZERO ) {
			return 0.0;
		}

		return 1.0 / (pixelSolidAngle * jacobian * width * height);
	}

	static Scalar PdfDirectionFisheye(
		const FisheyeCamera& cam,
		const Ray& ray )
	{
		// PDF over WORLD solid angle for one pixel:
		// p(omega_world) = 1 / (pixelSolidAngle_local * jacobian)
		// (DL-10; see ImportanceFisheye and FisheyeStretchJacobian above)

		const Matrix4 mxInv = Matrix4Ops::Inverse( cam.GetMatrix() );
		const Vector3 localDir = FisheyeWorldToLocal( mxInv, ray.Dir() );

		if( localDir.z < NEARZERO ) {
			return 0.0;
		}

		const Scalar radius = sqrt( localDir.x * localDir.x + localDir.y * localDir.y );
		if( radius > 1.0 ) {
			return 0.0;
		}

		const Scalar scale = FisheyeAccessor::GetScale( cam );
		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );
		const Scalar cosAngle = localDir.z;
		const Scalar pixelSolidAngle = (scale * scale) / (width * height * cosAngle);

		if( pixelSolidAngle < NEARZERO ) {
			return 0.0;
		}

		const Scalar jacobian = FisheyeStretchJacobian( cam.GetPixelAR(), localDir );
		if( jacobian < NEARZERO ) {
			return 0.0;
		}

		return 1.0 / (pixelSolidAngle * jacobian);
	}

	//////////////////////////////////////////////////////////////////////////
	// Orthographic camera
	//////////////////////////////////////////////////////////////////////////

	static bool RasterizeOrthographic(
		const OrthographicCamera& cam,
		const Point3& worldPoint,
		Point2& rasterPoint )
	{
		// OrthographicCamera::GenerateRay does:
		//   x = (w/2 - px) / w * vpScale.x
		//   y = (py - h/2) / h * vpScale.y
		//   ray origin = frame.GetOrigin() + Vector3(-x, y, 0)
		//   ray dir = Normalize(Transform(mxTrans, (0,0,1)))
		//
		// Note: the offset (-x, y, 0) is in WORLD coordinates.
		//
		// To invert: project worldPoint onto the image plane along the
		// optical axis, then solve for pixel coordinates.

		const Point3 camPos = cam.GetLocation();
		const Matrix4 mx = cam.GetMatrix();

		// All rays share the same direction
		const Vector3 optAxis = Vector3Ops::Normalize(
			Vector3Ops::Transform( mx, Vector3( 0.0, 0.0, 1.0 ) ) );

		const Vector3 toPoint = Vector3Ops::mkVector3( worldPoint, camPos );
		const Scalar dist = Vector3Ops::Dot( toPoint, optAxis );

		if( dist < 0.0 ) {
			return false;	// Behind camera
		}

		// Project onto the image plane (remove component along optical axis)
		const Vector3 projected(
			toPoint.x - dist * optAxis.x,
			toPoint.y - dist * optAxis.y,
			toPoint.z - dist * optAxis.z );

		// Invert the origin offset mapping:
		//   projected.x = (px - w/2) / w * vpScale.x
		//   projected.y = (py - h/2) / h * vpScale.y
		const Vector2 vpScale = OrthoAccessor::GetViewportScale( cam );
		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );

		if( fabs(vpScale.x) < NEARZERO || fabs(vpScale.y) < NEARZERO ) {
			return false;
		}

		const Scalar px = projected.x * width / vpScale.x + width * 0.5;
		const Scalar py = projected.y * height / vpScale.y + height * 0.5;

		// DL-294: film membership is the splat film's decision.
		if( !BDPTCameraUtilities::InRasterGuardBand( px, py, width, height ) ) {
			return false;
		}

		rasterPoint = Point2( px, py );
		return true;
	}

	static Scalar ImportanceOrthographic(
		const OrthographicCamera& cam,
		const Ray& ray )
	{
		// Orthographic has uniform importance: We = 1 / A_image
		const Vector2 vpScale = OrthoAccessor::GetViewportScale( cam );
		const Scalar imageArea = vpScale.x * vpScale.y;

		if( imageArea < NEARZERO ) {
			return 0.0;
		}

		return 1.0 / imageArea;
	}

	static Scalar PdfDirectionOrthographic(
		const OrthographicCamera& cam,
		const Ray& ray )
	{
		// All orthographic rays are parallel, so the direction PDF is a
		// delta function in solid angle.  In BDPT area-product measure,
		// the PDF is 1/A_image.  We return this; the caller must handle
		// the orthographic case in the path integral appropriately.
		const Vector2 vpScale = OrthoAccessor::GetViewportScale( cam );
		const Scalar imageArea = vpScale.x * vpScale.y;

		if( imageArea < NEARZERO ) {
			return 0.0;
		}

		return 1.0 / imageArea;
	}

} // anonymous namespace


//
// Public interface
//

bool BDPTCameraUtilities::Rasterize(
	const ICamera& cam,
	const Point3& worldPoint,
	Point2& rasterPoint )
{
	const PinholeCamera* pinhole = dynamic_cast<const PinholeCamera*>( &cam );
	if( pinhole ) {
		return RasterizePinhole( *pinhole, worldPoint, rasterPoint );
	}

	const ThinLensCamera* thinLens = dynamic_cast<const ThinLensCamera*>( &cam );
	if( thinLens ) {
		return RasterizeThinLens( *thinLens, worldPoint, rasterPoint );
	}

	const FisheyeCamera* fisheye = dynamic_cast<const FisheyeCamera*>( &cam );
	if( fisheye ) {
		return RasterizeFisheye( *fisheye, worldPoint, rasterPoint );
	}

	const OrthographicCamera* ortho = dynamic_cast<const OrthographicCamera*>( &cam );
	if( ortho ) {
		return RasterizeOrthographic( *ortho, worldPoint, rasterPoint );
	}

	return false;
}

Scalar BDPTCameraUtilities::Importance(
	const ICamera& cam,
	const Ray& ray )
{
	const PinholeCamera* pinhole = dynamic_cast<const PinholeCamera*>( &cam );
	if( pinhole ) {
		return ImportancePinhole( *pinhole, ray );
	}

	const ThinLensCamera* thinLens = dynamic_cast<const ThinLensCamera*>( &cam );
	if( thinLens ) {
		return ImportanceThinLens( *thinLens, ray );
	}

	const FisheyeCamera* fisheye = dynamic_cast<const FisheyeCamera*>( &cam );
	if( fisheye ) {
		return ImportanceFisheye( *fisheye, ray );
	}

	const OrthographicCamera* ortho = dynamic_cast<const OrthographicCamera*>( &cam );
	if( ortho ) {
		return ImportanceOrthographic( *ortho, ray );
	}

	return 0.0;
}

Scalar BDPTCameraUtilities::PdfDirection(
	const ICamera& cam,
	const Ray& ray )
{
	const PinholeCamera* pinhole = dynamic_cast<const PinholeCamera*>( &cam );
	if( pinhole ) {
		return PdfDirectionPinhole( *pinhole, ray );
	}

	const ThinLensCamera* thinLens = dynamic_cast<const ThinLensCamera*>( &cam );
	if( thinLens ) {
		return PdfDirectionThinLens( *thinLens, ray );
	}

	const FisheyeCamera* fisheye = dynamic_cast<const FisheyeCamera*>( &cam );
	if( fisheye ) {
		return PdfDirectionFisheye( *fisheye, ray );
	}

	const OrthographicCamera* ortho = dynamic_cast<const OrthographicCamera*>( &cam );
	if( ortho ) {
		return PdfDirectionOrthographic( *ortho, ray );
	}

	return 0.0;
}

BDPTCameraUtilities::ApertureSample BDPTCameraUtilities::SampleAperture(
	const ICamera& cam,
	const Point2& uv )
{
	ApertureSample ap;

	const ThinLensCamera* thinLens = dynamic_cast<const ThinLensCamera*>( &cam );
	if( thinLens && thinLens->GetApertureWorldArea() > 0 ) {
		ap.local = thinLens->SampleLensPoint( uv );
		ap.point = thinLens->LensPointToWorld( ap.local );
		ap.isFinite = true;
		return ap;
	}

	// Every other camera -- and a thin lens stopped all the way down --
	// emits from a single point.  `local` stays (0,0,0), which is the
	// lens centre for the degenerate thin lens and unused otherwise.
	ap.point = cam.GetLocation();
	ap.isFinite = false;
	return ap;
}

bool BDPTCameraUtilities::RasterizeThrough(
	const ICamera& cam,
	const Point3& worldPoint,
	const ApertureSample& ap,
	Point2& rasterPoint )
{
	if( ap.isFinite ) {
		const ThinLensCamera* thinLens = dynamic_cast<const ThinLensCamera*>( &cam );
		if( thinLens ) {
			return thinLens->RasterFromLensPoint( worldPoint, ap.local, rasterPoint );
		}
	}

	return Rasterize( cam, worldPoint, rasterPoint );
}

bool BDPTCameraUtilities::HasFiniteAperture(
	const ICamera& cam )
{
	const ThinLensCamera* thinLens = dynamic_cast<const ThinLensCamera*>( &cam );
	return thinLens != 0 && thinLens->GetApertureWorldArea() > 0;
}

Point2 BDPTCameraUtilities::DrawApertureSample(
	const ICamera& cam,
	ISampler& sampler,
	ApertureStreamPolicy policy )
{
	// Gate BOTH the stream switch and the draw.  Switching streams on
	// a pinhole would be harmless for Sobol (the next StartStream
	// overwrites the dimension counter) but it is not harmless for
	// PSSMLT, where `StartStream` also resets `sampleIndex` -- and the
	// point of this helper is that every camera without an aperture
	// leaves the sampler in exactly the state it was in.
	if( !HasFiniteAperture( cam ) ) {
		return Point2( 0, 0 );
	}

	if( policy == APERTURE_DEDICATED_STREAM ) {
		sampler.StartStream( kApertureSamplerStream );
	}

	return sampler.Get2D();
}

bool BDPTCameraUtilities::IsDeltaDirection(
	const ICamera& cam )
{
	// Orthographic cameras emit a single parallel direction per pixel —
	// a Dirac delta in solid angle.  All other supported cameras
	// (pinhole, thin lens, fisheye) map each pixel to a distinct,
	// non-delta direction, so the light-tracing (t==1) connection is a
	// legitimate sampling strategy for them.
	return dynamic_cast<const OrthographicCamera*>( &cam ) != 0;
}
