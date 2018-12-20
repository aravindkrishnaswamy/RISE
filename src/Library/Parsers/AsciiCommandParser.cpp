//////////////////////////////////////////////////////////////////////
//
//  AsciiCommandParser.cpp - Implementation of the ascii
//    command parser
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 19, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include "AsciiCommandParser.h"
#include "StdOutProgress.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/RTime.h"
#include "../Utilities/MediaPathLocator.h"
#include "../Utilities/RandomNumbers.h"

static const unsigned int RISE_NUMCOMMANDS = 13;
static const unsigned int RISE_MAX_TOKENS_PER_LINE = 1024;
static const char RISE_COMMENT = '#';

using namespace RISE;
using namespace RISE::Implementation;

AsciiCommandParser::AsciiCommandParser( )
{
	tCommandPair	funcTemp[RISE_NUMCOMMANDS] = 
	{
		{"set", ParseSet},
		{"remove", ParseRemove},
		{"modify", ParseModify},
		{"clearall", ParseClearAll},
		{"predict", ParsePredictRasterizationTime},
		{"render", ParseRasterize},
		{"renderanimation", ParseRasterizeAnimation},
		{"load", ParseLoad},
		{"photonmap", ParsePhotonMap},
		{"run", ParseRun},
		{"quit", ParseQuit},
		{"mediapath", ParseMediaPath},
		{"echo", ParseEcho},
	};

	functionList = new tCommandPair[RISE_NUMCOMMANDS];
	GlobalLog()->PrintNew( functionList, __FILE__, __LINE__, "functions list" );

	memcpy( functionList, funcTemp, sizeof( tCommandPair ) * RISE_NUMCOMMANDS );
}

AsciiCommandParser::~AsciiCommandParser( )
{
	if( functionList ) {
		GlobalLog()->PrintDelete( functionList, __FILE__, __LINE__ );
		delete [] functionList;
		functionList = 0;
	}
}

unsigned int AsciiCommandParser::TokenizeString( const char* szStr, String* tokens, unsigned int max_tokens )
{
	static const int std_string_npos = int(std::string::npos);
	std::string		st( szStr );
	unsigned int	cur_token = 0;

	int	x = 0;
	std::string			mine = st;

	for(;;)
	{
		x = mine.find_first_not_of( " \t\r" );

		if( x == std_string_npos ) {
			break;
		}

		mine = mine.substr( x, mine.size() );
		x = mine.find_first_of( " \t\r" );

		if( x == std_string_npos ) {
			x = mine.size();
		}

		tokens[cur_token] = String( mine.substr( 0, x ).c_str() );
		cur_token++;

		if( x == int(mine.size()) ) {
			break;
		}

		if( cur_token > max_tokens ) {
			break;
		}

		mine = mine.substr( x+1, mine.size() );

		if( mine.size() == 0 ) {
			break;
		}
	}

	return cur_token;
}

bool AsciiCommandParser::ParseCommand( const char * szLine, IJob& pJob )
{
	// First parse the line into tokens
	String			tokens[RISE_MAX_TOKENS_PER_LINE];

	unsigned int numTokens = TokenizeString( szLine, tokens, RISE_MAX_TOKENS_PER_LINE );

	String&			token = tokens[0];

	// First thing to always check is for the comment
	bool	bSuccess = false;

	if( strlen(szLine) == 0 ) {
		bSuccess = true;
	} else if( numTokens ) {
		if( token[0] == RISE_COMMENT ) {
			bSuccess = true;
		}
	}

	if( !bSuccess ) {
		bSuccess = ParseCommand( tokens, numTokens, pJob );
	}

	return bSuccess;
}

bool AsciiCommandParser::ParseCommand( String* tokens, unsigned int numTokens, IJob& pJob )
{
	// Otherwise, read the first token, the first token should always tell us
	// what we are dealing with and call that function
	String&			token = tokens[0];
	for( unsigned int j=0; j<RISE_NUMCOMMANDS; j++ ) {
		if( token == functionList[j].szCommand ) {
			return (*functionList[j].pFunc)(&tokens[1], numTokens-1, pJob, this);
		}
	}
	return false;
}

bool AsciiCommandParser::ParseClearAll( String*, unsigned int, IJob& pJob, AsciiCommandParser* )
{
	return pJob.ClearAll();
}

bool AsciiCommandParser::ParseLoad( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	// Loads an ascii scene file
	if( num_tokens < 1 ) {
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseLoad: No file specified to load" );
		return false;
	}

	return pJob.LoadAsciiScene( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRun( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	// Loads an ascii script file
	if( num_tokens < 1 ) {
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseLoad: No file specified to load" );
		return false;
	}

	return pJob.RunAsciiScript( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseQuit( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	GlobalLog()->Print( eLog_Warning, "Bye!" );
	exit(1);
	return true;
}

bool ParseSetAccelerator( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	// Check for the correct number of parameters
	if( num_tokens < 3 )
	{
		GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseSetAccelerator: Wrong number of parameters: %d", num_tokens );
		return false;
	}

	bool bOctree = false;
	bool bBSP = false;
	unsigned int nMaxObj;
	unsigned int nMaxRecur;

	char c = toupper((tokens[0].c_str())[0]);
	if( c == 'B' ) {
		bBSP = true;
	} else if( c == 'O' ) {
		bOctree = true;
	}

	nMaxObj = tokens[1].toUInt();
	nMaxRecur = tokens[2].toUInt();

	return pJob.SetPrimaryAcceleration( bBSP, bOctree, nMaxObj, nMaxRecur );
}

bool AsciiCommandParser::ParseSet( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser*)
{
	// We are to add some element into the engine
	if( num_tokens < 1 )
	{
		// But no element was specified!
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseSet: No element to add" );
		return false;
	}

	// Otherwise just do strcmps to find out what we add
	if( tokens[0] == "accelerator" ) {
		return ParseSetAccelerator( &tokens[1], num_tokens-1, pJob );
	}
	
	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseSet: Unknown element type: %s", tokens[0].c_str() );
	return false;
}

bool ParseModifyObject_UV_Spherical( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 2 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV_Spherical: not enough parameters" );
		return false;
	}

	const double radius = tokens[1].toDouble();

	return pJob.SetObjectUVToSpherical( tokens[0].c_str(), radius );
}

bool ParseModifyObject_UV_Box( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 4 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV_Box: not enough parameters" );
		return false;
	}

	const double width = tokens[1].toDouble();
	const double height = tokens[2].toDouble();
	const double depth = tokens[3].toDouble();

	return pJob.SetObjectUVToBox( tokens[0].c_str(), width, height, depth );
}

bool ParseModifyObject_UV_Cylindrical( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 4 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV_Cylindrical: not enough parameters" );
		return false;
	}

	char axis = (tokens[2])[0];
	const double radius = tokens[1].toDouble();
	const double size = tokens[2].toDouble();

	return pJob.SetObjectUVToCylindrical( tokens[0].c_str(), radius, axis, size );
}

bool ParseModifyObject_UV( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV: not enough parameters" );
		return false;
	}

	if( tokens[0] == "spherical" )
	{
		return ParseModifyObject_UV_Spherical( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "box" )
	{
		return ParseModifyObject_UV_Box( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "cylindrical" )
	{
		return ParseModifyObject_UV_Cylindrical( &tokens[1], num_tokens-1, pJob );
	}
	else
	{
		GlobalLog()->PrintEx( eLog_Warning, "AsciiCommandParser::ParseModifyObject_UV: Unknown mapping type: %s", tokens[0].c_str() );
		return false;
	}
}

bool ParseModifyObject( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject: no object specifed or no type to modify" );
		return false;
	}

	// Find out what about the object we are changing
	if( tokens[0] == "error" )
	{
		if( num_tokens < 3 )
		{
			GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModifyObject: no error level specifed!" );
			return false;
		}
		
		const double err = tokens[2].toDouble();

		return pJob.SetObjectIntersectionError( tokens[1].c_str(), err );
	}
	else if( tokens[0] == "uv" )
	{
		// The user is specifying an over-riding UV generator
		return ParseModifyObject_UV( &tokens[1], num_tokens-1, pJob );
	}

	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseModifyObject: Unknown modification target: %s", tokens[1].c_str() );
	return false;
}

bool AsciiCommandParser::ParseModify( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	// We are to modify some existing item
	if( num_tokens < 1 )
	{
		// But no element type was specified!
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseModify: No element type to modify" );
		return false;
	}

	// Otherwise just do strcmps to find out what to modify
	if( tokens[0] == "object" ) {
		return ParseModifyObject( &tokens[1], num_tokens-1, pJob );
	}
	
	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseModify: Unknown element type: %s", tokens[0].c_str() );
	return false;
}




bool AsciiCommandParser::ParseRemovePainter( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemovePainter: No painter to remove" );
		return false;
	}

	return pJob.RemovePainter( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveMaterial( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveMaterial: No Material to remove" );
		return false;
	}

	return pJob.RemoveMaterial( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveGeometry( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveGeometry: No geometry to remove" );
		return false;
	}

	return pJob.RemoveGeometry( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveObject( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveObject: No object to remove" );
		return false;
	}

	return pJob.RemoveObject( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveLight( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveLight: No light to remove" );
		return false;
	}

	return pJob.RemoveLight( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveModifier( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 )
	{
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemoveModifier: No modifier to remove" );
		return false;
	}

	return pJob.RemoveModifier( tokens[0].c_str() );
}

bool AsciiCommandParser::ParseRemoveRasterizerOutputs( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	return pJob.RemoveRasterizerOutputs();
}

bool AsciiCommandParser::ParseRemove( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	// We are to add some element into the engine
	if( num_tokens < 1 )
	{
		// But no object was specified!
		GlobalLog()->Print( eLog_Warning, "AsciiCommandParser::ParseRemove: No element to remove" );
		return false;
	}

	// Otherwise just do strcmps to find out what we add
	if( tokens[0] == "painter" )
	{
		return ParseRemovePainter( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "material" )
	{
		return ParseRemoveMaterial( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "geometry" )
	{
		return ParseRemoveGeometry( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "object" )
	{
		return ParseRemoveObject( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "light" )
	{
		return ParseRemoveLight( &tokens[1], num_tokens-1, pJob );
	}
	else if( tokens[0] == "modifier" )
	{
		return ParseRemoveModifier( &tokens[1], num_tokens-1, pJob );
	} 
	else if( tokens[0] == "rasterizeroutputs" )
	{
		return ParseRemoveRasterizerOutputs( &tokens[1], num_tokens-1, pJob );
	} 
	else if( tokens[0] == "all" )
	{
		return pJob.ClearAll();
	}
	
	GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseRemove: Unknown element type: %s", tokens[0].c_str() );
	return false;
}

bool AsciiCommandParser::ParsePredictRasterizationTime( String*, unsigned int, IJob& pJob, AsciiCommandParser* )
{
	unsigned int		predicted = 0;
	bool ret = pJob.PredictRasterizationTime( 4096, &predicted, 0 );

	if( ret ) {
		unsigned int		duration = predicted;
		unsigned int		days = duration/1000/60/60/24;
		duration -= days*1000*60*60*24;
		unsigned int		hours = duration/1000/60/60;
		duration -= hours*1000*60*60;
		unsigned int		mins = duration/1000/60;
		duration -= mins*1000*60;
		unsigned int		secs = duration/1000;
		unsigned int		ms = duration % 1000;

		char buf[1024] = {0};
		strcat( buf, "Predicted Rasterization Time: " );
		char daybuf[32] = {0};
		sprintf( daybuf, "%d days ", days );
		char hourbuf[32] = {0};
		sprintf( hourbuf, "%d hours ", hours );
		char minbuf[32] = {0};
		sprintf( minbuf, "%d minutes ", mins );
		char secbuf[32] = {0};
		sprintf( secbuf, "%d seconds ", secs );
		char msbuf[32] = {0};
		sprintf( msbuf, "%d ms", ms );

		if( days ) {
			strcat( buf, daybuf );
		}

		if( hours ) {
			strcat( buf, hourbuf );
		}

		if( mins ) {
			strcat( buf, minbuf );
		}

		if( secs ) {
			strcat( buf, secbuf );
		}

		if( ms ) {
			strcat( buf, msbuf );
		}

		strcat( buf, "\n" );

		GlobalLog()->Print( eLog_Benign, buf );
	}

	return ret;
}

bool AsciiCommandParser::ParseRasterize( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	Timer t;
	bool bRet = true;
	
	StdOutProgress	progress( "Rasterizing Scene: " );	
	pJob.SetProgress( &progress );
	if( num_tokens >= 4 )
	{
		// Read the region to rasterize
		const unsigned int left = tokens[0].toUInt();
		const unsigned int right = tokens[1].toUInt();
		const unsigned int top = tokens[2].toUInt();
		const unsigned int bottom = tokens[3].toUInt();

		t.start();
		bRet = pJob.RasterizeRegion(left, top, right, bottom);
		t.stop();

	} else {

		t.start();
		bRet = pJob.Rasterize();
		t.stop();
	}

	pJob.SetProgress( 0 );

	unsigned int		duration = t.getInterval();
	unsigned int		days = duration/1000/60/60/24;
	duration -= days*1000*60*60*24;
	unsigned int		hours = duration/1000/60/60;
	duration -= hours*1000*60*60;
	unsigned int		mins = duration/1000/60;
	duration -= mins*1000*60;
	unsigned int		secs = duration/1000;
	unsigned int		ms = duration % 1000;

	char buf[1024] = {0};
	strcat( buf, "Total Rasterization Time: " );
	char daybuf[32] = {0};
	sprintf( daybuf, "%d days ", days );
	char hourbuf[32] = {0};
	sprintf( hourbuf, "%d hours ", hours );
	char minbuf[32] = {0};
	sprintf( minbuf, "%d minutes ", mins );
	char secbuf[32] = {0};
	sprintf( secbuf, "%d seconds ", secs );
	char msbuf[32] = {0};
	sprintf( msbuf, "%d ms", ms );

	if( days ) {
		strcat( buf, daybuf );
	}

	if( hours ) {
		strcat( buf, hourbuf );
	}

	if( mins ) {
		strcat( buf, minbuf );
	}

	if( secs ) {
		strcat( buf, secbuf );
	}

	if( ms ) {
		strcat( buf, msbuf );
	}

	strcat( buf, "\n" );

	GlobalLog()->Print( eLog_Benign, buf );

	return bRet;
}

bool AsciiCommandParser::ParseRasterizeAnimation( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	double time_start=0, time_end=1.0;
	unsigned int frames=30;

	if( num_tokens >= 3 ) {
		time_start = tokens[0].toDouble();
		time_end = tokens[1].toDouble();
		frames = tokens[2].toUInt();
	}

	const bool do_fields = num_tokens>=4?(tokens[3].toBoolean()):false;
	const bool invert_fields = num_tokens>=5?(tokens[4].toBoolean()):false;

	Timer t;
	bool bRet = true;

	StdOutProgress	progress( "Rasterizing Scene: " );	
	pJob.SetProgress( &progress );
	
	t.start();
	if( num_tokens >= 3 ) {
		bRet = pJob.RasterizeAnimation(time_start, time_end, frames, do_fields, invert_fields );
	} else {
		if( num_tokens >= 1 ) {
			const unsigned int specific_frame = tokens[0].toUInt();
			bRet = pJob.RasterizeAnimationUsingOptions( specific_frame );
		} else {
			bRet = pJob.RasterizeAnimationUsingOptions( );
		}
	}
	t.stop();

	pJob.SetProgress( 0 );

	unsigned int		duration = t.getInterval();
	unsigned int		days = duration/1000/60/60/24;
	duration -= days*1000*60*60*24;
	unsigned int		hours = duration/1000/60/60;
	duration -= hours*1000*60*60;
	unsigned int		mins = duration/1000/60;
	duration -= mins*1000*60;
	unsigned int		secs = duration/1000;
	unsigned int		ms = duration % 1000;

	char buf[1024] = {0};
	strcat( buf, "Total Rasterization Time: " );
	char daybuf[32] = {0};
	sprintf( daybuf, "%d days ", days );
	char hourbuf[32] = {0};
	sprintf( hourbuf, "%d hours ", hours );
	char minbuf[32] = {0};
	sprintf( minbuf, "%d minutes ", mins );
	char secbuf[32] = {0};
	sprintf( secbuf, "%d seconds ", secs );
	char msbuf[32] = {0};
	sprintf( msbuf, "%d ms", ms );

	if( days ) {
		strcat( buf, daybuf );
	}

	if( hours ) {
		strcat( buf, hourbuf );
	}

	if( mins ) {
		strcat( buf, minbuf );
	}

	if( secs ) {
		strcat( buf, secbuf );
	}

	if( ms ) {
		strcat( buf, msbuf );
	}

	strcat( buf, "\n" );

	GlobalLog()->Print( eLog_Benign, buf );

	return bRet;
}

bool ParseCausticPelSave( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseCausticPelSave: Invalid number of paramters, need 1" );
	}

	return pJob.SaveCausticPelPhotonmap( tokens[0].c_str() );
}

bool ParseGlobalPelSave( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseGlobalPelSave: Invalid number of paramters, need 1" );
	}

	return pJob.SaveGlobalPelPhotonmap( tokens[0].c_str() );
}

bool ParseTranslucentPelSave( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseTranslucentPelSave: Invalid number of paramters, need 1" );
	}

	return pJob.SaveTranslucentPelPhotonmap( tokens[0].c_str() );
}

bool ParseShadowSave( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseShadowSave: Invalid number of paramters, need 1" );
	}

	return pJob.SaveShadowPhotonmap( tokens[0].c_str() );
}


bool ParseCausticSpectralSave( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseCausticSpectralSave: Invalid number of paramters, need 1" );
	}

	return pJob.SaveCausticSpectralPhotonmap( tokens[0].c_str() );
}

bool ParseCausticPelLoad( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseCausticPelLoad: Invalid number of paramters, need 1" );
	}

	return pJob.LoadCausticPelPhotonmap( tokens[0].c_str() );
}

bool ParseTranslucentPelLoad( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseTranslucentPelLoad: Invalid number of paramters, need 1" );
	}

	return pJob.LoadTranslucentPelPhotonmap( tokens[0].c_str() );
}

bool ParseGlobalPelLoad( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseGlobalPelLoad: Invalid number of paramters, need 1" );
	}

	return pJob.LoadGlobalPelPhotonmap( tokens[0].c_str() );
}

bool ParseCausticSpectralLoad( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseCausticSpectralLoad: Invalid number of paramters, need 1" );
	}

	return pJob.LoadCausticSpectralPhotonmap( tokens[0].c_str() );
}

bool ParseShadowLoad( String* tokens, unsigned int num_tokens, IJob& pJob )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseShadowLoad: Invalid number of paramters, need 1" );
	}

	return pJob.LoadShadowPhotonmap( tokens[0].c_str() );
}

bool AsciiCommandParser::ParsePhotonMap( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* )
{
	// Does stuff with photon map
	if( num_tokens < 1 )
	{
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParsePhotonMap: no map type specified" );
		return false;
	}

	if( num_tokens < 2 )
	{
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParsePhotonMap: no operation specified" );
		return false;

	}

	if( tokens[0] == "caustic" )
	{
		if( tokens[1] == "save" )
		{
			return ParseCausticPelSave( &tokens[2], num_tokens-2, pJob );
		}
		else if( tokens[1] == "load" )
		{
			return ParseCausticPelLoad( &tokens[2], num_tokens-2, pJob );
		}
		else
		{
			GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParsePhotonMap: Unknown operation: %s", tokens[1].c_str() );
			return false;
		}
	}
	else if( tokens[0] == "global" )
	{
		if( tokens[1] == "save" )
		{
			return ParseGlobalPelSave( &tokens[2], num_tokens-2, pJob );
		}
		else if( tokens[1] == "load" )
		{
			return ParseGlobalPelLoad( &tokens[2], num_tokens-2, pJob );
		}
		else
		{
			GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParsePhotonMap: Unknown operation: %s", tokens[1].c_str() );
			return false;
		}
	}
	else if( tokens[0] == "translucent" )
	{
		if( tokens[1] == "save" )
		{
			return ParseTranslucentPelSave( &tokens[2], num_tokens-2, pJob );
		}
		else if( tokens[1] == "load" )
		{
			return ParseTranslucentPelLoad( &tokens[2], num_tokens-2, pJob );
		}
		else
		{
			GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParsePhotonMap: Unknown operation: %s", tokens[1].c_str() );
			return false;
		}
	}
	else if( tokens[0] == "spectral_caustic" )
	{
		if( tokens[1] == "save" )
		{
			return ParseCausticSpectralSave( &tokens[2], num_tokens-2, pJob );
		}
		else if( tokens[1] == "load" )
		{
			return ParseCausticSpectralLoad( &tokens[2], num_tokens-2, pJob );
		}
		else
		{
			GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParsePhotonMap: Unknown operation: %s", tokens[1].c_str() );
			return false;
		}
	}
	else if( tokens[0] == "shadow" )
	{
		if( tokens[1] == "save" )
		{
			return ParseShadowSave( &tokens[2], num_tokens-2, pJob );
		}
		else if( tokens[1] == "load" )
		{
			return ParseShadowLoad( &tokens[2], num_tokens-2, pJob );
		}
		else
		{
			GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParsePhotonMap: Unknown operation: %s", tokens[1].c_str() );
			return false;
		}
	}
	else
	{
		GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParsePhotonMap: Unknown photonmap type: %s", tokens[0].c_str() );
		return false;
	}
}

static String AssembleFromTokens( String* tokens, unsigned int num_tokens )
{
	String ret;
	for( unsigned int i=0; i<num_tokens; i++ ) {
		ret.concatenate( tokens[i] );
		ret.concatenate( " " );
	}

	return ret;
}

bool AsciiCommandParser::ParseMediaPath( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	if( num_tokens < 1 ) {
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: You need to do something with the media path (add|remove|clearall)" );
		return false;
	}

	if( tokens[0] == "add" )
	{
		if( num_tokens < 2 ) {
			GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: You need to specify a path to add" );
			return false;
		}

		return GlobalMediaPathLocator().AddPath( AssembleFromTokens(&tokens[1],num_tokens-1).c_str() );
	}
	else if( tokens[0] == "remove" )
	{
		if( num_tokens < 2 ) {
			GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: You need to specify a path to remove" );
			return false;
		}

		return GlobalMediaPathLocator().RemovePath( AssembleFromTokens(&tokens[1],num_tokens-1).c_str() );
	}
	else if( tokens[0] == "clearall" )
	{
		GlobalMediaPathLocator().ClearAllPaths();
		return true;
	}
	else
	{
		GlobalLog()->PrintEasyError( "AsciiCommandParser::ParseMediaPath: Note sure what you want but valid commands are (add|remove|clearall)" );
	}

	return true;
}

bool AsciiCommandParser::ParseEcho( String* tokens, unsigned int num_tokens, IJob& pJob, AsciiCommandParser* pRise )
{
	if( num_tokens < 2 ) {
		GlobalLog()->PrintEasyError( "Usage: ParseEcho (info|warning|error) message" );
		return false;
	}

	if( tokens[0] == "info" ) {
		GlobalLog()->Print( eLog_Info, AssembleFromTokens(&tokens[1],num_tokens-1).c_str() );
	} else if( tokens[0] == "warning" ) {
		GlobalLog()->Print( eLog_Warning, AssembleFromTokens(&tokens[1],num_tokens-1).c_str() );
	} else if( tokens[0] == "event" ) {
		GlobalLog()->Print( eLog_Event, AssembleFromTokens(&tokens[1],num_tokens-1).c_str() );
	} else if( tokens[0] == "error" ) {
		GlobalLog()->Print( eLog_Error, AssembleFromTokens(&tokens[1],num_tokens-1).c_str() );
	} else {
		GlobalLog()->PrintEx( eLog_Error, "AsciiCommandParser::ParseEcho:: Unknown type '%s'", tokens[0].c_str() );
		return false;
	}

	return true;
}
