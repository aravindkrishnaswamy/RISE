//////////////////////////////////////////////////////////////////////
//
//  BitField.h - Its a bit field, whats more to say ?
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 9, 2000
//  Tabs: 4
//  Comments: Taken from earlier code I've written
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIT_FIELD
#define BIT_FIELD

namespace RISE
{
	class BitField
	{
	protected:
		int numBits;
		int numWords;

		unsigned int *themap;					// bit storage

	public:
		BitField( const int numElements );
		~BitField();

		void Mark( const int bit );				// sets a bit as marked
		void Clear( const int bit );			// clears the bit
		bool Test( const int bit ) const;		// tests to see if the bit is set
		int FindAndSet();						// finds the first free bit, and sets it
		int NumClear() const;					// return the number of clear bits
	};
}

#endif
