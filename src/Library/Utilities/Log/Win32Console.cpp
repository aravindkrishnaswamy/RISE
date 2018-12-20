//////////////////////////////////////////////////////////////////////
//
//  Win32Console.cpp - Implements the win32 console
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 20, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "Win32Console.h"
#include "../../Version.h"
#include <richedit.h>

#define CONSOLE_DISPLAYID		0x00000010
#define CONSOLE_COMMANDID		0x00000020

using namespace RISE;
using namespace RISE::Implementation;

Win32Console::Win32Console() : 
  eTypes( eLog_All ),
  bCurrentlyExecuting( false )
{
	m_hWndMain = 0;
	Shutdown();
}

Win32Console::Win32Console( const LOG_ENUM& eTypes_ ) :
  eTypes( eTypes_ ),
  bCurrentlyExecuting( false )
{
	m_hWndMain = 0;
	Shutdown();
}

Win32Console::~Win32Console()
{
	Shutdown();
}

bool Win32Console::Shutdown()
{
	// Destory the window and reset the variables...
	if( m_hWndMain ) {
		DestroyWindow( m_hWndMain );
	}

	m_hWndMain = NULL;
	m_hWndCommand = NULL;
	m_hWndDisplay = NULL;
	m_hRichEditLib = NULL;

	m_dwConsoleWidth = 0;
	m_dwConsoleHeight = 0;

	m_dwSelStart = 0;
	m_dwSelEnd = 0;

	ZeroMemory( &m_ConsoleLines[0], sizeof( TCONSOLELINES ) );

	m_iLogEndLine = 0;
	m_cLogLines = 1;

	ZeroMemory( m_rgszCommandHistory, sizeof( m_rgszCommandHistory ) );
	m_cCommandHistoryMax=0;
	m_iCommandHistory=0;

	return true;
}

bool Win32Console::Init( const char * szWindowTitle, HINSTANCE hInstance, HWND hwndParent, DWORD dwConsoleWidth, DWORD dwConsoleHeight, DWORD x, DWORD y, bool bCreateCommandWindow )
{
	WNDCLASS wnd;
	CHARFORMAT cfCharFormat;

	m_dwConsoleWidth = dwConsoleWidth;
	m_dwConsoleHeight = dwConsoleHeight;

	if( !hInstance ) {
		hInstance = GetModuleHandle( NULL );
	}

	m_hRichEditLib = LoadLibrary("RICHED32.DLL");
	if( !m_hRichEditLib ) {
		return false;
	}

	wnd.style = CS_HREDRAW|CS_VREDRAW;
	wnd.lpfnWndProc = ConsoleProc;
	wnd.cbWndExtra = 0;
	wnd.cbClsExtra = 0;
	wnd.hInstance = hInstance;
	wnd.hIcon = NULL;
	wnd.hCursor = LoadCursor(NULL,IDC_ARROW);
	wnd.hbrBackground = (HBRUSH)GetStockObject(LTGRAY_BRUSH);
	wnd.lpszMenuName = NULL;
	wnd.lpszClassName = szWindowTitle;
	RegisterClass( &wnd );

	m_hWndMain = CreateWindowEx( 0,// WS_EX_TOPMOST,
						szWindowTitle,
						szWindowTitle,
						WS_OVERLAPPEDWINDOW,
						x,
						y,
						m_dwConsoleWidth,
						m_dwConsoleHeight,
						hwndParent,
						NULL,
						hInstance,
						this );

	if( !m_hWndMain ) {
		return false;
	}

	m_hWndDisplay = CreateWindowEx( WS_EX_CLIENTEDGE,
						"RichEdit",
						"",
						WS_VISIBLE|WS_CHILD|ES_MULTILINE|ES_READONLY|ES_AUTOHSCROLL|ES_AUTOVSCROLL|WS_VSCROLL|WS_HSCROLL,
						0,
						0,
						m_dwConsoleWidth,
						m_dwConsoleHeight,
						m_hWndMain,
						(HMENU)CONSOLE_DISPLAYID,
						hInstance,
						0 );

	if( !m_hWndDisplay ) {
		return false;
	}

	SendMessage( m_hWndDisplay, WM_SETFONT, (WPARAM)GetStockObject(SYSTEM_FIXED_FONT), MAKELPARAM( false,0 ) );
	SendMessage( m_hWndDisplay, EM_SETBKGNDCOLOR, (LPARAM)FALSE, (WPARAM)0x00000000 );

	if( bCreateCommandWindow ) {
		m_hWndCommand = CreateWindowEx( WS_EX_CLIENTEDGE,
								"RichEdit",
								"",
								WS_VISIBLE|WS_CHILD|ES_AUTOHSCROLL,
								0,
								0,
								m_dwConsoleWidth,
								m_dwConsoleHeight,
								m_hWndMain,
								(HMENU)CONSOLE_COMMANDID,
								hInstance,
								0 );

		if( !m_hWndCommand ) {
			return false;
		}

		SendMessage( m_hWndCommand, WM_SETFONT, (WPARAM)GetStockObject(SYSTEM_FIXED_FONT), MAKELPARAM(false,0) );
		SendMessage( m_hWndCommand, EM_SETEVENTMASK, 0, (LPARAM)(ENM_KEYEVENTS|ENM_SELCHANGE) );
		SendMessage( m_hWndCommand, EM_SETBKGNDCOLOR, (LPARAM)FALSE, (WPARAM)0x00000000 );

		ZeroMemory( &cfCharFormat, sizeof(CHARFORMAT) );
		cfCharFormat.cbSize = sizeof(CHARFORMAT);
		cfCharFormat.dwMask = CFM_COLOR;
		cfCharFormat.crTextColor = 0x0044EE44;
		SendMessage( m_hWndCommand, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cfCharFormat );
		SendMessage( m_hWndCommand, EM_SETLIMITTEXT, MAXCOMMANDCHAR, 0 );
	}

	ShowWindow( m_hWndMain,SW_SHOWNORMAL );
	UpdateWindow( m_hWndMain );

	return true;
}

LRESULT WINAPI Win32Console::ConsoleProc( HWND hWndMain, UINT Msg, WPARAM wParam, LPARAM lParam )
{
	static Win32Console *pConsole = NULL;
	char szBuffer[MAXCOMMANDCHAR];
	char*	szCommand;
	int cLength;
	HWND hWnd;

	switch( Msg ) {
		case WM_CREATE:
			pConsole = (Win32Console*)(((CREATESTRUCT*)lParam)->lpCreateParams);
			break;
		case WM_SIZE:
			hWnd = GetDlgItem( hWndMain, CONSOLE_DISPLAYID );
			MoveWindow( hWnd, 0, 0, LOWORD(lParam), HIWORD(lParam)-24, false );

			hWnd = GetDlgItem( hWndMain, CONSOLE_COMMANDID );
			MoveWindow( hWnd, 0, HIWORD(lParam)-20, LOWORD(lParam), 20, false );
			return 0;
		case WM_KILLFOCUS:
		case WM_SETFOCUS:
			if( (GetActiveWindow()==hWndMain) && (GetFocus()==hWndMain) ) {
				SetFocus( GetDlgItem(hWndMain,CONSOLE_COMMANDID) );
			}
			return 0;
		case WM_NOTIFY:
			if( ((MSGFILTER*)lParam)->msg==WM_KEYDOWN )
			{
				hWnd = GetDlgItem( hWndMain, CONSOLE_COMMANDID );
				switch( ((MSGFILTER*)lParam)->wParam )
				{
					case VK_TAB:
						{
							//
							// !@ EXPERIMENTAL!
							//
/*
							// Tab was pressed, lets try to do some 
							// fancy tricks and attempt some name completion...
							*(WORD*)szBuffer = (WORD)(MAXCOMMANDCHAR-1);
							SendMessage( hWnd, EM_GETLINE, 0, (LPARAM)szBuffer );
							// We have to get the line length manually because
							// depending on EM_GETLINE does NOT work!
							cLength = SendMessage( hWnd, EM_LINELENGTH, 0, 0 );

							if( cLength < 2 )
								break;

							//	szBuffer[cLength-1] = '\0';
							szBuffer[cLength] = '\0';

							// Ok now that we have the buffer, lets look at the word
							// the user is typing... and check to see if its something
							// we can lend a hand with
							SendMessage( hWnd, WM_SETTEXT, 0, (LPARAM)"Foo" );
*/
						}
						return 0;
					case VK_RETURN:
						{
							*(WORD*)szBuffer = (WORD)(MAXCOMMANDCHAR-1);
							SendMessage( hWnd, EM_GETLINE, 0, (LPARAM)szBuffer );
							// We have to get the line length manually because
							// depending on EM_GETLINE does NOT work!
							cLength = SendMessage( hWnd, EM_LINELENGTH, 0, 0 );

							if( cLength < 2 )
								break;

						//	szBuffer[cLength-1] = '\0';
							szBuffer[cLength] = '\0';
							SendMessage( hWnd, WM_SETTEXT, 0, (LPARAM)"" );

							pConsole->AddCommandHistory(szBuffer);
							pConsole->PrintCommand( szBuffer );
							pConsole->ExecuteCommand( szBuffer );
						}
						return 0;
					case VK_UP:
						szCommand = pConsole->GetPrevCommand();
						if( szCommand != NULL )
						{ 
							SendMessage( hWnd, WM_SETTEXT, 0, (LPARAM)szCommand );
						}
						SendMessage( hWnd, EM_SETSEL, (WPARAM)-2, -1 );
						return 0;
						break;
					case VK_DOWN:
						szCommand = pConsole->GetNextCommand();
						if( szCommand!=NULL )
						{
							SendMessage( hWnd, WM_SETTEXT, 0, (LPARAM)szCommand );
						}
						SendMessage( hWnd, EM_SETSEL, (WPARAM)-2, -1);
						return 0;
						break;
					case VK_ESCAPE:
						SendMessage( hWnd, WM_SETTEXT, 0, (LPARAM)"" );
						return 0;
				}
			}
			else if( ((MSGFILTER*)lParam)->nmhdr.code == EN_SELCHANGE )
			{
				hWnd = GetDlgItem( hWndMain,CONSOLE_COMMANDID );
				SendMessage( hWnd, WM_GETTEXT, (WPARAM)MAXCOMMANDCHAR, (LPARAM)pConsole->GetConsoleLine(*(pConsole->GetLognEndLine()))->szText );
				pConsole->GetConsoleLine( pConsole->m_iLogEndLine )->eMessageType = eLog_Info;
				SendMessage( hWnd, EM_GETSEL, (LPARAM)(pConsole->GetSelStart()), (WPARAM)(pConsole->GetSelEnd()) );
				return 0;
			}
			break;
		case WM_CLOSE:
			pConsole->Shutdown();
			break;

	}

	return DefWindowProc( hWndMain,Msg,wParam,lParam );
}

void Win32Console::AddCommandHistory( char *szBuffer )
{
	if( _stricmp(szBuffer, m_rgszCommandHistory[m_cCommandHistoryMax-1])!=0 ) {
		if( m_cCommandHistoryMax==MAXCOMMANDHISTORY ) {
			for( int i=0; i<m_cCommandHistoryMax-1; i++ ) {
				strcpy( m_rgszCommandHistory[i],m_rgszCommandHistory[i+1] );
			}
			strcpy( m_rgszCommandHistory[m_cCommandHistoryMax-1],szBuffer );
		} else {
			strcpy( m_rgszCommandHistory[m_cCommandHistoryMax],szBuffer );
			m_cCommandHistoryMax++;
		}
	}

	m_iCommandHistory = m_cCommandHistoryMax;
}

char* Win32Console::GetPrevCommand()
{
	m_iCommandHistory--;
	if( m_iCommandHistory < 0 ){
		m_iCommandHistory = 0;
		return NULL;
	}

	return m_rgszCommandHistory[m_iCommandHistory];
}

char* Win32Console::GetNextCommand()
{
	m_iCommandHistory++;
	if( m_iCommandHistory > m_cCommandHistoryMax-1 ) {
		m_iCommandHistory = m_cCommandHistoryMax-1;
		return NULL;
	}

	return m_rgszCommandHistory[m_iCommandHistory];
}

void Win32Console::ClearConsole( )
{
	SendMessage( m_hWndDisplay, WM_SETTEXT, 0, 0 );
}

void Win32Console::Print( const LogEvent& event )
{
	const DWORD dwLogColors[5]={	0x006F5A6F,				
									0x00FF9B9B,				
									0x005555FF,
									0x005555FF,
									0x0044EE44 };			

	char szOutput[4096];
	CHARFORMAT CharFormat;

	if( !m_hWndMain ) {
		return;
	}

	if( eTypes & event.eType )
	{
		ZeroMemory( &CharFormat, sizeof(CharFormat) );
		CharFormat.cbSize = sizeof(CharFormat);
		CharFormat.dwMask = CFM_COLOR;

		switch( event.eType )
		{
		case eLog_Benign:
		case eLog_Event:
		case eLog_Info:
			strcpy( szOutput, "" );
			CharFormat.crTextColor = dwLogColors[0];
			break;
		case eLog_Warning:
			strcpy( szOutput, "[WARNING]: " );
			CharFormat.crTextColor = dwLogColors[1];
			break;
		case eLog_Error:
			strcpy( szOutput, "[ERROR]: " );
			CharFormat.crTextColor = dwLogColors[2];
			break;
		case eLog_Fatal:
			strcpy( szOutput, "[FATAL]: " );
			CharFormat.crTextColor = dwLogColors[3];
			break;
		};

		strcat( szOutput, event.szMessage );
		strcat( szOutput, "\n" );

		SendMessage( m_hWndDisplay, EM_SETSEL, (WPARAM)-2, -1 );
		SendMessage( m_hWndDisplay, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&CharFormat );
		SendMessage( m_hWndDisplay, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)szOutput );
		SendMessage( m_hWndDisplay, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, -1), 0 );

		// Add line to linelog array
		strcpy( m_ConsoleLines[m_iLogEndLine].szText, szOutput);
		m_iLogEndLine++;

		if( m_iLogEndLine >= MAXCONSOLELINES ) {
			m_iLogEndLine = 0;
		}

		if( m_cLogLines < MAXCONSOLELINES-1 ) {
			m_cLogLines++;
		}

		m_ConsoleLines[m_iLogEndLine].eMessageType = eLog_Info;

		// To ensure the message gets printed, process all messages in the queue
		MSG msg;
		while( PeekMessage( &msg, m_hWndDisplay, 0, 0, PM_NOREMOVE ) ) {
			GetMessage( &msg, m_hWndDisplay, 0, 0 );
			TranslateMessage( &msg ); 
			DispatchMessage( &msg );
		}
	}
}

void Win32Console::PrintCommand( const char * szCommand )
{
	if( !m_hWndMain ) {
		return;
	}

	char szOutput[4096];
	CHARFORMAT CharFormat;
	
	strcpy( szOutput, "> " );
	strcat( szOutput, szCommand );
	strcat( szOutput, "\n" );

	ZeroMemory( &CharFormat, sizeof(CharFormat) );
	CharFormat.cbSize = sizeof(CharFormat);
	CharFormat.dwMask = CFM_COLOR;
	CharFormat.crTextColor = 0x0044EE44;

	SendMessage( m_hWndDisplay, EM_SETSEL, (WPARAM)-2, -1 );
	SendMessage( m_hWndDisplay, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&CharFormat );
	SendMessage( m_hWndDisplay, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)szOutput );
	SendMessage( m_hWndDisplay, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, -1), 0 );

	// Add line to linelog array
	strcpy( m_ConsoleLines[m_iLogEndLine].szText, szOutput);
	m_iLogEndLine++;

	if( m_iLogEndLine >= MAXCONSOLELINES ) {
		m_iLogEndLine = 0;
	}

	if( m_cLogLines < MAXCONSOLELINES-1 ) {
		m_cLogLines++;
	}

	m_ConsoleLines[m_iLogEndLine].eMessageType = eLog_Info;

	// To ensure the message gets printed, process all messages in the queue
	MSG msg;
	while( PeekMessage( &msg, m_hWndDisplay, 0, 0, PM_NOREMOVE ) ) {
		GetMessage( &msg, m_hWndDisplay, 0, 0 );
		TranslateMessage( &msg ); 
		DispatchMessage( &msg );
	}
}

void* Win32Console::Win32Console_ExecuteCommandProc( void* pData )
{
//	Win32Console*	pMe = (Win32Console*)pData;

//	pMe->SetCurrentlyExecuting( true );
//	GlobalRISE()->ParseLine( pMe->GetExecutionBuffer() );
//	pMe->SetCurrentlyExecuting( false );

	return 0;
}

void Win32Console::ExecuteCommand( const char * szCommand )
{
	// Peek at the message and see if its something we can handle, if it is, 
	// then we should
	{
		if (strcmp( szCommand, "parse_enable" ) == 0 ) {
			// enable parsing
			bCurrentlyExecuting = false;
			return;
		} else if (strcmp( szCommand, "parse_disable" ) == 0 ) {
			bCurrentlyExecuting = true;
			return;
		} else if (strcmp( szCommand, "clear" ) == 0 ) {
			ClearConsole();
			return;
		} else if (strcmp( szCommand, "pause" ) == 0 ) {
			Threading::riseSuspendThread( tid );
			return;
		} else if( strcmp( szCommand, "resume" ) == 0 ) {
			Threading::riseResumeThread( tid );
			return;
		}
		
	}

	// Executes a command in a seperate thread so that the main window
	// does get blocked!
	if( bCurrentlyExecuting )
	{
		LogEvent	le;
		strcpy( le.szMessage, "RISE is currently executing a command, wait until it finishes" );
		le.eType = eLog_Error;
		Print( le );
	}
	else
	{
		strcpy( szExecutionBuffer, szCommand );
		Threading::riseCreateThread( Win32Console_ExecuteCommandProc, (void*)this, 0, 0, &tid );
	}
}
	

void Win32Console::Flush()
{
}

