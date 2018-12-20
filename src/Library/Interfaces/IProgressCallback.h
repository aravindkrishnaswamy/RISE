//////////////////////////////////////////////////////////////////////
//
//  IProgressCallback.h - Defines the interface that all classes
//    who want to be notified about progress must implement
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 25, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_PROGRESS_CALLBACK_
#define I_PROGRESS_CALLBACK_

namespace RISE
{
	class IProgressCallback
	{
	public:
		virtual ~IProgressCallback(){}

		// Return TRUE to continue, return FALSE to abort whatever operation 
		// we are getting progress for
		virtual bool Progress( const double progress, const double total ) = 0;
		virtual void SetTitle( const char* title ) = 0;
	};
}

#endif

