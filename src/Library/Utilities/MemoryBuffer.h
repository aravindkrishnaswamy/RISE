//////////////////////////////////////////////////////////////////////
//
//  MemoryBuffer.h - Defines a memory buffer with a virtual memory
//  cursor that helps read memory easily
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: July 18, 2002
//  Tabs: 4
//  Comments:  NOTE: The contents of this buffer from any application
//         should be flush safe.  ie. don't use stuff like pointers
//         offsets are fine, but pointers are a no-no, unless you are
//         absolutely 100% certain, the buffer won't get flushed!
//
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../Interfaces/IMemoryBuffer.h"
#include "Reference.h"

#ifndef MEMORY_BUFFER_
#define MEMORY_BUFFER_

namespace RISE
{
	namespace Implementation
	{
		class MemoryBuffer : public virtual IMemoryBuffer, public virtual Reference
		{
		public:
			//
			// Constructors
			//

			// Default constructor - there is no memory in the buffer
			// you'll have to resize the buffer to get some memory
			MemoryBuffer();

			// This constructor allocates the specified amount of memory
			// and uses that
			MemoryBuffer( const unsigned int size );

			// This is a copy constructor, it basically clones itself
			MemoryBuffer( const MemoryBuffer& mb );

			// This is a copy constructor of the interface function
			MemoryBuffer( IMemoryBuffer& mb );

			// This constructor creates a memory buffer from a file. 
			// It basically loads the entire file into the buffer
			MemoryBuffer( const char * szFileName );

			// This makes a MemoryBuffer out of pre-existing memory, note
			MemoryBuffer( char * pMemory, const unsigned int size, bool bTakeOwnership = true );

			//
			// These methods are to satisfy the IBuffer interface
			//

			void Clear( );
			bool Resize( unsigned int new_size, bool bForce = false );
			bool ResizeForMore( unsigned int more_bytes );
			unsigned int HowFarToEnd( ) const;
			unsigned int Size( ) const;
			bool EndOfBuffer( ) const;

			bool seek( const eSeek type, const int amount );
			inline unsigned int getCurPos( ) const { return nCursor; };

			//
			// This is for the IReadBuffer interface
			//

			char getChar();
			unsigned char getUChar();

			short getWord();
			unsigned short getUWord();

			int getInt();
			unsigned int getUInt();

			float getFloat();
			double getDouble();

			bool getBytes( void* pDest, unsigned int amount );
		
			int getLine( char* pDest, unsigned int max );


			//
			// This is for the IWriteBuffer interface
			//

			bool setChar( const char ch );
			bool setUChar( const unsigned char ch );

			bool setWord( const short sh );
			bool setUWord( const unsigned short sh );

			bool setInt( const int n );
			bool setUInt( const unsigned int n );

			bool setFloat( const float f );
			bool setDouble( const double d );

			bool setBytes( const void* pSource, unsigned int amount );

			//
			// This is for the IMemoryBuffer interface
			//

			// Dumps the buffer to a file
			bool DumpToFile( 
				const char * szFileName			///< Filename to dump buffer to
				);

			// Dumps the buffer from the begining to the cursor location to a file
			bool DumpToFileToCursor( 
				const char * szFileName			///< Filename to dump buffer to
				);

			char * Pointer( );
			const char * Pointer( ) const;

			char * PointerAtCursor( );
			const char * PointerAtCursor( ) const;

			inline bool DoOwnMemory() const { return bIOwnMemory; };


		protected:
			// Default destructor
			virtual ~MemoryBuffer( );


			unsigned int		nSize;			///< Size of the buffer
			char*				pBuffer;		///< Memory itself
			unsigned int		nCursor;		///< Current location within the buffer
			bool				bIOwnMemory;	///< Does the object own its pBuffer its using?
		};
	}
}

#endif

