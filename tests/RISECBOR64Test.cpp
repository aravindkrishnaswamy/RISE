//////////////////////////////////////////////////////////////////////
//
//  RISECBOR64Test.cpp - Canonical record wire-profile gates
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Utilities/RISECBOR64.h"
#include <cstdio>
#include <limits>
#include <string>

using namespace RISE;

namespace
{
	int failures = 0;

	void Check( const bool condition, const char* message )
	{
		if( !condition ) {
			std::printf("FAIL: %s\n",message);
			++failures;
		}
	}

	bool Rejects( const RISECBOR64::Bytes& bytes )
	{
		RISECBOR64::Value value;
		return !RISECBOR64::DecodeCanonical(bytes,value);
	}

	RISECBOR64::Bytes Bytes( std::initializer_list<unsigned int> values )
	{
		RISECBOR64::Bytes result;
		for( const unsigned int value : values ) {
			result.push_back(static_cast<unsigned char>(value));
		}
		return result;
	}
}

int main()
{
	std::printf("=== RISE-CBOR64-v1 canonical record tests ===\n");

	RISECBOR64::Value::Members members;
	members.push_back(std::make_pair("float",RISECBOR64::Value::Float(1.5)));
	members.push_back(std::make_pair("bb",RISECBOR64::Value::Unsigned(2u)));
	members.push_back(std::make_pair("a",RISECBOR64::Value::Unsigned(1u)));
	RISECBOR64::Bytes encoded;
	std::string error;
	Check( RISECBOR64::Encode(RISECBOR64::Value::MapValue(members),encoded,&error),
		"unsorted input maps encode canonically" );
	const RISECBOR64::Bytes expected = Bytes({
		0xa3, 0x61,0x61,0x01, 0x62,0x62,0x62,0x02,
		0x65,0x66,0x6c,0x6f,0x61,0x74,
		0xfb,0x3f,0xf8,0x00,0x00,0x00,0x00,0x00,0x00 });
	Check( encoded == expected,
		"maps sort lexicographically by UTF-8 key bytes and floats use binary64" );
	RISECBOR64::Value decoded;
	Check( RISECBOR64::DecodeCanonical(encoded,decoded,&error) &&
		decoded.Find("float") && decoded.Find("float")->GetFloat() == 1.5,
		"canonical bytes round-trip through generic token validation" );

	const RISECBOR64::Bytes negativeZeroEncoded = []{
		RISECBOR64::Bytes result;
		RISECBOR64::Encode(RISECBOR64::Value::Float(-0.0),result);
		return result;
	}();
	Check( negativeZeroEncoded == Bytes({0xfb,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}),
		"the encoder normalizes negative zero to positive zero" );

	Check( Rejects(Bytes({0x9f,0x01,0xff})),
		"indefinite-length arrays are rejected" );
	Check( Rejects(Bytes({0x18,0x17})),
		"non-shortest integer encodings are rejected" );
	Check( Rejects(Bytes({0x78,0x01,0x61})),
		"non-shortest text lengths are rejected" );
	Check( Rejects(Bytes({0xf9,0x3c,0x00})) &&
		Rejects(Bytes({0xfa,0x3f,0x80,0x00,0x00})),
		"binary16 and binary32 floats are rejected" );
	Check( Rejects(Bytes({0xfb,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00})),
		"negative-zero record bytes are rejected rather than normalized" );
	Check( Rejects(Bytes({0xfb,0x7f,0xf0,0x00,0x00,0x00,0x00,0x00,0x00})) &&
		Rejects(Bytes({0xfb,0x7f,0xf8,0x00,0x00,0x00,0x00,0x00,0x00})),
		"infinity and NaN are rejected" );
	Check( Rejects(Bytes({0xa2,0x61,0x62,0x01,0x61,0x61,0x02})) &&
		Rejects(Bytes({0xa2,0x61,0x61,0x01,0x61,0x61,0x02})),
		"unsorted and duplicate text keys are rejected" );
	Check( Rejects(Bytes({0xa1,0x01,0x01})),
		"non-text map keys are rejected" );
	Check( Rejects(Bytes({0x61,0x80})),
		"ill-formed UTF-8 is rejected" );
	Check( Rejects(Bytes({0x01,0x00})),
		"trailing bytes are rejected" );

	const std::string composedE("\xc3\xa9",2);
	const std::string decomposedE("e\xcc\x81",3);
	const std::string noncomposableQ("q\xcc\x81",3);
	const std::string precomposedOrdered("\xc3\xa0\xcc\x95",4);
	const std::string outOfOrder("a\xcc\x95\xcc\x80",5);
	const std::string hangulSyllable("\xea\xb0\x80",3);
	const std::string hangulDecomposed("\xe1\x84\x80\xe1\x85\xa1",6);
	const std::string cjk("\xe7\x81\xab",3);
	const std::string unicode17Composed("\xf0\x96\x84\xa1",4);
	const std::string unicode17Decomposed(
		"\xf0\x96\x84\x9e\xf0\x96\x84\x9e",8);
	Check( RISECBOR64::IsUTF8NFC(composedE) &&
		!RISECBOR64::IsUTF8NFC(decomposedE),
		"canonical Latin composition is enforced" );
	Check( RISECBOR64::IsUTF8NFC(noncomposableQ) &&
		RISECBOR64::IsUTF8NFC(precomposedOrdered) &&
		!RISECBOR64::IsUTF8NFC(outOfOrder),
		"noncomposable marks remain valid while canonical ordering is enforced" );
	Check( RISECBOR64::IsUTF8NFC(hangulSyllable) &&
		!RISECBOR64::IsUTF8NFC(hangulDecomposed) &&
		RISECBOR64::IsUTF8NFC(cjk),
		"algorithmic Hangul composition and non-Latin UTF-8 are supported" );
	Check( RISECBOR64::IsUTF8NFC(unicode17Composed) &&
		!RISECBOR64::IsUTF8NFC(unicode17Decomposed),
		"Unicode-17 compositions match the generator's pinned authority" );

	RISECBOR64::Value::Members duplicateMembers;
	duplicateMembers.push_back(std::make_pair("same",RISECBOR64::Value()));
	duplicateMembers.push_back(std::make_pair("same",RISECBOR64::Value()));
	Check( !RISECBOR64::Encode(
		RISECBOR64::Value::MapValue(duplicateMembers),encoded,&error),
		"the encoder rejects duplicate semantic map keys" );
	Check( !RISECBOR64::Encode(
		RISECBOR64::Value::Float(std::numeric_limits<double>::infinity()),
		encoded,&error),
		"the encoder rejects non-finite semantic floats" );

	const unsigned char abc[] = {'a','b','c'};
	Check( RISECBOR64::SHA256Hex(abc,sizeof(abc)) ==
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		"SHA-256 matches the FIPS abc vector" );

	if( failures ) {
		std::printf("%d RISE-CBOR64-v1 test(s) failed\n",failures);
		return 1;
	}
	std::printf("All RISE-CBOR64-v1 tests passed\n");
	return 0;
}
