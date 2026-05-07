//////////////////////////////////////////////////////////////////////
//
//  PinholeCamera.h - Declaration of a pinhole camera, ie. a 
//  traditional perspective camera
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PINHOLE_CAMERA_
#define PINHOLE_CAMERA_

#include "CameraCommon.h"

namespace RISE
{
	namespace Implementation
	{
		class PinholeCamera :
			public virtual CameraCommon
		{
		protected:
			virtual ~PinholeCamera( );

			Scalar				fov;				/// Field of view in radians (entire FOV) of this camera

			// Landing 5: photographic exposure metadata.  Pinhole has
			// no geometric DOF, so fstop only drives the EV stack;
			// iso == 0 means "physical exposure disabled, behave
			// exactly like pre-L5".  When iso > 0, evCompensation is
			// computed at construction from (iso, fstop, exposureTime)
			// per the UE5 / Filament saturation-based formula and
			// returned by GetExposureCompensationEV() for stacking
			// into FileRasterizerOutput on LDR outputs.
			Scalar				iso_;				/// ISO sensitivity (0 = disabled)
			Scalar				fstop_;				/// f-number (used for EV only on pinhole; no geometric effect)
			Scalar				evCompensation_;	/// Pre-computed exposure compensation in EV stops

			Matrix4	ComputeScaleFromFOV( ) const;

			//! Recomputes camera parameters from class values
			void Recompute( const unsigned int width, const unsigned int height );

		public:
			//! Sets the camera based on two basis vectors and position
			PinholeCamera(
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const Point3& vLookAt,				///< [in] A point in the world camera is looking at
				const Vector3& vUp,					///< [in] What is considered up in the world
				const Scalar fov_,					///< [in] Entire field of view
				const unsigned int width,			///< [in] Width of the frame of the camera
				const unsigned int height,			///< [in] Height of the frame of the camera
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Vector3& orientation,			///< [in] Orientation (Pitch,Roll,Yaw)
				const Vector2& target_orientation,	///< [in] Orientation relative to a target
				const Scalar iso = Scalar( 0 ),		///< [in] Landing 5: ISO sensitivity.  Default 0 = physical exposure disabled (pre-L5 behaviour preserved).  When > 0, fstop and exposure must also be > 0.
				const Scalar fstop = Scalar( 0 )	///< [in] Landing 5: f-number for EV computation.  Required when iso > 0; ignored otherwise.  Pinhole has no geometric DOF, so fstop only drives the exposure stack.
				);

			//! Sets the camera based on a given basis and the position
			PinholeCamera(
				const OrthonormalBasis3D& basis,	///< [in] Basis from which to derive basis vectors
				const Point3& vPosition,			///< [in] Location of the camera in the world
				const Scalar fov,					///< [in] Entire field of view
				const unsigned int width,			///< [in] Width of the frame of the camera
				const unsigned int height,			///< [in] Height of the frame of the camera
				const Scalar pixelAR,				///< [in] Pixel aspect ratio
				const Scalar exposure,				///< [in] Exposure time of the camera
				const Scalar scanningRate,			///< [in] Scanning rate of the camera
				const Scalar pixelRate,				///< [in] Pixel rate of the camera
				const Scalar iso = Scalar( 0 ),		///< [in] Landing 5: ISO sensitivity.  Default 0 = physical exposure disabled.
				const Scalar fstop = Scalar( 0 )	///< [in] Landing 5: f-number for EV computation.  Required when iso > 0.
				);

			bool GenerateRay( const RuntimeContext& rc, Ray& r, const Point2& ptOnScreen ) const;

			//! Landing 5: returns the pre-computed photographic exposure
			//! compensation in EV stops.  See ICamera.h for the formula
			//! and stacking semantics.  Returns 0 when iso == 0
			//! (physical exposure disabled).
			Scalar GetExposureCompensationEV() const override { return evCompensation_; }

			// Mutation surface used by the descriptor-driven properties
			// panel.  Caller invokes RegenerateData() after edits.
			inline Scalar GetFovStored() const         { return fov; }
			inline void   SetFovStored( Scalar v )     { fov = v; }

			// Landing 5: getters/setters for the photographic-exposure
			// pair (iso, fstop).  Pinhole has no geometric DOF, so
			// fstop here is solely an EV input — naming kept consistent
			// with the parser key `fstop` and with ThinLensCamera's
			// surface (where fstop drives BOTH DOF and EV).  Setters
			// only write the field; the cached evCompensation_ is
			// rebuilt by RegenerateData() (the standard batch-then-
			// regenerate contract from CameraCommon).
			inline Scalar GetIsoStored() const         { return iso_; }
			inline void   SetIsoStored( Scalar v )     { iso_ = v; }
			inline Scalar GetFstop() const             { return fstop_; }
			inline void   SetFstop( Scalar v )         { fstop_ = v; }

			//! Landing 5: also rebuild the photographic exposure cache
			//! (evCompensation_) from the current (iso_, fstop_,
			//! exposureTime).  CameraCommon's mutator contract is "edit
			//! the source-of-truth field, then call RegenerateData()
			//! once" — without this override, edits to `exposure` or
			//! `fstop` (motion blur / DOF parameters that ALSO feed the
			//! EV equation) would update the geometric state but leave
			//! brightness frozen at the constructor-time EV.  Live-
			//! editor and keyframed shutter changes would silently
			//! diverge from the photographic settings.
			void RegenerateData() override;

			// For keyframing the FOV
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value );
			void SetIntermediateValue( const IKeyframeParameter& val );
		};
	}
}

#endif
