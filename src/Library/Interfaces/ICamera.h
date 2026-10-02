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
	//! DL-368: THE pixel <-> screen convention, shared by every rasterizer
	//! (eye-ray sample placement), every film that reconstructs a sample at
	//! a fractional position (FilteredFilm::Splat, SplatFilm::SplatFiltered
	//! and SplatFilm::NearestPixel) and every camera (whose film mapping
	//! translates screen space by -W/2, -H/2).  Screen space is the
	//! camera's nominal film: x in [0, W), y in [0, H) with y UP.  Pixel
	//! (x, image row `row`, row 0 at the TOP) covers screen
	//! [x, x+1) x [H-1-row, H-row), so its centre is (x + 0.5, H - row - 0.5)
	//! -- PBRT's convention.  In FILM coordinates (fx, fy = H - screen y,
	//! rows from the top) the same pixel covers [x, x+1) x (row, row+1]
	//! and its centre is (x + 0.5, row + 0.5).
	//!
	//! Before DL-368 the rasterizers placed pixel (x, row) at
	//! (x + u - 0.5, H - row + v - 0.5) while the cameras' film was
	//! [0, W) x [0, H): every image sat half a pixel off its own camera.
	namespace RasterConvention
	{
		//! A pixel centre's offset from its left / lower edge.
		static const Scalar kPixelCentre = 0.5;

		//! Screen position of the sub-pixel sample (u, v) in [0,1)^2 of
		//! pixel (x, row) on a film `height` rows tall.
		inline Point2 PixelToScreen( const unsigned int x, const unsigned int row,
			const unsigned int height, const Scalar u, const Scalar v )
		{
			return Point2( static_cast<Scalar>( x ) + u,
				static_cast<Scalar>( height ) - static_cast<Scalar>( row ) - Scalar( 1 ) + v );
		}

		//! Screen position of the centre of pixel (x, row).
		inline Point2 PixelCentreToScreen( const unsigned int x, const unsigned int row,
			const unsigned int height )
		{
			return PixelToScreen( x, row, height, kPixelCentre, kPixelCentre );
		}
	}

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
		//! Generate the ray through screen point @a ptOnScreen (the
		//! camera's nominal film, x in [0, W), y in [0, H), y up -- see
		//! RasterConvention above for where a pixel sits on it).
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

		// Note: GetWidth() / GetHeight() were removed from ICamera in
		// the 2026-05 Camera/Film/Output refactor.  Resolution is now
		// a property of IFilm (the scene-level pixel-grid descriptor),
		// not of the camera.  External callers must query the scene's
		// active film: `pScene->GetFilm()->GetWidth()`.  Internal
		// camera math still caches the dimensions in CameraCommon's
		// `frame` member (populated at construction time from the
		// active film); CameraCommon retains non-virtual GetWidth() /
		// GetHeight() member functions for use by the camera-utility
		// helpers, but they are NOT part of the ICamera contract.

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
