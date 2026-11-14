//////////////////////////////////////////////////////////////////////
//
//  AsciiCommandParser.h - Command parser that parses ascii
//    strings and loads them into a job
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 19, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ASCIICOMMANDPARSER_
#define ASCIICOMMANDPARSER_

#include "../Interfaces/ISceneParser.h"
#include "../Interfaces/IJob.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/RString.h"
#include "../Utilities/Reference.h"

#include <string>
#include <string_view>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class AsciiCommandParser : public virtual ICommandParser, public virtual Implementation::Reference
		{
		protected:
			virtual ~AsciiCommandParser();
			

	public:
			static unsigned int TokenizeString( const char* szStr, String* tokens, unsigned int max_tokens );
			static std::vector<std::string> Tokenize( std::string_view line );
			AsciiCommandParser( );

			// This function parses a single line of a command
			bool ParseCommand( const char * szLine, IJob& pJob );
			bool ParseCommand( String* tokens, unsigned int numTokens, IJob& pJob );

			// Parsers for root commands
			static bool ParseSet( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRemove( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise);
			static bool ParsePredictRasterizationTime( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRasterize( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRasterizeAnimation( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseClearAll( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseLoad( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRun( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseQuit( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseModify( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParsePhotonMap( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseMediaPath( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseEcho( const std::string* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );

			// Parsers for 2nd level remove commands
			static bool ParseRemovePainter( const std::string* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveMaterial( const std::string* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveObject( const std::string* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveGeometry( const std::string* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveLight( const std::string* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveModifier( const std::string* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveRasterizerOutputs( const std::string* tokens, unsigned int num_tokens, IJob& pJob );
		};
	}
}

#endif
