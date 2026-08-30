//////////////////////////////////////////////////////////////////////
//
//  MeshSignalBake.cpp - Implementation of the per-vertex occlusion /
//  thickness bakes and their lazy find-or-build cache.
//
//  docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §7 (Phase 3).
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
	const Scalar maxTheta = ( kind == eOcclusion )
		? Scalar( PI_OV_TWO )
		: kThicknessConeHalfAngle;
	const Scalar sinMax   = std::sin( maxTheta );
	const Scalar sin2Max  = sinMax * sinMax;

	out.assign( verts.size(), 0.0f );

	for( size_t vi = 0; vi < verts.size(); ++vi ) {
		const Vector3& nRaw = normals[vi];
		const Scalar nLen2 = nRaw.x*nRaw.x + nRaw.y*nRaw.y + nRaw.z*nRaw.z;
		if( !( nLen2 > NEARZERO ) ) {
			// No usable orientation at this vertex (isolated / degenerate).
			// Write the signal's NEUTRAL value -- an unoriented vertex is an
			// ABSENCE of measurement, and the whole design answers absence
			// with the do-nothing end of the range, never with a plausible
			// invention.  Both neutrals are 1 (unoccluded / thick), so one
			// literal serves both kinds here; see
			// SurfaceSignalInfo::NeutralOcclusion / ::NeutralThickness for
			// why they agree.
			out[vi] = 1.0f;
			continue;
		}
		const Scalar invLen = Scalar(1) / std::sqrt( nLen2 );
		const Vector3 n( nRaw.x*invLen, nRaw.y*invLen, nRaw.z*invLen );

		// Trace direction frame: outward for occlusion, inward for
		// thickness.  Substance's thickness baker casts INTO the solid and
		// measures how far it gets; that is the only way a surface signal
		// can say anything about what is behind it.
		const Vector3 w = ( kind == eOcclusion ) ? n : Vector3( -n.x, -n.y, -n.z );
		OrthonormalBasis3D onb;
		onb.CreateFromW( w );

		// The origin is lifted along the TRACE axis, not always outward, and
		// the difference is not cosmetic.  Both bakes must clear the
		// triangles incident on this vertex, which all pass exactly through
		// it.  For occlusion that means stepping OUT.  For thickness it must
		// mean stepping IN: a thickness ray lifted OUTWARD would re-enter
		// through the very face it started on and report a thickness of
		// ~epsilon everywhere -- a mesh-wide flat zero that would look like
		// a plausible "thin" mask.  Starting just inside the solid, the
		// first thing an inward ray can hit is the far wall, which is the
		// quantity being measured.
		const Point3 origin(
			verts[vi].x + w.x * in.originEpsilon,
			verts[vi].y + w.y * in.originEpsilon,
			verts[vi].z + w.z * in.originEpsilon );

		const Scalar rot = GoldenRotation( vi );
		Scalar accum = Scalar(0);

		for( int s = 0; s < kRayCount; ++s ) {
			const Scalar u1 = ( Scalar(s) + Scalar(0.5) ) / Scalar( kRayCount );
			Scalar u2 = RadicalInverse2( static_cast<unsigned int>( s ) ) + rot;
			if( u2 >= Scalar(1) ) u2 -= Scalar(1);

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

		Scalar v = accum / Scalar( kRayCount );
		if( !RISE::IsFiniteDouble( static_cast<double>( v ) ) ) {
			v = Scalar(1);
		}
		if( v < Scalar(0) ) v = Scalar(0);
		if( v > Scalar(1) ) v = Scalar(1);
		out[vi] = static_cast<float>( v );
	}

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
{
	for( int k = 0; k < MeshSignalBake::eKindCount; ++k ) {
		m_warnedCap[k] = false;
	}
}

MeshSignalBakeCache::~MeshSignalBakeCache()
{
	Invalidate();
}

void MeshSignalBakeCache::Invalidate()
{
	std::lock_guard<RMutex> guard( m_mutex );
	for( int k = 0; k < MeshSignalBake::eKindCount; ++k ) {
		for( size_t i = 0; i < m_entries[k].size(); ++i ) {
			delete m_entries[k][i].pTable;
		}
		m_entries[k].clear();
		m_warnedCap[k] = false;
	}
}

const MeshSignalBakeCache::Table* MeshSignalBakeCache::FindOrBuild(
	const MeshSignalBake::Kind kind,
	const Scalar radiusFraction,
	const MeshSignalBake::IInputSource& src ) const
{
	if( kind != MeshSignalBake::eOcclusion && kind != MeshSignalBake::eThickness ) {
		return 0;
	}
	if( !RISE::IsFiniteDouble( static_cast<double>( radiusFraction ) ) || !( radiusFraction > Scalar(0) ) ) {
		return 0;
	}

	// ONE guard across the whole find-or-build, unlocking on every exit
	// including a bad_alloc out of the build -- the SSS discipline, and for
	// the same reason: an unlocked pre-check would race with the insert
	// below on a std::vector that can reallocate under it.
	std::lock_guard<RMutex> guard( m_mutex );

	std::vector<Entry>& entries = m_entries[kind];
	for( size_t i = 0; i < entries.size(); ++i ) {
		if( RadiiMatch( entries[i].radius, radiusFraction ) ) {
			return entries[i].pTable;		// may be 0: the cached failure
		}
	}

	if( entries.size() >= kMaxTablesPerKind ) {
		if( !m_warnedCap[kind] ) {
			m_warnedCap[kind] = true;
			GlobalLog()->PrintEasyWarning(
				"MeshSignalBakeCache:: more than 8 distinct occlusion()/thickness() radii on one mesh; "
				"further radii read the neutral fallback -- see docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 7.1" );
		}
		return 0;
	}

	// Input assembly happens HERE, under the lock, and only on a genuine
	// miss -- see IInputSource for why it is a callback.
	MeshSignalBake::Input in;
	in.pVertices = 0; in.pNormals = 0; in.maxDistance = Scalar(0);
	in.originEpsilon = Scalar(0); in.pOccluder = 0;
	const bool bHaveInput = src.MakeBakeInput( radiusFraction, in );

	Table* pTable = new Table();
	if( !bHaveInput || !MeshSignalBake::Build( kind, in, *pTable ) ) {
		// NULL SENTINEL, the SSS pattern: cache the failure so the next
		// several million samples do not each re-attempt a build that
		// cannot succeed, and warn exactly once for this (geometry, kind,
		// radius).
		delete pTable;
		Entry e;
		e.radius = radiusFraction;
		e.pTable = 0;
		entries.push_back( e );
		GlobalLog()->PrintEasyWarning(
			"MeshSignalBakeCache:: per-vertex signal bake failed on a mesh (no vertices, or no usable "
			"per-vertex normals); occlusion()/thickness() read their neutral fallback there" );
		return 0;
	}

	Entry e;
	e.radius = radiusFraction;
	e.pTable = pTable;
	entries.push_back( e );
	return pTable;
}
