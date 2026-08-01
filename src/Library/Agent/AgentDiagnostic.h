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
			//! The text holds NO scene chunk at all -- empty, whitespace, or
			//! comments only.  DeriveToJob has nothing to complain about in
			//! such a text, so without this an empty candidate came back with
			//! an EMPTY diagnostics array: a CLEAN verdict on a non-document,
			//! which reads to a model as "the scene I just wrote is fine".
			static const char* const EMPTY_DOCUMENT    = "EMPTY_DOCUMENT";
			//! Creative-richness P2.b (docs/agentic-redesign/73-creative-
			//! richness-design.md sec 9): the scene has enough
			//! standard_object chunks (>=3) to plausibly want a
			//! spatially-varying physical-scalar parameter, but no
			//! scalar_painter chunk exists anywhere in the document, so
			//! every physical-scalar slot that DOES exist is necessarily a
			//! flat constant.  Severity::Info -- ADVISORY, not a
			//! correctness problem; the message self-disarms for a
			//! deliberately flat/simple scene.  Fire condition is computed
			//! ONCE, shared with the render-result "DESIGN NOTE" (see
			//! AgentSession.cpp's ComputeDesignNoteConditionsFromDoc_).
			static const char* const DESIGN_SCALAR_PIPE_UNUSED    = "DESIGN_SCALAR_PIPE_UNUSED";
			//! Creative-richness P2.b: the scene has enough standard_object
			//! chunks (>=4) to plausibly want an advanced geometry form,
			//! but no sdf_geometry / sweep_geometry / displaced_geometry
			//! chunk exists anywhere in the document.  Severity::Info --
			//! same advisory/self-disarming convention as
			//! DESIGN_SCALAR_PIPE_UNUSED, and the same shared fire
			//! condition.
			static const char* const DESIGN_NO_ADVANCED_GEOMETRY  = "DESIGN_NO_ADVANCED_GEOMETRY";
			//! Crash-fix sibling (see LuminaryManager::AddToLuminaryList,
			//! src/Library/Rendering/LuminaryManager.cpp): an emissive material
			//! is bound to an object with no directly-owned geometry (e.g. a
			//! csg_object -- its shape comes from its two operand objects, not
			//! a single geometry chunk).  Such an object is silently skipped
			//! by NEE at render time (it still glows when hit directly).
			//! Severity::Warning -- a real functional gap, not a style
			//! suggestion, but the scene still renders (the render-side
			//! null-geometry candidate is refused, not dereferenced).
			static const char* const LUMINAIRE_NULL_GEOMETRY = "LUMINAIRE_NULL_GEOMETRY";
		}
	}
}

#endif
