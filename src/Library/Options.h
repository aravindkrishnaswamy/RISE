//////////////////////////////////////////////////////////////////////
//
//  Options.h - The Options class allows a user to query
//    options from a file.  This works almost like
//    GetPrivateProfile... in windows
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef OPTIONS_
#define OPTIONS_

#include "Interfaces/IOptions.h"
#include "Utilities/Reference.h"
#include <map>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class Options :
			public virtual IOptions,
			public virtual Implementation::Reference
		{
		protected:
			virtual ~Options();

			std::map<String,String> options;

		public:
			Options( const char* filename );

			// From the options interface
			int ReadInt( const char* name, int default_value );
			double ReadDouble( const char* name, double default_value );
			bool ReadBool( const char* name, bool default_value );
			String ReadString( const char* name, String default_value );
		};
	}
}

#endif

