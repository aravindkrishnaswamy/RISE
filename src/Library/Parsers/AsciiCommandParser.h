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

#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class AsciiCommandParser : public virtual ICommandParser, public virtual Implementation::Reference
		{
		protected:
			virtual ~AsciiCommandParser();
			
			typedef bool ( *COMMANDFUNCTION)( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );

			struct tCommandPair {
				char				szCommand[256];			// maximum 256 char. commands
				COMMANDFUNCTION		pFunc;					// the function to call for the command
			};
			
			tCommandPair* functionList;

	public:
			static unsigned int TokenizeString( const char* szStr, String* tokens, unsigned int max_tokens );	
			AsciiCommandParser( );

			// This function parses a single line of a command
			bool ParseCommand( const char * szLine, IJob& pJob );
			bool ParseCommand( String* tokens, unsigned int numTokens, IJob& pJob );

			// Parsers for root commands
			static bool ParseSet( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRemove( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise);
			static bool ParsePredictRasterizationTime( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRasterize( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRasterizeAnimation( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseClearAll( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseLoad( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseRun( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseQuit( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseModify( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParsePhotonMap( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseMediaPath( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );
			static bool ParseEcho( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise );

			// Parsers for 2nd level remove commands
			static bool ParseRemovePainter( String* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveMaterial( String* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveObject( String* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveGeometry( String* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveLight( String* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveModifier( String* tokens, unsigned int num_tokens, IJob& pJob );
			static bool ParseRemoveRasterizerOutputs( String* tokens, unsigned int num_tokens, IJob& pJob );
		};
	}
}

#endif
