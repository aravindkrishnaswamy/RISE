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
#include "../Interfaces/ICamera.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/IObjectManager.h"   // Toolkit slice 3a (objectmap): enumerate scene objects for the identity registry
#include "../Interfaces/IObject.h"          // Toolkit slice 3a (objectmap): const IObject* registry key
#include "../Interfaces/IEnumCallback.h"    // Toolkit slice 3a (objectmap): EnumerateItemNames collector
#include "../Utilities/Color/ColorUtils.h"  // Toolkit slice 3a (objectmap): SRGBTransferFunctionInverse for the linear pre-image; transitively pulls in Color.h's COLOR_SPACE enum (external review P2 fix: resolved output colour space)
#include "../Painters/ExpressionEval.h"     // External review P2 fix: ExpressionProgram -- reuse the SAME public expr(...) evaluator Cst.cpp's derive-time resolver is built on, so ResolveBeautyDisplayTransform_ can resolve an expr(...)-valued `exposure` param instead of silently strtod'ing it to 0
#include "../Interfaces/ILog.h"   // P1-A: RenderOverrideRestoreGuard's defensive log-and-swallow
#include "../Rendering/FrameStore.h"   // offscreen isolation: FrameStoreIsolationGuard's private throwaway FrameStore
#include "../Rendering/Rasterizer.h"   // offscreen isolation: concrete GetFrameStore/SetFrameStore/AcceptsFrameStorePush (not on IRasterizer)
#include "../Rendering/InteractivePelRasterizer.h"   // Toolkit slice 2 (quality:"draft"): CreateInteractiveMaterialPreviewPipeline
#include "../RISE_API.h"
#include "../SceneEditor/SceneEditController.h"   // Facet 5 slice 1b: LIVE-mode routing through the render-safe edit path
#include "../SceneEditor/CameraIntrospection.h"   // preview-render: ephemeral camera-pose override
#include "../SceneEditor/ChunkDescriptorRegistry.h"
#include "../Parsers/ChunkDescriptor.h"
#include "../Parsers/IAsciiChunkParser.h"
#include "../Utilities/RString.h"

#include <algorithm>   // Facet 5 slice S1: std::sort for the deterministic skills index
#include <cctype>
#include <cfloat>   // DBL_MAX for the -ffast-math-safe non-finite range test
#include <cmath>
#include <cstdio>   // Facet 5 slice 1a: std::snprintf for the conflict message
#include <cstdlib>  // Facet 5 slice S1: std::getenv for the skills-root resolution
#include <stdexcept>  // Fix-round (offscreen isolation): ForTest_ThrowBeforeRasterize's std::runtime_error
#include <fstream>  // Facet 5 slice S1: read-only skill-file reads
#include <iterator> // Facet 5 slice S1: istreambuf_iterator for whole-file reads
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
							// Non-finite detection via an explicit range test,
							// NOT std::isfinite: the production build compiles
							// with -ffast-math (-> -ffinite-math-only), under
							// which clang folds std::isfinite(x) to true and
							// this classification silently degrades (the
							// AgentRpc.cpp house idiom, q.v.).  A plain
							// >=/<= against +/-DBL_MAX survives the fold:
							// NaN fails both bounds, +/-inf fails one.
							// HONEST CAVEAT: for values the compiler may
							// ASSUME finite under -ffinite-math-only this
							// comparison is tautologically true, so a
							// future clang could legally fold it too --
							// AgentChatLoopTest T15(b) pins the behaviour
							// against production-flag objects, catching a
							// toolchain regression at test time.
							const double dv = std::strtod( v.c_str(), nullptr );
							if( !LooksNumeric( v ) || !( dv >= -DBL_MAX && dv <= DBL_MAX ) ) { bad = true; break; }
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

		AgentSession::~AgentSession()
		{
			// Fix-round-1 P1-A: drain BEFORE touching any member -- the
			// controller-attached worker's RenderAsync closure captures a
			// raw `this` and may still be running RenderCore_ on it right
			// now.  DrainAsyncRender_ cancels (so the render aborts
			// promptly rather than running to completion) then blocks
			// until the controller reports the job no longer active, so by
			// the time we reach the lines below, no other thread can be
			// mid-call into this object.
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
			{
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
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
			return r;
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

		// Fill `out`'s location/lookAt/up (always) and fov (only for a
		// Pinhole snapshot -- GetPropertyValue-style "unreadable" honesty: a
		// ThinLens/Fisheye/Orthographic view has no single scalar FOV) from
		// a captured CameraSnapshot.
		void CameraSnapshotToOverride( const RISE::CameraSnapshot& snap,
		                                AgentCameraOverride& out )
		{
			out.hasLocation = true; out.location = Vec3ToOverrideStr( snap.location );
			out.hasLookAt   = true; out.lookAt   = Vec3ToOverrideStr( snap.lookat );
			out.hasUp       = true; out.up       = Vec3ToOverrideStr( snap.up );
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
					effectiveCamera.hasUp || effectiveCamera.hasFov );

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
			if( !isObjectMap ) {
				ResolveBeautyDisplayTransform_( beautyExposureEV, beautyDisplayTransform, beautyColorSpace );
			}

			// Toolkit slice 2 (quality:"draft"): the EPHEMERAL preview-
			// pipeline render body.  Never references `rast` (the production
			// rasterizer), its FrameStore, or mJob->RemoveRasterizerOutputs()
			// -- a fresh, throwaway InteractivePelRasterizer pipeline (studio-
			// preview shading only -- see CreateInteractiveMaterialPreviewPipeline
			// and AgentRenderQuality's doc) is constructed, used for exactly
			// this one render, and released before this lambda returns.
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
				// principle).  No production state is touched by this
				// branch, so there is nothing else to restore.
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
				// free.  This instance's RenderOverrideRestoreGuard has none
				// of the fsGuard-ordering constraints the production branch
				// documents (there is no FrameStore identity to protect
				// here), so it is simply constructed+armed up front.
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
			// references the production rasterizer, its FrameStore, or
			// mJob->RemoveRasterizerOutputs().
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
			// pass.  Never references the production rasterizer, its
			// FrameStore, or mJob->RemoveRasterizerOutputs().
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

					// External review P2 fix: `effectiveCamera` (AgentCameraOverride)
					// only HAS fields for location/lookAt/up/fov -- the pinhole-only
					// pose+FOV subset CameraSnapshotToOverride fills in above -- and
					// applyCameraOverride below only ever SetProperty's those four
					// names on the ACTIVE camera.  There is no plumbing here (or in
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
							"view's pose+fov onto the active camera (AgentCameraOverride has "
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
						const bool activeAcceptsFov = activeCamForFov
							&& CameraIntrospection::CaptureCameraSnapshot( *activeCamForFov, activeSnapForFov )
							&& activeSnapForFov.type == RISE::CameraSnapshot::Pinhole;
						if( !activeAcceptsFov && effectiveCamera.hasFov )
						{
							effectiveCamera.hasFov = false;
							viewFovSkippedActiveNonPinhole = true;
						}
					}
				}

				wantCameraOverride =
					( effectiveCamera.hasLocation || effectiveCamera.hasLookAt ||
						effectiveCamera.hasUp || effectiveCamera.hasFov );

				if( isObjectMap ) {
					doObjectMapRenderWork();
					return;
				}
				if( isViewMode ) {
					if( isBeautyVariant ) {
						doBeautyVariantRenderWork();
					} else {
						doViewModeRenderWork();
					}
					return;
				}
				if( isDraft ) {
					doDraftRenderWork();
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
				// addrefs it (the rasterizer's `outs` keeps that ref until the
				// next RemoveRasterizerOutputs / rasterizer teardown).  We drop
				// OUR ref via safe_release(sink) after this lambda returns --
				// no extra addref here.
				sink = new InMemoryRasterizerOutput();
				// Beauty render: encode through the scene's effective display
				// transform (exposure + tone curve) so decode(rr.png) matches
				// the CLI file-output / viewport pipeline for the SAME head --
				// the divergence the image_reconstruct compareToImage grading
				// depends on being closed (see ResolveBeautyDisplayTransform_).
				sink->SetDisplayTransform( beautyExposureEV, beautyDisplayTransform );
				sink->SetOutputColorSpace( beautyColorSpace );   // External review P2 fix: honour the scene's declared output colour space instead of a hardcoded sRGB
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
					// aovChannels intentionally left empty: this is SAFE, not
					// merely "AOV writes are elsewhere" -- in production,
					// PropagateAOVsToFrameStore DOES populate a FrameStore's
					// Albedo/Normal AOV channels from the rasterizer's AOV
					// buffers when they are present, but it is per-channel
					// null-safe (it skips any channel the FrameStore didn't
					// allocate storage for). An agent render has no consumer
					// reading AOV storage back out, so simply not allocating
					// it here is a safe, deliberate omission, not a gap.
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
				if( concreteRast && capturedDisplayStore ) {
					concreteRast->SetFrameStore( capturedDisplayStore );
				}
				safe_release( capturedDisplayStore );
				fsGuard.Disarm();

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
				std::string thrownMessage;
				try {
					parked = mController->RunPreviewRenderParked(
						doRenderWork, SceneEditController::RenderClass::AgentPreview,
						String(), &controllerJobId );
				}
				catch( const std::exception& e ) { thrownMessage = e.what(); }
				catch( ... )                     { thrownMessage = "unknown exception"; }
				if( !parked && thrownMessage.empty() ) {
					// Fix-round-1 P2-B: refused (an editor transaction is
					// open) -- the override window could not be safely
					// parked against the interactive render thread.
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
					res.ok          = false;
					res.renderJobId = renderJobId;   // 0 here -- no render ran, matching the other pre-flight refusal paths in this function
					res.message     = "editor transaction in progress -- retry after the gesture completes";
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
			// here.  Nothing to do about it explicitly, though: `sink` is
			// still owned by `sinkUnwindGuard` (declared in the enclosing
			// scope, still in scope at this `return`), which releases it
			// when this function returns -- no leak, just not this
			// early-return's job to handle.
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

			res.width  = sink->Width();
			res.height = sink->Height();
			res.png    = sink->ToPng();
			sink->MeanChannels( res.meanR, res.meanG, res.meanB );
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
			// function.  Default is now TRUE (see AgentRenderParams::xray),
			// so the common case notes "active"; an explicit xray:false is
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

		// Toolkit slice 3b: query_object_at -------------------------------------

		AgentSession::AgentQueryObjectResult AgentSession::QueryObjectAt(
			int x, int y, const AgentQueryObjectParams& params )
		{
			AgentQueryObjectResult res;

			if( !mJob ) {
				res.message = "no head loaded";
				return res;
			}

			// Effective dims: the SAME width/height-override composition
			// render's own overrides use (AgentRenderParams::width/height's
			// doc) -- both supplied means "use these transient dims",
			// otherwise the Document's authored Film dims.  A CHEAP,
			// read-only Film query -- NO render -- so an out-of-range (x,y)
			// is rejected BEFORE paying for the ephemeral identity render
			// below (see QueryObjectAt's header doc).
			unsigned int effW = params.width;
			unsigned int effH = params.height;
			if( effW == 0 || effH == 0 ) {
				const IScenePriv* scenePriv = mJob->GetScene();
				const IFilm* curFilm = scenePriv ? scenePriv->GetFilm() : nullptr;
				effW = curFilm ? curFilm->GetWidth()  : 0;
				effH = curFilm ? curFilm->GetHeight() : 0;
			}
			res.width  = effW;
			res.height = effH;

			if( effW == 0 || effH == 0 ||
			    x < 0 || y < 0 ||
			    static_cast<unsigned int>( x ) >= effW ||
			    static_cast<unsigned int>( y ) >= effH )
			{
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

			const AgentRenderResult rr = Render( rparams );

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

			// Defensive: the render's OWN effective dims should match the
			// pre-render computation above (identical composition rule) --
			// never read out of bounds on a mismatch.  Unreachable in
			// practice (both derive the same width/height/camera
			// composition), kept for robustness.
			if( res.pixelX >= res.width || res.pixelY >= res.height ) {
				res.outOfRange = true;
				res.message = "x/y out of range for the effective film dims";
				return res;
			}

			RISEColor pixel;
			bool gotPixel = false;
			{
				// Guarded by mAsyncCacheMutex like every other mLastSink
				// access (ReadImage/ReadImage(maxEdge)) -- the Render() call
				// just above already populated it synchronously on THIS
				// thread, so this is uncontended in practice; the lock costs
				// nothing and keeps the access pattern uniform.
				std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
				if( mLastSink ) gotPixel = mLastSink->GetPixelColor( res.pixelX, res.pixelY, pixel );
			}
			if( !gotPixel ) {
				res.message = "could not read the rendered pixel";
				return res;
			}

			// Decode EXACTLY the way ToPng()/PNGWriter's eColorSpace_sRGB
			// path does (RISEColor::Integerize<sRGBPel,unsigned char>(255.0)
			// -- see PNGWriter::WriteColor) so this byte is GUARANTEED
			// identical to what the objectmap PNG's corresponding pixel
			// carries -- the exact-byte legend match below rides the same
			// quantizer contract the palette generator guarantees.
			const RGBA_T<unsigned char> enc = pixel.Integerize<sRGBPel, unsigned char>( 255.0 );
			char hexBuf[8];
			std::snprintf( hexBuf, sizeof( hexBuf ), "#%02X%02X%02X",
				static_cast<unsigned>( enc.r ), static_cast<unsigned>( enc.g ), static_cast<unsigned>( enc.b ) );
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
			// (guarded by mAsyncCacheMutex) stashes mLastPng/mLastSink on a
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
			std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
			return mLastPng;
		}

		std::vector<unsigned char> AgentSession::ReadImage( unsigned int maxEdge,
		                                                    unsigned int& outWidth,
		                                                    unsigned int& outHeight ) const
		{
			outWidth = 0;
			outHeight = 0;
			std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );   // Model-B F2 slice S2a
			if( maxEdge == 0 ) {
				// No bound requested -- byte-compatible with ReadImage(): same
				// cached bytes, dims read off the cached sink when available.
				if( mLastSink ) {
					outWidth  = mLastSink->Width();
					outHeight = mLastSink->Height();
				}
				return mLastPng;
			}
			if( !mLastSink ) return std::vector<unsigned char>();   // nothing rendered yet
			return mLastSink->ToPngDownscaled( maxEdge, outWidth, outHeight );
		}

		std::vector<unsigned char> AgentSession::ReadViewport(
			unsigned int maxEdge, unsigned int& outWidth, unsigned int& outHeight,
			bool& outAvailable, std::string& outReason ) const
		{
			outWidth  = 0;
			outHeight = 0;
			outAvailable = false;
			outReason.clear();

			// No live controller -> no viewport at all (headless session).
			// This is a STRUCTURED unavailable, NOT an error.
			if( !mController ) {
				outReason = "no_controller";
				return std::vector<unsigned char>();
			}

			// Copy the live interactive frame out of the controller.  This
			// call does the cross-thread-safe, tile-locked COHERENT copy
			// against the render thread (see CopyInteractiveFrame); false
			// means the interactive render loop has not produced a frame yet.
			std::vector<RISEColor> pixels;
			unsigned int w = 0, h = 0;
			if( !mController->CopyInteractiveFrame( pixels, w, h ) ) {
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
			{
				double vExposureEV = 0.0;
				int    vDisplayTransform = 2 /*eDisplayTransform_ACES*/;
				int    vColorSpace = eColorSpace_sRGB;
				ResolveBeautyDisplayTransform_( vExposureEV, vDisplayTransform, vColorSpace );
				sink->SetDisplayTransform( vExposureEV, vDisplayTransform );
				sink->SetOutputColorSpace( vColorSpace );   // External review P2 fix: honour the scene's declared output colour space instead of a hardcoded sRGB
			}

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
