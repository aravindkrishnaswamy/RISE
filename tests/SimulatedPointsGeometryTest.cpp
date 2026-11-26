//////////////////////////////////////////////////////////////////////
//
//  SimulatedPointsGeometryTest.cpp - M7 prerequisites:
//    simulated_points_geometry (Geometry/SimulatedPointsGeometry.h,
//    Simulation/SimulatedPointsSource.h).
//
//    Every check writes its own small points.v1 cache (no solver):
//      A  a non-blurred frame hits exactly like explicit SphereGeometry
//         spheres at the same centres and radii (range, point, normal, UV,
//         derivatives bit-equal; any-hit equal); a scene with the chunk
//         renders the same image as the same spheres as sphere objects
//      B  shutter keys from velocities: at the open / close sample time the
//         points hit like spheres at the key centres; without velocities the
//         keys come from the neighbours matched by id; without ids, none
//      C  radii: per point, uniform, the `radius` default, `radius_scale`;
//         the colour attribute reaches the hit's vertex colour
//      D  missing frames (hold / hide), predictive admission, end policy
//         error, an empty frame, rollback and the render freeze
//      E  through a Job: the chunk (cache form) loads and a prepared
//         animation installs the frame for its time; a mesh output is refused
//      F  blur row: a blurred render equals the time-average of static
//         renders (Welch t, as SimulatedMeshBlurTest)
//      V  the viewport scrub installs points frames
//
//    Usage: SimulatedPointsGeometryTest [--bench [--extended]] [--write-sample-cache <dir>]
//      --write-sample-cache writes the synthetic fountain that
//      scenes/Tests/Simulation/simulated_points_spray.RISEscene reads.
//      --bench (not a gate) times the 1M-point install + BVH build and the
//      intersection cost against the same spheres as separate objects.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/SceneEditor/SceneEditController.h"
#include "../src/Library/Rendering/InteractivePelRasterizer.h"
#include "../src/Library/Geometry/SimulatedPointsGeometry.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Simulation/SimulatedPointsSource.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "../src/Library/Simulation/SimulationHost.h"
#include "../src/Library/Simulation/SimulationPoints.h"
#include "../src/Library/Animation/TimeIndexedView.h"
#include "../src/Library/Rendering/TimeIndexedMotionBlur.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IRasterizer.h"
#include "../src/Library/Interfaces/IRasterizerOutput.h"
#include "../src/Library/Interfaces/IRasterImage.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/Color/Color_Template.h"
#include "../src/Library/Utilities/SobolSampler.h"
#include "../src/Library/Utilities/ThreadPool.h"
#include "TestTempDir.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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

	std::string Fmt( const char* f, ... )
	{
		char buf[4096];
		va_list ap;
		va_start( ap, f );
		std::vsnprintf( buf, sizeof( buf ), f, ap );
		va_end( ap );
		return buf;
	}

	constexpr std::size_t kBudget = std::size_t( 1024 ) * 1024 * 1024;	// encode / write budget (the 4M-point bench)
	std::uint32_t Bits( const double v ) { const float f = float( v ); std::uint32_t b; std::memcpy( &b, &f, 4 ); return b; }
	double Value( const std::uint32_t b ) { float f; std::memcpy( &f, &b, 4 ); return double( f ); }

	enum class RadiusKind { PerPoint, Uniform, None };

	//! N points on a jittered lattice moving with a smooth velocity field.
	//! Frame k is at time start + k * dt; ids 10 + 3i (every 5th point is
	//! missing from odd frames when `churn`).
	struct Fixture
	{
		int n = 40;
		double start = 0.0, dt = 0.25;
		int count = 5;
		bool velocities = true, ids = true, churn = false, color = true;
		double boost = 0;	//!< extra x velocity (the blur row moves points out of the frame)
		RadiusKind radius = RadiusKind::PerPoint;
		void Rest( const int i, double p[3] ) const
		{
			p[0] = -1.0 + 2.0 * ( ( i * 7 ) % n ) / n + 0.01 * ( i % 3 );
			p[1] = -0.7 + 1.4 * ( ( i * 13 ) % n ) / n;
			p[2] = 0.1 * std::sin( 1.7 * i );
		}
		void Velocity( const int i, double v[3] ) const
		{
			double p[3]; Rest( i, p );
			v[0] = 0.4 + 0.3 * p[1] + boost; v[1] = -0.2 * p[0]; v[2] = 0.15 * std::cos( 2.0 * p[0] );
		}
		PointsFrame Frame( const int k ) const
		{
			PointsFrame f;
			const double s = start + k * dt;
			if( velocities ) f.velocityUnits = "m/s";
			PointAttribute temperature{ "temperature", "K", 1, {} };
			for( int i = 0; i < n; ++i ) {
				if( churn && ( k % 2 ) == 1 && i % 5 == 0 ) continue;
				double p[3], v[3]; Rest( i, p ); Velocity( i, v );
				for( int c = 0; c < 3; ++c ) f.positions.push_back( Bits( p[c] + v[c] * s ) );
				if( velocities ) for( int c = 0; c < 3; ++c ) f.velocities.push_back( Bits( v[c] ) );
				if( radius == RadiusKind::PerPoint ) f.radii.push_back( Bits( 0.03 + 0.002 * ( i % 7 ) ) );
				if( ids ) f.ids.push_back( 10 + 3 * std::uint64_t( i ) );
				temperature.values.push_back( Bits( 300 + i ) );
			}
			if( radius == RadiusKind::Uniform ) { f.hasUniformRadius = true; f.uniformRadius = Bits( 0.04 ); }
			if( color ) f.attributes.push_back( temperature );
			return f;
		}
	};

	std::string Hex64( const unsigned salt ) { char b[65]; for( int i = 0; i < 64; ++i ) b[i] = "0123456789abcdef"[( salt * 7 + i * 13 ) & 15]; b[64] = 0; return b; }

	bool WriteCache( const fs::path& root, const Fixture& fx, const unsigned salt, const int written,
		const std::string& fidelity = "preview", const std::vector<std::pair<int, PointsFrame>>& overrides = {} )
	{
		std::error_code ec;
		fs::remove_all( root, ec );
		fs::create_directories( root.parent_path(), ec );
		CacheDescription d;
		d.key = Hex64( salt ); d.epoch = Hex64( salt + 1000 );
		d.producer = "m7-points-fixture"; d.producerVersion = "1";
		d.count = fx.count; d.startTime = fx.start; d.interval = fx.dt; d.hold = true;
		d.outputs = { { "particles", PointsCodec, { { "position", "m", "f32x3" }, { "radius", "m", "f32" } } } };
		d.fidelity = fidelity;
		if( fidelity == "predictive" ) d.evidence = { Hex64( salt + 77 ) };
		CacheWriter writer;
		std::string error;
		if( !writer.Begin( root, d, "", false, kBudget, error ) ) { std::printf( "  cache: %s\n", error.c_str() ); return false; }
		for( int k = 0; k < written; ++k ) {
			PointsFrame f = fx.Frame( k );
			for( const auto& o : overrides ) if( o.first == k ) f = o.second;
			Bytes bytes;
			if( !EncodePoints( f, kBudget, bytes, error ) || !writer.Append( k, "particles", fx.start + k * fx.dt, bytes, error ) ) {
				std::printf( "  cache %d: %s\n", k, error.c_str() );
				return false;
			}
		}
		if( written == fx.count && !writer.Seal( error ) ) { std::printf( "  seal: %s\n", error.c_str() ); return false; }
		return true;
	}

	SimulatedPointsGeometry* Bind( const fs::path& root, SimulatedPointsGeometry::Options o, std::string& error )
	{
		return SimulatedPointsGeometry::Create( root.string(), o, error );
	}

	std::vector<Ray> Rays()
	{
		std::vector<Ray> rays;
		for( int j = 0; j < 64; ++j ) for( int i = 0; i < 64; ++i ) {
			const Point3 o( -1.3 + 2.8 * ( i + 0.37 ) / 64, -1.0 + 2.0 * ( j + 0.61 ) / 64, 3.0 );
			rays.push_back( Ray( o, Vector3Ops::Normalize( Vector3( 0.02 * std::sin( 0.7 * i ), 0.01 * std::cos( 0.3 * j ), -1.0 ) ) ) );
		}
		return rays;
	}

	//! Explicit spheres: the closest hit over SphereGeometry instances, each
	//! intersecting the ray moved into its frame (what an object transform does).
	struct Spheres
	{
		std::vector<Point3> centre;
		std::vector<SphereGeometry*> geometry;
		~Spheres() { for( SphereGeometry* g : geometry ) g->release(); }
		void Add( const Point3& c, const Scalar r ) { centre.push_back( c ); geometry.push_back( new SphereGeometry( r ) ); }
		bool Hit( const Ray& ray, RayIntersectionGeometric& best ) const
		{
			bool any = false;
			for( std::size_t s = 0; s < centre.size(); ++s ) {
				const Point3& c = centre[s];
				RayIntersectionGeometric ri( Ray( Point3( ray.origin.x - c.x, ray.origin.y - c.y, ray.origin.z - c.z ), ray.Dir() ), nullRasterizerState );
				geometry[s]->IntersectRay( ri, true, true, false );
				if( ri.bHit && ri.range < best.range ) {
					any = true;
					best.bHit = true; best.range = ri.range;
					best.ptIntersection = Point3( ri.ptIntersection.x + c.x, ri.ptIntersection.y + c.y, ri.ptIntersection.z + c.z );
					best.vNormal = ri.vNormal; best.ptCoord = ri.ptCoord; best.derivatives = ri.derivatives;
				}
			}
			return any;
		}
		bool Any( const Ray& ray, const Scalar far ) const
		{
			for( std::size_t s = 0; s < centre.size(); ++s ) {
				const Point3& c = centre[s];
				if( geometry[s]->IntersectRay_IntersectionOnly( Ray( Point3( ray.origin.x - c.x, ray.origin.y - c.y, ray.origin.z - c.z ), ray.Dir() ), far, true, true ) ) return true;
			}
			return false;
		}
	};

	struct Compare { int hits = 0, hitMismatch = 0, notEqual = 0, anyMismatch = 0, fields = 0; double maxRange = 0; };

	bool Same( const Point3& a, const Point3& b ) { return a.x == b.x && a.y == b.y && a.z == b.z; }

	Compare CompareHits( const IGeometry& g, const Spheres& s )
	{
		Compare c;
		for( const Ray& ray : Rays() ) {
			RayIntersectionGeometric a( ray, nullRasterizerState ), b( ray, nullRasterizerState );
			g.IntersectRay( a, true, true, false );
			s.Hit( ray, b );
			if( a.bHit ) ++c.hits;
			if( g.IntersectRay_IntersectionOnly( ray, 100.0, true, true ) != s.Any( ray, 100.0 ) ) ++c.anyMismatch;
			if( a.bHit != b.bHit ) { ++c.hitMismatch; continue; }
			if( !a.bHit ) continue;
			c.maxRange = std::max( c.maxRange, std::fabs( a.range - b.range ) );
			// Range and point bit-equal; the normal, UV and derivatives to the last
			// bits (the library's fast-math build may contract the normalisation
			// differently in the two translation units).
			const auto near = []( const double x, const double y ) { return std::fabs( x - y ) <= 1e-13; };
			const auto nearV = [&]( const Vector3& x, const Vector3& y ) { return near( x.x, y.x ) && near( x.y, y.y ) && near( x.z, y.z ); };
			const int bad = ( a.range != b.range ) | ( !Same( a.ptIntersection, b.ptIntersection ) << 1 ) | ( !nearV( a.vNormal, b.vNormal ) << 2 ) |
				( ( !near( a.ptCoord.x, b.ptCoord.x ) || !near( a.ptCoord.y, b.ptCoord.y ) ) << 3 ) | ( !nearV( a.derivatives.dpdu, b.derivatives.dpdu ) << 4 ) |
				( !nearV( a.derivatives.dndv, b.derivatives.dndv ) << 5 );
			if( bad ) { ++c.notEqual; c.fields |= bad; }
		}
		return c;
	}

	std::string Describe( const Compare& c )
	{
		return Fmt( "%d hits, %d hit / %d any-hit mismatches, %d records not bit-equal (fields 0x%x, max range diff %.3g)",
			c.hits, c.hitMismatch, c.anyMismatch, c.notEqual, c.fields, c.maxRange );
	}

	void SpheresOf( const PointsFrame& f, const std::vector<double>& centres, const double defaultRadius, Spheres& s )
	{
		for( std::size_t i = 0; i < f.Count(); ++i ) {
			const double r = !f.radii.empty() ? Value( f.radii[i] ) : f.hasUniformRadius ? Value( f.uniformRadius ) : defaultRadius;
			s.Add( Point3( centres[3*i], centres[3*i+1], centres[3*i+2] ), r );
		}
	}

	std::vector<double> Doubles( const std::vector<std::uint32_t>& b ) { std::vector<double> d; for( auto x : b ) d.push_back( Value( x ) ); return d; }

	struct SampleTimeScope
	{
		TimeIndexed::SampleTime previous;
		explicit SampleTimeScope( const double t ) : previous( TimeIndexed::tlsSampleTime ) { TimeIndexed::tlsSampleTime = { t, true }; }
		~SampleTimeScope() { TimeIndexed::tlsSampleTime = previous; }
	};

	//////////////////////////////////////////////////////////////////
	void TestStatic( const fs::path& scratch )
	{
		std::printf( "A: non-blurred points == explicit spheres\n" );
		Fixture fx;
		const fs::path root = scratch / "a";
		Check( WriteCache( root, fx, 1, fx.count ), "A write cache" );
		std::string error;
		SimulatedPointsGeometry* g = Bind( root, SimulatedPointsGeometry::Options(), error );
		Check( g && g->PointCount() == std::size_t( fx.n ) && g->InstalledFrame() == 0, "A bind", error );
		if( !g ) return;
		for( const int k : { 0, 3 } ) {
			const double t = fx.start + k * fx.dt;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t, t, t }, error, nullptr ) && g->InstalledFrame() == k && !g->BlurActive(),
				"A install frame " + std::to_string( k ), error );
			const PointsFrame f = fx.Frame( k );
			Spheres s;
			SpheresOf( f, Doubles( f.positions ), 0.01, s );
			const Compare c = CompareHits( *g, s );
			std::printf( "  frame %d: %s\n", k, Describe( c ).c_str() );
			Check( c.hits > 100 && c.hitMismatch == 0 && c.notEqual == 0 && c.anyMismatch == 0,
				"A frame " + std::to_string( k ) + " hits like explicit spheres (range and point bit-equal)", Describe( c ) );
		}
		const unsigned long long installs = g->InstallCount();
		const double t3 = fx.start + 3 * fx.dt;
		Check( g->PrepareAssetForRender( RenderTimeSupport{ t3, t3, t3 }, error, nullptr ) && g->InstallCount() == installs,
			"A re-preparing the installed frame is a no-op" );
		g->release();
	}

	//////////////////////////////////////////////////////////////////
	void TestKeys( const fs::path& scratch )
	{
		std::printf( "B: shutter keys\n" );
		std::string error;
		for( const int mode : { 0, 1, 2 } ) {	// velocities, ids only, neither
			Fixture fx;
			fx.velocities = mode == 0;
			fx.ids = mode != 2;
			const fs::path root = scratch / ( "b" + std::to_string( mode ) );
			Check( WriteCache( root, fx, 10 + mode, fx.count ), "B write cache" );
			SimulatedPointsGeometry* g = Bind( root, SimulatedPointsGeometry::Options(), error );
			if( !g ) { Check( false, "B bind", error ); continue; }
			const double t2 = fx.start + 2 * fx.dt, E = 0.125;
			const RenderTimeSupport support{ t2, t2 - 0.5 * E, t2 + 0.5 * E };
			Check( g->PrepareAssetForRender( support, error, nullptr ) && g->InstalledFrame() == 2, "B install with a shutter", error );
			if( mode == 2 ) {
				Check( !g->BlurActive(), "B no velocities and no ids: no keys" );
				g->release();
				continue;
			}
			Check( g->BlurActive(), mode == 0 ? "B keys from velocities" : "B keys from neighbours matched by id" );
			const PointsFrame f = fx.Frame( 2 );
			const std::vector<double> p = Doubles( f.positions );
			std::vector<double> v( p.size() );
			for( int i = 0; i < fx.n; ++i ) {
				double vel[3]; fx.Velocity( i, vel );
				for( int c = 0; c < 3; ++c ) v[3*i+c] = mode == 0 ? Value( Bits( vel[c] ) ) : vel[c];
			}
			for( const double fraction : { 0.0, 0.5, 1.0 } ) {
				std::vector<double> key( p.size() );
				for( std::size_t i = 0; i < p.size(); ++i ) key[i] = p[i] + v[i] * ( fraction - 0.5 ) * E;
				Spheres s;
				SpheresOf( f, key, 0.01, s );
				SampleTimeScope scope( support.open + fraction * E );
				const Compare c = CompareHits( *g, s );
				std::printf( "  %s, fraction %.1f: %s\n", mode == 0 ? "velocities" : "ids", fraction, Describe( c ).c_str() );
				const double tol = mode == 0 ? 1e-12 : 1e-5;	// ids: differences of float positions
				Check( c.hits > 100 && c.hitMismatch <= 2 && c.maxRange < tol,
					"B blurred points at the sample time hit like spheres at that pose", Describe( c ) );
				if( mode == 0 && fraction != 0.5 )
					Check( c.notEqual == 0 && c.hitMismatch == 0, "B at a shutter end the hit records are bit-equal", Describe( c ) );
			}
			g->release();
		}
		// Churn: points appearing and vanishing; neighbours matched by id still work.
		Fixture churn; churn.velocities = false; churn.churn = true;
		const fs::path root = scratch / "b_churn";
		Check( WriteCache( root, churn, 15, churn.count ), "B write churn cache" );
		SimulatedPointsGeometry* g = Bind( root, SimulatedPointsGeometry::Options(), error );
		if( g ) {
			const double t1 = churn.start + churn.dt;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1 - 0.05, t1 + 0.05 }, error, nullptr ) &&
				g->PointCount() == std::size_t( churn.n - churn.n / 5 ) && g->BlurActive(), "B a frame with fewer points installs with keys", error );
			const double t2 = churn.start + 2 * churn.dt;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, nullptr ) && g->PointCount() == std::size_t( churn.n ),
				"B the count follows the frame", error );
			g->release();
		}
	}

	//////////////////////////////////////////////////////////////////
	void TestRadiiAndColour( const fs::path& scratch )
	{
		std::printf( "C: radii and the colour attribute\n" );
		std::string error;
		for( const RadiusKind kind : { RadiusKind::PerPoint, RadiusKind::Uniform, RadiusKind::None } ) {
			Fixture fx; fx.radius = kind;
			const fs::path root = scratch / ( "c" + std::to_string( int( kind ) ) );
			WriteCache( root, fx, 20 + int( kind ), fx.count );
			SimulatedPointsGeometry::Options o;
			o.source.radius = 0.05;
			o.source.radiusScale = 1.5;
			SimulatedPointsGeometry* g = Bind( root, o, error );
			if( !g ) { Check( false, "C bind", error ); continue; }
			const PointsFrame f = fx.Frame( 0 );
			const double expected = kind == RadiusKind::PerPoint ? Value( f.radii[6] ) * 1.5 : kind == RadiusKind::Uniform ? Value( f.uniformRadius ) * 1.5 : 0.05 * 1.5;
			Check( g->Radius( 6 ) == expected, "C radius: " + std::string( kind == RadiusKind::PerPoint ? "per point" : kind == RadiusKind::Uniform ? "uniform" : "the default" ) + " times the scale" );
			g->release();
		}
		Fixture fx;
		const fs::path root = scratch / "c_color";
		WriteCache( root, fx, 25, fx.count );
		SimulatedPointsGeometry::Options o;
		o.source.colorAttribute = "temperature";
		SimulatedPointsGeometry* g = Bind( root, o, error );
		Check( g != nullptr, "C bind with a colour attribute", error );
		if( g ) {
			int coloured = 0, correct = 0;
			for( const Ray& ray : Rays() ) {
				RayIntersectionGeometric ri( ray, nullRasterizerState );
				g->IntersectRay( ri, true, true, false );
				if( !ri.bHit ) continue;
				if( ri.bHasVertexColor ) ++coloured;
				// The hit point lies on one sphere; that point's attribute is 300 + i.
				for( std::size_t i = 0; i < g->PointCount(); ++i ) {
					const Vector3 d = Vector3Ops::mkVector3( ri.ptIntersection, g->Centre( i ) );
					if( std::fabs( Vector3Ops::Magnitude( d ) - g->Radius( i ) ) < 1e-9 && ri.vColor.r == 300.0 + double( i ) &&
						ri.vColor.b == 300.0 + double( i ) ) { ++correct; break; }
				}
			}
			Check( coloured > 100 && correct == coloured, "C every hit carries its point's attribute as vertex colour",
				Fmt( "%d coloured, %d correct", coloured, correct ) );
			g->release();
		}
		o.source.colorAttribute = "pressure";
		SimulatedPointsGeometry* missing = Bind( root, o, error );
		Check( missing == nullptr, "C a colour attribute the frame lacks is refused", error );
		if( missing ) missing->release();
	}

	//////////////////////////////////////////////////////////////////
	void TestPolicies( const fs::path& scratch )
	{
		std::printf( "D: missing frames, predictive, empty frame, rollback, freeze\n" );
		std::string error;
		Fixture fx;
		const fs::path partial = scratch / "d_partial";
		Check( WriteCache( partial, fx, 30, 3 ), "D write a partial cache" );
		SimulatedPointsGeometry::Options hold;
		SimulatedPointsGeometry* g = Bind( partial, hold, error );
		if( g ) {
			const double t4 = fx.start + 4 * fx.dt, t2 = fx.start + 2 * fx.dt;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, nullptr ) &&
				g->PrepareAssetForRender( RenderTimeSupport{ t4, t4, t4 }, error, nullptr ) && g->InstalledFrame() == 2 && !g->Hidden(),
				"D hold: a frame not baked yet keeps the installed frame", error );
			g->release();
		}
		SimulatedPointsGeometry::Options hide; hide.source.missing = MeshMissingPolicy::Hide;
		g = Bind( partial, hide, error );
		if( g ) {
			const double t4 = fx.start + 4 * fx.dt;
			std::function<void()> undo;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t4, t4, t4 }, error, &undo ) && g->Hidden(), "D hide: hidden", error );
			RayIntersectionGeometric ri( Rays()[2000], nullRasterizerState );
			g->IntersectRay( ri, true, true, false );
			Check( !ri.bHit, "D a hidden frame has no hits" );
			if( undo ) undo();
			Check( !g->Hidden(), "D the undo shows it again" );
			g->release();
		}
		SimulatedPointsGeometry::Options predictive; predictive.source.predictive = true;
		g = Bind( partial, predictive, error );
		Check( g == nullptr, "D predictive refuses a partial cache", error );
		if( g ) g->release();
		const fs::path full = scratch / "d_full";
		WriteCache( full, fx, 31, fx.count );
		g = Bind( full, predictive, error );
		Check( g == nullptr, "D predictive refuses a preview-labelled cache", error );
		if( g ) g->release();
		const fs::path labelled = scratch / "d_predictive";
		WriteCache( labelled, fx, 32, fx.count, "predictive" );
		g = Bind( labelled, predictive, error );
		Check( g != nullptr, "D predictive admits a complete predictive cache", error );
		if( g ) g->release();
		SimulatedPointsGeometry::Options strict; strict.source.end = MeshEndPolicy::Error;
		g = Bind( full, strict, error );
		if( g ) {
			Check( !g->PrepareAssetForRender( RenderTimeSupport{ 9.0, 9.0, 9.0 }, error, nullptr ) && g->InstalledFrame() == 0,
				"D end_policy error refuses a time outside the support; the frame stays" );
			g->release();
		}
		// An empty frame in the middle, and a rollback across it.
		const fs::path empty = scratch / "d_empty";
		WriteCache( empty, fx, 33, fx.count, "preview", { { 2, PointsFrame() } } );
		g = Bind( empty, SimulatedPointsGeometry::Options(), error );
		if( g ) {
			const double t1 = fx.start + fx.dt, t2 = fx.start + 2 * fx.dt;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t1, t1, t1 }, error, nullptr ), "D frame 1", error );
			std::function<void()> undo;
			Check( g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, &undo ) && g->PointCount() == 0 && g->GetArea() == 0,
				"D an empty frame installs no points", error );
			int hits = 0;
			for( const Ray& ray : Rays() ) { RayIntersectionGeometric ri( ray, nullRasterizerState ); g->IntersectRay( ri, true, true, false ); hits += ri.bHit; }
			const BoundingBox box = g->GenerateBoundingBox();
			Check( hits == 0 && std::isfinite( box.ll.x ) && std::isfinite( box.ur.x ), "D the empty frame: no hits, finite bounds" );
			if( undo ) undo();
			const PointsFrame f1 = fx.Frame( 1 );
			Spheres s;
			SpheresOf( f1, Doubles( f1.positions ), 0.01, s );
			const Compare c = CompareHits( *g, s );
			Check( g->InstalledFrame() == 1 && c.notEqual == 0 && c.hitMismatch == 0 && c.hits > 100, "D the undo restores frame 1 exactly", Describe( c ) );
			// Freeze: a prepare under the freeze is refused; release confirms.
			Check( g->AcquireAssetFreeze( error ), "D acquire the freeze" );
			Check( !g->PrepareAssetForRender( RenderTimeSupport{ t2, t2, t2 }, error, nullptr ) && error == "mutation_frozen", "D a prepare under the freeze is refused" );
			Check( g->ReleaseAssetFreeze( error ), "D release reports an unchanged frame", error );
			g->release();
		}
	}

	//////////////////////////////////////////////////////////////////
	class Capture : public virtual IRasterizerOutput, public virtual Reference
	{
	public:
		std::vector<RISEColor> pixels;
	protected:
		virtual ~Capture() {}
	public:
		void OutputIntermediateImage( const IRasterImage&, const Rect* ) override {}
		void OutputImage( const IRasterImage& img, const Rect*, const unsigned int ) override
		{
			pixels.resize( img.GetWidth() * img.GetHeight() );
			for( unsigned int y = 0; y < img.GetHeight(); y++ ) for( unsigned int x = 0; x < img.GetWidth(); x++ ) pixels[y * img.GetWidth() + x] = img.GetPEL( x, y );
		}
	};

	unsigned int g_render = 0;
	//! Renders one animation frame at `t`; mean over pixels (and the pixels).
	bool Render( const fs::path& dir, const std::string& text, const double t, double& mean, std::vector<RISEColor>* pixels = nullptr )
	{
		const fs::path path = dir / "points.RISEscene";
		{ std::ofstream o( path ); o << text; }
		IJobPriv* job = nullptr;
		bool ok = false;
		if( RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( path.string().c_str() ) ) {
			job->RemoveRasterizerOutputs();
			Capture* cap = new Capture();
			job->GetRasterizer()->AddRasterizerOutput( cap );
			SobolSamplerTestHooks::ValueSalt().store( SobolSequence::HashCombine( 4711u + g_render, 0x3b3u ) );
			std::srand( 4711u + g_render++ );
			ok = job->RasterizeAnimation( t, t + 1.0, 1, false, false ) && !cap->pixels.empty();
			SobolSamplerTestHooks::ValueSalt().store( 0u );
			mean = 0;
			for( const RISEColor& c : cap->pixels ) mean += ( c.base.r + c.base.g + c.base.b ) * c.a / 3.0;
			if( !cap->pixels.empty() ) mean /= double( cap->pixels.size() );
			if( pixels ) *pixels = cap->pixels;
			cap->release();
		}
		safe_release( job );
		return ok;
	}

	std::string SceneHead( const double exposure, const int spp, const int size )
	{
		return Fmt( "RISE ASCII SCENE 7\n\nfilm\n{\n\twidth %d\n\theight %d\n}\n\n", size, size ) +
			Fmt( "orthographic_camera\n{\n\tlocation 0 0 3\n\tlookat 0 0 0\n\tup 0 1 0\n\tviewport_scale 3 3\n\texposure %g\n}\n\n", exposure ) +
			"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
			"uniformcolor_painter\n{\n\tname white\n\tcolor 1 1 1\n}\n\n"
			"uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n}\n\n"
			"lambertian_material\n{\n\tname m\n\treflectance black\n}\n\n" +
			Fmt( "pathtracing_pel_rasterizer\n{\n\tsamples %d\n\toidn_denoise FALSE\n\tpixel_filter box\n\tradiance_map white\n\tradiance_scale 1.0\n\tradiance_background TRUE\n}\n\n", spp );
	}

	std::string PointsBody( const fs::path& cache, const std::string& extra = "" )
	{
		return "simulated_points_geometry\n{\n\tname spray\n\tcache " + cache.string() + "\n" + extra + "}\n\n"
			"standard_object\n{\n\tname drops\n\tgeometry spray\n\tmaterial m\n}\n\n";
	}

	void TestJob( const fs::path& scratch )
	{
		std::printf( "E: the chunk through a Job; scene equivalence with sphere objects\n" );
		Fixture fx;
		const fs::path root = scratch / "e";
		WriteCache( root, fx, 40, fx.count );
		const fs::path path = scratch / "e.RISEscene";
		{ std::ofstream o( path ); o << SceneHead( 0.0, 2, 8 ) + PointsBody( root ); }
		IJobPriv* job = nullptr;
		Check( RISE_CreateJobPriv( &job ) && job && job->LoadAsciiSceneViaCst( path.string().c_str() ), "E scene with simulated_points_geometry loads" );
		if( job ) {
			SimulatedPointsGeometry* g = dynamic_cast<SimulatedPointsGeometry*>( job->GetGeometries()->GetItem( "spray" ) );
			Check( g != nullptr, "E the geometry is a SimulatedPointsGeometry" );
			const double t3 = fx.start + 3 * fx.dt;
			Check( g && job->RasterizeAnimation( t3, t3 + 1.0, 1, false, false ) && g->InstalledFrame() == 3, "E a prepared animation render installs frame 3" );
			std::vector<Simulation::TimelineRange> ranges;
			Check( job->DescribeTimeVaryingAssets( ranges, true ) && ranges.size() == 1 && ranges[0].kind == Simulation::TimelineRange::SimulatedPoints &&
				ranges[0].framesAvailable == unsigned( fx.count ), "E the timeline lists the points' frames" );
			safe_release( job );
		}
		// Scene-level equivalence at frame 3: the same spheres as sphere objects.
		const PointsFrame f = fx.Frame( 3 );
		std::string spheres;
		for( std::size_t i = 0; i < f.Count(); ++i ) {
			spheres += Fmt( "sphere_geometry\n{\n\tname s%zu\n\tradius %.17g\n}\n\n", i, Value( f.radii[i] ) );
			spheres += Fmt( "standard_object\n{\n\tname o%zu\n\tgeometry s%zu\n\tmaterial m\n\tposition %.17g %.17g %.17g\n}\n\n", i, i,
				Value( f.positions[3*i] ), Value( f.positions[3*i+1] ), Value( f.positions[3*i+2] ) );
		}
		double a = 0, b = 0;
		std::vector<RISEColor> pa, pb;
		const double t3 = fx.start + 3 * fx.dt;
		const bool ra = Render( scratch, SceneHead( 0.0, 8, 32 ) + PointsBody( root ), t3, a, &pa );
		g_render = 0;
		const bool rb = Render( scratch, SceneHead( 0.0, 8, 32 ) + spheres, t3, b, &pb );
		g_render = 0;
		int differ = 0;
		for( std::size_t i = 0; i < pa.size() && i < pb.size(); ++i ) differ += pa[i].base.r != pb[i].base.r || pa[i].a != pb[i].a;
		std::printf( "  points %.6f vs sphere objects %.6f, %d of %zu pixels differ\n", a, b, differ, pa.size() );
		Check( ra && rb && pa.size() == pb.size() && std::fabs( a - b ) <= 0.02 * b, "E the points render like the same sphere objects" );
		// The sample scene loads whether or not its cache exists.
		{
			IJobPriv* sample = nullptr;
			const char* scene = "scenes/Tests/Simulation/simulated_points_spray.RISEscene";
			if( fs::exists( scene ) )
				Check( RISE_CreateJobPriv( &sample ) && sample && sample->LoadAsciiSceneViaCst( scene ), "E the sample scene loads" );
			safe_release( sample );
		}
		// A mesh output is refused.
		const fs::path meshScene = scratch / "e_mesh.RISEscene";
		{ std::ofstream o( meshScene ); o << SceneHead( 0.0, 2, 8 ) + "simulated_points_geometry\n{\n\tname spray\n\tcache " + root.string() + "\n\toutput surface\n}\n\n"; }
		IJobPriv* refused = nullptr;
		Check( RISE_CreateJobPriv( &refused ) && refused && !refused->LoadAsciiSceneViaCst( meshScene.string().c_str() ), "E an output that is not points.v1 fails the load" );
		safe_release( refused );
	}

	//////////////////////////////////////////////////////////////////
	void TestBlurRow( const fs::path& scratch )
	{
		std::printf( "F: blurred render vs the time-average of statics (PT)\n" );
		// Pairs of points that meet at the frame time t = 0.5 and separate
		// either side of it: the coverage is lowest at mid-shutter, so the
		// blurred mean is NOT the mid-shutter pose's (a shutter fraction
		// frozen at 0.5 fails this row).
		Fixture fx;
		fx.dt = 2.0; fx.start = -1.5; fx.count = 3;	// frames at -1.5, 0.5, 2.5: the shutter [0, 1] reads frame 1
		std::vector<std::pair<int, PointsFrame>> frames;
		for( int k = 0; k < fx.count; ++k ) {
			const double t = fx.start + k * fx.dt - 0.5;
			PointsFrame f;
			f.velocityUnits = "m/s";
			f.hasUniformRadius = true;
			f.uniformRadius = Bits( 0.07 );
			for( int j = 0; j < 5; ++j ) for( int i = 0; i < 5; ++i ) for( const double side : { -1.0, 1.0 } ) {
				const double cx = -1.0 + 0.5 * i, cy = -1.0 + 0.5 * j, v = 0.3 * side;
				for( const double x : { cx + v * t, cy, 0.0 } ) f.positions.push_back( Bits( x ) );
				for( const double x : { v, 0.0, 0.0 } ) f.velocities.push_back( Bits( x ) );
			}
			frames.push_back( { k, f } );
		}
		fx.n = 1;
		const fs::path root = scratch / "f";
		WriteCache( root, fx, 50, fx.count, "preview", frames );
		const int repeats = 8, times = 8, statics = 3;
		std::vector<double> blur;
		const unsigned long long frames0 = TimeIndexedFrame::CreatedCount();
		for( int i = 0; i < repeats; ++i ) { double m = 0; Render( scratch, SceneHead( 1.0, 16, 16 ) + PointsBody( root ), 0.5, m ); blur.push_back( m ); }
		const unsigned long long timeIndexed = TimeIndexedFrame::CreatedCount() - frames0;
		Check( timeIndexed == unsigned( repeats ), "F every blurred frame took the multi-threaded time-indexed path" );
		double bm = 0; for( double x : blur ) bm += x; bm /= repeats;
		double bv = 0; for( double x : blur ) bv += ( x - bm ) * ( x - bm ); bv /= ( repeats - 1 ) * repeats;
		double rm = 0, rv = 0;
		for( int k = 0; k < times; ++k ) {
			const double t = 0.0 + ( k + 0.5 ) / times;
			std::vector<double> v;
			for( int i = 0; i < statics; ++i ) { double m = 0; Render( scratch, SceneHead( 0.0, 16, 16 ) + PointsBody( root ), t, m ); v.push_back( m ); }
			double sm = 0; for( double x : v ) sm += x; sm /= statics;
			double sv = 0; for( double x : v ) sv += ( x - sm ) * ( x - sm ); sv /= ( statics - 1 ) * statics;
			rm += sm / times;
			rv += sv / ( double( times ) * times );
		}
		double m0 = 0, m1 = 0;
		Render( scratch, SceneHead( 0.0, 16, 16 ) + PointsBody( root ), 0.0, m0 );
		Render( scratch, SceneHead( 0.0, 16, 16 ) + PointsBody( root ), 1.0, m1 );
		const double se = std::sqrt( bv + rv );
		const double z = se > 0 ? ( bm - rm ) / se : 0;
		std::printf( "  blurred %.6f +/- %.6f, time-average %.6f +/- %.6f, t %+.2f; statics at open %.6f, close %.6f\n",
			bm, std::sqrt( bv ), rm, std::sqrt( rv ), z, m0, m1 );
		// t critical ~4.5 at alpha 0.01 for these degrees of freedom (as the mesh rows).
		double mid = 0;
		Render( scratch, SceneHead( 0.0, 16, 16 ) + PointsBody( root ), 0.5, mid );
		std::printf( "  static at mid-shutter %.6f (the blurred mean differs from it by %.1f standard errors)\n", mid, std::fabs( bm - mid ) / se );
		// Red-proof (2026-11-25): the shutter fraction frozen at 0.5 gives t = -6.0.
		Check( std::fabs( z ) <= 4.5 && std::fabs( m0 - m1 ) > 6 * se, "F blurred points = time average of static renders (and the statics move)" );
	}

	//////////////////////////////////////////////////////////////////
	void TestViewport( const fs::path& scratch )
	{
		std::printf( "V: viewport scrub\n" );
		Fixture fx;
		const fs::path root = scratch / "v";
		WriteCache( root, fx, 60, fx.count );
		const fs::path path = scratch / "v.RISEscene";
		{ std::ofstream o( path ); o << SceneHead( 0.0, 4, 48 ) + PointsBody( root ); }
		IJobPriv* job = nullptr;
		IRasterizer* rasterizer = nullptr;
		IRayCaster* preview = nullptr;
		IRayCaster* polish = nullptr;
		if( !RISE_CreateJobPriv( &job ) || !job || !job->LoadAsciiSceneViaCst( path.string().c_str() ) ||
			!CreateInteractiveMaterialPreviewPipeline( &rasterizer, &preview, &polish ) ) {
			Check( false, "V scene and viewport" );
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
		Check( waitPass( 0 ), "V a first pass" );
		bool has = false;
		Check( ctrl->GetHasAnimation( has ) && has, "V the timeline sees the points as animation" );
		SimulatedPointsGeometry* g = dynamic_cast<SimulatedPointsGeometry*>( job->GetGeometries()->GetItem( "spray" ) );
		for( int k = 0; g && k < fx.count; ++k ) {
			const std::uint64_t since = ctrl->GetCompletedPassCount();
			const bool ok = ctrl->OnTimeScrubBegin() && ctrl->OnTimeScrub( fx.start + k * fx.dt );
			ctrl->OnTimeScrubEnd();
			Check( ok && waitPass( since ) && g->InstalledFrame() == k, "V scrub to frame " + std::to_string( k ) );
		}
		ctrl->Stop();
		delete ctrl;
		rasterizer->release();
		if( preview ) preview->release();
		if( polish ) polish->release();
		safe_release( job );
	}

	//////////////////////////////////////////////////////////////////
	//! The sample scene's cache: 2,000 particles launched over the first
	//! second from the origin, ballistic, 25 frames at 1/24 s.
	int WriteSampleCache( const fs::path& root )
	{
		CacheDescription d;
		d.producer = "m7-sample-fountain"; d.producerVersion = "1";
		d.count = 25; d.interval = 1.0 / 24.0; d.hold = true;
		d.outputs = { { "particles", PointsCodec, { { "position", "m", "f32x3" }, { "velocity", "m/s", "f32x3" }, { "radius", "m", "f32" } } } };
		std::string key, error;
		MakeCacheKey( d.producer, "1", RISE::RISECBOR64::Value::MapValue( {} ), {}, key, error );
		d.key = key; d.epoch = DeriveEpoch( key, "" );
		CacheWriter writer;
		if( !writer.Begin( root, d, "", false, kBudget, error ) ) { std::printf( "write: %s\n", error.c_str() ); return 1; }
		const int n = 2000;
		for( int k = 0; k < d.count; ++k ) {
			const double t = k * d.interval;
			PointsFrame f;
			f.velocityUnits = "m/s";
			PointAttribute tint{ "tint", "1", 3, {} };
			for( int i = 0; i < n; ++i ) {
				const double launch = double( i ) / n, age = t - launch;
				if( age < 0 ) continue;
				const double a = 2.399963 * i, spread = 0.25 + 0.15 * std::sin( 0.37 * i );
				const double v0[3] = { spread * std::cos( a ), 2.6 + 0.3 * std::cos( 1.3 * i ), spread * std::sin( a ) };
				const double y = v0[1] * age - 4.905 * age * age;
				if( y < 0 ) continue;
				for( const double x : { v0[0] * age, y, v0[2] * age } ) f.positions.push_back( Bits( x ) );
				for( const double x : { v0[0], v0[1] - 9.81 * age, v0[2] } ) f.velocities.push_back( Bits( x ) );
				f.radii.push_back( Bits( 0.006 + 0.004 * ( i % 3 ) ) );
				f.ids.push_back( std::uint64_t( i ) );
				const double h = std::min( 1.0, y / 0.6 );
				for( const double c : { 0.2 + 0.8 * h, 0.5 + 0.5 * h, 1.0 } ) tint.values.push_back( Bits( c ) );
			}
			if( f.positions.empty() ) f.velocityUnits.clear();
			f.attributes.push_back( tint );
			Bytes bytes;
			if( !EncodePoints( f, kBudget, bytes, error ) || !writer.Append( k, "particles", t, bytes, error ) ) { std::printf( "write: %s\n", error.c_str() ); return 1; }
		}
		if( !writer.Seal( error ) ) { std::printf( "write: %s\n", error.c_str() ); return 1; }
		std::printf( "wrote %s (25 frames)\n", root.string().c_str() );
		return 0;
	}

	//////////////////////////////////////////////////////////////////
	void Bench( const fs::path& scratch, const bool extended )
	{
		std::printf( "bench (not a gate), %u pool workers\n", GlobalThreadPool().NumWorkers() );
		std::vector<int> sizes = { 100000, 1000000 };
		if( extended ) sizes.push_back( 4000000 );
		for( const int n : sizes ) {
			Fixture fx; fx.n = n; fx.count = 2; fx.color = false;
			const fs::path root = scratch / ( "bench" + std::to_string( n ) );
			WriteCache( root, fx, 70, fx.count );
			std::string error;
			auto t0 = std::chrono::steady_clock::now();
			SimulatedPointsGeometry* g = Bind( root, SimulatedPointsGeometry::Options(), error );
			const double bind = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
			if( !g ) { std::printf( "  bind failed: %s\n", error.c_str() ); continue; }
			double install = 1e30, blurred = 1e30;
			for( int rep = 0; rep < 3; ++rep ) {
				const double t = fx.start + ( ( rep + 1 ) % 2 ) * fx.dt;	// never the bind frame's support twice
				t0 = std::chrono::steady_clock::now();
				g->PrepareAssetForRender( RenderTimeSupport{ t, t, t }, error, nullptr );
				install = std::min( install, std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
				t0 = std::chrono::steady_clock::now();
				g->PrepareAssetForRender( RenderTimeSupport{ t, t - 0.01, t + 0.01 }, error, nullptr );
				blurred = std::min( blurred, std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count() );
			}
			std::printf( "  %8d points: bind %.0f ms, per-frame install (read, verify, decode, BVH build) %.0f ms, with shutter keys %.0f ms\n",
				n, bind, install, blurred );
			g->release();
		}
		// Intersection cost: the points geometry vs the same spheres as objects.
		for( const int n : { 1000, 10000 } ) {
			Fixture fx; fx.n = n; fx.count = 2; fx.color = false;
			const fs::path root = scratch / ( "ray" + std::to_string( n ) );
			WriteCache( root, fx, 80, fx.count );
			const PointsFrame f = fx.Frame( 0 );
			std::string spheres;
			for( std::size_t i = 0; i < f.Count(); ++i ) {
				spheres += Fmt( "standard_object\n{\n\tname o%zu\n\tgeometry s%d\n\tmaterial m\n\tposition %.17g %.17g %.17g\n}\n\n", i, int( i % 7 ),
					Value( f.positions[3*i] ), Value( f.positions[3*i+1] ), Value( f.positions[3*i+2] ) );
			}
			for( int r = 0; r < 7; ++r ) spheres = Fmt( "sphere_geometry\n{\n\tname s%d\n\tradius %.17g\n}\n\n", r, double( float( 0.03 + 0.002 * r ) ) ) + spheres;
			double ns[2] = { 0, 0 };
			for( int which = 0; which < 2; ++which ) {
				const fs::path path = scratch / "ray.RISEscene";
				{ std::ofstream o( path ); o << SceneHead( 0.0, 1, 8 ) + ( which == 0 ? PointsBody( root ) : spheres ); }
				IJobPriv* job = nullptr;
				if( !RISE_CreateJobPriv( &job ) || !job || !job->LoadAsciiSceneViaCst( path.string().c_str() ) ) { safe_release( job ); continue; }
				job->Rasterize();	// prepares the scene (and the TLAS)
				const IObjectManager* objects = job->GetScene()->GetObjects();
				std::vector<double> runs;
				int hits = 0;
				for( int rep = 0; rep < 5; ++rep ) {
					hits = 0;
					const auto t0 = std::chrono::steady_clock::now();
					for( int k = 0; k < 200000; ++k ) {
						const Ray ray( Point3( -1.4 + 2.8 * ( k % 447 ) / 447.0, -1.0 + 2.0 * ( k / 447 ) / 448.0, 3 ), Vector3( 0, 0, -1 ) );
						RayIntersection ri( ray, nullRasterizerState );
						objects->IntersectRay( ri, true, true, false );
						hits += ri.geometric.bHit;
					}
					runs.push_back( std::chrono::duration<double, std::nano>( std::chrono::steady_clock::now() - t0 ).count() / 200000 );
				}
				std::sort( runs.begin(), runs.end() );
				ns[which] = runs[2];
				std::printf( "  %6d spheres as %s: %.0f ns/ray closest hit (%d hits of 200k)\n", n, which == 0 ? "one simulated_points_geometry" : "sphere objects", runs[2], hits );
				safe_release( job );
			}
		}
	}
}

int main( int argc, char** argv )
{
	std::setvbuf( stdout, nullptr, _IONBF, 0 );
	bool bench = false, extended = false;
	for( int i = 1; i < argc; ++i ) {
		if( std::strcmp( argv[i], "--write-sample-cache" ) == 0 && i + 1 < argc ) return WriteSampleCache( argv[i + 1] );
		if( std::strcmp( argv[i], "--bench" ) == 0 ) bench = true;
		if( std::strcmp( argv[i], "--extended" ) == 0 ) extended = true;
	}
	std::printf( "SimulatedPointsGeometryTest (M7 prerequisites)\n" );
	const fs::path scratch = RISE::TestSupport::ScratchRunDir( "rise_simulated_points_", "suite" );
	std::error_code ec;
	fs::create_directories( scratch, ec );
	if( bench ) {
		Bench( scratch, extended );
	} else {
		TestStatic( scratch );
		TestKeys( scratch );
		TestRadiiAndColour( scratch );
		TestPolicies( scratch );
		TestJob( scratch );
		TestBlurRow( scratch );
		TestViewport( scratch );
	}
	fs::remove_all( RISE::TestSupport::ScratchTestRoot( "rise_simulated_points_" ), ec );
	std::printf( "\n%d passed, %d failed\n", passes, failures );
	return failures == 0 ? 0 : 1;
}
