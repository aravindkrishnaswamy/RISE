//////////////////////////////////////////////////////////////////////
//
//  WeavePresets.h - The `weave` PATTERN enum (with its cell functions)
//    and the `fabric` PRESET table for `weave_material`
//    (docs/CLOTH_FABRIC_DESIGN.md Phase 2, slice P2-A).
//
//  TWO INDEPENDENT ENUMS LIVE HERE, AND CONFLATING THEM IS THE TRAP.
//
//    * `weave` (WeavePatternKind) is the STRUCTURE: which thread family
//      is on top in each cell of the repeating unit.  It is a geometric
//      fact about the cloth's construction and it has a closed-form
//      mean coverage, asserted by WeaveMaterialChunkTest.
//    * `fabric` (WeavePreset) is the APPEARANCE: a named starting point
//      that seeds every slot on the chunk INCLUDING `weave`.  Any slot
//      the author writes explicitly wins, exactly as
//      `fabric_material`'s own `Has()`-driven seeding works
//      (FabricPresets.h).
//
//  A pattern with no preset is perfectly legal (`weave twill_2_1` on a
//  `fabric custom`); a preset always implies a pattern.
//
//  THE COVERAGE FIELD IS CONTINUOUS, NOT A CELL LOOKUP, AND THAT IS
//  MEASURED RATHER THAN STYLISTIC.  9.9 gate 9b's second finding is
//  that a piecewise-constant per-cell field renders as a BLOCKY
//  CHECKERBOARD -- the discontinuity, not the amplitude, is the defect
//  (retuning the excursion from 0.42 rad to 0.11 rad produced a
//  *fainter checkerboard of the same size*).  So `WeaveCoverageAt`
//  does two things no naive cell lookup does:
//
//    1. it SMOOTHS across cell boundaries, bilinearly blending the four
//       neighbouring cells over a narrow band (`kWeaveEdgeSoftness` of
//       a cell on each side of the boundary) so a yarn edge is a ramp
//       rather than a step;
//    2. it FADES TO THE PATTERN MEAN as the pixel footprint grows past
//       the cell size -- 5.5's `fw` idiom, made intrinsic instead of
//       left to the author's expression.  A woven surface seen from far
//       enough away IS its mean coverage, and fading to it is both the
//       correct limit and the anti-aliasing.
//
//  A footprint of 0 means "no footprint available" (secondary bounces,
//  non-mesh geometry) and correctly disengages the fade rather than
//  snapping to the mean -- the same convention `TextureFootprint::valid`
//  and the expression VM's `fw` already use.
//
//  THE PRESET NUMBERS ARE STARTING POINTS, NOT MEASUREMENTS, and the
//  same caveat FabricPresets.h carries applies with more force here:
//  Sadeghi et al. 2013's Table II is a six-fabric fit to THEIR captured
//  samples with THEIR tangent-curve machinery, and RISE's model
//  substitutes d'Eon's Mp for their Gaussian and a trimmed logistic for
//  their bare cos(phi_d/2).  What is carried over faithfully is the
//  SHAPE of the parameter set and the ranges: eta 1.345-1.539 (all
//  dielectrics), the surface longitudinal width 2.5 deg (flat satin
//  floats) to 30 deg (twisted matte threads), k_d 0.1-0.7, and the
//  recurring gamma_v == 2*gamma_s ratio that WeaveBRDF applies as a
//  derived value rather than a second authored one.  The COLOURS are
//  NOT Table II's: that table's samples are one blue linen, one yellow
//  silk and one salmon satin, which make poor generic presets, so each
//  row carries a representative dye for its fabric class instead and
//  says so.  Treat a change to any of it as an appearance change.
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

		//! The fraction of the repeating unit on which the WARP is the
		//! top thread.  This is the pattern's exact mean and it is the
		//! value `WeaveCoverageAt` fades to under minification -- both
		//! halves asserted by tests/WeaveMaterialChunkTest.cpp.
		//!
		//! `custom` has no analytic mean (the field is an authored
		//! painter), so it reports 0.5 -- the neutral split -- and the
		//! footprint fade is DISABLED for it in `WeaveCoverageAt`, since
		//! fading an authored field toward a number the author never
		//! stated would be inventing appearance.
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

		//! Positive modulus -- `%` in C++ keeps the sign of the dividend,
		//! and cell indices go negative the moment UVs do.
		inline int WeavePositiveMod( const int a, const int m )
		{
			const int r = a % m;
			return ( r < 0 ) ? r + m : r;
		}

		//! Is the WARP the top thread in cell (i, j)?  The four built-in
		//! draft patterns, each written so its mean over the repeating
		//! unit is exactly `WeavePatternMeanCoverage`.
		//!
		//!   plain       (i+j) even                    -> 1/2
		//!   twill_2_1   (i-j) mod 3 < 2               -> 2/3
		//!   twill_3_1   (i-j) mod 4 < 3               -> 3/4  (the denim wale)
		//!   satin_5     (i-2j) mod 5 != 0             -> 4/5  (long warp floats,
		//!                                                      step 2 = no two
		//!                                                      weft points adjacent)
		//!
		//! The DIAGONAL is what makes a twill a twill: the predicate
		//! depends on (i - j), so the float run marches one cell across
		//! for every cell down, which is the wale.  Satin's step of 2
		//! scatters the single weft point so no wale forms at all -- that
		//! is the whole difference between the two families, and it is
		//! one coefficient.
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

		//! Half-width of the smooth ramp at a yarn edge, in CELLS.  The
		//! transition band is twice this wide and centred on the cell
		//! boundary, so 0.22 means a yarn edge occupies the outer 22 % of
		//! each of the two cells it separates and the interior 56 % of a
		//! cell is exactly its own value.
		//!
		//! SET BY LOOKING AT A RENDER, and the first value was too small.
		//! 0.05 is what "a few percent of the cell" suggests and it is
		//! wrong for the same reason 9.9 gate 9b's checkerboard finding
		//! was: at any cell size a viewer can actually RESOLVE, a 5 %
		//! ramp is one or two pixels wide and the field reads as tiled
		//! blocks -- scenes/Tests/Materials/weave_presets.RISEscene at 22
		//! cells per UV unit made that plain.  A real yarn crossing is a
		//! ROUNDED cylinder passing over another, so the coverage should
		//! ease over a substantial fraction of the cell rather than step
		//! at its edge.
		//!
		//! It cannot go much further: at 0.5 the smoothstep pair
		//! degenerates to a full bilinear interpolation between cell
		//! centres, which erases the flat top the yarn actually has and
		//! turns every draft into the same soft lattice.  0.22 keeps a
		//! flat majority per cell and rounds the crossing.
		//!
		//! It does NOT affect the mean (a convolution with a normalised
		//! kernel preserves it), which is why the footprint fade's target
		//! and the draft's exact rational stay in agreement -- asserted
		//! by tests/WeaveMaterialChunkTest.cpp's "the SMOOTHED field
		//! averages to the same rational the fade targets".
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

		//! The warp-coverage field a_warp(u, v) in [0, 1] for a built-in
		//! pattern: the smoothed cell value, faded toward the pattern
		//! mean as the footprint grows.
		//!
		//! @param kind        the draft (never `custom` -- that binds a painter)
		//! @param u           surface texture coordinate, first axis
		//! @param v           surface texture coordinate, second axis
		//! @param cellsPerUV  `weave_scale`; cells per unit of UV
		//! @param footprintUV the pixel footprint's extent in UV units;
		//!                    0 = unavailable, which disengages the fade
		//!
		//! THE BILINEAR BLEND IS OVER CELL CENTRES, NOT CELL CORNERS.
		//! Shifting by half a cell puts the interpolation seam exactly on
		//! the cell BOUNDARY (where the yarns actually meet) instead of
		//! through the middle of a yarn, which is why the smoothing reads
		//! as a soft yarn edge rather than as a blurred pattern.
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

			// Cell centres land on integers after the half-cell shift.
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

			// Footprint fade.  `cell` is the cell's extent in UV; once one
			// pixel spans two cells the pattern is unresolvable and the
			// honest answer is its mean.
			if( !( footprintUV > 0 ) ) {
				return cov;
			}
			const Scalar cell = Scalar( 1 ) / cellsPerUV;
			const Scalar fade = WeaveSmoothStep( cell * Scalar( 0.5 ), cell * Scalar( 2.0 ), footprintUV );
			return cov + ( mean - cov ) * fade;
		}

		//! One thread family's appearance parameters.  Angular
		//! quantities are RADIANS throughout -- Sadeghi's Table II is in
		//! degrees and the conversion is done HERE, once, so no consumer
		//! has to remember which convention it is holding.
		struct WeaveThreadPreset
		{
			RISEPel		color;			///< A_k, the dye (tints the VOLUME lobe only)
			Scalar		ior;			///< eta_k
			Scalar		width;			///< beta_k, longitudinal width (rad)
			Scalar		azimuth;		///< gamma_k, azimuthal width (rad)
			Scalar		kd;				///< Sadeghi's isotropic volume-scattering fraction
			Scalar		tilt;			///< float tilt out of the surface plane (rad)
		};

		struct WeavePreset
		{
			const char*			name;
			bool				setsSlots;		///< false only for `custom`
			WeavePatternKind	weave;
			Scalar				weaveScale;		///< cells per UV unit
			Scalar				gap;
			WeaveThreadPreset	warp;
			WeaveThreadPreset	weft;
			const char*			note;			///< one line, for diagnostics and docs
		};

		//! The Phase-2 preset table.
		//!
		//! ORDER MATCHES the chunk descriptor's `enumValues` and
		//! `WeavePresetNamesText()`.  Lookup is BY NAME, so the order is
		//! presentation only.
		//!
		//! Provenance, row by row (P2_ZHU_READ 3.4 transcribes Table II
		//! in full):
		//!
		//!   denim   NOT in Table II -- no denim sample was captured.  The
		//!           3/1 twill draft and the indigo-warp / undyed-weft
		//!           split are the fabric's actual construction; the
		//!           SHAPE parameters are borrowed from Table II row (a)'s
		//!           cellulose entry (eta 1.46, k_d 0.3, gamma_s 12 deg),
		//!           because denim is cotton and (a) is the only cellulose
		//!           row.  Marked clearly rather than implied.
		//!   silk    Table II (b), silk crepe de chine: eta 1.345, a flat
		//!           class at gamma_s 5 deg / k_d 0.2 and a twisted class
		//!           at 18 deg / 0.3.  Its captured colour is a saturated
		//!           yellow; a champagne dye is substituted.
		//!   satin   Table II (c), polyester satin charmeuse (front):
		//!           eta 1.539, flat class gamma_s 2.5 deg / k_d 0.1 --
		//!           the narrowest lobe in the whole table, which is what
		//!           makes satin satin -- and a twisted class at 30 deg /
		//!           0.7.  Captured colour is salmon; a rose dye is
		//!           substituted, and both classes' albedo is kept LOW
		//!           because on a shiny satin the visible energy is the
		//!           (untinted, Fresnel-weighted) surface lobe.
		//!   linen   Table II (a), linen plain weave: eta 1.46, k_d 0.3,
		//!           gamma_s 12 deg, and a real GAP -- a plain linen weave
		//!           is not watertight, which is the one row where 9.9's
		//!           `gap` slot earns its place.  Captured colour is blue;
		//!           natural flax is substituted.
		//!
		//! TILTS ARE DELIBERATELY SMALL AND OPPOSITE across the two
		//! families on the two satin-weave rows.  Sadeghi's tangent
		//! OFFSETS run to +-35 deg, but he spends them on a multi-segment
		//! tangent CURVE per family (up to 8 segments with individual
		//! lengths); P2-A reduces that to one scalar per family
		//! (P2_ZHU_READ 4.3's stated simplification), and a single large
		//! tilt does not approximate a symmetric +-35 pair -- it just
		//! leans the whole family.  Small OPPOSITE tilts give the float
		//! asymmetry that reads as a satin face without the energy the
		//! sampler loses at latitudes a large tilt makes invisible
		//! (WeaveSPF.h's normalisation note).
		//!
		//! THEY WERE HALVED (0.12-0.14 -> 0.08-0.09) ON MEASUREMENT.  A
		//! tilt buries one family's visible face at a grazing view from
		//! the side it leans away from, and the masking term then
		//! collapses that family to zero: white silk's directional albedo
		//! fell from 0.53 at theta 80 to 0.016 at 85 with the original
		//! tilts.  That is the model behaving correctly -- you are looking
		//! at the back of the yarn -- but it put a dark band on every
		//! silhouette, and it is also what made `hemisphericalAlbedo`'s
		//! normal-incidence closed form run furthest high.  Halving keeps
		//! the float asymmetry and moves the collapse out toward the
		//! horizon.
		//!
		//! `warp_tilt`/`weft_tilt` ARE CLAMPED TO [-0.17, 0.17] rad
		//! (~10 deg), tighter than the 0.6 rad an earlier revision
		//! allowed (P2R4 review, finding P1-1).  The masking term's
		//! per-family azimuth (`WeaveBRDF::ProjectDir`) has a coordinate
		//! pole exactly where a direction approaches the fibre axis --
		//! view latitude `90 - tilt_deg` -- and at the old 0.6 rad clamp
		//! that pole sat at an ORDINARY, in-frame view angle (~55.6 deg
		//! at the clamp), producing a 48.9% brightness drop in a
		//! 0.5-degree step.  0.17 rad keeps the pole at or past 80 deg
		//! for any in-range tilt; `WeaveBRDF.cpp`'s pole-conditioned
		//! `ProjectDir` and its C1 `SmoothedRamp` masking gate remove the
		//! discontinuity itself, so this bound is defense-in-depth, not
		//! the whole fix.  Every shipped preset's tilt (0.08-0.09) is
		//! well inside the new clamp, so no preset numbers moved.
		//!
		//! WEAVE_SCALE IS THREAD-COUNT BASED, and it is the slot that
		//! decides whether this material reads as CLOTH or as printed
		//! stripes.  A shirting runs ~25-30 threads/cm, so a surface whose
		//! UV spans about a metre carries ~2500-3000 cells; a fine
		//! charmeuse or habotai two to three times that.  The defaults
		//! below are per UNIT OF UV SPAN on that reading, which puts a
		//! cell at or below pixel size in any normal framing -- exactly
		//! where `WeaveCoverageAt`'s footprint fade takes over and the
		//! draft stops being individually resolvable.  THAT IS THE POINT.
		//! What Phase 2 buys is not visible cells: it is the float
		//! DIRECTION, the two families' different lobes and their mutual
		//! shadowing.  An earlier revision shipped 36-80 here and the
		//! showcase scenes authored 170-310, at which a twill wale renders
		//! as coarse diagonal stripes -- carbon fibre, not denim.  An
		//! author with a hero close-up overrides this; nobody should have
		//! to override it to get cloth.
		inline const WeavePreset* WeavePresetTable( unsigned int& count )
		{
			static const WeavePreset kPresets[] = {
				{ "denim", true, eWeaveTwill31, 2500.0, 0.02,
				  /* warp */ { RISEPel( 0.062, 0.084, 0.175 ), 1.46, 0.28, 1.25, 0.35, 0.0 },
				  /* weft */ { RISEPel( 0.640, 0.600, 0.520 ), 1.46, 0.30, 1.30, 0.35, 0.0 },
				  "3/1 twill; indigo warp over undyed weft -- the wale IS the 3/1 draft, not a rotation field" },

				{ "silk", true, eWeaveSatin5, 8000.0, 0.0,
				  /* warp */ { RISEPel( 0.640, 0.570, 0.320 ), 1.345, 0.110, 1.10, 0.20,  0.08 },
				  /* weft */ { RISEPel( 0.520, 0.460, 0.255 ), 1.345, 0.300, 1.30, 0.30, -0.08 },
				  "Table II (b) crepe de chine: a flat warp against a twisted weft, on a 5-harness float" },

				{ "satin", true, eWeaveSatin5, 6000.0, 0.0,
				  /* warp */ { RISEPel( 0.520, 0.190, 0.150 ), 1.539, 0.100, 0.90, 0.10,  0.09 },
				  /* weft */ { RISEPel( 0.430, 0.155, 0.120 ), 1.539, 0.400, 1.40, 0.70, -0.09 },
				  "Table II (c) charmeuse: the flattest, shiniest float in the set" },

				{ "linen", true, eWeavePlain, 2000.0, 0.10,
				  /* warp */ { RISEPel( 0.740, 0.680, 0.560 ), 1.46, 0.240, 1.30, 0.30, 0.0 },
				  /* weft */ { RISEPel( 0.720, 0.660, 0.540 ), 1.46, 0.240, 1.30, 0.30, 0.0 },
				  "Table II (a) plain flax; the one preset with a real gap -- plain linen is not watertight" },

				{ "custom", false, eWeavePlain, 2500.0, 0.0,
				  /* warp */ { RISEPel( 1.0, 1.0, 1.0 ), 1.46, 0.25, 1.0, 0.30, 0.0 },
				  /* weft */ { RISEPel( 1.0, 1.0, 1.0 ), 1.46, 0.25, 1.0, 0.30, 0.0 },
				  "no preset; every slot is the author's own" }
			};
			count = (unsigned int)( sizeof( kPresets ) / sizeof( kPresets[0] ) );
			return kPresets;
		}

		inline const char* WeavePresetNamesText()
		{
			return "denim, silk, satin, linen, custom";
		}

		//! Case-sensitive lookup; an unrecognised (or null) name falls
		//! back to `custom`, found BY NAME rather than by position for
		//! the reason `LookupFabricPreset` spells out: a positional
		//! fallback becomes silently wrong the moment the table is
		//! reordered.
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
