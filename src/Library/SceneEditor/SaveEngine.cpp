//////////////////////////////////////////////////////////////////////
//
//  SaveEngine.cpp - Phase 6.4 implementation.
//
//  See docs/ROUND_TRIP_SAVE_PLAN.md §9 for the algorithm spec and
//  every refusal / gate's rationale.  Section numbers are referenced
//  inline below.
//
//////////////////////////////////////////////////////////////////////

#include "SaveEngine.h"

#include "DirtyTracker.h"
#include "OverrideSpanIndex.h"
#include "SourceSpanIndex.h"
#include "TransformSnapshot.h"
#include "../Interfaces/IJobPriv.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IObjectPriv.h"
#include "../Utilities/Log/Log.h"
#include "../Utilities/Math3D/Math3D.h"
// Phase B: property-pass introspection + manager lookups.
#include "CameraIntrospection.h"
#include "LightIntrospection.h"
#include "MaterialIntrospection.h"
#include "MediaIntrospection.h"
#include "ObjectIntrospection.h"
#include "../Interfaces/ICameraManager.h"
#include "../Interfaces/ILightManager.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IMaterialManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <sys/stat.h>

// POSIX-only headers + helpers used by the optional fsync hardening
// path in AtomicWrite.  On Windows we fall back to std::ofstream +
// std::filesystem::rename without the fsync step — the rename is
// still atomic on NTFS via ReplaceFile semantics (std::filesystem
// uses MoveFileEx with MOVEFILE_REPLACE_EXISTING under the hood).
// `<process.h>` is the Windows header that declares `_getpid`; on
// POSIX `getpid` is in `<unistd.h>`.
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace RISE
{

namespace {

// ----------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------

// Aliases for the shared canonical sentinels (defined in
// OverrideSpanIndex.h so the parser and save engine match exactly).
constexpr const char* kSentinelOpen  = kManagedBlockSentinelOpen;
constexpr const char* kSentinelClose = kManagedBlockSentinelClose;

// One edit operation against ORIGINAL file bytes (R5 §2 / pinned 2.21).
// Sorted descending by `begin` and applied at step 7; higher-`begin`
// edits get applied first so their length deltas don't shift lower-
// `begin` edits' input ranges.
struct EditOp
{
    std::size_t begin;
    std::size_t end;
    std::string replacement;
};

// §9.4 MatricesNearEqual: mixed absolute + relative tolerance.
bool MatricesNearEqual( const Matrix4& a, const Matrix4& b, double tol = 1e-9 )
{
    // Matrix4 fields are _<col><row>.  Walk all 16 cells.
    const double cells_a[16] = {
        a._00, a._01, a._02, a._03,
        a._10, a._11, a._12, a._13,
        a._20, a._21, a._22, a._23,
        a._30, a._31, a._32, a._33 };
    const double cells_b[16] = {
        b._00, b._01, b._02, b._03,
        b._10, b._11, b._12, b._13,
        b._20, b._21, b._22, b._23,
        b._30, b._31, b._32, b._33 };
    for( std::size_t k = 0; k < 16; ++k ) {
        const double ax = std::fabs( cells_a[k] );
        const double bx = std::fabs( cells_b[k] );
        const double scale = std::max( 1.0, std::max( ax, bx ) );
        if( std::fabs( cells_a[k] - cells_b[k] ) > tol * scale ) {
            return false;
        }
    }
    return true;
}

// §9.5 TryDecompose: try to express M as T·R·S.  Returns ok=false if
// the matrix has shear, negative determinant, or near-zero scale.
struct Decomposition
{
    bool    ok = false;
    Vector3 position;
    Vector3 orientationDeg;  // Euler XYZ in DEGREES
    Vector3 scale;
};

Decomposition TryDecompose( const Matrix4& M )
{
    Decomposition out;

    // ---- Affine check ------------------------------------------------
    // RISE matrices fed by FinalizeTransformations are always affine
    // (bottom row = [0 0 0 1]).  If a future projective input reaches
    // here, refuse decomposition rather than silently fabricate T·R·S.
    if( std::fabs( M._03 ) > 1e-9 || std::fabs( M._13 ) > 1e-9 ||
        std::fabs( M._23 ) > 1e-9 || std::fabs( M._33 - 1.0 ) > 1e-9 ) {
        out.ok = false;
        return out;
    }

    // ---- Translation (column 3) -------------------------------------
    out.position = Vector3( M._30, M._31, M._32 );

    // ---- Upper-left 3×3 = R·S, viewed as three column vectors --------
    // RISE stores `_<col><row>`, so column 0 of the affine block is
    // (M._00, M._01, M._02) and so on (verified against
    // Job::AddObjectMatrix, Job.cpp:5103-5107).
    Vector3 c0( M._00, M._01, M._02 );
    Vector3 c1( M._10, M._11, M._12 );
    Vector3 c2( M._20, M._21, M._22 );

    const double s0 = Vector3Ops::Magnitude( c0 );
    const double s1 = Vector3Ops::Magnitude( c1 );
    const double s2 = Vector3Ops::Magnitude( c2 );
    if( s0 < 1e-6 || s1 < 1e-6 || s2 < 1e-6 ) {
        // Near-zero scale on any axis — decomposition is degenerate
        // / numerically unstable.  Threshold tightened (P2-B): use
        // 1e-6 to match the orthonormality tolerance, eliminating
        // the band where 1e-9 < |scale| < 1e-6 would pass the
        // singularity check but produce unstable normalised columns.
        out.ok = false;
        return out;
    }
    out.scale = Vector3( s0, s1, s2 );

    // Normalised columns → candidate rotation R.
    Vector3 r0( c0.x / s0, c0.y / s0, c0.z / s0 );
    Vector3 r1( c1.x / s1, c1.y / s1, c1.z / s1 );
    Vector3 r2( c2.x / s2, c2.y / s2, c2.z / s2 );

    // Orthonormality check.
    const double tol = 1e-6;
    if( std::fabs( Vector3Ops::Dot( r0, r1 ) ) > tol ||
        std::fabs( Vector3Ops::Dot( r0, r2 ) ) > tol ||
        std::fabs( Vector3Ops::Dot( r1, r2 ) ) > tol ) {
        out.ok = false;
        return out;
    }
    // Reflection check: reject negative determinant (handedness flip).
    const Vector3 cr = Vector3Ops::Cross( r1, r2 );
    if( Vector3Ops::Dot( r0, cr ) < 0.0 ) {
        out.ok = false;
        return out;
    }

    // ---- Euler extraction --------------------------------------------
    //
    // RISE composes orientation as P · O · S where
    //   O = XRotation(x) * YRotation(y) * ZRotation(z)
    // (see Transformable.cpp:108-110).  Matrix multiplication is
    // standard column-major, so for the resulting R (and the
    // normalised columns above):
    //   R[r][c]  =  M._<c><r>
    //
    // Working out the product XRotation·YRotation·ZRotation analytically
    // is one source of bugs (reviewer P1-A caught a sign-error in the
    // hand-derived formulas).  Instead, derive ONLY the angles via a
    // canonical asin/atan2 chain, then VERIFY by building a candidate
    // matrix the SAME way RISE does it (Matrix4Ops::XRotation/YRotation/
    // ZRotation) and comparing the whole 4×4 via MatricesNearEqual.
    // This keeps the round-trip check in lock-step with RISE's
    // convention — if SetOrientation ever changes order, the
    // verification catches the regression rather than hiding it.
    //
    // For Rx · Ry · Rz applied to a column vector v, the (0,2) cell
    // works out to -sin(y) regardless of x, z — that's the canonical
    // place to extract y.  V1 takes the obvious atan2 chain; gimbal-
    // lock returns ok=false (matrix override is the safer fallback).
    //
    // Direct cell access: row 0 col 2 → r2.x (column 2's first
    // element).  We don't assume any particular sign on R[0][2] here
    // — atan2 + the rebuild-and-compare verification handles both
    // conventions correctly.
    const double R02 = r2.x;       // R[row=0][col=2]
    const double R12 = r2.y;       // R[row=1][col=2]
    const double R22 = r2.z;       // R[row=2][col=2]
    const double R01 = r1.x;       // R[row=0][col=1]
    const double R00 = r0.x;       // R[row=0][col=0]

    // For RISE's Rx·Ry·Rz convention (Transformable.cpp:108-110), the
    // matrix product worked out cell-by-cell gives:
    //   R[0][0] = +cos(y)·cos(z)
    //   R[0][1] = -cos(y)·sin(z)
    //   R[0][2] = +sin(y)
    //   R[1][2] = -sin(x)·cos(y)
    //   R[2][2] = +cos(x)·cos(y)
    // So we extract:
    //   y = atan2(+R02, cos_y)
    //   x = atan2(-R12, +R22)   [atan2 cancels cos(y)]
    //   z = atan2(-R01, +R00)
    // Whichever direction the signs land, the post-rebuild
    // MatricesNearEqual catches any disagreement and returns ok=false
    // — that's the safety net (P1-A).
    const double sin_y = R02;
    if( std::fabs( sin_y ) >= 1.0 - 1e-9 ) {
        // Gimbal-lock — fall back to matrix override.
        out.ok = false;
        return out;
    }
    const double cos_y = std::sqrt( 1.0 - sin_y * sin_y );
    const double y_rad = std::atan2( sin_y, cos_y );

    const double x_rad = std::atan2( -R12, R22 );
    const double z_rad = std::atan2( -R01, R00 );

    out.orientationDeg = Vector3(
        x_rad * 180.0 / PI,
        y_rad * 180.0 / PI,
        z_rad * 180.0 / PI );

    // ---- Round-trip verification (uses RISE's canonical builders) ----
    // Build the candidate matrix EXACTLY the way Transformable does:
    // M_candidate = T · R · S where R = XRotation·YRotation·ZRotation,
    // S = Stretch(scale), T = Translation(position).  This binds the
    // decomposition's correctness to RISE's actual composition rule —
    // if Transformable::SetOrientation ever changes order, this check
    // fails loudly and the engine falls back to matrix override.
    const Matrix4 candidateR =
        Matrix4Ops::XRotation( x_rad )
        * Matrix4Ops::YRotation( y_rad )
        * Matrix4Ops::ZRotation( z_rad );
    const Matrix4 candidateS = Matrix4Ops::Stretch( out.scale );
    const Matrix4 candidateT = Matrix4Ops::Translation( out.position );
    const Matrix4 candidate  = candidateT * candidateR * candidateS;
    if( !MatricesNearEqual( candidate, M, 1e-6 ) ) {
        out.ok = false;
        return out;
    }

    out.ok = true;
    return out;
}

// Locale-independent scalar formatter.  6 significant digits (§11.11).
// Round-trips through the parser's atof within tolerance 1e-6.
std::string FormatScalar( double v )
{
    char buf[64];
    std::snprintf( buf, sizeof(buf), "%.6g", v );
    return std::string( buf );
}

std::string FormatVec3( const Vector3& v )
{
    return FormatScalar( v.x ) + " " + FormatScalar( v.y ) + " " + FormatScalar( v.z );
}

std::string FormatMatrix( const Matrix4& m )
{
    // Column-major output to match standard_object's parser layout
    // (Job.cpp:5103-5107 reverse mapping).  16 doubles space-separated.
    std::string out;
    const double cells[16] = {
        m._00, m._01, m._02, m._03,
        m._10, m._11, m._12, m._13,
        m._20, m._21, m._22, m._23,
        m._30, m._31, m._32, m._33 };
    for( int i = 0; i < 16; ++i ) {
        if( i > 0 ) out += " ";
        out += FormatScalar( cells[i] );
    }
    return out;
}

// Accumulator entry for managed-block emission.
struct OverrideEntry
{
    bool         hasPosition    = false;
    bool         hasOrientation = false;
    bool         hasQuaternion  = false;   // R-final P1 #2 fix
    bool         hasScale       = false;
    bool         hasMatrix      = false;
    Vector3      position;
    Vector3      orientationDeg;
    double       quaternion[4] = {0,0,0,1};
    Vector3      scale;
    Matrix4      matrix;

    bool Empty() const
    {
        return !hasPosition && !hasOrientation && !hasQuaternion
            && !hasScale && !hasMatrix;
    }
    void ClearAllFields()
    {
        hasPosition = hasOrientation = hasQuaternion = hasScale = hasMatrix = false;
    }
};

// Detect CRLF vs LF on the input bytes — preserves whichever the
// loaded file used.
const char* DetectEol( const std::string& bytes )
{
    for( std::size_t i = 0; i + 1 < bytes.size(); ++i ) {
        if( bytes[i] == '\r' && bytes[i+1] == '\n' ) return "\r\n";
        if( bytes[i] == '\n' ) return "\n";
    }
    return "\n";
}

// Scan for sentinel-bracketed managed override block.  Returns the
// byte range covering the open sentinel line through the close
// sentinel line (inclusive of trailing newline).  Returns {0,0} if
// not found.
struct BlockRange {
    std::size_t start = 0;
    std::size_t end   = 0;
    bool found     = false;
    bool malformed = false;   // open sentinel with no matching close
};

BlockRange LocateManagedBlock( const std::string& bytes )
{
    BlockRange r;

    // P2-E fix: tighter detection.  A line is the OPEN sentinel iff
    // after stripping leading whitespace + trailing CR, it equals
    // the canonical `kSentinelOpen` exactly.  Same for `kSentinelClose`.
    // This rejects false positives like
    // `# Note: RISE editor overrides go here` which previously would
    // have flipped the in-block flag.
    auto LineMatches = []( const std::string& line, const char* canonical ) -> bool {
        std::string content = line;
        if( !content.empty() && content.back() == '\r' ) content.pop_back();
        const std::size_t firstNW = content.find_first_not_of( " \t" );
        if( firstNW == std::string::npos ) return false;
        return content.compare( firstNW, std::string::npos, canonical ) == 0;
    };

    std::size_t pos = 0;
    while( pos < bytes.size() ) {
        std::size_t lineEnd = bytes.find( '\n', pos );
        if( lineEnd == std::string::npos ) lineEnd = bytes.size();
        const std::string line = bytes.substr( pos, lineEnd - pos );
        if( LineMatches( line, kSentinelOpen ) ) {
            // Found open sentinel; now find close.
            const std::size_t lineStart = pos;
            std::size_t closePos = lineEnd + 1;
            while( closePos < bytes.size() ) {
                std::size_t closeEnd = bytes.find( '\n', closePos );
                if( closeEnd == std::string::npos ) closeEnd = bytes.size();
                const std::string cline = bytes.substr( closePos, closeEnd - closePos );
                if( LineMatches( cline, kSentinelClose ) ) {
                    // closeEnd points at the '\n' (or EOF); include
                    // the newline in the block range so the splice
                    // doesn't leave a trailing blank line.
                    r.start = lineStart;
                    r.end = closeEnd < bytes.size() ? closeEnd + 1 : closeEnd;
                    r.found = true;
                    return r;
                }
                closePos = closeEnd + 1;
            }
            // Opening sentinel without close — the managed block is
            // malformed (a hand-edit deleted the close sentinel).
            // Flag it so the engine refuses the save: proceeding
            // would leave the parser's "inside managed block" state
            // stuck and corrupt the file (round-3 review P2).
            r.malformed = true;
            return r;
        }
        pos = lineEnd + 1;
    }
    return r;
}

// Classify a `>` command line.
enum class CmdClass { Barrier, Destructive, Configuration, Unknown };

struct CmdRecord
{
    std::size_t offset;
    CmdClass    cls;
    std::string keyword;        // e.g. "render"
    std::string arg0;           // for `> remove object X`: arg0 == "object"
    std::string arg1;           // arg1 == "X"
};

CmdClass ClassifyCmd( const std::string& keyword )
{
    if( keyword == "render" || keyword == "renderanimation"
        || keyword == "predict" || keyword == "photonmap"
        || keyword == "quit" || keyword == "load" || keyword == "run" ) {
        return CmdClass::Barrier;
    }
    if( keyword == "remove" || keyword == "clearall" ) {
        return CmdClass::Destructive;
    }
    if( keyword == "set" || keyword == "modify" || keyword == "mediapath"
        || keyword == "echo" ) {
        return CmdClass::Configuration;
    }
    return CmdClass::Unknown;
}

// Scan `bytes` for `>` command lines.  Returns barrier + destructive
// records in scene-file order; configuration / unknown classifications
// are filtered out at this layer.
void ScanCommands( const std::string& bytes,
                   std::vector<CmdRecord>& barriers,
                   std::vector<CmdRecord>& destructives )
{
    std::size_t pos = 0;
    while( pos < bytes.size() ) {
        std::size_t lineEnd = bytes.find( '\n', pos );
        if( lineEnd == std::string::npos ) lineEnd = bytes.size();
        std::string line = bytes.substr( pos, lineEnd - pos );
        if( !line.empty() && line.back() == '\r' ) line.pop_back();
        std::size_t firstNW = line.find_first_not_of( " \t" );
        if( firstNW != std::string::npos && line[firstNW] == '>' ) {
            // Tokenize the rest of the line.
            std::istringstream is( line.substr( firstNW + 1 ) );
            std::vector<std::string> toks;
            std::string t;
            while( is >> t ) toks.push_back( t );
            if( !toks.empty() ) {
                CmdRecord rec;
                rec.offset  = pos + firstNW;
                rec.keyword = toks[0];
                rec.arg0    = toks.size() > 1 ? toks[1] : std::string();
                rec.arg1    = toks.size() > 2 ? toks[2] : std::string();
                rec.cls     = ClassifyCmd( rec.keyword );
                if( rec.cls == CmdClass::Barrier )      barriers.push_back( rec );
                else if( rec.cls == CmdClass::Destructive ) destructives.push_back( rec );
            }
        }
        pos = lineEnd + 1;
    }
}

// Build the canonical managed-block text.  §9.6 rules 1-4.  Names are
// alphabetised; within a chunk, parameter order is canonical;
// formatting is normalised regardless of input layout.
//
// Phase C: the block now also carries `createdChunks` — fully-rendered
// chunk texts for entities the editor created this session (e.g.
// AddCamera clones).  They're emitted FIRST, before the override
// entries: a fresh `pinhole_camera { ... }` is independent of any
// override and reads cleanly at the top of the managed region.
std::string RenderManagedBlock(
    const std::map<std::string, OverrideEntry>& accumulator,
    const std::vector<std::string>& createdChunks,
    const std::string& eol )
{
    if( accumulator.empty() && createdChunks.empty() ) return std::string();
    std::string out;
    out += kSentinelOpen;
    out += eol;
    for( const std::string& chunk : createdChunks ) {
        // Each entry is a complete, eol-terminated chunk text.
        out += chunk;
    }
    for( const auto& kv : accumulator ) {
        const std::string& name = kv.first;
        const OverrideEntry& e = kv.second;
        if( e.Empty() ) continue;
        out += "override_object";
        out += eol;
        out += "{";
        out += eol;
        out += "  name ";
        out += name;
        out += eol;
        if( e.hasPosition ) {
            out += "  position ";
            out += FormatVec3( e.position );
            out += eol;
        }
        if( e.hasOrientation ) {
            out += "  orientation ";
            out += FormatVec3( e.orientationDeg );
            out += eol;
        }
        if( e.hasQuaternion ) {
            // R-final P1 #2 fix: emit quaternion when present.  Order
            // follows the override_object descriptor: name → position
            // → orientation → quaternion → matrix → scale.
            out += "  quaternion ";
            out += FormatScalar( e.quaternion[0] );
            out += " ";
            out += FormatScalar( e.quaternion[1] );
            out += " ";
            out += FormatScalar( e.quaternion[2] );
            out += " ";
            out += FormatScalar( e.quaternion[3] );
            out += eol;
        }
        if( e.hasMatrix ) {
            out += "  matrix ";
            out += FormatMatrix( e.matrix );
            out += eol;
        }
        if( e.hasScale ) {
            out += "  scale ";
            out += FormatVec3( e.scale );
            out += eol;
        }
        out += "}";
        out += eol;
    }
    out += kSentinelClose;
    out += eol;
    return out;
}

// Phase C: render a fresh scene-file chunk for a newly-created camera
// from its descriptor introspection.  Returns "" if the camera type
// has no known chunk keyword (an out-of-tree camera subclass).  The
// chunk text is eol-terminated and ready to drop into the managed
// block.  Indented 2 spaces to match the override_object entries.
std::string RenderCreatedCameraChunk( const ICamera& cam,
                                      const std::string& name,
                                      const std::string& eol )
{
    const String keyword = CameraIntrospection::GetDescriptorKeyword( cam );
    if( keyword.size() == 0 ) return std::string();

    std::string out;
    out += std::string( keyword.c_str() );
    out += eol;
    out += "{";
    out += eol;
    // Emit `name` first so the chunk is human-scannable; the
    // introspection list's own `name` row (if any) is then skipped.
    out += "  name ";
    out += name;
    out += eol;

    // CameraIntrospection::Inspect filters the redundant scalar
    // shadows (`pitch` / `roll` / `yaw`) and the `target_orientation`
    // Vec3 (theta/phi cover it), so the editable rows are the
    // canonical set: `location` / `lookat` / `up` / `orientation` /
    // `theta` / `phi` / `fov` / lens params.  `includeRollOrientation
    // = true` keeps the `orientation` row so an orbited-AND-rolled
    // clone reproduces faithfully.  Emitting all of them faithfully
    // reproduces the camera.
    const std::vector<CameraProperty> props =
        CameraIntrospection::Inspect( cam, /*includeRollOrientation=*/true );
    for( const CameraProperty& p : props ) {
        if( !p.editable ) continue;          // read-only / synthetic row
        const std::string key = std::string( p.name.c_str() );
        if( key == "name" ) continue;        // already emitted above
        out += "  ";
        out += key;
        out += " ";
        out += std::string( p.value.c_str() );
        out += eol;
    }
    out += "}";
    out += eol;
    return out;
}

// Phase B: descriptor-introspect one editable entity → its current
// {name, value, editable, …} rows.  Same code path the properties
// panel uses.  Returns empty for a missing entity.  Used by the
// property pass AND the post-save loaded-snapshot refresh.
std::vector<CameraProperty> InspectEntity( IJobPriv& job,
                                           EntityCategory cat,
                                           const std::string& name )
{
    const IScene* scene = job.GetScene();
    switch( cat ) {
    case EntityCategory::Camera: {
        const ICameraManager* m = scene ? scene->GetCameras() : 0;
        const ICamera* c = m ? m->GetItem( name.c_str() ) : 0;
        // includeRollOrientation = true: the property pass must see
        // the `orientation` row to diff/round-trip a Roll-tool edit.
        if( c ) return CameraIntrospection::Inspect( *c, true );
        break;
    }
    case EntityCategory::Light: {
        const ILightManager* m = scene ? scene->GetLights() : 0;
        const ILightPriv* l = m ? m->GetItem( name.c_str() ) : 0;
        if( l ) return LightIntrospection::Inspect( String( name.c_str() ), *l );
        break;
    }
    case EntityCategory::Material: {
        IMaterialManager* m = job.GetMaterials();
        const IMaterial* mat = m ? m->GetItem( name.c_str() ) : 0;
        if( mat ) return MaterialIntrospection::Inspect(
            String( name.c_str() ), *mat,
            job.GetPainters(), job.GetScalarPainters(), &job );
        break;
    }
    case EntityCategory::Medium: {
        const IMedium* med = job.GetMedium( name.c_str() );
        if( med ) return MediaIntrospection::Inspect( String( name.c_str() ), *med );
        break;
    }
    case EntityCategory::Object: {
        IObjectManager* m = job.GetObjects();
        const IObject* o = m ? m->GetItem( name.c_str() ) : 0;
        if( o ) return ObjectIntrospection::Inspect(
            String( name.c_str() ), *o,
            job.GetMaterials(), job.GetShaders(), &job );
        break;
    }
    }
    return std::vector<CameraProperty>();
}

// Field comparison: equal within format-tolerance, since the saved
// scalar will be 6-sig-digit truncated on disk.  Component-wise.
bool ScalarEq( double a, double b )
{
    return std::fabs( a - b ) <= 1e-6 * std::max( 1.0, std::max( std::fabs(a), std::fabs(b) ) );
}
bool Vec3Eq( const Vector3& a, const Vector3& b )
{
    return ScalarEq( a.x, b.x ) && ScalarEq( a.y, b.y ) && ScalarEq( a.z, b.z );
}

// Atomic write: tmp + (POSIX-only fsync) + rename + (POSIX-only
// dir-fsync) (§9.8).  R-final P1 #1 fix: portable across POSIX and
// Windows.  Core flow uses std::ofstream + std::filesystem::rename —
// both atomic on supported filesystems (POSIX rename is atomic on
// same-FS; std::filesystem::rename calls MoveFileEx with
// MOVEFILE_REPLACE_EXISTING on Windows, also atomic on NTFS).  The
// fsync hardening (which protects against power-loss between rename
// and journal-flush) is POSIX-only; on Windows we rely on NTFS
// journaling and skip the explicit flush.
bool AtomicWrite( const std::string& filePath, const std::string& bytes,
                  std::string& outError )
{
    // Build a per-process / per-invocation tmp path.  pid + epoch
    // seconds is unique enough for single-threaded save sequences;
    // the rename is what enforces atomicity, not the tmp uniqueness.
#if defined(_WIN32)
    const long long pidValue = static_cast<long long>( ::_getpid() );
#else
    const long long pidValue = static_cast<long long>( ::getpid() );
#endif
    const std::string tmpPath = filePath + ".tmp." +
        std::to_string( pidValue ) + "." +
        std::to_string( static_cast<long long>( std::time( nullptr ) ) );

    // Write bytes to tmp via std::ofstream (binary mode, no text
    // translation — we manage line endings explicitly via DetectEol).
    {
        std::ofstream ofs( tmpPath.c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc );
        if( !ofs.is_open() ) {
            outError = "could not open temp file for write: " + tmpPath;
            return false;
        }
        ofs.write( bytes.data(), static_cast<std::streamsize>( bytes.size() ) );
        if( !ofs.good() ) {
            outError = "write to temp file failed: " + tmpPath;
            ofs.close();
            std::error_code rmEc;
            std::filesystem::remove( tmpPath, rmEc );
            return false;
        }
        ofs.flush();
        ofs.close();
        // POSIX-only: fsync the tmp file so the bytes hit storage
        // before the rename.  On Windows NTFS journaling does this
        // implicitly; std::ofstream::close already flushed the
        // userspace buffer.
#if !defined(_WIN32)
        int fd = ::open( tmpPath.c_str(), O_RDONLY );
        if( fd >= 0 ) {
            if( ::fsync( fd ) != 0 ) {
                outError = std::string( "fsync(tmp) failed: " ) + std::strerror( errno );
                ::close( fd );
                std::error_code rmEc;
                std::filesystem::remove( tmpPath, rmEc );
                return false;
            }
            ::close( fd );
        }
#endif
    }

    // Atomic rename — same FS guarantees on POSIX; MoveFileEx with
    // MOVEFILE_REPLACE_EXISTING on Windows (also atomic on NTFS).
    std::error_code renameEc;
    std::filesystem::rename( tmpPath, filePath, renameEc );
    if( renameEc ) {
        outError = std::string( "rename failed: " ) + renameEc.message();
        std::error_code rmEc;
        std::filesystem::remove( tmpPath, rmEc );
        return false;
    }

    // Best-effort directory fsync to harden the rename against power
    // loss.  POSIX-only; failure here is not fatal (file IS already
    // in place).  Windows skips: NTFS journaling covers it.
#if !defined(_WIN32)
    const std::size_t slash = filePath.find_last_of( "/\\" );
    const std::string dirPath = slash != std::string::npos
        ? filePath.substr( 0, slash )
        : std::string( "." );
    int dirfd = ::open( dirPath.c_str(), O_RDONLY );
    if( dirfd >= 0 ) {
        ::fsync( dirfd );
        ::close( dirfd );
    }
#endif
    return true;
}

// Read file bytes into a string.
bool ReadFile( const std::string& filePath, std::string& outBytes,
               std::string& outError )
{
    std::ifstream in( filePath.c_str(), std::ios::in | std::ios::binary );
    if( !in.is_open() ) {
        outError = "could not open file for reading";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    outBytes = ss.str();
    return true;
}

}  // anonymous namespace

// ----------------------------------------------------------------------
// SaveEngine
// ----------------------------------------------------------------------

SaveEngine::SaveEngine(
    IJobPriv&                              job,
    const SourceSpanIndex&                 spans,
    const OverrideSpanIndex&               overrideSpans,
    const TransformSnapshot&               base,
    const TransformSnapshot&               loaded,
    DirtyTracker&                          dirty,
    std::unordered_set<std::string>&       scaleFromAnchorSet )
    : mJob( job )
    , mSpans( spans )
    , mOverrideSpans( overrideSpans )
    , mBase( base )
    , mLoaded( loaded )
    , mDirty( dirty )
    , mScaleFromAnchorSet( scaleFromAnchorSet )
{
}

SaveResult SaveEngine::Save( const std::string& filePath )
{
    SaveResult result;
    result.filePath = filePath;

    // SourceSpanIndex stores byte offsets captured at LOAD time
    // against the bytes of the ORIGINALLY-LOADED file (`loadIdent.filePath`).
    // For an in-place save, source path == target path; for a Save-As,
    // they differ.  Either way the engine ALWAYS reads from the
    // captured source path and writes to the target path (`filePath`).
    // Reading from the target path on a Save-As would either fail (new
    // file) or worse, splice load-time offsets into an unrelated file.
    const FileIdentity& loadIdent = mSpans.GetFileIdentity();
    const std::string sourcePath =
        loadIdent.captured ? loadIdent.filePath : filePath;
    const bool isSaveAs =
        loadIdent.captured && ( sourcePath != filePath );

    // ---- Step 0: external-modification guard (R-final P1 #3 / §11.6) ----
    // Validate that the SOURCE file's on-disk state still matches the
    // identity captured at load time.  This protects both in-place
    // saves and Save-As: if the source was edited externally between
    // load and save, byte offsets stored in SourceSpanIndex no longer
    // describe the bytes we're about to read.
    if( loadIdent.captured ) {
        struct ::stat current_stat = {};
        if( ::stat( sourcePath.c_str(), &current_stat ) == 0 ) {
            const long long curSize  = static_cast<long long>( current_stat.st_size );
            const long long curMtime = static_cast<long long>( current_stat.st_mtime );
            long long curMtimeNsec = 0;
#if defined(__APPLE__)
            curMtimeNsec = static_cast<long long>( current_stat.st_mtimespec.tv_nsec );
#elif defined(__linux__) || defined(__unix__)
            curMtimeNsec = static_cast<long long>( current_stat.st_mtim.tv_nsec );
#endif
            if( curSize != loadIdent.sizeBytes
                || curMtime != loadIdent.mtimeSec
                || curMtimeNsec != loadIdent.mtimeNsec ) {
                result.status = SaveResult::Status::Refused;
                result.errorMessage =
                    "scene file '" + sourcePath + "' was modified externally "
                    "between load and save (mtime/size mismatch).  Applying "
                    "load-time byte offsets to the current file would "
                    "corrupt unrelated content.  Reload the file before "
                    "saving.";
                return result;
            }
        }
        // stat() failure here is non-fatal: the file might have been
        // deleted and re-created with the same content.  The
        // subsequent ReadFile will surface a more useful error in
        // that case.
    }

    // ---- Step 1: read file bytes from the SOURCE path ---------------
    // Save-As reads from sourcePath (captured at load time), NOT from
    // the user-chosen target — load-time byte offsets are only valid
    // against the source bytes.
    std::string bytes;
    std::string readError;
    if( !ReadFile( sourcePath, bytes, readError ) ) {
        result.status = SaveResult::Status::Failed;
        result.errorMessage = readError;
        return result;
    }

    // ---- Step 2: detect EOL convention ------------------------------
    const std::string eol = DetectEol( bytes );

    // ---- Step 3: scan for `>` commands + existing managed block -----
    std::vector<CmdRecord> barrierCmds;
    std::vector<CmdRecord> destructiveCmds;
    ScanCommands( bytes, barrierCmds, destructiveCmds );
    BlockRange existingBlock = LocateManagedBlock( bytes );

    // Round-3 review P2: refuse on a malformed managed block (an
    // opening sentinel with no matching close).  Re-rendering or
    // erasing a block whose extent is unknown would corrupt the
    // file; the user must repair the sentinels first.
    if( existingBlock.malformed ) {
        result.status = SaveResult::Status::Refused;
        result.errorMessage =
            "the managed override block is malformed — its opening "
            "sentinel ('" + std::string( kSentinelOpen ) + "') has no "
            "matching close.  Repair or remove the block before saving.";
        return result;
    }

    // ---- Step 4: CLASSIFICATION PASS --------------------------------
    // Seed accumulator from managed records only (R4 §1).
    std::map<std::string, OverrideEntry> accumulator;
    for( const OverrideRecord& rec : mOverrideSpans.Entries() ) {
        if( !rec.managed ) continue;
        OverrideEntry e;
        e.hasPosition    = rec.hasPosition;
        e.hasOrientation = rec.hasOrientation;
        e.hasQuaternion  = rec.hasQuaternion;   // R-final P1 #2 fix
        e.hasScale       = rec.hasScale;
        e.hasMatrix      = rec.hasMatrix;
        e.position       = rec.position;
        e.orientationDeg = rec.orientation;
        e.quaternion[0]  = rec.quaternion[0];
        e.quaternion[1]  = rec.quaternion[1];
        e.quaternion[2]  = rec.quaternion[2];
        e.quaternion[3]  = rec.quaternion[3];
        e.scale          = rec.scale;
        e.matrix         = rec.matrix;
        // Multiple managed records for the same name are
        // structurally invalid (canonical layout guarantees one
        // per name).  If they exist, last write wins.
        accumulator[rec.targetName] = e;
    }

    // Mode A edits — queued, not yet emitted.  Emitted in step 6.
    std::vector<EditOp> modeAQueue;

    IObjectManager* objs = mJob.GetObjects();
    if( !objs ) {
        result.status = SaveResult::Status::Failed;
        result.errorMessage = "IObjectManager unavailable";
        return result;
    }

    const std::vector<std::string> dirtyNames = mDirty.Snapshot();
    for( const std::string& name : dirtyNames ) {
        // --- Cross-file refusal (R7 §1 / pinned 2.25) ---
        // "Cross-file" means the entity was defined in a `> load`-ed
        // child file, not in the top-level scene we're saving.  The
        // top-level scene is the SOURCE file (the bytes we just read),
        // not the target path (which on Save-As is a different file).
        const std::string& creationFile = mSpans.GetCreationFilePath( name );
        if( !creationFile.empty() && creationFile != sourcePath ) {
            result.status = SaveResult::Status::Refused;
            result.errorMessage =
                "managed override for '" + name + "' cannot be saved: its "
                "source chunk lives in '" + creationFile + "', not in the "
                "top-level file '" + sourcePath + "'.  V1 does not support "
                "cross-file managed overrides; editing an object defined "
                "by `> load` or `> run` is out of scope.";
            return result;
        }

        // Look up runtime state + snapshots.
        IObjectPriv* obj = objs->GetItem( name.c_str() );
        if( !obj ) {
            // Object disappeared between dirty-mark and save.  V1
            // doesn't support runtime remove; skip with a counter
            // bump.  Save engine treats this as "no work needed".
            result.noOpCount++;
            continue;
        }
        const Matrix4 mFinal  = obj->GetFinalTransformMatrix();
        const Matrix4* pLoaded = mLoaded.Find( name );
        const Matrix4* pBase   = mBase.Find( name );

        if( !pLoaded ) {
            // §9.7 add-after-load fallback: emit a matrix override.
            OverrideEntry e;
            e.hasMatrix = true;
            e.matrix    = mFinal;
            accumulator[name] = e;
            result.matrixFallbackCount++;
            continue;
        }

        // --- No-op / revert classification (R2 §3) ---
        if( MatricesNearEqual( mFinal, *pLoaded ) ) {
            // Mfinal == Mloaded: no change since load.  Don't touch
            // retained overrides; they're still valid.
            result.noOpCount++;
            continue;
        }
        if( pBase && MatricesNearEqual( mFinal, *pBase ) ) {
            // Reverted past the override all the way back to the
            // standard_object-produced state.  Erase the retained
            // managed-block override (if any).
            accumulator.erase( name );
            continue;
        }

        // --- Force-matrix gates (R2 + R3 + R5) ---
        const SourceSpan* span = mSpans.Find( name );
        auto accIt = accumulator.find( name );
        // R-final P1 #2: a retained QUATERNION entry has the same
        // shadow-on-reload property as a retained MATRIX entry —
        // either replaces the transform stack atomically, so Mode A
        // splices on the source line would be shadowed.  Force
        // matrix-form save (the unified replacement path) when
        // either is retained.
        const bool accHasMatrixOrQuat = ( accIt != accumulator.end() )
            && ( accIt->second.hasMatrix || accIt->second.hasQuaternion );
        const Decomposition decompLoaded = TryDecompose( *pLoaded );
        const bool forceMatrixOverride =
              span == nullptr
           || span->chunkRevisited
           || span->authorMode != AuthorMode::Euler
           || mScaleFromAnchorSet.count( name ) > 0
           || mOverrideSpans.HasUnmanagedFor( name )
           || accHasMatrixOrQuat
           || !decompLoaded.ok;

        if( forceMatrixOverride ) {
            OverrideEntry e;
            e.hasMatrix = true;
            e.matrix    = mFinal;
            accumulator[name] = e;
            result.matrixFallbackCount++;
            continue;
        }

        // --- Decompose Mfinal; if it fails, matrix override ---
        const Decomposition decomp = TryDecompose( mFinal );
        if( !decomp.ok ) {
            OverrideEntry e;
            e.hasMatrix = true;
            e.matrix    = mFinal;
            accumulator[name] = e;
            result.matrixFallbackCount++;
            continue;
        }

        // --- Per-field routing.  Three cases per pinned 2.5/2.15. ---
        // Coalesce missing-field inserts per chunk (R6 §6).
        std::map<std::size_t, std::vector<std::string>> perChunkInserts;
        OverrideEntry& accEntry = accumulator[name];

        auto routeField = [&]( const char* fieldName,
                               const Vector3& newVal,
                               const Vector3& loadedVal,
                               bool& accHasFlag,
                               Vector3& accSlot )
        {
            if( Vec3Eq( newVal, loadedVal ) ) return;
            auto pit = span->parameterSpans.find( fieldName );
            if( pit == span->parameterSpans.end() ) {
                // Insert-new-line case — gather for coalescing.
                std::string line;
                line += "    ";
                line += fieldName;
                line += " ";
                line += FormatVec3( newVal );
                line += eol;
                perChunkInserts[span->bodyCloseBraceLineBegin].push_back( line );
                result.directRewriteCount++;
                accHasFlag = false;
            } else if( !pit->second.isSymbolic ) {
                // Mode A in-place splice.
                EditOp op;
                op.begin = pit->second.valueBegin;
                op.end   = pit->second.valueEnd;
                op.replacement = FormatVec3( newVal );
                modeAQueue.push_back( op );
                result.directRewriteCount++;
                accHasFlag = false;
            } else {
                // Mode B per-field override.
                accHasFlag = true;
                accSlot = newVal;
                result.overrideRewriteCount++;
            }
        };
        routeField( "position",    decomp.position,       decompLoaded.position,       accEntry.hasPosition,    accEntry.position );
        routeField( "orientation", decomp.orientationDeg, decompLoaded.orientationDeg, accEntry.hasOrientation, accEntry.orientationDeg );
        routeField( "scale",       decomp.scale,          decompLoaded.scale,          accEntry.hasScale,       accEntry.scale );

        // Emit ONE EditOp per chunk for missing-field inserts (R6 §6).
        for( const auto& kv : perChunkInserts ) {
            std::string coalesced;
            for( const std::string& s : kv.second ) coalesced += s;
            EditOp op;
            op.begin = kv.first;
            op.end   = kv.first;
            op.replacement = coalesced;
            modeAQueue.push_back( op );
        }

        if( accEntry.Empty() ) {
            accumulator.erase( name );
        }
    }

    // ---- Phase B: PROPERTY PASS -------------------------------------
    // Camera / light / material / medium property edits + object
    // material / shader / shadow / interior-medium binding edits.
    // For each property-dirty entity, diff CURRENT descriptor
    // introspection against the parse-time loaded snapshot
    // (SourceSpan::loadedPropertyValues) and Mode-A-splice every
    // parameter line whose value changed.  Unlike object transforms
    // there is no Mode B for these — a chunk that can't be spliced in
    // place (FOR-generated, cross-file, or symbolic source value) is
    // a refusal.  The resulting EditOps join `modeAQueue` and flow
    // through the same step 6/7 assembly + apply as transform splices.
    {
        const std::vector<DirtyEntity> dirtyEntities = mDirty.EntitySnapshot();

        // Object transform keywords are owned by the transform pass
        // above — the property pass must never also splice them.
        auto isObjectTransformKeyword = []( const std::string& k ) {
            return k == "position" || k == "orientation"
                || k == "quaternion" || k == "scale" || k == "matrix";
        };

        for( const DirtyEntity& de : dirtyEntities ) {
            const EntityCategory cat  = de.first;
            const std::string&   name = de.second;

            const SourceSpan* espan =
                ( cat == EntityCategory::Object )
                    ? mSpans.Find( name )
                    : mSpans.FindEntity( cat, name );

            const char* kindWord =
                  cat == EntityCategory::Camera   ? "camera"
                : cat == EntityCategory::Light    ? "light"
                : cat == EntityCategory::Material ? "material"
                : cat == EntityCategory::Medium   ? "medium"
                :                                   "object";

            if( !espan ) {
                // Phase C: a session-created entity (AddCamera clone)
                // legitimately has no source span — the created-entity
                // pass below re-emits its whole chunk from current
                // introspection, which already reflects this property
                // edit.  Skip it here rather than refusing.
                if( mDirty.IsSessionCreated( cat, name ) ) continue;

                result.status = SaveResult::Status::Refused;
                result.errorMessage = std::string( "property edit on " )
                    + kindWord + " '" + name + "' cannot be saved: no "
                    "editable source chunk (FOR-generated entity, or its "
                    "chunk lives in a `> load`-ed file).";
                return result;
            }
            // Phase C (round-2 review): an entity whose chunk lives
            // INSIDE the managed block is engine-owned — the created-
            // entity pass below re-emits its whole chunk from current
            // introspection (which already reflects this edit).  Skip
            // it here: a value splice into the wholesale-re-rendered
            // block would overlap the Case-(c) block-replace EditOp.
            if( espan->insideManagedBlock ) continue;
            if( espan->chunkRevisited ) {
                result.status = SaveResult::Status::Refused;
                result.errorMessage = std::string( "property edit on " )
                    + kindWord + " '" + name + "' cannot be saved: its "
                    "chunk is FOR-generated (the parser re-entered it).";
                return result;
            }
            if( !espan->filePath.empty() && espan->filePath != sourcePath ) {
                result.status = SaveResult::Status::Refused;
                result.errorMessage = std::string( "property edit on " )
                    + kindWord + " '" + name + "' cannot be saved: its "
                    "chunk lives in '" + espan->filePath + "', not the "
                    "saved file '" + sourcePath + "'.";
                return result;
            }

            // CURRENT descriptor introspection — same code path the
            // properties panel uses to display values.
            const std::vector<CameraProperty> cur =
                InspectEntity( mJob, cat, name );

            for( const CameraProperty& p : cur ) {
                if( !p.editable ) continue;   // read-only / synthetic row
                const std::string key    = std::string( p.name.c_str() );
                const std::string curVal = std::string( p.value.c_str() );

                // Objects: transform params belong to the transform pass.
                if( cat == EntityCategory::Object
                    && isObjectTransformKeyword( key ) ) {
                    continue;
                }

                // Diff against the parse-time loaded value.
                std::unordered_map<std::string,std::string>::const_iterator lit =
                    espan->loadedPropertyValues.find( key );
                if( lit == espan->loadedPropertyValues.end() ) {
                    // Round-1 review P2: the key is in current
                    // introspection but was NOT captured in the
                    // parse-time snapshot (descriptor row sets can be
                    // value-conditional — e.g. MaterialIntrospection
                    // gates rows on the bound painter resolving).  We
                    // can't diff what we never snapshotted; refuse
                    // rather than silently drop the edit.
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage = std::string( "property edit on " )
                        + kindWord + " '" + name + "': parameter '" + key
                        + "' was not captured at scene-load time and "
                        "cannot be safely diffed — reload the scene "
                        "before editing it.";
                    return result;
                }
                if( lit->second == curVal ) continue;                    // unchanged

                // Round-1 review P1: a light `color` round-trips
                // through TWO colour spaces — the scene file stores
                // sRGB, the engine works in linear ROMM, and the
                // descriptor introspection reports the linear value.
                // Splicing that linear value back would shift the
                // colour on reload.  Refuse rather than persist a
                // wrong colour.  (`power`, `position`, angles, etc.
                // carry no colour-space and save fine.)
                if( cat == EntityCategory::Light && key == "color" ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage = std::string( "property edit on light '" )
                        + name + "': the `color` parameter cannot be round-"
                        "tripped in V1 — the scene file stores it in a "
                        "non-linear colour space the editor does not yet "
                        "convert back to.  Other light properties save fine.";
                    return result;
                }

                // Camera transform parameters (`location`/`lookat`/
                // `up`/`orientation`/`theta`/`phi`) splice like any
                // other parameter: introspection reports the canonical
                // rows in units that match the chunk parser (the parser
                // converts `orientation` and `theta`/`phi` from degrees,
                // which is what introspection emits).
                //
                // One exception — the parser lets the `pitch`/`roll`/
                // `yaw` scalar lines OVERRIDE individual components of
                // the `orientation` Vec3 (AsciiSceneParser pinhole
                // Finalize).  If the source authored any of those
                // scalars, splicing the Vec3 alone would leave the
                // scalar override in force and reload to the wrong
                // orientation.  Refuse rather than corrupt — the author
                // can collapse the scalars into one `orientation` line
                // by hand.
                if( cat == EntityCategory::Camera && key == "orientation"
                    && ( espan->parameterSpans.count( "pitch" )
                      || espan->parameterSpans.count( "roll" )
                      || espan->parameterSpans.count( "yaw" ) ) ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage = std::string( "property edit on camera '" )
                        + name + "': the source chunk sets the camera tilt "
                        "with `pitch`/`roll`/`yaw` scalar lines, which the "
                        "parser applies ON TOP OF the `orientation` vector — "
                        "V1 cannot safely round-trip a Roll edit through "
                        "that.  Replace the pitch/roll/yaw lines with a "
                        "single `orientation` line in the text editor first.";
                    return result;
                }

                std::unordered_map<std::string,ParameterSpan>::const_iterator pit =
                    espan->parameterSpans.find( key );
                if( pit == espan->parameterSpans.end() ) {
                    // Parameter line absent in source — the user changed
                    // a parameter that was at its default (and so was
                    // omitted from the chunk).  Insert a fresh line just
                    // before the chunk's close brace.
                    EditOp op;
                    op.begin = espan->bodyCloseBraceLineBegin;
                    op.end   = espan->bodyCloseBraceLineBegin;
                    op.replacement = std::string( "    " ) + key + " " + curVal + eol;
                    modeAQueue.push_back( op );
                    result.directRewriteCount++;
                } else if( pit->second.isSymbolic ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage = std::string( "property edit on " )
                        + kindWord + " '" + name + "': parameter '" + key
                        + "' has a `$(...)` expression value in the source — "
                        "V1 cannot overwrite a symbolic value.";
                    return result;
                } else {
                    // Mode A in-place splice of the value range.
                    EditOp op;
                    op.begin = pit->second.valueBegin;
                    op.end   = pit->second.valueEnd;
                    op.replacement = curVal;
                    modeAQueue.push_back( op );
                    result.directRewriteCount++;
                }
            }
        }
    }

    // ---- Phase C: CREATED-ENTITY PASS -------------------------------
    // Emit a fresh chunk for every ENGINE-OWNED camera, to live inside
    // the managed block.  Engine-owned = either:
    //   (a) session-created this run (AddCamera clone — no source
    //       span at all), OR
    //   (b) parsed from INSIDE the managed block in a PREVIOUS session
    //       (SourceSpan::insideManagedBlock) — a camera the editor
    //       emitted before.  Without (b), a reload-then-save would see
    //       an empty `mSessionCreated`, decide the block has no managed
    //       content, and erase it — silently deleting the camera
    //       (round-2 review P1).
    // Re-emitted on EVERY save (the block is rendered wholesale); a
    // session-created camera that was undone / no longer exists is
    // simply skipped.  V1: only cameras are creatable.
    std::vector<std::string> createdChunks;
    {
        const IScene* scene = mJob.GetScene();
        const ICameraManager* cams = scene ? scene->GetCameras() : 0;

        // Union the two sources, deduped + name-sorted for a
        // deterministic block layout.
        std::set<std::string> ownedCameras;
        const std::vector<DirtyEntity> created = mDirty.SessionCreatedSnapshot();
        for( const DirtyEntity& ce : created ) {
            if( ce.first == EntityCategory::Camera ) {
                ownedCameras.insert( ce.second );
            }
        }
        for( const auto& kv : mSpans.EntityEntries() ) {
            if( !kv.second.insideManagedBlock ) continue;
            // Round-3 review P1: V1 only ever emits CAMERAS into the
            // managed block.  A non-camera chunk inside the sentinels
            // was hand-pasted; the property pass skips it and this
            // pass can't re-emit it, so a block re-render would delete
            // it.  Refuse rather than silently drop it.
            if( kv.first.first != EntityCategory::Camera ) {
                result.status = SaveResult::Status::Refused;
                result.errorMessage =
                    "the managed override block contains a hand-edited "
                    "non-camera chunk ('" + kv.first.second + "') that V1 "
                    "cannot round-trip — move it out of the block "
                    "(outside the `" + std::string( kSentinelOpen ) +
                    "` / `" + std::string( kSentinelClose ) + "` sentinels) "
                    "before saving.";
                return result;
            }
            ownedCameras.insert( kv.first.second );
        }

        for( const std::string& camName : ownedCameras ) {
            const ICamera* cam = cams ? cams->GetItem( camName.c_str() ) : 0;
            if( !cam ) continue;   // created then undone — nothing to emit

            // `RenderCreatedCameraChunk` emits the full canonical
            // introspected set — `location`/`lookat`/`up`/`theta`/
            // `phi`/`fov`/lens params — so an orbited clone's orbit
            // (`theta`/`phi`) round-trips along with the rest frame.
            const std::string text =
                RenderCreatedCameraChunk( *cam, camName, eol );
            if( !text.empty() ) {
                createdChunks.push_back( text );
                result.overrideRewriteCount++;
            }
        }
    }

    // ---- Step 5: PLACEMENT PASS -------------------------------------
    std::vector<std::string> targetNames;
    for( const auto& kv : accumulator ) targetNames.push_back( kv.first );

    // Retained-managed cross-file refusal (R7 §1).  Compare against
    // the SOURCE path (the file whose bytes we read) — see the matching
    // comment on the dirty-loop cross-file check above.
    for( const std::string& name : targetNames ) {
        const std::string& cf = mSpans.GetCreationFilePath( name );
        if( !cf.empty() && cf != sourcePath ) {
            result.status = SaveResult::Status::Refused;
            result.errorMessage =
                "managed override for '" + name + "' (retained from a "
                "previous save) no longer points at a top-level target: "
                "its source chunk now lives in '" + cf + "'.";
            return result;
        }
    }

    // Destructive refusal (R6 §4 / pinned 2.24).
    for( const CmdRecord& cmd : destructiveCmds ) {
        if( cmd.keyword == "clearall" ) {
            for( const std::string& name : targetNames ) {
                const std::size_t end = mSpans.GetCreationOffsetEnd( name );
                if( end != SourceSpanIndex::kNoCreationOffset && end < cmd.offset ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage =
                        "managed override for '" + name + "' would be erased "
                        "on reload: `> clearall` at offset " +
                        std::to_string( cmd.offset ) + " destroys it.";
                    return result;
                }
            }
        } else if( cmd.keyword == "remove" && cmd.arg0 == "object" ) {
            const std::string& targetName = cmd.arg1;
            if( std::find( targetNames.begin(), targetNames.end(), targetName )
                != targetNames.end() ) {
                const std::size_t end = mSpans.GetCreationOffsetEnd( targetName );
                if( end != SourceSpanIndex::kNoCreationOffset && end < cmd.offset ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage =
                        "managed override for '" + targetName + "' would be "
                        "erased on reload: `> remove object " + targetName +
                        "` at offset " + std::to_string( cmd.offset ) +
                        " destroys it.";
                    return result;
                }
            }
        }
    }

    // Compute blockInsertOffset.  The managed block needs placement
    // whenever it has ANY content — override targets OR Phase C
    // created-entity chunks.  With created chunks but no targets the
    // lowerBound / refusal loops below simply don't iterate (empty
    // targetNames), leaving lowerBound = 0 → the block lands before
    // the first BARRIER `>` command, same as a target-only block.
    std::size_t blockInsertOffset = bytes.size();   // default: EOF
    if( !targetNames.empty() || !createdChunks.empty() ) {
        // lowerBound: AFTER every target chunk AND every unmanaged
        // shadow of any target.
        std::size_t lowerBound = 0;
        for( const std::string& name : targetNames ) {
            const std::size_t end = mSpans.GetCreationOffsetEnd( name );
            if( end != SourceSpanIndex::kNoCreationOffset ) {
                lowerBound = std::max( lowerBound, end );
            }
            std::vector<const OverrideRecord*> all = mOverrideSpans.FindAll( name );
            for( const OverrideRecord* r : all ) {
                if( !r->managed ) {
                    lowerBound = std::max( lowerBound, r->chunkEndOffset );
                }
            }
        }

        // upperBound: BEFORE the first BARRIER `>` that follows lowerBound.
        std::size_t upperBound = bytes.size();
        for( const CmdRecord& cmd : barrierCmds ) {
            if( cmd.offset > lowerBound ) {
                upperBound = cmd.offset;
                break;
            }
        }

        // Refusal: target chunk falls AFTER upperBound.
        for( const std::string& name : targetNames ) {
            const std::size_t end = mSpans.GetCreationOffsetEnd( name );
            if( end != SourceSpanIndex::kNoCreationOffset && end > upperBound ) {
                result.status = SaveResult::Status::Refused;
                result.errorMessage =
                    "managed override block placement requires '" + name +
                    "'s source chunk (ends at offset " + std::to_string(end) +
                    ") to appear before the next BARRIER `>` command (at "
                    "offset " + std::to_string(upperBound) + ").  Move the "
                    "chunk or remove the barrier.";
                return result;
            }
        }
        // Refusal: unmanaged shadow falls AFTER upperBound.
        for( const std::string& name : targetNames ) {
            std::vector<const OverrideRecord*> all = mOverrideSpans.FindAll( name );
            for( const OverrideRecord* r : all ) {
                if( !r->managed && r->chunkEndOffset > upperBound ) {
                    result.status = SaveResult::Status::Refused;
                    result.errorMessage =
                        "managed override block cannot be placed after the "
                        "unmanaged override_object for '" + name + "' at "
                        "offset " + std::to_string(r->chunkEndOffset) +
                        " and before the BARRIER `>` at offset " +
                        std::to_string(upperBound) + ": no valid window exists.";
                    return result;
                }
            }
        }

        // §9.6 rule 5 "replace, don't append" + round-trip stability
        // (§8.8): if an existing managed block already sits in the
        // valid placement window [lowerBound, upperBound], reuse its
        // start offset so a no-edit save produces a byte-identical
        // file.  Otherwise default to upperBound — same upper bound
        // semantics for the first save or when target-set changes
        // force a new location.
        if( existingBlock.found
            && existingBlock.start >= lowerBound
            && existingBlock.start <= upperBound ) {
            blockInsertOffset = existingBlock.start;
        } else {
            blockInsertOffset = upperBound;
        }
    }

    // ---- Step 6: EDITSCRIPT ASSEMBLY --------------------------------
    std::vector<EditOp> editScript = modeAQueue;

    const std::string newBlockText =
        RenderManagedBlock( accumulator, createdChunks, eol );
    // The managed block exists iff it has EITHER override entries OR
    // created-entity chunks.
    const bool haveManagedContent =
        !accumulator.empty() || !createdChunks.empty();

    if( haveManagedContent ) {
        // Build the block-emission edit(s).  newBlockText already
        // ends with a newline (RenderManagedBlock terminates each line
        // with `eol`); no leading newline needed because the
        // managed-block placement always lands AFTER a newline-
        // terminated line (either the preceding chunk's `}` line or
        // the existing block's prior start position).
        if( existingBlock.found && existingBlock.start == blockInsertOffset ) {
            // Case (c) overlapping: one combined replace.  The
            // existing block's byte range INCLUDES its trailing
            // newline (LocateManagedBlock sets `end = closeEnd+1`);
            // newBlockText also ends with eol.  No leading newline.
            EditOp op;
            op.begin = existingBlock.start;
            op.end   = existingBlock.end;
            op.replacement = newBlockText;
            editScript.push_back( op );
        } else {
            // Case (c) disjoint OR case (d): pure insert at new offset,
            // plus an erase of the old block if any.
            EditOp ins;
            ins.begin = blockInsertOffset;
            ins.end   = blockInsertOffset;
            ins.replacement = newBlockText;
            editScript.push_back( ins );
            if( existingBlock.found ) {
                EditOp er;
                er.begin = existingBlock.start;
                er.end   = existingBlock.end;
                er.replacement = "";
                editScript.push_back( er );
            }
        }
    } else if( existingBlock.found ) {
        // Case (b): no managed content (no overrides AND no created
        // entities), but an old block exists — erase it.
        EditOp er;
        er.begin = existingBlock.start;
        er.end   = existingBlock.end;
        er.replacement = "";
        editScript.push_back( er );
    }

    // ---- Step 7: APPLY EditScript -----------------------------------
    // Sort descending by begin (stable for ties).
    // Defensive copy to detect "the editScript ended up being a no-op"
    // (e.g., second save with a retained managed block: the replace
    // edit substitutes identical bytes).  R6 §7 / pinned 2.22: NoOp
    // means "no IO performed; file unchanged."  We check byte-identity
    // post-apply to honour that contract even when the script wasn't
    // logically empty.
    const std::string originalBytes = bytes;

    // R-final P1 #1: build a (threshold, delta) list for post-save
    // SourceSpanIndex offset adjustment.  Captured BEFORE the
    // descending sort because the order of accumulation doesn't
    // matter (cumulative sum is commutative).  Skip zero-delta ops
    // (replacement length == removed length).
    std::vector<OffsetDelta> offsetDeltas;
    offsetDeltas.reserve( editScript.size() );
    for( const EditOp& op : editScript ) {
        const long long d = static_cast<long long>( op.replacement.size() )
                          - static_cast<long long>( op.end - op.begin );
        if( d != 0 ) {
            OffsetDelta od;
            od.threshold = op.end;
            od.delta     = d;
            offsetDeltas.push_back( od );
        }
    }

    std::stable_sort( editScript.begin(), editScript.end(),
        []( const EditOp& a, const EditOp& b ) {
            return a.begin > b.begin;
        } );

    // Round-3 review P3: descending-order apply is only correct when
    // the edit ranges are mutually DISJOINT — an overlap silently
    // corrupts the file.  Every pass is designed to keep them
    // disjoint (transform/property splices target distinct parameter
    // value ranges; the managed block lands in its own placement
    // window; inserts are zero-width).  This is a load-bearing
    // invariant with no single owner, so verify it here: if it ever
    // breaks, refuse the save rather than write garbage.  Ops are
    // sorted by descending `begin`; disjoint ⇒ each op's `end` is
    // ≤ the previous (higher-offset) op's `begin`.  Zero-width
    // inserts sharing an offset are not overlaps.
    for( std::size_t i = 1; i < editScript.size(); ++i ) {
        if( editScript[i].end > editScript[i-1].begin ) {
            result.status = SaveResult::Status::Refused;
            result.errorMessage =
                "internal consistency check failed: overlapping edits "
                "detected — the save was aborted to avoid corrupting "
                "the file.  Please report this scene.";
            return result;
        }
    }

    for( const EditOp& op : editScript ) {
        bytes.replace( op.begin, op.end - op.begin, op.replacement );
    }

    // ---- Step 8: WRITE or NoOp short-circuit ------------------------
    // The NoOp short-circuit applies ONLY to in-place saves: a drag-
    // then-undo leaves DirtyTracker non-empty but Mfinal == Mloaded,
    // so the EditScript ends up empty or splices back the same bytes.
    // For in-place save that's a real no-op (file already matches the
    // would-be output) — honour the R6 §7 / pinned 2.22 "NoOp means
    // no IO" contract.
    //
    // For SAVE-AS the user explicitly asked for a copy at a NEW path.
    // Even when the computed output is byte-identical to the source,
    // we MUST still AtomicWrite the target — otherwise the target
    // file is never created and the GUI silently reports success
    // (2026-05-21 review finding).  The post-write re-anchor below
    // then takes the Save-As path: FileIdentity → target,
    // RemapFilePath(source → target).
    const bool noEditsToWrite =
        editScript.empty() || bytes == originalBytes;
    if( noEditsToWrite && !isSaveAs ) {
        result.status = SaveResult::Status::NoOp;
        mDirty.Clear();
        mScaleFromAnchorSet.clear();
        return result;
    }

    std::string writeError;
    if( !AtomicWrite( filePath, bytes, writeError ) ) {
        result.status = SaveResult::Status::Failed;
        result.errorMessage = writeError;
        return result;
    }

    // R-final P1 #1 (part 1): apply EditScript length deltas to the
    // in-memory SourceSpanIndex so subsequent saves in the same
    // editor session splice at correct (post-write) byte positions.
    if( !offsetDeltas.empty() ) {
        const_cast<SourceSpanIndex&>( mSpans ).ApplyOffsetDeltas( offsetDeltas );
    }

    // R-final-3 P1: rebuild OverrideSpanIndex to mirror what's now
    // on disk.  Two parts, both required for same-session save
    // correctness:
    //
    //   (a) Apply EditScript length deltas to every record's
    //       chunkBeginOffset / chunkEndOffset.  UNMANAGED records
    //       survive verbatim in the file (the engine never touches
    //       their bytes), but Mode A splices on other entities can
    //       still shift their byte positions.  Stale offsets would
    //       later confuse the placement loop's barrier-conflict /
    //       unmanaged-shadow checks.
    //
    //   (b) Wipe MANAGED records and re-seed from `accumulator`.
    //       Without this, a same-session second save with no edits
    //       would see an empty mOverrideSpans (parse-time catalog
    //       had no managed block) + a managed block now on disk,
    //       and Case (b) of the EditScript would erase the block
    //       we just wrote.  The post-save catalog must match
    //       what's actually in the file.
    //
    // Done in this order so (a)'s deltas can land on managed entries
    // too before (b) wipes them — preserves the cross-index invariant
    // "every offset in either span index points at the right byte".
    OverrideSpanIndex& overrideMut =
        const_cast<OverrideSpanIndex&>( mOverrideSpans );
    if( !offsetDeltas.empty() ) {
        overrideMut.ApplyOffsetDeltas( offsetDeltas );
    }
    overrideMut.RemoveAllManaged();
    for( const auto& kv : accumulator ) {
        OverrideRecord rec;
        rec.targetName        = kv.first;
        rec.filePath          = filePath;   // we just wrote to this file
        // chunkBeginOffset / chunkEndOffset are placeholders; downstream
        // consumers (FindManaged value reads, HasUnmanagedFor membership)
        // don't look at byte offsets on managed records.  Set to 0 to
        // make stale-offset bugs surface obviously if anyone ever does.
        rec.chunkBeginOffset  = 0;
        rec.chunkEndOffset    = 0;
        rec.managed           = true;
        rec.hasPosition       = kv.second.hasPosition;
        rec.hasOrientation    = kv.second.hasOrientation;
        rec.hasQuaternion     = kv.second.hasQuaternion;
        rec.hasMatrix         = kv.second.hasMatrix;
        rec.hasScale          = kv.second.hasScale;
        rec.position          = kv.second.position;
        rec.orientation       = kv.second.orientationDeg;
        rec.quaternion[0]     = kv.second.quaternion[0];
        rec.quaternion[1]     = kv.second.quaternion[1];
        rec.quaternion[2]     = kv.second.quaternion[2];
        rec.quaternion[3]     = kv.second.quaternion[3];
        rec.matrix            = kv.second.matrix;
        rec.scale             = kv.second.scale;
        overrideMut.Add( std::move(rec) );
    }

    // R-final P1 #1 (part 2): refresh the load-time transform
    // snapshots to mirror what was just written to disk.  Without
    // this, the next save's `MatricesNearEqual(mFinal, mLoaded)`
    // skip check still compares against the ORIGINAL load-time
    // matrices, so every field that diverged from load-time would
    // be re-routed every save — producing redundant Mode A splices
    // (harmless to byte content but wasteful) and, more importantly,
    // breaking the "drag → save → drag-back → save → NoOp" idempotence
    // because the second drag-back wouldn't compare equal to a
    // post-first-save loaded snapshot that no longer exists.
    //
    // For every name in the dirty set (captured BEFORE the engine
    // clears it), refresh mLoaded[name] = current runtime matrix.
    // For names that landed in MODE A (i.e., not in the final
    // accumulator), also refresh mBase[name] — Mode A modified the
    // standard_object's source line, so the post-save "what the
    // chunk alone produces" baseline now equals the current matrix.
    // For Mode B names (in accumulator), leave mBase alone: the
    // override_object captures the delta from base, and base stays
    // as the original chunk-only state.
    TransformSnapshot* pLoadedMut = mJob.GetLoadedTransformSnapshotMutable();
    TransformSnapshot* pBaseMut   = mJob.GetBaseTransformSnapshotMutable();
    if( pLoadedMut && pBaseMut ) {
        for( const std::string& name : dirtyNames ) {
            IObjectPriv* obj = objs->GetItem( name.c_str() );
            if( !obj ) continue;
            const Matrix4 mNow = obj->GetFinalTransformMatrix();
            pLoadedMut->Add( name, mNow );
            // Mode A path: the chunk's own source line now reflects
            // mNow, so the "base" (chunk-only) matrix also equals
            // mNow.  Mode B path: the chunk source is unchanged; base
            // stays.  Distinguish by checking if the name has a
            // managed entry in the just-emitted accumulator.
            const bool isModeB = accumulator.find( name ) != accumulator.end();
            if( !isModeB ) {
                pBaseMut->Add( name, mNow );
            }
        }
    }

    // Round-1 review P1: refresh `loadedPropertyValues` for every
    // property-dirty entity to mirror what was just written.  The
    // property pass diffs CURRENT introspection against this snapshot;
    // without the refresh the snapshot stays frozen at scene-load
    // state, so an edit-then-revert in a later same-session save
    // diffs equal-to-load and the revert is silently dropped (the
    // property-pass analogue of the transform-snapshot refresh
    // above).  Re-introspect now (post-write the entity's runtime
    // state IS the saved state) and overwrite the map.
    {
        SourceSpanIndex& spansMut = const_cast<SourceSpanIndex&>( mSpans );
        const std::vector<DirtyEntity> propDirty = mDirty.EntitySnapshot();
        for( const DirtyEntity& de : propDirty ) {
            SourceSpan* sp = ( de.first == EntityCategory::Object )
                ? spansMut.FindMutable( de.second )
                : spansMut.FindEntityMutable( de.first, de.second );
            if( !sp ) continue;
            const std::vector<CameraProperty> now =
                InspectEntity( mJob, de.first, de.second );
            sp->loadedPropertyValues.clear();
            for( std::size_t i = 0; i < now.size(); ++i ) {
                sp->loadedPropertyValues[ std::string( now[i].name.c_str() ) ] =
                    std::string( now[i].value.c_str() );
            }
        }
    }

    // R-final P1 #3 + Save-As re-anchor (2026-05-21 review P2):
    // Refresh the stored FileIdentity to the freshly-written file's
    // identity so subsequent saves see the right baseline:
    //   - In-place save: filePath == sourcePath, stat the file we
    //     just wrote, update mtime/size (filePath unchanged).
    //   - Save-As:       filePath != sourcePath, stat the new target,
    //     RE-ANCHOR FileIdentity.filePath to the target path and
    //     update mtime/size.  The next in-place save now reads /
    //     validates against the target, not the original source.
    // Without the Save-As branch the next save would either refuse
    // on identity-mismatch or read the wrong file entirely.
    // Best-effort — if stat fails here, the next save will fail-open
    // (ReadFile will succeed or surface a clearer error).
    if( loadIdent.captured ) {
        struct ::stat post_stat = {};
        if( ::stat( filePath.c_str(), &post_stat ) == 0 ) {
            FileIdentity updated = loadIdent;
            updated.filePath  = filePath;   // re-anchor (no-op on in-place save)
            updated.mtimeSec  = static_cast<long long>( post_stat.st_mtime );
#if defined(__APPLE__)
            updated.mtimeNsec = static_cast<long long>( post_stat.st_mtimespec.tv_nsec );
#elif defined(__linux__) || defined(__unix__)
            updated.mtimeNsec = static_cast<long long>( post_stat.st_mtim.tv_nsec );
#else
            updated.mtimeNsec = 0;
#endif
            updated.sizeBytes = static_cast<long long>( post_stat.st_size );
            // SourceSpanIndex is const-borrowed; cast away const for
            // this update.  The engine holds the only mutator via
            // IJobPriv anyway.
            const_cast<SourceSpanIndex&>( mSpans ).SetFileIdentity( updated );
        }
    }

    // Save-As only: remap per-entity filePath fields in both span
    // indices from the old source path to the new target path.  The
    // unmanaged override chunks and standard_object source spans
    // physically live in the target file now (we wrote them there);
    // their `filePath` metadata must agree so the next save's
    // cross-file refusal compares them against `sourcePath` (which
    // for that next save will equal `filePath`) and they pass.
    if( isSaveAs ) {
        const_cast<SourceSpanIndex&>( mSpans ).RemapFilePath(
            sourcePath, filePath );
        const_cast<OverrideSpanIndex&>( mOverrideSpans ).RemapFilePath(
            sourcePath, filePath );
    }

    mDirty.Clear();
    mScaleFromAnchorSet.clear();
    result.status = SaveResult::Status::Saved;
    return result;
}

}  // namespace RISE
