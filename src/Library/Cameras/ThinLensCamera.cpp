//////////////////////////////////////////////////////////////////////
//
//  ThinLensCamera.cpp - Implementation of the thin lens camera
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 8, 2002
//  Tabs: 4
//  Comments:  Taken from ggLibrary
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ThinLensCamera.h"
#include "../Utilities/GeometricUtilities.h"
#include "CameraTransforms.h"
#include "../Animation/KeyframableHelper.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

void ThinLensCamera::Recompute( const unsigned int width, const unsigned int height )
{
	// For a non-distorting, regular (non-fisheye) lens, the angle of view is given by:
	// Angle of View = 2 * ArcTan(Film Dimension / (2 * Focal Length * (1 + Magnification)))
	// http://www.colloquial.com/photo/formats.html
	// Thus, to compute focal length from FOV:
	// Focal Length = Film Dimension / (2 * tan( Angle of View / 2 ) )
	// Also, relationship between F-Stop and aperture size:
	// F-Stop = Focal Length / Aperture Diameter, thus:
	// Aperture Diameter = Focal Length / F-Stop

	halfAperture = 0.5*aperture;

	dx = -0.5 * Scalar(width);
	dy = -0.5 * Scalar(height);

	const Scalar filmDistance = focusDistance * focalLength / (focusDistance - focalLength);
	const Scalar f_over_d_minus_f = focalLength / (filmDistance - focalLength);

	const Scalar sy = - 2.0 * filmDistance * tan(fov / 2.0) / Scalar(height);
	const Scalar sx = -sy;
	f_over_d_minus_f_sx = f_over_d_minus_f * sx;
	f_over_d_minus_f_sy = f_over_d_minus_f * sy;
	f_over_d_minus_f_d = f_over_d_minus_f * filmDistance;

	// Construct an OrthoNormalBasis that lines up with the viewing co-ordinates
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

	mxTrans = frame.GetTransformationMatrix( ) * ComputeScaleFromAR( );
}

ThinLensCamera::ThinLensCamera(
	const Point3& vPosition_,
	const Point3& vLookAt_,
	const Vector3& vUp_,
	const Scalar fov_,
	const unsigned int width,
	const unsigned int height,
	const Scalar pixelAR_,				///< [in] Pixel aspect ratio
	const Scalar exposure_,				///< [in] Exposure time of the camera
	const Scalar scanningRate,			///< [in] Scanning rate of the camera
	const Scalar pixelRate,				///< [in] Pixel rate of the camera
	const Vector3& orientation_,		///< [in] Orientation (Pitch,Roll,Yaw)
	const Vector2& target_orientation_,	///< [in] Orientation relative to a target
	const Scalar dAperture_,
	const Scalar focalLength_,
	const Scalar focusDistance_
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
  fov( fov_ ),
  aperture( dAperture_ ),
  halfAperture( 0.5*dAperture_ ),
  focalLength( focalLength_ ),
  focusDistance( focusDistance_ ),
  filmWidth( 35 ),
  filmHeight( 35 )
{
	Recompute( width, height );
}

ThinLensCamera::~ThinLensCamera( )
{
}

bool ThinLensCamera::GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const
{
	const Point2 xy = GeometricUtilities::PointOnDisk( halfAperture, Point2( rc.random.CanonicalRandom(), rc.random.CanonicalRandom() ) );

	const Point3		ptOnLens( xy.x, xy.y, 0.0 );

	const Scalar x = ptOnScreen.x + dx;
	const Scalar y = ptOnScreen.y + dy;

	const Point3		focus(  -x *  f_over_d_minus_f_sx,
								-y *  f_over_d_minus_f_sy,
								f_over_d_minus_f_d );

	r.Set( 
		Point3Ops::Transform(mxTrans,ptOnLens),
		Vector3Ops::Normalize(Vector3Ops::Transform(mxTrans, Vector3Ops::mkVector3(focus, ptOnLens))) );
	return true;
}

Matrix4 ThinLensCamera::ComputeScaleFromAR( ) const
{
	return Matrix4Ops::Stretch( Vector3( pixelAR, 1.0, 1.0 ) );
}

static const unsigned int FOV_ID = 100;
static const unsigned int APERTURE_ID = 101;
static const unsigned int FOCAL_ID = 102;
static const unsigned int FOCUS_ID = 103;

IKeyframeParameter* ThinLensCamera::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = CameraCommon::KeyframeFromParameters( name, value );

	if( !p ) {
		// Check the name and see if its something we recognize
		if( name == "fov" ) {
			p = new Parameter<Scalar>( value.toDouble(), FOV_ID );
		} else if( name == "aperture" ) {
			p = new Parameter<Scalar>( value.toDouble(), APERTURE_ID );
		} else if( name == "focal" ) {
			p = new Parameter<Scalar>( value.toDouble(), FOCAL_ID );
		} else if( name == "focus" ) {
			p = new Parameter<Scalar>( value.toDouble(), FOCUS_ID );
		} else {
			return 0;
		}

		GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	}

	return p;
}

void ThinLensCamera::SetIntermediateValue( const IKeyframeParameter& val )
{
	CameraCommon::SetIntermediateValue( val );

	switch( val.getID() )
	{
	case FOV_ID:
		{
			fov = *(Scalar*)val.getValue() * DEG_TO_RAD;
		}
		break;
	case APERTURE_ID:
		{
			aperture = *(Scalar*)val.getValue();
		}
		break;
	case FOCAL_ID:
		{
			focalLength = *(Scalar*)val.getValue();
		}
		break;
	case FOCUS_ID:
		{
			focusDistance = *(Scalar*)val.getValue();
		}
		break;
	}
}


