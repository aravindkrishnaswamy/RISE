//////////////////////////////////////////////////////////////////////
//
//  SmoothedNoise.h - Contains 1D and 2D smoothed noise
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments:  The noise functions here were taken from a Perlin Noise
//  tutorial, available here:
//  http://freespace.virgin.net/hugo.elias/models/m_perlin.htm
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SMOOTHED_NOISE_
#define SMOOTHED_NOISE_

#include "Noise.h"
#include "../Interfaces/ILog.h"
namespace RISE
{
	namespace Implementation
	{
		class SmoothedNoise1D : public virtual IFunction1D, public virtual Reference
		{
		protected:
			Noise1D*		Noise1;

			virtual ~SmoothedNoise1D()
			{
				safe_release( Noise1 );
			}

		public:
			SmoothedNoise1D() : Noise1(0)
			{
				Noise1 = new Noise1D();
				GlobalLog()->PrintNew( Noise1, __FILE__, __LINE__, "noise" );
			}

			virtual inline Scalar Evaluate( const Scalar x ) const
			{
				return( Noise1->Evaluate(x)/2. + Noise1->Evaluate(x-1)/4. + Noise1->Evaluate(x+1)/4. );
			}
		};

		class SmoothedNoise2D : public virtual IFunction2D, public virtual Reference
		{
		protected:
			Noise2D*	Noise2;

			virtual ~SmoothedNoise2D()
			{
				safe_release( Noise2 );
			}

		public:
			SmoothedNoise2D() : Noise2( 0 )
			{
				Noise2 = new Noise2D();
				GlobalLog()->PrintNew( Noise2, __FILE__, __LINE__, "noise" );
			}

			virtual inline Scalar Evaluate( const Scalar x, const Scalar y ) const
			{
				Scalar	corners = ( Noise2->Evaluate(x-1, y-1)+Noise2->Evaluate(x+1, y-1)+Noise2->Evaluate(x-1, y+1)+Noise2->Evaluate(x+1, y+1) ) / 16.0;
				Scalar	sides	= ( Noise2->Evaluate(x-1, y  )+Noise2->Evaluate(x+1, y  )+Noise2->Evaluate(x  , y-1)+Noise2->Evaluate(x  , y+1) ) / 8.0;
				Scalar	center = Noise2->Evaluate(x, y) / 4.0;
				return( corners + sides + center );
			}
		};

		class SmoothedNoise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			Noise3D*	Noise3;

			virtual ~SmoothedNoise3D()
			{
				safe_release( Noise3 );
			}

		public:
			SmoothedNoise3D() : Noise3( 0 )
			{
				Noise3 = new Noise3D();
				GlobalLog()->PrintNew( Noise3, __FILE__, __LINE__, "noise" );
			}

			virtual inline Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
			{
				Scalar	corners = ( Noise3->Evaluate(x-1, y-1, z-1)+Noise3->Evaluate(x+1, y-1, z-1)+Noise3->Evaluate(x-1, y+1, z-1)+Noise3->Evaluate(x+1, y+1, z-1) + Noise3->Evaluate(x-1, y-1, z+1)+Noise3->Evaluate(x+1, y-1, z+1)+Noise3->Evaluate(x-1, y+1, z+1)+Noise3->Evaluate(x+1, y+1, z+1) ) / 64.0;
				Scalar	edges = ( Noise3->Evaluate(x, y+1, z+1)+ Noise3->Evaluate(x, y+1, z-1)+ Noise3->Evaluate(x, y-1, z+1)+ Noise3->Evaluate(x, y-1, z-1)+ Noise3->Evaluate(x+1, y, z+1)+ Noise3->Evaluate(x+1, y, z-1) + Noise3->Evaluate(x-1, y, z+1) +  Noise3->Evaluate(x-1, y, z-1)+ Noise3->Evaluate(x+1, y+1, z)+ Noise3->Evaluate(x+1, y-1, z)+ Noise3->Evaluate(x-1, y+1, z)+ Noise3->Evaluate(x-1, y-1, z) ) / 48.0;
				Scalar	adjacent = ( Noise3->Evaluate(x-1, y, z)+Noise3->Evaluate(x+1, y, z)+Noise3->Evaluate(x, y-1, z)+Noise3->Evaluate(x,y+1,z) + Noise3->Evaluate(x,y,z-1) + Noise3->Evaluate(x,y,z+1) ) / 12.0;
				Scalar	center = Noise3->Evaluate(x, y, z) / 8.0;
				return( corners + edges + adjacent + center );
			}
		};
	}
}

#endif
