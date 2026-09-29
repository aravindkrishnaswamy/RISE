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
		/// (debt 28) -- for samplers whose streams are unbounded (Sobol,
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
		///   31 .. 46                              SMS (reserved; no
		///                                          `StartStream` call
		///                                          in this range today
		///                                          -- inherited from a
		///                                          stale SobolSampler.h
		///                                          comment, kept as a
		///                                          bound, not a claim
		///                                          of current use)
		///   47                                    BDPT strategy select
		///   kPSSMLTFilmLensApertureStream         MLT film/lens/aperture
		///                                          under PSSMLTSampler
		///                                          (DL-08 fix, 2026-09-17;
		///                                          was the literal 48)
		///   48 + i, i >= 1                        VCM per-eye-vertex NEE
		///                                          (i starts at 1; VCM
		///                                          never itself starts
		///                                          stream 48; VCM never
		///                                          drives a PSSMLTSampler
		///                                          either, so this range
		///                                          does not interact with
		///                                          `kPSSMLTFilmLensApertureStream`)
		///
		/// Both walk loops saturate their iteration count at 1024
		/// (`GenerateEyeSubpath` / `GenerateLightSubpath`), and an
		/// iteration appends at most three vertices (medium entry,
		/// medium scatter, surface), so the largest stream index any
		/// consumer can reach is bounded by
		///     48 + (3 * 1024 + 1)  =  3121.
		/// With `max_volume_bounce` at its 64 default and a typical
		/// depth of 20 the real maximum is ~116.
		///
		/// 3322 is NOT just "comfortably above 3121".  Being above the
		/// walk streams was the whole argument while
		/// `SobolSequence::Sample` was padded -- it hashed the dimension
		/// index, so any distinct stream was a distinct hash seed and
		/// the constant could be anything large (it was 8192).  DL-81
		/// gave the sampler a FINITE supply of real Sobol' dimensions,
		/// and 8192*32 = 262144 wrapped onto dimension 1001 -- stream 31
		/// slot 9, eye bounce 15, well inside a normal render -- so the
		/// aperture would have drawn the SAME dimension as that bounce.
		/// 3322 is the smallest stream above 3121 that placed the
		/// aperture clear of that.
		///
		/// Since DL-81 round 2 the wrap no longer enters the argument
		/// at all for THIS constant: `DrawApertureSample` draws with
		/// `Get2D`, and `Get2D` is a padded (0,2)-net pair -- Sobol'
		/// dimensions 0 and 1 at a sample index permuted per dimension
		/// group -- keyed by the RAW dimension index, 3322*32 = 106304.
		/// No table row is read, nothing is reduced modulo the table
		/// size, and no walk stream can key the same group because none
		/// reaches stream 3322.  What the constant still has to be is
		/// ABOVE every walk stream, which is what
		/// `SobolDimensionBudgetTest` Test F asserts; Test G1 separately
		/// asserts that no shipped scene drives a WALK stream past the
		/// end of the dimension table, which is where the wrap would
		/// still matter (for Get1D draws).
		///
		/// Drawing the aperture point from a dedicated stream keeps it
		/// stratified across pixels under Sobol and, under PSSMLT,
		/// makes a small mutation move the aperture point continuously
		/// instead of teleporting it (the same property
		/// `GenerateRayWithLensSample` exists to give the primary ray).
		static const int kApertureSamplerStream = 3322;

		/// Upper bound on any stream index BDPTIntegrator's own
		/// `StartStream` calls can reach WHILE DRIVEN BY A
		/// `PSSMLTSampler` (i.e. under MLT).  Unlike `kApertureSamplerStream`
		/// above (which has to clear Sobol's dimension-table wrap and
		/// therefore uses a conservative bound that also covers VCM's
		/// `48 + eye-vertex-index` NEE stream), VCM never drives a
		/// PSSMLTSampler at all, so the relevant bound here is BDPT's own
		/// two walks only:
		///   light walk:  `StartStream( 1u + depth )`,  depth < 1024 (the
		///                loop's saturating cap) -> max stream 1+1023 = 1024
		///   eye walk:    `StartStream( 16u + depth )`, depth < 1024      -> max stream 16+1023 = 1039
		///   BDPT (s,t) strategy select: fixed at stream 47
		///   SMS (reserved, unused today): streams 31..46
		/// giving a true maximum of 1039.  Written here as `16 +
		/// kWalkIterationCap` (1024, not 1023) for the same one-off
		/// margin `tests/SobolDimensionBudgetTest.cpp`'s
		/// `TestApertureDrawConsumption` uses when it derives
		/// `kMaxEyeWalkStream` the same way.
		///
		/// DL-283: still the true bound under PSSMLT.  The per-event
		/// medium distance-sampling blocks
		/// (`BDPTUtilities::MediumDistanceStream`, streams 8192+) are
		/// used only by fixed-budget samplers, never by PSSMLTSampler
		/// (SobolDimensionBudgetTest Test H drives the generators with
		/// one and asserts it).
		static const int kMaxBdptWalkStreamUnderPSSMLT = 16 + 1024;

		/// Stream reserved for the MLT film / lens / (debt 28) aperture
		/// block under `PSSMLTSampler` (DL-08 fix, 2026-09-17).  Used by
		/// `MLTRasterizer::EvaluateSample` and
		/// `MLTSpectralRasterizer::EvaluateSampleSpectral` in place of the
		/// historical literal `48`.
		///
		/// Debt 29 / DL-08: `PSSMLTSampler` multiplexes lanes as
		/// `idx = stream + kNumStreams*sample`.  Stream 48 was NOT a safe
		/// choice for this block: BDPT's eye walk reaches stream 48
		/// itself at eye depth 32 (`StabilityConfig::maxVolumeBounce`
		/// defaults to 64, so ordinary scattering-medium scenes reach that
		/// depth with no unusual settings), and a chain whose accepted
		/// path is that deep had its 32nd-bounce scattering direction and
		/// its film position living in the literal same primary-sample
		/// slot -- not modular aliasing, an outright integer collision.
		/// Chosen comfortably above `kMaxBdptWalkStreamUnderPSSMLT` (1039)
		/// so it can never collide with either walk at any depth
		/// `PSSMLTSampler`'s own loop caps allow, with margin for future
		/// per-stream lanes (e.g. a wider spectral wavelength count) to be
		/// added without re-deriving this constant.  See
		/// `tests/PSSMLTStreamAliasingTest.cpp` Test F for the red-proof
		/// and `PSSMLTSampler::kDefaultNumStreams` (2048 -> 4096, DL-08)
		/// for the paired modulus increase that keeps this a private lane.
		static const int kPSSMLTFilmLensApertureStream = 2048;

		/// Where `DrawApertureSample` takes its two canonical randoms.
		enum ApertureStreamPolicy
		{
			/// `StartStream( kApertureSamplerStream )` first.  Correct
			/// for `SobolSampler` / `IndependentSampler`, whose streams
			/// are unbounded (Sobol's Get2D is padded and keyed by the
			/// raw dimension index, so a large stream is simply its own
			/// group -- see `kApertureSamplerStream` -- and Independent
			/// ignores the stream entirely).
			APERTURE_DEDICATED_STREAM,

			/// Draw from whatever stream is already active.  This is
			/// the ONLY correct policy for `PSSMLTSampler`, which
			/// multiplexes lanes as `idx = stream + kNumStreams*sample`
			/// with `kNumStreams == PSSMLTSampler::kDefaultNumStreams`
			/// (4096, DL-08): a stream index >= kNumStreams does not
			/// get a fresh lane, it ALIASES an existing one.  The MLT
			/// rasterizers therefore draw the aperture point as a
			/// further `Get2D` on their own reserved stream
			/// (`kPSSMLTFilmLensApertureStream`, 2048 -- NOT the literal
			/// 48 this comment described before DL-08; see that
			/// constant's own doc for why 48 was unsafe), contiguous
			/// with the film and lens samples -- but the exact lanes
			/// depend on which MLT rasterizer is asking:
			///   `MLTRasterizer` (RGB): film Get2D + lens Get2D leave
			///     4 lanes consumed, so the aperture Get2D lands at
			///     sample indices 4 and 5 on the reserved stream.
			///   `MLTSpectralRasterizer::EvaluateSampleSpectral`:
			///     film + lens are the same 4 lanes, but it THEN
			///     pre-consumes `nSpectralSamples` (S) wavelength
			///     `Get1D`s from the reserved stream before drawing the
			///     aperture point, so the aperture lanes shift to
			///     sample indices `4+S` and `5+S` (8 and 9 at the
			///     default S=4).
			/// The safety argument is not "the lane depth stays below
			/// some fixed number" -- it is that lanes on streams
			/// `0..kNumStreams-1` are partitioned by RESIDUE mod
			/// `kNumStreams`, and every draw on the reserved stream
			/// (film, lens, wavelengths, aperture, at any sample index)
			/// keeps that stream's residue, which no OTHER stream <
			/// kNumStreams can ever produce.
			///
			/// That residue argument only protects streams that STAY
			/// below `kNumStreams`.  `kPSSMLTFilmLensApertureStream`
			/// (2048) is chosen strictly above
			/// `kMaxBdptWalkStreamUnderPSSMLT` (1039, BDPT's own
			/// documented walk-stream ceiling under PSSMLT) specifically
			/// so the eye walk's `StartStream( 16u + depth )` can never
			/// reach it at ANY depth PSSMLTSampler's own loop caps
			/// allow -- unlike the historical literal 48, which the eye
			/// walk reached at eye depth 32
			/// (`StabilityConfig::maxVolumeBounce` defaults to 64, so
			/// ordinary scattering-medium scenes reached it with no
			/// unusual settings).  This was debt 29 / DL-08 in
			/// docs/RENDERING_INTEGRATORS.md §7 and
			/// docs/DEBT_LEDGER.md; CLOSED 2026-09-17, see
			/// docs/DL08_PSSMLT_LANE_LAYOUT.md.
			APERTURE_CURRENT_STREAM
		};

		/// DL-294: the raster guard band.  The camera-side projections
		/// (`Rasterize`, `RasterizeThrough`, `ThinLensCamera::
		/// RasterFromLensPoint`) do NOT decide which raster points are on
		/// the film -- the FILM does, in the same pixel convention the
		/// eye subpaths are sampled in (every rasterizer draws pixel
		/// (x, row y) at screen (x + u - 0.5, H - y + v - 0.5), and
		/// `SplatFilm` and the unfiltered splat fallbacks round a splat
		/// to the nearest pixel centre in that convention).  Before
		/// DL-294 the camera cut at its NOMINAL film [0, W) x [0, H),
		/// which is half a pixel off that convention on both axes: one
		/// half-pixel edge strip per axis was rejected by the camera and
		/// the opposite one was accepted and then rounded off the film,
		/// so a light-traced (t = 1) splat covered (W - 0.5) x (H - 0.5)
		/// of a W x H film (-6.15 % on a uniformly lit 16 x 16 frame,
		/// image column 0 and row 0 at half radiance).  The projections
		/// now reject only points outside this convention-agnostic
		/// one-pixel band around the nominal film -- wide enough to
		/// contain the film under any half-pixel convention, narrow
		/// enough that every caller's integer cast of a rounded raster
		/// coordinate stays in range.
		inline bool InRasterGuardBand(
			const Scalar px, const Scalar py,
			const Scalar width, const Scalar height )
		{
			return px >= Scalar( -1 ) && px < width + Scalar( 1 ) &&
			       py >= Scalar( -1 ) && py < height + Scalar( 1 );
		}

		/// Maps a 3D world point to raster coordinates.  The result is
		/// NOT clipped to the film: see `InRasterGuardBand` (DL-294) --
		/// the splat film decides membership.
		/// \return FALSE if the point is behind the camera or outside
		/// the one-pixel guard band around the nominal [0,width) x
		/// [0,height) film
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
