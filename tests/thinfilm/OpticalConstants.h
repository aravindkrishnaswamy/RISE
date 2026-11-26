//////////////////////////////////////////////////////////////////////
//
//  OpticalConstants.h - Loads the tabulated complex-IOR (n + i*k)
//    optical-constant files under colors/thinfilm/ into interpolatable
//    tables, keyed by (metal, oxide), and assembles an air/oxide/metal
//    ThinFilmStack for the P1-A reference math.  Also embeds the
//    standard CIE D65 relative SPD (the one piece of data this header
//    adds — there is no samplable per-nm D65 in the renderer).
//
//    This is part of the Phase-1 thin-film validation gate
//    (docs/THIN_FILM_INTERFERENCE.md §8/§11, piece P1-C).  Header-only,
//    NO renderer dependency beyond the reference math under
//    tests/thinfilm/.  std::complex<double> throughout (this is the
//    oracle path, clarity over speed).
//
//    Interpolation mirrors PiecewiseLinearScalarPainter exactly:
//    binary-search linear interpolation between (nm, value) samples,
//    with FLAT-CLAMP extrapolation to the endpoint value outside the
//    sample range.  The data files (colors/thinfilm/*.{n,k}) carry
//    explicit boundary samples at/near 380 and 780 nm, so the flat
//    clamp is at worst a few-nm hold of the nearest measured value —
//    it never invents an interior value.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THINFILM_OPTICALCONSTANTS_H
#define THINFILM_OPTICALCONSTANTS_H

#include <algorithm>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "ThinFilmStack.h"

namespace RISE
{
	namespace ThinFilmReference
	{
		//! A 2-column (nm, value) table loaded from a colors/thinfilm
		//! *.n / *.k file.  Linear interpolation between samples,
		//! flat-clamp extrapolation at the band edges — bit-for-bit the
		//! PiecewiseLinearScalarPainter::EvalAtNM convention so the
		//! oracle reads the same numbers the renderer will.
		class SpectralTable
		{
		public:
			struct Sample { double nm; double value; };

			SpectralTable() {}

			//! Loads a bare 2-column "nm value" text file with the same
			//! `fscanf("%lf %lf")` loop the scene parser uses.  Returns
			//! false (and leaves the table empty) if the file can't be
			//! opened or contains no numeric pairs.
			bool LoadFromFile( const std::string& path )
			{
				samples.clear();
				FILE* f = std::fopen( path.c_str(), "r" );
				if( !f ) {
					return false;
				}
				double nm = 0.0, val = 0.0;
				while( std::fscanf( f, "%lf %lf", &nm, &val ) == 2 ) {
					Sample s;
					s.nm = nm;
					s.value = val;
					samples.push_back( s );
				}
				std::fclose( f );
				// Defensive sort: the files are ascending, but a stray
				// out-of-order line would silently corrupt the binary
				// search otherwise.
				std::sort( samples.begin(), samples.end(),
					[]( const Sample& a, const Sample& b ) { return a.nm < b.nm; } );
				return !samples.empty();
			}

			bool Empty() const { return samples.empty(); }
			size_t Size() const { return samples.size(); }
			double MinNm() const { return samples.empty() ? 0.0 : samples.front().nm; }
			double MaxNm() const { return samples.empty() ? 0.0 : samples.back().nm; }

			//! Linear interpolation + flat-clamp extrapolation.  Mirrors
			//! PiecewiseLinearScalarPainter::EvalAtNM.
			double EvalAtNM( double nm ) const
			{
				if( samples.empty() ) return 0.0;
				if( samples.size() == 1 ) return samples[0].value;
				if( nm <= samples.front().nm ) return samples.front().value;
				if( nm >= samples.back().nm  ) return samples.back().value;
				const auto it = std::lower_bound(
					samples.begin(), samples.end(), nm,
					[]( const Sample& s, double n ) { return s.nm < n; } );
				const Sample& hi = *it;
				const Sample& lo = *( it - 1 );
				if( hi.nm == lo.nm ) return lo.value;	// duplicate-nm guard (no div-by-zero)
				const double t = ( nm - lo.nm ) / ( hi.nm - lo.nm );
				return lo.value + t * ( hi.value - lo.value );
			}

		private:
			std::vector<Sample> samples;
		};

		//! A complex-index material: its n and k tables sampled together.
		class IndexedMaterial
		{
		public:
			IndexedMaterial() {}

			//! Loads `<base>.n` and `<base>.k` (e.g. base =
			//! "colors/thinfilm/substrates/Ti").  Returns false if
			//! either file fails to load.
			bool LoadFromBase( const std::string& base )
			{
				const bool okN = nTable.LoadFromFile( base + ".n" );
				const bool okK = kTable.LoadFromFile( base + ".k" );
				return okN && okK;
			}

			//! Complex index N = n + i*k at the given wavelength.  k is
			//! forced non-negative (the absorbing convention shared with
			//! ThinFilmStack::MakeIndex).
			Complex IndexAtNM( double nm ) const
			{
				return MakeIndex( nTable.EvalAtNM( nm ), kTable.EvalAtNM( nm ) );
			}

			bool Valid() const { return !nTable.Empty() && !kTable.Empty(); }
			const SpectralTable& N() const { return nTable; }
			const SpectralTable& K() const { return kTable; }

		private:
			SpectralTable nTable;
			SpectralTable kTable;
		};

		//! A substrate metal + its heat-tint oxide, with human-readable
		//! names for printing the ladder.  The metal is the semi-infinite
		//! substrate; the oxide is the thin film grown on it.
		struct MetalOxidePair
		{
			std::string			metalName;		//!< e.g. "Ti"
			std::string			oxideName;		//!< e.g. "TiO2"
			IndexedMaterial		metal;			//!< substrate complex index
			IndexedMaterial		oxide;			//!< film complex index

			bool Valid() const { return metal.Valid() && oxide.Valid(); }

			//! Assembles an air / oxide(thickness) / metal single-film
			//! stack at a given wavelength.  Both indices are sampled at
			//! `nm` (dispersion is honoured per wavelength), the ambient
			//! is air (N = 1), and the film thickness is in nm.
			Stack BuildStack( double thickness_nm, double nm ) const
			{
				return MakeSingleFilmStack(
					Air(),
					oxide.IndexAtNM( nm ), thickness_nm,
					metal.IndexAtNM( nm ) );
			}

			//! Bare-substrate (no film) stack at a wavelength — the d->0
			//! limit reference.
			Stack BuildBareStack( double nm ) const
			{
				return MakeBareStack( Air(), metal.IndexAtNM( nm ) );
			}
		};

		//! Loads all four canonical (metal, oxide) pairs from
		//! colors/thinfilm/ given the repository root (the directory that
		//! contains `colors/`).  Pairings per colors/thinfilm/README.md:
		//! Ti/TiO2, Steel/Fe3O4, Ta/Ta2O5, Nb/Nb2O5.  Returns the pairs
		//! that loaded successfully; a caller checks the count.
		inline std::vector<MetalOxidePair> LoadCanonicalPairs( const std::string& repoRoot )
		{
			struct PairSpec { const char* metal; const char* oxide; };
			static const PairSpec kSpecs[] = {
				{ "Ti",    "TiO2"  },
				{ "Steel", "Fe3O4" },
				{ "Ta",    "Ta2O5" },
				{ "Nb",    "Nb2O5" },
			};

			std::string root = repoRoot;
			if( !root.empty() && root.back() != '/' ) {
				root.push_back( '/' );
			}

			std::vector<MetalOxidePair> out;
			for( const PairSpec& spec : kSpecs ) {
				MetalOxidePair p;
				p.metalName = spec.metal;
				p.oxideName = spec.oxide;
				const std::string metalBase = root + "colors/thinfilm/substrates/" + spec.metal;
				const std::string oxideBase = root + "colors/thinfilm/oxides/"      + spec.oxide;
				if( p.metal.LoadFromBase( metalBase ) && p.oxide.LoadFromBase( oxideBase ) ) {
					out.push_back( std::move( p ) );
				}
			}
			return out;
		}

		//////////////////////////////////////////////////////////////////
		//  Standard CIE D65 relative spectral power distribution.
		//
		//  This is the ONLY data this header embeds (there is no
		//  samplable per-nm D65 SPD in the renderer; ColorUtils carries
		//  only the CMFs).  Values are the CIE published D65 relative SPD
		//  at 5 nm steps, normalised to 100 at 560 nm (the standard
		//  convention).  Range 380..780 nm to match the CIE 1931 CMF
		//  support that ColorUtils::XYZFromNM covers (the integral is
		//  taken over exactly the CMF range so D65 and the CMFs share
		//  support — see §8 / the swatch normalisation).
		//
		//  Source: CIE 15:2004, Table T.1 (standard illuminant D65
		//  relative SPD).  Public reference data.
		//////////////////////////////////////////////////////////////////
		namespace D65_DATA
		{
			static const int    min_nm = 380;
			static const int    max_nm = 780;
			static const int    step_nm = 5;
			static const int    count = ( max_nm - min_nm ) / step_nm + 1;	// 81

			// CIE D65 relative SPD, 380..780 nm @ 5 nm, S(560) = 100.
			static const double S[ count ] = {
				 49.9755,  52.3118,  54.6482,  68.7015,  82.7549,  87.1204,  91.4860,
				 92.4589,  93.4318,  90.0570,  86.6823,  95.7736, 104.8650, 110.9360,
				117.0080, 117.4100, 117.8120, 116.3360, 114.8610, 115.3920, 115.9230,
				112.3670, 108.8110, 109.0820, 109.3540, 108.5780, 107.8020, 106.2960,
				104.7900, 106.2390, 107.6890, 106.0470, 104.4050, 104.2250, 104.0460,
				102.0230, 100.0000,  98.1671,  96.3342,  96.0611,  95.7880,  92.2368,
				 88.6856,  89.3459,  90.0062,  89.8026,  89.5991,  88.6489,  87.6987,
				 85.4936,  83.2886,  83.4939,  83.6992,  81.8630,  80.0268,  80.1207,
				 80.2146,  81.2462,  82.2778,  80.2810,  78.2842,  74.0027,  69.7213,
				 70.6652,  71.6091,  72.9790,  74.3490,  67.9765,  61.6040,  65.7448,
				 69.8856,  72.4863,  75.0870,  69.3398,  63.5927,  55.0054,  46.4182,
				 56.6118,  66.8054,  65.0941,  63.3828
			};

			//! Linear interpolation of the D65 SPD at an arbitrary
			//! wavelength, with flat-clamp at the band edges (matching the
			//! optical-constant tables' extrapolation convention).
			inline double SampleAtNM( double nm )
			{
				if( nm <= min_nm ) return S[0];
				if( nm >= max_nm ) return S[count - 1];
				const double pos = ( nm - min_nm ) / double( step_nm );
				const int    i0 = static_cast<int>( pos );
				const int    i1 = i0 + 1;
				const double t = pos - i0;
				return S[i0] + t * ( S[i1] - S[i0] );
			}
		}
	}
}

#endif
