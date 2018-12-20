//////////////////////////////////////////////////////////////////////
//
//  MathExpressionEvaluator.cpp - Implements math expression
//    evaluator
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

#include <pch.h>
//#include <process.h>
//#include <ctype.h>
#include <iostream>
#include <iomanip>
#include <stdlib.h>
#include "MathExpressionEvaluator.h"

namespace RISE
{
	namespace MathExpressionEvaluator
	{

		Scanner *Production::input;
		int Production::errorOccurred;
		MemStack Production::mem( 1024 );   // this should be more than enough
											// for any reasonable expression

		const char Constant = 'c';
		const char None = 'n';
		const char EOS = '\0';

		void Production::error( const char * str )
		{
			GlobalLog()->PrintEx( eLog_Error, "MathExpressionEvaluator:: %s", str ); 
			errorOccurred = 1;
		}

		void Production::error( char ch )
		{
			switch( ch )
			{
			case EOS:
				error( "extra input after expression" );
				break;
			case Constant:
				error( "number expected" );
				break;
			default:
				{
					char buf[] = "  expected.";
					buf[0] = ch;
					error( buf );
				}
			}
		}

		void Production::expect( char ch )
		{
			if( input->curToken() != ch ) {
				error( ch );
			}

			input->nextToken();
		}

		void Scanner::nextToken()
		{
			while( isspace(input[0]) ) {
				input = &input[1];
			}

			bool bDigit = !!isdigit( input[0] );

			// This will break whitespacing, but for our parser we don't care, since
			// there is no white space allowed in expressions
			if( start != input && !bDigit ) {
				bDigit = (!isdigit(input[-1]) && input[0] == '-');
			}

			if( bDigit ) {
				char *end;
				lastValue = strtod( input, &end );
				input = end;
				curTok = Constant;
			} else {
				curTok = input[0];
				if( input[0] != EOS ) {
					input = &input[1];
				}
			}
		}

		class ScalarValue : public Production
		{
		public:
			ScalarValue()
			{
				if( errorOccurred ) {
					return;
				}

				if( input->curToken() == Constant ) {
					value = input->value();
				} else {
					error( Constant );
				}
				input->nextToken();
			}

			Scalar eval()
			{
				return value;
			}

		private:
			Scalar value;

		};


		class Factor : public Production
		{
		public:
			Factor()
			{
				if( errorOccurred ) {
					return;
				}

				if( input->curToken() == '(' ) {
					input->nextToken();
					expr = new AddOp;
					expect( ')' );
				} else {
					expr = new ScalarValue;
				}
			}

			Scalar eval()
			{
				return expr->eval();
			}

		private:
			Production *expr;
		};


		AddOp::AddOp()
		{
			if( errorOccurred ) {
				return;
			}

			left = new MulOp;
			if( isAddOp( input->curToken() ) ) {
				op = input->curToken();
				input->nextToken();
				right = new AddOp;
			} else {
				op = None;
			}
		}

		Scalar AddOp::eval()
		{
			Scalar result = left->eval();
			switch( op )
			{
			case '+':
				result += right->eval();
				break;
			case '-':
				result -= right->eval();
				break;
			}

			return result;
		}

		MulOp::MulOp()
		{
			if( errorOccurred ) {
				return;
			}

			left = new Factor;
			if( isMulOp( input->curToken() ) ) {
				op = input->curToken();
				input->nextToken();
				right = new MulOp;
			} else {
				op = None;
			}
		}

		Scalar MulOp::eval()
		{
			Scalar result = left->eval();
			switch( op )
			{
			case '*':
				result *= right->eval();
				break;
			case '/':
				result /= right->eval();
				break;
			}
			return result;
		}




		Expression::Expression( const char *str )
		{
			input = new Scanner(str);
			errorOccurred = 0;
			expr = new AddOp;
			expect( EOS );
		}

		Expression::~Expression()
		{
			delete expr;
			delete input;
		}

		Scalar Expression::eval()
		{
			if( errorOccurred ) {
				return 0;
			} else {
				return expr->eval();
			}
		}
	}
}


