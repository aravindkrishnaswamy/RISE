//////////////////////////////////////////////////////////////////////
//
//  AliasTable.h - Walker's alias method for O(1) discrete sampling
//  from a weighted distribution.
//
//  Construction is O(N) using Vose's algorithm.  After Build(),
//  Sample() returns a random index in O(1) using one uniform
//  random number.  Pdf() returns the exact selection probability
//  for a given index, also in O(1).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 30, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ALIAS_TABLE_
#define ALIAS_TABLE_

#include <vector>

namespace RISE
{
	class AliasTable
	{
	public:
		AliasTable() : totalWeight( 0 ) {}

		/// Builds the alias table from a vector of non-negative weights.
		/// After Build(), Sample() and Pdf() are valid.
		void Build( const std::vector<double>& weights )
		{
			const unsigned int n = static_cast<unsigned int>( weights.size() );

			probabilities.resize( n );
			aliases.resize( n );
			normalizedWeights.resize( n );

			totalWeight = 0;
			for( unsigned int i = 0; i < n; i++ ) {
				totalWeight += weights[i];
			}

			if( n == 0 || totalWeight <= 0 )
			{
				return;
			}

			// Compute normalized probabilities scaled by N
			// (each entry's "fair share" is 1.0)
			std::vector<double> scaled( n );
			for( unsigned int i = 0; i < n; i++ ) {
				normalizedWeights[i] = weights[i] / totalWeight;
				scaled[i] = normalizedWeights[i] * n;
			}

			// Vose's algorithm: partition into small (<1) and large (>=1)
			std::vector<unsigned int> small, large;
			small.reserve( n );
			large.reserve( n );

			for( unsigned int i = 0; i < n; i++ ) {
				if( scaled[i] < 1.0 ) {
					small.push_back( i );
				} else {
					large.push_back( i );
				}
			}

			while( !small.empty() && !large.empty() )
			{
				const unsigned int s = small.back(); small.pop_back();
				const unsigned int l = large.back(); large.pop_back();

				probabilities[s] = scaled[s];
				aliases[s] = l;

				scaled[l] = (scaled[l] + scaled[s]) - 1.0;

				if( scaled[l] < 1.0 ) {
					small.push_back( l );
				} else {
					large.push_back( l );
				}
			}

			// Remaining entries get probability 1.0 (numerical cleanup)
			while( !large.empty() ) {
				probabilities[large.back()] = 1.0;
				aliases[large.back()] = large.back();
				large.pop_back();
			}

			while( !small.empty() ) {
				probabilities[small.back()] = 1.0;
				aliases[small.back()] = small.back();
				small.pop_back();
			}
		}

		/// Samples an index from the distribution using one uniform
		/// random number in [0,1).  O(1) time.
		unsigned int Sample( const double xi ) const
		{
			const unsigned int n = static_cast<unsigned int>( probabilities.size() );
			if( n == 0 ) {
				return 0;
			}

			const double scaled = xi * n;
			const unsigned int idx = static_cast<unsigned int>( scaled );
			const unsigned int safeIdx = (idx >= n) ? (n - 1) : idx;
			const double frac = scaled - safeIdx;

			if( frac < probabilities[safeIdx] ) {
				return safeIdx;
			} else {
				return aliases[safeIdx];
			}
		}

		/// Returns the exact selection probability for a given index.
		/// O(1) time.
		double Pdf( const unsigned int index ) const
		{
			if( index >= normalizedWeights.size() ) {
				return 0;
			}
			return normalizedWeights[index];
		}

		/// Returns the total weight across all entries.
		double TotalWeight() const { return totalWeight; }

		/// Returns the number of entries in the table.
		unsigned int Size() const { return static_cast<unsigned int>( probabilities.size() ); }

		/// Returns true if the table has been built with at least one entry.
		bool IsValid() const { return !probabilities.empty() && totalWeight > 0; }

	private:
		std::vector<double>			probabilities;		///< Threshold for choosing primary vs alias
		std::vector<unsigned int>	aliases;			///< Alias index for each bin
		std::vector<double>			normalizedWeights;	///< True probability of each entry
		double						totalWeight;		///< Sum of all input weights
	};
}

#endif
