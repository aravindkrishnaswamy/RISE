//////////////////////////////////////////////////////////////////////
//
//  MappingPainter.cpp - Implementation of MappingPainter (doc 88
//  P2.3).  See MappingPainter.h for the design rationale.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "MappingPainter.h"

using namespace RISE;
using namespace RISE::Implementation;

MappingPainter::MappingPainter(
	const IPainter&		source_,
	const Projection	projection_,
	const Vector3&		scale,
	const Vector3&		rotateDeg,
	const Vector3&		translate,
	const Scalar		blendSharpness_ )
	:
	source( source_ ),
	projection( projection_ ),
	blendSharpness( blendSharpness_ )
{
	source.addref();

	// ---- 2D (uv) transform: scale.x/y, rotate.z, translate.x/y only.
	// scale.z, rotate.x, rotate.y, translate.z are IGNORED for uv --
	// see the file header comment for why (a 2D domain has no z-axis
	// to inherit rotation-mixing from).
	scaleU = scale.x;
	scaleV = scale.y;
	translateU = translate.x;
	translateV = translate.y;
	const Scalar rzRad = rotateDeg.z * DEG_TO_RAD;
	cosRz = Scalar( std::cos( rzRad ) );
	sinRz = Scalar( std::sin( rzRad ) );
	isIdentityUV =
		std::fabs( scaleU - Scalar(1) ) < Scalar(1e-9) &&
		std::fabs( scaleV - Scalar(1) ) < Scalar(1e-9) &&
		std::fabs( rzRad )              < Scalar(1e-9) &&
		std::fabs( translateU )         < Scalar(1e-9) &&
		std::fabs( translateV )         < Scalar(1e-9);

	// ---- 3D transform: full scale/rotate/translate, composed as
	// p' = translate + Rx(Ry(Rz(scale * p))) -- i.e. the combined
	// matrix M = Translation(translate) * XRotation(rx) * YRotation(ry) *
	// ZRotation(rz) * Stretch(scale), applied once via
	// Point3Ops::Transform( M, p ) (row-vector convention: p' = p*M).
	// (P2.6, S7 review round 1: this formula previously read "p' =
	// translate + Rz(Ry(Rx(scale*p)))" with the matrix product written
	// Stretch-leftmost/Translation-rightmost -- backwards from both the
	// actual code below AND the correct derivation two paragraphs down,
	// which already established Stretch is RIGHTMOST/first and rotation
	// composes Z-then-Y-then-X, i.e. X is innermost-of-the-rotations but
	// applied LAST overall, giving Rx(Ry(Rz(...))) reading outside-in.)
	// Matrix4Ops::operator* composes so that `A * B` applies B FIRST,
	// then A (Transform(A*B,p) == Transform(A,Transform(B,p))) -- i.e.
	// the RIGHTMOST factor is innermost/first, matching ordinary
	// function-composition notation.  So to get "scale first, then
	// rotate, then translate", the product must be written with Stretch
	// RIGHTMOST and Translation LEFTMOST: Translation * (rotation) *
	// Stretch.  This is the EXACT expression shape
	// Transformable::SetOrientation/SetPosition combine position +
	// orientation + scale with (`Translation(pos) * orientation *
	// Stretch(appliedStretch)`), so `rotate` composes Z-rotation FIRST,
	// then Y, then X (the `Matrix4Ops::XRotation(x) * YRotation(y) *
	// ZRotation(z)` product applies its RIGHTMOST factor -- Z -- first),
	// exactly like standard_object's `orientation`.
	const Scalar rxRad = rotateDeg.x * DEG_TO_RAD;
	const Scalar ryRad = rotateDeg.y * DEG_TO_RAD;
	const Scalar rzRad3 = rotateDeg.z * DEG_TO_RAD;
	xform3D =
		Matrix4Ops::Translation( translate ) *
		Matrix4Ops::XRotation( rxRad ) *
		Matrix4Ops::YRotation( ryRad ) *
		Matrix4Ops::ZRotation( rzRad3 ) *
		Matrix4Ops::Stretch( scale );
	isIdentity3D =
		std::fabs( scale.x - Scalar(1) ) < Scalar(1e-9) &&
		std::fabs( scale.y - Scalar(1) ) < Scalar(1e-9) &&
		std::fabs( scale.z - Scalar(1) ) < Scalar(1e-9) &&
		std::fabs( rxRad )  < Scalar(1e-9) &&
		std::fabs( ryRad )  < Scalar(1e-9) &&
		std::fabs( rzRad3 ) < Scalar(1e-9) &&
		std::fabs( translate.x ) < Scalar(1e-9) &&
		std::fabs( translate.y ) < Scalar(1e-9) &&
		std::fabs( translate.z ) < Scalar(1e-9);
}

RISEPel MappingPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	switch( projection ) {
		case Proj_UV: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptCoord = ApplyUV( ri.ptCoord );
			// P1-B fix (S7 review round 1): ri.txFootprint's (dudx,dudy,
			// dvdx,dvdy) were computed for the ORIGINAL ptCoord -- a
			// scale/rotate here changes the uv-to-screen density/
			// orientation by exactly that transform, so the footprint no
			// longer describes the remapped uv and would mis-select a
			// mip level (same rationale as TexCoord1Painter.h's UV1
			// swap).  Invalidate so TexturePainter falls back to its
			// bilinear-base path instead of sampling the wrong mip.
			ri2.txFootprint.valid = false;
			return source.GetColor( ri2 );
		}
		case Proj_World: {
			// world/object patch ptIntersection/ptObjIntersec, NOT
			// ptCoord -- the footprint describes a uv derivative that
			// this projection never touches, so it stays valid.
			RayIntersectionGeometric ri2 = ri;
			ri2.ptIntersection = Apply3D( ri.ptIntersection );
			return source.GetColor( ri2 );
		}
		case Proj_Object: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptObjIntersec = Apply3D( ri.ptObjIntersec );
			return source.GetColor( ri2 );
		}
		case Proj_Triplanar:
		default: {
			Scalar w[3];
			Point2 uv2[3];
			ComputeTriplanar( ri, w, uv2 );
			RayIntersectionGeometric ri2 = ri;
			// Same footprint-invalidation rationale as Proj_UV above:
			// each of the three per-axis samples patches ptCoord to a
			// world-derived (not the original UV0/UV1) coordinate, so
			// the inherited footprint no longer applies to ANY of them.
			ri2.txFootprint.valid = false;
			RISEPel sum( 0, 0, 0 );
			for( int k = 0; k < 3; ++k ) {
				if( w[k] <= Scalar(0) ) continue;
				ri2.ptCoord = uv2[k];
				sum = sum + source.GetColor( ri2 ) * w[k];
			}
			return sum;
		}
	}
}

Scalar MappingPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	switch( projection ) {
		case Proj_UV: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptCoord = ApplyUV( ri.ptCoord );
			// P1-B fix: stale-footprint rationale, see GetColor's Proj_UV
			// case above.
			ri2.txFootprint.valid = false;
			return source.GetColorNM( ri2, nm );
		}
		case Proj_World: {
			// world/object leave ptCoord untouched -- footprint stays valid.
			RayIntersectionGeometric ri2 = ri;
			ri2.ptIntersection = Apply3D( ri.ptIntersection );
			return source.GetColorNM( ri2, nm );
		}
		case Proj_Object: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptObjIntersec = Apply3D( ri.ptObjIntersec );
			return source.GetColorNM( ri2, nm );
		}
		case Proj_Triplanar:
		default: {
			Scalar w[3];
			Point2 uv2[3];
			ComputeTriplanar( ri, w, uv2 );
			RayIntersectionGeometric ri2 = ri;
			// P1-B fix: stale-footprint rationale, see GetColor's
			// Proj_Triplanar case above.
			ri2.txFootprint.valid = false;
			Scalar sum = 0;
			for( int k = 0; k < 3; ++k ) {
				if( w[k] <= Scalar(0) ) continue;
				ri2.ptCoord = uv2[k];
				sum += source.GetColorNM( ri2, nm ) * w[k];
			}
			return sum;
		}
	}
}

SpectralPacket MappingPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	switch( projection ) {
		case Proj_UV: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptCoord = ApplyUV( ri.ptCoord );
			// P1-B fix: stale-footprint rationale, see GetColor's Proj_UV
			// case above.
			ri2.txFootprint.valid = false;
			return source.GetSpectrum( ri2 );
		}
		case Proj_World: {
			// world/object leave ptCoord untouched -- footprint stays valid.
			RayIntersectionGeometric ri2 = ri;
			ri2.ptIntersection = Apply3D( ri.ptIntersection );
			return source.GetSpectrum( ri2 );
		}
		case Proj_Object: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptObjIntersec = Apply3D( ri.ptObjIntersec );
			return source.GetSpectrum( ri2 );
		}
		case Proj_Triplanar:
		default: {
			// Same SAME per-axis weight/coord helper as GetColor/
			// GetColorNM (the S2 lesson) -- synthesize the output packet
			// by sampling source.GetColorNM per bin, exactly the
			// technique RampPainter::GetSpectrum uses to stay consistent
			// with its own GetColorNM.
			Scalar w[3];
			Point2 uv2[3];
			ComputeTriplanar( ri, w, uv2 );

			const Scalar lambda_begin = Scalar( 380 );
			const Scalar lambda_end   = Scalar( 780 );
			const unsigned int nbins  = 81;
			SpectralPacket sp( lambda_begin, lambda_end, nbins );
			const Scalar delta = ( lambda_end - lambda_begin ) / Scalar( nbins );

			RayIntersectionGeometric ri2 = ri;
			// P1-B fix: stale-footprint rationale, see GetColor's
			// Proj_Triplanar case above.
			ri2.txFootprint.valid = false;
			for( unsigned int i = 0; i < nbins; ++i ) {
				const Scalar lambda = lambda_begin + Scalar( i ) * delta;
				Scalar val = 0;
				for( int k = 0; k < 3; ++k ) {
					if( w[k] <= Scalar(0) ) continue;
					ri2.ptCoord = uv2[k];
					val += source.GetColorNM( ri2, lambda ) * w[k];
				}
				sp.SetAtIndex( i, val );
			}
			return sp;
		}
	}
}

Scalar MappingPainter::GetAlpha( const RayIntersectionGeometric& ri ) const
{
	switch( projection ) {
		case Proj_UV: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptCoord = ApplyUV( ri.ptCoord );
			// P1-B fix: stale-footprint rationale, see GetColor's Proj_UV
			// case above.
			ri2.txFootprint.valid = false;
			return source.GetAlpha( ri2 );
		}
		case Proj_World: {
			// world/object leave ptCoord untouched -- footprint stays valid.
			RayIntersectionGeometric ri2 = ri;
			ri2.ptIntersection = Apply3D( ri.ptIntersection );
			return source.GetAlpha( ri2 );
		}
		case Proj_Object: {
			RayIntersectionGeometric ri2 = ri;
			ri2.ptObjIntersec = Apply3D( ri.ptObjIntersec );
			return source.GetAlpha( ri2 );
		}
		case Proj_Triplanar:
		default: {
			Scalar w[3];
			Point2 uv2[3];
			ComputeTriplanar( ri, w, uv2 );
			RayIntersectionGeometric ri2 = ri;
			// P1-B fix: stale-footprint rationale, see GetColor's
			// Proj_Triplanar case above.
			ri2.txFootprint.valid = false;
			Scalar sum = 0;
			for( int k = 0; k < 3; ++k ) {
				if( w[k] <= Scalar(0) ) continue;
				ri2.ptCoord = uv2[k];
				sum += source.GetAlpha( ri2 ) * w[k];
			}
			return sum;
		}
	}
}
