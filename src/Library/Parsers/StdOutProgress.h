//////////////////////////////////////////////////////////////////////
//
//  StdOutProgress.h - Progress that spits out the standard output
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 2, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef STDOUT_PROGRESS_
#define STDOUT_PROGRESS_

#include <iostream>
#include "../Interfaces/IProgressCallback.h"

namespace RISE
{
	class StdOutProgress : public virtual IProgressCallback
	{
	protected:
		char		prefix[1024];
		char		backspaces[1024];
		bool		bfirst;

	public:
		StdOutProgress( const char* prefix_ ) : 
		  bfirst( true )
		{
			SetTitle( prefix_ );
		}

		bool Progress( const double progress, const double total )
		{
			if( !bfirst ) {
				std::cout << backspaces;
			} else {
				bfirst = false;
			}

			int n = 0;
			if( total == 0 ) {
				n = 100;
			} else {
				n = static_cast<int>( (progress/total) * 100.0 );
			}

			if( n < 10 ) {
				std::cout << prefix << "[ " << n << "%]";
			} else {
				std::cout << prefix << "[" << n << "%]";
			}

			if( n == 100.0 ) {
				std::cout << std::endl;
			}

			std::cout.flush();

			return true;
		}

		void SetTitle( const char* title )
		{
			// Set a new title
			strncpy( prefix, title, 1023 );
			prefix[1023] = 0;

			unsigned int total_len = static_cast<unsigned int>(strlen( prefix )) + 1 + 2 + 2;
			unsigned int i=0;
			for( i=0; i<total_len; i++ ) {
				backspaces[i] = '\b';
			}
			backspaces[i] = 0;

			bfirst = true;
		}
	};
}

#endif
