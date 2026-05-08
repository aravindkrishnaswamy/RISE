//////////////////////////////////////////////////////////////////////
//
//  UVTransformPainterTest.cpp - Landing 12.A unit test.
//
//  Verifies UVTransformPainter applies the glTF KHR_texture_transform
//  affine TRS correctly to an input (u, v) before sampling the
//  wrapped source painter.  The transform matches the spec:
//    u' =  cos(r) * sx * u + sin(r) * sy * v + tx
//    v' = -sin(r) * sx * u + cos(r) * sy * v + ty
//  with R using -r so positive rotation rotates the IMAGE clockwise.
//
//  Approach: wrap a test painter that ECHOES the input UV as RGB
//  (R=u, G=v, B=0) — then GetColor at (u,v) reveals exactly which
//  UV the wrapper sampled at.  Covers identity, translation,
//  scale, rotation (90° + 45°), and combined TRS.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>

#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UVTransformPainter.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Interfaces/IPainter.h"
#include "../src/Library/Utilities/Reference.h"

using namespace RISE;
using namespace RISE::Implementation;

static int s_pass = 0;
static int s_fail = 0;

static bool Close( double a, double b, double eps = 1e-6 )
{
	return std::fabs( a - b ) <= eps;
}

static bool Check( bool ok, const char* what )
{
	if( ok ) {
		++s_pass;
		std::cout << "  PASS  " << what << "\n";
	} else {
		++s_fail;
		std::cout << "  FAIL  " << what << "\n";
	}
	return ok;
}

// Test painter: returns the input UV as color (R=u, G=v, B=0).
// Lets us read back exactly which UV the wrapper sampled at.
class UVEchoPainter : public Painter
{
public:
	UVEchoPainter() {}
	~UVEchoPainter() {}

	RISEPel GetColor( const RayIntersectionGeometric& ri ) const
	{
		return RISEPel( ri.ptCoord.x, ri.ptCoord.y, 0.0 );
	}

	Scalar GetColorNM( const RayIntersectionGeometric& ri, const Scalar /*nm*/ ) const
	{
		return ri.ptCoord.x;	// echo U
	}

	Scalar GetAlpha( const RayIntersectionGeometric& ri ) const
	{
		return ri.ptCoord.x;	// echo U as alpha
	}

	IKeyframeParameter* KeyframeFromParameters( const String&, const String& ) { return 0; }
	void SetIntermediateValue( const IKeyframeParameter& ) {}
	void RegenerateData() {}
};

static RayIntersectionGeometric MakeRiAtUV( double u, double v )
{
	const Ray r( Point3( 0, 0, 0 ), Vector3( 0, 0, 1 ) );
	const RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( r, rs );
	ri.bHit = true;
	ri.ptCoord = Point2( u, v );
	return ri;
}

static void TestIdentity()
{
	std::cout << "\n[1/5] Identity transform = passthrough\n";
	UVEchoPainter src; src.addref();
	IPainter* wrapper = 0;
	RISE_API_CreateUVTransformPainter( &wrapper, src,
		/*offset_u*/ 0.0, /*offset_v*/ 0.0,
		/*rotation*/ 0.0,
		/*scale_u*/ 1.0, /*scale_v*/ 1.0 );
	assert( wrapper );

	const RayIntersectionGeometric ri = MakeRiAtUV( 0.3, 0.7 );
	const RISEPel c = wrapper->GetColor( ri );
	Check( Close( c.r, 0.3 ) && Close( c.g, 0.7 ),
		"identity passes (u, v) unchanged" );

	wrapper->release();
	src.release();
}

static void TestTranslation()
{
	std::cout << "\n[2/5] Translation only: u' = u + tx, v' = v + ty\n";
	UVEchoPainter src; src.addref();
	IPainter* wrapper = 0;
	RISE_API_CreateUVTransformPainter( &wrapper, src,
		/*offset_u*/ 0.5, /*offset_v*/ -0.25,
		/*rotation*/ 0.0,
		/*scale_u*/ 1.0, /*scale_v*/ 1.0 );

	const RayIntersectionGeometric ri = MakeRiAtUV( 0.1, 0.2 );
	const RISEPel c = wrapper->GetColor( ri );
	Check( Close( c.r, 0.6 ) && Close( c.g, -0.05 ),
		"translation adds offset to (u, v)" );

	wrapper->release();
	src.release();
}

static void TestScale()
{
	std::cout << "\n[3/5] Scale only: u' = sx * u, v' = sy * v\n";
	UVEchoPainter src; src.addref();
	IPainter* wrapper = 0;
	RISE_API_CreateUVTransformPainter( &wrapper, src,
		/*offset_u*/ 0.0, /*offset_v*/ 0.0,
		/*rotation*/ 0.0,
		/*scale_u*/ 2.0, /*scale_v*/ 0.5 );

	const RayIntersectionGeometric ri = MakeRiAtUV( 0.25, 0.4 );
	const RISEPel c = wrapper->GetColor( ri );
	Check( Close( c.r, 0.5 ) && Close( c.g, 0.2 ),
		"scale multiplies (u, v) by (sx, sy)" );

	wrapper->release();
	src.release();
}

static void TestRotation90()
{
	std::cout << "\n[4/5] 90 deg rotation (KHR sign): (1, 0) -> (0, -1)\n";
	UVEchoPainter src; src.addref();
	IPainter* wrapper = 0;
	const double pi_2 = 3.14159265358979323846 / 2.0;
	RISE_API_CreateUVTransformPainter( &wrapper, src,
		/*offset_u*/ 0.0, /*offset_v*/ 0.0,
		/*rotation*/ pi_2,
		/*scale_u*/ 1.0, /*scale_v*/ 1.0 );

	// (1, 0): cos(pi/2) * 1 + sin(pi/2) * 0 = 0
	//         -sin(pi/2) * 1 + cos(pi/2) * 0 = -1
	const RayIntersectionGeometric ri = MakeRiAtUV( 1.0, 0.0 );
	const RISEPel c = wrapper->GetColor( ri );
	Check( Close( c.r, 0.0 ) && Close( c.g, -1.0 ),
		"90 deg rotation maps (1, 0) to (0, -1)" );

	// (0, 1): cos(pi/2) * 0 + sin(pi/2) * 1 = 1
	//         -sin(pi/2) * 0 + cos(pi/2) * 1 = 0
	const RayIntersectionGeometric ri2 = MakeRiAtUV( 0.0, 1.0 );
	const RISEPel c2 = wrapper->GetColor( ri2 );
	Check( Close( c2.r, 1.0 ) && Close( c2.g, 0.0 ),
		"90 deg rotation maps (0, 1) to (1, 0)" );

	wrapper->release();
	src.release();
}

static void TestCombinedTRS()
{
	std::cout << "\n[5/5] Combined T * R * S\n";
	UVEchoPainter src; src.addref();
	IPainter* wrapper = 0;
	const double pi_2 = 3.14159265358979323846 / 2.0;
	// scale(2, 3), rotate 90 deg, translate (0.1, 0.2):
	// (u=1, v=1) -> scale -> (2, 3) -> rotate -> (3, -2) -> translate -> (3.1, -1.8)
	RISE_API_CreateUVTransformPainter( &wrapper, src,
		/*offset_u*/ 0.1, /*offset_v*/ 0.2,
		/*rotation*/ pi_2,
		/*scale_u*/ 2.0, /*scale_v*/ 3.0 );

	const RayIntersectionGeometric ri = MakeRiAtUV( 1.0, 1.0 );
	const RISEPel c = wrapper->GetColor( ri );
	Check( Close( c.r, 3.1 ) && Close( c.g, -1.8 ),
		"T * R * S applied in correct order" );

	// GetAlpha forwards through transform too: at (u=1, v=1), wrapper
	// samples source at (3.1, -1.8); UVEchoPainter::GetAlpha returns U
	// of the SAMPLED uv, so we expect 3.1.
	const Scalar a = wrapper->GetAlpha( ri );
	Check( Close( (double)a, 3.1 ),
		"GetAlpha forwards through transform (matches alpha-cutout path)" );

	wrapper->release();
	src.release();
}

int main()
{
	std::cout << "UVTransformPainterTest -- KHR_texture_transform math\n";

	TestIdentity();
	TestTranslation();
	TestScale();
	TestRotation90();
	TestCombinedTRS();

	std::cout << "\n=========================================\n";
	std::cout << "  Pass: " << s_pass << "  Fail: " << s_fail << "\n";
	std::cout << "=========================================\n";
	return s_fail == 0 ? 0 : 1;
}
