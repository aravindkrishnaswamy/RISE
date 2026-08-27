//////////////////////////////////////////////////////////////////////
//
//  HairMedullaProfileGen.cpp - Bakes the medulla scattering profile
//    table consumed by the Yan et al. 2017 TTs / TRTs fur lobes.
//
//  Writes src/Library/Materials/HairMedullaProfile_LUTData.cpp
//  directly (there is no intermediate binary artefact -- unlike the
//  Jakob-Hanika LUT, whose `.coeff` file predates the baked-header
//  approach and is still consumed by other tooling; here the C++
//  emission is the only consumer, so the extra Python hop would buy
//  nothing).  Run from the PROJECT ROOT:
//
//      make -C build/make/rise tools
//      ./bin/tools/HairMedullaProfileGen
//
//  The exact invocation and the git commit are recorded in the
//  generated file's banner.  Re-run only when the model changes; the
//  output is deterministic (fixed per-cell seeds, a self-contained
//  PRNG), so a re-run on an unchanged tool reproduces the file
//  byte-for-byte on any platform with IEEE doubles.
//
//  ------------------------------------------------------------------
//  WHAT IS SIMULATED
//  ------------------------------------------------------------------
//
//  One crossing of the medulla, modelled as an infinitely long,
//  unit-radius, NON-ABSORBING cylinder of scattering density sigma
//  with a Henyey-Greenstein phase function of anisotropy g.  Photons
//  enter travelling perpendicular to the cylinder axis (theta == 0) at
//  signed impact parameter b, and are tracked in FULL 3D (the walk's
//  own longitudinal angle evolves with every scattering event and
//  correctly slows the photon's progress across the cross-section).
//  A photon leaves when it reaches the cylinder wall.
//
//  Recorded, CONDITIONAL ON AT LEAST ONE SCATTERING EVENT:
//    * a histogram of the exit azimuthal deflection d_phi, and
//    * the variance of the exit longitudinal angle theta.
//  The ballistic (zero-scatter) photons are deliberately excluded:
//  the runtime accounts for them exactly, as the surviving
//  UNSCATTERED lobe, with weight exp(-tau*sqrt(1-b^2)) -- which is by
//  construction the same fraction this simulation drops.
//
//  ------------------------------------------------------------------
//  DELIBERATE SIMPLIFICATIONS vs Yan et al. 2017  (honest accounting)
//  ------------------------------------------------------------------
//
//   1. NORMAL-INCIDENCE SIMULATION, PATH-STRETCHED AT RUNTIME.  The
//      table is generated with theta_entry == 0 only.  An inclined
//      entry (theta_t != 0) is handled at lookup time purely by
//      stretching the optical depth (tau = sigma_m * 2 kappa /
//      cos theta_t), which is EXACT for the first free flight and an
//      approximation thereafter, because a photon entering inclined
//      starts its walk from a different longitudinal angle.  Yan's
//      profiles carry the inclination as a real table axis.  The cost
//      of that axis here would be a 4-D table; the cost of the
//      approximation is a slightly under-spread longitudinal profile
//      at grazing inclination.
//
//   2. NON-ABSORBING MEDULLA.  Yan's medulla carries its own
//      absorption; ours does not.  The runtime instead applies the
//      CORTEX absorption sigma_a over the whole chord (medulla
//      included), which is what the pre-Phase-3 model already did --
//      chosen so that turning the medulla on redistributes energy
//      between lobes without changing the total, which is what makes
//      the white-furnace gate structurally safe.  A separately
//      authorable medulla absorption would need its own tier and its
//      own furnace argument.
//
//   3. NO CORTEX BOUNDARY FOR THE SCATTERED PART -- AND THEREFORE NO
//      TOTAL INTERNAL REFLECTION.  A photon that leaves the medulla
//      still has to cross the cortex and refract out of the fibre; we
//      ignore that boundary entirely and apply the tabulated
//      deflection directly around the parent lobe's exit azimuth.
//      This is NOT a second-order re-shaping, and it would be
//      dishonest to call it one: at eta = 1.55 the critical angle is
//      40.2 degrees, so only 1 - cos(40.2) ~ 24 % of an isotropic
//      medulla exit lies inside the escape cone.  The rest would in
//      reality be TIR'd back into the fibre and re-emerge somewhere
//      else (or in a higher order).  The consequence is that the
//      scattered lobes' broad TAILS are materially wider -- up to
//      ~4x the directional density -- than a re-refracted model would
//      give, most visibly at the large-sigma_m end where this bake's
//      longVariance saturates at the exactly-isotropic 0.467.
//      ENERGY is untouched (nothing is created; the split is exact),
//      which is precisely why no furnace gate can see this: it is an
//      accuracy caveat on the lobe SHAPE, and closing it means giving
//      the simulation a cortex boundary.
//
//   4. A 3-TAP CIRCULAR SMOOTHING [1 2 1]/4 is applied to each
//      histogram before normalisation, to suppress Monte-Carlo noise
//      at a cost of roughly one bin (11.25 degrees) of extra width.
//      It is renormalised afterwards, so it cannot change energy.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	// ---- table extents (must match HairMedullaProfile.h's doc) -----
	const unsigned int kNumB     = 8;
	const unsigned int kNumTau   = 10;
	const unsigned int kNumG     = 5;
	const unsigned int kPhiBins  = 32;

	const double kTauMin = 0.02;
	const double kTauMax = 16.0;
	const double kGMax   = 0.8;

	//! Photons per cell.  8 * 10 * 5 = 400 cells.
	const long kPhotonsPerCell = 150000;

	//! Minimum bin value, as a fraction of the cell's MEAN bin value.
	//! See the floor block in SimulateCell.
	const double kFloorFraction = 1e-4;

	//! Hard stop on one photon's walk.  Only reachable in the thickest
	//! cells; a truncated photon is recorded at its current direction
	//! (which is already diffuse there, so the bias is negligible) and
	//! counted, so the bake log can show the count is small.
	const int kMaxScatterEvents = 20000;

	const double kPI    = 3.14159265358979323846;
	const double kTwoPI = 6.28318530717958647692;

	//////////////////////////////////////////////////////////////////
	//  Self-contained deterministic PRNG (PCG32).  Deliberately NOT
	//  RISE's RandomNumberGenerator: the bake must reproduce
	//  bit-for-bit regardless of what the engine's RNG does later.
	//////////////////////////////////////////////////////////////////
	struct PCG32
	{
		unsigned long long state;
		unsigned long long inc;

		void Seed( const unsigned long long initState, const unsigned long long initSeq )
		{
			state = 0u;
			inc = ( initSeq << 1u ) | 1u;
			NextUInt();
			state += initState;
			NextUInt();
		}

		unsigned int NextUInt()
		{
			const unsigned long long old = state;
			state = old * 6364136223846793005ULL + inc;
			const unsigned int xorshifted = (unsigned int)( ( ( old >> 18u ) ^ old ) >> 27u );
			const unsigned int rot = (unsigned int)( old >> 59u );
			return ( xorshifted >> rot ) | ( xorshifted << ( ( 32u - rot ) & 31u ) );
		}

		//! Uniform in [0, 1).
		double Next()
		{
			return (double)NextUInt() * ( 1.0 / 4294967296.0 );
		}
	};

	//! Format a double as a VALID C++ float literal.  `%.9g` alone is
	//! not enough: it renders 16.0 as "16" and 0.0 as "0", and
	//! "16f" / "0f" are not legal C++ (a bare integer cannot take the
	//! `f` suffix).  Nine significant digits round-trip a float
	//! exactly, so no precision is lost by going through decimal.
	std::string FloatLit( const double v )
	{
		char buf[64];
		snprintf( buf, sizeof( buf ), "%.9g", v );
		std::string s( buf );
		if( s.find( '.' ) == std::string::npos &&
		    s.find( 'e' ) == std::string::npos &&
		    s.find( 'E' ) == std::string::npos &&
		    s.find( "inf" ) == std::string::npos &&
		    s.find( "nan" ) == std::string::npos ) {
			s += ".0";
		}
		return s + "f";
	}

	//! Henyey-Greenstein: cosine of the scattering deflection.
	double SampleHGCos( const double g, const double u )
	{
		if( fabs( g ) < 1e-4 ) {
			return 1.0 - 2.0 * u;
		}
		const double s = ( 1.0 - g * g ) / ( 1.0 - g + 2.0 * g * u );
		const double c = ( 1.0 + g * g - s * s ) / ( 2.0 * g );
		return ( c < -1.0 ) ? -1.0 : ( ( c > 1.0 ) ? 1.0 : c );
	}

	//! Rotate `w` (unit) by (cosTheta, phi) about itself.
	void ScatterDirection( double w[3], const double cosTheta, const double phi )
	{
		const double sinTheta = sqrt( ( 1.0 - cosTheta * cosTheta ) > 0 ? ( 1.0 - cosTheta * cosTheta ) : 0.0 );

		// Build an orthonormal basis around w (Duff et al. branchless
		// variant -- the sign trick keeps it stable at w.z ~ -1).
		const double sign = ( w[2] >= 0.0 ) ? 1.0 : -1.0;
		const double a = -1.0 / ( sign + w[2] );
		const double b = w[0] * w[1] * a;
		const double t1[3] = { 1.0 + sign * w[0] * w[0] * a, sign * b, -sign * w[0] };
		const double t2[3] = { b, sign + w[1] * w[1] * a, -w[1] };

		const double cp = cos( phi ), sp = sin( phi );
		double n[3];
		for( int i = 0; i < 3; i++ ) {
			n[i] = sinTheta * cp * t1[i] + sinTheta * sp * t2[i] + cosTheta * w[i];
		}
		const double len = sqrt( n[0]*n[0] + n[1]*n[1] + n[2]*n[2] );
		if( len > 0 ) {
			w[0] = n[0] / len; w[1] = n[1] / len; w[2] = n[2] / len;
		}
	}

	//! Distance from `p` (inside the unit disc) to the boundary along
	//! the unit 2-vector `d`.
	double DistanceToWall( const double py, const double pz, const double dy, const double dz )
	{
		const double pd = py * dy + pz * dz;
		const double pp = py * py + pz * pz;
		const double disc = pd * pd + ( 1.0 - pp );
		if( !( disc > 0 ) ) {
			return 0.0;
		}
		const double s = -pd + sqrt( disc );
		return ( s > 0 ) ? s : 0.0;
	}

	//! Wrap into [-pi, pi).
	double WrapPi( double x )
	{
		while( x >= kPI )  { x -= kTwoPI; }
		while( x < -kPI )  { x += kTwoPI; }
		return x;
	}

	struct CellResult
	{
		double		hist[kPhiBins];		//!< normalised density per radian
		double		longVariance;
		long		scatteredExits;
		long		ballistic;
		long		truncated;
	};

	//! Simulate one (b, tau, g) cell.
	//!
	//! Coordinates: x is the FIBRE AXIS; the medulla cross-section is
	//! (y, z).  Photons enter at (y, z) = (-sqrt(1-b^2), b) travelling
	//! along +y, i.e. direction (0, 1, 0) -- theta == 0, phi == 0 --
	//! so the exit azimuth IS the deflection.
	void SimulateCell( const double b, const double tau, const double g,
	                   const unsigned long long seed, CellResult& out )
	{
		const double sigma = 0.5 * tau;		// tau is the DIAMETRAL depth
		long counts[kPhiBins];
		for( unsigned int i = 0; i < kPhiBins; i++ ) { counts[i] = 0; }

		double sumTheta2 = 0;
		out.scatteredExits = 0;
		out.ballistic = 0;
		out.truncated = 0;

		PCG32 rng;
		rng.Seed( seed, 0x9E3779B97F4A7C15ULL );

		// The b == 1 node is TANGENTIAL: the true chord length is 0, so
		// there is nothing to simulate.  It is nudged to 0.999 (chord
		// 0.089) purely so the cell holds a defined shape.  That shape
		// is NOT the b == 1 shape -- there is no such thing -- and it
		// does participate in interpolation near b ~ 0.95.  Harmless
		// for energy, because the runtime weights that whole region by
		// 1 - exp(-tau*sqrt(1-b^2)) -> 0.
		const double bClamped = ( b > 0.999 ) ? 0.999 : b;
		const double y0 = -sqrt( 1.0 - bClamped * bClamped );

		for( long n = 0; n < kPhotonsPerCell; n++ )
		{
			double py = y0, pz = bClamped;
			double w[3] = { 0.0, 1.0, 0.0 };
			int nScatter = 0;
			bool truncated = false;

			for( ;; )
			{
				const double cosTheta = sqrt( ( w[1]*w[1] + w[2]*w[2] ) > 0 ? ( w[1]*w[1] + w[2]*w[2] ) : 0.0 );
				if( !( cosTheta > 1e-9 ) ) {
					// Running (essentially) along the axis: it will never
					// reach the wall before scattering again.  Force a
					// scattering event rather than looping forever.
					if( nScatter >= kMaxScatterEvents ) { truncated = true; break; }
					nScatter++;
					const double cs = SampleHGCos( g, rng.Next() );
					ScatterDirection( w, cs, kTwoPI * rng.Next() );
					continue;
				}
				const double dy = w[1] / cosTheta;
				const double dz = w[2] / cosTheta;

				const double u = rng.Next();
				const double t3D = -log( ( u > 1e-16 ) ? u : 1e-16 ) / sigma;
				const double tXS = t3D * cosTheta;

				const double wall = DistanceToWall( py, pz, dy, dz );
				if( tXS >= wall ) {
					break;						// leaves the medulla
				}
				py += tXS * dy;
				pz += tXS * dz;

				if( nScatter >= kMaxScatterEvents ) { truncated = true; break; }
				nScatter++;
				const double cs = SampleHGCos( g, rng.Next() );
				ScatterDirection( w, cs, kTwoPI * rng.Next() );
			}

			if( truncated ) { out.truncated++; }

			if( nScatter == 0 ) {
				out.ballistic++;
				continue;						// accounted for by the caller
			}

			const double theta = asin( ( w[0] < -1.0 ) ? -1.0 : ( ( w[0] > 1.0 ) ? 1.0 : w[0] ) );
			const double phi = atan2( w[2], w[1] );
			sumTheta2 += theta * theta;

			int bin = (int)floor( ( WrapPi( phi ) + kPI ) / kTwoPI * (double)kPhiBins );
			if( bin < 0 ) { bin = 0; }
			if( bin >= (int)kPhiBins ) { bin = (int)kPhiBins - 1; }
			counts[bin]++;
			out.scatteredExits++;
		}

		if( out.scatteredExits <= 0 ) {
			// No photon scattered at all (the thinnest, most grazing
			// cells).  A uniform profile is the only defensible answer,
			// and the runtime weights this cell by 1 - exp(-tau') ~ 0.
			for( unsigned int i = 0; i < kPhiBins; i++ ) {
				out.hist[i] = 1.0 / kTwoPI;
			}
			out.longVariance = 0;
			return;
		}

		// 3-tap circular smoothing, then normalise to a density.
		double sm[kPhiBins];
		for( unsigned int i = 0; i < kPhiBins; i++ ) {
			const unsigned int lo = ( i + kPhiBins - 1 ) % kPhiBins;
			const unsigned int hi = ( i + 1 ) % kPhiBins;
			sm[i] = 0.25 * (double)counts[lo] + 0.5 * (double)counts[i] + 0.25 * (double)counts[hi];
		}

		// STRICTLY-POSITIVE FLOOR.  In the thinnest, most strongly
		// forward-scattering cells a far-side bin can end up with ZERO
		// photons -- an artefact of finite sampling, not a claim that
		// the true density vanishes there.  A zero bin would make the
		// runtime report density 0 for a direction the lobe really can
		// reach, so every bin is floored at kFloorFraction of the cell
		// mean before normalisation (a Laplace-style prior).  The
		// most it can move is kPhiBins * kFloorFraction of the lobe --
		// 0.32 % at 32 bins x 1e-4 -- and that bound is only approached
		// if EVERY bin binds the floor.  On the shipped table 20 bins
		// out of 12800 bind it, so the realised shift is ~5e-6.
		{
			double raw = 0;
			for( unsigned int i = 0; i < kPhiBins; i++ ) { raw += sm[i]; }
			const double floorVal = kFloorFraction * raw / (double)kPhiBins;
			for( unsigned int i = 0; i < kPhiBins; i++ ) {
				if( sm[i] < floorVal ) { sm[i] = floorVal; }
			}
		}

		double total = 0;
		for( unsigned int i = 0; i < kPhiBins; i++ ) { total += sm[i]; }
		const double binWidth = kTwoPI / (double)kPhiBins;
		for( unsigned int i = 0; i < kPhiBins; i++ ) {
			out.hist[i] = ( total > 0 ) ? ( sm[i] / ( total * binWidth ) ) : ( 1.0 / kTwoPI );
		}

		// SECOND MOMENT ABOUT ZERO, which is the variance here ONLY
		// because E[theta] is exactly 0.  That is a real symmetry, not
		// an approximation: the walk enters along +y with theta = 0 and
		// the HG phase function is azimuthally symmetric, so the whole
		// process is invariant under x -> -x (x being the fibre axis),
		// which makes the exit-theta distribution exactly even.  If a
		// future bake ever introduces an inclined entry (simplification
		// 1), this line must subtract the mean.
		out.longVariance = sumTheta2 / (double)out.scatteredExits;
	}

	double BAt( const unsigned int i )   { return (double)i / (double)( kNumB - 1 ); }
	double GAt( const unsigned int i )   { return -kGMax + 2.0 * kGMax * (double)i / (double)( kNumG - 1 ); }
	double TauAt( const unsigned int i )
	{
		const double f = (double)i / (double)( kNumTau - 1 );
		return kTauMin * pow( kTauMax / kTauMin, f );
	}
}

int main( int argc, char** argv )
{
	std::string outPath = "src/Library/Materials/HairMedullaProfile_LUTData.cpp";
	for( int i = 1; i < argc; i++ ) {
		if( strcmp( argv[i], "--output" ) == 0 && i + 1 < argc ) {
			outPath = argv[++i];
		} else {
			fprintf( stderr, "usage: %s [--output <path>]\n", argv[0] );
			return 1;
		}
	}

	const unsigned int perCell = kPhiBins + 1;
	const unsigned int nCells  = kNumB * kNumTau * kNumG;
	std::vector<float> data( (size_t)nCells * perCell, 0.0f );

	long totalTruncated = 0;
	long totalScattered = 0;
	double worstLongVar = 0;

	printf( "HairMedullaProfileGen: %u cells (%u b x %u tau x %u g), %ld photons each\n",
	        nCells, kNumB, kNumTau, kNumG, kPhotonsPerCell );

	for( unsigned int ib = 0; ib < kNumB; ib++ )
	{
		for( unsigned int it = 0; it < kNumTau; it++ )
		{
			for( unsigned int ig = 0; ig < kNumG; ig++ )
			{
				CellResult r;
				// Seed depends ONLY on the cell index, so the bake is
				// reproducible and cell-order independent.
				const unsigned long long seed =
					( (unsigned long long)ib * 1000003ULL ) +
					( (unsigned long long)it * 10007ULL ) +
					( (unsigned long long)ig * 101ULL ) + 20260827ULL;

				SimulateCell( BAt( ib ), TauAt( it ), GAt( ig ), seed, r );

				const size_t base = ( ( (size_t)ib * kNumTau + it ) * kNumG + ig ) * perCell;
				for( unsigned int k = 0; k < kPhiBins; k++ ) {
					data[base + k] = (float)r.hist[k];
				}
				data[base + kPhiBins] = (float)r.longVariance;

				totalTruncated += r.truncated;
				totalScattered += r.scatteredExits;
				if( r.longVariance > worstLongVar ) { worstLongVar = r.longVariance; }
			}
		}
		printf( "  b row %u/%u done\n", ib + 1, kNumB );
		fflush( stdout );
	}

	printf( "  scattered exits: %ld ; truncated walks: %ld ; max longitudinal variance: %g\n",
	        totalScattered, totalTruncated, worstLongVar );

	FILE* f = fopen( outPath.c_str(), "wb" );
	if( !f ) {
		fprintf( stderr, "HairMedullaProfileGen: cannot open `%s` for writing\n", outPath.c_str() );
		return 1;
	}

	fprintf( f,
		"//////////////////////////////////////////////////////////////////////\n"
		"//\n"
		"//  HairMedullaProfile_LUTData.cpp - Baked medulla scattering\n"
		"//    profiles for the Yan et al. 2017 TTs / TRTs fur lobes.\n"
		"//\n"
		"//  AUTO-GENERATED by tools/HairMedullaProfileGen.cpp.  DO NOT EDIT\n"
		"//  BY HAND.  Regenerate from the PROJECT ROOT with:\n"
		"//\n"
		"//      make -C build/make/rise tools\n"
		"//      ./bin/tools/HairMedullaProfileGen\n"
		"//\n"
		"//  Layout, extents, simulation model and the list of deliberate\n"
		"//  simplifications versus Yan et al. 2017 are documented in\n"
		"//  HairMedullaProfile.h and in the generator's file header.\n"
		"//\n"
		"//  Cell index = ( ( ib * %u + it ) * %u + ig ), %u floats per cell:\n"
		"//  %u azimuthal density values (per radian, at bin centres,\n"
		"//  conditional on at least one scattering event) followed by the\n"
		"//  exit longitudinal-angle variance in radians^2.\n"
		"//\n"
		"//  Bake statistics: %ld photons per cell, %ld scattered exits\n"
		"//  total, %ld truncated walks, max longitudinal variance %g.\n"
		"//\n"
		"//////////////////////////////////////////////////////////////////////\n"
		"\n"
		"#include \"pch.h\"\n"
		"#include \"HairMedullaProfile.h\"\n"
		"\n"
		"namespace RISE\n"
		"{\n"
		"\textern const unsigned int kHairMedullaNumB      = %uu;\n"
		"\textern const unsigned int kHairMedullaNumTau    = %uu;\n"
		"\textern const unsigned int kHairMedullaNumG      = %uu;\n"
		"\textern const unsigned int kHairMedullaPhiBins   = %uu;\n"
		"\textern const unsigned int kHairMedullaNumFloats = %uu;\n"
		"\n"
		"\textern const float kHairMedullaTauMin = %s;\n"
		"\textern const float kHairMedullaTauMax = %s;\n"
		"\textern const float kHairMedullaGMax   = %s;\n"
		"\n"
		"\textern const float kHairMedullaProfileData[] = {\n",
		kNumTau, kNumG, perCell, kPhiBins,
		kPhotonsPerCell, totalScattered, totalTruncated, worstLongVar,
		kNumB, kNumTau, kNumG, kPhiBins,
		(unsigned int)data.size(),
		FloatLit( kTauMin ).c_str(), FloatLit( kTauMax ).c_str(), FloatLit( kGMax ).c_str() );

	for( size_t i = 0; i < data.size(); i += 6 ) {
		fprintf( f, "\t\t" );
		for( size_t j = i; j < i + 6 && j < data.size(); j++ ) {
			fprintf( f, "%s,%s", FloatLit( (double)data[j] ).c_str(),
			         ( j + 1 < i + 6 && j + 1 < data.size() ) ? " " : "" );
		}
		fprintf( f, "\n" );
	}

	fprintf( f, "\t};\n}\n" );
	fclose( f );

	printf( "HairMedullaProfileGen: wrote %s (%zu floats, %zu bytes of table data)\n",
	        outPath.c_str(), data.size(), data.size() * sizeof( float ) );
	return 0;
}
