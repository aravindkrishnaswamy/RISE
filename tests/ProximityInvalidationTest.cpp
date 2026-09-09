//////////////////////////////////////////////////////////////////////
//
//  ProximityInvalidationTest.cpp - Test F of
//  docs/CROSS_OBJECT_PROXIMITY_DESIGN.md 1: the CROSS-OBJECT proximity
//  signal has NOTHING TO INVALIDATE, so a neighbour that moves is seen
//  by the very next pass.
//
//  WHY THIS IS THE PROPERTY WORTH A TEST OF ITS OWN.  The predecessor
//  this signal replaced was a cross-object AMBIENT-OCCLUSION BAKE, and
//  the measurement that killed it was invalidation cost: moving one
//  object in Sponza invalidates a median of 33 neighbours' bakes and up
//  to 93 % of them, and an infinite plane is invisible to the bbox
//  census that would decide.  A LIVE QUERY has no such cost -- there is
//  no stored answer to go stale -- and this suite is what makes that a
//  checked property rather than an argument.
//
//  IT DRIVES REAL RENDER PASSES, not hand-built hit records, and that is
//  deliberate: the mechanism the design names (5.4) is the
//  unconditional generation bump at every render-pass ENTRY in
//  PixelBasedRasterizerHelper, so a test that evaluates the signal
//  without starting a pass would skip the very thing under test.  Each
//  probe below is `IJob::Rasterize()` on a tiny film with a capturing
//  sink, reading back a number the moved neighbour must have changed.
//
//  WHAT IT DOES *NOT* CLAIM, said here so the coverage is not
//  overstated.  It does not isolate the neighbour's move from the
//  pass-entry bump -- both happen between the two renders, by design,
//  because that IS the shipping sequence.  The unit-level pin that no
//  stale entry survives a pass boundary is ExpressionMemoTest's
//  generation red-proof, and the pin that the proximity entry
//  specifically clears is that suite's (l) / (n) rows.  Per-sample
//  motion blur is exercised by nothing here and is a stated residual
//  (design 10).
//
//  IT ALSO CARRIES THE DEMAND-GATE CHECK (c), which is about the same
//  scene-level state from the other side: the world-AABB snapshot that
//  makes a live query cheap is built ONLY when something in the process
//  actually calls proximity().  It lives here rather than in
//  ProximitySignalTest because it needs a whole render to observe, which
//  is what this file is already set up for.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <string>
#include <vector>

#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Job.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Managers/ObjectManager.h"	// (c)'s ForTest_HasBoxSnapshot

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( const bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

//! Captures the mean luminance of a rendered frame.
//!
//! OutputDenoisedImage IS OVERRIDDEN to do nothing, and that is not
//! boilerplate: IRasterizerOutput's default implementation FORWARDS
//! post-denoise pixels to OutputImage, so a sink that overrides only
//! OutputImage silently measures OIDN's output instead of the renderer's.
//! (The scenes below also set `oidn_denoise FALSE`; belt and braces,
//! because a future edit to the scene text should not be able to change
//! what this suite is measuring.)
class MeanLuminanceSink : public virtual IRasterizerOutput, public virtual Reference
{
public:
	double mean;
	MeanLuminanceSink() : mean( 0.0 ) {}

protected:
	virtual ~MeanLuminanceSink() {}

public:
	virtual void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
	virtual void OutputDenoisedImage( const IRasterImage&, const Rect*, const unsigned int ) override {}

	virtual void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
	{
		const unsigned int w = img.GetWidth(), h = img.GetHeight();
		double sum = 0.0;
		for( unsigned int y = 0; y < h; ++y ) {
			for( unsigned int x = 0; x < w; ++x ) {
				const RISEColor c = img.GetPEL( x, y );
				sum += (double)c.base.r + (double)c.base.g + (double)c.base.b;
			}
		}
		mean = ( w && h ) ? sum / (double)( w * h * 3 ) : 0.0;
	}
};

//! THE SCENE.  A floor plane whose ALBEDO IS THE SIGNAL: its reflectance
//! expression is `vec3(proximity(4), ...)`, and the only light is a white
//! ambient, so the rendered pixel is a monotone function of the signal at
//! that point.  That is all these checks need -- every assertion below is
//! about a value MOVING, or reaching the neutral, not about a calibrated
//! radiometric number.
//!
//! The neighbour is a 2x2x2 box whose `position` is the parameter the
//! edits below move.  It sits OFF TO THE SIDE at x = 3, and the camera
//! looks straight down at the floor point at the ORIGIN -- deliberately,
//! and it is the one geometric choice in this file worth stating.  Parked
//! the obvious way, directly ABOVE the probe point, the box is between the
//! camera and the floor: every pixel then shows the BOX's own flat 0.5
//! albedo, which does not move when the box moves, and every check here
//! reads a constant while looking like it is measuring the signal.  From
//! the side, the probe point sees the floor and the distance to the box is
//! `sqrt(3-1)^2 + (y-1)^2)` -- monotone in the box's height, which is what
//! the checks need.
//!
//! The radius is 8 (not 4) so the intermediate positions stay INSIDE it: a
//! sequence that saturates at the neutral after one step would make a
//! monotonicity check pass on equal zeros.  8x8 film: small enough to
//! render in milliseconds, large enough that one stray pixel cannot
//! dominate the mean.
static std::string SceneText( const char* boxY, const char* timelineBlock )
{
	std::string s =
		"RISE ASCII SCENE 7\n"
		"standard_shader\n{\nname global\nshaderop DefaultDirectLighting\n}\n"
		"pixelpel_rasterizer\n{\nsamples 1\noidn_denoise FALSE\n}\n"
		"film\n{\nwidth 8\nheight 8\n}\n"
		"pinhole_camera\n{\nlocation 0 6 0.0001\nlookat 0 0 0\nup 0 1 0\nfov 12.0\n}\n"
		"ambient_light\n{\nname amb\ncolor 1.0 1.0 1.0\npower 1.0\n}\n"
		"expression_painter\n{\nname p_probe\nexpr vec3( proximity(8.0), proximity(8.0), proximity(8.0) )\n}\n"
		"lambertian_material\n{\nname m_probe\nreflectance p_probe\n}\n"
		"uniformcolor_painter\n{\nname p_plain\ncolor 0.5 0.5 0.5\n}\n"
		"lambertian_material\n{\nname m_plain\nreflectance p_plain\n}\n"
		"infiniteplane_geometry\n{\nname g_plane\nxtile 1.0\nytile 1.0\n}\n"
		"box_geometry\n{\nname g_box\nwidth 2.0\nheight 2.0\ndepth 2.0\n}\n"
		"standard_object\n{\nname floor\ngeometry g_plane\nmaterial m_probe\norientation -90 0 0\nposition 0 0 0\n}\n"
		"standard_object\n{\nname mover\ngeometry g_box\nmaterial m_plain\nposition 3 ";
	s += boxY;
	s += " 0\n}\n";
	s += timelineBlock;
	return s;
}

//! Render the job as it stands and return the frame's mean luminance.
//! EVERY probe goes through a real `Rasterize()`, which is what takes the
//! pass-entry generation bump -- see this file's header.
static double RenderMean( Job& job )
{
	MeanLuminanceSink* sink = new MeanLuminanceSink();
	sink->addref();
	job.GetRasterizer()->AddRasterizerOutput( sink );
	job.Rasterize();
	const double m = sink->mean;
	sink->release();
	return m;
}

//======================================================================
// (a) A neighbour moved through the REAL incremental derive path
//======================================================================

static void TestIncrementalMove()
{
	std::cout << "(a) a neighbour moved through Cst::DeriveToJobIncremental is seen by the next render"
		<< std::endl;

	// The box starts at (3, 3, 0), so from the probe point at the origin
	// the nearest box corner edge is (2, 2, 0) -- sqrt(2^2 + 2^2) = 2.828
	// away -- and `proximity(8)` reads 1 - 2.828/8 = 0.646.
	Cst::Document doc = Cst::ParseToCst( SceneText( "3", "" ) );
	Job* job = new Job();
	std::vector<std::string> d0;
	Cst::DeriveToJob( doc, *job, &d0 );
	Check( d0.empty(), "(a) the fixture scene derives cleanly" );

	const double near_ = RenderMean( *job );
	Check( near_ > 0.0, "(a) the first render sees the neighbour (the signal is non-zero)" );

	// MOVE IT UP to y = 4.5, KEEPING x = 3.  The nearest corner is then
	// (2, 3.5, 0), so the distance rises to sqrt(2^2 + 3.5^2) = 4.031 and
	// the signal falls to 1 - 4.031/8 = 0.496.  Nothing is invalidated by
	// hand: there is no bake to invalidate, which is the entire point of
	// the design.
	//
	// *** KEEPING x = 3 IS THE WHOLE POINT, and this test got it wrong. ***
	// The first version moved the box to "0 4.5 0" -- straight under the
	// camera, which sits at (0, 6, 0.0001) with a 12-degree fov.  A 2x2x2
	// box centred at x = 0 spans [-1, 1], and the camera's footprint on the
	// floor is only about +/-0.63, so the box then covered the ENTIRE
	// frame: `mid` was reading the BOX's flat 0.5 Lambertian albedo
	// (0.5/pi = 0.159155), not the floor's signal at all.  That is exactly
	// the failure this file's own header warns about, four paragraphs up,
	// and it still shipped -- because every check downstream was a
	// COMPARISON (mid < near_ * 0.9, gone < mid * 0.05) and the accidental
	// constant happened to satisfy them.  The lesson generalises: a
	// monotonicity check cannot tell you WHICH quantity moved.  Hence the
	// closed-form check below, which can.
	const Cst::NodeId moverId = Cst::DocFindByName( doc, "standard_object/mover" );
	// DocFindByName returns 0 for "not found or ambiguous" -- see its own
	// doc comment; there is no named sentinel to compare against.
	Check( moverId != 0, "(a) the mover chunk is addressable by name" );

	// NOTE the argument shape: DocSetParamValue's third argument is the
	// param's OCCURRENCE index (for a repeatable role), NOT an index into
	// its value tokens, and the new value is re-tokenised WHOLE.  Passing
	// (…, 1, "4.5") asks for a SECOND `position` param, finds none, and
	// returns the document UNCHANGED -- after which
	// DeriveToJobIncremental still reports `applied > 0` (it re-applied an
	// identical chunk) and every check downstream passes against a scene
	// that never moved.
	Cst::Document docFar = Cst::DocSetParamValue( doc, moverId, "position", 0, "3 4.5 0" );
	std::vector<Cst::NodeId> closure = Cst::DocEditClosure( docFar, moverId );
	std::vector<std::string> d1;
	const int applied = Cst::DeriveToJobIncremental( docFar, *job, closure, &d1 );
	Check( applied > 0 && d1.empty(), "(a) the position edit applied (not refused)" );

	// TEETH FOR THE EDIT ITSELF.  `applied > 0` is NOT evidence that the
	// object moved -- a no-op document edit re-applies an identical chunk
	// and reports exactly the same thing (that is how the argument-shape
	// mistake above stayed invisible through four green checks).  So look
	// at the world bounding box.
	{
		IObjectPriv* mv = job->GetObjects()->GetItem( "mover" );
		Check( mv != 0, "(a) the mover object is still registered after the edit" );
		Check( mv && std::fabs( (double)mv->getBoundingBox().ll.y - 3.5 ) < 1e-9,
			"(a) TEETH -- the object's world bounding box really moved (its underside is now y = 3.5)" );
	}

	const double mid = RenderMean( *job );
	Check( mid > 0.0, "(a) the moved neighbour is still within the radius" );
	Check( mid < near_ * 0.9,
		"(a) MONEY -- the next render sees the move, with NO bake invalidated: the signal fell" );

	// TEETH FOR WHAT `mid` IS, not just that it fell.  Everything above is
	// a comparison, and a comparison cannot distinguish the floor's signal
	// from a box that wandered in front of the camera (see the note on the
	// pose, above).  So pin the NUMBER.
	//
	// THE CLOSED FORM.  The box is a 2x2x2 centred at (3, 4.5, 0), so it
	// occupies x in [2,4], y in [3.5,5.5], z in [-1,1].  For a floor point
	// (x, 0, z) with x < 2 and |z| <= 1 the nearest point of the box is
	// (2, 3.5, z), so d(x) = sqrt((2-x)^2 + 3.5^2) and the signal is
	// 1 - d/8.  At the patch CENTRE (the origin) that is
	// 1 - sqrt(4 + 12.25)/8 = 1 - 4.0311/8 = 0.49611.  The floor is
	// Lambertian with that value as its reflectance under a unit white
	// ambient, so the rendered channel value is reflectance/pi.
	//
	// THE TOLERANCE COMES FROM THE PATCH, not from the observed number.
	// `RenderMean` averages an 8x8 frame, and the camera at (0, 6, 0.0001)
	// with fov 12 has a half-angle of 6 degrees, so the footprint on the
	// floor is a square of half-width 6*tan(6 deg) = 0.6306 about the
	// origin.  Across that square the signal runs from 0.4526 (at
	// x = -0.63, the far corner) to 0.5302 (at x = +0.63), i.e. +/-0.039
	// about the centre value -- so the MEAN is within 0.039/pi = 0.0124 of
	// centre/pi.  Round that to 0.015 and the check is derived, not fitted.
	const double kPi        = 3.14159265358979323846;
	const double midClosed  = ( 1.0 - std::sqrt( 4.0 + 12.25 ) / 8.0 ) / kPi;	// 0.157915
	const double midTol     = 0.015;											// the 8x8 patch's own spread
	Check( std::fabs( mid - midClosed ) <= midTol,
		"(a) MONEY -- and `mid` IS the moved pose's closed-form proximity/pi, "
		"not some other quantity that happened to be smaller" );

	// AND THE DISCRIMINATOR THE TOLERANCE CANNOT CARRY.  The box's own
	// albedo, 0.5/pi = 0.159155, is unluckily close to this pose's answer
	// (0.157915) -- 0.8 % apart -- so the tolerance above admits both.
	// This check is what separates them, and it can be tight precisely
	// because the wrong answer is EXACT: when the box fills the frame every
	// pixel carries the same constant, so `mid` lands on 0.5/pi to within
	// rounding.  1e-4 is four orders above that and two below the 1.24e-3
	// gap to the right answer.
	//
	// RED-PROVED: restoring "0 4.5 0" above makes this check (and only
	// this one) fail, at |mid - 0.5/pi| < 1e-9.
	Check( std::fabs( mid - 0.5 / kPi ) > 1e-4,
		"(a) MONEY -- and it is NOT the box's own 0.5 Lambertian albedo: the neighbour "
		"stayed off to the side and the frame still shows the FLOOR" );

	// MOVE IT PAST THE RADIUS, to y = 21 (still at x = 3, same reason):
	// the nearest corner is then (2, 20, 0), 20.1 away, far beyond
	// `proximity(8)`, so the signal must reach its NEUTRAL 0 exactly --
	// not merely a small number.
	Cst::Document docGone = Cst::DocSetParamValue( docFar, moverId, "position", 0, "3 21 0" );
	std::vector<Cst::NodeId> closure2 = Cst::DocEditClosure( docGone, moverId );
	std::vector<std::string> d2;
	Cst::DeriveToJobIncremental( docGone, *job, closure2, &d2 );
	Check( d2.empty(), "(a) the second position edit applied" );

	const double gone = RenderMean( *job );
	Check( gone < mid * 0.05,
		"(a) MONEY -- moved beyond the radius, the signal reads its NEUTRAL: the floor goes dark" );

	std::cout << "    mean luminance: near " << near_ << " -> mid " << mid
		<< " -> beyond radius " << gone << std::endl;

	// AND BACK, which is the check that exercises the WORLD-AABB SNAPSHOT
	// rather than the query.  Moving a neighbour AWAY cannot be faked by a
	// stale cache -- the object's own DistanceToSurface reads the live
	// transform and reports the new, larger distance, which the caller
	// drops.  Moving it BACK can: the snapshot's cached box is what
	// decides whether a candidate is even asked, so a snapshot still
	// holding the FAR box would reject the returned neighbour at the box
	// test and this probe would stay dark.  It does not, so the snapshot
	// is genuinely rebuilt across the edit.
	std::vector<Cst::NodeId> closure3 = Cst::DocEditClosure( doc, moverId );
	std::vector<std::string> d3;
	Cst::DeriveToJobIncremental( doc, *job, closure3, &d3 );
	const double back = RenderMean( *job );
	Check( std::fabs( back - near_ ) < 1e-9,
		"(a) MONEY -- moving it BACK restores the original value exactly (the query is live, "
		"not a one-way cache)" );

	job->release();
}

//======================================================================
// (b) A KEYFRAMED neighbour, frame by frame
//======================================================================

static void TestKeyframedNeighbour()
{
	std::cout << "(b) a KEYFRAMED neighbour: each frame's value matches a fresh evaluation" << std::endl;

	// A timeline that lifts the box from y = 3 (distance 2.83 from the
	// probe point, signal 0.646) to y = 12 (distance 11.2, past the radius
	// 8, signal 0) over one second.  The intermediate times are chosen to
	// stay INSIDE the radius so the sequence falls STRICTLY rather than
	// saturating at the neutral after one step -- a saturated sequence
	// would make the monotonicity check pass on two equal zeros and prove
	// nothing.
	const char* timeline =
		"timeline\n{\nelement mover\nparam position\n"
		"time 0.0\nvalue 3 3 0\n"
		"time 1.0\nvalue 3 12 0\n}\n";

	Cst::Document doc = Cst::ParseToCst( SceneText( "3", timeline ) );
	Job* job = new Job();
	std::vector<std::string> d0;
	Cst::DeriveToJob( doc, *job, &d0 );
	Check( d0.empty(), "(b) the keyframed fixture derives cleanly" );

	IScenePriv* scene = job->GetScene();
	Check( scene != 0, "(b) the job has a scene" );
	if( !scene ) { job->release(); return; }

	// THE PER-FRAME SEQUENCE, which is exactly the body
	// PixelBasedRasterizerHelper::RasterizeSceneAnimation runs for each
	// frame: evaluate the animator, invalidate the spatial structure, and
	// prepare again.  Driven here rather than through RasterizeAnimation
	// itself because that entry point writes numbered files to disk and
	// owns its own frame loop; what is under test is the SEAM, and this is
	// the seam.
	struct Frame { Scalar t; const char* label; };
	const Frame frames[] = {
		{ Scalar( 0.0 ),  "t=0.0 (near)" },
		{ Scalar( 0.25 ), "t=0.25" },
		{ Scalar( 0.5 ),  "t=0.5" },
		{ Scalar( 1.0 ),  "t=1.0 (beyond the radius)" },
	};

	double prev = 1e30;
	for( int i = 0; i < 4; ++i ) {
		scene->GetAnimator()->EvaluateAtTime( frames[i].t );
		scene->GetObjects()->InvalidateSpatialStructure();
		scene->GetObjects()->PrepareForRendering();
		scene->SetSceneTime( frames[i].t );

		const double m = RenderMean( *job );
		std::cout << "    " << frames[i].label << ": mean luminance " << m << std::endl;

		// The box rises monotonically, so the signal must fall
		// monotonically.  A stale answer served across a frame boundary
		// would show up here as a plateau.
		Check( m < prev - 1e-9,
			std::string( "(b) MONEY -- " ) + frames[i].label +
			": the value fell STRICTLY from the previous frame (no stale answer crossed the seam)" );
		prev = m;
	}
	Check( prev < 1e-6,
		"(b) the last frame, with the neighbour far beyond the radius, reads the neutral 0" );

	// FRESH EVALUATION, the other half of the claim: re-deriving the whole
	// scene at the SAME time from scratch must give the SAME number, so
	// the animated path is not accumulating anything the fresh one does
	// not have.
	{
		Cst::Document doc2 = Cst::ParseToCst( SceneText( "3", timeline ) );
		Job* job2 = new Job();
		std::vector<std::string> dx;
		Cst::DeriveToJob( doc2, *job2, &dx );
		IScenePriv* s2 = job2->GetScene();
		Check( s2 != 0, "(b) the fresh job has a scene" );
		if( s2 ) {
			s2->GetAnimator()->EvaluateAtTime( Scalar( 0.5 ) );
			s2->GetObjects()->InvalidateSpatialStructure();
			s2->GetObjects()->PrepareForRendering();
			s2->SetSceneTime( Scalar( 0.5 ) );
			const double fresh = RenderMean( *job2 );

			// Drive the animated job back to t = 0.5 and compare.
			scene->GetAnimator()->EvaluateAtTime( Scalar( 0.5 ) );
			scene->GetObjects()->InvalidateSpatialStructure();
			scene->GetObjects()->PrepareForRendering();
			scene->SetSceneTime( Scalar( 0.5 ) );
			const double walked = RenderMean( *job );

			std::cout << "    t=0.5 fresh " << fresh << " vs walked-to " << walked << std::endl;
			Check( std::fabs( fresh - walked ) < 1e-9,
				"(b) MONEY -- a frame reached by WALKING the timeline equals the same frame "
				"evaluated FRESH (nothing accumulated across frames)" );
		}
		job2->release();
	}

	job->release();
}

//======================================================================
// (c) THE DEMAND GATE -- a scene that never says `proximity` pays nothing
//======================================================================

//! WHAT THIS PINS, and why an accessor rather than a timing check.
//!
//! The world-AABB snapshot is the one piece of per-pass state this signal
//! needs, and `ObjectManager::IntersectRay` has to keep it alive -- which,
//! ungated, is a load, a compare and a heap indirection on EVERY ray, in
//! every scene in the tree, including the overwhelming majority that never
//! mention the builtin.  Both that call and the eager build in
//! `PrepareForRendering` are now gated on `ProximityDemand::Any()`.
//!
//! The property is a NEGATIVE -- work NOT done -- so there is nothing to
//! measure without measuring time, and a timing assertion on a build this
//! small would be a flake.  `ForTest_HasBoxSnapshot()` is the honest
//! observation: after a full render, is there a snapshot at all?
//!
//! BOTH DIRECTIONS ARE CHECKED, because the "no" half alone would also pass
//! if the query were simply broken.  The same scene, same geometry, same
//! render -- only the painter's body differs.
static void TestDemandGate()
{
	std::cout << "(c) the snapshot is built only when something asks for proximity" << std::endl;

	// The expression is the ONLY difference between the two scenes.  The
	// `proximity`-free body still reads a signal-shaped context (`u`), so
	// it is a live ExpressionPainter running per hit -- what it does not do
	// is call the builtin.
	struct Local {
		static std::string Scene( const char* exprBody )
		{
			std::string s =
				"RISE ASCII SCENE 7\n"
				"standard_shader\n{\nname global\nshaderop DefaultDirectLighting\n}\n"
				"pixelpel_rasterizer\n{\nsamples 1\noidn_denoise FALSE\n}\n"
				"film\n{\nwidth 8\nheight 8\n}\n"
				"pinhole_camera\n{\nlocation 0 6 0.0001\nlookat 0 0 0\nup 0 1 0\nfov 12.0\n}\n"
				"ambient_light\n{\nname amb\ncolor 1.0 1.0 1.0\npower 1.0\n}\n"
				"expression_painter\n{\nname p_probe\nexpr ";
			s += exprBody;
			s +=
				"\n}\n"
				"lambertian_material\n{\nname m_probe\nreflectance p_probe\n}\n"
				"uniformcolor_painter\n{\nname p_plain\ncolor 0.5 0.5 0.5\n}\n"
				"lambertian_material\n{\nname m_plain\nreflectance p_plain\n}\n"
				"infiniteplane_geometry\n{\nname g_plane\nxtile 1.0\nytile 1.0\n}\n"
				"box_geometry\n{\nname g_box\nwidth 2.0\nheight 2.0\ndepth 2.0\n}\n"
				"standard_object\n{\nname floor\ngeometry g_plane\nmaterial m_probe\norientation -90 0 0\nposition 0 0 0\n}\n"
				"standard_object\n{\nname mover\ngeometry g_box\nmaterial m_plain\nposition 3 3 0\n}\n";
			return s;
		}

		//! Derive, render once, and report whether a snapshot exists after.
		//! Returns -1 if the scene did not derive or the manager could not
		//! be reached, so a plumbing failure cannot masquerade as a pass.
		static int RenderAndProbeSnapshot( const std::string& text )
		{
			Cst::Document doc = Cst::ParseToCst( text );
			Job* job = new Job();
			std::vector<std::string> d;
			Cst::DeriveToJob( doc, *job, &d );
			if( !d.empty() ) { job->release(); return -1; }

			(void)RenderMean( *job );

			// The accessor is ObjectManager's, not IObjectManager's: it is
			// test-only and does not belong on an engine interface.
			const ObjectManager* mgr =
				dynamic_cast<const ObjectManager*>( job->GetObjects() );
			const int r = mgr ? ( mgr->ForTest_HasBoxSnapshot() ? 1 : 0 ) : -1;
			job->release();
			return r;
		}
	};

	// THE PRECONDITION, asserted rather than assumed.  The demand counter
	// is PROCESS-WIDE and RAII-scoped to painter lifetime, so any painter
	// still alive from an earlier section would hold the gate open and turn
	// the "without" probe below into a silent false pass.  This is why the
	// section runs FIRST in main, ahead of (a) and (b), and why the state
	// it depends on is checked out loud instead of trusted.
	Check( !ProximityDemand::Any(),
		"(c) precondition -- no proximity consumer is live before this section runs" );

	// Both painters MUST also be destroyed before the NEXT scene is built,
	// for the same reason.  RenderAndProbeSnapshot releases its whole job,
	// which is what drops them.
	const int without = Local::RenderAndProbeSnapshot(
		Local::Scene( "vec3( u, u, u )" ) );
	Check( without == 0,
		"(c) MONEY -- a scene whose painter never calls proximity() builds NO "
		"world-AABB snapshot: the per-ray check and the eager build are both gated" );

	const int with = Local::RenderAndProbeSnapshot(
		Local::Scene( "vec3( proximity(8.0), proximity(8.0), proximity(8.0) )" ) );
	Check( with == 1,
		"(c) MONEY -- teeth: the SAME scene with proximity() in the body DOES build one, "
		"so the gate is a demand test and not a broken build path" );

	std::cout << "    snapshot after render: without proximity() = " << without
		<< ", with = " << with << "  (-1 would mean the probe itself failed)" << std::endl;
}

//======================================================================

int main()
{
	std::cout << "=== ProximityInvalidationTest (test F: nothing to invalidate) ===" << std::endl;

	// (c) RUNS FIRST, deliberately: its "no consumer, no snapshot" probe
	// needs the process-wide demand counter at zero, and (a) and (b) both
	// build proximity painters.  It asserts that precondition rather than
	// relying on this ordering alone.
	TestDemandGate();
	TestIncrementalMove();
	TestKeyframedNeighbour();

	std::cout << std::endl << "Passed: " << passCount << "   Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
