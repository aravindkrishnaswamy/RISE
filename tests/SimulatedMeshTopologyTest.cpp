//////////////////////////////////////////////////////////////////////
//
//  SimulatedMeshTopologyTest.cpp - M7 prerequisites: changing-topology and
//    empty meshes in simulated_mesh_geometry
//    (Geometry/SimulatedMeshGeometry.h, Simulation/SimulatedMeshSource.h,
//    TriangleMeshGeometryIndexed::ReplaceTopologyLean, the parallel BVH
//    build, the recycled mailbox ids).
//
//    The test writes its own mesh.v2 cache (no solver): a UV sphere that
//    refines, splits into two, vanishes (an empty frame), reappears as two
//    spheres, moves without changing topology, and merges back into one.
//      T  every frame installed at its own time hits exactly like a static
//         indexed mesh built from that frame (hits, distances, normals and
//         UVs bit-equal); the empty frame has no hits and no area; refit is
//         chosen exactly when the topology digest is unchanged
//      K  shutter keys from the current frame's own velocities on a
//         changed-topology frame (open / close sample times vs static meshes
//         at the key poses); without velocities, no keys across a topology
//         change
//      R  rollback across a topology change; rebind to a source of another
//         topology
//      P  the parallel BVH build is bit-identical to the serial one (the
//         serialized mesh, BVH included)
//      J  through a Job: a prepared animation over every frame, the empty one
//         included, renders
//      V  the interactive viewport scrubs through every frame (the M3c path)
//      M  memory over 1,000 frames: a changing-topology playback in place,
//         and 1,000 geometries created and destroyed (the mailbox table stays
//         bounded)
//
//    Usage: SimulatedMeshTopologyTest [--bench [--extended]]
//      --bench (not a gate) times the per-frame rebuild (serial and parallel
//      BVH) against a refit at 100k and 1M triangles (5M with --extended).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Geometry/SimulatedMeshGeometry.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Simulation/SimulatedMeshSource.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationMesh.h"
#include "../src/Library/Animation/TimeIndexedView.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/MemoryBuffer.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/ThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

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

	constexpr std::size_t kBudget = 256u * 1024 * 1024;

	std::uint32_t Bits( const double v ) { const float f = float( v ); std::uint32_t b; std::memcpy( &b, &f, 4 ); return b; }
	double Value( const std::uint32_t b ) { float f; std::memcpy( &f, &b, 4 ); return double( f ); }
	std::vector<double> Doubles( const std::vector<std::uint32_t>& b ) { std::vector<double> d; d.reserve( b.size() ); for( auto x : b ) d.push_back( Value( x ) ); return d; }

	double ResidentMB()
	{
#ifdef __APPLE__
		mach_task_basic_info info{};
		mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
		if( task_info( mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n ) != KERN_SUCCESS ) return -1;
		return double( info.resident_size ) / ( 1 << 20 );
#else
		std::ifstream statm( "/proc/self/statm" );
		double pages = 0, resident = 0;
		statm >> pages >> resident;
		return resident * 4096.0 / ( 1 << 20 );
#endif
	}

	//! A UV sphere appended to `f`: (lat+1) x (lon+1) vertices (seam and poles
	//! duplicated), velocity = translation `v` plus expansion rate `grow`.
	void AddSphere( MeshFrame& f, const double c[3], const double r, const int lat, const int lon,
		const double v[3], const double grow, const bool velocities )
	{
		const std::uint32_t base = static_cast<std::uint32_t>( f.positions.size() / 3 );
		MeshUVSet* uv = f.uvSets.empty() ? nullptr : &f.uvSets[0];
		for( int i = 0; i <= lat; ++i ) for( int j = 0; j <= lon; ++j ) {
			const double pi = 3.14159265358979323846;
			const double th = pi * i / lat, ph = 2 * pi * j / lon;
			const double n[3] = { std::sin( th ) * std::cos( ph ), std::sin( th ) * std::sin( ph ), std::cos( th ) };
			for( int k = 0; k < 3; ++k ) f.positions.push_back( Bits( c[k] + r * n[k] ) );
			if( velocities ) for( int k = 0; k < 3; ++k ) f.velocities.push_back( Bits( v[k] + grow * n[k] ) );
			if( uv ) { uv->values.push_back( Bits( double( j ) / lon ) ); uv->values.push_back( Bits( double( i ) / lat ) ); }
		}
		for( int i = 0; i < lat; ++i ) for( int j = 0; j < lon; ++j ) {
			const std::uint32_t a = base + i * ( lon + 1 ) + j, b = a + 1, cc = a + ( lon + 1 ), d = cc + 1;
			f.indices.insert( f.indices.end(), { a, cc, b, b, cc, d } );
		}
	}

	struct Fixture
	{
		double start = 0.0, dt = 0.25;
		int count = 7;
		bool velocities = true;
		MeshFrame Frame( const int k ) const
		{
			MeshFrame f;
			f.uvSets.push_back( MeshUVSet{ "uv", "1", {} } );
			if( velocities ) f.velocityUnits = "m/s";
			const double still[3] = { 0, 0, 0 }, drift[3] = { 0.4, 0.1, 0 };
			const double left[3] = { -0.6, 0, 0 }, right[3] = { 0.6, 0, 0 };
			switch( k ) {
			case 0: { const double c[3] = { 0, 0, 0 }; AddSphere( f, c, 0.5, 8, 12, drift, 0.2, velocities ); break; }
			case 1: { const double c[3] = { 0.1, 0.02, 0 }; AddSphere( f, c, 0.55, 12, 18, drift, 0.1, velocities ); break; }
			case 2: { const double a[3] = { -0.3, 0, 0 }, b[3] = { 0.3, 0, 0 };
				AddSphere( f, a, 0.3, 8, 12, left, 0.0, velocities ); AddSphere( f, b, 0.3, 8, 12, right, 0.0, velocities ); break; }
			case 3: break;	// empty: V = T = 0
			case 4: case 5: { const double s = 0.45 + 0.1 * ( k - 4 );
				const double a[3] = { -s, 0, 0 }, b[3] = { s, 0, 0.1 };
				AddSphere( f, a, 0.25, 10, 14, left, 0.0, velocities ); AddSphere( f, b, 0.2, 6, 10, right, 0.0, velocities ); break; }
			case 6: { const double c[3] = { 0, 0.1, 0 }; AddSphere( f, c, 0.6, 9, 13, still, 0.0, velocities ); break; }
			}
			if( f.positions.empty() ) { f.uvSets.clear(); f.velocityUnits = velocities ? "m/s" : ""; if( !velocities ) f.velocities.clear(); }
			if( f.velocities.empty() ) f.velocityUnits.clear();
			return f;
		}
	};

	std::string Hex64( const unsigned salt ) { char b[65]; for( int i = 0; i < 64; ++i ) b[i] = "0123456789abcdef"[( salt * 7 + i * 13 ) & 15]; b[64] = 0; return b; }

	bool WriteCache( const fs::path& root, const Fixture& fx, const unsigned salt )
	{
		std::error_code ec;
		fs::remove_all( root, ec );
		fs::create_directories( root.parent_path(), ec );
		CacheDescription d;
		d.key = Hex64( salt ); d.epoch = Hex64( salt + 1000 );
		d.producer = "m7-topology-fixture"; d.producerVersion = "1";
		d.firstIndex = 0; d.count = fx.count; d.startTime = fx.start; d.interval = fx.dt; d.hold = true;
		d.outputs = { { "surface", "mesh.v2", { { "position", "m", "f32x3" }, { "velocity", "m/s", "f32x3" } } } };
		CacheWriter writer;
		std::string error;
		if( !writer.Begin( root, d, "", false, kBudget, error ) ) { std::printf( "  cache Begin: %s\n", error.c_str() ); return false; }
		for( int k = 0; k < fx.count; ++k ) {
			Bytes bytes, statics;
			std::string staticDigest;
			if( !EncodeMeshV2( fx.Frame( k ), MeshStaticLayout(), kBudget, bytes, statics, staticDigest, error ) ||
				!writer.Append( k, "surface", fx.start + k * fx.dt, bytes, error ) ) {
				std::printf( "  cache Append %d: %s\n", k, error.c_str() );
				return false;
			}
		}
		return writer.Seal( error ) || ( std::printf( "  cache Seal: %s\n", error.c_str() ), false );
	}

	TriangleMeshGeometryIndexed* Static( const std::vector<double>& p, const std::vector<double>& uv,
		const std::vector<std::uint32_t>& idx )
	{
		TriangleMeshGeometryIndexed* g = new TriangleMeshGeometryIndexed( true, true );
		g->BeginIndexedTriangles();
		for( std::size_t v = 0; v < p.size() / 3; ++v ) g->AddVertex( Vertex( p[3*v], p[3*v+1], p[3*v+2] ) );
		for( std::size_t v = 0; v < uv.size() / 2; ++v ) g->AddTexCoord( Point2( uv[2*v], uv[2*v+1] ) );
		for( std::size_t t = 0; t < idx.size(); t += 3 ) {
			IndexedTriangle tri;
			for( int k = 0; k < 3; ++k ) { tri.iVertices[k] = idx[t+k]; tri.iNormals[k] = idx[t+k]; tri.iCoords[k] = uv.empty() ? 0 : idx[t+k]; }
			g->AddIndexedTriangle( tri );
		}
		g->DoneIndexedTriangles();
		return g;
	}

	std::vector<Ray> Rays()
	{
		std::vector<Ray> rays;
		for( int j = 0; j < 48; ++j ) for( int i = 0; i < 48; ++i ) {
			const Point3 o( -1.3 + 2.6 * ( i + 0.37 ) / 48, -0.9 + 1.8 * ( j + 0.61 ) / 48, 3.0 );
			const Vector3 d( 0.03 * std::sin( 0.7 * i ), 0.02 * std::cos( 0.3 * j ), -1.0 );
			rays.push_back( Ray( o, Vector3Ops::Normalize( d ) ) );
		}
		return rays;
	}

	struct Compare { int rays = 0, hitsA = 0, hitMismatch = 0, rangeNotEqual = 0, normalNotEqual = 0, uvNotEqual = 0, shadowMismatch = 0; double maxRange = 0; };

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
			if( ra.vNormal.x != rb.vNormal.x || ra.vNormal.y != rb.vNormal.y || ra.vNormal.z != rb.vNormal.z ) ++c.normalNotEqual;
			if( ra.ptCoord.x != rb.ptCoord.x || ra.ptCoord.y != rb.ptCoord.y ) ++c.uvNotEqual;
		}
		return c;
	}

	std::string Describe( const Compare& c )
	{
		char b[256];
		std::snprintf( b, sizeof( b ), "%d rays, %d hits, %d hit / %d any-hit mismatches, %d ranges (max %.3g), %d normals, %d UVs not bit-equal",
			c.rays, c.hitsA, c.hitMismatch, c.shadowMismatch, c.rangeNotEqual, c.maxRange, c.normalNotEqual, c.uvNotEqual );
		return b;
	}

	bool BitEqual( const Compare& c ) { return c.hitMismatch == 0 && c.shadowMismatch == 0 && c.rangeNotEqual == 0 && c.normalNotEqual == 0 && c.uvNotEqual == 0; }

	SimulatedMeshGeometry::Options FaceOptions()
	{
		SimulatedMeshGeometry::Options o;
		o.doubleSided = true;
		o.faceNormals = true;
		return o;
	}

	struct SampleTimeScope
	{
		TimeIndexed::SampleTime previous;
		explicit SampleTimeScope( const double t ) : previous( TimeIndexed::tlsSampleTime ) { TimeIndexed::tlsSampleTime = { t, true }; }
		~SampleTimeScope() { TimeIndexed::tlsSampleTime = previous; }
	};

	//////////////////////////////////////////////////////////////////
	void TestFrames( const fs::path& root, const Fixture& fx )
	{
		std::printf( "T: every frame vs a static mesh; refit only for an unchanged digest\n" );
		std::string error;
		SimulatedMeshGeometry* g = SimulatedMeshGeometry::Create( root.string(), FaceOptions(), error );
		Check( g != nullptr, "T bind", error );
		if( !g ) return;
		const unsigned long long rebuilds0 = g->TopologyRebuilds();
		std::string previousTopology = g->InstalledTopology();
		for( int k = 0; k < fx.count; ++k ) {
			const double t = fx.start + k * fx.dt;
			const unsigned long long before = g->TopologyRebuilds();
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t, t, t }, error, nullptr ) && g->InstalledFrame() == k,
				"T install frame " + std::to_string( k ), error );
			const MeshFrame f = fx.Frame( k );
			const std::string digest = MeshTopologyDigest( static_cast<std::uint32_t>( f.positions.size() / 3 ), f.indices );
			Check( g->InstalledTopology() == digest, "T installed digest is the frame's: " + std::to_string( k ) );
			const bool changed = k > 0 && digest != previousTopology;
			Check( g->TopologyRebuilds() == before + ( changed ? 1 : 0 ),
				"T frame " + std::to_string( k ) + ( changed ? " replaced the topology" : " refit the installed topology" ) );
			previousTopology = digest;
			Check( g->TriangleCount() == f.indices.size() / 3, "T triangle count follows the frame " + std::to_string( k ) );
			TriangleMeshGeometryIndexed* s = Static( Doubles( f.positions ), f.uvSets.empty() ? std::vector<double>() : Doubles( f.uvSets[0].values ), f.indices );
			const Compare c = CompareHits( *g, *s );
			std::printf( "  frame %d (%zu triangles): %s\n", k, f.indices.size() / 3, Describe( c ).c_str() );
			if( f.indices.empty() ) {
				Check( c.hitsA == 0 && c.hitMismatch == 0 && c.shadowMismatch == 0, "T the empty frame has no hits" );
				Check( g->GetArea() == 0.0, "T the empty frame has no area" );
				Point3 p; Vector3 n; Point2 uv;
				g->UniformRandomPoint( &p, &n, &uv, Point3( 0.3, 0.6, 0.9 ) );
				Check( std::isfinite( p.x ) && n.z == 1.0, "T sampling the empty frame answers a finite point" );
				const BoundingBox box = g->GenerateBoundingBox();
				Check( std::isfinite( box.ll.x ) && std::isfinite( box.ur.z ), "T the empty frame has finite bounds" );
			} else {
				Check( c.hitsA > 100 && BitEqual( c ), "T frame " + std::to_string( k ) + " bit-equal to a static mesh", Describe( c ) );
				char areas[96];
				std::snprintf( areas, sizeof( areas ), "%.17g vs %.17g", g->GetArea(), s->GetArea() );
				// ComputeAreasUncertified and the full build's ComputeAreas share the
				// formula; the library's fast-math build may round the running sum
				// differently in the last place.
				Check( std::fabs( g->GetArea() - s->GetArea() ) <= 1e-12 * s->GetArea(),
					"T frame " + std::to_string( k ) + " area equals the static mesh's (to rounding)", areas );
			}
			s->release();
		}
		Check( g->TopologyRebuilds() - rebuilds0 == 5, "T five topology replacements over the seven frames (frame 5 refits)" );
		// Re-preparing the installed frame is a no-op; going back to frame 2 rebuilds again.
		const unsigned long long installs = g->InstallCount();
		const double t6 = fx.start + 6 * fx.dt, t2 = fx.start + 2 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t6, t6, t6 }, error, nullptr ) && g->InstallCount() == installs, "T same frame again: no install" );
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, nullptr ) && g->InstalledFrame() == 2, "T scrub back to frame 2", error );
		g->release();

		// An empty bind frame: a cache whose first frame is the empty one.
		const fs::path emptyFirst = root.parent_path() / "empty_first";
		{
			std::error_code ec;
			fs::remove_all( emptyFirst, ec );
			CacheDescription d;
			d.key = Hex64( 91 ); d.epoch = Hex64( 92 ); d.producer = "m7-topology-fixture"; d.producerVersion = "1";
			d.count = 2; d.interval = 0.25; d.hold = true;
			d.outputs = { { "surface", "mesh.v1", { { "position", "m", "f32x3" } } } };
			CacheWriter w;
			MeshFrame empty, one = fx.Frame( 6 );
			one.uvSets.clear();
			Bytes b0, b1;
			Check( w.Begin( emptyFirst, d, "", false, kBudget, error ) && EncodeMesh( empty, kBudget, b0, error ) &&
				EncodeMesh( one, kBudget, b1, error ) && w.Append( 0, "surface", 0.0, b0, error ) &&
				w.Append( 1, "surface", 0.25, b1, error ) && w.Seal( error ), "T write an empty-first cache", error );
			SimulatedMeshGeometry* e = SimulatedMeshGeometry::Create( emptyFirst.string(), FaceOptions(), error );
			Check( e && e->TriangleCount() == 0 && !e->Hidden(), "T an empty bind frame binds (no triangles)", error );
			if( e ) {
				Check( e->PrepareAssetForRender( RenderTimeSupport{ 0.25, 0.25, 0.25 }, error, nullptr ) &&
					e->TriangleCount() == one.indices.size() / 3, "T the next frame grows from the empty bind frame", error );
				TriangleMeshGeometryIndexed* s = Static( Doubles( one.positions ), {}, one.indices );
				const Compare c = CompareHits( *e, *s );
				Check( c.hitsA > 100 && BitEqual( c ), "T grown frame bit-equal to a static mesh", Describe( c ) );
				s->release();
				e->release();
			}
		}
	}

	//////////////////////////////////////////////////////////////////
	void TestKeys( const fs::path& scratch, const Fixture& fx )
	{
		std::printf( "K: shutter keys on a changed-topology frame\n" );
		std::string error;
		const fs::path root = scratch / "topology";
		SimulatedMeshGeometry* g = SimulatedMeshGeometry::Create( root.string(), FaceOptions(), error );
		Check( g != nullptr, "K bind", error );
		if( !g ) return;
		const double t2 = fx.start + 2 * fx.dt, E = 0.125;
		const RenderTimeSupport support{ t2, t2 - 0.5 * E, t2 + 0.5 * E };
		Check( g->PrepareAssetForRender( support, error, nullptr ) && g->BlurActive() && g->InstalledFrame() == 2,
			"K frame 2 installs with shutter keys (topology replaced)", error );
		const MeshFrame f2 = fx.Frame( 2 );
		const std::vector<double> p = Doubles( f2.positions ), v = Doubles( f2.velocities ), uv = Doubles( f2.uvSets[0].values );
		for( const double fraction : { 0.0, 1.0 } ) {
			std::vector<double> key( p.size() );
			const double offset = ( fraction - 0.5 ) * E;
			for( std::size_t i = 0; i < p.size(); ++i ) key[i] = p[i] + v[i] * offset;
			TriangleMeshGeometryIndexed* s = Static( key, uv, f2.indices );
			SampleTimeScope scope( support.open + fraction * ( support.close - support.open ) );
			const Compare c = CompareHits( *g, *s );
			std::printf( "  fraction %.0f vs static mesh at the key: %s\n", fraction, Describe( c ).c_str() );
			Check( c.hitsA > 100 && c.hitMismatch <= 2 && c.maxRange < 1e-12 && c.uvNotEqual <= c.hitMismatch + 2,
				"K blurred changed-topology frame at a shutter end hits like a static mesh at that key", Describe( c ) );
			s->release();
		}
		g->release();

		// Without velocities: the neighbours (frames 1, 3) have other
		// topologies, so frame 2 gets no keys; frames 4 and 5 share one, so
		// frame 4 does (central / one-sided difference).
		Fixture noVel = fx;
		noVel.velocities = false;
		const fs::path rootNoVel = scratch / "topology_novel";
		Check( WriteCache( rootNoVel, noVel, 31 ), "K write a cache without velocities" );
		SimulatedMeshGeometry* h = SimulatedMeshGeometry::Create( rootNoVel.string(), FaceOptions(), error );
		Check( h != nullptr, "K bind without velocities", error );
		if( h ) {
			Check( h->PrepareAssetForRender( support, error, nullptr ) && h->InstalledFrame() == 2 && !h->BlurActive(),
				"K no keys across a topology change (no vertex correspondence)", error );
			const double t4 = fx.start + 4 * fx.dt;
			Check( h->PrepareAssetForRender( RenderTimeSupport{ t4, t4 - 0.5 * E, t4 + 0.5 * E }, error, nullptr ) &&
				h->InstalledFrame() == 4 && h->BlurActive(), "K keys from a neighbour of the same topology", error );
			h->release();
		}
	}

	//////////////////////////////////////////////////////////////////
	void TestRollback( const fs::path& scratch, const Fixture& fx )
	{
		std::printf( "R: rollback across a topology change; rebind\n" );
		std::string error;
		const fs::path root = scratch / "topology";
		SimulatedMeshGeometry* g = SimulatedMeshGeometry::Create( root.string(), FaceOptions(), error );
		if( !g ) { Check( false, "R bind", error ); return; }
		const double t1 = fx.start + fx.dt, t2 = fx.start + 2 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1, t1 }, error, nullptr ), "R frame 1", error );
		std::function<void()> undo;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, &undo ) && g->InstalledFrame() == 2 && undo,
			"R frame 2 with an undo", error );
		if( undo ) undo();
		const MeshFrame f1 = fx.Frame( 1 );
		TriangleMeshGeometryIndexed* s1 = Static( Doubles( f1.positions ), Doubles( f1.uvSets[0].values ), f1.indices );
		Compare c = CompareHits( *g, *s1 );
		Check( g->InstalledFrame() == 1 && BitEqual( c ) && c.hitsA > 100, "R the undo restores frame 1's topology exactly", Describe( c ) );
		s1->release();
		// Rebind to a source whose bind frame has another topology (frame 0 vs
		// frame 1 installed): accepted; the next prepare replaces the topology.
		std::unique_ptr<MeshCacheSource> other;
		Check( MeshCacheSource::Open( root, MeshSourceOptions(), other, error ), "R open a second source", error );
		Check( g->RebindSource( other, error ), "R rebind to a source of another bind topology (M7)", error );
		const double t5 = fx.start + 5 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t5, t5, t5 }, error, nullptr ) && g->InstalledFrame() == 5, "R prepare after rebind", error );
		const MeshFrame f5 = fx.Frame( 5 );
		TriangleMeshGeometryIndexed* s5 = Static( Doubles( f5.positions ), Doubles( f5.uvSets[0].values ), f5.indices );
		c = CompareHits( *g, *s5 );
		Check( BitEqual( c ) && c.hitsA > 100, "R frame 5 after the rebind bit-equal to a static mesh", Describe( c ) );
		s5->release();
		g->release();
	}

	//////////////////////////////////////////////////////////////////
	//! A triangle soup plus a dense sphere: N triangles.
	void SoupFrame( const std::size_t triangles, const unsigned seed, VerticesListType& p, std::vector<std::uint32_t>& idx )
	{
		p.clear(); idx.clear();
		std::uint64_t s = 0x9e3779b97f4a7c15ull * ( seed + 1 );
		auto rnd = [&s]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return double( s >> 11 ) * ( 1.0 / 9007199254740992.0 ); };
		// Half a grid surface (shared vertices), half a soup (some coincident centroids).
		const std::size_t half = triangles / 2;
		const int n = std::max( 2, int( std::sqrt( double( half ) / 2.0 ) ) + 1 );
		for( int j = 0; j < n; ++j ) for( int i = 0; i < n; ++i )
			p.push_back( Vertex( double( i ) / n, double( j ) / n, 0.1 * std::sin( 9.0 * i / n ) * std::cos( 7.0 * j / n ) ) );
		for( int j = 0; j + 1 < n && idx.size() / 3 < half; ++j ) for( int i = 0; i + 1 < n && idx.size() / 3 + 1 < half; ++i ) {
			const std::uint32_t a = j * n + i, b = a + 1, c = a + n, d = c + 1;
			idx.insert( idx.end(), { a, b, c, b, d, c } );
		}
		while( idx.size() / 3 < triangles ) {
			const std::uint32_t base = static_cast<std::uint32_t>( p.size() );
			const double cx = rnd(), cy = rnd(), cz = rnd() * 0.5 - 0.25;
			const double e = 0.002 + 0.01 * rnd();
			// Every 97th triangle repeats an earlier centroid exactly.
			const bool repeat = ( idx.size() / 3 ) % 97 == 0 && base > 3;
			const Point3 c0 = repeat ? p[base - 3] : Vertex( cx, cy, cz );
			p.push_back( c0 );
			p.push_back( Vertex( c0.x + e, c0.y, c0.z ) );
			p.push_back( Vertex( c0.x, c0.y + e, c0.z + 0.5 * e ) );
			idx.insert( idx.end(), { base, base + 1, base + 2 } );
		}
	}

	std::vector<char> Serialized( const TriangleMeshGeometryIndexed& g )
	{
		MemoryBuffer* b = new MemoryBuffer();
		g.Serialize( *b );
		std::vector<char> out( b->Pointer(), b->Pointer() + b->Size() );
		b->release();
		return out;
	}

	void TestParallelBuild()
	{
		std::printf( "P: parallel BVH build == serial build\n" );
		const unsigned workers = GlobalThreadPool().NumWorkers();
		for( const std::size_t n : { std::size_t( 1000 ), std::size_t( 300000 ) } ) {
			VerticesListType p;
			std::vector<std::uint32_t> idx;
			SoupFrame( n, 7, p, idx );
			TriangleMeshGeometryIndexed* serial = new TriangleMeshGeometryIndexed( false, true );
			TriangleMeshGeometryIndexed* parallel = new TriangleMeshGeometryIndexed( false, true );
			const auto t0 = std::chrono::steady_clock::now();
			const bool a = serial->ReplaceTopologyLean( p, NormalsListType(), TexCoordsListType(), idx, false );
			const auto t1 = std::chrono::steady_clock::now();
			const bool b = parallel->ReplaceTopologyLean( p, NormalsListType(), TexCoordsListType(), idx, true );
			const auto t2 = std::chrono::steady_clock::now();
			const std::vector<char> sa = Serialized( *serial ), sb = Serialized( *parallel );
			std::printf( "  %zu triangles, %u pool workers: serial %.1f ms, parallel %.1f ms, serialized %zu bytes, %s\n", n, workers,
				std::chrono::duration<double, std::milli>( t1 - t0 ).count(), std::chrono::duration<double, std::milli>( t2 - t1 ).count(),
				sa.size(), sa == sb ? "identical" : "DIFFERENT" );
			Check( a && b && !sa.empty() && sa == sb, "P parallel build serializes identically (nodes, prim order, bounds): " + std::to_string( n ) );
			// A rebuild in place over another list equals a fresh build.
			VerticesListType q;
			std::vector<std::uint32_t> jdx;
			SoupFrame( n / 2 + 17, 11, q, jdx );
			TriangleMeshGeometryIndexed* fresh = new TriangleMeshGeometryIndexed( false, true );
			Check( parallel->ReplaceTopologyLean( q, NormalsListType(), TexCoordsListType(), jdx, true ) &&
				fresh->ReplaceTopologyLean( q, NormalsListType(), TexCoordsListType(), jdx, false ) &&
				Serialized( *parallel ) == Serialized( *fresh ), "P an in-place rebuild equals a fresh build: " + std::to_string( n ) );
			// And the classic build (DoneIndexedTriangles) intersects identically.
			TriangleMeshGeometryIndexed* classic = new TriangleMeshGeometryIndexed( false, true );
			classic->BeginIndexedTriangles();
			classic->AddVertices( q );
			for( std::size_t t = 0; t < jdx.size(); t += 3 ) {
				IndexedTriangle tri;
				for( int k = 0; k < 3; ++k ) { tri.iVertices[k] = tri.iNormals[k] = jdx[t+k]; tri.iCoords[k] = 0; }
				classic->AddIndexedTriangle( tri );
			}
			classic->DoneIndexedTriangles();
			int mismatch = 0;
			for( int r = 0; r < 4000; ++r ) {
				const Ray ray( Point3( ( r % 64 + 0.5 ) / 64, ( r / 64 + 0.5 ) / 64, 2 ), Vector3( 0.01, -0.02, -1 ) );
				RayIntersectionGeometric x( ray, nullRasterizerState ), y( ray, nullRasterizerState );
				parallel->IntersectRay( x, true, true, false );
				classic->IntersectRay( y, true, true, false );
				if( x.bHit != y.bHit || ( x.bHit && std::memcmp( &x.range, &y.range, sizeof( Scalar ) ) != 0 ) ) ++mismatch;
			}
			Check( mismatch == 0, "P the rebuilt mesh hits like the classic build: " + std::to_string( n ), std::to_string( mismatch ) );
			classic->release(); fresh->release(); serial->release(); parallel->release();
		}
	}

	//////////////////////////////////////////////////////////////////
	void TestJob( const fs::path& scratch, const Fixture& fx )
	{
		std::printf( "J: a prepared animation through every frame\n" );
		const fs::path root = scratch / "topology";
		const std::string text = std::string( "RISE ASCII SCENE 7\n\nfilm\n{\n\twidth 8\n\theight 8\n}\n\n" ) +
			"orthographic_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 4\n\texposure 0.1\n}\n\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance white\n}\n\n"
			"lambertian_luminaire_material\n{\n\tname lum\n\texitance white\n\tscale 2\n\tmaterial none\n}\n\n"
			"simulated_mesh_geometry\n{\n\tname liquid\n\tcache " + root.string() + "\n\tdouble_sided TRUE\n}\n\n"
			"standard_object\n{\n\tname surface\n\tgeometry liquid\n\tmaterial lum\n}\n\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 2\n\toidn_denoise FALSE\n\tradiance_map white\n\tradiance_scale 0.5\n\tradiance_background TRUE\n}\n\n";
		const fs::path path = scratch / "topology.RISEscene";
		{ std::ofstream o( path ); o << text; }
		IJobPriv* job = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( path.string().c_str() ), "J scene loads" );
		if( !job ) return;
		SimulatedMeshGeometry* g = dynamic_cast<SimulatedMeshGeometry*>( job->GetGeometries()->GetItem( "liquid" ) );
		Check( g != nullptr, "J the geometry is a SimulatedMeshGeometry" );
		for( int k = 0; g && k < fx.count; ++k ) {
			const double t = fx.start + k * fx.dt;
			Check( job->RasterizeAnimation( t, t + 1.0, 1, false, false ) && g->InstalledFrame() == k,
				"J an emissive changing-topology frame renders: " + std::to_string( k ) );
		}
		safe_release( job );
	}

	//////////////////////////////////////////////////////////////////
	void TestViewport( const fs::path& scratch, const Fixture& fx )
	{
		std::printf( "V: viewport scrub through every frame\n" );
		const std::string text = std::string( "RISE ASCII SCENE 7\n\nfilm\n{\n\twidth 64\n\theight 36\n}\n\n" ) +
			"orthographic_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 4 2.25\n\texposure 0.05\n}\n\n"
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance white\n}\n\n"
			"simulated_mesh_geometry\n{\n\tname liquid\n\tcache " + ( scratch / "topology" ).string() + "\n\tdouble_sided TRUE\n}\n\n"
			"standard_object\n{\n\tname surface\n\tgeometry liquid\n\tmaterial m\n}\n\n"
			"pathtracing_pel_rasterizer\n{\n\tsamples 4\n\toidn_denoise FALSE\n\tradiance_map white\n\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n";
		const fs::path path = scratch / "viewport.RISEscene";
		{ std::ofstream o( path ); o << text; }
		IJobPriv* job = nullptr;
		IRasterizer* rasterizer = nullptr;
		IRayCaster* preview = nullptr;
		IRayCaster* polish = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job || !job->LoadAsciiSceneViaCst( path.string().c_str() ) ||
			!CreateInteractiveMaterialPreviewPipeline( &rasterizer, &preview, &polish ) ) {
			Check( false, "V scene and viewport pipeline" );
			safe_release( job );
			return;
		}
		SceneEditController* ctrl = new SceneEditController( *job, rasterizer );
		ctrl->Start();
		const auto waitPass = [&]( const std::uint64_t since ) {
			const auto t0 = std::chrono::steady_clock::now();
			while( ctrl->GetCompletedPassCount() <= since ) {
				if( std::chrono::steady_clock::now() - t0 > std::chrono::seconds( 20 ) ) return false;
				std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
			}
			return true;
		};
		Check( waitPass( 0 ), "V a first viewport pass" );
		SimulatedMeshGeometry* g = dynamic_cast<SimulatedMeshGeometry*>( job->GetGeometries()->GetItem( "liquid" ) );
		for( int k = 0; g && k < fx.count; ++k ) {
			const double t = fx.start + k * fx.dt;
			const std::uint64_t since = ctrl->GetCompletedPassCount();
			const bool scrubbed = ctrl->OnTimeScrubBegin() && ctrl->OnTimeScrub( t );
			ctrl->OnTimeScrubEnd();
			const MeshFrame f = fx.Frame( k );
			Check( scrubbed && waitPass( since ) && g->InstalledFrame() == k && g->TriangleCount() == f.indices.size() / 3,
				"V scrub to frame " + std::to_string( k ) + " installs its topology and the viewport renders it" );
		}
		ctrl->Stop();
		delete ctrl;
		rasterizer->release();
		if( preview ) preview->release();
		if( polish ) polish->release();
		safe_release( job );
	}

	//////////////////////////////////////////////////////////////////
	void TestMemory( const fs::path& scratch, const Fixture& fx )
	{
		std::printf( "M: memory over 1,000 frames\n" );
		std::string error;
		SimulatedMeshGeometry* g = SimulatedMeshGeometry::Create( ( scratch / "topology" ).string(), FaceOptions(), error );
		if( !g ) { Check( false, "M bind", error ); return; }
		RayIntersectionGeometric probe( Ray( Point3( 0.01, 0.02, 3 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
		auto play = [&]( const int frames ) {
			for( int i = 0; i < frames; ++i ) {
				const double t = fx.start + ( ( i * 3 ) % fx.count ) * fx.dt;	// 0, 3, 6, 2, 5, 1, 4, ...
				g->PrepareAssetForRender( RenderTimeSupport{ t, t, t }, error, nullptr );
				RayIntersectionGeometric ri( probe.ray, nullRasterizerState );
				g->IntersectRay( ri, true, true, false );
			}
		};
		play( 100 );	// warm-up: buffers reach their largest frame
		const double rss0 = ResidentMB();
		const std::size_t table0 = TriangleMeshGeometryIndexed::MailboxTableBytes();
		const unsigned long long rebuilds0 = g->TopologyRebuilds();
		play( 1000 );
		const double rss1 = ResidentMB();
		const std::size_t table1 = TriangleMeshGeometryIndexed::MailboxTableBytes();
		std::printf( "  changing-topology playback, 1,000 installs (%llu rebuilds): RSS %.1f -> %.1f MB, mailbox table %zu -> %zu bytes\n",
			g->TopologyRebuilds() - rebuilds0, rss0, rss1, table0, table1 );
		Check( g->TopologyRebuilds() - rebuilds0 >= 800, "M the playback rebuilt (almost) every frame" );
		Check( table1 == table0 && rss1 - rss0 < 8.0, "M playback in place: flat memory and mailbox table" );
		g->release();

		// 1,000 geometries created, intersected and destroyed (what a reload or a
		// per-frame geometry would do): the recycled ids keep the table bounded.
		const std::size_t entries0 = TriangleMeshGeometryIndexed::MailboxTableEntries();
		const double rssA = ResidentMB();
		VerticesListType p;
		std::vector<std::uint32_t> idx;
		SoupFrame( 5000, 3, p, idx );
		TriangleMeshGeometryIndexed* shared = new TriangleMeshGeometryIndexed( false, true );
		shared->ReplaceTopologyLean( p, NormalsListType(), TexCoordsListType(), idx, false );
		shared->release();
		for( int i = 0; i < 1000; ++i ) {
			// (5,000 triangles: a 20 kB stamp buffer each.)
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, true );
			m->ReplaceTopologyLean( p, NormalsListType(), TexCoordsListType(), idx, false );
			RayIntersectionGeometric ri( Ray( Point3( 0.5, 0.5, 3 ), Vector3( 0, 0, -1 ) ), nullRasterizerState );
			m->IntersectRay( ri, true, true, false );
			m->release();
		}
		const std::size_t entries1 = TriangleMeshGeometryIndexed::MailboxTableEntries();
		const double rssB = ResidentMB();
		std::printf( "  1,000 geometries of 5,000 triangles created and destroyed: mailbox entries %zu -> %zu, RSS %.1f -> %.1f MB"
			" (before the fix the table kept one entry and its stamps per geometry ever created)\n", entries0, entries1, rssA, rssB );
		Check( entries1 <= entries0 + 1 && rssB - rssA < 16.0, "M destroyed geometries' mailbox entries are reused (bounded table)" );
	}

	//////////////////////////////////////////////////////////////////
	void Bench( const bool extended )
	{
		std::printf( "bench (not a gate): per-frame rebuild vs refit, %u pool workers\n", GlobalThreadPool().NumWorkers() );
		std::vector<std::size_t> sizes = { 100000, 1000000 };
		if( extended ) sizes.push_back( 5000000 );
		for( const std::size_t n : sizes ) {
			VerticesListType p;
			std::vector<std::uint32_t> idx;
			SoupFrame( n, 5, p, idx );
			NormalsListType none;
			double serial = 1e30, parallel = 1e30, refit = 1e30;
			TriangleMeshGeometryIndexed* m = new TriangleMeshGeometryIndexed( false, true );
			const double rss0 = ResidentMB();
			for( int rep = 0; rep < 3; ++rep ) {
				auto t0 = std::chrono::steady_clock::now();
				m->ReplaceTopologyLean( p, none, TexCoordsListType(), idx, false );
				serial = std::min( serial, std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
				t0 = std::chrono::steady_clock::now();
				m->ReplaceTopologyLean( p, none, TexCoordsListType(), idx, true );
				parallel = std::min( parallel, std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
				t0 = std::chrono::steady_clock::now();
				m->UpdateVerticesLean( p, none, true );
				refit = std::min( refit, std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
			}
			std::printf( "  %8zu triangles (%zu vertices): rebuild serial %.0f ms, parallel %.0f ms (x%.1f), lean refit %.0f ms; RSS %.0f -> %.0f MB\n",
				n, p.size(), serial, parallel, serial / parallel, refit, rss0, ResidentMB() );
			m->release();
		}
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	bool bench = false, extended = false;
	for( int i = 1; i < argc; ++i ) {
		if( std::strcmp( argv[i], "--bench" ) == 0 ) bench = true;
		if( std::strcmp( argv[i], "--extended" ) == 0 ) extended = true;
	}
	std::printf( "SimulatedMeshTopologyTest (M7 prerequisites)\n" );
	const fs::path scratch = RISE::TestSupport::ScratchRunDir( "rise_simulated_mesh_topology_", "suite" );
	std::error_code ec;
	fs::create_directories( scratch, ec );
	if( bench ) {
		Bench( extended );
	} else {
		Fixture fx;
		Check( WriteCache( scratch / "topology", fx, 21 ), "write the changing-topology cache" );
		TestFrames( scratch / "topology", fx );
		TestKeys( scratch, fx );
		TestRollback( scratch, fx );
		TestParallelBuild();
		TestJob( scratch, fx );
		TestViewport( scratch, fx );
		TestMemory( scratch, fx );
	}
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_simulated_mesh_topology_" ), ec );
	std::printf( "\n%d passed, %d failed\n", passes, failures );
	return failures == 0 ? 0 : 1;
}
