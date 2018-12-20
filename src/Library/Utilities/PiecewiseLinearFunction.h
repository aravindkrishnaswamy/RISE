//////////////////////////////////////////////////////////////////////
//
//  PiecewiseLinearFunction.h - A piecewise function where each 
//  piece is a linear function.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 20, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <vector>
#include <algorithm>
#include "../Interfaces/IPiecewiseFunction.h"
#include "Reference.h"

namespace RISE
{
	////////////////////////////////////
	//
	// A piecewise linear function.  Each control point is part of 
	// a linear function.  Also supports acceleration by using
	// look up tables.
	//
	////////////////////////////////////
	namespace Implementation
	{
		class PiecewiseLinearFunction1D : public virtual IPiecewiseFunction1D, public virtual Reference
		{
		protected:
			typedef std::pair<Scalar,Scalar> ControlPoint;
			typedef std::vector<ControlPoint> ControlPointsListType;
			ControlPointsListType	controlPoints;
			Scalar*					LUT;
			int						LUTsize;
			bool					bUseLUT;
			Scalar					dBegin;			// function begins here
			Scalar					dEnd;			// function ends here
			Scalar					dRange;			// range of values
			Scalar					dOVRange;		// 1.0 / range of values

			void deleteLUT( )
			{
				if( LUT ) {
					GlobalLog()->PrintDelete( LUT, __FILE__, __LINE__ );
					delete LUT;
					LUT = 0;
				}
			}

			Scalar getYValueFromFunction( const Scalar& s ) const
			{
				// We have been given some x value, we return 
				// a y value

				// Make sure we have enough control points
				if( controlPoints.size() < 2 ) {
					return 1.0;
				}

				// Boundary conditions
				if( s <= controlPoints.front().first ) {
					return controlPoints.front().second;
				}

				if( s >= controlPoints.back().first ) {
					return controlPoints.back().second;
				}

				ControlPoint search;
				search.first = s;

				ControlPointsListType::const_iterator it = 
					std::lower_bound( controlPoints.begin(), controlPoints.end(), search, ControlPointCompare );

				if( it->first == s ) {
					return it->second;
				}

				if( (it-1)->first == s ) {
					return (it-1)->second;
				}

				const Scalar d = (s - (*(it-1)).first) / ((*it).first - (*(it-1)).first);
				return (d*((*it).second - (*(it-1)).second))+(*(it-1)).second;
			}

			static inline bool ControlPointCompare( const std::pair<Scalar,Scalar>& lhs, const std::pair<Scalar,Scalar>& rhs )
			{
				return lhs.first < rhs.first;
			}

		public:
			PiecewiseLinearFunction1D( ) : 
			LUT( 0 ),
			LUTsize( 0 ),
			bUseLUT( false ),
			dBegin( 0 ),
			dEnd( 0 )
			{
			};

			virtual ~PiecewiseLinearFunction1D( )
			{
			}

			void clearControlPoints( )
			{
				controlPoints.clear();
			}

			inline void sort_and_compute_ranges()
			{
				std::sort( controlPoints.begin(), controlPoints.end(), ControlPointCompare );

				dBegin = controlPoints.front().first;
				dEnd = controlPoints.back().first;

				dRange = (dEnd-dBegin);
				if( dRange != 0.0 ) {
					dOVRange = 1.0 / dRange;
				} else {
					dOVRange = 1.0;
				}

				if( bUseLUT ) {
					GenerateLUT( LUTsize );
				}
			}

			void addControlPoint( const std::pair<Scalar,Scalar>& v )
			{
				// Adds the given control point to this function
				controlPoints.push_back( v );
				sort_and_compute_ranges();
			}

			void addControlPoints( int count, const Scalar* x, const Scalar* y )
			{
				if( count && x && y ) {
					for( int i=0; i<count; i++ ) {
						controlPoints.push_back( std::make_pair( x[i], y[i] ) );
					}

					sort_and_compute_ranges();
				}
			}

			void addControlPoints( int count, const std::pair<Scalar,Scalar>* v )
			{
				if( count && v ) {
					for( int i=0; i<count; i++ ) {
						controlPoints.push_back( v[i] );
					}

					sort_and_compute_ranges();
				}
			}

			Scalar EvaluateFunctionAt( const Scalar& s ) const
			{
				if( bUseLUT ) {
					if( s < dBegin ) {
						return LUT[ 0 ];
					}

					if( s > dEnd ) {
						return LUT[LUTsize-1];
					}

				
					return LUT[ int((s-dBegin)* dOVRange * (LUTsize-1)) ];
				
				}
					
				return getYValueFromFunction( s );
			}

			void GenerateLUT( const int LUTsize )
			{
				// Generate a lookup table for this curve
				
				// If one already exists delete it
				deleteLUT( );

				this->LUTsize = LUTsize;
				LUT = new Scalar[LUTsize];
				GlobalLog()->PrintNew( LUT, __FILE__, __LINE__, "LUT" );

				for( int i=0; i<LUTsize; i++ ) {
					LUT[i] = getYValueFromFunction( (Scalar)i / Scalar(LUTsize-1) * dRange + dBegin );
				}
			}

			void setUseLUT( bool b )
			{
				bUseLUT = b;
			}
		};

		class PiecewiseLinearFunction2D : public virtual IPiecewiseFunction2D, public virtual Reference
		{
		protected:
			typedef std::pair<Scalar,const IFunction1D*> FunctionPoint;
			typedef std::vector<FunctionPoint> ControlPointsListType;
			ControlPointsListType	controlPoints;
			Scalar					dBegin;			// function begins here
			Scalar					dEnd;			// function ends here
			Scalar					dRange;			// range of values
			Scalar					dOVRange;		// 1.0 / range of values

			Scalar getYValueFromFunction( const Scalar& a, const Scalar& b ) const
			{
				// We have been given some x value, we return 
				// a y value

				// Make sure we have enough control points
				if( controlPoints.size() < 2 ) {
					return 1.0;
				}

				// Boundary conditions
				if( a <= controlPoints.front().first ) {
					return controlPoints.front().second->Evaluate( b );
				}

				if( a >= controlPoints.back().first ) {
					return controlPoints.back().second->Evaluate( b );
				}

				FunctionPoint search;
				search.first = a;

				ControlPointsListType::const_iterator it = 
					std::lower_bound( controlPoints.begin(), controlPoints.end(), search, ControlPointCompare );

				const Scalar l = (*it).second->Evaluate(b);

				if( it->first == a ) {
					return l;
				}

				const Scalar r = (*(it-1)).second->Evaluate(b);

				if( (it-1)->first == a ) {
					return r;
				}

				Scalar d = (a - (*(it-1)).first) / ((*it).first - (*(it-1)).first);

				return (d*(l-r)+r);
			}

			static inline bool ControlPointCompare( const FunctionPoint& lhs, const FunctionPoint& rhs )
			{
				return lhs.first < rhs.first;
			}

		public:
			PiecewiseLinearFunction2D( ) : 
			dBegin( 0 ),
			dEnd( 0 )
			{
			};

			virtual ~PiecewiseLinearFunction2D( )
			{
			}

			void clearControlPoints( )
			{
				controlPoints.clear();
			}

			bool addControlPoint( const Scalar value, const IFunction1D* pFunction )
			{
				if( !pFunction ) {
					return false;
				}

				pFunction->addref();

				// Adds the given control point to this function
				controlPoints.push_back( std::make_pair( value, pFunction ) );

				// Sort the list
				std::sort( controlPoints.begin(), controlPoints.end(), ControlPointCompare );

				dBegin = controlPoints.front().first;
				dEnd = controlPoints.back().first;

				dRange = (dEnd-dBegin);
				if( dRange != 0.0 ) {
					dOVRange = 1.0 / dRange;
				} else {
					dOVRange = 1.0;
				}

				return true;
			}

			Scalar EvaluateFunctionAt( const Scalar& a, const Scalar& b ) const
			{				
				return getYValueFromFunction( a, b );
			}
		};
	}
}

