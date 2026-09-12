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
		/// Sampler stream reserved for the t==1 camera-aperture sample
		/// (debt 28) -- for DIMENSION-PADDED samplers only (Sobol,
		/// Independent).  NOT usable with `PSSMLTSampler`: see the
		/// `APERTURE_CURRENT_STREAM` policy below and
		/// `DrawApertureSample`'s contract.
		///
		/// The walks are NOT confined to streams 0..48.  Their true
		/// ranges, read off the code rather than off an old comment:
		///
		///   0                                    light source sampling
		///   1 .. 1+maxLightDepth+maxVolumeBounce  light-subpath bounces
		///   16 .. 16+maxEyeDepth+maxVolumeBounce  eye-subpath bounces
		///   31 .. 46                              SMS
		///   47                                    BDPT strategy select
		///   48                                    MLT film/lens/aperture
		///   48 + i                                VCM per-eye-vertex NEE
		///
		/// Both walk loops saturate their iteration count at 1024
		/// (`GenerateEyeSubpath` / `GenerateLightSubpath`), and an
		/// iteration appends at most three vertices (medium entry,
		/// medium scatter, surface), so the largest stream index any
		/// consumer can reach is bounded by
		///     48 + (3 * 1024 + 1)  =  3121.
		/// With `max_volume_bounce` at its 64 default and a typical
		/// depth of 20 the real maximum is ~116; 8192 clears the
		/// worst case by 2.6x and costs nothing, because
		/// `SobolSequence::Sample` is PADDED (a per-dimension hash of
		/// dimensions 0/1) and therefore has no dimension capacity to
		/// exhaust.  `SobolSampler::StartStream` maps this to
		/// dimension 8192*32 = 262144, which is just another hash seed.
		///
		/// Drawing the aperture point from a dedicated stream keeps it
		/// stratified across pixels under Sobol and, under PSSMLT,
		/// makes a small mutation move the aperture point continuously
		/// instead of teleporting it (the same property
		/// `GenerateRayWithLensSample` exists to give the primary ray).
		static const int kApertureSamplerStream = 8192;

		/// Where `DrawApertureSample` takes its two canonical randoms.
		enum ApertureStreamPolicy
		{
			/// `StartStream( kApertureSamplerStream )` first.  Correct
			/// for `SobolSampler` / `IndependentSampler`, whose streams
			/// are unbounded (Sobol pads by hashing the dimension
			/// index, Independent ignores the stream entirely).
			APERTURE_DEDICATED_STREAM,

			/// Draw from whatever stream is already active.  This is
			/// the ONLY correct policy for `PSSMLTSampler`, which
			/// multiplexes lanes as `idx = stream + kNumStreams*sample`
			/// with `kNumStreams == 49`: a stream index >= 49 does not
			/// get a fresh lane, it ALIASES an existing one (stream 80
			/// is stream 31's sample 1, stream 129 its sample 2).  The
			/// MLT rasterizers therefore draw the aperture point as a
			/// third `Get2D` on their own stream 48, contiguous with
			/// the film and lens samples -- lanes 244/293 against the
			/// film's 48/97 and the lens's 146/195.  Lanes of streams
			/// 0..48 are partitioned by residue mod 49, so a stream-48
			/// lane can never collide with an integrator stream.
			APERTURE_CURRENT_STREAM
		};

		/// Maps a 3D world point to raster coordinates [0,width) x [0,height)
		/// \return FALSE if the point is behind the camera or outside the image
		bool Rasterize(
			const ICamera& cam,					///< [in] Camera to project through
			const Point3& worldPoint,				///< [in] 3D world point to project
			Point2& rasterPoint						///< [out] Resulting raster coordinates
			);

		/// Camera importance for the t==1 light-tracing connection.
		///
		/// CONTRACT (debt 28): this is NOT the bare PBRT `We`.  It is
		/// `We * cos(theta) / p_A(camera vertex)` -- the aperture
		/// cosine and the reciprocal of the aperture-sampling density
		/// are already folded in -- so that every t==1 site can write
		///     contribution = beta_light * f * G * Importance
		/// with G = |cos at the light vertex| / dist^2 and no further
		/// camera-side factor, for every camera type.
		///
		/// For a delta-position camera (pinhole / fisheye) the camera
		/// vertex is `GetLocation()` and `p_A` is PBRT-v4's unit-area
		/// fiction (`lensArea = 1` when `lensRadius == 0`), so the fold
		/// is just the cosine -- which is why the pinhole form carries
		/// cos^3 where PBRT's raw `We` carries cos^4.  For a thin lens
		/// the caller MUST have connected to a point drawn by
		/// `SampleAperture` below, whose density is uniform 1/A_lens;
		/// A_lens then cancels against the 1/A_lens in the thin-lens
		/// `We` and the result is the pinhole form again.  Connecting
		/// to the lens CENTRE and using this value anyway is the debt-28
		/// defect in reverse: it was the 1/A_lens, uncancelled, that
		/// inflated BDPT/VCM splats by up to 4e5x.
		///
		/// \return The importance value for this ray direction
		Scalar Importance(
			const ICamera& cam,					///< [in] Camera
			const Ray& ray							///< [in] Ray TOWARD the scene, originating at the camera vertex
			);

		/// PDF of generating this ray direction from the camera
		/// \return The probability density
		Scalar PdfDirection(
			const ICamera& cam,					///< [in] Camera
			const Ray& ray							///< [in] Ray from the camera
			);

		/// A point on the camera's entrance aperture, sampled for a
		/// t==1 light-tracing connection.
		struct ApertureSample
		{
			/// WORLD-space point the light vertex connects to.  For a
			/// delta-position camera this is `cam.GetLocation()`.
			Point3	point;
			/// The same point in the camera's LOCAL aperture
			/// coordinates (z == 0).  Meaningful only when `isFinite`;
			/// `RasterizeThrough` consumes it.
			Point3	local;
			/// The aperture has area and `local` was drawn from it with
			/// density 1 / (that area).  FALSE for pinhole / fisheye /
			/// orthographic cameras and for a degenerate thin-lens
			/// aperture, all of which collapse to the camera location.
			bool	isFinite;

			ApertureSample() : point( 0, 0, 0 ), local( 0, 0, 0 ), isFinite( false ) {}
		};

		/// Sample a point on the camera's entrance aperture, with the
		/// SAME shape and density the camera's primary rays use.
		///
		/// `uv` are two canonical randoms; they are ignored by cameras
		/// whose aperture is a point, which return the camera location
		/// with `isFinite == false`.  The returned point's density does
		/// not appear in the caller's estimator: `Importance` above has
		/// already folded `1 / p_A` in.
		ApertureSample SampleAperture(
			const ICamera& cam,					///< [in] Camera
			const Point2& uv						///< [in] Two canonical randoms in [0,1)
			);

		/// Raster position of `worldPoint` as imaged THROUGH `ap`.
		///
		/// For a finite aperture this is what gives a light-traced
		/// contribution the depth of field the eye rays have: the world
		/// point is imaged through the SAMPLED lens point, so an
		/// off-focus point lands a circle-of-confusion away from where
		/// the lens centre would put it, and an on-focus point lands at
		/// the same pixel for every lens sample.  For a delta-position
		/// aperture it is exactly `Rasterize` above.
		/// \return FALSE if the point is behind the camera or off-film
		bool RasterizeThrough(
			const ICamera& cam,					///< [in] Camera
			const Point3& worldPoint,				///< [in] 3D world point to project
			const ApertureSample& ap,				///< [in] Aperture point from SampleAperture
			Point2& rasterPoint						///< [out] Resulting raster coordinates
			);

		/// True if the camera emits importance from a region of
		/// non-zero AREA (a thin lens with a real aperture), as opposed
		/// to a single point.  Bidirectional integrators use this to
		/// decide whether the camera path vertex sits at the sampled
		/// ray origin (finite aperture) or at `GetLocation()`.
		///
		/// It does NOT change how MIS treats the camera vertex: the
		/// aperture-positional density is the same under every
		/// strategy -- each samples the camera vertex from the same
		/// aperture with the same density -- so it cancels out of every
		/// pdf ratio, exactly as PBRT-v4's `MISWeight` never reads
		/// `cameraVertices[0].pdfFwd`.
		bool HasFiniteAperture(
			const ICamera& cam						///< [in] Camera
			);

		/// The two canonical randoms a t==1 site feeds to
		/// `SampleAperture`, or (0,0) CONSUMING NOTHING when the
		/// camera's aperture is a point.
		///
		/// The "consuming nothing" half is load-bearing, not an
		/// optimisation.  Every integrator that supports light tracing
		/// calls this once per sample, pinhole scenes included; if it
		/// drew unconditionally, every pinhole MLT render would walk a
		/// different Markov chain than before debt 28 (two extra
		/// primary-sample dimensions per `EvaluateSample`) and every
		/// pinhole Sobol render would burn a dimension it does not use.
		/// A pinhole / fisheye / orthographic camera ignores `uv`
		/// entirely (`SampleAperture` returns `GetLocation()`), so the
		/// draw would be pure chain perturbation.
		/// \return Two canonical randoms in [0,1)^2, or (0,0)
		Point2 DrawApertureSample(
			const ICamera& cam,					///< [in] Camera
			ISampler& sampler,						///< [in,out] Sampler to draw from
			ApertureStreamPolicy policy			///< [in] Which stream to draw on
			);

		/// True if the camera's importance is a Dirac delta in DIRECTION
		/// (all rays parallel — orthographic).  Such a camera is the
		/// importance-side analogue of a directional light: the t==1
		/// light-tracing strategy (a non-specular light vertex scattering
		/// into the camera) has zero density, so BDPT/VCM must SKIP that
		/// strategy and exclude it from the MIS denominator — exactly the
		/// delta-direction-light treatment, applied on the camera side.
		/// Pinhole / thin-lens / fisheye are delta-POSITION (or finite)
		/// but finite-direction, so the t==1 connection is valid for them
		/// and this returns false.
		/// \return TRUE for orthographic cameras, FALSE otherwise
		bool IsDeltaDirection(
			const ICamera& cam						///< [in] Camera
			);
	}
}

#endif
