//////////////////////////////////////////////////////////////////////
//
//  IOptions.h - Interface to the options class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOPTIONS_
#define IOPTIONS_

#include "IReference.h"
#include "../Utilities/RString.h"

namespace RISE
{
	class IOptions :
		public virtual IReference
	{
	protected:
		IOptions(){};
		virtual ~IOptions(){};

	public:
		virtual int ReadInt( const char* name, int default_value ) = 0;
		virtual double ReadDouble( const char* name, double default_value ) = 0;
		virtual bool ReadBool( const char* name, bool default_value ) = 0;
		virtual String ReadString( const char* name, String default_value ) = 0;
	};

	// Returns global engine options, the global options file 
	// is controlled through the environment variable RISE_OPTIONS_FILE
	// alternatively, if that variable is missing it will look for the global.options 
	// file in the same folder as RISE_MEDIA_PATH
	IOptions& GlobalOptions();
}

#endif

