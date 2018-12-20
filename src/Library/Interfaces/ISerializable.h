//////////////////////////////////////////////////////////////////////
//
//  ISerializable.h - Serializable interface, used to help classes
//    serialize and deserialize themselves
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 9, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISERIALIZABLE_
#define ISERIALIZABLE_

#include "IReadBuffer.h"
#include "IWriteBuffer.h"

namespace RISE
{
	//! This interfaces means that the class is able to store and restore
	//! its state to and from buffers
	class ISerializable
	{
	protected:
		ISerializable(){};
		virtual ~ISerializable(){};

	public:
		//
		// Classes that export the serializable interface need only implement
		//   these two functions
		//
		// These preconditions and post conditions apply for both of these functions
		//   Precondition: the cur pointer in the buffer is at the begining of data
		//   Postcondition: the cur pointer in the buffer is at the end of data
		//
		// Classes that implement is interface *MUST* RIGOROUSLY follow these rules!
		//

		//! Asks the class to serialize its data 
		virtual void Serialize( 
			IWriteBuffer& buffer				///< [in] Buffer to serialize to
			) const = 0;

		//! Asks to restore itself from the buffer
		virtual void Deserialize(
			IReadBuffer& buffer					///< [in] Buffer to deserialize from
			) = 0;
	};
}

#endif

