//////////////////////////////////////////////////////////////////////
//
//  AsciiSceneParser.cpp - Implementation of the really simple
//  AsciiSceneParser class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 22, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <vector>
#include <map>
#include <stack>
#include <string>
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include "AsciiSceneParser.h"
#include "AsciiCommandParser.h"
#include "StdOutProgress.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/MediaPathLocator.h"
#include "../Sampling/HaltonPoints.h"
#include "MathExpressionEvaluator.h"

#ifdef WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif

using namespace RISE;
using namespace RISE::Implementation;

#define MAX_CHARS_PER_LINE		8192
#define CURRENT_SCENE_VERSION	5

static const unsigned int std_string_npos = (unsigned int)(std::string::npos);
static MultiHalton mh;

inline bool string_split( const String& s, String& first, String& second, const char ch )
{
	String::const_iterator it = std::find( s.begin(), s.end(), ch );
	if( it==s.end() ) {
		return false;
	}

	first = String( s.begin(), it );
	second = String( it+1, s.end() );
	return true;
}

inline void make_string_from_tokens( String& s, String* tokens, const unsigned int num_tokens, const char* ch )
{
	// Concatenate all the tokens together with the given character between each
	// of the tokens

	if( num_tokens < 1 ) {
		return;
	}

	s.clear();
	s.concatenate(tokens[0]);

	for( unsigned int i=1; i<num_tokens; i++ ) {
		s.concatenate( ch );
		s.concatenate( tokens[i] );
	}
}

inline char evaluate_first_function_in_expression( String& token )
{
	std::string str( token.c_str() );
	std::string processed;

	unsigned int x = str.find_first_of( "scth" );

	if( x == std_string_npos ) {
		return 0;
	}

	if( x > 0 ) {
		processed = str.substr( 0, x );
		str = str.substr( x, str.size()-1 );
	}

	x = str.find_first_of( "(" );
	if( x == std_string_npos ) {
		return 2;
	}

	unsigned int y = str.find_first_of( ")" );
	if( y == std_string_npos ) {
		return 2;
	}

	// Take the expression from to y and evaluate it
	std::string szexpr = str.substr( x, y-x+1 );

	MathExpressionEvaluator::Expression expr( szexpr.c_str() );
	if( expr.error() ) {
		return 2;
	}

	Scalar val = 0;

	switch( str[0] )
	{
	case 's':
		// Sin
		if( str[1] == 'i' && str[2] == 'n' ) {
			val = sin( expr.eval() );
		} else if( str[1] == 'q' && str[2] == 'r' && str[3] == 't' ) {
			val = sqrt( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 'c':
		// Cos
		if( str[1] == 'o' && str[2] == 's' ) {
			val = cos( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 't':
		// Tan
		if( str[1] == 'a' && str[2] == 'n' ) {
			val = tan( expr.eval() );
		} else {
			return 2;
		}
		break;
	case 'h':
		// Halton random number sequence
		if( str[1] == 'a' && str[2] == 'l' ) {
			val = mh.next_halton(int(expr.eval()));
		} else {
			return 2;
		}
		break;
	}

	// assemble together
	static const unsigned int MAX_CHARS = 64;
	char evaluated[MAX_CHARS] = {0};
	snprintf( evaluated, MAX_CHARS, "%.12f", val );

	processed.append( evaluated );
	processed.append( str.substr( y+1, str.length()-1 ) );

	token = String(processed.c_str());

	return 1;
}

inline bool evaluate_functions_in_expression( String& token )
{
	for(;;) {
		char c = evaluate_first_function_in_expression( token );

		if( c==0 ) {
			return true;
		}

		if( c==2 ) {
			return false;
		}
	}
}

inline bool evaluate_expression( String& token )
{
	// The definition of an expression is very simple
	//   All it is is a sequence of numbers seperated by either a +, -, / or *
	//   Brackets may be used to ensure processing order
	//
	// All expressions are evaluated as double precision floating point
    //
	// Expressions must be in the form $(expr)

	// Before evaluating the expression, we should first go through and
	//   evaluate all the functional stuff like sin, cos, tan, sqrt, etc

	if( token.size() <= 4 ) {
		return false;
	}

	if( token[0] != '$' ||
		token[1] != '(' ||
		token[strlen(token.c_str())-1] != ')' ) {
		return false;
	}

	if( !evaluate_functions_in_expression( token ) ) {
		return false;
	}

	// We clamp out string
	const char * str = token.c_str();
	char* s = (char*)&str[1];

	MathExpressionEvaluator::Expression expr( s );
	if( expr.error() ) {
		return false;
	}

	static const unsigned int MAX_CHARS = 64;
	char evaluated[MAX_CHARS] = {0};
	snprintf( evaluated, MAX_CHARS, "%.12f", expr.eval() );

	token = String(evaluated);
	return true;
}

inline bool evaluate_expressions_in_tokens( String* tokens, const unsigned int num_tokens )
{
	for( unsigned int i=0; i<num_tokens; i++ ) {
		// Check to see if we have an expression
		if( tokens[i][0] == '$' ) {
			// This token contains an expression
			if( !evaluate_expression( tokens[i] ) ) {
				return false;
			}
		}
	}

	return true;
}

namespace RISE
{
	//
	// Interface to an ascii chunk parser
	//
	class IAsciiChunkParser
	{
	protected:
		IAsciiChunkParser(){};

	public:
		typedef std::vector<String> ParamsList;
		virtual ~IAsciiChunkParser(){};

		virtual bool ParseChunk( const ParamsList& in, IJob& pJob ) const = 0;
	};
}

//////////////////////////////////////////////////
// Implementation of the different kinds of
//   chunk parsers
//////////////////////////////////////////////////

//////////////////////////////////////////////////
// Chunk breakdown:
//  Chunks are identified by name.  There are
//  two levels to chunks.  At the root level,
//  there are 8 primary chunks:
//    Geometry, Painter, Material, Object, Camera
//    PhotonMap, Rasterizer and Shader
//
//  Most of these primary chunks have subchunks.
//  For example each type of painter is a subchunk
//  The main painter chunk itself doesn't mean
//  anything without a subchunk
//
//  A note about parsing:  Each chunk MUST begin
//    with a '{' on its own line and MUST end
//    with a '}' on its own line.  The braces
//    and all comments will be automatically removed
//    by the primary parser before being passed
//    to the chunk parser
//////////////////////////////////////////////////

namespace RISE
{
	namespace Implementation
	{
		namespace ChunkParsers
		{


			//////////////////////////////////////////
			// Painters
			//////////////////////////////////////////

			struct UniformColorPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double color[3] = {0,0,0};
					String color_space = "sRGB";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "color" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &color[0], &color[1], &color[2] );
						} else if( pname == "colorspace" ) {
							color_space = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddUniformColorPainter( name.c_str(), color, color_space.c_str() );
				}
			};

			struct SpectralPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					std::vector<double> wavelengths;
					std::vector<double> amplitudes;
					double nmbegin=400.0;
					double nmend=700.0;
					double scale=1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "nmbegin" ) {
							nmbegin = pvalue.toDouble();
						} else if( pname == "nmend" ) {
							nmend = pvalue.toDouble();
						} else if( pname == "scale" ) {
							scale = pvalue.toDouble();
						} else if( pname == "cp" ) {
							double nm, amp;
							sscanf( pvalue.c_str(), "%lf %lf", &nm, &amp );
							wavelengths.push_back( nm );
							amplitudes.push_back( amp );
						} else if( pname == "file" ) {
							// Load the spectral values from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double nm, amp;
									fscanf( f, "%lf %lf", &nm, &amp );
									wavelengths.push_back( nm );
									amplitudes.push_back( amp );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "nmfile" ) {
							// Load the wavelengths from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double nm;
									fscanf( f, "%lf", &nm );
									wavelengths.push_back( nm );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "ampfile" ) {
							// Load the amplitudes from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double amp;
									fscanf( f, "%lf", &amp );
									amplitudes.push_back( amp );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddSpectralColorPainter( name.c_str(), &amplitudes[0], &wavelengths[0], nmbegin, nmend, amplitudes.size(), scale );
				}
			};

			struct PngPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String filename = "none";
					char color_space = 1;
					char filter_type = 1;
					bool lowmemory = false;
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							filename = pvalue;
						} else if( pname == "color_space" ) {
							if( pvalue=="Rec709RGB_Linear" ) {
								color_space = 0;
							} else if( pvalue=="sRGB" ) {
								color_space = 1;
							} else if( pvalue=="ROMMRGB_Linear" ) {
								color_space = 2;
							} else if( pvalue=="ProPhotoRGB" ) {
								color_space = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "filter_type" ) {
							if( pvalue=="NNB" ) {
								filter_type = 0;
							} else if( pvalue=="Bilinear" ) {
								filter_type = 1;
							} else if( pvalue=="CatmullRom" ) {
								filter_type = 2;
							} else if( pvalue=="UniformBSpline" ) {
								filter_type = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "lowmemory" ) {
							lowmemory = pvalue.toBoolean();
						} else if( pname == "scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &scale[0], &scale[1], &scale[2] );
						} else if( pname == "shift" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &shift[0], &shift[1], &shift[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPNGTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift );
				}
			};

			struct HdrPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String filename = "none";
					char filter_type = 1;
					bool lowmemory = false;
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							filename = pvalue;
						} else if( pname == "filter_type" ) {
							if( pvalue=="NNB" ) {
								filter_type = 0;
							} else if( pvalue=="Bilinear" ) {
								filter_type = 1;
							} else if( pvalue=="CatmullRom" ) {
								filter_type = 2;
							} else if( pvalue=="UniformBSpline" ) {
								filter_type = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "lowmemory" ) {
							lowmemory = pvalue.toBoolean();
						} else if( pname == "scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &scale[0], &scale[1], &scale[2] );
						} else if( pname == "shift" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &shift[0], &shift[1], &shift[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddHDRTexturePainter( name.c_str(), filename.c_str(), filter_type, lowmemory, scale, shift );
				}
			};

			struct ExrPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String filename = "none";
					char color_space = 0;
					char filter_type = 1;
					bool lowmemory = false;
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							filename = pvalue;
						} else if( pname == "color_space" ) {
							if( pvalue=="Rec709RGB_Linear" ) {
								color_space = 0;
							} else if( pvalue=="sRGB" ) {
								color_space = 1;
							} else if( pvalue=="ROMMRGB_Linear" ) {
								color_space = 2;
							} else if( pvalue=="ProPhotoRGB" ) {
								color_space = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "filter_type" ) {
							if( pvalue=="NNB" ) {
								filter_type = 0;
							} else if( pvalue=="Bilinear" ) {
								filter_type = 1;
							} else if( pvalue=="CatmullRom" ) {
								filter_type = 2;
							} else if( pvalue=="UniformBSpline" ) {
								filter_type = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "lowmemory" ) {
							lowmemory = pvalue.toBoolean();
						} else if( pname == "scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &scale[0], &scale[1], &scale[2] );
						} else if( pname == "shift" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &shift[0], &shift[1], &shift[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddEXRTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift );
				}
			};

			struct TiffPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String filename = "none";
					char color_space = 1;
					char filter_type = 1;
					bool lowmemory = false;
					double scale[3] = {1,1,1};
					double shift[3] = {0,0,0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							filename = pvalue;
						} else if( pname == "color_space" ) {
							if( pvalue=="Rec709RGB_Linear" ) {
								color_space = 0;
							} else if( pvalue=="sRGB" ) {
								color_space = 1;
							} else if( pvalue=="ROMMRGB_Linear" ) {
								color_space = 2;
							} else if( pvalue=="ProPhotoRGB" ) {
								color_space = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "filter_type" ) {
							if( pvalue=="NNB" ) {
								filter_type = 0;
							} else if( pvalue=="Bilinear" ) {
								filter_type = 1;
							} else if( pvalue=="CatmullRom" ) {
								filter_type = 2;
							} else if( pvalue=="UniformBSpline" ) {
								filter_type = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown filter type `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "lowmemory" ) {
							lowmemory = pvalue.toBoolean();
						} else if( pname == "scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &scale[0], &scale[1], &scale[2] );
						} else if( pname == "shift" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &shift[0], &shift[1], &shift[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddTIFFTexturePainter( name.c_str(), filename.c_str(), color_space, filter_type, lowmemory, scale, shift );
				}
			};


			struct CheckerPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String colora = "none";
					String colorb = "none";
					double size = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "colora" ) {
							colora = pvalue;
						} else if( pname == "colorb" ) {
							colorb = pvalue;
						} else if( pname == "size" ) {
							size = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddCheckerPainter( name.c_str(), size, colora.c_str(), colorb.c_str() );
				}
			};

			struct LinesPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String colora = "none";
					String colorb = "none";
					double size = 1.0;
					bool vertical = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "colora" ) {
							colora = pvalue;
						} else if( pname == "colorb" ) {
							colorb = pvalue;
						} else if( pname == "size" ) {
							size = pvalue.toDouble();
						} else if( pname == "vertical" ) {
							vertical = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddLinesPainter( name.c_str(), size, colora.c_str(), colorb.c_str(), vertical );
				}
			};

			struct MandelbrotPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String colora = "none";
					String colorb = "none";
					double xstart = 0.0;
					double xend = 1.0;
					double ystart = 0.0;
					double yend = 1.0;
					double exponent = 12.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "colora" ) {
							colora = pvalue;
						} else if( pname == "colorb" ) {
							colorb = pvalue;
						} else if( pname == "xstart" ) {
							xstart = pvalue.toDouble();
						} else if( pname == "xend" ) {
							xend = pvalue.toDouble();
						} else if( pname == "ystart" ) {
							ystart = pvalue.toDouble();
						} else if( pname == "yend" ) {
							yend = pvalue.toDouble();
						} else if( pname == "exponent" ) {
							exponent = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddMandelbrotFractalPainter( name.c_str(), colora.c_str(), colorb.c_str(), xstart, xend, ystart, yend, exponent );
				}
			};

			struct Perlin2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String colora = "none";
					String colorb = "none";
					double persistence = 1.0;
					double scale[2] = {1.0,1.0};
					double shift[2] = {0,0};
					unsigned int octaves = 4;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "colora" ) {
							colora = pvalue;
						} else if( pname == "colorb" ) {
							colorb = pvalue;
						} else if( pname == "persistence" ) {
							persistence = pvalue.toDouble();
						} else if( pname == "scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &scale[0], &scale[1] );
						} else if( pname == "shift" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &shift[0], &shift[1] );
						} else if( pname == "octaves" ) {
							octaves = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPerlin2DPainter( name.c_str(), persistence, octaves, colora.c_str(), colorb.c_str(), scale, shift );
				}
			};

			struct Perlin3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String colora = "none";
					String colorb = "none";
					double persistence = 1.0;
					double scale[3] = {1.0,1.0,1.0};
					double shift[3] = {0,0,0};
					unsigned int octaves = 4;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "colora" ) {
							colora = pvalue;
						} else if( pname == "colorb" ) {
							colorb = pvalue;
						} else if( pname == "persistence" ) {
							persistence = pvalue.toDouble();
						} else if( pname == "scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &scale[0], &scale[1], &scale[2] );
						} else if( pname == "shift" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &shift[0], &shift[1], &shift[2] );
						} else if( pname == "octaves" ) {
							octaves = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPerlin3DPainter( name.c_str(), persistence, octaves, colora.c_str(), colorb.c_str(), scale, shift );
				}
			};

			struct Voronoi2DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String border = "none";
					std::vector<double> ptx;
					std::vector<double> pty;
					std::vector<String> painters;

					double bordersize=0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "border" ) {
							border = pvalue;
						} else if( pname == "bordersize" ) {
							bordersize = pvalue.toDouble();
						} else if( pname == "gen" ) {
							double x, y;
							char painter[256] = {0};
							sscanf( pvalue.c_str(), "%lf %lf %255s", &x, &y, painter );
							ptx.push_back( x );
							pty.push_back( y );
							painters.push_back( painter );
						} else if( pname == "file" ) {
							// Load generators from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							while( !feof( f ) ) {
								double x, y;
								char painter[256] = {0};
								fscanf( f, "%lf %lf %255s", &x, &y, painter );
								ptx.push_back( x );
								pty.push_back( y );
								painters.push_back( painter );
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					const unsigned int num = painters.size();
					char* pntrmem = new char[num*256];
					memset( pntrmem, 0, num*256 );
					char** pntrs = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						pntrs[i] = &pntrmem[i*256];
						strncpy( pntrs[i], painters[i].c_str(), 255 );
					}

					bool bRet = pJob.AddVoronoi2DPainter( name.c_str(), &ptx[0], &pty[0], (const char**)pntrs, num, border=="none"?0:border.c_str(), bordersize );

					delete [] pntrs;
					delete [] pntrmem;

					return bRet;
				}
			};

			struct Voronoi3DPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String border = "none";
					std::vector<double> ptx;
					std::vector<double> pty;
					std::vector<double> ptz;
					std::vector<String> painters;

					double bordersize=0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "border" ) {
							border = pvalue;
						} else if( pname == "bordersize" ) {
							bordersize = pvalue.toDouble();
						} else if( pname == "gen" ) {
							double x, y, z;
							char painter[256] = {0};
							sscanf( pvalue.c_str(), "%lf %lf %lf %255s", &x, &y, &z, painter );
							ptx.push_back( x );
							pty.push_back( y );
							ptz.push_back( z );
							painters.push_back( painter );
						} else if( pname == "file" ) {
							// Load generators from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							// Read how many nm values and the range
							int num=0;
							fscanf( f, "%d", &num );

							for( int i=0; i<num; i++ ) {
								double x, y, z;
								char painter[256] = {0};
								fscanf( f, "%lf %lf %lf %255s", &x, &y, &z, painter );
								ptx.push_back( x );
								pty.push_back( y );
								ptz.push_back( z );
								painters.push_back( String(painter) );
							}
							fclose( f );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					const unsigned int num = painters.size();
					char* pntrmem = new char[num*256];
					memset( pntrmem, 0, num*256 );
					char** pntrs = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						pntrs[i] = &pntrmem[i*256];
						strncpy( pntrs[i], painters[i].c_str(), 255 );
					}

					bool bRet = pJob.AddVoronoi3DPainter( name.c_str(), &ptx[0], &pty[0], &ptz[0], (const char**)pntrs, num, border=="none"?0:border.c_str(), bordersize );

					delete [] pntrs;
					delete [] pntrmem;

					return bRet;
				}
			};

			struct IridescentPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String colora = "none";
					String colorb = "none";
					double bias = 0.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "colora" ) {
							colora = pvalue;
						} else if( pname == "colorb" ) {
							colorb = pvalue;
						} else if( pname == "bias" ) {
							bias = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddIridescentPainter( name.c_str(), colora.c_str(), colorb.c_str(), bias );
				}
			};

			struct BlackBodyPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";

					double temperature = 5600.0;
					double lambda_begin = 400.0;
					double lambda_end = 700.0;
					unsigned int num_freq = 30;
					double scale = 1.0;
					bool normalize = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "temperature" ) {
							temperature = pvalue.toDouble();
						} else if( pname == "nmbegin" ) {
							lambda_begin = pvalue.toDouble();
						} else if( pname == "nmend" ) {
							lambda_end = pvalue.toDouble();
						} else if( pname == "numfreq" ) {
							num_freq = pvalue.toUInt();
						} else if( pname == "scale" ) {
							scale = pvalue.toDouble();
						} else if( pname == "normalize" ) {
							normalize = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBlackBodyPainter( name.c_str(), temperature, lambda_begin, lambda_end, num_freq, normalize, scale );
				}
			};

			struct BlendPainterAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String colora = "none";
					String colorb = "none";
					String mask = "none";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "colora" ) {
							colora = pvalue;
						} else if( pname == "colorb" ) {
							colorb = pvalue;
						} else if( pname == "mask" ) {
							mask = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBlendPainter( name.c_str(), colora.c_str(), colorb.c_str(), mask.c_str() );
				}
			};

			//////////////////////////////////////////
			// Functions
			//////////////////////////////////////////

			struct PiecewiseLinearFunctionChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					std::vector<double> cp_x;
					std::vector<double> cp_y;
					bool bUseLUTs=false;
					unsigned int lutsize=1024;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "use_lut" ) {
							bUseLUTs = pvalue.toBoolean();
						} else if( pname == "lutsize" ) {
							lutsize = pvalue.toUInt();
						} else if( pname == "cp" ) {
							double x, y;
							sscanf( pvalue.c_str(), "%lf %lf", &x, &y );
							cp_x.push_back( x );
							cp_y.push_back( y );
						} else if( pname == "file" ) {
							// Load the spectral values from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double x, y;
									fscanf( f, "%lf %lf", &x, &y );
									cp_x.push_back( x );
									cp_y.push_back( y );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPiecewiseLinearFunction( name.c_str(), &cp_x[0], &cp_y[0], cp_x.size(), bUseLUTs, lutsize );
				}
			};

			struct PiecewiseLinearFunction2DChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					std::vector<double> cp_x;
					std::vector<String> cp_y;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "cp" ) {
							double x;
							char y[1024] = {0};
							sscanf( pvalue.c_str(), "%lf %s", &x, y );
							cp_x.push_back( x );
							cp_y.push_back( String(y) );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					// Setup the array of strings
					char** func = new char*[cp_x.size()];
					for( unsigned int i=0; i<cp_x.size(); i++ ) {
						func[i] = (char*)(&(*(cp_y[i].begin())));
					}

					bool bRet = pJob.AddPiecewiseLinearFunction2D( name.c_str(), &cp_x[0], func, cp_x.size() );

					safe_delete( func );

					return bRet;
				}
			};

			//////////////////////////////////////////
			// Materials
			//////////////////////////////////////////

			struct LambertianMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String reflectance = "none";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "reflectance" ) {
							reflectance = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddLambertianMaterial( name.c_str(), reflectance.c_str() );
				}
			};

			struct PerfectReflectorMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String reflectance = "none";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "reflectance" ) {
							reflectance = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPerfectReflectorMaterial( name.c_str(), reflectance.c_str() );
				}
			};

			struct PerfectRefractorMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String refractance = "none";
					String ior = "1.33";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "refractance" ) {
							refractance = pvalue;
						} else if( pname == "ior" ) {
							ior = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPerfectRefractorMaterial( name.c_str(), refractance.c_str(), ior.c_str() );
				}
			};

			struct PolishedMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String reflectance = "none";
					String tau = "none";
					String ior = "1.0";
					String scat = "64";
					bool hg = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "reflectance" ) {
							reflectance = pvalue;
						} else if( pname == "tau" ) {
							tau = pvalue;
						} else if( pname == "ior" ) {
							ior = pvalue;
						} else if( pname == "scattering" ) {
							scat = pvalue;
						} else if( pname == "henyey-greenstein" ) {
							hg = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPolishedMaterial( name.c_str(), reflectance.c_str(), tau.c_str(), ior.c_str(), scat.c_str(), hg );
				}
			};

			struct DielectricMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String tau = "none";
					String ior = "1.33";
					String scat = "10000";
					bool hg = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "tau" ) {
							tau = pvalue;
						} else if( pname == "ior" ) {
							ior = pvalue;
						} else if( pname == "scattering" ) {
							scat = pvalue;
						} else if( pname == "henyey-greenstein" ) {
							hg = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddDielectricMaterial( name.c_str(), tau.c_str(), ior.c_str(), scat.c_str(), hg );
				}
			};

			struct LambertianLuminaireMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String material = "none";
					String painter = "none";
					double scale = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "material" ) {
							material = pvalue;
						} else if( pname == "exitance" ) {
							painter = pvalue;
						} else if( pname == "scale" ) {
							scale = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddLambertianLuminaireMaterial( name.c_str(), painter.c_str(), material.c_str(), scale );
				}
			};

			struct PhongLuminaireMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String material = "none";
					String painter = "none";
					double scale = 1.0;
					String N = "16.0";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "material" ) {
							material = pvalue;
						} else if( pname == "exitance" ) {
							painter = pvalue;
						} else if( pname == "N" ) {
							N = pvalue;
						} else if( pname == "scale" ) {
							scale = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPhongLuminaireMaterial( name.c_str(), painter.c_str(), material.c_str(), N.c_str(), scale );
				}
			};

			struct AshikminShirleyAnisotropicPhongMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String rd = "none";
					String rs = "none";
					String Nu = "10.0";
					String Nv = "100.0";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "rd" ) {
							rd = pvalue;
						} else if( pname == "rs" ) {
							rs = pvalue;
						} else if( pname == "nu" ) {
							Nu = pvalue;
						} else if( pname == "nv" ) {
							Nv = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddAshikminShirleyAnisotropicPhongMaterial( name.c_str(), rd.c_str(), rs.c_str(), Nu.c_str(), Nv.c_str() );
				}
			};

			struct IsotropicPhongMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String rd = "none";
					String rs = "none";
					String N = "16.0";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "rd" ) {
							rd = pvalue;
						} else if( pname == "rs" ) {
							rs = pvalue;
						} else if( pname == "N" ) {
							N = pvalue;;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddIsotropicPhongMaterial( name.c_str(), rd.c_str(), rs.c_str(), N.c_str() );
				}
			};

			struct TranslucentMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String ref = "none";
					String tau = "none";
					String N = "1.0";
					String ext = "none";
					String scat = "0.0";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "ref" ) {
							ref = pvalue;
						} else if( pname == "tau" ) {
							tau = pvalue;
						} else if( pname == "ext" ) {
							ext = pvalue;
						} else if( pname == "N" ) {
							N = pvalue;
						} else if( pname == "scattering" ) {
							scat = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddTranslucentMaterial( name.c_str(), ref.c_str(), tau.c_str(), ext.c_str(), N.c_str(), scat.c_str() );
				}
			};

			struct BioSpecSkinMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String thickness_SC = "0.001";				// 10 - 40 um
					String thickness_epidermis = "0.01";			// 80 - 200 um
					String thickness_papillary_dermis = "0.02";
					String thickness_reticular_dermis = "0.18";
					String ior_SC = "1.55";
					String ior_epidermis = "1.4";
					String ior_papillary_dermis = "1.36";
					String ior_reticular_dermis = "1.38";
					String concentration_eumelanin = "80.0";
					String concentration_pheomelanin = "12.0";
					String melanosomes_in_epidermis = "0.10";		// dark east indian
					String hb_ratio = "0.75";
					String whole_blood_in_papillary_dermis = "0.012";			// 0.2 - 5%
					String whole_blood_in_reticular_dermis = "0.0091";		// 0.2 - 5%
					String bilirubin_concentration = "0.005";					// from 0.005 to 0.5 even 5.0 might be ok g/L
					String betacarotene_concentration_SC = "2.1e-4";
					String betacarotene_concentration_epidermis = "2.1e-4";
					String betacarotene_concentration_dermis = "7.0e-5";
					String folds_aspect_ratio = "0.75";
					bool bSubdermalLayer = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "thickness_SC" ) {
							thickness_SC = pvalue;
						} else if( pname == "thickness_epidermis" ) {
							thickness_epidermis = pvalue;
						} else if( pname == "thickness_papillary_dermis" ) {
							thickness_papillary_dermis = pvalue;
						} else if( pname == "thickness_reticular_dermis" ) {
							thickness_reticular_dermis = pvalue;
						} else if( pname == "ior_SC" ) {
							ior_SC = pvalue;
						} else if( pname == "ior_epidermis" ) {
							ior_epidermis = pvalue;
						} else if( pname == "ior_papillary_dermis" ) {
							ior_papillary_dermis = pvalue;
						} else if( pname == "ior_reticular_dermis" ) {
							ior_reticular_dermis = pvalue;
						} else if( pname == "concentration_eumelanin" ) {
							concentration_eumelanin = pvalue;
						} else if( pname == "concentration_pheomelanin" ) {
							concentration_pheomelanin = pvalue;
						} else if( pname == "melanosomes_in_epidermis" ) {
							melanosomes_in_epidermis = pvalue;
						} else if( pname == "hb_ratio" ) {
							hb_ratio = pvalue;
						} else if( pname == "whole_blood_in_papillary_dermis" ) {
							whole_blood_in_papillary_dermis = pvalue;
						} else if( pname == "whole_blood_in_reticular_dermis" ) {
							whole_blood_in_reticular_dermis = pvalue;
						} else if( pname == "bilirubin_concentration" ) {
							bilirubin_concentration = pvalue;
						} else if( pname == "betacarotene_concentration_SC" ) {
							betacarotene_concentration_SC = pvalue;
						} else if( pname == "betacarotene_concentration_epidermis" ) {
							betacarotene_concentration_epidermis = pvalue;
						} else if( pname == "betacarotene_concentration_dermis" ) {
							betacarotene_concentration_dermis = pvalue;
						} else if( pname == "folds_aspect_ratio" ) {
							folds_aspect_ratio = pvalue;
						} else if( pname == "subdermal_layer" ) {
							bSubdermalLayer = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBioSpecSkinMaterial( name.c_str(), thickness_SC.c_str(), thickness_epidermis.c_str(), thickness_papillary_dermis.c_str(), thickness_reticular_dermis.c_str(),
						ior_SC.c_str(), ior_epidermis.c_str(), ior_papillary_dermis.c_str(), ior_reticular_dermis.c_str(), concentration_eumelanin.c_str(), concentration_pheomelanin.c_str(),
						melanosomes_in_epidermis.c_str(), hb_ratio.c_str(), whole_blood_in_papillary_dermis.c_str(), whole_blood_in_reticular_dermis.c_str(),
						bilirubin_concentration.c_str(), betacarotene_concentration_SC.c_str(), betacarotene_concentration_epidermis.c_str(), betacarotene_concentration_dermis.c_str(),
						folds_aspect_ratio.c_str(), bSubdermalLayer );
				}
			};

			struct GenericHumanTissueMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String g = "none";
					String sca = "0.85";
					double hb_ratio = 0.75;
					double whole_blood = 0.012;							// 0.2 - 7%
					double bilirubin_concentration = 0.005;					// from 0.005 to 0.5 even 5.0 might be ok g/L
					double betacarotene_concentration = 7.0e-5;
					bool diffuse = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "g" ) {
							g = pvalue;
						} else if( pname == "sca" ) {
							sca = pvalue;
						} else if( pname == "hb_ratio" ) {
							hb_ratio = pvalue.toDouble();
						} else if( pname == "whole_blood" ) {
							whole_blood = pvalue.toDouble();
						} else if( pname == "bilirubin_concentration" ) {
							bilirubin_concentration = pvalue.toDouble();
						} else if( pname == "betacarotene_concentration" ) {
							betacarotene_concentration = pvalue.toDouble();
						} else if( pname == "diffuse" ) {
							diffuse = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddGenericHumanTissueMaterial( name.c_str(), sca.c_str(), g.c_str(), whole_blood, hb_ratio, bilirubin_concentration, betacarotene_concentration, diffuse );
				}
			};

			struct CompositeMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String top = "none";
					String bottom = "none";
					unsigned int max_recur = 3;
					unsigned int max_reflection_recur = 3;
					unsigned int max_refraction_recur = 3;
					unsigned int max_diffuse_recur = 3;
					unsigned int max_translucent_recur = 3;
					double thickness = 0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "top" ) {
							top = pvalue;
						} else if( pname == "bottom" ) {
							bottom = pvalue;
						} else if( pname == "max_recursion" ) {
							max_recur = pvalue.toUInt();
						} else if( pname == "max_reflection_recursion" ) {
							max_reflection_recur = pvalue.toUInt();
						} else if( pname == "max_refraction_recursion" ) {
							max_refraction_recur = pvalue.toUInt();
						} else if( pname == "max_diffuse_recursion" ) {
							max_diffuse_recur = pvalue.toUInt();
						} else if( pname == "max_translucent_recursion" ) {
							max_translucent_recur = pvalue.toUInt();
						} else if( pname == "thickness" ) {
							thickness = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddCompositeMaterial( name.c_str(), top.c_str(), bottom.c_str(), max_recur, max_reflection_recur, max_refraction_recur, max_diffuse_recur, max_translucent_recur, thickness );
				}
			};

			struct WardIsotropicGaussianMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String rd = "none";
					String rs = "none";
					String alpha = "0.1";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "rd" ) {
							rd = pvalue;
						} else if( pname == "rs" ) {
							rs = pvalue;
						} else if( pname == "alpha" ) {
							alpha = pvalue;;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddWardIsotropicGaussianMaterial( name.c_str(), rd.c_str(), rs.c_str(), alpha.c_str() );
				}
			};

			struct WardAnisotropicEllipticalGaussianMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String rd = "none";
					String rs = "none";
					String alphax = "0.1";
					String alphay = "0.2";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "rd" ) {
							rd = pvalue;
						} else if( pname == "rs" ) {
							rs = pvalue;
						} else if( pname == "alphax" ) {
							alphax = pvalue;;
						} else if( pname == "alphay" ) {
							alphay = pvalue;;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddWardAnisotropicEllipticalGaussianMaterial( name.c_str(), rd.c_str(), rs.c_str(), alphax.c_str(), alphay.c_str() );
				}
			};

			struct CookTorranceMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String rd = "none";
					String rs = "none";
					String facets = "0.15";
					String ior = "2.45";
					String extinction = "1";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "rd" ) {
							rd = pvalue;
						} else if( pname == "rs" ) {
							rs = pvalue;
						} else if( pname == "facets" ) {
							facets = pvalue;
						} else if( pname == "ior" ) {
							ior = pvalue;
						} else if( pname == "extinction" ) {
							extinction = pvalue;;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddCookTorranceMaterial( name.c_str(), rd.c_str(), rs.c_str(), facets.c_str(), ior.c_str(), extinction.c_str() );
				}
			};

			struct OrenNayarMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String reflectance = "none";
					String roughness = "0.5";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "reflectance" ) {
							reflectance = pvalue;
						} else if( pname == "roughness" ) {
							roughness = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddOrenNayarMaterial( name.c_str(), reflectance.c_str(), roughness.c_str() );
				}
			};

			struct SchlickMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String rd = "none";
					String rs = "none";
					String roughness = "0.05";
					String isotropy = "1";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "rd" ) {
							rd = pvalue;
						} else if( pname == "rs" ) {
							rs = pvalue;
						} else if( pname == "roughness" ) {
							roughness = pvalue;
						} else if( pname == "isotropy" ) {
							isotropy = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddSchlickMaterial( name.c_str(), rd.c_str(), rs.c_str(), roughness.c_str(), isotropy.c_str() );
				}
			};

			struct DataDrivenMaterialAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String filename = "";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "filename" ) {
							filename = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddDataDrivenMaterial( name.c_str(), filename.c_str() );
				}
			};


			//////////////////////////////////////////
			// Cameras
			//////////////////////////////////////////

			struct PinholeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double fov = 30.0;
					unsigned int xres = 256;
					unsigned int yres = 256;
					double loc[3] = {0};
					double lookat[3] = {0,0,-1};
					double up[3] = {0,1,0};
					double pixelAR = 1.0;
					double exposure = 0;
					double scanningRate = 0;
					double pixelRate = 0;
					double orientation[3] = {0};
					double target_orientation[2] = {0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "fov" ) {
							fov = pvalue.toDouble() * DEG_TO_RAD;
						} else if( pname == "width" ) {
							xres = pvalue.toUInt();
						} else if( pname == "height" ) {
							yres = pvalue.toUInt();
						} else if( pname == "location" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &loc[0], &loc[1], &loc[2] );
						} else if( pname == "lookat" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &lookat[0], &lookat[1], &lookat[2] );
						} else if( pname == "up" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &up[0], &up[1], &up[2] );
						} else if( pname == "pixelAR" ) {
							pixelAR = pvalue.toDouble();
						} else if( pname == "exposure" ) {
							exposure = pvalue.toDouble();
						} else if( pname == "scanning_rate" ) {
							scanningRate = pvalue.toDouble();
						} else if( pname == "pixel_rate" ) {
							pixelRate = pvalue.toDouble();
						} else if( pname == "pitch" ) {
							orientation[0] = pvalue.toDouble();
						} else if( pname == "roll" ) {
							orientation[1] = pvalue.toDouble();
						} else if( pname == "yaw" ) {
							orientation[2] = pvalue.toDouble();
						} else if( pname == "orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &orientation[0], &orientation[1], &orientation[2] );
						} else if( pname == "theta" ) {
							target_orientation[0] = pvalue.toDouble();
						} else if( pname == "phi" ) {
							target_orientation[1] = pvalue.toDouble();
						} else if( pname == "target_orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.SetPinholeCamera( loc, lookat, up, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation );
				}
			};

			struct ONBPinholeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double fov = 30.0;
					unsigned int xres = 256;
					unsigned int yres = 256;
					double loc[3] = {0};
					double vA[3] = {0};
					double vB[3] = {0};
					double pixelAR = 1.0;
					double exposure = 0;
					double scanningRate = 0;
					double pixelRate = 0;
					String components;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "fov" ) {
							fov = pvalue.toDouble() * DEG_TO_RAD;
						} else if( pname == "width" ) {
							xres = pvalue.toUInt();
						} else if( pname == "height" ) {
							yres = pvalue.toUInt();
						} else if( pname == "location" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &loc[0], &loc[1], &loc[2] );
						} else if( pname == "va" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &vA[0], &vA[1], &vA[2] );
						} else if( pname == "vb" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &vB[0], &vB[1], &vB[2] );
						} else if( pname == "components" ) {
							components = pvalue;
						} else if( pname == "pixelAR" ) {
							pixelAR = pvalue.toDouble();
						} else if( pname == "exposure" ) {
							exposure = pvalue.toDouble();
						} else if( pname == "scanning_rate" ) {
							scanningRate = pvalue.toDouble();
						} else if( pname == "pixel_rate" ) {
							pixelRate = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					OrthonormalBasis3D	onb;

					if( components == "UV" ) {
						onb.CreateFromUV( vA, vB );
					} else if( components == "VU" ) {
						onb.CreateFromVU( vA, vB );
					} else if( components == "UW" ) {
						onb.CreateFromUW( vA, vB );
					} else if( components == "WU" ) {
						onb.CreateFromWU( vA, vB );
					} else if( components == "VW" ) {
						onb.CreateFromVW( vA, vB );
					} else if( components == "WV" ) {
						onb.CreateFromWV( vA, vB );
					} else {
						GlobalLog()->PrintEx( eLog_Error, "ONBPinholeCameraAsciiChunkParser:: Unknown component type `%s`", components.c_str() );
							return false;
					}

					double ONB_U[3] = {onb.u().x, onb.u().y, onb.u().z};
					double ONB_V[3] = {onb.v().x, onb.v().y, onb.v().z};
					double ONB_W[3] = {onb.w().x, onb.w().y, onb.w().z};
					return pJob.SetPinholeCameraONB( ONB_U, ONB_V, ONB_W, loc, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate );
				}
			};

			struct ThinlensCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double fov = 30.0;
					unsigned int xres = 256;
					unsigned int yres = 256;
					double aperture = 1.0;
					double focal = 0.1;
					double focus = 1.0;
					double loc[3] = {0};
					double lookat[3] = {0,0,-1};
					double up[3] = {0,1,0};
					double pixelAR = 1.0;
					double exposure = 0;
					double scanningRate = 0;
					double pixelRate = 0;
					double orientation[3] = {0};
					double target_orientation[2] = {0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "fov" ) {
							fov = pvalue.toDouble() * DEG_TO_RAD;
						} else if( pname == "width" ) {
							xres = pvalue.toUInt();
						} else if( pname == "height" ) {
							yres = pvalue.toUInt();
						} else if( pname == "aperture_size" ) {
							aperture = pvalue.toDouble();
						} else if( pname == "focal_length" ) {
							focal = pvalue.toDouble();
						} else if( pname == "focus_distance" ) {
							focus = pvalue.toDouble();
						} else if( pname == "location" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &loc[0], &loc[1], &loc[2] );
						} else if( pname == "lookat" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &lookat[0], &lookat[1], &lookat[2] );
						} else if( pname == "up" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &up[0], &up[1], &up[2] );
						} else if( pname == "pixelAR" ) {
							pixelAR = pvalue.toDouble();
						} else if( pname == "exposure" ) {
							exposure = pvalue.toDouble();
						} else if( pname == "scanning_rate" ) {
							scanningRate = pvalue.toDouble();
						} else if( pname == "pixel_rate" ) {
							pixelRate = pvalue.toDouble();
						} else if( pname == "pitch" ) {
							orientation[0] = pvalue.toDouble();
						} else if( pname == "roll" ) {
							orientation[1] = pvalue.toDouble();
						} else if( pname == "yaw" ) {
							orientation[2] = pvalue.toDouble();
						} else if( pname == "orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &orientation[0], &orientation[1], &orientation[2] );
						} else if( pname == "theta" ) {
							target_orientation[0] = pvalue.toDouble();
						} else if( pname == "phi" ) {
							target_orientation[1] = pvalue.toDouble();
						} else if( pname == "target_orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					if( focal >= focus ) {
						GlobalLog()->PrintEx( eLog_Error, "Focal length is >= focus distance, that makes no sense!" );
						return false;
					}

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.SetThinlensCamera( loc, lookat, up, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, aperture, focal, focus );
				}
			};

			struct RealisticCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each

					unsigned int xres = 256;
					unsigned int yres = 256;

					// Default film sizes are 35mm
					double film_size = 35;
					double fstop = 2.8;
					double focal = 0.1;
					double focus = 1.0;
					double loc[3] = {0};
					double lookat[3] = {0,0,-1};
					double up[3] = {0,1,0};
					double pixelAR = 1.0;
					double exposure = 0;
					double scanningRate = 0;
					double pixelRate = 0;
					double orientation[3] = {0};
					double target_orientation[2] = {0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "width" ) {
							xres = pvalue.toUInt();
						} else if( pname == "height" ) {
							yres = pvalue.toUInt();
						} else if( pname == "film_size" ) {
							film_size = pvalue.toDouble();
						} else if( pname == "fstop" ) {
							fstop = pvalue.toDouble();
						} else if( pname == "focal_length" ) {
							focal = pvalue.toDouble();
						} else if( pname == "focus_distance" ) {
							focus = pvalue.toDouble();
						} else if( pname == "location" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &loc[0], &loc[1], &loc[2] );
						} else if( pname == "lookat" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &lookat[0], &lookat[1], &lookat[2] );
						} else if( pname == "up" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &up[0], &up[1], &up[2] );
						} else if( pname == "pixelAR" ) {
							pixelAR = pvalue.toDouble();
						} else if( pname == "exposure" ) {
							exposure = pvalue.toDouble();
						} else if( pname == "scanning_rate" ) {
							scanningRate = pvalue.toDouble();
						} else if( pname == "pixel_rate" ) {
							pixelRate = pvalue.toDouble();
						} else if( pname == "pitch" ) {
							orientation[0] = pvalue.toDouble();
						} else if( pname == "roll" ) {
							orientation[1] = pvalue.toDouble();
						} else if( pname == "yaw" ) {
							orientation[2] = pvalue.toDouble();
						} else if( pname == "orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &orientation[0], &orientation[1], &orientation[2] );
						} else if( pname == "theta" ) {
							target_orientation[0] = pvalue.toDouble();
						} else if( pname == "phi" ) {
							target_orientation[1] = pvalue.toDouble();
						} else if( pname == "target_orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					if( focal >= focus ) {
						GlobalLog()->PrintEx( eLog_Error, "Focal length is >= focus distance, that makes no sense!" );
						return false;
					}

					// From ThinLensCamera.cpp
					// Angle of View = 2 * ArcTan(Film Dimension / (2 * Focal Length))
					const double fov = 2.0 * atan(film_size / (2.0 * focal));
					const double aperture = focal / fstop;

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.SetThinlensCamera( loc, lookat, up, fov, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, aperture, focal, focus );
				}
			};

			struct FisheyeCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int xres = 256;
					unsigned int yres = 256;
					double loc[3] = {0};
					double lookat[3] = {0,0,-1};
					double up[3] = {0,1,0};
					double pixelAR = 1.0;
					double exposure = 0;
					double scanningRate = 0;
					double pixelRate = 0;
					double orientation[3] = {0};
					double target_orientation[2] = {0};
					double scale = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "width" ) {
							xres = pvalue.toUInt();
						} else if( pname == "height" ) {
							yres = pvalue.toUInt();
						} else if( pname == "location" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &loc[0], &loc[1], &loc[2] );
						} else if( pname == "lookat" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &lookat[0], &lookat[1], &lookat[2] );
						} else if( pname == "up" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &up[0], &up[1], &up[2] );
						} else if( pname == "pixelAR" ) {
							pixelAR = pvalue.toDouble();
						} else if( pname == "exposure" ) {
							exposure = pvalue.toDouble();
						} else if( pname == "scanning_rate" ) {
							scanningRate = pvalue.toDouble();
						} else if( pname == "pixel_rate" ) {
							pixelRate = pvalue.toDouble();
						} else if( pname == "pitch" ) {
							orientation[0] = pvalue.toDouble();
						} else if( pname == "roll" ) {
							orientation[1] = pvalue.toDouble();
						} else if( pname == "yaw" ) {
							orientation[2] = pvalue.toDouble();
						} else if( pname == "orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &orientation[0], &orientation[1], &orientation[2] );
						} else if( pname == "theta" ) {
							target_orientation[0] = pvalue.toDouble();
						} else if( pname == "phi" ) {
							target_orientation[1] = pvalue.toDouble();
						} else if( pname == "target_orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
						} else if( pname == "scale" ) {
							scale = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.SetFisheyeCamera( loc, lookat, up, xres, yres, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation, scale );
				}
			};

			struct OrthographicCameraAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int xres = 256;
					unsigned int yres = 256;
					double loc[3] = {0};
					double lookat[3] = {0,0,-1};
					double up[3] = {0,1,0};
					double pixelAR = 1.0;
					double vpscale[2] = {1.0,1.0};
					double exposure = 0;
					double scanningRate = 0;
					double pixelRate = 0;
					double orientation[3] = {0};
					double target_orientation[2] = {0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "width" ) {
							xres = pvalue.toUInt();
						} else if( pname == "height" ) {
							yres = pvalue.toUInt();
						} else if( pname == "location" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &loc[0], &loc[1], &loc[2] );
						} else if( pname == "lookat" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &lookat[0], &lookat[1], &lookat[2] );
						} else if( pname == "up" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &up[0], &up[1], &up[2] );
						} else if( pname == "viewport_scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &vpscale[0], &vpscale[1] );
						} else if( pname == "pixelAR" ) {
							pixelAR = pvalue.toDouble();
						} else if( pname == "exposure" ) {
							exposure = pvalue.toDouble();
						} else if( pname == "scanning_rate" ) {
							scanningRate = pvalue.toDouble();
						} else if( pname == "pixel_rate" ) {
							pixelRate = pvalue.toDouble();
						} else if( pname == "pitch" ) {
							orientation[0] = pvalue.toDouble();
						} else if( pname == "roll" ) {
							orientation[1] = pvalue.toDouble();
						} else if( pname == "yaw" ) {
							orientation[2] = pvalue.toDouble();
						} else if( pname == "orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &orientation[0], &orientation[1], &orientation[2] );
						} else if( pname == "theta" ) {
							target_orientation[0] = pvalue.toDouble();
						} else if( pname == "phi" ) {
							target_orientation[1] = pvalue.toDouble();
						} else if( pname == "target_orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf", &target_orientation[0], &target_orientation[1] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					orientation[0] *= DEG_TO_RAD;
					orientation[1] *= DEG_TO_RAD;
					orientation[2] *= DEG_TO_RAD;

					target_orientation[0] *= DEG_TO_RAD;
					target_orientation[1] *= DEG_TO_RAD;

					return pJob.SetOrthographicCamera( loc, lookat, up, xres, yres, vpscale, pixelAR, exposure, scanningRate, pixelRate, orientation, target_orientation );
				}
			};

			//////////////////////////////////////////
			// Geometries
			//////////////////////////////////////////

			struct SphereGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double radius = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddSphereGeometry( name.c_str(), radius );
				}
			};

			struct EllipsoidGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double radii[3] = {1.0,1.0,1.0};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "radii" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &radii[0], &radii[1], &radii[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddEllipsoidGeometry( name.c_str(), radii );
				}
			};

			struct CylinderGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double radius = 1.0;
					double height = 1.0;
					char axis = 'x';

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "height" ) {
							height = pvalue.toDouble();
						} else if( pname == "axis" ) {
							axis = pvalue[0];
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddCylinderGeometry( name.c_str(), axis, radius, height );
				}
			};

			struct TorusGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double majorradius = 1.0;
					double minorratio = 0.3;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "majorradius" ) {
							majorradius = pvalue.toDouble();
						} else if( pname == "minorratio" ) {
							minorratio = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddTorusGeometry( name.c_str(), majorradius, minorratio*majorradius );
				}
			};

			struct InfinitePlaneGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double xtile = 1.0;
					double ytile = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "xtile" ) {
							xtile = pvalue.toDouble();
						} else if( pname == "ytile" ) {
							ytile = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddInfinitePlaneGeometry( name.c_str(), xtile, ytile );
				}
			};

			struct BoxGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double width = 1.0;
					double height = 1.0;
					double depth = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "width" ) {
							width = pvalue.toDouble();
						} else if( pname == "height" ) {
							height = pvalue.toDouble();
						} else if( pname == "depth" ) {
							depth = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBoxGeometry( name.c_str(), width, height, depth );
				}
			};

			struct ClippedPlaneGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double pta[3] = {0};
					double ptb[3] = {0};
					double ptc[3] = {0};
					double ptd[3] = {0};
					bool doublesided = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "pta" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &pta[0], &pta[1], &pta[2] );
						} else if( pname == "ptb" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &ptb[0], &ptb[1], &ptb[2] );
						} else if( pname == "ptc" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &ptc[0], &ptc[1], &ptc[2] );
						} else if( pname == "ptd" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &ptd[0], &ptd[1], &ptd[2] );
						} else if( pname == "doublesided" ) {
							doublesided = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddClippedPlaneGeometry( name.c_str(), pta, ptb, ptc, ptd, doublesided );
				}
			};

			struct Mesh3DSGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String file = "none";
					unsigned int maxPoly = 10;
					unsigned int maxRecur = 8;
					unsigned int objid = 0;
					bool double_sided = false;
					bool bsp = false;
					bool face_normals = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							file = pvalue;
						} else if( pname == "maxpolygons" ) {
							maxPoly = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "objectid" ) {
							objid = pvalue.toUInt();
						} else if( pname == "double_sided" ) {
							double_sided = pvalue.toBoolean();
						} else if( pname == "bsp" ) {
							bsp = pvalue.toBoolean();
						} else if( pname == "face_normals" ) {
							face_normals = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.Add3DSTriangleMeshGeometry( name.c_str(), file.c_str(), maxPoly, maxRecur, double_sided, bsp, face_normals );
				}
			};

			struct RAWMeshGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String file = "none";
					unsigned int maxPoly = 10;
					unsigned int maxRecur = 8;
					bool double_sided = false;
					bool bsp = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							file = pvalue;
						} else if( pname == "maxpolygons" ) {
							maxPoly = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "double_sided" ) {
							double_sided = pvalue.toBoolean();
						} else if( pname == "bsp" ) {
							bsp = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddRAWTriangleMeshGeometry( name.c_str(), file.c_str(), maxPoly, maxRecur, double_sided, bsp );
				}
			};

			struct RAWMesh2GeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String file = "none";
					unsigned int maxPoly = 10;
					unsigned int maxRecur = 8;
					bool double_sided = false;
					bool bsp = false;
					bool face_normals = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							file = pvalue;
						} else if( pname == "maxpolygons" ) {
							maxPoly = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "double_sided" ) {
							double_sided = pvalue.toBoolean();
						} else if( pname == "bsp" ) {
							bsp = pvalue.toBoolean();
						} else if( pname == "face_normals" ) {
							face_normals = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddRAW2TriangleMeshGeometry( name.c_str(), file.c_str(), maxPoly, maxRecur, double_sided, bsp, face_normals );
				}
			};

			struct BezierMeshGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String file = "none";
					unsigned int maxPoly = 10;
					unsigned int maxRecur = 8;
					unsigned int detail = 6;
					bool combine_shared = false;
					bool center_object = false;
					bool double_sided = false;
					bool bsp = false;
					bool face_normals = false;

					String displacement = "none";
					double disp_scale = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							file = pvalue;
						} else if( pname == "maxpolygons" ) {
							maxPoly = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "detail" ) {
							detail = pvalue.toUInt();
						} else if( pname == "combine_shared" ) {
							combine_shared = pvalue.toBoolean();
						} else if( pname == "center_object" ) {
							center_object = pvalue.toBoolean();
						} else if( pname == "double_sided" ) {
							double_sided = pvalue.toBoolean();
						} else if( pname == "bsp" ) {
							bsp = pvalue.toBoolean();
						} else if( pname == "face_normals" ) {
							face_normals = pvalue.toBoolean();
						} else if( pname == "displacement" ) {
							displacement = pvalue;
						} else if( pname == "disp_scale" ) {
							disp_scale = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBezierTriangleMeshGeometry( name.c_str(), file.c_str(), detail, combine_shared, center_object, maxPoly, maxRecur, double_sided, bsp, face_normals, displacement=="none"?0:displacement.c_str(), disp_scale );
				}
			};

			struct RISEMeshGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String file = "none";
					bool loadintomem = true;
					bool face_normals = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							file = pvalue;
						} else if( pname == "loadintomemory" ) {
							loadintomem = pvalue.toBoolean();
						} else if( pname == "face_normals" ) {
							face_normals = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddRISEMeshTriangleMeshGeometry( name.c_str(), file.c_str(), loadintomem, face_normals );
				}
			};

			struct CircularDiskGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double radius = 1.0;
					char axis = 'x';

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "axis" ) {
							axis = pvalue[0];
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddCircularDiskGeometry( name.c_str(), radius, axis );
				}
			};

			struct BezierPatchGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String file = "none";
					unsigned int maxPatches = 2;
					unsigned int maxRecur = 8;
					bool bsp = false;
					bool analytic = false;
					unsigned int cache_size = 30;

					unsigned int maxPoly = 10;
					unsigned int maxPolyRecur = 8;
					bool double_sided = false;
					bool poly_bsp = false;
					bool face_normals = false;

					unsigned int detail = 6;

					String displacement = "none";
					double disp_scale = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							file = pvalue;
						} else if( pname == "maxpatches" ) {
							maxPatches = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "bsp" ) {
							bsp = pvalue.toBoolean();
						} else if( pname == "analytic" ) {
							analytic = pvalue.toBoolean();
						} else if( pname == "cache_size" ) {
							cache_size = pvalue.toUInt();
						} else if( pname == "maxpolygons" ) {
							maxPoly = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "double_sided" ) {
							double_sided = pvalue.toBoolean();
						} else if( pname == "poly_bsp" ) {
							poly_bsp = pvalue.toBoolean();
						} else if( pname == "face_normals" ) {
							face_normals = pvalue.toBoolean();
						} else if( pname == "detail" ) {
							detail = pvalue.toUInt();
						} else if( pname == "displacement" ) {
							displacement = pvalue;
						} else if( pname == "disp_scale" ) {
							disp_scale = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBezierPatchGeometry( name.c_str(), file.c_str(), maxPatches, maxRecur, bsp, analytic, cache_size, maxPoly, maxPolyRecur, double_sided, poly_bsp, face_normals, detail, displacement=="none"?0:displacement.c_str(), disp_scale );
				}
			};

			struct BilinearPatchGeometryAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String file = "none";
					unsigned int maxPoly = 10;
					unsigned int maxRecur = 8;
					bool bsp = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file" ) {
							file = pvalue;
						} else if( pname == "maxpolygons" ) {
							maxPoly = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "bsp" ) {
							bsp = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBilinearPatchGeometry( name.c_str(), file.c_str(), maxPoly, maxRecur, bsp );
				}
			};

			//////////////////////////////////////////
			// Modifiers
			//////////////////////////////////////////

			struct BumpmapModifierAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String function = "none";
					double scale = 1.0;
					double window = 0.01;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "function" ) {
							function = pvalue;
						} else if( pname == "scale" ) {
							scale = pvalue.toDouble();
						} else if( pname == "windowsize" ) {
							window = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddBumpMapModifier( name.c_str(), function.c_str(), scale, window );
				}
			};

			//////////////////////////////////////////
			// Objects
			//////////////////////////////////////////

			struct StandardObjectAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String geometry = "none";
					double pos[3] = {0};
					double orient[3] = {0};
					double scale[3] = {1.0,1.0,1.0};
					String material = "none";
					String modifier = "none";
					String shader = "none";
					String radiancemap = "none";
					double radianceScale = 1.0;
					double radorient[3] = {0};
					bool bCastsShadows = true;
					bool bReceivesShadows = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "geometry" ) {
							geometry = pvalue;
						} else if( pname == "material" ) {
							material = pvalue;
						} else if( pname == "modifier" ) {
							modifier = pvalue;
						} else if( pname == "shader" ) {
							shader = pvalue;
						} else if( pname == "radiance_map" ) {
							radiancemap = pvalue;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "radiance_orient" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &radorient[0], &radorient[1], &radorient[2] );
							radorient[0] *= DEG_TO_RAD;
							radorient[1] *= DEG_TO_RAD;
							radorient[2] *= DEG_TO_RAD;
						} else if( pname == "position" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &pos[0], &pos[1], &pos[2] );
						} else if( pname == "orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &orient[0], &orient[1], &orient[2] );
							orient[0] *=  DEG_TO_RAD;
							orient[1] *=  DEG_TO_RAD;
							orient[2] *=  DEG_TO_RAD;
						} else if( pname == "scale" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &scale[0], &scale[1], &scale[2] );
						} else if( pname == "casts_shadows" ) {
							bCastsShadows = pvalue.toBoolean();
						} else if( pname == "receives_shadows" ) {
							bReceivesShadows = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddObject( name.c_str(), geometry.c_str(), material=="none"?0:material.c_str(), modifier=="none"?0:modifier.c_str(), shader=="none"?0:shader.c_str(), radiancemap=="none"?0:radiancemap.c_str(), radianceScale, radorient, pos, orient, scale, bCastsShadows, bReceivesShadows );
				}
			};

			struct CSGObjectAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String obja = "none";
					String objb = "none";
					char op = 0;
					double pos[3] = {0};
					double orient[3] = {0};
					String material = "none";
					String modifier = "none";
					String shader = "none";
					String radiancemap = "none";
					double radianceScale = 1.0;
					double radorient[3] = {0};
					bool bCastsShadows = true;
					bool bReceivesShadows = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "obja" ) {
							obja = pvalue;
						} else if( pname == "objb" ) {
							objb = pvalue;
						} else if( pname == "material" ) {
							material = pvalue;
						} else if( pname == "modifier" ) {
							modifier = pvalue;
						} else if( pname == "shader" ) {
							shader = pvalue;
						} else if( pname == "radiance_map" ) {
							radiancemap = pvalue;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "radiance_orient" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &radorient[0], &radorient[1], &radorient[2] );
							radorient[0] *= DEG_TO_RAD;
							radorient[1] *= DEG_TO_RAD;
							radorient[2] *= DEG_TO_RAD;
						} else if( pname == "position" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &pos[0], &pos[1], &pos[2] );
						} else if( pname == "orientation" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &orient[0], &orient[1], &orient[2] );
							orient[0] *=  DEG_TO_RAD;
							orient[1] *=  DEG_TO_RAD;
							orient[2] *=  DEG_TO_RAD;
						} else if( pname == "operation" ) {
							if( pvalue=="union" ) {
								op = 0;
							} else if( pvalue == "intersection" ) {
								op = 1;
							} else if( pvalue == "subtraction" ) {
								op = 2;
							} else {
								return false;
							}
						} else if( pname == "casts_shadows" ) {
							bCastsShadows = pvalue.toBoolean();
						} else if( pname == "receives_shadows" ) {
							bReceivesShadows = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddCSGObject( name.c_str(), obja.c_str(), objb.c_str(), op, material=="none"?0:material.c_str(), modifier=="none"?0:modifier.c_str(), shader=="none"?0:shader.c_str(), radiancemap=="none"?0:radiancemap.c_str(), radianceScale, radorient, pos, orient, bCastsShadows, bReceivesShadows );
				}
			};

			//////////////////////////////////////////
			// Photon Mapping
			//////////////////////////////////////////


			//////////////////////////////////////////
			// Lights
			//////////////////////////////////////////

			struct AmbientLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double color[3] = {0};
					double power = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "power" ) {
							power = pvalue.toDouble();
						} else if( pname == "color" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &color[0], &color[1], &color[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddAmbientLight( name.c_str(), power, color );
				}
			};


			struct OmniLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double position[3] = {0};
					double color[3] = {0};
					double power = 1.0;
					double linearAttenuation = 0;
					double quadraticAttenuation = 0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "power" ) {
							power = pvalue.toDouble();
						} else if( pname == "position" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &position[0], &position[1], &position[2] );
						} else if( pname == "color" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &color[0], &color[1], &color[2] );
						} else if( pname == "linear_attenuation" ) {
							linearAttenuation = pvalue.toDouble();
						} else if( pname == "quadratic_attenuation" ) {
							quadraticAttenuation = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPointOmniLight( name.c_str(), power, color, position, linearAttenuation, quadraticAttenuation );
				}
			};


			struct SpotLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double position[3] = {0};
					double target[3] = {0};
					double color[3] = {0};
					double power = 1.0;
					double inner = PI_OV_FOUR;
					double outer = PI_OV_TWO;
					double linearAttenuation = 0;
					double quadraticAttenuation = 0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "power" ) {
							power = pvalue.toDouble();
						} else if( pname == "inner" ) {
							inner = pvalue.toDouble() * DEG_TO_RAD;
						} else if( pname == "outer" ) {
							outer = pvalue.toDouble() * DEG_TO_RAD;
						} else if( pname == "position" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &position[0], &position[1], &position[2] );
						} else if( pname == "target" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &target[0], &target[1], &target[2] );
						} else if( pname == "color" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &color[0], &color[1], &color[2] );
						} else if( pname == "linear_attenuation" ) {
							linearAttenuation = pvalue.toDouble();
						} else if( pname == "quadratic_attenuation" ) {
							quadraticAttenuation = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPointSpotLight( name.c_str(), power, color, target, inner, outer, position, linearAttenuation, quadraticAttenuation );
				}
			};

			struct DirectionalLightAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double dir[3] = {0};
					double color[3] = {0};
					double power = 1.0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "power" ) {
							power = pvalue.toDouble();
						} else if( pname == "direction" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &dir[0], &dir[1], &dir[2] );
						} else if( pname == "color" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &color[0], &color[1], &color[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddDirectionalLight( name.c_str(), power, color, dir );
				}
			};

			//////////////////////////////////////////
			// ShaderOps
			//////////////////////////////////////////

			struct PathTracingShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					bool branch = true;
					bool forcecheckemitters = false;
					bool finalgather = false;
					bool reflections = true;
					bool refractions = true;
					bool diffuse = true;
					bool translucents = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "branch" ) {
							branch = pvalue.toBoolean();
						} else if( pname == "force_check_emitters" ) {
							forcecheckemitters = pvalue.toBoolean();
						} else if( pname == "finalgather" ) {
							finalgather = pvalue.toBoolean();
						} else if( pname == "reflections" ) {
							reflections = pvalue.toBoolean();
						} else if( pname == "refractions" ) {
							refractions = pvalue.toBoolean();
						} else if( pname == "diffuse" ) {
							diffuse = pvalue.toBoolean();
						} else if( pname == "translucents" ) {
							translucents = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddPathTracingShaderOp( name.c_str(), branch, forcecheckemitters, finalgather, reflections, refractions, diffuse, translucents );
				}
			};

			struct DistributionTracingShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					unsigned int samples = 16;
					bool irradiancecaching = false;
					bool forcecheckemitters = false;
					bool branch = true;
					bool reflections = true;
					bool refractions = true;
					bool diffuse = true;
					bool translucents = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "samples" ) {
							samples = pvalue.toUInt();
						} else if( pname == "irradiance_caching" ) {
							irradiancecaching = pvalue.toBoolean();
						} else if( pname == "force_check_emitters" ) {
							forcecheckemitters = pvalue.toBoolean();
						} else if( pname == "branch" ) {
							branch = pvalue.toBoolean();
						} else if( pname == "reflections" ) {
							reflections = pvalue.toBoolean();
						} else if( pname == "refractions" ) {
							refractions = pvalue.toBoolean();
						} else if( pname == "diffuse" ) {
							diffuse = pvalue.toBoolean();
						} else if( pname == "translucents" ) {
							translucents = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddDistributionTracingShaderOp( name.c_str(), samples, irradiancecaching, forcecheckemitters, branch, reflections, refractions, diffuse, translucents );
				}
			};

			struct FinalGatherShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					unsigned int thetasamples = 15;
					unsigned int phisamples = (unsigned int)(Scalar(thetasamples)*PI);
					bool cachegradients = true;
					bool cache = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "phi_samples" ) {
							phisamples = pvalue.toUInt();
						} else if( pname == "theta_samples" ) {
							thetasamples = pvalue.toUInt();
						} else if( pname == "samples" ) {
							const unsigned int samples = pvalue.toUInt();
							const Scalar base = sqrt(Scalar(samples)/PI);
							thetasamples = static_cast<unsigned int>( base );
							phisamples = static_cast<unsigned int>( PI*base );
						} else if( pname == "cachegradients" ) {
							cachegradients = pvalue.toBoolean();
						} else if( pname == "cache" ) {
							cache = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddFinalGatherShaderOp( name.c_str(), thetasamples, phisamples, cachegradients, cache );
				}
			};


			struct AmbientOcclusionShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					unsigned int numtheta = 5;
					unsigned int numphi = 15;
					bool multiplybrdf = true;
					bool irradiance_cache = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "numtheta" ) {
							numtheta = pvalue.toUInt();
						} else if( pname == "numphi" ) {
							numphi = pvalue.toUInt();
						} else if( pname == "samples" ) {
							const unsigned int samples = pvalue.toUInt();
							const Scalar base = sqrt(Scalar(samples)/PI);
							numtheta = static_cast<unsigned int>( base );
							numphi = static_cast<unsigned int>( PI*base );
						} else if( pname == "multiplybrdf" ) {
							multiplybrdf = pvalue.toBoolean();
						} else if( pname == "irradiance_cache" ) {
							irradiance_cache = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddAmbientOcclusionShaderOp( name.c_str(), numtheta, numphi, multiplybrdf, irradiance_cache );
				}
			};

			struct DirectLightingShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String bsdf = "none";
					bool nonmeshlights = true;
					bool meshlights = true;
					bool cache = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "bsdf" ) {
							bsdf = pvalue;
						} else if( pname == "nonmeshlights" ) {
							nonmeshlights = pvalue.toBoolean();
						} else if( pname == "meshlights" ) {
							meshlights = pvalue.toBoolean();
						} else if( pname == "cache" ) {
							cache = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddDirectLightingShaderOp( name.c_str(), bsdf=="none"?0:bsdf.c_str(), nonmeshlights, meshlights, cache );
				}
			};

			struct SimpleSubSurfaceScatteringShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					unsigned int numpoints = 1000;
					double error = 0.001;
					unsigned int maxPointsPerNode = 40;
					unsigned char maxDepth = 8;
					double irrad_scale = 1.0;
					double geometric_scale = 1.0;
					bool multiplyBSDF = false;
					bool regenerate = true;
					String shader = "none";
					bool cache = true;
					bool low_discrepancy = true;
					double extinction[3] = {0.02, 0.03, 0.09};

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "numpoints" ) {
							numpoints = pvalue.toUInt();
						} else if( pname == "error" ) {
							error = pvalue.toDouble();
						} else if( pname == "maxpointspernode" ) {
							maxPointsPerNode = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxDepth = pvalue.toUChar();
						} else if( pname == "irrad_scale" ) {
							irrad_scale = pvalue.toDouble();
						} else if( pname == "geometric_scale" ) {
							geometric_scale = pvalue.toDouble();
						} else if( pname == "multiplybsdf" ) {
							multiplyBSDF = pvalue.toBoolean();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else if( pname == "shader" ) {
							shader = pvalue;
						} else if( pname == "cache" ) {
							cache = pvalue.toBoolean();
						} else if( pname == "low_discrepancy" ) {
							low_discrepancy = pvalue.toBoolean();
						} else if( pname == "extinction" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &extinction[0], &extinction[1], &extinction[2] );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddSimpleSubSurfaceScatteringShaderOp( name.c_str(), numpoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, shader.c_str(), cache, low_discrepancy, extinction );
				}
			};

			struct DiffusionApproximationSubSurfaceScatteringShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					unsigned int numpoints = 1000;
					double error = 0.001;
					unsigned int maxPointsPerNode = 40;
					unsigned char maxDepth = 8;
					double irrad_scale = 1.0;
					double geometric_scale = 1.0;
					bool multiplyBSDF = false;
					bool regenerate = true;
					String shader = "none";
					bool cache = true;
					bool low_discrepancy = true;
					double scattering[3] = {2.19, 2.62, 3.0};
					double absorption[3] = {0.0021, 0.0041, 0.0071};
					double ior = 1.3;
					double g = 0.8;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "numpoints" ) {
							numpoints = pvalue.toUInt();
						} else if( pname == "error" ) {
							error = pvalue.toDouble();
						} else if( pname == "maxpointspernode" ) {
							maxPointsPerNode = pvalue.toUInt();
						} else if( pname == "maxdepth" ) {
							maxDepth = pvalue.toUChar();
						} else if( pname == "irrad_scale" ) {
							irrad_scale = pvalue.toDouble();
						} else if( pname == "geometric_scale" ) {
							geometric_scale = pvalue.toDouble();
						} else if( pname == "multiplybsdf" ) {
							multiplyBSDF = pvalue.toBoolean();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else if( pname == "shader" ) {
							shader = pvalue;
						} else if( pname == "cache" ) {
							cache = pvalue.toBoolean();
						} else if( pname == "low_discrepancy" ) {
							low_discrepancy = pvalue.toBoolean();
						} else if( pname == "scattering" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &scattering[0], &scattering[1], &scattering[2] );
						} else if( pname == "absorption" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &absorption[0], &absorption[1], &absorption[2] );
						} else if( pname == "ior" ) {
							ior = pvalue.toDouble();
						} else if( pname == "g" ) {
							g = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddDiffusionApproximationSubSurfaceScatteringShaderOp( name.c_str(), numpoints, error, maxPointsPerNode, maxDepth, irrad_scale, geometric_scale, multiplyBSDF, regenerate, shader.c_str(), cache, low_discrepancy, scattering, absorption, ior, g );
				}
			};

			struct AreaLightShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					double width = 1.0;
					double height = 1.0;
					double location[3] = {0, 10, 0};
					double dir[3] = {0, -1, 0};
					unsigned int samples = 9;
					String emm = "color_white";
					Scalar power = 1.0;
					String N = "1.0";
					Scalar hotspot = PI;
					bool cache = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "width" ) {
							width = pvalue.toDouble();
						} else if( pname == "height" ) {
							height = pvalue.toDouble();
						} else if( pname == "location" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &location[0], &location[1], &location[2] );
						} else if( pname == "dir" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &dir[0], &dir[1], &dir[2] );
						} else if( pname == "make_dir" ) {
							double target[3] = {0,0,0};
							sscanf( pvalue.c_str(), "%lf %lf %lf", &target[0], &target[1], &target[2] );
							dir[0] = target[0] - location[0];
							dir[1] = target[1] - location[1];
							dir[2] = target[2] - location[2];
						} else if( pname == "samples" ) {
							samples = pvalue.toUInt();
						} else if( pname == "emission" ) {
							emm = pvalue;
						} else if( pname == "power" ) {
							power = pvalue.toDouble();
						} else if( pname == "N" ) {
							N = pvalue;
						} else if( pname == "cache" ) {
							cache = pvalue.toBoolean();
						} else if( pname == "hotspot" ) {
							hotspot = pvalue.toDouble() * DEG_TO_RAD;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddAreaLightShaderOp( name.c_str(), width, height, location, dir, samples, emm.c_str(), power, N.c_str(), hotspot, cache );
				}
			};

			struct TransparencyShaderOpAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String trans = "color_white";
					bool one_sided = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "transparency" ) {
							trans = pvalue;
						} else if( pname == "one_sided" ) {
							one_sided = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddTransparencyShaderOp( name.c_str(), trans.c_str(), one_sided );
				}
			};

			//////////////////////////////////////////
			// Shaders
			//////////////////////////////////////////

			struct StandardShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					std::vector<String> shaderops;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "shaderop" ) {
							shaderops.push_back( pvalue );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					const unsigned int num = shaderops.size();
					char* shmem = new char[num*256];
					memset( shmem, 0, num*256 );
					char** shops = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						shops[i] = &shmem[i*256];
						strncpy( shops[i], shaderops[i].c_str(), 255 );
					}

					bool bRet = pJob.AddStandardShader( name.c_str(), num, (const char**)shops );

					delete [] shops;
					delete [] shmem;

					return bRet;
				}
			};

			struct AdvancedShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					std::vector<String> shaderops;
					std::vector<unsigned int> mins, maxs;
					std::vector<char> operations;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "shaderop" ) {
							char buf[256] = {0};
							unsigned int min=1, max=10000;
							char operation = '+';
							sscanf( pvalue.c_str(), "%s %u %u %c", buf, &min, &max, &operation );
							shaderops.push_back( String(buf) );
							mins.push_back( min );
							maxs.push_back( max );
							operations.push_back( operation );
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					const unsigned int num = shaderops.size();
					char* shmem = new char[num*256];
					memset( shmem, 0, num*256 );
					char** shops = new char*[num];

					for( unsigned int i=0; i<num; i++ ) {
						shops[i] = &shmem[i*256];
						strncpy( shops[i], shaderops[i].c_str(), 255 );
					}

					bool bRet = pJob.AddAdvancedShader( name.c_str(), num, (const char**)shops, (unsigned int*)(&(*(mins.begin()))), (unsigned int*)(&(*(maxs.begin()))), (char*)(&(*(operations.begin()))) );

					delete [] shops;
					delete [] shmem;

					return bRet;
				}
			};

			struct DirectVolumeRenderingShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String szVolumeFilePattern = "";
					String iso_shader = "none";
					unsigned int width = 0;
					unsigned int height = 0;
					unsigned int startz = 0;
					unsigned int endz = 0;
					char accessor = 'n';
					char gradient = 'i';
					char composite = 'c';
					double dThresholdStart = 0.4;
					double dThresholdEnd = 1.0;
					char sampler = 'u';
					unsigned int samples = 50;
					String transfer_red = "none";
					String transfer_green = "none";
					String transfer_blue = "none";
					String transfer_alpha = "none";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file_pattern" ) {
							szVolumeFilePattern = pvalue;
						} else if( pname == "width" ) {
							width = pvalue.toUInt();
						} else if( pname == "height" ) {
							height = pvalue.toUInt();
						} else if( pname == "startz" ) {
							startz = pvalue.toUInt();
						} else if( pname == "endz" ) {
							endz = pvalue.toUInt();
						} else if( pname == "accessor" ) {
							accessor = tolower( pvalue[0] );
						} else if( pname == "gradient" ) {
							gradient = tolower( pvalue[0] );
						} else if( pname == "composite" ) {
							composite = tolower( pvalue[0] );
						} else if( pname == "threshold_start" ) {
							dThresholdStart = pvalue.toDouble();
						} else if( pname == "threshold_end" ) {
							dThresholdEnd = pvalue.toDouble();
						} else if( pname == "sampler" ) {
							sampler = tolower( pvalue[0] );
						} else if( pname == "samples" ) {
							samples = pvalue.toUInt();
						} else if( pname == "transfer_red" ) {
							transfer_red = pvalue;
						} else if( pname == "transfer_green" ) {
							transfer_green = pvalue;
						} else if( pname == "transfer_blue" ) {
							transfer_blue = pvalue;
						} else if( pname == "transfer_alpha" ) {
							transfer_alpha = pvalue;
						} else if( pname == "iso_shader" ) {
							iso_shader = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddDirectVolumeRenderingShader( name.c_str(), szVolumeFilePattern.c_str(), width, height, startz, endz,
						accessor, gradient, composite, dThresholdStart, dThresholdEnd, sampler, samples, transfer_red.c_str(), transfer_green.c_str(), transfer_blue.c_str(), transfer_alpha.c_str(), iso_shader=="none"?0:iso_shader.c_str()
						);
				}
			};

			struct SpectralDirectVolumeRenderingShaderAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String name = "noname";
					String szVolumeFilePattern = "";
					String iso_shader = "none";
					unsigned int width = 0;
					unsigned int height = 0;
					unsigned int startz = 0;
					unsigned int endz = 0;
					char accessor = 'n';
					char gradient = 'i';
					char composite = 'c';
					double dThresholdStart = 0.4;
					double dThresholdEnd = 1.0;
					char sampler = 'u';
					unsigned int samples = 50;
					String transfer_spectral = "none";
					String transfer_alpha = "none";

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "name" ) {
							name = pvalue;
						} else if( pname == "file_pattern" ) {
							szVolumeFilePattern = pvalue;
						} else if( pname == "width" ) {
							width = pvalue.toUInt();
						} else if( pname == "height" ) {
							height = pvalue.toUInt();
						} else if( pname == "startz" ) {
							startz = pvalue.toUInt();
						} else if( pname == "endz" ) {
							endz = pvalue.toUInt();
						} else if( pname == "accessor" ) {
							accessor = tolower( pvalue[0] );
						} else if( pname == "gradient" ) {
							gradient = tolower( pvalue[0] );
						} else if( pname == "composite" ) {
							composite = tolower( pvalue[0] );
						} else if( pname == "threshold_start" ) {
							dThresholdStart = pvalue.toDouble();
						} else if( pname == "threshold_end" ) {
							dThresholdEnd = pvalue.toDouble();
						} else if( pname == "sampler" ) {
							sampler = tolower( pvalue[0] );
						} else if( pname == "samples" ) {
							samples = pvalue.toUInt();
						} else if( pname == "transfer_spectral" ) {
							transfer_spectral = pvalue;
						} else if( pname == "transfer_alpha" ) {
							transfer_alpha = pvalue;
						} else if( pname == "iso_shader" ) {
							iso_shader = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddSpectralDirectVolumeRenderingShader( name.c_str(), szVolumeFilePattern.c_str(), width, height, startz, endz,
						accessor, gradient, composite, dThresholdStart, dThresholdEnd, sampler, samples, transfer_alpha.c_str(), transfer_spectral.c_str(), iso_shader=="none"?0:iso_shader.c_str()
						);
				}
			};

			//////////////////////////////////////////
			// Rasterizers
			//////////////////////////////////////////

			struct PixelPelRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String defaultshader = "global";
					unsigned int maxRecur = 10;
					unsigned int numSamples = 1;
					unsigned int numLumSamples = 1;
					double minImportance = 0.01;
					String radiancemap = "none";
					double radianceScale = 1.0;
					double radorient[3] = {0};
					bool radback = true;
					String pixelFilter = "none";
					String pixelSampler = "none";
					String luminarySampler = "none";
					double pixelSamplerParam = 1.0;
					double luminarySamplerParam = 1.0;
					double pixelFilterWidth = 1.0;
					double pixelFilterHeight = 1.0;
					double pixelFilterParamA = 1.0;
					double pixelFilterParamB = 1.0;
					bool showLuminaires = true;
					bool useiorstack = false;
					bool onlyonelight = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "defaultshader" ) {
							defaultshader = pvalue;
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "samples" ) {
							numSamples = pvalue.toUInt();
						} else if( pname == "lum_samples" ) {
							numLumSamples = pvalue.toUInt();
						} else if( pname == "radiance_map" ) {
							radiancemap = pvalue;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "radiance_background" ) {
							radback = pvalue.toBoolean();
						} else if( pname == "radiance_orient" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &radorient[0], &radorient[1], &radorient[2] );
							radorient[0] *= DEG_TO_RAD;
							radorient[1] *= DEG_TO_RAD;
							radorient[2] *= DEG_TO_RAD;
						} else if( pname == "pixel_sampler" ) {
							pixelSampler = pvalue;
						} else if( pname == "pixel_sampler_param" ) {
							pixelSamplerParam = pvalue.toDouble();
						} else if( pname == "luminary_sampler" ) {
							luminarySampler = pvalue;
						} else if( pname == "luminary_sampler_param" ) {
							luminarySamplerParam = pvalue.toDouble();
						} else if( pname == "pixel_filter" ) {
							pixelFilter = pvalue;
						} else if( pname == "pixel_filter_width" ) {
							pixelFilterWidth = pvalue.toDouble();
						} else if( pname == "pixel_filter_height" ) {
							pixelFilterHeight = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramA" ) {
							pixelFilterParamA = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramB" ) {
							pixelFilterParamB = pvalue.toDouble();
						} else if( pname == "show_luminaires" ) {
							showLuminaires = pvalue.toBoolean();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "choose_one_light" ) {
							onlyonelight = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetPixelBasedPelRasterizer( numSamples, numLumSamples,
						maxRecur, minImportance, defaultshader.c_str(), radiancemap=="none"?0:radiancemap.c_str(), radback, radianceScale, radorient,
						pixelSampler=="none"?0:pixelSampler.c_str(), pixelSamplerParam,
						luminarySampler=="none"?0:luminarySampler.c_str(), luminarySamplerParam,
						pixelFilter=="none"?0:pixelFilter.c_str(), pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB,
						showLuminaires, useiorstack, onlyonelight );
				}
			};

			struct PixelIntegratingSpectralRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String defaultshader = "global";
					unsigned int maxRecur = 10;
					unsigned int numSamples = 1;
					unsigned int numLumSamples = 1;
					unsigned int numSpectralSamples = 1;
					unsigned int numWavelengths = 10;
					double nmbegin = 380.0;
					double nmend = 780.0;
					double minImportance = 0.01;
					String radiancemap = "none";
					double radianceScale = 1.0;
					double radorient[3] = {0};
					bool radback = true;
					String pixelFilter = "none";
					String pixelSampler = "none";
					String luminarySampler = "none";
					double pixelSamplerParam = 1.0;
					double luminarySamplerParam = 1.0;
					double pixelFilterWidth = 1.0;
					double pixelFilterHeight = 1.0;
					double pixelFilterParamA = 1.0;
					double pixelFilterParamB = 1.0;
					bool showLuminaires = true;
					bool useiorstack = false;
					bool onlyonelight = false;
					bool integrateRGB = false;
					std::vector<double> spd_wavelengths;
					std::vector<double> spd_r;
					std::vector<double> spd_g;
					std::vector<double> spd_b;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "defaultshader" ) {
							defaultshader = pvalue;
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "samples" ) {
							numSamples = pvalue.toUInt();
						} else if( pname == "lum_samples" ) {
							numLumSamples = pvalue.toUInt();
						} else if( pname == "radiance_map" ) {
							radiancemap = pvalue;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "spectral_samples" ) {
							numSpectralSamples = pvalue.toUInt();
						} else if( pname == "num_wavelengths" ) {
							numWavelengths = pvalue.toUInt();
						} else if( pname == "nmbegin" ) {
							nmbegin = pvalue.toDouble();
						} else if( pname == "nmend" ) {
							nmend = pvalue.toDouble();
						} else if( pname == "radiance_map" ) {
							radiancemap = pname;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "radiance_background" ) {
							radback = pvalue.toBoolean();
						} else if( pname == "radiance_orient" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &radorient[0], &radorient[1], &radorient[2] );
							radorient[0] *= DEG_TO_RAD;
							radorient[1] *= DEG_TO_RAD;
							radorient[2] *= DEG_TO_RAD;
						} else if( pname == "pixel_sampler" ) {
							pixelSampler = pvalue;
						} else if( pname == "pixel_sampler_param" ) {
							pixelSamplerParam = pvalue.toDouble();
						} else if( pname == "luminary_sampler" ) {
							luminarySampler = pvalue;
						} else if( pname == "luminary_sampler_param" ) {
							luminarySamplerParam = pvalue.toDouble();
						} else if( pname == "pixel_filter" ) {
							pixelFilter = pvalue;
						} else if( pname == "pixel_filter_width" ) {
							pixelFilterWidth = pvalue.toDouble();
						} else if( pname == "pixel_filter_height" ) {
							pixelFilterHeight = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramA" ) {
							pixelFilterParamA = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramB" ) {
							pixelFilterParamB = pvalue.toDouble();
						} else if( pname == "show_luminaires" ) {
							showLuminaires = pvalue.toBoolean();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "choose_one_light" ) {
							onlyonelight = pvalue.toBoolean();
						} else if( pname == "integrate_rgb" ) {
							integrateRGB = pvalue.toBoolean();
						} else if( pname == "rgb_spd" ) {
							// Read the spd from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double nm, r, g, b;
									fscanf( f, "%lf %lf %lf %lf", &nm, &r, &g, &b );
									spd_wavelengths.push_back( nm );
									spd_r.push_back( r );
									spd_g.push_back( g );
									spd_b.push_back( b );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "rgb_spd_wavelengths" ) {
							// Read the spd wavelengths from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double nm;
									fscanf( f, "%lf", &nm );
									spd_wavelengths.push_back( nm );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "rgb_spd_r" ) {
							// Read the spd red amplitude from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double r;
									fscanf( f, "%lf", &r );
									spd_r.push_back( r );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "rgb_spd_g" ) {
							// Read the spd green amplitude from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double g;
									fscanf( f, "%lf", &g );
									spd_g.push_back( g );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "rgb_spd_b" ) {
							// Read the spd blue amplitude from a file
							FILE* f = fopen( GlobalMediaPathLocator().Find(pvalue).c_str(), "r" );

							if( f ) {
								while( !feof( f ) ) {
									double b;
									fscanf( f, "%lf", &b );
									spd_b.push_back( b );
								}
								fclose( f );
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to open file `%s`", pvalue.c_str() );
								return false;
							}
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetPixelBasedSpectralIntegratingRasterizer( numSamples, numLumSamples, numSpectralSamples, nmbegin, nmend, numWavelengths, maxRecur, minImportance, defaultshader.c_str(), radiancemap=="none"?0:radiancemap.c_str(), radback, radianceScale, radorient,
						pixelSampler=="none"?0:pixelSampler.c_str(), pixelSamplerParam,
						luminarySampler=="none"?0:luminarySampler.c_str(), luminarySamplerParam,
						pixelFilter=="none"?0:pixelFilter.c_str(), pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB,
						showLuminaires, useiorstack, onlyonelight,
						integrateRGB, spd_wavelengths.size(), integrateRGB?&spd_wavelengths[0]:0, integrateRGB?&spd_r[0]:0, integrateRGB?&spd_g[0]:0, integrateRGB?&spd_b[0]:0
						);
				}
			};

			struct AdaptivePixelPelRasterizerAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String defaultshader = "global";
					unsigned int maxRecur = 10;
					unsigned int minSamples = 4;
					unsigned int maxSamples = 16;
					unsigned int numSteps = 3;
					unsigned int numLumSamples = 1;
					double minImportance = 0.01;
					double threshold = 0.005;
					String radiancemap = "none";
					double radianceScale = 1.0;
					double radorient[3] = {0};
					bool radback = true;
					bool bOutputSamples = false;
					String pixelFilter = "none";
					String pixelSampler = "none";
					String luminarySampler = "none";
					double pixelSamplerParam = 1.0;
					double luminarySamplerParam = 1.0;
					double pixelFilterWidth = 1.0;
					double pixelFilterHeight = 1.0;
					double pixelFilterParamA = 1.0;
					double pixelFilterParamB = 1.0;
					bool showLuminaires = true;
					bool useiorstack = false;
					bool onlyonelight = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "defaultshader" ) {
							defaultshader = pvalue;
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "min_samples" ) {
							minSamples = pvalue.toUInt();
						} else if( pname == "max_samples" ) {
							maxSamples = pvalue.toUInt();
						} else if( pname == "num_steps" ) {
							numSteps = pvalue.toUInt();
						} else if( pname == "lum_samples" ) {
							numLumSamples = pvalue.toUInt();
						} else if( pname == "radiance_map" ) {
							radiancemap = pvalue;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "threshold" ) {
							threshold = pvalue.toDouble();
						} else if( pname == "radiance_map" ) {
							radiancemap = pvalue;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "radiance_background" ) {
							radback = pvalue.toBoolean();
						} else if( pname == "show_samples" ) {
							bOutputSamples = pvalue.toBoolean();
						} else if( pname == "radiance_orient" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &radorient[0], &radorient[1], &radorient[2] );
							radorient[0] *= DEG_TO_RAD;
							radorient[1] *= DEG_TO_RAD;
							radorient[2] *= DEG_TO_RAD;
						} else if( pname == "pixel_sampler" ) {
							pixelSampler = pvalue;
						} else if( pname == "pixel_sampler_param" ) {
							pixelSamplerParam = pvalue.toDouble();
						} else if( pname == "luminary_sampler" ) {
							luminarySampler = pvalue;
						} else if( pname == "luminary_sampler_param" ) {
							luminarySamplerParam = pvalue.toDouble();
						} else if( pname == "pixel_filter" ) {
							pixelFilter = pvalue;
						} else if( pname == "pixel_filter_width" ) {
							pixelFilterWidth = pvalue.toDouble();
						} else if( pname == "pixel_filter_height" ) {
							pixelFilterHeight = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramA" ) {
							pixelFilterParamA = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramB" ) {
							pixelFilterParamB = pvalue.toDouble();
						} else if( pname == "show_luminaires" ) {
							showLuminaires = pvalue.toBoolean();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "choose_one_light" ) {
							onlyonelight = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetAdaptivePixelBasedPelRasterizer( minSamples, maxSamples, numSteps, numLumSamples, threshold, bOutputSamples, maxRecur, minImportance, defaultshader.c_str(), radiancemap=="none"?0:radiancemap.c_str(), radback, radianceScale, radorient,
						pixelSampler=="none"?0:pixelSampler.c_str(), pixelSamplerParam,
						luminarySampler=="none"?0:luminarySampler.c_str(), luminarySamplerParam,
						pixelFilter=="none"?0:pixelFilter.c_str(), pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB,
						showLuminaires, useiorstack, onlyonelight
						);
				}
			};

			struct PixelPelRasterizerContrastAAAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String defaultshader = "global";
					unsigned int maxRecur = 10;
					unsigned int numSamples = 1;
					unsigned int numLumSamples = 1;
					double minImportance = 0.01;
					String radiancemap = "none";
					double radianceScale = 1.0;
					double radorient[3] = {0};
					bool radback = true;
					String pixelFilter = "none";
					String pixelSampler = "none";
					String luminarySampler = "none";
					double pixelSamplerParam = 1.0;
					double luminarySamplerParam = 1.0;
					double pixelFilterWidth = 1.0;
					double pixelFilterHeight = 1.0;
					double pixelFilterParamA = 1.0;
					double pixelFilterParamB = 1.0;
					bool showLuminaires = true;
					bool useiorstack = false;
					bool onlyonelight = false;
					double contrast_threshold[3] = {0.01, 0.01, 0.01};
					bool show_samples = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "defaultshader" ) {
							defaultshader = pvalue;
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "samples" ) {
							numSamples = pvalue.toUInt();
						} else if( pname == "lum_samples" ) {
							numLumSamples = pvalue.toUInt();
						} else if( pname == "radiance_map" ) {
							radiancemap = pvalue;
						} else if( pname == "radiance_scale" ) {
							radianceScale = pvalue.toDouble();
						} else if( pname == "radiance_background" ) {
							radback = pvalue.toBoolean();
						} else if( pname == "radiance_orient" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &radorient[0], &radorient[1], &radorient[2] );
							radorient[0] *= DEG_TO_RAD;
							radorient[1] *= DEG_TO_RAD;
							radorient[2] *= DEG_TO_RAD;
						} else if( pname == "pixel_sampler" ) {
							pixelSampler = pvalue;
						} else if( pname == "pixel_sampler_param" ) {
							pixelSamplerParam = pvalue.toDouble();
						} else if( pname == "luminary_sampler" ) {
							luminarySampler = pvalue;
						} else if( pname == "luminary_sampler_param" ) {
							luminarySamplerParam = pvalue.toDouble();
						} else if( pname == "pixel_filter" ) {
							pixelFilter = pvalue;
						} else if( pname == "pixel_filter_width" ) {
							pixelFilterWidth = pvalue.toDouble();
						} else if( pname == "pixel_filter_height" ) {
							pixelFilterHeight = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramA" ) {
							pixelFilterParamA = pvalue.toDouble();
						} else if( pname == "pixel_filter_paramB" ) {
							pixelFilterParamB = pvalue.toDouble();
						} else if( pname == "show_luminaires" ) {
							showLuminaires = pvalue.toBoolean();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "choose_one_light" ) {
							onlyonelight = pvalue.toBoolean();
						} else if( pname == "contrast_threshold" ) {
							sscanf( pvalue.c_str(), "%lf %lf %lf", &contrast_threshold[0], &contrast_threshold[1], &contrast_threshold[2] );
						} else if( pname == "show_samples" ) {
							show_samples = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetContrastAAPixelBasedPelRasterizer( numSamples, numLumSamples,
						maxRecur, minImportance, defaultshader.c_str(), radiancemap=="none"?0:radiancemap.c_str(), radback, radianceScale, radorient,
						pixelSampler=="none"?0:pixelSampler.c_str(), pixelSamplerParam,
						luminarySampler=="none"?0:luminarySampler.c_str(), luminarySamplerParam,
						pixelFilter=="none"?0:pixelFilter.c_str(), pixelFilterWidth, pixelFilterHeight, pixelFilterParamA, pixelFilterParamB,
						showLuminaires, useiorstack, onlyonelight, contrast_threshold, show_samples );
				}
			};

			struct FileRasterizerOutputAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String pattern = "none";
					bool multiple = false;
					char type = 0;
					int bpp = 8;
					char color_space = 1;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "pattern" ) {
							pattern = pvalue;
						} else if( pname == "multiple" ) {
							multiple = pvalue.toBoolean();
						} else if( pname == "type" ) {
							if( pvalue == "TGA" ) {
								type = 0;
							} else if( pvalue == "PPM" ) {
								type = 1;
							} else if( pvalue == "PNG" ) {
						#ifndef NO_PNG_SUPPORT
								type = 2;
						#else
								type = 0;
								GlobalLog()->PrintEasyWarning( "AsciiCommandParser::ParseAddRasterizeroutput::File: NO PNG SUPPORT was compiled, reverting to TGA instead" );
						#endif
							} else if( pvalue == "TIFF" ) {
						#ifndef NO_TIFF_SUPPORT
								type = 4;
						#else
								type = 0;
								GlobalLog()->PrintEasyWarning( "AsciiCommandParser::ParseAddRasterizeroutput::File: NO TIFF SUPPORT was compiled, reverting to TGA instead" );
						#endif
							} else if( pvalue == "HDR" ) {
								type = 3;
							} else if( pvalue == "RGBEA" ) {
								type = 5;
							} else if( pvalue == "EXR" ) {
						#ifndef NO_EXR_SUPPORT
								type = 6;
						#else
								type = 0;
								GlobalLog()->PrintEasyWarning( "AsciiCommandParser::ParseAddRasterizeroutput::File: NO EXR SUPPORT was compiled, reverting to TGA instead" );
						#endif
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown output file type type `%s`", pvalue.c_str() );
								return false;
							}
						} else if( pname == "bpp" ) {
							bpp = pvalue.toInt();
						} else if( pname == "color_space" ) {
							if( pvalue=="Rec709RGB_Linear" ) {
								color_space = 0;
							} else if( pvalue=="sRGB" ) {
								color_space = 1;
							} else if( pvalue=="ROMMRGB_Linear" ) {
								color_space = 2;
							} else if( pvalue=="ProPhotoRGB" ) {
								color_space = 3;
							} else {
								GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Unknown color space `%s`", pvalue.c_str() );
								return false;
							}
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddFileRasterizerOutput( pattern.c_str(), multiple, type, (unsigned char)bpp, color_space );
				}
			};


			//////////////////////////////////////////
			// Photon Mapping
			//////////////////////////////////////////

			struct CausticPelPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int photons = 10000;
					double power_scale = 1.0;
					unsigned int maxRecur = 10;
					double minImportance = 0.01;
					bool branch = true;
					bool reflect = true;
					bool refract = true;
					bool nonmeshlights = true;
					bool useiorstack = false;
					unsigned int temporal_samples = 100;
					bool regenerate = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "num" ) {
							photons = pvalue.toUInt();
						} else if( pname == "power_scale" ) {
							power_scale = pvalue.toDouble();
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "branch" ) {
							branch = pvalue.toBoolean();
						} else if( pname == "reflect" ) {
							reflect = pvalue.toBoolean();
						} else if( pname == "refract" ) {
							refract = pvalue.toBoolean();
						} else if( pname == "nonmeshlights" ) {
							nonmeshlights = pvalue.toBoolean();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "temporal_samples" ) {
							temporal_samples = pvalue.toUInt();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					std::cout << "Shooting Caustic Pel Photons: " << std::endl;

					return pJob.ShootCausticPelPhotons( photons, power_scale, maxRecur, minImportance, branch, reflect, refract, nonmeshlights, useiorstack, temporal_samples, regenerate );
				}
			};

			struct CausticSpectralPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int photons = 10000;
					double power_scale = 1.0;
					unsigned int maxRecur = 10;
					double nmbegin = 400.0;
					double nmend = 700.0;
					unsigned int numWavelengths = 30;
					double minImportance = 0.01;
					bool useiorstack = false;
					bool branch = true;
					bool reflect = true;
					bool refract = true;
					unsigned int temporal_samples = 100;
					bool regenerate = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "num" ) {
							photons = pvalue.toUInt();
						} else if( pname == "power_scale" ) {
							power_scale = pvalue.toDouble();
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "nmbegin" ) {
							nmbegin = pvalue.toDouble();
						} else if( pname == "nmend" ) {
							nmend = pvalue.toDouble();
						} else if( pname == "num_wavelengths" ) {
							numWavelengths = pvalue.toUInt();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "branch" ) {
							branch = pvalue.toBoolean();
						} else if( pname == "reflect" ) {
							reflect = pvalue.toBoolean();
						} else if( pname == "refract" ) {
							refract = pvalue.toBoolean();
						} else if( pname == "temporal_samples" ) {
							temporal_samples = pvalue.toUInt();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					std::cout << "Shooting Caustic Spectral Photons: " << std::endl;

					return pJob.ShootCausticSpectralPhotons( photons, power_scale, maxRecur, minImportance, nmbegin, nmend, numWavelengths, useiorstack, branch, reflect, refract, temporal_samples, regenerate );
				}
			};

			struct GlobalSpectralPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int photons = 10000;
					double power_scale = 1.0;
					unsigned int maxRecur = 10;
					double nmbegin = 400.0;
					double nmend = 700.0;
					unsigned int numWavelengths = 30;
					double minImportance = 0.01;
					bool useiorstack = false;
					bool branch = true;
					unsigned int temporal_samples = 100;
					bool regenerate = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "num" ) {
							photons = pvalue.toUInt();
						} else if( pname == "power_scale" ) {
							power_scale = pvalue.toDouble();
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "nmbegin" ) {
							nmbegin = pvalue.toDouble();
						} else if( pname == "nmend" ) {
							nmend = pvalue.toDouble();
						} else if( pname == "num_wavelengths" ) {
							numWavelengths = pvalue.toUInt();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "branch" ) {
							branch = pvalue.toBoolean();
						} else if( pname == "temporal_samples" ) {
							temporal_samples = pvalue.toUInt();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					std::cout << "Shooting Global Spectral Photons: " << std::endl;

					return pJob.ShootGlobalSpectralPhotons( photons, power_scale, maxRecur, minImportance, nmbegin, nmend, numWavelengths, useiorstack, branch, temporal_samples, regenerate );
				}
			};

			struct TranslucentPelPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int photons = 10000;
					unsigned int maxRecur = 10;
					double minImportance = 0.01;
					double power_scale = 1.0;
					bool nonmeshlights = true;
					bool useiorstack = false;
					bool reflect = true;
					bool refract = true;
					bool direct_translucent = true;
					unsigned int temporal_samples = 100;
					bool regenerate = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "num" ) {
							photons = pvalue.toUInt();
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "power_scale" ) {
							power_scale = pvalue.toDouble();
						} else if( pname == "nonmeshlights" ) {
							nonmeshlights = pvalue.toBoolean();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "reflect" ) {
							reflect = pvalue.toBoolean();
						} else if( pname == "refract" ) {
							refract = pvalue.toBoolean();
						} else if( pname == "direct_translucent" ) {
							direct_translucent = pvalue.toBoolean();
						} else if( pname == "temporal_samples" ) {
							temporal_samples = pvalue.toUInt();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					std::cout << "Shooting Translucent Pel Photons: " << std::endl;

					return pJob.ShootTranslucentPelPhotons( photons, power_scale, maxRecur, minImportance, reflect, refract, direct_translucent, nonmeshlights, useiorstack, temporal_samples, regenerate );
				}
			};

			struct ShadowPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int photons = 10000;
					unsigned int temporal_samples = 100;
					bool regenerate = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "num" ) {
							photons = pvalue.toUInt();
						} else if( pname == "temporal_samples" ) {
							temporal_samples = pvalue.toUInt();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					std::cout << "Shooting Shadow Photons: " << std::endl;

					return pJob.ShootShadowPhotons( photons, temporal_samples, regenerate );
				}
			};

			struct GlobalPelPhotonMapGenerateAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					unsigned int photons = 10000;
					double power_scale = 1.0;
					unsigned int maxRecur = 10;
					double minImportance = 0.01;
					bool branch = true;
					bool nonmeshlights = true;
					bool useiorstack = false;
					unsigned int temporal_samples = 100;
					bool regenerate = true;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "num" ) {
							photons = pvalue.toUInt();
						} else if( pname == "power_scale" ) {
							power_scale = pvalue.toDouble();
						} else if( pname == "max_recursion" ) {
							maxRecur = pvalue.toUInt();
						} else if( pname == "min_importance" ) {
							minImportance = pvalue.toDouble();
						} else if( pname == "branch" ) {
							branch = pvalue.toBoolean();
						} else if( pname == "nonmeshlights" ) {
							nonmeshlights = pvalue.toBoolean();
						} else if( pname == "ior_stack" ) {
							useiorstack = pvalue.toBoolean();
						} else if( pname == "temporal_samples" ) {
							temporal_samples = pvalue.toUInt();
						} else if( pname == "regenerate" ) {
							regenerate = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					std::cout << "Shooting Global Pel Photons: " << std::endl;

					return pJob.ShootGlobalPelPhotons( photons, power_scale, maxRecur, minImportance, branch, nonmeshlights, useiorstack, temporal_samples, regenerate );
				}
			};

			struct CausticPelPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double radius = 0.0;
					double ellipse_ratio = 0.05;
					unsigned int min = 8;
					unsigned int max = 150;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "ellipse_ratio" ) {
							ellipse_ratio = pvalue.toDouble();
						} else if( pname == "min_photons" ) {
							min = pvalue.toUInt();
						} else if( pname == "max_photons" ) {
							max = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetCausticPelGatherParameters( radius, ellipse_ratio, min, max );
				}
			};

			struct CausticSpectralPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double radius = 0.0;
					double ellipse_ratio = 0.05;
					double nm_range = 10.0;
					unsigned int min = 8;
					unsigned int max = 150;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "ellipse_ratio" ) {
							ellipse_ratio = pvalue.toDouble();
						} else if( pname == "nm_range" ) {
							nm_range = pvalue.toDouble();
						} else if( pname == "min_photons" ) {
							min = pvalue.toUInt();
						} else if( pname == "max_photons" ) {
							max = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetCausticSpectralGatherParameters( radius, ellipse_ratio, min, max, nm_range );
				}
			};

			struct GlobalSpectralPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double radius = 0.0;
					double ellipse_ratio = 0.05;
					double nm_range = 10.0;
					unsigned int min = 8;
					unsigned int max = 150;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "ellipse_ratio" ) {
							ellipse_ratio = pvalue.toDouble();
						} else if( pname == "nm_range" ) {
							nm_range = pvalue.toDouble();
						} else if( pname == "min_photons" ) {
							min = pvalue.toUInt();
						} else if( pname == "max_photons" ) {
							max = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetGlobalSpectralGatherParameters( radius, ellipse_ratio, min, max, nm_range );
				}
			};

			struct TranslucentPelPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double radius = 0.0;
					double ellipse_ratio = 0.05;
					unsigned int min = 8;
					unsigned int max = 150;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "ellipse_ratio" ) {
							ellipse_ratio = pvalue.toDouble();
						} else if( pname == "min_photons" ) {
							min = pvalue.toUInt();
						} else if( pname == "max_photons" ) {
							max = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetTranslucentPelGatherParameters( radius, ellipse_ratio, min, max );
				}
			};

			struct ShadowPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double radius = 0.0;
					double ellipse_ratio = 0.05;
					unsigned int min = 1;
					unsigned int max = 100;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "ellipse_ratio" ) {
							ellipse_ratio = pvalue.toDouble();
						} else if( pname == "min_photons" ) {
							min = pvalue.toUInt();
						} else if( pname == "max_photons" ) {
							max = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetShadowGatherParameters( radius, ellipse_ratio, min, max );
				}
			};

			struct GlobalPelPhotonMapGatherAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double radius = 0.0;
					double ellipse_ratio = 0.05;
					unsigned int min = 8;
					unsigned int max = 150;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "radius" ) {
							radius = pvalue.toDouble();
						} else if( pname == "ellipse_ratio" ) {
							ellipse_ratio = pvalue.toDouble();
						} else if( pname == "min_photons" ) {
							min = pvalue.toUInt();
						} else if( pname == "max_photons" ) {
							max = pvalue.toUInt();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetGlobalPelGatherParameters( radius, ellipse_ratio, min, max );
				}
			};

			struct IrradianceCacheAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double tolerance = 0.1;
					unsigned int size = 100000;
					double min_spacing = 0.05;
					double max_spacing = min_spacing * 100;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "tolerance" ) {
							tolerance = pvalue.toDouble();
						} else if( pname == "size" ) {
							size = pvalue.toUInt();
						} else if( pname == "min_spacing" ) {
							min_spacing = pvalue.toDouble();
						} else if( pname == "max_spacing" ) {
							max_spacing = pvalue.toDouble();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetIrradianceCacheParameters( size, tolerance, min_spacing, max_spacing );
				}
			};

			struct KeyframeAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String element_type = "object";
					String element = "none";
					String param = "none";
					String value = "none";
					String interp = "none";
					String interp_params = "none";
					double time = 0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "element" ) {
							element = pvalue;
						} else if( pname == "element_type" ) {
							param = element_type;
						} else if( pname == "param" ) {
							param = pvalue;
						} else if( pname == "value" ) {
							value = pvalue;
						} else if( pname == "time" ) {
							time = pvalue.toDouble();
						} else if( pname == "interpolator" ) {
							interp = pvalue;
						} else if( pname == "interpolator_params" ) {
							interp_params = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.AddKeyframe( element_type.c_str(), element_type.c_str(), param.c_str(), value.c_str(), time, interp=="none"?0:interp.c_str(), interp_params=="none"?0:interp_params.c_str() );
				}
			};

			struct TimelineAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					String element_type = "object";
					String element = "none";
					String param = "none";
					String value = "none";
					String interp = "none";
					String interp_params = "none";
					double time = 0;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "element" ) {
							element = pvalue;
						} else if( pname == "element_type" ) {
							element_type = pvalue;
						} else if( pname == "param" ) {
							param = pvalue;
						} else if( pname == "value" ) {
							if( !pJob.AddKeyframe( element_type.c_str(), element.c_str(), param.c_str(), pvalue.c_str(), time, interp=="none"?0:interp.c_str(), interp_params=="none"?0:interp_params.c_str() ) ) {
								return false;
							}
						} else if( pname == "time" ) {
							time = pvalue.toDouble();
						} else if( pname == "interpolator" ) {
							interp = pvalue;
						} else if( pname == "interpolator_params" ) {
							interp_params = pvalue;
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return true;
				}
			};

			struct AnimationOptionsAsciiChunkParser : public IAsciiChunkParser
			{
				bool ParseChunk( const ParamsList& in, IJob& pJob ) const
				{
					// Set up the set of parameters we want
					// with defaults for each
					double time_start = 0;
					double time_end = 1.0;
					unsigned int num_frames = 30;
					bool do_fields = false;
					bool invert_fields = false;

					ParamsList::const_iterator i=in.begin(), e=in.end();
					for( ;i!=e; i++ ) {
						// Split the param
						String pname;
						String pvalue;
						if( !string_split( *i, pname, pvalue, ' ' ) ) {
							return false;
						}

						// Now search the parameter value names
						if( pname == "time_start" ) {
							time_start = pvalue.toDouble();
						} else if( pname == "time_end" ) {
							time_end = pvalue.toDouble();
						} else if( pname == "num_frames" ) {
							num_frames = pvalue.toUInt();
						} else if( pname == "do_fields" ) {
							do_fields = pvalue.toBoolean();
						} else if( pname == "invert_fields" ) {
							invert_fields = pvalue.toBoolean();
						} else {
							GlobalLog()->PrintEx( eLog_Error, "ChunkParser:: Failed to parse parameter name `%s`", pname.c_str() );
							return false;
						}
					}

					return pJob.SetAnimationOptions( time_start, time_end, num_frames, do_fields, invert_fields );
				}
			};
		}
	}
}


//////////////////////////////////////////////////
// Implementation AsciiSceneParser itself
//////////////////////////////////////////////////
AsciiSceneParser::AsciiSceneParser( const char * szFilename_ )
{
	memset( szFilename, 0, 1024 );
	if( szFilename_ ) {
		strcpy( szFilename, GlobalMediaPathLocator().Find(szFilename_).c_str() );
	}

	// Populate the default macros
	macros["PI"] = PI;
	macros["E"] = E_;
}

AsciiSceneParser::~AsciiSceneParser( )
{
}

using namespace RISE::Implementation::ChunkParsers;

bool AsciiSceneParser::ParseAndLoadScene( IJob& pJob )
{
	// Setup a map to map chunk names to their respective parsers
	std::map<std::string,IAsciiChunkParser*> chunks;

	chunks["uniformcolor_painter"] = new UniformColorPainterAsciiChunkParser();
	chunks["spectral_painter"] = new SpectralPainterAsciiChunkParser();
	chunks["png_painter"] = new PngPainterAsciiChunkParser();
	chunks["hdr_painter"] = new HdrPainterAsciiChunkParser();
	chunks["exr_painter"] = new ExrPainterAsciiChunkParser();
	chunks["tiff_painter"] = new TiffPainterAsciiChunkParser();
	chunks["checker_painter"] = new CheckerPainterAsciiChunkParser();
	chunks["lines_painter"] = new LinesPainterAsciiChunkParser();
	chunks["mandelbrot_painter"] = new MandelbrotPainterAsciiChunkParser();
	chunks["perlin2d_painter"] = new Perlin2DPainterAsciiChunkParser();
	chunks["perlin3d_painter"] = new Perlin3DPainterAsciiChunkParser();
	chunks["voronoi2d_painter"] = new Voronoi2DPainterAsciiChunkParser();
	chunks["voronoi3d_painter"] = new Voronoi3DPainterAsciiChunkParser();
	chunks["iridescent_painter"] = new IridescentPainterAsciiChunkParser();
	chunks["blackbody_painter"] = new BlackBodyPainterAsciiChunkParser();
	chunks["blend_painter"] = new BlendPainterAsciiChunkParser();

	chunks["piecewise_linear_function"] = new PiecewiseLinearFunctionChunkParser();
	chunks["piecewise_linear_function2d"] = new PiecewiseLinearFunction2DChunkParser();

	chunks["lambertian_material"] = new LambertianMaterialAsciiChunkParser();
	chunks["perfectreflector_material"] = new PerfectReflectorMaterialAsciiChunkParser();
	chunks["perfectrefractor_material"] = new PerfectRefractorMaterialAsciiChunkParser();
	chunks["polished_material"] = new PolishedMaterialAsciiChunkParser();
	chunks["dielectric_material"] = new DielectricMaterialAsciiChunkParser();
	chunks["lambertian_luminaire_material"] = new LambertianLuminaireMaterialAsciiChunkParser();
	chunks["phong_luminaire_material"] = new PhongLuminaireMaterialAsciiChunkParser();
	chunks["ashikminshirley_anisotropicphong_material"] = new AshikminShirleyAnisotropicPhongMaterialAsciiChunkParser();
	chunks["isotropic_phong_material"] = new IsotropicPhongMaterialAsciiChunkParser();
	chunks["translucent_material"] = new TranslucentMaterialAsciiChunkParser();
	chunks["biospec_skin_material"] = new BioSpecSkinMaterialAsciiChunkParser();
	chunks["generic_human_tissue_material"] = new GenericHumanTissueMaterialAsciiChunkParser();
	chunks["composite_material"] = new CompositeMaterialAsciiChunkParser();
	chunks["ward_isotropic_material"] = new WardIsotropicGaussianMaterialAsciiChunkParser();
	chunks["ward_anisotropic_material"] = new WardAnisotropicEllipticalGaussianMaterialAsciiChunkParser();
	chunks["cooktorrance_material"] = new CookTorranceMaterialAsciiChunkParser();
	chunks["orennayar_material"] = new OrenNayarMaterialAsciiChunkParser();
	chunks["schlick_material"] = new SchlickMaterialAsciiChunkParser();
	chunks["datadriven_material"] = new DataDrivenMaterialAsciiChunkParser();

	chunks["pinhole_camera"] = new PinholeCameraAsciiChunkParser();
	chunks["onb_pinhole_camera"] = new ONBPinholeCameraAsciiChunkParser();
	chunks["thinlens_camera"] = new ThinlensCameraAsciiChunkParser();
	chunks["realistic_camera"] = new RealisticCameraAsciiChunkParser();
	chunks["fisheye_camera"] = new FisheyeCameraAsciiChunkParser();
	chunks["orthographic_camera"] = new OrthographicCameraAsciiChunkParser();

	chunks["sphere_geometry"] = new SphereGeometryAsciiChunkParser();
	chunks["ellipsoid_geometry"] = new EllipsoidGeometryAsciiChunkParser();
	chunks["cylinder_geometry"] = new CylinderGeometryAsciiChunkParser();
	chunks["torus_geometry"] = new TorusGeometryAsciiChunkParser();
	chunks["infiniteplane_geometry"] = new InfinitePlaneGeometryAsciiChunkParser();
	chunks["box_geometry"] = new BoxGeometryAsciiChunkParser();
	chunks["clippedplane_geometry"] = new ClippedPlaneGeometryAsciiChunkParser();
	chunks["3dsmesh_geometry"] = new Mesh3DSGeometryAsciiChunkParser();
	chunks["rawmesh_geometry"] = new RAWMeshGeometryAsciiChunkParser();
	chunks["rawmesh2_geometry"] = new RAWMesh2GeometryAsciiChunkParser();
	chunks["beziermesh_geometry"] = new BezierMeshGeometryAsciiChunkParser();
	chunks["risemesh_geometry"] = new RISEMeshGeometryAsciiChunkParser();
	chunks["circulardisk_geometry"] = new CircularDiskGeometryAsciiChunkParser();
	chunks["bezierpatch_geometry"] = new BezierPatchGeometryAsciiChunkParser();
	chunks["bilinearpatch_geometry"] = new BilinearPatchGeometryAsciiChunkParser();

	chunks["bumpmap_modifier"] = new BumpmapModifierAsciiChunkParser();

	chunks["standard_object"] = new StandardObjectAsciiChunkParser();
	chunks["csg_object"] = new CSGObjectAsciiChunkParser();

	chunks["ambientocclusion_shaderop"] = new AmbientOcclusionShaderOpAsciiChunkParser();
	chunks["directlighting_shaderop"] = new DirectLightingShaderOpAsciiChunkParser();
	chunks["pathtracing_shaderop"] = new PathTracingShaderOpAsciiChunkParser();
	chunks["distributiontracing_shaderop"] = new DistributionTracingShaderOpAsciiChunkParser();
	chunks["finalgather_shaderop"] = new FinalGatherShaderOpAsciiChunkParser();
	chunks["simple_sss_shaderop"] = new SimpleSubSurfaceScatteringShaderOpAsciiChunkParser();
	chunks["diffusion_approximation_sss_shaderop"] = new DiffusionApproximationSubSurfaceScatteringShaderOpAsciiChunkParser();
	chunks["arealight_shaderop"] = new AreaLightShaderOpAsciiChunkParser();
	chunks["transparency_shaderop"] = new TransparencyShaderOpAsciiChunkParser();

	chunks["standard_shader"] = new StandardShaderAsciiChunkParser();
	chunks["advanced_shader"] = new AdvancedShaderAsciiChunkParser();
	chunks["directvolumerendering_shader"] = new DirectVolumeRenderingShaderAsciiChunkParser();
	chunks["spectraldirectvolumerendering_shader"] = new SpectralDirectVolumeRenderingShaderAsciiChunkParser();

	chunks["pixelpel_rasterizer"] = new PixelPelRasterizerAsciiChunkParser();
	chunks["pixelintegratingspectral_rasterizer"] = new PixelIntegratingSpectralRasterizerAsciiChunkParser();
	chunks["adaptivepixelpel_rasterizer"] = new AdaptivePixelPelRasterizerAsciiChunkParser();
	chunks["contrastAApixelpel_rasterizer"] = new PixelPelRasterizerContrastAAAsciiChunkParser();
	chunks["file_rasterizeroutput"] = new FileRasterizerOutputAsciiChunkParser();

	chunks["ambient_light"] = new AmbientLightAsciiChunkParser();
	chunks["omni_light"] = new OmniLightAsciiChunkParser();
	chunks["spot_light"] = new SpotLightAsciiChunkParser();
	chunks["directional_light"] = new DirectionalLightAsciiChunkParser();

	chunks["caustic_pel_photonmap"] = new CausticPelPhotonMapGenerateAsciiChunkParser();
	chunks["translucent_pel_photonmap"] = new TranslucentPelPhotonMapGenerateAsciiChunkParser();
	chunks["caustic_spectral_photonmap"] = new CausticSpectralPhotonMapGenerateAsciiChunkParser();
	chunks["global_pel_photonmap"] = new GlobalPelPhotonMapGenerateAsciiChunkParser();
	chunks["global_spectral_photonmap"] = new GlobalSpectralPhotonMapGenerateAsciiChunkParser();
	chunks["shadow_photonmap"] = new ShadowPhotonMapGenerateAsciiChunkParser();
	chunks["caustic_pel_gather"] = new CausticPelPhotonMapGatherAsciiChunkParser();
	chunks["translucent_pel_gather"] = new TranslucentPelPhotonMapGatherAsciiChunkParser();
	chunks["caustic_spectral_gather"] = new CausticSpectralPhotonMapGatherAsciiChunkParser();
	chunks["global_pel_gather"] = new GlobalPelPhotonMapGatherAsciiChunkParser();
	chunks["global_spectral_gather"] = new GlobalSpectralPhotonMapGatherAsciiChunkParser();
	chunks["shadow_gather"] = new ShadowPhotonMapGatherAsciiChunkParser();
	chunks["irradiance_cache"] = new IrradianceCacheAsciiChunkParser();

	chunks["keyframe"] = new KeyframeAsciiChunkParser();
	chunks["timeline"] = new TimelineAsciiChunkParser();
	chunks["animation_options"] = new AnimationOptionsAsciiChunkParser();

	// Open up the file and start parsing!
	struct stat file_stats = {0};
	stat( szFilename, &file_stats );
	unsigned int nSize = file_stats.st_size;

	// I realize this is ugly, but it is necessary for proper
	// clean up after breaking part way
	String strBuffer;
	strBuffer.resize( nSize );
	char* pBuffer = (char*)strBuffer.c_str();
	memset( pBuffer, 0, nSize );

	FILE* f = fopen( szFilename, "rb" );
	if( f ) {
		fread( pBuffer, nSize, 1, f );
		fclose( f );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to load scene file \'%s\'", szFilename );
		return false;
	}

	std::istringstream		in( pBuffer );
	unsigned int			linenum = 0;

	// Command parser for parsing commands embedded in the scene
	AsciiCommandParser* parser = new AsciiCommandParser();
	GlobalLog()->PrintNew( parser, __FILE__, __LINE__, "command parser" );

	{
		char				line[MAX_CHARS_PER_LINE] = {0};		// <sigh>....

		// Verify version number
		in.getline( line, MAX_CHARS_PER_LINE );
		linenum++;

		// First check the first few characters to see if it contains our marker
		static const char* id = "RISE ASCII SCENE";
		if( strncmp( line, id, strlen(id) ) ) {
			GlobalLog()->Print( eLog_Error, "AsciiSceneParser: Scene does not contain RISE ASCII SCENE marker" );
			return false;
		}

		// Next find the scene version number
		const char* num = &line[strlen(id)];

		int version = atoi( num );

		if( version != CURRENT_SCENE_VERSION ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Scene version problem, scene is version \'%d\', we require \'%d\'", version, CURRENT_SCENE_VERSION );
			return false;
		}
	}

	//
	// Parse the rest of the scene, basically read each line and see if
	//  we have a chunk, a comment or a command to pass to the command
	//  parser
	//

	std::stack<LOOP> loops;

	bool bInCommentBlock = false;
	for(;;) {
		char				line[MAX_CHARS_PER_LINE] = {0};		// <sigh>....
		in.getline( line, MAX_CHARS_PER_LINE );

		linenum++;

		if( in.fail() || in.eof() ) {
			break;
		}

		// Tokenize the string to get rid of comments etc
		String			tokens[1024];
		unsigned int numTokens = AsciiCommandParser::TokenizeString( line, tokens, 1024 );

		if( bInCommentBlock ) {
			if( tokens[0].size() >= 2 && tokens[0][0] == '*' && tokens[0][1] == '/' ) {
				bInCommentBlock = false;
			}
			continue;
		}

		if( numTokens == 0 ) {
			// Empty
			continue;
		}

		if( tokens[0][0] == '#' ) {
			// Comment
			continue;
		}

		if( tokens[0].size() >= 2 && tokens[0][0] == '/' && tokens[0][1] == '*' ) {
			// Comment block
			bInCommentBlock = true;
			continue;
		}

		if( tokens[0][0] == '>' ) {
			// Command
			if( !parser->ParseCommand( &tokens[1], numTokens-1, pJob ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to parse line \'%s\' (%u)", line, linenum );
				return false;
			}
			continue;
		}

		// Check for a macro definition
		if( tokens[0][0] == '!' || tokens[0] == "DEFINE" || tokens[0] == "define" ) {
			// We have a macro
			if( numTokens < 3 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough parameters for macro definition line (%u)", linenum );
				return false;
			}

			if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				return false;
			}

			if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				return false;
			}

			if( !add_macro( tokens[1], tokens[2] ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error adding new macro (%u)", linenum );
				return false;
			}
			continue;
		}

		// Check for macro removal
		if( tokens[0][0] == '~' || tokens[0] == "undef" || tokens[0] == "UNDEF" ) {
			if( numTokens < 2 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough parameters for macro removal line (%u)", linenum );
				return false;
			}

			if( !remove_macro( tokens[1] ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Couldn't find the macro to remove (%u)", linenum );
				return false;
			}
			continue;
		}

		// Check for loops
		if( tokens[0] == "FOR" ) {
			// loops require the following format
			// FOR <variable name> <start value> <end value> <increment size>
			if( numTokens < 5 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Not enough paramters for loop line (%u)", linenum );
				return false;
			}

			// First check to see if the variable name is already in the macro map
			if( macros.find( tokens[1] ) != macros.end() ) {
				// Already there
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Variable \'%s\' already exists line (%u)", tokens[1].c_str(), linenum );
				return false;
			}

			if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				return false;
			}

			if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				return false;
			}

			LOOP l;
            l.position = in.tellg();
			l.var = tokens[1];
			l.curvalue = atof( tokens[2].c_str() );
			l.endvalue = atof( tokens[3].c_str() );
			l.increment = atof( tokens[4].c_str() );
			l.linecount = linenum;

			macros[l.var] = l.curvalue;

			loops.push( l );
			continue;
		}

		// Check for loop end
		if( tokens[0] == "ENDFOR" ) {
            // We are at the end of the current loop
			if( loops.size() == 0 ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: LOOPEND found with no current loop, line (%u)", linenum );
			}

			LOOP& l = loops.top();
			l.curvalue += l.increment;

			MacroTable::iterator it = macros.find( l.var );

			if( l.curvalue > l.endvalue ) {
				// This loop is done, remove it from the queue and continue
				loops.pop();
				if( it == macros.end() ) {
					GlobalLog()->PrintEasyError( "AsciiSceneParser:: Fatal error in trying to remove loop variable" );
					return false;
				}
				macros.erase( it );
				continue;
			}

			// Otherwise, update the value in the macro list and continue
			if( it == macros.end() ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser:: Fatal error in trying to update loop variable" );
				return false;
			}

			it->second = l.curvalue;

			// Set the file back to the line this loop begins at and continue
			in.seekg( l.position );
			linenum = l.linecount;
			continue;
		}

		// Otherwise must be a chunk
		// Read the chunk type
		std::map<std::string,IAsciiChunkParser*>::iterator it = chunks.find( std::string(tokens[0].c_str()) );

		if( it == chunks.end() ) {
			GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to find chunk \'%s\' on line %u", tokens[0].c_str(), linenum );
			return false;
		}

		const IAsciiChunkParser* pChunkParser = it->second;

		// Parse the '{'
		{
			in.getline( line, MAX_CHARS_PER_LINE );
			linenum++;
			if( in.fail() ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Failed reading looking for '{' for chunk" );
				break;
			}

			String			toks[8];
			unsigned int numTokens = AsciiCommandParser::TokenizeString( line, toks, 8 );

			if( numTokens < 1 ) {
				return false;
			}

			if( toks[0][0] != '{' ) {
				GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Cannot find '{' for chunk" );
				return false;
			}

			// Keep reading the parameters for the chunk until we encounter the closing '}'
			IAsciiChunkParser::ParamsList chunkparams;
			for(;;) {
				in.getline( line, MAX_CHARS_PER_LINE );

				linenum++;
				if( in.fail() ) {
					GlobalLog()->PrintEasyError( "AsciiSceneParser::ParseScene:: Failed reading while reading chunk" );
					break;
				}

				// Don't bother reading comments or commands
				String			tokens[1024];
				unsigned int numTokens = AsciiCommandParser::TokenizeString( line, tokens, 1024 );

				if( numTokens < 1 ) {
					continue;
				}

				if( tokens[0][0] == '#' ) {
					continue;
				}

				if( tokens[0][0] == '>' ) {
					// We could optionally just parse the command...
					continue;
				}

				if( tokens[0][0] == '}' ) {
					// End of chunk, so break out
					break;
				}

				if( !substitute_macros_in_tokens( tokens, numTokens ) ) {
					GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing macro subsitution on line %u", linenum );
				}

				if( !evaluate_expressions_in_tokens( tokens, numTokens ) ) {
					GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Fatal error while performing math expression evaluation %u", linenum );
				}

				// Otherwise, assemble the tokens and add it to the chunk list
				String s;
				make_string_from_tokens( s, tokens, numTokens, " " );
				chunkparams.push_back( s );
			}

			// Finished reading a chunk so parse it
			if( !pChunkParser->ParseChunk( chunkparams, pJob ) ) {
				GlobalLog()->PrintEx( eLog_Error, "AsciiSceneParser: Failed to load chunk \'%s\' on line %u", tokens[0].c_str(), linenum );
				return false;
			}
		}
	}

	safe_release( parser );
	GlobalLog()->PrintEx( eLog_Info, "AsciiSceneParser: Successfully loaded \'%s\'", szFilename );

	// Free the chunks map
	{
		std::map<std::string,IAsciiChunkParser*>::iterator i=chunks.begin(), e=chunks.end();
		for( ; i!=e; i++ ) {
			delete i->second;
		}
		chunks.clear();
	}

	return true;
}

//////////////////////////////////////////////////
// Implementation of the macro substituion code
//////////////////////////////////////////////////

char AsciiSceneParser::substitute_macro( String& token )
{
	// A macro can be any part of a token
	std::string str( token.c_str() );
	unsigned int x = str.find_first_of( "@!" );

	std::string processed;

	if( x != std_string_npos ) {
		char macro_char = str[x];		// remember this, depending on whether its an @ or % we do different operations

		// We have a macro!
		if( x > 0 ) {
			processed = str.substr( 0, x );
		}
		str = str.substr( x+1, str.size() );

		// Find the end of the macro
		x = str.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZ_" );

		std::string macro;
		if( x == std_string_npos ) {
			macro = str;
		} else {
			macro = str.substr( 0, x );
		}

		MacroTable::const_iterator it = macros.find( macro.c_str() );

		if( it == macros.end() ) {
			return 2;	// Error
		}

		// Re-assemble the string
		static const int MAX_BUF_SIZE = 64;
		char buf[MAX_BUF_SIZE] = {0};
		if( macro_char == '@' ) {
			snprintf( buf, MAX_BUF_SIZE, "%.12f", it->second );
		} else {
			snprintf( buf, MAX_BUF_SIZE, "%.4d", (int)it->second );
		}
		processed.append( buf );

		if( x<str.size() ) {
			processed.append( str.substr( x, str.size() ) );
		}

		token = processed.c_str();

		return 1;	// Successfull subsitution
	}

	return 0;	// No substituion
}

bool AsciiSceneParser::substitute_macros_in_tokens( String* tokens, const unsigned int num_tokens )
{
	for( unsigned int i=0; i<num_tokens; i++ ) {
		for(;;) {
			char c = substitute_macro( tokens[i] );

			if( c==0 ) {
				break;
			}

			if( c==2 ) {
				return false;
			}
		}
	}

	return true;
}

bool AsciiSceneParser::add_macro( String& name, String& value  )
{
	// Add a new macro
	std::string str( name.c_str() );

	// Make sure only valid things are in the macro
	if( str.find_first_not_of( "ABCDEFGHIJKLMNOPQRSTUVWXYZ_" ) != std::string::npos ) {
		return false;
	}

	// Check to see if it already exists
	if( macros.find( name ) != macros.end() ) {
		return false;
	}

	macros[name] = atof(value.c_str() );
	return true;
}

bool AsciiSceneParser::remove_macro( String& name )
{
	MacroTable::iterator it = macros.find( name );
	if( it == macros.end() ) {
		return false;
	}

	macros.erase( it );
	return true;
}
