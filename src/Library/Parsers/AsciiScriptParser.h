//////////////////////////////////////////////////////////////////////
//
//  AsciiScriptParser.h - Defines the AsciiScriptParser class.  The 
//    AsciiScriptParser is basic line-by-line script parser where
//    each line is a command
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 22, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ASCII_SCRIPTPARSER_
#define ASCII_SCRIPTPARSER_

#include "../Interfaces/ISceneParser.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class AsciiScriptParser : public virtual IScriptParser, public virtual Reference
		{
		protected:
			virtual ~AsciiScriptParser();

			char		szFilename[1024];

		public:
			AsciiScriptParser( const char * szFilename_ );
			bool ParseScript( IJob& pJob );
		};
	}
}

#endif
