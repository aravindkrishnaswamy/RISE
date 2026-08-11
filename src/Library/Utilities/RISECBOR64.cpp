//////////////////////////////////////////////////////////////////////
//
//  RISECBOR64.cpp - Canonical RISE-CBOR64-v1 record utilities
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 6, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RISECBOR64.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace RISE;

namespace
{
	struct CanonicalDecomposition
	{
		std::uint32_t codePoint;
		std::uint32_t offset;
		std::uint8_t length;
	};

	struct CanonicalCombiningClass
	{
		std::uint32_t codePoint;
		std::uint8_t value;
	};

	struct CanonicalComposition
	{
		std::uint32_t first;
		std::uint32_t second;
		std::uint32_t composite;
	};

	// Generated from the Unicode 17.0.0 UnicodeData.txt and
	// CompositionExclusions.txt files. The generation retains only canonical
	// decomposition, nonzero combining-class, and composition entries.
#include "UnicodeNFCData.inc"

	template<typename Entry>
	static const Entry* FindCodePointEntry(
		const Entry* begin,
		const Entry* end,
		const std::uint32_t codePoint
		)
	{
		const Entry* it = std::lower_bound(
			begin, end, codePoint,
			[]( const Entry& entry, const std::uint32_t value ) {
				return entry.codePoint < value;
			} );
		return it != end && it->codePoint == codePoint ? it : 0;
	}

	static std::uint8_t CombiningClass( const std::uint32_t codePoint )
	{
		const CanonicalCombiningClass* entry = FindCodePointEntry(
			kCanonicalCombiningClasses,
			kCanonicalCombiningClasses + kCanonicalCombiningClassCount,
			codePoint );
		return entry ? entry->value : 0;
	}

	static bool DecodeUTF8(
		const std::string& text,
		std::vector<std::uint32_t>& codePoints
		)
	{
		codePoints.clear();
		for( std::size_t i = 0; i < text.size(); ) {
			const unsigned char first = static_cast<unsigned char>(text[i]);
			std::uint32_t codePoint = 0;
			std::size_t length = 0;
			std::uint32_t minimum = 0;
			if( first <= 0x7fu ) {
				codePoint = first;
				length = 1;
			} else if( first >= 0xc2u && first <= 0xdfu ) {
				codePoint = first & 0x1fu;
				length = 2;
				minimum = 0x80u;
			} else if( first >= 0xe0u && first <= 0xefu ) {
				codePoint = first & 0x0fu;
				length = 3;
				minimum = 0x800u;
			} else if( first >= 0xf0u && first <= 0xf4u ) {
				codePoint = first & 0x07u;
				length = 4;
				minimum = 0x10000u;
			} else {
				return false;
			}
			if( i + length > text.size() ) return false;
			for( std::size_t j = 1; j < length; ++j ) {
				const unsigned char continuation =
					static_cast<unsigned char>(text[i+j]);
				if( (continuation & 0xc0u) != 0x80u ) return false;
				codePoint = (codePoint << 6) | (continuation & 0x3fu);
			}
			if( codePoint < minimum || codePoint > 0x10ffffu ||
				(codePoint >= 0xd800u && codePoint <= 0xdfffu) ) return false;
			codePoints.push_back(codePoint);
			i += length;
		}
		return true;
	}

	static void DecomposeCodePoint(
		const std::uint32_t codePoint,
		std::vector<std::uint32_t>& output
		)
	{
		static const std::uint32_t sBase = 0xac00u;
		static const std::uint32_t lBase = 0x1100u;
		static const std::uint32_t vBase = 0x1161u;
		static const std::uint32_t tBase = 0x11a7u;
		static const std::uint32_t lCount = 19u;
		static const std::uint32_t vCount = 21u;
		static const std::uint32_t tCount = 28u;
		static const std::uint32_t nCount = vCount*tCount;
		static const std::uint32_t sCount = lCount*nCount;
		if( codePoint >= sBase && codePoint < sBase+sCount ) {
			const std::uint32_t sIndex = codePoint-sBase;
			output.push_back(lBase+sIndex/nCount);
			output.push_back(vBase+(sIndex%nCount)/tCount);
			const std::uint32_t tIndex = sIndex%tCount;
			if( tIndex ) output.push_back(tBase+tIndex);
			return;
		}

		const CanonicalDecomposition* entry = FindCodePointEntry(
			kCanonicalDecompositions,
			kCanonicalDecompositions+kCanonicalDecompositionCount,
			codePoint );
		if( !entry ) {
			output.push_back(codePoint);
			return;
		}
		for( std::uint8_t i = 0; i < entry->length; ++i ) {
			DecomposeCodePoint(
				kCanonicalDecompositionData[entry->offset+i], output );
		}
	}

	static bool ComposePair(
		const std::uint32_t first,
		const std::uint32_t second,
		std::uint32_t& composite
		)
	{
		static const std::uint32_t sBase = 0xac00u;
		static const std::uint32_t lBase = 0x1100u;
		static const std::uint32_t vBase = 0x1161u;
		static const std::uint32_t tBase = 0x11a7u;
		static const std::uint32_t lCount = 19u;
		static const std::uint32_t vCount = 21u;
		static const std::uint32_t tCount = 28u;
		static const std::uint32_t nCount = vCount*tCount;
		static const std::uint32_t sCount = lCount*nCount;
		const std::uint32_t lIndex = first-lBase;
		if( lIndex < lCount ) {
			const std::uint32_t vIndex = second-vBase;
			if( vIndex < vCount ) {
				composite = sBase+(lIndex*vCount+vIndex)*tCount;
				return true;
			}
		}
		const std::uint32_t sIndex = first-sBase;
		if( sIndex < sCount && sIndex%tCount == 0u ) {
			const std::uint32_t tIndex = second-tBase;
			if( tIndex > 0u && tIndex < tCount ) {
				composite = first+tIndex;
				return true;
			}
		}

		const CanonicalComposition* begin = kCanonicalCompositions;
		const CanonicalComposition* end = begin+kCanonicalCompositionCount;
		const CanonicalComposition* entry = std::lower_bound(
			begin, end, std::make_pair(first,second),
			[]( const CanonicalComposition& value,
				const std::pair<std::uint32_t,std::uint32_t>& key ) {
				return value.first < key.first ||
					(value.first == key.first && value.second < key.second);
			} );
		if( entry == end || entry->first != first || entry->second != second ) {
			return false;
		}
		composite = entry->composite;
		return true;
	}

	static bool IsNFC( const std::vector<std::uint32_t>& input )
	{
		std::vector<std::uint32_t> decomposed;
		decomposed.reserve(input.size());
		for( const std::uint32_t codePoint : input ) {
			DecomposeCodePoint(codePoint,decomposed);
		}

		for( std::size_t i = 1; i < decomposed.size(); ++i ) {
			const std::uint8_t ccc = CombiningClass(decomposed[i]);
			if( ccc == 0u ) continue;
			std::size_t insertion = i;
			while( insertion > 0u ) {
				const std::uint8_t previous = CombiningClass(
					decomposed[insertion-1u] );
				if( previous == 0u || previous <= ccc ) break;
				std::swap(decomposed[insertion],decomposed[insertion-1u]);
				--insertion;
			}
		}

		std::vector<std::uint32_t> composed;
		composed.reserve(decomposed.size());
		std::size_t starterIndex = 0u;
		std::uint8_t lastClass = 0u;
		for( const std::uint32_t codePoint : decomposed ) {
			const std::uint8_t ccc = CombiningClass(codePoint);
			std::uint32_t composite = 0u;
			const bool canCompose = !composed.empty() &&
				ComposePair(composed[starterIndex],codePoint,composite) &&
				(lastClass < ccc || lastClass == 0u);
			if( canCompose ) {
				composed[starterIndex] = composite;
			} else {
				if( ccc == 0u ) starterIndex = composed.size();
				composed.push_back(codePoint);
				lastClass = ccc;
			}
		}
		return composed == input;
	}

	static void SetError( std::string* error, const char* message )
	{
		if( error ) *error = message;
	}

	static bool KeyLess( const std::string& a, const std::string& b )
	{
		return std::lexicographical_compare(
			a.begin(), a.end(), b.begin(), b.end(),
			[]( const char lhs, const char rhs ) {
				return static_cast<unsigned char>(lhs) <
					static_cast<unsigned char>(rhs);
			} );
	}

	static void AppendUnsigned(
		const unsigned int major,
		const std::uint64_t value,
		RISECBOR64::Bytes& output
		)
	{
		const unsigned char base = static_cast<unsigned char>(major << 5);
		if( value < 24u ) {
			output.push_back(static_cast<unsigned char>(base|value));
		} else if( value <= 0xffu ) {
			output.push_back(static_cast<unsigned char>(base|24u));
			output.push_back(static_cast<unsigned char>(value));
		} else if( value <= 0xffffu ) {
			output.push_back(static_cast<unsigned char>(base|25u));
			for( int shift = 8; shift >= 0; shift -= 8 ) {
				output.push_back(static_cast<unsigned char>(value>>shift));
			}
		} else if( value <= 0xffffffffu ) {
			output.push_back(static_cast<unsigned char>(base|26u));
			for( int shift = 24; shift >= 0; shift -= 8 ) {
				output.push_back(static_cast<unsigned char>(value>>shift));
			}
		} else {
			output.push_back(static_cast<unsigned char>(base|27u));
			for( int shift = 56; shift >= 0; shift -= 8 ) {
				output.push_back(static_cast<unsigned char>(value>>shift));
			}
		}
	}

	static bool EncodeValue(
		const RISECBOR64::Value& value,
		RISECBOR64::Bytes& output,
		std::string* error,
		const unsigned int depth
		)
	{
		if( depth > 64u ) {
			SetError(error,"RISE-CBOR64-v1 nesting exceeds 64 levels");
			return false;
		}
		switch( value.GetType() ) {
		case RISECBOR64::Value::Null:
			output.push_back(0xf6u);
			return true;
		case RISECBOR64::Value::Boolean:
			output.push_back(value.GetBoolean() ? 0xf5u : 0xf4u);
			return true;
		case RISECBOR64::Value::UnsignedInteger:
			AppendUnsigned(0u,value.GetIntegerArgument(),output);
			return true;
		case RISECBOR64::Value::NegativeInteger:
			AppendUnsigned(1u,value.GetIntegerArgument(),output);
			return true;
		case RISECBOR64::Value::Float64:
		{
			const double number = value.GetFloat();
			std::uint64_t bits = 0;
			std::memcpy(&bits,&number,sizeof(bits));
			if( (bits&0x7ff0000000000000ull) == 0x7ff0000000000000ull ) {
				SetError(error,"RISE-CBOR64-v1 forbids NaN and infinity");
				return false;
			}
			if( (bits&0x7fffffffffffffffull) == 0u ) bits = 0u;
			output.push_back(0xfbu);
			for( int shift = 56; shift >= 0; shift -= 8 ) {
				output.push_back(static_cast<unsigned char>(bits>>shift));
			}
			return true;
		}
		case RISECBOR64::Value::Text:
			if( !RISECBOR64::IsUTF8NFC(value.GetText()) ) {
				SetError(error,"RISE-CBOR64-v1 text is not well-formed UTF-8 NFC");
				return false;
			}
			AppendUnsigned(3u,value.GetText().size(),output);
			output.insert(output.end(),value.GetText().begin(),value.GetText().end());
			return true;
		case RISECBOR64::Value::ByteString:
			AppendUnsigned(2u,value.GetBytes().size(),output);
			output.insert(output.end(),value.GetBytes().begin(),value.GetBytes().end());
			return true;
		case RISECBOR64::Value::Array:
			AppendUnsigned(4u,value.GetArray().size(),output);
			for( const RISECBOR64::Value& item : value.GetArray() ) {
				if( !EncodeValue(item,output,error,depth+1u) ) return false;
			}
			return true;
		case RISECBOR64::Value::Map:
		{
			std::vector<const RISECBOR64::Value::Members::value_type*> members;
			members.reserve(value.GetMap().size());
			for( const auto& member : value.GetMap() ) {
				if( !RISECBOR64::IsUTF8NFC(member.first) ) {
					SetError(error,"RISE-CBOR64-v1 map key is not UTF-8 NFC");
					return false;
				}
				members.push_back(&member);
			}
			std::sort(members.begin(),members.end(),
				[]( const auto* lhs, const auto* rhs ) {
					return KeyLess(lhs->first,rhs->first);
				} );
			for( std::size_t i = 1; i < members.size(); ++i ) {
				if( members[i-1u]->first == members[i]->first ) {
					SetError(error,"RISE-CBOR64-v1 map keys must be unique");
					return false;
				}
			}
			AppendUnsigned(5u,members.size(),output);
			for( const auto* member : members ) {
				AppendUnsigned(3u,member->first.size(),output);
				output.insert(output.end(),member->first.begin(),member->first.end());
				if( !EncodeValue(member->second,output,error,depth+1u) ) return false;
			}
			return true;
		}
		}
		SetError(error,"RISE-CBOR64-v1 value type is invalid");
		return false;
	}

	class Decoder
	{
		const unsigned char* m_bytes;
		std::size_t m_size;
		std::size_t m_offset;
		std::string* m_error;

		bool Fail( const char* message )
		{
			SetError(m_error,message);
			return false;
		}

		bool ReadByte( unsigned char& value )
		{
			if( m_offset >= m_size ) return Fail("RISE-CBOR64-v1 input is truncated");
			value = m_bytes[m_offset++];
			return true;
		}

		bool ReadArgument(
			const unsigned int additional,
			std::uint64_t& value
			)
		{
			if( additional < 24u ) {
				value = additional;
				return true;
			}
			unsigned int width = 0u;
			std::uint64_t minimum = 0u;
			switch( additional ) {
			case 24u: width=1u; minimum=24u; break;
			case 25u: width=2u; minimum=0x100u; break;
			case 26u: width=4u; minimum=0x10000u; break;
			case 27u: width=8u; minimum=0x100000000ull; break;
			case 31u: return Fail("RISE-CBOR64-v1 forbids indefinite lengths");
			default: return Fail("RISE-CBOR64-v1 additional information is reserved");
			}
			if( width > m_size-m_offset ) return Fail("RISE-CBOR64-v1 argument is truncated");
			value = 0u;
			for( unsigned int i = 0; i < width; ++i ) {
				value = (value<<8)|m_bytes[m_offset++];
			}
			if( value < minimum ) return Fail("RISE-CBOR64-v1 integer or length is not shortest-form");
			return true;
		}

		bool ReadLength(
			const unsigned int additional,
			std::size_t& value
			)
		{
			std::uint64_t argument = 0u;
			if( !ReadArgument(additional,argument) ) return false;
			if( argument > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ) {
				return Fail("RISE-CBOR64-v1 length exceeds platform size_t");
			}
			value = static_cast<std::size_t>(argument);
			return true;
		}

		bool ReadTextBody(
			const unsigned int additional,
			std::string& text
			)
		{
			std::size_t length = 0u;
			if( !ReadLength(additional,length) ) return false;
			if( length > m_size-m_offset ) return Fail("RISE-CBOR64-v1 text is truncated");
			text.assign(reinterpret_cast<const char*>(m_bytes+m_offset),length);
			m_offset += length;
			if( !RISECBOR64::IsUTF8NFC(text) ) {
				return Fail("RISE-CBOR64-v1 text is not well-formed UTF-8 NFC");
			}
			return true;
		}

		bool ReadValue( RISECBOR64::Value& value, const unsigned int depth )
		{
			if( depth > 64u ) return Fail("RISE-CBOR64-v1 nesting exceeds 64 levels");
			unsigned char initial = 0u;
			if( !ReadByte(initial) ) return false;
			const unsigned int major = initial>>5;
			const unsigned int additional = initial&0x1fu;
			std::uint64_t argument = 0u;
			switch( major ) {
			case 0u:
				if( !ReadArgument(additional,argument) ) return false;
				value = RISECBOR64::Value::Unsigned(argument);
				return true;
			case 1u:
				if( !ReadArgument(additional,argument) ) return false;
				value = RISECBOR64::Value::NegativeArgument(argument);
				return true;
			case 2u:
			{
				std::size_t length = 0u;
				if( !ReadLength(additional,length) ) return false;
				if( length > m_size-m_offset ) return Fail("RISE-CBOR64-v1 byte string is truncated");
				RISECBOR64::Bytes bytes(m_bytes+m_offset,m_bytes+m_offset+length);
				m_offset += length;
				value = RISECBOR64::Value::BytesValue(bytes);
				return true;
			}
			case 3u:
			{
				std::string text;
				if( !ReadTextBody(additional,text) ) return false;
				value = RISECBOR64::Value::String(text);
				return true;
			}
			case 4u:
			{
				std::size_t count = 0u;
				if( !ReadLength(additional,count) ) return false;
				if( count > m_size-m_offset ) return Fail("RISE-CBOR64-v1 array length is impossible");
				RISECBOR64::Value::Values items;
				items.reserve(count);
				for( std::size_t i = 0; i < count; ++i ) {
					RISECBOR64::Value item;
					if( !ReadValue(item,depth+1u) ) return false;
					items.push_back(item);
				}
				value = RISECBOR64::Value::ArrayValue(items);
				return true;
			}
			case 5u:
			{
				std::size_t count = 0u;
				if( !ReadLength(additional,count) ) return false;
				if( count > (m_size-m_offset)/2u ) return Fail("RISE-CBOR64-v1 map length is impossible");
				RISECBOR64::Value::Members members;
				members.reserve(count);
				std::string previous;
				for( std::size_t i = 0; i < count; ++i ) {
					unsigned char keyInitial = 0u;
					if( !ReadByte(keyInitial) ) return false;
					if( (keyInitial>>5) != 3u ) return Fail("RISE-CBOR64-v1 map keys must be text strings");
					std::string key;
					if( !ReadTextBody(keyInitial&0x1fu,key) ) return false;
					if( i && !KeyLess(previous,key) ) {
						return Fail("RISE-CBOR64-v1 map keys are duplicate or unsorted");
					}
					previous = key;
					RISECBOR64::Value member;
					if( !ReadValue(member,depth+1u) ) return false;
					members.push_back(std::make_pair(key,member));
				}
				value = RISECBOR64::Value::MapValue(members);
				return true;
			}
			case 6u:
				return Fail("RISE-CBOR64-v1 does not admit CBOR tags");
			case 7u:
				if( additional == 20u || additional == 21u ) {
					value = RISECBOR64::Value::Bool(additional==21u);
					return true;
				}
				if( additional == 22u ) {
					value = RISECBOR64::Value();
					return true;
				}
				if( additional != 27u ) {
					return Fail("RISE-CBOR64-v1 permits only binary64 floats and true/false/null simple values");
				}
				if( m_size-m_offset < 8u ) return Fail("RISE-CBOR64-v1 binary64 is truncated");
				argument = 0u;
				for( unsigned int i = 0; i < 8u; ++i ) {
					argument = (argument<<8)|m_bytes[m_offset++];
				}
				double number;
				std::memcpy(&number,&argument,sizeof(number));
				if( !std::isfinite(number) ) return Fail("RISE-CBOR64-v1 forbids NaN and infinity");
				if( argument == 0x8000000000000000ull ) {
					return Fail("RISE-CBOR64-v1 normalizes negative zero to positive zero");
				}
				value = RISECBOR64::Value::Float(number);
				return true;
			}
			return Fail("RISE-CBOR64-v1 major type is invalid");
		}

	public:
		Decoder(
			const unsigned char* bytes,
			const std::size_t size,
			std::string* error
			) :
		  m_bytes(bytes), m_size(size), m_offset(0u), m_error(error)
		{
		}

		bool Decode( RISECBOR64::Value& value )
		{
			if( !m_bytes || m_size == 0u ) return Fail("RISE-CBOR64-v1 input is empty");
			if( !ReadValue(value,0u) ) return false;
			return m_offset == m_size || Fail("RISE-CBOR64-v1 input has trailing bytes");
		}
	};

	class SHA256
	{
		std::uint32_t m_state[8];
		std::uint64_t m_bitCount;
		unsigned char m_buffer[64];
		std::size_t m_bufferSize;

		static std::uint32_t RotateRight(
			const std::uint32_t value,
			const unsigned int bits
			)
		{
			return (value>>bits)|(value<<(32u-bits));
		}

		void Transform( const unsigned char* block )
		{
			static const std::uint32_t constants[64] = {
				0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
				0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
				0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
				0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
				0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
				0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
				0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
				0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
			std::uint32_t words[64];
			for( unsigned int i = 0; i < 16u; ++i ) {
				words[i] = (std::uint32_t(block[4u*i])<<24) |
					(std::uint32_t(block[4u*i+1u])<<16) |
					(std::uint32_t(block[4u*i+2u])<<8) |
					std::uint32_t(block[4u*i+3u]);
			}
			for( unsigned int i = 16u; i < 64u; ++i ) {
				const std::uint32_t s0 = RotateRight(words[i-15u],7u) ^
					RotateRight(words[i-15u],18u) ^ (words[i-15u]>>3u);
				const std::uint32_t s1 = RotateRight(words[i-2u],17u) ^
					RotateRight(words[i-2u],19u) ^ (words[i-2u]>>10u);
				words[i] = words[i-16u]+s0+words[i-7u]+s1;
			}
			std::uint32_t a=m_state[0], b=m_state[1], c=m_state[2], d=m_state[3];
			std::uint32_t e=m_state[4], f=m_state[5], g=m_state[6], h=m_state[7];
			for( unsigned int i = 0; i < 64u; ++i ) {
				const std::uint32_t sum1 = RotateRight(e,6u)^RotateRight(e,11u)^RotateRight(e,25u);
				const std::uint32_t choice = (e&f)^((~e)&g);
				const std::uint32_t temp1 = h+sum1+choice+constants[i]+words[i];
				const std::uint32_t sum0 = RotateRight(a,2u)^RotateRight(a,13u)^RotateRight(a,22u);
				const std::uint32_t majority = (a&b)^(a&c)^(b&c);
				const std::uint32_t temp2 = sum0+majority;
				h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
			}
			m_state[0]+=a; m_state[1]+=b; m_state[2]+=c; m_state[3]+=d;
			m_state[4]+=e; m_state[5]+=f; m_state[6]+=g; m_state[7]+=h;
		}

	public:
		SHA256() :
		  m_state{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
			0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u},
		  m_bitCount(0u), m_buffer{}, m_bufferSize(0u)
		{
		}

		void Update( const unsigned char* data, std::size_t size )
		{
			if( !data || size == 0u ) return;
			m_bitCount += static_cast<std::uint64_t>(size)*8u;
			while( size ) {
				const std::size_t copy = std::min(size,64u-m_bufferSize);
				std::memcpy(m_buffer+m_bufferSize,data,copy);
				m_bufferSize += copy;
				data += copy;
				size -= copy;
				if( m_bufferSize == 64u ) {
					Transform(m_buffer);
					m_bufferSize = 0u;
				}
			}
		}

		void Final( unsigned char (&digest)[32] )
		{
			m_buffer[m_bufferSize++] = 0x80u;
			if( m_bufferSize > 56u ) {
				while( m_bufferSize < 64u ) m_buffer[m_bufferSize++] = 0u;
				Transform(m_buffer);
				m_bufferSize = 0u;
			}
			while( m_bufferSize < 56u ) m_buffer[m_bufferSize++] = 0u;
			for( int shift = 56; shift >= 0; shift -= 8 ) {
				m_buffer[m_bufferSize++] = static_cast<unsigned char>(m_bitCount>>shift);
			}
			Transform(m_buffer);
			for( unsigned int i = 0; i < 8u; ++i ) {
				for( unsigned int j = 0; j < 4u; ++j ) {
					digest[4u*i+j] = static_cast<unsigned char>(
						m_state[i]>>(24u-8u*j) );
				}
			}
		}
	};
}

RISECBOR64::Value::Value() :
  m_type(Null), m_boolean(false), m_integer(0u), m_float(0.0)
{
}

RISECBOR64::Value RISECBOR64::Value::Bool( const bool value )
{
	Value result;
	result.m_type = Boolean;
	result.m_boolean = value;
	return result;
}

RISECBOR64::Value RISECBOR64::Value::Unsigned( const std::uint64_t value )
{
	Value result;
	result.m_type = UnsignedInteger;
	result.m_integer = value;
	return result;
}

RISECBOR64::Value RISECBOR64::Value::Signed( const std::int64_t value )
{
	return value >= 0 ? Unsigned(static_cast<std::uint64_t>(value)) :
		NegativeArgument(static_cast<std::uint64_t>(-(value+1)));
}

RISECBOR64::Value RISECBOR64::Value::NegativeArgument(
	const std::uint64_t argument
	)
{
	Value result;
	result.m_type = NegativeInteger;
	result.m_integer = argument;
	return result;
}

RISECBOR64::Value RISECBOR64::Value::Float( const double value )
{
	Value result;
	result.m_type = Float64;
	result.m_float = value;
	return result;
}

RISECBOR64::Value RISECBOR64::Value::String( const std::string& value )
{
	Value result;
	result.m_type = Text;
	result.m_text = value;
	return result;
}

RISECBOR64::Value RISECBOR64::Value::BytesValue( const Bytes& value )
{
	Value result;
	result.m_type = ByteString;
	result.m_bytes = value;
	return result;
}

RISECBOR64::Value RISECBOR64::Value::ArrayValue( const Values& value )
{
	Value result;
	result.m_type = Array;
	result.m_array = value;
	return result;
}

RISECBOR64::Value RISECBOR64::Value::MapValue( const Members& value )
{
	Value result;
	result.m_type = Map;
	result.m_map = value;
	return result;
}

const RISECBOR64::Value* RISECBOR64::Value::Find(
	const std::string& key
	) const
{
	if( m_type != Map ) return 0;
	for( const auto& member : m_map ) {
		if( member.first == key ) return &member.second;
	}
	return 0;
}

bool RISECBOR64::Encode(
	const Value& value,
	Bytes& encoded,
	std::string* error
	)
{
	encoded.clear();
	if( error ) error->clear();
	if( !EncodeValue(value,encoded,error,0u) ) {
		encoded.clear();
		return false;
	}
	return true;
}

bool RISECBOR64::DecodeCanonical(
	const unsigned char* encoded,
	const std::size_t encodedSize,
	Value& value,
	std::string* error
	)
{
	if( error ) error->clear();
	Decoder decoder(encoded,encodedSize,error);
	Value decoded;
	if( !decoder.Decode(decoded) ) return false;
	Bytes canonical;
	std::string encodeError;
	if( !Encode(decoded,canonical,&encodeError) ) {
		SetError(error,"RISE-CBOR64-v1 validated value cannot be re-encoded");
		return false;
	}
	if( canonical.size() != encodedSize ||
		!std::equal(canonical.begin(),canonical.end(),encoded) ) {
		SetError(error,"RISE-CBOR64-v1 bytes are not canonical");
		return false;
	}
	value = decoded;
	return true;
}

bool RISECBOR64::IsUTF8NFC( const std::string& text )
{
	std::vector<std::uint32_t> codePoints;
	return DecodeUTF8(text,codePoints) && IsNFC(codePoints);
}

std::string RISECBOR64::SHA256Hex(
	const unsigned char* bytes,
	const std::size_t byteCount
	)
{
	SHA256 sha;
	sha.Update(bytes,byteCount);
	unsigned char digest[32];
	sha.Final(digest);
	static const char hex[] = "0123456789abcdef";
	std::string result(64u,'0');
	for( unsigned int i = 0; i < 32u; ++i ) {
		result[2u*i] = hex[digest[i]>>4];
		result[2u*i+1u] = hex[digest[i]&0x0fu];
	}
	return result;
}
