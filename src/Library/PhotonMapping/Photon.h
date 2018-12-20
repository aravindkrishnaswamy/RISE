//////////////////////////////////////////////////////////////////////
//
//  Photon.h - Definition of a photon used in photon mapping
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 23, 2002
//  Tabs: 4
//  Comments:  The code here is an implementation from Henrik Wann
//             Jensen's book Realistic Image Synthesis Using 
//             Photon Mapping.  Much of the code is influeced or
//             taken from the sample code in the back of his book.
//			   I have however used STD data structures rather than
//			   reinventing the wheel as he does.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PHOTON_
#define PHOTON_

#include "../Utilities/Color/Color.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	class Photon
	{
	public:
		Point3			ptPosition;			// Location of the photon in three space
		unsigned char	plane;				// splitting plane used in the kd-tree
		RISEPel			power;				// photon power
		unsigned char	theta, phi;			// incoming direction of the photon

		Photon() : 
		plane( 0 ),
		theta( 0 ),
		phi( 0 )
		{};
	};

	class IrradPhoton : public Photon
	{
	public:
		RISEPel			irrad;				// precomputed irradiance
		unsigned char	Ntheta, Nphi;		// direction of the normal 

		IrradPhoton() : 
		Ntheta( 0 ),
		Nphi( 0 )
		{};
	};

	class SpectralPhoton
	{
	public:
		Point3			ptPosition;			// Location of the photon in three space
		unsigned char	plane;				// splitting plane used in the kd-tree
		Scalar			power;				// photon power
		unsigned char	theta, phi;			// incoming direction of the photon
		Scalar			nm;					// wavelength of the photon

		SpectralPhoton() : 
		plane( 0 ),
		theta( 0 ),
		phi( 0 ),
		nm( 400 )
		{};
	};

	class TranslucentPhoton
	{
	public:
		Point3			ptPosition;			// Location of the photon in three space
		unsigned char	plane;				// splitting plane used in the kd-tree
		RISEPel			power;				// photon power

		TranslucentPhoton() : 
		plane( 0 )
		{};
	};

	class ShadowPhoton
	{
	public:
		Point3			ptPosition;			// Location of the photon in three space
		unsigned char	plane;				// splitting plane used in the kd-tree
		RISEPel			power;				// photon power
		bool			shadow;				// is this a shadow photon ?

		ShadowPhoton() : 
		shadow( false )
		{};
	};

	//
	// This little helper class allows arbritary data types to be sorted by their 
	// data.  This is particularily useful for the photons to be sorted by the distance
	// from the hit point
	//
	template<class T>
	class distance_container
	{
		public:
			T element;
			Scalar distance;

			distance_container( const T& elem, const Scalar& d ) : element( elem ), distance( d ) {}
	};

	//
	// The operator is what allows the STL to actually sort with this data type
	//
	template<class T>
	inline bool operator<(const distance_container<T>& a, const distance_container<T>& b)
	{
		// You might be wondering why the hell I do this, well its because
		// return a.distance < b.distance doesn't work on some compilers
		// the sgi compiler, and gcc to name a couple, it is fine under
		// MSVC though, but its left this way to make the code the most compatible
		const Scalar& dista = a.distance;
		const Scalar& distb = b.distance;
		return dista < distb;
	}
}

#endif
