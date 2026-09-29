//////////////////////////////////////////////////////////////////////
//
//  CameraImportanceTest.cpp - the camera side of bidirectional
//    transport: aperture sampling, the inverse projection through a
//    sampled aperture point, and the importance / directional-pdf
//    pair that the t==1 light-tracing strategy multiplies by.
//
//  WHAT THIS FILE EXISTS TO PIN (debt 28).
//
//    `thinlens_camera` is the only RISE camera whose importance is
//    emitted from a region of non-zero AREA.  PBRT-v4's thin-lens
//    importance is
//        We = d^2 / (A_lens * W * H * cos^4(theta)),
//    and the 1/A_lens in it is there for exactly one reason: the
//    light-tracing connection is supposed to SAMPLE a point on the
//    aperture with density 1/A_lens, and the estimator's 1/pdf cancels
//    it.  BDPT and VCM connected to the lens CENTRE instead and
//    multiplied by that importance anyway, so every t==1 splat came
//    out 1/(cos(theta) * A_lens) too bright -- 2.7e6 for a 15.1 mm
//    f/22 lens in a metres scene.
//
//    The end-to-end guards are the thin-lens topologies in
//    BDPTStrategyBalanceTest / VCMStrategyBalanceTest.  THIS file
//    pins the camera-side algebra underneath them, where the oracles
//    are closed forms rather than render means:
//
//      1. GetApertureWorldArea really is 1 / (SampleLensPoint's area
//         density) -- disk, polygonal blades, and anamorphic squeeze.
//         Everything else rests on that identity.
//      2. RasterFromLensPoint round-trips GenerateRayWithLensSample.
//      3. Zero circle of confusion ON the plane of focus: every
//         aperture point images a focus-plane point to the SAME pixel.
//      4. Off focus, the aperture rim images a world point onto a
//         CIRCLE of the analytic CoC radius.
//      5. Importance and PdfDirection equal the pinhole's closed form
//         at matched field of view, for EVERY f-stop -- the aperture
//         area has cancelled out of both, which is the fix.
//      6. The film response from a uniformly-radiating plane has the
//         closed form  k * A_patch / (W * H * D^2), and for a plane
//         that overfills the frustum it is exactly 1, independent of
//         f-stop, focus setting and camera type.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/Library/Cameras/ThinLensCamera.h"
#include "../src/Library/Cameras/PinholeCamera.h"
#include "../src/Library/Cameras/FisheyeCamera.h"
#include "../src/Library/Cameras/CameraUtilities.h"
#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Rendering/SplatFilm.h"

using namespace RISE;
using namespace RISE::Implementation;

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond) do {                                              \
		if( !(cond) ) {                                                \
			std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__      \
			          << "  " #cond << std::endl;                      \
			++g_fail;                                                  \
		} else { ++g_pass; }                                           \
	} while(0)

#define EXPECT_NEAR(a, b, tol) do {                                    \
		const double _a = (a);                                         \
		const double _b = (b);                                         \
		if( !( fabs( _a - _b ) <= (tol) ) ) {                          \
			std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__      \
			          << "  |" #a " - " #b "| > " #tol                 \
			          << "   (" << _a << " vs " << _b                  \
			          << ", diff " << fabs(_a-_b) << ")" << std::endl; \
			++g_fail;                                                  \
		} else { ++g_pass; }                                           \
	} while(0)

#define EXPECT_REL(a, b, rel) do {                                     \
		const double _a = (a);                                         \
		const double _b = (b);                                         \
		const double _d = fabs( _b ) > 0 ? fabs( _a - _b ) / fabs( _b ) \
		                                 : fabs( _a - _b );            \
		if( !( _d <= (rel) ) ) {                                       \
			std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__      \
			          << "  " #a " vs " #b " relative " << _d          \
			          << " > " #rel                                    \
			          << "   (" << _a << " vs " << _b << ")"           \
			          << std::endl;                                    \
			++g_fail;                                                  \
		} else { ++g_pass; }                                           \
	} while(0)

template<typename T> static void release( T* p ) { if( p ) p->release(); }

//////////////////////////////////////////////////////////////////////
// The one camera geometry every test in this file uses.
//
// Camera at (0, 0, D) looking at the origin, so the optical axis is
// world -Z and a plane at z = 0 sits at axial depth D.  The 36 mm
// sensor / 50 mm lens pair gives a vertical field of view of
// 2*atan(36/100) = 39.598 deg at 1:1 image aspect and 1:1 pixel
// aspect, which is also what the matched pinhole is built with.
//////////////////////////////////////////////////////////////////////

static const double  kCamZ        = 6.0;		// scene units (metres)
static const double  kSensorMM    = 36.0;
static const double  kFocalMM     = 50.0;
static const unsigned int kWidth  = 64;
static const unsigned int kHeight = 64;
static const double  kPixelAR     = 1.0;

//! Vertical field of view in radians for the sensor/focal pair above
//! at the given image aspect.  Mirrors ThinLensCamera::Recompute.
static double FovVertical( double imageAspect )
{
	return 2.0 * atan( ( kSensorMM / imageAspect ) / ( 2.0 * kFocalMM ) );
}

static ThinLensCamera* MakeThinLens(
	double fstop,
	double focusDistance,
	unsigned int blades = 0,
	double squeeze = 1.0,
	double shiftXmm = 0.0,
	double shiftYmm = 0.0,
	unsigned int width = kWidth,
	unsigned int height = kHeight,
	double pixelAR = kPixelAR )
{
	return new ThinLensCamera(
		Point3( 0, 0, kCamZ ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		kSensorMM, kFocalMM, fstop, focusDistance,
		1.0 /* sceneUnitMeters */,
		width, height, pixelAR,
		0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
		blades, 0.0, squeeze,
		0.0, 0.0, shiftXmm, shiftYmm );
}

static PinholeCamera* MakePinholeMatched(
	unsigned int width = kWidth,
	unsigned int height = kHeight,
	double pixelAR = kPixelAR )
{
	const double imageAspect = double( width ) * pixelAR / double( height );
	return new PinholeCamera(
		Point3( 0, 0, kCamZ ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		FovVertical( imageAspect ),
		width, height, pixelAR,
		0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ) );
}

//! Aperture radius in scene units, derived the way Recompute does:
//! diameter = focal / fstop, in scene units.
static double ApertureRadius( double fstop )
{
	const double focalScene = kFocalMM * 0.001;		// sceneUnitMeters == 1
	return focalScene / ( 2.0 * fstop );
}

//////////////////////////////////////////////////////////////////////
// Test 1 - GetApertureWorldArea IS 1 / (SampleLensPoint's density).
//
// The whole fix rests on this identity: `Importance` folds in
// 1 / p_A(lens point), and it is allowed to do that ONLY because
// SampleLensPoint is uniform over an area the camera can report.
//
// Two independent checks per aperture shape, both purely geometric so
// there is no circularity with the camera's own area formula:
//
//   (a) CONTAINMENT.  Every sample lies inside the claimed shape (a
//       disk of radius r, or a regular n-gon inscribed in it), after
//       undoing the anamorphic squeeze and the pixelAR compensation.
//   (b) UNIFORMITY + SCALE.  The fraction of samples landing inside a
//       concentric probe disk of radius rho must be
//       (pi * rho^2) / GetApertureWorldArea().  A non-uniform density
//       breaks it (the naive n-gon sampler this code could have used
//       under-samples the corners by cos^2(pi/n), a 25% error at 6
//       blades); a wrong reported area breaks it linearly.
//
// The probe radius is the polygon's INRADIUS, so the probe disk is
// entirely inside the shape for every blade count.
//////////////////////////////////////////////////////////////////////
static void TestApertureAreaIsOneOverDensity()
{
	std::cout << "TestApertureAreaIsOneOverDensity" << std::endl;

	struct Case { const char* name; unsigned int blades; double squeeze; };
	const Case cases[] = {
		{ "disk",              0, 1.0 },
		{ "6 blades",          6, 1.0 },
		{ "5 blades",          5, 1.0 },
		{ "disk, squeeze 2.0", 0, 2.0 },
		{ "6 blades, sq 0.5",  6, 0.5 },
	};

	const double fstop = 2.8;
	const double r = ApertureRadius( fstop );

	for( unsigned int c = 0; c < sizeof(cases)/sizeof(cases[0]); c++ )
	{
		ThinLensCamera* cam = MakeThinLens( fstop, 3.0, cases[c].blades, cases[c].squeeze );

		const double area = cam->GetApertureWorldArea();

		// Closed form for the shape the sampler draws over, times the
		// squeeze (an x-axis scale on the lens plane).
		const double shape = ( cases[c].blades < 3 )
			? PI * r * r
			: 0.5 * double( cases[c].blades )
			      * sin( 2.0 * PI / double( cases[c].blades ) ) * r * r;
		EXPECT_REL( area, shape * fabs( cases[c].squeeze ), 1e-12 );

		// Probe radius: the inradius of the shape, so the probe disk
		// is strictly inside it.
		const double inradius = ( cases[c].blades < 3 )
			? r * 0.75
			: r * cos( PI / double( cases[c].blades ) ) * 0.75;

		// Stratified uv over [0,1)^2 so the estimate is not at the
		// mercy of an RNG seed; the sampler is a deterministic
		// function of uv, which is what makes this reproducible.
		const unsigned int NS = 512;
		unsigned int inside = 0;
		unsigned int outsideShape = 0;
		for( unsigned int i = 0; i < NS; i++ ) {
			for( unsigned int j = 0; j < NS; j++ ) {
				const Point2 uv( ( i + 0.5 ) / double( NS ), ( j + 0.5 ) / double( NS ) );
				const Point3 p = cam->SampleLensPoint( uv );

				// Undo the pixelAR compensation (pixelAR == 1 here, so
				// this is a no-op, but it states the convention) and
				// the squeeze, landing back on the un-squeezed shape.
				const double x = p.x * kPixelAR / cases[c].squeeze;
				const double y = p.y;
				const double rad = sqrt( x * x + y * y );

				if( rad > r * ( 1.0 + 1e-9 ) ) outsideShape++;

				// The probe region is a DISK of radius `inradius` in
				// the UN-squeezed frame -- i.e. an ellipse of area
				// pi * inradius^2 * squeeze on the lens plane itself.
				// Testing it in the un-squeezed frame is the same
				// region, stated without the ellipse algebra.
				if( rad <= inradius ) inside++;
			}
		}

		// (a) containment in the circumscribing circle (after
		// un-squeezing).  A polygon sampler is additionally inside the
		// polygon by construction of rMax.
		EXPECT( outsideShape == 0 );

		// (b) the measured fraction against area.  The probe region is
		// the un-squeezed disk of radius `inradius`, which the squeeze
		// maps to an ellipse of area pi * inradius^2 * squeeze.
		const double probeArea = PI * inradius * inradius * fabs( cases[c].squeeze );
		const double expectedFraction = probeArea / area;
		const double measuredFraction = double( inside ) / double( NS * NS );
		// 512x512 stratified samples: the residual is the boundary
		// discretisation of the probe circle, O(perimeter/NS) ~ 0.5%.
		EXPECT_REL( measuredFraction, expectedFraction, 0.01 );

		std::printf( "    %-18s area=%.6e  frac %.5f vs %.5f\n",
			cases[c].name, area, measuredFraction, expectedFraction );

		release( cam );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 2 - RasterFromLensPoint inverts GenerateRayWithLensSample.
//
// The strongest available oracle for the inverse projection: take the
// ray the camera itself would generate for a given film sample through
// a given aperture point, walk along it, and ask which film sample
// images that world point through the SAME aperture point.  It must be
// the one we started from, for any distance along the ray, in front of
// or behind the plane of focus.
//
// Run with lens shift on as well, because the hand-rolled projection
// this replaced (px = w/2 - k*x/z) silently ignored shift and tilt and
// would fail the shifted rows.
//////////////////////////////////////////////////////////////////////
static void TestInverseProjectionRoundTrip()
{
	std::cout << "TestInverseProjectionRoundTrip" << std::endl;

	struct Case { const char* name; double fstop; double focus; double shiftX; double shiftY; double tiltDeg; };
	const Case cases[] = {
		{ "f/22 focused",        22.0, 6.0, 0.0, 0.0, 0.0 },
		{ "f/2.8 focused",        2.8, 6.0, 0.0, 0.0, 0.0 },
		{ "f/2.8 near focus",     2.8, 1.0, 0.0, 0.0, 0.0 },
		{ "f/2.8 + shift 6/-4mm", 2.8, 6.0, 6.0, -4.0, 0.0 },
	};

	RandomNumberGenerator rng;
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	for( unsigned int c = 0; c < sizeof(cases)/sizeof(cases[0]); c++ )
	{
		ThinLensCamera* cam = MakeThinLens(
			cases[c].fstop, cases[c].focus, 0, 1.0, cases[c].shiftX, cases[c].shiftY );

		double worstErr = 0;
		for( unsigned int s = 0; s < 7; s++ ) {
			for( unsigned int p = 0; p < 5; p++ ) {
				const Point2 uv( ( s * 0.13 + 0.07 ), ( s * 0.37 + 0.11 ) );
				const Point2 uvw( uv.x - floor( uv.x ), uv.y - floor( uv.y ) );
				const Point2 screen(
					2.0 + p * ( kWidth  - 4.0 ) / 4.0,
					3.0 + s * ( kHeight - 6.0 ) / 6.0 );

				Ray r;
				EXPECT( cam->GenerateRayWithLensSample( rc, r, screen, uvw ) );

				const Point3 local = cam->SampleLensPoint( uvw );

				// Several distances, straddling the plane of focus.
				const double dists[] = { 0.4, 1.0, 6.0, 30.0 };
				for( unsigned int d = 0; d < 4; d++ ) {
					const Point3 world = r.PointAtLength( dists[d] );
					Point2 back;
					const bool ok = cam->RasterFromLensPoint( world, local, back );
					EXPECT( ok );
					if( !ok ) continue;
					const double e = std::max( fabs( back.x - screen.x ), fabs( back.y - screen.y ) );
					if( e > worstErr ) worstErr = e;
				}
			}
		}
		// Raster coordinates run 0..64 here; 1e-9 px is ~1e-11 relative.
		EXPECT_NEAR( worstErr, 0.0, 1e-9 );
		std::printf( "    %-22s worst round-trip error %.3e px\n", cases[c].name, worstErr );
		release( cam );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 3 - ZERO circle of confusion on the plane of focus.
//
// The defining property of the plane of focus: a point on it images to
// the same film position from EVERY point on the aperture.  If this
// failed, a light-traced splat of an in-focus surface would smear
// while the eye-traced layer stayed sharp.
//////////////////////////////////////////////////////////////////////
static void TestZeroCoCAtFocus()
{
	std::cout << "TestZeroCoCAtFocus" << std::endl;

	const double focus = 4.0;					// axial depth of the focal plane
	const double planeZ = kCamZ - focus;		// world z of that plane

	const double fstops[] = { 1.4, 2.8, 22.0 };

	for( unsigned int fi = 0; fi < 3; fi++ )
	{
		ThinLensCamera* cam = MakeThinLens( fstops[fi], focus );
		const double r = ApertureRadius( fstops[fi] );

		// A few off-axis world points ON the focal plane.
		const double xs[] = { 0.0, 0.4, -0.9 };
		const double ys[] = { 0.0, -0.3, 0.7 };

		double worst = 0;
		for( unsigned int k = 0; k < 3; k++ )
		{
			const Point3 world( xs[k], ys[k], planeZ );

			Point2 ref;
			EXPECT( cam->RasterFromLensPoint( world, Point3( 0, 0, 0 ), ref ) );

			// Sweep the whole aperture, rim included.
			for( unsigned int a = 0; a < 16; a++ ) {
				const double th = 2.0 * PI * a / 16.0;
				for( unsigned int q = 1; q <= 4; q++ ) {
					const double rr = r * q / 4.0;
					const Point3 lens( rr * cos( th ), rr * sin( th ), 0.0 );
					Point2 got;
					if( !cam->RasterFromLensPoint( world, lens, got ) ) continue;
					worst = std::max( worst,
						std::max( fabs( got.x - ref.x ), fabs( got.y - ref.y ) ) );
				}
			}
		}
		EXPECT_NEAR( worst, 0.0, 1e-9 );
		std::printf( "    f/%-5.1f  worst focus-plane spread %.3e px\n", fstops[fi], worst );
		release( cam );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 4 - OFF focus, the aperture rim images onto a circle of the
// analytic circle-of-confusion radius.
//
// Closed form.  With the plane of focus at axial depth S1 and the
// world point at axial depth S2, the film displacement produced by an
// aperture point at lens-plane offset L is
//
//     delta_raster = L * H * (1/S1 - 1/S2) / (2 tan(fov_v/2))
//
// (derived by pushing the focal-plane intersection back through the
// lens centre; the film distance and the magnification cancel).  Its
// magnitude at |L| = aperture radius is the CoC RADIUS in pixels, and
// it agrees with the photographic
//     c = A * f/(S1-f) * |S2-S1|/S2
// once c is divided by the pixel pitch 2*filmDistance*tan/H.
//
// Three things are asserted: the magnitude matches to 1e-9 relative
// (that is the CoC); the SIGNED scale is the same on both axes -- so
// the blur figure is a circle, not an ellipse, a shear or a mirror;
// and each axis's signed scale equals `kRasterSign{X,Y} * H*(1/S1 -
// 1/S2)/(2 tan)` with a single pair of +-1 constants shared by every
// case, which is the statement that the displacement reverses
// direction when the plane of focus crosses the world point.  (An earlier version of this test claimed the
// displacement is anti-parallel to the lens offset but compared only
// magnitudes, so it would have passed a mirrored or a never-reversing
// blur alike -- debt 28 review, A P2-2.)
//////////////////////////////////////////////////////////////////////
static void TestAnalyticCircleOfConfusion()
{
	std::cout << "TestAnalyticCircleOfConfusion" << std::endl;

	// Fixed by the camera basis and the raster convention alone
	// (MakeThinLens looks down -Z with +Y up, and raster y runs DOWN
	// the image while raster x runs with the projected world x through
	// one net reflection), so these are the same for every case below
	// -- which is exactly why asserting them as constants has teeth.
	const double kRasterSignX = -1.0;
	const double kRasterSignY = +1.0;

	const double fov = FovVertical( 1.0 );
	const double tanHalf = tan( fov * 0.5 );

	struct Case { double fstop; double S1; double S2; };
	const Case cases[] = {
		{  2.8, 4.0, 6.0 },			// focus in front of the point
		{  2.8, 6.0, 3.0 },			// focus behind it
		{ 22.0, 4.0, 6.0 },
		{  1.4, 0.5, 6.0 },			// extreme: focus far in front
	};

	for( unsigned int c = 0; c < sizeof(cases)/sizeof(cases[0]); c++ )
	{
		ThinLensCamera* cam = MakeThinLens( cases[c].fstop, cases[c].S1 );
		const double r = ApertureRadius( cases[c].fstop );

		const double expectedRadiusPx =
			r * double( kHeight ) * fabs( 1.0 / cases[c].S1 - 1.0 / cases[c].S2 )
			  / ( 2.0 * tanHalf );

		// On-axis world point at axial depth S2 keeps the algebra exact
		// (no perspective foreshortening of the blur figure).
		const Point3 world( 0, 0, kCamZ - cases[c].S2 );

		Point2 centre;
		EXPECT( cam->RasterFromLensPoint( world, Point3( 0, 0, 0 ), centre ) );

		// Signed prediction.  kRasterSign is a property of the camera
		// basis, not of the case: MakeThinLens looks down -Z with +Y up,
		// and RasterFromLensPoint's projection through the lens centre
		// carries one net reflection, so a lens offset displaces the
		// image by kRasterSign * (that magnitude).  Asserting it as a
		// CONSTANT across all four cases is what pins the sign flip
		// between "focus in front of the point" and "focus behind it".
		const double signedFactor =
			double( kHeight ) * ( 1.0 / cases[c].S1 - 1.0 / cases[c].S2 )
			  / ( 2.0 * tanHalf );
		const double expectedSignedScaleX = kRasterSignX * signedFactor;
		const double expectedSignedScaleY = kRasterSignY * signedFactor;
		const double expectedScaleMag     = fabs( signedFactor );

		double worstRadiusErr = 0;
		double worstShapeErr = 0;
		double worstSignedErr = 0;
		for( unsigned int a = 0; a < 32; a++ ) {
			const double th = 2.0 * PI * a / 32.0;
			const Point3 lens( r * cos( th ), r * sin( th ), 0.0 );
			Point2 got;
			EXPECT( cam->RasterFromLensPoint( world, lens, got ) );
			const double dx = got.x - centre.x;
			const double dy = got.y - centre.y;
			const double rad = sqrt( dx * dx + dy * dy );
			worstRadiusErr = std::max( worstRadiusErr,
				fabs( rad - expectedRadiusPx ) / expectedRadiusPx );

			// SIGNED scale on each axis.  Comparing magnitudes only
			// (which this test used to do) would pass a renderer whose
			// blur figure is mirrored, sheared, or points the wrong way
			// -- all of which still produce a circle of the right
			// radius.  The closed form predicts the signed value
			//     d(raster)/d(lens) = kSign * H*(1/S1 - 1/S2)/(2 tan)
			// on BOTH axes, with kSign a fixed +-1 set only by the
			// camera basis's raster orientation (the same constant for
			// every case below, including the two whose (1/S1 - 1/S2)
			// have opposite sign -- which is the real content: the
			// displacement REVERSES when the focus plane crosses the
			// point).
			const double sxScale = ( fabs( lens.x ) > 1e-12 ) ? ( dx / lens.x ) : 0;
			const double syScale = ( fabs( lens.y ) > 1e-12 ) ? ( dy / lens.y ) : 0;
			// Isotropy: the two axes must scale by the same MAGNITUDE
			// (their signs differ by the raster y-flip, which the
			// per-axis signed checks below pin exactly).
			if( fabs( lens.x ) > 1e-12 && fabs( lens.y ) > 1e-12 ) {
				worstShapeErr = std::max( worstShapeErr,
					fabs( fabs( sxScale ) - fabs( syScale ) ) / fabs( syScale ) );
			}
			if( fabs( lens.x ) > 1e-12 ) {
				worstSignedErr = std::max( worstSignedErr,
					fabs( sxScale - expectedSignedScaleX ) / expectedScaleMag );
			}
			if( fabs( lens.y ) > 1e-12 ) {
				worstSignedErr = std::max( worstSignedErr,
					fabs( syScale - expectedSignedScaleY ) / expectedScaleMag );
			}
		}

		EXPECT_NEAR( worstRadiusErr, 0.0, 1e-9 );
		EXPECT_NEAR( worstShapeErr,  0.0, 1e-9 );
		EXPECT_NEAR( worstSignedErr, 0.0, 1e-9 );
		std::printf( "    f/%-5.1f S1=%.2f S2=%.2f  CoC r = %.4f px (err %.2e, shape %.2e, signed %.2e vs %+.4f px/unit)\n",
			cases[c].fstop, cases[c].S1, cases[c].S2,
			expectedRadiusPx, worstRadiusErr, worstShapeErr,
			worstSignedErr, expectedSignedScaleX );
		release( cam );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 5 - Importance and PdfDirection are the PINHOLE's closed form
// at matched field of view, for EVERY f-stop.
//
// This is the fix stated as an identity.  RISE's convention (see
// BDPTCameraUtilities::Importance) is that `Importance` already folds
// in the aperture cosine and 1 / p_A(aperture point), so
//
//     Importance = We * cos(theta) * A_lens
//                = d^2 / (pixelAR * W * H * cos^3(theta))
//
// with d = H / (2 tan(fov_v/2)) -- the aperture area has CANCELLED.
// That is the physical statement that a lens trades depth of field for
// noise, not exposure: a thin lens gathers exactly as much radiance
// per unit film area as a pinhole of the same field of view, and the
// only thing the aperture changes about a t==1 contribution is which
// pixel it lands in.
//
// Three consequences, all asserted:
//   - thin lens == matched pinhole, to 1e-12 relative;
//   - f/1.4 == f/22 == f/1e5 (the pinhole limit is continuous, so the
//     degenerate-aperture special case -- which used to be a
//     reinterpret_cast to PinholeCamera, undefined behaviour -- is
//     genuinely gone);
//   - the same holds for a non-square film and a non-unit pixel
//     aspect, which is where the previously-missing 1/pixelAR showed.
//////////////////////////////////////////////////////////////////////
static void TestImportanceMatchesPinholeClosedForm()
{
	std::cout << "TestImportanceMatchesPinholeClosedForm" << std::endl;

	struct Geo { const char* name; unsigned int w; unsigned int h; double par; };
	const Geo geos[] = {
		{ "64x64  par 1.0",  64, 64, 1.0 },
		{ "96x54  par 1.0",  96, 54, 1.0 },
		{ "96x54  par 1.25", 96, 54, 1.25 },
	};
	const double fstops[] = { 1.4, 2.8, 22.0, 1e5 };

	for( unsigned int gi = 0; gi < 3; gi++ )
	{
		const Geo& g = geos[gi];
		const double imageAspect = double( g.w ) * g.par / double( g.h );
		const double d = double( g.h ) / ( 2.0 * tan( FovVertical( imageAspect ) * 0.5 ) );

		PinholeCamera* pin = MakePinholeMatched( g.w, g.h, g.par );

		// A spread of directions across the frustum, built from the
		// camera's own ray generator so cos(theta) is whatever the
		// camera really produces.
		RandomNumberGenerator rng;
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

		for( unsigned int fi = 0; fi < 4; fi++ )
		{
			ThinLensCamera* tl = MakeThinLens( fstops[fi], 6.0, 0, 1.0, 0.0, 0.0, g.w, g.h, g.par );

			double worstImp = 0, worstPdf = 0, worstClosed = 0;
			for( unsigned int p = 0; p < 5; p++ ) {
				for( unsigned int q = 0; q < 5; q++ ) {
					const Point2 screen(
						0.5 + p * ( g.w - 1.0 ) / 4.0,
						0.5 + q * ( g.h - 1.0 ) / 4.0 );

					Ray rp;
					if( !pin->GenerateRay( rc, rp, screen ) ) continue;

					const double impPin = BDPTCameraUtilities::Importance( *pin, rp );
					const double pdfPin = BDPTCameraUtilities::PdfDirection( *pin, rp );
					const double impTL  = BDPTCameraUtilities::Importance( *tl, rp );
					const double pdfTL  = BDPTCameraUtilities::PdfDirection( *tl, rp );

					worstImp = std::max( worstImp, fabs( impTL - impPin ) / impPin );
					worstPdf = std::max( worstPdf, fabs( pdfTL - pdfPin ) / pdfPin );

					// Independent closed form, from the camera basis
					// rather than from either camera's internals.
					const Vector3 axis = Vector3Ops::Normalize(
						Vector3Ops::mkVector3( Point3( 0, 0, 0 ), Point3( 0, 0, kCamZ ) ) );
					const double cosTheta = fabs( Vector3Ops::Dot( rp.Dir(), axis ) );
					const double closed = ( d * d ) /
						( g.par * double( g.w ) * double( g.h ) * cosTheta * cosTheta * cosTheta );
					worstClosed = std::max( worstClosed, fabs( impTL - closed ) / closed );
				}
			}

			EXPECT_NEAR( worstImp,    0.0, 1e-12 );
			EXPECT_NEAR( worstPdf,    0.0, 1e-12 );
			EXPECT_NEAR( worstClosed, 0.0, 1e-12 );

			std::printf( "    %-16s f/%-8.4g  imp %.2e  pdf %.2e  closed-form %.2e\n",
				g.name, fstops[fi], worstImp, worstPdf, worstClosed );

			release( tl );
		}
		release( pin );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 6 - the film response of a uniformly radiating plane.
//
// The quadrature the t==1 estimator actually performs.  For a plane of
// unit radiance at axial depth D, perpendicular to the optical axis,
// the light-tracing contribution of a surface element dA imaged
// through an aperture point is
//
//     dPhi = G * Importance * dA,   G = |cos(theta)| / dist^2.
//
// For a plane perpendicular to the axis, dist = D / cos(theta), so
//
//     G * Importance = (cos^3(theta) / D^2) * k / (W H cos^3(theta))
//                    = k / (W H D^2),     k = d^2 / pixelAR,
//
// a CONSTANT -- independent of position on the plane, of the aperture
// point, and of theta.  Two closed forms follow, and both are asserted:
//
//   (a) For a patch entirely inside the frustum (so every element
//       images onto the film for every aperture point),
//           Phi = k * A_patch / (W * H * D^2)
//       exactly.  The quadrature is then exact up to floating point,
//       so the tolerance is 1e-9 relative rather than a grid error.
//
//   (b) For a plane that OVERFILLS the frustum, the set of elements
//       imaging onto the film is the frustum footprint rigidly
//       TRANSLATED by the aperture point's parallax -- same area for
//       every aperture point -- so
//           Phi = k * (2 D tan)^2 * (W/H) * pixelAR / (W H D^2) = 1
//       exactly, for every f-stop, focus setting and camera type.  The
//       residual here is the grid's discretisation of the footprint
//       boundary, O(1/N).
//
// (b) is the energy statement the whole debt-28 fix is about: opening
// the aperture must not change how much light the film collects, only
// where on the film it lands.  The pre-fix code returned 2.7e6 for the
// f/22 row of (b) instead of 1.
//////////////////////////////////////////////////////////////////////
static double IntegrateEmittingPlane(
	const ICamera& cam,
	double halfExtent,			///< half-width of the square emitting region, world units
	unsigned int N,				///< grid resolution per axis
	unsigned int M,				///< aperture samples
	bool* allOnFilm )
{
	const double planeZ = 0.0;						// camera sits at z = kCamZ
	const Vector3 planeNormal( 0, 0, 1 );
	const double dA = ( 2.0 * halfExtent / double( N ) ) * ( 2.0 * halfExtent / double( N ) );

	double total = 0;
	bool everyPointLanded = true;

	for( unsigned int m = 0; m < M; m++ )
	{
		// Stratified aperture samples (deterministic; the sampler is a
		// pure function of uv).
		const unsigned int side = static_cast<unsigned int>( sqrt( double( M ) ) + 0.5 );
		const Point2 uv(
			( ( m % side ) + 0.5 ) / double( side ),
			( ( m / side ) + 0.5 ) / double( side ) );
		const BDPTCameraUtilities::ApertureSample ap =
			BDPTCameraUtilities::SampleAperture( cam, uv );

		for( unsigned int i = 0; i < N; i++ ) {
			for( unsigned int j = 0; j < N; j++ ) {
				const double x = -halfExtent + ( i + 0.5 ) * 2.0 * halfExtent / double( N );
				const double y = -halfExtent + ( j + 0.5 ) * 2.0 * halfExtent / double( N );
				const Point3 world( x, y, planeZ );

				// DL-294: the camera projection no longer clips to the
				// film; the film's own rule decides membership.
				Point2 raster;
				unsigned int ix = 0, iy = 0;
				if( !BDPTCameraUtilities::RasterizeThrough( cam, world, ap, raster ) ||
				    !SplatFilm::NearestPixel( raster.x, double( kHeight ) - raster.y, kWidth, kHeight, ix, iy ) ) {
					everyPointLanded = false;
					continue;
				}

				Vector3 dirToCam = Vector3Ops::mkVector3( ap.point, world );
				const double dist = Vector3Ops::Magnitude( dirToCam );
				dirToCam = dirToCam * ( 1.0 / dist );

				const double G = fabs( Vector3Ops::Dot( planeNormal, dirToCam ) ) / ( dist * dist );

				Ray camRay( ap.point, -dirToCam );
				const double We = BDPTCameraUtilities::Importance( cam, camRay );

				total += G * We * dA;
			}
		}
	}

	if( allOnFilm ) *allOnFilm = everyPointLanded;
	return total / double( M );
}

static void TestEmittingPlaneFilmResponse()
{
	std::cout << "TestEmittingPlaneFilmResponse" << std::endl;

	const double D = kCamZ;						// plane at z = 0, camera at z = D
	const double fov = FovVertical( 1.0 );
	const double a = D * tan( fov * 0.5 );		// frustum half-extent at depth D (square film)
	const double d = double( kHeight ) / ( 2.0 * tan( fov * 0.5 ) );
	const double k = d * d / kPixelAR;

	struct Case { const char* name; bool pinhole; double fstop; double focus; };
	const Case cases[] = {
		{ "pinhole",              true,   0.0, 0.0 },
		{ "thin lens f/22 focus", false, 22.0, D   },
		{ "thin lens f/2.8 focus",false,  2.8, D   },
		{ "thin lens f/2.8 near", false,  2.8, 0.5 },		// strongly defocused
		{ "thin lens f/1e5",      false,  1e5, D   },		// pinhole limit
	};

	for( unsigned int c = 0; c < sizeof(cases)/sizeof(cases[0]); c++ )
	{
		ICamera* cam = cases[c].pinhole
			? static_cast<ICamera*>( MakePinholeMatched() )
			: static_cast<ICamera*>( MakeThinLens( cases[c].fstop, cases[c].focus ) );

		// ---- (a) patch strictly inside the frustum.  Half the linear
		// extent, so even the f/2.8-near row's parallax cannot push any
		// element off the film (its worst shift is
		// r * |D/S1 - 1| = 0.0089 * 11 = 0.098 world units against the
		// 1.08 units of margin).
		{
			const double half = 0.5 * a;
			bool allOn = false;
			const double phi = IntegrateEmittingPlane( *cam, half, 240, 4, &allOn );
			const double expected = k * ( 2.0 * half ) * ( 2.0 * half )
				/ ( double( kWidth ) * double( kHeight ) * D * D );
			EXPECT( allOn );
			EXPECT_REL( phi, expected, 1e-9 );
			std::printf( "    %-24s inside-frustum patch  %.9f vs %.9f\n",
				cases[c].name, phi, expected );
		}

		// ---- (b) plane overfilling the frustum: exactly 1.
		{
			bool ignored = false;
			const double phi = IntegrateEmittingPlane( *cam, 1.6 * a, 900, 4, &ignored );
			// Grid error is the footprint boundary, ~ perimeter*h/area
			// = 8a * (3.2a/900) / (4a^2) = 0.7%.
			EXPECT_REL( phi, 1.0, 0.015 );
			std::printf( "    %-24s full-frustum response %.6f (closed form 1)\n",
				cases[c].name, phi );
		}

		release( cam );
	}
}

//////////////////////////////////////////////////////////////////////
// Test 7 - the delta-position cameras are untouched.
//
// SampleAperture must report a pinhole (and a thin lens stopped all
// the way down) as a point aperture at the camera location, and
// RasterizeThrough must then be exactly Rasterize.  This is the
// "pinhole path is bit-identical" claim, asserted rather than argued.
//////////////////////////////////////////////////////////////////////
static void TestDeltaPositionCamerasUnchanged()
{
	std::cout << "TestDeltaPositionCamerasUnchanged" << std::endl;

	PinholeCamera* pin = MakePinholeMatched();

	EXPECT( !BDPTCameraUtilities::HasFiniteAperture( *pin ) );

	const Point2 uvs[] = { Point2( 0.1, 0.9 ), Point2( 0.5, 0.5 ), Point2( 0.97, 0.03 ) };
	for( unsigned int u = 0; u < 3; u++ ) {
		const BDPTCameraUtilities::ApertureSample ap =
			BDPTCameraUtilities::SampleAperture( *pin, uvs[u] );
		EXPECT( !ap.isFinite );
		EXPECT_NEAR( ap.point.x, 0.0,   1e-15 );
		EXPECT_NEAR( ap.point.y, 0.0,   1e-15 );
		EXPECT_NEAR( ap.point.z, kCamZ, 1e-15 );

		const Point3 world( 0.7, -0.3, 1.1 );
		Point2 viaThrough, viaPlain;
		const bool a1 = BDPTCameraUtilities::RasterizeThrough( *pin, world, ap, viaThrough );
		const bool a2 = BDPTCameraUtilities::Rasterize( *pin, world, viaPlain );
		EXPECT( a1 == a2 );
		if( a1 && a2 ) {
			EXPECT_NEAR( viaThrough.x, viaPlain.x, 0.0 );
			EXPECT_NEAR( viaThrough.y, viaPlain.y, 0.0 );
		}
	}
	release( pin );

	// A thin lens whose aperture has collapsed reports the same way.
	// `anamorphic_squeeze 0` is the degenerate case the area formula
	// has to catch alongside a huge f-stop.
	ThinLensCamera* squashed = MakeThinLens( 2.8, 6.0, 0, 0.0 );
	EXPECT_NEAR( squashed->GetApertureWorldArea(), 0.0, 0.0 );
	EXPECT( !BDPTCameraUtilities::HasFiniteAperture( *squashed ) );
	{
		const BDPTCameraUtilities::ApertureSample ap =
			BDPTCameraUtilities::SampleAperture( *squashed, Point2( 0.3, 0.8 ) );
		EXPECT( !ap.isFinite );
		EXPECT_NEAR( ap.point.z, kCamZ, 1e-15 );
	}
	release( squashed );

	// A real aperture reports finite, and its sampled point is off the
	// lens centre.
	ThinLensCamera* open = MakeThinLens( 2.8, 6.0 );
	EXPECT( BDPTCameraUtilities::HasFiniteAperture( *open ) );
	{
		const BDPTCameraUtilities::ApertureSample ap =
			BDPTCameraUtilities::SampleAperture( *open, Point2( 0.3, 0.8 ) );
		EXPECT( ap.isFinite );
		const double off = sqrt( ap.local.x * ap.local.x + ap.local.y * ap.local.y );
		EXPECT( off > 0.5 * ApertureRadius( 2.8 ) );
		EXPECT( off <= ApertureRadius( 2.8 ) * ( 1.0 + 1e-9 ) );
	}
	release( open );
}

//////////////////////////////////////////////////////////////////////
// Test 8 - the FISHEYE camera's importance, audited alongside the
// thin lens (debt 28's "does any other camera have the same defect?"
// question).
//
// It does not: a fisheye has no aperture of non-zero area, its
// `Rasterize` and `Importance` read the same direction through the
// same inverse matrix, and its importance is defined per SOLID ANGLE
// rather than per unit lens area, so there is no cos(theta) * A_lens
// fold to get wrong (that is why the fisheye form carries no cosine
// where the pinhole's carries cos^3).  Pinned here by the same energy
// invariant Test 6 uses, restated for a spherical film:
//
//   A unit-radiance sphere of radius R centred on the camera gives
//       Phi = integral over the imaged set of G * We dA
//           = integral of (1/R^2) * (cosAngle/scale^2) * R^2 dw
//           = Area_xy / scale^2  =  1
//   because dw = dA_xy / cosAngle and the imaged set is exactly the
//   square |x|,|y| <= scale/2 (which lies wholly inside the unit disk
//   for scale <= sqrt(2)).  So the total film response is 1, exactly
//   as it is for the pinhole and the thin lens.
//
// DL-10 (docs/DEBT_LEDGER.md): at pixelAR == 1 the closed form above is
// exact because the local hemisphere point v IS the world direction (up
// to a solid-angle-preserving rotation) -- `mxTrans`'s 3x3 part is
// R * Stretch(pixelAR,1,1), and Stretch collapses to the identity.  At
// pixelAR != 1 the per-pixel WORLD solid angle differs from the local
// `scale^2/(W H cosAngle)` formula by the Jacobian of
// `v -> normalize(Stretch(pixelAR,1,1) * v)`, which is
// `pixelAR / |Stretch(pixelAR,1,1)*v|^3` (the general result for a
// linear map applied to a unit vector and renormalized).  Folding that
// Jacobian into `ImportanceFisheye` / `PdfDirectionFisheye`
// (CameraUtilities.cpp) makes the CLOSED FORM BELOW INVARIANT TO
// pixelAR: the Jacobian appears once in `We` (dividing it out) and once
// in the world solid-angle element the integral below sums over
// (multiplying it back in), so the two cancel and the total film
// response is 1 at every pixelAR, exactly as it is at pixelAR == 1.
// Pre-fix, `We` carried NO Jacobian while the true world solid angle
// still had it, so the totals below diverge from 1 in proportion to how
// hard `Stretch` distorts the direction distribution.
//
// Audited alongside pinhole / thin lens / orthographic for the same
// defect (debt 28's "does any other camera have the same defect?"
// question, re-asked for DL-10): none of the three carry it.
// `ComputePixelAreaAndDistance` (pinhole's pixel-area helper, shared by
// the thin lens via `GetImagePlanePixelDensity`) measures the world-
// space pixel footprint by literally transforming pixel CORNERS through
// the camera's affine matrix and taking the cross product -- an affine
// map has no per-direction renormalization step, so whatever `pixelAR`
// stretch is baked into the matrix is already exactly represented with
// no separate Jacobian to derive.  Orthographic has no per-ray
// projection at all (every ray is parallel; `viewportScale` already
// carries the world-space extent).  The fisheye is the only camera that
// projects onto a CURVED (hemispherical) film via a per-ray
// normalize-after-stretch step, which is what makes the Jacobian
// direction-dependent rather than a constant the way it is for a flat
// film.
//////////////////////////////////////////////////////////////////////
static void TestFisheyeFilmResponse( double pixelAR )
{
	std::cout << "TestFisheyeFilmResponse  pixelAR=" << pixelAR << std::endl;

	const double scale = 1.0;
	FisheyeCamera* cam = new FisheyeCamera(
		Point3( 0, 0, kCamZ ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
		kWidth, kHeight, pixelAR,
		0.0, 0.0, 0.0,
		Vector3( 0, 0, 0 ), Vector2( 0, 0 ),
		scale );

	EXPECT( !BDPTCameraUtilities::HasFiniteAperture( *cam ) );
	const BDPTCameraUtilities::ApertureSample ap =
		BDPTCameraUtilities::SampleAperture( *cam, Point2( 0.4, 0.6 ) );
	EXPECT( !ap.isFinite );
	EXPECT_NEAR( ap.point.z, kCamZ, 1e-15 );

	// Unit-radiance sphere of radius R centred on the camera, walked
	// on a (theta, phi) grid over the forward hemisphere.  Only the
	// hemisphere can image, so the integration domain is complete.
	const double R = 5.0;
	const unsigned int NT = 1400, NP = 1400;
	double total = 0;
	for( unsigned int i = 0; i < NT; i++ ) {
		const double th = ( i + 0.5 ) * ( PI * 0.5 ) / double( NT );		// 0 .. pi/2
		const double dth = ( PI * 0.5 ) / double( NT );
		for( unsigned int j = 0; j < NP; j++ ) {
			const double ph = ( j + 0.5 ) * ( 2.0 * PI ) / double( NP );
			const double dph = ( 2.0 * PI ) / double( NP );

			// Camera looks toward world -Z, so the forward hemisphere
			// is z < kCamZ.
			const Point3 world(
				R * sin( th ) * cos( ph ),
				R * sin( th ) * sin( ph ),
				kCamZ - R * cos( th ) );

			Point2 raster;
			unsigned int ix = 0, iy = 0;
			if( !BDPTCameraUtilities::RasterizeThrough( *cam, world, ap, raster ) ) continue;
			// DL-294: film membership is the film's decision.
			if( !SplatFilm::NearestPixel( raster.x, double( kHeight ) - raster.y, kWidth, kHeight, ix, iy ) ) continue;

			Vector3 dirToCam = Vector3Ops::mkVector3( ap.point, world );
			const double dist = Vector3Ops::Magnitude( dirToCam );
			dirToCam = dirToCam * ( 1.0 / dist );

			// Sphere normal at `world` points back at the camera, so
			// the geometric cosine is exactly 1.
			const double G = 1.0 / ( dist * dist );

			Ray camRay( ap.point, -dirToCam );
			const double We = BDPTCameraUtilities::Importance( *cam, camRay );

			total += G * We * ( R * R * sin( th ) * dth * dph );
		}
	}

	// Grid error is the discretisation of the imaged square's boundary
	// in (theta, phi), O(1/N) -- unchanged by pixelAR, since the accept/
	// reject boundary (`radius > 1` in the PRE-stretch local frame) does
	// not depend on it.
	EXPECT_REL( total, 1.0, 0.01 );
	std::printf( "    fisheye scale %.1f pixelAR %.1f  full-field response %.6f (closed form 1)\n",
		scale, pixelAR, total );

	release( cam );
}

//////////////////////////////////////////////////////////////////////
// Test 9 (DL-294) - the light-tracing projection covers EXACTLY the film
// the eye subpaths sample.
//
// Every rasterizer draws pixel (x, image row y) at screen position
// (x + u - 0.5, H - y + v - 0.5), u, v in [0, 1), and a t = 1 splat is
// deposited at SplatFilm::NearestPixel( raster.x, H - raster.y ).  So
// for every screen point an eye sample can have, the camera's
// world-to-raster inverse must (a) accept the point on the ray,
// (b) return that same screen point, and (c) send it to the same pixel.
// Before DL-294 the projection clipped at the camera's nominal
// [0, W) x [0, H), half a pixel off that convention on both axes, so
// the left half of image column 0 (screen x in [-0.5, 0)) and the upper
// half of image row 0 (screen y in [H, H + 0.5)) failed (a) -- 469 of
// 1525 probes at the DL-294 framing.  Run at the DL-294 framing
// (pinhole fov 2 deg, 16 x 16), at an ordinary one, and through a thin
// lens and a fisheye.  The film-response integrals above (Tests 6 and
// 8) now count only what the FILM accepts; they read 0.984 against the
// pre-fix projection -- they had been blind to this because they
// integrated over the camera's own accept region.
//
// CORRECTION (external review, 2026-09-29): this test does NOT catch a
// future ONE-SIDED convention change (the earlier claim that check (c)
// "guards the other direction" was false, and so was the identical claim
// in docs/DL294_NARROW_FOV_SPLAT.md section 4).  The reason is structural:
// `screen` above is built from `x + offs[a] - 0.5` / `H - y + offs[b] - 0.5`
// -- this test's OWN copy of the rasterizers' convention -- so if a future
// change moves ONLY the rasterizers' eye-ray placement (or only
// `SplatFilm::NearestPixel`'s rounding) and leaves this test's hard-coded
// formula alone, check (c) is comparing the mutated code against this
// test's UNCHANGED assumption, not against the other side of the real
// convention -- it can only ever catch a drift in `RasterizeThrough`'s OWN
// round-trip, which is what checks (a)/(b)/`worstErr` already cover.
// Measured: mutating `BDPTPelRasterizer`, `VCMPelRasterizer` and
// `BoxPixelFilter::warpOnScreen` to PBRT's (x + u, H - y + v - 1) sample
// placement, with `SplatFilm::NearestPixel` left untouched, leaves this
// whole test at 984/0 -- it cannot see the resulting splat/hit
// misregistration at all.  The real guard for that class of defect is
// `tests/BDPTStrategyBalanceTest.cpp`'s `TestNarrowFovStripeGuard`
// (`--narrow-fov-only`), which renders an INTERIOR floor edge through the
// real rasterizer and splat code paths (no hard-coded convention of its
// own) and compares where each side actually puts the energy.
//////////////////////////////////////////////////////////////////////
static void TestProjectionCoversEyeFilm()
{
	std::cout << "TestProjectionCoversEyeFilm (DL-294)" << std::endl;

	RandomNumberGenerator rng;
	RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );

	struct Case { const char* name; ICamera* cam; unsigned int w, h; ThinLensCamera* thin; };
	const unsigned int W = 16, H = 16;
	Case cases[] = {
		{ "pinhole fov 2 deg 16x16", new PinholeCamera(
			Point3( 0, 1, 1.2 ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ), 2.0 * DEG_TO_RAD,
			W, H, 1.0, 0.0, 0.0, 0.0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ) ), W, H, nullptr },
		{ "pinhole matched 64x64", MakePinholeMatched(), kWidth, kHeight, nullptr },
		{ "thin lens f/22 64x64", nullptr, kWidth, kHeight, MakeThinLens( 22.0, 6.0 ) },
		{ "fisheye scale 1 64x64", new FisheyeCamera(
			Point3( 0, 0, kCamZ ), Point3( 0, 0, 0 ), Vector3( 0, 1, 0 ),
			kWidth, kHeight, 1.0, 0.0, 0.0, 0.0, Vector3( 0, 0, 0 ), Vector2( 0, 0 ), 1.0 ), kWidth, kHeight, nullptr },
	};
	for( Case& c : cases ) {
		if( c.thin ) c.cam = c.thin;
	}

	// Sub-pixel offsets (u, v) out to the extreme edges of a pixel.  The
	// exact boundary u or v == 0 is left out: it is measure-zero, and the
	// rasterizers' row mapping H - y + v - 0.5 closes a pixel at the
	// opposite end from NearestPixel's round-half-up (a tie, not a strip).
	const double offs[] = { 0.001, 0.25, 0.5, 0.75, 0.999 };
	const unsigned int nOff = sizeof( offs ) / sizeof( offs[0] );

	for( Case& c : cases )
	{
		// The thin lens images through ONE fixed lens point, the same one
		// the projection is handed, so the round trip is exact at any
		// depth (a random lens point would add the circle of confusion).
		const Point2 lensUV( 0.3, 0.7 );
		const BDPTCameraUtilities::ApertureSample ap = BDPTCameraUtilities::SampleAperture( *c.cam, lensUV );
		unsigned int rejected = 0, wrongPixel = 0, tried = 0;
		double worstErr = 0;
		// Only the border ring of pixels and a few interior ones --
		// the defect lives at the edges.
		for( unsigned int y = 0; y < c.h; y++ ) {
			for( unsigned int x = 0; x < c.w; x++ ) {
				const bool border = ( x == 0 || y == 0 || x == c.w - 1 || y == c.h - 1 );
				if( !border && !( x == c.w / 2 && y == c.h / 2 ) ) continue;
				for( unsigned int a = 0; a < nOff; a++ ) {
					for( unsigned int b = 0; b < nOff; b++ ) {
						const Point2 screen( double( x ) + offs[a] - 0.5, double( c.h - y ) + offs[b] - 0.5 );
						Ray r;
						const bool ok = c.thin ? c.thin->GenerateRayWithLensSample( rc, r, screen, lensUV )
						                       : c.cam->GenerateRay( rc, r, screen );
						if( !ok ) continue;
						tried++;
						const Point3 world = r.PointAtLength( 3.0 );
						Point2 back;
						if( !BDPTCameraUtilities::RasterizeThrough( *c.cam, world, ap, back ) ) { rejected++; continue; }
						worstErr = std::max( worstErr, std::max( fabs( back.x - screen.x ), fabs( back.y - screen.y ) ) );
						unsigned int ix = 0, iy = 0;
						if( !SplatFilm::NearestPixel( back.x, double( c.h ) - back.y, c.w, c.h, ix, iy ) || ix != x || iy != y ) {
							wrongPixel++;
						}
					}
				}
			}
		}
		EXPECT( tried > 0 );
		EXPECT( rejected == 0 );
		EXPECT( wrongPixel == 0 );
		EXPECT_NEAR( worstErr, 0.0, 1e-8 );
		std::printf( "    %-26s tried %u  rejected by projection %u  landed in the wrong/no pixel %u  worst round trip %.2e px\n",
			c.name, tried, rejected, wrongPixel, worstErr );
		c.cam->release();
	}
}

int main()
{
	std::cout << "=== CameraImportanceTest ===" << std::endl;

	TestApertureAreaIsOneOverDensity();
	TestInverseProjectionRoundTrip();
	TestZeroCoCAtFocus();
	TestAnalyticCircleOfConfusion();
	TestImportanceMatchesPinholeClosedForm();
	TestEmittingPlaneFilmResponse();
	TestDeltaPositionCamerasUnchanged();
	TestFisheyeFilmResponse( 0.5 );
	TestFisheyeFilmResponse( 1.0 );
	TestFisheyeFilmResponse( 2.0 );
	TestProjectionCoversEyeFilm();

	std::cout << std::endl;
	std::cout << "Passed: " << g_pass << std::endl;
	std::cout << "Failed: " << g_fail << std::endl;
	return g_fail == 0 ? 0 : 1;
}
