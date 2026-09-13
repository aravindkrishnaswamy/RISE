//////////////////////////////////////////////////////////////////////
//
//  GrazingFresnelThroughputTest.cpp - DL-58 consumer regressions.
//
//  The direct helper test owns Snell/Fresnel arithmetic.  This file owns
//  two production consumers: SMS chain throughput (RGB and NM) and the
//  weave BRDF's OIDN albedo surface-reflection estimate.  The SMS chains
//  are deliberately hand-built: this isolates the already-converged
//  consumer without a scene, ray caster, or Newton solve.
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Materials/WeaveBRDF.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/ManifoldSolver.h"
#include "../src/Library/Utilities/Ray.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int gChecks = 0;
	int gFailures = 0;

	class TestableSolver : public ManifoldSolver
	{
	public:
		explicit TestableSolver( const ManifoldSolverConfig& config ) : ManifoldSolver(config) {}
		using ManifoldSolver::ComputeSpecularDirection;
		using ManifoldSolver::ComputeSpecularDirectionDerivativeWrtNormal;
	};

	void Check( const bool condition, const char* const label )
	{
		++gChecks;
		if( !condition ) {
			++gFailures;
			std::cerr << "FAIL: " << label << std::endl;
		}
	}

	void CheckNear( const Scalar actual, const long double expected,
		const char* const label, const Scalar tolerance = 1e-10 )
	{
		++gChecks;
		if( !std::isfinite(actual) || !std::isfinite(expected) ||
			std::fabs((long double)actual-expected) > tolerance ) {
			++gFailures;
			std::cerr << "FAIL: " << label << " (got " << actual
				<< ", expected " << (double)expected << ")" << std::endl;
		}
	}

	long double PhysicalNormalFresnel( const long double ni, const long double nt )
	{
		const long double r = (ni - nt) / (ni + nt);
		return r * r;
	}

	ManifoldVertex MakeTransmissionVertex( const Scalar etaI, const Scalar etaT )
	{
		ManifoldVertex v;
		v.position = Point3(0,0,0);
		v.normal = v.geomNormal = Vector3(0,0,1);
		v.eta = etaT;
		v.etaI = etaI;
		v.etaT = etaT;
		v.attenuation = RISEPel(1,1,1);
		v.isReflection = false;
		v.canRefract = true;
		v.valid = true;
		return v;
	}

	Point3 StartAtCosine( const Scalar cosine )
	{
		const Scalar tangent = std::sqrt( Scalar(1) - cosine*cosine );
		// EvaluateChainThroughput forms normalize(vertex - start), so this
		// places its actual Fresnel cosine at the requested positive value.
		return Point3( -tangent, 0, -cosine );
	}

	void TestSMSMatchedGrazingTransmission()
	{
		std::cout << "SMS matched-index grazing transmission" << std::endl;
		const Scalar cosine = Scalar(1e-9);
		Check( Scalar(1) - cosine*cosine == Scalar(1),
			"SMS input cosine reaches the rounded grazing cancellation band" );

		std::vector<ManifoldVertex> chain( 1, MakeTransmissionVertex(Scalar(1.5),Scalar(1.5)) );
		const ManifoldSolverConfig config;
		TestableSolver solver( config );
		const Point3 start = StartAtCosine( cosine );
		const Point3 end( std::sqrt(Scalar(1)-cosine*cosine), 0, cosine );
		const RISEPel rgb = solver.EvaluateChainThroughput( start, end, chain );
		const Scalar nm = solver.EvaluateChainThroughputNM( start, end, chain, Scalar(550) );

		for( unsigned int channel = 0; channel < 3; ++channel ) {
			CheckNear( rgb[channel], 1.0L, "matched SMS RGB transmission remains active" );
		}
		CheckNear( nm, 1.0L, "matched SMS NM transmission remains active" );
	}

	void TestSMSReflectionAndNormalControls()
	{
		std::cout << "SMS reflection and normal-incidence controls" << std::endl;
		const ManifoldSolverConfig config;
		TestableSolver solver( config );
		const Point3 normalStart(0,0,-1);
		const Point3 end(0,0,1);

		ManifoldVertex matchedReflection = MakeTransmissionVertex(Scalar(1.5),Scalar(1.5));
		matchedReflection.isReflection = true;
		std::vector<ManifoldVertex> chain( 1, matchedReflection );
		CheckNear( solver.EvaluateChainThroughput(normalStart,end,chain)[0], 0.0L,
			"matched dielectric reflection is zero" );
		CheckNear( solver.EvaluateChainThroughputNM(normalStart,end,chain,Scalar(550)), 0.0L,
			"matched dielectric NM reflection is zero" );

		chain[0].canRefract = false;
		CheckNear( solver.EvaluateChainThroughput(normalStart,end,chain)[0], 1.0L,
			"pure mirror RGB control remains fully reflective" );
		CheckNear( solver.EvaluateChainThroughputNM(normalStart,end,chain,Scalar(550)), 1.0L,
			"pure mirror NM control remains fully reflective" );

		chain[0] = MakeTransmissionVertex(Scalar(1),Scalar(1.5));
		const long double F = PhysicalNormalFresnel(1.0L,1.5L);
		const RISEPel rgb = solver.EvaluateChainThroughput(normalStart,end,chain);
		CheckNear( rgb[0], (1.0L-F)/(1.5L*1.5L),
			"unequal-index SMS RGB includes independent Fresnel and radiance scale" );
		CheckNear( solver.EvaluateChainThroughputNM(normalStart,end,chain,Scalar(550)), 1.0L-F,
			"unequal-index SMS NM includes independent Fresnel transmission" );
	}

	void TestMatchedGrazingSpecularDirection()
	{
		std::cout << "Matched-index grazing specular direction" << std::endl;
		const Scalar cosine = Scalar(1e-9);
		const Vector3 wi( std::sqrt(Scalar(1)-cosine*cosine), 0, cosine );
		const Vector3 normal( 0, 0, 1 );
		const ManifoldSolverConfig config;
		TestableSolver solver( config );
		Vector3 wo;
		Check( solver.ComputeSpecularDirection(wi,normal,Scalar(1),false,wo),
			"matched grazing specular transmission succeeds" );
		CheckNear( wo.x, -(long double)wi.x, "matched grazing specular direction preserves x" );
		CheckNear( wo.y, -(long double)wi.y, "matched grazing specular direction preserves y" );
		CheckNear( wo.z, -(long double)wi.z, "matched grazing specular direction preserves normal component", 1e-12 );

		// This raw derivative has a pre-existing critical-angle safeguard for
		// unequal IORs.  The matched-IOR identity is distinct: transmission
		// cannot depend on the normal at all, so every raw derivative is zero.
		Scalar derivative[9];
		solver.ComputeSpecularDirectionDerivativeWrtNormal(
			wi, normal, Scalar(1), false, derivative );
		for( unsigned int i = 0; i < 9; ++i ) {
			CheckNear( derivative[i], 0.0L, "matched specular-direction normal derivative is zero", 1e-12 );
		}
	}

	template<class T>
	class Ref
	{
	public:
		explicit Ref( T* const value ) : p(value) {}
		~Ref() { p->release(); }
		T& operator*() const { return *p; }
		T* get() const { return p; }
	private:
		T* p;
		Ref( const Ref& );
		Ref& operator=( const Ref& );
	};

	RayIntersectionGeometric MakeWeaveHit( const Scalar cosine )
	{
		const Vector3 view( std::sqrt(Scalar(1)-cosine*cosine), 0, cosine );
		RayIntersectionGeometric ri( Ray(Point3(view.x,view.y,view.z),-view), nullRasterizerState );
		ri.bHit = true;
		ri.range = 1;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = ri.vGeomNormal = Vector3(0,0,1);
		ri.onb.CreateFromWU( ri.vNormal, Vector3(1,0,0) );
		ri.ptCoord = ri.ptCoord1 = Point2(0.5,0.5);
		return ri;
	}

	RISEPel WeaveSurfaceAlbedo( const Scalar eta, const Scalar cosine )
	{
		Ref<UniformScalarPainter> one( new UniformScalarPainter(1) );
		Ref<UniformScalarPainter> zero( new UniformScalarPainter(0) );
		Ref<UniformScalarPainter> ior( new UniformScalarPainter(eta) );
		Ref<UniformScalarPainter> width( new UniformScalarPainter(0.2) );
		Ref<UniformScalarPainter> azimuth( new UniformScalarPainter(0.5) );
		Ref<UniformColorPainter> black( new UniformColorPainter(RISEPel(0,0,0),eSpectrumKind_Albedo) );
		WeaveBRDF* const brdf = new WeaveBRDF( eWeavePlain, eWeaveTransmissionNone,
			*one, *zero, *zero, 0, *zero, *black, *ior, *width, *azimuth,
			*zero, *zero, *zero, *black, *ior, *width, *azimuth, *zero, *zero, *zero );
		const RISEPel result = brdf->albedo( MakeWeaveHit(cosine) );
		brdf->release();
		return result;
	}

	void TestWeaveSurfaceConsumer()
	{
		std::cout << "Weave surface Fresnel consumer" << std::endl;
		const Scalar grazing = Scalar(1e-9);
		Check( Scalar(1) - grazing*grazing == Scalar(1),
			"weave albedo passes a rounded-band grazing cosine to FrDielectric" );
		const RISEPel matched = WeaveSurfaceAlbedo( Scalar(1), grazing );
		for( unsigned int channel = 0; channel < 3; ++channel ) {
			CheckNear( matched[channel], 0.0L, "matched weave surface reflection is zero" );
		}

		const RISEPel unequal = WeaveSurfaceAlbedo( Scalar(1.5), Scalar(1) );
		const long double expected = PhysicalNormalFresnel(1.0L,1.5L);
		for( unsigned int channel = 0; channel < 3; ++channel ) {
			CheckNear( unequal[channel], expected, "unequal weave surface reflection is positive Fresnel" );
		}
	}
}

int main()
{
	TestSMSMatchedGrazingTransmission();
	TestSMSReflectionAndNormalControls();
	TestMatchedGrazingSpecularDirection();
	TestWeaveSurfaceConsumer();
	Check( gChecks >= 31, "all consumer controls executed" );
	std::cout << "Checks: " << gChecks << "  Failures: " << gFailures << std::endl;
	return gFailures ? 1 : 0;
}
