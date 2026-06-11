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

			//! Finished height in [0,1] at dial-space (x, y) (world units,
			//! origin = dial centre).  Mirrors `field_<pattern>` + `_finish`,
			//! normalised by the ANALYTIC raw range (see RawMin/RawMax scope
			//! note).  The dial-geometry factory instead normalises by the
			//! achieved grid range via RawHeight + FinishWithBounds, which is
			//! the exact Python `_finish` for every parameter sign.
			Scalar Height( const Scalar x, const Scalar y ) const
			{
				const Scalar r = std::hypot( x, y );
				return FinishWithBounds( RawHeight( x, y ), r, RawMin(), RawMax() );
			}

			//! The UN-normalised pattern field (the `field_<pattern>` value
			//! BEFORE `_finish`).  Grid bakers min/max-reduce this over the
			//! full grid, then call FinishWithBounds -- exactly the Python.
			Scalar RawHeight( const Scalar x, const Scalar y ) const
			{
				const GuillocheParams& p = m_p;
				const Scalar r = std::hypot( x, y );
				switch( p.pattern )
				{
				default:
				case GuillocheParams::ePatUniform:   return RawUniform( x, y, r );
				case GuillocheParams::ePatLightning: return RawLightning( x, y, r );
				case GuillocheParams::ePatRadial:    return RawRadial( x, y, r );
				case GuillocheParams::ePatIris:      return RawIris( x, y, r );
				case GuillocheParams::ePatSwirl:     return RawSwirl( x, y, r );
				case GuillocheParams::ePatVarwidth:  return RawVarwidth( x, y, r );
				}
			}

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

		public:
			static constexpr Scalar kGasConstant = 8.314;	// J/(mol K)
			static constexpr Scalar kTMinK = 700.0;			// K, centre (~427 C)
			static constexpr Scalar kTMaxK = 900.0;			// K, rim (~627 C)

		protected:

			GuillocheParams m_p;

			//! Shared front matter (dial_variants_gen._frame): warped angle,
			//! petal lens, per-sector rotated (radial, tangential) frame.
			void Frame( const Scalar x, const Scalar y, const Scalar r,
			            Scalar& petal, Scalar& xr, Scalar& yr ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				const Scalar rho = clamp01( r / p.radius );
				const Scalar theta = std::atan2( y, x );
				const Scalar jag = p.seamJag * triWave( p.seamJagFreq * rho );
				const Scalar psi = theta + p.swirl * rho + jag;
				const Scalar N = Scalar( p.numArms );
				petal = lens( std::cos( N * psi ), p.petalE0, p.petalE1 );
				const Scalar w = Scalar(TWO_PI) / N;
				const Scalar sector = std::round( psi / w );
				const Scalar thetaC = sector * w - p.swirl * rho - jag;
				const Scalar cc = std::cos( thetaC ), ss = std::sin( thetaC );
				xr =  cc * x + ss * y;
				yr = -ss * x + cc * y;
			}

			//! One woven-pillow grid (half-cell brick offset between rows).
			Scalar Woven( const Scalar xr, const Scalar yr, const Scalar freq ) const
			{
				using namespace GuillocheMath;
				const Scalar ax = freq * xr;
				Scalar ay = freq * yr;
				const Scalar rowpar = std::floor( Scalar(2) * ax );
				ay += Scalar(0.25) * floorMod( rowpar, Scalar(2) );
				return stripe( ax, m_p.gridE0, m_p.gridE1 ) * stripe( ay, m_p.gridE0, m_p.gridE1 );
			}

			//! Tight ALIGNED square-cell grid (no brick offset).
			Scalar Cube( const Scalar u, const Scalar v, const Scalar freq ) const
			{
				using namespace GuillocheMath;
				return stripe( freq * u, m_p.gridE0, m_p.gridE1 ) * stripe( freq * v, m_p.gridE0, m_p.gridE1 );
			}

			// ---- raw (pre-finish) fields; each also reports its analytic range
			//      through RawMin/RawMax below.

			Scalar RawUniform( const Scalar x, const Scalar y, const Scalar r ) const
			{
				Scalar petal, xr, yr;
				Frame( x, y, r, petal, xr, yr );
				const Scalar grid = Woven( xr, yr, Scalar(0.5) / m_p.cell );
				return m_p.base + m_p.petalAmp * petal + m_p.gridAmp * grid;
			}

			Scalar RawRadial( const Scalar x, const Scalar y, const Scalar r ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				Scalar petal, xr, yr;
				Frame( x, y, r, petal, xr, yr );
				const Scalar mask = smoothstepG( p.lightningLo, p.lightningHi, petal );
				const Scalar fField = Scalar(0.5) / p.cell;
				const Scalar fBolt  = Scalar(0.5) / ( p.cell * p.lightningCellScale );
				Scalar grid;
				if( p.cellMode == GuillocheParams::eCellFreqBlend ) {
					grid = Woven( xr, yr, fField * ( Scalar(1) - mask ) + fBolt * mask );
				} else {
					grid = Woven( xr, yr, fField ) * ( Scalar(1) - mask ) + Woven( xr, yr, fBolt ) * mask;
				}
				return p.base + p.petalAmp * petal + p.gridAmp * grid + p.lightningRelief * mask;
			}

			Scalar RawLightning( const Scalar x, const Scalar y, const Scalar r ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				const Scalar rho = clamp01( r / p.radius );
				const Scalar theta = std::atan2( y, x );
				const Scalar N = Scalar( p.numArms );

				// zigzag ray mask (NO swirl / seam-jag: bolts jag, they don't spiral)
				const Scalar zig = p.zigzagAmp * triWave( p.zigzagFreq * rho );
				const Scalar rayc = Scalar(0.5) + Scalar(0.5) * std::cos( N * ( theta + zig ) );
				const Scalar mask = smoothstepG( p.lightningLo, p.lightningHi, rayc );

				// frame stays RADIAL (theta, not theta_z)
				const Scalar w = Scalar(TWO_PI) / N;
				const Scalar sector = std::round( theta / w );
				const Scalar thetaC = sector * w;
				const Scalar cc = std::cos( thetaC ), ss = std::sin( thetaC );
				const Scalar xr =  cc * x + ss * y;
				const Scalar yr = -ss * x + cc * y;

				const Scalar fBolt = Scalar(0.5) / ( p.cell * p.lightningCellScale );
				Scalar grid;
				if( p.cellMode == GuillocheParams::eCellFreqBlend ) {
					const Scalar fField = Scalar(0.5) / p.cell;
					grid = Woven( xr, yr, fField * ( Scalar(1) - mask ) + fBolt * mask );
				} else {
					const Scalar fInside = Scalar(0.5) / p.fieldCell;
					const Scalar uu = ( p.fieldFrame == GuillocheParams::eFrameRadial ) ? xr : x;
					const Scalar vv = ( p.fieldFrame == GuillocheParams::eFrameRadial ) ? yr : y;
					const Scalar insideGrid = Cube( uu, vv, fInside );
					Scalar betweenGrid;
					if( p.boltStyle == GuillocheParams::eBoltWoven ) {
						betweenGrid = Woven( xr, yr, fBolt );
					} else if( p.boltStyle == GuillocheParams::eBoltCube ) {
						betweenGrid = Cube( xr, yr, fBolt );
					} else {	// rung: fixed-size rectangular blocks in the radial frame
						betweenGrid = stripe( ( Scalar(0.5) / p.rungLen ) * xr, p.gridE0, p.gridE1 )
						            * stripe( ( Scalar(0.5) / p.rungWidth ) * yr, p.gridE0, p.gridE1 );
					}
					grid = betweenGrid * ( Scalar(1) - mask ) + insideGrid * mask;
				}
				return p.base + p.petalAmp * mask + p.gridAmp * grid + p.lightningRelief * mask;
			}

			Scalar RawIris( const Scalar x, const Scalar y, const Scalar r ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				const Scalar rho = clamp01( r / p.radius );
				const Scalar a = p.irisAperture * p.radius;
				const int N = p.numArms;
				Scalar edgeD = Scalar(1e9);
				Scalar smax = Scalar(-1e9);
				Scalar ownTx = 0, ownTy = 0, ownNx = 0, ownNy = 0;
				for( int k = 0; k < N; ++k )
				{
					const Scalar tk = Scalar(TWO_PI) * Scalar(k) / Scalar(N) + p.irisSwirl * rho;
					const Scalar ck = std::cos( tk ), sk = std::sin( tk );
					const Scalar d = ( x * ck + y * sk ) - a;
					edgeD = std::min( edgeD, std::fabs( d ) );
					if( d > smax ) {
						smax = d;
						ownTx = -sk; ownTy = ck;
						ownNx =  ck; ownNy = sk;
					}
				}
				const Scalar groove = smoothstepG( Scalar(0), p.irisEdge, edgeD );
				const Scalar along  = x * ownTx + y * ownTy;
				const Scalar across = x * ownNx + y * ownNy;
				const Scalar f = Scalar(0.5) / p.cell;
				const Scalar cube = stripe( f * along, p.gridE0, p.gridE1 ) * stripe( f * across, p.gridE0, p.gridE1 );
				return p.base + p.petalAmp * groove + p.gridAmp * cube * groove;
			}

			Scalar RawSwirl( const Scalar x, const Scalar y, const Scalar r ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				const Scalar theta = std::atan2( y, x );
				const Scalar lr = std::log( std::max( r, Scalar(1e-3) ) );
				const Scalar u = ( Scalar(p.numArms) * theta + p.swirlTurns * lr ) / Scalar(TWO_PI);
				const Scalar v = r / p.cell;
				const Scalar grid = stripe( u, p.gridE0, p.gridE1 ) * stripe( v, p.gridE0, p.gridE1 );
				return p.base + p.gridAmp * grid;
			}

			Scalar RawVarwidth( const Scalar x, const Scalar y, const Scalar r ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				const Scalar theta = std::atan2( y, x );
				const Scalar w = Scalar(TWO_PI) / Scalar( p.numArms );
				const Scalar sector = std::round( theta / w );
				const Scalar tc = sector * w;
				const Scalar cc = std::cos( tc ), ss = std::sin( tc );
				const Scalar xr =  cc * x + ss * y;
				const Scalar yr = -ss * x + cc * y;
				const bool coarse = ( floorMod( sector, Scalar(2) ) != Scalar(0) );
				const Scalar cellLocal = coarse ? p.cell * p.lightningCellScale : p.cell;
				const Scalar fr = Scalar(0.5) / cellLocal;
				const Scalar grid = stripe( fr * xr, p.gridE0, p.gridE1 ) * stripe( fr * yr, p.gridE0, p.gridE1 );
				return p.base + p.gridAmp * grid;
			}

		public:
			//! Analytic raw range for POINTWISE use (Height()).  Exact when the
			//! composed terms reach their extremes independently -- true for all
			//! six shipped pattern configurations (verified <= 3.4e-8 vs the
			//! grid min/max at mesh_n 560..880).  NOT exact when terms couple,
			//! e.g. lightning_relief < 0 (the relief term is gated by the petal
			//! smoothstep) or negative grid/petal amplitudes -- the dial-geometry
			//! factory therefore normalises by the ACHIEVED grid range
			//! (RawHeight + FinishWithBounds), which is the exact Python
			//! `_finish` semantics for every parameter sign.
			Scalar RawMin() const
			{
				const GuillocheParams& p = m_p;
				Scalar mn = p.base;
				if( ( p.pattern == GuillocheParams::ePatRadial ||
				      p.pattern == GuillocheParams::ePatLightning ) && p.lightningRelief < 0 ) {
					mn += p.lightningRelief;
				}
				return mn;
			}
			Scalar RawMax() const
			{
				const GuillocheParams& p = m_p;
				switch( p.pattern )
				{
				case GuillocheParams::ePatSwirl:
				case GuillocheParams::ePatVarwidth:
					return p.base + p.gridAmp;
				case GuillocheParams::ePatRadial:
				case GuillocheParams::ePatLightning:
					return p.base + p.petalAmp + p.gridAmp + std::max( Scalar(0), p.lightningRelief );
				default:
					return p.base + p.petalAmp + p.gridAmp;
				}
			}

			//! Normalise -> land-gamma -> relief squeeze -> flush hub (dial_
			//! variants_gen._finish) against EXPLICIT raw bounds (grid bakers
			//! pass the achieved grid min/max; Height() passes the analytic
			//! range).
			Scalar FinishWithBounds( const Scalar raw, const Scalar r,
			                         const Scalar mn, const Scalar mx ) const
			{
				using namespace GuillocheMath;
				const GuillocheParams& p = m_p;
				const Scalar rng = mx - mn;
				Scalar h = ( rng < Scalar(1e-12) ) ? Scalar(0) : clamp01( ( raw - mn ) / rng );
				const Scalar ll = std::min( std::max( p.landLevel, Scalar(1e-3) ), Scalar(1) - Scalar(1e-3) );
				h = std::pow( h, std::log( ll ) / std::log( Scalar(0.5) ) );
				h = Scalar(0.5) + ( h - Scalar(0.5) ) * p.reliefDepth;
				// flush hub: blend the inner centerRadius*R down to the squeezed
				// field minimum (gamma(0)=0 -> squeeze -> 0.5*(1-reliefDepth)).
				const Scalar hubLevel = Scalar(0.5) * ( Scalar(1) - p.reliefDepth );
				const Scalar rin = std::max( p.centerRadius * p.radius, Scalar(1e-6) );
				Scalar w = clamp01( r / rin );
				w *= w;
				h = ( Scalar(1) - w ) * hubLevel + w * h;
				return clamp01( h );
			}
		};
	}
}

#endif
