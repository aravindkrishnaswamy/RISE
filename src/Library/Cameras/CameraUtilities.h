//////////////////////////////////////////////////////////////////////
//
//  CameraUtilities.h - BDPT camera utility functions providing
//  inverse projection, importance, and PDF for all camera types.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CAMERA_UTILITIES_
#define CAMERA_UTILITIES_

#include "../Interfaces/ICamera.h"
#include "../Utilities/Ray.h"
#include "../Utilities/ISampler.h"

namespace RISE
{
	namespace BDPTCameraUtilities
	{
		/// Dedicated t==1 aperture stream for unbounded samplers (Sobol, Independent),
		/// placed above all light/eye/VCM-NEE walk streams (max VCM NEE <= 3121).
		/// Do NOT use with PSSMLTSampler (see APERTURE_CURRENT_STREAM).
		static const int kApertureSamplerStream = 3322;

		/// End of BDPT's shallow walk streams under PSSMLTSampler:
		/// 0 = light source, 1..15 = light bounces, 16..46 = eye bounces, 47 = strategy select.
		/// Deeper PSSMLT bounces use 2049..4050 (above kPSSMLTFilmLensApertureStream).
		static const int kBdptShallowWalkStreamEndUnderPSSMLT = 48;

		/// Reserved MLT film/lens/wavelength/aperture stream under PSSMLTSampler
		/// (sits between shallow walk 0..47 and deep lanes 2049..4050; modulus = 4096).
		static const int kPSSMLTFilmLensApertureStream = 2048;

		/// Stream selection policy for DrawApertureSample().
		enum ApertureStreamPolicy
		{
			/// Calls StartStream(kApertureSamplerStream); use for Sobol / Independent samplers.
			APERTURE_DEDICATED_STREAM,

			/// Draws from the active stream; required for PSSMLTSampler (multiplexes lanes
			/// mod kDefaultNumStreams = 4096 on kPSSMLTFilmLensApertureStream).
			APERTURE_CURRENT_STREAM
		};

		/// Convention-agnostic 1-pixel guard band around [0, width) x [0, height);
		/// camera projections clip here while the splat film decides exact pixel membership.
		inline bool InRasterGuardBand(
			const Scalar px, const Scalar py,
			const Scalar width, const Scalar height )
		{
			return px >= Scalar( -1 ) && px < width + Scalar( 1 ) &&
			       py >= Scalar( -1 ) && py < height + Scalar( 1 );
		}

		/// Projects a 3D world point to raster coordinates; returns false if behind
		/// the camera or outside InRasterGuardBand().
		bool Rasterize(
			const ICamera& cam,					///< [in] Camera to project through
			const Point3& worldPoint,				///< [in] 3D world point to project
			Point2& rasterPoint						///< [out] Resulting raster coordinates
			);

		/// Camera importance for t==1 light tracing: `We * cos(theta) / p_A` already folded in,
		/// so callers evaluate `beta_light * f * G * Importance` with `G = |cos_light| / dist^2`.
		/// For thin-lens cameras, the ray MUST originate at a point drawn by SampleAperture().
		Scalar Importance(
			const ICamera& cam,					///< [in] Camera
			const Ray& ray							///< [in] Ray TOWARD the scene, originating at the camera vertex
			);

		/// Per-pixel directional PDF of generating `ray` from the camera.
		Scalar PdfDirection(
			const ICamera& cam,					///< [in] Camera
			const Ray& ray							///< [in] Ray from the camera
			);

		/// Whole-film directional PDF for BDPT/MLT MIS ratios: `PdfDirection / (W * H)`
		/// (Veach 9.2 / PBRT PdfWe; unscaled for orthographic cameras which have no t==1).
		/// Must be used at both eyeVerts[1].pdfFwd and t==1 pdfRev.
		Scalar PdfDirectionMIS(
			const ICamera& cam,					///< [in] Camera
			const Ray& ray							///< [in] Ray from the camera
			);

		/// Sampled point on the camera's entrance aperture for t==1 light tracing.
		struct ApertureSample
		{
			Point3	point;		///< world-space aperture point (GetLocation() if delta)
			Point3	local;		///< camera-local aperture coords (z == 0; valid iff isFinite)
			bool	isFinite;	///< true iff drawn from a non-zero aperture area

			ApertureSample() : point( 0, 0, 0 ), local( 0, 0, 0 ), isFinite( false ) {}
		};

		/// Samples an entrance-aperture point matching primary-ray shape/density (`1/p_A` is
		/// already folded into Importance()). Point-aperture cameras ignore `uv` (isFinite = false).
		ApertureSample SampleAperture(
			const ICamera& cam,					///< [in] Camera
			const Point2& uv						///< [in] Two canonical randoms in [0,1)
			);

		/// Projects `worldPoint` through aperture point `ap` (gives t==1 splats matching DoF).
		/// Returns false if behind camera or outside InRasterGuardBand().
		bool RasterizeThrough(
			const ICamera& cam,					///< [in] Camera
			const Point3& worldPoint,				///< [in] 3D world point to project
			const ApertureSample& ap,				///< [in] Aperture point from SampleAperture
			Point2& rasterPoint						///< [out] Resulting raster coordinates
			);

		/// Tangent-plane 1-pixel surface displacements (`dpdx`, `dpdy`) as imaged through `ap`,
		/// computed by inverting the 2x2 central-difference Jacobian of RasterizeThrough()
		/// (Igehy-equivalent footprint for t==1 texture filtering). False if unprojected or singular.
		bool PixelFootprintThrough(
			const ICamera& cam,					///< [in] Camera
			const ApertureSample& ap,				///< [in] Aperture point the point is imaged through
			const Point3& worldPoint,				///< [in] Surface point
			const Vector3& normal,					///< [in] Surface (shading) normal there
			Vector3& dpdx,							///< [out] Surface step for +1 raster x
			Vector3& dpdy							///< [out] Surface step for +1 raster y
			);

		/// True if the camera samples a finite-area entrance aperture (e.g. thin-lens).
		/// Note: BDPT/VCM check `HasFiniteAperture || IsDeltaDirection` for eyeVerts[0].position
		/// since orthographic rays vary origin across the film plane.
		bool HasFiniteAperture(
			const ICamera& cam						///< [in] Camera
			);

		/// Draws two canonical randoms for SampleAperture(), or returns (0,0) WITHOUT consuming
		/// sampler dimensions when HasFiniteAperture(cam) is false (preserves pinhole chains).
		Point2 DrawApertureSample(
			const ICamera& cam,					///< [in] Camera
			ISampler& sampler,						///< [in,out] Sampler to draw from
			ApertureStreamPolicy policy			///< [in] Which stream to draw on
			);

		/// True if camera importance is a Dirac delta in direction (orthographic);
		/// BDPT/VCM skip t==1 light tracing and omit it from MIS denominators.
		bool IsDeltaDirection(
			const ICamera& cam						///< [in] Camera
			);

		/// Option A near-clip helpers (dispatched via CameraCommon without growing ICamera vtable):
		/// - ComputeNearClipDistance: distance along unit `rayDir` to near-clip plane/sphere (0 if off).
		/// - IsPointPastNearClip: true iff `worldPoint` is at/beyond the near-clip boundary.
		/// - ApplyNearClipToConnectionEndpoint: advances `camPos` along `dirFromCam` to near-clip
		///   so t==1 visibility skips clipped geometry while keeping dist/We/G unclipped.
		Scalar ComputeNearClipDistance(
			const ICamera& cam,
			const Vector3& rayDir
			);

		bool IsPointPastNearClip(
			const ICamera& cam,
			const Point3& worldPoint
			);

		Point3 ApplyNearClipToConnectionEndpoint(
			const ICamera& cam,
			const Point3& camPos,
			const Vector3& dirFromCam
			);
	}
}

#endif
