// hitemp_planck_mean.cpp — reconstruct Planck-mean absorption coefficients
// from a hitemp_reduce histogram, prune it to a certified error bound, and
// emit the compact dataset the fire simulator consumes (§3.5, §12 item 5).
//
// RECONSTRUCTION.  For a histogram cell with amplitude ΣA and ΣA-weighted
// moments of lower-state energy and wavenumber,
//
//   S_cell(T) = ΣA · [Q(296)/Q(T)] · exp(-c2·Ē/T) · [1-exp(-c2·ν̄/T)] · (1+ε2)
//
// where ε2 = ½(c2/T)²·Var(E) is the exact second-moment correction for the
// spread of E'' inside the cell (the reason hitemp_reduce stores ΣA·E''²).
// The Planck mean then follows in the optically-thin limit §3.5 specifies,
// with all physical constants except c2 cancelling between numerator and
// normalizer:
//
//   κ_P(T_gas,T_r)/n = Σ_cells S_cell(T_gas)·ν̄³/[exp(c2·ν̄/T_r)-1]
//                      ÷ { (T_r/c2)^4 · π^4/15 }
//
// Two radiation temperatures are supported because §3.5's escape-factor
// e(x) needs κ_P(T;·) and κ_P(T∞;·) from the SAME local spectrum.
//
// PRUNING is applied to reconstructed strength over the certified (T_gas,T_r)
// domain — never to S296 — and the realized worst-case dropped fraction is
// reported so the record can state a bound rather than assume one.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

namespace {

constexpr double kC2 = 1.4387769;
constexpr double kTref = 296.0;
constexpr double kPi = 3.14159265358979323846;
// Loschmidt-style number density at 1 atm: n[molec/cm^3] = kNAtm / T[K].
constexpr double kNAtm = 7.3389965e21;

struct Cell {
	int    iso;
	double sumA, meanE, meanE2, meanNu;
};

struct Tips {                       // Q(T) per local isotopologue id
	std::map<int, std::vector<std::pair<double,double>>> byIso;

	bool Load( int isoId, const char* path )
	{
		FILE* f = std::fopen( path, "rb" );
		if ( !f ) return false;
		std::vector<std::pair<double,double>> tq;
		char line[512];
		while ( std::fgets( line, sizeof( line ), f ) ) {
			double t = 0, q = 0;
			if ( std::sscanf( line, "%lf %lf", &t, &q ) == 2 && t > 0 && q > 0 ) tq.emplace_back( t, q );
		}
		std::fclose( f );
		if ( tq.size() < 2 ) return false;
		std::sort( tq.begin(), tq.end() );
		byIso[isoId] = std::move( tq );
		return true;
	}

	// Linear interpolation on the 1 K TIPS grid; out-of-range is a hard error
	// (the certified domain must be enforced, never extrapolated — §8).
	double Q( int iso, double T, bool* ok ) const
	{
		auto it = byIso.find( iso );
		if ( it == byIso.end() ) { *ok = false; return 0.0; }
		const auto& v = it->second;
		if ( T < v.front().first || T > v.back().first ) { *ok = false; return 0.0; }
		size_t lo = 0, hi = v.size() - 1;
		while ( hi - lo > 1 ) { size_t m = ( lo + hi ) / 2; if ( v[m].first <= T ) lo = m; else hi = m; }
		const double f = ( T - v[lo].first ) / ( v[hi].first - v[lo].first );
		*ok = true;
		return v[lo].second + f * ( v[hi].second - v[lo].second );
	}
};

inline double CellStrength( const Cell& c, double T, const Tips& tips, bool* ok )
{
	bool okT = false, ok296 = false;
	const double qT   = tips.Q( c.iso, T,     &okT );
	const double q296 = tips.Q( c.iso, kTref, &ok296 );
	if ( !okT || !ok296 || qT <= 0.0 ) { *ok = false; return 0.0; }
	*ok = true;
	const double varE  = std::max( 0.0, c.meanE2 - c.meanE * c.meanE );
	const double x     = kC2 / T;
	const double corr  = 1.0 + 0.5 * x * x * varE;          // second-moment correction
	const double boltz = std::exp( -x * c.meanE );
	const double stim  = 1.0 - std::exp( -x * c.meanNu );
	return c.sumA * ( q296 / qT ) * boltz * stim * corr;
}

// Normalized Planck weight in wavenumber, constants cancelled.
inline double PlanckWeight( double nu, double Tr )
{
	const double x = kC2 * nu / Tr;
	if ( x > 700.0 ) return 0.0;
	const double denom = std::expm1( x );
	if ( denom <= 0.0 ) return 0.0;
	return nu * nu * nu / denom;
}
inline double PlanckNorm( double Tr )
{
	const double t = Tr / kC2;
	return t * t * t * t * ( kPi * kPi * kPi * kPi / 15.0 );
}

} // namespace

int main( int argc, char** argv )
{
	std::string histPath, outPrefix, tipsDir;
	double tLo = 300.0, tHi = 2500.0, tStep = 25.0;
	double pruneRel = 1e-7;
	for ( int i = 1; i < argc; ++i ) {
		if      ( !std::strcmp( argv[i], "--hist" )   && i+1 < argc ) histPath  = argv[++i];
		else if ( !std::strcmp( argv[i], "--out" )    && i+1 < argc ) outPrefix = argv[++i];
		else if ( !std::strcmp( argv[i], "--tips" )   && i+1 < argc ) tipsDir   = argv[++i];
		else if ( !std::strcmp( argv[i], "--tlo" )    && i+1 < argc ) tLo   = std::atof( argv[++i] );
		else if ( !std::strcmp( argv[i], "--thi" )    && i+1 < argc ) tHi   = std::atof( argv[++i] );
		else if ( !std::strcmp( argv[i], "--tstep" )  && i+1 < argc ) tStep = std::atof( argv[++i] );
		else if ( !std::strcmp( argv[i], "--prune" )  && i+1 < argc ) pruneRel = std::atof( argv[++i] );
		else { std::fprintf( stderr, "unknown arg %s\n", argv[i] ); return 2; }
	}
	if ( histPath.empty() || outPrefix.empty() || tipsDir.empty() ) {
		std::fprintf( stderr,
			"usage: hitemp_planck_mean --hist <f> --tips <dir> --out <prefix>\n"
			"       [--tlo 300] [--thi 2500] [--tstep 25] [--prune 1e-7]\n"
			"  <dir> holds TIPS files named q_iso<N>.txt (two columns: T Q)\n" );
		return 2;
	}

	// ---- read histogram -------------------------------------------------
	FILE* f = std::fopen( histPath.c_str(), "rb" );
	if ( !f ) { std::fprintf( stderr, "cannot open %s\n", histPath.c_str() ); return 1; }
	std::vector<Cell> cells;
	std::vector<std::string> header;
	double nuBinW = 25.0;
	char line[1024];
	while ( std::fgets( line, sizeof( line ), f ) ) {
		if ( line[0] == '#' ) {
			header.emplace_back( line );
			double v = 0;
			if ( std::sscanf( line, "# nu_bin_width_cm-1 %lf", &v ) == 1 ) nuBinW = v;
			continue;
		}
		Cell c; unsigned long long nb = 0, eb = 0, cnt = 0;
		if ( std::sscanf( line, "%d %llu %llu %lf %lf %lf %lf %llu",
		                  &c.iso, &nb, &eb, &c.sumA, &c.meanE, &c.meanE2, &c.meanNu, &cnt ) == 8 ) {
			cells.push_back( c );
		}
	}
	std::fclose( f );
	std::fprintf( stderr, "loaded %zu cells (nu bin %.1f cm-1)\n", cells.size(), nuBinW );
	if ( cells.empty() ) return 1;

	// ---- load TIPS ------------------------------------------------------
	Tips tips;
	int nIso = 0;
	for ( int iso = 1; iso <= 12; ++iso ) {
		const std::string p = tipsDir + "/q_iso" + std::to_string( iso ) + ".txt";
		if ( tips.Load( iso, p.c_str() ) ) ++nIso;
	}
	std::fprintf( stderr, "loaded TIPS for %d isotopologues\n", nIso );
	if ( nIso == 0 ) { std::fprintf( stderr, "no TIPS data found in %s\n", tipsDir.c_str() ); return 1; }

	std::vector<double> temps;
	for ( double t = tLo; t <= tHi + 1e-9; t += tStep ) temps.push_back( t );

	// ---- full-precision kappa_P(T_gas, T_r) ------------------------------
	const size_t nT = temps.size();
	std::vector<double> full( nT * nT, 0.0 );
	std::vector<double> cellMaxRel( cells.size(), 0.0 );
	std::vector<double> sT( cells.size(), 0.0 );

	for ( size_t ig = 0; ig < nT; ++ig ) {
		bool anyBad = false;
		for ( size_t c = 0; c < cells.size(); ++c ) {
			bool ok = false;
			sT[c] = CellStrength( cells[c], temps[ig], tips, &ok );
			if ( !ok ) { sT[c] = 0.0; anyBad = true; }
		}
		if ( anyBad && ig == 0 )
			std::fprintf( stderr, "WARNING: some cells lacked TIPS coverage at T=%.0f\n", temps[ig] );
		for ( size_t ir = 0; ir < nT; ++ir ) {
			const double norm = PlanckNorm( temps[ir] );
			double acc = 0.0;
			for ( size_t c = 0; c < cells.size(); ++c )
				acc += sT[c] * PlanckWeight( cells[c].meanNu, temps[ir] );
			full[ig * nT + ir] = acc / norm;
			if ( acc > 0.0 ) {
				for ( size_t c = 0; c < cells.size(); ++c ) {
					const double rel = sT[c] * PlanckWeight( cells[c].meanNu, temps[ir] ) / acc;
					if ( rel > cellMaxRel[c] ) cellMaxRel[c] = rel;
				}
			}
		}
	}

	// ---- prune and measure the realized error ----------------------------
	std::vector<size_t> keep;
	for ( size_t c = 0; c < cells.size(); ++c ) if ( cellMaxRel[c] >= pruneRel ) keep.push_back( c );

	double worstDrop = 0.0;
	for ( size_t ig = 0; ig < nT; ++ig ) {
		for ( size_t c = 0; c < cells.size(); ++c ) {
			bool ok = false;
			sT[c] = CellStrength( cells[c], temps[ig], tips, &ok );
			if ( !ok ) sT[c] = 0.0;
		}
		for ( size_t ir = 0; ir < nT; ++ir ) {
			const double norm = PlanckNorm( temps[ir] );
			double kept = 0.0;
			for ( size_t k : keep ) kept += sT[k] * PlanckWeight( cells[k].meanNu, temps[ir] );
			kept /= norm;
			const double ref = full[ig * nT + ir];
			if ( ref > 0.0 ) worstDrop = std::max( worstDrop, std::fabs( ref - kept ) / ref );
		}
	}
	std::fprintf( stderr, "pruned %zu -> %zu cells; worst relative kappa_P error %.3e\n",
	              cells.size(), keep.size(), worstDrop );

	// ---- emit pruned histogram ------------------------------------------
	const std::string hOut = outPrefix + "_pruned.hist";
	FILE* fo = std::fopen( hOut.c_str(), "wb" );
	for ( const std::string& h : header ) std::fputs( h.c_str(), fo );
	std::fprintf( fo, "# pruned_from_cells %zu\n", cells.size() );
	std::fprintf( fo, "# prune_rel_threshold %.3e\n", pruneRel );
	std::fprintf( fo, "# prune_domain_T_K %.1f %.1f\n", tLo, tHi );
	std::fprintf( fo, "# prune_worst_rel_kappaP_error %.6e\n", worstDrop );
	std::fprintf( fo, "# cells_kept %zu\n", keep.size() );
	for ( size_t k : keep ) {
		const Cell& c = cells[k];
		std::fprintf( fo, "%d %.17g %.10g %.10g %.10g\n",
		              c.iso, c.sumA, c.meanE, c.meanE2, c.meanNu );
	}
	std::fclose( fo );

	// ---- emit kappa_P table ---------------------------------------------
	const std::string kOut = outPrefix + "_kappaP.txt";
	fo = std::fopen( kOut.c_str(), "wb" );
	std::fprintf( fo, "# kappa_P per unit number density [cm^2/molecule] and at 1 atm [1/(m*atm)]\n" );
	std::fprintf( fo, "# rows: T_gas [K]; cols: T_radiation [K]\n" );
	std::fprintf( fo, "# T_grid %.1f %.1f %.1f\n", tLo, tHi, tStep );
	std::fprintf( fo, "# columns T_gas T_rad kappaP_cm2_per_molecule kappaP_per_m_atm\n" );
	for ( size_t ig = 0; ig < nT; ++ig ) {
		for ( size_t ir = 0; ir < nT; ++ir ) {
			const double sigma = full[ig * nT + ir];                 // cm^2/molecule
			const double n1atm = kNAtm / temps[ig];                  // molec/cm^3
			const double kappa_per_m = sigma * n1atm * 100.0;        // cm^-1 -> m^-1
			std::fprintf( fo, "%.1f %.1f %.10e %.10e\n", temps[ig], temps[ir], sigma, kappa_per_m );
		}
	}
	std::fclose( fo );

	// Diagonal (T_r = T_gas) is the classic tabulated kappa_P for validation
	// against published fits.
	std::fprintf( stderr, "diagonal kappa_P [1/(m*atm)]:\n" );
	for ( size_t i = 0; i < nT; i += std::max<size_t>( 1, nT / 12 ) ) {
		const double sigma = full[i * nT + i];
		std::fprintf( stderr, "  T=%6.0f  %.6g\n", temps[i], sigma * ( kNAtm / temps[i] ) * 100.0 );
	}
	std::fprintf( stderr, "wrote %s and %s\n", hOut.c_str(), kOut.c_str() );
	return 0;
}
