//////////////////////////////////////////////////////////////////////
//
//  TexturePainter.cpp - Implementation of the TexturePainter class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:  Add wrapping and clamping abilities!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TexturePainter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Profiling.h"

#include <cmath>
#include <cstdint>
#include <cstring>

using namespace RISE;
using namespace RISE::Implementation;

TexturePainter::TexturePainter( IRasterImageAccessor* pRIA_ ) :
  pRIA( pRIA_ ),
  filter_mode( Mode_Base )
{
	if( pRIA ) {
		pRIA->addref();
		// Landing 2: resolve LOD dispatch once at construction so
		// per-sample GetColor doesn't pay 2 virtual-call dispatches.
		// Pyramid mode wins when both are advertised (it's more
		// accurate; supersample is the lowmem fallback).
		if( pRIA->SupportsLOD() ) {
			filter_mode = Mode_Pyramid;
		} else if( pRIA->SupportsFootprint() ) {
			filter_mode = Mode_Supersample;
		}
	} else {
		GlobalLog()->PrintSourceError( "TexturePainter:: Invalid raster image accessor", __FILE__, __LINE__ );
	}
}

TexturePainter::~TexturePainter( )
{
	safe_release( pRIA );
}

namespace
{
	// Landing 2: PBRT-style longer-axis isotropic LOD from a 2x2
	// texture-space footprint Jacobian.  Returns LOD in mip-level
	// units (0 = base; +1 = next level down; etc.).
	inline Scalar ComputeLODFromTexelFootprint(
		const Scalar dsdx, const Scalar dsdy,
		const Scalar dtdx, const Scalar dtdy )
	{
		const Scalar lengthX = std::sqrt( dsdx * dsdx + dtdx * dtdx );
		const Scalar lengthY = std::sqrt( dsdy * dsdy + dtdy * dtdy );
		const Scalar maxLen = ( lengthX > lengthY ) ? lengthX : lengthY;
		// Guard against zero footprint (would log to -inf): clamp to
		// LOD = 0 (base level) which is what we want for "infinitely
		// fine sampling" anyway.
		if( maxLen <= Scalar( 1 ) ) return Scalar( 0 );
		return std::log2( maxLen );
	}

	// Landing 2: stochastic jitter for the supersampling path.
	// Hashed-coordinate function, stable per ray and decorrelated
	// across pixels.  Mirrors the MipHash used inside the accessor.
	inline double FootprintHash( double x, double y, double salt )
	{
		std::uint64_t u, v, w;
		std::memcpy( &u, &x,    sizeof(u) );
		std::memcpy( &v, &y,    sizeof(v) );
		std::memcpy( &w, &salt, sizeof(w) );
		std::uint64_t h = u * 0x9E3779B97F4A7C15ULL;
		h ^= v + 0x517CC1B727220A95ULL + (h << 6) + (h >> 2);
		h ^= w + 0x517CC1B727220A95ULL + (h << 6) + (h >> 2);
		h ^= h >> 33;
		h *= 0xff51afd7ed558ccdULL;
		h ^= h >> 33;
		return double( h >> 11 ) * (1.0 / double( 1ULL << 53 ));
	}
}

RISEColor TexturePainter::SampleTextured( const RayIntersectionGeometric& ri ) const
{
	// Landing 2: dispatch on the cached filter mode (resolved at
	// construction).  No virtual call for the dispatch decision;
	// the accessor's own virtuals fire only down the path actually
	// selected.  Returns a full RISEColor so callers get both the
	// RGB and alpha components from the SAME LOD pick — important
	// for alphaMode = BLEND consistency.
	if( ri.txFootprint.valid && filter_mode != Mode_Base ) {
		const Scalar texW = Scalar( pRIA->GetWidth() );
		const Scalar texH = Scalar( pRIA->GetHeight() );

		if( filter_mode == Mode_Pyramid ) {
			// Mip-pyramid path.  Compute texel-space LOD from
			// footprint × texture dimensions, dispatch to accessor.
			const Scalar lod = ComputeLODFromTexelFootprint(
				ri.txFootprint.dudx * texW, ri.txFootprint.dudy * texW,
				ri.txFootprint.dvdx * texH, ri.txFootprint.dvdy * texH );
			RISEColor c;
			pRIA->GetPELwithLOD( ri.ptCoord.y, ri.ptCoord.x, lod, c );
			return c;
		}

		// Mode_Supersample: stochastic supersampling within footprint.
		// Used when the accessor opted out of pyramid (lowmem mode).
		//
		// Pass the FULL 2x2 Jacobian (dudx, dudy, dvdx, dvdy) — the
		// accessor jitters within the screen-pixel parallelogram its
		// columns span, matching the integration region of a proper
		// mip lookup.  Earlier revisions collapsed the Jacobian to
		// two scalar magnitudes (per-axis L2 norms) and sampled an
		// axis-aligned bounding box; that biased the result on any
		// rotated or sheared UV chart by inflating the footprint.
		//
		// Hash salts pick stable per-ray, decorrelated-across-pixel
		// offsets — same hash family as the pyramid path's MipHash.
		// Use distinct Jacobian components as salts so neighbouring
		// pixels with the same hit point but different footprints
		// (e.g. silhouette vs interior of the same triangle) get
		// independent jitter.
		const double j1 = FootprintHash(
			ri.ptCoord.x, ri.ptCoord.y, ri.txFootprint.dudx );
		const double j2 = FootprintHash(
			ri.ptCoord.y, ri.ptCoord.x, ri.txFootprint.dvdy );
		RISEColor c;
		pRIA->GetPELwithFootprint(
			ri.ptCoord.y, ri.ptCoord.x,
			ri.txFootprint.dudx, ri.txFootprint.dudy,
			ri.txFootprint.dvdx, ri.txFootprint.dvdy,
			Scalar( j1 ), Scalar( j2 ),
			c );
		return c;
	}

	RISEColor c;
	pRIA->GetPEL( ri.ptCoord.y, ri.ptCoord.x, c );
	return c;
}

RISEPel TexturePainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	RISE_PROFILE_PHASE(TexturePainter);
	RISE_PROFILE_INC(nTexturePainterSamples);
	// Returns straight (un-premultiplied) RGB.  Earlier revisions
	// returned `c.base * c.a` — that's premultiplied alpha, which
	// disagrees with the glTF straight-alpha convention and double-
	// dims textured baseColor under alphaMode = BLEND once the shader-
	// op also weights by alpha.  For RGB images the loader fills
	// `c.a = 1` so this is a no-op; for RGBA images callers that need
	// premultiplied output should explicitly multiply with a
	// ChannelPainter(CHAN_A) via a ProductPainter.
	if( !pRIA ) {
		return RISEPel( 1.0, 1.0, 1.0 );
	}
	return SampleTextured( ri ).base;
}

Scalar TexturePainter::GetAlpha( const RayIntersectionGeometric& ri ) const
{
	RISE_PROFILE_PHASE(TexturePainter);
	RISE_PROFILE_INC(nTexturePainterSamples);
	// Returns the straight A channel of an RGBA texture (1.0 for RGB).
	// Used by the alpha-aware painter chain in the glTF importer for
	// alphaMode = MASK / BLEND.
	//
	// Landing 2: alpha is filtered at the SAME LOD as colour (via the
	// shared SampleTextured dispatch).  This is the principled choice:
	//   - alphaMode=BLEND: filtered alpha matches filtered colour, so
	//     the BLEND composite has consistent coverage and tint at every
	//     LOD.  Without this, minified BLEND assets shimmer because
	//     RGB band-limits but A doesn't.
	//   - alphaMode=MASK: filtered alpha at the threshold transition is
	//     handled by the path tracer's per-sample stochastic mip
	//     selection — averaged over spp the cutout boundary stays sharp
	//     in expectation while individual samples dither smoothly across
	//     the mip-blurred alpha edge.  At low spp the boundary may
	//     stipple visibly; at production spp it's correct.
	//
	// Earlier revisions hard-coded base-level alpha sampling on the
	// theory that MASK needs a crisp threshold input.  That broke BLEND
	// minification (color/alpha mismatch) and was rejected in adversarial
	// review.  The right place for sharpness policy is the threshold
	// shader-op (which can dither / stochastic-test), not the painter.
	if( pRIA ) {
		return SampleTextured( ri ).a;
	}
	return Scalar(1);
}
