//////////////////////////////////////////////////////////////////////
//
//  DielectricGrazingFresnelTest.cpp - DL-56 grazing dielectric Fresnel.
//
//  Matched IORs are optically invisible: their unpolarized Fresnel
//  reflectance is zero even at grazing angles.  The independent scalar
//  oracle below guards the shared helper and the real RGB/NM dielectric and
//  standalone non-absorbing subsurface SPF paths which consume it.
//
//  Author: GPT-5.6 Terra
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdlib>
#include <iostream>

#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Materials/DielectricSPF.h"
#include "../src/Library/Materials/SubSurfaceScatteringSPF.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Utilities/ISampler.h"
#include "../src/Library/Utilities/Optics.h"
#include "../src/Library/Utilities/Ray.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int gChecks = 0;
	int gFailures = 0;
	const Scalar kIOR = 1.5;
	const Scalar kTolerance = 1e-10;

	void Check( const bool ok, const char* const message )
	{
		++gChecks;
		if( !ok ) {
			++gFailures;
			std::cerr << "FAIL: " << message << std::endl;
		}
	}

	void CheckNear( const Scalar actual, const Scalar expected,
		const char* const message, const Scalar tolerance = kTolerance )
	{
		++gChecks;
		const Scalar scaled = tolerance * std::fmax( Scalar(1), std::fabs(expected) );
		if( !std::isfinite(actual) || !std::isfinite(expected) || std::fabs(actual-expected) > scaled ) {
			++gFailures;
			std::cerr << "FAIL: " << message << " (got " << actual
				<< ", expected " << expected << ")" << std::endl;
		}
	}

	void CheckVector( const Vector3& actual, const Vector3& expected,
		const char* const message )
	{
		const bool finite = std::isfinite(actual.x) && std::isfinite(actual.y) && std::isfinite(actual.z) &&
			std::isfinite(expected.x) && std::isfinite(expected.y) && std::isfinite(expected.z);
		Check( finite && std::fabs(actual.x-expected.x) <= kTolerance &&
			std::fabs(actual.y-expected.y) <= kTolerance && std::fabs(actual.z-expected.z) <= kTolerance,
			message );
	}

	// Independent scalar Fresnel; it must never call the production helper.
	Scalar Fresnel( const Scalar ni, const Scalar nt, const Scalar cosI )
	{
		const Scalar sinT2 = ni*ni/(nt*nt) * (Scalar(1)-cosI*cosI);
		if( sinT2 >= Scalar(1) ) return Scalar(1);
		const Scalar cosT = std::sqrt( Scalar(1)-sinT2 );
		const Scalar rs = (ni*cosI-nt*cosT) / (ni*cosI+nt*cosT);
		const Scalar rp = (nt*cosI-ni*cosT) / (nt*cosI+ni*cosT);
		return Scalar(0.5) * (rs*rs+rp*rp);
	}

	Vector3 Incident( const Scalar cosI, const bool fromInside )
	{
		return Vector3( std::sqrt(Scalar(1)-cosI*cosI), 0, fromInside ? cosI : -cosI );
	}

	Vector3 Transmitted( const Scalar cosI, const Scalar ni, const Scalar nt, const bool fromInside )
	{
		const Scalar sinT = ni/nt * std::sqrt(Scalar(1)-cosI*cosI);
		return Vector3( sinT, 0, fromInside ? std::sqrt(Scalar(1)-sinT*sinT) : -std::sqrt(Scalar(1)-sinT*sinT) );
	}

	RayIntersectionGeometric MakeRI( const Scalar cosI, const bool fromInside )
	{
		const Ray ray( Point3(0,0,fromInside ? -1 : 1), Incident(cosI,fromInside) );
		RayIntersectionGeometric ri( ray, nullRasterizerState );
		ri.bHit = true;
		ri.range = 1;
		ri.ptIntersection = Point3(0,0,0);
		ri.vNormal = ri.vGeomNormal = Vector3(0,0,1);
		ri.onb.CreateFromW(ri.vNormal);
		ri.ptCoord = Point2(0.5,0.5);
		return ri;
	}

	class OrderedSampler : public ISampler
	{
	public:
		OrderedSampler() : next(0), overdraws(0) {}
		Scalar Get1D() override
		{
			static const Scalar values[2] = { 0.25, 0.75 };
			if( next >= 2 ) { ++overdraws; return 0; }
			return values[next++];
		}
		Point2 Get2D() override
		{
			const Scalar first = Get1D();
			const Scalar second = Get1D();
			return Point2(first,second);
		}
		void StartStream( int ) override {}
		unsigned int Draws() const { return next; }
		unsigned int Overdraws() const { return overdraws; }
	private:
		unsigned int next;
		unsigned int overdraws;
	};

	const ScatteredRay* FindRay( const ScatteredRayContainer& rays, const ScatteredRay::ScatRayType type )
	{
		for( unsigned int i = 0; i < rays.Count(); ++i ) if( rays[i].type == type ) return &rays[i];
		return 0;
	}

	void CheckDelta( const ScatteredRay& ray, const char* const label )
	{
		Check(ray.isDelta,label);
		CheckNear(ray.pdf,1,"smooth dielectric lobe has unit discrete PDF");
	}

	IORStack EnteringStack( const IObject* const object )
	{
		IORStack stack(kIOR);
		stack.SetCurrentObject(object);
		return stack;
	}

	IORStack InsideStack( const IObject* const object )
	{
		IORStack stack = EnteringStack(object);
		stack.push(kIOR);
		return stack;
	}

	void TestDirectHelper()
	{
		std::cout << "Direct helper + Snell controls" << std::endl;
		const Scalar grazing[] = { 0.01, 0.005, 0.001 };
		for( unsigned int i = 0; i < 3; ++i ) {
			const Scalar ni[2] = { kIOR, kIOR };
			const Scalar nt[2] = { kIOR, 1.5001 };
			for( unsigned int j = 0; j < 2; ++j ) {
				const Vector3 incident = Incident(grazing[i],false);
				Vector3 refracted = incident;
				const bool success = Optics::CalculateRefractedRay(Vector3(0,0,1),ni[j],nt[j],refracted);
				Check(success,"grazing Snell control must refract");
				if( success ) {
					CheckVector(refracted,Transmitted(grazing[i],ni[j],nt[j],false),
						"grazing Snell direction matches independent scalar construction");
					CheckNear(Optics::CalculateDielectricReflectance(incident,refracted,Vector3(0,0,1),ni[j],nt[j]),
						Fresnel(ni[j],nt[j],grazing[i]), j == 0 ?
						"matched-index grazing reflectance is zero" :
						"nearby unequal-index grazing reflectance matches scalar oracle");
				}
			}
		}

		// The matched-index identity also defines the exact tangent limit.
		const Vector3 tangent(1,0,0);
		CheckNear(Optics::CalculateDielectricReflectance(tangent,tangent,Vector3(0,0,1),kIOR,kIOR),
			0,"matched-index exact tangent reflectance is zero");

		// Common IOR scaling cancels from Fresnel. These finite positive
		// normal-incidence cases expose the same absolute quotient cutoff
		// and the overflow risk of squaring the combined numerator.
		const Scalar scales[] = { 1e-4, 1, 1e100 };
		for( const Scalar scale : scales ) {
			CheckNear(Optics::CalculateDielectricReflectance(Incident(1,false),Incident(1,false),
				Vector3(0,0,1),scale,1.5*scale),0.04,
				"normal-incidence Fresnel is invariant under common IOR scaling");
		}

		Vector3 normal = Incident(1,false);
		const bool normalRefracts = Optics::CalculateRefractedRay(Vector3(0,0,1),1,kIOR,normal);
		Check(normalRefracts,"normal-incidence Snell control must refract");
		if( normalRefracts ) {
			CheckNear(Optics::CalculateDielectricReflectance(Incident(1,false),normal,Vector3(0,0,1),1,kIOR),
				Fresnel(1,kIOR,1),"normal-incidence reflectance matches scalar oracle");
		}

		Vector3 tir = Incident(0.5,false);
		Check(!Optics::CalculateRefractedRay(Vector3(0,0,1),kIOR,1,tir),
			"above-critical-angle Snell control must reject transmission");
		CheckNear(Fresnel(kIOR,1,0.5),1,"scalar Fresnel is one at total internal reflection");
	}

	void CheckEnterState( const IORStack& input, const ScatteredRay& transmission,
		const IObject* const object )
	{
		Check(!input.containsCurrent(),"dielectric input remains outside current object");
		CheckNear(input.top(),kIOR,"dielectric input IOR remains matched");
		Check(transmission.ior_stack != 0,"dielectric transmission carries an IOR stack");
		if( transmission.ior_stack ) {
			Check(transmission.delete_stack,"stored dielectric transmission owns its stack");
			Check(transmission.ior_stack->containsCurrent(),"dielectric transmission enters current object");
			Check(transmission.ior_stack->topObject() == object,"dielectric transmitted stack names current object");
			CheckNear(transmission.ior_stack->top(),kIOR,"dielectric transmitted stack keeps matched IOR");
		}
	}

	void TestDielectricSPF( const IObject* const object )
	{
		std::cout << "DielectricSPF RGB/NM matched-index paths" << std::endl;
		UniformScalarPainter* const tau = new UniformScalarPainter(1);
		UniformScalarPainter* const ior = new UniformScalarPainter(kIOR);
		UniformScalarPainter* const scatter = new UniformScalarPainter(1000000);
		DielectricSPF* const spf = new DielectricSPF(*tau,*ior,*scatter,false);
		const Scalar cosines[] = { 1, 0.01, 0.005, 0.001 };

		for( unsigned int i = 0; i < 4; ++i ) {
			const Scalar c = cosines[i];
			const Scalar expected = Fresnel(kIOR,kIOR,c);
			const RayIntersectionGeometric ri = MakeRI(c,false);
			const Vector3 reflected(std::sqrt(Scalar(1)-c*c),0,c);
			const Vector3 transmitted = Transmitted(c,kIOR,kIOR,false);
			IORStack input = EnteringStack(object);
			OrderedSampler sampler;
			ScatteredRayContainer rays;
			spf->Scatter(ri,sampler,rays,input);
			const ScatteredRay* const r = FindRay(rays,ScatteredRay::eRayReflection);
			const ScatteredRay* const t = FindRay(rays,ScatteredRay::eRayRefraction);
			Check(rays.Count() == 2 && r && t,"RGB matched-index path keeps both Fresnel lobes active");
			Check(sampler.Draws() == 2 && sampler.Overdraws() == 0,"RGB uses exactly two ordered sampler draws");
			if( r && t ) {
				CheckDelta(*r,"RGB reflection is delta"); CheckDelta(*t,"RGB transmission is delta");
				for( unsigned int channel = 0; channel < 3; ++channel ) {
					CheckNear(r->kray[channel],expected,"RGB reflection weight matches scalar Fresnel");
					CheckNear(t->kray[channel],1-expected,"RGB transmission weight is Fresnel complement");
				}
				CheckNear(r->kray[0]+t->kray[0],1,"RGB Fresnel lobes conserve surface throughput");
				CheckVector(r->ray.Dir(),reflected,"RGB reflection follows analytic mirror direction");
				CheckVector(t->ray.Dir(),transmitted,"RGB transmission follows analytic Snell direction");
				Check(r->ior_stack == 0,"RGB reflection leaves entering medium state implicit");
				CheckEnterState(input,*t,object);
			}

			IORStack nmInput = EnteringStack(object);
			OrderedSampler nmSampler;
			ScatteredRayContainer nmRays;
			spf->ScatterNM(ri,nmSampler,550,nmRays,nmInput);
			const ScatteredRay* const nmR = FindRay(nmRays,ScatteredRay::eRayReflection);
			const ScatteredRay* const nmT = FindRay(nmRays,ScatteredRay::eRayRefraction);
			Check(nmRays.Count() == 1 && !nmR && nmT,"NM matched-index path retains unit transmission lobe");
			Check(nmSampler.Draws() == 2 && nmSampler.Overdraws() == 0,"NM uses exactly two ordered sampler draws");
			if( nmT ) {
				CheckDelta(*nmT,"NM transmission is delta");
				CheckNear(nmT->krayNM,1-expected,"NM transmission weight is Fresnel complement");
				CheckVector(nmT->ray.Dir(),transmitted,"NM transmission follows analytic Snell direction");
				CheckEnterState(nmInput,*nmT,object);
			}
		}
		spf->release(); tau->release(); ior->release(); scatter->release();
	}

	void CheckExitState( const IORStack& input, const ScatteredRay& reflection,
		const ScatteredRay& transmission )
	{
		Check(input.containsCurrent(),"subsurface input remains inside current object");
		CheckNear(input.top(),kIOR,"subsurface input IOR remains matched");
		Check(reflection.ior_stack != 0 && transmission.ior_stack != 0,
			"subsurface reflection and transmission both carry stacks");
		if( reflection.ior_stack ) {
			Check(reflection.delete_stack,"stored subsurface reflection owns its stack");
			Check(reflection.ior_stack->containsCurrent(),"subsurface reflection retains current object");
			CheckNear(reflection.ior_stack->top(),kIOR,"subsurface reflection keeps matched IOR");
		}
		if( transmission.ior_stack ) {
			Check(transmission.delete_stack,"stored subsurface transmission owns its stack");
			Check(!transmission.ior_stack->containsCurrent(),"subsurface transmission pops current object");
			Check(transmission.ior_stack->topObject() == 0,"subsurface transmission reveals root medium");
			CheckNear(transmission.ior_stack->top(),kIOR,"subsurface transmission keeps matched root IOR");
		}
	}

	void TestSubSurfaceSPF( const IObject* const object )
	{
		std::cout << "Standalone SubSurfaceScatteringSPF RGB/NM exits" << std::endl;
		UniformScalarPainter* const ior = new UniformScalarPainter(kIOR);
		SubSurfaceScatteringSPF* const spf = new SubSurfaceScatteringSPF(*ior,0,0,false);
		const Scalar cosines[] = { 1, 0.01, 0.005, 0.001 };

		for( unsigned int i = 0; i < 4; ++i ) {
			const Scalar c = cosines[i];
			const Scalar expected = Fresnel(kIOR,kIOR,c);
			const RayIntersectionGeometric ri = MakeRI(c,true);
			const Vector3 reflected(std::sqrt(Scalar(1)-c*c),0,-c);
			const Vector3 transmitted = Transmitted(c,kIOR,kIOR,true);
			for( unsigned int nm = 0; nm < 2; ++nm ) {
				IORStack input = InsideStack(object);
				OrderedSampler sampler;
				ScatteredRayContainer rays;
				if( nm ) spf->ScatterNM(ri,sampler,550,rays,input); else spf->Scatter(ri,sampler,rays,input);
				const ScatteredRay* const r = FindRay(rays,ScatteredRay::eRayReflection);
				const ScatteredRay* const t = FindRay(rays,ScatteredRay::eRayRefraction);
				Check(rays.Count() == 2 && r && t,"standalone non-absorbing subsurface exit retains reflection and transmission");
				Check(sampler.Draws() == 0 && sampler.Overdraws() == 0,"smooth standalone subsurface exit consumes no sampler draws");
				if( r && t ) {
					CheckDelta(*r,"subsurface reflection is delta"); CheckDelta(*t,"subsurface transmission is delta");
					if( nm ) {
						CheckNear(r->krayNM,expected,"NM subsurface reflection weight matches scalar Fresnel");
						CheckNear(t->krayNM,1-expected,"NM subsurface transmission is Fresnel complement");
						CheckNear(r->krayNM+t->krayNM,1,"NM subsurface Fresnel lobes conserve throughput");
					} else {
						CheckNear(r->kray[0],expected,"RGB subsurface reflection weight matches scalar Fresnel");
						CheckNear(t->kray[0],1-expected,"RGB subsurface transmission is Fresnel complement");
						CheckNear(r->kray[0]+t->kray[0],1,"RGB subsurface Fresnel lobes conserve throughput");
					}
					CheckVector(r->ray.Dir(),reflected,"subsurface reflection follows analytic mirror direction");
					CheckVector(t->ray.Dir(),transmitted,"subsurface transmission follows analytic Snell direction");
					CheckExitState(input,*r,*t);
				}
			}
		}
		spf->release(); ior->release();
	}
}

int main()
{
	std::cout << "=== DielectricGrazingFresnelTest (DL-56) ===" << std::endl;
	// New Reference objects start at one reference.  IORStack only reads this
	// object as an identity key, so no extra addref is needed before release.
	StubObject* const object = new StubObject;
	TestDirectHelper();
	TestDielectricSPF(object);
	TestSubSurfaceSPF(object);
	object->release();
	if( gFailures ) {
		std::cerr << "Checks: " << gChecks << "  Failures: " << gFailures << std::endl;
		return EXIT_FAILURE;
	}
	std::cout << "Checks: " << gChecks << "  Failures: 0" << std::endl;
	return EXIT_SUCCESS;
}
