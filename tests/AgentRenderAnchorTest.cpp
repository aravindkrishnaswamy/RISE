//////////////////////////////////////////////////////////////////////
//
//  AgentRenderAnchorTest.cpp - doc 90 slice R1, THE ITERATION RATCHET.
//
//  docs/agentic-redesign/90-iteration-ratchet.md sec 1 records the failure
//  this slice answers: on the dragon probe, render 00 was the best picture
//  of the session and twelve self-directed edit rounds later the final
//  render was strictly worse.  The model judges only the CURRENT frame; so
//  nothing anchored best-so-far and every regression was permanent.
//
//  R1's answer is a PICTURE, never a number.  Sec 2's history note records
//  that the SCORED form of this same slice was falsified twice over --
//  Phase 2b had already shipped, measured and deleted such a score from
//  this very surface, and the underlying metric INVERTS in the operative
//  regime (deleting the hero object improves it).  So the assertions below
//  pin the absence of any similarity number as hard as they pin the
//  presence of the composite.
//
//  What this binary proves, in order:
//
//    1. BEFORE THE COMPOSE PHASE there is no anchor.  A full-frame
//       production render in the Pieces phase carries anchorApplied ==
//       false and an EMPTY composite -- absence pinned, not a zero.
//
//    2. THE FIRST post-compose render BECOMES the anchor: anchorApplied
//       true, anchorEstablished true, NO composite (nothing yet to set it
//       beside), anchorRevision == the revision it rendered at.
//
//    3. THE SECOND post-compose render CARRIES THE COMPOSITE, with BOTH
//       revisions correct -- parsed out of the result struct, out of the
//       RPC `anchor` block, AND out of the note text, never inferred from
//       "an image exists".
//
//    4. PIXELS.  The composite really CONTAINS both frames: every pixel of
//       the anchor pane matches the anchor render's own decoded PNG, and
//       every pixel of the lower pane matches this render's, with the grey
//       rule between them.  An image-exists check would pass a blank
//       canvas; this cannot.  Red-proof against vacuity: the two renders
//       are asserted to DIFFER first (a recoloured albedo between them).
//
//    5. set_render_anchor RE-PINS to the most recent render, and the NEXT
//       render's composite names the new anchor revision.  The no-argument
//       shape is pinned too: pinning with no render yet is a plain ok:false
//       that changes nothing.
//
//    6. NO SIMILARITY NUMBER anywhere -- not in the struct, not in the RPC
//       block, not in the note text.  Phase 2b's law, restated for this
//       mechanism (and AgentRenderAsyncTest's own rmse-absence assertion is
//       the guard for the scene-target half; this file does not touch it).
//
//  Self-contained: an inline native-v7 scene (a lit sphere), OIDN off, no
//  RISE_MEDIA_PATH, headless (no controller), single-threaded.
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Agent/AgentSession.h"
#include "../src/Library/Agent/AgentRpc.h"
#include "../src/Library/Agent/Json.h"
#include "../src/Library/Cst/Cst.h"
#include "../src/Library/Interfaces/IRasterImageReader.h"
#include "../src/Library/Interfaces/IRasterImageWriter.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Color/Color.h"
#include "../src/Library/Utilities/Reference.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
	#include <process.h>
	#define getpid _getpid
#else
	#include <unistd.h>
#endif

using namespace RISE;
using namespace RISE::Agent;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const std::string& w )
{
	if( c ) ++g_pass;
	else { ++g_fail; std::printf( "  FAIL: %s\n", w.c_str() ); }
}

// A small, self-contained native-v7 scene: a lit diffuse sphere with an
// area emitter, path-traced at a low sample count with OIDN off.  Same
// body as AgentProposeRenderTest's, which is known to render non-black.
static const char* const kScene =
	"RISE ASCII SCENE 7\n"
	"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
	"pathtracing_pel_rasterizer\n{\n\tsamples 8\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
	"film\n{\n\twidth 24\n\theight 24\n}\n\n"
	"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40.0\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_albedo\n\tcolor 0.5 0.5 0.5\n}\n\n"
	"lambertian_material\n{\n\tname mat_diffuse\n\treflectance pnt_albedo\n}\n\n"
	"sphere_geometry\n{\n\tname sph\n\tradius 0.8\n}\n\n"
	"standard_object\n{\n\tname obj_sph\n\tgeometry sph\n\tmaterial mat_diffuse\n}\n\n"
	"uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1.0 1.0 1.0\n}\n\n"
	"lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale 30.0\n\tmaterial none\n}\n\n"
	"clippedplane_geometry\n{\n\tname quad_emit\n\tpta -0.6 0.6 3.5\n\tptb 0.6 0.6 3.5\n\tptc 0.6 -0.6 3.5\n\tptd -0.6 -0.6 3.5\n}\n\n"
	"standard_object\n{\n\tname obj_emit\n\tgeometry quad_emit\n\tmaterial mat_emit\n}\n";

// Per-process temp filename: nothing stops two copies of this binary from
// running at once (a developer running it by hand while the suite runs, or
// a repeat-run loop chasing a suspected flake), and a fixed name would let
// them clobber each other's scene file mid-load.
static std::string WriteTemp( const char* name, const std::string& text )
{
	const char* base = std::getenv( "TMPDIR" );
	std::string dir = base ? base : "/tmp";
	if( !dir.empty() && dir.back() != '/' ) dir += '/';
	std::string path = dir + std::to_string( (long)getpid() ) + "_" + name;
	std::ofstream f( path.c_str(), std::ios::binary );
	if( !f ) return std::string();
	f.write( text.data(), (std::streamsize)text.size() );
	f.close();
	return path;
}

typedef std::array<unsigned char, 3> Px;

struct Decoded
{
	unsigned int    w = 0, h = 0;
	std::vector<Px> px;
	const Px& at( unsigned int x, unsigned int y ) const { return px[ (std::size_t)y * w + x ]; }
};

// Decode PNG bytes into the raw STORED bytes.  Reading with
// eColorSpace_Rec709RGB_Linear makes the PNGReader do a bare byte/255 with
// NO transfer-function conversion, so round(v*255) recovers the exact byte
// that was written -- the same convention AgentSession's own
// DecodeReferencePngToRgb8_ uses, which is what makes a byte-for-byte
// comparison between a source render and a composite pane meaningful.
static bool DecodePng( const std::vector<unsigned char>& png, Decoded& out )
{
	if( png.empty() ) return false;
	Implementation::MemoryBuffer* buf = new Implementation::MemoryBuffer(
		const_cast<char*>( reinterpret_cast<const char*>( png.data() ) ),
		(unsigned int)png.size(), /*bTakeOwnership*/false );
	IRasterImageReader* reader = nullptr;
	if( !RISE_API_CreatePNGReader( &reader, *buf, eColorSpace_Rec709RGB_Linear ) || !reader ) {
		safe_release( buf );
		return false;
	}
	unsigned int w = 0, h = 0;
	if( !reader->BeginRead( w, h ) ) { safe_release( reader ); safe_release( buf ); return false; }
	out.w = w; out.h = h;
	out.px.resize( (std::size_t)w * h );
	auto toB = []( double v ) -> unsigned char {
		int i = (int)( v * 255.0 + 0.5 );
		if( i < 0 ) i = 0;
		if( i > 255 ) i = 255;
		return (unsigned char)i;
	};
	for( unsigned int y = 0; y < h; ++y ) {
		for( unsigned int x = 0; x < w; ++x ) {
			RISEColor c;
			reader->ReadColor( c, x, y );
			Px p = { toB( c.base.r ), toB( c.base.g ), toB( c.base.b ) };
			out.px[ (std::size_t)y * w + x ] = p;
		}
	}
	reader->EndRead();
	safe_release( reader );
	safe_release( buf );
	return true;
}

// Fix-round P1: encode a solid-colour WxH PNG through RISE's OWN PNG writer,
// via the SAME eColorSpace_Rec709RGB_Linear "no gamma, byte in == byte out"
// convention AgentSession.cpp's EncodeLinearPassthroughPng_ uses and DecodePng
// above reads back -- so a round trip through this function is exact (mod the
// truncating-cast 1-LSB tolerance every other probe in this file already
// budgets for).  Used to mint a fake imagine_scene target: a SOLID colour,
// distinct from the render's own palette and from the composite's black/grey
// structural pixels, so a pixel probe against it cannot pass by accident.
static std::vector<unsigned char> EncodeSolidRgb8Png( unsigned char r, unsigned char g, unsigned char b,
                                                       unsigned int w, unsigned int h )
{
	std::vector<unsigned char> out;
	if( w == 0 || h == 0 ) return out;
	Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer();
	IRasterImageWriter* writer = nullptr;
	if( !RISE_API_CreatePNGWriter( &writer, *buffer, /*bpp=*/8, eColorSpace_Rec709RGB_Linear ) || !writer ) {
		safe_release( buffer );
		return out;
	}
	writer->BeginWrite( w, h );
	const RISEColor c( r / 255.0, g / 255.0, b / 255.0, 1.0 );
	for( unsigned int y = 0; y < h; ++y )
		for( unsigned int x = 0; x < w; ++x )
			writer->WriteColor( c, x, y );
	writer->EndWrite();
	const unsigned int nBytes = buffer->getCurPos();
	const char* p = buffer->Pointer();
	if( p && nBytes > 0 ) {
		out.assign( reinterpret_cast<const unsigned char*>( p ),
		            reinterpret_cast<const unsigned char*>( p ) + nBytes );
	}
	safe_release( writer );
	safe_release( buffer );
	return out;
}

// Same shape as AgentProposeRenderTest.cpp's MakeFakeImageGen (not shared --
// a different translation unit's static helper): a host-installed
// AgentImageGenerator whose `generate` always answers with the given PNG
// bytes, unconditionally of the description text.
static AgentSession::AgentImageGenerator MakeFakeImageGen( const std::vector<unsigned char>& png )
{
	AgentSession::AgentImageGenerator g;
	g.supported    = true;
	g.providerName = "test-provider";
	g.modelId      = "test-image-model";
	g.generate = [png]( const std::string& ) {
		AgentSession::AgentImageGenOutcome o;
		o.ok       = true;
		o.bytes    = png;
		o.mimeType = "image/png";
		return o;
	};
	return g;
}

// The composite's label strips are AnchorLabelHeight_(scale) = 7*scale rows
// tall and the rule is 2 rows.  The test derives `scale` from the composite
// geometry rather than hardcoding it, so a future change to the label size
// makes this test recompute instead of silently mis-probing.
struct CompositeLayout
{
	unsigned int labelH = 0;
	unsigned int ruleY  = 0;   //!< first row of the grey rule
	unsigned int paneY  = 0;   //!< first row of the ANCHOR pane
	unsigned int baseY  = 0;   //!< first row of the LOWER pane
	bool         ok     = false;
};

// compH = labelH + paneH + 2 + labelH + baseH, with paneH == baseH == tileH
// here (both frames come from the same film at the same imageMaxEdge, and
// imageMaxEdge is chosen above the film's long edge so neither is scaled).
static CompositeLayout SolveLayout( unsigned int compH, unsigned int tileH )
{
	CompositeLayout L;
	if( compH < 2 * tileH + 2 ) return L;
	const unsigned int labels = compH - 2 * tileH - 2;
	if( labels == 0 || ( labels % 2 ) != 0 ) return L;
	L.labelH = labels / 2;
	L.paneY  = L.labelH;
	L.ruleY  = L.labelH + tileH;
	L.baseY  = L.ruleY + 2 + L.labelH;
	L.ok     = true;
	return L;
}

// Fix-round P1: the THREE-WAY layout, for when the lower pane is ITSELF the
// scene-target composite -- label + ANCHOR pane + rule + label +
// [TARGET band + rule + RENDER tile].  Generalizes SolveLayout to unequal
// pane/base heights (the scene-target composite's own height, read straight
// off AgentRenderResult::sceneTargetCompositeHeight rather than assumed) and
// further decomposes the base region into its own two sub-bands.  `ok`
// requires the base height to actually equal bandH+2+tileH -- a sanity check
// on the caller's own inputs, not just an arithmetic solve.
struct ThreeWayLayout
{
	unsigned int labelH  = 0;
	unsigned int paneY   = 0;   //!< first row of the ANCHOR pane
	unsigned int ruleY   = 0;   //!< first row of the OUTER grey rule
	unsigned int label2Y = 0;   //!< first row of the second label strip
	unsigned int targetY = 0;   //!< first row of the TARGET band (base region start)
	unsigned int rule2Y  = 0;   //!< first row of the INNER grey rule
	unsigned int tileY   = 0;   //!< first row of the RENDER tile
	bool         ok      = false;
};

static ThreeWayLayout SolveThreeWayLayout( unsigned int compH, unsigned int paneH, unsigned int baseH,
                                            unsigned int bandH, unsigned int tileH )
{
	ThreeWayLayout L;
	if( baseH != bandH + 2u + tileH ) return L;   // the caller's own inputs must be self-consistent
	if( compH < paneH + 2u + baseH ) return L;
	const unsigned int labels = compH - paneH - 2u - baseH;
	if( labels == 0 || ( labels % 2 ) != 0 ) return L;
	L.labelH  = labels / 2;
	L.paneY   = L.labelH;
	L.ruleY   = L.paneY + paneH;
	L.label2Y = L.ruleY + 2u;
	L.targetY = L.label2Y + L.labelH;
	L.rule2Y  = L.targetY + bandH;
	L.tileY   = L.rule2Y + 2u;
	L.ok      = true;
	return L;
}

// Every field of the anchor block, in every surface, checked against the
// vocabulary of a similarity metric.  Phase 2b's law is that the composite
// IS the comparison; doc 90 sec 2 records that the scored form of THIS
// slice was falsified.  A future hand must not be able to reintroduce one
// under a different name.
static const char* const kBannedScoreNames[] = {
	"rmse", "score", "similarity", "difference", "delta", "distance",
	"match", "improvement", "percent", "pct", "mse", "psnr", "ssim",
	// Fix-round P2-4: the reviewer's 8, extending the sweep to cover
	// every other spelling a similarity number could hide under.
	"confidence", "agreement", "closeness", "iou", "cosine", "l2",
	"variance", "deviation"
};

//! Fix-round P2-4: CheckNoScoreFields above is an ENUMERATION -- it can
//! only catch a banned name it was told to look for, so a future field
//! added under some 21st name would sail through silently.  This makes
//! the wire `anchor` block's key set STRUCTURAL instead: pin it EXACTLY
//! (AgentObjectMapTest's kPlainKeys pattern), so ANY new field -- metric-
//! shaped or not -- fails this assertion and must be consciously added
//! here, read, and judged against Phase 2b's law rather than merely
//! missing the enumerated-name sweep.  Two shapes, because the block
//! itself has two: `composite:false` (the establishing render, nothing
//! yet to set beside) omits compositeWidth/compositeHeight entirely.
static void CheckAnchorBlockExactKeys( const JsonValue& an, bool haveComposite,
                                        const char* where )
{
	std::vector<std::string> want = { "anchored", "established", "anchorRevision",
	                                   "currentRevision", "composite" };
	if( haveComposite ) {
		want.push_back( "compositeWidth" );
		want.push_back( "compositeHeight" );
	}
	bool allPresent = true;
	for( const std::string& k : want ) if( !an.has( k.c_str() ) ) allPresent = false;
	Check( allPresent && an.members().size() == want.size(),
	       std::string( "MONEY ASSERTION (structural, fix-round P2-4): " ) + where +
	       " carries EXACTLY the " + std::to_string( want.size() ) +
	       " known anchor-block keys (got " + std::to_string( an.members().size() ) +
	       ") -- a new field of ANY name, not just an enumerated banned one, fails this" );
}

static void CheckNoScoreFields( const JsonValue& block, const char* where )
{
	for( const char* banned : kBannedScoreNames ) {
		Check( !block.has( banned ),
		       std::string( "MONEY ASSERTION (Phase 2b's law, doc 90 sec 2): the " ) + where +
		       " carries no `" + banned + "` -- no similarity number may be added to this "
		       "mechanism under any name; the two pictures ARE the comparison" );
	}
}

static AgentRenderParams FullFrameParams( unsigned int imageMaxEdge )
{
	AgentRenderParams p;
	p.imageMaxEdge     = imageMaxEdge;
	p.fromAgentSurface = true;
	return p;
}

//======================================================================
// 4d (2026-08-24, THE LIT MATERIAL LOOK) helpers.
//
// Every probe below reads LUMINANCE off a decoded 256-square and reduces
// it to ONE robust statistic.  Deliberately statistics, not byte
// comparisons: the material look is a real path-traced frame, and RISE's
// PT workers each seed their own RandomNumberGenerator (see
// RasterizeDispatchers.h's DoWork), so two runs of the same render differ
// by a Monte-Carlo noise floor -- measured at max 13/255 per pixel, mean
// 0.09/255, at this pipeline's fixed 64 spp with OIDN on.  A statistic
// whose margin is an order of magnitude wider than that floor is a pin;
// a byte comparison would be a flake.
//======================================================================

static double Luma( const Px& p )
{
	return ( 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2] ) / 255.0;
}

//! The four frame CORNERS, in decode order.  The isolate auto-frame puts
//! the object in the middle at 85% fill, so a corner is always background
//! -- which is exactly the pixel a draft frame and a material look
//! disagree about most sharply (see 4d-A).
static std::vector<double> CornerLumas( const Decoded& d )
{
	std::vector<double> out;
	if( d.w < 2 || d.h < 2 ) return out;
	out.push_back( Luma( d.at( 0, 0 ) ) );
	out.push_back( Luma( d.at( d.w - 1, 0 ) ) );
	out.push_back( Luma( d.at( 0, d.h - 1 ) ) );
	out.push_back( Luma( d.at( d.w - 1, d.h - 1 ) ) );
	return out;
}

//! Pixel indices of a CORE DISC inside the object's silhouette, derived
//! from an OPAQUE DRAFT frame of the same isolate (draft backgrounds are
//! exactly black, so "not background" is unambiguous) and then shrunk to
//! 70% of the silhouette's radius.
//!
//! DERIVED, NOT HARDCODED: the isolate auto-framing is a solve over the
//! object's bounding box and the active camera's FOV, so a hardcoded disc
//! would silently start probing background the day either changes.
//!
//! SHRUNK, because the rim is where every probe would lie to us: a
//! silhouette edge pixel is part object and part background under any
//! filter, so at the rim a "the object is dark here" reading is
//! indistinguishable from "the background is dark here".  70% keeps the
//! probe strictly interior.
static std::vector<std::size_t> CoreDiscFromDraft( const Decoded& draft, double bgEps = 0.02 )
{
	std::vector<std::size_t> out;
	if( draft.w == 0 || draft.h == 0 ) return out;
	unsigned int minX = draft.w, maxX = 0, minY = draft.h, maxY = 0;
	bool any = false;
	for( unsigned int y = 0; y < draft.h; ++y ) {
		for( unsigned int x = 0; x < draft.w; ++x ) {
			if( Luma( draft.at( x, y ) ) > bgEps ) {
				any = true;
				if( x < minX ) minX = x;
				if( x > maxX ) maxX = x;
				if( y < minY ) minY = y;
				if( y > maxY ) maxY = y;
			}
		}
	}
	if( !any ) return out;
	const double cx = ( minX + maxX ) * 0.5;
	const double cy = ( minY + maxY ) * 0.5;
	const double spanX = static_cast<double>( maxX - minX );
	const double spanY = static_cast<double>( maxY - minY );
	const double r = 0.70 * ( ( spanX < spanY ) ? spanX : spanY ) * 0.5;
	if( !( r > 2.0 ) ) return out;
	for( unsigned int y = 0; y < draft.h; ++y ) {
		for( unsigned int x = 0; x < draft.w; ++x ) {
			const double dx = x - cx, dy = y - cy;
			if( dx*dx + dy*dy <= r*r ) out.push_back( (std::size_t)y * draft.w + x );
		}
	}
	return out;
}

//! The reductions every 4d probe is stated in.
struct CoreStats
{
	double median   = 0.0;
	double peak     = 0.0;   //!< the brightest core pixel
	double stdDev   = 0.0;
	double darkFrac = 0.0;   //!< fraction of core pixels below kDarkLuma
	double peakOverMedian() const { return peak / ( median > 1e-6 ? median : 1e-6 ); }
};

//! "Dark" means DARKER THAN THE RIG'S OWN DARK CHECK could ever leave an
//! opaque, rig-lit surface.  The rig floors a lit surface at its ambient
//! term; only background seen THROUGH the object gets below this.
static const double kDarkLuma = 0.15;

static CoreStats MeasureCore( const Decoded& img, const std::vector<std::size_t>& core )
{
	CoreStats s;
	if( core.empty() || img.px.size() < core.back() + 1 ) return s;
	std::vector<double> v;
	v.reserve( core.size() );
	for( std::size_t i : core ) v.push_back( Luma( img.px[i] ) );
	std::vector<double> sorted = v;
	std::sort( sorted.begin(), sorted.end() );
	s.median = sorted[ sorted.size() / 2 ];
	s.peak   = sorted.back();
	double mean = 0.0;
	for( double x : v ) mean += x;
	mean /= (double)v.size();
	double acc = 0.0;
	std::size_t dark = 0;
	for( double x : v ) {
		acc += ( x - mean ) * ( x - mean );
		if( x < kDarkLuma ) ++dark;
	}
	s.stdDev   = std::sqrt( acc / (double)v.size() );
	s.darkFrac = (double)dark / (double)v.size();
	return s;
}

//! Mean absolute per-pixel luminance difference over the whole frame.
static double MeanAbsLuma( const Decoded& a, const Decoded& b )
{
	if( a.w != b.w || a.h != b.h || a.px.empty() ) return 1.0;
	double acc = 0.0;
	for( std::size_t i = 0; i < a.px.size(); ++i )
		acc += std::fabs( Luma( a.px[i] ) - Luma( b.px[i] ) );
	return acc / (double)a.px.size();
}

//! An isolate render of ONE object at the SAME size, framing and caps
//! FinishElement uses -- the only free variable is `q`.  That the two
//! qualities share every other input is what makes the 4d comparisons a
//! statement about FIDELITY rather than about framing.
static AgentRenderParams IsolateProbeParams( const char* object, AgentRenderQuality q )
{
	AgentRenderParams p;
	p.isolate          = object;
	p.fromAgentSurface = true;
	p.quality          = q;
	p.width            = kAgentSurfaceMaxRenderEdge;
	p.height           = kAgentSurfaceMaxRenderEdge;
	return p;
}

//----------------------------------------------------------------------
int main()
{
	std::printf( "=== AgentRenderAnchorTest (doc 90 slice R1: the iteration ratchet) ===\n" );

	// The phase machinery must be IN FORCE for this binary: the anchor's
	// default definition is "the first qualifying render at or after the
	// session enters Compose", and a session with the protocol off arms the
	// ratchet immediately instead (which the last block below covers
	// separately).  Both switches, because BuildProtocolActive_ needs both.
	AgentSession::SetBuildProtocolDefaultEnabled( true );
	AgentSession::SetBuildPlanGateDefaultEnabled( true );

	const std::string scenePath = WriteTemp( "rise_agent_anchor.RISEscene", kScene );
	Check( !scenePath.empty(), "wrote the scene to a temp file" );
	if( scenePath.empty() ) return 1;

	// imageMaxEdge ABOVE the film's long edge (24) on purpose: the
	// never-upscale rule then leaves each pane at the render's own dims, so
	// the pixel comparison below is byte-for-byte rather than through a
	// resampling filter.
	const unsigned int kMaxEdge = 64;

	std::unique_ptr<AgentSession> session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "AgentSession::LoadFromFile loads the native-v7 scene" );
	if( !session ) { std::remove( scenePath.c_str() ); return 1; }

	//------------------------------------------------------------------
	// 1. BEFORE COMPOSE: no anchor at all.
	//------------------------------------------------------------------
	std::printf( "[1] before the compose phase: renders carry no anchor\n" );
	{
		std::vector<AgentSession::AgentBuildPlanEntry> plan;
		AgentSession::AgentBuildPlanEntry e1;
		e1.element = "body";
		e1.pieces.push_back( "shell" );
		e1.construction.push_back( "primitive" );
		e1.outline = "0 0; 1 0; 1 1; 0 1";
		plan.push_back( e1 );
		AgentSession::AgentBuildPlanEntry e2;
		e2.element = "base";
		e2.pieces.push_back( "slab" );
		e2.construction.push_back( "primitive" );
		e2.outline = "0 0; 1 0; 1 1; 0 1";
		plan.push_back( e2 );

		const AgentSession::AgentBuildPlanResult pr = session->FileBuildPlan( plan );
		Check( pr.ok, "a two-element build plan is filed" );
		Check( session->BuildProtocolActive(), "the phase machinery is in force for this session" );
		Check( session->BuildPhase() == AgentSession::AgentBuildPhase::Pieces,
		       "the session is in the PIECES phase (one element active)" );

		const AgentRenderResult rr = session->Render( FullFrameParams( kMaxEdge ) );
		Check( rr.ok, std::string( "the pieces-phase render succeeded: " ) + rr.message );
		Check( !rr.png.empty(), "the pieces-phase render produced pixels (the check is not vacuous)" );
		// ABSENCE PINNED, not a zero: the block is not emitted at all, so a
		// caller cannot read "anchorRevision 0" as "anchored at revision 0".
		Check( !rr.anchorApplied,
		       "MONEY ASSERTION: a render BEFORE the compose phase carries NO anchor block -- the "
		       "anchor is the first render of the WHOLE scene, and a look at parts being built is "
		       "not one" );
		Check( !rr.anchorEstablished, "and nothing was established by it" );
		Check( rr.anchorCompositePng.empty(), "and it carries no anchor composite" );
		Check( !session->HasRenderAnchor(), "and the session still has no anchor pinned" );

		// The no-render-yet arm of the verb, which is reachable exactly here:
		// a qualifying render HAS happened, so this must SUCCEED -- the
		// ratchet's latest-frame bookkeeping runs even before the anchor is
		// armed, which is what lets a model pin early if it wants to.
		const AgentSession::AgentSetRenderAnchorResult early = session->SetRenderAnchor();
		Check( early.ok,
		       "set_render_anchor pins the pieces-phase render when the model explicitly asks -- the "
		       "AUTOMATIC anchor waits for compose, an explicit one does not" );
		Check( session->HasRenderAnchor(), "and the session now has an anchor" );
	}

	// That early pin would contaminate the default-anchor assertions below,
	// so the rest of the binary runs on a FRESH session over the same scene.
	session.reset();
	session = AgentSession::LoadFromFile( scenePath );
	Check( session != nullptr, "a fresh session reloads the scene for the default-anchor block" );
	if( !session ) { std::remove( scenePath.c_str() ); return 1; }

	//------------------------------------------------------------------
	// 2 + 3 + 4. The default anchor, the composite, and its pixels.
	//------------------------------------------------------------------
	std::printf( "[2] the first post-compose render becomes the anchor\n" );

	std::uint64_t rev1 = 0, rev2 = 0, rev3 = 0;
	std::vector<unsigned char> png1, png2;
	unsigned int tileW = 0, tileH = 0;
	{
		std::vector<AgentSession::AgentBuildPlanEntry> plan;
		AgentSession::AgentBuildPlanEntry e1;
		e1.element = "body";
		e1.pieces.push_back( "shell" );
		e1.construction.push_back( "primitive" );
		e1.outline = "0 0; 1 0; 1 1; 0 1";
		plan.push_back( e1 );
		Check( session->FileBuildPlan( plan ).ok, "a one-element build plan is filed" );

		const AgentSession::AgentFinishElementResult fe = session->FinishElement();
		Check( fe.ok, std::string( "finish_element closes the only element: " ) + fe.message );
		Check( session->BuildPhase() == AgentSession::AgentBuildPhase::Compose,
		       "finishing the LAST element enters the COMPOSE phase -- the moment the anchor arms" );

		rev1 = session->ReadHeadVersion().revision;
		const AgentRenderResult r1 = session->Render( FullFrameParams( kMaxEdge ) );
		Check( r1.ok, std::string( "the first compose-phase render succeeded: " ) + r1.message );
		Check( r1.anchorApplied, "MONEY ASSERTION: the FIRST post-compose render is anchored" );
		Check( r1.anchorEstablished,
		       "MONEY ASSERTION: and it is the render that BECAME the anchor" );
		Check( r1.anchorRevision == rev1,
		       "the anchor's revision is the head revision this render was made at" );
		Check( r1.anchorCurrentRevision == rev1, "which is also this render's own revision" );
		Check( r1.anchorCompositePng.empty(),
		       "MONEY ASSERTION: the establishing render carries NO composite -- there is nothing "
		       "yet to set it beside, and an image of one frame twice would be a lie about what "
		       "the mechanism is showing" );
		Check( session->HasRenderAnchor() && session->RenderAnchorRevision() == rev1,
		       "the session now holds the anchor at that revision" );
		png1  = r1.png;
		tileW = r1.width;
		tileH = r1.height;
		Check( tileW == 24 && tileH == 24, "the render is at the film's 24x24 dims" );
	}

	std::printf( "[3] the second render carries the composite, both revisions correct\n" );
	{
		// A VISIBLE edit between the two renders: it bumps the head revision
		// (so the two labels differ) AND changes the pixels (so the pane
		// comparison below cannot pass vacuously on two identical frames).
		AgentSetPatch sp;
		sp.target = "pnt_albedo";
		sp.param  = "color";
		sp.value  = "0.95 0.05 0.05";
		Check( session->ProposePatch( sp ).applied, "the albedo recolour applied" );

		rev2 = session->ReadHeadVersion().revision;
		Check( rev2 > rev1, "the edit bumped the head revision (the two labels will differ)" );

		const AgentRenderResult r2 = session->Render( FullFrameParams( kMaxEdge ) );
		Check( r2.ok, std::string( "the second compose-phase render succeeded: " ) + r2.message );
		Check( r2.anchorApplied, "the second render is anchored" );
		Check( !r2.anchorEstablished, "and it did NOT become the anchor (one is already pinned)" );
		Check( r2.anchorRevision == rev1,
		       "MONEY ASSERTION: the composite names the ANCHOR's revision (the earlier render), "
		       "parsed out of the result rather than inferred from the presence of an image" );
		Check( r2.anchorCurrentRevision == rev2,
		       "MONEY ASSERTION: and THIS render's own revision, which the edit above moved" );
		Check( !r2.anchorCompositePng.empty(), "the composite was built" );
		Check( r2.anchorCompositeWidth == tileW,
		       "the composite is exactly as wide as the frame -- Phase 2b's size contract: the "
		       "render is never shown smaller than it would have been" );
		png2 = r2.png;

		// The note text names BOTH revisions in prose, because that is what
		// a model actually reads.  Parsed, not eyeballed.
		const std::string want1 = "revision " + std::to_string( (unsigned long long)rev1 );
		const std::string want2 = "revision " + std::to_string( (unsigned long long)rev2 );
		Check( r2.message.find( "(anchor:" ) != std::string::npos,
		       "the render note carries an anchor sentence" );
		Check( r2.message.find( want1 ) != std::string::npos,
		       "the note names the anchor's revision (" + want1 + ")" );
		Check( r2.message.find( want2 ) != std::string::npos,
		       "the note names this render's revision (" + want2 + ")" );
		Check( r2.message.find( "set_render_anchor" ) != std::string::npos,
		       "and it names the ONE action that follows -- keeping this render if it is better" );
		// DELIBERATELY INVERTED, doc 90 slice R2 (2026-08-23).  This assertion
		// pinned the ABSENCE of `revert_to_revision` from the note for exactly
		// as long as the verb did not exist -- doc 90 sec 2's rule that a stale
		// promise burns the repair budget.  R2 landed the verb, so the same
		// rule now demands the opposite: the note must name it, AND must name
		// the anchor's own revision as the argument, because a model told "you
		// can go back" without the number spends a turn finding it.  The
		// absence half of the rule did not disappear -- it moved to the guard
		// this assertion's sibling in AgentRevertRevisionTest pins: the
		// sentence appears only when the session can really restore that
		// revision.
		Check( r2.message.find( "revert_to_revision" ) != std::string::npos,
		       "MONEY ASSERTION: the note NAMES revert_to_revision -- slice R2 landed the verb, so "
		       "withholding it would now be the stale promise doc 90 sec 2 warns about, in reverse" );
		Check( r2.message.find( "revert_to_revision " + std::to_string( (unsigned long long)rev1 ) ) !=
		           std::string::npos,
		       "MONEY ASSERTION: and it names the ANCHOR's revision as the argument (revert_to_revision " +
		           std::to_string( (unsigned long long)rev1 ) + "), not this render's -- going back means "
		           "going back to the anchor" );
		// Phase 2b's law in the prose too: no verdict, no characterization.
		Check( r2.message.find( "better" ) != std::string::npos,
		       "the note hands the judgment to the model (\"if this render is the better one\")" );
		Check( r2.message.find( "closer" ) == std::string::npos &&
		       r2.message.find( "worse than" ) == std::string::npos,
		       "and it never characterizes the render itself" );

		//--------------------------------------------------------------
		// 4. PIXELS.  An image-exists check would pass a blank composite.
		//--------------------------------------------------------------
		std::printf( "[4] the composite really contains BOTH frames (pixel probe)\n" );
		Decoded comp, d1, d2;
		Check( DecodePng( r2.anchorCompositePng, comp ), "the composite PNG decodes" );
		Check( DecodePng( png1, d1 ), "the anchor render's own PNG decodes" );
		Check( DecodePng( png2, d2 ), "this render's own PNG decodes" );

		// RED-PROOF AGAINST VACUITY: if the two renders were identical, every
		// probe below would pass no matter which frame the composite carried.
		bool framesDiffer = false;
		if( d1.w == d2.w && d1.h == d2.h ) {
			for( unsigned int y = 0; y < d1.h && !framesDiffer; ++y )
				for( unsigned int x = 0; x < d1.w; ++x )
					if( d1.at( x, y ) != d2.at( x, y ) ) { framesDiffer = true; break; }
		}
		Check( framesDiffer,
		       "the two renders DIFFER (the recoloured albedo landed) -- without this the pane "
		       "probes below would pass on any composite carrying either frame twice" );

		const CompositeLayout L = SolveLayout( comp.h, tileH );
		Check( L.ok, "the composite's height decomposes into label + pane + rule + label + pane" );
		Check( comp.w == tileW, "the composite is one tile wide" );
		if( L.ok && comp.w == tileW && d1.w == tileW && d2.w == tileW ) {
			// TOLERANCE OF ONE LSB, and it is the shared composite
			// pipeline's, not this mechanism's: the linear-passthrough PNG
			// WRITER quantizes through Integerize's TRUNCATING cast (see
			// AgentSession.cpp's EncodeLinearPassthroughPng_ and the warning
			// on DecodeReferencePngToRgb8_), so a byte that decodes to
			// b/255.0 can re-encode as b-1.  The scene-target composite has
			// carried exactly this property since Phase 2b.  Anything larger
			// than one LSB is a real defect, and the cross-probe below is
			// what keeps a 1-LSB tolerance from hiding a wrong-pane blit.
			auto within1 = []( const Px& a, const Px& b ) {
				for( int i = 0; i < 3; ++i ) {
					const int d = (int)a[i] - (int)b[i];
					if( d > 1 || d < -1 ) return false;
				}
				return true;
			};
			std::size_t anchorMismatch = 0, baseMismatch = 0;
			std::size_t anchorVsOther = 0, baseVsOther = 0;
			for( unsigned int y = 0; y < tileH; ++y ) {
				for( unsigned int x = 0; x < tileW; ++x ) {
					const Px& a = comp.at( x, L.paneY + y );
					const Px& b = comp.at( x, L.baseY + y );
					if( !within1( a, d1.at( x, y ) ) ) ++anchorMismatch;
					if( !within1( b, d2.at( x, y ) ) ) ++baseMismatch;
					if( !within1( a, d2.at( x, y ) ) ) ++anchorVsOther;
					if( !within1( b, d1.at( x, y ) ) ) ++baseVsOther;
				}
			}
			Check( anchorMismatch == 0,
			       "MONEY ASSERTION: EVERY pixel of the upper pane is the ANCHOR render's own "
			       "pixel (" + std::to_string( (unsigned long long)anchorMismatch ) +
			       " pixels off by more than one LSB)" );
			Check( baseMismatch == 0,
			       "MONEY ASSERTION: EVERY pixel of the lower pane is THIS render's own pixel (" +
			       std::to_string( (unsigned long long)baseMismatch ) +
			       " pixels off by more than one LSB)" );
			// THE CROSS-PROBE: each pane matches its OWN frame and not the
			// other one.  Without this, a composite that carried the same
			// frame twice would satisfy both checks above.
			Check( anchorVsOther > 0,
			       "MONEY ASSERTION: the upper pane does NOT match this render -- it really is the "
			       "EARLIER frame, not a second copy of the current one" );
			Check( baseVsOther > 0,
			       "MONEY ASSERTION: and the lower pane does NOT match the anchor -- the two panes "
			       "carry two different renders" );

			// The grey rule between them, and the dark label ground above
			// each pane -- the two structural elements that make the stack
			// readable as two frames rather than one tall image.
			const Px& rule = comp.at( tileW / 2, L.ruleY );
			Check( rule[0] == 128 && rule[1] == 128 && rule[2] == 128,
			       "a grey rule separates the two panes" );
			bool sawLabelInk = false;
			for( unsigned int y = 0; y < L.labelH && !sawLabelInk; ++y )
				for( unsigned int x = 0; x < tileW; ++x )
					if( comp.at( x, y )[0] > 200 ) { sawLabelInk = true; break; }
			Check( sawLabelInk,
			       "the anchor pane's label strip carries drawn text (light ink on a dark bar), so "
			       "the picture itself says which frame is which" );
		}
	}

	//------------------------------------------------------------------
	// 4b. Fix-round (P2-1): a VIEW-MODE render (objectmap) must not
	//     establish or update the anchor.  RenderQualifiesForAnchor_
	//     refuses anything but `params.renderTarget == Beauty` (see
	//     AgentSession.cpp:21801) -- objectmap paints identity colours, not
	//     appearance, so setting it beside a lit anchor render would show a
	//     difference that is really a render-mode setting.  This session
	//     already has an anchor pinned at rev1 (section 2), so the failure
	//     mode this guards is narrower than "never anchors" -- it is
	//     "never DISTURBS an anchor that already exists" either.
	//------------------------------------------------------------------
	std::printf( "[4b] a view-mode (objectmap) render neither establishes nor updates the anchor\n" );
	{
		AgentRenderParams op   = FullFrameParams( kMaxEdge );
		op.renderTarget        = AgentRenderTarget::ObjectMap;
		const AgentRenderResult orr = session->Render( op );
		Check( orr.ok, std::string( "the objectmap render itself succeeded: " ) + orr.message );
		Check( !orr.anchorApplied,
		       "MONEY ASSERTION: an OBJECTMAP render carries NO anchor block -- it paints identity "
		       "colours, not appearance, and a difference against a lit anchor render would be a "
		       "render-mode setting rather than a change to the scene" );
		Check( !orr.anchorEstablished, "and nothing was established by it" );
		Check( orr.anchorCompositePng.empty(), "and it carries no anchor composite" );
		Check( session->HasRenderAnchor() && session->RenderAnchorRevision() == rev1,
		       "MONEY ASSERTION: the session's PINNED anchor is still rev1, untouched by the "
		       "objectmap render -- a view-mode render must not silently re-point the bookmark "
		       "either" );
	}

	//------------------------------------------------------------------
	// 4c. finish_element's OWN isolate render never becomes the anchor
	//     (2026-08-23, the close-range element look).
	//
	//     This is the render most likely to be mistaken for "the first
	//     post-compose render", and by construction rather than by
	//     accident: finish_element on the LAST element advances the phase
	//     to COMPOSE -- the exact moment the ratchet arms -- and THEN
	//     renders the element it just closed.  A ratchet anchored on one
	//     part alone, at draft fidelity, would make every later
	//     comparison partly a comparison of render settings, and the
	//     model would read the whole scene appearing beside it as a
	//     regression.
	//
	//     TWO independent exclusions in RenderQualifiesForAnchor_ cover
	//     it (`isolate` and `quality == Draft`).  This block pins the
	//     OUTCOME rather than either exclusion, so removing one alone
	//     still passes here and removing BOTH fails -- which is exactly
	//     the honest statement: the guarantee is "not anchored", held up
	//     by two beams.
	//
	//     A FRESH session, because `session` above already has an anchor
	//     pinned: the assertion has to be "no anchor exists at all", not
	//     "the anchor did not move", or an isolate that ESTABLISHED one
	//     would slip through.
	//------------------------------------------------------------------
	std::printf( "[4c] finish_element's own isolate render never becomes the anchor\n" );
	{
		std::unique_ptr<AgentSession> iso = AgentSession::LoadFromFile( scenePath );
		Check( iso != nullptr, "4c: a fresh session loads the scene" );
		if( iso ) {
			std::vector<AgentSession::AgentBuildPlanEntry> plan;
			AgentSession::AgentBuildPlanEntry e1;
			e1.element = "body";
			e1.pieces.push_back( "shell" );
			e1.construction.push_back( "primitive" );
			e1.outline = "0 0; 1 0; 1 1; 0 1";
			plan.push_back( e1 );
			Check( iso->FileBuildPlan( plan ).ok, "4c: a one-element plan files" );

			// A WHOLE object -- geometry PLUS the standard_object that makes
			// it renderable.  Without one there is no isolate render at all
			// and every assertion below would pass vacuously.
			Check( iso->InsertChunk( "sphere_geometry\n{\n\tname body_sph\n\tradius 0.5\n}\n" ).applied,
			       "4c: the element's geometry inserts" );
			Check( iso->InsertChunk( "standard_object\n{\n\tname body_obj\n\tgeometry body_sph\n"
			                          "\tmaterial mat_diffuse\n}\n" ).applied,
			       "4c: the element's object inserts" );

			const AgentSession::AgentFinishElementResult fe = iso->FinishElement();
			Check( fe.ok, std::string( "4c: finish_element closes the only element: " ) + fe.message );
			Check( iso->BuildPhase() == AgentSession::AgentBuildPhase::Compose,
			       "4c: and the session is in COMPOSE -- the ratchet is armed from here on" );
			Check( fe.rendered && !fe.png.empty(),
			       "4c: the finish carried a REAL isolate render (so the assertion below is not vacuous)" );
			Check( !iso->HasRenderAnchor(),
			       "MONEY ASSERTION: finish_element's own isolate render did NOT establish the anchor, "
			       "even though it ran with the phase already advanced to COMPOSE" );

			// AND NOT VACUOUS FROM THE OTHER SIDE EITHER: the very next
			// full-frame render DOES establish the anchor, which proves the
			// session was genuinely armed while the isolate render ran.
			const AgentRenderResult ar = iso->Render( FullFrameParams( kMaxEdge ) );
			Check( ar.ok, std::string( "4c: the following full-frame render succeeded: " ) + ar.message );
			Check( ar.anchorEstablished && iso->HasRenderAnchor(),
			       "4c: and THAT render is the one that becomes the anchor -- so the session was armed "
			       "the whole time the isolate render was running" );
		}
	}

	//------------------------------------------------------------------
	// 4d. THE LIT MATERIAL LOOK (2026-08-24).
	//
	//     finish_element now takes TWO renders of its one isolate: the
	//     draft form look 4c covers, and a fixed-PT render of the SAME
	//     object under a canonical studio rig.  The reason is a measured
	//     one: the three worst appearance failures this project has seen
	//     -- glass rendered opaque, a membrane faked with emission, no
	//     specular at all -- are INVISIBLE in a draft frame, because
	//     draft shading answers a question about form.
	//
	//     Every assertion below is a STATISTIC over a decoded frame, with
	//     the margins stated.  See the 4d helper block above for why a
	//     byte comparison would be the wrong instrument (a real path
	//     tracer has a noise floor) and for how the probe disc is derived
	//     rather than hardcoded.
	//
	//     THREE PROBES, then the leak checks:
	//       A. THE RIG IS THERE, and the draft cannot fake it.  A draft
	//          background is exactly black -- its pipeline evaluates no
	//          environment at all -- while the material look's corners
	//          carry the rig's checker dome, and carry it UNEVENLY (the
	//          checker), so a flat fill could not pass either.
	//       B. SPECULAR.  A low-roughness GGX sphere's core peaks far
	//          above its own median under the rig; the diffuse sphere in
	//          the same frame geometry does not.  That ratio IS the
	//          roughness read.
	//       C. TRANSMISSION -- the money assertion.  The SAME geometry
	//          with a dielectric material versus a lambertian one: the
	//          dielectric's interior carries the dark half of the
	//          background refracted through it, the opaque one cannot.
	//          Pinned as a fraction of core pixels darker than any
	//          rig-lit opaque surface can be, so it is one-sided and
	//          cannot be satisfied by "the glass render is just darker".
	//------------------------------------------------------------------
	std::printf( "[4d] the lit material look: rig, specular, transmission, and no leak\n" );
	{
		std::unique_ptr<AgentSession> mat = AgentSession::LoadFromFile( scenePath );
		Check( mat != nullptr, "4d: a fresh session loads the scene" );
		if( mat ) {
			std::vector<AgentSession::AgentBuildPlanEntry> plan;
			AgentSession::AgentBuildPlanEntry e1;
			e1.element = "orbs";
			e1.pieces.push_back( "shell" );
			e1.construction.push_back( "primitive" );
			e1.outline = "0 0; 1 0; 1 1; 0 1";
			plan.push_back( e1 );
			Check( mat->FileBuildPlan( plan ).ok, "4d: a one-element plan files" );

			// THREE OBJECTS ON ONE GEOMETRY, at one position.  Sharing the
			// scene's own `sph` chunk is what makes probe C a statement about
			// MATERIALS: identical geometry means an identical world bounding
			// box, means an identical auto-framed camera, means the two frames
			// are pixel-aligned and the ONLY thing that differs between them
			// is the material.  (They overlap in world space, which is
			// irrelevant: `isolate` renders exactly one of them at a time.)
			Check( mat->InsertChunk( "uniformcolor_painter\n{\n\tname probe_spec\n\tcolor 0.9 0.9 0.9\n}\n" ).applied,
			       "4d: the specular painter inserts" );
			Check( mat->InsertChunk( "uniformcolor_painter\n{\n\tname probe_dark\n\tcolor 0.04 0.04 0.04\n}\n" ).applied,
			       "4d: the dark painter inserts" );
			Check( mat->InsertChunk( "dielectric_material\n{\n\tname probe_glass_mat\n\ttau 1 1 1\n"
			                          "\tior 1.5\n\tscattering 1000000\n}\n" ).applied,
			       "4d: the dielectric material inserts (scattering 1e6 = delta pass-through, "
			       "the idiom SCENE_CONVENTIONS records -- `scattering 0` would be maximally DIFFUSE "
			       "transmission, not clear glass)" );
			Check( mat->InsertChunk( "ggx_material\n{\n\tname probe_glossy_mat\n\trd probe_dark\n"
			                          "\trs probe_spec\n\talphax 0.08\n\talphay 0.08\n\tior 1.5\n"
			                          "\textinction 0\n\tfresnel_mode schlick_f0\n}\n" ).applied,
			       "4d: the glossy GGX material inserts" );
			Check( mat->InsertChunk( "standard_object\n{\n\tname probe_opaque\n\tgeometry sph\n"
			                          "\tmaterial mat_diffuse\n}\n" ).applied,
			       "4d: the opaque probe object inserts" );
			Check( mat->InsertChunk( "standard_object\n{\n\tname probe_glass\n\tgeometry sph\n"
			                          "\tmaterial probe_glass_mat\n}\n" ).applied,
			       "4d: the dielectric probe object inserts" );
			Check( mat->InsertChunk( "standard_object\n{\n\tname probe_glossy\n\tgeometry sph\n"
			                          "\tmaterial probe_glossy_mat\n}\n" ).applied,
			       "4d: the glossy probe object inserts" );

			auto shoot = [&]( const char* obj, AgentRenderQuality q, Decoded& out ) -> bool {
				const AgentRenderResult r = mat->Render( IsolateProbeParams( obj, q ) );
				if( !r.ok || r.png.empty() ) {
					Check( false, std::string( "4d: render of " ) + obj + " failed: " + r.message );
					return false;
				}
				return DecodePng( r.png, out );
			};

			Decoded opaqueDraft, opaqueMat, glassDraft, glassMat, glossyMat;
			const bool decoded =
				shoot( "probe_opaque", AgentRenderQuality::Draft,        opaqueDraft ) &&
				shoot( "probe_opaque", AgentRenderQuality::MaterialLook, opaqueMat )   &&
				shoot( "probe_glass",  AgentRenderQuality::Draft,        glassDraft )  &&
				shoot( "probe_glass",  AgentRenderQuality::MaterialLook, glassMat )    &&
				shoot( "probe_glossy", AgentRenderQuality::MaterialLook, glossyMat );
			Check( decoded, "4d: all five probe frames decode" );

			if( decoded ) {
				Check( opaqueDraft.w == kAgentSurfaceMaxRenderEdge &&
				       opaqueMat.w   == opaqueDraft.w && opaqueMat.h == opaqueDraft.h &&
				       glassMat.w    == opaqueDraft.w && glassMat.h  == opaqueDraft.h,
				       "4d: draft and material look render at the SAME dims -- the comparisons below "
				       "are between pixel-aligned frames, not between two framings" );

				const std::vector<std::size_t> core = CoreDiscFromDraft( opaqueDraft );
				Check( core.size() > 1000,
				       "4d: the core disc really covers the silhouette's interior (" +
				       std::to_string( core.size() ) + " px) -- a tiny or empty disc would make every "
				       "statistic below vacuously true" );

				// ---- A. THE RIG IS THERE, AND ONLY THE MATERIAL LOOK HAS IT.
				{
					const std::vector<double> dc = CornerLumas( opaqueDraft );
					const std::vector<double> mc = CornerLumas( opaqueMat );
					Check( dc.size() == 4 && mc.size() == 4, "4d-A: four corners each" );
					if( dc.size() == 4 && mc.size() == 4 ) {
						double dMax = 0.0, mMin = 1.0, mMax = 0.0;
						for( int i = 0; i < 4; ++i ) {
							if( dc[i] > dMax ) dMax = dc[i];
							if( mc[i] < mMin ) mMin = mc[i];
							if( mc[i] > mMax ) mMax = mc[i];
						}
						Check( dMax < 0.02,
						       "4d-A: the DRAFT frame's background is black -- its pipeline evaluates no "
						       "environment at all (measured 0.000)" );
						Check( mMax > 0.30,
						       "4d-A MONEY ASSERTION: the material look's background carries the rig's "
						       "environment dome, which a draft frame structurally cannot produce "
						       "(measured max corner 0.594, floor 0.30)" );
						Check( ( mMax - mMin ) > 0.20,
						       "4d-A: and it carries it UNEVENLY -- the dome is a two-tone checker, so a "
						       "flat grey fill (or a leaked scene environment) could not pass this "
						       "(measured spread 0.519, floor 0.20)" );
					}
				}

				// ---- B. SPECULAR / ROUGHNESS.
				{
					const CoreStats glossy = MeasureCore( glossyMat, core );
					const CoreStats diff   = MeasureCore( opaqueMat, core );
					Check( glossy.peakOverMedian() > 1.7,
					       "4d-B MONEY ASSERTION: a low-roughness GGX sphere PEAKS far above its own "
					       "median under the rig -- a specular highlight, and the tightness of it is "
					       "the roughness read (measured 2.21, floor 1.7)" );
					Check( diff.peakOverMedian() < 1.35,
					       "4d-B: and the LAMBERTIAN sphere in the same frame geometry does not, so the "
					       "ratio is measuring the material and not the rig (measured 1.12, ceiling 1.35)" );
					Check( glossy.peak > 0.85,
					       "4d-B: the highlight reaches near the top of the range rather than being a "
					       "faint gradient (measured 0.996, floor 0.85)" );
				}

				// ---- C. TRANSMISSION.  THE money assertion of this slice.
				{
					const CoreStats glassM  = MeasureCore( glassMat,   core );
					const CoreStats opaqueM = MeasureCore( opaqueMat,  core );
					const CoreStats glassD  = MeasureCore( glassDraft, core );

					Check( glassM.darkFrac > 0.15,
					       "4d-C MONEY ASSERTION: under the rig, a DIELECTRIC's interior carries the dark "
					       "half of the background refracted through it -- pixels darker than any rig-lit "
					       "opaque surface can be (measured 0.391 of the core, floor 0.15)" );
					Check( opaqueM.darkFrac < 0.03,
					       "4d-C MONEY ASSERTION: and the SAME GEOMETRY with a lambertian material has "
					       "none of them -- so the statistic is reading TRANSMISSION, not exposure "
					       "(measured 0.000, ceiling 0.03)" );
					Check( glassM.stdDev > 2.0 * opaqueM.stdDev,
					       "4d-C: the dielectric's interior also carries far more structure than the "
					       "opaque twin's smooth shading gradient (measured 0.210 vs 0.057)" );

					// AND THE WHOLE POINT, stated as its own assertion: the
					// DRAFT of that same dielectric is a nearly FLAT DISC.  It
					// is not that the draft shows transmission worse -- it
					// shows none, and it shows the object as opaque, which is
					// precisely the failure that shipped opaque "glass"
					// bottles past a draft-only review.
					Check( glassD.darkFrac < 0.03 && glassD.stdDev < 0.03,
					       "4d-C MONEY ASSERTION: the DRAFT frame of that same dielectric is a nearly "
					       "FLAT DISC -- no refracted background, almost no variation (measured darkFrac "
					       "0.000, stddev 0.007).  This is the entire reason the second render exists: "
					       "reviewing glass through a draft frame reviews an opaque ball" );
				}

				// ---- DETERMINISM.  Two material looks of one object, taken
				// in the same session with an unrelated render in between,
				// must agree -- stated on the ROBUST statistics plus a
				// whole-frame mean, never byte equality (see the helper
				// block's noise-floor note).
				{
					Decoded again;
					if( shoot( "probe_glass", AgentRenderQuality::MaterialLook, again ) ) {
						const CoreStats a = MeasureCore( glassMat, core );
						const CoreStats b = MeasureCore( again,    core );
						Check( MeanAbsLuma( glassMat, again ) < 0.02,
						       "4d: two material looks of one element agree over the whole frame "
						       "(measured mean |dLuma| ~0.0008, ceiling 0.02)" );
						Check( std::fabs( a.median - b.median ) < 0.03 &&
						       std::fabs( a.darkFrac - b.darkFrac ) < 0.03,
						       "4d MONEY ASSERTION: and agree on the statistics the model would be "
						       "comparing across iterations -- the rig, the framing, the fidelity and "
						       "the tone curve are all fixed, so only a Monte-Carlo noise floor moves" );
					}
				}
			}

			// ---- THE EPHEMERAL RIG DOES NOT LEAK.
			//
			// The rig replaces the SCENE's light manager and global radiance
			// map for the duration of one render.  Everything below is about
			// the state that survives it.
			{
				const std::string          docBefore = mat->ReadDocument();
				const RISE::Cst::CstHeadVersion verBefore = mat->ReadHeadVersion();
				const std::size_t          revsBefore = mat->RecordedRevisionCount();

				const AgentRenderResult r =
					mat->Render( IsolateProbeParams( "probe_opaque", AgentRenderQuality::MaterialLook ) );
				Check( r.ok, std::string( "4d-leak: the material look under test succeeded: " ) + r.message );

				Check( mat->ReadDocument() == docBefore,
				       "4d-leak MONEY ASSERTION: the DOCUMENT is byte-identical across a material-look "
				       "render -- the rig is Scene state, mutated and restored inside the render, and "
				       "never a chunk" );
				Check( mat->ReadHeadVersion() == verBefore,
				       "4d-leak MONEY ASSERTION: and the head version (uuid AND revision) did not move "
				       "-- no epoch bump" );
				Check( mat->RecordedRevisionCount() == revsBefore,
				       "4d-leak MONEY ASSERTION: and no revision was recorded -- nothing for "
				       "revert_to_revision to land on, i.e. no undo entry" );

				// THE SCENE'S OWN LIGHT SET, read back through the one public
				// surface that enumerates it: an UNRESOLVED `light` name fails
				// the render with the available names listed.  If any rig light
				// had survived the restore, its name would be in that list.
				AgentRenderParams probe = FullFrameParams( kMaxEdge );
				probe.light = "__rise_no_such_light__";
				const AgentRenderResult lr = mat->Render( probe );
				Check( !lr.ok,
				       "4d-leak: an unresolved `light` fails loudly and lists the scene's lights (the "
				       "probe this check is built on)" );
				Check( lr.message.find( "__rise_studio_" ) == std::string::npos,
				       "4d-leak MONEY ASSERTION: NONE of the rig's lights are in the scene's light list "
				       "afterwards -- the swapped-in light manager was put back, not merged" );
				Check( lr.message.find( "obj_emit" ) != std::string::npos ||
				       lr.message.find( "\"" ) != std::string::npos,
				       "4d-leak: and the list is a real one (not empty), so the absence above is a "
				       "restored scene rather than an unread one" );
			}

			// ---- AND A SUBSEQUENT WHOLE-SCENE RENDER IS UNAFFECTED.  The
			// scene is lit ONLY by its emissive quad and has no environment,
			// so its background is black; a leaked rig dome would paint that
			// background with the checker, which no tolerance could hide.
			{
				const AgentRenderResult after = mat->Render( FullFrameParams( kMaxEdge ) );
				Check( after.ok && !after.png.empty(),
				       "4d-after: a whole-scene render after the material look still succeeds" );
				Decoded d;
				if( !after.png.empty() && DecodePng( after.png, d ) ) {
					const std::vector<double> c = CornerLumas( d );
					double worst = 0.0;
					for( double v : c ) if( v > worst ) worst = v;
					Check( worst < 0.05,
					       "4d-after MONEY ASSERTION: the scene's background is still BLACK -- the rig's "
					       "environment dome did not survive the render that installed it" );
				}
			}

			// ---- AND IT NEVER BECOMES THE ANCHOR.  4c pins the outcome for
			// finish_element's pair; this pins the QUALITY's own exclusion,
			// which is the beam that would still hold if `isolate` were ever
			// dropped from the rule.
			{
				std::unique_ptr<AgentSession> anch = AgentSession::LoadFromFile( scenePath );
				Check( anch != nullptr, "4d-anchor: a fresh session loads" );
				if( anch ) {
					std::vector<AgentSession::AgentBuildPlanEntry> p2;
					AgentSession::AgentBuildPlanEntry e2;
					e2.element = "body";
					e2.pieces.push_back( "shell" );
					e2.construction.push_back( "primitive" );
					e2.outline = "0 0; 1 0; 1 1; 0 1";
					p2.push_back( e2 );
					Check( anch->FileBuildPlan( p2 ).ok, "4d-anchor: a one-element plan files" );
					Check( anch->FinishElement().ok, "4d-anchor: finishing it enters Compose" );
					Check( anch->BuildPhase() == AgentSession::AgentBuildPhase::Compose,
					       "4d-anchor: the ratchet is armed" );
					Check( !anch->HasRenderAnchor(),
					       "4d-anchor: nothing is anchored yet (the element had no object, so the finish "
					       "carried no render at all)" );
					// A MaterialLook render WITHOUT `isolate`, so the isolate
					// beam cannot be what holds this.
					AgentRenderParams full = FullFrameParams( kMaxEdge );
					full.quality = AgentRenderQuality::MaterialLook;
					full.width   = kAgentSurfaceMaxRenderEdge;
					full.height  = kAgentSurfaceMaxRenderEdge;
					const AgentRenderResult mr = anch->Render( full );
					Check( mr.ok, std::string( "4d-anchor: the un-isolated material look succeeded: " ) + mr.message );
					Check( mr.renderMode == "material",
					       "4d-anchor: and reports its own renderMode, distinct from production/draft" );
					Check( !mr.anchorEstablished && !anch->HasRenderAnchor(),
					       "4d-anchor MONEY ASSERTION: a material-look render never establishes the "
					       "anchor even with no `isolate` in play -- an anchor made of a rig-lit frame "
					       "would show every later render as a regression the moment the rig went away" );
					const AgentRenderResult pr2 = anch->Render( FullFrameParams( kMaxEdge ) );
					Check( pr2.anchorEstablished && anch->HasRenderAnchor(),
					       "4d-anchor: and the very next production render DOES anchor -- so the session "
					       "was armed the whole time" );
				}
			}
		}
	}

	//------------------------------------------------------------------
	// 5. The RPC surface: the `anchor` block and the composite as the
	//    call's one image.
	//------------------------------------------------------------------
	std::printf( "[5] the wire: the anchor block, and the composite as the call's image\n" );
	{
		// Fix-round P2-4, the OTHER shape: the ESTABLISHING render's wire
		// block carries composite:false and omits compositeWidth/
		// compositeHeight entirely -- a fresh throwaway session/dispatcher,
		// discarded right after, because `session` below already carries an
		// established anchor from [2] and its next wire render is the
		// composite:true shape.
		{
			std::unique_ptr<AgentSession> freshSession = AgentSession::LoadFromFile( scenePath );
			Check( freshSession != nullptr, "a fresh session loads for the composite:false wire shape" );
			if( freshSession ) {
				// The build-plan default is still ON at this point in the
				// binary (turned off only in [7], below) -- reach Compose
				// exactly like [2] does, or RenderAnchorArmed_ never arms and
				// this render carries no anchor block at all.
				std::vector<AgentSession::AgentBuildPlanEntry> freshPlan;
				AgentSession::AgentBuildPlanEntry fe1;
				fe1.element = "body";
				fe1.pieces.push_back( "shell" );
				fe1.construction.push_back( "primitive" );
				fe1.outline = "0 0; 1 0; 1 1; 0 1";
				freshPlan.push_back( fe1 );
				Check( freshSession->FileBuildPlan( freshPlan ).ok,
				       "a one-element build plan is filed for the fresh wire-shape session" );
				Check( freshSession->FinishElement().ok,
				       "finish_element enters Compose for the fresh wire-shape session" );
				AgentRpcDispatcher freshRpc( std::move( freshSession ), AgentAutonomy::Commit );
				std::string ferr;
				JsonValue fenv;
				Check( JsonParse( freshRpc.HandleLine(
					"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"render\",\"params\":{\"imageMaxEdge\":64}}" ),
					fenv, ferr ), "the establishing wire render response parses" );
				const JsonValue& fres = fenv.get( "result" );
				Check( fres.get( "ok" ).asBool(), "the establishing wire render succeeded" );
				const JsonValue& fan = fres.get( "anchor" );
				Check( fan.get( "established" ).asBool( false ), "established:true on this shape" );
				Check( !fan.get( "composite" ).asBool( true ), "composite:false (nothing yet to set it beside)" );
				CheckAnchorBlockExactKeys( fan, /*haveComposite=*/false,
				                           "wire `anchor` block (establishing, composite:false)" );
			}
		}

		AgentRpcDispatcher rpc( std::move( session ), AgentAutonomy::Commit );
		std::string err;

		JsonValue env;
		Check( JsonParse( rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"render\",\"params\":{\"imageMaxEdge\":64}}" ),
			env, err ), "the render response parses" );
		const JsonValue& res = env.get( "result" );
		Check( res.get( "ok" ).asBool(), "the wire render succeeded" );
		Check( res.has( "anchor" ), "the render result carries an `anchor` block" );
		const JsonValue& an = res.get( "anchor" );
		Check( an.get( "anchored" ).asBool(), "anchored:true" );
		Check( !an.get( "established" ).asBool( true ), "established:false (one was already pinned)" );
		Check( an.get( "anchorRevision" ).asNumber( -1.0 ) == (double)rev1,
		       "the block names the anchor's revision" );
		Check( an.get( "currentRevision" ).asNumber( -1.0 ) >= (double)rev2,
		       "and this render's own revision" );
		Check( an.get( "composite" ).asBool(), "composite:true" );
		CheckNoScoreFields( an, "wire `anchor` block" );
		CheckNoScoreFields( res, "render result itself" );
		CheckAnchorBlockExactKeys( an, /*haveComposite=*/true, "wire `anchor` block (composite:true)" );

		Check( res.has( "png_base64" ), "the call carries exactly one image" );
		Check( (unsigned int)res.get( "imageHeight" ).asNumber( 0.0 ) > 2u * tileH,
		       "MONEY ASSERTION: and that image is the COMPOSITE (taller than two frames), not the "
		       "plain frame -- the whole retention argument is that this is the moment the anchor "
		       "re-enters the model's context" );

		//--------------------------------------------------------------
		// 6. set_render_anchor RE-PINS, and the next render says so.
		//--------------------------------------------------------------
		std::printf( "[6] set_render_anchor re-pins; the next composite names the new anchor\n" );
		JsonValue pinEnv;
		Check( JsonParse( rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"set_render_anchor\",\"params\":{}}" ),
			pinEnv, err ), "the set_render_anchor response parses" );
		Check( !pinEnv.has( "error" ),
		       "set_render_anchor is a known method on the wire (registered, not -32601)" );
		const JsonValue& pr = pinEnv.get( "result" );
		Check( pr.get( "ok" ).asBool(), "the pin succeeded" );
		Check( pr.get( "pinned" ).asBool(), "pinned:true" );
		const double newAnchorRev = pr.get( "anchorRevision" ).asNumber( -1.0 );
		Check( newAnchorRev >= (double)rev2,
		       "the new anchor is the MOST RECENT render, not the old one" );
		Check( pr.get( "previousAnchorRevision" ).asNumber( -1.0 ) == (double)rev1,
		       "and the result names the anchor it replaced" );
		CheckNoScoreFields( pr, "set_render_anchor result" );

		JsonValue env3;
		Check( JsonParse( rpc.HandleLine(
			"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"render\",\"params\":{\"imageMaxEdge\":64}}" ),
			env3, err ), "the third render response parses" );
		const JsonValue& an3 = env3.get( "result" ).get( "anchor" );
		rev3 = (std::uint64_t)an3.get( "currentRevision" ).asNumber( 0.0 );
		Check( an3.get( "anchorRevision" ).asNumber( -1.0 ) == newAnchorRev,
		       "MONEY ASSERTION: the NEXT render's composite names the RE-PINNED anchor -- goes RED "
		       "the moment set_render_anchor stops actually moving the anchor" );
		Check( an3.get( "anchorRevision" ).asNumber( -1.0 ) != (double)rev1,
		       "and it is no longer the original anchor" );
		Check( rev3 > 0, "the third render reports its own revision" );
		Check( an3.get( "composite" ).asBool(), "and it still carries a composite" );
		CheckAnchorBlockExactKeys( an3, /*haveComposite=*/true, "wire `anchor` block (re-pinned, composite:true)" );
	}

	//------------------------------------------------------------------
	// 7. A session with the phase machinery OFF arms immediately.
	//------------------------------------------------------------------
	std::printf( "[7] with no build protocol, the first full-frame render is the anchor\n" );
	{
		AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<AgentSession> plain = AgentSession::LoadFromFile( scenePath );
		Check( plain != nullptr, "a protocol-off session loads" );
		if( plain ) {
			Check( !plain->BuildProtocolActive(), "the phase machinery is NOT in force" );
			const AgentRenderResult r = plain->Render( FullFrameParams( kMaxEdge ) );
			Check( r.ok && r.anchorApplied && r.anchorEstablished,
			       "MONEY ASSERTION: its FIRST full-frame render is the anchor -- a session that "
			       "never reaches Compose must still get a ratchet, or the mechanism is off for "
			       "exactly the configurations that get no staging discipline either" );

			// And the exclusions hold there too: a DRAFT render is neither
			// anchored nor compared, because a difference against one would
			// be a render setting rather than a change to the scene.
			AgentRenderParams dp = FullFrameParams( kMaxEdge );
			dp.quality = AgentRenderQuality::Draft;
			const AgentRenderResult dr = plain->Render( dp );
			Check( dr.ok, std::string( "the draft render succeeded: " ) + dr.message );
			Check( !dr.anchorApplied && dr.anchorCompositePng.empty(),
			       "a DRAFT render carries no anchor block and no composite" );
			Check( plain->RenderAnchorRevision() == r.anchorRevision,
			       "and it did not disturb the anchor the production render established" );

			// AND THE EXCLUSION THE SCENE TARGET DOES NOT MAKE: a render the
			// MODEL did not issue.  `Render(AgentRenderParams)` is also the
			// internal entry point CompareToReference's grading pass uses,
			// and that pass is a full-frame production beauty render by every
			// other test -- so without this exclusion, scoring a candidate
			// against a host-registered reference would silently re-point the
			// model's bookmark at a frame it never saw.
			AgentRenderParams hostParams = FullFrameParams( kMaxEdge );
			hostParams.fromAgentSurface = false;
			const AgentRenderResult hr = plain->Render( hostParams );
			Check( hr.ok, std::string( "the host-issued render succeeded: " ) + hr.message );
			Check( !hr.anchorApplied && hr.anchorCompositePng.empty(),
			       "MONEY ASSERTION: a render NOT issued from the agent surface carries no anchor "
			       "block -- the anchor is a bookmark in the MODEL's own sequence of looks. (The "
			       "SAME predicate gates the latest-frame bookkeeping, so such a render cannot "
			       "become what a later set_render_anchor would pin either.)" );
		}
	}

	//------------------------------------------------------------------
	// 8. Fix-round P1: the THREE-WAY composite -- when a scene-target
	//    composite is ALSO built for this call, it IS the lower pane, so
	//    the stack reads anchor / imagined target / this render (see
	//    AgentRenderResult::anchorApplied's doc, "the two mechanisms
	//    compose instead of one silently suppressing the other").
	//    ApplyRenderAnchorComparison_ branches on `baseIsSceneTarget`
	//    (AgentSession.cpp ~21974-21984 decode, ~22037 label, ~22107 note)
	//    and until now nothing armed a scene target while probing the
	//    anchor, so that whole branch had zero coverage.
	//------------------------------------------------------------------
	std::printf( "[8] P1: the THREE-WAY composite -- anchor over (imagined target + this render)\n" );
	{
		AgentSession::SetBuildProtocolDefaultEnabled( false );   // immediate arming, like [7]
		std::unique_ptr<AgentSession> tw = AgentSession::LoadFromFile( scenePath );
		Check( tw != nullptr, "P1: a fresh session loads for the three-way composite" );
		if( tw ) {
			// The target is minted at EXACTLY the film's own dims (24x24, ==
			// tileW/tileH at kMaxEdge): BoxDownscaleRgb8_ short-circuits to an
			// IDENTITY copy when src dims == dst dims (see AgentSession.cpp),
			// so the composite's target band is BYTE-FOR-BYTE the target's
			// own raw pixels rather than something this test would need to
			// reproduce a resampling filter to check.  A solid, saturated
			// green: distinct from the render's grey/red palette AND from
			// the composite's black label ground and 128-grey rules, so a
			// probe landing in the wrong band cannot pass by accident.
			const unsigned char kTargetR = 20, kTargetG = 210, kTargetB = 40;
			const std::vector<unsigned char> targetPng =
				EncodeSolidRgb8Png( kTargetR, kTargetG, kTargetB, 24, 24 );   // == the film's own 24x24 dims
			Check( !targetPng.empty(), "P1: the canned 24x24 solid-green target PNG encoded" );

			tw->SetImageGenerator( MakeFakeImageGen( targetPng ) );
			Check( tw->ImagineScene( "a solid green target" ).ok, "P1: the imagine succeeds" );

			// The FIRST qualifying render becomes the anchor (protocol off,
			// same rule as [7]) -- established, no composite yet, exactly
			// like [2].
			const AgentRenderResult twFirst = tw->Render( FullFrameParams( kMaxEdge ) );
			Check( twFirst.ok && twFirst.anchorApplied && twFirst.anchorEstablished,
			       "P1: the first render establishes the anchor" );
			const std::uint64_t twAnchorRev = twFirst.anchorRevision;
			const std::vector<unsigned char> twAnchorPng = twFirst.png;
			Check( twFirst.width == 24 && twFirst.height == 24, "P1: the anchor render is 24x24" );

			// A VISIBLE edit before the second render -- same red-proof
			// discipline as [3]/[4]: without a real pixel difference between
			// the anchor source and this render, the pane probes below would
			// pass on a composite carrying either frame twice.
			AgentSetPatch twPatch;
			twPatch.target = "pnt_albedo";
			twPatch.param  = "color";
			twPatch.value  = "0.05 0.05 0.95";
			Check( tw->ProposePatch( twPatch ).applied, "P1: the albedo recolour applied" );

			const AgentRenderResult twSecond = tw->Render( FullFrameParams( kMaxEdge ) );
			Check( twSecond.ok, std::string( "P1: the second render succeeded: " ) + twSecond.message );
			Check( twSecond.anchorApplied && !twSecond.anchorEstablished,
			       "P1: the second render is anchored (not establishing)" );
			Check( twSecond.anchorRevision == twAnchorRev,
			       "P1: the composite still names the ANCHOR's revision" );
			Check( twSecond.sceneTargetApplied,
			       "P1 PRECONDITION: the scene target ALSO applies to this render -- without this "
			       "the baseIsSceneTarget branch under test never fires" );
			Check( !twSecond.sceneTargetCompositePng.empty(),
			       "P1 PRECONDITION: and its OWN [target|render] composite was built -- this is what "
			       "`baseIsSceneTarget` tests for (!rr.sceneTargetCompositePng.empty())" );
			Check( !twSecond.anchorCompositePng.empty(), "P1: the anchor composite was built" );

			Decoded comp, anchorSrc, curSrc, targetSrc;
			Check( DecodePng( twSecond.anchorCompositePng, comp ), "P1: the three-way composite decodes" );
			Check( DecodePng( twAnchorPng, anchorSrc ), "P1: the anchor source render decodes" );
			Check( DecodePng( twSecond.png, curSrc ), "P1: this render's own PNG decodes" );
			Check( DecodePng( targetPng, targetSrc ), "P1: the target PNG decodes" );

			// RED-PROOF AGAINST VACUITY, same shape as [4]: the anchor
			// source and this render must really differ, or every probe
			// below passes no matter which frame occupies which band.
			bool twFramesDiffer = false;
			if( anchorSrc.w == curSrc.w && anchorSrc.h == curSrc.h ) {
				for( unsigned int y = 0; y < anchorSrc.h && !twFramesDiffer; ++y )
					for( unsigned int x = 0; x < anchorSrc.w; ++x )
						if( anchorSrc.at( x, y ) != curSrc.at( x, y ) ) { twFramesDiffer = true; break; }
			}
			Check( twFramesDiffer,
			       "P1 RED-PROOF: the anchor source and this render DIFFER (the recoloured albedo "
			       "landed) -- without this the pane probes below would pass vacuously" );

			const unsigned int tileW = twSecond.width, tileH = twSecond.height;   // 24x24
			const unsigned int paneH = tileH;                                   // anchor not up/downscaled (24==24)
			const unsigned int baseH = twSecond.sceneTargetCompositeWidth == tileW
				? twSecond.sceneTargetCompositeHeight : 0;
			Check( twSecond.sceneTargetCompositeWidth == tileW,
			       "P1: the scene-target sub-composite is exactly one tile wide" );
			Check( twSecond.anchorCompositeWidth == tileW,
			       "P1: MONEY ASSERTION -- the OUTER composite is ALSO exactly one tile wide (the "
			       "scene-target composite really did become the base, not a re-derived tile)" );
			Check( comp.w == tileW, "P1: the decoded composite is one tile wide" );

			const unsigned int bandH = 24;   // the target's own height: identity-scaled, == tileH here
			const ThreeWayLayout L = SolveThreeWayLayout( comp.h, paneH, baseH, bandH, tileH );
			Check( L.ok,
			       "P1 MONEY ASSERTION: the composite height decomposes EXACTLY into label + anchor "
			       "pane + rule + label + (target band + rule + render tile) -- the four-band "
			       "structure the baseIsSceneTarget branch is supposed to produce" );

			if( L.ok && comp.w == tileW && anchorSrc.w == tileW && curSrc.w == tileW &&
			    targetSrc.w == tileW ) {
				auto within1 = []( const Px& a, const Px& b ) {
					for( int i = 0; i < 3; ++i ) {
						const int d = (int)a[i] - (int)b[i];
						if( d > 1 || d < -1 ) return false;
					}
					return true;
				};

				// THE ANCHOR PANE: every pixel is the ANCHOR SOURCE render's
				// own pixel, and (cross-probe) NOT this render's.
				std::size_t anchorMismatch = 0, anchorVsOther = 0;
				for( unsigned int y = 0; y < paneH; ++y ) {
					for( unsigned int x = 0; x < tileW; ++x ) {
						const Px& p = comp.at( x, L.paneY + y );
						if( !within1( p, anchorSrc.at( x, y ) ) ) ++anchorMismatch;
						if( !within1( p, curSrc.at( x, y ) ) )    ++anchorVsOther;
					}
				}
				Check( anchorMismatch == 0,
				       "P1 MONEY ASSERTION: every pixel of the ANCHOR pane matches the anchor source "
				       "render (" + std::to_string( (unsigned long long)anchorMismatch ) + " off)" );
				Check( anchorVsOther > 0,
				       "P1: and the anchor pane does NOT match this render -- it is really the "
				       "earlier frame" );

				// THE TARGET BAND, inside the base region: every pixel is the
				// TARGET's own solid colour, and (cross-probe) neither the
				// anchor source nor this render's palette.
				// Cross-probe convention, matching [4]'s exactly: count
				// MISMATCHES against the OTHER source and require AT LEAST
				// ONE (not "zero matches") -- both frames share plenty of
				// incidental pixels (the background is black in both the
				// anchor source and this render, for instance), so "zero
				// pixels coincide" is not a safe bar; "not a full duplicate
				// of the other frame" is.
				std::size_t targetMismatch = 0, targetVsAnchorDiffer = 0, targetVsCurDiffer = 0;
				for( unsigned int y = 0; y < bandH; ++y ) {
					for( unsigned int x = 0; x < tileW; ++x ) {
						const Px& p = comp.at( x, L.targetY + y );
						if( !within1( p, targetSrc.at( x, y ) ) )  ++targetMismatch;
						if( !within1( p, anchorSrc.at( x, y ) ) )  ++targetVsAnchorDiffer;
						if( !within1( p, curSrc.at( x, y ) ) )     ++targetVsCurDiffer;
					}
				}
				Check( targetMismatch == 0,
				       "P1 MONEY ASSERTION: every pixel of the TARGET band (inside the base region) "
				       "matches the imagined target's own pixel (" +
				       std::to_string( (unsigned long long)targetMismatch ) + " off) -- the inner "
				       "region really does contain the target" );
				Check( targetVsAnchorDiffer == static_cast<std::size_t>( bandH ) * tileW &&
				       targetVsCurDiffer == static_cast<std::size_t>( bandH ) * tileW,
				       "P1: and the target band matches NEITHER the anchor source NOR this render at "
				       "ANY pixel (the solid green is its own distinct colour, unlike the shared black "
				       "background the render tile and anchor pane happen to share)" );

				// THE RENDER TILE, at the bottom of the base region: every
				// pixel is THIS RENDER's own pixel, and (cross-probe) the
				// tile is not a full duplicate of the anchor source -- the
				// inner region contains the CURRENT render too, not just the
				// target.
				std::size_t tileMismatch = 0, tileVsAnchorDiffer = 0;
				for( unsigned int y = 0; y < tileH; ++y ) {
					for( unsigned int x = 0; x < tileW; ++x ) {
						const Px& p = comp.at( x, L.tileY + y );
						if( !within1( p, curSrc.at( x, y ) ) )    ++tileMismatch;
						if( !within1( p, anchorSrc.at( x, y ) ) ) ++tileVsAnchorDiffer;
					}
				}
				Check( tileMismatch == 0,
				       "P1 MONEY ASSERTION: every pixel of the RENDER TILE (inside the base region) "
				       "matches THIS render's own pixel (" +
				       std::to_string( (unsigned long long)tileMismatch ) + " off) -- the inner "
				       "region contains the CURRENT render, not just the target" );
				Check( tileVsAnchorDiffer > 0,
				       "P1: and the render tile is NOT a full duplicate of the anchor source -- it is "
				       "really the current (recoloured) frame, same red-proof shape as [4]'s "
				       "anchorVsOther/baseVsOther" );

				// The two rules and the two label strips: structural, ink-
				// presence checks (same idiom as [4]'s sawLabelInk) --
				// "outer label strip says ANCHOR" / the second says
				// "TARGET AND THIS RENDER REV N" is asserted at the string
				// level below via the note text, since this file does not
				// carry a glyph-OCR table (kAnchorGlyph3x5 is private to
				// AgentSession.cpp).  ONE LSB of tolerance on both rules, not
				// just the outer one: the INNER rule (the scene-target
				// composite's own) has already been through ONE MORE
				// encode/decode round trip than the outer rule by the time it
				// lands in this composite (drawn -> encoded as the scene-
				// target composite -> decoded as `base` -> re-encoded here),
				// so it inherits the SAME truncating-cast 1-LSB budget every
				// other cross-composite probe in this file already carries
				// (see [4]'s "TOLERANCE OF ONE LSB" comment) -- measured
				// 127 in practice, not a layout bug.
				const Px kGrey128 = { 128, 128, 128 };
				const Px& outerRule = comp.at( tileW / 2, L.ruleY );
				Check( within1( outerRule, kGrey128 ),
				       "P1: the OUTER grey rule separates the anchor pane from the base region" );
				const Px& innerRule = comp.at( tileW / 2, L.rule2Y );
				Check( within1( innerRule, kGrey128 ),
				       "P1: the INNER grey rule (the scene-target composite's own) separates the "
				       "target band from the render tile" );
				bool sawOuterLabelInk = false;
				for( unsigned int y = 0; y < L.labelH && !sawOuterLabelInk; ++y )
					for( unsigned int x = 0; x < tileW; ++x )
						if( comp.at( x, y )[0] > 200 ) { sawOuterLabelInk = true; break; }
				Check( sawOuterLabelInk,
				       "P1: MONEY ASSERTION -- the OUTER label strip carries drawn text (\"ANCHOR "
				       "REV N\")" );
				bool sawInnerLabelInk = false;
				for( unsigned int y = 0; y < L.labelH && !sawInnerLabelInk; ++y )
					for( unsigned int x = 0; x < tileW; ++x )
						if( comp.at( x, L.label2Y + y )[0] > 200 ) { sawInnerLabelInk = true; break; }
				Check( sawInnerLabelInk,
				       "P1: MONEY ASSERTION -- the SECOND label strip carries drawn text (\"TARGET "
				       "AND THIS RENDER REV N\", the three-way variant, not the plain two-way one)" );
			}

			// THE NOTE: the three-way variant of the sentence, distinct from
			// the plain "above this render, revision" phrasing [3] pins for
			// the two-way case.
			const std::string want3way = "above your imagined target and this render, revision " +
				std::to_string( (unsigned long long)twSecond.anchorCurrentRevision );
			Check( twSecond.message.find( want3way ) != std::string::npos,
			       "P1 MONEY ASSERTION: the note names the THREE-WAY variant (\"" + want3way +
			       "\") -- goes RED the moment baseIsSceneTarget stops actually gating the phrasing" );
		}
	}

	//------------------------------------------------------------------
	// 9. Fix-round P2-3: LABEL LEGIBILITY AT THE FLOOR.  At imageMaxEdge
	//    16 the label strip has no room to say much -- DrawAnchorLabel_'s
	//    drop-not-wrap rule truncates the pixel text to roughly 4
	//    characters at scale 1.  This is NOT a bug to fix by building text
	//    wrapping (see the sentence added to AgentRenderResult::
	//    anchorApplied's doc).  What IS pinned here is the mitigation:
	//    truncation drops cleanly (no crash, no OOB, the composite
	//    geometry stays coherent) and the note text carries BOTH
	//    revisions in FULL prose regardless of pixel width.
	//------------------------------------------------------------------
	std::printf( "[9] P2-3: label legibility at the imageMaxEdge floor (16)\n" );
	{
		const unsigned int kFloorEdge = 16;
		AgentSession::SetBuildProtocolDefaultEnabled( false );
		std::unique_ptr<AgentSession> floorSess = AgentSession::LoadFromFile( scenePath );
		Check( floorSess != nullptr, "P2-3: a fresh session loads for the floor-dims composite" );
		if( floorSess ) {
			const AgentRenderResult f1 = floorSess->Render( FullFrameParams( kFloorEdge ) );
			Check( f1.ok && f1.anchorApplied && f1.anchorEstablished,
			       "P2-3: the first floor-dims render establishes the anchor" );

			AgentSetPatch fp;
			fp.target = "pnt_albedo";
			fp.param  = "color";
			fp.value  = "0.9 0.9 0.1";
			Check( floorSess->ProposePatch( fp ).applied, "P2-3: the albedo recolour applied" );

			const AgentRenderResult f2 = floorSess->Render( FullFrameParams( kFloorEdge ) );
			Check( f2.ok, std::string( "P2-3: the second floor-dims render succeeded: " ) + f2.message );
			Check( f2.anchorApplied && !f2.anchorEstablished,
			       "P2-3: the second floor-dims render carries the composite" );
			Check( !f2.anchorCompositePng.empty(), "P2-3: the composite was built even at the floor" );
			Check( f2.anchorCompositeWidth <= kFloorEdge,
			       "P2-3: the composite is at (or below, by the never-upscale rule) the floor width" );

			Decoded fc;
			Check( DecodePng( f2.anchorCompositePng, fc ),
			       "P2-3 MONEY ASSERTION: the composite decodes cleanly at the floor -- no OOB, no "
			       "corrupt PNG out of the truncated-label draw path" );
			Check( fc.w == f2.anchorCompositeWidth && fc.h == f2.anchorCompositeHeight,
			       "P2-3: the decoded dims match the reported dims exactly" );

			// NOTE: `f2.height` is the RENDER's own resolution (the film's
			// 24x24, unaffected by imageMaxEdge -- imageMaxEdge only bounds
			// the COMPOSITE tile, per FrameDimsAtMaxEdge_).  The tile height
			// actually embedded in the composite is the floor-scaled one,
			// which for this square film at a square floor edge is exactly
			// `f2.anchorCompositeWidth` on both axes (24x24 -> 16x16,
			// scale applied uniformly) -- using the render's raw height here
			// would silently mis-solve the layout on any scene where the
			// floor edge actually resamples.
			const unsigned int floorTileEdge = f2.anchorCompositeWidth;
			const CompositeLayout FL = SolveLayout( fc.h, floorTileEdge );
			Check( FL.ok,
			       "P2-3 MONEY ASSERTION: the composite geometry stays coherent at the floor -- label "
			       "+ pane + rule + label + pane still decomposes exactly, even though the LABEL TEXT "
			       "inside each strip had to drop characters to fit -- the drop is confined to the "
			       "glyph draw, it never disturbs the surrounding layout" );

			const std::string want1 = "revision " + std::to_string( (unsigned long long)f2.anchorRevision );
			const std::string want2 =
				"revision " + std::to_string( (unsigned long long)f2.anchorCurrentRevision );
			Check( f2.message.find( want1 ) != std::string::npos &&
			       f2.message.find( want2 ) != std::string::npos,
			       "P2-3 MONEY ASSERTION: the note text carries BOTH revisions in FULL prose even at "
			       "the floor -- the pixel label truncates, the sentence does not" );
		}
	}

	//------------------------------------------------------------------
	// 10. Fix-round P3: the kRenderAnchorMaxPngBytes SKIP path.  Driven
	//     via the direct C++ surface (AgentSession::Render, not the wire):
	//     an honestly oversized full-resolution render (no synthetic byte
	//     injection -- ApplyRenderAnchorComparison_ is private, so the
	//     ONLY honest way to trip `rr.png.size() > kRenderAnchorMaxPngBytes`
	//     is a render whose OWN sink->ToPng() really is that big).
	//
	//     WHAT DIDN'T WORK, kept as a lesson in the comment: a geometry-
	//     free env-dome background (checker_painter through radiance_map)
	//     seemed like the cheapest possible fixture -- one primary ray per
	//     pixel, no bounces -- but a fine checker under equirectangular
	//     direction-mapping produces a beautifully SYMMETRIC moire (visibly
	//     verified: concentric interference rings around the view axis),
	//     which is exactly the kind of long-range regularity PNG's LZ77
	//     stage crushes -- a 3600x3600 frame of it compressed to ~15 KB,
	//     ~2500x short of the 8 MiB target.  A deterministic function of
	//     ray DIRECTION is not the same thing as noise.
	//
	//     What actually works: real MONTE CARLO variance.  A frame-filling
	//     diffuse plane lit by many small, scattered omni lights, at 1 spp
	//     -- RISE's unified LightSampler picks ONE light per NEE sample
	//     (see `choose_one_light`'s "always selects one light per NEE"
	//     doc), so adjacent pixels' independent RNG draws land on
	//     different lights at different distances/colours, giving genuine
	//     per-pixel brightness variance with no cross-pixel structure to
	//     exploit.  Cheap to trace: one bounce (direct-lit plane, no GI
	//     needed), no dielectrics, no SMS.
	//------------------------------------------------------------------
	std::printf( "[10] P3: the kRenderAnchorMaxPngBytes skip path (an honestly oversized render)\n" );
	{
		AgentSession::SetBuildProtocolDefaultEnabled( false );
		// Mirrors the private AgentSession::kRenderAnchorMaxPngBytes (8 MiB,
		// AgentSession.h ~7563) -- there is no test accessor for a private
		// static constexpr, and the value is stable enough (doc'd, belt-and-
		// braces) to pin locally rather than add one for this alone.
		const std::size_t kMirroredMaxPngBytes = 8u * 1024u * 1024u;
		const unsigned int kBigEdge = 3000;

		// The lights: a fixed (seeded, deterministic) pseudo-random
		// scatter, so the fixture is reproducible across runs rather than
		// flaky on light placement.  Positions span a plane-filling frustum
		// slice; colours/powers vary so NEE's per-pixel light choice swings
		// brightness hard, not just position.
		std::string lightChunks;
		std::uint32_t rngState = 0x9E3779B9u;
		auto nextRand = [&rngState]() -> double {
			rngState = rngState * 1664525u + 1013904223u;   // classic LCG
			return static_cast<double>( rngState ) / 4294967296.0;   // [0,1)
		};
		const int kLightCount = 64;
		for( int i = 0; i < kLightCount; ++i ) {
			const double px = ( nextRand() - 0.5 ) * 16.0;
			const double py = ( nextRand() - 0.5 ) * 16.0;
			const double pz = -2.0 + nextRand() * 4.0;
			const double r  = 0.2 + nextRand() * 0.8;
			const double g  = 0.2 + nextRand() * 0.8;
			const double b  = 0.2 + nextRand() * 0.8;
			const double pw = 4.0 + nextRand() * 20.0;
			lightChunks += "omni_light\n{\n\tname lt" + std::to_string( i ) +
				"\n\tposition " + std::to_string( px ) + " " + std::to_string( py ) + " " +
				std::to_string( pz ) + "\n\tcolor " + std::to_string( r ) + " " +
				std::to_string( g ) + " " + std::to_string( b ) + "\n\tpower " +
				std::to_string( pw ) + "\n\tshootphotons false\n}\n\n";
		}

		const std::string bigScene =
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname pnt_plane\n\tcolor 0.8 0.8 0.8\n}\n\n"
			"lambertian_material\n{\n\tname mat_plane\n\treflectance pnt_plane\n}\n\n"
			"clippedplane_geometry\n{\n\tname quad_plane\n\tpta -12 12 -6\n\tptb 12 12 -6\n"
			"\tptc 12 -12 -6\n\tptd -12 -12 -6\n}\n\n"
			"standard_object\n{\n\tname obj_plane\n\tgeometry quad_plane\n\tmaterial mat_plane\n}\n\n" +
			lightChunks +
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise false\n}\n\n"
			"film\n{\n\twidth " + std::to_string( kBigEdge ) + "\n\theight " +
			std::to_string( kBigEdge ) + "\n}\n\n"
			"pinhole_camera\n{\n\tlocation 0 0 3.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 90.0\n}\n";
		const std::string bigScenePath = WriteTemp( "rise_agent_anchor_big.RISEscene", bigScene );
		Check( !bigScenePath.empty(), "P3: wrote the oversized-fixture scene to a temp file" );
		std::unique_ptr<AgentSession> big = AgentSession::LoadFromFile( bigScenePath );
		Check( big != nullptr, "P3: the oversized-fixture session loads" );
		if( big ) {
			// EXPLICIT width/height, not just a big authored Film size: the
			// "no width/height requested" agent-surface default silently
			// caps an authored Film down to kAgentSurfaceMaxRenderEdge (256)
			// for fromAgentSurface renders (AgentRenderResult::
			// agentResolutionCapped) -- measured directly during development
			// of this fixture (identical 217162-byte output at authored Film
			// 2200 AND 5000, both silently rendered at 256x256).  An
			// EXPLICIT width/height override is the documented direct-C++
			// escape hatch (see kRenderAnchorMaxPngBytes's own doc: "what it
			// covers is a DIRECT C++ caller that sets fromAgentSurface itself
			// and asks for megapixel dims" -- the RPC layer's 256 clamp does
			// not apply here).
			AgentRenderParams bigParams = FullFrameParams( kBigEdge );
			bigParams.width  = kBigEdge;
			bigParams.height = kBigEdge;
			const AgentRenderResult b1 = big->Render( bigParams );
			Check( b1.ok, std::string( "P3: the oversized render itself succeeded: " ) + b1.message );
			Check( b1.png.size() > kMirroredMaxPngBytes,
			       "P3 PRECONDITION: the render's own full-resolution PNG really exceeds "
			       "kRenderAnchorMaxPngBytes (" + std::to_string( b1.png.size() ) +
			       " bytes) -- without this the skip path under test never fires" );
			Check( !b1.anchorApplied,
			       "P3 MONEY ASSERTION: an oversized render is skipped ENTIRELY, not remembered at a "
			       "downscaled resolution -- an anchor quietly stored smaller than the render beside "
			       "it would make the composite partly a comparison of resolutions" );
			Check( !b1.anchorEstablished, "P3: and nothing was established by it" );
			Check( b1.anchorCompositePng.empty(), "P3: and it carries no anchor composite" );
			Check( !big->HasRenderAnchor(),
			       "P3: the session still has NO anchor pinned after an oversized render -- the skip "
			       "is not merely \"no composite this time\", nothing was remembered at all" );
		}
		std::remove( bigScenePath.c_str() );
	}

	std::remove( scenePath.c_str() );
	std::printf( "=== AgentRenderAnchorTest: %d passed, %d failed ===\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
