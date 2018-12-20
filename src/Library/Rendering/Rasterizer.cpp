//////////////////////////////////////////////////////////////////////
//
//  Rasterizer.cpp - Implements the functions in implementation help
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Rasterizer.h"
#include "../Interfaces/IOptions.h"
#include "../Utilities/CPU.h"

using namespace RISE;
using namespace RISE::Implementation;

Rasterizer::Rasterizer() :
  pProgressFunc( 0 )
{
}

Rasterizer::~Rasterizer( )
{
	FreeRasterizerOutputs();
}

int Rasterizer::HowManyThreadsToSpawn() const
{
	int logical, physical;
	RISE::CPU_COUNT_ENUM eHTStatus = GetCPUCount( logical, physical );

	// Lets have some fun with the options file
	IOptions& options = GlobalOptions();

	const bool bHyperthreading = options.ReadBool( "support_hyperthreading", true );
	const int maxThreads = options.ReadInt( "maximum_thread_count", 0xFFFFFFF );
	const int force_number_of_threads = options.ReadInt( "force_number_of_threads", 0 );

	int totalThreads = logical*physical;

	if( !bHyperthreading ) {
		totalThreads = physical;
	}

	if( totalThreads > maxThreads ) {
		totalThreads = maxThreads;
	}

	if( force_number_of_threads ) {
		return force_number_of_threads;
	}

	return totalThreads;
}

void Rasterizer::AddRasterizerOutput( IRasterizerOutput* ro )
{
	if( ro ) {
		ro->addref();
		outs.push_back( ro );
	}
}

void Rasterizer::FreeRasterizerOutputs( )
{
	RasterizerOutputListType::iterator	i, e;
	for( i=outs.begin(), e=outs.end(); i!=e; i++ ) {
		safe_release( (*i) );
	}
	outs.clear();
}

void Rasterizer::EnumerateRasterizerOutputs( IEnumCallback<IRasterizerOutput>& pFunc ) const
{
	RasterizerOutputListType::const_iterator	i, e;
	for( i=outs.begin(), e=outs.end(); i!=e; i++ ) {
		pFunc( *(*i) );
	}	
}

void Rasterizer::SetProgressCallback( IProgressCallback* pFunc )
{
	pProgressFunc = pFunc;
}

