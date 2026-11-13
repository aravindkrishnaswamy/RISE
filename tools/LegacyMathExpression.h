#ifndef RISE_LEGACY_MATH_EXPRESSION_H
#define RISE_LEGACY_MATH_EXPRESSION_H

#include <cctype>
#include <cstdlib>

namespace RISE::MathExpressionEvaluator
{
	// The v6 grammar evaluates operator chains from the right. Keep the
	// original strtod tokenization and evaluation order for migration.
	class Expression
	{
	public:
		explicit Expression( const char* text ) : input_( text ), start_( text )
		{
			Next();
			value_ = Add( 0 );
			failed_ |= token_ != '\0';
		}

		bool error() const { return failed_; }
		double eval() const { return failed_ ? 0 : value_; }

	private:
		static constexpr int kNumber = 256;
		static constexpr unsigned int kMaxDepth = 512;

		void Next()
		{
			while( std::isspace( static_cast<unsigned char>(*input_) ) ) ++input_;
			const auto current = static_cast<unsigned char>(*input_);
			if( std::isdigit( current ) ||
				(current == '-' && input_ != start_ &&
					!std::isdigit( static_cast<unsigned char>(input_[-1]) )) ) {
				char* end = nullptr;
				number_ = std::strtod( input_, &end );
				if( end != input_ ) {
					input_ = end;
					token_ = kNumber;
					return;
				}
			}
			token_ = current;
			if( current ) ++input_;
		}

		double Add( unsigned int depth )
		{
			const double left = Multiply( depth );
			const int op = token_;
			if( failed_ || (op != '+' && op != '-') ) return left;
			Next();
			const double right = Add( depth + 1 );
			return op == '+' ? left + right : left - right;
		}

		double Multiply( unsigned int depth )
		{
			const double left = Factor( depth );
			const int op = token_;
			if( failed_ || (op != '*' && op != '/') ) return left;
			Next();
			const double right = Multiply( depth + 1 );
			return op == '*' ? left * right : left / right;
		}

		double Factor( unsigned int depth )
		{
			if( depth == kMaxDepth ) {
				failed_ = true;
				return 0;
			}
			if( token_ == kNumber ) {
				const double value = number_;
				Next();
				return value;
			}
			if( token_ == '(' ) {
				Next();
				const double value = Add( depth + 1 );
				if( token_ == ')' ) Next();
				else failed_ = true;
				return value;
			}
			failed_ = true;
			return 0;
		}

		const char* input_;
		const char* start_;
		int token_ = 0;
		double number_ = 0;
		double value_ = 0;
		bool failed_ = false;
	};
}

#endif
