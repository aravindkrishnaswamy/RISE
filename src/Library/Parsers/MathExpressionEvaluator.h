//////////////////////////////////////////////////////////////////////
//
//  MathExpressionEvaluator.h - Defines classes for math evaluation
//
//  Author: Aravind Krishnaswamy, Original author unknown
//  Date of Birth: April 23, 2004
//  Tabs: 4
//  Comments:  Based off code from here:
//    http://www.programmersheaven.com/zone3/cat415/6663.htm
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MATH_EXPRESSION_EVALUATOR_
#define MATH_EXPRESSION_EVALUATOR_

#include "../Utilities/Math3D/Math3D.h"
#include "../Interfaces/ILog.h"

namespace RISE
{
	namespace MathExpressionEvaluator
	{

		class Scanner
		{
		public:
			Scanner( const char * str ) :
			input(str),
			start(str)
			{
				nextToken();
			}

			void nextToken();

			int curToken() { return curTok; }
			Scalar value() { return lastValue; }

		private:

			const char *input;
			const char *start;
			Scalar lastValue;
			int curTok;

		};

		class MemStack
		{
		public:
			MemStack( size_t sz ) :
			memBlock( new char[sz] ),
			size(sz),
			top(0)
			{}

			~MemStack()
			{
				delete[] memBlock;
			}

			void *allocate( size_t sz )
			{
				char *result = memBlock + top;
				top += sz;
				if( top > size ) {
					GlobalLog()->PrintEasyError( "Math expression stack ran out of memory" );
				}
				return result;
			}

			void free( void * ptr )
			{
				top = (char *)ptr - memBlock;
			}

		private:

			char *memBlock;

			size_t size;
			size_t top;
		};

		class Production
		{
		public:
			virtual ~Production(){};
			virtual Scalar eval() = 0;
			void *operator new( size_t sz ) { return mem.allocate( sz ); }
			void operator delete( void *ptr ) { mem.free( ptr ); }

		protected:
			static void error( const char * );
			static void error( char );
			void expect( char );

			static Scanner *input;
			static int errorOccurred;

		private:
			static MemStack mem;

		};

		class AddOp : public Production
		{
		public:
			AddOp();

			Scalar eval();

			static int isAddOp( char ch ) { return ch == '+' || ch == '-'; }

		private:
			int op;
			Production *left;
			Production *right;

		};

		class MulOp : public Production
		{
		public:
			MulOp();
			Scalar eval();

			static int isMulOp( char ch ) { return ch == '*' || ch == '/'; }

		private:
			int op;
			Production *left;
			Production *right;

		};


		class Expression : public Production
		{
		public:
			Expression( const char * );
			virtual ~Expression();

			Scalar eval();
			int error() { return errorOccurred; }

		private:
			Production *expr;

		};
	}
}

#endif
