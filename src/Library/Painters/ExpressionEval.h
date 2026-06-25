//////////////////////////////////////////////////////////////////////
//
//  ExpressionEval.h - A small, self-contained math-expression engine
//  for procedural 2D fields authored IN the scene file.
//
//  It compiles an expression string ONCE (recursive-descent parse to a
//  flat postfix instruction list) and evaluates it fast per (u, v) with
//  a stack machine -- no per-eval allocation, no AST pointer chasing.
//
//  Variables:  u, v          -- the query coordinates (set per eval)
//              + any named `params` (constants) and `defs` (named
//                sub-expressions / let-bindings) registered before
//                compiling.  A def may reference u, v, earlier params,
//                and earlier defs -- so a complex field (e.g. guilloché)
//                is built up as readable named steps instead of one
//                giant string.
//  Operators:  + - * / %  ^(right-assoc)  unary -  and the comparisons
//              < > <= >= == !=  (yield 1.0 / 0.0).
//  Functions:  sin cos tan asin acos atan exp log sqrt abs floor ceil
//              frac sign  (unary);  atan2 mod min max pow hypot step
//              (binary);  clamp smoothstep mix select  (ternary).
//
//  Designed for displacement bakes (compiled once, ~10^5 evals at parse
//  time) and small procedural textures.  Not a hot per-sample path
//  replacement for a hand-written C++ field, by design -- it trades a
//  little speed for authoring patterns without recompiling the engine.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXPRESSION_EVAL_
#define EXPRESSION_EVAL_

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdlib>
#include "../Utilities/Math3D/Math3D.h"	// Scalar

namespace RISE
{
	namespace Implementation
	{
		//! Compiled program over a shared variable environment.  Build it
		//! through ExpressionProgram::Builder: register params + defs, then
		//! Finalize an expression.  Eval sets u/v and runs every def in order
		//! followed by the final expression.
		class ExpressionProgram
		{
		public:
			//! One compiled expression: a postfix instruction list referencing
			//! environment slots (variables) by index.
			struct Compiled
			{
				enum Op {
					kConst, kVar, kAdd, kSub, kMul, kDiv, kMod, kPow, kNeg,
					kLt, kGt, kLe, kGe, kEq, kNe, kFunc
				};
				struct Instr { Op op; Scalar val; int idx; int fn; };
				std::vector<Instr> code;
				int writeSlot;	//!< env slot this expression writes (a def), or -1 (the final expr)
			};

			//! Slot / stack / parse-depth caps -- a compiled program is
			//! rejected at build time if it would exceed them, so Eval can use
			//! fixed stack-allocated buffers (no per-call heap, no overflow).
			static const int kMaxSlots      = 512;	//!< named vars (u,v + params + defs)
			static const int kStackCap      = 512;	//!< value-stack depth
			static const int kMaxParseDepth = 200;	//!< recursive-descent nesting

			//! Finiteness test by exponent-bit inspection.  CAVEAT: under -O3 -flto -ffast-math the
			//! compiler may ASSUME finiteness and fold this to always-true (the union read is optimized
			//! away), so it is NOT a reliable hard guard in the production build -- it is best-effort.
			//! A caller needing a DEFINITE non-finite rejection must inspect the FORMATTED string (the
			//! byte scan is compiler-opaque), as Cst.cpp's TryEvalExprValue does.  (isnan/isinf are also
			//! unreliable under -ffast-math; this is the standing 'ffast-math: no infinity' limitation.)
			static bool IsFinite( const Scalar x )
			{
				union { double d; unsigned long long u; } c;
				c.d = (double)x;
				return ( ( c.u >> 52 ) & 0x7FF ) != 0x7FF;	// exponent != all-ones (not inf/nan)
			}

			//! Reset the environment + bind u, v, then run the program.
			//! Fixed stack-allocated env -- no per-call allocation.
			Scalar Eval( const Scalar u, const Scalar v ) const
			{
				Scalar env[ kMaxSlots ];
				const size_t n = m_initEnv.size();
				for( size_t i = 0; i < n; ++i ) env[i] = m_initEnv[i];
				env[ m_uSlot ] = u;
				env[ m_vSlot ] = v;
				for( size_t i = 0; i < m_defs.size(); ++i ) {
					env[ m_defs[i].writeSlot ] = RunOne( m_defs[i], env );
				}
				return RunOne( m_final, env );
			}

			bool IsValid() const { return m_valid; }
			const std::string& Error() const { return m_error; }

			//////////////////////////////////////////////////////////
			// Builder
			//////////////////////////////////////////////////////////
			class Builder
			{
			public:
				Builder()
				{
					// u, v are always present (slots 0, 1).
					Slot( "u" ); Slot( "v" );
				}

				//! A named numeric constant.
				void AddParam( const std::string& name, const Scalar value )
				{
					const int s = Slot( name );
					EnsureInit( s );
					m_init[s] = value;
				}

				//! A named sub-expression (let-binding), evaluated in
				//! registration order; visible to later defs + the final expr.
				//! Returns false (and sets error) on a parse failure.
				bool AddDef( const std::string& name, const std::string& expr )
				{
					const int s = Slot( name );
					EnsureInit( s );
					Compiled c;
					if( !Compile( expr, c ) ) {
						return false;
					}
					c.writeSlot = s;
					m_defs.push_back( c );
					return true;
				}

				//! Compile the final expression and produce the program.
				bool Finalize( const std::string& expr, ExpressionProgram& out )
				{
					Compiled c;
					if( !Compile( expr, c ) ) {
						out.m_valid = false;
						out.m_error = m_error;
						return false;
					}
					if( (int)m_names.size() > kMaxSlots ) {
						m_error = "too many variables (param + def); limit is 512";
						out.m_valid = false; out.m_error = m_error;
						return false;
					}
					c.writeSlot = -1;
					out.m_uSlot = 0;
					out.m_vSlot = 1;
					out.m_initEnv.assign( m_names.size(), Scalar(0) );
					for( std::map<int,Scalar>::const_iterator it = m_init.begin(); it != m_init.end(); ++it ) {
						out.m_initEnv[ it->first ] = it->second;
					}
					out.m_defs = m_defs;
					out.m_final = c;
					out.m_valid = true;
					return true;
				}

				const std::string& Error() const { return m_error; }

			private:
				std::vector<std::string> m_names;	// slot index -> name
				std::map<std::string,int> m_index;	// name -> slot
				std::map<int,Scalar> m_init;		// param initial values
				std::vector<Compiled> m_defs;
				std::string m_error;

				int Slot( const std::string& name )
				{
					std::map<std::string,int>::const_iterator it = m_index.find( name );
					if( it != m_index.end() ) return it->second;
					const int s = (int)m_names.size();
					m_names.push_back( name );
					m_index[ name ] = s;
					return s;
				}
				void EnsureInit( int ) {}	// m_initEnv sized at Finalize

				// --- function table ---
				static int FuncId( const std::string& n, int& arity )
				{
					struct F { const char* n; int id; int ar; };
					static const F fns[] = {
						{"sin",1,1},{"cos",2,1},{"tan",3,1},{"asin",4,1},{"acos",5,1},
						{"atan",6,1},{"exp",7,1},{"log",8,1},{"sqrt",9,1},{"abs",10,1},
						{"floor",11,1},{"ceil",12,1},{"frac",13,1},{"sign",14,1},
						{"atan2",20,2},{"mod",21,2},{"min",22,2},{"max",23,2},
						{"pow",24,2},{"hypot",25,2},{"step",26,2},
						{"clamp",30,3},{"smoothstep",31,3},{"mix",32,3},{"select",33,3},
					};
					for( size_t i = 0; i < sizeof(fns)/sizeof(fns[0]); ++i ) {
						if( n == fns[i].n ) { arity = fns[i].ar; return fns[i].id; }
					}
					arity = -1;
					return -1;
				}

				//////////////////////////////////////////////////
				// Recursive-descent parser -> postfix emit
				//////////////////////////////////////////////////
				struct Tok { enum T { Num, Ident, Op, LP, RP, Comma, End } t; Scalar num; std::string s; };

				bool Tokenize( const std::string& e, std::vector<Tok>& out )
				{
					size_t i = 0, n = e.size();
					while( i < n ) {
						const char c = e[i];
						if( c == ' ' || c == '\t' || c == '\n' || c == '\r' ) { ++i; continue; }
						if( ( c >= '0' && c <= '9' ) || c == '.' ) {
							const char* start = e.c_str() + i;
							char* end = 0;
							const double val = strtod( start, &end );
							if( end == start ) { m_error = "bad number"; return false; }
							Tok t; t.t = Tok::Num; t.num = Scalar(val);
							out.push_back( t );
							i += ( end - start );
							continue;
						}
						if( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' ) {
							size_t j = i;
							while( j < n && ( ( e[j]>='a'&&e[j]<='z' ) || ( e[j]>='A'&&e[j]<='Z' ) || ( e[j]>='0'&&e[j]<='9' ) || e[j]=='_' ) ) ++j;
							Tok t; t.t = Tok::Ident; t.s = e.substr( i, j - i );
							out.push_back( t );
							i = j;
							continue;
						}
						if( c == '(' ) { Tok t; t.t = Tok::LP; out.push_back(t); ++i; continue; }
						if( c == ')' ) { Tok t; t.t = Tok::RP; out.push_back(t); ++i; continue; }
						if( c == ',' ) { Tok t; t.t = Tok::Comma; out.push_back(t); ++i; continue; }
						// operators, incl. two-char comparisons
						Tok t; t.t = Tok::Op;
						if( ( c=='<'||c=='>'||c=='='||c=='!' ) && i+1<n && e[i+1]=='=' ) { t.s = e.substr(i,2); i+=2; }
						else { t.s = std::string(1,c); ++i; }
						out.push_back( t );
					}
					Tok t; t.t = Tok::End;
					out.push_back( t );
					return true;
				}

				// parser state
				const std::vector<Tok>* m_toks;
				size_t m_pos;
				Compiled* m_emit;
				int m_parseDepth;

				const Tok& Cur() const { return (*m_toks)[ m_pos ]; }
				void Advance() { ++m_pos; }

				bool Compile( const std::string& expr, Compiled& out )
				{
					std::vector<Tok> toks;
					if( !Tokenize( expr, toks ) ) return false;
					m_toks = &toks; m_pos = 0; m_emit = &out; m_parseDepth = 0;
					out.code.clear();
					if( !ParseCmp() ) return false;
					if( Cur().t != Tok::End ) { m_error = "trailing tokens in `" + expr + "`"; return false; }
					// compile-time value-stack bound: simulate the postfix stack
					// effect so a deferred-operand expression can never overflow
					// the fixed Eval stack.
					int sp = 0, mx = 0;
					for( size_t i = 0; i < out.code.size(); ++i ) {
						const Compiled::Instr& in = out.code[i];
						switch( in.op ) {
						case Compiled::kConst: case Compiled::kVar: sp += 1; break;
						case Compiled::kNeg: break;
						case Compiled::kFunc: { const int id = in.fn; const int ar = ( id < 20 ) ? 1 : ( id < 30 ? 2 : 3 ); sp -= ( ar - 1 ); } break;
						default: sp -= 1; break;	// binary ops
						}
						if( sp > mx ) mx = sp;
					}
					if( mx > ExpressionProgram::kStackCap ) { m_error = "expression too large (value-stack depth exceeds 512)"; return false; }
					return true;
				}

				// depth-bounded entry to the recursive descent (rejects
				// pathologically nested input before it overflows the C++ stack).
				bool ParseCmp()
				{
					if( ++m_parseDepth > ExpressionProgram::kMaxParseDepth ) { m_error = "expression nesting too deep"; --m_parseDepth; return false; }
					const bool ok = ParseCmpImpl();
					--m_parseDepth;
					return ok;
				}

				void EmitConst( Scalar v ) { Compiled::Instr in; in.op=Compiled::kConst; in.val=v; in.idx=-1; in.fn=-1; m_emit->code.push_back(in); }
				void EmitVar( int idx )    { Compiled::Instr in; in.op=Compiled::kVar; in.val=0; in.idx=idx; in.fn=-1; m_emit->code.push_back(in); }
				void EmitOp( Compiled::Op o ) { Compiled::Instr in; in.op=o; in.val=0; in.idx=-1; in.fn=-1; m_emit->code.push_back(in); }
				void EmitFunc( int fn )    { Compiled::Instr in; in.op=Compiled::kFunc; in.val=0; in.idx=-1; in.fn=fn; m_emit->code.push_back(in); }

				// cmp := add ( (<|>|<=|>=|==|!=) add )?
				bool ParseCmpImpl()
				{
					if( !ParseAdd() ) return false;
					if( Cur().t == Tok::Op ) {
						const std::string& o = Cur().s;
						Compiled::Op op;
						if( o=="<" ) op=Compiled::kLt; else if( o==">" ) op=Compiled::kGt;
						else if( o=="<=" ) op=Compiled::kLe; else if( o==">=" ) op=Compiled::kGe;
						else if( o=="==" ) op=Compiled::kEq; else if( o=="!=" ) op=Compiled::kNe;
						else return true;
						Advance();
						if( !ParseAdd() ) return false;
						EmitOp( op );
					}
					return true;
				}
				// add := mul ( (+|-) mul )*
				bool ParseAdd()
				{
					if( !ParseMul() ) return false;
					while( Cur().t == Tok::Op && ( Cur().s=="+" || Cur().s=="-" ) ) {
						const bool plus = ( Cur().s=="+" );
						Advance();
						if( !ParseMul() ) return false;
						EmitOp( plus ? Compiled::kAdd : Compiled::kSub );
					}
					return true;
				}
				// mul := unary ( (*|/|%) unary )*
				bool ParseMul()
				{
					if( !ParseUnary() ) return false;
					while( Cur().t == Tok::Op && ( Cur().s=="*" || Cur().s=="/" || Cur().s=="%" ) ) {
						const std::string o = Cur().s;
						Advance();
						if( !ParseUnary() ) return false;
						EmitOp( o=="*" ? Compiled::kMul : ( o=="/" ? Compiled::kDiv : Compiled::kMod ) );
					}
					return true;
				}
				// unary := (-|+)? pow
				bool ParseUnary()
				{
					if( Cur().t == Tok::Op && ( Cur().s=="-" || Cur().s=="+" ) ) {
						const bool neg = ( Cur().s=="-" );
						Advance();
						if( !ParseUnary() ) return false;
						if( neg ) EmitOp( Compiled::kNeg );
						return true;
					}
					return ParsePow();
				}
				// pow := atom (^ unary)?    right-assoc
				bool ParsePow()
				{
					if( !ParseAtom() ) return false;
					if( Cur().t == Tok::Op && Cur().s=="^" ) {
						Advance();
						if( !ParseUnary() ) return false;
						EmitOp( Compiled::kPow );
					}
					return true;
				}
				// atom := NUM | IDENT | IDENT '(' args ')' | '(' cmp ')'
				bool ParseAtom()
				{
					const Tok& t = Cur();
					if( t.t == Tok::Num ) { EmitConst( t.num ); Advance(); return true; }
					if( t.t == Tok::LP ) {
						Advance();
						if( !ParseCmp() ) return false;
						if( Cur().t != Tok::RP ) { m_error = "missing )"; return false; }
						Advance();
						return true;
					}
					if( t.t == Tok::Ident ) {
						const std::string name = t.s;
						Advance();
						if( Cur().t == Tok::LP ) {
							// function call
							int arity = 0;
							const int fn = FuncId( name, arity );
							if( fn < 0 ) { m_error = "unknown function `" + name + "`"; return false; }
							Advance();	// (
							int got = 0;
							if( Cur().t != Tok::RP ) {
								for( ;; ) {
									if( !ParseCmp() ) return false;
									++got;
									if( Cur().t == Tok::Comma ) { Advance(); continue; }
									break;
								}
							}
							if( Cur().t != Tok::RP ) { m_error = "missing ) in " + name; return false; }
							Advance();
							if( got != arity ) { m_error = name + " expects " + std::to_string(arity) + " args"; return false; }
							EmitFunc( fn );
							return true;
						}
						// variable / param / def reference
						std::map<std::string,int>::const_iterator it = m_index.find( name );
						if( it == m_index.end() ) {
							// allow a couple of math constants
							if( name == "pi" )  { EmitConst( Scalar(3.14159265358979323846) ); return true; }
							if( name == "tau" ) { EmitConst( Scalar(6.28318530717958647692) ); return true; }
							if( name == "e" )   { EmitConst( Scalar(2.71828182845904523536) ); return true; }
							m_error = "unknown variable `" + name + "`";
							return false;
						}
						EmitVar( it->second );
						return true;
					}
					m_error = "unexpected token";
					return false;
				}
			};

		private:
			int m_uSlot, m_vSlot;
			std::vector<Scalar> m_initEnv;
			std::vector<Compiled> m_defs;
			Compiled m_final;
			bool m_valid;
			std::string m_error;

			ExpressionProgram() : m_uSlot(0), m_vSlot(1), m_valid(false) {}
			friend class Builder;

		public:
			//! Default-constructed program is invalid until a Builder fills it.
			static ExpressionProgram Invalid() { return ExpressionProgram(); }

			static Scalar CallFunc( int fn, const Scalar* a )
			{
				switch( fn )
				{
				case 1:  return std::sin(a[0]);
				case 2:  return std::cos(a[0]);
				case 3:  return std::tan(a[0]);
				case 4:  return std::asin(a[0]);
				case 5:  return std::acos(a[0]);
				case 6:  return std::atan(a[0]);
				case 7:  return std::exp(a[0]);
				case 8:  return std::log(a[0]);
				case 9:  return std::sqrt(a[0]);
				case 10: return std::fabs(a[0]);
				case 11: return std::floor(a[0]);
				case 12: return std::ceil(a[0]);
				case 13: return a[0] - std::floor(a[0]);				// frac
				case 14: return ( a[0] > 0 ) ? Scalar(1) : ( a[0] < 0 ? Scalar(-1) : Scalar(0) );
				case 20: return std::atan2(a[0],a[1]);
				case 21: { const Scalar m = a[1]; return ( m != 0 ) ? ( a[0] - std::floor( a[0]/m ) * m ) : Scalar(0); }	// floor-mod
				case 22: return std::min(a[0],a[1]);
				case 23: return std::max(a[0],a[1]);
				case 24: return std::pow(a[0],a[1]);
				case 25: return std::sqrt(a[0]*a[0]+a[1]*a[1]);		// hypot (ffast-math-safe form)
				case 26: return ( a[1] < a[0] ) ? Scalar(0) : Scalar(1);	// step(edge, x)
				case 30: return std::min( std::max( a[0], a[1] ), a[2] );	// clamp(x,lo,hi)
				case 31: { const Scalar t = std::min( std::max( ( a[2]-a[0] )/( a[1]-a[0] ), Scalar(0) ), Scalar(1) ); return t*t*(Scalar(3)-Scalar(2)*t); }	// smoothstep(e0,e1,x)
				case 32: return a[0] + ( a[1]-a[0] ) * a[2];			// mix(a,b,t)
				case 33: return ( a[0] != 0 ) ? a[1] : a[2];			// select(cond,a,b)
				default: return Scalar(0);
				}
			}

		private:
			static Scalar RunOne( const Compiled& c, const Scalar* env )
			{
				Scalar stack[ kStackCap ];
				int sp = 0;
				for( size_t i = 0; i < c.code.size(); ++i ) {
					const Compiled::Instr& in = c.code[i];
					switch( in.op )
					{
					case Compiled::kConst: stack[sp++] = in.val; break;
					case Compiled::kVar:   stack[sp++] = env[ in.idx ]; break;
					case Compiled::kNeg:   stack[sp-1] = -stack[sp-1]; break;
					case Compiled::kAdd:   stack[sp-2] = stack[sp-2] + stack[sp-1]; --sp; break;
					case Compiled::kSub:   stack[sp-2] = stack[sp-2] - stack[sp-1]; --sp; break;
					case Compiled::kMul:   stack[sp-2] = stack[sp-2] * stack[sp-1]; --sp; break;
					case Compiled::kDiv:   stack[sp-2] = ( stack[sp-1] != 0 ) ? ( stack[sp-2] / stack[sp-1] ) : Scalar(0); --sp; break;
					case Compiled::kMod:   { const Scalar m = stack[sp-1]; stack[sp-2] = ( m != 0 ) ? ( stack[sp-2] - std::floor( stack[sp-2]/m ) * m ) : Scalar(0); --sp; } break;
					case Compiled::kPow:   stack[sp-2] = std::pow( stack[sp-2], stack[sp-1] ); --sp; break;
					case Compiled::kLt:    stack[sp-2] = ( stack[sp-2] <  stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kGt:    stack[sp-2] = ( stack[sp-2] >  stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kLe:    stack[sp-2] = ( stack[sp-2] <= stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kGe:    stack[sp-2] = ( stack[sp-2] >= stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kEq:    stack[sp-2] = ( stack[sp-2] == stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kNe:    stack[sp-2] = ( stack[sp-2] != stack[sp-1] ) ? Scalar(1):Scalar(0); --sp; break;
					case Compiled::kFunc:
					{
						int arity = 1;
						// recover arity from id band
						const int id = in.fn;
						arity = ( id < 20 ) ? 1 : ( id < 30 ? 2 : 3 );
						sp -= arity;
						stack[sp] = CallFunc( id, &stack[sp] );
						++sp;
					} break;
					}
				}
				return ( sp > 0 ) ? stack[sp-1] : Scalar(0);
			}
		};
	}
}

#endif
