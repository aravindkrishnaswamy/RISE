//////////////////////////////////////////////////////////////////////
//
//  TestableManifoldSolver.h - Test-access subclass for ManifoldSolver.
//
//    Re-exports protected methods as public via C++ using-declarations
//    so unit tests can call them directly.  No forwarding functions or
//    friend declarations needed.
//
//////////////////////////////////////////////////////////////////////

#ifndef TESTABLE_MANIFOLD_SOLVER_H_
#define TESTABLE_MANIFOLD_SOLVER_H_

#include "../src/Library/Utilities/ManifoldSolver.h"

namespace RISE
{
	namespace Implementation
	{
		class TestableManifoldSolver : public ManifoldSolver
		{
		public:
			TestableManifoldSolver()
				: ManifoldSolver( ManifoldSolverConfig() )
			{
			}

			// Re-export protected methods as public for testing
			using ManifoldSolver::ComputeSpecularDirection;
			using ManifoldSolver::ComputeSpecularDirectionDerivativeWrtNormal;
			using ManifoldSolver::DirectionToSpherical;
			using ManifoldSolver::EvaluateConstraint;
			using ManifoldSolver::EvaluateConstraintAtVertex;
			using ManifoldSolver::BuildJacobian;
			using ManifoldSolver::BuildJacobianNumerical;
			using ManifoldSolver::SolveBlockTridiagonal;
			using ManifoldSolver::ValidateChainPhysics;
			using ManifoldSolver::ComputeBlockTridiagonalDeterminant;
			using ManifoldSolver::BuildJacobianAngleDiff;
			using ManifoldSolver::BuildJacobianAngleDiffNumerical;
			using ManifoldSolver::ComputeLastBlockLightJacobian;
			using ManifoldSolver::ComputeLightToFirstVertexJacobianDet;
		};
	}
}

#endif
