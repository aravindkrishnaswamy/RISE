// DL334: adjacent-index/grazing physical-law regression.
// Decimal110 exact-binary64 oracle; fixture table in SSSNearUnityFresnelCases.inl
// (verified and reproducible via tools/generate_sss_near_unity_fresnel_cases.py).
#include <cmath>
#include <cstdio>
#include <limits>
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/PathVertexEval.h"
#include "../src/Library/Shaders/BSSRDFEntryAdapters.h"
using namespace RISE;
using namespace RISE::Implementation;

static unsigned checks = 0, failures = 0;
static double worstR = 0, worstT = 0;

static void Check( bool ok, const char* label, double actual, double expected )
{
	++checks;
	if( !ok ) {
		++failures;
		std::printf( "FAIL %s actual=%.17g expected=%.17g\n", label, actual, expected );
	}
}

struct Case {
	unsigned painter;
	double nm, nt, ni, mu, R, C;
	bool transmits;
	double cosineBound, reflectBound;
};

static const Case cases[] = {
#include "SSSNearUnityFresnelCases.inl"
};

int main()
{
	IScalarPainter *painters[9] = {}, *zero = nullptr, *scatter = nullptr, *dermis = nullptr;
	RISE_API_CreatePiecewiseLinearScalarPainter( &painters[0], {{380,1.63},{457,1.57},{549,1.39},{613,1.29},{780,1.18}} );
	RISE_API_CreateSellmeierScalarPainter( &painters[1], 1.03961212, .231792344, 1.01046945, .00600069867, .0200179144, 103.560653 );
	const double constants[] = { .73, 1.191, 1.4673, 2.113, 7.713, 1e-6, 1e6 };
	for( unsigned p = 2; p < 9; p++ ) RISE_API_CreateUniformScalarPainter( &painters[p], constants[p-2] );
	RISE_API_CreateUniformScalarPainter( &zero, 0 );
	RISE_API_CreateUniformScalarPainter( &scatter, 80 );
	RISE_API_CreateUniformScalarPainter( &dermis, 1.5 );
	IScalarPainter* skinP[7] = {};
	const double skinV[] = { .02, .5, .002, .001, .005, .025, .7 };
	for( unsigned i = 0; i < 7; i++ ) RISE_API_CreateUniformScalarPainter( &skinP[i], skinV[i] );
	RandomNumberGenerator rng( 334 );
	IndependentSampler sampler( rng );
	for( unsigned p = 0; p < 9; p++ ) for( bool slot : { false, true } ) {
		IScalarPainter* view = slot ? painters[p]->MakeSingleScalarSlotView() : nullptr;
		const auto& ior = view ? *view : *painters[p];
		IMaterial *diff = nullptr, *rw = nullptr, *skin = nullptr;
		RISE_API_CreateSubSurfaceScatteringMaterial( &diff, ior, *zero, *scatter, 0, 0 );
		RISE_API_CreateRandomWalkSSSMaterial( &rw, ior, *zero, *scatter, 0, 0, 512 );
		RISE_API_CreateDonnerJensenSkinBSSRDFMaterial( &skin, *skinP[0], *skinP[1], *skinP[2], *skinP[3], *skinP[4], *skinP[5], ior, *dermis, *skinP[6], 0 );
		for( const auto& c : cases ) if( c.painter == p ) for( unsigned axis = 0; axis < 3; axis++ ) {
			double nv[3] = {}, wv[3] = {};
			nv[axis] = 1;
			wv[axis] = c.mu;
			wv[(axis+1)%3] = std::sqrt( ( 1 - c.mu ) * ( 1 + c.mu ) );
			Vector3 n( nv[0], nv[1], nv[2] ), wi( wv[0], wv[1], wv[2] );
			RayIntersectionGeometric ri( Ray( Point3(), -wi ), nullRasterizerState );
			ri.bHit = true;
			ri.vNormal = ri.vGeomNormal = n;
			ri.onb.CreateFromW( n );
			ri.ambientIOR = c.ni;
			Check( ior.GetValueAtNM( ri, c.nm ) == c.nt, "public index exact input", ior.GetValueAtNM( ri, c.nm ), c.nt );
			Check( std::fabs( Vector3Ops::Magnitude( wi ) - 1 ) <= 4 * std::numeric_limits<double>::epsilon(), "unit direction", Vector3Ops::Magnitude( wi ), 1 );
			for( const IMaterial* m : { diff, rw, skin } ) {
				IORStack stack( c.ni );
				ScatteredRayContainer rays;
				m->GetSPF()->ScatterNM( ri, sampler, c.nm, rays, stack );
				double R = 0;
				for( unsigned j = 0; j < rays.Count(); j++ ) R += rays[j].krayNM;
				double T = 0;
				auto* prof = m->GetDiffusionProfile();
				RandomWalkSSSParams rp;
				if( prof ) T = prof->FresnelTransmissionNM( c.mu, ri, c.nm );
				else {
					Check( m->GetRandomWalkSSSParamsNM( c.nm, rp ) && rp.ior == c.nt, "RW index", rp.ior, c.nt );
					T = BSSRDFSampling::BoundaryTransmission( c.mu, c.nt, c.ni );
					if( c.mu > 0 ) {
						BSSRDFAdapters::RandomWalkEntryBSDF a( rp.ior );
						double recovered = a.valueNM( wi, ri, c.nm ) * BSSRDFSampling::BoundaryTransmissionNormalization( c.nt / c.ni ) * PI;
						Check( std::fabs( recovered - ( 1 - c.R ) ) <= c.reflectBound, "RW adapter independent T", recovered, 1 - c.R );
					}
				}
				worstR = std::fmax( worstR, std::fabs( R - c.R ) );
				worstT = std::fmax( worstT, std::fabs( T - ( 1 - c.R ) ) );
				Check( std::fabs( R - c.R ) <= c.reflectBound, "public independent R", R, c.R );
				Check( std::fabs( T - ( 1 - c.R ) ) <= c.reflectBound, "public independent T", T, 1 - c.R );
				Check( R >= 0 && R <= 1 && T >= 0 && T <= 1, "physical range", R, T );
				Check( std::fabs( R + T - 1 ) <= 1e-6, "partition", R + T, 1 );
				if( c.mu > 0 ) {
					BDPTVertex vertex;
					vertex.pMaterial = m;
					vertex.type = BDPTVertex::SURFACE;
					vertex.isBSSRDFEntry = true;
					vertex.position = Point3();
					vertex.normal = vertex.geomNormal = n;
					vertex.onb = ri.onb;
					vertex.mediumIOR = c.ni;
					double shared = PathVertexEval::EvalBSDFAtVertexNM( vertex, wi, wi, c.nm ) * BSSRDFSampling::BoundaryTransmissionNormalization( c.nt / c.ni ) * PI;
					if( c.mu > NEARZERO ) Check( std::fabs( shared - ( 1 - c.R ) ) <= c.reflectBound, "shared independent T", shared, 1 - c.R );
					else Check( shared == 0, "existing shared admission gate", shared, 0 );
					if( prof ) {
						BSSRDFAdapters::BSSRDFEntryBSDF a( prof, 0 );
						double adapter = a.valueNM( wi, ri, c.nm ) * BSSRDFSampling::BoundaryTransmissionNormalization( c.nt / c.ni ) * PI;
						Check( std::fabs( adapter - ( 1 - c.R ) ) <= c.reflectBound, "adapter independent T", adapter, 1 - c.R );
					}
				}
				if( p >= 2 ) {
					ScatteredRayContainer pel;
					m->GetSPF()->Scatter( ri, sampler, pel, stack );
					double r = 0;
					for( unsigned j = 0; j < pel.Count(); j++ ) r += pel[j].kray[0];
					Check( std::fabs( r - c.R ) <= c.reflectBound, "Pel independent R", r, c.R );
					if( prof ) {
						double t = prof->FresnelTransmission( c.mu, ri );
						Check( std::fabs( t - ( 1 - c.R ) ) <= c.reflectBound, "Pel independent T", t, 1 - c.R );
					}
				}
			}
			for( int exponent : { -20, 0, 20 } ) {
				double ni = std::ldexp( c.ni, exponent ), nt = std::ldexp( c.nt, exponent ), ct = 0;
				bool trans = Optics::CalculateRefractedCosine( c.mu, ni, nt, ct );
				Check( trans == c.transmits, "scaled Snell sign", trans, c.transmits );
				if( trans ) Check( std::fabs( ct - c.C ) <= c.cosineBound, "scaled cosine oracle", ct, c.C );
				double R = Optics::CalculateDielectricReflectanceCosine( c.mu, ni, nt );
				Check( std::fabs( R - c.R ) <= c.reflectBound, "scaled Fresnel oracle", R, c.R );
				double normal = -1;
				Check( Optics::CalculateRefractedCosine( 1, ni, nt, normal ) && normal == 1, "normal incidence exact", normal, 1 );
			}
			Vector3 ray = -wi;
			bool trans = Optics::CalculateRefractedRay( n, c.ni, c.nt, ray );
			Check( trans == c.transmits, "vector Snell sign", trans, c.transmits );
			if( trans ) {
				double R = Optics::CalculateDielectricReflectance( -wi, ray, n, c.ni, c.nt );
				Check( std::fabs( R - c.R ) <= c.reflectBound, "vector R oracle", R, c.R );
			}
		}
		diff->release();
		rw->release();
		skin->release();
		if( view ) view->release();
	}
	for( auto* p : painters ) if( p ) p->release();
	zero->release();
	scatter->release();
	dermis->release();
	for( auto* p : skinP ) p->release();
	std::printf( "Cases: %zu Worst R: %.17g Worst T: %.17g\nChecks: %u Failures: %u\n",
		sizeof(cases)/sizeof(cases[0]), worstR, worstT, checks, failures );
	return failures ? 1 : 0;
}
