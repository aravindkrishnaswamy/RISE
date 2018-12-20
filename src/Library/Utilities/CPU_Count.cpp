//////////////////////////////////////////////////////////////////////
//
//  CPU_Count.cpp - CPU counting
//
//  Author: Aravind Krishnaswamy (adapted to RISE)
//  Date of Birth: February 20, 2006
//  Tabs: 4
//  Comments: This code is borrowed from the Intel Developer site
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CPU.h"

#ifdef WIN32


namespace RISE
{
	static const unsigned int HT_BIT =             0x10000000;		// EDX[28]  Bit 28 is set if HT is supported
	static const unsigned int FAMILY_ID =          0x0F00;			// EAX[11:8] Bit 8-11 contains family processor ID.
	static const unsigned int PENTIUM4_ID =        0x0F00;
	static const unsigned int EXT_FAMILY_ID =      0x0F00000;		// EAX[23:20] Bit 20-23 contains extended family processor ID
	static const unsigned int NUM_LOGICAL_BITS =   0x00FF0000;		// EBX[23:16] Bit 16-23 in ebx contains the number of logical
																	// processors per physical processor when execute cpuid with
																	// eax set to 1

	static const unsigned int INITIAL_APIC_ID_BITS =  0xFF000000;	// EBX[31:24] Bits 24-31 (8 bits) return the 8-bit unique
																	// initial APIC ID for the processor this code is running on.
																	// Default value = 0xff if HT is not supported

	unsigned int HTSupported()
	{
		unsigned int Regedx      = 0,
			         Regeax      = 0,
			         VendorId[3] = {0, 0, 0};

		__try    // Verify cpuid instruction is supported
		{
			__asm
			{
				xor eax, eax          // call cpuid with eax = 0
        		cpuid                 // Get vendor id string
				mov VendorId, ebx
				mov VendorId + 4, edx
				mov VendorId + 8, ecx

				mov eax, 1            // call cpuid with eax = 1
				cpuid
				mov Regeax, eax      // eax contains family processor type
				mov Regedx, edx      // edx has info about the availability of hyper-Threading

			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
			return 0;                   // cpuid is unavailable
		}

		if (((Regeax & FAMILY_ID) ==  PENTIUM4_ID) || (Regeax & EXT_FAMILY_ID))
			if (VendorId[0] == 'uneG')
				if (VendorId[1] == 'Ieni')
					if (VendorId[2] == 'letn')
						return(Regedx & HT_BIT);    // Genuine Intel with hyper-Threading technology
		return 0;    // Not genuine Intel processor
	}

	unsigned int LogicalProcPerPhysicalProc()
	{
		unsigned int Regebx = 0;
		if( !HTSupported() ) {
			return 1;  // HT not supported, Logical processor = 1
		}

		__asm
		{
			mov eax, 1
			cpuid
			mov Regebx, ebx
		}

		return ((Regebx & NUM_LOGICAL_BITS) >> 16);
	}


	unsigned int GetAPIC_ID(void)
	{
		unsigned int Regebx = 0;
		if( !HTSupported() ) {
			return (unsigned char) -1;  // HT not supported, Logical processor = 1
		}

		__asm
		{
			mov eax, 1
			cpuid
			mov Regebx, ebx
		}

		return ((Regebx & INITIAL_APIC_ID_BITS) >> 24);
	}

	CPU_COUNT_ENUM GetCPUCount( int& logical, int& physical )
	{
		CPU_COUNT_ENUM status  = HT_NOT_CAPABLE;
		SYSTEM_INFO info;
		physical = 0;
		logical = 0;
		info.dwNumberOfProcessors = 0;
		GetSystemInfo (&info);

		// Number of physical processors in a non-Intel system
		// or in a 32-bit Intel system with Hyper-Threading technology disabled
		physical =  info.dwNumberOfProcessors;

		// Check for hyperthreading
		if( HTSupported() )
		{
			unsigned char HT_Enabled = 0;
			logical = LogicalProcPerPhysicalProc();
			if( logical >= 1)    // >1 Doesn't mean HT is enabled in the BIOS
			{
				// Calculate the appropriate  shifts and mask based on the
				// number of logical processors.

				unsigned char i = 1;
				unsigned char PHY_ID_MASK  = 0xFF;
				unsigned char PHY_ID_SHIFT = 0;

				while( i < logical ) {
					i <<=1;
					PHY_ID_MASK  <<= 1;
					PHY_ID_SHIFT++;
				}

				HANDLE hCurrentProcessHandle = GetCurrentProcess();
				DWORD dwProcessAffinity;
				DWORD dwSystemAffinity;
				GetProcessAffinityMask( hCurrentProcessHandle, &dwProcessAffinity, &dwSystemAffinity );

				// Check if available process affinity mask is equal to the
				// available system affinity mask
				if( dwProcessAffinity != dwSystemAffinity ) {
					status = HT_CANNOT_DETECT;
					physical = -1;
					return status;
				}

				DWORD dwAffinityMask = 1;
				while( dwAffinityMask != 0 && dwAffinityMask <= dwProcessAffinity )
				{
					// Check if this CPU is available
					if( dwAffinityMask & dwProcessAffinity )
					{
						if( SetProcessAffinityMask( hCurrentProcessHandle, dwAffinityMask ) )
						{
							unsigned char APIC_ID,LOG_ID,PHY_ID;

							Sleep(0); // Give OS time to switch CPU
							APIC_ID = GetAPIC_ID();
							LOG_ID  = APIC_ID & ~PHY_ID_MASK;
							PHY_ID  = APIC_ID >> PHY_ID_SHIFT;
							if( LOG_ID != 0 ) {
								HT_Enabled = 1;
							}
						}
					}

					dwAffinityMask = dwAffinityMask << 1;
				}

				// Reset the processor affinity
				SetProcessAffinityMask( hCurrentProcessHandle, dwProcessAffinity );
				if( logical == 1 ) {  // Normal P4 : HT is disabled in hardware
					status = HT_DISABLED;
				} else {
					if( HT_Enabled ) {
						// Total physical processors in a Hyper-Threading enabled system.
						physical /= logical;
						status = HT_ENABLED;
					} else {
						status = HT_SUPPORTED_NOT_ENABLED;
					}
				}
			}
		}
		else
		{
			// Processors do not have Hyper-Threading technology
			status = HT_NOT_CAPABLE;
			logical = 1;
		}

		return status;
	}
}

#else

namespace RISE
{
	CPU_COUNT_ENUM GetCPUCount( int& logical, int& physical )
	{
		// TODO, we need a way to detect this in other operating systems
		logical = physical = 1;

		return HT_CANNOT_DETECT;
	}
}

#endif
