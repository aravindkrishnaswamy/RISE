//////////////////////////////////////////////////////////////////////
//
//  Optics.hpp - Inline implementation of templated optics functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 11, 2002
//  Tabs: 4
//  Comments: Must be included from Optics.h, do not include yourself
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "Color/Color.h"

namespace RISE
{
	template< class T >
	static inline void EnsurePositive( T& t )
	{
		if( t < 0 ) {
			t = 0.0;
		}
	}

	template<>
	inline void EnsurePositive( RISEPel& t )
	{
		ColorMath::EnsurePositve( t );
	}

	//! Computes fresnel conductor reflectance
	/// \return Conductor reflectance value
	template< class T >
	T Optics::CalculateConductorReflectance( 
		const Vector3& v,			///< [in] Incoming vector
		const Vector3& n,			///< [in] Normal at point of reflectance
		const T& Ni,				///< [in] Index of refraction of outside (IOR refracting from)
		const T& Nt,				///< [in] Index of refraction of inside (IOR refracting to)
		const T& kt					///< [in] Extinction
		)
	{
		Scalar cos = fabs(Vector3Ops::Dot(v, n));
		if( cos < NEARZERO ) {
			cos = NEARZERO;
		}

		const Scalar cos2 = cos*cos;
		Scalar sin2 = 1 - cos2;
		if( sin2 < 0.0 ) {
			sin2 = 0.0;
		}

		const Scalar sin = sqrt(sin2);
		const Scalar tan = sin / cos;
		const Scalar tan2 = tan*tan;

		const T Ni2 = Ni*Ni;
		const T Nt2 = Nt*Nt;
		const T kt2 = kt*kt;
		const T Ns  = Ni2*sin2;
		const T Nk  = 4*Nt2*kt2;
		T a2 = Nt2 - kt2 - Ns;
		const T Sq = sqrt(a2*a2 + Nk);

		T b2 = Sq - Nt2 + kt2 + Ns;
		a2 = Sq + Nt2 - kt2 - Ns;
		const T dNi = (0.5 / Ni2);

		a2 = a2 * dNi;
		b2 = b2 * dNi;

		EnsurePositive( a2 );
		EnsurePositive( b2 );

		const T a = sqrt(a2);
		const T ab2 = a2 + b2;
		const T dcos = 2*a*cos;
		const T astan = 2*a*sin*tan;
		const Scalar stan =  sin2*tan2;
		const T Rs = (ab2 - dcos  + cos2) / (ab2 + dcos + cos2);
		const T Rp = Rs * (ab2 - astan + stan) / (ab2 + astan + stan);
		return 0.5*(Rp + Rs);
	}
}


