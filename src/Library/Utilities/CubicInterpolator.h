//////////////////////////////////////////////////////////////////////
//
//  CubicInterpolator.h - Defines a cubic interpolator
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CUBIC_INTERPOLATOR_
#define CUBIC_INTERPOLATOR_

#include "../Interfaces/ICubicInterpolator.h"
#include "Reference.h"

namespace RISE
{
	//! A cubic interpolator uses a cubic spline to interpolate.  It takes a matrix 
	//! which describes the weights.
	template< class T >
	class CubicInterpolator : 
		public ICubicInterpolator<T>,
		public virtual Implementation::Reference
	{
	protected:
		const Matrix4 m;

		virtual ~CubicInterpolator(){};

	public:
		CubicInterpolator( const Matrix4& m_ ) : m( m_ ) {};

		T	Interpolate2Values( 
			typename std::vector<T>& values, 
			typename std::vector<T>::const_iterator& first, 
			typename std::vector<T>::const_iterator& second, 
			const Scalar x ) const
		{
			// y0 = the point before a
			// y1 = the point a
			// y2 = the point b
			// y3 = the after b
			const T& y1 = *first;
			const T& y2 = *second;

			const T& y0 = first > values.begin() ? *(first-1) : y1;
			const T& y3 = second < (values.end()-1) ? *(second+1) : y2;

			return InterpolateValues( y0, y1, y2, y3, x );
		}

		//! Interpolates between two values, given all the possible values
		/// \return The interpolated value
		T InterpolateValues( 
			const T& y0,									///< [in] The control point before the one we are interested in
			const T& y1,									///< [in] The first control point we are interested in
			const T& y2,									///< [in] The second control point we are interested in
			const T& y3,									///< [in] The control point after the one we are interested in
			const Scalar mu									///< [in] Specifies the interpolate amount, scalar from [0..1]
			) const
		{
			const Scalar mu2 = mu*mu;
			const Scalar mu3 = mu2*mu;

			const T a0 = m._00*y0 + m._01*y1 + m._02*y2 + m._03*y3;
			const T a1 = m._10*y0 + m._11*y1 + m._12*y2 + m._13*y3;
			const T a2 = m._20*y0 + m._21*y1 + m._22*y2 + m._23*y3;
			const T a3 = m._30*y0 + m._31*y1 + m._32*y2 + m._33*y3;
			
			return(a0*mu3+a1*mu2+a2*mu+a3);
		}
	};

	static const Matrix4 CatmullRomMatrix = Matrix4( 
			-1.0/2.0,  3.0/2.0, -3.0/2.0,  1.0/2.0,
	  		2.0/2.0, -5.0/2.0,  4.0/2.0, -1.0/2.0,
			-1.0/2.0,  0.0/2.0,  1.0/2.0,  0.0/2.0,
			0.0/2.0,  2.0/2.0,  0.0/2.0,  0.0/2.0 );

	template< class T >
	class CatmullRomCubicInterpolator : public CubicInterpolator<T>
	{
	protected:
		virtual ~CatmullRomCubicInterpolator(){};

	public:
		CatmullRomCubicInterpolator() : CubicInterpolator<T>( CatmullRomMatrix ) {};
	};

	static const Matrix4 UniformBSplineMatrix = Matrix4(
			-1.0/6.0,  3.0/6.0, -3.0/6.0,  1.0/6.0,
			3.0/6.0, -6.0/6.0,  3.0/6.0,  0.0/6.0,
			-3.0/6.0,  0.0/6.0,  3.0/6.0,  0.0/6.0,
			1.0/6.0,  4.0/6.0,  1.0/6.0,  0.0/6.0 );
	template< class T >
	class UniformBSplineCubicInterpolator : public CubicInterpolator<T>
	{
	protected:
		virtual ~UniformBSplineCubicInterpolator(){};

	public:
		UniformBSplineCubicInterpolator() : CubicInterpolator<T>( UniformBSplineMatrix ) {};
	};
}

#endif
