#ifndef STANDALONE_TEST_REPORTING_H
#define STANDALONE_TEST_REPORTING_H

#include <iostream>
#include <string>

namespace StandaloneTestReporting
{
	inline void PrintSection( const char* label )
	{
		std::cout << std::endl << "-- " << label << " --" << std::endl;
	}

	inline void PrintPassed( const std::string& detail = std::string() )
	{
		if( detail.empty() ) {
			std::cout << "    PASSED" << std::endl;
		} else {
			std::cout << "    PASSED (" << detail << ")" << std::endl;
		}
	}

	inline void PrintFailed( const std::string& detail )
	{
		std::cout << "    FAIL: " << detail << std::endl;
	}

	inline void PrintWarning( const std::string& detail )
	{
		std::cout << "    WARN: " << detail << std::endl;
	}
}

#endif
