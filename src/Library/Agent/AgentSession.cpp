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
#include "../Interfaces/ILog.h"   // P1-A: RenderOverrideRestoreGuard's defensive log-and-swallow
#include "../Rendering/FrameStore.h"   // offscreen isolation: FrameStoreIsolationGuard's private throwaway FrameStore
#include "../Rendering/Rasterizer.h"   // offscreen isolation: concrete GetFrameStore/SetFrameStore/AcceptsFrameStorePush (not on IRasterizer)
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
		}

		AgentRenderResult AgentSession::Render( int samplesOverride )
		{
			// Legacy entry point: build an all-absent AgentRenderParams so
			// this is BYTE-COMPATIBLE with the pre-preview-render behaviour
			// (samplesOverride is folded in -- still advisory/ignored, same
			// as before; see RenderCore_'s doc for why).
			AgentRenderParams params;
			params.samples = samplesOverride;
			return RenderCore_( params );
		}

		AgentRenderResult AgentSession::Render( const AgentRenderParams& params )
		{
			return RenderCore_( params );
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

			// Round-3 additive wire field: report the ACTIVE rasterizer's
			// registered type name (= its scene-file chunk keyword, e.g.
			// "bdpt_pel_rasterizer") so the agent can observe which
			// integrator a rasterizer insert_chunk activated.  Filled on
			// BOTH the success and the render-failure paths below -- the
			// active integrator is a property of the head, not of whether
			// this particular render produced an image.
			res.integrator = mJob->GetActiveRasterizerName();

			const bool wantFilmOverride =
				( params.width > 0 && params.height > 0 );
			const bool wantCameraOverride =
				( params.camera.hasLocation || params.camera.hasLookAt ||
				  params.camera.hasUp || params.camera.hasFov );

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
			// Model-B F2 slice S3: sample-count override state.  Captured
			// via GetSampleCountOverride() BEFORE any mutation (-1 = the
			// active rasterizer doesn't support the override at all);
			// `overrodeSamples` is set true only once SetSampleCountOverride
			// actually returns true for THIS render, so a caller reading
			// res.samplesOverridden gets an honest answer even when
			// `wantSamplesOverride` was requested against an unsupported
			// rasterizer.
			const int origSamples = rast->GetSampleCountOverride();
			bool overrodeSamples = false;
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

			auto doRenderWork = [&]()
			{
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

				// Model-B F2 slice S3: sample-count override.  Armed
				// BEFORE the apply (same "arm before mutate" discipline as
				// restoreGuard above) so a throw between here and the
				// explicit tail restore still restores via the destructor.
				// `origSamples` was already captured (GetSampleCountOverride,
				// before ANY mutation) by the caller, outside this lambda.
				// (No FrameStore/film interaction, so its position relative
				// to fsGuard/restoreGuard's load-bearing order is free.)
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
				// `wantFilmOverride` is false.
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
				// `overrodeCamera` reports true.
				if( wantCameraOverride ) {
					ICameraManager* cams = mJob->GetCameras();
					const std::string activeName = mJob->GetActiveCameraName();
					activeCam = ( cams && !activeName.empty() )
						? cams->GetItem( activeName.c_str() ) : nullptr;
					if( activeCam ) {
						// captureAndSet returns true iff the field was
						// requested AND applied cleanly; false on a rejected
						// SetProperty (the field is captured but NOT pushed
						// onto capturedCam in that case, since nothing was
						// actually changed -- there is nothing to restore
						// for it, and restoring it would be a harmless but
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
						// Apply in the SAME order as before; stop at the
						// first failure (fail-loud -- no point applying
						// further fields once one has already failed) but
						// keep going through captureAndSet's own bookkeeping
						// so every field ALREADY applied before the failure
						// is captured and will be restored by the guard.
						captureAndSet( params.camera.hasLocation, "location", params.camera.location )
							&& captureAndSet( params.camera.hasLookAt, "lookat", params.camera.lookAt )
							&& captureAndSet( params.camera.hasUp,     "up",     params.camera.up )
							&& captureAndSet( params.camera.hasFov,    "fov",    params.camera.fov );
						overrodeCamera = !capturedCam.empty();
					}
					// else: no active camera resolved at all -- nothing to
					// override; not itself a failure (matches pre-P1-B
					// behaviour: overrodeCamera stays false).
				}

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

				// ---- Restore, in reverse order, BEFORE unlocking (live mode)
				// / returning (headless mode) -- ReadDocument's byte-identity
				// contract only covers the Document, but the active camera's
				// PROPERTIES must also be back to their pre-call values (the
				// restoration test AgentSession callers rely on).  This is the
				// ORDINARY-path restore; the guard exists for the ABNORMAL
				// (exception) path, so Disarm() it here to skip the
				// redundant destructor-time restore.
				for( std::size_t i = capturedCam.size(); i-- > 0; ) {
					CameraIntrospection::SetProperty( *activeCam,
						String( capturedCam[i].name.c_str() ),
						String( capturedCam[i].value.c_str() ) );
				}
				if( overrodeFilm ) {
					mJob->SetFilm( origFilmW, origFilmH, origFilmPAR );
				}
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
			} else if( mController && ( wantFilmOverride || wantCameraOverride ) ) {
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
					res.ok          = false;
					res.integrator  = mJob->GetActiveRasterizerName();
					res.renderJobId = renderJobId;   // 0 here -- no render ran, matching the other pre-flight refusal paths in this function
					res.message     = "editor transaction in progress -- retry after the gesture completes";
					return res;
				}
				if( !thrownMessage.empty() ) {
					res.ok          = false;
					res.integrator  = mJob->GetActiveRasterizerName();
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
					res.ok = false;
					res.integrator = mJob->GetActiveRasterizerName();
					const SceneEditController::RenderJobStatus cur = mController->CurrentRenderJob();
					res.message = ( cur.active && cur.pinned )
						? "render refused: a pinned render is in flight -- pinned renders run to completion and are never superseded; retry after it completes"
						: "render refused: the agent-render worker is busy or an editor transaction is open -- retry shortly";
					return res;
				}
				if( !thrownMessage.empty() ) {
					res.ok          = false;
					res.integrator  = mJob->GetActiveRasterizerName();
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
				res.ok = false;
				res.integrator = mJob->GetActiveRasterizerName();
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
				res.ok          = false;
				res.integrator  = mJob->GetActiveRasterizerName();
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

			// Model-B F2 slice S3: report the sample-count override
			// outcome.  `overrodeSamples` is only true when
			// SetSampleCountOverride actually accepted THIS render's
			// request (see doRenderWork above); `effectiveSamples` is the
			// requested count on that path, else whatever
			// GetSampleCountOverride() reads back (the rasterizer's own
			// current/restored count, when that query is supported) --
			// never guessed.  A requested-but-unsupported override gets a
			// clear, additive note appended to `message` rather than
			// silently overwriting the "ok" success message.
			res.samplesOverridden = overrodeSamples;
			if( overrodeSamples ) {
				res.effectiveSamples = params.samples;
			} else {
				const int readBack = rast->GetSampleCountOverride();
				res.effectiveSamples = ( readBack >= 1 ) ? readBack : 0;
				if( wantSamplesOverride && res.ok ) {
					res.message += " (samples override not supported by the active rasterizer -- rendered at the scene-authored count)";
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
	}
}
