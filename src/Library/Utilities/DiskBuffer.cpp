//////////////////////////////////////////////////////////////////////
//
//  DiskBuffer.cpp - Implements the DiskBuffer helper class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 3, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DiskBuffer.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "../Interfaces/ILog.h"

using namespace RISE::Implementation;

DiskBuffer::DiskBuffer( ) : 
  hFile( 0 )
{
}

DiskBuffer::~DiskBuffer( )
{
}

unsigned int DiskBuffer::HowFarToEnd( ) const
{
	return Size() - getCurPos();
}

unsigned int DiskBuffer::Size( ) const
{	
	struct stat file_stats = {0};
	stat( szFileName, &file_stats );
	return file_stats.st_size;
}

unsigned int DiskBuffer::getCurPos( ) const
{
	return ftell( hFile );
}

bool DiskBuffer::EndOfBuffer( ) const
{
	return (HowFarToEnd()==0);
}

bool DiskBuffer::seek( const eSeek type, const int amount )
{
	bool bReturn = false; 

	switch( type )
	{
	case START:
		bReturn = !fseek( hFile, amount, SEEK_SET );
		break;

	case CUR:
		bReturn = !fseek( hFile, amount, SEEK_CUR );
		break;
	case END:
		bReturn = !fseek( hFile, amount, SEEK_END );
		break;
	}

	return bReturn;
}
