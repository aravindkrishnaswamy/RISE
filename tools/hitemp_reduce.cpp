// hitemp_reduce.cpp — HITEMP line-list → compact temperature-independent
// spectral-energy histogram, for the fire simulator's CO2/H2O cooling record
// (FIRE_SMOKE_DESIGN.md §3.5, §12 item 5).
//
// WHY A HISTOGRAM AND NOT A FILTERED LINE LIST.  §3.5 consumes Planck means
// κ_P(T_r; κ_λ) at two radiation temperatures, in the optically-thin limit.
// A Planck mean integrates κ_λ against a Planck function that varies over
// ~10^3 cm^-1 while line widths are ~10^-1 cm^-1, so the *line shape*
// (γ_air, γ_self, n_air, δ_air — the bulk of every 160-byte record) does not
// enter: only line position and temperature-dependent strength do.  The
// HITRAN temperature scaling
//
//   S(T) = S296 · [Q(296)/Q(T)] · exp(-c2·E''/T)/exp(-c2·E''/296)
//                              · [1-exp(-c2·ν/T)]/[1-exp(-c2·ν/296)]
//
// factors into a per-line temperature-INDEPENDENT amplitude
//
//   A = S296 / { exp(-c2·E''/296) · [1-exp(-c2·ν/296)] }
//
// times factors depending only on (E'', ν, T).  Binning in (ν, E'') and
// accumulating ΣA, ΣA·E'', ΣA·E''², ΣA·ν per cell therefore reproduces
// Σ S(T) at ANY temperature from the cell moments — no temperature grid is
// baked in, and the result stays analytically differentiable in T, which is
// what §3.5's certified F′(T) enclosure needs.  Reconstruction uses the
// cell's exact ΣA-weighted mean E''(and a second-moment correction), so the
// binning error is third order, not first.
//
// A cutoff on S296 would be catastrophic here and is deliberately absent:
// HITEMP's high-E'' lines are astronomically weak at 296 K and dominant at
// flame temperatures (the first record of the 50–150 cm^-1 H2O segment has
// E'' = 17632 cm^-1, S296 = 9e-59, and gains ~32 orders of magnitude by
// 2000 K).  Any pruning must be applied to reconstructed S(T_max) downstream,
// never to S296, and never in this pass.
//
// Build (standalone; not part of the library build):
//   clang++ -O3 -std=c++17 -o /tmp/hitemp_reduce tools/hitemp_reduce.cpp
// Run (streams .par records on stdin):
//   bzcat 02_HITEMP2024.par.bz2 | /tmp/hitemp_reduce --mol 2 --out co2.hist
//   for z in H2O/*.zip; do unzip -p "$z"; done | /tmp/hitemp_reduce --mol 1 --out h2o.hist

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace {

constexpr double kC2      = 1.4387769;   // second radiation constant, cm*K
constexpr double kTref    = 296.0;       // HITRAN reference temperature, K
constexpr double kPi      = 3.141592653589793238462643383279502884;

// Bin widths.  Both are moment-corrected at reconstruction, so these control
// cell count far more than accuracy.
constexpr double kNuBinWidth = 25.0;     // cm^-1
constexpr double kEBinWidth  = 50.0;     // cm^-1
constexpr int    kMaxIso     = 12;

struct Cell {
	double sumA    = 0.0;   // Σ A
	double sumA_E  = 0.0;   // Σ A·E''
	double sumA_E2 = 0.0;   // Σ A·E''^2
	double sumA_nu = 0.0;   // Σ A·ν
	uint64_t count = 0;
};

struct VisibleTailCell {
	double sumAStimTail = 0.0;
	uint64_t count = 0;
};

// Fixed-width field parse.  The HITRAN 160-char layout is positional, so a
// bounded manual scan is both faster and stricter than strtod on a copy.
inline double ParseFixed( const char* p, int len )
{
	char buf[24];
	if ( len >= (int)sizeof( buf ) ) return NAN;
	std::memcpy( buf, p, (size_t)len );
	buf[len] = '\0';
	char* end = nullptr;
	const double v = std::strtod( buf, &end );
	if ( end == buf ) return NAN;
	return v;
}

inline int ParseIsoChar( char c )
{
	// HITRAN encodes isotopologue 10,11,12 as '0','A','B'.
	if ( c >= '1' && c <= '9' ) return c - '0';
	if ( c == '0' ) return 10;
	if ( c == 'A' || c == 'a' ) return 11;
	if ( c == 'B' || c == 'b' ) return 12;
	return -1;
}

double VisibleVoigtProbabilityUpper( double nu, double gammaAir, double gammaSelf,
	double nAir, double deltaAir, double bandLo, double bandHi, int molecule )
{
	// At one atmosphere the line centre can move by the tabulated air shift.
	// Expanding in both directions is conservative for any sign convention.
	const double shift = std::fabs( deltaAir );
	const double centreLo = nu-shift;
	const double centreHi = nu+shift;
	if ( centreHi >= bandLo && centreLo <= bandHi ) return 1.0;
	const double centre = centreHi < bandLo ? centreHi : centreLo;
	const double distance = centreHi < bandLo ? bandLo-centre : centre-bandHi;
	if ( distance <= 0.0 ) return 1.0;

	// For non-negative HITRAN exponents both certified-temperature ratios are
	// below one, so one is a cheap conservative upper.  Negative exponents are
	// unusual and take the exact hot-end maximum.
	const double temperatureScale = nAir >= 0.0 ? 1.0 : std::pow(kTref/2500.0,nAir);
	const double gamma = std::max(std::fabs(gammaAir),std::fabs(gammaSelf))*temperatureScale;
	// Use the lightest isotopologue mass for each molecule and the hottest
	// certified temperature to upper-bound every Doppler standard deviation.
	constexpr double kBoltzmann = 1.380649e-23;
	constexpr double kAtomicMass = 1.66053906660e-27;
	constexpr double kLightSpeed = 299792458.0;
	// Deliberately round the lightest actual isotopologue mass downward, and
	// the pressure-shifted wavenumber upward, so Doppler sigma is one-sided.
	const double mass = ( molecule == 1 ? 17.0 : 43.0 )*kAtomicMass;
	const double sigma = (nu+shift)*
		std::sqrt(kBoltzmann*2500.0/(mass*kLightSpeed*kLightSpeed));
	const double split = 0.5*distance;
	// Chebyshev bounds the Gaussian displacement without an erfc per source
	// line.  The Cauchy term is interval width times its maximum density; the
	// expanded interval's nearest point is exactly distance/2 from the centre.
	const double gaussianTail = sigma > 0.0 ?
		std::min(1.0,4.0*sigma*sigma/(distance*distance)) : 0.0;
	const double halfDistanceSquared = split*split;
	// Maximize gamma/(split^2+gamma^2) over every actual width in
	// [0,gammaMax]; it peaks at gamma=split rather than necessarily at the
	// widest line.
	const double densityWidth = std::min(gamma,split);
	const double lorentzProbability = densityWidth > 0.0 ?
		std::min(1.0,(bandHi-bandLo+distance)*densityWidth/
			(kPi*(halfDistanceSquared+densityWidth*densityWidth))) : 0.0;
	return std::min(1.0,std::max(0.0,gaussianTail+lorentzProbability));
}

} // namespace

int main( int argc, char** argv )
{
	int wantMol = -1;
	std::string outPath;
	std::string visibleOutPath;
	std::string visibleTailOutPath;
	double visibleLoWn = NAN;
	double visibleHiWn = NAN;
	for ( int i = 1; i < argc; ++i ) {
		if ( !std::strcmp( argv[i], "--mol" ) && i + 1 < argc )      wantMol = std::atoi( argv[++i] );
		else if ( !std::strcmp( argv[i], "--out" ) && i + 1 < argc ) outPath = argv[++i];
		else if ( !std::strcmp( argv[i], "--visible-out" ) && i + 1 < argc ) visibleOutPath = argv[++i];
		else if ( !std::strcmp( argv[i], "--visible-tail-out" ) && i + 1 < argc ) visibleTailOutPath = argv[++i];
		else if ( !std::strcmp( argv[i], "--visible-lo" ) && i + 1 < argc ) visibleLoWn = std::atof( argv[++i] );
		else if ( !std::strcmp( argv[i], "--visible-hi" ) && i + 1 < argc ) visibleHiWn = std::atof( argv[++i] );
		else { std::fprintf( stderr, "unknown arg: %s\n", argv[i] ); return 2; }
	}
	const bool emitVisible = !visibleOutPath.empty() && !visibleTailOutPath.empty();
	if ( wantMol < 0 || outPath.empty() ||
	     ( emitVisible && ( !std::isfinite( visibleLoWn ) || !std::isfinite( visibleHiWn ) ||
	                        visibleLoWn <= 0.0 || visibleHiWn <= visibleLoWn ) ) ||
	     ( visibleOutPath.empty() != visibleTailOutPath.empty() ) ||
	     ( !emitVisible && ( std::isfinite( visibleLoWn ) || std::isfinite( visibleHiWn ) ) ) ) {
		std::fprintf( stderr, "usage: hitemp_reduce --mol <id> --out <file> "
		                      "[--visible-out <file> --visible-tail-out <file> "
		                      "--visible-lo <cm-1> --visible-hi <cm-1>]\n" );
		return 2;
	}

	std::unordered_map<uint64_t, Cell> cells;
	cells.reserve( 1u << 22 );
	std::unordered_map<uint64_t, Cell> visibleCells;
	if ( emitVisible ) visibleCells.reserve( 1u << 19 );
	std::unordered_map<uint64_t, VisibleTailCell> visibleTailCells;
	if ( emitVisible ) visibleTailCells.reserve( 1u << 14 );

	// Running diagnostics: everything the provenance record needs to state
	// what was read.  When requested, visibleCells is populated from exact
	// line centres before spectral binning; a cell straddling a wavelength
	// boundary can therefore never hide an in-band line.
	uint64_t nRead = 0, nKept = 0, nBadMol = 0, nBadParse = 0, nBadIso = 0;
	double nuMin = 1e300, nuMax = -1e300, eMin = 1e300, eMax = -1e300;
	uint64_t nVisible = 0;             // lines inside 12820.5–26315.8 cm^-1
	double   sumA_visible = 0.0;
	uint64_t isoCount[kMaxIso + 1] = {0};

	// Block-buffered stdin; records are fixed length but line endings vary.
	constexpr size_t kBufSize = 1u << 22;
	std::vector<char> buf( kBufSize );
	std::string pending;
	pending.reserve( 512 );

	auto handleRecord = [&]( const char* rec, size_t len ) {
		if ( len < 60 ) { ++nBadParse; return; }
		++nRead;
		const int mol = (int)ParseFixed( rec + 0, 2 );
		if ( mol != wantMol ) { ++nBadMol; return; }
		const int iso = ParseIsoChar( rec[2] );
		if ( iso < 1 || iso > kMaxIso ) { ++nBadIso; return; }

		const double nu   = ParseFixed( rec + 3,  12 );
		const double s296 = ParseFixed( rec + 15, 10 );
		const double elow = ParseFixed( rec + 45, 10 );
		const double gammaAir = ParseFixed( rec + 35, 5 );
		const double gammaSelf = ParseFixed( rec + 40, 5 );
		const double nAir = ParseFixed( rec + 55, 4 );
		const double deltaAir = ParseFixed( rec + 59, 8 );
		if ( !std::isfinite( nu ) || !std::isfinite( s296 ) || !std::isfinite( elow ) ||
		     ( emitVisible && ( !std::isfinite(gammaAir) || !std::isfinite(gammaSelf) ||
		                          !std::isfinite(nAir) || !std::isfinite(deltaAir) ) ) ) {
			++nBadParse; return;
		}
		if ( nu <= 0.0 || s296 <= 0.0 ) { ++nBadParse; return; }
		// HITRAN marks unknown lower-state energy as -1.
		const double e = ( elow < 0.0 ) ? 0.0 : elow;

		// Temperature-independent amplitude.  Both reference factors are
		// evaluated in fp64; the stimulated-emission factor is ~1 in the IR
		// but matters at low ν where c2·ν/296 is O(1).
		const double boltz296 = std::exp( -kC2 * e / kTref );
		const double stim296  = 1.0 - std::exp( -kC2 * nu / kTref );
		if ( boltz296 <= 0.0 || stim296 <= 0.0 ) { ++nBadParse; return; }
		const double A = s296 / ( boltz296 * stim296 );
		if ( !std::isfinite( A ) || A <= 0.0 ) { ++nBadParse; return; }

		const uint64_t nuBin = (uint64_t)( nu / kNuBinWidth );
		const uint64_t eBin  = (uint64_t)( e  / kEBinWidth );
		const uint64_t key   = ( (uint64_t)iso << 56 ) | ( nuBin << 28 ) | eBin;

		Cell& c = cells[key];
		c.sumA    += A;
		c.sumA_E  += A * e;
		c.sumA_E2 += A * e * e;
		c.sumA_nu += A * nu;
		c.count   += 1;
		if ( emitVisible && nu >= visibleLoWn && nu <= visibleHiWn ) {
			Cell& visible = visibleCells[key];
			visible.sumA    += A;
			visible.sumA_E  += A * e;
			visible.sumA_E2 += A * e * e;
			visible.sumA_nu += A * nu;
			visible.count   += 1;
		}
		if ( emitVisible ) {
			const double tail = VisibleVoigtProbabilityUpper(
				nu,gammaAir,gammaSelf,nAir,deltaAir,visibleLoWn,visibleHiWn,wantMol);
			const double stimulatedUpper = 1.0-std::exp(-kC2*nu/300.0);
			const double weighted = A*stimulatedUpper*tail;
			if ( !std::isfinite(weighted) || weighted < 0.0 ) { ++nBadParse; return; }
			const uint64_t tailKey = ((uint64_t)iso << 56) | eBin;
			VisibleTailCell& tailCell = visibleTailCells[tailKey];
			tailCell.sumAStimTail += weighted;
			tailCell.count += 1;
		}

		++nKept;
		++isoCount[iso];
		if ( nu < nuMin ) nuMin = nu;
		if ( nu > nuMax ) nuMax = nu;
		if ( e  < eMin  ) eMin  = e;
		if ( e  > eMax  ) eMax  = e;
		if ( emitVisible && nu >= visibleLoWn && nu <= visibleHiWn ) {
			++nVisible;
			sumA_visible += A;
		}
	};

	size_t got = 0;
	while ( ( got = std::fread( buf.data(), 1, kBufSize, stdin ) ) > 0 ) {
		size_t start = 0;
		for ( size_t i = 0; i < got; ++i ) {
			if ( buf[i] == '\n' ) {
				if ( !pending.empty() ) {
					pending.append( buf.data() + start, i - start );
					// strip CR
					while ( !pending.empty() && ( pending.back() == '\r' ) ) pending.pop_back();
					handleRecord( pending.data(), pending.size() );
					pending.clear();
				} else {
					size_t len = i - start;
					while ( len > 0 && buf[start + len - 1] == '\r' ) --len;
					handleRecord( buf.data() + start, len );
				}
				start = i + 1;
			}
		}
		if ( start < got ) pending.append( buf.data() + start, got - start );
		if ( ( nRead % 50000000u ) == 0 && nRead > 0 ) {
			std::fprintf( stderr, "  ... %llu records, %zu cells\n",
			              (unsigned long long)nRead, cells.size() );
		}
	}
	if ( !pending.empty() ) {
		while ( !pending.empty() && pending.back() == '\r' ) pending.pop_back();
		if ( !pending.empty() ) handleRecord( pending.data(), pending.size() );
	}

	// Emit keys in numeric order.  unordered_map traversal order differs by
	// standard library and insertion order, while the derived bytes are part
	// of the adopted source identity.
	auto writeHistogram = [&]( const std::string& path,
	                           const std::unordered_map<uint64_t, Cell>& histogram,
	                           const char* kind ) -> bool {
		FILE* f = std::fopen( path.c_str(), "wb" );
		if ( !f ) { std::fprintf( stderr, "cannot open %s\n", path.c_str() ); return false; }
		std::fprintf( f, "# hitemp_reduce v1 spectral-energy histogram\n" );
		std::fprintf( f, "# histogram_kind %s\n", kind );
		std::fprintf( f, "# molecule_id %d\n", wantMol );
		std::fprintf( f, "# nu_bin_width_cm-1 %.6f\n", kNuBinWidth );
		std::fprintf( f, "# e_bin_width_cm-1 %.6f\n", kEBinWidth );
		std::fprintf( f, "# c2_cm_K %.7f\n", kC2 );
		std::fprintf( f, "# t_ref_K %.1f\n", kTref );
		std::fprintf( f, "# records_read %llu\n", (unsigned long long)nRead );
		std::fprintf( f, "# records_kept %llu\n", (unsigned long long)nKept );
		std::fprintf( f, "# rejected_other_molecule %llu\n", (unsigned long long)nBadMol );
		std::fprintf( f, "# rejected_bad_iso %llu\n", (unsigned long long)nBadIso );
		std::fprintf( f, "# rejected_unparseable %llu\n", (unsigned long long)nBadParse );
		std::fprintf( f, "# nu_min_cm-1 %.6f\n", nuMin );
		std::fprintf( f, "# nu_max_cm-1 %.6f\n", nuMax );
		std::fprintf( f, "# elow_min_cm-1 %.4f\n", eMin );
		std::fprintf( f, "# elow_max_cm-1 %.4f\n", eMax );
		if ( emitVisible ) {
			std::fprintf( f, "# visible_band_lo_cm-1 %.17g\n", visibleLoWn );
			std::fprintf( f, "# visible_band_hi_cm-1 %.17g\n", visibleHiWn );
			std::fprintf( f, "# visible_band_line_count %llu\n", (unsigned long long)nVisible );
			std::fprintf( f, "# visible_band_sumA %.17g\n", sumA_visible );
		}
		for ( int i = 1; i <= kMaxIso; ++i ) {
			if ( isoCount[i] ) std::fprintf( f, "# iso_%d_lines %llu\n", i, (unsigned long long)isoCount[i] );
		}
		std::fprintf( f, "# cells %zu\n", histogram.size() );
		std::fprintf( f, "# columns iso nu_bin e_bin sumA meanE meanE2 meanNu count\n" );
		std::vector<uint64_t> keys;
		keys.reserve( histogram.size() );
		for ( const auto& entry : histogram ) keys.push_back( entry.first );
		std::sort( keys.begin(), keys.end() );
		for ( const uint64_t key : keys ) {
			const Cell& c = histogram.at( key );
			const int      iso   = (int)( key >> 56 );
			const uint64_t nuBin = ( key >> 28 ) & 0x0FFFFFFFull;
			const uint64_t eBin  = key & 0x0FFFFFFFull;
			std::fprintf( f, "%d %llu %llu %.17g %.10g %.10g %.10g %llu\n",
			              iso, (unsigned long long)nuBin, (unsigned long long)eBin,
			              c.sumA, c.sumA_E / c.sumA, c.sumA_E2 / c.sumA, c.sumA_nu / c.sumA,
			              (unsigned long long)c.count );
		}
		std::fclose( f );
		return true;
	};
	if ( !writeHistogram( outPath, cells, "all_lines" ) ) return 1;
	if ( emitVisible && !writeHistogram( visibleOutPath, visibleCells, "exact_line_center_band" ) ) return 1;
	if ( emitVisible ) {
		FILE* tailFile = std::fopen(visibleTailOutPath.c_str(),"wb");
		if ( !tailFile ) { std::fprintf(stderr,"cannot open %s\n",visibleTailOutPath.c_str()); return 1; }
		std::fprintf(tailFile,"# hitemp_reduce v1 conservative visible Voigt-tail basis\n");
		std::fprintf(tailFile,"# histogram_kind all_line_voigt_band_probability_upper\n");
		std::fprintf(tailFile,"# molecule_id %d\n",wantMol);
		std::fprintf(tailFile,"# pressure_Pa 101325\n");
		std::fprintf(tailFile,"# temperature_domain_K 300 2500\n");
		std::fprintf(tailFile,"# visible_band_lo_cm-1 %.17g\n",visibleLoWn);
		std::fprintf(tailFile,"# visible_band_hi_cm-1 %.17g\n",visibleHiWn);
		std::fprintf(tailFile,"# cells %zu\n",visibleTailCells.size());
		std::fprintf(tailFile,"# columns iso e_bin sumA_stimulated_upper_times_voigt_probability_upper count\n");
		std::vector<uint64_t> tailKeys;
		for ( const auto& entry : visibleTailCells ) tailKeys.push_back(entry.first);
		std::sort(tailKeys.begin(),tailKeys.end());
		for ( const uint64_t key : tailKeys ) {
			const VisibleTailCell& cell = visibleTailCells.at(key);
			std::fprintf(tailFile,"%d %llu %.17g %llu\n",(int)(key>>56),
				(unsigned long long)(key&0x00FFFFFFFFFFFFFFull),cell.sumAStimTail,
				(unsigned long long)cell.count);
		}
		std::fclose(tailFile);
	}

	std::fprintf( stderr, "read %llu records, kept %llu, %zu cells -> %s\n",
	              (unsigned long long)nRead, (unsigned long long)nKept, cells.size(), outPath.c_str() );
	if ( emitVisible ) {
		std::fprintf( stderr, "selected %llu exact-centre visible lines, %zu cells -> %s\n",
		              (unsigned long long)nVisible, visibleCells.size(), visibleOutPath.c_str() );
		std::fprintf( stderr, "bounded all-line visible Voigt leakage in %zu cells -> %s\n",
		              visibleTailCells.size(),visibleTailOutPath.c_str() );
	}
	return 0;
}
