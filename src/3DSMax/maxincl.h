//***************************************************************************
// rise3dsmax - [maxincl.h] Sample Plugin Renderer for 3D Studio MAX.
// 
// By Christer Janson - Kinetix
//
// May     11, 1996	CCJ Initial coding
// October 28, 1996	CCJ Texture mapping, raycasting etc.
// December    1996	CCJ General cleanup
//
// Description:
// Here we include the Max SDK headers for use with precompiled headers
//
//***************************************************************************

#pragma warning( disable : 4002 )		// disables warning about too many actual parameters for macro FN_0

#include "Max.h"
#include "bmmlib.h"
#include "meshadj.h"
#include "modstack.h"
#include "stdmat.h"
#include "templt.h"
#include "render.h"
#include <float.h>		// Include these guys, otherwise sqrt() doesn't work!
#include <math.h>
#include <notify.h>

#pragma warning( default : 4002 )

// Prototype for a utility function used for string table resources
TCHAR *GetString(int id);
