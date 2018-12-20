
#pragma warning( disable : 4512 )		// disables warning about not being able to generate an assignment operator (.NET 2003)
#pragma warning( disable : 4250 )		// disables silly virtual inheritance warning
#pragma warning( disable : 4344 )		// disables warning about explicit template argument passed to template function

// Bloody 3DSMAX... not only is it bad enough they pollute the hell out of the
// global namespace, they then use a #DEFINE for freaking PI and DEG_TO_RAD... grrrr...

#undef PI
#undef DEG_TO_RAD
#undef RAD_TO_DEG

#include <RISE_API.h>
#include <Interfaces\IGeometry.h>
#include <Interfaces\IBSDF.h>
#include <Utilities\Reference.h>
#include <Polygon.h>
#include <Utilities\GeometricUtilities.h>
#include <Interfaces\IJobPriv.h>


// Repollute the global namespace with the 3DSMAX crap....
#define PI  ((float)3.1415926535)
#define DEG_TO_RAD (PI/(float)180.0)
#define RAD_TO_DEG ((float)180.0/PI)

