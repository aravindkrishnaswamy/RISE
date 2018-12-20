//////////////////////////////////////////////////////////////////////
//
//  CPU.h - CPU utilities
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 20, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CPU_H_
#define CPU_H_

namespace RISE
{
	enum CPU_COUNT_ENUM
	{
		HT_NOT_CAPABLE,
		HT_ENABLED,
		HT_DISABLED,
		HT_SUPPORTED_NOT_ENABLED,
		HT_CANNOT_DETECT
	};

	// Returns the number of logical and physical processors
	CPU_COUNT_ENUM GetCPUCount( int& logical, int& physical );
}

#endif
