//////////////////////////////////////////////////////////////////////
//
//  Win32WindowRasterizerOutput.cpp - Implementation of a file rasterizer output
//  object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef _WIN32
#error "This is a Win32 file only!"
#endif

#include "pch.h"
#include "Win32WindowRasterizerOutput.h"
#include "../Interfaces/ILog.h"

#include <string.h>
#include <stdio.h>
#include <map>

using namespace RISE;
using namespace RISE::Implementation;

LRESULT WINAPI MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	static std::map<HWND,Win32WindowRasterizerOutput*>	whoami;

	PAINTSTRUCT ps;
	HDC hdcSurface = NULL;

    switch( msg )
    {
		case WM_CREATE:
			{
				Win32WindowRasterizerOutput* pMe = (Win32WindowRasterizerOutput*)(((CREATESTRUCT*)lParam)->lpCreateParams);
				whoami[hWnd] = pMe;
			}
			return 0;
        case WM_DESTROY:
			{
				Win32WindowRasterizerOutput* pMe = whoami[hWnd];
				if( pMe ) {
					whoami[hWnd]->WindowClosed();
					whoami[hWnd] = 0;
				}
			}
            return 0;

        case WM_PAINT:
			// We should refresh from an internal surface
			{
				Win32WindowRasterizerOutput* pMe = whoami[hWnd];
				if( pMe )
				{
					hdcSurface = BeginPaint( hWnd, &ps );
					if( pMe->hBitmap )
					{
						HDC hdcBitmap = CreateCompatibleDC(NULL);
						SelectObject( hdcBitmap, pMe->hBitmap );

						// Copy the bitmap image to the surface.
						BITMAP bm;
						GetObject( pMe->hBitmap, sizeof(BITMAP), &bm );
						BitBlt( hdcSurface, 0, 0, bm.bmWidth, bm.bmHeight, hdcBitmap, 0, 0, SRCCOPY );
						DeleteDC( hdcBitmap );
					}
					EndPaint( hWnd, &ps );
				}
			}
            return 0;

    }

    return DefWindowProc( hWnd, msg, wParam, lParam );
}

DWORD WINAPI WindowMessagePump( LPVOID v )
{
	Win32WindowRasterizerOutput* pObj = (Win32WindowRasterizerOutput*)v;

	pObj->CreateStuff();

	while( WaitForSingleObject( pObj->hStop, 0 ) == WAIT_TIMEOUT ) {
		WaitMessage();
		pObj->UpdateAndProcessIfNecessary();
	}

	return false;
}

Win32WindowRasterizerOutput::Win32WindowRasterizerOutput(
	const unsigned int width_,
	const unsigned int height_,
	const unsigned int xpos_,
	const unsigned int ypos_,
	const char * szDisplayName_
	) : 
	  width( width_ ),
	  height( height_ ),
	  xpos( xpos_ ),
	  ypos( ypos_ ),
	  pBits( 0 )
{
	strncpy( szDisplayName, szDisplayName_, 256 );

	// Create the bitmap for this context
	LONG lHeight = height;

	BITMAPINFO	bmi;
	ZeroMemory( &bmi, sizeof( BITMAPINFO ) );

	bmi.bmiHeader.biSize = sizeof( BITMAPINFOHEADER );
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -lHeight;
    bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = 0;
    bmi.bmiHeader.biXPelsPerMeter = 0;
    bmi.bmiHeader.biYPelsPerMeter = 0;
    bmi.bmiHeader.biClrUsed = 0;
    bmi.bmiHeader.biClrImportant = 0;
	
	hBitmap = CreateDIBSection( NULL, &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, NULL );

	for( unsigned int y=0;y<height; y++ ) {
		for( unsigned int x=0; x<width; x++ ) {
			memset( &pBits[y*width*4+x*4], 0x80, 1 );
			memset( &pBits[y*width*4+x*4+1], 0, 1 );
			memset( &pBits[y*width*4+x*4+2], 0, 1 );
		}
	}
	
	char buf[_MAX_PATH] = {0};
	sprintf( buf, "RISE_Neverset_Thread_Handle_%d_%d", GetTickCount(), rand() );
	hStop = CreateEvent( 0, FALSE, FALSE, buf );

	// We need to create a message pump so that this window can keep processing messages
	hThread = CreateThread( 0, 0, WindowMessagePump, (LPVOID)this, 0, 0 );
}

Win32WindowRasterizerOutput::~Win32WindowRasterizerOutput()
{
	if( hBitmap ) {
		DeleteObject( hBitmap );
		hBitmap = NULL;
	}

	// If the window is still around, destroy it
	if( hWnd ) {
		DestroyWindow( hWnd );
		hWnd = 0;
	}

	if( hThread ) {
		SetEvent( hStop );
		if( WaitForSingleObject( hThread, 1000 ) == WAIT_TIMEOUT ) {
			TerminateThread( hThread, 0 );
		}
		hThread = 0;
		DeleteObject( hStop );
		hStop = 0;
	}

	// Unregister the class
	if( szClassName[0] ) {
		UnregisterClass( szClassName, hInstance );
		szClassName[0] = 0;
	}

	hInstance = 0;
}

// Interface requirements functions

inline unsigned char Rescale( unsigned int n )
{
	n = n + (n / 256);
	return (n / 256);
}

void Win32WindowRasterizerOutput::OutputIntermediateImage( const IRasterImage& pImage, const Rect* pRegion )
{
	if( hWnd ) {
		// Set the appropriate bytes
		RECT rc;
		if( pRegion ) {
			rc.left = pRegion->left;
			rc.top = pRegion->top;
			rc.right = pRegion->right+1;
			rc.bottom = pRegion->bottom+1;
		} else {
			rc.left = 0;
			rc.top = 0;
			rc.right = width;
			rc.bottom = height;
		}

		static const unsigned int pelsize = 4;
		const unsigned int stride = width*pelsize;

		for( int y=rc.top; y<rc.bottom; y++ ) {
			for( int x=rc.left; x<rc.right; x++ ) {
				RGBA8 rgba = pImage.GetPEL( x, y ).Integerize<sRGBPel,unsigned char>(255.0);

				// Do a non-linear composite, it will be incorrect, but it will be fast
				if( rgba.a == 0xff ) {
					// Opaque
					// Do nothing	
				} else if( rgba.a == 0 ) {
					// Transparent, just the checker
					// Composite on checkerboard
					if( ((x/8)%2 == 0 && (y/8)%2 == 0) ||
						((x/8)%2 == 1 && (y/8)%2 == 1) ) {
						rgba.r = rgba.g = rgba.b = 108;
					} else {
						rgba.r = rgba.g = rgba.b = 124;
					}
                    
				} else {

					// Composite on checkerboard
					unsigned char val = 0;
					if( ((x/8)%2 == 0 && (y/8)%2 == 0) ||
						((x/8)%2 == 1 && (y/8)%2 == 1) ) {
						val = 108;
					} else {
						val = 124;
					}

					unsigned char ba = 255-rgba.a;

					rgba.r = Rescale( val * ba + rgba.r * rgba.a );
					rgba.g = Rescale( val * ba + rgba.g * rgba.a );
					rgba.b = Rescale( val * ba + rgba.b * rgba.a );
				}

				pBits[y*stride+(x*pelsize)] = rgba.b;
				pBits[y*stride+(x*pelsize)+1] = rgba.g;
				pBits[y*stride+(x*pelsize)+2] = rgba.r;
			}
		}

		// Force an update
		InvalidateRect( hWnd, &rc, TRUE );
	}
}

void Win32WindowRasterizerOutput::OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame )
{
	OutputIntermediateImage( pImage, pRegion );
}

//
// Win32 ui specific functions
//

void Win32WindowRasterizerOutput::WindowClosed( )
{
	if( hWnd ) {
		DestroyWindow( hWnd );
		hWnd = 0;
	}

	// Stop the message pump
	if( hThread ) {
		SetEvent( hStop );
		if( WaitForSingleObject( hThread, 1000 ) == WAIT_TIMEOUT ) {
			TerminateThread( hThread, 0 );
		}
		hThread = 0;
		DeleteObject( hStop );
		hStop = 0;
	}
}

bool Win32WindowRasterizerOutput::UpdateAndProcessIfNecessary( )
{
	// Basically check to see if there are any messages in the queue, if there
	// are then process one message
	if( !hWnd ) {
		return false;
	}

	// Someone else is responsible for maintaining our pump
	MSG		msg;

	if( PeekMessage( &msg, hWnd, 0, 0, PM_NOREMOVE ) ) {
		if( GetMessage( &msg, NULL, 0, 0 ) ) {
			TranslateMessage( &msg ); 
			DispatchMessage( &msg );
			return true;
		}
	}

	return false;
}

void Win32WindowRasterizerOutput::CreateStuff()
{
	// Before creating the hwnd, we have to register the class
	// that requires a unique name for this particular context... <sigh>

	hInstance = GetModuleHandle( NULL );

	szClassName[0] = 0;
	sprintf( szClassName, "RISE::Win32Context::%d::%d", GetTickCount(), rand() );

	WNDCLASSEX		wc = {	sizeof(WNDCLASSEX),
							CS_CLASSDC,
							MsgProc,
							0L, 0L, 
							hInstance,
							NULL, NULL,
							NULL, NULL,
							szClassName,
							NULL };

	RegisterClassEx( &wc );

	// Adjust the size of the window to account for the borders and such
	RECT rc;
	rc.top = ypos;
	rc.left = xpos;
	rc.bottom = rc.top + height;
	rc.right = rc.left + width;

	unsigned int	nFlags = WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX;

	AdjustWindowRect( &rc, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, false );

	// Now actually create the window
	hWnd = CreateWindow(	szClassName,
							szDisplayName,
							nFlags,
							rc.left, rc.top, rc.right-rc.left, rc.bottom-rc.top,
							GetDesktopWindow(),
							NULL,
							hInstance,
							this );

	if( hWnd ) {
		ShowWindow( hWnd, SW_SHOW );
		BringWindowToTop( hWnd );
		SetFocus( hWnd );
	}
	
}