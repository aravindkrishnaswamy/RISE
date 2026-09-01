//////////////////////////////////////////////////////////////////////
//
//  CompositeSPF.h - Defines a SPF that that composes two SPFs
//    together
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPOSITE_SPF_
#define COMPOSITE_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class CompositeSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			virtual ~CompositeSPF( );

			const ISPF&	top;				// top
			const ISPF& bottom;				// bottom
			const unsigned int max_recur;	// maximum level of absolute recusion before terminating

			const unsigned int max_reflection_recursion;		// maximum level of reflection recursion
			const unsigned int max_refraction_recursion;		// maximum level of refraction recursion
			const unsigned int max_diffuse_recursion;			// maximum level of diffuse recursion
			const unsigned int max_translucent_recursion;		// maximum level of translucent recursion

			const Scalar thickness;			// thickness of each of the layers

			//! Beer-Lambert extinction coefficient for the inter-layer gap.
			//! A PHYSICAL SCALAR, not a colour: it is an inverse length that
			//! is routinely authored well above 1 (the shipped scene uses
			//! 8.0 on blue).  Typed `IScalarPainter` for exactly the reason
			//! docs/ISCALARPAINTER_REFACTOR.md gives -- `IPainter::GetColorNM`
			//! routes through the Jakob-Hanika ALBEDO uplift, which is
			//! bounded to [0,1], so every extinction above ~1 used to
			//! saturate to ~1.0 in every SPECTRAL rasterizer while the RGB
			//! walk used the authored value.  `IScalarPainter` never touches
			//! colourspace.  Mirrors `TranslucentSPF::pExtinction`.
			const IScalarPainter& extinction;

			//! The walk carries TWO stacks -- `outside` (without this object's
			//! IOR-stack entry, i.e. the medium above the top interface) and
			//! `gap` (with the entry the top interface pushed).  A single
			//! stack cannot work: IORStack keys its entries on the IObject*,
			//! which is shared by both layers, so the top's push is
			//! indistinguishable from an entry of the bottom's.
			//!
			//! EvalStack picks which one a layer's Scatter() sees: a
			//! DOWN-going ray is arriving from the medium above that layer and
			//! must see `outside` (so a stack-sensitive bottom layer reads
			//! "entering from outside"); an UP-going ray is arriving from
			//! inside and must see `gap` (so a dielectric top layer takes its
			//! from-inside branch and refracts OUT).  Full failure-mode
			//! history in the block comment in CompositeSPF.cpp.
			static const IORStack& EvalStack(
					const RayIntersectionGeometric& ri,							///< [in] The intersection whose ray direction selects the stack
					const IORStack& outside_stack,								///< [in] Stack without this object's entry
					const IORStack& gap_stack									///< [in] Stack of the inter-layer gap
					);

			//! Returns the gap stack for the leg BELOW the top interface: a
			//! ray the top layer refracted downward carries its own pushed
			//! stack, and that stack IS the gap medium.  Rays with no stack of
			//! their own did not change medium.  Deliberately not applied in
			//! the bottom->top direction -- see CompositeSPF.cpp.
			static const IORStack& GapStackBelowTop(
					const ScatteredRay& scat,									///< [in] The scattered ray about to cross the gap
					const IORStack& gap_stack									///< [in] The gap stack so far
					);

			bool	ShouldScatteredRayBePropagated(
					const ScatteredRay::ScatRayType type,
					const unsigned int steps
					) const;

			void	ProcessTopLayer(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const RISEPel& importance,									///< [in] Importance from prevous pass
					ISampler& sampler,									///< Sampler for the MC process
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
					const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
					) const;

			void	ProcessBottomLayer(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const RISEPel& importance,									///< [in] Importance from prevous pass
					ISampler& sampler,									///< Sampler for the MC process
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
					const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
					) const;

			void	ProcessTopLayerNM(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const Scalar importance,									///< [in] Importance from prevous pass
					ISampler& sampler,									///< Sampler for the MC process
					const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
					const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
					) const;

			void	ProcessBottomLayerNM(
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					const Scalar importance,									///< [in] Importance from prevous pass
					ISampler& sampler,									///< Sampler for the MC process
					const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const unsigned int steps,									///< [in] Number of steps taken in the random walk process
					const IORStack& outside_stack,							///< [in] Stack of the medium above the top interface (no entry for this object)
					const IORStack& gap_stack								///< [in] Stack of the inter-layer gap (with the top's pushed entry)
					) const;

		public:
			CompositeSPF(
				const ISPF& top_,
				const ISPF& bottom_,
				const unsigned int max_recur_,
				const unsigned int max_reflection_recursion_,		// maximum level of reflection recursion
				const unsigned int max_refraction_recursion_,		// maximum level of refraction recursion
				const unsigned int max_diffuse_recursion_,			// maximum level of diffuse recursion
				const unsigned int max_translucent_recursion_,		// maximum level of translucent recursion
				const Scalar thickness_,							// thickness between the materials
				const IScalarPainter& extinction_					// extinction coefficient for absorption between layers (physical scalar)
				);

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.  
			void	Scatter( 
					const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
					ISampler& sampler,									///< [in] Sampler
					ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
					const IORStack& ior_stack								///< [in/out] Index of refraction stack
					) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into 
			//! account spectral affects.  
			void	ScatterNM(
				const RayIntersectionGeometric& ri,								///< [in] Geometric intersection details for point of intersection
				ISampler& sampler,										///< [in] Sampler
				const Scalar nm,												///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,								///< [out] The list of scattered rays from the surface
				const IORStack& ior_stack									///< [in/out] Index of refraction stack
				) const;

			//! Returns the PDF for the composite SPF as the sum of weighted child PDFs
			Scalar Pdf(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details
				const Vector3& wo,											///< [in] Outgoing scattered direction
				const IORStack& ior_stack								///< [in] Index of refraction stack
				) const;

			//! Spectral version of Pdf
			Scalar PdfNM(
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details
				const Vector3& wo,											///< [in] Outgoing scattered direction
				const Scalar nm,											///< [in] Wavelength
				const IORStack& ior_stack								///< [in] Index of refraction stack
				) const;
		};
	}
}

#endif
