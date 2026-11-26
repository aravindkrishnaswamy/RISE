//////////////////////////////////////////////////////////////////////
//
//  IBuffer.h - Interface to a buffer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 16, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_BUFFER_H_
#define I_BUFFER_H_

#include <cstdint>
#include <limits>

#include "IReference.h"

namespace RISE
{
	//! Generic interface to a buffer
	/// \sa IReadBuffer
	/// \sa IWriteBuffer
	class IBuffer : public virtual IReference
	{
	protected:
		IBuffer() {};
		virtual ~IBuffer() {};

	public:
		//! How many are left until the end of the buffer
		/// \return Number of bytes to the end of the buffer
		virtual unsigned int HowFarToEnd( ) const = 0;

		//! Size of the buffer
		/// \return Size of the buffer in bytes
		virtual unsigned int Size( ) const = 0;

		/// \return TRUE if the cursor is sitting at the end of the buffer
		virtual bool EndOfBuffer( ) const = 0;

		//! Seek origin, the point of fixation for a seek
		enum eSeek{ 
			START,								///< Seeks from the begining of the buffer
			CUR,								///< Seeks from the current point in the buffer
			END									///< Seeks from the end of the buffer
		};

		//! Seeks the cursor to the right location
		//! If you tell it to do silly things in debug it won't
		/// \return TRUE if successful, FALSE otherwise
		virtual bool seek(
			const eSeek type,					///< [in] Where does the seek begin
			const int amount					///< [in] Offset amount to seek
			) = 0;

		//! Returns the cursor position
		/// \return The position of the cursor in bytes from the begining of the buffer
		virtual unsigned int getCurPos( ) const = 0;
	};
	// Optional large-buffer queries preserve the existing IBuffer vtable.
	class IBuffer64
	{
	protected:
		virtual ~IBuffer64() = default;
	public:
		virtual std::uint64_t Size64() const = 0;
		virtual std::uint64_t getCurPos64() const = 0;
		virtual bool seek64( IBuffer::eSeek origin, std::int64_t amount ) = 0;
	};

	inline std::uint64_t BufferSize( const IBuffer& buffer )
	{
		if( const auto* wide = dynamic_cast<const IBuffer64*>(&buffer) ) return wide->Size64();
		return buffer.Size();
	}

	inline std::uint64_t BufferPosition( const IBuffer& buffer )
	{
		if( const auto* wide = dynamic_cast<const IBuffer64*>(&buffer) ) return wide->getCurPos64();
		return buffer.getCurPos();
	}

	inline std::uint64_t BufferBytesAvailable( const IBuffer& buffer )
	{
		const auto size = BufferSize(buffer);
		const auto position = BufferPosition(buffer);
		return position < size ? size - position : 0;
	}

	inline bool BufferSeek( IBuffer& buffer, IBuffer::eSeek origin, std::int64_t amount )
	{
		if( auto* wide = dynamic_cast<IBuffer64*>(&buffer) ) return wide->seek64(origin, amount);
		if( amount < std::numeric_limits<int>::min() || amount > std::numeric_limits<int>::max() ) return false;
		return buffer.seek( origin, static_cast<int>(amount) );
	}

}

#endif

