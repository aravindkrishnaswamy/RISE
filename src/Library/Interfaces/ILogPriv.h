//////////////////////////////////////////////////////////////////////
//
//  ILogPriv.h - Priviledged interface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ILOGPRIV_
#define ILOGPRIV_

#include "ILog.h"
#include "ILogPrinter.h"

namespace RISE
{
	class ILogPriv : public /*virtual*/ ILog /* Commented out to account for a GCC 3.x bug with virtual inheritance and var args */
	{
	protected:
		virtual ~ILogPriv(){};
		ILogPriv(){};

	public:
		//! Adds a printer the logger is supposed to write to
		virtual void AddPrinter( 
			ILogPrinter* pPrinter									///< [in] The printer to add
			) = 0;

		//! Removes all printers, the logger won't write anything out after this
		virtual void RemoveAllPrinters( ) = 0;
	};

	//! Returns the priviledged interface to the global log
	extern ILogPriv* GlobalLogPriv();

	//! Sets the filename of the global log
	extern void SetGlobalLogFileName(
		const char * name				///< [in] Name of file to write to
		);
}

#endif

