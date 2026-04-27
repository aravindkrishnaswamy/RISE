//////////////////////////////////////////////////////////////////////
//
//  CameraCommon.h - Common stuff between cameras (mostly keyframing)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 28, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAMERA_COMMON_
#define CAMERA_COMMON_

#include "../Interfaces/ICamera.h"
#include "../Utilities/Reference.h"
#include "Frame.h"

namespace RISE
{
	namespace Implementation
	{
		class CameraCommon : 
			public virtual ICamera, 
			public virtual Reference
		{
		protected:
			Point3				vPosition;			/// Location of the camera in the world
			Point3				vLookAt;			/// A point in the world camera is looking at
			Vector3				vUp;				/// What is considered up in the world
			const Scalar		pixelAR;			/// Pixel aspect ratio
			Scalar				exposureTime;		/// Exposure time of the camera
			Scalar				scanningRate;		/// Rate at which the camera records each scanline (normalized unit time / scanline)
			Scalar				pixelRate;			/// Rate at which the camera records each pixel (normalized unit time / pixel)
			Frame				frame;				/// The frame of the camera
			Matrix4				mxTrans;			/// Transformation matrix of this camera
			Vector3				orientation;		/// Orientation (Pitch,Roll,Yaw)
			Vector2				target_orientation;	/// Orientation relative to a target
			const bool			from_onb;			/// Was this camera created with an ONB passed in explicitly?

			// Non ONB specifying version
			CameraCommon(
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const Point3& vLookAt,				///< [in] A point in the world camera is looking at
				const Vector3& vUp,					///< [in] What is considered up in the world
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Vector3& orientation,			///< [in] Orientation (Pitch,Roll,Yaw)
				const Vector2& target_orientation	///< [in] Orientation relative to a target
				);

			// ONB specifying version
			CameraCommon(
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate				///< [in] Pixel rate of the camera
				);

			virtual ~CameraCommon(){};

			//! Recomputes camera parameters from class values
			virtual void Recompute( const unsigned int width, const unsigned int height ) = 0;

		public:

			//
			// Mutation surface for the interactive scene editor.
			// Setters update the stored member; the caller is
			// responsible for invoking RegenerateData() afterwards
			// (typically by calling all the setters then a single
			// Regenerate) so the basis matrix is rebuilt exactly
			// once per logical edit.
			//
			inline void SetLocation( const Point3& p )    { vPosition = p; }
			inline Point3 GetStoredLookAt() const         { return vLookAt; }
			inline Vector3 GetStoredUp() const            { return vUp; }
			inline void SetLookAt( const Point3& p )      { vLookAt = p; }
			inline void SetUp( const Vector3& u )         { vUp = u; }

			//! Rest position — `vPosition` as stored, BEFORE the
			//! orbit-around-look-at rotation that Recompute applies via
			//! `target_orientation`.  This is the value the user sets
			//! in the .RISEscene file's `location` field.  Distinct
			//! from `GetLocation()` which returns the post-orbit
			//! `frame.GetOrigin()` — useful for the rasterizer (it
			//! wants the actual eye position) but misleading in the
			//! editor properties panel (an orbit shouldn't appear to
			//! mutate a parameter the user didn't touch).
			inline Point3 GetRestLocation() const         { return vPosition; }

			// Additional getters/setters used by the descriptor-driven
			// properties panel.  pixelAR is intentionally read-only —
			// it's stored as `const` and changing it would require
			// recreating the camera.
			inline Scalar  GetPixelAR()                   const { return pixelAR; }
			inline Scalar  GetExposureTimeStored()        const { return exposureTime; }
			inline Scalar  GetScanningRateStored()        const { return scanningRate; }
			inline Scalar  GetPixelRateStored()           const { return pixelRate; }
			inline Vector3 GetEulerOrientation()          const { return orientation; }
			inline Vector2 GetTargetOrientation()         const { return target_orientation; }
			inline void    SetExposureTimeStored( Scalar v )    { exposureTime = v; }
			inline void    SetScanningRateStored( Scalar v )    { scanningRate = v; }
			inline void    SetPixelRateStored( Scalar v )       { pixelRate = v; }
			inline void    SetEulerOrientation( const Vector3& v )  { orientation = v; }
			inline void    SetTargetOrientation( const Vector2& v ) { target_orientation = v; }

			//! Update the camera's frame dimensions and rebuild the
			//! basis matrix.  Used by the interactive editor to render
			//! at a downsampled resolution during fast drags so each
			//! render pass completes within the 30Hz budget.
			//! Caller is responsible for restoring the original
			//! dimensions when the drag ends.
			inline void SetDimensions( unsigned int w, unsigned int h )
			{
				frame.SetDimensions( w, h );
				RegenerateData();
			}

			inline Point3 GetLocation( ) const
			{
				return frame.GetOrigin();
			}

			inline Matrix4 GetMatrix( ) const
			{ 
				return mxTrans;
			}

			inline unsigned int GetWidth( ) const
			{
				return frame.GetWidth();
			}

			inline unsigned int GetHeight( ) const
			{
				return frame.GetHeight();
			}

			inline Scalar GetExposureTime( ) const
			{
				return exposureTime;
			}

			inline Scalar GetScanningRate( ) const
			{
				return scanningRate;
			}

			inline Scalar GetPixelRate( ) const
			{
				return pixelRate;
			}

			// For keyframamble interface
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
			void RegenerateData( );
		};
	}
}

#endif

