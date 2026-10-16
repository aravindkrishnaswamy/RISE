// DL-382 / DL-620 certificate cost diagnostic.
//
// Build: make -C build/make/rise build-tool/OpenSheetCertificateTiming
// Run:   ./bin/tools/OpenSheetCertificateTiming [N=700] [reps=5] [fixture]
//
// Each fixture is an (N+1)^2-vertex grid, about 2 N^2 triangles (980,000 at
// N = 700).  `fixture` (default: all) is one of
//   planar     flat open sheet (planar certificate)
//   curved     open graph sheet z = 0.2 (x^2 + y^2) (projected-graph proof)
//   closedish  UV sphere with coincident pole rows (refused: exact
//              coincident corners -- the retained refusal pair)
//   closed     UV sphere with the pole fans removed and capped by welding
//              (closed body: watertight, open certificate refuses)
//   fold       figure-eight ribbon that passes through itself (open, not a
//              graph, refused by the Step-4 embedding test)
//   dense      the curved sheet plus ONE unused vertex far enough away that
//              the all-vertex weld epsilon is 1/1.2 of the grid spacing: the
//              DL-150 regime (dense model, one stray far vertex), where
//              every grid vertex is a separate representative barely more
//              than eps from its neighbours.
//   twodensity flat sheet whose last N/9 columns are 4000x wider than the
//              rest, sharing the seam column (a terrain LOD seam): about
//              1/9 of the triangles are Step-4 "oversize" (DL-623 review;
//              at most ~1/8 can be, since a triangle is oversize only
//              when the largest extent exceeds 8x the mean): the direct
//              certificate row shows the oversize sweep's bounded refusal.
// Every fixture also reports GeometricUtilities::WeldVertexPositions alone
// on its vertex list (weld), and GeometricUtilities::IsEmbeddedOpenOrientedSurface
// called directly on its corners (cert; the mesh may decide by a cheaper
// planar or graph proof and never reach it).
// and every row reports the mean and sample standard deviation over `reps`
// fresh meshes of the WHOLE call: DoneIndexedTriangles (build), an
// UpdateVertices with identical positions (same), and two successive
// UpdateVertices calls that move every vertex (def1: z += 0.03 (x^2+y^2);
// def2: z += 0.01 y; the far vertex of `dense` is not moved) -- the second
// shows the steady state once any one-time projection cost of the first
// has been paid.  Only the UpdateVertices call itself is timed, not the
// vertex edits.  The process's peak
// RSS is printed last; run one fixture per process to attribute it.  The
// open-certificate audit counter shows which updates re-ran the full proof.
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Utilities/GeometricUtilities.h"
#include <sys/resource.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
using namespace RISE; using namespace RISE::Implementation;

namespace
{
	struct Stat { std::vector<double> v; void add( double x ) { v.push_back( x ); }
		double mean() const { double s = 0; for( double x : v ) s += x; return v.empty() ? 0 : s / v.size(); }
		double sd() const { if( v.size() < 2 ) return 0; const double m = mean(); double s = 0; for( double x : v ) s += ( x - m ) * ( x - m ); return std::sqrt( s / ( v.size() - 1 ) ); } };

	void BuildFixture( const std::string& kind, const int N, VerticesListType& v, NormalsListType& n,
		TexCoordsListType& c, IndexTriangleListType& t )
	{
		const double pi = 3.14159265358979323846;
		const bool sphere = kind == "closedish" || kind == "closed";
		for( int j = 0; j <= N; ++j ) for( int i = 0; i <= N; ++i ) {
			const double x = -1 + 2.0 * i / N, y = -1 + 2.0 * j / N;
			if( sphere ) {
				const double th = pi * j / N, ph = 2 * pi * i / N;
				v.push_back( Point3( std::sin( th ) * std::cos( ph ), std::sin( th ) * std::sin( ph ), std::cos( th ) ) );
			} else if( kind == "fold" ) {
				// x = sin s, z = sin(2s)/2 over s in [-0.3, pi + 0.3]: the curve
				// passes through the origin at s = 0 and s = pi.
				const double s = -0.3 + ( pi + 0.6 ) * i / N;
				v.push_back( Point3( std::sin( s ), y, 0.5 * std::sin( 2 * s ) ) );
			} else if( kind == "twodensity" ) {
				const int h = N - N / 9;
				const double xi = i <= h ? double( i ) : double( h ) + 4000.0 * double( i - h );
				v.push_back( Point3( -1 + 2.0 * xi / N, y, 0 ) );
			} else {
				v.push_back( Point3( x, y, kind == "planar" ? 0 : 0.2 * ( x * x + y * y ) ) );
			}
			n.push_back( Vector3( 0, 0, 1 ) ); c.push_back( Point2( 0, 0 ) );
		}
		if( kind == "dense" ) {
			// eps = 1e-6 x diagonal ~ 1e-6 x D; make it the grid spacing / 1.2.
			const double D = ( 2.0 / N ) / 1.2e-6;
			v.push_back( Point3( -1 + D, -1, 0 ) );
			n.push_back( Vector3( 0, 0, 1 ) ); c.push_back( Point2( 0, 0 ) );
		}
		auto id = [&]( int i, int j ) { return (unsigned)( j * ( N + 1 ) + i ); };
		for( int j = 0; j < N; ++j ) for( int i = 0; i < N; ++i ) {
			IndexedTriangle a; a.iVertices[0] = a.iNormals[0] = a.iCoords[0] = id( i, j ); a.iVertices[1] = a.iNormals[1] = a.iCoords[1] = id( i + 1, j ); a.iVertices[2] = a.iNormals[2] = a.iCoords[2] = id( i + 1, j + 1 );
			if( kind != "closed" || j != 0 ) t.push_back( a );
			IndexedTriangle b; b.iVertices[0] = b.iNormals[0] = b.iCoords[0] = id( i, j ); b.iVertices[1] = b.iNormals[1] = b.iCoords[1] = id( i + 1, j + 1 ); b.iVertices[2] = b.iNormals[2] = b.iCoords[2] = id( i, j + 1 );
			if( kind != "closed" || j != N - 1 ) t.push_back( b );
		}
	}

	double Ms( std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b )
	{
		return std::chrono::duration<double, std::milli>( b - a ).count();
	}
}

int main( int argc, char** argv )
{
	GlobalLogPriv()->RemoveAllPrinters();
	const int N = argc > 1 ? std::atoi( argv[1] ) : 700;
	const int reps = argc > 2 ? std::atoi( argv[2] ) : 5;
	const std::string only = argc > 3 ? argv[3] : "all";
	const char* kinds[] = { "planar", "curved", "closedish", "closed", "fold", "dense", "twodensity" };
	for( const char* kind : kinds ) {
		if( only != "all" && only != kind ) continue;
		VerticesListType v0; NormalsListType n; TexCoordsListType c; IndexTriangleListType t;
		BuildFixture( kind, N, v0, n, c, t );
		Stat build, same, def1, def2, weld, cert;
		bool certified = false, budgetRefusal = false;
		const size_t moved = kind == std::string( "dense" ) ? v0.size() - 1 : v0.size();
		bool open = false, wt = false, open1 = false, wt1 = false, open2 = false, wt2 = false;
		unsigned long long audits[3] = { 0, 0, 0 };
		for( int r = 0; r < reps; ++r ) {
			VerticesListType v = v0;
			auto* m = new TriangleMeshGeometryIndexed( true, true ); m->addref();
			const auto t0 = std::chrono::steady_clock::now();
			m->BeginIndexedTriangles(); m->AddVertices( v ); m->AddNormals( n ); m->AddTexCoords( c ); m->AddIndexedTriangles( t ); m->DoneIndexedTriangles();
			const auto t1 = std::chrono::steady_clock::now();
			open = m->IsProvablyOpenSheet(); wt = m->IsCertifiedWatertight(); audits[0] = m->OpenCertificateAudits();
			m->UpdateVertices( v, NormalsListType() );
			const auto t2 = std::chrono::steady_clock::now();
			for( size_t i = 0; i < moved; ++i ) { Point3& p = v[i]; p.z += 0.03 * ( p.x * p.x + p.y * p.y ); }
			const auto t3a = std::chrono::steady_clock::now();
			m->UpdateVertices( v, NormalsListType() );
			const auto t3 = std::chrono::steady_clock::now();
			open1 = m->IsProvablyOpenSheet(); wt1 = m->IsCertifiedWatertight(); audits[1] = m->OpenCertificateAudits();
			for( size_t i = 0; i < moved; ++i ) { Point3& p = v[i]; p.z += 0.01 * p.y; }
			const auto t4a = std::chrono::steady_clock::now();
			m->UpdateVertices( v, NormalsListType() );
			const auto t4 = std::chrono::steady_clock::now();
			open2 = m->IsProvablyOpenSheet(); wt2 = m->IsCertifiedWatertight(); audits[2] = m->OpenCertificateAudits();
			build.add( Ms( t0, t1 ) ); same.add( Ms( t1, t2 ) ); def1.add( Ms( t3a, t3 ) ); def2.add( Ms( t4a, t4 ) );
			{
				std::vector<unsigned int> ids; Scalar eps = 0;
				const auto w0 = std::chrono::steady_clock::now();
				GeometricUtilities::WeldVertexPositions( v0, ids, &eps );
				const auto w1 = std::chrono::steady_clock::now();
				weld.add( Ms( w0, w1 ) );
			}
			{
				std::vector<Point3> corners;
				corners.reserve( 3 * t.size() );
				for( const IndexedTriangle& tri : t ) {
					for( int k = 0; k < 3; ++k ) corners.push_back( v0[tri.iVertices[k]] );
				}
				const auto c0 = std::chrono::steady_clock::now();
				const GeometricUtilities::OpenSurfaceCertificate oc = GeometricUtilities::IsEmbeddedOpenOrientedSurface( corners );
				const auto c1 = std::chrono::steady_clock::now();
				cert.add( Ms( c0, c1 ) );
				certified = oc.certified;
				budgetRefusal = oc.budgetRefusal;
			}
			m->release();
		}
		std::cout << kind << " tris=" << t.size() << " reps=" << reps
			<< " open/wt build=" << open << "/" << wt << " def1=" << open1 << "/" << wt1 << " def2=" << open2 << "/" << wt2
			<< " open-certificate audits build/def1/def2=" << audits[0] << "/" << audits[1] << "/" << audits[2]
			<< "\n  build ms " << build.mean() << " +- " << build.sd()
			<< "\n  same  ms " << same.mean() << " +- " << same.sd()
			<< "\n  def1  ms " << def1.mean() << " +- " << def1.sd()
			<< "\n  def2  ms " << def2.mean() << " +- " << def2.sd() << "\n";
		if( !weld.v.empty() ) std::cout << "  weld  ms " << weld.mean() << " +- " << weld.sd() << " (WeldVertexPositions alone)\n";
		if( !cert.v.empty() ) std::cout << "  cert  ms " << cert.mean() << " +- " << cert.sd()
			<< " (IsEmbeddedOpenOrientedSurface direct: certified=" << certified << " budgetRefusal=" << budgetRefusal << ")\n";
	}
	struct rusage ru; getrusage( RUSAGE_SELF, &ru );
#ifdef __APPLE__
	const double rssBytes = double( ru.ru_maxrss );			// bytes on macOS
#else
	const double rssBytes = double( ru.ru_maxrss ) * 1024.0;	// kilobytes on Linux
#endif
	std::cout << "peak RSS MB " << rssBytes / ( 1024.0 * 1024.0 ) << "\n";
	return 0;
}
