//////////////////////////////////////////////////////////////////////
//
//  IMaterial.cpp - Implementation of IMaterial default virtual methods
//  that depend on ISPF (avoiding circular include issues).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ISPF.h"

using namespace RISE;

Scalar IMaterial::Pdf(
	const Vector3& wo,
	const RayIntersectionGeometric& ri,
	const IORStack& ior_stack
	) const
{
	const ISPF* pSPF = GetSPF();
	return pSPF ? pSPF->Pdf( ri, wo, ior_stack ) : 0;
}

Scalar IMaterial::PdfNM(
	const Vector3& wo,
	const RayIntersectionGeometric& ri,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	const ISPF* pSPF = GetSPF();
	return pSPF ? pSPF->PdfNM( ri, wo, nm, ior_stack ) : 0;
}
