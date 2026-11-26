//////////////////////////////////////////////////////////////////////
//
//  Json.h - a tiny hand-rolled JSON value + parser + serializer scoped
//    to the JSON-RPC message set (Facet 5, the agentic surface --
//    slice 0c).
//
//    The RISE tree deliberately has NO general JSON library (extlib/cgltf
//    is glTF-specific + parse-only), and the JSON-RPC transport
//    (docs/agentic-redesign/50-agentic-surface.md §2.1, §6) needs to both
//    PARSE incoming request lines and SERIALIZE outgoing response lines.
//    So this is a deliberately small RFC-8259 codec -- a message codec,
//    NOT a general JSON library -- sized to the request/response shapes
//    AgentRpc emits: objects, arrays, strings (with full escaping so a
//    base64 PNG field + a multi-line document string with embedded
//    quotes/newlines/tabs round-trips), numbers (doubles, %.17g), bools,
//    and null.
//
//    Correctness scope explicitly covered (tested in AgentFirstSliceTest):
//      * string escaping / unescaping: `"` `\` `\b \f \n \r \t`, the
//        solidus `\/` (accepted on read, NOT emitted on write per RFC),
//        and `\u00XX` control-char escapes (read: full `\uXXXX` incl. a
//        surrogate pair -> UTF-8).
//      * numbers: parse + emit doubles losslessly enough for scene values
//        (%.17g, matching the CST serializer); integers emitted without a
//        trailing ".0" when they are integral and small enough.
//      * nesting: arrays + objects to arbitrary depth (a recursion guard
//        caps pathological input so a malformed line can never blow the
//        stack -- it fails as a parse error instead).
//
//    Deliberately minimal corners (safe for the slice-0c message set):
//      * duplicate object keys: last wins (we store an ordered vector of
//        pairs; lookup returns the last).  The RPC set never sends dupes.
//      * a bare top-level scalar parses fine, but the RPC set is always a
//        top-level object.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_JSON_
#define RISE_AGENT_JSON_

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RISE
{
	namespace Agent
	{
		//! A JSON value: one of null / bool / number / string / array /
		//! object.  Value-semantics (copyable); objects preserve insertion
		//! order and, on a duplicate key, last-set wins on lookup.
		class JsonValue
		{
		public:
			enum class Type { Null, Bool, Number, String, Array, Object };

			JsonValue() : mType( Type::Null ), mBool( false ), mNumber( 0.0 ) {}

			static JsonValue MakeNull()                    { return JsonValue(); }
			static JsonValue MakeBool( bool b )            { JsonValue v; v.mType = Type::Bool;   v.mBool = b;   return v; }
			static JsonValue MakeNumber( double d )        { JsonValue v; v.mType = Type::Number; v.mNumber = d; return v; }
			static JsonValue MakeString( const std::string& s ) { JsonValue v; v.mType = Type::String; v.mString = s; return v; }
			static JsonValue MakeArray()                   { JsonValue v; v.mType = Type::Array;  return v; }
			static JsonValue MakeObject()                  { JsonValue v; v.mType = Type::Object; return v; }

			Type type() const { return mType; }
			bool isNull()   const { return mType == Type::Null; }
			bool isBool()   const { return mType == Type::Bool; }
			bool isNumber() const { return mType == Type::Number; }
			bool isString() const { return mType == Type::String; }
			bool isArray()  const { return mType == Type::Array; }
			bool isObject() const { return mType == Type::Object; }

			//! Scalar accessors -- each returns a caller-supplied default when
			//! the value is not of the requested type (never throws).
			bool        asBool( bool def = false ) const   { return mType == Type::Bool   ? mBool   : def; }
			double      asNumber( double def = 0.0 ) const { return mType == Type::Number ? mNumber : def; }
			const std::string& asString() const;   //!< "" when not a string (returns a static empty)

			//! Array access.
			std::size_t size() const { return mArray.size(); }
			const JsonValue& at( std::size_t i ) const;   //!< Null when out of range
			void push_back( const JsonValue& v ) { mArray.push_back( v ); }

			//! Object access.  `set` appends a key/value (last-set wins on
			//! `find`).  `find` returns nullptr when the key is absent; `get`
			//! returns a static Null.  `has` is a presence test.
			void set( const std::string& key, const JsonValue& v ) { mObject.emplace_back( key, v ); }
			const JsonValue* find( const std::string& key ) const;
			const JsonValue& get( const std::string& key ) const;
			bool has( const std::string& key ) const { return find( key ) != nullptr; }

			//! The ordered object members (for serialization / iteration).
			const std::vector<std::pair<std::string, JsonValue>>& members() const { return mObject; }

		private:
			Type                                            mType;
			bool                                            mBool;
			double                                          mNumber;
			std::string                                     mString;
			std::vector<JsonValue>                          mArray;
			std::vector<std::pair<std::string, JsonValue>>  mObject;
		};

		//! Parse `text` into a JsonValue.  On success returns true and fills
		//! `out`; on any malformation (bad token, unterminated string,
		//! trailing garbage, over-deep nesting) returns false, fills
		//! `outError` with a short human message, and leaves `out` Null.
		//! NEVER throws -- a caller (the RPC dispatcher) maps a false return
		//! to a JSON-RPC -32700 parse error.
		bool JsonParse( const std::string& text, JsonValue& out, std::string& outError );

		//! Serialize `v` to a compact (no-whitespace) RFC-8259 JSON string.
		//! Strings are escaped per RFC 8259 (quotes, backslash, the C0
		//! shorthands for backspace/formfeed/newline/return/tab, and other
		//! control chars as a u-hex escape; the solidus is emitted raw,
		//! which is legal).  Numbers use %.17g (lossless for doubles),
		//! collapsing an integral value to its integer form when exact.
		std::string JsonSerialize( const JsonValue& v );

		//! Append `s` as a JSON string LITERAL (surrounding quotes included),
		//! escaped per RFC 8259.  Exposed so SchemaGen (and any other Agent
		//! emitter) can share the ONE correct escaper rather than duplicate
		//! it.  This is the exact routine JsonSerialize uses for strings.
		void JsonAppendEscapedString( std::string& out, const std::string& s );
	}
}

#endif
