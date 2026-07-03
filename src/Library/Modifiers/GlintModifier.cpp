//////////////////////////////////////////////////////////////////////
//
//  GlintModifier.cpp - Implementation of the discrete-facet glint
//  normal modifier.  See GlintModifier.h for the model and
//  docs/ENAMEL_SPARKLE_BRDF.md for the design + ratification record.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 3, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GlintModifier.h"
#include "../Utilities/math_utils.h"
#include "../Utilities/Math3D/Math3D.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! Hard safety ceiling on the facet tilt (radians).  The Rayleigh
	//! tail is unbounded; a facet tilted anywhere near the tangent
	//! plane is un-physical for this model (real flakes lie roughly
	//! in-plane) and is where the shading-normal energy asymmetry
	//! bites.  60 degrees is far beyond any sane `spread` (single-digit
	//! degrees) so the clamp is invisible in practice.
	const Scalar GLINT_THETA_MAX = Scalar(1.0471975511965976);		// 60 deg

	//! Minimum cosine between the perturbed normal and the (orientation-
	//! matched) geometric normal.  Below this the tilt is rejected and
	//! the hit keeps its smooth normal — prevents facets from flipping
	//! past the geometric tangent plane on curved/bumpy bases where the
	//! shading normal already leans away from the geometric one.
	const Scalar GLINT_MIN_GEOM_COS = Scalar(0.05);
}

GlintModifier::GlintModifier(
	const Scalar density_,
	const Scalar coverage_,
	const Scalar fill_,
	const Scalar spreadDeg_,
	const Vector3& vScale_,
	const Vector3& vShift_,
	const unsigned int seed_
	) :
  density( density_ ),
  coverage( r_max( Scalar(0), r_min( Scalar(1), coverage_ ) ) ),
  fill( r_min( Scalar(1), fill_ ) ),
  spreadRad( spreadDeg_ * DEG_TO_RAD ),
  vScale( vScale_ ),
  vShift( vShift_ ),
  seed( seed_ )
{
}

GlintModifier::~GlintModifier( )
{
}

GlintFacet GlintModifier::FindFacet( const Point3& objPt ) const
{
	GlintFacet result;

	// Inert configurations: no cells, no coverage, no facet area, or no
	// tilt to apply.  fill <= 0 is inert like density/spread (a facet
	// with zero radius is no facet), keeping the authored-domain
	// semantics uniform across all four scalars.
	if( !(density > 0) || !(coverage > 0) || !(fill > 0) || !(spreadRad > 0) ) {
		return result;
	}

	// Scaled cell space ("q-space"): Worley3DPainter convention
	// (pt*scale + shift), then density cells per unit.
	const Vector3 q(
		( objPt.x * vScale.x + vShift.x ) * density,
		( objPt.y * vScale.y + vShift.y ) * density,
		( objPt.z * vScale.z + vShift.z ) * density );

	const double fx = std::floor( (double)q.x );
	const double fy = std::floor( (double)q.y );
	const double fz = std::floor( (double)q.z );

	// Facet disc radius in q-space.  fill in (0,1] keeps r <= 0.5, so a
	// facet's disc can only reach into the immediately adjacent cells —
	// the 3x3x3 sweep below is sufficient (no clipped facets).
	const Scalar r = fill * Scalar(0.5);
	const Scalar r2 = r * r;

	Scalar bestD2 = r2;
	bool bestFound = false;
	Vector3 bestCentre( 0, 0, 0 );
	unsigned int bestLane = 0;

	for( int dz = -1; dz <= 1; dz++ ) {
		for( int dy = -1; dy <= 1; dy++ ) {
			for( int dx = -1; dx <= 1; dx++ )
			{
				const double cxd = fx + (double)dx;
				const double cyd = fy + (double)dy;
				const double czd = fz + (double)dz;

				const unsigned int h0 = GlintHash::HashCell(
					GlintHash::FoldToCellIndex( cxd ),
					GlintHash::FoldToCellIndex( cyd ),
					GlintHash::FoldToCellIndex( czd ),
					seed );

				// Per-cell Bernoulli existence draw (lane 0).
				if( GlintHash::U01( h0 ) >= coverage ) {
					continue;
				}

				// Jittered facet centre inside the cell (lanes 1-3).
				const unsigned int hA = GlintHash::HashU32( h0 );
				const unsigned int hB = GlintHash::HashU32( hA );
				const unsigned int hC = GlintHash::HashU32( hB );

				const Vector3 centre(
					Scalar( cxd ) + GlintHash::U01( hA ),
					Scalar( cyd ) + GlintHash::U01( hB ),
					Scalar( czd ) + GlintHash::U01( hC ) );

				// 3D distance test: the facet is the intersection of a
				// sphere around the centre with the surface, so apparent
				// facet size varies naturally with how centrally the
				// surface slices the sphere.  Nearest centre wins when
				// discs overlap (deterministic, ties measure-zero).
				const Vector3 d( q.x - centre.x, q.y - centre.y, q.z - centre.z );
				const Scalar d2 = d.x*d.x + d.y*d.y + d.z*d.z;

				if( d2 < bestD2 ) {
					bestD2 = d2;
					bestFound = true;
					bestCentre = centre;
					bestLane = hC;
				}
			}
		}
	}

	if( !bestFound ) {
		return result;
	}

	// Tilt lanes (4-5) from the winning facet: Rayleigh-distributed
	// polar angle with scale `spreadRad` (theta = sigma*sqrt(-2 ln(1-u))),
	// uniform azimuth.  The 1-u argument is bounded away from 0 so the
	// log never blows up; the safety ceiling truncates a far tail whose
	// mass is ~1e-87 at sigma=3deg and ~1.5e-8 at sigma=10deg (it only
	// meaningfully engages for hostile spreads, e.g. ~32% at 40deg).
	const unsigned int hD = GlintHash::HashU32( bestLane );
	const unsigned int hE = GlintHash::HashU32( hD );

	const Scalar u1 = GlintHash::U01( hD );
	const Scalar u2 = GlintHash::U01( hE );

	Scalar theta = spreadRad * std::sqrt( Scalar(-2.0) * std::log( r_max( Scalar(1.0) - u1, Scalar(1e-7) ) ) );
	if( theta > GLINT_THETA_MAX ) {
		theta = GLINT_THETA_MAX;
	}

	result.found = true;
	result.centreQ = bestCentre;
	result.theta = theta;
	result.phi = TWO_PI * u2;
	return result;
}

void GlintModifier::Modify( RayIntersectionGeometric& ri ) const
{
	const GlintFacet facet = FindFacet( ri.ptObjIntersec );
	if( !facet.found ) {
		return;
	}

	// Express the facet tilt in the hit's smooth shading frame.  The
	// tilt angles are frame-independent facet properties; the frame
	// varies negligibly across one facet (facets are ~sub-pixel, base
	// curvature is macro-scale), so the facet normal is effectively
	// constant across its disc.  One known seam: a facet whose disc
	// straddles the world +/-Y pole line sees CreateFromW's canonical
	// tangent flip, so its azimuth jumps there (a localized cosmetic
	// discontinuity on general closed surfaces; unreachable on the
	// enamel dome, whose normals stay near +Z).
	const Scalar st = std::sin( facet.theta );
	const Scalar ct = std::cos( facet.theta );
	const Scalar cp = std::cos( facet.phi );
	const Scalar sp = std::sin( facet.phi );

	const Vector3 newN = Vector3Ops::Normalize(
		ri.onb.u() * ( st * cp ) +
		ri.onb.v() * ( st * sp ) +
		ri.onb.w() * ct );

	// Safety guard: the facet must stay on the outward side of the
	// GEOMETRIC surface (vGeomNormal, orientation-matched to the
	// shading normal in case the geometry flipped one of them).  A
	// facet past the geometric tangent plane is where the classic
	// shading-normal energy asymmetry bites; reject to the smooth
	// normal instead of emitting a near-tangent frame.
	const Scalar geomSign = ( Vector3Ops::Dot( ri.vNormal, ri.vGeomNormal ) >= 0 ) ? Scalar(1) : Scalar(-1);
	if( Vector3Ops::Dot( newN, ri.vGeomNormal ) * geomSign < GLINT_MIN_GEOM_COS ) {
		return;
	}

	// Replace the shading normal and rebuild the ONB about it,
	// PRESERVING the tangent direction (project the old u onto the new
	// tangent plane) rather than CreateFromW's canonical-axis pick —
	// keeps geometry-defined tangents (bShadingTangentFromGeometry,
	// anisotropic tangent_rotation consumers) coherent across facets.
	const Vector3 oldU = ri.onb.u();
	ri.vNormal = newN;

	const Vector3 uProj = oldU - newN * Vector3Ops::Dot( oldU, newN );
	if( Vector3Ops::SquaredModulus( uProj ) > Scalar(1e-12) ) {
		ri.onb.CreateFromWU( newN, uProj );
	} else {
		// Degenerate only if the tilt swung the normal onto the old
		// tangent — impossible under the 60-degree ceiling, but guard
		// the fallback anyway.
		ri.onb.CreateFromW( newN );
	}
}
