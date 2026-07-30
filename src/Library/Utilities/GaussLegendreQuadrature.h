//////////////////////////////////////////////////////////////////////
//
//  GaussLegendreQuadrature.h - Shared fixed-order Gauss-Legendre rules
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GAUSS_LEGENDRE_QUADRATURE_
#define GAUSS_LEGENDRE_QUADRATURE_

#include "Math3D/Math3D.h"

namespace RISE
{
	namespace GaussLegendre21
	{
		static const unsigned int NodeCount = 21;

		/// Binary64 nodes and weights for the 21-point Gauss-Legendre rule
		/// mapped to [0,1]. Definitions live in the matching .cpp so every
		/// consumer shares one table.
		extern const Scalar Nodes[NodeCount];
		extern const Scalar Weights[NodeCount];

		template<typename Function>
		inline Scalar Integrate(
			const Scalar begin,
			const Scalar end,
			const Function& function
			)
		{
			const Scalar extent = end - begin;
			Scalar result = 0.0;
			for( unsigned int i = 0; i < NodeCount; ++i ) {
				result += Weights[i] * function( begin + extent * Nodes[i] );
			}
			return extent * result;
		}

		template<typename Function>
		inline Scalar IntegrateVisibleBand( const Function& function )
		{
			return Integrate( Scalar(380.0), Scalar(780.0), function );
		}
	}
}

#endif
