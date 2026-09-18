//////////////////////////////////////////////////////////////////////
//
//  GenericHumanTissueInteriorScatterTest.cpp - DL-184 red-proof.
//
//    GenericHumanTissueSPF::Scatter/ScatterNM's interior branch
//    (ior_stack.containsCurrent() == true) computes a scattered
//    direction INSIDE `if( x < (pa + ps) ) { ... }` and then
//    unconditionally overwrites it with `trans.ray.SetDir(
//    ri.ray.Dir())` on the very next statement, OUTSIDE that `if`.
//    Every interior interaction is therefore straight-through
//    regardless of the scattering roll: `scattering` (`sca`) and the
//    phase-function asymmetry `g` have no effect at all in-medium, in
//    both RGB and NM.
//
//    This is DISTINCT from DL-183 (the same material's scattered-ray
//    ORIGIN is `ri.ray.origin` instead of `ri.ptIntersection`), whose
//    own red-proof (`CheckOrigins` below) was added to this file when
//    DL-183 was closed -- see that function's own header comment.
//
//    RECIPE (per docs/DEBT_LEDGER.md's DL-131 row, whose defect this
//    test also exercises -- DL-131 and this dead-lobe bug share one
//    root cause and one fix): drive Scatter/ScatterNM with an
//    inside-stack fixture and a large `sca` so the scattering branch
//    is reached essentially every non-absorbed trial, and count
//    emitted directions that differ from `ri.ray.Dir()`.  Zero
//    pre-fix at any `scattering`; a large majority post-fix (the
//    diffuse cosine-weighted perturbation only reproduces the exact
//    incoming direction at a measure-zero sample).
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Utilities/IORStack.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/GenericHumanTissueSPF.h"
#include "../src/Library/Utilities/Reference.h"

#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	static int passCount = 0;
	static int failCount = 0;

	void Check( bool cond, const char* desc )
	{
		if( cond ) {
			passCount++;
		} else {
			failCount++;
			std::cout << "  FAIL: " << desc << std::endl;
		}
	}

	RayIntersectionGeometric MakeInteriorHit()
	{
		// Ray travelling +Z, hit at the origin, unit distance behind it --
		// `distance = |ray.origin - ptIntersection|` feeds both the
		// absorption and scattering roll.
		Ray inRay( Point3( 0, 0, -1 ), Vector3( 0, 0, 1 ) );
		RasterizerState rs = {0,0};
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, -1 );
		ri.onb.CreateFromW( ri.vNormal );
		ri.vGeomNormal = Vector3( 0, 0, -1 );
		ri.ptCoord = Point2( 0.5, 0.5 );
		return ri;
	}

	IORStack MakeInsideStack( const IObject* obj, Scalar ior )
	{
		IORStack stack( 1.0 );
		stack.SetCurrentObject( obj );
		stack.push( ior );
		stack.SetCurrentObject( obj );
		return stack;
	}

	//! Counts, over kTrials calls, how many produced a non-absorbed
	//! scattered ray whose direction differs from ri.ray.Dir() by more
	//! than a tight epsilon.  `useNM` selects ScatterNM (nm=550) vs
	//! Scatter (RGB).
	struct Tally
	{
		unsigned int nonAbsorbed;
		unsigned int directionChanged;
		Tally() : nonAbsorbed( 0 ), directionChanged( 0 ) {}
	};

	Tally RunTrials(
		const GenericHumanTissueSPF& spf,
		const RayIntersectionGeometric& ri,
		const IORStack& stack,
		bool useNM,
		unsigned int kTrials,
		unsigned int seed
		)
	{
		Tally t;
		RandomNumberGenerator rng( seed );
		IndependentSampler sampler( rng );

		for( unsigned int i = 0; i < kTrials; i++ ) {
			ScatteredRayContainer scattered;
			if( useNM ) {
				spf.ScatterNM( ri, sampler, 550.0, scattered, stack );
			} else {
				spf.Scatter( ri, sampler, scattered, stack );
			}
			if( scattered.Count() == 0 ) {
				continue;	// absorbed
			}
			t.nonAbsorbed++;
			for( unsigned int k = 0; k < scattered.Count(); k++ ) {
				const Vector3 dir = scattered[k].ray.Dir();
				const Scalar dot = Vector3Ops::Dot( dir, ri.ray.Dir() );
				if( dot < 1.0 - 1e-9 ) {
					t.directionChanged++;
					break;
				}
			}
		}
		return t;
	}

	//! DL-183 red-proof.  `GenericHumanTissueSPF::Scatter`/`ScatterNM`
	//! set `trans.ray.origin = ri.ray.origin` (the INCOMING ray's
	//! origin -- for `MakeInteriorHit()` below, the point (0,0,-1) one
	//! unit behind the surface) instead of `ri.ptIntersection` (the
	//! actual hit point, (0,0,0)) -- BEFORE either the outside/inside
	//! branch or the absorb/scatter/transmit roll, so every emitted
	//! ray from this material starts on the WRONG LINE, not merely a
	//! wrong point on the right one.  The sibling `BioSpecSkinSPF`
	//! sets `remmitted.ray.origin = ri.ptIntersection` correctly at
	//! both of its call sites (see BioSpecSkinSPF.cpp:957/:1040).
	//!
	//! Drives all four combinations (RGB/NM x outside/inside-stack)
	//! and asserts every emitted ray's origin equals `ri.ptIntersection`
	//! exactly (no perturbation of `ri.ptIntersection` itself is in
	//! play here, so this is a bit-exact check, not a tolerance one).
	unsigned int CheckOrigins(
		const GenericHumanTissueSPF& spf,
		const RayIntersectionGeometric& ri,
		const IORStack& stack,
		bool useNM,
		unsigned int kTrials,
		unsigned int seed,
		const char* label
		)
	{
		unsigned int wrongOrigin = 0;
		unsigned int emitted = 0;
		RandomNumberGenerator rng( seed );
		IndependentSampler sampler( rng );

		for( unsigned int i = 0; i < kTrials; i++ ) {
			ScatteredRayContainer scattered;
			if( useNM ) {
				spf.ScatterNM( ri, sampler, 550.0, scattered, stack );
			} else {
				spf.Scatter( ri, sampler, scattered, stack );
			}
			for( unsigned int k = 0; k < scattered.Count(); k++ ) {
				emitted++;
				const Point3 origin = scattered[k].ray.origin;
				const Scalar d = Vector3Ops::Magnitude(
					Vector3Ops::mkVector3( origin, ri.ptIntersection ) );
				if( d > 1e-9 ) {
					wrongOrigin++;
				}
			}
		}

		std::cout << "  " << label << ": emitted=" << emitted
		          << " wrong-origin=" << wrongOrigin << std::endl;
		return wrongOrigin;
	}
}

int main()
{
	std::cout << "=== GenericHumanTissueInteriorScatterTest (DL-184) ===" << std::endl;

	StubObject* obj = new StubObject(); obj->addref();

	UniformScalarPainter* pSca = new UniformScalarPainter( 1.0e6 ); pSca->addref();	// saturates ps -> ~1 for any distance > 0
	UniformScalarPainter* pG   = new UniformScalarPainter( 0.0 );   pG->addref();

	RayIntersectionGeometric ri = MakeInteriorHit();
	IORStack stack = MakeInsideStack( obj, 1.4 );

	const unsigned int kTrials = 20000;
	const unsigned int kMinSamples = 100;	// enough for a meaningful direction-changed fraction

	// diffuse=true: the interior scattering lobe should be a
	// cosine-weighted perturbation around ri.ray.Dir(), i.e. it should
	// differ from ri.ray.Dir() on very nearly every non-absorbed trial.
	{
		GenericHumanTissueSPF* pSpf = new GenericHumanTissueSPF( *pSca, *pG, 0.012, 7.0e-5, 0.05, 0.75, true );
		pSpf->addref();

		Tally rgb = RunTrials( *pSpf, ri, stack, false, kTrials, 12345 );
		std::cout << "  diffuse RGB: non-absorbed=" << rgb.nonAbsorbed
		          << " direction-changed=" << rgb.directionChanged << std::endl;
		Check( rgb.nonAbsorbed > kMinSamples,
			"diffuse RGB: scattering branch reached on most trials (sca saturated)" );
		Check( rgb.directionChanged > ( rgb.nonAbsorbed * 9 ) / 10,
			"DL-184: diffuse RGB interior scatter changes direction on most non-absorbed trials (not dead)" );

		Tally nm = RunTrials( *pSpf, ri, stack, true, kTrials, 54321 );
		std::cout << "  diffuse NM : non-absorbed=" << nm.nonAbsorbed
		          << " direction-changed=" << nm.directionChanged << std::endl;
		Check( nm.nonAbsorbed > kMinSamples,
			"diffuse NM: scattering branch reached on most trials (sca saturated)" );
		Check( nm.directionChanged > ( nm.nonAbsorbed * 9 ) / 10,
			"DL-184: diffuse NM interior scatter changes direction on most non-absorbed trials (not dead)" );

		pSpf->release();
	}

	// diffuse=false (Henyey-Greenstein, g=0.0 -> isotropic-ish): should
	// also differ from ri.ray.Dir() on nearly every trial (g=0 gives no
	// forward bias at all, so the sampled direction lands off-axis with
	// overwhelming probability).
	{
		GenericHumanTissueSPF* pSpf = new GenericHumanTissueSPF( *pSca, *pG, 0.012, 7.0e-5, 0.05, 0.75, false );
		pSpf->addref();

		Tally rgb = RunTrials( *pSpf, ri, stack, false, kTrials, 99991 );
		std::cout << "  HG RGB     : non-absorbed=" << rgb.nonAbsorbed
		          << " direction-changed=" << rgb.directionChanged << std::endl;
		Check( rgb.nonAbsorbed > kMinSamples,
			"HG RGB: scattering branch reached on most trials (sca saturated)" );
		Check( rgb.directionChanged > ( rgb.nonAbsorbed * 9 ) / 10,
			"DL-184: HG RGB interior scatter changes direction on most non-absorbed trials (not dead)" );

		Tally nm = RunTrials( *pSpf, ri, stack, true, kTrials, 11117 );
		std::cout << "  HG NM      : non-absorbed=" << nm.nonAbsorbed
		          << " direction-changed=" << nm.directionChanged << std::endl;
		Check( nm.nonAbsorbed > kMinSamples,
			"HG NM: scattering branch reached on most trials (sca saturated)" );
		Check( nm.directionChanged > ( nm.nonAbsorbed * 9 ) / 10,
			"DL-184: HG NM interior scatter changes direction on most non-absorbed trials (not dead)" );

		pSpf->release();
	}

	// DL-183: scattered-ray origin.  `trans.ray.origin` is set once,
	// before either the outside/inside branch or the absorb/scatter/
	// transmit roll -- so this must hold for every code path: outside
	// vs inside the IOR stack, RGB vs NM, and (implicitly, since the
	// assignment precedes the roll) absorbed-or-not is irrelevant
	// (absorbed trials emit no ray at all and are simply skipped by
	// `CheckOrigins`'s `scattered.Count()` loop).
	{
		IORStack outsideStack( 1.0 );	// pCurrentObject never set -> containsCurrent() == false

		GenericHumanTissueSPF* pSpfDiffuse = new GenericHumanTissueSPF( *pSca, *pG, 0.012, 7.0e-5, 0.05, 0.75, true );
		pSpfDiffuse->addref();
		GenericHumanTissueSPF* pSpfHG = new GenericHumanTissueSPF( *pSca, *pG, 0.012, 7.0e-5, 0.05, 0.75, false );
		pSpfHG->addref();

		unsigned int wrong = 0;
		wrong += CheckOrigins( *pSpfDiffuse, ri, outsideStack, false, kTrials, 2001, "DL-183 outside-stack diffuse RGB" );
		wrong += CheckOrigins( *pSpfDiffuse, ri, outsideStack, true,  kTrials, 2002, "DL-183 outside-stack diffuse NM " );
		wrong += CheckOrigins( *pSpfDiffuse, ri, stack,        false, kTrials, 2003, "DL-183 inside-stack  diffuse RGB" );
		wrong += CheckOrigins( *pSpfDiffuse, ri, stack,        true,  kTrials, 2004, "DL-183 inside-stack  diffuse NM " );
		wrong += CheckOrigins( *pSpfHG,      ri, outsideStack, false, kTrials, 2005, "DL-183 outside-stack HG      RGB" );
		wrong += CheckOrigins( *pSpfHG,      ri, outsideStack, true,  kTrials, 2006, "DL-183 outside-stack HG      NM " );
		wrong += CheckOrigins( *pSpfHG,      ri, stack,        false, kTrials, 2007, "DL-183 inside-stack  HG      RGB" );
		wrong += CheckOrigins( *pSpfHG,      ri, stack,        true,  kTrials, 2008, "DL-183 inside-stack  HG      NM " );

		Check( wrong == 0,
			"DL-183: every emitted ray's origin equals ri.ptIntersection (outside+inside stack, RGB+NM, diffuse+HG)" );

		pSpfDiffuse->release();
		pSpfHG->release();
	}

	pSca->release();
	pG->release();
	obj->release();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;
	return failCount == 0 ? 0 : 1;
}
