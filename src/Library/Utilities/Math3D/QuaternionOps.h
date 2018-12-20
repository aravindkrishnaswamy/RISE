//////////////////////////////////////////////////////////////////////
//
//  QuaternionOps.h - Contains operations on quaternions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 6, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef QUATERNION_OPS_
#define QUATERNION_OPS_

namespace RISE
{
	namespace QuaternionOps
	{
		// Dot product
		inline Scalar Dot( const Quaternion& a, const Quaternion& b ) 
		{
			return a.s * b.s + Vector3Ops::Dot(a.v,b.v);
		}

		// Unary negation
		inline Quaternion UnaryNegation( const Quaternion& q )
		{
			return Quaternion( -q.s, -q.v );
		}

		// Unary conjugate
		inline Quaternion UnaryConjugate( const Quaternion& q )
		{
			return Quaternion( q.s, -q.v );
		}

		// Normal
		inline Scalar Normal( const Quaternion& q )
		{
			return Scalar( q.s*q.s + Vector3Ops::SquaredModulus(q.v) );
		}

		// Magnitude
		inline Scalar Magnitude( const Quaternion& q )
		{
			return Scalar( sqrt( Normal(q) ) );
		}

		// Inverse
		inline Quaternion Inverse( const Quaternion& q ) 
		{
			const Scalar temp = Normal(q);

			if( temp == 0.0 ) {
				return q;
			}

			return UnaryConjugate(q) * temp;
		}

		// Exponentiation
		inline Quaternion Pow( const Quaternion& q, const Scalar t )
		{
			Quaternion	ret;

			const Scalar theta = Scalar( acos(q.s) );
			const Scalar factor = Scalar(sin(t * theta)) / Scalar(sin(theta));

			ret.s = Scalar(cos(t * theta));
			ret.v = q.v * factor;

			return ret;
		}

		inline Quaternion AxisRotation( const Vector3& v, const Direction& d )
		{
			Scalar		t = d.x * 0.5;
			Scalar		S = Scalar( sin( t ) );
			Scalar		C = Scalar( cos( t ) );

			return Quaternion( C, S * v );
		}

		inline Quaternion Slerp( const Quaternion& p, const Quaternion& q, const Scalar t )
		{
			Scalar cs = Dot(p, q);
			Scalar sn = sqrt( fabs(1.0-cs*cs) );
			if ( fabs( sn ) < 0.00001 ) {
				return p;
			}

			bool notFlipped = true;
			if ( cs < 0.0 ) {
				cs = -cs;
				notFlipped = false;
			}

			Scalar angle = atan2(sn,cs);
			Scalar invSn = 1.0/sn;
			Scalar c0 = Scalar( sin((1.0-t)*angle)*invSn );
			Scalar c1 = Scalar( sin(t*angle)*invSn );

			return ( notFlipped ? (c0*p + c1*q) : (c0*p + UnaryNegation(c1*q)) );
		}

		// Division
		inline Quaternion Divide( const Quaternion& a, const Quaternion& b ) 
		{
			return a * Inverse(b);
		}
	}
}

#endif
