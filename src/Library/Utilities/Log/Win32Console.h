//////////////////////////////////////////////////////////////////////
//
//  Win32Console.h - Provides a handy console to entering and 
//  keeping track of RISE commands for the Win32 platform
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 20, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifdef WIN32

	#ifndef WIN32_CONSOLE_
	#define WIN32_CONSOLE_

	#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers
	#define _WIN32_WINNT 0x0400		// require NT4 or greater for TryEnterCriticalSection
	#include <windows.h>
	#include <windowsx.h>

	#include "../../Interfaces/ILogPrinter.h"
	#include "../../Interfaces/IJob.h"
	#include "../Threads/Threads.h"
	#include "../Reference.h"

	namespace RISE {
	namespace Implementation
	{
		static const unsigned int MAXNUMPERLINE	= 4096;
		static const unsigned int MAXCONSOLELINES =	8192;
		static const unsigned int MAXCOMMANDHISTORY = 128;
		static const unsigned int MAXCOMMANDCHAR = 4096;

		class Win32Console : public virtual ILogPrinter, public virtual Implementation::Reference
		{
		protected:
			struct TCONSOLELINES
			{
				char				szText[MAXNUMPERLINE];
				LOG_ENUM			eMessageType;
			};

			HWND			m_hWndMain;
			HWND			m_hWndCommand;
			HWND			m_hWndDisplay;
			HINSTANCE		m_hRichEditLib;			// RichEdit Library

			DWORD			m_dwConsoleWidth;
			DWORD			m_dwConsoleHeight;

			DWORD			m_dwSelStart;
			DWORD			m_dwSelEnd;

			int				m_iLogEndLine;
			int				m_cLogLines;

			TCONSOLELINES	m_ConsoleLines[MAXCONSOLELINES];

			char			m_rgszCommandHistory[MAXCOMMANDHISTORY][MAXNUMPERLINE];
			int				m_cCommandHistoryMax;
			int				m_iCommandHistory;

			LOG_ENUM		eTypes;

			bool			bCurrentlyExecuting;
			char			szExecutionBuffer[8192];

			RISETHREADID	tid;

			char* GetPrevCommand();
			char* GetNextCommand();

			virtual ~Win32Console();

		public:
			Win32Console();
			Win32Console( const LOG_ENUM& eTypes_ );

			bool Init( const char * szWindowTitle, HINSTANCE hInstance=0, HWND hwndParent=0, const DWORD m_ConsoleWidth = 800, const DWORD m_ConsoleHeight = 480, DWORD x=20, DWORD y=30, bool bCreateCommandWindow=true );
			bool Shutdown( );
			static LRESULT WINAPI ConsoleProc( HWND hWndMain, UINT Msg, WPARAM wParam, LPARAM lParam );

			inline TCONSOLELINES* GetConsoleLine( DWORD n )	{		return &m_ConsoleLines[n];	}
			inline DWORD*		 GetSelStart()				{		return &m_dwSelStart;		}
			inline DWORD*		 GetSelEnd()				{		return &m_dwSelEnd;			}
			inline int*			 GetLognEndLine()			{		return &m_iLogEndLine;		}

			inline void			 SetCurrentlyExecuting( bool b ){	bCurrentlyExecuting = b;	}
			inline char*		 GetExecutionBuffer()		{		return szExecutionBuffer;	}

			void AddCommandHistory( char *szBuffer );

			void ClearConsole( );

			void PrintCommand( const char * szCommand );
			void ExecuteCommand( const char * szCommand );

			// These functions are need to implement a log printer
			void Print( const LogEvent& event );
			void Flush();

			static void* Win32Console::Win32Console_ExecuteCommandProc( void* );
		};
	} }

	#endif
#else
	#error "This is a Win32 only file!, you might want to specify the WIN32 preprocessor directive if you are on windows."
#endif

