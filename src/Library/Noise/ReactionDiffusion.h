//////////////////////////////////////////////////////////////////////
//
//  ReactionDiffusion.h - Defines 3D reaction-diffusion (Turing)
//  patterns.  Precomputes a 3D grid of steady-state
//  activator-inhibitor concentrations using the Gray-Scott model.
//
//  Reference: Turing 1952 "The Chemical Basis of Morphogenesis"
//  Reference: Turk 1991, Witkin & Kass 1991
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 13, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef REACTION_DIFFUSION_
#define REACTION_DIFFUSION_

#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class ReactionDiffusion3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~ReactionDiffusion3D();

			unsigned int	nGridSize;		///< Grid resolution per axis
			Scalar*			pGrid;			///< Precomputed concentration grid
			Scalar			dDa;			///< Diffusion rate of activator
			Scalar			dDb;			///< Diffusion rate of inhibitor
			Scalar			dFeed;			///< Feed rate
			Scalar			dKill;			///< Kill rate
			unsigned int	nIterations;	///< Number of simulation steps

			/// Run the Gray-Scott reaction-diffusion simulation
			void Simulate();

		public:
			ReactionDiffusion3D(
				const unsigned int nGridSize_,
				const Scalar dDa_,
				const Scalar dDb_,
				const Scalar dFeed_,
				const Scalar dKill_,
				const unsigned int nIterations_
			);

			/// Evaluates the precomputed reaction-diffusion field.
			/// Returns a value in [0, 1].
			virtual Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const;
		};
	}
}

#endif
