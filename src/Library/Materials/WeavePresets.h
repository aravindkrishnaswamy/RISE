//////////////////////////////////////////////////////////////////////
//
//  WeavePresets.h - `weave` pattern enum + cell coverage field and
//    `fabric` preset table for `weave_material`
//    (docs/CLOTH_FABRIC_DESIGN.md).
//
//  `WeavePatternKind` is the structural draft (which thread family is
//  on top per cell, with an exact mean coverage); `WeavePreset` is an
//  appearance starting point that seeds all slots including `weave`
//  (explicitly authored slots win).
//
//  `WeaveCoverageAt` smooths across cell boundaries (bilinear
//  smoothstep over kWeaveEdgeSoftness, preserving the pattern mean)
//  and fades to the pattern mean as footprintUV exceeds cell size
//  (footprintUV == 0 disables the fade).
//
//  Preset shape parameters follow Sadeghi et al. 2013 Table II ranges
//  with representative dyes substituted.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WEAVE_PRESETS_
#define WEAVE_PRESETS_

#include "../Utilities/Color/Color.h"
#include <cstring>
#include <cmath>

namespace RISE
{
	namespace Implementation
	{
		//! Which thread family is on top, per cell of the repeating unit.
		//! Order matches the chunk descriptor's `enumValues` and
		//! `WeavePatternNamesText()`.
		enum WeavePatternKind
		{
			eWeavePlain = 0,	///< 1/1 -- warp and weft alternate every cell
			eWeaveTwill21,		///< 2/1 twill -- warp floats over two, under one
			eWeaveTwill31,		///< 3/1 twill -- the denim wale
			eWeaveSatin5,		///< 5-harness satin, step 2 -- long warp floats
			eWeaveCustom		///< the `coverage` scalar painter supplies the field
		};

		//! The enum's value list as ONE string, so the chunk descriptor,
		//! the diagnostics and the docs cannot drift.
		inline const char* WeavePatternNamesText()
		{
			return "plain, twill_2_1, twill_3_1, satin_5, custom";
		}

		inline const char* WeavePatternText( const WeavePatternKind k )
		{
			switch( k ) {
				case eWeaveTwill21:  return "twill_2_1";
				case eWeaveTwill31:  return "twill_3_1";
				case eWeaveSatin5:   return "satin_5";
				case eWeaveCustom:   return "custom";
				case eWeavePlain:
				default:             return "plain";
			}
		}

		//! Case-sensitive lookup; an unrecognised or null spelling falls
		//! back to `plain`, which is also the descriptor's default.
		inline WeavePatternKind LookupWeavePattern( const char* name )
		{
			if( !name || !name[0] )                          return eWeavePlain;
			if( std::strcmp( name, "twill_2_1" ) == 0 )      return eWeaveTwill21;
			if( std::strcmp( name, "twill_3_1" ) == 0 )      return eWeaveTwill31;
			if( std::strcmp( name, "satin_5" )   == 0 )      return eWeaveSatin5;
			if( std::strcmp( name, "custom" )    == 0 )      return eWeaveCustom;
			return eWeavePlain;
		}

		//! Thin-cloth transmission mode (docs/CLOTH_FABRIC_DESIGN.md): `none`
		//! keeps ScattersFullSphere/CouldLightPassThrough false; `thin` enables
		//! delta gap + diffuse transmission lobes.
		enum WeaveTransmissionKind
		{
			eWeaveTransmissionNone = 0,
			eWeaveTransmissionThin
		};

		inline const char* WeaveTransmissionNamesText()
		{
			return "none, thin";
		}

		inline const char* WeaveTransmissionText( const WeaveTransmissionKind k )
		{
			return ( k == eWeaveTransmissionThin ) ? "thin" : "none";
		}

		inline WeaveTransmissionKind LookupWeaveTransmission( const char* name )
		{
			if( name && std::strcmp( name, "thin" ) == 0 ) {
				return eWeaveTransmissionThin;
			}
			return eWeaveTransmissionNone;
		}

		//! Exact mean warp fraction of the repeating unit (minification fade
		//! target; `custom` returns 0.5 and does not fade).
		inline Scalar WeavePatternMeanCoverage( const WeavePatternKind k )
		{
			switch( k ) {
				case eWeaveTwill21:  return Scalar( 2.0 / 3.0 );
				case eWeaveTwill31:  return Scalar( 3.0 / 4.0 );
				case eWeaveSatin5:   return Scalar( 4.0 / 5.0 );
				case eWeaveCustom:   return Scalar( 0.5 );
				case eWeavePlain:
				default:             return Scalar( 0.5 );
			}
		}

		inline int WeavePositiveMod( const int a, const int m )
		{
			const int r = a % m;
			return ( r < 0 ) ? r + m : r;
		}

		//! True if warp is on top in cell (i, j); mean matches WeavePatternMeanCoverage.
		inline bool WeaveCellWarpOnTop( const WeavePatternKind k, const int i, const int j )
		{
			switch( k ) {
				case eWeaveTwill21:  return WeavePositiveMod( i - j, 3 ) < 2;
				case eWeaveTwill31:  return WeavePositiveMod( i - j, 4 ) < 3;
				case eWeaveSatin5:   return WeavePositiveMod( i - 2 * j, 5 ) != 0;
				case eWeaveCustom:   return true;		// unreachable: custom binds a painter
				case eWeavePlain:
				default:             return WeavePositiveMod( i + j, 2 ) == 0;
			}
		}

		//! Half-width (in cells) of the smooth yarn-edge transition centered on
		//! each cell boundary; normalized kernel preserves exact pattern mean.
		const Scalar kWeaveEdgeSoftness = Scalar( 0.22 );

		inline Scalar WeaveSmoothStep( const Scalar a, const Scalar b, const Scalar x )
		{
			if( !( b > a ) ) {
				return ( x >= b ) ? Scalar( 1 ) : Scalar( 0 );
			}
			Scalar t = ( x - a ) / ( b - a );
			t = ( t < 0 ) ? Scalar( 0 ) : ( t > 1 ? Scalar( 1 ) : t );
			return t * t * ( Scalar( 3 ) - Scalar( 2 ) * t );
		}

		//! Smoothed warp-coverage field in [0, 1] for built-in patterns.
		//! Half-cell shift places bilinear seams on cell boundaries; fades to
		//! pattern mean as footprintUV grows (0 = unavailable, no fade).
		inline Scalar WeaveCoverageAt(
			const WeavePatternKind kind,
			const Scalar u, const Scalar v,
			const Scalar cellsPerUV,
			const Scalar footprintUV )
		{
			const Scalar mean = WeavePatternMeanCoverage( kind );
			if( !( cellsPerUV > 0 ) ) {
				return mean;
			}

			const Scalar x = u * cellsPerUV - Scalar( 0.5 );
			const Scalar y = v * cellsPerUV - Scalar( 0.5 );
			const Scalar fx = std::floor( x );
			const Scalar fy = std::floor( y );
			const int    ix = (int)fx;
			const int    iy = (int)fy;

			const Scalar bx = WeaveSmoothStep( Scalar( 0.5 ) - kWeaveEdgeSoftness,
			                                   Scalar( 0.5 ) + kWeaveEdgeSoftness, x - fx );
			const Scalar by = WeaveSmoothStep( Scalar( 0.5 ) - kWeaveEdgeSoftness,
			                                   Scalar( 0.5 ) + kWeaveEdgeSoftness, y - fy );

			const Scalar c00 = WeaveCellWarpOnTop( kind, ix,     iy     ) ? Scalar( 1 ) : Scalar( 0 );
			const Scalar c10 = WeaveCellWarpOnTop( kind, ix + 1, iy     ) ? Scalar( 1 ) : Scalar( 0 );
			const Scalar c01 = WeaveCellWarpOnTop( kind, ix,     iy + 1 ) ? Scalar( 1 ) : Scalar( 0 );
			const Scalar c11 = WeaveCellWarpOnTop( kind, ix + 1, iy + 1 ) ? Scalar( 1 ) : Scalar( 0 );

			const Scalar cov = ( c00 * ( Scalar( 1 ) - bx ) + c10 * bx ) * ( Scalar( 1 ) - by )
			                 + ( c01 * ( Scalar( 1 ) - bx ) + c11 * bx ) * by;

			if( !( footprintUV > 0 ) ) {
				return cov;
			}
			const Scalar cell = Scalar( 1 ) / cellsPerUV;
			const Scalar fade = WeaveSmoothStep( cell * Scalar( 0.5 ), cell * Scalar( 2.0 ), footprintUV );
			return cov + ( mean - cov ) * fade;
		}

		//! Per-thread-family appearance parameters (angles in radians).
		struct WeaveThreadPreset
		{
			RISEPel		color;			///< dye tinting volume + diffuse transmission lobes
			Scalar		ior;			///< eta_k
			Scalar		width;			///< beta_k, longitudinal width (rad)
			Scalar		azimuth;		///< gamma_k, azimuthal width (rad)
			Scalar		kd;				///< isotropic volume-scattering fraction
			Scalar		tilt;			///< float tilt out of surface plane (rad, clamped to [-0.17, 0.17])
			Scalar		transmit;		///< k_t: volume-budget share sent to diffuse transmission, [0,1]
		};

		struct WeavePreset
		{
			const char*				name;
			bool					setsSlots;		///< false only for `custom`
			WeavePatternKind		weave;
			Scalar					weaveScale;		///< cells per UV unit (thread-count scale)
			Scalar					gap;			///< hole fraction (also delta-transmission aperture when `thin`)
			WeaveTransmissionKind	transmission;
			WeaveThreadPreset		warp;
			WeaveThreadPreset		weft;
			const char*				note;
		};

		//! Built-in fabric presets (order matches WeavePresetNamesText()).
		//! `tilt` is clamped to [-0.17, 0.17] rad (~10 deg) so the fibre-axis
		//! coordinate pole stays past 80 deg; `weaveScale` is thread-count-based
		//! per UV unit so normal framing relies on WeaveCoverageAt's mean fade.
		inline const WeavePreset* WeavePresetTable( unsigned int& count )
		{
			static const WeavePreset kPresets[] = {
				{ "denim", true, eWeaveTwill31, 2500.0, 0.02, eWeaveTransmissionNone,
				  /* warp */ { RISEPel( 0.062, 0.084, 0.175 ), 1.46, 0.28, 1.25, 0.35, 0.0, 0.0 },
				  /* weft */ { RISEPel( 0.640, 0.600, 0.520 ), 1.46, 0.30, 1.30, 0.35, 0.0, 0.0 },
				  "3/1 twill; indigo warp over undyed weft -- the wale IS the 3/1 draft, not a rotation field" },

				{ "silk", true, eWeaveSatin5, 8000.0, 0.0, eWeaveTransmissionThin,
				  /* warp */ { RISEPel( 0.640, 0.570, 0.320 ), 1.345, 0.110, 1.10, 0.20,  0.08, 0.35 },
				  /* weft */ { RISEPel( 0.520, 0.460, 0.255 ), 1.345, 0.300, 1.30, 0.30, -0.08, 0.35 },
				  "Table II (b) crepe de chine: a flat warp against a twisted weft, on a 5-harness float" },

				{ "satin", true, eWeaveSatin5, 6000.0, 0.0, eWeaveTransmissionThin,
				  /* warp */ { RISEPel( 0.520, 0.190, 0.150 ), 1.539, 0.100, 0.90, 0.10,  0.09, 0.15 },
				  /* weft */ { RISEPel( 0.430, 0.155, 0.120 ), 1.539, 0.400, 1.40, 0.70, -0.09, 0.15 },
				  "Table II (c) charmeuse: the flattest, shiniest float in the set" },

				{ "linen", true, eWeavePlain, 2000.0, 0.10, eWeaveTransmissionThin,
				  /* warp */ { RISEPel( 0.740, 0.680, 0.560 ), 1.46, 0.240, 1.30, 0.30, 0.0, 0.25 },
				  /* weft */ { RISEPel( 0.720, 0.660, 0.540 ), 1.46, 0.240, 1.30, 0.30, 0.0, 0.25 },
				  "Table II (a) plain flax; the one preset with a real gap -- plain linen is not watertight" },

				{ "custom", false, eWeavePlain, 2500.0, 0.0, eWeaveTransmissionNone,
				  /* warp */ { RISEPel( 1.0, 1.0, 1.0 ), 1.46, 0.25, 1.0, 0.30, 0.0, 0.0 },
				  /* weft */ { RISEPel( 1.0, 1.0, 1.0 ), 1.46, 0.25, 1.0, 0.30, 0.0, 0.0 },
				  "no preset; every slot is the author's own" }
			};
			count = (unsigned int)( sizeof( kPresets ) / sizeof( kPresets[0] ) );
			return kPresets;
		}

		inline const char* WeavePresetNamesText()
		{
			return "denim, silk, satin, linen, custom";
		}

		//! Case-sensitive lookup; falls back to `custom` by name (independent of table order).
		inline const WeavePreset& LookupWeavePreset( const char* name )
		{
			unsigned int n = 0;
			const WeavePreset* t = WeavePresetTable( n );
			if( name && name[0] ) {
				for( unsigned int i = 0; i < n; ++i ) {
					if( std::strcmp( t[i].name, name ) == 0 ) return t[i];
				}
			}
			for( unsigned int i = 0; i < n; ++i ) {
				if( std::strcmp( t[i].name, "custom" ) == 0 ) return t[i];
			}
			return t[n - 1];	// unreachable while the table has a `custom` row
		}
	}
}

#endif
