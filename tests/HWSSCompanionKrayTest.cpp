//////////////////////////////////////////////////////////////////////
//
//  HWSSCompanionKrayTest.cpp - Active HWSS companion-weight (DL-125 /
//    DL-222) contract test for TranslucentSPF and negative controls.
//
//    1. NEGATIVE CONTROLS: SPFs for which the aggregate companion
//       fallback is exact (`GGXSPF`, `LambertianSPF`) must keep
//       declining (`EvaluateKrayNM < 0`) and must not name themselves
//       to `PerLobeDensityFallbackName()`.
//    2. TRANSLUCENTSPF (DL-222): entry diffuse/Phong lobe weights
//       match the reflection/transmission painters, interior exit
//       diffuse/backscatter weights match Beer extinction times
//       (1 - scattering) / scattering, unsupported scatter types
//       decline while retaining the diagnostic identity, and
//       `BDPTIntegrator::RecomputeSubpathThroughputNM` replays both
//       eye and light subpaths using the recorded non-unit incoming
//       Beer distance.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/ISPF.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/ILog.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/TranslucentSPF.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Materials/GGXSPF.h"
#include "../src/Library/Materials/LambertianSPF.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Scene.h"
#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const std::string& testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static StubObject* g_stubObject = 0;

//! A scalar painter whose value ramps linearly with wavelength.
class LambdaRampScalarPainter :
	public virtual IScalarPainter,
	public virtual Reference
{
protected:
	const Scalar base;
	const Scalar slope;			///< per nm
	virtual ~LambdaRampScalarPainter() {}
public:
	LambdaRampScalarPainter( Scalar b, Scalar s ) : base( b ), slope( s ) {}

	ScalarTriple GetValuesAt( const RayIntersectionGeometric& ) const
	{
		return ScalarTriple( base + slope * ( 549.0 - 550.0 ) );
	}

	Scalar GetValueAtNM( const RayIntersectionGeometric&, Scalar nm ) const
	{
		return base + slope * ( nm - 550.0 );
	}
};

static RayIntersectionGeometric MakeIntersection( double incomingTheta )
{
	const double sinT = sin( incomingTheta );
	const double cosT = cos( incomingTheta );
	const Vector3 inDir( sinT, 0, -cosT );

	Ray inRay( Point3( sinT, 0, 1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );

	ri.bHit = true;
	ri.range = 1.0 / cosT;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.vGeomNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );

	return ri;
}

static double RelDiff( double a, double b )
{
	const double d = std::fabs( a - b );
	const double m = std::max( std::fabs( a ), std::fabs( b ) );
	return ( m > 1e-300 ) ? ( d / m ) : d;
}

int main()
{
	GlobalLog();

	std::cout << "=== HWSSCompanionKrayTest (DL-125 / DL-222) ===" << std::endl;
	std::cout << std::fixed << std::setprecision( 6 );

	g_stubObject = new StubObject();
	g_stubObject->addref();
	IORStack iorStack = MakeTestIORStack( g_stubObject );

	UniformColorPainter* spec = new UniformColorPainter( RISEPel( 0.75, 0.30, 0.12 ) ); spec->addref();
	UniformColorPainter* diff = new UniformColorPainter( RISEPel( 0.20, 0.55, 0.80 ) ); diff->addref();

	std::cout << std::endl << "-- Section D: controls and Translucent lobe/replay oracles" << std::endl;
	{
		const RayIntersectionGeometric ri = MakeIntersection( 30.0 * PI / 180.0 );
		const Vector3 wo = Vector3Ops::Normalize( Vector3( 0.3, 0.2, 0.9 ) );

		// GGXSPF: the fallback is exact for it (single emitted lobe whose
		// `.pdf` is the aggregate mixture density), so it must keep
		// declining and must not name itself to the diagnostic.
		UniformScalarPainter* ggxAX  = new UniformScalarPainter( 0.3 ); ggxAX->addref();
		UniformScalarPainter* ggxAY  = new UniformScalarPainter( 0.3 ); ggxAY->addref();
		UniformScalarPainter* ggxIOR = new UniformScalarPainter( 1.5 ); ggxIOR->addref();
		UniformScalarPainter* ggxExt = new UniformScalarPainter( 0.0 ); ggxExt->addref();
		GGXSPF* ggx = new GGXSPF( *diff, *spec, *ggxAX, *ggxAY, *ggxIOR, *ggxExt ); ggx->addref();
		Check( ggx->EvaluateKrayNM( ri, wo, ScatteredRay::eRayReflection, 550.0, iorStack ) < 0,
			"DL-125 section D (GGXSPF): still declines -- its companion fallback is exact" );
		Check( ggx->PerLobeDensityFallbackName() == 0,
			"DL-125 section D (GGXSPF): does not name itself to the fallback diagnostic" );
		ggx->release(); ggxAX->release(); ggxAY->release(); ggxIOR->release(); ggxExt->release();

		LambertianSPF* lam = new LambertianSPF( *diff ); lam->addref();
		Check( lam->EvaluateKrayNM( ri, wo, ScatteredRay::eRayDiffuse, 550.0, iorStack ) < 0,
			"DL-125 section D (LambertianSPF): still declines -- single lobe, aggregate density" );
		Check( lam->PerLobeDensityFallbackName() == 0,
			"DL-125 section D (LambertianSPF): does not name itself to the fallback diagnostic" );
		lam->release();

		// DL-222 integration: derive from painter and Beer/scattering
		// inputs, independently of ScatterNM and BuildLobeSet.
		UniformScalarPainter* tN = new UniformScalarPainter( 8.0 );
		LambdaRampScalarPainter* tExt = new LambdaRampScalarPainter( 0.2, 0.0005 );
		LambdaRampScalarPainter* tS = new LambdaRampScalarPainter( 0.3, 0.0005 );
		TranslucentMaterial* transMat = new TranslucentMaterial( *diff, *spec, *tExt, *tN, *tS );
		const ISPF* trans = transMat->GetSPF();
		IORStack inside = iorStack;
		inside.push( inside.top() );
		Check( !iorStack.containsCurrent() && inside.containsCurrent(),
			"DL-222: entry and exit oracle stacks differ in actual object membership" );
		Scene* scene = new Scene();
		StandardShader* shader = new StandardShader( std::vector<IShaderOp*>() );
		RayCaster* caster = new RayCaster( false, 8, *shader, false );
		BDPTIntegrator* integrator = new BDPTIntegrator( 4, 4, StabilityConfig() );
		const Scalar distances[] = { 0.4, 2.5 };
		const Scalar wavelengths[] = { 450, 550, 650 };
		for( Scalar distance : distances ) {
			RayIntersectionGeometric entry = MakeIntersection( 0 );
			entry.ray.Set( Point3( 0, 0, distance ), Vector3( 0, 0, -1 ) );
			RayIntersectionGeometric exit = MakeIntersection( 0 );
			exit.ray.Set( Point3( 0, 0, -distance ), Vector3( 0, 0, 1 ) );
			for( Scalar nm : wavelengths ) {
				const Scalar sigma = 0.2 + 0.0005 * ( nm - 550 );
				const Scalar split = 0.3 + 0.0005 * ( nm - 550 );
				const Scalar beer = std::exp( -sigma * distance );
				Check( RelDiff( trans->EvaluateKrayNM( entry, Vector3(0,0,1), ScatteredRay::eRayDiffuse, nm, iorStack ), diff->GetColorNM(entry,nm) ) < 1e-12,
					"DL-222 entry: diffuse lobe equals reflection painter" );
				Check( RelDiff( trans->EvaluateKrayNM( entry, Vector3(0,0,-1), ScatteredRay::eRayTranslucent, nm, iorStack ), spec->GetColorNM(entry,nm) ) < 1e-12,
					"DL-222 entry: Phong lobe equals transmission painter" );
				Check( RelDiff( trans->EvaluateKrayNM( exit, Vector3(0,0,1), ScatteredRay::eRayDiffuse, nm, inside ), beer*(1-split) ) < 1e-12,
					"DL-222 exit: diffuse lobe equals Beer times one minus scattering" );
				Check( RelDiff( trans->EvaluateKrayNM( exit, Vector3(0,0,-1), ScatteredRay::eRayTranslucent, nm, inside ), beer*split ) < 1e-12,
					"DL-222 exit: backscatter lobe equals Beer times scattering" );
				Check( trans->EvaluateKrayNM( exit, wo, ScatteredRay::eRayRefraction, nm, inside ) < 0,
					"DL-222: unsupported scatter type still declines" );
				for( bool light : { false, true } ) {
					for( ScatteredRay::ScatRayType lobe : { ScatteredRay::eRayDiffuse, ScatteredRay::eRayTranslucent } ) {
						std::vector<BDPTVertex> vertices( 4 );
						// The geometric predecessor is deliberately 0.125 further
						// away than the actual live origin: using predecessor
						// position instead of recorded distance must fail too.
						vertices[0].type = BDPTVertex::CAMERA;
						vertices[0].position = Point3( 0, 0, -distance-0.125 );
						vertices[1].position = Point3( 0, 0, 0 );
						vertices[1].normal = vertices[1].geomNormal = Vector3( 0, 0, 1 );
						vertices[1].onb.CreateFromW( Vector3( 0, 0, 1 ) );
						vertices[1].pMaterial = transMat;
						vertices[1].pObject = g_stubObject;
						vertices[1].insideObject = true;
						vertices[1].scatterType = lobe;
						vertices[1].scatterIncomingDistance = distance;
						vertices[2].position = Point3( 0, 0, lobe == ScatteredRay::eRayDiffuse ? 1 : -1 );
						vertices[2].isDelta = true;
						vertices[3].position = Point3( 0, 0, 2 );
						for( size_t j = 0; j < vertices.size(); ++j ) vertices[j].throughputNM = j + 1;
						integrator->RecomputeSubpathThroughputNM( vertices, light, 550, nm, *scene, *caster );
						const Scalar splitRatio = lobe == ScatteredRay::eRayDiffuse ? (1-split)/0.7 : split/0.3;
						const Scalar expectedRatio = std::exp( -(sigma-0.2)*distance ) * splitRatio;
						Check( vertices[0].throughputNM == 1 && vertices[1].throughputNM == 2,
							"DL-222 replay: scatter ratio does not affect its own vertex or predecessor" );
						Check( RelDiff( vertices[2].throughputNM, 3*expectedRatio ) < 1e-12 &&
						       RelDiff( vertices[3].throughputNM, 4*expectedRatio ) < 1e-12,
							"DL-222 replay: eye/light downstream throughput uses recorded non-unit Beer distance" );
					}
				}
			}
		}
		Check( trans->PerLobeDensityFallbackName() != 0 &&
		       std::string( trans->PerLobeDensityFallbackName() ) == "TranslucentSPF",
			"DL-222: unsupported lobe retains its diagnostic identity" );
		std::cout << "   BDPTVertex storage: " << sizeof(BDPTVertex) << " bytes" << std::endl;
		integrator->release(); caster->release(); shader->release(); scene->release();
		transMat->release(); tExt->release(); tN->release(); tS->release();
	}

	spec->release(); diff->release();
	g_stubObject->release();

	std::cout << std::endl << "=== Results: " << passCount << " passed, "
	          << failCount << " failed ===" << std::endl;
	return failCount == 0 ? 0 : 1;
}
