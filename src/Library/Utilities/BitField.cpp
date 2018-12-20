//////////////////////////////////////////////////////////////////////
//
//  BitField.cpp - Implements the bit field
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 9, 2000
//  Tabs: 4
//  Comments: Taken from earlier code I've written
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BitField.h"

using namespace RISE;

const int BitsInByte =	8;
const int BitsInWord = sizeof(unsigned int) * BitsInByte;

static inline int divRoundUp( int a, int b )
{
	return (a+b/2)/b;
}

/*
#define divRoundUp( a, b )\
	(a+b/2)/b;
*/

BitField::BitField( const int numElements ) 
{ 
	numBits = numElements;
	numWords = divRoundUp(numBits, BitsInWord);
	themap = new unsigned int[numWords];

	int i;

	for( i = 0; i < numWords; i++ ) {
		themap[i] = 0;
	}

	for( i = 0; i < numBits; i++ ) {
		Clear( i );
	}
}

BitField::~BitField()
{ 
    delete themap;
}

void BitField::Mark( const int bit ) 
{ 
	themap[bit / BitsInWord] |= 1 << (bit % BitsInWord);
}
    
void BitField::Clear( const int bit ) 
{
    themap[bit / BitsInWord] &= ~(1 << (bit % BitsInWord));
}

bool BitField::Test( const int bit ) const
{
	if( themap[bit / BitsInWord] & (1 << (bit % BitsInWord)) )
	{
		return true;
	} else {
		return false;
	}
}

int BitField::FindAndSet() 
{
	for( int i = 0; i < numBits; i++ ) {
		if( !Test(i) ) {
			Mark(i);
			return i;
		}
	}
	return -1;
}

int BitField::NumClear() const
{
	int count = 0;

	for( int i = 0; i < numBits; i++ ) {
		if( !Test(i) ) {
			count++;
		}
	}
	return count;
}