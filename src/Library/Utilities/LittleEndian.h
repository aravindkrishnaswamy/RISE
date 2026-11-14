#ifndef RISE_LITTLE_ENDIAN_H
#define RISE_LITTLE_ENDIAN_H

#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

namespace RISE::Implementation
{
	// The binary buffer formats store integer and IEEE-754 bits least significant byte first.
	template<class Value, class Bits>
	Value DecodeLittleEndian( const unsigned char* bytes )
	{
		static_assert( sizeof(Value) == sizeof(Bits) && std::is_unsigned_v<Bits> );
		static_assert( !std::is_floating_point_v<Value> || std::numeric_limits<Value>::is_iec559 );
		Bits bits = 0;
		for( std::size_t i = 0; i < sizeof(Bits); ++i ) {
			bits |= static_cast<Bits>(bytes[i]) << (8 * i);
		}
		Value value;
		std::memcpy( &value, &bits, sizeof(value) );
		return value;
	}

	template<class Bits, class Value>
	void EncodeLittleEndian( unsigned char* bytes, Value value )
	{
		static_assert( sizeof(Value) == sizeof(Bits) && std::is_unsigned_v<Bits> );
		static_assert( !std::is_floating_point_v<Value> || std::numeric_limits<Value>::is_iec559 );
		Bits bits;
		std::memcpy( &bits, &value, sizeof(bits) );
		for( std::size_t i = 0; i < sizeof(Bits); ++i ) {
			bytes[i] = static_cast<unsigned char>(bits >> (8 * i));
		}
	}
}

#endif
