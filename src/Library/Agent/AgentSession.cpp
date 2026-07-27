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

#include <type_traits>   // static_assert guarding the RISEPel == Rec709RGBPel decode assumption
#include "pch.h"
#include "AgentSession.h"
#include "SchemaGen.h"
#include "InMemoryRasterizerOutput.h"

#include "../Cst/Cst.h"
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/IJob.h"
#include "../Interfaces/IRasterizer.h"
#include "../Interfaces/IScenePriv.h"
#include "../Interfaces/IFilm.h"
#include "../Interfaces/IRasterImageReader.h"
#include "../Interfaces/ICamera.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/IObjectManager.h"   // Toolkit slice 3a (objectmap): enumerate scene objects for the identity registry
#include "../Interfaces/IObject.h"          // Toolkit slice 3a (objectmap): const IObject* registry key
#include "../Interfaces/IEnumCallback.h"    // Toolkit slice 3a (objectmap): EnumerateItemNames collector
#include "../Utilities/Color/ColorUtils.h"  // Toolkit slice 3a (objectmap): SRGBTransferFunctionInverse for the linear pre-image; transitively pulls in Color.h's COLOR_SPACE enum (external review P2 fix: resolved output colour space)
#include "../Painters/ExpressionEval.h"     // External review P2 fix: ExpressionProgram -- reuse the SAME public expr(...) evaluator Cst.cpp's derive-time resolver is built on, so ResolveBeautyDisplayTransform_ can resolve an expr(...)-valued `exposure` param instead of silently strtod'ing it to 0
#include "../Interfaces/IRasterImageReader.h"   // compare_to_reference: decode a registered reference PNG through RISE's OWN reader (brings RISEColor + COLOR_SPACE via Color.h)
#include "../Interfaces/IRasterImageWriter.h"   // compare_to_reference: encode the composite [render|reference|heatmap] diff PNG
#include "../Utilities/MemoryBuffer.h"          // compare_to_reference: Implementation::MemoryBuffer, the in-memory IWriteBuffer the composite PNG encodes into
#include "../Interfaces/ILog.h"   // P1-A: RenderOverrideRestoreGuard's defensive log-and-swallow
#include "../Rendering/FrameStore.h"   // offscreen isolation: FrameStoreIsolationGuard's private throwaway FrameStore
#include "../Rendering/Rasterizer.h"   // offscreen isolation: concrete GetFrameStore/SetFrameStore/AcceptsFrameStorePush (not on IRasterizer)
#include "../Rendering/InteractivePelRasterizer.h"   // Toolkit slice 2 (quality:"draft"): CreateInteractiveMaterialPreviewPipeline
#include "../Rendering/PathTracingPelRasterizer.h"       // review-p2d P1-2: light solo is a PathTracingIntegrator mechanism
#include "../Rendering/PathTracingSpectralRasterizer.h"  // review-p2d P1-2: spectral twin
#include "../Rendering/RayCaster.h"   // GUI render modes P2b (light solo): concrete RayCaster::SetSoloLightByName/ClearSoloLight
#include "../Rendering/PixelBasedRasterizerHelper.h"   // GUI render modes P2b (light solo): reach the production rasterizer's RayCaster, mirroring Job::SetActiveRasterizerRadianceScale
#include "../RISE_API.h"
#include "../SceneEditor/SceneEditController.h"   // Facet 5 slice 1b: LIVE-mode routing through the render-safe edit path
#include "../SceneEditor/CameraIntrospection.h"   // preview-render: ephemeral camera-pose override
#include "../SceneEditor/ChunkDescriptorRegistry.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Parsers/IAsciiChunkParser.h"
#include "../Parsers/ChunkParserRegistry.h"   // F5 S3 (actionable insert_chunk diagnostics): CreateAllChunkParsers -- AllChunkKeywords' near-miss keyword set
#include "../Utilities/RString.h"
#include "../Utilities/MemoryBuffer.h"
#include "../Utilities/FiniteMath.h"

#include <algorithm>   // Facet 5 slice S1: std::sort for the deterministic skills index
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>   // Facet 5 slice 1a: std::snprintf for the conflict message
#include <cstdlib>  // Facet 5 slice S1: std::getenv for the skills-root resolution
#include <stdexcept>  // Fix-round (offscreen isolation): ForTest_ThrowBeforeRasterize's std::runtime_error
#include <fstream>  // Facet 5 slice S1: read-only skill-file reads
#include <iterator> // Facet 5 slice S1: istreambuf_iterator for whole-file reads
#include <limits>
#include <mutex>    // F5 S3: std::once_flag for AllChunkKeywords' one-time cache (also already relied on by AgentSession.h's mAsyncCacheMutex)
#include <unordered_set>  // Toolkit slice 3a fix-round P2-1: objectmap palette byte-uniqueness set

// Facet 5 slice S1: directory enumeration for the skills index (the one
// place the Library scans a directory; read-only).
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>   // S1 review round 1: regular-file check for the skills index
#endif

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
							// This diagnostic consumes already-tokenised numeric
							// text, so materialise the parsed value before its
							// finiteness test; -ffast-math can fold ordinary FP
							// classification predicates away.
							const double dv = std::strtod( v.c_str(), nullptr );
							if( !LooksNumeric( v ) || !RISE::IsFiniteDouble( dv ) ) { bad = true; break; }
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

			//----------------------------------------------------------------
			// Facet 5 slice S1: read_skill helpers (all STATELESS; see the
			// ReadSkill doc in AgentSession.h for the root-resolution and
			// path-safety contract).
			//----------------------------------------------------------------

			//! The skills root, WITH a trailing slash.  First hit wins:
			//! $RISE_SKILLS_PATH -> $RISE_MEDIA_PATH + "skills/agent/" ->
			//! "./skills/agent/".
			std::string SkillsRoot()
			{
				const char* sp = std::getenv( "RISE_SKILLS_PATH" );
				if( sp && sp[0] ) {
					std::string r = sp;
					if( r[r.size()-1] != '/' && r[r.size()-1] != '\\' ) r += '/';
					return r;
				}
				const char* mp = std::getenv( "RISE_MEDIA_PATH" );
				if( mp && mp[0] ) {
					std::string r = mp;
					if( r[r.size()-1] != '/' && r[r.size()-1] != '\\' ) r += '/';
					return r + "skills/agent/";
				}
				return "./skills/agent/";
			}

			//! PATH SAFETY: a skill name must be a BARE filename component --
			//! reject any '/', '\\', or ".." so a hostile name can never
			//! traverse out of the skills root.  (The ".md" suffix is appended
			//! by the caller of this check, so only .md files are served.)
			bool IsSafeSkillName( const std::string& name )
			{
				if( name.empty() ) return false;
				if( name.find( '/' )  != std::string::npos ) return false;
				if( name.find( '\\' ) != std::string::npos ) return false;
				if( name.find( ".." ) != std::string::npos ) return false;
				return true;
			}

			//! Read a whole file (binary, read-only).  False when absent.
			bool ReadFileText( const std::string& path, std::string& out )
			{
				std::ifstream f( path.c_str(), std::ios::binary );
				if( !f ) return false;
				out.assign( std::istreambuf_iterator<char>( f ),
				            std::istreambuf_iterator<char>() );
				return true;
			}

			//! Parse the skill metadata header the index displays: the first
			//! line is "# <Title>", the second "> hook: <one-line hook>".
			//! Lenient on the hook (missing -> empty), so a malformed skill
			//! still indexes under its title/name rather than vanishing.
			void ParseSkillHeader( const std::string& markdown,
			                       std::string& outTitle, std::string& outHook )
			{
				outTitle.clear();
				outHook.clear();
				std::size_t pos = 0;
				int lineNo = 0;
				while( pos < markdown.size() && lineNo < 2 ) {
					std::size_t eol = markdown.find( '\n', pos );
					if( eol == std::string::npos ) eol = markdown.size();
					std::string line = markdown.substr( pos, eol - pos );
					if( !line.empty() && line[line.size()-1] == '\r' ) line.erase( line.size()-1 );
					if( lineNo == 0 && line.rfind( "# ", 0 ) == 0 )
						outTitle = line.substr( 2 );
					else if( lineNo == 1 && line.rfind( "> hook:", 0 ) == 0 ) {
						outHook = line.substr( 7 );
						while( !outHook.empty() && outHook[0] == ' ' ) outHook.erase( 0, 1 );
					}
					pos = eol + 1;
					++lineNo;
				}
			}

			//! The bare skill names (*.md, suffix stripped) under `root`,
			//! sorted byte-wise so the index order is deterministic across
			//! platforms and readdir orderings.  Only REGULAR files index
			//! (a directory / FIFO / socket / device named "*.md" is
			//! skipped).  SYMLINK-FOLLOW, honestly documented: the stat()
			//! check FOLLOWS symlinks, so a symlink resolving to a regular
			//! file still indexes -- deliberate, because this is a
			//! trusted-OPERATOR surface (the root comes from the operator's
			//! environment, never from agent input; the agent can only pick
			//! names off this index).  `outRootFound`, when non-null,
			//! reports whether the root DIRECTORY itself was reachable --
			//! distinguishing a missing skills root (miswired install) from
			//! a present-but-empty one (both return an empty list).
			std::vector<std::string> ListSkillNames( const std::string& root, bool* outRootFound = nullptr )
			{
				if( outRootFound ) *outRootFound = false;
				std::vector<std::string> names;
#ifdef _WIN32
				{
					const DWORD attr = GetFileAttributesA( root.c_str() );
					if( outRootFound )
						*outRootFound = ( attr != INVALID_FILE_ATTRIBUTES ) &&
						                ( ( attr & FILE_ATTRIBUTE_DIRECTORY ) != 0 );
				}
				WIN32_FIND_DATAA fd;
				HANDLE h = FindFirstFileA( ( root + "*.md" ).c_str(), &fd );
				if( h != INVALID_HANDLE_VALUE ) {
					do {
						if( !( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) )
							names.push_back( fd.cFileName );
					} while( FindNextFileA( h, &fd ) );
					FindClose( h );
				}
#else
				DIR* d = opendir( root.c_str() );
				if( d ) {
					if( outRootFound ) *outRootFound = true;
					while( struct dirent* e = readdir( d ) ) {
						// Regular files only (stat FOLLOWS symlinks; see the
						// doc note above on why that is deliberate).
						struct stat st;
						if( stat( ( root + e->d_name ).c_str(), &st ) != 0 ) continue;
						if( !S_ISREG( st.st_mode ) ) continue;
						names.push_back( e->d_name );
					}
					closedir( d );
				}
#endif
				std::vector<std::string> out;
				for( std::size_t i = 0; i < names.size(); ++i ) {
					const std::string& n = names[i];
					// Only *.md files (and never dotfiles / "." / "..").
					if( n.size() <= 3 || n[0] == '.' ) continue;
					if( n.compare( n.size() - 3, 3, ".md" ) != 0 ) continue;
					out.push_back( n.substr( 0, n.size() - 3 ) );
				}
				std::sort( out.begin(), out.end() );
				return out;
			}
		}

		AgentSkillResult AgentSession::ReadSkill( const std::string& name )
		{
			AgentSkillResult r;
			const std::string root = SkillsRoot();

			// Progressive disclosure, tier 1: no name -> the INDEX.
			if( name.empty() ) {
				bool rootFound = false;
				const std::vector<std::string> names = ListSkillNames( root, &rootFound );
				// An empty index is AMBIGUOUS without this: distinguish a
				// missing skills root (miswired install / wrong cwd) from a
				// present-but-empty directory (legitimately no skills).
				if( !rootFound )
					r.note = "skills root not found at '" + root + "' (set RISE_SKILLS_PATH or run from the repo root)";
				for( std::size_t i = 0; i < names.size(); ++i ) {
					std::string text;
					if( !ReadFileText( root + names[i] + ".md", text ) ) continue;
					AgentSkillEntry e;
					e.name = names[i];
					ParseSkillHeader( text, e.title, e.hook );
					if( e.title.empty() ) e.title = e.name;   // lenient: never a blank index row
					r.index.push_back( e );
				}
				r.ok = true;
				return r;
			}

			// Tier 2: a named fetch.  Path safety FIRST -- a rejected name
			// never touches the filesystem.
			if( !IsSafeSkillName( name ) ) {
				r.ok = false;
				r.error = "invalid skill name '" + name +
				          "': must be a bare skill name (no '/', '\\', or \"..\")";
				return r;
			}
			// MEMBERSHIP GATE (S1 review round 1): the name must be in the
			// LISTED index -- the fetchable set IS the listed set.  This one
			// check closes four edges at once: a dotfile skill would be
			// fetchable-but-unlisted; a DIRECTORY named '<x>.md' could be
			// opened; a FIFO would HANG the read on POSIX; a Windows device
			// name (CON / NUL) would resolve to a device.  None of those are
			// in ListSkillNames' regular-*.md-files index, so none reach
			// ReadFileText.
			const std::vector<std::string> listed = ListSkillNames( root );
			if( std::find( listed.begin(), listed.end(), name ) == listed.end() ) {
				r.ok = false;
				r.error = "unknown skill '" + name +
				          "' -- call read_skill with no name for the index";
				return r;
			}
			std::string text;
			if( !ReadFileText( root + name + ".md", text ) ) {
				r.ok = false;
				r.error = "unknown skill '" + name +
				          "' -- call read_skill with no name for the index";
				return r;
			}
			r.ok = true;
			r.name = name;
			r.markdown = text;
			return r;
		}

		AgentSession::AgentSession( IJobPriv* job, bool owns, AgentAuthority authority )
			: mJob( job ), mOwnsJob( owns ), mAuthority( authority )
		{
		}

		std::uint64_t AgentSession::RetainedPerceptionBytesLocked_() const
		{
			std::uint64_t total = 0;
			auto add = [&]( InMemoryRasterizerOutput* sink ) {
				if( !sink ) return;
				const std::uint64_t bytes = sink->GetPerceptionInfo().persistentBytes;
				const std::uint64_t room = std::numeric_limits<std::uint64_t>::max() - total;
				total += bytes <= room ? bytes : room;
			};
			add( mLastSink );
			for( const auto& lease : mActiveSinkReadLeases ) {
				if( lease.first != mLastSink ) add( lease.first );
			}
			return total;
		}

		void AgentSession::ReleaseSinkReadLease_( InMemoryRasterizerOutput* sink ) const
		{
			if( !sink ) return;
			bool drained = false;
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				auto it = mActiveSinkReadLeases.find( sink );
				if( it != mActiveSinkReadLeases.end() ) {
					if( it->second > 1 ) --it->second;
					else mActiveSinkReadLeases.erase( it );
				}
				// Keep release inside the same lock as erasure. A replacement render
				// can never snapshot "not leased" while this sidecar is still alive.
				safe_release( sink );
				drained = mActiveSinkReadLeases.empty();
			}
			if( drained ) mSinkReadLeaseCv.notify_all();
		}

		void AgentSession::ReleaseSinkReadEntrant_() const
		{
			bool drained = false;
			{
				// Serialize the zero transition with the destructor's CV predicate.
				// The entry increment must remain lock-free so a caller already at
				// the public method boundary cannot disappear behind this mutex.
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				const unsigned int prior = mSinkReadEntrants.fetch_sub(
					1, std::memory_order_acq_rel );
				drained = prior == 1;
			}
			if( drained ) mSinkReadLeaseCv.notify_all();
		}

		AgentSession::~AgentSession()
		{
			// Fix-round-1 P1-A: drain BEFORE touching any member -- the
			// controller-attached worker's RenderAsync closure captures a
			// raw `this` and may still be running RenderCore_ on it right
			// now.  DrainAsyncRender_ cancels (so the render aborts
			// promptly rather than running to completion) then blocks
			// until the controller reports the job no longer active, so by
			// the time we reach the lines below, no async render worker can be
			// mid-call into this object. Bounded image readers are independent
			// of that worker lifetime and are closed/drained below.
			DrainAsyncRender_();

			// Fix-round-1 P1-A: take mAsyncCacheMutex for the sink release
			// -- mLastSink is the SAME field the (now-drained, but let's be
			// precise about what "drained" guarantees) worker thread writes
			// under this lock at the tail of a successful async render (see
			// RenderCore_'s cache-population tail).  DrainAsyncRender_
			// above already guarantees no worker call into THIS object is
			// still in flight, so this lock is technically uncontended by
			// the time we get here -- but taking it anyway costs nothing
			// and removes any doubt for a future maintainer who changes the
			// drain's bound (e.g. a shorter timeout) without re-deriving
			// this invariant from scratch.
			std::function<void()> shutdownWaitHook;
			{
				std::unique_lock<std::mutex> cacheLk( mAsyncCacheMutex );
				// Counted image readers may still be queued on this mutex, or may
				// have dropped it while encoding under a registered sink lease. Close
				// admission, then keep the complete session state alive until BOTH
				// populations drain.
				mSinkReadsClosing = true;
				if( ( mSinkReadEntrants.load( std::memory_order_acquire ) != 0 ||
				      !mActiveSinkReadLeases.empty() ) &&
				    mSinkReadShutdownWaitHookForTest ) {
					shutdownWaitHook = mSinkReadShutdownWaitHookForTest;
				}
			}
			// Test coordination must not run under the cache mutex: it may need
			// to release or re-enter the reader whose lifetime is being tested.
			if( shutdownWaitHook ) shutdownWaitHook();
			{
				std::unique_lock<std::mutex> cacheLk( mAsyncCacheMutex );
				mSinkReadLeaseCv.wait( cacheLk,
					[this]() {
						return mSinkReadEntrants.load( std::memory_order_acquire ) == 0 &&
						       mActiveSinkReadLeases.empty();
					} );
				safe_release( mLastSink );
				mLastSink = nullptr;
			}
			if( mOwnsJob && mJob ) mJob->release();
			mJob = nullptr;
		}

		// Fix-round-1 P1-A / round-2 P1-1: see the header doc for the full
		// contract.  Reads (and, on eventual completion, clears)
		// mAsyncOutstandingJobId under mAsyncCacheMutex, then -- OUTSIDE
		// that lock (cancel/wait must not hold a lock the worker thread
		// might need in order to make progress toward completing) --
		// cancels and waits, UNBOUNDED, on the controller that was attached
		// AT THE TIME the async render was submitted.
		void AgentSession::DrainAsyncRender_( unsigned int chunkMs )
		{
			// Round-2 P1-1 test hook: ForTest_SetDrainChunkMs overrides the
			// per-chunk wait for this instance when nonzero -- see that
			// setter's doc.  Production callers never set it, so this is a
			// no-op there.
			if( mDrainChunkMsForTest != 0 ) chunkMs = mDrainChunkMsForTest;

			std::uint64_t outstandingId = 0;
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				outstandingId = mAsyncOutstandingJobId;
			}
			if( outstandingId == 0 ) return;   // nothing outstanding -- common case, cheap check

			// Fix-round-1 P1-A: mController is read here, OUTSIDE
			// mAsyncCacheMutex -- this method is called from ~AgentSession
			// (single-threaded teardown; nothing else touches mController by
			// then) and from AttachController (which is documented
			// single-caller / main-thread, same contract as the rest of this
			// class's non-Render surface -- see the class's original
			// "deliberately single-threaded" note, now narrowed by P3-a's
			// doc fix to spell out exactly which calls are cross-thread).
			SceneEditController* controllerAtSubmitTime = mController;
			if( !controllerAtSubmitTime ) {
				// No controller to ask (already detached some other way) --
				// there is nothing this method can drain against.  Clear the
				// stale id so a later drain doesn't keep retrying a job no
				// controller will ever report on.
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				mAsyncOutstandingJobId = 0;
				return;
			}

			// Round-2 P1-1: loop cancel-then-wait in `chunkMs` slices until
			// the wait ACTUALLY observes completion -- never proceed past
			// this call on a bare timeout.  In the overwhelmingly common
			// case this loop body runs exactly once: cancelling makes the
			// render abort at its next progress/block-boundary check
			// (see SceneEditController::CancelAgentRender_ + RenderCore_'s
			// progress-hook install), live-measured at 7-20ms in
			// AgentRenderAsyncTest.cpp's Stop()-during-a-render red-prove --
			// nowhere near one chunk.  The loop only iterates more than
			// once against a render that is itself ignoring cancellation
			// (a coarse-grained integrator checkpoint gap, a wedged OIDN
			// call) -- exactly the case this fix exists to make safe rather
			// than merely bounded: re-issuing the cancel costs nothing (it
			// is idempotent -- RequestCancel just (re-)sets a flag) and
			// escalating the log warning gives an operator a live signal
			// that teardown is genuinely blocked on a runaway render,
			// rather than silently proceeding into a use-after-free.
			unsigned int elapsedMs = 0;
			for( ;; ) {
				controllerAtSubmitTime->CancelAgentRender_();
				const bool completed = controllerAtSubmitTime->WaitForRenderJob(
					static_cast<SceneEditController::RenderJobId>( outstandingId ), chunkMs );
				if( completed ) break;

				elapsedMs += chunkMs;
				GlobalLog()->PrintEx( eLog_Warning,
					"AgentSession::DrainAsyncRender_: agent render (job %llu) ignoring cancellation for %ums; "
					"session teardown blocked (unbounded by design -- see DrainAsyncRender_'s doc).",
					static_cast<unsigned long long>( outstandingId ), elapsedMs );
			}

			// The wait observed genuine completion (never a bare timeout) --
			// clear the id so a second drain call (e.g. AttachController(nullptr)
			// followed by ~AgentSession) is a fast no-op rather than
			// re-issuing a cancel against a job the controller may have
			// already recycled the slot out from under.
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				if( mAsyncOutstandingJobId == outstandingId ) mAsyncOutstandingJobId = 0;
			}
		}

		std::unique_ptr<AgentSession> AgentSession::LoadFromFile( const std::string& path, AgentAuthority authority )
		{
			IJobPriv* job = nullptr;
			if( !RISE_CreateJobPriv( &job ) || !job ) return nullptr;
			if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) {
				job->release();
				return nullptr;
			}
			return std::unique_ptr<AgentSession>( new AgentSession( job, /*owns=*/true, authority ) );
		}

		std::unique_ptr<AgentSession> AgentSession::WrapJob( IJobPriv* job, AgentAuthority authority )
		{
			if( !job ) return nullptr;
			return std::unique_ptr<AgentSession>( new AgentSession( job, /*owns=*/false, authority ) );
		}

		void AgentSession::AttachController( SceneEditController* controller )
		{
			// Fix-round-1 P1-A: drain any outstanding async render against
			// the OLD controller BEFORE swapping mController -- a RenderAsync
			// closure captured `this` and (if still in flight) will call back
			// into RenderCore_'s cache-population tail regardless of what
			// mController points to by the time it finishes; draining first
			// means that closure has ALREADY completed (or been cancelled to
			// completion) before this session's notion of "which controller
			// is live" changes out from under it.
			DrainAsyncRender_();

			// BORROWED: no addref/release -- the caller owns the controller and
			// must outlive this session (or detach first).  Null detaches.
			mController = controller;
		}

		bool AgentSession::HasDocument() const
		{
			return mJob && mJob->HasRetainedCstDocument();
		}

		RISE::Cst::CstHeadVersion AgentSession::HeadVersion() const
		{
			// {0,0} when there is no wrapped Job -- the "no retained head" sentinel.
			return mJob ? mJob->GetCstHeadVersion() : RISE::Cst::CstHeadVersion{};
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
			// Thin forwarder to the stateless core: Validate references NO
			// member state, so the transport can validate a candidate with no
			// head loaded (no-head bootstrap) via ValidateText directly.
			return ValidateText( candidateText );
		}

		std::vector<AgentDiagnostic> AgentSession::ValidateText( const std::string& candidateText )
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

		namespace
		{
			//! Forward declaration: AttachParamEditRejectionIssues is DEFINED
			//! further down this file (alongside AnalyzeRejectedParamEdit and its
			//! insert_chunk/remove_chunk sibling analysers -- keeping the three
			//! actionable-rejection analysers + their message-builders together),
			//! but ProposePatch (right below) needs to call it from BOTH its LIVE
			//! and headless branches. Every `namespace { ... }` block at this
			//! scope in this translation unit reopens the SAME compiler-unique
			//! anonymous namespace, so a forward declaration here and the
			//! definition later in the file refer to the identical symbol.
			void AttachParamEditRejectionIssues( AgentPatchResult& r, const RISE::Cst::Document& doc,
			                                     const std::string& target, const std::string& kind,
			                                     const std::string& param, const std::string& value );
		}

		AgentPatchResult AgentSession::ProposePatch( const AgentSetPatch& patch )
		{
			AgentPatchResult r;

			// Secure-MCP slice 5a: the authority gate, enforced HERE -- not
			// trusted to any caller-side flag -- BEFORE the existing LIVE-mode
			// commit branch below.  This is the choke point that closes the
			// "external commits with no human gate" hole: an External-authority
			// session NEVER reaches ApplyAgentParamEdit / ApplyCstParamEditChecked
			// directly; it either STAGES (a live controller is attached, so
			// there is a queue + an Owner to resolve against) or is REFUSED
			// outright (headless -- nowhere to stage to).  An Owner-authority
			// session falls straight through, unchanged, to the existing
			// LIVE-mode / direct-Job behaviour below.
			if( mAuthority == AgentAuthority::External )
			{
				if( !mController )
				{
					r.applied = false;
					r.rawCode = 0;
					r.status  = "rejected";
					r.headVersion = HeadVersion();
					r.message = "propose_patch refused: this session is External-authority and no live "
					            "controller is attached -- staging needs a live Owner to resolve against";
					return r;
				}
				// S5a hardening: do NOT pre-read mJob->GetCstHeadVersion()
				// here -- that would race the controller's render thread /
				// a concurrent commit against the non-atomic 16-byte
				// CstHeadVersion, outside mMutex.  When the patch pins an
				// explicit baseVersion, hand it through untouched; otherwise
				// let StageProposal itself stamp the head-at-stage-time,
				// atomically, under its OWN mMutex hold (see its doc).
				SceneEditController::AgentProposal p;
				p.kind        = SceneEditController::AgentProposalKind::ParamEdit;
				p.target      = String( patch.target.c_str() );
				p.entityKind  = String( patch.kind.c_str() );
				p.param       = String( patch.param.c_str() );
				p.value       = String( patch.value.c_str() );
				p.hasExplicitBaseVersion = patch.hasBaseVersion;
				if( patch.hasBaseVersion ) p.baseVersion = patch.baseVersion;
				// Secure-MCP slice 5c: stamp this session's diagnostic label
				// (see SetSessionLabel's doc) -- "" for every pre-5c session,
				// byte-for-byte unchanged behaviour.
				p.sessionLabel = String( mSessionLabel.c_str() );
				RISE::Cst::CstHeadVersion stagedHead{};
				const std::uint64_t id = mController->StageProposal( p, &stagedHead );
				if( id == 0 )
				{
					// Secure-MCP slice 6: StageProposal refused -- the
					// attached controller's PENDING proposal queue is
					// already at kMaxPendingProposals. Nothing was
					// enqueued (stagedHead was left untouched by
					// StageProposal), so headVersion here reads the
					// CURRENT head fresh, same as every other permanent
					// reject in this function.
					r.applied    = false;
					r.rawCode    = 0;
					r.status     = "rejected";
					r.queueFull  = true;
					r.headVersion = HeadVersion();
					r.message = "propose_patch refused: the pending-proposal queue is full -- "
					            "the Owner must resolve (approve/reject) some pending proposals "
					            "before another can be staged";
					return r;
				}
				r.applied = false;
				r.rawCode = 0;
				r.status  = "staged";
				r.headVersion = stagedHead;
				char buf[128];
				std::snprintf( buf, sizeof( buf ), "proposal %llu staged (pending owner approval)",
					static_cast<unsigned long long>( id ) );
				r.message = buf;
				return r;
			}

			// Facet 5 slice 1b: LIVE mode.  When a controller is attached, the
			// session shares a Job with a running interactive editor, so the
			// commit MUST go through the controller's render-thread-SAFE edit
			// path (cancel-and-park + rebind-after-D2) rather than calling
			// Job::ApplyCstParamEdit directly (which would race the render
			// thread and dangle the editor's cached pointers on a D2).  The
			// controller does its OWN guards (no-Document / empty-field /
			// open-editor-transaction / conflict), so we delegate wholesale and
			// map its AgentCommitResult 1:1 onto AgentPatchResult -- the mapping
			// is identical to the direct-path switch below (same 0/1/2/3
			// folding, same conflict semantics) plus the controller-only
			// `retriable` flag (true only on the transaction refusal).  When NOT
			// attached (the default), fall through to the prior byte-for-byte
			// direct-Job behaviour.
			if( mController )
			{
				const RISE::Cst::CstHeadVersion* basePtr =
					patch.hasBaseVersion ? &patch.baseVersion : nullptr;
				const SceneEditController::AgentCommitResult cr =
					mController->ApplyAgentParamEdit(
						String( patch.target.c_str() ),
						String( patch.kind.c_str() ),
						String( patch.param.c_str() ),
						String( patch.value.c_str() ),
						basePtr );
				r.applied     = cr.applied;
				r.retriable   = cr.retriable;
				r.rawCode     = cr.rawCode;
				r.status      = cr.status.c_str();
				r.headVersion = cr.headVersion;
				r.message     = cr.message.c_str();
				// Actionable rejection diagnostics on the LIVE path -- THIS is the
				// GUI path (see AttachRejectionIssues' identical rationale on
				// InsertChunk's LIVE branch, the case that motivated this whole
				// mechanism: a local model cannot read the server log the engine's
				// own message points at).  Gated on the SAME generic rawCode==0
				// catch-all insert_chunk uses, so a conflict / queue-full / transient
				// open-editor-transaction refusal is never second-guessed.  A
				// rejection leaves the head UNCHANGED, so the retained Document is
				// the correct namespace to resolve the target/reference against.
				if( !r.applied && r.rawCode == 0 && mJob && mJob->GetCstDocument() )
					AttachParamEditRejectionIssues( r, *mJob->GetCstDocument(),
					                                patch.target, patch.kind, patch.param, patch.value );
				return r;
			}

			// Guard: a Job not loaded via the CST path retains no Document, so
			// there is nothing to edit -- reject clearly rather than silently.
			if( !mJob || !mJob->HasRetainedCstDocument() ) {
				r.applied = false;
				r.rawCode = 0;
				r.status  = "rejected";
				r.headVersion = HeadVersion();
				r.message = "no retained CST Document -- ProposePatch needs a CST-loaded head";
				return r;
			}
			if( patch.target.empty() || patch.param.empty() || patch.value.empty() ) {
				r.applied = false;
				r.rawCode = 0;
				r.status  = "rejected";
				r.headVersion = mJob->GetCstHeadVersion();
				r.message = "target, param, and value must all be non-empty";
				return r;
			}

			// Facet 5 slice 1a: the optimistic-concurrency CONFLICT precondition.
			// This runs BEFORE any mutation -- a stale patch must NEVER touch the
			// Document.  When the patch carries a base head-version and it does NOT
			// equal the Job's CURRENT head, the head moved since the agent read it,
			// so REJECT with a CONFLICT (head byte-identical) and hand back the
			// current head so the caller can re-read + re-propose.  Absent
			// baseVersion -> unconditional (slice-0 back-compat).
			if( patch.hasBaseVersion ) {
				const RISE::Cst::CstHeadVersion cur = mJob->GetCstHeadVersion();
				if( patch.baseVersion != cur ) {
					r.applied     = false;
					r.rawCode     = 0;
					r.status      = "conflict";
					r.headVersion = cur;
					char buf[160];
					std::snprintf( buf, sizeof( buf ),
						"baseHeadVersion does not match the current head (revision %llu) -- re-read and re-propose",
						static_cast<unsigned long long>( cur.revision ) );
					r.message = buf;
					return r;
				}
			}

			// Route through the CHECKED variant of the call the GUI property
			// panel makes (round-2 P1-A root gate): an agent patch may RETARGET
			// a reference, and the incremental fast path validates only against
			// the LIVE managers -- without the gate a retarget to an entity
			// declared LATER in the document commits a forward reference whose
			// bytes fail to reload (silent save-time data loss).  The checked
			// call dry-runs the FULL derive first and refuses (code 0, head
			// untouched) when the edited document no longer derives in order.
			// It mutates the retained Document (DocSetOrAddParamValue) and
			// re-derives the LIVE Job itself -- incremental fast path, or the
			// D2 full re-derive fallback -- so the head's derived Scene is
			// consistent with the mutated Document afterward.  We add NO extra
			// re-derive: the Job owns that (see Job.cpp
			// DeriveEditedCstDocument_).  `occ = 0` = the first (typically
			// only) occurrence of the param on that entity.
			const int code = mJob->ApplyCstParamEditChecked(
				patch.target.c_str(),
				patch.kind.empty() ? nullptr : patch.kind.c_str(),
				patch.param.c_str(),
				/*occ=*/0,
				patch.value.c_str() );

			r.rawCode = code;
			switch( code ) {
				case 1:
					r.applied = true;
					r.status  = "applied";
					r.message = "applied incrementally (managers untouched)";
					break;
				case 2:
					r.applied = true;
					r.status  = "applied";
					r.message = "applied via a full re-derive (Scene + managers were replaced)";
					break;
				case 3:
					// Code 3 is a "rebind" code (the Scene + managers WERE
					// replaced) BUT the re-derive ALSO emitted diagnostics.  The
					// source contract (Job.cpp DeriveEditedCstDocument_) is
					// explicit: 3 means "the edit FAILED -- treat as failure".
					// So `applied` is FALSE (NOT a clean success -- a caller
					// gating on applied==true must not proceed) and `status` is
					// "diagnosed", the tri-state's non-success-but-not-a-reject
					// value.  Crucially this is NOT a byte-identical reject: the
					// Document WAS mutated and the live managers WERE replaced,
					// so the message says so plainly -- a caller must neither
					// treat it as a clean apply NOR assume nothing changed.
					r.applied = false;
					r.status  = "diagnosed";
					r.message = "edit NOT a clean success: the Document was mutated and the live managers were "
					            "replaced, BUT the full re-derive emitted diagnostics (see log) -- do NOT treat as applied";
					break;
				case 0:
				default:
					// 0 = clean reject (head byte-identical).  Any unexpected
					// code also lands here: pick the SAFE non-success -- reject
					// with an unchanged head is the conservative reading (an
					// unknown code should never claim a clean apply).  rawCode
					// preserves whatever the underlying call returned.
					r.applied = false;
					r.status  = "rejected";
					r.message = "edit rejected (entity/param not found or the edit would not derive) -- head unchanged";
					break;
			}
			// Facet 5 slice 1a: carry the head-version AFTER the ApplyCstParamEdit -- the POST-COMMIT head on a
			// clean apply (its revision bumped by the Job's commit path), and the UNCHANGED current head on a
			// reject (code 0) / diagnosed (code 3, where the Document WAS mutated so its revision also bumped).
			r.headVersion = mJob->GetCstHeadVersion();
			// Model-B: the pre-flight CAUSE analysis for a REJECTED param edit --
			// ONLY for literal code 0 (ApplyCstParamEditChecked's generic "would
			// not derive" catch-all; there is no -1/-2 class here the way chunk
			// CRUD has, so code==0 IS the whole reject bucket).  `mJob->GetCstDocument()`
			// here is the SAME, UNCHANGED head the failed dry-run ran against (a
			// reject never mutates), so this is a faithful re-check, not a stale one.
			if( code == 0 && mJob->GetCstDocument() )
				AttachParamEditRejectionIssues( r, *mJob->GetCstDocument(),
				                                patch.target, patch.kind, patch.param, patch.value );
			return r;
		}

		namespace
		{
			//! Model-B F5 slice S2: fold a chunk-CRUD code from the DIRECT
			//! (headless) Job path into the result.  The controller path has
			//! its own identical fold (SceneEditController::ApplyAgentChunkCrud_)
			//! -- kept separate because the layers may not depend on each
			//! other's result types (the dependency runs Agent -> SceneEditor
			//! only, and SceneEditor cannot see AgentChunkResult).
			void FoldChunkCode( AgentChunkResult& r, int code, bool isInsert,
			                    const std::string& target, const char* diag,
			                    bool kindWasPassed )
			{
				r.rawCode = ( code < 0 ) ? 0 : code;
				switch( code ) {
					case 2:
						r.applied = true;
						r.status  = "applied";
						r.message = isInsert
							? "chunk inserted via a full re-derive (Scene + managers were replaced)"
							: "chunk removed via a full re-derive (Scene + managers were replaced)";
						break;
					case 3:
						r.applied = false;
						r.status  = "diagnosed";
						r.message = "edit NOT a clean success: the Document was mutated and the live managers were "
						            "replaced, BUT the full re-derive emitted diagnostics (see log) -- do NOT treat as applied";
						break;
					case -1:
						r.applied = false;
						r.status  = "rejected";
						if( isInsert ) {
							r.message = "insert rejected: chunkText must parse to exactly ONE complete chunk "
							            "(`keyword { ... }`, braces on their own lines; no scene header/directives)";
							if( diag && diag[0] ) { r.message += ": "; r.message += diag; }
						} else if( diag && diag[0] ) {
							// A diagnosed -1 (e.g. the kind-verification refusal:
							// the name resolved but to a DIFFERENT kind) carries
							// its own specific reason -- surface it verbatim.
							r.message = std::string( "remove rejected: " ) + diag + " -- head unchanged";
						} else {
							r.message = "remove rejected: no chunk named '" + target + "' found -- head unchanged";
						}
						break;
					case -2:
						r.applied = false;
						r.status  = "rejected";
						if( isInsert ) {
							// Round-3: a "reserved name"-prefixed diag (Job's `name none`
							// refusal) is NOT a chunk collision -- surface the real cause
							// verbatim instead of the misleading "already exists" claim.
							// Prefix kept in lockstep with Job::ApplyCstInsertChunk.
							const std::string dstr = ( diag && diag[0] ) ? diag : "";
							if( dstr.compare( 0, 13, "reserved name" ) == 0 ) {
								r.message = "insert rejected: " + dstr + " -- head unchanged";
								break;
							}
							r.message = "insert rejected: a chunk with the same kind and name already exists";
							if( diag && diag[0] ) { r.message += " ("; r.message += diag; r.message += ")"; }
							r.message += " -- head unchanged";
						} else {
							// Round-2 P3: conditional hint -- "pass `kind`" is a
							// dead-end instruction when kind WAS passed.
							r.message = "remove rejected: name '" + target + "'";
							r.message += kindWasPassed
								? " is ambiguous even under that kind -- pass a more specific kind"
								: " is ambiguous -- pass `kind` to narrow";
							if( diag && diag[0] ) { r.message += " ("; r.message += diag; r.message += ")"; }
						}
						break;
					case 0:
					default:
						r.applied = false;
						r.status  = "rejected";
						// Round-2 P1-A: name BOTH would-not-derive causes honestly
						// (the old "likely still REFERENCED" wording hid the
						// order-invalid-head cause).
						r.message = isInsert
							? std::string( "insert rejected: the chunk would not derive in context -- head unchanged" )
							: "remove rejected: removing '" + target + "' would not derive (it is likely still REFERENCED by another chunk, or the remaining document no longer derives in order -- read_document and validate to inspect) -- head unchanged";
						if( diag && diag[0] ) { r.message += ": "; r.message += diag; }
						break;
				}
			}
		}

		namespace
		{
			//! Case-insensitive lowercasing (ASCII -- every chunk/painter/material
			//! name in practice is an ASCII identifier) -- shared basis for the two
			//! near-miss tests CollectUnresolvedRefSuggestions uses below.
			std::string ToLowerCopy( const std::string& s )
			{
				std::string out = s;
				for( char& c : out ) c = (char)std::tolower( (unsigned char)c );
				return out;
			}

			//! Case-insensitive "is `needle` a substring of `hay`?" An empty needle
			//! never matches (an empty candidate name can't be a meaningful near-miss
			//! of anything).
			bool CiContains( const std::string& hay, const std::string& needle )
			{
				if( needle.empty() ) return false;
				return ToLowerCopy( hay ).find( ToLowerCopy( needle ) ) != std::string::npos;
			}

			//! Case-insensitive Levenshtein edit distance -- O(len(a).len(b)) DP,
			//! cheap at chunk-name lengths. The suggestion builder's typo-distance
			//! fallback for near-misses the substring test above misses (e.g.
			//! "wall_pnik" vs "wall_pink" shares no useful substring either way).
			int CiEditDistance( const std::string& a, const std::string& b )
			{
				const std::string la = ToLowerCopy( a ), lb = ToLowerCopy( b );
				const std::size_t n = la.size(), m = lb.size();
				std::vector<std::vector<int> > dp( n + 1, std::vector<int>( m + 1, 0 ) );
				for( std::size_t i = 0; i <= n; ++i ) dp[i][0] = (int)i;
				for( std::size_t j = 0; j <= m; ++j ) dp[0][j] = (int)j;
				for( std::size_t i = 1; i <= n; ++i )
					for( std::size_t j = 1; j <= m; ++j ) {
						const int sub = dp[i-1][j-1] + ( la[i-1] == lb[j-1] ? 0 : 1 );
						const int del = dp[i-1][j] + 1;
						const int ins = dp[i][j-1] + 1;
						dp[i][j] = std::min( sub, std::min( del, ins ) );
					}
				return dp[n][m];
			}

			//! THE shared near-miss ranking core -- every near-miss suggestion list
			//! in this file (a dangling reference's candidate chunk names, an
			//! unknown chunk keyword's candidate registered keywords) funnels
			//! through this ONE function, so there is exactly one typo-distance
			//! policy to keep in sync, not several. Ranks `candidates` against
			//! `value` by EITHER test: (1) a case-insensitive substring either
			//! direction -- ranks FIRST, catches the motivating "right name, wrong
			//! prefix/suffix" bug class (e.g. "_wall_pink" inside
			//! "uniform_wall_pink"); (2) a small case-insensitive edit distance
			//! (budget scales with name length) -- ranks SECOND, catches
			//! single-character typos substring matching would miss. Returns up
			//! to 3, best match first. A candidate identical to `value` is never
			//! suggested (it would have RESOLVED, so it can't be the fix for a
			//! dangling value / can't be what a caller mistyped AWAY from). This is
			//! a best-effort HEURISTIC ranking, not a claim the top suggestion is
			//! what the author meant -- it's a hint, and the caller still has to
			//! look.
			std::vector<std::string> RankNearMisses( const std::vector<std::string>& candidates,
			                                          const std::string& value )
			{
				// (rank, tiebreak) -- rank 0 = substring hit (tiebreak = -overlap
				// length, so a LONGER shared substring sorts first), rank 1 =
				// edit-distance hit (tiebreak = the distance itself, so a SMALLER
				// distance sorts first). A plain std::pair<int,int> sorts
				// lexicographically, which is exactly this priority order.
				std::vector<std::pair<std::pair<int,int>, std::string> > ranked;
				for( const std::string& name : candidates ) {
					if( name == value ) continue;
					if( CiContains( value, name ) || CiContains( name, value ) ) {
						ranked.push_back( std::make_pair(
							std::make_pair( 0, -(int)std::min( name.size(), value.size() ) ), name ) );
						continue;
					}
					const int dist = CiEditDistance( name, value );
					const int budget = std::max<int>( 2, (int)( 0.34 * std::max( name.size(), value.size() ) ) );
					if( dist <= budget )
						ranked.push_back( std::make_pair( std::make_pair( 1, dist ), name ) );
				}
				std::sort( ranked.begin(), ranked.end() );
				std::vector<std::string> out;
				for( const std::pair<std::pair<int,int>, std::string>& c : ranked ) {
					if( out.size() >= 3 ) break;
					out.push_back( c.second );
				}
				return out;
			}

			//! Up to 3 near-miss candidate names for a dangling reference (motivating
			//! case: chunkKeyword="lambertian_material", param="reflectance",
			//! value="uniform_wall_pink", and the document has a
			//! `uniformcolor_painter` named "_wall_pink" -- MUST surface "_wall_pink").
			//! A candidate is any chunk DEFINED in `doc` whose OWN
			//! ChunkDescriptor::category is one of the offending param's declared
			//! `referenceCategories` (so e.g. a Material name is never suggested for a
			//! Painter slot); the actual near-miss test is RankNearMisses above.
			std::vector<std::string> CollectUnresolvedRefSuggestions(
				const RISE::Cst::Document& doc, const std::string& chunkKeyword,
				const std::string& param, const std::string& value )
			{
				std::vector<std::string> out;
				const ChunkDescriptor* srcDesc = DescriptorForKeyword( String( chunkKeyword.c_str() ) );
				if( !srcDesc ) return out;
				const ParameterDescriptor* pd = nullptr;
				for( const ParameterDescriptor& p : srcDesc->parameters ) if( p.name == param ) { pd = &p; break; }
				if( !pd || pd->referenceCategories.empty() ) return out;

				std::vector<std::string> candidates;
				const int nItems = RISE::Cst::DocItemCount( doc );
				for( int i = 0; i < nItems; ++i ) {
					const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
					const RISE::Cst::NodeRef item = RISE::Cst::DocResolveNodeId( doc, id );
					if( !item || item->kind != RISE::Cst::NodeKind::Chunk ) continue;
					const std::string namePath = RISE::Cst::ChunkNamePath( item );   // "keyword/name", "" if unnamed
					if( namePath.empty() ) continue;
					const std::string prefix = item->role + "/";
					if( namePath.size() <= prefix.size() || namePath.compare( 0, prefix.size(), prefix ) != 0 ) continue;
					const std::string name = namePath.substr( prefix.size() );

					const ChunkDescriptor* candDesc = DescriptorForKeyword( String( item->role.c_str() ) );
					if( !candDesc ) continue;
					bool categoryOk = false;
					for( ChunkCategory cc : pd->referenceCategories ) if( cc == candDesc->category ) { categoryOk = true; break; }
					if( !categoryOk ) continue;

					candidates.push_back( name );
				}
				return RankNearMisses( candidates, value );
			}

			//! The bare keyword of every registered chunk type (cached once --
			//! CreateAllChunkParsers() constructs a full parser instance per
			//! keyword, so this is not something to call on every request). Used
			//! ONLY for near-miss suggestions on an "unknown_chunk_type" issue;
			//! DescriptorForKeyword (ChunkDescriptorRegistry.cpp) is the canonical
			//! per-keyword lookup everywhere else -- this is a second, independent
			//! call to the SAME CreateAllChunkParsers() factory (not a second
			//! registry) purely to enumerate the keyword set that lookup doesn't
			//! expose.
			const std::vector<std::string>& AllChunkKeywords()
			{
				static std::vector<std::string> keywords;
				static std::once_flag once;
				std::call_once( once, [] {
					for( const RISE::ChunkParserEntry& e : RISE::CreateAllChunkParsers() )
						if( e.parser ) keywords.push_back( e.keyword );
				} );
				return keywords;
			}

			//! Near-miss declared parameter names for an UNDECLARED param name on
			//! `d` (the "unknown_param" issue's suggestions). `name` (every
			//! chunk's own identity param, never a plausible typo target for an
			//! unrelated param) is excluded from the candidate set.
			//!
			//! FALLBACK: when the typo reads as a WHOLLY DIFFERENT WORD from every
			//! declared name (no substring / edit-distance near-miss at all -- the
			//! motivating case: `constant` typed for the real param `value` on
			//! `scalar_painter`, which share no meaningful substring or small edit
			//! distance), RankNearMisses legitimately returns empty -- but an
			//! empty suggestions list leaves the author with nothing to act on.
			//! Fall back to the chunk's COMPLETE declared parameter list in that
			//! case (chunk descriptors have at most a few dozen parameters, so
			//! this stays a genuinely useful, boundedly-sized answer, and the
			//! call site's `message` ALSO spells out this same list in prose --
			//! see AnalyzeRejectedInsert -- so this is not the only place an
			//! author can find it).
			std::vector<std::string> NearMissParamNames( const ChunkDescriptor& d, const std::string& badName )
			{
				std::vector<std::string> candidates;
				for( const ParameterDescriptor& p : d.parameters )
					if( p.name != "name" ) candidates.push_back( p.name );
				std::vector<std::string> ranked = RankNearMisses( candidates, badName );
				if( !ranked.empty() ) return ranked;
				return candidates;   // fallback -- see the doc above
			}

			//! Are ALL of `toks` individually numeric (RISE::Agent's LooksNumeric,
			//! single-token strtod-consumes-the-whole-string test above in this
			//! file)? Mirrors Cst.cpp's own (file-local, unexported) `LooksNumeric`
			//! "entirely numeric tokens" rule -- a scalar (`0.5`) or a whitespace
			//! tuple of scalars (`1 2 3`) -- so this layer's notion of "that's a
			//! literal, not a name" agrees with the derive layer's. Empty input is
			//! NOT all-numeric (mirrors Cst.cpp's `any` guard: a value-less param
			//! is a DIFFERENT, already-diagnosed derive failure, not a numeric
			//! literal).
			bool AllTokensNumeric( const std::vector<std::string>& toks )
			{
				if( toks.empty() ) return false;
				for( const std::string& t : toks ) if( !LooksNumeric( t ) ) return false;
				return true;
			}

			//! Split `s` on ASCII whitespace into non-empty tokens.  Needed by
			//! the propose_patch rejection analyser below: unlike an
			//! insert_chunk candidate (already tokenized by the CST parser into
			//! discrete `pvalue` Tokens -- see AnalyzeRejectedInsert's
			//! valueTokens loop), propose_patch's `value` arrives as a single
			//! raw string over the wire, so it needs its own split before it can
			//! be run through the SAME AllTokensNumeric test insert_chunk uses
			//! (reused, not reimplemented -- see AnalyzeRejectedParamEdit).
			std::vector<std::string> SplitWhitespace( const std::string& s )
			{
				std::vector<std::string> out;
				std::string cur;
				for( char c : s ) {
					if( std::isspace( static_cast<unsigned char>( c ) ) ) {
						if( !cur.empty() ) { out.push_back( cur ); cur.clear(); }
					} else {
						cur += c;
					}
				}
				if( !cur.empty() ) out.push_back( cur );
				return out;
			}

			//! Is `role` (a chunk's own keyword) an instance of `kind` -- exact
			//! match, or the same "ends in `_<kind>`" suffix rule
			//! DocFindByNameAnyRole's `roleKindSuffix` narrowing and
			//! Job::ApplyCstRemoveChunk's post-resolve kind-verification both
			//! already apply (e.g. `kind="material"` matches both the bare
			//! `material` keyword and every `..._material` keyword). A SEPARATE
			//! small copy of that one-line rule rather than a shared helper --
			//! consistent with this file's existing practice for a rule this
			//! short (see CategoryNameLower's doc).
			bool RoleMatchesKind( const std::string& role, const std::string& kind )
			{
				if( kind.empty() ) return true;
				if( role == kind ) return true;
				return role.size() > kind.size() + 1 &&
					role.compare( role.size() - kind.size() - 1, kind.size() + 1, "_" + kind ) == 0;
			}

			//! Near-miss candidate chunk NAMES for an unresolved propose_patch
			//! `target` address -- every NAMED chunk in `doc`, restricted (via
			//! RoleMatchesKind) to chunks of `kind` when a kind hint was given.
			//! Mirrors CollectUnresolvedRefSuggestions' candidate-gathering loop
			//! above, but keyed on KIND rather than on a param's declared
			//! referenceCategories (a propose_patch target has no such param to
			//! consult -- the caller IS the "which chunk did you mean" question).
			std::vector<std::string> CollectTargetNameCandidates( const RISE::Cst::Document& doc,
			                                                       const std::string& kind )
			{
				std::vector<std::string> candidates;
				const int n = RISE::Cst::DocItemCount( doc );
				for( int i = 0; i < n; ++i ) {
					const RISE::Cst::NodeRef item = RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
					if( !item || item->kind != NodeKind::Chunk ) continue;
					if( !RoleMatchesKind( item->role, kind ) ) continue;
					const std::string namePath = RISE::Cst::ChunkNamePath( item );
					if( namePath.empty() ) continue;
					const std::string prefix = item->role + "/";
					if( namePath.size() <= prefix.size() || namePath.compare( 0, prefix.size(), prefix ) != 0 ) continue;
					candidates.push_back( namePath.substr( prefix.size() ) );
				}
				return candidates;
			}

			//! A stable lowercase name for a ChunkCategory, for the
			//! "numeric_in_reference_slot" fix-shape message (e.g. "a painter").
			//! Deliberately a SEPARATE small copy from SchemaGen.cpp's identical
			//! table (that one is a file-local anonymous-namespace helper in a
			//! different translation unit, not exported) rather than a shared
			//! header -- consistent with this file's existing practice of small
			//! local re-derivations over cross-TU coupling for a two-line switch
			//! (see e.g. this file's own SerializeNode).
			const char* CategoryNameLower( ChunkCategory c )
			{
				switch( c ) {
					case ChunkCategory::Painter:          return "painter";
					case ChunkCategory::Function:         return "function";
					case ChunkCategory::Material:         return "material";
					case ChunkCategory::Camera:           return "camera";
					case ChunkCategory::Film:              return "film";
					case ChunkCategory::Geometry:         return "geometry";
					case ChunkCategory::Modifier:         return "modifier";
					case ChunkCategory::Medium:            return "medium";
					case ChunkCategory::Object:            return "object";
					case ChunkCategory::ShaderOp:          return "shaderop";
					case ChunkCategory::Shader:             return "shader";
					case ChunkCategory::Rasterizer:        return "rasterizer";
					case ChunkCategory::RasterizerOutput:  return "rasterizer_output";
					case ChunkCategory::Light:              return "light";
					case ChunkCategory::PhotonMap:          return "photon_map";
					case ChunkCategory::PhotonGather:       return "photon_gather";
					case ChunkCategory::IrradianceCache:    return "irradiance_cache";
					case ChunkCategory::Animation:           return "animation";
					case ChunkCategory::SceneVariant:        return "scene_variant";
				}
				return "chunk";
			}

			//! Post-process a SUCCESSFUL insert_chunk: re-derive the reference graph
			//! over the JUST-LANDED head and attribute any dangling reference to the
			//! chunk THIS call inserted, filling r.issues (reason "unresolved_reference",
			//! with near-miss suggestions per offending param) and appending a
			//! one-sentence note to r.message. NEVER touches r.applied/r.status --
			//! see AgentChunkIssue's doc for why a forward reference (the painter
			//! comes in a later call) must stay a WARNING, never a rejection:
			//! refusing it would make normal declare-after-use scene building
			//! impossible. No-op unless `r.applied` is already true -- InsertChunk's
			//! two success paths (LIVE-controller and headless) both call this at
			//! their own success point, after r.kind/r.name are filled in from the
			//! commit.
			//!
			//! ATTRIBUTION: BuildReferenceGraph's structured out-param reports EVERY
			//! dangling reference in the WHOLE document, not just this insert's -- a
			//! caller must filter to the chunk it just landed (the SCOPING
			//! requirement: an unrelated pre-existing dangling reference elsewhere
			//! must not leak into this insert's report). The precise filter is by
			//! NodeId, via DocFindByName( doc, "<kind>/<name>" ) -- guaranteed UNIQUE
			//! here because a successful insert can never have landed a (kind,name)
			//! duplicate (that collision is refused before commit, see
			//! TestInsertRejections). FAILURE MODE: r.name can be empty for an
			//! UNNAMED chunk (e.g. `film`) -- DocFindByName has nothing to look up
			//! then, so this falls back to a best-effort match on chunkKeyword PLUS
			//! both the param name and the dangling value appearing verbatim in the
			//! raw chunkText. That fallback is heuristic -- a coincidental substring
			//! hit could mis-attribute -- but it's moot in practice: every unnamed
			//! chunk descriptor in this codebase has zero Reference-kind params, so an
			//! unnamed chunk never has a dangling reference of its own to report.
			void AttachChunkIssueWarnings( AgentChunkResult& r, const RISE::Cst::Document& doc,
			                                const std::string& chunkText )
			{
				if( !r.applied ) return;
				std::vector<RISE::Cst::UnresolvedReference> all;
				RISE::Cst::BuildReferenceGraph( doc, nullptr, &all );
				if( all.empty() ) return;

				RISE::Cst::NodeId insertedId = 0;
				if( !r.name.empty() )
					insertedId = RISE::Cst::DocFindByName( doc, r.kind + "/" + r.name );

				std::vector<const RISE::Cst::UnresolvedReference*> mine;
				if( insertedId != 0 ) {
					for( const RISE::Cst::UnresolvedReference& u : all )
						if( u.sourceChunkId == insertedId ) mine.push_back( &u );
				} else {
					for( const RISE::Cst::UnresolvedReference& u : all ) {
						if( u.chunkKeyword != r.kind ) continue;
						if( chunkText.find( u.param ) != std::string::npos &&
						    chunkText.find( u.value ) != std::string::npos )
							mine.push_back( &u );
					}
				}
				if( mine.empty() ) return;

				std::string clauses;
				for( const RISE::Cst::UnresolvedReference* u : mine ) {
					AgentChunkIssue issue;
					issue.param  = u->param;
					issue.value  = u->value;
					issue.reason = "unresolved_reference";
					issue.suggestions = CollectUnresolvedRefSuggestions( doc, u->chunkKeyword, u->param, u->value );
					r.issues.push_back( issue );

					if( !clauses.empty() ) clauses += "; ";
					clauses += u->param + "='" + u->value + "'";
					if( !issue.suggestions.empty() ) clauses += " (did you mean '" + issue.suggestions.front() + "'?)";
				}
				r.message += " WARNING: unresolved reference(s) in this chunk: " + clauses +
					" -- the named chunk is not defined (yet). Fine if this is a forward "
					"reference you're about to define next; otherwise insert the missing "
					"chunk or correct the name.";
			}

			//! Model-B F5 slice S3: pre-flight, DESCRIPTOR-BASED analysis of a
			//! REJECTED insert_chunk (called ONLY from InsertChunk's headless path,
			//! and only for the generic "the chunk would not derive in context"
			//! catch-all -- Job::ApplyCstInsertChunk's code 0 -- never for a parse
			//! failure (-1, chunk didn't even parse to one closed chunk) or a name
			//! collision / reserved name (-2), both of which already carry a
			//! precise, actionable message of their own). `chunkText` PARSES
			//! CLEANLY here (InsertChunk's own -1 guard already ran), so this is
			//! purely about WHY the dry-run derive refused it, checked against the
			//! chunk's OWN descriptor plus `headDoc` (the CURRENT head -- byte-
			//! identical to what the rejected insert was dry-run against, since a
			//! reject never mutates).
			//!
			//! See AgentChunkIssue's doc for the four `reason` slugs. Three are
			//! decidable from the candidate chunk's OWN CST alone (no head needed):
			//! the keyword isn't registered (unknown_chunk_type); a param name
			//! isn't declared on the descriptor (unknown_param); a Reference-kind
			//! param's value is entirely numeric tokens, a type mismatch
			//! (numeric_in_reference_slot). The fourth -- a Reference-kind param's
			//! value is a NAME that doesn't resolve (unresolved_reference) -- needs
			//! the head: a THROWAWAY copy of `headDoc` gets this ONE candidate
			//! chunk appended (never mutates the caller's `headDoc`), then
			//! RISE::Cst::BuildReferenceGraph resolves over it EXACTLY as the real
			//! derive would (the CST chunk definitions the head + this candidate
			//! now carry, AND the engine's runtime defaults -- see BuildReferenceGraph's
			//! doc) -- reusing that resolver rather than re-implementing the
			//! (category,name) namespace here.
			//!
			//! HONESTY (read before trusting an empty return as exoneration): this
			//! is a STATIC, DESCRIPTOR-ONLY pass -- it does not see everything
			//! DeriveToJob's real apply-time Finalize() validation does (e.g. a
			//! `ggx_material` in `thinfilm` mode requiring `film_ior`+`film_thickness`
			//! together, a semantic cross-param constraint no descriptor field
			//! encodes). A REJECTED insert this analyser finds NOTHING wrong with
			//! is a REAL possibility -- it then returns an EMPTY vector, and the
			//! caller (InsertChunk) must NOT claim that as a clean bill of health;
			//! see its call site for how the appended sentence stays silent rather
			//! than implying the analyser exonerated the chunk.
			std::vector<AgentChunkIssue> AnalyzeRejectedInsert( const RISE::Cst::Document& headDoc,
			                                                     const std::string& chunkText )
			{
				std::vector<AgentChunkIssue> out;

				RISE::Cst::Document chunkDoc = RISE::Cst::ParseToCst( chunkText );
				RISE::Cst::NodeRef chunkItem;
				{
					const int n = RISE::Cst::DocItemCount( chunkDoc );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId( chunkDoc, RISE::Cst::DocNodeIdAt( chunkDoc, i ) );
						if( it && it->kind == NodeKind::Chunk ) { chunkItem = it; break; }
					}
				}
				if( !chunkItem ) return out;   // defensive only -- InsertChunk's own -1 guard already refused anything that doesn't parse to exactly one closed chunk

				const std::string keyword = chunkItem->role;
				const ChunkDescriptor* desc = DescriptorForKeyword( String( keyword.c_str() ) );
				if( !desc ) {
					AgentChunkIssue issue;
					issue.value       = keyword;
					issue.reason      = "unknown_chunk_type";
					issue.suggestions = RankNearMisses( AllChunkKeywords(), keyword );
					out.push_back( issue );
					return out;   // no descriptor -- nothing further to check against
				}

				// Reference-kind params whose value is a NAME (not numeric, not the
				// explicit-none idiom) -- resolved against the head in ONE shared
				// BuildReferenceGraph pass below, rather than one dry-run per param.
				std::vector<std::pair<std::string, std::string> > pendingRefChecks;   // (param, joined value)

				for( const RISE::Cst::NodeRef& kid : chunkItem->kids ) {
					if( kid->kind != NodeKind::Param ) continue;
					std::string pname;
					std::vector<std::string> valueTokens;
					for( const RISE::Cst::NodeRef& tk : kid->kids ) {
						if( tk->kind != NodeKind::Token ) continue;
						if( tk->role == "pname" ) pname = tk->text;
						else if( tk->role == "pvalue" ) valueTokens.push_back( tk->text );
					}
					if( pname.empty() ) continue;

					const ParameterDescriptor* pd = FindParam( *desc, pname );
					if( !pd ) {
						AgentChunkIssue issue;
						issue.param = pname;
						for( std::size_t t = 0; t < valueTokens.size(); ++t ) {
							if( t ) issue.value += ' ';
							issue.value += valueTokens[t];
						}
						issue.reason      = "unknown_param";
						issue.suggestions = NearMissParamNames( *desc, pname );
						out.push_back( issue );
						continue;
					}

					if( pd->kind != ValueKind::Reference ) continue;   // only a Reference slot can mistake a literal for a name (or vice versa)
					if( valueTokens.empty() ) continue;                // value-less -- a DIFFERENT, already-diagnosed derive failure

					std::string joined;
					for( std::size_t t = 0; t < valueTokens.size(); ++t ) {
						if( t ) joined += ' ';
						joined += valueTokens[t];
					}

					if( AllTokensNumeric( valueTokens ) ) {
						AgentChunkIssue issue;
						issue.param  = pname;
						issue.value  = joined;
						issue.reason = "numeric_in_reference_slot";
						out.push_back( issue );
						continue;
					}
					if( joined == "none" ) continue;   // explicit-none idiom -- never dangling

					pendingRefChecks.push_back( std::make_pair( pname, joined ) );
				}

				if( !pendingRefChecks.empty() ) {
					// A THROWAWAY copy -- headDoc (the caller's Document, byte-
					// identical to the live head since a reject never mutates) is
					// untouched; `merged` is a local value carrying this ONE
					// candidate chunk appended, purely so BuildReferenceGraph can
					// resolve against the SAME complete namespace a landed insert
					// would derive against.
					RISE::Cst::Document merged = headDoc;
					const int endAt = RISE::Cst::DocItemCount( merged );
					merged = RISE::Cst::DocInsertItem( merged, endAt, chunkItem );
					const std::string namePath = RISE::Cst::ChunkNamePath( chunkItem );
					const RISE::Cst::NodeId insertedId = namePath.empty() ? 0 : RISE::Cst::DocFindByName( merged, namePath );

					std::vector<RISE::Cst::UnresolvedReference> unresolved;
					RISE::Cst::BuildReferenceGraph( merged, nullptr, &unresolved );

					for( const std::pair<std::string, std::string>& pending : pendingRefChecks ) {
						for( const RISE::Cst::UnresolvedReference& u : unresolved ) {
							const bool mine = ( insertedId != 0 ) ? ( u.sourceChunkId == insertedId )
							                                       : ( u.chunkKeyword == keyword );
							if( !mine || u.param != pending.first || u.value != pending.second ) continue;
							AgentChunkIssue issue;
							issue.param       = u.param;
							issue.value       = u.value;
							issue.reason      = "unresolved_reference";
							issue.suggestions = CollectUnresolvedRefSuggestions( merged, keyword, u.param, u.value );
							out.push_back( issue );
							break;   // one issue per pending param
						}
					}
				}
				return out;
			}

			//! One issue's human-readable clause, for the FOUR `reason`s that can
			//! be produced against an EXISTING, resolved chunk's descriptor --
			//! shared verbatim by insert_chunk's AttachRejectionIssues and
			//! propose_patch's AttachParamEditRejectionIssues below (remove_chunk's
			//! sole reason, "still_referenced", has no chunk-descriptor content to
			//! share, so AttachRemoveRejectionIssues builds its own clause).
			//! `keyword` is the resolved chunk's own keyword (insert_chunk: the
			//! candidate chunk's parsed keyword, `r.kind`; propose_patch: the
			//! target's resolved keyword, threaded out of
			//! AnalyzeRejectedParamEdit) -- needed here for the "valid parameters
			//! are" / reference-category-name lookups, which both verbs share
			//! byte-for-byte. Returns "" for a `reason` this shared shape does not
			//! cover (unknown_chunk_type is insert-only and handled at insert's
			//! own call site instead, since it has no resolved keyword to key a
			//! descriptor lookup off of; unknown_target/invalid_value/
			//! still_referenced are each single-verb and built at their own call
			//! sites too).
			std::string IssueClause( const AgentChunkIssue& issue, const std::string& keyword )
			{
				if( issue.reason == "unknown_param" ) {
					std::string clause = "`" + issue.param + "` is not a valid parameter of `" + keyword + "`";
					if( !issue.suggestions.empty() ) clause += " (did you mean '" + issue.suggestions.front() + "'?)";
					std::string valid;
					const ChunkDescriptor* d = DescriptorForKeyword( String( keyword.c_str() ) );
					if( d ) {
						for( const ParameterDescriptor& p : d->parameters ) {
							if( p.name == "name" ) continue;
							if( !valid.empty() ) valid += ", ";
							valid += p.name;
						}
					}
					if( !valid.empty() ) clause += " -- valid parameters are: " + valid;
					return clause;
				}
				if( issue.reason == "numeric_in_reference_slot" ) {
					const ParameterDescriptor* pd = nullptr;
					const ChunkDescriptor* d = DescriptorForKeyword( String( keyword.c_str() ) );
					if( d ) pd = FindParam( *d, issue.param );
					std::string catList;
					if( pd ) for( ChunkCategory cc : pd->referenceCategories ) {
						if( !catList.empty() ) catList += "/";
						catList += CategoryNameLower( cc );
					}
					if( catList.empty() ) catList = "chunk";
					std::string clause = "`" + issue.param + "` = '" + issue.value + "' is a number, but this slot needs "
					           "the NAME of a " + catList + " chunk, not a literal";
					if( pd && !pd->referenceCategories.empty() && pd->referenceCategories.front() == ChunkCategory::Painter )
						clause += " -- e.g. define `uniformcolor_painter { name <n>  color " + issue.value +
						           " }` and set `" + issue.param + " <n>`";
					return clause;
				}
				if( issue.reason == "unresolved_reference" ) {
					std::string clause = "`" + issue.param + "` = '" + issue.value + "' does not name anything defined in the document";
					if( !issue.suggestions.empty() ) clause += " (did you mean '" + issue.suggestions.front() + "'?)";
					return clause;
				}
				return std::string();
			}

			//! Run the descriptor-based rejection analyser and fold what it finds
			//! into `r` (both the structured `issues` and a human-readable
			//! " ACTIONABLE: ..." clause appended to `message`).
			//!
			//! Shared by BOTH insert paths on purpose.  The LIVE (controller) path
			//! is the GUI, which is exactly where the unactionable
			//! "apply failed (e.g. unresolved reference); see log" was observed
			//! stalling a local model -- an agent cannot read the log -- so wiring
			//! this headless-only would have missed the case that motivated it.
			//! Callers gate on the generic rawCode-0 "would not derive in context"
			//! catch-all; the specific causes (parse failure, name collision)
			//! already carry precise messages and must not be second-guessed.
			//!
			//! HONESTY: finding nothing is NOT exoneration.  This is a static,
			//! descriptor-only pass, so a genuinely rejected chunk whose cause it
			//! cannot see statically leaves `issues` empty and `message`
			//! untouched -- never a clause implying the chunk checked out.
			void AttachRejectionIssues( AgentChunkResult& r, const RISE::Cst::Document& doc,
			                            const std::string& chunkText )
			{
				const std::vector<AgentChunkIssue> found =
					AnalyzeRejectedInsert( doc, chunkText );
				if( !found.empty() ) {
					r.issues = found;
					std::string clauses;
					for( const AgentChunkIssue& issue : found ) {
						if( !clauses.empty() ) clauses += "; ";
						if( issue.reason == "unknown_chunk_type" ) {
							clauses += "unknown chunk type '" + issue.value + "'";
							if( !issue.suggestions.empty() ) clauses += " (did you mean '" + issue.suggestions.front() + "'?)";
						} else {
							clauses += IssueClause( issue, r.kind );
						}
					}
					if( !clauses.empty() )
						r.message += " ACTIONABLE: " + clauses + ".";
				}
				// HONESTY: `found` empty is NOT exoneration -- the analyser is a
				// static, descriptor-only pass, so a genuinely rejected chunk whose
				// cause it cannot see statically leaves `issues` empty and `message`
				// untouched rather than implying the chunk checked out.
			}

			//! Model-B F5 (actionable propose_patch diagnostics): pre-flight,
			//! DESCRIPTOR- and REFERENCE-GRAPH-based analysis of a REJECTED
			//! propose_patch (called ONLY for the generic rawCode==0 "would not
			//! derive" catch-all -- ApplyCstParamEditChecked's code 0; a CONFLICT
			//! or an empty-field guard already carries its own precise message and
			//! must not be second-guessed). Unlike AnalyzeRejectedInsert, there is
			//! no candidate chunk TEXT to parse here -- (target, kind, param,
			//! value) are already in hand from the call, so this resolves the
			//! target directly against `doc`.
			//!
			//! See AgentChunkIssue's doc for the full seven-reason set; this
			//! analyser produces "unknown_target" and "invalid_value" (propose_
			//! patch-only), plus the three reasons it shares with insert_chunk
			//! ("unknown_param", "numeric_in_reference_slot",
			//! "unresolved_reference" -- reused via the SAME helpers
			//! AnalyzeRejectedInsert uses: FindParam, NearMissParamNames,
			//! AllTokensNumeric, CollectUnresolvedRefSuggestions, RankNearMisses).
			//!
			//! `outResolvedKeyword` (optional): filled with the target's resolved
			//! chunk KEYWORD whenever resolution succeeds, so the caller
			//! (AttachParamEditRejectionIssues) can build the "valid parameters
			//! are" / reference-category-name clauses via the shared IssueClause
			//! without a second resolution pass.
			//!
			//! HONESTY: same rule as AnalyzeRejectedInsert -- an empty return is
			//! NOT exoneration. A semantic cross-param constraint (e.g. a
			//! `fresnel_mode thinfilm` requiring film_ior+film_thickness together)
			//! is invisible to this static, per-field pass.
			std::vector<AgentChunkIssue> AnalyzeRejectedParamEdit( const RISE::Cst::Document& doc,
			                                                       const std::string& target,
			                                                       const std::string& kind,
			                                                       const std::string& param,
			                                                       const std::string& value,
			                                                       std::string* outResolvedKeyword = nullptr )
			{
				std::vector<AgentChunkIssue> out;

				// 1) Resolve the target -- the SAME call
				// Job::ApplyCstParamEditImpl_ itself makes (DocFindByNameAnyRole,
				// camera-only positional fallback), so a MISS here is the exact
				// cause of a code-0 reject when it is the TARGET that is wrong.
				const bool uniqueFallback = ( kind == "camera" );
				const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( doc, target, nullptr, kind, uniqueFallback );
				if( id == 0 ) {
					AgentChunkIssue issue;
					issue.value       = target;
					issue.reason      = "unknown_target";
					issue.suggestions = RankNearMisses( CollectTargetNameCandidates( doc, kind ), target );
					out.push_back( issue );
					return out;
				}

				const RISE::Cst::NodeRef chunkItem = RISE::Cst::DocResolveNodeId( doc, id );
				if( !chunkItem ) return out;   // defensive only -- `id` just resolved a moment ago
				const std::string keyword = chunkItem->role;
				if( outResolvedKeyword ) *outResolvedKeyword = keyword;

				const ChunkDescriptor* desc = DescriptorForKeyword( String( keyword.c_str() ) );
				if( !desc ) return out;   // a resolved chunk's own keyword is always registered -- defensive only

				// 2) Is `param` declared on this chunk's descriptor at all?
				const ParameterDescriptor* pd = FindParam( *desc, param );
				if( !pd ) {
					AgentChunkIssue issue;
					issue.param       = param;
					issue.value       = value;
					issue.reason      = "unknown_param";
					issue.suggestions = NearMissParamNames( *desc, param );
					out.push_back( issue );
					return out;
				}

				// 3) Reference-kind: a numeric literal in the slot, or a NAME that
				// does not resolve.
				if( pd->kind == ValueKind::Reference ) {
					const std::vector<std::string> toks = SplitWhitespace( value );
					if( AllTokensNumeric( toks ) ) {
						AgentChunkIssue issue;
						issue.param  = param;
						issue.value  = value;
						issue.reason = "numeric_in_reference_slot";
						out.push_back( issue );
						return out;
					}
					if( value == "none" ) return out;   // explicit-none idiom -- never dangling

					// Apply the PROPOSED value to a throwaway copy of `doc` (a value
					// edit preserves NodeIds -- see ReferenceGraph's doc -- so `id`
					// stays valid) and resolve THAT against BuildReferenceGraph, the
					// SAME resolver AnalyzeRejectedInsert / AttachChunkIssueWarnings
					// use, rather than re-implementing the (category,name) namespace
					// here. DocSetOrAddParamValue is the SAME mutator
					// ApplyCstParamEditImpl_ itself uses.
					const RISE::Cst::Document trial = RISE::Cst::DocSetOrAddParamValue( doc, id, param, /*occ*/0, value );
					std::vector<RISE::Cst::UnresolvedReference> unresolved;
					RISE::Cst::BuildReferenceGraph( trial, nullptr, &unresolved );
					for( const RISE::Cst::UnresolvedReference& u : unresolved ) {
						if( u.sourceChunkId != id || u.param != param || u.value != value ) continue;
						AgentChunkIssue issue;
						issue.param       = param;
						issue.value       = value;
						issue.reason      = "unresolved_reference";
						issue.suggestions = CollectUnresolvedRefSuggestions( trial, keyword, param, value );
						out.push_back( issue );
						return out;
					}
					return out;   // resolves fine (or this static pass can't see why not) -- nothing more to say
				}

				// 4) Numeric / Enum: an ill-typed value.
				if( IsNumericKind( pd->kind ) ) {
					const std::vector<std::string> toks = SplitWhitespace( value );
					if( !AllTokensNumeric( toks ) ) {
						AgentChunkIssue issue;
						issue.param  = param;
						issue.value  = value;
						issue.reason = "invalid_value";
						out.push_back( issue );
					}
					return out;
				}
				if( pd->kind == ValueKind::Enum ) {
					bool known = false;
					for( const std::string& ev : pd->enumValues ) if( ev == value ) { known = true; break; }
					if( !known ) {
						AgentChunkIssue issue;
						issue.param  = param;
						issue.value  = value;
						issue.reason = "invalid_value";
						out.push_back( issue );
					}
					return out;
				}

				return out;   // Bool/String/Filename -- nothing this static pass can say (toBoolean() never rejects; String/Filename have no structural constraint to violate)
			}

			//! propose_patch's sibling of AttachRejectionIssues -- see that
			//! function's doc for the shared shape/gating rationale (callers gate
			//! on the same generic rawCode==0 catch-all).
			void AttachParamEditRejectionIssues( AgentPatchResult& r, const RISE::Cst::Document& doc,
			                                     const std::string& target, const std::string& kind,
			                                     const std::string& param, const std::string& value )
			{
				std::string resolvedKeyword;
				const std::vector<AgentChunkIssue> found =
					AnalyzeRejectedParamEdit( doc, target, kind, param, value, &resolvedKeyword );
				if( found.empty() ) return;   // HONESTY -- see AnalyzeRejectedParamEdit's doc; not exoneration
				r.issues = found;
				std::string clauses;
				for( const AgentChunkIssue& issue : found ) {
					if( !clauses.empty() ) clauses += "; ";
					if( issue.reason == "unknown_target" ) {
						clauses += "target '" + issue.value + "' does not name";
						clauses += kind.empty() ? std::string( " any chunk" ) : ( " a `" + kind + "` chunk" );
						clauses += " in the document";
						if( !issue.suggestions.empty() ) clauses += " (did you mean '" + issue.suggestions.front() + "'?)";
					} else if( issue.reason == "invalid_value" ) {
						const ChunkDescriptor* d = DescriptorForKeyword( String( resolvedKeyword.c_str() ) );
						const ParameterDescriptor* pd = d ? FindParam( *d, issue.param ) : nullptr;
						if( pd && pd->kind == ValueKind::Enum ) {
							std::string allowed;
							for( const std::string& ev : pd->enumValues ) {
								if( !allowed.empty() ) allowed += ", ";
								allowed += ev;
							}
							clauses += "`" + issue.param + "` = '" + issue.value + "' is not one of `" + resolvedKeyword + "`'s allowed values";
							if( !allowed.empty() ) clauses += " -- allowed values are: " + allowed;
						} else {
							clauses += "`" + issue.param + "` = '" + issue.value + "' is not a valid numeric value for this parameter";
						}
					} else {
						clauses += IssueClause( issue, resolvedKeyword );
					}
				}
				if( !clauses.empty() )
					r.message += " ACTIONABLE: " + clauses + ".";
			}

			//! Model-B F5 (actionable remove_chunk diagnostics): pre-flight,
			//! REFERENCE-GRAPH-based analysis of a REJECTED remove_chunk (called
			//! ONLY for the generic rawCode==0 "would not derive" catch-all -- the
			//! SAME gating FoldChunkCode's -1/-2 branches already carry their own
			//! precise causes and must not be second-guessed).  `doc` is the
			//! CURRENT head (byte-identical to what the rejected removal dry-ran
			//! against, since a reject never mutates); `target`/`kind` are the
			//! caller's original remove_chunk arguments.
			//!
			//! The engine's own hedge ("...it is likely still REFERENCED by
			//! another chunk, or the remaining document no longer derives in
			//! order...") names TWO possible causes without picking one.
			//! BuildReferenceGraph's reverse adjacency (`dependents`: a
			//! referenced chunk -> the chunks that reference it) can DISTINGUISH
			//! them for the common case: resolve the target's NodeId (mirroring
			//! Job::ApplyCstRemoveChunk's own DocFindByNameAnyRole resolution --
			//! the camera-only positional fallback is the one `uniqueFallback`
			//! case cheap to replicate here; every other kind stays strict
			//! unique-or-refuse, same as the engine), then look it up in
			//! `dependents`. A non-empty hit names the referrer(s) precisely
			//! (reason "still_referenced"); an EMPTY hit means the OTHER cause
			//! (the remaining document no longer derives in order) is the real
			//! one -- see the HONESTY note below.
			//!
			//! HONESTY: mirrors AnalyzeRejectedInsert's rule exactly -- finding no
			//! dependents is NOT proof the chunk is unreferenced everywhere (a
			//! DYNAMIC reference this static graph does not model -- e.g. a
			//! timeline `element` naming an entity outside any declared Reference
			//! param -- could still exist) and is NOT proof the OTHER cause is
			//! real either. So an unresolvable target (id==0) or a resolved
			//! target with no dependents both return an EMPTY vector, and the
			//! caller must leave the honest hedged message untouched -- never
			//! invent a referrer that is not there.
			std::vector<AgentChunkIssue> AnalyzeRejectedRemove( const RISE::Cst::Document& doc,
			                                                    const std::string& target,
			                                                    const std::string& kind )
			{
				std::vector<AgentChunkIssue> out;

				const bool uniqueFallback = ( kind == "camera" );
				const RISE::Cst::NodeId id = RISE::Cst::DocFindByNameAnyRole( doc, target, nullptr, kind, uniqueFallback );
				if( id == 0 ) return out;   // could not uniquely resolve -- nothing to look up (HONESTY)

				const RISE::Cst::ReferenceGraph graph = RISE::Cst::BuildReferenceGraph( doc, nullptr, nullptr );
				const std::map<RISE::Cst::NodeId, std::set<RISE::Cst::NodeId> >::const_iterator dep =
					graph.dependents.find( id );
				if( dep == graph.dependents.end() || dep->second.empty() ) return out;   // no referrers -- the OTHER cause; stay silent (HONESTY)

				AgentChunkIssue issue;
				issue.value  = target;
				issue.reason = "still_referenced";
				for( const RISE::Cst::NodeId refId : dep->second ) {
					const RISE::Cst::NodeRef refItem = RISE::Cst::DocResolveNodeId( doc, refId );
					if( !refItem ) continue;
					const std::string namePath = RISE::Cst::ChunkNamePath( refItem );
					std::string label = refItem->role;   // fallback for an unnamed referrer
					if( !namePath.empty() ) {
						const std::string prefix = refItem->role + "/";
						if( namePath.size() > prefix.size() && namePath.compare( 0, prefix.size(), prefix ) == 0 )
							label = namePath.substr( prefix.size() );
					}
					issue.suggestions.push_back( label );
				}
				if( issue.suggestions.empty() ) return out;   // resolved referrer NodeIds but none produced a nameable label -- stay silent (HONESTY)
				out.push_back( issue );
				return out;
			}

			//! remove_chunk's sibling of AttachRejectionIssues -- see that
			//! function's doc for the shared shape/gating rationale.
			void AttachRemoveRejectionIssues( AgentChunkResult& r, const RISE::Cst::Document& doc,
			                                  const std::string& target, const std::string& kind )
			{
				const std::vector<AgentChunkIssue> found = AnalyzeRejectedRemove( doc, target, kind );
				if( found.empty() ) return;   // HONESTY -- see AnalyzeRejectedRemove's doc; not exoneration
				r.issues = found;
				std::string clauses;
				for( const AgentChunkIssue& issue : found ) {
					if( !clauses.empty() ) clauses += "; ";
					std::string refs;
					for( const std::string& s : issue.suggestions ) {
						if( !refs.empty() ) refs += ", ";
						refs += "'" + s + "'";
					}
					clauses += "'" + issue.value + "' is still referenced by " + refs;
				}
				if( !clauses.empty() )
					r.message += " ACTIONABLE: " + clauses + ".";
			}

		}

		std::vector<AgentPatchResult> AgentSession::ProposePatches( const std::vector<AgentSetPatch>& patches,
		                                                            const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			std::vector<AgentPatchResult> out;

			if( patches.empty() )
				return out;

			out.reserve( patches.size() );

			for( std::size_t i = 0; i < patches.size(); ++i )
			{
				AgentSetPatch item = patches[i];
				if( i == 0 && baseOrNull )
				{
					item.hasBaseVersion = true;
					item.baseVersion    = *baseOrNull;
				}
				else
				{
					item.hasBaseVersion = false;
				}
				out.push_back( ProposePatch( item ) );

				// STALE-BASE CONFLICT IS BATCH-FATAL (see the header doc).
				// Only the FIRST element carries the caller's precondition, so
				// only it can report a stale base -- and when it does, every
				// remaining element would apply UNCONDITIONALLY against a head
				// the caller has never read.  Since a patch OVERWRITES rather
				// than adds, that would silently clobber whatever a concurrent
				// co-editor changed.  Stop, and fill the unattempted tail with
				// an explicit conflict so results[i] still lines up with
				// patches[i] and the caller can see nothing else was tried.
				if( i == 0 && baseOrNull && out.back().status == "conflict" )
				{
					const AgentPatchResult& head = out.back();
					for( std::size_t j = 1; j < patches.size(); ++j )
					{
						AgentPatchResult skipped;
						skipped.applied     = false;
						skipped.retriable   = head.retriable;
						skipped.rawCode     = head.rawCode;
						skipped.status      = "conflict";
						skipped.headVersion = head.headVersion;
						skipped.message     = "not attempted: the batch's baseHeadVersion is stale "
						                      "(element 0 conflicted) -- re-read the head and resubmit";
						out.push_back( skipped );
					}
					break;
				}
			}

			return out;
		}

		AgentChunkResult AgentSession::InsertChunk( const std::string& chunkText,
		                                            const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			AgentChunkResult r;

			// Secure-MCP slice 5a: the SAME authority gate as ProposePatch
			// (see that method's doc for the full rationale) -- enforced
			// before the existing LIVE-mode commit branch.
			if( mAuthority == AgentAuthority::External )
			{
				if( !mController )
				{
					r.applied = false;
					r.rawCode = 0;
					r.status  = "rejected";
					r.headVersion = HeadVersion();
					r.message = "insert_chunk refused: this session is External-authority and no live "
					            "controller is attached -- staging needs a live Owner to resolve against";
					return r;
				}
				// S5a hardening: see ProposePatch's identical comment --
				// no unlocked head pre-read; StageProposal stamps it under
				// its own mMutex hold when no explicit base was supplied.
				SceneEditController::AgentProposal p;
				p.kind        = SceneEditController::AgentProposalKind::InsertChunk;
				p.chunkText   = String( chunkText.c_str() );
				p.hasExplicitBaseVersion = ( baseOrNull != nullptr );
				if( baseOrNull ) p.baseVersion = *baseOrNull;
				// Secure-MCP slice 5c: see ProposePatch's identical comment.
				p.sessionLabel = String( mSessionLabel.c_str() );
				RISE::Cst::CstHeadVersion stagedHead{};
				const std::uint64_t id = mController->StageProposal( p, &stagedHead );
				if( id == 0 )
				{
					// Secure-MCP slice 6: see ProposePatch's identical
					// queue-full branch for the full rationale.
					r.applied    = false;
					r.rawCode    = 0;
					r.status     = "rejected";
					r.queueFull  = true;
					r.headVersion = HeadVersion();
					r.message = "insert_chunk refused: the pending-proposal queue is full -- "
					            "the Owner must resolve (approve/reject) some pending proposals "
					            "before another can be staged";
					return r;
				}
				r.applied = false;
				r.rawCode = 0;
				r.status  = "staged";
				r.headVersion = stagedHead;
				char buf[128];
				std::snprintf( buf, sizeof( buf ), "proposal %llu staged (pending owner approval)",
					static_cast<unsigned long long>( id ) );
				r.message = buf;
				return r;
			}

			// LIVE mode: delegate wholesale to the controller's render-safe
			// path (park + conflict gate + Job primitive + rebind + dirty +
			// kick), exactly as ProposePatch does.  Map its AgentCommitResult
			// 1:1, including the chunk-identity echo.
			if( mController )
			{
				const SceneEditController::AgentCommitResult cr =
					mController->ApplyAgentInsertChunk( String( chunkText.c_str() ), baseOrNull );
				r.applied     = cr.applied;
				r.retriable   = cr.retriable;
				r.rawCode     = cr.rawCode;
				r.status      = cr.status.c_str();
				r.headVersion = cr.headVersion;
				r.message     = cr.message.c_str();
				r.name        = cr.chunkName.c_str();
				r.kind        = cr.chunkKeyword.c_str();
				// Model-B: non-blocking dangling-reference WARNING (see
				// AttachChunkIssueWarnings's doc) -- the controller commits through
				// the SAME mJob, so its retained Document already reflects this insert.
				if( r.applied && mJob && mJob->GetCstDocument() )
					AttachChunkIssueWarnings( r, *mJob->GetCstDocument(), chunkText );
				// ...and the REJECTION analyser on the failing side.  THIS is the
				// GUI path -- the one where the unactionable "apply failed (e.g.
				// unresolved reference); see log" was actually observed stalling a
				// local model -- so it needs the actionable clause at least as much
				// as the headless path does.  A rejection leaves the head
				// UNCHANGED, so the retained Document is the correct namespace to
				// resolve the candidate chunk's references against.
				else if( !r.applied && r.rawCode == 0 && mJob && mJob->GetCstDocument() )
					AttachRejectionIssues( r, *mJob->GetCstDocument(), chunkText );
				return r;
			}

			// HEADLESS (direct-Job) mode.  Pre-flight guards mirror ProposePatch.
			if( !mJob || !mJob->HasRetainedCstDocument() ) {
				r.applied = false;
				r.rawCode = 0;
				r.status  = "rejected";
				r.headVersion = HeadVersion();
				r.message = "no retained CST Document -- InsertChunk needs a CST-loaded head";
				return r;
			}
			if( chunkText.empty() ) {
				r.applied = false;
				r.rawCode = 0;
				r.status  = "rejected";
				r.headVersion = mJob->GetCstHeadVersion();
				r.message = "chunkText must be non-empty";
				return r;
			}
			// Optimistic-concurrency CONFLICT precondition, BEFORE any mutation
			// (same semantics + message as ProposePatch's slice-1a gate).
			if( baseOrNull ) {
				const RISE::Cst::CstHeadVersion cur = mJob->GetCstHeadVersion();
				if( *baseOrNull != cur ) {
					r.applied     = false;
					r.rawCode     = 0;
					r.status      = "conflict";
					r.headVersion = cur;
					char buf[160];
					std::snprintf( buf, sizeof( buf ),
						"baseHeadVersion does not match the current head (revision %llu) -- re-read and re-propose",
						static_cast<unsigned long long>( cur.revision ) );
					r.message = buf;
					return r;
				}
			}

			char kwBuf[128];   kwBuf[0] = '\0';
			char nameBuf[256]; nameBuf[0] = '\0';
			char diagBuf[512]; diagBuf[0] = '\0';
			const int code = mJob->ApplyCstInsertChunk( chunkText.c_str(),
			                                            kwBuf, sizeof( kwBuf ),
			                                            nameBuf, sizeof( nameBuf ),
			                                            diagBuf, sizeof( diagBuf ) );
			r.kind = kwBuf;
			r.name = nameBuf;
			FoldChunkCode( r, code, /*isInsert*/ true, std::string(), diagBuf, /*kindWasPassed*/ false );
			r.headVersion = mJob->GetCstHeadVersion();
			// Model-B: non-blocking dangling-reference WARNING (see
			// AttachChunkIssueWarnings's doc above FoldChunkCode's namespace).
			if( r.applied && mJob->GetCstDocument() )
				AttachChunkIssueWarnings( r, *mJob->GetCstDocument(), chunkText );
			// Model-B F5 slice S3: the pre-flight CAUSE analysis for a REJECTED
			// insert -- ONLY for code 0 (Job::ApplyCstInsertChunk's generic
			// "would not derive in context" catch-all; see AnalyzeRejectedInsert's
			// doc for why -1/-2 are excluded, they already carry a precise cause).
			// `mJob->GetCstDocument()` here is the SAME, UNCHANGED head the failed
			// dry-run ran against (a reject never mutates), so this is a faithful
			// re-check, not a stale one.
			if( code == 0 && mJob->GetCstDocument() )
				AttachRejectionIssues( r, *mJob->GetCstDocument(), chunkText );
			return r;
		}

		std::vector<AgentChunkResult> AgentSession::InsertChunks( const std::vector<std::string>& chunkTexts,
		                                                          const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			std::vector<AgentChunkResult> out;

			// Empty input is a no-op -- return an empty vector, don't call
			// InsertChunk at all (see the header doc).
			if( chunkTexts.empty() )
				return out;

			out.reserve( chunkTexts.size() );

			// SEQUENTIAL, BEST-EFFORT: delegate to the existing InsertChunk
			// for every element, in order, without duplicating any of its
			// logic (authority gate, LIVE-vs-headless routing, conflict
			// detection, issue diagnostics all come along for free).  Only
			// the FIRST call gets the caller's `baseOrNull` -- every later
			// element applies against the head as this batch has evolved
			// it so far (see the header doc's rationale).  A rejected
			// element does not abort the loop; later elements are still
			// attempted so their own results are informative even when
			// they depended on the rejected one.
			for( std::size_t i = 0; i < chunkTexts.size(); ++i )
			{
				const RISE::Cst::CstHeadVersion* base = ( i == 0 ) ? baseOrNull : nullptr;
				out.push_back( InsertChunk( chunkTexts[i], base ) );
			}

			return out;
		}

		AgentChunkResult AgentSession::RemoveChunk( const std::string& target,
		                                            const std::string& kind,
		                                            const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			AgentChunkResult r;
			r.name = target;

			// Secure-MCP slice 5a: the SAME authority gate as ProposePatch /
			// InsertChunk (see ProposePatch's doc for the full rationale).
			if( mAuthority == AgentAuthority::External )
			{
				if( !mController )
				{
					r.applied = false;
					r.rawCode = 0;
					r.status  = "rejected";
					r.headVersion = HeadVersion();
					r.message = "remove_chunk refused: this session is External-authority and no live "
					            "controller is attached -- staging needs a live Owner to resolve against";
					return r;
				}
				// S5a hardening: see ProposePatch's identical comment --
				// no unlocked head pre-read; StageProposal stamps it under
				// its own mMutex hold when no explicit base was supplied.
				SceneEditController::AgentProposal p;
				p.kind        = SceneEditController::AgentProposalKind::RemoveChunk;
				p.target      = String( target.c_str() );
				p.entityKind  = String( kind.c_str() );
				p.hasExplicitBaseVersion = ( baseOrNull != nullptr );
				if( baseOrNull ) p.baseVersion = *baseOrNull;
				// Secure-MCP slice 5c: see ProposePatch's identical comment.
				p.sessionLabel = String( mSessionLabel.c_str() );
				RISE::Cst::CstHeadVersion stagedHead{};
				const std::uint64_t id = mController->StageProposal( p, &stagedHead );
				if( id == 0 )
				{
					// Secure-MCP slice 6: see ProposePatch's identical
					// queue-full branch for the full rationale.
					r.applied    = false;
					r.rawCode    = 0;
					r.status     = "rejected";
					r.queueFull  = true;
					r.headVersion = HeadVersion();
					r.message = "remove_chunk refused: the pending-proposal queue is full -- "
					            "the Owner must resolve (approve/reject) some pending proposals "
					            "before another can be staged";
					return r;
				}
				r.applied = false;
				r.rawCode = 0;
				r.status  = "staged";
				r.headVersion = stagedHead;
				char buf[128];
				std::snprintf( buf, sizeof( buf ), "proposal %llu staged (pending owner approval)",
					static_cast<unsigned long long>( id ) );
				r.message = buf;
				return r;
			}

			// LIVE mode: delegate to the controller (see InsertChunk).
			if( mController )
			{
				const SceneEditController::AgentCommitResult cr =
					mController->ApplyAgentRemoveChunk( String( target.c_str() ),
					                                    String( kind.c_str() ), baseOrNull );
				r.applied     = cr.applied;
				r.retriable   = cr.retriable;
				r.rawCode     = cr.rawCode;
				r.status      = cr.status.c_str();
				r.headVersion = cr.headVersion;
				r.message     = cr.message.c_str();
				r.name        = cr.chunkName.c_str();
				r.kind        = cr.chunkKeyword.c_str();
				// Actionable rejection diagnostics on the LIVE path -- see
				// ProposePatch's identical rationale/gating comment on its own
				// LIVE branch (this IS the GUI path).  A rejection leaves the
				// head UNCHANGED, so the retained Document is the correct
				// namespace to resolve the target + its referrers against.
				if( !r.applied && r.rawCode == 0 && mJob && mJob->GetCstDocument() )
					AttachRemoveRejectionIssues( r, *mJob->GetCstDocument(), target, kind );
				return r;
			}

			if( !mJob || !mJob->HasRetainedCstDocument() ) {
				r.applied = false;
				r.rawCode = 0;
				r.status  = "rejected";
				r.headVersion = HeadVersion();
				r.message = "no retained CST Document -- RemoveChunk needs a CST-loaded head";
				return r;
			}
			if( target.empty() ) {
				r.applied = false;
				r.rawCode = 0;
				r.status  = "rejected";
				r.headVersion = mJob->GetCstHeadVersion();
				r.message = "target must be non-empty";
				return r;
			}
			if( baseOrNull ) {
				const RISE::Cst::CstHeadVersion cur = mJob->GetCstHeadVersion();
				if( *baseOrNull != cur ) {
					r.applied     = false;
					r.rawCode     = 0;
					r.status      = "conflict";
					r.headVersion = cur;
					char buf[160];
					std::snprintf( buf, sizeof( buf ),
						"baseHeadVersion does not match the current head (revision %llu) -- re-read and re-propose",
						static_cast<unsigned long long>( cur.revision ) );
					r.message = buf;
					return r;
				}
			}

			char kwBuf[128];   kwBuf[0] = '\0';
			char diagBuf[512]; diagBuf[0] = '\0';
			const int code = mJob->ApplyCstRemoveChunk( target.c_str(),
			                                            kind.empty() ? nullptr : kind.c_str(),
			                                            kwBuf, sizeof( kwBuf ),
			                                            diagBuf, sizeof( diagBuf ) );
			r.kind = kwBuf;
			FoldChunkCode( r, code, /*isInsert*/ false, target, diagBuf, /*kindWasPassed*/ !kind.empty() );
			r.headVersion = mJob->GetCstHeadVersion();
			// Model-B: the pre-flight CAUSE analysis for a REJECTED remove --
			// ONLY for literal code 0 (Job::ApplyCstRemoveChunk's generic "would
			// not derive" catch-all; see AnalyzeRejectedRemove's doc for why -1/-2
			// are excluded -- they already carry a precise cause, same rule
			// InsertChunk's AttachRejectionIssues call applies).
			if( code == 0 && mJob->GetCstDocument() )
				AttachRemoveRejectionIssues( r, *mJob->GetCstDocument(), target, kind );
			return r;
		}

		namespace
		{
			//! Secure-MCP slice 5a: SceneEditController::AgentProposalKind ->
			//! the wire-friendly string ListProposals reports.
			const char* ProposalKindName( SceneEditController::AgentProposalKind k )
			{
				switch( k )
				{
					case SceneEditController::AgentProposalKind::ParamEdit:   return "param_edit";
					case SceneEditController::AgentProposalKind::InsertChunk: return "insert_chunk";
					case SceneEditController::AgentProposalKind::RemoveChunk: return "remove_chunk";
				}
				return "param_edit";
			}
		}

		std::vector<AgentSession::AgentProposalEntry> AgentSession::ListProposals() const
		{
			std::vector<AgentProposalEntry> out;
			// CONTROLLER-ATTACHED ONLY: a headless session has no shared queue
			// (the queue lives on SceneEditController -- see that class's doc)
			// to list against.  Empty, not an error: "no proposals" and "no
			// queue to ask" look the same to a caller that just wants a list.
			if( !mController ) return out;

			const std::vector<SceneEditController::AgentProposal> proposals = mController->ListProposals();
			out.reserve( proposals.size() );
			for( const SceneEditController::AgentProposal& p : proposals )
			{
				AgentProposalEntry e;
				e.id           = p.id;
				e.kind         = ProposalKindName( p.kind );
				e.target       = p.target.c_str();
				e.entityKind   = p.entityKind.c_str();
				e.param        = p.param.c_str();
				e.value        = p.value.c_str();
				e.chunkText    = p.chunkText.c_str();
				e.baseVersion  = p.baseVersion;
				e.sessionLabel = p.sessionLabel.c_str();
				e.status       = p.status.c_str();
				out.push_back( e );
			}
			return out;
		}

		AgentSession::AgentResolveResult AgentSession::ResolveProposal( std::uint64_t proposalId, bool approve )
		{
			AgentResolveResult r;

			// OWNER-ONLY GATE, enforced HERE -- an External-authority session
			// may not resolve ANY proposal, including one it staged itself.
			// This is the "external approving its own proposal" invariant:
			// refused before the controller (which has no notion of
			// per-session authority -- see SceneEditController::
			// ResolveProposal's doc) is even consulted.
			if( mAuthority != AgentAuthority::Owner )
			{
				r.ok = false;
				r.message = "resolve_proposal refused: only an Owner-authority session may approve or reject "
				            "a proposal (an External session may not resolve ANY proposal, including its own)";
				return r;
			}

			// CONTROLLER-ATTACHED ONLY: there is no queue to resolve against
			// without a live controller.
			if( !mController )
			{
				r.ok = false;
				r.message = "resolve_proposal refused: no live controller attached -- there is no proposal queue";
				return r;
			}

			SceneEditController::AgentCommitResult cr;
			const bool found = mController->ResolveProposal( proposalId, approve, &cr );
			if( !found )
			{
				r.ok = false;
				r.message = "resolve_proposal refused: no pending proposal with that id (unknown id, or it was already resolved)";
				return r;
			}

			r.ok = true;

			// Fold the underlying outcome into the wire-visible result shape
			// (mirrors ProposePatch / InsertChunk / RemoveChunk's own 1:1
			// AgentCommitResult mapping -- see those methods for the
			// rationale of each field).  Done UNIFORMLY for both approve AND
			// reject -- Secure-MCP slice 5b fix round (P2-2): a reject used
			// to return early here with paramResult/chunkResult left
			// default-constructed, so AgentRpc.cpp's wire selector (which
			// reads whichever of the two carries a non-empty `status`) fell
			// through to chunkResult's default headVersion {0,0} -- silently
			// colliding with the "no session / unknown id" sentinel, even
			// though SceneEditController::ResolveProposal's reject branch
			// (see that method's doc) now populates `cr.headVersion` with
			// the REAL current head.  We do not know here which of the two
			// result SHAPES (param-edit vs chunk-CRUD) applies without
			// asking the controller which kind this proposal was, so
			// re-derive it from the queue snapshot (cheap; ListProposals is
			// O(n) and this runs once per resolve, not per hot loop) --
			// resolved proposals stay in the queue for audit, so this lookup
			// finds the entry whether this is a reject, an approve, or a
			// conflict.
			SceneEditController::AgentProposalKind kind = SceneEditController::AgentProposalKind::ParamEdit;
			{
				const std::vector<SceneEditController::AgentProposal> all = mController->ListProposals();
				for( const SceneEditController::AgentProposal& p : all )
					if( p.id == proposalId ) { kind = p.kind; break; }
			}

			r.status = cr.status.c_str();
			if( kind == SceneEditController::AgentProposalKind::ParamEdit )
			{
				r.paramResult.applied     = cr.applied;
				r.paramResult.retriable   = cr.retriable;
				r.paramResult.rawCode     = cr.rawCode;
				r.paramResult.status      = cr.status.c_str();
				r.paramResult.headVersion = cr.headVersion;
				r.paramResult.message     = cr.message.c_str();
			}
			else
			{
				r.chunkResult.applied     = cr.applied;
				r.chunkResult.retriable   = cr.retriable;
				r.chunkResult.rawCode     = cr.rawCode;
				r.chunkResult.status      = cr.status.c_str();
				r.chunkResult.headVersion = cr.headVersion;
				r.chunkResult.message     = cr.message.c_str();
				r.chunkResult.name        = cr.chunkName.c_str();
				r.chunkResult.kind        = cr.chunkKeyword.c_str();
			}
			r.message = cr.message.c_str();
			return r;
		}

		namespace
		{
			//! Preview-render: the captured original value of one overridden
			//! camera field, so it can be restored verbatim after the render.
			struct CapturedCameraField
			{
				std::string name;    //!< the CameraIntrospection property name
				std::string value;   //!< its value BEFORE the override
			};

			//! P1-A fix: RAII guard that restores the film-dims / camera-pose
			//! overrides on EVERY exit from the render window, including an
			//! exception unwinding out of mJob->Rasterize() (OIDN denoise is a
			//! documented real throw site -- see
			//! PixelBasedRasterizerHelper.cpp's own FrameStoreBulkBracket RAII
			//! guard for the identical rationale).  Before this guard, the
			//! capture -> override -> render -> restore sequence was a plain
			//! straight-line lambda body with the restore AFTER Rasterize(): an
			//! exception there would unwind past the restore entirely, leaving
			//! the shared Film dims / active camera PERMANENTLY overridden even
			//! though AgentRpc's dispatch-level catch(...) reports a clean
			//! -32603 to the caller.
			//!
			//! Construct with references to the state the render window needs;
			//! call Arm() once the capture step has actually recorded the
			//! pre-override values (so a guard destructed before Arm() -- e.g.
			//! a throw during the capture step itself, before any override was
			//! applied -- is a safe no-op).  The destructor is NOEXCEPT-SAFE:
			//! CameraIntrospection::SetProperty and Job::SetFilm are both
			//! plain-data setters that do not throw (verified by inspection --
			//! neither contains a `throw` and both fail via a bool return), but
			//! the restore is wrapped in a try/catch anyway as a defensive belt
			//! (log-and-swallow, NEVER rethrow from a destructor) in case a
			//! future change to either introduces one.
			class RenderOverrideRestoreGuard
			{
			public:
				RenderOverrideRestoreGuard( IJobPriv& job,
				                            bool& overrodeFilm,
				                            unsigned int& origFilmW,
				                            unsigned int& origFilmH,
				                            double& origFilmPAR,
				                            ICamera*& activeCam,
				                            std::vector<CapturedCameraField>& capturedCam )
					: mJob( job )
					, mOverrodeFilm( overrodeFilm )
					, mOrigFilmW( origFilmW )
					, mOrigFilmH( origFilmH )
					, mOrigFilmPAR( origFilmPAR )
					, mActiveCam( activeCam )
					, mCapturedCam( capturedCam )
					, mArmed( false )
				{
				}

				//! Call once the pre-override state has been captured (whether
				//! or not any override actually applied) -- makes the
				//! destructor's restore live.
				void Arm() { mArmed = true; }

				//! Explicit early restore (used once the render window's
				//! normal-path restore runs) so the destructor's restore is a
				//! no-op on the ordinary success path -- Disarm() after a
				//! successful explicit restore avoids a harmless-but-redundant
				//! double SetProperty/SetFilm call.
				void Disarm() { mArmed = false; }

				~RenderOverrideRestoreGuard()
				{
					if( !mArmed ) return;
					try {
						// Reverse order, matching the original tail-of-lambda
						// restore sequencing: camera fields first, film dims
						// last.
						if( mActiveCam ) {
							for( std::size_t i = mCapturedCam.size(); i-- > 0; ) {
								CameraIntrospection::SetProperty( *mActiveCam,
									String( mCapturedCam[i].name.c_str() ),
									String( mCapturedCam[i].value.c_str() ) );
							}
						}
						if( mOverrodeFilm ) {
							mJob.SetFilm( mOrigFilmW, mOrigFilmH, mOrigFilmPAR );
						}
					}
					catch( ... ) {
						// NEVER rethrow from a destructor (would terminate() if
						// already unwinding from Rasterize()'s own exception).
						// Log so a future throw here is at least diagnosable.
						GlobalLog()->PrintEx( eLog_Error,
							"AgentSession::Render: exception escaped the film/camera "
							"override restore -- state may be left overridden" );
					}
				}

			private:
				RenderOverrideRestoreGuard( const RenderOverrideRestoreGuard& );             // deleted
				RenderOverrideRestoreGuard& operator=( const RenderOverrideRestoreGuard& );   // deleted

				IJobPriv&                          mJob;
				bool&                               mOverrodeFilm;
				unsigned int&                       mOrigFilmW;
				unsigned int&                       mOrigFilmH;
				double&                              mOrigFilmPAR;
				ICamera*&                            mActiveCam;
				std::vector<CapturedCameraField>&    mCapturedCam;
				bool                                 mArmed;
			};

			//! Round-2 P2-A: RAII restore of the Job's progress-callback hook,
			//! matching RenderOverrideRestoreGuard's house shape (Arm/Disarm,
			//! restore-on-every-exit-including-a-throw).  Before this fix,
			//! doRenderWork's `mJob->SetProgress( mController->
			//! AgentRenderProgress() ); rendered = mJob->Rasterize(); ...
			//! mJob->SetProgress( nullptr );` left the SECOND SetProgress
			//! call SKIPPED whenever Rasterize() threw (the exception
			//! unwinds straight past it) -- the Job's progress hook stayed
			//! pointed at this controller's mCancelProgress forever, a stale
			//! cancel hook that could confuse whatever installs (or fails to
			//! install) a progress callback on this Job for the NEXT render.
			//! Arm() once installed; the destructor restores (to whatever
			//! value was live before the install -- captured in-slot at the
			//! install site since the 2026-07-12 hardening: the platform's
			//! persistent callback on a live-GUI Job, nullptr headless --
			//! see doRenderWork's own comment) on every exit,
			//! ordinary or exceptional; the ordinary-path tail below calls
			//! Disarm() after its own explicit restore so the destructor is
			//! a no-op there (avoids a harmless but redundant double
			//! SetProgress call), exactly mirroring RenderOverrideRestoreGuard.
			class ProgressRestoreGuard
			{
			public:
				ProgressRestoreGuard( IJobPriv& job, IProgressCallback* priorValue )
					: mJob( job ), mPriorValue( priorValue ), mArmed( false )
				{
				}

				void Arm()    { mArmed = true; }
				void Disarm() { mArmed = false; }

				~ProgressRestoreGuard()
				{
					if( !mArmed ) return;
					try {
						mJob.SetProgress( mPriorValue );
					}
					catch( ... ) {
						// NEVER rethrow from a destructor (would terminate()
						// if already unwinding from Rasterize()'s own
						// exception).  Log so a future throw here is at
						// least diagnosable.
						GlobalLog()->PrintEx( eLog_Error,
							"AgentSession::Render: exception escaped the progress-hook "
							"restore -- the Job's progress callback may be left stale" );
					}
				}

			private:
				ProgressRestoreGuard( const ProgressRestoreGuard& );             // deleted
				ProgressRestoreGuard& operator=( const ProgressRestoreGuard& );  // deleted

				IJobPriv&           mJob;
				IProgressCallback*  mPriorValue;
				bool                mArmed;
			};

			//! Model-B F2 slice S3 (EffectiveRenderConfig): RAII restore of
			//! IRasterizer::SetSampleCountOverride, matching the SAME house
			//! shape as RenderOverrideRestoreGuard / ProgressRestoreGuard
			//! (Arm/Disarm, restore-on-every-exit-including-a-throw) so a
			//! render's sample-count override can NEVER be left applied
			//! past this one render -- including when Rasterize() throws
			//! (OIDN is a documented real throw site).  `mPriorSamples` is
			//! captured via GetSampleCountOverride() BEFORE the override is
			//! applied; the destructor re-applies it via
			//! SetSampleCountOverride so the rasterizer is left EXACTLY as
			//! found.  A no-op (never armed) when no override was
			//! requested, OR the rasterizer doesn't support the override
			//! (GetSampleCountOverride returned -1 -- nothing meaningful to
			//! restore).
			class SampleCountRestoreGuard
			{
			public:
				SampleCountRestoreGuard( IRasterizer& rast, int priorSamples )
					: mRast( rast ), mPriorSamples( priorSamples ), mArmed( false )
				{
				}

				void Arm()    { mArmed = true; }
				void Disarm() { mArmed = false; }

				~SampleCountRestoreGuard()
				{
					if( !mArmed ) return;
					if( mPriorSamples < 1 ) return;   // -1 (unsupported) or otherwise unknown -- nothing to restore
					try {
						mRast.SetSampleCountOverride( mPriorSamples );
					}
					catch( ... ) {
						// NEVER rethrow from a destructor (would terminate()
						// if already unwinding from Rasterize()'s own
						// exception).  Log so a future throw here is at
						// least diagnosable.
						GlobalLog()->PrintEx( eLog_Error,
							"AgentSession::Render: exception escaped the sample-count "
							"override restore -- the rasterizer's SPP may be left overridden" );
					}
				}

			private:
				SampleCountRestoreGuard( const SampleCountRestoreGuard& );             // deleted
				SampleCountRestoreGuard& operator=( const SampleCountRestoreGuard& );  // deleted

				IRasterizer& mRast;
				int          mPriorSamples;
				bool         mArmed;
			};

			//! GUI render modes P2b `render{light:}` surface (light solo):
			//! RAII restore of the production rasterizer's light-solo state,
			//! same Arm/Disarm/restore-on-every-exit house shape as
			//! SampleCountRestoreGuard above.  The PRODUCTION RayCaster is
			//! long-lived and shared across agent render calls (unlike the
			//! BeautyVariant path's ephemeral caster, which is destroyed
			//! with its pipeline and needs no restore) -- every light-solo
			//! render on it must leave it exactly as found (no solo, the
			//! steady-state default) once this render's scope exits,
			//! including on an exception unwinding out of mJob->Rasterize()
			//! (OIDN denoise is a documented real throw site).
			//!
			//! review-p2d P3-5: this used to CLEAR unconditionally, resting
			//! on a comment-enforced invariant that agent light-solo was the
			//! only writer of solo state on a production caster.  It was
			//! armed on EVERY production render (including ones with no
			//! `light` arg), so the first GUI-side solo toggle would have
			//! been silently dropped by the next agent render.  It now
			//! CAPTURES the real prior state at Arm() and restores exactly
			//! that -- removing the invariant rather than documenting it.
			class LightSoloRestoreGuard
			{
			public:
				explicit LightSoloRestoreGuard( RISE::Implementation::RayCaster* pCaster )
					: mCaster( pCaster ), mArmed( false )
				{
				}

				void Arm()
				{
					if( mCaster ) mPrior = mCaster->CaptureSoloState();
					mArmed = true;
				}
				void Disarm() { mArmed = false; }

				~LightSoloRestoreGuard()
				{
					if( !mArmed || !mCaster ) return;
					try {
						mCaster->RestoreSoloState( mPrior );
					}
					catch( ... ) {
						GlobalLog()->PrintEx( eLog_Error,
							"AgentSession::Render: exception escaped the light-solo "
							"restore -- the rasterizer may be left soloed to one light" );
					}
				}

			private:
				LightSoloRestoreGuard( const LightSoloRestoreGuard& );             // deleted
				LightSoloRestoreGuard& operator=( const LightSoloRestoreGuard& );  // deleted

				RISE::Implementation::RayCaster* mCaster;
				RISE::Implementation::RayCaster::SoloStateSnapshot mPrior;
				bool                              mArmed;
			};

			//! Offscreen isolation for agent/LLM renders: RAII restore of the
			//! active rasterizer's FrameStore IDENTITY, matching the SAME
			//! house shape as RenderOverrideRestoreGuard / ProgressRestoreGuard
			//! / SampleCountRestoreGuard (Arm/Disarm, restore-on-every-exit-
			//! including-a-throw).
			//!
			//! ROOT CAUSE this closes: an agent `render` call used to run
			//! mJob->Rasterize() directly on the PRODUCTION rasterizer, whose
			//! Implementation::Rasterizer::GetFrameStore() is the SAME
			//! canonical FrameStore the GUI's ViewportFrameStore is bound to
			//! via FrameStore::AddObserver -- independent of the `outs` sink
			//! list this class already swaps (RemoveRasterizerOutputs +
			//! AddRasterizerOutput(InMemoryRasterizerOutput) only ever
			//! touched `outs`, never the FrameStore).  Without this guard, an
			//! agent render's BeginTile/EndTile/MarkFrameComplete calls land
			//! in that shared store and fire the VFS's observer callbacks,
			//! visibly corrupting the interactive viewport with the agent's
			//! (possibly different-dims / different-camera-pose) pixels --
			//! reproducible on a GUI window resize after an agent render.
			//!
			//! doRenderWork installs a PRIVATE, unobserved FrameStore for the
			//! duration of the render (see the no-override install site
			//! below, and the override path's SetFilm-driven install via
			//! Job::PushJobFrameStoreToRasterizers).  This guard restores the
			//! rasterizer's FrameStore POINTER IDENTITY back to the EXACT
			//! object the VFS observes -- even when Rasterize() throws (OIDN
			//! is a documented real throw site), and even though the film-
			//! dims-override restore path's SetFilm(origFilmW, origFilmH, ...)
			//! reallocates a BRAND NEW FrameStore instance at the original
			//! dims (Job::EnsureJobFrameStore_locked reallocates on any dims
			//! change -- the restored store is a DIFFERENT object than the one
			//! the VFS was originally bound to, so without this identity
			//! restore the viewport would be left observing a stale,
			//! now-orphaned store).
			//!
			//! `mCaptured` is an OWNED reference: the caller addref()s the
			//! captured display FrameStore BEFORE constructing this guard;
			//! whichever path releases it -- this destructor (abnormal exit),
			//! or the ordinary-path explicit restore-then-Disarm() -- drops
			//! that ONE ref, exactly mirroring the `sink` / SinkUnwindGuard
			//! pattern elsewhere in this function.  A null `rast` (the
			//! dynamic_cast<Rasterizer*> failed -- no in-tree IRasterizer
			//! fails this cast today, but a future non-Implementation
			//! subclass might) or a null `capturedDisplayStore` (no
			//! FrameStore was bound before this render -- nothing to
			//! restore) makes every operation here a safe no-op.
			class FrameStoreIsolationGuard
			{
			public:
				FrameStoreIsolationGuard( Implementation::Rasterizer* rast,
				                          Implementation::FrameStore* capturedDisplayStore )
					: mRast( rast ), mCaptured( capturedDisplayStore ), mArmed( false )
				{
				}

				void Arm()    { mArmed = true; }
				void Disarm() { mArmed = false; }

				// Ordinary-path counterpart to the destructor.  Keep the guard
				// armed until SetFrameStore succeeds: if that call ever throws,
				// stack unwinding reaches the destructor, which retries the restore
				// without leaking the owned capture.  On success this consumes the
				// capture and leaves the destructor as a no-op.
				void RestoreAndDisarm()
				{
					if( !mArmed ) return;
					if( mRast ) {
						mRast->SetFrameStore( mCaptured );
					}
					safe_release( mCaptured );
					mArmed = false;
				}

				~FrameStoreIsolationGuard()
				{
					if( !mArmed ) return;
					try {
						if( mRast ) {
							mRast->SetFrameStore( mCaptured );
						}
					}
					catch( ... ) {
						// NEVER rethrow from a destructor (would terminate() if
						// already unwinding from Rasterize()'s own exception).
						// Log so a future throw here is at least diagnosable.
						GlobalLog()->PrintEx( eLog_Error,
							"AgentSession::Render: exception escaped the display-FrameStore "
							"restore -- viewport may be bound to a stale/private store" );
					}
					safe_release( mCaptured );
				}

			private:
				FrameStoreIsolationGuard( const FrameStoreIsolationGuard& );             // deleted
				FrameStoreIsolationGuard& operator=( const FrameStoreIsolationGuard& );  // deleted

				Implementation::Rasterizer* mRast;
				Implementation::FrameStore* mCaptured;
				bool                        mArmed;
			};

			// ---- Toolkit slice 3a (objectmap): palette + registry construction ----
			//
			// The exact colour contract (see ObjectMapPalette's header doc):
			// the shader emits a LINEAR pre-image L per channel so the final
			// encode -- (unsigned char)(SRGBTransferFunction(clamp01(L))*255)
			// (PNGWriter::WriteColor -> Integerize<sRGBPel>, a TRUNCATING
			// cast) -- lands on EXACTLY the reserved byte.  Targeting the
			// half-LSB center (B+0.5)/255 gives a half-LSB margin on both
			// sides, so the truncation is robust.

			inline Scalar ObjectMapLinearFromByte( unsigned char b )
			{
				return ColorUtils::SRGBTransferFunctionInverse(
					( static_cast<Scalar>( b ) + 0.5 ) / 255.0 );
			}

			inline RISEPel ObjectMapLinearFromBytes( const std::array<unsigned char, 3>& b )
			{
				return RISEPel( ObjectMapLinearFromByte( b[0] ),
				                ObjectMapLinearFromByte( b[1] ),
				                ObjectMapLinearFromByte( b[2] ) );
			}

			//! Does byte `b`'s half-LSB-centered linear pre-image re-encode
			//! (forward sRGB + truncating *255) back to EXACTLY `b`?  This is
			//! the quantizer contract, evaluated with the SAME arithmetic the
			//! encode path uses.  Under -ffast-math the sole non-roundtrippable
			//! value is 255: its target (255.5/255) exceeds 1.0 so the
			//! pre-image clamps to 1.0, and SRGBTransferFunction(1.0)*255 can
			//! land a hair below 255.0 and truncate to 254 -- there is no
			//! half-LSB headroom above white.  The palette generator rejects
			//! such bytes so the identity colours are exact BY CONSTRUCTION.
			inline bool ObjectMapByteRoundtrips( unsigned char b )
			{
				const Scalar L  = ObjectMapLinearFromByte( b );
				const Scalar Lc = L < 0 ? 0 : ( L > 1 ? 1 : L );
				Scalar enc = ColorUtils::SRGBTransferFunction( Lc );
				if( enc > 1.0 ) enc = 1.0;
				const unsigned char back = static_cast<unsigned char>( enc * 255.0 );   // TRUNCATING, matching Integerize<sRGBPel>
				return back == b;
			}

			inline bool ObjectMapBytesRoundtrip( const std::array<unsigned char, 3>& c )
			{
				return ObjectMapByteRoundtrips( c[0] )
				    && ObjectMapByteRoundtrips( c[1] )
				    && ObjectMapByteRoundtrips( c[2] );
			}

			std::string ObjectMapColorHex( const std::array<unsigned char, 3>& b )
			{
				char buf[8];
				std::snprintf( buf, sizeof( buf ), "#%02X%02X%02X",
				               static_cast<unsigned>( b[0] ),
				               static_cast<unsigned>( b[1] ),
				               static_cast<unsigned>( b[2] ) );
				return std::string( buf );
			}

			unsigned int ObjectMapL1( const std::array<unsigned char, 3>& a,
			                          const std::array<unsigned char, 3>& b )
			{
				const int dr = static_cast<int>( a[0] ) - static_cast<int>( b[0] );
				const int dg = static_cast<int>( a[1] ) - static_cast<int>( b[1] );
				const int db = static_cast<int>( a[2] ) - static_cast<int>( b[2] );
				return static_cast<unsigned int>(
					( dr < 0 ? -dr : dr ) + ( dg < 0 ? -dg : dg ) + ( db < 0 ? -db : db ) );
			}

			//! HSV (h in [0,1), s/v in [0,1]) -> 8-bit sRGB byte triple, for
			//! the golden-ratio hue walk that extends the base palette beyond
			//! its hand-picked entries.  Rounds to nearest byte.
			std::array<unsigned char, 3> ObjectMapHsvToBytes( double h, double s, double v )
			{
				h -= std::floor( h );
				const double hp = h * 6.0;
				const int    i  = static_cast<int>( std::floor( hp ) ) % 6;
				const double f  = hp - std::floor( hp );
				const double p  = v * ( 1.0 - s );
				const double q  = v * ( 1.0 - s * f );
				const double t  = v * ( 1.0 - s * ( 1.0 - f ) );
				double r = 0, g = 0, b = 0;
				switch( i ) {
					case 0: r = v; g = t; b = p; break;
					case 1: r = q; g = v; b = p; break;
					case 2: r = p; g = v; b = t; break;
					case 3: r = p; g = q; b = v; break;
					case 4: r = t; g = p; b = v; break;
					default: r = v; g = p; b = q; break;
				}
				auto toByte = []( double x ) -> unsigned char {
					int iv = static_cast<int>( x * 255.0 + 0.5 );
					if( iv < 0 ) iv = 0;
					if( iv > 255 ) iv = 255;
					return static_cast<unsigned char>( iv );
				};
				std::array<unsigned char, 3> out = { { toByte( r ), toByte( g ), toByte( b ) } };
				return out;
			}

			//! Assign `count` well-separated 8-bit sRGB byte triples, each at
			//! least L1 kMinL1 from every prior assignment AND from every
			//! `reserved` colour (background + UNKNOWN).  A fixed high-contrast
			//! base list covers the common small-count case; a deterministic
			//! golden-ratio hue walk (alternating value/saturation) extends it.
			//!
			//! TWO NON-NEGOTIABLE INVARIANTS, held for EVERY emitted colour even
			//! when the palette is exhausted at large counts:
			//!   * BYTE-UNIQUENESS -- no two ids ever share a triple (legend
			//!     matching is by exact byte, so a dup would make two objects
			//!     indistinguishable in the map).  Achievable to ~16.7M colours.
			//!   * ROUNDTRIP -- the half-LSB-centered linear pre-image re-encodes
			//!     to the exact byte (the quantizer contract).
			//! Only the MIN-L1 SEPARATION degrades under pressure: the walk
			//! retries the whole palette at 24 -> 12 -> 6 -> 1, keeping the two
			//! invariants above at every relaxation.  `outMinDistanceUsed` (when
			//! non-null) reports the smallest separation that was actually
			//! needed (== kDefaultMinL1 when the palette never degraded), so the
			//! caller can append an honest "closer than default" note.
			const unsigned int kDefaultMinL1 = 24;

			std::vector<std::array<unsigned char, 3> > BuildObjectMapPaletteBytes(
				std::size_t count, const std::vector<std::array<unsigned char, 3> >& reserved,
				unsigned int* outMinDistanceUsed = nullptr,
				unsigned int goldenTries = 4096 /* test seam: 0 forces the exhaustive branch */ )
			{
				// 24 hand-picked, mutually well-separated triples (Trubetskoy-
				// style distinct-colour set), excluding (0,0,0) background and
				// (255,0,255) UNKNOWN.
				static const std::array<unsigned char, 3> kBase[] = {
					{ { 230,  25,  75 } }, { {  60, 180,  75 } }, { { 255, 225,  25 } }, { {   0, 130, 200 } },
					{ { 245, 130,  48 } }, { { 145,  30, 180 } }, { {  70, 240, 240 } }, { { 240,  50, 230 } },
					{ { 210, 245,  60 } }, { { 250, 190, 190 } }, { {   0, 128, 128 } }, { { 230, 190, 255 } },
					{ { 170, 110,  40 } }, { { 255, 250, 200 } }, { { 128,   0,   0 } }, { { 170, 255, 195 } },
					{ { 128, 128,   0 } }, { { 255, 215, 180 } }, { {   0,   0, 128 } }, { { 128, 128, 128 } },
					{ { 255, 255, 255 } }, { { 100, 100, 255 } }, { { 255, 100, 100 } }, { { 100, 255, 100 } }
				};
				const std::size_t  kBaseCount = sizeof( kBase ) / sizeof( kBase[0] );

				// A packed-byte set of every triple already accepted, so
				// byte-uniqueness is enforced in O(1) regardless of the L1
				// relaxation level (the L1 walk can, at min-L1==1, still hand
				// back a same-byte candidate -- the set is what forbids it).
				std::unordered_set<std::uint32_t> takenKeys;
				auto keyOf = []( const std::array<unsigned char, 3>& c ) -> std::uint32_t {
					return ( static_cast<std::uint32_t>( c[0] ) << 16 )
					     | ( static_cast<std::uint32_t>( c[1] ) <<  8 )
					     |   static_cast<std::uint32_t>( c[2] );
				};

				// The RESERVED colours (background, UNKNOWN) are interned up
				// front so NO acceptance path can ever hand an object the
				// literal background/unknown bytes.  This matters ONLY for the
				// exhaustive last-resort scan below, which deliberately skips
				// farEnough (the golden ladder's reserved check): that scan
				// starts at rgb=0 == the reserved background, which round-trips
				// and was never in the set -- pre-fix, the FIRST id to reach
				// the exhaustive path was deterministically painted as
				// background (closing-review P1).
				for( std::size_t r = 0; r < reserved.size(); ++r ) {
					takenKeys.insert( keyOf( reserved[r] ) );
				}

				std::vector<std::array<unsigned char, 3> > out;
				out.reserve( count );

				auto farEnough = [&]( const std::array<unsigned char, 3>& c, unsigned int minL1 ) -> bool {
					for( std::size_t r = 0; r < reserved.size(); ++r )
						if( ObjectMapL1( c, reserved[r] ) < minL1 ) return false;
					for( std::size_t a = 0; a < out.size(); ++a )
						if( ObjectMapL1( c, out[a] ) < minL1 ) return false;
					return true;
				};

				const double kGolden  = 0.6180339887498949;
				const double sats[2]  = { 0.90, 0.62 };
				const double vals[3]  = { 0.98, 0.72, 0.86 };

				// ≤~200-object FAST PATH (allocation-identical to the pre-fix
				// code at the target scale): try base list, then the 4096-try
				// golden walk, at the DEFAULT separation.  A placed colour is
				// interned in `takenKeys`.  If EVERY colour places here (the
				// common case), we never touch the degrade machinery.
				unsigned int minDistanceUsed = kDefaultMinL1;

				auto tryPlaceAt = [&]( std::size_t i, unsigned int minL1,
				                       std::array<unsigned char, 3>& c ) -> bool {
					// Base entry (only at the default separation -- once we are
					// relaxing, the base list is already exhausted by definition).
					if( minL1 == kDefaultMinL1 && i < kBaseCount
					    && takenKeys.find( keyOf( kBase[i] ) ) == takenKeys.end()
					    && farEnough( kBase[i], minL1 ) && ObjectMapBytesRoundtrip( kBase[i] ) ) {
						c = kBase[i];
						return true;
					}
					for( unsigned int tries = 0; tries < goldenTries; ++tries ) {
						const double h = std::fmod( 0.11 + static_cast<double>( i + tries ) * kGolden, 1.0 );
						const double s = sats[ ( i + tries ) % 2 ];
						const double v = vals[ ( i + tries ) % 3 ];
						const std::array<unsigned char, 3> cand = ObjectMapHsvToBytes( h, s, v );
						if( takenKeys.find( keyOf( cand ) ) == takenKeys.end()
						    && farEnough( cand, minL1 ) && ObjectMapBytesRoundtrip( cand ) ) {
							c = cand;
							return true;
						}
					}
					return false;
				};

				for( std::size_t i = 0; i < count; ++i ) {
					std::array<unsigned char, 3> c = { { 0, 0, 0 } };
					bool placed = tryPlaceAt( i, kDefaultMinL1, c );

					// DEGRADE: relax ONLY the separation, keeping uniqueness +
					// roundtrip.  We retry the golden walk for THIS id at each
					// smaller threshold; if even min-L1==1 can't place, fall to
					// an exhaustive scan of the whole cube for the first unused,
					// round-trippable byte (guaranteed to exist below ~16.7M ids).
					if( !placed ) {
						static const unsigned int kRelaxSteps[] = { 12, 6, 1 };
						for( std::size_t s = 0; s < sizeof( kRelaxSteps ) / sizeof( kRelaxSteps[0] ) && !placed; ++s ) {
							if( tryPlaceAt( i, kRelaxSteps[s], c ) ) {
								placed = true;
								if( kRelaxSteps[s] < minDistanceUsed ) minDistanceUsed = kRelaxSteps[s];
							}
						}
					}
					if( !placed ) {
						// Exhaustive last resort: the first byte triple not yet
						// taken that round-trips.  Guarantees uniqueness even
						// past the point where ANY positive separation is
						// achievable (this is where `takenKeys` is the SOLE
						// uniqueness authority -- the golden ladder above relies
						// on farEnough for it; this scan does not).  minDistanceUsed
						// collapses to 1 (colours are byte-unique but may be
						// visually adjacent).
						for( unsigned int rgb = 0; rgb < 0x1000000u && !placed; ++rgb ) {
							std::array<unsigned char, 3> cand = { {
								static_cast<unsigned char>( ( rgb >> 16 ) & 0xFF ),
								static_cast<unsigned char>( ( rgb >>  8 ) & 0xFF ),
								static_cast<unsigned char>(   rgb         & 0xFF ) } };
							if( takenKeys.find( keyOf( cand ) ) == takenKeys.end()
							    && ObjectMapBytesRoundtrip( cand ) ) {
								c = cand;
								placed = true;
								minDistanceUsed = 1;
							}
						}
					}

					takenKeys.insert( keyOf( c ) );
					out.push_back( c );
				}

				if( outMinDistanceUsed ) *outMinDistanceUsed = minDistanceUsed;
				return out;
			}

			//! Build the full ObjectMapPalette from the scene's ObjectManager:
			//! enumerate object names in deterministic (sorted, per
			//! GenericManager's std::map) order, assign each a separated
			//! identity colour + its linear pre-image, map the manager's stored
			//! IObjectPriv* (== RayIntersection::pObject) to its id, and size
			//! the zero-initialized atomic tally (one per id + 1 for UNKNOWN).
			void BuildObjectMapPalette( IObjectManager* objMgr,
			                            RISE::Implementation::ObjectMapPalette& out )
			{
				struct NameCollector : public IEnumCallback<const char*>
				{
					std::vector<std::string> names;
					bool operator()( const char* const& n ) override
					{
						if( n ) names.push_back( std::string( n ) );
						return true;
					}
				} collector;
				if( objMgr ) objMgr->EnumerateItemNames( collector );

				// Legend semantics: ONE entry per WORLD-VISIBLE object -- the
				// objects a ray can actually land on and the agent can select.
				// A composite's operands (a CSGObject's children are
				// SetWorldVisible(false) by AssignObjects, and re-report the
				// composite root as ri.pObject) are ObjectManager items but are
				// NOT independently hit, so they must not appear in the legend.
				// Filtering here keeps the palette, registry, id-space, and
				// per-id atomic tally all consistent over the visible set only.
				// P3-b: resolve each object POINTER once here and carry it
				// forward in `objs` (parallel to `names`), so the registry loop
				// below reuses it instead of a second GetItem per object.
				std::vector<std::string> names;
				std::vector<IObjectPriv*> objs;
				names.reserve( collector.names.size() );
				objs.reserve( collector.names.size() );
				for( std::size_t i = 0; i < collector.names.size(); ++i ) {
					IObjectPriv* obj = objMgr ? objMgr->GetItem( collector.names[i].c_str() ) : 0;
					if( obj && static_cast<const IObject*>( obj )->IsWorldVisible() ) {
						names.push_back( collector.names[i] );
						objs.push_back( obj );
					}
				}
				const std::size_t count = names.size();

				const std::array<unsigned char, 3> background = { { 0, 0, 0 } };
				const std::array<unsigned char, 3> unknown    = { { 255, 0, 255 } };   // magenta

				std::vector<std::array<unsigned char, 3> > reserved;
				reserved.push_back( background );
				reserved.push_back( unknown );
				unsigned int minDistanceUsed = 24;
				const std::vector<std::array<unsigned char, 3> > assigned =
					BuildObjectMapPaletteBytes( count, reserved, &minDistanceUsed );

				out.names = names;
				out.bytes = assigned;
				out.minColorDistance = minDistanceUsed;
				out.linearColors.resize( count );
				out.registry.clear();
				out.registry.reserve( count );
				for( std::size_t id = 0; id < count; ++id ) {
					out.linearColors[id] = ObjectMapLinearFromBytes( assigned[id] );
					if( objs[id] ) {
						out.registry[ static_cast<const IObject*>( objs[id] ) ] = static_cast<std::uint32_t>( id );
					}
				}
				out.unknownBytes  = unknown;
				out.unknownLinear = ObjectMapLinearFromBytes( unknown );

				// vector<atomic> is movable but not copyable; move-assign a
				// freshly sized one, then zero each slot explicitly (pre-C++20
				// the atomic default ctor leaves the value uninitialized).
				out.counts = std::vector<std::atomic<std::uint32_t> >( count + 1 );
				for( std::size_t i = 0; i < out.counts.size(); ++i ) {
					out.counts[i].store( 0u, std::memory_order_relaxed );
				}
			}
		}

		// GUI render modes P2a `render{view:}` surface (docs/gui/
		// RENDER_MODES.md §8): format a Vector3/Scalar into the SAME string
		// shape CameraIntrospection::SetProperty accepts, so a resolved
		// CameraSnapshot (from a live controller's named-view store, or
		// CameraIntrospection::CaptureCameraSnapshot on a headless scene
		// camera) can feed straight into the EXISTING AgentCameraOverride /
		// applyCameraOverride plumbing.
		std::string Vec3ToOverrideStr( const double v[3] )
		{
			char buf[96];
			std::snprintf( buf, sizeof( buf ), "%.17g %.17g %.17g", v[0], v[1], v[2] );
			return std::string( buf );
		}

		std::string OrientationToOverrideStr( const double v[3] )
		{
			static constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
			double degrees[3] = { v[0] * kRadToDeg, v[1] * kRadToDeg, v[2] * kRadToDeg };
			return Vec3ToOverrideStr( degrees );
		}

		// Fill `out`'s complete CameraCommon pose (location/lookAt/up plus
		// Euler and target orientation) and fov (only for a Pinhole snapshot
		// -- a ThinLens/Fisheye/Orthographic view has no single scalar FOV)
		// from a captured CameraSnapshot.
		void CameraSnapshotToOverride( const RISE::CameraSnapshot& snap,
		                                AgentCameraOverride& out )
		{
			out.hasLocation = true; out.location = Vec3ToOverrideStr( snap.location );
			out.hasLookAt   = true; out.lookAt   = Vec3ToOverrideStr( snap.lookat );
			out.hasUp       = true; out.up       = Vec3ToOverrideStr( snap.up );
			out.hasOrientation = true;
			out.orientation = OrientationToOverrideStr( snap.orientation );
			double targetOrientation[3] = {
				snap.target_orientation[0], snap.target_orientation[1], 0.0
			};
			out.hasTargetOrientation = true;
			out.targetOrientation = OrientationToOverrideStr( targetOrientation );
			if( snap.type == RISE::CameraSnapshot::Pinhole ) {
				// CameraSnapshot::fov is stored in RADIANS (CameraIntrospection.cpp's
				// own CaptureCameraSnapshot / GetFovStored convention); SetProperty's
				// "fov" -- and AgentCameraOverride::fov's documented contract -- both
				// take DEGREES (matching the scene-file `fov` param).  Convert on the
				// way out, mirroring CameraIntrospection::GetPropertyValue's own
				// RAD_TO_DEG conversion for the SAME field.
				static constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
				char buf[48];
				std::snprintf( buf, sizeof( buf ), "%.17g", snap.fov * kRadToDeg );
				out.hasFov = true; out.fov = buf;
			}
		}

		// GUI render modes P2b `render{light:}` surface (docs/gui/
		// RENDER_MODES.md §3 "light solo"): resolves `lightName` against
		// `caster`'s attached scene and designates it the sole active
		// light, mirroring Job::SetActiveRasterizerRadianceScale's
		// PixelBasedRasterizerHelper -> concrete RayCaster downcast (the
		// SAME dynamic_cast pattern; every in-tree pixel-based rasterizer
		// -- PT / BDPT / VCM / MLT and the spectral variants -- derives
		// from PixelBasedRasterizerHelper, which owns the RayCaster).
		// `caster` here is already the concrete IRayCaster* (the
		// BeautyVariant path's ephemeral `variantCaster`, or the
		// production rasterizer's own caster reached via GetRayCaster()
		// at the call site) -- this function does the FINAL downcast to
		// the concrete RayCaster that actually owns SetSoloLightByName.
		// \return True if `lightName` is empty (nothing to do) or
		// resolved; false with `outMessage` populated with the "unknown
		// light" + available-name list on an unresolved name, or a
		// generic "no ray caster" message if `caster` isn't a concrete
		// RayCaster (should not happen for any in-tree rasterizer).
		//! review-p2d P1-2: LIGHT SOLO IS A PATH-TRACING MECHANISM ONLY.
		//!
		//! Every solo touchpoint in LightSampler lives in
		//! EvaluateDirectLighting{,NM} and CachedPdfSelectLuminary; the
		//! generic entry points BDPT/VCM/MLT use -- SampleLight,
		//! PdfSelectLight, PdfSelectLuminary, SelectLightRIS, LightBVH::Pdf
		//! -- have no solo branch and still see the full scene
		//! distribution.  So:
		//!   * VCM / MLT: solo is a complete no-op (all lights render).
		//!   * BDPT: WORSE than a no-op -- its eye-subpath NEE goes through
		//!     EvaluateDirectLighting (soloed) while light-subpath
		//!     generation goes through SampleLight (not soloed), giving a
		//!     mixed, mutually-inconsistent estimator.
		//!
		//! ApplyLightSoloByName's downcast reaches the RayCaster of ANY
		//! PixelBasedRasterizerHelper, which all of these are -- so without
		//! this gate a `vcm_pel_rasterizer` scene returned ok:true plus the
		//! note "light solo: X is the only active light" over an image with
		//! every light lit.  Silently claiming an effect that did not happen
		//! is worse than either fixing it or refusing it, so refuse loudly,
		//! matching the unresolvable-name and non-pinhole-`view` contracts.
		//!
		//! Deliberately a WHITELIST, not a blacklist: `auto_*` rasterizers
		//! resolve to PT/BDPT/VCM at render time, so they cannot be cleared
		//! statically and are (conservatively) refused.  A new PT-family
		//! rasterizer must opt in here explicitly rather than inherit a
		//! silent claim it may not honour.
		bool RasterizerSupportsLightSolo( IRasterizer* rast )
		{
			return dynamic_cast<RISE::Implementation::PathTracingPelRasterizer*>( rast ) != nullptr
			    || dynamic_cast<RISE::Implementation::PathTracingSpectralRasterizer*>( rast ) != nullptr;
		}

		bool ApplyLightSoloByName( IRayCaster* caster, const std::string& lightName,
		                           std::string& outMessage )
		{
			if( lightName.empty() ) {
				return true;
			}

			RISE::Implementation::RayCaster* pConcreteCaster =
				dynamic_cast<RISE::Implementation::RayCaster*>( caster );
			if( !pConcreteCaster ) {
				outMessage = "light \"" + lightName + "\" could not be applied -- the active rasterizer has no ray caster";
				return false;
			}

			std::string available;
			if( !pConcreteCaster->SetSoloLightByName( lightName.c_str(), &available ) ) {
				outMessage = "unknown light \"" + lightName + "\"";
				outMessage += available.empty()
					? std::string( " -- no lights or emissive objects available" )
					: ( " -- available: " + available );
				return false;
			}

			return true;
		}

		bool DecodePngRgbAt( const std::vector<unsigned char>& png,
		                     unsigned int x, unsigned int y,
		                     unsigned char& outR, unsigned char& outG, unsigned char& outB )
		{
			if( png.empty() || png.size() > static_cast<std::size_t>( UINT_MAX ) ) return false;
			Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer(
				const_cast<char*>( reinterpret_cast<const char*>( png.data() ) ),
				static_cast<unsigned int>( png.size() ), false );
			IRasterImageReader* reader = nullptr;
			if( !RISE_API_CreatePNGReader( &reader, *buffer, eColorSpace_Rec709RGB_Linear ) || !reader ) {
				safe_release( buffer );
				return false;
			}

			unsigned int width = 0, height = 0;
			if( !reader->BeginRead( width, height ) || x >= width || y >= height ) {
				safe_release( reader );
				safe_release( buffer );
				return false;
			}
			RISEColor pixel;
			reader->ReadColor( pixel, x, y );
			reader->EndRead();
			safe_release( reader );
			safe_release( buffer );

			auto toByte = []( double value ) -> unsigned char {
				const int rounded = static_cast<int>( value * 255.0 + 0.5 );
				return static_cast<unsigned char>( rounded < 0 ? 0 : ( rounded > 255 ? 255 : rounded ) );
			};
			outR = toByte( pixel.base.r );
			outG = toByte( pixel.base.g );
			outB = toByte( pixel.base.b );
			return true;
		}

		// External review P2 fix (ResolveBeautyDisplayTransform_'s exposure-as-
		// expr(...) gap): a `file_rasterizeroutput` numeric param (e.g.
		// `exposure`) can be authored as `expr(...)` over the document's `let`
		// bindings (the CST v7 computed-value grammar -- see the `let` chunk's
		// doc in Cst.cpp).  DeriveToJob evaluates that GENERICALLY for every
		// chunk param before the value ever reaches Job::AddFileRasterizerOutput
		// (Cst.cpp's ResolveChunkParams -> TryEvalExprValue), so the actual
		// rendered/file-written exposure is the EVALUATED number -- never the
		// literal "expr(...)" text.  ResolveBeautyDisplayTransform_ necessarily
		// reads the RAW, PRE-derive CST text instead (Job exposes no post-
		// derive accessor for a stripped file output -- see that function's own
		// doc), so a plain std::strtod on an expr(...) value silently parsed to
		// 0 instead of the real exposure: a materially wrong "CLI parity"
		// render reported as success.
		//
		// Cst.cpp keeps its own let-collection/expr-eval (CollectLetBindings /
		// TryEvalExprValue / IsExprValue) `static` -- Cst.h exposes no resolved-
		// numeric-value accessor to call instead (the CST "resolver" that DOES
		// have a public surface, TraceReferences/BuildReferenceGraph, resolves
		// NAME references like `material foo`, not arithmetic expr(...)
		// values -- a different resolver entirely).  Rather than leave the gap
		// or duplicate the maths, this reuses the SAME public
		// RISE::Implementation::ExpressionProgram engine Cst.cpp's evaluator is
		// built on (Painters/ExpressionEval.h) -- only the "walk `let` chunks in
		// document order, evaluate expr(...) over EARLIER lets" bookkeeping is
		// repeated (deliberately mirroring Cst.cpp's CollectLetBindings/
		// TryEvalExprValue shape line-for-line), so the arithmetic result is
		// byte-identical to what the actual derive already computed when the
		// head was loaded.
		typedef std::vector<std::pair<std::string,double> > LocalLetBindings;

		//! Is `value` exactly `expr( <balanced> )`?  If so set `body` to the
		//! inside.  Mirrors Cst.cpp's IsExprValue exactly (same balanced-paren
		//! scan), so a value this rejects is ALSO rejected by the derive-time
		//! evaluator (never a false positive/negative divergence between the
		//! two).
		bool LocalIsExprValue( const std::string& value, std::string& body )
		{
			if( value.size() < 6 || value.compare( 0, 5, "expr(" ) != 0 || value.back() != ')' ) return false;
			int depth = 0; std::size_t lastClose = std::string::npos;
			for( std::size_t k = 4; k < value.size(); ++k ) {
				if( value[k] == '(' ) ++depth;
				else if( value[k] == ')' ) { if( --depth == 0 ) { lastClose = k; break; } }
			}
			if( lastClose != value.size() - 1 ) return false;
			body = value.substr( 5, value.size() - 6 );
			return true;
		}

		//! Compile + evaluate one expr BODY over the numeric `lets` + PI/E
		//! (added after the lets, matching Cst.cpp's EvalExprBody so a let
		//! cannot shadow them either).  u/v are bound to 0 -- a scene-level
		//! expr (unlike an instance_array component) has no per-instance
		//! coordinate.  Returns false (never a silent 0) on a compile error or
		//! a non-finite result.
		bool LocalEvalExprBody( const std::string& body, const LocalLetBindings& lets, double& outVal )
		{
			RISE::Implementation::ExpressionProgram::Builder b;
			for( const std::pair<std::string,double>& L : lets ) {
				b.AddParam( L.first, static_cast<RISE::Scalar>( L.second ) );
			}
			b.AddParam( "PI", static_cast<RISE::Scalar>( 3.14159265358979323846 ) );
			b.AddParam( "E",  static_cast<RISE::Scalar>( 2.71828182845904523536 ) );
			RISE::Implementation::ExpressionProgram prog = RISE::Implementation::ExpressionProgram::Invalid();
			if( !b.Finalize( body, prog ) || !prog.IsValid() ) return false;
			const RISE::Scalar r = prog.Eval( RISE::Scalar( 0 ), RISE::Scalar( 0 ) );
			if( !RISE::Implementation::ExpressionProgram::IsFinite( r ) ) return false;
			outVal = static_cast<double>( r );
			return true;
		}

		//! Walk `doc`'s top-level `let` chunks in DOCUMENT ORDER, evaluating
		//! each binding exactly as Cst.cpp's CollectLetBindings does: a literal
		//! is one finite numeric token; an expr(...) binding evaluates over the
		//! EARLIER lets only (lexical scope, no forward/cyclic refs -- a
		//! binding that fails to evaluate is skipped, matching
		//! CollectLetBindings' diagnose-and-continue).  A scene that loaded
		//! successfully already passed this same check at derive time, so a
		//! skip here is unreachable in practice for a live head; defensive
		//! only.
		LocalLetBindings CollectLocalLetBindings( const RISE::Cst::Document& doc )
		{
			LocalLetBindings out;
			const int n = RISE::Cst::DocItemCount( doc );
			for( int i = 0; i < n; ++i ) {
				const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
				const RISE::Cst::NodeRef node = id ? RISE::Cst::DocResolveNodeId( doc, id ) : RISE::Cst::NodeRef();
				if( !node || node->kind != RISE::Cst::NodeKind::Chunk || node->role != "let" ) continue;
				for( const auto& kid : node->kids ) {
					if( kid->kind != RISE::Cst::NodeKind::Param ) continue;
					std::string name, raw; bool first = true;
					for( const auto& tk : kid->kids ) {
						if( tk->kind != RISE::Cst::NodeKind::Token ) continue;
						if( first ) { name = tk->text; first = false; }
						else { if( !raw.empty() ) raw += ' '; raw += tk->text; }
					}
					if( name.empty() ) continue;
					std::string body; double val = 0.0;
					if( LocalIsExprValue( raw, body ) ) {
						if( !LocalEvalExprBody( body, out, val ) ) continue;
					} else {
						char* end = nullptr;
						val = std::strtod( raw.c_str(), &end );
						if( raw.empty() || end != raw.c_str() + raw.size() ) continue;
					}
					out.push_back( std::make_pair( name, val ) );
				}
			}
			return out;
		}

		//! Resolve a chunk param's RAW CST text (as `ResolveBeautyDisplayTransform_`'s
		//! `paramValue` lambda returns it) to its numeric value: a plain finite
		//! literal parses via std::strtod exactly as before; an expr(...) value
		//! is evaluated over `doc`'s `let` bindings via the helpers above.
		//! Returns false (leaving `outVal` untouched) when `raw` is neither a
		//! plain literal nor a resolvable expr(...) -- the caller keeps its own
		//! pre-set default in that case (documented at the one call site) rather
		//! than mis-reporting a parse failure as "the scene authored 0".
		bool ResolveParamNumeric( const std::string& raw, const RISE::Cst::Document& doc, double& outVal )
		{
			if( raw.empty() ) return false;
			std::string body;
			if( LocalIsExprValue( raw, body ) ) {
				return LocalEvalExprBody( body, CollectLocalLetBindings( doc ), outVal );
			}
			char* end = nullptr;
			const double v = std::strtod( raw.c_str(), &end );
			if( end != raw.c_str() + raw.size() ) return false;   // trailing garbage -- not a plain literal (e.g. a bare let-NAME reference, only valid wrapped in expr(...) per the CST grammar)
			outVal = v;
			return true;
		}

		void AgentSession::ForTest_BuildObjectMapPaletteBytes(
			std::size_t count,
			std::vector<std::array<unsigned char, 3> >& outBytes,
			unsigned int& outMinDistanceUsed,
			unsigned int forTestGoldenTries )
		{
			// Same reserved set the production BuildObjectMapPalette uses
			// (background + UNKNOWN) so the test measures the identical
			// generator behaviour.
			const std::array<unsigned char, 3> background = { { 0, 0, 0 } };
			const std::array<unsigned char, 3> unknown    = { { 255, 0, 255 } };
			std::vector<std::array<unsigned char, 3> > reserved;
			reserved.push_back( background );
			reserved.push_back( unknown );
			outMinDistanceUsed = 24;
			outBytes = BuildObjectMapPaletteBytes( count, reserved, &outMinDistanceUsed,
			                                       forTestGoldenTries );
		}

		AgentRenderResult AgentSession::Render( int samplesOverride )
		{
			// Legacy entry point: build an all-absent AgentRenderParams so
			// this is BYTE-COMPATIBLE with the pre-preview-render behaviour
			// apart from `samplesOverride`, which forwards into
			// params.samples below and is honoured IDENTICALLY to a direct
			// Render(AgentRenderParams) call -- Model-B F2 slice S3 gave
			// this a real, non-mutating effect (IRasterizer::
			// SetSampleCountOverride) for the pixel-based rasterizer family
			// (PT, spectral PT, BDPT, VCM); on an unsupported rasterizer
			// (MLT, photon-map-only, ...) it is honestly reported as NOT
			// applied via res.samplesOverridden/res.message, never silently
			// ignored.  See RenderCore_'s doc for the full mechanism.
			AgentRenderParams params;
			params.samples = samplesOverride;
			return RenderCore_( params );
		}

		AgentRenderResult AgentSession::Render( const AgentRenderParams& params )
		{
			return RenderCore_( params );
		}

		void AgentSession::ResolveBeautyDisplayTransform_( double& outExposureEV,
		                                                   int& outDisplayTransform,
		                                                   int& outColorSpace ) const
		{
			// LDR defaults, matching the CLI file-output pipeline: ACES filmic,
			// 0 EV.  An agent render is always an 8-bit PNG preview, so even a
			// head with NO file output (the image_reconstruct scaffolds) or an
			// HDR-only output still gets a viewable tone curve.
			double exposureEV = 0.0;
			int    dt         = 2 /*eDisplayTransform_ACES*/;
			// External review P2 fix: the resolved OUTPUT COLOUR SPACE, matching
			// the descriptor's own `defaultValueHint = "sRGB"` for
			// file_rasterizeroutput's `color_space` param (ChunkParserRegistry.cpp)
			// -- was previously never read at all; the in-memory PNG sink just
			// hardcoded eColorSpace_sRGB unconditionally regardless of what the
			// scene declared.
			int    cs         = eColorSpace_sRGB;

			// Honour a declared LDR file_rasterizeroutput's tone curve +
			// exposure so a compareToImage grading render (and read_image of a
			// scene the author configured with, say, `display_transform none`)
			// is BYTE-IDENTICAL to a CLI PNG render of that same scene.  Read
			// straight from the retained CST (the source of truth; the parsed
			// file outputs carry no post-load accessor and are stripped from
			// the rasterizer by the render itself).
			if( mJob ) {
				const RISE::Cst::Document* doc = mJob->GetCstDocument();
				if( doc ) {
					// Concatenate a chunk param's pvalue tokens (space-joined);
					// empty when the param is absent or valueless.
					auto paramValue = []( const RISE::Cst::NodeRef& chunk,
					                      const char* pname ) -> std::string {
						if( !chunk ) return std::string();
						for( const auto& kid : chunk->kids ) {
							if( kid->kind != RISE::Cst::NodeKind::Param ) continue;
							std::string name;
							std::string value;
							for( const auto& tk : kid->kids ) {
								if( tk->kind != RISE::Cst::NodeKind::Token ) continue;
								if( tk->role == "pname" ) name = tk->text;
								else if( tk->role == "pvalue" ) {
									if( !value.empty() ) value += ' ';
									value += tk->text;
								}
							}
							if( name == pname ) return value;
						}
						return std::string();
					};

					const int n = RISE::Cst::DocItemCount( *doc );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( *doc, i );
						const RISE::Cst::NodeRef node = id ? RISE::Cst::DocResolveNodeId( *doc, id ) : RISE::Cst::NodeRef();
						if( !node || node->kind != RISE::Cst::NodeKind::Chunk ) continue;
						if( node->role != "file_rasterizeroutput" ) continue;

						// P2 fix: scan ALL file_rasterizeroutput chunks in document
						// order and adopt the FIRST **LDR** one's declared curve +
						// exposure (matches the CLI, whose first PNG output produced
						// these references) -- an HDR output is SKIPPED, not stopped
						// on, so a LATER LDR output's transform is still found. The
						// pre-fix code `break`-ed after the FIRST output regardless
						// of kind: a scene declaring an HDR output first and an LDR
						// output with, say, `display_transform none` second wrongly
						// fell back to the ACES/0EV default (set above the loop)
						// instead of honouring the LDR output's declared curve.
						const std::string type = paramValue( node, "type" );
						const bool typeIsHDR = ( type == "HDR" || type == "RGBEA" || type == "EXR" );
						if( typeIsHDR ) continue;   // keep scanning -- a linear archival curve would blow out the 8-bit PNG preview anyway

						// LDR: adopt its declared curve (default ACES) + exposure,
						// then stop -- this is the "first LDR output wins" output.
						const std::string dtStr = paramValue( node, "display_transform" );
						if     ( dtStr == "none"     ) dt = 0;
						else if( dtStr == "reinhard" ) dt = 1;
						else if( dtStr == "aces"     ) dt = 2;
						else if( dtStr == "agx"      ) dt = 3;
						else if( dtStr == "hable"    ) dt = 4;
						// (absent/unknown -> keep the ACES default)

						// External review P2 fix: `exStr` is the RAW pre-derive CST
						// text -- it may be a plain literal ("1.5") OR an expr(...)
						// value ("expr( BASE_EV + 0.5 )") the CST v7 `let`/`expr`
						// grammar supports for ANY chunk param, DeriveToJob evaluates
						// generically (Cst.cpp's ResolveChunkParams), and the CLI file-
						// output pipeline therefore honours for real.  The pre-fix
						// `std::strtod` here silently parsed an expr(...) string to 0
						// -- a materially wrong exposure reported as a byte-parity
						// render.  ResolveParamNumeric (above) evaluates expr(...) via
						// the SAME public ExpressionProgram engine the derive-time
						// evaluator is built on; a value it still can't resolve (a
						// malformed literal, or an expr(...) that fails to compile --
						// unreachable for a scene that loaded successfully, since
						// DeriveToJob runs the identical evaluator at load time) leaves
						// exposureEV at its pre-loop default (0 EV) rather than
						// mis-reporting the failure as "the scene authored exposure 0".
						const std::string exStr = paramValue( node, "exposure" );
						if( !exStr.empty() ) {
							double resolvedExposure = 0.0;
							if( ResolveParamNumeric( exStr, *doc, resolvedExposure ) ) {
								exposureEV = resolvedExposure;
							}
						}

						// External review P2 fix: honour the scene's declared output
						// colour space instead of the previous hardcoded sRGB.  Always
						// a plain enum token (never expr(...) -- an expr evaluates to a
						// NUMBER, which can't match one of these string literals), so a
						// direct string map is exact; matches ChunkParserRegistry.cpp's
						// own color_space parsing for this same chunk one-for-one.
						const std::string csStr = paramValue( node, "color_space" );
						if     ( csStr == "Rec709RGB_Linear" ) cs = eColorSpace_Rec709RGB_Linear;
						else if( csStr == "sRGB" )             cs = eColorSpace_sRGB;
						else if( csStr == "ROMMRGB_Linear" )   cs = eColorSpace_ROMMRGB_Linear;
						else if( csStr == "ProPhotoRGB" )      cs = eColorSpace_ProPhotoRGB;
						// (absent/unknown -> keep the sRGB default)

						break;
					}
				}

				// Stack the active camera's exposure compensation, exactly as
				// FileRasterizerOutput stacks Meta().cameraExposureEV onto the
				// static exposure at encode time (FrameEncoders.cpp totalEV).
				ICameraManager* cams = mJob->GetCameras();
				const std::string activeName = mJob->GetActiveCameraName();
				const ICamera* cam = ( cams && !activeName.empty() )
					? cams->GetItem( activeName.c_str() ) : nullptr;
				if( cam ) {
					exposureEV += static_cast<double>( cam->GetExposureCompensationEV() );
				}
			}

			outExposureEV       = exposureEV;
			outDisplayTransform = dt;
			outColorSpace       = cs;
		}

		AgentRenderResult AgentSession::RenderCore_( const AgentRenderParams& params,
		                                              bool assumeParked,
		                                              std::uint64_t forcedJobId )
		{
			AgentRenderResult res;

			if( !mJob ) {
				res.ok = false;
				res.message = "no head loaded";
				return res;
			}

			// Preserve the last successful frame until this render and its PNG
			// encode succeed, but make its compact perception sidecar plus every
			// superseded read-leased sidecar visible to the replacement sink's peak
			// accounting. The cache lock is intentionally narrow; references keep
			// the snapshotted sinks alive across the render.
			std::uint64_t cachedPerceptionBytesAtStart = 0;
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				cachedPerceptionBytesAtStart = RetainedPerceptionBytesLocked_();
			}

			// A render MUST NOT mutate the retained Document.  The earlier
			// slice-0b draft honoured `samplesOverride` by routing it through
			// ProposePatch -> ApplyCstParamEdit, which permanently rewrote the
			// `samples` param in the retained CST (a subsequent ReadDocument
			// reflected it forever) -- a render silently editing the scene,
			// contradicting the design's non-mutating override model
			// (docs/agentic-redesign/50-agentic-surface.md §2.2.5's
			// ResolveEffectiveRenderConfig layers render overrides onto the
			// DerivedScene WITHOUT touching the scene).
			//
			// Model-B F2 slice S3 (EffectiveRenderConfig) CLOSES this gap:
			// IRasterizer::SetSampleCountOverride / GetSampleCountOverride
			// (implemented on the pixel-based rasterizer family --
			// PixelBasedRasterizerHelper -- covering PT, spectral PT, BDPT,
			// VCM) give the transient, non-mutating setter this comment used
			// to say did not exist.  `params.samples` (>= 1) is now
			// CAPTURED (GetSampleCountOverride, before any mutation) ->
			// APPLIED (SetSampleCountOverride) -> render -> RESTORED to the
			// captured value, via SampleCountRestoreGuard (the SAME
			// Arm/Disarm/restore-on-every-exit house pattern as
			// RenderOverrideRestoreGuard / ProgressRestoreGuard below, so a
			// throw out of Rasterize() -- OIDN is a documented real throw
			// site -- still restores).  A rasterizer that has NOT opted in
			// (MLT, photon-map-only integrators, AutoRasterizer's outer
			// wrapper) reports false/-1 and the override is honestly NOT
			// applied -- res.samplesOverridden stays false and `message`
			// notes it.  ReadDocument() remains byte-identical across a
			// Render call REGARDLESS of `params.samples` (this is a LIVE
			// rasterizer-state mutation, exactly like the film-dims /
			// camera-pose overrides below -- never a CST edit).
			const bool wantSamplesOverride = ( params.samples >= 1 );

			// Round-2 P2-A fix: compute isDraft/res.renderMode FIRST, before
			// fetching/gating the live rasterizer below -- doDraftRenderWork
			// (see its own doc) never dereferences the PRODUCTION rasterizer,
			// its FrameStore, or mJob->RemoveRasterizerOutputs() at all, so a
			// scene with NO active rasterizer chunk must still be able to
			// run a draft render.  Pre-fix, the `!rast` bail-out below ran
			// UNCONDITIONALLY before this was even computed, so a
			// rasterizer-less head could never reach quality:"draft" though
			// the draft path never needed `rast` in the first place.
			// `res.integrator` is DELIBERATELY left as the head's active
			// (production) rasterizer's name in EITHER mode -- see
			// AgentRenderResult::renderMode's doc for why the two fields
			// answer different questions.
			const bool isDraft = ( params.quality == AgentRenderQuality::Draft );
			// Toolkit slice 3a: objectmap is a THIRD, orthogonal render
			// target (a flat per-object identity segmentation).  It routes
			// through its OWN ephemeral pipeline (like draft, never the
			// production rasterizer) and has exactly one fidelity -- so it
			// takes priority over the beauty renderMode string, and `quality`
			// / `samples` are honestly ignored under it (noted below).
			const bool isObjectMap = ( params.renderTarget == AgentRenderTarget::ObjectMap );
			// GUI render modes P1 (docs/gui/RENDER_MODES.md §8): the fourth,
			// orthogonal render target -- one of the ShaderPipeline data modes
			// (Normals/Depth/Facets/Wireframe).  Structurally a sibling of
			// isObjectMap: its own ephemeral pipeline, one fidelity, quality/
			// samples ignored.  `viewModeInfo` resolves the registry entry ONCE
			// so both the renderMode string below and doViewModeRenderWork's
			// factory call use the SAME lookup (a null result -- an out-of-
			// range enum value reaching here, which the RPC layer already
			// refuses -- degrades to the render-failed tail via
			// CreateInteractiveViewModePipeline's own false return).
			const bool isViewMode = ( params.renderTarget == AgentRenderTarget::ViewMode );
			const Implementation::ViewportRenderModeInfo* viewModeInfo =
				isViewMode ? Implementation::FindViewportRenderModeInfo( params.viewMode ) : nullptr;
			// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): a
			// BeautyVariant target (deep_reflect/direct) is a REAL
			// production-class PT pipeline, structurally a sibling of
			// isViewMode's ShaderPipeline data modes but routed to
			// doBeautyVariantRenderWork instead of doViewModeRenderWork --
			// see that lambda's doc below.
			const bool isBeautyVariant =
				isViewMode && viewModeInfo && Implementation::IsBeautyVariantMode( viewModeInfo->mode );
			res.renderMode = isObjectMap ? "objectmap"
				: isViewMode ? ( viewModeInfo ? viewModeInfo->name : "" )
				: ( isDraft ? "draft" : "production" );

			// Round-3 additive wire field: report the ACTIVE rasterizer's
			// registered type name (= its scene-file chunk keyword, e.g.
			// "bdpt_pel_rasterizer") so the agent can observe which
			// integrator a rasterizer insert_chunk activated.  Job::
			// GetActiveRasterizerName() is a plain member-string accessor
			// that defaults to "" (Job.h) -- null-safe with no active
			// rasterizer at all.  `res.integrator` is DELIBERATELY left as
			// the head's active (production) rasterizer's name in EITHER
			// mode -- see AgentRenderResult::renderMode's doc for why the
			// two fields answer different questions.  Filled on BOTH the
			// success and the render-failure paths -- the active integrator
			// is a property of the head, not of whether this particular
			// render produced an image.
			//
			// Re-review P1 FIX: this resolution (`res.integrator =
			// mJob->GetActiveRasterizerName();` + `IRasterizer* rast =
			// mJob->GetRasterizer();` + the "!rast" bail-out), and the
			// `view`/camera-override resolution that used to sit right after
			// it, USED to run HERE -- on the CALLING thread, before the
			// render is ever parked.  The actual render body runs LATER,
			// inside the parked closure (RunPreviewRenderParked /
			// SubmitAgentRenderSync / the direct headless call below),
			// possibly after an arbitrary delay (the controller's fairness
			// queue).  A concurrent GUI edit landing in that window could
			// replace the Job's rasterizer/camera managers out from under a
			// pointer cached this early -- a use-after-free class of bug,
			// not merely a stale value.  ALL live-Job-state resolution now
			// happens INSIDE `doRenderWork` (see its top) instead, which only
			// ever executes already-parked (or, headless with no controller,
			// with no concurrent GUI thread to race at all): resolved inside
			// the park; never snapshot live Job state on the calling thread.
			// `isDraft`/`isObjectMap`/`isViewMode`/`viewModeInfo`/
			// `isBeautyVariant`/`res.renderMode` above are all derived PURELY
			// from `params`, never from live Job state, so they stay safe to
			// compute here, unchanged.

			const bool wantFilmOverride =
				( params.width > 0 && params.height > 0 );

			// GUI render modes P2a `render{view:}` surface (docs/gui/
			// RENDER_MODES.md section 8, deferred from P1): resolve an optional
			// NAMED VIEW into the SAME ephemeral camera-override fields
			// `params.camera` uses, so it composes for free with EVERY render
			// target (Beauty/ObjectMap/ViewMode, draft or production) via the
			// EXISTING applyCameraOverride machinery below -- no parallel
			// override mechanism.  `view` wins over an explicit `camera`
			// override when both are supplied (`effectiveCamera` starts as a
			// COPY of params.camera, then a resolved view's pose overwrites it
			// wholesale).
			//
			// Re-review P1 FIX: resolving WHICH named view/camera this is
			// (mController->FindNamedViewPose / mJob->GetCameras()) needs live
			// Job/controller state, so -- like `rast` above -- that resolution
			// now happens fresh INSIDE doRenderWork (see its top), not here.
			// `effectiveCamera` and `wantCameraOverride` are declared here
			// (mutable) purely so applyCameraOverride (defined below,
			// capturing both by reference) can read whatever doRenderWork
			// fills in later; neither is assigned a live-state-derived value
			// at this point.
			AgentCameraOverride effectiveCamera = params.camera;
			bool wantCameraOverride = false;

			// Re-review P1: doRenderWork sets exactly one of these two flags
			// (never both) instead of returning `res` directly when the
			// `view` resolution fails -- see doRenderWork's top for the two
			// failure cases (unknown view name; a resolved but non-pinhole
			// NAMED VIEW) and the dispatch tail below (`if(
			// viewResolutionFailed )`) for how the flag short-circuits the
			// generic renderRan/rendered fallback message.
			// Set by ANY early bail inside doRenderWork that has already written a
// specific res.message (an unresolvable/non-pinhole `view`, OR the
// production "no active rasterizer" bail).  Since that resolution moved
// INSIDE the park, those bails can no longer `return res;` directly, so
// without this flag the shared tail's generic renderRan/rendered fallback
// overwrites the specific reason with "render failed".
			bool specificFailureReported = false;
			// Re-review P2 fix: set when a resolved PINHOLE named view's FOV
			// had to be dropped because the ACTIVE camera cannot store one
			// (anything but PinholeCamera) -- see doRenderWork's
			// active-camera preflight.  Surfaced as an honest note on the
			// result message below (res.cameraOverridden site), never a
			// render failure.
			bool viewFovSkippedActiveNonPinhole = false;

			// ROUTING-ONLY signal (P1 fix): choosing which controller entry
			// point parks this render (RunPreviewRenderParked, when a
			// film/camera override is in play, vs. the plain
			// SubmitAgentRenderSync otherwise) must happen BEFORE doRenderWork
			// ever runs -- but the REAL wantCameraOverride (above) is now
			// resolved live, INSIDE doRenderWork, from state we cannot yet see
			// out here.  A `view` request is therefore treated CONSERVATIVELY
			// as "assume a camera override" for routing purposes only: a
			// resolved pinhole view always sets at least location/lookat/up
			// (see CameraSnapshotToOverride), so this is never a false
			// negative; it can only ever route an eventually-failing `view`
			// request through the override-aware entry point, which is
			// harmless -- the actual failure message doRenderWork produces is
			// unaffected by which entry point ran it.
			const bool wantCameraOverrideForRouting =
				!params.view.empty() ||
				( effectiveCamera.hasLocation || effectiveCamera.hasLookAt ||
					effectiveCamera.hasUp || effectiveCamera.hasOrientation ||
					effectiveCamera.hasTargetOrientation || effectiveCamera.hasFov );

			// LIVE-mode safety (see AgentSession.h Render(AgentRenderParams)
			// doc + CLAUDE.md investigation note): DoOneRenderPass swaps the
			// SAME shared Film dims / camera frame in place, unsynchronized
			// against anything outside SceneEditController.  When a controller
			// is attached, run the WHOLE capture-override-render-restore
			// sequence for BOTH overrides inside ONE
			// RunPreviewRenderParked callback so it cannot race that swap.
			// Headless mode (no controller) has no interactive thread to
			// race, so the lambda runs directly.
			bool overrodeFilm = false;
			bool overrodeCamera = false;
			unsigned int origFilmW = 0, origFilmH = 0;
			double origFilmPAR = 1.0;
			std::vector<CapturedCameraField> capturedCam;
			ICamera* activeCam = nullptr;
			// Model-B F2 slice S3: sample-count override state.
			// `overrodeSamples` is set true only once SetSampleCountOverride
			// actually returns true for THIS render, so a caller reading
			// res.samplesOverridden gets an honest answer even when
			// `wantSamplesOverride` was requested against an unsupported
			// rasterizer.
			//
			// Re-review P1 fix: `origSamples` (GetSampleCountOverride,
			// captured BEFORE any mutation) USED to be captured HERE, on the
			// calling thread, from the (now-removed) outer-scope `rast` --
			// but it is consumed ONLY inside doRenderWork's production body,
			// so it is now declared fresh there instead, right where `rast`
			// is guaranteed freshly-resolved and non-null.
			bool overrodeSamples = false;
			// P1 fix: the TAIL (after doRenderWork returns, possibly after
			// the park has released) needs the effective sample count for
			// the result message -- captured under the park, inside
			// doRenderWork's production body, instead of re-dereferencing
			// `rast` from the tail (see the `readBack` site below).
			int productionSampleReadBack = -1;
			// Toolkit slice 2 (quality:"draft"): the draft path's OWN
			// sample-cap outcome, tracked separately from
			// overrodeSamples/origSamples above -- those describe the
			// PRODUCTION rasterizer `rast`, which a draft render never
			// touches at all.  `draftEffectiveSamples` is 0 until
			// doDraftRenderWork actually runs; kDraftMaxSamples is the
			// hard cap (see AgentRenderParams::quality's doc).
			static constexpr int kDraftMaxSamples = 4;
			bool draftSamplesApplied   = false;
			bool draftSamplesCapped    = false;
			int  draftEffectiveSamples = 0;
			// Toolkit slice 3a (objectmap): the per-render identity palette +
			// registry + atomic pixel tally, built INSIDE doObjectMapRenderWork
			// (on the render thread, before RasterizeScene) and read AFTER the
			// render to assemble res.legend.  Lives in this scope so it
			// outlives the render lambda.  Empty/unused in every beauty render.
			Implementation::ObjectMapPalette objectMapPalette;
			bool renderRan = false;
			bool rendered = false;
			// Fix-round-1 P2-C: true iff CancelAgentRender_ / Stop() tripped
			// the controller's cancel signal DURING this specific render --
			// checked right after Rasterize() returns, before the flag could
			// be cleared by anything else (nothing else touches
			// mCancelProgress while this worker holds mMutex -- see the
			// worker loop's own Reset() site).  Job::Rasterize() has no
			// cancelled/not-cancelled return value of its own (it always
			// returns true once RasterizeScene is invoked, whether or not a
			// block dispatcher aborted early) and the sink may hold a
			// PARTIAL image at that point -- so this flag is the ONLY
			// reliable signal that the render was actually cut short.
			bool wasCancelled = false;
			InMemoryRasterizerOutput* sink = nullptr;

			// Unwind guard for OUR owning ref on `sink` (refcount 1 from
			// `new` below): if mJob->Rasterize() throws (OIDN is a real
			// throw site), the normal release sites are skipped and the
			// owning ref would leak.  safe_release() nulls the pointer, so
			// every normal path (failure release, tail release, ownership
			// transfer to mLastSink -- which nulls `sink` explicitly) leaves
			// this a no-op.  Declared AFTER `sink` so it destructs FIRST.
			struct SinkUnwindGuard {
				InMemoryRasterizerOutput*& p;
				~SinkUnwindGuard() { safe_release( p ); }
			} sinkUnwindGuard{ sink };

			// AddRasterizerOutput owns a second reference. Retain the exact
			// rasterizer/output pair before attaching so every exit can undo only
			// that attachment even if a controller edit replaces the Job's
			// active rasterizer after the render park ends.  Looking the rasterizer
			// up through Job during unwind is a race; freeing every output can also
			// erase a viewport sink installed by another owner in that window. On
			// success mLastSink owns the observation, so keeping it attached would
			// strand a full image/sidecar on every inactive rasterizer after an
			// integrator switch and invalidate the bounded-memory contract.
			struct JobSinkAttachmentUnwindGuard {
				IRasterizer* rasterizer = nullptr;
				Implementation::Rasterizer* concreteRasterizer = nullptr;
				IRasterizerOutput* output = nullptr;
				bool armed = false;

				void Arm( IRasterizer* r, IRasterizerOutput* o ) {
					if( r ) r->addref();
					rasterizer = r;
					concreteRasterizer = dynamic_cast<Implementation::Rasterizer*>( r );
					output = o;
					armed = rasterizer && output;
				}
				void Detach() noexcept {
					if( armed && rasterizer ) {
						try {
							if( concreteRasterizer ) {
								concreteRasterizer->RemoveRasterizerOutput( output );
							} else {
								// No out-of-tree implementation currently reaches this
								// path.  Retaining the exact rasterizer still avoids the
								// mutable-Job lookup race for a future implementation.
								rasterizer->FreeRasterizerOutputs();
							}
						}
						catch( ... ) {
							GlobalLog()->PrintEx( eLog_Error,
								"AgentSession::Render: exception while detaching in-memory output" );
						}
					}
					safe_release( rasterizer );
					concreteRasterizer = nullptr;
					output = nullptr;
					armed = false;
				}
				~JobSinkAttachmentUnwindGuard() noexcept { Detach(); }
			} sinkAttachmentUnwindGuard;

			// T4 Last Render: this helper is called only from INSIDE
			// doRenderWork, while every controller-attached path owns the
			// coordinated render slot.  Capture the final sink image before
			// that park is released; the result/cache tail below may execute
			// later on the submitting thread and must not touch live pane
			// state there.
			auto publishCompletedToLastRender = [&]()
			{
				if( !mController || !renderRan || !rendered || wasCancelled
				 || !sink || !sink->HasImage() )
					return;
				IRasterImage* image = nullptr;
				if( sink->CopyToRasterImage( &image ) && image )
				{
					mController->AdoptAgentRenderImageParked( image );
					if( image ) image->release();
				}
			};

			// P1-B (belt-and-braces): if ANY requested camera field fails to
			// apply, fail loud -- restore what was already applied THIS call
			// and skip the render entirely rather than reporting
			// cameraOverridden==true on a partial/no-op override.  The RPC
			// layer (AgentRpc.cpp ParseCameraOverrideParam) is the primary
			// gate (validates vector SHAPE before this call is ever reached),
			// but a direct C++ caller (bypassing the RPC layer) must get the
			// same honesty guarantee.
			bool cameraOverrideFailed = false;
			std::string cameraOverrideFailedField;

			// Toolkit slice 2 (quality:"draft") shared helpers ------------------
			//
			// Film-dims / camera-pose override capture+apply is IDENTICAL in
			// EITHER render mode -- it mutates the Job's Film / the active
			// camera's properties, never the rasterizer -- so it is factored
			// out here (verbatim, unchanged logic) and called from BOTH
			// doRenderWork's production branch (at the SAME relative position
			// the inline code used to occupy) and doDraftRenderWork below.
			// Neither helper touches a restore guard itself -- each branch
			// constructs and arms its OWN RenderOverrideRestoreGuard (the
			// production branch's construction order relative to fsGuard is
			// LOAD-BEARING, see fsGuard's doc below; the draft branch has no
			// fsGuard at all, so its instance has no such constraint) -- these
			// lambdas only do the capture/apply WORK.
			auto applyFilmOverride = [&]()
			{
				const IScenePriv* scenePriv = mJob->GetScene();
				const IFilm* curFilm = scenePriv ? scenePriv->GetFilm() : nullptr;
				if( curFilm ) {
					origFilmW   = curFilm->GetWidth();
					origFilmH   = curFilm->GetHeight();
					origFilmPAR = curFilm->GetPixelAR();
				}
				if( wantFilmOverride && curFilm ) {
					if( mJob->SetFilm( params.width, params.height, origFilmPAR ) ) {
						overrodeFilm = true;
					}
				}
			};

			auto applyCameraOverride = [&]()
			{
				if( !wantCameraOverride ) return;
				ICameraManager* cams = mJob->GetCameras();
				const std::string activeName = mJob->GetActiveCameraName();
				activeCam = ( cams && !activeName.empty() )
					? cams->GetItem( activeName.c_str() ) : nullptr;
				if( !activeCam ) return;
				// captureAndSet returns true iff the field was requested AND
				// applied cleanly; false on a rejected SetProperty (the field
				// is captured but NOT pushed onto capturedCam in that case,
				// since nothing was actually changed -- there is nothing to
				// restore for it, and restoring it would be a harmless but
				// misleading no-op given it was never applied).
				auto captureAndSet = [&]( bool has, const char* name, const std::string& newVal ) -> bool
				{
					if( !has ) return true;   // not requested -- vacuously fine
					const String priorValue = CameraIntrospection::GetPropertyValue( *activeCam, String( name ) );
					const bool applied = CameraIntrospection::SetProperty( *activeCam, String( name ), String( newVal.c_str() ) );
					if( !applied ) {
						cameraOverrideFailed = true;
						cameraOverrideFailedField = name;
						return false;
					}
					CapturedCameraField f;
					f.name  = name;
					f.value = priorValue.c_str();
					capturedCam.push_back( f );
					return true;
				};
				// Apply in the SAME order as before; stop at the first
				// failure (fail-loud -- no point applying further fields
				// once one has already failed) but keep going through
				// captureAndSet's own bookkeeping so every field ALREADY
				// applied before the failure is captured and will be
				// restored by the guard.
				// `effectiveCamera` (params.camera, or the resolved `view`
				// override -- see its declaration above) is the single
				// source every camera-override consumer reads from.
				captureAndSet( effectiveCamera.hasLocation, "location", effectiveCamera.location )
					&& captureAndSet( effectiveCamera.hasLookAt, "lookat", effectiveCamera.lookAt )
					&& captureAndSet( effectiveCamera.hasUp,     "up",     effectiveCamera.up )
					&& captureAndSet( effectiveCamera.hasOrientation, "orientation", effectiveCamera.orientation )
					&& captureAndSet( effectiveCamera.hasTargetOrientation, "target_orientation", effectiveCamera.targetOrientation )
					&& captureAndSet( effectiveCamera.hasFov,    "fov",    effectiveCamera.fov );
				overrodeCamera = !capturedCam.empty();
			};

			// The ORDINARY-path restore (camera fields, then film dims) --
			// the guard exists for the ABNORMAL (exception) path; each branch
			// calls this explicitly then Disarm()s its OWN restoreGuard so
			// the destructor's restore is a no-op on the ordinary path
			// (avoids a harmless but redundant double SetProperty/SetFilm
			// call).
			auto restoreFilmAndCameraOverridesOrdinary = [&]()
			{
				for( std::size_t i = capturedCam.size(); i-- > 0; ) {
					CameraIntrospection::SetProperty( *activeCam,
						String( capturedCam[i].name.c_str() ),
						String( capturedCam[i].value.c_str() ) );
				}
				if( overrodeFilm ) {
					mJob->SetFilm( origFilmW, origFilmH, origFilmPAR );
				}
			};

			// The effective BEAUTY display transform (exposure + tone curve)
			// the in-memory PNG encode must apply so this render mirrors the
			// CLI file-output / viewport pipeline instead of emitting a raw
			// linear->sRGB image (see ResolveBeautyDisplayTransform_ +
			// InMemoryRasterizerOutput::SetDisplayTransform).  Resolved ONCE
			// here and installed on the beauty sinks (production + draft)
			// below; the OBJECTMAP sink deliberately leaves it at identity so
			// its per-pixel identity bytes pass through un-tonemapped.
			double beautyExposureEV       = 0.0;
			int    beautyDisplayTransform = 2 /*eDisplayTransform_ACES*/;
			// External review P2 fix: the resolved OUTPUT COLOUR SPACE, installed
			// on the beauty sinks alongside the display transform below (see
			// InMemoryRasterizerOutput::SetOutputColorSpace) -- default matches
			// today's pre-fix hardcoded behaviour (sRGB) when there's no LDR
			// file_rasterizeroutput to read one from, or under objectmap (which,
			// like the display transform, must stay untouched -- the identity
			// sink's per-pixel bytes are not colour-managed at all).
			int    beautyColorSpace       = eColorSpace_sRGB;
			// Toolkit slice 2 (quality:"draft"): the EPHEMERAL preview-
			// pipeline render body.  Never renders through `rast` (the production
			// rasterizer), its FrameStore, or mJob->RemoveRasterizerOutputs()
			// -- a fresh, throwaway InteractivePelRasterizer pipeline (studio-
			// preview shading only -- see CreateInteractiveMaterialPreviewPipeline
			// and AgentRenderQuality's doc) is constructed, used for exactly
			// this one render, and released before this lambda returns.  Film
			// overrides still call Job::SetFilm, which indirectly pushes a new
			// FrameStore to `rast`; the common dispatcher below isolates that
			// production-store side effect around every ephemeral branch.
			auto doDraftRenderWork = [&]()
			{
				IRasterizer* ephemeralRast = nullptr;
				IRayCaster*  previewCaster = nullptr;
				IRayCaster*  polishCaster  = nullptr;
				if( !Implementation::CreateInteractiveMaterialPreviewPipeline(
						&ephemeralRast, &previewCaster, &polishCaster ) )
				{
					return;   // rendered/renderRan stay false -- the shared tail reports "render failed"
				}
				// RAII release of the three factory refs (each an owning
				// reference the caller must release exactly once -- the SAME
				// contract RISEViewportBridge.mm's tryBuildLivePreviewForJob
				// / releaseLivePreview follow) -- runs on every exit,
				// including an exception unwinding out of RasterizeScene()
				// below (OIDN is a documented real throw site for the
				// production path; the interactive preview pipeline never
				// denoises, but this stays exception-safe on general
				// principle).  The common dispatcher's FrameStore guard restores
				// the one production-side effect of Job::SetFilm.
				//
				// Round-2 P3 (documented limitation, deliberate): there is
				// NO test coverage proving these three owned pointers are
				// actually released on every exit path (vs. e.g. a future
				// edit that reorders `pipelineGuard`'s construction past a
				// throwing call, silently reintroducing a leak).  A leak of
				// three small ray-caster/rasterizer objects per draft render
				// is RSS-undetectable at this size against normal process
				// noise, so a black-box "did memory grow" test would not
				// reliably catch a regression here -- this comment is the
				// tracked acknowledgment of that gap rather than a claim
				// it's covered.
				struct EphemeralPipelineGuard
				{
					IRasterizer*& r; IRayCaster*& p; IRayCaster*& q;
					~EphemeralPipelineGuard() { safe_release( r ); safe_release( p ); safe_release( q ); }
				} pipelineGuard{ ephemeralRast, previewCaster, polishCaster };

				// Film-dims / camera-pose overrides are SHARED with the
				// production path (see the helpers above) -- they mutate
				// Job/Scene state the ephemeral pipeline's RasterizeScene
				// call reads through `*scenePriv` below, so they compose for
				// free.  This inner guard is deliberately constructed AFTER
				// the common dispatcher's outer FrameStore guard.  On unwind,
				// film dimensions restore first and FrameStore identity wins last.
				RenderOverrideRestoreGuard restoreGuard( *mJob,
					overrodeFilm, origFilmW, origFilmH, origFilmPAR,
					activeCam, capturedCam );
				restoreGuard.Arm();

				// Attach the agent's sink DIRECTLY to the ephemeral instance
				// -- deliberately do NOT call mJob->RemoveRasterizerOutputs()
				// (that would perturb the PRODUCTION rasterizer's outs for
				// no reason; this fresh instance starts with an empty outs
				// list of its own).
				sink = new InMemoryRasterizerOutput();
				// Beauty preview: encode through the scene's effective display
				// transform (see the resolve above) so a draft render mirrors
				// the file/viewport look, not a raw linear->sRGB image.
				sink->SetDisplayTransform( beautyExposureEV, beautyDisplayTransform );
				sink->SetOutputColorSpace( beautyColorSpace );   // External review P2 fix: honour the scene's declared output colour space instead of a hardcoded sRGB
				ephemeralRast->AddRasterizerOutput( sink );

				applyFilmOverride();
				applyCameraOverride();

				if( cameraOverrideFailed ) {
					// FAIL LOUD, same contract as the production branch:
					// restoreGuard (still armed) restores whatever WAS
					// applied before the failure; skip the render entirely.
					return;
				}

				// Samples: CAP at kDraftMaxSamples regardless of what was
				// requested -- see AgentRenderParams::quality's doc for the
				// honesty contract this enforces (a draft render must stay
				// cheap even if a caller asks for a high sample count).
				// Absent an explicit request, the pipeline's own Config
				// default (1 SPP -- InteractivePelRasterizer::Config::
				// liveSamplesPerPass, already the state of a freshly
				// constructed instance with no sampling kernel installed) is
				// left untouched: no SetSampleCountOverride call at all in
				// that case.
				if( wantSamplesOverride ) {
					draftEffectiveSamples = ( params.samples > kDraftMaxSamples ) ? kDraftMaxSamples : params.samples;
					draftSamplesCapped    = ( params.samples > kDraftMaxSamples );
					draftSamplesApplied   = ephemeralRast->SetSampleCountOverride( draftEffectiveSamples );
					if( !draftSamplesApplied ) {
						draftEffectiveSamples = 0;   // honest: the request had no effect
					}
				} else {
					draftEffectiveSamples = 1;   // the pipeline's own uncustomized default
				}

				// Cancel wiring: mJob->SetProgress (the production path's
				// mechanism, driven through Job::Rasterize) never reaches an
				// object mJob does not own.  Install the SAME controller-
				// owned mCancelProgress the production path would
				// (AgentRenderProgress()) DIRECTLY on this ephemeral
				// instance instead, so CancelAgentRender_() / Stop() can
				// still abort an in-flight draft render.  No restore needed
				// afterward: this object (and whatever progress callback it
				// holds) is destroyed by `pipelineGuard` at the end of this
				// lambda.
				if( mController ) {
					ephemeralRast->SetProgressCallback( mController->AgentRenderProgress() );
				}

				// Test-only seam (shared with the production path) -- see
				// AgentSession.h's ForTest_SetThrowBeforeRasterize doc.
				if( mThrowBeforeRasterizeForTest ) {
					throw std::runtime_error(
						"AgentSession::ForTest_ThrowBeforeRasterize: test-only forced throw immediately before RasterizeScene() (draft path)" );
				}

				const IScenePriv* scenePriv = mJob->GetScene();
				if( scenePriv ) {
					// Dims and camera come from the Scene's Film / active
					// camera -- RasterizeScene reads both off `*scenePriv`
					// directly, so the film-dims (SetFilm) and camera-pose
					// overrides applied above compose for free; nothing
					// draft-specific to do here for either.
					IRasterizeSequence* pSeq = nullptr;
					if( mController ) {
						// Mirror Job::Rasterize's own fallback for "a
						// progress callback is installed" (Job.cpp's
						// RasterizeSequenceFromOptions, a private file-
						// static -- not reachable from here): a Morton
						// tile-32 sequence, the same default
						// RasterizeSequenceFromOptions produces absent a
						// non-default options-file override.
						RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
					}
					ephemeralRast->RasterizeScene( *scenePriv, 0, pSeq );
					safe_release( pSeq );

					if( mController ) {
						wasCancelled = mController->IsCancelRequested();
					}
					renderRan = true;
					rendered  = true;
				}

				restoreFilmAndCameraOverridesOrdinary();
				restoreGuard.Disarm();
			};

			// Toolkit slice 3a (objectmap): the EPHEMERAL identity render
			// body.  Structurally a sibling of doDraftRenderWork -- a fresh
			// throwaway pipeline, the agent's own sink, shared film/camera
			// overrides, controller-owned cancel wiring -- but its shader
			// emits each hit object's flat identity colour (from the palette
			// built here, on the render thread, before RasterizeScene) and it
			// NEVER installs a sampling kernel or a samples override (the
			// EXACTNESS INVARIANT: every pixel must take IntegratePixel's
			// single-ray branch so its identity byte is un-blended).  Never
			// renders through the production rasterizer, its FrameStore, or
			// mJob->RemoveRasterizerOutputs(); Job::SetFilm's indirect store
			// push is isolated by the common dispatcher below.
			auto doObjectMapRenderWork = [&]()
			{
				// Build the identity registry + palette FIRST (read-only over
				// the live ObjectManager; on this render thread, before any
				// RasterizeScene).  `objectMapPalette` lives in RenderCore_'s
				// scope so it outlives this lambda for the legend assembly.
				BuildObjectMapPalette( mJob->GetObjects(), objectMapPalette );

				IRasterizer* ephemeralRast = nullptr;
				IRayCaster*  objCaster     = nullptr;
				if( !Implementation::CreateInteractiveObjectMapPipeline(
						&ephemeralRast, &objCaster, objectMapPalette ) )
				{
					return;   // rendered/renderRan stay false -- shared tail reports "render failed"
				}
				// RAII release of the two factory refs (each an owning
				// reference the caller must release exactly once) -- runs on
				// every exit, including an exception unwinding out of
				// RasterizeScene() below.  No production state is touched.
				struct EphemeralPipelineGuard
				{
					IRasterizer*& r; IRayCaster*& p;
					~EphemeralPipelineGuard() { safe_release( r ); safe_release( p ); }
				} pipelineGuard{ ephemeralRast, objCaster };

				// Film-dims / camera-pose overrides compose exactly as in the
				// draft path (Job/Scene state the ephemeral pipeline reads
				// through *scenePriv).  No FrameStore-identity constraint here.
				RenderOverrideRestoreGuard restoreGuard( *mJob,
					overrodeFilm, origFilmW, origFilmH, origFilmPAR,
					activeCam, capturedCam );
				restoreGuard.Arm();

				sink = new InMemoryRasterizerOutput();
				ephemeralRast->AddRasterizerOutput( sink );

				applyFilmOverride();
				applyCameraOverride();

				if( cameraOverrideFailed ) {
					// FAIL LOUD, same contract as the other branches.
					return;
				}

				// EXACTNESS INVARIANT: deliberately NO SetSampleCountOverride
				// and NO sampling kernel -- a freshly constructed
				// InteractivePelRasterizer has pSampling == null, so every
				// pixel takes the single-ray (no jitter, no filter) branch,
				// the only path that yields an exact per-pixel identity byte.
				// `params.samples` is intentionally ignored (honestly noted in
				// the result message in RenderCore_'s tail).

				if( mController ) {
					ephemeralRast->SetProgressCallback( mController->AgentRenderProgress() );
				}

				// Test-only seam (shared contract with the other branches).
				if( mThrowBeforeRasterizeForTest ) {
					throw std::runtime_error(
						"AgentSession::ForTest_ThrowBeforeRasterize: test-only forced throw immediately before RasterizeScene() (objectmap path)" );
				}

				const IScenePriv* scenePriv = mJob->GetScene();
				if( scenePriv ) {
					IRasterizeSequence* pSeq = nullptr;
					if( mController ) {
						RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
					}
					ephemeralRast->RasterizeScene( *scenePriv, 0, pSeq );
					safe_release( pSeq );

					if( mController ) {
						wasCancelled = mController->IsCancelRequested();
					}
					renderRan = true;
					rendered  = true;
				}

				restoreFilmAndCameraOverridesOrdinary();
				restoreGuard.Disarm();
			};

			// GUI render modes P1 (docs/gui/RENDER_MODES.md §8): the EPHEMERAL
			// view-mode render body.  Structurally a sibling of
			// doObjectMapRenderWork -- a fresh throwaway pipeline, the agent's
			// own sink, shared film/camera overrides, controller-owned cancel
			// wiring -- but there is no identity palette/legend to build (a
			// view mode has no per-object registry) and it NEVER installs a
			// sampling kernel or a samples override, matching the objectmap
			// EXACTNESS INVARIANT: a diagnostic image is a single exact 1-spp
			// pass.  Never renders through the production rasterizer, its
			// FrameStore, or mJob->RemoveRasterizerOutputs(); Job::SetFilm's
			// indirect store push is isolated by the common dispatcher below.
			auto doViewModeRenderWork = [&]()
			{
				IRasterizer* ephemeralRast = nullptr;
				IRayCaster*  viewCaster    = nullptr;
				if( !viewModeInfo ||
					!Implementation::CreateInteractiveViewModePipeline(
						params.viewMode, &ephemeralRast, &viewCaster, params.xray ) )
				{
					return;   // rendered/renderRan stay false -- shared tail reports "render failed"
				}
				// RAII release of the two factory refs (each an owning reference
				// the caller must release exactly once) -- runs on every exit,
				// including an exception unwinding out of RasterizeScene() below.
				// No production state is touched.
				struct EphemeralPipelineGuard
				{
					IRasterizer*& r; IRayCaster*& p;
					~EphemeralPipelineGuard() { safe_release( r ); safe_release( p ); }
				} pipelineGuard{ ephemeralRast, viewCaster };

				// Film-dims / camera-pose overrides compose exactly as in the
				// other ephemeral branches.  No FrameStore-identity constraint here.
				RenderOverrideRestoreGuard restoreGuard( *mJob,
					overrodeFilm, origFilmW, origFilmH, origFilmPAR,
					activeCam, capturedCam );
				restoreGuard.Arm();

				sink = new InMemoryRasterizerOutput();
				ephemeralRast->AddRasterizerOutput( sink );

				applyFilmOverride();
				applyCameraOverride();

				if( cameraOverrideFailed ) {
					// FAIL LOUD, same contract as the other branches.
					return;
				}

				// EXACTNESS INVARIANT: deliberately NO SetSampleCountOverride and
				// NO sampling kernel -- CreateInteractiveViewModePipeline already
				// builds a freshly constructed InteractivePelRasterizer with
				// pSampling == null, so every pixel takes the single-ray (no
				// jitter, no filter) branch.  `params.samples`/`params.quality`
				// are intentionally ignored (honestly noted in the result message
				// in RenderCore_'s tail).

				if( mController ) {
					ephemeralRast->SetProgressCallback( mController->AgentRenderProgress() );
				}

				// Test-only seam (shared contract with the other branches).
				if( mThrowBeforeRasterizeForTest ) {
					throw std::runtime_error(
						"AgentSession::ForTest_ThrowBeforeRasterize: test-only forced throw immediately before RasterizeScene() (view-mode path)" );
				}

				const IScenePriv* scenePriv = mJob->GetScene();
				if( scenePriv ) {
					// Depth auto-windowing (docs/gui/RENDER_MODES.md "Depth
					// axis" self-calibration): InteractivePelRasterizer::
					// RasterizeScene now self-calibrates WITHIN one call --
					// if the pass it just ran recorded samples but the
					// window is still pending, it re-runs its base pass
					// once more before returning, so a single call here
					// already ships the calibrated image.  No warmup pass
					// needed on this side (a second RasterizeScene call
					// here would just be wasted work on top of the one the
					// rasterizer now performs internally).
					IRasterizeSequence* pSeq = nullptr;
					if( mController ) {
						RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
					}
					ephemeralRast->RasterizeScene( *scenePriv, 0, pSeq );
					safe_release( pSeq );

					if( mController ) {
						wasCancelled = mController->IsCancelRequested();
					}
					renderRan = true;
					rendered  = true;
				}

				restoreFilmAndCameraOverridesOrdinary();
				restoreGuard.Disarm();
			};

			// GUI render modes P2a (docs/gui/RENDER_MODES.md §6): the EPHEMERAL
			// BeautyVariant render body -- a sibling of doViewModeRenderWork in
			// SHAPE (fresh throwaway pipeline, the agent's own sink, shared
			// film/camera overrides, controller-owned cancel wiring, never
			// touches the production rasterizer) but a REAL production-class PT
			// render, not a diagnostic first-hit shader: real per-object
			// materials/lights, OIDN denoise, a fixed multi-sample count.  The
			// mode's fixed resolution divisor (variantScaleDivisor) is applied
			// to the EFFECTIVE requested dims (the caller's width/height
			// override if given, else the scene's current authored Film dims)
			// -- always applied, since a variant mode ALWAYS renders reduced.
			auto doBeautyVariantRenderWork = [&]()
			{
				if( !viewModeInfo ) {
					return;   // rendered/renderRan stay false -- shared tail reports "render failed"
				}

				// review-p3 P2-c: recover the PRODUCTION rasterizer's actual
				// configured default shader instead of letting
				// CreateBeautyVariantPipeline fall back to its own generic
				// internal DefaultPathTracing default -- a scene whose
				// `global` shader is a CUSTOM shaderop chain would otherwise
				// diverge from a CLI render on caster-dispatched SSS/BSSRDF
				// continuations.  Sound, not a guess: GetRasterizerParameter's
				// "shader" case reads back the exact resolved shader NAME
				// every Set*Rasterizer call stamped into its own registry
				// snapshot at construction time (Job.cpp).  Falls back to
				// null (CreateBeautyVariantPipeline's own real internal
				// default) when no rasterizer is active yet or the resolved
				// name doesn't exist in the shader manager.
				IShader* pProductionDefaultShader = nullptr;
				{
					const std::string activeRastName = mJob->GetActiveRasterizerName();
					if( !activeRastName.empty() ) {
						const std::string shaderName = mJob->GetRasterizerParameter(
							activeRastName.c_str(), "shader" );
						if( !shaderName.empty() && mJob->GetShaders() ) {
							pProductionDefaultShader = mJob->GetShaders()->GetItem( shaderName.c_str() );
						}
					}
				}

				IRasterizer* ephemeralRast  = nullptr;
				IRayCaster*  variantCaster  = nullptr;
				if( !Implementation::CreateBeautyVariantPipeline(
						params.viewMode, &ephemeralRast, &variantCaster, pProductionDefaultShader ) )
				{
					return;
				}
				struct EphemeralPipelineGuard
				{
					IRasterizer*& r; IRayCaster*& p;
					~EphemeralPipelineGuard() { safe_release( r ); safe_release( p ); }
				} pipelineGuard{ ephemeralRast, variantCaster };

				// GUI render modes P2b `render{light:}` surface: apply on
				// this EPHEMERAL variant caster -- no restore guard needed,
				// `pipelineGuard` above destroys the whole caster (and its
				// LightSampler) with it at scope exit.  An unresolved name
				// FAILS the render loudly, same contract as the production
				// branch (see ApplyLightSoloByName's doc).
				//
				// `SetSoloLightByName` resolves against `variantCaster`'s
				// OWN attached scene, but CreateBeautyVariantPipeline never
				// attaches one (that normally only happens inside the
				// terminal RasterizeScene call below) -- so an EARLY
				// AttachScene here is required before resolution can see
				// any lights at all.  Harmless to call again later:
				// RasterizeScene's own AttachScene(&scene) hits the same-
				// Scene-pointer fast path (RayCaster::AttachScene) since
				// nothing changes the scene's light-topology generation in
				// between, so the LightSampler this call just built (with
				// the solo already applied) is NOT rebuilt/dropped.
				if( !params.light.empty() ) {
					const IScenePriv* scenePrivForSolo = mJob->GetScene();
					if( scenePrivForSolo ) {
						variantCaster->AttachScene( scenePrivForSolo );
					}
					std::string lightSoloMessage;
					if( !ApplyLightSoloByName( variantCaster, params.light, lightSoloMessage ) ) {
						res.ok = false;
						res.message = lightSoloMessage;
						specificFailureReported = true;
						return;
					}
				}

				RenderOverrideRestoreGuard restoreGuard( *mJob,
					overrodeFilm, origFilmW, origFilmH, origFilmPAR,
					activeCam, capturedCam );
				restoreGuard.Arm();

				sink = new InMemoryRasterizerOutput();
				// A BeautyVariant pass genuinely shades + denoises -- apply the
				// SAME display transform as production/draft beauty so it isn't
				// a raw linear image (unlike the data-mode view sinks above,
				// which stay untonemapped by design).
				sink->SetDisplayTransform( beautyExposureEV, beautyDisplayTransform );
				sink->SetOutputColorSpace( beautyColorSpace );   // External review P2 fix: honour the scene's declared output colour space instead of a hardcoded sRGB
				ephemeralRast->AddRasterizerOutput( sink );

				// Film dims: the EFFECTIVE requested dims (an explicit
				// width/height override if the caller supplied one, else the
				// scene's current authored Film dims) divided by the mode's
				// fixed resolution divisor.  Deliberately does NOT reuse
				// applyFilmOverride() (which only resizes when wantFilmOverride
				// is true) -- a variant pass resizes UNCONDITIONALLY.
				const IScenePriv* scenePrivForDims = mJob->GetScene();
				const IFilm* curFilmForDims = scenePrivForDims ? scenePrivForDims->GetFilm() : nullptr;
				if( curFilmForDims ) {
					origFilmW   = curFilmForDims->GetWidth();
					origFilmH   = curFilmForDims->GetHeight();
					origFilmPAR = curFilmForDims->GetPixelAR();
				}
				const unsigned int effW = wantFilmOverride ? params.width  : origFilmW;
				const unsigned int effH = wantFilmOverride ? params.height : origFilmH;
				const unsigned int div  = viewModeInfo->variantScaleDivisor > 0
					? viewModeInfo->variantScaleDivisor : 1;
				const unsigned int scaledW = ( effW / div ) > 0 ? ( effW / div ) : 1;
				const unsigned int scaledH = ( effH / div ) > 0 ? ( effH / div ) : 1;
				if( curFilmForDims && mJob->SetFilm( scaledW, scaledH, origFilmPAR ) ) {
					overrodeFilm = true;
				}

				applyCameraOverride();

				if( cameraOverrideFailed ) {
					// FAIL LOUD, same contract as the other branches.
					return;
				}

				// `params.quality`/`params.samples`/`params.xray` are all
				// intentionally ignored -- the mode's spp/bounce-depth/OIDN
				// config is FIXED by the registry (CreateBeautyVariantPipeline);
				// honestly noted in the result message in RenderCore_'s tail.

				if( mController ) {
					ephemeralRast->SetProgressCallback( mController->AgentRenderProgress() );
				}

				// Test-only seam (shared contract with the other branches).
				if( mThrowBeforeRasterizeForTest ) {
					throw std::runtime_error(
						"AgentSession::ForTest_ThrowBeforeRasterize: test-only forced throw immediately before RasterizeScene() (beauty-variant path)" );
				}

				const IScenePriv* scenePriv = mJob->GetScene();
				if( scenePriv ) {
					IRasterizeSequence* pSeq = nullptr;
					if( mController ) {
						RISE_API_CreateMortonRasterizeSequence( &pSeq, 32 );
					}
					ephemeralRast->RasterizeScene( *scenePriv, 0, pSeq );
					safe_release( pSeq );

					if( mController ) {
						wasCancelled = mController->IsCancelRequested();
					}
					renderRan = true;
					rendered  = true;
				}

				restoreFilmAndCameraOverridesOrdinary();
				restoreGuard.Disarm();
			};

			auto doRenderWork = [&]()
			{
				// Re-review P1 fix: resolve ALL live Job/rasterizer/camera state
				// HERE, inside the parked closure -- never snapshot it on the
				// calling thread before park.  See the comment that used to sit
				// just ahead of this lambda (now short and pointing here) for the
				// full rationale.  `res.integrator` is set FIRST, unconditionally,
				// on every invocation of this closure (every render target,
				// success or failure) -- matches the pre-fix contract exactly,
				// just resolved fresh here instead of on the calling thread.
				res.integrator = mJob->GetActiveRasterizerName();

				if( !isObjectMap ) {
					ResolveBeautyDisplayTransform_( beautyExposureEV, beautyDisplayTransform, beautyColorSpace );
				}

				if( params.maxPixelCount > 0 ) {
					const IScenePriv* scenePrivForBudget = mJob->GetScene();
					const IFilm* filmForBudget = scenePrivForBudget ? scenePrivForBudget->GetFilm() : nullptr;
					const unsigned int budgetW = wantFilmOverride ? params.width
						: ( filmForBudget ? filmForBudget->GetWidth() : 0 );
					const unsigned int budgetH = wantFilmOverride ? params.height
						: ( filmForBudget ? filmForBudget->GetHeight() : 0 );
					const std::uint64_t pixels = static_cast<std::uint64_t>( budgetW ) * budgetH;
					if( budgetW == 0 || budgetH == 0 || pixels > params.maxPixelCount ) {
						res.ok = false;
						res.message = "render dimensions exceed the configured pixel budget";
						specificFailureReported = true;
						return;
					}
				}

				// Fetch the live rasterizer, fresh, under the park.  Same gating
				// condition and same message as the pre-fix calling-thread check:
				// the "!rast" bail-out applies ONLY to the production BEAUTY
				// branch -- draft/objectmap/view-mode all run their own ephemeral
				// pipelines and never dereference the production rasterizer.
				IRasterizer* rast = mJob->GetRasterizer();
				if( !isDraft && !isObjectMap && !isViewMode && !rast ) {
					res.ok = false;
					res.message = "no active rasterizer";
					// This bail moved INSIDE the park (P1: no snapshotting live
					// Job state on the calling thread), so it can no longer
					// `return res;` directly -- flag it so the shared tail
					// preserves this specific reason instead of overwriting it
					// with the generic "render failed".
					specificFailureReported = true;
					return;
				}

				if( !params.view.empty() )
				{
					bool resolved = false;
					CameraSnapshot pose;
					if( mController && mController->FindNamedViewPose( String( params.view.c_str() ), pose ) )
					{
						resolved = true;
					}
					else if( ICameraManager* cams = mJob->GetCameras() )
					{
						if( const ICamera* namedCam = cams->GetItem( params.view.c_str() ) )
						{
							resolved = CameraIntrospection::CaptureCameraSnapshot( *namedCam, pose );
						}
					}

					if( !resolved )
					{
						std::string available;
						if( mController )
						{
							const unsigned int n = mController->NamedViewCount();
							for( unsigned int i = 0; i < n; ++i )
							{
								char nameBuf[257];
								if( mController->NamedViewName( i, nameBuf, sizeof( nameBuf ) ) )
								{
									if( !available.empty() ) available += ", ";
									available += "\"";
									available += nameBuf;
									available += "\"";
								}
							}
						}
						if( ICameraManager* cams = mJob->GetCameras() )
						{
							struct NameCollector : public IEnumCallback<const char*>
							{
								std::string* out;
								bool operator()( const char* const& n ) override
								{
									if( !out->empty() ) *out += ", ";
									*out += "\"";
									*out += ( n ? n : "" );
									*out += "\"";
									return true;
								}
							} collector;
							collector.out = &available;
							cams->EnumerateItemNames( collector );
						}
						// res.renderMode / res.integrator were already set above
						// (before this resolution block runs) -- no need to
						// recompute them here.
						res.ok = false;
						res.message = "unknown view \"" + params.view + "\"";
						res.message += available.empty()
							? std::string( " -- no named views or scene cameras available" )
							: ( " -- available: " + available );
						specificFailureReported = true;
						return;
					}

					// A named view transfers every shared CameraCommon pose field
					// (location/lookat/up plus Euler and target orientation) and FOV.
					// It still cannot transfer a non-pinhole camera's own optics:
					// it only sets those shared fields on the ACTIVE camera.  There
					// is no plumbing here (or in
					// CameraIntrospection::SetProperty, which is descriptor-driven by
					// the camera's OWN chunk type and cannot re-type an ICamera in
					// place) to carry a ThinLens/Fisheye/Orthographic view's real
					// optics -- sensor/focal-length/fstop/focus-distance/aperture/
					// tilt-shift, fisheye scale, or ortho viewport scale -- onto
					// whatever type the active camera happens to be.  Silently
					// dropping those and rendering with the ACTIVE camera's own
					// optics under the requested view's pose would be a materially
					// wrong image reported as success.  Fail loudly instead of
					// guessing: name the unsupported camera type so the caller knows
					// exactly why, rather than getting a quietly-wrong PNG.
					if( pose.type != RISE::CameraSnapshot::Pinhole )
					{
						const char* typeName = "unknown";
						switch( pose.type )
						{
						case RISE::CameraSnapshot::ThinLens:     typeName = "thinlens";     break;
						case RISE::CameraSnapshot::Fisheye:      typeName = "fisheye";      break;
						case RISE::CameraSnapshot::Orthographic: typeName = "orthographic"; break;
						default: break;
						}
						res.ok = false;
						res.message = "view \"" + params.view + "\" is a " + typeName +
							" camera -- render{view:} can currently only transfer a pinhole "
							"view's shared pose+fov onto the active camera (AgentCameraOverride has "
							"no thinlens/fisheye/orthographic fields), so rendering it would "
							"silently use the active camera's own optics under the requested "
							"pose. Not supported yet: switch the active camera to \"" +
							params.view + "\" (if it names a real scene camera) and render "
							"without `view`, or request a pinhole named view instead.";
						specificFailureReported = true;
						return;
					}

					CameraSnapshotToOverride( pose, effectiveCamera );

					// Re-review P2 fix: a valid PINHOLE named view must not be
					// rejected just because the ACTIVE camera happens to be
					// thin-lens/fisheye/orthographic -- CameraIntrospection::SetProperty's
					// own "fov" branch (CameraIntrospection.cpp) rejects "fov" on
					// anything but a PinholeCamera, so applyCameraOverride below would
					// set cameraOverrideFailed=true and fail the WHOLE render just
					// because the resolved pose carries a FOV the active camera cannot
					// store.  That is the FALSE-REJECTION direction of the fix just
					// above (which correctly keeps failing loudly when the NAMED VIEW
					// itself is non-pinhole).  Chosen fix: (a) apply the pose, drop the
					// FOV, note the drop honestly -- NOT (b) render through a temporary
					// swapped-in pinhole camera.  (b) would need camera CREATE/
					// activate/teardown plumbing this file does not have:
					// applyCameraOverride only ever SetProperty's fields on the
					// ALREADY-active camera (captured via
					// mJob->GetCameras()->GetItem(activeName) below); there is no
					// existing "render through a substitute camera" path to reuse, and
					// building one (IJob::Add*Camera + SetActiveCamera + post-render
					// teardown/restore) would be new, load-bearing plumbing, not a
					// surgical fix.
					if( ICameraManager* camsForFov = mJob->GetCameras() )
					{
						const std::string activeNameForFov = mJob->GetActiveCameraName();
						const ICamera* activeCamForFov = !activeNameForFov.empty()
							? camsForFov->GetItem( activeNameForFov.c_str() ) : nullptr;
						CameraSnapshot activeSnapForFov;
						const bool activeSnapshotted = activeCamForFov
							&& CameraIntrospection::CaptureCameraSnapshot( *activeCamForFov, activeSnapForFov );
						if( !activeSnapshotted ) {
							res.ok = false;
							res.message = "view \"" + params.view + "\" cannot be transferred to the active camera because its pose cannot be snapshotted";
							specificFailureReported = true;
							return;
						}
						const bool activeAcceptsFov = activeSnapForFov.type == RISE::CameraSnapshot::Pinhole;
						if( !activeAcceptsFov && effectiveCamera.hasFov )
						{
							effectiveCamera.hasFov = false;
							viewFovSkippedActiveNonPinhole = true;
						}
					}
				}

				wantCameraOverride =
					( effectiveCamera.hasLocation || effectiveCamera.hasLookAt ||
						effectiveCamera.hasUp || effectiveCamera.hasOrientation ||
						effectiveCamera.hasTargetOrientation || effectiveCamera.hasFov );

				// Every ephemeral pipeline is independent of the production
				// rasterizer for actual rendering, but a shared film override calls
				// Job::SetFilm.  That method pushes its newly allocated canonical
				// FrameStore to every production rasterizer.  Capture and hold the
				// current display-store identity across the whole ephemeral call so
				// it remains alive and is rebound last, after the branch's inner
				// RenderOverrideRestoreGuard restores the original film dimensions.
				// This wrapper also covers early returns and exceptions uniformly.
				auto runEphemeralIsolated = [&]( auto&& renderWork )
				{
					Implementation::Rasterizer* productionRast =
						dynamic_cast<Implementation::Rasterizer*>( rast );
					Implementation::FrameStore* capturedStore =
						productionRast ? productionRast->GetFrameStore() : nullptr;
					if( capturedStore ) {
						capturedStore->addref();
					}
					FrameStoreIsolationGuard fsGuard( productionRast, capturedStore );
					fsGuard.Arm();

					renderWork();

					fsGuard.RestoreAndDisarm();
				};

				if( isObjectMap ) {
					runEphemeralIsolated( doObjectMapRenderWork );
					publishCompletedToLastRender();
					return;
				}
				if( isViewMode ) {
					if( isBeautyVariant ) {
						runEphemeralIsolated( doBeautyVariantRenderWork );
					} else {
						runEphemeralIsolated( doViewModeRenderWork );
					}
					publishCompletedToLastRender();
					return;
				}
				if( isDraft ) {
					runEphemeralIsolated( doDraftRenderWork );
					publishCompletedToLastRender();
					return;
				}

				// ---- PRODUCTION render body (unchanged apart from the
				// film/camera capture-apply-restore steps now factored into
				// the shared helpers above -- guard construction ORDER
				// relative to fsGuard is unchanged) ----
				//
				// Offscreen isolation: capture the DISPLAY FrameStore's
				// identity BEFORE any mutation, and addref our own copy of
				// the pointer (FrameStoreIsolationGuard's destructor -- or
				// the ordinary-path explicit tail restore below -- owns
				// that ref and drops it exactly once).  `concreteRast` is
				// null only if some future non-Implementation::Rasterizer
				// IRasterizer subclass is in play (none exist in tree
				// today); every check below degrades to a safe no-op in
				// that case.  Captured/armed BEFORE the private-store
				// install (and before the film-override's SetFilm, which
				// can ALSO push a fresh FrameStore -- see the override
				// branch below) so a throw between here and the tail
				// restore still restores the display FrameStore identity
				// via the destructor.
				//
				// P1-A FIX -- CONSTRUCTION ORDER IS LOAD-BEARING: `fsGuard`
				// is constructed FIRST, BEFORE `restoreGuard` just below, so
				// that on an exception unwinding out of this lambda the two
				// destructors run in the OPPOSITE (reverse-construction)
				// order: `restoreGuard` (film-dims restore) destructs
				// FIRST, THEN `fsGuard` (FrameStore-identity restore)
				// destructs LAST.  Getting this backwards is a real,
				// reproduced SIGABRT use-after-free: if `fsGuard` destructed
				// first, it would rebind the rasterizer to
				// `capturedDisplayStore` and drop ITS OWN ref to it; then
				// `restoreGuard`'s destructor calls
				// `mJob->SetFilm(origFilmW, origFilmH, origFilmPAR)`, which
				// (Job::EnsureJobFrameStore_locked) reallocates yet ANOTHER
				// FrameStore and rebinds the rasterizer to THAT one via
				// PushJobFrameStoreToRasterizers -- releasing the rasterizer's
				// own ref on `capturedDisplayStore` with no other ref left
				// to keep it alive, freeing the SAME object a GUI's
				// ViewportFrameStore is still observing (FrameStore::
				// RemoveObserver on the freed object is the UAF a live-lldb
				// session reproduced twice).  With `fsGuard` outliving
				// `restoreGuard`, its OWN addref on `capturedDisplayStore`
				// (taken right here, before either guard exists) keeps the
				// object alive THROUGH `restoreGuard`'s SetFilm-driven
				// reallocation, so the final `SetFrameStore(captured)` in
				// `fsGuard`'s destructor rebinds to a still-live object.
				Implementation::Rasterizer* concreteRast = dynamic_cast<Implementation::Rasterizer*>( rast );
				Implementation::FrameStore* capturedDisplayStore = concreteRast ? concreteRast->GetFrameStore() : nullptr;
				if( capturedDisplayStore ) {
					capturedDisplayStore->addref();
				}
				FrameStoreIsolationGuard fsGuard( concreteRast, capturedDisplayStore );
				fsGuard.Arm();

				// P1-A FIX (see `fsGuard` above for the full exception-
				// safety explanation): this guard is constructed SECOND, so
				// it is what runs on the ordinary "declare it done" path
				// first: this guard's destructor MUST run before
				// `fsGuard`'s does on any unwind, since the film-dims
				// restore below can reallocate a FrameStore that `fsGuard`
				// then needs to replace.  Do NOT reorder these two
				// constructions.  Armed BEFORE any override is applied so
				// restoration runs unconditionally on every exit from this
				// lambda -- including an exception unwinding out of
				// mJob->Rasterize() below (OIDN denoise is a documented real
				// throw site).  The explicit tail restore further down
				// Disarm()s the guard on the ordinary success path so the
				// destructor's restore is a no-op there (avoids a harmless
				// but redundant double SetProperty/SetFilm call).
				RenderOverrideRestoreGuard restoreGuard( *mJob,
					overrodeFilm, origFilmW, origFilmH, origFilmPAR,
					activeCam, capturedCam );
				restoreGuard.Arm();

				// Model-B F2 slice S3: sample-count override.  Armed BEFORE the
				// apply (same "arm before mutate" discipline as restoreGuard
				// above) so a throw between here and the explicit tail restore
				// still restores via the destructor.  Re-review P1 fix:
				// `origSamples` is captured HERE, fresh, from the `rast` this
				// closure already resolved above (guaranteed non-null by the
				// bail-out at the top of this closure) -- never from a pointer
				// cached on the calling thread before park.
				// (No FrameStore/film interaction, so its position relative
				// to fsGuard/restoreGuard's load-bearing order is free.)
				const int origSamples = rast->GetSampleCountOverride();
				SampleCountRestoreGuard sampleGuard( *rast, origSamples );
				sampleGuard.Arm();

				if( wantSamplesOverride ) {
					overrodeSamples = rast->SetSampleCountOverride( params.samples );
				}

				// GUI render modes P2b `render{light:}` surface (docs/gui/
				// RENDER_MODES.md §3 "light solo"): resolve + apply on the
				// PRODUCTION rasterizer's own RayCaster BEFORE the render
				// call.  Armed BEFORE the apply (same "arm before mutate"
				// discipline as sampleGuard above) so a throw between here
				// and the tail restore still clears it via the destructor.
				// An unresolved name FAILS the render loudly (res.ok=false),
				// mirroring `view`'s unresolvable-name contract -- see
				// ApplyLightSoloByName's doc.
				RISE::Implementation::PixelBasedRasterizerHelper* pHelperForSolo =
					dynamic_cast<RISE::Implementation::PixelBasedRasterizerHelper*>( rast );
				RISE::Implementation::RayCaster* pConcreteCasterForSolo = pHelperForSolo
					? dynamic_cast<RISE::Implementation::RayCaster*>( pHelperForSolo->GetRayCaster() )
					: nullptr;
				LightSoloRestoreGuard lightSoloGuard( pConcreteCasterForSolo );
				lightSoloGuard.Arm();

				if( !params.light.empty() && !RasterizerSupportsLightSolo( rast ) ) {
					// review-p2d P1-2: refuse rather than render every light
					// while reporting that only one is lit.  See
					// RasterizerSupportsLightSolo's doc for why BDPT is
					// actively wrong here and VCM/MLT a silent no-op.
					res.ok = false;
					res.message = "light \"" + params.light + "\" could not be applied -- light solo is "
						"implemented in the path-tracing integrator only.  BDPT, VCM, MLT and the auto-routed "
						"rasterizers would ignore it (BDPT would render a mixed estimator), so this render is "
						"refused rather than silently reporting a solo that did not happen.  Switch the scene "
						"to pathtracing_pel_rasterizer / pathtracing_spectral_rasterizer, or use a BeautyVariant "
						"view mode (which builds its own path-tracing pipeline and supports light solo).";
					specificFailureReported = true;
					return;
				}

				if( !params.light.empty() ) {
					// `SetSoloLightByName` resolves against the caster's
					// OWN attached scene, but a Job whose production
					// rasterizer has never rendered yet has no scene
					// attached to its caster (PixelBasedRasterizerHelper
					// only calls AttachScene from inside RasterizeScene
					// itself).  An early AttachScene here guarantees
					// resolution always sees the real light/object
					// managers; it is a same-Scene-pointer no-op (or a
					// harmless re-Prepare if something upstream already
					// bumped the light-topology generation) on every OTHER
					// call, since mJob->Rasterize() below attaches the
					// SAME Scene pointer again.
					if( pConcreteCasterForSolo ) {
						if( const IScenePriv* scenePrivForSolo = mJob->GetScene() ) {
							pConcreteCasterForSolo->AttachScene( scenePrivForSolo );
						}
					}
					std::string lightSoloMessage;
					if( !ApplyLightSoloByName( pConcreteCasterForSolo, params.light, lightSoloMessage ) ) {
						res.ok = false;
						res.message = lightSoloMessage;
						specificFailureReported = true;
						return;
					}
				}

				// ---- The render itself (in-memory sink; never touches the
				// filesystem).  MOVED ahead of the film-dims / camera-pose
				// override blocks (offscreen isolation): this guarantees
				// `outs` == [sink] BEFORE any SetFilm/SetFrameStore fires
				// below, so Rasterizer::ReannounceFrameStore's dispatch to
				// `outs` can only ever reach the agent's own throwaway sink
				// (a harmless no-op OnRasterizerFrameStoreChanged) and never
				// a production output that happened to still be attached.
				mJob->RemoveRasterizerOutputs();
				// `new` yields refcount 1 (our owning ref); AddRasterizerOutput
				// addrefs it. The attachment unwind guard removes this exact output
				// on every exit, including success: mLastSink becomes the sole owner
				// of a published observation, so inactive rasterizers cannot retain
				// one full image/sidecar each after integrator switches. We drop OUR
				// ref via safe_release(sink) after this
				// lambda returns -- no extra addref here.
				sink = new InMemoryRasterizerOutput();
				sink->SetConcurrentCachedPerceptionBytes( cachedPerceptionBytesAtStart );
				sink->SetPerceptionPrefilter( concreteRast
					? concreteRast->GetDenoisingPrefilter() : OidnPrefilter::Fast );
				// Beauty render: encode through the scene's effective display
				// transform (exposure + tone curve) so decode(rr.png) matches
				// the CLI file-output / viewport pipeline for the SAME head --
				// the divergence the image_reconstruct compareToImage grading
				// depends on being closed (see ResolveBeautyDisplayTransform_).
				sink->SetDisplayTransform( beautyExposureEV, beautyDisplayTransform );
				sink->SetOutputColorSpace( beautyColorSpace );   // External review P2 fix: honour the scene's declared output colour space instead of a hardcoded sRGB
				// This local is deliberately scoped to doRenderWork, which is the
				// closure executed while the controller's render park is held. Exact
				// detachment must happen before that closure returns: rasterizer
				// output iteration is intentionally unlocked during a render, so
				// deferring removal to RenderCore_'s outer PNG/cache tail would race
				// a newly resumed interactive render. The outer guard remains the
				// fallback if attachment setup itself throws.
				struct ParkedSinkDetachGuard {
					JobSinkAttachmentUnwindGuard& attachment;
					~ParkedSinkDetachGuard() noexcept { attachment.Detach(); }
				} parkedSinkDetachGuard{ sinkAttachmentUnwindGuard };
				sinkAttachmentUnwindGuard.Arm( rast, sink );
				rast->AddRasterizerOutput( sink );

				// ---- Film-dims override: capture -> set -> (render happens
				// after this lambda's camera section) -- restore happens at
				// the tail of this SAME lambda so both overrides are undone
				// before RunPreviewRenderParked unlocks (headless mode: same
				// ordering, just without the lock).
				//
				// Offscreen isolation: the (origFilmW, origFilmH, origFilmPAR)
				// capture below runs UNCONDITIONALLY (not just inside the
				// `wantFilmOverride` branch) -- the private-store install
				// right after this block needs the CURRENT film dims (as
				// its no-override sizing fallback) even when THIS render
				// does request an override, since `renderW`/`renderH` below
				// fall back to `origFilmW`/`origFilmH` whenever
				// `wantFilmOverride` is false.  (Toolkit slice 2: this is
				// `applyFilmOverride()`, the SAME shared helper doDraftRenderWork
				// calls -- factored out above so the logic is not duplicated.)
				applyFilmOverride();

				// Offscreen isolation: install a PRIVATE throwaway FrameStore,
				// UNCONDITIONALLY, sized to the EFFECTIVE render dims for
				// THIS call.  Placed AFTER the film-override block just
				// above so our explicit SetFrameStore here always wins over
				// whatever (if anything) Job::SetFilm's own
				// PushJobFrameStoreToRasterizers did.
				//
				// P1-1 FIX: this used to be gated on `!wantFilmOverride`,
				// relying on the override branch's `mJob->SetFilm(...)` to
				// supply a fresh (and therefore automatically DISTINCT-
				// pointer) FrameStore via its own PushJobFrameStoreToRasterizers
				// path.  That silently fails whenever the requested override
				// dims equal the CURRENT film dims -- the plausible "re-
				// render this scene's own resolution" input: Job::SetFilm
				// short-circuits on a same-dims request (Job.cpp's
				// "Same-dim short-circuit" block, `return true;` with NO
				// call to PushJobFrameStoreToRasterizers), and even on a
				// dims CHANGE, Rasterizer::SetFrameStore(sameStorePointer)
				// itself early-returns as a no-op if EnsureJobFrameStore
				// ever reused the existing store object.  Net effect pre-fix:
				// an agent override-render at the scene's current resolution
				// painted straight into the shared display FrameStore -- the
				// exact bug this whole mechanism exists to close.  Installing
				// our OWN fresh `new Implementation::FrameStore(spec)` here,
				// unconditionally, guarantees a genuinely distinct pointer on
				// EVERY render (override or not, same-dims or not), so
				// isolation no longer depends on SetFilm's internal store-
				// swap behavior at all.
				//
				// Gated on AcceptsFrameStorePush() -- true for every
				// Implementation::Rasterizer subclass in tree today,
				// including MLT (which opted back INTO the FrameStore push
				// in commit 36809dcf, "L6d-2b") -- so this only skips a
				// rasterizer that genuinely returns false / carries no
				// store (e.g. a direct-constructed test rasterizer with no
				// active camera, which has nothing to isolate in the first
				// place).  Also gated on a non-null CURRENT FrameStore so a
				// rasterizer that never had one isn't handed one for the
				// first time by an agent render.
				const unsigned int renderW = wantFilmOverride ? params.width  : origFilmW;
				const unsigned int renderH = wantFilmOverride ? params.height : origFilmH;
				if( concreteRast && concreteRast->AcceptsFrameStorePush()
				    && concreteRast->GetFrameStore() != nullptr )
				{
					Implementation::FrameStore::Spec spec;
					spec.width    = renderW;
					spec.height   = renderH;
					spec.tileEdge = 32;
					// Perception capture is isolated to this throwaway store: the
					// production/display FrameStore remains byte- and allocation-
					// identical.  Other agent render modes already ARE diagnostics;
					// only a production beauty render compiles this multi-AOV view.
					if( params.perception && !isDraft && !isObjectMap && !isViewMode ) {
						spec.aovChannels.push_back( FrameStoreOutput::ChannelId::Albedo );
						spec.aovChannels.push_back( FrameStoreOutput::ChannelId::Normal );
						spec.aovChannels.push_back( FrameStoreOutput::ChannelId::Depth );
					}
					Implementation::FrameStore* privateStore = new Implementation::FrameStore( spec );
					concreteRast->SetFrameStore( privateStore );
					safe_release( privateStore );   // the rasterizer's own addref (inside SetFrameStore) keeps it alive for the render
				}

				// ---- Camera-pose override: resolve the ACTIVE camera,
				// capture the current value of every REQUESTED field, then
				// apply the overrides.  P1-B: SetProperty's bool return is
				// now CHECKED -- a parse failure (malformed vector shape,
				// non-finite number, ...) must not silently no-op while
				// `overrodeCamera` reports true.  (Toolkit slice 2: this is
				// `applyCameraOverride()`, the SAME shared helper
				// doDraftRenderWork calls -- factored out above so the logic
				// is not duplicated.)
				applyCameraOverride();

				if( cameraOverrideFailed ) {
					// FAIL LOUD: do not render un-overridden and do not
					// report a partial override as applied.  The guard's
					// destructor (still armed) restores every field that
					// WAS applied before the failure; nothing further to do
					// here except skip the render.
					return;
				}

				// ---- The render itself: the in-memory sink was already
				// installed above (moved ahead of the film/camera override
				// blocks -- offscreen isolation); nothing left to set up but
				// the progress hook before Rasterize().
				//
				// Fix-round-1 P2-C: when a LIVE controller is attached, install
				// its mCancelProgress (via AgentRenderProgress()) as this Job's
				// progress callback BEFORE Rasterize() -- this is what makes
				// SceneEditController::CancelAgentRender_ / Stop() able to
				// actually ABORT an in-flight agent render instead of merely
				// flipping a flag nothing downstream ever consults.  Job::
				// Rasterize forwards pGlobalProgress to the rasterizer via
				// SetProgressCallback (see Job.cpp), and the block-fetch loop
				// (PixelBasedRasterizerHelper.cpp) polls IsCancelled()/Progress()
				// between blocks -- the SAME mechanism the interactive preview
				// rasterizer already relies on for cancel-and-park.  Restored to
				// the in-slot-captured prior afterward (see the capture below):
				// on a live-GUI Job that is the platform's persistent progress
				// callback; on a headless Job it is nullptr.  (The previous
				// "restored to null -- nothing else ever installs on this Job"
				// rationale was stale in the live-GUI configuration.)
				//
				// Round-2 P2-A: arm an RAII guard BEFORE the install so the
				// restore runs on EVERY exit -- including an exception
				// unwinding out of mJob->Rasterize() below (OIDN denoise is
				// a documented real throw site).  Pre-fix, that throw skipped
				// straight past the `mJob->SetProgress( nullptr )` call
				// below, leaving this controller's mCancelProgress installed
				// as the Job's progress hook forever -- a stale cancel hook
				// that could poison whatever the NEXT render (a different
				// controller-less caller, say) does with this Job's progress
				// state.  The ordinary-path restore further down Disarm()s
				// the guard so its destructor is a no-op there (avoids a
				// harmless but redundant double restore).
				// Slot-ownership hardening (2026-07-12): the restore value is no
				// longer a hardcoded nullptr -- capture whatever the slot honestly
				// holds HERE, inside the coordinator's cancel-and-park critical
				// section (every controller-attached shape of this render runs
				// in-slot -- see the S2a routing note below doRenderWork), so the
				// read can't race another slot writer.  On a live-GUI Job this is
				// the mac bridge's PERSISTENT BlockProgressCallback: the old
				// restore-to-null WIPED it from the slot after every agent render
				// (the GUI's progress hook silently went dead until the next
				// setProgressBlock: at the next render start).  Headless agent
				// renders read nullptr here, so their behaviour is unchanged.
				// Review round-2 P1: the capture and the install are ONE atomic
				// exchange, not a GetProgress() read followed by SetProgress().
				// Split in two, a platform adapter's conditional clear could
				// "succeed" (the slot still held its old callback) and delete
				// an object THIS path had already captured as its restore
				// value -- the exchange makes a successful adapter-side clear
				// genuinely mean "no render holds that pointer as prior".
				IProgressCallback* const priorProgress =
					mController ? mJob->ExchangeProgress( mController->AgentRenderProgress() ) : nullptr;
				ProgressRestoreGuard progressGuard( *mJob, priorProgress );
				if( mController ) {
					progressGuard.Arm();
				}

				// Test-only seam (P1-A regression lock): force a throw
				// immediately before Rasterize() so a test can exercise the
				// EXACT unwind path FrameStoreIsolationGuard /
				// RenderOverrideRestoreGuard / SampleCountRestoreGuard /
				// ProgressRestoreGuard exist for -- including with a film-
				// dims override already applied -- without depending on
				// OIDN or any other real throw site. `mThrowBeforeRasterizeForTest`
				// defaults to false; production code never sets it. See
				// AgentSession.h's ForTest_SetThrowBeforeRasterize doc.
				if( mThrowBeforeRasterizeForTest ) {
					throw std::runtime_error(
						"AgentSession::ForTest_ThrowBeforeRasterize: test-only forced throw immediately before Rasterize()" );
				}

				rendered = mJob->Rasterize();
				if( mController ) {
					// Read the cancel state BEFORE clearing the progress
					// hook -- IsCancelRequested() is a query, not a
					// consuming read, but keep the order symmetric with the
					// install-then-run-then-uninstall shape above.
					wasCancelled = mController->IsCancelRequested();
					mJob->SetProgress( priorProgress );
					progressGuard.Disarm();
				}
				renderRan = true;
				// P1 fix: capture the effective sample count NOW, under the
				// park (rast is guaranteed valid here) -- the tail (after
				// doRenderWork returns, possibly after the park has released)
				// must never dereference `rast` again; it reads this captured
				// int instead (see the `readBack` site below).
				productionSampleReadBack = rast->GetSampleCountOverride();

				// ---- Restore, in reverse order, BEFORE unlocking (live mode)
				// / returning (headless mode) -- ReadDocument's byte-identity
				// contract only covers the Document, but the active camera's
				// PROPERTIES must also be back to their pre-call values (the
				// restoration test AgentSession callers rely on).  This is the
				// ORDINARY-path restore; the guard exists for the ABNORMAL
				// (exception) path, so Disarm() it here to skip the
				// redundant destructor-time restore.  (Toolkit slice 2: this
				// is `restoreFilmAndCameraOverridesOrdinary()`, the SAME
				// shared helper doDraftRenderWork calls.)
				restoreFilmAndCameraOverridesOrdinary();
				restoreGuard.Disarm();

				// Offscreen isolation: explicit ordinary-path restore of the
				// rasterizer's FrameStore IDENTITY, same "Disarm after
				// explicit restore" discipline as restoreGuard just above.
				// Placed AFTER the film-dims restore so identity wins last:
				// `overrodeFilm`'s SetFilm(origFilmW, origFilmH, origFilmPAR)
				// just above reallocates a BRAND NEW FrameStore instance at
				// the original dims (Job::EnsureJobFrameStore_locked always
				// reallocates on a dims change) -- that fresh instance is a
				// DIFFERENT object than `capturedDisplayStore` (the one the
				// VFS observer is bound to), so restoring identity here,
				// last, is what actually re-points the rasterizer back at
				// the object the viewport watches; without this line the
				// viewport would be left observing a stale, now-orphaned
				// store after every override-render.  No-op (safe) when
				// `concreteRast` or `capturedDisplayStore` is null.
				fsGuard.RestoreAndDisarm();

				// Model-B F2 slice S3: explicit ordinary-path restore of
				// the sample-count override, same "Disarm after explicit
				// restore" discipline as restoreGuard just above --
				// restores unconditionally (not just when overrodeSamples
				// is true) so a rasterizer that supports the override but
				// where THIS render didn't request one is still left at
				// whatever GetSampleCountOverride() reported before (a
				// no-op in that common case, since origSamples ==
				// whatever's already live).
				if( origSamples >= 1 ) {
					rast->SetSampleCountOverride( origSamples );
				}
				sampleGuard.Disarm();
				publishCompletedToLastRender();
			};

			// Model-B F2 slice S1: render identity.  When this call actually
			// routes through the controller's coordinator (either because an
			// override forces the RunPreviewRenderParked-equivalent park, OR
			// -- Model-B F2 slice S2a -- because a controller is attached at
			// all, even with no override), that path assigns the id (a real
			// coordinator-tracked job); the ONLY remaining session-local path
			// is fully headless (no controller attached whatsoever) -- see
			// AgentRenderResult::renderJobId's doc for the honesty contract
			// (session-local ids are not comparable across sessions).
			//
			// Model-B F2 slice S2a CLOSES the pre-existing race documented on
			// this method's own header comment (LIVE-MODE SAFETY): before
			// this slice, a controller-attached render with NO override
			// called doRenderWork() DIRECTLY on the calling thread, wholly
			// unserialized against DoOneRenderPass -- the ONLY unparked path
			// left after slice 1b's ApplyAgentParamEdit and preview-render's
			// override-window parking.  Routing it through
			// SceneEditController::SubmitAgentRenderSync means EVERY
			// controller-attached agent render -- override or not -- now
			// runs under the SAME cancel-and-park critical section the
			// interactive loop respects, on the dedicated agent-render
			// worker rather than the caller's own thread.
			//
			// S1-delta doc-truth fix: AgentRenderResult::renderJobId's field
			// doc claims "a FAILED render still carries a real, nonzero
			// renderJobId when the render actually reached that stage" --
			// but prior to this fix, a THROW out of doRenderWork() propagated
			// as a raw C++ exception past this whole function, so the id was
			// never attached to any result at all (the caller got an
			// exception, not an AgentRenderResult).  Fixed here: every call
			// shape that can run doRenderWork() (RunPreviewRenderParked,
			// SubmitAgentRenderSync, and the direct headless call) is now
			// wrapped in its OWN try/catch that converts a thrown exception
			// into res.ok=false + res.renderJobId=<the id that was assigned
			// before the throw> + a message naming the exception -- making
			// the doc's claim TRUE end-to-end instead of only true for the
			// non-throwing failure paths further down.  This is a DELIBERATE
			// behavior change from pre-S2a: Render() used to let a Rasterize()
			// throw (e.g. OIDN) escape as a raw exception; it now reports it
			// as an ordinary ok=false result, matching every other failure
			// mode this method already reports that way and matching the
			// RPC layer's existing catch-all (AgentRpc.cpp already turned an
			// escaped exception into a clean -32603 with no id attached --
			// this makes the C++ API layer equally honest without requiring
			// the wire transport).  AgentProposeRenderTest's two throw tests
			// are updated accordingly (see RunRestoreOnThrowTest /
			// RunPreS2HardeningTests): they now assert ok==false + a nonzero
			// renderJobId instead of a caught C++ exception, while the
			// restore-state assertions (the actual money assertions) are
			// unchanged -- restoration still runs via the same RAII guard.
			std::uint64_t renderJobId = 0;

			// Model-B F2 slice S2a: `assumeParked` means this call is
			// running INSIDE the controller's dedicated agent-render
			// worker already (RenderAsync's submitted closure) -- the
			// cancel-and-park critical section is already held by that
			// worker, so routing through RunPreviewRenderParked /
			// SubmitAgentRenderSync AGAIN from here would try to re-enter
			// the controller's non-recursive mMutex and self-deadlock.
			// Run the render body directly (same shape as the headless
			// branch) and report the id the ASYNC submitter already
			// minted, rather than minting or routing a fresh one.
			if( assumeParked ) {
				renderJobId = forcedJobId;
				std::string thrownMessage;
				try {
					doRenderWork();
				}
				catch( const std::exception& e ) { thrownMessage = e.what(); }
				catch( ... )                     { thrownMessage = "unknown exception"; }
				if( !thrownMessage.empty() ) {
					res.ok          = false;
					res.integrator  = mJob->GetActiveRasterizerName();
					res.renderJobId = renderJobId;
					res.message     = "render failed: " + thrownMessage;
					return res;
				}
			} else if( mController && ( wantFilmOverride || wantCameraOverrideForRouting ) ) {
				SceneEditController::RenderJobId controllerJobId = 0;
				bool parked = false;
				// Fix-round-8 P1: initialized to None, which is ALSO the
				// value that correctly describes the throw path (the
				// callee never reaches a refusal site when `fn` throws, so
				// it does not write this).
				SceneEditController::PreviewRenderRefusal refusal =
					SceneEditController::PreviewRenderRefusal::None;
				std::string thrownMessage;
				try {
					parked = mController->RunPreviewRenderParked(
						doRenderWork, SceneEditController::RenderClass::AgentPreview,
						String(), &controllerJobId, &refusal );
				}
				catch( const std::exception& e ) { thrownMessage = e.what(); }
				catch( ... )                     { thrownMessage = "unknown exception"; }
				if( !parked && thrownMessage.empty() ) {
					// Fix-round-1 P2-B: refused -- the override window could
					// not be safely parked against the interactive render
					// thread.  (Several causes; see the cause-discrimination
					// note further down before assuming "a transaction is
					// open".)
					//
					// DECIDED SEMANTICS: refuse HONESTLY and RETRIABLY.  The
					// prior code fell back to an un-overridden render by
					// re-entering RenderCore_(noOverride) -- but with a
					// controller attached and no override requested, that
					// recursive call lands in the `else if( mController )`
					// no-override branch below, which routes through
					// SubmitAgentRenderSync -- and SubmitAgentRenderSync
					// refuses for the EXACT SAME reason (mTxnOpen), producing
					// a confusing COMPOUND failure message rather than a
					// clean signal.  This was dead code protecting against a
					// race that no longer exists (S2a's mTxnOpen check is
					// stable for the duration of one RenderCore_ call), not a
					// real degrade path.  Mirror the edit verbs' retriable
					// phrasing (see ApplyAgentParamEdit / ApplyAgentChunkCrud_'s
					// "editor transaction in progress -- retry after the
					// gesture completes") so a caller sees the identical
					// wording whether it hit a param edit, a chunk edit, or a
					// preview render.
					// P2 fix (2026-07-19 mutation review): doRenderWork never
					// entered the park on THIS path (RunPreviewRenderParked
					// refused before running it) -- there is no live-render
					// state to resolve, and reading mJob->GetActiveRasterizerName()
					// here would be an unsynchronized read racing the interactive
					// render thread for nothing.  Leave res.integrator at its
					// default-constructed empty string; see AgentRenderResult::
					// integrator's field doc for the "never resolved" contract.
					//
					// RunPreviewRenderParked refuses at SEVEN distinct gates,
					// and this string is surfaced VERBATIM to the model
					// (CompareToReference puts it in `res.split.note`,
					// QueryObjectAt in its own `message`), so it steers the
					// next tool call.  Getting the cause wrong re-creates the
					// retry loop this branch exists to reduce.
					//
					// Fix-round-6 P2 tried to discriminate by READING
					// `CurrentRenderJob().active`.  Fix-round-8 P1: that was a
					// REGRESSION, not a fix.  `active` is not a proxy for the
					// agent-render gate -- RenderLoop mints an `active == true`
					// RenderClass::Interactive job for EVERY ordinary viewport
					// pass (SceneEditController.cpp, RenderLoop's per-pass
					// mint), so whenever the viewport is drawing (the normal
					// steady state) an mTxnOpen refusal was reported as "render
					// queued or in progress -- retry after it completes",
					// pointing the model at something that completes constantly
					// while the transaction that actually blocks stays open.
					// Two further windows were never even disclosed: both
					// SubmitAgentRenderAsync_Locked and RunPreviewRenderParked
					// CLAIM the coordinated-render gate BEFORE they mint a job
					// record, so during those windows the reconstruction gave
					// the wrong wording in the other direction too.
					//
					// PROPER FIX: stop reverse-engineering a cause the callee
					// already knows.  RunPreviewRenderParked now REPORTS which
					// gate fired via `outRefusal`, and this maps it.  That
					// eliminates the whole class -- races, gate-claimed-but-
					// not-yet-minted windows, and any refusal cause added
					// later: the retriable transaction wording is assigned
					// BEFORE the switch narrows it, so an enumerator somebody
					// adds without extending this switch still yields an
					// honest, retriable message instead of an EMPTY one --
					// while the switch stays TOTAL (no `default:` arm) so
					// -Wswitch flags the omission at build time first, and
					// warnings are bugs in this repo.
					//
					// Wording is reused verbatim from the controller's OWN
					// refusals so the two layers agree: SceneEditController's
					// chunk-CRUD refusals say exactly "render queued or in
					// progress -- retry after it completes", and the edit verbs
					// (ApplyAgentParamEdit / ApplyAgentChunkCrud_) say exactly
					// "editor transaction in progress -- retry after the
					// gesture completes".
					res.ok          = false;
					res.renderJobId = renderJobId;   // 0 here -- no render ran, matching the other pre-flight refusal paths in this function
					res.message     = "editor transaction in progress -- retry after the gesture completes";
					switch( refusal ) {
					case SceneEditController::PreviewRenderRefusal::CoordinatedRenderBusy:
						res.message = "render queued or in progress -- retry after it completes";
						break;
					case SceneEditController::PreviewRenderRefusal::ControllerStopped:
						// NOT retriable -- say so rather than inviting a retry
						// loop against a controller that is going away.
						res.message = "render refused: the editor is shutting down";
						break;
					case SceneEditController::PreviewRenderRefusal::InteractionFinalizeFailed:
						res.message = "render refused: an open editor interaction could not be finalized -- retry shortly";
						break;
					case SceneEditController::PreviewRenderRefusal::EditorBusy:
					case SceneEditController::PreviewRenderRefusal::None:
						// Keep the pre-switch default.  `None` is unreachable on
						// a refusal (the callee sets a cause at every `return
						// false`); listed so the switch stays total and -Wswitch
						// keeps working, and so a contract slip still produces
						// the honest, retriable message.
						break;
					}
					return res;
				}
				if( !thrownMessage.empty() ) {
					// P2 fix (2026-07-19 mutation review): same reasoning as the
					// refusal branch just above -- do not read live Job state on
					// the calling thread here.  See AgentRenderResult::integrator's
					// field doc.
					res.ok          = false;
					res.renderJobId = static_cast<std::uint64_t>( controllerJobId );
					res.message     = "render failed: " + thrownMessage;
					return res;
				}
				renderJobId = static_cast<std::uint64_t>( controllerJobId );
			} else if( mController ) {
				// Model-B F2 slice S2a: no override requested, but a
				// controller IS attached -- route through the SAME
				// cancel-and-park critical section (via the dedicated
				// worker) rather than calling doRenderWork() directly on
				// this thread.  This is the no-override race closure.
				SceneEditController::RenderJobId controllerJobId = 0;
				bool submitted = false;
				std::string thrownMessage;
				try {
					submitted = mController->SubmitAgentRenderSync( doRenderWork, String(), &controllerJobId,
						/*timeoutMs=*/30000, params.pinned );
				}
				catch( const std::exception& e ) { thrownMessage = e.what(); }
				catch( ... )                     { thrownMessage = "unknown exception"; }
				if( !submitted && thrownMessage.empty() ) {
					// Refused: either an editor transaction is open (same
					// rule as RunPreviewRenderParked), the single-slot
					// worker already has a render queued/running, or --
					// Model-B F2 slice S3 -- the occupant is a PINNED
					// render (never silently superseded; see
					// SubmitAgentRenderSync's `pinned` doc).  Honest
					// failure -- no fallback direct call here, since a
					// direct call is exactly the race this slice closes.
					// P2 fix (2026-07-19 mutation review): doRenderWork never
					// entered the park on THIS path (SubmitAgentRenderSync
					// refused before running it) -- see the RunPreviewRenderParked
					// refusal branch above and AgentRenderResult::integrator's
					// field doc for the same reasoning.
					res.ok = false;
					const SceneEditController::RenderJobStatus cur = mController->CurrentRenderJob();
					res.message = ( cur.active && cur.pinned )
						? "render refused: a pinned render is in flight -- pinned renders run to completion and are never superseded; retry after it completes"
						: "render refused: the agent-render worker is busy or an editor transaction is open -- retry shortly";
					return res;
				}
				if( !thrownMessage.empty() ) {
					// P2 fix (2026-07-19 mutation review): same reasoning --
					// do not read live Job state on the calling thread here.
					res.ok          = false;
					res.renderJobId = static_cast<std::uint64_t>( controllerJobId );
					res.message     = "render failed: " + thrownMessage;
					return res;
				}
				renderJobId = static_cast<std::uint64_t>( controllerJobId );
			} else {
				// Pre-S2 hardening: ODD ids only (see
				// mNextSessionLocalRenderJobId's doc) -- disjoint from
				// SceneEditController's EVEN coordinator-tracked ids.  Only
				// reachable HEADLESS now (no controller at all) -- a
				// controller-attached render always goes through one of the
				// two coordinator-tracked branches above (S2a).  Assign the
				// id BEFORE calling doRenderWork() (mirrors
				// RunPreviewRenderParked's "id names a call that ran, not a
				// call that succeeded" convention) so a throw still reports
				// a real id below.
				renderJobId = mNextSessionLocalRenderJobId;
				mNextSessionLocalRenderJobId += kSessionLocalRenderJobIdStride;
				std::string thrownMessage;
				try {
					doRenderWork();
				}
				catch( const std::exception& e ) { thrownMessage = e.what(); }
				catch( ... )                     { thrownMessage = "unknown exception"; }
				if( !thrownMessage.empty() ) {
					res.ok          = false;
					res.integrator  = mJob->GetActiveRasterizerName();
					res.renderJobId = renderJobId;
					res.message     = "render failed: " + thrownMessage;
					return res;
				}
			}
			res.renderJobId = renderJobId;

			// P1 fix: a `view` resolution failure (unknown name / a resolved
			// but non-pinhole NAMED VIEW) OR the production "no active
			// rasterizer" bail is now detected fresh INSIDE
			// doRenderWork (under the park -- see doRenderWork's top) rather
			// than on the calling thread before dispatch, so it can no
			// longer `return res;` directly from there.
			// res.ok/res.integrator/res.message were already fully populated
			// inside doRenderWork for this case -- just stop here so the
			// generic renderRan/rendered/HasImage fallback below (which would
			// overwrite the specific message with a generic "render failed")
			// never runs.
			if( specificFailureReported ) {
				return res;
			}

			// P1-B (belt-and-braces, fail-loud): a camera override field that
			// failed to apply means the requested pose was NOT achieved --
			// report ok=false rather than silently rendering un-overridden
			// (or worse, reporting cameraOverridden==true on a no-op).  The
			// guard already restored every field that DID apply before the
			// failure; only mJob->Rasterize() itself was skipped
			// (doRenderWork returns early on this path, before reaching the
			// Rasterize() call) -- the in-memory `sink` WAS already created
			// and attached (that setup runs ahead of the camera-override
			// block; see doRenderWork's "moved ahead of the film-dims /
			// camera-pose override blocks" comment), so it does still exist
			// here. The paired unwind guards release both our local reference
			// and the rasterizer's registered-output reference on this return.
			if( cameraOverrideFailed ) {
				// P2 fix (2026-07-19 mutation review): NOT a fresh read here --
				// doRenderWork already ran on this call (cameraOverrideFailed can
				// only be set from inside applyCameraOverride, which only runs
				// inside doRenderWork's park) and set res.integrator FIRST,
				// unconditionally, before doing anything else (see doRenderWork's
				// own comment at its top).  Re-reading mJob->GetActiveRasterizerName()
				// here, on the calling thread, AFTER the park has already
				// released, was both redundant and the exact unsynchronized-
				// std::string-read race the park fix exists to prevent.
				res.ok = false;
				res.cameraOverridden = false;
				res.message = "camera override failed: '" + cameraOverrideFailedField +
					"' did not parse -- render skipped, camera left unchanged";
				return res;
			}

			// Fix-round-1 P2-C: a CANCELLED render (Stop() / a session
			// teardown drain tripped the controller's cancel signal while
			// this render was in flight) is reported HONESTLY as a clean
			// failure, never as a partial-image success -- Job::Rasterize()
			// has no cancelled/success distinction of its own, and the sink
			// may well satisfy HasImage() with a partially-filled frame at
			// this point (some blocks flushed before the abort landed).
			// Checked BEFORE the renderRan/rendered/HasImage gate below so a
			// cancelled render never falls through to "ok" just because
			// SOME pixels got written.
			if( wasCancelled ) {
				safe_release( sink );
				// P2 fix (2026-07-19 mutation review): same reasoning as the
				// cameraOverrideFailed branch above -- doRenderWork already ran
				// (wasCancelled is only ever set from inside doRenderWork's park)
				// and already set res.integrator unconditionally.  Re-reading it
				// here on the calling thread was redundant and racy.
				res.ok          = false;
				res.renderJobId = renderJobId;
				res.message     = "render cancelled";
				return res;
			}

			if( !renderRan || !rendered || !sink || !sink->HasImage() ) {
				safe_release( sink );
				res.ok = false;
				res.message = ( renderRan && rendered ) ? "render produced no image" : "render failed";
				return res;
			}

			// The AOVs were compacted during OutputImage, while the private
			// FrameStore was still bound.  FrameStore restoration subsequently
			// re-announces the production/display store to every output.  Do not
			// let the cached agent sink retain that (potentially very large) store:
			// beauty pixels and the 7-B/pixel sidecar are now self-contained.
			sink->OnRasterizerFrameStoreChanged( nullptr );

			res.width  = sink->Width();
			res.height = sink->Height();
			res.png    = sink->ToPng();
			sink->MeanChannels( res.meanR, res.meanG, res.meanB );
			{
				const InMemoryRasterizerOutput::PerceptionInfo pi = sink->GetPerceptionInfo();
				res.perceptionAvailable = pi.available;
				res.perceptionPersistentBytes = pi.persistentBytes;
				res.perceptionAuxiliaryPeakBytes = pi.auxiliaryPeakBytes;
			}
			res.ok     = !res.png.empty();
			res.message = res.ok ? "ok" : "PNG encode produced no bytes";
			res.previewWidth     = res.width;
			res.previewHeight    = res.height;
			res.cameraOverridden = overrodeCamera;
			// Re-review P2 fix: a valid PINHOLE named view rendered through a
			// non-pinhole ACTIVE camera got its pose applied but its FOV
			// honestly dropped (see doRenderWork's view-resolution block) --
			// note that here rather than silently reporting an unqualified
			// success.
			if( viewFovSkippedActiveNonPinhole && res.ok ) {
				res.message += " (view \"" + params.view + "\": FOV not applied -- "
					"the active camera has no editable field-of-view (only a pinhole "
					"camera does); the view's pose (location/lookat/up) WAS applied)";
			}

			// Model-B F2 slice S3 (production) / Toolkit slice 2 (draft):
			// report the sample-count outcome.  DRAFT and PRODUCTION are
			// tracked through COMPLETELY INDEPENDENT bookkeeping --
			// production's `overrodeSamples`+`rast` (the production
			// rasterizer) vs draft's `draftSamplesApplied`+
			// `draftEffectiveSamples` (the now-destroyed ephemeral
			// pipeline, captured DURING doDraftRenderWork since there is
			// nothing left to query afterward) -- so this branches on
			// `isDraft` rather than sharing one code path.  See
			// AgentRenderParams::quality's doc for the draft sample-cap
			// honesty contract.
			if( isObjectMap ) {
				// Toolkit slice 3a: an objectmap render has exactly ONE
				// fidelity -- a single ray per pixel (the EXACTNESS
				// INVARIANT).  `samples` and `quality` are both honestly
				// ignored here (note: this branch is reached BEFORE the
				// production `else` that dereferences `rast`, so a
				// rasterizer-less head is safe).
				res.samplesOverridden = false;
				res.effectiveSamples  = 1;
				if( wantSamplesOverride && res.ok ) {
					res.message += " (objectmap ignores the samples override -- an identity render is exactly 1 spp for per-pixel exactness)";
				}
				if( params.quality == AgentRenderQuality::Draft && res.ok ) {
					res.message += " (objectmap ignores quality -- it has a single fidelity)";
				}
				// P2-1: at large object counts the palette exhausts its default
				// separation and degrades ONLY the min-L1 distance (colours stay
				// byte-UNIQUE and round-trippable).  Tell the agent honestly so
				// it matches legend entries by exact byte, not by eye.
				if( res.ok && objectMapPalette.minColorDistance < 24 ) {
					char note[192];
					std::snprintf( note, sizeof( note ),
						" (legend colours are byte-unique but closer than the default separation "
						"-- %zu objects exhausted the palette; match by exact colorHex byte, not by eye)",
						objectMapPalette.names.size() );
					res.message += note;
				}

				// Assemble the legend from the palette + the atomic tally
				// (single-threaded now the render is done).  One entry per
				// registered object in deterministic (sorted-name) id order,
				// plus a trailing "<unmapped>" entry IFF any hit pixel
				// resolved to no registered object.
				res.legend.clear();
				res.legend.reserve( objectMapPalette.names.size() + 1 );
				for( std::size_t id = 0; id < objectMapPalette.names.size(); ++id ) {
					LegendEntry e;
					e.name       = objectMapPalette.names[id];
					e.colorHex   = ObjectMapColorHex( objectMapPalette.bytes[id] );
					e.pixelCount = objectMapPalette.counts[id].load( std::memory_order_relaxed );
					res.legend.push_back( e );
				}
				const std::uint32_t unknownCount = objectMapPalette.counts.empty()
					? 0u
					: objectMapPalette.counts[ objectMapPalette.names.size() ].load( std::memory_order_relaxed );
				if( unknownCount > 0 ) {
					LegendEntry e;
					e.name       = "<unmapped>";
					e.colorHex   = ObjectMapColorHex( objectMapPalette.unknownBytes );
					e.pixelCount = unknownCount;
					res.legend.push_back( e );
				}
			} else if( isBeautyVariant ) {
				// GUI render modes P2a: a BeautyVariant render has a FIXED
				// production-class config (spp/bounces/OIDN, per the
				// registry) -- `samples`/`quality` are both honestly
				// ignored, same precedent as the data view-modes above, but
				// the effective sample count is the mode's REAL fixed spp
				// (not the ShaderPipeline exactness invariant's 1).  No
				// legend (no per-object identity registry).
				res.samplesOverridden = false;
				res.effectiveSamples  = viewModeInfo ? static_cast<int>( viewModeInfo->variantSamplesPerPass ) : 0;
				if( res.ok && ( wantSamplesOverride || params.quality == AgentRenderQuality::Draft ) ) {
					const char* modeName = viewModeInfo ? viewModeInfo->name : "view";
					res.message += " (mode:";
					res.message += modeName;
					res.message += " uses a FIXED production-quality config; quality/samples ignored)";
				}
			} else if( isViewMode ) {
				// GUI render modes P1: a view-mode render has exactly ONE
				// fidelity, the SAME EXACTNESS INVARIANT reasoning as objectmap
				// above -- a single ray per pixel, `samples`/`quality` both
				// honestly ignored.  No legend (view modes have no per-object
				// identity registry).
				res.samplesOverridden = false;
				res.effectiveSamples  = 1;
				if( res.ok && ( wantSamplesOverride || params.quality == AgentRenderQuality::Draft ) ) {
					const char* modeName = viewModeInfo ? viewModeInfo->name : "view";
					res.message += " (mode:";
					res.message += modeName;
					res.message += " is a single-pass diagnostic render; quality/samples ignored)";
				}
			} else if( isDraft ) {
				res.samplesOverridden = draftSamplesApplied;
				res.effectiveSamples  = draftEffectiveSamples;
				if( draftSamplesCapped && res.ok ) {
					char capNote[160];
					std::snprintf( capNote, sizeof( capNote ),
						" (draft quality caps samples at %d -- requested %d, rendered at %d)",
						kDraftMaxSamples, params.samples, draftEffectiveSamples );
					res.message += capNote;
				} else if( wantSamplesOverride && !draftSamplesApplied && res.ok ) {
					res.message += " (samples override not supported by the draft preview pipeline -- rendered at its default 1 spp)";
				}
			} else {
				res.samplesOverridden = overrodeSamples;
				if( overrodeSamples ) {
					res.effectiveSamples = params.samples;
				} else {
					const int readBack = productionSampleReadBack;   // P1 fix: captured under the park inside doRenderWork -- never re-dereference `rast` here
					res.effectiveSamples = ( readBack >= 1 ) ? readBack : 0;
					if( wantSamplesOverride && res.ok ) {
						res.message += " (samples override not supported by the active rasterizer -- rendered at the scene-authored count)";
					}
				}
			}

			// X-ray axis (docs/gui/RENDER_MODES.md "X-ray axis"): honest note
			// about whether `xray` actually took effect.  It only applies to
			// the view-mode pipeline (Normals/Depth/Facets/Wireframe) --
			// Beauty/Draft/ObjectMap all silently ignore it, matching the
			// quality/samples-ignored precedent used throughout this
			// function. Default is FALSE (see AgentRenderParams::xray), so the
			// common case notes that transmissive surfaces are shown; xray:true is
			// noted too, so a caller can tell "inactive by request" apart
			// from "not a view-mode render at all" (no note either way).
			if( res.ok && isBeautyVariant ) {
				// GUI render modes P2a: xray is meaningless for a
				// BeautyVariant mode -- it's a real production transport
				// render, not a first-hit diagnostic; skipping through
				// transmissive surfaces would defeat deep_reflect's entire
				// purpose (seeing what reflections/refractions resolve to).
				res.message += " (xray is ignored under mode:";
				res.message += viewModeInfo ? viewModeInfo->name : "";
				res.message += " -- variant modes render fixed production transport)";
			} else if( res.ok && isViewMode ) {
				if( params.xray ) {
					res.message += " (xray: transmissive surfaces skipped (straight-line))";
				} else {
					res.message += " (xray:false -- transmissive surfaces shown, not skipped)";
				}
			} else if( res.ok && params.xray ) {
				res.message += " (xray is ignored outside the view-mode diagnostics -- mode:\"normals\"|\"depth\"|\"facets\"|\"wireframe\")";
			}

			// GUI render modes P2b `render{light:}` surface (light solo):
			// honest note about whether `light` actually took effect.
			// Meaningful ONLY for Beauty (production) and BeautyVariant --
			// data view-modes/objectmap/draft never evaluate scene lighting
			// at all (first-hit diagnostic shaders / the studio preview's
			// own fixed headlamp+AO rig), so `light` is silently ignored
			// there, same precedent as quality/samples/xray above.  An
			// unresolved name already failed the render loudly inside
			// doRenderWork (see ApplyLightSoloByName's call sites) -- this
			// block only runs when res.ok, i.e. either `light` was empty or
			// it resolved.
			if( res.ok && !params.light.empty() ) {
				if( isBeautyVariant || ( !isObjectMap && !isViewMode && !isDraft ) ) {
					res.message += " (light solo: \"" + params.light + "\" is the only active light)";
				} else {
					const char* what = isObjectMap ? "objectmap"
						: isDraft ? "draft"
						: ( viewModeInfo ? viewModeInfo->name : "view" );
					res.message += " (light is ignored under mode:";
					res.message += what;
					res.message += " -- no scene lighting is evaluated)";
				}
			}

			// Cache for ReadImage() ONLY on a successful, non-empty encode --
			// a failed render must not wipe a prior good cache (ReadImage
			// documents "the LAST successful Render").  Also keep the SINK
			// itself (swap-release the previous one) so ReadImage(maxEdge) can
			// re-encode a downscaled PNG from the cached full-res linear
			// pixels without re-rendering -- `sink` already carries refcount 1
			// (our owning ref from `new` above); we transfer that ownership to
			// mLastSink instead of releasing it.
			//
			// Model-B F2 slice S2a: guarded by mAsyncCacheMutex -- when this
			// is running on the async worker thread (assumeParked), a
			// concurrent ReadImage() call on the submitter's thread must not
			// observe a torn mLastPng/mLastSink update.
			if( res.ok ) {
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				mLastPng = res.png;
				safe_release( mLastSink );
				mLastSink = sink;
				sink = nullptr;   // ownership transferred -- the unwind guard must not release it
				return res;
			}

			safe_release( sink );
			return res;
		}

		// Ephemeral-render cache guard -------------------------------------------

		namespace
		{
			//! RAII stash/restore of the LAST-RENDER cache state -- the pair
			//! `mLastPng` / `mLastSink` (the two fields `ReadImage()` serves
			//! and the `read_image` verb returns) PLUS the async-render
			//! result record `mLastAsyncRenderResult` /
			//! `mLastAsyncRenderResultJobId` (what `render_status` /
			//! `render_wait` report) -- across an EPHEMERAL `Render()` whose
			//! pixels must NEVER become "the last render the caller can read".
			//!
			//! WHY THIS EXISTS.  `Render()` unconditionally caches every
			//! success into that pair (RenderCore_'s tail, above).  Two call
			//! sites in this file fire an internal `mode:"objectmap"` render
			//! purely as a means to an end -- `QueryObjectAt` (one pixel ->
			//! one object name) and `CompareToReference`'s object/background
			//! RMSE split (a per-pixel mask).  Left unguarded, either one
			//! leaves a flat SEGMENTATION image sitting in the cache, so a
			//! caller that renders a beauty frame and then calls
			//! `read_image` -- exactly the sequence the
			//! modeling-from-image-captures skill teaches -- is handed the
			//! segmentation image and "judges" the beauty render from it.
			//! Both sites now take this guard, so the two can never drift
			//! apart again (they did: only CompareToReference was guarded).
			//!
			//! CONSTRAINTS THIS ENCODES -- do not "simplify" any of them away:
			//!
			//!  * The cache mutex MUST NOT be held across the guarded
			//!    `Render()` call.  `Render()` locks that SAME non-recursive
			//!    mutex at RenderCore_'s cache tail, so widening either the
			//!    ctor's or the dtor's lock scope to span the call
			//!    self-deadlocks the agent thread with no diagnostic.  Both
			//!    bodies therefore take the lock, finish, and release it.
			//!  * RAII rather than a plain second block: `mSavedSink` is a raw
			//!    refcounted pointer held across a call, so an unwind between
			//!    stash and restore would leak a full framebuffer AND silently
			//!    wipe the stashed cache.  `Render()` happens to catch
			//!    everything today, but it is not declared `noexcept`.  Every
			//!    site in this file that holds a raw refcounted pointer across
			//!    a RENDER call uses an RAII guard for exactly this reason --
			//!    `SinkUnwindGuard` around the sink attachment, the FOUR
			//!    `EphemeralPipelineGuard`s around the rasterizer/pipeline
			//!    swaps (verified 2026-07-27: FOUR distinct definitions, one per
			//!    ephemeral render closure -- doDraftRenderWork,
			//!    doObjectMapRenderWork, doViewModeRenderWork and
			//!    doBeautyVariantRenderWork; the count said "three"), and
			//!    `SinkLease` in `ReadImage(maxEdge,...)`.  (The
			//!    file's PNG codec helpers -- `DecodePngRgbAt` and
			//!    `EncodeLinearPassthroughPng_` -- do use plain paired
			//!    `safe_release` across their reader/writer calls; they are the
			//!    exception, not a precedent to copy here.)
			//!  * THE GUARDED SCOPE IS PRIMARILY A ZERO-BYTE WINDOW, not a
			//!    "shows-the-ephemeral-render" window.  The ctor moves the
			//!    cache OUT (swaps `mLastPng` with a default-constructed
			//!    vector, nulls `mLastSink`, zeroes the async-result id), so
			//!    from the ctor until SOME render's cache tail repopulates
			//!    it -- i.e. THE WHOLE RENDER DURATION -- a concurrent
			//!    `ReadImage()` on another thread returns ZERO BYTES,
			//!    `ReadImage(unsigned int,...)` returns empty too (by TWO
			//!    different lines: for `maxEdge > 0` from the `if( !sink )`
			//!    early-out right after that function's lock scope; for
			//!    `maxEdge == 0` from the IN-LOCK `return mLastPng;` inside
			//!    its `maxEdge == 0` branch, which hands back the emptied
			//!    cache and leaves outWidth/outHeight at 0 because
			//!    `mLastSink` is null), and `ReadPerception` likewise.
			//!    HOW WIDE, stated honestly: the ephemeral render is an
			//!    OBJECTMAP IDENTITY render, not a beauty pass -- measured
			//!    ~20ms at 256x256 (see QueryObjectAt's header doc);
			//!    CompareToReference's split runs it at the REFERENCE dims,
			//!    so it scales with pixel count but stays in that class.  A
			//!    multi-SECOND window is therefore QUEUEING, not rendering,
			//!    and only ONE of the two routes below can queue at all --
			//!    see the ROUTES bullet.  Only AFTER a cache tail
			//!    lands inside the window do the fields hold pixels again --
			//!    the ephemeral render's own segmentation image, or, if an
			//!    async render completed in here first, THAT render's beauty
			//!    frame.  Triage rule, stated in the direction that holds:
			//!    a "read_image returned byteLength 0 right after a
			//!    successful render" report MUST count this window as a live
			//!    suspect (cross-thread ReadImage is a designed call shape --
			//!    see mAsyncCacheMutex's doc); the converse does NOT follow,
			//!    since getting bytes back does not prove the reader was
			//!    outside the window.  The window is bounded by the scope and
			//!    never persists past it.
			//!  * ROUTES -- the two ephemeral call sites do NOT reach the
			//!    controller the same way, and which way decides whether an
			//!    in-flight async render's cache write is swallowed by this
			//!    window.  `RenderCore_` picks the route on
			//!    `wantFilmOverride` (`params.width > 0 && params.height > 0`)
			//!    or a camera override:
			//!
			//!      (A) NO OVERRIDE -- `query_object_at {x,y}` with no
			//!          width/height/camera, the common shape a model emits.
			//!          Routes through `SubmitAgentRenderSync`, which takes a
			//!          FIFO fairness ticket and WAITS (up to the 30000ms
			//!          `timeoutMs` RenderCore_ passes) for the single
			//!          agent-render slot; it does NOT cancel the occupant.
			//!          An async render in flight when the ctor runs
			//!          therefore runs to FULL completion INSIDE the window
			//!          -- cache write included, since the async closure
			//!          stores `mLastAsyncRenderResult` under
			//!          mAsyncCacheMutex before the worker releases the slot
			//!          the fairness wait is blocked on -- and the dtor then
			//!          discards it.  On this route the zero-byte window
			//!          really is seconds wide, and the RESIDUAL below is
			//!          the ordinary case, not a race.
			//!
			//!      (B) WITH OVERRIDE -- `CompareToReference`'s split (it
			//!          forces `omParams.width/height = refW/refH`, so
			//!          `wantFilmOverride` is ALWAYS true there) and
			//!          `query_object_at` whenever width/height/camera are
			//!          supplied.  Routes through
			//!          `SceneEditController::RunPreviewRenderParked`, which
			//!          tests `mAgentRenderBlocksInteractive` and returns
			//!          FALSE IMMEDIATELY, BEFORE it ever takes the
			//!          controller's render mutex (its own comment: checking
			//!          afterward would "wait for the render only to
			//!          refuse").  An async submission claims that gate at
			//!          SUBMIT time and holds it through worker completion,
			//!          so with one in flight this route does NOT wait -- it
			//!          is REFUSED in microseconds, `omr.ok` is false, and
			//!          the guard's window closes long before the async
			//!          render finishes.  That render's cache write then
			//!          lands AFTER the window and SURVIVES.  The caller is
			//!          told: CompareToReference records the refusal message
			//!          in `res.split.note`, QueryObjectAt in its own
			//!          `message`.
			//!
			//!  * RESIDUAL, accepted: an async render that completes INSIDE
			//!    the window loses its ENTIRE cache write -- its pixels (the
			//!    dtor restores png/sink unconditionally) AND its result
			//!    record.  On route (A) this is the ORDINARY outcome, per
			//!    the wait described above.  On route (B) it shrinks to a
			//!    genuinely narrow race: the async render must complete in
			//!    the microseconds between the ctor and
			//!    RunPreviewRenderParked's gate test (complete any earlier
			//!    and the gate is already clear, so the split render simply
			//!    runs with nothing of the async render's left to lose;
			//!    complete any later and its write lands outside the
			//!    window).  Either way the handling is the same, and
			//!    stashing the result record alongside the pixels is
			//!    deliberate: the pixels are lost either way, so KEEPING an
			//!    `ok:true` record would make `render_wait` answer
			//!    `completed:true` with a full `{ok,width,height,meanR,...}`
			//!    payload for a frame `read_image` can no longer serve -- the
			//!    exact "success reported over stale pixels" lie this guard
			//!    exists to prevent.  Discarding both makes the loss uniform
			//!    and surfaces through `render_wait`'s ALREADY-HANDLED
			//!    completed-but-no-cached-result path (AgentRpc.cpp sets the
			//!    additive `result` key only `if( ar.found )`, and that
			//!    verb's doc already lists "absent" as a normal outcome).
			//!    KNOWN COST of that uniformity, stated plainly: the async
			//!    closure caches FAILURES too (no `if( r.ok )`, unlike the
			//!    pixel tail), and an `ok:false` record has no pixels to be
			//!    inconsistent with -- so discarding it costs the caller a
			//!    failure MESSAGE ("render cancelled", an encode throw) it
			//!    could honestly have read back.  Judged the better trade
			//!    against a conditional restore whose rule ("keep it iff it
			//!    failed") is harder to hold in the head than "the window
			//!    leaves no trace".  The principled fix for the WHOLE
			//!    residual is upstream and out of scope here: give
			//!    `RenderCore_` a "do not cache this one" flag so an
			//!    ephemeral render never writes the cache at all and no
			//!    stash/restore is needed.
			//!
			//! Takes the pieces of state by reference rather than an
			//! `AgentSession&` so it stays a file-local helper with no access
			//! to the class's private section (no friendship, no header churn).
			class EphemeralRenderCacheGuard
			{
			public:
				EphemeralRenderCacheGuard( std::mutex& cacheMutex,
				                           std::vector<unsigned char>& lastPng,
				                           InMemoryRasterizerOutput*& lastSink,
				                           AgentRenderResult& lastAsyncResult,
				                           std::uint64_t& lastAsyncResultJobId )
					: mCacheMutex( cacheMutex ), mLastPng( lastPng ), mLastSink( lastSink ),
					  mLastAsyncResult( lastAsyncResult ), mLastAsyncResultJobId( lastAsyncResultJobId )
				{
					// THE STASH ORDER BELOW IS NOT LOAD-BEARING -- this
					// static_assert is what makes that true, so nobody has to
					// hold it in their head.  TWO sites bind the property, and
					// the DESTRUCTOR is the stronger one:
					//
					//  * ~EphemeralRenderCacheGuard also move-assigns
					//    AgentRenderResult, and a destructor is implicitly
					//    noexcept -- a throwing move there is std::terminate,
					//    which no restructuring of any constructor can avoid.
					//  * the ctor takes ownership of the sink's raw ref
					//    (mSavedSink) BEFORE its two AgentRenderResult
					//    move-assignments; a throw there unwinds out of the
					//    ctor, and a ctor that throws never runs its own dtor,
					//    so the stashed framebuffer would leak AND the caller's
					//    cache would stay wiped for the rest of the session.
					//
					// Fix-round-8 P2: the assert message used to name only the
					// ctor and suggest "restructure this ctor to stash the
					// result record first" -- an escape hatch that would
					// satisfy a future maintainer while leaving the dtor's
					// terminate hazard fully in place.  Naming the dtor, and
					// offering no alternative, is the honest framing: the only
					// fix is to keep the type nothrow-move-assignable.
					// Asserted at COMPILE time so the breakage is a build error
					// at the offending member, not a runtime terminate/leak
					// nobody traces back to here.
					static_assert( std::is_nothrow_move_assignable<AgentRenderResult>::value,
						"EphemeralRenderCacheGuard move-assigns AgentRenderResult in BOTH its ctor and "
						"its dtor.  ~EphemeralRenderCacheGuard is implicitly noexcept, so a throwing "
						"move-assign there is std::terminate; in the ctor it leaks the stashed "
						"framebuffer and leaves the last-render cache permanently wiped (a throwing "
						"ctor never runs its own dtor).  Keep AgentRenderResult's members "
						"nothrow-move-assignable -- there is no safe restructuring that avoids the "
						"destructor case." );

					std::lock_guard<std::mutex> lk( mCacheMutex );
					mSavedPng.swap( mLastPng );        // cache -> stash (cache now empty)
					mSavedSink = mLastSink;            // take over the sink's ref
					// Nulling this is MEMORY SAFETY, not just visibility (round-8
					// sabotage finding, recorded so nobody "simplifies" it as a
					// mere zero-byte-window nicety): the guarded Render()'s cache
					// tail safe_release()s whatever mLastSink holds before storing
					// its own sink.  Leave the stashed pointer aliased here and
					// that release frees the framebuffer mSavedSink still owns, so
					// the dtor hands a DANGLING sink back to the session.
					// Verified: dropping this line segfaults the suite.
					mLastSink  = nullptr;
					// Same move-out for the async-render result record: the
					// window must leave NO trace of anything that completed
					// inside it, pixels and result alike (see the RESIDUAL
					// bullet above).  Zeroing the id is what makes
					// LastAsyncRenderResult() report "not found" for the
					// duration -- 0 is never a real job id.
					mSavedAsyncResult      = std::move( mLastAsyncResult );
					mLastAsyncResult       = AgentRenderResult();
					mSavedAsyncResultJobId = mLastAsyncResultJobId;
					mLastAsyncResultJobId  = 0;
				}

				~EphemeralRenderCacheGuard()
				{
					std::lock_guard<std::mutex> lk( mCacheMutex );
					safe_release( mLastSink );         // drop the ephemeral render's sink
					mLastPng.swap( mSavedPng );        // stashed bytes back in place
					mLastSink  = mSavedSink;           // ownership handed back
					mSavedSink = nullptr;
					mLastAsyncResult      = std::move( mSavedAsyncResult );
					mLastAsyncResultJobId = mSavedAsyncResultJobId;
				}

				EphemeralRenderCacheGuard( const EphemeralRenderCacheGuard& ) = delete;
				EphemeralRenderCacheGuard& operator=( const EphemeralRenderCacheGuard& ) = delete;

			private:
				std::mutex&                 mCacheMutex;
				std::vector<unsigned char>& mLastPng;
				InMemoryRasterizerOutput*&  mLastSink;
				AgentRenderResult&          mLastAsyncResult;
				std::uint64_t&              mLastAsyncResultJobId;
				std::vector<unsigned char>  mSavedPng;
				InMemoryRasterizerOutput*   mSavedSink = nullptr;
				AgentRenderResult           mSavedAsyncResult;
				std::uint64_t               mSavedAsyncResultJobId = 0;
			};
		}

		// Toolkit slice 3b: query_object_at -------------------------------------

		AgentSession::AgentQueryObjectResult AgentSession::QueryObjectAt(
			int x, int y, const AgentQueryObjectParams& params )
		{
			AgentQueryObjectResult res;

			if( !mJob ) {
				res.message = "no head loaded";
				return res;
			}

			if( x < 0 || y < 0 ) {
				res.outOfRange = true;
				res.message = "x/y out of range for the effective film dims";
				return res;
			}
			res.pixelX = static_cast<unsigned int>( x );
			res.pixelY = static_cast<unsigned int>( y );

			// IMPLEMENTATION CHOICE (a): reuse render's mode:"objectmap"
			// machinery WHOLESALE -- one full ephemeral identity render at
			// the effective dims, sharing every invariant that pipeline
			// already proves (exactness, byte-uniqueness, emissive
			// visibility, production-FrameStore isolation) -- rather than a
			// bespoke second GetCamera()->GenerateRay + caster code path.
			// See QueryObjectAt's header doc for the full rationale.
			AgentRenderParams rparams;
			rparams.renderTarget = AgentRenderTarget::ObjectMap;
			rparams.width  = params.width;
			rparams.height = params.height;
			rparams.camera = params.camera;

			// This objectmap render is EPHEMERAL -- it exists only to resolve
			// ONE pixel to ONE object name -- but Render() unconditionally
			// caches every success into mLastPng/mLastSink for ReadImage().
			// Left unguarded it would clobber whatever beauty frame the
			// caller last rendered, so the `read_image` that typically
			// follows would hand the model a flat SEGMENTATION image to judge
			// a beauty render from.  Same hazard, same guard, as
			// CompareToReference's split-mask render below -- see
			// EphemeralRenderCacheGuard's doc for the deadlock/refcount/
			// zero-byte-window constraints it encodes.  `rr` is the render's OWN returned
			// result and is unaffected by the restore: everything below
			// decodes rr.png / rr.legend, never the session cache.
			AgentRenderResult rr;
			{
				EphemeralRenderCacheGuard cacheGuard( mAsyncCacheMutex, mLastPng, mLastSink,
				                                      mLastAsyncRenderResult, mLastAsyncRenderResultJobId );
				// Fix-round-8 P2 seam: the guard's ctor has run (cache and
				// async-result record moved OUT to its stash) and the guarded
				// render has NOT started.  This is the only place the
				// documented "ONE window reports found == false" property is
				// observable, so it is the only place a test can pin the ctor
				// half -- see ForTest_SetEphemeralCacheGuardOpenHook's doc.
				// No AgentSession mutex is held here (the ctor took and
				// released mAsyncCacheMutex), so the hook may re-enter the
				// session's read paths.
				if( mEphemeralCacheGuardOpenHookForTest ) {
					mEphemeralCacheGuardOpenHookForTest();
				}
				rr = Render( rparams );
			}

			if( !rr.ok ) {
				res.message = rr.message.empty()
					? "query_object_at's identity render failed"
					: rr.message;
				if( rr.width )  res.width  = rr.width;
				if( rr.height ) res.height = rr.height;
				return res;
			}
			res.width  = rr.width;
			res.height = rr.height;

			// Determine bounds from the completed identity render, not from a
			// live Film read on the unparked caller thread.  Another render may
			// legally replace the session cache after Render returns, so decode
			// this result's own immutable PNG below rather than consulting it.
			if( res.pixelX >= res.width || res.pixelY >= res.height ) {
				res.outOfRange = true;
				res.message = "x/y out of range for the effective film dims";
				return res;
			}

			unsigned char red = 0, green = 0, blue = 0;
			if( !DecodePngRgbAt( rr.png, res.pixelX, res.pixelY, red, green, blue ) ) {
				res.message = "could not read the rendered pixel";
				return res;
			}

			// Decode EXACTLY the way ToPng()/PNGWriter's eColorSpace_sRGB
			// path does (RISEColor::Integerize<sRGBPel,unsigned char>(255.0)
			// -- see PNGWriter::WriteColor) so this byte is GUARANTEED
			// identical to what the objectmap PNG's corresponding pixel
			// carries -- the exact-byte legend match below rides the same
			// quantizer contract the palette generator guarantees.
			char hexBuf[8];
			std::snprintf( hexBuf, sizeof( hexBuf ), "#%02X%02X%02X",
				static_cast<unsigned>( red ), static_cast<unsigned>( green ), static_cast<unsigned>( blue ) );
			const std::string pixelHex( hexBuf );

			if( pixelHex == "#000000" ) {
				// The reserved background byte -- a genuine miss.  STRUCTURED
				// result, not a failure (see AgentQueryObjectResult::hit's doc).
				res.hit = false;
				res.message = "no object at this pixel";
				return res;
			}

			for( const LegendEntry& e : rr.legend ) {
				if( e.name == "<unmapped>" ) continue;
				if( e.colorHex == pixelHex ) {
					res.hit  = true;
					res.name = e.name;
					res.message = "ok";
					return res;
				}
			}

			// Either the pixel decoded to the reserved UNKNOWN colour (a hit
			// on an object the identity registry could not map -- see
			// ObjectIdShader::LookupAndTally's unknown branch) or no legend
			// entry matched at all (unreachable given the palette's byte-
			// uniqueness contract).  The ray DID hit something -- report a
			// hit with an honest caveat rather than silently claiming a miss.
			res.hit  = true;
			res.name = "";
			res.message = "hit an unregistered/unmapped object -- no legend name available for this pixel";
			return res;
		}

		// compare_to_reference ----------------------------------------------------

		namespace
		{
			//! compare_to_reference: decode PNG bytes already in memory into a
			//! tightly-packed 8-bit RGB pixel buffer (row-major, 3 bytes/pixel,
			//! alpha dropped) through RISE's OWN PNGReader -- the SAME decoder
			//! png_painter / read_image use.  A DELIBERATE, MINIMAL duplicate of
			//! AgentEvalRunner.cpp's DecodePngToRgb8 (that copy is a file-local
			//! helper in an anonymous namespace in a DIFFERENT translation unit
			//! -- not exported -- and the two call sites are small enough that
			//! factoring a shared header adds more indirection than it saves;
			//! keep the two in lockstep if the decode contract ever changes).
			//! Decodes with eColorSpace_Rec709RGB_Linear (the reader's "do
			//! nothing" branch: byte/255 in) and scales back out by *255 so
			//! the EXACT stored 8-bit byte the writer emitted comes back
			//! verbatim -- a pure pass-through with NO gamma step.
			//!
			//! The write-back ROUNDS (+0.5) rather than truncating, and that
			//! is load-bearing, not cosmetic.  The read side
			//! (Color_Template::SetFromIntegerized) scales by a PRECOMPUTED
			//! RECIPROCAL -- `OVMax = 1.0/255.0`, itself a rounded double --
			//! so the channel holds b*OVMax, which is NOT bit-identical to
			//! b/255.0.  Re-scaling that by *255.0 therefore lands a hair
			//! BELOW b for exactly 24 of the 256 byte values (33, 37, 41, 45,
			//! 49, 53, 57, 61, 66, 74, 82, 90, 98, 106, 114, 122, 132, 148,
			//! 164, 180, 196, 212, 228, 244) -- e.g. 180 -> 179.99999999999997.
			//! `Integerize` ends in a TRUNCATING cast, so those 24 values came
			//! back one LSB LOW while the other 232 round-tripped fine.
			//!
			//! That partial, value-dependent bias went unnoticed in the beauty
			//! RMSE (candidate and reference share the decoder, so it largely
			//! cancels in the difference) but silently broke the objectmap
			//! mask, whose whole contract is matching a decoded pixel to a
			//! legend colorHex by EXACT byte: the sphere's #3CB44B came back
			//! as #3CB34B -- green 180 is in the bad set, red 60 and blue 75
			//! are not -- so it matched nothing and EVERY pixel fell into the
			//! background bucket.  Round-trip exactness here is what the
			//! agent-facing "match by exact colorHex byte" instruction rests
			//! on -- keep the rounding.
			//! This is why compare_to_reference compares like-with-like: the
			//! candidate is decode(rr.png) where rr.png is byte-identical to
			//! what read_image returns, and a registered reference was authored
			//! by that same PNG family.  Returns false (populating `err`) on
			//! any decode failure -- empty buffer, non-PNG bytes, or degenerate
			//! dims -- never throws.
			//! The two decoders below read with eColorSpace_Rec709RGB_Linear and
			//! treat the resulting pel channel as the stored byte/255 VERBATIM.
			//! That is only a no-op while RISEPel IS Rec709RGBPel: PNGReader
			//! hardcodes SetFromIntegerized<Rec709RGBPel,...>, whose ColorBase
			//! conversion is a plain copy in that case.  If RISEPel is ever
			//! retyped -- the documented ACEScg migration in
			//! docs/COLOR_SPACE_MIGRATION.md would do exactly that -- the read
			//! silently becomes a real primaries conversion and EVERY decoded
			//! byte shifts, with no compile error and no failing test.  Fail
			//! loudly at compile time instead.
			static_assert( std::is_same<RISEPel, Rec709RGBPel>::value,
				"PNG decode assumes RISEPel == Rec709RGBPel (verbatim byte passthrough). "
				"RISEPel was retyped -- re-derive the decode path in "
				"DecodeReferencePngToRgb8_ / DecodePngToRgb8 before flipping the typedef." );

			//! Scale one decoded [0,1] channel back to its stored 8-bit byte,
			//! ROUNDING to the nearest integer and clamping to [0,255].  See
			//! DecodeReferencePngToRgb8_'s doc for why rounding (not
			//! Integerize's truncating cast) is required for exactness.
			//!
			//! The guard is written as `!( d > 0.0 )` rather than `d <= 0.0`
			//! so a NaN (every comparison against which is false) falls into
			//! the 0 branch instead of reaching the cast -- casting a NaN or
			//! an out-of-range double to an integral type is UB.  A PNGReader
			//! channel cannot currently be NaN, so this is defence in depth,
			//! not a live path.
			inline unsigned char QuantizeDecodedChannel_( Scalar v )
			{
				const double d = static_cast<double>( v ) * 255.0 + 0.5;
				if( !( d > 0.0 ) ) return 0;     // also catches NaN
				if( d >= 255.0 )   return 255;
				return static_cast<unsigned char>( d );
			}

			bool DecodeReferencePngToRgb8_( const unsigned char* bytes, std::size_t byteCount,
				std::vector<unsigned char>& outRgb, unsigned int& outW, unsigned int& outH,
				std::string& err )
			{
				outRgb.clear();
				outW = 0;
				outH = 0;
				if( !bytes || byteCount == 0 ) {
					err = "empty PNG buffer";
					return false;
				}

				IMemoryBuffer* buffer = nullptr;
				if( !RISE_API_CreateCompatibleMemoryBuffer( &buffer,
					const_cast<char*>( reinterpret_cast<const char*>( bytes ) ),
					static_cast<unsigned int>( byteCount ), /*bTakeOwnership=*/false ) || !buffer )
				{
					err = "could not wrap PNG bytes in a read buffer";
					return false;
				}

				IRasterImageReader* reader = nullptr;
				if( !RISE_API_CreatePNGReader( &reader, *buffer, eColorSpace_Rec709RGB_Linear ) || !reader ) {
					buffer->release();
					err = "could not create a PNG reader";
					return false;
				}

				unsigned int w = 0, h = 0;
				if( !reader->BeginRead( w, h ) || w == 0 || h == 0 ) {
					reader->EndRead();
					reader->release();
					buffer->release();
					err = "PNG decode failed (not a valid PNG, or zero dimensions)";
					return false;
				}

				outRgb.resize( static_cast<std::size_t>( w ) * h * 3 );
				for( unsigned int y = 0; y < h; ++y ) {
					for( unsigned int x = 0; x < w; ++x ) {
						RISEColor c;
						reader->ReadColor( c, x, y );
						// eColorSpace_Rec709RGB_Linear read leaves c.base holding
						// the stored byte/255 verbatim (RISEPel IS Rec709RGBPel --
						// no conversion). Scale back by *255 with ROUNDING, NOT
						// Integerize's truncating cast -- see the function doc for
						// why truncation loses an LSB and breaks objectmap matching.
						const std::size_t idx = ( static_cast<std::size_t>( y ) * w + x ) * 3;
						outRgb[idx + 0] = QuantizeDecodedChannel_( c.base.r );
						outRgb[idx + 1] = QuantizeDecodedChannel_( c.base.g );
						outRgb[idx + 2] = QuantizeDecodedChannel_( c.base.b );
					}
				}

				reader->EndRead();
				reader->release();
				buffer->release();
				outW = w;
				outH = h;
				return true;
			}

			//! compare_to_reference visual=true: encode `w`x`h` RISEColor
			//! pixels -- whose base.r/g/b already hold a DISPLAY-READY 8-bit
			//! byte value scaled to [0,1] (as DecodeReferencePngToRgb8_ above
			//! produces; NOT true HDR linear radiance) -- to 8-bit PNG bytes
			//! via the tree's PNGWriter using eColorSpace_Rec709RGB_Linear: the
			//! SAME "no further gamma" branch DecodeReferencePngToRgb8_ reads
			//! with (PNGWriter::WriteColor -> Integerize<Rec709RGBPel>), so
			//! byte N in -> byte N out, an exact pass-through rather than a
			//! re-gamma-encode.  Mirrors InMemoryRasterizerOutput.cpp's
			//! EncodePng helper (not exported / not reusable here -- a
			//! different translation unit's anonymous-namespace helper --
			//! and deliberately simpler: no display-transform wrapper, since
			//! the composite's bytes are already final display values).
			//! Returns an empty vector on a writer-creation failure or zero
			//! dims.
			std::vector<unsigned char> EncodeLinearPassthroughPng_(
				const std::vector<RISEColor>& pels, unsigned int w, unsigned int h )
			{
				std::vector<unsigned char> out;
				if( w == 0 || h == 0 ) return out;

				// `new` yields refcount 1 (Reference starts at 1) -- our owning
				// ref; PNGWriter addrefs it internally.  safe_release both at
				// the end (no extra addref taken here).
				Implementation::MemoryBuffer* buffer = new Implementation::MemoryBuffer();

				IRasterImageWriter* writer = nullptr;
				if( !RISE_API_CreatePNGWriter( &writer, *buffer, /*bpp=*/8, eColorSpace_Rec709RGB_Linear ) || !writer ) {
					safe_release( buffer );
					return out;
				}

				writer->BeginWrite( w, h );
				for( unsigned int y = 0; y < h; ++y ) {
					for( unsigned int x = 0; x < w; ++x ) {
						writer->WriteColor( pels[ static_cast<std::size_t>( y ) * w + x ], x, y );
					}
				}
				writer->EndWrite();   // flushes the encoded PNG bytes into `buffer`

				const unsigned int nBytes = buffer->getCurPos();
				const char* p = buffer->Pointer();
				if( p && nBytes > 0 ) {
					out.assign(
						reinterpret_cast<const unsigned char*>( p ),
						reinterpret_cast<const unsigned char*>( p ) + nBytes );
				}

				safe_release( writer );
				safe_release( buffer );
				return out;
			}

			//! compare_to_reference visual=true: map a per-pixel mean |delta|
			//! in [0,1] to an RGB byte triple through a simple 4-stop ramp:
			//! black (no difference) -> red -> yellow -> white (maximum
			//! difference).  Deliberately inline (~20 lines) rather than
			//! pulling in a general-purpose colour-ramp utility for this one
			//! call site.
			void HeatmapRampByte_( double t, unsigned char& r, unsigned char& g, unsigned char& b )
			{
				if( t < 0.0 ) t = 0.0;
				if( t > 1.0 ) t = 1.0;
				struct Stop { double r, g, b; };
				static const Stop kStops[4] = {
					{   0.0,   0.0,   0.0 },   // black  -- t=0
					{ 255.0,   0.0,   0.0 },   // red    -- t=1/3
					{ 255.0, 255.0,   0.0 },   // yellow -- t=2/3
					{ 255.0, 255.0, 255.0 }    // white  -- t=1
				};
				const double scaled = t * 3.0;
				int seg = static_cast<int>( scaled );
				if( seg > 2 ) seg = 2;
				const double f = scaled - seg;
				const Stop& a = kStops[seg];
				const Stop& c = kStops[seg + 1];
				r = static_cast<unsigned char>( a.r + ( c.r - a.r ) * f + 0.5 );
				g = static_cast<unsigned char>( a.g + ( c.g - a.g ) * f + 0.5 );
				b = static_cast<unsigned char>( a.b + ( c.b - a.b ) * f + 0.5 );
			}

			//! compare_to_reference: the human label for 3x3 grid cell
			//! `index` (0=top-left .. 8=bottom-right, row-major -- row =
			//! index/3, col = index%3).
			const char* GridCellLabel_( int index )
			{
				static const char* const kLabels[9] = {
					"top-left",    "top-center",    "top-right",
					"middle-left", "center",        "middle-right",
					"bottom-left", "bottom-center", "bottom-right"
				};
				return ( index >= 0 && index < 9 ) ? kLabels[index] : "unknown";
			}
		}

		AgentCompareToReferenceResult AgentSession::CompareToReference(
			const AgentCompareToReferenceParams& params )
		{
			AgentCompareToReferenceResult res;
			res.reference = params.reference;

			if( !mJob ) {
				res.error = "no head loaded";
				return res;
			}

			if( params.reference.empty() ) {
				res.error = "'reference' must be a non-empty name";
				res.badReference = true;
				return res;
			}

			const AgentReferenceImage* found = nullptr;
			for( const AgentReferenceImage& img : mReferenceImages ) {
				if( img.name == params.reference ) { found = &img; break; }
			}
			if( !found ) {
				std::string names;
				for( std::size_t i = 0; i < mReferenceImages.size(); ++i ) {
					if( i ) names += ", ";
					names += mReferenceImages[i].name;
				}
				res.error = "unknown reference '" + params.reference + "' -- registered reference(s): " +
					( names.empty() ? std::string( "(none registered)" ) : names );
				res.badReference = true;
				return res;
			}

			std::vector<unsigned char> refRgb;
			unsigned int refW = 0, refH = 0;
			std::string decErr;
			if( !DecodeReferencePngToRgb8_(
				reinterpret_cast<const unsigned char*>( found->pngBytes.data() ), found->pngBytes.size(),
				refRgb, refW, refH, decErr ) )
			{
				res.error = "reference '" + params.reference + "' could not be decoded (" + decErr + ")";
				return res;
			}

			// Render at the reference's EXACT dims, through the SAME pipeline
			// read_image / the eval checker's compareToImage use -- see
			// AgentCompareToReferenceParams' doc for the Draft/Production
			// quality tradeoff `samples` selects.
			AgentRenderParams rparams;
			rparams.width  = refW;
			rparams.height = refH;
			rparams.camera = params.camera;
			if( params.samples >= 1 ) {
				rparams.quality = AgentRenderQuality::Production;
				rparams.samples = params.samples;
			} else {
				rparams.quality = AgentRenderQuality::Draft;
			}

			const AgentRenderResult rr = Render( rparams );
			if( !rr.ok ) {
				res.error = "comparison render failed: " + rr.message;
				return res;
			}

			std::vector<unsigned char> candRgb;
			unsigned int candW = 0, candH = 0;
			if( !DecodeReferencePngToRgb8_(
				rr.png.empty() ? nullptr : rr.png.data(), rr.png.size(), candRgb, candW, candH, decErr ) )
			{
				res.error = "comparison render's PNG could not be decoded (" + decErr + ")";
				return res;
			}

			if( candW != refW || candH != refH ) {
				// Defensive only -- Render is forced to the reference's own
				// dims above, so this should be unreachable in practice; kept
				// so a future rasterizer quirk fails loud rather than reading
				// past either buffer's end.
				char db[320];
				std::snprintf( db, sizeof( db ),
					"comparison render is %ux%u but reference '%s' is %ux%u -- dims must match",
					candW, candH, params.reference.c_str(), refW, refH );
				res.error = db;
				return res;
			}

			res.width  = refW;
			res.height = refH;

			// Overall RMSE + per-channel mean signed delta -- THE SAME
			// FORMULA the eval checker's "render" checkpoint compareToImage
			// assertion uses (AgentEvalRunner.cpp's CheckRenderKind): this IS
			// the grader's own objective function, computed before the
			// grader ever runs.
			const std::size_t nPixels = static_cast<std::size_t>( refW ) * refH;
			double sumSq = 0.0;
			double sumDR = 0.0, sumDG = 0.0, sumDB = 0.0;
			for( std::size_t i = 0; i < nPixels; ++i ) {
				const double dr = ( static_cast<double>( candRgb[i*3+0] ) - static_cast<double>( refRgb[i*3+0] ) ) / 255.0;
				const double dg = ( static_cast<double>( candRgb[i*3+1] ) - static_cast<double>( refRgb[i*3+1] ) ) / 255.0;
				const double db = ( static_cast<double>( candRgb[i*3+2] ) - static_cast<double>( refRgb[i*3+2] ) ) / 255.0;
				sumSq += dr*dr + dg*dg + db*db;
				sumDR += dr; sumDG += dg; sumDB += db;
			}
			const std::size_t nSamples = nPixels * 3;
			res.rmse = ( nSamples > 0 ) ? std::sqrt( sumSq / static_cast<double>( nSamples ) ) : 0.0;
			res.channelDeltaR = ( nPixels > 0 ) ? sumDR / static_cast<double>( nPixels ) : 0.0;
			res.channelDeltaG = ( nPixels > 0 ) ? sumDG / static_cast<double>( nPixels ) : 0.0;
			res.channelDeltaB = ( nPixels > 0 ) ? sumDB / static_cast<double>( nPixels ) : 0.0;

			// 3x3 spatial grid: split into 3 columns / 3 rows (the last
			// column/row absorbs any remainder when width/height isn't a
			// multiple of 3); bounds are clamped monotonic-non-decreasing so
			// a degenerately small dim (width < 3, say) still yields
			// well-formed (possibly empty) cell ranges rather than inverted
			// ones.
			res.grid.assign( 9, AgentCompareGridCell() );
			unsigned int colBounds[4] = { 0, refW / 3, ( refW * 2 ) / 3, refW };
			unsigned int rowBounds[4] = { 0, refH / 3, ( refH * 2 ) / 3, refH };
			for( int i = 1; i < 4; ++i ) {
				if( colBounds[i] < colBounds[i-1] ) colBounds[i] = colBounds[i-1];
				if( rowBounds[i] < rowBounds[i-1] ) rowBounds[i] = rowBounds[i-1];
			}
			int worstIdx = 0;
			double worstRmse = -1.0;
			for( int gr = 0; gr < 3; ++gr ) {
				for( int gc = 0; gc < 3; ++gc ) {
					const int idx = gr * 3 + gc;
					double cellSumSq = 0.0, cellDR = 0.0, cellDG = 0.0, cellDB = 0.0;
					std::size_t cellN = 0;
					for( unsigned int y = rowBounds[gr]; y < rowBounds[gr+1]; ++y ) {
						for( unsigned int x = colBounds[gc]; x < colBounds[gc+1]; ++x ) {
							const std::size_t i = static_cast<std::size_t>( y ) * refW + x;
							const double dr = ( static_cast<double>( candRgb[i*3+0] ) - static_cast<double>( refRgb[i*3+0] ) ) / 255.0;
							const double dg = ( static_cast<double>( candRgb[i*3+1] ) - static_cast<double>( refRgb[i*3+1] ) ) / 255.0;
							const double db = ( static_cast<double>( candRgb[i*3+2] ) - static_cast<double>( refRgb[i*3+2] ) ) / 255.0;
							cellSumSq += dr*dr + dg*dg + db*db;
							cellDR += dr; cellDG += dg; cellDB += db;
							++cellN;
						}
					}
					AgentCompareGridCell& cell = res.grid[idx];
					if( cellN > 0 ) {
						cell.rmse = std::sqrt( cellSumSq / static_cast<double>( cellN * 3 ) );
						cell.dr = cellDR / static_cast<double>( cellN );
						cell.dg = cellDG / static_cast<double>( cellN );
						cell.db = cellDB / static_cast<double>( cellN );
					}
					if( cell.rmse > worstRmse ) {
						worstRmse = cell.rmse;
						worstIdx = idx;
					}
				}
			}
			res.worstCell = GridCellLabel_( worstIdx );

			// Object-vs-background RMSE split: an EXTRA, ephemeral
			// mode:"objectmap" render of the CANDIDATE at the SAME
			// dims/camera as the comparison above, used purely to build a
			// per-pixel object mask -- see AgentCompareSplitResult's doc
			// for the exact rule and the "candidate's own mask" honesty
			// caveat.  Pure ADD-ON: any failure here is recorded in
			// res.split.note and never fails the overall compare.
			res.hasSplit = params.split;
			if( params.split ) {
				AgentRenderParams omParams;
				omParams.width        = refW;
				omParams.height       = refH;
				omParams.camera       = params.camera;
				omParams.renderTarget = AgentRenderTarget::ObjectMap;

				// The objectmap render below is EPHEMERAL -- it exists only to
				// build a mask -- but Render() unconditionally caches every
				// success into mLastPng/mLastSink for ReadImage().  Left
				// alone it would clobber the beauty frame this compare just
				// graded, silently breaking the contract documented on
				// CompareToReference ("a caller CAN read_image afterward to
				// see the same frame the comparison graded") -- and the
				// modeling-from-image-captures skill recommends exactly that
				// compare-then-read_image sequence, so a model following the
				// skill would get handed a flat segmentation image.  Stash
				// the beauty cache, let the objectmap render populate a
				// throwaway, then put the beauty cache back.
				//
				// Shared with QueryObjectAt (the file's other ephemeral-render
				// site) so the two can never drift apart -- they did once, and
				// the unguarded one shipped the exact bug described above.
				// EphemeralRenderCacheGuard's doc carries the deadlock
				// ("never hold mAsyncCacheMutex across Render()"), refcount,
				// zero-byte-window, and discarded-async-result constraints
				// this scope relies on.
				AgentRenderResult omr;
				{
					EphemeralRenderCacheGuard cacheGuard( mAsyncCacheMutex, mLastPng, mLastSink,
					                                      mLastAsyncRenderResult, mLastAsyncRenderResultJobId );
					omr = Render( omParams );
				}

				if( !omr.ok ) {
					res.split.note = "split: candidate objectmap render failed (" +
						( omr.message.empty() ? std::string( "no message" ) : omr.message ) +
						") -- object/background split unavailable";
				} else {
					std::vector<unsigned char> objRgb;
					unsigned int objW = 0, objH = 0;
					std::string omDecErr;
					if( !DecodeReferencePngToRgb8_(
						omr.png.empty() ? nullptr : omr.png.data(), omr.png.size(), objRgb, objW, objH, omDecErr ) )
					{
						res.split.note = "split: candidate objectmap PNG could not be decoded (" + omDecErr +
							") -- object/background split unavailable";
					} else if( objW != refW || objH != refH ) {
						// Defensive only -- the objectmap render is forced to
						// the SAME refW/refH above, so this should be
						// unreachable in practice; kept so a future
						// rasterizer quirk fails loud rather than silently
						// mis-indexing the mask against candRgb/refRgb.
						char db[256];
						std::snprintf( db, sizeof( db ),
							"split: objectmap render is %ux%u but the comparison is %ux%u -- object/background split unavailable",
							objW, objH, refW, refH );
						res.split.note = db;
					} else {
						// The mask: a pixel is OBJECT iff its objectmap
						// colour matches a REAL (non-"<unmapped>") legend
						// entry's exact byte -- UNLESS the caller passed a
						// non-empty params.splitObjects, in which case the
						// mask is further scoped to ONLY the named entries
						// (see AgentCompareToReferenceParams::splitObjects'
						// doc for why: an unscoped mask counts a scene's own
						// ground plane / backdrop as OBJECT, since they are
						// ordinary registered objects too).  Background/
						// no-hit and unregistered-object hits both fall into
						// the background bucket either way -- see the struct
						// doc.
						std::unordered_set<std::string> objectColorHexes;
						std::vector<std::string> unknownSplitObjects;
						if( params.splitObjects.empty() ) {
							for( const LegendEntry& e : omr.legend ) {
								if( e.name == "<unmapped>" ) continue;
								objectColorHexes.insert( e.colorHex );
							}
						} else {
							// Scoped: only legend entries whose name was
							// explicitly requested count as OBJECT.
							// "<unmapped>" is never selectable by name -- it
							// is not a real object -- so it is skipped even
							// if a caller (mistakenly) names it.
							for( const std::string& want : params.splitObjects ) {
								const LegendEntry* match = nullptr;
								for( const LegendEntry& e : omr.legend ) {
									if( e.name == "<unmapped>" ) continue;
									if( e.name == want ) { match = &e; break; }
								}
								if( match ) objectColorHexes.insert( match->colorHex );
								else        unknownSplitObjects.push_back( want );
							}
						}

						double objSumSq = 0.0, bgSumSq = 0.0;
						std::size_t objN = 0, bgN = 0;
						for( std::size_t i = 0; i < nPixels; ++i ) {
							const std::array<unsigned char, 3> px =
								{ { objRgb[i*3+0], objRgb[i*3+1], objRgb[i*3+2] } };
							const bool isObject =
								objectColorHexes.find( ObjectMapColorHex( px ) ) != objectColorHexes.end();

							const double dr = ( static_cast<double>( candRgb[i*3+0] ) - static_cast<double>( refRgb[i*3+0] ) ) / 255.0;
							const double dg = ( static_cast<double>( candRgb[i*3+1] ) - static_cast<double>( refRgb[i*3+1] ) ) / 255.0;
							const double db = ( static_cast<double>( candRgb[i*3+2] ) - static_cast<double>( refRgb[i*3+2] ) ) / 255.0;
							const double sq = dr*dr + dg*dg + db*db;

							if( isObject ) { objSumSq += sq; ++objN; }
							else           { bgSumSq  += sq; ++bgN;  }
						}

						res.split.ok = true;
						res.split.objectPixelFraction =
							( nPixels > 0 ) ? static_cast<double>( objN ) / static_cast<double>( nPixels ) : 0.0;
						// BOTH buckets sentinel to -1 when empty, and the
						// sentinel is SYMMETRIC on purpose: reporting 0.0 for
						// an empty bucket would read to a model as "that
						// region matches the reference perfectly" when in
						// truth nothing was measured there at all -- the
						// exact misreading this split exists to prevent.
						if( objN > 0 ) {
							res.split.objectRmse = std::sqrt( objSumSq / static_cast<double>( objN * 3 ) );
						} else {
							res.split.objectRmse = -1.0;
							res.split.note = "split: no object pixels visible in the candidate's objectmap render "
								"(object off-frame, occluded, or camera pointed away) -- objectRmse unavailable, "
								"backgroundRmse covers the whole frame";
						}
						if( bgN > 0 ) {
							res.split.backgroundRmse = std::sqrt( bgSumSq / static_cast<double>( bgN * 3 ) );
						} else {
							res.split.backgroundRmse = -1.0;
							res.split.note = "split: no background pixels -- registered objects cover the ENTIRE frame "
								"(camera inside/too close to the geometry, or a backdrop object filling the view) -- "
								"backgroundRmse unavailable, objectRmse covers the whole frame";
						}

						// Requested-name diagnostics -- ONLY when the caller
						// scoped the mask (params.splitObjects non-empty).
						// This MUST override/augment the generic empty-mask
						// notes above whenever an unknown name is involved:
						// an empty OBJECT mask caused by a typo'd name is a
						// completely different failure than a genuinely
						// off-frame object, and reporting the off-frame
						// wording in that case would actively mislead a
						// caller trying to debug why their scoped split
						// came back empty.
						if( !params.splitObjects.empty() && !unknownSplitObjects.empty() ) {
							std::string availableNames;
							for( const LegendEntry& e : omr.legend ) {
								if( e.name == "<unmapped>" ) continue;
								if( !availableNames.empty() ) availableNames += ", ";
								availableNames += e.name;
							}
							if( availableNames.empty() ) availableNames = "(none registered)";

							std::string unknownNames;
							for( std::size_t i = 0; i < unknownSplitObjects.size(); ++i ) {
								if( i ) unknownNames += ", ";
								unknownNames += unknownSplitObjects[i];
							}

							if( unknownSplitObjects.size() == params.splitObjects.size() ) {
								// NONE of the requested names matched -- the
								// mask is empty because the name(s) don't
								// exist in this scene, NOT because the
								// object is off-frame/occluded.  Replace
								// (not append to) the generic empty-mask
								// note above so the off-frame wording never
								// appears here.
								res.split.note = "split: none of the requested splitObjects name(s) [" + unknownNames +
									"] exist in this scene's objectmap legend -- available object name(s): " +
									availableNames + " -- objectRmse unavailable";
							} else {
								// SOME requested names matched (the split
								// still computed normally on those) and some
								// didn't -- surface the unknown ones so a
								// typo can't silently shrink the mask
								// unnoticed.  Prepend to (rather than
								// clobber) any genuine empty-mask note the
								// blocks above may have already set.
								std::string unknownNote = "split: requested splitObjects name(s) not found and excluded "
									"from the OBJECT mask: [" + unknownNames + "] -- available object name(s): " +
									availableNames;
								res.split.note = res.split.note.empty()
									? unknownNote
									: unknownNote + " | " + res.split.note;
							}
						}
					}
				}
			}

			// Human summary.
			{
				const double overallDelta = ( res.channelDeltaR + res.channelDeltaG + res.channelDeltaB ) / 3.0;
				char buf[512];
				std::snprintf( buf, sizeof( buf ),
					"RMSE %.4f vs reference '%s'; mean channel delta dR=%+.3f dG=%+.3f dB=%+.3f "
					"(render is on average %s the reference by %.3f); worst region %s (RMSE %.4f)%s",
					res.rmse, params.reference.c_str(),
					res.channelDeltaR, res.channelDeltaG, res.channelDeltaB,
					( overallDelta >= 0.0 ? "brighter than" : "darker than" ), std::fabs( overallDelta ),
					res.worstCell.c_str(), worstRmse,
					( rr.renderMode == "draft"
						? " [draft mode -- materials/lighting ignored; this RMSE reflects geometry/composition only, not colour/material match -- pass samples>=1 for a production-quality reading]"
						: "" ) );
				res.summary = buf;

				if( res.hasSplit ) {
					// 1 KiB, not 320: the unknown-splitObjects notes embedded
					// by the branches below list EVERY available object name,
					// so a scene with many objects would silently truncate at
					// the old size.
					char splitBuf[1024];
					if( res.split.ok && res.split.objectRmse >= 0.0 && res.split.backgroundRmse >= 0.0 ) {
						std::snprintf( splitBuf, sizeof( splitBuf ),
							" | split: object-region RMSE %.4f, background RMSE %.4f, object covers %.0f%% of frame -- %s",
							res.split.objectRmse, res.split.backgroundRmse, res.split.objectPixelFraction * 100.0,
							( res.split.backgroundRmse + 0.02 < res.split.objectRmse )
								? "staging is close; the gap is the object"
								: "background/staging still carries meaningful error too" );
						// A note CAN be set even when BOTH buckets are valid: a
						// partial splitObjects match (some names found, some
						// unknown) computes fine but must still surface the
						// typo.  Dropping it would silently shrink the mask
						// behind the caller's back -- exactly what the
						// unknown-name diagnostics exist to prevent -- and the
						// summary is what a model actually reads.  Appended to
						// the std::string, NOT into splitBuf, because these
						// notes list every available object name and would
						// blow the fixed buffer's budget and be truncated.
						if( !res.split.note.empty() ) {
							res.summary += splitBuf;
							res.summary += " [" + res.split.note + "]";
							splitBuf[0] = '\0';   // already folded in above
						}
					} else if( res.split.ok && res.split.backgroundRmse >= 0.0 ) {
						std::snprintf( splitBuf, sizeof( splitBuf ),
							" | split: no object visible in the candidate (%s); background RMSE %.4f covers the whole frame",
							res.split.note.c_str(), res.split.backgroundRmse );
					} else if( res.split.ok ) {
						std::snprintf( splitBuf, sizeof( splitBuf ),
							" | split: no background visible in the candidate (%s); object RMSE %.4f covers the whole frame",
							res.split.note.c_str(), res.split.objectRmse );
					} else {
						std::snprintf( splitBuf, sizeof( splitBuf ),
							" | split: unavailable (%s)", res.split.note.c_str() );
					}
					res.summary += splitBuf;
				}
			}

			// Optional composite [render | reference | |delta| heatmap],
			// side-by-side, each panel at the reference's own dims.
			if( params.visual ) {
				const unsigned int compW = refW * 3;
				std::vector<RISEColor> composite( static_cast<std::size_t>( compW ) * refH );
				for( unsigned int y = 0; y < refH; ++y ) {
					for( unsigned int x = 0; x < refW; ++x ) {
						const std::size_t i = static_cast<std::size_t>( y ) * refW + x;
						composite[ static_cast<std::size_t>( y ) * compW + x ] =
							RISEColor( candRgb[i*3+0] / 255.0, candRgb[i*3+1] / 255.0, candRgb[i*3+2] / 255.0, 1.0 );
						composite[ static_cast<std::size_t>( y ) * compW + refW + x ] =
							RISEColor( refRgb[i*3+0] / 255.0, refRgb[i*3+1] / 255.0, refRgb[i*3+2] / 255.0, 1.0 );
						const double dr = std::fabs( static_cast<double>( candRgb[i*3+0] ) - static_cast<double>( refRgb[i*3+0] ) ) / 255.0;
						const double dg = std::fabs( static_cast<double>( candRgb[i*3+1] ) - static_cast<double>( refRgb[i*3+1] ) ) / 255.0;
						const double db = std::fabs( static_cast<double>( candRgb[i*3+2] ) - static_cast<double>( refRgb[i*3+2] ) ) / 255.0;
						unsigned char hr, hg, hb;
						HeatmapRampByte_( ( dr + dg + db ) / 3.0, hr, hg, hb );
						composite[ static_cast<std::size_t>( y ) * compW + 2*refW + x ] =
							RISEColor( hr / 255.0, hg / 255.0, hb / 255.0, 1.0 );
					}
				}
				res.compositePng = EncodeLinearPassthroughPng_( composite, compW, refH );
				if( !res.compositePng.empty() ) {
					res.compositeWidth  = compW;
					res.compositeHeight = refH;
				}
			}

			res.ok = true;
			return res;
		}

		// Model-B F2 slice S2a -------------------------------------------------

		AgentSession::AgentRenderAsyncResult AgentSession::RenderAsync( const AgentRenderParams& params )
		{
			AgentRenderAsyncResult out;
			out.pinned = params.pinned;   // echoed regardless of accepted -- see the struct doc

			if( !mController ) {
				out.accepted = false;
				out.message  = "no controller attached -- RenderAsync requires a LIVE controller (headless sessions have no coordinator/worker to submit to); use the synchronous Render() instead";
				return out;
			}

			// Submit a closure that runs the FULL render body (override
			// capture/apply/render/restore, same as the synchronous path)
			// via RenderCore_'s `assumeParked` mode -- it must NOT re-enter
			// the controller's routing (that would self-deadlock on
			// mMutex, since this closure already runs INSIDE the worker's
			// cancel-and-park hold).  `assumeParked=true` skips RenderCore_'s
			// own controller routing (this closure already runs INSIDE the
			// worker's cancel-and-park hold, so routing again would
			// self-deadlock); `forcedJobId` is left at its default 0 -- S2a's
			// minimal surface does not thread the id INTO the result the
			// worker discards (`r` below), only OUT via
			// SubmitAgentRenderAsync's `outJobId` param, which the caller
			// already has.  The cache-population tail inside RenderCore_
			// (guarded by mAsyncCacheMutex) populates mLastPng/mLastSink on a
			// successful render, which is how ReadImage() picks up the async
			// result once it completes.  S2a's minimal surface exposes
			// completion via RenderStatus/RenderWait + ReadImage rather than
			// returning the full AgentRenderResult from this call (there is
			// nothing to return yet at submit time -- the render hasn't run).
			//
			// Fix-round-1 P1-A: the closure captures `this` and a BY-VALUE
			// copy of `params` -- `this` does NOT unconditionally outlive
			// the async render (that was the bug: an AgentRpcDispatcher /
			// GUI teardown can destroy this AgentSession while the worker
			// thread is still inside this closure).  ~AgentSession now
			// DRAINS (cancel + wait, unbounded -- round-2 P1-1) any
			// outstanding async job before any member is torn down -- see
			// DrainAsyncRender_ -- which is what makes this capture safe in
			// practice: the destructor does not return until this closure
			// has finished running.
			//
			// Round-2 P1-2 fix: the closure needs to know ITS OWN jobId so
			// its completion guard can CLEAR mAsyncOutstandingJobId only if
			// it still names THIS closure's job (a compare, not an
			// unconditional zero) -- otherwise a stale closure (one whose
			// drain already timed out a cancel-ignoring render, if a future
			// change ever reintroduced a bound) could clear a NEWER
			// submission's id out from under a live drain.  The id is not
			// known until AFTER SubmitAgentRenderAsync mints it below, so a
			// plain by-value capture at lambda-construction time (before the
			// call) cannot see it -- share a heap cell (shared_ptr, default
			// 0 = "not yet known") that this function fills in AFTER
			// SubmitAgentRenderAsync returns, and that the closure reads
			// when it actually completes.  This is safe (not a second race)
			// specifically because of the NEW lock ordering below: this
			// function holds mAsyncCacheMutex across BOTH the
			// SubmitAgentRenderAsync call AND the publish, so the worker's
			// OutstandingGuard destructor -- which takes the SAME
			// mAsyncCacheMutex to read the cell and compare -- cannot run
			// until this function has already written the real id and
			// released the lock, however fast the worker gets there.
			auto ownJobIdCell = std::make_shared<std::uint64_t>( 0 );
			SceneEditController::RenderJobId jobId = SceneEditController::kInvalidRenderJobId;

			// Round-2 P1-2 fix: hold mAsyncCacheMutex across the WHOLE
			// mint-and-publish sequence (SubmitAgentRenderAsync + the
			// mAsyncOutstandingJobId write below) instead of releasing it
			// between the two.  The old code released the lock as soon as
			// SubmitAgentRenderAsync returned, then reacquired it to publish
			// the id -- if the worker (which can start the instant
			// SubmitAgentRenderAsync's internal notify_all() fires, i.e.
			// WHILE still inside that call on this thread's stack) ran a
			// trivially-fast closure to completion before this function got
			// back around to the publish, the OutstandingGuard's clear (see
			// below) landed on mAsyncOutstandingJobId==0 first and the
			// SUBSEQUENT publish then left it permanently stuck at a
			// nonzero, already-completed id -- a later drain would then
			// call CancelAgentRender_() (tripping the SHARED mCancelProgress
			// -- see that method's doc) against a controller that may by
			// then be running a wholly UNRELATED interactive or agent
			// render, spuriously cancelling it.
			//
			// LOCK ORDER -- this call site nests:
			//   mAsyncCacheMutex (AgentSession)  ->  mAgentRenderSlotMutex (SceneEditController, taken INSIDE SubmitAgentRenderAsync)
			//     -> (briefly, nested further) SceneEditController::mMutex, mJobStatusMutex
			// Full table + the worker-side REVERSE nesting (mMutex -> mAsyncCacheMutex,
			// inside RenderCore_'s cache-population tail) and why that reverse
			// order is still deadlock-safe (the two orderings apply to
			// disjoint instants, never simultaneous contenders for the same
			// pair -- the single-slot check that gates this method's brief
			// mMutex nesting only passes once the worker has already
			// RELEASED mMutex for whatever render preceded this submission)
			// is spelled out in full at SceneEditController.h, next to
			// mAgentRenderSlotMutex's three-lock note.  DrainAsyncRender_ /
			// the OutstandingGuard sites each take at most one of these
			// locks at a time, so neither is a second nesting site to
			// reason about here.
			std::unique_lock<std::mutex> cacheLk( mAsyncCacheMutex );

			const bool accepted = mController->SubmitAgentRenderAsync(
				[this, params, ownJobIdCell]() {
					struct OutstandingGuard {
						AgentSession&                       self;
						std::shared_ptr<std::uint64_t>      ownJobIdCell;
						~OutstandingGuard() {
							std::lock_guard<std::mutex> cacheLk( self.mAsyncCacheMutex );
							// Round-2 P1-2: compare-then-clear, not an
							// unconditional zero -- only retire the id if it
							// is still THIS closure's own job (cross-
							// generation safety: a stale closure must never
							// clear a newer submission's outstanding id).
							if( self.mAsyncOutstandingJobId == *ownJobIdCell ) {
								self.mAsyncOutstandingJobId = 0;
							}
						}
					} outstandingGuard{ *this, ownJobIdCell };
					AgentRenderResult r = RenderCore_( params, /*assumeParked=*/true );
					// Model-B F2 slice S2b: cache the FULL result (the whole
					// point of RenderCore_ having computed it) so a caller
					// that drove this render via render{"async":true} ->
					// render_status/render_wait can retrieve the identical
					// {ok,width,height,meanR,...} shape a synchronous Render()
					// call returns directly -- see LastAsyncRenderResult's
					// doc.  Guarded by the SAME mAsyncCacheMutex the
					// OutstandingGuard above already takes (a separate, brief
					// critical section here rather than widening the guard's
					// own hold, so a future change to OutstandingGuard's
					// scope doesn't have to reason about this too).
					//
					// r.renderJobId is OVERWRITTEN with `*ownJobIdCell` (this
					// closure's OWN job id, published by RenderAsync before
					// the worker could possibly have started) rather than
					// trusting whatever RenderCore_ set it to: `assumeParked`
					// mode calls RenderCore_ with the default `forcedJobId=0`
					// (there is no way to pass the real id in BEFORE
					// SubmitAgentRenderAsync mints it -- the same chicken/egg
					// this closure already solves for mAsyncOutstandingJobId
					// via ownJobIdCell), so RenderCore_'s internal
					// `renderJobId = forcedJobId;` line leaves r.renderJobId
					// at 0 on every async render, success or failure alike.
					// Left uncorrected, a caller reading the cached result's
					// OWN renderJobId field (e.g. the render_wait JSON echo)
					// would see 0 instead of the id it just polled.
					r.renderJobId = *ownJobIdCell;
					{
						std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
						mLastAsyncRenderResult      = r;
						mLastAsyncRenderResultJobId = *ownJobIdCell;
					}
				},
				String( "render_async" ),
				&jobId,
				params.pinned );

			if( !accepted ) {
				out.accepted = false;
				// Model-B F2 slice S3: distinguish "a PINNED render is
				// occupying the slot" from the generic busy/transaction
				// refusal -- CurrentRenderJob() is a fast mJobStatusMutex
				// read (never blocks behind an in-flight render; see that
				// method's doc), so this check is cheap even though the
				// slot was JUST found busy a moment ago.  A benign race
				// (the pinned job completes between the refusal above and
				// this read) just falls back to the generic message,
				// which is still accurate (the submission WAS refused).
				const SceneEditController::RenderJobStatus cur = mController->CurrentRenderJob();
				out.message = ( cur.active && cur.pinned )
					? "render refused: a pinned render is in flight -- pinned renders run to completion and are never superseded; retry after it completes"
					: "render refused: the agent-render worker is busy or an editor transaction is open -- retry shortly";
				return out;
			}

			// Publish BEFORE releasing mAsyncCacheMutex (still held from
			// above) -- this is the fix: the worker's OutstandingGuard
			// cannot observe (or clear) this id until this write has
			// landed, because it takes the SAME lock.  Fill the shared
			// cell too, so the closure's own eventual compare reads the
			// real id rather than the placeholder 0.
			*ownJobIdCell          = static_cast<std::uint64_t>( jobId );
			mAsyncOutstandingJobId = static_cast<std::uint64_t>( jobId );
			cacheLk.unlock();

			out.accepted    = true;
			out.renderJobId = static_cast<std::uint64_t>( jobId );
			out.message     = "submitted";
			return out;
		}

		AgentSession::AgentRenderJobStatus AgentSession::RenderStatus( std::uint64_t renderJobId ) const
		{
			AgentRenderJobStatus out;
			if( !mController ) return out;   // headless -- no coordinator to ask
			const SceneEditController::RenderJobLookup lookup =
				mController->GetRenderJobStatus( static_cast<SceneEditController::RenderJobId>( renderJobId ) );
			out.found  = lookup.found;
			out.active = lookup.found && lookup.status.active;
			out.pinned = lookup.found && lookup.status.pinned;
			return out;
		}

		bool AgentSession::RenderWait( std::uint64_t renderJobId, unsigned int timeoutMs ) const
		{
			if( !mController ) return false;   // headless -- nothing to wait for
			return mController->WaitForRenderJob(
				static_cast<SceneEditController::RenderJobId>( renderJobId ), timeoutMs );
		}

		void AgentSession::CancelAsyncRender( std::uint64_t renderJobId )
		{
			// `renderJobId` is advisory only (see the header doc) -- the
			// controller's agent-render worker is single-slot, so there is
			// at most one outstanding async render to cancel regardless of
			// which id was named.  A headless session (no controller) has
			// nothing to cancel: no-op, matching CancelAgentRender_'s own
			// "safe to call on an idle controller" contract rather than
			// erroring out.
			(void)renderJobId;
			if( !mController ) return;
			mController->CancelAgentRender_();
		}

		AgentSession::AgentLastAsyncRenderResult AgentSession::LastAsyncRenderResult( std::uint64_t renderJobId ) const
		{
			std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
			AgentLastAsyncRenderResult out;
			// Strict identity check (see the header doc): renderJobId must
			// match the job THIS cache entry belongs to.  renderJobId==0 is
			// never a real job id (kInvalidRenderJobId / the "none" sentinel
			// -- see AgentRenderResult::renderJobId's doc), so it can never
			// spuriously match an unset mLastAsyncRenderResultJobId==0 cache.
			if( renderJobId == 0 || renderJobId != mLastAsyncRenderResultJobId ) return out;
			out.found  = true;
			out.result = mLastAsyncRenderResult;
			return out;
		}

		std::vector<unsigned char> AgentSession::ReadImage() const
		{
			SinkReadEntrantGuard entrant( *this );
			std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
			if( mSinkReadsClosing ) return std::vector<unsigned char>();
			return mLastPng;
		}

		std::vector<unsigned char> AgentSession::ReadImage( unsigned int maxEdge,
		                                                    unsigned int& outWidth,
		                                                    unsigned int& outHeight ) const
		{
			SinkReadEntrantGuard entrant( *this );
			outWidth = 0;
			outHeight = 0;
			InMemoryRasterizerOutput* sink = 0;
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				if( mSinkReadsClosing ) return std::vector<unsigned char>();
				if( maxEdge == 0 ) {
					// No bound requested -- byte-compatible with ReadImage(): same
					// cached bytes, dims read off the cached sink when available.
					if( mLastSink ) {
						outWidth  = mLastSink->Width();
						outHeight = mLastSink->Height();
					}
					return mLastPng;
				}
				sink = mLastSink;
				if( sink ) {
					// Register first: map insertion may allocate and throw, while
					// mLastSink still owns the object under this lock. Roll the
					// registry back if the legacy virtual addref() ever throws.
					++mActiveSinkReadLeases[sink];
					try {
						sink->addref();
					}
					catch( ... ) {
						auto it = mActiveSinkReadLeases.find( sink );
						if( --it->second == 0 ) mActiveSinkReadLeases.erase( it );
						throw;
					}
				}
			}
			if( !sink ) return std::vector<unsigned char>();
			struct SinkLease {
				const AgentSession* session;
				InMemoryRasterizerOutput* sink;
				~SinkLease() { session->ReleaseSinkReadLease_( sink ); }
			} lease{ this, sink };
			return sink->ToPngDownscaled( maxEdge, outWidth, outHeight );
		}

		std::vector<unsigned char> AgentSession::ReadPerception(
			unsigned int maxEdge,
			unsigned int& outWidth,
			unsigned int& outHeight,
			AgentPerceptionInfo& outInfo ) const
		{
			SinkReadEntrantGuard entrant( *this );
			if( mReadPerceptionBeforeCacheHookForTest ) {
				mReadPerceptionBeforeCacheHookForTest();
			}
			outWidth = outHeight = 0;
			outInfo = AgentPerceptionInfo();
			InMemoryRasterizerOutput* sink = 0;
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				if( mSinkReadsClosing ) return std::vector<unsigned char>();
				sink = mLastSink;
				if( sink ) {
					// See ReadImage(maxEdge): registry insertion must precede the
					// new reference so std::bad_alloc cannot leak an untracked sink.
					++mActiveSinkReadLeases[sink];
					try {
						sink->addref();
					}
					catch( ... ) {
						auto it = mActiveSinkReadLeases.find( sink );
						if( --it->second == 0 ) mActiveSinkReadLeases.erase( it );
						throw;
					}
				}
			}
			if( !sink ) return std::vector<unsigned char>();
			struct SinkLease {
				const AgentSession* session;
				InMemoryRasterizerOutput* sink;
				~SinkLease() { session->ReleaseSinkReadLease_( sink ); }
			} lease{ this, sink };
			if( mReadPerceptionAfterLeaseHookForTest ) mReadPerceptionAfterLeaseHookForTest();
			InMemoryRasterizerOutput::PerceptionInfo pi;
			std::vector<unsigned char> png =
				sink->ToPerceptionPng( maxEdge, outWidth, outHeight, pi );
			outInfo.available = pi.available;
			outInfo.sourceWidth = pi.sourceWidth;
			outInfo.sourceHeight = pi.sourceHeight;
			outInfo.validDepthPixels = pi.validDepthPixels;
			outInfo.depthMin = pi.depthMin;
			outInfo.depthMax = pi.depthMax;
			outInfo.guidePrefilter = pi.guidePrefilter;
			outInfo.persistentBytes = pi.persistentBytes;
			outInfo.auxiliaryPeakBytes = pi.auxiliaryPeakBytes;
			outInfo.encoderRowBytes = pi.encoderRowBytes;
			return png;
		}

		// user-review P1-3 (round 2): the standalone DescribeViewportPanes was
		// REMOVED -- its one caller (read_viewport RPC) now takes the pane set
		// ATOMICALLY with the frame via SceneEditController::
		// SnapshotPaneSetForParkedRead (inside the parked window), so a separate
		// non-atomic getter that reads the pane set back AFTER the render resumed
		// would only invite drift.  The ViewportPaneInfo / ViewportPanesInfo wire
		// structs live on -- ReadViewport fills them.

		std::vector<unsigned char> AgentSession::ReadViewport(
			unsigned int maxEdge, unsigned int& outWidth, unsigned int& outHeight,
			bool& outAvailable, std::string& outReason,
		                                                 unsigned int& outSourcePane,
		                                                 ViewportPanesInfo& outPaneSet,
		                                                 bool& outHavePaneSet ) const
		{
			outWidth  = 0;
			outHeight = 0;
			outAvailable = false;
			outReason.clear();
			// review P2: initialize WITH the other out-params, before any early
			// return -- the no_controller path below returns without reaching
			// the later assignment, leaving this indeterminate for any future
			// caller that doesn't pre-init its local.
			outSourcePane = 0;
			// user-review P1-3: same all-paths init for the pane-set flag.
			outHavePaneSet = false;
			outPaneSet = ViewportPanesInfo();

			// No live controller -> no viewport at all (headless session).
			// This is a STRUCTURED unavailable, NOT an error.
			if( !mController ) {
				outReason = "no_controller";
				return std::vector<unsigned char>();
			}

			std::vector<RISEColor> pixels;
			unsigned int w = 0, h = 0;
			double vExposureEV = 0.0;
			int    vDisplayTransform = 2 /*eDisplayTransform_ACES*/;
			int    vColorSpace = eColorSpace_sRGB;
			bool copiedFrame = false;
			SceneEditController::PaneSetSnapshot paneSnap;   // user-review P1-3
			bool haveSnap = false;
			// Fix-round-8 P1 (sibling site): this call used to take the
			// one-arg overload and hard-code `editor_transaction_in_progress`
			// as the reason for EVERY refusal -- the same
			// misattribute-the-cause defect fixed in RenderCore_, just
			// expressed as a constant instead of a bad inference.  Route
			// through the identity-tracking overload purely to collect
			// `outRefusal` (the id is discarded, the class/label are exactly
			// what the one-arg forwarder passes, so this is behaviourally
			// identical apart from the reason string).
			SceneEditController::PreviewRenderRefusal refusal =
				SceneEditController::PreviewRenderRefusal::None;
			const bool parked = mController->RunPreviewRenderParked( [&]() {
				// Keep the frame copy and its live display-transform lookup in the
				// same parked interval.  CopyInteractiveFrame itself is tile-safe,
				// but ResolveBeautyDisplayTransform_ reads the Job's CST/camera.
				// user-review P1#3: the source pane is captured ATOMICALLY with
				// the frame (same frame-store lock), and this whole copy runs
				// PARKED, so it cannot drift from the returned pixels.
				copiedFrame = mController->CopyInteractiveFrame( pixels, w, h, &outSourcePane );
				// user-review P1-3: capture the WHOLE pane set HERE, inside the
				// same parked+locked window, so layout/primary/visibility/mode/
				// vantage describe the SAME frame as the pixels above (not a
				// later state read back through the locking getters after the
				// render resumed).  SnapshotPaneSetForParkedRead takes no lock --
				// mMutex is already held across this closure.
				mController->SnapshotPaneSetForParkedRead( paneSnap );
				haveSnap = true;
				if( copiedFrame ) {
					ResolveBeautyDisplayTransform_( vExposureEV, vDisplayTransform, vColorSpace );
				}
			}, SceneEditController::RenderClass::AgentPreview, String(),
			   /*outJobId=*/nullptr, &refusal );
			if( !parked ) {
				// The pre-existing reason is assigned FIRST and the switch
				// narrows it, so the gesture/save causes keep their exact
				// pre-round-8 wire value (this change is additive), a future
				// enumerator cannot produce an EMPTY reason, and the switch
				// still has no `default:` arm so -Wswitch catches the omission.
				outReason = "editor_transaction_in_progress";
				switch( refusal ) {
				case SceneEditController::PreviewRenderRefusal::CoordinatedRenderBusy:
					outReason = "render_in_progress";
					break;
				case SceneEditController::PreviewRenderRefusal::ControllerStopped:
					outReason = "editor_shutting_down";
					break;
				case SceneEditController::PreviewRenderRefusal::EditorBusy:
				case SceneEditController::PreviewRenderRefusal::InteractionFinalizeFailed:
				case SceneEditController::PreviewRenderRefusal::None:
					// Keep the pre-existing reason.  `None` is unreachable on a
					// refusal; listed to keep the switch total.
					break;
				}
				return std::vector<unsigned char>();
			}

			// Test-only race injector: the parked closure above already captured
			// pixels and pane metadata as one coherent snapshot.  Mutating the
			// live controller here proves callers never rebuild the response from
			// post-park getters, which could describe a later layout.
			if( mReadViewportAfterParkHookForTest ) {
				std::function<void()> hook = std::move( mReadViewportAfterParkHookForTest );
				hook();
			}

			// Publish the parked pane snapshot even when there is not yet a
			// frame.  Pane introspection is useful while the initial viewport pass
			// is pending, and the old read_viewport path returned it independently
			// of image availability.
			if( haveSnap ) {
				outHavePaneSet = true;
				outPaneSet.layout     = paneSnap.layout;
				outPaneSet.primary    = paneSnap.primary;
				outPaneSet.sourcePane = outSourcePane;
				for( unsigned int i = 0; i < SceneEditController::kViewportPaneCount; ++i ) {
					outPaneSet.panes[i].visible = paneSnap.panes[i].visible;
					outPaneSet.panes[i].contentSource =
						static_cast<int>( paneSnap.panes[i].contentSource );
					const Implementation::ViewportRenderModeInfo* mi =
						Implementation::FindViewportRenderModeInfo( paneSnap.panes[i].mode );
					outPaneSet.panes[i].mode        = mi ? mi->name : "preview";
					outPaneSet.panes[i].vantageKind = static_cast<int>( paneSnap.panes[i].vantageKind );
					outPaneSet.panes[i].namedView   = paneSnap.panes[i].namedView.c_str()
						? paneSnap.panes[i].namedView.c_str() : "";
				}
			}
			if( !copiedFrame ) {
				outReason = "no_frame_yet";
				return std::vector<unsigned char>();
			}

			// Encode via InMemoryRasterizerOutput WITHOUT re-rendering: adopt
			// the already-coherent buffer and reuse the exact ToPng /
			// ToPngDownscaled path read_image uses (box filter, linear space,
			// aspect-preserving downscale).  Scoped: released before return.
			InMemoryRasterizerOutput* sink = new InMemoryRasterizerOutput();
			sink->AdoptCoherentSnapshot( std::move( pixels ), w, h );
			// The interactive FrameStore is copied out LINEAR (a raw beauty
			// DumpImage, no view transform); apply the scene's effective
			// display transform at encode time so read_viewport shows the SAME
			// tonemapped image a human watching the viewport sees, matching
			// read_image (see ResolveBeautyDisplayTransform_).
			sink->SetDisplayTransform( vExposureEV, vDisplayTransform );
			sink->SetOutputColorSpace( vColorSpace );

			std::vector<unsigned char> png;
			if( maxEdge > 0 ) {
				png = sink->ToPngDownscaled( maxEdge, outWidth, outHeight );
			} else {
				png = sink->ToPng();
				outWidth  = sink->Width();
				outHeight = sink->Height();
			}
			safe_release( sink );

			outAvailable = true;
			return png;
		}
	}
}
