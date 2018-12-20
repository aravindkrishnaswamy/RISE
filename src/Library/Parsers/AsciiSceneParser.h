//////////////////////////////////////////////////////////////////////
//
//  AsciiSceneParser.h - Defines the AsciiSceneParser class.  The 
//    AsciiSceneParser parses an ascii scene file.  This is the new
//    ascii scene format introduced on December 22, 2003.  The scene
//    is broken down into chunks where each chunk can be parsed
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 22, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ASCII_SCENEPARSER_
#define ASCII_SCENEPARSER_

#include "../Interfaces/ISceneParser.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RString.h"
#include "../Utilities/Math3D/Math3D.h"
#include <istream>

namespace RISE
{
	namespace Implementation
	{
		class AsciiSceneParser : public virtual ISceneParser, public virtual Reference
		{
		protected:
			virtual ~AsciiSceneParser();

			char		szFilename[1024];

			typedef std::map<String,Scalar> MacroTable;
			typedef std::map<String,String> StringMacroTable;

			MacroTable	macros;
			StringMacroTable string_macros;

			char substitute_macro( String& token );
			bool substitute_macros_in_tokens( String* tokens, const unsigned int num_tokens );

			bool add_macro( String& name, String& value );
			bool remove_macro( String& name );

			// The LOOP strucuture contains the data we need to 
			// iterate through loops
			struct LOOP
			{
				std::istream::pos_type position;			// Position in the file that loop contents begin at
				String var;									// Loop variable
				Scalar curvalue;							// Current value of the loop var
				Scalar endvalue;							// The value to end the loop at
				Scalar increment;							// The increment amount
				unsigned int linecount;						// The line count at the begining of the loop
			};

		public:
			AsciiSceneParser( const char * szFilename_ );
			bool ParseAndLoadScene( IJob& pJob );
		};
	}
}

#endif
