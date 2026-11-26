//////////////////////////////////////////////////////////////////////
//
//  PendingPhotonShoots.h - POD descriptors for deferred photon-map
//    shoots.  Populated by the scene parser / Job API, consumed by
//    Scene::BuildPendingPhotonMaps at the start of rendering.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PENDING_PHOTON_SHOOTS_
#define PENDING_PHOTON_SHOOTS_

#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	// Gather parameters shared by the pel maps (caustic / global / translucent / shadow).
	struct PendingPelGatherParams
	{
		bool			set;
		Scalar			radius;
		Scalar			ellipseRatio;
		unsigned int	minPhotons;
		unsigned int	maxPhotons;
	};

	// Gather parameters for spectral maps (extra nm_range).
	struct PendingSpectralGatherParams
	{
		bool			set;
		Scalar			radius;
		Scalar			ellipseRatio;
		unsigned int	minPhotons;
		unsigned int	maxPhotons;
		Scalar			nmRange;
	};

	struct PendingCausticPelShoot
	{
		bool			pending;
		unsigned int	num;
		Scalar			powerScale;
		unsigned int	maxRecur;
		Scalar			minImportance;
		bool			branch;
		bool			reflect;
		bool			refract;
		bool			shootFromNonMeshLights;
		unsigned int	temporalSamples;
		bool			regenerate;
		bool			shootFromMeshLights;
	};

	struct PendingGlobalPelShoot
	{
		bool			pending;
		unsigned int	num;
		Scalar			powerScale;
		unsigned int	maxRecur;
		Scalar			minImportance;
		bool			branch;
		bool			shootFromNonMeshLights;
		unsigned int	temporalSamples;
		bool			regenerate;
		bool			shootFromMeshLights;
	};

	struct PendingTranslucentPelShoot
	{
		bool			pending;
		unsigned int	num;
		Scalar			powerScale;
		unsigned int	maxRecur;
		Scalar			minImportance;
		bool			reflect;
		bool			refract;
		bool			directTranslucent;
		bool			shootFromNonMeshLights;
		unsigned int	temporalSamples;
		bool			regenerate;
		bool			shootFromMeshLights;
	};

	struct PendingCausticSpectralShoot
	{
		bool			pending;
		unsigned int	num;
		Scalar			powerScale;
		unsigned int	maxRecur;
		Scalar			minImportance;
		Scalar			nmBegin;
		Scalar			nmEnd;
		unsigned int	numWavelengths;
		bool			branch;
		bool			reflect;
		bool			refract;
		unsigned int	temporalSamples;
		bool			regenerate;
	};

	struct PendingGlobalSpectralShoot
	{
		bool			pending;
		unsigned int	num;
		Scalar			powerScale;
		unsigned int	maxRecur;
		Scalar			minImportance;
		Scalar			nmBegin;
		Scalar			nmEnd;
		unsigned int	numWavelengths;
		bool			branch;
		unsigned int	temporalSamples;
		bool			regenerate;
	};

	struct PendingShadowShoot
	{
		bool			pending;
		unsigned int	num;
		unsigned int	temporalSamples;
		bool			regenerate;
	};
}

#endif
