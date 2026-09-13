//////////////////////////////////////////////////////////////////////
//
//  BSSRDFProjectionNormalTest.cpp - DL-54 geometric projection PDF.
//
//  SampleEntryPoint samples a disk, then converts that disk density to
//  surface area.  That conversion is geometry, so its three cosines use
//  the geometric normal.  A normal map changes the scattering frame but
//  cannot change how likely the unmodified sphere was to be probed.
//
//  The fixed exit record is synthetic, but the entry probe hits are produced
//  by a real SphereGeometry and NormalMap.  Paired samplers make the selected
//  probe hit and local cosine sample identical, while a retraced geometric
//  disk-to-area oracle checks the reported PDF itself.
//
//////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "../src/Library/Utilities/Math3D/Math3D.h"
#include "../src/Library/Utilities/Math3D/Constants.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/RandomNumbers.h"
#include "../src/Library/Utilities/BSSRDFSampling.h"
#include "../src/Library/Intersection/RayIntersection.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Geometry/SphereGeometry.h"
#include "../src/Library/Objects/Object.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Modifiers/NormalMap.h"
#include "../src/Library/Materials/SubSurfaceScatteringMaterial.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{

static int gFailures = 0;
static const Scalar kTolerance = 5e-10;

class RecordingSampler : public ISampler
{
	RandomNumberGenerator rng;
	std::vector<Scalar> draws;

public:
	explicit RecordingSampler( const unsigned int seed ) : rng( seed ) {}

	Scalar Get1D()
	{
		const Scalar value = rng.CanonicalRandom();
		draws.push_back( value );
		return value;
	}

	Point2 Get2D()
	{
		const Scalar u = Get1D();
		const Scalar v = Get1D();
		return Point2( u, v );
	}
	const std::vector<Scalar>& Draws() const { return draws; }
};

static void Fail( const char* const label )
{
	std::cerr << "FAIL: " << label << std::endl;
	++gFailures;
}

static bool Close( const Scalar actual, const Scalar expected,
	const Scalar tolerance = kTolerance )
{
	return std::fabs( actual - expected ) <= tolerance *
		std::max( Scalar(1), std::fabs(expected) );
}

static void RequireFinitePositive( const Scalar value, const char* const label )
{
	if( !std::isfinite(value) || value <= 0 ) {
		std::cerr << "FAIL: " << label << " must be finite and positive, got "
			<< value << std::endl;
		++gFailures;
	}
}

static void RequireClose( const Scalar actual, const Scalar expected,
	const char* const label )
{
	if( !std::isfinite(actual) || !std::isfinite(expected) || !Close(actual,expected) ) {
		std::cerr << "FAIL: " << label << " actual=" << actual
			<< " expected=" << expected << std::endl;
		++gFailures;
	}
}

static Scalar Distance( const Point3& a, const Point3& b )
{
	return Vector3Ops::Magnitude( Vector3Ops::mkVector3(a,b) );
}

static Scalar Distance( const Vector3& a, const Vector3& b )
{
	return Vector3Ops::Magnitude( a - b );
}

static bool RequireDistanceAtMost( const Point3& actual, const Point3& expected,
	const Scalar tolerance, const char* const label )
{
	const Scalar distance = Distance( actual, expected );
	if( !std::isfinite(distance) || distance > tolerance ) {
		std::cerr << "FAIL: " << label << " distance=" << distance
			<< " tolerance=" << tolerance << std::endl;
		++gFailures;
		return false;
	}
	return true;
}

static bool RequireDistanceAtMost( const Vector3& actual, const Vector3& expected,
	const Scalar tolerance, const char* const label )
{
	const Scalar distance = Distance( actual, expected );
	if( !std::isfinite(distance) || distance > tolerance ) {
		std::cerr << "FAIL: " << label << " distance=" << distance
			<< " tolerance=" << tolerance << std::endl;
		++gFailures;
		return false;
	}
	return true;
}

static Object* MakeUnitSphere()
{
	SphereGeometry* geometry = new SphereGeometry( 1.0 );
	Object* object = new Object( geometry );
	geometry->release();
	return object;
}

static RayIntersectionGeometric MakeExitRecord()
{
	const Point3 point( 0, -1, 0 );
	const Vector3 normal( 0, -1, 0 );
	const Vector3 incoming( 0, 1, 0 );
	RayIntersectionGeometric ri(
		Ray( Point3Ops::mkPoint3(point, -incoming * 2.0), incoming ),
		nullRasterizerState );
	ri.bHit = true;
	ri.ptIntersection = point;
	ri.vNormal = normal;
	ri.vGeomNormal = normal;
	ri.onb.CreateFromW( normal );
	return ri;
}

struct ProbeReference
{
	Point3 point;
	Vector3 geometricNormal;
	int numHits;
};

// Reconstruct the probe traversal from the fixed sampler prefix.  It does
// not read SampleResult::pdfSurface: the geometric PDF oracle below is
// calculated from the selected geometric normal and retraced hit chain.
static bool SelectedProbeHit(
	const RayIntersectionGeometric& ri,
	const IObject* const sphere,
	const ISubSurfaceDiffusionProfile* const profile,
	const std::vector<Scalar>& draws,
	ProbeReference& result )
{
	if( draws.size() < 5 ) return false;

	const int channel = std::min( 2, static_cast<int>(draws[0] * 3.0) );
	const Scalar axisSample = draws[1];
	Vector3 probeAxis, perpU, perpV;
	if( axisSample < 0.5 ) {
		probeAxis = ri.onb.w();
		perpU = ri.onb.u();
		perpV = ri.onb.v();
	} else if( axisSample < 0.75 ) {
		probeAxis = ri.onb.u();
		perpU = ri.onb.w();
		perpV = ri.onb.v();
	} else {
		probeAxis = ri.onb.v();
		perpU = ri.onb.w();
		perpV = ri.onb.u();
	}

	const Scalar radius = profile->SampleRadius( draws[2], channel, ri );
	if( radius <= 0 ) return false;
	const Scalar phi = TWO_PI * draws[3];
	const Point3 probeCenter = Point3Ops::mkPoint3( ri.ptIntersection,
		perpU * (radius * cos(phi)) + perpV * (radius * sin(phi)) );

	struct RawHit { Point3 point; Vector3 normal; };
	std::vector<RawHit> hits;
	const Scalar maxDistance = profile->GetMaximumDistanceForError( 1e-4 );
	for( int sign = 0; sign < 2; ++sign ) {
		const Vector3 direction = sign == 0 ? probeAxis : -probeAxis;
		Ray probeRay( probeCenter, direction );
		probeRay.Advance( BSSRDFSampling::BSSRDF_RAY_EPSILON );
		Scalar traveled = 0;
		for( int bounce = 0; bounce < 64; ++bounce ) {
			const Scalar remaining = maxDistance - traveled;
			if( remaining < BSSRDFSampling::BSSRDF_RAY_EPSILON ) break;
			RayIntersection probeRI( probeRay, nullRasterizerState );
			sphere->IntersectRay( probeRI, remaining, true, true, false );
			if( !probeRI.geometric.bHit ) break;
			hits.push_back( RawHit{ probeRI.geometric.ptIntersection,
				probeRI.geometric.vGeomNormal } );
			traveled += probeRI.geometric.range;
			probeRay = Ray( probeRI.geometric.ptIntersection, direction );
			probeRay.Advance( BSSRDFSampling::BSSRDF_RAY_EPSILON );
			traveled += BSSRDFSampling::BSSRDF_RAY_EPSILON;
		}
	}

	if( hits.empty() ) return false;
	const int selected = std::min( static_cast<int>(hits.size()) - 1,
		static_cast<int>(draws[4] * hits.size()) );
	result.point = hits[selected].point;
	result.geometricNormal = hits[selected].normal;
	result.numHits = static_cast<int>( hits.size() );
	return true;
}

// Independent disk-to-area Jacobian.  The normal is intentionally supplied
// by the retraced geometry, never by SampleResult::entryNormal.
static Scalar GeometricSurfacePdf(
	const RayIntersectionGeometric& exit,
	const ProbeReference& entry,
	const ISubSurfaceDiffusionProfile* const profile )
{
	const Vector3 offset = Vector3Ops::mkVector3( exit.ptIntersection, entry.point );
	const Scalar dN = Vector3Ops::Dot( offset, exit.onb.w() );
	const Scalar dT = Vector3Ops::Dot( offset, exit.onb.u() );
	const Scalar dB = Vector3Ops::Dot( offset, exit.onb.v() );
	const Scalar radii[3] = {
		sqrt(dT*dT + dB*dB), sqrt(dN*dN + dB*dB), sqrt(dN*dN + dT*dT)
	};
	const Scalar cosines[3] = {
		fabs(Vector3Ops::Dot(entry.geometricNormal,exit.onb.w())),
		fabs(Vector3Ops::Dot(entry.geometricNormal,exit.onb.u())),
		fabs(Vector3Ops::Dot(entry.geometricNormal,exit.onb.v()))
	};
	const Scalar axisProbability[3] = { 0.5, 0.25, 0.25 };
	Scalar pdf = 0;
	for( int axis = 0; axis < 3; ++axis ) {
		if( radii[axis] < 1e-10 || cosines[axis] < 1e-6 ) continue;
		Scalar meanRadialPdf = 0;
		for( int channel = 0; channel < 3; ++channel ) {
			meanRadialPdf += profile->PdfRadius( radii[axis], channel, exit );
		}
		pdf += axisProbability[axis] * (meanRadialPdf / 3.0) * cosines[axis] /
			(TWO_PI * radii[axis]);
	}
	return pdf / entry.numHits;
}

static bool SameDraws( const RecordingSampler& a, const RecordingSampler& b )
{
	const std::vector<Scalar>& left = a.Draws();
	const std::vector<Scalar>& right = b.Draws();
	if( left.size() != right.size() ) return false;
	for( size_t i = 0; i < left.size(); ++i ) {
		if( left[i] != right[i] ) return false;
	}
	return true;
}

static void TestGeometricProjectionPdf( const Scalar nm, const char* const label )
{
	std::cout << "Test " << label
		<< ": curved-sphere projection PDF ignores normal-map tilt" << std::endl;

	Object* plainSphere = MakeUnitSphere();
	Object* tiltedSphere = MakeUnitSphere();
	UniformColorPainter* normalMap = new UniformColorPainter( RISEPel(0.8,0.5,1.0) );
	NormalMap* modifier = new NormalMap( *normalMap, 1.0 );
	tiltedSphere->AssignModifier( *modifier );
	modifier->release();
	normalMap->release();

	UniformScalarPainter* ior = new UniformScalarPainter( 1.3 );
	RGBScalarPainter* absorption = new RGBScalarPainter(0.05,0.10,0.20);
	RGBScalarPainter* scattering = new RGBScalarPainter(1.0,1.0,1.0);
	SubSurfaceScatteringMaterial* material = new SubSurfaceScatteringMaterial(
		*ior, *absorption, *scattering, 0.0, 0.2 );
	ISubSurfaceDiffusionProfile* profile = material->GetDiffusionProfile();
	const RayIntersectionGeometric exit = MakeExitRecord();

	int active = 0;
	int matchingActivity = 0;
	int tiltedNormals = 0;
	int changedDirections = 0;
	int axisActivity[3] = { 0, 0, 0 };
	for( unsigned int attempt = 0; attempt < 128; ++attempt ) {
		RecordingSampler plainSampler( 5400u + attempt );
		RecordingSampler tiltedSampler( 5400u + attempt );
		const BSSRDFSampling::SampleResult plain = BSSRDFSampling::SampleEntryPoint(
			exit, plainSphere, material, plainSampler, nm );
		const BSSRDFSampling::SampleResult tilted = BSSRDFSampling::SampleEntryPoint(
			exit, tiltedSphere, material, tiltedSampler, nm );

		if( !SameDraws(plainSampler,tiltedSampler) ) Fail( "paired samplers consumed different draws" );
		if( plain.valid != tilted.valid ) {
			Fail( "normal-map modifier changed probe-hit activity" );
			continue;
		}
		if( !plain.valid ) continue;
		if( plainSampler.Draws().size() != 7 || tiltedSampler.Draws().size() != 7 ) {
			Fail( "valid BSSRDF sample must use the fixed seven-draw sequence" );
			continue;
		}
		++active;

		const Point3 plainSurface = Point3Ops::mkPoint3( plain.entryPoint,
			-plain.entryNormal * BSSRDFSampling::BSSRDF_RAY_EPSILON );
		const Point3 tiltedSurface = Point3Ops::mkPoint3( tilted.entryPoint,
			-tilted.entryNormal * BSSRDFSampling::BSSRDF_RAY_EPSILON );
		if( RequireDistanceAtMost(plainSurface,tiltedSurface,2e-8,
			"paired modifier samples selected different geometric surface points") ) {
			++matchingActivity;
		}
		const Scalar axisDraw = plainSampler.Draws()[1];
		const int axis = axisDraw < 0.5 ? 0 : (axisDraw < 0.75 ? 1 : 2);
		++axisActivity[axis];

		ProbeReference reference;
		if( !SelectedProbeHit(exit,plainSphere,profile,plainSampler.Draws(),reference) ) {
			Fail( "retraced probe chain did not recover selected hit" );
			continue;
		}
		RequireDistanceAtMost(plainSurface,reference.point,2e-8,
			"plain reported entry point disagrees with retraced sphere hit");
		RequireDistanceAtMost(tiltedSurface,reference.point,2e-8,
			"tilted reported entry point disagrees with retraced sphere hit");
		RequireDistanceAtMost(plain.entryGeomNormal,reference.geometricNormal,2e-10,
			"plain entry geometric normal must remain the sphere normal");
		RequireDistanceAtMost(tilted.entryGeomNormal,reference.geometricNormal,2e-10,
			"tilted entry geometric normal must remain the sphere normal");
		const Scalar tiltedGeomDot = Vector3Ops::Dot(
			tilted.entryNormal,tilted.entryGeomNormal);
		if( std::isfinite(tiltedGeomDot) && tiltedGeomDot < 0.90 ) {
			++tiltedNormals;
		} else {
			Fail( "normal-map modifier did not tilt the shading normal" );
		}

		const Scalar referencePdf = GeometricSurfacePdf( exit, reference, profile );
		RequireFinitePositive( referencePdf, "retraced geometric surface PDF" );
		RequireFinitePositive( plain.pdfSurface, "plain reported surface PDF" );
		RequireFinitePositive( tilted.pdfSurface, "tilted reported surface PDF" );
		RequireClose( plain.pdfSurface, referencePdf,
			"plain PDF must equal retraced geometric Jacobian" );
		RequireClose( tilted.pdfSurface, referencePdf,
			"tilted PDF must equal retraced geometric Jacobian" );
		RequireClose( tilted.pdfSurface, plain.pdfSurface,
			"shading-normal tilt must not change surface PDF" );

		const Scalar localPlain = Vector3Ops::Dot(plain.scatteredRay.Dir(),plain.entryNormal);
		const Scalar localTilted = Vector3Ops::Dot(tilted.scatteredRay.Dir(),tilted.entryNormal);
		RequireFinitePositive( localPlain, "plain local continuation cosine" );
		RequireFinitePositive( localTilted, "tilted local continuation cosine" );
		RequireClose( localPlain, localTilted,
			"paired samples retain the same local cosine draw" );
		const Scalar directionDistance = Distance(
			plain.scatteredRay.Dir(),tilted.scatteredRay.Dir());
		if( std::isfinite(directionDistance) && directionDistance > 1e-5 ) {
			++changedDirections;
		} else {
			Fail( "normal-map modifier did not rotate the continuation direction" );
		}

		// Spatial weights depend on the disk-to-area PDF and therefore must be
		// invariant.  The full-weight equality here follows only because this
		// paired fixture keeps the local cosine sample identical; it is not a
		// claim that Sw is invariant for one fixed world-space direction.
		for( int channel = 0; channel < 3; ++channel ) {
			RequireFinitePositive( plain.weightSpatial[channel], "plain RGB spatial weight" );
			RequireFinitePositive( tilted.weightSpatial[channel], "tilted RGB spatial weight" );
			RequireFinitePositive( plain.weight[channel], "plain RGB full weight" );
			RequireFinitePositive( tilted.weight[channel], "tilted RGB full weight" );
			RequireClose( tilted.weightSpatial[channel], plain.weightSpatial[channel],
				"shading-normal tilt must not change RGB spatial weight" );
			RequireClose( tilted.weight[channel], plain.weight[channel],
				"paired RGB full weight must preserve its local Sw sample" );
		}
		RequireFinitePositive( plain.weightSpatialNM, "plain NM spatial weight" );
		RequireFinitePositive( tilted.weightSpatialNM, "tilted NM spatial weight" );
		RequireFinitePositive( plain.weightNM, "plain NM full weight" );
		RequireFinitePositive( tilted.weightNM, "tilted NM full weight" );
		RequireClose( tilted.weightSpatialNM, plain.weightSpatialNM,
			"shading-normal tilt must not change NM spatial weight" );
		RequireClose( tilted.weightNM, plain.weightNM,
			"paired NM full weight must preserve its local Sw sample" );
	}

	std::cout << "  active=" << active << "/128 matched=" << matchingActivity
		<< " tilted-normals=" << tiltedNormals
		<< " rotated-directions=" << changedDirections
		<< " axes=" << axisActivity[0] << "/" << axisActivity[1]
		<< "/" << axisActivity[2] << std::endl;
	if( active < 16 ) Fail( "curved BSSRDF fixture produced too few valid samples" );
	if( matchingActivity != active ) Fail( "valid samples must have matching geometric activity" );
	if( tiltedNormals != active ) Fail( "every valid tilted sample must carry the modifier normal" );
	if( changedDirections != active ) Fail( "every valid tilted sample must use its shading frame" );
	for( int axis = 0; axis < 3; ++axis ) {
		if( axisActivity[axis] == 0 ) Fail( "all three disk-projection axes need valid activity" );
	}

	material->release();
	ior->release();
	absorption->release();
	scattering->release();
	plainSphere->release();
	tiltedSphere->release();
}

} // namespace

int main()
{
	std::cout << "=== DL-54 BSSRDF geometric projection normal ===" << std::endl;
	TestGeometricProjectionPdf( 0.0, "RGB" );
	TestGeometricProjectionPdf( 550.0, "NM 550nm" );
	if( gFailures ) {
		std::cerr << "=== DL-54 failures: " << gFailures << " ===" << std::endl;
		return 1;
	}
	std::cout << "=== All DL-54 projection-normal tests passed ===" << std::endl;
	return 0;
}
