//////////////////////////////////////////////////////////////////////
//
//  AgentSession.cpp - the headless read/validate surface (see AgentSession.h).
//
//  VALIDATE localization -- honest slice-0 note
//  --------------------------------------------
//  DeriveToJob (src/Library/Cst/Cst.cpp) reports its refuse-all failures
//  as a `std::vector<std::string>` of COARSE messages:
//     * "unknown chunk type '<kw>'"                  (localizable: the chunk keyword,
//                                                     by item-walking to the Chunk
//                                                     whose role == <kw> -- its item
//                                                     start IS the keyword offset --
//                                                     then falling back to a
//                                                     candidateText string-search)
//     * "<kw>: value-less parameter '<name>'"        (localizable: the param name --
//                                                     a value-less line flattens into
//                                                     a BARE pname Token that is a
//                                                     DIRECT child of the Chunk, so we
//                                                     item-walk to the chunk base then
//                                                     scan its direct kids for that
//                                                     bare token -- OffsetOfBarePname,
//                                                     NOT OffsetOfParamName)
//     * "<kw>: invalid parameter(s) (see log)"       (COARSE -- the offending
//                                                     parameter NAME is written to
//                                                     the LOG, not the diag string)
//     * other apply-time messages                    (kept as DERIVE_ERROR)
//  So for the "invalid parameter(s)" case we RE-DERIVE the classification
//  ourselves against the live descriptor: for the named chunk we walk its
//  Param nodes and find the first param whose name is not declared on the
//  chunk's ChunkDescriptor (-> UNKNOWN_PARAMETER) and compute that name
//  token's byte span (OffsetOfParamName, which descends into Param kids);
//  if every name is declared it is an ill-typed VALUE (-> INVALID_VALUE)
//  and we localize the first numeric-kind param whose value is non-finite /
//  non-numeric.  When we cannot pin a span we return offset=length=0 with
//  the message intact.  This is a best-effort reconstruction -- structured,
//  node-localized diagnostics emitted BY DeriveToJob itself are a later
//  refinement (design §2.5 `nodePath`).
//
//////////////////////////////////////////////////////////////////////

#include "AgentSession.h"
#include "SchemaGen.h"
#include "InMemoryRasterizerOutput.h"

#include "../Cst/Cst.h"
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/IJob.h"
#include "../Interfaces/IRasterizer.h"
#include "../RISE_API.h"
#include "../SceneEditor/ChunkDescriptorRegistry.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Parsers/IAsciiChunkParser.h"
#include "../Utilities/RString.h"

#include <cctype>
#include <cmath>

namespace RISE
{
	namespace Agent
	{
		namespace
		{
			using RISE::Cst::Node;
			using RISE::Cst::NodeRef;
			using RISE::Cst::NodeKind;
			using RISE::Cst::Document;

			//! Serialize a green node's bytes (leaves carry text; internal
			//! nodes are the concatenation of their kids -- the same
			//! contract as Cst.cpp's internal Serialize, re-expressed here
			//! because that helper is not exported).
			void SerializeNode( const NodeRef& n, std::string& out )
			{
				if( !n ) return;
				if( n->kids.empty() ) out += n->text;
				else for( const auto& k : n->kids ) SerializeNode( k, out );
			}

			//! Byte width of a node's serialization.
			std::size_t NodeBytes( const NodeRef& n )
			{
				std::string s;
				SerializeNode( n, s );
				return s.size();
			}

			//! Is `s` a purely-numeric token (an int / float, optionally
			//! signed, with a decimal point / exponent)?  A NON-numeric
			//! token in a numeric slot is what makes a value INVALID_VALUE.
			bool LooksNumeric( const std::string& s )
			{
				if( s.empty() ) return false;
				char* end = nullptr;
				std::strtod( s.c_str(), &end );
				return end != nullptr && *end == '\0';
			}

			//! Is a numeric-kind ValueKind (one that the derive validator
			//! type-checks token-by-token)?
			bool IsNumericKind( ValueKind k )
			{
				switch( k ) {
					case ValueKind::UInt:
					case ValueKind::Double:
					case ValueKind::DoubleVec3:
					case ValueKind::DoubleVec4:
					case ValueKind::DoubleMat4:
						return true;
					default:
						return false;
				}
			}

			//! Look up a ParameterDescriptor by name on a chunk descriptor
			//! (null if not declared).
			const ParameterDescriptor* FindParam( const ChunkDescriptor& d, const std::string& name )
			{
				for( const auto& p : d.parameters )
					if( p.name == name ) return &p;
				return nullptr;
			}

			//! The keyword prefix of a "<kw>: ..." derive diagnostic (empty
			//! if the message has no "<kw>:" prefix).
			std::string KeywordPrefix( const std::string& msg )
			{
				std::size_t colon = msg.find( ':' );
				if( colon == std::string::npos ) return std::string();
				// The prefix must be a single token (a chunk keyword), so
				// reject a message whose pre-colon text contains a space
				// (those are free-form messages, not "<kw>: ...").
				std::string pre = msg.substr( 0, colon );
				if( pre.find( ' ' ) != std::string::npos ) return std::string();
				return pre;
			}

			//! Extract the single-quoted token from a message like
			//! "unknown chunk type 'foo'" or "...value-less parameter 'bar'"
			//! (empty if none).
			std::string QuotedToken( const std::string& msg )
			{
				std::size_t a = msg.find( '\'' );
				if( a == std::string::npos ) return std::string();
				std::size_t b = msg.find( '\'', a + 1 );
				if( b == std::string::npos ) return std::string();
				return msg.substr( a + 1, b - a - 1 );
			}

			//! Walk the document computing the ABSOLUTE byte offset at which
			//! each top-level item starts (so we can localize inside a chunk).
			//! Returns the items in order plus their start offsets.
			void CollectItems( const Document& doc,
			                   std::vector<NodeRef>& outItems,
			                   std::vector<std::size_t>& outStarts )
			{
				// Each item's absolute start is the running total of the
				// preceding items' serialized widths (the CST is lossless, so
				// the concatenation of item bytes IS the document text).  The
				// i-th item resolves via its NodeId position.
				const int n = RISE::Cst::DocItemCount( doc );
				std::size_t running = 0;
				for( int i = 0; i < n; ++i ) {
					RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
					NodeRef node;
					if( id ) node = RISE::Cst::DocResolveNodeId( doc, id );
					outItems.push_back( node );
					outStarts.push_back( running );
					running += node ? NodeBytes( node ) : 0;
				}
			}

			//! Compute the absolute byte offset of the FIRST Token child with
			//! role `role` whose text == `text`, inside `chunk` which begins
			//! at absolute offset `chunkStart`.  Returns true + fills
			//! outOffset/outLength on a hit.
			bool OffsetOfParamName( const NodeRef& chunk, std::size_t chunkStart,
			                        const std::string& pname,
			                        std::size_t& outOffset, std::size_t& outLength )
			{
				if( !chunk ) return false;
				std::size_t running = chunkStart;
				for( const auto& kid : chunk->kids ) {
					if( kid->kind == NodeKind::Param ) {
						// A Param's first Token child (role "pname") is the name.
						std::size_t inner = running;
						for( const auto& tk : kid->kids ) {
							if( tk->kind == NodeKind::Token && tk->role == "pname" ) {
								if( tk->text == pname ) {
									outOffset = inner;
									outLength = tk->text.size();
									return true;
								}
								break;   // only the first token is the pname
							}
							inner += NodeBytes( tk );
						}
					}
					running += NodeBytes( kid );
				}
				return false;
			}

			//! Compute the absolute byte offset of a value-less parameter's
			//! bare pname token inside `chunk` (which begins at absolute offset
			//! `chunkStart`).  A value-less line is flattened by ParseChunk into
			//! a BARE `NodeKind::Token` (role "pname") that is a DIRECT child of
			//! the Chunk -- NOT wrapped in a `NodeKind::Param` -- so
			//! OffsetOfParamName (which only descends into Param kids) can never
			//! see it.  We scan the chunk's direct kids for the first
			//! `kind==Token, role=="pname"` whose text == `pname`, accumulating
			//! the same serialized byte widths as OffsetOfParamName.  Returns
			//! true + fills outOffset/outLength on a hit.
			bool OffsetOfBarePname( const NodeRef& chunk, std::size_t chunkStart,
			                        const std::string& pname,
			                        std::size_t& outOffset, std::size_t& outLength )
			{
				if( !chunk ) return false;
				std::size_t running = chunkStart;
				for( const auto& kid : chunk->kids ) {
					if( kid->kind == NodeKind::Token && kid->role == "pname" && kid->text == pname ) {
						outOffset = running;
						outLength = kid->text.size();
						return true;
					}
					running += NodeBytes( kid );
				}
				return false;
			}

			//! Locate the unknown / ill-typed parameter inside a chunk named
			//! `keyword`.  Sets `outCode` to UNKNOWN_PARAMETER (an undeclared
			//! name) or INVALID_VALUE (a declared but ill-typed value), and
			//! fills the byte span when found.  Returns false when the chunk
			//! is absent or nothing could be pinned (caller keeps offset 0).
			bool LocalizeInvalidParam( const Document& doc, const std::string& keyword,
			                           std::string& outCode,
			                           std::size_t& outOffset, std::size_t& outLength )
			{
				const ChunkDescriptor* d = DescriptorForKeyword( String( keyword.c_str() ) );
				if( !d ) return false;

				std::vector<NodeRef> items;
				std::vector<std::size_t> starts;
				CollectItems( doc, items, starts );

				for( std::size_t i = 0; i < items.size(); ++i ) {
					const NodeRef& item = items[i];
					if( !item || item->kind != NodeKind::Chunk || item->role != keyword ) continue;

					// PASS 1: the first UNDECLARED parameter name.
					for( const auto& kid : item->kids ) {
						if( kid->kind != NodeKind::Param ) continue;
						std::string pname;
						for( const auto& tk : kid->kids )
							if( tk->kind == NodeKind::Token && tk->role == "pname" ) { pname = tk->text; break; }
						if( pname.empty() ) continue;
						if( !FindParam( *d, pname ) ) {
							outCode = AgentDiagnosticCode::UNKNOWN_PARAMETER;
							if( !OffsetOfParamName( item, starts[i], pname, outOffset, outLength ) ) {
								outOffset = 0; outLength = 0;
							}
							return true;
						}
					}

					// PASS 2: a DECLARED but ill-typed numeric value.
					for( const auto& kid : item->kids ) {
						if( kid->kind != NodeKind::Param ) continue;
						std::string pname;
						std::vector<std::string> values;
						for( const auto& tk : kid->kids ) {
							if( tk->kind != NodeKind::Token ) continue;
							if( tk->role == "pname" ) pname = tk->text;
							else if( tk->role == "pvalue" ) values.push_back( tk->text );
						}
						const ParameterDescriptor* p = pname.empty() ? nullptr : FindParam( *d, pname );
						if( !p || !IsNumericKind( p->kind ) ) continue;
						bool bad = values.empty();   // a numeric param needs a value
						for( const std::string& v : values ) {
							if( !LooksNumeric( v ) || !std::isfinite( std::strtod( v.c_str(), nullptr ) ) ) { bad = true; break; }
						}
						if( bad ) {
							outCode = AgentDiagnosticCode::INVALID_VALUE;
							if( !OffsetOfParamName( item, starts[i], pname, outOffset, outLength ) ) {
								outOffset = 0; outLength = 0;
							}
							return true;
						}
					}
				}
				return false;
			}
		}

		AgentSession::AgentSession( IJobPriv* job, bool owns )
			: mJob( job ), mOwnsJob( owns )
		{
		}

		AgentSession::~AgentSession()
		{
			if( mOwnsJob && mJob ) mJob->release();
			mJob = nullptr;
		}

		std::unique_ptr<AgentSession> AgentSession::LoadFromFile( const std::string& path )
		{
			IJobPriv* job = nullptr;
			if( !RISE_CreateJobPriv( &job ) || !job ) return nullptr;
			if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) {
				job->release();
				return nullptr;
			}
			return std::unique_ptr<AgentSession>( new AgentSession( job, /*owns=*/true ) );
		}

		std::unique_ptr<AgentSession> AgentSession::WrapJob( IJobPriv* job )
		{
			if( !job ) return nullptr;
			return std::unique_ptr<AgentSession>( new AgentSession( job, /*owns=*/false ) );
		}

		bool AgentSession::HasDocument() const
		{
			return mJob && mJob->HasRetainedCstDocument();
		}

		std::string AgentSession::ReadDocument() const
		{
			if( !mJob ) return std::string();
			const RISE::Cst::Document* doc = mJob->GetCstDocument();
			if( !doc ) return std::string();
			return RISE::Cst::SerializeCst( *doc );
		}

		std::string AgentSession::ReadSchema( const std::string& keyword ) const
		{
			if( keyword.empty() ) return SchemaGenAll();
			return SchemaGenForChunk( keyword );
		}

		std::vector<AgentDiagnostic> AgentSession::Validate( const std::string& candidateText ) const
		{
			std::vector<AgentDiagnostic> out;

			// (a) bytes -> CST.  ParseToCst is LOSSLESS and structurally
			// total (malformed bytes land as stray leaves, never a throw),
			// so a genuine PARSE_ERROR here is a round-trip failure -- a
			// defensive check that should never fire, but reported as
			// PARSE_ERROR if it ever does.
			Document candidateDoc = RISE::Cst::ParseToCst( candidateText );
			if( RISE::Cst::SerializeCst( candidateDoc ) != candidateText ) {
				AgentDiagnostic d;
				d.severity = AgentDiagnostic::Severity::Error;
				d.code     = AgentDiagnosticCode::PARSE_ERROR;
				d.message  = "candidate text did not round-trip through the CST parser (malformed structure)";
				out.push_back( d );
				return out;
			}

			// (b) derive into a THROWAWAY Job -- NEVER this session's mJob
			// (Validate has no side effects on the head).
			IJobPriv* throwaway = nullptr;
			if( !RISE_CreateJobPriv( &throwaway ) || !throwaway ) {
				AgentDiagnostic d;
				d.severity = AgentDiagnostic::Severity::Error;
				d.code     = AgentDiagnosticCode::DERIVE_ERROR;
				d.message  = "internal: could not create a throwaway Job for validation";
				out.push_back( d );
				return out;
			}

			std::vector<std::string> diags;
			RISE::Cst::DeriveToJob( candidateDoc, *throwaway, &diags );
			throwaway->release();
			throwaway = nullptr;

			// (c) map each coarse derive message -> a structured diagnostic
			// with best-effort byte-offset localization (see file header).
			for( const std::string& msg : diags ) {
				AgentDiagnostic d;
				d.severity = AgentDiagnostic::Severity::Error;
				d.message  = msg;
				d.offset   = 0;
				d.length   = 0;

				if( msg.rfind( "unknown chunk type", 0 ) == 0 ) {
					d.code = AgentDiagnosticCode::UNKNOWN_CHUNK;
					const std::string kw = QuotedToken( msg );
					// Localize the chunk keyword token.  A top-level item begins
					// exactly at its keyword (inter-item trivia is a SEPARATE
					// item), and a Chunk's first kid is the `kw` Token -- so the
					// item-walk gives the keyword's absolute offset directly,
					// avoiding candidateText.find(kw)'s substring-mislocalization
					// (kw appearing earlier as a substring, e.g. inside a name).
					// Fall back to the string-search only if the item-walk misses.
					if( !kw.empty() ) {
						std::vector<NodeRef> items;
						std::vector<std::size_t> starts;
						CollectItems( candidateDoc, items, starts );
						bool anchored = false;
						for( std::size_t i = 0; i < items.size(); ++i ) {
							if( items[i] && items[i]->kind == NodeKind::Chunk && items[i]->role == kw ) {
								d.offset = starts[i];
								d.length = ( !items[i]->kids.empty() && items[i]->kids.front() )
								           ? items[i]->kids.front()->text.size() : kw.size();
								anchored = true;
								break;
							}
						}
						if( !anchored ) {
							std::size_t p = candidateText.find( kw );
							if( p != std::string::npos ) { d.offset = p; d.length = kw.size(); }
						}
					}
				}
				else if( msg.find( "value-less parameter" ) != std::string::npos ) {
					d.code = AgentDiagnosticCode::INVALID_VALUE;
					const std::string kw    = KeywordPrefix( msg );
					const std::string pname = QuotedToken( msg );
					std::size_t off = 0, len = 0;
					// A value-less line is flattened into a BARE pname Token that
					// is a DIRECT child of the Chunk (not a Param), so we item-walk
					// to the offending chunk's absolute base offset and then scan
					// its direct kids for that bare pname (OffsetOfBarePname) --
					// OffsetOfParamName only descends into Param kids and would
					// never see it.  Genuine miss keeps the honest 0/0 fallback.
					if( !kw.empty() && !pname.empty() ) {
						std::vector<NodeRef> items;
						std::vector<std::size_t> starts;
						CollectItems( candidateDoc, items, starts );
						for( std::size_t i = 0; i < items.size(); ++i ) {
							if( items[i] && items[i]->kind == NodeKind::Chunk && items[i]->role == kw ) {
								if( OffsetOfBarePname( items[i], starts[i], pname, off, len ) ) {
									d.offset = off; d.length = len; break;
								}
							}
						}
					}
				}
				else if( msg.find( "invalid parameter(s)" ) != std::string::npos ) {
					// The offending name is in the LOG, not the message -- so
					// re-derive the classification (UNKNOWN_PARAMETER vs an
					// ill-typed INVALID_VALUE) + its span ourselves.
					const std::string kw = KeywordPrefix( msg );
					std::string code = AgentDiagnosticCode::DERIVE_ERROR;
					std::size_t off = 0, len = 0;
					if( !kw.empty() && LocalizeInvalidParam( candidateDoc, kw, code, off, len ) ) {
						d.code   = code;
						d.offset = off;
						d.length = len;
					} else {
						d.code = AgentDiagnosticCode::DERIVE_ERROR;
					}
				}
				else {
					// Any other apply-time message (unresolved reference, a
					// scene_variant / let / instance_array error, ...) is kept
					// verbatim under the generic DERIVE_ERROR code.
					d.code = AgentDiagnosticCode::DERIVE_ERROR;
				}

				out.push_back( d );
			}

			return out;
		}

		AgentPatchResult AgentSession::ProposePatch( const AgentSetPatch& patch )
		{
			AgentPatchResult r;

			// Guard: a Job not loaded via the CST path retains no Document, so
			// there is nothing to edit -- reject clearly rather than silently.
			if( !mJob || !mJob->HasRetainedCstDocument() ) {
				r.applied = false;
				r.rawCode = 0;
				r.message = "no retained CST Document -- ProposePatch needs a CST-loaded head";
				return r;
			}
			if( patch.target.empty() || patch.param.empty() || patch.value.empty() ) {
				r.applied = false;
				r.rawCode = 0;
				r.message = "target, param, and value must all be non-empty";
				return r;
			}

			// Route through the SAME call the GUI property panel makes.  It
			// mutates the retained Document (DocSetOrAddParamValue) and
			// re-derives the LIVE Job itself -- incremental fast path, or the
			// D2 full re-derive fallback -- so the head's derived Scene is
			// consistent with the mutated Document afterward.  We add NO extra
			// re-derive: ApplyCstParamEdit owns that (see Job.cpp
			// DeriveEditedCstDocument_).  `occ = 0` = the first (typically
			// only) occurrence of the param on that entity.
			const int code = mJob->ApplyCstParamEdit(
				patch.target.c_str(),
				patch.kind.empty() ? nullptr : patch.kind.c_str(),
				patch.param.c_str(),
				/*occ=*/0,
				patch.value.c_str() );

			r.rawCode = code;
			switch( code ) {
				case 1:
					r.applied = true;
					r.message = "applied incrementally (managers untouched)";
					break;
				case 2:
					r.applied = true;
					r.message = "applied via a full re-derive (Scene + managers were replaced)";
					break;
				case 3:
					// 2/3 are "rebind" codes: the Scene was replaced.  Code 3
					// means the re-derive ALSO emitted diagnostics -- the source
					// contract (Job.cpp DeriveEditedCstDocument_) treats 3 as
					// failure.  But the Document WAS mutated and the live Job
					// managers WERE replaced, so per the slice-0b contract we
					// fold it to applied=true and surface it as
					// applied-WITH-WARNING; `rawCode` (3) preserves the
					// distinction from a clean apply (1/2) for a caller that
					// cares.
					r.applied = true;
					r.message = "applied via a full re-derive, but the re-derive emitted diagnostics (see log)";
					break;
				case 0:
				default:
					r.applied = false;
					r.message = "edit rejected (entity/param not found or the edit would not derive) -- head unchanged";
					break;
			}
			return r;
		}

		AgentRenderResult AgentSession::Render( int samplesOverride )
		{
			AgentRenderResult res;

			if( !mJob ) {
				res.ok = false;
				res.message = "no head loaded";
				return res;
			}

			// A render MUST NOT mutate the retained Document.  The earlier
			// slice-0b draft honoured `samplesOverride` by routing it through
			// ProposePatch -> ApplyCstParamEdit, which permanently rewrote the
			// `samples` param in the retained CST (a subsequent ReadDocument
			// reflected it forever) -- a render silently editing the scene,
			// contradicting the design's non-mutating override model
			// (docs/agentic-redesign/50-agentic-surface.md §2.2.5's
			// ResolveEffectiveRenderConfig layers render overrides onto the
			// DerivedScene WITHOUT touching the scene).  The clean transient
			// path -- applying the override on the DERIVED/live rasterizer
			// after derive, before Rasterize() -- needs a sample-count setter
			// on IRasterizer, which does NOT exist in slice 0b (SetSampleCount
			// lives only on InteractivePelRasterizer, not on the general
			// interface GetRasterizer() returns).  So in slice 0b the
			// render-scoped sample override is NOT yet wired without mutating
			// the CST; it is deferred to the EffectiveRenderConfig layer.
			// `samplesOverride` is currently IGNORED -- the render uses the
			// authored sample count -- and the signature is retained only for
			// API stability.  ReadDocument() is guaranteed byte-identical
			// across a Render call.
			(void)samplesOverride;

			// Fetch and null-check the live rasterizer BEFORE any mutation:
			// RemoveRasterizerOutputs() unconditionally dereferences
			// pRasterizer (Job::RemoveRasterizerOutputs has no null guard), so
			// a head with no active rasterizer (or a re-derive that left it
			// null) would crash if we removed outputs first.
			IRasterizer* rast = mJob->GetRasterizer();
			if( !rast ) {
				res.ok = false;
				res.message = "no active rasterizer";
				return res;
			}

			// Swap in the in-memory sink (remove any authored file outputs so
			// a headless render does not touch the filesystem), render, read
			// the PNG bytes back, then release the sink.
			mJob->RemoveRasterizerOutputs();

			// `new` yields refcount 1 (our owning ref); AddRasterizerOutput
			// addrefs it (the rasterizer's `outs` keeps that ref until the
			// next RemoveRasterizerOutputs / rasterizer teardown).  We drop
			// OUR ref via safe_release(sink) below -- no extra addref here.
			InMemoryRasterizerOutput* sink = new InMemoryRasterizerOutput();
			rast->AddRasterizerOutput( sink );

			const bool rendered = mJob->Rasterize();
			if( !rendered || !sink->HasImage() ) {
				safe_release( sink );
				res.ok = false;
				res.message = rendered ? "render produced no image" : "render failed";
				return res;
			}

			res.width  = sink->Width();
			res.height = sink->Height();
			res.png    = sink->ToPng();
			sink->MeanChannels( res.meanR, res.meanG, res.meanB );
			res.ok     = !res.png.empty();
			res.message = res.ok ? "ok" : "PNG encode produced no bytes";

			// Cache for ReadImage() ONLY on a successful, non-empty encode --
			// a failed render must not wipe a prior good cache (ReadImage
			// documents "the LAST successful Render").
			if( res.ok ) {
				mLastPng = res.png;
			}

			safe_release( sink );
			return res;
		}

		std::vector<unsigned char> AgentSession::ReadImage() const
		{
			return mLastPng;
		}
	}
}
