//////////////////////////////////////////////////////////////////////
//
//  SimulatedMeshGeometryTest.cpp - Simulation M3b: simulated_mesh_geometry
//    (Geometry/SimulatedMeshGeometry.h, Simulation/SimulatedMeshSource.h).
//
//    Every check writes its own small mesh.v1 cache (no solver):
//      A  bind frame and an installed frame intersect exactly like a static
//         indexed mesh built from the same frame (hits and distances bit-equal)
//      B  shutter keys: positions = frame + velocity x dt; at the open and
//         close sample times the blurred mesh hits like a static mesh at that
//         key; the sample time comes from the thread's time-indexed pose or the
//         shared animator
//      C  velocities absent: keys from the adjacent frames
//      D  time map: offset / scale, hold, loop, error
//      E  missing frame of a partial cache: preview hold / hide (with a
//         warning), predictive fails closed; predictive admission
//      F  corrupt payload, decode budget, changed topology: the frame fails and
//         the previously installed frame stays
//      G  render freeze and rollback
//      H  the chunk through a Job: load, predictive refusal, and a prepared
//         animation render installing the frame for its time
//
//    Usage: SimulatedMeshGeometryTest [--bench]
//      --bench (not a gate) times UpdateVertices against the lean install at
//      2.6k / 10k / 100k vertices and the full per-frame install.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Geometry/SimulatedMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Simulation/SimulatedMeshSource.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Animation/Animator.h"
#include "../src/Library/Animation/TimeIndexedView.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Simulation;
namespace fs = std::filesystem;

namespace RISE { bool RISE_CreateJobPriv( IJobPriv** ppi ); }

namespace
{
	int failures = 0, passes = 0;
	void Check( const bool ok, const std::string& what, const std::string& detail = std::string() )
	{
		if( ok ) { ++passes; return; }
		++failures;
		std::printf( "  FAIL: %s%s%s\n", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str() );
	}

	constexpr std::size_t kBudget = 64u * 1024 * 1024;

	std::uint32_t Bits( const double v ) { const float f = float( v ); std::uint32_t b; std::memcpy( &b, &f, 4 ); return b; }
	double Value( const std::uint32_t b ) { float f; std::memcpy( &f, &b, 4 ); return double( f ); }

	//! A sheet of nx x ny vertices.  Frame k sits at simulation time start + k*dt.
	struct Fixture
	{
		int nx = 12, ny = 9;
		double start = 0.0, dt = 0.25;
		int count = 6;
		bool velocities = true, normals = true;
		bool linear = false;	//!< positions exactly linear in time (no wobble)
		std::vector<std::uint32_t> Indices() const
		{
			std::vector<std::uint32_t> idx;
			for( int j = 0; j + 1 < ny; ++j ) for( int i = 0; i + 1 < nx; ++i ) {
				const std::uint32_t a = j * nx + i, b = a + 1, c = a + nx, d = c + 1;
				idx.insert( idx.end(), { a, b, c, b, d, c } );
			}
			return idx;
		}
		void Rest( const int i, const int j, double& x, double& y ) const
		{
			x = -1.0 + 2.0 * i / ( nx - 1 );
			y = -0.75 + 1.5 * j / ( ny - 1 );
		}
		void Velocity( const int i, const int j, double v[3] ) const
		{
			double x, y; Rest( i, j, x, y );
			v[0] = 0.4 + 0.2 * y; v[1] = -0.3 * x; v[2] = 0.5 * std::sin( x );
		}
		MeshFrame Frame( const int k ) const
		{
			MeshFrame f;
			const double s = start + k * dt;
			for( int j = 0; j < ny; ++j ) for( int i = 0; i < nx; ++i ) {
				double x, y, v[3]; Rest( i, j, x, y ); Velocity( i, j, v );
				const double wobble = linear ? 0.0 : 0.05 * std::sin( k + 3.0 * x );
				f.positions.push_back( Bits( x + v[0] * s ) );
				f.positions.push_back( Bits( y + v[1] * s ) );
				f.positions.push_back( Bits( 0.2 + v[2] * s + wobble ) );
				if( velocities ) for( int c = 0; c < 3; ++c ) f.velocities.push_back( Bits( v[c] ) );
			}
			f.indices = Indices();
			if( velocities ) f.velocityUnits = "m/s";
			if( normals ) {
				// Area-weighted vertex normals of the float positions.
				std::vector<double> n( f.positions.size(), 0.0 );
				for( std::size_t t = 0; t < f.indices.size(); t += 3 ) {
					const std::uint32_t a = f.indices[t], b = f.indices[t+1], c = f.indices[t+2];
					double e1[3], e2[3];
					for( int q = 0; q < 3; ++q ) {
						e1[q] = Value( f.positions[3*b+q] ) - Value( f.positions[3*a+q] );
						e2[q] = Value( f.positions[3*c+q] ) - Value( f.positions[3*a+q] );
					}
					const double cr[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
					for( const std::uint32_t vtx : { a, b, c } ) for( int q = 0; q < 3; ++q ) n[3*vtx+q] += cr[q];
				}
				for( std::size_t vtx = 0; vtx < n.size() / 3; ++vtx ) {
					const double m = std::sqrt( n[3*vtx]*n[3*vtx] + n[3*vtx+1]*n[3*vtx+1] + n[3*vtx+2]*n[3*vtx+2] );
					for( int q = 0; q < 3; ++q ) f.normals.push_back( Bits( n[3*vtx+q] / m ) );
				}
			}
			return f;
		}
	};

	std::string Hex64( const unsigned salt ) { char b[65]; for( int i = 0; i < 64; ++i ) b[i] = "0123456789abcdef"[( salt * 7 + i * 13 ) & 15]; b[64] = 0; return b; }

	CacheDescription Describe( const Fixture& fx, const unsigned salt, const std::string& fidelity = "preview", const bool hold = true )
	{
		CacheDescription d;
		d.key = Hex64( salt ); d.epoch = Hex64( salt + 1000 );
		d.producer = "m3b-fixture"; d.producerVersion = "1";
		d.firstIndex = 0; d.count = fx.count; d.startTime = fx.start; d.interval = fx.dt; d.hold = hold;
		d.outputs = { { "surface", "mesh.v1", { { "position", "m", "f32x3" }, { "velocity", "m/s", "f32x3" } } } };
		d.fidelity = fidelity;
		if( fidelity == "predictive" ) d.evidence = { Hex64( salt + 77 ) };
		return d;
	}

	//! Writes frames [0, written) of the fixture; seals when all are written.
	bool WriteCache( const fs::path& root, const Fixture& fx, const unsigned salt, const int written,
		const std::string& fidelity = "preview", const bool hold = true,
		const std::vector<std::pair<int, MeshFrame>>& overrides = {} )
	{
		std::error_code ec;
		fs::remove_all( root, ec );
		fs::create_directories( root.parent_path(), ec );
		CacheWriter writer;
		std::string error;
		if( !writer.Begin( root, Describe( fx, salt, fidelity, hold ), "", false, kBudget, error ) ) {
			std::printf( "  cache Begin: %s\n", error.c_str() );
			return false;
		}
		for( int k = 0; k < written; ++k ) {
			MeshFrame frame = fx.Frame( k );
			for( const auto& o : overrides ) if( o.first == k ) frame = o.second;
			Bytes bytes;
			if( !EncodeMesh( frame, kBudget, bytes, error ) ||
				!writer.Append( k, "surface", fx.start + k * fx.dt, bytes, error ) ) {
				std::printf( "  cache Append %d: %s\n", k, error.c_str() );
				return false;
			}
		}
		if( written == fx.count && !writer.Seal( error ) ) { std::printf( "  cache Seal: %s\n", error.c_str() ); return false; }
		return true;
	}

	TriangleMeshGeometryIndexed* Static( const std::vector<double>& p, const std::vector<double>& n,
		const std::vector<std::uint32_t>& idx, const bool doubleSided, const bool faceNormals )
	{
		TriangleMeshGeometryIndexed* g = new TriangleMeshGeometryIndexed( doubleSided, faceNormals );
		g->BeginIndexedTriangles();
		for( std::size_t v = 0; v < p.size() / 3; ++v ) g->AddVertex( Vertex( p[3*v], p[3*v+1], p[3*v+2] ) );
		if( !faceNormals ) for( std::size_t v = 0; v < n.size() / 3; ++v ) g->AddNormal( Normal( n[3*v], n[3*v+1], n[3*v+2] ) );
		for( std::size_t t = 0; t < idx.size(); t += 3 ) {
			IndexedTriangle tri;
			for( int k = 0; k < 3; ++k ) { tri.iVertices[k] = idx[t+k]; tri.iNormals[k] = idx[t+k]; tri.iCoords[k] = 0; }
			g->AddIndexedTriangle( tri );
		}
		g->DoneIndexedTriangles();
		return g;
	}

	std::vector<double> Doubles( const std::vector<std::uint32_t>& b ) { std::vector<double> d; for( auto x : b ) d.push_back( Value( x ) ); return d; }

	std::vector<Ray> Rays()
	{
		std::vector<Ray> rays;
		for( int j = 0; j < 48; ++j ) for( int i = 0; i < 48; ++i ) {
			const Point3 o( -1.6 + 3.2 * ( i + 0.37 ) / 48, -1.3 + 2.6 * ( j + 0.61 ) / 48, 3.0 );
			const Vector3 d( 0.03 * std::sin( 0.7 * i ), 0.02 * std::cos( 0.3 * j ), -1.0 );
			rays.push_back( Ray( o, Vector3Ops::Normalize( d ) ) );
		}
		return rays;
	}

	struct Compare { int rays = 0, hitsA = 0, hitMismatch = 0, rangeNotEqual = 0, normalNotEqual = 0, shadowMismatch = 0; double maxRange = 0, maxNormal = 0; };

	Compare CompareHits( const IGeometry& a, const IGeometry& b )
	{
		Compare c;
		for( const Ray& ray : Rays() ) {
			RayIntersectionGeometric ra( ray, nullRasterizerState ), rb( ray, nullRasterizerState );
			a.IntersectRay( ra, true, true, false );
			b.IntersectRay( rb, true, true, false );
			++c.rays;
			if( ra.bHit ) ++c.hitsA;
			if( ra.bHit != rb.bHit ) { ++c.hitMismatch; continue; }
			if( a.IntersectRay_IntersectionOnly( ray, 100.0, true, true ) != b.IntersectRay_IntersectionOnly( ray, 100.0, true, true ) ) ++c.shadowMismatch;
			if( !ra.bHit ) continue;
			if( std::memcmp( &ra.range, &rb.range, sizeof( Scalar ) ) != 0 ) ++c.rangeNotEqual;
			c.maxRange = std::max( c.maxRange, std::fabs( ra.range - rb.range ) );
			const Vector3 na = Vector3Ops::Normalize( ra.vNormal ), nb = Vector3Ops::Normalize( rb.vNormal );
			const double dn = Vector3Ops::Magnitude( na - nb );
			if( dn != 0.0 ) ++c.normalNotEqual;
			c.maxNormal = std::max( c.maxNormal, dn );
		}
		return c;
	}

	std::string Describe( const Compare& c )
	{
		char b[256];
		std::snprintf( b, sizeof( b ), "%d rays, %d hits, %d hit mismatches, %d any-hit mismatches, %d ranges not bit-equal (max %.3g), %d normals differ (max %.3g)",
			c.rays, c.hitsA, c.hitMismatch, c.shadowMismatch, c.rangeNotEqual, c.maxRange, c.normalNotEqual, c.maxNormal );
		return b;
	}

	SimulatedMeshGeometry* Bind( const fs::path& root, SimulatedMeshGeometry::Options o, std::string& error )
	{
		o.doubleSided = true;
		return SimulatedMeshGeometry::Create( root.string(), o, error );
	}

	struct SampleTimeScope
	{
		TimeIndexed::SampleTime previous;
		explicit SampleTimeScope( const double t ) : previous( TimeIndexed::tlsSampleTime ) { TimeIndexed::tlsSampleTime = { t, true }; }
		~SampleTimeScope() { TimeIndexed::tlsSampleTime = previous; }
	};

	//////////////////////////////////////////////////////////////////
	void TestStaticEquality( const fs::path& scratch )
	{
		std::printf( "A: non-blurred simulated mesh == static indexed mesh\n" );
		Fixture fx;
		const fs::path root = scratch / "a";
		Check( WriteCache( root, fx, 1, fx.count ), "A write cache" );
		std::string error;
		SimulatedMeshGeometry::Options o;
		SimulatedMeshGeometry* g = Bind( root, o, error );
		Check( g != nullptr, "A bind", error );
		if( !g ) return;
		const MeshFrame f0 = fx.Frame( 0 );
		TriangleMeshGeometryIndexed* s0 = Static( Doubles( f0.positions ), Doubles( f0.normals ), f0.indices, true, false );
		Compare c = CompareHits( *g, *s0 );
		std::printf( "  bind frame: %s\n", Describe( c ).c_str() );
		Check( c.hitsA > 500 && c.hitMismatch == 0 && c.shadowMismatch == 0 && c.rangeNotEqual == 0 && c.normalNotEqual == 0,
			"A bind frame bit-equal to a static mesh", Describe( c ) );
		s0->release();

		// Install frame 3 at its own time (no exposure): a refit, not a rebuild.
		const double t3 = fx.start + 3 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t3, t3, t3 }, error, nullptr ), "A install frame 3", error );
		Check( g->InstalledFrame() == 3 && !g->BlurActive() && !g->Hidden(), "A frame 3 installed without blur" );
		const MeshFrame f3 = fx.Frame( 3 );
		TriangleMeshGeometryIndexed* s3 = Static( Doubles( f3.positions ), Doubles( f3.normals ), f3.indices, true, false );
		c = CompareHits( *g, *s3 );
		std::printf( "  frame 3 (refit BVH vs fresh build): %s\n", Describe( c ).c_str() );
		Check( c.hitsA > 500 && c.hitMismatch == 0 && c.shadowMismatch == 0 && c.rangeNotEqual == 0,
			"A installed frame: same hits and bit-equal distances as a static mesh", Describe( c ) );
		// Normals: equal except where a ray meets an edge two triangles share at
		// the same distance -- then the traversal order (refit tree vs fresh
		// tree) picks the triangle, and the barycentric normal of each differs
		// in the last bits.
		Check( c.maxNormal < 1e-12, "A installed frame: normals agree (ties aside, bit-equal)", Describe( c ) );
		const Scalar areaG = g->GetArea(), areaS = s3->GetArea();
		Check( areaG == areaS, "A lean install recomputes the same area as a full build" );
		s3->release();

		// Same time again: a no-op (no second install).
		const unsigned long long installs = g->InstallCount();
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t3, t3, t3 }, error, nullptr ) && g->InstallCount() == installs,
			"A re-preparing the installed frame is a no-op" );

		// motion_blur off between frames: the nearest frame, no extrapolation.
		SimulatedMeshGeometry::Options off; off.motionBlur = false;
		SimulatedMeshGeometry* h = Bind( root, off, error );
		const double between = t3 + 0.3 * fx.dt;
		Check( h && h->PrepareAssetForRender( RenderTimeSupport{ between, between - 0.1, between + 0.1 }, error, nullptr ) &&
			h->InstalledFrame() == 3 && !h->BlurActive(), "A motion_blur off: nearest frame, no keys", error );
		if( h ) {
			TriangleMeshGeometryIndexed* s3b = Static( Doubles( f3.positions ), Doubles( f3.normals ), f3.indices, true, false );
			c = CompareHits( *h, *s3b );
			Check( c.hitMismatch == 0 && c.rangeNotEqual == 0, "A motion_blur off renders the frame as a static mesh", Describe( c ) );
			s3b->release();
			h->release();
		}
		g->release();
	}

	//////////////////////////////////////////////////////////////////
	void TestKeys( const fs::path& scratch, const bool withVelocities )
	{
		std::printf( "%s: shutter keys %s\n", withVelocities ? "B" : "C", withVelocities ? "from the frame velocities" : "from the adjacent frames" );
		Fixture fx;
		fx.velocities = withVelocities;
		fx.linear = !withVelocities;	// adjacent frames reproduce the velocity only for linear motion
		const fs::path root = scratch / ( withVelocities ? "b" : "c" );
		Check( WriteCache( root, fx, withVelocities ? 2 : 3, fx.count ), "B/C write cache" );
		std::string error;
		SimulatedMeshGeometry* g = Bind( root, SimulatedMeshGeometry::Options(), error );
		Check( g != nullptr, "B/C bind", error );
		if( !g ) return;
		// Dyadic times (frame 2 at 0.5, shutter 0.375..0.625) keep the key
		// offsets exact, so the reference poses below are the source's own.
		const double t2 = fx.start + 2 * fx.dt, E = 0.25;
		const RenderTimeSupport support{ t2, t2 - 0.5 * E, t2 + 0.5 * E };
		Check( g->PrepareAssetForRender( support, error, nullptr ), "B/C install with shutter", error );
		Check( g->BlurActive() && g->InstalledFrame() == 2, "B/C shutter keys installed" );

		const MeshFrame f2 = fx.Frame( 2 );
		const std::vector<double> p = Doubles( f2.positions );
		std::vector<double> v( p.size() );
		for( int j = 0, q = 0; j < fx.ny; ++j ) for( int i = 0; i < fx.nx; ++i, ++q ) {
			double vel[3]; fx.Velocity( i, j, vel );
			for( int c = 0; c < 3; ++c ) v[3*q+c] = withVelocities ? Value( Bits( vel[c] ) ) : vel[c];
		}
		const std::vector<std::uint32_t> idx = f2.indices;
		const double tol = withVelocities ? 1e-12 : 1e-5;
		for( const double fraction : { 0.0, 0.5, 1.0 } ) {
			std::vector<double> key( p.size() );
			const double offset = ( fraction - 0.5 ) * E;
			for( std::size_t i = 0; i < p.size(); ++i ) key[i] = p[i] + v[i] * offset;
			// Face normals for the reference: the key normals are interpolated, so
			// compare positions (hits / distances) only.
			TriangleMeshGeometryIndexed* s = Static( key, {}, idx, true, true );
			SampleTimeScope scope( support.open + fraction * ( support.close - support.open ) );
			Check( std::fabs( g->ShutterFraction() - fraction ) < 1e-12, "B/C the thread's time-indexed sample time drives the fraction" );
			const Compare c = CompareHits( *g, *s );
			std::printf( "  fraction %.1f vs static mesh at the key: %s\n", fraction, Describe( c ).c_str() );
			Check( c.hitsA > 500 && c.hitMismatch <= 2 && c.maxRange < tol,
				"B/C blurred mesh at the sample time hits like a static mesh at that pose", Describe( c ) );
			// Informational: at a shutter end the interpolation weights are exactly
			// 0 and 1; distances are bit-equal unless the library (fast-math) and
			// this test round P + V*dt differently.
			if( withVelocities && fraction != 0.5 )
				std::printf( "    shutter end: %d of %d distances bit-equal to the reference\n", c.hitsA - c.rangeNotEqual, c.hitsA );
			s->release();
		}
		// Outside a time-indexed pass: the shared pose time (the animator's).
		Animator* animator = new Animator();
		g->SetSampleClock( animator );
		animator->EvaluateAtTime( support.open + 0.25 * E );
		Check( std::fabs( g->ShutterFraction() - 0.25 ) < 1e-12, "B/C the shared scene's pose time drives the fraction" );
		{
			SampleTimeScope scope( support.close );
			Check( g->ShutterFraction() == 1.0, "B/C a time-indexed pose overrides the shared time on its thread" );
		}
		animator->EvaluateAtTime( support.close + 5.0 );
		Check( g->ShutterFraction() == 1.0, "B/C times past the shutter clamp" );
		g->SetSampleClock( nullptr );
		Check( g->ShutterFraction() == 0.5, "B/C no clock: the nominal time" );
		animator->release();

		// Zero exposure: the keys collapse and the mesh is a static mesh at the
		// nominal time -- here between frames, extrapolated by the velocity.
		const double tm = t2 + 0.1 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ tm, tm, tm }, error, nullptr ) && !g->BlurActive() && g->InstalledFrame() == 2,
			"B/C zero exposure installs no keys", error );
		std::vector<double> at( p.size() );
		for( std::size_t i = 0; i < p.size(); ++i ) at[i] = p[i] + v[i] * ( tm - t2 );
		TriangleMeshGeometryIndexed* s = Static( at, {}, idx, true, true );
		const Compare c = CompareHits( *g, *s );
		Check( c.hitMismatch <= 2 && c.maxRange < tol, "B/C zero exposure between frames = frame + velocity x dt", Describe( c ) );
		s->release();
		g->release();
	}

	//////////////////////////////////////////////////////////////////
	void TestTimeMap( const fs::path& scratch )
	{
		std::printf( "D: time map\n" );
		Fixture fx;	// frames at 0, 0.25, ..., 1.25; support [0, 1.5)
		const fs::path root = scratch / "d";
		Check( WriteCache( root, fx, 4, fx.count, "preview", true ), "D write cache" );
		std::string error;
		auto map = [&]( MeshEndPolicy end, double offset, double scale, double t, double delta, std::int64_t& k, double& d, bool& held ) {
			MeshSourceOptions o; o.end = end; o.timeOffset = offset; o.timeScale = scale;
			std::unique_ptr<MeshCacheSource> s;
			if( !MeshCacheSource::Open( root, o, s, error ) ) return false;
			return s->MapTime( t, delta, k, d, held, error );
		};
		std::int64_t k = -1; double d = 0; bool held = false;
		Check( map( MeshEndPolicy::Cache, 0, 1, 0.5, 0, k, d, held ) && k == 2 && d == 0.0 && !held, "D frame time maps to its frame" );
		Check( map( MeshEndPolicy::Cache, 0, 1, 0.6, 0, k, d, held ) && k == 2 && std::fabs( d - 0.1 ) < 1e-15, "D nearest frame below the midpoint" );
		Check( map( MeshEndPolicy::Cache, 0, 1, 0.65, 0, k, d, held ) && k == 3 && std::fabs( d + 0.1 ) < 1e-15, "D nearest frame above the midpoint" );
		Check( map( MeshEndPolicy::Cache, 0, 1, 0.5, -0.2, k, d, held ) && k == 2 && std::fabs( d + 0.2 ) < 1e-15, "D a shutter key keeps the nominal's frame" );
		Check( map( MeshEndPolicy::Cache, 1.0, 2.0, 1.25, 0, k, d, held ) && k == 2, "D offset and scale: (t - offset) x scale" );
		Check( map( MeshEndPolicy::Cache, 0, 1, 7.0, 0, k, d, held ) && k == 5 && held && d == 0.0, "D the cache's hold clamps past the end" );
		Check( map( MeshEndPolicy::Hold, 0, 1, 7.0, -0.1, k, d, held ) && k == 5 && d == 0.0, "D a held frame does not move over the shutter" );
		Check( !map( MeshEndPolicy::Error, 0, 1, 1.6, 0, k, d, held ), "D error policy refuses a time outside the support" );
		Check( map( MeshEndPolicy::Error, 0, 1, 0.01, -0.05, k, d, held ) && k == 0 && d == 0.0,
			"D error policy clamps a shutter key leaving the support (the first frame renders)" );
		Check( map( MeshEndPolicy::Loop, 0, 1, 1.5 + 0.5, 0, k, d, held ) && k == 2 && std::fabs( d ) < 1e-12, "D loop repeats with period count x interval" );
		Check( map( MeshEndPolicy::Loop, 0, 1, 1.5 - 0.05, 0.1, k, d, held ) && k == 0 && std::fabs( d - 0.05 ) < 1e-12, "D loop wraps to frame 0 at the period" );
		std::unique_ptr<MeshCacheSource> s;
		MeshSourceOptions zero; zero.timeScale = 0;
		Check( !MeshCacheSource::Open( root, zero, s, error ), "D a zero time scale is refused" );
		MeshSourceOptions noOutput; noOutput.output = "nope";
		Check( !MeshCacheSource::Open( root, noOutput, s, error ), "D an absent output is refused", error );
	}

	//////////////////////////////////////////////////////////////////
	void TestMissingAndPredictive( const fs::path& scratch )
	{
		std::printf( "E: missing frames, predictive\n" );
		Fixture fx;
		const fs::path partial = scratch / "e_partial";
		Check( WriteCache( partial, fx, 5, 3 ), "E write partial cache (3 of 6 frames)" );
		std::string error;
		SimulatedMeshGeometry::Options hold;
		SimulatedMeshGeometry* g = Bind( partial, hold, error );
		Check( g != nullptr, "E preview binds a partial cache", error );
		if( g ) {
			const double t1 = fx.start + fx.dt, t4 = fx.start + 4 * fx.dt;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1, t1 }, error, nullptr ) && g->InstalledFrame() == 1, "E frame 1 installs", error );
			const MeshFrame f1 = fx.Frame( 1 );
			TriangleMeshGeometryIndexed* s1 = Static( Doubles( f1.positions ), Doubles( f1.normals ), f1.indices, true, false );
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t4, t4, t4 }, error, nullptr ), "E missing frame, preview hold: prepare succeeds", error );
			Compare c = CompareHits( *g, *s1 );
			Check( g->InstalledFrame() == 1 && !g->Hidden() && c.hitMismatch == 0 && c.rangeNotEqual == 0,
				"E hold keeps the previously installed frame", Describe( c ) );
			// Holding a frame that had shutter keys drops them: they belong to
			// the earlier frame's shutter, and the held pose is its nominal one.
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1 - 0.05, t1 + 0.05 }, error, nullptr ) && g->BlurActive(), "E frame 1 with keys" );
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t4, t4 - 0.05, t4 + 0.05 }, error, nullptr ) && !g->BlurActive() && g->InstalledFrame() == 1,
				"E a held frame installs no shutter keys" );
			c = CompareHits( *g, *s1 );
			Check( c.hitMismatch == 0 && c.rangeNotEqual == 0, "E the held pose is the frame's nominal pose", Describe( c ) );
			s1->release();
			g->release();
		}
		SimulatedMeshGeometry::Options hide; hide.source.missing = MeshMissingPolicy::Hide;
		g = Bind( partial, hide, error );
		if( g ) {
			const double t2 = fx.start + 2 * fx.dt, t5 = fx.start + 5 * fx.dt;
			std::function<void()> undo;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t5, t5, t5 }, error, &undo ) && g->Hidden(), "E hide policy hides the surface", error );
			const Ray down( Point3( 0, 0, 3 ), Vector3( 0, 0, -1 ) );
			RayIntersectionGeometric ri( down, nullRasterizerState );
			g->IntersectRay( ri, true, true, false );
			Check( !ri.bHit && !g->IntersectRay_IntersectionOnly( down, 100, true, true ), "E a hidden surface has no hits" );
			if( undo ) undo();
			Check( !g->Hidden(), "E rolling back the hide shows the surface again" );
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t5, t5, t5 }, error, nullptr ) && g->Hidden(), "E hidden again" );
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, nullptr ) && !g->Hidden() && g->InstalledFrame() == 2,
				"E a baked frame shows the surface again", error );
			g->release();
		}
		// Predictive: refuses the partial cache, a complete preview-labelled
		// cache, and a missing frame; admits a complete predictive cache.
		SimulatedMeshGeometry::Options predictive; predictive.source.predictive = true;
		Check( Bind( partial, predictive, error ) == nullptr, "E predictive refuses a partial cache", error );
		const fs::path previewComplete = scratch / "e_preview";
		Check( WriteCache( previewComplete, fx, 6, fx.count ), "E write complete preview cache" );
		Check( Bind( previewComplete, predictive, error ) == nullptr, "E predictive refuses a preview-labelled cache", error );
		const fs::path qualified = scratch / "e_predictive";
		Check( WriteCache( qualified, fx, 7, fx.count, "predictive" ), "E write predictive cache" );
		g = Bind( qualified, predictive, error );
		Check( g != nullptr, "E predictive admits a complete predictive cache", error );
		if( g ) {
			const double t1 = fx.start + fx.dt, t3 = fx.start + 3 * fx.dt;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1, t1 }, error, nullptr ), "E predictive installs", error );
			// Corrupt a frame after the predictive admission: that frame fails
			// closed and the installed frame stays.
			CacheSnapshot snap;
			Check( CacheSnapshot::Open( qualified, kBudget, snap, error ), "E reopen predictive cache" );
			for( const CacheEntry& e : snap.Entries() ) if( e.index == 3 ) {
				std::fstream f( ( qualified / ( e.digest + ".frame" ) ).string(), std::ios::in | std::ios::out | std::ios::binary );
				f.seekp( 100 ); const char x = 0x55; f.write( &x, 1 );
			}
			error.clear();
			Check( !g->PrepareAssetForRender( RenderTimeSupport{ t3, t3, t3 }, error, nullptr ) && g->InstalledFrame() == 1,
				"E predictive: a corrupted frame fails closed, the previous frame stays", error );
			std::printf( "  (refusal: %s)\n", error.c_str() );
			g->release();
		}
	}

	//////////////////////////////////////////////////////////////////
	void TestFailures( const fs::path& scratch )
	{
		std::printf( "F: corrupt, budget, topology\n" );
		Fixture fx;
		// Frame 4 carries a different topology; frame 3 has no normals and no
		// velocities (a smaller payload, for the budget check).
		Fixture other = fx; other.nx = 11;
		MeshFrame lean = fx.Frame( 0 ); lean.normals.clear(); lean.velocities.clear(); lean.velocityUnits.clear();
		const fs::path root = scratch / "f";
		Check( WriteCache( root, fx, 8, fx.count, "preview", true, { { 4, other.Frame( 4 ) }, { 0, lean } } ), "F write cache" );
		std::string error;
		SimulatedMeshGeometry* g = Bind( root, SimulatedMeshGeometry::Options(), error );
		Check( g != nullptr, "F bind", error );
		if( !g ) return;
		const double t1 = fx.start + fx.dt, t2 = fx.start + 2 * fx.dt, t4 = fx.start + 4 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1, t1 }, error, nullptr ), "F frame 1", error );
		const MeshFrame f1 = fx.Frame( 1 );
		TriangleMeshGeometryIndexed* s1 = Static( Doubles( f1.positions ), Doubles( f1.normals ), f1.indices, true, false );
		error.clear();
		Check( !g->PrepareAssetForRender( RenderTimeSupport{ t4, t4, t4 }, error, nullptr ) && g->InstalledFrame() == 1,
			"F a frame with a different topology fails (changing topology is M7)", error );
		std::printf( "  (refusal: %s)\n", error.c_str() );
		CacheSnapshot snap;
		Check( CacheSnapshot::Open( root, kBudget, snap, error ), "F reopen" );
		for( const CacheEntry& e : snap.Entries() ) if( e.index == 2 ) {
			std::fstream f( ( root / ( e.digest + ".frame" ) ).string(), std::ios::in | std::ios::out | std::ios::binary );
			f.seekp( 200 ); const char x = 0x7f; f.write( &x, 1 );
		}
		error.clear();
		Check( !g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, nullptr ) && g->InstalledFrame() == 1,
			"F a corrupt payload fails the frame (per-frame digest verification)", error );
		std::printf( "  (refusal: %s)\n", error.c_str() );
		const Compare c = CompareHits( *g, *s1 );
		Check( c.hitMismatch == 0 && c.rangeNotEqual == 0, "F after the failures the previously installed frame is intact", Describe( c ) );
		s1->release();
		g->release();

		// Decode budget: large enough for the lean bind frame (0), too small for
		// a full frame (1).
		Bytes a, b;
		EncodeMesh( lean, kBudget, a, error );
		EncodeMesh( fx.Frame( 1 ), kBudget, b, error );
		auto minimal = [&]( const Bytes& bytes ) {
			std::size_t lo = bytes.size(), hi = kBudget;
			while( hi - lo > 1 ) { const std::size_t mid = lo + ( hi - lo ) / 2; std::string e; MeshFrame out; ( DecodeMesh( bytes, mid, out, e ) ? hi : lo ) = mid; }
			return hi;
		};
		const std::size_t needLean = minimal( a ), needFull = minimal( b );
		SimulatedMeshGeometry::Options tight;
		tight.source.frameBudget = ( needLean + needFull ) / 2;
		std::printf( "  decode budget: bind frame needs %zu, full frame %zu, binding with %zu\n", needLean, needFull, tight.source.frameBudget );
		g = Bind( root, tight, error );
		Check( g != nullptr && needLean < needFull, "F bind under a tight budget", error );
		if( g ) {
			error.clear();
			Check( !g->PrepareAssetForRender( RenderTimeSupport{ t1, t1, t1 }, error, nullptr ) && g->InstalledFrame() == 0,
				"F a frame over the decode budget fails and the bind frame stays", error );
			std::printf( "  (refusal: %s)\n", error.c_str() );
			g->release();
		}
	}

	//////////////////////////////////////////////////////////////////
	void TestFreezeAndRollback( const fs::path& scratch )
	{
		std::printf( "G: freeze, rollback\n" );
		Fixture fx;
		const fs::path root = scratch / "g";
		Check( WriteCache( root, fx, 9, fx.count ), "G write cache" );
		std::string error;
		SimulatedMeshGeometry* g = Bind( root, SimulatedMeshGeometry::Options(), error );
		if( !g ) { Check( false, "G bind", error ); return; }
		const double t1 = fx.start + fx.dt, t2 = fx.start + 2 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1, t1 }, error, nullptr ), "G frame 1" );
		Check( g->AcquireAssetFreeze( error ), "G freeze" );
		error.clear();
		Check( !g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, nullptr ) && error == "mutation_frozen" && g->InstalledFrame() == 1,
			"G a prepare under the render freeze is refused", error );
		Check( g->ReleaseAssetFreeze( error ), "G release reports the frame stayed put", error );
		std::function<void()> undo;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t2, t2 - 0.05, t2 + 0.05 }, error, &undo ) && g->InstalledFrame() == 2 && g->BlurActive(),
			"G frame 2 with keys", error );
		Check( static_cast<bool>( undo ), "G the prepare hands back an undo" );
		if( undo ) undo();
		const MeshFrame f1 = fx.Frame( 1 );
		TriangleMeshGeometryIndexed* s1 = Static( Doubles( f1.positions ), Doubles( f1.normals ), f1.indices, true, false );
		const Compare c = CompareHits( *g, *s1 );
		Check( g->InstalledFrame() == 1 && !g->BlurActive() && c.hitMismatch == 0 && c.rangeNotEqual == 0,
			"G rollback restores the previous frame exactly", Describe( c ) );
		s1->release();
		g->release();
	}

	//////////////////////////////////////////////////////////////////
	std::string SceneText( const fs::path& cache, const std::string& extra, const double exposure )
	{
		char cam[256];
		std::snprintf( cam, sizeof( cam ), "orthographic_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 4\n\texposure %g\n}\n\n", exposure );
		return std::string( "RISE ASCII SCENE 7\n\nfilm\n{\n\twidth 8\n\theight 8\n}\n\n" ) + cam +
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance white\n}\n\n"
			"simulated_mesh_geometry\n{\n\tname cloth\n\tcache " + cache.string() + "\n\tdouble_sided TRUE\n" + extra + "}\n\n"
			"standard_object\n{\n\tname sheet\n\tgeometry cloth\n\tmaterial m\n}\n\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\toidn_denoise FALSE\n\tradiance_map white\n\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n";
	}

	bool LoadScene( const std::string& text, const fs::path& dir, IJobPriv*& job )
	{
		const fs::path path = dir / "scene.RISEscene";
		{ std::ofstream o( path ); o << text; }
		job = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job ) return false;
		if( job->LoadAsciiSceneViaCst( path.string().c_str() ) ) return true;
		safe_release( job );
		return false;
	}

	void TestJob( const fs::path& scratch )
	{
		std::printf( "H: the chunk through a Job\n" );
		Fixture fx;
		const fs::path root = scratch / "h";
		Check( WriteCache( root, fx, 10, fx.count ), "H write cache" );
		IJobPriv* job = nullptr;
		Check( LoadScene( SceneText( root, "", 0.0 ), scratch, job ), "H scene with simulated_mesh_geometry loads" );
		if( job ) {
			SimulatedMeshGeometry* g = dynamic_cast<SimulatedMeshGeometry*>( job->GetGeometries()->GetItem( "cloth" ) );
			Check( g != nullptr, "H the geometry is a SimulatedMeshGeometry" );
			if( g ) {
				// A one-frame animation render at frame 4's time goes through
				// the prepared path and installs frame 4.
				const double t4 = fx.start + 4 * fx.dt;
				Check( job->RasterizeAnimation( t4, t4 + 1.0, 1, false, false ), "H prepared animation render" );
				Check( g->InstalledFrame() == 4, "H the render installed the frame for its time" );
				Check( job->Rasterize(), "H a still render goes through the prepared path too" );
				Check( g->InstalledFrame() == 0, "H the still installed the frame for the animation start time" );
			}
			safe_release( job );
		}
		IJobPriv* refused = nullptr;
		Check( !LoadScene( SceneText( root, "\tfidelity predictive\n", 0.0 ), scratch, refused ), "H a predictive binding of a preview cache fails the load" );
		safe_release( refused );
		IJobPriv* missing = nullptr;
		Check( !LoadScene( SceneText( scratch / "no_such_cache", "", 0.0 ), scratch, missing ), "H a missing cache fails the load" );
		safe_release( missing );
		// A render whose frame fails (time outside the support under `error`)
		// is refused rather than rendered with a stale frame.
		IJobPriv* strict = nullptr;
		Check( LoadScene( SceneText( root, "\tend_policy error\n", 0.0 ), scratch, strict ), "H end_policy error loads" );
		if( strict ) {
			Check( !strict->RasterizeAnimation( 9.0, 10.0, 1, false, false ), "H a frame outside the support fails the render" );
			safe_release( strict );
		}
	}

	//////////////////////////////////////////////////////////////////
	void Bench( const fs::path& scratch )
	{
		std::printf( "bench (not a gate): per-frame install cost\n" );
		for( const int n : { 51, 101, 317 } ) {
			Fixture fx; fx.nx = n; fx.ny = n; fx.count = 12; fx.dt = 1.0 / 30.0;
			const fs::path root = scratch / ( "bench" + std::to_string( n ) );
			if( !WriteCache( root, fx, 20 + n, fx.count ) ) continue;
			std::string error;
			// Full UpdateVertices vs the lean install on the same frames.
			std::vector<double> full, leanAreas, leanNoAreas;
			for( int mode = 0; mode < 3; ++mode ) {
				const MeshFrame f0 = fx.Frame( 0 );
				TriangleMeshGeometryIndexed* s = Static( Doubles( f0.positions ), Doubles( f0.normals ), f0.indices, true, false );
				for( int k = 1; k < fx.count; ++k ) {
					const MeshFrame f = fx.Frame( k );
					const std::vector<double> p = Doubles( f.positions ), nn = Doubles( f.normals );
					VerticesListType v; NormalsListType nl;
					for( std::size_t i = 0; i < p.size() / 3; ++i ) { v.push_back( Vertex( p[3*i], p[3*i+1], p[3*i+2] ) ); nl.push_back( Normal( nn[3*i], nn[3*i+1], nn[3*i+2] ) ); }
					const auto t0 = std::chrono::steady_clock::now();
					if( mode == 0 ) s->UpdateVertices( v, nl );
					else s->UpdateVerticesLean( v, nl, mode == 1 );
					const double ms = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
					( mode == 0 ? full : mode == 1 ? leanAreas : leanNoAreas ).push_back( ms );
				}
				s->release();
			}
			// Logging alone: the two Info lines a full refit prints.
			const auto l0 = std::chrono::steady_clock::now();
			for( int i = 0; i < 100; ++i ) {
				GlobalLog()->PrintEx( eLog_Info, "BVH:: Refit %u nodes (%u prims) in %u ms (SAH ratio %.2fx)", 1u, 2u, 3u, 1.0 );
				GlobalLog()->PrintEx( eLog_Info, "BVH:: Float filter enabled (%u triangles precomputed, %u bytes)", 1u, 2u );
			}
			const double logMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - l0 ).count() / 100.0;
			// The whole per-frame install (read, verify, decode, keys, install).
			std::vector<double> install, installBlur, sample;
			SimulatedMeshGeometry* g = Bind( root, SimulatedMeshGeometry::Options(), error );
			if( g ) {
				// The cache side alone: map, read, SHA-256 verify, decode, convert.
				for( int k = 1; k < fx.count; ++k ) {
					const double t = fx.start + k * fx.dt;
					MeshFrameSample out;
					auto t0 = std::chrono::steady_clock::now();
					g->Source().Sample( t, t, t, true, out, error );
					sample.push_back( std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
				}
				for( int k = 1; k < fx.count; ++k ) {
					const double t = fx.start + k * fx.dt;
					auto t0 = std::chrono::steady_clock::now();
					g->PrepareAssetForRender( RenderTimeSupport{ t, t, t }, error, nullptr );
					install.push_back( std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
				}
				for( int k = 1; k < fx.count; ++k ) {
					const double t = fx.start + k * fx.dt;
					auto t0 = std::chrono::steady_clock::now();
					g->PrepareAssetForRender( RenderTimeSupport{ t, t - 0.25 * fx.dt, t + 0.25 * fx.dt }, error, nullptr );
					installBlur.push_back( std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
				}
			}
			// Intersection cost of the installed mesh without and with shutter
			// keys (float filter off, per-ray interpolation), mid-shutter.
			double nsStatic = 0, nsBlur = 0;
			if( g ) {
				std::vector<Ray> rays;
				for( int i = 0; i < 200000; ++i ) {
					const double u = ( ( i * 7919 ) % 200000 ) / 200000.0, v = ( ( i * 104729 ) % 200000 ) / 200000.0;
					rays.push_back( Ray( Point3( -1.1 + 2.2 * u, -0.85 + 1.7 * v, 3.0 ), Vector3( 0, 0, -1 ) ) );
				}
				const double t = fx.start + 5 * fx.dt;
				auto cast = [&]() {
					std::vector<double> ns;
					for( int r = 0; r < 5; ++r ) {
						const auto t0 = std::chrono::steady_clock::now();
						for( const Ray& ray : rays ) { RayIntersectionGeometric ri( ray, nullRasterizerState ); g->IntersectRay( ri, true, true, false ); }
						ns.push_back( std::chrono::duration<double, std::nano>( std::chrono::steady_clock::now() - t0 ).count() / rays.size() );
					}
					std::sort( ns.begin(), ns.end() );
					return ns[ns.size() / 2];
				};
				g->PrepareAssetForRender( RenderTimeSupport{ t, t, t }, error, nullptr );
				nsStatic = cast();
				g->PrepareAssetForRender( RenderTimeSupport{ t, t - 0.25 * fx.dt, t + 0.25 * fx.dt }, error, nullptr );
				nsBlur = cast();
				g->release();
			}
			auto med = []( std::vector<double> v ) { if( v.empty() ) return 0.0; std::sort( v.begin(), v.end() ); return v[v.size() / 2]; };
			auto mn = []( const std::vector<double>& v ) { return v.empty() ? 0.0 : *std::min_element( v.begin(), v.end() ); };
			auto mx = []( const std::vector<double>& v ) { return v.empty() ? 0.0 : *std::max_element( v.begin(), v.end() ); };
			std::printf( "  %6d vertices %6d triangles | UpdateVertices %7.3f ms [%.3f-%.3f] | lean %7.3f ms [%.3f-%.3f] | lean w/o areas %7.3f ms | 2 log lines %.4f ms | cache read+verify+decode %7.3f ms | install %7.3f ms [%.3f-%.3f] | install+keys %7.3f ms [%.3f-%.3f]\n",
				n * n, 2 * ( n - 1 ) * ( n - 1 ), med( full ), mn( full ), mx( full ), med( leanAreas ), mn( leanAreas ), mx( leanAreas ),
				med( leanNoAreas ), 2 * logMs, med( sample ), med( install ), mn( install ), mx( install ), med( installBlur ), mn( installBlur ), mx( installBlur ) );
			std::printf( "         closest-hit %.1f ns/ray without keys, %.1f ns/ray with shutter keys (x%.2f)\n", nsStatic, nsBlur, nsStatic > 0 ? nsBlur / nsStatic : 0.0 );
		}
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	const bool bench = argc > 1 && std::string( argv[1] ) == "--bench";
	const fs::path scratch = RISE::TestSupport::ScratchRunDir( "rise_simulated_mesh_", "suite" );
	std::error_code ec;
	fs::create_directories( scratch, ec );
	std::printf( "SimulatedMeshGeometryTest (simulation M3b)\n" );
	if( bench ) {
		Bench( scratch );
	} else {
		TestStaticEquality( scratch );
		TestKeys( scratch, true );
		TestKeys( scratch, false );
		TestTimeMap( scratch );
		TestMissingAndPredictive( scratch );
		TestFailures( scratch );
		TestFreezeAndRollback( scratch );
		TestJob( scratch );
	}
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_simulated_mesh_" ), ec );
	std::printf( "\n%d passed, %d failed\n", passes, failures );
	return failures ? 1 : 0;
}
