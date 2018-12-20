#ifndef HIT_INFO_H
#define HIT_INFO_H

// Information about closest hit
class HitInfo : public virtual RISE::IReference, RISE::Implementation::Reference
{
public:
	Instance*	instance;
	int			faceNum;
	Point3		baryCoord;
//	Point3		normalAtHitPoint;
//	Point3		hitPos;
//	Point3		normal;
};


#endif

