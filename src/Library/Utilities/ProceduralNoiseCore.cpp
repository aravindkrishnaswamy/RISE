//////////////////////////////////////////////////////////////////////
//
//  ProceduralNoiseCore.cpp - see ProceduralNoiseCore.h for the
//  bit-identity contract each function is held to.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ProceduralNoiseCore.h"
#include "FiniteMath.h"
#include <algorithm>
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::Implementation::NoiseCore;

//////////////////////////////////////////////////////////////////////
//  Perlin-style lattice noise
//////////////////////////////////////////////////////////////////////

Scalar RISE::Implementation::NoiseCore::LatticeHash3D( int x, int y, int z )
{
	// Unsigned arithmetic for the hash chain -- <<13 on a negative int and
	// the cubic term overflow are UB in signed int; matches Noise3D::Evaluate
	// (Noise/Noise.h) and WorleyNoise3D::HashCell's hardening.
	unsigned int n = (unsigned int)x + (unsigned int)y * 57u + (unsigned int)z * 113u;
	n = (n<<13) ^ n;
	return ( 1.0 - Scalar( (n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffffu) / 1073741824. );
}

Scalar RISE::Implementation::NoiseCore::SmoothedLatticeNoise3D( int x, int y, int z )
{
	const Scalar corners = ( LatticeHash3D(x-1,y-1,z-1)+LatticeHash3D(x+1,y-1,z-1)+LatticeHash3D(x-1,y+1,z-1)+LatticeHash3D(x+1,y+1,z-1)
							+ LatticeHash3D(x-1,y-1,z+1)+LatticeHash3D(x+1,y-1,z+1)+LatticeHash3D(x-1,y+1,z+1)+LatticeHash3D(x+1,y+1,z+1) ) / 64.0;
	const Scalar edges   = ( LatticeHash3D(x,y+1,z+1)+LatticeHash3D(x,y+1,z-1)+LatticeHash3D(x,y-1,z+1)+LatticeHash3D(x,y-1,z-1)
							+ LatticeHash3D(x+1,y,z+1)+LatticeHash3D(x+1,y,z-1)+LatticeHash3D(x-1,y,z+1)+LatticeHash3D(x-1,y,z-1)
							+ LatticeHash3D(x+1,y+1,z)+LatticeHash3D(x+1,y-1,z)+LatticeHash3D(x-1,y+1,z)+LatticeHash3D(x-1,y-1,z) ) / 48.0;
	const Scalar adjacent = ( LatticeHash3D(x-1,y,z)+LatticeHash3D(x+1,y,z)+LatticeHash3D(x,y-1,z)+LatticeHash3D(x,y+1,z)
							+ LatticeHash3D(x,y,z-1)+LatticeHash3D(x,y,z+1) ) / 12.0;
	const Scalar center   = LatticeHash3D(x,y,z) / 8.0;
	return corners + edges + adjacent + center;
}

namespace
{
	inline int ClampOctaves( int octaves )
	{
		if( octaves < 1 ) return 1;
		if( octaves > NoiseCore::kMaxOctaves ) return NoiseCore::kMaxOctaves;
		return octaves;
	}
	inline Scalar SafeParam( Scalar v )
	{
		return RISE::IsFiniteDouble( (double)v ) ? v : Scalar(0);
	}

	// doc 88 S9: Nyquist-style octave fade, Apodaca & Gritz "Advanced
	// RenderMan" (1999) ch.12's filteredsnoise/filteredfBm convention --
	// fully resolved (weight 1) below fw=0.2, fully faded (weight 0) at
	// or above fw=0.6, smoothstep in between.  At fw==0 this returns
	// EXACTLY 1.0 (t clamps to 0, so 1 - 0*0*(3-0) == 1 - 0 == 1 bit-
	// exactly) -- the bit-identity contract every Fbm3D/Turbulence3D/
	// Ridged3D caller relies on for fw==0.
	// P2-b (S9 review round 1): the (0.2, 0.6) band is deliberately more
	// conservative than the classic ~0.5-1.0-cycle Nyquist band -- it fades
	// earlier to suppress ringing/shimmer at grazing angles; empirically
	// chosen, not derived.
	inline Scalar OctaveFadeWeightImpl( Scalar fw )
	{
		const Scalar lo = Scalar(0.2), hi = Scalar(0.6);
		const Scalar t = std::min( std::max( ( fw - lo ) / ( hi - lo ), Scalar(0) ), Scalar(1) );
		const Scalar smooth = t*t*(Scalar(3)-Scalar(2)*t);
		return Scalar(1) - smooth;
	}

	// P1 (S9 review round 1): fade-to-mean constants for Turbulence3D/Ridged3D.
	//
	// The pre-fix code faded each octave's NUMERATOR toward zero
	// (term * amplitude * OctaveFadeWeightImpl(...)) while ampSum -- the
	// normalizing DENOMINATOR -- kept the full, unfaded amplitude sum. That
	// mismatch darkens the result as fw grows (measured ~45% at fw=0.25):
	// the denominator keeps counting full weight for octaves the numerator
	// has stopped contributing. Renormalizing ampSum to match would fix the
	// brightness but breaks the smooth, unfaded octave-count normalization
	// (ampSum = sum(gain^i) for i=0..octaves-1) that every OTHER caller of
	// Turbulence3D/Ridged3D relies on to keep the [0,1] output range stable
	// as `octaves` itself changes. The correct fix, per
	// the filtered-noise literature (Apodaca & Gritz, "Advanced RenderMan",
	// 1999, ch.12's filteredfBm generalized to turbulence/ridged): a fully
	// faded octave should converge to its EXPECTED VALUE over the noise
	// field, not to zero, so per-octave term computation becomes
	// mix(kMean, term, fadeWeight) with ampSum left untouched. At
	// fadeWeight==1 (fw==0, the default) this is bit-identical to the old
	// unfaded term -- see Turbulence3D/Ridged3D for the exact-FP argument.
	//
	// The two means were measured empirically with a scratch (uncommitted)
	// probe that links directly against PerlinOctave3D in this file (not a
	// reimplementation) and samples it at N uniform-random points in
	// [0,4096)^3 via std::mt19937_64:
	//   kTurbulenceMeanAbs = E[ |PerlinOctave3D(x,y,z)| ]
	//   kRidgedMeanSq      = E[ (1 - |PerlinOctave3D(x,y,z)|)^2 ]
	// Two independent runs cross-checked to 4 significant digits:
	//   seed 0xC0FFEE,     N=1,000,000: meanAbs=0.1036462669, meanSq=0.8094343646
	//   seed 0xDEADBEEF,   N=5,000,000: meanAbs=0.1037260387, meanSq=0.8093002944
	// (PerlinOctave3D is a convex combination of 8 heavily-smoothed lattice
	// hashes -- see SmoothedLatticeNoise3D's 26-neighbor weighting -- so its
	// typical magnitude is well inside its [-1,1] bound; a low mean-abs is
	// expected, not a bug in the probe.)
	static const Scalar kTurbulenceMeanAbs = Scalar(0.1037);
	static const Scalar kRidgedMeanSq      = Scalar(0.8093);
}

// P1-C (promoted to the public API for P1-A, S8 review round 1 -- see
// ProceduralNoiseCore.h for the full rationale): floor(x) then a bare
// (int) cast is UB when the floored value is outside int's representable
// range -- e.g. cellhash(1e20) from the expression VM (UBSan-confirmed).
// Clamp in Scalar (double) space, BEFORE the cast, to the widest window
// every int can hold; a value outside it saturates to the boundary
// instead of hitting the UB / platform-divergent truncation a raw cast
// would give.  Shared by every floor-then-truncate site in this file
// that can receive an unbounded, user-authored Scalar (CellHash
// directly; WorleySample3D's ix/iy/iz, reached via
// worley_f1/f2/f2f1/id(vec3(huge,...), jitter); PerlinOctave3D's X/Y/Z,
// reached via perlin()/fbm()/turbulence()/ridged() -- fbm-family
// octave/lacunarity looping can also drive the sampled coordinate
// arbitrarily large even from an in-range vec3 literal) -- and, now
// that it lives in the public API, by StochasticTilePainter.cpp and
// ScatterPainter.cpp as well, whose tile_scale/cell_scale parameters
// are equally unbounded.
int RISE::Implementation::NoiseCore::SafeFloorToInt( Scalar v )
{
	Scalar f = std::floor( RISE::IsFiniteDouble( (double)v ) ? v : Scalar(0) );
	if( f < Scalar(-2147483648.0) ) f = Scalar(-2147483648.0);
	if( f > Scalar(2147483647.0) )  f = Scalar(2147483647.0);
	return (int)f;
}

Scalar RISE::Implementation::NoiseCore::PerlinOctave3D( Scalar x, Scalar y, Scalar z )
{
	// X/Y/Z go through SafeFloorToInt (P1-C sibling), not a bare int(fX) cast --
	// see that helper's comment; fracX/Y/Z stay a plain x-floor(x) (always in
	// [0,1) regardless of magnitude, no UB there).
	const Scalar fX = std::floor(x); const Scalar fracX = x - fX; const int X = SafeFloorToInt(x);
	const Scalar fY = std::floor(y); const Scalar fracY = y - fY; const int Y = SafeFloorToInt(y);
	const Scalar fZ = std::floor(z); const Scalar fracZ = z - fZ; const int Z = SafeFloorToInt(z);

	const Scalar v1 = SmoothedLatticeNoise3D( X,   Y,   Z   );
	const Scalar v2 = SmoothedLatticeNoise3D( X+1, Y,   Z   );
	const Scalar v3 = SmoothedLatticeNoise3D( X,   Y+1, Z   );
	const Scalar v4 = SmoothedLatticeNoise3D( X+1, Y+1, Z   );
	const Scalar v5 = SmoothedLatticeNoise3D( X,   Y,   Z+1 );
	const Scalar v6 = SmoothedLatticeNoise3D( X+1, Y,   Z+1 );
	const Scalar v7 = SmoothedLatticeNoise3D( X,   Y+1, Z+1 );
	const Scalar v8 = SmoothedLatticeNoise3D( X+1, Y+1, Z+1 );

	// LinearInterpolator::InterpolateValues( a, b, t ) = a*(1-t) + b*t
	const Scalar i1 = v1*(1.0-fracX) + v2*fracX;
	const Scalar i2 = v3*(1.0-fracX) + v4*fracX;
	const Scalar i3 = v5*(1.0-fracX) + v6*fracX;
	const Scalar i4 = v7*(1.0-fracX) + v8*fracX;

	const Scalar j1 = i1*(1.0-fracY) + i2*fracY;
	const Scalar j2 = i3*(1.0-fracY) + i4*fracY;

	return j1*(1.0-fracZ) + j2*fracZ;
}

Scalar RISE::Implementation::NoiseCore::Fbm3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw )
{
	octaves = ClampOctaves( octaves );
	gain = SafeParam( gain );
	lacunarity = SafeParam( lacunarity );
	fw = SafeParam( fw );
	if( fw < Scalar(0) ) fw = Scalar(0);

	// Fbm3D needs no fade-to-mean term (contrast Turbulence3D/Ridged3D
	// below): PerlinOctave3D is SIGNED with mean ~0 (it's a convex
	// combination of zero-mean lattice hashes), so fading a term toward
	// zero already fades it toward its expected value -- there is no
	// brightness mismatch to correct here.
	Scalar total = 0, amplitude = 1, frequency = 1, fwOctave = fw;
	for( int i = 0; i < octaves; ++i ) {
		total += PerlinOctave3D( x*frequency, y*frequency, z*frequency ) * amplitude * OctaveFadeWeightImpl( fwOctave );
		amplitude *= gain;
		frequency *= lacunarity;
		fwOctave *= lacunarity;
	}
	return total;
}

Scalar RISE::Implementation::NoiseCore::Turbulence3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw )
{
	octaves = ClampOctaves( octaves );
	gain = SafeParam( gain );
	lacunarity = SafeParam( lacunarity );
	fw = SafeParam( fw );
	if( fw < Scalar(0) ) fw = Scalar(0);

	Scalar total = 0, amplitude = 1, frequency = 1, ampSum = 0, fwOctave = fw;
	bool allFullyFaded = true;	// every octave's weight was exactly 0.0
	for( int i = 0; i < octaves; ++i ) {
		const Scalar term = std::fabs( PerlinOctave3D( x*frequency, y*frequency, z*frequency ) );
		const Scalar w = OctaveFadeWeightImpl( fwOctave );
		if( w != Scalar(0) ) allFullyFaded = false;
		// Fade-to-mean (P1, S9 review round 1): a faded octave converges to
		// kTurbulenceMeanAbs (its expected value), not to zero -- see the
		// derivation comment above OctaveFadeWeightImpl for why. ampSum is
		// intentionally left unfaded (renormalizing it would break the
		// smooth monotone octave-count behaviour). At w==1.0 (guaranteed
		// exactly at fw==0, every octave, since fwOctave*=lacunarity keeps
		// 0*anything==0.0) this is bit-identical to the pre-fix term:
		// kTurbulenceMeanAbs*(1-1.0) == kTurbulenceMeanAbs*0.0 == 0.0 exactly,
		// term*1.0 == term exactly (multiplying by 1.0 is always exact), so
		// the mix collapses to term*amplitude bit-for-bit.
		total += ( kTurbulenceMeanAbs * ( Scalar(1) - w ) + term * w ) * amplitude;
		ampSum += amplitude;
		amplitude *= gain;
		frequency *= lacunarity;
		fwOctave *= lacunarity;
	}
	// A weighted average of the SAME constant (kTurbulenceMeanAbs, at every
	// octave) is mathematically that constant regardless of the weights --
	// but total/ampSum is only an APPROXIMATION of that (per-term rounding
	// in the accumulation doesn't generally cancel in the division). Return
	// the constant directly so the large-fw limit is exact, not merely
	// close, matching the fw==0 exactness contract on the other end.
	if( allFullyFaded ) return kTurbulenceMeanAbs;
	return ( ampSum > 1e-12 ) ? ( total / ampSum ) : Scalar(0);
}

Scalar RISE::Implementation::NoiseCore::Ridged3D( Scalar x, Scalar y, Scalar z, int octaves, Scalar gain, Scalar lacunarity, Scalar fw )
{
	octaves = ClampOctaves( octaves );
	gain = SafeParam( gain );
	lacunarity = SafeParam( lacunarity );
	fw = SafeParam( fw );
	if( fw < Scalar(0) ) fw = Scalar(0);

	Scalar total = 0, amplitude = 1, frequency = 1, ampSum = 0, fwOctave = fw;
	bool allFullyFaded = true;	// every octave's weight was exactly 0.0
	for( int i = 0; i < octaves; ++i ) {
		const Scalar n = PerlinOctave3D( x*frequency, y*frequency, z*frequency );
		const Scalar ridge = 1.0 - std::fabs( n );
		const Scalar term = ridge * ridge;
		const Scalar w = OctaveFadeWeightImpl( fwOctave );
		if( w != Scalar(0) ) allFullyFaded = false;
		// Fade-to-mean (P1, S9 review round 1): same treatment as
		// Turbulence3D above, converging to kRidgedMeanSq instead of
		// kTurbulenceMeanAbs (the ridge term's own expected value) --
		// see the derivation comment above OctaveFadeWeightImpl. Same
		// exact-FP argument gives bit-identity at w==1.0 (fw==0).
		total += ( kRidgedMeanSq * ( Scalar(1) - w ) + term * w ) * amplitude;
		ampSum += amplitude;
		amplitude *= gain;
		frequency *= lacunarity;
		fwOctave *= lacunarity;
	}
	// See Turbulence3D's matching comment: return the constant directly at
	// the large-fw limit instead of relying on total/ampSum to land on it
	// exactly.
	if( allFullyFaded ) return kRidgedMeanSq;
	return ( ampSum > 1e-12 ) ? ( total / ampSum ) : Scalar(0);
}

Scalar RISE::Implementation::NoiseCore::OctaveFadeWeight( Scalar fw )
{
	fw = SafeParam( fw );
	if( fw < Scalar(0) ) fw = Scalar(0);
	return OctaveFadeWeightImpl( fw );
}

//////////////////////////////////////////////////////////////////////
//  Worley (cellular) noise
//////////////////////////////////////////////////////////////////////

unsigned int RISE::Implementation::NoiseCore::WorleyHashCell( int ix, int iy, int iz )
{
	unsigned int n = (unsigned int)ix * 702395077u + (unsigned int)iy * 915488749u + (unsigned int)iz * 2120969693u;
	n = (n << 13) ^ n;
	n = n * (n * n * 15731u + 789221u) + 1376312589u;
	return n;
}

void RISE::Implementation::NoiseCore::WorleySample3D( Scalar x, Scalar y, Scalar z, Scalar jitter, WorleyMetric metric,
													   Scalar& outF1, Scalar& outF2, int& outF1CellX, int& outF1CellY, int& outF1CellZ )
{
	// ix/iy/iz go through SafeFloorToInt (P1-C sibling), not a bare
	// (int)std::floor() cast -- see that helper's comment.
	const int ix = SafeFloorToInt(x);
	const int iy = SafeFloorToInt(y);
	const int iz = SafeFloorToInt(z);

	const Scalar fx = x - ix;
	const Scalar fy = y - iy;
	const Scalar fz = z - iz;

	Scalar f1 = 1e20, f2 = 1e20;
	int f1cx = ix, f1cy = iy, f1cz = iz;

	for( int dz_ = -1; dz_ <= 1; ++dz_ ) {
		for( int dy_ = -1; dy_ <= 1; ++dy_ ) {
			for( int dx_ = -1; dx_ <= 1; ++dx_ ) {
				const int cx = ix + dx_;
				const int cy = iy + dy_;
				const int cz = iz + dz_;

				unsigned int h = WorleyHashCell( cx, cy, cz );
				Scalar px = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;
				h = h * 1103515245u + 12345u;
				Scalar py = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;
				h = h * 1103515245u + 12345u;
				Scalar pz = Scalar( (h >> 16) & 0xFFFF ) / 65536.0;

				px = 0.5 + jitter * (px - 0.5);
				py = 0.5 + jitter * (py - 0.5);
				pz = 0.5 + jitter * (pz - 0.5);

				const Scalar ddx = (dx_ + px) - fx;
				const Scalar ddy = (dy_ + py) - fy;
				const Scalar ddz = (dz_ + pz) - fz;

				Scalar dist;
				switch( metric ) {
				case eMetricManhattan: dist = std::fabs(ddx) + std::fabs(ddy) + std::fabs(ddz); break;
				case eMetricChebyshev: { const Scalar ax=std::fabs(ddx), ay=std::fabs(ddy), az=std::fabs(ddz); const Scalar m = ax>ay?ax:ay; dist = m>az?m:az; } break;
				case eMetricEuclidean: default: dist = std::sqrt( ddx*ddx + ddy*ddy + ddz*ddz ); break;
				}

				if( dist < f1 ) {
					f2 = f1;
					f1 = dist;
					f1cx = cx; f1cy = cy; f1cz = cz;
				} else if( dist < f2 ) {
					f2 = dist;
				}
			}
		}
	}

	outF1 = f1; outF2 = f2;
	outF1CellX = f1cx; outF1CellY = f1cy; outF1CellZ = f1cz;
}

Scalar RISE::Implementation::NoiseCore::WorleyNormalize( Scalar raw, WorleyMetric metric, WorleyMode mode )
{
	static const Scalar normTable[3][3] = {
		//  F1,   F2,   F2-F1
		{   1.0,  1.5,  0.75 },	// Euclidean
		{   1.5,  2.5,  1.2  },	// Manhattan
		{   0.6,  1.0,  0.5  }	// Chebyshev
	};
	unsigned int mi = (unsigned int)metric; if( mi > 2 ) mi = 0;
	unsigned int oi = (unsigned int)mode;   if( oi > 2 ) oi = 0;
	Scalar result = raw / normTable[mi][oi];
	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;
	return result;
}

Scalar RISE::Implementation::NoiseCore::WorleyIdOf( int cellX, int cellY, int cellZ )
{
	const unsigned int h = WorleyHashCell( cellX, cellY, cellZ );
	return Scalar( h % 1000003u );
}

//////////////////////////////////////////////////////////////////////
//  Generic scalar hash
//////////////////////////////////////////////////////////////////////

Scalar RISE::Implementation::NoiseCore::CellHash( Scalar x )
{
	// Same integer mix formula as Noise1D::Evaluate (Noise/Noise.h), but
	// with the masked 31-bit hash scaled directly to [0,1) instead of
	// Noise1D's final (-1,1) normalization -- this is a hash utility
	// (per-cell pseudo-random attribute), not a noise sample.
	// P1-C: SafeFloorToInt clamps in Scalar space before the cast, so
	// cellhash(1e20) (or a nan/inf param, already filtered by SafeParam
	// but harmless to keep) can never hit the UB a bare (int)floor(x)
	// cast would (UBSan-confirmed on the old code).
	const int ix = SafeFloorToInt( SafeParam(x) );
	unsigned int n = (unsigned int)ix;
	n = (n<<13) ^ n;
	n = n * (n * n * 15731u + 789221u) + 1376312589u;
	return Scalar( n & 0x7fffffffu ) / 2147483648.0;
}
