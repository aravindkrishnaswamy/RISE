//////////////////////////////////////////////////////////////////////
//
//  Base64.h - a tiny standard (RFC 4648) base64 codec for the agentic
//    surface (Facet 5 -- slice 0c).
//
//    The `render` / `read_image` verbs return a PNG, which is BINARY and
//    cannot travel raw inside a JSON string.  The transport therefore
//    base64-encodes the PNG bytes into a `png_base64` string field.  The
//    RISE tree has no base64 helper (the only prior use is glTF-internal
//    in the importer), so this is a small, self-contained, standard
//    (`+` `/`, `=` padding) codec placed in the Agent module.
//
//    Encode is what the transport needs; decode is provided so a caller
//    (the end-to-end test) can round-trip a `png_base64` field back to
//    bytes and confirm the PNG signature.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_BASE64_
#define RISE_AGENT_BASE64_

#include <string>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		//! Encode `bytes` to a standard RFC-4648 base64 string (with `=`
		//! padding).  An empty input yields an empty string.  Never throws.
		std::string Base64Encode( const std::vector<unsigned char>& bytes );

		//! Decode a standard RFC-4648 base64 string back to bytes.  Skips
		//! ASCII whitespace; on an invalid character (outside the alphabet /
		//! padding / whitespace) returns false and leaves `out` empty.  An
		//! empty / whitespace-only input decodes to an empty vector (true).
		bool Base64Decode( const std::string& text, std::vector<unsigned char>& out );
	}
}

#endif
