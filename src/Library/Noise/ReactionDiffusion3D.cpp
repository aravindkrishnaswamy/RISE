//////////////////////////////////////////////////////////////////////
//
//  ReactionDiffusion3D.cpp - Implements 3D reaction-diffusion
//  (Turing) patterns via the Gray-Scott model.
//
//  The Gray-Scott model simulates two chemicals:
//    U (activator): dU/dt = Da * laplacian(U) - U*V^2 + feed*(1-U)
//    V (inhibitor): dV/dt = Db * laplacian(V) + U*V^2 - (feed+kill)*V
//
//  The output is the V concentration field, which forms
//  spots, stripes, coral, or sponge-like patterns depending
//  on the feed/kill parameters.
//
//  Reference: Gray & Scott 1984; Pearson 1993
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ReactionDiffusion.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

using namespace RISE;
using namespace RISE::Implementation;

ReactionDiffusion3D::ReactionDiffusion3D(
	const unsigned int nGridSize_,
	const Scalar dDa_,
	const Scalar dDb_,
	const Scalar dFeed_,
	const Scalar dKill_,
	const unsigned int nIterations_
) :
  nGridSize( nGridSize_ < 8 ? 8 : (nGridSize_ > 64 ? 64 : nGridSize_) ),
  pGrid( 0 ),
  dDa( dDa_ ),
  dDb( dDb_ ),
  dFeed( dFeed_ ),
  dKill( dKill_ ),
  nIterations( nIterations_ > 5000 ? 5000 : nIterations_ )
{
	Simulate();
}

ReactionDiffusion3D::~ReactionDiffusion3D()
{
	if( pGrid ) {
		GlobalLog()->PrintDelete( pGrid, __FILE__, __LINE__ );
		delete [] pGrid;
		pGrid = 0;
	}
}

void ReactionDiffusion3D::Simulate()
{
	const int n = (int)nGridSize;
	const int n3 = n * n * n;

	Scalar* U = new Scalar[n3];
	Scalar* V = new Scalar[n3];
	Scalar* Unew = new Scalar[n3];
	Scalar* Vnew = new Scalar[n3];

	// Initialize: U=1 everywhere, V=0 except a seed region
	for( int i = 0; i < n3; i++ ) {
		U[i] = 1.0;
		V[i] = 0.0;
	}

	// Seed V in a small central region with some randomness
	unsigned int seed = 12345u;
	int center = n / 2;
	int seedRadius = n / 6;
	if( seedRadius < 2 ) seedRadius = 2;

	for( int z = center - seedRadius; z <= center + seedRadius; z++ ) {
		for( int y = center - seedRadius; y <= center + seedRadius; y++ ) {
			for( int x = center - seedRadius; x <= center + seedRadius; x++ ) {
				int wx = ((x % n) + n) % n;
				int wy = ((y % n) + n) % n;
				int wz = ((z % n) + n) % n;
				int idx = wz * n * n + wy * n + wx;
				// Add some randomness to break symmetry
				seed = seed * 1103515245u + 12345u;
				Scalar r = Scalar((seed >> 16) & 0xFFFF) / 65536.0;
				V[idx] = 0.25 + 0.25 * r;
				U[idx] = 0.5 + 0.25 * r;
			}
		}
	}

	// Run simulation
	const Scalar dt = 1.0;
	for( unsigned int iter = 0; iter < nIterations; iter++ ) {
		for( int z = 0; z < n; z++ ) {
			for( int y = 0; y < n; y++ ) {
				for( int x = 0; x < n; x++ ) {
					int idx = z * n * n + y * n + x;

					// 6-neighbor Laplacian with periodic boundary
					int xm = ((x-1) % n + n) % n;
					int xp = ((x+1) % n + n) % n;
					int ym = ((y-1) % n + n) % n;
					int yp = ((y+1) % n + n) % n;
					int zm = ((z-1) % n + n) % n;
					int zp = ((z+1) % n + n) % n;

					Scalar lapU = U[z*n*n + y*n + xp] + U[z*n*n + y*n + xm]
								+ U[z*n*n + yp*n + x] + U[z*n*n + ym*n + x]
								+ U[zp*n*n + y*n + x] + U[zm*n*n + y*n + x]
								- 6.0 * U[idx];

					Scalar lapV = V[z*n*n + y*n + xp] + V[z*n*n + y*n + xm]
								+ V[z*n*n + yp*n + x] + V[z*n*n + ym*n + x]
								+ V[zp*n*n + y*n + x] + V[zm*n*n + y*n + x]
								- 6.0 * V[idx];

					Scalar u = U[idx];
					Scalar v = V[idx];
					Scalar uvv = u * v * v;

					Unew[idx] = u + dt * (dDa * lapU - uvv + dFeed * (1.0 - u));
					Vnew[idx] = v + dt * (dDb * lapV + uvv - (dFeed + dKill) * v);

					// Clamp
					if( Unew[idx] < 0.0 ) Unew[idx] = 0.0;
					if( Unew[idx] > 1.0 ) Unew[idx] = 1.0;
					if( Vnew[idx] < 0.0 ) Vnew[idx] = 0.0;
					if( Vnew[idx] > 1.0 ) Vnew[idx] = 1.0;
				}
			}
		}

		// Swap buffers
		Scalar* tmpU = U;
		U = Unew;
		Unew = tmpU;
		Scalar* tmpV = V;
		V = Vnew;
		Vnew = tmpV;
	}

	// Store V as the output grid (V has the patterns)
	pGrid = new Scalar[n3];
	GlobalLog()->PrintNew( pGrid, __FILE__, __LINE__, "reaction-diffusion grid" );
	memcpy( pGrid, V, n3 * sizeof(Scalar) );

	delete [] U;
	delete [] V;
	delete [] Unew;
	delete [] Vnew;
}

Scalar ReactionDiffusion3D::Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
{
	if( !pGrid ) return 0.0;

	const int n = (int)nGridSize;

	// Map coordinates to grid with wrapping
	Scalar fx = fmod( x * n, (Scalar)n );
	if( fx < 0 ) fx += n;
	Scalar fy = fmod( y * n, (Scalar)n );
	if( fy < 0 ) fy += n;
	Scalar fz = fmod( z * n, (Scalar)n );
	if( fz < 0 ) fz += n;

	// Trilinear interpolation
	int ix = (int)floor(fx);
	int iy = (int)floor(fy);
	int iz = (int)floor(fz);

	Scalar tx = fx - ix;
	Scalar ty = fy - iy;
	Scalar tz = fz - iz;

	int ix1 = (ix + 1) % n;
	int iy1 = (iy + 1) % n;
	int iz1 = (iz + 1) % n;
	ix = ix % n;
	iy = iy % n;
	iz = iz % n;

	Scalar c000 = pGrid[iz  * n*n + iy  * n + ix ];
	Scalar c100 = pGrid[iz  * n*n + iy  * n + ix1];
	Scalar c010 = pGrid[iz  * n*n + iy1 * n + ix ];
	Scalar c110 = pGrid[iz  * n*n + iy1 * n + ix1];
	Scalar c001 = pGrid[iz1 * n*n + iy  * n + ix ];
	Scalar c101 = pGrid[iz1 * n*n + iy  * n + ix1];
	Scalar c011 = pGrid[iz1 * n*n + iy1 * n + ix ];
	Scalar c111 = pGrid[iz1 * n*n + iy1 * n + ix1];

	Scalar c00 = c000 * (1-tx) + c100 * tx;
	Scalar c10 = c010 * (1-tx) + c110 * tx;
	Scalar c01 = c001 * (1-tx) + c101 * tx;
	Scalar c11 = c011 * (1-tx) + c111 * tx;

	Scalar c0 = c00 * (1-ty) + c10 * ty;
	Scalar c1 = c01 * (1-ty) + c11 * ty;

	Scalar result = c0 * (1-tz) + c1 * tz;

	// V is already in [0, 1] from the simulation clamping
	if( result < 0.0 ) result = 0.0;
	if( result > 1.0 ) result = 1.0;

	return result;
}
