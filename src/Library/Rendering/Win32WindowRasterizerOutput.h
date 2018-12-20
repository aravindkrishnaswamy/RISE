//////////////////////////////////////////////////////////////////////
//
//  Win32WindowRasterizerOutput.h - A rasterizer output object that
//    displays the output to a window
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 24, 2003
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef WIN32_WINDOW_RASTERIZEROUTPUT_
#define WIN32_WINDOW_RASTERIZEROUTPUT_

#ifdef _WIN32

#include "../Interfaces/IRasterizerOutput.h"
#include "../Utilities/Reference.h"

#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
#define _WIN32_WINNT 0x0400		// require NT4 or greater for TryEnterCriticalSection
#include <windows.h>
#include <windowsx.h>

namespace RISE
{
	namespace Implementation
	{
		class Win32WindowRasterizerOutput : public virtual IRasterizerOutput, public virtual Reference
		{
		protected:
			virtual ~Win32WindowRasterizerOutput( );

			char				szClassName[128];	// class name is needed for windows' windows	
			char				szDisplayName[256];	// name to display on window
			HWND				hWnd;				// handle to the window
			HINSTANCE			hInstance;			// handle to this particular instance
			unsigned int		width;				// width of the window
			unsigned int		height;				// height of the window
			unsigned int		xpos;				// position of the window
			unsigned int		ypos;

			unsigned char*		pBits;				// raw bytes of the bitmap

			HANDLE				hThread;			// Handle to the message pump thread

		public:

			HBITMAP				hBitmap;
			HANDLE				hStop;				// Stop event for the message pump thread

			Win32WindowRasterizerOutput( 
				const unsigned int width_,
				const unsigned int height_,
				const unsigned int xpos_,
				const unsigned int ypos_,
				const char * szDisplayName
				);

			// IRasterizerOutput requirements

			void	OutputIntermediateImage( const IRasterImage& pImage, const Rect* pRegion );
			void	OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame );


			//
			// Specific functions for handling a Win32 window
			//
			// This function is only called by the message proc, it tells us that we've closed the window
			void	WindowClosed( );
			bool	UpdateAndProcessIfNecessary( );

			void	CreateStuff();
		};
	}
}

#endif
#endif
