//////////////////////////////////////////////////////////////////////
//
//  SchlickDirectionalAlbedoGen.cpp - DL-310: bakes the directional
//    albedo of the DL-225 bounded Schlick SPECULAR lobe, the quantity
//    the coupled diffuse term of SchlickBRDF / SchlickSPF reads
//    (docs/DL178_DL212_BOUNDED_SCHLICK_WARD.md, "DL-310").
//
//  Writes src/Library/Materials/SchlickDirectionalAlbedo_LUTData.cpp.
//  Run from the PROJECT ROOT:
//
//      c++ -O3 -std=c++17 -pthread -I src/Library \
//          -o /tmp/SchlickDirectionalAlbedoGen tools/SchlickDirectionalAlbedoGen.cpp
//      /tmp/SchlickDirectionalAlbedoGen
//
//  No RNG anywhere and a fixed work order, so a re-run reproduces the
//  file byte-for-byte.  It #includes Materials/SchlickMasking.h and
//  calls the production masking verbatim (header-only, compiles
//  standalone), so the bake cannot drift from the BRDF's masking; the
//  remaining factors (Z, A, Fresnel, the reflection geometry) are the
//  six lines below, written from SchlickBRDF.cpp's formula.  REGENERATE
//  whenever SchlickBRDF's specular term or SchlickMasking.h changes.
//
//  WHAT IS BAKED.  With S = rho + (1-rho) F, F = (1 - h.v)^5, the
//  UNGATED directional albedo of the specular lobe is linear in rho:
//
//      A(v) = int f_S(v,l) nl dl = rho M0(v) + (1-rho) M5(v),
//      M_k(v) = int Z A(phi_h) m(v) m(l) (h.v)/(pi nv) F^(k/5) dh,
//
//  with m the DL-225 masking.  Both moments depend on the view's
//  cosine mu = n.v, its azimuth phi_v, the roughness r and the isotropy
//  p (folded into (0,1]; A_p(phi) = A_{1/p}(phi + pi/2), so the
//  azimuth MAXIMUM below is fold-invariant).  The table stores, per
//  (mu, r, p) node, the MAXIMUM over the view azimuth of each moment
//  (17 samples of [0, pi/2], which the quarter-period symmetry of
//  A(phi) covers) -- the coupled diffuse needs an UPPER bound of A
//  (DL-310: rho_d(i) <= A_true(i) + (1 - A_used(i)) <= 1 needs
//  A_used >= A_true), and a function of mu alone keeps the Pdf's
//  second quadrature one-dimensional.
//
//  CONSERVATIVE INTERPOLATION.  The runtime interpolates trilinearly
//  in (sqrt(mu), log r, log p) index space.  A multilinear interpolant
//  is a convex combination of its corners, so it bounds the function
//  from above inside a cell whenever every corner is at least the
//  function's maximum there; short of that, each node is RAISED by the
//  largest deficit (true minus interpolated) found at the probe points
//  of the cells it bounds: every cell centre, and the midpoint of every
//  cell edge.  After that bump the interpolant is >= the true value at
//  every probe (the bump is constant over a cell's corners, and a
//  multilinear interpolant of a constant is that constant).  Between
//  probes it is a measured, not a proven, bound -- see the banner's
//  residual line and tests/SchlickKrayBRDFConsistencyTest.cpp.
//
//  QUADRATURE.  Per azimuth phi_h the reflection is above the horizon
//  exactly for theta_h < (atan2(V, nv) + pi/2)/2, V the view's
//  tangential component along phi_h, so the theta_h integral runs over
//  that interval with the sampler's own warp cos^2 theta =
//  xi/(r + xi(1-r)) and xi = u^2 (removes the grazing endpoint
//  singularity), 64 Gauss-Legendre nodes in u.  phi_h is integrated
//  directly, 16-node Gauss-Legendre per sub-interval, the sub-intervals
//  graded geometrically toward A's peaks at phi = 0 and pi (width ~p).
//  Measured convergence against 32/128 nodes is printed in the banner.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include "Materials/SchlickMasking.h"

using namespace RISE::Implementation;

namespace
{
	const double kPi = 3.14159265358979323846264338327950288;

	// MUST match src/Library/Materials/SchlickDirectionalAlbedo.h.
	const int kNumMu = 33;
	const int kNumR  = 31;
	const int kNumP  = 13;
	const double kLogRMin = -5.0, kLogRMax = 0.0;
	const double kLogPMin = -3.0, kLogPMax = 0.0;
	const double kMuFloor = 1e-6;		// node 0 stands for the grazing limit

	double MuAt( double u ) { const double x = u / double(kNumMu - 1); return std::max( kMuFloor, x*x ); }
	double RAt( double k ) { return std::pow( 10.0, kLogRMin + (kLogRMax - kLogRMin) * k / double(kNumR - 1) ); }
	double PAt( double m ) { return std::pow( 10.0, kLogPMin + (kLogPMax - kLogPMin) * m / double(kNumP - 1) ); }

	struct GL
	{
		std::vector<double> x, w;
		explicit GL( int n ) : x(n), w(n)
		{
			for( int i = 0; i < n; ++i ) {
				double z = std::cos( kPi * (i + 0.75) / (n + 0.5) ), z1, pp;
				do {
					double p1 = 1, p2 = 0;
					for( int j = 0; j < n; ++j ) { const double p3 = p2; p2 = p1; p1 = ((2*j + 1)*z*p2 - j*p3)/(j + 1); }
					pp = n*(z*p1 - p2)/(z*z - 1);
					z1 = z; z = z1 - p1/pp;
				} while( std::fabs( z - z1 ) > 1e-15 );
				x[i] = 0.5*(1 - z); w[i] = 1.0/((1 - z*z)*pp*pp);
			}
		}
	};

	//! M0 and M5 at view (mu, phiV) for roughness r, isotropy p.
	void Moments( const double mu, const double phiV, const double r, const double p,
		const GL& glPhi, const GL& glU, double& M0, double& M5 )
	{
		M0 = M5 = 0;
		const double nv = mu;
		const double sv = std::sqrt( std::max( 0.0, 1 - nv*nv ) );
		const double vx = sv*std::cos( phiV ), vy = sv*std::sin( phiV );
		SchlickMasking::Lane lane;
		SchlickMasking::PrepareUncached( lane, r, p );
		const double denV = SchlickMasking::MaskDen( lane, nv, vx, vy );
		const double pf = ( p > 1 ) ? 1.0/p : p;
		std::vector<double> bp;
		bp.push_back( 0 );
		for( double x = 0.25*pf; x < 0.45*kPi; x *= 2 ) bp.push_back( x );
		bp.push_back( 0.5*kPi );
		for( int quad = 0; quad < 4; ++quad ) {
			for( size_t k = 0; k + 1 < bp.size(); ++k ) {
				const double lo = bp[k], hi = bp[k+1];
				for( size_t g = 0; g < glPhi.x.size(); ++g ) {
					const double d = lo + (hi - lo)*glPhi.x[g], wphi = glPhi.w[g]*(hi - lo);
					double phi = ( quad == 0 ) ? d : ( quad == 1 ) ? kPi - d : ( quad == 2 ) ? kPi + d : 2*kPi - d;
					if( p > 1 ) phi += 0.5*kPi;
					const double cp = std::cos( phi ), sp = std::sin( phi );
					const double A = std::sqrt( p/(p*p - p*p*sp*sp + sp*sp) );
					const double V = cp*vx + sp*vy;
					const double thMax = 0.5*(std::atan2( V, nv ) + 0.5*kPi);
					if( thMax <= 0 ) continue;
					const double cmin = std::cos( std::min( thMax, 0.5*kPi ) );
					const double c2 = cmin*cmin;
					const double xiMin = ( c2 >= 1 ) ? 1.0 : c2*r/(1 - c2*(1 - r));
					const double uMin = std::sqrt( std::max( 0.0, xiMin ) );
					double s0 = 0, s5 = 0;
					for( size_t k2 = 0; k2 < glU.x.size(); ++k2 ) {
						const double u = uMin + (1 - uMin)*glU.x[k2], wgt = glU.w[k2]*(1 - uMin);
						const double xi = u*u;
						const double t = std::sqrt( xi/(r + (1 - r)*xi) );
						const double st = std::sqrt( std::max( 0.0, 1 - t*t ) );
						const double hx = st*cp, hy = st*sp, hz = t;
						const double hv = hx*vx + hy*vy + hz*nv;
						if( hv <= 0 ) continue;
						const double nl = 2*hv*hz - nv;
						if( nl <= 0 ) continue;
						const double lx = 2*hv*hx - vx, ly = 2*hv*hy - vy;
						const double denL = SchlickMasking::MaskDen( lane, nl, lx, ly );
						// f_S nl dl / p_h dh with p_h = 2 t Z (1/2pi) (the xi
						// warp in theta, phi integrated in its own measure):
						// A hv nl / (2 pi t denV denL), times 2u for xi = u^2.
						const double R = A/(2*kPi)*hv*nl/(t*denV*denL);
						const double f = 1 - hv, f2 = f*f, F = f2*f2*f;
						s0 += wgt*2*u*R;
						s5 += wgt*2*u*R*F;
					}
					M0 += wphi*s0;
					M5 += wphi*s5;
				}
			}
		}
	}

	//! Maximum over nAz view azimuths in [0, pi/2] of each moment.
	void MaxMoments( const double mu, const double r, const double p, const int nAz,
		const GL& glPhi, const GL& glU, double& M0, double& M5 )
	{
		M0 = M5 = 0;
		for( int a = 0; a < nAz; ++a ) {
			const double phiV = 0.5*kPi*double(a)/double(nAz - 1);
			double m0, m5;
			Moments( mu, phiV, r, p, glPhi, glU, m0, m5 );
			M0 = std::max( M0, m0 );
			M5 = std::max( M5, m5 );
		}
	}

	inline int Idx( int j, int k, int m ) { return (m*kNumR + k)*kNumMu + j; }

	template< class F >
	void ParallelFor( const int n, F f )
	{
		const unsigned nt = std::max( 1u, std::thread::hardware_concurrency() );
		std::atomic<int> next( 0 );
		std::vector<std::thread> th;
		for( unsigned t = 0; t < nt; ++t ) {
			th.emplace_back( [&]() {
				const GL glPhi( 16 ), glU( 64 );
				for( int i = next++; i < n; i = next++ ) f( i, glPhi, glU );
			} );
		}
		for( auto& t : th ) t.join();
	}

	float RoundUp( const double v )
	{
		float f = float( v );
		if( double(f) < v ) f = std::nextafter( f, 1e30f );
		return f;
	}
}

int main( int argc, char** argv )
{
	const char* outPath = ( argc > 1 ) ? argv[1] : "src/Library/Materials/SchlickDirectionalAlbedo_LUTData.cpp";
	const int N = kNumMu*kNumR*kNumP;
	std::vector<double> base0( N ), base5( N );

	// 1. Nodes: azimuth maximum over 17 samples.
	ParallelFor( N, [&]( int i, const GL& glPhi, const GL& glU ) {
		const int j = i % kNumMu, k = (i / kNumMu) % kNumR, m = i / (kNumMu*kNumR);
		MaxMoments( MuAt( j ), RAt( k ), PAt( m ), 17, glPhi, glU, base0[i], base5[i] );
	} );

	// 2. Probes: cell centres and edge midpoints, each against the
	//    multilinear interpolant of the base nodes.
	struct Probe { double u, k, m; };
	std::vector<Probe> probes;
	for( int m = 0; m < kNumP; ++m ) for( int k = 0; k < kNumR; ++k ) for( int j = 0; j < kNumMu; ++j ) {
		if( j + 1 < kNumMu && k + 1 < kNumR && m + 1 < kNumP ) probes.push_back( { j + 0.5, k + 0.5, m + 0.5 } );
		if( j + 1 < kNumMu ) probes.push_back( { j + 0.5, double(k), double(m) } );
		if( k + 1 < kNumR )  probes.push_back( { double(j), k + 0.5, double(m) } );
		if( m + 1 < kNumP )  probes.push_back( { double(j), double(k), m + 0.5 } );
	}
	std::vector<double> def0( probes.size() ), def5( probes.size() );
	auto Interp = [&]( const std::vector<double>& t, double u, double kk, double mm ) {
		const int j0 = std::min( int(u), kNumMu - 2 ), k0 = std::min( int(kk), kNumR - 2 ), m0 = std::min( int(mm), kNumP - 2 );
		const double fu = u - j0, fk = kk - k0, fm = mm - m0;
		double s = 0;
		for( int c = 0; c < 8; ++c ) {
			const int dj = c & 1, dk = (c >> 1) & 1, dm = (c >> 2) & 1;
			const double w = (dj ? fu : 1 - fu)*(dk ? fk : 1 - fk)*(dm ? fm : 1 - fm);
			if( w != 0 ) s += w*t[Idx( j0 + dj, k0 + dk, m0 + dm )];
		}
		return s;
	};
	ParallelFor( int(probes.size()), [&]( int i, const GL& glPhi, const GL& glU ) {
		const Probe& pr = probes[i];
		const double mu = std::max( kMuFloor, (pr.u/double(kNumMu - 1))*(pr.u/double(kNumMu - 1)) );
		double t0, t5;
		MaxMoments( mu, RAt( pr.k ), PAt( pr.m ), 17, glPhi, glU, t0, t5 );
		def0[i] = std::max( 0.0, t0 - Interp( base0, pr.u, pr.k, pr.m ) );
		def5[i] = std::max( 0.0, t5 - Interp( base5, pr.u, pr.k, pr.m ) );
	} );

	// 3. Bump every node by the worst deficit of any probe whose
	//    interpolation stencil it belongs to.
	std::vector<double> bump0( N, 0.0 ), bump5( N, 0.0 );
	double worstDef0 = 0, worstDef5 = 0;
	for( size_t i = 0; i < probes.size(); ++i ) {
		const Probe& pr = probes[i];
		worstDef0 = std::max( worstDef0, def0[i] );
		worstDef5 = std::max( worstDef5, def5[i] );
		const int j0 = std::min( int(pr.u), kNumMu - 2 ), k0 = std::min( int(pr.k), kNumR - 2 ), m0 = std::min( int(pr.m), kNumP - 2 );
		for( int c = 0; c < 8; ++c ) {
			const int dj = c & 1, dk = (c >> 1) & 1, dm = (c >> 2) & 1;
			const int id = Idx( j0 + dj, k0 + dk, m0 + dm );
			bump0[id] = std::max( bump0[id], def0[i] );
			bump5[id] = std::max( bump5[id], def5[i] );
		}
	}

	// 4. Convergence evidence: the same maxima at 32/128 nodes on a
	//    deterministic subset of nodes.
	double worstConv = 0; int convAt = -1;
	{
		std::vector<int> subset;
		for( int i = 0; i < N; i += 37 ) subset.push_back( i );
		std::vector<double> diff( subset.size() );
		const unsigned nt = std::max( 1u, std::thread::hardware_concurrency() );
		std::atomic<int> next( 0 );
		std::vector<std::thread> th;
		for( unsigned t = 0; t < nt; ++t ) th.emplace_back( [&]() {
			const GL glPhi( 32 ), glU( 128 );
			for( int s = next++; s < int(subset.size()); s = next++ ) {
				const int i = subset[s];
				const int j = i % kNumMu, k = (i / kNumMu) % kNumR, m = i / (kNumMu*kNumR);
				double a0, a5;
				MaxMoments( MuAt( j ), RAt( k ), PAt( m ), 17, glPhi, glU, a0, a5 );
				diff[s] = std::max( std::fabs( a0 - base0[i] ), std::fabs( a5 - base5[i] ) );
			}
		} );
		for( auto& t : th ) t.join();
		for( size_t s = 0; s < subset.size(); ++s ) if( diff[s] > worstConv ) { worstConv = diff[s]; convAt = subset[s]; }
	}

	std::vector<float> out0( N ), out5( N );
	double worstBump = 0; int bumpAt = -1;
	for( int i = 0; i < N; ++i ) {
		out0[i] = RoundUp( base0[i] + bump0[i] );
		out5[i] = RoundUp( base5[i] + bump5[i] );
		const double b = std::max( bump0[i], bump5[i] );
		if( b > worstBump ) { worstBump = b; bumpAt = i; }
	}
	// 5. Residual evidence: the FINAL (bumped, float) interpolant against
	//    the true azimuth maxima at points that are not probes -- a
	//    deterministic low-discrepancy set (Weyl sequence) over the whole
	//    (sqrt mu, log r, log p) box.
	const int kValidate = 4000;
	std::vector<double> resid( kValidate ), over( kValidate );
	{
		std::vector<double> fin0( N ), fin5( N );
		for( int i = 0; i < N; ++i ) { fin0[i] = out0[i]; fin5[i] = out5[i]; }
		ParallelFor( kValidate, [&]( int i, const GL& glPhi, const GL& glU ) {
			const double a1 = 0.7548776662466927, a2 = 0.5698402909980532, a3 = 0.4192247;
			const double x1 = std::fmod( 0.5 + a1*(i + 1), 1.0 ), x2 = std::fmod( 0.5 + a2*(i + 1), 1.0 ), x3 = std::fmod( 0.5 + a3*(i + 1), 1.0 );
			const double u = x1*(kNumMu - 1), kk = x2*(kNumR - 1), mm = x3*(kNumP - 1);
			const double mu = std::max( kMuFloor, x1*x1 );
			double t0, t5;
			MaxMoments( mu, RAt( kk ), PAt( mm ), 17, glPhi, glU, t0, t5 );
			// worst over rho in [0,1] of A_true - A_table is at rho 0 or 1
			const double d0 = t0 - Interp( fin0, u, kk, mm ), d5 = t5 - Interp( fin5, u, kk, mm );
			resid[i] = std::max( d0, d5 );
			over[i] = std::max( -d0, -d5 );
		} );
	}
	double worstResid = -1, worstOver = 0; int residAt = -1;
	for( int i = 0; i < kValidate; ++i ) {
		if( resid[i] > worstResid ) { worstResid = resid[i]; residAt = i; }
		worstOver = std::max( worstOver, over[i] );
	}
	int nResid3 = 0, nResid2 = 0;
	for( int i = 0; i < kValidate; ++i ) { if( resid[i] > 1e-3 ) ++nResid3; if( resid[i] > 1e-2 ) ++nResid2; }

	std::vector<float> top0( kNumR*kNumP, 0.0f ), top5( kNumR*kNumP, 0.0f );
	for( int m = 0; m < kNumP; ++m ) for( int k = 0; k < kNumR; ++k ) for( int j = 0; j < kNumMu; ++j ) {
		top0[m*kNumR + k] = std::max( top0[m*kNumR + k], out0[Idx( j, k, m )] );
		top5[m*kNumR + k] = std::max( top5[m*kNumR + k], out5[Idx( j, k, m )] );
	}

	FILE* f = std::fopen( outPath, "w" );
	if( !f ) { std::perror( outPath ); return 1; }
	std::fprintf( f,
		"//////////////////////////////////////////////////////////////////////\n"
		"//\n"
		"//  SchlickDirectionalAlbedo_LUTData.cpp - DL-310: azimuth-maximum\n"
		"//    directional albedo moments of the DL-225 bounded Schlick\n"
		"//    specular lobe, conservatively interpolable.\n"
		"//\n"
		"//  AUTO-GENERATED by tools/SchlickDirectionalAlbedoGen.cpp.  DO NOT\n"
		"//  EDIT BY HAND.  Regenerate from the PROJECT ROOT with:\n"
		"//\n"
		"//      c++ -O3 -std=c++17 -pthread -I src/Library \\\n"
		"//          -o /tmp/SchlickDirectionalAlbedoGen tools/SchlickDirectionalAlbedoGen.cpp\n"
		"//      /tmp/SchlickDirectionalAlbedoGen\n"
		"//\n"
		"//  Layout, axes and the conservative-envelope construction: see\n"
		"//  SchlickDirectionalAlbedo.h and the generator's file header.\n"
		"//\n"
		"//  Evidence (deterministic; reproduced byte-for-byte by a re-run):\n"
		"//    %d probes (cell centres + edge midpoints)\n"
		"//    worst pre-bump interpolation deficit  M0 %.3e  M5 %.3e\n"
		"//    worst node bump                       %.3e (node %d)\n"
		"//    quadrature 16/64 vs 32/128 nodes      %.3e (node %d, every 37th node)\n"
		"//    %d off-probe validation points (Weyl): worst remaining deficit\n"
		"//      max(A_true - A_table) = %.3e (point %d); %d points > 1e-3,\n"
		"//      %d > 1e-2; worst over-estimate %.3e\n"
		"//\n"
		"//////////////////////////////////////////////////////////////////////\n\n"
		"#include \"pch.h\"\n"
		"#include \"SchlickDirectionalAlbedo.h\"\n\n"
		"namespace RISE\n{\n\tnamespace SchlickDirectionalAlbedo\n\t{\n",
		int(probes.size()), worstDef0, worstDef5, worstBump, bumpAt, worstConv, convAt,
		kValidate, worstResid, residAt, nResid3, nResid2, worstOver );
	auto Emit = [&]( const char* name, const std::vector<float>& t, int rows, int cols, const char* dims ) {
		std::fprintf( f, "\t\textern const float %s%s = {\n", name, dims );
		for( int r = 0; r < rows; ++r ) {
			std::fprintf( f, "\t\t\t" );
			for( int c = 0; c < cols; ++c ) std::fprintf( f, "%.9gf,%s", double( t[r*cols + c] ), ( c + 1 < cols ) ? " " : "" );
			std::fprintf( f, "\n" );
		}
		std::fprintf( f, "\t\t};\n\n" );
	};
	Emit( "kM0", out0, kNumP*kNumR, kNumMu, "[ kNumP * kNumR * kNumMu ]" );
	Emit( "kM5", out5, kNumP*kNumR, kNumMu, "[ kNumP * kNumR * kNumMu ]" );
	Emit( "kTopM0", top0, kNumP, kNumR, "[ kNumP * kNumR ]" );
	Emit( "kTopM5", top5, kNumP, kNumR, "[ kNumP * kNumR ]" );
	std::fprintf( f, "\t}\n}\n" );
	std::fclose( f );
	std::fprintf( stderr, "probes %d  deficit M0 %.3e M5 %.3e  bump %.3e (node %d)  conv %.3e (node %d)\n"
		"validation: worst deficit %.3e (point %d), >1e-3: %d, >1e-2: %d, worst over %.3e\n",
		int(probes.size()), worstDef0, worstDef5, worstBump, bumpAt, worstConv, convAt,
		worstResid, residAt, nResid3, nResid2, worstOver );
	return 0;
}
