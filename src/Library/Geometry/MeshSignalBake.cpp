//////////////////////////////////////////////////////////////////////
//
//  MeshSignalBake.cpp - Implementation of the per-vertex occlusion /
//  thickness / convexity bakes and their lazy find-or-build cache.
//
//  docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §7 (Phase 3);
//  docs/OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md §4 for the convexity bake
//  (and for why the occlusion bake needed no change: its cosine-weighted
//  hemisphere already equals the accessibility-based definition on every
//  wedge feature).
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MeshSignalBake.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/FiniteMath.h"
#include "../Utilities/RTime.h"
#include "../Interfaces/ILog.h"
#include <cmath>
#include <mutex>

using namespace RISE;

namespace
{
	//! Van der Corput radical inverse, base 2.  The stratified half of the
	//! Hammersley pair below.  Deterministic by construction -- that is the
	//! whole reason a bake uses a low-discrepancy SEQUENCE and not an RNG:
	//! nothing about the result may depend on which thread won the
	//! find-or-build race or on when it ran.
	inline Scalar RadicalInverse2( unsigned int i )
	{
		i = ( i << 16 ) | ( i >> 16 );
		i = ( ( i & 0x55555555u ) << 1 ) | ( ( i & 0xAAAAAAAAu ) >> 1 );
		i = ( ( i & 0x33333333u ) << 2 ) | ( ( i & 0xCCCCCCCCu ) >> 2 );
		i = ( ( i & 0x0F0F0F0Fu ) << 4 ) | ( ( i & 0xF0F0F0F0u ) >> 4 );
		i = ( ( i & 0x00FF00FFu ) << 8 ) | ( ( i & 0xFF00FF00u ) >> 8 );
		return Scalar( i ) * Scalar( 2.3283064365386963e-10 );	// / 2^32
	}

	//! Per-vertex azimuth rotation, so 64 samples do not land on the same 64
	//! directions at every vertex (which reads as banding once the table is
	//! barycentrically interpolated).  A function of the vertex INDEX, not
	//! of any running state, so it is still reproducible.
	inline Scalar GoldenRotation( const size_t vertexIndex )
	{
		const Scalar g = Scalar( 0.61803398874989485 );
		const Scalar x = Scalar( vertexIndex ) * g;
		return x - std::floor( x );
	}

	//! Cosine-weighted direction in the hemisphere around +W of `onb`,
	//! restricted to a cone of half-angle `maxTheta`.  `maxTheta = pi/2` is
	//! the full hemisphere.
	//!
	//! Sampling: cos^2(theta) is uniform on [cos^2(maxTheta), 1] for a
	//! cosine-weighted cone, so sin(theta) = sqrt(u * sin^2(maxTheta)).
	inline Vector3 CosineConeDirection(
		const OrthonormalBasis3D& onb,
		const Scalar u1, const Scalar u2,
		const Scalar sin2MaxTheta )
	{
		const Scalar sinTheta2 = u1 * sin2MaxTheta;
		const Scalar sinTheta  = std::sqrt( sinTheta2 );
		const Scalar cosTheta  = std::sqrt( ( sinTheta2 < Scalar(1) ) ? ( Scalar(1) - sinTheta2 ) : Scalar(0) );
		const Scalar phi       = Scalar( TWO_PI ) * u2;
		const Vector3 local( sinTheta * std::cos( phi ), sinTheta * std::sin( phi ), cosTheta );
		return Vector3(
			onb.u().x * local.x + onb.v().x * local.y + onb.w().x * local.z,
			onb.u().y * local.x + onb.v().y * local.y + onb.w().y * local.z,
			onb.u().z * local.x + onb.v().z * local.y + onb.w().z * local.z );
	}

	//! UNIFORM-in-solid-angle direction in the hemisphere around +W.  The
	//! convexity bake needs this rather than the cosine-weighted form above
	//! because it estimates a SOLID-ANGLE fraction (`A`, the accessibility),
	//! not an irradiance-like quantity: a cosine weight would tilt the
	//! estimate toward the pole and stop it agreeing with the SDF family's
	//! ball-volume `A` on the wedge features both are meant to find.
	//!
	//! Emitted for the UPPER hemisphere only; the convexity bake uses each
	//! direction with both signs, so the full sphere is covered by antipodal
	//! PAIRS.  That pairing is what resolves the half of the sphere pointing
	//! INTO the solid as well as the open half -- and half of the sphere
	//! pointing into the solid is precisely what makes `A = 1/2` on a plane.
	inline Vector3 UniformHemisphereDirection(
		const OrthonormalBasis3D& onb, const Scalar u1, const Scalar u2 )
	{
		const Scalar cosTheta = u1;
		const Scalar sinTheta = std::sqrt( ( cosTheta < Scalar(1) ) ? ( Scalar(1) - cosTheta*cosTheta ) : Scalar(0) );
		const Scalar phi      = Scalar( TWO_PI ) * u2;
		const Vector3 local( sinTheta * std::cos( phi ), sinTheta * std::sin( phi ), cosTheta );
		return Vector3(
			onb.u().x * local.x + onb.v().x * local.y + onb.w().x * local.z,
			onb.u().y * local.x + onb.v().y * local.y + onb.w().y * local.z,
			onb.u().z * local.x + onb.v().z * local.y + onb.w().z * local.z );
	}
}

std::atomic<unsigned int>& MeshSignalBake::BuildCounter()
{
	// Function-local static inside a non-inline function: exactly one
	// instance across the process, same shape as SurfaceSignalDemand's
	// counter.
	static std::atomic<unsigned int> counter( 0 );
	return counter;
}

bool MeshSignalBake::Build( const Kind kind, const Input& in, std::vector<float>& out )
{
	// Counted whether or not it succeeds: the counter answers "did a build
	// ATTEMPT happen", which is what the draft-mode proof needs (a failed
	// build that ran is still work draft must not have paid for).
	BuildCounter().fetch_add( 1, std::memory_order_relaxed );

	if( !in.pVertices || !in.pNormals || !in.pOccluder ) {
		return false;
	}
	const std::vector<Point3>&  verts   = *in.pVertices;
	const std::vector<Vector3>& normals = *in.pNormals;
	if( verts.empty() || normals.size() != verts.size() ) {
		return false;
	}
	if( !RISE::IsFiniteDouble( static_cast<double>( in.maxDistance ) ) || !( in.maxDistance > Scalar(0) ) ) {
		return false;
	}

	// Cone geometry.  Occlusion samples the full outward hemisphere (that
	// IS the exposure measure); thickness samples a narrow INWARD cone --
	// see kThicknessConeHalfAngle for why narrow and not hemispherical.
	// Convexity samples the full SPHERE and does not use this cone at all
	// (see the eConvexity branch below).
	const Scalar maxTheta = ( kind == eThickness )
		? kThicknessConeHalfAngle
		: Scalar( PI_OV_TWO );
	const Scalar sinMax   = std::sin( maxTheta );
	const Scalar sin2Max  = sinMax * sinMax;

	// Convexity spends its ray budget as antipodal PAIRS, so it emits half
	// as many directions and traces both signs of each -- same kRayCount
	// rays cast, same cost class as the other two bakes.
	const int nDirections = ( kind == eConvexity ) ? ( kRayCount / 2 ) : kRayCount;

	out.assign( verts.size(), 0.0f );

	// One line per bake, at Info.  A bake is a one-time cost the user did
	// not ask for explicitly (they wrote `occlusion(0.05)` in a material,
	// not "bake now"), so it has to be visible when it happens and how long
	// it took -- the same courtesy the SSS point-set build extends.
	Timer timer;
	timer.start();

	for( size_t vi = 0; vi < verts.size(); ++vi ) {
		const Vector3& nRaw = normals[vi];
		const Scalar nLen2 = nRaw.x*nRaw.x + nRaw.y*nRaw.y + nRaw.z*nRaw.z;
		if( !( nLen2 > NEARZERO ) ) {
			// No usable orientation at this vertex (isolated / degenerate).
			// Write the signal's NEUTRAL value -- an unoriented vertex is an
			// ABSENCE of measurement, and the whole design answers absence
			// with the do-nothing end of the range, never with a plausible
			// invention.  Occlusion's and thickness's neutrals are both 1
			// (unoccluded / thick) but CONVEXITY's is 0 (flat) -- the
			// do-nothing end of an edge-wear mask is "no edge here", not
			// "knife edge everywhere"; see
			// SurfaceSignalInfo::NeutralOcclusion / ::NeutralThickness /
			// ::NeutralConvexity for each argument.
			out[vi] = ( kind == eConvexity ) ? 0.0f : 1.0f;
			continue;
		}
		const Scalar invLen = Scalar(1) / std::sqrt( nLen2 );
		const Vector3 n( nRaw.x*invLen, nRaw.y*invLen, nRaw.z*invLen );

		// Trace direction frame: outward for occlusion and convexity, inward
		// for thickness.  Substance's thickness baker casts INTO the solid
		// and measures how far it gets; that is the only way a surface signal
		// can say anything about what is behind it.
		const Vector3 w = ( kind == eThickness ) ? Vector3( -n.x, -n.y, -n.z ) : n;
		OrthonormalBasis3D onb;
		onb.CreateFromW( w );

		// The origin is lifted along the TRACE axis, not always outward, and
		// the difference is not cosmetic.  Every bake must clear the
		// triangles incident on this vertex, which all pass exactly through
		// it.  For occlusion and convexity that means stepping OUT.  For
		// thickness it must mean stepping IN: a thickness ray lifted OUTWARD
		// would re-enter through the very face it started on and report a
		// thickness of ~epsilon everywhere -- a mesh-wide flat zero that
		// would look like a plausible "thin" mask.  Starting just inside the
		// solid, the first thing an inward ray can hit is the far wall, which
		// is the quantity being measured.
		//
		// CONVEXITY'S KNOWN BIAS, stated where it is created.  Its rays also
		// start `originEpsilon` OUTWARD, so on a flat surface a ray aimed
		// just below the horizon escapes instead of hitting whenever
		// |cos theta| < originEpsilon/maxDistance.  A flat mesh therefore
		// reads A ~= 1/2 + originEpsilon/maxDistance, i.e. convexity ~= 0.004
		// at the default 1e-4 diagonal fraction and a 5 %-of-diagonal radius.
		// Bounded, one-sided (never negative, so a mask lights nothing), and
		// an order of magnitude under any threshold an author would set --
		// but it is a bias, not noise, and it does not average away.
		const Point3 origin(
			verts[vi].x + w.x * in.originEpsilon,
			verts[vi].y + w.y * in.originEpsilon,
			verts[vi].z + w.z * in.originEpsilon );

		const Scalar rot = GoldenRotation( vi );
		Scalar accum = Scalar(0);

		for( int s = 0; s < nDirections; ++s ) {
			const Scalar u1 = ( Scalar(s) + Scalar(0.5) ) / Scalar( nDirections );
			Scalar u2 = RadicalInverse2( static_cast<unsigned int>( s ) ) + rot;
			if( u2 >= Scalar(1) ) u2 -= Scalar(1);

			if( kind == eConvexity ) {
				// ACCESSIBILITY over the FULL sphere, as antipodal pairs:
				// the fraction of all directions that escape within the query
				// radius.  On a wedge of solid dihedral angle theta this is
				// exactly 1 - theta/2pi, which is the same `A` the SDF family
				// measures by ball volume -- so the two families agree on
				// every edge, crease and corner (they differ only at second
				// order on smoothly curved geometry, where one measures solid
				// angle and the other volume).
				const Vector3 d = UniformHemisphereDirection( onb, u1, u2 );
				const Vector3 dNeg( -d.x, -d.y, -d.z );
				if( !in.pOccluder->AnyHitWithin( origin, d,    in.maxDistance ) ) accum += Scalar(1);
				if( !in.pOccluder->AnyHitWithin( origin, dNeg, in.maxDistance ) ) accum += Scalar(1);
				continue;
			}

			const Vector3 dir = CosineConeDirection( onb, u1, u2, sin2Max );

			if( kind == eOcclusion ) {
				// Cosine-weighted directions make the plain hit FRACTION the
				// AO estimator -- the cosine term is already carried by the
				// sampling density, so no per-sample weight is needed.
				if( !in.pOccluder->AnyHitWithin( origin, dir, in.maxDistance ) ) {
					accum += Scalar(1);		// this direction is open sky
				}
			} else {
				// Distance to the near wall on the far side, saturating at
				// the query radius.  A MISS is a measurement too: "no far
				// side within R" means the solid is at least R thick, which
				// is the interface's saturated 1 -- exactly what the SDF
				// estimator reports in the same situation.
				Scalar d = in.maxDistance;
				Scalar hitDist = Scalar(0);
				if( in.pOccluder->NearestHitWithin( origin, dir, in.maxDistance, hitDist ) ) {
					d = ( hitDist < in.maxDistance ) ? hitDist : in.maxDistance;
					if( d < Scalar(0) ) d = Scalar(0);
				}
				accum += d / in.maxDistance;
			}
		}

		// Convexity traced two rays per direction, so its denominator is the
		// pair count doubled -- which is kRayCount again, but spelled from
		// what was actually accumulated rather than from a constant that
		// happens to match.
		const int nTraced = ( kind == eConvexity ) ? ( 2 * nDirections ) : nDirections;
		Scalar v = accum / Scalar( nTraced );
		if( !RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
			v = ( kind == eConvexity ) ? Scalar(0) : Scalar(1);
		} else if( kind == eConvexity ) {
			// `accum/nTraced` is the accessibility A; the stored signal is
			// its EXCESS above the planar half.  A plane gives A = 1/2 and
			// therefore 0, every cavity gives A < 1/2 and is clamped to 0
			// (occlusion owns that half of the range), and a knife edge
			// approaches 1.
			v = Scalar(2) * v - Scalar(1);
		}
		if( v < Scalar(0) ) v = Scalar(0);
		if( v > Scalar(1) ) v = Scalar(1);
		out[vi] = static_cast<float>( v );
	}

	timer.stop();
	// eLog_Event, not eLog_Info, and deliberately louder than the BVH /
	// area-CDF builds around it: those are part of loading ANY scene,
	// whereas this is a seconds-scale cost the user triggered IMPLICITLY by
	// writing `occlusion(0.05)` in a material.  A cost nobody asked for by
	// name should say so, and the number is what makes the trade
	// (bake once vs. read per sample) checkable rather than assumed.
	const char* kindName = ( kind == eOcclusion ) ? "OCCLUSION"
	                     : ( kind == eThickness ) ? "THICKNESS"
	                                              : "CONVEXITY";
	GlobalLog()->PrintEx( eLog_Event,
		"MeshSignalBake:: baked per-vertex %s over %u vertices x %d rays in %u ms",
		kindName, (unsigned)verts.size(), kRayCount, timer.getInterval() );

	return true;
}

//////////////////////////////////////////////////////////////////////
// MeshSignalBakeCache
//////////////////////////////////////////////////////////////////////

const Scalar MeshSignalBakeCache::kRadiusRelTolerance = Scalar( 1e-4 );

bool MeshSignalBakeCache::RadiiMatch( const Scalar a, const Scalar b )
{
	const Scalar scale = ( a > b ) ? a : b;
	return std::fabs( a - b ) <= kRadiusRelTolerance * scale;
}

MeshSignalBakeCache::MeshSignalBakeCache()
  : m_vertexNormalsBuilt( false )
{
	for( int k = 0; k < MeshSignalBake::eKindCount; ++k ) {
		m_warnedCap[k] = false;
	}
}

MeshSignalBakeCache::~MeshSignalBakeCache()
{
	Invalidate();
}

bool MeshSignalBakeCache::Invalidate()
{
	// ONE acquisition covers the tables AND the per-position normals they
	// were derived from.  Dropping them in two lock regions -- or, as an
	// earlier shape had it, dropping the tables under the cache's lock and
	// then clearing a geometry-owned normal array with no lock at all --
	// leaves a window in which a concurrent find-or-build reads the array
	// mid-clear.
	std::lock_guard<RMutex> guard( m_mutex );

	bool bDroppedSomething = m_vertexNormalsBuilt;
	for( int k = 0; k < MeshSignalBake::eKindCount; ++k ) {
		if( !m_entries[k].empty() ) {
			bDroppedSomething = true;
		}
		// Clearing drops only THIS cache's references.  A table a reader is
		// still interpolating stays alive until that reader is done with it
		// (see TableRef): the reader finishes on stale data rather than on
		// freed data.
		m_entries[k].clear();
		m_warnedCap[k] = false;
	}
	m_vertexNormals.clear();
	m_vertexNormalsBuilt = false;
	return bDroppedSomething;
}

MeshSignalBakeCache::TableRef MeshSignalBakeCache::FindOrBuild(
	const MeshSignalBake::Kind kind,
	const Scalar radiusFraction,
	const MeshSignalBake::IInputSource& src ) const
{
	// Range-checked against the enumeration rather than listed kind by kind:
	// `m_entries` / `m_warnedCap` are sized by eKindCount, so THAT is the
	// real precondition, and spelling it as a whitelist of the kinds that
	// existed when this was written is how eConvexity silently read its
	// neutral fallback for a whole review round instead of baking.
	if( kind < 0 || kind >= MeshSignalBake::eKindCount ) {
		return TableRef();
	}
	if( !RISE::IsFiniteDouble( static_cast<double>( radiusFraction ) ) || !( radiusFraction > Scalar(0) ) ) {
		return TableRef();
	}

	// ONE guard across the whole find-or-build, unlocking on every exit
	// including a bad_alloc out of the build -- the SSS discipline, and for
	// the same reason: an unlocked pre-check would race with the insert
	// below on a std::vector that can reallocate under it.
	std::lock_guard<RMutex> guard( m_mutex );

	std::vector<Entry>& entries = m_entries[kind];
	for( size_t i = 0; i < entries.size(); ++i ) {
		if( RadiiMatch( entries[i].radius, radiusFraction ) ) {
			// ONE refcount pair per query: this copy, released when the
			// caller's local dies.  The interpolation that follows reads
			// through it without touching the count again.
			return entries[i].table;		// may be empty: the cached failure
		}
	}

	if( entries.size() >= kMaxTablesPerKind ) {
		if( !m_warnedCap[kind] ) {
			m_warnedCap[kind] = true;
			GlobalLog()->PrintEasyWarning(
				"MeshSignalBakeCache:: more than 8 distinct occlusion()/thickness()/convexity() radii on one mesh; "
				"further radii read the neutral fallback -- see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 7.1" );
		}
		return TableRef();
	}

	// Input assembly happens HERE, under the lock, and only on a genuine
	// miss -- see IInputSource for why it is a callback.  The per-position
	// normals it needs are cache-owned and built at most once per
	// generation, so the SECOND radius on this mesh reuses them.
	if( !m_vertexNormalsBuilt ) {
		m_vertexNormalsBuilt = true;
		src.BuildVertexNormals( m_vertexNormals );
	}

	MeshSignalBake::Input in;
	in.pVertices = 0; in.pNormals = 0; in.maxDistance = Scalar(0);
	in.originEpsilon = Scalar(0); in.pOccluder = 0;
	const bool bHaveInput = src.MakeBakeInput( radiusFraction, m_vertexNormals, in );

	std::shared_ptr<Table> pTable( new Table() );
	if( !bHaveInput || !MeshSignalBake::Build( kind, in, *pTable ) ) {
		// NULL SENTINEL, the SSS pattern: cache the failure so the next
		// several million samples do not each re-attempt a build that
		// cannot succeed, and warn exactly once for this (geometry, kind,
		// radius).
		Entry e;
		e.radius = radiusFraction;
		e.table = TableRef();
		entries.push_back( e );
		GlobalLog()->PrintEasyWarning(
			"MeshSignalBakeCache:: per-vertex signal bake failed on a mesh (no vertices, or no usable "
			"per-vertex normals); occlusion()/thickness() read their neutral fallback there" );
		return TableRef();
	}

	Entry e;
	e.radius = radiusFraction;
	e.table = TableRef( pTable );		// const-qualified from here on
	entries.push_back( e );
	return e.table;
}
