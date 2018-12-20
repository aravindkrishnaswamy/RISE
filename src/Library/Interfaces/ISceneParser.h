//////////////////////////////////////////////////////////////////////
//
//  ISceneParser.h - Defines an interface for scene parsers.  Scene
//  parsers are objects that can parse a particular scene and load
//  it in to a given scene object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 6, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISCENEPARSER_
#define ISCENEPARSER_

#include "IJob.h"

namespace RISE
{
	class ICommandParser : public virtual IReference
	{
	protected:
		ICommandParser(){};
		virtual ~ICommandParser(){};

	public:
		//! Parses a single command, processes it, and applies to the given job
		/// \return TRUE if the parsing was successful, FALSE otherwise
		virtual bool ParseCommand( 
			const char * szCommand,				///< [in] String containing the command to parse					
			IJob& pJob							///< [in/out] Where the command should be parsed into
			) = 0;
	};

	class IScriptParser : public virtual IReference
	{
	protected:
		IScriptParser(){};
		virtual ~IScriptParser(){};

	public:
		//! Parses and loads a script
		/// \return TRUE if the parsing and loading was successful, FALSE otherwise
		virtual bool ParseScript(
			IJob& pJob							///< [in/out] Where the script should be parsed to
			) = 0;
	};

	class ISceneParser : public virtual IReference
	{
	protected:
		ISceneParser(){};
		virtual ~ISceneParser(){};

	public:
		//! Parses and loads a scene
		/// \return TRUE if the parsing and loading was successful, FALSE otherwise
		virtual bool ParseAndLoadScene(
			IJob& pJob							///< [in/out] Where the scene should be loaded into
			) = 0;
	};
}

#endif
