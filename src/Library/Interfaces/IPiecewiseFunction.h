//////////////////////////////////////////////////////////////////////
//
//  IPiecewiseFunction.h - Interface to piecewise functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 15, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPIECEWISEFUNCTION_
#define IPIECEWISEFUNCTION_

#include "IFunction1D.h"

namespace RISE
{
	//
	// Interface to piecewise functions
	//
	class IPiecewiseFunction1D : public virtual IFunction1D
	{
	protected:
		IPiecewiseFunction1D(){};

	public:
		virtual ~IPiecewiseFunction1D(){};

		virtual void addControlPoint( const std::pair<Scalar,Scalar>& v ) = 0;
		virtual void addControlPoints( int count, const Scalar* x, const Scalar* y ) = 0;
		virtual void addControlPoints( int count, const std::pair<Scalar,Scalar>* v ) = 0;

		virtual void clearControlPoints( ) = 0;
		virtual void GenerateLUT( const int LUTsize ) = 0;
		virtual void setUseLUT( bool b ) = 0;

		virtual Scalar EvaluateFunctionAt( const Scalar& s ) const = 0;
		inline Scalar Evaluate( const Scalar variable ) const
		{
			return EvaluateFunctionAt( variable );
		}
	};

	class IPiecewiseFunction2D : public virtual IFunction2D
	{
	protected:
		IPiecewiseFunction2D(){};

	public:
		virtual ~IPiecewiseFunction2D(){};

		virtual bool addControlPoint( const Scalar value, const IFunction1D* pFunction ) = 0;
		virtual void clearControlPoints( ) = 0;

		virtual Scalar EvaluateFunctionAt( const Scalar& a, const Scalar& b ) const = 0;
		inline Scalar Evaluate( const Scalar x, const Scalar y ) const
		{
			return EvaluateFunctionAt( x, y );
		}
	};
}


#endif
