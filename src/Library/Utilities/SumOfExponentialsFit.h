//////////////////////////////////////////////////////////////////////
//
//  SumOfExponentialsFit.h - Fits a radial profile Rd(r) to a sum
//  of K weighted exponentials: Rd(r) ~ sum_k w_k * exp(-s_k * r) / r
//
//  Uses non-negative least squares (NNLS) via the Lawson & Hanson
//  active-set algorithm to find non-negative weights w_k given
//  fixed rates s_k and tabulated Rd(r) samples.
//
//  The fitting model is:
//    Rd(r_i) * r_i  ~  sum_k  w_k * exp(-s_k * r_i)
//  so the 1/r singularity is factored out and the linear system
//  solves for weights of pure exponentials.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 22, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUM_OF_EXPONENTIALS_FIT_H
#define SUM_OF_EXPONENTIALS_FIT_H

#include <cmath>
#include <cstring>

namespace RISE
{
	/// A single exponential term: w * exp(-rate * r) / r
	struct ExponentialTerm
	{
		double	weight;			///< Non-negative amplitude
		double	rate;			///< Exponential decay rate (cm^-1)
	};

	/// Fits tabulated Rd(r) to sum_k w_k * exp(-rate_k * r) / r.
	///
	/// The model is linearized by multiplying both sides by r:
	///   y_i = Rd(r_i) * r_i  =  sum_k  w_k * exp(-rate_k * r_i)
	///   A[i][k] = exp(-rate_k * r_i)
	///   Solve A * w = y  subject to w >= 0
	///
	/// \param r_samples    Radial sample points (cm), length N
	/// \param Rd_samples   Profile values Rd(r_i), length N
	/// \param N            Number of radial samples
	/// \param rates        Fixed exponential rates, length K
	/// \param K            Number of exponential terms (max 16)
	/// \param terms_out    Output: fitted terms with weights and rates
	/// \return Number of active terms (weight > 0)
	inline int FitSumOfExponentials(
		const double* r_samples,
		const double* Rd_samples,
		const int N,
		const double* rates,
		const int K,
		ExponentialTerm* terms_out
		)
	{
		// Safety limit
		const int MAX_K = 16;
		if( K > MAX_K || K <= 0 || N <= 0 ) return 0;

		// Build A[i][k] = exp(-rates[k] * r[i])
		// and y[i] = Rd(r[i]) * r[i]
		// Then solve normal equations: (A^T A) w = A^T y  with w >= 0

		// Normal equation matrices (K x K and K x 1)
		double AtA[MAX_K][MAX_K];
		double Aty[MAX_K];
		memset( AtA, 0, sizeof(AtA) );
		memset( Aty, 0, sizeof(Aty) );

		for( int i = 0; i < N; i++ )
		{
			const double ri = r_samples[i];
			const double yi = Rd_samples[i] * ri;		// factor out 1/r

			double Ai[MAX_K];
			for( int k = 0; k < K; k++ )
			{
				Ai[k] = exp( -rates[k] * ri );
			}

			for( int k = 0; k < K; k++ )
			{
				Aty[k] += Ai[k] * yi;
				for( int j = 0; j < K; j++ )
				{
					AtA[k][j] += Ai[k] * Ai[j];
				}
			}
		}

		//=============================================================
		// NNLS: Lawson & Hanson active-set method
		// Solve AtA * w = Aty subject to w >= 0
		//=============================================================

		double w[MAX_K];
		bool   passive[MAX_K];		// true = variable is in passive set (free)
		memset( w, 0, sizeof(w) );
		memset( passive, 0, sizeof(passive) );

		const int MAX_ITER = K * 3 + 30;

		for( int iter = 0; iter < MAX_ITER; iter++ )
		{
			// Compute gradient of 0.5 * ||Aw - y||^2 = AtA*w - Aty
			double grad[MAX_K];
			for( int k = 0; k < K; k++ )
			{
				grad[k] = -Aty[k];
				for( int j = 0; j < K; j++ )
				{
					grad[k] += AtA[k][j] * w[j];
				}
			}

			// Find the active variable with most negative gradient
			// (steepest descent direction for a currently-zero variable)
			int best = -1;
			double bestGrad = -1e-12;		// threshold to avoid noise
			for( int k = 0; k < K; k++ )
			{
				if( !passive[k] && grad[k] < bestGrad )
				{
					bestGrad = grad[k];
					best = k;
				}
			}

			if( best < 0 ) break;		// optimality: all active gradients >= 0

			// Move variable 'best' to passive set
			passive[best] = true;

			// Inner loop: solve unconstrained sub-problem on passive set,
			// then fix any variables that went negative
			for( int inner = 0; inner < MAX_ITER; inner++ )
			{
				// Count passive variables
				int passiveIdx[MAX_K];
				int nPassive = 0;
				for( int k = 0; k < K; k++ )
				{
					if( passive[k] ) passiveIdx[nPassive++] = k;
				}

				if( nPassive == 0 ) break;

				// Solve the nPassive x nPassive sub-system via Cholesky
				double subAtA[MAX_K][MAX_K];
				double subRhs[MAX_K];
				for( int a = 0; a < nPassive; a++ )
				{
					subRhs[a] = Aty[passiveIdx[a]];
					for( int b = 0; b < nPassive; b++ )
					{
						subAtA[a][b] = AtA[passiveIdx[a]][passiveIdx[b]];
					}
				}

				// Cholesky decomposition: subAtA = L * L^T
				double L[MAX_K][MAX_K];
				memset( L, 0, sizeof(L) );
					for( int a = 0; a < nPassive; a++ )
				{
					double sum = 0;
					for( int p = 0; p < a; p++ )
						sum += L[a][p] * L[a][p];

					double diag = subAtA[a][a] - sum;
					if( diag <= 1e-30 )
					{
						L[a][a] = 1e-15;
					}
					else
					{
						L[a][a] = sqrt( diag );
					}

					for( int b = a + 1; b < nPassive; b++ )
					{
						double s = 0;
						for( int p = 0; p < a; p++ )
							s += L[b][p] * L[a][p];
						L[b][a] = (subAtA[b][a] - s) / L[a][a];
					}
				}

				// Forward substitution: L * z = subRhs
				double z[MAX_K];
				for( int a = 0; a < nPassive; a++ )
				{
					double s = 0;
					for( int p = 0; p < a; p++ )
						s += L[a][p] * z[p];
					z[a] = (subRhs[a] - s) / L[a][a];
				}

				// Back substitution: L^T * wSub = z
				double wSub[MAX_K];
				for( int a = nPassive - 1; a >= 0; a-- )
				{
					double s = 0;
					for( int p = a + 1; p < nPassive; p++ )
						s += L[p][a] * wSub[p];
					wSub[a] = (z[a] - s) / L[a][a];
				}

				// Check for negative values in passive set
				bool allPositive = true;
				for( int a = 0; a < nPassive; a++ )
				{
					if( wSub[a] <= 0 )
					{
						allPositive = false;
						break;
					}
				}

				if( allPositive )
				{
					// Accept the solution
					for( int a = 0; a < nPassive; a++ )
						w[passiveIdx[a]] = wSub[a];
					break;
				}

				// Interpolate toward feasibility: find alpha such that
				// w + alpha*(wSub - w) has its first zero
				double alpha = 1.0;
				for( int a = 0; a < nPassive; a++ )
				{
					if( wSub[a] <= 0 )
					{
						double t = w[passiveIdx[a]] / (w[passiveIdx[a]] - wSub[a]);
						if( t < alpha ) alpha = t;
					}
				}

				// Update w and move zero-crossing variables to active set
				for( int a = 0; a < nPassive; a++ )
				{
					int k = passiveIdx[a];
					w[k] = w[k] + alpha * (wSub[a] - w[k]);
					if( w[k] < 1e-30 )
					{
						w[k] = 0;
						passive[k] = false;
					}
				}
			}
		}

		// Write output
		int numActive = 0;
		for( int k = 0; k < K; k++ )
		{
			terms_out[k].rate = rates[k];
			terms_out[k].weight = (w[k] > 0) ? w[k] : 0;
			if( w[k] > 1e-20 ) numActive++;
		}

		return numActive;
	}
}

#endif
