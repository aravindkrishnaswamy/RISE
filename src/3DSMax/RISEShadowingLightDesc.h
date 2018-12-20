
#include "maxincl.h"
#include "RISEIncludes.h"
#include "MAX2RISE_Helpers.h"
#include "scontext.h"

class RISEShadowingLightDesc : public LightDesc
{
public:
	const RISE::IRayCaster*		pCaster;
	ObjLightDesc*				pLightDesc;
	Matrix3				affineTM;
	Point3				worldPtIntersection;

	BOOL Illuminate(ShadeContext& sc, Point3& normal, Color& color, Point3 &dir, float &dot_nl, float &diffuseCoef)
	{
		if( pLightDesc ) {
			sc.shadow = FALSE;
			if( pLightDesc->Illuminate( sc, normal, color, dir, dot_nl, diffuseCoef ) ) {
				// Get the underlying light desc to do the illumination, if this point is illuminated, AND
				// we are supposed to do shadowing for this light, then do the shadow ray
				if( color.r > 0 || color.g > 0 || color.b > 0 ) {
					// Do the shadow check
					RISE::Ray ray;
					Matrix3 invTM = Inverse(affineTM);
					Point3 ptLightPos = invTM*pLightDesc->LightPosition();
					ray.origin = MAX2RISEPoint(ptLightPos);
					ray.dir = RISE::Vector3Ops::Normalize( MAX2RISEVector(VectorTransform(invTM,-dir)) );

					RISE::Scalar dist = RISE::Vector3Ops::Magnitude( MAX2RISEVector(worldPtIntersection - ptLightPos) );

					if( pCaster->CastShadowRay( ray, dist-0.001 ) ) {
						color.r = color.g = color.b = 0;
					}
				}

				return TRUE;
			}
		}

		return FALSE;
	}

	Point3 LightPosition()
	{
		if( pLightDesc ) {
			return pLightDesc->LightPosition();
		}

		return Point3(0,0,0);
	}
};
