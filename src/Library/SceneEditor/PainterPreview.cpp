//////////////////////////////////////////////////////////////////////
//
//  PainterPreview.cpp - See PainterPreview.h.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PainterPreview.h"
#include "../Interfaces/IPainterManager.h"
#include "../Interfaces/IScalarPainterManager.h"
#include "../Painters/ExpressionPainter.h"
#include "../Painters/RampPainter.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace RISE
{

namespace
{
	//! The unit-patch world/object position for grid coordinate (u,v) --
	//! see PainterPreview.h's domain-conventions note.
	Vector3 UnitPatchPos( double u, double v )
	{
		return Vector3( Scalar( u - 0.5 ), Scalar( v - 0.5 ), Scalar( 0 ) );
	}

	//! Builds the synthetic-but-honest RayIntersectionGeometric for grid
	//! coordinate (u,v) -- see PainterPreview.h's domain-conventions note.
	RayIntersectionGeometric MakePreviewRi( double u, double v, Scalar fw )
	{
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		ri.bHit = true;
		ri.ptCoord = Point2( Scalar( u ), Scalar( v ) );
		const Vector3 p = UnitPatchPos( u, v );
		ri.ptIntersection = Point3( p.x, p.y, p.z );
		ri.ptObjIntersec  = Point3( p.x, p.y, p.z );
		ri.vNormal = Vector3( Scalar( 0 ), Scalar( 0 ), Scalar( 1 ) );
		// `widthValid`, not `valid`: `fw` is a footprint WIDTH, and
		// `valid` means "the UV Jacobian is usable" -- which the preview has
		// nothing to offer for.  Setting `valid` alone would stop feeding
		// `fw` to the expression VM entirely (ExpressionPainter keys `ctx.fw`
		// on `widthValid`), and setting it in addition would falsely promise
		// TexturePainter a zero Jacobian.  See
		// docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md's two-flag contract.
		ri.txFootprint.widthValid = true;
		ri.txFootprint.worldWidth = fw;
		return ri;
	}

	//! Fixed [0,1]-clamp + gamma-2.2 display encode -- see the header's
	//! "DISPLAY ENCODE" note.  NaN/negative both fall out of `!(c>0)`
	//! (NaN compares false against everything) to 0, matching the rest of
	//! the painter pipeline's NaN->0 convention (ExpressionPainter::SafeComp).
	unsigned char EncodeChannel( Scalar linear )
	{
		Scalar c = linear;
		if( !( c > Scalar( 0 ) ) ) c = Scalar( 0 );
		if( c > Scalar( 1 ) ) c = Scalar( 1 );
		const double enc = std::pow( (double)c, 1.0 / 2.2 );
		int v = (int)( enc * 255.0 + 0.5 );
		if( v < 0 ) v = 0;
		if( v > 255 ) v = 255;
		return (unsigned char)v;
	}

	void WriteRGBA( std::vector<unsigned char>& rgba, std::size_t idx, Scalar r, Scalar g, Scalar b )
	{
		rgba[idx+0] = EncodeChannel( r );
		rgba[idx+1] = EncodeChannel( g );
		rgba[idx+2] = EncodeChannel( b );
		rgba[idx+3] = 255;
	}

	bool ValidDims( unsigned int w, unsigned int h, std::string& err )
	{
		if( w == 0 || h == 0 ) { err = "width and height must both be > 0"; return false; }
		if( w > PainterPreview::kMaxDim || h > PainterPreview::kMaxDim ) {
			err = "width/height exceeds the " + std::to_string( PainterPreview::kMaxDim ) + "-pixel preview cap";
			return false;
		}
		return true;
	}

	//! Normalizes a full-patch scalar buffer in place (already NaN/Inf-
	//! scrubbed to 0 by the caller) to grayscale RGBA -- the auto-range
	//! contract from the header's "SCALAR NORMALIZATION" note.  A flat
	//! patch (dataMin == dataMax, including the degenerate w*h==1 case)
	//! maps to a uniform mid-gray rather than dividing by zero.
	void NormalizeScalarToRGBA( const std::vector<double>& raw, unsigned int w, unsigned int h,
		std::vector<unsigned char>& outRGBA, double& outMin, double& outMax )
	{
		double dataMin = 0, dataMax = 0;
		bool first = true;
		for( double val : raw ) {
			if( first ) { dataMin = dataMax = val; first = false; continue; }
			if( val < dataMin ) dataMin = val;
			if( val > dataMax ) dataMax = val;
		}
		const double range = dataMax - dataMin;
		for( unsigned int y = 0; y < h; ++y ) {
			for( unsigned int x = 0; x < w; ++x ) {
				const std::size_t i = (std::size_t)y * w + x;
				const double norm = ( range > 1e-12 ) ? ( ( raw[i] - dataMin ) / range ) : 0.5;
				const unsigned char g = EncodeChannel( Scalar( norm ) );
				const std::size_t idx = i * 4;
				outRGBA[idx+0] = g; outRGBA[idx+1] = g; outRGBA[idx+2] = g; outRGBA[idx+3] = 255;
			}
		}
		outMin = dataMin;
		outMax = dataMax;
	}

	//! Auto-ranges a per-pixel RGB scalar TRIPLE (already NaN/Inf-scrubbed
	//! by the caller) to RGBA -- the per-channel-jointly variant of
	//! NormalizeScalarToRGBA above, for P2-4's HasPerChannelVariation()
	//! render path (see the header's PER-CHANNEL SCALAR PAINTERS note).
	//! `raw` is w*h*3 doubles, R/G/B interleaved per pixel; ONE shared
	//! [dataMin, dataMax] is computed across ALL three channels of every
	//! pixel (not three independent per-channel ranges), so the relative
	//! magnitude between channels survives the encode.  Degenerate
	//! (dataMin == dataMax) maps every channel to mid-gray 0.5, matching
	//! NormalizeScalarToRGBA's convention.
	void NormalizeScalarTripleToRGBA( const std::vector<double>& raw, unsigned int w, unsigned int h,
		std::vector<unsigned char>& outRGBA, double& outMin, double& outMax )
	{
		double dataMin = 0, dataMax = 0;
		bool first = true;
		for( double val : raw ) {
			if( first ) { dataMin = dataMax = val; first = false; continue; }
			if( val < dataMin ) dataMin = val;
			if( val > dataMax ) dataMax = val;
		}
		const double range = dataMax - dataMin;
		outRGBA.assign( (std::size_t)w * h * 4, 0 );
		for( unsigned int y = 0; y < h; ++y ) {
			for( unsigned int x = 0; x < w; ++x ) {
				const std::size_t i = (std::size_t)y * w + x;
				const std::size_t ridx = i * 3;
				const std::size_t idx = i * 4;
				for( int c = 0; c < 3; ++c ) {
					const double norm = ( range > 1e-12 ) ? ( ( raw[ridx+c] - dataMin ) / range ) : 0.5;
					outRGBA[idx + (std::size_t)c] = EncodeChannel( Scalar( norm ) );
				}
				outRGBA[idx+3] = 255;
			}
		}
		outMin = dataMin;
		outMax = dataMax;
	}

	//! P2-1 (S10 review round 1): the sample-count budget's grid-sizing
	//! policy -- see PainterPreview.h's EVALUATION BUDGET note.  Picks
	//! (gw, gh) with gw <= w, gh <= h, gw*gh <= budget, gw >= 1, gh >= 1,
	//! preserving w:h aspect ratio as closely as integer rounding allows.
	//! When w*h is already within budget, returns (w, h) unchanged (the
	//! common case: every default-sized preview this module ships with
	//! today never decimates).
	void ComputeDecimatedGrid( unsigned int w, unsigned int h, unsigned int budget,
		unsigned int& gw, unsigned int& gh )
	{
		const std::uint64_t total = (std::uint64_t)w * (std::uint64_t)h;
		if( total <= (std::uint64_t)budget ) { gw = w; gh = h; return; }

		const double s = std::sqrt( (double)budget / (double)total );
		double gwD = std::floor( (double)w * s );
		double ghD = std::floor( (double)h * s );
		gw = (unsigned int)std::max( 1.0, gwD );
		gh = (unsigned int)std::max( 1.0, ghD );
		if( gw > w ) gw = w;
		if( gh > h ) gh = h;

		// Floor + clamp above can still leave gw*gh a little over budget
		// for some aspect ratios (rounding, or the >w/>h clamp) -- trim
		// the longer axis one step at a time.  Bounded: each iteration
		// removes at least one row/column and the starting point is
		// already close to budget, so this converges in a handful of
		// steps, never a meaningful fraction of w or h.
		while( (std::uint64_t)gw * (std::uint64_t)gh > (std::uint64_t)budget && ( gw > 1 || gh > 1 ) ) {
			if( gw >= gh && gw > 1 ) --gw;
			else if( gh > 1 ) --gh;
			else --gw;
		}
	}

	//! P2-1: nearest-neighbour upscale of a `sw`x`sh` RGBA8 grid into a
	//! full `dw`x`dh` buffer -- the ABI-preserving half of the sample
	//! budget (PainterPreview.h's EVALUATION BUDGET note).  `dst` is
	//! (re)sized to exactly dw*dh*4 bytes.  No-op copy when the grid
	//! already matches the requested dimensions (the un-decimated case).
	void UpscaleRGBANearest( const std::vector<unsigned char>& src, unsigned int sw, unsigned int sh,
		std::vector<unsigned char>& dst, unsigned int dw, unsigned int dh )
	{
		if( sw == dw && sh == dh ) { dst = src; return; }
		dst.assign( (std::size_t)dw * dh * 4, 0 );
		for( unsigned int y = 0; y < dh; ++y ) {
			unsigned int sy = (unsigned int)( ( (std::uint64_t)y * sh ) / dh );
			if( sy >= sh ) sy = sh - 1;
			for( unsigned int x = 0; x < dw; ++x ) {
				unsigned int sx = (unsigned int)( ( (std::uint64_t)x * sw ) / dw );
				if( sx >= sw ) sx = sw - 1;
				const std::size_t si = ( (std::size_t)sy * sw + sx ) * 4;
				const std::size_t di = ( (std::size_t)y * dw + x ) * 4;
				dst[di+0] = src[si+0]; dst[di+1] = src[si+1]; dst[di+2] = src[si+2]; dst[di+3] = src[si+3];
			}
		}
	}
}   // anonymous namespace

PainterPreview::Result PainterPreview::RenderPainterPreview(
	IJobPriv& job, const String& painterName, unsigned int w, unsigned int h )
{
	Result out;
	std::string err;
	if( !ValidDims( w, h, err ) ) { out.refusalReason = err; return out; }
	if( painterName.size() <= 1 ) { out.refusalReason = "empty painter name"; return out; }

	// Colour pipe tried first: PainterIntrospection::PipesFor documents
	// that no painter kind registers in both managers today, so trying
	// colour-then-scalar never papers over a real ambiguity.
	IPainter* colourP = nullptr;
	if( IPainterManager* pm = job.GetPainters() ) colourP = pm->GetItem( painterName.c_str() );
	IScalarPainter* scalarP = nullptr;
	if( !colourP ) {
		if( IScalarPainterManager* spm = job.GetScalarPainters() ) scalarP = spm->GetItem( painterName.c_str() );
	}
	if( !colourP && !scalarP ) {
		out.refusalReason = std::string( "painter `" ) + painterName.c_str() + "` not found in either painter manager";
		return out;
	}

	// P2-1: bound the number of distinct evaluations regardless of the
	// requested w*h -- see the header's EVALUATION BUDGET note.  (gw,gh)
	// == (w,h) whenever w*h is already within budget, so every path below
	// is a no-op decimation (UpscaleRGBANearest short-circuits to a plain
	// copy) for any default-sized request.
	unsigned int gw, gh;
	ComputeDecimatedGrid( w, h, kMaxSampleBudget, gw, gh );
	const Scalar fw = Scalar( 1.0 / (double)std::max( gw, gh ) );
	out.width = w; out.height = h;

	if( colourP ) {
		std::vector<unsigned char> grid( (std::size_t)gw * gh * 4, 0 );
		for( unsigned int y = 0; y < gh; ++y ) {
			const double v = ( (double)y + 0.5 ) / (double)gh;
			for( unsigned int x = 0; x < gw; ++x ) {
				const double u = ( (double)x + 0.5 ) / (double)gw;
				const RayIntersectionGeometric ri = MakePreviewRi( u, v, fw );
				const RISEPel c = colourP->GetColor( ri );
				WriteRGBA( grid, ( (std::size_t)y * gw + x ) * 4, c.r, c.g, c.b );
			}
		}
		UpscaleRGBANearest( grid, gw, gh, out.rgba, w, h );
		out.status = Status::Ok;
		return out;
	}

	// Scalar pipe.
	if( scalarP->HasPerChannelVariation() ) {
		// P2-4: a genuine per-channel physical-scalar triple (e.g.
		// RGB-dispersive IOR) -- render it as RGB, jointly auto-ranged
		// across all three channels, rather than collapsing to v[0]-only
		// grayscale.  See the header's PER-CHANNEL SCALAR PAINTERS note.
		std::vector<double> raw3( (std::size_t)gw * gh * 3 );
		for( unsigned int y = 0; y < gh; ++y ) {
			const double v = ( (double)y + 0.5 ) / (double)gh;
			for( unsigned int x = 0; x < gw; ++x ) {
				const double u = ( (double)x + 0.5 ) / (double)gw;
				const RayIntersectionGeometric ri = MakePreviewRi( u, v, fw );
				const ScalarTriple t = scalarP->GetValuesAt( ri );
				const std::size_t ridx = ( (std::size_t)y * gw + x ) * 3;
				for( int c = 0; c < 3; ++c ) {
					double val = (double)t.v[c];
					if( !std::isfinite( val ) ) val = 0.0;
					raw3[ridx + (std::size_t)c] = val;
				}
			}
		}
		std::vector<unsigned char> grid;
		NormalizeScalarTripleToRGBA( raw3, gw, gh, grid, out.scalarRangeMin, out.scalarRangeMax );
		UpscaleRGBANearest( grid, gw, gh, out.rgba, w, h );
		out.status = Status::Ok;
		out.wasScalar = true;
		return out;
	}

	// Single-valued scalar pipe: auto-range v[0] only, grayscale (header's
	// SCALAR NORMALIZATION note).
	std::vector<double> raw( (std::size_t)gw * gh );
	for( unsigned int y = 0; y < gh; ++y ) {
		const double v = ( (double)y + 0.5 ) / (double)gh;
		for( unsigned int x = 0; x < gw; ++x ) {
			const double u = ( (double)x + 0.5 ) / (double)gw;
			const RayIntersectionGeometric ri = MakePreviewRi( u, v, fw );
			const ScalarTriple t = scalarP->GetValuesAt( ri );
			double val = (double)t.v[0];
			if( !std::isfinite( val ) ) val = 0.0;
			raw[ (std::size_t)y * gw + x ] = val;
		}
	}
	std::vector<unsigned char> grid( (std::size_t)gw * gh * 4, 0 );
	NormalizeScalarToRGBA( raw, gw, gh, grid, out.scalarRangeMin, out.scalarRangeMax );
	UpscaleRGBANearest( grid, gw, gh, out.rgba, w, h );
	out.status = Status::Ok;
	out.wasScalar = true;
	return out;
}

PainterPreview::Result PainterPreview::RenderDefStagePreview(
	IJobPriv& job, const String& painterName, int defIndex, unsigned int w, unsigned int h )
{
	Result out;
	std::string err;
	if( !ValidDims( w, h, err ) ) { out.refusalReason = err; return out; }
	if( painterName.size() <= 1 ) { out.refusalReason = "empty painter name"; return out; }
	if( defIndex < 0 ) { out.refusalReason = "defIndex must be >= 0"; return out; }

	const Implementation::ExpressionProgram* prog = nullptr;
	if( IPainterManager* pm = job.GetPainters() ) {
		if( IPainter* p = pm->GetItem( painterName.c_str() ) ) {
			if( const Implementation::ExpressionPainter* ep = dynamic_cast<const Implementation::ExpressionPainter*>( p ) )
				prog = &ep->GetProgram();
		}
	}
	if( !prog ) {
		if( IScalarPainterManager* spm = job.GetScalarPainters() ) {
			if( IScalarPainter* p = spm->GetItem( painterName.c_str() ) ) {
				if( const Implementation::ExpressionScalarPainter* ep = dynamic_cast<const Implementation::ExpressionScalarPainter*>( p ) )
					prog = &ep->GetProgram();
			}
		}
	}
	if( !prog ) {
		out.refusalReason = std::string( "painter `" ) + painterName.c_str() +
			"` is not an expression painter (expression_painter or scalar_painter { expression ... })";
		return out;
	}
	if( defIndex >= prog->DefCount() ) {
		out.refusalReason = "defIndex " + std::to_string( defIndex ) + " is out of range (program has " +
			std::to_string( prog->DefCount() ) + " def stage(s))";
		return out;
	}

	// P2-1: same decimated-grid budget as RenderPainterPreview above --
	// see the header's EVALUATION BUDGET note.
	unsigned int gw, gh;
	ComputeDecimatedGrid( w, h, kMaxSampleBudget, gw, gh );
	const Scalar fw = Scalar( 1.0 / (double)std::max( gw, gh ) );
	out.width = w; out.height = h;

	std::vector<Vector3> vals( (std::size_t)gw * gh );
	Implementation::ExpressionProgram::VType stageType = Implementation::ExpressionProgram::kScalar;
	for( unsigned int y = 0; y < gh; ++y ) {
		const double v = ( (double)y + 0.5 ) / (double)gh;
		for( unsigned int x = 0; x < gw; ++x ) {
			const double u = ( (double)x + 0.5 ) / (double)gw;
			Implementation::ExprEvalContext ctx;
			ctx.u = Scalar( u ); ctx.v = Scalar( v );
			const Vector3 p = UnitPatchPos( u, v );
			ctx.P = p; ctx.Po = p;
			ctx.N = Vector3( 0, 0, 1 );
			ctx.fw = fw; ctx.time = 0;
			Vector3 outVal; Implementation::ExpressionProgram::VType t = Implementation::ExpressionProgram::kScalar;
			// defIndex was already bounds-checked against DefCount() above,
			// so this call cannot fail; the return is still checked rather
			// than assumed (defensive, matches the module's general style).
			if( !prog->EvalDefStage( ctx, defIndex, outVal, t ) ) {
				out = Result();
				out.refusalReason = "internal error: EvalDefStage refused a bounds-checked defIndex";
				return out;
			}
			vals[ (std::size_t)y * gw + x ] = outVal;
			stageType = t;
		}
	}

	if( stageType == Implementation::ExpressionProgram::kVec3 ) {
		std::vector<unsigned char> grid( (std::size_t)gw * gh * 4, 0 );
		for( unsigned int y = 0; y < gh; ++y ) {
			for( unsigned int x = 0; x < gw; ++x ) {
				const Vector3& c = vals[ (std::size_t)y * gw + x ];
				WriteRGBA( grid, ( (std::size_t)y * gw + x ) * 4, c.x, c.y, c.z );
			}
		}
		UpscaleRGBANearest( grid, gw, gh, out.rgba, w, h );
		out.status = Status::Ok;
		return out;
	}

	std::vector<double> raw( vals.size() );
	for( std::size_t i = 0; i < vals.size(); ++i ) {
		double val = (double)vals[i].x;   // scalar stage broadcasts x==y==z
		if( !std::isfinite( val ) ) val = 0.0;
		raw[i] = val;
	}
	std::vector<unsigned char> grid( (std::size_t)gw * gh * 4, 0 );
	NormalizeScalarToRGBA( raw, gw, gh, grid, out.scalarRangeMin, out.scalarRangeMax );
	UpscaleRGBANearest( grid, gw, gh, out.rgba, w, h );
	out.status = Status::Ok;
	out.wasScalar = true;
	return out;
}

PainterPreview::Result PainterPreview::RenderRampStripPreview(
	IJobPriv& job, const String& painterName, unsigned int w, unsigned int h )
{
	Result out;
	std::string err;
	if( !ValidDims( w, h, err ) ) { out.refusalReason = err; return out; }
	if( painterName.size() <= 1 ) { out.refusalReason = "empty painter name"; return out; }

	const Implementation::RampPainter* ramp = nullptr;
	if( IPainterManager* pm = job.GetPainters() ) {
		if( IPainter* p = pm->GetItem( painterName.c_str() ) )
			ramp = dynamic_cast<const Implementation::RampPainter*>( p );
	}
	if( !ramp ) {
		out.refusalReason = std::string( "painter `" ) + painterName.c_str() + "` is not a ramp_painter";
		return out;
	}
	if( ramp->StopCount() < 2 ) {
		out.refusalReason = "ramp_painter has fewer than 2 stops (malformed)";
		return out;
	}

	const Scalar firstPos = ramp->StopPos( 0 );
	const Scalar lastPos  = ramp->StopPos( ramp->StopCount() - 1 );

	out.width = w; out.height = h;
	out.rgba.assign( (std::size_t)w * h * 4, 0 );

	std::vector<unsigned char> rowRGBA( (std::size_t)w * 4 );
	for( unsigned int x = 0; x < w; ++x ) {
		const double frac = ( (double)x + 0.5 ) / (double)w;
		const Scalar t = firstPos + Scalar( frac ) * ( lastPos - firstPos );
		const RISEPel c = ramp->EvalAt( t );
		WriteRGBA( rowRGBA, (std::size_t)x * 4, c.r, c.g, c.b );
	}
	for( unsigned int y = 0; y < h; ++y ) {
		std::copy( rowRGBA.begin(), rowRGBA.end(), out.rgba.begin() + (std::ptrdiff_t)( (std::size_t)y * w * 4 ) );
	}
	out.status = Status::Ok;
	return out;
}

}   // namespace RISE
