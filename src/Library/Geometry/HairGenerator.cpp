//////////////////////////////////////////////////////////////////////
//
//  HairGenerator.cpp - Implementation of the groom generator.
//
//  THE ALGORITHM, in the order the code runs it.  Each numbered step is
//  a separate, composable effect that SKIPS CLEANLY when its parameter
//  is absent or zero, so a groom authored with nothing but `count` /
//  `length` is exactly "straight quills along the surface normal".
//
//    1. TESSELLATE THE BASE.  The base geometry is Realize()d (it may
//       itself be deferred -- a displaced_geometry base is legal and
//       common: displace a scalp, then grow on the displaced surface)
//       and tessellated through the universal TessellateToMesh
//       contract at `baseDetail`.  Roots live on THAT mesh, so a coarse
//       `base_detail` quantises where hair can grow.
//
//    2. AREA-WEIGHTED ROOT CANDIDATES.  A prefix-sum of triangle areas
//       plus a binary search picks each candidate's triangle with
//       probability proportional to its area -- so a groom is uniform
//       over the SURFACE, not over the triangulation (which would crowd
//       hair into wherever the tessellator happened to put small
//       triangles).  Barycentrics come from the standard
//       (1-sqrt(r1), sqrt(r1)(1-r2), sqrt(r1) r2) uniform-triangle map;
//       position, normal and UV are interpolated with them.
//
//    3. DENSITY MASK.  The optional `density` IScalarPainter is
//       evaluated at the candidate root through a SYNTHETIC
//       RayIntersectionGeometric (the build-time painter-evaluation
//       idiom -- ApplyDisplacementMapToObject / PainterPreview do the
//       same), clamped to [0,1], and used as a REJECTION probability.
//       `count` is therefore a budget, not a guarantee.
//
//    3b. THE ROOT TANGENT FRAME.  `comb` and `curl` are both expressed
//       in a frame {tangent, bitangent, normal} anchored at the root, so
//       that frame has to be CONTINUOUS ACROSS THE SURFACE or a constant
//       comb field reads as per-triangle noise.  The tangent is
//       therefore derived from the base's UV PARAMETERIZATION (dP/du),
//       which is continuous wherever the UV map is and is invariant to
//       how finely the base tessellates.  A base with no UVs (or a
//       UV-degenerate triangle) falls back to the triangle's first edge,
//       which IS per-triangle -- documented as such in the chunk
//       descriptor, because on such a base a comb field cannot be made
//       coherent at all.
//
//    4. GROWTH.  `segments` control points from the root along the
//       interpolated surface normal, total length `length` times the
//       optional `length_painter` at the root, then four independent
//       displacements per control point:
//         comb    -- a constant world-space lateral push, direction and
//                    magnitude decoded from the `comb` IPainter's RGB as
//                    a FLOW MAP (see the decode site: tangential
//                    components only, blue ignored)
//         gravity -- a world -Y push
//         curl    -- a helix superposed in the root's tangent frame
//         frizz   -- a per-control-point random jitter
//       comb / gravity / curl all use the TIP WEIGHT t^2 (t = the
//       control point's fraction along the strand), which is exactly 0
//       and has zero slope at t = 0: the strand leaves the follicle
//       along the surface normal no matter how it is styled, which is
//       what a real follicle does and what keeps roots from poking back
//       through the scalp.  Frizz uses a LINEAR t weight -- it is
//       high-frequency noise, not a bend, and quadratic weighting would
//       make the mid-strand look implausibly clean.
//
//    5. CLUMPING (second pass, once every strand exists).  See
//       `AssignClumps` for the cell scheme and why it is spatial rather
//       than index-strided.
//
//    6. WIDTHS + ROOT UV.  Root/tip widths are per-groom constants;
//       the base surface's UV at the root becomes the strand's
//       `rootUV`, which HairGeometry reports verbatim as `ri.ptCoord1`
//       so a `hair_material` can drive colour from a scalp texture.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "HairGenerator.h"
#include "../Interfaces/ILog.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Polygon.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//////////////////////////////////////////////////////////////////
	//  A self-contained, platform-independent PRNG.
	//
	//  Deliberately NOT RISE::GlobalRNG(): that is a shared, globally-
	//  seeded Mersenne Twister whose configuration is a compile-time
	//  #define (MERSENNE / MERSENNE53 / DRAND48 / rand()), so the same
	//  scene could groom differently between two builds of the same
	//  source.  A groom's determinism is a documented contract
	//  (HairGenerator.h), so it gets its own generator: a PCG-XSH-RR
	//  style 64-bit LCG with an output permutation.  Integer arithmetic
	//  only, in fixed-width types, so the stream is bit-identical
	//  everywhere.
	//////////////////////////////////////////////////////////////////
	struct HairRNG
	{
		uint64_t state;

		explicit HairRNG( uint64_t seed ) : state( 0 )
		{
			// Two rounds of the LCG on the raw seed before any output,
			// so neighbouring seeds (1, 2, 3 ...) -- which is exactly
			// how authors number them -- do not produce correlated
			// first draws.
			state = seed + 0x9E3779B97F4A7C15ull;
			Advance();
			Advance();
		}

		void Advance()
		{
			state = state * 6364136223846793005ull + 1442695040888963407ull;
		}

		uint32_t NextUInt32()
		{
			const uint64_t x = state;
			Advance();
			const uint32_t xorshifted = (uint32_t)( ( ( x >> 18 ) ^ x ) >> 27 );
			const uint32_t rot        = (uint32_t)( x >> 59 );
			return ( xorshifted >> rot ) | ( xorshifted << ( ( 32u - rot ) & 31u ) );
		}

		//! Uniform in the OPEN interval (0,1) -- never exactly 0 or 1,
		//! so a caller can divide by it or use it as a strict
		//! rejection threshold without a special case.
		double Canonical()
		{
			return ( (double)NextUInt32() + 0.5 ) * ( 1.0 / 4294967296.0 );
		}

		//! Symmetric in (-1, 1).
		double Signed()
		{
			return Canonical() * 2.0 - 1.0;
		}
	};

	//! Mixes two 32-bit values into a 64-bit stream key (SplitMix64's
	//! finalizer).  Used to give every strand its OWN jitter stream
	//! keyed on (seed, strand ordinal) -- see HairGenerator.h.
	uint64_t MixSeed( uint32_t a, uint32_t b )
	{
		uint64_t z = ( (uint64_t)a << 32 ) ^ ( (uint64_t)b + 0x9E3779B97F4A7C15ull );
		z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
		z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
		return z ^ ( z >> 31 );
	}

	//! A safe normalize: returns false (leaving `out` untouched) when
	//! the vector is too short or non-finite to have a direction.
	bool SafeNormalize( const Vector3& v, Vector3& out )
	{
		const Scalar m2 = Vector3Ops::SquaredModulus( v );
		if( !( m2 > Scalar(1e-24) ) || !RISE::IsFiniteDouble( m2 ) ) {
			return false;
		}
		out = v * ( Scalar(1.0) / sqrt( m2 ) );
		return true;
	}

	//! The synthetic hit a build-time painter evaluation is handed at a
	//! root.  Honest about what it knows -- position, surface normal and
	//! the base surface's (u,v) -- and silent about what it does not
	//! (no texture footprint, so mip-selecting painters fall back to
	//! their unfiltered path, which is the right answer for a point
	//! query).  Same shape as PainterPreview::MakePreviewRi.
	RayIntersectionGeometric MakeRootRi( const Point3& pos, const Vector3& normal, const Point2& uv )
	{
		RayIntersectionGeometric ri( Ray(), nullRasterizerState );
		ri.bHit            = true;
		ri.ptIntersection  = pos;
		ri.ptObjIntersec   = pos;
		ri.vNormal         = normal;
		ri.vGeomNormal     = normal;
		ri.ptCoord         = uv;
		ri.ptCoord1        = uv;
		ri.bHasTexCoord1   = true;
		return ri;
	}

	//! One generated root, before growth.
	struct RootSample
	{
		Point3  pos;
		Vector3 normal;		//!< unit
		Vector3 tangent;	//!< unit, perpendicular to `normal`
		Point2  uv;
		Scalar  length;		//!< post-`length_painter`
		Vector3 comb;		//!< world-space lateral push direction * magnitude; zero when unbound
	};

	//! Quantises a position onto the clump grid.  Returned as a
	//! 3-int key; std::map over it gives a deterministic iteration
	//! order (unlike a hash map), which matters because the clump
	//! CENTRE is defined as the first strand to land in a cell.
	struct CellKey
	{
		int64_t x, y, z;
		bool operator<( const CellKey& o ) const
		{
			if( x != o.x ) return x < o.x;
			if( y != o.y ) return y < o.y;
			return z < o.z;
		}
	};

	CellKey MakeCellKey( const Point3& p, const Scalar cell )
	{
		CellKey k;
		k.x = (int64_t)floor( p.x / cell );
		k.y = (int64_t)floor( p.y / cell );
		k.z = (int64_t)floor( p.z / cell );
		return k;
	}
}

//////////////////////////////////////////////////////////////////////
//  Recipe validation (parse time)
//////////////////////////////////////////////////////////////////////

bool RISE::Implementation::ValidateHairGroomRecipe( const HairGroomRecipe& recipe, const char* chunkName )
{
	const char* who = chunkName ? chunkName : "hair_geometry";
	const HairGroomParams& p = recipe.p;

	if( !recipe.pBase ) {
		GlobalLog()->PrintEx( eLog_Error, "%s:: `base_geometry` is required -- hair grows on a surface", who );
		return false;
	}
	if( !recipe.pBase->CanTessellate() ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: `base_geometry` cannot be tessellated (e.g. an infinite plane, or another hair_geometry) -- "
			"roots are area-sampled on the base's triangle mesh, so the base must support TessellateToMesh",
			who );
		return false;
	}
	if( p.count == 0 ) {
		GlobalLog()->PrintEx( eLog_Error, "%s:: `count` must be at least 1", who );
		return false;
	}
	if( p.count > kMaxHairStrandCount ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: `count` %u exceeds the %u-strand budget cap.  A groom that large is "
			"multiple gigabytes of control points and segment BVH; split it across several "
			"hair_geometry chunks if you really mean it",
			who, p.count, kMaxHairStrandCount );
		return false;
	}
	if( p.segments < 2 ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: `segments` must be at least 2 (a strand needs a root and a tip control point), got %u",
			who, p.segments );
		return false;
	}
	if( p.segments > HairGeometry::kMaxControlPointsPerStrand ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: `segments` %u exceeds the %u control-point-per-strand limit",
			who, p.segments, HairGeometry::kMaxControlPointsPerStrand );
		return false;
	}
	if( !( p.length > 0 ) || !RISE::IsFiniteDouble( p.length ) ) {
		GlobalLog()->PrintEx( eLog_Error, "%s:: `length` must be finite and strictly positive, got %g", who, p.length );
		return false;
	}
	if( !( p.widthRoot > 0 ) || !( p.widthTip > 0 ) ||
	    !RISE::IsFiniteDouble( p.widthRoot ) || !RISE::IsFiniteDouble( p.widthTip ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: `width_root` / `width_tip` must be finite and strictly positive, got %g / %g",
			who, p.widthRoot, p.widthTip );
		return false;
	}
	if( p.baseDetail == 0 ) {
		GlobalLog()->PrintEx( eLog_Error, "%s:: `base_detail` must be at least 1", who );
		return false;
	}
	if( p.curlRadius > 0 && !( p.curlStep > 0 ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: `curl_radius` %g is set but `curl_step` is %g -- a helix needs a strictly positive pitch",
			who, p.curlRadius, p.curlStep );
		return false;
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
//  Generation
//////////////////////////////////////////////////////////////////////

namespace
{
	//! Applies the clump pass IN PLACE.
	//!
	//! CLUMP CENTRES ARE SPATIAL, NOT INDEX-STRIDED.  The obvious
	//! scheme -- "every Nth generated root is a clump centre" -- does
	//! not work here, because roots come out of an AREA-WEIGHTED
	//! sampler: consecutive generation indices are uncorrelated points
	//! anywhere on the surface, so an index-strided centre would pull
	//! strands across the whole scalp.  Instead the root positions are
	//! quantised onto a grid of side `clumpSize` and each occupied
	//! cell's FIRST strand (lowest generation index, hence deterministic
	//! from the seed alone) becomes that cell's centre.  Every other
	//! strand in the cell is pulled toward it.  Cells are the Phase-1
	//! simplification of a true nearest-centre-within-radius search:
	//! clumps are cell-shaped and two strands either side of a cell
	//! boundary join different clumps.  That reads as clump variation
	//! at grooming scale; a radius search (and soft clump falloff) is a
	//! Phase-2 refinement.
	//!
	//! The pull is toward the centre strand's CORRESPONDING control
	//! point, weighted by `clump * t^2` with t the tip fraction, so
	//! roots never move (t = 0) and tips converge hardest.  The centre
	//! strand is its own centre and is therefore left untouched.
	void AssignClumps( std::vector<HairGeometry::StrandDesc>& strands,
	                   const Scalar clump, const Scalar clumpSize )
	{
		if( !( clump > 0 ) || !( clumpSize > 0 ) || strands.size() < 2 ) {
			return;
		}

		std::map<CellKey, size_t> centreOfCell;
		std::vector<size_t> centreIndex( strands.size(), 0 );
		std::vector<bool>   hasCentre( strands.size(), false );

		for( size_t i = 0; i < strands.size(); ++i ) {
			if( strands[i].controlPoints.empty() ) {
				continue;
			}
			const CellKey key = MakeCellKey( strands[i].controlPoints[0], clumpSize );
			std::map<CellKey, size_t>::const_iterator it = centreOfCell.find( key );
			if( it == centreOfCell.end() ) {
				centreOfCell[key] = i;			// first strand in this cell IS the centre
			} else if( it->second != i ) {
				centreIndex[i] = it->second;
				hasCentre[i]   = true;
			}
		}

		// The pull reads each centre strand IN PLACE, which is exactly
		// correct here and NOT a shortcut: the write loop below skips
		// every strand without a centre, and a centre never has one
		// (`hasCentre` is only set for the non-first strand of a cell),
		// so no strand this loop reads is ever a strand this loop
		// writes.  The result is therefore independent of iteration
		// order without needing a snapshot -- and a whole-groom deep
		// copy is not a cheap thing to take for reassurance: it doubles
		// peak groom memory at the exact moment the groom is largest.
		// A future scheme in which centres DO move would have to
		// reintroduce a snapshot (of the centre strands only).
		const Scalar clumpClamped = ( clump > Scalar(1) ) ? Scalar(1) : clump;

		for( size_t i = 0; i < strands.size(); ++i ) {
			if( !hasCentre[i] ) {
				continue;
			}
			const HairGeometry::StrandDesc& c = strands[ centreIndex[i] ];
			HairGeometry::StrandDesc& s = strands[i];

			const size_t n = s.controlPoints.size();
			if( n < 2 ) {
				continue;
			}
			for( size_t k = 1; k < n; ++k ) {
				// The centre may have a different control-point count
				// only if a future change makes `segments` per-strand;
				// today they match, but map by FRACTION so this stays
				// correct either way.
				const Scalar t = (Scalar)k / (Scalar)( n - 1 );
				const Scalar cf = t * (Scalar)( c.controlPoints.size() - 1 );
				const size_t c0 = (size_t)floor( cf );
				const size_t c1 = std::min( c0 + 1, c.controlPoints.size() - 1 );
				const Scalar cl = cf - (Scalar)c0;
				const Point3& a = c.controlPoints[c0];
				const Point3& b = c.controlPoints[c1];
				const Point3  target( a.x + ( b.x - a.x ) * cl,
				                      a.y + ( b.y - a.y ) * cl,
				                      a.z + ( b.z - a.z ) * cl );

				const Scalar w = clumpClamped * t * t;
				Point3& pk = s.controlPoints[k];
				pk.x += ( target.x - pk.x ) * w;
				pk.y += ( target.y - pk.y ) * w;
				pk.z += ( target.z - pk.z ) * w;
			}
		}
	}
}

bool RISE::Implementation::GenerateHairStrands(
	const HairGroomRecipe&                 recipe,
	const char*                            chunkName,
	std::vector<HairGeometry::StrandDesc>& out )
{
	const char* who = chunkName ? chunkName : "hair_geometry";
	const HairGroomParams& p = recipe.p;

	if( !recipe.pBase ) {
		GlobalLog()->PrintEx( eLog_Error, "%s:: cannot generate -- base geometry is null", who );
		return false;
	}

	// ---- 1. tessellate the base.  CASCADE first: the base may itself
	//         be deferred (a displaced_geometry scalp), in which case
	//         TessellateToMesh would re-emit an empty mesh until it is
	//         baked.  Same cascade DisplacedGeometry::BuildMesh does.
	recipe.pBase->Realize();

	IndexTriangleListType tris;
	VerticesListType      verts;
	NormalsListType       norms;
	TexCoordsListType     coords;

	if( !recipe.pBase->TessellateToMesh( tris, verts, norms, coords, p.baseDetail ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: base geometry does not support tessellation -- roots are area-sampled on its triangle mesh", who );
		return false;
	}
	if( tris.empty() || verts.empty() ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: base geometry tessellated to %u triangles / %u vertices -- nothing to grow hair on",
			who, (unsigned)tris.size(), (unsigned)verts.size() );
		return false;
	}

	const bool haveNormals = !norms.empty();
	const bool haveCoords  = !coords.empty();

	// ---- 2. area prefix sum over the triangles.
	std::vector<double> cumArea( tris.size(), 0.0 );
	double totalArea = 0.0;
	for( size_t i = 0; i < tris.size(); ++i ) {
		const IndexedTriangle& tri = tris[i];
		const Point3& a = verts[ tri.iVertices[0] ];
		const Point3& b = verts[ tri.iVertices[1] ];
		const Point3& c = verts[ tri.iVertices[2] ];
		const Vector3 e0 = Vector3Ops::mkVector3( b, a );
		const Vector3 e1 = Vector3Ops::mkVector3( c, a );
		double area = 0.5 * (double)Vector3Ops::Magnitude( Vector3Ops::Cross( e0, e1 ) );
		if( !RISE::IsFiniteDouble( area ) || area < 0.0 ) {
			area = 0.0;					// a degenerate / NaN triangle simply gets zero probability
		}
		totalArea += area;
		cumArea[i] = totalArea;
	}

	if( !( totalArea > 0.0 ) ) {
		GlobalLog()->PrintEx( eLog_Error,
			"%s:: base geometry tessellates to zero total surface area (%u triangles, all degenerate) -- "
			"there is no surface to area-sample roots on", who, (unsigned)tris.size() );
		return false;
	}

	// ---- 3-4. candidates, mask, growth.
	out.clear();
	out.reserve( p.count );

	unsigned int nMaskedOut   = 0;
	unsigned int nZeroLength  = 0;
	unsigned int nDegenerate  = 0;

	for( unsigned int si = 0; si < p.count; ++si )
	{
		// PLACEMENT STREAM, KEYED PER CANDIDATE.  One shared stream
		// walked across the whole loop would make every candidate's
		// placement depend on how many draws the candidates BEFORE it
		// happened to consume -- and that count is not constant: binding
		// a `density` painter adds a fourth draw per candidate, and a
		// candidate dropped on a degenerate triangle skips its remaining
		// draws entirely.  Either edit would then re-roll the position of
		// every LATER strand, so painting a bald patch on one ear would
		// silently rearrange the hair on the other.  Keying on (seed,
		// candidate ordinal) instead makes each candidate's draws depend
		// on nothing but its own ordinal: adding or editing `density`
		// leaves every SURVIVING strand's root, and its jitter, bit-
		// identical.  The tag folded into the key keeps this stream
		// disjoint from the per-strand jitter stream below, which is
		// keyed on the same ordinal.
		HairRNG placementRng( MixSeed( p.seed ^ 0x524F4F54u /* 'ROOT' */, si ) );

		// -- pick a triangle with probability proportional to its area.
		const double r = placementRng.Canonical() * totalArea;
		size_t ti = (size_t)( std::lower_bound( cumArea.begin(), cumArea.end(), r ) - cumArea.begin() );
		if( ti >= tris.size() ) {
			ti = tris.size() - 1;		// r == totalArea to the last ULP
		}
		const IndexedTriangle& tri = tris[ti];

		// -- uniform barycentrics on that triangle.
		const double r1 = placementRng.Canonical();
		const double r2 = placementRng.Canonical();
		const double sq = sqrt( r1 );
		const Scalar w0 = (Scalar)( 1.0 - sq );
		const Scalar w1 = (Scalar)( sq * ( 1.0 - r2 ) );
		const Scalar w2 = (Scalar)( sq * r2 );

		const Point3& v0 = verts[ tri.iVertices[0] ];
		const Point3& v1 = verts[ tri.iVertices[1] ];
		const Point3& v2 = verts[ tri.iVertices[2] ];

		RootSample root;
		root.pos = Point3( v0.x * w0 + v1.x * w1 + v2.x * w2,
		                   v0.y * w0 + v1.y * w1 + v2.y * w2,
		                   v0.z * w0 + v1.z * w1 + v2.z * w2 );

		// -- normal: interpolated shading normal when the base supplied
		//    one, else the triangle's own geometric normal.  Both can be
		//    degenerate (a zero-area triangle slipped through with zero
		//    probability only if totalArea is dominated elsewhere), in
		//    which case the candidate is dropped rather than grown along
		//    a fabricated direction.
		const Vector3 e0 = Vector3Ops::mkVector3( v1, v0 );
		const Vector3 e1 = Vector3Ops::mkVector3( v2, v0 );
		Vector3 nRaw = Vector3Ops::Cross( e0, e1 );
		if( haveNormals ) {
			const Vector3& n0 = norms[ tri.iNormals[0] ];
			const Vector3& n1 = norms[ tri.iNormals[1] ];
			const Vector3& n2 = norms[ tri.iNormals[2] ];
			const Vector3 nInterp = n0 * w0 + n1 * w1 + n2 * w2;
			if( Vector3Ops::SquaredModulus( nInterp ) > Scalar(1e-24) ) {
				nRaw = nInterp;
			}
		}
		if( !SafeNormalize( nRaw, root.normal ) ) {
			++nDegenerate;
			continue;
		}

		// -- the root's UV, and the root TANGENT derived from the same
		//    three texture coordinates.  The two are one block because
		//    they are one question -- what does the base's
		//    parameterization do here -- asked at value and at first
		//    derivative.
		//
		//    THE TANGENT COMES FROM THE UV PARAMETERIZATION.
		//    Solving the standard tangent-space system for dP/du --
		//    given the two edge vectors e0, e1 and their UV deltas
		//    (du1,dv1), (du2,dv2),
		//        dP/du = ( e0*dv2 - e1*dv1 ) / ( du1*dv2 - du2*dv1 )
		//    -- and orthogonalising against the shading normal gives a
		//    tangent field that is CONTINUOUS wherever the UV map is,
		//    and that does not move when `base_detail` changes the
		//    triangulation underneath it.  Both properties are what the
		//    comb field needs: a constant comb painter must sweep the
		//    whole surface ONE way, and a hand-painted comb map must
		//    mean at render time what it meant when it was painted.
		//
		//    The obvious alternative -- orthogonalise the triangle's
		//    FIRST EDGE against the normal -- is per-triangle: two
		//    triangles of the same quad disagree by up to ~60 degrees,
		//    so a constant comb map combs in a per-triangle
		//    checkerboard and re-rotates whenever the tessellation
		//    changes.  It survives only as the FALLBACK for a base that
		//    carries no UVs at all, or whose UV triangle is degenerate
		//    (|det| below): there is no parameterization there to derive
		//    from, so an incoherent-but-deterministic tangent is the
		//    best available answer, and the chunk descriptor says so.
		root.uv = Point2( 0, 0 );
		bool haveTangent = false;
		if( haveCoords ) {
			const Point2& c0 = coords[ tri.iCoords[0] ];
			const Point2& c1 = coords[ tri.iCoords[1] ];
			const Point2& c2 = coords[ tri.iCoords[2] ];
			root.uv = Point2( c0.x * w0 + c1.x * w1 + c2.x * w2,
			                  c0.y * w0 + c1.y * w1 + c2.y * w2 );

			const Scalar du1 = c1.x - c0.x, dv1 = c1.y - c0.y;
			const Scalar du2 = c2.x - c0.x, dv2 = c2.y - c0.y;
			const Scalar det = du1 * dv2 - du2 * dv1;
			// The threshold is a "is there a parameterization here at
			// all" test, not a quality bar: the result is normalised, so
			// a small-but-nonzero det costs no accuracy -- it only has
			// to stay clear of an overflowing reciprocal.
			if( RISE::IsFiniteDouble( det ) && fabs( det ) > Scalar(1e-20) ) {
				const Vector3 dPdu = ( e0 * dv2 - e1 * dv1 ) * ( Scalar(1) / det );
				Vector3 tUV = dPdu - root.normal * Vector3Ops::Dot( dPdu, root.normal );
				haveTangent = SafeNormalize( tUV, root.tangent );
			}
		}
		if( !haveTangent ) {
			Vector3 tRaw = e0 - root.normal * Vector3Ops::Dot( e0, root.normal );
			if( !SafeNormalize( tRaw, root.tangent ) ) {
				// Edge parallel to the normal (only possible for a
				// degenerate triangle): fall back to any perpendicular.
				const Vector3 helper = ( fabs( root.normal.x ) < Scalar(0.8) ) ? Vector3( 1, 0, 0 ) : Vector3( 0, 1, 0 );
				if( !SafeNormalize( Vector3Ops::Cross( helper, root.normal ), root.tangent ) ) {
					++nDegenerate;
					continue;
				}
			}
		}

		// -- the synthetic hit every root-side painter is evaluated at.
		const RayIntersectionGeometric rootRi = MakeRootRi( root.pos, root.normal, root.uv );

		// -- 3. density mask.  ONE canonical draw per candidate,
		//    consumed unconditionally when the painter is bound, so the
		//    stream position does not depend on the outcome.
		if( recipe.pDensity ) {
			Scalar d = recipe.pDensity->GetValuesAt( rootRi ).v[0];
			if( !RISE::IsFiniteDouble( d ) ) d = 0;
			if( d < 0 ) d = 0;
			if( d > 1 ) d = 1;
			if( placementRng.Canonical() >= (double)d ) {
				++nMaskedOut;
				continue;
			}
		}

		// -- per-strand length.
		root.length = (Scalar)p.length;
		if( recipe.pLengthScale ) {
			Scalar ls = recipe.pLengthScale->GetValuesAt( rootRi ).v[0];
			if( !RISE::IsFiniteDouble( ls ) || ls < 0 ) ls = 0;
			root.length *= ls;
		}
		if( !( root.length > 0 ) ) {
			++nZeroLength;
			continue;
		}

		// -- comb: the IPainter's RGB decoded as a TANGENT-SPACE
		//    direction by  d = 2*rgb - 1, then rotated into world by the
		//    root frame {tangent, bitangent, normal}.  A neutral
		//    0.5 0.5 0.5 painter is therefore exactly "no comb", and the
		//    vector's MAGNITUDE is the comb strength expressed as a
		//    fraction of the strand's own length (so 1.0 0.5 0.5 --
		//    d = (1,0,0) -- pushes the tip a full strand-length along
		//    +tangent).
		//
		//    THIS IS A FLOW MAP, NOT A NORMAL MAP.  The encoding borrows
		//    the normal map's 2*rgb-1 remap and nothing else: only the
		//    TANGENTIAL part of d survives (a comb must not push a
		//    strand into or out of the scalp -- growing along the normal
		//    is what the growth direction is already for), so the BLUE
		//    channel is projected straight back out and has no effect
		//    whatsoever.  The practical consequence, which surprises
		//    people who reach for a normal map here: a normal map's
		//    "flat" pixel 0.5 0.5 1.0 decodes to d = (0,0,1), which is
		//    pure normal and therefore ZERO comb -- flat blue is not
		//    "comb straight up", it is "do not comb".
		root.comb = Vector3( 0, 0, 0 );
		if( recipe.pComb ) {
			const RISEPel rgb = recipe.pComb->GetColor( rootRi );
			const Vector3 dTangentSpace( Scalar(2) * rgb[0] - Scalar(1),
			                             Scalar(2) * rgb[1] - Scalar(1),
			                             Scalar(2) * rgb[2] - Scalar(1) );
			const Vector3 bitangent = Vector3Ops::Cross( root.normal, root.tangent );
			Vector3 dWorld = root.tangent * dTangentSpace.x
			               + bitangent    * dTangentSpace.y
			               + root.normal  * dTangentSpace.z;
			dWorld = dWorld - root.normal * Vector3Ops::Dot( dWorld, root.normal );
			if( RISE::IsFiniteDouble( Vector3Ops::SquaredModulus( dWorld ) ) ) {
				root.comb = dWorld;
			}
		}

		// -- 4. growth.
		HairGeometry::StrandDesc sd;
		sd.controlPoints.resize( p.segments );
		sd.rootWidth = (Scalar)p.widthRoot;
		sd.tipWidth  = (Scalar)p.widthTip;
		sd.rootUV    = root.uv;

		// Per-strand jitter stream, keyed on (seed, this strand's
		// CANDIDATE ordinal).  Keyed on the candidate ordinal rather
		// than the surviving-strand ordinal so that a density painter
		// edit that removes an EARLIER strand does not reshuffle the
		// frizz of every later one.
		HairRNG strandRng( MixSeed( p.seed, si ) );

		// One phase offset per strand so curls do not all start at the
		// same angle (which would read as a corduroy pattern).
		const Scalar curlPhase = (Scalar)( strandRng.Canonical() * 2.0 * PI );

		const Vector3 bitangent = Vector3Ops::Cross( root.normal, root.tangent );
		const Scalar  spacing   = root.length / (Scalar)( p.segments - 1 );

		for( unsigned int k = 0; k < p.segments; ++k )
		{
			const Scalar t   = (Scalar)k / (Scalar)( p.segments - 1 );
			const Scalar tip = t * t;					// see the header comment on tip weighting
			const Scalar arc = root.length * t;

			Vector3 pos = Vector3( root.pos.x, root.pos.y, root.pos.z ) + root.normal * arc;

			// comb (world-space lateral push, quadratic tip weight)
			if( Vector3Ops::SquaredModulus( root.comb ) > 0 ) {
				pos = pos + root.comb * ( root.length * tip );
			}

			// gravity (world -Y, quadratic tip weight)
			if( p.gravity != 0.0 ) {
				pos.y -= (Scalar)p.gravity * root.length * tip;
			}

			// curl (helix in the root's tangent frame; tip-weighted so
			// the helix opens out of the follicle rather than starting
			// at full radius and lifting the root off the surface)
			if( p.curlRadius > 0 && p.curlStep > 0 ) {
				const Scalar theta = curlPhase + Scalar(2) * (Scalar)PI * ( arc / (Scalar)p.curlStep );
				const Scalar rad   = (Scalar)p.curlRadius * tip;
				pos = pos + root.tangent * ( rad * cos( theta ) )
				          + bitangent    * ( rad * sin( theta ) );
			}

			// frizz (per-control-point jitter; LINEAR tip weight, and
			// scaled by the strand's own control-point spacing so the
			// same `frizz` value looks the same on a long strand and a
			// short one).  Three draws are consumed for EVERY control
			// point including the root, so the stream position is a
			// function of k alone.
			{
				const Scalar jx = (Scalar)strandRng.Signed();
				const Scalar jy = (Scalar)strandRng.Signed();
				const Scalar jz = (Scalar)strandRng.Signed();
				if( p.frizz != 0.0 ) {
					const Scalar amp = (Scalar)p.frizz * spacing * t;
					pos = pos + Vector3( jx, jy, jz ) * amp;
				}
			}

			sd.controlPoints[k] = Point3( pos.x, pos.y, pos.z );
		}

		out.push_back( sd );
	}

	// ---- 5. clumping.
	AssignClumps( out, (Scalar)p.clump, (Scalar)p.clumpSize );

	// ---- diagnostics: one summary line, never one per strand.
	if( out.empty() ) {
		GlobalLog()->PrintEx( eLog_Warning,
			"%s:: generated 0 strands from %u candidates (%u masked out by `density`, %u zero-length, "
			"%u on degenerate triangles) -- the groom will be invisible",
			who, p.count, nMaskedOut, nZeroLength, nDegenerate );
	} else {
		GlobalLog()->PrintEx( eLog_Info,
			"%s:: grew %u strands of %u control points from %u candidates on %u base triangles "
			"(%u masked out by `density`, %u zero-length, %u degenerate)",
			who, (unsigned)out.size(), p.segments, p.count, (unsigned)tris.size(),
			nMaskedOut, nZeroLength, nDegenerate );
	}

	return true;
}
