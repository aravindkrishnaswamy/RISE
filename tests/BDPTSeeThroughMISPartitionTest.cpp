//////////////////////////////////////////////////////////////////////
//
//  BDPTSeeThroughMISPartitionTest.cpp - DL-424/425 review follow-up:
//  a DETERMINISTIC partition-of-unity check for BDPT's see-through s = 1
//  strategy (docs/DL05_WEAVE_GAP_SHADOW_TRANSMITTANCE.md section 11).
//
//  Tabs: 4
//  Comments:
//
//    A synthetic path  L - S1..Sk - D - [D2] - E  (L a point light, Si
//    thin-weave delta pass-throughs on the straight segment L -> D, D and
//    D2 diffuse, E a pinhole camera) is built vertex by vertex, and every
//    strategy BDPT evaluates for it is split out of the SAME full-path
//    densities exactly as ConnectAndEvaluate leaves them (light-side
//    density l[i], eye-side density e[i]; the delta-transparency zeros:
//    the vertex after a delta scatter has forward density 0).  The
//    strategies that exist are the light-tracing ones whose light subpath
//    passes the gaps and the see-through connection (s = 1, gaps = k,
//    whose eye endpoint carries pdfRev = the light's emission density
//    toward D times the gaps' pseudo-probabilities, converted to area
//    over |L - D|).  Everything else crosses a delta edge and evaluates
//    zero.  MISWeight over the existing strategies must sum to 1.
//
//    Variants: one / two gaps; with / without D2; generous depth caps and
//    caps that cut one family (max_light_depth too small for a light
//    walk to reach D2, max_eye_depth too small for the see-through's eye
//    walk to reach D) -- the strategies that cannot exist under a cap are
//    then left out of the sum, which must not exceed 1 (BDPT's standard
//    ratio walk ignores caps, so truncation alone can leave it below 1).
//
//    A second check pins the pairing guard: a light subpath whose gap
//    did NOT record its pseudo-probability (passThroughProb 0, the
//    state a future SPF emitting its gap ray with a different type would
//    produce if the recording keyed on the type) breaks the partition --
//    the sum moves off 1.  That configuration also trips the MISWeight
//    assert in an assert-enabled build, so it is evaluated only when
//    NDEBUG is defined.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Utilities/BDPTUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& name )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << name << std::endl;
	}
}

namespace
{
	//! Minimal material: only identity and HasDeltaPassThrough matter to
	//! MISWeight.
	class StubMaterial : public IMaterial, public Reference
	{
		bool passThrough;
	protected:
		virtual ~StubMaterial() {}
	public:
		explicit StubMaterial( bool p ) : passThrough( p ) {}
		IBSDF* GetBSDF() const override { return nullptr; }
		ISPF* GetSPF() const override { return nullptr; }
		IEmitter* GetEmitter() const override { return nullptr; }
		bool HasDeltaPassThrough() const override { return passThrough; }
	};

	enum Kind { kLight, kGap, kDiffuse, kCamera };

	struct PathSpec
	{
		std::vector<Kind>		kinds;
		std::vector<Point3>		pos;
		std::vector<Scalar>		l;			// light-side area density
		std::vector<Scalar>		e;			// eye-side area density
		std::vector<Scalar>		gapProb;	// pseudo-probability per gap
	};

	struct Ctx
	{
		const PointLight*	light;
		const StubMaterial*	gap;
		const StubMaterial*	diffuse;
	};

	//! Gap-chain see-through density of D: emission density toward D times
	//! the gap product, to area at D over |L - D| (as the integrator does).
	Scalar SeeThroughArea( const PathSpec& p, unsigned int dIdx, const Ctx& c )
	{
		const Vector3 dL = Vector3Ops::mkVector3( p.pos[dIdx], p.pos[0] );
		const Scalar d2 = Vector3Ops::SquaredModulus( dL );
		const Vector3 dir = dL * ( Scalar( 1 ) / std::sqrt( d2 ) );
		Scalar prob = 1;
		for( unsigned int q = 1; q < dIdx; q++ ) prob *= p.gapProb[q];
		const Vector3 n( 0, 0, 1 );
		return BDPTUtilities::SolidAngleToArea( c.light->pdfDirection( dir ) * prob,
			std::fabs( Vector3Ops::Dot( n, dir ) ), d2 );
	}

	BDPTVertex MakeVertex( const PathSpec& p, unsigned int i, const Ctx& c, bool lightSide )
	{
		BDPTVertex v;
		v.position = p.pos[i];
		v.geomNormal = Vector3( 0, 0, 1 );
		v.normal = v.geomNormal;
		switch( p.kinds[i] ) {
		case kLight:
			v.type = BDPTVertex::LIGHT;
			v.pLight = c.light;
			v.isDelta = true;
			v.isConnectible = false;
			break;
		case kGap:
			v.type = BDPTVertex::SURFACE;
			v.pMaterial = c.gap;
			v.isDelta = true;
			v.isConnectible = true;
			v.passThroughProb = lightSide ? p.gapProb[i] : 0;
			break;
		case kDiffuse:
			v.type = BDPTVertex::SURFACE;
			v.pMaterial = c.diffuse;
			break;
		case kCamera:
			v.type = BDPTVertex::CAMERA;
			break;
		}
		v.isLightSubpathVertex = lightSide;
		v.pdfFwd = lightSide ? p.l[i] : p.e[i];
		v.pdfRev = lightSide ? p.e[i] : p.l[i];
		return v;
	}

	//! Sum of MISWeight over every strategy BDPT evaluates (nonzero
	//! contribution) for the path, under the integrator's caps.
	Scalar PartitionSum( const PathSpec& p, const Ctx& c, unsigned int maxEye,
		unsigned int maxLight, bool recordGapProbs, std::string& detail,
		bool lightSideSeesThrough = true )
	{
		StabilityConfig stab;
		BDPTIntegrator* integ = new BDPTIntegrator( maxEye, maxLight, stab );

		const unsigned int n = static_cast<unsigned int>( p.kinds.size() );
		unsigned int dIdx = 1;
		while( p.kinds[dIdx] == kGap ) dIdx++;
		const unsigned int k = dIdx - 1;

		// Light walk surfaces/iterations to reach vertex i (gaps count).
		auto lightReaches = [&]( unsigned int lastIdx ) {
			return lastIdx <= maxLight;		// vertices 1..lastIdx are all surfaces
		};
		// Eye walk: camera + surfaces back to index j (n-1 .. j).
		auto eyeReaches = [&]( unsigned int firstIdx ) {
			return ( n - 1 - firstIdx ) <= maxEye;
		};

		Scalar sum = 0;
		char buf[128];
		detail.clear();

		// Light tracing / interior connections: s >= dIdx + 1 (light
		// subpath passes all gaps and reaches D), t >= 1, s + t = n.
		for( unsigned int s = dIdx + 1; s + 1 <= n; s++ ) {
			const unsigned int t = n - s;
			if( !lightReaches( s - 1 ) || !eyeReaches( n - t ) ) continue;
			std::vector<BDPTVertex> lv, ev;
			for( unsigned int i = 0; i < s; i++ ) {
				BDPTVertex v = MakeVertex( p, i, c, true );
				if( !recordGapProbs ) v.passThroughProb = 0;
				lv.push_back( v );
			}
			for( unsigned int j = 0; j < t; j++ ) ev.push_back( MakeVertex( p, n - 1 - j, c, false ) );
			BDPTIntegrator::SeeThroughMIS st;
			st.live = lightSideSeesThrough;
			st.gaps = 0;
			const Scalar w = integ->MISWeight( lv, ev, s, t, &st );
			std::snprintf( buf, sizeof(buf), " (%u,%u)=%.6f", s, t, w );
			detail += buf;
			sum += w;
		}

		// See-through: s = 1, t = n - 1 - k (eye walk reaches D).
		{
			const unsigned int t = n - 1 - k;
			if( eyeReaches( dIdx ) ) {
				std::vector<BDPTVertex> lv, ev;
				lv.push_back( MakeVertex( p, 0, c, true ) );
				lv[0].pdfRev = 0;			// a point light is never hit
				for( unsigned int j = 0; j < t; j++ ) ev.push_back( MakeVertex( p, n - 1 - j, c, false ) );
				ev[t - 1].pdfRev = SeeThroughArea( p, dIdx, c );
				BDPTIntegrator::SeeThroughMIS st;
				st.live = true;
				st.gaps = k;
				const Scalar w = integ->MISWeight( lv, ev, 1, t, &st );
				std::snprintf( buf, sizeof(buf), " ST(1,%u)=%.6f", t, w );
				detail += buf;
				sum += w;
			}
		}

		safe_release( integ );
		return sum;
	}

	PathSpec MakePath( unsigned int gaps, bool withD2 )
	{
		PathSpec p;
		// L at z = 3, gaps on the straight segment down to D at the origin.
		p.kinds.push_back( kLight );	p.pos.push_back( Point3( 0, 0, 3 ) );
		for( unsigned int g = 0; g < gaps; g++ ) {
			p.kinds.push_back( kGap );
			p.pos.push_back( Point3( 0, 0, 2.0 - 0.5 * g ) );
		}
		p.kinds.push_back( kDiffuse );	p.pos.push_back( Point3( 0, 0, 0 ) );
		if( withD2 ) {
			p.kinds.push_back( kDiffuse );	p.pos.push_back( Point3( 1.5, 0.5, 0.8 ) );
		}
		p.kinds.push_back( kCamera );	p.pos.push_back( Point3( 0.3, -0.4, 4 ) );

		const unsigned int n = static_cast<unsigned int>( p.kinds.size() );
		p.l.assign( n, 0 );
		p.e.assign( n, 0 );
		p.gapProb.assign( n, 0 );
		// Arbitrary positive densities; the delta-transparency zeros are
		// what BDPT stores: l = 0 right after a delta scatter on the light
		// side (D after the last gap, a gap after a gap), e = 0 right after
		// a delta scatter on the eye side (the vertex before each gap).
		const Scalar lv[] = { 1.0, 0.31, 0.27, 0.9, 0.44, 0.6, 0.5 };
		const Scalar evv[] = { 0.0, 0.52, 0.47, 1.7, 0.83, 1.0, 1.0 };
		for( unsigned int i = 0; i < n; i++ ) {
			p.l[i] = lv[i % 7];
			p.e[i] = evv[i % 7];
			if( p.kinds[i] == kGap ) p.gapProb[i] = 0.3 + 0.1 * i;
		}
		for( unsigned int i = 1; i < n; i++ ) {
			if( p.kinds[i - 1] == kGap ) p.l[i] = 0;
		}
		for( unsigned int i = 0; i + 1 < n; i++ ) {
			if( p.kinds[i + 1] == kGap ) p.e[i] = 0;
		}
		// Balance the two families at D: the see-through's density of D
		// is ~3e-3 here (1/(4 pi) * gaps / |L - D|^2), so give the eye
		// walk a comparable density there and both weights are O(1).
		{
			unsigned int d = 1;
			while( p.kinds[d] == kGap ) d++;
			p.e[d] = 0.004 / gaps;
		}
		p.e[0] = 0;					// L never hit
		p.e[n - 1] = 1;				// camera pixel density
		return p;
	}
}

int main()
{
	std::cout << "BDPTSeeThroughMISPartitionTest (DL-424/425 follow-up)" << std::endl;

	PointLight* light = new PointLight( 1.0, RISEPel( 1, 1, 1 ), false );
	StubMaterial* gap = new StubMaterial( true );
	StubMaterial* diffuse = new StubMaterial( false );
	const Ctx c = { light, gap, diffuse };

	struct Variant { unsigned int gaps; bool d2; unsigned int maxEye, maxLight; const char* label; };
	const Variant variants[] = {
		{ 1, false, 8, 8, "1 gap, L-S-D-E" },
		{ 2, false, 8, 8, "2 gaps, L-S-S-D-E" },
		{ 1, true,  8, 8, "1 gap, L-S-D-D2-E" },
		{ 2, true,  8, 8, "2 gaps, L-S-S-D-D2-E" },
		{ 1, true,  8, 2, "1 gap + D2, max_light_depth 2 (no light walk to D2)" },
		{ 1, true,  1, 8, "1 gap + D2, max_eye_depth 1 (no see-through to D)" },
		{ 2, true,  8, 3, "2 gaps + D2, max_light_depth 3" },
	};
	for( const Variant& v : variants ) {
		const PathSpec p = MakePath( v.gaps, v.d2 );
		std::string detail;
		const Scalar sum = PartitionSum( p, c, v.maxEye, v.maxLight, true, detail );
		std::cout << "  " << v.label << ": sum " << sum << "  [" << detail << " ]" << std::endl;
		if( v.maxEye >= 8 && v.maxLight >= 8 ) {
			Check( std::fabs( sum - 1.0 ) < 1e-9, std::string( "partition of unity: " ) + v.label );
		} else {
			// Under a cap BDPT's STANDARD ratio walk still reserves mass
			// for strategies past the cap (ordinary truncation, an
			// under-count); the see-through rule may only ever leave
			// strategies OUT, so the sum must not exceed 1.
			Check( sum <= 1.0 + 1e-9 && sum > 0.5, std::string( "no over-count under caps: " ) + v.label );
		}
	}

	// Contrast (assert-free): light-tracing strategies that do not count
	// the see-through connection while it still counts them -- the
	// one-sided bookkeeping the pairing exists to prevent -- over-count.
	{
		const PathSpec p = MakePath( 1, true );
		std::string detail;
		const Scalar sum = PartitionSum( p, c, 8, 8, true, detail, false );
		std::cout << "  contrast, light side blind to the see-through: sum " << sum << "  [" << detail << " ]" << std::endl;
		Check( sum > 1.0 + 1e-3, "one-sided see-through bookkeeping double counts (sum > 1)" );
	}

#ifdef NDEBUG
	// Pairing guard (see the header): an unrecorded gap pseudo-probability
	// breaks the partition, which is what the recording / assert guard.
	{
		const PathSpec p = MakePath( 1, true );
		std::string detail;
		const Scalar sum = PartitionSum( p, c, 8, 8, false, detail );
		std::cout << "  unrecorded gap probability: sum " << sum << "  [" << detail << " ]" << std::endl;
		Check( sum > 1.0 + 1e-6, "unrecorded gap probability double counts (sum > 1)" );
	}
#endif

	safe_release( light );
	safe_release( gap );
	safe_release( diffuse );

	std::cout << "\nBDPTSeeThroughMISPartitionTest: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount == 0 ? 0 : 1;
}
