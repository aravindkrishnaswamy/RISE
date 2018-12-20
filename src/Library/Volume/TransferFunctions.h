//////////////////////////////////////////////////////////////////////
//
//  TransferFunctions.h - Class representing transfer functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef _TRANSFERFUNCTIONS
#define _TRANSFERFUNCTIONS

#include "../Interfaces/IFunction1D.h"
#include "../Interfaces/IFunction2D.h"
#include "../Utilities/Color/Color_Template.h"

namespace RISE
{
	class TransferFunctions
	{
	protected:
		const IFunction1D&		red;
		const IFunction1D&		green;
		const IFunction1D&		blue;
		const IFunction1D&		alpha;

	public:
		TransferFunctions(
			const IFunction1D& r,
			const IFunction1D& g,
			const IFunction1D& b,
			const IFunction1D& a
			) : 
		red( r ),
		green( g ),
		blue( b ),
		alpha( a )
		{
			red.addref();
			green.addref();
			blue.addref();
			alpha.addref();
		}

		virtual ~TransferFunctions( )
		{
			red.release();
			green.release();
			blue.release();
			alpha.release();
		}

		//! Computes a color from voxel intensity using the specified transfer functions
		RISEColor ComputeColorFromIntensity( const Scalar intensity ) const
		{
			// Compute a PEL from the given intensity using the transfer functions
			return RISEColor(	
				red.Evaluate( intensity ),
				green.Evaluate( intensity ),
				blue.Evaluate( intensity ),
				alpha.Evaluate( intensity )
				);
		}
	};

	class SpectralTransferFunctions
	{
	protected:
		const IFunction1D&		alpha;
		const IFunction2D&		spectral;

	public:
		SpectralTransferFunctions(
			const IFunction1D& a,
			const IFunction2D& s
			) : 
		alpha( a ),
		spectral( s )
		{
			alpha.addref();
		}

		virtual ~SpectralTransferFunctions( )
		{
			alpha.release();
		}

		//! Computes alpha from intensity
		Scalar ComputeAlphaFromIntensity( const Scalar intensity ) const
		{
			return alpha.Evaluate( intensity );
		}

		//! Computes the spectral intensity from volume intensity
		Scalar ComputeSpectralIntensityFromVolumeIntensity( const Scalar nm, const Scalar intensity ) const
		{
			return spectral.Evaluate( nm, intensity );
		}
	};
}

#endif

