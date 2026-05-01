//////////////////////////////////////////////////////////////////////
//
//  AlphaTestShaderOp.cpp - Implementation.  See header for the
//  rationale, integrator-compatibility caveats, and Phase 3 design.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "AlphaTestShaderOp.h"
#include "../Utilities/Color/Color_Template.h"

using namespace RISE;
using namespace RISE::Implementation;

AlphaTestShaderOp::AlphaTestShaderOp(
	const IPainter& alpha_painter,
	const Scalar    cutoff_
	) :
  alphaPainter( alpha_painter ),
  cutoff( cutoff_ )
{
	alphaPainter.addref();
}

AlphaTestShaderOp::~AlphaTestShaderOp()
{
	alphaPainter.release();
}

void AlphaTestShaderOp::PerformOperation(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	RISEPel& c,
	const IORStack& ior_stack,
	const ScatteredRayContainer* /*pScat*/
	) const
{
	// Sample alpha at the hit and decide whether to keep or skip.
	//
	// The "alpha" here is whatever the supplied painter decides to
	// broadcast -- typically a ChannelPainter wired to the A channel
	// (CHAN_A=3) of an RGBA texture (Phase 4 added IPainter::GetAlpha
	// for un-premultiplied access; ChannelPainter routes CHAN_A there).
	// Hand-authored uses of this op may pass any painter that
	// broadcasts the intended alpha; we still take max(R,G,B) so that
	// a single-channel painter (broadcast as v,v,v) is interpreted
	// correctly without depending on which channel callers pick.
	const Scalar alpha = ColorMath::MaxValue( alphaPainter.GetColor( ri.geometric ) );

	if( alpha >= cutoff ) {
		// Surface is opaque enough; leave `c` untouched and let the next
		// op in the pipeline (the actual surface BSDF shader) handle the hit.
		return;
	}

	// Surface is below the cutoff.  Continue the ray past the hit and
	// shade whatever is behind into `c`, exactly as TransparencyShaderOp
	// does for fully transparent surfaces -- but as a deterministic
	// keep/skip rather than a blend.
	Ray ray = ri.geometric.ray;
	ray.Advance( ri.geometric.range + 2e-8 );

	RISEPel cthis;
	if( caster.CastRay( rc, ri.geometric.rast, ray, cthis, rs, 0, ri.pRadianceMap, ior_stack ) ) {
		c = cthis;
	}
}

Scalar AlphaTestShaderOp::PerformOperationNM(
	const RuntimeContext& rc,
	const RayIntersection& ri,
	const IRayCaster& caster,
	const IRayCaster::RAY_STATE& rs,
	const Scalar caccum,
	const Scalar nm,
	const IORStack& ior_stack,
	const ScatteredRayContainer* /*pScat*/
	) const
{
	const Scalar alpha = alphaPainter.GetColorNM( ri.geometric, nm );

	if( alpha >= cutoff ) {
		return caccum;	// Leave the running NM value alone; next op shades it.
	}

	Ray ray = ri.geometric.ray;
	ray.Advance( ri.geometric.range + 2e-8 );

	Scalar c = 0;
	if( caster.CastRayNM( rc, ri.geometric.rast, ray, c, rs, nm, 0, ri.pRadianceMap, ior_stack ) ) {
		return c;
	}
	return caccum;
}
