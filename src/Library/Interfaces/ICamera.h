//////////////////////////////////////////////////////////////////////
//
//  ICamera.h - Declaration of abstract camera class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ICAMERA_
#define ICAMERA_

#include "IReference.h"
#include "IKeyframable.h"
#include "../Utilities/Ray.h"
#include "../Utilities/RuntimeContext.h"

namespace RISE
{
	//! A Camera is the viewer is located in the scene.  It also is what
	//! generates rays from a virtual screen
	class ICamera : 
		public virtual IReference,
		public virtual IKeyframable
	{
	protected:
		ICamera( ){};
		virtual ~ICamera( ){};

	public:
		//! Given two Scalars x and y in [0..1], generate a ray to fire
		/// \return TRUE if a ray exists, FALSE otherwise
		virtual bool GenerateRay(
			const RuntimeContext& rc,					///< [in] Runtime context
			Ray& ray,									///< [in] The ray cast from point on screen
			const Point2& ptOnScreen					///< [in] Point on the virtual screen to generate for
			) const = 0;

		/// \return Point in space occupied by the camera
		virtual Point3 GetLocation( ) const = 0;

		/// \return Transformation matrix
		virtual Matrix4 GetMatrix( ) const = 0;

		/// \return The width of the frame of the camera
		virtual unsigned int GetWidth( ) const = 0;

		/// \return The height of the frame of the camera
		virtual unsigned int GetHeight( ) const = 0;

		/// \return The exposure time of the camera in some units (normalized unit time).  This is the same primary unit of animations
		virtual Scalar GetExposureTime( ) const = 0;

		/// \return The rate at which each scanline is 'recorded' by the camera in some units (normalized unit time/scanline)
		///         This is in the same primary units of animations
		///         If the scanning rate is infintely fast, returns 0
		virtual Scalar GetScanningRate( ) const = 0;

		/// \return The rate at which each pixel on a scanline is 'recorded' by the camera in some units (normalized unit time/pixel)
		///         This is in the same primary units of animations
		///         If the pixel rate is infintely fast, returns 0
		virtual Scalar GetPixelRate( ) const = 0;

		//! Landing 5: photographic exposure compensation in EV stops.
		//! When the camera is configured with physical photographic
		//! parameters (ISO, f-number, shutter time), this returns
		//!   evComp = -log2(1.2) - log2(N² × 100 / (ISO × T))
		//! per the UE5 / Filament convention where a 100% white
		//! reflector saturates at scene luminance L = 1.2 × 2^EV100
		//! cd/m² (matches ISO 12232 saturation-based standard).  The
		//! value stacks ADDITIVELY into FileRasterizerOutput's
		//! exposure_compensation parameter on LDR outputs (PNG / JPEG)
		//! — HDR archival outputs (EXR / RGBE) ignore it to preserve
		//! "linear radiance ground truth" semantics from Landing 1.
		//!
		//! Default 0 = no compensation, which is what cameras
		//! authored without photographic units return.  Existing
		//! scenes (every pinhole_camera / thinlens_camera that
		//! doesn't set `iso > 0`) keep this default and render
		//! pixel-identically to pre-L5 builds.
		//!
		//! Default implementation returns 0; cameras that opt in to
		//! photographic units override.
		virtual Scalar GetExposureCompensationEV() const { return Scalar( 0 ); }

		// NOTE on lens sampling: the ICamera vtable intentionally has
		// NO entry for "generate ray with an externally supplied lens
		// sample".  Adding one — even appended at the end — would
		// crash out-of-tree camera objects compiled against the old
		// interface the moment a new caller dispatched through the
		// missing slot.  Lens-sample injection for MLT is therefore
		// done via a non-virtual helper
		// (MLTRasterizer::GenerateCameraRayWithLensSample) that
		// dynamic_casts to ThinLensCamera at the call site and
		// falls back to GenerateRay for every other camera type.
		// Concrete ThinLensCamera exposes a non-virtual
		// GenerateRayWithLensSample method used only by that helper.
	};
}

#endif
