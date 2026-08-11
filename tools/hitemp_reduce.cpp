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
#include <unordered_map>

namespace {

constexpr double kC2      = 1.4387769;   // second radiation constant, cm*K
constexpr double kTref    = 296.0;       // HITRAN reference temperature, K
constexpr int    kRecLen  = 160;         // HITRAN 160-char record + newline

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

} // namespace

int main( int argc, char** argv )
{
	int wantMol = -1;
	std::string outPath;
	for ( int i = 1; i < argc; ++i ) {
		if ( !std::strcmp( argv[i], "--mol" ) && i + 1 < argc )      wantMol = std::atoi( argv[++i] );
		else if ( !std::strcmp( argv[i], "--out" ) && i + 1 < argc ) outPath = argv[++i];
		else { std::fprintf( stderr, "unknown arg: %s\n", argv[i] ); return 2; }
	}
	if ( wantMol < 0 || outPath.empty() ) {
		std::fprintf( stderr, "usage: hitemp_reduce --mol <id> --out <file>\n" );
		return 2;
	}

	std::unordered_map<uint64_t, Cell> cells;
	cells.reserve( 1u << 22 );

	// Running diagnostics: everything the provenance record needs to state
	// what was read, and the un-pruned visible-band totals that the §12
	// item-5 380–780 nm upper bound must be computed from.
	uint64_t nRead = 0, nKept = 0, nBadMol = 0, nBadParse = 0, nBadIso = 0;
	double nuMin = 1e300, nuMax = -1e300, eMin = 1e300, eMax = -1e300;
	uint64_t nVisible = 0;             // lines inside 12820.5–26315.8 cm^-1
	double   sumA_visible = 0.0;
	uint64_t isoCount[kMaxIso + 1] = {0};

	constexpr double kVisLoWn = 1.0e7 / 780.0;   // 780 nm
	constexpr double kVisHiWn = 1.0e7 / 380.0;   // 380 nm

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
		if ( !std::isfinite( nu ) || !std::isfinite( s296 ) || !std::isfinite( elow ) ) { ++nBadParse; return; }
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

		++nKept;
		++isoCount[iso];
		if ( nu < nuMin ) nuMin = nu;
		if ( nu > nuMax ) nuMax = nu;
		if ( e  < eMin  ) eMin  = e;
		if ( e  > eMax  ) eMax  = e;
		if ( nu >= kVisLoWn && nu <= kVisHiWn ) { ++nVisible; sumA_visible += A; }
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

	// Emit: a text header of diagnostics/provenance inputs, then one line per
	// occupied cell.  Text keeps the intermediate auditable; the downstream
	// certified record generator owns the canonical binary/CBOR encoding.
	FILE* f = std::fopen( outPath.c_str(), "wb" );
	if ( !f ) { std::fprintf( stderr, "cannot open %s\n", outPath.c_str() ); return 1; }
	std::fprintf( f, "# hitemp_reduce v1 spectral-energy histogram\n" );
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
	std::fprintf( f, "# visible_band_lo_cm-1 %.4f\n", kVisLoWn );
	std::fprintf( f, "# visible_band_hi_cm-1 %.4f\n", kVisHiWn );
	std::fprintf( f, "# visible_band_line_count %llu\n", (unsigned long long)nVisible );
	std::fprintf( f, "# visible_band_sumA %.17g\n", sumA_visible );
	for ( int i = 1; i <= kMaxIso; ++i ) {
		if ( isoCount[i] ) std::fprintf( f, "# iso_%d_lines %llu\n", i, (unsigned long long)isoCount[i] );
	}
	std::fprintf( f, "# cells %zu\n", cells.size() );
	std::fprintf( f, "# columns iso nu_bin e_bin sumA meanE meanE2 meanNu count\n" );
	for ( const auto& kv : cells ) {
		const uint64_t key = kv.first;
		const Cell& c = kv.second;
		const int      iso   = (int)( key >> 56 );
		const uint64_t nuBin = ( key >> 28 ) & 0x0FFFFFFFull;
		const uint64_t eBin  = key & 0x0FFFFFFFull;
		std::fprintf( f, "%d %llu %llu %.17g %.10g %.10g %.10g %llu\n",
		              iso, (unsigned long long)nuBin, (unsigned long long)eBin,
		              c.sumA, c.sumA_E / c.sumA, c.sumA_E2 / c.sumA, c.sumA_nu / c.sumA,
		              (unsigned long long)c.count );
	}
	std::fclose( f );

	std::fprintf( stderr, "read %llu records, kept %llu, %zu cells -> %s\n",
	              (unsigned long long)nRead, (unsigned long long)nKept, cells.size(), outPath.c_str() );
	return 0;
}
