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
//  ...and it reports NOTHING AT ALL for a text that declares no chunks, so
//  ValidateText screens that case itself first (EMPTY_DOCUMENT) rather than
//  letting an empty / whitespace / comments-only text come back clean.
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
#include "../Rendering/AutoRasterizer.h"   // G1 fix-round (2026-08-10, render{isolate:}): concrete AutoRasterizer::PreResolveIntegrator -- the object-solo apply must pre-resolve the once-only integrator choice against the FULL scene (invariant 3)
#include "../Objects/CSGObject.h"   // G1 fix-round (2026-08-10, FIX 6): concrete CSGObject::GetOperandA/GetOperandB -- the real parent-composite lookup for ResolveIsolateObject's CSG-operand failure message
#include "../Scene.h"   // G1 (2026-08-10, render{isolate:}): concrete Scene::BumpLightTopologyGeneration -- the luminary-list rebuild trigger the object-solo apply/restore pair needs (not on IScenePriv; same downcast RayCaster.cpp's SceneLightGeneration uses)
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
#include <cstdint>  // Arc-75 S2.1: std::uint64_t for the material-scaffold jitter hash
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
							// finiteness test; bare -ffast-math could fold ordinary FP (fixed 2026-07-29)
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
				for( std::size_t i = 0; i < names.size(); ++i ) {
					std::string text;
					if( !ReadFileText( root + names[i] + ".md", text ) ) continue;
					AgentSkillEntry e;
					e.name = names[i];
					ParseSkillHeader( text, e.title, e.hook );
					if( e.title.empty() ) e.title = e.name;   // lenient: never a blank index row
					r.index.push_back( e );
				}
				// AN EMPTY INDEX IS NEVER SILENT.  A zero-skill result used
				// to be indistinguishable from a healthy one at every layer
				// (bare empty list here, no skills section in the system
				// prompt), so a miswired skills root degraded the agent with
				// no signal to anyone.  Say plainly that no skills are
				// available, that it is abnormal, and which root was tried.
				// The two causes stay distinguishable: a MISSING root
				// (miswired install / wrong cwd) vs a present-but-empty one.
				if( r.index.empty() ) {
					r.note = std::string( "NO SKILLS ARE AVAILABLE. RISE ships scene-authoring "
					                      "skills, so an empty index means this installation is "
					                      "miswired, not that there is nothing to read: " ) +
					         ( rootFound
					           ? "the skills root '" + root + "' exists but holds no readable *.md skill."
					           : "the skills root '" + root + "' does not exist." ) +
					         " Tell the user their RISE install cannot find its agent skills "
					         "(RISE_SKILLS_PATH overrides the location), and continue WITHOUT "
					         "skill guidance -- do not keep re-listing.";
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

		std::string AgentSession::RenderSkillIndex( const AgentSkillResult& skills )
		{
			// See the header doc: the single-source "name -- hook" rendering
			// every C++ SetSkillIndex caller uses. Skip an unnamed entry
			// (never emitted by ReadSkill(), but a defensive skip costs
			// nothing); an empty hook renders as the bare name.
			std::string out;
			for( const AgentSkillEntry& e : skills.index ) {
				if( e.name.empty() ) continue;
				if( !out.empty() ) out += '\n';
				out += e.hook.empty() ? e.name : ( e.name + " -- " + e.hook );
			}
			return out;
		}

		//==============================================================
		// AgentImageCache -- the last-render frame, optionally shared by a
		// group of sessions.  Every method is a single leaf critical
		// section; see the header for the sharing contract.
		//==============================================================

		AgentImageCache::~AgentImageCache()
		{
			// No lock: a destructor runs when the LAST shared_ptr owner is
			// gone, so by definition nobody can still be calling in.  The
			// sink may outlive this release -- a reader that leased it holds
			// its own reference (AgentSession's lease bookkeeping addrefs
			// before dropping the lock), so this only drops OUR reference.
			safe_release( mSink );
			mSink = nullptr;
		}

		void AgentImageCache::Store( std::vector<unsigned char> png,
		                             InMemoryRasterizerOutput* sink )
		{
			std::lock_guard<std::mutex> lk( mMutex );
			mPng.swap( png );
			// Self-store must not release the reference it is about to keep.
			if( mSink != sink ) safe_release( mSink );
			mSink = sink;
		}

		std::vector<unsigned char> AgentImageCache::Png() const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			return mPng;
		}

		std::vector<unsigned char> AgentImageCache::PngWithDims( unsigned int& outWidth,
		                                                         unsigned int& outHeight ) const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			outWidth = 0;
			outHeight = 0;
			if( mSink ) {
				outWidth  = mSink->Width();
				outHeight = mSink->Height();
			}
			return mPng;
		}

		InMemoryRasterizerOutput* AgentImageCache::AcquireSink() const
		{
			std::lock_guard<std::mutex> lk( mMutex );
			if( mSink ) mSink->addref();
			return mSink;
		}

		void AgentImageCache::Take( std::vector<unsigned char>& outPng,
		                            InMemoryRasterizerOutput*& outSink )
		{
			std::lock_guard<std::mutex> lk( mMutex );
			outPng.swap( mPng );
			mPng.clear();
			outSink = mSink;
			mSink = nullptr;   // reference handed to the caller, not released
		}

		AgentSession::AgentSession( IJobPriv* job, bool owns, AgentAuthority authority,
		                            std::shared_ptr<AgentImageCache> sharedImageCache )
			: mJob( job ), mOwnsJob( owns ), mAuthority( authority ),
			  // No handle => a PRIVATE cache.  Isolation is the default; see
			  // AgentImageCache's doc for why that matters (the hosted
			  // loopback server's External session relies on it).
			  mImageCache( sharedImageCache ? sharedImageCache
			                                : std::make_shared<AgentImageCache>() )
		{
			// G2 (2026-08-10): SNAPSHOT the process-wide build-plan-gate
			// default here, once.  A session's posture is fixed for its whole
			// life -- a mid-session change of the process default (only a test
			// can do that; the launch flag is resolved before any session
			// exists) must never flip a running session's gate.
			mBuildPlanGateEnabled = BuildPlanGateDefaultEnabled();
			// S1 (2026-08-11): the staged build protocol's own switch, snapshot
			// at the same moment and for the same reason.  Both are consulted
			// together (BuildProtocolActive_), so a session that snapshotted
			// them one instant apart could not disagree with itself later.
			mBuildProtocolEnabled = BuildProtocolDefaultEnabled();
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
			// The cached sink now lives in the (possibly SHARED) cache.
			// AcquireSink takes the cache's leaf lock while we hold
			// mAsyncCacheMutex -- the documented outer->inner order -- and
			// hands back a reference, so the sink cannot be replaced out from
			// under GetPerceptionInfo().
			InMemoryRasterizerOutput* cached = mImageCache->AcquireSink();
			add( cached );
			for( const auto& lease : mActiveSinkReadLeases ) {
				if( lease.first != cached ) add( lease.first );
			}
			safe_release( cached );
			// SHARED-CACHE HONESTY: `mActiveSinkReadLeases` is this session's
			// leases only, so with a shared cache a SIBLING session's lease on
			// a SUPERSEDED sink is not counted here.  That under-reports a
			// peak-bytes DIAGNOSTIC by the size of that sidecar; it cannot
			// affect correctness or lifetime (the sibling holds its own
			// reference).  Counting it would mean the cache tracking every
			// session's leases, i.e. exactly the per-session lifetime coupling
			// mImageCache's doc says must not be shared.
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

			// Close ReadImage/ReadPerception admission, then wait for every
			// call that entered before teardown -- unchanged by the cache
			// move, and deliberately so: this protocol is about "is a call
			// still in flight ON THIS OBJECT", which is per-session no matter
			// where the pixels live.  A sibling session's readers are none of
			// our business and must not hold this destructor up.
			//
			// WHAT THE CACHE MOVE DID CHANGE: this used to end by releasing
			// mLastSink.  It no longer may -- the frame belongs to
			// mImageCache, which a sibling session may still be sharing.  The
			// handle is released with the rest of the members after this
			// body, and the LAST session out runs ~AgentImageCache, which
			// releases the sink exactly once.  A sink still leased by one of
			// OUR readers is safe either way: the lease holds its own
			// reference, and we do not get past the wait below until that
			// lease is gone anyway.
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
			// A LoadFromFile session owns its Job outright, so there is
			// nobody to share a last-render cache WITH -- always private.
			return std::unique_ptr<AgentSession>(
				new AgentSession( job, /*owns=*/true, authority,
				                  std::shared_ptr<AgentImageCache>() ) );
		}

		std::unique_ptr<AgentSession> AgentSession::WrapJob( IJobPriv* job, AgentAuthority authority,
		                                                     std::shared_ptr<AgentImageCache> sharedImageCache )
		{
			if( !job ) return nullptr;
			return std::unique_ptr<AgentSession>(
				new AgentSession( job, /*owns=*/false, authority, sharedImageCache ) );
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

		AgentSession::AgentDocumentSnapshot AgentSession::ReadDocumentSnapshot() const
		{
			AgentDocumentSnapshot snap;
			if( mController ) {
				mController->ReadAgentSceneSnapshot( snap.hasDocument, snap.document,
				                                     snap.headVersion );
				return snap;
			}
			// Headless: no controller means no render thread and no second
			// writer -- see the header for why these unlocked reads are the
			// correct answer here rather than a fallback.
			snap.hasDocument = HasDocument();
			snap.document    = ReadDocument();
			snap.headVersion = HeadVersion();
			return snap;
		}

		RISE::Cst::CstHeadVersion AgentSession::ReadHeadVersion() const
		{
			return mController ? mController->ReadAgentHeadVersion() : HeadVersion();
		}

		bool AgentSession::IsAnalysableRejection_( const AgentPatchResult& r )
		{
			return !r.applied && r.rawCode == 0 && !r.retriable && r.headVersion.uuid != 0;
		}

		bool AgentSession::IsAnalysableRejection_( const AgentChunkResult& r )
		{
			return !r.applied && r.rawCode == 0 && !r.retriable && r.headVersion.uuid != 0;
		}

		bool AgentSession::ReadHeadDocumentAt_( const RISE::Cst::CstHeadVersion& stamped,
		                                       RISE::Cst::Document& outDoc ) const
		{
			const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
			if( !snap.hasDocument || snap.headVersion != stamped ) return false;
			outDoc = RISE::Cst::ParseToCst( snap.document );
			return true;
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

		namespace
		{
			//! Forward declaration: AppendDesignDiagnostics_ is DEFINED further
			//! down this file (alongside ComputeDesignNoteConditionsFromDoc_ and
			//! ComputeDesignNoteFromDoc_ -- creative-richness P2.b,
			//! 73-creative-richness-design.md sec 9), but ValidateText (right
			//! below) needs to call it before that definition appears in file
			//! order.  Every `namespace { ... }` block at this scope in this
			//! translation unit reopens the SAME compiler-unique anonymous
			//! namespace (see AttachParamEditRejectionIssues's forward
			//! declaration further down for the identical pattern), so this and
			//! the later definition refer to the same symbol.
			void AppendDesignDiagnostics_( const Document& doc, std::vector<AgentDiagnostic>& out );

			//! Post-arc enforcement E1 (docs/agentic-redesign/75-expressive-surface-
			//! arc.md sec 7's LUMINAIRE_NULL_GEOMETRY entry; 76-...-log.md sec 3's
			//! mechanism law -- blocking facts act, a Warning gets skimmed): the
			//! SHARED classification predicate both the LUMINAIRE_NULL_GEOMETRY
			//! Warning (ValidateText's (b2) audit, just below) and the
			//! InsertChunk/ProposePatch creation gate key off -- an object binds
			//! an emissive material but owns no directly-owned geometry of its
			//! own (the csg_object class; see LuminaryManager::AddToLuminaryList,
			//! src/Library/Rendering/LuminaryManager.cpp).  ONE predicate, two
			//! consumers -- do not duplicate this test at either call site.
			bool IsNullGeometryEmitter_( const IObject& obj )
			{
				const IMaterial* pMat = obj.GetMaterial();
				return pMat && pMat->GetEmitter() && !obj.GetGeometry();
			}

			//! One post-derive finding: a named csg_object IsNullGeometryEmitter_
			//! flags, plus whether ITS OWN chunk in the source Document
			//! acknowledges the gap via `allow_non_sampling_emitter TRUE`.
			struct NullGeometryEmitterFinding
			{
				std::string name;
				bool        acknowledged = false;
			};

			//! Reads a Bool-kind param's raw value off a chunk NodeRef the same
			//! way ParseStateBag::GetBool does (RISE::String::toBoolean) -- `def`
			//! when the param is absent or value-less.  Same (pname,pvalue)
			//! token walk AnalyzeRejectedInsert / AnalyzeRejectedParamEdit use
			//! elsewhere in this file, just reading instead of collecting.
			bool ChunkParamBool_( const NodeRef& chunkItem, const std::string& pname, bool def )
			{
				if( !chunkItem ) return def;
				for( const NodeRef& kid : chunkItem->kids ) {
					if( !kid || kid->kind != NodeKind::Param ) continue;
					std::string thisName, val;
					for( const NodeRef& tk : kid->kids ) {
						if( !tk || tk->kind != NodeKind::Token ) continue;
						if( tk->role == "pname" ) thisName = tk->text;
						else if( tk->role == "pvalue" ) { if( !val.empty() ) val += ' '; val += tk->text; }
					}
					if( thisName == pname ) return val.empty() ? def : RISE::String( val.c_str() ).toBoolean();
				}
				return def;
			}

			//! Reads a String-kind param's raw value off a chunk NodeRef -- same
			//! walk as ChunkParamBool_, joined-token form.  "" when absent.
			std::string ChunkParamString_( const NodeRef& chunkItem, const std::string& pname )
			{
				if( !chunkItem ) return std::string();
				for( const NodeRef& kid : chunkItem->kids ) {
					if( !kid || kid->kind != NodeKind::Param ) continue;
					std::string thisName, val;
					for( const NodeRef& tk : kid->kids ) {
						if( !tk || tk->kind != NodeKind::Token ) continue;
						if( tk->role == "pname" ) thisName = tk->text;
						else if( tk->role == "pvalue" ) { if( !val.empty() ) val += ' '; val += tk->text; }
					}
					if( thisName == pname ) return val;
				}
				return std::string();
			}

			//! Every csg_object in `doc` that IsNullGeometryEmitter_ flags in
			//! `job`, each paired with whether its own chunk acknowledges the
			//! gap.  csg_object is the sole null-geometry Object class (see
			//! LuminaryManager::AddToLuminaryList), so restricting the walk to
			//! that keyword is exhaustive, not a heuristic.  `job` must already
			//! be a completed DeriveToJob of `doc` (or of a document that
			//! extends it while preserving every existing csg_object's
			//! registered name) -- this function does not derive anything
			//! itself.  Shared by ValidateText's post-derive Warning audit and
			//! the agent-edit creation gate (InsertChunk/ProposePatch) -- ONE
			//! emitter/geometry walk, not duplicated at either call site.
			std::vector<NullGeometryEmitterFinding> CollectNullGeometryEmitters_( IJobPriv& job, const Document& doc )
			{
				std::vector<NullGeometryEmitterFinding> out;
				IObjectManager* pObjMan = job.GetObjects();
				if( !pObjMan ) return out;

				std::vector<NodeRef> items;
				std::vector<std::size_t> starts;
				CollectItems( doc, items, starts );
				for( const NodeRef& it : items ) {
					if( !it || it->kind != NodeKind::Chunk || it->role != "csg_object" ) continue;
					const std::string name = ChunkParamString_( it, "name" );
					if( name.empty() ) continue;
					const IObjectPriv* pObj = pObjMan->GetItem( name.c_str() );
					if( !pObj || !IsNullGeometryEmitter_( *pObj ) ) continue;
					NullGeometryEmitterFinding f;
					f.name         = name;
					f.acknowledged = ChunkParamBool_( it, "allow_non_sampling_emitter", false );
					out.push_back( f );
				}
				return out;
			}

			//! Post-arc enforcement E1's creation gate, core derive step: given
			//! `candidateDoc` (the CANDIDATE state -- the current head with the
			//! touched edit already applied, NOT yet committed) and a list of
			//! csg_object names the CALLER has determined are worth checking
			//! (the touched csg_object itself, or every csg_object that
			//! references a touched MATERIAL), derives a throwaway Job
			//! (mirroring ValidateText's (b2) derive above) and reuses
			//! CollectNullGeometryEmitters_ -- the SAME classification/ack-
			//! lookup the Warning audit uses -- to return the SUBSET of
			//! `candidateNames` that come back as UNACKNOWLEDGED null-geometry
			//! emitters.  Empty when there is nothing to refuse: none of the
			//! candidates resolve to an emitter, all are acknowledged, or the
			//! candidate doesn't derive far enough to tell.  HONESTY: ambiguity
			//! is not proof, so an inconclusive derive fails OPEN -- the normal
			//! insert/patch machinery downstream still gets the last word (its
			//! own dangling-reference / derive-failure diagnostics fire as
			//! usual).
			std::vector<std::string> FindUnacknowledgedNullGeometryEmitters_( const Document& candidateDoc,
			                                                                  const std::vector<std::string>& candidateNames )
			{
				std::vector<std::string> out;
				if( candidateNames.empty() ) return out;

				IJobPriv* throwaway = nullptr;
				if( !RISE_CreateJobPriv( &throwaway ) || !throwaway ) return out;

				std::vector<std::string> diags;
				RISE::Cst::DeriveToJob( candidateDoc, *throwaway, &diags );

				for( const NullGeometryEmitterFinding& f : CollectNullGeometryEmitters_( *throwaway, candidateDoc ) ) {
					if( f.acknowledged ) continue;
					for( const std::string& n : candidateNames ) {
						if( n == f.name ) { out.push_back( f.name ); break; }
					}
				}

				throwaway->release();
				return out;
			}

			//! P1-1 fix round: the cheap, CST-ONLY (no derive) pre-filter for
			//! the creation gate's MATERIAL-side arm -- every csg_object chunk
			//! in `doc` whose `material` param equals `materialName`.  A
			//! material-chunk edit (e.g. `emissive` on ggx_material /
			//! pbr_metallic_roughness_material, `exitance` on
			//! lambertian_luminaire_material -- deliberately NOT enumerated by
			//! param name here; see the header doc's "whatever its name" note)
			//! can create the SAME refused construct the csg-side `material`
			//! re-point does, without ever touching the csg_object chunk.
			//! Empty here means "pay nothing further" -- the candidate-derive
			//! below is skipped entirely for the overwhelming majority of
			//! material edits, which are never bound to a csg_object at all.
			std::vector<std::string> CollectCsgObjectsReferencingMaterial_( const Document& doc,
			                                                                const std::string& materialName )
			{
				std::vector<std::string> out;
				if( materialName.empty() ) return out;
				std::vector<NodeRef> items;
				std::vector<std::size_t> starts;
				CollectItems( doc, items, starts );
				for( const NodeRef& it : items ) {
					if( !it || it->kind != NodeKind::Chunk || it->role != "csg_object" ) continue;
					if( ChunkParamString_( it, "material" ) != materialName ) continue;
					const std::string name = ChunkParamString_( it, "name" );
					if( !name.empty() ) out.push_back( name );
				}
				return out;
			}

			//! The actionable refusal clause for CREATING the construct --
			//! shared by every creation-gate arm (csg-side `material`
			//! re-point, material-side edit, insert), in the SAME VERIFIED
			//! phrasing ValidateText's (b2) Warning uses just below (P2a fix
			//! round: the prior gate text overclaimed "will never illuminate"
			//! / "only... direct camera view" -- LuminaryManager::
			//! AddToLuminaryList's verified contract, reproduced in that
			//! Warning's comment, is narrower: the gap is NEE light-sampling
			//! specifically, and BOTH direct-view AND a BSDF-sampled hit still
			//! contribute).  `csgNames` is the ACTUAL affected csg_object(s) --
			//! a single name for the csg-side/insert arms, POSSIBLY several for
			//! the material-side arm, so the fix reads actionably from either
			//! direction ("csg_object 'obj_x' ... has no directly-owned
			//! geometry" even when the edit under refusal is on the MATERIAL
			//! chunk, not `obj_x` itself).  Always called with a non-empty list.
			std::string DescribeUnacknowledgedNullGeometryEmitters_( const std::vector<std::string>& csgNames )
			{
				std::string named;
				for( std::size_t i = 0; i < csgNames.size(); ++i ) {
					if( i ) named += ( i + 1 == csgNames.size() ? " and " : ", " );
					named += "'" + csgNames[i] + "'";
				}
				const bool plural = csgNames.size() > 1;
				return "csg_object" + std::string( plural ? "s " : " " ) + named +
					std::string( plural ? " bind" : " binds" ) + " an emissive material but " +
					std::string( plural ? "have" : "has" ) + " no directly-owned geometry -- "
					"it will NOT act as an area light for next-event estimation (no NEE "
					"importance sampling, never selected by light-sampling); it still contributes "
					"emission on direct camera view (PT/BDPT/VCM pel + the legacy EmissionShaderOp "
					"chain) or a BSDF-sampled hit. Fix: back the emitter with a real-geometry object "
					"(a standard_object) instead, or add `allow_non_sampling_emitter TRUE` to the "
					"referencing csg_object" + std::string( plural ? "s" : "" ) +
					" to acknowledge the glow-only intent.";
			}

			//! P1-2 fix round: the refusal clause for REMOVING an
			//! acknowledgment (`allow_non_sampling_emitter TRUE` -> FALSE/
			//! absent) that would RECREATE the construct a prior insert/patch
			//! was already refused for (or that a scene-file load carried in
			//! already-acknowledged, silencing the Warning) -- a distinct
			//! message from DescribeUnacknowledgedNullGeometryEmitters_'s
			//! "creates" framing because the causal story is "you already
			//! disclosed this and are now un-disclosing it", not "you are
			//! introducing it for the first time".  Same verified NEE-vs-
			//! direct-view/BSDF-hit phrasing.
			std::string DescribeAcknowledgmentRemoval_( const std::string& csgName )
			{
				return "removing `allow_non_sampling_emitter` from csg_object '" + csgName + "' would "
					"RECREATE the construct insert_chunk/propose_patch already refuse elsewhere: an "
					"emissive material with no directly-owned geometry will NOT act as an area light "
					"for next-event estimation (no NEE importance sampling, never selected by "
					"light-sampling); it still contributes emission on direct camera view (PT/BDPT/VCM "
					"pel + the legacy EmissionShaderOp chain) or a BSDF-sampled hit. Fix: back the "
					"emitter with a real-geometry object (a standard_object) instead, or keep the "
					"acknowledgment flag TRUE.";
			}

			//----------------------------------------------------------------
			// R1c (2026-08-09): the agent rasterizer ALLOWLIST gate.  See
			// AgentRasterizerPolicy in AgentSession.h for the policy itself
			// and the derivation rules; this block holds the classification
			// primitives and the two document-level comparisons the public
			// gate functions below are built from.
			//
			// NO ESCAPE PARAMETER, deliberately.  E1 shipped
			// `allow_non_sampling_emitter` because that construct has a rare
			// but LEGITIMATE authoring intent (a glow-only csg) the agent can
			// honestly disclose.  This directive is CATEGORICAL -- "agents may
			// use PT and VCM only" -- so there is nothing for an agent to
			// disclose, and an escape param would be exactly the habituation
			// surface E1's stop rule watches for: a flag the model learns to
			// set reflexively, turning a refusal into a two-call formality.
			// The alternative is a HUMAN one and is named in every refusal
			// message: the user selects any rasterizer they like.
			//----------------------------------------------------------------

			//! The FOUR integrator kinds an agent may select.
			const char* const kAgentAllowedRasterizers_[] = {
				"pathtracing_pel_rasterizer",
				"pathtracing_spectral_rasterizer",
				"vcm_pel_rasterizer",
				"vcm_spectral_rasterizer",
			};

			//! DELIBERATELY UNGATED (supervisor decision): these are not
			//! integrator choices, and `pixelpel_rasterizer` is REQUIRED for
			//! alpha-mask scenes (docs/SCENE_CONVENTIONS.md).  The directive
			//! concerns which INTEGRATOR an agent reaches for.
			const char* const kAgentUngatedUtilityRasterizers_[] = {
				"pixelpel_rasterizer",
				"pixelintegratingspectral_rasterizer",
			};

			//! The rasterizer kinds this policy has EXPLICITLY considered and
			//! blocked.  Runtime blocking does NOT read this list -- anything
			//! that is a rasterizer and is in neither of the two sets above
			//! blocks (allowlist semantics).  This list exists ONLY so
			//! AgentRasterizerKindIsExplicitlyClassified can tell "blocked
			//! because we decided to" apart from "blocked because nobody has
			//! looked at it yet", which is what the coverage test asserts.
			const char* const kAgentBlockedRasterizers_[] = {
				"bdpt_pel_rasterizer",
				"bdpt_spectral_rasterizer",
				"mlt_rasterizer",
				"mlt_spectral_rasterizer",
				"auto_rasterizer",
				"auto_spectral_rasterizer",
			};

			//! Is `kw` a real rasterizer CHUNK?  Two conditions, both from the
			//! parser's own descriptor rather than any list here: the
			//! descriptor's category is ChunkCategory::Rasterizer, AND the
			//! keyword ends in `_rasterizer`.  The suffix test is load-bearing
			//! and not cosmetic -- `light_rr_threshold` is registered under
			//! ChunkCategory::Rasterizer but is a scalar knob, not a
			//! rasterizer.  This is the SAME predicate pair
			//! SceneEditController's entity-addressability switch uses ("a
			//! real rasterizer kind, NOT light_rr_threshold").
			bool IsRasterizerChunkKeyword_( const std::string& kw )
			{
				static const std::string suffix = "_rasterizer";
				if( kw.size() <= suffix.size() ) return false;
				if( kw.compare( kw.size() - suffix.size(), suffix.size(), suffix ) != 0 ) return false;
				const ChunkDescriptor* d = DescriptorForKeyword( String( kw.c_str() ) );
				return d && d->category == ChunkCategory::Rasterizer;
			}

			//! The rasterizer chunk keywords of `doc`, in DOCUMENT ORDER.  The
			//! LAST entry is the one that will be ACTIVE after a load: "A scene
			//! that declares two different rasterizer chunks ends up with both
			//! in the registry; the last-declared one wins for active"
			//! (IJob.h's rasterizer-registry contract).
			std::vector<std::string> CollectRasterizerKeywords_( const Document& doc )
			{
				std::vector<std::string> out;
				std::vector<NodeRef> items;
				std::vector<std::size_t> starts;
				CollectItems( doc, items, starts );
				for( const NodeRef& it : items ) {
					if( !it || it->kind != NodeKind::Chunk ) continue;
					if( IsRasterizerChunkKeyword_( it->role ) ) out.push_back( it->role );
				}
				return out;
			}

			//! Lower-cased, quote-stripped form of an `integrator` pin value,
			//! folded through the SAME synonym set the two auto_* chunk
			//! parsers accept (`pt` / `pathtracing` / `path_tracing` all mean
			//! PT).  Anything unrecognised comes back as-is.  R1c round-3
			//! FIX A (2026-08-09): the caller now tests membership in the
			//! full ALLOW set {auto,pt,vcm} (see IsAgentAllowedIntegratorPin_
			//! below), so an unrecognised/garbage string IS refused here too
			//! -- allowlist semantics, unclassified defaults to refused --
			//! even though the PARSER itself would silently treat it as
			//! `auto` (with a warning) at Finalize time.  A gate that let
			//! garbage slide through on the theory that the parser would
			//! sort it out later is not an allowlist.
			std::string NormalizeAutoIntegratorPin_( const std::string& raw )
			{
				std::string v = raw;
				// Strip surrounding quotes -- `integrator "vcm"` is accepted by
				// the parser, so the gate must see through the quoted form too.
				if( v.size() >= 2 && ( ( v[0] == '"' && v[v.size()-1] == '"' ) ||
				                       ( v[0] == '\'' && v[v.size()-1] == '\'' ) ) )
					v = v.substr( 1, v.size() - 2 );
				for( std::size_t i = 0; i < v.size(); ++i )
					v[i] = static_cast<char>( std::tolower( static_cast<unsigned char>( v[i] ) ) );
				if( v == "pathtracing" || v == "path_tracing" ) return "pt";
				return v;
			}
		}

		AgentRasterizerPolicy ClassifyAgentRasterizerKind( const std::string& keyword )
		{
			if( !IsRasterizerChunkKeyword_( keyword ) ) return AgentRasterizerPolicy::NotARasterizer;
			for( const char* k : kAgentAllowedRasterizers_ )
				if( keyword == k ) return AgentRasterizerPolicy::Allowed;
			for( const char* k : kAgentUngatedUtilityRasterizers_ )
				if( keyword == k ) return AgentRasterizerPolicy::UngatedUtility;
			// ALLOWLIST semantics: a rasterizer kind nobody has classified is
			// BLOCKED, not admitted.  See kAgentBlockedRasterizers_'s doc.
			return AgentRasterizerPolicy::Blocked;
		}

		bool AgentRasterizerKindIsExplicitlyClassified( const std::string& keyword )
		{
			for( const char* k : kAgentAllowedRasterizers_ )        if( keyword == k ) return true;
			for( const char* k : kAgentUngatedUtilityRasterizers_ ) if( keyword == k ) return true;
			for( const char* k : kAgentBlockedRasterizers_ )        if( keyword == k ) return true;
			return false;
		}

		namespace
		{
			//! The actionable refusal clause for a BLOCKED rasterizer kind.
			//! Names the rejected kind, names the allowed set EXPLICITLY, and
			//! states the alternative (the user selects it themselves) -- the
			//! same three obligations E1's refusal clauses carry.
			std::string DescribeBlockedRasterizer_( const std::string& rejected )
			{
				std::string allowed;
				const std::size_t n = sizeof( kAgentAllowedRasterizers_ ) / sizeof( kAgentAllowedRasterizers_[0] );
				for( std::size_t i = 0; i < n; ++i ) {
					if( i ) allowed += ( i + 1 == n ) ? " and " : ", ";
					allowed += "`";
					allowed += kAgentAllowedRasterizers_[i];
					allowed += "`";
				}
				return "`" + rejected + "` is a SPECIALIZED rasterizer that a scene-editing agent may not "
					"select. The only rasterizers this surface may select are " + allowed + " -- PT for "
					"general scenes, VCM for caustic / refractive / dispersive transport. There is NO "
					"override parameter on this refusal: if the scene genuinely needs `" + rejected + "`, "
					"the USER selects it themselves (the GUI's rasterizer accordion, or by authoring the "
					"chunk into the scene file), and the agent then keeps editing the scene around it "
					"normally. The non-integrator utility rasterizers `pixelpel_rasterizer` and "
					"`pixelintegratingspectral_rasterizer` are NOT gated -- `pixelpel_rasterizer` is "
					"required for alpha-mask scenes.";
			}

			//! R1c round-3 FIX A (2026-08-09): the FULL accepted value domain
			//! of the `integrator` enum on auto_rasterizer /
			//! auto_spectral_rasterizer, per both chunks' descriptor in
			//! ChunkParserRegistry.cpp (`p.enumValues =
			//! {"auto","pt","bdpt","vcm"}`, identical on the two chunks --
			//! verified by reading the descriptor, not guessed).  ALLOWLIST
			//! semantics, mirroring kAgentAllowedRasterizers_ /
			//! kAgentBlockedRasterizers_ above: the dispatcher's own choice
			//! (`auto`) and the two agent-selectable integrators (`pt`,
			//! `vcm`) are explicitly allowed; `bdpt` is explicitly blocked;
			//! and a value the descriptor adds later that NEITHER list names
			//! falls through to REFUSED (see IsAgentAllowedIntegratorPin_
			//! below) -- a new accepted value defaults to refused, not
			//! silently admitted.
			const char* const kAgentAllowedIntegratorPins_[] = { "auto", "pt", "vcm" };

			//! The pin values this policy has EXPLICITLY considered and
			//! blocked -- exists ONLY so
			//! AgentIntegratorPinIsExplicitlyClassified can tell "blocked
			//! because we decided to" apart from "blocked because nobody has
			//! looked at it yet" (same purpose as kAgentBlockedRasterizers_
			//! above).
			const char* const kAgentBlockedIntegratorPins_[] = { "bdpt" };

			//! TRUE iff `pin` (already run through
			//! NormalizeAutoIntegratorPin_) is in the explicit ALLOW set.
			//! Anything else -- `bdpt`, a future descriptor addition this
			//! policy hasn't named yet, or outright garbage -- is refused.
			//! STATELESS: does not consult the head's current value (see
			//! CheckRasterizerAllowlistGateForPatch's arm (a) for why the
			//! prior state-comparing form was a TOCTOU hazard).
			bool IsAgentAllowedIntegratorPin_( const std::string& pin )
			{
				for( const char* k : kAgentAllowedIntegratorPins_ ) if( pin == k ) return true;
				return false;
			}

			//! The refusal clause for arm (a): pinning an auto_* rasterizer's
			//! `integrator` to a non-allowed value.  A distinct message from
			//! DescribeBlockedRasterizer_ because the causal story is "you
			//! selected an integrator through a chunk that was already
			//! here", not "you tried to add a chunk".
			std::string DescribeBlockedIntegratorPin_( const std::string& autoKeyword, const std::string& rejectedPin )
			{
				return "pinning `" + autoKeyword + "`'s `integrator` to `" + rejectedPin + "` is refused. "
					"On this chunk the agent-selectable pins are `pt` (path tracing), `vcm` (caustic / "
					"refractive / dispersive transport), and `auto` (leaves the dispatcher's own choice in "
					"place) -- EVERY other value, including `bdpt`, is refused UNCONDITIONALLY, regardless "
					"of what the chunk is already pinned to (a stateless allowlist, not a delta against the "
					"chunk's current value). There is NO override parameter on this refusal -- the USER pins "
					"the rejected value themselves (the GUI, or by authoring the pin into the scene file). "
					"Every OTHER parameter on this chunk remains freely editable.";
			}

			//! Arm (b): compare the candidate document against the head by
			//! rasterizer-keyword MULTISET and by ACTIVE rasterizer, and return
			//! the refusal clause for the first NEWLY introduced / NEWLY
			//! activated blocked kind.  "" when the delta introduces none.
			//!
			//! Two independent conditions, both needed:
			//!   * a blocked keyword whose COUNT goes up -- covers an added
			//!     chunk however it got there (a value-splice injection, a
			//!     wholesale chunk-text rewrite), including adding a SECOND
			//!     copy to a scene that already had one;
			//!   * the ACTIVE (last-declared) rasterizer flipping from
			//!     non-blocked to blocked WITHOUT any count change -- covers a
			//!     reordering/removal that re-activates a pre-existing blocked
			//!     chunk.  No agent verb can do that today (rasterizer chunks
			//!     declare no `name`, so remove_chunk/remove_chunks cannot
			//!     resolve one and the unique-in-kind positional fallback fires
			//!     only for `camera` and unnamedRepeatable kinds -- neither of
			//!     which a rasterizer is), but the condition costs one string
			//!     compare and closes the class rather than the instance.
			std::string DescribeNewlyBlockedRasterizerDelta_( const Document& headDoc, const Document& candidateDoc )
			{
				const std::vector<std::string> headKw = CollectRasterizerKeywords_( headDoc );
				const std::vector<std::string> candKw = CollectRasterizerKeywords_( candidateDoc );

				std::map<std::string,int> headCounts;
				for( const std::string& k : headKw ) ++headCounts[k];

				std::map<std::string,int> candCounts;
				for( const std::string& k : candKw ) ++candCounts[k];

				for( const std::pair<const std::string,int>& kv : candCounts ) {
					if( ClassifyAgentRasterizerKind( kv.first ) != AgentRasterizerPolicy::Blocked ) continue;
					const std::map<std::string,int>::const_iterator h = headCounts.find( kv.first );
					const int before = ( h == headCounts.end() ) ? 0 : h->second;
					if( kv.second > before ) return DescribeBlockedRasterizer_( kv.first );
				}

				const std::string headActive = headKw.empty() ? std::string() : headKw.back();
				const std::string candActive = candKw.empty() ? std::string() : candKw.back();
				if( !candActive.empty() && candActive != headActive &&
				    ClassifyAgentRasterizerKind( candActive ) == AgentRasterizerPolicy::Blocked &&
				    ClassifyAgentRasterizerKind( headActive ) != AgentRasterizerPolicy::Blocked )
					return DescribeBlockedRasterizer_( candActive );

				return std::string();
			}

			//! G2 fix-round (2026-08-10): resolve the chunk a PARAM EDIT
			//! addresses, exactly the way Job::ApplyCstParamEditImpl_ will --
			//! a named target by name, or (EMPTY name + non-empty kind) the
			//! sole chunk of that kind, the KIND-ADDRESSED SINGLETON form the
			//! agent surface uses for the unnamed film / rasterizer chunks.
			//! Null when the patch addresses nothing.
			//!
			//! Factored out of CheckRasterizerAllowlistGateForPatch so R1c's
			//! arm (b) and the G2 geometry-delta arm below resolve the target
			//! through ONE definition: two document-delta gates that disagreed
			//! about which chunk an edit lands on would be a bypass by
			//! construction.
			RISE::Cst::NodeId ResolvePatchTargetChunk_( const Document& headDoc,
			                                            const std::string& target,
			                                            const std::string& kind )
			{
				if( target.empty() && kind.empty() ) return RISE::Cst::NodeId();
				const bool uniqueFallback = ( target.empty() && !kind.empty() );
				return RISE::Cst::DocFindByNameAnyRole( headDoc, target, nullptr, kind, uniqueFallback );
			}

			//! G2 fix-round (2026-08-10): build the document a param edit
			//! WOULD PRODUCE, ROUND-TRIPPED THROUGH BYTES.
			//!
			//! The round-trip is load-bearing, not defensive, and the reason is
			//! the same one R1c's arm (b) documents: DocSetOrAddParamValue
			//! writes `value` VERBATIM into the target param's value token, so
			//! the in-memory candidate still carries exactly the chunks the head
			//! carried, no matter what the value string contains.  It is the
			//! SERIALIZED bytes -- the head an agent commits, and the bytes any
			//! later load re-parses -- where a value carrying `}` followed by a
			//! whole `box_geometry { ... }` block becomes a real second chunk.
			//! So any gate that asks "what would this edit ADD to the document"
			//! must ask it of ParseToCst(SerializeCst(candidate)), i.e. of what
			//! the document WILL MEAN, not of the node tree the edit
			//! mechanically produced.
			//!
			//! Extracted so R1c's rasterizer delta and G2's geometry delta share
			//! ONE definition of "the candidate" (see ResolvePatchTargetChunk_).
			Document BuildPatchCandidateAsBytes_( const Document& headDoc,
			                                      const RISE::Cst::NodeId& id,
			                                      const std::string& param,
			                                      const std::string& value )
			{
				const Document candidate = RISE::Cst::DocSetOrAddParamValue( headDoc, id, param, 0, value );
				return RISE::Cst::ParseToCst( RISE::Cst::SerializeCst( candidate ) );
			}

			//! G2 fix-round (2026-08-10): every top-level chunk in `doc` whose
			//! REGISTRY descriptor is ChunkCategory::Geometry, as (keyword,
			//! bare `name` param) pairs in document order.  The registry IS the
			//! classifier -- not a `_geometry` suffix match -- so a geometry
			//! kind added to the registry later is covered with no edit here,
			//! the same rule AgentSession::ChunkTextCreatesGeometry_ applies to
			//! the insert verbs.
			std::vector<std::pair<std::string, std::string> > CollectGeometryChunks_( const Document& doc )
			{
				std::vector<std::pair<std::string, std::string> > out;
				const int n = RISE::Cst::DocItemCount( doc );
				for( int i = 0; i < n; ++i ) {
					const NodeRef it =
						RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
					if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
					const ChunkDescriptor* d = DescriptorForKeyword( String( it->role.c_str() ) );
					if( d && d->category == ChunkCategory::Geometry )
						out.push_back( std::make_pair( it->role, ChunkParamString_( it, "name" ) ) );
				}
				return out;
			}
		}

		//! R1c round-3 FIX A (2026-08-09).  TRUE iff `pin` (already run
		//! through NormalizeAutoIntegratorPin_) appears in one of the TWO
		//! hand-maintained integrator-pin policy sets (allowed / known-
		//! blocked) above.  A pin value the descriptor accepts that this
		//! policy never named comes back FALSE -- it still REFUSES at
		//! runtime (allowlist semantics, IsAgentAllowedIntegratorPin_), but
		//! a coverage test can fail so the omission is a decision someone
		//! makes deliberately, mirroring
		//! AgentRasterizerKindIsExplicitlyClassified above.
		bool AgentIntegratorPinIsExplicitlyClassified( const std::string& pin )
		{
			for( const char* k : kAgentAllowedIntegratorPins_ ) if( pin == k ) return true;
			for( const char* k : kAgentBlockedIntegratorPins_ ) if( pin == k ) return true;
			return false;
		}

		//! Post-arc enforcement E1's creation gate, patch arm -- see the
		//! declaration in AgentSession.h for the full contract (three
		//! triggers: csg-side `material` re-point, `allow_non_sampling_emitter`
		//! removal, material-side edit).  A FREE function (external linkage,
		//! declared in the header, OUTSIDE the anonymous namespace above) so
		//! SceneEditController::ResolveProposal's stale-staged-proposal
		//! re-check can call the identical logic AgentSession::ProposePatch
		//! uses below, from a different translation unit, with zero
		//! duplication of the resolve/derive/classify walk.
		std::string CheckNonSamplingEmitterGateForPatch( const std::string& headText,
		                                                 const std::string& target,
		                                                 const std::string& kind,
		                                                 const std::string& param,
		                                                 const std::string& value )
		{
			if( target.empty() ) return std::string();

			const RISE::Cst::Document headDoc = RISE::Cst::ParseToCst( headText );
			const bool uniqueFallback = ( kind == "camera" );
			const RISE::Cst::NodeId id =
				RISE::Cst::DocFindByNameAnyRole( headDoc, target, nullptr, kind, uniqueFallback );
			if( !id ) return std::string();
			const RISE::Cst::NodeRef chunkItem = RISE::Cst::DocResolveNodeId( headDoc, id );
			if( !chunkItem ) return std::string();

			// Arm A/B: the touched chunk IS the csg_object -- re-pointing
			// `material` (the original vehicle) or clearing
			// `allow_non_sampling_emitter` (P1-2's ack-removal bypass).  Both
			// reduce to the identical mechanical check (build the candidate
			// with the edit applied, ask whether the csg's OWN name comes
			// back unacknowledged); only the REFUSAL MESSAGE differs.
			if( chunkItem->role == "csg_object" &&
			    ( param == "material" || param == "allow_non_sampling_emitter" ) ) {
				const std::string touchedName = ChunkParamString_( chunkItem, "name" );
				if( touchedName.empty() ) return std::string();
				const RISE::Cst::Document candidate =
					RISE::Cst::DocSetOrAddParamValue( headDoc, id, param, 0, value );
				const std::vector<std::string> hits =
					FindUnacknowledgedNullGeometryEmitters_( candidate, std::vector<std::string>( 1, touchedName ) );
				if( hits.empty() ) return std::string();
				return param == "allow_non_sampling_emitter"
					? DescribeAcknowledgmentRemoval_( touchedName )
					: DescribeUnacknowledgedNullGeometryEmitters_( hits );
			}

			// Arm C (P1-1): the touched chunk is a MATERIAL, not the
			// csg_object.  Cheap CST-only pre-filter FIRST -- pay for the
			// candidate-derive only when at least one csg_object in the
			// CURRENT head actually references this material by name; the
			// vast majority of material edits (nothing bound to a csg) skip
			// the derive entirely.
			const ChunkDescriptor* desc = DescriptorForKeyword( String( chunkItem->role.c_str() ) );
			if( !desc || desc->category != ChunkCategory::Material ) return std::string();
			const std::string materialName = ChunkParamString_( chunkItem, "name" );
			if( materialName.empty() ) return std::string();
			const std::vector<std::string> referencing =
				CollectCsgObjectsReferencingMaterial_( headDoc, materialName );
			if( referencing.empty() ) return std::string();

			// DELTA, not state (round-2 fix): unlike Arm A/B -- which edit
			// the EXACT field that determines a csg's emissive/acknowledged
			// status, so "does the candidate come back unacknowledged" IS
			// the right question -- Arm C's target is a MATERIAL, and
			// `param` need not have anything to do with emission at all
			// (alphax, roughness, ...).  A referencing csg can ALREADY be
			// an unacknowledged null-geometry emitter on the CURRENT head
			// (a pre-existing, scene-file-loaded construct Validate is
			// already Warning about) -- that is NOT this edit's doing, and
			// refusing an unrelated param edit on that basis would freeze
			// every future edit to the material, contradicting the CREATE-
			// or-RECREATE contract this whole gate exists to enforce (and
			// the scene's correct posture: Warning nags, edits proceed).
			// Compute the SAME finder on the CURRENT head (restricted to
			// the same `referencing` set) and refuse ONLY names that are
			// NEW in the candidate -- i.e. THIS edit created or worsened
			// their unacknowledged status.  One extra head-derive, paid
			// only inside this already-narrow (referencing non-empty) arm.
			const RISE::Cst::Document candidate = RISE::Cst::DocSetOrAddParamValue( headDoc, id, param, 0, value );
			const std::vector<std::string> candidateHits = FindUnacknowledgedNullGeometryEmitters_( candidate, referencing );
			if( candidateHits.empty() ) return std::string();
			const std::vector<std::string> headHits = FindUnacknowledgedNullGeometryEmitters_( headDoc, referencing );
			std::vector<std::string> createdHits;
			for( const std::string& n : candidateHits ) {
				bool preExisting = false;
				for( const std::string& h : headHits ) if( h == n ) { preExisting = true; break; }
				if( !preExisting ) createdHits.push_back( n );
			}
			if( createdHits.empty() ) return std::string();
			return DescribeUnacknowledgedNullGeometryEmitters_( createdHits );
		}

		//! Post-arc enforcement E1's creation gate, insert arm -- see the
		//! declaration in AgentSession.h.  Free function for the same
		//! cross-TU reason as the patch arm above.
		std::string CheckNonSamplingEmitterGateForInsert( const std::string& headText, const std::string& chunkText )
		{
			RISE::Cst::Document chunkDoc = RISE::Cst::ParseToCst( chunkText );
			RISE::Cst::NodeRef chunkItem;
			{
				const int n = RISE::Cst::DocItemCount( chunkDoc );
				for( int i = 0; i < n; ++i ) {
					const RISE::Cst::NodeRef it =
						RISE::Cst::DocResolveNodeId( chunkDoc, RISE::Cst::DocNodeIdAt( chunkDoc, i ) );
					if( it && it->kind == RISE::Cst::NodeKind::Chunk ) { chunkItem = it; break; }
				}
			}
			if( !chunkItem || chunkItem->role != "csg_object" ) return std::string();

			// P3: skip the candidate-derive entirely when the inserted chunk
			// ALREADY carries the acknowledgment -- cheap (a single param
			// read on the not-yet-merged candidate chunk), and no false
			// negative is possible: an already-acknowledged insert can never
			// be refused regardless of what the rest of the document says.
			if( ChunkParamBool_( chunkItem, "allow_non_sampling_emitter", false ) ) return std::string();

			const std::string touchedName = ChunkParamString_( chunkItem, "name" );
			if( touchedName.empty() ) return std::string();

			RISE::Cst::Document candidate = RISE::Cst::ParseToCst( headText );
			const int endAt = RISE::Cst::DocItemCount( candidate );
			candidate = RISE::Cst::DocInsertItem( candidate, endAt, chunkItem );

			const std::vector<std::string> hits =
				FindUnacknowledgedNullGeometryEmitters_( candidate, std::vector<std::string>( 1, touchedName ) );
			if( hits.empty() ) return std::string();
			return DescribeUnacknowledgedNullGeometryEmitters_( hits );
		}

		//! R1c (2026-08-09) -- see the declaration in AgentSession.h for the
		//! contract.  A FREE function (external linkage) for the same
		//! cross-TU reason E1's pair are: SceneEditController::ResolveProposal
		//! re-runs the IDENTICAL check at approval time, so a proposal staged
		//! before the head moved cannot slip a blocked rasterizer through on
		//! approval (the E1 re-gate lesson).
		//!
		//! Cost: ONE CST parse of the (tiny) candidate chunk text plus one
		//! descriptor lookup per top-level chunk.  No derive, no throwaway
		//! Job -- unlike E1, this policy is answerable from the keyword alone.
		std::string CheckRasterizerAllowlistGateForInsert( const std::string& chunkText )
		{
			const RISE::Cst::Document chunkDoc = RISE::Cst::ParseToCst( chunkText );
			const int n = RISE::Cst::DocItemCount( chunkDoc );
			for( int i = 0; i < n; ++i )
			{
				const RISE::Cst::NodeRef it =
					RISE::Cst::DocResolveNodeId( chunkDoc, RISE::Cst::DocNodeIdAt( chunkDoc, i ) );
				if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
				// EVERY top-level chunk, not just the first: insert_chunk's own
				// one-chunk-per-call rule is enforced downstream, and a gate
				// that trusted it would be a gate with a bypass.
				if( ClassifyAgentRasterizerKind( it->role ) == AgentRasterizerPolicy::Blocked )
					return DescribeBlockedRasterizer_( it->role );
			}
			return std::string();
		}

		//! R1c -- see the declaration in AgentSession.h.  Free function for the
		//! same cross-TU reason as the insert arm above.
		std::string CheckRasterizerAllowlistGateForPatch( const std::string& headText,
		                                                  const std::string& target,
		                                                  const std::string& kind,
		                                                  const std::string& param,
		                                                  const std::string& value )
		{
			// A patch with neither a target nor a kind addresses nothing; the
			// normal machinery rejects it and there is no candidate to build.
			if( target.empty() && kind.empty() ) return std::string();
			if( param.empty() ) return std::string();

			const RISE::Cst::Document headDoc = RISE::Cst::ParseToCst( headText );

			// Resolution mirrors Job::ApplyCstParamEditImpl_'s kind-addressed
			// SINGLETON rule -- an EMPTY name with a non-empty kind resolves
			// the sole chunk of that kind.  That rule is EXACTLY how the agent
			// surface addresses the unnamed film / rasterizer chunks (rasterizer
			// chunks declare no `name` param at all), so a gate that only
			// handled named targets -- as E1's patch arm does, correctly, since
			// a csg_object is always named -- would miss every rasterizer param
			// edit.  The camera-specific DocCameraUniqueFallbackPermitted gate
			// is deliberately NOT reproduced here: a camera is never a
			// rasterizer, so the two rules can only differ on chunks this gate
			// has nothing to say about.
			// G2 fix-round (2026-08-10): resolution moved into the SHARED
			// ResolvePatchTargetChunk_ so this gate and the G2 geometry-delta
			// gate below cannot drift on which chunk an edit lands on.  Same
			// rule as before, byte-for-byte.
			const RISE::Cst::NodeId id = ResolvePatchTargetChunk_( headDoc, target, kind );
			if( !id ) return std::string();
			const RISE::Cst::NodeRef chunkItem = RISE::Cst::DocResolveNodeId( headDoc, id );
			if( !chunkItem ) return std::string();

			// ---- Arm (a): the auto_* dispatcher's `integrator` pin ----------
			// STATELESS ALLOWLIST (R1c round-3 FIX A, 2026-08-09): refuse
			// any value outside {auto,pt,vcm} UNCONDITIONALLY -- no
			// comparison against the head's current value.  The prior form
			// read the head's EXISTING value and skipped the refusal when
			// the head was already pinned to `bdpt` (treating the write as
			// an inert no-op).  That state comparison is a TOCTOU hazard:
			// when the caller omits baseHeadVersion, a co-editor can move
			// the pin OFF `bdpt` between this gate check and
			// ApplyAgentParamEdit's commit, so the "inert" write lands and
			// actually RE-SELECTS BDPT.  Refusing a genuine no-op costs the
			// agent nothing (the value is already what it would write); the
			// stateless form is race-free by construction and keeps policy
			// at this AgentSession choke point rather than pushed into the
			// controller's critical section.
			if( param == "integrator" &&
			    ( chunkItem->role == "auto_rasterizer" || chunkItem->role == "auto_spectral_rasterizer" ) )
			{
				const std::string want = NormalizeAutoIntegratorPin_( value );
				if( !IsAgentAllowedIntegratorPin_( want ) )
					return DescribeBlockedIntegratorPin_( chunkItem->role, want );
			}

			// ---- Arm (b): the document-level delta --------------------------
			// A param VALUE is spliced into the document as TEXT, so the only
			// honest way to ask "did this edit add a rasterizer chunk" is to
			// build the candidate and look.  This is what makes the arm
			// generic: it does not have to recognise any particular injection
			// shape, only the resulting document.
			//
			// ROUND-TRIP, and this is load-bearing, not defensive.
			// DocSetOrAddParamValue writes `value` VERBATIM into the target
			// param's value token: the in-memory candidate still has exactly
			// the chunks the head had, no matter what the value string
			// contains.  It is the SERIALIZED bytes -- the head an agent
			// commits, and the bytes any later load re-parses -- where a value
			// carrying `}` + a whole `mlt_rasterizer { ... }` block becomes a
			// real second chunk.  So the comparison must be made against
			// ParseToCst(SerializeCst(candidate)), i.e. against what the
			// document WILL MEAN, not against the node tree the edit
			// mechanically produced.  (The derive layer independently rejects
			// most such splices -- a Double slot refuses a non-numeric token
			// -- but "most" is not a gate.)
			//
			// G2 fix-round (2026-08-10): the construction itself moved into the
			// SHARED BuildPatchCandidateAsBytes_ (which carries the round-trip
			// rationale above in full) so the G2 geometry-delta gate below
			// judges the SAME candidate this one does.
			return DescribeNewlyBlockedRasterizerDelta_(
				headDoc, BuildPatchCandidateAsBytes_( headDoc, id, param, value ) );
		}

		//! G2 fix-round (2026-08-10) -- see the declaration in AgentSession.h.
		//! A FREE function for the same cross-TU reason E1's and R1c's patch
		//! arms are: SceneEditController::ResolveProposal's stale-staged-
		//! proposal re-check calls the IDENTICAL delta AgentSession::
		//! ProposePatch's arm uses, with zero duplication.
		std::string DescribeBuildPlanGeometryDeltaForPatch( const std::string& headText,
		                                                   const std::string& target,
		                                                   const std::string& kind,
		                                                   const std::string& param,
		                                                   const std::string& value,
		                                                   std::string* outName )
		{
			// Same two cheap pre-filters R1c's patch arm applies: a patch that
			// addresses nothing, or names no param, produces no candidate.
			if( target.empty() && kind.empty() ) return std::string();
			if( param.empty() ) return std::string();

			const Document headDoc = RISE::Cst::ParseToCst( headText );
			const RISE::Cst::NodeId id = ResolvePatchTargetChunk_( headDoc, target, kind );
			if( !id ) return std::string();
			if( !RISE::Cst::DocResolveNodeId( headDoc, id ) ) return std::string();

			const Document candidateAsBytes = BuildPatchCandidateAsBytes_( headDoc, id, param, value );

			// DELTA, not state -- the E1 lesson, and the reason a patch that
			// merely edits an EXISTING geometry chunk's params (a sphere's
			// `radius`, a box's `width`) can never trip this gate: the
			// per-keyword geometry MULTISET is identical before and after, so
			// there is nothing NEWLY introduced to refuse.  Only a keyword
			// whose COUNT GOES UP counts -- which covers a value-splice
			// injection however it is shaped, including splicing a SECOND
			// chunk of a kind the scene already had.  Keyword multiset rather
			// than name set, matching DescribeNewlyBlockedRasterizerDelta_
			// exactly, so a RENAME of an existing geometry chunk is also
			// correctly not-a-creation.
			const std::vector<std::pair<std::string, std::string> > headGeom = CollectGeometryChunks_( headDoc );
			const std::vector<std::pair<std::string, std::string> > candGeom = CollectGeometryChunks_( candidateAsBytes );

			std::map<std::string, int> headCounts;
			for( std::size_t i = 0; i < headGeom.size(); ++i ) ++headCounts[headGeom[i].first];
			std::map<std::string, int> candCounts;
			for( std::size_t i = 0; i < candGeom.size(); ++i ) ++candCounts[candGeom[i].first];

			for( std::size_t i = 0; i < candGeom.size(); ++i )
			{
				const std::string& kw = candGeom[i].first;
				const std::map<std::string, int>::const_iterator h = headCounts.find( kw );
				const int before = ( h == headCounts.end() ) ? 0 : h->second;
				if( candCounts[kw] <= before ) continue;
				// This KIND gained a chunk.  For the identity echo prefer the
				// first candidate chunk of that kind whose `name` is not on the
				// head (the newly spliced one); fall back to this one's name
				// when every name already existed (a duplicate-name splice).
				if( outName ) {
					std::string pick = candGeom[i].second;
					for( std::size_t j = 0; j < candGeom.size(); ++j ) {
						if( candGeom[j].first != kw ) continue;
						bool onHead = false;
						for( std::size_t k = 0; k < headGeom.size() && !onHead; ++k )
							onHead = ( headGeom[k].first == kw && headGeom[k].second == candGeom[j].second );
						if( !onHead ) { pick = candGeom[j].second; break; }
					}
					*outName = pick;
				}
				return kw;
			}
			return std::string();
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

			// (a2) NO CHUNKS AT ALL -> EMPTY_DOCUMENT, before the derive.
			// "", "   \n", and a comments-only text all parse and round-trip
			// perfectly and derive with ZERO diagnostics, so the derive layer
			// below would call them clean.  A text that declares no scene
			// object is not a clean scene, and saying otherwise is exactly the
			// dishonesty this surface refuses everywhere else.  Degenerate
			// inputs are therefore ONE answer, not three: emptiness is judged
			// on chunk COUNT, not on byte length.
			bool anyChunk = false;
			{
				std::vector<NodeRef> items;
				std::vector<std::size_t> starts;
				CollectItems( candidateDoc, items, starts );
				for( std::size_t i = 0; i < items.size() && !anyChunk; ++i )
					anyChunk = items[i] && items[i]->kind == NodeKind::Chunk;
			}
			if( !anyChunk ) {
				AgentDiagnostic d;
				d.severity = AgentDiagnostic::Severity::Error;
				d.code     = AgentDiagnosticCode::EMPTY_DOCUMENT;
				d.message  = "the text declares no scene chunks at all -- an empty or "
				             "comments-only document is not a valid scene";
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

			// (b2) Crash-fix sibling (see LuminaryManager::AddToLuminaryList,
			// src/Library/Rendering/LuminaryManager.cpp): a luminaire-bound
			// object with no directly-owned geometry (a csg_object, whose shape
			// comes from its two operand objects rather than a single geometry
			// chunk) is silently skipped by NEE at render time -- refused, not
			// dereferenced, so it no longer crashes, but a model authoring the
			// scene should still be told its light won't act as an area light.
			// Walk the throwaway job's realized objects (post-derive, so
			// geometry/material bindings are fully resolved) BEFORE releasing
			// it and surface one Warning diagnostic summarizing every match.
			{
				// Post-arc enforcement E1: the walk itself is shared
				// (CollectNullGeometryEmitters_, defined above) with the
				// InsertChunk/ProposePatch creation gate -- this audit now
				// only WARNS on UNACKNOWLEDGED bindings.  An object whose
				// csg_object chunk carries `allow_non_sampling_emitter TRUE`
				// has already told the reader its glow-only intent is
				// deliberate; repeating the same Warning on every subsequent
				// Validate call is the nag-loop anti-pattern the two-tier
				// design (75-arc sec 7 / 76-log sec 3) exists to avoid, and
				// it would fail eval's `diagnostics: clean` on a scene that
				// deliberately, honestly acknowledged the gap.
				unsigned int unacknowledgedCount = 0;
				for( const NullGeometryEmitterFinding& f : CollectNullGeometryEmitters_( *throwaway, candidateDoc ) )
					if( !f.acknowledged ) ++unacknowledgedCount;
				if( unacknowledgedCount > 0 ) {
					AgentDiagnostic d;
					d.severity = AgentDiagnostic::Severity::Warning;
					d.code     = AgentDiagnosticCode::LUMINAIRE_NULL_GEOMETRY;
					// TRUTH-DEFECT FIX (2026-07-31 fix round 2, scoped precisely fix
					// round 3 / P2b -- see LuminaryManager.cpp's AddToLuminaryList for
					// the full "exactly what is verified" breakdown, reproduced in
					// scope here): "contributes emission" is VERIFIED (not merely
					// asserted) for DIRECT camera view under pathtracing_pel_rasterizer,
					// bdpt_pel_rasterizer, vcm_pel_rasterizer, AND the legacy
					// EmissionShaderOp (DefaultEmission) shaderop chain under
					// pixelpel_rasterizer -- all four in
					// tests/CSGNullGeometryLuminaireCrashTest.cpp.  For an INDIRECT
					// (BSDF-sampled) hit, only PathTracingIntegrator.cpp's bsdfPdf>0
					// block is independently tested; EmissionShaderOp.cpp's identically-
					// gated bsdfPdf>0 block is code-verified but untested.  Spectral/HWSS
					// twins of every path above are code-identical fixes, untested.  BDPT
					// additionally carries a PRE-EXISTING, unrelated MIS energy deficit
					// on this class of emitter (see BDPTIntegrator.cpp's eye-walk
					// comment) -- "contributes emission" holds for BDPT, "full weight"
					// does not.
					d.message  = std::to_string( unacknowledgedCount ) +
						" object(s) bind an emissive material but have no directly-owned "
						"geometry (e.g. a csg_object, whose geometry comes from its two "
						"operand objects rather than a single geometry chunk) -- they will "
						"NOT act as an area light for next-event estimation (no NEE "
						"importance sampling, never selected by light-sampling); each still "
						"contributes emission on direct camera view (PT/BDPT/VCM pel + the "
						"legacy EmissionShaderOp chain) or a BSDF-sampled hit. Bind the "
						"emissive material to a standard_object with real geometry instead, "
						"or add `allow_non_sampling_emitter TRUE` to acknowledge the glow-only "
						"intent and silence this warning.";
					out.push_back( d );
				}
			}

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

			// (d) Creative-richness P2.b (73-creative-richness-design.md sec
			// 9): the SAME two design-note conditions the render-result note
			// computes (AppendDesignDiagnostics_ / ComputeDesignNoteFromDoc_
			// share ONE conditions scan, ComputeDesignNoteConditionsFromDoc_,
			// so the thresholds can never drift apart) -- surfaced here as
			// Info-severity diagnostics, the exact list a model consults when
			// it calls validate and reads "no errors" (sec 9's qwen
			// transcript: "validated clean - no errors ... just a design
			// suggestion").  validate-ONLY: the render-result note path is
			// untouched (see 73-creative-richness-design.md sec 9's closing
			// recommendation -- "keep the render-result note as-is").
			// candidateDoc is ALREADY parsed above (step (a)), so this is a
			// second scan of the same Document, not a re-parse.
			AppendDesignDiagnostics_( candidateDoc, out );

			return out;
		}

		namespace
		{
			//! Creative-richness P2 / P2.b (73-creative-richness-design.md sec
			//! 2 P2, RE-TARGETED by sec 7 to the two MEASURED bare-prompt
			//! deficits, DIAGNOSTIC-FRAMED by sec 9): the shared, engine-side
			//! "design note conditions" scan CORE, over an ALREADY-PARSED
			//! Document -- ONE function so its consumers (ComputeDesignNoteFromDoc_
			//! below, feeding the render-result "DESIGN NOTE" string via
			//! RenderCore_'s designNoteLocal AND the ComputeDesignNote text
			//! wrapper; and AppendDesignDiagnostics_ below, feeding validate's
			//! Info-severity diagnostics) can NEVER drift on the threshold
			//! logic -- sec 9's P2.b directive is "do not duplicate", so this
			//! is computed exactly ONCE per Document scan and both consumers
			//! read the same struct.
			//!
			//! Walks the document's top-level chunks ONCE, counting:
			//!   * `standard_object` chunks (the gate quantity for BOTH
			//!     conditions below);
			//!   * whether ANY `scalar_painter` chunk exists ANYWHERE in the
			//!     document (existence, not reachability from an object -- the
			//!     measured baseline failure was literally "no chunk of kind
			//!     'scalar_painter' exists", so that is exactly what this
			//!     checks);
			//!   * whether ANY `sdf_geometry` / `sweep_geometry` /
			//!     `displaced_geometry` chunk exists (same existence test, the
			//!     second measured deficit);
			//!   * a per-keyword census of every OTHER Geometry-category chunk
			//!     (registry-resolved via DescriptorForKeyword, the same
			//!     category lookup AgentEvalRunner.cpp's
			//!     CheckerCollectKindFilterMatches uses) -- purely for the
			//!     human-readable "k box_geometry, m sphere_geometry" clause;
			//!     it plays no role in either fire condition.
			//!
			//! FIRE CONDITIONS (sec 7's re-target, both pure existence/count
			//! checks -- deliberately no BuildReferenceGraph closure; a v1 that
			//! needs one hasn't been justified by the baseline):
			//!   A (scalar-pipe unused):  standardObjectCount >= 3 && !hasScalarPainter
			//!   B (no advanced geometry): standardObjectCount >= 4 && !hasAdvancedGeometry
			//! Neither condition looks at whether an object's MATERIAL actually
			//! reaches the missing kind -- an object with no material bound at
			//! all still counts toward the gate, matching the sec 7 text's
			//! "walk doc.items roles" framing (a coarser, cheaper test than A1's
			//! originally-approved reachability scan, superseded here).
			struct DesignNoteConditions_
			{
				bool conditionA = false;   //!< scalar pipe unused
				bool conditionB = false;   //!< no advanced geometry
				int  standardObjectCount = 0;
				std::map<std::string, int> geometryCensus;   //!< keyword -> count, every OTHER geometry kind seen (condition-B clause only)
			};

			DesignNoteConditions_ ComputeDesignNoteConditionsFromDoc_( const Document& doc )
			{
				DesignNoteConditions_ c;
				bool hasScalarPainter    = false;
				bool hasAdvancedGeometry = false;

				const int n = RISE::Cst::DocItemCount( doc );
				for( int i = 0; i < n; ++i ) {
					const RISE::Cst::NodeId id = RISE::Cst::DocNodeIdAt( doc, i );
					if( !id ) continue;
					const NodeRef item = RISE::Cst::DocResolveNodeId( doc, id );
					if( !item || item->kind != NodeKind::Chunk ) continue;
					const std::string& role = item->role;

					if( role == "standard_object" ) { ++c.standardObjectCount; continue; }
					if( role == "scalar_painter" )  { hasScalarPainter = true; continue; }
					if( role == "sdf_geometry" || role == "sweep_geometry" || role == "displaced_geometry" ) {
						hasAdvancedGeometry = true;
						++c.geometryCensus[role];
						continue;
					}

					const ChunkDescriptor* d = DescriptorForKeyword( String( role.c_str() ) );
					if( !d ) continue;
					if( d->category == ChunkCategory::Geometry ) ++c.geometryCensus[role];
				}

				c.conditionA = c.standardObjectCount >= 3 && !hasScalarPainter;
				c.conditionB = c.standardObjectCount >= 4 && !hasAdvancedGeometry;
				return c;
			}

			//! Shared formatter for condition B's "k box_geometry, m
			//! sphere_geometry" clause -- factored out so the note builder and
			//! the diagnostic builder render the SAME census string rather
			//! than two independently-maintained loops.
			std::string FormatGeometryCensus_( const std::map<std::string, int>& geometryCensus )
			{
				std::string census;
				for( const auto& kv : geometryCensus ) {
					if( !census.empty() ) census += ", ";
					census += std::to_string( kv.second ) + " " + kv.first;
				}
				if( census.empty() ) census = "no geometry chunks bound";
				return census;
			}

			//! RETURNS empty iff neither condition fires (the "omit the note
			//! entirely when clean" convention -- see AgentSkillResult::note
			//! and its AgentRpc.cpp `read_skill` carrier for the precedent this
			//! mirrors).  When one or both fire, returns ONE combined
			//! "DESIGN NOTE: ..." string carrying every firing clause plus the
			//! anti-churn escape clause (load-bearing from day one, sec 2 P2 --
			//! a loud signal with no escape clause just buys a different
			//! wasted-turn loop).  RENDER-RESULT PATH ONLY as of sec 9's P2.b
			//! (validate no longer attaches this string -- see
			//! AppendDesignDiagnostics_ below, its validate-side sibling).
			std::string ComputeDesignNoteFromDoc_( const Document& doc )
			{
				const DesignNoteConditions_ c = ComputeDesignNoteConditionsFromDoc_( doc );
				if( !c.conditionA && !c.conditionB ) return std::string();

				std::string note = "DESIGN NOTE:";
				if( c.conditionA ) {
					// Review P1 fix: the ORIGINAL wording ("all N materials use
					// constant roughness") is FALSE whenever the scene's
					// materials don't even HAVE a roughness parameter
					// (lambertian, perfect reflector/refractor, luminaire,
					// SSS, ...) -- including both of this file's own test
					// fixtures.  This wording claims only what is true
					// regardless of material mix: the scalar pipe (a
					// spatially-varying scalar_painter) is unused, so ANY
					// physical-scalar parameter that DOES exist is
					// necessarily a constant -- and names the material KINDS
					// that actually carry a roughness slot, rather than
					// implying every material in the scene has one.
					// Round-3 review P1 fix named only ggx/ward here on the
					// claim that pbr_metallic_roughness's `roughness` and
					// cooktorrance's `facets` are baked ValueKind::Double.
					// CORRECTION (2026-07-31, 74-creative-richness-arc-log.md
					// sec 4.3 correction note): that claim is FALSE -- both
					// are Reference-kind painter slots (since 64ca16bc,
					// 2026-04-30) and the parser accepts a painter binding.
					// The note text below therefore steers to the
					// HIGHER-friction path and omits the cheapest true one
					// (bind a painter to pbr `roughness` directly).  The text
					// is deliberately left as shipped: it is measured-inert
					// (0/24) and any wording change is a behavioural variable
					// that belongs to a measured arc-75 phase, not a comment
					// fix.
					note += " the scalar pipe is unused -- no scalar_painter chunk exists in this scene, "
						"so any physical-scalar material parameter (roughness, displacement) is a "
						"constant. Where a ggx_material (or ward_anisotropic_material) suits a "
						"surface, spatially-varying roughness via expression_function2d -> "
						"scalar_painter -> alphax/alphay adds realism "
						"(read_skill {\"name\":\"procedural-textures\"}).";
				}
				if( c.conditionB ) {
					note += " geometry census: " + std::to_string( c.standardObjectCount ) + " objects -- " +
						FormatGeometryCensus_( c.geometryCensus ) +
						"; no sdf_geometry/sweep_geometry/displaced_geometry forms (profiles of revolution are "
						"sdf roundcone+smin; read_skill {\"name\":\"object-modeling-recipes\"}).";
				}
				note += " If the user asked for a deliberately simple/stylised scene, this is fine -- "
					"ignore this note and do not churn.";
				return note;
			}

			//! validate's Info-severity sibling of ComputeDesignNoteFromDoc_
			//! (creative-richness P2.b, 73-creative-richness-design.md sec 9's
			//! closing recommendation): consumes the SAME
			//! ComputeDesignNoteConditionsFromDoc_ scan -- never re-derives the
			//! >=3 / >=4 threshold logic -- and appends ZERO, ONE, or TWO
			//! AgentDiagnostic entries to `out`: condition A ->
			//! DESIGN_SCALAR_PIPE_UNUSED, condition B ->
			//! DESIGN_NO_ADVANCED_GEOMETRY.  Both severity Info (an ADVISORY,
			//! not a correctness problem -- see AgentDiagnostic.h).
			//!
			//! Message text is the SAME material/parameter claim the note
			//! clause carries (the clause substring is copied verbatim, not
			//! reworded -- it survived two truth reviews), with the leading
			//! space that glued it onto "DESIGN NOTE:" trimmed since this is
			//! now a standalone message, plus a per-diagnostic self-disarm
			//! suffix.  Sec 9's caveat is binding here: the diagnostics shape
			//! has no escape-clause slot the way the note's trailing sentence
			//! does, so a deliberately flat/simple scene would otherwise carry
			//! what LOOKS like a permanent, unfixable problem -- the suffix on
			//! EACH message is what keeps it advisory instead.
			//!
			//! Called ONLY from AgentSession::ValidateText -- the render-result
			//! note path (ComputeDesignNoteFromDoc_'s OTHER two call sites, in
			//! RenderCore_, plus the ComputeDesignNote text wrapper) is
			//! untouched; see that function's doc for why validate no longer
			//! calls it.
			void AppendDesignDiagnostics_( const Document& doc, std::vector<AgentDiagnostic>& out )
			{
				const DesignNoteConditions_ c = ComputeDesignNoteConditionsFromDoc_( doc );
				if( !c.conditionA && !c.conditionB ) return;

				static const char* const kSelfDisarm =
					" If flat/simple styling is intentional, this is fine -- ignore.";

				if( c.conditionA ) {
					AgentDiagnostic d;
					d.severity = AgentDiagnostic::Severity::Info;
					d.code     = AgentDiagnosticCode::DESIGN_SCALAR_PIPE_UNUSED;
					d.message  = "the scalar pipe is unused -- no scalar_painter chunk exists in this scene, "
						"so any physical-scalar material parameter (roughness, displacement) is a "
						"constant. Where a ggx_material (or ward_anisotropic_material) suits a "
						"surface, spatially-varying roughness via expression_function2d -> "
						"scalar_painter -> alphax/alphay adds realism "
						"(read_skill {\"name\":\"procedural-textures\"}).";
					d.message += kSelfDisarm;
					out.push_back( d );
				}
				if( c.conditionB ) {
					AgentDiagnostic d;
					d.severity = AgentDiagnostic::Severity::Info;
					d.code     = AgentDiagnosticCode::DESIGN_NO_ADVANCED_GEOMETRY;
					d.message  = "geometry census: " + std::to_string( c.standardObjectCount ) + " objects -- " +
						FormatGeometryCensus_( c.geometryCensus ) +
						"; no sdf_geometry/sweep_geometry/displaced_geometry forms (profiles of revolution are "
						"sdf roundcone+smin; read_skill {\"name\":\"object-modeling-recipes\"}).";
					d.message += kSelfDisarm;
					out.push_back( d );
				}
			}
		}

		//! Thin, STATELESS wrapper (like ValidateText above) around
		//! ComputeDesignNoteFromDoc_: parses `documentText` to its OWN
		//! throwaway CST Document (never touches a session's retained one)
		//! and runs the shared scan.  Used ONLY by the `validate` carrier
		//! (AgentRpc.cpp, both its head-form and text-form branches, on the
		//! retained-snapshot / candidate text they already have in hand).
		//!
		//! NEVER call this (or ReadDocumentSnapshot, or anything else that
		//! re-enters the controller) from inside a RunPreviewRenderParked /
		//! SubmitAgentRender* closure -- those already hold the
		//! controller's non-recursive mMutex for the closure's whole
		//! duration, and this wrapper's ParseToCst is also needless
		//! SerializeCst+ParseToCst work when a live, already-parsed
		//! Document is sitting right there.  RenderCore_'s render-result
		//! carrier does NOT use this overload for exactly that reason: it
		//! calls ComputeDesignNoteFromDoc_ directly on
		//! `mJob->GetCstDocument()` from INSIDE doRenderWork's tail, while
		//! still under the park -- see designNoteLocal's declaration and
		//! its two call sites in RenderCore_.
		std::string AgentSession::ComputeDesignNote( const std::string& documentText )
		{
			if( documentText.empty() ) return std::string();
			return ComputeDesignNoteFromDoc_( RISE::Cst::ParseToCst( documentText ) );
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

			//! G2 (2026-08-10, refuse-until-filed cap): folds a build-plan-gate
			//! GIVE-UP notice into a std::string result field no matter which
			//! `return` inside the caller actually fires.  The notice is only
			//! known the instant CheckBuildPlanGate_ decides to give up --
			//! which happens BEFORE the rest of the caller's normal logic
			//! runs and long before that logic picks its own exit -- so a
			//! destructor-time append is the one place that can't be missed
			//! by an early return.  `field` binds to a result object declared
			//! earlier in the same scope (so it outlives this guard); setting
			//! `.notice` after construction is enough, there is nothing else
			//! to call.  A no-op when `.notice` stays empty (the overwhelming
			//! common case: disabled, already filed, mid-refusal, or already
			//! given up).
			//!
			//! G2 fix-round (2026-08-10): DEFINED HERE, above ProposePatch,
			//! rather than at its original site above InsertChunk -- the patch
			//! arm added below needs it too and is the earlier of the two in
			//! this file.  Every `namespace { ... }` block at this scope
			//! reopens the SAME anonymous namespace, so InsertChunk's use
			//! further down names this identical type.
			struct BuildPlanGiveUpFold_
			{
				std::string& field;
				std::string  notice;
				~BuildPlanGiveUpFold_()
				{
					if( notice.empty() ) return;
					if( !field.empty() ) field += "  ";
					field += notice;
				}
			};
		}

		AgentPatchResult AgentSession::ProposePatch( const AgentSetPatch& patch )
		{
			AgentPatchResult r;
			BuildPlanGiveUpFold_ g2Fold{ r.message, std::string() };
			// S1 (2026-08-11): the phase machinery's own give-up fold (a second
			// instance, never a shared field -- see InsertChunk's identical
			// pair), and the splice-attribution hook whose name/kind the arm
			// below fills in when a value really would introduce a chunk.
			BuildPlanGiveUpFold_ s1Fold{ r.message, std::string() };
			AttributePatchOnApply_ s1Attr{ *this, r, std::string(), std::string() };

			// G2 fix-round (2026-08-10, build-plan gate -- the PATCH arm): FIRST,
			// ahead of E1's and R1c's and ahead of the authority branching, for
			// the same two reasons InsertChunk's G2 arm goes first.  (1) It is a
			// pure SEQUENCING check that consults nothing about whether the
			// patch would otherwise be accepted.  (2) The three gates are
			// DISJOINT by construction, so ordering cannot steal a refusal from
			// another gate: E1 polices `csg_object` (registry category Object),
			// R1c polices rasterizer chunks (category Rasterizer), and this arm
			// fires only on ChunkCategory::Geometry.
			//
			// WHY A PATCH ARM EXISTS AT ALL.  Until this fix the build-plan gate
			// had exactly four call sites, all INSERT verbs, while
			// propose_patch could create geometry outright: a param value is
			// spliced into the document as TEXT, so a value carrying `}` plus a
			// whole `box_geometry { ... }` block lands a real geometry chunk on
			// serialization.  That is the same VALUE-SPLICE bypass R1c's arm (b)
			// was built to close, and with no equivalent here a session could
			// build an entire scene through propose_patch with no plan filed and
			// the refusal counter still reading zero -- the instrument measuring
			// nothing.  The delta itself lives in the shared, PURE
			// DescribeBuildPlanGeometryDeltaForPatch (AgentSession.h), which
			// SceneEditController::ResolveProposal's re-check also calls.
			//
			// COST.  Guarded on BuildPlanGateArmed_() -- a pure bool/int test
			// with no parse, no lock and no document access -- exactly as the
			// insert arms are.  The gate disarms PERMANENTLY on the first filed
			// plan or the third refusal, so for the whole rest of any session
			// this arm costs one predictable branch and nothing else.  While
			// armed it does pay its own snapshot + serialize/reparse round-trip
			// (it does not share R1c's, which is computed inside a free function
			// R1c may not even reach); that is a handful of calls at the head of
			// a session, against a surface whose cheapest verb is a render.
			//
			// `retriable` stays FALSE, for the two reasons InsertChunk's arm
			// documents (the wire flag is the GUI chat loops' SILENT client-side
			// auto-retry signal -- it would burn all three refusals and trip the
			// give-up before the model ever saw one).
			if( BuildPlanGateArmed_() && ( !patch.target.empty() || !patch.kind.empty() ) )
			{
				const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
				std::string g2Name;
				const std::string g2Kind = snap.hasDocument
					? DescribeBuildPlanGeometryDeltaForPatch( snap.document, patch.target, patch.kind,
					                                          patch.param, patch.value, &g2Name )
					: std::string();
				if( !g2Kind.empty() ) {
					// SAME machinery as the four insert sites: same refusal text
					// (verb substituted), the SAME session-wide 3-refusal budget
					// (one counter, not a second), the same give-up transition
					// folded through g2Fold, and the same document-untouched
					// guarantee -- this returns before E1, R1c, staging and every
					// commit path below.
					const std::string clause = CheckBuildPlanGate_( "propose_patch", &g2Fold.notice );
					if( !clause.empty() ) {
						r.applied     = false;
						r.retriable   = false;
						r.rawCode     = 0;
						r.status      = "rejected";
						r.headVersion = snap.headVersion;
						r.message     = clause + " (this patch's value would have introduced a `" +
						                g2Kind + "` chunk" +
						                ( g2Name.empty() ? std::string() : ( " named `" + g2Name + "`" ) ) + ".)";
						return r;
					}
					// clause.empty() here means this call IS the give-up -- the
					// notice was written into g2Fold and its destructor folds it
					// into r.message whichever return below fires.
				}
			}

			// S1 (2026-08-11, the staged build protocol): TWO arms on this verb,
			// mutually exclusive by phase.
			//
			// (a) THE CROSS-ELEMENT EDIT.  While an element window is open, a
			// patch aimed at a chunk created under a DIFFERENT element is
			// refused.  A pure lookup in this session's own attribution list --
			// no parse, no snapshot, no document access -- and it is skipped
			// entirely for an empty `target` (a kind-addressed singleton: film,
			// rasterizer, the sole camera), for an UNATTRIBUTED chunk, and for
			// the phase-exempt kinds (see KindIsPhaseExemptCategory_ -- lights,
			// cameras, film, rasterizers, rasterizer outputs, materials and
			// painters), which is what keeps the design's "over-refusal is the
			// failure mode to fear" rule true here.
			{
				const std::string clause = CheckElementWindowForEdit_( "propose_patch", patch.target,
				                                                        &s1Fold.notice );
				if( !clause.empty() ) {
					r.applied     = false;
					r.retriable   = false;   // see the G2 arm above for why
					r.rawCode     = 0;
					r.status      = "rejected";
					r.headVersion = ReadHeadVersion();
					r.message     = clause;
					return r;
				}
			}

			// (b) THE VALUE-SPLICE CREATION, in the COMPOSE phase and as an
			// ATTRIBUTION everywhere else.  The neighbouring gate arm exists
			// because a param value is spliced into the document as TEXT, so a
			// value carrying `}` plus a whole `box_geometry { ... }` block lands
			// a real geometry chunk on serialization; that arm must stay closed,
			// and the phase rules have exactly the same hole to close -- a
			// compose-phase session could otherwise build new geometry through
			// propose_patch, and a pieces-phase one could land geometry that
			// belongs to no element and is therefore exempt from every window
			// rule afterwards.
			//
			// COST.  The delta needs a snapshot plus a serialize/reparse round
			// trip, and unlike the gate's arm (armed only at the head of a
			// session) this one would otherwise run on EVERY patch for the whole
			// build.  The `}` pre-filter is what bounds it: closing the current
			// chunk is a NECESSARY step of the splice mechanism -- chunk syntax
			// has no brace-free form -- so a value with no `}` cannot introduce
			// a chunk, and an ordinary param value never carries one.
			if( BuildProtocolActive_() &&
			    ( !patch.target.empty() || !patch.kind.empty() ) &&
			    patch.value.find( '}' ) != std::string::npos )
			{
				const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
				std::string s1Name;
				const std::string s1Kind = snap.hasDocument
					? DescribeBuildPlanGeometryDeltaForPatch( snap.document, patch.target, patch.kind,
					                                           patch.param, patch.value, &s1Name )
					: std::string();
				if( !s1Kind.empty() ) {
					if( mBuildPhase == AgentBuildPhase::Compose ) {
						const std::string clause = CheckComposePhaseForCreate_( "propose_patch",
						                                                         &s1Fold.notice );
						if( !clause.empty() ) {
							r.applied     = false;
							r.retriable   = false;
							r.rawCode     = 0;
							r.status      = "rejected";
							r.headVersion = snap.headVersion;
							r.message     = clause + " (this patch's value would have introduced a `" +
							                s1Kind + "` chunk" +
							                ( s1Name.empty() ? std::string()
							                                 : ( " named `" + s1Name + "`" ) ) + ".)";
							return r;
						}
					}
					// Not refused -- so if it LANDS, the chunk it introduces is
					// this element's, exactly like one from insert_chunk.
					s1Attr.name = s1Name;
					s1Attr.kind = s1Kind;
				}
			}

			// Post-arc enforcement E1 (docs/agentic-redesign/75-expressive-surface-
			// arc.md sec 7 / 76-...-log.md sec 3's mechanism law -- blocking
			// facts act, a Warning gets skimmed): does this patch CREATE (or
			// RECREATE, via an acknowledgment removal) an unacknowledged
			// emissive-CSG null-geometry binding?  Engine-side, ahead of the
			// authority/mode branching below, so every surface (CLI, GUI,
			// External-authority staging) inherits it identically.  The three
			// triggers (csg-side `material` re-point / `allow_non_sampling_
			// emitter` removal / a material-side edit reaching a referencing
			// csg_object) and the cheap pre-filters that bound the cost of
			// each live in CheckNonSamplingEmitterGateForPatch (AgentSession.h)
			// -- SHARED with SceneEditController::ResolveProposal's stale-
			// staged-proposal re-check, so this is a two-line call-through,
			// not a duplicate of that logic.
			if( !patch.target.empty() )
			{
				const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
				if( snap.hasDocument ) {
					const std::string clause = CheckNonSamplingEmitterGateForPatch(
						snap.document, patch.target, patch.kind, patch.param, patch.value );
					if( !clause.empty() ) {
						r.applied     = false;
						r.rawCode     = 0;
						r.status      = "rejected";
						r.headVersion = snap.headVersion;
						r.message     = "propose_patch refused: " + clause;
						return r;
					}
				}
			}

			// R1c (2026-08-09, agent rasterizer allowlist): the SECOND
			// engine-side creation gate, same choke point and same shape as
			// E1's above -- ahead of the authority/mode branching so CLI, GUI
			// and External-authority staging all inherit it identically, and
			// SHARED with SceneEditController::ResolveProposal's re-check via
			// CheckRasterizerAllowlistGateForPatch (AgentSession.h).  NOT
			// folded into the block above because the target predicate
			// differs: E1 requires a NAMED target (a csg_object always has a
			// name), while the rasterizer chunks this gate polices have NO
			// `name` param and are addressed as kind-addressed singletons
			// (empty target + kind).
			if( !patch.target.empty() || !patch.kind.empty() )
			{
				const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
				if( snap.hasDocument ) {
					const std::string clause = CheckRasterizerAllowlistGateForPatch(
						snap.document, patch.target, patch.kind, patch.param, patch.value );
					if( !clause.empty() ) {
						r.applied     = false;
						r.rawCode     = 0;
						r.status      = "rejected";
						r.headVersion = snap.headVersion;
						r.message     = "propose_patch refused: " + clause;
						return r;
					}
				}
			}

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
				// G2 fix-round (2026-08-10): carry the gate's ARMED-ness to
				// resolve time.  Reaching here with it armed means the G2 arm at
				// the top of this function found NO geometry delta against the
				// head as it stood a moment ago -- but a param edit's effect is
				// head-dependent (an unresolvable target can become resolvable),
				// so the controller re-runs the same stateless delta on approval.
				// See AgentProposal::buildPlanGateArmedAtStage.
				p.buildPlanGateArmedAtStage = BuildPlanGateArmed_();
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
					// reject in this function.  Via ReadHeadVersion, NOT
					// the bare accessor: a controller IS attached on this
					// branch, so an unlocked read of the 16-byte
					// CstHeadVersion races that controller's writers.
					r.applied    = false;
					r.rawCode    = 0;
					r.status     = "rejected";
					r.queueFull  = true;
					r.headVersion = ReadHeadVersion();
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
				// Through IsAnalysableRejection_ + ReadHeadDocumentAt_, not
				// `rawCode == 0` + a bare GetCstDocument(): a controller is
				// attached here, so the head must be read under its lock and
				// must still BE the version this result stamps -- and the
				// controller's pre-derive GATE refusals share the rawCode==0
				// bucket while having no head to read (see both helpers).
				RISE::Cst::Document headDoc;
				if( IsAnalysableRejection_( r ) && ReadHeadDocumentAt_( r.headVersion, headDoc ) )
					AttachParamEditRejectionIssues( r, headDoc,
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
			// An EMPTY target with a NON-EMPTY kind is the KIND-ADDRESSED
			// SINGLETON form -- the sole unnamed camera / film / rasterizer,
			// resolved by Job::ApplyCstParamEditImpl_'s unique-in-kind
			// fallback.  Only the BOTH-empty case is invalid.
			//
			// This guard used to reject every empty target, which put this
			// direct/headless path out of step with the controller path
			// (SceneEditController::ApplyAgentParamEdit already gates on
			// "both empty" and documents the singleton form).  The divergence
			// was invisible until a scene build tried to move a camera it had
			// just created: with no name to address, the model guessed the
			// chunk KEYWORD as the target, was rejected, and fell back to
			// remove_chunk + insert_chunk -- three turns and two extra head
			// revisions to set a location.
			if( ( patch.target.empty() && patch.kind.empty() )
			 || patch.param.empty() || patch.value.empty() ) {
				r.applied = false;
				r.rawCode = 0;
				r.status  = "rejected";
				r.headVersion = mJob->GetCstHeadVersion();
				r.message = patch.target.empty() && patch.kind.empty()
					? "target and kind cannot BOTH be empty -- name the entity, or leave "
					  "target empty and pass kind to address the sole unnamed chunk of "
					  "that kind (camera / film / rasterizer)"
					: "param and value must both be non-empty";
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

			//! R1a (2026-08-09, batched remove_chunks): FoldChunkCode's batch sibling -- Job::ApplyCstRemoveChunks
			//! returns the SAME 2/3/0/-1/-2 alphabet, so this fold mirrors the remove half of FoldChunkCode
			//! with two batch-specific additions: every refusal message says explicitly that NOTHING was
			//! removed (the all-or-nothing contract is the one thing a model must not have to infer), and a
			//! -1/-2 names the OFFENDING TARGET by `failIndex` so the next call can fix that one element.
			//! `unique` is the deduped target list `failIndex` indexes into.
			void FoldRemoveBatchCode_( AgentSession::AgentRemoveBatchResult& r, int code, int failIndex,
			                           const std::vector<std::string>& unique, const char* diag )
			{
				const bool haveOffender = ( failIndex >= 0 && failIndex < static_cast<int>( unique.size() ) );
				const std::string offender = haveOffender ? ( " '" + unique[static_cast<std::size_t>( failIndex )] + "'" )
				                                          : std::string();
				r.rawCode = ( code < 0 ) ? 0 : code;
				switch( code ) {
					case 2:
						r.applied = true;
						r.status  = "applied";
						{
							char buf[160];
							std::snprintf( buf, sizeof( buf ),
								"%d chunk%s removed via a SINGLE full re-derive (one head-version bump, one undo step)",
								static_cast<int>( unique.size() ), unique.size() == 1 ? "" : "s" );
							r.message = buf;
						}
						break;
					case 3:
						r.applied = false;
						r.status  = "diagnosed";
						r.message = "batch remove NOT a clean success: the Document was mutated and the live managers "
						            "were replaced, BUT the full re-derive emitted diagnostics (see log) -- do NOT "
						            "treat as applied";
						break;
					case -1:
						r.applied = false;
						r.status  = "rejected";
						r.message = "batch remove rejected -- NOTHING was removed, head unchanged: target" + offender;
						if( diag && diag[0] ) { r.message += " -- "; r.message += diag; }
						else                  { r.message += " was not found"; }
						break;
					case -2:
						r.applied = false;
						r.status  = "rejected";
						// remove_chunks takes bare names only, so the escape hatch for an ambiguous name is
						// the SINGULAR verb (which owns `kind`) -- say that, rather than "pass a kind" for a
						// parameter this verb does not have.
						r.message = "batch remove rejected -- NOTHING was removed, head unchanged: target" + offender
						          + " is ambiguous; remove_chunks takes bare names only, so remove that one with a "
						            "single remove_chunk call passing its `kind`";
						if( diag && diag[0] ) { r.message += " ("; r.message += diag; r.message += ")"; }
						break;
					case 0:
					default:
						r.applied = false;
						r.status  = "rejected";
						// Both honest causes, restated for a batch.  An INTRA-batch reference is explicitly
						// NOT one of them: every target leaves the Document before the single re-derive runs.
						r.message = "batch remove rejected -- NOTHING was removed, head unchanged: the remaining "
						            "document would not derive (a target is likely still REFERENCED by a chunk "
						            "OUTSIDE this batch, or the remaining document no longer derives in order -- "
						            "read_document and validate to inspect)";
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

			//! R1a (2026-08-09, batched remove_chunks): the BATCH sibling of AttachRemoveRejectionIssues.
			//! Runs ONCE over the whole (already-deduped) target list against the head Document a REFUSED
			//! batch left byte-identical, and attaches at most one issue to each per-target result:
			//!
			//!   * "unknown_target"    -- the name does not resolve to any chunk (near-miss candidates ranked
			//!                            by the SAME RankNearMisses/CollectTargetNameCandidates pair
			//!                            AnalyzeRejectedParamEdit uses, so the two verbs suggest alike).
			//!                            This EXTENDS "unknown_target" from the propose_patch-only group in
			//!                            AgentChunkIssue's doc to remove_chunks -- the slug's meaning is
			//!                            unchanged (a `target` that resolves to nothing).
			//!   * "still_referenced"  -- the name resolves AND the reference graph names at least one
			//!                            referrer that is NOT ITSELF IN THIS BATCH.  Subtracting the batch
			//!                            is the whole difference from the singular analyser and it is the
			//!                            honest thing to report: an intra-batch referrer does NOT block this
			//!                            removal (every target leaves the Document before the single
			//!                            re-derive runs), so naming one would send the model chasing a
			//!                            referrer it already asked to delete.
			//!
			//! HONESTY, inherited verbatim from AnalyzeRejectedRemove: finding nothing is NOT exoneration --
			//! a dynamic reference this static graph does not model could still exist, and the OTHER cause
			//! (the remaining document no longer derives in order) produces no issue at all.  A target with
			//! no attributable cause simply gets no issue and keeps the caller's hedged message.
			void AttachRemoveBatchRejectionIssues( std::vector<AgentChunkResult>& perTarget,
			                                       const RISE::Cst::Document& doc,
			                                       const std::vector<std::string>& targets )
			{
				if( perTarget.size() != targets.size() ) return;   // defensive: the two are built in lockstep

				// Resolve every target ONCE -- the ids double as the "is this referrer in the batch?" set.
				// No kind narrowing and no camera positional fallback: remove_chunks takes bare names only
				// (see AgentSession::RemoveChunks' NO PER-TARGET `kind` note).
				std::vector<RISE::Cst::NodeId> ids( targets.size(), 0 );
				std::vector<int>               occs( targets.size(), 0 );
				std::set<RISE::Cst::NodeId>    inBatch;
				for( std::size_t i = 0; i < targets.size(); ++i ) {
					ids[i] = RISE::Cst::DocFindByNameAnyRole( doc, targets[i], &occs[i], "", false );
					if( ids[i] != 0 ) inBatch.insert( ids[i] );
				}

				const RISE::Cst::ReferenceGraph graph = RISE::Cst::BuildReferenceGraph( doc, nullptr, nullptr );
				for( std::size_t i = 0; i < targets.size(); ++i )
				{
					if( ids[i] == 0 ) {
						// AMBIGUOUS (occ > 1), not unknown: the name DOES resolve to chunks, just not to
						// ONE.  There is no "ambiguous" slug in AgentChunkIssue's closed reason set, and
						// labelling it "unknown_target" would be a LIE that sends the model hunting for a
						// typo it did not make.  Stay silent and let the caller's message -- which names
						// the target and points at the singular verb's `kind` -- carry it.
						if( occs[i] > 1 ) continue;
						AgentChunkIssue issue;
						issue.value       = targets[i];
						issue.reason      = "unknown_target";
						issue.suggestions = RankNearMisses( CollectTargetNameCandidates( doc, std::string() ), targets[i] );
						perTarget[i].issues.push_back( issue );
						continue;
					}
					const std::map<RISE::Cst::NodeId, std::set<RISE::Cst::NodeId> >::const_iterator dep =
						graph.dependents.find( ids[i] );
					if( dep == graph.dependents.end() || dep->second.empty() ) continue;   // HONESTY: no referrer -> stay silent

					AgentChunkIssue issue;
					issue.value  = targets[i];
					issue.reason = "still_referenced";
					for( const RISE::Cst::NodeId refId : dep->second )
					{
						if( inBatch.count( refId ) != 0 ) continue;   // an INTRA-BATCH referrer does not block
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
					// Every referrer was in the batch (or none produced a nameable label) -> this target is
					// NOT blocked; emit nothing rather than an issue with an empty referrer list.
					if( issue.suggestions.empty() ) continue;
					perTarget[i].issues.push_back( issue );
				}
			}

			//! R1a: fold the per-target issues attached above into ONE actionable clause for the batch-level
			//! message -- the batch analogue of AttachRemoveRejectionIssues' own message tail.  Returns ""
			//! when no issue was attributable to any target (HONESTY: the caller then leaves its hedged
			//! message untouched).
			std::string BuildRemoveBatchActionableClause( const std::vector<AgentChunkResult>& perTarget )
			{
				std::string clauses;
				for( const AgentChunkResult& tr : perTarget ) {
					for( const AgentChunkIssue& issue : tr.issues ) {
						if( !clauses.empty() ) clauses += "; ";
						if( issue.reason == "still_referenced" ) {
							std::string refs;
							for( const std::string& s : issue.suggestions ) {
								if( !refs.empty() ) refs += ", ";
								refs += "'" + s + "'";
							}
							clauses += "'" + issue.value + "' is still referenced from OUTSIDE this batch by " + refs;
						} else if( issue.reason == "unknown_target" ) {
							clauses += "'" + issue.value + "' does not name any chunk";
							if( !issue.suggestions.empty() ) {
								std::string cands;
								for( const std::string& s : issue.suggestions ) {
									if( !cands.empty() ) cands += ", ";
									cands += "'" + s + "'";
								}
								clauses += " (did you mean " + cands + "?)";
							}
						}
					}
				}
				if( clauses.empty() ) return std::string();
				return " ACTIONABLE: " + clauses + ".";
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
			BuildPlanGiveUpFold_ g2Fold{ r.message, std::string() };
			// S1 (2026-08-11): the phase machinery's OWN give-up fold, a second
			// instance of the same destructor-time appender rather than a reuse
			// of g2Fold's `notice` field -- one field holding two notices would
			// silently drop whichever was written second.  The two can only
			// co-occur across separate calls (the gate is armed in the Plan
			// phase, the compose refusal fires in the Compose phase), so this is
			// belt-and-braces, and cheap.
			BuildPlanGiveUpFold_ s1Fold{ r.message, std::string() };
			// S1: attribute whatever this call LANDS to the active element, on
			// whichever of this function's returns fires.
			AttributeOnApply_ s1Attr{ *this, r };

			// G2 (2026-08-10, build-plan gate): the InsertChunk arm, FIRST --
			// ahead of R1c's and E1's, because this gate is a pure SEQUENCING
			// check that consults nothing about the document and nothing about
			// whether the chunk would otherwise be accepted.  It only fires on
			// a chunk text that really does create geometry (the registry
			// category test), so it can never burn a refusal on a call that
			// was going to be refused for being a blocked rasterizer (not a
			// Geometry chunk) or an unacknowledged emissive csg_object
			// (csg_object is not a Geometry chunk either) -- the three gates
			// are disjoint by construction.
			//
			// `retriable` stays FALSE, deliberately -- now for TWO independent
			// reasons, either alone sufficient.  (1) The wire's `retriable`
			// flag means something narrower than "the model is meant to
			// reissue this": it is the CLIENT-SIDE auto-retry signal both GUI
			// chat loops read (ChatViewModel.swift / ChatPanel.cpp re-dispatch
			// a retriable refusal up to 5 times WITHOUT showing the model
			// anything).  Setting it true would have the GUI silently burn
			// the gate on the model's behalf -- the refusal would never reach
			// the model at all, defeating the mechanism on the exact surface
			// it was designed for.  (2) That risk is now STRICTLY WORSE under
			// the refuse-until-filed cap: a client that auto-retries up to 5
			// times would burn all kBuildPlanGateMaxRefusals (3) refusals AND
			// trip the give-up transition before the model ever saw a single
			// one of them -- the model would never learn the gate exists.
			std::string g2Kind, g2Name;
			if( BuildPlanGateArmed_() && ChunkTextCreatesGeometry_( chunkText, &g2Kind, &g2Name ) ) {
				const std::string clause = CheckBuildPlanGate_( "insert_chunk", &g2Fold.notice );
				if( !clause.empty() ) {
					r.applied     = false;
					r.retriable   = false;
					r.rawCode     = 0;
					r.status      = "rejected";
					r.headVersion = ReadHeadVersion();
					// Identity echo even on refusal -- the contract every other
					// InsertChunk guard honours.  Free here: the gate already
					// parsed the chunk to classify it.
					r.kind        = g2Kind;
					r.name        = g2Name;
					r.message     = clause;
					return r;
				}
				// clause.empty() here means either "not armed / already
				// resolved" OR "this call is the give-up" -- g2Fold.notice was
				// written in the latter case and stays empty in the former;
				// either way, normal InsertChunk processing continues below
				// and g2Fold's destructor folds any notice into r.message
				// whichever return statement this function ultimately takes.
			}

			// S1 (2026-08-11, the staged build protocol): the COMPOSE-phase
			// creation refusal, immediately after the gate arm and on the same
			// terms -- it fires only on a chunk text that really does create
			// geometry (the same registry-category test), so it can never burn
			// a phase refusal on a call that was going to be refused for being
			// malformed, a blocked rasterizer or an unacknowledged emissive CSG.
			// The two mechanisms cannot both fire on one call: the gate is armed
			// only while no plan is filed (phase Plan), and this one only after
			// every element is finished (phase Compose).
			//
			// `retriable` stays FALSE for the same two reasons the gate's arms
			// document (the wire flag is the GUI chat loops' SILENT client-side
			// auto-retry signal; a client that auto-retried would burn the whole
			// phase budget before the model saw one refusal).
			if( mBuildPhase == AgentBuildPhase::Compose ) {
				std::string s1Kind, s1Name;
				if( ChunkTextCreatesGeometry_( chunkText, &s1Kind, &s1Name ) ) {
					const std::string clause = CheckComposePhaseForCreate_( "insert_chunk", &s1Fold.notice );
					if( !clause.empty() ) {
						r.applied     = false;
						r.retriable   = false;
						r.rawCode     = 0;
						r.status      = "rejected";
						r.headVersion = ReadHeadVersion();
						r.kind        = s1Kind;
						r.name        = s1Name;
						r.message     = clause;
						return r;
					}
				}
			}

			// S2 (2026-08-11, clean-room construction): the FIRST-GEOMETRY
			// refusal -- while the active element has no chunk of its own, the
			// first geometry for it comes from build_element (design sec 4).
			// Placed on the SAME terms as the two arms above (geometry-creating
			// text only, same shared counter, same retriable=false) and mutually
			// exclusive with the compose arm by phase.  It never fires on
			// BuildElement's own insertion (see the predicate) and never on a
			// session whose host installed no text completer, so it can only
			// redirect construction toward a path that actually exists.
			if( mBuildPhase == AgentBuildPhase::Pieces ) {
				std::string s2Kind, s2Name;
				if( ChunkTextCreatesGeometry_( chunkText, &s2Kind, &s2Name ) ) {
					const std::string clause =
						CheckFirstGeometryThroughCleanRoom_( "insert_chunk", &s1Fold.notice );
					if( !clause.empty() ) {
						r.applied     = false;
						r.retriable   = false;
						r.rawCode     = 0;
						r.status      = "rejected";
						r.headVersion = ReadHeadVersion();
						r.kind        = s2Kind;
						r.name        = s2Name;
						r.message     = clause;
						return r;
					}
				}
			}

			// R1c (2026-08-09, agent rasterizer allowlist): the InsertChunk arm
			// of the gate, FIRST -- before E1's and before the authority
			// branching -- because it is the cheapest of the three (a CST parse
			// and a descriptor lookup; no derive) and because a blocked
			// rasterizer is refused unconditionally, so nothing later in this
			// function can change the answer.  SHARED with
			// SceneEditController::ResolveProposal's re-check via
			// CheckRasterizerAllowlistGateForInsert (AgentSession.h).
			{
				const std::string clause = CheckRasterizerAllowlistGateForInsert( chunkText );
				if( !clause.empty() ) {
					// Identity echo even on refusal, the contract every other
					// InsertChunk guard honours.  A rasterizer chunk has no
					// `name`, so only `kind` is echoable -- and it is the one
					// the model needs.
					const RISE::Cst::Document chunkDoc = RISE::Cst::ParseToCst( chunkText );
					const int n = RISE::Cst::DocItemCount( chunkDoc );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeRef it =
							RISE::Cst::DocResolveNodeId( chunkDoc, RISE::Cst::DocNodeIdAt( chunkDoc, i ) );
						if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
						if( ClassifyAgentRasterizerKind( it->role ) == AgentRasterizerPolicy::Blocked ) {
							r.kind = it->role;
							break;
						}
					}
					r.applied     = false;
					r.rawCode     = 0;
					r.status      = "rejected";
					r.headVersion = ReadHeadVersion();
					r.message     = "insert_chunk refused: " + clause;
					return r;
				}
			}

			// Post-arc enforcement E1: the InsertChunk sibling of ProposePatch's
			// identical gate -- does inserting this chunk CREATE an
			// unacknowledged emissive-CSG null-geometry binding?  SHARED with
			// SceneEditController::ResolveProposal's stale-staged-proposal
			// re-check via CheckNonSamplingEmitterGateForInsert
			// (AgentSession.h); this parse-for-echo is ONLY so a refusal can
			// still stamp r.kind/r.name (the same identity-echo-even-on-
			// refusal contract every other InsertChunk guard honours) --
			// the gate function does its OWN parse + the derive-bearing work.
			{
				const RISE::Cst::Document chunkDoc = RISE::Cst::ParseToCst( chunkText );
				RISE::Cst::NodeRef chunkItem;
				{
					const int n = RISE::Cst::DocItemCount( chunkDoc );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId( chunkDoc, RISE::Cst::DocNodeIdAt( chunkDoc, i ) );
						if( it && it->kind == RISE::Cst::NodeKind::Chunk ) { chunkItem = it; break; }
					}
				}
				if( chunkItem && chunkItem->role == "csg_object" ) {
					const std::string touchedName = ChunkParamString_( chunkItem, "name" );
					if( !touchedName.empty() ) {
						const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
						if( snap.hasDocument ) {
							const std::string clause = CheckNonSamplingEmitterGateForInsert( snap.document, chunkText );
							if( !clause.empty() ) {
								r.applied     = false;
								r.rawCode     = 0;
								r.status      = "rejected";
								r.headVersion = snap.headVersion;
								r.kind        = chunkItem->role;
								r.name        = touchedName;
								r.message     = "insert_chunk refused: " + clause;
								return r;
							}
						}
					}
				}
			}

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
					// queue-full branch for the full rationale, including
					// why the head-version read is controller-mediated.
					r.applied    = false;
					r.rawCode    = 0;
					r.status     = "rejected";
					r.queueFull  = true;
					r.headVersion = ReadHeadVersion();
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
				// Through ReadHeadDocumentAt_ on BOTH arms: a controller is
				// attached, so the head is read under its lock and only
				// analysed while it still IS the version this result stamps.
				// The reject arm additionally screens out the controller's
				// pre-derive gate refusals (IsAnalysableRejection_).
				RISE::Cst::Document headDoc;
				if( r.applied && r.headVersion.uuid != 0
				 && ReadHeadDocumentAt_( r.headVersion, headDoc ) )
					AttachChunkIssueWarnings( r, headDoc, chunkText );
				// ...and the REJECTION analyser on the failing side.  THIS is the
				// GUI path -- the one where the unactionable "apply failed (e.g.
				// unresolved reference); see log" was actually observed stalling a
				// local model -- so it needs the actionable clause at least as much
				// as the headless path does.  A rejection leaves the head
				// UNCHANGED, so the retained Document is the correct namespace to
				// resolve the candidate chunk's references against.
				else if( IsAnalysableRejection_( r ) && ReadHeadDocumentAt_( r.headVersion, headDoc ) )
					AttachRejectionIssues( r, headDoc, chunkText );
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

			// G2 (2026-08-10, build-plan gate): an UP-FRONT scan, before the
			// per-element loop and before R1c's own scan, for the SAME reason
			// R1c deviates from the documented best-effort contract -- a
			// POLICY refusal is not an authoring failure, so landing half a
			// batch and then refusing the rest leaves a scene the model never
			// asked for.  ONE geometry-creating element anywhere in the batch
			// refuses the WHOLE batch atomically, document byte-identical,
			// head unbumped, every element carrying the same verdict.  This is
			// also what makes the gate cost exactly ONE INTERCEPTION per call:
			// a model that batches its whole build into one insert_chunks is
			// intercepted once per call, not once per element -- so the
			// refuse-until-filed cap (kBuildPlanGateMaxRefusals) is spent at
			// the same rate a single-chunk caller spends it, never faster.
			//
			// `g2GiveUpNotice` is set IFF this call is the one that trips the
			// cap (see CheckBuildPlanGate_'s doc) and is folded into EVERY
			// result this call returns below -- the R1c-refused-batch return
			// right after this block, and each per-element InsertChunk result
			// in the SEQUENTIAL loop further down -- so the give-up is
			// visible in the payload regardless of which of those paths this
			// particular call ends up taking.
			std::string g2GiveUpNotice;
			{
				bool createsGeometry = false;
				if( BuildPlanGateArmed_() ) {
					for( std::size_t i = 0; i < chunkTexts.size() && !createsGeometry; ++i )
						createsGeometry = ChunkTextCreatesGeometry_( chunkTexts[i] );
				}
				if( createsGeometry ) {
					const std::string clause = CheckBuildPlanGate_( "insert_chunks", &g2GiveUpNotice );
					if( !clause.empty() ) {
						const RISE::Cst::CstHeadVersion head = ReadHeadVersion();
						for( std::size_t i = 0; i < chunkTexts.size(); ++i ) {
							AgentChunkResult e;
							e.applied     = false;
							e.retriable   = false;   // see InsertChunk's arm for why
							e.rawCode     = 0;
							e.status      = "rejected";
							e.headVersion = head;
							e.message     = clause;
							out.push_back( e );
						}
						return out;
					}
				}
			}

			// S1 (2026-08-11, the staged build protocol): the COMPOSE-phase
			// creation refusal, as its OWN up-front batch scan for exactly the
			// reason the gate's scan above is one -- landing half a batch and
			// then refusing the rest leaves a scene the model never asked for,
			// and one scan per CALL means the phase budget is spent at the same
			// rate by a batching caller as by a single-chunk one.  The
			// per-element InsertChunk below carries the identical check for the
			// direct caller; it cannot double-fire, because this scan already
			// refused the whole batch if any element would trip it.
			std::string s1GiveUpNotice;
			if( mBuildPhase == AgentBuildPhase::Compose ) {
				bool createsGeometry = false;
				for( std::size_t i = 0; i < chunkTexts.size() && !createsGeometry; ++i )
					createsGeometry = ChunkTextCreatesGeometry_( chunkTexts[i] );
				if( createsGeometry ) {
					const std::string clause = CheckComposePhaseForCreate_( "insert_chunks", &s1GiveUpNotice );
					if( !clause.empty() ) {
						const RISE::Cst::CstHeadVersion head = ReadHeadVersion();
						for( std::size_t i = 0; i < chunkTexts.size(); ++i ) {
							AgentChunkResult e;
							e.applied     = false;
							e.retriable   = false;   // see InsertChunk's arm for why
							e.rawCode     = 0;
							e.status      = "rejected";
							e.headVersion = head;
							e.message     = clause;
							// The gate above may have tripped its OWN give-up on
							// this same call; carry that notice too rather than
							// lose it behind this refusal.
							if( !g2GiveUpNotice.empty() ) e.message += "  " + g2GiveUpNotice;
							out.push_back( e );
						}
						return out;
					}
				}
			}

			// S2 (2026-08-11, clean-room construction): the FIRST-GEOMETRY
			// refusal, the third arm of the same phase counter, as its own
			// up-front batch scan for the identical half-a-batch reason.  It
			// shares s1GiveUpNotice with the compose arm above because the two
			// are the same counter -- they cannot both fire on one call (one
			// needs the Compose phase, the other the Pieces phase), so one
			// notice variable is enough and cannot lose an event.
			if( mBuildPhase == AgentBuildPhase::Pieces ) {
				bool createsGeometry = false;
				for( std::size_t i = 0; i < chunkTexts.size() && !createsGeometry; ++i )
					createsGeometry = ChunkTextCreatesGeometry_( chunkTexts[i] );
				if( createsGeometry ) {
					const std::string clause =
						CheckFirstGeometryThroughCleanRoom_( "insert_chunks", &s1GiveUpNotice );
					if( !clause.empty() ) {
						const RISE::Cst::CstHeadVersion head = ReadHeadVersion();
						for( std::size_t i = 0; i < chunkTexts.size(); ++i ) {
							AgentChunkResult e;
							e.applied     = false;
							e.retriable   = false;   // see InsertChunk's arm for why
							e.rawCode     = 0;
							e.status      = "rejected";
							e.headVersion = head;
							e.message     = clause;
							if( !g2GiveUpNotice.empty() ) e.message += "  " + g2GiveUpNotice;
							out.push_back( e );
						}
						return out;
					}
				}
			}

			// R1c (2026-08-09, agent rasterizer allowlist): an UP-FRONT scan of
			// EVERY element, before a single insert runs.  This is the one
			// place R1c deviates from InsertChunks' documented SEQUENTIAL,
			// BEST-EFFORT contract, deliberately: best-effort is right for a
			// per-element AUTHORING failure (a partially-landed additive batch
			// is a coherent, extendable state the model can reason about), but
			// a POLICY refusal is not an authoring failure -- landing the first
			// half of a batch and then refusing the integrator the rest of it
			// was written for leaves a scene the model never asked for.  So a
			// blocked rasterizer ANYWHERE in the batch refuses the WHOLE batch
			// atomically, with the document byte-identical and the head version
			// unbumped, matching R1a's all-or-nothing remove semantics.  Every
			// element reports the SAME verdict; the message names the offending
			// INDEX so the fix is one edit, not a hunt.
			{
				std::size_t offender = chunkTexts.size();
				std::string clause;
				for( std::size_t i = 0; i < chunkTexts.size(); ++i ) {
					clause = CheckRasterizerAllowlistGateForInsert( chunkTexts[i] );
					if( !clause.empty() ) { offender = i; break; }
				}
				if( offender < chunkTexts.size() ) {
					char buf[96];
					std::snprintf( buf, sizeof( buf ),
						"insert_chunks refused: NOTHING was inserted (chunks[%d]) -- ",
						static_cast<int>( offender ) );
					const RISE::Cst::CstHeadVersion head = ReadHeadVersion();
					for( std::size_t i = 0; i < chunkTexts.size(); ++i ) {
						AgentChunkResult e;
						e.applied     = false;
						e.rawCode     = 0;
						e.status      = "rejected";
						e.headVersion = head;
						e.message     = std::string( buf ) + clause;
						// G2: this call may have JUST tripped the give-up cap
						// (see the block above) even though it goes on to be
						// refused here for the UNRELATED rasterizer reason --
						// the give-up already happened and disarmed the gate
						// permanently, so report it here too rather than lose
						// it.
						if( !g2GiveUpNotice.empty() ) e.message += "  " + g2GiveUpNotice;
						// S1: same for the phase machinery's give-up.
						if( !s1GiveUpNotice.empty() ) e.message += "  " + s1GiveUpNotice;
						out.push_back( e );
					}
					return out;
				}
			}

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
				AgentChunkResult e = InsertChunk( chunkTexts[i], base );
				// G2: fold this call's give-up notice (if any) into every
				// element -- InsertChunk's OWN gate check sees the gate
				// already disarmed by the up-front scan above, so it never
				// writes a second notice; this is the one and only place it
				// is attached for a batched insert.
				if( !g2GiveUpNotice.empty() ) {
					if( !e.message.empty() ) e.message += "  ";
					e.message += g2GiveUpNotice;
				}
				// S1: the same treatment for the phase machinery's give-up --
				// the up-front compose scan above is the only place it can be
				// written for a batched insert (the per-element InsertChunk sees
				// the phase rules already disarmed), so this is where it rides.
				if( !s1GiveUpNotice.empty() ) {
					if( !e.message.empty() ) e.message += "  ";
					e.message += s1GiveUpNotice;
				}
				out.push_back( e );
			}

			return out;
		}

		namespace
		{
			//----------------------------------------------------------------
			// Arc-75 slice S2.1 (insert_material_scaffold): deterministic
			// jitter + the five family chunk-graph generators.  See
			// AgentSession::InsertMaterialScaffold's header doc for the
			// contract; this namespace holds pure text-generation helpers
			// with NO session/controller/lock interaction at all -- the
			// generated chunk texts are submitted through the EXISTING
			// InsertChunks path, so nothing here touches
			// SceneEditController::mMutex.
			//----------------------------------------------------------------

			//! FNV-1a 64-bit -- byte-stable across platforms/compilers/runs
			//! (unlike std::hash<std::string>, which the standard leaves
			//! implementation-defined).  The determinism red-proof (same
			//! `name` twice -> byte-identical chunk text) depends on this
			//! function NEVER changing for a given input.  Weak-avalanche
			//! caveat: FNV-1a's per-byte diffusion is modest, so two SHORT
			//! salts differing only in a trailing sequential digit (e.g.
			//! "axis1" vs "axis2") can correlate more than a stronger hash
			//! would -- every call site below therefore uses a distinctive
			//! WORD per knob ("wood_persist", "stone_axis", ...), never a
			//! numbered/sequential salt scheme, so this weakness is never
			//! actually exercised.
			std::uint64_t ScaffoldFnv1a64( const std::string& s )
			{
				std::uint64_t h = 14695981039346656037ull;
				for( unsigned char c : s ) { h ^= c; h *= 1099511628211ull; }
				return h;
			}

			//! Deterministic value in [0,1) from `name` + a per-knob `salt`
			//! string -- different salts decorrelate different knobs of the
			//! SAME scaffold; different `name`s decorrelate different
			//! scaffolds (two families with the same name still differ
			//! because each knob's salt is also family-specific text).
			double ScaffoldJitter01( const std::string& name, const char* salt )
			{
				const std::uint64_t h = ScaffoldFnv1a64( name + "|" + salt );
				// Top 53 bits -> a double in [0,1) (mirrors the standard
				// generate_canonical technique for a one-shot deterministic
				// draw, no distribution object needed).
				return static_cast<double>( ( h >> 11 ) & ( ( 1ull << 53 ) - 1 ) )
				     / static_cast<double>( 1ull << 53 );
			}
			double ScaffoldJitterRange( const std::string& name, const char* salt, double lo, double hi )
			{
				return lo + ScaffoldJitter01( name, salt ) * ( hi - lo );
			}
			unsigned int ScaffoldJitterUInt( const std::string& name, const char* salt, unsigned int lo, unsigned int hi )
			{
				return lo + static_cast<unsigned int>( ScaffoldJitter01( name, salt ) * static_cast<double>( hi - lo + 1 ) );
			}

			//! Fixed 4-decimal formatting -- deterministic text (no
			//! scientific notation at the magnitudes used here).
			std::string ScaffoldFmt( double v )
			{
				char buf[64];
				std::snprintf( buf, sizeof( buf ), "%.4f", v );
				return buf;
			}
			double ScaffoldClamp01( double v ) { return v < 0.0 ? 0.0 : ( v > 1.0 ? 1.0 : v ); }

			std::string ScaffoldChunkText( const char* keyword,
			                               const std::vector<std::pair<std::string,std::string>>& params )
			{
				std::string out = std::string( keyword ) + "\n{\n";
				for( const auto& kv : params ) out += kv.first + " " + kv.second + "\n";
				out += "}\n";
				return out;
			}

			std::string ScaffoldVec3( double a, double b, double c )
			{
				return ScaffoldFmt( a ) + " " + ScaffoldFmt( b ) + " " + ScaffoldFmt( c );
			}

			std::string ScaffoldUniformColorText( const std::string& name, double r, double g, double b )
			{
				return ScaffoldChunkText( "uniformcolor_painter", {
					{ "name",  name },
					{ "color", ScaffoldVec3( r, g, b ) },
				} );
			}

			//! A genuinely UV-varying [0,1] scalar field, defined directly
			//! as a math expression of u,v.  IMPORTANT -- this is the ONLY
			//! chunk kind this generator wraps in scalar_painter's
			//! `function2d` slot for a truly spatially-varying scalar.  The
			//! 3D-SOLID noise painters (perlin3d/worley3d/domainwarp3d/
			//! reactiondiffusion3d/...) dual-register into the SAME
			//! Function2D manager (Job::RegisterPainterDual), so
			//! scalar_painter{function2d `a_3d_noise_painter`} PARSES and
			//! RENDERS non-black -- but their GetColor reads
			//! ri.ptIntersection (world-space), which the base
			//! Painter::Evaluate(x,y)'s synthetic RayIntersectionGeometric
			//! (Painter.cpp) never populates (it sets only ptCoord), so
			//! that binding is silently evaluated at the SAME fixed point
			//! every time -- spatially CONSTANT, not varying.  Verified by
			//! reading Perlin3DPainter::GetColor / Painter::Evaluate /
			//! Function2DScalarPainter::GetValuesAt directly; not used here.
			//! expression_function2d has no such trap (its value IS u,v).
			std::string ScaffoldExprFunction2DText( const std::string& name, double freqU, double freqV, double phase )
			{
				std::string out = "expression_function2d\n{\n";
				out += "name " + name + "\n";
				out += "param freq_u " + ScaffoldFmt( freqU ) + "\n";
				out += "param freq_v " + ScaffoldFmt( freqV ) + "\n";
				out += "param ph " + ScaffoldFmt( phase ) + "\n";
				out += "expr 0.5 + 0.5 * sin(freq_u * u * 6.283185 + ph) * cos(freq_v * v * 6.283185 + ph)\n";
				out += "}\n";
				return out;
			}

			std::string ScaffoldScalarFn2DText( const std::string& name, const std::string& fn2d, double scale, double bias )
			{
				return ScaffoldChunkText( "scalar_painter", {
					{ "name",       name },
					{ "function2d", fn2d },
					{ "scale",      ScaffoldFmt( scale ) },
					{ "bias",       ScaffoldFmt( bias ) },
				} );
			}

			std::string ScaffoldDomainWarp3DText( const std::string& name, const std::string& colora, const std::string& colorb,
			                                      double persistence, unsigned int octaves, double warpAmp, unsigned int warpLevels,
			                                      double axisA, double axisB, double axisC, double shiftX, double shiftY, double shiftZ )
			{
				return ScaffoldChunkText( "domainwarp3d_painter", {
					{ "name", name }, { "colora", colora }, { "colorb", colorb },
					{ "persistence", ScaffoldFmt( persistence ) }, { "octaves", std::to_string( octaves ) },
					{ "warp_amplitude", ScaffoldFmt( warpAmp ) }, { "warp_levels", std::to_string( warpLevels ) },
					{ "scale", ScaffoldVec3( axisA, axisB, axisC ) },
					{ "shift", ScaffoldVec3( shiftX, shiftY, shiftZ ) },
				} );
			}

			std::string ScaffoldWorley3DText( const std::string& name, const std::string& colora, const std::string& colorb,
			                                  double jitter, const std::string& output,
			                                  double axis, double shiftX, double shiftY, double shiftZ )
			{
				return ScaffoldChunkText( "worley3d_painter", {
					{ "name", name }, { "colora", colora }, { "colorb", colorb },
					{ "jitter", ScaffoldFmt( jitter ) }, { "output", output },
					{ "scale", ScaffoldVec3( axis, axis, axis ) },
					{ "shift", ScaffoldVec3( shiftX, shiftY, shiftZ ) },
				} );
			}

			std::string ScaffoldReactionDiffusion3DText( const std::string& name, const std::string& colora, const std::string& colorb,
			                                             unsigned int gridSize, double feed, double kill, unsigned int iterations,
			                                             double axis, double shiftX, double shiftY, double shiftZ )
			{
				return ScaffoldChunkText( "reactiondiffusion3d_painter", {
					{ "name", name }, { "colora", colora }, { "colorb", colorb },
					{ "grid_size", std::to_string( gridSize ) },
					{ "feed", ScaffoldFmt( feed ) }, { "kill", ScaffoldFmt( kill ) },
					{ "iterations", std::to_string( iterations ) },
					{ "scale", ScaffoldVec3( axis, axis, axis ) },
					{ "shift", ScaffoldVec3( shiftX, shiftY, shiftZ ) },
				} );
			}

			std::string ScaffoldPbrMetallicRoughnessText( const std::string& name, const std::string& baseColor,
			                                              const std::string& roughness, double metallic )
			{
				return ScaffoldChunkText( "pbr_metallic_roughness_material", {
					{ "name", name }, { "base_color", baseColor },
					{ "metallic", ScaffoldFmt( metallic ) }, { "roughness", roughness },
				} );
			}

			std::string ScaffoldCookTorranceText( const std::string& name, const std::string& rd, const std::string& rs,
			                                      const std::string& facets )
			{
				return ScaffoldChunkText( "cooktorrance_material", {
					{ "name", name }, { "rd", rd }, { "rs", rs }, { "facets", facets },
				} );
			}

			std::string ScaffoldWardAnisotropicText( const std::string& name, const std::string& rd, const std::string& rs,
			                                         const std::string& alphax, const std::string& alphay )
			{
				return ScaffoldChunkText( "ward_anisotropic_material", {
					{ "name", name }, { "rd", rd }, { "rs", rs },
					{ "alphax", alphax }, { "alphay", alphay },
				} );
			}

			std::string ScaffoldGGXText( const std::string& name, const std::string& rd, const std::string& rs,
			                            const std::string& alphax, const std::string& alphay, const std::string& fresnelMode )
			{
				return ScaffoldChunkText( "ggx_material", {
					{ "name", name }, { "rd", rd }, { "rs", rs },
					{ "alphax", alphax }, { "alphay", alphay },
					{ "fresnel_mode", fresnelMode },
				} );
			}

			//! `tone` is "r g b", each 0..1 -- parsed strictly (no trailing
			//! junk beyond whitespace) so a malformed value is refused
			//! rather than silently truncated.
			bool ScaffoldParseTone( const std::string& tone, double& r, double& g, double& b )
			{
				char trailing[8] = { 0 };
				if( std::sscanf( tone.c_str(), "%lf %lf %lf %7s", &r, &g, &b, trailing ) != 3 ) return false;
				if( !std::isfinite( r ) || !std::isfinite( g ) || !std::isfinite( b ) ) return false;
				if( r < 0.0 || r > 1.0 || g < 0.0 || g > 1.0 || b < 0.0 || b > 1.0 ) return false;
				return true;
			}

			//! Arc-75 S2.1 fix-round P3: a sane upper bound on `name`'s
			//! length.  Nothing downstream is unsafe past this (every
			//! generated chunk name is a plain `std::string` concatenation,
			//! no fixed buffer involved) -- this is purely a SANITY cap
			//! against a pathological caller-supplied prefix bloating every
			//! generated chunk name (`tmpl_<name>_<role>`) past what any
			//! honest scene-authoring `name` should ever need.  64 is
			//! generous against every real chunk name in this codebase's
			//! own scenes/tests.
			const std::size_t kScaffoldMaxNameLength = 64;

			//! `name` is embedded directly into generated `name <token>`
			//! lines and `tmpl_<name>_<role>` chunk names -- restrict to
			//! characters that can never split a line or open/close a brace
			//! early (the same token-safety policy every other chunk name
			//! in this file relies on implicitly), and cap the length
			//! (kScaffoldMaxNameLength) against a pathological caller.
			bool ScaffoldNameIsValid( const std::string& name )
			{
				if( name.empty() || name.size() > kScaffoldMaxNameLength ) return false;
				for( unsigned char c : name ) {
					if( !( std::isalnum( c ) || c == '_' || c == '-' ) ) return false;
				}
				return true;
			}

			//! One generated chunk: its kind (for the collision precheck),
			//! its own chunk `name` (ditto), and its full chunk text.
			struct ScaffoldChunkEntry
			{
				std::string kind;
				std::string name;
				std::string text;
			};

			//! The whole expansion for one family: every chunk in insertion
			//! order, plus the factual material/bound-slot summary
			//! InsertMaterialScaffold's result reports.
			struct ScaffoldGraph
			{
				std::vector<ScaffoldChunkEntry> chunks;
				std::string materialName;
				std::string materialKind;
				std::vector<std::pair<std::string,std::string>> boundSlots;   //!< (param, painterName)
			};

			//! weathered_wood: pbr_metallic_roughness_material, base_color
			//! AND roughness both bound to the SAME domainwarp3d grain
			//! painter (a real wood ridge is both darker-and-rougher at the
			//! same grain lines, so sharing one painter across both slots
			//! is the honest choice, not just the cheap one).  `wear` warps
			//! the grain harder and darkens the low end more; `scale`
			//! stretches the grain frequency (anisotropic per-axis, for a
			//! grain direction rather than blobs).
			ScaffoldGraph BuildWeatheredWood( const std::string& name, double r, double g, double b, double wear, double scale )
			{
				ScaffoldGraph out;
				const std::string nLight = "tmpl_" + name + "_tonelight";
				const std::string nDark  = "tmpl_" + name + "_tonedark";
				const std::string nGrain = "tmpl_" + name + "_grain";
				const std::string nMat   = "tmpl_" + name + "_mat";

				const double darkFactor = ScaffoldClamp01(
					ScaffoldJitterRange( name, "wood_dark", 0.35, 0.60 ) - wear * 0.15 );
				out.chunks.push_back( { "uniformcolor_painter", nLight, ScaffoldUniformColorText( nLight, r, g, b ) } );
				out.chunks.push_back( { "uniformcolor_painter", nDark,
					ScaffoldUniformColorText( nDark, r * darkFactor, g * darkFactor, b * darkFactor ) } );

				const double persistence  = ScaffoldJitterRange( name, "wood_persist", 0.50, 0.80 );
				const unsigned int octaves = ScaffoldJitterUInt( name, "wood_octaves", 3, 5 );
				const double warpAmp      = ScaffoldJitterRange( name, "wood_warpamp", 2.0, 6.0 ) * ( 0.5 + wear );
				const unsigned int warpLevels = ScaffoldJitterUInt( name, "wood_warplevels", 1, 3 );
				const double axisA = ScaffoldJitterRange( name, "wood_axisa", 0.6, 1.4 ) * scale;
				const double axisB = ScaffoldJitterRange( name, "wood_axisb", 2.0, 4.0 ) * scale;
				const double shiftX = ScaffoldJitterRange( name, "wood_shiftx", 0.0, 100.0 );
				const double shiftY = ScaffoldJitterRange( name, "wood_shifty", 0.0, 100.0 );
				const double shiftZ = ScaffoldJitterRange( name, "wood_shiftz", 0.0, 100.0 );
				out.chunks.push_back( { "domainwarp3d_painter", nGrain,
					ScaffoldDomainWarp3DText( nGrain, nDark, nLight, persistence, octaves, warpAmp, warpLevels,
						axisA, axisB, axisA, shiftX, shiftY, shiftZ ) } );

				out.chunks.push_back( { "pbr_metallic_roughness_material", nMat,
					ScaffoldPbrMetallicRoughnessText( nMat, nGrain, nGrain, 0.0 ) } );
				out.materialName = nMat;
				out.materialKind = "pbr_metallic_roughness_material";
				out.boundSlots.push_back( { "roughness", nGrain } );
				out.boundSlots.push_back( { "base_color", nGrain } );
				return out;
			}

			//! rough_stone: cooktorrance_material, rd bound to a worley3d
			//! pebble/cell field (colora=tone, colorb="none"), facets bound
			//! to scalar_painter{function2d} over a jittered
			//! expression_function2d.  `wear` widens and raises the facet
			//! band; `scale` sets the pebble frequency.
			ScaffoldGraph BuildRoughStone( const std::string& name, double r, double g, double b, double wear, double scale )
			{
				ScaffoldGraph out;
				const std::string nTone   = "tmpl_" + name + "_tone";
				const std::string nPebble = "tmpl_" + name + "_pebble";
				const std::string nWear   = "tmpl_" + name + "_wearfield";
				const std::string nFacets = "tmpl_" + name + "_facets";
				const std::string nMat    = "tmpl_" + name + "_mat";

				out.chunks.push_back( { "uniformcolor_painter", nTone, ScaffoldUniformColorText( nTone, r, g, b ) } );

				const double jitterAmt = ScaffoldJitterRange( name, "stone_jitter", 0.6, 1.0 );
				const std::string output = ( ScaffoldJitter01( name, "stone_output" ) < 0.5 ) ? "f1" : "f2-f1";
				const double axis  = ScaffoldJitterRange( name, "stone_axis", 3.0, 8.0 ) * scale;
				const double shiftX = ScaffoldJitterRange( name, "stone_shiftx", 0.0, 100.0 );
				const double shiftY = ScaffoldJitterRange( name, "stone_shifty", 0.0, 100.0 );
				const double shiftZ = ScaffoldJitterRange( name, "stone_shiftz", 0.0, 100.0 );
				out.chunks.push_back( { "worley3d_painter", nPebble,
					ScaffoldWorley3DText( nPebble, nTone, "none", jitterAmt, output, axis, shiftX, shiftY, shiftZ ) } );

				const double freqU = ScaffoldJitterRange( name, "stone_frequ", 6.0, 14.0 );
				const double freqV = ScaffoldJitterRange( name, "stone_freqv", 6.0, 14.0 );
				const double phase = ScaffoldJitterRange( name, "stone_phase", 0.0, 6.283185 );
				out.chunks.push_back( { "expression_function2d", nWear,
					ScaffoldExprFunction2DText( nWear, freqU, freqV, phase ) } );
				out.chunks.push_back( { "scalar_painter", nFacets,
					ScaffoldScalarFn2DText( nFacets, nWear, 0.05 + 0.30 * wear, 0.04 + 0.05 * wear ) } );

				out.chunks.push_back( { "cooktorrance_material", nMat,
					ScaffoldCookTorranceText( nMat, nPebble, "none", nFacets ) } );
				out.materialName = nMat;
				out.materialKind = "cooktorrance_material";
				out.boundSlots.push_back( { "facets", nFacets } );
				out.boundSlots.push_back( { "rd", nPebble } );
				return out;
			}

			//! brushed_metal: ward_anisotropic_material, alphax (narrow,
			//! along the brush direction) AND alphay (wide, across it) both
			//! bound to scalar_painter{function2d} wrapping the SAME
			//! groove expression_function2d at different scale/bias --
			//! the anisotropy IS the two bands reading a shared groove
			//! field differently, not two unrelated noises.  `wear` widens
			//! both bands (a more-worn brushed surface scatters more in
			//! both directions); `scale` sets the groove pitch.
			ScaffoldGraph BuildBrushedMetal( const std::string& name, double r, double g, double b, double wear, double scale )
			{
				ScaffoldGraph out;
				const std::string nTint   = "tmpl_" + name + "_tint";
				const std::string nGroove = "tmpl_" + name + "_groove";
				const std::string nAlphaX = "tmpl_" + name + "_alphax";
				const std::string nAlphaY = "tmpl_" + name + "_alphay";
				const std::string nMat    = "tmpl_" + name + "_mat";

				out.chunks.push_back( { "uniformcolor_painter", nTint, ScaffoldUniformColorText( nTint, r, g, b ) } );

				const double freqU = ScaffoldJitterRange( name, "metal_frequ", 20.0, 40.0 ) * scale;
				const double freqV = ScaffoldJitterRange( name, "metal_freqv", 1.0, 3.0 ) * scale;
				const double phase = ScaffoldJitterRange( name, "metal_phase", 0.0, 6.283185 );
				out.chunks.push_back( { "expression_function2d", nGroove,
					ScaffoldExprFunction2DText( nGroove, freqU, freqV, phase ) } );

				out.chunks.push_back( { "scalar_painter", nAlphaX,
					ScaffoldScalarFn2DText( nAlphaX, nGroove, 0.01 + 0.02 * wear, 0.015 + 0.01 * wear ) } );
				out.chunks.push_back( { "scalar_painter", nAlphaY,
					ScaffoldScalarFn2DText( nAlphaY, nGroove, 0.05 + 0.25 * wear, 0.10 + 0.10 * wear ) } );

				out.chunks.push_back( { "ward_anisotropic_material", nMat,
					ScaffoldWardAnisotropicText( nMat, "none", nTint, nAlphaX, nAlphaY ) } );
				out.materialName = nMat;
				out.materialKind = "ward_anisotropic_material";
				out.boundSlots.push_back( { "alphax", nAlphaX } );
				out.boundSlots.push_back( { "alphay", nAlphaY } );
				return out;
			}

			//! aged_bronze: cooktorrance_material, rd bound to a
			//! reactiondiffusion3d patina field (its OWN description names
			//! "oxidation blooms" -- the honest fit for bronze patina,
			//! deliberately distinct from rough_stone's worley so the two
			//! families don't read as the same recipe in different paint),
			//! rs reuses the base tone (a warm specular tint), facets bound
			//! to scalar_painter{function2d} like rough_stone.  `wear`
			//! raises the facet band (more pitting); `scale` sets the
			//! patina blotch frequency.
			ScaffoldGraph BuildAgedBronze( const std::string& name, double r, double g, double b, double wear, double scale )
			{
				ScaffoldGraph out;
				const std::string nTone   = "tmpl_" + name + "_tone";
				const std::string nPatina = "tmpl_" + name + "_patina";
				const std::string nWear   = "tmpl_" + name + "_wearfield";
				const std::string nFacets = "tmpl_" + name + "_facets";
				const std::string nMat    = "tmpl_" + name + "_mat";

				out.chunks.push_back( { "uniformcolor_painter", nTone, ScaffoldUniformColorText( nTone, r, g, b ) } );

				const unsigned int gridSize   = ScaffoldJitterUInt( name, "bronze_grid", 14, 20 );
				const double feed             = ScaffoldJitterRange( name, "bronze_feed", 0.030, 0.045 );
				const double kill             = ScaffoldJitterRange( name, "bronze_kill", 0.055, 0.065 );
				const unsigned int iterations = ScaffoldJitterUInt( name, "bronze_iter", 300, 700 );
				const double axis  = ScaffoldJitterRange( name, "bronze_axis", 2.0, 5.0 ) * scale;
				const double shiftX = ScaffoldJitterRange( name, "bronze_shiftx", 0.0, 100.0 );
				const double shiftY = ScaffoldJitterRange( name, "bronze_shifty", 0.0, 100.0 );
				const double shiftZ = ScaffoldJitterRange( name, "bronze_shiftz", 0.0, 100.0 );
				out.chunks.push_back( { "reactiondiffusion3d_painter", nPatina,
					ScaffoldReactionDiffusion3DText( nPatina, nTone, "none", gridSize, feed, kill, iterations,
						axis, shiftX, shiftY, shiftZ ) } );

				const double freqU = ScaffoldJitterRange( name, "bronze_frequ", 6.0, 14.0 );
				const double freqV = ScaffoldJitterRange( name, "bronze_freqv", 6.0, 14.0 );
				const double phase = ScaffoldJitterRange( name, "bronze_phase", 0.0, 6.283185 );
				out.chunks.push_back( { "expression_function2d", nWear,
					ScaffoldExprFunction2DText( nWear, freqU, freqV, phase ) } );
				out.chunks.push_back( { "scalar_painter", nFacets,
					ScaffoldScalarFn2DText( nFacets, nWear, 0.03 + 0.20 * wear, 0.03 + 0.04 * wear ) } );

				out.chunks.push_back( { "cooktorrance_material", nMat,
					ScaffoldCookTorranceText( nMat, nPatina, nTone, nFacets ) } );
				out.materialName = nMat;
				out.materialKind = "cooktorrance_material";
				out.boundSlots.push_back( { "facets", nFacets } );
				out.boundSlots.push_back( { "rd", nPatina } );
				return out;
			}

			//! glazed_ceramic: ggx_material, fresnel_mode schlick_f0 (a
			//! dielectric glaze, not a conductor), rd is the body colour,
			//! rs is a small F0 tint (the glaze coat), alphax AND alphay
			//! both bound to the SAME LOW-amplitude scalar_painter{function2d}
			//! (isotropic, "low-alpha with subtle scalar variation" per the
			//! family brief).  `wear` nudges the alpha band up slightly;
			//! `scale` sets the ripple frequency.
			ScaffoldGraph BuildGlazedCeramic( const std::string& name, double r, double g, double b, double wear, double scale )
			{
				ScaffoldGraph out;
				const std::string nBody      = "tmpl_" + name + "_body";
				const std::string nGlaze     = "tmpl_" + name + "_glaze";
				const std::string nVariation = "tmpl_" + name + "_variation";
				const std::string nAlpha     = "tmpl_" + name + "_alpha";
				const std::string nMat       = "tmpl_" + name + "_mat";

				out.chunks.push_back( { "uniformcolor_painter", nBody, ScaffoldUniformColorText( nBody, r, g, b ) } );

				const double f0 = ScaffoldJitterRange( name, "ceramic_f0", 0.04, 0.06 );
				out.chunks.push_back( { "uniformcolor_painter", nGlaze,
					ScaffoldUniformColorText( nGlaze, ScaffoldClamp01( f0 + 0.02 * r ),
						ScaffoldClamp01( f0 + 0.02 * g ), ScaffoldClamp01( f0 + 0.02 * b ) ) } );

				const double freqU = ScaffoldJitterRange( name, "ceramic_frequ", 8.0, 16.0 ) * scale;
				const double freqV = ScaffoldJitterRange( name, "ceramic_freqv", 8.0, 16.0 ) * scale;
				const double phase = ScaffoldJitterRange( name, "ceramic_phase", 0.0, 6.283185 );
				out.chunks.push_back( { "expression_function2d", nVariation,
					ScaffoldExprFunction2DText( nVariation, freqU, freqV, phase ) } );
				out.chunks.push_back( { "scalar_painter", nAlpha,
					ScaffoldScalarFn2DText( nAlpha, nVariation, 0.005 + 0.015 * wear, 0.01 + 0.01 * wear ) } );

				out.chunks.push_back( { "ggx_material", nMat,
					ScaffoldGGXText( nMat, nBody, nGlaze, nAlpha, nAlpha, "schlick_f0" ) } );
				out.materialName = nMat;
				out.materialKind = "ggx_material";
				out.boundSlots.push_back( { "alphax", nAlpha } );
				out.boundSlots.push_back( { "alphay", nAlpha } );
				return out;
			}

			bool ScaffoldParseFamily( const std::string& family, AgentSession::MaterialScaffoldFamily& out )
			{
				if( family == "weathered_wood" ) { out = AgentSession::MaterialScaffoldFamily::WeatheredWood; return true; }
				if( family == "rough_stone" )    { out = AgentSession::MaterialScaffoldFamily::RoughStone;    return true; }
				if( family == "brushed_metal" )  { out = AgentSession::MaterialScaffoldFamily::BrushedMetal;  return true; }
				if( family == "aged_bronze" )    { out = AgentSession::MaterialScaffoldFamily::AgedBronze;    return true; }
				if( family == "glazed_ceramic" ) { out = AgentSession::MaterialScaffoldFamily::GlazedCeramic; return true; }
				return false;
			}

			ScaffoldGraph BuildScaffoldGraph( AgentSession::MaterialScaffoldFamily family, const std::string& name,
			                                  double r, double g, double b, double wear, double scale )
			{
				switch( family )
				{
					case AgentSession::MaterialScaffoldFamily::WeatheredWood:  return BuildWeatheredWood( name, r, g, b, wear, scale );
					case AgentSession::MaterialScaffoldFamily::RoughStone:     return BuildRoughStone( name, r, g, b, wear, scale );
					case AgentSession::MaterialScaffoldFamily::BrushedMetal:   return BuildBrushedMetal( name, r, g, b, wear, scale );
					case AgentSession::MaterialScaffoldFamily::AgedBronze:     return BuildAgedBronze( name, r, g, b, wear, scale );
					case AgentSession::MaterialScaffoldFamily::GlazedCeramic:  return BuildGlazedCeramic( name, r, g, b, wear, scale );
				}
				return ScaffoldGraph();
			}
		}

		AgentSession::AgentScaffoldResult AgentSession::InsertMaterialScaffold(
			const std::string& family, const std::string& name, const std::string& tone,
			double wear, double scale, const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			AgentScaffoldResult out;
			out.family = family;

			MaterialScaffoldFamily fam;
			if( !ScaffoldParseFamily( family, fam ) ) {
				out.ok = false;
				out.message = "insert_material_scaffold refused: unknown family `" + family +
					"` -- valid families are: weathered_wood, rough_stone, brushed_metal, aged_bronze, glazed_ceramic";
				return out;
			}
			if( !ScaffoldNameIsValid( name ) ) {
				out.ok = false;
				out.message = "insert_material_scaffold refused: `name` must be a non-empty token "
					"(letters, digits, underscore, hyphen only) -- got `" + name + "`";
				return out;
			}
			double r = 0.0, g = 0.0, b = 0.0;
			if( !ScaffoldParseTone( tone, r, g, b ) ) {
				out.ok = false;
				out.message = "insert_material_scaffold refused: `tone` must be three numbers `r g b`, "
					"each in [0,1] -- got `" + tone + "`";
				return out;
			}
			if( !std::isfinite( wear ) || wear < 0.0 || wear > 1.0 ) {
				out.ok = false;
				out.message = "insert_material_scaffold refused: `wear` must be a finite number in [0,1]";
				return out;
			}
			if( !std::isfinite( scale ) || scale <= 0.0 ) {
				out.ok = false;
				out.message = "insert_material_scaffold refused: `scale` must be a finite number > 0";
				return out;
			}

			const ScaffoldGraph graph = BuildScaffoldGraph( fam, name, r, g, b, wear, scale );

			// Collision precheck: refuse the WHOLE expansion, document
			// UNCHANGED, if ANY generated (kind,name) already exists AT
			// SNAPSHOT TIME.  InsertChunks' own duplicate rejection is
			// per-ELEMENT and best-effort by design (see its header doc)
			// -- a mid-batch collision would still land the chunks before
			// and after it, leaving a half-wired graph -- so without this
			// precheck a caller reusing a colliding `name` could end up
			// with a partial scaffold instead of a clean refusal.
			//
			// Arc-75 S2.1 fix-round P2c (precision correction -- the prior
			// comment here overclaimed "atomic: either the whole graph
			// lands, or nothing does", which AgentScaffoldResult's OWN doc
			// already hedges more carefully).  What this precheck DOES
			// guarantee: ok==false refuses BEFORE any chunk is generated,
			// so a genuine collision (the common case: replaying the same
			// `name`) never partially lands.  What it does NOT guarantee:
			//   * TOCTOU -- there is a real window between this snapshot
			//     read and the InsertChunks submission below.  A
			//     CONCURRENT live writer (another session, or the
			//     interactive editor, mutating the SAME controller's
			//     document) that inserts a colliding chunk inside that
			//     window is invisible to this precheck; the collision then
			//     surfaces as InsertChunks' own ordinary per-element
			//     "rejected" result for THAT chunk -- best-effort, not a
			//     whole-batch rollback (see InsertChunks' header doc).
			//   * Once the precheck passes, everything downstream is
			//     InsertChunks' EXISTING best-effort semantics, unchanged:
			//     a LATER element can still be rejected for an unrelated
			//     reason (e.g. a concurrent edit invalidating an earlier-
			//     landed reference), leaving a partially-applied graph on
			//     the document -- callers must check every `chunkResults[i]`
			//     rather than trusting ok==true as "the whole graph
			//     landed" (see AgentScaffoldResult's own doc).
			//   * Optimistic concurrency (`baseOrNull`) is honored exactly
			//     as InsertChunks documents it: checked against the FIRST
			//     element only: a stale head surfaces as that element's
			//     own "conflict" status, not a distinct scaffold-level
			//     guarantee.
			//   * A chunk that APPLIES but references a name the document
			//     has no definition for yet is a non-blocking WARNING
			//     (`issues`), never a failure -- same AttachChunkIssueWarnings
			//     contract InsertChunk/InsertChunks already carry.
			{
				const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
				if( snap.hasDocument ) {
					const RISE::Cst::Document headDoc = RISE::Cst::ParseToCst( snap.document );
					for( const ScaffoldChunkEntry& c : graph.chunks ) {
						const RISE::Cst::NodeId id = RISE::Cst::DocFindByName( headDoc, c.kind + "/" + c.name );
						if( id != 0 ) {
							out.ok = false;
							out.message = "insert_material_scaffold refused: a `" + c.kind + "` named `" + c.name +
								"` already exists -- every generated chunk is named tmpl_<name>_<role>, "
								"and this collides; pick a different `name` -- document unchanged";
							return out;
						}
					}
				}
			}

			std::vector<std::string> chunkTexts;
			chunkTexts.reserve( graph.chunks.size() );
			for( const ScaffoldChunkEntry& c : graph.chunks ) chunkTexts.push_back( c.text );

			out.chunkResults = InsertChunks( chunkTexts, baseOrNull );
			out.ok           = true;
			out.materialName = graph.materialName;
			out.materialKind = graph.materialKind;
			out.boundSlots   = graph.boundSlots;
			return out;
		}

		namespace
		{
			//----------------------------------------------------------------
			// Arc-75 slice S3b (insert_geometry_scaffold): the geometry
			// sibling of the material-scaffold block above.  Reuses
			// ScaffoldFnv1a64/ScaffoldJitter01/ScaffoldJitterRange/
			// ScaffoldJitterUInt/ScaffoldFmt/ScaffoldClamp01/ScaffoldVec3/
			// ScaffoldChunkText/ScaffoldNameIsValid/kScaffoldMaxNameLength
			// VERBATIM (defined above, same translation unit, same
			// anonymous namespace linkage) -- do NOT duplicate them here,
			// per the S2.1 avalanche-caveat lesson: a second copy is a
			// second thing that can drift out of determinism-lockstep.
			//
			// DECOY-LANDMINE NOTE (S2.1's design landmine, checked and
			// found NOT to apply here): S2.1's decoy was 3D-SOLID noise
			// painters (perlin3d/worley3d/...) silently evaluating at a
			// FIXED point when wrapped in scalar_painter{function2d},
			// because their GetColor reads world-space ptIntersection,
			// which the scalar-pipe's synthetic RayIntersectionGeometric
			// never populates.  displaced_slab's noise source is
			// perlin2d_painter bound DIRECTLY to displaced_geometry's
			// `displacement` slot -- no scalar_painter/expression_function2d
			// wrapper anywhere in this family.  Job::AddDisplacedGeometry
			// (src/Library/Job.cpp ~5316-5352) resolves `displacement`
			// through `pFunc2DManager->GetItem(...)` -- the SAME
			// Function2D manager perlin2d_painter dual-registers into
			// (Job::AddPerlin2DPainter) -- and DisplacedGeometry evaluates
			// it as a genuine IFunction2D at each tessellated vertex's
			// (u,v), exactly the domain perlin2d's own GetValue(u,v) is
			// defined over.  There is no fixed-point-evaluation trap here
			// because there is no colour-pipe GetColor(ri) call anywhere
			// on this path -- unlike scalar_painter{function2d}, which
			// wraps a painter's colour-pipe accessor and can therefore
			// land on the WRONG accessor for a 3D-solid painter.  The
			// spatial-effect test (see GS1b in AgentChunkCrudTest.cpp)
			// still pins this DIRECTLY (bbox height-extent of the
			// resolved DisplacedGeometry vs the flat base box), rather
			// than trusting this argument alone.
			//----------------------------------------------------------------

			bool ScaffoldParseGeometryFamily( const std::string& family, AgentSession::GeometryScaffoldFamily& out )
			{
				if( family == "displaced_slab" )  { out = AgentSession::GeometryScaffoldFamily::DisplacedSlab;  return true; }
				if( family == "sweep_rail" )      { out = AgentSession::GeometryScaffoldFamily::SweepRail;      return true; }
				if( family == "blended_vessel" )  { out = AgentSession::GeometryScaffoldFamily::BlendedVessel;  return true; }
				if( family == "sdf_column" )      { out = AgentSession::GeometryScaffoldFamily::SdfColumn;      return true; }
				if( family == "blended_chain" )   { out = AgentSession::GeometryScaffoldFamily::BlendedChain;   return true; }
				if( family == "volume_bank" )     { out = AgentSession::GeometryScaffoldFamily::VolumeBank;     return true; }
				return false;
			}

			//! Parses `raw` as 2-6 semicolon-separated "x y z" triplets (each
			//! of the three numbers finite) -- the wire grammar `points`
			//! REQUIRES for blended_chain.  Returns false with an actionable
			//! `err` on ANY malformed input: wrong triplet count (<2 or >6),
			//! a triplet that isn't exactly three whitespace-separated
			//! numbers, or a non-finite number (NaN/inf).  A single leading
			//! or trailing pure-whitespace segment (from a stray leading/
			//! trailing `;`) is tolerated and dropped; any OTHER blank or
			//! malformed segment (a doubled `;;`, a triplet missing a
			//! coordinate) fails the "exactly three numbers" check below
			//! rather than being silently skipped, so a typo'd separator
			//! is caught instead of quietly changing the point count.
			bool ScaffoldParsePoints( const std::string& raw, std::vector<std::array<double,3>>& out, std::string& err )
			{
				out.clear();
				std::vector<std::string> segs;
				{
					std::string cur;
					for( char ch : raw ) {
						if( ch == ';' ) { segs.push_back( cur ); cur.clear(); }
						else cur += ch;
					}
					segs.push_back( cur );
				}
				auto isBlank = []( const std::string& s ) {
					for( unsigned char c : s ) { if( !std::isspace( c ) ) return false; }
					return true;
				};
				if( !segs.empty() && isBlank( segs.front() ) ) segs.erase( segs.begin() );
				if( !segs.empty() && isBlank( segs.back() ) )  segs.pop_back();

				if( segs.size() < 2 || segs.size() > 6 ) {
					err = "`points` must have 2-6 semicolon-separated \"x y z\" triplets -- got " +
						std::to_string( segs.size() );
					return false;
				}

				for( std::size_t i = 0; i < segs.size(); ++i ) {
					double x = 0.0, y = 0.0, z = 0.0;
					char trailing[8] = { 0 };
					const int n = std::sscanf( segs[i].c_str(), " %lf %lf %lf %7s", &x, &y, &z, trailing );
					if( n != 3 ) {
						err = "`points` triplet " + std::to_string( i + 1 ) + " (\"" + segs[i] +
							"\") must be exactly three numbers \"x y z\"";
						return false;
					}
					if( !std::isfinite( x ) || !std::isfinite( y ) || !std::isfinite( z ) ) {
						err = "`points` triplet " + std::to_string( i + 1 ) + " (\"" + segs[i] +
							"\") must be finite (no NaN/inf)";
						return false;
					}
					// P2 fix-round: a sane magnitude cap.  Nothing downstream
					// is UNSAFE past this (every coordinate is a plain
					// double flowing into text formatting and arithmetic,
					// no fixed buffer) -- this guards against a
					// pathological coordinate silently producing a
					// degenerate/absurdly-scaled chain (a path point at
					// 1e300 would swamp every other point's contribution to
					// the Catmull-Rom curve and the node-count/spacing
					// heuristics above).  1e6 is generous against any real
					// scene's world-space units (RISE scenes are typically
					// authored in the 0.1-1000 range).
					constexpr double kScaffoldPointsMaxCoord = 1e6;
					if( std::fabs( x ) > kScaffoldPointsMaxCoord || std::fabs( y ) > kScaffoldPointsMaxCoord ||
					    std::fabs( z ) > kScaffoldPointsMaxCoord ) {
						err = "`points` triplet " + std::to_string( i + 1 ) + " (\"" + segs[i] +
							"\") has a coordinate magnitude past the " + std::to_string( static_cast<long long>( kScaffoldPointsMaxCoord ) ) +
							" sane bound";
						return false;
					}
					out.push_back( { x, y, z } );
				}
				return true;
			}

			//! One component of a clamped Catmull-Rom spline through `ctrl`
			//! (2-6 points): the standard 4-point basis, with the endpoints
			//! DUPLICATED at each boundary (a virtual P[-1]:=P[0], virtual
			//! P[m]:=P[m-1]) so the curve is clamped rather than looped, and
			//! passes through EVERY control point exactly at its own segment
			//! boundary (q(local t=0) == p1 exactly) -- the property
			//! BuildBlendedChain relies on to land its first/last node
			//! EXACTLY on the model-authored first/last `points` entry (the
			//! documented attachment mechanism: end a chain precisely at
			//! another part's surface by naming that surface's coordinate).
			double ScaffoldCatmullRom1D( double p0, double p1, double p2, double p3, double t )
			{
				const double t2 = t * t;
				const double t3 = t2 * t;
				return 0.5 * ( 2.0 * p1 + ( -p0 + p2 ) * t
				             + ( 2.0*p0 - 5.0*p1 + 4.0*p2 - p3 ) * t2
				             + ( -p0 + 3.0*p1 - 3.0*p2 + p3 ) * t3 );
			}

			//! Evaluates the clamped Catmull-Rom path through `ctrl` at
			//! global parameter `u` in [0,1] (0 = first point, 1 = last
			//! point) -- componentwise ScaffoldCatmullRom1D on x/y/z.
			std::array<double,3> ScaffoldCatmullRomPath( const std::vector<std::array<double,3>>& ctrl, double u )
			{
				const int m = static_cast<int>( ctrl.size() );
				if( m <= 1 ) return ctrl.empty() ? std::array<double,3>{0.0,0.0,0.0} : ctrl[0];
				double segF = ScaffoldClamp01( u ) * static_cast<double>( m - 1 );
				int segIdx = static_cast<int>( segF );
				if( segIdx > m - 2 ) segIdx = m - 2;
				if( segIdx < 0 ) segIdx = 0;
				const double t = segF - static_cast<double>( segIdx );

				const int i0 = ( segIdx - 1 < 0 ) ? 0 : segIdx - 1;
				const int i1 = segIdx;
				const int i2 = segIdx + 1;
				const int i3 = ( segIdx + 2 > m - 1 ) ? m - 1 : segIdx + 2;

				std::array<double,3> out;
				for( int k = 0; k < 3; ++k ) {
					out[static_cast<std::size_t>(k)] = ScaffoldCatmullRom1D(
						ctrl[static_cast<std::size_t>(i0)][static_cast<std::size_t>(k)],
						ctrl[static_cast<std::size_t>(i1)][static_cast<std::size_t>(k)],
						ctrl[static_cast<std::size_t>(i2)][static_cast<std::size_t>(k)],
						ctrl[static_cast<std::size_t>(i3)][static_cast<std::size_t>(k)], t );
				}
				return out;
			}

			std::string ScaffoldBoxGeometryText( const std::string& name, double width, double height, double depth )
			{
				return ScaffoldChunkText( "box_geometry", {
					{ "name",   name },
					{ "width",  ScaffoldFmt( width ) },
					{ "height", ScaffoldFmt( height ) },
					{ "depth",  ScaffoldFmt( depth ) },
				} );
			}

			//! perlin2d_painter with NO colora/colorb (both default "none")
			//! -- the drop-in recipe's own choice (modeling-workflow-and-
			//! geometry.md): a displacement source needs no colour, only
			//! its raw [0,1] noise magnitude, which Job::AddDisplacedGeometry
			//! reads straight off the Function2D accessor.
			std::string ScaffoldPerlin2DText( const std::string& name, double persistence, unsigned int octaves,
			                                  double scaleU, double scaleV, double shiftU, double shiftV )
			{
				return ScaffoldChunkText( "perlin2d_painter", {
					{ "name",        name },
					{ "persistence", ScaffoldFmt( persistence ) },
					{ "octaves",     std::to_string( octaves ) },
					{ "scale",       ScaffoldFmt( scaleU ) + " " + ScaffoldFmt( scaleV ) },
					{ "shift",       ScaffoldFmt( shiftU ) + " " + ScaffoldFmt( shiftV ) },
				} );
			}

			std::string ScaffoldDisplacedGeometryText( const std::string& name, const std::string& baseGeometry,
			                                           unsigned int tessDetail, const std::string& displacement, double dispScale )
			{
				return ScaffoldChunkText( "displaced_geometry", {
					{ "name",          name },
					{ "base_geometry", baseGeometry },
					{ "detail",        std::to_string( tessDetail ) },
					{ "displacement",  displacement },
					{ "disp_scale",    ScaffoldFmt( dispScale ) },
				} );
			}

			//! One `sweep_geometry` `part`-style repeatable line's VALUE
			//! (the "profile_point <x> <h>" grammar -- see
			//! SweepGeometryAsciiChunkParser::Describe).
			std::string ScaffoldProfilePointLine( double x, double h )
			{
				return ScaffoldFmt( x ) + " " + ScaffoldFmt( h );
			}

			std::string ScaffoldSweepGeometryText( const std::string& name,
			                                       const std::vector<std::pair<double,double>>& profile,
			                                       const std::vector<std::array<double,3>>& path,
			                                       unsigned int nLen, double endScaleX, double endScaleY )
			{
				std::vector<std::pair<std::string,std::string>> params;
				params.push_back( { "name", name } );
				for( const auto& pp : profile ) params.push_back( { "profile_point", ScaffoldProfilePointLine( pp.first, pp.second ) } );
				for( const auto& pt : path )     params.push_back( { "point", ScaffoldVec3( pt[0], pt[1], pt[2] ) } );
				params.push_back( { "n_len",       std::to_string( nLen ) } );
				params.push_back( { "end_scale_x", ScaffoldFmt( endScaleX ) } );
				params.push_back( { "end_scale_y", ScaffoldFmt( endScaleY ) } );
				return ScaffoldChunkText( "sweep_geometry", params );
			}

			//! One `sdf_geometry` `part` line's VALUE: `<prim> <op> <k>
			//! <px py pz>  <exDeg eyDeg ezDeg>  <sx sy sz>  <c1 c2 c3>  <round>`
			//! (the shape-specific third triple is spelled `c1 c2 c3` here,
			//! not the single-letter form SDFGeometryAsciiChunkParser::
			//! Describe's `part` doc uses, ONLY because clang's
			//! `-Wdocumentation-html` misreads a single-letter placeholder
			//! immediately after an opening angle bracket as an HTML tag
			//! name needing a matching close tag; no OTHER placeholder
			//! above collides with a real HTML tag name, so none of the
			//! others needed renaming) -- see that `part` doc for the
			//! authoritative field meanings.
			//! Every part below uses identity rotation/scale (matching
			//! both object-modeling-recipes.md worked examples), so this
			//! helper hardcodes those two triples rather than taking six
			//! more parameters nobody would ever vary.
			std::string ScaffoldSdfPartLine( const char* prim, const char* op, double k,
			                                 double px, double py, double pz,
			                                 double a, double b, double c, double round )
			{
				return std::string( prim ) + " " + op + " " + ScaffoldFmt( k ) + "  " +
				       ScaffoldVec3( px, py, pz ) + "  " + ScaffoldVec3( 0.0, 0.0, 0.0 ) + "  " +
				       ScaffoldVec3( 1.0, 1.0, 1.0 ) + "  " + ScaffoldVec3( a, b, c ) + "  " + ScaffoldFmt( round );
			}

			std::string ScaffoldSDFGeometryText( const std::string& name, const std::vector<std::string>& partLines, unsigned int maxsteps )
			{
				std::vector<std::pair<std::string,std::string>> params;
				params.push_back( { "name", name } );
				for( const std::string& pl : partLines ) params.push_back( { "part", pl } );
				params.push_back( { "maxsteps", std::to_string( maxsteps ) } );
				return ScaffoldChunkText( "sdf_geometry", params );
			}

			//! One generated chunk: its kind (for the collision precheck),
			//! its own chunk `name` (ditto), and its full chunk text --
			//! deliberately the SAME shape as ScaffoldChunkEntry above
			//! (not reused directly: that struct is anonymous-namespace-
			//! local to the material-scaffold block and duplicating a
			//! 3-field aggregate is cheaper and clearer than exporting it).
			struct GeoScaffoldChunkEntry
			{
				std::string kind;
				std::string name;
				std::string text;
			};

			//! The whole expansion for one geometry family: every chunk in
			//! insertion order, plus the ONE geometry chunk's name/kind a
			//! model should bind into a `standard_object.geometry` slot.
			//! E3 addendum: materialName/materialKind, mediumName/mediumKind,
			//! objectName/objectKind stay empty for every family except
			//! volume_bank -- see BuildVolumeBank's own comment for why
			//! that ONE family emits a material/medium/object too.
			struct GeoScaffoldGraph
			{
				std::vector<GeoScaffoldChunkEntry> chunks;
				std::string geometryName;
				std::string geometryKind;
				std::string materialName;
				std::string materialKind;
				std::string mediumName;
				std::string mediumKind;
				std::string objectName;
				std::string objectKind;
			};

			//! displaced_slab: box_geometry base + perlin2d_painter noise
			//! source + displaced_geometry bolt-on (the S3a skill fence's
			//! proven drop-in pattern -- modeling-workflow-and-geometry.md
			//! "Drop-in: a bumpy slab via displaced_geometry").  `size`
			//! sets the footprint and thickness; `aspect` elongates the
			//! footprint (width:depth ratio, footprint area held roughly
			//! constant); `detail` is HONEST for BOTH displacement
			//! amplitude (disp_scale) AND tessellation (displaced_geometry's
			//! own `detail` field) -- the one family in this tool where
			//! finer tessellation is warranted by genuinely finer surface
			//! content, unlike the SMS "finer tessellation does not fix a
			//! too-busy displacement" caution (that caution is about
			//! FIXING a bad amplitude choice by adding more triangles;
			//! this scaffold ties both to the SAME creative knob so they
			//! move together, not the too-busy failure mode).
			GeoScaffoldGraph BuildDisplacedSlab( const std::string& name, double size, double detail, double aspect )
			{
				GeoScaffoldGraph out;
				const std::string nBase = "tmpl_" + name + "_base";
				const std::string nBump = "tmpl_" + name + "_bump";
				const std::string nDisp = "tmpl_" + name + "_disp";

				const double width  = size * std::sqrt( aspect );
				const double depth  = size / std::sqrt( aspect );
				const double thin   = ScaffoldJitterRange( name, "slab_thin", 0.12, 0.22 );
				const double height = size * thin;
				out.chunks.push_back( { "box_geometry", nBase, ScaffoldBoxGeometryText( nBase, width, height, depth ) } );

				const double persistence   = ScaffoldJitterRange( name, "slab_persist", 0.35, 0.65 );
				const unsigned int octaves = ScaffoldJitterUInt( name, "slab_octaves", 3, 5 );
				const double freqJitter    = ScaffoldJitterRange( name, "slab_freq", 3.0, 7.0 );
				const double freqU = freqJitter * ( 1.0 + 3.0 * detail );
				const double freqV = ScaffoldJitterRange( name, "slab_freqv", 3.0, 7.0 ) * ( 1.0 + 3.0 * detail );
				const double shiftU = ScaffoldJitterRange( name, "slab_shiftu", 0.0, 100.0 );
				const double shiftV = ScaffoldJitterRange( name, "slab_shiftv", 0.0, 100.0 );
				out.chunks.push_back( { "perlin2d_painter", nBump,
					ScaffoldPerlin2DText( nBump, persistence, octaves, freqU, freqV, shiftU, shiftV ) } );

				const double dispScale = size * ( 0.03 + 0.22 * detail ) * ScaffoldJitterRange( name, "slab_dispjit", 0.85, 1.15 );
				const unsigned int tessBase = 16 + static_cast<unsigned int>( 40.0 * detail + 0.5 );
				const unsigned int tessDetail = ScaffoldJitterUInt( name, "slab_tess",
					tessBase > 4 ? tessBase - 4 : tessBase, tessBase + 4 );
				out.chunks.push_back( { "displaced_geometry", nDisp,
					ScaffoldDisplacedGeometryText( nDisp, nBase, tessDetail, nBump, dispScale ) } );

				out.geometryName = nDisp;
				out.geometryKind = "displaced_geometry";
				return out;
			}

			//! sweep_rail: a single sweep_geometry chunk -- a compact
			//! regular-polygon profile ("compact closed profile" per the
			//! design brief) swept along a 3-point bowed path with a
			//! linear end taper (the sweep_instances.RISEscene horn idiom
			//! -- square profile, 3 path points, end_scale taper --
			//! generalized to a jittered N-gon).  `size` sets the profile
			//! radius and path bow; `aspect` elongates the path length
			//! (a thin long rail vs a short stub); `detail` is the
			//! profile's SIDE COUNT (4..8 -- "profile complexity"; `sides =
			//! 4 + uint(detail*4.0 + 0.5)` maxes out at detail's own upper
			//! bound of 1.0 -> 4+uint(4.5) = 8, so 8 is a REACHED ceiling,
			//! not a clamp -- there is deliberately no `sides > 8` guard
			//! below, only the `< 4` floor for detail's lower bound of 0.0).
			GeoScaffoldGraph BuildSweepRail( const std::string& name, double size, double detail, double aspect )
			{
				GeoScaffoldGraph out;
				const std::string nRail = "tmpl_" + name + "_rail";

				unsigned int sides = 4 + static_cast<unsigned int>( detail * 4.0 + 0.5 );
				if( sides < 4 ) sides = 4;
				const double profileRadius = size * ScaffoldJitterRange( name, "rail_profr", 0.06, 0.12 );
				const double phase0 = ScaffoldJitterRange( name, "rail_profphase", 0.0, 6.283185 );
				std::vector<std::pair<double,double>> profile;
				profile.reserve( sides );
				for( unsigned int i = 0; i < sides; ++i ) {
					const double ang = phase0 + 2.0 * 3.14159265358979323846 * static_cast<double>( i ) / static_cast<double>( sides );
					profile.push_back( { profileRadius * std::cos( ang ), profileRadius * std::sin( ang ) } );
				}

				const double halfLen = size * aspect * ScaffoldJitterRange( name, "rail_halflen", 0.6, 1.0 );
				const double bowSignY = ( ScaffoldJitter01( name, "rail_bowsigny" ) < 0.5 ) ? -1.0 : 1.0;
				const double bowSignZ = ( ScaffoldJitter01( name, "rail_bowsignz" ) < 0.5 ) ? -1.0 : 1.0;
				const double bowY = bowSignY * size * ScaffoldJitterRange( name, "rail_bowy", 0.10, 0.35 );
				const double bowZ = bowSignZ * size * ScaffoldJitterRange( name, "rail_bowz", 0.05, 0.20 );
				std::vector<std::array<double,3>> path;
				path.push_back( { -halfLen, 0.0, 0.0 } );
				path.push_back( { 0.0, bowY, bowZ } );
				path.push_back( { halfLen, 0.0, 0.0 } );

				const unsigned int nLen = ScaffoldJitterUInt( name, "rail_nlen", 24, 64 );
				const double endScaleX = ScaffoldJitterRange( name, "rail_endx", 0.20, 0.45 );
				const double endScaleY = ScaffoldJitterRange( name, "rail_endy", 0.20, 0.45 );
				out.chunks.push_back( { "sweep_geometry", nRail,
					ScaffoldSweepGeometryText( nRail, profile, path, nLen, endScaleX, endScaleY ) } );

				out.geometryName = nRail;
				out.geometryKind = "sweep_geometry";
				return out;
			}

			//! blended_vessel: a single sdf_geometry chunk -- a 3-segment
			//! roundcone+smin profile (base -> belly -> rim, the
			//! object-modeling-recipes.md turned-vessel idiom, simplified
			//! to 3 spans with no separate swept neck) closed with a
			//! flat-bottom `box subtract` (Recipe 4's flat-bottom-needs-a-
			//! cut rule).  `size` sets the base/belly radii; `aspect`
			//! elongates total height (a squat bowl at low aspect, a
			//! tall vase at high aspect); `detail` is SMIN TIGHTNESS (the
			//! two blend radii shrink toward a crisper joint as detail
			//! rises toward 1, widen toward a softer shoulder as it falls
			//! toward 0).
			GeoScaffoldGraph BuildBlendedVessel( const std::string& name, double size, double detail, double aspect )
			{
				GeoScaffoldGraph out;
				const std::string nVessel = "tmpl_" + name + "_vessel";

				const double totalH  = size * aspect * ScaffoldJitterRange( name, "vessel_h", 0.7, 1.1 );
				const double baseR   = size * ScaffoldJitterRange( name, "vessel_baser", 0.10, 0.16 );
				const double baseTopR = baseR * ScaffoldJitterRange( name, "vessel_basetopr", 1.05, 1.30 );
				const double bellyR  = size * ScaffoldJitterRange( name, "vessel_bellyr", 0.34, 0.46 );
				const double rimR    = bellyR * ScaffoldJitterRange( name, "vessel_rimr", 0.55, 0.78 );

				double baseH  = totalH * ScaffoldJitterRange( name, "vessel_baseh", 0.12, 0.20 );
				double bellyH = totalH * ScaffoldJitterRange( name, "vessel_bellyh", 0.42, 0.58 );
				double rimH   = totalH - baseH - bellyH;
				if( rimH < totalH * 0.08 ) rimH = totalH * 0.08;   // guard: keep every span honestly positive

				const double tightness = 1.0 - 0.6 * detail;
				const double k1 = size * ScaffoldJitterRange( name, "vessel_k1", 0.14, 0.24 ) * tightness;
				const double k2 = size * ScaffoldJitterRange( name, "vessel_k2", 0.10, 0.18 ) * tightness;

				std::vector<std::string> parts;
				parts.push_back( ScaffoldSdfPartLine( "roundcone", "union", 0.0,   0.0, 0.0, 0.0,               baseR, baseTopR, baseH, 0.0 ) );
				parts.push_back( ScaffoldSdfPartLine( "roundcone", "smin", k1,     0.0, baseH, 0.0,             baseTopR, bellyR, bellyH, 0.0 ) );
				parts.push_back( ScaffoldSdfPartLine( "roundcone", "smin", k2,     0.0, baseH + bellyH, 0.0,    bellyR, rimR, rimH, 0.0 ) );
				const double cutHalfY = baseR * 1.5;
				parts.push_back( ScaffoldSdfPartLine( "box", "subtract", 0.0,      0.0, -cutHalfY, 0.0,         baseR * 2.0, cutHalfY, baseR * 2.0, 0.0 ) );

				out.chunks.push_back( { "sdf_geometry", nVessel, ScaffoldSDFGeometryText( nVessel, parts, 256 ) } );
				out.geometryName = nVessel;
				out.geometryKind = "sdf_geometry";
				return out;
			}

			//! sdf_column: a single sdf_geometry chunk -- a 3-segment
			//! base/shaft/capital roundcone+smin chain (a turned-column
			//! silhouette: a wide flat foot narrowing into a constant-
			//! radius shaft, then flaring back out into a capital),
			//! closed with the SAME flat-bottom `box subtract` as
			//! blended_vessel.  `size` sets the base/shaft/capital radii;
			//! `aspect` elongates the SHAFT (a squat pedestal at low
			//! aspect, a tall slender column at high aspect); `detail` is
			//! SMIN TIGHTNESS, identical semantics to blended_vessel's.
			GeoScaffoldGraph BuildSdfColumn( const std::string& name, double size, double detail, double aspect )
			{
				GeoScaffoldGraph out;
				const std::string nCol = "tmpl_" + name + "_col";

				const double baseR   = size * ScaffoldJitterRange( name, "col_baser", 0.28, 0.38 );
				const double shaftR  = baseR * ScaffoldJitterRange( name, "col_shaftr", 0.45, 0.62 );
				const double capTopR = shaftR * ScaffoldJitterRange( name, "col_captopr", 1.4, 1.9 );

				const double baseH  = size * ScaffoldJitterRange( name, "col_baseh", 0.10, 0.18 );
				const double shaftH = size * aspect * ScaffoldJitterRange( name, "col_shafth", 0.55, 0.85 );
				const double capH   = size * ScaffoldJitterRange( name, "col_caph", 0.10, 0.16 );

				const double tightness = 1.0 - 0.6 * detail;
				const double k1 = size * ScaffoldJitterRange( name, "col_k1", 0.16, 0.26 ) * tightness;
				const double k2 = size * ScaffoldJitterRange( name, "col_k2", 0.14, 0.22 ) * tightness;

				std::vector<std::string> parts;
				parts.push_back( ScaffoldSdfPartLine( "roundcone", "union", 0.0,   0.0, 0.0, 0.0,              baseR, shaftR, baseH, 0.0 ) );
				parts.push_back( ScaffoldSdfPartLine( "roundcone", "smin", k1,     0.0, baseH, 0.0,            shaftR, shaftR, shaftH, 0.0 ) );
				parts.push_back( ScaffoldSdfPartLine( "roundcone", "smin", k2,     0.0, baseH + shaftH, 0.0,   shaftR, capTopR, capH, 0.0 ) );
				const double cutHalfY = baseR * 1.5;
				parts.push_back( ScaffoldSdfPartLine( "box", "subtract", 0.0,      0.0, -cutHalfY, 0.0,        baseR * 2.0, cutHalfY, baseR * 2.0, 0.0 ) );

				out.chunks.push_back( { "sdf_geometry", nCol, ScaffoldSDFGeometryText( nCol, parts, 256 ) } );
				out.geometryName = nCol;
				out.geometryKind = "sdf_geometry";
				return out;
			}

			//----------------------------------------------------------------
			// Slice E3: blended_chain, volume_bank.  Both take a param
			// SHAPE different from the original four (points/taper instead
			// of aspect; tone in addition to aspect) so, unlike
			// DisplacedSlab/SweepRail/BlendedVessel/SdfColumn, they are
			// NOT routed through BuildGeometryScaffoldGraph below --
			// InsertGeometryScaffold calls BuildBlendedChain/BuildVolumeBank
			// directly.  See docs/agentic-redesign/75-expressive-surface-arc.md
			// slice E3 for the full design; the census that motivated it
			// (cheapest-primitive parts, assembly-by-overlap instead of
			// smin, uniform-density atmosphere that cannot swirl).
			//----------------------------------------------------------------

			//! blended_chain: ONE sdf_geometry chunk -- a smin-blended chain
			//! of spheres swept along the model-authored `ctrl` path (a
			//! clamped Catmull-Rom spline through every given point -- see
			//! ScaffoldCatmullRomPath).  Deliberately SPHERES ONLY, not
			//! roundcones: a sphere's SDF is position-only, so no per-
			//! segment orientation math is needed to keep every node
			//! correctly aligned to an arbitrary 3D path direction --
			//! continuity comes from smin alone, never from matching a
			//! cone's local axis to a path tangent (which would need a
			//! from-Y-to-tangent rotation-to-Euler-angle derivation this
			//! generator deliberately avoids -- one fewer place for a sign/
			//! convention bug to hide).  `size` sets the base radius AT THE
			//! FIRST POINT; `taper` (0..1) linearly falls the radius toward
			//! the LAST point (floored at 12% of `size`, and a second hard
			//! floor at 5%, so a fully-tapered tip never collapses to a
			//! literal zero-radius degenerate sphere); `detail` (0..1) sets
			//! the DETAIL-DRIVEN component of node count (5..20 -- "more
			//! nodes = smoother") AND, independently, the cosmetic
			//! component of each joint's smin blend radius (see kCosmetic
			//! below).
			//!
			//! CONTINUITY BY CONSTRUCTION -- FIX-ROUND P1a (a review round
			//! found the original "k >= 0.6x spacing" floor mathematically
			//! insufficient: a reviewer's render of a sparse/tapered
			//! adversarial config fractured into 7 disconnected blobs).
			//! The exact bridging condition for RISE's polynomial smin
			//! (SDFGeometry.cpp's `sminP`: min(a,b) - h*h*k/4, h =
			//! max(k-|a-b|,0)/k) is derived by evaluating it along the
			//! straight line joining two adjacent sphere centers: the two
			//! spheres' signed distances cross at the point where each
			//! equals HALF the surface gap (gap = spacing - r[i-1] - r[i],
			//! the sphere-SURFACE-to-surface distance, floored at 0 when
			//! they already overlap); substituting a=b=gap/2 into sminP
			//! gives gap/2 - k/4, so the blended field reaches THAT point
			//! (genuinely bridges, no visible neck) iff **k >= 2*gap**.
			//! This chunk applies that EXACT threshold, UNCONDITIONALLY,
			//! per joint, computed from the REALIZED (post-jitter) radii
			//! and node spacing -- with a 1.25x safety margin (k >=
			//! 2.5*gap) absorbing finite ray-march step size and the fact
			//! the derivation is a 1-D argument along one line, not a full
			//! 3-D proof.  THIS is the correctness guarantee: it holds
			//! regardless of node count, `size`, `taper`, or `detail`.
			//!
			//! A second, independent mechanism keeps the RESULT
			//! sculptural rather than merely correct: naive uniform-in-u
			//! node placement on a heavily-tapered/long/sparse chain would
			//! force the bridging floor to demand a k many times larger
			//! than the local radius (fixing the gap by ballooning the
			//! blend into an oversized blob near the tapered tip, not by
			//! genuinely sculpting it) -- so node count ALSO scales with a
			//! GEOMETRIC estimate (`nodeCountGeom` below) that targets
			//! spacing <= 1.5x the worst-case realizable radius sum,
			//! capped at 40 total nodes.  Sparse, heavily-tapered chains
			//! past that cap trade blend sharpness (a wider-than-ideal k
			//! near the tip) for the UNCONDITIONAL continuity guarantee --
			//! an honest, documented trade, never a silent fracture.
			//! Per-node radius jitter (+-12%) and a small perpendicular
			//! path-jitter on INTERIOR nodes only (enveloped to EXACTLY
			//! ZERO at both endpoints via a sin(pi*u) window, so the
			//! model's authored endpoint coordinates are preserved EXACTLY
			//! -- the documented attachment mechanism) keep two chains
			//! with identical params from being visually identical beyond
			//! their explicit params, matching every other family's
			//! jitter contract.
			GeoScaffoldGraph BuildBlendedChain( const std::string& name, const std::vector<std::array<double,3>>& ctrl,
			                                    double size, double taper, double detail )
			{
				GeoScaffoldGraph out;
				const std::string nChain = "tmpl_" + name + "_chain";

				const unsigned int nodeCountDetail = ScaffoldJitterUInt( name, "chain_nodecount",
					5 + static_cast<unsigned int>( 10.0 * detail ), 8 + static_cast<unsigned int>( 12.0 * detail ) );

				// Geometric density uplift (P1a fix round) -- a cheap,
				// deterministic (no jitter) path-length estimate via a
				// fine Catmull-Rom sampling, and the GLOBAL worst-case
				// realizable radius (taperFactor is non-increasing in u,
				// so its minimum is at u=1; the per-node jitter's
				// documented range below is [0.88,1.12], so 0.88 is its
				// conservative lower bound) -- see the function doc above
				// for the target-spacing derivation.
				double pathLen = 0.0;
				{
					const int kLenSamples = 64;
					std::array<double,3> prev = ScaffoldCatmullRomPath( ctrl, 0.0 );
					for( int s = 1; s <= kLenSamples; ++s ) {
						const double u = static_cast<double>( s ) / static_cast<double>( kLenSamples );
						const std::array<double,3> cur = ScaffoldCatmullRomPath( ctrl, u );
						const double dx = cur[0]-prev[0], dy = cur[1]-prev[1], dz = cur[2]-prev[2];
						pathLen += std::sqrt( dx*dx + dy*dy + dz*dz );
						prev = cur;
					}
				}
				const double tipFactor = std::max( 1.0 - taper, 0.12 );
				const double rMinConservative = size * tipFactor * 0.88;
				unsigned int nodeCountGeom = 2;
				if( rMinConservative > 1e-9 ) {
					const double targetSpacing = 3.0 * rMinConservative;   // 1.5 * (r+r)
					nodeCountGeom = static_cast<unsigned int>( std::ceil( pathLen / targetSpacing ) ) + 1;
				}
				unsigned int n = std::max( nodeCountDetail, nodeCountGeom );
				if( n < 2 )  n = 2;
				if( n > 40 ) n = 40;   // sane cap on part count; the per-joint floor below still guarantees bridging past it

				std::vector<std::array<double,3>> pos( n );
				for( unsigned int i = 0; i < n; ++i ) {
					const double u = static_cast<double>( i ) / static_cast<double>( n - 1 );
					pos[i] = ScaffoldCatmullRomPath( ctrl, u );
				}

				// Interior-node perpendicular jitter, enveloped to zero at
				// both ends -- a stable perpendicular basis per node from
				// the local finite-difference tangent crossed with a
				// reference axis (world-up, falling back to world-X when
				// the tangent is nearly parallel to world-up).
				const double jitterMag = size * 0.10 * ( 0.4 + 0.6 * ScaffoldJitter01( name, "chain_jmagbase" ) );
				for( unsigned int i = 1; i + 1 < n; ++i ) {
					const double u = static_cast<double>( i ) / static_cast<double>( n - 1 );
					const double envelope = std::sin( 3.14159265358979323846 * u );

					double tx = pos[i+1][0] - pos[i-1][0];
					double ty = pos[i+1][1] - pos[i-1][1];
					double tz = pos[i+1][2] - pos[i-1][2];
					const double tlen = std::sqrt( tx*tx + ty*ty + tz*tz );
					if( tlen < 1e-9 ) continue;
					tx /= tlen; ty /= tlen; tz /= tlen;

					double rx = 0.0, ry = 1.0, rz = 0.0;
					if( std::fabs( ty ) > 0.98 ) { rx = 1.0; ry = 0.0; rz = 0.0; }
					double px = ty*rz - tz*ry;
					double py = tz*rx - tx*rz;
					double pz = tx*ry - ty*rx;
					const double plen = std::sqrt( px*px + py*py + pz*pz );
					if( plen < 1e-9 ) continue;
					px /= plen; py /= plen; pz /= plen;

					const std::string saltA = "chain_jn" + std::to_string( i );
					const double amt = ( ScaffoldJitter01( name, saltA.c_str() ) * 2.0 - 1.0 ) * jitterMag * envelope;
					pos[i][0] += px * amt;
					pos[i][1] += py * amt;
					pos[i][2] += pz * amt;
				}

				// Realized (post-jitter) per-node radii -- computed BEFORE
				// the part-assembly loop so the bridging floor below can
				// reference BOTH radii of a joint (radii[i-1], radii[i]).
				std::vector<double> radii( n );
				for( unsigned int i = 0; i < n; ++i ) {
					const double u = static_cast<double>( i ) / static_cast<double>( n - 1 );
					const double taperFactor = 1.0 - taper * u;
					const std::string saltR = "chain_r" + std::to_string( i );
					const double rJit = 1.0 + ( ScaffoldJitter01( name, saltR.c_str() ) * 2.0 - 1.0 ) * 0.12;
					double radius = size * std::max( taperFactor, 0.12 ) * rJit;
					if( radius < size * 0.05 ) radius = size * 0.05;   // hard floor: never a literal zero-radius sphere
					radii[i] = radius;
				}

				const double detailK = 1.0 - 0.35 * detail;
				std::vector<std::string> parts;
				for( unsigned int i = 0; i < n; ++i ) {
					if( i == 0 ) {
						parts.push_back( ScaffoldSdfPartLine( "sphere", "union", 0.0,
							pos[i][0], pos[i][1], pos[i][2], radii[i], 0.0, 0.0, 0.0 ) );
					} else {
						const double dx = pos[i][0] - pos[i-1][0];
						const double dy = pos[i][1] - pos[i-1][1];
						const double dz = pos[i][2] - pos[i-1][2];
						const double spacing = std::sqrt( dx*dx + dy*dy + dz*dz );
						const std::string saltK = "chain_k" + std::to_string( i );
						const double kJit = ScaffoldJitterRange( name, saltK.c_str(), 0.75, 1.05 );
						const double kCosmetic = spacing * kJit * detailK;

						// EXACT sminP bridging floor -- see the function
						// doc above for the derivation.  UNCONDITIONAL:
						// always wins over kCosmetic when the realized
						// geometry demands it, regardless of detail/size/
						// taper/node-count.
						const double gap = std::max( 0.0, spacing - radii[i-1] - radii[i] );
						const double kBridgeFloor = 2.5 * gap;   // 1.25 * 2 * gap
						const double k = std::max( kCosmetic, kBridgeFloor );

						parts.push_back( ScaffoldSdfPartLine( "sphere", "smin", k,
							pos[i][0], pos[i][1], pos[i][2], radii[i], 0.0, 0.0, 0.0 ) );
					}
				}

				out.chunks.push_back( { "sdf_geometry", nChain, ScaffoldSDFGeometryText( nChain, parts, 256 ) } );
				out.geometryName = nChain;
				out.geometryKind = "sdf_geometry";
				return out;
			}

			//! volume_bank: an elongated atmospheric volume, ready-wired --
			//! ellipsoid_geometry container + a near-invisible dielectric
			//! boundary material + a painter_heterogeneous_medium whose
			//! density comes from a domain-warped 3D noise painter (so the
			//! volume genuinely SWIRLS, unlike a uniform-density sphere --
			//! see the media-surface finding in
			//! docs/agentic-redesign/75-expressive-surface-arc.md slice E3:
			//! RISE DOES support spatially-modulated density via ANY painter
			//! bound to painter_heterogeneous_medium's `density_painter`
			//! slot (ChunkParserRegistry.cpp's PainterHeterogeneousMedium
			//! parser + Job::AddPainterHeterogeneousMedium), so this family
			//! uses that path directly rather than the layered-homogeneous-
			//! banks fallback the design brief allowed for if heterogeneous
			//! density were unsupported) -- PLUS a standard_object binding
			//! all three together.  `size` sets the container's cross-
			//! section radii; `aspect` (>0) elongates it along local X (a
			//! horizontal atmospheric bank; reorient with the emitted
			//! object's own `orientation` for a different axis); `detail`
			//! (0..1) raises the density painter's domain-warp amplitude AND
			//! frequency together (a genuinely wispier, more swirled field,
			//! not just a louder single knob); `toneR/G/B` (each 0..1, the
			//! parsed `tone` param) tints the medium's `scattering`
			//! coefficient (and inversely its `absorption`, so a strongly-
			//! tinted tone both scatters its own hue and absorbs less of
			//! it -- the physically honest direction).
			//!
			//! THE ONE FAMILY THAT EMITS A MATERIAL, A MEDIUM, AND A
			//! STANDARD_OBJECT (every other family emits geometry only, per
			//! S3b's "the model wires standard_object/material itself"
			//! convention -- see InsertGeometryScaffold's header doc for the
			//! two-part rationale this family deviates for): (1) a bare
			//! volume graph does nothing until an object binds the medium
			//! via `interior_medium` (Job::SetObjectInteriorMedium is the
			//! ONLY thing that traces a medium chunk at all), and (2) more
			//! importantly, painter_heterogeneous_medium's `bbox_min`/
			//! `bbox_max` are WORLD-SPACE and fixed at chunk-parse time (see
			//! HeterogeneousMedium.cpp's LookupDensity, which tests the
			//! RAY's world-space point against m_bboxMin/m_bboxMax directly
			//! -- there is no per-object inverse-transform step) -- so this
			//! scaffold can only guarantee a correctly-aligned density field
			//! by choosing the object's placement ITSELF (identity transform
			//! at the origin) and computing the bbox to match.  The emitted
			//! object sits at the origin with `casts_shadows FALSE` (the
			//! pt_alchemists_sanctum.RISEscene idiom for a pure-volume
			//! container -- see that scene's `fog_ring`/`aureole` objects)
			//! -- the medium's bbox is computed DIRECTLY from the
			//! container's own realized radii at that same identity
			//! transform, with an 8% margin so the density field comfortably
			//! covers the ellipsoid's surface.  InsertGeometryScaffold
			//! attaches a FACTUAL (not advisory) reminder of this coupling
			//! to `message` on an ok==true result.
			GeoScaffoldGraph BuildVolumeBank( const std::string& name, double size, double detail, double aspect,
			                                  double toneR, double toneG, double toneB )
			{
				GeoScaffoldGraph out;
				const std::string nContainer = "tmpl_" + name + "_container";
				const std::string nDenseLo   = "tmpl_" + name + "_denselo";
				const std::string nDenseHi   = "tmpl_" + name + "_densehi";
				const std::string nDensity   = "tmpl_" + name + "_density";
				const std::string nShell     = "tmpl_" + name + "_shell";
				const std::string nMedium    = "tmpl_" + name + "_medium";
				const std::string nObject    = "tmpl_" + name + "_obj";

				const double rx = size * aspect * ScaffoldJitterRange( name, "bank_rx", 0.9, 1.1 );
				const double ry = size * ScaffoldJitterRange( name, "bank_ry", 0.28, 0.42 );
				const double rz = size * ScaffoldJitterRange( name, "bank_rz", 0.28, 0.42 );
				out.chunks.push_back( { "ellipsoid_geometry", nContainer, ScaffoldChunkText( "ellipsoid_geometry", {
					{ "name", nContainer }, { "radii", ScaffoldVec3( rx, ry, rz ) } } ) } );

				const double loV = ScaffoldJitterRange( name, "bank_lo", 0.01, 0.05 );
				const double hiV = ScaffoldJitterRange( name, "bank_hi", 0.85, 1.0 );
				out.chunks.push_back( { "uniformcolor_painter", nDenseLo, ScaffoldUniformColorText( nDenseLo, loV, loV, loV ) } );
				out.chunks.push_back( { "uniformcolor_painter", nDenseHi, ScaffoldUniformColorText( nDenseHi, hiV, hiV, hiV ) } );

				const double persistence   = ScaffoldJitterRange( name, "bank_persist", 0.45, 0.75 );
				const unsigned int octaves = ScaffoldJitterUInt( name, "bank_octaves", 3, 5 );
				const double warpAmp       = ScaffoldJitterRange( name, "bank_warpamp", 1.5, 4.0 ) * ( 0.4 + 1.2 * detail );
				const unsigned int warpLevels = ScaffoldJitterUInt( name, "bank_warplevels", 1, 3 );
				const double freqBase = ScaffoldJitterRange( name, "bank_freq", 1.5, 3.5 ) * ( 0.6 + 1.0 * detail ) / size;
				const double shiftX = ScaffoldJitterRange( name, "bank_shiftx", 0.0, 100.0 );
				const double shiftY = ScaffoldJitterRange( name, "bank_shifty", 0.0, 100.0 );
				const double shiftZ = ScaffoldJitterRange( name, "bank_shiftz", 0.0, 100.0 );
				out.chunks.push_back( { "domainwarp3d_painter", nDensity,
					ScaffoldDomainWarp3DText( nDensity, nDenseLo, nDenseHi, persistence, octaves, warpAmp, warpLevels,
						freqBase, freqBase, freqBase, shiftX, shiftY, shiftZ ) } );

				const double iorEps = ScaffoldJitterRange( name, "bank_ior", 0.0005, 0.003 );
				const double tauEps = ScaffoldJitterRange( name, "bank_tau", 0.0005, 0.003 );
				out.chunks.push_back( { "dielectric_material", nShell, ScaffoldChunkText( "dielectric_material", {
					{ "name", nShell },
					{ "ior", ScaffoldFmt( 1.0 + iorEps ) },
					{ "tau", ScaffoldFmt( 1.0 - tauEps ) },
					{ "scattering", "100000.0" },
				} ) } );

				const double scatScale = ScaffoldJitterRange( name, "bank_scatscale", 0.25, 0.55 ) * ( 0.5 + 0.8 * detail );
				const double absScale  = ScaffoldJitterRange( name, "bank_absscale", 0.01, 0.05 );
				const double hg        = ScaffoldJitterRange( name, "bank_hg", 0.15, 0.45 );
				const double margin = 1.08;
				out.chunks.push_back( { "painter_heterogeneous_medium", nMedium, ScaffoldChunkText( "painter_heterogeneous_medium", {
					{ "name", nMedium },
					{ "absorption", ScaffoldVec3( (1.0-toneR)*absScale, (1.0-toneG)*absScale, (1.0-toneB)*absScale ) },
					{ "scattering", ScaffoldVec3( toneR*scatScale, toneG*scatScale, toneB*scatScale ) },
					{ "phase", "hg " + ScaffoldFmt( hg ) },
					{ "density_painter", nDensity },
					{ "resolution", std::to_string( ScaffoldJitterUInt( name, "bank_res", 28, 48 ) ) },
					{ "color_to_scalar", "luminance" },
					{ "bbox_min", ScaffoldVec3( -rx*margin, -ry*margin, -rz*margin ) },
					{ "bbox_max", ScaffoldVec3(  rx*margin,  ry*margin,  rz*margin ) },
				} ) } );

				out.chunks.push_back( { "standard_object", nObject, ScaffoldChunkText( "standard_object", {
					{ "name", nObject },
					{ "geometry", nContainer },
					{ "material", nShell },
					{ "interior_medium", nMedium },
					{ "position", ScaffoldVec3( 0.0, 0.0, 0.0 ) },
					{ "casts_shadows", "FALSE" },
				} ) } );

				out.geometryName = nContainer;
				out.geometryKind = "ellipsoid_geometry";
				out.materialName = nShell;
				out.materialKind = "dielectric_material";
				out.mediumName   = nMedium;
				out.mediumKind   = "painter_heterogeneous_medium";
				out.objectName   = nObject;
				out.objectKind   = "standard_object";
				return out;
			}

			GeoScaffoldGraph BuildGeometryScaffoldGraph( AgentSession::GeometryScaffoldFamily family, const std::string& name,
			                                             double size, double detail, double aspect )
			{
				switch( family )
				{
					case AgentSession::GeometryScaffoldFamily::DisplacedSlab: return BuildDisplacedSlab( name, size, detail, aspect );
					case AgentSession::GeometryScaffoldFamily::SweepRail:     return BuildSweepRail( name, size, detail, aspect );
					case AgentSession::GeometryScaffoldFamily::BlendedVessel: return BuildBlendedVessel( name, size, detail, aspect );
					case AgentSession::GeometryScaffoldFamily::SdfColumn:     return BuildSdfColumn( name, size, detail, aspect );
					// BlendedChain/VolumeBank take a DIFFERENT param shape
					// (points/taper, tone) and are dispatched directly by
					// InsertGeometryScaffold via BuildBlendedChain/
					// BuildVolumeBank -- never routed through this function.
					// Cases kept here (rather than a `default:`) so this
					// switch stays EXHAUSTIVE and -Wswitch still catches a
					// future family added to the enum but forgotten here.
					case AgentSession::GeometryScaffoldFamily::BlendedChain:  return GeoScaffoldGraph();
					case AgentSession::GeometryScaffoldFamily::VolumeBank:    return GeoScaffoldGraph();
				}
				return GeoScaffoldGraph();
			}

			//! R2 (2026-08-10, replace_geometry_scaffold): the SHARED
			//! validate-then-expand front half of BOTH geometry scaffold verbs.
			//! Extracted VERBATIM from InsertGeometryScaffold's own prologue --
			//! every guard, every message, and the per-family dispatch below are
			//! the original text with the VERB NAME passed in rather than
			//! hard-coded, so insert_geometry_scaffold's refusal strings stay
			//! byte-identical and the two verbs can never drift on what a valid
			//! family/param set is or on what chunks a family expands to.
			//!
			//! Returns true with `outFam`/`outGraph` populated; false with
			//! `outMessage` carrying the actionable, verb-labelled refusal.
			//! Pure text generation -- no session/controller/document access at
			//! all (the name-collision precheck, which DOES need the document,
			//! stays at each caller).
			bool ScaffoldValidateAndBuildGeometry( const char* verb,
			                                       const std::string& family, const std::string& name,
			                                       double size, double detail, double aspect,
			                                       const std::string& points, double taper, const std::string& tone,
			                                       AgentSession::GeometryScaffoldFamily& outFam,
			                                       GeoScaffoldGraph& outGraph, std::string& outMessage )
			{
				const std::string v( verb );

				if( !ScaffoldParseGeometryFamily( family, outFam ) ) {
					outMessage = v + " refused: unknown family `" + family +
						"` -- valid families are: displaced_slab, sweep_rail, blended_vessel, sdf_column, "
						"blended_chain, volume_bank";
					return false;
				}
				if( !ScaffoldNameIsValid( name ) ) {
					outMessage = v + " refused: `name` must be a non-empty token "
						"(letters, digits, underscore, hyphen only, max " + std::to_string( kScaffoldMaxNameLength ) +
						" chars) -- got `" + name + "`";
					return false;
				}
				if( !std::isfinite( size ) || size <= 0.0 ) {
					outMessage = v + " refused: `size` must be a finite number > 0";
					return false;
				}
				if( !std::isfinite( detail ) || detail < 0.0 || detail > 1.0 ) {
					outMessage = v + " refused: `detail` must be a finite number in [0,1]";
					return false;
				}

				// Per-family param shape (E3): blended_chain takes `points`/
				// `taper` INSTEAD of `aspect`; volume_bank takes `aspect` PLUS
				// `tone`; the original four take only `aspect`.  See
				// InsertGeometryScaffold's header doc for the full per-family
				// param table.
				std::vector<std::array<double,3>> parsedPoints;
				double toneR = 0.0, toneG = 0.0, toneB = 0.0;

				if( outFam == AgentSession::GeometryScaffoldFamily::BlendedChain ) {
					std::string perr;
					if( !ScaffoldParsePoints( points, parsedPoints, perr ) ) {
						outMessage = v + " refused: " + perr;
						return false;
					}
					if( !std::isfinite( taper ) || taper < 0.0 || taper > 1.0 ) {
						outMessage = v + " refused: `taper` must be a finite number in [0,1]";
						return false;
					}
				} else if( outFam == AgentSession::GeometryScaffoldFamily::VolumeBank ) {
					if( !std::isfinite( aspect ) || aspect <= 0.0 ) {
						outMessage = v + " refused: `aspect` must be a finite number > 0";
						return false;
					}
					if( !ScaffoldParseTone( tone, toneR, toneG, toneB ) ) {
						outMessage = v + " refused: `tone` must be \"r g b\", each 0..1 -- got `" + tone + "`";
						return false;
					}
				} else {
					if( !std::isfinite( aspect ) || aspect <= 0.0 ) {
						outMessage = v + " refused: `aspect` must be a finite number > 0";
						return false;
					}
				}

				if( outFam == AgentSession::GeometryScaffoldFamily::BlendedChain ) {
					outGraph = BuildBlendedChain( name, parsedPoints, size, taper, detail );
				} else if( outFam == AgentSession::GeometryScaffoldFamily::VolumeBank ) {
					outGraph = BuildVolumeBank( name, size, detail, aspect, toneR, toneG, toneB );
				} else {
					outGraph = BuildGeometryScaffoldGraph( outFam, name, size, detail, aspect );
				}
				return true;
			}

			//! R2 (2026-08-10): the DECLARATION-TIER classifier
			//! Job::ApplyCstInsertChunk's own insert-position block uses, restated
			//! here (same two tiers, same ChunkCategory mapping, same "everything
			//! else appends" default) because replace_geometry_scaffold builds its
			//! candidate document in ONE piece and therefore cannot route each
			//! chunk through that Job primitive.  Keep the two in lockstep: a new
			//! ChunkCategory must be classified in BOTH.
			int ScaffoldDeclarationTier_( const std::string& keyword )
			{
				const ChunkDescriptor* d = DescriptorForKeyword( String( keyword.c_str() ) );
				if( !d ) return -1;
				switch( d->category ) {
					case ChunkCategory::Painter:
					case ChunkCategory::Function: return 0;
					case ChunkCategory::Material:
					case ChunkCategory::Geometry:
					case ChunkCategory::Modifier:
					case ChunkCategory::Medium:
					case ChunkCategory::Shader:
					case ChunkCategory::ShaderOp: return 1;
					case ChunkCategory::Object:
					case ChunkCategory::Light:    return 2;
					default:                      return -1;   // append-at-end class
				}
			}

			//! R2 (2026-08-10): splice ONE generated chunk text into `doc` at its
			//! DECLARATION TIER -- the [leadSep "\n"][chunk][trailSep "\n"]
			//! anti-glue triple Job::ApplyCstInsertChunk uses, positioned by
			//! ScaffoldDeclarationTier_ above so a geometry chunk lands BEFORE the
			//! first object/light and therefore before the very object whose
			//! `geometry` slot this verb is about to rebind (scenes declare before
			//! use; an appended-at-end geometry would not derive).  Returns the
			//! spliced document; returns `doc` unchanged when the text does not
			//! parse to a chunk (impossible for generator output -- defensive).
			//!
			//! NO append-at-end fallback (unlike ApplyCstInsertChunk's second
			//! attempt): the whole candidate gets ONE dry-run, and for THIS verb
			//! append-at-end is not a useful fallback anyway -- it is precisely the
			//! position that cannot derive, since the consumer object is already in
			//! the document ahead of it.
			RISE::Cst::Document ScaffoldSpliceChunkTierPositioned_( const RISE::Cst::Document& doc,
			                                                        const std::string& chunkText )
			{
				RISE::Cst::Document chunkDoc = RISE::Cst::ParseToCst( chunkText );
				RISE::Cst::NodeRef  chunkItem;
				{
					const int n = RISE::Cst::DocItemCount( chunkDoc );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeRef it =
							RISE::Cst::DocResolveNodeId( chunkDoc, RISE::Cst::DocNodeIdAt( chunkDoc, i ) );
						if( it && it->kind == RISE::Cst::NodeKind::Chunk ) { chunkItem = it; break; }
					}
				}
				if( !chunkItem ) return doc;

				RISE::Cst::Document leadDoc  = RISE::Cst::ParseToCst( std::string( "\n" ) );
				RISE::Cst::NodeRef  leadItem = RISE::Cst::DocResolveNodeId( leadDoc, RISE::Cst::DocNodeIdAt( leadDoc, 0 ) );
				RISE::Cst::Document sepDoc   = RISE::Cst::ParseToCst( std::string( "\n" ) );
				RISE::Cst::NodeRef  sepItem  = RISE::Cst::DocResolveNodeId( sepDoc, RISE::Cst::DocNodeIdAt( sepDoc, 0 ) );
				if( !leadItem || !sepItem ) return doc;

				const int tier  = ScaffoldDeclarationTier_( chunkItem->role );
				const int endAt = RISE::Cst::DocItemCount( doc );
				int at = endAt;
				if( tier == 0 || tier == 1 ) {
					for( int i = 0; i < endAt; ++i ) {
						const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
						if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
						if( ScaffoldDeclarationTier_( it->role ) > tier ) { at = i; break; }
					}
				}

				RISE::Cst::Document work = doc;
				work = RISE::Cst::DocInsertItem( work, at,     leadItem );
				work = RISE::Cst::DocInsertItem( work, at + 1, chunkItem );
				work = RISE::Cst::DocInsertItem( work, at + 2, sepItem );
				return work;
			}

			//! R2 (2026-08-10): the bare display label for a chunk NodeRef -- its
			//! `name` when it has one, else its keyword (the SAME fallback
			//! AnalyzeRejectedRemove's referrer labelling uses).
			std::string ScaffoldChunkLabel_( const RISE::Cst::Document& doc, RISE::Cst::NodeId id )
			{
				const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId( doc, id );
				if( !it ) return std::string();
				const std::string namePath = RISE::Cst::ChunkNamePath( it );
				const std::string prefix   = it->role + "/";
				if( namePath.size() > prefix.size() && namePath.compare( 0, prefix.size(), prefix ) == 0 )
					return namePath.substr( prefix.size() );
				return it->role;
			}
		}

		AgentSession::AgentGeometryScaffoldResult AgentSession::InsertGeometryScaffold(
			const std::string& family, const std::string& name, double size, double detail, double aspect,
			const std::string& points, double taper, const std::string& tone,
			const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			AgentGeometryScaffoldResult out;
			out.family = family;
			// G2: folds a give-up notice into out.message no matter which of
			// this function's several `return out;` statements fires -- see
			// BuildPlanGiveUpFold_'s doc (above AgentSession::InsertChunk).
			BuildPlanGiveUpFold_ g2Fold{ out.message, std::string() };
			// S1 (2026-08-11): the phase machinery's own fold, a second instance
			// for the reason InsertChunk's identical pair documents.
			BuildPlanGiveUpFold_ s1Fold{ out.message, std::string() };

			// G2 (2026-08-10, build-plan gate): FIRST -- every geometry-scaffold
			// family expands to at least one Geometry-category chunk, so this
			// verb is unconditionally geometry-creating and needs no per-family
			// test.  Placed ahead of the family/param validation on purpose:
			// the gate's subject is the DECISION to build a part, which the
			// caller has already made by naming this verb, and a caller who
			// mistyped a family should not be told about a plan only on their
			// second attempt.  When the gate passes, the InsertChunks this
			// verb routes through sees a disarmed gate and cannot double-fire
			// (or double-notice, for the same reason).
			{
				const std::string clause = CheckBuildPlanGate_( "insert_geometry_scaffold", &g2Fold.notice );
				if( !clause.empty() ) {
					out.ok      = false;
					out.message = clause;
					return out;
				}
			}

			// S1 (2026-08-11, the staged build protocol): the COMPOSE-phase
			// creation refusal, immediately after the gate and on the same
			// terms -- this verb is unconditionally geometry-creating, so it
			// needs no per-family test either.  The InsertChunks it routes
			// through sees the phase rules already satisfied here and cannot
			// double-fire, exactly as with the gate.
			{
				const std::string clause = CheckComposePhaseForCreate_( "insert_geometry_scaffold",
				                                                         &s1Fold.notice );
				if( !clause.empty() ) {
					out.ok      = false;
					out.message = clause;
					return out;
				}
			}

			// R2 (2026-08-10): the family parse, the per-family param
			// validation and the graph expansion now live in the SHARED
			// ScaffoldValidateAndBuildGeometry (extracted verbatim from here,
			// verb-name-parameterized) so replace_geometry_scaffold expands
			// through the IDENTICAL path -- see that helper's doc.
			GeometryScaffoldFamily fam = GeometryScaffoldFamily::DisplacedSlab;
			GeoScaffoldGraph graph;
			{
				std::string err;
				if( !ScaffoldValidateAndBuildGeometry( "insert_geometry_scaffold", family, name,
				                                       size, detail, aspect, points, taper, tone,
				                                       fam, graph, err ) ) {
					out.ok      = false;
					out.message = err;
					return out;
				}
			}

			// Collision precheck: refuse the WHOLE expansion, document
			// UNCHANGED, if ANY generated (kind,name) already exists AT
			// SNAPSHOT TIME -- the IDENTICAL precheck (and the IDENTICAL
			// TOCTOU/best-effort hedge) InsertMaterialScaffold's own
			// precheck comment documents in full above; not restated
			// verbatim here to avoid the two copies drifting -- read that
			// comment for what this precheck DOES and does NOT guarantee.
			{
				const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
				if( snap.hasDocument ) {
					const RISE::Cst::Document headDoc = RISE::Cst::ParseToCst( snap.document );
					for( const GeoScaffoldChunkEntry& c : graph.chunks ) {
						const RISE::Cst::NodeId id = RISE::Cst::DocFindByName( headDoc, c.kind + "/" + c.name );
						if( id != 0 ) {
							out.ok = false;
							out.message = "insert_geometry_scaffold refused: a `" + c.kind + "` named `" + c.name +
								"` already exists -- every generated chunk is named tmpl_<name>_<role>, "
								"and this collides; pick a different `name` -- document unchanged";
							return out;
						}
					}
				}
			}

			std::vector<std::string> chunkTexts;
			chunkTexts.reserve( graph.chunks.size() );
			for( const GeoScaffoldChunkEntry& c : graph.chunks ) chunkTexts.push_back( c.text );

			out.chunkResults = InsertChunks( chunkTexts, baseOrNull );
			out.ok           = true;
			out.geometryName = graph.geometryName;
			out.geometryKind = graph.geometryKind;
			out.materialName = graph.materialName;
			out.materialKind = graph.materialKind;
			out.mediumName   = graph.mediumName;
			out.mediumKind   = graph.mediumKind;
			out.objectName   = graph.objectName;
			out.objectKind   = graph.objectKind;
			if( fam == GeometryScaffoldFamily::VolumeBank ) {
				// Factual (not advisory) wiring note -- see BuildVolumeBank's
				// doc for why this coupling exists.  message stays empty on
				// an ok==true result for every OTHER family, unchanged.
				out.message = "volume_bank emitted its own `" + graph.objectKind + "` (`" + graph.objectName +
					"`) because painter_heterogeneous_medium's bbox_min/bbox_max are WORLD-SPACE and fixed at "
					"creation time -- if you reposition or rescale that object, also update `" + graph.mediumName +
					"`'s bbox_min/bbox_max to match, or the density field will no longer align with the container.";
			}
			return out;
		}

		AgentSession::AgentGeometryScaffoldResult AgentSession::ReplaceGeometryScaffold(
			const std::string& target, const std::string& family, const std::string& name,
			double size, double detail, double aspect,
			const std::string& points, double taper, const std::string& tone,
			const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			AgentGeometryScaffoldResult out;
			out.family         = family;
			out.replacedObject = target;
			// G2: folds a give-up notice into out.message no matter which of
			// this function's several `return out;` statements fires -- see
			// BuildPlanGiveUpFold_'s doc (above AgentSession::InsertChunk).
			BuildPlanGiveUpFold_ g2Fold{ out.message, std::string() };
			// S1 (2026-08-11): the phase machinery's own fold -- see
			// InsertChunk's identical pair for why it is a second instance.
			BuildPlanGiveUpFold_ s1Fold{ out.message, std::string() };

			// G2 (2026-08-10, build-plan gate): FIRST, for the same reasons the
			// insert sibling states -- this verb is unconditionally
			// geometry-creating, and a refusal here leaves `status` empty /
			// `ok` false, i.e. the pre-commit refusal shape every other
			// validation return in this method already produces (see the
			// section-(1) note directly below: nothing is touched on ANY
			// failure before the single commit).
			{
				const std::string clause = CheckBuildPlanGate_( "replace_geometry_scaffold", &g2Fold.notice );
				if( !clause.empty() ) {
					out.message = clause;
					return out;
				}
			}

			// S1 (2026-08-11, the staged build protocol): this verb is BOTH an
			// edit of `target` and a geometry creation, so it takes BOTH arms --
			// mutually exclusive by phase, so at most one can fire.
			//
			// (a) CROSS-ELEMENT: rebinding the geometry of an object created
			// under another element reaches into that element's window exactly
			// as a propose_patch on the same object would.
			{
				const std::string clause = CheckElementWindowForEdit_( "replace_geometry_scaffold",
				                                                        target, &s1Fold.notice );
				if( !clause.empty() ) {
					out.message = clause;
					return out;
				}
			}
			// (b) COMPOSE: it creates new geometry, unconditionally.
			{
				const std::string clause = CheckComposePhaseForCreate_( "replace_geometry_scaffold",
				                                                         &s1Fold.notice );
				if( !clause.empty() ) {
					out.message = clause;
					return out;
				}
			}
			// (c) S2 fix-round (2026-08-11, P1): the FIRST-GEOMETRY clean-room
			// check -- InsertGeometryScaffold gets this transitively through
			// InsertChunks, but this verb splices the CST and commits by its
			// own path (never reaching InsertChunk/InsertChunks), so without
			// this call an active element with no attributed chunks could get
			// its first geometry landed here instead of through
			// build_element, then have the gate's "any attributed chunk lifts
			// this permanently" rule disarm the clean room for that element
			// for the rest of the session -- exactly the bypass design sec 4
			// exists to prevent.  Unconditional: this verb always creates
			// geometry, so it needs no ChunkTextCreatesGeometry_ predicate
			// (unlike InsertChunk, which must distinguish geometry-creating
			// text from everything else it also accepts).
			{
				const std::string clause = CheckFirstGeometryThroughCleanRoom_( "replace_geometry_scaffold",
				                                                                  &s1Fold.notice );
				if( !clause.empty() ) {
					out.message = clause;
					return out;
				}
			}

			// ---- (1) Request validation + expansion.  Nothing is touched on any failure below; every
			// return in this whole method before the single commit leaves the document, the head version,
			// the history and the proposal queue byte-identical.
			if( target.empty() ) {
				out.message = "replace_geometry_scaffold refused: `target` must name the standard_object whose "
					"geometry slot to rebind";
				return out;
			}

			// volume_bank emits its OWN standard_object (plus a material and a medium) because
			// painter_heterogeneous_medium's bbox is WORLD-SPACE and fixed at creation time -- see
			// InsertGeometryScaffold's doc.  There is therefore no existing object's geometry slot for it to
			// rebind, and rebinding one to its container geometry would silently strand the medium.  Refuse
			// with the route that DOES work.  Keyed on the LITERAL family string, BEFORE the shared
			// validate-and-expand below, for two reasons: a caller who picked an unavailable family must not
			// first be sent to fix that family's OWN params (a missing `tone` here is not the problem), and
			// there is no point expanding a graph that is about to be discarded.  An UNKNOWN family string
			// can never equal "volume_bank", so this cannot intercept the "valid families are ..." message.
			if( family == "volume_bank" ) {
				out.message = "replace_geometry_scaffold refused: family `volume_bank` emits its OWN "
					"standard_object (plus a dielectric material and a medium, wired to a WORLD-SPACE density "
					"bbox), so there is no existing object's `geometry` slot to rebind -- use "
					"insert_geometry_scaffold for volume_bank, then remove the object you were replacing if "
					"you no longer want it -- document unchanged";
				return out;
			}

			GeometryScaffoldFamily fam = GeometryScaffoldFamily::DisplacedSlab;
			GeoScaffoldGraph graph;
			{
				std::string err;
				if( !ScaffoldValidateAndBuildGeometry( "replace_geometry_scaffold", family, name,
				                                       size, detail, aspect, points, taper, tone,
				                                       fam, graph, err ) ) {
					out.message = err;
					return out;
				}
			}

			// ---- (2) Snapshot the head ONCE.  Everything below is computed against THESE bytes, and the
			// commit re-checks that the head is still exactly this version (see step 7) -- so the candidate
			// can never be committed on top of a head that moved underneath it.
			const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
			if( !snap.hasDocument ) {
				out.message = "replace_geometry_scaffold refused: no retained CST Document -- this verb needs a "
					"CST-loaded head";
				return out;
			}
			if( baseOrNull && *baseOrNull != snap.headVersion ) {
				// R2 fix-round (P1): a stale caller-supplied base is a CONFLICT, not a request-validity
				// failure -- the SAME distinction ProposePatch's identical precondition draws
				// (AgentPatchResult::status=="conflict", applied==false).  `ok` stays true: the request
				// itself was well-formed and reached a commit-stage disposition; `status`/`retriable`/
				// `headVersion` carry the actual disposition so AgentRpc.cpp returns MakeSuccess rather
				// than folding this into an "invalid params" error.
				char buf[192];
				std::snprintf( buf, sizeof( buf ),
					"replace_geometry_scaffold refused: baseHeadVersion does not match the current head "
					"(revision %llu) -- re-read and re-propose -- document unchanged",
					static_cast<unsigned long long>( snap.headVersion.revision ) );
				out.ok          = true;
				out.status      = "conflict";
				out.retriable   = false;
				out.headVersion = snap.headVersion;
				out.message     = buf;
				return out;
			}

			const RISE::Cst::Document headDoc = RISE::Cst::ParseToCst( snap.document );

			// ---- (3) Resolve `target`.  Bare-name, unique-or-refuse -- the SAME DocFindByNameAnyRole
			// resolution ProposePatch/remove_chunk use, with NO kind narrowing parameter (the required kind
			// IS standard_object) and NO positional fallback (that exists for cameras, which this verb can
			// never address).  Resolving with an EMPTY kind constraint deliberately: it is what lets the
			// "you passed a geometry chunk, not an object" case below be diagnosed SPECIFICALLY instead of
			// coming back as an indistinguishable "not found".
			int occ = 0;
			const RISE::Cst::NodeId targetId =
				RISE::Cst::DocFindByNameAnyRole( headDoc, target, &occ, "", /*uniqueFallback*/ false );
			if( targetId == 0 ) {
				if( occ > 1 ) {
					char buf[224];
					std::snprintf( buf, sizeof( buf ),
						"replace_geometry_scaffold refused: `%s` is ambiguous -- %d chunks share that name; "
						"rename one, or address the object by a unique name -- document unchanged",
						target.c_str(), occ );
					out.message = buf;
					return out;
				}
				std::string m = "replace_geometry_scaffold refused: no chunk named `" + target +
					"` -- `target` is the standard_object whose geometry slot to rebind";
				const std::vector<std::string> near =
					RankNearMisses( CollectTargetNameCandidates( headDoc, std::string() ), target );
				if( !near.empty() ) {
					m += "; did you mean ";
					for( std::size_t i = 0; i < near.size(); ++i ) {
						if( i ) m += ", ";
						m += "`" + near[i] + "`";
					}
					m += "?";
				}
				m += " -- document unchanged";
				out.message = m;
				return out;
			}
			const RISE::Cst::NodeRef targetItem = RISE::Cst::DocResolveNodeId( headDoc, targetId );
			if( !targetItem ) {
				out.message = "replace_geometry_scaffold refused: `" + target + "` did not resolve to a chunk "
					"-- document unchanged";
				return out;
			}
			if( targetItem->role != "standard_object" ) {
				// THE LIKELY MODEL MISTAKE: passing the GEOMETRY chunk's name (the thing being replaced)
				// rather than the OBJECT's.  A generic "not a standard_object" would cost a whole turn, so
				// name the object(s) that actually consume this geometry -- that IS the argument the caller
				// meant to pass.
				const ChunkDescriptor* d = DescriptorForKeyword( String( targetItem->role.c_str() ) );
				if( d && d->category == ChunkCategory::Geometry ) {
					std::vector<std::string> consumers;
					{
						const RISE::Cst::ReferenceGraph g = RISE::Cst::BuildReferenceGraph( headDoc, nullptr, nullptr );
						const std::map<RISE::Cst::NodeId, std::set<RISE::Cst::NodeId> >::const_iterator dep =
							g.dependents.find( targetId );
						if( dep != g.dependents.end() ) {
							for( const RISE::Cst::NodeId refId : dep->second ) {
								const RISE::Cst::NodeRef refItem = RISE::Cst::DocResolveNodeId( headDoc, refId );
								if( !refItem || refItem->role != "standard_object" ) continue;
								const std::string lbl = ScaffoldChunkLabel_( headDoc, refId );
								if( !lbl.empty() ) consumers.push_back( lbl );
							}
						}
					}
					std::string m = "replace_geometry_scaffold refused: `" + target + "` is a `" +
						targetItem->role + "` (a GEOMETRY chunk), not the object that uses it -- `target` must "
						"name the standard_object whose `geometry` slot to rebind";
					if( !consumers.empty() ) {
						m += "; the standard_object" + std::string( consumers.size() == 1 ? "" : "s" ) +
							" using this geometry: ";
						for( std::size_t i = 0; i < consumers.size(); ++i ) {
							if( i ) m += ", ";
							m += "`" + consumers[i] + "`";
						}
						m += " -- pass " + std::string( consumers.size() == 1 ? "that" : "one of those" ) +
							" instead";
					} else {
						m += "; nothing currently references it, so there is no slot to rebind -- use "
							"insert_geometry_scaffold and wire the object yourself";
					}
					m += " -- document unchanged";
					out.message = m;
					return out;
				}
				out.message = "replace_geometry_scaffold refused: `" + target + "` is a `" + targetItem->role +
					"`, not a `standard_object` -- `target` must name the object whose `geometry` slot to "
					"rebind -- document unchanged";
				return out;
			}

			const std::string oldGeomName = ChunkParamString_( targetItem, "geometry" );
			if( oldGeomName.empty() ) {
				out.message = "replace_geometry_scaffold refused: `" + target + "` has no `geometry` param to "
					"rebind -- add one with propose_patch, or use insert_geometry_scaffold and wire it "
					"yourself -- document unchanged";
				return out;
			}
			out.previousGeometryName = oldGeomName;

			// ---- (4) Name-collision precheck: refuse the WHOLE expansion, document UNCHANGED, if ANY
			// generated (kind,name) already exists AT SNAPSHOT TIME.  The IDENTICAL precheck (and the
			// IDENTICAL TOCTOU hedge) InsertGeometryScaffold's own precheck comment documents -- except that
			// here the TOCTOU hedge is not even needed: the commit's conflict gate (step 7) refuses outright
			// if the head moved between this check and the swap.
			for( const GeoScaffoldChunkEntry& c : graph.chunks ) {
				if( RISE::Cst::DocFindByName( headDoc, c.kind + "/" + c.name ) != 0 ) {
					out.message = "replace_geometry_scaffold refused: a `" + c.kind + "` named `" + c.name +
						"` already exists -- every generated chunk is named tmpl_<name>_<role>, and this "
						"collides; pick a different `name` -- document unchanged";
					return out;
				}
			}

			// ---- (5) Build the CANDIDATE document: splice in every generated chunk (declaration-tier
			// positioned, so the new geometry is declared ahead of the object that will reference it),
			// rebind the object's `geometry` slot, then erase the old geometry chunk IF it is now
			// unreferenced.  One document, committed once -- that is the whole atomicity story.
			RISE::Cst::Document work = headDoc;
			for( const GeoScaffoldChunkEntry& c : graph.chunks )
				work = ScaffoldSpliceChunkTierPositioned_( work, c.text );

			// Re-resolve the object in the SPLICED document rather than reusing `targetId`: the splices
			// produced a new Document value, and re-resolving by the canonical (kind,name) path is both
			// cheap and immune to any NodeId-stability assumption.
			const RISE::Cst::NodeId objIdInWork =
				RISE::Cst::DocFindByName( work, std::string( "standard_object/" ) + target );
			if( objIdInWork == 0 ) {
				out.message = "replace_geometry_scaffold refused: internal -- `" + target + "` could not be "
					"re-resolved after the scaffold splice; document unchanged";
				return out;
			}
			work = RISE::Cst::DocSetParamValue( work, objIdInWork, "geometry", 0, graph.geometryName );

			// ORPHAN POLICY (see the header doc for the full rationale).  After the rebind, ask the
			// reference graph who still points at the OLD geometry chunk.  Nobody -> erase it in this same
			// candidate.  Somebody -> RETAIN it and name the referrers.  Either way, report (never remove)
			// the chunks that were referenced ONLY by it -- exactly the set whose `dependents` is the
			// singleton {old geometry}, which is what "this chunk existed only to feed the discarded form"
			// means in graph terms.
			{
				const RISE::Cst::NodeId oldGeomId =
					RISE::Cst::DocFindByNameAnyRole( work, oldGeomName, nullptr, "", false );
				const RISE::Cst::NodeRef oldItem =
					( oldGeomId != 0 ) ? RISE::Cst::DocResolveNodeId( work, oldGeomId ) : RISE::Cst::NodeRef();
				// KIND VERIFICATION before anything DESTRUCTIVE -- the defensive pattern Job.cpp's
				// CstResolveRemoveTarget_ applies for the same reason.  The bare-name lookup above
				// carries no kind constraint DELIBERATELY (a same-named chunk of another kind then
				// comes back AMBIGUOUS, id 0, instead of being mistaken for the geometry) -- but a
				// DANGLING `geometry` value that happens to match some unrelated uniquely-named chunk
				// would otherwise resolve here and be ERASED.  Only ever touch a chunk the descriptor
				// registry itself calls Geometry.
				const ChunkDescriptor* oldDesc =
					oldItem ? DescriptorForKeyword( String( oldItem->role.c_str() ) ) : nullptr;
				if( oldItem && oldDesc && oldDesc->category == ChunkCategory::Geometry )
				{
					out.previousGeometryKind = oldItem->role;

					const RISE::Cst::ReferenceGraph g = RISE::Cst::BuildReferenceGraph( work, nullptr, nullptr );
					const std::map<RISE::Cst::NodeId, std::set<RISE::Cst::NodeId> >::const_iterator dep =
						g.dependents.find( oldGeomId );
					const bool stillReferenced = ( dep != g.dependents.end() && !dep->second.empty() );

					if( stillReferenced ) {
						for( const RISE::Cst::NodeId refId : dep->second ) {
							const std::string lbl = ScaffoldChunkLabel_( work, refId );
							if( !lbl.empty() ) out.previousGeometryReferrers.push_back( lbl );
						}
					}
					else {
						// Deeper orphans, computed BEFORE the erase (afterwards the edges are gone).
						for( const std::pair<const RISE::Cst::NodeId, std::set<RISE::Cst::NodeId> >& kv : g.dependents ) {
							if( kv.second.size() != 1 || *kv.second.begin() != oldGeomId ) continue;
							const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId( work, kv.first );
							if( !it ) continue;
							const std::string lbl = ScaffoldChunkLabel_( work, kv.first );
							if( lbl.empty() || lbl == it->role ) continue;   // unnamed -> not addressable by remove_chunks
							out.reportedOrphans.push_back( it->role + "/" + lbl );
						}
						const int idx = RISE::Cst::DocIndexOfNodeId( work, oldGeomId, nullptr );
						if( idx >= 0 ) {
							work = RISE::Cst::DocEraseChunkTidy( work, idx );
							out.previousGeometryRemoved = true;
						}
						else {
							// No top-level index (should be impossible for a resolved chunk) -- leave it
							// rather than erase something else, and report honestly.
							out.reportedOrphans.clear();
						}
					}
				}
			}

			const std::string candidateText = RISE::Cst::SerializeCst( work );
			if( candidateText.empty() ) {
				out.message = "replace_geometry_scaffold refused: internal -- the candidate document "
					"serialized to nothing; document unchanged";
				return out;
			}

			// ---- (6) GATES against the CANDIDATE document.  This is a CREATION path, so both post-arc
			// enforcement gates are evaluated here rather than argued away -- see the header doc.  Neither
			// can fire through R2's own mutation today (no geometry family emits a rasterizer chunk, and no
			// family emits or edits a csg_object), which is precisely why they are cheap: the rasterizer arm
			// is a keyword multiset compare, and the emitter arm short-circuits on a CST-only scan for any
			// csg_object at all before paying a derive.
			{
				const std::string clause = DescribeNewlyBlockedRasterizerDelta_( headDoc, work );
				if( !clause.empty() ) {
					out.message = "replace_geometry_scaffold refused: " + clause;
					return out;
				}
			}
			{
				// DELTA, not state (the E1 lesson -- see CheckNonSamplingEmitterGateForPatch's arm C): a
				// csg_object that was ALREADY an unacknowledged null-geometry emitter on the head is not
				// this edit's doing and must not freeze every future form revision in the scene.
				std::vector<std::string> csgNames;
				{
					const int n = RISE::Cst::DocItemCount( work );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeRef it =
							RISE::Cst::DocResolveNodeId( work, RISE::Cst::DocNodeIdAt( work, i ) );
						if( !it || it->kind != RISE::Cst::NodeKind::Chunk || it->role != "csg_object" ) continue;
						const std::string n2 = ChunkParamString_( it, "name" );
						if( !n2.empty() ) csgNames.push_back( n2 );
					}
				}
				if( !csgNames.empty() ) {
					const std::vector<std::string> candHits = FindUnacknowledgedNullGeometryEmitters_( work, csgNames );
					if( !candHits.empty() ) {
						const std::vector<std::string> headHits =
							FindUnacknowledgedNullGeometryEmitters_( headDoc, csgNames );
						std::vector<std::string> created;
						for( const std::string& nm : candHits ) {
							bool preExisting = false;
							for( const std::string& h : headHits ) if( h == nm ) { preExisting = true; break; }
							if( !preExisting ) created.push_back( nm );
						}
						if( !created.empty() ) {
							out.message = "replace_geometry_scaffold refused: " +
								DescribeUnacknowledgedNullGeometryEmitters_( created );
							return out;
						}
					}
				}
			}

			// ---- (7) COMMIT: ONE whole-document swap, ONE dry-run-guarded re-derive, ONE head bump.
			// `snap.headVersion` is passed as the base UNCONDITIONALLY (even when the caller omitted
			// baseHeadVersion): the candidate above was computed outside the commit lock, so committing it
			// against a head that moved would silently clobber a co-editor.  See the header doc.
			AgentChunkResult commit;
			commit.name = target;
			commit.kind = "standard_object";

			if( mAuthority == AgentAuthority::External )
			{
				// No staging path for this verb: an AgentProposal replays ONE of the four
				// AgentProposalKind verbs, and a composite whole-document swap is none of them -- an Owner
				// approving it card-by-card is not even a meaningful operation, and inventing a
				// "replay the scaffold generator later" kind would re-run generation against a DIFFERENT
				// head than the one the candidate was computed for.  Refuse with the two-call route that
				// DOES stage cleanly.  Document byte-identical.
				out.message = "replace_geometry_scaffold refused: this session is External-authority, and this "
					"verb has no staged-proposal form (it is ONE composite document swap, not a single "
					"chunk edit an Owner can approve card-by-card) -- do it in two staged steps instead: "
					"insert_geometry_scaffold to create the new geometry, then propose_patch on `" + target +
					"`'s `geometry` param to rebind it -- document unchanged";
				return out;
			}

			if( mController )
			{
				const SceneEditController::AgentCommitResult cr =
					mController->ApplyAgentReplaceGeometry( String( target.c_str() ),
					                                        String( candidateText.c_str() ),
					                                        &snap.headVersion );
				commit.applied     = cr.applied;
				commit.retriable   = cr.retriable;
				commit.rawCode     = cr.rawCode;
				commit.status      = cr.status.c_str();
				commit.headVersion = cr.headVersion;
				commit.message     = cr.message.c_str();
			}
			else if( !mJob || !mJob->HasRetainedCstDocument() )
			{
				out.message = "replace_geometry_scaffold refused: no retained CST Document -- this verb needs a "
					"CST-loaded head";
				return out;
			}
			else
			{
				// HEADLESS (direct-Job).  The conflict gate the controller applies under its lock is applied
				// here too -- single-threaded in practice, but the invariant ("the candidate is committed
				// against exactly the head it was computed from, or not at all") is the verb's, not the
				// controller's.
				const RISE::Cst::CstHeadVersion cur = mJob->GetCstHeadVersion();
				if( cur != snap.headVersion ) {
					// R2 fix-round (P1): same reclassification as the explicit-baseOrNull check in step
					// (2) -- a head that moved underneath the headless commit is a CONFLICT, not a
					// request-validity failure.  See this method's header doc, CONCURRENCY.
					char buf[192];
					std::snprintf( buf, sizeof( buf ),
						"replace_geometry_scaffold refused: the head moved (revision %llu) while the "
						"replacement was being composed -- re-read and retry -- document unchanged",
						static_cast<unsigned long long>( cur.revision ) );
					out.ok          = true;
					out.status      = "conflict";
					out.retriable   = false;
					out.headVersion = cur;
					out.message     = buf;
					// R2 fix-round (P2, review round 2): this early return SKIPS the shared
					// !commit.applied block below, which is where a non-applied outcome clears the
					// step-(5) disposition fields.  Those fields were computed against the CANDIDATE
					// document -- they describe a plan that did NOT happen here -- so clear them
					// explicitly, exactly as lines ~6248-6250 do for the controller-mediated path.
					// Leaving them set would contradict this very message ("document unchanged") with
					// a `previousGeometry.removed:true` a model may act on.
					out.previousGeometryRemoved = false;
					out.previousGeometryReferrers.clear();
					out.reportedOrphans.clear();
					return out;
				}
				char diagBuf[512]; diagBuf[0] = '\0';
				const int code = mJob->ApplyCstReplaceDocumentText( candidateText.c_str(),
				                                                    /*restoreActiveRasterizer*/ true,
				                                                    diagBuf, sizeof( diagBuf ),
				                                                    "replace_geometry_scaffold" );
				commit.rawCode     = ( code < 0 ) ? 0 : code;
				commit.headVersion = mJob->GetCstHeadVersion();
				if( code == 2 ) {
					commit.applied = true;
					commit.status  = "applied";
					commit.message = "geometry replaced via a single full re-derive (Scene + managers were replaced)";
				}
				else if( code == 3 ) {
					commit.applied = false;
					commit.status  = "diagnosed";
					commit.message = "geometry replacement NOT a clean success: the Document was mutated and the "
						"live managers were replaced, BUT the full re-derive emitted diagnostics (see log) -- "
						"do NOT treat as applied";
				}
				else {
					commit.applied = false;
					commit.status  = "rejected";
					std::string m = "geometry replacement rejected (NOTHING changed): the candidate document "
						"would not derive -- head unchanged";
					if( diagBuf[0] ) { m += ": "; m += diagBuf; }
					commit.message = m;
				}
			}

			// ---- (8) Report.  `chunkResults` keeps the shape every scaffold caller already handles -- one
			// entry per GENERATED chunk, in generation order -- but, unlike insert_geometry_scaffold's, these
			// are NOT independent verdicts: the whole call is one atomic mutation, so every entry carries the
			// SAME verdict, status and head version.  A caller that checks each element still reads the truth.
			out.chunkResults.reserve( graph.chunks.size() );
			for( const GeoScaffoldChunkEntry& c : graph.chunks ) {
				AgentChunkResult e = commit;
				e.name = c.name;
				e.kind = c.kind;
				out.chunkResults.push_back( e );
			}

			// S1 (2026-08-11): attribute every chunk this ONE atomic mutation
			// landed to the active element.  Explicit here, unlike the insert
			// verbs, because this verb rewrites the document itself instead of
			// routing through InsertChunk -- so nothing else would record it.
			// `diagnosed` counts: code 3 DID mutate (see the block just below),
			// so those chunks are in the document and must carry an element like
			// any other -- which is exactly what ResultMutatedDocument_ says
			// (S1 fix-round 2026-08-11: this site was the ONE of five that had
			// the condition right, and it is now the shared predicate).
			if( ResultMutatedDocument_( commit ) ) {
				// S1 fix-round (2026-08-11, P1): the ERASED previous geometry
				// must lose its attribution on the SAME condition the erase
				// happened under.  Step (5) removed the chunk from the document
				// when nothing referenced it any more; an attribution entry that
				// outlives it can do exactly one thing -- make
				// CheckElementWindowForEdit_, which is a pure attribution lookup
				// that runs BEFORE any document read, refuse a later edit naming
				// a chunk that does not exist.  That refusal is unfixable by the
				// reopen_element it recommends, and it spends one of the three
				// SHARED phase-refusal slots.  Dropped BEFORE the attribution
				// loop so a generated chunk that reuses the erased name (only
				// possible if the erase and the splice disagreed, which they
				// cannot) would still end up attributed, never dropped.
				if( out.previousGeometryRemoved )
					DropChunkAttribution_( oldGeomName );
				for( const GeoScaffoldChunkEntry& c : graph.chunks )
					AttributeChunkToActiveElement_( c.name, c.kind );
			}

			// R2 fix-round (P1): every path that reaches here ATTEMPTED a commit -- `ok` reflects that (the
			// request was well-formed and submitted), matching InsertGeometryScaffold's identical "ok is not
			// a promise every chunk landed" hedge; `status`/`retriable`/`headVersion` carry the ACTUAL
			// disposition (applied / rejected / conflict / diagnosed) so a caller can branch on it without
			// digging into chunkResults[0], and AgentRpc.cpp returns MakeSuccess for every one of these --
			// MakeError is reserved for the pre-commit refusals that returned before this point.
			out.ok          = true;
			out.status      = commit.status;
			out.retriable   = commit.retriable;
			out.headVersion = commit.headVersion;

			if( !commit.applied ) {
				if( commit.status == "diagnosed" ) {
					// R2 fix-round (P2): code 3 DID mutate the Document -- the new chunk(s) landed, the
					// slot was rebound, the old geometry's disposition below is whatever step (5) actually
					// computed against the SPLICED document (BEFORE this commit, but that IS what landed),
					// RebindEditorToJob ran, and a real undo record was pushed.  Unlike a true refusal,
					// nothing here is a "plan that did not happen" -- report it as reality, not as if
					// nothing changed.  previousGeometryRemoved/previousGeometryReferrers/reportedOrphans
					// were already computed correctly in step (5) against `work`; leave them alone.
					out.geometryName = graph.geometryName;
					out.geometryKind = graph.geometryKind;
					out.message = "replace_geometry_scaffold: " + commit.message + " -- the Document WAS "
						"mutated (the new geometry landed and the object's `geometry` slot was rebound; the "
						"previous-geometry disposition in this response is real, not a discarded plan) even "
						"though the re-derive diagnosed -- treat as a mutation that needs a look at the log, "
						"not as unchanged.";
				}
				else {
					// A true refusal (rejected / conflict / a transient retriable reject) -- nothing was
					// touched, so none of the disposition fields describe reality.  Clear them rather than
					// report a plan that did not happen.
					out.previousGeometryRemoved = false;
					out.previousGeometryReferrers.clear();
					out.reportedOrphans.clear();
					out.message = "replace_geometry_scaffold: " + commit.message;
				}
				return out;
			}

			out.geometryName = graph.geometryName;
			out.geometryKind = graph.geometryKind;

			// A factual (non-advisory) account of what the one mutation did -- the model needs the old
			// chunk's disposition to know whether it still has cleanup to do.
			std::string m = "`" + target + "`.geometry rebound to the new `" + graph.geometryKind + "` `" +
				graph.geometryName + "` (" + std::to_string( graph.chunks.size() ) +
				" chunk" + ( graph.chunks.size() == 1 ? "" : "s" ) + " added); the object's transform and every "
				"other param are unchanged.";
			if( out.previousGeometryRemoved ) {
				m += " The previous geometry `" + oldGeomName + "` was unreferenced after the rebind and was "
					"removed in the same edit.";
			}
			else if( !out.previousGeometryReferrers.empty() ) {
				m += " The previous geometry `" + oldGeomName + "` was RETAINED because it is still referenced by ";
				for( std::size_t i = 0; i < out.previousGeometryReferrers.size(); ++i ) {
					if( i ) m += ", ";
					m += "`" + out.previousGeometryReferrers[i] + "`";
				}
				m += ".";
			}
			else {
				m += " The previous geometry `" + oldGeomName + "` was left in place.";
			}
			if( !out.reportedOrphans.empty() ) {
				m += " Now unreferenced and NOT removed (this verb removes only the geometry chunk it "
					"unbound, never deeper): ";
				for( std::size_t i = 0; i < out.reportedOrphans.size(); ++i ) {
					if( i ) m += ", ";
					m += "`" + out.reportedOrphans[i] + "`";
				}
				m += " -- pass them to remove_chunks if you want them gone.";
			}
			out.message = m;
			return out;
		}

		//--------------------------------------------------------------------
		// G2 (2026-08-10): the build-plan gate.  See AgentSession.h's block
		// above FileBuildPlan for what it is and why; this is the whole
		// implementation, four call sites aside (InsertChunk, InsertChunks,
		// InsertGeometryScaffold, ReplaceGeometryScaffold).
		//
		// NOTHING here reads or writes the Document, takes a controller lock,
		// or derives -- the gate is pure per-session bookkeeping plus one CST
		// parse of the caller's own chunk text.
		//--------------------------------------------------------------------

		const char* const AgentSession::kBuildPlanConstructionValues[6] =
		{
			"primitive", "csg", "sweep", "chain", "displaced", "mesh"
		};

		const char* const AgentSession::kBuildPlanViewValues[3] = { "front", "side", "top" };
		const char* const AgentSession::kBuildPlanDefaultView   = "front";

		namespace
		{
			//! G2 (2026-08-10): the PROCESS-WIDE gate default -- see
			//! AgentSession::SetBuildPlanGateDefaultEnabled's doc.  TRUE by
			//! default, deliberately: a construction site nobody remembered to
			//! touch gets the gate, which is the fail-safe polarity (the
			//! inverse -- default off, enabled per host -- would make a
			//! forgotten host silently opt out of the measurement, the exact
			//! fail-open shape IsReadSafeVerb's doc argues against).
			std::atomic<bool> gBuildPlanGateDefaultEnabled_{ true };

			//! S1 (2026-08-11): the PROCESS-WIDE staged-build-protocol default
			//! -- see AgentSession::SetBuildProtocolDefaultEnabled's doc.  TRUE
			//! for the SAME fail-safe reason the gate default is: a host nobody
			//! remembered to touch runs the protocol rather than silently
			//! opting out of the measurement it exists to produce.
			std::atomic<bool> gBuildProtocolDefaultEnabled_{ true };

			//----------------------------------------------------------------
			// G3a (2026-08-10): the sketch rasterizer.
			//
			// Model-authored numbers in, host-computed mask bytes out.  Pure
			// in-memory arithmetic: no file, no scene, no Document, no lock,
			// no rasterizer -- the same "nothing but per-session bookkeeping"
			// property the G2 gate has, which is why file_build_plan stays
			// READ-SAFE with an image in its result.
			//----------------------------------------------------------------

			//! FORWARD DECLARATION, not a second helper.  The PNG encoder is
			//! this same anonymous namespace's EncodeLinearPassthroughPng_,
			//! DEFINED further down alongside compare_to_reference's composite
			//! (search "compare_to_reference visual=true: encode").  Declared
			//! here only because FileBuildPlan is implemented above that point;
			//! reusing it is the point -- the sketch composite and the
			//! reference-diff composite must encode identically or two
			//! "PNG bytes" in the same result set would mean two things.
			std::vector<unsigned char> EncodeLinearPassthroughPng_(
				const std::vector<RISEColor>& pels, unsigned int w, unsigned int h );

			//! One parsed outline vertex, in the model's own 2D coordinates.
			struct SketchPoint_ { double x; double y; };

			//! G3a: the sane coordinate bound, and NOT copied verbatim from
			//! ScaffoldParsePoints' identical-looking cap -- the standing
			//! question "what invariant does the copied idiom depend on?"
			//! has a DIFFERENT answer here.  There the cap guards a
			//! world-space spline against a point that swamps the curve.
			//! Here every coordinate is normalized by the outline's OWN bbox,
			//! so magnitude alone is harmless -- what is NOT harmless is
			//! `RasterizeElementOutline_`'s fit-scale arithmetic in BOTH
			//! directions: `maxX - minX` OVERFLOWING to +inf for coordinates
			//! near the double range (1e300 - -1e300) is the direction this
			//! cap guards; a bbox extent UNDERFLOWING toward zero (a
			//! subnormal, e.g. 1e-320) makes `scale = fillFraction * canvas /
			//! extent` OVERFLOW to +inf the other way -- kSketchMinExtent_
			//! below is what guards THAT direction.  Either overflow makes
			//! every device coordinate NaN, every edge test false, and the
			//! mask silently empty while reporting an honest-looking
			//! areaFraction 0.00.  Same failure mode, two independent causes,
			//! two independent bounds.
			const double kSketchMaxCoord_ = 1e6;
			//! G3a fix-round (2026-08-10): the minimum bounding-box extent
			//! (on EITHER axis) an outline may have.  1e-9 is far above any
			//! plausible authored precision (no one is drawing a silhouette
			//! a billionth of a canvas unit wide) and far below where
			//! `kElementSketchFillFraction * kElementSketchCanvas / extent` can
			//! overflow a double -- so it rejects only the pathological case,
			//! never a legitimate small-but-sane outline.
			const double kSketchMinExtent_ = 1e-9;

			//! Parse "x y; x y; x y[; ...]" into >= 3 finite points with a
			//! non-degenerate bounding box.  Mirrors ScaffoldParsePoints'
			//! segment handling deliberately (one blank leading/trailing
			//! segment tolerated so a trailing `;` is not an error; a doubled
			//! `;;` in the MIDDLE fails the "exactly two numbers" check
			//! rather than being silently skipped, so a typo cannot quietly
			//! change the point count).  `err` is a lowercase phrase that
			//! reads correctly after "elements[i].outline ".
			bool ParseElementOutlinePoints_( const std::string& raw,
			                              std::vector<SketchPoint_>& out,
			                              std::string& err )
			{
				out.clear();
				std::vector<std::string> segs;
				{
					std::string cur;
					for( char ch : raw ) {
						if( ch == ';' ) { segs.push_back( cur ); cur.clear(); }
						else cur += ch;
					}
					segs.push_back( cur );
				}
				auto isBlank = []( const std::string& s ) {
					for( unsigned char c : s ) { if( !std::isspace( c ) ) return false; }
					return true;
				};
				if( !segs.empty() && isBlank( segs.front() ) ) segs.erase( segs.begin() );
				if( !segs.empty() && isBlank( segs.back() ) )  segs.pop_back();

				if( segs.size() < 3 ) {
					err = "must be at least 3 semicolon-separated \"x y\" points (the polygon is "
						"closed implicitly) -- got " + std::to_string( segs.size() );
					return false;
				}
				if( segs.size() > AgentSession::kElementOutlineMaxPoints ) {
					err = "must have at most " + std::to_string( AgentSession::kElementOutlineMaxPoints ) +
						" points -- got " + std::to_string( segs.size() );
					return false;
				}

				out.reserve( segs.size() );
				for( std::size_t i = 0; i < segs.size(); ++i ) {
					double x = 0.0, y = 0.0;
					char trailing[8] = { 0 };
					const int n = std::sscanf( segs[i].c_str(), " %lf %lf %7s", &x, &y, trailing );
					if( n != 2 ) {
						err = "point " + std::to_string( i + 1 ) + " (\"" + segs[i] +
							"\") must be exactly two numbers \"x y\"";
						return false;
					}
					if( !std::isfinite( x ) || !std::isfinite( y ) ) {
						err = "point " + std::to_string( i + 1 ) + " (\"" + segs[i] +
							"\") must be finite (no NaN/inf)";
						return false;
					}
					if( std::fabs( x ) > kSketchMaxCoord_ || std::fabs( y ) > kSketchMaxCoord_ ) {
						err = "point " + std::to_string( i + 1 ) + " (\"" + segs[i] +
							"\") has a coordinate magnitude past the " +
							std::to_string( static_cast<long long>( kSketchMaxCoord_ ) ) + " sane bound";
						return false;
					}
					SketchPoint_ p; p.x = x; p.y = y;
					out.push_back( p );
				}

				double minX = out[0].x, maxX = out[0].x, minY = out[0].y, maxY = out[0].y;
				for( const SketchPoint_& p : out ) {
					if( p.x < minX ) minX = p.x;
					if( p.x > maxX ) maxX = p.x;
					if( p.y < minY ) minY = p.y;
					if( p.y > maxY ) maxY = p.y;
				}
				if( !( maxX > minX ) || !( maxY > minY ) ) {
					err = "has a zero-area bounding box (axis-aligned degeneracy: every point shares "
						"the same x, or the same y) -- a silhouette needs extent on both axes "
						"(a diagonal sliver, e.g. \"0 0; 1 1; 2 2\", is fine -- it has extent on "
						"both axes and is accepted)";
					return false;
				}
				// G3a fix-round (2026-08-10): a bbox extent that is POSITIVE but
				// subnormal (e.g. "0 0; 1e-320 0; 0 1e-320") passes the check
				// above yet overflows RasterizeElementOutline_'s fit scale to +inf
				// -- see kSketchMinExtent_'s comment for the mechanism.  Same
				// -32602 shape as every other rejection here, so the wire layer
				// and FileBuildPlan (both of which call this one function) name
				// the defect identically.
				if( ( maxX - minX ) < kSketchMinExtent_ || ( maxY - minY ) < kSketchMinExtent_ ) {
					err = "has a bounding-box extent below 1e-9 on at least one axis -- too small to "
						"rasterize without the fit scale overflowing";
					return false;
				}
				// A SELF-INTERSECTING polygon reaches here and is ACCEPTED on
				// purpose: the even-odd fill rule below makes it perfectly
				// well-defined (a bowtie fills as two lobes), so rejecting it
				// would refuse a legal imagination for no gain.
				return true;
			}

			//----------------------------------------------------------------
			//! G3b (2026-08-10): THE SHARED BBOX-NORMALIZED FIT.  ONE
			//! definition, called by BOTH mask producers -- the sketch
			//! rasterizer (RasterizeElementOutline_, below) and the rendered
			//! silhouette normalizer (NormalizeSilhouetteToCanvas_, further
			//! down) -- because an IoU between two masks fitted by DIFFERENT
			//! transforms is not a shape comparison at all, it is a
			//! comparison of two framings, and the degradation is SILENT (a
			//! plausible number, systematically wrong).  A shared function
			//! rather than a shared constant: the fill fraction, the
			//! aspect-preserving max(), the centering and the letterbox all
			//! have to agree, not just the 0.85.
			//!
			//! Maps a source bounding box of extent (`bw`, `bh`) onto the
			//! kElementSketchCanvas square: `outScale` device units per source
			//! unit, `outOffX`/`outOffY` the device-space origin of the
			//! source box's min corner.  Aspect preserved (the LONGER axis
			//! fills kElementSketchFillFraction of the canvas), the shorter axis
			//! letterboxed and centered.  Callers guarantee bw, bh > 0.
			void SketchFitTransform_( double bw, double bh,
			                          double& outScale, double& outOffX, double& outOffY )
			{
				const double canvas = static_cast<double>( AgentSession::kElementSketchCanvas );
				outScale = AgentSession::kElementSketchFillFraction * canvas / ( bw > bh ? bw : bh );
				outOffX  = ( canvas - bw * outScale ) * 0.5;
				outOffY  = ( canvas - bh * outScale ) * 0.5;
			}

			//! Rasterize `pts` into a kElementSketchCanvas^2 0/1 mask.
			//!
			//! CONVENTIONS, all of them load-bearing for determinism and for
			//! G3b's comparison:
			//!   * FIT: SketchFitTransform_ above -- the outline's own bbox
			//!     scaled by kElementSketchFillFraction * canvas /
			//!     max(bboxW, bboxH), aspect PRESERVED, letterboxed, centered
			//!     on both axes.  G3b's silhouette goes through the SAME
			//!     function; see its doc for why that has to be a function
			//!     call and not a duplicated formula.
			//!   * ORIENTATION: outline +Y is UP, image rows run DOWN, so the
			//!     mask reads the way the model drew it.
			//!   * SAMPLING: one sample at each pixel's CENTRE (col+0.5,
			//!     row+0.5).  No anti-aliasing -- it is a mask, not art, and a
			//!     partially-covered pixel has no meaning in an IoU.
			//!   * COVERAGE: even-odd, via the half-open edge test
			//!     (a.y <= sy) != (b.y <= sy).  Half-open is what makes a
			//!     vertex landing exactly on a scanline count ONCE, so spans
			//!     always pair up.
			//!   * ROUNDING: explicit and integral -- a span [xa,xb) covers
			//!     columns ceil(xa-0.5) .. ceil(xb-0.5)-1, clamped to the
			//!     canvas.  No implicit float->int truncation anywhere.
			//! DETERMINISM: pure double arithmetic plus std::ceil (exact) and
			//! a sort of doubles; no RNG, no time, no threading, no
			//! platform-dependent rounding mode.  The same outline string
			//! therefore produces byte-identical bytes in any session, any
			//! process, any run -- pinned by a test.
			void RasterizeElementOutline_( const std::vector<SketchPoint_>& pts,
			                            std::vector<unsigned char>& outMask,
			                            std::size_t& outFilled )
			{
				const int N = AgentSession::kElementSketchCanvas;
				outMask.assign( static_cast<std::size_t>( N ) * static_cast<std::size_t>( N ), 0 );
				outFilled = 0;
				if( pts.size() < 3 ) return;   // ParseElementOutlinePoints_ already guarantees this

				double minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
				for( const SketchPoint_& p : pts ) {
					if( p.x < minX ) minX = p.x;
					if( p.x > maxX ) maxX = p.x;
					if( p.y < minY ) minY = p.y;
					if( p.y > maxY ) maxY = p.y;
				}
				const double bw = maxX - minX;
				const double bh = maxY - minY;
				double scale = 0.0, offX = 0.0, offY = 0.0;
				SketchFitTransform_( bw, bh, scale, offX, offY );

				std::vector<SketchPoint_> dev( pts.size() );
				for( std::size_t i = 0; i < pts.size(); ++i ) {
					dev[i].x = offX + ( pts[i].x - minX ) * scale;
					dev[i].y = offY + ( maxY - pts[i].y ) * scale;   // +Y up -> rows down
				}

				std::vector<double> xs;
				xs.reserve( dev.size() );
				for( int row = 0; row < N; ++row ) {
					const double sy = static_cast<double>( row ) + 0.5;
					xs.clear();
					for( std::size_t i = 0; i < dev.size(); ++i ) {
						const SketchPoint_& a = dev[i];
						const SketchPoint_& b = dev[( i + 1 ) % dev.size()];
						if( ( a.y <= sy ) == ( b.y <= sy ) ) continue;
						// b.y != a.y is guaranteed by the test above.
						xs.push_back( a.x + ( sy - a.y ) * ( b.x - a.x ) / ( b.y - a.y ) );
					}
					if( xs.size() < 2 ) continue;
					std::sort( xs.begin(), xs.end() );
					for( std::size_t k = 0; k + 1 < xs.size(); k += 2 ) {
						int c0 = static_cast<int>( std::ceil( xs[k]     - 0.5 ) );
						int c1 = static_cast<int>( std::ceil( xs[k + 1] - 0.5 ) ) - 1;
						if( c0 < 0 )     c0 = 0;
						if( c1 > N - 1 ) c1 = N - 1;
						for( int c = c0; c <= c1; ++c ) {
							unsigned char& px =
								outMask[ static_cast<std::size_t>( row ) * static_cast<std::size_t>( N ) +
								         static_cast<std::size_t>( c ) ];
							// Count-once even if two spans ever touched: the
							// area fraction is a reported FACT, so it must not
							// be able to exceed 1.
							if( !px ) { px = 1; ++outFilled; }
						}
					}
				}
			}

			//! Tile every sketch mask into ONE composite PNG, left to right,
			//! wrapping every AgentSession::kElementSketchTilesPerRow tiles, in
			//! FILING order.  One image rather than N: on a vision-capable
			//! model N inline images cost N times the tokens for the same
			//! information, and on a text-only model N images are N times the
			//! waste.  Filled = white, empty = black, plus a one-pixel grey
			//! frame per tile so a 4-wide row reads as four sketches rather
			//! than one wide black field.  The frame can never erase mask
			//! content: kElementSketchFillFraction 0.85 leaves a >= 19-pixel
			//! margin on every side of a 256-pixel tile.
			//! At most kElementSketchMaxCompositeTiles tiles are drawn (see that
			//! constant); `outTiled` reports how many, so the caller can state
			//! the fact rather than silently show a partial set.
			//! Returns empty (and leaves the dims at 0) if there is nothing to
			//! draw or the encoder fails.
			std::vector<unsigned char> BuildSketchCompositePng_(
				const std::vector<AgentSession::AgentElementSketch>& sketches,
				unsigned int& outW, unsigned int& outH, std::size_t& outTiled )
			{
				outW = 0;
				outH = 0;
				outTiled = 0;
				if( sketches.empty() ) return std::vector<unsigned char>();

				const int tile   = AgentSession::kElementSketchCanvas;
				const int perRow = AgentSession::kElementSketchTilesPerRow;
				const std::size_t drawn = sketches.size() < AgentSession::kElementSketchMaxCompositeTiles
					? sketches.size() : AgentSession::kElementSketchMaxCompositeTiles;
				const int n      = static_cast<int>( drawn );
				const int cols   = ( n < perRow ) ? n : perRow;
				const int rows   = ( n + perRow - 1 ) / perRow;
				const unsigned int W = static_cast<unsigned int>( cols * tile );
				const unsigned int H = static_cast<unsigned int>( rows * tile );

				const RISEColor kBlack( 0.0, 0.0, 0.0, 1.0 );
				const RISEColor kWhite( 1.0, 1.0, 1.0, 1.0 );
				const RISEColor kFrame( 96.0 / 255.0, 96.0 / 255.0, 96.0 / 255.0, 1.0 );

				std::vector<RISEColor> pels( static_cast<std::size_t>( W ) * H, kBlack );
				for( int i = 0; i < n; ++i ) {
					const std::vector<unsigned char>& mask = sketches[ static_cast<std::size_t>( i ) ].mask;
					if( mask.size() != static_cast<std::size_t>( tile ) * static_cast<std::size_t>( tile ) )
						continue;   // defensive: a target is always full-size
					const int tx = ( i % perRow ) * tile;
					const int ty = ( i / perRow ) * tile;
					for( int y = 0; y < tile; ++y ) {
						for( int x = 0; x < tile; ++x ) {
							const bool onFrame = ( x == 0 || y == 0 || x == tile - 1 || y == tile - 1 );
							const bool filled  =
								mask[ static_cast<std::size_t>( y ) * static_cast<std::size_t>( tile ) +
								      static_cast<std::size_t>( x ) ] != 0;
							pels[ static_cast<std::size_t>( ty + y ) * W +
							      static_cast<std::size_t>( tx + x ) ] =
								onFrame ? kFrame : ( filled ? kWhite : kBlack );
						}
					}
				}

				std::vector<unsigned char> png = EncodeLinearPassthroughPng_( pels, W, H );
				if( png.empty() ) return png;
				outW = W;
				outH = H;
				outTiled = drawn;
				return png;
			}

			//! Format one double as a fixed 2-decimal fact for the filing
			//! message.  std::to_string on a double emits 6 decimals, which
			//! reads as false precision on an area fraction.
			std::string SketchFact2dp_( double v )
			{
				char buf[32];
				std::snprintf( buf, sizeof( buf ), "%.2f", v );
				return std::string( buf );
			}

			//----------------------------------------------------------------
			// G3b (2026-08-10): the sketch COMPARISON.
			//
			// Everything below is pure integer/double arithmetic over two 0/1
			// masks -- no scene, no Document, no lock, no rasterizer.  The
			// one rendered input (an ephemeral objectmap PNG) is produced by
			// AgentSession::ApplyTargetComparison_ and decoded here.
			//----------------------------------------------------------------

			//! Decode an OBJECTMAP identity PNG into a 0/1 silhouette mask:
			//! 1 wherever the pixel is NOT the palette's reserved background
			//! byte #000000, 0 elsewhere.  Exact by construction -- the
			//! objectmap palette generator guarantees every identity colour
			//! round-trips to its own byte and is never (0,0,0) (see
			//! BuildObjectMapPalette's `reserved` list), so "not background"
			//! is precisely "a ray hit the one visible object".  Deliberately
			//! reads the WHOLE image in ONE decode rather than calling
			//! DecodePngRgbAt per pixel (that helper constructs a reader per
			//! call -- 65536 readers for a 256x256 frame).
			//! Returns false (leaving the outputs untouched) when the bytes
			//! do not decode.
			bool DecodePngSilhouetteMask_( const std::vector<unsigned char>& png,
			                               std::vector<unsigned char>& outMask,
			                               unsigned int& outW, unsigned int& outH )
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
				if( !reader->BeginRead( width, height ) || width == 0 || height == 0 ) {
					safe_release( reader );
					safe_release( buffer );
					return false;
				}
				// The SAME quantizer DecodePngRgbAt uses (round-to-nearest of
				// the linear-passthrough channel), so "is this the reserved
				// #000000 background" is decided by identical arithmetic on
				// both sides of the objectmap contract.
				auto toByte = []( double value ) -> unsigned char {
					const int rounded = static_cast<int>( value * 255.0 + 0.5 );
					return static_cast<unsigned char>( rounded < 0 ? 0 : ( rounded > 255 ? 255 : rounded ) );
				};
				std::vector<unsigned char> mask(
					static_cast<std::size_t>( width ) * static_cast<std::size_t>( height ), 0 );
				for( unsigned int y = 0; y < height; ++y ) {
					for( unsigned int x = 0; x < width; ++x ) {
						RISEColor pixel;
						reader->ReadColor( pixel, x, y );
						const bool hit = toByte( pixel.base.r ) != 0 || toByte( pixel.base.g ) != 0 ||
							toByte( pixel.base.b ) != 0;
						if( hit ) mask[ static_cast<std::size_t>( y ) * width + x ] = 1;
					}
				}
				reader->EndRead();
				safe_release( reader );
				safe_release( buffer );

				outMask.swap( mask );
				outW = width;
				outH = height;
				return true;
			}

			//! Crop `src` (a `w` x `h` 0/1 mask, row-major from the TOP row)
			//! to its own filled bounding box and resample it onto the shared
			//! kElementSketchCanvas^2 canvas through SketchFitTransform_ -- the
			//! EXACT transform the sketch rasterizer used, which is what
			//! makes the resulting IoU a shape comparison rather than a
			//! framing comparison.
			//!
			//! Nearest-neighbour, sampled at each destination pixel's CENTRE
			//! and mapped BACK through the fit (inverse mapping, so every
			//! destination pixel gets exactly one source sample and no source
			//! pixel can be written twice).  No anti-aliasing and no
			//! area-averaging: both masks are binary, and a fractional pixel
			//! has no meaning in an integer IoU.
			//!
			//! When `mirrorX` is true the source is flipped left-to-right
			//! WITHIN ITS OWN BBOX before resampling -- the mirrored IoU is
			//! then the same shape measurement of the mirror image, not of a
			//! translated one.
			//!
			//! Returns false and leaves `outMask` empty-but-sized when the
			//! source has no filled pixel at all (nothing rendered inside the
			//! frame); `outBBoxW`/`outBBoxH` are then 0.
			bool NormalizeSilhouetteToCanvas_( const std::vector<unsigned char>& src,
			                                   unsigned int w, unsigned int h, bool mirrorX,
			                                   std::vector<unsigned char>& outMask,
			                                   std::size_t& outFilled,
			                                   unsigned int& outBBoxW, unsigned int& outBBoxH )
			{
				const int N = AgentSession::kElementSketchCanvas;
				outMask.assign( static_cast<std::size_t>( N ) * static_cast<std::size_t>( N ), 0 );
				outFilled = 0;
				outBBoxW  = 0;
				outBBoxH  = 0;
				if( w == 0 || h == 0 ||
					src.size() != static_cast<std::size_t>( w ) * static_cast<std::size_t>( h ) )
					return false;

				unsigned int minX = w, maxX = 0, minY = h, maxY = 0;
				bool any = false;
				for( unsigned int y = 0; y < h; ++y ) {
					for( unsigned int x = 0; x < w; ++x ) {
						if( !src[ static_cast<std::size_t>( y ) * w + x ] ) continue;
						any = true;
						if( x < minX ) minX = x;
						if( x > maxX ) maxX = x;
						if( y < minY ) minY = y;
						if( y > maxY ) maxY = y;
					}
				}
				if( !any ) return false;

				// Pixel bbox extents are INCLUSIVE counts (a single filled
				// pixel is a 1x1 box, never a 0x0 one), so the fit below can
				// never divide by zero.
				const unsigned int bw = maxX - minX + 1;
				const unsigned int bh = maxY - minY + 1;
				outBBoxW = bw;
				outBBoxH = bh;

				double scale = 0.0, offX = 0.0, offY = 0.0;
				SketchFitTransform_( static_cast<double>( bw ), static_cast<double>( bh ),
				                     scale, offX, offY );
				if( !( scale > 0.0 ) ) return false;

				for( int row = 0; row < N; ++row ) {
					const double sy = ( static_cast<double>( row ) + 0.5 - offY ) / scale;
					if( sy < 0.0 || sy >= static_cast<double>( bh ) ) continue;
					const unsigned int srcY = minY + static_cast<unsigned int>( sy );
					for( int col = 0; col < N; ++col ) {
						const double sx = ( static_cast<double>( col ) + 0.5 - offX ) / scale;
						if( sx < 0.0 || sx >= static_cast<double>( bw ) ) continue;
						unsigned int bx = static_cast<unsigned int>( sx );
						if( mirrorX ) bx = bw - 1 - bx;
						const unsigned int srcX = minX + bx;
						if( !src[ static_cast<std::size_t>( srcY ) * w + srcX ] ) continue;
						outMask[ static_cast<std::size_t>( row ) * static_cast<std::size_t>( N ) +
						         static_cast<std::size_t>( col ) ] = 1;
						++outFilled;
					}
				}
				return true;
			}

			//! Intersection-over-union of two equal-sized 0/1 masks.  Pure
			//! integer counting -- no floating-point image math at all, so
			//! the number is bit-reproducible.  Two empty masks have an empty
			//! union; that returns 0.0 rather than the mathematically
			//! conventional 1.0, because "nothing matched nothing" must not
			//! read as a perfect match in a payload a model acts on.
			double MaskIoU_( const std::vector<unsigned char>& a,
			                 const std::vector<unsigned char>& b )
			{
				if( a.size() != b.size() || a.empty() ) return 0.0;
				std::size_t inter = 0, uni = 0;
				for( std::size_t i = 0; i < a.size(); ++i ) {
					const bool pa = a[i] != 0, pb = b[i] != 0;
					if( pa && pb ) ++inter;
					if( pa || pb ) ++uni;
				}
				if( uni == 0 ) return 0.0;
				return static_cast<double>( inter ) / static_cast<double>( uni );
			}

			//! The G3b comparison composite: THREE kElementSketchCanvas-square
			//! tiles side by side -- [ sketch | silhouette | overlay ].
			//!
			//! This image is a REQUIREMENT of the slice, not decoration: the
			//! transport keeps only the MOST RECENT tool-result image live,
			//! so the sketch's pixels are gone from the model's context by
			//! the time it looks again, and this composite is what puts them
			//! back at the exact moment of consultation (design doc
			//! docs/agentic-redesign/77-imagination-target-design.md §4.3).
			//!
			//! COLOURING, stated here and quoted verbatim in both tool
			//! surfaces so a reader is never guessing at what a colour means:
			//!   * tiles 1 and 2: filled WHITE on BLACK, the same rendering
			//!     file_build_plan's own filing echo used, so the sketch tile
			//!     looks identical to what the model already saw.
			//!   * tile 3 (overlay) is CHANNEL-ADDITIVE: the sketch drives
			//!     the RED channel and the silhouette drives GREEN+BLUE, so
			//!     sketch-only reads RED, silhouette-only reads CYAN, and the
			//!     overlap is their exact sum, WHITE.  Neither is BLACK.
			//!     Red/cyan is the opponent pair that survives the common
			//!     colour-vision deficiencies (unlike red/green), the three
			//!     states differ in luminance as well as hue, and every tile
			//!     carries its OWN black background -- so the strip reads the
			//!     same whether the surrounding page is light or dark.
			//! Each tile keeps the one-pixel grey frame BuildSketchCompositePng_
			//! draws, for the same reason and with the same guarantee: at
			//! kElementSketchFillFraction 0.85 both masks leave a >= 19-pixel
			//! margin, so a frame can never erase mask content.
			//! Returns empty (dims left 0) if the encoder fails.
			std::vector<unsigned char> BuildTargetComparisonPng_(
				const std::vector<unsigned char>& sketchMask,
				const std::vector<unsigned char>& silhouetteMask,
				unsigned int& outW, unsigned int& outH )
			{
				outW = 0;
				outH = 0;
				const int tile = AgentSession::kElementSketchCanvas;
				const std::size_t tilePixels =
					static_cast<std::size_t>( tile ) * static_cast<std::size_t>( tile );
				if( sketchMask.size() != tilePixels || silhouetteMask.size() != tilePixels )
					return std::vector<unsigned char>();

				const unsigned int W = static_cast<unsigned int>( tile * 3 );
				const unsigned int H = static_cast<unsigned int>( tile );

				// Exact-byte colours: every channel is 0.0 or 1.0, which the
				// linear-passthrough PNG encode turns into exactly 0 or 255
				// -- so the composite's bytes are pinnable by a test rather
				// than being whatever a transfer function happened to give.
				const RISEColor kBlack( 0.0, 0.0, 0.0, 1.0 );
				const RISEColor kWhite( 1.0, 1.0, 1.0, 1.0 );
				const RISEColor kRed  ( 1.0, 0.0, 0.0, 1.0 );
				const RISEColor kCyan ( 0.0, 1.0, 1.0, 1.0 );
				const RISEColor kFrame( 96.0 / 255.0, 96.0 / 255.0, 96.0 / 255.0, 1.0 );

				std::vector<RISEColor> pels( static_cast<std::size_t>( W ) * H, kBlack );
				for( int y = 0; y < tile; ++y ) {
					for( int x = 0; x < tile; ++x ) {
						const std::size_t si =
							static_cast<std::size_t>( y ) * static_cast<std::size_t>( tile ) +
							static_cast<std::size_t>( x );
						const bool inSketch = sketchMask[si] != 0;
						const bool inSil    = silhouetteMask[si] != 0;
						const bool onFrame  = ( x == 0 || y == 0 || x == tile - 1 || y == tile - 1 );
						const std::size_t rowBase = static_cast<std::size_t>( y ) * W;
						pels[ rowBase + static_cast<std::size_t>( x ) ] =
							onFrame ? kFrame : ( inSketch ? kWhite : kBlack );
						pels[ rowBase + static_cast<std::size_t>( tile + x ) ] =
							onFrame ? kFrame : ( inSil ? kWhite : kBlack );
						pels[ rowBase + static_cast<std::size_t>( 2 * tile + x ) ] =
							onFrame ? kFrame
							        : ( inSketch && inSil ? kWhite
							          : inSketch          ? kRed
							          : inSil             ? kCyan
							                              : kBlack );
					}
				}

				std::vector<unsigned char> png = EncodeLinearPassthroughPng_( pels, W, H );
				if( png.empty() ) return png;
				outW = W;
				outH = H;
				return png;
			}

			//! Format one double as a fixed 3-decimal fact.  IoU is reported
			//! to three places (an area ratio over a 65536-pixel canvas
			//! genuinely carries that much precision, unlike the 2-decimal
			//! area/aspect facts the filing echo prints).
			std::string SketchFact3dp_( double v )
			{
				char buf[32];
				std::snprintf( buf, sizeof( buf ), "%.3f", v );
				return std::string( buf );
			}
		}

		void AgentSession::SetBuildPlanGateDefaultEnabled( bool enabled )
		{
			gBuildPlanGateDefaultEnabled_.store( enabled, std::memory_order_relaxed );
		}

		bool AgentSession::BuildPlanGateDefaultEnabled()
		{
			return gBuildPlanGateDefaultEnabled_.load( std::memory_order_relaxed );
		}

		void AgentSession::SetBuildProtocolDefaultEnabled( bool enabled )
		{
			gBuildProtocolDefaultEnabled_.store( enabled, std::memory_order_relaxed );
		}

		bool AgentSession::BuildProtocolDefaultEnabled()
		{
			return gBuildProtocolDefaultEnabled_.load( std::memory_order_relaxed );
		}

		bool AgentSession::IsValidElementConstruction( const std::string& v )
		{
			for( std::size_t i = 0; i < kBuildPlanConstructionCount; ++i ) {
				if( v == kBuildPlanConstructionValues[i] ) return true;
			}
			return false;
		}

		std::string AgentSession::BuildPlanConstructionList()
		{
			std::string s;
			for( std::size_t i = 0; i < kBuildPlanConstructionCount; ++i ) {
				if( i ) s += ", ";
				s += kBuildPlanConstructionValues[i];
			}
			return s;
		}

		bool AgentSession::IsValidElementView( const std::string& v )
		{
			for( std::size_t i = 0; i < kBuildPlanViewCount; ++i ) {
				if( v == kBuildPlanViewValues[i] ) return true;
			}
			return false;
		}

		std::string AgentSession::BuildPlanViewList()
		{
			std::string s;
			for( std::size_t i = 0; i < kBuildPlanViewCount; ++i ) {
				if( i ) s += ", ";
				s += kBuildPlanViewValues[i];
			}
			return s;
		}

		bool AgentSession::ValidateElementOutline( const std::string& outline, std::string& outError )
		{
			std::vector<SketchPoint_> pts;
			return ParseElementOutlinePoints_( outline, pts, outError );
		}

		bool AgentSession::ChunkTextCreatesGeometry_( const std::string& chunkText,
		                                              std::string* outKind, std::string* outName )
		{
			if( chunkText.empty() ) return false;
			const RISE::Cst::Document doc = RISE::Cst::ParseToCst( chunkText );
			const int n = RISE::Cst::DocItemCount( doc );
			for( int i = 0; i < n; ++i ) {
				const RISE::Cst::NodeRef it =
					RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
				if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
				// The REGISTRY is the classifier, not a `_geometry` suffix
				// match -- the same DescriptorForKeyword / ChunkCategory
				// lookup ComputeDesignNoteConditionsFromDoc_ and
				// AgentEvalRunner.cpp's CheckerCollectKindFilterMatches use.
				// A geometry kind added to the registry later is covered here
				// with no edit.
				const ChunkDescriptor* d = DescriptorForKeyword( String( it->role.c_str() ) );
				if( d && d->category == ChunkCategory::Geometry ) {
					if( outKind ) *outKind = it->role;
					if( outName ) *outName = ChunkParamString_( it, "name" );
					return true;
				}
			}
			return false;
		}

		std::string AgentSession::CheckBuildPlanGate_( const char* verb, std::string* outGiveUpNotice )
		{
			// The armed-ness test is factored into BuildPlanGateArmed_ so the
			// hot call sites can skip their CST parse without duplicating it.
			// Disarmed covers three cases identically: disabled, a plan
			// already filed, or the gate already gave up -- none of them
			// write outGiveUpNotice, so a caller that clears its own local
			// before calling sees "no notice" on every one of them.
			if( !BuildPlanGateArmed_() ) return std::string();

			// Arc 77 Phase 2 (2026-08-11): WHICH of the gate's two conditions
			// is unmet.  BuildPlanGateArmed_ above already established that at
			// least one is, so these two bools are never both false here.
			// `needImagine` is false on every provider without image
			// generation, which is what makes the non-capable refusal below
			// byte-identical to the shipped plan-only one.
			const bool needPlan    = !mBuildPlanFiled;
			const bool needImagine = ImagineRequirementActive_() && !mSceneTarget;

			if( mBuildPlanGateRefusalCount < kBuildPlanGateMaxRefusals )
			{
				// REFUSE -- and count it, but do NOT disarm.  This is the
				// 2026-08-10 supervisor overrule of the original once-per-
				// session design: a gate a model can clear by simply
				// re-issuing the SAME call without filing anything yields
				// zero plans to measure, so the gate stays ARMED across the
				// 1st, 2nd and 3rd interception and refuses every one of
				// them, document byte-identical each time.
				++mBuildPlanGateRefusalCount;
				const int remaining = kBuildPlanGateMaxRefusals - mBuildPlanGateRefusalCount;

				// FACTS ONLY.  No "consider using", no recommendation, no
				// richness advice -- both because this project has measured
				// ambient exhortation at ~0 effect, and because any nudge
				// toward a particular construction here would contaminate the
				// very behaviour this gate exists to measure.  The remaining-
				// attempts count is itself a fact this refusal MUST get right
				// (a false claim in a model-facing payload is a P1 in this
				// repo): it is exactly kBuildPlanGateMaxRefusals minus the
				// count just incremented to, so it can never drift from what
				// CheckBuildPlanGate_ actually does on the next call.
				//
				// G3a (2026-08-10): the text now names the `outline` and
				// `view` fields too.  EVERY CLAUSE MUST STAY TRUE of the
				// schema the dispatcher actually enforces: `outline` really
				// is required with no opt-out, 3 points really is the
				// minimum, the example really does validate, `view` really
				// is optional with a `front` default, and any non-degenerate
				// polygon really is accepted.  A model that follows this
				// sentence must never then get a -32602.
				//
				// Arc 77 Phase 2 (2026-08-11): the refusal now has THREE
				// shapes, one per unmet-condition combination, and every one
				// of them must be TRUE of what the code does.  In particular
				// the closing "the gate clears as soon as ..." clause names
				// EXACTLY the conditions still outstanding -- the pre-Phase-2
				// wording ("as soon as a plan is filed") would be a false
				// claim in a model-facing payload the moment a second
				// condition existed.  On a provider with no image capability
				// `needImagine` is false and the assembled string is
				// BYTE-IDENTICAL to the shipped plan-only refusal.
				std::string msg = std::string( verb ) + " refused: ";
				if( needPlan && needImagine )
					msg += "no build plan has been filed for this session, and no imagined scene target "
					       "has been created for it. ";
				else if( needPlan )
					msg += "no build plan has been filed for this session. ";
				else
					msg += "no imagined scene target has been created for this session. ";

				if( needPlan ) {
					// S1 (2026-08-11): the schema this sentence describes is
					// schema v3 -- elements, each with pieces.  EVERY CLAUSE
					// MUST STAY TRUE of what the dispatcher enforces: `pieces`
					// really is required with at least one entry, `outline`
					// really is required with no opt-out, and any names really
					// are accepted.  A model that follows this sentence must
					// never then get a -32602.
					msg += "Call file_build_plan first, listing the elements of the scene you are about to "
						"build; each element needs `pieces` -- at least one name for the pieces it breaks "
						"down into -- a `construction` value from: " + BuildPlanConstructionList() + ", "
						"and an `outline` -- a closed 2D polygon of at least 3 \"x y\" points separated by "
						"semicolons, e.g. \"0 0; 1 0; 1 2; 0 2\" -- and may carry a `view` of " +
						BuildPlanViewList() + " (default " + kBuildPlanDefaultView + "). "
						"Any of those construction values is accepted -- `primitive` for every element is a "
						"complete plan -- any piece names are accepted, and any outline shape with extent "
						"on both axes is accepted, a rough blob included. The plan "
						"does not constrain what you author afterwards. Filing it makes the first element "
						"active: chunks you create are recorded against it, and finish_element closes it "
						"and moves to the next. ";
				}
				if( needImagine ) {
					// FACTS ONLY, same measurement hygiene as the plan half:
					// no advice about WHAT to describe, because the content of
					// the description is precisely what this mechanism exists
					// to measure.
					msg += "Call imagine_scene with a `description` -- your own words for what the "
						"finished scene should look like -- and " + mImageGenerator.providerName +
						" will generate one image from exactly that text and hold it as this session's "
						"scene target. Any description is accepted; nothing checks what it says. Calling "
						"it again replaces the target. ";
				}

				msg += "Nothing in the document was changed by this call; reissue it after ";
				if( needPlan && needImagine ) msg += "both calls";
				else if( needPlan )           msg += "filing";
				else                          msg += "imagining";
				msg += ". The gate clears as soon as ";
				if( needPlan && needImagine ) msg += "a plan is filed AND a scene target exists";
				else if( needPlan )           msg += "a plan is filed";
				else                          msg += "a scene target exists";
				msg += "; otherwise " + std::to_string( remaining ) +
					( remaining == 1 ? " more call will be refused" : " more calls will be refused" ) +
					" before this gate stops intercepting.";
				return msg;
			}

			// GIVE UP.  This is the (kBuildPlanGateMaxRefusals+1)'th
			// interception -- unlike the shipped E4 render-cadence gate,
			// where any cheap render satisfies the condition, clearing THIS
			// gate requires discovering a brand-new tool and producing a
			// valid schema for it, so an uncapped refuse-until-filed could
			// strand a session that genuinely cannot form that call.  Let
			// this call through and disarm PERMANENTLY (mBuildPlanGateGaveUp,
			// read by BuildPlanGateArmed_) rather than refuse a 4th time.
			mBuildPlanGateGaveUp = true;
			if( outGiveUpNotice ) {
				// FACTS ONLY, same measurement-hygiene rule as the refusal
				// text above.  The caller is responsible for folding this
				// into ITS OWN result (not a log line) so the give-up is
				// greppable in the trajectory payload a census reads.
				// Arc 77 Phase 2: the "-- <verb> proceeded without ..." clause
				// names the condition that was ACTUALLY still unmet.  The
				// leading "build-plan gate: not satisfied after N refusals --"
				// is unchanged and remains the census anchor for the give-up
				// event; only the clause after it varies, so a census that
				// greps the anchor keeps working and one that wants to know
				// WHICH half stranded the session can read on.
				std::string unmet = "a filed plan";
				if( needPlan && needImagine ) unmet = "a filed plan or an imagined scene target";
				else if( !needPlan )          unmet = "an imagined scene target";
				*outGiveUpNotice = std::string( "build-plan gate: not satisfied after " ) +
					std::to_string( kBuildPlanGateMaxRefusals ) + " refusals -- " + verb +
					" proceeded without " + unmet + " and the gate has disarmed for this session; "
					"no further geometry-creating call will be intercepted.";
			}
			return std::string();
		}

		AgentSession::AgentBuildPlanResult AgentSession::FileBuildPlan(
			const std::vector<AgentBuildPlanEntry>& elements )
		{
			AgentBuildPlanResult out;
			out.replacedPreviousPlan = mBuildPlanFiled;

			// G3a: RASTERIZE FIRST, COMMIT AFTER.  Every outline is parsed and
			// drawn into a local target set before ANY member is written, so a
			// defect in element 4 cannot leave the session holding elements 1-3
			// of a plan it never accepted.  All-or-nothing, and on the failure
			// path the previous plan, the previous targets, mBuildPlanFiled and
			// the gate's refusal counter are ALL exactly as they were -- a
			// malformed filing must never be a way to disarm the gate, and it
			// must never be a way to BURN a refusal either.  S1 extends the
			// same rule to the phase state: a rejected filing changes no phase,
			// drops no attribution and closes no window.
			if( elements.size() > kBuildPlanMaxElements ) {
				out.ok = false;
				out.replacedPreviousPlan = false;
				out.message = "build plan not filed: " + std::to_string( elements.size() ) +
					" elements exceeds the " + std::to_string( kBuildPlanMaxElements ) +
					"-element maximum. Nothing was recorded; the build plan and the build-plan gate are "
					"unchanged.";
				return out;
			}

			std::vector<AgentElementSketch> sketches;
			sketches.reserve( elements.size() );
			for( std::size_t i = 0; i < elements.size(); ++i )
			{
				const std::string idx = "elements[" + std::to_string( i ) + "]";

				// S1 (2026-08-11): `pieces` is REQUIRED with at least one entry
				// -- the decomposition IS the artifact this slice measures, so
				// an element with no pieces is a plan with nothing to report
				// against, not a plan with a default.  Checked here as a
				// PRECONDITION (the wire pre-validates it into a -32602 that
				// never touches the gate's counter), exactly like `outline`.
				if( elements[i].pieces.empty() ) {
					out.ok = false;
					out.replacedPreviousPlan = false;
					out.message = "build plan not filed: " + idx + ".pieces is empty -- every element "
						"needs at least one piece name. Nothing was recorded; the build plan and the "
						"build-plan gate are unchanged.";
					return out;
				}
				if( elements[i].pieces.size() > kBuildPlanMaxPiecesPerElement ) {
					out.ok = false;
					out.replacedPreviousPlan = false;
					out.message = "build plan not filed: " + idx + ".pieces has " +
						std::to_string( elements[i].pieces.size() ) + " entries -- at most " +
						std::to_string( kBuildPlanMaxPiecesPerElement ) +
						" are accepted. Nothing was recorded; the build plan and the build-plan gate "
						"are unchanged.";
					return out;
				}

				std::string view = elements[i].view;
				if( view.empty() ) view = kBuildPlanDefaultView;
				if( !IsValidElementView( view ) ) {
					out.ok = false;
					out.replacedPreviousPlan = false;
					out.message = "build plan not filed: " + idx + ".view is `" + elements[i].view +
						"` -- it must be one of: " + BuildPlanViewList() +
						". Nothing was recorded; the build plan and the build-plan gate are unchanged.";
					return out;
				}

				std::vector<SketchPoint_> pts;
				std::string perr;
				if( !ParseElementOutlinePoints_( elements[i].outline, pts, perr ) ) {
					out.ok = false;
					out.replacedPreviousPlan = false;
					out.message = "build plan not filed: " + idx + ".outline " + perr +
						". Nothing was recorded; the build plan and the build-plan gate are unchanged.";
					return out;
				}

				AgentElementSketch s;
				s.element    = elements[i].element;
				s.view       = view;
				s.outline    = elements[i].outline;
				s.pointCount = pts.size();

				double minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
				for( const SketchPoint_& p : pts ) {
					if( p.x < minX ) minX = p.x;
					if( p.x > maxX ) maxX = p.x;
					if( p.y < minY ) minY = p.y;
					if( p.y > maxY ) maxY = p.y;
				}
				// The AUTHORED aspect (bbox width / height), not the mask's --
				// the mask's own bbox is this same ratio quantized to whole
				// pixels, so reporting the authored one is the more precise
				// statement of the same fact.  Positive and finite by
				// ParseElementOutlinePoints_'s zero-area rejection.
				s.aspect = ( maxX - minX ) / ( maxY - minY );

				std::size_t filled = 0;
				RasterizeElementOutline_( pts, s.mask, filled );
				s.areaFraction = static_cast<double>( filled ) /
					( static_cast<double>( kElementSketchCanvas ) * static_cast<double>( kElementSketchCanvas ) );

				sketches.push_back( s );
			}

			// COMMIT.  mBuildPlan and mElementSketches are written together and
			// REPLACED wholesale (never merged), so a re-filing that drops an
			// element leaves no target for an element that is no longer planned.
			mBuildPlan      = elements;
			mElementSketches  = sketches;
			mBuildPlanFiled = true;

			// S1 (2026-08-11): ENTER THE PIECES PHASE with the FIRST element
			// active, and drop every attribution recorded under the previous
			// plan.  The drop is the plan's own REPLACE semantics applied to the
			// state keyed by element NAME: after a re-filing that renames or
			// removes an element, an attribution pointing at the old name would
			// make CheckElementWindowForEdit_ refuse an edit on behalf of an
			// element the plan no longer has -- an over-refusal with no escape,
			// since reopen_element cannot reach a name that is not in the plan.
			// The finished set is rebuilt to match the new element count for the
			// same reason (it is index-parallel to mBuildPlan).
			//
			// The phase machinery being OFF leaves all of this untouched: the
			// phase stays Plan, nothing is ever attributed, and this method
			// behaves exactly as G3a shipped it.
			if( BuildProtocolActive_() ) {
				mBuildPhase = AgentBuildPhase::Pieces;
				mActiveElement = 0;
				mElementFinished.assign( mBuildPlan.size(), false );
				mChunkAttribution.clear();
			}

			out.ok         = true;
			out.elements   = mBuildPlan;
			out.sketches   = mElementSketches;
			std::size_t tiled = 0;
			out.compositePng = BuildSketchCompositePng_( out.sketches, out.compositeWidth,
			                                             out.compositeHeight, tiled );

			// The result ECHOES the plan back factually -- this project has
			// measured that task-specific facts in a JUST-REQUESTED tool
			// result are acted on, while ambient advice is not, so the echo is
			// the one place worth spending words.  It states what was
			// recorded and nothing else: no grading, no suggestion, no
			// "consider" of any kind.  G3a keeps that rule for the sketch
			// facts too -- point count, area fraction and aspect are reported
			// and never characterized, because this is a measurement surface
			// and a word like "simple" or "detailed" here would steer the very
			// distribution the census is about to read.
			std::string m = "build plan filed: " + std::to_string( mBuildPlan.size() ) +
				( mBuildPlan.size() == 1 ? " element" : " elements" ) + " -- ";
			for( std::size_t i = 0; i < mBuildPlan.size(); ++i ) {
				if( i ) m += "; ";
				m += mBuildPlan[i].element + ": " + mBuildPlan[i].construction + ", " +
					std::to_string( mBuildPlan[i].pieces.size() ) +
					( mBuildPlan[i].pieces.size() == 1 ? " piece (" : " pieces (" );
				for( std::size_t p = 0; p < mBuildPlan[i].pieces.size(); ++p ) {
					if( p ) m += ", ";
					m += mBuildPlan[i].pieces[p];
				}
				m += "), " + out.sketches[i].view + " view, " +
					std::to_string( out.sketches[i].pointCount ) + " points, area " +
					SketchFact2dp_( out.sketches[i].areaFraction ) + ", aspect " +
					SketchFact2dp_( out.sketches[i].aspect );
			}
			if( !out.sketches.empty() ) {
				m += ". Sketches, in order: ";
				for( std::size_t i = 0; i < out.sketches.size(); ++i ) {
					if( i ) m += ", ";
					m += out.sketches[i].element;
				}
			}
			if( !out.compositePng.empty() ) {
				m += " -- each outline rasterized to a " + std::to_string( kElementSketchCanvas ) + "x" +
					std::to_string( kElementSketchCanvas ) + " silhouette and returned as one " +
					std::to_string( out.compositeWidth ) + "x" + std::to_string( out.compositeHeight ) +
					" image, tiled left to right, at most " + std::to_string( kElementSketchTilesPerRow ) +
					" per row.";
				// Stated, never silent: a partial composite that looked
				// complete would let a model conclude its later sketches
				// rasterized to nothing.
				if( tiled < out.sketches.size() )
					m += " The image shows the first " + std::to_string( tiled ) +
						" of " + std::to_string( out.sketches.size() ) +
						" sketches; the rest were rasterized and are reported as facts only.";
			}
			else {
				m += ".";
			}
			m += " The build-plan gate is now off for this session; it will not intercept any call. "
			     "This declaration does not constrain what you author -- any element may be built with "
			     "any chunk kind.";
			if( out.replacedPreviousPlan )
				m += " This replaced the plan previously filed in this session, and every sketch "
				     "filed with it.";
			// S1: the phase facts, stated in the SAME factual register as the
			// rest of the echo -- what the session is now in, which element is
			// active, and what the two phase verbs do.  No advice about how to
			// spend the window: what the model does inside it is exactly what
			// this slice measures.
			if( BuildProtocolActive_() ) {
				m += " The session is now in the pieces phase and \"" + mBuildPlan[0].element +
					"\" is the active element";
				if( mBuildPlan.size() > 1 )
					m += " (element 1 of " + std::to_string( mBuildPlan.size() ) + ", in the order "
					     "listed above)";
				m += ". Every chunk created while an element is active is recorded against it. "
				     "finish_element closes the active element, reports what was recorded against it, "
				     "returns an isolate render of it, and makes the next element active; after the "
				     "last one the session enters the compose phase, where creating new geometry is "
				     "refused and reopen_element re-enters an element's window. An edit aimed at a "
				     "chunk recorded against a DIFFERENT element is refused; chunks with no element "
				     "recorded against them, and light, camera, film, rasterizer, rasterizer-output, "
				     "material and painter chunks, are editable in every phase.";
				if( out.replacedPreviousPlan )
					m += " Re-filing also dropped every chunk-to-element record from the previous "
					     "plan and made the first element of this one active.";
			}
			out.message = m;
			return out;
		}

		//----------------------------------------------------------------------
		// G3b (2026-08-10): the plan-to-object join, made by the CALLER at
		// comparison time.  These three functions are the ONLY place a
		// `target` name is matched against the filed plan; the wire layer's
		// pre-check and RenderCore_'s render-failing check both go through
		// ResolveTargetSketch, so a caller cannot be shown one contract by a
		// -32602 and a different one by the render.
		//----------------------------------------------------------------------

		const AgentSession::AgentElementSketch* AgentSession::FindElementSketch( const std::string& element ) const
		{
			for( std::size_t i = 0; i < mElementSketches.size(); ++i ) {
				if( mElementSketches[i].element == element ) return &mElementSketches[i];
			}
			return nullptr;
		}

		std::string AgentSession::ElementSketchNameList() const
		{
			std::string s;
			for( std::size_t i = 0; i < mElementSketches.size(); ++i ) {
				if( i ) s += ", ";
				s += "\"";
				s += mElementSketches[i].element;
				s += "\"";
			}
			return s;
		}

		std::string AgentSession::TargetRequiresIsolateMessage( const std::string& target )
		{
			return "`target` (\"" + target + "\") requires `isolate`: a shape comparison measures ONE "
				"object's silhouette against ONE filed sketch, so name the object to isolate in the "
				"same call.";
		}

		bool AgentSession::ResolveTargetSketch( const std::string& target, const std::string& isolate,
		                                         AgentElementSketch& out, std::string& outError ) const
		{
			if( target.empty() ) {
				outError = "target is empty";
				return false;
			}
			// REQUIRES `isolate`.  A comparison measures ONE object's
			// silhouette; with no isolate there is no single object to
			// measure, and inferring one from the element name is exactly the
			// join-by-naming-convention this design rejected (design doc
			// docs/agentic-redesign/77-imagination-target-design.md sec 5.1:
			// the join is made BY THE CALLER, at the one moment it is
			// unambiguous and free).
			if( isolate.empty() ) {
				outError = TargetRequiresIsolateMessage( target ) + " Nothing was rendered.";
				return false;
			}
			if( mElementSketches.empty() ) {
				outError = "target \"" + target + "\" cannot be compared: no build plan has been filed in "
					"this session, so there is no sketch to compare against. Call file_build_plan first. "
					"Nothing was rendered.";
				return false;
			}
			const AgentElementSketch* found = FindElementSketch( target );
			if( !found ) {
				// The available-name list, same fail-loud contract as an
				// unresolvable `view`/`light`/`isolate`.
				outError = "unknown target element \"" + target + "\" -- the filed build plan lists: " +
					ElementSketchNameList() + ". Nothing was rendered.";
				return false;
			}
			out = *found;
			return true;
		}

		AgentChunkResult AgentSession::RemoveChunk( const std::string& target,
		                                            const std::string& kind,
		                                            const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			AgentChunkResult r;
			r.name = target;
			// S1 (2026-08-11): forget the removed chunk's attribution on
			// whichever return actually landed the remove.
			DropAttributionOnRemove_ s1Drop{ *this, r };

			// S1 (2026-08-11, the staged build protocol): the CROSS-ELEMENT
			// arm.  Removing a chunk another element created is the same reach
			// into another window an edit is, and it is strictly more
			// destructive.  FIRST, ahead of the authority gate, because it is a
			// pure sequencing check that consults nothing about the document or
			// about whether the remove would otherwise be accepted -- the same
			// placement rule the build-plan gate's arms follow.  There is no
			// compose arm here: removing is not creating, and the compose phase
			// allows edits to any element's chunks.
			BuildPlanGiveUpFold_ s1Fold{ r.message, std::string() };
			{
				const std::string clause = CheckElementWindowForEdit_( "remove_chunk", target,
				                                                        &s1Fold.notice );
				if( !clause.empty() ) {
					r.applied     = false;
					r.retriable   = false;   // see InsertChunk's G2 arm for why
					r.rawCode     = 0;
					r.status      = "rejected";
					r.headVersion = ReadHeadVersion();
					r.message     = clause;
					return r;
				}
			}

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
					// queue-full branch for the full rationale, including
					// why the head-version read is controller-mediated.
					r.applied    = false;
					r.rawCode    = 0;
					r.status     = "rejected";
					r.queueFull  = true;
					r.headVersion = ReadHeadVersion();
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
				// Through IsAnalysableRejection_ + ReadHeadDocumentAt_ (see
				// ProposePatch's LIVE branch): controller-mediated read, only
				// on a real derive rejection, and only while the head still IS
				// the version this result stamps.
				RISE::Cst::Document headDoc;
				if( IsAnalysableRejection_( r ) && ReadHeadDocumentAt_( r.headVersion, headDoc ) )
					AttachRemoveRejectionIssues( r, headDoc, target, kind );
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

		AgentSession::AgentRemoveBatchResult AgentSession::RemoveChunks( const std::vector<std::string>& targets,
		                                                                const RISE::Cst::CstHeadVersion* baseOrNull )
		{
			AgentRemoveBatchResult r;
			// S1 (2026-08-11): forget every removed chunk's attribution when the
			// batch lands, and fold a phase give-up into this call's own result
			// -- the same two hooks the singular verb takes.
			DropAttributionOnRemoveBatch_ s1Drop{ *this, r };
			BuildPlanGiveUpFold_ s1Fold{ r.message, std::string() };

			// ---- Request validation (nothing touched on any failure) --------------------------------
			if( targets.empty() ) {
				r.status      = "rejected";
				r.headVersion = HeadVersion();
				r.message     = "remove_chunks refused: `targets` must list at least one chunk name";
				return r;
			}

			// DEDUPE by exact name, FIRST-OCCURRENCE order.  Silently-but-honestly: the chunk is removed
			// once and `note` says so.  Refusing a harmless bookkeeping slip would cost the model a whole
			// round-trip to fix nothing.  An EMPTY element is a different matter -- it addresses no chunk
			// at all, so it refuses the batch (all-or-nothing) rather than being quietly dropped.
			std::vector<std::string>  unique;
			std::vector<int>          repeatCount;   // parallel to `unique`; >1 means it was listed more than once
			for( std::size_t i = 0; i < targets.size(); ++i )
			{
				if( targets[i].empty() ) {
					char buf[160];
					std::snprintf( buf, sizeof( buf ),
						"remove_chunks refused: targets[%d] is empty -- every element must name a chunk (nothing was removed)",
						static_cast<int>( i ) );
					r.status      = "rejected";
					r.headVersion = HeadVersion();
					r.message     = buf;
					return r;
				}
				std::size_t at = unique.size();
				for( std::size_t u = 0; u < unique.size(); ++u ) { if( unique[u] == targets[i] ) { at = u; break; } }
				if( at == unique.size() ) { unique.push_back( targets[i] ); repeatCount.push_back( 1 ); }
				else                      { ++repeatCount[at]; }
			}
			{
				std::string dupes;
				for( std::size_t u = 0; u < unique.size(); ++u ) {
					if( repeatCount[u] <= 1 ) continue;
					if( !dupes.empty() ) dupes += ", ";
					char buf[96];
					std::snprintf( buf, sizeof( buf ), "'%s' listed %dx", unique[u].c_str(), repeatCount[u] );
					dupes += buf;
				}
				if( !dupes.empty() )
					r.note = "deduped: " + dupes + " -- each chunk is removed once";
			}

			// Per-target results, one per UNIQUE target, in first-occurrence order.  `kind` fills in below
			// from whatever the engine resolved; `name` is set now so even a total refusal identifies what
			// was attempted (the SAME rule RemoveChunk follows).
			r.targetResults.resize( unique.size() );
			for( std::size_t u = 0; u < unique.size(); ++u ) r.targetResults[u].name = unique[u];

			// `fanOutVerdict` stamps the batch verdict onto every per-target entry.  ALL-OR-NOTHING means
			// there is exactly one verdict; the entries exist to localize the CAUSE, not to disagree.
			auto fanOutVerdict = [&r]() {
				for( AgentChunkResult& tr : r.targetResults ) {
					tr.applied     = r.applied;
					tr.retriable   = r.retriable;
					tr.rawCode     = r.rawCode;
					tr.status      = r.status;
					tr.headVersion = r.headVersion;
					tr.queueFull   = r.queueFull;
				}
			};

			// ---- S1 (2026-08-11, the staged build protocol): the CROSS-ELEMENT arm, as an UP-FRONT
			// scan of every UNIQUE target, before anything is staged or removed.  All-or-nothing, for
			// the reason this verb is all-or-nothing everywhere else: a policy refusal is not an
			// authoring failure, and tearing down half a batch leaves a scene the model never asked
			// for.  ONE interception per CALL, so a batching caller spends the phase budget at the
			// same rate a singular caller does.  There is no compose arm -- removing is not creating.
			{
				std::string s1Clause;
				for( std::size_t u = 0; u < unique.size() && s1Clause.empty(); ++u )
					s1Clause = CheckElementWindowForEdit_( "remove_chunks", unique[u], &s1Fold.notice );
				if( !s1Clause.empty() ) {
					r.applied     = false;
					r.retriable   = false;   // see InsertChunk's G2 arm for why
					r.rawCode     = 0;
					r.status      = "rejected";
					r.headVersion = ReadHeadVersion();
					r.message     = s1Clause;
					fanOutVerdict();
					return r;
				}
			}

			// ---- External authority: stage the WHOLE batch as ONE proposal ---------------------------
			// The same authority gate RemoveChunk applies (see ProposePatch's doc for the rationale), with
			// ONE deliberate difference in shape: the batch stages as a SINGLE AgentProposal
			// (AgentProposalKind::RemoveChunks) rather than N.  An Owner must be able to approve or reject
			// exactly the atomic edit that was proposed -- N cards could be partially approved into a
			// half-torn-down state the agent never asked for, which is the precise failure this verb's
			// all-or-nothing contract exists to prevent.
			if( mAuthority == AgentAuthority::External )
			{
				if( !mController )
				{
					r.status      = "rejected";
					r.headVersion = HeadVersion();
					r.message     = "remove_chunks refused: this session is External-authority and no live "
					                "controller is attached -- staging needs a live Owner to resolve against";
					fanOutVerdict();
					return r;
				}
				// S5a hardening: see ProposePatch's identical comment -- no unlocked head pre-read;
				// StageProposal stamps it under its own mMutex hold when no explicit base was supplied.
				SceneEditController::AgentProposal p;
				p.kind = SceneEditController::AgentProposalKind::RemoveChunks;
				// The '\n'-packed carriers documented on AgentProposal: `target` = the N names, `entityKind`
				// = the N kinds (all EMPTY here -- remove_chunks takes bare names only), same order.
				{
					std::string packedNames, packedKinds;
					for( std::size_t u = 0; u < unique.size(); ++u ) {
						if( u ) { packedNames += '\n'; packedKinds += '\n'; }
						packedNames += unique[u];
					}
					// packedKinds is (unique.size()-1) newlines: N empty entries, positionally aligned.
					p.target     = String( packedNames.c_str() );
					p.entityKind = String( packedKinds.c_str() );
				}
				p.hasExplicitBaseVersion = ( baseOrNull != nullptr );
				if( baseOrNull ) p.baseVersion = *baseOrNull;
				// Secure-MCP slice 5c: see ProposePatch's identical comment.
				p.sessionLabel = String( mSessionLabel.c_str() );
				RISE::Cst::CstHeadVersion stagedHead{};
				const std::uint64_t id = mController->StageProposal( p, &stagedHead );
				if( id == 0 )
				{
					// Secure-MCP slice 6: see ProposePatch's identical queue-full branch.
					r.queueFull   = true;
					r.status      = "rejected";
					r.headVersion = ReadHeadVersion();
					r.message = "remove_chunks refused: the pending-proposal queue is full -- "
					            "the Owner must resolve (approve/reject) some pending proposals "
					            "before another can be staged";
					fanOutVerdict();
					return r;
				}
				r.status      = "staged";
				r.headVersion = stagedHead;
				char buf[192];
				std::snprintf( buf, sizeof( buf ),
					"proposal %llu staged (pending owner approval) -- ONE proposal for all %d targets; approving it "
					"removes them atomically",
					static_cast<unsigned long long>( id ), static_cast<int>( unique.size() ) );
				r.message = buf;
				fanOutVerdict();
				return r;
			}

			// ---- LIVE mode: delegate to the controller ----------------------------------------------
			if( mController )
			{
				std::vector<String> ctlTargets, ctlKinds;
				ctlTargets.reserve( unique.size() );
				ctlKinds.reserve( unique.size() );
				for( std::size_t u = 0; u < unique.size(); ++u ) {
					ctlTargets.push_back( String( unique[u].c_str() ) );
					ctlKinds.push_back( String() );   // bare names only -- see the header's NO PER-TARGET `kind` note
				}
				const SceneEditController::AgentCommitResult cr =
					mController->ApplyAgentRemoveChunks( ctlTargets, ctlKinds, baseOrNull );
				r.applied     = cr.applied;
				r.retriable   = cr.retriable;
				r.rawCode     = cr.rawCode;
				r.status      = cr.status.c_str();
				r.headVersion = cr.headVersion;
				r.message     = cr.message.c_str();
				fanOutVerdict();
				// Per-target keywords: the controller echoes them '\n'-joined, one per INPUT target, in the
				// order we passed them (empty for a target that never resolved).
				{
					const std::string kws( cr.chunkKeyword.c_str() );
					std::size_t at = 0, u = 0;
					while( u < r.targetResults.size() )
					{
						const std::size_t nl = kws.find( '\n', at );
						r.targetResults[u].kind = kws.substr( at, ( nl == std::string::npos ) ? std::string::npos : nl - at );
						++u;
						if( nl == std::string::npos ) break;
						at = nl + 1;
					}
				}
				// Actionable rejection diagnostics on the LIVE path -- the SAME gating RemoveChunk's LIVE
				// branch applies (IsAnalysableRejection_ + the controller-mediated ReadHeadDocumentAt_, only
				// on a real derive rejection and only while the head still IS the version this result
				// stamps).  A rejection left the head UNCHANGED, so the retained Document is the correct
				// namespace to resolve the targets and their referrers against.
				// Widened for the batch exactly as the headless tail below is (see its comment): the
				// probe's rawCode is 0 for EVERY batch refusal (the controller normalizes negatives),
				// so IsAnalysableRejection_ admits -1/-2 refusals here too.
				AgentChunkResult probe;   // IsAnalysableRejection_ reads only status/applied/rawCode
				probe.applied = r.applied; probe.retriable = r.retriable; probe.rawCode = r.rawCode;
				probe.status  = r.status;  probe.headVersion = r.headVersion;
				RISE::Cst::Document headDoc;
				if( IsAnalysableRejection_( probe ) && ReadHeadDocumentAt_( r.headVersion, headDoc ) ) {
					AttachRemoveBatchRejectionIssues( r.targetResults, headDoc, unique );
					r.message += BuildRemoveBatchActionableClause( r.targetResults );
				}
				return r;
			}

			// ---- Headless: straight to the Job primitive --------------------------------------------
			if( !mJob || !mJob->HasRetainedCstDocument() ) {
				r.status      = "rejected";
				r.headVersion = HeadVersion();
				r.message     = "no retained CST Document -- RemoveChunks needs a CST-loaded head";
				fanOutVerdict();
				return r;
			}
			if( baseOrNull ) {
				const RISE::Cst::CstHeadVersion cur = mJob->GetCstHeadVersion();
				if( *baseOrNull != cur ) {
					r.status      = "conflict";
					r.headVersion = cur;
					char buf[160];
					std::snprintf( buf, sizeof( buf ),
						"baseHeadVersion does not match the current head (revision %llu) -- re-read and re-propose",
						static_cast<unsigned long long>( cur.revision ) );
					r.message = buf;
					fanOutVerdict();
					return r;
				}
			}

			std::vector<const char*> targetPtrs;
			std::vector<const char*> kindPtrs;
			targetPtrs.reserve( unique.size() );
			kindPtrs.reserve( unique.size() );
			for( std::size_t u = 0; u < unique.size(); ++u ) {
				targetPtrs.push_back( unique[u].c_str() );
				kindPtrs.push_back( nullptr );   // bare names only
			}
			char kwBuf[1024];  kwBuf[0]   = '\0';
			char diagBuf[512]; diagBuf[0] = '\0';
			int  failIndex = -1;
			const int code = mJob->ApplyCstRemoveChunks( &targetPtrs[0], &kindPtrs[0],
			                                             static_cast<int>( targetPtrs.size() ),
			                                             kwBuf, sizeof( kwBuf ),
			                                             diagBuf, sizeof( diagBuf ), &failIndex );
			// Resolved keywords, one per unique target, '\n'-separated in the order we passed them.
			{
				const std::string kws( kwBuf );
				std::size_t at = 0, u = 0;
				while( u < r.targetResults.size() )
				{
					const std::size_t nl = kws.find( '\n', at );
					r.targetResults[u].kind = kws.substr( at, ( nl == std::string::npos ) ? std::string::npos : nl - at );
					++u;
					if( nl == std::string::npos ) break;
					at = nl + 1;
				}
			}
			FoldRemoveBatchCode_( r, code, failIndex, unique, diagBuf );
			r.headVersion = mJob->GetCstHeadVersion();
			fanOutVerdict();
			// The pre-flight CAUSE analysis for a REJECTED batch -- ONLY for literal code 0
			// (Job::ApplyCstRemoveChunks' generic "would not derive" catch-all).  -1/-2 already carry a
			// precise, index-attributed cause, the SAME rule RemoveChunk's own AttachRemoveRejectionIssues
			// call applies.
			// The pre-flight CAUSE analysis for a REFUSED batch.  UNLIKE the singular verb -- whose
			// analyser runs for code 0 ONLY, because its -1/-2 already carry a precise cause in the
			// message -- the BATCH runs it for every refusal code.  Rationale: a batch refusal's message
			// can name only the FIRST offender (Job stops resolving there), while the per-target `issues`
			// can flag EVERY target the static analysis can explain in one round-trip, which is exactly
			// what "fix the offenders and resend" needs.  The analyser is honest about what it cannot
			// classify (ambiguous names and unexplainable derive failures produce no issue at all).
			if( code <= 0 && mJob->GetCstDocument() ) {
				AttachRemoveBatchRejectionIssues( r.targetResults, *mJob->GetCstDocument(), unique );
				r.message += BuildRemoveBatchActionableClause( r.targetResults );
			}
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
					case SceneEditController::AgentProposalKind::RemoveChunks: return "remove_chunks";   // R1a: ONE entry for a whole batch
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
				// `false` covers TWO cases (see SceneEditController::
				// ResolveProposal's doc): the queue said no, or the
				// render-admission gate refused before the queue was even
				// consulted.  Only the first is a not-found.
				if( cr.retriable )
				{
					r.retriable = true;
					r.message = std::string( "resolve_proposal did not resolve: " )
					          + cr.message.c_str()
					          + " -- the proposal is STILL PENDING; resolve it again once the gate clears";
				}
				else
				{
					r.message = "resolve_proposal refused: no pending proposal with that id (unknown id, or it was already resolved)";
				}
				return r;
			}

			// The replay was TRANSIENTLY refused (an open editor transaction
			// or gesture), so SceneEditController::ResolveProposal left the
			// proposal PENDING -- see its TRANSIENT-REFUSAL EXCEPTION.  This
			// is not a resolve: reporting ok=true with status="rejected"
			// would put a still-approvable proposal on the wire as resolved.
			// The predicate MIRRORS that method's status fold (applied ->
			// conflict -> retriable) so the two cannot disagree about which
			// outcome left the entry pending.
			if( !cr.applied && std::string( cr.status.c_str() ) != "conflict" && cr.retriable )
			{
				r.ok = false;
				r.retriable = true;
				r.message = std::string( "resolve_proposal did not resolve: " )
				          + cr.message.c_str()
				          + " -- the proposal is STILL PENDING; resolve it again once the gate clears";
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

			//! G1 (2026-08-10) `render{isolate:}` surface: RAII restore of
			//! the SCENE's per-object world-visible flags, the sibling of
			//! LightSoloRestoreGuard above and the same Arm/restore-on-
			//! every-exit house shape.
			//!
			//! WHY world-visible is the mechanism: every ray path in the
			//! renderer already consults it -- ObjectManager's three
			//! RayElementIntersection overloads (primary/secondary and the
			//! shadow-only one), both linear-loop fallbacks, and the shadow
			//! cache's stale-entry recheck -- so hiding an object removes it
			//! from camera, secondary AND shadow rays with no new per-object
			//! state and no change to any hot path.  Nothing else in the
			//! tree writes this flag during a render (CSGObject writes it
			//! ONCE at AssignObjects time to hide its operands), so a
			//! capture/restore of the flag is exact.
			//!
			//! TWO invariants the caller MUST have established before Arm():
			//!   1. The object manager's TLAS is already built over the FULL
			//!      object set (PrepareForRendering()).  ObjectManager::
			//!      CreateBVH filters its element list on IsWorldVisible and
			//!      then CACHES the result -- a BVH first built while the
			//!      scene is isolated would contain ONE object and survive
			//!      the restore, silently deleting every other object from
			//!      all later renders.  Building first (idempotent) makes
			//!      the isolated render reuse the full-set BVH and reject
			//!      hidden objects at the LEAF test, which is both correct
			//!      and free -- no invalidation, no rebuild, in either
			//!      direction.
			//!   2. The Scene's light-topology generation is bumped on BOTH
			//!      the apply and the restore (this guard does the restore
			//!      half).  A caster's LuminaryManager is rebuilt from
			//!      ObjectManager::EnumerateObjects, which ALSO filters on
			//!      world-visible; without the bump, a caster that happened
			//!      to (re)build its luminary list during the isolated
			//!      render would keep that reduced list for every later
			//!      render of the same Scene pointer (RayCaster::AttachScene
			//!      only rebuilds when the generation moved).
			//!
			//! `mPrior` holds the pre-isolate flag of EVERY object the
			//! manager reported, so a CSG operand (already world-invisible)
			//! is restored to invisible, not blanket-true.
			class ObjectSoloRestoreGuard
			{
			public:
				explicit ObjectSoloRestoreGuard( IScenePriv* scene )
					: mScene( scene ), mArmed( false )
				{
				}

				//! Takes ownership of the captured (object, priorVisible)
				//! list and makes the destructor's restore live.  Called
				//! only once the hide pass has actually run.
				void Arm( std::vector<std::pair<IObjectPriv*, bool> >&& prior )
				{
					mPrior = std::move( prior );
					mArmed = true;
				}

				~ObjectSoloRestoreGuard()
				{
					if( !mArmed ) return;
					try {
						for( std::size_t i = 0; i < mPrior.size(); ++i ) {
							if( mPrior[i].first ) mPrior[i].first->SetWorldVisible( mPrior[i].second );
						}
						// Invariant 2 (see the class doc): force every caster
						// to rebuild its luminary list against the restored,
						// full object set.
						if( RISE::Implementation::Scene* concrete =
								dynamic_cast<RISE::Implementation::Scene*>( mScene ) ) {
							concrete->BumpLightTopologyGeneration();
						}
					}
					catch( ... ) {
						// NEVER rethrow from a destructor (would terminate()
						// if already unwinding from Rasterize()'s own
						// exception).  Log so a future throw here is at least
						// diagnosable -- this one is load-bearing: a skipped
						// restore leaves the SCENE showing one object.
						GlobalLog()->PrintEx( eLog_Error,
							"AgentSession::Render: exception escaped the object-isolate "
							"restore -- the scene may be left with only one visible object" );
					}
				}

			private:
				ObjectSoloRestoreGuard( const ObjectSoloRestoreGuard& );             // deleted
				ObjectSoloRestoreGuard& operator=( const ObjectSoloRestoreGuard& );  // deleted

				IScenePriv*     mScene;
				std::vector<std::pair<IObjectPriv*, bool> > mPrior;
				bool            mArmed;
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

		// ---- G1 (2026-08-10) `render{isolate:}` surface -------------------
		//
		// Sibling of the light-solo block above: resolve a NAME against the
		// scene, apply a transient solo, and let a RAII guard restore it.
		// The three helpers below are the resolve / apply / frame steps;
		// ObjectSoloRestoreGuard (declared with the other render guards) is
		// the restore step.

		//! Collect every object name the manager knows, in the manager's own
		//! deterministic (sorted, std::map) order.
		std::vector<std::string> CollectObjectNames( IObjectManager* objMgr )
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
			return collector.names;
		}

		//! Format a quoted, comma-separated list of the INDEPENDENTLY
		//! RENDERABLE object names (world-visible ones -- a CSG operand is an
		//! ObjectManager item but is never hit on its own, exactly the filter
		//! BuildObjectMapPalette applies for the objectmap legend, so the two
		//! surfaces name the same set).
		std::string FormatRenderableObjectNames( IObjectManager* objMgr )
		{
			const std::vector<std::string> names = CollectObjectNames( objMgr );
			std::string out;
			for( std::size_t i = 0; i < names.size(); ++i ) {
				IObjectPriv* obj = objMgr ? objMgr->GetItem( names[i].c_str() ) : 0;
				if( !obj || !static_cast<const IObject*>( obj )->IsWorldVisible() ) continue;
				if( !out.empty() ) out += ", ";
				out += "\"";
				out += names[i];
				out += "\"";
			}
			return out;
		}

		//! G1 fix-round (2026-08-10, FIX 6): find the CSGObject composite (if
		//! any) that consumes `operand` as one of its two operands, by
		//! walking every object the manager knows and checking
		//! CSGObject::GetOperandA/GetOperandB for an identity match.  Real
		//! lookup, not a guess: CSGObject already exposes both accessors
		//! (used by the CST incremental-apply path to detect a re-pointed
		//! operand), so there is no need to infer the parent from the
		//! renderable-names list the way the caller used to.  Returns "" if
		//! no composite in the manager claims `operand` -- shouldn't happen
		//! for a genuinely CSG-hidden object (AssignObjects is the only
		//! in-tree writer of world-invisible), but the caller falls back to
		//! a generic message rather than asserting.
		std::string FindCsgParentName( IObjectManager* objMgr, IObjectPriv* operand )
		{
			if( !objMgr || !operand ) return std::string();
			const std::vector<std::string> names = CollectObjectNames( objMgr );
			for( std::size_t i = 0; i < names.size(); ++i ) {
				IObjectPriv* candidate = objMgr->GetItem( names[i].c_str() );
				if( !candidate ) continue;
				if( RISE::Implementation::CSGObject* csg =
						dynamic_cast<RISE::Implementation::CSGObject*>( candidate ) ) {
					if( csg->GetOperandA() == operand || csg->GetOperandB() == operand )
						return names[i];
				}
			}
			return std::string();
		}

		//! Resolve `name` to the single object an `isolate` render should
		//! keep.  Fails LOUDLY (false + a specific `outMessage`) rather than
		//! guessing, mirroring ApplyLightSoloByName's unresolved-name
		//! contract.  Four distinguishable failures, each with its own
		//! actionable text:
		//!   * no object manager at all (no scene loaded);
		//!   * the name is a GENERATOR PREFIX -- the scene has "<name>[i,j]"
		//!     instances but no object literally called "<name>" (the
		//!     instance_array case objectmap's legend already warns about);
		//!   * the name resolves to an object that is NOT independently
		//!     renderable (a CSGObject operand, world-invisible by
		//!     construction) -- name the SPECIFIC composite that consumes it
		//!     (FindCsgParentName above), since "unknown object" would be a
		//!     lie and a list of every renderable object in the scene would
		//!     be a claim of precision the old message didn't back up;
		//!   * the name is unknown -- list what IS available.
		bool ResolveIsolateObject( IObjectManager* objMgr, const std::string& name,
		                            IObjectPriv*& outObj, std::string& outMessage )
		{
			outObj = 0;
			if( !objMgr ) {
				outMessage = "isolate \"" + name + "\" could not be applied -- the scene has no object manager";
				return false;
			}

			IObjectPriv* obj = objMgr->GetItem( name.c_str() );
			if( obj ) {
				if( !static_cast<const IObject*>( obj )->IsWorldVisible() ) {
					// World-invisible means "no ray ever lands on this object
					// directly".  The only in-tree writer of that flag (outside
					// this feature's own transient solo) is
					// CSGObject::AssignObjects on its operands, so naming the
					// composite case is accurate rather than a guess -- but the
					// message leads with the OBSERVED fact, not the inferred
					// cause, so it stays honest if a future producer of
					// world-invisible objects appears.
					outMessage = "isolate \"" + name + "\" is not independently renderable -- it is marked "
						"world-invisible, so no ray lands on it directly.";
					const std::string parentName = FindCsgParentName( objMgr, obj );
					if( !parentName.empty() ) {
						outMessage += "  It is a CSG operand consumed by \"" + parentName +
							"\" -- isolate \"" + parentName + "\" instead.";
					} else {
						outMessage += "  In-tree that means a CSG object consumed it as an operand, but "
							"the specific composite could not be identified.  Available: "
							+ FormatRenderableObjectNames( objMgr );
					}
					return false;
				}
				outObj = obj;
				return true;
			}

			// Generator prefix?  ("grid" when the scene holds "grid[0,0]", ...)
			const std::vector<std::string> names = CollectObjectNames( objMgr );
			std::string instances;
			unsigned int instanceCount = 0;
			for( std::size_t i = 0; i < names.size(); ++i ) {
				if( names[i].size() > name.size() + 1 &&
					names[i].compare( 0, name.size(), name ) == 0 &&
					names[i][ name.size() ] == '[' )
				{
					++instanceCount;
					if( instanceCount <= 8 ) {
						if( !instances.empty() ) instances += ", ";
						instances += "\"";
						instances += names[i];
						instances += "\"";
					}
				}
			}
			if( instanceCount > 0 ) {
				char tail[64];
				std::snprintf( tail, sizeof( tail ), " (%u instance%s in total)",
					instanceCount, instanceCount == 1 ? "" : "s" );
				outMessage = "isolate \"" + name + "\" is AMBIGUOUS -- it is a generator name, not a single "
					"object; the scene holds " + instances + tail +
					".  Isolate ONE instance by its full name.";
				return false;
			}

			const std::string available = FormatRenderableObjectNames( objMgr );
			outMessage = "unknown object \"" + name + "\"";
			outMessage += available.empty()
				? std::string( " -- the scene has no renderable objects" )
				: ( " -- available: " + available );
			return false;
		}

		//! Hide every object EXCEPT `keep`, returning the (object, prior
		//! world-visible) list the restore guard needs.  Reports how many
		//! objects were actually hidden.  Assumes the caller has already
		//! built the full-set TLAS -- see ObjectSoloRestoreGuard's invariant 1.
		std::vector<std::pair<IObjectPriv*, bool> > ApplyObjectSolo(
			IObjectManager* objMgr, IObjectPriv* keep, unsigned int& outHiddenCount )
		{
			std::vector<std::pair<IObjectPriv*, bool> > prior;
			outHiddenCount = 0;
			if( !objMgr ) return prior;
			const std::vector<std::string> names = CollectObjectNames( objMgr );
			prior.reserve( names.size() );
			for( std::size_t i = 0; i < names.size(); ++i ) {
				IObjectPriv* obj = objMgr->GetItem( names[i].c_str() );
				if( !obj ) continue;
				const bool wasVisible = static_cast<const IObject*>( obj )->IsWorldVisible();
				prior.push_back( std::make_pair( obj, wasVisible ) );
				if( obj != keep && wasVisible ) {
					obj->SetWorldVisible( false );
					++outHiddenCount;
				}
			}
			return prior;
		}

		//! The auto-framing vantage.  A FIXED three-quarter direction --
		//! 35 deg of azimuth off the +Z axis toward +X, 25 deg of elevation
		//! -- expressed as the unit vector FROM the box centre TOWARD the
		//! eye.  +Z is "front" by RISE scene convention (docs/
		//! SCENE_CONVENTIONS.md: a camera at +Z looking at the origin);
		//! 35/25 is the classic product-shot three-quarter, far enough off
		//! axis that two faces and the silhouette's depth both read, and far
		//! enough below the pole that a world +Y up vector is never
		//! degenerate.  Deliberately FIXED rather than derived from the box's
		//! own proportions: a stable vantage means two isolate renders of the
		//! same part before and after an edit are directly comparable.
		void IsolateThreeQuarterOffset( double out[3] )
		{
			static const double kPi      = 3.14159265358979323846;
			static const double kAzimuth = 35.0 * kPi / 180.0;
			static const double kElev    = 25.0 * kPi / 180.0;
			out[0] = std::sin( kAzimuth ) * std::cos( kElev );
			out[1] = std::sin( kElev );
			out[2] = std::cos( kAzimuth ) * std::cos( kElev );
		}

		//! G3b (2026-08-10): the NAMED-VANTAGE set -- G1's single fixed
		//! three-quarter direction generalized to the small closed set a
		//! sketch's `view` can name.  Fills `outDir` (the unit vector FROM
		//! the box centre TOWARD the eye) and `outUp` (the up HINT the fit
		//! and the camera both use).
		//!
		//! `view` is one of AgentSession::kBuildPlanViewValues; ANY other
		//! string (including "") yields G1's three-quarter vantage
		//! BYTE-FOR-BYTE, which is what keeps a plain `isolate` render
		//! (no `target`) identical to what G1 shipped -- the three-quarter
		//! case still calls IsolateThreeQuarterOffset above rather than
		//! re-deriving the same numbers here.
		//!
		//! The three axis-aligned vantages, all stated as FACTS in the tool
		//! surfaces because a silhouette is only interpretable against a
		//! known orientation:
		//!   * front -- eye on +Z looking along -Z; world +X to the right,
		//!     world +Y up.  (+Z is "front" by RISE scene convention, see
		//!     docs/SCENE_CONVENTIONS.md.)
		//!   * side  -- eye on +X looking along -X; world -Z to the right
		//!     (so the object's +Z "front" faces image-left), world +Y up.
		//!   * top   -- eye on +Y looking straight down; world +X to the
		//!     right, world -Z up in the frame (i.e. "away from the front
		//!     view" is at the top of the image).  The up hint here CANNOT
		//!     be world +Y: it is parallel to the view direction, which
		//!     makes IsolateFitDistance's right-vector cross product
		//!     degenerate and would silently fall back to a guessed
		//!     distance.  (0,0,-1) is the only choice that both is
		//!     perpendicular and keeps the frame's handedness matching the
		//!     front view.
		void IsolateVantageOffset( const std::string& view, double outDir[3], double outUp[3] )
		{
			outUp[0] = 0.0; outUp[1] = 1.0; outUp[2] = 0.0;
			if( view == "front" ) {
				outDir[0] = 0.0; outDir[1] = 0.0; outDir[2] = 1.0;
				return;
			}
			if( view == "side" ) {
				outDir[0] = 1.0; outDir[1] = 0.0; outDir[2] = 0.0;
				return;
			}
			if( view == "top" ) {
				outDir[0] = 0.0; outDir[1] = 1.0; outDir[2] = 0.0;
				outUp[0]  = 0.0; outUp[1]  = 0.0; outUp[2]  = -1.0;
				return;
			}
			IsolateThreeQuarterOffset( outDir );
		}

		//! The fraction of each frame half-extent the object's bounding BOX is
		//! allowed to fill -- i.e. a 15% margin on every side, so the
		//! silhouette never touches the frame edge.
		//! G3a (2026-08-10): AgentSession::kElementSketchFillFraction is
		//! deliberately the SAME 0.85 -- G3b compares a sketch target against
		//! an isolate render's silhouette, and the two are comparable only if
		//! both were framed at the same fill.  Change one, change both.
		const double kIsolateFrameFill = 0.85;

		//! Solve the eye distance (along `dir`, the unit vector from the box
		//! centre toward the eye) that fits the WHOLE world AABB inside
		//! kIsolateFrameFill of the frame, for a pinhole of vertical half-tangent
		//! `tanHalfV` and aspect `aspect`.
		//!
		//! Deliberately the EXACT box fit, not the cheaper bounding-SPHERE fit:
		//! a sphere circumscribing an AABB has 1.73x the box's half-extent, so
		//! the sphere fit pushes the eye far enough back that a compact part
		//! ends up filling well under half the frame it was supposed to fill --
		//! which defeats the entire point of framing it.  Per corner, the
		//! perspective constraint |screenOffset| <= tan*fill * depth with
		//! depth = distance - dot(corner-centre, dir) rearranges to a LOWER
		//! BOUND on distance; the answer is the max of those 16 bounds (8
		//! corners x 2 axes).  A floor keeps the eye outside the box even for a
		//! flat/degenerate one (where some bound can be <= the nearest corner's
		//! own offset).
		double IsolateFitDistance( const double bbMin[3], const double bbMax[3],
		                            const double center[3], const double dir[3],
		                            const double upHint[3],
		                            const double tanHalfV, const double aspect )
		{
			// Eye-space basis for the FIXED vantage: forward is -dir.
			const double f[3] = { -dir[0], -dir[1], -dir[2] };
			double r[3] = { f[1]*upHint[2] - f[2]*upHint[1],
			                f[2]*upHint[0] - f[0]*upHint[2],
			                f[0]*upHint[1] - f[1]*upHint[0] };
			const double rl = std::sqrt( r[0]*r[0] + r[1]*r[1] + r[2]*r[2] );
			if( !( rl > 1e-12 ) ) return 0.0;   // caller falls back
			r[0] /= rl; r[1] /= rl; r[2] /= rl;
			const double u[3] = { r[1]*f[2] - r[2]*f[1],
			                      r[2]*f[0] - r[0]*f[2],
			                      r[0]*f[1] - r[1]*f[0] };

			const double limV = tanHalfV * kIsolateFrameFill;
			const double limH = tanHalfV * aspect * kIsolateFrameFill;
			if( !( limV > 1e-9 ) || !( limH > 1e-9 ) ) return 0.0;

			double dist = 0.0, maxTowardEye = 0.0;
			for( int c = 0; c < 8; ++c ) {
				const double p[3] = {
					( ( c & 1 ) ? bbMax[0] : bbMin[0] ) - center[0],
					( ( c & 2 ) ? bbMax[1] : bbMin[1] ) - center[1],
					( ( c & 4 ) ? bbMax[2] : bbMin[2] ) - center[2] };
				// Depth of this corner = dist - toward (it sits `toward`
				// nearer the eye than the centre plane does).
				const double toward = p[0]*dir[0] + p[1]*dir[1] + p[2]*dir[2];
				const double a = std::fabs( p[0]*r[0] + p[1]*r[1] + p[2]*r[2] );
				const double b = std::fabs( p[0]*u[0] + p[1]*u[1] + p[2]*u[2] );
				const double needH = a / limH + toward;
				const double needV = b / limV + toward;
				if( needH > dist ) dist = needH;
				if( needV > dist ) dist = needV;
				if( toward > maxTowardEye ) maxTowardEye = toward;
			}

			// Never place the eye inside (or on) the box: keep at least a
			// tenth of the diagonal of clearance ahead of the nearest corner.
			const double ext[3] = { bbMax[0]-bbMin[0], bbMax[1]-bbMin[1], bbMax[2]-bbMin[2] };
			const double diag = std::sqrt( ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2] );
			const double floorDist = maxTowardEye + diag * 0.1;
			return dist > floorDist ? dist : floorDist;
		}

		//! Screen-space coverage of a world AABB through an explicit camera.
		//! Projects the 8 corners, takes their axis-aligned screen-space
		//! bounds, clips to the frame, and returns area/(W*H) in [0,1].  An
		//! UPPER BOUND on silhouette coverage (an AABB is not the object) --
		//! see AgentRenderResult::isolateBBoxCoverage for why that, rather
		//! than a per-pixel count, is the reported number.  Returns -1 when
		//! any corner lies at or behind the camera plane (a screen-space area
		//! is then undefined) or the inputs are degenerate -- never a guess.
		//! `tanHalfVFov` is tan(fov/2) with fov the FULL VERTICAL field of
		//! view: PinholeCamera::ComputeScaleFromFOV stretches y by
		//! 2*tan(fov/2)/H and x by the same times W/H*pixelAR, so the
		//! vertical half-extent at unit depth is exactly tan(fov/2) and the
		//! horizontal one is that times `aspect`.
		double ProjectedBBoxCoverage( const double bbMin[3], const double bbMax[3],
		                               const double eye[3], const double target[3],
		                               const double upHint[3],
		                               const double tanHalfVFov, const double aspect )
		{
			if( !( tanHalfVFov > 0.0 ) || !( aspect > 0.0 ) ) return -1.0;

			double f[3] = { target[0]-eye[0], target[1]-eye[1], target[2]-eye[2] };
			double fl = std::sqrt( f[0]*f[0] + f[1]*f[1] + f[2]*f[2] );
			if( !( fl > 0.0 ) ) return -1.0;
			f[0] /= fl; f[1] /= fl; f[2] /= fl;

			double r[3] = { f[1]*upHint[2] - f[2]*upHint[1],
			                f[2]*upHint[0] - f[0]*upHint[2],
			                f[0]*upHint[1] - f[1]*upHint[0] };
			double rl = std::sqrt( r[0]*r[0] + r[1]*r[1] + r[2]*r[2] );
			if( !( rl > 1e-12 ) ) return -1.0;   // view direction parallel to the up hint
			r[0] /= rl; r[1] /= rl; r[2] /= rl;

			const double u[3] = { r[1]*f[2] - r[2]*f[1],
			                      r[2]*f[0] - r[0]*f[2],
			                      r[0]*f[1] - r[1]*f[0] };

			double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
			for( int c = 0; c < 8; ++c ) {
				const double p[3] = {
					( c & 1 ) ? bbMax[0] : bbMin[0],
					( c & 2 ) ? bbMax[1] : bbMin[1],
					( c & 4 ) ? bbMax[2] : bbMin[2] };
				const double v[3] = { p[0]-eye[0], p[1]-eye[1], p[2]-eye[2] };
				const double z = v[0]*f[0] + v[1]*f[1] + v[2]*f[2];
				if( !( z > 1e-9 ) ) return -1.0;   // at/behind the camera plane
				const double sx = ( v[0]*r[0] + v[1]*r[1] + v[2]*r[2] ) / ( z * tanHalfVFov * aspect );
				const double sy = ( v[0]*u[0] + v[1]*u[1] + v[2]*u[2] ) / ( z * tanHalfVFov );
				if( sx < minX ) minX = sx;
				if( sx > maxX ) maxX = sx;
				if( sy < minY ) minY = sy;
				if( sy > maxY ) maxY = sy;
			}

			// Clip the [-1,1]x[-1,1] normalized frame.
			if( minX < -1.0 ) minX = -1.0;
			if( maxX >  1.0 ) maxX =  1.0;
			if( minY < -1.0 ) minY = -1.0;
			if( maxY >  1.0 ) maxY =  1.0;
			const double w = maxX - minX;
			const double h = maxY - minY;
			if( !( w > 0.0 ) || !( h > 0.0 ) ) return 0.0;   // entirely off-frame
			const double coverage = ( w * 0.5 ) * ( h * 0.5 );
			return coverage > 1.0 ? 1.0 : coverage;
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

		//----------------------------------------------------------------------
		// S1 (2026-08-11): the STAGED BUILD PROTOCOL.
		//
		// See AgentSession.h's block above FinishElement for the mechanism and
		// docs/agentic-redesign/78-staged-build-protocol.md for the evidence.
		// This is the whole implementation apart from the call-site hooks in
		// the creating and editing verbs (InsertChunk, InsertChunks,
		// InsertGeometryScaffold, ReplaceGeometryScaffold, ProposePatch,
		// RemoveChunk, RemoveChunks), each marked "S1 (2026-08-11)".
		//
		// PLACED HERE, below the isolate helpers, deliberately: FinishElement's
		// payload-fact render reuses G1's ResolveIsolateObject and the ordinary
		// synchronous Render entry point below it, and reusing them is the
		// point -- an element look and an `isolate` look must be the same look.
		//
		// Nothing here reads or writes the Document; nothing takes a controller
		// lock; the only expensive thing any of it does is the ONE render
		// FinishElement issues, through the same path the `render` verb uses.
		//----------------------------------------------------------------------

		const char* AgentSession::BuildPhaseName( AgentBuildPhase p )
		{
			switch( p ) {
				case AgentBuildPhase::Pieces:  return "pieces";
				case AgentBuildPhase::Compose: return "compose";
				case AgentBuildPhase::Plan:    break;
			}
			return "plan";
		}

		std::string AgentSession::ActiveElement() const
		{
			if( mBuildPhase != AgentBuildPhase::Pieces ) return std::string();
			if( mActiveElement >= mBuildPlan.size() )    return std::string();
			return mBuildPlan[ mActiveElement ].element;
		}

		std::vector<std::string> AgentSession::ElementChunks( const std::string& element ) const
		{
			std::vector<std::string> out;
			if( element.empty() ) return out;
			for( std::size_t i = 0; i < mChunkAttribution.size(); ++i ) {
				if( mChunkAttribution[i].element == element )
					out.push_back( mChunkAttribution[i].chunk );
			}
			return out;
		}

		std::string AgentSession::ChunkElement( const std::string& chunk ) const
		{
			const ChunkAttribution_* a = FindChunkAttribution_( chunk );
			return a ? a->element : std::string();
		}

		const AgentSession::ChunkAttribution_* AgentSession::FindChunkAttribution_(
			const std::string& chunk ) const
		{
			if( chunk.empty() ) return nullptr;
			for( std::size_t i = 0; i < mChunkAttribution.size(); ++i ) {
				if( mChunkAttribution[i].chunk == chunk ) return &mChunkAttribution[i];
			}
			return nullptr;
		}

		void AgentSession::AttributeChunkToActiveElement_( const std::string& name,
		                                                   const std::string& kind )
		{
			// THE THREE NO-OP CASES, each load-bearing.  (1) The machinery is
			// off -- nothing is ever attributed, so no edit is ever refused for
			// belonging to an element.  (2) The session is not in the Pieces
			// phase -- a chunk created in PLAN (before any window opened) or in
			// COMPOSE (only non-geometry can be created there) belongs to no
			// element and stays freely editable forever, which is exactly the
			// design's "never refuse on unattributed chunks" rule.  (3) An
			// UNNAMED chunk (film, rasterizer, a bare camera) has no key to
			// record it under -- and it is a phase-exempt kind anyway.
			if( !BuildProtocolActive_() ) return;
			if( mBuildPhase != AgentBuildPhase::Pieces ) return;
			if( name.empty() ) return;
			if( mActiveElement >= mBuildPlan.size() ) return;

			const std::string& element = mBuildPlan[ mActiveElement ].element;
			for( std::size_t i = 0; i < mChunkAttribution.size(); ++i ) {
				if( mChunkAttribution[i].chunk != name ) continue;
				// LAST CREATION WINS.  A name can be created twice in one
				// session only by being removed in between (there is no rename
				// verb and an insert of a duplicate name is refused), so the
				// chunk that exists in the document now came from THIS window.
				mChunkAttribution[i].element = element;
				mChunkAttribution[i].kind    = kind;
				return;
			}
			ChunkAttribution_ a;
			a.chunk   = name;
			a.element = element;
			a.kind    = kind;
			mChunkAttribution.push_back( a );
		}

		void AgentSession::DropChunkAttribution_( const std::string& chunk )
		{
			if( chunk.empty() ) return;
			for( std::size_t i = 0; i < mChunkAttribution.size(); ++i ) {
				if( mChunkAttribution[i].chunk != chunk ) continue;
				mChunkAttribution.erase( mChunkAttribution.begin() + static_cast<std::ptrdiff_t>( i ) );
				return;
			}
		}

		bool AgentSession::KindIsPhaseExemptCategory_( const std::string& kind )
		{
			if( kind.empty() ) return false;
			const ChunkDescriptor* d = DescriptorForKeyword( String( kind.c_str() ) );
			if( !d ) return false;
			// THE RULE (S1 fix-round 2026-08-11, supervisor design call):
			// ATTRIBUTE EVERYTHING (for measurement), REFUSE ONLY ON
			// FORM-BEARING CHUNKS.  Attribution is unchanged -- every kind
			// listed here is still recorded against the element that created
			// it, so finish_element and the census see it.  What this predicate
			// decides is only whether an edit to it can be REFUSED across
			// element windows.
			//
			// Lights, cameras, film, rasterizers and rasterizer outputs are
			// SCENE-WIDE, not element-owned -- a model that cannot light or
			// re-aim at its own isolated element cannot see it.  Materials and
			// painters join them because SHARING one across elements is
			// ordinary authoring, not an edge case: one skin material on a head
			// and a pair of hands, one fabric painter on a robe and a hat trim.
			// Refusing those edits would refuse the most common legitimate
			// cross-element reach there is, and over-refusal (E1's review P1)
			// is the failure mode this design fears most.
			//
			// What is LEFT refusable is the form-bearing set: geometry chunks
			// and the standard_object that places them -- exactly the pieces
			// whose serialization the protocol exists to enforce, and, together
			// with materials and painters, the whole of what a model actually
			// creates inside an element window.  The REGISTRY is the
			// classifier, so a light (or material, or painter) kind added later
			// is exempt with no edit here.
			return d->category == ChunkCategory::Light      ||
			       d->category == ChunkCategory::Camera     ||
			       d->category == ChunkCategory::Film       ||
			       d->category == ChunkCategory::Rasterizer ||
			       d->category == ChunkCategory::RasterizerOutput ||
			       d->category == ChunkCategory::Material   ||
			       d->category == ChunkCategory::Painter;
		}

		std::string AgentSession::RefuseForPhase_( const char* verb, const std::string& body,
		                                            std::string* outGiveUpNotice )
		{
			// The SAME three-outcome shape CheckBuildPlanGate_ has, and the
			// same reasons for it: refuse while under the cap, then GIVE UP
			// rather than refuse a fourth time, with the give-up notice folded
			// into the call's own result so the event is visible in the payload
			// a trajectory census reads rather than only in a log line.  The
			// counter is SHARED by both phase refusals (cross-element edit and
			// compose-phase creation) because they are two faces of one
			// mechanism: a model that cannot work the protocol should not have
			// to exhaust two separate budgets to get out of it.
			if( !BuildProtocolActive_() || mBuildPhaseGaveUp ) return std::string();

			if( mBuildPhaseRefusalCount < kBuildPhaseMaxRefusals )
			{
				++mBuildPhaseRefusalCount;
				const int remaining = kBuildPhaseMaxRefusals - mBuildPhaseRefusalCount;
				// FACTS ONLY -- no advice about what to build, no judgement of
				// what has been built, no score.  The remaining-attempts count
				// is a claim this message must get right: it is exactly the cap
				// minus the count just incremented to, so it cannot drift from
				// what the next call actually does.
				std::string msg = std::string( verb ) + " refused: " + body +
					" Nothing in the document was changed by this call. " +
					std::to_string( remaining ) +
					( remaining == 1 ? " more call will be refused"
					                 : " more calls will be refused" ) +
					" for this reason before the phase rules stop intercepting.";
				return msg;
			}

			mBuildPhaseGaveUp = true;
			if( outGiveUpNotice ) {
				*outGiveUpNotice = std::string( "build phase: not satisfied after " ) +
					std::to_string( kBuildPhaseMaxRefusals ) + " refusals -- " + verb +
					" proceeded and the phase rules have stopped intercepting for this session; "
					"no further call will be refused for belonging to another element or for "
					"creating geometry in the compose phase. Elements, attribution, "
					"finish_element and reopen_element all keep working.";
			}
			return std::string();
		}

		std::string AgentSession::CheckElementWindowForEdit_( const char* verb,
		                                                       const std::string& target,
		                                                       std::string* outGiveUpNotice )
		{
			if( !BuildProtocolActive_() || mBuildPhaseGaveUp ) return std::string();
			if( mBuildPhase != AgentBuildPhase::Pieces )       return std::string();
			if( target.empty() )                               return std::string();
			if( mActiveElement >= mBuildPlan.size() )          return std::string();

			const ChunkAttribution_* a = FindChunkAttribution_( target );
			// NO ATTRIBUTION IS ALWAYS ALLOWED.  Pre-existing scene content and
			// anything created before the first window opened belong to no
			// element; refusing there would lock a model out of the scene it
			// was handed.
			if( !a ) return std::string();
			const std::string& active = mBuildPlan[ mActiveElement ].element;
			if( a->element == active ) return std::string();
			if( KindIsPhaseExemptCategory_( a->kind ) ) return std::string();

			const std::string body =
				"\"" + target + "\" was created while the element \"" + a->element +
				"\" was active, and the active element is now \"" + active +
				"\". finish_element closes \"" + active + "\" and moves on; reopen_element with "
				"element \"" + a->element + "\" re-enters that element's window from here.";
			return RefuseForPhase_( verb, body, outGiveUpNotice );
		}

		std::string AgentSession::CheckComposePhaseForCreate_( const char* verb,
		                                                        std::string* outGiveUpNotice )
		{
			if( !BuildProtocolActive_() || mBuildPhaseGaveUp ) return std::string();
			if( mBuildPhase != AgentBuildPhase::Compose )      return std::string();

			// WHAT COMPOSE IS FOR, stated as fact: what is allowed and what is
			// not, plus the escape.  No advice about what to adjust -- whether
			// the arrangement pass happens, and what it changes, is precisely
			// what this slice measures.
			const std::string body =
				"this session is in the compose phase, which allows arrangement, lighting, camera, "
				"materials and edits to any element's chunks, and refuses the creation of new "
				"geometry. reopen_element with the name of an element re-enters that element's "
				"window, where creating geometry is allowed again.";
			return RefuseForPhase_( verb, body, outGiveUpNotice );
		}

		namespace
		{
			//! S1: lowercase a copy, for the piece-name NAMING check.  ASCII
			//! only and deliberately so -- it is a report about names, never a
			//! gate, so a non-ASCII piece name that fails to match is reported
			//! as "not named", which is exactly what it is.
			std::string LowerAscii_( const std::string& s )
			{
				std::string out = s;
				for( std::size_t i = 0; i < out.size(); ++i ) {
					if( out[i] >= 'A' && out[i] <= 'Z' ) out[i] = static_cast<char>( out[i] - 'A' + 'a' );
				}
				return out;
			}
		}

		AgentSession::AgentFinishElementResult AgentSession::FinishElement()
		{
			AgentFinishElementResult out;
			out.phase = BuildPhaseName( mBuildPhase );

			// THE THREE DO-NOTHING CASES.  None of them is a phase refusal:
			// they change no state, cost no budget, and are never counted --
			// there is nothing here for a cap to bound.
			if( !BuildProtocolActive_() ) {
				out.message = "finish_element did nothing: the staged build protocol is off for this "
				              "session, so no element window is open.";
				return out;
			}
			if( mBuildPhase == AgentBuildPhase::Plan ) {
				out.message = "finish_element did nothing: no build plan has been filed in this "
				              "session, so no element is active. file_build_plan declares the elements "
				              "and makes the first one active.";
				return out;
			}
			if( mBuildPhase == AgentBuildPhase::Compose ) {
				out.message = "finish_element did nothing: every element in the filed plan is already "
				              "finished and this session is in the compose phase. reopen_element with "
				              "an element name re-enters that element's window.";
				return out;
			}
			if( mActiveElement >= mBuildPlan.size() ) {
				// Unreachable by construction (every writer of mActiveElement
				// stores a resolved index into the CURRENT plan, and a re-filing
				// rewrites both together) -- reported rather than asserted, so a
				// future writer that breaks the invariant fails loudly and
				// factually instead of indexing out of range.
				out.message = "finish_element did nothing: the active element index does not resolve "
				              "against the filed build plan.";
				return out;
			}

			const AgentBuildPlanEntry entry = mBuildPlan[ mActiveElement ];
			out.ok      = true;
			out.element = entry.element;
			out.chunks  = ElementChunks( entry.element );

			// THE PIECE CHECK IS A NAMING OBSERVATION.  A declared piece counts
			// as "named" when its own text appears, case-insensitively, inside
			// the NAME of at least one chunk attributed to this element.  That
			// is all it can honestly claim: nothing here inspects geometry, and
			// a correctly built beard in a chunk called `wiz_sdf` reads as "not
			// named" while an empty box called `beard` reads as "named".  The
			// message says so outright, because a fact a model over-reads is a
			// fact that steers behaviour it never described.
			{
				std::vector<std::string> lowerChunks;
				lowerChunks.reserve( out.chunks.size() );
				for( std::size_t i = 0; i < out.chunks.size(); ++i )
					lowerChunks.push_back( LowerAscii_( out.chunks[i] ) );
				for( std::size_t p = 0; p < entry.pieces.size(); ++p ) {
					const std::string needle = LowerAscii_( entry.pieces[p] );
					bool found = false;
					if( !needle.empty() ) {
						for( std::size_t c = 0; c < lowerChunks.size() && !found; ++c )
							found = lowerChunks[c].find( needle ) != std::string::npos;
					}
					if( found ) out.piecesNamed.push_back( entry.pieces[p] );
					else        out.piecesNotNamed.push_back( entry.pieces[p] );
				}
			}

			// ADVANCE.  Mark this element finished, then take the FIRST element
			// with no finish recorded, in plan order -- "first unfinished"
			// rather than "the next index" because reopen_element can leave an
			// earlier element open, and skipping it would strand it forever.
			if( mElementFinished.size() != mBuildPlan.size() )
				mElementFinished.resize( mBuildPlan.size(), false );
			mElementFinished[ mActiveElement ] = true;
			std::size_t next = mBuildPlan.size();
			for( std::size_t i = 0; i < mBuildPlan.size(); ++i ) {
				if( !mElementFinished[i] ) { next = i; break; }
			}
			if( next < mBuildPlan.size() ) {
				mActiveElement  = next;
				mBuildPhase     = AgentBuildPhase::Pieces;
				out.nextElement = mBuildPlan[next].element;
			}
			else {
				mBuildPhase = AgentBuildPhase::Compose;
			}
			out.phase = BuildPhaseName( mBuildPhase );

			// THE PAYLOAD FACT: an isolate render of the element just closed --
			// a look the model did not ask for, arriving in the result of a
			// call it just made.  It reuses G1's machinery whole (the same
			// ResolveIsolateObject, the same auto-framing, the same
			// agent-surface caps), because an element look and an `isolate`
			// look must be the same look.
			//
			// WHICH OBJECT.  Every chunk attributed to this element whose
			// registry category is Object and which G1 can actually isolate --
			// that filter drops CSG operands (world-invisible by construction)
			// and any object the scene no longer has, for free and with G1's
			// own definition of renderable.  With several, the one with the
			// LARGEST world bounding-box diagonal, stated in the message so the
			// choice is never silent.
			std::string isolateName;
			{
				double bestDiag = -1.0;
				for( std::size_t i = 0; i < out.chunks.size(); ++i ) {
					const ChunkAttribution_* a = FindChunkAttribution_( out.chunks[i] );
					if( !a ) continue;
					const ChunkDescriptor* d = DescriptorForKeyword( String( a->kind.c_str() ) );
					if( !d || d->category != ChunkCategory::Object ) continue;
					IObjectManager* objMgr = mJob ? mJob->GetObjects() : nullptr;
					IObjectPriv* obj = nullptr;
					std::string ignored;
					if( !ResolveIsolateObject( objMgr, out.chunks[i], obj, ignored ) || !obj ) continue;
					++out.isolateCandidates;
					const BoundingBox bb = static_cast<const IObject*>( obj )->getBoundingBox();
					const double dx = bb.ur.x - bb.ll.x;
					const double dy = bb.ur.y - bb.ll.y;
					const double dz = bb.ur.z - bb.ll.z;
					const double diag = std::sqrt( dx*dx + dy*dy + dz*dz );
					// A non-finite or zero-extent box loses to any real one but
					// still beats "no object at all" (bestDiag starts at -1), so
					// a degenerate object is rendered rather than silently
					// dropped -- the render itself is then the honest report.
					const double score = RISE::IsFiniteDouble( diag ) ? diag : 0.0;
					if( score > bestDiag ) { bestDiag = score; isolateName = out.chunks[i]; }
				}
			}

			std::string renderNote;
			if( !isolateName.empty() ) {
				out.isolateObject = isolateName;
				AgentRenderParams rp;
				rp.isolate           = isolateName;
				// The agent-surface caps (<= 256 px long edge, <= 16 spp) apply
				// exactly as they do to a model-issued `render`: this image is
				// spent on the model's context, so it is sized by the same
				// policy and never by the scene's authored Film.
				rp.fromAgentSurface  = true;
				const AgentRenderResult rr = Render( rp );
				if( rr.ok ) {
					out.png = ReadImage( kAgentSurfaceMaxRenderEdge, out.width, out.height );
					out.rendered = !out.png.empty();
				}
				if( out.rendered ) {
					renderNote = " Isolate render of \"" + isolateName + "\"";
					if( out.isolateCandidates > 1 )
						renderNote += " (the largest of the " + std::to_string( out.isolateCandidates ) +
							" objects recorded against this element, by bounding-box diagonal)";
					renderNote += ", auto-framed, with every other object hidden for this render only.";
				}
				else {
					// A failed render never fails the call -- the advance
					// already happened -- but it is never silent either: a
					// missing image with no explanation reads as "the element
					// renders empty".
					renderNote = " The isolate render of \"" + isolateName +
						"\" did not produce an image" +
						( rr.ok || rr.message.empty() ? std::string( "." )
						                              : ( ": " + rr.message ) );
				}
			}
			else {
				renderNote = out.chunks.empty()
					? std::string( " No chunk was recorded against this element, so there was no object "
					               "to render." )
					: std::string( " No renderable object was recorded against this element, so there "
					               "is no isolate render with this result." );
			}

			// THE MESSAGE: what was recorded, what the piece check found and
			// what it means, where the session is now, and what was rendered.
			// Facts in the same register as the filing echo -- nothing here
			// characterizes the work, and nothing suggests what to do next.
			std::string m = "element \"" + entry.element + "\" finished. Chunks recorded against it: ";
			if( out.chunks.empty() ) m += "none";
			else {
				for( std::size_t i = 0; i < out.chunks.size(); ++i ) {
					if( i ) m += ", ";
					m += out.chunks[i];
				}
			}
			m += ". Declared pieces: " + std::to_string( entry.pieces.size() ) + " -- ";
			if( !out.piecesNamed.empty() ) {
				m += "named by a chunk name: ";
				for( std::size_t i = 0; i < out.piecesNamed.size(); ++i ) {
					if( i ) m += ", ";
					m += out.piecesNamed[i];
				}
				m += out.piecesNotNamed.empty() ? "" : "; ";
			}
			if( !out.piecesNotNamed.empty() ) {
				m += "not named by any chunk name: ";
				for( std::size_t i = 0; i < out.piecesNotNamed.size(); ++i ) {
					if( i ) m += ", ";
					m += out.piecesNotNamed[i];
				}
			}
			m += ". That check is a case-insensitive substring match of each piece name against the "
			     "NAMES of the chunks recorded against this element -- it says nothing about what "
			     "was built or how well.";
			m += renderNote;
			if( !out.nextElement.empty() ) {
				m += " The active element is now \"" + out.nextElement + "\"";
				std::size_t remaining = 0;
				for( std::size_t i = 0; i < mElementFinished.size(); ++i )
					if( !mElementFinished[i] ) ++remaining;
				m += " (" + std::to_string( remaining ) +
					( remaining == 1 ? " element" : " elements" ) + " with no finish recorded).";
			}
			else {
				m += " Every element in the plan is now finished and the session is in the compose "
				     "phase: arrangement, lighting, camera, materials and edits to any element's "
				     "chunks are allowed, creating new geometry is refused, and reopen_element with "
				     "an element name re-enters that element's window.";
			}
			out.message = m;
			return out;
		}

		AgentSession::AgentReopenElementResult AgentSession::ReopenElement( const std::string& element )
		{
			AgentReopenElementResult out;
			out.previousPhase = BuildPhaseName( mBuildPhase );
			out.phase         = out.previousPhase;

			if( !BuildProtocolActive_() ) {
				out.message = "reopen_element did nothing: the staged build protocol is off for this "
				              "session, so there are no element windows to re-enter.";
				return out;
			}
			if( mBuildPlan.empty() ) {
				out.message = "reopen_element did nothing: no build plan has been filed in this "
				              "session, so there is no element to reopen. file_build_plan declares "
				              "the elements.";
				return out;
			}
			std::size_t at = mBuildPlan.size();
			for( std::size_t i = 0; i < mBuildPlan.size(); ++i ) {
				// EXACT match, no normalization -- the same rule
				// FindElementSketch follows, so `target` in a render and
				// `element` here name elements the same way.
				if( mBuildPlan[i].element == element ) { at = i; break; }
			}
			if( at == mBuildPlan.size() ) {
				out.message = "reopen_element did nothing: \"" + element + "\" is not in the filed "
					"build plan, which lists: " + ElementSketchNameList() + ".";
				return out;
			}

			// NEVER GATED, NEVER COUNTED.  This verb is the escape the
			// compose-phase refusal names; refusing it, or spending a refusal
			// budget on it, would strand exactly the session that needs it.
			if( mElementFinished.size() != mBuildPlan.size() )
				mElementFinished.resize( mBuildPlan.size(), false );
			mElementFinished[at] = false;
			mActiveElement       = at;
			mBuildPhase          = AgentBuildPhase::Pieces;

			out.ok      = true;
			out.element = mBuildPlan[at].element;
			out.phase   = BuildPhaseName( mBuildPhase );
			out.chunks  = ElementChunks( out.element );
			for( std::size_t i = 0; i < mBuildPlan.size(); ++i )
				if( !mElementFinished[i] ) out.unfinished.push_back( mBuildPlan[i].element );

			std::string m = "element \"" + out.element + "\" reopened: it is the active element and "
				"the session is in the pieces phase";
			if( out.previousPhase != std::string( "pieces" ) )
				m += " (it was in the " + out.previousPhase + " phase)";
			m += ". Chunks already recorded against it: ";
			if( out.chunks.empty() ) m += "none";
			else {
				for( std::size_t i = 0; i < out.chunks.size(); ++i ) {
					if( i ) m += ", ";
					m += out.chunks[i];
				}
			}
			m += ". Declared pieces: ";
			for( std::size_t p = 0; p < mBuildPlan[at].pieces.size(); ++p ) {
				if( p ) m += ", ";
				m += mBuildPlan[at].pieces[p];
			}
			m += ". Chunks created from here are recorded against \"" + out.element +
				"\", and finish_element closes it again. Elements with no finish recorded: " +
				std::to_string( out.unfinished.size() ) + ".";
			out.message = m;
			return out;
		}

		//----------------------------------------------------------------------
		// S2 (2026-08-11): CLEAN-ROOM CONSTRUCTION -- build_element and
		// place_element.
		//
		// See AgentSession.h's block above AgentTextCompleter for the mechanism
		// and docs/agentic-redesign/79-clean-room-construction.md for the
		// measurement.  This is the whole implementation apart from the ONE
		// call-site hook in the two hand-authoring insert verbs (InsertChunk,
		// InsertChunks), each marked "S2 (2026-08-11)".
		//
		// PLACED HERE, below the arc-78 protocol, deliberately: every gate this
		// slice adds is a THIRD ARM of RefuseForPhase_ above, and BuildElement's
		// bbox report reuses the same ResolveIsolateObject filter FinishElement's
		// isolate pick uses.
		//----------------------------------------------------------------------

		const char* AgentSession::LocalFrameContract()
		{
			// SENT TO THE BUILDER VERBATIM.  Requirements and facts only: what
			// frame to author in, what to name things, what to include and what
			// not to.  Nothing here says what the element should LOOK like or
			// how much detail to put in it -- the whole point of the fresh
			// context is that the builder decides that with its full attention.
			return
				"LOCAL FRAME -- build this element as if it stood alone:\n"
				"1. ORIGIN AT THE BASE CENTRE. The element's lowest point sits at y = 0, "
				"horizontally centred on x = 0, z = 0. Something that will end up airborne is "
				"still authored base-at-origin; lift is placement, not construction.\n"
				"2. +Y IS UP, AND THE ELEMENT FACES +Z (toward a camera on the +Z side).\n"
				"3. HEIGHT BUDGET. Occupy roughly the requested height in Y. This is a request, "
				"not a limit: the realised bounding box is measured and reported back to the "
				"caller, and nothing is refused for missing it.\n"
				"4. NAME PREFIX. Every chunk name begins with the required prefix given above. "
				"This one IS enforced: a chunk whose name does not begin with it is rejected and "
				"NOT renamed, because renaming would break the references between your own "
				"chunks.\n"
				"5. SELF-CONTAINED. Define your own painters and materials, and finish with at "
				"least one standard_object binding a geometry to a material. Every name a chunk "
				"references must be one you defined in this same answer.\n"
				"6. DO NOT AUTHOR: cameras, lights, film, rasterizers, ground planes, or any "
				"world placement. Those belong to the scene, not to this element, and a chunk "
				"of those kinds will be rejected.\n"
				"7. EXACT SYNTAX -- a correctly-formed, correctly-named chunk looks like this "
				"(replace <prefix> with the required prefix given above; every name is a BARE "
				"TOKEN, never in quotes -- this scene language has no quoted-string syntax, so "
				"quoting a name just makes the quote characters part of it):\n"
				"uniformcolor_painter\n"
				"{\n"
				"\tname <prefix>rock_pnt\n"
				"\tcolor 0.4 0.3 0.2\n"
				"}\n"
				"lambertian_material\n"
				"{\n"
				"\tname <prefix>rock_mat\n"
				"\treflectance <prefix>rock_pnt\n"
				"}\n"
				// DELIBERATELY NO GEOMETRY CHUNK IN THIS EXAMPLE.  The
				// example exists to fix a SYNTAX failure (a builder that
				// wrote its names as chunk keywords, then as quoted
				// strings), and this project has measured that an example
				// moves COPYING -- so a concrete `box_geometry { }` here
				// would be a drop-in anchor on the exact axis arc 79
				// measures (SDF part count per element).  The chunk shape
				// and the naming rule are fully demonstrated by the painter
				// / material / object chain; the geometry kinds and the
				// part grammar are already given in full above, where they
				// carry no worked example to copy.
				"standard_object\n"
				"{\n"
				"\tname <prefix>rock_obj\n"
				"\tgeometry <prefix>rock_geo\n"
				"\tmaterial <prefix>rock_mat\n"
				"}\n"
				"The geometry chunk named <prefix>rock_geo there is written the same way: its "
				"kind on its own line, its brace on its own line, and a bare-token name carrying "
				"the prefix.";
		}

		std::string AgentSession::ElementChunkNamePrefix( const std::string& element )
		{
			std::string out;
			bool pendingSep = false;
			for( std::size_t i = 0; i < element.size(); ++i ) {
				const unsigned char c = static_cast<unsigned char>( element[i] );
				const bool alnum = ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'z' ) ||
				                   ( c >= 'A' && c <= 'Z' );
				if( alnum ) {
					if( pendingSep && !out.empty() ) out += '_';
					pendingSep = false;
					out += static_cast<char>( ( c >= 'A' && c <= 'Z' ) ? ( c - 'A' + 'a' ) : c );
				}
				else {
					pendingSep = true;
				}
			}
			// An element name with no ASCII alphanumeric in it at all still
			// needs a legal, STATED prefix -- the builder is told the literal
			// string, so any deterministic answer works as long as the check
			// and the prompt agree, which they do by both calling this.
			if( out.empty() ) out = "element";
			return out + "_";
		}

		void AgentSession::ExtractChunkTexts( const std::string& text,
		                                       std::vector<std::string>& outChunks,
		                                       std::vector<std::string>& outProblems )
		{
			outChunks.clear();
			outProblems.clear();
			if( text.empty() ) return;

			// (1) Drop markdown fence LINES.  A provider asked for scene text
			// very often wraps it in ``` fences; the fence is not part of any
			// chunk and would otherwise sit in the prose the scan skips
			// anyway -- removing it up front keeps the line numbers this
			// function reports meaningful (a removed fence line is replaced by
			// an EMPTY line, not deleted, so reported line numbers still match
			// the builder's own answer).
			std::string src;
			src.reserve( text.size() );
			{
				std::size_t at = 0;
				while( at <= text.size() ) {
					std::size_t eol = text.find( '\n', at );
					const bool last = ( eol == std::string::npos );
					if( last ) eol = text.size();
					const std::string line = text.substr( at, eol - at );
					std::size_t b = 0, e = line.size();
					while( b < e && ( line[b] == ' ' || line[b] == '\t' || line[b] == '\r' ) ) ++b;
					while( e > b && ( line[e-1] == ' ' || line[e-1] == '\t' || line[e-1] == '\r' ) ) --e;
					const bool fence = ( e - b ) >= 3 && line.compare( b, 3, "```" ) == 0;
					if( !fence ) src += line;
					if( !last ) src += '\n';
					if( last ) break;
					at = eol + 1;
				}
			}

			// (2) The single-pass balanced-brace walk.  `lineOf` is computed on
			// demand rather than tracked, because it is needed only when
			// something is wrong.
			const auto lineOf = [&src]( std::size_t pos ) -> int {
				int line = 1;
				for( std::size_t i = 0; i < pos && i < src.size(); ++i )
					if( src[i] == '\n' ) ++line;
				return line;
			};
			// Skip the two comment forms the CST lexer absorbs (`#` to
			// end-of-line and `/* ... */`) so a brace inside a comment can
			// neither open nor close a chunk.  Returns the index just past the
			// comment, or `i` when there is no comment at `i`.
			const auto skipComment = [&src]( std::size_t i ) -> std::size_t {
				if( i >= src.size() ) return i;
				if( src[i] == '#' ) {
					const std::size_t nl = src.find( '\n', i );
					return ( nl == std::string::npos ) ? src.size() : nl;
				}
				if( src[i] == '/' && i + 1 < src.size() && src[i+1] == '*' ) {
					const std::size_t end = src.find( "*/", i + 2 );
					return ( end == std::string::npos ) ? src.size() : ( end + 2 );
				}
				return i;
			};
			// S2 fix-round (2026-08-11, P2): the BACKWARD twin of `skipComment`,
			// for the keyword scan below.  `k` is a boundary such that
			// `src[k-1]` (when `k > 0`) is the nearest not-yet-skipped
			// character to the left; returns the new boundary after skipping
			// ONE trailing comment that ends exactly at `k`, or `k` unchanged
			// when none does.  Without this, `keyword /* note */\n{` and
			// `keyword # note\n{` -- both legal RISE syntax -- made the
			// backward whitespace-only skip stop inside the comment body,
			// mis-scanning the keyword (or finding none) and silently
			// dropping a well-formed chunk; see the forward `skipComment`
			// call at the matching site for why comments must be transparent
			// to this walk in both directions.
			const auto skipCommentBackward = [&src]( std::size_t k ) -> std::size_t {
				// A block comment ending right at `k`: `src[k-2..k-1] == "*/"`.
				// Find the matching `/*` and jump to its start; the minimum
				// comment `/**/` is 4 chars, so `k >= 4` before searching.
				if( k >= 4 && src[k-2] == '*' && src[k-1] == '/' ) {
					const std::size_t start = src.rfind( "/*", k - 4 );
					if( start != std::string::npos ) return start;
				}
				// A line comment covering `k`: walk back to the start of the
				// current line and look for a `#` before `k` on it.  RISE
				// keywords/identifiers never contain `#`, so the first `#` on
				// the line is unambiguously the comment's start.
				const std::size_t lineStart =
					( k == 0 ) ? 0 : ( [&]() -> std::size_t {
						const std::size_t nl = src.rfind( '\n', k - 1 );
						return ( nl == std::string::npos ) ? std::size_t( 0 ) : nl + 1;
					} )();
				const std::size_t hash = src.find( '#', lineStart );
				if( hash != std::string::npos && hash < k ) return hash;
				return k;
			};

			// Walk from the `{` at `open` to just past its matching `}`.
			// Returns npos when the braces never balance.  Shared by the
			// keyword-less skip and the chunk-body scan so the two can never
			// disagree about where a block ends.
			const auto matchBrace = [&src, &skipComment]( std::size_t open ) -> std::size_t {
				std::size_t j = open + 1;
				int depth = 1;
				while( j < src.size() && depth > 0 ) {
					const std::size_t afterC = skipComment( j );
					if( afterC != j ) { j = afterC; continue; }
					if( src[j] == '{' ) ++depth;
					else if( src[j] == '}' ) --depth;
					++j;
				}
				return ( depth > 0 ) ? std::string::npos : j;
			};

			std::size_t i = 0;
			while( i < src.size() ) {
				const std::size_t afterComment = skipComment( i );
				if( afterComment != i ) { i = afterComment; continue; }

				if( src[i] == '}' ) {
					char buf[128];
					std::snprintf( buf, sizeof( buf ),
						"a closing brace on line %d had no chunk open before it", lineOf( i ) );
					outProblems.push_back( buf );
					++i;
					continue;
				}
				if( src[i] != '{' ) { ++i; continue; }

				// The KEYWORD is the identifier token immediately before this
				// brace, whitespace and comments allowed in between.  Skip
				// whitespace and comments alternately (a comment can be
				// followed by more whitespace, then another comment) until
				// neither moves `k` -- symmetric with `matchBrace`'s forward
				// interleaving of the same two skips.
				std::size_t k = i;
				for( bool moved = true; moved; ) {
					moved = false;
					while( k > 0 ) {
						const char c = src[k-1];
						if( c == ' ' || c == '\t' || c == '\r' || c == '\n' ) { --k; moved = true; continue; }
						break;
					}
					const std::size_t afterC = skipCommentBackward( k );
					if( afterC != k ) { k = afterC; moved = true; }
				}
				const std::size_t kwEnd = k;
				while( k > 0 ) {
					const unsigned char c = static_cast<unsigned char>( src[k-1] );
					const bool ident = ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'z' ) ||
					                   ( c >= 'A' && c <= 'Z' ) || c == '_';
					if( !ident ) break;
					--k;
				}
				const std::string keyword = ( kwEnd > k ) ? src.substr( k, kwEnd - k ) : std::string();
				if( keyword.empty() ) {
					char buf[128];
					std::snprintf( buf, sizeof( buf ),
						"an opening brace on line %d had no chunk keyword before it", lineOf( i ) );
					outProblems.push_back( buf );
					// Skip the WHOLE block, not just the brace: its closing
					// brace belongs to it, and reporting that close as a
					// second, stray problem would turn one defect into two.
					const std::size_t past = matchBrace( i );
					if( past == std::string::npos ) {
						char ubuf[160];
						std::snprintf( ubuf, sizeof( ubuf ),
							"the brace block opened on line %d was never closed; nothing after it "
							"could be read as a chunk", lineOf( i ) );
						outProblems.push_back( ubuf );
						return;
					}
					i = past;
					continue;
				}

				// Walk to the matching close.
				const std::size_t j = matchBrace( i );
				if( j == std::string::npos ) {
					// THE FAILURE THE HAND SIMULATION SWALLOWED.  Report it and
					// stop: everything after an unclosed brace is inside that
					// chunk by definition, so continuing would invent chunks.
					char buf[192];
					std::snprintf( buf, sizeof( buf ),
						"the `%s` chunk opened on line %d was never closed (its braces do not balance "
						"before the end of the answer); it was not inserted, and nothing after it "
						"could be read as a chunk either",
						keyword.c_str(), lineOf( i ) );
					outProblems.push_back( buf );
					return;
				}

				// Re-emit in the CANONICAL form InsertChunk requires: the
				// keyword alone on its line, both braces on their own lines.
				// The body is passed through verbatim.  This normalization is
				// deliberate -- `sdf_geometry {` on one line is the single most
				// likely formatting slip and refusing it would spend the repair
				// retry on punctuation rather than on substance.
				std::string body = src.substr( i + 1, ( j - 1 ) - ( i + 1 ) );
				while( !body.empty() && ( body[0] == '\n' || body[0] == '\r' ) )
					body.erase( body.begin() );
				while( !body.empty() && ( body[body.size()-1] == '\n' || body[body.size()-1] == '\r' ||
				                          body[body.size()-1] == ' '  || body[body.size()-1] == '\t' ) )
					body.erase( body.size() - 1 );
				outChunks.push_back( keyword + "\n{\n" + body + "\n}\n" );
				i = j;
			}
		}

		std::string AgentSession::CheckFirstGeometryThroughCleanRoom_( const char* verb,
		                                                                std::string* outGiveUpNotice )
		{
			if( !BuildProtocolActive_() || mBuildPhaseGaveUp ) return std::string();
			if( mBuildPhase != AgentBuildPhase::Pieces )       return std::string();
			// NEVER REFUSE ON BEHALF OF A PATH THAT DOES NOT EXIST.  On a host
			// with no text completer installed `build_element` answers with a
			// capability statement, so forcing construction through it would
			// strand the session outright -- the same capability-conditional
			// rule the imagine half of the build-plan gate follows.
			if( !BuildCapable() )                              return std::string();
			// BuildElement's own insertion is the clean room; refusing it would
			// have the mechanism refuse the verb it names.
			if( mInBuildElementInsert )                        return std::string();
			if( mActiveElement >= mBuildPlan.size() )          return std::string();

			const std::string& active = mBuildPlan[ mActiveElement ].element;
			// ANY chunk already recorded against the element lifts this
			// permanently for that element (design sec 4): construction goes
			// through the clean room, refinement stays in the model's hands.
			if( !ElementChunks( active ).empty() )             return std::string();

			const std::string body =
				"the element \"" + active + "\" has no chunk recorded against it yet, and the first "
				"geometry for an element is built by build_element -- one call, in which " +
				( mTextCompleter.providerName.empty() ? std::string( "this session's provider" )
				                                      : ( "`" + mTextCompleter.providerName + "`" ) ) +
				" constructs the whole element on its own and the result is checked and inserted "
				"here. Once \"" + active + "\" has a chunk recorded against it, authoring geometry "
				"for it directly is allowed and is never refused again.";
			return RefuseForPhase_( verb, body, outGiveUpNotice );
		}

		namespace
		{
			//! S2: does `chunkItem` carry a parameter named `pname` at all?
			//! ChunkParamString_ cannot answer this (an absent param and a
			//! present-but-empty one both read as ""), and place_element's
			//! matrix / quaternion precedence checks need PRESENCE.
			bool ChunkHasParam_( const RISE::Cst::NodeRef& chunkItem, const std::string& pname )
			{
				if( !chunkItem ) return false;
				for( const RISE::Cst::NodeRef& kid : chunkItem->kids ) {
					if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
					for( const RISE::Cst::NodeRef& tk : kid->kids ) {
						if( tk && tk->kind == RISE::Cst::NodeKind::Token &&
						    tk->role == "pname" && tk->text == pname ) return true;
					}
				}
				return false;
			}

			//! S2: how many `part` lines does this chunk text carry?  The
			//! arc's headline measurement, counted from the CST rather than by
			//! substring so a `part` inside a comment or a value cannot
			//! inflate it.
			unsigned int CountSdfPartLines_( const std::string& chunkText )
			{
				unsigned int n = 0;
				const RISE::Cst::Document doc = RISE::Cst::ParseToCst( chunkText );
				const int items = RISE::Cst::DocItemCount( doc );
				for( int i = 0; i < items; ++i ) {
					const RISE::Cst::NodeRef it =
						RISE::Cst::DocResolveNodeId( doc, RISE::Cst::DocNodeIdAt( doc, i ) );
					if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
					for( const RISE::Cst::NodeRef& kid : it->kids ) {
						if( !kid || kid->kind != RISE::Cst::NodeKind::Param ) continue;
						for( const RISE::Cst::NodeRef& tk : kid->kids ) {
							if( tk && tk->kind == RISE::Cst::NodeKind::Token &&
							    tk->role == "pname" && tk->text == "part" ) ++n;
						}
					}
				}
				return n;
			}

			//! S2: parse "x y z" into three finite doubles.  Returns false on
			//! anything else -- a wrong-arity or non-finite triple is a schema
			//! defect the caller reports, never a silently-substituted zero.
			bool ParseVec3_( const std::string& s, double out[3] )
			{
				double v[3] = { 0.0, 0.0, 0.0 };
				char extra[8] = { 0 };
				// The 4th conversion is the TRAILING-GARBAGE probe: it must
				// NOT match, so "1 2 3 4" and "1 2 3 oops" are both rejected
				// rather than silently read as "1 2 3".
				const int n = std::sscanf( s.c_str(), "%lf %lf %lf %7s", &v[0], &v[1], &v[2], extra );
				if( n != 3 ) return false;
				for( int i = 0; i < 3; ++i )
					if( !RISE::IsFiniteDouble( v[i] ) ) return false;
				out[0] = v[0]; out[1] = v[1]; out[2] = v[2];
				return true;
			}

			//! S2: format a double for a scene-text parameter value.  %.6g is
			//! the same shortest-round-trippable-enough form the scaffold chunk
			//! writers use; -0 is normalized to 0 so a placement at the origin
			//! reads as one.
			std::string FormatScalar_( double v )
			{
				if( v == 0.0 ) v = 0.0;
				char buf[48];
				std::snprintf( buf, sizeof( buf ), "%.6g", v );
				return buf;
			}

			std::string FormatVec3_( const double v[3] )
			{
				return FormatScalar_( v[0] ) + " " + FormatScalar_( v[1] ) + " " + FormatScalar_( v[2] );
			}

			//! S2: rotate `p` by the Euler triple `deg` using the SAME
			//! composition Transformable::SetOrientation applies --
			//! XRotation(x) * YRotation(y) * ZRotation(z) in RISE's row-vector
			//! convention, i.e. the point is rotated about X first, then Y,
			//! then Z.  Written out rather than reusing Matrix4Ops so this file
			//! does not have to agree with that header's storage order as well
			//! as its composition order; the two rotations a placement can
			//! involve are the only rotations here.
			void RotateEulerDeg_( const double deg[3], const double p[3], double out[3] )
			{
				const double d2r = 3.14159265358979323846 / 180.0;
				const double cx = std::cos( deg[0]*d2r ), sx = std::sin( deg[0]*d2r );
				const double cy = std::cos( deg[1]*d2r ), sy = std::sin( deg[1]*d2r );
				const double cz = std::cos( deg[2]*d2r ), sz = std::sin( deg[2]*d2r );
				// X, then Y, then Z.
				double x = p[0], y = p[1], z = p[2];
				double ny =  cx*y - sx*z;
				double nz =  sx*y + cx*z;
				y = ny; z = nz;
				double nx =  cy*x + sy*z;
				nz        = -sy*x + cy*z;
				x = nx; z = nz;
				nx =  cz*x - sz*y;
				ny =  sz*x + cz*y;
				out[0] = nx; out[1] = ny; out[2] = z;
			}
		}

		bool AgentSession::ElementWorldBounds_( const std::string& element,
		                                         double outMin[3], double outMax[3] ) const
		{
			bool any = false;
			double lo[3] = { 0.0, 0.0, 0.0 };
			double hi[3] = { 0.0, 0.0, 0.0 };
			const std::vector<std::string> chunks = ElementChunks( element );
			for( std::size_t i = 0; i < chunks.size(); ++i ) {
				const ChunkAttribution_* a = FindChunkAttribution_( chunks[i] );
				if( !a ) continue;
				const ChunkDescriptor* d = DescriptorForKeyword( String( a->kind.c_str() ) );
				if( !d || d->category != ChunkCategory::Object ) continue;
				IObjectManager* objMgr = mJob ? mJob->GetObjects() : nullptr;
				IObjectPriv* obj = nullptr;
				std::string ignored;
				if( !ResolveIsolateObject( objMgr, chunks[i], obj, ignored ) || !obj ) continue;
				const BoundingBox bb = static_cast<const IObject*>( obj )->getBoundingBox();
				const double bmin[3] = { bb.ll.x, bb.ll.y, bb.ll.z };
				const double bmax[3] = { bb.ur.x, bb.ur.y, bb.ur.z };
				bool finite = true;
				for( int k = 0; k < 3; ++k )
					if( !RISE::IsFiniteDouble( bmin[k] ) || !RISE::IsFiniteDouble( bmax[k] ) ) finite = false;
				if( !finite ) continue;
				for( int k = 0; k < 3; ++k ) {
					if( !any ) { lo[k] = bmin[k]; hi[k] = bmax[k]; }
					else {
						if( bmin[k] < lo[k] ) lo[k] = bmin[k];
						if( bmax[k] > hi[k] ) hi[k] = bmax[k];
					}
				}
				any = true;
			}
			if( !any ) return false;
			for( int k = 0; k < 3; ++k ) { outMin[k] = lo[k]; outMax[k] = hi[k]; }
			return true;
		}

		namespace
		{
			//! S2: the chunk kinds whose descriptor-generated schema is sent to
			//! the builder.  DELIBERATELY SHORT.  The measurement this whole
			//! mechanism exploits is that context VOLUME collapses construction
			//! richness (18.0 SDF parts at short context, 7.7 with 60k of
			//! skills prepended), so a builder prompt that grew to a full
			//! grammar dump would destroy the very effect it exists to capture.
			//! These five cover the contract's requirements: a rich implicit
			//! geometry, a simple explicit one, two materials at two levels of
			//! detail, a painter, and the standard_object every element must
			//! finish with.
			//!
			//! THE TEXT IS THE DESCRIPTOR REGISTRY'S OWN, fetched through the
			//! same ReadSchema the `read_schema` tool answers with -- there is
			//! no second, hand-written grammar description in this file that
			//! could drift from the parser.
			const char* const kBuilderGrammarKeywords[] = {
				"sdf_geometry",
				"box_geometry",
				"uniformcolor_painter",
				"lambertian_material",
				"pbr_metallic_roughness_material",
				"standard_object"
			};
			const std::size_t kBuilderGrammarKeywordCount =
				sizeof( kBuilderGrammarKeywords ) / sizeof( kBuilderGrammarKeywords[0] );
		}

		std::string AgentSession::ComposeBuilderPrompt_( const std::string& element,
		                                                  double height,
		                                                  const std::string& notes,
		                                                  const std::string& rejectionText ) const
		{
			// THE WHOLE PROMPT IS COMPOSED HERE, HOST-SIDE.  The model supplies
			// the element name, the height and the notes; every other span --
			// the grammar, the pieces, the outline, the frame contract, the
			// output instruction -- is this function's own text or the
			// descriptor registry's.  There is no parameter through which a
			// caller can hand raw prompt text to the provider.
			const std::string prefix = ElementChunkNamePrefix( element );

			std::string p;
			p += "You are writing RISE scene-language chunks that build ONE element of a 3D scene: \"";
			p += element;
			p += "\".\n\n";

			// The element's declared decomposition and its filed outline, both
			// from the arc-78 build plan -- the model's OWN earlier statements
			// about this element, restated to a context that has never seen
			// them.
			const AgentBuildPlanEntry* entry = nullptr;
			for( std::size_t i = 0; i < mBuildPlan.size(); ++i )
				if( mBuildPlan[i].element == element ) { entry = &mBuildPlan[i]; break; }
			if( entry ) {
				if( !entry->pieces.empty() ) {
					p += "PIECES it was broken into (build all of them; they are a checklist, not a "
					     "chunk-per-piece requirement):\n";
					for( std::size_t i = 0; i < entry->pieces.size(); ++i ) {
						p += "  - ";
						p += entry->pieces[i];
						p += "\n";
					}
					p += "\n";
				}
				if( !entry->construction.empty() ) {
					p += "DECLARED CONSTRUCTION METHOD: " + entry->construction + "\n\n";
				}
				if( !entry->outline.empty() ) {
					p += "OUTLINE SKETCH of the element's silhouette, as \"x y\" points (";
					p += entry->view.empty() ? std::string( kBuildPlanDefaultView ) : entry->view;
					p += " view; units are arbitrary and the shape is what matters, not the scale):\n";
					p += entry->outline;
					p += "\n\n";
				}
			}

			{
				char hb[96];
				std::snprintf( hb, sizeof( hb ), "%.6g", height );
				p += "REQUESTED HEIGHT: ";
				p += hb;
				p += " world units in Y.\n";
			}
			p += "REQUIRED CHUNK-NAME PREFIX: ";
			p += prefix;
			p += "\n\n";

			p += LocalFrameContract();
			p += "\n\n";

			if( !notes.empty() ) {
				// THE ONE MODEL-SUPPLIED SPAN, clearly labelled as such so the
				// builder reads it as a note from the caller rather than as
				// part of the contract above it.  It is length-capped before
				// it gets here and is JSON-escaped by the request builder, so
				// it cannot reach the endpoint, the headers or the key.
				p += "NOTES FROM THE CALLER:\n";
				p += notes;
				p += "\n\n";
			}

			p += "THE CHUNK GRAMMAR you may use. This is the scene language's own parameter "
			     "reference for the chunk kinds relevant here; the parser is authoritative.\n";
			for( std::size_t i = 0; i < kBuilderGrammarKeywordCount; ++i ) {
				p += "\n";
				p += ReadSchema( kBuilderGrammarKeywords[i] );
				p += "\n";
			}

			p += "\nWRITE YOUR ANSWER AS SCENE TEXT ONLY -- a sequence of complete chunks, each in "
			     "the form\n"
			     "keyword\n"
			     "{\n"
			     "\tparameter value\n"
			     "}\n"
			     "with the braces on their own lines. A chunk that another chunk references must "
			     "come first. No prose, no explanation, no markdown fences, no scene header.\n";

			if( !rejectionText.empty() ) {
				// THE ONE REPAIR RETRY.  The rejection text is this harness's
				// own, verbatim, so the builder is corrected by facts about
				// what happened rather than by a paraphrase of them.
				p += "\nA PREVIOUS ANSWER TO THIS SAME REQUEST WAS PARTLY REJECTED:\n";
				p += rejectionText;
				p += "\nReturn the CORRECTED SET WHOLE -- every chunk this element needs, including "
				     "the ones that were accepted, in one answer.\n";
			}
			return p;
		}

		namespace
		{
			//! Fix 3 (2026-08-11, live-run defect): a short, whole-line excerpt
			//! of `text` for the build_element TOTAL-rejection report -- cut at
			//! `capChars`, backed up to the last newline at or before the cut
			//! so no line is sliced mid-way, with an explicit truncation
			//! marker whenever it WAS cut.  Never silent, matching this file's
			//! own truncation convention (see `notesTruncated` in BuildElement
			//! below).  Before this, a total rejection (every chunk the
			//! builder wrote refused) retained the builder's actual answer
			//! nowhere, so the failure was undiagnosable after the fact.
			std::string ExcerptWholeLines_( const std::string& text, std::size_t capChars )
			{
				if( text.size() <= capChars ) return text;
				std::size_t cut = text.rfind( '\n', capChars );
				if( cut == std::string::npos || cut == 0 ) cut = capChars;
				std::string ex = text.substr( 0, cut );
				while( !ex.empty() && ( ex.back() == '\n' || ex.back() == '\r' ) ) ex.pop_back();
				ex += "\n[...truncated...]";
				return ex;
			}
		}

		AgentSession::AgentBuildElementResult AgentSession::BuildElement(
			const std::string& element, double height, const std::string& notes )
		{
			AgentBuildElementResult out;
			out.providerName = mTextCompleter.providerName;
			out.modelId      = mTextCompleter.modelId;
			// G2 fix-round (2026-08-11): build_element's OWN give-up fold --
			// same destructor-time appender InsertChunk/ProposePatch declare
			// up front, bound to `out.message` so a give-up notice reaches
			// the caller regardless of which of this function's many
			// `return out;` statements ultimately fires, success included.
			BuildPlanGiveUpFold_ g2Fold{ out.message, std::string() };

			//------------------------------------------------------------------
			// THE DO-NOTHING CASES.  None is a phase refusal: each changes no
			// state, mutates no document, costs no budget and is never counted.
			//------------------------------------------------------------------
			if( element.empty() ) {
				out.message = "build_element: `element` must be a non-empty string. Nothing was built.";
				return out;
			}
			if( !RISE::IsFiniteDouble( height ) || height <= 0.0 ) {
				out.message = "build_element: `height` must be a finite number greater than 0. "
				              "Nothing was built.";
				return out;
			}
			if( !BuildProtocolActive_() ) {
				out.message = "build_element did nothing: the staged build protocol is off for this "
				              "session, so no element window is open. Author the element's chunks "
				              "directly with insert_chunk or insert_chunks.";
				return out;
			}
			if( mBuildPhase != AgentBuildPhase::Pieces ) {
				out.message = std::string( "build_element did nothing: this session is in the " ) +
					BuildPhaseName( mBuildPhase ) + " phase, and build_element builds the ACTIVE "
					"element of the pieces phase. " +
					( mBuildPhase == AgentBuildPhase::Plan
						? std::string( "file_build_plan declares the elements and makes the first one "
						               "active." )
						: std::string( "reopen_element with an element name re-enters that element's "
						               "window." ) );
				return out;
			}
			if( mActiveElement >= mBuildPlan.size() ) {
				out.message = "build_element did nothing: the active element index does not resolve "
				              "against the filed build plan.";
				return out;
			}
			const std::string active = mBuildPlan[ mActiveElement ].element;
			if( element != active ) {
				out.message = "build_element did nothing: \"" + element + "\" is not the active "
					"element -- \"" + active + "\" is. reopen_element with element \"" + element +
					"\" makes it active, if it is in the filed build plan.";
				return out;
			}
			if( !BuildCapable() ) {
				out.capabilityRefusal = true;
				const std::string who = mTextCompleter.providerName.empty()
					? std::string( "this session's provider" )
					: ( "`" + mTextCompleter.providerName + "`" );
				out.message = "build_element is not available: " + who + " does not run a separate "
					"builder completion through this build, so there is no fresh context to "
					"construct the element in. Nothing was built and nothing else about this session "
					"changes -- authoring the element's chunks directly with insert_chunk or "
					"insert_chunks is not blocked by this.";
				return out;
			}

			// G2 fix-round (2026-08-11): build_element's OWN build-plan-gate
			// arm, consulted HERE -- after every do-nothing prologue check
			// above (so those keep priority and their messages are
			// unchanged) and BEFORE the per-session cap below and the
			// provider completion further down -- rather than left to
			// InsertChunks's arm inside the validated-insert this function
			// calls once the completion comes back.  Without this, a session
			// with no imagined scene target paid for a full builder
			// completion only to have InsertChunks refuse every chunk it
			// produced: the gate exists to stop geometry-creating work
			// before it is *spent*, and by the time InsertChunks sees this
			// function's chunks the completion is already sunk cost.  This
			// is the exact same shared gate InsertChunks's own arm calls (see
			// its call site above, verb "insert_chunks") -- build_element
			// only ever runs in the PIECES phase, so `needPlan` is already
			// satisfied in practice and `needImagine` is the condition that
			// actually fires, but that is a fact about session state, not
			// something this call special-cases: calling the shared gate
			// unconditionally is what keeps the two paths from ever drifting
			// apart on what "satisfied" means.
			{
				const std::string g2Clause = CheckBuildPlanGate_( "build_element", &g2Fold.notice );
				if( !g2Clause.empty() ) {
					out.message = g2Clause;
					return out;
				}
				// g2Clause.empty() here means either "not armed / already
				// resolved" OR "this call is the give-up" -- g2Fold.notice
				// was written in the latter case and stays empty in the
				// former; either way, normal build_element processing
				// continues below and g2Fold's destructor folds any notice
				// into out.message whichever return statement fires.
			}

			if( mBuildElementCalls >= kBuildElementMaxPerSession ) {
				char capBuf[224];
				std::snprintf( capBuf, sizeof( capBuf ),
					"build_element has already run %d builder completions this session -- the "
					"per-session cap. Nothing was built; the document is unchanged.",
					kBuildElementMaxPerSession );
				out.message = capBuf;
				return out;
			}
			++mBuildElementCalls;

			out.element = element;

			// The caller's notes, length-capped BEFORE composition and with the
			// truncation stated rather than silent.
			std::string useNotes = notes;
			bool notesTruncated = false;
			if( useNotes.size() > kBuildElementMaxNotes ) {
				useNotes.resize( kBuildElementMaxNotes );
				notesTruncated = true;
			}

			const std::string basePrompt = ComposeBuilderPrompt_( element, height, useNotes,
			                                                      std::string() );
			const std::string prefix = ElementChunkNamePrefix( element );

			//------------------------------------------------------------------
			// ONE ATTEMPT = one completion, extract, prefix-check, insert.
			// Everything the attempt learned rides back in `attemptRejections`,
			// which is also the exact text the repair retry is given.
			//------------------------------------------------------------------
			std::vector<std::string> landedNames;
			std::vector<std::string> rejectionLines;
			std::string providerFailure;
			// Fix 1 (2026-08-11, live-run defect): chunk names the builder
			// quoted (`name "foo"` instead of `name foo` -- this scene
			// language has no quoted-string syntax, so the quotes come back
			// as literal characters in the extracted name) but that are
			// otherwise fine, across both attempts.  Disclosed to the caller
			// as a statement of what this harness did, never as advice.
			std::vector<std::string> unquotedNames;
			// Fix 3: the most recent completion's raw text, kept so a
			// TOTAL rejection (every chunk the builder wrote was refused)
			// can show an excerpt of what it actually returned -- today
			// that text is retained nowhere once extraction/validation
			// finishes, so a total rejection is undiagnosable after the
			// fact.
			std::string lastCompletionText;

			const auto runAttempt = [&]( const std::string& prompt ) -> bool
			{
				const AgentTextCompletionOutcome comp = mTextCompleter.complete( prompt );
				if( !comp.ok || comp.text.empty() ) {
					providerFailure = comp.error.empty()
						? std::string( "the provider returned no text and no reason" )
						: comp.error;
					return false;
				}
				lastCompletionText = comp.text;

				std::vector<std::string> chunks, problems;
				ExtractChunkTexts( comp.text, chunks, problems );
				out.chunksExtracted += static_cast<unsigned int>( chunks.size() );
				for( std::size_t i = 0; i < problems.size(); ++i ) {
					AgentBuildElementRejection r;
					r.reason = problems[i];
					out.rejected.push_back( r );
					rejectionLines.push_back( problems[i] );
				}
				if( chunks.empty() ) {
					if( problems.empty() ) {
						const std::string why =
							"the answer contained no complete chunk (no `keyword { ... }` block)";
						AgentBuildElementRejection r;
						r.reason = why;
						out.rejected.push_back( r );
						rejectionLines.push_back( why );
					}
					return false;
				}

				// PREFIX CHECK BEFORE INSERTION, and never a rename: renaming
				// would break the references the builder just wrote between its
				// own chunks.
				std::vector<std::string> submit;
				for( std::size_t i = 0; i < chunks.size(); ++i ) {
					std::string kind, name;
					RISE::Cst::Document cdoc = RISE::Cst::ParseToCst( chunks[i] );
					RISE::Cst::NodeId chunkNodeId = 0;
					{
						const int n = RISE::Cst::DocItemCount( cdoc );
						for( int c = 0; c < n; ++c ) {
							const RISE::Cst::NodeId nid = RISE::Cst::DocNodeIdAt( cdoc, c );
							const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId( cdoc, nid );
							if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
							kind = it->role;
							name = ChunkParamString_( it, "name" );
							chunkNodeId = nid;
							break;
						}
					}
					if( name.empty() ) {
						AgentBuildElementRejection r;
						r.kind   = kind;
						r.reason = "a " + ( kind.empty() ? std::string( "chunk" ) : ( "`" + kind + "`" ) ) +
							" carried no `name`, so it cannot be recorded against the element; every "
							"chunk must be named and every name must begin \"" + prefix + "\"";
						out.rejected.push_back( r );
						rejectionLines.push_back( r.reason );
						continue;
					}
					// Fix 1 (2026-08-11, live-run defect): a QUOTED name --
					// `name "coral_reef_rock_painter"` -- is tolerated, not
					// rejected.  ChunkParamString_ returns the raw token
					// text, so a quoted value comes back WITH the quote
					// characters, and comparing that against a bare `prefix`
					// always failed even when the name legitimately began
					// with it (the live-run rejection message then LIED:
					// it claimed the name did not begin with the prefix
					// when it did, just wrapped in quotes).  This is
					// deliberately NOT the rename the comment below forbids:
					// the builder's OWN references to this same chunk,
					// written elsewhere in this same answer, are bare
					// tokens -- this scene language has no quoted-string
					// syntax at all -- so stripping the quotes here makes
					// the DEFINITION match what the builder already wrote,
					// which is the opposite of breaking those references.
					if( name.size() >= 2 && name.front() == '"' && name.back() == '"' ) {
						const std::string stripped = name.substr( 1, name.size() - 2 );
						cdoc = RISE::Cst::DocSetParamValue( cdoc, chunkNodeId, "name", 0, stripped );
						chunks[i] = RISE::Cst::SerializeCst( cdoc );
						unquotedNames.push_back( stripped );
						name = stripped;
					}
					if( name.compare( 0, prefix.size(), prefix ) != 0 ) {
						AgentBuildElementRejection r;
						r.name   = name;
						r.kind   = kind;
						r.reason = "the chunk named \"" + name + "\" does not begin with the required "
							"prefix \"" + prefix + "\", so it was not inserted (it was NOT renamed -- "
							"renaming would break the references between the chunks)";
						out.rejected.push_back( r );
						rejectionLines.push_back( r.reason );
						continue;
					}
					// A name that already landed in the FIRST attempt is not
					// re-submitted: the repair retry is asked for the corrected
					// set WHOLE, so it legitimately repeats what worked, and
					// re-inserting it would only draw a duplicate-name refusal.
					bool already = false;
					for( std::size_t l = 0; l < landedNames.size() && !already; ++l )
						already = ( landedNames[l] == name );
					if( already ) continue;
					submit.push_back( chunks[i] );
				}
				if( submit.empty() ) return false;

				std::vector<AgentChunkResult> results;
				{
					// The clean room's own insertion must not be refused by the
					// clean-room gate it arms (AgentSession.h's guard doc).
					BuildElementInsertGuard_ guard( *this );
					results = InsertChunks( submit );
				}
				bool landedAny = false;
				for( std::size_t i = 0; i < results.size(); ++i ) {
					out.chunkResults.push_back( results[i] );
					if( results[i].applied ) {
						landedNames.push_back( results[i].name );
						out.landed.push_back( results[i].name );
						out.sdfPartCount += CountSdfPartLines_( submit[i] );
						landedAny = true;
					}
					else {
						AgentBuildElementRejection r;
						r.name   = results[i].name;
						r.kind   = results[i].kind;
						r.reason = results[i].message.empty()
							? std::string( "the insertion was rejected with no reason given" )
							: results[i].message;
						out.rejected.push_back( r );
						rejectionLines.push_back(
							( r.name.empty() ? std::string( "a chunk" ) : ( "the chunk \"" + r.name + "\"" ) ) +
							" was rejected: " + r.reason );
					}
				}
				return landedAny;
			};

			runAttempt( basePrompt );

			// THE ONE REPAIR RETRY.  It fires when the first attempt rejected
			// ANYTHING -- an unbalanced chunk, a prefix violation, an insert
			// refusal -- or when the provider itself failed.  Exactly one, then
			// stop, whatever the outcome.
			if( !rejectionLines.empty() || !providerFailure.empty() ) {
				std::string rejectionText;
				if( !providerFailure.empty() )
					rejectionText += "- the previous attempt did not complete: " + providerFailure + "\n";
				for( std::size_t i = 0; i < rejectionLines.size(); ++i )
					rejectionText += "- " + rejectionLines[i] + "\n";

				const std::size_t landedBefore = landedNames.size();
				const std::size_t rejectedBefore = out.rejected.size();
				out.retryRan = true;
				providerFailure.clear();
				rejectionLines.clear();
				runAttempt( ComposeBuilderPrompt_( element, height, useNotes, rejectionText ) );
				out.retrySucceeded = ( landedNames.size() > landedBefore );
				(void)rejectedBefore;
			}

			// A PURE PROVIDER FAILURE (nothing landed AND nothing was rejected,
			// because no answer was ever parsed) is NOT an ok result: `ok`
			// means the builder answered and its answer was processed.
			// Reported as its own outcome rather than as an empty success, and
			// -- unlike imagine_scene's provider failure -- it disarms nothing,
			// because the clean-room refusal is already conditional on there
			// being a completer at all and the 3-refusal give-up bounds it.
			if( out.landed.empty() && out.rejected.empty() ) {
				out.message = "build_element did not complete: " +
					( providerFailure.empty()
						? std::string( "the builder returned nothing this harness could read as a chunk" )
						: providerFailure ) +
					". Nothing was inserted and the document is unchanged" +
					( out.retryRan ? std::string( "; the one repair retry ran and did not complete "
					                              "either, and there is no second retry." )
					               : std::string( "." ) );
				return out;
			}

			out.ok = true;
			out.bboxValid = ElementWorldBounds_( element, out.bboxMin, out.bboxMax );

			//------------------------------------------------------------------
			// THE REPORT: facts only.  What landed, what did not and why,
			// whether the retry ran, the realised box and the part count.  No
			// characterization of the element, no advice about what to do next,
			// no score.
			//------------------------------------------------------------------
			std::string m = "build_element ran one builder completion for \"" + element + "\" on " +
				mTextCompleter.providerName + "/" + mTextCompleter.modelId + ". Chunks inserted: ";
			if( out.landed.empty() ) m += "none";
			else {
				for( std::size_t i = 0; i < out.landed.size(); ++i ) {
					if( i ) m += ", ";
					m += out.landed[i];
				}
			}
			m += ".";
			if( !out.rejected.empty() ) {
				m += " Not inserted: ";
				for( std::size_t i = 0; i < out.rejected.size(); ++i ) {
					if( i ) m += "; ";
					m += out.rejected[i].reason;
				}
				m += ".";
			}
			if( out.retryRan ) {
				m += out.retrySucceeded
					? std::string( " One repair retry ran and inserted more chunks; there is no second "
					               "retry." )
					: std::string( " One repair retry ran and inserted nothing further; there is no "
					               "second retry." );
			}
			if( !providerFailure.empty() )
				m += " The last builder completion did not complete: " + providerFailure + ".";
			if( !unquotedNames.empty() ) {
				// Fix 1's disclosure: a statement of what this harness did,
				// never advice.
				m += " This harness stripped a wrapping pair of double quotes from the `name` value "
				     "of ";
				for( std::size_t i = 0; i < unquotedNames.size(); ++i ) {
					if( i ) m += ", ";
					m += unquotedNames[i];
				}
				m += " before checking the prefix and inserting.";
			}
			if( out.landed.empty() && !lastCompletionText.empty() ) {
				// Fix 3: TOTAL rejection ONLY -- out.landed.empty() here means
				// every chunk across both attempts was rejected (the pure
				// provider-failure case already returned above), so this is
				// reached exactly on a total rejection and never on a partial
				// or full success.
				m += " Nothing landed for this element; the builder's last answer, before this "
				     "harness's own truncation, began:\n";
				m += ExcerptWholeLines_( lastCompletionText, 400 );
			}
			{
				char pb[128];
				std::snprintf( pb, sizeof( pb ), " SDF part lines across the inserted geometry: %u.",
					out.sdfPartCount );
				m += pb;
			}
			if( out.bboxValid ) {
				m += " Realised world bounding box of the objects recorded against \"" + element +
					"\": (" + FormatScalar_( out.bboxMin[0] ) + ", " + FormatScalar_( out.bboxMin[1] ) +
					", " + FormatScalar_( out.bboxMin[2] ) + ") to (" + FormatScalar_( out.bboxMax[0] ) +
					", " + FormatScalar_( out.bboxMax[1] ) + ", " + FormatScalar_( out.bboxMax[2] ) +
					"), so " + FormatScalar_( out.bboxMax[1] - out.bboxMin[1] ) +
					" units tall against the " + FormatScalar_( height ) + " requested.";
			}
			else {
				m += " No object recorded against \"" + element + "\" resolves in the scene, so there "
				     "is no bounding box to report.";
			}
			if( notesTruncated ) {
				char nb[144];
				std::snprintf( nb, sizeof( nb ),
					" The `notes` string was truncated to the first %u characters before it was sent.",
					static_cast<unsigned int>( kBuildElementMaxNotes ) );
				m += nb;
			}
			m += " place_element moves everything recorded against this element into the scene as one "
			     "rigid transform.";
			out.message = m;
			return out;
		}

		AgentSession::AgentPlaceElementResult AgentSession::PlaceElement(
			const std::string& element, const std::string& position,
			const std::string& scale, const std::string& orientation )
		{
			AgentPlaceElementResult out;
			out.element = element;

			if( !BuildProtocolActive_() ) {
				out.message = "place_element did nothing: the staged build protocol is off for this "
				              "session, so no chunk is recorded against an element. Set each object's "
				              "position with propose_patch instead.";
				return out;
			}
			if( mBuildPlan.empty() ) {
				out.message = "place_element did nothing: no build plan has been filed in this "
				              "session, so there is no element to place.";
				return out;
			}
			bool known = false;
			for( std::size_t i = 0; i < mBuildPlan.size() && !known; ++i )
				known = ( mBuildPlan[i].element == element );
			if( !known ) {
				out.message = "place_element did nothing: \"" + element + "\" is not in the filed "
					"build plan, which lists: " + ElementSketchNameList() + ".";
				return out;
			}

			double pos[3] = { 0.0, 0.0, 0.0 };
			if( !ParseVec3_( position, pos ) ) {
				out.message = "place_element did nothing: `position` must be three finite numbers, "
				              "\"x y z\" -- the element's new base-centre in world space.";
				return out;
			}
			double s = 1.0;
			if( !scale.empty() ) {
				char sextra[8] = { 0 };
				const int sn = std::sscanf( scale.c_str(), "%lf %7s", &s, sextra );
				if( sn != 1 ) {
					out.message = "place_element did nothing: `scale` must be ONE number (a uniform "
					              "factor), not three.";
					return out;
				}
				if( !RISE::IsFiniteDouble( s ) || s <= 0.0 ) {
					out.message = "place_element did nothing: `scale` must be one finite number "
					              "greater than 0 -- a uniform factor.";
					return out;
				}
			}
			double rot[3] = { 0.0, 0.0, 0.0 };
			if( !orientation.empty() && !ParseVec3_( orientation, rot ) ) {
				out.message = "place_element did nothing: `orientation` must be three finite numbers, "
				              "\"ex ey ez\" in degrees.";
				return out;
			}
			const bool rotating = ( rot[0] != 0.0 || rot[1] != 0.0 || rot[2] != 0.0 );

			// Read the objects' CURRENT params from the document, so the
			// composition below is against what is actually authored rather
			// than against a derived transform this verb cannot write back.
			const AgentDocumentSnapshot snap = ReadDocumentSnapshot();
			if( !snap.hasDocument ) {
				out.message = "place_element did nothing: this session has no scene document.";
				return out;
			}

			// ONE parse of the snapshot's bytes, reused for every object --
			// AgentDocumentSnapshot carries the document TEXT, not a CST.
			const RISE::Cst::Document liveDoc = RISE::Cst::ParseToCst( snap.document );

			std::vector<AgentSetPatch> patches;
			std::vector<std::string> approxObjects;
			const std::vector<std::string> chunks = ElementChunks( element );
			for( std::size_t c = 0; c < chunks.size(); ++c ) {
				const ChunkAttribution_* a = FindChunkAttribution_( chunks[c] );
				if( !a || a->kind != "standard_object" ) continue;

				RISE::Cst::NodeRef objItem;
				{
					const int n = RISE::Cst::DocItemCount( liveDoc );
					for( int i = 0; i < n; ++i ) {
						const RISE::Cst::NodeRef it = RISE::Cst::DocResolveNodeId(
							liveDoc, RISE::Cst::DocNodeIdAt( liveDoc, i ) );
						if( !it || it->kind != RISE::Cst::NodeKind::Chunk ) continue;
						if( it->role != "standard_object" ) continue;
						if( ChunkParamString_( it, "name" ) != chunks[c] ) continue;
						objItem = it;
						break;
					}
				}
				if( !objItem ) {
					AgentPlaceElementSkip sk;
					sk.object = chunks[c];
					sk.reason = "this standard_object is no longer in the document";
					out.skipped.push_back( sk );
					continue;
				}
				if( ChunkHasParam_( objItem, "matrix" ) ) {
					// A `matrix` bypasses position / orientation / scale
					// entirely, so patching them would be a silent no-op --
					// exactly the failure mode this arc exists to remove.
					AgentPlaceElementSkip sk;
					sk.object = chunks[c];
					sk.reason = "this standard_object is authored with `matrix`, which bypasses "
					            "position, orientation and scale, so a placement written into those "
					            "parameters would have no effect";
					out.skipped.push_back( sk );
					continue;
				}
				const bool hasQuat = ChunkHasParam_( objItem, "quaternion" );

				double oldPos[3] = { 0.0, 0.0, 0.0 };
				double oldScale[3] = { 1.0, 1.0, 1.0 };
				double oldRot[3] = { 0.0, 0.0, 0.0 };
				const std::string posStr = ChunkParamString_( objItem, "position" );
				const std::string sclStr = ChunkParamString_( objItem, "scale" );
				const std::string rotStr = ChunkParamString_( objItem, "orientation" );
				if( !posStr.empty() && !ParseVec3_( posStr, oldPos ) ) {
					AgentPlaceElementSkip sk;
					sk.object = chunks[c];
					sk.reason = "this standard_object's `position` is not three finite numbers, so "
					            "there is nothing to compose the placement with";
					out.skipped.push_back( sk );
					continue;
				}
				if( !sclStr.empty() && !ParseVec3_( sclStr, oldScale ) ) {
					oldScale[0] = oldScale[1] = oldScale[2] = 1.0;
				}
				const bool hadRot = ( !rotStr.empty() && ParseVec3_( rotStr, oldRot ) &&
				                      ( oldRot[0] != 0.0 || oldRot[1] != 0.0 || oldRot[2] != 0.0 ) );

				// SCALE about the element's own origin, then ROTATE about it,
				// then OFFSET -- so the object's own place inside the element
				// is preserved and carried rigidly.
				double p[3] = { oldPos[0] * s, oldPos[1] * s, oldPos[2] * s };
				if( rotating ) {
					double r[3];
					RotateEulerDeg_( rot, p, r );
					p[0] = r[0]; p[1] = r[1]; p[2] = r[2];
				}
				p[0] += pos[0]; p[1] += pos[1]; p[2] += pos[2];

				AgentSetPatch pp;
				pp.target = chunks[c];
				pp.kind   = "standard_object";
				pp.param  = "position";
				pp.value  = FormatVec3_( p );
				patches.push_back( pp );

				if( s != 1.0 || !sclStr.empty() ) {
					const double ns[3] = { oldScale[0] * s, oldScale[1] * s, oldScale[2] * s };
					AgentSetPatch sp;
					sp.target = chunks[c];
					sp.kind   = "standard_object";
					sp.param  = "scale";
					sp.value  = FormatVec3_( ns );
					patches.push_back( sp );
				}

				if( rotating ) {
					if( hasQuat ) {
						AgentPlaceElementSkip sk;
						sk.object = chunks[c];
						sk.reason = "this standard_object is authored with `quaternion`, which takes "
						            "precedence over `orientation`, so it was moved and scaled but "
						            "not rotated";
						out.skipped.push_back( sk );
					}
					else {
						const double nr[3] = { oldRot[0] + rot[0], oldRot[1] + rot[1],
						                       oldRot[2] + rot[2] };
						AgentSetPatch rp;
						rp.target = chunks[c];
						rp.kind   = "standard_object";
						rp.param  = "orientation";
						rp.value  = FormatVec3_( nr );
						patches.push_back( rp );
						if( hadRot ) approxObjects.push_back( chunks[c] );
					}
				}

				out.objects.push_back( chunks[c] );
			}

			if( patches.empty() ) {
				out.message = "place_element did nothing: no standard_object recorded against \"" +
					element + "\" could be placed";
				if( !out.skipped.empty() ) {
					out.message += " (";
					for( std::size_t i = 0; i < out.skipped.size(); ++i ) {
						if( i ) out.message += "; ";
						out.message += out.skipped[i].object + ": " + out.skipped[i].reason;
					}
					out.message += ")";
				}
				else {
					out.message += " -- build_element creates the element's objects, and every chunk "
					               "created while an element is active is recorded against it";
				}
				out.message += ". The document is unchanged.";
				return out;
			}

			// ONE batch, so ONE head bump and ONE undo step for the whole
			// placement.  ProposePatches is sequential and best-effort inside
			// the batch, which is right here: a rejected object does not make
			// the others' placement wrong.
			out.patchResults = ProposePatches( patches );
			for( std::size_t i = 0; i < out.patchResults.size(); ++i ) {
				if( out.patchResults[i].applied ) ++out.patchesApplied;
				else                              ++out.patchesRejected;
			}
			out.ok = ( out.patchesApplied > 0 );
			out.bboxValid = ElementWorldBounds_( element, out.bboxMin, out.bboxMax );

			std::string m = "place_element applied one rigid transform to " +
				std::to_string( out.objects.size() ) +
				( out.objects.size() == 1 ? " object" : " objects" ) + " recorded against \"" +
				element + "\": base-centre moved to (" + FormatVec3_( pos ) + ")";
			if( s != 1.0 ) m += ", scaled by " + FormatScalar_( s ) + " about the element's origin";
			if( rotating ) m += ", rotated (" + FormatVec3_( rot ) + ") degrees about it";
			m += ". Each object's own offset inside the element was scaled and rotated with the "
			     "element and then added to the new base-centre, so their relative arrangement is "
			     "unchanged. Patches applied: " + std::to_string( out.patchesApplied );
			if( out.patchesRejected > 0 )
				m += ", rejected: " + std::to_string( out.patchesRejected );
			m += ".";
			if( !approxObjects.empty() ) {
				m += " These objects already carried a rotation of their own, and the placement's "
				     "Euler degrees were ADDED per axis, which is exact only when both rotations "
				     "are about the same axis: ";
				for( std::size_t i = 0; i < approxObjects.size(); ++i ) {
					if( i ) m += ", ";
					m += approxObjects[i];
				}
				m += ".";
			}
			if( !out.skipped.empty() ) {
				m += " Not fully placed: ";
				for( std::size_t i = 0; i < out.skipped.size(); ++i ) {
					if( i ) m += "; ";
					m += out.skipped[i].object + " -- " + out.skipped[i].reason;
				}
				m += ".";
			}
			if( out.bboxValid ) {
				m += " The element's world bounding box is now (" + FormatScalar_( out.bboxMin[0] ) +
					", " + FormatScalar_( out.bboxMin[1] ) + ", " + FormatScalar_( out.bboxMin[2] ) +
					") to (" + FormatScalar_( out.bboxMax[0] ) + ", " + FormatScalar_( out.bboxMax[1] ) +
					", " + FormatScalar_( out.bboxMax[2] ) + ").";
			}
			out.message = m;
			return out;
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
			// G3b fix-round (2026-08-10) FIX 1: RESOLVE ONCE, HERE, ON THE
			// CALLER'S THREAD -- the SAME shape the async path uses, so the
			// two entry points consume a resolved COPY rather than a name.
			// Pre-fix, RenderCore_ resolved by name and ApplyTargetComparison_
			// resolved the SAME name a second time; on the async path both of
			// those ran on the controller's render worker while
			// FileBuildPlan could be reassigning mElementSketches on the
			// dispatcher thread (the P1).  Unifying here also deletes the
			// redundant double-resolve and makes the comparison describe the
			// plan AS IT WAS at submission, which is the right semantics for
			// an async call independently of the race.
			//
			// Ordering note: this now runs BEFORE RenderCore_'s `!mJob`
			// check, so a session with no head AND an unresolvable target
			// reports the target failure rather than "no head loaded".  Both
			// are honest, the target check is the cheaper one, and a wrapped
			// session always has a head.
			AgentElementSketch resolvedTarget;
			const AgentElementSketch* resolvedTargetPtr = nullptr;
			if( !params.target.empty() ) {
				std::string targetError;
				if( !ResolveTargetSketch( params.target, params.isolate, resolvedTarget, targetError ) ) {
					AgentRenderResult bad;
					bad.ok      = false;
					bad.message = targetError;
					return bad;
				}
				resolvedTargetPtr = &resolvedTarget;
			}

			// Arc 77 Phase 2 (2026-08-11): the SCENE-TARGET snapshot, taken
			// HERE for the same reason and by the same discipline as the
			// sketch snapshot above -- see ApplySceneTargetComparison_'s doc.
			// A shared_ptr copy, so this costs a refcount bump rather than a
			// copy of the image, and the pointee is immutable so a later
			// re-imagine cannot re-point what this render measured against.
			const std::shared_ptr<const AgentSceneTarget> sceneTargetSnapshot = mSceneTarget;

			AgentRenderResult rr = RenderCore_( params, /*assumeParked=*/false,
			                                    /*forcedJobId=*/0, resolvedTargetPtr );
			// G3b (2026-08-10): the sketch comparison runs AFTER the render
			// returns, on this thread, with the render's park already
			// released (assumeParked=false) -- it fires ONE more internal
			// render of its own and must therefore not be inside the first
			// one's critical section.  A no-op unless `target` was requested
			// AND the render succeeded; see ApplyTargetComparison_'s doc.
			// RenderAsync's worker closure calls the SAME helper with
			// assumeParked=true and the SAME kind of snapshot, so both entry
			// points measure identically.
			ApplyTargetComparison_( params, rr, /*assumeParked=*/false, resolvedTargetPtr );
			// Arc 77 Phase 2: the whole-scene comparison, AFTER the part-sketch
			// one so its own `targetApplied` guard sees the final state (the
			// two are mutually exclusive by construction -- a part comparison
			// requires `isolate`, which this one refuses -- and the ordering
			// makes that exclusivity enforced rather than merely true).  No
			// extra render, so unlike the sketch comparison it does not care
			// whether the park has released.
			ApplySceneTargetComparison_( params, rr, sceneTargetSnapshot );
			return rr;
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
		                                              std::uint64_t forcedJobId,
		                                              const AgentElementSketch* resolvedTarget )
		{
			AgentRenderResult res;

			if( !mJob ) {
				res.ok = false;
				res.message = "no head loaded";
				return res;
			}

			// ---- G3b (2026-08-10) `render{target:}`: the CALLER-RESOLVED
			// sketch, consumed before anything is rendered.
			//
			// G3b fix-round (2026-08-10) FIX 1: this block used to call
			// ResolveTargetSketch itself, i.e. it read mElementSketches -- and
			// on the async path this whole function runs on the controller's
			// render worker thread while the dispatcher thread may be inside
			// FileBuildPlan reassigning that very vector.  Resolution now
			// happens EXACTLY ONCE, on the submitting thread, in Render() /
			// RenderAsync, and the resolved COPY arrives here; the fail-loud
			// refusal (which needs the filed part-name list) moved with it,
			// so a caller sees the identical message from the identical
			// wording, just one frame earlier.  Nothing on this code path
			// touches session build-plan state any more.
			//
			// The resolved sketch's `view` is what selects the render's
			// AXIS-ALIGNED vantage in the isolate block below.  Its mask is
			// NOT read here: the measurement happens in
			// ApplyTargetComparison_, after this render (and its own internal
			// identity pass) have completed -- and that helper is handed the
			// SAME snapshot, so the two cannot disagree.
			//
			// The nested identity pass carries the same `target` precisely so
			// it resolves to the same vantage; ApplyTargetComparison_ passes
			// its own snapshot straight through for that reason.
			const AgentElementSketch* const targetSketch =
				params.target.empty() ? nullptr : resolvedTarget;
			if( !params.target.empty() && !targetSketch ) {
				// A private-caller programming error, not a user-reachable
				// state: every in-class call site resolves first.  Fail
				// loudly rather than rendering without the comparison the
				// caller asked for.
				res.ok = false;
				res.message = "internal error: `target` (\"" + params.target + "\") reached the render "
					"body without a resolved sketch snapshot -- the caller must resolve on its own "
					"thread first (G3b fix-round FIX 1). Nothing was rendered.";
				return res;
			}
			if( targetSketch ) {
				res.targetElement = targetSketch->element;
				res.targetView = targetSketch->view;
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

			// R1b (2026-08-09): a "production BEAUTY" render is exactly the
			// case none of the three fixed-fidelity targets claim -- pure
			// function of `params`, same safety rationale as the flags above.
			const bool isProductionBeauty = !isDraft && !isObjectMap && !isViewMode;
			// R1b: the agent-surface ABSENT-dims default (see
			// AgentRenderParams::fromAgentSurface's doc) applies iff this is a
			// production beauty render, on the agent RPC surface, that did NOT
			// supply an explicit width/height PAIR.  Pure function of params
			// (fromAgentSurface/isProductionBeauty/width/height), safe to
			// compute here; the ACTUAL scaled dims need the live Film's aspect
			// ratio, which is only resolved under the park, inside
			// applyFilmOverride() below.
			const bool wantsAgentDefaultResolutionCap =
				params.fromAgentSurface && isProductionBeauty &&
				!( params.width > 0 && params.height > 0 );

			const bool wantFilmOverride =
				( params.width > 0 && params.height > 0 ) || wantsAgentDefaultResolutionCap;
			// R1b: forcing `wantFilmOverride` true for the absent-dims-default
			// case is DELIBERATE, not incidental -- see the LIVE-mode safety
			// block below (`DoOneRenderPass swaps the SAME shared Film dims...
			// in place`).  A default that resizes the Film genuinely IS a
			// film-dims override now, so routing it through the SAME
			// RunPreviewRenderParked path an explicit width/height override
			// takes (rather than the plain no-override SubmitAgentRenderSync
			// path) is required for the same race-safety reason, not optional.

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

			// G1 (2026-08-10) `render{isolate:}`: set when the auto-framing
			// distance had to assume a field of view because the ACTIVE
			// camera is not a pinhole (only a PinholeCamera reports one) --
			// the framing is then approximate, and the tail says so rather
			// than reporting a clean auto-frame.  Never a render failure.
			bool isolateFovAssumed = false;

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
			// G1 (2026-08-10): an `isolate` request is treated the same
			// CONSERVATIVE way -- absent a caller-supplied camera it always
			// resolves to an auto-framed pose (location/lookat/up), so this
			// is never a false negative; when the caller DID supply one, the
			// camera terms below already fire.
			const bool wantCameraOverrideForRouting =
				!params.view.empty() || !params.isolate.empty() ||
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
			// R1b: the dims applyFilmOverride() actually resolved and applied
			// via SetFilm -- params.width/height ALONE are no longer a
			// faithful "what did this render at" answer once
			// wantsAgentDefaultResolutionCap can flip wantFilmOverride true
			// while params.width/height stay 0 (the agent-surface absent-dims
			// default is resolved live, inside applyFilmOverride, never
			// written back to `params`).  Meaningful only when
			// `wantFilmOverride` is true, same convention as origFilmW/H.
			unsigned int appliedFilmW = 0, appliedFilmH = 0;
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
			// R1b (2026-08-09): agent-surface samples cap bookkeeping -- see
			// AgentRenderParams::fromAgentSurface's doc.  `agentSamplesForceCapped`
			// is true only once SetSampleCountOverride actually SUCCEEDED for
			// the FORCED (no explicit `samples` requested) cap attempt --
			// mirrors overrodeSamples' own "true only on real success" honesty
			// contract.  `agentSamplesForceCapAttempted` is true whenever the
			// forced cap was ATTEMPTED regardless of outcome, so the tail can
			// report an honest "could not be applied" note (never silent) on
			// an unsupported rasterizer, matching the explicit-override
			// precedent just below.  `agentSamplesCapOrigValue` is the
			// scene-authored sample count (origSamples, captured before any
			// mutation) that triggered the attempt, for the message text.
			bool agentSamplesForceCapped = false;
			bool agentSamplesForceCapAttempted = false;
			int  agentSamplesCapOrigValue = -1;
			// P1 fix: the TAIL (after doRenderWork returns, possibly after
			// the park has released) needs the effective sample count for
			// the result message -- captured under the park, inside
			// doRenderWork's production body, instead of re-dereferencing
			// `rast` from the tail (see the `readBack` site below).
			int productionSampleReadBack = -1;
			// Creative-richness P2 (73-creative-richness-design.md sec 2 P2 /
			// sec 7 re-target; RELOCATED post-review from a post-park
			// ReadDocumentSnapshot() re-lock -- see ComputeDesignNoteFromDoc_'s
			// doc and this closure's own tail comments for the full
			// rationale): the design-note text, computed INSIDE doRenderWork's
			// tail (draft branch and production branch only) from the LIVE
			// `mJob->GetCstDocument()` while still under the park -- exactly
			// the document THIS render actually saw, no re-serialize/re-parse,
			// no re-entering the controller.  Read back at the carrier site
			// below, gated on res.ok/renderMode, same as productionSampleReadBack.
			std::string designNoteLocal;
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
			// transfer to the image cache -- which nulls `sink` explicitly) leaves
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
			// success the image cache owns the observation, so keeping it attached would
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
				// R1b (2026-08-09): the EXPLICIT override dims (both params.width
				// and params.height nonzero) by default; for the agent-surface
				// ABSENT-dims case (wantsAgentDefaultResolutionCap -- see its doc
				// above) resolve the scaled dims HERE instead, from the live
				// Film's aspect ratio just captured above -- this is the one
				// place in RenderCore_ where reading that aspect ratio is safe
				// (under the park; see the P1 fix comment above doRenderWork).
				// NEVER upscales: a Film already at or under
				// kAgentSurfaceMaxRenderEdge on its long edge renders unchanged
				// -- honestly reflected below by leaving agentResolutionCapped
				// false in that case (nothing was actually reduced).
				unsigned int overrideW = params.width;
				unsigned int overrideH = params.height;
				if( wantsAgentDefaultResolutionCap && curFilm && origFilmW > 0 && origFilmH > 0 ) {
					const unsigned int longEdge = origFilmW > origFilmH ? origFilmW : origFilmH;
					if( longEdge > kAgentSurfaceMaxRenderEdge ) {
						const double scale = static_cast<double>( kAgentSurfaceMaxRenderEdge ) /
							static_cast<double>( longEdge );
						overrideW = static_cast<unsigned int>( origFilmW * scale + 0.5 );
						overrideH = static_cast<unsigned int>( origFilmH * scale + 0.5 );
						if( overrideW < 1 ) overrideW = 1;
						if( overrideH < 1 ) overrideH = 1;
						res.agentResolutionCapped = true;
						res.filmWidth  = origFilmW;
						res.filmHeight = origFilmH;
					} else {
						// Already within the cap -- render at the Film's own
						// dims, unchanged (SetFilm below is then a same-dims
						// no-op via Job::SetFilm's short-circuit).
						overrideW = origFilmW;
						overrideH = origFilmH;
					}
				}
				// R1b: record what was actually applied -- see appliedFilmW/H's
				// doc above.  Set unconditionally alongside overrodeFilm's own
				// "meaningful only when wantFilmOverride" convention (a caller
				// reading these without checking wantFilmOverride first gets
				// 0/0 in the common no-override case, same honesty contract as
				// origFilmW/H before this lambda ran).
				appliedFilmW = overrideW;
				appliedFilmH = overrideH;
				if( wantFilmOverride && curFilm ) {
					if( mJob->SetFilm( overrideW, overrideH, origFilmPAR ) ) {
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
					// R1b NOTE: `params.width`/`height` here assume an EXPLICIT
					// override -- correct today because every caller that sets
					// maxPixelCount>0 (AgentEvalRunner.cpp's CheckRenderKind) also
					// leaves fromAgentSurface false, so wantsAgentDefaultResolutionCap
					// is false and wantFilmOverride reduces to the plain
					// (params.width>0 && params.height>0) check -- params.width/
					// height are never 0 when wantFilmOverride is true on that
					// path.  A future caller combining maxPixelCount>0 WITH
					// fromAgentSurface's absent-dims default would need
					// appliedFilmW/H here instead (not yet resolved at this point
					// in the closure -- applyFilmOverride() runs later).
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

				// ---- G1 (2026-08-10) `render{isolate:}`: resolve the object,
				// auto-frame the camera on it, and hide everything else for
				// the duration of THIS render.
				//
				// Placed AFTER the `view` block (so "did the caller supply a
				// camera?" is already settled, including a resolved view's
				// pose) and BEFORE `wantCameraOverride` below (so an
				// auto-framed pose flows through the SAME applyCameraOverride
				// machinery every other override uses -- no parallel
				// mechanism), and BEFORE the branch dispatch (so isolation
				// covers beauty, objectmap, the data view modes, the
				// BeautyVariant transports and draft identically -- object
				// visibility is Scene state, not rasterizer state).
				//
				// `isolateGuard` is declared at THIS scope so it outlives
				// every branch below -- including their early `return`s and
				// an exception unwinding out of Rasterize() -- and destructs
				// LAST relative to the production branch's fsGuard/
				// restoreGuard/sampleGuard (declared later, destructed
				// first).  Its restore touches only per-object flags and the
				// Scene's light generation, so it has no ordering
				// relationship with those three.
				ObjectSoloRestoreGuard isolateGuard( mJob->GetScene() );
				if( !params.isolate.empty() )
				{
					IObjectManager* objMgrForIsolate = mJob->GetObjects();
					IObjectPriv* isolateObj = nullptr;
					std::string isolateMessage;
					if( !ResolveIsolateObject( objMgrForIsolate, params.isolate, isolateObj, isolateMessage ) ) {
						res.ok = false;
						res.message = isolateMessage;
						specificFailureReported = true;
						return;
					}

					// Invariant 1 (see ObjectSoloRestoreGuard's doc): build
					// the TLAS over the FULL object set BEFORE hiding
					// anything, so the cached BVH can never be built from the
					// isolated set and outlive the restore.  Idempotent, and
					// it also runs RealizeAllObjects() -- which is what makes
					// the bounding box read below the object's REAL baked
					// extent rather than a deferred geometry's zero box.
					if( objMgrForIsolate ) objMgrForIsolate->PrepareForRendering();

					// Invariant 3 (G1 fix-round, 2026-08-10): the THIRD
					// visibility-dependent cache, closed with the SAME
					// pre-build-against-the-full-scene shape as invariant 1.
					//
					// AutoRasterizer resolves `auto_rasterizer`'s concrete
					// integrator inside a std::call_once -- ONCE per
					// dispatcher object, by design -- and its Tier-1 static
					// scan (SceneHasTransmissiveMaterial) walks
					// IObjectManager::EnumerateObjects, which filters on
					// IsWorldVisible.  If an `isolate` render were the FIRST
					// render on this Job, that one-and-only resolution would
					// see a ONE-OBJECT scene: a glass-plus-point-light scene
					// isolated down to an opaque part resolves to PT with the
					// reason "no caustic/strong-indirect signal", and NOTHING
					// invalidates mResolveOnce afterwards -- the restore below
					// puts the objects back but every later render, INCLUDING
					// the user's own full-scene production renders, keeps the
					// wrong integrator and the confidently-wrong reason.
					// (Missing caustic energy with no diagnostic pointing at
					// the cause; per docs/UNIFIED_INTEGRATOR_DECISION.md, VCM
					// is the only integrator that reaches some of it.)
					//
					// Forcing the resolution HERE -- full object set still
					// visible, before ApplyObjectSolo -- makes the isolated
					// render reuse the CORRECT resolution and leaves the
					// restore with nothing to undo, exactly like the TLAS.
					// Chosen over re-resolving on a topology change (that
					// would change AutoRasterizer's once-only contract, and
					// its per-render probe cost, for every host in the tree)
					// and over refusing `isolate` on an unresolved auto
					// rasterizer (a real capability lost to an internal
					// caching detail).
					//
					// Gated on isProductionBeauty because that is the ONLY
					// branch below that renders through the production
					// rasterizer: doDraftRenderWork / doObjectMapRenderWork /
					// doViewModeRenderWork / doBeautyVariantRenderWork each
					// build their OWN throwaway pipeline and never touch it,
					// so they cannot poison it and must not pay a probe's
					// cost.  Keep this gate in step if that ever changes.
					// The dynamic_cast is the SAME rasterizer-identity idiom
					// RasterizerSupportsLightSolo uses; a non-auto rasterizer
					// (or a null one) costs one failed cast and nothing else.
					// KNOWN, ACCEPTED ORDERING CAVEAT (G1 review round 3, P2).
					// This fires BEFORE applyFilmOverride() and the sample-count
					// override further down, whereas the BASELINE (non-isolate)
					// path resolves lazily inside mJob->Rasterize(), i.e. AFTER
					// both.  The Tier-2 PROBE reads the film's dimensions when it
					// runs, so on the isolate path it can see the scene's authored
					// dims rather than this request's effective (agent-capped)
					// ones.  Left as-is deliberately: the probe is opt-in
					// (`probe` default off) AND needs samples >= its activation
					// threshold AND needs the isolate render to be the FIRST on
					// this dispatcher, while the correct reordering would mean
					// hiding objects after the overrides -- restructuring render
					// setup, which is a larger risk than the narrow case it
					// closes.  Tier-1 (static scene analysis, the common path) is
					// unaffected: it reads materials and lights, not film dims.
					// The sample-count override is a non-issue either way --
					// AutoRasterizer does not implement Set/GetSampleCountOverride,
					// so the probe's activation gate reads the authored count
					// regardless of ordering.
					if( isProductionBeauty ) {
						if( RISE::Implementation::AutoRasterizer* autoRast =
								dynamic_cast<RISE::Implementation::AutoRasterizer*>( rast ) ) {
							autoRast->PreResolveIntegrator( mJob->GetScene() );
						}
					}

					const BoundingBox bb = static_cast<const IObject*>( isolateObj )->getBoundingBox();
					const double bbMin[3] = { bb.ll.x, bb.ll.y, bb.ll.z };
					const double bbMax[3] = { bb.ur.x, bb.ur.y, bb.ur.z };
					double ext[3] = { bbMax[0]-bbMin[0], bbMax[1]-bbMin[1], bbMax[2]-bbMin[2] };
					bool bboxUsable = true;
					for( int a = 0; a < 3; ++a ) {
						if( !RISE::IsFiniteDouble( bbMin[a] ) || !RISE::IsFiniteDouble( bbMax[a] ) ||
							!( ext[a] >= 0.0 ) || ext[a] > 1.0e12 ) {
							bboxUsable = false;
						}
					}
					const double diag = bboxUsable
						? std::sqrt( ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2] ) : 0.0;
					if( bboxUsable && !( diag > 0.0 ) ) bboxUsable = false;

					// The caller's own camera (an explicit `camera` override,
					// or a resolved `view`) WINS -- no auto-framing at all.
					const bool callerSuppliedCamera =
						effectiveCamera.hasLocation || effectiveCamera.hasLookAt ||
						effectiveCamera.hasUp || effectiveCamera.hasOrientation ||
						effectiveCamera.hasTargetOrientation || effectiveCamera.hasFov;

					if( !callerSuppliedCamera && !bboxUsable ) {
						char box[224];
						std::snprintf( box, sizeof( box ),
							" -- its world bounding box is (%.6g %.6g %.6g) .. (%.6g %.6g %.6g)",
							bbMin[0], bbMin[1], bbMin[2], bbMax[0], bbMax[1], bbMax[2] );
						res.ok = false;
						res.message = "isolate \"" + params.isolate + "\" cannot be auto-framed: the object has a "
							"degenerate or unbounded extent" + box +
							".  Supply an explicit `camera` to isolate it anyway.";
						specificFailureReported = true;
						return;
					}

					// The ACTIVE camera's own pose/FOV: the baseline the
					// coverage projection below measures against, and the FOV
					// the auto-framing distance is solved for (so an isolate
					// render keeps the scene's own lens rather than imposing
					// one).  Only a PinholeCamera reports a FOV; for anything
					// else assume 45 deg and say so (isolateFovAssumed).
					static const double kDefaultVFovRad = 45.0 * 3.14159265358979323846 / 180.0;
					double camEye[3]    = { 0.0, 0.0, 0.0 };
					double camTarget[3] = { 0.0, 0.0, -1.0 };
					double camUp[3]     = { 0.0, 1.0, 0.0 };
					double camVFovRad   = kDefaultVFovRad;
					bool   haveActiveSnapshot = false;
					if( ICameraManager* camsForIsolate = mJob->GetCameras() ) {
						const std::string activeNameForIsolate = mJob->GetActiveCameraName();
						const ICamera* activeCamForIsolate = !activeNameForIsolate.empty()
							? camsForIsolate->GetItem( activeNameForIsolate.c_str() ) : nullptr;
						CameraSnapshot snap;
						if( activeCamForIsolate && CameraIntrospection::CaptureCameraSnapshot( *activeCamForIsolate, snap ) ) {
							haveActiveSnapshot = true;
							for( int a = 0; a < 3; ++a ) {
								camEye[a]    = snap.location[a];
								camTarget[a] = snap.lookat[a];
								camUp[a]     = snap.up[a];
							}
							if( snap.type == RISE::CameraSnapshot::Pinhole && snap.fov > 0.0 ) {
								camVFovRad = snap.fov;   // CameraSnapshot::fov is RADIANS (CameraIntrospection's convention)
							} else {
								isolateFovAssumed = true;
							}
						}
					}
					if( !haveActiveSnapshot ) isolateFovAssumed = true;

					// The aspect ratio THIS render will actually use: an
					// explicit width/height pair when given, else the live
					// Film's -- every implicit path (the agent-surface
					// absent-dims default, the BeautyVariant divisor)
					// preserves the Film's ratio, so the Film is the right
					// fallback for all of them.
					double renderAspect = 1.0;
					{
						unsigned int aspW = params.width, aspH = params.height;
						double pixAR = 1.0;
						const IScenePriv* scenePrivForAspect = mJob->GetScene();
						const IFilm* filmForAspect = scenePrivForAspect ? scenePrivForAspect->GetFilm() : nullptr;
						if( filmForAspect ) {
							pixAR = filmForAspect->GetPixelAR();
							if( !( aspW > 0 && aspH > 0 ) ) {
								aspW = filmForAspect->GetWidth();
								aspH = filmForAspect->GetHeight();
							}
						}
						if( aspW > 0 && aspH > 0 && RISE::IsFiniteDouble( pixAR ) && pixAR > 0.0 ) {
							renderAspect = ( static_cast<double>( aspW ) / static_cast<double>( aspH ) ) * pixAR;
						}
					}

					if( !callerSuppliedCamera )
					{
						// Solve the eye distance that fits the object's whole
						// world AABB into kIsolateFrameFill of the frame from
						// the chosen vantage -- see IsolateFitDistance for the
						// derivation (and for why the exact box fit, not a
						// bounding sphere).
						//
						// G3b (2026-08-10): the vantage is now NAMED.  With no
						// `target` (or a `target` whose view is somehow not one
						// of the three) IsolateVantageOffset returns G1's fixed
						// three-quarter direction and world +Y up -- the plain
						// isolate render is byte-for-byte what G1 shipped.  With
						// a resolved target the axis-aligned vantage matching the
						// sketch's declared view is used instead, because that is
						// the projection the sketch actually describes; the fit,
						// the fill fraction and every other framing step are
						// untouched.
						double offset[3];
						double worldUp[3];
						IsolateVantageOffset( targetSketch ? targetSketch->view : std::string(),
						                      offset, worldUp );
						const double center[3] = { ( bbMin[0]+bbMax[0] ) * 0.5,
						                            ( bbMin[1]+bbMax[1] ) * 0.5,
						                            ( bbMin[2]+bbMax[2] ) * 0.5 };
						const double tanHalfV   = std::tan( camVFovRad * 0.5 );
						double distance = IsolateFitDistance( bbMin, bbMax, center, offset,
						                                       worldUp, tanHalfV, renderAspect );
						if( !( distance > 0.0 ) ) distance = diag * 2.0;   // degenerate basis/FOV -- a sane, non-zero fallback
						for( int a = 0; a < 3; ++a ) {
							camTarget[a] = center[a];
							camEye[a]    = center[a] + offset[a] * distance;
						}
						camUp[0] = worldUp[0]; camUp[1] = worldUp[1]; camUp[2] = worldUp[2];
						// The vantage FACT, recorded whether or not a target is
						// in play (it is only reported when one is -- see the
						// `targetVantage` field's doc).
						if( targetSketch ) res.targetVantage = targetSketch->view;

						// Feed the SAME AgentCameraOverride fields `camera`
						// and `view` use, so applyCameraOverride restores the
						// active camera afterwards with no extra machinery.
						// FOV is deliberately NOT overridden: the framing was
						// solved FOR the active camera's own lens, and setting
						// "fov" would fail-loud on a non-pinhole active camera.
						effectiveCamera.hasLocation = true;
						effectiveCamera.location    = Vec3ToOverrideStr( camEye );
						effectiveCamera.hasLookAt   = true;
						effectiveCamera.lookAt      = Vec3ToOverrideStr( camTarget );
						effectiveCamera.hasUp       = true;
						effectiveCamera.up          = Vec3ToOverrideStr( camUp );
						res.isolateAutoFramed = true;
					}
					else
					{
						// Caller-supplied pose: read the numbers back out of
						// the SAME override fields the render will apply, so
						// the coverage below is measured against the camera
						// that actually renders -- not the pre-override
						// active camera.  A pose expressed ONLY as an
						// orientation (no location/lookat pair) cannot be
						// resolved to an eye/target here, so coverage is
						// honestly left unavailable in that case.
						auto parseVec3 = []( const std::string& s, double out[3] ) -> bool {
							return std::sscanf( s.c_str(), "%lf %lf %lf", &out[0], &out[1], &out[2] ) == 3;
						};
						bool poseResolvable = true;
						if( effectiveCamera.hasLocation && !parseVec3( effectiveCamera.location, camEye ) ) poseResolvable = false;
						if( effectiveCamera.hasLookAt   && !parseVec3( effectiveCamera.lookAt,   camTarget ) ) poseResolvable = false;
						if( effectiveCamera.hasUp       && !parseVec3( effectiveCamera.up,       camUp ) ) poseResolvable = false;
						if( effectiveCamera.hasFov ) {
							const double fovDeg = std::strtod( effectiveCamera.fov.c_str(), nullptr );
							if( fovDeg > 0.0 && fovDeg < 180.0 ) {
								camVFovRad = fovDeg * 3.14159265358979323846 / 180.0;
								isolateFovAssumed = false;
							}
						}
						if( ( effectiveCamera.hasOrientation || effectiveCamera.hasTargetOrientation ) &&
							!( effectiveCamera.hasLocation && effectiveCamera.hasLookAt ) ) {
							poseResolvable = false;
						}
						// G3b: the caller's camera wins over the sketch's
						// axis-aligned vantage (G1's rule, unchanged), so the
						// silhouette this comparison measures is NOT the
						// projection the sketch describes.  Say which vantage
						// ran, as a fact -- no warning, no adjective; the
						// caller asked for this camera.
						if( targetSketch ) res.targetVantage = "caller-camera";
						if( !poseResolvable || !haveActiveSnapshot ) {
							// Signal "coverage unavailable" to the projection below.
							camVFovRad = -1.0;
						}
					}

					// The measured facts (see AgentRenderResult's docs).
					res.isolateObject = params.isolate;
					// FIX 4 (G1 fix-round, 2026-08-10): only publish bbox/
					// longestEdge when the box is actually USABLE.  A
					// degenerate/unbounded bbox is reachable here despite the
					// refusal above -- that refusal only fires when the
					// caller did NOT supply their own camera; a caller who
					// supplies an explicit `camera` can still isolate an
					// object whose box fails the finite/non-negative-extent
					// checks.  Copying raw bb.ll/bb.ur unconditionally would
					// report NaN/Inf, which SerializeNumber (Json.cpp) clamps
					// to a literal 0 -- indistinguishable from "the object is
					// genuinely a point".  Mirror bboxCoverage's own
					// sentinel-then-omit convention instead: leave the
					// default-constructed 0/0/0 values but gate the
					// STRUCTURED FIELD on isolateBBoxUsable (see AgentRpc.cpp)
					// so the RPC layer omits bboxMin/bboxMax/longestEdge
					// entirely rather than emitting fabricated-looking zeros.
					res.isolateBBoxUsable = bboxUsable;
					if( bboxUsable ) {
						for( int a = 0; a < 3; ++a ) {
							res.isolateBBoxMin[a] = bbMin[a];
							res.isolateBBoxMax[a] = bbMax[a];
						}
						res.isolateLongestEdge = std::max( ext[0], std::max( ext[1], ext[2] ) );
					}
					// FIX 2 (G1 fix-round, 2026-08-10): SUPPRESS bboxCoverage
					// entirely -- rather than caveat it -- whenever the
					// projection model isn't a pinhole (isolateFovAssumed).
					// applyCameraOverride can only SetProperty fields on the
					// ALREADY-active camera (it never re-types it), so a
					// thin-lens/fisheye/orthographic active camera renders
					// with ITS OWN (non-tan-based) projection regardless of
					// what pose/fov the caller or the auto-framer computed;
					// ProjectedBBoxCoverage's formula is a pinhole tan(fov/2)
					// model, so applying it there wouldn't be "approximate",
					// it would be the WRONG projection model entirely.  A
					// wrong-model number that looks clean is worse than an
					// absent one -- this project has measured that payload
					// facts get acted on by the calling model.  Applies in
					// EVERY branch (auto-framed and caller-supplied alike);
					// previously only `!poseResolvable || !haveActiveSnapshot`
					// forced the sentinel, so a caller-supplied camera over a
					// non-pinhole active camera fell through to a computed
					// (wrong-model) number with nothing disclosing it.
					res.isolateBBoxCoverage = ( bboxUsable && !isolateFovAssumed && camVFovRad > 0.0 )
						? ProjectedBBoxCoverage( bbMin, bbMax, camEye, camTarget, camUp,
						                          std::tan( camVFovRad * 0.5 ), renderAspect )
						: -1.0;

					// Apply the isolation LAST, once every read of the
					// unmodified scene above is done, and arm the restore in
					// the same breath.  The light-generation bump is the
					// apply half of ObjectSoloRestoreGuard's invariant 2: any
					// caster that (re)builds during this render must see the
					// ISOLATED luminary set, and the guard's matching bump on
					// restore forces the full set back afterwards.
					unsigned int hiddenCount = 0;
					std::vector<std::pair<IObjectPriv*, bool> > priorVisibility =
						ApplyObjectSolo( objMgrForIsolate, isolateObj, hiddenCount );
					isolateGuard.Arm( std::move( priorVisibility ) );
					if( RISE::Implementation::Scene* concreteSceneForIsolate =
							dynamic_cast<RISE::Implementation::Scene*>( mJob->GetScene() ) ) {
						concreteSceneForIsolate->BumpLightTopologyGeneration();
					}
					res.isolateApplied = true;
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
					// Creative-richness P2: read the LIVE document HERE, still
					// under the park -- never re-enter the controller
					// (ReadDocumentSnapshot / ComputeDesignNote) from inside
					// this closure.  See designNoteLocal's declaration above.
					if( const RISE::Cst::Document* liveDoc = mJob->GetCstDocument() )
						designNoteLocal = ComputeDesignNoteFromDoc_( *liveDoc );
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
				} else if( params.fromAgentSurface &&
					( origSamples < 0 || origSamples > kAgentSurfaceMaxSamples ) ) {
					// R1b (2026-08-09): no `samples` override was requested, but
					// the scene's own authored count (origSamples, just
					// captured above, before any mutation) exceeds the agent
					// surface's cap -- force one, exactly as if the caller had
					// asked for kAgentSurfaceMaxSamples.  Keeps the render
					// cheap even when an untrusted agent-driven scene edit
					// bumped the rasterizer's own `samples` chunk param
					// arbitrarily high; see AgentRenderResult::agentSamplesCapped's
					// doc for the honesty contract this feeds.
					//
					// FIX 1 (P1, 2026-08-09): `origSamples < 0` widens this to
					// also fire when the active rasterizer doesn't report a
					// sample count at all.  IRasterizer::GetSampleCountOverride's
					// base default returns -1 for every rasterizer outside the
					// PixelBasedRasterizerHelper family (MLT, photon-map-only,
					// AutoRasterizer's outer wrapper -- see
					// IRasterizer.h:181-190); PixelBasedRasterizerHelper's own
					// override (the ONLY Get/SetSampleCountOverride pair in the
					// tree) never returns -1, so -1 here unambiguously means
					// "this rasterizer doesn't participate in the override
					// protocol", not "zero samples".  Without this widening,
					// `-1 > kAgentSurfaceMaxSamples` is false, the cap is never
					// attempted, and an untrusted scene's
					// `mutations_per_pixel 5000000` on an MLT rasterizer would
					// render at full authored cost with no honesty note at all
					// (agentSamplesForceCapAttempted stays false, so the "could
					// NOT be applied" branch below never fires either).
					// SetSampleCountOverride on these rasterizers still returns
					// false (IRasterizer's base default), so
					// agentSamplesForceCapped stays false and the message tail
					// below correctly reports the honest "could NOT be applied"
					// case for both the known-too-high and unknown origSamples
					// values.
					agentSamplesForceCapAttempted = true;
					agentSamplesCapOrigValue = origSamples;
					overrodeSamples = rast->SetSampleCountOverride( kAgentSurfaceMaxSamples );
					agentSamplesForceCapped = overrodeSamples;
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
				// on every exit, including success: the image cache becomes the sole owner
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
				// R1b: use appliedFilmW/H (what applyFilmOverride() actually
				// resolved and set via SetFilm just above), NOT params.width/
				// height directly -- those stay 0 for the agent-surface
				// absent-dims default even though wantFilmOverride is true
				// for it (the scaled dims are resolved live inside
				// applyFilmOverride, never written back to `params`).
				const unsigned int renderW = wantFilmOverride ? appliedFilmW : origFilmW;
				const unsigned int renderH = wantFilmOverride ? appliedFilmH : origFilmH;
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
				// Creative-richness P2: read the LIVE document HERE, still
				// under the park -- see designNoteLocal's declaration above
				// and the isDraft branch's twin comment for why this must
				// never go through ReadDocumentSnapshot / ComputeDesignNote
				// (both re-enter the controller) from inside this closure.
				if( const RISE::Cst::Document* liveDoc = mJob->GetCstDocument() )
					designNoteLocal = ComputeDesignNoteFromDoc_( *liveDoc );
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
				// value the callee leaves here on the throw path.  Round-10
				// correction: NOT because "the callee never reaches a refusal
				// site so it does not write this" -- it writes None
				// UNCONDITIONALLY on entry, so this initializer is
				// belt-and-braces, not the mechanism.  The mechanism is that an
				// exception out of `fn` unwinds past every refusal site, leaving
				// that entry write as the last one -- see
				// RunPreviewRenderParked's header doc.
				SceneEditController::RenderRefusal refusal =
					SceneEditController::RenderRefusal::None;
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
					// "editor transaction or gesture in progress -- retry after
					// it completes") so a caller sees the identical wording
					// whether it hit a param edit, a chunk edit, or a preview
					// render.  Round-10 P1: this quotation used to read "editor
					// transaction in progress -- retry after the gesture
					// completes", which is not what either sibling says; the
					// message emitted below was changed to the sibling's ACTUAL
					// string, so the claim of verbatim reuse is now true.
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
					// refusals so the two layers agree.  Both quotations were
					// re-checked against their sources in round 10:
					//   * SceneEditController's chunk-CRUD render-busy refusals
					//     say exactly "render queued or in progress -- retry
					//     after it completes" -- matches.
					//   * the edit verbs (ApplyAgentParamEdit /
					//     ApplyAgentChunkCrud_) say exactly "editor transaction
					//     or gesture in progress -- retry after it completes".
					//     The round-8 comment MISQUOTED this one as "editor
					//     transaction in progress -- retry after the gesture
					//     completes", and the string emitted below WAS that
					//     misquote -- so the two layers did not in fact agree.
					//     Round-10 P1 fix: emit the sibling's real string.
					res.ok          = false;
					res.renderJobId = renderJobId;   // 0 here -- no render ran, matching the other pre-flight refusal paths in this function
					res.message     = "editor transaction or gesture in progress -- retry after it completes";
					switch( refusal ) {
					case SceneEditController::RenderRefusal::CoordinatedRenderBusy:
						res.message = "render queued or in progress -- retry after it completes";
						break;
					case SceneEditController::RenderRefusal::ControllerStopped:
						// NOT retriable -- say so rather than inviting a retry
						// loop against a controller that is going away.
						res.message = "render refused: the editor is shutting down";
						break;
					case SceneEditController::RenderRefusal::InteractionFinalizeFailed:
						res.message = "render refused: an open editor interaction could not be finalized -- retry shortly";
						break;
					case SceneEditController::RenderRefusal::InteractionFinalizeLatched:
						// Round-10 finding 3.  The LATCHED case: the controller's
						// mInteractionPersistenceFailed flag is set and is NEVER
						// cleared, so this refusal fires for every render and every
						// viewport read for the rest of the session.  Round 8 could
						// not tell the two apart and said "retry shortly" here --
						// precisely the infinite-retry instruction this branch exists
						// to remove.
						res.message = "render refused: an editor interaction failed to persist and the editor has LATCHED that failure -- this does NOT clear on its own and retrying will not help; renders and viewport reads stay refused until the scene is reopened";
						break;
					case SceneEditController::RenderRefusal::PinnedRenderBusy:
						// Not producible by RunPreviewRenderParked (no slot concept) --
						// listed to keep the switch total so -Wswitch keeps guarding
						// this mapping, and mapped rather than left to the pre-switch
						// default so a future routing change cannot silently mislabel
						// it as a transaction refusal.
						res.message = "render refused: a pinned render is in flight -- pinned renders run to completion and are never superseded; retry after it completes";
						break;
					case SceneEditController::RenderRefusal::EditorBusy:
					case SceneEditController::RenderRefusal::None:
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
				// Round-10 finding 2b: collect the REPORTED refusal cause instead
				// of inferring one afterwards.  Seeded to None for the same
				// belt-and-braces reason as the override branch above.
				SceneEditController::RenderRefusal refusal =
					SceneEditController::RenderRefusal::None;
				std::string thrownMessage;
				try {
					submitted = mController->SubmitAgentRenderSync( doRenderWork, String(), &controllerJobId,
						/*timeoutMs=*/30000, params.pinned,
						SceneEditController::RenderClass::AgentPreview, &refusal );
				}
				catch( const std::exception& e ) { thrownMessage = e.what(); }
				catch( ... )                     { thrownMessage = "unknown exception"; }
				if( !submitted && thrownMessage.empty() ) {
					// Refused.  Honest failure -- no fallback direct call here,
					// since a direct call is exactly the race S2a closes.
					// P2 fix (2026-07-19 mutation review): doRenderWork never
					// entered the park on THIS path (SubmitAgentRenderSync
					// refused before running it) -- see the RunPreviewRenderParked
					// refusal branch above and AgentRenderResult::integrator's
					// field doc for the same reasoning.
					//
					// ROUND-10 finding 2 (P1).  This branch USED to pick its
					// message by reading `CurrentRenderJob().pinned` after the
					// refusal.  That was wrong twice over.  (1) The field was
					// written at ONE of the three job-record mint sites, so once
					// any pinned render had ever completed it stayed true for the
					// session's life and this -- the path EVERY plain `render`
					// call takes -- told the model "a pinned render is in flight"
					// when none was.  (2) Even with the field fixed, a sync
					// submission does not refuse on a pinned occupant at all: it
					// WAITS for the fairness window (see SubmitAgentRenderSync's
					// S3-P2 doc), so a pinned occupant can only surface here as a
					// fairness-wait TIMEOUT.  Both are the same defect round 8
					// removed from the override branch: inferring a cause the
					// callee already knows.  The callee now reports it.
					//
					// As in the override branch, the retriable generic message is
					// assigned BEFORE the switch narrows it (an enumerator added
					// without extending this switch still yields an honest
					// message, never an empty one) and the switch stays TOTAL so
					// -Wswitch flags the omission at build time first.
					res.ok = false;
					res.message = "render refused: the agent-render worker is busy or an editor transaction is open -- retry shortly";
					switch( refusal ) {
					case SceneEditController::RenderRefusal::PinnedRenderBusy:
						res.message = "render refused: a pinned render is in flight -- pinned renders run to completion and are never superseded; retry after it completes";
						break;
					case SceneEditController::RenderRefusal::CoordinatedRenderBusy:
						res.message = "render queued or in progress -- retry after it completes";
						break;
					case SceneEditController::RenderRefusal::ControllerStopped:
						// NOT retriable.
						res.message = "render refused: the editor is shutting down";
						break;
					case SceneEditController::RenderRefusal::EditorBusy:
						res.message = "editor transaction or gesture in progress -- retry after it completes";
						break;
					case SceneEditController::RenderRefusal::InteractionFinalizeFailed:
						res.message = "render refused: an open editor interaction could not be finalized -- retry shortly";
						break;
					case SceneEditController::RenderRefusal::InteractionFinalizeLatched:
						// Round-10 finding 3 -- NOT retriable, and it never clears.
						res.message = "render refused: an editor interaction failed to persist and the editor has LATCHED that failure -- this does NOT clear on its own and retrying will not help; renders and viewport reads stay refused until the scene is reopened";
						break;
					case SceneEditController::RenderRefusal::None:
						// Unreachable on a refusal (the callee sets a cause at every
						// `return false`); listed so the switch stays total, and so a
						// contract slip still produces the honest generic message.
						break;
					}
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
			// R1b (2026-08-09): honest note for the agent-surface ABSENT-dims
			// default -- see AgentRenderResult::agentResolutionCapped's doc.
			// Only fires when it actually reduced the render below the
			// scene's authored Film size (never for a Film already at or
			// under the cap, and never for an explicit width/height pair --
			// that clamp, when it fires, is reported by the RPC layer, which
			// alone has the caller's raw pre-clamp request).
			if( res.agentResolutionCapped && res.ok ) {
				char resNote[256];
				std::snprintf( resNote, sizeof( resNote ),
					" (agent render cap: no width/height requested and the scene's authored Film (%ux%u) "
					"exceeds the %upx agent surface max edge -- rendered at %ux%u)",
					res.filmWidth, res.filmHeight, kAgentSurfaceMaxRenderEdge, res.width, res.height );
				res.message += resNote;
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
				// R1b (2026-08-09) ADDITIVE wire field -- see
				// AgentRenderResult::agentSamplesCapped's doc.  False (the
				// default) whenever the forced-cap branch inside doRenderWork
				// never fired (an explicit `samples` was requested, or the
				// scene's authored count was already <= the cap, or this
				// wasn't an agent-surface call at all).
				res.agentSamplesCapped = agentSamplesForceCapped;
				if( overrodeSamples ) {
					// R1b: read back the ACTUAL applied count rather than
					// assuming params.samples -- the forced-cap branch applies
					// kAgentSurfaceMaxSamples with params.samples still at its
					// -1 "no override requested" sentinel, so params.samples is
					// not a faithful answer for that case.  productionSampleReadBack
					// was captured under the park, right after Rasterize(),
					// while the override was still in effect -- correct for
					// BOTH the explicit and the forced-cap case.
					const int readBack = productionSampleReadBack;
					res.effectiveSamples = ( readBack >= 1 ) ? readBack : params.samples;
					if( agentSamplesForceCapped && res.ok ) {
						char capNote[224];
						std::snprintf( capNote, sizeof( capNote ),
							" (agent render cap: no samples requested and the scene's authored sample count (%d) "
							"exceeded the %d-spp agent surface cap -- capped to %d)",
							agentSamplesCapOrigValue, kAgentSurfaceMaxSamples, kAgentSurfaceMaxSamples );
						res.message += capNote;
					}
				} else {
					const int readBack = productionSampleReadBack;   // P1 fix: captured under the park inside doRenderWork -- never re-dereference `rast` here
					res.effectiveSamples = ( readBack >= 1 ) ? readBack : 0;
					if( wantSamplesOverride && res.ok ) {
						res.message += " (samples override not supported by the active rasterizer -- rendered at the scene-authored count)";
					} else if( agentSamplesForceCapAttempted && res.ok ) {
						// R1b: the forced cap was ATTEMPTED but the active
						// rasterizer does not support SetSampleCountOverride
						// (MLT, photon-map-only, AutoRasterizer's outer
						// wrapper) -- report that HONESTLY (never silently
						// fall through) without refusing the render, matching
						// the explicit-override precedent just above.
						//
						// FIX 1 (P1, 2026-08-09): `agentSamplesCapOrigValue` can
						// now be -1 (rasterizer never reported a count -- see
						// the widened gate above), so the two cases get
						// distinct wording: printing "-1" as if it were the
						// scene's authored count would read as nonsense.
						char capNote[320];
						if( agentSamplesCapOrigValue < 0 ) {
							std::snprintf( capNote, sizeof( capNote ),
								" (agent render cap: the active rasterizer does not report its sample count "
								"(unknown/unsupported by the override protocol), so the %d-spp agent surface cap "
								"could NOT be verified or applied; rendered at the scene-authored count)",
								kAgentSurfaceMaxSamples );
						} else {
							std::snprintf( capNote, sizeof( capNote ),
								" (agent render cap: the scene's authored sample count (%d) exceeds the %d-spp agent "
								"surface cap, but the active rasterizer does not support a sample-count override -- "
								"the cap could NOT be applied; rendered at the scene-authored count)",
								agentSamplesCapOrigValue, kAgentSurfaceMaxSamples );
						}
						res.message += capNote;
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

			// G1 (2026-08-10) `render{isolate:}`: the honest note.  Unlike
			// `light`, isolation applies to EVERY render target (it is Scene
			// object state, not rasterizer state), so there is no
			// "ignored under mode X" branch here -- only the framing fact,
			// plus the qualifiers that would otherwise leave the caller
			// guessing: a caller-supplied camera beat the auto-frame; the
			// active camera is not a pinhole (G1 fix-round FIX 2 -- affects
			// BOTH the auto-framed distance solve, where it's an
			// approximation caveat, and bboxCoverage, which is SUPPRESSED
			// rather than caveated -- see the isolateFovAssumed comment
			// above); the bbox itself is degenerate/unusable (G1 fix-round
			// FIX 4 -- only reachable with a caller-supplied camera, since
			// the render() refusal path already rejects this case
			// otherwise).  An unresolvable/ambiguous/degenerate-with-no-
			// caller-camera `isolate` already failed the render loudly
			// inside doRenderWork, so this block only runs when it resolved.
			if( res.ok && res.isolateApplied ) {
				char isoNote[512];
				if( !res.isolateBBoxUsable ) {
					// Degenerate/unbounded bbox, reached only via an explicit
					// caller camera (see the refusal above) -- bboxMin/
					// bboxMax/longestEdge/bboxCoverage are all omitted on
					// the wire (FIX 4); say so instead of silently reporting
					// nothing, which would look like the render just forgot.
					std::snprintf( isoNote, sizeof( isoNote ),
						" (isolate: \"%s\" is the only object rendered; its bounding box is degenerate "
						"or unbounded, so bboxMin/bboxMax/longestEdge/bboxCoverage are not reported; "
						"framed by the camera you supplied, not auto-framed)",
						res.isolateObject.c_str() );
				} else if( res.isolateAutoFramed ) {
					// G3b (2026-08-10): the vantage is named rather than
					// hardcoded to "three-quarter" -- with a `target` in play
					// the auto-frame is axis-aligned to the sketch's declared
					// view, and a message that still said "three-quarter"
					// would be a false statement about the image the caller is
					// about to read.  With no target the string is unchanged.
					const std::string vantageWord = targetSketch
						? ( targetSketch->view + " (axis-aligned)" ) : std::string( "three-quarter" );
					std::snprintf( isoNote, sizeof( isoNote ),
						" (isolate: \"%s\" is the only object rendered; longest bbox edge %.6g; auto-framed "
						"%s view%s)",
						res.isolateObject.c_str(), res.isolateLongestEdge, vantageWord.c_str(),
						isolateFovAssumed
							? ", distance APPROXIMATE -- the active camera is not a pinhole, so a 45 deg "
							  "vertical FOV was assumed; bboxCoverage was not computed for the same reason"
							: "" );
				} else {
					std::snprintf( isoNote, sizeof( isoNote ),
						" (isolate: \"%s\" is the only object rendered; longest bbox edge %.6g; framed "
						"by the camera you supplied, not auto-framed%s)",
						res.isolateObject.c_str(), res.isolateLongestEdge,
						isolateFovAssumed
							? "; bboxCoverage was not computed because the active camera is not a pinhole"
							: "" );
				}
				res.message += isoNote;
			}

			// Creative-richness P2 (73-creative-richness-design.md sec 2 P2,
			// RE-TARGETED by sec 7): attach the observed-state design note
			// on a successful BEAUTY render only -- "production" or
			// "draft" `renderMode`, deliberately never "objectmap" or a
			// view mode (those are diagnostic/segmentation renders, not
			// the "the model just looked at its finished work" moment the
			// note is anchored to; matches the isObjectMap/isViewMode
			// gating precedent used throughout this function).
			//
			// RELOCATED post-review (concurrency review, round 2): the
			// document is read and the note COMPUTED inside doRenderWork's
			// tail (the isDraft branch and the production branch each set
			// `designNoteLocal` -- see its declaration above), while this
			// call is STILL under the park in every controller-attached
			// branch (the worker's cancel-and-park hold for both
			// RunPreviewRenderParked and SubmitAgentRender*, or headless's
			// single-writer guarantee) -- so it describes EXACTLY the
			// document this render saw, never a later edit that landed in
			// the window after the park released.  This site only
			// PUBLISHES the already-computed local into `res`, gated on
			// success/renderMode; it does no document I/O of its own and
			// never touches the controller.  This is also why the earlier
			// `assumeParked` branch (which called ReadDocumentSnapshot()/
			// ComputeDesignNote() from OUTSIDE the park, and for
			// assumeParked==true self-deadlocked on the worker's own
			// already-held mMutex -- see the historical mutation-probe log
			// in the final report) is gone: there is no document access
			// left here to branch on.
			if( res.ok && ( res.renderMode == "production" || res.renderMode == "draft" ) ) {
				res.note = designNoteLocal;
			}

			// Cache for ReadImage() ONLY on a successful, non-empty encode --
			// a failed render must not wipe a prior good cache (ReadImage
			// documents "the LAST successful Render").  Also keep the SINK
			// itself (swap-release the previous one) so ReadImage(maxEdge) can
			// re-encode a downscaled PNG from the cached full-res linear
			// pixels without re-rendering -- `sink` already carries refcount 1
			// (our owning ref from `new` above); we transfer that ownership to
			// the image cache (AgentImageCache::Store) instead of releasing it.
			//
			// Model-B F2 slice S2a: the store is atomic under the cache's own
			// lock -- when this is running on the async worker thread
			// (assumeParked), a concurrent ReadImage() call on the
			// submitter's thread must not observe a torn png/sink update.
			// With a SHARED cache this is also the moment a render performed
			// through one session becomes readable through its siblings,
			// which is the entire point.
			if( res.ok ) {
				mImageCache->Store( res.png, sink );
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
			//! the AgentImageCache frame (the png/sink pair `ReadImage()` serves
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
			//!    cache OUT (AgentImageCache::Take leaves the png empty and
			//!    the sink null, and it zeroes the async-result id), so
			//!    from the ctor until SOME render's cache tail repopulates
			//!    it -- i.e. THE WHOLE RENDER DURATION -- a concurrent
			//!    `ReadImage()` on another thread returns ZERO BYTES,
			//!    `ReadImage(unsigned int,...)` returns empty too (by TWO
			//!    different lines: for `maxEdge > 0` from the `if( !sink )`
			//!    early-out right after that function's lock scope; for
			//!    `maxEdge == 0` from the IN-LOCK `PngWithDims` read inside
			//!    its `maxEdge == 0` branch, which hands back the emptied
			//!    cache and leaves outWidth/outHeight at 0 because
			//!    the cached sink is null), and `ReadPerception` likewise.
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
			//!  * SHARED-CACHE SCOPE (2026-07).  The cache this guard stashes
			//!    is now an `AgentImageCache` that a host may SHARE across
			//!    several sessions (the GUIs' three in-app ones), so the
			//!    zero-byte window and the RESIDUAL above are no longer
			//!    confined to the session running the ephemeral render --
			//!    they now apply to every session sharing the handle.  What
			//!    that does and does not mean:
			//!      - A SIBLING'S SYNCHRONOUS read cannot land in the window.
			//!        Both bridges drive all three in-app sessions from the
			//!        host UI thread (RISEViewportBridge's
			//!        -agentHandleToolCall:autonomy: / -agentHandleLine:,
			//!        ViewportBridge's agentHandleToolCall / agentHandleLine),
			//!        and the guarded render blocks that thread for the whole
			//!        window.  There is no thread left to observe it.
			//!      - A SIBLING'S ASYNC render completing inside the window
			//!        loses its cache write, exactly as a same-session one
			//!        already does.  This widens an accepted residual; it
			//!        does not introduce a new failure mode, and the upstream
			//!        fix named above closes both at once.  It is now worth
			//!        more than it was.
			//!      - The hosted loopback server's External session is NOT in
			//!        the group (nobody hands it the handle), so none of this
			//!        reaches it.
			//!
			//! Takes the pieces of state by reference rather than an
			//! `AgentSession&` so it stays a file-local helper with no access
			//! to the class's private section (no friendship, no header churn).
			class EphemeralRenderCacheGuard
			{
			public:
				EphemeralRenderCacheGuard( std::mutex& sessionMutex,
				                           AgentImageCache& imageCache,
				                           AgentRenderResult& lastAsyncResult,
				                           std::uint64_t& lastAsyncResultJobId )
					: mSessionMutex( sessionMutex ), mImageCache( imageCache ),
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

					// BOTH halves under the session lock, with the cache's
					// leaf lock nested inside it (AgentImageCache::Take takes
					// it) -- the documented outer->inner order.  Doing them in
					// two SEPARATE scopes would open a window where the pixels
					// are stashed but the async-result record is not, i.e.
					// render_wait answering "completed, here is your result"
					// for a frame read_image can no longer serve: the exact lie
					// this guard exists to prevent.
					std::lock_guard<std::mutex> lk( mSessionMutex );
					// Take() moves the frame OUT and leaves the cache empty,
					// handing us the sink's reference.  That transfer is what
					// makes the stash MEMORY-SAFE, not merely invisible (round-8
					// sabotage finding, recorded so nobody "simplifies" it): the
					// guarded Render()'s cache tail releases whatever sink the
					// cache holds before storing its own.  Leave the stashed
					// pointer ALSO reachable from the cache and that release
					// frees the framebuffer we still owe back, so the dtor hands
					// a DANGLING sink to the session.  Verified: breaking this
					// segfaults the suite.
					mImageCache.Take( mSavedPng, mSavedSink );
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
					std::lock_guard<std::mutex> lk( mSessionMutex );
					// Store() releases whatever the ephemeral render left in
					// the cache and takes over our stashed sink's reference.
					mImageCache.Store( std::move( mSavedPng ), mSavedSink );
					mSavedSink = nullptr;
					mLastAsyncResult      = std::move( mSavedAsyncResult );
					mLastAsyncResultJobId = mSavedAsyncResultJobId;
				}

				EphemeralRenderCacheGuard( const EphemeralRenderCacheGuard& ) = delete;
				EphemeralRenderCacheGuard& operator=( const EphemeralRenderCacheGuard& ) = delete;

			private:
				std::mutex&                 mSessionMutex;
				AgentImageCache&            mImageCache;
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
			// caches every success into the image cache for ReadImage().
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
				EphemeralRenderCacheGuard cacheGuard( mAsyncCacheMutex, *mImageCache,
				                                      mLastAsyncRenderResult, mLastAsyncRenderResultJobId );
				// Fix-round-8 P2 seam: the guard's ctor has run (cache and
				// async-result record moved OUT to its stash) and the guarded
				// render has NOT started -- the documented "ONE window reports
				// found == false" property is observable from here, so a test can
				// pin the ctor half -- see ForTest_SetEphemeralCacheGuardOpenHook's
				// doc.  Round-10 P2 correction: the previous wording said "the
				// ONLY place" it is observable.  It is not -- CompareToReference
				// opens the same window with its own EphemeralRenderCacheGuard
				// around the split-mask render.  What is true, and what the
				// header phrases correctly, is that ONE seam is enough to pin the
				// ctor half: the two sites share the guard type, so a test that
				// observes the window here covers the property for both.
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

		// G3b (2026-08-10) `render{target:}` -- the sketch comparison ---------------

		void AgentSession::ApplyTargetComparison_( const AgentRenderParams& params,
		                                            AgentRenderResult& rr,
		                                            bool assumeParked,
		                                            const AgentElementSketch* resolvedTarget )
		{
			// GATED ON ok FROM THE START.  This is G1's fix-round P1 in
			// advance: a block of measured facts attached to a render that
			// failed describes an image that never existed, and this project
			// has measured that models ACT on result facts.  Also a no-op for
			// every render that did not ask for a comparison, so the
			// non-target path is byte-identical to G1's.
			if( params.target.empty() || !rr.ok ) return;

			// G3b fix-round (2026-08-10) FIX 1 -- THE SNAPSHOT, not a
			// re-resolve.  This used to call ResolveTargetSketch a SECOND
			// time (after RenderCore_ had already resolved the same name),
			// which was two bugs in one: an unsynchronized mElementSketches read
			// on the controller's render worker under the async path, and a
			// comparison that would silently describe a DIFFERENT plan from
			// the one the caller submitted against if FileBuildPlan landed in
			// between.  The caller now hands us the copy it resolved on its
			// own thread; the measurement below reads nothing but that copy.
			if( !resolvedTarget ) {
				rr.message += " (target comparison not performed: no resolved sketch snapshot was "
				              "supplied for \"" + params.target + "\" -- internal caller error)";
				return;
			}
			const AgentElementSketch& sketch = *resolvedTarget;
			const std::size_t canvasPixels =
				static_cast<std::size_t>( kElementSketchCanvas ) * static_cast<std::size_t>( kElementSketchCanvas );
			if( sketch.mask.size() != canvasPixels ) {
				rr.message += " (target comparison not performed: the filed sketch for \"" +
					sketch.element + "\" has no rasterized mask)";
				return;
			}

			// ---- the internal identity pass ----
			//
			// SAME `isolate`, SAME `target` (so IsolateVantageOffset resolves
			// the SAME vantage), SAME `camera`/`view`, SAME width/height and
			// SAME fromAgentSurface flag -- copied wholesale from `params` and
			// then narrowed -- so the pose and dims match the render just
			// completed BY CONSTRUCTION rather than by a duplicated
			// calculation that could drift.  `renderTarget` becomes ObjectMap:
			// the identity pipeline is exact (one ray per pixel, flat identity
			// colours, reserved #000000 background), so the mask is the
			// object's true silhouette under EVERY mode and quality -- a
			// beauty-pixel threshold would depend on lighting, materials and
			// the tone curve.  `quality`/`samples`/`perception`/`xray` are
			// narrowed because objectmap ignores all four anyway; narrowing
			// them here keeps this pass from paying for anything it cannot use.
			//
			// When the caller's own render WAS an objectmap, this repeats it.
			// Accepted: an identity pass is the cheap render in the system
			// (~20ms at 256x256 -- see QueryObjectAt's header doc), and reusing
			// the caller's PNG would mean the comparison worked differently
			// depending on which mode was asked for, which is exactly the
			// mode-dependence the identity pass exists to remove.
			AgentRenderParams om = params;
			om.renderTarget = AgentRenderTarget::ObjectMap;
			om.quality      = AgentRenderQuality::Production;
			om.samples      = -1;
			om.perception   = false;
			om.xray         = false;
			// DIMS ARE PINNED TO WHAT THE CALLER'S RENDER ACTUALLY PRODUCED,
			// not copied from `params`.  Copying them would be wrong in two
			// separate ways, and both are silent:
			//   * R1b's absent-dims agent default is gated on
			//     `isProductionBeauty` (see wantsAgentDefaultResolutionCap),
			//     so an agent-surface beauty render with no width/height
			//     renders CAPPED at 256 while an objectmap pass with the same
			//     absent dims renders at the scene's full authored Film -- a
			//     400x300 identity pass behind a 256x192 beauty one.
			//   * a BeautyVariant mode renders at a FIXED reduced resolution
			//     (quarter/half) that `params` never mentions at all.
			// `rr.width`/`rr.height` are the effective dims after every one of
			// those layers, so pinning to them is the only formulation that
			// means "the same dims" for all of them.  The aspect the framing
			// solves against comes out identical either way (every implicit
			// path preserves the Film's ratio), so this changes cost and
			// fidelity, not the measured shape.
			if( rr.width > 0 && rr.height > 0 ) {
				om.width  = rr.width;
				om.height = rr.height;
			}

			AgentRenderResult omr;
			{
				// The identity frame must NEVER displace the caller's own last
				// render in the image cache -- a `read_image` after a target
				// comparison would otherwise hand back a flat segmentation
				// image of one object.  Same hazard, same guard, as
				// QueryObjectAt's and CompareToReference's identity passes; see
				// EphemeralRenderCacheGuard's doc for the deadlock / refcount /
				// zero-byte-window constraints it encodes.
				EphemeralRenderCacheGuard cacheGuard( mAsyncCacheMutex, *mImageCache,
				                                      mLastAsyncRenderResult, mLastAsyncRenderResultJobId );
				// G3b fix-round (2026-08-10) FIX 1: the nested identity pass
				// gets the SAME snapshot -- it must resolve to the SAME
				// vantage, and it must not re-read mElementSketches either.
				omr = RenderCore_( om, assumeParked, /*forcedJobId=*/0, resolvedTarget );
			}
			if( !omr.ok ) {
				rr.message += " (target comparison not performed: the internal identity render failed -- " +
					( omr.message.empty() ? std::string( "no reason reported" ) : omr.message ) + ")";
				return;
			}

			std::vector<unsigned char> silRaw;
			unsigned int silW = 0, silH = 0;
			if( !DecodePngSilhouetteMask_( omr.png, silRaw, silW, silH ) ) {
				rr.message += " (target comparison not performed: the internal identity render's image "
				              "could not be decoded)";
				return;
			}

			// Both masks now go through SketchFitTransform_ -- the ONE fit the
			// sketch rasterizer used -- so the IoU below is a SHAPE match:
			// invariant to where the part sits in the frame and to how big it
			// is, which is deliberate (place is not what a sketch encodes).
			std::vector<unsigned char> silCanvas, silMirrored;
			std::size_t silFilled = 0, mirroredFilled = 0;
			unsigned int silBBoxW = 0, silBBoxH = 0, mirBBoxW = 0, mirBBoxH = 0;
			const bool haveSilhouette = NormalizeSilhouetteToCanvas_(
				silRaw, silW, silH, /*mirrorX=*/false, silCanvas, silFilled, silBBoxW, silBBoxH );
			NormalizeSilhouetteToCanvas_(
				silRaw, silW, silH, /*mirrorX=*/true, silMirrored, mirroredFilled, mirBBoxW, mirBBoxH );

			rr.targetElement                    = sketch.element;
			rr.targetView                    = sketch.view;
			rr.targetIou                     = MaskIoU_( sketch.mask, silCanvas );
			rr.targetMirroredIou             = MaskIoU_( sketch.mask, silMirrored );
			rr.targetSketchAreaFraction      = sketch.areaFraction;
			rr.targetSilhouetteAreaFraction  =
				static_cast<double>( silFilled ) / static_cast<double>( canvasPixels );
			rr.targetSketchAspect            = sketch.aspect;
			// -1.0 (omitted on the wire) when nothing rendered inside the
			// frame: an aspect of an empty box is not a measurement.
			rr.targetSilhouetteAspect        = ( haveSilhouette && silBBoxH > 0 )
				? ( static_cast<double>( silBBoxW ) / static_cast<double>( silBBoxH ) ) : -1.0;
			// The billboard fact: smallest 3D bbox extent over the largest.
			// Same -1.0-means-not-computed convention as isolateBBoxCoverage,
			// and it rides the SAME isolateBBoxUsable gate G1 established --
			// an unusable box must not produce a ratio out of clamped zeros.
			if( rr.isolateBBoxUsable ) {
				double ext[3];
				for( int a = 0; a < 3; ++a ) ext[a] = rr.isolateBBoxMax[a] - rr.isolateBBoxMin[a];
				double lo = ext[0], hi = ext[0];
				for( int a = 1; a < 3; ++a ) {
					if( ext[a] < lo ) lo = ext[a];
					if( ext[a] > hi ) hi = ext[a];
				}
				if( hi > 0.0 ) rr.targetThinnestAxisRatio = lo / hi;
			}

			rr.targetCompositePng = BuildTargetComparisonPng_( sketch.mask, silCanvas,
			                                                    rr.targetCompositeWidth,
			                                                    rr.targetCompositeHeight );
			rr.targetApplied = true;

			// The note.  NUMBERS ONLY -- no threshold, no verdict, no advice.
			// A word like "close" or "poor" here would be the harness grading
			// the model's imagination, which the design forbids outright
			// (docs/agentic-redesign/77-imagination-target-design.md sec 5.4:
			// the number is reported and never characterized).
			std::string note = " (target \"" + rr.targetElement + "\": sketch view " + rr.targetView +
				", rendered from the " + ( rr.targetVantage.empty() ? std::string( "unrecorded" )
				                                                    : rr.targetVantage ) +
				" vantage; iou " + SketchFact3dp_( rr.targetIou ) +
				", mirroredIou " + SketchFact3dp_( rr.targetMirroredIou ) +
				"; sketch area " + SketchFact3dp_( rr.targetSketchAreaFraction ) +
				", silhouette area " + SketchFact3dp_( rr.targetSilhouetteAreaFraction ) +
				"; sketch aspect " + SketchFact2dp_( rr.targetSketchAspect );
			if( rr.targetSilhouetteAspect >= 0.0 )
				note += ", silhouette aspect " + SketchFact2dp_( rr.targetSilhouetteAspect );
			else
				note += ", silhouette aspect not measured -- no pixel of the object landed in the frame";
			if( rr.targetThinnestAxisRatio >= 0.0 )
				note += "; thinnest/longest 3D bbox axis " + SketchFact3dp_( rr.targetThinnestAxisRatio );
			if( !rr.targetCompositePng.empty() )
				note += "; the image returned with this call is the [sketch | silhouette | overlay] "
				        "composite, not the rendered frame";
			note += ")";
			rr.message += note;
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

			//----------------------------------------------------------------
			// Arc 77 Phase 2 (2026-08-11): the imagined whole-scene target.
			//----------------------------------------------------------------

			//! Decode a PROVIDER-GENERATED image (whatever container it came
			//! in) into the SAME tightly-packed 8-bit RGB representation
			//! DecodeReferencePngToRgb8_ produces, so everything downstream --
			//! the RMSE, the means, the composite -- is one code path
			//! regardless of what the provider sent.
			//!
			//! WHAT WE DID ABOUT NON-PNG, stated plainly because the design
			//! brief asks for it: we STORE AND COMPARE IN THE DECODED DOMAIN.
			//! The container is decoded here through the tree's own readers
			//! (PNG or JPEG -- both already linked, both already used by
			//! png_painter / read_image) and the target is then re-encoded to
			//! PNG exactly once, at imagine time, so the session holds ONE
			//! representation and the inline echo is always a real PNG no
			//! matter which provider produced it.  Dispatch is on the MAGIC
			//! BYTES first and the provider-declared mime only as a
			//! tie-breaker: a provider that mislabels its own payload should
			//! not turn into a decode failure.  Anything that is neither PNG
			//! nor JPEG (WebP, AVIF, ...) fails HONESTLY, naming the declared
			//! mime -- guessing a decoder for an unknown container would turn
			//! a clear provider-format problem into a mysterious one.
			bool DecodeGeneratedImageToRgb8_( const std::vector<unsigned char>& bytes,
			                                  const std::string& mimeType,
			                                  std::vector<unsigned char>& outRgb,
			                                  unsigned int& outW, unsigned int& outH,
			                                  std::string& err )
			{
				outRgb.clear();
				outW = 0;
				outH = 0;
				if( bytes.size() < 4 ) {
					err = "the provider returned " + std::to_string( bytes.size() ) +
						" bytes -- too few to be an image";
					return false;
				}
				const bool pngMagic = bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
				const bool jpegMagic = bytes[0] == 0xFF && bytes[1] == 0xD8;
				const bool wantJpeg = jpegMagic || ( !pngMagic && mimeType == "image/jpeg" );
				if( !pngMagic && !wantJpeg ) {
					err = "the provider returned an image this build cannot decode (declared mime `" +
						( mimeType.empty() ? std::string( "unspecified" ) : mimeType ) +
						"`, and the bytes are neither PNG nor JPEG)";
					return false;
				}

				IMemoryBuffer* buffer = nullptr;
				if( !RISE_API_CreateCompatibleMemoryBuffer( &buffer,
					const_cast<char*>( reinterpret_cast<const char*>( bytes.data() ) ),
					static_cast<unsigned int>( bytes.size() ), /*bTakeOwnership=*/false ) || !buffer )
				{
					err = "could not wrap the generated image bytes in a read buffer";
					return false;
				}

				IRasterImageReader* reader = nullptr;
				// Same colour-space argument as DecodeReferencePngToRgb8_ (see
				// its doc): eColorSpace_Rec709RGB_Linear is the readers'
				// "do nothing" branch, so the stored byte comes back verbatim
				// and the *255 write-back below is an exact round trip.
				const bool made = wantJpeg
					? RISE_API_CreateJPEGReader( &reader, *buffer, eColorSpace_Rec709RGB_Linear )
					: RISE_API_CreatePNGReader( &reader, *buffer, eColorSpace_Rec709RGB_Linear );
				if( !made || !reader ) {
					buffer->release();
					err = wantJpeg ? "could not create a JPEG reader" : "could not create a PNG reader";
					return false;
				}

				unsigned int w = 0, h = 0;
				if( !reader->BeginRead( w, h ) || w == 0 || h == 0 ) {
					reader->EndRead();
					reader->release();
					buffer->release();
					err = "the generated image did not decode (corrupt payload, or zero dimensions)";
					return false;
				}

				outRgb.resize( static_cast<std::size_t>( w ) * h * 3 );
				for( unsigned int y = 0; y < h; ++y ) {
					for( unsigned int x = 0; x < w; ++x ) {
						RISEColor c;
						reader->ReadColor( c, x, y );
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

			//! Box-DOWNSCALE a tightly-packed RGB8 image to `dstW`x`dstH` by
			//! averaging each destination pixel's source footprint.  Callers
			//! only ever ask for dimensions <= the source's in BOTH axes (the
			//! comparison canvas is the per-axis minimum, and the target cap
			//! only ever shrinks), so this never interpolates upward and never
			//! invents detail.  A same-size request is a straight copy.
			//! Deterministic integer footprints, doubles only for the average.
			bool BoxDownscaleRgb8_( const std::vector<unsigned char>& src,
			                        unsigned int srcW, unsigned int srcH,
			                        unsigned int dstW, unsigned int dstH,
			                        std::vector<unsigned char>& dst )
			{
				dst.clear();
				if( srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0 ) return false;
				if( src.size() != static_cast<std::size_t>( srcW ) * srcH * 3 ) return false;
				if( dstW > srcW || dstH > srcH ) return false;
				if( dstW == srcW && dstH == srcH ) { dst = src; return true; }

				dst.assign( static_cast<std::size_t>( dstW ) * dstH * 3, 0 );
				for( unsigned int dy = 0; dy < dstH; ++dy ) {
					// Half-open source row span [y0,y1); the +1 guard keeps a
					// span non-empty when the ratio rounds two destination rows
					// onto the same source row boundary.
					unsigned int y0 = static_cast<unsigned int>(
						( static_cast<std::uint64_t>( dy ) * srcH ) / dstH );
					unsigned int y1 = static_cast<unsigned int>(
						( static_cast<std::uint64_t>( dy + 1 ) * srcH ) / dstH );
					if( y1 <= y0 ) y1 = y0 + 1;
					if( y1 > srcH ) y1 = srcH;
					for( unsigned int dx = 0; dx < dstW; ++dx ) {
						unsigned int x0 = static_cast<unsigned int>(
							( static_cast<std::uint64_t>( dx ) * srcW ) / dstW );
						unsigned int x1 = static_cast<unsigned int>(
							( static_cast<std::uint64_t>( dx + 1 ) * srcW ) / dstW );
						if( x1 <= x0 ) x1 = x0 + 1;
						if( x1 > srcW ) x1 = srcW;
						std::uint64_t sr = 0, sg = 0, sb = 0, n = 0;
						for( unsigned int sy = y0; sy < y1; ++sy ) {
							for( unsigned int sx = x0; sx < x1; ++sx ) {
								const std::size_t i = ( static_cast<std::size_t>( sy ) * srcW + sx ) * 3;
								sr += src[i + 0];
								sg += src[i + 1];
								sb += src[i + 2];
								++n;
							}
						}
						if( n == 0 ) continue;
						const std::size_t o = ( static_cast<std::size_t>( dy ) * dstW + dx ) * 3;
						dst[o + 0] = static_cast<unsigned char>( ( sr + n / 2 ) / n );
						dst[o + 1] = static_cast<unsigned char>( ( sg + n / 2 ) / n );
						dst[o + 2] = static_cast<unsigned char>( ( sb + n / 2 ) / n );
					}
				}
				return true;
			}

			//! Encode a tightly-packed RGB8 buffer through the SAME
			//! linear-passthrough PNG writer compare_to_reference's composite
			//! uses, so byte N in is byte N out (no re-gamma).
			std::vector<unsigned char> EncodeRgb8Png_( const std::vector<unsigned char>& rgb,
			                                           unsigned int w, unsigned int h )
			{
				if( w == 0 || h == 0 ||
					rgb.size() != static_cast<std::size_t>( w ) * h * 3 )
					return std::vector<unsigned char>();
				std::vector<RISEColor> pels( static_cast<std::size_t>( w ) * h );
				for( std::size_t i = 0; i < pels.size(); ++i ) {
					pels[i] = RISEColor( rgb[i * 3 + 0] / 255.0, rgb[i * 3 + 1] / 255.0,
					                     rgb[i * 3 + 2] / 255.0, 1.0 );
				}
				return EncodeLinearPassthroughPng_( pels, w, h );
			}

			//! Arc 77 Phase 2b (2026-08-11): the dimensions
			//! InMemoryRasterizerOutput::ToPngDownscaled would produce for a
			//! `srcW`x`srcH` frame at `maxEdge` -- the SAME never-upscale rule,
			//! the SAME single scale factor applied to both axes with the SAME
			//! std::round.  Duplicated (not called) because that function
			//! encodes a PNG from the live sink, while the scene-target
			//! composite must size a tile cut from THIS render's own bytes; the
			//! two must agree exactly, which is what makes "the render is never
			//! shown smaller than it would have been without a scene target" a
			//! provable statement rather than an intention.  A test pins the
			//! equality against a real ReadImage(maxEdge) call.
			void FrameDimsAtMaxEdge_( unsigned int srcW, unsigned int srcH, unsigned int maxEdge,
			                          unsigned int& outW, unsigned int& outH )
			{
				outW = srcW;
				outH = srcH;
				if( srcW == 0 || srcH == 0 || maxEdge == 0 ) return;
				const unsigned int longEdge = ( srcW >= srcH ) ? srcW : srcH;
				if( longEdge <= maxEdge ) return;
				const double scale = static_cast<double>( maxEdge ) / static_cast<double>( longEdge );
				outW = static_cast<unsigned int>( std::round( scale * srcW ) );
				outH = static_cast<unsigned int>( std::round( scale * srcH ) );
				if( outW < 1 ) outW = 1;
				if( outH < 1 ) outH = 1;
			}
		}

		//! R1 fix round (2026-08-09) -- supervisor decision, recorded here so a future pass doesn't "close
		//! the gap" by mistake: this comparison render's RESOLUTION is deliberately NEVER capped by the
		//! agent-surface resolution axis (kAgentSurfaceMaxRenderEdge) -- see `rparams.width`/`rparams.height`
		//! being forced to the reference image's own `refW`/`refH` a few lines below.  RMSE comparison
		//! REQUIRES the comparison render and the reference to share exact dimensions (see the defensive
		//! `candW != refW || candH != refH` guard further down), and the reference's dims are HOST-registered
		//! by the eval author via whatever attached the reference image, never model-chosen -- capping
		//! resolution here would just break the feature for no honesty gain (an agent can't inflate cost by
		//! picking a large reference; it isn't the one choosing the dims).  The SAMPLES axis, by contrast, IS
		//! capped -- at the RPC layer (AgentRpc.cpp's `compare_to_reference` handler clamps an explicit
		//! `samples` to [1,kAgentSurfaceMaxSamples] before it ever reaches this function; see the comment
		//! there for the full rationale), so an agent-driven comparison render still can't run away on sample
		//! cost even though its resolution is unbounded.
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
				// success into the image cache for ReadImage().  Left
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
					EphemeralRenderCacheGuard cacheGuard( mAsyncCacheMutex, *mImageCache,
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

		// Arc 77 Phase 2 (2026-08-11): imagine_scene + the whole-scene ----------
		// target comparison.  See the public block above ImagineScene in
		// AgentSession.h for the mechanism and the three contracts
		// (capability-conditional, replace-on-re-imagine, anti-stranding).

		AgentSession::AgentImagineResult AgentSession::ImagineScene( const std::string& description )
		{
			AgentImagineResult out;
			out.description  = description;
			out.providerName = mImageGenerator.providerName;
			out.modelId      = mImageGenerator.modelId;

			// An empty description never reaches here from the wire (AgentRpc
			// answers it with a -32602, which by design disarms nothing).
			// A direct C++ caller gets the same refusal, and -- like the
			// -32602 -- it changes no session state whatsoever.
			if( description.empty() ) {
				out.message = "imagine_scene: `description` must be a non-empty string. Nothing was "
					"generated and this session's scene target is unchanged.";
				return out;
			}

			// CAPABILITY REFUSAL.  Honest, and deliberately NOT a disarm: the
			// imagine half of the gate was never armed on a provider that
			// cannot generate images (see ImagineRequirementActive_), so there
			// is nothing here to disarm and reporting one would be a false
			// statement about the session's own state.  The tool stays in the
			// shared table for every provider on purpose -- a per-provider
			// tool table would fork the one definition list every codec maps
			// from -- so this answer is a first-class outcome, not an error.
			if( !ImagineCapable() ) {
				out.capabilityRefusal = true;
				const std::string who = mImageGenerator.providerName.empty()
					? std::string( "this session's provider" )
					: ( "`" + mImageGenerator.providerName + "`" );
				out.message = "imagine_scene is not available: " + who + " does not generate images "
					"through this build, so there is no way to turn your description into a picture "
					"here. Nothing was generated and this session has no scene target; renders will "
					"carry no comparison. Build and look as you normally would -- nothing else about "
					"this session changes, and no other call is blocked by this.";
				return out;
			}

			// PER-SESSION SPEND CAP (Phase 2 review round, P2-2).  imagine_scene
			// is the first read-safe verb whose cost is real, billed provider
			// money rather than local compute, and the interactive surface has
			// no call budget (only eval runs have maxToolCalls).  A stuck retry
			// loop could otherwise spend without bound.  Cap the number of
			// calls that REACH the generator; capability/schema refusals above
			// never count.  Cap-hit cannot strand the gate: reaching it means
			// kSceneImagineMaxPerSession earlier calls reached the generator,
			// and each either produced a target (gate satisfied) or failed at
			// the provider (which disarmed the imagine requirement on the
			// first failure) -- so by this point the imagine half is always
			// already settled.  The refusal states the cap factually at the
			// moment it matters; the tool description does not pre-advertise
			// it (it promises nothing about call counts either way).
			if( mSceneImagineCalls >= kSceneImagineMaxPerSession ) {
				char capBuf[224];
				std::snprintf( capBuf, sizeof( capBuf ),
					"imagine_scene has already generated %d images this session -- the per-session "
					"image-generation cap. Nothing was generated; this session's scene target is "
					"unchanged.", kSceneImagineMaxPerSession );
				out.message = capBuf;
				return out;
			}
			++mSceneImagineCalls;

			const AgentImageGenOutcome gen = mImageGenerator.generate( description );

			// PROVIDER FAILURE -> DISARM.  The one rule that keeps a network
			// blip from stranding a session: a transport error, an HTTP
			// status, a quota, a missing key or a malformed response all drop
			// the imagine REQUIREMENT for the rest of the session, so the gate
			// falls back to plan-only rather than refusing geometry forever
			// over something the model cannot fix.  The 3-refusal give-up
			// still bounds everything on top of this.  `error` is the
			// transport's HEADER-FREE category (never the request, never the
			// key, never the response body) -- see ChatHttpTransport.h.
			if( !gen.ok || gen.bytes.empty() ) {
				mImagineRequirementDisarmed = true;
				out.requirementDisarmed = true;
				out.message = "imagine_scene failed: " +
					( gen.error.empty() ? std::string( "the provider returned no image and no reason" )
					                    : gen.error ) +
					". This session's scene target is unchanged" +
					( mSceneTarget ? " (the one imagined earlier still stands)" : " (there is none)" ) +
					". Because the failure was on the provider side, the requirement to imagine a scene "
					"is now dropped for this session -- no call will be blocked for the lack of a scene "
					"target. You may call imagine_scene again if you want to retry.";
				return out;
			}

			// Decode, cap and RE-ENCODE, so the session holds exactly one
			// representation of the target regardless of what the provider
			// sent (see DecodeGeneratedImageToRgb8_'s doc for the non-PNG
			// decision).  A decode failure is a PROVIDER failure too -- the
			// bytes arrived but are unusable -- and disarms on the same rule.
			std::vector<unsigned char> rgb;
			unsigned int w = 0, h = 0;
			std::string derr;
			if( !DecodeGeneratedImageToRgb8_( gen.bytes, gen.mimeType, rgb, w, h, derr ) ) {
				mImagineRequirementDisarmed = true;
				out.requirementDisarmed = true;
				out.message = "imagine_scene failed: " + derr +
					". This session's scene target is unchanged. Because the failure was on the "
					"provider side, the requirement to imagine a scene is now dropped for this "
					"session -- no call will be blocked for the lack of a scene target.";
				return out;
			}

			// Cap the LONG edge (see kSceneTargetMaxEdge): a provider image
			// arrives at 1024+ and every agent-surface render is capped at
			// 256, so the full-size original could never widen the comparison
			// canvas -- it would only inflate the inline echo.
			unsigned int storeW = w, storeH = h;
			const unsigned int longEdge = ( w > h ) ? w : h;
			if( longEdge > kSceneTargetMaxEdge ) {
				const double s = static_cast<double>( kSceneTargetMaxEdge ) / static_cast<double>( longEdge );
				storeW = static_cast<unsigned int>( static_cast<double>( w ) * s );
				storeH = static_cast<unsigned int>( static_cast<double>( h ) * s );
				if( storeW == 0 ) storeW = 1;
				if( storeH == 0 ) storeH = 1;
				std::vector<unsigned char> scaled;
				if( BoxDownscaleRgb8_( rgb, w, h, storeW, storeH, scaled ) ) {
					rgb.swap( scaled );
				}
				else {
					// Keep the full-size image rather than losing the target
					// over a resample that should not fail; the comparison
					// downscales per render anyway.
					storeW = w;
					storeH = h;
				}
			}

			std::vector<unsigned char> png = EncodeRgb8Png_( rgb, storeW, storeH );
			if( png.empty() ) {
				// A HOST-side encode failure, not a provider one -- so it does
				// NOT disarm the requirement (nothing about the provider is
				// broken) and the previous target, if any, stands.
				out.message = "imagine_scene: the provider returned an image but this build could not "
					"re-encode it for return. This session's scene target is unchanged. Call "
					"imagine_scene again to retry.";
				return out;
			}

			// COMMIT.  Build the whole object first and publish it by
			// REPLACING the pointer -- the pointee is never mutated after
			// this, which is what makes a render's snapshot safe across a
			// later re-imagine.
			std::shared_ptr<AgentSceneTarget> next( new AgentSceneTarget() );
			next->description    = description;
			next->rgb            = rgb;
			next->width          = storeW;
			next->height         = storeH;
			next->png            = png;
			next->providerName   = mImageGenerator.providerName;
			next->modelId        = mImageGenerator.modelId;
			next->sourceMimeType = gen.mimeType;

			out.replacedPreviousTarget = ( mSceneTarget != nullptr );
			mSceneTarget = next;

			out.ok     = true;
			out.width  = storeW;
			out.height = storeH;
			out.png    = png;

			// The echo: FACTS ONLY.  It states what was generated and what
			// will now happen automatically, and says nothing at all about
			// whether the image is any good -- characterizing the model's own
			// imagination is exactly what this mechanism must not do (design
			// doc sec 5.4).
			out.message = "scene imagined: " + mImageGenerator.providerName + "/" +
				mImageGenerator.modelId + " generated one " + std::to_string( storeW ) + "x" +
				std::to_string( storeH ) + " image from your description, returned with this call and "
				"held as this session's scene target. From now on every full-frame production render "
				"(not draft, not a mode: render, not an isolate render) reports `sceneTarget`, and any "
				"such render you ask an image of shows this target ABOVE your render in one picture, at "
				"the same render size you would have got anyway. There is no score and nothing is "
				"gated: look at the two and decide for yourself what to change. Calling imagine_scene "
				"again replaces this target.";
			if( out.replacedPreviousTarget )
				out.message += " This replaced the scene target imagined earlier in this session.";
			return out;
		}

		void AgentSession::ApplySceneTargetComparison_(
			const AgentRenderParams& params,
			AgentRenderResult& rr,
			const std::shared_ptr<const AgentSceneTarget>& target )
		{
			// THE QUALIFICATION RULE, in one place.  See
			// AgentRenderResult::sceneTargetApplied for why each exclusion is
			// an honesty requirement rather than caution.  Every term is a
			// pure function of `params` or of the completed result, so this
			// cannot disagree with what actually rendered.
			if( !target || !rr.ok ) return;
			if( params.quality == AgentRenderQuality::Draft ) return;
			if( params.renderTarget != AgentRenderTarget::Beauty ) return;
			if( !params.isolate.empty() || rr.isolateApplied ) return;
			// A part-sketch comparison already owns this call's image and its
			// own criterion; two comparisons on one render would also mean two
			// png_base64 writes (see AgentRpc.cpp's exactly-one discipline).
			if( !params.target.empty() || rr.targetApplied ) return;
			if( rr.png.empty() ) return;
			if( target->width == 0 || target->height == 0 ||
				target->rgb.size() != static_cast<std::size_t>( target->width ) * target->height * 3 )
				return;

			// THE FACTS, and there are only these.  Phase 2b deleted the RMSE
			// and the six per-channel means that used to be computed here --
			// see AgentRenderResult::sceneTargetApplied for the measured
			// reason (a tone metric fed a tone-chasing edit loop and produced
			// a worse picture).  What is left cannot be chased: a target
			// exists, and it is this many pixels.  Do not add a number back.
			rr.sceneTargetApplied = true;
			rr.sceneTargetWidth   = target->width;
			rr.sceneTargetHeight  = target->height;

			// No inline image was requested, so there is nothing to compose:
			// the caller gets the facts and, exactly as before this mechanism
			// existed, no bytes.
			if( params.imageMaxEdge == 0 ) return;

			std::vector<unsigned char> renderRgb;
			unsigned int rw = 0, rh = 0;
			std::string derr;
			if( !DecodeReferencePngToRgb8_( rr.png.data(), rr.png.size(), renderRgb, rw, rh, derr ) ) {
				rr.message += " (scene target: the composite was not built -- this render's image could "
				              "not be decoded: " + derr + ")";
				return;
			}

			// THE RENDER TILE IS THE WHOLE POINT OF PHASE 2b.  It is sized by
			// the same rule ReadImage(imageMaxEdge) applies to the very same
			// frame, so the render inside the composite is pixel-for-pixel as
			// large as the plain frame this call would have returned with no
			// scene target at all.  (The averaging differs in the last bit --
			// ReadImage box-filters the sink's linear samples, this filters the
			// encoded sRGB bytes -- but the DIMENSIONS, which is what the
			// contract is about, are identical by construction.)
			unsigned int tileW = 0, tileH = 0;
			FrameDimsAtMaxEdge_( rw, rh, params.imageMaxEdge, tileW, tileH );
			std::vector<unsigned char> tile;
			if( tileW == 0 || tileH == 0 ||
				!BoxDownscaleRgb8_( renderRgb, rw, rh, tileW, tileH, tile ) ) {
				rr.message += " (scene target: the composite was not built -- this render's image could "
				              "not be fitted to the requested size)";
				return;
			}

			// THE TARGET BAND, above.  Scaled to the tile's width when it is
			// wider, and NEVER upscaled when it is narrower -- a narrower
			// target is centred at its own size on a black band, keeping the
			// "no interpolation invents detail" rule this mechanism has held
			// since Phase 2.
			unsigned int bandW = target->width;
			unsigned int bandH = target->height;
			if( bandW > tileW ) {
				const double s = static_cast<double>( tileW ) / static_cast<double>( bandW );
				bandW = tileW;
				bandH = static_cast<unsigned int>( std::round( s * static_cast<double>( target->height ) ) );
				if( bandH < 1 ) bandH = 1;
			}
			std::vector<unsigned char> band;
			if( !BoxDownscaleRgb8_( target->rgb, target->width, target->height, bandW, bandH, band ) ) {
				rr.message += " (scene target: the composite was not built -- the target could not be "
				              "fitted to this render's width)";
				return;
			}

			// COMPOSE: target on top, a thin grey rule, render below.  Stacked
			// rather than side by side precisely so the render tile keeps the
			// full width; the rule exists because a dark target edge above a
			// dark render edge is otherwise one continuous image.
			{
				const unsigned int kRule = 2;
				const unsigned int compW = tileW;
				const unsigned int compH = bandH + kRule + tileH;
				std::vector<RISEColor> pels( static_cast<std::size_t>( compW ) * compH,
				                             RISEColor( 0.0, 0.0, 0.0, 1.0 ) );
				const unsigned int bandX = ( tileW - bandW ) / 2;
				for( unsigned int y = 0; y < bandH; ++y ) {
					for( unsigned int x = 0; x < bandW; ++x ) {
						const std::size_t i = ( static_cast<std::size_t>( y ) * bandW + x ) * 3;
						pels[ static_cast<std::size_t>( y ) * compW + bandX + x ] =
							RISEColor( band[i+0] / 255.0, band[i+1] / 255.0, band[i+2] / 255.0, 1.0 );
					}
				}
				for( unsigned int y = 0; y < kRule; ++y ) {
					for( unsigned int x = 0; x < compW; ++x ) {
						pels[ static_cast<std::size_t>( bandH + y ) * compW + x ] =
							RISEColor( 0.5, 0.5, 0.5, 1.0 );
					}
				}
				for( unsigned int y = 0; y < tileH; ++y ) {
					for( unsigned int x = 0; x < tileW; ++x ) {
						const std::size_t i = ( static_cast<std::size_t>( y ) * tileW + x ) * 3;
						pels[ static_cast<std::size_t>( bandH + kRule + y ) * compW + x ] =
							RISEColor( tile[i+0] / 255.0, tile[i+1] / 255.0, tile[i+2] / 255.0, 1.0 );
					}
				}
				rr.sceneTargetCompositePng = EncodeLinearPassthroughPng_( pels, compW, compH );
				if( !rr.sceneTargetCompositePng.empty() ) {
					rr.sceneTargetCompositeWidth  = compW;
					rr.sceneTargetCompositeHeight = compH;
				}
			}

			// The note.  FACTS ONLY -- and now not even a number to
			// characterize, which is the point.
			if( !rr.sceneTargetCompositePng.empty() ) {
				rr.message += " (scene target: the image returned with this call is your imagined scene "
					"above this render, separated by a grey rule -- the render half is the same size the "
					"frame alone would have been. Look at the two and decide for yourself what to change; "
					"read_image returns this render's own frame.)";
			}
			else {
				rr.message += " (scene target: this session has an imagined scene target, but the "
				              "composite could not be encoded, so the plain frame was returned.)";
			}
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

			// ---- G3b fix-round (2026-08-10) FIX 1: SNAPSHOT THE TARGET
			// SKETCH AT SUBMISSION, ON THIS (the caller's) THREAD.
			//
			// THE BUG THIS CLOSES.  The submitted closure below runs on the
			// controller's dedicated render worker thread.  Pre-fix it called
			// RenderCore_ and ApplyTargetComparison_, each of which resolved
			// `params.target` by NAME against mElementSketches -- while
			// FileBuildPlan, on the dispatcher thread, reassigns that very
			// std::vector with no lock.  Concurrent read/write of a vector
			// being reallocated is UB (use-after-free on the mask bytes), and
			// it is REACHABLE: the raw JSON-RPC wire accepts
			// {"async":true,"isolate":X,"target":Y}, and RenderAsync is public
			// C++.  mElementSketches is single-threaded-caller state (see the
			// class contract in AgentSession.h).
			//
			// WHY A SNAPSHOT AND NOT A LOCK.  (a) It closes the race without
			// inventing a lock convention this state has never had, and
			// without refusing a wire shape that is otherwise legal.  (b) It
			// is the CORRECT semantics independently of the race: an async
			// comparison should describe the plan AS IT WAS WHEN THE CALLER
			// SUBMITTED, not whichever plan happens to be filed when the
			// worker finishes.  (c) It removes the redundant double-resolve
			// (RenderCore_ and ApplyTargetComparison_ each resolving the same
			// name).  The copy is ~64KB (a 256x256 byte mask plus small
			// strings) and is taken once, on the submitting thread.
			//
			// An unresolvable `target` FAILS THE SUBMISSION -- accepted=false
			// with the same sentence a synchronous render puts in `message`,
			// which the wire renders as status:"refused" (the same shape the
			// no-controller refusal directly above already uses).  Nothing is
			// queued, so there is no later result to carry the reason.
			AgentElementSketch resolvedTarget;
			bool haveResolvedTarget = false;
			if( !params.target.empty() ) {
				std::string targetError;
				if( !ResolveTargetSketch( params.target, params.isolate, resolvedTarget, targetError ) ) {
					out.accepted = false;
					out.message  = targetError;
					return out;
				}
				haveResolvedTarget = true;
			}

			// Arc 77 Phase 2 (2026-08-11): THE SCENE-TARGET SNAPSHOT, taken
			// HERE on the submitting thread for EXACTLY the reason FIX 1 above
			// takes the sketch snapshot here.  The reachability question was
			// enumerated across all three surfaces before deciding this was
			// needed rather than assumed: the TOOL SCHEMA exposes `async` on
			// `render`; the RAW WIRE accepts {"method":"render","params":
			// {"async":true}} whether or not any schema mentions it; and
			// RenderAsync is PUBLIC C++.  So the worker closure below really
			// can run while the dispatcher thread is inside ImagineScene.
			// Copying the shared_ptr here is a refcount bump (not an image
			// copy), and because ImagineScene REPLACES the pointer instead of
			// mutating the pointee, the worker's copy keeps describing the
			// target the caller submitted against -- the same
			// as-it-was-at-submission semantics FIX 1 established for the plan.
			const std::shared_ptr<const AgentSceneTarget> sceneTargetSnapshot = mSceneTarget;

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
			// (guarded by the image cache's own lock) populates it on a
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

			// Round-10 finding 2b: collect the REPORTED refusal cause rather
			// than inferring it from a status read after the fact.
			SceneEditController::RenderRefusal refusal =
				SceneEditController::RenderRefusal::None;
			const bool accepted = mController->SubmitAgentRenderAsync(
				// G3b fix-round (2026-08-10) FIX 1: `resolvedTarget` /
				// `haveResolvedTarget` ride into the closure BY VALUE -- the
				// worker consumes the submission-time copy and never reads
				// mElementSketches.
				[this, params, ownJobIdCell, resolvedTarget, haveResolvedTarget,
				 sceneTargetSnapshot]() {
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
					const AgentElementSketch* const targetSnapshot =
						haveResolvedTarget ? &resolvedTarget : nullptr;
					AgentRenderResult r = RenderCore_( params, /*assumeParked=*/true,
					                                   /*forcedJobId=*/0, targetSnapshot );
					// G3b (2026-08-10): the async path measures a `target`
					// comparison exactly as the synchronous Render() does --
					// same helper, same one-extra-identity-render mechanism --
					// with assumeParked=true, because this closure is STILL
					// inside the worker's park and a nested re-park would
					// self-deadlock on the controller's non-recursive mMutex
					// (the same reason RenderCore_ itself takes the flag).
					// Placed BEFORE the mLastAsyncRenderResult store below so
					// the cached result a later render_wait echoes carries the
					// target block, not a copy taken before the measurement.
					// A no-op unless `target` was requested AND `r.ok`.
					// G3b fix-round (2026-08-10) FIX 1: measured against the
					// SUBMISSION-TIME snapshot -- the same copy RenderCore_
					// above framed the vantage from -- so a FileBuildPlan that
					// lands while this render is in flight can neither race
					// this read nor silently re-point the comparison at a
					// different sketch.
					ApplyTargetComparison_( params, r, /*assumeParked=*/true, targetSnapshot );
					// Arc 77 Phase 2: the whole-scene comparison, measured
					// against the SUBMISSION-TIME snapshot captured by value
					// above -- this closure never reads mSceneTarget.  Placed
					// before the mLastAsyncRenderResult store below for the
					// same reason the sketch comparison is: the cached result a
					// later render_wait echoes must carry the block.
					ApplySceneTargetComparison_( params, r, sceneTargetSnapshot );
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
				params.pinned,
				SceneEditController::RenderClass::AgentPreview,
				&refusal );

			if( !accepted ) {
				out.accepted = false;
				// Model-B F2 slice S3 distinguished "a PINNED render is
				// occupying the slot" from the generic busy/transaction
				// refusal by reading CurrentRenderJob().pinned here.
				// ROUND-10 finding 2 (P1): that read was STALE, not merely
				// racy.  `pinned` was written at ONE of the three job-record
				// mint sites, so once ANY pinned render had completed the
				// field stayed true for the session's life and this branch
				// announced a pinned render that did not exist.  Both halves
				// are fixed: the record is now published whole (so the field
				// is no longer stale) AND this branch no longer infers --
				// SubmitAgentRenderAsync reports the cause it decided under
				// the slot lock, which no after-the-fact status read can
				// reconstruct anyway.
				//
				// Generic retriable message first, switch narrows it, switch
				// stays TOTAL (no `default:`) so -Wswitch catches an
				// unmapped enumerator at build time -- same discipline as
				// RenderCore_'s two switches.
				out.message = "render refused: the agent-render worker is busy or an editor transaction is open -- retry shortly";
				switch( refusal ) {
				case SceneEditController::RenderRefusal::PinnedRenderBusy:
					out.message = "render refused: a pinned render is in flight -- pinned renders run to completion and are never superseded; retry after it completes";
					break;
				case SceneEditController::RenderRefusal::CoordinatedRenderBusy:
					out.message = "render queued or in progress -- retry after it completes";
					break;
				case SceneEditController::RenderRefusal::ControllerStopped:
					// NOT retriable.
					out.message = "render refused: the editor is shutting down";
					break;
				case SceneEditController::RenderRefusal::EditorBusy:
					out.message = "editor transaction or gesture in progress -- retry after it completes";
					break;
				case SceneEditController::RenderRefusal::InteractionFinalizeFailed:
					out.message = "render refused: an open editor interaction could not be finalized -- retry shortly";
					break;
				case SceneEditController::RenderRefusal::InteractionFinalizeLatched:
					// Round-10 finding 3 -- NOT retriable, and it never clears.
					out.message = "render refused: an editor interaction failed to persist and the editor has LATCHED that failure -- this does NOT clear on its own and retrying will not help; renders and viewport reads stay refused until the scene is reopened";
					break;
				case SceneEditController::RenderRefusal::None:
					// Unreachable on a refusal; listed to keep the switch total.
					break;
				}
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
			// mAsyncCacheMutex still gates the closing check; the cache's own
			// leaf lock is taken INSIDE it (the documented order), which keeps
			// "a closing session reads empty" exactly as atomic as before.
			std::lock_guard<std::mutex> cacheLk( mAsyncCacheMutex );
			if( mSinkReadsClosing ) return std::vector<unsigned char>();
			return mImageCache->Png();
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
					// ONE cache-lock scope so the bytes and the dimensions
					// cannot come from two different renders.
					return mImageCache->PngWithDims( outWidth, outHeight );
				}
				// AcquireSink hands back a REFERENCE, so the sink is already
				// safe from a replacing render before the registry insert
				// below; the try/catch is now only about not leaking that
				// reference if the map allocation throws.
				sink = mImageCache->AcquireSink();
				if( sink ) {
					try {
						++mActiveSinkReadLeases[sink];
					}
					catch( ... ) {
						safe_release( sink );
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
				// See ReadImage(maxEdge): AcquireSink returns an already-held
				// reference, so the only failure to unwind is the map insert.
				sink = mImageCache->AcquireSink();
				if( sink ) {
					try {
						++mActiveSinkReadLeases[sink];
					}
					catch( ... ) {
						safe_release( sink );
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
			SceneEditController::RenderRefusal refusal =
				SceneEditController::RenderRefusal::None;
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
				case SceneEditController::RenderRefusal::CoordinatedRenderBusy:
					outReason = "render_in_progress";
					break;
				case SceneEditController::RenderRefusal::ControllerStopped:
					outReason = "editor_shutting_down";
					break;
				case SceneEditController::RenderRefusal::InteractionFinalizeFailed:
					// Round-10 finding 3: this used to fall through to
					// "editor_transaction_in_progress", which is a different
					// cause -- no transaction need be open for a finalize to
					// fail.  Still retriable, but named for what it is.
					outReason = "editor_interaction_finalize_failed";
					break;
				case SceneEditController::RenderRefusal::InteractionFinalizeLatched:
					// Round-10 finding 3: the LATCHED variant.  It shared
					// "editor_transaction_in_progress" with the cases above,
					// and both AgentSession.h and the model-facing
					// AgentMcpAdapter tool description say that value clears on
					// its own -- so the model was told to retry a gate that can
					// never open again.  Distinct value, documented as
					// permanent, on every surface.
					outReason = "editor_interaction_unrecoverable";
					break;
				case SceneEditController::RenderRefusal::PinnedRenderBusy:
					// Not producible here (RunPreviewRenderParked has no slot
					// concept) -- listed to keep the switch total, and mapped
					// rather than silently reported as a transaction.
					outReason = "render_in_progress";
					break;
				case SceneEditController::RenderRefusal::EditorBusy:
				case SceneEditController::RenderRefusal::None:
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
