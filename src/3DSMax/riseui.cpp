//***************************************************************************
// rise3dsmax - [riseui.cpp] Sample Plugin Renderer for 3D Studio MAX.
// 
// By Christer Janson - Kinetix
//
// Description:
// Implementation of the user interface dialogs
//
//***************************************************************************

#include "pch.h"
#pragma warning( disable : 4250 )		// disables silly virtual inheritance warning

#include "maxincl.h"
#include "resource.h"
#include "rise3dsmax.h"
#include "r:\personal\rise\src\Library\Interfaces/ILogPriv.h"
#include "r:\personal\rise\src\Library\Utilities\Log\Win32Console.h"

class RendRiseParamDlg : public RendParamDlg
{
public:
	RiseRenderer *rend;
	IRendParams *ir;
	HWND hRendererPanel;
	HWND hPMapPanel;
	BOOL prog;
	HFONT hFont;
	TSTR workFileName;
	RendRiseParamDlg( RiseRenderer *r, IRendParams *i, BOOL prog );
	~RendRiseParamDlg();
	void AcceptParams();
	void DeleteThis() 
	{
		delete this;
	}

	void InitParamDialog(HWND hWnd);
	void InitProgDialog(HWND hWnd);
	void ReleaseControls() {}
	BOOL WndProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam);


	//
	// Our own methods
	//
	void OnRendererSettingsFileBrowse();
	void OnRendererSettingsFileEdit();

	void OnSupplementarySettingsFileBrowse();
	void OnSupplementarySettingsFileEdit();
};

RendRiseParamDlg::~RendRiseParamDlg()
{
	DeleteObject(hFont);
	ir->DeleteRollupPage(hRendererPanel);
	ir->DeleteRollupPage(hPMapPanel);
}

BOOL RendRiseParamDlg::WndProc(
		HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
	switch (msg) {
		case WM_INITDIALOG:
			if (prog) InitProgDialog(hWnd);
			else InitParamDialog(hWnd);
			break;
		
		case WM_DESTROY:
			if (!prog) ReleaseControls();
			break;
		default:
			return FALSE;
		}
	return TRUE;
}

static INT_PTR CALLBACK RendRiseParamDlgProc(
		HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam)
{
	RendRiseParamDlg *dlg = (RendRiseParamDlg*)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	switch (msg) {
		case WM_INITDIALOG:
			dlg = (RendRiseParamDlg*)lParam;
			SetWindowLongPtr(hWnd,GWLP_USERDATA,lParam);
			break;
		case WM_LBUTTONDOWN:
		case WM_MOUSEMOVE:
		case WM_LBUTTONUP:
			dlg->ir->RollupMouseMessage(hWnd,msg,wParam,lParam);
			break;
		case WM_COMMAND:
			// One of the controls
			switch( wParam ) 
			{
			case IDC_BUTTON_RENDSETT_FILE:
				dlg->OnRendererSettingsFileBrowse();
				break;
			case IDC_BUTTON_RENDSETT_EDIT:
				dlg->OnRendererSettingsFileEdit();
				break;
			case IDC_BUTTON_SUPPLEMENTARY_FILE:
				dlg->OnSupplementarySettingsFileBrowse();
				break;
			case IDC_BUTTON_SUPPLEMENTARY_EDIT:
				dlg->OnSupplementarySettingsFileEdit();
				break;
			case IDC_BUTTON_CREATELOGWINDOW:
				{
					RISE::Implementation::Win32Console* pConsole = new RISE::Implementation::Win32Console();
					RISE::GlobalLog()->PrintNew( pConsole, __FILE__, __LINE__, "pConsole" );

					pConsole->Init( "R.I.S.E. Log", 0, 0, 800, 400, 20, 20, false );
					RISE::GlobalLogPriv()->AddPrinter( pConsole );		
					safe_release( pConsole );
				}
				break;
			};
			break;
		}	
	if (dlg) return dlg->WndProc(hWnd,msg,wParam,lParam);
	else return FALSE;
}

RendRiseParamDlg::RendRiseParamDlg(
		RiseRenderer *r,IRendParams *i,BOOL prog)
{
	hFont      = hFont = CreateFont(14,0,0,0,FW_BOLD,0,0,0,0,0,0,0, VARIABLE_PITCH | FF_SWISS, _T(""));
	rend       = r;
	ir         = i;
	this->prog = prog;
	if (prog) {
		// Dialog while the renderer is rendering
//		hPanel = ir->AddRollupPage(
//			hInstance, 
//			MAKEINTRESOURCE(IDD_RENDRISE_PROG),
//			RendRiseParamDlgProc,
//			GetString(IDS_VRENDTITLE),
//			(LPARAM)this);
	} else {
		// Dialog for configuring the renderer before running
		hRendererPanel = ir->AddRollupPage(
			hInstance, 
			MAKEINTRESOURCE(IDD_RENDRISE_PARAMS),
			RendRiseParamDlgProc,
			GetString(IDS_VRENDTITLE),
			(LPARAM)this);

		hPMapPanel = ir->AddRollupPage(
			hInstance,
			MAKEINTRESOURCE(IDD_RENDRISE_PMAPS),
			RendRiseParamDlgProc,
			GetString(IDS_PMAPTITLE),
			(LPARAM)this);
	}
}

void RendRiseParamDlg::InitParamDialog(HWND hWnd)
{
	// Set the UI values from the RiseRenderer class, which should do the saving and loading for us
	SetDlgItemText( hWnd, IDC_RENDRISE_RENDERERSETTINGSFILENAME, rend->szRenderSettingsFile );
	SetDlgItemText( hWnd, IDC_RENDRISE_SUPPLEMENTARYSETTINGSFILENAME, rend->szSupplementarySettingsFile );
}

void RendRiseParamDlg::InitProgDialog(HWND hWnd)
{
}

void RendRiseParamDlg::AcceptParams()
{
	GetDlgItemText( hRendererPanel, IDC_RENDRISE_RENDERERSETTINGSFILENAME, rend->szRenderSettingsFile, 255 );
	GetDlgItemText( hRendererPanel, IDC_RENDRISE_SUPPLEMENTARYSETTINGSFILENAME, rend->szSupplementarySettingsFile, 255 );
}

RendParamDlg * RiseRenderer::CreateParamDialog(IRendParams *ir,BOOL prog)
{
	return new RendRiseParamDlg(this,ir,prog);
}

void RendRiseParamDlg::OnRendererSettingsFileBrowse()
{
	OPENFILENAME ofn = {0};
	char szFile[260] = {0};       // buffer for file name

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hRendererPanel;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = "RISEscene files\0*.RISEscene\0";
	ofn.nFilterIndex = 0;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST;
	ofn.lpstrTitle = "Open R.I.S.E. scene file containing renderer and material settings";
	ofn.lpstrDefExt = ".RISEscene";

	if( GetOpenFileName( &ofn ) ) {
		SetDlgItemText( hRendererPanel, IDC_RENDRISE_RENDERERSETTINGSFILENAME, ofn.lpstrFile );
	}
}

void RendRiseParamDlg::OnRendererSettingsFileEdit()
{
	char szFile[256] = {0};
	GetDlgItemText( hRendererPanel, IDC_RENDRISE_RENDERERSETTINGSFILENAME, szFile, 255 );
	
	char command[512] = {0};
	strcpy( command, "notepad.exe " );
	strcat( command, szFile );

	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	ZeroMemory( &si, sizeof(si) );
	si.cb = sizeof(si);
	ZeroMemory( &pi, sizeof(pi) );

	CreateProcess( 0, command, 0, 0, FALSE, 0, 0, 0, &si, &pi );

	// Close process and thread handles. 
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
}

void RendRiseParamDlg::OnSupplementarySettingsFileBrowse()
{
	OPENFILENAME ofn = {0};
	char szFile[260] = {0};       // buffer for file name

	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hRendererPanel;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile);
	ofn.lpstrFilter = "RISEscene files\0*.RISEscene\0";
	ofn.nFilterIndex = 0;
	ofn.lpstrFileTitle = NULL;
	ofn.nMaxFileTitle = 0;
	ofn.lpstrInitialDir = NULL;
	ofn.Flags = OFN_PATHMUSTEXIST;
	ofn.lpstrTitle = "Open R.I.S.E. scene file containing supplementary settings";
	ofn.lpstrDefExt = ".RISEscene";

	if( GetOpenFileName( &ofn ) ) {
		SetDlgItemText( hRendererPanel, IDC_RENDRISE_SUPPLEMENTARYSETTINGSFILENAME, ofn.lpstrFile );
	}
}

void RendRiseParamDlg::OnSupplementarySettingsFileEdit()
{
	char szFile[256] = {0};
	GetDlgItemText( hRendererPanel, IDC_RENDRISE_SUPPLEMENTARYSETTINGSFILENAME, szFile, 255 );
	
	char command[512] = {0};
	strcpy( command, "notepad.exe " );
	strcat( command, szFile );

	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	ZeroMemory( &si, sizeof(si) );
	si.cb = sizeof(si);
	ZeroMemory( &pi, sizeof(pi) );

	CreateProcess( 0, command, 0, 0, FALSE, 0, 0, 0, &si, &pi );

	// Close process and thread handles. 
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
}

