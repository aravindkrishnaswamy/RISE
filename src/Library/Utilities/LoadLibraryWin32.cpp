//////////////////////////////////////////////////////////////////////
//
//  LoadLibraryWin32.cpp - Win32 implementation of LoadLibrary.h
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 28, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"

#include "LoadLibrary.h"
#ifdef WIN32

#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
#define _WIN32_WINNT 0x0400		// require NT4 or greater for TryEnterCriticalSection
#include <windows.h>

using namespace RISE;

bool Libraries::riseLoadLibrary( 
	RISEHMODULE& hmodule,					///< [out] Handle of module loaded
	const char * szFilename					///< [in] Name of the file to load
	)
{
	if( !szFilename ) {
		return false;
	}

	hmodule = (RISEHMODULE)LoadLibrary( szFilename );

	return true;
}

//! Frees the library specified
/// \return true if successful, false otherwise
bool Libraries::riseFreeLibrary(
	RISEHMODULE& hmodule					///< [out] Handle of module to free
	)
{
	return !!FreeLibrary( (HMODULE)hmodule );
}

//! Gets the address of the exported function
/// \return pointer to the exported function, 0 if not found
RISEFPROC Libraries::riseGetProcAddress(
	const RISEHMODULE& hmodule,				///< [in] Handle to the module
	const char * szFunctionname				///< [in] Name of the function to load
	)
{
	if( !szFunctionname ) {
		return 0;
	}

	return GetProcAddress( (HMODULE)hmodule, szFunctionname );
}

#endif
