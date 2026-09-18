//////////////////////////////////////////////////////////////////////
//
//  TranslucentBSDF.cpp - Implements the translucent BSDF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "TranslucentBSDF.h"
#include "TranslucentSPF.h"

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Implementation::TranslucentSPFDetail;

namespace
{
	// DL-112 review P1-2 (2026-09-17).
	//
	// DL-112 renormalized `TranslucentSPF`'s entry front-reflection lobe:
	// the SAMPLER now draws the exact 2-draw remap onto the geometrically
	// valid region (rather than dropping a below-horizon trial) and
	// `TranslucentSPF::Pdf` reports the matching NORMALIZED conditional
	// density `cos/pi / P(valid)`.  `TranslucentBSDF::value` was left at
	// the bare `pRefFront * INV_PI`, so the two techniques that estimate
	// the same integral stopped agreeing:
	//
	//     kray  ==  value * cos / pdf
	//
	// held exactly before DL-112 and afterwards read `1 / P(valid)` --
	// measured over the SPF's own draws at 40k trials, reflectance 0.5:
	// 1.00000 / 1.07180 / 1.17157 / 1.33333 / 1.58879 / 1.96569 at tilts
	// 0 / 30 / 45 / 60 / 75 / 89 degrees, matching `2/(1+cos phi)` to
	// five digits (tests/SPFBSDFConsistencyTest.cpp Part F).
	//
	// `TranslucentBSDF` is LIVE on that path: PT's NEE
	// (`PathTracingIntegrator.cpp` -> `LightSampler::EvaluateDirectLighting`)
	// and BDPT / VCM connections (`BDPTIntegrator.cpp`) all read
	// `GetBSDF()->value`, so a BSDF-sampled continuation carried the full
	// albedo while a connection to the SAME direction was valued at
	// `albedo * P(valid)` -- up to 2x apart at grazing shading tilt, and
	// any MIS combination of the two is then biased (the DL-74 lesson).
	//
	// THE RULING (docs/DL111_DL112_TRANSMISSION_PUSH_GATES.md): the
	// clipped lobe always existed only on the valid side, so the value
	// follows the sampler -- clip it to the same half-space and divide by
	// the same `P(valid)`.  The lobe's albedo is then the painter's
	// reflectance at EVERY tilt (integral of `rho/(pi P) * cos` over the
	// valid region = `rho/(pi P) * pi P` = `rho`), which is exactly what
	// the sampler's `kray` has been claiming all along.  The alternative
	// -- scaling `kray` DOWN by `P(valid)` instead -- would restore
	// agreement at the cost of reinstating DL-112's own energy loss.
	//
	// Identity at zero tilt (P(valid) == 1), so no untilted fixture moves.
	//
	// Returns false when the valid region has vanished, matching
	// `TranslucentSPF::Pdf`'s own `return 0` for the same configuration
	// (unreachable from production once the axis is oriented, since
	// P(valid) >= 0.5 by construction).
	inline bool FrontLobeValidFraction(
		const Vector3& vLightIn, const RayIntersectionGeometric& ri, Scalar& outPValid )
	{
		const Vector3 n = ri.onb.w();
		// Same recovery TranslucentSPF::Scatter/Pdf perform: the TRUE
		// (un-flipped) geometric normal where one exists (DL-70), the
		// shading normal for hair / a degenerate normal, then anchored to
		// the ray -- a REFLECTION must stay on the side the incoming ray
		// arrived from.
		const Vector3 trueGeomNormal = ri.HasTrueGeomSide() ? ri.UnflippedGeomNormal() : n;
		const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( trueGeomNormal ) > Scalar(1e-12) )
			? trueGeomNormal : n;
		const Vector3 geomN = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;

		const Vector3 nFront = OrientedLobeAxis( n, geomN );
		const Scalar pValid = ExitValidFraction( nFront, geomN );
		if( pValid < kExitVanishThreshold ) return false;

		// Same support the sampler and the density have: inside the
		// oriented lobe's own hemisphere AND across the geometric horizon.
		// (Sign tests only, so an unnormalized `vLightIn` is fine -- the
		// two callers below hand this the same vector `GetReflectedSide`
		// normalizes for its own use.)
		if( Vector3Ops::Dot( vLightIn, nFront ) <= 0 ) return false;
		if( Vector3Ops::Dot( vLightIn, geomN )  <= 0 ) return false;

		outPValid = pValid;
		return true;
	}
}

TranslucentBSDF::TranslucentBSDF( const IPainter& rF, const IPainter& T, const IScalarPainter& exp ) :
  pRefFront( &rF ), pTrans( &T ), pExponent( &exp )
{
	pRefFront->addref();
	pTrans->addref();
	pExponent->addref();
}

TranslucentBSDF::~TranslucentBSDF( )
{
	safe_release( pRefFront );
	safe_release( pTrans );
	safe_release( pExponent );
}

void TranslucentBSDF::SetRefFront( const IPainter& v )      { v.addref(); safe_release( pRefFront ); pRefFront = &v; }
void TranslucentBSDF::SetTrans( const IPainter& v )         { v.addref(); safe_release( pTrans );    pTrans    = &v; }
void TranslucentBSDF::SetN( const IScalarPainter& v )       { v.addref(); safe_release( pExponent ); pExponent = &v; }

template< class T >
static char GetReflectedSide( T& intensity, const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Vector3& n, const T& exponent )
{
	Vector3 v = Vector3Ops::Normalize(vLightIn);
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir());

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	// Build the specular lobe direction from |n.r| (nrSigned is a magnitude,
	// not a signed, per-branch mirror-reflect of r about n the way
	// IsotropicPhongBRDF::ComputeDiffuseSpecularFactors does it) -- this is
	// a pre-existing, intentionally-simple approximation: GetReflectedSide's
	// case 0 (the transmission lobe, `intensity` consumed by value()'s
	// `case 0`) fires for TWO geometrically distinct configurations (viewer
	// front/light back, and viewer back/light front) and reuses this one
	// `sd` formula for both, where IsotropicPhongBRDF has no equivalent
	// mismatched-sign case to be equivalent to (its two branches only cover
	// matched-sign viewer/light pairs).  What actually changed here: the old
	// construction dotted an UN-normalized `incident` (|incident|^2 = 1+8nr^2
	// for nr<0) against v, so sd could exceed 1 and pow(sd, exponent) could
	// blow up to Inf for a tilted shading frame.  Normalizing before the dot
	// bounds sd to [0,1] by construction, which is the actual fix -- it does
	// not make this construction equivalent to a Householder mirror
	// reflection for the nr<0 sub-case.
	Scalar nrSigned = nr;
	if( nrSigned <= 0 ) {
		nrSigned = -nrSigned;
	}

	const Point3 incident = Point3Ops::mkPoint3(Point3( r.x, r.y, r.z ), ((nrSigned*-2.0)*n));
	const Scalar sd = fabs( Vector3Ops::Dot( Vector3Ops::Normalize(Vector3( incident.x, incident.y, incident.z )), v ) );
	intensity = pow( sd, exponent );

	if( nr <= /*-NEARZERO*/ 0 )						// viewer front
	{
		if( nv <= -NEARZERO ) {						// light front
			return 1;
		} else {									// light back
			return 0;
		}
	}
	else if( nr >= NEARZERO )						// viewer back
	{
		if( nv >= NEARZERO) {						// light back
			return 2;
		} else {									// light front
			return 0;
		}
	}

	return 3;
}


RISEPel TranslucentBSDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel intensity = RISEPel(1,1,1);
	const ScalarTriple exp_t = pExponent->GetValuesAt(ri);
	const RISEPel exp_rgb( exp_t.v[0], exp_t.v[1], exp_t.v[2] );
	switch( GetReflectedSide<RISEPel>(intensity, vLightIn, ri, ri.onb.w(), exp_rgb ) )
	{
	case 0:
		return pTrans->GetColor(ri) * intensity * INV_PI;
		break;
	case 1:
		// Viewer and light BOTH on the -n side.  Despite the branch
		// comments in GetReflectedSide (whose "viewer front" is named
		// from `Dot(n, -ray.Dir())`, the OPPOSITE sense from the
		// geometric front face), this is the INTERIOR reflection -- an
		// ordinary front-face hit with the light outside returns case 2.
		// Its SPF counterpart is the exit branch's Phong backscatter lobe
		// (`pTrans * scattering`), not this `pRefFront` cosine; that
		// mismatch is pre-existing and is NOT DL-112's, so it is
		// deliberately left alone.  Renormalizing it would silently
		// change a lobe this expression never described in the first
		// place.
		return pRefFront->GetColor(ri) * INV_PI;
		break;
	case 2:
	{
		// Viewer and light both on the +n side: the ENTRY
		// front-reflection lobe, the one DL-112 renormalized.  See
		// FrontLobeValidFraction above for the full derivation.
		Scalar pValid = 1.0;
		if( !FrontLobeValidFraction( vLightIn, ri, pValid ) ) return RISEPel(0,0,0);
		return pRefFront->GetColor(ri) * ( INV_PI / pValid );
	}
	default:
	case 3:
		return RISEPel(0,0,0);
		break;
	}
}

Scalar TranslucentBSDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar intensity = 1.0;
	switch( GetReflectedSide<Scalar>(intensity, vLightIn, ri, ri.onb.w(), pExponent->GetValueAtNM(ri,nm) ) )
	{
	case 0:
		return GuardedGetColorNM( *pTrans, ri, nm ) * intensity * INV_PI;
		break;
	case 1:
		// Interior reflection -- see the RGB twin's `case 1` note.
		return GuardedGetColorNM( *pRefFront, ri, nm ) * intensity * INV_PI;
		break;
	case 2:
	{
		// DL-112, NM twin of the RGB entry front-reflection branch.
		Scalar pValid = 1.0;
		if( !FrontLobeValidFraction( vLightIn, ri, pValid ) ) return 0;
		return GuardedGetColorNM( *pRefFront, ri, nm ) * intensity * ( INV_PI / pValid );
	}
	default:
	case 3:
		return 0;
		break;
	}
}

RISEPel TranslucentBSDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Only the reflective lobe returns energy back toward the camera —
	// transmitted energy reaches the OIDN beauty pass via what's behind
	// the surface, not via this BSDF's albedo AOV.
	return pRefFront->GetColor( ri );
}
