//////////////////////////////////////////////////////////////////////
//
//  AgentDiagnostic.h - one structured validation diagnostic (Facet 5,
//    the agentic surface -- slice 0a).
//
//    A single, localized problem produced by AgentSession::Validate.
//    This is the slice-0 seed of the richer `ValidationReport` the
//    design (docs/agentic-redesign/50-agentic-surface.md §2.5)
//    ultimately returns: severity + a stable machine-matchable `code`
//    + a human message + a best-effort byte span (offset/length) into
//    the validated text.  Node-path anchoring, line/column, and
//    suggestion/candidates ranking are LATER refinements -- this slice
//    lands the read/validate keystone with byte-offset localization only.
//
//    `offset`/`length` are byte offsets into the text that was validated
//    (the candidate `.RISEscene` string), and are 0/0 when the diagnostic
//    cannot be localized to a span (see AgentSession::Validate for the
//    honest limits of slice-0 localization -- the underlying DeriveToJob
//    refuse-all messages are coarse, so localization is a best-effort
//    re-derivation of WHICH token is at fault).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_AGENT_AGENTDIAGNOSTIC_
#define RISE_AGENT_AGENTDIAGNOSTIC_

#include <cstddef>
#include <string>

namespace RISE
{
	namespace Agent
	{
		//! One structured validation diagnostic.  See the file header for
		//! the offset/length contract (byte span into the validated text;
		//! 0/0 when not localizable).
		struct AgentDiagnostic
		{
			enum class Severity { Error, Warning, Info };

			Severity    severity = Severity::Error;
			std::string code;      //!< a stable machine-matchable code (see AgentDiagnosticCode)
			std::string message;   //!< the human-readable message (verbatim from the derive layer when coarse)
			std::size_t offset = 0;   //!< byte offset of the offending span into the validated text (0 when not localizable)
			std::size_t length = 0;   //!< byte length of the offending span (0 when not localizable)
		};

		//! Stable diagnostic codes -- the slice-0 subset of the design's
		//! §2.5 code set.  String constants (not an enum) so they match the
		//! JSON-RPC `code` field 1:1 when slice 0c adds the transport, and so
		//! a caller can compare without a lookup table.
		namespace AgentDiagnosticCode
		{
			//! The candidate text could not be parsed into a CST at all
			//! (malformed structure -- e.g. an unbalanced brace).
			static const char* const PARSE_ERROR       = "PARSE_ERROR";
			//! A chunk keyword is not a registered scene chunk type.
			static const char* const UNKNOWN_CHUNK     = "UNKNOWN_CHUNK";
			//! A parameter name is not declared on its chunk's descriptor.
			static const char* const UNKNOWN_PARAMETER = "UNKNOWN_PARAMETER";
			//! A parameter value is ill-typed for its declared kind (a
			//! non-numeric / non-finite value in a numeric slot, a value-less
			//! parameter line, etc.).
			static const char* const INVALID_VALUE     = "INVALID_VALUE";
			//! A derive-time (apply) failure not reducible to one of the
			//! above -- carried verbatim from DeriveToJob's diagnostics.
			static const char* const DERIVE_ERROR      = "DERIVE_ERROR";
		}
	}
}

#endif
