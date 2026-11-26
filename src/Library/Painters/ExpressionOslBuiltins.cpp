//////////////////////////////////////////////////////////////////////
//
//  ExpressionOslBuiltins.cpp - Signature table and dispatch for the
//    expression VM's `osl_` builtins.  See ExpressionOslBuiltins.h.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ExpressionOslBuiltins.h"
#include "../Utilities/OslNoise.h"
#include "../Utilities/OslVoronoi.h"
#include "../Utilities/OslColor.h"
#include "../Utilities/OslMath.h"
#include "../Utilities/OslVector.h"
#include "../Utilities/OslShaderNodes.h"
#include "../Utilities/FiniteMath.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/SurfaceSignalProximity.h"
#include "../Interfaces/ICamera.h"
#include "../Cameras/CameraCommon.h"
#include <atomic>
#include <cmath>
#include <cstdint>

namespace RISE
{
	// Single-TU AO self-primitive exclusion demand counter & TLS (Wave C H9/H10):
	// never duplicated across DSO boundaries; zero-cost atomic check on non-AO scenes.
	std::atomic<unsigned int> g_surfaceSignalSelfExclusionActive( 0u );

	SelfPrimExclusionState& SurfaceSignalSelfExclusion()
	{
		static thread_local SelfPrimExclusionState s_state;
		return s_state;
	}

	bool& SurfaceSignalInAmbientOcclusion()
	{
		static thread_local bool s_inAO = false;
		return s_inAO;
	}

	namespace Implementation
	{
		namespace OslBuiltins
		{
			namespace
			{
				enum Id
				{
					kIdSNoise = 1,
					kIdFractal,
					kIdNoiseTexture,
					kIdNoiseTextureColor,
					kIdWhiteNoise,
					kIdWhiteNoiseColor,
					kIdVoronoiDistance,
					kIdVoronoiColor,
					kIdVoronoiPosition,
					kIdVoronoiW,
					kIdVoronoiRadius,
					kIdRgb2Hsv,
					kIdHsv2Rgb,
					kIdRgb2Hsl,
					kIdHsl2Rgb,
					kIdMixColor,
					kIdMath,
					kIdVecMathScalar,
					kIdVecMath,
					kIdClamp,
					kIdMapRange,
					kIdVecMapRange,
					kIdVMin,
					kIdVMax,
					kIdVAbs,
					kIdVFloor,
					kIdVFract,
					kIdVPow,
					kIdVClamp,
					kIdRotate,
					kIdVectorRotate,
					kIdToObjectPoint,
					kIdToObjectVector,
					kIdToObjectNormal,
					kIdToWorldPoint,
					kIdToWorldVector,
					kIdToWorldNormal,
					kIdToCameraPoint,
					kIdToCameraVector,
					kIdToCameraNormal,
					kIdCameraToWorldPoint,
					kIdCameraToWorldVector,
					kIdCameraToWorldNormal,
					kIdObjectColor,
					kIdObjectAlpha,
					kIdObjectIndex,
					kIdObjectRandom,
					kIdAmbientOcclusion,
					kIdBackfacing,
					kIdTrueNormal,
					kIdShadingNormal,
					kIdParticleRandom,
					kIdGradient,
					kIdBrick,
					kIdLayerWeight,
					kIdFresnel,
					kIdIsCameraRay
				};

				const ArgKind S = kScalarArg, V = kVec3Arg, L = kIntLiteralArg;

				// Per argument: its kind, then the literal range (litMin,
				// litMax) -- 0..0 for a non-literal slot, which nothing reads.
				// dims is 1..4, type 0..4 (OslNoise::FractalType), normalize
				// 0..1, feature 0..4 (OslVoronoi::Feature), metric 0..3
				// (OslVoronoi::Metric), mode 0..18 (OslColor::MixMode),
				// clamp_factor/clamp_result 0..1.
				const Sig kTable[] = {
					{ "osl_snoise", kIdSNoise, 3,
						{ V, S, L },
						{ 0, 0, 1 },
						{ 0, 0, 4 },
						false, true, false },
					{ "osl_fractal", kIdFractal, 10,
						{ V, S, L, L, S, S, S, S, S, L },
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 },
						{ 0, 0, 4, OslNoise::kFractalTypeCount - 1, 0, 0, 0, 0, 0, 1 },
						false, true, false },
					{ "osl_noise_texture", kIdNoiseTexture, 11,
						{ V, S, L, L, S, S, S, S, S, S, L },
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
						{ 0, 0, 4, OslNoise::kFractalTypeCount - 1, 0, 0, 0, 0, 0, 0, 1 },
						false, true, false },
					{ "osl_noise_texture_color", kIdNoiseTextureColor, 11,
						{ V, S, L, L, S, S, S, S, S, S, L },
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
						{ 0, 0, 4, OslNoise::kFractalTypeCount - 1, 0, 0, 0, 0, 0, 0, 1 },
						true, true, false },
					// A few integer hashes, judged too cheap to be worth a
					// memo lookup (not measured; cellhash is treated the same).
					{ "osl_white_noise", kIdWhiteNoise, 3,
						{ V, S, L },
						{ 0, 0, 1 },
						{ 0, 0, 4 },
						false, false, false },
					{ "osl_white_noise_color", kIdWhiteNoiseColor, 3,
						{ V, S, L },
						{ 0, 0, 1 },
						{ 0, 0, 4 },
						true, false, false },
					// The Voronoi node's five outputs: (p, w, dims, feature,
					// metric, scale, detail, roughness, lacunarity, smoothness,
					// exponent, randomness, normalize).  Each runs the whole
					// node (one or two cell searches per octave); memo-worthy.
#define OSL_VORONOI_SIG( name, id, vec3 ) \
					{ name, id, 13, \
						{ V, S, L, L, L, S, S, S, S, S, S, S, L }, \
						{ 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, \
						{ 0, 0, 4, OslVoronoi::kFeatureCount - 1, OslVoronoi::kMetricCount - 1, 0, 0, 0, 0, 0, 0, 0, 1 }, \
						vec3, true, false }
					OSL_VORONOI_SIG( "osl_voronoi_distance", kIdVoronoiDistance, false ),
					OSL_VORONOI_SIG( "osl_voronoi_color", kIdVoronoiColor, true ),
					OSL_VORONOI_SIG( "osl_voronoi_position", kIdVoronoiPosition, true ),
					OSL_VORONOI_SIG( "osl_voronoi_w", kIdVoronoiW, false ),
					OSL_VORONOI_SIG( "osl_voronoi_radius", kIdVoronoiRadius, false ),
#undef OSL_VORONOI_SIG
					// Colour conversions and Mix Color: straight arithmetic
					// with no loops or hashes, too cheap to be worth a memo
					// lookup (see OslColor.h, COST).
					{ "osl_rgb2hsv", kIdRgb2Hsv, 1,
						{ V }, { 0 }, { 0 }, true, false, false },
					{ "osl_hsv2rgb", kIdHsv2Rgb, 1,
						{ V }, { 0 }, { 0 }, true, false, false },
					{ "osl_rgb2hsl", kIdRgb2Hsl, 1,
						{ V }, { 0 }, { 0 }, true, false, false },
					{ "osl_hsl2rgb", kIdHsl2Rgb, 1,
						{ V }, { 0 }, { 0 }, true, false, false },
					// (mode, t, a, b, clamp_factor, clamp_result)
					{ "osl_mix_color", kIdMixColor, 6,
						{ L, S, V, V, L, L },
						{ 0, 0, 0, 0, 0, 0 },
						{ OslColor::kMixModeCount - 1, 0, 0, 0, 1, 1 },
						true, false, false },
					// Group 5: Safe Math, Vector Math, Clamp, Map Range (not memo-worthy).
					// (op, a, b, c)
					{ "osl_math", kIdMath, 4,
						{ L, S, S, S },
						{ 0, 0, 0, 0 },
						{ OslMath::kMathOpCount - 1, 0, 0, 0 },
						false, false, false },
					// (op, a, b) where op in {7=Dot, 8=Distance, 9=Length}
					{ "osl_vmath_scalar", kIdVecMathScalar, 3,
						{ L, V, V },
						{ OslMath::eVecDot, 0, 0 },
						{ OslMath::eVecLength, 0, 0 },
						false, false, false },
					// (op, a, b, c, scale)
					{ "osl_vmath", kIdVecMath, 5,
						{ L, V, V, V, S },
						{ 0, 0, 0, 0, 0 },
						{ OslMath::kVecMathOpCount - 1, 0, 0, 0, 0 },
						true, false, false },
					// (type, v, mn, mx)
					{ "osl_clamp", kIdClamp, 4,
						{ L, S, S, S },
						{ 0, 0, 0, 0 },
						{ OslMath::kClampTypeCount - 1, 0, 0, 0 },
						false, false, false },
					// (type, clamp, v, fmin, fmax, tmin, tmax, steps)
					{ "osl_map_range", kIdMapRange, 8,
						{ L, L, S, S, S, S, S, S },
						{ 0, 0, 0, 0, 0, 0, 0, 0 },
						{ OslMath::kMapRangeTypeCount - 1, 1, 0, 0, 0, 0, 0, 0 },
						false, false, false },
					{ "osl_vmap_range", kIdVecMapRange, 8,
						{ L, L, V, V, V, V, V, V },
						{ 0, 0, 0, 0, 0, 0, 0, 0 },
						{ OslMath::kMapRangeTypeCount - 1, 1, 0, 0, 0, 0, 0, 0 },
						true, false, false },
					// Group 6: Componentwise vec3 ops and Vector Rotate (not memo-worthy).
					{ "osl_vmin", kIdVMin, 2,
						{ V, V }, { 0, 0 }, { 0, 0 }, true, false, false },
					{ "osl_vmax", kIdVMax, 2,
						{ V, V }, { 0, 0 }, { 0, 0 }, true, false, false },
					{ "osl_vabs", kIdVAbs, 1,
						{ V }, { 0 }, { 0 }, true, false, false },
					{ "osl_vfloor", kIdVFloor, 1,
						{ V }, { 0 }, { 0 }, true, false, false },
					{ "osl_vfract", kIdVFract, 1,
						{ V }, { 0 }, { 0 }, true, false, false },
					{ "osl_vpow", kIdVPow, 2,
						{ V, V }, { 0, 0 }, { 0, 0 }, true, false, false },
					{ "osl_vclamp", kIdVClamp, 3,
						{ V, V, V }, { 0, 0, 0 }, { 0, 0, 0 }, true, false, false },
					{ "osl_rotate", kIdRotate, 3,
						{ V, V, S }, { 0, 0, 0 }, { 0, 0, 0 }, true, false, false },
					// (type, invert, v, center, axis, angle, euler)
					{ "osl_vector_rotate", kIdVectorRotate, 7,
						{ L, L, V, V, V, S, V },
						{ 0, 0, 0, 0, 0, 0, 0 },
						{ OslVector::kRotateTypeCount - 1, 1, 0, 0, 0, 0, 0 },
						true, false, false },
					// Group 7: Live space transforms (require hit context; not memo-worthy).
					{ "osl_to_object_point",       kIdToObjectPoint,       1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_to_object_vector",      kIdToObjectVector,      1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_to_object_normal",      kIdToObjectNormal,      1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_to_world_point",        kIdToWorldPoint,        1, { V }, { 0 }, { 0 }, true, false, true, false, false, 0u, true },
					{ "osl_to_world_vector",       kIdToWorldVector,       1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_to_world_normal",       kIdToWorldNormal,       1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_to_camera_point",       kIdToCameraPoint,       1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_to_camera_vector",      kIdToCameraVector,      1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_to_camera_normal",      kIdToCameraNormal,      1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_camera_to_world_point", kIdCameraToWorldPoint,  1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_camera_to_world_vector",kIdCameraToWorldVector, 1, { V }, { 0 }, { 0 }, true, false, true },
					{ "osl_camera_to_world_normal",kIdCameraToWorldNormal, 1, { V }, { 0 }, { 0 }, true, false, true },
					// Per-object / per-instance Cycles Object Info attributes (require hit context; not memo-worthy).
					{ "osl_object_color",          kIdObjectColor,         0, { S }, { 0 }, { 0 }, true,  false, true },
					{ "osl_object_alpha",          kIdObjectAlpha,         0, { S }, { 0 }, { 0 }, false, false, true },
					{ "osl_object_index",          kIdObjectIndex,         0, { S }, { 0 }, { 0 }, false, false, true },
					{ "osl_object_random",         kIdObjectRandom,        0, { S }, { 0 }, { 0 }, false, false, true },
					// Ray-traced ambient occlusion: (distance, samples, normal, flags) -> scalar in [0, 1].
					{ "osl_ao",                    kIdAmbientOcclusion,    4,
						{ S, L, V, L },
						{ 0, 1, 0, 0 },
						{ 0, 128, 0, 7 },
						false, true, true, true },
					// Geometry / particle hit-context queries (not memo-worthy; keyed in SpaceTransformHitKey).
					{ "osl_backfacing",            kIdBackfacing,          0, { S }, { 0 }, { 0 }, false, false, true, false, false, PathDependence::kBackfacing },
					{ "osl_true_normal",           kIdTrueNormal,          0, { S }, { 0 }, { 0 }, true,  false, true },
					{ "osl_shading_normal",        kIdShadingNormal,       0, { S }, { 0 }, { 0 }, true,  false, true },
					{ "osl_particle_random",       kIdParticleRandom,      0, { S }, { 0 }, { 0 }, false, false, true },
					// Cycles Light Path.Is Camera Ray (keyed in SpaceTransformHitKey::isCameraRay).
					{ "osl_is_camera_ray",         kIdIsCameraRay,         0, { S }, { 0 }, { 0 }, false, false, true, false, false, PathDependence::kCameraRay },
					// Cycles Layer Weight: (blend, normal, flags: bit0=Facing, bit1=hasCustomNormal) -> scalar.
					{ "osl_layer_weight",          kIdLayerWeight,         3,
						{ S, V, L },
						{ 0, 0, 0 },
						{ 0, 0, 3 },
						false, false, true, false, true, PathDependence::kViewDirection },
					// Cycles Fresnel: (ior, normal, hasCustomNormal 0..1) -> scalar.
					{ "osl_fresnel",               kIdFresnel,             3,
						{ S, V, L },
						{ 0, 0, 0 },
						{ 0, 0, 1 },
						false, false, true, false, true, PathDependence::kViewDirection },
					// Cycles Gradient Texture: (p, type 0..6) -> scalar in [0, 1].
					{ "osl_gradient",              kIdGradient,            2,
						{ V, L },
						{ 0, 0 },
						{ 0, 6 },
						false, false, false },
					// Cycles Brick Texture kernel: (p, mortar_size, mortar_smooth, bias,
					// brick_width, row_height, offset_amount, offset_freq, squash_amount,
					// squash_freq) -> vec3(tint, mortar, 0).
					{ "osl_brick",                 kIdBrick,               10,
						{ V, S, S, S, S, S, S, L, S, L },
						{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
						{ 0, 0, 0, 0, 0, 0, 0, 1000000, 0, 1000000 },
						true, false, false },
				};

				const int kTableSize = (int)( sizeof( kTable ) / sizeof( kTable[0] ) );

				//! The coordinates `dims` names, from the (p.x, p.y, p.z, w)
				//! the call pushed.  See the header's table.
				void Gather( const Scalar* a, const int dims, Scalar* c )
				{
					c[0] = c[1] = c[2] = c[3] = Scalar(0);
					switch( dims ) {
					case 1: c[0] = a[3]; break;
					case 2: c[0] = a[0]; c[1] = a[1]; break;
					case 3: c[0] = a[0]; c[1] = a[1]; c[2] = a[2]; break;
					case 4: c[0] = a[0]; c[1] = a[1]; c[2] = a[2]; c[3] = a[3]; break;
					default: break;
					}
				}

				//! A literal argument as the int the compiler proved it is.
				//! The range was checked at compile time; this rounds rather
				//! than truncates only so a value that is not exactly an
				//! integer (impossible from the compiler) still lands on the
				//! nearest one instead of drifting down.
				inline int Lit( const Scalar x )
				{
					return (int)( x + ( x >= Scalar(0) ? Scalar(0.5) : Scalar(-0.5) ) );
				}

				inline Scalar Finite( const Scalar x )
				{
					return IsFiniteDouble( x ) ? x : Scalar(0);
				}

				inline Vector3 SafeNormalize3( const Vector3& v )
				{
					const Scalar lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
					if( !( lenSq > Scalar(0) ) || !IsFiniteDouble( static_cast<double>( lenSq ) ) ) {
						return Vector3( 0, 0, 0 );
					}
					const Scalar inv = Scalar(1) / std::sqrt( lenSq );
					return Vector3( v.x * inv, v.y * inv, v.z * inv );
				}

				// Applies the transpose of the linear 3x3 block of a RISE
				// row-vector Matrix4 to a normal: n' = n * M^T (i.e. M * n
				// in column-vector notation).
				inline Vector3 TransformTransposeDir( const Matrix4& m, const Vector3& n )
				{
					return Vector3(
						m._00 * n.x + m._01 * n.y + m._02 * n.z,
						m._10 * n.x + m._11 * n.y + m._12 * n.z,
						m._20 * n.x + m._21 * n.y + m._22 * n.z );
				}
			}

			const Sig* Find( const std::string& name )
			{
				for( int i = 0; i < kTableSize; ++i ) {
					if( name == kTable[i].name ) return &kTable[i];
				}
				return 0;
			}

			const Sig* FindById( const int id )
			{
				for( int i = 0; i < kTableSize; ++i ) {
					if( kTable[i].id == id ) return &kTable[i];
				}
				return 0;
			}

			int ScalarArity( const Sig& s )
			{
				int n = 0;
				for( int i = 0; i < s.nArgs; ++i ) n += ( s.kind[i] == kVec3Arg ) ? 3 : 1;
				return n;
			}

			void Call( const int id, const Scalar* a, Scalar* out,
				const RayIntersectionGeometric* pHit,
				const SurfaceSignalInfo* pSignals,
				const Vector3* pFallbackNormal )
			{
				switch( id )
				{
				case kIdSNoise:
				{
					// (p, w, dims)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					out[0] = Finite( OslNoise::SNoise( dims, c ) );
					return;
				}
				case kIdFractal:
				{
					// (p, w, dims, type, detail, roughness, lacunarity, offset, gain, normalize)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					out[0] = Finite( OslNoise::Fractal( dims, c, Lit( a[5] ),
						a[6], a[7], a[8], a[9], a[10], Lit( a[11] ) != 0 ) );
					return;
				}
				case kIdNoiseTexture:
				case kIdNoiseTextureColor:
				{
					// (p, w, dims, type, detail, roughness, lacunarity, offset, gain, distortion, normalize)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					Scalar fac = Scalar(0);
					Scalar color[3] = { Scalar(0), Scalar(0), Scalar(0) };
					const bool wantColor = ( id == kIdNoiseTextureColor );
					OslNoise::NoiseTexture( dims, c, Lit( a[5] ),
						a[6], a[7], a[8], a[9], a[10], a[11], Lit( a[12] ) != 0,
						fac, wantColor ? color : 0 );
					if( wantColor ) {
						out[0] = Finite( color[0] );
						out[1] = Finite( color[1] );
						out[2] = Finite( color[2] );
					} else {
						out[0] = Finite( fac );
					}
					return;
				}
				case kIdWhiteNoise:
				case kIdWhiteNoiseColor:
				{
					// (p, w, dims)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					Scalar value = Scalar(0);
					Scalar color[3] = { Scalar(0), Scalar(0), Scalar(0) };
					const bool wantColor = ( id == kIdWhiteNoiseColor );
					OslNoise::WhiteNoise( dims, c, value, wantColor ? color : 0 );
					if( wantColor ) {
						out[0] = color[0]; out[1] = color[1]; out[2] = color[2];
					} else {
						out[0] = value;
					}
					return;
				}
				case kIdVoronoiDistance:
				case kIdVoronoiColor:
				case kIdVoronoiPosition:
				case kIdVoronoiW:
				case kIdVoronoiRadius:
				{
					// (p, w, dims, feature, metric, scale, detail, roughness,
					//  lacunarity, smoothness, exponent, randomness, normalize)
					Scalar c[4];
					const int dims = Lit( a[4] );
					Gather( a, dims, c );
					OslVoronoi::Result r;
					OslVoronoi::VoronoiTexture( dims, c, Lit( a[5] ), Lit( a[6] ),
						a[7], a[8], a[9], a[10], a[11], a[12], a[13], Lit( a[14] ) != 0, r );
					out[0] = out[1] = out[2] = Scalar(0);
					switch( id ) {
					case kIdVoronoiDistance: out[0] = Finite( r.distance ); break;
					case kIdVoronoiColor:
						out[0] = Finite( r.color[0] ); out[1] = Finite( r.color[1] ); out[2] = Finite( r.color[2] );
						break;
					case kIdVoronoiPosition:
						out[0] = Finite( r.position[0] ); out[1] = Finite( r.position[1] ); out[2] = Finite( r.position[2] );
						break;
					case kIdVoronoiW: out[0] = Finite( r.w ); break;
					default: out[0] = Finite( r.radius ); break;
					}
					return;
				}
				case kIdRgb2Hsv:
				case kIdHsv2Rgb:
				case kIdRgb2Hsl:
				case kIdHsl2Rgb:
				{
					// (c) -> a[0..2]
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					switch( id ) {
					case kIdRgb2Hsv: OslColor::RgbToHsv( a, r ); break;
					case kIdHsv2Rgb: OslColor::HsvToRgb( a, r ); break;
					case kIdRgb2Hsl: OslColor::RgbToHsl( a, r ); break;
					default:         OslColor::HslToRgb( a, r ); break;
					}
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdMixColor:
				{
					// (mode, t, a, b, clamp_factor, clamp_result)
					// popped scalars: mode=a[0], t=a[1], a=a[2..4], b=a[5..7],
					// clamp_factor=a[8], clamp_result=a[9].
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					OslColor::MixColor( Lit( a[0] ), a[1], a + 2, a + 5,
						Lit( a[8] ) != 0, Lit( a[9] ) != 0, r );
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdMath:
				{
					// (op, a, b, c)
					out[0] = Finite( OslMath::Math( Lit( a[0] ), a[1], a[2], a[3] ) );
					return;
				}
				case kIdVecMathScalar:
				{
					// (op, a, b) -> a[0], a[1..3], a[4..6]
					out[0] = Finite( OslMath::VectorMathScalar( Lit( a[0] ), a + 1, a + 4 ) );
					return;
				}
				case kIdVecMath:
				{
					// (op, a, b, c, scale) -> a[0], a[1..3], a[4..6], a[7..9], a[10]
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					OslMath::VectorMathVector( Lit( a[0] ), a + 1, a + 4, a + 7, a[10], r );
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdClamp:
				{
					// (type, v, mn, mx)
					out[0] = Finite( OslMath::Clamp( Lit( a[0] ), a[1], a[2], a[3] ) );
					return;
				}
				case kIdMapRange:
				{
					// (type, clamp, v, fmin, fmax, tmin, tmax, steps)
					out[0] = Finite( OslMath::MapRange(
						Lit( a[0] ), Lit( a[1] ) != 0,
						a[2], a[3], a[4], a[5], a[6], a[7] ) );
					return;
				}
				case kIdVecMapRange:
				{
					// (type, clamp, v, fmin, fmax, tmin, tmax, steps)
					// a[0], a[1], a[2..4], a[5..7], a[8..10], a[11..13], a[14..16], a[17..19]
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					OslMath::VectorMapRange(
						Lit( a[0] ), Lit( a[1] ) != 0,
						a + 2, a + 5, a + 8, a + 11, a + 14, a + 17, r );
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdVMin:
				case kIdVMax:
				case kIdVAbs:
				case kIdVFloor:
				case kIdVFract:
				case kIdVPow:
				case kIdVClamp:
				{
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					switch( id ) {
					case kIdVMin:   OslVector::VecMin( a, a + 3, r ); break;
					case kIdVMax:   OslVector::VecMax( a, a + 3, r ); break;
					case kIdVAbs:   OslVector::VecAbs( a, r ); break;
					case kIdVFloor: OslVector::VecFloor( a, r ); break;
					case kIdVFract: OslVector::VecFract( a, r ); break;
					case kIdVPow:   OslVector::VecPow( a, a + 3, r ); break;
					default:        OslVector::VecClamp( a, a + 3, a + 6, r ); break;
					}
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdRotate:
				{
					// (v, axis, angle) -> a[0..2], a[3..5], a[6]
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					OslVector::RotateAroundAxis( a, a + 3, a[6], r );
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdVectorRotate:
				{
					// (type, invert, v, center, axis, angle, euler)
					// a[0], a[1], a[2..4], a[5..7], a[8..10], a[11], a[12..14]
					Scalar r[3] = { Scalar(0), Scalar(0), Scalar(0) };
					OslVector::VectorRotate(
						Lit( a[0] ), Lit( a[1] ) != 0,
						a + 2, a + 5, a + 8, a[11], a + 12, r );
					out[0] = Finite( r[0] );
					out[1] = Finite( r[1] );
					out[2] = Finite( r[2] );
					return;
				}
				case kIdToObjectPoint:
				{
					Point3 p( a[0], a[1], a[2] );
					if( pHit && pHit->pmxWorldToObject ) {
						p = Point3Ops::Transform( *pHit->pmxWorldToObject, p );
					}
					out[0] = Finite( p.x ); out[1] = Finite( p.y ); out[2] = Finite( p.z );
					return;
				}
				case kIdToObjectVector:
				{
					Vector3 v( a[0], a[1], a[2] );
					if( pHit && pHit->pmxWorldToObject ) {
						v = Vector3Ops::Transform( *pHit->pmxWorldToObject, v );
					}
					out[0] = Finite( v.x ); out[1] = Finite( v.y ); out[2] = Finite( v.z );
					return;
				}
				case kIdToObjectNormal:
				{
					Vector3 n( a[0], a[1], a[2] );
					if( pHit && pHit->pmxObjectToWorld ) {
						n = SafeNormalize3( TransformTransposeDir( *pHit->pmxObjectToWorld, n ) );
					} else {
						n = SafeNormalize3( n );
					}
					out[0] = Finite( n.x ); out[1] = Finite( n.y ); out[2] = Finite( n.z );
					return;
				}
				case kIdToWorldPoint:
				{
					Point3 p( a[0], a[1], a[2] );
					if( pHit && pHit->pmxObjectToWorld ) {
						p = Point3Ops::Transform( *pHit->pmxObjectToWorld, p );
					}
					out[0] = Finite( p.x ); out[1] = Finite( p.y ); out[2] = Finite( p.z );
					return;
				}
				case kIdToWorldVector:
				{
					Vector3 v( a[0], a[1], a[2] );
					if( pHit && pHit->pmxObjectToWorld ) {
						v = Vector3Ops::Transform( *pHit->pmxObjectToWorld, v );
					}
					out[0] = Finite( v.x ); out[1] = Finite( v.y ); out[2] = Finite( v.z );
					return;
				}
				case kIdToWorldNormal:
				{
					Vector3 n( a[0], a[1], a[2] );
					if( pHit && pHit->pmxWorldToObject ) {
						n = SafeNormalize3( TransformTransposeDir( *pHit->pmxWorldToObject, n ) );
					} else {
						n = SafeNormalize3( n );
					}
					out[0] = Finite( n.x ); out[1] = Finite( n.y ); out[2] = Finite( n.z );
					return;
				}
				case kIdToCameraPoint:
				{
					Vector3 res( a[0], a[1], a[2] );
					Point3 origin;
					Vector3 right, up, forward;
					if( pHit && QueryCameraFrame( pHit->pCamera, origin, right, up, forward ) ) {
						const Vector3 d( a[0] - origin.x, a[1] - origin.y, a[2] - origin.z );
						res = Vector3( Vector3Ops::Dot( d, right ), Vector3Ops::Dot( d, up ), Vector3Ops::Dot( d, forward ) );
					}
					out[0] = Finite( res.x ); out[1] = Finite( res.y ); out[2] = Finite( res.z );
					return;
				}
				case kIdToCameraVector:
				{
					Vector3 res( a[0], a[1], a[2] );
					Point3 origin;
					Vector3 right, up, forward;
					if( pHit && QueryCameraFrame( pHit->pCamera, origin, right, up, forward ) ) {
						const Vector3 v( a[0], a[1], a[2] );
						res = Vector3( Vector3Ops::Dot( v, right ), Vector3Ops::Dot( v, up ), Vector3Ops::Dot( v, forward ) );
					}
					out[0] = Finite( res.x ); out[1] = Finite( res.y ); out[2] = Finite( res.z );
					return;
				}
				case kIdToCameraNormal:
				{
					Vector3 res( a[0], a[1], a[2] );
					Point3 origin;
					Vector3 right, up, forward;
					if( pHit && QueryCameraFrame( pHit->pCamera, origin, right, up, forward ) ) {
						const Vector3 n( a[0], a[1], a[2] );
						res = SafeNormalize3( Vector3( Vector3Ops::Dot( n, right ), Vector3Ops::Dot( n, up ), Vector3Ops::Dot( n, forward ) ) );
					} else {
						res = SafeNormalize3( res );
					}
					out[0] = Finite( res.x ); out[1] = Finite( res.y ); out[2] = Finite( res.z );
					return;
				}
				case kIdCameraToWorldPoint:
				{
					Vector3 res( a[0], a[1], a[2] );
					Point3 origin;
					Vector3 right, up, forward;
					if( pHit && QueryCameraFrame( pHit->pCamera, origin, right, up, forward ) ) {
						res = Vector3( origin.x, origin.y, origin.z ) + right * a[0] + up * a[1] + forward * a[2];
					}
					out[0] = Finite( res.x ); out[1] = Finite( res.y ); out[2] = Finite( res.z );
					return;
				}
				case kIdCameraToWorldVector:
				{
					Vector3 res( a[0], a[1], a[2] );
					Point3 origin;
					Vector3 right, up, forward;
					if( pHit && QueryCameraFrame( pHit->pCamera, origin, right, up, forward ) ) {
						res = right * a[0] + up * a[1] + forward * a[2];
					}
					out[0] = Finite( res.x ); out[1] = Finite( res.y ); out[2] = Finite( res.z );
					return;
				}
				case kIdCameraToWorldNormal:
				{
					Vector3 res( a[0], a[1], a[2] );
					Point3 origin;
					Vector3 right, up, forward;
					if( pHit && QueryCameraFrame( pHit->pCamera, origin, right, up, forward ) ) {
						res = SafeNormalize3( right * a[0] + up * a[1] + forward * a[2] );
					} else {
						res = SafeNormalize3( res );
					}
					out[0] = Finite( res.x ); out[1] = Finite( res.y ); out[2] = Finite( res.z );
					return;
				}
				case kIdObjectColor:
				{
					if( pHit && pHit->pObjectInfo ) {
						out[0] = Finite( static_cast<Scalar>( pHit->pObjectInfo->color[0] ) );
						out[1] = Finite( static_cast<Scalar>( pHit->pObjectInfo->color[1] ) );
						out[2] = Finite( static_cast<Scalar>( pHit->pObjectInfo->color[2] ) );
					} else {
						out[0] = out[1] = out[2] = Scalar(0);
					}
					return;
				}
				case kIdObjectAlpha:
				{
					out[0] = ( pHit && pHit->pObjectInfo )
						? Finite( static_cast<Scalar>( pHit->pObjectInfo->alpha ) )
						: Scalar(0);
					return;
				}
				case kIdObjectIndex:
				{
					out[0] = ( pHit && pHit->pObjectInfo )
						? Finite( static_cast<Scalar>( pHit->pObjectInfo->objectIndex ) )
						: Scalar(0);
					return;
				}
				case kIdObjectRandom:
				{
					out[0] = ( pHit && pHit->pObjectInfo )
						? Finite( static_cast<Scalar>( pHit->pObjectInfo->random ) )
						: Scalar(0);
					return;
				}
				case kIdAmbientOcclusion:
				{
					// (distance, samples, normal(3), flags)
					const SurfaceSignalInfo* sig = pSignals ? pSignals : ( pHit ? &pHit->signals : nullptr );
					if( !sig ) {
						out[0] = Scalar(1);
						return;
					}
					// Use the PRE-modifier shading normal N0 (Cycles sd->N parity,
					// MIX §III.3 Decision 12) when a normal-perturbing modifier
					// has recorded its pre-modifier frame; fall back to ctx.N when
					// pHit is absent (direct ExpressionProgram::Eval(ctx)).
					const Vector3 shadingN = pHit
						? ( pHit->bHasNormalMapFrame ? pHit->normalMapOnb.w()
						  : ( pHit->bHasCoatDecodeOnb ? pHit->coatDecodeOnb.w() : pHit->vNormal ) )
						: ( pFallbackNormal ? *pFallbackNormal : Vector3( 0, 0, 0 ) );
					const Vector3 geomN    = pHit ? pHit->vGeomNormal : Vector3( 0, 0, 0 );
					const bool opposesArr  = pHit ? pHit->GeomNormalOpposesArrival() : true;
					out[0] = Finite( sig->AmbientOcclusion(
						a[0], Lit( a[1] ), Vector3( a[2], a[3], a[4] ), Lit( a[5] ),
						shadingN, geomN, opposesArr ) );
					return;
				}
				case kIdBackfacing:
				{
					const bool backfacing = pHit && pHit->HasTrueGeomSide()
						&& ( pHit->bGeomNormalOrientedToRay
							? pHit->GeomNormalOpposesArrival()
							: !pHit->GeomNormalOpposesArrival() );
					out[0] = backfacing ? Scalar(1) : Scalar(0);
					return;
				}
				case kIdTrueNormal:
				{
					if( pHit ) {
						const Vector3 ng = pHit->GeomNormalOpposesArrival()
							? pHit->vGeomNormal : -pHit->vGeomNormal;
						out[0] = Finite( ng.x );
						out[1] = Finite( ng.y );
						out[2] = Finite( ng.z );
					} else {
						out[0] = out[1] = out[2] = Scalar(0);
					}
					return;
				}
				case kIdShadingNormal:
				{
					const Vector3 shadingN = pHit
						? ( pHit->bHasNormalMapFrame ? pHit->normalMapOnb.w()
						  : ( pHit->bHasCoatDecodeOnb ? pHit->coatDecodeOnb.w() : pHit->vNormal ) )
						: ( pFallbackNormal ? *pFallbackNormal : Vector3( 0, 0, 0 ) );
					out[0] = Finite( shadingN.x );
					out[1] = Finite( shadingN.y );
					out[2] = Finite( shadingN.z );
					return;
				}
				case kIdParticleRandom:
				{
					out[0] = ( pHit && pHit->pObjectInfo )
						? Finite( static_cast<Scalar>( pHit->pObjectInfo->particleRandom ) )
						: Scalar(0);
					return;
				}
				case kIdGradient:
				{
					const float f = OslShaderNodes::Gradient(
						static_cast<float>( a[0] ),
						static_cast<float>( a[1] ),
						static_cast<float>( a[2] ),
						Lit( a[3] ) );
					out[0] = static_cast<Scalar>( f );
					return;
				}
				case kIdBrick:
				{
					for( int i = 0; i < 12; ++i ) {
						if( !IsFiniteDouble( a[i] ) ) {
							out[0] = out[1] = out[2] = Scalar(0);
							return;
						}
					}
					float tint = 0.0f, mortar = 0.0f;
					OslShaderNodes::Brick(
						static_cast<float>( a[0] ),
						static_cast<float>( a[1] ),
						static_cast<float>( a[3] ),
						static_cast<float>( a[4] ),
						static_cast<float>( a[5] ),
						static_cast<float>( a[6] ),
						static_cast<float>( a[7] ),
						static_cast<float>( a[8] ),
						Lit( a[9] ),
						static_cast<float>( a[10] ),
						Lit( a[11] ),
						tint,
						mortar );
					out[0] = Finite( static_cast<Scalar>( tint ) );
					out[1] = Finite( static_cast<Scalar>( mortar ) );
					out[2] = Scalar(0);
					return;
				}
				case kIdIsCameraRay:
				{
					out[0] = ( pHit && pHit->bIsCameraRay ) ? Scalar(1) : Scalar(0);
					return;
				}
				case kIdLayerWeight:
				case kIdFresnel:
				{
					const int flags = Lit( a[4] );
					const bool hasCustomN = ( id == kIdLayerWeight ) ? ( ( flags & 2 ) != 0 ) : ( flags != 0 );
					Vector3 nIn;
					if( hasCustomN ) {
						nIn = Vector3( Finite( a[1] ), Finite( a[2] ), Finite( a[3] ) );
					} else if( pHit ) {
						nIn = pHit->bHasNormalMapFrame ? pHit->normalMapOnb.w()
							: ( pHit->bHasCoatDecodeOnb ? pHit->coatDecodeOnb.w() : pHit->vNormal );
					} else if( pFallbackNormal ) {
						nIn = *pFallbackNormal;
					} else {
						nIn = Vector3( 0, 0, 1 );
					}

					Vector3 wi( 0, 0, 1 );
					bool backfacing = false;
					if( pHit ) {
						const Vector3 d = pHit->ray.Dir();
						const Scalar dLen = Vector3Ops::Magnitude( d );
						if( dLen > NEARZERO ) {
							wi = d * ( Scalar(-1) / dLen );
						}
						if( pHit->HasTrueGeomSide() ) {
							const bool opposes = pHit->GeomNormalOpposesArrival();
							backfacing = pHit->bGeomNormalOrientedToRay ? opposes : !opposes;
						}
					}
					const float cosi = static_cast<float>( Vector3Ops::Dot( wi, nIn ) );
					const float param = IsFiniteDouble( a[0] )
						? static_cast<float>( a[0] )
						: ( id == kIdFresnel ? 1e-5f : 0.5f );
					const float f = ( id == kIdFresnel )
						? OslShaderNodes::FresnelNode( param, cosi, backfacing )
						: OslShaderNodes::LayerWeightNode( param, cosi, backfacing, ( flags & 1 ) != 0 );
					out[0] = Finite( static_cast<Scalar>( f ) );
					return;
				}
				default:
					// An id with no case (none is emitted: the parser only
					// emits table ids).  Zero all three, so a vec3 call
					// site never reads an unwritten value.
					out[0] = out[1] = out[2] = Scalar(0);
					return;
				}
			}

			bool QueryCameraFrame(
				const ICamera* pCamera,
				Point3& origin,
				Vector3& right,
				Vector3& up,
				Vector3& forward )
			{
				const CameraCommon* cc = dynamic_cast<const CameraCommon*>( pCamera );
				return cc ? cc->GetCameraFrame( origin, right, up, forward ) : false;
			}
		}
	}
}
