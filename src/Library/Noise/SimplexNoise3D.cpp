//////////////////////////////////////////////////////////////////////
//
//  SimplexNoise3D.cpp - Implements 3D simplex noise with FBM.
//  Uses the simplex (tetrahedral) grid approach for smoother,
//  more isotropic noise than classic Perlin.
//
//  Reference: Perlin 2002, Gustavson 2005
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "SimplexNoise.h"
#include <math.h>

using namespace RISE;
using namespace RISE::Implementation;

// Permutation table (doubled to avoid wrapping)
static const int perm[512] = {
	151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
	140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
	247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
	57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
	74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
	60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
	65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
	200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
	52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
	207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
	119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
	129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
	218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
	81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
	4,184,127,222,195,203,50,115,78,29,180,142,176,167,72,243,
	141,128,45,236,254,156,113,160,205,150,144,215,184,220,66,237,
	// Second copy
	151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,
	140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,
	247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,
	57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
	74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,
	60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,
	65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,
	200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,
	52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,
	207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,
	119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,
	129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,
	218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,
	81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
	4,184,127,222,195,203,50,115,78,29,180,142,176,167,72,243,
	141,128,45,236,254,156,113,160,205,150,144,215,184,220,66,237
};

// 12 gradient vectors for 3D simplex noise
static const Scalar grad3[12][3] = {
	{1,1,0}, {-1,1,0}, {1,-1,0}, {-1,-1,0},
	{1,0,1}, {-1,0,1}, {1,0,-1}, {-1,0,-1},
	{0,1,1}, {0,-1,1}, {0,1,-1}, {0,-1,-1}
};

// Skewing constants for 3D
// F3 = 1/3, G3 = 1/6
static const Scalar F3 = 1.0 / 3.0;
static const Scalar G3 = 1.0 / 6.0;

SimplexNoise3D::SimplexNoise3D(
	const Scalar dPersistence_,
	const int numOctaves_
) :
  dPersistence( dPersistence_ ),
  numOctaves( numOctaves_ > 32 ? 32 : numOctaves_ ),
  pAmplitudesLUT( 0 ),
  pFrequenciesLUT( 0 ),
  dNormFactor( 0.0 )
{
	int n = numOctaves;
	if( n < 1 ) n = 1;

	pAmplitudesLUT = new Scalar[n];
	GlobalLog()->PrintNew( pAmplitudesLUT, __FILE__, __LINE__, "amplitudesLUT" );
	pFrequenciesLUT = new Scalar[n];
	GlobalLog()->PrintNew( pFrequenciesLUT, __FILE__, __LINE__, "frequenciesLUT" );

	dNormFactor = 0.0;
	for( int i = 0; i < n; i++ ) {
		pAmplitudesLUT[i] = pow( dPersistence, Scalar(i) );
		pFrequenciesLUT[i] = pow( 2.0, Scalar(i) );
		dNormFactor += pAmplitudesLUT[i];
	}
	if( dNormFactor < 1e-10 ) dNormFactor = 1.0;
}

SimplexNoise3D::~SimplexNoise3D()
{
	if( pAmplitudesLUT ) {
		GlobalLog()->PrintDelete( pAmplitudesLUT, __FILE__, __LINE__ );
		delete [] pAmplitudesLUT;
		pAmplitudesLUT = 0;
	}
	if( pFrequenciesLUT ) {
		GlobalLog()->PrintDelete( pFrequenciesLUT, __FILE__, __LINE__ );
		delete [] pFrequenciesLUT;
		pFrequenciesLUT = 0;
	}
}

int SimplexNoise3D::Hash( int i )
{
	return perm[i & 255];
}

Scalar SimplexNoise3D::GradDot( int hash, Scalar x, Scalar y, Scalar z )
{
	int h = hash % 12;
	return grad3[h][0] * x + grad3[h][1] * y + grad3[h][2] * z;
}

Scalar SimplexNoise3D::RawSimplex3D( Scalar xin, Scalar yin, Scalar zin )
{
	Scalar n0, n1, n2, n3;	// Noise contributions from the four corners

	// Skew the input space to determine which simplex cell we're in
	Scalar s = (xin + yin + zin) * F3;
	int i = (int)floor(xin + s);
	int j = (int)floor(yin + s);
	int k = (int)floor(zin + s);

	Scalar t = (i + j + k) * G3;
	Scalar X0 = i - t;	// Unskew the cell origin back to (x,y,z) space
	Scalar Y0 = j - t;
	Scalar Z0 = k - t;
	Scalar x0 = xin - X0;	// The x,y,z distances from the cell origin
	Scalar y0 = yin - Y0;
	Scalar z0 = zin - Z0;

	// Determine which simplex we are in (6 possible simplices in 3D)
	int i1, j1, k1;	// Offsets for second corner
	int i2, j2, k2;	// Offsets for third corner

	if( x0 >= y0 ) {
		if( y0 >= z0 ) {
			i1=1; j1=0; k1=0; i2=1; j2=1; k2=0;	// XYZ order
		} else if( x0 >= z0 ) {
			i1=1; j1=0; k1=0; i2=1; j2=0; k2=1;	// XZY order
		} else {
			i1=0; j1=0; k1=1; i2=1; j2=0; k2=1;	// ZXY order
		}
	} else {
		if( y0 < z0 ) {
			i1=0; j1=0; k1=1; i2=0; j2=1; k2=1;	// ZYX order
		} else if( x0 < z0 ) {
			i1=0; j1=1; k1=0; i2=0; j2=1; k2=1;	// YZX order
		} else {
			i1=0; j1=1; k1=0; i2=1; j2=1; k2=0;	// YXZ order
		}
	}

	// Offsets for remaining corners in (x,y,z) coords
	Scalar x1 = x0 - i1 + G3;
	Scalar y1 = y0 - j1 + G3;
	Scalar z1 = z0 - k1 + G3;
	Scalar x2 = x0 - i2 + 2.0*G3;
	Scalar y2 = y0 - j2 + 2.0*G3;
	Scalar z2 = z0 - k2 + 2.0*G3;
	Scalar x3 = x0 - 1.0 + 3.0*G3;
	Scalar y3 = y0 - 1.0 + 3.0*G3;
	Scalar z3 = z0 - 1.0 + 3.0*G3;

	// Hash coordinates of the 4 simplex corners
	int ii = i & 255;
	int jj = j & 255;
	int kk = k & 255;
	int gi0 = perm[ii      + perm[jj      + perm[kk     ]]];
	int gi1 = perm[ii + i1 + perm[jj + j1 + perm[kk + k1]]];
	int gi2 = perm[ii + i2 + perm[jj + j2 + perm[kk + k2]]];
	int gi3 = perm[ii + 1  + perm[jj + 1  + perm[kk + 1 ]]];

	// Calculate the contribution from the four corners
	Scalar t0 = 0.6 - x0*x0 - y0*y0 - z0*z0;
	if( t0 < 0 ) {
		n0 = 0.0;
	} else {
		t0 *= t0;
		n0 = t0 * t0 * GradDot( gi0, x0, y0, z0 );
	}

	Scalar t1 = 0.6 - x1*x1 - y1*y1 - z1*z1;
	if( t1 < 0 ) {
		n1 = 0.0;
	} else {
		t1 *= t1;
		n1 = t1 * t1 * GradDot( gi1, x1, y1, z1 );
	}

	Scalar t2 = 0.6 - x2*x2 - y2*y2 - z2*z2;
	if( t2 < 0 ) {
		n2 = 0.0;
	} else {
		t2 *= t2;
		n2 = t2 * t2 * GradDot( gi2, x2, y2, z2 );
	}

	Scalar t3 = 0.6 - x3*x3 - y3*y3 - z3*z3;
	if( t3 < 0 ) {
		n3 = 0.0;
	} else {
		t3 *= t3;
		n3 = t3 * t3 * GradDot( gi3, x3, y3, z3 );
	}

	// Sum up and scale the result to cover [-1, 1]
	return 32.0 * (n0 + n1 + n2 + n3);
}

Scalar SimplexNoise3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	Scalar total = 0.0;

	for( int i = 0; i < numOctaves; i++ ) {
		Scalar freq = pFrequenciesLUT[i];
		total += RawSimplex3D( x * freq, y * freq, z * freq ) * pAmplitudesLUT[i];
	}

	// Normalize from [-normFactor, normFactor] to [0, 1]
	Scalar result = (total / dNormFactor + 1.0) / 2.0;
	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
