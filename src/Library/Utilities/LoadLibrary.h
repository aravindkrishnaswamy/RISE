//////////////////////////////////////////////////////////////////////
//
//  LoadLibrary.h - Provides a platform independent interface to 
//  dynamically loading external libraries.  This interface
//  only a set of declarations for functions, other files will have
//  implementations for their respective platforms
//
//  All our functions are prefixed with rise, so that we don't have
//  namespace collisions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 28, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef UTIL_LOADLIBRARY_
#define UTIL_LOADLIBRARY_

namespace RISE
{
	typedef unsigned int RISEHMODULE;
	typedef int (_stdcall *RISEFPROC)();

	//! Utility load library functions
	struct Libraries
	{
		//! Loads the library specified
		/// \return true if successful, false otherwise
		static bool riseLoadLibrary( 
			RISEHMODULE& hmodule,					///< [out] Handle of module loaded
			const char * szFilename					///< [in] Name of the file to load
			);

		//! Frees the library specified
		/// \return true if successful, false otherwise
		static bool riseFreeLibrary(
			RISEHMODULE& hmodule					///< [out] Handle of module to free
			);

		//! Gets the address of the exported function
		/// \return pointer to the exported function, 0 if not found
		static RISEFPROC riseGetProcAddress(
			const RISEHMODULE& hmodule,				///< [in] Handle to the module
			const char * szFunctionname				///< [in] Name of the function to load
			);

	};
}

#endif

