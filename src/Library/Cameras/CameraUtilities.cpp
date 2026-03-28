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

struct ThinLensAccessor : public ThinLensCamera {
	static Scalar GetFov( const ThinLensCamera& c ) {
		return static_cast<const ThinLensAccessor&>(c).fov;
	}
	static Scalar GetHalfAperture( const ThinLensCamera& c ) {
		return static_cast<const ThinLensAccessor&>(c).halfAperture;
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

		if( px < 0.0 || px >= width || py < 0.0 || py >= height ) {
			return false;
		}

		rasterPoint = Point2( px, py );
		return true;
	}

	//
	// Helper: compute the image plane distance (in pixel units) for a
	// pinhole-style camera with given FOV and image height.
	// Returns height / (2 * tan(fov/2)).
	//
	static inline Scalar ComputeImagePlaneDistance( Scalar fov, Scalar height )
	{
		return height / (2.0 * tan( fov * 0.5 ));
	}

	//
	// Helper: compute the world-space area of a single pixel on the
	// image plane, and the distance from camera to image plane center.
	// Works for any camera with a standard pinhole-like mxTrans.
	//
	static void ComputePixelAreaAndDistance(
		const ICamera& cam,
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
		// A thin lens camera projects through the lens center identically
		// to a pinhole with the same FOV.  The pixel assignment for a world
		// point depends only on the direction from lens center to the point.
		//
		// In the thin lens's local space (via mxInv), localP.x/localP.z
		// and localP.y/localP.z encode the angular tangent of the direction.
		// The pixel mapping is:
		//   px = w/2 - k * localP.x / localP.z
		//   py = h/2 + k * localP.y / localP.z
		// where k = height / (2 * tan(fov/2)).
		//
		// mxTrans for thin lens = Trans(origin) * BasisToCanonical * Stretch(ar,1,1)
		// mxInv = Stretch(1/ar,1,1) * BasisTranspose * Trans(-origin)

		const Matrix4 mx = cam.GetMatrix();
		const Matrix4 mxInv = Matrix4Ops::Inverse( mx );

		const Point3 localP = Point3Ops::Transform( mxInv, worldPoint );
		// Camera origin maps to (0,0,0) in local space, so localP IS the
		// local-space direction from lens center to world point.

		if( localP.z < NEARZERO ) {
			return false;	// Behind camera
		}

		const Scalar fov = ThinLensAccessor::GetFov( cam );
		const Scalar width = Scalar( cam.GetWidth() );
		const Scalar height = Scalar( cam.GetHeight() );
		const Scalar k = ComputeImagePlaneDistance( fov, height );

		const Scalar px = width * 0.5 - k * localP.x / localP.z;
		const Scalar py = height * 0.5 + k * localP.y / localP.z;

		if( px < 0.0 || px >= width || py < 0.0 || py >= height ) {
			return false;
		}

		rasterPoint = Point2( px, py );
		return true;
	}

	static Scalar ImportanceThinLens(
		const ThinLensCamera& cam,
		const Ray& ray )
	{
		// We = d^2 / (A_lens * W * H * cos^4(theta))
		// A_lens = PI * halfAperture^2

		const Scalar cosTheta = ComputeCosTheta( cam.GetMatrix(), ray );
		if( cosTheta < NEARZERO ) {
			return 0.0;
		}

		const Scalar halfAperture = ThinLensAccessor::GetHalfAperture( cam );
		const Scalar lensArea = PI * halfAperture * halfAperture;
		const Scalar fov = ThinLensAccessor::GetFov( cam );
		const Scalar height = Scalar( cam.GetHeight() );
		const Scalar width = Scalar( cam.GetWidth() );

		if( lensArea < NEARZERO ) {
			// Degenerate: zero aperture, treat as pinhole
			const PinholeCamera* ph = reinterpret_cast<const PinholeCamera*>( &cam );
			return ImportancePinhole( *ph, ray );
		}

		const Scalar d = ComputeImagePlaneDistance( fov, height );
		const Scalar cos4 = cosTheta * cosTheta * cosTheta * cosTheta;

		return (d * d) / (lensArea * width * height * cos4);
	}

	static Scalar PdfDirectionThinLens(
		const ThinLensCamera& cam,
		const Ray& ray )
	{
		// Conditional PDF of this direction given a lens point:
		// p(omega | lens_pt) = d^2 / cos^3(theta)
		// (normalized over the solid angle subtended by the image)

		const Scalar cosTheta = ComputeCosTheta( cam.GetMatrix(), ray );
		if( cosTheta < NEARZERO ) {
			return 0.0;
		}

		const Scalar fov = ThinLensAccessor::GetFov( cam );
		const Scalar height = Scalar( cam.GetHeight() );
		const Scalar d = ComputeImagePlaneDistance( fov, height );
		const Scalar cos3 = cosTheta * cosTheta * cosTheta;

		return (d * d) / cos3;
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

		if( px < 0.0 || px >= width || py < 0.0 || py >= height ) {
			return false;
		}

		rasterPoint = Point2( px, py );
		return true;
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
		// We = 1 / (d(omega)/d(pixel) * W * H)

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

		return 1.0 / (pixelSolidAngle * width * height);
	}

	static Scalar PdfDirectionFisheye(
		const FisheyeCamera& cam,
		const Ray& ray )
	{
		// PDF over solid angle for one pixel:
		// p(omega) = 1 / pixelSolidAngle

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

		return 1.0 / pixelSolidAngle;
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

		if( px < 0.0 || px >= width || py < 0.0 || py >= height ) {
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
