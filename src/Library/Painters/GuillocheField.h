//////////////////////////////////////////////////////////////////////
//
//  GuillocheField.h - Pointwise guilloché height-field + oxide-dose
//  math for the procedural watch-dial painters.
//
//  This is a FAITHFUL PORT of the Python bakers it replaces
//  (scenes/FeatureBased/GuillocheWatch/dial_mesh_gen.py height_field +
//  dial_variants_gen.py FIELDS + thermal_oxide_sim.py
//  build_thickness_profile/apply_torch_pattern).  Those scripts baked
//  27-70 MB meshes and oxide PNGs offline; evaluating the SAME math
//  per-point lets `displaced_geometry` and the film-thickness
//  scalar_painter consume it live -- no bake step, GUI-tunable.
//
//  PORT NOTES (the three places the Python is NOT pointwise):
//   * `_finish` normalised by the GRID min/max.  Every field's raw
//     extremes are achieved on the dial (each composed term reaches 0
//     and 1), so the normalisation is replaced by the ANALYTIC range
//     [rawMin(), rawMax()] -- identical result, no grid.
//   * `build_mesh` mean-centred the height (z = (h - mean)*disp).  The
//     painter instead offers `centered` output ((h - 0.5)*1.0) so the
//     relief straddles its midline; the residual mean offset (the
//     field mean is not exactly 0.5) is absorbed by the dial object's
//     z position, exactly as the old mesh's arbitrary offset was.
//   * numpy `np.mod` is floor-mod; C++ fmod truncates.  floorMod()
//     below matches numpy for the negative arguments that occur left
//     of centre.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GUILLOCHE_FIELD_
#define GUILLOCHE_FIELD_

#include "../Utilities/Math3D/Math3D.h"
#include <cmath>
#include <algorithm>

namespace RISE
{
	namespace Implementation
	{
		namespace GuillocheMath
		{
			inline Scalar clamp01( const Scalar x )
			{
				return x < 0 ? Scalar(0) : ( x > 1 ? Scalar(1) : x );
			}

			inline Scalar smoothstepG( const Scalar e0, const Scalar e1, const Scalar x )
			{
				const Scalar t = clamp01( ( x - e0 ) / ( e1 - e0 ) );
				return t * t * ( Scalar(3) - Scalar(2) * t );
			}

			//! Raised plateau of carrier c in [-1,1]: 1 where |c| large.
			inline Scalar lens( const Scalar c, const Scalar e0, const Scalar e1 )
			{
				return smoothstepG( e0, e1, std::fabs( c ) );
			}

			//! Periodic groove/land stripe: |cos(2 pi arg)| plateau, land-to-land 0.5 in arg.
			inline Scalar stripe( const Scalar arg, const Scalar e0, const Scalar e1 )
			{
				return smoothstepG( e0, e1, std::fabs( std::cos( Scalar(TWO_PI) * arg ) ) );
			}

			//! [-1,1] triangle wave with period 1 (numpy `_tri`).
			inline Scalar triWave( const Scalar x )
			{
				const Scalar f = x - std::floor( x );
				return Scalar(2) * std::fabs( Scalar(2) * f - Scalar(1) ) - Scalar(1);
			}

			//! numpy floor-mod (result in [0, y) for y > 0, unlike std::fmod).
			inline Scalar floorMod( const Scalar x, const Scalar y )
			{
				return x - std::floor( x / y ) * y;
			}
		}

		//! All knobs of every pattern, defaults = the shipped bakers' defaults
		//! (dial_mesh_gen.py argparse defaults; per-pattern overrides live in the
		//! scene chunks, mirroring gen_dials.sh's blessed parameter sets).
		struct GuillocheParams
		{
			enum Pattern   { ePatUniform = 0, ePatLightning, ePatRadial, ePatIris, ePatSwirl, ePatVarwidth };
			enum CellMode  { eCellFreqBlend = 0, eCellSelect };
			enum BoltStyle { eBoltRung = 0, eBoltCube, eBoltWoven };
			enum FieldFrame{ eFrameGlobal = 0, eFrameRadial };

			Pattern    pattern;
			Scalar     radius;				//!< dial radius (world units)
			int        numArms;
			Scalar     swirl;				//!< angular lean centre->rim (rad)
			Scalar     seamJag;				//!< petal seam zig-zag amplitude (rad)
			Scalar     seamJagFreq;
			Scalar     cell;				//!< woven cell world size (land-to-land)
			Scalar     gridAmp;
			Scalar     petalAmp;
			Scalar     gridE0, gridE1;
			Scalar     petalE0, petalE1;
			Scalar     base;
			Scalar     landLevel;
			Scalar     reliefDepth;
			Scalar     centerRadius;		//!< inner fraction flattened to the flush hub
			// variant knobs
			CellMode   cellMode;
			Scalar     lightningCellScale;
			Scalar     lightningLo, lightningHi;
			Scalar     lightningRelief;
			Scalar     zigzagAmp;			//!< lightning: ray kink amplitude (rad)
			Scalar     zigzagFreq;			//!< lightning: kinks centre->rim
			Scalar     fieldCell;			//!< lightning: tight cube cell INSIDE the bolts
			FieldFrame fieldFrame;
			BoltStyle  boltStyle;
			Scalar     rungLen;				//!< lightning 'rung': block size ALONG the channel
			Scalar     rungWidth;			//!< lightning 'rung': block size ACROSS the channel
			Scalar     irisAperture;		//!< iris: hole radius as a fraction of R
			Scalar     irisSwirl;
			Scalar     irisEdge;
			Scalar     swirlTurns;			//!< swirl: log-spiral tightness

			GuillocheParams() :
				pattern( ePatUniform ),
				radius( 20.6 ),
				numArms( 12 ),
				swirl( 0.0 ),
				seamJag( 0.16 ),
				seamJagFreq( 3.0 ),
				cell( 0.9 ),
				gridAmp( 0.85 ),
				petalAmp( 0.30 ),
				gridE0( 0.12 ), gridE1( 0.5 ),
				petalE0( 0.0 ), petalE1( 0.82 ),
				base( 0.15 ),
				landLevel( 0.45 ),
				reliefDepth( 0.85 ),
				centerRadius( 0.03 ),
				cellMode( eCellFreqBlend ),
				lightningCellScale( 0.6 ),
				lightningLo( 0.30 ), lightningHi( 0.72 ),
				lightningRelief( 0.0 ),
				zigzagAmp( 0.16 ),
				zigzagFreq( 3.0 ),
				fieldCell( 0.45 ),
				fieldFrame( eFrameGlobal ),
				boltStyle( eBoltRung ),
				rungLen( 1.2 ),
				rungWidth( 1.5 ),
				irisAperture( 0.13 ),
				irisSwirl( 0.5 ),
				irisEdge( 0.6 ),
				swirlTurns( 6.0 )
			{
			}
		};

		//! Pointwise guilloché evaluation.  All methods are const + stateless ->
		//! safe to share across render threads.
		class GuillocheField
		{
		public:
			explicit GuillocheField( const GuillocheParams& p ) : m_p( p ) {}

			const GuillocheParams& Params() const { return m_p; }

			//! The pattern's torch MASK in [0,1] (the petal / bolt band) at dial
			//! (x, y) -- what thermal_oxide_sim.apply_torch_pattern modulated.
			//! For uniform/radial it is the warped petal lens (dial_mesh_gen
			//! lightning_mask); for lightning it is the zigzag ray band; other
			//! patterns reuse the petal-style term where meaningful.
			Scalar TorchMask( const Scalar x, const Scalar y ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				const Scalar r = std::hypot( x, y );
				const Scalar rho = clamp01( r / p.radius );
				const Scalar theta = std::atan2( y, x );
				if( p.pattern == GuillocheParams::ePatLightning )
				{
					const Scalar zig = p.zigzagAmp * triWave( p.zigzagFreq * rho );
					const Scalar rayc = Scalar(0.5) + Scalar(0.5) * std::cos( Scalar(p.numArms) * ( theta + zig ) );
					return smoothstepG( p.lightningLo, p.lightningHi, rayc );
				}
				const Scalar jag = p.seamJag * triWave( p.seamJagFreq * rho );
				const Scalar psi = theta + p.swirl * rho + jag;
				return lens( std::cos( Scalar(p.numArms) * psi ), p.petalE0, p.petalE1 );
			}

			//! Normalized oxide dose in [0,1] at dial (x, y): the Arrhenius /
			//! parabolic radial profile (thermal_oxide_sim.build_thickness_profile
			//! with d_center=0, d_rim=1) plus the signed torch-pattern term
			//! (apply_torch_pattern).  falloffMode: 0 linear, 1 quadratic,
			//! 2 smoothstep.  activationEa in J/mol (METAL_KINETICS).
			Scalar OxideDose( const Scalar x, const Scalar y,
			                  const int falloffMode, const Scalar activationEa,
			                  const Scalar torchAmount ) const
			{
				using namespace GuillocheMath;
				const Scalar r = std::hypot( x, y );
				const Scalar rho = clamp01( r / m_p.radius );
				Scalar heat;
				switch( falloffMode )
				{
				case 0:  heat = rho; break;
				default:
				case 1:  heat = rho * rho; break;
				case 2:  heat = rho * rho * ( Scalar(3) - Scalar(2) * rho ); break;
				}
				// d(T) = sqrt(A t) * exp(-Ea / (2 R T)); the sqrt(A t) factor and
				// the affine endpoint rescale cancel into a closed form:
				//   t = (g(T(heat)) - g(T0)) / (g(T1) - g(T0)),  g(T) = exp(-Ea/(2 R T))
				const Scalar g0 = ArrheniusG( kTMinK, activationEa );
				const Scalar g1 = ArrheniusG( kTMaxK, activationEa );
				return OxideDoseWithEndpoints( x, y, heat, activationEa, g0, g1 - g0, torchAmount );
			}

			//! The hot inner loop of OxideDose with the Ea-only endpoint
			//! constants hoisted -- GuillocheOxidePainter computes g(T0) and
			//! the span ONCE at construction (they depend only on
			//! activation_ea), turning 3 exp per render-time query into 1.
			Scalar OxideDoseWithEndpoints( const Scalar x, const Scalar y,
			                               const Scalar heat,
			                               const Scalar activationEa,
			                               const Scalar g0, const Scalar span,
			                               const Scalar torchAmount ) const
			{
				using namespace GuillocheMath;
				const Scalar T = kTMinK + heat * ( kTMaxK - kTMinK );
				const Scalar g = ArrheniusG( T, activationEa );
				const Scalar dose = ( std::fabs( span ) < Scalar(1e-300) ) ? heat : ( g - g0 ) / span;
				const Scalar mask = ( torchAmount != Scalar(0) ) ? TorchMask( x, y ) : Scalar(0);
				return clamp01( dose + torchAmount * mask );
			}

			//! The radial heat dose in [0,1] at dial (x, y) for a falloff mode
			//! (the `heat` input of OxideDoseWithEndpoints).
			Scalar HeatAt( const Scalar x, const Scalar y, const int falloffMode ) const
			{
				using namespace GuillocheMath;
				const Scalar rho = clamp01( std::hypot( x, y ) / m_p.radius );
				switch( falloffMode )
				{
				case 0:  return rho;
				default:
				case 1:  return rho * rho;
				case 2:  return rho * rho * ( Scalar(3) - Scalar(2) * rho );
				}
			}

			static Scalar ArrheniusG( const Scalar T, const Scalar activationEa )
			{
				return std::exp( -activationEa / ( Scalar(2) * kGasConstant * T ) );
			}

			//! Per-metal parabolic-oxidation activation energies (J/mol) --
			//! thermal_oxide_sim.METAL_KINETICS verbatim.
			static Scalar MetalEa( const char metal0 /* 'T'i 'N'b 'a'=Ta 'S'teel */ )
			{
				switch( metal0 )
				{
				case 'N': return Scalar(135.0e3);	// Nb
				case 'a': return Scalar(80.0e3);	// Ta (disambiguated by caller)
				case 'S': return Scalar(165.0e3);	// Steel
				default:
				case 'T': return Scalar(160.0e3);	// Ti
				}
			}

			//! Per-metal heat-tint THERMAL model for the absolute-temperature
			//! dose mode (the temper-comparison renders).  Temperature
			//! breakpoints (DEGREES CELSIUS) + the oxide's interference
			//! thickness range (nm).
			//!
			//! CALIBRATION + HONESTY NOTES (this is a comparison SHOWCASE, not
			//! metrology):
			//!  - onsetC / optLoC / optHiC / flakeC and dLo/dHi were chosen
			//!    with the Airy/CIE swatch oracle (thermal_oxide_sim) for a
			//!    legible, vivid sweep -- NOT auto-extracted from it.  dHi
			//!    here (73/88/94/93 nm) is WIDER than the oracle's blessed
			//!    first-order temper ladder (Ti ~22-38, Nb ~30-55, Ta ~26-52,
			//!    Steel ~28-56 nm); it runs each oxide to ~late first order so
			//!    the optimal-window sweep traverses the full straw->blue
			//!    range rather than the narrower monotone ladder.
			//!  - The dHi values are ISO-INTERFERENCE-ORDER, not "index
			//!    tracking": 2*n*dHi (optical path / colour order) is ~const
			//!    across oxides, so the highest-index oxide (TiO2) gets the
			//!    THINNEST film for the same colour (n*dHi ~= 193..218, flat
			//!    to +/-6%).
			//!  - flakeC means "useful colour ends".  For the valve metals /
			//!    Ti it is near real breakaway/grey-out.  For STEEL (420 C) it
			//!    is COLOUR-DEATH (the temper film goes grey) -- literal
			//!    wuestite spallation is much higher (~570 C); the showcase
			//!    still renders steel's high-temperature face as matte scale
			//!    because at the controlled-ramp rim (~1000 C) it genuinely is
			//!    heavy scale.
			//!  - Per-metal Arrhenius Ea (MetalEa) belongs to the
			//!    normalized-dose path and is deliberately UNUSED here; the
			//!    T->thickness map below is piecewise-LINEAR in temperature
			//!    (the sqrt-Arrhenius curvature is dropped -- a comparison
			//!    simplification, see thermal_oxide_sim.py's own disclaimer).
			struct MetalThermal
			{
				Scalar onsetC, optLoC, optHiC, flakeC;
				Scalar dLoNm, dHiNm;
			};

			static MetalThermal MetalThermalModel( const char metal0 )
			{
				MetalThermal m;
				switch( metal0 )
				{
				case 'N': m.onsetC = 200; m.optLoC = 250; m.optHiC = 520; m.flakeC = 580; m.dLoNm = 12; m.dHiNm = 88; break;	// Nb / Nb2O5
				case 'a': m.onsetC = 250; m.optLoC = 300; m.optHiC = 560; m.flakeC = 620; m.dLoNm = 10; m.dHiNm = 94; break;	// Ta / Ta2O5
				case 'S': m.onsetC = 210; m.optLoC = 230; m.optHiC = 350; m.flakeC = 420; m.dLoNm = 11; m.dHiNm = 93; break;	// Steel / Fe3O4 (flakeC = colour-death, not spall)
				default:
				case 'T': m.onsetC = 250; m.optLoC = 300; m.optHiC = 580; m.flakeC = 650; m.dLoNm = 10; m.dHiNm = 73; break;	// Ti / TiO2
				}
				return m;
			}

			//! Absolute oxide thickness (nm) at dial (x, y) for an ABSOLUTE
			//! radial temperature ramp tempCenterC -> tempRimC (mapped by the
			//! falloff over the radius).  Piecewise-LINEAR in T through the
			//! metal's temper anchors: 0 below onset, the beautiful sweep
			//! dLo..dHi across the optimal window, then 2*dHi at the flake
			//! temperature and growing to a 3.5*dHi cap beyond -- a thick,
			//! high-order film that DESATURATES (so even where the spall
			//! blend mask is < 1 the underlying interference reads neutral,
			//! not a garish 2nd-order colour).  The matte-scale appearance is
			//! then completed by the spall mask driving rd/rs dark + rough.
			Scalar AbsoluteThicknessNm( const Scalar x, const Scalar y, const int falloffMode,
			                            const Scalar tempCenterC, const Scalar tempRimC,
			                            const MetalThermal& m ) const
			{
				const Scalar heat = HeatAt( x, y, falloffMode );
				const Scalar T = tempCenterC + heat * ( tempRimC - tempCenterC );
				if( T < m.onsetC )  return Scalar(0);
				if( T < m.optLoC )  return m.dLoNm * ( T - m.onsetC ) / std::max( Scalar(1), m.optLoC - m.onsetC );
				if( T <= m.optHiC ) return m.dLoNm + ( m.dHiNm - m.dLoNm ) * ( T - m.optLoC ) / std::max( Scalar(1), m.optHiC - m.optLoC );
				if( T <= m.flakeC ) return m.dHiNm + m.dHiNm * ( T - m.optHiC ) / std::max( Scalar(1), m.flakeC - m.optHiC );	// dHi -> 2*dHi
				return std::min( Scalar(2) * m.dHiNm + m.dHiNm * ( T - m.flakeC ) / Scalar(100), Scalar(3.5) * m.dHiNm );
			}

			//! Spall fraction in [0,1] at dial (x, y): a smoothstep through the
			//! metal's flake temperature on the same radial ramp.  Drives the
			//! matte oxide-scale blend (0 = intact thin film, 1 = spalled).
			//! The +/-22 C half-width reads as "scale forming" -- a soft band
			//! rather than a hard ring -- and reaches mask~1 well before the
			//! thickness has climbed into the desaturated high order.
			Scalar SpallMask( const Scalar x, const Scalar y, const int falloffMode,
			                  const Scalar tempCenterC, const Scalar tempRimC,
			                  const MetalThermal& m ) const
			{
				using namespace GuillocheMath;
				const Scalar heat = HeatAt( x, y, falloffMode );
				const Scalar T = tempCenterC + heat * ( tempRimC - tempCenterC );
				const Scalar w = Scalar(22);	// transition half-width (deg C)
				return smoothstepG( m.flakeC - w, m.flakeC + w, T );
			}

		public:
			static constexpr Scalar kGasConstant = 8.314;	// J/(mol K)
			static constexpr Scalar kTMinK = 700.0;			// K, centre (~427 C)
			static constexpr Scalar kTMaxK = 900.0;			// K, rim (~627 C)

		protected:

			GuillocheParams m_p;

		};
	}
}

#endif
