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
	struct Curve {
		std::vector<std::pair<double,double>> samples;
		std::vector<double> slopes;
	};
	std::map<int, Curve> byIso;

	static std::vector<double> PCHIPSlopes(
		const std::vector<std::pair<double,double>>& samples )
	{
		const size_t count = samples.size();
		std::vector<double> h(count-1), delta(count-1), slopes(count,0.0);
		for ( size_t i = 0; i+1 < count; ++i ) {
			h[i] = samples[i+1].first-samples[i].first;
			delta[i] = (samples[i+1].second-samples[i].second)/h[i];
		}
		if ( count == 2 ) { slopes[0] = slopes[1] = delta[0]; return slopes; }
		for ( size_t i = 1; i+1 < count; ++i ) {
			if ( delta[i-1]*delta[i] > 0.0 ) {
				const double w1 = 2.0*h[i]+h[i-1], w2 = h[i]+2.0*h[i-1];
				slopes[i] = (w1+w2)/(w1/delta[i-1]+w2/delta[i]);
			}
		}
		auto endpoint = []( double h0, double h1, double d0, double d1 ) {
			double value = ((2.0*h0+h1)*d0-h0*d1)/(h0+h1);
			if ( value*d0 <= 0.0 ) return 0.0;
			if ( d0*d1 < 0.0 && std::fabs(value) > std::fabs(3.0*d0) ) return 3.0*d0;
			return value;
		};
		slopes.front() = endpoint(h[0],h[1],delta[0],delta[1]);
		slopes.back() = endpoint(h[count-2],h[count-3],delta[count-2],delta[count-3]);
		return slopes;
	}

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
		Curve curve;
		curve.samples = std::move( tq );
		curve.slopes = PCHIPSlopes(curve.samples);
		byIso[isoId] = std::move( curve );
		return true;
	}

	// Shape-preserving C1 interpolation on the 1 K TIPS grid.  Values at the
	// integer knots exactly reproduce TIPS; the analytic derivative closes the
	// arbitrary-temperature opacity representation required by SS3.5.
	double Q( int iso, double T, double* derivative, bool* ok ) const
	{
		auto it = byIso.find( iso );
		if ( it == byIso.end() ) { *ok = false; return 0.0; }
		const auto& v = it->second.samples;
		if ( T < v.front().first || T > v.back().first ) { *ok = false; return 0.0; }
		size_t lo = 0, hi = v.size() - 1;
		while ( hi - lo > 1 ) { size_t m = ( lo + hi ) / 2; if ( v[m].first <= T ) lo = m; else hi = m; }
		const double h = v[hi].first-v[lo].first;
		const double u = (T-v[lo].first)/h;
		const double u2 = u*u, u3 = u2*u;
		const double h00 = 2.0*u3-3.0*u2+1.0;
		const double h10 = u3-2.0*u2+u;
		const double h01 = -2.0*u3+3.0*u2;
		const double h11 = u3-u2;
		const double value = h00*v[lo].second+h10*h*it->second.slopes[lo]+
			h01*v[hi].second+h11*h*it->second.slopes[hi];
		if ( derivative ) {
			*derivative = ((6.0*u2-6.0*u)*v[lo].second+
				(3.0*u2-4.0*u+1.0)*h*it->second.slopes[lo]+
				(-6.0*u2+6.0*u)*v[hi].second+
				(3.0*u2-2.0*u)*h*it->second.slopes[hi])/h;
		}
		*ok = true;
		return value;
	}
};

inline double CellStrength(
	const Cell& c, double T, const Tips& tips, double* derivative, bool* ok )
{
	bool okT = false, ok296 = false;
	double qDerivative = 0.0;
	const double qT   = tips.Q( c.iso, T,     &qDerivative, &okT );
	const double q296 = tips.Q( c.iso, kTref, nullptr, &ok296 );
	if ( !okT || !ok296 || qT <= 0.0 ) { *ok = false; return 0.0; }
	*ok = true;
	const double varE  = std::max( 0.0, c.meanE2 - c.meanE * c.meanE );
	const double x     = kC2 / T;
	const double corr  = 1.0 + 0.5 * x * x * varE;          // second-moment correction
	const double boltz = std::exp( -x * c.meanE );
	const double exponential = std::exp( -x * c.meanNu );
	const double stim  = -std::expm1( -x * c.meanNu );
	const double value = c.sumA * ( q296 / qT ) * boltz * stim * corr;
	if ( derivative ) {
		const double logarithmicDerivative = -qDerivative/qT+
			kC2*c.meanE/(T*T)-exponential*kC2*c.meanNu/(T*T*stim)-
			x*x*varE/(T*corr);
		*derivative = value*logarithmicDerivative;
	}
	return value;
}

inline double PlanckNorm( double Tr )
{
	const double t = Tr / kC2;
	return t * t * t * t * ( kPi * kPi * kPi * kPi / 15.0 );
}

// Normalized Planck weight in wavenumber, constants cancelled.
inline double PlanckWeight( double nu, double Tr, double* derivative )
{
	const double x = kC2 * nu / Tr;
	if ( x > 700.0 ) { if ( derivative ) *derivative = 0.0; return 0.0; }
	const double denom = std::expm1( x );
	if ( denom <= 0.0 ) { if ( derivative ) *derivative = 0.0; return 0.0; }
	const double value = nu*nu*nu/denom/PlanckNorm(Tr);
	if ( derivative ) {
		*derivative = value*(x/Tr*(denom+1.0)/denom-4.0/Tr);
	}
	return value;
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
		                  &c.iso, &nb, &eb, &c.sumA, &c.meanE, &c.meanE2, &c.meanNu, &cnt ) == 8 ||
		     std::sscanf( line, "%d %lf %lf %lf %lf",
		                  &c.iso, &c.sumA, &c.meanE, &c.meanE2, &c.meanNu ) == 5 ) {
			if ( c.iso < 1 || c.iso > 12 || !std::isfinite(c.sumA) || c.sumA <= 0.0 ||
			     !std::isfinite(c.meanE) || c.meanE < 0.0 ||
			     !std::isfinite(c.meanE2) ||
			     c.meanE2+2.0e-9*std::max(1.0,c.meanE*c.meanE) < c.meanE*c.meanE ||
			     !std::isfinite(c.meanNu) || c.meanNu <= 0.0 ) {
				std::fprintf( stderr, "invalid histogram cell\n" );
				std::fclose( f );
				return 1;
			}
			cells.push_back( c );
		} else {
			std::fprintf( stderr, "unparseable histogram row\n" );
			std::fclose( f );
			return 1;
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
	std::vector<double> gasDerivative( nT * nT, 0.0 );
	std::vector<double> radiationDerivative( nT * nT, 0.0 );
	std::vector<double> mixedDerivative( nT * nT, 0.0 );
	std::vector<double> cellMaxRel( cells.size(), 0.0 );
	std::vector<double> sT( cells.size(), 0.0 );
	std::vector<double> dsT( cells.size(), 0.0 );

	for ( size_t ig = 0; ig < nT; ++ig ) {
		bool anyBad = false;
		for ( size_t c = 0; c < cells.size(); ++c ) {
			bool ok = false;
			sT[c] = CellStrength( cells[c], temps[ig], tips, &dsT[c], &ok );
			if ( !ok ) { sT[c] = dsT[c] = 0.0; anyBad = true; }
		}
		if ( anyBad && ig == 0 )
			std::fprintf( stderr, "WARNING: some cells lacked TIPS coverage at T=%.0f\n", temps[ig] );
		for ( size_t ir = 0; ir < nT; ++ir ) {
			double acc = 0.0, dGas = 0.0, dRadiation = 0.0, dMixed = 0.0;
			for ( size_t c = 0; c < cells.size(); ++c ) {
				double dWeight = 0.0;
				const double weight = PlanckWeight(cells[c].meanNu,temps[ir],&dWeight);
				acc += sT[c]*weight;
				dGas += dsT[c]*weight;
				dRadiation += sT[c]*dWeight;
				dMixed += dsT[c]*dWeight;
			}
			const size_t index = ig*nT+ir;
			full[index] = acc;
			gasDerivative[index] = dGas;
			radiationDerivative[index] = dRadiation;
			mixedDerivative[index] = dMixed;
			if ( acc > 0.0 ) {
				for ( size_t c = 0; c < cells.size(); ++c ) {
					const double rel = sT[c]*PlanckWeight(cells[c].meanNu,temps[ir],nullptr)/acc;
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
			sT[c] = CellStrength( cells[c], temps[ig], tips, nullptr, &ok );
			if ( !ok ) sT[c] = 0.0;
		}
		for ( size_t ir = 0; ir < nT; ++ir ) {
			double kept = 0.0;
			for ( size_t k : keep ) kept += sT[k]*PlanckWeight(cells[k].meanNu,temps[ir],nullptr);
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

	// Analytic knot derivatives from the committed exponential-sum basis.
	// These seed the record's C1 bicubic representation; its per-cell
	// derivative enclosures are then obtained algebraically from the bicubic
	// coefficients, never by finite differencing runtime values.
	const std::string sOut = outPrefix + "_surface.txt";
	fo = std::fopen( sOut.c_str(), "wb" );
	if ( !fo ) { std::fprintf(stderr,"cannot write %s\n",sOut.c_str()); return 1; }
	std::fprintf( fo, "# HITEMP pruned exponential-sum Planck-mean surface v1\n" );
	std::fprintf( fo, "# T_grid %.1f %.1f %.1f\n", tLo, tHi, tStep );
	std::fprintf( fo, "# columns T_gas T_rad kappaP_cm2_per_molecule d_dTgas d_dTrad d2_dTgas_dTrad\n" );
	for ( size_t ig = 0; ig < nT; ++ig ) {
		for ( size_t ir = 0; ir < nT; ++ir ) {
			const size_t index = ig*nT+ir;
			std::fprintf( fo, "%.1f %.1f %.17e %.17e %.17e %.17e\n",
				temps[ig],temps[ir],full[index],gasDerivative[index],
				radiationDerivative[index],mixedDerivative[index] );
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
	std::fprintf( stderr, "wrote %s, %s, and %s\n",
	              hOut.c_str(), kOut.c_str(), sOut.c_str() );
	return 0;
}
