//////////////////////////////////////////////////////////////////////
//
//  BlendedModifier.cpp - Weighted blend of two ray-intersection
//    modifiers.  See BlendedModifier.h for the contract.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BlendedModifier.h"
#include "ModifierFrame.h"
#include "../Utilities/FiniteMath.h"

using namespace RISE;
using namespace RISE::Implementation;

BlendedModifier::BlendedModifier(
	const IRayIntersectionModifier* pModA_,
	const IRayIntersectionModifier* pModB_,
	const IScalarPainter* pWeight_
	) :
	pModA( pModA_ ),
	pModB( pModB_ ),
	pWeight( pWeight_ )
{
	if( pModA ) {
		pModA->addref();
	}
	if( pModB ) {
		pModB->addref();
	}
	if( pWeight ) {
		pWeight->addref();
	}
}

BlendedModifier::~BlendedModifier()
{
	safe_release( pModA );
	safe_release( pModB );
	safe_release( pWeight );
}

void BlendedModifier::Modify( RayIntersectionGeometric& ri ) const
{
	// DL-302: capture pre-modifier frame so Coat Normal decodes against the
	// unperturbed surface frame even when a BlendedModifier runs.
	ModifierFrame::RecordPreModifierFrame( ri );

	// Evaluate blend weight on the PRE-modifier hit record.
	Scalar t = pWeight
		? pWeight->GetValuesAt( ri ).v[ScalarPainterRGB::kSingleSampleChannel]
		: Scalar( 0 );
	if( !IsFiniteDouble( static_cast<double>( t ) ) || t <= Scalar( 0 ) ) {
		if( pModA ) {
			pModA->Modify( ri );
		}
		return;
	}
	if( t >= Scalar( 1 ) ) {
		if( pModB ) {
			pModB->Modify( ri );
		}
		return;
	}

	RayIntersectionGeometric riA( ri );
	if( pModA ) {
		pModA->Modify( riA );
	}

	RayIntersectionGeometric riB( ri );
	if( pModB ) {
		pModB->Modify( riB );
	}

	const Vector3 blended = riA.vNormal * ( Scalar( 1 ) - t ) + riB.vNormal * t;
	const Scalar len = Vector3Ops::Magnitude( blended );
	if( !IsFiniteDouble( static_cast<double>( len ) ) || len <= NEARZERO ) {
		return;
	}

	const Vector3 newN = blended * ( Scalar( 1 ) / len );
	if( ModifierFrame::HasCoherentTangent( ri ) ) {
		ModifierFrame::RebuildPreservingTangent( ri, newN );
	} else {
		ri.vNormal = newN;
		ri.onb.CreateFromW( newN );
	}
}
