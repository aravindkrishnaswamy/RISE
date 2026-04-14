//////////////////////////////////////////////////////////////////////
//
//  WaveletNoise.h - Defines 3D wavelet noise.
//  Band-limited noise constructed via wavelet decomposition.
//  Precomputes a tiling noise tile, providing consistent detail
//  across all scales without spectral falloff.
//
//  Reference: Cook & DeRose 2005 "Wavelet Noise" (SIGGRAPH)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WAVELET_NOISE_
#define WAVELET_NOISE_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class WaveletNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~WaveletNoise3D();

			unsigned int	nTileSize;		///< Tile resolution (must be even)
			Scalar*			pTile;			///< Precomputed noise tile [nTileSize^3]
			Scalar			dPersistence;
			int				numOctaves;

			/// Generate the wavelet noise tile via downsample/upsample/subtract
			void GenerateTile();

			/// Downsample a 3D signal by factor 2
			static void Downsample( const Scalar* from, Scalar* to, int n, int stride );

			/// Upsample a 3D signal by factor 2
			static void Upsample( const Scalar* from, Scalar* to, int n, int stride );

			/// Evaluate a single band from the precomputed tile
			Scalar EvaluateTile( Scalar x, Scalar y, Scalar z ) const;

		public:
			WaveletNoise3D(
				const unsigned int nTileSize_,
				const Scalar dPersistence_,
				const int numOctaves_
			);

			/// Evaluates wavelet noise FBM at (x,y,z).
			/// Returns a value in [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
