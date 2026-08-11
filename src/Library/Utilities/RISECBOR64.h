//////////////////////////////////////////////////////////////////////
//
//  RISECBOR64.h - Canonical RISE-CBOR64-v1 record utilities
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 6, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISECBOR64_
#define RISECBOR64_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	namespace RISECBOR64
	{
		class Value
		{
		public:
			enum Type
			{
				Null,
				Boolean,
				UnsignedInteger,
				NegativeInteger,
				Float64,
				Text,
				ByteString,
				Array,
				Map
			};

			using Bytes = std::vector<unsigned char>;
			using Values = std::vector<Value>;
			using Members = std::vector<std::pair<std::string, Value> >;

		private:
			Type m_type;
			bool m_boolean;
			std::uint64_t m_integer;
			double m_float;
			std::string m_text;
			Bytes m_bytes;
			Values m_array;
			Members m_map;

		public:
			Value();

			static Value Bool( bool value );
			static Value Unsigned( std::uint64_t value );
			static Value Signed( std::int64_t value );
			static Value NegativeArgument( std::uint64_t argument );
			static Value Float( double value );
			static Value String( const std::string& value );
			static Value BytesValue( const Bytes& value );
			static Value ArrayValue( const Values& value );
			static Value MapValue( const Members& value );

			Type GetType() const { return m_type; }
			bool GetBoolean() const { return m_boolean; }
			std::uint64_t GetIntegerArgument() const { return m_integer; }
			double GetFloat() const { return m_float; }
			const std::string& GetText() const { return m_text; }
			const Bytes& GetBytes() const { return m_bytes; }
			const Values& GetArray() const { return m_array; }
			const Members& GetMap() const { return m_map; }

			const Value* Find( const std::string& key ) const;
		};

		using Bytes = Value::Bytes;

		/// Encodes one value using the RISE-CBOR64-v1 application profile.
		/// Map members may be supplied in any order; the encoder sorts them by
		/// their UTF-8 key bytes and rejects duplicate or non-NFC keys.
		bool Encode(
			const Value& value,
			Bytes& encoded,
			std::string* error = 0
			);

		/// Validates the complete raw token stream before returning its generic
		/// value tree. Noncanonical input is rejected rather than normalized.
		bool DecodeCanonical(
			const unsigned char* encoded,
			std::size_t encodedSize,
			Value& value,
			std::string* error = 0
			);

		inline bool DecodeCanonical(
			const Bytes& encoded,
			Value& value,
			std::string* error = 0
			)
		{
			return DecodeCanonical(
				encoded.empty() ? 0 : &encoded[0], encoded.size(), value, error );
		}

		/// True exactly when the bytes are well-formed UTF-8 in Unicode 17 NFC.
		bool IsUTF8NFC( const std::string& text );

		/// SHA-256 of exact bytes, rendered as 64 lowercase hexadecimal digits.
		std::string SHA256Hex(
			const unsigned char* bytes,
			std::size_t byteCount
			);

		inline std::string SHA256Hex( const Bytes& bytes )
		{
			return SHA256Hex(
				bytes.empty() ? 0 : &bytes[0], bytes.size() );
		}
	}
}

#endif
