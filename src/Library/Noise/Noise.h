//////////////////////////////////////////////////////////////////////
//
//  Noise.h - Contains 1D, 2D and 3D noise functions
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

#ifndef NOISE_
#define NOISE_

#include "../Interfaces/IFunction1D.h"
#include "../Interfaces/IFunction2D.h"
#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class Noise1D : public virtual IFunction1D, public virtual Reference
		{
		protected:
			virtual ~Noise1D(){};

		public:
			virtual inline Scalar Evaluate( const Scalar variable ) const
			{
				int x = int(variable);
				x = (x<<13) ^ x;
				return ( 1.0 - Scalar( (x * (x * x * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.);
			}
		};

		class Noise2D : public virtual IFunction2D, public virtual Reference
		{
		protected:
			virtual ~Noise2D(){};

		public:
			virtual inline Scalar Evaluate( const Scalar x, const Scalar y ) const
			{
				int		n = int(x) * int(y) * 57;
				n = (n<<13) ^ n;
				return ( 1.0 - Scalar( (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.);
			}
		};

		class Noise3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			virtual ~Noise3D(){};

		public:
			virtual inline Scalar Evaluate( const Scalar x, const Scalar y, const Scalar z ) const
			{
				int		n = int(x) * int(y) * int(z) * 57;
				n = (n<<13) ^ n;
				return ( 1.0 - Scalar( (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.);
			}
		};
	}
}

#endif
